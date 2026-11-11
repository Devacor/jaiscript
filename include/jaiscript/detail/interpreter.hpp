#pragma once

#ifndef __JAISCRIPT_DETAIL_INTERPRETER_HPP__
#define __JAISCRIPT_DETAIL_INTERPRETER_HPP__

// NOTE: the compile-time integer-overflow policy (kCheckedOverflow and the
// jai::ints helpers) lives in <jaiscript/detail/integer_ops.hpp>, which the
// engine .cpp files include directly. It is intentionally kept out of this
// widely-included header so its <intrin.h> dependency does not bloat consumer
// translation units.

#include "ast.hpp"
#include "null_member_diagnostic.hpp"
#include "arity_diagnostic.hpp"
#include "slot_site.hpp"
#include "string_symbolizer.hpp"
#include "environment.hpp"
#include "member_scope.hpp"
#include "builtin_methods.hpp"
#include "execution_limits.hpp"
#include "native_stack.hpp"
#include "operator_table.hpp"
#include "value_text.hpp"
#include "truthiness.hpp"
#include "operator_ids.hpp"
#include <jaiscript/core/value.hpp>
#include <jaiscript/core/coroutine.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/core/types.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>
#include <system_error>
#include <optional>
#include <chrono>
#include <atomic>

namespace jai {

    // Forward declarations
    class script_class_definition;
    class class_definition;
    class class_instance;
    namespace detail { struct namespace_store_target; struct aggregate_plan; struct construction_scope; }
    namespace debug { class controller; }

    // Forward declare interpreter for scoped_method_environment
    class interpreter;

    // RAII wrapper for method environments to ensure proper cleanup
    // Automatically defines 'this' and releases the environment with clear=true on destruction
    class scoped_method_environment {
    public:
        scoped_method_environment(
            interpreter* interp,
            std::shared_ptr<environment> parent,
            const script_value& this_obj,
            class_definition* access_ctx = nullptr);

        ~scoped_method_environment();

        // Get the managed environment
        std::shared_ptr<environment> get() const { return env_; }

        // Implicit conversion to environment for convenience
        operator std::shared_ptr<environment>() const { return env_; }

        // Prevent copying
        scoped_method_environment(const scoped_method_environment&) = delete;
        scoped_method_environment& operator=(const scoped_method_environment&) = delete;

    private:
        interpreter* interp_;
        std::shared_ptr<environment> env_;
    };

    using stack_trace_entry = stack_frame;

    // Interpreter-shaped coroutine continuation state, stored opaquely on the handle.
    struct interpreter_coroutine_state : coroutine_backend_state {
        // A breadcrumb recording where we were in the AST when yield fired.
        // Built bottom-up during yield unwind, consumed top-down (LIFO) during resume.
        struct continuation_point {
            ast_node* node;   // which block/loop/if we were inside
            size_t index;     // statement index (blocks), iteration (range-for), branch (if: 0=then,1=else)
            std::shared_ptr<environment> saved_env;  // environment to restore on resume (may be null)
            // range-for only: the evaluated container (array/map/inner coroutine handle),
            // so resume continues the SAME iteration instead of re-evaluating the
            // container expression (which would re-mint a fresh inner coroutine)
            std::optional<script_value> saved_container;
            // Expression replay: which evaluation path of the node suspended (replay_phase) and
            // the operands it had produced before the suspended child, in evaluation order;
            // for those frames `index` is the suspended child's ordinal
            uint32_t phase = 0;
            std::vector<script_value> partial;
        };

        std::shared_ptr<environment> saved_environment;
        std::vector<call_frame> saved_call_stack;
        std::optional<script_value> saved_return_value;
        bool saved_has_return = false;
        // The yield-point member scope rides the suspension exactly like the env
        // chain (call_function's yield path skips cleanup, so the ordinary
        // save/restore never runs for a suspending body).
        detail::member_scope_ctx saved_member_scope{};
        // The exception trio is coroutine state too (vm run_fiber parity): a yield inside a
        // catch keeps the caught exception (bare throw;) and the in-catch marker across the
        // resumer's own try/catch
        std::optional<script_exception> parked_exception;
        std::optional<script_value> parked_exception_value;
        uint64_t parked_catch_marker = 0;
        // The body's function object outlives its first resume: the saved frames point at
        // it across every later resume (return-site conversion, debugger locals)
        std::optional<script_defined_function> function;
        engine* eng = nullptr;

        // A coroutine dropped while suspended: its frames die here, innermost first, then the cells they shared are checked for
        // cycles; a handler it suspended in holds its thrown value only through its catch variable (interpreter.cpp)
        ~interpreter_coroutine_state() override;

        void push_continuation(ast_node* node, size_t index,
                               std::shared_ptr<environment> env = nullptr,
                               std::optional<script_value> container = std::nullopt) {
            continuations_.push_back({node, index, std::move(env), std::move(container)});
        }
        void push_replay(ast_node* node, uint32_t phase, size_t index,
                         std::vector<script_value> partial = {},
                         std::shared_ptr<environment> env = nullptr,
                         std::optional<script_value> container = std::nullopt) {
            continuations_.push_back({node, index, std::move(env), std::move(container), phase, std::move(partial)});
        }
        // The innermost frame pushed so far (the node that owns the suspended child)
        ast_node* back_node() const { return continuations_.empty() ? nullptr : continuations_.back().node; }
        continuation_point* peek_continuation(ast_node* node) {
            if (!continuations_.empty() && continuations_.back().node == node) {
                return &continuations_.back();
            }
            return nullptr;
        }
        void pop_continuation() {
            if (!continuations_.empty()) {
                continuations_.pop_back();
            }
        }
        bool has_continuations() const { return !continuations_.empty(); }
        void drop_saved_containers(uint32_t phase) {
            for (auto& point : continuations_) { if (point.phase == phase) { point.saved_container.reset(); } }
        }

    private:
        std::vector<continuation_point> continuations_;
    };

    // The interpreter implements the visitor pattern to execute the AST
    class interpreter final : public execution_backend {
    public:
		checked_result<bool> language_less(const script_value& a_left, const script_value& a_right) override;
		checked_result<script_value> array_transform(uint64_t a_member, script_value& a_self, const std::vector<script_value>& a_args) override;
		checked_result<script_value> map_transform(uint64_t a_member, script_value& a_self, const std::vector<script_value>& a_args) override;
		bool native_less_available() const override {
			return !operator_table_ || !operator_table_->entry(detail::op_slot::less);
		}

        using class_lookup_callback = std::function<std::shared_ptr<class_definition>(const std::string&)>;

        // Method type for built-in type methods (shared registries, see detail/builtin_methods.hpp)
        using builtin_method = ::jai::builtin_method;

        interpreter();
        interpreter(string_symbolizer* external_symbolizer);
        interpreter(string_symbolizer* external_symbolizer, std::shared_ptr<environment> global_env);
        ~interpreter() = default;
        
        // Add a single global variable
        void add_global(const std::string& name, const script_value& value);
        
        // Prepare interpreter for a new execution (clears transient state)
        void prepare_for_execution() override;

        // ===== Execution budget (engine::execution_budget) =====
        // A top-level execute()/next() that runs past the armed deadline raises a
        // catchable runtime error. Ticked at loop back-edges and call entry; the clock
        // is only sampled every 1024 ticks so the hot path pays one increment+branch.
        void set_execution_budget(std::chrono::nanoseconds budget) override {
            execution_budget_ = budget;
            if (budget.count() <= 0) { budget_active_ = false; }
        }
        std::chrono::nanoseconds execution_budget() const { return execution_budget_; }
        void extend_execution_deadline(std::chrono::nanoseconds by) override { if (budget_active_) { execution_deadline_ += by; } }
        std::chrono::steady_clock::time_point execution_deadline() const override { return budget_active_ ? execution_deadline_ : std::chrono::steady_clock::time_point{}; }

        void arm_execution_deadline() {
            budget_tick_ = 0;
            budget_active_ = execution_budget_.count() > 0;
            const auto now = std::chrono::steady_clock::now();   // one read serves the deadline and the engine's clock sample
            if (budget_active_) {
                execution_deadline_ = now + execution_budget_;
            }
            note_clock_sample(now);
        }
        void note_clock_sample(std::chrono::steady_clock::time_point now);


