#pragma once

#ifndef __JAISCRIPT_DETAIL_ENVIRONMENT_HPP__
#define __JAISCRIPT_DETAIL_ENVIRONMENT_HPP__

#include "ast.hpp"
#include <jaiscript/core/value.hpp>
#include <jaiscript/core/checked_result.hpp>
#include <jaiscript/detail/env_storage.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jai {

    class string_symbolizer;
    class class_definition;
    class class_instance;

    namespace detail {
        // True when the current thread's remaining native stack is under the safety
        // margin - recursive native call paths raise the catchable recursion error
        // instead of dying on a hardware stack overflow (implemented in environment.cpp)
        bool native_stack_low() noexcept;
    }

    // environment for storing variables in a scope
    // Environment kind - replaces virtual dispatch with simple enum check
    enum class env_kind {
        standard,       // Regular scope (function, block, global)
        method,         // Instance method - has 'this' object for field access
        static_method   // Static method - has class_def for static field access
    };

    class environment {
    public:
        // Standard environment constructor
        environment(string_symbolizer* symbolizer)
            : symbolizer_(symbolizer), kind_(env_kind::standard) {
            bump_env_epoch();
        }

        // Standard environment with parent
        environment(std::shared_ptr<environment> parent, string_symbolizer* symbolizer)
            : parent_(parent), symbolizer_(symbolizer), engine_(parent ? parent->engine_ : nullptr), kind_(env_kind::standard) {
            bump_env_epoch();
        }

        // Method environment constructor (with 'this' object)
        environment(std::shared_ptr<environment> parent, string_symbolizer* symbolizer, script_value this_obj)
            : parent_(parent), symbolizer_(symbolizer), engine_(parent ? parent->engine_ : nullptr), kind_(env_kind::method),
              method_(std::make_unique<method_fields>()) {
            method_->this_object = std::move(this_obj);
            bump_env_epoch();
        }

        // Static method environment constructor (with class definition)
        environment(std::shared_ptr<environment> parent, string_symbolizer* symbolizer, std::shared_ptr<class_definition> class_def)
            : parent_(parent), symbolizer_(symbolizer), engine_(parent ? parent->engine_ : nullptr), kind_(env_kind::static_method),
              method_(std::make_unique<method_fields>()),
              access_context_(class_def.get()) {
            method_->class_def = class_def;
            bump_env_epoch();
        }

        // the engine whose allowance this scope's growth counts toward; a child scope takes its parent's
        void bind_engine(engine* a_engine) noexcept { engine_ = a_engine; }
        engine* bound_engine() const noexcept { return engine_; }

        // reverse declaration order (C++), as clear_values; past the engine's release depth the values die on its release machine
        ~environment();

        // Reference tracking for proper reference parameter support
        struct ref_info {
            uint64_t target_id;  // Symbol ID of the target variable
            std::shared_ptr<environment> target_env;  // environment containing the target
        };

        // Define a variable in the current scope
        void define(const std::string& name, const script_value& value);
        void define(const std::string& name, script_value&& value);
        void define(uint64_t id, const script_value& value);
        void define(uint64_t id, script_value&& value);
        // A variable declaration at program scope: another unit redeclaring the name gets its own
        // cell and the old one stays the binding of the units that declared or saw it; the same
        // unit, a child environment, a function or a class definition write in place (define). False, defining
        // nothing: inline declarations of other units created the name's program-scope cell (detail::inline_redefinition);
        // the running unit's own inline cell turns plain in place
        bool define_variable(uint64_t id, const script_value& value);
        bool define_variable(uint64_t id, script_value&& value);
        // The program-scope cell the running unit already declared for `id` (a re-run of the same unit), else null; the
        // unit's own inline cell turns plain (a declaration switched from inline)
        script_value* same_unit_cell(uint64_t id);
        // Inline and plain declarations of one name never mix across units. An inline declaration
        // in this program or namespace scope: create (nothing there), adopt (an inline cell, a host global, the running
        // unit's own plain cell - which turns inline), or conflict (a plain declaration of `owner` defined it)
        enum class inline_verdict : uint8_t { create, adopt, conflict };
        inline_verdict inline_declare(uint64_t id, uint32_t& owner);
        void share(uint64_t id);   // the cell an inline declaration just created (program scope: owned by no unit)
        // A plain declaration in a namespace scope: false while inline declarations of other units own the name
        bool plain_declare(uint64_t id);
        // A plain extern names a global some unit or the host defines; a lookup that finds nothing says so
        void note_extern(uint64_t id, bool function);
        std::string_view undefined_text(uint64_t id, std::string_view otherwise = "Undefined variable '{0}'") const;
        std::pair<std::string_view, uint64_t> unknown_name_error(std::string_view name) const;

        // Get a variable value (searches parent scopes, then this/static fields based on kind)
        // Returns checked_result to avoid exceptions in regular control flow
        checked_result<script_value> get(const std::string& name) const;
        checked_result<script_value> get(uint64_t id) const;
        // A program's name looked up by its id in the program's symbolizer (a parallel worker's environments intern into
        // a private one)
        checked_result<script_value> get(const std::string& name, const string_symbolizer& program_symbols) const;

        // Get a reference to variable value (for avoiding copies)
        // Returns checked_result to avoid exceptions in regular control flow
        checked_result<std::reference_wrapper<const script_value>> get_ref(const std::string& name) const;
        checked_result<std::reference_wrapper<const script_value>> get_ref(uint64_t id) const;
        checked_result<std::reference_wrapper<script_value>> get_ref(const std::string& name);
        checked_result<std::reference_wrapper<script_value>> get_ref(uint64_t id);

        // Assign to an existing variable (searches parent scopes, then this/static fields based on kind)
        // Returns checked_result to avoid exceptions in regular control flow
        [[nodiscard]] checked_result<void> assign(const std::string& name, const script_value& value);
        [[nodiscard]] checked_result<void> assign(const std::string& name, script_value&& value);
        [[nodiscard]] checked_result<void> assign(uint64_t id, const script_value& value);
        [[nodiscard]] checked_result<void> assign(uint64_t id, script_value&& value);

        // Check if variable exists in current or parent scopes
        bool contains(const std::string& name) const;
        bool contains(uint64_t id) const;

        // Get a pointer to a value (for references)
        // Checks local storage, parent chain, and this/static fields based on kind
        script_value* get_value_ptr(uint64_t id);

        // get_value_ptr WITHOUT this env's kind fallbacks (this/static fields):
        // non-null exactly when contains(id) — the store path's single-walk resolve
        script_value* get_env_var_ptr(uint64_t id);

        // Get all variables in this scope (not including parent scopes)
        // Returns a map with string_view keys pointing into the symbolizer (stable until engine destruction)
        std::unordered_map<std::string_view, script_value> get_local_variables() const;

        // Get all variables including parent scopes
        std::unordered_map<std::string_view, script_value> get_all_variables() const;

        // The program-scope variables `unit` declared (its plain cells, current or shadowed by another unit's), by name id
        std::vector<std::pair<uint64_t, const script_value*>> unit_variables(uint32_t unit) const;

        // Reset environment for reuse (optimization helper)
        void reset(std::shared_ptr<environment> new_parent);

        // Reset for method environment reuse (sets kind to method)
        void reset_as_method(std::shared_ptr<environment> parent, script_value this_obj);

        // Reset for static method environment reuse (sets kind to static_method)
        void reset_as_static_method(std::shared_ptr<environment> parent, std::shared_ptr<class_definition> class_def);

        bool is_method_env() const { return kind_ == env_kind::method; }
        bool is_static_method_env() const { return kind_ == env_kind::static_method; }

        // Method environment accessors (only valid when kind == method)
        const script_value& get_this_object() const { return method_ ? method_->this_object : null_this(); }

        // Clear just the this_object without clearing local values
        // Called when a method/constructor returns to ensure timely destruction
        void clear_this_reference() {
            if (!method_) { return; }
            method_->this_object = script_value::make_null(static_cast<engine*>(nullptr));
            method_->bound_method = script_value::make_null(static_cast<engine*>(nullptr));
        }

        // Sticky method-scope rebind (vm fast path): swap the receiver on a persistent
        // per-dispatch method env between calls. Deliberately NO epoch bump - 'this' is
        // never memoized by the vm lookup caches (vm_storage_lookup marks it uncacheable)
        // and nothing name-resolvable changed.
        void rebind_method_this(script_value this_obj) { method_box().this_object = std::move(this_obj); }

        // vm ip-cache pin: identity+epoch of this env are stable across calls (persistent
        // method scope parented on the global env), so env_lookup_cached may memoize
        // against it exactly like the global env itself.
        bool vm_pinned_scope() const noexcept { return vm_pinned_scope_; }
        void set_vm_pinned_scope(bool pinned) noexcept { vm_pinned_scope_ = pinned; }

        // vm ip-cache transparency: a standard scope env that has DEFINED nothing of its
        // own (local_ids_ — the flat memo map doesn't count) cannot shadow any name, so
        // lookups provably continue past it. Callers re-prove this per access: a
        // mid-body define flips it and the site falls back to the full walk.
        bool vm_transparent_for_lookup() const noexcept {
            return kind_ == env_kind::standard && local_ids_.empty();
        }
        environment* parent_raw() const noexcept { return parent_.get(); }
        // a lookup's step up the parent chain; Debug builds count it (string_symbolizer::env_hops, a deterministic cost measure)
        environment* chain_step(environment* p) const noexcept {
#ifndef NDEBUG
            if (p) { symbolizer_->note_env_hop(); }
#endif
            return p;
        }
        bool is_program_scope() const noexcept { return !parent_ || namespace_scope_; }
        void mark_namespace_scope() noexcept { namespace_scope_ = true; }

        // A { } block's (or a loop's, a branch's) scope: a free function called from inside it sees the scope it was defined
        // in, never the block's declarations (C++ scoping; a function defined inside the block keeps its caller's chain)
        bool block_scope() const noexcept { return block_scope_; }
        void set_block_scope(bool block) noexcept { block_scope_ = block; }
        // The environment a free function's call is parented on: the caller's, past the block scopes it stands in when
        // the function was declared outside every block
        static std::shared_ptr<environment> call_parent(std::shared_ptr<environment> caller, bool block_defined) {
            if (!block_defined) {
                while (caller && caller->block_scope_ && caller->parent_) { caller = caller->parent_; }
            }
            return caller;
        }
        // A free function's call scope (vm): one that defined nothing is invisible to lookups while the activations above it
        // run, so a call made under it is parented past it and a recursion's chain stays as deep as the source's nesting
        void set_call_scope() noexcept { call_scope_ = true; }
        static std::shared_ptr<environment> past_idle_call_scopes(std::shared_ptr<environment> env, bool block_defined) {
            while (env && env->call_scope_ && env->parent_ && env->local_ids_.empty() && !env->access_context_) {
                env = env->parent_;
                while (!block_defined && env->block_scope_ && env->parent_) { env = env->parent_; }
            }
            return env;
        }

        // Static method environment accessors (only valid when kind == static_method)
        std::shared_ptr<class_definition> get_class_definition() const { return method_ ? method_->class_def : nullptr; }

        // Member-shadowing ladder (detail/member_scope.hpp): stable storage of a method
        // env's receiver, so closure frames can probe the creation-site 'this' without
        // copying it per identifier. Null for non-method envs.
        script_value* method_this_storage() { return kind_ == env_kind::method ? &method_box().this_object : nullptr; }

        // OWN-storage probe for the member-shadowing ladder's function-scope phase:
        // names DEFINED in this env only (local_ids_-gated) - never ancestor cache
        // entries (their provenance spans the whole chain), never kind fallbacks.
        script_value* get_own_value_ptr(uint64_t id) {
            if (!local_ids_.contains(id)) return nullptr;
            if (symbolizer_->shadow_count() != 0) [[unlikely]] {
                if (script_value* cell = shadowed_cell(id)) return cell;
            }
            auto* it = flat_lookup_.find(id);
            return it ? *it : nullptr;
        }

        // Generation SERIAL for the vm's env-lookup inline caches (chunk.hpp
        // env_lookup_cache_entry): re-drawn from the per-engine monotonic counter on
        // construction, reset (pool reuse), clear, parent change, and define - the
        // events that can invalidate a cached pointer RESOLVED AGAINST THIS ENV.
        // Per-env (not the old global bump) on purpose: a method scope resetting
        // ~900x/tick must not invalidate entries cached against the global env
        // (measured cache poisoning; see the b377b5ac scope-elision commit). A serial
        // rather than a per-env counter because the allocator recycles env addresses:
        // {address, count} can repeat across lifetimes, {address, serial} cannot.
        // Cached pointers target storage in the entry's env or an ANCESTOR (a parent
        // can't be pool-recycled while its child chain is live; the global env is
        // engine-lifetime with in-place redefinition into stable storage).
        uint64_t local_epoch() const noexcept { return local_epoch_; }

        // Access-enforcement context: the class whose method/ctor body this scope env
        // executes (stamped by the backend at method-scope creation; nullptr elsewhere).
        // Lambdas inherit through the parent chain — the walk only runs on the
        // enforcement slow path (target class has nonpublic members). Raw pointer:
        // class_definitions are engine-lifetime and hot reload mutates them in place.
        void set_access_context(class_definition* cls) { access_context_ = cls; }
        class_definition* get_access_context() const { return access_context_; }
        const class_definition* find_access_context() const {
            for (const environment* env = this; env; env = env->parent_.get()) {
                if (env->access_context_) return env->access_context_;
            }
            return nullptr;
        }

        // Clear all values but keep parent chain intact (for proper scope cleanup)
        void clear_values();

        // Clear cached parent pointers (keeps local variable pointers which are stable)
        // Call on hot reload to invalidate stale pointers to object fields
        void clear_parent_cache();

        // Clear parent caches in this environment and all parents (for hot reload)
        void clear_all_parent_caches() {
            clear_parent_cache();
            if (parent_) {
                parent_->clear_all_parent_caches();
            }
        }

        // Engine teardown: every variable that may own an instance (an object, an array, a map) dies, the last defined first
        // (C++ statics at exit); functions stay callable for the destructors that run
        void release_values_in_reverse() {
            for (size_t i = local_storage_.size(); i-- > 0;) {
                script_value& cell = local_storage_[i];
                if (dies_at_teardown(cell)) { script_value dying = std::move(cell); }
            }
        }
        // A program-scope value that may own an instance: an object, an array, a map, a lambda (its captures), a property
        // reference (its receiver's handle), or a reference naming one of those; a declared function stays for the destructors
        // that call it
        static bool dies_at_teardown(const script_value& cell);
        script_value* own_cell(size_t slot) noexcept { return slot < local_storage_.size() ? &local_storage_[slot] : nullptr; }

        // Get parent environment (needed for method_environment handling)
        std::shared_ptr<environment> get_parent() const { return parent_; }

        // Set parent environment (use this instead of directly setting parent_)
        void set_parent(std::shared_ptr<environment> new_parent) {
            parent_ = new_parent;
            if (parent_) { engine_ = parent_->engine_; }
            bump_env_epoch();
        }

        // The pure env-storage prefix of get_ref/get_value_ptr (this special-case, flat
        // caches, parent-chain walk) with provenance: cacheable is true only when the
        // pointer is a found-env LOCAL (stable storage), so the vm's per-instruction
        // lookup caches never memoize this/static-field fallbacks. nullptr = the prefix is
        // exhausted; the caller runs its original full lookup for the fallback tails.
        script_value* vm_storage_lookup(uint64_t id, bool& cacheable);
        const detail::env_value_store& own_values() const noexcept { return local_storage_; }

    protected:
        // Invalidate the vm's env-lookup inline caches (defined in environment.cpp:
        // symbolizer is forward-declared here)
        void bump_env_epoch() noexcept;

    private:
        static environment* sole_closure_env(const script_value& a_value) noexcept;

        // === Lazy-Cached Flat Lookup ===
        // THIS scope's values, each at a fixed address for the env's life
        detail::env_value_store local_storage_;

        // Lazy lookup cache - starts empty, populated on first access to each variable
        // Key: symbol ID, Value: pointer to script_value in some environment's local_storage_
        // mutable because const lookup methods cache for performance (doesn't change logical state)
        mutable detail::env_id_map<script_value*> flat_lookup_;

        // Track which IDs we defined locally (for O(1) "is local" checks)
        detail::env_id_set<> local_ids_;
        // Root only: the earlier cells of a name redeclared across units, each with the unit that
        // declared it, and the unit of the cell in flat_lookup_ (absent = 0: host, function, class)
        struct binding_version { unit_stamp unit; script_value* cell; };
        // the program scope's unit bookkeeping, made on its first use (no other environment keeps any)
        struct root_tables {
            std::unordered_map<uint64_t, std::vector<binding_version>> versions;
            std::unordered_map<uint64_t, unit_stamp> unit_of;
            std::unordered_map<uint64_t, std::vector<uint32_t>> inline_cells;   // an inline cell's declaring units
            std::unordered_map<uint64_t, bool> extern_names;   // plain externs (true = function)
        };
        std::unique_ptr<root_tables> root_;
        root_tables& root_tables_mut();
        script_value* shadowed_cell(uint64_t id) const;
        script_value* uncached_value_ptr(uint64_t id);
        script_value* kind_fallback_ptr(uint64_t id);

        // Parent pointer kept for: closure semantics, 'this' lookup, debugging
        std::shared_ptr<environment> parent_;
        string_symbolizer* symbolizer_;
        engine* engine_ = nullptr;   // a root's is bound by its engine (or backend); a child takes its parent's

        // === Unified environment fields (replaces method_environment / static_method_environment) ===
        env_kind kind_;
        bool namespace_scope_ = false;

        // What only method and static-method scopes hold ('this', the class, the bound-method slot), boxed so other scopes pay
        // one pointer; a pooled env keeps its box across resets
        struct method_fields {
            script_value this_object{std::monostate{}, static_cast<engine*>(nullptr)};
            std::shared_ptr<class_definition> class_def;
            script_value bound_method{std::monostate{}, static_cast<engine*>(nullptr)};
        };
        std::unique_ptr<method_fields> method_;
        method_fields& method_box() {
            if (!method_) { method_ = std::make_unique<method_fields>(); }
            return *method_;
        }
        static const script_value& null_this() {
            static const script_value none{std::monostate{}, static_cast<engine*>(nullptr)};
            return none;
        }


        // Access-enforcement context (see set_access_context above). NOT owning: the
        // pooled-env reset paths null it so a recycled env never leaks a stale context.
        class_definition* access_context_ = nullptr;
        uint64_t local_epoch_ = 0;   // generation serial; 0 = never cache (see local_epoch())
        bool vm_pinned_scope_ = false;   // see vm_pinned_scope(); reset paths clear it
        bool block_scope_ = false;       // see block_scope(); reset paths clear it
        bool call_scope_ = false;        // see set_call_scope(); reset paths clear it


        // Internal helpers for kind-specific lookups
        // These check the receiver's fields (method kind) or the class's static fields (static_method kind)
        script_value* get_static_field_ptr(uint64_t id);
    };

    // Create a bound method - binds 'this' as the first argument
    script_value make_bound_method(const script_value& this_obj, const script_value& method);

    // make_bound_method's callable, named so a call site can see the method it forwards to
    struct bound_method_fn {
        script_value this_obj;
        script_value method;
        checked_result<script_value> operator()(const std::vector<script_value>& args) const { return call(args, true); }
        checked_result<script_value> call(const std::vector<script_value>& args, bool host_call) const;   // script_callable_thunk::call's
    };

    namespace detail {
        // A bound method's non-owning reference into its receiver (the chaining idiom's `return *this`) keeps the receiver's
        // object alive, so a temporary receiver outlives the call
        void anchor_bound_method_result(const script_value& this_obj, checked_result<script_value>& result);
    }

    namespace detail {
        // A host object a T& result lends script (no owner, only the borrowed address): an auto or T declaration from it
        // copies, as C++ auto and T declarations do
        inline bool borrows_host_object(const script_value& value) {
            if (value.raw_storage_index() != script_value::TYPEID_OBJECT) { return false; }
            const auto* holder = value.peek_object_holder();
            return holder && holder->bound_object() && !holder->holds_object();
        }
    }

    // The ONE implicit-copy kernel for assignment-shaped store boundaries (decl from
    // lvalue, identifier/field/element assign, range-for bindings, destructuring,
    // by-value parameter binding), shared by both backends: plain values deep-copy
    // (script value semantics), shared_ptr<T>-marked values copy the HANDLE (guide
    // ch03: "assignment shares instead of deep-copying" - clone() stays the explicit
    // deep copy). A store site calling clone() directly is the GLOOM bug class:
    // `var e = arr[i]; e.hurt()` silently mutated a deep copy.
    inline script_value clone_for_assignment(const script_value& value) {
        // Element/map-entry/field reads arrive as reference wrappers - deref() reads
        // through them (and returns *this for plain values), so the shared_ptr marker
        // check sees the referent, not 'reference'. Copy-only consumption: safe to
        // use the bare-deref idiom (no move of the deref result back into value).
        const script_value& target = value.deref();
        // clone() of a plain scalar/string IS `storage_ = storage_` (strings share
        // storage by design) - return the copy inline instead of the out-of-line
        // clone() call every store-shaped boundary otherwise pays. These raw indexes
        // can't be borrow/bound/shared-marked, so no clone() special case is skipped.
        switch (target.raw_storage_index()) {
            case script_value::TYPEID_NULL:
            case script_value::TYPEID_INT:
            case script_value::TYPEID_FLOAT:
            case script_value::TYPEID_STRING:
            case script_value::TYPEID_CHAR:
            case script_value::TYPEID_BOOL:
                return target;
            default:
                break;
        }
        auto type_info = target.get_type_info();
        if (type_info && type_info->base_type == script_value_type::jai_shared_ptr_type) {
            return target;  // Share the handle, don't clone
        }
        return target.clone();
    }

    // Lvalue-shaped initializer / assignment source (identifier, member, subscript, and a
    // conditional or ?? with such an operand, which may yield it): an existing object the
    // store must copy. Everything else (call, construction, literal, operator result) is a
    // temporary the store may move. Shared by both backends.
    inline bool is_lvalue_shaped_expr(const expression* e) noexcept {
        if (!e) return false;
        const node_type t = e->get_type();
        if (t == node_type::identifier_expr || t == node_type::member_expr) return true;
        if (t == node_type::ternary_expr) {
            const auto* n = static_cast<const ternary_expr*>(e);
            return is_lvalue_shaped_expr(n->then_expression.get()) || is_lvalue_shaped_expr(n->else_expression.get());
        }
        if (t != node_type::binary_expr) return false;
        const auto* b = static_cast<const binary_expr*>(e);
        if (b->op.type == token_type::question_question) {
            return is_lvalue_shaped_expr(b->left.get()) || is_lvalue_shaped_expr(b->right.get());
        }
        return b->op.type == token_type::left_bracket;
    }

    // The decl copy rule for an evaluated initializer (`T x = e`, `T f = e` in a class body,
    // `static T s = e`): a reference-wrapped read (subscript, ternary over elements) or an
    // lvalue-shaped initializer copies through clone_for_assignment; a temporary moves.
    inline script_value initializer_store_value(script_value&& evaluated, const expression* initializer) {
        if (evaluated.is_reference() || is_lvalue_shaped_expr(initializer)) {
            return clone_for_assignment(evaluated);
        }
        return std::move(evaluated);
    }

    struct script_defined_function;

    /// Stack-based call frame for fast function parameter access - avoids hash map
    /// overhead for the most frequently accessed variables.
    /// Defined at namespace scope so coroutine_handle can reference it without circular includes.
    // A frame's slots die last slot first: locals in reverse declaration order (C++)
