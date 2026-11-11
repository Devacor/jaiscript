#pragma once

#ifndef __JAISCRIPT_DETAIL_PARALLEL_TRANSFORM_HPP__
#define __JAISCRIPT_DETAIL_PARALLEL_TRANSFORM_HPP__

// parallel_transform.
// A body either satisfies the parallel contract or ERRORS — never silent-serial.
// Everything here constructs lazily inside the first parallel_transform call; worker
// contexts persist in an engine-owned slot pool and are RESET per call (reuse
// — rebuild only on a provisioning-fingerprint change). Outside an active region
// the engine carries exactly one extra null-pointer test on the (cold, allocation-path)
// engine::execution_limits() accessor — the zero-concurrency-cost-outside-parallel
// invariant; sequential paths never touch any of this.

#include <jaiscript/core/checked_result.hpp>
#include <jaiscript/core/value.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/execution_limits.hpp>
#include <jaiscript/detail/fault_injection.hpp>
#include <jaiscript/detail/object_borrow.hpp>
#include <jaiscript/thread_pool.hpp>
#include <map>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jai {
    class class_instance;
    class ast_node;
    class engine;
    class environment;
    class execution_backend;
    class function_decl;
    class identifier_expr;
    class statement;
    struct parameter;
    class parallel_for_stmt;
    class type_info;
    struct script_defined_function;
    class variable_decl;
    struct local_alias_home;
}

namespace jai::detail {

    class parameter_storage;

    // Thread -> per-worker limits table, installed on the engine only while a region's
    // workers are running (read-only during the region; built/torn down single-threaded).
    // engine::execution_limits() consults it so EVERY charge site — backends, shared
    // builtin methods, value clone/cell factories, environment growth — redirects to the
    // calling worker's own accounting without any per-site changes.
    // The C++ object of a serial-only class's instance that died on a region chunk, released by the join: element is the
    // element the chunk was running (SIZE_MAX outside one)
    struct deferred_release {
        size_t element = SIZE_MAX;
        script_value value;
    };
    // The join's release of every chunk's deferred objects on the calling thread: element order, then the order they died in
    void release_deferred_in_order(const std::vector<std::vector<deferred_release>*>& lists);
    // An instance a chunk constructed or copied, and what a failing destructor on a chunk reported: the join registers and reports
    // them in element order (the serial loop's), whichever worker ran the element
    struct pending_registration {
        size_t element = SIZE_MAX;
        weaker_ptr<class_instance> instance;
    };
    struct deferred_report {
        size_t element = SIZE_MAX;
        std::error_code code;
        std::string message;
    };

    // A chunk's copy of a builtin formatter and the chunk's own literal entry for it (an engine::format_literal_row)
    struct format_literal_alias {
        const void* copy = nullptr;
        const void* row = nullptr;
    };

    struct parallel_region_table {
        struct entry {
            std::thread::id id;
            execution_limits* limits = nullptr;
            // Instances this chunk constructs or copies register HERE instead of in the class registry - no lock on the worker;
            // the join registers the survivors in element order, a copy that died meanwhile just expires
            std::vector<pending_registration>* pending_registrations = nullptr;
            // A script destructor that runs on this thread runs on the chunk's own backend in its method sandbox (the barrier
            // admitted every destructor the region can run); what a failing one reports waits for the join
            execution_backend* backend = nullptr;
            const std::shared_ptr<environment>* definition_env = nullptr;
            std::vector<deferred_report>* deferred_reports = nullptr;
            // The script layers this chunk's hybrids adopt their host objects into, recorded by the join (the engine's map is shared)
            std::vector<std::pair<const void*, weaker_ptr<class_instance>>>* pending_script_layers = nullptr;
            // The C++ objects of serial-only classes that die on this chunk, released by the join; cursors name the element running
            std::vector<deferred_release>* deferred_releases = nullptr;
            const std::vector<script_value>* cursors = nullptr;
            // The shared objects this chunk reads through borrows (detail/object_borrow.hpp), swapped for handles by the join
            object_borrows* borrows = nullptr;
            // The parameter storage of the bound call this thread is in (engine::get_current_parameter_storage while a region runs)
            mutable parameter_storage* call_storage = nullptr;
            // engine::format_literal_for answers a chunk's copy of print or format with the chunk's own row
            const std::vector<format_literal_alias>* format_aliases = nullptr;
            size_t element() const noexcept { return cursors && !cursors->empty() ? cursors->front().element_cursor_index() : SIZE_MAX; }
        };
        std::vector<entry> entries;