        // Scope management for local variables
        void push_scope();
        void pop_scope();
        void push_instance_scope() override;
        void pop_instance_scope() override;
        
        // Define a variable in the current scope
        void define_variable(const std::string& name, const script_value& value) override;
        
        // Set class lookup callback (for finding C++ classes)
        void set_class_lookup_callback(class_lookup_callback callback) override {
            class_lookup_callback_ = std::move(callback);
        }
        
        // Set engine pointer for script_value creation
        void set_engine_reference(engine* eng) override {
            engine_ = eng;
            if (environment_) { environment_->bind_engine(eng); }
        }

        // Wire (or clear) the step-debugger. The statement hook itself tests only the
        // plain cached gate debug_hook_, refreshed from this atomic at the debug sync
        // points (sync_debug_hook).
        void set_debug_controller(debug::controller* c) override {
            debugger_.store(c, std::memory_order_release);
        }

        // Get engine pointer (for internal use in lambdas)
        engine* get_engine() const {
            return engine_;
        }

        // Helper to create script_value with engine context
        script_value make_value(script_int i) const { return script_value(i, engine_); }
        script_value make_value(script_float f) const { return script_value(f, engine_); }
        script_value make_value(const script_string& s) const { return script_value(s, engine_); }
        script_value make_value(script_string&& s) const { return script_value(std::move(s), engine_); }
        script_value make_value(script_bool b) const { return script_value(b, engine_); }
        script_value make_value(script_char c) const { return script_value(c, engine_); }
        script_value make_value() const { return script_value(std::monostate{}, engine_); }

        // AST literals carry no engine/type_info; rebuild a real value before any slow-path
        // consumer sees them (operator-method args get cloned during param bind).
        // A string literal's value: the engine that parsed the program shares the AST
        // node (copies share string storage everywhere else; a write unshares); a parallel
        // worker copies the bytes, because the node's refcount is not atomic and the main
        // engine and the other workers hold the same literal
        script_value literal_string_value(const strong_ptr<script_string>& node) const {
            return parallel_worker_ ? script_value(*node, engine_) : script_value(node, engine_);
        }

        // Execute a list of declarations and return the last value
        script_value execute(const std::vector<declaration_ptr>& declarations) override { return execute(declarations, false); }
        script_value execute(const std::vector<declaration_ptr>& declarations, bool textual);   // textual: an include, run where it is written
        using execution_backend::execute;
        script_value execute_textual(const std::vector<declaration_ptr>& declarations, std::shared_ptr<void>&) override { return execute(declarations, true); }
        std::string get_backend_name() const override { return "interpreter"; }
        // the unit of the running code, derived from the executing call frame, and the
        // re-alignment of the symbolizer's ambient with it (a store only while a name has versions)
        void sync_unit() noexcept override { if (!parallel_worker_ && string_symbolizer_->shadow_count() != 0) { string_symbolizer_->set_current_unit(current_unit()); } }
        
        // Execute a single expression and return its value
        script_value evaluate(expression_ptr expr);
        
        // Variable access methods
        script_value get_variable(const std::string& name) const override;
        bool has_variable(const std::string& name) const override;
        std::unordered_map<std::string_view, script_value> get_all_variables() const;
        
        // Exception handling methods
        bool is_unwinding() const override { return is_unwinding_; }
        const script_exception& get_current_exception() const override { return current_exception_.value(); }

        // Host-boundary discriminator (same rule as resume_coroutine's host-level
        // detection): false only when no execute is in flight (counted - nested
        // executes decrement without clearing the outer run) and no frames are live
        bool is_executing() const override { return execute_depth_ != 0 || current_call_depth_ != 0; }

        std::vector<stack_frame> last_stack_trace() const override { return captured_trace_; }
        std::string format_stack_trace() const override;
        std::string current_source_file() const override;
        void park_nested_failure() override;

        // Resume a suspended (or start a created) coroutine on this interpreter.
        // Reached via execution_backend::resume_coroutine; the handle's opaque
        // backend_state holds the interpreter_coroutine_state.
        checked_result<script_value> resume_coroutine(coroutine_handle& handle) override;

        std::unique_ptr<backend_run_state> make_run_state() override;
        void swap_run_state(backend_run_state& state) noexcept override;
        void reset_run_state(backend_run_state& state) override;
        void adopt_run_trace(backend_run_state& state) override;

        // Execute a script-defined callable payload (reached via execution_backend::execute_callable)
        checked_result<script_value> execute_callable(const script_callable& payload, const std::vector<script_value>& args) override;

        // expression visitors
        checked_result<void> visit_literal_expr(literal_expr* expr);
        checked_result<void> visit_identifier_expr(identifier_expr* expr);
        checked_result<void> visit_binary_expr(binary_expr* expr);
        checked_result<void> visit_unary_expr(unary_expr* expr);
        checked_result<void> visit_assignment_expr(assignment_expr* expr);
        checked_result<void> visit_call_expr(call_expr* expr);
        checked_result<void> visit_member_expr(member_expr* expr);
        checked_result<void> visit_lambda_expr(lambda_expr* expr);
        checked_result<void> visit_new_expr(new_expr* expr);
        checked_result<void> visit_ternary_expr(ternary_expr* expr);
        checked_result<void> visit_array_literal_expr(array_literal_expr* expr);
        checked_result<void> visit_map_literal_expr(map_literal_expr* expr);
        checked_result<void> visit_this_expr(this_expr* expr);
        checked_result<void> visit_super_expr(super_expr* expr);
        checked_result<void> visit_throw_expr(throw_expr* expr);
        checked_result<void> visit_cast_expr(cast_expr* expr);
        checked_result<void> visit_yield_expr(yield_expr* expr);

