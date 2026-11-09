#pragma once

#ifndef __JAISCRIPT_CORE_ENGINE_HPP__
#define __JAISCRIPT_CORE_ENGINE_HPP__

#include "types.hpp"
#include "value.hpp"
#include "function_binder.hpp"
#include "binding_traits.hpp"
#include "conversion_registry.hpp"
#include "conversion_registry_impl.hpp"
// conversion_registry_templates.hpp moved to after engine class definition
#include "bound_array.hpp"
#include "bound_map.hpp"
#include "jaibite.hpp"
#include "privilege.hpp"
#include "hot_reload_report.hpp"
#include <jaiscript/detail/type_checker.hpp>
#include <jaiscript/detail/first_type.hpp>
#include <jaiscript/serialization/serialization_metadata.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <string_view>
#include <span>
#include <initializer_list>
#include <unordered_set>
#include <typeindex>
#include <iosfwd>

namespace jai {

    // Forward declarations
    class class_decl;
    class class_definition;
    class class_instance;
    class class_registry;
    class execution_backend;
    class engine_time;
    class http_transport;
    class script_run;
    class string_symbolizer;
    struct import_site;
    namespace vm { class vm_backend; struct coverage_tables; }
    namespace debug { class controller; class transport; }
    namespace detail { struct release_machine; struct run_record; struct run_entry_state; struct run_registry; struct engine_access; }
    struct script_namespace_data;

    // engine::vm_coverage_summary: the report's class totals as numbers, for a harness that aggregates many runs
    struct vm_coverage_totals {
        uint64_t operations = 0, cells = 0, cells_closed = 0, cells_straight = 0, cells_frame = 0, block_entry = 0, typed = 0, untyped = 0;
        uint64_t closed_entered = 0;   // closed loops (not straight regions or frame programs) entered at least once
        std::vector<std::pair<std::string, uint64_t>> reasons;   // the general form's operations by the report's reason, most first
    };

    namespace detail {
        struct execution_limits;
        struct parallel_engine_state;
        enum class parallel_capture_kind : int;
    }

    namespace serialization {
        class serialization_registry;
        // Note: serialization metadata structs (property_metadata, class_metadata, etc.)
        // are defined in serialization_metadata.hpp which is included above
    }
    
    // Backend type enumeration
    enum class backend_type {
        interpreter,    // Tree-walking interpreter (the reference oracle)
        vm,             // Bytecode virtual machine (the default)
        auto_select     // The engine's default: set_backend stores vm
    };

    // Extended type info for overload resolution - stores base type + optional C++ type for objects
    struct param_type_info {
        script_value_type base_type;
        std::type_index cpp_type{typeid(void)};  // For object types, used to lookup class def

        param_type_info() : base_type(script_value_type::jai_null_type) {}
        param_type_info(script_value_type t) : base_type(t), cpp_type(typeid(void)) {}
        param_type_info(script_value_type t, std::type_index idx) : base_type(t), cpp_type(idx) {}

        bool operator==(const param_type_info& other) const {
            return base_type == other.base_type && cpp_type == other.cpp_type;
        }
    };

    // Compile-time-tagged script source for engine::execute's literal fast lane.
    // Constructing from a string literal is consteval (explicit — bare `execute("...")`
    // keeps its std::string overload), so a literal-tagged instance is GUARANTEED to
    // point at immutable static storage: the engine may key its parse cache on
    // pointer+length alone, skipping content keying entirely on re-execute. A runtime
    // char array is a compile error here (not a constant expression) — wrap dynamic
    // text in std::string_view instead, which routes to the content-keyed lane.
    // MSVC /GF string pooling merges identical literals to one address (one cache
    // entry); without pooling, duplicate text yields harmless duplicate entries.
    class script_source {
    public:
        template<size_t N>
        consteval explicit script_source(const char (&literal)[N]) noexcept
            : text_(literal), length_(N - 1), literal_(true) {}
        constexpr script_source(std::string_view source) noexcept
            : text_(source.data()), length_(source.size()), literal_(false) {}

        constexpr const char* data() const noexcept { return text_; }
        constexpr size_t size() const noexcept { return length_; }
        constexpr bool literal() const noexcept { return literal_; }

    private:
        const char* text_;
        size_t length_;
        bool literal_;
    };

    // The public members are the host API, as the embedding pages and the reference document it (docs/site/stability.html);
    // the library reaches the rest through detail::engine_access (detail/engine_access.hpp), which can change in any release
    class engine : public std::enable_shared_from_this<engine> {
    private:
        engine();

    public:
        ~engine();

        // Filesystem access is enabled by default. Changes affect the next protected
        // operation, including after a reentrant C++ callback; started work is not
        // cancelled. Grants are per-engine, not shared through script variables.
        // Configure on the engine thread (or while idle), outside parallel regions.
        // Concurrent cross-thread grant changes are not supported.
        void enable(privilege flags);
        void disable(privilege flags);
        bool is_enabled(privilege flags) const noexcept;
        checked_result<void> require_privilege(privilege flags) const noexcept;

        // Custom names are interned once, start disabled, and register idempotently.
        // "filesystem" refers to the existing builtin, without changing its grant.
        // The host uses names, never engine-local IDs. Bindings resolve names once.
        void register_privilege(std::string_view name);
        void enable(std::span<const std::string_view> names);
        void disable(std::span<const std::string_view> names);
        bool is_enabled(std::span<const std::string_view> names) const noexcept;
        checked_result<void> require_privilege(std::span<const std::string_view> names) const noexcept;
        void enable(std::string_view name) { enable(std::span(&name, 1)); }
        void disable(std::string_view name) { disable(std::span(&name, 1)); }
        bool is_enabled(std::string_view name) const noexcept { return is_enabled(std::span(&name, 1)); }
        checked_result<void> require_privilege(std::string_view name) const noexcept { return require_privilege(std::span(&name, 1)); }
        void enable(std::initializer_list<std::string_view> names) { enable(std::span(names.begin(), names.size())); }
        void disable(std::initializer_list<std::string_view> names) { disable(std::span(names.begin(), names.size())); }
        bool is_enabled(std::initializer_list<std::string_view> names) const noexcept { return is_enabled(std::span(names.begin(), names.size())); }
        checked_result<void> require_privilege(std::initializer_list<std::string_view> names) const noexcept { return require_privilege(std::span(names.begin(), names.size())); }
        
        // Factory method to ensure engines are always created as shared_ptr
        static std::shared_ptr<engine> make() {
            auto eng = std::shared_ptr<engine>(new engine());
            eng->initialize_engine_reference();
            return eng;
        }

		// Address-stable: values, bindings and backends retain this engine's address.
		// Transfer ownership by moving shared_ptr<engine>, never the engine itself.
		engine(const engine&) = delete;
		engine& operator=(const engine&) = delete;
		engine(engine&&) = delete;
		engine& operator=(engine&&) = delete;
        
        
        // Primary API - execute methods
        // A result handed out of the engine is a held_value (value.hpp): readable until the engine is destroyed, then it throws
        held_value execute(const std::string& script_content);
        held_value execute(const std::string& script_content, const instance_variables& instanceVars);

        // Literal fast lane: execute(script_source("...")) re-executes through a
        // pointer+length-keyed cache — the content is never re-keyed on a hit. A
        // string_view-wrapped dynamic falls through to the content-keyed lane, same
        // semantics as execute(std::string). Only consulted for tagged sources; the
        // std::string overloads are untouched.
        held_value execute(script_source source);

        // execute_file attributes the script to `script_path`: the path is stamped onto
        // every AST node (file:line:col), so stack traces, error messages, and the
        // debugger name the originating file. Prefer this over execute() for files —
        // the source path is carried for free, no debugging-only parameter to pass.
        held_value execute_file(const std::string& script_path);
        held_value execute_file(const std::string& script_path, const instance_variables& instanceVars);

        // Automatic jaibite disk cache (default ON). Every FILE-based load — execute_file,
        // script include (all forms), script import — transparently maintains a cache file
        // for the source (jaibite_cache_path: scripts/foo.jai -> scripts/.cache/foo.jai.jaibite):
        // a cache whose header records the exact source text (its size and hash, compared for
        // equality - mtimes never decide) and a matching registration fingerprint loads instead
        // of parsing; otherwise the source parses and the cache is rewritten. A write creates
        // the folder when missing and goes to a temporary file renamed over the cache, so no
        // reader sees part of one. Cache writes are best-effort and SILENT on failure (read-only
        // asset dirs, shipped games), tried once per source text — jaibite_cache_write_failures()
        // counts them for diagnostics. Raw string roots have no cache, but include/import
        // dependencies can access files. Disabling filesystem also skips all automatic disk caching.
        // Backend-agnostic: the cache holds the parsed
        // AST (filename stamps included, so traces still name the .jai); the vm compiles
        // its chunk lazily and reuses it in-memory as usual.
        void jaibite_cache(bool enabled);
        bool jaibite_cache() const;
        size_t jaibite_cache_write_failures() const;
        // Hosts whose sources are read-only (a mobile app bundle) send the cache elsewhere: with a
        // directory set, every cache is written below it at the source's path relative to source_root
        // (a source outside source_root, or with no root given, mirrors its whole path, a drive as its
        // first folder). When the directory holds no valid cache for a source, a valid one in the .cache
        // folder beside it still loads (never rewritten there), so a bundle can ship its caches. Empty
        // restores the folder beside each source.
        void jaibite_cache_directory(const std::string& directory, const std::string& source_root = {});
        std::string jaibite_cache_directory() const;
        // The cache file the engine writes for source_path (empty for a .jaibite source, which keeps none, and for a source
        // outside confine_paths when no cache directory is set)
        std::string jaibite_cache_path(const std::string& source_path) const;

        // Execute already-read script file contents with jaibite disk-cache maintenance
        // keyed beside resolved_path — for hosts that read files through their own VFS but
        // still want the sibling cache (engine-side file loads use it internally).
        // Identical to execute(content) + path attribution when the cache is disabled or
        // the path is a .jaibite itself.
        script_value execute_file_source(const std::string& resolved_path, const std::string& content);

        // Pre-parsed script handle: parse once here, then run it repeatedly via
        // bite.execute() or engine->execute(bite) without re-lexing/parsing (the vm
        // backend also caches its compiled bytecode inside the bite). Engine-bound —
        // never share a jaibite across engines. Parse errors throw here.
        jai::jaibite jaibite(const std::string& script_content);
        held_value execute(jai::jaibite& bite);

        // === SUSPENDABLE RUNS (<jaiscript/core/script_run.hpp>; docs/site/embedding/17-host-api.html#suspendable-runs) ===
        // Each starts a run on a native fiber of its own and runs it now, until a sleep suspends it or it ends; the returned
        // script_run resumes it. A run from source, a file or a jaibite reports a failure as execute() does (the call that ends
        // the run throws it); a callable's run reports it through result(). A jaibite must outlive its run. execute() never
        // suspends: a sleep outside a run blocks.
        script_run start(const std::string& script_content);
        script_run start_file(const std::string& script_path);
        script_run start_file_source(const std::string& resolved_path, const std::string& content);   // execute_file_source's run
        script_run start(jai::jaibite& bite);
        script_run start(const script_value& callable, std::vector<script_value> args = {});

        // Load a jaibite saved via jaibite::save()/save_bytes(). Symbols and type_info
        // are re-interned into THIS engine, so a bite saved from another engine loads
        // correctly. Throws jai::runtime_error on missing file / bad format / version
        // mismatch / corrupt data. Check bite.registration_mismatch() for an advisory
        // registered-class/function fingerprint difference.
        jai::jaibite jaibite_load(const std::string& path);
        jai::jaibite jaibite_load_bytes(const uint8_t* data, size_t size, const std::string& own_path = {});   // own_path: the file the bytes came from (engine-written siblings mark their own file)
        jai::jaibite jaibite_load_bytes(const std::vector<uint8_t>& bytes) {
            return jaibite_load_bytes(bytes.data(), bytes.size());
        }