        execution_limits* find(std::thread::id tid) const noexcept {
            for (const auto& e : entries) {
                if (e.id == tid) { return e.limits; }
            }
            return nullptr;
        }
        const entry* find_entry(std::thread::id tid) const noexcept {
            for (const auto& e : entries) {
                if (e.id == tid) { return &e; }
            }
            return nullptr;
        }

    };

    // How a captured enclosing name is provisioned into the workers, decided at the
    // barrier from the LIVE value (admission only collects the touches):
    //   scalar   - plain per-worker copy (null/int/float/char/bool; bound primitives decode)
    //   string   - per-worker detached copy (a string's or a math vector's storage is shared under plain copy)
    //   borrow   - zero-copy view of the shared container (all-primitive content AND
    //              every body touch is a subscript read - the raw-read fast tier)
    //   snapshot - one per-worker detached deep copy at the barrier (everything else:
    //              non-primitive content or non-subscript uses such as iteration,
    //              read-only methods, whole-value reads)
    // written: a fresh private copy per worker, merged back at the join; object: a class instance (or a handle to one) read
    // through a worker's own view - a detached copy of a value-class instance, a view sharing a C++ object - never written;
    // stream: a captured random, a per-worker copy drawing each element's own stream from one draw of the captured generator
    enum class parallel_capture_kind : int { scalar = 0, string = 1, borrow = 2, snapshot = 3, written = 4, object = 5, stream = 6 };

    // Amortized admission verdict for one function body (keyed by body block pointer).
    // The graph snapshot records what the body needs provisioned per worker; entries are
    // re-verified against the live global environment at every region entry (hot reload
    // swaps function values; body-pointer equality is the staleness check).
    struct parallel_admission {
        bool admitted = false;
        uint64_t generation = 0;   // stamped per build by admit_cached (0: never cached), the key of the barrier's method admission
        bool inplace_root = false;   // parallel_for body (element bound in place) vs a transform fn
        std::error_code code{};
        std::string message;   // full "parallel_transform: ..." text (owned)

        // Transitive script call targets (global name id -> admission-time payload),
        // fn itself included when named.
        std::vector<std::pair<uint64_t, std::shared_ptr<script_defined_function>>> script_functions;
        // Whitelisted host functions the bodies reference (name id + registered name).
        std::vector<std::pair<uint64_t, std::string>> host_functions;
        // Parse-time type_infos referenced by the bodies (pre-warm set: reference-of and
        // component shapes are interned at the barrier so workers never intern).
        std::vector<type_info*> referenced_types;
        // Enclosing names the bodies TOUCH in read positions (captured reads). Admission
        // collects the touches and rejects the statically-visible WRITE shapes; value
        // classification happens per region entry at the barrier (parallel_capture_kind).
        // Keyed by the interned symbol id; `name` is a zero-alloc view into the
        // symbolizer's permanent storage, used only when composing an error message.
        struct capture_entry {
            uint64_t name_id = 0;
            std::string_view name;
            bool borrow_eligible = true;   // every touch was a subscript read root
            const ast_node* bare_use = nullptr;   // a use of the value itself (not a member chain or a method receiver)
            const ast_node* member_use = nullptr;   // a member chain rooted at it: only a captured object has members
            const ast_node* qualified_use = nullptr;   // a ::name use: the program's variable, never the enclosing function's local
            bool unqualified_use = false;
        };
        // A method called on a chain rooted at a captured name (cam.len(), cam.inner.len()): the barrier refuses one that writes
        // its object, and a container's mutating builtin
        struct captured_method_call {
            uint64_t capture_id = 0;
            uint64_t name_id = 0;
            std::string_view name;
            const ast_node* site = nullptr;
        };
        std::vector<captured_method_call> captured_method_calls;
        std::vector<capture_entry> captures;
        // parallel_for's disjoint index writes: `out[i] = v` (and op=, ++, --) where out is an enclosing array and i a loop
        // variable the body never writes. Each worker writes a private copy of out; the join copies back slot out[i] of every
        // element from the worker that ran it, in element order up to the first failing element. The barrier proves the i
        // values distinct; admission proves out is touched as out[i] alone.
        struct index_write {
            uint64_t name_id = 0;
            std::string_view name;
            size_t index_param = 0;     // the loop variable's position: 0 the range's element, j the j-th zipped array's
            const ast_node* site = nullptr;
        };
        std::vector<index_write> index_writes;
        const ast_node* loop_variable_write = nullptr;   // the body's first write of its (first) loop variable
        bool index_written(uint64_t name_id) const {
            for (const auto& w : index_writes) { if (w.name_id == name_id) { return true; } }
            return false;
        }

