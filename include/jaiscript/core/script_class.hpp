#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <iostream>
#include <climits>

// Include class_definition (extracted from dynamic_binder.hpp for faster compilation)
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/core/overload_resolution.hpp>

// Needed for the inline thunk implementations at the end (execute_callable seam)
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/detail/ast.hpp>

namespace jai {

namespace detail {
    // Every method dispatches dynamically, so redefining a base class method says so with 'override' (the run time's text;
    // the checker reports it at load with a lowercase first letter)
    inline std::string missing_override_text(std::string_view name, std::string_view owner) {
        return "Method '" + std::string(name) + "' redefines '" + std::string(owner) + "::" + std::string(name) + "': mark it 'override'";
    }
    // A constructor's initializer list against the class's bases (C++ [class.base.init]); both backends and the strict checker
    inline constexpr std::string_view k_base_needs_naming = "Class '{0}' must construct base '{1}': it has no default constructor";
    inline constexpr std::string_view k_not_a_base = "Class '{0}' has no base '{1}' to construct";
    inline constexpr std::string_view k_base_named_twice = "Class '{0}' constructs base '{1}' twice";
    // An initializer naming a base: its class name, or the name after the namespace a base declared in one carries
    inline bool initializer_names(std::string_view target, std::string_view base_name) {
        if (target == base_name) { return true; }
        return base_name.size() > target.size() + 2 && base_name.substr(base_name.size() - target.size()) == target &&
               base_name.substr(base_name.size() - target.size() - 2, 2) == "::";
    }
    inline std::string diamond_inheritance_text(std::string_view cls, std::string_view shared, std::string_view first, std::string_view second) {
        return "Class '" + std::string(cls) + "' inherits '" + std::string(shared) + "' through both '" + std::string(first) + "' and '" +
               std::string(second) + "' (diamond inheritance is not supported)";
    }
    inline std::string override_without_base_text(std::string_view name) {
        return "Method '" + std::string(name) + "' marked as override but no base class method found";
    }
}

// Forward declarations - class_definition is available since we're included after it's defined
class function_decl;

// Script class definition - now inherits from class_definition!
class script_class_definition : public class_definition {
public:
    // Constructor - takes pre-interned type_id from parser or runtime
    script_class_definition(std::string_view name, uint64_t type_id, engine* eng)
        : class_definition(name, type_id, class_type::script_class, eng) {
        declare_implicit_default_constructor();
    }

    void add_method_from_ast(std::string_view name,
                            std::shared_ptr<function_decl> ast,
                            std::shared_ptr<environment> definition_env,
                            bool is_hot_reload = false) {
        auto eng = get_engine();
        if (!eng) return;
        uint64_t name_id = detail::engine_access::symbolize(*eng, name);
        ast->unit = detail::engine_access::current_unit(*eng);

        // Constructors and destructors don't need override validation
        bool is_constructor = (name == get_name());
        bool is_destructor = (name.size() > 1 && name[0] == '~');   // "~" alone is operator~

        // Find if this method exists in parent classes
        auto method_info = find_method(name);
        
        // Validate override usage (skip for constructors/destructors)
        if (!is_constructor && !is_destructor && ast->is_override) {
            if (!method_info.owner_class || method_info.owner_class.get() == this) {
                throw runtime_error(detail::override_without_base_text(name));
            }
            
            // Promote the entire hierarchy to virtual
            // Walk up from the method owner to promote all instances
            auto current = method_info.owner_class;
            while (current) {
                auto* metadata = current->get_method_metadata_mutable(name);
                if (metadata) {
                    if (metadata->is_virtual) {
                        // Already virtual, we're done
                        break;
                    }
                    metadata->is_virtual = true;
                }
                // Check parent for the same method
                current = current->get_parent();
                if (current && !current->has_method(name_id)) {
                    break; // Method doesn't exist higher up
                }
            }
        } else if (!is_constructor && !is_destructor && !is_hot_reload) {
            // Not marked as override (skip check for constructors/destructors and hot reload);
            // a base's SYNTHESIZED field accessor is invisible to shadowing
            if (method_info.owner_class && method_info.owner_class.get() != this &&
                !method_info.owner_class->owns_auto_accessor(name_id)) {
                throw runtime_error(detail::missing_override_text(name, method_info.owner_class->get_name()));
            }
        }
        
        method_asts_[name_id] = ast;
        add_script_method(name, ast, definition_env);
        method_metadata metadata;
        metadata.is_virtual = ast->is_override || is_destructor;  // Override methods and destructors are virtual
        
        set_method_metadata(name, metadata);
    }
    