        // FNV-1a over the sorted registered function/class/template-type names — the
        // parse-affecting registration surface. Stamped into saved jaibites; compared on
        // load to set jaibite::registration_mismatch().
        uint64_t registration_fingerprint() const;

        // === STATIC TYPE CHECKING (docs/site/embedding/06-static-checking.html) ===
        // Every load is checked: an error-severity diagnostic makes execute()/jaibite
        // creation/load throw static_check_error BEFORE anything runs; what() is the full
        // accumulated, location-sorted, deduplicated listing with source + carets. A typed ->
        // typed mismatch is therefore a load error; only a var-sourced value can still fail at
        // runtime, where try/catch sees it. A [[warn_mode]] region reports its errors as
        // warnings and loads. Checks are amortized once per unique source (cached beside the
        // parse) and re-run when the registration surface changes.

        // Parse (cached) + check, without running or gating — tooling entry point.
        // Throws parse_error if the source doesn't parse (checking is post-parse).
        check_report check(const std::string& script_content);

        // Diagnostics from the most recent checked execute()/jaibite path.
        const check_report& last_check_diagnostics() const;

        // === PARSE-TIME TYPE PROOFS ===
        // On by default. The checker's proof pass runs beside the diagnostics walk (one walk per
        // unique source per surface epoch, cached beside the parse) and stamps same-type / field proofs onto AST
        // nodes; both backends consume them behind total runtime guards, so results never
        // depend on this switch. Off: fresh entries stay unproven and already-stamped cached
        // entries are stripped at their next execute (jaibites likewise at their next
        // execute). Proofs are never serialized - a loaded jaibite re-derives them.
        void type_proofs(bool on);
        bool type_proofs() const;
        // === THE AGGREGATE LOWERING ===
        // Typed math locals of function and lambda bodies run as their component slots (detail/aggregate_lowering.hpp);
        // off, every vector stays one boxed value, observationally identical. The default follows the backend: on for the
        // vm (its typed rows and cells run the slots at the float-local floor), off for the interpreter (whose per-node
        // dispatch runs the boxed kernel faster), so the default interpreter-against-vm fuzz is itself the lowering's
        // differential. Set before the first execute, like the backend; the getter answers the effective setting.
        void aggregate_lowering(bool on);
        bool aggregate_lowering() const;
        // === THE MIDDLE END'S SWITCH (the pinned forms onward) ===
        // On by default. The VM's proof-based elisions and passes over closed regions (the forms
        // that skip a check the region analysis proved redundant, the passes that rewrite a region)
        // follow this switch; off, the region runs its checked canonical form, observationally
        // identical (the differential gate is `jaiscript_fuzz --no-passes`). Takes effect at the
        // next compile; a chunk already compiled keeps its form.
        void ir_passes(bool on);
        bool ir_passes() const;
        // === THE VM REFERENCE MODE (docs/site/embedding/03-backends.html) ===
        // Off by default. On, the VM compiles and runs its plain general path: type_proofs, ir_passes, parameter_alias,
        // local_alias and aggregate_lowering answer off whatever they were set to, and the compiler emits no superinstruction,
        // typed or dynamic slot row, closed region, graft, counted loop, return or tail-call fusion, builtin dispatch index or
        // compile-time builtin lane; the VM fills no inline cache (member, call, method, lookup fast heads) and folds no scoped
        // const. Every program gives the optimized VM's values, output and error text (the differential gate is
        // `jaiscript_fuzz --vm-reference`); a deep self tail call raises where the optimized VM reuses its frame. Every choice
        // is made when a chunk compiles or the backend is built, so the optimized VM pays nothing for the mode. Set before the
        // first execute, like the backend.
        void vm_reference_mode(bool on);
        bool vm_reference_mode() const;
        // === MISS INJECTION (a testing lever for the cell blocks) ===
        // Built only when JAISCRIPT_MISS_INJECTION is defined (the fuzz's MISS configuration): every nth cell
        // instruction whose own miss re-runs its original instruction misses before it runs, as a failed guard
        // would, so the flush and the continuation contract run against the interpreter at every such position
        // (`jaiscript_fuzz --miss-every n`, the runner's `--miss-every=<n>`). 0 = off, the default. A build
        // without the lever throws std::logic_error on a nonzero n, so a gate never runs without the misses it
        // asked for; miss_injection_available() says which build this is.
        void cells_miss_every(uint32_t n);
        uint32_t cells_miss_every() const;
        static bool miss_injection_available();
        // === THE LISTING SINK (`jaiscript --disasm=<file>`) ===
        // Unset by default. When set, every chunk the VM compiles from here on (a program, a function at its
        // define, a standalone expression or declaration) is disassembled as it is compiled, with the regions'
        // canonical operations listed, and handed to the sink in compile order. No bytecode changes either way.
        // The sink receives the chunk's readable listing (disassembler.hpp) and its semantic manifest (chunk_manifest.hpp).
        using disassembly_sink_fn = std::function<void(const std::string& listing, const std::string& manifest)>;
        void disassembly_sink(disassembly_sink_fn sink);
        const disassembly_sink_fn& disassembly_sink() const;
        // === EXECUTION COVERAGE (a diagnostic build: JAISCRIPT_VM_COVERAGE; vm/coverage.hpp) ===
        // The vm counts every instruction where it runs (a cell block, the general form typed or untyped) and, for the
        // general form, the reason the compiler left it there, attributed to the function and the source line; the
        // parallel workers' backends count too. vm_coverage_report ranks the reasons by executed operations with their top
        // sites (top rows per table); vm_coverage_reset zeroes the counters (a warm-up drops out). A build without the
        // define reports vm_coverage_available() false and throws std::logic_error from the other two.
        static bool vm_coverage_available();
        void vm_coverage_reset();
        std::string vm_coverage_report(size_t top = 25) const;
        vm_coverage_totals vm_coverage_summary() const;
        // Stamp counters from the most recent proof pass (tests assert proofs happened).
        const proof_stats& last_proof_stats() const;

        // Host-function surface for the static checker: per-overload arity (SIZE_MAX =
        // variadic) + C++ parameter signature (empty = untyped legacy binding). Returns
        // false when the name has no registered C++ function entry.
        struct host_overload_signature {
            size_t arity = 0;
            std::vector<param_type_info> param_types;
        };
        bool host_function_signatures(const std::string& name, std::vector<host_overload_signature>& out) const;
        // A free host function that touches state other objects share: a parallel body may not call it. Every other
        // registered host function, the library's own included, is taken to be safe on any thread. A namespace entry is
        // marked by its qualified name (serial_function("game::save")).
        void serial_function(const std::string& name);

        // Constant arguments: a host function ("name") or a bound method ("Class.method") whose argument `index` only the
        // program's own constants may fill: a string literal, a const holding one, a ternary or + of them, an array literal of
        // them. Register before loading scripts. The checker refuses any other argument at load with "argument N of 'name' is
        // not a constant: <refusal>"; each unit's constant texts for the argument are gathered as it loads, in every check
        // mode, and constant_argument_admits says whether a run-time text is one of them (one the program does not spell, or
        // a unit that was never gathered, is refused: the host raises the refusal).
        void constant_argument(const std::string& callable, size_t index, std::string refusal);
        bool constant_argument_admits(const std::string& callable, size_t index, std::string_view text) const;
        // [[nodiscard]] for a host function ("name") or a bound method ("Class.method"), as a script declares it on its own: an
        // expression statement that drops the result of a call the checker resolves to it fails the load. Register before
        // loading scripts.
        void nodiscard_function(const std::string& callable);
        bool is_nodiscard_function(const std::string& callable) const;

        // Global registration
        void add_global(const std::string& name, script_value value, bool is_serializable = true);
        void add_global(const std::string& name, const held_value& value, bool is_serializable = true) { add_global(name, make_value(value), is_serializable); }
        
        // Convenience overload for add_global that automatically wraps values
        template<typename T>
        void add_global(const std::string& name, const T& value, bool is_serializable = true) {
            add_global(name, make_value(value), is_serializable);
        }
        
        // Add a global reference that binds to a C++ variable
        template<typename T>
        void add_global_ref(const std::string& name, T& value, bool is_serializable = false) {
            // For primitive types, use cpp_bound for direct binding
            if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T> ||
                          std::is_same_v<T, bool> || std::is_same_v<T, std::string> ||
                          std::is_same_v<T, char>) {
                auto ref = script_value::make_cpp_bound(&value, this);
                add_global(name, std::move(ref), is_serializable);
            } else {
                // For custom types, create a non-owning shared_ptr using aliasing constructor
                // This allows script to access the object without taking ownership
                // WARNING: Caller must ensure 'value' outlives the script engine!
                auto lifetime_tracker = std::make_shared<bool>(true);
                std::shared_ptr<T> non_owning_ptr(lifetime_tracker, &value);
                add_global(name, make_value_object(non_owning_ptr), is_serializable);
            }
        }
        
        // Reports this build's COMPILE-TIME integer-overflow policy:
        //   true  => overflowing integer +, -, *, unary -, and INT64_MIN/-1 division
        //            raise a catchable runtime error (the default, "safe by default").
        //   false => those operations wrap with well-defined two's-complement semantics.
        // Chosen at build time via JAISCRIPT_CHECKED_OVERFLOW / JAISCRIPT_WRAP_ON_OVERFLOW;
        // there is no runtime toggle (so the hot path carries no policy branch).
        bool throw_on_overflow() const;

        // Wall-clock budget in seconds for one host entry: an execute()/next() call, or a script function the host
        // calls while no script runs, each armed from the budget current at the entry (fractional is
        // fine — e.g. 1.0/60 for a frame). A script exceeding it raises a TERMINAL
        // runtime error (execution_budget_exceeded): script catch handlers are skipped
        // and execute() reports the failure to the host — a script can never swallow a
        // timeout. Zero disables the limit. Default 5. Checked at loop back-edges and
        // call entry, so straight-line C++ bindings are never interrupted mid-call.
        void execution_budget(double seconds);
        double execution_budget() const;
        // A clock-free bound beside it for harnesses that must not depend on load: at most `checks` budget checks (each one
        // per 1024 loop back-edges and call entries) per host entry, then the same terminal budget error. Deterministic per
        // backend and program; zero (the default) disables it.
        void execution_step_budget(uint64_t checks);
        uint64_t execution_step_budget() const;

        // Maximum active call depth for each built-in backend: interpreter 256,
        // VM 250000 by default. Configure between executions; depth must be in
        // [1, INT_MAX]. Each engine owns its settings, copied to parallel workers
        // at region entry. Native-stack protection remains active independently;
        // increasing this cap cannot make native recursion safe on a small stack.
        // VM tail calls that reuse a frame do not consume additional depth.
        void max_call_depth(backend_type backend, size_t depth);
        size_t max_call_depth(backend_type backend) const;

        // The step-debugger controller for this engine (lazily created, engine-owned).
        // First call wires the active backend's statement hook. Arm a session with
        // debugger().set_enabled(true). See jaiscript/debug/controller.hpp.
        debug::controller& debugger();

        // Attach an optional debug transport (typically jai::debug::debug_connector, which
        // speaks DAP over a TCP socket so VS Code can attach). The engine keeps it alive and
        // stops it — joining its thread and detaching the controller — before teardown, so a
        // paused script can never wedge the host. Passing nullptr stops and releases any
        // current one. One connector per engine is the norm;
        void set_debug_connector(std::shared_ptr<debug::transport> connector);