        // Method names called on element/worker-local receivers. Admission only collects
        // (the receiver's class is a runtime fact); the barrier resolves each name against
        // the live element classes and admits or rejects the (class, method) pair — see
        // admit_element_methods. Builtin names that no element class defines are ignored.
        struct element_method_call {
            uint64_t name_id = 0;
            std::string_view name;
            const ast_node* site = nullptr;   // pinned transitively via script_functions
            uint64_t enclosing_arguments = 0;   // the argument positions any call of the name passes enclosing state at
            const ast_node* enclosing_site = nullptr;
            std::vector<std::pair<size_t, size_t>> loop_variable_arguments;   // (argument position, loop variable position)
            size_t fewest_arguments = SIZE_MAX;   // a call passing fewer binds the defaults past it
        };
        // Host static methods the bodies call, by the class name as written (Name::method(...)): pinned per worker
        struct static_call {
            uint64_t class_name_id = 0;
            uint64_t name_id = 0;
        };
        std::vector<static_call> static_calls;
        // Class constants the bodies read (Class::K, a const static of a primitive value): pinned per worker at the barrier
        std::vector<static_call> const_static_reads;
        std::vector<element_method_call> element_method_calls;
        // Script classes the bodies construct (Name(args), new Name(args), Name n(args)) with the argument count. Admission
        // only collects; the barrier resolves each name against the live class and walks every constructor, base
        // construction, field initializer and destructor the construction can run (admit_construction)
        struct construction {
            uint64_t name_id = 0;
            std::string_view name;
            size_t argc = 0;
            const ast_node* site = nullptr;
        };
        std::vector<construction> constructions;
        // Member ids the body names on chains rooted at the element (its own fields and their
        // members, any depth), recorded for both forms. parallel_for's barrier normalization
        // descends an object only through fields the body can reach: a field no worker can
        // name is never copied by one, so its shared strings may stay shared (the transform
        // reaches every element whole - its by-value binding clones the entire element).
        // element_reach_all = the element is used whole somewhere (bare read, argument,
        // method receiver, store), so every field counts.
        // element_reached_whole = chain terminals used as a VALUE or REPLACED (a copy or a
        // release touches every handle below them), so their whole subtree counts.
        std::vector<uint64_t> element_reached_members;
        std::vector<uint64_t> element_reached_whole;
        bool element_reach_all = false;
        // A store or a mutating method call through the element (member store, element
        // replacement, a container/string mutator on an element-rooted chain, an alias of it).
        // A by-value element parameter whose body neither writes through it nor uses it whole
        // (element_reach_all) binds the element IN PLACE on the worker: every remaining use is
        // a member read, so the copy is unobservable (a copy we can prove away goes).
        bool element_mutated = false;
        bool transform_cursor = false;   // the transform delivers elements through a chunk cursor (set once its chunk loop lands)
    };

    // One barrier-admitted script-class method, provisioned per worker. The method value
    // is a worker-private copy (its strong_ptr counts never cross threads), the overload
    // is pre-resolved (workers never touch shared resolution caches), and the body chunk
    // is precompiled on the worker's own backend at the barrier (no in-region compiles).
    struct parallel_method_pin {
        script_value method_value{std::monostate{}, nullptr};
        std::shared_ptr<function_decl> resolved;   // null: a host method (or accessor), called with its receiver first
        // By argument count (a bit each, the last for 63 and more): the overloads over a non-const receiver, which run only on
        // an object the chunk owns, and those bound jai::serial, whose call raises the refusal
        uint64_t mutating_arities = 0;
        uint64_t serial_arities = 0;
    };