    // Add constructor from AST
    void add_constructor_from_ast(std::shared_ptr<function_decl> ctor_ast) {
        // Store for later reference
        if (auto eng = get_engine()) { ctor_ast->unit = detail::engine_access::current_unit(*eng); }
        for (const auto& earlier : constructor_asts_) {
            if (detail::differs_only_by_reference(ctor_ast->parameters, earlier->parameters)) [[unlikely]] {
                throw runtime_error(detail::reference_only_overload_text(get_name(), ctor_ast->parameters, earlier->parameters, earlier->location.line));
            }
        }
        constructor_asts_.push_back(ctor_ast);

        // The constructor is already added to the environment by visit_class_decl
        // We just need to store it for potential VM compilation
    }

    // Add destructor from AST
    void add_destructor_from_ast(std::shared_ptr<function_decl> dtor_ast,
                                std::shared_ptr<environment> definition_env) {
        destructor_ast_ = dtor_ast;

        // Add as a special method (will be marked virtual automatically)
        add_method_from_ast("~" + get_name(), dtor_ast, definition_env);
    }
    
    // Get ASTs for VM compilation
    const std::map<uint64_t, std::shared_ptr<function_decl>>& get_method_asts() const {
        return method_asts_;
    }
    
    const std::vector<std::shared_ptr<function_decl>>& get_constructor_asts() const {
        return constructor_asts_;
    }

    // The class declares the constructor now: one of its overloads or its implicit default (a reload replaces both)
    bool declares_constructor(const function_decl* ctor) const noexcept {
        if (ctor == implicit_default_ctor_.get()) { return true; }
        for (const auto& declared : constructor_asts_) {
            if (declared.get() == ctor) { return true; }
        }
        return false;
    }

    // The constructor overloads declared under private: (or protected:), set with the class's access labels
    void set_private_constructors(std::vector<const function_decl*> ctors) { private_ctors_ = std::move(ctors); }
    bool constructor_is_private(const function_decl* ctor) const noexcept {
        for (const function_decl* p : private_ctors_) {
            if (p == ctor) { return true; }
        }
        return false;
    }
    
    std::shared_ptr<function_decl> get_destructor_ast() const {
        return destructor_ast_;
    }

    // The scope of the namespace that declares the class, where its implicit construction reads names; null at program scope
    void set_namespace_scope(std::shared_ptr<environment> scope) { namespace_scope_ = std::move(scope); }
    const std::shared_ptr<environment>& namespace_scope() const noexcept { return namespace_scope_; }

    // C++ implicit construction: a base chain that runs code (a declared constructor or a C++ class above) must be
    // constructed by a class that declares no constructor of its own
    // Some base runs code when the class is constructed: a C++ base, or a script base with a constructor or such a base of its own
    bool base_needs_construction() const {
        for (const auto& parent : get_parent_classes()) {
            if (!parent) { continue; }
            if (!parent->is_script_class()) { return true; }
            const auto& script_parent = static_cast<const script_class_definition&>(*parent);
            if (!script_parent.get_constructor_asts().empty() || script_parent.base_needs_construction()) { return true; }
        }
        return false;
    }

    // A constructor's initializer list as it runs: one naming none over a base that needs construction is `: super()`
    const std::vector<constructor_initializer>& effective_initializers(const function_decl& ctor) const {
        if (ctor.initializers.empty() && base_needs_construction()) { return implicit_default_constructor()->initializers; }
        return ctor.initializers;
    }

    // The implicitly-declared default constructor: no parameters, an empty body, `: super()` over a base. Built with the
    // declaration and never by a construction, so the workers of a parallel region constructing at once only read it
    const std::shared_ptr<function_decl>& implicit_default_constructor() const { return implicit_default_ctor_; }

    // (Re)declares it from the class's name, unit and bases: at birth, when a reload clears the old one (a fresh identity, so
    // the old one stops being declared) and once the declaration has set the bases and unit
    void declare_implicit_default_constructor() {
        auto eng = get_engine();
        if (!eng) { return; }
        auto [name_id, name_view] = detail::engine_access::get_symbolizer(*eng)->intern_with_view(get_name());
        auto ctor = std::make_shared<function_decl>(source_location{}, name_view, name_id);
        ctor->body = std::make_shared<block_stmt>(source_location{}, std::vector<declaration_ptr>{});
        if (!get_parent_classes().empty()) { ctor->initializers.emplace_back("super", std::vector<expression_ptr>{}); }
        ctor->unit = unit();
        ctor->shared_params_cache = std::make_shared<const std::vector<parameter>>();
        implicit_default_ctor_ = std::move(ctor);
    }
    