        // Approximate script memory cap in bytes for one top-level execute()/next()
        // call. Accounting is a HIGH-WATER MARK charged at the heavy-value chokepoints
        // (container growth, string concat, clone, environment growth); frees are not
        // credited back. An allocation that would exceed the cap is DENIED and raises a
        // memory_cap error: the FIRST raise per execute is an ordinary catchable script
        // error (the counter re-arms so the script can free caches and continue); the
        // SECOND raise in the same execute is TERMINAL like a budget overrun. Reentrant
        // executes share the outer call's allowance. Zero disables the cap. Default 0.
        void memory_cap(size_t bytes);
        size_t memory_cap() const;

        // The most bytes a binary::reader's text() (a C string) scans for its NUL; a scan that reaches the limit raises a
        // catchable error naming it and moves nothing, so a script can never run an unbounded scan (scripts cannot change
        // it). Default 1 MiB (1048576): far above any string field real formats hold, and a full miss costs a memchr over
        // 1 MiB, well under a millisecond. There is no unlimited setting; a larger value is the host's choice.
        void binary_scan_limit(size_t bytes);
        size_t binary_scan_limit() const;

        // The most bytes one template() render writes (default 10 MiB, 10485760) and one scoped_template or scoped_template_text
        // render writes (default 1 MiB, 1048576); a render that would write past its limit raises a catchable error naming the
        // limit. Zero is no limit. Scripts read them (reflect::template_output_limit()) and cannot change them; a change follows
        // the privilege rules: idle or on the execution thread, never inside a parallel region, and a render already started
        // keeps the limit it started with.
        void template_output_limit(size_t bytes);
        size_t template_output_limit() const;
        void scoped_template_output_limit(size_t bytes);
        size_t scoped_template_output_limit() const;
        // The folders a scoped_template(path, map) file is read from, under the scoped_template privilege alone. The path is
        // relative with no .. segment; it is tried in each folder in list order, and the first file that exists and canonically lies
        // inside its folder (links resolved) is the template. A relative folder is relative to the directory of the script that
        // calls; the default is {"template"}, and a folder that does not exist holds nothing. Scripts read the list with
        // reflect::scoped_template_dirs() and cannot change it; a change follows the privilege rules.
        void add_scoped_template_dir(const std::string& dir);
        void remove_scoped_template_dir(const std::string& dir);
        void set_scoped_template_dirs(std::vector<std::string> dirs);
        const std::vector<std::string>& scoped_template_dirs() const;

        // === PARALLEL EXECUTION (parallel_transform v0) ===
        // Worker count the next parallel_transform uses (also the thread_count() builtin).
        // 0 = auto (hardware concurrency). The calling thread runs one chunk; the pool
        // supplies the rest.
        void parallel_thread_count(size_t workers);
        size_t parallel_thread_count() const;

        // Hot-reload migration reports (off by default): with reporting on, every automatic field operation a
        // reload performs aggregates into hot_reload_reports() until cleared; off, migration records nothing.
        void hot_reload_reporting(bool enabled) { hot_reload_reporting_ = enabled; }
        bool hot_reload_reporting() const noexcept { return hot_reload_reporting_; }
        const std::vector<field_migration_report>& hot_reload_reports() const noexcept { return hot_reload_reports_; }
        void clear_hot_reload_reports() { hot_reload_reports_.clear(); }

        // A host function's entry over its arguments where they are, beside the function `name` binds now: a VM call site whose
        // callee is that function runs `invoke(target, args, arity)` for a call of `arity` arguments, none of them a reference,
        // instead of the function (no argument vector, no std::function). `invoke` must return what the function returns for
        // those arguments; a re-registration or a rebinding of the name is another function, which the site calls as before.
        using host_direct_invoke = checked_result<script_value> (*)(const void*, const script_value*, size_t);
        static constexpr size_t host_direct_any_arity = SIZE_MAX;   // a variadic function's row: any call of 1 to 8 arguments
        void add_host_direct(std::string_view name, size_t arity, host_direct_invoke invoke, const void* target);
        void add_host_direct(const script_value& function, size_t arity, host_direct_invoke invoke, const void* target);   // a function value (a namespace's)

        using stack_frame = ::jai::stack_frame;
        std::vector<stack_frame> last_stack_trace() const;
        std::string format_stack_trace() const;

        // Per-user folders for an app's data and cache, the platform's convention: %LOCALAPPDATA%/name
        // (cache below it), ~/Library/Application Support/name and ~/Library/Caches/name, $XDG_DATA_HOME
        // and $XDG_CACHE_HOME (~/.local/share, ~/.cache)/name. Queries only, nothing is created. A host
        // on a platform the defaults do not know (Android) installs its own resolvers; the script
        // builtins app_data_dir/app_cache_dir answer through whichever is installed.
        void app_directory_resolvers(std::function<std::string(const std::string& app)> data,
                                     std::function<std::string(const std::string& app)> cache);
        std::string app_data_dir(const std::string& app) const;
        std::string app_cache_dir(const std::string& app) const;
        // The running executable, generic (forward-slash) spelling; empty when the platform cannot say
        static std::string executable_path();

        // The (modification time, size) stamp that execute_file, include, import and their caches compare to see a file
        // change, and that a read is checked under (a stamp that moves during the read marks the text torn). An empty
        // source selects the file system's; a source answers nullopt for a file it cannot stamp.
        struct file_stamp {
            uint64_t mtime = 0;
            uint64_t size = 0;
        };
        using file_stamp_reading = std::function<std::optional<file_stamp>(const std::string& a_path)>;
        void file_stamp_source(file_stamp_reading a_source);

        // The HTTP stack spider::catch / sling / get / post send through (<jaiscript/core/http_transport.hpp>); none is
        // installed by default, and a call without one raises a catchable error naming this function. Install it on the
        // engine thread while no script runs.
        void http_transport(std::shared_ptr<jai::http_transport> a_transport);
        const std::shared_ptr<jai::http_transport>& http_transport() const noexcept;

        // The clock time:: reads and the running execute's deadline (<jaiscript/core/engine_time.hpp>, which brings <chrono>)
        engine_time time();
        // From the system clock's reading at construction, or from zero on a host monotonic provider
        double engine_elapsed();
        double execute_elapsed();

        // Object creation through registered class system
        template<typename T, typename... Args>
        script_value make_object(Args&&... args) {
            return make_value_object(std::make_shared<T>(std::forward<Args>(args)...));
        }

        // A host std::shared_ptr<T> is a handle, as add_global and a host shared_ptr result make one: copies share the object
        template<typename T>
        script_value make_object(std::shared_ptr<T> data) { return make_handle(std::move(data)); }

        // Convenient script_value creation methods
        // Usage: engine->make_value(42) instead of script_value(42, engine)
        // A value of another engine enters by copy (import_value); this engine's own values pass as they are
        script_value make_value(const script_value& value) { return owns_value(value) ? value : import_value(value); }
        script_value make_value(script_value& value) { return owns_value(value) ? value : import_value(value); }
        script_value make_value(script_value&& value) { return owns_value(value) ? std::move(value) : import_value(value); }
        script_value make_value(const held_value& value) { return make_value(value.value()); }
        script_value make_value(held_value& value) { return make_value(value.value()); }
        script_value make_value(held_value&& value) { return make_value(value.value()); }

        template<typename T>
        requires (std::is_arithmetic_v<std::remove_cvref_t<T>> ||
                  std::is_same_v<std::remove_cvref_t<T>, script_string> ||
                  std::is_same_v<std::remove_cvref_t<T>, std::string> ||
                  std::is_convertible_v<T, const char*> ||  // Handles const char*, const char[N], etc.
                  std::is_same_v<std::remove_cvref_t<T>, std::monostate>)
        script_value make_value(T&& value) {
            return script_value(std::forward<T>(value), this);
        }

        template<typename T>
        requires (!std::is_arithmetic_v<std::remove_cvref_t<T>> &&
                  !std::is_same_v<std::remove_cvref_t<T>, script_string> &&
                  !std::is_same_v<std::remove_cvref_t<T>, std::string> &&
                  !std::is_convertible_v<T, const char*> &&  // Exclude string literals and char pointers
                  !std::is_same_v<std::remove_cvref_t<T>, std::monostate> &&
                  std::is_lvalue_reference_v<T>)
        script_value make_value(T&& value);  // Implemented in engine_impl.hpp

        template<typename T>
        requires (!std::is_arithmetic_v<std::remove_cvref_t<T>> &&
                  !std::is_same_v<std::remove_cvref_t<T>, script_string> &&
                  !std::is_same_v<std::remove_cvref_t<T>, std::string> &&
                  !std::is_convertible_v<T, const char*> &&
                  !std::is_same_v<std::remove_cvref_t<T>, std::monostate> &&
                  !std::is_pointer_v<std::remove_cvref_t<T>> &&
                  std::is_rvalue_reference_v<T&&> &&
                  !std::is_lvalue_reference_v<T>)
        script_value make_value(T&& value);  // Implemented in engine_impl.hpp

        template<typename T>
        script_value make_value(const std::shared_ptr<T>& value) {
            if (!value) {
                return script_value(std::monostate{}, this);
            }
            return make_handle(value);
        }

        template<typename T>
        script_value make_value(std::shared_ptr<T>&& value) {
            if (!value) {
                return script_value(std::monostate{}, this);
            }
            return make_handle(std::move(value));
        }

        template<typename T>
        requires (!std::is_same_v<T, char> && !std::is_same_v<T, const char>)  // Exclude char* (handled as strings)
        script_value make_value(T* value) {
            if (!value) {
                return script_value(std::monostate{}, this);
            }
            return script_value::make_cpp_bound(value, this);
        }

        // Create null/void value
        script_value make_null() {
            return script_value(std::monostate{}, this);
        }

        // Create array value
        script_value make_array() {
            return script_value::make_array(nullptr, this);
        }

        // Create map value
        script_value make_map() {
            return script_value::make_map(nullptr, nullptr, this);
        }
        
        // === FUNCTION REGISTRATION ===
        
        // Variadic functions - handle any number of arguments at runtime
        // Use this for functions like print(), max(), or functions with optional parameters
        // Example: engine.add_variadic_function("print", [](const std::vector<script_value>& args) { ... });
        void add_variadic_function(const std::string& name, script_function func, privilege required = privilege::none);
        void add_variadic_function(const std::string& name, script_function func, std::span<const std::string_view> names) {
            add_variadic_function(name, guard_function(std::move(func), names));
        }
        void add_variadic_function(const std::string& name, script_function func, std::string_view required) {
            add_variadic_function(name, std::move(func), std::span(&required, 1));
        }
        void add_variadic_function(const std::string& name, script_function func, std::initializer_list<std::string_view> names) {
            add_variadic_function(name, std::move(func), std::span(names.begin(), names.size()));
        }
        void add_function_with_arity(const std::string& name, script_function func, size_t arity);
        