    // (class_definition*, method name id) -> pin. Consulted by both backends' worker
    // method walls: a hit dispatches on the pinned worker-private state; a miss keeps
    // the wall error (now an admission verdict). Exact linear table: a region admits a
    // handful of methods, and the key must never collide (wrong-method dispatch).
    struct parallel_method_pin_table {
        struct entry {
            const void* cls = nullptr;
            uint64_t name_id = 0;
            parallel_method_pin pin;
        };
        std::vector<entry> entries;
        // Host static methods a body calls, by the class name as written: worker copies of the static method values
        struct static_entry {
            uint64_t class_name_id = 0;
            uint64_t name_id = 0;
            script_value value{std::monostate{}, nullptr};
        };
        std::vector<static_entry> statics;
        // The operators (and the implicit to_string/to_bool calls) of every class the region's objects can belong to, admitted at the
        // barrier like methods: a worker dispatches an operator only through its class's pin (a script operator's overloads all
        // walked, a host operator with its serial and mutating arities)
        struct operator_entry {
            const void* cls = nullptr;
            uint64_t name_id = 0;
            parallel_method_pin pin;
        };
        std::vector<operator_entry> operators;
        const parallel_method_pin* find_operator(const void* cls, uint64_t name_id) const noexcept {
            for (const auto& e : operators) {
                if (e.cls == cls && e.name_id == name_id) { return &e.pin; }
            }
            return nullptr;
        }
        // A script operator whose walk refused: the violation it raises when a worker applies it (built at the barrier)
        struct operator_refusal {
            const void* cls = nullptr;
            uint64_t name_id = 0;
            std::string text;
        };
        std::vector<operator_refusal> refused_operators;
        // The worker's method sandbox: the root a construction without a constructor initializes its fields in
        std::shared_ptr<environment> definition_env;
        const parallel_method_pin* find(const void* cls, uint64_t name_id) const noexcept {
            for (const auto& e : entries) {
                if (e.cls == cls && e.name_id == name_id) { return &e.pin; }
            }
            return nullptr;
        }
        const script_value* find_static(uint64_t class_name_id, uint64_t name_id) const noexcept {
            for (const auto& e : statics) {
                if (e.class_name_id == class_name_id && e.name_id == name_id) { return &e.value; }
            }
            return nullptr;
        }
    };

    inline constexpr const char* parallel_operator_verdict_text =
        "custom operator dispatch on class instances is not admitted in a parallel body";

    // What a worker raises for a class operator its region did not pin: the operator's own violation when the barrier walked it,
    // else the verdict. KEEP the one text both backends raise
    inline std::string_view parallel_operator_refusal(const parallel_method_pin_table* pins, const void* cls, std::initializer_list<uint64_t> ids) noexcept {
        if (pins && cls) {
            for (const uint64_t id : ids) {
                for (const auto& r : pins->refused_operators) {
                    if (r.cls == cls && r.name_id == id) { return r.text; }
                }
            }
        }
        return parallel_operator_verdict_text;
    }

    // A pinned host method's call on a chunk: a mutating one refuses a receiver the chunk did not make
    checked_result<script_value> call_parallel_host_pin(const parallel_method_pin& pin, const script_value& receiver, uint64_t name_id,
                                                        std::vector<script_value> args, engine* eng, bool property = false);
    // A pinned operator's call on self from a worker: a script operator resolves its overload without writing the class's resolution
    // cache and runs on this worker's backend; a host operator follows call_parallel_host_pin's rules
    checked_result<script_value> call_parallel_operator_pin(execution_backend& backend, const parallel_method_pin& pin, uint64_t name_id,
                                                            const script_value& self, std::vector<script_value> args, engine* eng);
    // A pinned host method as the value a member read names: its call goes through call_parallel_host_pin
    script_value parallel_bound_pin(const parallel_method_pin& pin, const script_value& receiver, uint64_t name_id, engine* eng);
    // An instance's script field a worker reads and stores in place (a C++ property goes through its pinned accessor)
    script_value* parallel_script_field(class_instance& inst, uint64_t member_id);
    // A worker's call of a method no region pin admits, on any receiver shape
    checked_result<void> parallel_method_not_admitted(uint64_t name_id);
    // What the calling chunk's method sandbox binds name_id to (the worker's own constructor of a class the region admitted), null
    // when it binds none; nullopt off a parallel chunk
    std::optional<script_value> parallel_chunk_binding(engine& eng, uint64_t name_id);
    // A conversion that constructs class_name_id on a chunk whose region did not admit the construction
    checked_result<void> parallel_conversion_not_admitted(uint64_t class_name_id);

