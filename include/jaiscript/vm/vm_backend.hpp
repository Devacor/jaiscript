#pragma once

#ifndef __JAISCRIPT_VM_VM_BACKEND_HPP__
#define __JAISCRIPT_VM_VM_BACKEND_HPP__

#include "chunk.hpp"
#include "coverage.hpp"
#include "vm_compiler.hpp"
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/detail/value_text.hpp>
#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/execution_limits.hpp>
#include <jaiscript/detail/member_scope.hpp>
#include <jaiscript/detail/return_conv.hpp>
#include <jaiscript/detail/parallel_schedule.hpp>
#include <jaiscript/detail/null_member_diagnostic.hpp>
#include <jaiscript/detail/arity_diagnostic.hpp>
#include <jaiscript/detail/slot_site.hpp>
#include <jaiscript/detail/operator_ids.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <deque>
#include <new>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jai {
    class script_class_definition;
    namespace detail { struct caller_frame_view; struct namespace_store_target; struct aggregate_plan; struct construction_scope; }
    struct script_namespace_data;
}

namespace jai::vm {

    // The numeric-fast-path op set shared by the fused binary/compare/index kernels
    // (WPR at 99deff47: 40k µs of out-of-line calls when this lived in the vm TU)
    JAI_FORCEINLINE bool is_numeric_binary_op(token_type op) {
        switch (op) {
            case token_type::plus:
            case token_type::minus:
            case token_type::star:
            case token_type::slash:
            case token_type::percent:
            case token_type::less:
            case token_type::less_equal:
            case token_type::greater:
            case token_type::greater_equal:
            case token_type::equal_equal:
            case token_type::bang_equal:
            case token_type::spaceship:
                return true;
            default:
                return false;
        }
    }

    class vm_backend final : public execution_backend {
    public:
		checked_result<bool> language_less(const script_value& a_left, const script_value& a_right) override;
		bool native_less_available() const override;
		checked_result<script_value> array_transform(uint64_t a_member, script_value& a_self, const std::vector<script_value>& a_args) override;
		checked_result<script_value> map_transform(uint64_t a_member, script_value& a_self, const std::vector<script_value>& a_args) override;

        vm_backend(string_symbolizer* symbolizer, std::shared_ptr<environment> global_env);
        cell_counters cells_counters() const override { return cells_counters_; }
        ~vm_backend() override { release_site_pins(); release_retained_decls(); }
        // A call site pins the callee it cached (ABA): a recursive function's own body pins the function, function -> body
        // chunk -> site -> function; the backend drops every pin as it dies so the engine's functions die with the engine
        void release_site_pins() noexcept;
        // Hot reload's reclamation, at the end of an outermost execute: the cached chunks nothing outside the backend's own caches
        // holds any more (their bodies' declarations gone, no frame, fiber or run on them) are dropped. The site pins go first when no
        // suspended fiber parks a callee, so code that only its own call sites kept alive (recursion, mutual calls) goes too.
        void retire_unreachable_code();
        void note_cache_insert() noexcept { ++cache_inserts_total_; ++cache_inserts_since_retire_; }
        // A method frame names its overload raw: the main backend keeps every declaration it entered alive until a retire point finds
        // only itself holding it (no frame can name it then); a worker, or a declaration another backend retains, pins it on the record
        std::vector<std::shared_ptr<function_decl>> retained_decls_;
        void release_retained_decls() noexcept {
            for (auto& decl : retained_decls_) { if (decl->vm_retainer == this) { decl->vm_retainer = nullptr; } }
            retained_decls_.clear();
        }
        void retire_site_callee(const call_site& site);

        script_value execute(const std::vector<declaration_ptr>& declarations) override;
        script_value execute(const std::vector<declaration_ptr>& declarations, std::shared_ptr<void>& compiled_slot) override;
        script_value execute_textual(const std::vector<declaration_ptr>& declarations, std::shared_ptr<void>& compiled_slot) override;
        void prepare_for_execution() override;
        checked_result<script_value> execute_callable(const script_callable& payload, const std::vector<script_value>& args) override {
            return execute_callable_from(payload, args, 0);
        }
        checked_result<script_value> execute_callable_from(const script_callable& payload, const std::vector<script_value>& args, size_t first) override;
        checked_result<script_value> execute_method_from(const script_method_dispatch& dispatch, const std::shared_ptr<function_decl>& ast,
                                                         const script_value& this_obj, const std::vector<script_value>& args, size_t first) override;
        struct unwind_park;
        struct switch_park;
        prepared_callback* prepare_callback(const script_value& callable) override;
        void release_callback(prepared_callback* prepared) override;

        script_value get_variable(const std::string& name) const override;
        void push_external_call_scope() override;
        void begin_host_call() override;
        std::vector<script_value>* exchange_owned_arguments(std::vector<script_value>* args) override {
            auto* previous = owned_args_;
            owned_args_ = args;
            return previous;
        }
        void pop_external_call_scope() override;
        checked_result<script_value> host_reference_argument(const std::vector<script_value>& args, size_t index, size_t offset,
                                                             host_slot& slot) override;
        checked_result<void> host_slot_store(const host_slot& slot, script_value& value) override;
        bool has_variable(const std::string& name) const override;

        void pop_scope();
        void define_variable(const std::string& name, const script_value& value) override;
        void push_instance_scope() override;
        void pop_instance_scope() override;

        void set_execution_budget(std::chrono::nanoseconds budget) override;
        void extend_execution_deadline(std::chrono::nanoseconds by) override { if (budget_active_) { execution_deadline_ += by; } }
        std::chrono::steady_clock::time_point execution_deadline() const override { return budget_active_ ? execution_deadline_ : std::chrono::steady_clock::time_point{}; }
        void set_has_custom_numeric_ops(bool value) override { has_custom_numeric_ops_ = value; }
        void set_has_custom_binary_ops(bool value) override { has_custom_binary_ops_ = value; }
        void set_operator_table(const detail::engine_operator_table* table) override { operator_table_ = table; }
        void set_class_lookup_callback(std::function<std::shared_ptr<class_definition>(const std::string&)> callback) override {
            class_lookup_ = std::move(callback);
        }
        void set_engine_reference(engine* engine_ref) override;

        // run_dispatch detects statement boundaries via chunk::stmt_nodes
        // and drives the SAME controller as the interpreter. The per-op path tests one
        // plain cached pointer (debug_hook_); transport state is pulled in only at the
        // debug sync points (sync_debug_hook).
        void set_debug_controller(debug::controller* controller) override {
            debug_controller_.store(controller, std::memory_order_release);
        }

        // Innermost live environment (debugger locals fallback / globals scope).
        std::shared_ptr<environment> get_current_environment() const override { return environment_; }

        std::vector<stack_frame> debug_frames() const override;
        // Named slot locals of a parked frame, reconstructed lazily from the chunk's
        // decl/load/store operands (no name table baked into release chunks).
        std::vector<std::pair<std::string, script_value>> get_frame_locals(size_t frame) const override;
        script_value* debug_local_storage(const std::string& name) override;
        int debug_depth() const override { return static_cast<int>(call_records_top_ + static_cast<size_t>(current_call_depth_)); }
        // Debug sync point (prepare_for_execution, every 1024 budget ticks, a sleep, an armed
        // session's pending change, park exit via the controller): mirrors
        // debug_controller_ into the plain per-op gate and hands a session-end a fresh budget
        // deadline. Twin of interpreter::sync_debug_hook.
        void sync_debug_hook() override;

        bool is_unwinding() const override { return is_unwinding_; }
        const script_exception& get_current_exception() const override { return current_exception_.value(); }

        // Host-boundary discriminator (same rule as resume_coroutine's host-level
        // detection): false only when no frames are live and depth is zero
        bool is_executing() const override { return current_call_depth_ != 0 || !frames_.empty(); }

        void park_nested_failure() override;
        std::vector<stack_frame> last_stack_trace() const override { return captured_trace_; }
        std::string format_stack_trace() const override;
        std::string current_source_file() const override;


        checked_result<script_value> resume_coroutine(coroutine_handle& handle) override;

        std::unique_ptr<backend_run_state> make_run_state() override;
        void swap_run_state(backend_run_state& state) noexcept override;
        void reset_run_state(backend_run_state& state) override;
        void adopt_run_trace(backend_run_state& state) override;

        std::string get_backend_name() const override { return "vm"; }
        checked_result<std::string> text_of(const script_value& v) override { return value_to_string_with_method(v.deref()); }

        // === Parallel worker setup (parallel_transform v0) ===
        // Pins this instance to a worker context built at the region barrier: root
        // environment (parent = nullptr, the hard partition), a private env-epoch sink
        // (worker env churn never bumps the shared engine epoch), the worker's own
        // limits, and an armed budget clock. cached_global_env_ goes null so the SHARED
        // per-chunk env-lookup caches never engage (no epoch reads, no cache writes into
        // chunks); lookups memoize instead in this instance's worker-private tables
        // (worker_env_caches_) against worker_root_env_ - the slot root every capture is
        // defined into, whose identity is slot-lifetime and whose epoch survives calls
        // (per-region capture redefines update in place, no bump). Workers never call
        // prepare_for_execution, so nothing here is ever repointed at shared state.
        void configure_parallel_worker(std::shared_ptr<environment> root_env,
                                       string_symbolizer* env_epoch_sink,
                                       detail::execution_limits* worker_limits,
                                       std::chrono::nanoseconds budget) override {
            environment_ = std::move(root_env);
            cached_global_env_ = nullptr;
            worker_root_env_ = environment_.get();
            if (env_symbolizer_ != env_epoch_sink) { scope_env_pool_.clear(); }   // pooled envs minted under another sink carry it
            env_symbolizer_ = env_epoch_sink;
            limits_ = worker_limits;
            // Slot REUSE reset (worker contexts persist across regions): clear every
            // piece of residual execution state from a prior region's chunk.
            stack_.clear();
            frames_.clear();
            top_frame_mark_ = SIZE_MAX;
            call_records_top_ = 0;
            for (auto& r : call_records_) { r->raw_prepared = nullptr; }
            entry_areas_top_ = 0;
            try_records_.clear();
            iter_states_.clear();
            cfor_states_.clear();
            pending_callees_.clear();
            current_call_depth_ = 0;
            is_unwinding_ = false;
            current_exception_.reset();
            active_exception_value_.reset();
            trace_captured_ = false;
            captured_trace_.clear();
            return_value_.reset();
            has_return_value_ = false;
            implicit_result_.reset();
            yielding_ = false;
            switch_to_ = nullptr;
            raw_switch_ = nullptr;
            cells_frame_ = nullptr;
            raw_epoch_ = 0;   // even: no plan the main backend wrote ever matches
            pending_site_ctx_ = {};
            external_site_stack_.clear();
            execution_budget_ = budget;
            if (worker_math_alias_rows_) { worker_math_alias_rows_->fill(engine::math_alias_row{}); }
            for (auto& rec : worker_env_caches_) { std::fill(rec->inline_holds.begin(), rec->inline_holds.end(), nullptr); std::fill(rec->call_ics.begin(), rec->call_ics.end(), worker_chunk_env_cache::call_ic{}); std::fill(rec->method_holds.begin(), rec->method_holds.end(), std::pair<const class_definition*, uint64_t>{nullptr, 0}); }
            parallel_worker_ = true;
            arm_execution_deadline();
        }

        // True on worker-configured instances only: the script-class method dispatch
        // wall (methods aren't provisioned per worker - their shared body caches and
        // arbitrary env reach are unsafe off-thread) checks this. Never set on the engine's own backend.
        bool parallel_worker_ = false;
        size_t structural_equality_depth_ = 0;   // memberwise ==/<=> levels nested natively (detail/structural_equality.hpp)
        // parallel_for chunk loop: the worker's element cursor (a reference value owned by the
        // worker slot) that op_parallel_advance re-points and op_parallel_element pushes
        std::vector<script_value>* parallel_cursors_ = nullptr;   // [0] = the range, then the zip's arrays
        void set_parallel_cursors(std::vector<script_value>* cursors, size_t block = 0, size_t stride_blocks = 1, detail::parallel_schedule_cursor* schedule = nullptr,
                                  const detail::parallel_counted_sequence* counted = nullptr) noexcept {
            parallel_cursors_ = cursors;
            parallel_block_ = block;
            parallel_stride_blocks_ = stride_blocks;
            parallel_schedule_ = schedule;
            parallel_counted_ = counted;
        }
        const detail::parallel_counted_sequence* parallel_counted_ = nullptr;   // the counted form's range (op_parallel_element's b)
        detail::parallel_schedule_cursor* parallel_schedule_ = nullptr;   // the region's shared blocks (null: the static partition)
        size_t parallel_block_ = 0;           // the chunk's block-cyclic step (0 = contiguous)
        size_t parallel_stride_blocks_ = 1;
        // Barrier-provisioned method pins for the active region (worker instances only)
        const detail::parallel_method_pin_table* parallel_method_pins_ = nullptr;
        // Worker-private env-lookup cache (workers may not touch the SHARED
        // chunk::env_lookup_cache): same entries and validation as the main path but
        // stored per chunk in THIS instance, resolve target = worker_root_env_. The pin
        // keeps the chunk alive for the backend's life, so the raw key can never be a
        // recycled address (a false identity hit would alias another chunk's ip space).
        // Created only at push_script_frame_pinned (where the owning shared_ptr is in
        // hand) - method/coroutine frames keep today's full walk.
        struct worker_chunk_env_cache {
            std::shared_ptr<void> pin;
            const chunk* code = nullptr;
            std::vector<env_lookup_cache_entry> entries;
            // The worker's own copy of each string constant, minted on first use (indexed
            // like chunk::constants; the pin keeps that table alive): a literal evaluated
            // in a region shares this node instead of allocating per evaluation, and the
            // main engine's node is never touched from the worker thread
            std::vector<script_value> string_constants;
            std::vector<uint64_t> site_epochs;   // per inline site: the definition epoch this worker last validated its globals and callees at
            std::vector<chunk::entry_memo_row> entry_rows;   // the worker's block-entry rows for this chunk (chunk::entry_memos' rule)
            std::vector<chunk::member_ic_entry> member_ic;   // the worker's member IC rows for this chunk (chunk::member_ic's rule)
            // per inline site: the callee function object this worker last validated the graft against (chunk::inline_site::holds_fn's
            // rule); worker copies live for a region, so the rows clear when the worker is configured for the next one
            std::vector<const script_function*> inline_holds;
            // per call site: the callee the worker's probe last parked (call_site::ic_*'s rule), held by the row; a hit parks it as the
            // general probe would, pinned
            struct call_ic { const script_function* identity = nullptr; const script_defined_function* fn = nullptr; strong_ptr<script_function_node> pin; uint32_t argc = 0; bool native = false; };
            std::vector<call_ic> call_ics;
            // per inline site of a method graft: the class (at its method epoch) the worker validated the graft for against the region's pin
            std::vector<std::pair<const class_definition*, uint64_t>> method_holds;
            // per method call site: the bound method's direct lane for the receiver class (call_site::nat_*'s rule)
            struct native_row { const class_definition* cd = nullptr; uint64_t stamp = 0; uint64_t shape = 0; bool field_free = false; detail::host_method_hit hit; };
            std::vector<native_row> native_rows;
            bool rows_sized = false;   // the per-site rows above hold a row for each of the chunk's sites
        };
        void size_worker_rows(worker_chunk_env_cache& rec);
        // the element's index in the vector, SIZE_MAX when it lies in another (a caller chunk's site)
        template <class T> JAI_FORCEINLINE static size_t index_within(const T& item, const std::vector<T>& items) noexcept {
            const uintptr_t at = reinterpret_cast<uintptr_t>(&item), base = reinterpret_cast<uintptr_t>(items.data());
            return at >= base && at < base + items.size() * sizeof(T) ? (at - base) / sizeof(T) : SIZE_MAX;
        }
        std::vector<std::unique_ptr<worker_chunk_env_cache>> worker_env_caches_;   // records never move: callers keep pointers
        std::unordered_map<const chunk*, worker_chunk_env_cache*> worker_env_index_;
        worker_chunk_env_cache* worker_env_mru_ = nullptr;
        JAI_FORCEINLINE worker_chunk_env_cache* worker_chunk_record(const chunk* code) {
            if (worker_env_mru_ && worker_env_mru_->code == code) { return worker_env_mru_; }
            return worker_chunk_record_slow(code);
        }
        worker_chunk_env_cache* worker_chunk_record_slow(const chunk* code);
        // the member IC rows a site reads and fills: the chunk's on the main engine, a worker's own in its record of the chunk (the chunk is
        // shared); null: no rows, the probe answers each read
        // the checker's proofs serve reads: off on the vm-reference backend (its null cached_global_env_), on in a worker (whose
        // cached_global_env_ is null too)
        bool proofs_on() const { return cached_global_env_ || parallel_worker_; }
        std::vector<chunk::member_ic_entry>* member_ic_rows(const chunk& code) {
            if (!parallel_worker_) { return cached_global_env_ ? &code.member_ic : nullptr; }
            worker_chunk_env_cache* rec = worker_chunk_record(&code);
            return rec ? &rec->member_ic : nullptr;
        }
        chunk::member_ic_entry* member_ic_row(const chunk& code, size_t ip) {
            std::vector<chunk::member_ic_entry>* rows = member_ic_rows(code);
            if (!rows) { return nullptr; }
            if (rows->size() < code.code.size()) { rows->resize(code.code.size()); }
            return &(*rows)[ip];
        }
        // A worker's own copies of the borrowed table cells it reads, keyed by (array node,
        // index) and validated against the live bytes on every hit (a recycled node address
        // or a rebuilt table can only refresh the copy, never return stale text): the row
        // builders read the same palette cells thousands of times per region, and each read
        // allocated a string. Bounded; cleared when it grows past the cap.
        std::unordered_map<uint64_t, script_value> worker_borrowed_strings_;
        struct ctor_dispatch_entry {
            std::optional<script_value> method;
            const class_definition* cls = nullptr;
            const environment* env = nullptr;
            std::weak_ptr<const function_decl> decl;
            uint64_t chunk_unit = UINT64_MAX;    // the constructor chunk its body slot holds is current for this unit and class epoch
            uint64_t chunk_epoch = UINT64_MAX;
        };
        std::unordered_map<const function_decl*, ctor_dispatch_entry> ctor_dispatch_;   // a constructor's method-door dispatcher (its constructor chunk cached on it)
        // A constructor's chunk: its prologue (vm_compiler::constructor_prologue) ahead of its body, rebuilt when a class is (re)defined;
        // a replaced chunk stays alive for the frames still running it, and retire_unreachable_code frees one its class no longer declares
        struct ctor_chunk_entry {
            uint64_t unit = UINT64_MAX;
            uint64_t class_epoch = UINT64_MAX;
            std::shared_ptr<chunk> code;
            std::weak_ptr<const function_decl> decl;
            std::weak_ptr<const class_definition> level;
        };
        std::unordered_map<const function_decl*, ctor_chunk_entry> ctor_chunks_;
        std::vector<std::shared_ptr<chunk>> retired_ctor_chunks_;
        chunk* constructor_chunk(const std::shared_ptr<script_class_definition>& level, const std::shared_ptr<function_decl>& ctor);
        const script_value& constructor_dispatch(const std::shared_ptr<script_class_definition>& level, const std::shared_ptr<function_decl>& ctor,
                                                 const std::shared_ptr<environment>& definition_env);
        // One construction in flight (innermost last): its class, whose construction order the partial-destruction marks index, its
        // instance and definition environment, and whether code ran since create_instance seeded the defaults
        struct ctor_context {
            const class_definition* cls = nullptr;
            class_instance* instance = nullptr;
            std::shared_ptr<environment> definition_env;
            bool code_ran = false;
        };
        std::vector<ctor_context> ctor_contexts_;
        // a constructor frame's exit: the dispatch level it set back (ctor_exit_dispatch), the construction it owns (ctor_exit_context)
        static constexpr uint8_t ctor_exit_dispatch = 1, ctor_exit_context = 2;
        script_value worker_borrowed_string(const script_value& borrow, const script_value& index, const script_string& source);
        script_value worker_owned_string(const script_array* node, uint64_t index, const script_string& source);   // the cache by node and index
        // A worker-minted string node can be published into the parent's output by the region
        // that minted it; from then on the main thread copies it, so the worker must hold no
        // handle to it in a later region (the counts are not atomic). Called at the end of the
        // worker's chunk, on the worker thread, before the join.
        // A node only this worker's cache still holds was never published (or its holders died in the region): it stays for the next
        // region, whose hits compare the live bytes again
        static bool worker_string_private(const script_value& v) noexcept {
            return v.raw_storage_index() == script_value::TYPEID_STRING && v.unchecked_string_handle().unique();
        }
        void release_region_strings() noexcept {
            for (auto it = worker_borrowed_strings_.begin(); it != worker_borrowed_strings_.end();) {
                if (worker_string_private(it->second)) { ++it; } else { it = worker_borrowed_strings_.erase(it); }
            }
            for (auto& rec : worker_env_caches_) {
                for (script_value& own : rec->string_constants) {
                    if (own.raw_storage_index() == script_value::TYPEID_STRING && !worker_string_private(own)) { own = script_value(std::monostate{}, engine_); }
                }
            }
            // a block's copies stay for the next region: a string this region published leaves, a chunk it did not enter leaves whole
            const uint32_t region = engine_->parallel_region_serial();
            for (auto it = worker_block_constants_.begin(); it != worker_block_constants_.end();) {
                if (it->second.region != region) { it = worker_block_constants_.erase(it); continue; }
                for (script_value& own : it->second.values) {
                    if (own.raw_storage_index() == script_value::TYPEID_STRING && !worker_string_private(own)) { own = script_value(std::monostate{}, engine_); }
                }
                ++it;
            }
            worker_block_constants_code_ = nullptr;
            worker_block_constants_mru_ = nullptr;
        }
        // a block's constants on a worker: the chunk's, each string its own copy (a block borrows a string constant's node into its
        // cells and pushes, and the main engine and the other workers hold the chunk's node; the counts are not atomic). A copy kept
        // from an earlier region is checked against the chunk at its first use in a region (the address may name a new chunk)
        struct worker_constants {
            uint32_t region = 0;   // engine::parallel_region_serial of the region that last checked it
            std::vector<script_value> values;
        };
        std::unordered_map<const chunk*, worker_constants> worker_block_constants_;
        const chunk* worker_block_constants_code_ = nullptr;
        const script_value* worker_block_constants_mru_ = nullptr;
        const script_value* block_constants(const chunk& code) {
            if (!parallel_worker_) { return code.constants.data(); }
            if (worker_block_constants_code_ == &code) { return worker_block_constants_mru_; }
            return worker_block_constants_for(code);
        }
        const script_value* worker_block_constants_for(const chunk& code);
        environment* worker_root_env_ = nullptr;   // slot root env (identity stable per slot)
        // ::name's scope: the program's; a parallel worker's is the root of its own chain, which holds its copies of the program's
        // names (KEEP BYTE-PARALLEL with interpreter::program_scope)
        environment* program_scope() const noexcept {
            if (!parallel_worker_) { return engine_->get_global_environment().get(); }
            environment* result = environment_.get();
            while (result->parent_raw()) { result = result->parent_raw(); }
            return result;
        }

        // Compile a per-worker function copy's body on the MAIN thread at the region
        // barrier (compilation interns; workers must only ever hit) and pin the chunk on
        // the copy's own backend_body_cache.
        void precompile_parallel_function(const script_defined_function& fn) {
            fn.backend_body_cache = chunk_for_body(fn.name, fn.parameters(), fn.body, fn.local_count, fn.unit);
        }