        // Typed function registration with automatic overloading
        // Use this for functions with specific C++ signatures
        // Example: engine.add_function("add", [](int a, int b) -> int { return a + b; });
        template<typename Func, typename Requirements = privilege>
        void add_function(const std::string& name, Func&& func, Requirements required = privilege::none) {
            // Check for rejected container references
            binding_traits::check_binding_validity<Func>(name);
            
            using traits = detail::function_traits<std::decay_t<Func>>;
            using return_type = typename traits::return_type;
            constexpr size_t arity = traits::arity;
            
            auto binder = make_function_binder(std::forward<Func>(func), this);
            script_function bound_func = binder.bind();
            stage_host_result(host_result_kind<return_type>());
            if constexpr (detail::host_lane_params<typename traits::argument_types>() && std::is_same_v<Requirements, privilege>) {
                if (required == privilege::none) {
                    using call_t = typename decltype(binder)::by_value_call;
                    stage_host_direct({&call_t::invoke, &call_t::locate});
                }
            }
            
            script_function wrapped_func = guard_function(std::move(bound_func), std::move(required));
            if constexpr (constexpr uint64_t writeback = detail::tuple_writeback_mask<typename traits::argument_types>::value; writeback != 0) {
                wrapped_func = host_writeback_function{ std::move(wrapped_func), writeback };
            }
            
            std::vector<param_type_info> param_types = extract_parameter_types_with_info<Func>();

            // Check if this should be registered as an overload
            if (has_function(name)) {
                // Automatically convert to overloaded function with type info
                add_overloaded_function_with_full_types(name, arity, wrapped_func, param_types);
            } else {
                // Register normally but store arity info for future overloading
                add_function_with_arity_and_full_types(name, wrapped_func, arity, param_types);
            }
        }

        template<typename Func>
        void add_function(const std::string& name, Func&& func, std::initializer_list<std::string_view> names) {
            add_function(name, std::forward<Func>(func), std::span(names.begin(), names.size()));
        }
        // Overloaded function registration
        void add_overloaded_function(const std::string& name, size_t arg_count, script_function func);
        
        // Class registration
        // Register a class with its type information
        template<typename T>
        void add_class(const std::string& name, std::shared_ptr<class_definition> classDef) {
            add_class_impl(name, classDef);
            register_class_by_type(std::type_index(typeid(T)), classDef);
        }
        
        // Template type registration for parsing
        // Registers base template names (e.g., "Point" from "Point<int>") 
        // so the parser knows which identifiers can be followed by template syntax
        void register_template_type(const std::string& baseTemplateName);

        // Every class THIS engine can name (host-bound + script-declared), sorted —
        // reflect::classes()'s deterministic source
        std::vector<std::string> registered_class_names() const;

        // Check if a type is registered (by name)
        bool is_type_registered(const std::string& name) const {
            return get_class_definition(name) != nullptr;
        }

        // Check if type T has been registered via dynamic_binder
        template<typename T>
        bool has_registered_class() const {
            return get_class_definition_by_type(std::type_index(typeid(T))) != nullptr;
        }

        template<typename T>
        void bind_static_type();  // Implemented in static_binder_impl.hpp

        template<typename... Ts>
        void bind_static_types() {
            (bind_static_type<Ts>(), ...);
        }
        
        // Register vector conversion for a specific type
        template<typename T>
        void add_vector_conversion() {
            auto manager = get_conversion_manager();
            manager.add_vector_conversion<T>();
        }
        
        // Register map conversion for specific key/value types
        template<typename K, typename V>
        void add_map_conversion() {
            auto manager = get_conversion_manager();
            manager.add_map_conversion<K, V>();
        }
        
        // Register custom conversion for any type
        template<typename T>
        void add_custom_conversion(
            std::function<T(const script_value&)> from_func,
            std::function<script_value(const T&)> to_func
        );

        // Variable access
        held_value get_variable(const std::string& name) const;
        // A plain script_value C++ receives (a bound function's argument, a value read out of a container, a signal's argument) is
        // a borrow, valid for the call; hold(v) keeps it until this engine is destroyed (held_value)
        held_value hold(script_value value) const;
        bool has_variable(const std::string& name) const;
        bool has_function(const std::string& name) const;

        // Backend configuration
        void set_backend(backend_type type);
        backend_type get_backend_type() const;
        std::string get_backend_name() const;
        // A value's text as string + value spells it (a class's own to_string() runs; its error is the result)
        checked_result<std::string> text_of(const script_value& v) const;

        // Format a checked_result error to a human-readable string
        // Resolves symbol IDs to their string representations
        template<typename T>
        std::string format_error(const checked_result<T>& result) {
            return jai::format_error(result, *get_symbolizer());
        }

        // Script layers (docs/site/embedding/13-script-extensible.html). A script class deriving a bound C++ class
        // owns its object as its native object; the object carries the layer back through its
        // script_extensible slot (else this engine's weak map), so every boundary return of a
        // shared_ptr<Base> hands script the layer, and serialization writes the script class.
        //
        // extend(object, Class) - the script builtin and this engine's door. Class: a constructor
        // value (the bare class name in script) or a name. object a C++ wrapper or a layer:
        // Class must derive the object's C++ base; the current layer's class returns it
        // (idempotent), another class re-derives (fields carried where both classes have them,
        // the old layer retired). object a pure script instance: Class must derive its class; the
        // instance itself is re-derived in place (its fields carried) and returned. Field
        // initializers run, no constructor body. An extended object owns its layer for its
        // lifetime (slot types); a type without the slot keeps the layer engine-owned until
        // script_layer_detach. An object layered by another engine is moved here.
        script_value extend(const script_value& object, const script_value& script_class);
        // fields_json ($script from script_layer_detach) seeds the new layer: the explicit
        // two-step migration for types without the slot, which no engine can see into another's
        // map for.
        script_value extend(const script_value& object, const script_value& script_class, const std::string& fields_json);
        template<typename T>
        script_value extend(const std::shared_ptr<T>& object, std::string_view script_class, const std::string& fields_json = std::string());
        // The layer's fields as JSON, and the object no longer carries a layer; the old layer is
        // retired (C++ access through it errors, its script fields stay). "" without a layer.
        template<typename T>
        std::string script_layer_detach(const std::shared_ptr<T>& object);
        // The live layer owned by this engine, if any.
        template<typename T>
        strong_ptr<class_instance> script_layer(const T& object) const;
        // Cores (error codes; the public extend throws at the host boundary): extend itself,
        // the script class behind a constructor value / name, and a layer of script_def over an
        // object typed as base_def (state from carry_from's fields or fields_json).
        checked_result<script_value> extend_checked(const script_value& object, const script_value& script_class, const std::string& fields_json);
        // The elision's kill switch (default on): off binds every by-value parameter with the
        // clone, observationally identical - the differential gates flip it
        void parameter_alias(bool on);
        bool parameter_alias() const;
        void local_alias(bool on);
        bool local_alias() const;

        // Script errors at a boundary with no error-value channel (a destructor that fails where nothing can catch it) route
        // here; a converted std::function callback and a signal's script receiver throw to the host caller instead
        // (engine_impl.hpp, signal_impl.hpp). Default sink is std::cerr; the message includes the script stack trace if captured.
        void set_script_error_handler(std::function<void(const std::string&)> a_handler);

        // Advisory diagnostics that are NOT errors (e.g. a class method shadowing a builtin
        // handle method). Always-on, independent of the static checker.
        // Default sink is std::cerr ("[jaiscript] warning: ..."); the host app / tests
        // install their own handler to capture. Execution always proceeds.
        void set_script_warning_handler(std::function<void(const std::string&)> a_handler);

        // RAII scope for C++ code invoking script functions directly (converted callbacks,
        // signal receivers): without it, reference parameters would bind against whatever
        // script call happens to be in flight.
        class external_call_guard {
        public:
            explicit external_call_guard(engine* a_engine) : engine_(a_engine) {
                if (engine_) { engine_->push_external_call_scope(); }
            }
            ~external_call_guard() {
                if (engine_) { engine_->pop_external_call_scope(); }
            }
            external_call_guard(const external_call_guard&) = delete;
            external_call_guard& operator=(const external_call_guard&) = delete;
        private:
            engine* engine_;
        };

        // Output stream redirection
        // Set a custom output stream for print() and related functions
        // Pass nullptr to reset to std::cout
        void set_output_stream(std::shared_ptr<std::ostream> stream);
        void add_include_path(const std::string& path);
        // Confinement (opt-in; empty, the default, lets the ladder open any path the process can read): include, import,
        // resolve_path and file_data open only files whose canonical path lies under one of these folders. A file the ladder
        // finds outside them is skipped, and when no rung finds one inside, the reach fails with "Path outside the confined
        // roots: <path as written>". No .cache folder is written beside a source outside them.
        void confine_paths(const std::vector<std::string>& roots);
        std::vector<std::string> confine_paths() const;
        
        // Import behavior configuration
        enum class import_behavior {
            file_timestamp, // Re-import if file modified (DEFAULT)
            once,           // Import only once, ignore file changes
            always         // Treat import as include
        };
        
        void set_import_behavior(import_behavior behavior);

        // The engine-wide import check pass (guide ch. 10): at most once per `seconds` (default 0.2; 0 = every reach) the first
        // import reached opens a pass, every import reached during it stats its modules, and the pass closes when a module it
        // already checked is reached again or when the outermost execute()/next() returns; between passes an import does
        // nothing. A frame therefore checks every import it reaches or none. The window runs on the engine's clock sample
        // (the real steady clock as read at the host entry, at the budget check's samples and by a time:: read of the system
        // clock), so an import reads no clock itself; a host engine_time::source never moves it.
        void import_recheck_interval(double seconds);
        double import_recheck_interval() const;
        
        // Import management
        void reset_imports();
        void reset_import(const std::string& path);
        bool is_imported(const std::string& path) const;
        std::vector<std::string> get_imported_files() const;
        bool loading() const noexcept { return load_depth_ > 0; }

    private:
        friend struct detail::engine_access;
        friend class caught_error_scope;

        // Attach a requirement to the actual callable, so aliases, overload dispatch
        // and worker-local copies all recheck this engine's current grants. An empty
        // requirement returns the original callable with no invocation overhead.
        script_function guard_function(script_function function, privilege required);
        script_function guard_function(script_function function, std::span<const std::string_view> names);
        script_function guard_function(script_function function, std::string_view name) {
            return guard_function(std::move(function), std::span(&name, 1));
        }
        script_function guard_function(script_function function, std::initializer_list<std::string_view> names) {
            return guard_function(std::move(function), std::span(names.begin(), names.size()));
        }
        // include: the file runs at the include site, in the current scope and under the running unit, so
        // its declarations are the includer's. scope names the function or block the
        // include runs in (empty at program scope): an enum the file declares there stays local to it.
        script_value execute_include_source(const std::string& resolved_path, const std::string& content, std::string_view scope = {});
        // An include's file read for the backends: false = it cannot be opened. Inside a running import the file joins the
        // module's closure under the stamp it was read at, so a later change to it re-runs the module.
        bool read_include_file(const std::string& resolved_path, std::string& content);