    // Member access through an object borrow (detail/object_borrow.hpp), the one rule both backends' member reads, calls and
    // stores take: a script field of the shared instance reads as a worker-local copy (nullopt: no such script field), a host
    // member runs on the worker's view of its C++ object, a script method or any store refuses
    std::optional<checked_result<script_value>> parallel_borrow_field(const script_value& borrow, uint64_t member_id);
    // A method call on an object borrow: refused unless the barrier pinned a C++ method of the shared instance's class (it then runs
    // on the worker's view)
    checked_result<void> parallel_borrow_call(const script_value& borrow, uint64_t name_id, const parallel_method_pin_table* pins);
    const script_value* parallel_borrow_view(const script_value& borrow) noexcept;
    checked_result<script_value> parallel_shared_store_refusal(uint64_t member_id);
    checked_result<script_value> parallel_shared_host_refusal(uint64_t name_id);
    // A host call's arguments from a worker: a shared object passes as the worker's view of its C++ object (false: one has none)
    bool parallel_host_arguments(std::vector<script_value>& args);
    // A value a worker reads out of shared data, as its own: scalars as they are, strings and containers detached, objects borrowed
    script_value parallel_shared_value(const script_value& value, engine& eng);
    // The join's swap of every object borrow reachable from `root` for the shared instance's handle (single-threaded)
    void parallel_swap_borrows(script_value& root, engine& eng, std::unordered_set<const void*>& seen);

    // A reusable worker execution context (definition private to parallel_transform.cpp:
    // backend instance, root env, epoch sink, limits, provisioned callables).
    struct parallel_worker_slot;

    // Engine-owned parallel machinery. Lazily created at the first parallel_transform /
    // thread_count touch; the pool's parked workers impose no cost on any sequential
    // script path (no engine lock exists outside an active region).
    struct parallel_engine_state {
        std::unique_ptr<jai::thread_pool> pool;
        std::vector<std::pair<size_t, jai::thread_pool::task_type>> region_tasks;   // a region's submission, its storage kept
        parallel_region_table* active_region = nullptr;   // non-null only while workers run
        std::mutex instance_registry_mutex;                // class_definition instance registry, region-time only
        bool region_running = false;                       // reentrancy guard (v0: no nesting)
        // The bound C++ objects the element walk meets in elements' fields (a random, a time::date): the barrier proves each its
        // element's own by identity, naming the field when two elements share one
        struct element_host_value {
            const void* object = nullptr;
            uint64_t field = UINT64_MAX;                   // UINT64_MAX: held outside any field (an array element)
            const class_definition* cls = nullptr;
            size_t element = SIZE_MAX;
        };
        std::vector<element_host_value> element_host_values;
        uint64_t element_host_field = UINT64_MAX;          // the field the walk is inside
        uint32_t region_serial = 0;                        // the running region's number, bumped as each one starts
        // Every name a C++ level of a registered class binds (methods and accessors), refreshed when the classes change: a
        // region pins only the names it calls that are among them
        std::unordered_set<uint64_t> host_method_names;
        uint64_t host_method_names_key = UINT64_MAX;
        // The barrier's method admission (element methods, constructions, destructors, operators), kept per region shape while
        // nothing it read changed (parallel_transform.cpp's method_admission_record); program_destructors_key memoizes whether any
        // class declares a destructor
        std::shared_ptr<void> method_admissions;
        uint64_t program_destructors_key = UINT64_MAX;
        bool program_destructors = false;
        bool pending_registration_retry = false;           // exceptional join: drain before cache mutation or class reload
        fault_injection faults;                            // host/test checkpoints; disabled by default
        size_t spin_wait_us = 1000;                        // engine::parallel_spin_wait_us(us): the pool's spin-before-park budget
        std::unordered_map<const void*, parallel_admission> admission_cache;
        uint64_t admission_generation = 0;
        // parallel_transform's chunk loop (k_parallel_chunk_transform), parsed once per engine
        // and provisioned per worker: one script entry per chunk, fn called in-loop per element,
        // results emitted into the worker's output slice
        std::shared_ptr<function_decl> transform_chunk_wrapper;
        // Per fn (keyed by its body): fn's body grafted into the chunk loop - `out = fn(in)` with
        // zero calls per element - when fn only reads its parameter and returns only from its
        // top level; otherwise the call-form skeleton above. Hot reload mints a fresh body.
        std::unordered_map<const void*, std::shared_ptr<function_decl>> transform_inline_wrappers;
        std::unordered_map<const void*, std::shared_ptr<function_decl>> reduce_wrappers;   // parallel_reduce chunk loops by step body
        // parallel_reduce's region admission by step body: the step's admission (its generation) plus the chunk loop's own reads
        struct reduce_admission {
            uint64_t generation = 0;
            parallel_admission adm;
        };
        std::unordered_map<const void*, reduce_admission> reduce_admissions;
        std::string error_text;                            // owns the message a re-raise views
        std::vector<size_t> last_chunk_bounds;             // instrumentation (tests/bench)
        // Instrumentation: how the most recent region provisioned each captured name
        // (engine::last_parallel_captures) - pins borrow-vs-snapshot classification.
        // INTERNED symbol ids, never std::string (no per-region allocation).
        std::vector<std::pair<uint64_t, parallel_capture_kind>> last_captures;