        // Coroutine expression replay: a yield unwinds as the coroutine_suspended signal,
        // each visitor on the way out parks the operands it had produced (replay_phase +
        // child ordinal), and the <true> instantiations resume from the parked child. The
        // <false> instantiations are the ordinary visitors, byte for byte.
        using replay_point = interpreter_coroutine_state::continuation_point;
        enum replay_phase : uint32_t {
            rp_none = 0,
            rp_root,           // statement: its one expression suspended
            rp_cond,           // if / while / for / switch: the condition
            rp_update,         // for: the update expression
            rp_init,           // for: the initializer declaration
            rp_container,      // range-for: the container expression
            rp_body_done,      // if / while / for / range-for: a brace-less body completed at the yield
            rp_case_value,     // switch: case[index]'s value (partial[0] = the switch value)
            rp_case_body,      // switch: case[index]'s body (saved_env = its scope)
            rp_default_body,   // switch: the default body (saved_env = its scope)
            rp_catch,          // try: inside the catch block (saved_container = the caught value)
            rp_operand,        // ordinal-only expression nodes
            rp_call_math, rp_call_string, rp_call_direct, rp_call_generic,
            rp_new_weak, rp_new_shared, rp_new_class,
            rp_asg_rhs, rp_asg_member_compound, rp_asg_sub_fast, rp_asg_general, rp_asg_regular,
        };
        template<bool Replaying> checked_result<void> visit_binary_expr_t(binary_expr* expr, const replay_point* rs);
        template<bool Replaying> checked_result<void> visit_unary_expr_t(unary_expr* expr, const replay_point* rs);
        template<bool Replaying> checked_result<void> visit_assignment_expr_t(assignment_expr* expr, const replay_point* rs);
        template<bool Replaying> checked_result<void> visit_call_expr_t(call_expr* expr, const replay_point* rs);
        template<bool Replaying> checked_result<void> visit_member_expr_t(member_expr* expr, const replay_point* rs);
        template<bool Replaying> checked_result<void> visit_new_expr_t(new_expr* expr, const replay_point* rs);
        template<bool Replaying> checked_result<void> visit_ternary_expr_t(ternary_expr* expr, const replay_point* rs);
        template<bool Replaying> checked_result<void> visit_array_literal_expr_t(array_literal_expr* expr, const replay_point* rs);
        template<bool Replaying> checked_result<void> visit_map_literal_expr_t(map_literal_expr* expr, const replay_point* rs);
        template<bool Replaying> checked_result<void> visit_cast_expr_t(cast_expr* expr, const replay_point* rs);
        template<bool Replaying> checked_result<void> visit_throw_expr_t(throw_expr* expr, const replay_point* rs);
        template<bool Replaying> checked_result<void> visit_yield_expr_t(yield_expr* expr, const replay_point* rs);
        template<bool Replaying> checked_result<void> visit_expression_stmt_t(expression_stmt* stmt);
        template<bool Replaying> checked_result<void> visit_return_stmt_t(return_stmt* stmt);
        template<bool Replaying> checked_result<void> visit_variable_decl_t(variable_decl* decl);
        template<bool Replaying> checked_result<void> visit_destructuring_decl_t(destructuring_decl* decl);
        template<bool Replaying> checked_result<void> visit_expression_decl_t(expression_decl* decl);
        // Resume side: run the suspended child of a parked node (a child with no frame is the
        // consumed yield leaf: null, its operand never re-evaluated)
        checked_result<void> replay_expression(expression* expr);
        checked_result<void> dispatch_stmt_resuming(statement* stmt);
        checked_result<void> dispatch_decl_resuming(declaration* decl);
        checked_result<void> run_case_body(ast_node* self, const std::vector<statement_ptr>& body);
        checked_result<void> store_through_subscript_reference(script_value& target_ref, script_value value);
        checked_result<void> non_lvalue_target(const expression* target);
        checked_result<void> const_guard_at(const ast_node* node, const const_site& site);
        script_value string_element_pin(script_value& base_raw, const expression* base_expr);
        // Unwind side (cold): park a node's produced operands / mark a statement's root
        void park_operands(ast_node* node, uint32_t phase, size_t index, std::vector<script_value> partial);
        void note_statement_yield(ast_node* stmt, expression* root);
        template<typename T> static bool is_yield_signal(const checked_result<T>& r) {
            return !r && r.error() == make_error_code(runtime_error_code::coroutine_suspended);
        }
        // Operand helpers: the <false> instantiations fold to the plain dispatch
        template<bool Replaying> checked_result<void> eval_child(expression* child, const replay_point* rs, size_t ordinal) {
            if constexpr (Replaying) { if (rs && rs->index == ordinal) return replay_expression(child); }
            return dispatch_expr(child);
        }
        template<bool Replaying> checked_result<void> eval_root(expression* root) {
            if constexpr (Replaying) return replay_expression(root); else return dispatch_expr(root);
        }
        template<bool Replaying> static constexpr bool parked(const replay_point* rs, size_t ordinal) {
            if constexpr (Replaying) return rs && ordinal < rs->index; else return false;
        }

        // statement visitors
        checked_result<void> visit_expression_stmt(expression_stmt* stmt);
        checked_result<void> visit_block_stmt(block_stmt* stmt);
        checked_result<void> visit_if_stmt(if_stmt* stmt);
        checked_result<void> visit_while_stmt(while_stmt* stmt);
        checked_result<void> visit_for_stmt(for_stmt* stmt);
        checked_result<void> visit_range_for_stmt(range_for_stmt* stmt);
        checked_result<void> run_for_stmt(for_stmt* stmt);
        checked_result<void> run_range_for_stmt(range_for_stmt* stmt);
        // A loop's header runs as the loop's own statement (a frame's trace line and in-scope locals), as the vm's ip has it
        void note_running_statement(statement* stmt);
        std::vector<std::pair<size_t, const for_stmt*>> updating_loops_;   // (call depth, loop) whose update is running, for the debugger's locals
        // Scope exits destroy the frame-slot locals that may own an instance, last declared first (the vm's op_kill_slot)
        void kill_scope_slot(const statement* decl);
        template <class Decls> void kill_scope_slots(const Decls& decls) {
            if (call_stack_.empty()) { return; }
            for (auto decl = decls.rbegin(); decl != decls.rend(); ++decl) { kill_scope_slot(decl->get()); }
        }
        checked_result<void> visit_parallel_for_stmt(parallel_for_stmt* stmt);
        checked_result<void> visit_return_stmt(return_stmt* stmt);
        checked_result<void> visit_break_stmt(break_stmt* stmt);
        checked_result<void> visit_continue_stmt(continue_stmt* stmt);
        checked_result<void> visit_try_stmt(try_stmt* stmt);
        void fresh_loop_binding(const for_stmt* stmt);
        checked_result<void> visit_switch_stmt(switch_stmt* stmt);
        checked_result<void> visit_case_stmt(case_stmt* stmt);
        checked_result<void> visit_default_stmt(default_stmt* stmt);
        checked_result<void> visit_fallthrough_stmt(fallthrough_stmt* stmt);

        // declaration visitors
        checked_result<void> visit_variable_decl(variable_decl* decl);
        checked_result<void> visit_function_decl(function_decl* decl);
        checked_result<void> visit_class_decl(class_decl* decl);
        checked_result<void> visit_namespace_decl(namespace_decl* decl);
        checked_result<void> visit_expression_decl(expression_decl* decl);
        checked_result<void> visit_include_decl(include_decl* decl);
        checked_result<void> visit_include_expr(include_expr* expr);
        checked_result<void> visit_import_expr(import_expr* expr);
        checked_result<void> import_step(import_site& site, const std::vector<std::string>* literal, const std::string* single,
                                         const std::vector<expression_ptr>& computed, bool force, bool yields);
        checked_result<void> include_into_value_stack(expression* path_expr_node, const std::string& literal_path);
        checked_result<void> visit_import_decl(import_decl* decl);
        checked_result<void> visit_enum_decl(enum_decl* decl);
        checked_result<void> visit_destructuring_decl(destructuring_decl* decl);
        checked_result<void> visit_extern_decl(extern_decl* decl);
        
        // Performance optimization flags
        void set_has_custom_numeric_ops(bool value) override { has_custom_numeric_ops_ = value; }
        bool has_custom_numeric_ops() const { return has_custom_numeric_ops_; }
        void set_has_custom_binary_ops(bool value) override { has_custom_binary_ops_ = value; }
        void set_operator_table(const detail::engine_operator_table* table) override { operator_table_ = table; }
        
        // Accessors for script class support
        std::shared_ptr<environment> get_current_environment() const override { return environment_; }

        // Caller context for builtins (reflect::set/invoke enforcement): a builtin call
        // pushes no script frame, so the live environment chain answers for the caller
        const class_definition* current_access_context() const override {
            return environment_ ? environment_->find_access_context() : nullptr;
        }

        std::vector<stack_frame> debug_frames() const override;
        // Debugger: named slot-locals (params + reached body decls) of a call frame, reconstructed
        // from the frame's function AST. Empty at global scope / for foreign frames.
        std::vector<std::pair<std::string, script_value>> get_frame_locals(size_t frame) const override;
        script_value* debug_local_storage(const std::string& name) override;
        std::vector<std::pair<size_t, std::string_view>> frame_scope_slots(size_t frame_index) const;
        int debug_depth() const override { return static_cast<int>(call_stack_.size()); }
        // Debug sync point (out of line: needs the controller type). Refreshes
        // debug_hook_ + the controller's hot cache, and hands a session-end a fresh
        // budget deadline (the armed one is long past after a pause).
        void sync_debug_hook() override;

        // Get the global (root) environment from the engine directly
        // Previously walked up the parent chain, but that can diverge when closures/methods
        // capture stale environment references from different execute() calls
        std::shared_ptr<environment> get_global_environment() const;

        string_symbolizer* get_string_symbolizer() const { return string_symbolizer_; }
        
        // External (C++) invocations of script functions must not see the pending
        // call-site context of whatever script call is in flight - reference params
        // would bind its variables. A one-pointer shelf replaces the old metadata stack.
        void begin_host_call() override;
        std::vector<script_value>* exchange_owned_arguments(std::vector<script_value>* args) override {
            auto* previous = const_cast<std::vector<script_value>*>(owned_args_);
            owned_args_ = args;
            return previous;
        }
        void push_external_call_scope() override {
            // A host-level callback (no script in flight) is its own budgeted entry
            if (execute_depth_ == 0 && call_stack_.empty()) { arm_execution_deadline(); }
            external_ctx_stack_.push_back(pending_call_ctx_);
            pending_call_ctx_ = nullptr;
        }
        checked_result<script_value> host_reference_argument(const std::vector<script_value>& args, size_t index, size_t offset,
                                                             host_slot& slot) override;
        checked_result<void> host_slot_store(const host_slot& slot, script_value& value) override;
        void pop_external_call_scope() override {
            if (!external_ctx_stack_.empty()) {
                pending_call_ctx_ = external_ctx_stack_.back();
                external_ctx_stack_.pop_back();
            }
        }