        // Same barrier discipline for an admitted class method: warm THIS worker
        // backend's chunk cache so the region never compiles (compilation copies AST
        // literal strong_ptrs — main-thread-only traffic).
        void precompile_parallel_method(const function_decl& decl);
        // A construction's expression a worker evaluates outside any body (a field initializer, an initializer list's argument),
        // compiled at the barrier and found by the expression alone (the unit it would run under is the caller's)
        void precompile_parallel_expression(const expression_ptr& expr);

        void set_parallel_method_pins(const detail::parallel_method_pin_table* pins) override {
            parallel_method_pins_ = pins;
        }

    private:
        struct cell_state;
        struct frame {
            chunk* code = nullptr;
            std::shared_ptr<chunk> pin;
            size_t ip = 0;
            call_frame* locals = nullptr;
            std::shared_ptr<environment> entry_env;
            size_t stack_base = 0;
            bool top_level = false;
            unit_stamp unit{};                         // the unit of the code this frame runs: its function's stamp, the run's at top level
            // Native-entry / fiber frames: the declared return type and its detail::return_conv
            // class, converted against at the completion door (record frames carry theirs on
            // the call_record). class 0 = no door conversion (top-level / default-arg chunks).
            type_info_ptr return_type;
            uint8_t return_conv_class = 0;
            // Frame window: slot k lives at stack_[window_base + k] when window_backed
            // (record frames, native entries, fibers). window_live replicates
            // call_frame's grow-on-declare semantics: reads at/above it answer nullptr
            // ("reserved capacity only: not live yet").
            size_t window_base = 0;
            uint32_t window_live = 0;
            bool window_backed = false;
            bool raw = false;                          // raw-entered: its window is reserved and built when the general form first needs it
            // Debugger edge state (touched only while a session is armed): the last instruction this frame dispatched
            // inside a stop span, of which chunk, under which session. A reused record enters at ip 0, which the backend
            // tells from a back edge to ip 0 by the frame it dispatched last.
            const chunk* debug_prev_code = nullptr;
            uint32_t debug_prev_ip = 0;
            uint32_t debug_epoch = 0;
            struct cell_state* cells = nullptr;        // the record's raw area (null: a frame that never runs cells)
            // a worker's private lookup rows for the chunk env_table_code names (worker_env_cache_slot's), found once by the frame's first
            // lookup; any other chunk (a reused record, a frame that never looked one up) reads through worker_env_cache_slot
            const chunk* env_table_code = nullptr;
            env_lookup_cache_entry* env_table = nullptr;
            const script_defined_function::converted_captures* captures = nullptr;   // the running converted closure's (op_bind_captures)
            // a member-class body's receiver whose class lays the body's fields out at the compiled slots (bind_member_layout), under
            // the engine's layout generation this_gen; null: the body's bare names take the general reads
            class_instance* this_inst = nullptr;
            const class_definition* this_cd = nullptr;
            uint64_t this_gen = 0;
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
            uint64_t timed_entry = 0;                  // the clock at entry: its function's inclusive time
#endif
        };
        // a byte per cell: a cell store marks its cell with one byte write, so consecutive stores carry no read-modify-write chain
        struct cell_dirty {
            uint8_t b[chunk::k_cell_limit] = {};
            void set(uint32_t k) noexcept { b[k] = 1; }
            void clear(uint32_t k) noexcept { b[k] = 0; }
            bool test(uint32_t k) const noexcept { return b[k] != 0; }
            // the first n bytes, in fixed widths the compiler stores inline (a byte past n is never set by the loop that owns n)
            void reset(size_t n) noexcept {
                if (n <= 16) { std::memset(b, 0, 16); } else if (n <= 64) { std::memset(b, 0, 64); } else { std::memset(b, 0, sizeof(b)); }
            }
            chunk::cell_mask mask(size_t n) const noexcept { chunk::cell_mask m; for (uint32_t k = 0; k < n; ++k) { if (b[k]) m.set(k); } return m; }
            bool any_of(const std::vector<uint32_t>& cells) const noexcept { for (const uint32_t k : cells) { if (b[k]) return true; } return false; }
        };
        // Cell loops: a call record's raw area: the cells, the active loop and a dirty byte per cell. Cleared at record push and
        // on every exit, miss and raise.
        // A cell is laid out as a script_value: the payload word, then the tag word whose low byte is the kind (0 empty, else the
        // storage index) and whose high bits are the stamp, the slot's type stamp at entry (a local's declared type: int, float or
        // any; null for temps, pinned cells and env values). A float's payload is its bits, a string's the borrowed node, a bool's 0 or 1
        struct alignas(8) cell_word {
            int64_t bits;
            uint8_t kind;
            uint8_t stamp_high[7];
            type_info* stamp() const noexcept { uint64_t w; std::memcpy(&w, &kind, sizeof w); return reinterpret_cast<type_info*>(static_cast<uintptr_t>(w & ~uint64_t{0x1FF})); }
            void set_stamp(const type_info* t) noexcept { const uint64_t w = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(t)) | kind; std::memcpy(&kind, &w, sizeof w); }
            void set_kind_stamp(uint8_t k, const type_info* t) noexcept { const uint64_t w = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(t)) | k; std::memcpy(&kind, &w, sizeof w); }
        };
        static_assert(sizeof(cell_word) == sizeof(script_value) && offsetof(cell_word, kind) == 8);
        struct cell_state {
            std::vector<cell_word> cells;
            uint32_t loop = k_invalid_u32;
            // a raw call in flight from this frame: 1 + the call index (its landing, if the callee comes back through the general return, binds
            // nothing of this frame's by reference); and 1 + the call index whose probe found a raw callee without parking it (its arguments
            // instruction enters it). A block's calls number below 65535 (the translation marks no raw site past that)
            uint16_t raw_landing = 0;
            uint16_t raw_call = 0;
            cell_dirty dirty;
            std::vector<const script_array*> site_node;  // per copy instruction: the load/store site's container node, resolved at entry
            std::vector<const script_array*> site_node2;  // per copy instruction: a push site's source container (an element push), resolved at entry
            std::vector<const script_value*> site_member;   // per copy instruction: a member read site's field value, resolved at entry (the instance's plain field)
            std::vector<class_definition*> site_member_class;   // per copy instruction: the class the member site's field slot was probed for (an alias site: per element; nullptr = probe)
            std::vector<uint32_t> site_member_slot;              // per copy instruction: that class's field slot
            // per copy instruction of a nested member site (`p.pos.x`): the prefix field's slot for a (class, method epoch); sized when such a
            // site first resolves, cleared at every entry (another loop's site may sit at the same offset)
            struct prefix_slot { const class_definition* cd = nullptr; uint64_t epoch = 0; uint32_t slot = UINT32_MAX; };
            std::vector<prefix_slot> site_prefix;
            // per rebind of the running loop: the field slots its member sites found for the last element's class and method epoch (and each
            // prefix's class and slot), so the next element of the same class reads its fields through the slots directly
            struct rebind_memo {
                const chunk* code = nullptr; uint32_t copy_ip = UINT32_MAX;
                const class_definition* cd = nullptr; uint64_t epoch = 0;
                uint8_t prefix_count = 0;
                const class_definition* prefix_cd[4] = {}; uint64_t prefix_epoch[4] = {}; uint32_t prefix_slot[4] = {};
                std::vector<uint32_t> slot; std::vector<uint8_t> prefix_of;   // per dependent: the field slot, and its prefix (0xFF: the element itself)
            };
            std::vector<rebind_memo> rebind_memos;
            // how each site's container is reached (recorded by the resolution): its frame slot, or its environment binding; a landing
            // after a call re-checks the node's identity through that path instead of resolving again. An environment cell's binding
            // and the environment and epoch at entry: while both hold, the bindings are re-read directly (the lookup cache's own rule)
            std::vector<uint32_t> site_slot;
            std::vector<const script_value*> site_env;
            std::vector<const script_value*> site_value;   // per site: the container value the entry resolved (a flush builds an alias's element reference from its source's)
            std::vector<const script_value*> env_ptr;
            const environment* env_of = nullptr;
            uint64_t env_serial = 0;
            const void* env_loop = nullptr;   // the loop whose entry recorded env_ptr in this call (its next entry re-reads them while the environment holds)
            // the refusal valve: loops (by index, below 64) whose entry a container site refused in this call; container bindings
            // do not change within a call, so the entry is not attempted again until the next call
            uint64_t refused_loops = 0;
            // a parked block (a boundary operation): the general form runs a statement and the block resumes where it ends (park_at, a general
            // ip) when nothing entered another block of this frame since (entry_serial, counted by every entry)
            uint32_t park_at = k_invalid_u32;
            uint32_t park_loop = k_invalid_u32;
            uint32_t park_boundary = 0;
            uint32_t park_serial = 0;
            uint32_t entry_serial = 0;
            void reset(size_t cells) { loop = k_invalid_u32; dirty.reset(cells); }
            // a call's entry: cells = the body's largest loop (chunk::max_cells), past which this frame never reads a dirty byte
            void reset_call(size_t cells) { reset(cells); refused_loops = 0; park_at = k_invalid_u32; env_loop = nullptr; }
        };

        struct frame_guard;

        // Flat value stack with a growth CHOKEPOINT: the vm caches raw
        // script_value* into frame windows across dispatch iterations (counted-for fast
        // states — the ONE sanctioned cross-op raw-pointer cache, invariants §2b), so
        // every reallocation rebases them (rebase_window_pointers). Deliberately flat,
        // NOT segmented: operand push/pop is the hot-loop path and must stay pure index
        // arithmetic. push_back self-alias (op_dup pushing back()) is grow-safe: the
        // value detours through a temp on the grow branch only.
        // Owned raw buffer: truncation ELIDES the destructor for slots it can
        // prove effect-free — trivial-band payload (null/int/float/bool/char; string sits
        // mid-band and the mask excludes it) AND no canonical type tag. Such a dtor is a
        // no-op by construction (storage's trivial branch + null strong_ptr decref + raw
        // engine_), so eliding it is observationally identical. Tagless temps and null
        // window padding pop for free; typed locals keep their tag decref through the
        // real dtor, first->last like vector's tail erase. Relocation is memcpy —
        // script_value is trivially relocatable by design (storage relocation runs no
        // effectful ctor/dtor; refcounts ride the bytes).
        struct value_stack {
            script_value* slots = nullptr;
            size_t count = 0;
            size_t cap = 0;
            vm_backend* owner = nullptr;

            value_stack() = default;
            value_stack(const value_stack&) = delete;
            value_stack& operator=(const value_stack&) = delete;
            ~value_stack();

            static constexpr uint32_t trivial_payload_mask =
                (1u << script_value::TYPEID_NULL) | (1u << script_value::TYPEID_INT) |
                (1u << script_value::TYPEID_FLOAT) | (1u << script_value::TYPEID_CHAR) |
                (1u << script_value::TYPEID_BOOL);
            // a trivial payload destroys nothing whatever its stamp: the type tag is a raw pointer wrapper and the engine a raw
            // pointer, so a typed local's int, float, bool, char or null pops as free as a tagless temp
            static bool dtor_elidable(const script_value& s) noexcept {
                return ((trivial_payload_mask >> s.raw_storage_index()) & 1u) != 0;
            }

            JAI_FORCEINLINE void push_back(const script_value& x) {
                if (count == cap) [[unlikely]] { grow_push(script_value(x)); return; }
                new (slots + count) script_value(x);
                ++count;
            }
            JAI_FORCEINLINE void push_back(script_value&& x) {
                if (count == cap) [[unlikely]] { grow_push(std::move(x)); return; }
                new (slots + count) script_value(std::move(x));
                ++count;
            }
            // doubling from a small first block: a fiber's stack starts empty, and every suspended coroutine keeps its own
            static constexpr size_t grown_capacity(size_t a_cap) noexcept { return a_cap < 32 ? 64 : a_cap * 2; }
            void grow_push(script_value x);   // out of line: relocate + rebase + push
            void relocate_to(size_t new_cap); // the ONE buffer move (grow_push + reserve)
            script_value& back() noexcept { assert(count); return slots[count - 1]; }
            const script_value& back() const noexcept { assert(count); return slots[count - 1]; }
            // A release can run code (a script or host destructor) that pushes onto this stack and moves its buffer, so a
            // slot dies in place while count still covers it (the re-entry pushes above it, never over it) and the loop
            // re-reads the buffer by index. A handle's destructor reads its pointer once, nulls it and releases through a
            // local, so the dying slot is not touched after the release, wherever the buffer went (invariants §2c).
            JAI_FORCEINLINE void pop_back() noexcept {
                assert(count);
                script_value& s = slots[count - 1];
                if (!dtor_elidable(s)) { s.~script_value(); }
                --count;
            }
            void truncate(size_t new_size) noexcept {
                assert(new_size <= count);
                // last slot first: a frame's locals die in reverse declaration order (C++)
                for (size_t i = count; i > new_size; --i) {
                    if (!dtor_elidable(slots[i - 1])) { slots[i - 1].~script_value(); }
                }
                count = new_size;
            }
            // Bulk window padding: one capacity check + in-place null construction
            // (the per-slot make_null ctor+move+cap-check loop was the fill half of
            // the window lifecycle tax). No-op when target <= size.
            void fill_null_to(size_t target, class engine* eng) {
                if (target > cap) [[unlikely]] {
                    const size_t grown = grown_capacity(cap);
                    relocate_to(target > grown ? target : grown);
                }
                while (count < target) {
                    new (slots + count) script_value(std::monostate{}, eng);
                    ++count;
                }
            }
            size_t size() const noexcept { return count; }
            bool empty() const noexcept { return count == 0; }
            void clear() noexcept { truncate(0); }
            void reserve(size_t want);
            script_value* data() noexcept { return slots; }
            const script_value* data() const noexcept { return slots; }
            script_value& operator[](size_t i) noexcept { assert(i < count); return slots[i]; }
            const script_value& operator[](size_t i) const noexcept { assert(i < count); return slots[i]; }
            // Fiber stack flip: payload only — owner names the rebase target and stays put
            void swap_payload(value_stack& other) noexcept {
                std::swap(slots, other.slots);
                std::swap(count, other.count);
                std::swap(cap, other.cap);
            }
        };
        // Rebase every sanctioned raw pointer into the old stack buffer (counted-for
        // fast states) onto the new one; called only from value_stack::grow_push.
        void rebase_window_pointers(const script_value* old_begin, const script_value* old_end,
                                    script_value* new_begin);

        // === Slot-accessor funnel: the frame window implementation ===
        // EVERY read/write of a frame's slot-local storage in this backend goes through
        // these. Window-backed frames address stack_[window_base + slot]; frames without
        // a window forward to call_frame. Callers keep their own slot-validity/top_level
        // guards — these assume a live locals home.
        // Statement-position results: a call/store op immediately followed by op_pop
        // discards its value, so the landing drops the value and skips the pop dispatch
        // together (the compiler still emits the pop; every landing route honors it); every chunk ends in op_halt, so the next
        // instruction exists
        bool next_op_discards_result(const frame& f) const noexcept {
            return f.code->code[f.ip + 1].op == opcode::op_pop;
        }
        // An assignment expression's value is consumed unless a POP follows (a statement) or the
        // top-level implicit return does: the host's convenience result stays the right-hand side
        // (no read-back copy, no extra getter call for a statement-shaped last line)
        bool assignment_result_used(const frame& f) const noexcept {
            const opcode next = f.code->code[f.ip + 1].op;
            return next != opcode::op_pop && next != opcode::op_implicit_return;
        }
        // every chunk ends in op_halt, so a call's next instruction exists
        JAI_FORCEINLINE void land_call_result(frame& caller, script_value&& v) {
            const vm_instruction* code = caller.code->code.data();
            const size_t ip = caller.ip;
            assert(ip + 1 < caller.code->code.size());
            if (code[ip].op == opcode::op_call_typed && call_typed_land(caller, v)) { caller.ip += 2; return; }
            const opcode next = code[ip + 1].op;
            if (next == opcode::op_pop) { caller.ip += 2; return; }
            if (next == opcode::op_call_temp) { frame_slot_set(caller, code[ip + 1].a, std::move(v)); caller.ip += 2; return; }   // op_call_temp's move, the stack skipped
            if (next == opcode::op_c_land && caller.code->cell_loops[code[ip + 1].b].calls[code[ip + 1].a].consumer == 0) { ++caller.ip; return; }   // a statement call inside a block: the landing has nothing to consume
            stack_.push_back(std::move(v));
            ++caller.ip;
        }
        // Every frame enters through here (record, raw and session entries, method and tail-call pushes, fibers and native entries through
        // frame_guard): it joins frames_, and its function's captures bind into its slots (a raw-entered frame's slots are its cells until
        // build_raw_window builds its window).
        JAI_FORCEINLINE void enter_frame(frame& f, const script_defined_function* function = nullptr) {
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
            f.timed_entry = coverage_clock();
#endif
            frames_.push_back(&f);
            bind_frame_captures(f, function);
        }
        // The one place a function's captures bind into its frame: a converted closure's cells, which its body's first
        // instruction (op_bind_captures) lands in its capture slots once the window stands
        static void bind_frame_captures(frame& f, const script_defined_function* function) noexcept {
            f.captures = function && function->converted() ? function->captures.get() : nullptr;
        }
        // a frame whose code reads its slots: a call frame, or a program frame whose block-scoped declarations have them (its window)
        static bool has_slots(const frame& f) noexcept { return f.locals && (!f.top_level || f.window_backed); }
        script_value* frame_slot(frame& f, size_t slot) noexcept {
            if (f.window_backed) {
                return slot < f.window_live ? &stack_[f.window_base + slot] : nullptr;
            }
            return f.locals->get_local(slot);
        }
        const script_value* frame_slot(const frame& f, size_t slot) const noexcept {
            if (f.window_backed) {
                return slot < f.window_live ? &stack_[f.window_base + slot] : nullptr;
            }
            return static_cast<const call_frame*>(f.locals)->get_local(slot);
        }
        void frame_slot_set(frame& f, size_t slot, script_value value) {
            if (f.window_backed) {
                const size_t idx = f.window_base + slot;
                if (idx < stack_.size()) {
                    stack_[idx] = std::move(value);
                } else {
                    // Build-up writes (pooled-arg binding, defaults): fill the gap with
                    // live nulls exactly like call_frame::set_local's slot>size path
                    stack_.fill_null_to(idx, engine_);
                    stack_.push_back(std::move(value));
                }
                if (slot >= f.window_live) { f.window_live = static_cast<uint32_t>(slot + 1); }
                return;
            }
            f.locals->set_local(slot, std::move(value));
        }
        size_t frame_slot_count(const frame& f) const noexcept {
            return f.window_backed ? f.window_live : f.locals->local_count();
        }
        // Storage-handle twins for paths that hold a call_frame slot home directly
        // (coroutine fiber state, the caller side of ref-param binds on fiber frames).
        static script_value* frame_slot(call_frame& locals, size_t slot) noexcept {
            return locals.get_local(slot);
        }
        static void frame_slot_set(call_frame& locals, size_t slot, script_value value) {
            locals.set_local(slot, std::move(value));
        }

        // Return classification (detail/return_conv.hpp, shared with the interpreter),
        // cached per function (backend_return_conv) and stamped on the record at push.
        // unclassified routes through the full conversion (method frames); the prim_*
        // classes pass a storage-matching result through verbatim — exactly what the full
        // path's identity try_convert produces for primitives.
        using return_conv = detail::return_conv;
        static return_conv classify_return_conv(const type_info_ptr& t) { return detail::classify_return_conv(t.get()); }

        // One logical script->script activation executed inside the current native run()
        // (Squirrel EnterFrame shape). Records live behind unique_ptr in a grow-only pool
        // so &rec.f stays address-stable (frames_ entries, try_record.owner and rec.caller
        // hold raw frame*).
        struct call_record {
            frame f;                                   // frames_ gets &f; f.locals = &locals
            call_frame locals;                         // record-owned slot storage; vector capacity reused across calls
            cell_state cells;                          // cell loops: the raw area, capacity reused across calls
            script_value callee_pin{std::monostate{}, nullptr};   // moved-off-stack callee value: pins the callable
            strong_ptr<script_function_node> direct_pin;    // probe-called frames: pin without a callee value/slot
            type_info_ptr return_type;                 // pop drivers convert the result against this
            uint8_t return_conv_class = 0;             // return_conv stamped at push (0 = the full conversion)
            // the caller's unit is the frame below's (vm_backend::caller_frame_unit); no stamp is saved per call
            std::shared_ptr<function_decl> ast_pin;    // a worker's method frame: its resolved overload outlives a mid-call hot reload
            const function_decl* ast = nullptr;        // the method frame's overload, kept alive by ast_pin or by the backend's retained declarations
            // the overloads this pooled record pinned before: a depth that alternates among a few methods swaps pins without counting them
            std::shared_ptr<function_decl> ast_pin_ring[3];
            uint8_t ast_pin_next = 0;
            void pin_ast(const std::shared_ptr<function_decl>& ast) {
                for (auto& kept : ast_pin_ring) {
                    if (kept.get() == ast.get()) { ast_pin.swap(kept); return; }
                }
                ast_pin.swap(ast_pin_ring[ast_pin_next]);
                ast_pin_next = static_cast<uint8_t>((ast_pin_next + 1) % 3);
                ast_pin = ast;
            }
            bool method_result_anchor = false;         // method frames: replicate make_bound_method's keep-alive fix-up
            // In-loop ctor-body frames (exec_new shared_ptr arm): stamp the completed
            // frame's OBJECT result with this type, replicating the native arm's
            // post-call value.set_type_info(expr->type). Raw ptr: the new_expr's type
            // is AST-pinned. Applied at both completion doors (return_with_result +
            // fall_off_script_frame), never on unwind paths.
            type_info* ctor_result_stamp = nullptr;
            bool ctor_frame = false;   // a constructor body run through the method door: every return yields the frame's `this`
            uint8_t ctor_exit = 0;     // ctor_exit_* the pop undoes
            uint8_t ctor_restore_phase = 0;
            bool ctor_relaid = false;
            class_definition* ctor_level = nullptr;     // the level the instance answered as
            class_definition* ctor_restore = nullptr;   // what it answered as before
            frame* caller = nullptr;                   // resume target: native entry frame or another record's f
            std::shared_ptr<environment> prev_env;
            bool env_lazy = false;
            bool lean = false;   // pushed by push_lean_method_frame or push_plain_frame on an untouched environment: its exit is pop_lean_frame
            // Lazy no-closure frames never touch environment_ (nothing saved,
            // nothing restored) — the biggest per-call env cost on the fib path gone
            bool env_untouched = false;
            size_t try_base = 0;
            size_t iter_base = 0;
            size_t cfor_base = 0;
            size_t pending_base = 0;
            uint64_t raw_mark = 0;     // a raw-entered record: the raw epoch at its entry (its raw return re-validates the caller when the epoch moved)
            // a raw-entered record's return, decided at its entry: the caller's call instruction (its ip and its raw_landing mark), the caller's
            // loop and the call's destination cell, the result kind a raw return lands (0: none), and how it lands (k_raw_land_*, or the contract
            // kind of a compiler value)
            static constexpr uint8_t k_raw_land_drop = 0xFF, k_raw_land_rule = 0xFE;   // a statement call, a declared local (cell_land's rule)
            // the body whose raw entry last wrote the fields every raw entry of it writes alike (return type, name, member scope, frame flags,
            // the cells' sizes); any other push and the general pop clear it
            const chunk* raw_prepared = nullptr;
            const chunk::cell_loop* raw_caller_loop = nullptr;
            size_t raw_parked_ip = SIZE_MAX;
            uint32_t raw_dst = 0;
            uint16_t raw_landing = 0;
            uint8_t raw_ret_kind = 0;
            uint8_t raw_land = 0;
        };
        std::vector<std::unique_ptr<call_record>> call_records_;  // grows, never shrinks mid-run
        size_t call_records_top_ = 0;                  // records [0, top) are live
        frame* switch_to_ = nullptr;                   // set by the push path, consumed by op_call's case

        // Pending call-site context armed around OPAQUE invokes (std::function values:
        // bound methods, constructors, function values) and consumed once by the next
        // call_script_function entered - ref params bind against the caller's variables
        // through it. Save/restored as a single value by nested opaque calls; shelved
        // across external (C++) invocations by the external_call_guard overrides.
        struct pending_call_site {
            const call_site* site = nullptr;
            frame* caller_frame = nullptr;
            chunk* caller_code = nullptr;
            const std::vector<script_value>* arguments = nullptr;   // as passed (a host callable's T& resolution)
        };
        pending_call_site pending_site_ctx_{};
        std::vector<pending_call_site> external_site_stack_;

        string_symbolizer* symbolizer_;
        // Shadowing unit of the running code (string_symbolizer::current_unit): set at every frame
        // push from the callee's function or class, restored at pop; a worker never writes it
        void enter_unit(unit_stamp unit) noexcept { if (!parallel_worker_ && symbolizer_->shadow_count() != 0) { symbolizer_->set_current_unit(unit); } }
        // derived: a scoped override at its depth (a class's field initializers), else the top frame's, else the ambient
        // (no frame: the run's stamp execute_parsed installed)
        unit_stamp current_unit() const noexcept override {
            if (unit_override_ && frames_.size() <= unit_override_depth_) return *unit_override_;
            if (!frames_.empty()) return frames_.back()->unit;
            return symbolizer_->current_unit();
        }
        // the frame below the top one: the caller of the frame just pushed (a by-reference argument resolves under it)
        unit_stamp caller_frame_unit() const noexcept { return frames_.size() >= 2 ? frames_[frames_.size() - 2]->unit : symbolizer_->current_unit(); }
        // the ambient re-aligned with the executing frame: a store only while a name has versions
        void sync_unit() noexcept override { if (!parallel_worker_ && symbolizer_->shadow_count() != 0) { symbolizer_->set_current_unit(current_unit()); } }
        const unit_stamp* unit_override_ = nullptr;
        size_t unit_override_depth_ = 0;
        struct unit_override_scope {   // the override for a span of code without a frame of its own; the ambient re-aligned after
            vm_backend* vm; const unit_stamp* prev; size_t prev_depth;
            unit_override_scope(vm_backend* v, const unit_stamp* unit) : vm(v), prev(v->unit_override_), prev_depth(v->unit_override_depth_) {
                vm->unit_override_ = unit; vm->unit_override_depth_ = vm->frames_.size(); vm->enter_unit(*unit);
            }
            ~unit_override_scope() { vm->unit_override_ = prev; vm->unit_override_depth_ = prev_depth; vm->sync_unit(); }
        };
        struct unit_sync_scope { vm_backend* vm; ~unit_sync_scope() { vm->sync_unit(); } };   // the frame below's unit again when this scope ends
        // Epoch sink handed to every environment this vm constructs (== symbolizer_
        // everywhere except a parallel worker; see configure_parallel_worker)
        string_symbolizer* env_symbolizer_ = nullptr;
        std::shared_ptr<environment> environment_;
        std::vector<std::shared_ptr<environment>> instance_scope_saved_;
        std::shared_ptr<environment> pending_instance_env_;
        engine* engine_ = nullptr;
        type_info* ti_int_ = nullptr; type_info* ti_float_ = nullptr; type_info* ti_bool_ = nullptr; type_info* ti_string_ = nullptr;   // the engine's canonical stamps, cached with the engine
        uint64_t shared_ptr_ctor_symbol_ = 0, weak_ptr_ctor_symbol_ = 0;   // type_ctor_base_symbol's two names, interned with this engine's symbolizer
        uint64_t type_ctor_base_symbol(uint64_t sym);