        // Worker-context reuse: slots built inside a region persist and
        // are RESET per call (limits re-slice, budget re-arm, residual-state clear,
        // host-copy refresh) instead of rebuilt; a slot rebuilds only when its
        // provisioning fingerprint (backend type + admission-graph bodies) changes.
        // Slot-sets are POOLED PER REGION FINGERPRINT (fn body + backend): a frame that
        // cycles several distinct regions (GLOOM: rays, transform, glyph rows) keeps a
        // persistent context set for each instead of rebuilding every slot every call
        // (measured: 10k backend constructions per 600 ticks with the single-set pool).
        // Small LRU cap; sequential paths never see any of this - the state itself is
        // created lazily inside the first region.
        struct parallel_region_pool {
            const void* fn_body = nullptr;
            bool use_vm = false;
            uint64_t last_use = 0;
            // Last region of this statement: ns per element as the WORKER path runs it (main's
            // own chunk, timed). 0 = no region yet: the first region fans out to every thread
            // with an even split (the baseline); later regions wake only the pool workers whose
            // chunk is worth ~3x the pool's measured wake + join cost (worker_overhead_ns).
            // Chunking never changes results, so this is a pure cost knob.
            double ns_per_element = 0.0;
            std::vector<std::unique_ptr<parallel_worker_slot>> worker_slots;
            // the last prewarm's seed and the interner, symbol and class counts after it: a region that matches all four skips its prewarm
            std::vector<type_info*> prewarm_seed;
            size_t prewarm_types = 0, prewarm_symbols = 0, prewarm_classes = 0;
        };
        static constexpr size_t max_region_pools = 8;
        std::vector<parallel_region_pool> region_pools;
        // Measured once when the pool is built (three no-op round trips, the minimum): the
        // fixed ns a pool worker costs to submit, wake and join on THIS machine. 0 = not yet.
        double worker_overhead_ns = 0.0;
        uint64_t region_use_counter = 0;
        std::unordered_map<uint64_t, uint64_t> snapshot_validated;   // a region's snapshot validations by capture key, cleared per region
        cell_counters worker_cells{};   // what the workers' blocks counted, folded in at each join (engine::cells_counters adds it)

#ifdef JAISCRIPT_VM_PROFILE
        // Fork-join phase decomposition (profiling builds; dumped by the dtor): where
        // main's blocked cycles go per region. wait = the straggler window after main's
        // own chunk; wake = submit-end -> worker-chunk-start per pool worker (kernel
        // wake latency), clamped at 0 for workers that started mid-submit-loop.
        // A region's phases (region_run in parallel_transform.cpp), each timed whole
        enum class region_phase : size_t { captures, elements, methods, partition, slots, prewarm, fan_out, drain, count };
        struct region_phase_prof {
            uint64_t regions = 0;
            uint64_t phase_cyc[static_cast<size_t>(region_phase::count)] = {};
            uint64_t pins_kept = 0, pins_built = 0;   // slots whose pins were kept from the record they last ran under, or built
            uint64_t slot_resets = 0;      // slots reused for a region (the provisioning fingerprint held)
            uint64_t slot_provisions = 0;  // slots built for a region (no slot, or another body's)
            uint64_t cache_hits = 0;       // snapshot cache: content-matched reuses
            uint64_t cache_stores = 0;     // snapshot cache: records (re)stored
            uint64_t cache_misses = 0;     // snapshot cache: validate failed or no record
            uint64_t cache_unprovable = 0; // snapshot cache: shape not flat -> never cached
            std::map<std::string, uint64_t> cache_miss_names;   // miss counts by capture name
            std::map<std::string, uint64_t> cache_miss_cyc;     // detach cycles by capture name
        };
        region_phase_prof prof_transform;
        region_phase_prof prof_for;
#endif