        // Execute a method AST with a given environment
        checked_result<script_value> execute_method_ast(std::shared_ptr<function_decl> ast,
                                      std::shared_ptr<environment> method_env,
                                      const std::vector<script_value>& args);

        // Evaluate field initializers for a script class instance at construction time
        // This should be called BEFORE the constructor body executes
        // If skip_parent_recursion is true, only evaluate THIS class's field initializers (parents already handled)
        checked_result<void> evaluate_field_initializers(strong_ptr<class_instance> instance,
                                        std::shared_ptr<script_class_definition> class_def,
                                        std::shared_ptr<environment> init_env,
                                        bool skip_parent_recursion = false,
                                        const detail::aggregate_plan* aggregate = nullptr);
        // execution_backend::apply_field_initializers
        void apply_field_initializers(const strong_ptr<class_instance>& instance,
                                      const std::shared_ptr<class_definition>& class_def) override;

        // Helper to create a bound method - binds 'this' as the first argument
        static script_value create_bound_method(const script_value& this_obj, const script_value& method);

    private:
        // Script-class construction: detail::prepare_construction, then the constructor's activation (its prologue, then its body)
        checked_result<script_value> construct_instance(std::shared_ptr<script_class_definition> class_def,
                                                        std::shared_ptr<environment> definition_env,
                                                        const std::vector<script_value>& args, bool check_access = true);
        // What the interpreter lends the construction kernel (detail/construction.hpp)
        struct construction_lender;

        void initialize();
        checked_result<void> increment_variable(unary_expr* expr);
        template<bool Replaying> checked_result<void> assign_compound_identifier(assignment_expr* expr, const replay_point* rs);
        template<bool Replaying> checked_result<void> assign_compound_member(assignment_expr* expr, const replay_point* rs);
        template<bool Replaying> checked_result<void> assign_compound_element(assignment_expr* expr, const replay_point* rs);
        checked_result<void> assign_identifier(assignment_expr* expr, script_value value);
        checked_result<script_value> compound_value(token_type a_op, const script_value& a_current, const script_value& a_right, bool a_incdec = false,
                                                    bool a_text_slot = false);

        // Optimized binary operation handlers - return checked_result for error propagation
        struct binary_host;   // this backend's hooks for detail/binary_handlers.hpp
        // a op b on evaluated operands, by the shared kernels (detail/binary_handlers.hpp)
        checked_result<script_value> binary_operator(token_type op, const script_value& left, const script_value& right);
        checked_result<script_value> join_text(const script_value& left, const script_value& right);
        checked_result<bool> language_equal(const script_value& left, const script_value& right) override;
        checked_result<script_value> invoke_method(const script_value& self, uint64_t name_id) override;
        // A function value running script calls: while a run is active a script callable enters without pinning it
        checked_result<script_value> call_value(const script_function& f, const std::vector<script_value>& args) {
            return engine_ && engine_->active_run_pins() ? detail::call_function_value(f, args) : f(args);
        }
        builtin_method_context builtin_ctx() {
            return builtin_method_context{engine_, string_symbolizer_,
                [](void* self, const script_value& a, const script_value& b) {
                    engine::run_pin pin(static_cast<interpreter*>(self)->engine_);   // a builtin's comparison is its callback
                    return static_cast<interpreter*>(self)->language_equal(a, b);
                },
                [](void* self) { return static_cast<interpreter*>(self)->is_unwinding_; }, this};
        }
        
        // Performance optimization: Skip custom operator checks for numeric types
        bool has_custom_numeric_ops_ = false;
        // ANY binary-operator / "[]" override: transient subscript reads keep minting
        bool has_custom_binary_ops_ = false;
        const detail::engine_operator_table* operator_table_ = nullptr;   // engine-owned flat dispatch
        
        // Script-defined function storage (hoisted to namespace scope in environment.hpp)
        using script_defined_function = jai::script_defined_function;


        // script_string symbolizer for variable names
        std::unique_ptr<string_symbolizer> ownedSymbolizer_;  // Only used if we own it
        string_symbolizer* string_symbolizer_;  // Points to either owned or external

        // Epoch sink handed to every environment this interpreter constructs. Equal to
        // string_symbolizer_ everywhere except a parallel worker, whose private instance
        // keeps worker env churn from bumping the shared engine env-epoch.
        string_symbolizer* env_symbolizer_ = nullptr;
        
        // Current environment for variable storage
        std::shared_ptr<environment> environment_;
        std::vector<std::shared_ptr<environment>> instance_scope_saved_;
        std::shared_ptr<environment> pending_instance_env_;

        // Current frame's member scope (detail/member_scope.hpp): stamped by
        // call_function's callee-kind branches (and the field-initializer site),
        // saved/restored around every call like environment_. Drives the C++
        // members-shadow-globals resolution order in the identifier ladders.
        detail::member_scope_ctx member_scope_{};

        // Pending call-site context (set by visit_call around the invoke, consumed by
        // the next call_function entered): ref params bind against the caller's
        // variables through it. Save/restored as a single pointer by nested calls.
        const detail::call_site_context* pending_call_ctx_ = nullptr;
        std::vector<const detail::call_site_context*> external_ctx_stack_;
        // An argument list a construction built (an initializer list's arguments, a T&-taking constructor's captured list): its
        // prvalues bind as a call site's do (KEEP BYTE-PARALLEL with vm_backend::owned_args_)
        const std::vector<script_value>* owned_args_ = nullptr;
        
        // Optimized stack for expression evaluation results
        class script_valueStack {
        private:
            std::vector<script_value> values_;
            size_t top_ = 0;
            
        public:
            script_valueStack() { 
                values_.reserve(512);  // Increased capacity for complex expressions
            }
            
            void push(script_value&& v) {
                if (top_ >= values_.size()) {
                    values_.emplace_back(std::move(v));
                    top_++;
                } else {
                    values_[top_++] = std::move(v);
                }
            }
            
            void push(const script_value& v) {
                if (top_ >= values_.size()) {
                    values_.emplace_back(v);
                    top_++;
                } else {
                    values_[top_++] = v;
                }
            }
            
            script_value& top() {
                if (top_ == 0) {
                    throw runtime_error("Internal error: empty value stack");
                }
                return values_[top_ - 1];
            }

            // Indexed peek below the top (0 = top): in-place reads for multi-arg
            // consumers (math:: intrinsics) - no pops, no moves
            script_value& peek(size_t from_top) {
                if (from_top >= top_) {
                    throw runtime_error("Internal error: value stack peek past bottom");
                }
                return values_[top_ - 1 - from_top];
            }
            
            script_value pop() {
                if (top_ == 0) {
                    throw runtime_error("Internal error: empty value stack");
                }
                script_value result = std::move(values_[--top_]);
                // Note: moved-from value is in valid but unspecified state, which is fine
                // Don't create a new null value here as it would have invalid engine_ref
                return result;
            }

            // Discard the top value (ignored return values)
            void discard() {
                if (top_ == 0) {
                    throw runtime_error("Internal error: empty value stack");
                }
                script_value released = std::move(values_[--top_]);   // a discarded value dies now, not when the slot is reused
            }

            bool empty() const { return top_ == 0; }
            size_t size() const { return top_; }

            void clear() {
                // Actually destroy the values to release references
                values_.clear();
                top_ = 0;
            }

            void swap(script_valueStack& other) noexcept {
                values_.swap(other.values_);
                std::swap(top_, other.top_);
            }
        };
        struct interpreter_run_state;   // a suspendable run's own copy of every member that execution changes (swap_run_state)
        
        script_valueStack valueStack_;
        