        // The sleep builtin (detail: script_run.cpp): seconds on the time source; suspends the innermost run when nothing pins it,
        // else blocks. sleep_callee: `function` is the core sleep function (a call of it runs the builtin)
        checked_result<script_value> sleep_builtin(const script_value& seconds);
        bool sleep_callee(const void* function) const noexcept;
        // A builtin formatter's literal entry (stdlib/io.cpp's print and format): a call of it by name whose first argument is a
        // string literal (call_expr::literal_format_call) runs `literal` while the name binds the function; any other call of it
        // takes its first argument as text like the rest
        struct format_literal_row {
            const void* identity = nullptr;   // &function.unchecked_as_function()
            script_value function{std::monostate{}, nullptr};   // pins the node: a freed function's address never matches later
            script_value literal{std::monostate{}, nullptr};
            host_direct_invoke invoke = nullptr;   // the literal entry over arguments where they are, none of them a reference
            const void* target = nullptr;
        };
        void add_format_literal(std::string_view name, script_function literal, host_direct_invoke invoke, const void* target);
        const format_literal_row* format_literal_for(const void* function) const noexcept;
        // Native code running script inside a run (a host callback, a destructor, a signal receiver, an import, a builtin's
        // callback): while one is on the stack a sleep blocks instead of suspending the run
        void pin_run() noexcept { if (active_run_pins_) { ++*active_run_pins_; } }
        void unpin_run() noexcept { if (active_run_pins_) { --*active_run_pins_; } }
        class run_pin {
        public:
            explicit run_pin(engine* a_engine) noexcept : pins_(a_engine ? a_engine->active_run_pins_ : nullptr) { if (pins_) { ++*pins_; } }
            ~run_pin() { if (pins_) { --*pins_; } }
            run_pin(const run_pin&) = delete;
            run_pin& operator=(const run_pin&) = delete;
        private:
            int* pins_;
        };
        // The innermost active run's pin count (null: no run is active, so nothing pins); set by every switch into a run
        int* active_run_pins() const noexcept { return active_run_pins_; }
        void active_run_pins(int* pins) noexcept { active_run_pins_ = pins; }
        // A debug sync point (sleep with no duration): pending breakpoint edits apply and a pending pause parks here; one
        // pointer test while no debugger exists
        void debug_sync_point();
        // Internal (script_run.cpp): the run machinery's registry, the exchange of the engine's own per-entry state with a run's
        detail::run_registry& run_registry();
        void swap_run_entry_state(detail::run_entry_state& state) noexcept;
        uint64_t aggregate_lowered_locals() const;   // the typed locals and parameters lowered so far (a coverage count for the gates)
        uint64_t aggregate_lowered_sites() const;    // the function and lambda bodies the lowering rewrote so far (vector buffer statements included)
        // The vm's cell block counters since the engine was made (block entries, boundary operations, resumed blocks, misses): a
        // testing lever, counted on those paths only; zeros on the interpreter
        cell_counters cells_counters() const;
#ifdef JAISCRIPT_VM_COVERAGE
        void vm_coverage_adopt(std::shared_ptr<vm::coverage_tables> tables);   // a vm backend's tables, kept past the backend
#endif
        bool host_function_serial(const std::string& name) const;
        // A free function the host (or the library) registered by name, as opposed to a function value script made
        bool host_function_registered(const std::string& name) const;
        // A global the host (or the library) registered with add_global, as opposed to a variable script declared
        bool host_global_registered(const std::string& name) const;
        // The argument positions any registration of `name` writes back through (non-const T& parameters), a bit each
        uint64_t host_function_writeback(const std::string& name) const;
        // Every entry of a namespace the library owns whole (reflect::, spider::, time::) is serial
        void serial_namespace(const std::string& space);
        // Every free function registered by name (host_function_registered's set)
        std::vector<std::string> host_function_names() const;
        // The running parallel region's number (0 outside one): what a chunk makes carries it (class_instance::region_tag)
        uint32_t parallel_region_serial() const;
        // The scalar result every registration of `name` declares by its C++ return type (int, float, bool, char,
        // string); false when a registration returns anything else or registrations disagree
        bool host_function_result(const std::string& name, script_value_type& out) const;
        // the indexes and refusals registered for a callable (empty for most): what the checker asks at a call
        std::vector<std::pair<size_t, const std::string*>> constant_argument_indexes(const std::string& callable) const;
        bool has_constant_arguments() const;
        // the program's const texts by name, as the units gathered so far declare them (detail::constant_texts reads it)
        const std::unordered_map<std::string, std::vector<std::string>>& constant_texts_by_name() const;

        // Per-engine execution-limit state (terminal-error latch, memory accounting)
        // shared by the backend and the builtin-method chokepoints. Internal. While a
        // parallel region's workers are running this resolves to the CALLING WORKER's
        // own accounting; outside a region it is the plain
        // engine instance with one never-taken null test.
        detail::execution_limits& execution_limits() noexcept;

        // How long a pool worker (and the joining thread) spins for the next region before
        // parking on the kernel, when regions arrive at least that densely (a frame's worth of
        // fork-joins); sparse regions park at once. 0 = always park. Default 1000 us.
        void parallel_spin_wait_us(size_t microseconds);
        size_t parallel_spin_wait_us() const;

        // Hot-reload work counter: per-instance field migrations performed since engine
        // construction. Tests assert DELTAS of this instead of wall-clock ratios (a
        // fields-unchanged reload proves itself by migrating zero instances).
        size_t hot_reload_field_migrations() const { return hot_reload_field_migrations_; }
        void count_hot_reload_field_migration() { ++hot_reload_field_migrations_; }
        // Frames a trace capture reads to count the grafts in the frames past the ones it shows (the trace-cost tests' work counter)
        uint64_t trace_graft_frame_reads() const { return trace_graft_frame_reads_; }
        void count_trace_graft_frame_reads(uint64_t frames) { trace_graft_frame_reads_ += frames; }
        void record_field_migration(std::string_view class_name, uint64_t field_id, field_migration_outcome outcome,
                                    const script_value* before, const script_value* after);

        // Serializes IN-REGION chunk compiles engine-wide (a constructor lazily compiles on worker
        // threads; compilation copies AST literal strong_ptrs whose counts are non-atomic).
        // Never taken outside an active parallel region.
        // unique_ptr: the engine is movable, a mutex is not.
        std::mutex& parallel_compile_mutex() { return *parallel_compile_mutex_; }

        // The most recent parallel region's cumulative element counts per thread (W+1 entries for W
        // threads). A weight-hinted region's are the contiguous chunks the hint chose, fixed by the
        // weights; a hint-less region's count what each thread took from the shared blocks, which
        // follows timing. Instrumentation for tests/benchmarks — never observable from script.
        const std::vector<size_t>& last_parallel_chunk_bounds() const;

        // How the most recent parallel_transform provisioned each captured enclosing
        // name (scalar/string/borrow/snapshot), keyed by INTERNED symbol id (resolve via
        // symbolize()). Instrumentation: pins the borrow-vs-snapshot classification in
        // tests — never observable from script.
        const std::vector<std::pair<uint64_t, detail::parallel_capture_kind>>& last_parallel_captures() const;

        // Internal: engine-owned parallel machinery (lazily created) and the per-worker
        // provisioning helper (fresh copies of registered callables whose invocation
        // shares no refcounts with the engine's stored values).
        detail::parallel_engine_state& parallel_state();
        script_value make_parallel_host_function_copy(const std::string& name);
        size_t interned_type_count() const;   // internal: the type interner's size (a region's prewarm repeats nothing while it holds)

        // Stdlib math builtins the VM evaluates through the math:: kernel while the name still binds the function registered
        // here (compared per call; any rebinding or re-registration takes the real call). The name must hold exactly one
        // registration and `intrinsic` must name a kernel whose result equals it for int and float arguments
        // (detail::math_alias_kernel); anything else registers nothing.
        struct math_alias_row {
            const void* identity = nullptr;   // &value.unchecked_as_function()
            script_value value{std::monostate{}, nullptr};   // pins the node: a freed function's address never matches later
        };
        static constexpr size_t k_math_alias_rows = 64;
        void register_math_alias(std::string_view name, std::string_view intrinsic);
        uint8_t math_alias_fn(uint64_t symbol) const;   // 0xFF: not an alias
        const math_alias_row* math_alias_rows() const;
        struct host_direct_row {
            const void* identity = nullptr;   // &value.unchecked_as_function()
            script_value value{std::monostate{}, nullptr};   // pins the node: a freed function's address never matches later
            host_direct_invoke invoke = nullptr;
            const void* target = nullptr;
            size_t arity = 0;
        };
        const host_direct_row* host_direct_for(const void* identity) const;
        const uint64_t* host_direct_generation() const;   // never moves: a row holds for as long as its function is the callee

        // The file whose code is executing right now: the innermost frame's statement, as the
        // parser stamped it (the resolved path an include/import loaded, the path execute_file was
        // given, "<script>" for raw content). Empty when nothing is executing.
        std::string current_source_file() const;
        // The file the outermost execute_file/execute_file_source is running, kept after it returns
        // (a host calling script functions later still gets the program's root). Empty before the
        // first file execute; a nested execute from inside a running script does not replace it.
        std::string root_source_file() const;
        void note_host_entry();
        // The host entry running now (an execute, execute_file or host call into script, with everything it runs): one execute
        // context. A definition in another context, or in a re-run of its unit, redefines instead of conflicting
        uint64_t host_entry_generation() const noexcept;
        // Debug builds: the thread running the engine's outermost execution. An instance may die only there, on a thread of
        // the region it runs, or while the engine is idle (Release records nothing and answers true)
        bool debug_enter_execution() noexcept;   // true when this call became the outermost
        void debug_leave_execution() noexcept;
        bool debug_releases_here() const noexcept;
        class debug_execution_scope {
        public:
#ifndef NDEBUG
            explicit debug_execution_scope(engine& a_engine) noexcept : engine_(a_engine.debug_enter_execution() ? &a_engine : nullptr) {}
            ~debug_execution_scope() { if (engine_) { engine_->debug_leave_execution(); } }
            debug_execution_scope(const debug_execution_scope&) = delete;
            debug_execution_scope& operator=(const debug_execution_scope&) = delete;
        private:
            engine* engine_;
#else
            explicit debug_execution_scope(engine&) noexcept {}
#endif
        };
        bool unit_rerun(unit_stamp unit) const;
        void note_execute_elapsed_use();

        // The object data owns, as a value: a by-value copy runs the copy constructor (a host T result boxes this way).
        // Defined in engine_impl.hpp where class_definition is complete.
        template<typename T>
        script_value make_value_object(std::shared_ptr<T> data);

        // A host std::shared_ptr<T> entering script is a handle, as new T(...) makes one: copies share the object
        template<typename T>
        script_value make_handle(std::shared_ptr<T> data) { return marked_shared_handle(make_value_object(std::move(data))); }
        script_value marked_shared_handle(script_value object);
        // Interns the handle type of def's objects, ahead of a parallel region whose chunks a host call can hand one
        void prewarm_handle_type(const class_definition& def);
        // A value this engine made, or one that names no engine (a host literal): a heap payload records its engine, a
        // scalar's stamp is one of this engine's types
        bool owns_value(const script_value& value) const noexcept;
        // Another engine's value as this engine's: numbers, strings and math values copy, arrays and maps convert deeply; an
        // object, a function or a reference into the other engine's memory throws, as does a held value that outlived its engine
        script_value import_value(const script_value& value);

        // Create empty weak_ptr value with specified type
        script_value make_empty_weak_ptr(type_info_ptr weak_ptr_type) {
            return script_value::make_empty_weak_ptr(weak_ptr_type, this);
        }
        void add_overloaded_function_with_full_types(const std::string& name, size_t arg_count, script_function func, const std::vector<param_type_info>& param_types);
        void add_function_with_arity_and_full_types(const std::string& name, script_function func, size_t arity, const std::vector<param_type_info>& param_types);
        // the same, declaring the scalar result the checker sees (add_function reads it from the C++ return type)
        void add_function_with_arity_and_full_types(const std::string& name, script_function func, size_t arity, const std::vector<param_type_info>& param_types, script_value_type result);
        
        // Get all registered template types for parser
        std::unordered_set<std::string> get_registered_template_types() const;
        
        // Get class definition by name
        std::shared_ptr<class_definition> get_class_definition(const std::string& name) const;

        // Get class definition by type_id (faster than string lookup)
        std::shared_ptr<class_definition> get_class_definition(uint64_t type_id) const;

        // Get registered script name for a C++ type
        template<typename T>
        std::string get_registered_name() const;

        // Enhanced Conversion System
        // Get the conversion manager for this engine
        conversions::conversion_manager get_conversion_manager();
        