        parallel_engine_state();
        ~parallel_engine_state();   // out of line: worker_slot is incomplete here
    };

    // Main-thread exceptional-join recovery, shared by region entry and class reload.
    void recover_pending_registrations(parallel_engine_state& state);

    // The builtin bodies (registered by the engine constructor).
    checked_result<script_value> run_parallel_transform(engine& eng, const std::vector<script_value>& args);
    // parallel_reduce(xs, init, step, combine): chunks sized by xs.size() alone fold in element order, then combine in chunk order
    checked_result<script_value> run_parallel_reduce(engine& eng, const std::vector<script_value>& args);

    // parallel_for (auto& x : arr) { body } — the in-place fork-join statement
    // (parallel_for v1). Both backends call this after evaluating the container in the
    // enclosing scope: `container` must hold (or reference) the LIVE array handle so
    // element writes land in the script-visible storage. Admission runs the same
    // fail-closed walk in "in-place root" mode (the element may bind by reference; member
    // access/stores rooted at locals are admitted in both forms); the barrier proves every
    // element safe for exclusive in-place mutation (primitives, typed primitive
    // containers, flat-value-class instances, distinct handles to them) and pre-mints the element references single-threaded. capture_env is the
    // statement's enclosing environment: captured reads resolve through its chain, except
    // the enclosing frame's own locals (parallel_for_stmt::frame_captures), which frame_slot
    // reads from the executing frame (null when the slot is not live).
    checked_result<void> run_parallel_for(engine& eng, parallel_for_stmt* stmt,
                                          std::vector<script_value>& containers,   // [0] = the range, then the zip's arrays
                                          const std::shared_ptr<environment>& capture_env,
                                          const std::function<const script_value*(size_t)>& frame_slot);

    // Subscript READ through a parallel borrow - the tier-1 raw-read path. Called by
    // BOTH backends' subscript twins when the (deref'd) receiver is a parallel borrow
    // (parity by construction; error text matches the twins' byte-for-byte, pre-formatted
    // so no worker-side intern happens against the frozen symbolizer). Primitive elements
    // mint plain worker-local values (zero refcount traffic); non-primitive elements
    // materialize through the refcount-silent deep clone (tier 3 - defensive: the barrier
    // only borrows all-primitive content). lvalue_write is the runtime write wall.
    checked_result<script_value> parallel_borrow_subscript_read(const script_value& borrow,
                                                                const script_value& index,
                                                                engine* eng,
                                                                bool lvalue_write);
    // The string a borrowed-array read would copy: the element's live bytes (a hetero string
    // element or a string-buffer slot) when the index is an int in range, else null and the
    // read runs the kernel above. Lets a worker serve repeated reads of the same table cell
    // from its own copy (validated against these bytes) instead of allocating per read.
    const script_string* parallel_borrow_string_source(const script_value& borrow, const script_value& index) noexcept;

    // The counted parallel_for's range (parallel_for_stmt::k_counted_range): __jai_parallel_range(a, b, k, comparison, step)
    // with comparison 0 <, 1 <=, 2 >, 3 >= and step 1 ++, -1 --, 2 +=, -2 -= describes the values the serial loop gives its
    // variable (parallel_counted_sequence: [start, up, size, n] as array<int>, read by run_parallel_for); the serial loop that
    // never ends or overflows its last step is refused with its text
    checked_result<script_value> parallel_counted_range(engine& eng, const std::vector<script_value>& args);