struct reverse_destroyed_values : std::vector<script_value> {
    using std::vector<script_value>::vector;
    reverse_destroyed_values() = default;
    reverse_destroyed_values(const reverse_destroyed_values&) = default;
    reverse_destroyed_values(reverse_destroyed_values&&) noexcept = default;
    reverse_destroyed_values& operator=(const reverse_destroyed_values&) = default;
    reverse_destroyed_values& operator=(reverse_destroyed_values&&) noexcept = default;
    ~reverse_destroyed_values() { while (!empty()) { pop_back(); } }
};

namespace detail {
    // The cells a dying frame shared with its lambdas, checked for cycles once the frame's locals have died
    struct shared_cell_release {
        engine* eng;
        std::vector<script_value> cells;
        explicit shared_cell_release(engine* e) noexcept : eng(e) {}
        shared_cell_release(const shared_cell_release&) = delete;
        shared_cell_release& operator=(const shared_cell_release&) = delete;
        ~shared_cell_release() { if (!cells.empty()) { script_value::release_cell_cycles(cells, eng); } }
    };
}

struct call_frame {
        std::shared_ptr<environment> closure_env;

        /// We store a pointer to avoid script_value default construction issues
        std::unique_ptr<script_value> this_object_ptr;
        bool is_method = false;