        // Register standard vector and map conversions
        void add_standard_conversions();
        
        // Register bound_array conversion for a specific type
        template<typename T>
        void add_bound_array_conversion() {
            auto manager = get_conversion_manager();
            manager.add_bound_array_conversion<T>();
        }
        
        // Register bound_map conversion for specific key/value types
        template<typename K, typename V>
        void add_bound_map_conversion() {
            auto manager = get_conversion_manager();
            manager.add_bound_map_conversion<K, V>();
        }
        // const program-scope and namespace cells: marked by their declaration, consulted by the checker and the
        // host's writes (add_global / add_global_ref refuse a const)
        void mark_const_cell(const script_value* cell, bool is_const);
        bool const_cell(const script_value* cell) const;
        // the counters a value read once from a program-scope const validates against, read in place on a hot path: the first moves
        // at every program-scope declaration (each marks its cell), the second is definition_epoch's
        const uint64_t* const_epoch_counter() const;
        const uint64_t* definition_epoch_counter() const;
        bool is_const_global(const std::string& name) const;
        bool is_type_name(const std::string& name) const;
        
        // Get the global environment (for internal use)
        std::shared_ptr<environment> get_global_environment() const;
        
        // Performance optimization
        void set_has_custom_numeric_operators(bool value);
        execution_backend* get_execution_backend() const;
        // The backend running script on the calling thread: a parallel region's chunk runs on its own
        execution_backend* calling_backend() const {
            return parallel_region_active_ ? region_chunk_backend() : get_execution_backend();
        }
        execution_backend* region_chunk_backend() const;
        // A parallel chunk's call of a class operator through its region pin, on the chunk's backend; nullopt outside a region or for an
        // operator the region did not pin
        std::optional<checked_result<script_value>> parallel_operator_call(const script_value& self, uint64_t id, std::vector<script_value> args) const;
        bool backend_unwinding() const;
        std::string backend_exception_text() const;   // the text of the exception the backend is unwinding with

        // Engine-owned script namespace registry (namespace_id -> data); survives across backends
        std::unordered_map<uint64_t, std::shared_ptr<script_namespace_data>>& script_namespaces();
        // Names an enum declaration defined (both backends note them): `E::v` reads the value through the enum's variable
        void note_enum_name(uint64_t name_id);
        bool is_enum_name(uint64_t name_id) const;

        // Get string symbolizer for interning identifiers
        string_symbolizer* get_symbolizer();

        // Intern a string and return its unique ID (const-safe)
        uint64_t symbolize(std::string_view str) const;
        // A transient error text's symbol (string_symbolizer::park): it rides an error's {0} without growing the
        // interned table. While frozen it is the text's interned id when one exists, else 0 (the caller's slot-less text)
        uint64_t park_error_text(std::string_view text) const;
        // template(path, ...) with a path known at run time (detail/template_library.hpp): the unit `source` (a lambda over the
        // template) run under `unit`, its parse or check failure as the error; and whether that unit's parse still stands for
        // every file it read
        checked_result<script_value> run_template_unit(const std::string& unit, const std::string& source);
        bool template_unit_current(const std::string& unit, const std::string& source);

        // Get class definition by type index
        std::shared_ptr<class_definition> get_class_definition_by_type(const std::type_index& type) const;
        
        // Get the conversion registry - shareable between engines
        std::shared_ptr<conversions::conversion_registry> get_conversion_registry() const;
        
        // Set a shared conversion registry (for sharing between engine instances)
        void set_conversion_registry(std::shared_ptr<conversions::conversion_registry> registry);
        
        // Get the serialization registry for this engine
        serialization::serialization_registry& get_serialization_registry();
        
        // Get the class registry for this engine
        class_registry& get_class_registry();

        // Map path for types without the slot, keyed by the native object's pointer.
        // note_script_layer is called from class_instance::set_native.
        void note_script_layer(const void* cpp_object, const weaker_ptr<class_instance>& owner);
        strong_ptr<class_instance> script_layer_of(const void* cpp_object) const;
        // The instance script already holds for a native object: its layer, or the plain wrapper make_object handed out
        // (one identity per object, so extend re-classes the object every holder sees)
        strong_ptr<class_instance> object_instance_of(const void* cpp_object) const;
        // The same lookup taking no count, and the borrow a parallel worker reads such a shared instance through
        // (detail/object_borrow.hpp): what make_value_object hands out while a region runs
        const class_instance* object_instance_peek(const void* cpp_object) const;
        script_value parallel_borrow_of(const class_instance& source, type_info_ptr tag);
        // Archive: $type (script class) + $script (fields as JSON) for a layer this engine owns;
        // the C++ base a script class extends ("" when it is not such a class); re-attachment
        // around a freshly loaded object (initializers, then hydration, then post_load) - the
        // caller aliases the returned layer only when the object cannot own it.
        bool script_layer_save(const strong_ptr<class_instance>& layer, std::string& script_class_name, std::string& fields_json);
        std::string script_layer_base_name(const std::string& script_class_name);
        strong_ptr<class_instance> extend_loaded(const std::string& script_class_name,
            const std::shared_ptr<void>& cpp_object, const std::string& fields_json);
        bool script_layer_object_owned(const strong_ptr<class_instance>& layer) const;
        // A load constructs a class that has a zero-argument constructor (any access, its initializer list included, so
        // a script class over a C++ base builds that base), then loads the saved values over it, then runs post_load
        bool script_class_default_constructible(const class_definition& cls) const;
        // The archive's hybrid load when the script class has a zero-argument constructor: the constructed instance and
        // the C++ object its constructor built (null when the class has none); then the saved $script fields hydrate over
        // it and post_load runs
        strong_ptr<class_instance> construct_script_layer_for_load(const std::string& script_class_name, std::shared_ptr<void>& cpp_object);
        checked_result<void> hydrate_script_layer(const strong_ptr<class_instance>& layer, const std::string& fields_json);
        std::string detach_object(const strong_ptr<class_instance>& layer);
        checked_result<std::shared_ptr<class_definition>> resolve_script_class(const script_value& script_class);
        checked_result<strong_ptr<class_instance>> build_script_layer(const std::shared_ptr<class_definition>& script_def,
            const std::shared_ptr<class_definition>& base_def, const std::shared_ptr<void>& object, bool engine_owned,
            const strong_ptr<class_instance>& carry_from, const std::string& fields_json);

        // Monotonic counter bumped on every script-class (re)definition. Class-shape
        // caches (class_definition::flat_value_semantics) validate against it: a nested
        // field class redefining must invalidate its CONTAINING classes' stamps too, and
        // per-class epochs cannot see containment.
        uint64_t class_definition_epoch() const;
        void bump_class_definition_epoch();
        // moves whenever any class's methods or field shape change: a frame that addresses its receiver's fields by slot (the vm's
        // member-class bodies) took its receiver's layout under one value and drops back to the general reads under any other
        const uint64_t* layout_generation() const noexcept;
        void bump_layout_generation() noexcept;
        // Monotonic counter bumped whenever the set of definitions a call could resolve
        // against changes (every top-level run, every function/class/global (re)definition,
        // every host registration). Cross-function proofs cache against it.
        uint64_t definition_epoch() const;
        void bump_definition_epoch();
        // moves when a declaration that can change a folded scoped const runs (a class, a namespace, an enum at program scope)
        uint64_t fold_epoch() const;
        void bump_fold_epoch();
        // A program unit's name for diagnostics: its file's path, <script> for a literal, <host> for unit 0
        std::string unit_label(uint32_t unit) const;
        // The shadowing unit whose code is running (0 = the host: a file by path or a literal by
        // content, numbered at first execution) and its execution number
        unit_stamp current_unit() const;
        // Read-only by-value parameter binds that shared the argument instead of cloning it
        // (main thread; a test observable for the elision)
        uint64_t parameter_alias_binds() const;
        void note_parameter_alias_bind();
        // The release stack (detail/release_machine.hpp); _here is null while a parallel region runs (workers release natively)
        detail::release_machine& release_machine();
        detail::release_machine* release_machine_here();
        // Native destruction levels before a dying object's members move to the release stack (default 64; 0: always)
        void release_recursion_limit(uint32_t levels);
        uint32_t release_recursion_limit() const;
        uint64_t local_alias_binds() const;
        void note_local_alias_bind();

        // Script-class SHAPE change (first definition, non-identical redefinition): the static
        // checker's surface moved, cached entries re-check (and re-stamp proofs) at their next
        // execute. Identical reloads must NOT call this.
        void note_class_shape_change();
        // A script class's first definition: news to the checker unless the program pools model this very declaration
        void note_class_defined(const std::string& name, const class_decl* declaration);

        // === TYPE INFO INTERNING ===
        // Get or create type_info for basic types (fast O(1) access for common types)
        type_info* get_type_info_int();
        type_info* get_type_info_float();
        type_info* get_type_info_string();
        type_info* get_type_info_utf8_string();
        type_info* get_type_info_math(script_value_type kind);   // the canonical stamp of math::vec2/vec3/ivec2/ivec3
        type_info* get_type_info_bool();
        type_info* get_type_info_char();
        type_info* get_type_info_uint8();   // array<uint8>'s element type
        type_info* get_type_info_int8();    // array<int8>'s element type
        type_info* get_type_info_void();
        type_info* get_type_info_invalid();
        type_info* get_type_info_any();  // 'var' marker (dynamic); var-held values stay fully dynamic

        // Get or create type_info for arrays/maps (interned by type_id)
        type_info* get_type_info_array(type_info* element_type);
        type_info* get_type_info_map(type_info* key_type, type_info* value_type);
        type_info* get_type_info_weak_ptr(type_info* pointee_type);
        type_info* get_type_info_shared_ptr(type_info* pointee_type);
        // var-held-handle twin of get_type_info_shared_ptr: identical shape/name/id but
        // dynamic_pointee=true, so '=' with a handle rhs rebinds unchecked
        type_info* get_type_info_shared_ptr_dynamic(type_info* pointee_type);
        // A var slot's twin of a typed array's tag: dynamic_slot=true
        type_info* get_type_info_array_dynamic(type_info* array_type);
		type_info* get_type_info_map_dynamic(type_info* a_mapType);
        type_info* get_type_info_reference(type_info* referenced_type);
        type_info* get_type_info_declared_reference(type_info* referenced_type);
        type_info* get_type_info_function(type_info* return_type, const std::vector<type_info*>& arg_types);
        // the type every function value carries (void(), interned once per engine)
        type_info* get_type_info_untyped_function();

        // Get or create type_info for object types (uses class_definition's type_info)
        type_info* get_type_info_object(const std::string& class_name);
        type_info* get_type_info_object(uint64_t type_id);

        // Generic helper to intern any type_info
        type_info* get_type_info(const type_info& temp);

        // Direct lookup by interned ID (fastest - O(1) with no string operations)
        type_info* get_type_info_by_id(uint64_t type_id);

        // Lookup by canonical name (interns the name first, then does ID lookup)
        type_info* get_type_info_by_name(const std::string& canonical_name);

        // Template method to get type_info for any C++ type
        template<typename T>
        type_info* get_type_info_for_cpp_type() {
            if (!get_symbolizer()) return nullptr;
            type_info temp = type_info::make<T>(*get_symbolizer());
            return get_type_info(temp);
        }