    // A field initializer, run at construction in source declaration order: an initializer may read a sibling field
    void add_field_initializer_ast(uint64_t field_id, expression_ptr initializer) {
        for (auto& [id, ast] : field_initializer_asts_) {
            if (id == field_id) { ast = initializer; return; }
        }
        field_initializer_asts_.emplace_back(field_id, initializer);
    }

    // Get field initializer AST (for instance construction)
    expression_ptr get_field_initializer_ast(uint64_t field_id) const {
        for (const auto& [id, ast] : field_initializer_asts_) {
            if (id == field_id) return ast;
        }
        return nullptr;
    }

    // Get all field initializer ASTs (in declaration order)
    const std::vector<std::pair<uint64_t, expression_ptr>>& get_field_initializer_asts() const {
        return field_initializer_asts_;
    }

    // Construction fast path: every field initializer in the script chain is a scalar
    // literal (the shape evaluate_field_initializers stamps without evaluating), so a
    // construction with no initializer list never consults its init env. Memo under
    // method_epoch (a reload re-adds the ASTs after clear_asts and bumps it)
    // Instance-plan hooks (class_definition::instance_plan): the plan is valid only for a
    // literal-only chain, and each literal's value is what the stamp pass would store
    bool chain_literal_plan_ok() const override { return chain_initializers_all_literal(); }
    const script_value* literal_initializer_for(uint64_t id) const override {
        for (const auto& [fid, ast] : field_initializer_asts_) {
            if (fid == id) { return ast && ast->get_type() == node_type::literal_expr ? &static_cast<const literal_expr*>(ast.get())->value : nullptr; }
        }
        return nullptr;
    }
    // A null initializer the stamp passes may store untouched: the field's slot holds null (an int's refuses it, a bool's reads it false)
    bool null_initializer_stored_as_is(uint64_t field_id) const {
        const type_info_ptr declared = get_field_declared_type(field_id);
        if (!declared) { return true; }
        switch (declared->base_type) {
            case script_value_type::jai_any_type: case script_value_type::jai_shared_ptr_type:
            case script_value_type::jai_weak_ptr_type: case script_value_type::jai_function_type: return true;
            default: return false;
        }
    }
    bool chain_initializers_all_literal() const {
        if (literal_memo_epoch_ != method_epoch() || literal_memo_count_ != field_initializer_asts_.size()) {
            assert(!(get_engine() && detail::engine_access::parallel_region_active(*get_engine())) && "a parallel worker writes the class's literal-chain memo (prime it at region start)");
            bool all = true;
            for (const auto& parent : get_parent_classes()) {
                if (parent && parent->is_script_class() &&
                    !std::static_pointer_cast<script_class_definition>(parent)->chain_initializers_all_literal()) {
                    all = false;
                    break;
                }
            }
            for (size_t i = 0; all && i < field_initializer_asts_.size(); ++i) {
                const auto& ast = field_initializer_asts_[i].second;
                if (!ast) { continue; }
                if (ast->get_type() != node_type::literal_expr) { all = false; break; }
                const size_t li = static_cast<const literal_expr*>(ast.get())->value.raw_storage_index();
                all = li == script_value::TYPEID_INT || li == script_value::TYPEID_FLOAT ||
                      li == script_value::TYPEID_BOOL || li == script_value::TYPEID_CHAR ||
                      (li == script_value::TYPEID_NULL && null_initializer_stored_as_is(field_initializer_asts_[i].first));
            }
            literal_memo_ = all;
            literal_memo_epoch_ = method_epoch();
            literal_memo_count_ = field_initializer_asts_.size();
        }
        return literal_memo_;
    }