        // Return value storage for global scope returns
        std::optional<script_value> returnValue_;  // Use optional to avoid needing default constructor
        bool hasReturnValue_ = false;

        // Control flow flags (for break/continue without exception overhead)
        bool hasBreakRequest_ = false;
        bool hasContinueRequest_ = false;

        // Execute-nesting depth (a COUNTER, not a flag: a nested C++
        // execute() on the same engine must not clear the outer run's in-flight
        // state on exit). Non-zero while any execute() runs a program - a reentrant
        // execute (host callback mid-script, e.g. hot reload) isolates itself at
        // top level and must not reset the environment pool the outer run still uses
        int execute_depth_ = 0;
        // evaluation's own recursion (nesting within one call) stops here; a recursion of calls meets native_stack_low first
        detail::native_stack_mark stack_mark_;
        void arm_native_stack() noexcept { if (!is_executing()) { stack_mark_.arm(); } }

        // Coroutine support
        coroutine_handle* active_coroutine_ = nullptr;
        bool hasYieldRequest_ = false;

        // Exception handling state
        std::optional<script_exception> current_exception_;
        bool is_unwinding_ = false;
        std::optional<script_value> active_exception_value_;  // The exception message as a script_value (optional)
        // A C++ exception raised under a call becomes the script exception its text throws; the call yields null
        checked_result<void> raise_native_exception(const std::exception& e);

        std::vector<stack_trace_entry> captured_trace_;
        bool trace_captured_ = false;
        const ast_node* top_level_node_ = nullptr;
        void capture_stack_trace();
        static stack_frame call_frame_trace(const call_frame& frame);
        // A construction level's initializer-list argument or field initializer under evaluation: a frame of the trace named for
        // its class, above the `depth` call frames below it (the vm runs that code in the level's own frame)
        struct construction_trace_frame {
            std::string_view level;
            const ast_node* node;
            size_t depth;
        };
        std::vector<construction_trace_frame> construction_traces_;
        // A constructor's activation (construction_lender::run_constructor): its prologue runs in the frame the call pushes at `frame`
        // for `body`, once the parameters are bound
        struct constructor_prologue_hook {
            size_t frame = SIZE_MAX;
            const block_stmt* body = nullptr;
            const std::shared_ptr<environment>* definition_env = nullptr;
            const script_value* this_value = nullptr;
            class_definition* level = nullptr;
            const function_decl* ctor = nullptr;
            void* context = nullptr;
            checked_result<void> (*run)(void* context, detail::construction_scope& scope) = nullptr;
        };
        constructor_prologue_hook* pending_prologue_ = nullptr;
        size_t prologue_frame_ = SIZE_MAX;   // the frame running a constructor's prologue
        checked_result<void> run_constructor_prologue(constructor_prologue_hook& hook, size_t frame_index, const script_defined_function& function);
        struct construction_trace_scope {
            interpreter* self;
            construction_trace_scope(interpreter* a_self, std::string_view a_level, const ast_node* a_node) : self(a_self) {
                self->construction_traces_.push_back({a_level, a_node, self->call_stack_.size()});
            }
            ~construction_trace_scope() { self->construction_traces_.pop_back(); }
            construction_trace_scope(const construction_trace_scope&) = delete;
            construction_trace_scope& operator=(const construction_trace_scope&) = delete;
        };

        std::optional<error_propagator> run_error_;   // the runtime error that ended this run's top level
        detail::nested_failure nested_failure_;
        void absorb_nested_trace();
        checked_result<void> raise_nested_failure(const std::exception& e);

        // "Inside a catch block" marker of the rethrow protocol (0 = none); the catch
        // VARIABLE is a frame slot / scope local bound by visit_try_stmt, never resolved
        // through this. Parked per coroutine with the exception pair (resume_coroutine).
        uint64_t current_catch_var_id_ = 0;

        // Switch statement control flow state
        bool in_switch_ = false;
        bool should_fallthrough_ = false;

        // True only while evaluating the outermost subscript that is the TARGET of
        // an assignment (e.g. the `m[k]` in `m[k] = v`). Map subscript needs to
        // auto-insert a slot in that case, but a plain READ (`x = m[k]`) must NOT
        // grow the map. visit_binary captures and clears this immediately so
        // nested subscripts are treated as reads.
        bool lvalue_target_context_ = false;
        // The compound-store target read (s[i] op= v): a string base mints a STRING-ELEMENT
        // reference like the `=` target does; captured and cleared with the flag above
        bool string_target_context_ = false;

        // Call depth tracking for recursion limit
        int current_call_depth_ = 0;

        // Execution budget state (see execution_budget()/arm_execution_deadline above)
        std::chrono::nanoseconds execution_budget_{std::chrono::seconds(5)};
        std::chrono::steady_clock::time_point execution_deadline_{};
        uint32_t budget_tick_ = 0;
        bool budget_active_ = false;

        // Execution-limit state (terminal-error latch). Engine-less fallback storage;
        // prepare_for_execution repoints limits_ at the engine's per-engine instance so
        // reentrant executes share it and terminal errors cross the reentrant boundary.
        detail::execution_limits local_limits_;
        detail::execution_limits* limits_ = &local_limits_;

        // ============================================================
        // CALL FRAME OPTIMIZATION
        // ============================================================
        // Stack-based call frames for fast function parameter access.
        // Parameters are stored in a simple vector for O(n) lookup where n is small (typically < 8).
        // This avoids hash map overhead for the most frequently accessed variables.
        // Closure/global variables still use environment_ for lookup.
        // NOTE: call_frame struct is defined at namespace scope in environment.hpp
        // so that coroutine_handle can reference it without circular includes.

        /// Call stack for function execution
        std::vector<call_frame> call_stack_;

        // Function call optimization pools

        // Class lookup callback for finding C++ classes
        class_lookup_callback class_lookup_callback_;

        // Cached symbol IDs for object type names (initialized in constructor)
        uint64_t namespace_function_type_id_;
        uint64_t namespace_class_type_id_;
        uint64_t namespace_identifier_type_id_;
        uint64_t class_definition_type_id_;
        uint64_t weak_ptr_holder_type_id_;
        uint64_t shared_ptr_holder_type_id_;
        uint64_t weak_from_this_id_;
        uint64_t shared_from_this_id_;
        uint64_t coroutine_handle_type_id_;
        uint64_t next_id_;
        uint64_t handle_reset_id_ = 0;
        uint64_t done_id_;
        uint64_t advance_id_ = 0;
        uint64_t result_id_ = 0;
        uint64_t same_as_id_;
        uint64_t eq_method_id_ = 0;   // "==" interned at construction (worker probes must not intern)
        uint64_t to_bool_id_ = 0;
        uint64_t to_string_id_ = 0;
        base_type_symbols base_type_ids_;   // scalar_member_message spellings, interned at construction
        detail::arity_count_symbols arity_count_ids_;   // arity text count spellings, interned at construction
        bool identifier_callee_lookup_ = false;   // visit_call_expr -> visit_identifier: the bare name is being called
        bool member_callee_lookup_ = false;   // visit_call_expr -> visit_member_expr: the member is being called (null-receiver text)
        script_value* chain_root_hold_ = nullptr;   // a read chain's outer link -> its inner links: where the innermost leaves the chain's root (held until the outer read completes)

        detail::operator_ids ops_;
        uint64_t subscript_op_id_;  // "[]"
        uint64_t assign_operator_id_;  // "=" for class assignment operator overload

        // a compound assignment's operator method (`&=` -> operator&); 0 for none
        uint64_t compound_op_symbol(token_type assign_op) const noexcept {
            switch (assign_op) {
                case token_type::plus_equal: return ops_.plus;
                case token_type::minus_equal: return ops_.minus;
                case token_type::star_equal: return ops_.star;
                case token_type::slash_equal: return ops_.slash;
                case token_type::percent_equal: return ops_.percent;
                case token_type::ampersand_equal: return ops_.ampersand;
                case token_type::pipe_equal: return ops_.pipe;
                case token_type::caret_equal: return ops_.caret;
                case token_type::left_shift_equal: return ops_.left_shift;
                case token_type::right_shift_equal: return ops_.right_shift;
                default: return 0;
            }
        }