        // INTERNAL (value.cpp): per-engine reference_holder block pool, held type-erased so
        // the private holder type never crosses this header. value.cpp lazily creates the
        // pool into the slot and registers the teardown; the engine dtor invokes it.
        void*& reference_holder_pool_slot();
        void set_reference_holder_pool_teardown(void (*teardown)(void*));
        // The same for the math vector node pool and the object_holder pool
        void*& vec_node_pool_slot();
        void set_vec_node_pool_teardown(void (*teardown)(void*));
        void*& object_holder_pool_slot();
        void set_object_holder_pool_teardown(void (*teardown)(void*));
        void*& array_node_pool_slot();
        void set_array_node_pool_teardown(void (*teardown)(void*));
        // The weak list of cells an escaped closure kept past a frame-exit cycle check (script_value::release_cell_cycles)
        void*& cell_cycle_registry_slot();
        void set_cell_cycle_registry_teardown(void (*teardown)(void*));
        // True while a parallel region's workers run (main thread parked). Worker-side
        // mints must not touch the single-threaded pool.
        bool parallel_region_active() const noexcept { return parallel_region_active_; }
        // the region guard's record of its region (on while workers run, back to the enclosing region's state after)
        void note_parallel_region(bool active) noexcept { parallel_region_active_ = active; }
        // Workers copy and construct class instances (by-value bindings, whole reads): the
        // class-definition instance registry they register into is engine-shared, so its
        // mutations serialize on this lock while a region runs (unlocked otherwise).
        std::unique_lock<std::mutex> parallel_instance_registry_lock();
        // While a region runs, a chunk's thread parks an instance it constructed or copied with the element it is running (false
        // outside a region's chunk): class_definition::register_instance parks there, the join registers the survivors.
        bool park_parallel_registration(weaker_ptr<class_instance>& instance);
        // Cold class-reload boundary: recover surviving worker copies after a failed join.
        void recover_parallel_registrations();
        void report_script_error(const std::string& a_message, std::error_code a_code = {});
        void report_script_warning(const std::string& a_message);

        // Shelve/restore the in-flight script call's argument metadata around an external
        // (C++) invocation of a script function - see external_call_guard below.
        void push_external_call_scope();
        void pop_external_call_scope();

        // The checked_result flavor's host entry: `run` executes on the backend, and a call made while no
        // script runs reports every failure in its result (a run-time error, an uncaught throw, a failed import or reload), the
        // engine left ready for the next call; a call from inside running script passes straight through
        using host_entry_body = checked_result<script_value> (*)(execution_backend&, void*);
        // (host_call false: running script made the call, which pins no suspendable run)
        checked_result<script_value> checked_host_entry(host_entry_body run, void* context, bool host_call = true);

        // An overloaded host function name's callable: the overload the arguments select
        struct overload_dispatch {
            engine* eng;
            uint64_t name_id;
            std::string name;
            const void* set = nullptr;   // the engine's overload set of the name: its entry is never erased
            checked_result<script_value> operator()(const std::vector<script_value>& args) const;
            bool try_direct(const script_value* args, size_t argc, std::optional<checked_result<script_value>>& out) const;
            // the one overload try_direct runs, for a caller that keeps it: its binding and invoker, each parameter's kind, and the
            // function that owns the binding (held, the binding outlives a re-registration)
            struct direct_snapshot {
                const void* target = nullptr;
                checked_result<script_value> (*invoke)(const void*, const script_value*, size_t) = nullptr;
                size_t argc = 0;
                script_value_type kinds[8]{};
                script_value owner{std::monostate{}, nullptr};
                const uint64_t* generation = nullptr;   // the set's mutation count, read where it lives: the overload holds while it reads at
                uint64_t at = 0;
            };
            bool direct_entry(direct_snapshot& out) const;
            // how the overloads taking argc arguments bind argument i (detail::arg_binding bits)
            uint8_t argument_binding(size_t i, size_t argc) const;
        };

        // Get the current output stream (defaults to std::cout)
        std::ostream& get_output_stream();

        // Include/Import path management
        void include_paths(const std::vector<std::string>& paths);
        void clear_include_paths();
        std::vector<std::string> get_include_paths() const;
        bool outside_confined_paths(const std::string& canonicalPath) const;

        // The resolution ladder's memo (resolve_include_path): a name asked from a folder answers its earlier canonical result
        // while every rung before it still holds no file and the hit still exists (one attribute query each, instead of the
        // canonicalizing walk); an include-path change forgets every answer
        const std::string* remembered_resolution(const std::string& key);
        void remember_resolution(const std::string& key, std::vector<std::string> misses, std::string candidate, std::string resolved);
        std::string resolution_key(const std::string& path, const std::string& from_file);   // the working directory cached, refreshed when an import check pass opens
        // The builtin method names a receiver of a kind answers (0 array, 1 map, 2 string, 3 utf8_string): the one registry
        // both backends dispatch through, read once (the static checker's member check on typed containers)
        const std::unordered_set<uint64_t>& builtin_method_ids(int kind);
        import_behavior get_import_behavior() const;
        // Both backends' import step. import_skips: no pass is open, the window has not expired and the site has run its
        // modules before (compares against the clock sample and the site's memory; a computed site also hashes its paths).
        // import_modules: the reach the pass lets through - resolve, stat, parse and check every changed module before any
        // runs, run them in order, then the changed modules the unchanged ones imported (each alone); true = a module of the
        // tree (re)ran. Throws what a load or a run raises.
        bool import_skips(const import_site& site) noexcept;
        bool import_skips(const import_site& site, const script_value* const* args, size_t count);
        checked_result<bool> import_modules(import_site& site, const std::vector<std::string>& paths, bool force);
        // Filesystem queries the import step has made (resolution, stamps): the counter the throttle's tests read
        uint64_t import_stat_count() const noexcept;
        // Source texts the loads read from disk, and diagnostics walks the cached checker ran: what a load costs, counted
        uint64_t source_read_count() const noexcept;
        uint64_t check_walk_count() const noexcept;
        // Writes of the engine's clock sample (host entries, budget-check samples, time:: reads of the system clock)
        uint64_t clock_sample_count() const noexcept;

        // The catch variable's value, both backends at the catch: the error's message as a string (every string operation
        // reads it unchanged) that also answers e.message, e.file, e.line, e.column, e.stack (file:line per frame,
        // innermost first) and e.value (a throw's value; null for a run-time error). The place is the throw's, else the
        // innermost frame's statement.
        // thrown_text: the throw's own message (a thrown object's to_string(), engine::text_of), spoken for a value that is no text
        script_value make_caught_error(const script_value& thrown, bool by_throw, const source_location* throw_site,
                                       const std::vector<stack_frame>& trace, const char* thrown_text = nullptr);
        // The backends' catch: the captured frames move into the error and are spelled only when e.file, e.line,
        // e.column or e.stack is read
        script_value make_caught_error(const script_value& thrown, bool by_throw, const source_location* throw_site,
                                       std::vector<stack_frame>&& trace, const char* thrown_text = nullptr);
        // A member read on a string receiver that is a caught error (nullopt: not one, or not an error member; a thrown
        // object or map answers its own fields)
        std::optional<script_value> caught_error_member(const script_value& receiver, uint64_t member_id);
        // The handler that bound `caught` is done and its variable let go: the thrown value dies, as C++ destroys the exception
        // object when its handler exits, unless a copy of the error (a capture, a stored e) still holds it
        void end_caught_error(const script_value& caught) noexcept;
        // A suspended coroutine's frame dropped whole: its values die last slot first, and a handler it was suspended in lets go
        // of its thrown value as the slot of its catch variable dies
        void release_suspended_values(script_value* values, size_t count) noexcept;
        
        // Execute a file with import tracking
        script_value execute_import(const std::string& resolved_path);
        script_value execute_unheld(script_source source);
        script_value execute_unheld(jai::jaibite& bite);
        script_value execute_file_unheld(const std::string& script_path, const instance_variables& instanceVars);
        script_value get_variable_unheld(const std::string& name) const;

        // Parameter storage for function calls (replaces thread_local)
        detail::parameter_storage* get_current_parameter_storage() const;
        void set_current_parameter_storage(detail::parameter_storage* storage);

        // Rank same-arity C++ method overload candidates (each a parameter-type signature) against
        // a call's actual arguments (args[0] = 'this'); returns the best candidate index, or -1.
        // Shares the free-function overload scorer so resolution stays consistent.
        // The name a class's reflected binary operator binds under (C++'s free op(L, const T&) bound for T): no script spelling reaches it
        static std::string reflected_operator_name(std::string_view op) { return "reflected " + std::string(op); }
        // The reflected binding's name id for a binary operator's id (0: not a binary operator)
        uint64_t reflected_operator_id(uint64_t op_id) const noexcept;
        // The compound form's name id for a binary operator's id ("+" -> "+="; 0: none)
        uint64_t compound_operator_id(uint64_t op_id) const noexcept;
        // The names a class value map key's class orders, compares and hashes through (core/script_map.hpp)
        struct map_key_ids { uint64_t less = 0; uint64_t spaceship = 0; uint64_t equal = 0; uint64_t hash = 0; };
        const map_key_ids& map_key_operator_ids() const noexcept;
        // A C++ signature's match cost for the arguments at args (free-function overload ranking), -1 when it does not take them
        int cpp_signature_cost(const script_value* args, const std::vector<param_type_info>& signature) const;

        // Expires when this engine is destroyed: script callbacks a host keeps (signal receivers, converted std::functions)
        // check it before they run, so a destroy order that leaves them behind never reaches the dead engine
        std::weak_ptr<const void> lifetime_token() const noexcept { return lifetime_; }

        // An archive load constructs a class through its constructor whatever the constructor's access (as reads see
        // private state): held around the load's constructor call, taken by the first constructor dispatch inside it
        class construction_access_waiver {
        public:
            explicit construction_access_waiver(engine* a_engine) : eng(a_engine) { eng->construction_access_waived_ = true; }
            ~construction_access_waiver() { eng->construction_access_waived_ = false; }
            construction_access_waiver(const construction_access_waiver&) = delete;
            construction_access_waiver& operator=(const construction_access_waiver&) = delete;
        private:
            engine* eng;
        };
        bool take_construction_access_waiver() noexcept { return std::exchange(construction_access_waived_, false); }

