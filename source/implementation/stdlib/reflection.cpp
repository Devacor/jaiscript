#include <jaiscript/stdlib/reflection.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/properties/property_schema.hpp>
#include <jaiscript/stdlib/io.hpp>
#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <typeindex>
#include <vector>

namespace jai {
namespace stdlib {
namespace reflect_detail {

// The semantic type name: objects answer their class (ladder) name, everything else the
// same names the type system uses. References deref; cpp-bound primitives answer their
// semantic kind, never the storage wrapper.
inline std::string semantic_type_name(const script_value& val, engine* eng) {
    const script_value& d = val.is_reference() ? val.deref() : val;
    if (d.is_object()) {
        return object_type_name(d);
    }
    // Typed containers carry their canonical tag ("array<int>"); untyped report the kind
    if (d.is_array() || d.is_map()) {
        if (auto ti = d.get_type_info()) {
            if (ti->base_type == script_value_type::jai_array_type || ti->base_type == script_value_type::jai_map_type) { return ti->spelling(); }
        }
        return d.is_array() ? "array" : "map";
    }
    if (d.is_null()) { return "null"; }
    if (d.is_bool()) { return "bool"; }
    if (d.is_int()) { return "int"; }
    if (d.is_float()) { return "float"; }
    if (d.is_char()) { return "char"; }
    if (d.is_string()) { return "string"; }
    if (d.is_utf8_string()) { return "utf8_string"; }
    if (d.is_math()) { return math_type_name(d.current_type()); }
    if (d.is_function()) { return "function"; }
    if (d.is_weak_ptr()) { return "weak_ptr"; }
    (void)eng;
    return "unknown";
}

// object | "Class" -> class_definition. Unknown class NAME throws the family error;
// a non-class VALUE returns nullptr (callers decide whether that is an error).
inline class_definition* resolve_class(engine* eng, const script_value& v,
                                       const char* who) {
    // Raw pointers throughout: the engine's registries own every definition returned
    // here, so lifetime spans any single builtin call.
    const script_value& d = v.is_reference() ? v.deref() : v;
    if (d.is_string()) {
        const std::string name = d.as_string();
        auto def = detail::engine_access::get_class_definition(*eng, name);
        if (!def) {
            throw runtime_error(std::string("reflect::") + who + ": unknown class '" + name + "'");
        }
        return def.get();
    }
    if (d.is_object()) {
        try {
            auto instance = d.as<strong_ptr<class_instance>>();
            if (instance) {
                if (auto* def = instance->get_class_definition()) { return def; }
            }
        } catch (...) {}
        // Host-bound object without a class_instance wrapper: resolve by semantic name
        if (auto ti = d.get_type_info()) {
            if (!ti->type_name.empty()) {
                if (auto def = detail::engine_access::get_class_definition(*eng, ti->type_name)) { return def.get(); }
            }
        }
    }
    return nullptr;
}

inline void expect_args(const std::vector<script_value>& args, size_t n, const char* who) {
    if (args.size() != n) {
        throw runtime_error(std::string("reflect::") + who + " expects " +
                            std::to_string(n) + (n == 1 ? " argument" : " arguments"));
    }
}

inline void expect_min_args(const std::vector<script_value>& args, size_t n, const char* who) {
    if (args.size() < n) {
        throw runtime_error(std::string("reflect::") + who + " expects at least " +
                            std::to_string(n) + (n == 1 ? " argument" : " arguments"));
    }
}

inline const std::string& expect_name(const std::vector<script_value>& args, size_t i, const char* who) {
    const script_value& d = args[i].deref();
    if (!d.is_string()) {
        throw runtime_error(std::string("reflect::") + who + ": name must be a string");
    }
    return d.as_string();
}

// A name the engine never interned names no member, function or namespace: the lookups ask without interning, so a
// script probing unknown names does not grow the symbolizer (and nothing interns while it is frozen)
inline std::optional<uint64_t> known_symbol(engine* eng, std::string_view name) {
    return detail::engine_access::get_symbolizer(*eng)->find(name);
}

// The B-group doors. KEEP ALIGNED with interpreter::resolve_member_target — the ONE
// object-shape resolution direct member syntax uses (class_instance wrappers, cpp_bound
// holders, raw holders resolved by engine registration).
struct member_door {
    strong_ptr<class_instance> instance;       // fields door; null for host holders
    std::shared_ptr<class_definition> engine_def;   // anchors engine-resolved definitions
    class_definition* def = nullptr;                // methods/accessors, both shapes
};

inline member_door resolve_member_door(engine* eng, const script_value& value) {
    member_door door;
    const script_value& v = value.deref();
    auto holder = const_cast<script_value&>(v).get_object_holder();
    if (!holder) { return door; }
    if (holder->is_instance()) {
        door.instance = holder->instance_handle();
        if (door.instance) { door.def = door.instance->get_class_definition(); }
    } else {
        door.engine_def = holder->type_id() != UINT64_MAX
            ? detail::engine_access::get_class_definition(*eng, holder->type_id())
            : detail::engine_access::get_class_definition(*eng, std::string(holder->type_name()));
        door.def = door.engine_def.get();
    }
    return door;
}

// Surface a kernel error with the SAME final text the backends produce at their
// boundary: plain {0}/{1} substitution through the symbolizer (checked_result.hpp)
template <typename T>
[[noreturn]] inline void throw_kernel_error(engine* eng, const checked_result<T>& r) {
    throw runtime_error(format_error_message(r.message(),
        detail::engine_access::get_symbolizer(*eng)->get_string(r.symbol_id()),
        detail::engine_access::get_symbolizer(*eng)->get_string(r.symbol_id2())));
}

// Reflection reaches a script class's private constructors as it reaches its other private members
inline checked_result<script_value> construct_through_reflection(engine* eng, const std::string& name,
                                                                 const script_value& ctor, const std::vector<script_value>& args) {
    const std::shared_ptr<class_definition> def = detail::engine_access::get_class_definition(*eng, name);
    if (def && def->is_script_class()) {
        detail::engine_access::construction_access_waiver waiver(eng);
        return ctor.as_function()(args);
    }
    return ctor.as_function()(args);
}

// Strict `ident(::ident)*` shape — the gate before a name string may be evaluated
// as a member expression (never an expression-injection surface)
inline bool is_qualified_identifier(const std::string& name) {
    auto ident_start = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; };
    auto ident_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    size_t i = 0;
    while (i < name.size()) {
        if (!ident_start(name[i])) { return false; }
        ++i;
        while (i < name.size() && ident_char(name[i])) { ++i; }
        if (i == name.size()) { return true; }
        if (name[i] != ':' || i + 1 >= name.size() || name[i + 1] != ':') { return false; }
        i += 2;
    }
    return false;
}

// Resolve the callable a free-form name means: "ns::fn" through the engine's script
// namespaces, a bare name through the global environment — the same places direct
// call syntax resolves.
inline script_value resolve_free_callable(engine* eng, const std::string& name, const char* who) {
    if (auto pos = name.find("::"); pos != std::string::npos) {
        auto& spaces = detail::engine_access::script_namespaces(*eng);
        const auto ns_id = known_symbol(eng, std::string_view(name).substr(0, pos));
        const auto member_known = known_symbol(eng, std::string_view(name).substr(pos + 2));
        auto ns_it = ns_id && member_known ? spaces.find(*ns_id) : spaces.end();
        if (ns_it != spaces.end() && ns_it->second) {
            const uint64_t member_id = *member_known;
            if (ns_it->second->scope) {   // a function held by a namespace variable
                if (auto* cell = ns_it->second->scope->get_own_value_ptr(member_id); cell && cell->deref().is_function()) {
                    return cell->deref();
                }
            }
            auto var_it = ns_it->second->variables.find(member_id);
            if (var_it != ns_it->second->variables.end() && var_it->second.is_function()) {
                return var_it->second;
            }
            // Script-declared namespace functions are AST overload sets, not values.
            // Evaluating the (identifier-shape-validated) name AS the member expression
            // mints the very callable direct syntax uses — overload pick, defaults,
            // namespace-variable environment, byte-identical behavior by construction.
            if (ns_it->second->functions.count(member_id) && is_qualified_identifier(name)) {
                script_value fn = eng->execute(name);
                if (fn.is_function()) { return fn; }
            }
        }
    } else if (const auto id = known_symbol(eng, name)) {
        auto found = detail::engine_access::get_global_environment(*eng)->get(*id);
        if (found && found.value().is_function()) {
            return std::move(found.value());
        }
    }
    throw runtime_error(std::string("reflect::") + who + ": unknown function '" + name + "'");
}

// A C++ value type's script-facing name for field entries (host property types)
inline std::string cpp_type_display_name(engine* eng, std::type_index ti) {
    if (ti == std::type_index(typeid(int)) || ti == std::type_index(typeid(int64_t)) ||
        ti == std::type_index(typeid(script_int))) { return "int"; }
    if (ti == std::type_index(typeid(float)) || ti == std::type_index(typeid(double)) ||
        ti == std::type_index(typeid(script_float))) { return "float"; }
    if (ti == std::type_index(typeid(bool))) { return "bool"; }
    if (ti == std::type_index(typeid(std::string))) { return "string"; }
    if (auto def = detail::engine_access::get_class_definition_by_type(*eng, ti)) { return def->get_name(); }
    return "var";
}

inline script_value make_field_entry(engine* eng, const std::string& name, const std::string& type,
                                     const std::string& kind, const std::string& access,
                                     bool is_static, const std::string& from) {
    auto entry = script_value::make_map(detail::engine_access::get_type_info_string(*eng), nullptr, eng);
    auto& m = const_cast<script_map&>(entry.as_map());
    m[script_value(std::string("name"), eng)] = script_value(name, eng);
    m[script_value(std::string("type"), eng)] = script_value(type, eng);
    m[script_value(std::string("kind"), eng)] = script_value(kind, eng);
    m[script_value(std::string("access"), eng)] = script_value(access, eng);
    m[script_value(std::string("static"), eng)] = script_value(is_static, eng);
    m[script_value(std::string("from"), eng)] = script_value(from, eng);
    return entry;
}

// A declared slot's type as the script spells it: auto when undeclared, var for any, element types spelled out
inline std::string declared_type_spelling(const type_info_ptr& type) {
    if (!type || (type->type_name.empty() && type->base_type == script_value_type::jai_object_type)) { return "auto"; }
    return type->spelling();
}

// Base-first, declaration-order field walk (the determinism contract): each level
// contributes its OWN fields from the retained field_order_, parents before children.
inline void append_fields_of(engine* eng, class_definition* def, script_value& arr) {
    for (const auto& parent : def->get_parent_classes()) {
        if (parent) { append_fields_of(eng, parent.get(), arr); }
    }
    const std::string from = def->get_name();
    for (uint64_t id : def->field_order()) {
        const std::string name{detail::engine_access::get_symbolizer(*eng)->get_string(id)};
        const std::string type = declared_type_spelling(def->is_static_field(id) ? def->get_static_declared_type(id)
                                                                                 : def->get_field_declared_type(id));
        std::string access = "public";
        access_level level = access_level::public_access;
        if (def->find_nonpublic_declarer(id, level)) {
            access = (level == access_level::private_access) ? "private" : "protected";
        }
        arr.as_array().push_back(make_field_entry(eng, name, type, "field", access,
                                                  def->is_static_field(id), from));
    }
    // Host-backed classes: the type_registry schema is the declaration record —
    // own_properties() is declaration order, kind distinguishes signals/observables
    if (def->cpp_backed() && def->cpp_type()) {
        if (const auto* schema = type_registry::instance().try_get(std::type_index(*def->cpp_type()))) {
            for (const auto& prop : schema->own_properties()) {
                const char* kind = prop.is_signal ? "signal"
                                 : prop.is_observable ? "observable" : "field";
                arr.as_array().push_back(make_field_entry(
                    eng, prop.name, cpp_type_display_name(eng, prop.value_type_id),
                    kind, "public", false, from));
            }
        }
    }
}

// ---- C group: navigate (methods, functions, globals, argument lists) ----

inline script_value make_param_entry(engine* eng, const std::string& name,
                                     const std::string& type, bool has_default) {
    auto entry = script_value::make_map(detail::engine_access::get_type_info_string(*eng), nullptr, eng);
    auto& m = const_cast<script_map&>(entry.as_map());
    m[script_value(std::string("name"), eng)] = script_value(name, eng);
    m[script_value(std::string("type"), eng)] = script_value(type, eng);
    m[script_value(std::string("default"), eng)] = script_value(has_default, eng);
    return entry;
}

inline std::string declared_param_type_name(const parameter& p) {
    return declared_type_spelling(p.type);
}

inline script_value params_array_for(engine* eng, const std::vector<parameter>& params) {
    auto arr = script_value::make_array(nullptr, eng);
    for (const auto& p : params) {
        arr.as_array().push_back(make_param_entry(
            eng, p.name, declared_param_type_name(p), p.default_value != nullptr));
    }
    return arr;
}

inline std::string cpp_param_type_name(engine* eng, const param_type_info& pt) {
    switch (pt.base_type) {
        case script_value_type::jai_int_type: return "int";
        case script_value_type::jai_float_type: return "float";
        case script_value_type::jai_bool_type: return "bool";
        case script_value_type::jai_string_type: return "string";
        case script_value_type::jai_char_type: return "char";
        case script_value_type::jai_array_type: return "array";
        case script_value_type::jai_map_type: return "map";
        case script_value_type::jai_object_type:
            if (auto def = detail::engine_access::get_class_definition_by_type(*eng, pt.cpp_type)) {
                return def->get_name();
            }
            return "object";
        default: return "var";
    }
}

inline script_value make_method_entry(engine* eng, const std::string& name, int64_t arity,
                                      bool is_static, const std::string& access,
                                      const std::string& from, script_value params) {
    auto entry = script_value::make_map(detail::engine_access::get_type_info_string(*eng), nullptr, eng);
    auto& m = const_cast<script_map&>(entry.as_map());
    m[script_value(std::string("name"), eng)] = script_value(name, eng);
    m[script_value(std::string("arity"), eng)] = script_value(static_cast<script_int>(arity), eng);
    m[script_value(std::string("static"), eng)] = script_value(is_static, eng);
    m[script_value(std::string("access"), eng)] = script_value(access, eng);
    m[script_value(std::string("from"), eng)] = script_value(from, eng);
    m[script_value(std::string("params"), eng)] = std::move(params);
    return entry;
}

// Chain walk for reflect::methods / method_arguments: OWN tables per level,
// derived-first with (name, arity, static) dedupe so an override wins; results carry
// a sort key — enumeration order is (name, arity, static), never hash order.
struct method_collector {
    engine* eng;
    std::vector<std::pair<std::tuple<std::string, int64_t, bool>, script_value>> entries;
    std::set<std::tuple<std::string, int64_t, bool>> seen;