        std::shared_ptr<class_definition> static_class_def;
        bool is_static_method = false;

        // Member-shadowing scope (detail/member_scope.hpp): C++ unqualified-lookup
        // order makes class members resolve between the function's own scopes and the
        // definition/global environments. scope_floor = the first env in the chain
        // OUTSIDE the function scope (the member probe runs when a lookup crosses it);
        // scope_this/scope_static = what to probe. The interpreter stamps these at
        // frame push (the call branch knows the callee kind); the vm computes them
        // lazily from entry_env/locals on first use. 0=uncomputed, 1=none,
        // 2=instance, 3=static (member_scope_state).
        uint8_t scope_state = 0;
        environment* scope_floor = nullptr;
        script_value* scope_this = nullptr;
        class_definition* scope_static = nullptr;

        void reset_member_scope() noexcept {
            scope_state = 0;
            scope_floor = nullptr;
            scope_this = nullptr;
            scope_static = nullptr;
        }

        std::string_view function_name;
        const ast_node* current_node = nullptr;

        // The detail::script_defined_function this frame executes: the return site reads
        // its declared return type (the typed-return conversion runs at the `return`) and
        // a debugger names the slot-based locals below from it. Null for frames not pushed
        // by call_function. One pointer store per call — no measurable cost.
        const script_defined_function* function = nullptr;