        // A load in progress (from_json, from_binary, a C++ archive read, extend with saved fields): its writes do not
        // notify (observable properties write silently, script receivers skip), and each loaded object's post_load waits
        // until the outermost load completes, then runs once per object in completion order (children before parents).
        // complete() runs them and may throw (a post_load's error fails the load); a session left without complete()
        // (the load threw) drops what it queued.
        class load_session {
        public:
            explicit load_session(engine* a_engine);
            ~load_session();
            void complete();
            load_session(const load_session&) = delete;
            load_session& operator=(const load_session&) = delete;
        private:
            engine* eng;
            bool completed = false;
        };
        // A class's properties in the order a load restores them (its declaration order, base first), kept for the
        // load in progress
        struct load_field {
            uint64_t id;
            uint64_t setter_id;
            script_value key;
        };
        const std::vector<load_field>& load_field_order(const class_definition& cls);
        // Queues self.post_load(version) for the outermost load, or runs it now when no load is in progress
        void defer_post_load(const script_value& self, script_int version);
        // Program-scope storage in the order it came to be (a global's or a namespace variable's cell, a class static): the
        // engine's teardown releases it in reverse while destructors still run
        void note_program_scope_cell(environment* scope, size_t slot);
        void note_static_field(class_definition* cls, uint64_t id);
        // Map C++ types to param_type_info (with type_index for object types). Public so the
        // binding layer (dynamic_binder) can build per-overload C++ parameter signatures.
        template<typename T>
        static param_type_info map_cpp_type_to_param_info() {
            using decay_t = std::decay_t<T>;

            if constexpr (detail::is_script_value_v<decay_t>) {
                // Wildcard: a script_value parameter accepts ANY argument type (a generic binding,
                // e.g. pair(script_value, script_value)). jai_null_type is the sentinel — no real
                // C++ binding parameter is genuinely null-typed — and the overload scorer gives it a
                // small fixed cost so a concrete-typed overload still ranks ahead.
                return param_type_info(script_value_type::jai_null_type);
            } else if constexpr (std::is_same_v<decay_t, int> || std::is_same_v<decay_t, int64_t> ||
                          std::is_same_v<decay_t, script_int> || std::is_enum_v<decay_t>) {
                return param_type_info(script_value_type::jai_int_type);
            } else if constexpr (is_specialization_v<decay_t, std::function>) {
                return param_type_info(script_value_type::jai_function_type);
            } else if constexpr (std::is_same_v<decay_t, float> || std::is_same_v<decay_t, double> ||
                                 std::is_same_v<decay_t, script_float>) {
                return param_type_info(script_value_type::jai_float_type);
            } else if constexpr (std::is_same_v<decay_t, bool> || std::is_same_v<decay_t, script_bool>) {
                return param_type_info(script_value_type::jai_bool_type);
            } else if constexpr (std::is_same_v<decay_t, char> || std::is_same_v<decay_t, script_char>) {
                return param_type_info(script_value_type::jai_char_type);
            } else if constexpr (std::is_same_v<decay_t, std::string> || std::is_same_v<decay_t, script_string>) {
                return param_type_info(script_value_type::jai_string_type);
            } else if constexpr (is_specialization_v<decay_t, std::vector>) {
                return param_type_info(script_value_type::jai_array_type);
            } else if constexpr (is_specialization_v<decay_t, bound_array>) {
                return param_type_info(script_value_type::jai_array_type);
            } else if constexpr (is_script_map_like_v<decay_t>) {
                return param_type_info(script_value_type::jai_map_type);
            } else if constexpr (is_specialization_v<decay_t, bound_map>) {
                return param_type_info(script_value_type::jai_map_type);
            } else if constexpr (is_specialization_v<decay_t, std::shared_ptr>) {
                using element_type = typename decay_t::element_type;
                return param_type_info(script_value_type::jai_shared_ptr_type, std::type_index(typeid(element_type)));
            } else if constexpr (is_specialization_v<decay_t, std::weak_ptr>) {
                using element_type = typename decay_t::element_type;
                return param_type_info(script_value_type::jai_weak_ptr_type, std::type_index(typeid(element_type)));
            } else if constexpr (math_adapted<decay_t>) {
                return param_type_info(math_adapter<decay_t>::kind);
            } else {
                return param_type_info(script_value_type::jai_object_type, std::type_index(typeid(decay_t)));
            }
        }
        template <typename R>
        static constexpr script_value_type host_result_kind() {
            using D = std::decay_t<R>;
            if constexpr (std::is_same_v<D, int> || std::is_same_v<D, int64_t> || std::is_same_v<D, script_int>) { return script_value_type::jai_int_type; }
            else if constexpr (std::is_same_v<D, float> || std::is_same_v<D, double>) { return script_value_type::jai_float_type; }
            else if constexpr (std::is_same_v<D, bool>) { return script_value_type::jai_bool_type; }
            else if constexpr (std::is_same_v<D, char> || std::is_same_v<D, script_char>) { return script_value_type::jai_char_type; }
            else if constexpr (std::is_same_v<D, std::string>) { return script_value_type::jai_string_type; }
            else { return script_value_type::jai_null_type; }
        }
        // the result kind of the registration about to be made (register_overload_impl consumes it; unstaged = unknown)
        void stage_host_result(script_value_type kind);
        // the registration about to be made is a by-value binding: its callable's own entry, found inside the stored function
        struct host_direct_entry {
            checked_result<script_value> (*invoke)(const void*, const script_value*, size_t) = nullptr;
            const void* (*locate)(const script_function&) = nullptr;
        };
        void stage_host_direct(host_direct_entry entry);

        // The single overloaded-function registration algorithm shared by every public entry point
        // (add_function*, add_overloaded_function*, add_variadic_function). create_set_on_first is the
        // only policy difference between the entry points.
        void register_overload_impl(const std::string& name, size_t arity, script_function func,
                                    const std::vector<param_type_info>& param_types, bool create_set_on_first);
        // a qualified registration ("game::tick") is also a host entry of its namespace, as the library's namespaces register theirs
        void publish_namespace_entry(const std::string& name);
        friend class held_value;
        void register_held(held_value* held);
        void unregister_held(held_value* held) noexcept;
        void release_held_values() noexcept;
        void release_program_scope_in_reverse() noexcept;
        // declared ahead of impl: alive through the whole teardown, expired once the engine is gone
        std::shared_ptr<const void> lifetime_ = std::make_shared<char>('\0');
        struct implementation;
        std::unique_ptr<implementation> impl;

        size_t hot_reload_field_migrations_ = 0;
        uint64_t trace_graft_frame_reads_ = 0;
        bool hot_reload_reporting_ = false;
        bool parallel_region_active_ = false;
        bool construction_access_waived_ = false;
        int* active_run_pins_ = nullptr;
        std::vector<field_migration_report> hot_reload_reports_;
        void register_location_builtins();   // engine-core script builtins: root/current_script_dir, resolve_path, app dirs
        script_run launch_run(std::unique_ptr<detail::run_record> run);   // start's first switch into the run (script_run.cpp)
        void cancel_suspended_runs() noexcept;   // the engine's teardown: every run still suspended is cancelled
        void register_privilege_builtins();
        void set_privileges(privilege flags, bool enabled);
        void set_privileges(std::span<const std::string_view> names, bool enabled);
        void check_privilege_mutation() const;
        std::optional<uint64_t> find_custom_privilege(std::string_view name) const noexcept;
        struct resolved_privileges {
            privilege builtins = privilege::none;
            std::vector<uint64_t> custom;
        };
        resolved_privileges resolve_privileges(std::span<const std::string_view> names) const;
        checked_result<void> require_privileges(privilege flags, std::span<const uint64_t> slots) const noexcept;
        std::unique_ptr<std::mutex> parallel_compile_mutex_ = std::make_unique<std::mutex>();
        int load_depth_ = 0;
        std::vector<std::pair<script_value, script_int>> deferred_post_loads_;
        std::unordered_map<const class_definition*, std::vector<load_field>> load_field_orders_;

        // Single post-parse execution pipeline shared by execute(string) and
        // execute(jaibite&): every string execute exercises the same machinery a
        // jaibite does, so the pre-parsed path can never drift semantically.
        script_value execute_parsed(const std::vector<declaration_ptr>& declarations,
                                    std::shared_ptr<void>& compiled_slot,
                                    const instance_variables* instanceVars, uint32_t unit, bool textual = false);

        // The one lex+parse+run entry: stamps `source_path` onto every AST node and keys
        // the parse cache on (source_path, content). Not public — attributing raw content
        // to a path is debugger plumbing, a sharp edge for callers. Reached via execute()
        // ("<script>"), execute_file() (the file path), and include/import (resolved path).
        script_value execute_source(const std::string& script_content,
                                    const instance_variables& instanceVars,
                                    const std::string& source_path,
                                    bool textual = false,   // textual: an include, run where it is written
                                    std::string_view include_scope = {});

        // Lazily constructs + wires the selected backend on first use, so creating an
        // engine (or switching backend type) costs nothing until a script actually runs.
        execution_backend* backend() const;

        // Static-check a jaibite once (creation/load/first checked execute) and gate
        // under strict; source is only available at creation time.
        void check_bite(jai::jaibite& bite, const std::string* source);

        void initialize_engine_reference();
        void wire_backend();
        void add_class_impl(const std::string& name, std::shared_ptr<class_definition> classDef);
        void register_class_by_type(std::type_index type, std::shared_ptr<class_definition> classDef);

        // Records a base link whose base class has not registered yet (registrar order is
        // type_index order, not dependency order); resolved when the base arrives.
        void defer_base_link(std::type_index base, std::shared_ptr<class_definition> derived);

        // Allow dynamic_binder to access implementation details
        template<typename T> friend class dynamic_binder;
        // Allow function_binder to access conversion registry
        template<typename T> friend struct detail::value_converter;
        // Allow interpreter to access implementation for include/import
        friend class interpreter;
        // Allow the vm backend the same access (include/import call execute_source)
        friend class vm::vm_backend;
        friend class engine_time;
        
        template<typename Func>
        std::vector<param_type_info> extract_parameter_types_with_info() {
            using traits = detail::function_traits<std::decay_t<Func>>;
            return extract_parameter_types_with_info_impl<typename traits::argument_types>(std::make_index_sequence<traits::arity>{});
        }

        template<typename ArgsTuple, size_t... Is>
        std::vector<param_type_info> extract_parameter_types_with_info_impl(std::index_sequence<Is...>) {
            return {map_cpp_type_to_param_info<std::tuple_element_t<Is, ArgsTuple>>()...};
        }
    };

    // Resolve an include/import path against the engine's include paths. Shared by the
    // interpreter and vm backends (defined in interpreter.cpp) so path resolution stays
    // single-sourced.
    // One resolution ladder for include, import, the checker's closure gather and the
    // resolve_path builtin: absolute as given; relative against from_file's folder, then the
    // include paths in order, then the working directory. Canonical spelling on success.
    // A miss raises `Failed to open include file: <path>` (`import file` for an import)
    checked_result<std::string> resolve_include_path(const std::string& path, engine* engine_ptr, const std::string& from_file,
                                                     bool for_import = false);
    checked_result<std::string> resolve_include_path(const std::string& path, engine* engine_ptr);   // from the executing file
    namespace detail {
        // The same ladder without the filesystem privilege's gate: a scoped template folder resolves as an import's file does,
        // under the scoped_template privilege (detail/template_library.hpp)
        checked_result<std::string> resolve_on_import_ladder(const std::string& path, engine* engine_ptr, const std::string& from_file);
    }

    // A bound catch variable's handler, ended when this dies (both backends hold one per handler that bound a variable)
    class caught_error_scope {
    public:
        caught_error_scope() = default;
        caught_error_scope(engine* a_engine, script_value a_caught) : engine_(a_engine), caught_(std::move(a_caught)) {}
        caught_error_scope(caught_error_scope&& other) noexcept : engine_(std::exchange(other.engine_, nullptr)), caught_(std::move(other.caught_)) {}
        caught_error_scope& operator=(caught_error_scope&& other) noexcept {
            if (this != &other) {
                end();
                engine_ = std::exchange(other.engine_, nullptr);
                caught_ = std::move(other.caught_);
            }
            return *this;
        }
        caught_error_scope(const caught_error_scope&) = delete;
        caught_error_scope& operator=(const caught_error_scope&) = delete;
        ~caught_error_scope() { end(); }
        void end() noexcept {
            if (engine* eng = std::exchange(engine_, nullptr)) {
                eng->end_caught_error(*caught_);
                caught_.reset();
            }
        }
        void dismiss() noexcept { engine_ = nullptr; caught_.reset(); }
    private:
        engine* engine_ = nullptr;
        std::optional<script_value> caught_;
    };

    // typeid(engine) taken where engine is complete; archives compare context requests against it.
    std::type_index engine_context_type_index() noexcept;

} // namespace jai

#include <jaiscript/detail/engine_access.hpp>

#ifndef JAISCRIPT_ENGINE_BODIES_AFTER_CLASS_DEFINITION
#include "engine_impl.hpp"
#include "conversion_registry_templates.hpp"
#include "value_impl.hpp"
#endif

#endif // __JAISCRIPT_CORE_ENGINE_HPP__