#ifdef JAISCRIPT_MISS_INJECTION
        uint32_t miss_every_ = 0;        // engine::cells_miss_every, read at every block entry
        uint32_t miss_countdown_ = 0;    // plain cell instructions until the next injected miss; runs across entries
#endif
#ifdef JAISCRIPT_VM_COVERAGE
    public:
        // the execution-coverage census (vm/coverage.hpp, defined in vm_coverage.cpp): this backend's tables, adopted by the engine
        // at set_engine_reference so a worker's outlive it
        std::shared_ptr<coverage_tables> coverage_ = std::make_shared<coverage_tables>();
        void coverage_general(frame& f);
        void coverage_cell(const frame& f, size_t ip, opcode op);
        void coverage_cell_pair(const frame& f, size_t ip, const vm_instruction& ci, size_t next);
        void coverage_entry(const frame& f, uint32_t loop, bool taken);
        void coverage_miss(const frame& f);
    private:
        void coverage_observe(frame& f, const vm_instruction& ins, coverage_chunk& row);
        coverage_chunk& coverage_row(const chunk& c);
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
        void coverage_frame_exit(const frame& f);
        // a nested dispatch loop's instructions are its own: the running instruction's clock stops for its span
        struct coverage_timing_pause {
            coverage_tables& t; coverage_chunk* row; size_t ip;
            explicit coverage_timing_pause(coverage_tables& a_tables) : t(a_tables), row(a_tables.timed_row), ip(a_tables.timed_ip) {
                t.charge(coverage_clock());
                t.timed_row = nullptr;
            }
            ~coverage_timing_pause() {
                t.charge(coverage_clock());
                t.timed_row = row;
                t.timed_ip = ip;
                t.timed_since = coverage_clock();
            }
        };