    // size(), length() and empty() with no arguments: the builtins a borrowed container answers in place (the count is frozen
    // for the region). parallel_borrow_count gives the call's result, null when the name is not one of them
    bool parallel_borrow_count_method(std::string_view name) noexcept;
    std::optional<script_value> parallel_borrow_count(const script_value& borrow, std::string_view name, size_t argc, engine* eng);
    // Any other method on a borrow (admission borrows a method receiver only for the count builtins)
    checked_result<script_value> parallel_borrow_method_refusal();

    // Read-only by-value parameter elision. Bit i of the result: the body of `fn` (and every
    // script function it calls) would be admitted in a parallel body - so it writes nothing
    // but its own locals - AND parameter i is only ever read through member chains and
    // subscripts whose static terminal is a primitive or a string, or as the receiver of a
    // read-only builtin method. Such a parameter can bind the argument by handle: no store
    // can reach the caller's value through it and no identity-observing use (comparison,
    // whole-value pass, alias, return) exists. Main thread only (interns, resolves globals).
    uint32_t prove_readonly_parameters(engine& eng, const script_defined_function& fn);
    // The argument side of the elision: a script class instance that is not nocopy (a nocopy
    // by-value pass must keep raising whatever the callee does), an array or a map. Host
    // objects keep the clone.
    bool parameter_alias_allows(const script_value& derefed);

    // Read-only by-value local elision: a function's `T x = lvalue;` declarations whose proof (the parameter proof's walk
    // over the statements after the declaration, plus: no write in that range may reach the source's object) holds bind
    // the source's object instead of a clone. stamp_local_alias_homes marks a plain function's candidate declarations
    // when it is minted; prove_local_aliases stamps every verdict of one home (main thread, no parallel region).
    void stamp_local_alias_homes(const script_defined_function& fn);
    void prove_local_aliases(engine& eng, const local_alias_home& home);
    // The declaration may bind `value` (its initializer's derefed value) without the clone: proven for the current
    // definition epoch, the value's type is exactly the declared one, and the argument-side gate admits it
    inline bool local_alias_binds(engine& eng, const variable_decl& d, const script_value& value, bool may_prove) {
        const local_alias_home* home = d.alias_home.get();
        if (!home || !eng.local_alias()) { return false; }
        const uint64_t epoch = engine_access::definition_epoch(eng);
        if (home->epoch != epoch) {
            if (!may_prove) { return false; }
            prove_local_aliases(eng, *home);
            home->epoch = epoch;
        }
        return d.alias_index < home->verdicts.size() && home->verdicts[d.alias_index] && value.get_type_info().get() == d.type.get() &&
               parameter_alias_allows(value);
    }

    // Inferred-const reference parameters (detail/ref_param_writes.hpp): the body never
    // writes through reference parameter i, callees resolved through the global
    // environment. Main thread only (resolves globals).
    // A temporary argument may bind reference parameter i (the verdict cached on the
    // parameter; a parallel worker consumes only a verdict the main thread already derived)
    bool ref_parameter_takes_temporary(engine& eng, const std::vector<parameter>& params, const statement* body, size_t i, bool may_prove);
    // A callee name that is a declared script function in the global environment (the
    // backends' resolution for the kernel; the checker falls back to it)
    struct ref_write_callee;
    bool resolve_global_ref_write_callee(engine& eng, const identifier_expr& id, std::vector<ref_write_callee>& out);

    // The cached verdict for parameter i (recomputed when the definition epoch moved; a
    // parallel worker may only consume a verdict the main thread already stamped).
    inline bool readonly_parameter_alias(engine& eng, const script_defined_function& fn, size_t i, bool may_prove) {
        if (!fn.alias_candidate || i >= 32 || !eng.parameter_alias()) { return false; }
        const uint64_t epoch = engine_access::definition_epoch(eng);
        if (fn.alias_proof_epoch != epoch) {
            if (!may_prove) { return false; }
            fn.alias_proof_mask = prove_readonly_parameters(eng, fn);
            fn.alias_proof_epoch = epoch;
        }
        return ((fn.alias_proof_mask >> i) & 1u) != 0;
    }

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_PARALLEL_TRANSFORM_HPP__