        /// O(1) access by slot index - much faster than hash map or linear search
        reverse_destroyed_values locals;
        // A slot was boxed into a cell on demand (a reference bound to it): the frame's exit checks its cells for cycles
        bool boxed_cells = false;

        /// Invalid slot constant - SIZE_MAX naturally fails bounds check
        static constexpr size_t INVALID_SLOT = SIZE_MAX;

        script_value* get_local(size_t slot) noexcept {
            if (slot < locals.size()) {
                return &locals[slot];
            }
            return nullptr;
        }

        const script_value* get_local(size_t slot) const noexcept {
            if (slot < locals.size()) {
                return &locals[slot];
            }
            return nullptr;
        }

        void set_local(size_t slot, script_value value) {
            if (slot == locals.size()) {
                locals.push_back(std::move(value));
            } else if (slot < locals.size()) {
                locals[slot] = std::move(value);
            } else {
                // slot > size: a lower-numbered slot belongs to a declaration in a
                // block that was not executed (e.g. an untaken if-branch). The parser
                // assigns slots monotonically across the whole function body, so such
                // gaps are legal. Grow the locals vector with null placeholders so the
                // target slot exists, then store the value. Placeholders carry the same
                // engine context as the value being stored.
                locals.reserve(slot + 1);
                while (locals.size() < slot) {
                    locals.emplace_back(std::monostate{}, nullptr);
                }
                locals.push_back(std::move(value));
            }
        }