    void add(const std::string& name, int64_t arity, bool is_static,
             const class_definition* level, script_value params) {
        std::tuple<std::string, int64_t, bool> key{name, arity, is_static};
        if (!seen.insert(key).second) { return; }
        std::string access = "public";
        access_level lvl = access_level::public_access;
        if (level->find_nonpublic_declarer(detail::engine_access::get_symbolizer(*eng)->intern(name), lvl)) {
            access = (lvl == access_level::private_access) ? "private" : "protected";
        }
        entries.emplace_back(std::move(key),
            make_method_entry(eng, name, arity, is_static, access, level->get_name(),
                              std::move(params)));
    }

    script_value sorted_array() {
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        auto arr = script_value::make_array(nullptr, eng);
        for (auto& e : entries) { arr.as_array().push_back(std::move(e.second)); }
        return arr;
    }
};

// only_id = 0 enumerates everything; a member id filters to that name. Auto-accessors
// and _get_/_set_ spellings are the PROPERTY surface (fields() owns them); a method
// sharing the class's name is the constructor surface (construct() owns it).
inline void collect_methods_of(engine* eng, const class_definition* def,
                               method_collector& out, uint64_t only_id) {
    auto* sym = detail::engine_access::get_symbolizer(*eng);
    auto surfaced = [&](uint64_t id, std::string& name_out) {
        if (only_id && id != only_id) { return false; }
        name_out = std::string(sym->get_string(id));
        if (name_out.empty() || name_out == def->get_name()) { return false; }
        if (name_out.rfind("_get_", 0) == 0 || name_out.rfind("_set_", 0) == 0) { return false; }
        return true;
    };
    std::string name;
    for (const auto& [id, decls] : def->script_method_overloads()) {
        if (!surfaced(id, name)) { continue; }
        for (const auto& fd : decls) {
            out.add(name, static_cast<int64_t>(fd->parameters.size()), false, def,
                    params_array_for(eng, fd->parameters));
        }
    }
    for (const auto& [id, decls] : def->script_static_method_overloads()) {
        if (!surfaced(id, name)) { continue; }
        for (const auto& fd : decls) {
            out.add(name, static_cast<int64_t>(fd->parameters.size()), true, def,
                    params_array_for(eng, fd->parameters));
        }
    }
    for (const auto& [id, by_arity] : def->cpp_method_overload_table()) {
        if (!surfaced(id, name)) { continue; }
        for (const auto& [arity, overloads] : by_arity) {
            for (const auto& entry : overloads) {
                auto params = script_value::make_array(nullptr, eng);
                if (entry.param_types.empty()) {   // legacy untyped binding: arity only
                    for (size_t i = 0; i < arity; ++i) {
                        params.as_array().push_back(make_param_entry(
                            eng, "arg" + std::to_string(i), "var", false));
                    }
                } else {
                    size_t i = 0;
                    for (const auto& pt : entry.param_types) {
                        params.as_array().push_back(make_param_entry(
                            eng, "arg" + std::to_string(i++), cpp_param_type_name(eng, pt), false));
                    }
                }
                out.add(name, static_cast<int64_t>(arity), false, def, std::move(params));
            }
        }
    }
    // Names in neither table (opaque add_method_by_id registrations): unknown signature
    for (const auto& [id, value] : def->method_values()) {
        if (!surfaced(id, name) || !value.is_function()) { continue; }
        if (def->script_method_overloads().count(id) ||
            def->cpp_method_overload_table().count(id)) { continue; }
        out.add(name, -1, false, def, script_value::make_array(nullptr, eng));
    }
    for (const auto& [id, value] : def->static_method_values()) {
        if (!surfaced(id, name) || !value.is_function()) { continue; }
        if (def->script_static_method_overloads().count(id)) { continue; }
        out.add(name, -1, true, def, script_value::make_array(nullptr, eng));
    }
    for (const auto& parent : def->get_parent_classes()) {
        if (parent) { collect_methods_of(eng, parent.get(), out, only_id); }
    }
    if (auto cpp_base = def->get_cpp_base_class()) {
        collect_methods_of(eng, cpp_base.get(), out, only_id);
    }
}

} // namespace reflect_detail

void register_reflection_functions(engine& eng_ref) {
    detail::engine_access::library_registration library(eng_ref);
    engine* eng = &eng_ref;
    auto& ns = detail::engine_access::script_namespaces(eng_ref)[detail::engine_access::get_symbolizer(eng_ref)->intern("reflect")];
    if (!ns) { ns = std::make_shared<script_namespace_data>(); }

    auto add = [&](const char* name, script_function fn) {
        ns->variables[detail::engine_access::get_symbolizer(eng_ref)->intern(name)] =
            script_value::make_function(std::move(fn), eng);
    };

    add("type_name", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 1, "type_name");
        return script_value(reflect_detail::semantic_type_name(args[0], eng), eng);
    });

    add("is_cpp_bound", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 1, "is_cpp_bound");
        const script_value& d = args[0].is_reference() ? args[0].deref() : args[0];
        // Class form: host-registered vs script-declared
        if (d.is_string()) {
            auto def = reflect_detail::resolve_class(eng, d, "is_cpp_bound");
            return script_value(def->cpp_backed(), eng);
        }
        // Value form: is this value C++-backed (bound primitive or bound object payload)?
        if (d.is_cpp_bound_primitive()) { return script_value(true, eng); }
        if (d.is_object()) {
            if (d.get_cpp_bound_ptr() != nullptr) { return script_value(true, eng); }
            if (auto def = reflect_detail::resolve_class(eng, d, "is_cpp_bound")) {
                return script_value(def->cpp_backed(), eng);
            }
        }
        return script_value(false, eng);
    });

    add("has_field", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 2, "has_field");
        if (!args[1].is_string()) { throw runtime_error("reflect::has_field expects a field name string"); }
        auto def = reflect_detail::resolve_class(eng, args[0], "has_field");
        if (!def) { return script_value(false, eng); }
        const std::string& field = args[1].as_string();
        const auto id = reflect_detail::known_symbol(eng, field);
        if (id && def->get_all_field_defaults().count(*id)) { return script_value(true, eng); }
        for (auto sv : def->get_static_field_names()) {
            if (sv == field) { return script_value(true, eng); }
        }
        for (const auto& prop : def->get_property_names(true)) {
            if (prop == field) { return script_value(true, eng); }
        }
        return script_value(false, eng);
    });

    add("has_method", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 2, "has_method");
        if (!args[1].is_string()) { throw runtime_error("reflect::has_method expects a method name string"); }
        auto def = reflect_detail::resolve_class(eng, args[0], "has_method");
        if (!def) { return script_value(false, eng); }
        const auto id = reflect_detail::known_symbol(eng, args[1].as_string());
        return script_value(id && def->defines_method(*id), eng);
    });

    add("classes", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 0, "classes");
        auto arr = script_value::make_array(nullptr, eng);
        for (const auto& name : eng->registered_class_names()) {
            arr.as_array().push_back(script_value(name, eng));
        }
        return arr;
    });

    add("bases", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 1, "bases");
        auto def = reflect_detail::resolve_class(eng, args[0], "bases");
        auto arr = script_value::make_array(nullptr, eng);
        if (def) {
            for (const auto& parent : def->get_parent_classes()) {
                if (parent) { arr.as_array().push_back(script_value(parent->get_name(), eng)); }
            }
        }
        return arr;
    });

    add("generation", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 1, "generation");
        auto def = reflect_detail::resolve_class(eng, args[0], "generation");
        if (!def) { throw runtime_error("reflect::generation expects a class or class name"); }
        return script_value(static_cast<script_int>(def->reflection_generation()), eng);
    });

    add("fields", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 1, "fields");
        auto arr = script_value::make_array(nullptr, eng);
        // Non-class values (primitives, containers, functions) answer an empty array —
        // a property grid can call fields() on anything (front door §A)
        if (auto* def = reflect_detail::resolve_class(eng, args[0], "fields")) {
            reflect_detail::append_fields_of(eng, def, arr);
        }
        return arr;
    });

    add("instances", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 1, "instances");
        auto* def = reflect_detail::resolve_class(eng, args[0], "instances");
        if (!def) { throw runtime_error("reflect::instances expects a class or class name"); }
        auto arr = script_value::make_array(nullptr, eng);
        for (auto& inst : def->live_instances()) {
            arr.as_array().push_back(
                script_value::make_object(def->get_name(), std::move(inst), eng));
        }
        return arr;
    });

    // ---- B group: act (read, write, call, construct). The one rule: reflection is a
    // spelling, not a bypass — writes and invokes run the SAME access/type kernels as
    // direct syntax (byte-identical error text); reads see everything (the to_json rule).

    add("get", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 2, "get");
        const std::string& name = reflect_detail::expect_name(args, 1, "get");
        const auto known = reflect_detail::known_symbol(eng, name);
        const uint64_t id = known ? *known : UINT64_MAX;
        const script_value& target = args[0].deref();
        if (target.is_string()) {   // class form: static read
            auto* def = reflect_detail::resolve_class(eng, target, "get");
            if (const script_value* field = known ? def->get_static_field_ptr(id) : nullptr) {
                return field->deref();
            }
            throw runtime_error("reflect::get: class '" + def->get_name() +
                                "' has no static field '" + name + "'");
        }
        auto door = reflect_detail::resolve_member_door(eng, target);
        if (!door.instance && !door.def) {
            throw runtime_error("reflect::get expects an object or class name");
        }
        // Getter first, like direct reads: a _get_<name> method SHADOWS the field
        // (script classes synthesize an auto-getter whose body is the verbatim field
        // read, so this is raw-read-equivalent there; host .property() bindings and
        // computed getters only answer here). No access check: reads see everything.
        if (known) {
            if (door.def) {
                script_value getter = door.def->get_method(detail::engine_access::get_symbolizer(*eng)->get_getter_id_with_view(id).first, false);
                if (getter.is_function()) {
                    return make_bound_method(target, getter).as_function()({});
                }
            }
            if (door.instance) {
                if (const script_value* field = door.instance->find_field_value(id)) {
                    return field->deref();
                }
            }
            if (door.def) {
                if (const script_value* field = door.def->get_static_field_ptr(id)) {
                    return field->deref();
                }
            }
        }
        throw runtime_error("reflect::get: '" +
            (door.def ? door.def->get_name() : std::string("object")) +
            "' has no member '" + name + "'");
    });

    add("set", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 3, "set");
        const std::string& name = reflect_detail::expect_name(args, 1, "set");
        const auto known = reflect_detail::known_symbol(eng, name);
        const uint64_t id = known ? *known : UINT64_MAX;
        script_value value = args[2].deref();
        const script_value& target = args[0].deref();
        if (target.is_string()) {   // class form: static write
            auto* def = reflect_detail::resolve_class(eng, target, "set");
            if (!known || !def->get_static_field_ptr(id)) {
                throw runtime_error("reflect::set: class '" + def->get_name() +
                                    "' has no static field '" + name + "'");
            }
            (void)def->set_static_field(id, std::move(value));
            return script_value(std::monostate{}, eng);
        }
        auto door = reflect_detail::resolve_member_door(eng, target);
        if (!door.instance && !door.def) {
            throw runtime_error("reflect::set expects an object or class name");
        }
        // Setter first, like direct writes: a _set_<name> method shadows the field
        // (the script auto-setter body IS enforce_field_write + store, so this is
        // raw-store-equivalent there; host property setters only answer here).
        if (known && door.def) {
            script_value setter = door.def->get_method(detail::engine_access::get_symbolizer(*eng)->get_setter_id_with_view(id).first, false);
            if (setter.is_function()) {
                return make_bound_method(target, setter).as_function()({std::move(value)});
            }
        }
        if (known && door.instance && door.instance->find_field_value(id)) {
            auto converted = door.instance->enforce_field_write(id, std::move(value));
            if (!converted) { reflect_detail::throw_kernel_error(eng, converted); }
            door.instance->set_field_unchecked(id, std::move(converted.value()));
            return script_value(std::monostate{}, eng);
        }
        if (known && door.def && door.def->get_static_field_ptr(id)) {
            (void)door.def->set_static_field(id, std::move(value));
            return script_value(std::monostate{}, eng);
        }
        throw runtime_error("reflect::set: '" +
            (door.def ? door.def->get_name() : std::string("object")) +
            "' has no member '" + name + "'");
    });

    add("invoke", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_min_args(args, 2, "invoke");
        const std::string& name = reflect_detail::expect_name(args, 1, "invoke");
        const auto known = reflect_detail::known_symbol(eng, name);
        const uint64_t id = known ? *known : UINT64_MAX;
        std::vector<script_value> call_args(args.begin() + 2, args.end());
        const script_value& target = args[0].deref();
        if (target.is_string()) {   // class form: static method
            auto* def = reflect_detail::resolve_class(eng, target, "invoke");
            script_value method = known ? def->get_static_method(id, false) : script_value::make_null(eng);
            if (method.is_function()) {
                return method.as_function()(call_args);
            }
            throw runtime_error("reflect::invoke: class '" + def->get_name() +
                                "' has no static method '" + name + "'");
        }
        auto door = reflect_detail::resolve_member_door(eng, target);
        if (!door.def) {
            throw runtime_error("reflect::invoke expects an object or class name");
        }
        script_value dispatcher = known ? door.def->get_method(id, false) : script_value::make_invalid(eng);
        if (!dispatcher.is_function()) {
            throw runtime_error("reflect::invoke: '" + door.def->get_name() +
                                "' has no method '" + name + "'");
        }
        return make_bound_method(target, dispatcher).as_function()(call_args);
    });

    // A method taken as a value is spelled reflect::bind(obj, "name"): the callable holds the object
    // and keeps it alive, exactly as a bound method always has; a value type's methods run on the value, so a lambda wraps them
    add("bind", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 2, "bind");
        const std::string& name = reflect_detail::expect_name(args, 1, "bind");
        const script_value& target = args[0].deref();
        auto door = reflect_detail::resolve_member_door(eng, target);
        if (!door.def) {
            throw runtime_error("reflect::bind takes a class instance, a handle or a host object; the methods of " +
                                reflect_detail::semantic_type_name(target, eng) +
                                " run on the value: wrap the call in a lambda ([&]() { ... })");
        }
        const auto known = reflect_detail::known_symbol(eng, name);
        const uint64_t id = known ? *known : UINT64_MAX;
        script_value dispatcher = known ? door.def->get_method(id, false) : script_value::make_invalid(eng);
        if (!dispatcher.is_function()) {
            throw runtime_error("reflect::bind: '" + door.def->get_name() + "' has no method '" + name + "'");
        }
        return make_bound_method(target, dispatcher);
    });

    add("call", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_min_args(args, 1, "call");
        const std::string& name = reflect_detail::expect_name(args, 0, "call");
        std::vector<script_value> call_args(args.begin() + 1, args.end());
        script_value fn = reflect_detail::resolve_free_callable(eng, name, "call");
        return fn.as_function()(call_args);
    });

    add("construct", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_min_args(args, 1, "construct");
        const std::string& name = reflect_detail::expect_name(args, 0, "construct");
        std::vector<script_value> call_args(args.begin() + 1, args.end());
        // The same global the direct spelling Class(...) resolves
        auto ctor = detail::engine_access::get_global_environment(*eng)->get(name);
        if (!ctor || !ctor.value().is_function()) {
            throw runtime_error(std::string("reflect::construct: unknown class '") + name + "'");
        }
        return reflect_detail::construct_through_reflection(eng, name, ctor.value(), call_args);
    });

    add("construct_shared", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_min_args(args, 1, "construct_shared");
        const std::string& name = reflect_detail::expect_name(args, 0, "construct_shared");
        std::vector<script_value> call_args(args.begin() + 1, args.end());
        auto ctor = detail::engine_access::get_global_environment(*eng)->get(name);
        if (!ctor || !ctor.value().is_function()) {
            throw runtime_error(std::string("reflect::construct_shared: unknown class '") + name + "'");
        }
        auto result = reflect_detail::construct_through_reflection(eng, name, ctor.value(), call_args);
        if (!result) { return result; }
        script_value value = std::move(result.value());
        // `new Class(...)` semantics: shared_ptr is a TYPE MARKER, not storage — mark
        // the constructed object exactly like the backends' new-expr paths
        if (value.type() == script_value_type::jai_object_type) {
            type_info* pointee = value.get_type_info().get();
            if (!pointee) {
                if (auto def = detail::engine_access::get_class_definition(*eng, name)) {
                    pointee = def->get_type_info().get();
                }
            }
            value.set_type_info(detail::engine_access::get_type_info_shared_ptr(*eng, pointee));
        }
        return value;
    });

    // ---- C group: navigate (methods, functions, globals, argument lists) ----

    add("methods", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 1, "methods");
        auto* def = reflect_detail::resolve_class(eng, args[0], "methods");
        if (!def) { throw runtime_error("reflect::methods expects a class or class name"); }
        reflect_detail::method_collector out{eng};
        reflect_detail::collect_methods_of(eng, def, out, 0);
        return out.sorted_array();
    });

    add("method_arguments", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 2, "method_arguments");
        const std::string& name = reflect_detail::expect_name(args, 1, "method_arguments");
        auto* def = reflect_detail::resolve_class(eng, args[0], "method_arguments");
        if (!def) { throw runtime_error("reflect::method_arguments expects a class or class name"); }
        reflect_detail::method_collector out{eng};
        if (const auto known = reflect_detail::known_symbol(eng, name)) {
            reflect_detail::collect_methods_of(eng, def, out, *known);
        }
        if (out.entries.empty()) {
            throw runtime_error("reflect::method_arguments: '" + def->get_name() +
                                "' has no method '" + name + "'");
        }
        // One param-array PER OVERLOAD (overloads make any single answer ambiguous)
        auto sorted = out.sorted_array();
        auto arr = script_value::make_array(nullptr, eng);
        for (auto& entry : const_cast<std::vector<script_value>&>(sorted.as_array())) {
            arr.as_array().push_back(
                const_cast<script_map&>(entry.as_map())[script_value(std::string("params"), eng)]);
        }
        return arr;
    });

    add("functions", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 0, "functions");
        std::set<std::string> names;   // sorted, deduped — the determinism contract
        for (auto& [name_view, value] : detail::engine_access::get_global_environment(*eng)->get_all_variables()) {
            if (!value.is_function()) { continue; }
            std::string name{name_view};
            if (name.find('#') != std::string::npos) { continue; }   // a builtin a desugaring calls (template#html): no script names it
            if (detail::engine_access::get_class_definition(*eng, name)) { continue; }   // constructors: classes() owns them
            names.insert(std::move(name));
        }
        auto* sym = detail::engine_access::get_symbolizer(*eng);
        for (auto& [ns_id, data] : detail::engine_access::script_namespaces(*eng)) {
            if (!data) { continue; }
            const std::string ns{sym->get_string(ns_id)};
            for (auto& [fid, decls] : data->functions) {
                (void)decls;
                names.insert(ns + "::" + std::string(sym->get_string(fid)));
            }
            for (auto& [vid, value] : data->variables) {
                if (value.is_function()) {
                    names.insert(ns + "::" + std::string(sym->get_string(vid)));
                }
            }
            if (data->scope) {
                for (auto& [vname, value] : data->scope->get_all_variables()) {
                    if (value.deref().is_function()) {
                        names.insert(ns + "::" + std::string(vname));
                    }
                }
            }
        }
        auto arr = script_value::make_array(nullptr, eng);
        for (const auto& n : names) { arr.as_array().push_back(script_value(n, eng)); }
        return arr;
    });

    add("globals", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 0, "globals");
        std::set<std::string> names;
        for (auto& [name_view, value] : detail::engine_access::get_global_environment(*eng)->get_all_variables()) {
            if (value.is_function()) { continue; }
            if (value.is_object()) {   // class-definition holders are registry internals
                auto holder = value.get_object_holder();
                if (holder && holder->type_name() == "class_definition") { continue; }
            }
            names.insert(std::string(name_view));
        }
        auto arr = script_value::make_array(nullptr, eng);
        for (const auto& n : names) { arr.as_array().push_back(script_value(n, eng)); }
        return arr;
    });

    add("function_arguments", [eng](const std::vector<script_value>& args) -> checked_result<script_value> {
        reflect_detail::expect_args(args, 1, "function_arguments");
        const std::string& name = reflect_detail::expect_name(args, 0, "function_arguments");
        auto arr = script_value::make_array(nullptr, eng);
        if (auto pos = name.find("::"); pos != std::string::npos) {
            auto* sym = detail::engine_access::get_symbolizer(*eng);
            auto& spaces = detail::engine_access::script_namespaces(*eng);
            const auto ns_id = sym->find(std::string_view(name).substr(0, pos));
            const auto member_id = sym->find(std::string_view(name).substr(pos + 2));
            auto ns_it = ns_id && member_id ? spaces.find(*ns_id) : spaces.end();
            if (ns_it != spaces.end() && ns_it->second) {
                auto fn_it = ns_it->second->functions.find(*member_id);
                if (fn_it != ns_it->second->functions.end()) {
                    for (const auto& fd : fn_it->second) {
                        arr.as_array().push_back(
                            reflect_detail::params_array_for(eng, fd->parameters));
                    }
                    return arr;
                }
                // a host entry that declares its signature answers it (arguments argN, as host methods do)
                auto sig_it = ns_it->second->signatures.find(*member_id);
                if (sig_it != ns_it->second->signatures.end()) {
                    const host_entry_signature& sig = sig_it->second;
                    auto params = script_value::make_array(nullptr, eng);
                    for (size_t i = 0; i < sig.max_args; ++i) {
                        const script_value_type t = i < sig.params.size() ? sig.params[i] : script_value_type::jai_null_type;
                        params.as_array().push_back(reflect_detail::make_param_entry(eng, "arg" + std::to_string(i),
                            reflect_detail::cpp_param_type_name(eng, param_type_info(t)), i >= sig.min_args));
                    }
                    arr.as_array().push_back(std::move(params));
                    return arr;
                }
            }
        } else if (const auto id = reflect_detail::known_symbol(eng, name)) {
            auto found = detail::engine_access::get_global_environment(*eng)->get(*id);
            if (found && found.value().is_function()) {
                // Script functions carry their decl in the callable payload; host
                // registrations are type-erased — they answer an EMPTY overload list
                const script_function& fn = found.value().as_function();
                if (const auto* thunk = fn.target<script_callable_thunk>()) {
                    if (thunk->payload.fn && thunk->payload.fn->shared_parameters) {
                        arr.as_array().push_back(reflect_detail::params_array_for(
                            eng, *thunk->payload.fn->shared_parameters));
                    } else if (thunk->payload.ast()) {
                        arr.as_array().push_back(reflect_detail::params_array_for(
                            eng, thunk->payload.ast()->parameters));
                    }
                }
                return arr;
            }
        }
        throw runtime_error("reflect::function_arguments: unknown function '" + name + "'");
    });
}

// shared_ptr / raw-pointer convenience, matching every other stdlib unit

} // namespace stdlib
} // namespace jai