    // Clear all stored ASTs (for hot reload)
    void clear_asts() {
        // Both backend reload builders enter here before mutating the old class.
        // Recover surviving worker copies before a fallible retry can leave mixed ASTs.
        if (auto eng = get_engine()) { detail::engine_access::recover_parallel_registrations(*eng); }
        method_asts_.clear();
        constructor_asts_.clear();
        destructor_ast_.reset();
        declare_implicit_default_constructor();
        field_initializer_asts_.clear();
    }

private:
    // Store ASTs for potential VM compilation (keyed by name_id for efficiency)
    std::map<uint64_t, std::shared_ptr<function_decl>> method_asts_;
    std::vector<std::shared_ptr<function_decl>> constructor_asts_;
    std::vector<const function_decl*> private_ctors_;
    std::shared_ptr<function_decl> destructor_ast_;
    std::shared_ptr<function_decl> implicit_default_ctor_;
    std::shared_ptr<environment> namespace_scope_;
    // Vector (not map) to preserve source declaration order for field initializers.
    std::vector<std::pair<uint64_t, expression_ptr>> field_initializer_asts_;
    mutable uint64_t literal_memo_epoch_ = UINT64_MAX;
    mutable size_t literal_memo_count_ = SIZE_MAX;
    mutable bool literal_memo_ = false;
};

// Instance-method dispatcher body (declared in execution_backend.hpp). Resolves the
// overload against the live class every call (hot reload) and executes through the
// current backend; args[0] is the 'this' receiver.
inline checked_result<script_value> script_method_dispatch::operator()(const std::vector<script_value>& args) const {
    if (args.empty()) {
        return checked_result<script_value>(make_error_code(runtime_error_code::this_outside_method), "Method called without 'this' object");
    }
    return invoke(args[0], args, 1);
}

inline checked_result<const std::shared_ptr<function_decl>*> script_method_dispatch::resolve(const std::vector<script_value>& args,
                                                                                              size_t first) const {
    const size_t argc = args.size() - first;
    if (resolved_memo && resolved_memo_epoch == cls->method_epoch() && resolved_memo_argc == argc) { return resolved_memo; }
    const bool region = eng && detail::engine_access::parallel_region_active(*eng);
    auto resolved = region ? cls->resolve_method_overload_shared(name_id, args.data() + first, argc)
                           : cls->resolve_method_overload_ref(name_id, args.data() + first, argc);
    if (resolved && !region && cls->method_arity_decides(name_id, argc)) {
        resolved_memo = resolved.value();
        resolved_memo_epoch = cls->method_epoch();
        resolved_memo_argc = argc;
    }
    return resolved;
}

inline checked_result<script_value> script_method_dispatch::invoke(const script_value& this_obj, const std::vector<script_value>& args,
                                                                   size_t first, bool host_call) const {
    auto resolved = resolve(args, first);
    if (!resolved) {
        return resolved.error_value();
    }

    execution_backend* backend = eng ? detail::engine_access::calling_backend(*eng) : nullptr;
    if (!backend) {
        return checked_result<script_value>(make_error_code(runtime_error_code::engine_destroyed), "Engine backend unavailable");
    }

    const std::shared_ptr<function_decl>& ast = *resolved.value();
    if (backend->is_executing() || backend->is_unwinding()) {
        detail::engine_access::run_pin pin(host_call ? eng : nullptr);
        return backend->execute_method_from(*this, ast, this_obj, args, first);
    }
    struct call {
        const script_method_dispatch& dispatch; const std::shared_ptr<function_decl>& ast; const script_value& this_obj;
        const std::vector<script_value>& args; size_t first;
    } c{*this, ast, this_obj, args, first};
    return detail::engine_access::checked_host_entry(*eng, [](execution_backend& be, void* ctx) {
        auto* cc = static_cast<call*>(ctx);
        return be.execute_method_from(cc->dispatch, cc->ast, cc->this_obj, cc->args, cc->first);
    }, &c);
}

// Implementation of add_script_method. The stored dispatcher captures only
// (engine*, backend-neutral payload) and resolves the live backend at call time.
inline void class_definition::add_script_method(std::string_view name, std::shared_ptr<function_decl> ast, std::shared_ptr<environment> definition_env) {
    engine* eng = get_engine();
    if (!eng) return;

    // Track if this is a property getter for fast-path optimization
    if (name.size() > 5 && name.substr(0, 5) == "_get_") {
        has_property_getters_ = true;
    }

    uint64_t name_id = detail::engine_access::symbolize(*eng, name);

    // Store the AST in method_overloads_ for type-based resolution
    if (auto stamping = get_engine()) { ast->unit = detail::engine_access::current_unit(*stamping); }
    for (const auto& earlier : method_overloads_[name_id]) {
        if (detail::same_run_redefinition(*earlier, *ast)) [[unlikely]] {
            throw runtime_error(detail::duplicate_definition_text(name, ast->parameters, earlier->location.line));
        }
        if (detail::differs_only_by_reference(ast->parameters, earlier->parameters)) [[unlikely]] {
            throw runtime_error(detail::reference_only_overload_text(name, ast->parameters, earlier->parameters, earlier->location.line));
        }
    }
    method_overloads_[name_id].push_back(ast);

    // Invalidate overload resolution cache for this method name (new overload may change resolution)
    for (auto it = overload_resolution_cache_.begin(); it != overload_resolution_cache_.end(); ) {
        if (it->first.name_id == name_id) {
            it = overload_resolution_cache_.erase(it);
        } else {
            ++it;
        }
    }

    // Capture shared_ptr to this class definition for overload resolution
    // (intentional lifetime cycle: methods_ -> thunk -> class_def keeps the class alive
    // for bound-method copies)
    std::shared_ptr<class_definition> class_def = shared_from_this();

    methods_.insert_or_assign(name_id, script_value::make_function(
        script_method_dispatch{eng, class_def, name_id, definition_env}, eng));
    auto_accessor_ids_.erase(name_id);   // a user-defined _get_/_set_ is a CUSTOM accessor
    bump_method_epoch();
}

inline checked_result<script_value> script_static_method_dispatch::call(const std::vector<script_value>& args, bool host_call) const {
    execution_backend* backend = eng ? detail::engine_access::get_execution_backend(*eng) : nullptr;
    if (!backend) {
        return checked_result<script_value>(make_error_code(runtime_error_code::engine_destroyed), "Engine backend unavailable");
    }
    script_callable payload;
    payload.kind = script_callable::kind_type::static_method;
    payload.more().ast = fallback;
    payload.more().cls = cls;
    payload.more().definition_env = definition_env;
    const auto& table = cls->script_static_method_overloads();
    const auto it = table.find(name_id);
    if (it != table.end() && it->second.size() > 1) {
        const auto verdict = score_declarations(it->second, args.data(), args.size(), eng);
        if (verdict.rival) [[unlikely]] { return detail::ambiguous_call(name_id, (*verdict.best)->parameters, (*verdict.rival)->parameters, args.data(), args.size(), eng); }
        if (verdict.best) { payload.more().ast = *verdict.best; }
    }
    if (backend->is_executing() || backend->is_unwinding()) {
        detail::engine_access::run_pin pin(host_call ? eng : nullptr);
        return backend->execute_callable(payload, args);
    }
    struct call { const script_callable& payload; const std::vector<script_value>& args; } c{payload, args};
    return detail::engine_access::checked_host_entry(*eng, [](execution_backend& be, void* ctx) {
        auto* cc = static_cast<call*>(ctx);
        return be.execute_callable(cc->payload, cc->args);
    }, &c);
}

// Implementation of add_static_script_method. Same engine*+payload pattern; overloads
// are resolved over static_method_overloads_ with the shared resolver, falling back to
// the mint-time AST when nothing matches (preserving the old last-registered behavior).
inline void class_definition::add_static_script_method(std::string_view name, std::shared_ptr<function_decl> ast, std::shared_ptr<environment> definition_env) {
    engine* eng = get_engine();
    if (!eng) return;

    // Track if this is a property getter for fast-path optimization
    if (name.size() > 5 && name.substr(0, 5) == "_get_") {
        has_property_getters_ = true;
    }

    uint64_t name_id = detail::engine_access::symbolize(*eng, name);

    // Capture shared_ptr to this class definition (C++ scope rules for static methods)
    std::shared_ptr<class_definition> class_def = shared_from_this();

    // Store in overloads map for arity-aware lookup
    uint64_t method_name_id = ast->name_id;
    if (method_name_id == UINT64_MAX) {
        method_name_id = name_id;
    }
    if (auto stamping = get_engine()) { ast->unit = detail::engine_access::current_unit(*stamping); }
    for (const auto& earlier : static_method_overloads_[method_name_id]) {
        if (detail::same_run_redefinition(*earlier, *ast)) [[unlikely]] {
            throw runtime_error(detail::duplicate_definition_text(name, ast->parameters, earlier->location.line));
        }
        if (detail::differs_only_by_reference(ast->parameters, earlier->parameters)) [[unlikely]] {
            throw runtime_error(detail::reference_only_overload_text(name, ast->parameters, earlier->parameters, earlier->location.line));
        }
    }
    static_method_overloads_[method_name_id].push_back(ast);

    static_methods_.insert_or_assign(name_id, script_value::make_function(
        script_static_method_dispatch{eng, class_def, method_name_id, definition_env, ast}, eng));
    bump_method_epoch();
}

// Implementation of has_static_method_with_arity (needs full function_decl definition)
inline bool class_definition::has_static_method_with_arity(uint64_t name_id, size_t arity) const {
    // Check script method overloads (have full AST)
    auto it = static_method_overloads_.find(name_id);
    if (it != static_method_overloads_.end()) {
        for (const auto& func_decl : it->second) {
            if (arity_accepts(func_decl->parameters, arity)) {   // trailing defaults widen
                return true;
            }
        }
    }

    // Check C++ method arities
    auto arity_it = static_method_arities_.find(name_id);
    if (arity_it != static_method_arities_.end() && arity_it->second == arity) {
        return true;
    }

    return false;
}

// A defaulted-parameter constructor is NOT a converting
// constructor (deliberate C++ divergence) - implicit conversion requires a TRUE
// one-parameter constructor; opt in with a delegating overload
// (Money(int a) : this(a, 0) {}). Explicit construction keeps the arity window.
// Host-registered classes are unaffected (their ctor overloads are exact-arity).
inline bool has_true_single_param_conversion_ctor(const std::shared_ptr<class_definition>& def) {
    auto script_def = std::dynamic_pointer_cast<script_class_definition>(def);
    if (!script_def) { return true; }   // host classes: the dispatcher stays authoritative
    for (const auto& ctor_ast : script_def->get_constructor_asts()) {
        if (ctor_ast->parameters.size() == 1) { return true; }
    }
    return false;
}

// Collect a class declaration's private:/protected: labels into the runtime access map
// (both backends call this from their class-decl execution — parity by construction).
// A labeled constructor is recorded by overload: enforce_constructor_access checks the one overload resolution picks.
template<typename Symbolizer>
inline void apply_member_access_labels(class_definition& def, const class_decl& decl, Symbolizer& symbols) {
    std::unordered_map<uint64_t, access_level> nonpublic;
    std::vector<const function_decl*> private_ctors;
    for (const auto& member : decl.members) {
        if (member.visibility == class_decl::Public || !member.declaration) continue;
        uint64_t id = UINT64_MAX;
        if (member.declaration->get_type() == node_type::variable_decl) {
            auto* vd = static_cast<variable_decl*>(member.declaration.get());
            id = vd->name_id != UINT64_MAX ? vd->name_id : symbols.intern(std::string(vd->name));
        } else if (member.declaration->get_type() == node_type::function_decl) {
            auto* fd = static_cast<function_decl*>(member.declaration.get());
            if (fd->name == decl.name) {
                private_ctors.push_back(fd);
                continue;
            }
            id = fd->name_id != UINT64_MAX ? fd->name_id : symbols.intern(std::string(fd->name));
        }
        if (id != UINT64_MAX) {
            nonpublic.emplace(id, member.visibility == class_decl::Private
                                      ? access_level::private_access
                                      : access_level::protected_access);
        }
    }
    def.set_nonpublic_members(std::move(nonpublic));
    if (def.is_script_class()) {
        static_cast<script_class_definition&>(def).set_private_constructors(std::move(private_ctors));
    }
}

// Constructor access, enforce_member_access's rule applied to the overload resolution picked (both backends call it
// verbatim before an instance exists): a private constructor is callable from its class and every class derived from it.
// caller_context() answers the calling code's class, asked only when the overload is private
template<typename CallerContext>
inline checked_result<void> enforce_constructor_access(const script_class_definition& cls, const function_decl* ctor,
                                                       CallerContext&& caller_context) {
    if (!cls.constructor_is_private(ctor)) {
        return {};
    }
    const class_definition* ctx = caller_context();
    if (ctx == &cls || (ctx && ctx->derives_from(&cls))) {
        return {};
    }
    return checked_result<void>(make_error_code(runtime_error_code::access_violation),
        "Cannot access private constructor of class '{0}'", cls.get_type_id());
}

inline bool converting_constructor_takes(const script_value& arg, const type_info& target, engine* eng) {
    auto cls = detail::engine_access::get_class_definition(eng, target.type_name);
    if (!cls || !cls->is_script_class()) { return false; }
    for (const auto& ctor : static_cast<const script_class_definition&>(*cls).get_constructor_asts()) {
        if (ctor->parameters.size() == 1 && parameter_admits(ctor->parameters[0], arg)) { return true; }
    }
    return false;
}

} // namespace jai