#endif
#endif
        // Step-debugger controller (engine-owned, shared with the interpreter backend), wired by the first engine::debugger()
        // call, which runs before the script thread starts or on it; read only at the sync points, where sync_debug_hook
        // mirrors it into the plain gate below.
        std::atomic<debug::controller*> debug_controller_{nullptr};
        // Script-thread cached statement-hook gate: non-null only while a session is
        // enabled; run_dispatch tests this one plain pointer per dispatch iteration.
        debug::controller* debug_hook_ = nullptr;
        // Bumped at every arming: a frame's edge state from an earlier session is stale. The last dispatch's frame
        // tells a new frame at ip 0 from a jump back to ip 0.
        uint32_t debug_epoch_ = 1;
        const frame* debug_last_frame_ = nullptr;
        const chunk* debug_last_code_ = nullptr;
        size_t debug_last_depth_ = SIZE_MAX;
        environment* cached_global_env_ = nullptr;   // env_lookup_cached gate (never dereferenced)
        // stdlib math aliases (engine::register_math_alias): the engine's rows, or on a worker its own copies' rows
        const engine::math_alias_row* math_alias_rows_ = nullptr;
        std::unique_ptr<std::array<engine::math_alias_row, engine::k_math_alias_rows>> worker_math_alias_rows_;
    public:
        // a worker's provisioned copy of a host function: an alias still bound to the registered builtin compares against the copy
        void adopt_math_alias(uint64_t symbol, const script_value& copy);
    private:

        vm_compiler compiler_;
        // a chunk belongs to one unit: every cache of compiled bodies is keyed by the node AND the
        // unit that runs it, so a body two units share (a textual include's) compiles once per unit and its lookup caches
        // never serve another unit's bindings
        struct node_unit_key {
            const void* node = nullptr;
            uint32_t unit = 0;
            bool operator==(const node_unit_key& o) const noexcept { return node == o.node && unit == o.unit; }
        };
        struct node_unit_hash {
            size_t operator()(const node_unit_key& k) const noexcept { return std::hash<const void*>{}(k.node) ^ (static_cast<size_t>(k.unit) * 0x9E3779B97F4A7C15ull); }
        };
        struct chunk_cache_entry {
            std::shared_ptr<chunk> compiled;
            std::shared_ptr<block_stmt> pin;
        };
        std::unordered_map<node_unit_key, chunk_cache_entry, node_unit_hash> chunk_cache_;

        value_stack stack_;
        // the active frames, innermost last: slots.size() is the capacity, so a push is a compare and a store
        struct frame_stack {
            std::vector<frame*> slots;
            size_t top = 0;
            void push_back(frame* f) { if (top == slots.size()) [[unlikely]] { grow(); } slots[top++] = f; }
            void pop_back() noexcept { assert(top); --top; }
            frame* back() const noexcept { assert(top); return slots[top - 1]; }
            frame* operator[](size_t i) const noexcept { assert(i < top); return slots[i]; }
            size_t size() const noexcept { return top; }
            bool empty() const noexcept { return top == 0; }
            size_t capacity() const noexcept { return slots.size(); }
            void clear() noexcept { top = 0; }
            void reserve(size_t n) { if (slots.size() < n) { slots.resize(n); } }
            void shrink_to_fit() { slots.resize(top); slots.shrink_to_fit(); }
            auto rbegin() const noexcept { return std::make_reverse_iterator(slots.begin() + static_cast<std::ptrdiff_t>(top)); }
            auto rend() const noexcept { return slots.rend(); }
            JAI_NOINLINE void grow() { slots.resize(slots.empty() ? 64 : slots.size() * 2); }
        };
        frame_stack frames_;
        size_t top_frame_mark_ = SIZE_MAX;   // frames_ index of the innermost top-level frame (a trace counts the frames above it)

        // Pending-callee register stack (callee-first): identifier
        // callees resolve at the pre-args observation point WITHOUT materializing on
        // the value stack. LIFO across nested arg calls; per-FIBER (run_fiber swaps it
        // with the other execution stacks - `f(yield x)` suspends between probe and
        // call); truncated on every unwind exactly like the value stack (try records,
        // record pops, native-entry cleanups).
        struct pending_callee {
            script_value value{std::monostate{}, nullptr};   // opaque/default-arg path; a sibling method's dispatcher
            const script_defined_function* fn = nullptr;     // direct path
            strong_ptr<script_function_node> pin;
            const std::vector<uint32_t>* staged_slots = nullptr;
            const script_method_dispatch* method = nullptr;   // a bare sibling method of this frame's class: value is its dispatcher, the receiver the frame's this
            const script_function* host_fn = nullptr;   // a host callable the call's site cached at the probe, borrowed: the site's while it caches it, else retired
            const void* native_entry = nullptr;   // a bare sibling call of the frame receiver's C++ base method: value is the host method, the lane the site's mic_native
            // a host callable held by pin, or borrowed from its site (no fn): invoked with the arguments as they are
            bool native() const { return !fn && (pin || host_fn); }
            const script_function& native_fn() const { return pin ? *pin : *host_fn; }
        };
        std::vector<pending_callee> pending_callees_;

        std::optional<script_value> return_value_;
        bool has_return_value_ = false;
        std::optional<script_value> implicit_result_;

        std::optional<script_exception> current_exception_;
        std::optional<script_value> active_exception_value_;
        bool is_unwinding_ = false;
        bool trace_captured_ = false;
        std::vector<stack_frame> captured_trace_;
        std::optional<error_propagator> run_error_;   // the runtime error that ended this run's top level
        detail::nested_failure nested_failure_;
        // "Inside a catch block" marker of the rethrow protocol (the catch symbol, or the
        // unnamed catch's "" symbol) - the catch VARIABLE itself is a frame slot / scope
        // local bound at handler entry, never resolved through this. Parked per fiber
        // with the exception pair (run_fiber).
        uint64_t current_catch_var_id_ = 0;

        // One record per active try/catch region, innermost last (dual error model:
        // catches checked_result failures AND throw-flag unwinding, like the interpreter)
        struct try_record {
            frame* owner = nullptr;
            size_t handler_ip = 0;
            uint64_t catch_var = 0;
            uint32_t catch_slot = k_invalid_u32;   // frame slot of the catch variable (k_invalid_u32 = scope env)
            bool catch_env_pushed = false;         // handler entry pushed the catch variable's scope env
            const environment* catch_env = nullptr;   // that env, which an error leaving the handler pops with the scopes inside it
            std::optional<script_exception> saved_exception;
            std::optional<script_value> saved_exception_value;
            uint64_t saved_catch_var_id = 0;
            bool saved_unwinding = false;
            bool in_catch = false;
            size_t stack_size = 0;
            size_t iter_size = 0;
            size_t cfor_size = 0;
            size_t pending_size = 0;
            int call_depth = 0;                    // the depth at the try (no graft encloses a try): the handler restores it
            std::shared_ptr<environment> entry_env;
            std::shared_ptr<caught_error_scope> caught_scope;   // the handler's bound error: its thrown value dies with the record
            size_t trace_grafts_below = SIZE_MAX;   // trace_grafts_through's sum under the owner frame, which cannot change while the record lives
        };
        std::vector<try_record> try_records_;
        void release_catch_variable(frame& f, const try_record& rec);
        void release_loops(frame& f, size_t base);
        void kill_unwound_try_locals(frame& f, size_t handler_ip);
        void pop_scopes_through(const environment* scope);
        // C++'s category of an argument's, a capture's or a return's source expression (detail::classify_source)
        uint8_t source_class(const expression* e);
        bool argument_skips_copy(const chunk* caller_code, const call_site& site, size_t i, const script_value& arg);

        struct iter_state {
            std::optional<script_value> container;
            std::shared_ptr<class_definition> pair_class;   // a plain loop variable's pair over a map
            type_info_ptr map_value_type;                    // a by-reference entry's store converts to it
            size_t index = 0;
            script_map::iterator map_it{};
            bool is_map = false;
            bool map_started = false;  // map_it names the entry the last step bound: the next step advances past it
            bool is_string = false;    // chars by value, like s[i]
            bool is_utf8 = false;      // a utf8_string: characters by value, like u[i]
            bool env_pushed = false;   // slotted loop vars skip the iteration scope env
            // A slotted by-reference loop variable: its slot is cleared when the loop ends
            // (normal exit, break, or an unwind caught in `owner`), so no element cursor
            // outlives its loop - a later parallel region over the same array must not see
            // a live reference the program can no longer name
            size_t slot = SIZE_MAX;
            const frame* owner = nullptr;
            std::shared_ptr<coroutine_handle> coroutine;   // set = coroutine-driven range-for
            // a slotted loop variable's frame and slot: the loop's interior holds that slot and the ones above it, which die before
            // the range when a return, an unwind or an abandoned coroutine leaves the loop
            const frame* body_frame = nullptr;
            uint32_t first_slot = UINT32_MAX;
            const environment* scope = nullptr;   // an environment loop variable's scope (env_pushed), which dies before the range
        };
        std::vector<iter_state> iter_states_;

        // Counted-for runtime state (op_cfor_prep/back/pop). Pointers are cached into
        // environment/frame storage — stable for the loop's lifetime (deque-backed envs,
        // and fibers pin their env chains across suspension).
        struct counted_for_state {
            script_value* var = nullptr;
            script_int* end_ptr = nullptr;
            script_int end_val = 0;
            script_int* step_ptr = nullptr;
            script_int step_val = 1;
            uint8_t cmp = 0;
            bool subtract = false;
            bool incdec = false; // ++/-- update: overflow error names the real op
            bool fast = false;   // false: run the loop's generic cond/update bytecode
        };
        std::vector<counted_for_state> cfor_states_;

        // Suspended-fiber state stored opaquely on a coroutine_handle (frames+ip+stack model,
        // not the interpreter's AST continuation replay). Defined in vm_backend.cpp.
        struct vm_coroutine_state;
        struct vm_run_state;   // a suspendable run's own copy of every member below that execution changes (swap_run_state)
        coroutine_handle* active_coroutine_ = nullptr;
        bool yielding_ = false;


        bool has_custom_numeric_ops_ = false;
        bool has_custom_binary_ops_ = false;
        const detail::engine_operator_table* operator_table_ = nullptr;   // engine-owned flat dispatch
        std::function<std::shared_ptr<class_definition>(const std::string&)> class_lookup_;

        std::chrono::nanoseconds execution_budget_{std::chrono::seconds(5)};
        std::chrono::steady_clock::time_point execution_deadline_{};
        uint32_t budget_tick_ = 0;
        bool budget_active_ = false;
        int current_call_depth_ = 0;

        // Execution-limit state (terminal-error latch). Engine-less fallback storage;
        // prepare_for_execution repoints limits_ at the engine's per-engine instance so
        // reentrant executes share it and terminal errors cross the reentrant boundary.
        detail::execution_limits local_limits_;
        detail::execution_limits* limits_ = &local_limits_;

        detail::operator_ids ops_;

        // a compound kind's operator method (compound_and -> operator&)
        uint64_t compound_kind_symbol(uint32_t kind) const noexcept {
            switch (kind & compound_kind_mask) {
                case compound_minus: return ops_.minus;
                case compound_star: return ops_.star;
                case compound_slash: return ops_.slash;
                case compound_percent: return ops_.percent;
                case compound_and: return ops_.ampersand;
                case compound_or: return ops_.pipe;
                case compound_xor: return ops_.caret;
                case compound_shl: return ops_.left_shift;
                case compound_shr: return ops_.right_shift;
                default: return ops_.plus;
            }
        }
        // a op= b, or a desugared ++/-- (the compound's kind and flags), on an object through its class's in-place operator
        // (detail::object_operator_in_place); nullopt for any other target
        std::optional<checked_result<script_value>> compound_in_place(const script_value& target, uint32_t kind, const script_value& right) {
            if (target.deref().raw_storage_index() != script_value::TYPEID_OBJECT) [[likely]] { return std::nullopt; }
            const detail::incdec_form form = !(kind & compound_flag_incdec) ? detail::incdec_form::none
                                             : (kind & compound_flag_old_result) ? detail::incdec_form::postfix : detail::incdec_form::prefix;
            return object_operator_in_place(target.deref(), compound_kind_symbol(kind), right, form);
        }
        uint64_t this_id_ = 0;
        uint64_t subscript_op_id_ = 0;
        uint64_t assign_operator_id_ = 0;
        uint64_t to_string_id_ = 0;
        uint64_t to_bool_id_ = 0;
        uint64_t eq_method_id_ = 0;
        uint64_t same_as_id_ = 0;
        uint64_t builtin_push_id_ = 0;   // "push" (the array-push inline fast path)
        uint64_t builtin_has_id_ = 0;    // "has" (the map-has inline)
        uint64_t coroutine_handle_type_id_ = 0;
        uint64_t next_id_ = 0;
        uint64_t handle_reset_id_ = 0;
        uint64_t done_id_ = 0;
        uint64_t advance_id_ = 0;
        uint64_t result_id_ = 0;
        uint64_t class_definition_type_id_ = 0;
        uint64_t hot_reload_migrate_id_ = 0;
        base_type_symbols base_type_ids_;   // scalar_member_message spellings, interned at init
        detail::arity_count_symbols arity_count_ids_;   // arity text count spellings, interned at init

        builtin_method_registries builtins_;

        struct expr_chunk_cache_entry {
            std::shared_ptr<chunk> compiled;
            expression_ptr pin;
        };
        std::unordered_map<node_unit_key, expr_chunk_cache_entry, node_unit_hash> expr_chunk_cache_;
        std::unordered_map<const expression*, expr_chunk_cache_entry> parallel_expr_chunks_;   // a worker's precompiled construction expressions
        struct decl_chunk_cache_entry {   // a namespace's variable declaration as a one-declaration program
            std::shared_ptr<chunk> compiled;
            declaration_ptr pin;
        };
        std::unordered_map<node_unit_key, decl_chunk_cache_entry, node_unit_hash> decl_chunk_cache_;

        void arm_execution_deadline();
        bool execution_budget_exhausted();
        // Combined limit check at loop back-edges / call entry (budget tick + memory
        // high-water) and its cold raise twin. KEEP BYTE-PARALLEL with the interpreter.
        JAI_FORCEINLINE bool execution_limit_exhausted() {
            if (budget_tick_ < 1023 && !limits_->memory_tripped()) [[likely]] { ++budget_tick_; return false; }
            return execution_limit_exhausted_slow();
        }
        bool execution_limit_exhausted_slow();
        error_propagator execution_limit_failure();
        // Statement-boundary edge detection + hook fire; out of line so run_dispatch's
        // Debug frame stays flat (JAI_MAX_CALL_DEPTH path). Called only when armed.
        void debug_statement_boundary(frame& f);
        void debug_stop_at(const ast_node* node);   // a statement the backend runs outside any chunk (a namespace's class)

        // the compiled body of a function object, cached per (body, unit): the unit is the function's own
        std::shared_ptr<chunk> chunk_for_body(std::string_view name,
                                              const std::vector<parameter>& params,
                                              const std::shared_ptr<block_stmt>& body,
                                              size_t local_count, unit_stamp unit);

        checked_result<void> run(frame& entry);
        op_status run_dispatch(frame*& fp, const size_t records_base);
        template <bool Hooked> op_status run_dispatch_loop(frame*& fp, const size_t records_base);   // Hooked: a debugger session's loop
        // In-loop call machinery: push/pop of call_records_ without native recursion.
        // ArgsT is value_stack (zero-copy slice [args_base, args_base+argc) with the
        // callee slot at args_base-1 — args IS stack_, which conversions can relocate
        // mid-bind, so the container travels, never a raw pointer) or a pooled
        // std::vector<script_value> passed as (vec, 0, vec.size()).
        bool args_are_stack(const value_stack& a) const noexcept { return &a == &stack_; }
        static constexpr bool args_are_stack(const std::vector<script_value>&) noexcept { return false; }
        // An argument list a call owns (arg_vector_return, enter_constructor_in_loop's captured list): its prvalues bind as the stack's do
        std::vector<script_value>* owned_args_ = nullptr;
        std::vector<script_value>* owned_args(const value_stack&) const noexcept { return nullptr; }
        std::vector<script_value>* owned_args(const std::vector<script_value>& a) const noexcept { return &a == owned_args_ ? owned_args_ : nullptr; }
        template <class ArgsT>
        op_status push_script_frame(frame& caller, script_value&& callee,
                                               const script_defined_function& function,
                                               const ArgsT& args,
                                               size_t args_base, size_t argc,
                                               const call_site* site);
        // the frame-push kernels every push shares (vm_backend.cpp, beside push_plain_frame)
        op_status pass_call_limits();
        call_record& take_call_record();
        void init_call_record(call_record& rec, frame& caller, const type_info_ptr& return_type, uint8_t return_conv, std::string_view name, const chunk& body);
        void open_record_frame(call_record& rec, chunk* body, size_t window_base, size_t window_live, size_t stack_base, unit_stamp unit,
                               const script_defined_function* function);
        template <class ArgsT>
        op_status bind_frame_parameters(call_record& rec, const std::vector<parameter>& parameters, const ArgsT& args, size_t args_base, size_t argc,
                                        frame& caller, const call_site* site, const script_defined_function* function, const statement* body);
        void finish_frame_window(call_record& rec, size_t local_count);
        void abandon_call_record(call_record& rec);
        template <class ArgsT>
        op_status open_script_frame(frame& caller, const script_defined_function& function, const ArgsT& args, size_t args_base, size_t argc,
                                    const call_site* site, script_value* callee, strong_ptr<script_function_node>* pin, size_t stack_base);
        template <class DispatchT>
        chunk* method_body_chunk(const DispatchT& dispatch, const function_decl& ast, const class_definition* member_class);
        call_record& open_method_record(frame& caller, const script_method_dispatch& dispatch, const std::shared_ptr<function_decl>& ast, const chunk& body,
                                        bool anchor);
        template <class TakeReceiver>
        void enter_method_env(call_record& rec, const script_method_dispatch& dispatch, const chunk& body, TakeReceiver&& take_receiver);
        // Method-call flattening: a script-class instance method enters the dispatch loop
        // directly (no bound-method mint, no arg re-copies, no native run() recursion)
        op_status enter_script_method(frame& caller, script_value&& method_val,
                                                 const script_method_dispatch& dispatch,
                                                 const std::shared_ptr<function_decl>& ast,
                                                 script_value&& receiver,
                                                 const std::vector<script_value>& arguments,
                                                 const call_site& site);
        // Call-arg window twins: the receiver sits at stack_[args_base-1]
        // and the args at [args_base, args_base+argc) — they bind IN PLACE as frame
        // slots (zero per-arg moves; the receiver slot is the pin, destroyed by pop
        // truncation exactly like the plain path's callee slot). The dispatcher still
        // pins through the record; env setup COPIES the receiver (the slot must live).
        // receiver_owned: null = receiver in the stack slot at args_base-1 (the slot is
        // the pin); non-null = thunk-payload receiver (moved into the env binding).
        // frame_base = first stack index of this frame's territory (pop truncation).
        op_status enter_script_method_sliced(frame& caller, const script_value& method_val,
                                             const script_method_dispatch& dispatch,
                                             const std::shared_ptr<function_decl>& ast,
                                             script_value* receiver_owned, size_t frame_base,
                                             size_t args_base, size_t argc,
                                             const call_site& site);
        op_status push_method_frame_sliced(frame& caller, const script_value& method_val,
                                           const script_method_dispatch& dispatch,
                                           const std::shared_ptr<function_decl>& ast,
                                           script_value* receiver_owned, size_t frame_base,
                                           size_t args_base, size_t argc,
                                           const call_site* site);
        op_status push_method_frame(frame& caller, const script_value& method_val,
                                               const script_method_dispatch& dispatch,
                                               const std::shared_ptr<function_decl>& ast,
                                               script_value&& receiver,
                                               const std::vector<script_value>& arguments,
                                               const call_site* site);
        // a script class's static method entered in-loop (the native path's static scope env, no receiver): true when callee is one
        // this engine's loop may enter (st holds the push's outcome), false for the opaque call
        bool enter_static_method(frame& caller, script_value& callee, const std::vector<script_value>& arguments, const call_site& site, op_status& st);
        op_status push_static_method_frame(frame& caller, const script_value& method_val, const script_static_method_dispatch& dispatch,
                                           const std::shared_ptr<function_decl>& ast, const std::vector<script_value>& arguments, const call_site* site);
        void anchor_method_result(script_value& result, script_value& receiver);
        void pin_method_decl(call_record& rec, const std::shared_ptr<function_decl>& ast);
        void pop_script_frame_core(call_record& rec);
        // A sharing frame's cells (chunk::shares_cells), gathered before its locals die and checked after
        void gather_frame_shared_cells(const frame& f);
        void pop_script_frame_boxed_cells(call_record& rec);
        std::vector<std::vector<script_value>> cell_lists_;   // reused gathered-cell lists (a nested frame exit takes its own)
        void note_boxed_slot(const script_value& storage);
        void kill_shared_cell_slot(script_value& slot);
        void kill_frame_slot(frame& f, uint32_t slot);
        void take_temp_value(script_value& slot);   // op_load's take bit: the read value moves off the temp
        void kill_frame_slots(frame& f, uint32_t top, uint32_t below);   // op_kill_slot's run: top first, then the `below` slots under it
        JAI_NOINLINE void release_read_temps(frame& f, const fused_binary_proto& p);
        JAI_NOINLINE void release_index_temps(frame& f, const fused_index_proto& p);
        // A native entry frame's cells (shared by a lambda, or a slot boxed on demand), gathered before its window dies
        void gather_native_frame_cells(const frame& f, call_frame& locals, std::vector<script_value>& cells);
        void release_frame_shared_cells();
        std::vector<script_value> shared_cells_;
        std::vector<size_t> shared_cell_bases_;
        op_status return_from_script_frame(frame*& fp, const vm_instruction& ins);
        // Shared return epilogue (classification switch + pop + result push); the
        // return superinstructions feed it a result they resolved without the stack
        // round trip. KEEP the exact order: convert while the callee frame is live.
        // mode = op_return.b: 0 plain; return_mode_converted = already converted at an
        // in-region return site (op_return_convert), landed as is
        op_status return_with_result(frame*& fp, script_value result, uint32_t mode = 0);
        op_status return_with_conversion(frame*& fp, script_value result, uint32_t mode);
        // Fused `return <ident>;` / `return <a op b>;`: record-frame and
        // native-entry variants (the entry twins set return_value_ like op_return's
        // entry branch)
        template <bool Entry> op_status exec_return_ident(frame*& fp, const vm_instruction& ins);   // Entry: the entry frame's return door
        static fused_operand ident_operand_at(const frame& f, const vm_instruction& ins) noexcept;
        template <bool Entry> op_status exec_return_binary(frame*& fp, const vm_instruction& ins);
        op_status fall_off_script_frame(frame*& fp);
        // Return-site conversion (op_return_convert inside try regions) and the
        // native-entry / fiber completion door; both run convert_return_at_site
        op_status exec_return_convert(frame& f, const vm_instruction& ins);
        // A yielded value takes the coroutine's declared type (the return's table, the yield's text)
        op_status exec_yield_convert(frame& f);
        op_status entry_return_finish(frame& f, script_value result, uint32_t mode);
        // By-value return materialization (detail/return_conv.hpp), run by every return
        // door BEFORE the typed conversion: the callee's own slot moves out, other lvalue
        // reads and reference results copy, temporaries move. operand_class 0 = nothing
        // to do (ref return, no value, or an in-region site already ran it); the
        // top-level program's `return` is not a function return and is untouched.
        // KEEP BYTE-PARALLEL with the interpreter's visit_return_stmt. Out-of-line + noinline:
        // inlined into run_dispatch's op_return case (LTCG) the clone machinery fattens the
        // whole loop's frame, paid on every loop entry (coroutine resumes); each door tests
        // return_value_plain first so a plain result never calls.
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        void copy_reference_result(script_value& result);   // a reference result copies its target (cold)
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        void materialize_return(frame& f, script_value& result, uint32_t operand_class, uint32_t own_slot);
        void convert_cpp_exception_in_frame(const script_exception& e);
        void pop_records_to(size_t records_base, frame*& fp);
        script_value run_program(std::shared_ptr<chunk> program, bool textual = false);   // textual: an include, run in the current environment
        // A deep recursion leaves the record pool, the value stack and the frame list at their high-water size; the end of a
        // top-level run gives back what a shallow program never needs
        void release_deep_call_storage();
        // The call path's one compare is against the depth gate: past it the cap raises, or the frames' storage is charged to
        // the memory allowance (its text when over) and the gate moves k_frame_charge_levels deeper
        std::optional<error_propagator> pass_depth_gate();
        // A method body entered through the opaque door: the frame's method scope comes from these, not a carrier env
        struct method_entry {
            const std::shared_ptr<environment>* parent = nullptr;
            const script_value* this_obj = nullptr;
            class_definition* access = nullptr;
        };
        checked_result<script_value> call_script_function(const script_defined_function& function,
                                                          const std::vector<script_value>& args,
                                                          const call_site* site = nullptr,
                                                          frame* caller_frame = nullptr,
                                                          chunk* caller_code = nullptr,
                                                          const method_entry* method = nullptr,
                                                          size_t first = 0);   // the arguments are args[first..]
        // Shared between call_script_function and the in-loop call path (single source of truth)
        void setup_callee_env(const script_defined_function& function, call_frame& locals,
                              const std::shared_ptr<environment>& prev_env, const method_entry* method = nullptr);
        // The opaque door's method body as a function value, one per declaration (its body chunk cached on it)
        struct method_function_entry { std::shared_ptr<function_decl> pin; std::unique_ptr<script_defined_function> fn; };
        std::unordered_map<const function_decl*, method_function_entry> method_functions_;
        const script_defined_function& method_function_for(const std::shared_ptr<function_decl>& ast);
        template <class ArgsT>
        op_status bind_parameters(const std::vector<parameter>& parameters,
                                             const ArgsT& args,
                                             size_t args_base, size_t argc,
                                             frame& callee, chunk& body_chunk,
                                             const std::shared_ptr<environment>& caller_env,
                                             const call_site* site, frame* caller_frame,
                                             chunk* caller_code,
                                             const script_defined_function* alias_fn,
                                             unit_stamp caller_unit,   // a by-reference argument resolves under the caller's unit
                                             size_t first_param = 0, bool later_taken = false,   // a resumed bind (the arguments after a user-code conversion taken)
                                             const statement* callee_body = nullptr);   // a method's body (a function's is alias_fn's): a temporary may bind a reference parameter it never writes through
        // Ref-param bind against the caller's variable storage: shares an existing
        // reference holder (cells alias), or boxes the value on demand (cell) when the
        // variable predates its escape mark
        op_status reference_to_storage(script_value& storage, script_value& out, bool& bound);
        // `v.x` where a reference binds: a COMPONENT reference (detail/math_component_store.hpp), else a non-reference
        checked_result<script_value> component_reference(frame* caller_frame, const expression* e, const std::shared_ptr<environment>& caller_env);
        op_status bind_place_reference(const expression* place, frame* caller_frame, const std::shared_ptr<environment>& caller_env,
                                       script_value& out, bool& bound);
        op_status bind_later_places(const std::vector<parameter>& parameters, std::vector<script_value>* held, size_t args_base, size_t from, size_t argc, const call_site* site,
                                    frame* caller_frame, chunk* caller_code, const std::shared_ptr<environment>& caller_env, unit_stamp caller_unit);
        op_status bind_site_reference(size_t i, const call_site& site, frame* caller_frame, chunk* caller_code,
                                      const std::shared_ptr<environment>& caller_env, script_value& out, bool& bound,
                                      host_slot* slot_out = nullptr);
        // Kernel caller-view over a frame: window frames hand the resolver their raw
        // window span (lawful: resolve_ref_lvalue never runs script, so the stack
        // cannot grow mid-resolve); frames without a window keep the call_frame read.
        // Defined in vm_backend.cpp (ref_lvalue.hpp stays out of this header).
        detail::caller_frame_view caller_view(frame* f) const;
        // Box-in-place + share for env-variable storage (decl refs, [&] captures, ref
        // returns); boxing demotes active counted-for fast states
        script_value share_env_ref(script_value& storage);
        void clear_this_on_frame_exit(const std::shared_ptr<environment>& entry_env);
        script_value implicit_this_result(call_frame& locals);
        script_value implicit_result_for_record(call_record& rec);
        checked_result<script_value> convert_return_value(script_value result, const type_info_ptr& return_type,
                                                          std::string_view callee = {});
        checked_result<script_value> convert_return_at_site(script_value result, const type_info_ptr& return_type,
                                                            return_conv conv, std::string_view callee = {});

        void capture_stack_trace();
        size_t trace_grafts_through(size_t cut);
        static size_t graft_frames_at(const chunk& code, size_t ip, const ast_node** node, std::vector<stack_frame>& out);
        static const ast_node* frame_statement(const frame& fr);
        static std::string frame_function_name(const frame& fr);
        std::vector<stack_frame> debug_frame_walk(std::vector<const frame*>* slot_frames) const;
        std::vector<std::pair<std::string, script_value>> frame_slot_locals(const frame* f) const;
        void absorb_nested_trace();
        op_status raise_nested_failure(const std::exception& e);
        op_status raise_nested_failure_statement(const std::exception& e);

        script_value make_null() const;
        bool is_truthy(const script_value& value);
        bool object_to_bool_via_method(const script_value& value);
        checked_result<std::string> value_to_string_with_method(const script_value& val);
        checked_result<detail::own_text_verdict> object_own_text(const script_value& val, std::string& out);
        checked_result<script_string> concat_text(const script_value& left, const script_value& right);
        script_value materialize_constant(const script_value& tmpl);
        script_value materialize_constant(const chunk& code, uint32_t const_index);
        // A string literal's value: the main engine shares the AST node (string copies share
        // storage everywhere else; a write unshares); a parallel worker never touches that
        // node's count (not atomic; the main engine and the other workers hold the same
        // literal, the XTHREAD canary pins this): it shares its own per-chunk copy when the
        // chunk is pinned in its tables, else it copies the bytes
        script_value literal_string_value(const strong_ptr<script_string>& node) const {
            return parallel_worker_ ? script_value(*node, engine_) : script_value(node, engine_);
        }
        script_value literal_string_value(const chunk& code, uint32_t const_index, const strong_ptr<script_string>& node) {
            if (!parallel_worker_) { return script_value(node, engine_); }
            if (worker_chunk_env_cache* rec = worker_chunk_record(&code)) {
                if (rec->string_constants.size() <= const_index) [[unlikely]] {
                    rec->string_constants.resize(code.constants.size(), script_value(std::monostate{}, engine_));
                }
                script_value& own = rec->string_constants[const_index];
                if (own.raw_storage_index() != script_value::TYPEID_STRING) [[unlikely]] {
                    own = script_value(*node, engine_);
                }
                return own;
            }
            return script_value(*node, engine_);
        }
        // the CONST instruction's push: literals are engine-less parse-time templates, re-materialized with the engine
        JAI_FORCEINLINE void push_constant(const chunk& code, uint32_t index) {
            const script_value& tmpl = code.constants[index];
            const auto& storage = tmpl.get_storage();
            switch (storage.index()) {
            case script_value::TYPEID_INT: stack_.push_back(script_value(storage.get<script_int>(), engine_)); break;
            case script_value::TYPEID_FLOAT: stack_.push_back(script_value(storage.get<script_float>(), engine_)); break;
            case script_value::TYPEID_STRING: stack_.push_back(literal_string_value(code, index, storage.get<strong_ptr<script_string>>())); break;
            case script_value::TYPEID_CHAR: stack_.push_back(script_value(storage.get<script_char>(), engine_)); break;
            case script_value::TYPEID_BOOL: stack_.push_back(script_value(storage.get<script_bool>(), engine_)); break;
            case script_value::TYPEID_NULL: stack_.push_back(make_null()); break;
            default: {
                script_value copy = tmpl;
                stack_.push_back(std::move(copy));
                break;
            }
            }
        }
        bool object_defines_custom_equality(const script_value& v) const;
        // user operator methods: nullopt when the operand defines none, else the method's result or the runtime error it raised
        std::optional<checked_result<script_value>> object_compound_through_binary(const script_value& target, const script_value& right, uint64_t op_symbol_id);
        std::optional<checked_result<script_value>> object_operator_in_place(const script_value& target, uint64_t base_id, const script_value& right, detail::incdec_form form);
        std::optional<checked_result<script_value>> object_compound_operand(const script_value& left, token_type op, const script_value& right);
        std::optional<checked_result<script_value>> object_unary_via_method(const script_value& operand, token_type op);   // a class's unary - + ~
        script_value call_operator_of(const script_value& callee);   // the instance's operator() bound to it, else null
        op_status raise_method_value(const member_expr* expr, const script_value& object, bool bindable);   // obj.method without a call
        op_status raise_bare_method_value(uint64_t name_id, const script_value& self);   // a bare method name without a call
        std::optional<op_status> bare_method_reference(frame& f, uint64_t name_id);   // a reference to a bare method name

        struct member_target {
            strong_ptr<class_instance> instance;
            std::shared_ptr<class_definition> engine_def;
            class_definition* class_def = nullptr;
            explicit operator bool() const { return instance != nullptr || class_def != nullptr; }
            script_value method(uint64_t id) const;
            script_value user_accessor(uint64_t id) const;   // nearest user _get_/_set_ (synthesized skipped); invalid when absent
            bool has_field(uint64_t id) const;
            const script_value& get_field(uint64_t id) const;
            std::string_view class_name() const;
        };
        member_target resolve_member_target(const script_value& objectValue) const;
        const detail::parallel_method_pin* worker_operator_pin(const script_value& v, uint64_t id) const;
        bool worker_operators_pinned(const script_value& v, std::initializer_list<uint64_t> ids) const;
        std::string_view worker_operator_refusal(const script_value& v, std::initializer_list<uint64_t> ids) const;
        script_value operator_value(const member_target& target, uint64_t id) const;
        checked_result<script_value> operator_call(const script_value& self, const script_value& method_val, uint64_t id, std::vector<script_value> args);
    public:
        std::optional<checked_result<script_value>> parallel_operator_call(const script_value& self, uint64_t id, std::vector<script_value> args) override;
    private:

        // Scope-environment free list for op_scope_push/pop (mirrors the interpreter's
        // environment pool). release only recycles unshared scopes, so anything captured
        // (closure env, coroutine snapshot, reference source) keeps its identity.
        std::vector<std::shared_ptr<environment>> scope_env_pool_;
        std::vector<std::unique_ptr<script_value>> this_box_pool_;   // call_script_function frames' `this` boxes, reused
        std::shared_ptr<environment> acquire_scope_env(std::shared_ptr<environment> parent);
        std::shared_ptr<environment> acquire_block_env(std::shared_ptr<environment> parent);   // a { } / loop / catch scope
        std::shared_ptr<environment> acquire_method_scope_env(std::shared_ptr<environment> parent, script_value this_obj, class_definition* access_ctx = nullptr);
        std::shared_ptr<environment> acquire_static_scope_env(std::shared_ptr<environment> parent, std::shared_ptr<class_definition> class_def);
        void release_scope_env(std::shared_ptr<environment> env);
        void pop_scopes_pooled(uint32_t count);

        // Argument-vector free list for script calls (mirrors the interpreter's argument_pool_)
        std::vector<std::vector<script_value>> arg_vector_pool_;
        std::vector<script_value> acquire_arg_vector(size_t reserve);
        void release_arg_vector(std::vector<script_value> vec);
        // An argument list the call owns while the scope lives (null: the owner stays as it was)
        struct owned_args_scope {
            vm_backend* vm;
            std::vector<script_value>* saved = nullptr;
            bool owns;
            owned_args_scope(vm_backend* a_vm, std::vector<script_value>* a_args) : vm(a_vm), owns(a_args != nullptr) {
                if (owns) { saved = a_vm->owned_args_; a_vm->owned_args_ = a_args; }
            }
            ~owned_args_scope() { if (owns) { vm->owned_args_ = saved; } }
        };
        // The arguments a call moved off the stack, owned for the call (a prvalue among them is its parameter, as on the stack), back
        // to the pool on every exit path
        struct arg_vector_return {
            vm_backend* vm;
            std::vector<script_value>* vec;
            owned_args_scope owned;
            arg_vector_return(vm_backend* a_vm, std::vector<script_value>* a_vec, size_t a_argc) : vm(a_vm), vec(a_vec), owned(a_vm, a_argc ? a_vec : nullptr) {}
            ~arg_vector_return() { vm->release_arg_vector(std::move(*vec)); }
        };

        // Bodies live outside run() to keep its Debug frame flat (JAI_MAX_CALL_DEPTH path)
        void exec_array(frame& f, const vm_instruction& ins);
        void exec_map(frame& f, const vm_instruction& ins);

        script_value* resolve_local_or_env(frame& f, uint32_t slot, uint64_t symbol_id, size_t cache_slot = SIZE_MAX);
        script_value* variable_cell(frame& f, uint32_t slot, uint64_t symbol_id, size_t cache_slot);
        // The cell an identifier RECEIVER of a string builtin mutates: exec_load's ladder as
        // a pointer (:: door, slot, member scope, env). KEEP BYTE-PARALLEL with
        // interpreter::string_receiver_cell.
        script_value* string_receiver_cell(frame& f, const call_site& site, const identifier_expr* ident);
        // The pin a string subscript store target writes through (detail/string_element.hpp);
        // null for a temporary base. KEEP BYTE-PARALLEL with interpreter::string_element_pin.
        script_value string_element_pin(frame& f, script_value& base_raw, uint32_t site_index);
        // Env-path lookup memoized per instruction (cache_slot = 2*ip [+1 for fused right]).
        // Returns the same pointer environment::get_ref/get_value_ptr's storage prefix
        // would; nullptr = caller must run its original full lookup (fallback tails).
        script_value* env_lookup_cached(frame& f, size_t cache_slot, uint64_t symbol_id);
        // Transparency-walk bail tail: chain-agnostic storage-prefix walk from the
        // CURRENT env (real shadowing), arming the entry's fast head for env-local hits
        script_value* env_arm_fast_head(env_lookup_cache_entry* fast_entry, uint64_t symbol_id);
        // Worker-private cache row for this chunk+slot (nullptr = chunk never pinned
        // here - caller falls through to the full walk). MRU compare first.
        env_lookup_cache_entry* worker_env_cache_slot(const chunk* code, size_t cache_slot);
        // the lookup-cache entry the general resolution of cache_slot arms for this frame: the chunk's on the main vm, the worker's own row in a
        // worker whose frame found its table (null: none to read)
        const env_lookup_cache_entry* armed_env_entry(const frame& f, size_t cache_slot) const noexcept {
            if (cached_global_env_) return cache_slot < f.code->env_lookup_cache.size() ? &f.code->env_lookup_cache[cache_slot] : nullptr;
            return f.env_table_code == f.code && cache_slot < f.code->code.size() * 3 ? &f.env_table[cache_slot] : nullptr;
        }
        // find-or-create the worker cache record for a chunk (called where the owning
        // shared_ptr is in hand; the pin makes the raw-pointer key sound forever)
        void worker_pin_env_cache(const std::shared_ptr<void>& pin, const chunk* code);
        // box_cell: escape-marked decls wrap the value into a cell (see reference_holder)
        op_status define_decl_value(frame& f, uint64_t name_id, size_t slot_index, script_value value, bool box_cell = false, const variable_decl* decl = nullptr);
        op_status define_decl_value(frame& f, const variable_decl* decl, script_value value, bool box_cell = false) {
            return define_decl_value(f, decl->name_id, decl->slot_index, std::move(value), box_cell, decl);
        }
        // `int& r = e`: the reference carries its declared type (detail::ref_declare)
        // `int& r = e`: a reference whose value reaches another binding (or that no frame slot holds) carries its declared
        // type, marked once it landed (the compiler's c = 1); a slot-resident one is decided at its stores from the chunk
        // (chunk::slot_declared_kinds)
        op_status declared_ref_landed(frame& f, const vm_instruction& ins, const variable_decl* decl, op_status landed) {
            if ((ins.c & 1) && landed == op_status{}) [[unlikely]] { declare_stored_reference(f, decl); }
            return landed;
        }
        op_status define_ref_decl_value(frame& f, const vm_instruction& ins, const variable_decl* decl, script_value value);
        void declare_stored_reference(frame& f, const variable_decl* decl);
        uint8_t slot_declared_kind(const frame& f, uint32_t slot) const noexcept {
            const auto& k = f.code->slot_declared_kinds;
            return slot < k.size() ? k[slot] : uint8_t{0};
        }
        type_info* slot_declared_ref(const frame& f, uint32_t slot) const noexcept {
            const auto& t = f.code->slot_declared_refs;
            return slot < t.size() ? t[slot] : nullptr;
        }

        checked_result<script_value> enforce_type_compatibility(script_value value, type_info_ptr target_type,
                                                                std::string_view var_name = {},
                                                                detail::slot_kind kind = detail::slot_kind::assignment,
                                                                std::string_view owner = {});
        // Compound-assignment store-back (x op= rhs ≡ x = T(x op rhs)); interpreter twin:
        // interpreter::compound_typed_store_back
        op_status compound_typed_store_back(script_value& target, script_value promoted, std::string_view var_name = {});
        op_status compound_typed_store_back(script_value& target, script_value promoted, uint64_t name_sym);
        checked_result<script_value> converted_for_variable(const type_info_ptr& tag, script_value value, uint64_t name_sym);
        template <class NameOf> op_status compound_typed_store_back_named(script_value& target, script_value promoted, NameOf&& name_of);
        checked_result<script_value> enforce_type_compatibility(script_value value, type_info_ptr target_type, uint64_t name_sym);
        template <class NameOf> checked_result<script_value> enforce_type_compatibility_named(script_value value, type_info_ptr target_type, NameOf&& name_of,
                                                                                              detail::slot_kind kind, std::string_view owner);
        checked_result<script_value> try_convert_for_parameter(const script_value& arg, type_info_ptr target_type,
                                                               const detail::slot_site& site = detail::slot_site{},
                                                               script_value* consumable = nullptr);
        checked_result<script_value> evaluate_arithmetic(const script_value& left_in, token_type op, const script_value& right_in, int compound = -1);

        checked_result<script_value> binary_general(token_type op, const script_value& left, const script_value& right);
        struct binary_host;   // this backend's hooks for detail/binary_handlers.hpp
        checked_result<script_value> handle_binary_op(token_type op, const script_value& left, const script_value& right);
        checked_result<script_value> handle_add(const script_value& left, const script_value& right);
        checked_result<script_value> join_text(const script_value& left, const script_value& right);
        checked_result<script_value> handle_subtract(const script_value& left, const script_value& right);
        checked_result<script_value> handle_multiply(const script_value& left, const script_value& right);
        checked_result<script_value> handle_divide(const script_value& left, const script_value& right);
        checked_result<script_value> handle_modulo(const script_value& left, const script_value& right);
        checked_result<script_value> handle_less(const script_value& left, const script_value& right);
        checked_result<script_value> handle_less_equal(const script_value& left, const script_value& right);
        checked_result<script_value> handle_greater(const script_value& left, const script_value& right);
        checked_result<script_value> handle_greater_equal(const script_value& left, const script_value& right);
        checked_result<script_value> handle_equal(const script_value& left, const script_value& right);
        checked_result<script_value> handle_not_equal(const script_value& left, const script_value& right);
        checked_result<script_value> handle_spaceship(const script_value& left, const script_value& right);

        builtin_method_context builtin_ctx() const {
            return {engine_, symbolizer_,
                [](void* self, const script_value& a, const script_value& b) {
                    engine::run_pin pin(static_cast<vm_backend*>(self)->engine_);   // a builtin's comparison is its callback
                    return static_cast<vm_backend*>(self)->language_equal(a, b);
                },
                [](void* self) { return static_cast<vm_backend*>(self)->is_unwinding_; }, const_cast<vm_backend*>(this)};
        }
        checked_result<bool> language_equal(const script_value& a, const script_value& b) override;
        checked_result<script_value> invoke_method(const script_value& self, uint64_t name_id) override;   // the language's == (a global operator==, the value's own)

        void raise_script_exception(const std::string& message, const source_location& location);
        // the member paths' composed texts, built out of line (no string temporaries in the handlers)
        void raise_no_member(std::string_view kind, std::string_view member, const source_location& location);
        // a dot read of a plain map's key (only an enum's value table reads its keys with a dot): KEEP BYTE-PARALLEL with the interpreter
        JAI_NOINLINE op_status raise_map_dot_key(uint64_t member_id) {
            return raise_(make_error_code(runtime_error_code::member_not_found), "map has no member '{0}'; read the key as [\"{1}\"]", member_id, member_id);
        }

        // Runs a standalone expression chunk (node-keyed cache) in the given environment
        checked_result<script_value> eval_expression(const expression_ptr& expr, const std::shared_ptr<environment>& env);

        checked_result<script_value> execute_method_ast(const std::shared_ptr<function_decl>& ast,
                                                        std::shared_ptr<environment> method_env,
                                                        const std::vector<script_value>& args);

        checked_result<script_value> construct_instance(std::shared_ptr<script_class_definition> class_def,
                                                        std::shared_ptr<environment> definition_env,
                                                        const std::vector<script_value>& args, bool check_access = true);
        // detail::prepare_construction under this backend: success value = the new instance ('this'); captured_out = the arguments
        // with the T& parameters' places bound (empty: none); caller = the frame whose class a private constructor's access is
        // checked against (null: the current context); site = the call's site, where a T& parameter binds its place
        checked_result<script_value> construct_instance_pre_body(const std::shared_ptr<script_class_definition>& class_def,
                                                        const std::vector<script_value>& args,
                                                        std::shared_ptr<function_decl>& matching_ctor_out,
                                                        std::vector<script_value>& captured_out,
                                                        const frame* caller, bool check_access, const call_site* site = nullptr);
        checked_result<script_value> run_constructor_natively(const std::shared_ptr<script_class_definition>& class_def, const std::shared_ptr<function_decl>& ctor,
                                                              const std::shared_ptr<environment>& definition_env, const script_value& this_value,
                                                              const std::vector<script_value>& args);
        // What the vm lends the construction kernel (detail/construction.hpp)
        struct construction_lender;
        // Construction protocol native, ctor BODY on the dispatch loop (op_new + direct
        // ctor calls); result_stamp = the shared_ptr arm's post-call type stamp
        op_status enter_constructor_in_loop(frame& f,
                                            const std::shared_ptr<script_class_definition>& class_def,
                                            const std::shared_ptr<environment>& definition_env,
                                            const std::vector<script_value>& args,
                                            type_info* result_stamp, const call_site* site);
        checked_result<script_value> construct_default_instance(const std::shared_ptr<script_class_definition>& class_def,
                                                                const std::vector<script_value>& args);
        // The script class a constructor thunk builds, cast once per call site; null for anything else
        const std::shared_ptr<script_class_definition>* site_script_class(const call_site* site, const script_callable_thunk* thunk);
        // op_new's class name resolved now: the cell, and the constructor's thunk read once per value at the site
        script_value* new_class_cell(frame& f, const vm_instruction& ins, const type_info& cls);
        const script_callable_thunk* new_constructor_thunk(const call_site& site, const script_value& ctor);
        checked_result<void> evaluate_field_initializers(strong_ptr<class_instance> instance,
                                         std::shared_ptr<script_class_definition> class_def,
                                         std::shared_ptr<environment> init_env,
                                         bool skip_parent_recursion = false,
                                         const detail::aggregate_plan* aggregate = nullptr);
        void apply_field_initializers(const strong_ptr<class_instance>& instance,
                                      const std::shared_ptr<class_definition>& class_def) override;
        // A script class whose field initializers are all constants (literals, and array and map literals of them): the values
        // its first instance stored, written into every later one with no initializer environment or evaluation. Keyed by the
        // class's method and field-shape epochs (a reload re-plans).
        struct field_init_plan {
            uint64_t epoch = 0;
            uint64_t shape = 0;
            bool checked = false;
            bool armed = false;
            std::vector<std::pair<uint64_t, script_value>> values;
        };
        std::unordered_map<const class_definition*, field_init_plan> field_init_plans_;
        // A class level's field initializers as one method body over its initializer environment (an aggregate's, a reload's), keyed by
        // the class, rebuilt when its initializers or the unit change
        struct level_initializer_body {
            uint64_t unit = UINT64_MAX;
            std::vector<const expression*> asts;
            std::unique_ptr<script_defined_function> fn;
        };
        // The construction machinery's state, boxed: the backend's own layout stays as it was for code that never constructs
        struct construction_state {
            std::unordered_map<const expression*, level_initializer_body> single_initializers;   // run_level_initializers' `only` bodies
            std::unordered_map<const class_definition*, level_initializer_body> level_initializers;
            std::exception_ptr field_init_exception;   // what a field store in a level's initializer body threw, rethrown natively
        };
        std::unique_ptr<construction_state> construction_;
        construction_state& construction() {
            if (!construction_) [[unlikely]] { construction_ = std::make_unique<construction_state>(); }
            return *construction_;
        }
        // only: that one initializer (an aggregate's field past its last initializer), else every one of the level
        checked_result<void> run_level_initializers(class_instance& instance, script_class_definition& class_def,
                                                    const std::shared_ptr<environment>& init_env,
                                                    const std::pair<uint64_t, expression_ptr>* only = nullptr);
        // A fresh layer's whole starting field state for a class whose initializers are all constants (the same keys)
        struct fresh_layer_plan {
            uint64_t epoch = 0;
            uint64_t shape = 0;
            bool checked = false;
            bool armed = false;
            std::vector<std::pair<uint32_t, script_value>> values;   // by slot
        };
        std::unordered_map<const class_definition*, fresh_layer_plan> fresh_layer_plans_;
        bool seed_fresh_layer_fields(const strong_ptr<class_instance>& instance, const std::shared_ptr<class_definition>& class_def) override;
        void note_fresh_layer_fields(const strong_ptr<class_instance>& instance, const std::shared_ptr<class_definition>& class_def) override;

        // Mirrors visit_member_expr's non-static path; unwinding cases leave out null
        // Lexical access context for member enforcement: an env-untouched frame
        // (f.entry_env == nullptr) is a plain non-closure function — lexically
        // top-level, NO class context; its environment_ is the CALLER's chain,
        // whose context must not leak into it. Lazy METHOD frames answer with the
        // receiver's class (their lazy gate excludes nonpublic chains, where the
        // declaring-vs-dynamic class distinction could matter).
        const class_definition* frame_access_context(const frame& f) const {
            if (f.entry_env) { return environment_->find_access_context(); }
            if (f.locals && f.locals->is_method && f.locals->this_object_ptr) {
                if (f.locals->scope_static) { return f.locals->scope_static; }   // the declaring class, as a method env's context
                if (auto inst = f.locals->get_this().get_class_instance()) {
                    return inst->get_class_definition();
                }
            }
            return nullptr;
        }

        // Caller context for builtins (reflect::set/invoke enforcement): a builtin call
        // pushes no vm frame, so the innermost live frame answers for the caller
        const class_definition* current_access_context() const override {
            return frames_.empty() ? nullptr : frame_access_context(*frames_.back());
        }

        // Frame-first 'this': method frames carry the receiver on the record
        // (identical to the method env's copy by construction); the env-kind lookup
        // stays as the fallback for closure/coroutine frames whose 'this' flows
        // through the captured chain
        script_value* current_this(frame& f) {
            if (f.locals && f.locals->is_method && f.locals->this_object_ptr) {
                return &f.locals->get_this();
            }
            return environment_->get_value_ptr(this_id_);
        }

        // Member-shadowing scope (detail/member_scope.hpp): computed lazily per frame
        // from entry_env/locals - every push re-stamps scope_state = 0 (pooled
        // call_records reuse the same call_frame), so only bare identifiers that
        // reach the env walk pay the compute. frame_env = the env chain this frame
        // resolves against (environment_ for the executing frame; the saved caller
        // env when computing a CALLER's scope mid-push, e.g. ref-arg binding).
        void ensure_frame_member_scope(frame& f, environment* frame_env) {
            if (!f.locals || f.locals->scope_state != 0) { return; }
            detail::compute_member_scope(*f.locals, f.entry_env.get(), frame_env);
        }
        detail::member_scope_state frame_scope_state(const frame& f) const {
            return f.locals ? static_cast<detail::member_scope_state>(f.locals->scope_state)
                            : detail::member_scope_state::none;
        }
        // The env-resolved identifier node pinned for the executing ip (chunk::ident_nodes),
        // null when the instruction has none
        const identifier_expr* ident_node_at(const frame& f) const {
            const chunk& code = *f.code;
            if (code.ident_nodes.size() <= f.ip || code.ident_nodes[f.ip] == k_invalid_u32) { return nullptr; }
            return static_cast<const identifier_expr*>(code.nodes[code.ident_nodes[f.ip]].get());
        }
        bool frame_scope_active(const frame& f) const {
            const auto s = frame_scope_state(f);
            return s == detail::member_scope_state::instance ||
                   s == detail::member_scope_state::statics;
        }

        // The member-miss proof's subjects: the receiver's script class (instance scope) and the declaring class; false when either
        // is a class the proof does not cover (a host class or base: its properties and lanes answer outside the epochs)
        static bool member_miss_subject(const class_definition* cd) {
            return !cd || (cd->is_script_class() && !cd->find_cpp_base_class());
        }
        const class_definition* member_miss_dynamic_class(frame& f) const {
            const script_value& this_val = *f.locals->scope_this;
            if (this_val.raw_storage_index() != script_value::TYPEID_OBJECT) { return nullptr; }
            auto* oh = const_cast<script_value&>(this_val).unchecked_get_object_storage().get();
            if (!oh || !oh->is_instance()) { return nullptr; }
            const auto* instance = oh->unchecked_instance();
            if (instance->has_user_overflow_fields()) { return nullptr; }
            return instance->get_class_definition();
        }
        // true: no member of the frame's scope can answer sym (the proof armed at this site holds)
        bool member_miss_proven(frame& f, size_t cache_slot, uint64_t sym) {
            if (cache_slot == SIZE_MAX) { return false; }
            std::vector<env_lookup_cache_entry>* rows = frame_lookup_rows(f);
            if (!rows || rows->size() <= cache_slot) { return false; }
            const env_lookup_cache_entry& e = (*rows)[cache_slot];
            if (e.miss_sym != sym || e.miss_sym == 0) { return false; }
            const class_definition* st = f.locals->scope_static;
            if (st != e.miss_static || (st && st->method_epoch() != e.miss_static_epoch)) { return false; }
            const auto state = frame_scope_state(f);
            if (state == detail::member_scope_state::statics) { return e.miss_dynamic == nullptr; }
            if (state != detail::member_scope_state::instance || !f.locals->scope_this) { return false; }
            const class_definition* dyn = member_miss_dynamic_class(f);
            if (!dyn) { return false; }
            if (dyn == e.miss_dynamic) { return dyn->method_epoch() == e.miss_dynamic_epoch; }
            // a base method running on another receiver class: the site's second class row (main engine)
            if (parallel_worker_ || cache_slot >= f.code->member_miss_alt.size()) { return false; }
            const chunk::member_miss_row& alt = f.code->member_miss_alt[cache_slot];
            return alt.dyn == dyn && dyn->method_epoch() == alt.epoch;
        }
        // after a full probe missed (no field, method or static of the name)
        void member_miss_arm(frame& f, size_t cache_slot, uint64_t sym) {
            if (cache_slot == SIZE_MAX || sym == 0) { return; }
            std::vector<env_lookup_cache_entry>* rows = frame_lookup_rows(f);
            if (!rows) { return; }
            if (rows->size() < f.code->code.size() * 3) [[unlikely]] { rows->resize(f.code->code.size() * 3); }
            if (rows->size() <= cache_slot) { return; }
            const class_definition* st = f.locals->scope_static;
            const class_definition* dyn = nullptr;
            const auto state = frame_scope_state(f);
            if (state == detail::member_scope_state::instance) {
                if (!f.locals->scope_this || !(dyn = member_miss_dynamic_class(f))) { return; }
            } else if (state != detail::member_scope_state::statics) {
                return;
            }
            if (!member_miss_subject(st) || !member_miss_subject(dyn)) { return; }
            env_lookup_cache_entry& e = (*rows)[cache_slot];
            if (!parallel_worker_ && dyn && e.miss_sym == sym && e.miss_static == st && e.miss_dynamic && e.miss_dynamic != dyn) {
                if (f.code->member_miss_alt.size() <= cache_slot) { f.code->member_miss_alt.resize(f.code->code.size() * 3); }
                if (cache_slot < f.code->member_miss_alt.size()) {
                    f.code->member_miss_alt[cache_slot] = chunk::member_miss_row{static_cast<const class_definition*>(e.miss_dynamic), e.miss_dynamic_epoch};
                }
            }
            e.miss_sym = sym;
            e.miss_static = st;
            e.miss_static_epoch = st ? st->method_epoch() : 0;
            e.miss_dynamic = dyn;
            e.miss_dynamic_epoch = dyn ? dyn->method_epoch() : 0;
        }

        // The non-slot rungs of the member-scope ladder for POINTER resolvers (the
        // funnel twins of the interpreter's scoped_env_lookup): own scopes below the
        // floor, then fields/statics, then (post-member-miss) the per-site cache and
        // the definition/global chain. A member METHOD answers nullptr - it shadows,
        // and only value/callee sites mint.
        script_value* member_scope_env_lookup(frame& f, uint64_t symbol_id, size_t cache_slot) {
            if (lazy_method_frame(f)) {
                if (script_value* ic_field = this_field_ic_probe(f, cache_slot, symbol_id)) { return ic_field; }
            }
            if (script_value* own = detail::own_scope_lookup(environment_.get(), f.locals->scope_floor, symbol_id)) {
                return own;
            }
            if (script_value* ic_field = this_field_ic_probe(f, cache_slot, symbol_id)) {
                return ic_field;   // the walk-miss proof covers the own scopes; a field is the probe's first rung
            }
            if (member_miss_proven(f, cache_slot, symbol_id)) {
                if (script_value* cached = env_lookup_cached(f, cache_slot, symbol_id)) {
                    return cached;
                }
                return f.locals->scope_floor ? f.locals->scope_floor->get_value_ptr(symbol_id)
                                             : environment_->get_value_ptr(symbol_id);
            }
            auto hit = detail::probe_member_scope(frame_scope_state(f), f.locals->scope_this,
                                                  f.locals->scope_static, symbol_id, /*want_methods*/ false);
            if (hit) {
                if (hit.class_def && hit.class_def->chain_has_nonpublic() &&
                    !detail::bare_member_access(hit, symbol_id, frame_access_context(f))) [[unlikely]] {
                    return nullptr;   // out of reach: the value site raises the access text
                }
                if (hit.what == detail::member_probe_hit::kind::field && hit.instance) {
                    this_field_ic_arm(f, cache_slot, hit.instance, symbol_id);
                }
                return hit.value_ptr;   // a C++ property answers nullptr: a shadow, minted by value sites
            }
            if (detail::member_scope_has_method(frame_scope_state(f), f.locals->scope_this,
                                                f.locals->scope_static, symbol_id)) {
                return nullptr;
            }
            member_miss_arm(f, cache_slot, symbol_id);
            if (cache_slot != SIZE_MAX) {
                if (script_value* cached = env_lookup_cached(f, cache_slot, symbol_id)) {
                    return cached;
                }
            }
            return f.locals->scope_floor ? f.locals->scope_floor->get_value_ptr(symbol_id)
                                         : environment_->get_value_ptr(symbol_id);
        }

        // Env-lazy method frames (the common all-public case) resolve with the current
        // env AS the scope floor: own_scope_lookup walks zero envs, so no env variable can
        // shadow a field regardless of the engine-wide serial - class identity + slot are
        // the whole proof. Eager frames keep the serial (their scope env sits below the
        // floor and may define). Loop scopes churning the serial (top-level loop bodies,
        // constructions) no longer starve lazy methods' field caches.
        // a method frame that runs over its definition environment (no frame env: entry_env null) has no scopes of its own, so a member
        // outranks every environment name whatever the env serial says (lazy_frame_no_own_scopes' answer, read off the frame)
        static bool lazy_method_frame(const frame& f) noexcept {
            return f.locals && f.locals->is_method && !f.entry_env && !f.top_level;
        }
        bool lazy_frame_no_own_scopes(frame& f) {
            ensure_frame_member_scope(f, environment_.get());
            return f.locals && frame_scope_state(f) == detail::member_scope_state::instance &&
                   f.locals->scope_floor == environment_.get();
        }

        // This-field IC probe (bare method-field reads, the BST/gameplay heat): the
        // site's entry carries a serial-guarded WALK-MISS PROOF — while the engine-wide
        // env serial is unchanged since a full walk missed here, no env cell anywhere
        // can shadow the symbol, so resolution jumps straight to the receiver's field
        // cell. find_field_value re-derives slot/class per access: polymorphic
        // receivers and hot reload stay correct with no identity compare. Main engine
        // only (workers keep their private machinery); method-locals frames only so
        // this-resolution is one pointer read.
        script_value* this_field_ic_probe(frame& f, size_t cache_slot, uint64_t sym) {
            if (cache_slot == SIZE_MAX) { return nullptr; }
            if (!f.locals || !f.locals->is_method || !f.locals->this_object_ptr) { return nullptr; }
            std::vector<env_lookup_cache_entry>* rows = frame_lookup_rows(f);
            if (!rows) { return nullptr; }
            auto& cache = *rows;
            if (cache.size() <= cache_slot) { return nullptr; }
            const auto& entry = cache[cache_slot];
            if (entry.this_field_serial == 0 || entry.this_field_sym != sym) { return nullptr; }
            if (!lazy_method_frame(f) && entry.this_field_serial != env_symbolizer_->env_epoch() && !lazy_frame_no_own_scopes(f)) { return nullptr; }
            script_value& this_val = f.locals->get_this();
            if (!this_val.is_object() || this_val.is_null()) { return nullptr; }
            // Raw extraction (get_class_instance's exact ladder minus its handle
            // copies): the frame's this anchor pins the instance for the whole call,
            // so a raw pointer over the op is sound.
            auto* oh = this_val.unchecked_get_object_storage().get();
            if (!oh || !oh->is_instance()) { return nullptr; }
            auto* instance = oh->unchecked_instance();
            if (!instance) { return nullptr; }
            if (entry.this_field_class &&
                entry.this_field_class == static_cast<const void*>(instance->get_class_definition())) {
                // Absent/invalid cell returns null -> the caller's full ladder handles
                // defaults/errors exactly as a find_field_value miss would
                return instance->field_slot_value(entry.this_field_slot);
            }
            // another class at the site: a C++ property's placeholder is no cell (the object answers, through the member probe)
            if (const class_definition* cd = instance->get_class_definition()) {
                if (!parallel_worker_ && cache_slot < f.code->this_field_alt.size() && f.code->this_field_alt[cache_slot].cd == cd) {
                    return instance->field_slot_value(f.code->this_field_alt[cache_slot].slot);
                }
                const uint32_t slot = cd->field_slot(sym);
                if (slot != class_definition::k_no_field_slot && cd->slot_is_cpp_property(slot)) { return nullptr; }
                if (slot != class_definition::k_no_field_slot && !parallel_worker_) {
                    // the id->slot mapping is append-only for a class object's lifetime: the pair stays true
                    if (f.code->this_field_alt.size() <= cache_slot) { f.code->this_field_alt.resize(f.code->code.size() * 3); }
                    if (cache_slot < f.code->this_field_alt.size()) { f.code->this_field_alt[cache_slot] = chunk::this_field_row{cd, slot}; }
                    return instance->field_slot_value(slot);
                }
            }
            return instance->find_field_value(sym);
        }
        // Method-IC field-shadow guard: a field named like the method shadows it. The
        // class-level answer is memoized at IC arm (mic_field_free, valid under the
        // mic's method_epoch - every field-set mutation bumps it); only an instance that
        // grew user fields at runtime re-asks has_field.
        static bool mic_field_shadowed(const call_site& site, const class_instance& inst, uint64_t id) {
            if (site.mic_field_free && !inst.has_user_overflow_fields()) { return false; }
            return inst.has_field(id);
        }
        static bool probe_field_shadowed(const call_site& site, const class_instance& inst, uint64_t id) {
            if (site.probe_field_free && !inst.has_user_overflow_fields()) { return false; }
            return inst.has_field(id);
        }
        static bool native_field_shadowed(const call_site& site, const class_instance& inst, uint64_t id) {
            if (site.nat_field_free && !inst.has_user_overflow_fields()) { return false; }
            return inst.has_field(id);
        }
        // A bound method's direct lane at a method call site (detail/host_member_access.hpp host_method_fill): the receiver class's
        // own, or a script layer's C++ base's. True when the lane ran the call (out holds the status); false sends the call on.
        // The object the site's lane runs on for this receiver (site.nat_hit.entry), else null: the call goes on
        void* native_site_self(const call_site& site, class_definition& cd, class_instance& inst, uint64_t id, const void*& entry);
        // a worker's: the region pins the host method, the call's arity is neither serial nor a mutation of an object the chunk did not make
        // (the pinned call raises those), and the worker's own row holds the lane
        void* worker_native_self(const frame& f, const call_site& site, class_definition& cd, class_instance& inst, uint64_t id, size_t argc, const void*& entry);
        void worker_native_refill(worker_chunk_env_cache::native_row& row, class_definition& cd, class_instance& inst, uint64_t id, uint64_t stamp);
        void native_site_refill(const call_site& site, class_definition& cd, class_instance& inst, uint64_t id, uint64_t stamp);
        // the instance a host-method call's receiver holds when the site's ordinary-path memo answers it, else null
        class_instance* ordinary_site_instance(const call_site& site, const script_value& receiver) const;
        void ordinary_site_arm(const call_site& site, const script_value& receiver, uint64_t member_id);
        // A bare name read in a method that names a C++ member of the receiver: the value through the lane the slot holds for
        // the receiver's class (never fills), and the fill once the member probe answered cpp_property
        bool bare_host_read(frame& f, size_t cache_slot, script_value& out);
        bool bare_host_fill_read(frame& f, size_t cache_slot, uint64_t sym, class_instance* inst, script_value& out);
        void this_field_ic_arm(frame& f, size_t cache_slot, class_instance* instance, uint64_t sym) {
            if (cache_slot == SIZE_MAX) { return; }
            if (!f.locals || !f.locals->is_method) { return; }
            std::vector<env_lookup_cache_entry>* rows = frame_lookup_rows(f);
            if (!rows) { return; }
            auto& cache = *rows;
            // env_lookup_cached sizes this vector on ITS fill path; a method body that
            // only reads fields resolves at the member probe and never gets there
            if (cache.size() < f.code->code.size() * 3) [[unlikely]] {
                cache.resize(f.code->code.size() * 3);
            }
            if (cache.size() <= cache_slot) { return; }
            auto& entry = cache[cache_slot];
            entry.this_field_serial = env_symbolizer_->env_epoch();
            class_definition* cd = instance ? instance->get_class_definition() : nullptr;
            // Re-arm after serial churn (eager frames, constructions): the class's id->slot
            // mapping is append-only, so a same-class re-arm keeps slot and store facts
            if (cd && entry.this_field_class == static_cast<const void*>(cd) && entry.this_field_sym == sym) { return; }
            entry.this_field_sym = sym;
            entry.this_field_class = nullptr;
            entry.this_field_slot = 0xFFFFFFFFu;
            entry.this_store_state = 0;
            if (cd) {
                const uint32_t slot = cd->field_slot(sym);
                if (slot != class_definition::k_no_field_slot) {
                    entry.this_field_class = cd;
                    entry.this_field_slot = slot;
                }
            }
        }
        // a store the checker resolved to a field of the method's class (no local of the name, no user accessor, not a global-tier
        // name): no environment cell can take the name, so the entry's walk-miss proof holds whatever the epoch
        bool stamped_this_field_store(const frame& f, uint64_t sym) const {
            const identifier_expr* id = ident_node_at(f);
            return id && id->this_field && id->symbol_id == sym;
        }
        // Store-side this-field IC (bare method-field writes): the read proof plus the
        // arm-time facts of the store landing. Returns the live cell, or null for the ladder.
        script_value* this_field_store_probe(frame& f, size_t cache_slot, uint64_t sym, type_info*& declared_out) {
            if (cache_slot == SIZE_MAX) { return nullptr; }
            if (!f.locals || !f.locals->is_method || !f.locals->this_object_ptr) { return nullptr; }
            std::vector<env_lookup_cache_entry>* rows = frame_lookup_rows(f);
            if (!rows) { return nullptr; }
            auto& cache = *rows;
            if (cache.size() <= cache_slot) { return nullptr; }
            const auto& entry = cache[cache_slot];
            if (entry.this_store_state != 1 || entry.this_field_sym != sym) { return nullptr; }
            if (!lazy_method_frame(f) && entry.this_field_serial != env_symbolizer_->env_epoch() && !lazy_frame_no_own_scopes(f) && !stamped_this_field_store(f, sym)) { return nullptr; }
            script_value& this_val = f.locals->get_this();
            if (!this_val.is_object() || this_val.is_null()) { return nullptr; }
            auto* oh = this_val.unchecked_get_object_storage().get();
            if (!oh || !oh->is_instance()) { return nullptr; }
            auto* instance = oh->unchecked_instance();
            if (!instance) { return nullptr; }
            class_definition* cd = instance->get_class_definition();
            if (!cd || entry.this_field_class != static_cast<const void*>(cd) ||
                entry.this_store_epoch != cd->method_epoch()) { return nullptr; }
            script_value* cell = instance->field_slot_value(entry.this_field_slot);
            if (!cell) { return nullptr; }
            declared_out = entry.this_store_type;
            return cell;
        }
        void this_field_store_arm(frame& f, size_t cache_slot, class_instance* instance, uint64_t sym) {
            this_field_ic_arm(f, cache_slot, instance, sym);
            if (cache_slot == SIZE_MAX || !f.locals || !f.locals->is_method) { return; }
            std::vector<env_lookup_cache_entry>* rows = frame_lookup_rows(f);
            if (!rows) { return; }
            auto& cache = *rows;
            if (cache.size() <= cache_slot) { return; }
            auto& entry = cache[cache_slot];
            class_definition* cd = instance ? instance->get_class_definition() : nullptr;
            if (!cd || entry.this_field_class != static_cast<const void*>(cd)) { return; }
            if (entry.this_store_state == 1 && entry.this_store_epoch == cd->method_epoch()) { return; }   // facts still valid
            if (auto cpp_base = cd->find_cpp_base_class(); cpp_base && cpp_base->get_property_setter_id(sym) != 0) { return; }
            entry.this_store_type = cd->get_field_declared_type(sym).get();
            entry.this_store_epoch = cd->method_epoch();
            entry.this_store_state = 1;
        }

        // Frame-twin of environment::get_value_ptr's method-kind fallback (receiver
        // fields, then class statics) for method-lazy frames — env-resolved eager
        // frames never miss into this. KEEP BYTE-PARALLEL with environment.cpp.
        // The bare-name ref-bind rung after the slot/env/this-member misses: a hybrid's
        // C++ property on the frame's receiver mints a CPP-MEMBER / PROPERTY reference
        // (nullopt = not a property here; the caller keeps its undefined-variable text).
        // Defined in vm_backend.cpp (needs detail/ref_lvalue.hpp).
        std::optional<checked_result<script_value>> bind_bare_cpp_property_ref(frame& f, uint64_t sym, environment* env = nullptr);
        script_value* frame_this_member_ptr(frame& f, uint64_t sym) {
            if (!f.locals || !f.locals->is_method || !f.locals->this_object_ptr) { return nullptr; }
            script_value& this_val = f.locals->get_this();
            if (!this_val.is_object() || this_val.is_null()) { return nullptr; }
            auto instance = this_val.get_class_instance();
            if (!instance) { return nullptr; }
            if (const class_definition* def = instance->get_class_definition(); def && def->chain_has_nonpublic() &&
                !detail::enforce_member_access(def, sym, frame_access_context(f))) [[unlikely]] {
                return nullptr;   // out of reach: frame_bare_member_access names the text
            }
            // A C++ property has no cell: the pointer form answers null (the ref arms mint instead)
            if (detail::cpp_property_getter_id(instance.get(), sym)) { return nullptr; }
            script_value& field_ref = instance->get_field(sym, false);
            if (!field_ref.is_invalid()) { return &field_ref; }
            if (class_definition* owner = detail::bare_static_owner(frame_declaring_class(f), instance->get_class_definition(), sym)) {
                return owner->get_static_field_ptr(sym);
            }
            return nullptr;
        }
        // The class whose code the frame runs (a bare static's lookup origin): stamped on method
        // frames, else the lexical access context
        class_definition* frame_declaring_class(const frame& f) const {
            if (f.locals && f.locals->is_method && f.locals->scope_static) { return f.locals->scope_static; }
            return detail::lexical_access_class(environment_.get());
        }
        // A reference to a bare member out of reach names the access text, never the undefined one
        checked_result<void> frame_bare_member_access(frame& f, uint64_t sym) {
            if (!f.locals || f.top_level) { return {}; }
            ensure_frame_member_scope(f, environment_.get());
            if (!frame_scope_active(f)) { return {}; }
            auto hit = detail::probe_member_scope(frame_scope_state(f), f.locals->scope_this, f.locals->scope_static, sym,
                                                  /*want_methods*/ false);
            return hit ? detail::bare_member_access(hit, sym, frame_access_context(f)) : checked_result<void>{};
        }
        op_status member_access_value(frame& f, const script_value& raw_object, member_expr* expr, script_value& out,
                                      bool as_callee = false);
        op_status static_member_value(frame& f, member_expr* expr, script_value& out);
        // consumable: the store's own operand, which a converting constructor may take as its parameter
        op_status assign_member(frame& f, const script_value& object_value, member_expr* member, const script_value& value,
                                script_value* consumable = nullptr);
        // The field-write landing op_set_member and op_member_compound share: the proven
        // field write, then the write-side site IC (member_ic at ins_index). nullopt = the
        // general assign_member ladder runs; a value is the landed store's status.
        std::optional<op_status> member_field_write_fast(frame& f, size_t ins_index, member_expr* member,
                                                         const script_value& dereferenced, const script_value& value,
                                                         script_value* consumable = nullptr);
        op_status store_static_target(frame& f, member_expr* member, const script_value& result, script_value* stored_out = nullptr);
        // ns::x = v / op= / ++ on a namespace variable's cell, and a namespace's variable declaration run in its scope
        // (detail/namespace_scope.hpp)
        op_status store_namespace_cell(const detail::namespace_store_target& target, member_expr* member, script_value incoming, script_value* stored_out);
        // the engine's listing sink: the compiler lists its regions while a sink is set, and every chunk this backend
        // compiles is handed to the sink disassembled, in compile order (engine::disassembly_sink)
        void arm_listing();
        void note_compiled(const chunk& compiled);
        op_status exec_namespace_variable(const declaration_ptr& member_decl, script_namespace_data& ns_data, uint64_t namespace_id);
        op_status invoke_callee(frame& f, script_value&& callee, std::vector<script_value>& arguments, const call_site& site);
        op_status invoke_native(frame& f, const script_function& func, std::vector<script_value>& arguments, const call_site& site);
        // Typed bound-method mint for this-fallback identifier resolutions (bare
        // sibling calls): call paths recover the payload (kind bound_method) and enter
        // the method in-loop; every other caller gets make_bound_method's exact
        // behavior from script_callable_thunk::operator().
        script_value make_bound_method_thunk(const script_value& this_val, script_value method);
        bool park_sibling_method(frame& f, const call_site& site);
        int worker_park_sibling(frame& f, const call_site& site);
        op_status exec_math_pair(frame& f, const vm_instruction& ins, detail::math_fn fn);   // the sibling IC's hit: the class's method and this frame's receiver
        void arm_sibling_method(frame& f, const call_site& site, const script_value& resolved);
        // ONE native boundary for the direct dispatches (builtin methods, coroutine
        // resume): routine errors travel as checked_result -> raise_from; the catch is
        // last-ditch armor for callees that physically throw C++ (host-bound methods,
        // legacy throwing accessors), converting exactly like invoke_callee's opaque
        // branch. Out-of-line + noinline so opcode bodies stay EH-free; dies once the
        // engine no-throw conversion starves it.
        template <typename Call>
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status guarded_native_call(Call&& call);

        op_status exec_class_decl_node(class_decl* decl);
        op_status exec_namespace_decl_node(namespace_decl* decl);

        // Op failures write details here (only via raise_); returns carry op_status.
        error_propagator pending_error_{};
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status raise_(std::error_code ec, std::string_view msg = {}, uint64_t sym = 0, uint64_t sym2 = 0) {
            pending_error_ = error_propagator{ec, msg, sym, sym2};
            return op_status::failed;
        }
        // not_a_function with the family-B text (name_id 0 = computed callee)
        op_status raise_not_callable(uint64_t name_id, const script_value& callee);
        op_status raise_from(const error_propagator& prop) {
            pending_error_ = prop;
            return op_status::failed;
        }
        template <typename T>
        op_status raise_from(const checked_result<T>& r) {
            pending_error_ = r.error_value();
            return op_status::failed;
        }
        op_status vm_check(op_status s) { return s; }
        op_status vm_check(const checked_result<void>& r) { return r ? op_status::ok : raise_from(r); }

        op_status exec_load(frame& f, const vm_instruction& ins);
        op_status exec_load_impl(frame& f, const vm_instruction& ins);
        op_status exec_store(frame& f, const vm_instruction& ins);
        op_status exec_compound_store(frame& f, const vm_instruction& ins);
        op_status exec_compound_store_full(frame& f, const vm_instruction& ins);
        op_status compound_append_string(script_value& slot, const vm_instruction& ins);
        bool charge_text_append(const script_value& target, const script_value& rhs);
        bool charge_text_concat(const script_value& target, const script_value& rhs);
        std::optional<op_status> index_text_append(frame& f);
        bool self_concat_store(frame& f, const fused_binary_dst_proto& dp, op_status& st);
        std::optional<op_status> member_text_append(frame& f, size_t ins_index, member_expr* member, const script_value& object,
                                                    script_value& current, const script_value& right);
        std::optional<op_status> host_member_text_append(frame& f, size_t ins_index, member_expr* member, const script_value& object,
                                                         script_value& current, const script_value& right);
        std::optional<op_status> static_text_append(frame& f, member_expr* member, script_value& current, const script_value& right);
        std::optional<op_status> field_cell_text_append(script_value* cell, const class_definition* cd, uint64_t field_id,
                                                        script_value& current, const script_value& right, bool charge);
        class_instance* plain_member_store_slot(frame& f, size_t ins_index, member_expr* member, const script_value& object, uint32_t& slot);
        void append_text_in_place(script_value& target, const script_value& rhs, bool replacing);
        op_status exec_incdec(frame& f, const vm_instruction& ins);
        op_status exec_decl_var(frame& f, const vm_instruction& ins);
        op_status exec_decl_var_full(frame& f, const vm_instruction& ins);
        [[nodiscard]] checked_result<script_value> compound_value(token_type op, const script_value& current, const script_value& right, bool incdec = false,
                                                                  bool text_slot = false);
        op_status bind_graft_parameter(frame& f, const variable_decl* decl);   // a graft's by-value parameter: bind_parameters's by-value lane over the popped argument
        bool graft_parameter_alias(frame& f, const variable_decl* decl, const script_value& value);   // the read-only alias the call would take
        op_status exec_decl_ref_ident(frame& f, const vm_instruction& ins);
        op_status exec_decl_ref_value(frame& f, const vm_instruction& ins);
        op_status exec_binary(frame& f, const vm_instruction& ins);
        bool stack_numeric_rows(script_value& l, const script_value& r, token_type op);
        op_status exec_binary_fused(frame& f, const vm_instruction& ins);
        op_status exec_operand_temp(frame& f, const vm_instruction& ins);
        op_status exec_call_temp(frame& f, const vm_instruction& ins);   // a call's result into a compiler temp (op_call_temp)
        op_status exec_member_temp(frame& f, const vm_instruction& ins);
        JAI_FORCEINLINE op_status exec_member_temp_at(frame& f, const vm_instruction& ins, size_t ip);
        op_status exec_member_temp_this(frame& f, const vm_instruction& ins);
        op_status exec_member_temp_this_at(frame& f, const vm_instruction& ins, size_t ip);
        const script_value* member_temp_field(frame& f, const vm_instruction& mt, size_t ip);
        int member_fold_fast(frame& f, const chunk::member_fold& fd);
        op_status exec_mfold(frame& f, const vm_instruction& ins);
        op_status exec_get_member_path(frame& f, const vm_instruction& ins);
        const script_value* path_index_value(frame& f, const fused_operand& index);
        op_status local_member_receiver(frame& f, const member_expr* member, uint32_t slot, script_value& out);   // a member site's receiver from its base slot (a nested site's prefix read)
        op_status exec_set_member_fused(frame& f, const vm_instruction& ins);
        op_status exec_math_temp(frame& f, const vm_instruction& ins);
        op_status exec_math_alias(frame& f, const vm_instruction& ins);
        op_status exec_math_guard(frame& f, const vm_instruction& ins);
        // regex(literal pattern[, flags]): the value over the pattern compiled at load while the callee is the engine's
        // constructor, else the call with the literal texts (the interpreter's visit_call_expr_t takes the same branch)
        op_status exec_regex_constant(frame& f, const vm_instruction& ins);
        op_status exec_format_call(frame& f, const vm_instruction& ins);
        op_status exec_cells_enter(frame& f, const vm_instruction& ins);
        int math_alias_verdict(frame& f, const call_site& site, uint8_t fn, size_t cache_slot);   // 1 builtin, 0 parked, -1 raised
        op_status park_callee(frame& f, const call_site& site, size_t argc, const script_value& callee_value);
        op_status exec_unary_temp(frame& f, const vm_instruction& ins);
        // The ONE fused-binary computation, sink-templated (dest-addressing):
        // op_binary_fused pushes, the _decl/_store variants land the
        // result without a push or a second dispatch. Defined in vm_backend.cpp.
        template <typename Sink>
        op_status binary_fused_compute(frame& f, uint32_t proto_index, Sink&& sink);
        // Typed heads: a proto whose operands the checker
        // proved int/int or float/float addresses its slots directly and computes with one
        // tag guard per operand; false = not handled (guard miss, an error case, a dead
        // slot), the general kernel then runs and raises exactly as before.
        const script_value* direct_operand(frame& f, const fused_operand& operand) noexcept;
        template <typename Sink>
        bool typed_binary_head(frame& f, const fused_binary_proto& p, Sink& sink);
        op_status exec_binary_fused_decl(frame& f, const vm_instruction& ins);
        op_status exec_binary_fused_store(frame& f, const vm_instruction& ins);
        // Fused subscript read/store: container+index as operands (no LOAD dispatches);
        // non-array shapes replay the verbatim unfused ops. The read core is
        // sink-templated (defined in vm_backend.cpp): the plain op pushes, the decl
        // variant lands in the declared slot.
        template <bool SinkIsStack, typename Sink>
        op_status index_fused_read(frame& f, const vm_instruction& ins, Sink&& sink, uint32_t extra_flags = 0);
        struct fused_index_operand {
            enum outcome_kind : uint8_t { operand, boxed, settled };
            std::optional<script_value> computed, container_snapshot, index_scratch;
            const script_value* index = nullptr;
            script_int raw = 0;
            bool have_raw = false;
            outcome_kind outcome = operand;
        };
        op_status resolve_fused_index_operand(frame& f, const fused_index_proto& p, const script_value*& container_ptr,
                                              const script_value* pending, fused_index_operand& out);
        // Typed element-read lane for the proven decl shape (fused_index_proto::typed_lane):
        // operands straight off the frame or the per-site cache, the int index folded raw,
        // the element landed in the declared slot by the decl lane's rules. false = a
        // runtime miss (not an array, a non-int operand, an overflowing fold, out of
        // bounds, a kind the lane does not land): the general kernel runs unchanged.
        const script_value* fast_operand(frame& f, const fused_operand& operand, size_t cache_slot);
        // A reference a lane may read through before it knows it will land: every mode but
        // the binder-property ones (their deref runs the getter, which a host can observe,
        // and a lane that misses after reading would run it again on the general path)
        static bool lane_deref_silent(const script_value& v) noexcept;
        // Typed scalar set (opcode.hpp op_t_*): guarded fast paths over frame slots, the
        // original instruction from chunk::typed_sites on any miss
        const script_value* typed_right(frame& f, uint32_t c) {
            if (c & k_typed_const_bit) { return &f.code->constants[c & ~k_typed_const_bit]; }
            if (c & k_typed_env_bit) { return typed_env(f, c, 1); }
            return frame_slot(f, c);
        }
        const script_value* typed_left(frame& f, uint32_t b) {
            if (b & k_typed_env_bit) { return typed_env(f, b, 0); }
            return frame_slot(f, b);
        }
        // an environment operand through the replaced instruction's nullable fast head and cache slot (null: its general form)
        const script_value* typed_env(frame& f, uint32_t v, uint32_t side) {
            return fused_cmp_operand(f, f.code->typed_env_operands[v & k_typed_slot_mask], f.ip * 3 + side);
        }
        bool typed_land_int(frame& f, uint32_t a, script_int v);
        bool typed_land_float(frame& f, uint32_t a, script_float v);
        // The typed ops' fast paths without their fallbacks (guards, arithmetic, landing): the
        // wrappers below add the fallback, the cell block loop returns to the general dispatch
        // at the missing ip instead. cmp_jump: 1 = fell through, 2 = jumped, -1 = miss
        // force-inlined into their wrappers (and the cell block loop): wrappers calling them out of line
        // cost measurably on JaiDOOM
        JAI_FORCEINLINE bool typed_arith_fast(frame& f, const vm_instruction& ins);
        JAI_FORCEINLINE int typed_cmp_jump_fast(frame& f, const vm_instruction& ins);
        JAI_FORCEINLINE bool typed_load_fast(frame& f, const vm_instruction& ins);
        template <class Index, class Stamp> JAI_FORCEINLINE bool typed_element_read(frame& f, const script_value* cp, uint32_t guard_temp, bool guard_always, uint32_t dslot, uint8_t expect, Index&& index_of, Stamp&& stamp_of);
        type_info* temp_stamp_for(const script_array& node, size_t at) const;
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status exec_inline_guard(frame& f, const vm_instruction& ins);   // an inlined leaf's guard: the callee name binds the site's function, or the fallback
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        bool inline_site_holds(frame& f, const chunk::inline_site& site);
        // a worker's row for an inline site's validated callee / a call site's parked callee (null: the chunk has no record on this worker,
        // or the site is another chunk's)
        JAI_FORCEINLINE const script_function** worker_inline_holds(const frame& f, const chunk::inline_site& site) {
            worker_chunk_env_cache* rec = worker_chunk_record(f.code);
            const size_t i = rec ? index_within(site, f.code->inline_sites) : SIZE_MAX;
            if (i == SIZE_MAX) { return nullptr; }
            if (!rec->rows_sized) [[unlikely]] { size_worker_rows(*rec); }
            return &rec->inline_holds[i];
        }
        JAI_FORCEINLINE worker_chunk_env_cache::call_ic* worker_call_ic(const frame& f, const call_site& site) {
            worker_chunk_env_cache* rec = worker_chunk_record(f.code);
            const size_t i = rec ? index_within(site, f.code->call_sites) : SIZE_MAX;
            if (i == SIZE_MAX) { return nullptr; }
            if (!rec->rows_sized) [[unlikely]] { size_worker_rows(*rec); }
            return &rec->call_ics[i];
        }
        // the lookup rows of the frame's chunk: the chunk's own on the main engine, a worker's own in its record of the chunk, sized once
        // so the frames' tables never move (null: the chunk has no record on this worker)
        std::vector<env_lookup_cache_entry>* frame_lookup_rows(const frame& f) {
            if (cached_global_env_) { return &f.code->env_lookup_cache; }
            if (!parallel_worker_) { return nullptr; }
            worker_chunk_env_cache* rec = worker_chunk_record(f.code);
            if (!rec) { return nullptr; }
            if (rec->entries.size() < f.code->code.size() * 3) [[unlikely]] { rec->entries.resize(f.code->code.size() * 3); }
            return &rec->entries;
        }
        // a worker frame's own lookup row for cache_slot, the frame's table found once (null: the chunk has no record on this worker)
        const env_lookup_cache_entry* worker_armed_entry(frame& f, size_t cache_slot) {
            if (f.env_table_code == f.code) { return cache_slot < f.code->code.size() * 3 ? &f.env_table[cache_slot] : nullptr; }
            env_lookup_cache_entry* e = cache_slot < f.code->code.size() * 3 ? worker_env_cache_slot(f.code, cache_slot) : nullptr;
            if (e) { f.env_table_code = f.code; f.env_table = e - cache_slot; }
            return e;
        }
        bool inline_globals_hold(const frame& f, const chunk::inline_site& site);
        bool inline_forward_callees_hold(const chunk::inline_site& site);
        bool inline_method_verdict(const chunk::inline_site& site, const class_definition* cd, const class_definition* ctx);
        bool comparison_free(const script_value& v);
        const uint64_t* layout_gen_ = nullptr;   // engine::layout_generation
        void bind_member_layout(frame& f, const chunk& body, class_instance* receiver = nullptr);
        bool member_layout_same(const chunk& body, const class_definition* cd);
        bool member_layout_verdict(const chunk& body, const class_definition* cd);
        bool park_frame_sibling(frame& f, const call_site& site);
        frame* push_lean_method_frame(frame& caller, const script_method_dispatch& dispatch, const std::shared_ptr<function_decl>& ast,
                                      script_value& receiver, class_instance* inst, size_t frame_base, size_t args_base, size_t argc);
        bool lean_method_site(const chunk& code, const call_site& site) const;
        void pop_lean_frame(call_record& rec);
        // a lean frame pops through pop_lean_frame
        static bool pops_lean(const call_record& rec) noexcept {
            return rec.lean && !rec.locals.boxed_cells;
        }
        // the call instructions' lanes into a lean frame: the frame pushed, or null when the instruction's general path runs
        frame* enter_lean_method_call(frame& f, const vm_instruction& ins);
        frame* enter_lean_sibling_call(frame& f, const vm_instruction& ins);
        frame* enter_probed_call(frame& f, const vm_instruction& ins);
        // a member-class body's bare field at slot + 1 `s`: the receiver's cell, while the frame's layout holds (no class anywhere
        // changed methods or field shape, the receiver still has its class); null sends the read to the general ladder
        JAI_FORCEINLINE script_value* frame_this_field(const frame& f, uint32_t s) const noexcept {
            if (!f.this_inst || f.this_gen != *layout_gen_ || f.this_inst->get_class_definition() != f.this_cd) { return nullptr; }
            return f.this_inst->field_slot_value(s - 1);
        }
        // a member-class body's bare name past the members (chunk::this_global_uses) is the environment's while the frame's layout holds and
        // the receiver carries no field of its own making
        JAI_FORCEINLINE bool frame_globals_hold(const frame& f) const noexcept {
            return f.this_inst && f.this_gen == *layout_gen_ && f.this_inst->get_class_definition() == f.this_cd && !f.this_inst->has_user_overflow_fields();
        }
        static uint32_t this_field_slot_at(const frame& f) noexcept {
            const auto& t = f.code->this_field_slots;
            return f.ip < t.size() ? t[f.ip] : 0;
        }
        bool handle_admits(const script_value& v, const type_info_ptr& declared);
        struct handle_admits_row { const class_definition* cd = nullptr; const type_info* tag = nullptr; const type_info* declared = nullptr; uint64_t epoch = 0; bool admits = false; };
        static constexpr size_t k_handle_admits_rows = 8;
        handle_admits_row handle_admits_rows_[k_handle_admits_rows];
        size_t handle_admits_next_ = 0;
        struct comparison_free_row { const class_definition* cd = nullptr; uint64_t epoch = 0; bool free = false; };
        static constexpr size_t k_comparison_free_rows = 8;
        comparison_free_row comparison_free_rows_[k_comparison_free_rows];
        size_t comparison_free_next_ = 0;
        bool math_alias_holds(frame& f, const chunk::cell_loop::math_alias_site& site);
        bool call_typed_land(frame& caller, const script_value& v);   // op_call_typed's landing (the caller's ip on the call)
        JAI_FORCEINLINE bool inline_method_holds(frame& f, const chunk::inline_site& site);
        bool worker_inline_method_holds(frame& f, const chunk::inline_site& site);   // the identity, read without running a getter
        op_status exec_cells_back(frame& f, const vm_instruction& ins);
        op_status exec_c_mload(frame& f, const vm_instruction& ins);   // a member read site: the field's kind and payload into the cell
        op_status exec_c_mstore(frame& f, const vm_instruction& ins);   // a member store site: the field's payload written in place when the kinds agree
        // fused cell pairs: the first instruction's executor on its own word, then the second's on the next word when the block is
        // still live and the ip fell through; out of line, so the block switch keeps its shape
        struct cell_regs;   // the block loop's registers (defined with cells_run below)
        void cells_miss_ip(frame& f, size_t& ip, const chunk::cell_loop& loop, uint32_t original_ip, unsigned step);
        void cells_flush_ip(frame& f, size_t& ip, const chunk::cell_loop& loop);
        op_status exec_c_arith2(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
        op_status exec_c_jcmp2(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
        op_status exec_c_load2(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
        op_status exec_c_arith_load(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
        op_status exec_c_load_jcmp(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
        JAI_FORCEINLINE bool cells_second_half_injected(frame& f, size_t& ip);   // the injection lever's count for a fused pair's second instruction (a build without the lever: false)
        script_array* velem_site(frame& f, const velem_proto& p, size_t& at, int& comp);   // a vector buffer's element and component for a velem site, else null
        op_status exec_velem_load(frame& f, const vm_instruction& ins);
        op_status exec_velem_store(frame& f, const vm_instruction& ins);
        op_status exec_set_member_value(frame& f, const vm_instruction& ins);   // `base.member = operand;` in statement position: the store kernel, no result
        op_status store_member(frame& f, size_t ins_index, uint32_t node, script_value&& object_value, script_value&& value, bool want_result);   // SET_MEMBER's kernel: the fast lane, the ladder, the result when asked
        op_status assign_math_component(frame& f, uint32_t node, script_value& base, member_expr* member, const script_value& value, script_value& out);   // `base.x = v` on a math value
        op_status exec_cells_cfor_back(frame& f, const vm_instruction& ins);   // a counted loop's back edge: the general step and compare, then the block entry
        op_status cells_counted_head_entry(frame& f, uint32_t li);             // a counted loop's entry at its CFOR_PREP after the first compare
        bool cells_enter(frame& f, const chunk::cell_loop& loop, bool head = false);   // the guards, the container sites and the unboxing; false = the general form runs (a loop's head entry refuses softly: the back edge tries again)
        op_status cells_run(frame& f, const chunk::cell_loop& loop);           // the copy as one block until an exit, a miss or a raise
        // the block loop's registers: the cells, their dirty bytes, the chunk's constants, the copy's code and
        // the loop, read once per run; every executor on the hot switch reads through them instead of re-deriving frame -> cells -> vectors
        struct cell_regs { cell_word* cells; cell_dirty* dirty; const script_value* consts; const vm_instruction* code; const chunk::cell_loop* loop; };
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status cells_run_cold(frame& f, const chunk::cell_loop& loop, const vm_instruction& ci);   // the call door's instructions, off the block loop's hot switch
        op_status cfor_advance(counted_for_state& st, bool& again);             // the counted back edge's step and compare on the general slot
        JAI_FORCEINLINE op_status exec_c_cfor_back(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status exec_c_cmp_any(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr, bool head);
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status exec_c_neg(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status exec_c_bnot(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status exec_c_lnot(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);     // the truthiness inverted, a bool
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status exec_c_cmpval(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);   // a comparison as a value, a bool
        bool cell_truthy(frame& f, uint32_t operand, bool& truthy);                    // the language's truthiness of an int, a float or a bool cell or constant
        // a call inside the block: the probe, the pushes and the call run the general executors verbatim; the landing after the
        // return re-reads what the callee could have touched and lands the result by the declared-type rule
        op_status exec_c_probe(frame& f, const vm_instruction& ins);
        op_status exec_c_args(frame& f, const vm_instruction& ins);
        op_status exec_c_call(frame& f, const vm_instruction& ins);
        op_status cells_land(frame& f, const chunk::cell_loop& loop, const chunk::cell_loop::cell_call& cc);
        // after an effect point inside the block: what user code could have reached is checked or re-read; false = a miss. raw: the call entered a
        // frame program raw (by-value scalar parameters bind nothing of the caller's, so the arguments' slots need no check)
        bool cells_reland(frame& f, const chunk::cell_loop& loop, const chunk::cell_loop::cell_call& cc, bool raw = false, bool args_flushed = true);   // args_flushed: after the call (C_ARGS wrote the argument slots), not at the probe
        // raw typed frames: the arguments instruction of a raw call site enters the parked callee's
        // frame program raw when it has one whose parameter kinds are the arguments' kinds (false: the general door runs); a frame program's return
        // lands raw in a raw caller's cell, else boxes through its return pad. Both switch the block loop's frame through raw_switch_
        // in_loop: the frame-program loop follows the callee itself (*in_loop names it; raw_switch_ untouched)
        bool raw_call_enter(frame& f, const chunk::cell_loop& loop, const vm_instruction& ins, op_status& st, frame** in_loop = nullptr);
        const script_value* raw_env_value(const chunk& body, const chunk::cell_loop& loop, uint32_t k) const;
        static constexpr size_t k_raw_small_cells = 16;   // a small program's cells clear with fixed-size stores (the vectors keep at least this many)
        frame& raw_push(frame& f, const chunk::cell_loop& loop, uint32_t index, const chunk::cell_loop::call_plan& plan);
        void raw_pop(call_record& rec);
        // the frame-program loop's raw call and return when nothing is left to decide (out of the loop: its inlining budget is spent); nullptr
        // leaves them to raw_call_enter and exec_c_ret
        JAI_NOINLINE frame* raw_enter_lean(frame& f, const chunk::cell_loop& loop, uint32_t index);
        JAI_NOINLINE void cells_run_lean(frame*& fp, const chunk::cell_loop*& lp);
        JAI_NOINLINE frame* raw_return_lean(frame& f, uint8_t kind, int64_t bits, const chunk::cell_loop*& back_loop);
        bool raw_probe(frame& f, const chunk::cell_loop& loop, uint32_t index);   // the callee found through the site's cache: a raw-enterable frame program
        void park_raw_callee(frame& f, const chunk::cell_loop& loop);             // a raw probe's callee parked as the general probe parks it (a miss before the arguments)
        // back: a raw return that landed in a frame-program caller names it (and its loop) for the frame-program loop to follow; raw_switch_ otherwise
        op_status exec_c_ret(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr, frame*& back, const chunk::cell_loop*& back_loop);
        void materialize_raw_window(frame& f) { if (f.raw) [[unlikely]] { build_raw_window(f); } }   // a raw-entered frame's window, built when the general form first needs it
        void build_raw_window(frame& f);
        // one frame's block run; Raw: a frame program's, whose calls and returns are on the hot switch. A run that hands the block loop another
        // frame (raw_switch_) returns, and the raw chain (cells_run_raw, which holds the C++ boundary) runs that frame next. Each loop is its own
        // function (the block loop's code generation is the scalar ops' alone)
        template <bool Raw>
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status cells_run_frame(frame*& fp, const chunk::cell_loop& entry_loop);
        // a raw switch the running frame-program loop takes in place: the next frame is a frame program with its cells live (fp, the loop
        // and cells_frame_ follow it); false leaves the switch to the raw chain
        bool raw_follow(frame*& fp, const chunk::cell_loop*& lp, op_status st) noexcept {
            frame* const next = raw_switch_;
            if (st == op_status::failed || !next->cells || next->cells->loop == k_invalid_u32) return false;
            const chunk::cell_loop& nl = next->code->cell_loops[next->cells->loop];
            if (!nl.frame_program) return false;
            raw_switch_ = nullptr;
            if (switch_to_ == next) { switch_to_ = nullptr; }
            fp = next; cells_frame_ = next; lp = &nl;
            return true;
        }
        op_status cells_run_raw(frame& entry, op_status st, bool run_first);
        const script_value* env_operand_value(frame& f, const chunk::cell_loop::env_operand_ref& er, const script_value** binding = nullptr);   // the binding's value (through a plain cell), or an enum member's entry; binding: what the name resolved to   // the raw chain from a block run (run_first false: the block already ran and switched)
        op_status cells_resume(frame& f, const vm_instruction& ins);   // the dispatcher's entry after a callee returned to the parked caller
        // boundary operations: the block parks for a statement the general form runs, and a
        // CELLS_RESUME or a block's back-edge entry at the statement's end resumes it; the re-read is cells_resync
        void cells_writeback(frame& f, const chunk::cell_loop& loop);   // every dirty cell a statement may read back to its slot or binding; the cells stay valid
        const chunk::cell_loop* cells_unpark(frame& f);   // a block parked for a statement ending at f.ip, resumed (f.ip its resume copy ip); nullptr = the general form continues
        bool cells_resync(frame& f, const chunk::cell_loop& loop);   // after a statement on the general form: the slot cells it changed, then the landing's checks
        bool cells_reland_effects(frame& f, const chunk::cell_loop& loop, const chunk::cell_loop::cell_call* landing = nullptr);   // cells_reland's checks after user code ran, without an argument's slot
        op_status exec_cells_resume(frame& f, const vm_instruction& ins);
        bool cells_resolve_sites(frame& f, const chunk::cell_loop& loop);   // the load and store sites' containers; false = a site the block cannot hold
        op_status exec_c_push(frame& f, const vm_instruction& ins, const cell_regs& cr);   // recv.push(v) inside a block by the push kernel's rows; a declined kind or a refused charge misses
        const script_value* cells_member_container(frame& f, const fused_operand& container, const script_array*& pin, size_t site, size_t off = SIZE_MAX, size_t env_cache = SIZE_MAX);
        class_instance* cells_member_prefix(frame& f, class_instance* inst, const member_expr* expr, size_t off);   // a nested member site's prefix instance (`p.pos` of `p.pos.x`); inst itself for a direct site; nullptr = the general form
        const script_value* cells_member_field(frame& f, class_instance* inst, uint32_t member_node, size_t site, size_t off, bool reprobe, bool prefixed = false, uint32_t* slot_out = nullptr);   // a member site's scalar field of an instance, the slot through a class-keyed cache per site   // a member of a frame-slot instance: its plain field, read once at the entry; pin = the table an element-reference base names (no store site may write it); nullptr = the general form
        bool cells_sites_hold(frame& f, const chunk::cell_loop& loop, bool env_ok, const std::vector<uint32_t>& land_sites);
        bool cells_sites_reenter(frame& f, const chunk::cell_loop& loop);   // a re-entry: every site still names the node its last resolution found
        const chunk::entry_memo_row* cells_entry_row(frame& f, const chunk::cell_loop& loop);   // the loop's block-entry row while its environment and epoch hold; nullptr = resolve
        void cells_restore_sites(frame& f, const chunk::cell_loop& loop, const chunk::entry_memo_row& row);
        void cells_save_entry_row(frame& f, const chunk::cell_loop& loop);
        std::vector<chunk::entry_memo_row>* cells_entry_rows(frame& f);
        bool cells_entry_scope_free(frame& f);   // no member scope ahead of the environment: a name the row bound cannot be a field of this call's receiver
        bool cells_rebinds_hold(frame& f, const chunk::cell_loop& loop);   // after a call: every alias live at the landing names its source table's element again
        bool cells_rebind_apply(frame& f, const chunk::cell_loop& loop, const chunk::cell_loop::rebind& rb, script_int index, bool reprobe);   // the alias's sites take the table's element at index, a plain array node, else false
        op_status exec_c_rebind(frame& f, const vm_instruction& ins);
        op_status exec_c_range(frame& f, const vm_instruction& ins);
        JAI_FORCEINLINE op_status exec_c_back_step(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);   // after a call: every site's container is the node resolved at entry, reached the same way
        JAI_FORCEINLINE void cell_flush_one(frame& f, const chunk::cell_loop& loop, uint32_t k);   // one cell's payload back to its slot
        JAI_FORCEINLINE void env_flush_one(frame& f, const chunk::cell_loop& loop, uint32_t k);    // an environment cell's payload back to the value its name resolves to
        bool cell_cmp_text(frame& f, const vm_instruction& ins, bool& truthy);   // the string lane of the fused compare over string cells
        bool cell_cmp_any(frame& f, const vm_instruction& ins, bool& truthy);   // the general kernel's numeric compare rows on cells; false = a kind it does not take
        // checked: the per-read kind checks; false for the pinned forms, whose operand cells the emission proved hold the kind
        JAI_FORCEINLINE op_status exec_c_arith(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
        JAI_FORCEINLINE op_status exec_c_cmp(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
        JAI_FORCEINLINE op_status exec_c_jcmp(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status exec_c_arith_any(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
        JAI_FORCEINLINE op_status exec_c_mov(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status exec_c_append(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
        op_status exec_c_text_append(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);   // a statement's pieces appended to a slot's string in place   // the compound plus with run-time kinds: strings in place
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status exec_c_math(frame& f, const vm_instruction& ins, const cell_regs& cr);
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status exec_c_guard(frame& f, const vm_instruction& ins, const cell_regs& cr);
        bool cell_land(frame& f, const chunk::cell_loop& loop, uint32_t dk, uint8_t kind, int64_t bits);
        // cell_land's rows that convert nothing (a compiler value its contract admits, a local already of the kind) inline; the rest is cell_land's
        bool cell_land_fast(frame& f, const chunk::cell_loop& loop, uint32_t dk, uint8_t kind, int64_t bits) {
            cell_state& cs = *f.cells;
            const chunk::cell_loop::cell& cl = loop.cells[dk];
            if (cl.temp ? (cl.kind == 0 || cl.kind == kind) : (cs.cells[dk].kind == kind && kind != 0)) {
                cs.cells[dk].bits = bits; cs.cells[dk].kind = kind; cs.dirty.set(dk);
                return true;
            }
            return cell_land(f, loop, dk, kind, bits);
        }
        bool cell_takes_string(const frame& f, const chunk::cell_loop& loop, uint32_t dk) const noexcept;
        void cell_land_string(frame& f, const chunk::cell_loop& loop, uint32_t dk, script_value&& v);
        bool cells_store_string(frame& f, const chunk::cell_loop& loop, uint32_t dk, script_value& v);
        JAI_NOINLINE bool cells_concat(frame& f, const chunk::cell_loop& loop, uint32_t dk, uint32_t left, uint32_t right, uint32_t origin);   // cold: kept out of exec_c_arith_any, whose numeric rows are hot
        bool cells_self_concat(frame& f, const chunk::cell_loop& loop, uint32_t dk, const script_string* b);
        JAI_FORCEINLINE op_status exec_c_load(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr, bool adopted);
        JAI_FORCEINLINE op_status exec_c_index_store(frame& f, const vm_instruction& ins, const cell_regs& cr);
        JAI_FORCEINLINE op_status exec_c_back(frame& f, size_t& ip, const vm_instruction& ins, const cell_regs& cr);
        void cells_flush(frame& f, const chunk::cell_loop& loop);          // dirty cells back to their slots, state cleared
        void cfor_state_push(frame& f, const counted_for_proto& p);   // the state a counted loop's general back edge expects, built as exec_cfor_prep builds it minus the initial test
        void cells_push_inner_states(frame& f, const chunk::cell_loop& loop, size_t ip);
        void refuse_container_site(frame& f, const chunk::cell_loop& loop);   // the refusal valve: mark the loop for the rest of the call
        void cells_miss(frame& f, const chunk::cell_loop& loop, uint32_t original_ip, unsigned step);
        op_status exec_typed_fallback(frame& f, const vm_instruction& orig);
        op_status exec_typed_arith(frame& f, const vm_instruction& ins);
        op_status exec_typed_cmp_jump(frame& f, const vm_instruction& ins);
        const script_value* dyn_operand(frame& f, uint32_t v);
        bool dyn_arith_fast(frame& f, const vm_instruction& ins);
        bool dyn_land(frame& f, uint32_t a, size_t kind, script_int iv, script_float fv);
        op_status exec_dyn_arith(frame& f, const vm_instruction& ins);
        op_status exec_dyn_cmp_jump(frame& f, const vm_instruction& ins);
        int fused_cmp_rows(const script_value& lv, const script_value& rv, token_type op);
        op_status exec_typed_decl_mov(frame& f, const vm_instruction& ins);
        op_status exec_typed_decl_const(frame& f, const vm_instruction& ins);
        op_status exec_typed_store_mov(frame& f, const vm_instruction& ins);
        op_status exec_typed_store_const(frame& f, const vm_instruction& ins);
        op_status exec_typed_push(frame& f, const vm_instruction& ins);
        // The value of an assignment expression is the destination's stored value
        // (`auto r = (c.n = 3.9)` is int 3). A consumed member store reads the member
        // back (through a user _get_ when one exists); a discarded one lands the rhs, which
        // nothing observes
        op_status land_member_store_result(frame& f, const script_value& object, member_expr* member, script_value&& rhs);
        // The append rows shared by op_push_fused and op_t_push: a payload the buffer takes
        // as-is lands with the kernel's memory charge; declined = run the slice kernel
        enum class push_append { appended, declined, failed };
        push_append typed_push_append(const script_value& objd, const script_value& pv);
        // op_binary_fused_temp's landing: the kernel's result lands RAW in a compiler temp
        // (temps only ever receive kernel mints, so a live temp's tag is null)
        struct temp_lane_sink {
            vm_backend* vm;
            frame& fr;
            uint32_t slot;
            void operator()(script_value&& v) { vm->frame_slot_set(fr, slot, std::move(v)); }
            // The canonical scalar stamp is what the construct path below produces (script_value's
            // int/float constructors install it), with this engine's pointer, so a payload write into such a slot is the same value
            void raw_int(script_int iv) {
                script_value* s = vm->frame_slot(fr, slot);
                if (s && s->raw_storage_index() == script_value::TYPEID_INT && s->stamped_as(vm->engine_->get_type_info_int())) {
                    s->unchecked_set_int_payload(iv);
                    return;
                }
                (*this)(script_value(iv, vm->engine_));
            }
            void raw_float(script_float fv) {
                script_value* s = vm->frame_slot(fr, slot);
                if (s && s->raw_storage_index() == script_value::TYPEID_FLOAT && s->stamped_as(vm->engine_->get_type_info_float())) {
                    s->unchecked_set_float_payload(fv);
                    return;
                }
                (*this)(script_value(fv, vm->engine_));
            }
        };
        bool typed_fold_index(frame& f, const fused_index_proto& p, script_int& index);
        bool typed_decl_read(frame& f, const fused_index_proto& p, uint32_t flags);
        // Typed element-store lane for the proven shape: a numeric value (operand or the
        // stack top) into an int/float buffer or an untyped array's same-kind element,
        // exactly the kernel's raw and in-place lanes with the operands off the frame.
        bool typed_index_store(frame& f, const fused_index_proto& p, bool no_result);
        bool typed_decl_land_int(frame& f, const fused_index_proto& p, script_int iv) { return typed_decl_land_int(f, p.decl_slot, p.decl_expect, p.decl_type, iv); }
        bool typed_decl_land_float(frame& f, const fused_index_proto& p, script_float fv) { return typed_decl_land_float(f, p.decl_slot, p.decl_expect, p.decl_type, fv); }
        bool typed_decl_land_int(frame& f, uint32_t slot, uint8_t expect, type_info* stamp, script_int iv);
        bool typed_decl_land_float(frame& f, uint32_t slot, uint8_t expect, type_info* stamp, script_float fv);
        bool typed_decl_land_element(frame& f, uint32_t slot, uint8_t expect, type_info* stamp, const script_array& node, size_t at, bool borrowed);
        // The buffer a decl lane may read an element from: an array, or a worker's borrow of a
        // captured array (its scalar slots read raw like the borrow kernel's; a string slot is
        // the main thread's handle, which the kernel copies as bytes, so `borrowed` keeps the
        // string rows off the lane). A map borrow answers nullptr
        static const script_array* lane_element_source(const script_value& v, bool& borrowed) noexcept;
        op_status exec_typed_load(frame& f, const vm_instruction& ins);
        op_status exec_index_fused(frame& f, const vm_instruction& ins);
        template <bool Lane> op_status exec_index_fused_decl(frame& f, const vm_instruction& ins);   // Lane: try the typed element read first
        op_status exec_index_store_fused(frame& f, const vm_instruction& ins);
        op_status index_fused_push(frame& f, const vm_instruction& ins);
        template <bool Lane> op_status index_fused_decl_rows(frame& f, const vm_instruction& ins);
        op_status index_store_fused_rows(frame& f, const vm_instruction& ins);
        op_status index_compound_operand_rows(frame& f, const vm_instruction& ins);
        template <op_status (vm_backend::*Rows)(frame&, const vm_instruction&)> op_status index_releasing(frame& f, const vm_instruction& ins);
        op_status exec_call_builtin(frame& f, const vm_instruction& ins);
        op_status exec_concat_append(frame& f, const vm_instruction& ins);
        op_status concat_step(const script_value& right, uint32_t hint, const void* site, bool joins_text);
        // An append site's largest length seen when its string had to grow: the next string built there reserves it at
        // its first growth, so a string rebuilt every pass grows once instead of once per capacity step
        std::unordered_map<const void*, size_t> text_growth_memo_;
        JAI_NOINLINE void grow_text_for_site(std::string& out, size_t add, const void* site);
        op_status exec_concat_operands(frame& f, const vm_instruction& ins);
        op_status exec_concat_pending(frame& f, const vm_instruction& ins);
        op_status exec_concat_store(frame& f, const vm_instruction& ins);
        op_status exec_concat_store_member(frame& f, const vm_instruction& ins);
        op_status exec_concat_store_index(frame& f, const vm_instruction& ins);
        op_status exec_text_append(frame& f, const vm_instruction& ins);
        op_status text_append_rows(frame& f, const vm_instruction& ins);
        const script_value* text_piece(frame& f, const fused_operand& piece, std::optional<script_value>& scratch, uint32_t& env_role);
        op_status text_append_general(frame& f, const text_append_proto& p, size_t next, script_value* held, script_value acc, bool has_acc, uint32_t env_role);
        script_value* text_builder_target(frame& f, const vm_instruction& ins, const script_value& snapshot, type_info*& field_tag);
        op_status exec_size_temp(frame& f, const vm_instruction& ins);
        const builtin_method* open_receiver_builtin(const call_site& site, size_t kind, uint32_t receiver_rule) const;
        static bool open_receiver_inline_fits(uint32_t inline_op, size_t kind);
        std::deque<std::vector<script_value>> builtin_arg_pool_;   // op_call_builtin's argument vectors by nesting depth: a builtin may call back into the VM, and a deeper call's vector must not move this one
        size_t builtin_arg_depth_ = 0;
        // A builtin's script callback: call_script_function's entry with the per-call work it repeats decided once
        struct callback_session final : prepared_callback {
            vm_backend* vm = nullptr;
            const script_function* callable = nullptr;
            const script_defined_function* fn = nullptr;
            std::shared_ptr<chunk> body;
            call_frame locals;
            frame f;
            cell_state cells;   // the session frame's raw area: its loops run as blocks, as a record frame's do
            size_t window_slots = 0;
            bool lazy = false;
            std::shared_ptr<environment> closure_env;   // a lazy closure's env, swapped with the caller's around each call
            // a bound method: its dispatcher and receiver; fn is the overload the argument count picks, rebound when the
            // count or the class's method epoch moves
            const script_method_dispatch* dispatch = nullptr;
            const script_value* receiver = nullptr;
            class_definition* scope_static = nullptr;   // the method's declaring class (a function session's stays null)
            method_entry method{};   // a body that needs a frame env gets its method env from it
            size_t method_argc = SIZE_MAX;
            uint64_t method_epoch = 0;
            bool method_arity_decides = false;   // the count alone picks fn: a later call with the same count skips resolution
            checked_result<script_value> call(const std::vector<script_value>& args) override {
                return dispatch ? vm->run_method_callback(*this, args) : vm->run_callback(*this, args);
            }
        };
        checked_result<script_value> run_callback(callback_session& s, const std::vector<script_value>& args);
        JAI_NOINLINE checked_result<script_value> run_method_callback(callback_session& s, const std::vector<script_value>& args);
        // a bound method session takes the overload argc picks when argc alone decides it and its body needs no frame env
        JAI_NOINLINE bool bind_method_session(callback_session& s, const std::vector<script_value>& args);
        op_status map_scalar_store(script_value& container, const script_value& key, const script_value& value, bool& handled, script_value*& stored);
        op_status map_numeric_compound(frame& f, const vm_instruction& ins, const script_value& container, script_value& target, bool& handled);
        const script_map::value_type* map_lane_find(const script_map& map, const script_value& key) const;
        script_value& map_lane_slot(script_map& map, const script_value& key) const;
        void commit_string_scratch(script_value& recv, uint16_t bi_string, uint64_t member_id);
        // The getter-shadow/field-slot probe behind every GET_MEMBER-family site cache
        // fill (slot UINT32_MAX = negative: same_as / nonpublic chain / sp-builtin name /
        // custom-getter shadow / no field slot)
        uint32_t member_read_slot_probe(class_definition* cd, member_expr* expr);
        const script_value* site_field(const chunk& code, const script_value& objd, member_expr* expr, size_t site, script_value& host_value);
        const script_value* site_field_of_holder(const chunk& code, script_value::object_holder* holder, size_t site);
        // a negative member_ic row on a bound C++ class: the member's in-place lane when the ladder would reach its accessors
        void fill_host_member_hit(class_definition* cd, const class_instance* inst, uint64_t member_id, const script_value& receiver,
                                  chunk::member_ic_entry& entry);
        // Fused-INDEX member container `base.member[...]`: the GET_MEMBER site IC at the
        // fused op's own member_ic row, member_access_value ladder for everything else
        op_status fused_member_container(frame& f, size_t site, const script_value& base,
                                         uint32_t member_node, std::optional<script_value>& scratch,
                                         const script_value*& out);
        // binary_fused_compute's int-row arithmetic VERBATIM, raw in/out (the fused-INDEX
        // raw path and its chain steps share one spelling). handled=false for
        // non-arithmetic ops — the caller falls to the boxed compute.
        op_status raw_int_arith(token_type op, script_int a, script_int b,
                                script_int& out, bool& handled);
        // op_store's exact post-pop tail, shared with op_binary_fused_store
        op_status store_popped_value(frame& f, const vm_instruction& ins, script_value value);
        op_status store_popped_value_full(frame& f, const vm_instruction& ins, script_value value);
        bool store_scalar_lane(script_value& slot, script_value& value, const vm_instruction& ins);
        bool store_ref_alias_var(frame& f, const vm_instruction& ins, script_value& value);
        // '=' whose store the slot's held object decides (detail::slot_value_assign): true = stored, its result pushed or the error raised
        bool held_object_store(script_value& storage, script_value& value, bool result_used, op_status& status, bool declared_var = false);
        bool store_this_field_scalar(frame& f, const vm_instruction& ins, script_value& value);
        bool store_this_field_handle(frame& f, const vm_instruction& ins, script_value& value);
        bool store_this_field_slot_scalar(frame& f, const vm_instruction& ins, script_value& value);
        op_status dyn_string_add(frame& f, const vm_instruction& ins, bool& landed);
        script_value* env_store_cell(frame& f, size_t cache_slot, uint64_t symbol_id);
        bool store_slot_raw(frame& f, const vm_instruction& ins, size_t kind, script_int iv, script_float fv);
        bool store_this_field_raw(frame& f, const vm_instruction& ins, size_t kind, script_int iv, script_float fv);
        op_status exec_math_kernel(size_t base, size_t argc, detail::math_fn fn);
        op_status fused_string_add(const script_value& l, const script_value& r, std::optional<script_value>& out);
        op_status binary_fused_tail(frame& f, const fused_binary_proto& p, token_type op, const script_value* lp, const script_value* rp, std::optional<script_value>& out);
        op_status binary_fused_general_rows(const fused_binary_proto& p, token_type op, const script_value* lp, const script_value* rp, std::optional<script_value>& out);
        // Superinstruction: fused comparison + jump_if_false (always retargets f.ip on success)
        op_status exec_fused_cmp_jump(frame& f, const vm_instruction& ins);
        const script_value* fused_cmp_operand(frame& f, const fused_operand& operand, size_t cache_slot);
        const script_value* fused_cmp_operand_env(frame& f, const fused_operand& operand, size_t cache_slot);
        // Superinstruction: fused binary RHS + identifier compound store
        op_status exec_compound_fused(frame& f, const vm_instruction& ins);
        // Counted-for ops: both always retarget f.ip (run() must `continue` after them)
        op_status exec_cfor_prep(frame& f, const vm_instruction& ins);
        op_status exec_cfor_back(frame& f, const vm_instruction& ins);
        bool resolve_cfor_int_operand(frame& f, const fused_operand& operand, script_int*& ptr, script_int& val);
        // Resolves an identifier operand exactly like exec_load (frame slot,
        // type-ctor names, env, this-field/bound-method/static) without pushing;
        // values that must be materialized land in scratch. nullptr = failed, with the
        // error already parked in pending_error_ (the hottest resolver: a bare pointer
        // return instead of a 56-byte checked_result per operand).
        const script_value* fused_ident_value(frame& f, const fused_operand& operand,
                                              std::optional<script_value>& scratch,
                                              size_t cache_slot, bool* observed = nullptr, bool resolving_callee = false);   // observed: the resolution ran a getter (a property reference, a bare C++ property); resolving_callee: the bare name is being called
        op_status exec_push_fused(frame& f, const vm_instruction& ins);
        op_status exec_index(frame& f, const vm_instruction& ins);
        op_status exec_index_impl(frame& f, const vm_instruction& ins);
        op_status exec_index_assign(frame& f, const vm_instruction& ins);
        op_status exec_index_compound(frame& f, uint32_t compound);
        op_status exec_index_store(frame& f, const vm_instruction& ins);
        op_status exec_index_compound_fused(frame& f, const vm_instruction& ins);
        op_status exec_index_compound_operand(frame& f, const vm_instruction& ins);
        op_status exec_math(frame& f, const vm_instruction& ins);
        op_status exec_parallel_for(frame& f, const vm_instruction& ins);
        const script_value* fused_subscript_value(frame& f, const fused_operand& operand,
                                                  std::optional<script_value>& scratch, size_t cache_slot);
        op_status exec_unary(frame& f, const vm_instruction& ins);
        op_status exec_call(frame& f, const vm_instruction& ins);
        // Callee-first probe pair (op_probe_callee / op_call_from_scratch)
        op_status exec_probe_callee(frame& f, const vm_instruction& ins, bool* observed = nullptr);
#if defined(_MSC_VER)
        __declspec(noinline)
#endif
        op_status capture_arg(frame& f, const vm_instruction& ins);   // cold: the value may still share storage, or a place binds (op_capture_arg)
        op_status exec_place_arg(frame& f, const vm_instruction& ins);
        op_status exec_call_from_scratch(frame& f, const vm_instruction& ins);
        op_status exec_sleep(frame& f, const vm_instruction& ins);
        // A function value running script calls: while a run is active a script callable enters without pinning it
        checked_result<script_value> call_value(const script_function& f, const std::vector<script_value>& args) {
            JAI_CENSUS(call_function_object);
            return engine_->active_run_pins() ? detail::call_function_value(f, args) : f(args);
        }
        op_status exec_call_from_scratch_general(frame& f, const vm_instruction& ins);
        op_status call_parked_script(frame& f, const vm_instruction& ins, const script_defined_function& fn, strong_ptr<script_function_node> pin);
        // the call paths' catch: the active exception becomes the unwinding state (out of line, no texts on the caller's
        // frame); truncate_to drops the stack first (SIZE_MAX keeps it)
        op_status call_threw(size_t truncate_to);
        bool call_host_direct(const script_function& callee, size_t argc, op_status& st);
        bool call_host_site(const call_site& site, size_t argc, op_status& st);
        void fill_host_site(const call_site& site, const script_value& callee);
        static void clear_host_site(const call_site& site) noexcept {
            site.ic_host_target = nullptr;
            site.ic_host_invoke = nullptr;
            site.ic_host_checked = false;
            if (site.ic_host_owner) { site.ic_host_owner = {}; }
        }
        uint32_t host_site_calls_ = 0;   // host callees running through call_host_site: a refill retires the site's pins while one runs
        bool push_plain_frame(frame& caller, const call_site& site, const script_defined_function& function, strong_ptr<script_function_node>& pin, size_t args_base, size_t argc);
        bool bind_plain_references(const script_defined_function& function, frame& caller, const call_site& site, size_t args_base, size_t argc);
        // Callee-first probe for member / computed callees (op_probe_member)
        op_status exec_probe_member(frame& f, const vm_instruction& ins);
        op_status push_script_frame_pinned(frame& caller,
                                               const script_defined_function& function,
                                               strong_ptr<script_function_node> pin,
                                               size_t args_base, size_t argc,
                                               const call_site* site);
        // Real TCO: a tail-flagged bare self-call reuses the live record + window in
        // place — no record push, no depth tick (no artificial depth limit)
        op_status tail_splice_frame(call_record& rec,
                                               const script_defined_function& function,
                                               strong_ptr<script_function_node> pin,
                                               size_t args_base, size_t argc,
                                               const call_site* site);
        op_status exec_func_decl(frame& f, const vm_instruction& ins);
        op_status exec_closure(frame& f, const vm_instruction& ins);
        op_status exec_bind_captures(frame& f, const vm_instruction& ins);
        // the converted form of a closure node, made at its first creation that can use it (null: the node keeps its environment)
        const closure_proto::converted_form* converted_form_of(const closure_proto& proto);
        op_status exec_destructure(frame& f, const vm_instruction& ins);
        op_status exec_extended(frame& f, const vm_instruction& ins);
        op_status exec_throw(frame& f, const vm_instruction& ins);
        op_status exec_try_push(frame& f, const vm_instruction& ins);
        op_status exec_try_pop(frame& f, const vm_instruction& ins);
        op_status exec_catch_end(frame& f, const vm_instruction& ins);
        op_status exec_case_eq(frame& f, const vm_instruction& ins);
        void exec_switch_table(frame& f, const vm_instruction& ins);
        bool build_switch_table(frame& f, const chunk::switch_table& t);
        // the table holds for the names' environment, the program-scope consts and the definitions as they stand (custom numeric operators,
        // the debugger and a worker run the case tests instead)
        bool switch_table_ready(frame& f, const chunk::switch_table& t) {
            if (has_custom_numeric_ops_ || debug_hook_ || parallel_worker_) return false;
            if (t.built && t.constant_keys) return t.usable;
            if (t.built && switch_const_epoch_ && t.env == environment_.get() && t.env_serial == env_symbolizer_->env_epoch() &&
                t.const_epoch == *switch_const_epoch_ && t.definition_epoch == *switch_definition_epoch_) return t.usable;
            return build_switch_table(f, t);
        }
        const uint64_t* switch_const_epoch_ = nullptr;        // the engine's counters (engine::const_epoch_counter), taken at the first table build
        const uint64_t* switch_definition_epoch_ = nullptr;
        static uint32_t switch_case_of(const chunk::switch_table& t, script_int v) {
            const uint64_t at = static_cast<uint64_t>(v) - static_cast<uint64_t>(t.low);
            return at < t.dense.size() ? t.dense[at] : k_invalid_u32;
        }
        op_status exec_iter_init(frame& f, const vm_instruction& ins);
        op_status exec_iter_next(frame& f, const vm_instruction& ins);
        op_status iter_next_map_entry(frame& f, const iter_proto& proto, iter_state& state);
        op_status exec_iter_entry(frame& f, const vm_instruction& ins);
        op_status exec_iter_pop(frame& f, const vm_instruction& ins);
        void release_iter_slots(frame& f, size_t from);
        void exec_yield(frame& f, const vm_instruction& ins);
        op_status exec_include(frame& f, const vm_instruction& ins);
        op_status exec_import(frame& f, const vm_instruction& ins);
        op_status exec_ref_return_bind(frame& f, const vm_instruction& ins);
        op_status exec_ref_return_lvalue(frame& f, const vm_instruction& ins);
        std::optional<script_value> binding_arm_storage(frame& f, const expression* e);
        op_status exec_ref_target_store(frame& f, const vm_instruction& ins);
        op_status exec_const_guard(frame& f, const vm_instruction& ins);
        // scoped const folding
        struct fold_site {
            chunk* code = nullptr;
            std::shared_ptr<chunk> pin;   // a chunk the cache does not hold
            uint32_t ip = 0;
            uint32_t const_index = 0;
            vm_instruction orig{opcode::op_halt, 0, 0, 0};
            const member_expr* expr = nullptr;
            bool prefolded = false;   // folded at a region barrier (a worker's chunk): an enum name reads the program scope's
        };
        std::vector<fold_site> fold_sites_;
        std::unordered_set<const chunk*> body_chunks_;   // chunk_for_body's chunks: pinned by the cache for the backend's lifetime
        std::unordered_set<const chunk*> prefolded_chunks_;
        size_t cache_inserts_total_ = 0;   // every entry ever added (the prefold scan's trigger: entries also leave)
        size_t cache_inserts_since_retire_ = 0;   // chunk, expression and declaration cache entries added since retire_unreachable_code last ran
        size_t cache_size_after_retire_ = 0;
        // suspended fibers that parked a callee (a site pin may be its only owner) and suspended runs that hold frames; shared with
        // the states, which may outlive the backend
        std::shared_ptr<size_t> parked_fibers_ = std::make_shared<size_t>(0);
        std::shared_ptr<size_t> parked_runs_ = std::make_shared<size_t>(0);
        uint64_t prefold_epoch_ = 0;
        size_t prefold_cached_ = 0;
        std::optional<script_value> folded_static_value(const member_expr* expr, bool prefold = false);
        void fold_static_read(frame& f, const vm_instruction& ins, const member_expr* expr, const script_value& out);
        bool prefold_chunk(const std::shared_ptr<chunk>& code);
    public:
        void revalidate_folds();
        // A worker's chunks, at the region barrier: every scoped const read (E::v, C::K, ns::x) folds before the region runs, and the
        // folds already made follow a definition that ran since (a worker cannot fold in the region: its symbols are frozen)
        void prefold_parallel_reads(uint64_t fold_epoch);
    private:
        op_status non_lvalue_target(const expression* target);
        op_status exec_cast(frame& f, const vm_instruction& ins);

        vm_coroutine_state& coroutine_fiber_state(coroutine_handle& handle);
        // Runs (or continues) the fiber; returns the yield value, the final return value on the
        // completing resume, or an error (handle marked failed) — never a stale prior yield.
        checked_result<script_value> run_fiber(coroutine_handle& handle, vm_coroutine_state& state);
        // Walks try records innermost-out: false = no handler in this frame, propagate
        bool unwind_to_handler(frame& f, const error_propagator* failure);
        // Both walk outward over in-loop frames down to records_base, popping as they go
        bool handle_op_error(frame*& fp, size_t records_base);
        bool handle_throw_unwind(frame*& fp, size_t records_base);
        op_status exec_this(frame& f, const vm_instruction& ins);
        op_status exec_field_init(frame& f, const vm_instruction& ins);
        op_status exec_ctor_base(frame& f, const vm_instruction& ins);
        op_status exec_ctor_delegate(frame& f, const vm_instruction& ins);
        op_status exec_ctor_done(frame& f, const vm_instruction& ins);
        op_status exec_ctor_literals(frame& f, const vm_instruction& ins);
        op_status exec_ctor_scope(frame& f, const vm_instruction& ins);
        void leave_constructor_frame(call_record& rec);
        op_status push_constructor_frame(frame& f, const std::shared_ptr<script_class_definition>& level, const std::shared_ptr<function_decl>& ctor,
                                         size_t argc, const call_site* site, bool answer_as_level);
        op_status exec_super(frame& f, const vm_instruction& ins);
        op_status exec_from_this(frame& f, const vm_instruction& ins);
        op_status exec_get_member(frame& f, const vm_instruction& ins);
        JAI_FORCEINLINE op_status exec_get_member_at(frame& f, const vm_instruction& ins, size_t ip);   // ip: the member_ic row (a folded member temp's own)
        op_status exec_get_member_this(frame& f, const vm_instruction& ins);
        op_status exec_get_static(frame& f, const vm_instruction& ins);
        op_status exec_set_member(frame& f, const vm_instruction& ins);
        op_status exec_set_static(frame& f, const vm_instruction& ins);
        op_status exec_member_compound(frame& f, const vm_instruction& ins, size_t ins_index);   // ins_index: the member_ic row (ins may be a temporary)
        op_status exec_call_method(frame& f, const vm_instruction& ins);
        op_status exec_call_method_general(frame& f, const vm_instruction& ins);
        op_status exec_new(frame& f, const vm_instruction& ins);
        op_status exec_class_decl(frame& f, const vm_instruction& ins);
        op_status exec_namespace_decl(frame& f, const vm_instruction& ins);
        op_status exec_enum_decl(frame& f, const vm_instruction& ins);
        op_status exec_extern_decl(frame& f, const vm_instruction& ins);

        frame* raw_switch_ = nullptr;    // the frame a raw call or a raw return handed the block loop
        frame* cells_frame_ = nullptr;   // the frame the innermost block loop runs (a C++ exception converts there)
        // the raw epoch: advanced wherever general code may have run before the next instruction of a block that can call raw (its entry from
        // the dispatcher, its door calls, a callee getter or an environment load at its doors) and when the debugger's hook changes. A call site's
        // plan holds for the epoch that validated it (a raw chain starts at such a block, so no plan outlives general code), and a raw return
        // re-validates its caller when the epoch moved during the raw call. Odd on the main backend, even on a worker (never equal: a worker
        // never plans, and a plan written by the main thread never matches a worker's epoch)
        uint64_t raw_epoch_ = 1;
        cell_counters cells_counters_{};   // entries, boundaries, resumes, misses (engine::cells_counters)
        void raw_epoch_advance() { raw_epoch_ += 2; }
        std::vector<strong_ptr<script_function_node>> retired_callees_;   // site callees replaced while a borrowed call of theirs was live
        std::vector<std::unique_ptr<callback_session>> callback_sessions_;   // by nesting depth: frames_ holds &f while a call runs
        size_t callback_depth_ = 0;
        // the native-entry frames' raw areas, by nesting depth (a parallel region's chunk body, an opaque call's frame): their loops run as
        // blocks, as a record frame's do
        std::vector<std::unique_ptr<cell_state>> entry_areas_;
        size_t entry_areas_top_ = 0;
        // engine::vm_reference_mode, read when the backend is built and on cold paths only (last, so no hot member moves): the
        // cache gate stays null (no inline cache fills), a worker pins no cache record, no scoped const folds
        bool reference_mode_ = false;
    };

} // namespace jai::vm

#endif // __JAISCRIPT_VM_VM_BACKEND_HPP__