        // The running assignment's value is consumed (it reads the destination back); false only for the root of an
        // expression statement, whose value is discarded
        bool expression_result_needed_ = true;
        const expression* unconsumed_root_ = nullptr;

        // Engine pointer for script_value creation (raw pointer - no atomic ops on copy)
        // Interpreter lifetime is managed by engine, so engine will always outlive interpreter
        engine* engine_ = nullptr;

        // Step-debugger controller (engine-owned), wired by the first engine::debugger() call, which runs before the script
        // thread starts or on it; read only at the debug sync points, where sync_debug_hook (below) mirrors it into the plain
        // gate (attach and detach reach this thread through the controller's own state).
        std::atomic<debug::controller*> debugger_{nullptr};

        // Script-thread cached statement-hook gate: non-null only while a debug session
        // is enabled. dispatch_stmt/dispatch_decl test this one plain pointer per
        // statement; when no debugger was ever constructed that test is the hook's
        // entire cost. Written ONLY by sync_debug_hook on this thread.
        debug::controller* debug_hook_ = nullptr;
        // The next dispatch is no new statement execution (set only while armed): a for initializer, whose header
        // stopped, or a statement a coroutine resume re-enters.
        const ast_node* debug_quiet_node_ = nullptr;

        // Temporary storage for 'this' value during method/constructor execution
        script_value current_method_this_;

        // ============================================================
        // SWITCH-BASED DISPATCH (faster than virtual calls)
        // ============================================================

        // Evaluate an expression using switch-based dispatch
        // Returns checked_result<void>, result is pushed to valueStack_
        [[nodiscard]] checked_result<void> dispatch_expr(expression* expr);

        // Execute a statement using switch-based dispatch
        [[nodiscard]] checked_result<void> dispatch_stmt(statement* stmt);

        // Execute a declaration using switch-based dispatch
        [[nodiscard]] checked_result<void> dispatch_decl(declaration* decl);

        // Helper to get the last evaluated value (inlined for performance)
        script_value share_boxed_slot(script_value& storage);

        inline script_value pop_value() {
            return valueStack_.pop();
        }

        inline void push_value(const script_value& value) {
            valueStack_.push(value);
        }

        inline void push_value(script_value&& value) {
            valueStack_.push(std::move(value));
        }

        // Resolve variable: slot-based O(1) first, then the scope ladder. Inside a
        // member scope (detail/member_scope.hpp) the order is C++ unqualified lookup:
        // the function's own scopes, the class's members, then the definition/global
        // chain. A member METHOD or C++ PROPERTY hit answers nullptr - it shadows (never
        // fall past the class scope to a same-named global) but only value/callee/ref-bind
        // sites mint, so pointer callers land on their existing miss paths.
        // ::name's scope: the program's; a parallel worker's is the root of its own chain, which holds its copies of the program's
        // names (KEEP BYTE-PARALLEL with vm_backend::program_scope)
        environment* program_scope() const noexcept {
            if (!parallel_worker_) { return get_global_environment().get(); }
            environment* result = environment_.get();
            while (result->parent_raw()) { result = result->parent_raw(); }
            return result;
        }
        [[nodiscard]] JAI_FORCEINLINE script_value* resolve_local_or_env(size_t slot_index, uint64_t symbol_id, bool global_only = false) noexcept {
            if (global_only) [[unlikely]] {
                // ::name - GLOBAL env only (locals, captures, member scope all skipped)
                return program_scope()->get_value_ptr(symbol_id);
            }
            if (slot_index != SIZE_MAX && !call_stack_.empty()) {
                if (auto* ptr = call_stack_.back().get_local(slot_index)) {
                    return ptr;
                }
            }
            if (member_scope_.active()) {
                return scoped_env_lookup(symbol_id);
            }
            return environment_->get_value_ptr(symbol_id);
        }

        // The cell a compound store or ++/-- on a name writes in place; null for a field of this, whose store converts through the
        // field's declared type (the value it holds carries no var marker)
        [[nodiscard]] script_value* variable_cell(const identifier_expr* identifier) noexcept {
            script_value* cell = resolve_local_or_env(identifier->slot_index, identifier->symbol_id, identifier->global_qualified);
            if (cell && !identifier->global_qualified && member_scope_.state == detail::member_scope_state::instance) {
                auto hit = detail::probe_member_scope(member_scope_.state, member_scope_.this_storage, member_scope_.statics, identifier->symbol_id, false);
                if (hit.what == detail::member_probe_hit::kind::field && hit.value_ptr == cell) { return nullptr; }
            }
            return cell;
        }

        // A reference to a bare member out of reach names the access text, never the undefined one
        checked_result<void> bare_reference_access(uint64_t symbol_id) {
            if (!member_scope_.active()) { return {}; }
            auto hit = detail::probe_member_scope(member_scope_.state, member_scope_.this_storage,
                                                  member_scope_.statics, symbol_id, /*want_methods*/ false);
            return hit ? detail::bare_member_access(hit, symbol_id, environment_->find_access_context()) : checked_result<void>{};
        }

        // The class whose code is running, for a bare static name off the member-scope ladder
        class_definition* bare_declaring_class() const noexcept {
            if (member_scope_.state == detail::member_scope_state::instance && member_scope_.statics) { return member_scope_.statics; }
            return detail::lexical_access_class(environment_.get());
        }

        // The non-slot rungs of the member-scope ladder (member_scope_.active() only).
        script_value* scoped_env_lookup(uint64_t symbol_id) noexcept {
            if (script_value* own = detail::own_scope_lookup(environment_.get(), member_scope_.floor, symbol_id)) {
                return own;
            }
            auto hit = detail::probe_member_scope(member_scope_.state, member_scope_.this_storage,
                                                  member_scope_.statics, symbol_id, /*want_methods*/ false);
            if (hit) {
                if (hit.class_def && hit.class_def->chain_has_nonpublic() &&
                    !detail::bare_member_access(hit, symbol_id, environment_->find_access_context())) [[unlikely]] {
                    return nullptr;   // out of reach: the value site raises the access text
                }
                return hit.value_ptr;   // a C++ property answers nullptr: a shadow, minted by value sites
            }
            if (detail::member_scope_has_method(member_scope_.state, member_scope_.this_storage,
                                                member_scope_.statics, symbol_id)) {
                return nullptr;
            }
            return member_scope_.floor ? member_scope_.floor->get_value_ptr(symbol_id)
                                       : environment_->get_value_ptr(symbol_id);
        }

        // The cell an identifier RECEIVER of a string builtin mutates: visit_identifier_expr's
        // ladder as a pointer (:: door, slot, member scope, env). KEEP BYTE-PARALLEL
        // with vm_backend::string_receiver_cell.
        [[nodiscard]] script_value* string_receiver_cell(const identifier_expr* ident) noexcept {
            return resolve_local_or_env(ident->slot_index, ident->symbol_id, ident->global_qualified);
        }

        // The bare-name ref-bind rung after resolve_local_or_env misses: a hybrid's C++
        // property under the current member scope mints a CPP-MEMBER / PROPERTY reference
        // (nullopt = not a property here; the caller keeps its undefined-variable text).
        // Defined in interpreter.cpp (needs detail/ref_lvalue.hpp).
        std::optional<checked_result<script_value>> bind_bare_cpp_property_ref(uint64_t symbol_id, bool global_qualified,
                                                                                environment* env = nullptr);

        // Execution-budget check (see the public execution_budget API above)
        [[nodiscard]] JAI_FORCEINLINE bool execution_budget_exhausted() noexcept {
            if (++budget_tick_ < 1024) [[likely]] { return false; }
            budget_tick_ = 0;
            // Off-cycle debug sync: the ONLY mid-run point where this thread notices a
            // debugger attach/detach or breakpoint edit (one relaxed load when no
            // debugger was ever constructed). The vm twin carries the same shape
            // (vm_backend::execution_budget_exhausted) — keep them parallel.
            if (debugger_.load(std::memory_order_relaxed) || debug_hook_) [[unlikely]] {
                sync_debug_hook();
            }
            // An armed debug session suspends the wall-clock deadline (a paused script
            // must not be killed by the act of debugging); sync_debug_hook re-arms a
            // fresh deadline when the session ends.
            const auto now = std::chrono::steady_clock::now();
            limits_->note_clock_sample(now);
            if (limits_->step_budget_spent()) [[unlikely]] { limits_->terminal_error = true; return true; }   // the clock-free bound (vm twin)
            if (!budget_active_ || debug_hook_) { return false; }
            if (now < execution_deadline_) { return false; }
            // Budget overruns are TERMINAL: no script catch may swallow a timeout
            limits_->terminal_error = true;
            return true;
        }