        void reserve_locals(size_t count) {
            if (count > 0) {
                locals.reserve(count);
            }
        }

        /// Number of live local slots in this frame (used by closure capture to
        /// identify slots that belong to this frame vs. an inner lambda's scope).
        size_t local_count() const noexcept { return locals.size(); }

        void set_this(script_value this_obj) {
            // Reused frames keep the allocation (the vm's pooled records null the VALUE on
            // pop, not the pointer) - one heap round trip per method call saved
            if (this_object_ptr) {
                *this_object_ptr = std::move(this_obj);
            } else {
                this_object_ptr = std::make_unique<script_value>(std::move(this_obj));
            }
            is_method = true;
        }

        /// Get 'this' object (only valid if is_method is true)
        script_value& get_this() {
            return *this_object_ptr;
        }

        const script_value& get_this() const {
            return *this_object_ptr;
        }

    };

    namespace detail {
        /// Call-site context handed from the caller's invoke to the callee's parameter
        /// binding (a single save/restored pointer, consumed once): the argument
        /// expressions live in the caller's AST and outlive the call; the caller's
        /// environment is the callee's previousEnv at bind time.
        struct call_site_context {
            const std::vector<expression_ptr>* arg_exprs = nullptr;
            const std::vector<script_value>* values = nullptr;   // the arguments as passed (a host callable's T& resolution)
        };
    }

    // Script-defined function storage. Parameters live behind a shared immutable vector:
    // every function value minted from the same AST node shares ONE storage (the vm's
    // compile-time protos always did this implicitly; the interpreter's per-execution
    // lambda/method/namespace mints used to deep-copy the vector each time). parameter's
    // mutable symbol_id/slot_index caches are warmed single-threaded (parse or the
    // parallel admission walk), so sharing is read-safe; parallel workers still take
    // private copies (provision_worker) so their lazy fills never cross threads.
    struct script_defined_function {
        std::string_view name;  // Points to symbolizer storage (permanent)
        std::shared_ptr<const std::vector<parameter>> shared_parameters;
        type_info_ptr return_type;
        std::shared_ptr<block_stmt> body;
        std::shared_ptr<environment> closure_env;  // capture environment for closures
        size_t local_count = 0;  // Total slots needed (params + locals) for stack allocation

        // Backend-owned compiled artifact for the body (the vm caches its chunk here so a
        // call skips the body-keyed cache lookup; the interpreter ignores it). mutable:
        // filled through const refs on the call path; per-engine, single backend.
        mutable std::shared_ptr<void> backend_body_cache;

        // Cached return-type classification (detail::return_conv, filled through
        // detail::return_conv_of on both backends; 0 = not yet classified). Derived purely
        // from return_type on first call; mutable like backend_body_cache (workers hold
        // private function copies, so lazy fills never cross threads).
        mutable uint8_t backend_return_conv = 0;
        mutable uint8_t backend_lean_bind = 0;   // 1: every parameter a by-value primitive or string slot in order (a matching argument is its own slot), 2: not
        mutable uint8_t backend_frame_plan = 0;   // the vm's frame shape: 1 general, 2 plain over a lean bind, 3 plain with T& parameters (no frame env; a closure not a method's)

        // Precomputed so ref-free call sites can skip the per-call arg-metadata build
        bool has_reference_parameters = false;

        // Read-only by-value parameter elision (parameter aliasing):
        // bit i set = the body provably never mutates, aliases, compares or otherwise uses
        // parameter i whole, and is itself pure with respect to enclosing state, so binding
        // the argument by handle instead of cloning it is unobservable. Proven lazily on the
        // first call (main thread only) against engine::definition_epoch(); only plain
        // functions are candidates (methods and coroutines bind through other paths).
        bool alias_candidate = false;
        mutable uint64_t alias_proof_epoch = 0;
        mutable uint32_t alias_proof_mask = 0;
        unit_stamp unit{};   // the unit and the execution that declared it
        bool block_defined = false;   // declared inside a block scope (an include in a { }): a call keeps the caller's chain
        bool declared = false;   // a function declaration's (never a lambda's): another signature of its name joins it in an overload set
        size_t line = 0;   // the declaration's line (an overload's diagnostics name it)
        uint64_t context = 0;   // the host entry that declared it (engine::host_entry_generation)
        std::shared_ptr<const function_decl> decl;   // the vm's mint pins its declaration, so the inliner's stamps on it resolve while the function lives (the interpreter's decl already owns its mint)
        // A converted closure: closure_env is null, backend_body_cache is the chunk compiled
        // with a slot per captured name, and captures holds each name's cell in capture_ids' order (body stays the lambda's own)
        struct converted_captures {
            std::vector<script_value> cells;
            std::shared_ptr<const std::vector<uint64_t>> ids;
            std::shared_ptr<script_defined_function> environment_twin;
        };
        std::unique_ptr<converted_captures> captures;

        const std::vector<parameter>& parameters() const noexcept { return *shared_parameters; }
        bool converted() const noexcept { return captures != nullptr; }
        // fn as code that runs its body by name sees it: a converted closure answers its twin over one environment holding its
        // cells (made once; the parallel builtins and admission run bodies that way), any other function itself
        static std::shared_ptr<script_defined_function> environment_form(const std::shared_ptr<script_defined_function>& fn, engine* eng);
        ~script_defined_function();

        // slots is the body's frame size (its declaration's local_count): the vm places its temporaries above it
        script_defined_function(std::string_view n, std::vector<parameter> params,
                            type_info_ptr retType, std::shared_ptr<block_stmt> b,
                            std::shared_ptr<environment> env, size_t slots)
            : script_defined_function(n, std::make_shared<const std::vector<parameter>>(std::move(params)),
                                      retType, std::move(b), std::move(env), slots) {}

        // Sharing constructor: node-cached parameter storage, zero per-mint copies
        script_defined_function(std::string_view n, std::shared_ptr<const std::vector<parameter>> params,
                            type_info_ptr retType, std::shared_ptr<block_stmt> b,
                            std::shared_ptr<environment> env, size_t slots)
            : name(n), shared_parameters(std::move(params)), return_type(retType), body(std::move(b)), closure_env(std::move(env)), local_count(slots) {
            for (const auto& p : *shared_parameters) {
                if (p.is_reference) { has_reference_parameters = true; break; }
            }
        }
    };

} // namespace jai

#endif // __JAISCRIPT_DETAIL_ENVIRONMENT_HPP__