        // Combined limit check at loop back-edges / call entry: the budget tick plus a
        // one-compare memory high-water test (deferred raise point for charge sites
        // that cannot deny in place). KEEP BYTE-PARALLEL with the vm twin.
        [[nodiscard]] JAI_FORCEINLINE bool execution_limit_exhausted() noexcept {
            return execution_budget_exhausted() || limits_->memory_tripped();
        }

        // Cold raise twin for execution_limit_exhausted (error_propagator converts to
        // any checked_result<T> the site returns)
        [[nodiscard]] error_propagator execution_limit_failure() {
            if (limits_->memory_tripped()) {
                return detail::raise_memory_cap(*limits_);
            }
            return detail::raise_execution_budget(*limits_);
        }

        // Binary operation helpers
        [[nodiscard]] checked_result<script_value> evaluate_arithmetic(const script_value& left, token_type op, const script_value& right, int compound = -1);

        // Strong types: Type enforcement for assignment
        // Returns error if value cannot be assigned to a variable with target_type
        // - nullptr target = uninitialized (auto x;), locks to source type
        // - any_type target = dynamic (var x), allows anything
        // - locked target = enforce compatibility or convert
        // ns::x = v / op= / ++ on a namespace variable's cell (detail/namespace_scope.hpp resolves the target)
        checked_result<script_value> store_namespace_cell(const detail::namespace_store_target& target, const member_expr* member, script_value incoming);
        checked_result<script_value> converted_for_variable(const type_info_ptr& tag, script_value value, std::string_view name);
        checked_result<script_value> enforce_type_compatibility(
            script_value value,
            type_info_ptr target_type,
            std::string_view var_name = "",
            detail::slot_kind kind = detail::slot_kind::assignment
        );

        // Compound-assignment store-back (x op= rhs ≡ x = T(x op rhs)): a locked target
        // converts the promoted result through enforce_type_compatibility exactly like '=';
        // var (any-tagged) targets keep the promoted result AND their dynamic tag.
        checked_result<void> compound_typed_store_back(
            script_value& target,
            script_value promoted,
            std::string_view var_name
        );

        // Type conversion helpers (inlined for performance)
        // Helper function for object to_bool() method lookup (defined in interpreter.cpp)
        bool object_to_bool_via_method(const script_value& value);
        // A value's display text (detail::value_text) with each object's to_string() run on this backend; its error is the caller's
        checked_result<std::string> value_to_string_with_method(const script_value& val);
        checked_result<detail::own_text_verdict> object_own_text(const script_value& val, std::string& out);
        checked_result<script_value> concat_with_methods(const script_value& left, const script_value& right);
        // User operator methods (==, <, <=, >, >=, + - * / %): nullopt when the operand defines none (or the method's result
        // does not answer the question), else the method's result or the runtime error it raised
        bool object_defines_custom_equality(const script_value& v) const;
        std::optional<checked_result<script_value>> object_compound_through_binary(const script_value& target, const script_value& right, uint64_t op_symbol_id);
        std::optional<checked_result<script_value>> object_operator_in_place(const script_value& target, uint64_t base_id, const script_value& right, detail::incdec_form form);
        std::optional<checked_result<script_value>> compound_in_place(const script_value& target, const assignment_expr* expr, const script_value& right);
        std::optional<checked_result<script_value>> object_compound_operand(const script_value& left, token_type op, const script_value& right);
        std::optional<checked_result<script_value>> object_unary_via_method(const script_value& operand, token_type op);   // a class's unary - + ~
        script_value call_operator_of(const script_value& callee);   // the instance's operator() bound to it, else null
        checked_result<void> method_value_error(const member_expr* expr, const script_value& object, bool bindable);   // obj.method without a call
        // the unwinding state for a composed text, built out of line so the member paths carry no string temporaries
        void raise_script_text(const std::string& text, const source_location& location);
        void raise_no_member(std::string_view kind, std::string_view member, const source_location& location);
        checked_result<void> bare_method_value_error(const identifier_expr* expr, const script_value& self);   // a bare method name without a call
        std::optional<checked_result<void>> bare_method_reference_error(const identifier_expr* expr);   // a reference to a bare method name
        std::optional<checked_result<script_value>> named_storage_reference(expression* value);
        std::optional<checked_result<script_value>> static_storage_reference(const member_expr* member);

        // One resolution for every shape a C++/script object reaches the interpreter in:
        // class_instance wrappers (make_object, script classes), cpp_bound non-owning
        // references (T& returns), and raw C++ holders (unregistered make_object fallback).
        // Every member/method/operator path resolves through this so the shapes can't drift.
        struct member_target {
            strong_ptr<class_instance> instance;      // fields; null for cpp_bound/raw holders
            std::shared_ptr<class_definition> engine_def;  // anchors engine-resolved definitions
            class_definition* class_def = nullptr;         // methods/getters/setters/unwrap, both shapes

            explicit operator bool() const { return instance != nullptr || class_def != nullptr; }
            script_value method(uint64_t id) const;        // invalid value when absent
            script_value user_accessor(uint64_t id) const; // nearest user _get_/_set_ (synthesized skipped); invalid when absent
            bool has_field(uint64_t id) const;
            const script_value& get_field(uint64_t id) const;
            std::string_view class_name() const;           // empty when unknown
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

        // Writes 'value' to a member of 'objectValue' (deref'd, object-typed): C++ property
        // setters (including on cpp_bound receivers) take precedence over instance fields.
        // Raises a script exception (is_unwinding_) for non-existent members.
        checked_result<void> assign_member_value(const script_value& objectValue, member_expr* memberExpr, const script_value& value,
                                                 script_value* consumable = nullptr);
        // `base.x = v` on a math value: the store lands in the storage the base names (detail/math_component_store.hpp)
        checked_result<script_value> assign_math_component(script_value& base, member_expr* memberExpr, const script_value& value);
        // The member read on an already-evaluated receiver (visit_member_expr's tail; the
        // compound-member store evaluates the receiver once and reads through this)
        checked_result<void> member_read_on(member_expr* expr, script_value objectValue, bool as_callee);

        // Helper to check if an expression is an lvalue (existing object that should be cloned)
        // Lvalues: identifiers, member access, subscript access
        // Non-lvalues (temporaries): function calls, constructors, literals, operators
        bool is_lvalue_expression(expression* e) const;

        inline bool is_truthy(const script_value& value) {
            return detail::truthy_of(value, [this](const script_value& v) {return object_to_bool_via_method(v);});
        }
        
        
        // Coroutine helper - creates a script_value wrapping a coroutine_handle.
        // Static (friend access to script_value) so coroutine-factory thunks need no interpreter.
        static script_value make_coroutine_object(engine* eng, uint64_t type_id, std::shared_ptr<coroutine_handle> handle);

        // Function call helpers
        // Returns checked_result - exceptions only thrown at execute() boundary
        // method_ctx supplies the method scope pieces (definition env, receiver, access
        // class) directly for the direct-dispatch fast path - same net frame/env shape
        // as a method-env closure_env carrier, minus the carrier.
        struct method_call_override {
            const std::shared_ptr<environment>* definition_env;
            const script_value* this_obj;
            class_definition* access_ctx;
        };
        checked_result<script_value> call_function(const script_defined_function& function, const std::vector<script_value>& args,
                                                   const method_call_override* method_ctx = nullptr);
        // Stateless reference-parameter binding from the call-site arg expression,
        // factored out of call_function so its locals don't sit in the
        // per-recursion-level frame (Debug stack ceiling)
        // a by-reference argument is the caller's code: it resolves under caller_unit (the call has entered the callee's unit)
        checked_result<void> bind_reference_parameter(const parameter& param, size_t frame_index,
                                                      const expression* argExpr,
                                                      const std::shared_ptr<environment>& caller_env,
                                                      const script_value* evaluated_arg,
                                                      unit_stamp caller_unit,
                                                      const script_defined_function* callee = nullptr, size_t param_index = 0);
        // Share-or-box against the caller's variable storage (cell model)
        checked_result<bool> reference_to_storage(script_value& storage, script_value& out);
        // `v.x` where a reference binds: a COMPONENT reference (detail/math_component_store.hpp), else a non-reference
        checked_result<script_value> component_reference(const expression* e, call_frame* caller_locals, const std::shared_ptr<environment>& caller_env);
        // A reference declaration's math-component initializer as a COMPONENT reference, else the declaration's non-lvalue error
        // temporary: the initializer's value, which a `const T&` declaration owns when it names no storage
        checked_result<script_value> declared_component_reference(variable_decl* decl, script_value* temporary = nullptr);
        // C++'s category of an argument's, a capture's or a return's source expression (detail::classify_source)
        uint8_t source_class(const expression* e);
        checked_result<bool> bind_site_reference(const expression* argExpr, call_frame* caller_locals,
                                                 const std::shared_ptr<environment>& caller_env, script_value& out,
                                                 host_slot* slot_out = nullptr, size_t caller_index = 0);
        checked_result<bool> mint_argument_place(const expression* argExpr, script_value& out);
        bool later_place_argument(const script_defined_function& function, const detail::call_site_context* call_ctx, size_t from,
                                  const std::vector<script_value>& args) const;
        checked_result<void> bind_later_places(const script_defined_function& function, const detail::call_site_context* call_ctx,
                                               size_t from, std::vector<script_value>& args, size_t frame_index,
                                               const std::shared_ptr<environment>& caller_env, unit_stamp caller_unit);

        // Type conversion helpers (constructor-based conversions)
        // Attempts to convert a value to a target type using constructor-based conversion.
        // Returns the converted value on success, or the original value if no conversion is needed/possible.
        // Returns checked_result with error if conversion fails.
        [[nodiscard]] checked_result<script_value> try_convert_for_parameter(const script_value& arg, type_info_ptr target_type,
                                                                            const detail::slot_site& site = detail::slot_site{},
                                                                            script_value* consumable = nullptr);

        // Static method registries for built-in types
        // Built-in method registries (using interned IDs for O(1) lookup)
        std::unordered_map<uint64_t, builtin_method> array_methods_;
        std::unordered_map<uint64_t, builtin_method> map_methods_;
        std::unordered_map<uint64_t, builtin_method> string_methods_;
        std::unordered_map<uint64_t, builtin_method> utf8_methods_;
        std::unordered_set<uint64_t> string_mutator_ids_;   // k_string_mutator_method_names (receiver lvalue binding)
        builtin_method_registries::in_place_ids in_place_;   // the builtins whose result a call may not use (detail/in_place_result.hpp)
        std::unordered_map<uint64_t, builtin_method> weak_ptr_methods_;
        std::unordered_map<uint64_t, builtin_method> shared_ptr_methods_;

        // Initialize built-in method registries with interned method names
        void init_builtin_methods();
        
        // Helper to access value's private storage (since interpreter is a friend)
        static strong_ptr<script_array>& get_array_storage(const script_value& value) {
            return const_cast<script_value&>(value).get_array_storage();
        }

        static strong_ptr<script_map>& get_map_storage(const script_value& value) {
            return const_cast<script_value&>(value).get_map_storage();
        }
        
    public:
        checked_result<std::string> text_of(const script_value& v) override { return value_to_string_with_method(v.deref()); }

        // Parallel worker setup (parallel_transform v0): pins this instance to a worker
        // context built at the region barrier - root environment (parent = nullptr, the
        // hard partition), a private env-epoch sink, the worker's own limits, and an
        // armed budget clock. Workers never call prepare_for_execution, so nothing here
        // is ever repointed at engine-shared state.
        void configure_parallel_worker(std::shared_ptr<environment> root_env,
                                       string_symbolizer* env_epoch_sink,
                                       detail::execution_limits* worker_limits,
                                       std::chrono::nanoseconds budget) override {
            environment_ = std::move(root_env);
            env_symbolizer_ = env_epoch_sink;
            limits_ = worker_limits;
            // Slot REUSE reset (worker contexts persist across regions): a prior call
            // may have ended mid-chunk on an error - clear every piece of residual
            // execution state so the slot starts this region pristine.
            valueStack_.clear();
            call_stack_.clear();
            current_call_depth_ = 0;
            is_unwinding_ = false;
            current_exception_.reset();
            active_exception_value_.reset();
            trace_captured_ = false;
            captured_trace_.clear();
            hasBreakRequest_ = false;
            hasContinueRequest_ = false;
            hasYieldRequest_ = false;
            returnValue_.reset();
            hasReturnValue_ = false;
            pending_call_ctx_ = nullptr;
            external_ctx_stack_.clear();
            set_execution_budget(budget);
            parallel_worker_ = true;
            arm_execution_deadline();
        }

        // True on worker-configured instances only: the script-class method dispatch
        // wall (methods aren't provisioned per worker - their shared body caches and
        // arbitrary env reach are unsafe off-thread) checks this. Never set on the engine's own backend.
        bool parallel_worker_ = false;
        // Shadowing unit of the running code (string_symbolizer::current_unit): set around a call from the
        // callee's function or class, restored after; a worker interpreter never writes it (the symbolizer
        // is shared, and a worker's environments consult their private sink, never the versions)
        void enter_unit(unit_stamp unit) noexcept { if (!parallel_worker_ && string_symbolizer_->shadow_count() != 0) { string_symbolizer_->set_current_unit(unit); } }
        // derived: a scoped override at its depth (a class's field initializers, a coroutine's replay), else the top call
        // frame's function, else the ambient (top-level code: the run's stamp execute_parsed installed)
        unit_stamp current_unit() const noexcept override {
            if (unit_override_ && call_stack_.size() <= unit_override_depth_) return *unit_override_;
            if (!call_stack_.empty() && call_stack_.back().function) return call_stack_.back().function->unit;
            return is_executing() ? run_unit_ : string_symbolizer_->current_unit();   // top-level code: the run's stamp
        }
        const unit_stamp* unit_override_ = nullptr;
        size_t unit_override_depth_ = 0;
        unit_stamp run_unit_{};   // the stamp execute_parsed installed for the run in flight (execute saves and restores it around a nested run)
        // the override for a span of code without a frame of its own; the ambient re-aligned when the span ends
        struct unit_override_scope {
            interpreter* in; const unit_stamp* prev; size_t prev_depth;
            unit_override_scope(interpreter* i, const unit_stamp* unit) : in(i), prev(i->unit_override_), prev_depth(i->unit_override_depth_) {
                in->unit_override_ = unit; in->unit_override_depth_ = in->call_stack_.size(); in->enter_unit(*unit);
            }
            ~unit_override_scope() { in->unit_override_ = prev; in->unit_override_depth_ = prev_depth; in->sync_unit(); }
        };
        struct unit_sync_scope { interpreter* in; ~unit_sync_scope() { in->sync_unit(); } };   // the frame below's unit again when this scope ends
        size_t structural_equality_depth_ = 0;   // memberwise ==/<=> levels nested natively (detail/structural_equality.hpp)
        // Barrier-provisioned method pins for the active region (worker instances only)
        const detail::parallel_method_pin_table* parallel_method_pins_ = nullptr;

        void set_parallel_method_pins(const detail::parallel_method_pin_table* pins) override {
            parallel_method_pins_ = pins;
        }

        // Environment pooling functions (public: scoped_method_environment uses them)
        std::shared_ptr<environment> block_environment(std::shared_ptr<environment> parent);   // a { } / loop / case scope
        std::shared_ptr<environment> method_environment(std::shared_ptr<environment> parent, script_value this_obj, class_definition* access_ctx = nullptr);
        void end_scope(const std::shared_ptr<environment>& env);
    };
    
} // namespace jai

#endif // __JAISCRIPT_DETAIL_INTERPRETER_HPP__
