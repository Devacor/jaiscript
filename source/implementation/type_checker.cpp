// Static type checker (docs/site/embedding/06-static-checking.html). Backend-neutral AST walk,
// run once per unique source at parse-cache-entry creation (engine.cpp hook).
//
// Guiding contract: NO FALSE POSITIVES. A program that runs clean and doesn't
// genuinely violate declared types must produce zero error diagnostics. Anywhere
// the static story is uncertain (var, unknown classes, includes, builtin methods,
// unknown element types, host object conversions) the checker stays lenient.
// Known declared containers follow their element boundary; scalar destinations
// follow detail::convert_for_typed_slot. Declared references, weak handles and
// callable slots additionally enforce the approved static contract.

#include <jaiscript/detail/type_checker.hpp>
#include <jaiscript/detail/foreign_keyword.hpp>
#include <jaiscript/detail/generics.hpp>
#include <jaiscript/detail/less_than.hpp>
#include <filesystem>
#include <jaiscript/detail/inline_variable.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/native_stack.hpp>
#include <jaiscript/detail/body_walker.hpp>
#include <jaiscript/detail/method_value.hpp>
#include <jaiscript/detail/construction.hpp>
#include <jaiscript/detail/map_entry_binding.hpp>
#include <jaiscript/detail/null_member_diagnostic.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/core/class_registry.hpp>
#include <jaiscript/core/script_class.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/function_overloads.hpp>
#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/in_place_result.hpp>
#include <jaiscript/detail/builtin_api_arities.hpp>
#include <jaiscript/detail/arity_diagnostic.hpp>
#include <jaiscript/detail/math_intrinsics.hpp>
#include <jaiscript/detail/numeric_conversion.hpp>
#include <jaiscript/detail/parallel_transform.hpp>
#include <jaiscript/detail/run_texts.hpp>
#include <jaiscript/detail/ref_param_writes.hpp>
#include <jaiscript/detail/const_rules.hpp>
#include <jaiscript/detail/lvalue_shape.hpp>
#include <cctype>
#include <jaiscript/detail/return_flow.hpp>
#include <jaiscript/detail/ref_lvalue.hpp>
#include <jaiscript/detail/cast_kernel.hpp>
#include <jaiscript/detail/constant_arguments.hpp>
#include <jaiscript/detail/format_text.hpp>
#include <jaiscript/detail/regex_library.hpp>
#include <jaiscript/detail/template_library.hpp>

#include <algorithm>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace jai {

std::string check_report::format(std::string_view source, std::string_view script_name) const {
    // Pre-split source lines for the caret display (empty source = no snippet).
    std::vector<std::string_view> lines;
    if (!source.empty()) {
        size_t start = 0;
        while (start <= source.size()) {
            size_t end = source.find('\n', start);
            if (end == std::string_view::npos) {
                lines.push_back(source.substr(start));
                break;
            }
            lines.push_back(source.substr(start, end - start));
            start = end + 1;
        }
    }

    std::string out;
    for (const auto& d : diagnostics) {
        out.append(d.file.empty() ? script_name : std::string_view(d.file));
        out += ':';
        out += std::to_string(d.line);
        out += ':';
        out += std::to_string(d.column);
        out += (d.severity == check_diagnostic::level::error) ? ": error: " : ": warning: ";
        out += d.message;
        if (d.related_line != 0) {
            out += " (declared at " + (d.related_file.empty() ? std::string() : d.related_file + ":") + std::to_string(d.related_line) + ":" + std::to_string(d.related_column) + ")";
        }
        out += '\n';
        for (const auto& note : d.notes) {
            out += "    ";
            out += note;
            out += '\n';
        }
        if (d.file.empty() && d.line >= 1 && d.line <= lines.size()) {
            std::string_view line = lines[d.line - 1];
            out += "    ";
            out.append(line);
            out += "\n    ";
            for (size_t i = 0; i + 1 < d.column && i < line.size(); ++i) {
                out += (line[i] == '\t') ? '\t' : ' ';
            }
            out += "^\n";
        }
    }
    if (capped) {
        out += "(diagnostics capped at " + std::to_string(detail::k_check_diagnostic_cap) + ")\n";
    }
    return out;
}

namespace detail {
namespace {

using diag_level = check_diagnostic::level;

// ------------------------------------------------------------------ ctype

// The checker's static-type lattice. `unknown` is the gradual-typing opt-out
// (var, unresolvable expressions): it flows through everything and never errors.
struct ctype {
    enum kind_t : uint8_t {
        unknown, null_k, int_k, float_k, string_k, char_k, bool_k,
        array_k, map_k, function_k, object_k, weak_k,
        class_ref_k,   // a class used as a value (Class::static access, constructor callee)
        enum_ref_k,    // an enum name (member access checks its values)
        utf8_k,
        math_k,        // a native math kind; cls names it ("math::vec3")
        coroutine_k    // the handle a coroutine call returns
    };
    kind_t kind = unknown;
    std::string_view cls{};   // object_k / class_ref_k / enum_ref_k; int_k: the enum whose value it is (empty: a plain int)
    // Proof pass only: the interned type_info this ctype came from (null = kind only), and
    // whether the kind was read off a runtime value tag rather than a declaration (engine
    // globals) - tentative kinds propagate through operators and are never proof sources.
    const type_info* ti = nullptr;
    bool tentative = false;
    // An element read from a declared, non-tentative typed container (array<int> x; x[i]): the kind is a
    // proof source for the operand class stamp (the typed rows guard it at runtime) while it stays
    // tentative for diagnostics, so no load-time text changes
    bool element_proof = false;
    // tentative, yet the kind is what a namespace variable's declaration spells (or what is read through one): the diagnostics
    // hold the name to it as to a declared local, and no proof rests on it
    bool declared_slot = false;
    // tentative, yet the kind is the result a declaration states: a builtin method's in the registry's catalog, a host function's
    // or a host namespace entry's by its registration, a math value's component. A deduction reads it as it reads a declared
    // type (deduction_type); no proof rests on it and no diagnostic changes with it
    bool declared_result = false;
    // a reference alias of a declared typed container (var& x = G): its element reads may prove while
    // diagnostics still see a reference
    bool alias_proof = false;
    // A map key read through a range-for pair (`kv.first`, C++ `const Key`): copies freely,
    // never mutated or aliased (reference, shared_ptr, T& parameter, T& return, cast<T&>)
    bool const_key = false;
    // The class a reference names (T&, auto& of a known class, a range-for variable over class elements): member diagnostics
    // read it, while the receiver keeps the proof pass's view
    bool diagnostic_only = false;
    // What a flow test proved of a variable declared wider; open: the variable
    // may hold a class the test did not name, so a member the narrowed class lacks is the run time's to answer
    bool narrowed = false;
    bool open = false;
    const expression* origin = nullptr; // this use's expression, for literal-only checks
    const type_info* yields = nullptr;  // coroutine_k: the declared type each yield converts to (null: undeclared)
    // function_k read off a program function's name: the declarations the name holds (a signature slot checks them)
    const std::vector<const function_decl*>* fns = nullptr;
    // a result that is null where its coroutine hands out nothing (next()): typed for diagnostics, and an auto declared with it is a var
    bool nullable_result = false;

    bool is_unknown() const { return kind == unknown; }
    static ctype make(kind_t k) { return ctype{k, {}}; }
    static ctype object(std::string_view name) { return ctype{object_k, name}; }
};

std::string ctype_name(const ctype& t) {
    switch (t.kind) {
        case ctype::null_k: return "null";
        case ctype::int_k: return t.cls.empty() ? "int" : std::string(t.cls);   // an enum's value is an int named by its enum
        case ctype::float_k: return "float";
        case ctype::string_k: return "string";
        case ctype::utf8_k: return "utf8_string";
        case ctype::char_k: return "char";
        case ctype::bool_k: return "bool";
        case ctype::array_k: return t.ti ? t.ti->canonical_name() : "array";
        case ctype::map_k: return t.ti ? t.ti->canonical_name() : "map";
        case ctype::function_k: return t.ti && t.ti->is_signature() ? t.ti->canonical_name() : "function";
        case ctype::weak_k: return t.ti ? t.ti->canonical_name() : "weak_ptr";
        case ctype::object_k:
            if (t.ti && t.ti->base_type == script_value_type::jai_shared_ptr_type) { return t.ti->canonical_name(); }
            return t.cls.empty() ? "object" : std::string(t.cls);
        case ctype::class_ref_k: return "class " + std::string(t.cls);
        case ctype::enum_ref_k: return "enum " + std::string(t.cls);
        case ctype::math_k: return std::string(t.cls);
        case ctype::coroutine_k: return "coroutine<" + (t.yields ? t.yields->canonical_name() : std::string("var")) + ">";
        default: return "unknown";
    }
}

ctype math_ctype(script_value_type kind) {
    return ctype{ctype::math_k, math_type_name(kind)};
}

ctype spelled_ctype_from_type_info(const type_info* ti) {
    if (!ti) {
        return {};
    }
    ctype t;
    switch (ti->base_type) {
        case script_value_type::jai_any_type: return {};
        case script_value_type::jai_null_type: t = ctype::make(ctype::null_k); break;
        case script_value_type::jai_int_type: t = ctype::make(ctype::int_k); break;
        case script_value_type::jai_float_type: t = ctype::make(ctype::float_k); break;
        case script_value_type::jai_string_type: t = ctype::make(ctype::string_k); break;
        case script_value_type::jai_utf8_string_type: t = ctype::make(ctype::utf8_k); break;
        case script_value_type::jai_char_type: t = ctype::make(ctype::char_k); break;
        case script_value_type::jai_bool_type: t = ctype::make(ctype::bool_k); break;
        case script_value_type::jai_array_type: t = ctype::make(ctype::array_k); break;
        case script_value_type::jai_map_type: t = ctype::make(ctype::map_k); break;
        case script_value_type::jai_function_type: t = ctype::make(ctype::function_k); break;
        case script_value_type::jai_weak_ptr_type: t = ctype::make(ctype::weak_k); break;
        case script_value_type::jai_object_type:
        case script_value_type::jai_shared_ptr_type:
            // shared_ptr<T> carries T's name; member access resolves identically (ti keeps
            // the base_type, so the proof pass still tells value object from handle)
            if (ti->type_name.empty()) { return {}; }
            if (ti->is_coroutine) {
                t = ctype::make(ctype::coroutine_k);
                const type_info* yields = ti->type_params.empty() ? nullptr : ti->type_params[0].get();
                t.yields = yields && yields->base_type != script_value_type::jai_any_type ? yields : nullptr;
                break;
            }
            t = ctype::object(ti->type_name);
            break;
        case script_value_type::jai_reference_type:
            return spelled_ctype_from_type_info(ti->type_params.empty() ? nullptr : ti->type_params[0].get());
        case script_value_type::jai_vec2_type:
        case script_value_type::jai_vec3_type:
        case script_value_type::jai_vec4_type:
        case script_value_type::jai_color_type:
        case script_value_type::jai_ivec2_type:
        case script_value_type::jai_ivec3_type:
            t = math_ctype(ti->base_type);
            break;
        default:
            return {};   // cpp_bound, invalid: lenient
    }
    t.ti = ti;
    return t;
}

std::string declared_variable_type_name(const variable_decl* decl) {
    return decl->function_spelling ? "function" : (decl->type ? decl->type->canonical_name() : "auto");
}

bool is_numeric(ctype::kind_t k) { return k == ctype::int_k || k == ctype::float_k; }
// Integral promotion (char_promotion.hpp): char acts as int64 0..255 under arithmetic/bitwise.
bool is_numeric_or_char(ctype::kind_t k) { return is_numeric(k) || k == ctype::char_k; }
// A char beside text compares as its one-character string (char_promotion.hpp char_beside_string)
bool char_beside_text(const ctype& l, const ctype& r) {
    const auto text = [](ctype::kind_t k) { return k == ctype::string_k || k == ctype::utf8_k; };
    return (l.kind == ctype::char_k && text(r.kind)) || (text(l.kind) && r.kind == ctype::char_k);
}
// A char slot takes a string (one character), a string or utf8_string slot a char: the typed-slot table's text rows
bool char_text_slot(const ctype& src, const ctype& dst) {
    return (dst.kind == ctype::char_k && src.kind == ctype::string_k) ||
           (src.kind == ctype::char_k && (dst.kind == ctype::string_k || dst.kind == ctype::utf8_k));
}

// Result type of `l op r` (token base ops). Anything uncertain -> unknown.
ctype binary_result_kind(const ctype& l, token_type op, const ctype& r) {
    switch (op) {
        case token_type::less:
        case token_type::greater:
        case token_type::less_equal:
        case token_type::greater_equal:
        case token_type::equal_equal:
        case token_type::bang_equal:
        case token_type::ampersand_ampersand:
        case token_type::pipe_pipe:
            return ctype::make(ctype::bool_k);   // guaranteed bool (binary_expr::returns_bool)
        case token_type::plus:
            if (l.kind == ctype::utf8_k || r.kind == ctype::utf8_k) {
                return ctype::make(ctype::utf8_k);   // a utf8_string side keeps the kind
            }
            if (l.kind == ctype::string_k || r.kind == ctype::string_k) {
                return ctype::make(ctype::string_k);   // concat, either side
            }
            [[fallthrough]];
        case token_type::minus:
        case token_type::star:
        case token_type::slash:
            if (is_numeric_or_char(l.kind) && is_numeric_or_char(r.kind)) {
                return ctype::make((l.kind == ctype::float_k || r.kind == ctype::float_k)
                                       ? ctype::float_k : ctype::int_k);
            }
            return {};
        case token_type::percent:
            if ((l.kind == ctype::float_k && is_numeric_or_char(r.kind)) || (r.kind == ctype::float_k && is_numeric_or_char(l.kind))) {
                return ctype::make(ctype::float_k);
            }
            [[fallthrough]];
        case token_type::ampersand:
        case token_type::pipe:
        case token_type::caret:
        case token_type::left_shift:
        case token_type::right_shift:
            return ((l.kind == ctype::int_k || l.kind == ctype::char_k) &&
                    (r.kind == ctype::int_k || r.kind == ctype::char_k)) ? ctype::make(ctype::int_k) : ctype{};
        case token_type::spaceship:
            return (is_numeric_or_char(l.kind) && is_numeric_or_char(r.kind)) ? ctype::make(ctype::int_k) : ctype{};
        default:
            return {};
    }
}

ctype binary_result(const ctype& l, token_type op, const ctype& r) {
    ctype out = binary_result_kind(l, op, r);
    out.tentative = l.tentative || r.tentative;
    out.element_proof = (l.tentative || r.tentative) && (!l.tentative || l.element_proof) && (!r.tentative || r.element_proof);
    out.declared_result = out.tentative && !out.is_unknown() && (!l.tentative || l.declared_result) && (!r.tentative || r.declared_result);
    return out;
}

// ------------------------------------------------------------------ checker

// The engine as the walk asks it. Each name asked is one the unit resolved outside itself; a walk that keeps them
// (check_options::outside) hands them back sorted, the names its verdict depends on beyond its own text.
class outside_engine {
public:
    outside_engine(engine& eng, bool recording) : engine_(eng), recording_(recording) {}

    void note(std::string_view name) const {
        if (recording_ && !names_.count(std::string(name))) { names_.emplace(name); }
    }
    engine& asking(std::string_view name) const { note(name); return engine_; }
    std::vector<std::string> take_names() {
        std::vector<std::string> result(names_.begin(), names_.end());
        std::sort(result.begin(), result.end());
        return result;
    }

    bool has_variable(const std::string& name) const { note(name); return engine_.has_variable(name); }
    const std::vector<generic_definition_ptr>* find_generics(const std::string& qualified) const { note(qualified); return engine_access::find_generics(engine_, qualified); }
    bool declares_generics() const { return engine_access::declares_generics(engine_); }
    bool names_generic_method(const std::string& name) const { note(name); return engine_access::names_generic_method(engine_, name); }
    bool generic_instance(const std::string& name, std::string& generic, std::vector<type_info_ptr>& arguments) const { return engine_access::generic_instance(engine_, name, generic, arguments); }
    string_symbolizer& symbols() const { return *engine_access::get_symbolizer(engine_); }
    uint64_t intern(std::string_view text, std::string_view& view) const {
        const auto interned = engine_access::get_symbolizer(engine_)->intern_with_view(text);
        view = interned.second;
        return interned.first;
    }
    bool has_function(const std::string& name) const { note(name); return engine_.has_function(name); }
    bool is_type_name(const std::string& name) const { note(name); return engine_access::is_type_name(engine_, name); }
    // an enum a declaration that ran named (engine::note_enum)
    bool is_enum_name(const std::string& name) const {
        note(name);
        const auto id = engine_access::get_symbolizer(engine_)->find(name);
        return id && engine_access::is_enum_name(engine_, *id);
    }
    bool is_const_global(const std::string& name) const { note(name); return engine_access::is_const_global(engine_, name); }
    // The script class of the object an instance variable of the debugger's evaluation holds now (detail: the receiver of a
    // generic method's instance); empty for any other name or value
    std::string_view evaluation_receiver_class(const std::string& name) const {
        const instance_variables* scope = engine_access::debug_evaluation_scope(engine_);
        const auto held = scope ? scope->find(name) : instance_variables::const_iterator();
        if (!scope || held == scope->end()) { return {}; }
        const script_value& value = held->second.deref();
        if (!value.is_object() || value.is_null()) { return {}; }
        const auto instance = const_cast<script_value&>(value).get_class_instance();
        const class_definition* cls = instance ? instance->get_class_definition() : nullptr;
        return cls && cls->is_script_class() ? std::string_view(cls->get_name()) : std::string_view();
    }
    held_value get_variable(const std::string& name) const { note(name); return engine_.get_variable(name); }
    std::shared_ptr<class_definition> get_class_definition(const std::string& name) const { note(name); return engine_access::get_class_definition(engine_, name); }
    std::shared_ptr<script_class_definition> find_script_class(const std::string& name) const { note(name); return engine_access::get_class_registry(engine_).find_script_class(name); }
    // a generic whose declaration has run in the engine (an earlier run of the unit, another unit): class_registry's linker
    bool generic_declared(const std::string& name) const { note(name); return engine_access::get_class_registry(engine_).declared_generics_.count(name) != 0; }
    std::shared_ptr<class_definition> get_class_definition_by_type(const std::type_index& type) const {
        auto result = engine_access::get_class_definition_by_type(engine_, type);
        if (result) { note(result->get_name()); }
        return result;
    }
    bool host_function_signatures(const std::string& name, std::vector<engine::host_overload_signature>& out) const { note(name); return engine_.host_function_signatures(name, out); }
    uint64_t host_function_writeback(const std::string& name) const { note(name); return engine_access::host_function_writeback(engine_, name); }
    // the script function declarations the engine holds at program scope under a name (an earlier unit's, one or an overload set)
    std::vector<std::shared_ptr<script_defined_function>> held_function_declarations(std::string_view name) const {
        note(name);
        std::vector<std::shared_ptr<script_defined_function>> result;
        const auto id = engine_access::get_symbolizer(engine_)->find(name);
        const auto env = engine_access::get_global_environment(engine_);
        const script_value* held = id && env ? env->get_own_value_ptr(*id) : nullptr;
        const script_callable* callable = held ? detail::script_callable_of(*held, &engine_) : nullptr;
        if (!callable) { return result; }
        if (callable->kind == script_callable::kind_type::function && callable->fn && callable->fn->declared) {
            result.push_back(callable->fn);
        } else if (callable->kind == script_callable::kind_type::overload_set && callable->overloads()) {
            result = callable->overloads()->members;
        }
        return result;
    }
    std::string unit_label(uint32_t unit) const { return engine_access::unit_label(engine_, unit); }
    // the units whose variable declarations made the program-scope cells the engine holds under a name (environment::declaring_units)
    std::vector<uint32_t> variable_units(const std::string& name) const {
        note(name);
        const auto id = engine_access::get_symbolizer(engine_)->find(name);
        const auto env = engine_access::get_global_environment(engine_);
        return id && env ? env->declaring_units(*id) : std::vector<uint32_t>{};
    }
    bool host_function_result(const std::string& name, script_value_type& out) const { note(name); return engine_access::host_function_result(engine_, name, out); }
    bool host_nodiscard(const std::string& callable) const { note(callable); return engine_.is_nodiscard_function(callable); }
    uint8_t type_argument_result(const std::string& callable) const { note(callable); return engine_.type_argument_result(callable); }
    // array<T> and shared_ptr<T> as the engine interns them (a typed host result: jai::returns<jai::array_of<jai::type_arg>>)
    const type_info* array_type_of(const type_info* element) const { return engine_access::get_type_info_array(engine_, const_cast<type_info*>(element)); }
    const type_info* shared_type_of(const type_info* pointee) const { return engine_access::get_type_info_shared_ptr(engine_, const_cast<type_info*>(pointee)); }
    std::vector<std::string> registered_class_names() const { return engine_.registered_class_names(); }
    std::string unbound_instantiation_text(std::string_view name) const { note(name); return detail::unbound_instantiation_text(engine_, name); }
    std::vector<std::pair<size_t, const std::string*>> constant_argument_indexes(const std::string& callable) const { note(callable); return engine_access::constant_argument_indexes(engine_, callable); }
    const std::unordered_map<std::string, std::vector<std::string>>& constant_texts_by_name() const { note(k_outside_constants); return engine_access::constant_texts_by_name(engine_); }
    script_namespace_data* find_namespace(const std::string& path) const {
        note(path);
        const auto id = engine_access::get_symbolizer(engine_)->find(path);
        if (!id) { return nullptr; }
        auto& all = engine_access::script_namespaces(engine_);
        auto it = all.find(*id);
        return it == all.end() ? nullptr : it->second.get();
    }

    uint64_t symbolize(std::string_view text) const { return engine_access::symbolize(engine_, text); }
    string_symbolizer* get_symbolizer() const { return engine_access::get_symbolizer(engine_); }
    std::shared_ptr<environment> get_global_environment() const { return engine_access::get_global_environment(engine_); }
    std::unordered_set<std::string> get_registered_template_types() const { return engine_access::get_registered_template_types(engine_); }
    bool is_template_type(const std::string& base) const { return engine_access::is_template_type(engine_, base); }
    bool has_constant_arguments() const { return engine_access::has_constant_arguments(engine_); }
    bool sleep_callee(const void* fn) const noexcept { return engine_access::sleep_callee(engine_, fn); }
    bool format_literal_callee(const void* fn) const noexcept { return engine_access::format_literal_for(engine_, fn) != nullptr; }
    uint8_t math_alias_fn(uint64_t symbol) const { return engine_access::math_alias_fn(engine_, symbol); }
    const std::unordered_set<uint64_t>& builtin_method_ids(int kind) const { return engine_access::builtin_method_ids(engine_, kind); }
    type_info* get_type_info_object(const std::string& cls) const { return engine_access::get_type_info_object(engine_, cls); }
    type_info* get_type_info_shared_ptr(type_info* pointee) const { return engine_access::get_type_info_shared_ptr(engine_, pointee); }
    type_info* get_type_info_int() const { return engine_access::get_type_info_int(engine_); }
    type_info* get_type_info_float() const { return engine_access::get_type_info_float(engine_); }
    type_info* get_type_info_bool() const { return engine_access::get_type_info_bool(engine_); }
    type_info* get_type_info_char() const { return engine_access::get_type_info_char(engine_); }
    type_info* get_type_info_any() const { return engine_access::get_type_info_any(engine_); }
    type_info* get_type_info_uint8() const { return engine_access::get_type_info_uint8(engine_); }
    type_info* get_type_info_array(type_info* element) const { return engine_access::get_type_info_array(engine_, element); }
    type_info* get_type_info_string() const { return engine_access::get_type_info_string(engine_); }
    type_info* get_type_info_utf8_string() const { return engine_access::get_type_info_utf8_string(engine_); }
    type_info* get_type_info_math(script_value_type kind) const { return engine_access::get_type_info_math(engine_, kind); }
    type_info* type_named(std::string_view canonical_name) const { return engine_access::type_named(engine_, canonical_name); }
    type_info* get_type_info_map(type_info* key, type_info* value) const { return engine_access::get_type_info_map(engine_, key, value); }
    type_info* get_type_info_coroutine(type_info* yields) const { return engine_access::get_type_info_coroutine(engine_, yields); }
    type_info* get_type_info_void() const { return engine_access::get_type_info_void(engine_); }
    type_info* get_type_info_reference(type_info* referent) const { return engine_access::get_type_info_reference(engine_, referent); }
    type_info* get_type_info_untyped_function() const { return engine_access::get_type_info_untyped_function(engine_); }
    type_info* get_type_info_function(type_info* result, const std::vector<type_info*>& arguments) const {
        return engine_access::get_type_info_function(engine_, result, arguments);
    }

private:
    engine& engine_;
    bool recording_;
    mutable std::unordered_set<std::string> names_;
};

// A namespace's members by their qualified names ("ns::f", "a::b::C"), and every path it opens marked with a trailing "::"
// ("a::", "a::b::"), which no identifier spells: what a qualified name may name, whichever unit declares it. A nested block's
// name is already its full path (the parser prefixes the enclosing ones).
// A qualified extern (`extern game::tick;`, or any extern in a namespace) declares its full name and opens every path above it
void collect_qualified_extern(const extern_decl* x, std::unordered_set<std::string>& out) {
    const std::string name(x->name);
    for (size_t cut = name.find("::"); cut != std::string::npos; cut = name.find("::", cut + 2)) { out.emplace(name.substr(0, cut + 2)); }
    out.emplace(name);
}

void collect_namespace_members(const namespace_decl* n, std::unordered_set<std::string>& out) {
    const std::string path(n->name);
    for (size_t cut = path.find("::"); cut != std::string::npos; cut = path.find("::", cut + 2)) { out.emplace(path.substr(0, cut + 2)); }
    out.emplace(path + "::");
    const auto member = [&](std::string_view name) { out.emplace(path + "::" + std::string(name)); };
    for (const auto& d : n->declarations) {
        if (!d) { continue; }
        switch (d->get_type()) {
            case node_type::function_decl: member(static_cast<const function_decl*>(d.get())->name); break;
            case node_type::variable_decl: member(static_cast<const variable_decl*>(d.get())->name); break;
            case node_type::extern_decl: collect_qualified_extern(static_cast<const extern_decl*>(d.get()), out); break;
            case node_type::class_decl: member(static_cast<const class_decl*>(d.get())->name); break;
            case node_type::enum_decl: out.emplace(static_cast<const enum_decl*>(d.get())->name); break;   // already its qualified name
            case node_type::destructuring_decl:
                for (const auto& [name, _] : static_cast<const destructuring_decl*>(d.get())->names) { member(name); }
                break;
            case node_type::namespace_decl: collect_namespace_members(static_cast<const namespace_decl*>(d.get()), out); break;
            default: break;
        }
    }
}

class static_checker {
public:
    static_checker(engine& eng, const std::vector<declaration_ptr>& decls,
                   const std::vector<std::string>* extra_globals, const check_options& opts)
        : eng_(eng, opts.outside != nullptr), decls_(decls), proofs_(opts.proofs), collect_diagnostics_(opts.diagnostics), stats_out_(opts.stats) {
        if (extra_globals) {
            for (const auto& n : *extra_globals) {
                extra_globals_.insert(n);
                instance_names_.insert(n);
            }
        }
        program_names_ = opts.program_names;
        program_classes_ = opts.program_classes;
        program_functions_ = opts.program_functions;
        program_globals_ = opts.program_globals;
        program_enums_ = opts.program_enums;
        deferred_out_ = opts.deferred_classes;
        reload_dependent_ = opts.reload_dependent;
        observer_ = opts.observer;
        outside_out_ = opts.outside;
        generic_requests_ = opts.generic_requests;
        remade_failures_ = opts.remade_failures;
        kept_instances_ = opts.kept_instances;
    }

    check_report run() {
        stack_.arm();
        try {
            collect_top_level();
            for (const auto& d : decls_) {   // the unit's own text (an include spliced into a body keeps its file's places)
                if (unit_file_.empty() && d && !instance_node(*d) && !d->location.filename.empty()) { unit_file_ = d->location.filename; }
            }
            scopes_.emplace_back();
            mode_guard unit(mode_stack_, leading_mode(decls_), /*scoped=*/true);
            warn_duplicate_top_level_functions();   // under the unit's [[warn_mode]] / [[strict_mode]]
            warn_kept_instances();
            lw_open_region();
            for (const auto& d : decls_) {
                walk_listed(d.get());
            }
            lw_close_region(nullptr, &decls_, nullptr);
            scopes_.pop_back();
            warn_handle_shadowed_builtins();
        } catch (const nesting_exhausted& e) {
            // an error under [[warn_mode]] and with diagnostics off as well: the walk stopped, so nothing past it was checked
            diags_.push_back({diag_level::error, e.location().line, e.location().column, std::string(k_nesting_too_deep_listing)});
        }

        if (stats_out_) { counts_.deferred = deferred_ ? 1 : 0; *stats_out_ = counts_; }
        if (outside_out_) { *outside_out_ = eng_.take_names(); }
        check_report report;
        report.capped = capped_;
        report.diagnostics = std::move(diags_);
        std::stable_sort(report.diagnostics.begin(), report.diagnostics.end(),
            [](const check_diagnostic& a, const check_diagnostic& b) {
                if (a.line != b.line) { return a.line < b.line; }
                return a.column < b.column;
            });
        return report;
    }

    // what each site's head names, by the lookup a bare or a qualified name takes in this walk
    std::string less_answers(const std::vector<std::string>& queries) {
        stack_.arm();
        std::string answers;
        try {
            collect_top_level();
            for (const auto& query : queries) { answers.push_back(less_answer(less_query::parse(query))); }
        } catch (const nesting_exhausted&) {
            answers.resize(queries.size(), k_less_nothing);
        }
        return answers;
    }

private:
    char less_answer(const less_query& q) {
        switch (q.form) {
            case less_head::member:
                return generic_method_names_.count(q.head) || eng_.names_generic_method(q.head) ||
                       engine_access::type_argument_method(eng_.asking(q.head), q.head) ? k_less_declared : k_less_nothing;
            case less_head::qualified:
                return less_qualified(q);
            case less_head::bare:
                if (const char kind = less_class_member(q.cls, q.head, 0)) { return kind; }
                for (std::string space = q.space; !space.empty();) {
                    if (const char kind = less_namespace_member(space, q.head)) { return kind; }
                    const size_t cut = space.rfind("::");
                    if (cut == std::string::npos) { break; }
                    space.resize(cut);
                }
                return less_program_name(q.head);
            default:
                return less_program_name(q.head);
        }
    }
    static bool less_listed(const std::vector<std::string_view>& names, std::string_view name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    }
    // A member of a registered class: a field, a static or a property is a variable, a method is not
    char less_engine_member(const class_definition& def, std::string_view name, int depth) {
        const uint64_t id = eng_.symbolize(name);
        const uint64_t getter = eng_.symbolize("_get_" + std::string(name));
        if (def.is_static_field(id) || def.has_static_field(id) || def.find_field_declaring_class(id) != nullptr ||
            def.method_ptr(getter) != nullptr || def.has_static_method(getter)) { return k_less_variable; }
        if (def.method_ptr(id) != nullptr || def.has_static_method(id)) { return k_less_declared; }
        if (depth > 32) { return 0; }
        for (const auto& parent : def.get_parent_classes()) {
            if (parent) { if (const char kind = less_engine_member(*parent, name, depth + 1)) { return kind; } }
        }
        return 0;
    }
    // A class's member of that name, its own before each base's; 0 when the class or the member is not known
    char less_class_member(std::string_view cls, std::string_view name, int depth) {
        if (cls.empty() || depth > 32) { return 0; }
        if (!generics_named(std::string(cls) + "::" + std::string(name)).empty()) { return k_less_declared; }
        if (const sclass* c = find_sclass(cls)) {
            if (c->fields.count(name) || c->static_fields.count(name)) { return k_less_variable; }
            if (c->methods.count(name) || c->static_methods.count(name)) { return k_less_declared; }
            for (auto base : c->decl->base_classes) {
                if (const char kind = less_class_member(base, name, depth + 1)) { return kind; }
            }
            return 0;
        }
        // a generic class, or an instance the unit has not made yet: the members its declaration lists
        const std::string_view generic = cls.substr(0, cls.find('<'));
        if (const generic_definition_ptr g = generic_class_named(generic)) {
            if (less_listed(g->member_variables, name)) { return k_less_variable; }
            if (less_listed(g->member_functions, name)) { return k_less_declared; }
            for (const std::string& base : g->bases) {
                if (const char kind = less_class_member(base, name, depth + 1)) { return kind; }
            }
            return 0;
        }
        if (auto def = engine_class(cls)) { return less_engine_member(*def, name, 0); }
        return 0;
    }
    template <typename Pool>
    static bool less_pooled(const Pool* pool, const std::string& name) {
        if (!pool) { return false; }
        const auto held = pool->find(name);
        return held != pool->end() && !held->second.empty();
    }
    char less_namespace_member(const std::string& space, std::string_view name) {
        const std::string qualified = space + "::" + std::string(name);
        if (namespace_vars_.count(qualified)) { return k_less_variable; }
        if (namespace_fns_.count(qualified) || !generics_named(qualified).empty() || classes_.count(qualified) || enums_.count(qualified)) { return k_less_declared; }
        if (less_pooled(program_globals_, qualified)) { return k_less_variable; }
        if (less_pooled(program_functions_, qualified) || less_pooled(program_enums_, qualified) || (program_classes_ && program_classes_->count(qualified))) { return k_less_declared; }
        if (namespace_members_.count(qualified + "::") || extra_global(qualified + "::")) { return k_less_declared; }   // a nested namespace
        if (engine_class(qualified) || eng_.is_enum_name(qualified) || eng_.has_function(qualified)) { return k_less_declared; }
        if (eng_.has_variable(qualified)) { return k_less_variable; }
        eng_.note(qualified);
        if (const script_namespace_data* ns = eng_.find_namespace(space)) {
            const uint64_t id = eng_.symbolize(name);
            if (ns->functions.count(id) || ns->classes.count(id) || ns->enums.count(id) || ns->signatures.count(id)) { return k_less_declared; }
            if (ns->variables.count(id) || (ns->scope && ns->scope->get_own_value_ptr(id) != nullptr)) { return k_less_variable; }
        }
        if (namespace_members_.count(qualified) || extra_global(qualified)) { return k_less_variable; }   // an extern, a destructured name
        return 0;
    }
    // A program-scope name: this unit's, then the pooled program's, then what the engine holds
    char less_program_name(std::string_view name) {
        const std::string spelled(name);
        if (global_var_names_.count(name)) { return k_less_variable; }
        if (functions_.count(name) || classes_.count(name) || enums_.count(name) || generics_.count(spelled) || namespace_members_.count(spelled + "::")) { return k_less_declared; }
        if (less_pooled(program_globals_, spelled)) { return k_less_variable; }
        if (less_pooled(program_functions_, spelled) || less_pooled(program_enums_, spelled) || (program_classes_ && program_classes_->count(spelled))) { return k_less_declared; }
        if (const auto* held = eng_.find_generics(spelled); held && !held->empty()) { return k_less_declared; }
        if (builtin_type_name(name) || engine_access::get_registered_template_types(eng_.asking(name)).count(spelled) || engine_class(name) ||
            eng_.is_enum_name(spelled) || eng_.has_function(spelled) || extra_global(spelled + "::") || eng_.find_namespace(spelled)) { return k_less_declared; }
        if (eng_.has_variable(spelled) || extra_global(spelled)) { return k_less_variable; }
        return k_less_nothing;
    }
    // A::B::x: A::B as a namespace, a class or an enum, found from the enclosing namespace outward, and x in it
    char less_qualified(const less_query& q) {
        const size_t cut = q.head.rfind("::");
        if (cut == std::string::npos) { return less_program_name(q.head); }
        const std::string prefix = q.head.substr(0, cut);
        const std::string_view name = std::string_view(q.head).substr(cut + 2);
        std::vector<std::string> owners;
        for (std::string space = q.space; !space.empty();) {
            owners.push_back(space + "::" + prefix);
            const size_t up = space.rfind("::");
            if (up == std::string::npos) { break; }
            space.resize(up);
        }
        owners.push_back(prefix);
        for (const std::string& owner : owners) {
            if (const char kind = less_class_member(owner, name, 0)) { return kind; }
            if (enums_.count(owner) || less_pooled(program_enums_, owner) || eng_.is_enum_name(owner)) { return k_less_variable; }
            if (const char kind = less_namespace_member(owner, name)) { return kind; }
        }
        return k_less_nothing;
    }

    // -------------------------------------------------------------- tables

    struct sclass {
        const class_decl* decl = nullptr;
        std::unordered_map<std::string_view, const variable_decl*> fields;
        std::unordered_map<std::string_view, const variable_decl*> static_fields;
        std::unordered_map<std::string_view, std::vector<const function_decl*>> methods;
        std::unordered_map<std::string_view, std::vector<const function_decl*>> static_methods;
        std::vector<const function_decl*> ctors;
        std::vector<const function_decl*> private_ctors;   // the ctors declared under private: / protected:
        std::unordered_map<std::string_view, uint8_t> nonpublic;   // private: / protected: members (member_hit::access)
        // Declared by another closure unit (program table): its locations belong to that unit's
        // source, and its member types seed proofs only once the engine has registered it.
        bool foreign = false;
        bool provable = true;
    };

    outside_engine eng_;
    std::vector<std::string>* outside_out_ = nullptr;
    const std::vector<declaration_ptr>& decls_;

    // Proof pass: independent of diagnostics - a stamp never
    // depends on a diag() having fired, and diag() is a no-op when diagnostics are off.
    using proof_mode = check_options::proofs_t;
    proof_mode proofs_ = proof_mode::stamp;
    bool collect_diagnostics_ = true;
    proof_stats* stats_out_ = nullptr;
    proof_stats counts_{};
    check_observer* observer_ = nullptr;

    std::vector<check_diagnostic> diags_;
    std::unordered_set<std::string> seen_;   // "file:line:col:message" dedupe keys
    int template_files_ = 0;   // inside a template(...) body: diagnostics name the template's file (check_diagnostic::file)
    bool capped_ = false;
    bool has_includes_ = false;           // include/import present (any form): proof gates stay shut
    bool has_dynamic_includes_ = false;   // a path the closure cannot read: undeclared identifiers downgrade to warnings
    // [[warn_mode]] / [[strict_mode]] regions, innermost on top. A statement-level frame stops
    // at nested function/lambda bodies (they re-push the nearest scoped mode); a block/unit
    // frame flows into them.
    struct mode_frame { uint8_t mode; bool scoped; };
    std::vector<mode_frame> mode_stack_;
    struct mode_guard {
        std::vector<mode_frame>& stack;
        bool pushed;
        mode_guard(std::vector<mode_frame>& s, uint8_t mode, bool scoped, bool always = false)
            : stack(s), pushed(always || mode != check_mode_attr_none) {
            if (pushed) { stack.push_back({mode, scoped}); }
        }
        ~mode_guard() { if (pushed) { stack.pop_back(); } }
    };
    uint8_t region_mode() const { return mode_stack_.empty() ? check_mode_attr_none : mode_stack_.back().mode; }
    uint8_t scoped_region_mode() const {
        for (auto it = mode_stack_.rbegin(); it != mode_stack_.rend(); ++it) {
            if (it->scoped) { return it->mode; }
        }
        return check_mode_attr_none;
    }
    // the mode attribute of the first declaration of the text itself (a unit's instances and their requests stand ahead of it)
    static uint8_t leading_mode(const std::vector<declaration_ptr>& decls) {
        for (const auto& d : decls) {
            if (!d) { return check_mode_attr_none; }
            if (!instance_node(*d)) { return d->check_mode_attr; }
        }
        return check_mode_attr_none;
    }
    static bool instance_node(const declaration& d) {
        switch (d.get_type()) {
            case node_type::generic_instance_decl: return true;
            case node_type::function_decl: return static_cast<const function_decl&>(d).instance_stamp != 0;
            case node_type::class_decl: return static_cast<const class_decl&>(d).instance_stamp != 0 || static_cast<const class_decl&>(d).method_instances;
            case node_type::namespace_decl: {
                const auto& members = static_cast<const namespace_decl&>(d).declarations;
                return !members.empty() && std::all_of(members.begin(), members.end(), [](const declaration_ptr& m) { return m && instance_node(*m); });
            }
            default: return false;
        }
    }

    std::unordered_map<std::string_view, sclass> classes_;
    // Program-closure classes, modeled on first use from the engine's table (a same-unit class
    // always wins over its table entry).
    const program_class_map* program_classes_ = nullptr;
    const program_function_map* program_functions_ = nullptr;
    const program_global_map* program_globals_ = nullptr;
    struct top_var_decl { const variable_decl* decl = nullptr; size_t count = 0; };
    std::unordered_map<std::string_view, top_var_decl> top_var_decls_;   // the unit's program-scope variables, ahead of the walk that meets them
    const program_enum_map* program_enums_ = nullptr;
    bool deferred_ = false;   // a program-table class met before the engine defined it: the unit's proofs re-stamp after the next import
    std::vector<std::string>* deferred_out_ = nullptr;   // their names, for the engine's re-stamp test
    std::unordered_map<std::string_view, sclass> program_sclasses_;
    std::unordered_map<std::string_view, std::vector<const function_decl*>> functions_;
    // A name's program-scope declarations outside this unit that a call's overload set also holds (foreign_overloads), and the
    // declarations the checker spells for the engine's held functions
    std::unordered_map<std::string_view, std::vector<const function_decl*>> foreign_overloads_;
    std::vector<std::unique_ptr<function_decl>> held_declarations_;
    std::unordered_map<std::string_view, const enum_decl*> enums_;
    std::unordered_set<std::string_view> namespaces_;
    std::unordered_set<std::string> namespace_members_;   // this unit's namespace members and paths (collect_namespace_members)
    std::unordered_map<std::string, const variable_decl*> namespace_vars_;   // "path::name" -> its declaration (null: two spellings)
    std::unordered_map<std::string, ctype> namespace_inferred_types_;   // "path::name" -> what the walk inferred for an auto namespace variable it has passed
    std::unordered_map<std::string, std::vector<const function_decl*>> namespace_fns_;   // "path::name" -> the unit's declarations of it
    // [[nodiscard]] (docs/site/guide/06-functions.html): what resolving the call being inferred found, and every such call the walk met;
    // twin names the in-place method a builtin namespace copy pairs with (empty for everything else)
    struct nodiscard_callee { std::string name; std::string twin; };
    nodiscard_callee nodiscard_hit_;
    std::unordered_map<const call_expr*, nodiscard_callee> nodiscard_calls_;
    // A callee declared to return nothing (docs/site/guide/06-functions.html#returning-nothing) that resolving the call being
    // inferred found: its name as the error quotes it ('f', 'C.m', or the callee), and for a method beside a copying static of
    // its name the twin text (detail::in_place_result_text) and the call's member
    // reason: for a callee whose deduced result is nothing because it returns a builtin in-place call, that call's text (it names
    // the copying twin)
    struct void_callee { std::string shown; std::string twin; const member_expr* member = nullptr; std::string reason; bool typed_slot = false; };
    void_callee void_hit_;
    // the calls in a place that discards the result (a statement, a return in a body that declares no value) whose callee returns
    // nothing: the error a use of the result gives, and the reason a body that returns the call passes on
    struct nothing_call { std::string used; std::string reason; };
    std::unordered_map<const call_expr*, nothing_call> nothing_calls_;
    // the bodies with a deduced result (an auto function or method, a lambda with no result type) whose returns all return nothing,
    // with the reason they pass on: callees that return nothing
    std::unordered_map<const lambda_expr*, std::string> nothing_lambdas_;
    std::unordered_map<const function_decl*, std::string> nothing_functions_;
    // A call with type arguments (call_expr::type_argument_count) being resolved: the script function it resolved to (an error:
    // only a host takes a type argument), else the host callable whose registration may type its result
    bool type_argument_call_ = false;
    std::vector<std::pair<type_info_ptr, source_location>>* unknown_type_arguments_ = nullptr;   // the call's type arguments that name no type, while they are inferred
    std::string type_argument_script_callee_;
    std::string_view type_argument_script_kind_;   // what it is: a function, a method, a class, a lambda
    std::string type_argument_host_callee_;
    // Every name the program scope declares (top level and namespace bodies, any order): a function reads a global declared
    // after it, so "undeclared" fires for names absent from this set AND the engine surface. A name a block, function, lambda
    // or class declares is visible only in its own scope, which the walk's scopes and member lookups resolve.
    std::unordered_set<std::string_view> all_names_;
    int collect_depth_ = 0;   // collect_names: 0 at program scope
    native_stack_mark stack_;
    bool collecting_namespace_names_ = false;
    void collect_program_name(std::string_view name) {
        if (collect_depth_ == 0 && !collecting_namespace_names_) { all_names_.insert(name); }   // a namespace's names: enclosing_namespace_member
    }
    std::unordered_map<std::string_view, source_location> program_decls_;   // this unit's program-scope variables, functions and classes: each name's first declaration
    std::unordered_map<std::string, source_location> namespace_decls_;   // this unit's namespace functions and variables by qualified name: each one's first declaration
    void note_program_decl(std::string_view name, const source_location& at) {
        if (collect_depth_ == 0 && !collecting_namespace_names_) { program_decls_.emplace(name, at); }
    }
    bool program_or_derived_name(std::string_view name) {
        return all_names_.count(name) || !enclosing_namespace_member(name).empty() || derived_class_member(name);
    }
    // names an assignment expression or a reference declaration targets, and each variable declaration's binary::layout(...)
    // initializer (null for any other initializer): a layout variable declared once and never assigned is statically known
    std::unordered_set<std::string_view> assigned_names_;
    // the names a call may rebind: an identifier or a namespace path passed as an argument to anything but the library's own
    // value-taking forms (array::, map::, parallel_transform, parallel_reduce), and a namespace path assigned (a name is a known
    // callee only while nothing can rebind it)
    std::unordered_set<std::string_view> passed_names_;
    std::unordered_set<std::string> rebindable_paths_;
    std::unordered_map<std::string_view, std::vector<const expression*>> layout_inits_;
    std::unordered_set<std::string> extra_globals_;
    const std::unordered_set<std::string>* program_names_ = nullptr;   // the program's names, consulted beside extra_globals_ (never copied per check)
    bool extra_global(const std::string& name) const {
        if (extra_globals_.count(name)) { return true; }
        eng_.note(name);
        return program_names_ && program_names_->count(name);
    }
    std::unordered_set<std::string> instance_names_;   // the host's per-call instance variables (a subset of extra_globals_)
    // Top-level (and namespace-flattened) variable names. Runtime resolution reads the
    // scope chain THROUGH the globals before falling back to this-fields, so a name that
    // is both a global and a field is flow-order-ambiguous to a static walk: member hits
    // on these names degrade to unknown instead of trusting the field's declared type.
    std::unordered_set<std::string_view> global_var_names_;
    // Classes this unit declares that the engine already holds with a different field set:
    // a hot-reload redefinition. Instances made under the old shape are live while the unit
    // runs (migrate hooks read retired fields, probes expect the runtime error), so member
    // misses on those receivers stay silent (stale_shape_receiver).
    std::unordered_set<std::string_view> open_classes_;
    bool reload_dependent_ = false;   // check_options::reload_dependent: no class is open
    // extend(x, Class) re-classes x IN PLACE: every name passed to extend anywhere in the unit
    // reads as unknown from then on (flow-insensitive, by name).
    std::unordered_set<std::string_view> extended_names_;
    std::unordered_set<std::string_view> extended_members_;

    // Mixed-branch ternary stash: infer()'s ternary case records the two known-but-
    // different branch types so walk_variable_decl can warn when an auto decl would
    // lock its type from whichever branch RUNTIME takes. Stashed rather than re-inferred
    // at the decl site - re-running infer() on the arms would duplicate sub-diagnostics.
    struct mixed_ternary_note {
        const expression* node = nullptr;
        ctype then_type{};
        ctype else_type{};
    } last_mixed_ternary_;
    std::unordered_map<const expression*, mixed_ternary_note> mixed_ternaries_;   // every mixed conditional, for the reference bindings

    // A reference bound to a conditional names the arm the condition picks; arms of two types make a converted temporary in C++,
    // which a non-const reference cannot bind
    std::unordered_map<const call_expr*, ctype> call_results_;   // what each call the walk inferred yields
    std::unordered_set<const function_decl*> auto_value_returns_;   // auto functions whose every return yields a value (names_a_value)
    // A value the checker knows, which names no storage once a call returns it: not a handle, a function or a coroutine
    static bool names_a_value(const ctype& r) {
        return !r.is_unknown() && r.kind != ctype::function_k && r.kind != ctype::coroutine_k && r.kind != ctype::null_k &&
               !(r.ti && handle_type(r.ti)) && (r.kind != ctype::object_k || !r.cls.empty());
    }
    // every function a call names is an auto one whose returns all yield values
    bool call_returns_auto_value(const call_expr* c) {
        if (!c->callee || c->callee->get_type() != node_type::identifier_expr) { return false; }
        auto it = functions_.find(static_cast<const identifier_expr*>(c->callee.get())->name);
        if (it == functions_.end() || it->second.empty()) { return false; }
        for (const auto* fn : it->second) { if (!auto_value_returns_.count(fn)) { return false; } }
        return true;
    }
    // call_returns_by_value, also for a program function named at program scope (its name is a variable holding the function)
    bool call_certainly_by_value(const call_expr* c) {
        if (call_returns_by_value(c)) { return true; }
        if (!c->callee || c->callee->get_type() != node_type::identifier_expr || class_ctx_) { return false; }
        const auto* id = static_cast<const identifier_expr*>(c->callee.get());
        if (id->global_qualified) { return false; }
        const var_entry* v = find_local(id->name);
        if (!v || v->type.kind != ctype::function_k || !v->type.fns || v->type.fns->empty()) { return false; }
        for (const auto* fn : *v->type.fns) {
            if (fn->return_type && fn->return_type->is_reference()) { return false; }
        }
        return true;
    }
    // Whether every function a call may reach returns a handle (shared_ptr<T>): its result names an object, as C++'s *make() does
    bool call_returns_handle(const call_expr* c) {
        if (!c->callee || c->callee->get_type() != node_type::identifier_expr) { return false; }
        auto it = functions_.find(static_cast<const identifier_expr*>(c->callee.get())->name);
        if (it == functions_.end() || it->second.empty()) { return false; }
        for (const auto* fn : it->second) {
            if (!fn->return_type || fn->return_type->base_type != script_value_type::jai_shared_ptr_type) { return false; }
        }
        return true;
    }

    // An expression the checker knows names no storage: a literal, an operator's result, a construction or a call that returns
    // a value (a handle names its object, so a handle result is not one)
    bool certainly_temporary(const expression* e) {
        if (!e) { return false; }
        switch (e->get_type()) {
            case node_type::literal_expr: case node_type::array_literal_expr: case node_type::map_literal_expr:
                return true;
            case node_type::call_expr: {
                const auto* c = static_cast<const call_expr*>(e);
                if (!call_certainly_by_value(c) || call_returns_handle(c)) { return false; }
                const auto found = call_results_.find(c);
                if (found == call_results_.end()) { return false; }
                return found->second.is_unknown() ? call_returns_auto_value(c) : names_a_value(found->second);
            }
            case node_type::binary_expr: {
                const auto* b = static_cast<const binary_expr*>(e);
                return b->op.type != token_type::left_bracket && (b->op.type == token_type::plus || b->op.type == token_type::minus ||
                       b->op.type == token_type::star || b->op.type == token_type::slash || b->op.type == token_type::percent) &&
                       certainly_temporary_operand(b->left.get()) && certainly_temporary_operand(b->right.get());
            }
            default:
                return false;
        }
    }
    // an operand of a builtin arithmetic operator: a literal or a variable the checker knows holds a number (a class
    // operator may return a reference)
    bool certainly_temporary_operand(const expression* e) {
        if (e->get_type() == node_type::literal_expr) { return true; }
        if (e->get_type() != node_type::identifier_expr) { return false; }
        const var_entry* v = find_local(static_cast<const identifier_expr*>(e)->name);
        return v && !v->dynamic && !v->is_reference && !v->type.tentative && (v->type.kind == ctype::int_k || v->type.kind == ctype::float_k);
    }
    // A chain of member reads and subscripts whose base is a temporary: the element or field of an object that dies with
    // its statement
    bool rooted_in_temporary(const expression* e) {
        bool stepped = false;
        while (e) {
            if (e->get_type() == node_type::member_expr) {
                const auto* m = static_cast<const member_expr*>(e);
                if (m->is_static || m->null_safe || m->is_arrow) { return false; }
                e = m->object.get();
            } else if (e->get_type() == node_type::binary_expr && static_cast<const binary_expr*>(e)->op.type == token_type::left_bracket) {
                e = static_cast<const binary_expr*>(e)->left.get();
            } else {
                return stepped && certainly_temporary(e);
            }
            stepped = true;
        }
        return false;
    }

    void report_mixed_reference_conditional(const expression* e) {
        if (!detail::conditional_names_storage(e)) { return; }
        auto found = mixed_ternaries_.find(e);
        if (found == mixed_ternaries_.end()) { return; }
        diag(diag_level::error, e->location, "cannot bind a reference to a conditional of '" + ctype_name(found->second.then_type) + "' and '" +
             ctype_name(found->second.else_type) + "': its arms must have one type");
    }

    struct var_entry {
        ctype type;
        bool dynamic = false;             // var (or untyped) — assignments never checked
        bool is_reference = false;        // T&/auto&/var& decl, & parameter, auto& loop variable
        bool is_handle = false;           // declared shared_ptr<T> / shared_ptr<auto>
        bool map_pair = false;            // range-for variable over a map: .first is the read-only key
        ctype alias;                      // a reference alias of a declared typed container: the type its element reads may prove
        ctype::kind_t builtin_alias = ctype::unknown;   // a range-for reference over a typed array's elements: the element kind, for builtin receivers only
        const type_info* declared = nullptr;   // for message text; null for auto-inferred
        std::string display;              // declared-type text for messages
        size_t line = 0, column = 0;
        const lambda_expr* lambda = nullptr;   // a frame-slot local declared with a lambda literal (its calls' graft declaration)
        const lambda_expr* declared_lambda = nullptr;   // any local declared with a lambda literal, which a call through it runs unless it is assigned
        const enum_decl* enum_def = nullptr;   // an enum declared in a function or block: local to that scope
        bool is_const = false;            // const T x / const T& p / a const reference (auto& over a const)
        ctype referent;                   // the class (or typed container) a reference or an auto range-for variable holds: member diagnostics only
        bool has_const_text = false;      // a const declared with a literal text (a + of them): a regex pattern the load can check
        std::string const_text;
        int lw = -1;                      // its lost-write candidate (lw_cands_), -1 for none
        bool lw_handle = false;           // a class-typed slot that may hold a handle (an auto from one, a by-value class parameter)
        parameter* reference_parameter = nullptr;   // a reference parameter's own declaration
        bool placeholders = false;        // this scope declares '_' more than once: naming it is ambiguous (C++26 [basic.scope.scope])
        const type_info* const_element = nullptr;   // a const range-for variable or binding over value elements of a known class: that class
        const type_info* pair_value = nullptr;      // a range-for map entry: the map's declared value type
        bool shared_name = false;         // an extern's or an inline declaration's name: the unit's top level may declare it again
        ctype::kind_t literal_elem = ctype::unknown;   // an auto declared with a uniform scalar array literal: the element kind the run types the array by
        const void* narrow_key = nullptr;   // a local's, parameter's or loop variable's own declaration: the variable a flow test may narrow
        bool capture_copy = false;          // a lambda's named by-value capture: its own snapshot of the variable
        bool capture_alias = false;         // a lambda's named by-reference capture: the enclosing frame's storage
        bool namespace_function = false;    // a namespace's function in its body's scope: the namespace's member (enclosing_namespace_member), never a local
    };

    // What a call's arguments may bind to, for flow narrowing: a reference parameter may let the callee write the variable
    struct narrow_callee {
        bool copies = false;         // a library builtin: every argument is taken as a value
        std::string_view host;       // a free function the engine holds under this name
        bool resolved = false;       // a namespace entry: its declarations and its host write-back mask say it all
        uint64_t writeback = 0;
        std::vector<const function_decl*> declarations;
    };

    // A reference's (or an auto loop variable's) known class or typed container, for member diagnostics alone
    static ctype referent_type(const ctype& t) {
        const bool known_class = t.kind == ctype::object_k && !t.cls.empty();
        const bool typed_container = (t.kind == ctype::array_k || t.kind == ctype::map_k) && t.ti;
        if (!known_class && !typed_container) { return {}; }
        ctype result = t;
        result.tentative = true;
        result.diagnostic_only = true;
        result.element_proof = false;
        result.alias_proof = false;
        result.const_key = false;
        result.origin = nullptr;
        return result;
    }
    std::vector<std::unordered_map<std::string_view, var_entry>> scopes_;
    std::vector<size_t> namespace_scopes_;   // indices into scopes_ that are namespace bodies (their enums are the namespace's)
    std::vector<std::string_view> namespace_walk_names_;   // the namespace each of those bodies declares into
    std::unordered_map<std::string, var_entry> qualified_externs_;   // "ns::x" -> what its extern declares, from the extern on

    const enum_decl* find_enum(std::string_view name) {
        for (size_t i = scopes_.size(); i-- > 0;) {
            auto found = scopes_[i].find(name);
            if (found != scopes_[i].end() && found->second.enum_def) { return found->second.enum_def; }
        }
        auto it = enums_.find(name);
        return it != enums_.end() ? it->second : nullptr;
    }

    // An enum declared in a function or a block is local to that scope at run time (both backends): when the program
    // scope already names it, the declaration leaves that enum unchanged - say so.
    void warn_scoped_enum(const enum_decl* e) {
        const std::string name(e->name);
        auto top = enums_.find(e->name);
        if (top == enums_.end() && !extra_global(name) && !eng_.has_variable(name)) { return; }
        std::string where = "a block";
        if (!fn_stack_.empty()) {
            where = fn_stack_.back().name.empty() || fn_stack_.back().name == "<lambda>" ? std::string("a lambda") : "function '" + std::string(fn_stack_.back().name) + "'";
        }
        diag(diag_level::warning, e->location, "enum '" + name + "' declared in " + where +
             " is local to it: the program-scope '" + name + "' keeps its values",
             top != enums_.end() ? top->second->location.line : 0, top != enums_.end() ? top->second->location.column : 0);
    }

    // An include inside a function body is the function's own text, so a function it declares is a nested one: when the
    // program scope names it too, the include hides that function for the body and replaces nothing - the likely mistake
    void warn_spliced_function(const function_decl* fn) {
        if (fn_stack_.empty() || !fn_stack_.back().file || *fn_stack_.back().file == fn->location.filename) { return; }
        const std::string name(fn->name);
        auto top = functions_.find(fn->name);
        eng_.note(name);
        const bool program_has = top != functions_.end() || (program_functions_ && program_functions_->count(name)) || eng_.has_variable(name);
        if (!program_has) { return; }
        const std::string where = fn_stack_.back().name == "lambda" ? std::string("a lambda") : "function '" + std::string(fn_stack_.back().name) + "'";
        // the listing renders this unit's text: the warning stands at the enclosing body and names the included file's place
        source_location at = fn->location;
        at.line = fn_stack_.back().line;
        at.column = fn_stack_.back().column;
        diag(diag_level::warning, at, fn->location.to_string() + ": function '" + name + "' from the include in " + where +
             " is local to it and hides the program-scope '" + name + "' there without replacing it; import the file to replace program-scope functions",
             top != functions_.end() && !top->second.empty() ? top->second.front()->location.line : 0,
             top != functions_.end() && !top->second.empty() ? top->second.front()->location.column : 0);
    }

    // Live lambda bodies: a name declared below base is the lambda's own; above it the capture list must admit it
    struct capture_ctx {
        size_t base = 0;
        const lambda_expr* lambda = nullptr;
    };
    std::vector<capture_ctx> capture_stack_;

    struct fn_ctx {
        const type_info* ret = nullptr;   // null = auto
        bool is_coroutine = false;
        std::string_view name;
        size_t line = 0, column = 0;
        // Bare names in this body can reach the enclosing class's fields (method/ctor,
        // or [this]-capturing lambda). Plain [=]/[&] lambdas cannot - no field-shadow
        // warnings inside them.
        bool sees_fields = false;
        bool is_static_method = false;
        bool in_ctor_initializers = false;   // : super(...)/this(...) args run before fields exist
        // Names this body resolved through the global tier so far (shadow shape D:
        // "declaration shadows a global used earlier in the same scope chain").
        std::unordered_set<std::string_view> globals_used;
        uint8_t inferred_return = 0;      // auto: the kind every return so far certainly yields (1 int, 2 float, 3 bool)
        bool inferred_mixed = false;      // auto: a return of another kind, of an uncertain kind, or without a value
        bool returns_seen = false;        // auto: a return with a value was walked
        bool returns_values = true;       // auto: every return yields a value the checker knows (no handle, no reference, no function)
        // auto, where the program declares a generic: the one type every return so far yields, for a deduction through a lambda's
        // result (lambda_return_types_); mixed for two types or one the load cannot name
        const type_info* returned = nullptr;
        bool returned_mixed = false;
        // the same with the results declarations state read (ctype::declared_result): what a deduction reads
        const type_info* stated = nullptr;
        bool stated_mixed = false;
        bool returns_nothing = false;
        bool gives_value = false;   // a return gave a value
        bool dispatched = false;    // a non-static method with a deduced result: its calls dispatch, so its return is checked instead
        std::vector<const call_expr*> nothing_returns;   // returns of calls that return nothing (an error once another return gives a value)
        const std::string* file = nullptr;   // the file the body is written in (a declaration from another one was spliced by an include)
    };
    std::vector<fn_ctx> fn_stack_;
    sclass* class_ctx_ = nullptr;
    std::string_view class_ctx_name_{};
    enum class cdtor_ctx : uint8_t { none, construction, destruction };
    cdtor_ctx cdtor_ctx_ = cdtor_ctx::none;   // code the class's constructor, field initializers or destructor run (not a lambda they create)
    // C++ [class.base.init]: fields initialize in declaration order, so an instance field's initializer (not a lambda it creates)
    // reads its own field and the class's later ones before their initialization
    struct initializing_field { std::string_view name; std::unordered_map<std::string_view, const variable_decl*> unready; };
    const initializing_field* initializing_field_ = nullptr;
    int parallel_body_depth_ = 0;   // a parallel_for body, or a lambda written as a parallel_transform/parallel_reduce argument (not combine)

    bool core_sleep_bound() const {
        const script_value bound = eng_.get_variable("sleep");
        return bound.is_function() && eng_.sleep_callee(&bound.unchecked_as_function());
    }
    // format(text, values...) over a text the load can see is not a literal: as in C++20, only a constant is a format string
    void check_format_text(const call_expr* c, const std::vector<ctype>& args) {
        if (c->arguments.size() < 2 || c->literal_format_call() || args.empty() || (args[0].kind != ctype::string_k && args[0].kind != ctype::utf8_k)) { return; }
        const script_value bound = eng_.get_variable("format");
        if (!bound.is_function() || !eng_.format_literal_callee(&bound.unchecked_as_function())) { return; }
        diag(diag_level::error, c->arguments[0]->location,
             "the format text of 'format' must be a string literal: vformat(fmt, ...) formats a run-time text, and + or a template string joins text");
    }
    // print / format over a literal that holds a placeholder: its placeholders and the values after it pair up (the formatter
    // raises the run-time twin). A trailing skip_newline / skip_flush is no value, and an argument of unknown type may be one:
    // the counts then err toward the larger, and an unused argument is the run's to report.
    // the name binds the engine's own print or format
    bool formatter_bound(const std::string& name) const {
        const script_value bound = eng_.get_variable(name);
        return bound.is_function() && eng_.format_literal_callee(&bound.unchecked_as_function());
    }
    void check_format_arguments(const call_expr* c, const std::string& name, const std::vector<ctype>& args) {
        if (!c->literal_format_call() || args.size() != c->arguments.size() || !formatter_bound(name)) { return; }
        const std::string_view text = static_cast<const literal_expr*>(c->arguments[0].get())->value.unchecked_as_string();
        if (!detail::has_format_placeholders(text)) { return; }
        enum class role : uint8_t { value, control, unsure };
        const auto role_of = [&](size_t i) {
            const ctype& t = args[i];
            const bool control_class = t.kind == ctype::object_k && (t.cls == "skip_newline_t" || t.cls == "skip_flush_t");
            if (!t.is_unknown() && !t.tentative && !control_class) { return role::value; }
            if (c->arguments[i]->get_type() == node_type::identifier_expr) {
                const std::string_view spelled = static_cast<const identifier_expr*>(c->arguments[i].get())->name;
                if (spelled == "skip_newline" || spelled == "skip_flush") { return role::control; }
            }
            return role::unsure;
        };
        size_t count = 0;
        bool unsure = false;
        const bool every_position = name == "format";   // format drops a control value anywhere, print reads its last argument
        for (size_t i = 1; i < args.size(); ++i) {
            const role r = every_position || i + 1 == args.size() ? role_of(i) : role::value;
            if (r == role::control) { continue; }
            unsure = unsure || r == role::unsure;
            ++count;
        }
        const detail::format_text_use use = detail::walk_format_text(text, count, detail::format_text_counter{});
        if (use.agrees(count) || (unsure && use.sequential <= count && use.positional_past == detail::format_text_use::none)) { return; }
        diag(diag_level::error, c->arguments[0]->location, "the format text of '" + name + "' " + detail::format_text_mismatch(use, count));
    }
    // sleep(seconds): one number, and a place where a run can suspend
    void check_sleep_call(const std::vector<ctype>& args, const source_location& loc) {
        if (args.size() != 1) {
            diag(diag_level::error, loc, "'sleep' expects 1 argument(s), got " + std::to_string(args.size()));
            return;
        }
        if (!namespace_arg_accepts(args[0], script_value_type::jai_float_type)) {
            diag(diag_level::error, loc, "argument 1 of 'sleep': cannot pass '" + ctype_name(args[0]) + "' where " +
                 namespace_param_spelling(script_value_type::jai_float_type) + " is expected");
            return;
        }
        check_sleep_context(loc);
    }
    // A sleep where a run can never suspend: a parallel body refuses it as admission does, a destructor or the reload hook blocks
    void check_sleep_context(const source_location& loc) {
        if (parallel_body_depth_ > 0) {
            diag(diag_level::error, loc, std::string(detail::k_sleep_in_parallel_body));
            return;
        }
        const bool in_reload_hook = std::any_of(fn_stack_.begin(), fn_stack_.end(), [](const fn_ctx& f) { return f.name == "<hot_reload_migrate>" || f.name == "hot_reload_migrate"; });
        if (cdtor_ctx_ == cdtor_ctx::destruction) { diag(diag_level::warning, loc, std::string(detail::k_sleep_in_destructor)); }
        else if (in_reload_hook) { diag(diag_level::warning, loc, std::string(detail::k_sleep_in_reload_hook)); }
    }
    std::string_view ov_class_{};   // the class whose methods infer_call_impl resolved the call to (empty: not a method call)

    static ref_write_callee ref_write_callee_of(const function_decl* f) {
        return ref_write_callee{&f->parameters, f->body.get(), f->name, false, f->is_coroutine};
    }
    // detail/ref_param_writes.hpp with the checker's names: this unit's function, then the program's, then the engine's;
    // an overloaded name is not seen (the backends' global lookup cannot see an overload set either)
    ref_param_write ref_writes(const function_decl* fn, size_t i) {
        ref_write_analysis analysis([this](const identifier_expr& id, std::vector<ref_write_callee>& out) {
            if (auto it = functions_.find(id.name); it != functions_.end()) {
                if (it->second.size() != 1) { return false; }
                out.push_back(ref_write_callee_of(it->second.front()));
                return true;
            }
            eng_.note(id.name);
            if (program_functions_) {
                if (auto pit = program_functions_->find(std::string(id.name)); pit != program_functions_->end()) {
                    if (pit->second.size() != 1 || pit->second.begin()->second.size() != 1) { return false; }
                    out.push_back(ref_write_callee_of(pit->second.begin()->second.front().get()));
                    return true;
                }
            }
            return resolve_global_ref_write_callee(eng_.asking(id.name), id, out);
        });
        return analysis.analyze(ref_write_callee_of(fn), i);
    }
    // every method named `name` a class derived from `cls` declares (a call through a `cls` receiver may reach any)
    std::vector<const function_decl*> overrides_of(std::string_view cls, std::string_view name) {
        std::vector<const function_decl*> out;
        if (cls.empty()) { return out; }   // no method call: no class is asked
        for (const auto* table : {&classes_, &program_sclasses_}) {
            for (const auto& [cname, sc] : *table) {
                if (cname == cls || class_relation(cname, cls) != 1) { continue; }
                if (auto m = sc.methods.find(name); m != sc.methods.end()) {
                    for (const auto* f : m->second) { out.push_back(f); }
                }
            }
        }
        return out;
    }
    // a bare name in a body that sees this-fields, declared by a class derived from the enclosing one: `this` may be one of those
    bool derived_class_member(std::string_view name) {
        if (!class_ctx_ || fn_stack_.empty() || !fn_stack_.back().sees_fields) { return false; }
        for (const auto* table : {&classes_, &program_sclasses_}) {
            for (const auto& [cname, sc] : *table) {
                if (cname == class_ctx_name_ || class_relation(cname, class_ctx_name_) != 1) { continue; }
                if (sc.fields.count(name) || sc.static_fields.count(name) || sc.methods.count(name) || sc.static_methods.count(name)) { return true; }
            }
        }
        return false;
    }

    // C++ [class.cdtor]: a constructor, field initializer or destructor reaches its own class's members only, so a call there to a
    // name only a derived class declares is the refusal the backends raise
    bool report_cdtor_call(std::string_view name, const source_location& loc) {
        if (cdtor_ctx_ == cdtor_ctx::none || !class_ctx_ || overrides_of(class_ctx_name_, name).empty()) { return false; }
        diag(diag_level::error, loc, format_error_message(cdtor_ctx_ == cdtor_ctx::construction ? detail::k_call_under_construction
                                                                                                 : detail::k_call_under_destruction,
                                                          name, class_ctx_name_));
        return true;
    }

    // -------------------------------------------------------------- diagnostics

    // Where the declaration a diagnostic cites stands. One an AST node gives carries its file, which the listing names when
    // it is not the diagnostic's own (an instance of a generic another file declares, text spliced into a body)
    struct declared_at {
        size_t line = 0, column = 0;
        const std::string* file = nullptr;
        declared_at() = default;
        declared_at(size_t a_line, size_t a_column) : line(a_line), column(a_column) {}
        declared_at(const source_location& a_place) : line(a_place.line), column(a_place.column), file(&a_place.filename) {}
    };
    void diag(diag_level severity, const source_location& loc, std::string message,
              size_t related_line = 0, size_t related_column = 0, bool always_error = false) {
        diag(severity, loc, std::move(message), declared_at{related_line, related_column}, always_error);
    }
    // always_error: a program no run survives, an error under [[warn_mode]] too
    void diag(diag_level severity, const source_location& loc, std::string message, const declared_at& related, bool always_error = false) {
        if (!collect_diagnostics_) { return; }
        const uint8_t mode = region_mode();
        // an error in an instance the unit only makes again is that instance's, which keeps its earlier declaration: under
        // [[warn_mode]] too, where the unit's own errors are warnings
        if (severity == diag_level::error && remade_failures_) {
            if (const generic_instance_decl* remade = remade_root(instance_ctx_)) { note_remade_failure(*remade, loc, message); }
        }
        if (mode == check_mode_attr_warn && !always_error) { severity = diag_level::warning; }
        if (diags_.size() >= k_check_diagnostic_cap) {
            capped_ = true;
            return;
        }
        // inside a template's body, a generic another file declares and a file an include splices in, the place is that file's
        std::string file = template_files_ > 0 || (!unit_file_.empty() && !loc.filename.empty() && loc.filename != unit_file_) ? loc.filename : std::string();
        std::string key = file + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column) + ":" + message;
        if (instance_ctx_) { key += ":"; key += instance_ctx_->name; }   // each instance reports what its own arguments break
        if (!seen_.insert(std::move(key)).second) {
            return;
        }
        const bool cites = file.empty() && related.line != 0;
        std::string related_file = cites && related.file && !related.file->empty() && *related.file != loc.filename ? *related.file : std::string();
        diags_.push_back({severity, loc.line, loc.column, std::move(message), cites ? related.line : 0, cites ? related.column : 0, std::move(file),
                          instance_notes(instance_ctx_), std::move(related_file)});
    }

    // -------------------------------------------------------------- generics (detail/generics.hpp)

    std::unordered_map<std::string, std::vector<generic_definition_ptr>> generics_;   // this unit's, by qualified name
    std::unordered_set<std::string_view> generic_method_names_;                        // its generic methods' bare names
    bool generics_live_ = false;   // the unit or the program declares a generic: calls look their callee up among them
    // This unit's requests, by instance name: one per definition the name's instances are made from (generic_instance_id; two
    // generic functions that take the same type arguments are two instances of one name)
    std::unordered_map<std::string_view, std::vector<const generic_instance_decl*>> instance_requests_;
    std::unordered_set<const generic_instance_decl*> unmade_instances_;   // their bounds do not hold: their declarations are not checked
    struct method_instance {
        std::vector<const function_decl*> fns;
        uint8_t access = 0;
        std::string_view owner;
        bool is_static = false;
    };
    std::unordered_map<std::string_view, method_instance> method_instances_;   // "Class::name<Args>" -> the unit's declarations, one per definition
    std::vector<generic_request>* generic_requests_ = nullptr;
    std::set<std::pair<std::string, uint64_t>> requested_now_;   // the instances this check asked for, each by its name and stamp
    std::vector<generic_kept_instance>* remade_failures_ = nullptr;
    const std::vector<generic_kept_instance>* kept_instances_ = nullptr;
    const generic_instance_decl* instance_ctx_ = nullptr;      // the instance whose declarations are being walked
    std::string unit_file_;                                    // the file the unit's own text is in

    void collect_generics(const std::vector<declaration_ptr>& decls) {
        for (const auto& d : decls) {
            if (!d) { continue; }
            switch (d->get_type()) {
                case node_type::generic_decl: {
                    const auto& definition = static_cast<const generic_decl*>(d.get())->definition;
                    generics_[definition->qualified_name()].push_back(definition);
                    break;
                }
                case node_type::generic_instance_decl: {
                    auto* request = static_cast<const generic_instance_decl*>(d.get());
                    instance_requests_[request->name].push_back(request);
                    if (request->within.empty() && unit_file_.empty()) { unit_file_ = request->location.filename; }
                    break;
                }
                case node_type::namespace_decl:
                    collect_generics(static_cast<const namespace_decl*>(d.get())->declarations);
                    break;
                case node_type::class_decl: {
                    auto* cd = static_cast<const class_decl*>(d.get());
                    for (const auto& m : cd->members) {
                        if (!m.declaration) { continue; }
                        if (m.declaration->get_type() == node_type::generic_decl) {
                            const auto& definition = static_cast<const generic_decl*>(m.declaration.get())->definition;
                            generics_[definition->qualified_name()].push_back(definition);
                            generic_method_names_.insert(definition->name);
                        } else if (cd->method_instances && m.declaration->get_type() == node_type::function_decl) {
                            auto* fn = static_cast<const function_decl*>(m.declaration.get());
                            method_instance& instance = method_instances_[fn->instance_name];
                            const auto held = std::find_if(instance.fns.begin(), instance.fns.end(), [&](const function_decl* f) { return f->instance_stamp == fn->instance_stamp; });
                            if (held == instance.fns.end()) { instance.fns.push_back(fn); } else { *held = fn; }
                            instance.access = m.visibility == class_decl::Private ? 1 : m.visibility == class_decl::Protected ? 2 : 0;
                            instance.owner = cd->name;
                            instance.is_static = fn->is_static;
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    // "in Stack<int>, instantiated at app.jai:12:5", then the instance that asked for that one, outward
    std::vector<std::string> instance_notes(const generic_instance_decl* instance) const {
        std::vector<std::string> notes;
        size_t levels = 0;
        for (; instance && levels <= k_generic_depth_limit; ++levels) {
            if (levels < 4) { notes.push_back("in " + shown_instance_name(instance->name) + ", instantiated at " + instance->location.to_string()); }
            instance = instance->within.empty() ? nullptr : instance_request(instance->asker());
        }
        if (levels > 4) { notes.push_back("and " + std::to_string(levels - 4) + " more instantiations around these"); }
        return notes;
    }
    const generic_instance_decl* instance_request(const generic_instance_id& instance) const {
        const auto held = instance_requests_.find(instance.name);
        if (held == instance_requests_.end()) { return nullptr; }
        const auto request = std::find_if(held->second.begin(), held->second.end(), [&](const generic_instance_decl* r) { return r->stamp == instance.stamp; });
        return request != held->second.end() ? *request : nullptr;
    }
    // the request a function's or a method's instance was made for
    const generic_instance_decl* instance_request(const function_decl& fn) const { return instance_request(generic_instance_id{fn.instance_name, fn.instance_stamp}); }
    // a class instance's: a class's name is one class
    const generic_instance_decl* class_instance_request(std::string_view name) const { return instance_request(generic_instance_id{name, 0}); }
    // The instance the unit makes again (generic_instance_decl::remade) that `instance` is, or was asked for by; null for one
    // the unit's own text asked for
    const generic_instance_decl* remade_root(const generic_instance_decl* instance) const {
        for (size_t levels = 0; instance && levels <= k_generic_depth_limit; ++levels) {
            if (instance->remade) { return instance; }
            instance = instance->within.empty() ? nullptr : instance_request(instance->asker());
        }
        return nullptr;
    }
    void note_remade_failure(const generic_instance_decl& remade, const source_location& at, const std::string& message) {
        const bool at_declaration = at.line == remade.location.line && at.column == remade.location.column && at.filename == remade.location.filename;
        remade_failures_->push_back({std::string(remade.name), std::string(remade.generic), at_declaration ? message : message + " (" + at.to_string() + ")", remade.location, remade.stamp});
    }
    // The instances a declaration of this unit could not make again keep the declaration they have
    void warn_kept_instances() {
        if (!kept_instances_) { return; }
        for (const auto& kept : *kept_instances_) {
            diag(diag_level::warning, kept.at, "'" + kept.name + "' keeps its earlier declaration: this declaration of '" + kept.generic + "' cannot make it: " + kept.reason);
        }
    }
    struct instance_scope {
        const generic_instance_decl*& slot;
        const generic_instance_decl* saved;
        instance_scope(const generic_instance_decl*& s, const generic_instance_decl* entered) : slot(s), saved(s) { slot = entered; }
        ~instance_scope() { slot = saved; }
    };

    // A generic's own declaration: its name is the unit's once per shape. A host template's name stays the template's, since
    // Name<Args> spells the host's instantiation; a generic class cannot take a host-bound class's name, as no script class
    // can; a generic function may, as a plain function may (a method's name is its class's own)
    void walk_generic(const generic_definition& g) {
        const std::string qualified = g.qualified_name();
        if (g.kind != generic_kind::method) {
            if (eng_.is_template_type(qualified)) {
                diag(diag_level::error, g.location, "'" + qualified + "' is a host template: a generic cannot take its name", 0, 0, /*always_error=*/true);
            } else if (g.kind == generic_kind::class_type) {
                if (const auto held = eng_.get_class_definition(qualified); held && !held->is_script_class()) {
                    diag(diag_level::error, g.location, format_error_message(detail::k_host_class_name_taken, qualified, qualified), 0, 0, /*always_error=*/true);
                }
            }
        }
        if (!generics_.count(qualified)) { return; }
        const auto counted = [](size_t count) { return std::to_string(count) + (count == 1 ? " type parameter" : " type parameters"); };
        // one name, one class: a class and a generic class of a name in one unit are an error at the second of them
        if (const class_decl* plain = g.kind == generic_kind::class_type ? unit_plain_class(qualified) : nullptr) {
            const bool generic_second = plain->location.line < g.location.line || (plain->location.line == g.location.line && plain->location.column < g.location.column);
            const source_location& first = generic_second ? plain->location : g.location;
            diag(diag_level::error, generic_second ? g.location : plain->location, "class '" + qualified + "' is declared " +
                 (generic_second ? "without type parameters" : "with " + counted(g.parameters.size())) + " on line " + std::to_string(first.line) + " and " +
                 (generic_second ? "with " + counted(g.parameters.size()) : "without type parameters") + " here: a class and a generic class cannot share a name",
                 first.line, first.column, /*always_error=*/true);
        }
        const generic_definition* earlier = clashing_declaration(g);
        if (!earlier) { return; }
        if (!same_generic_signature(*earlier, g)) {
            diag(diag_level::error, g.location, "generic class '" + qualified + "' is declared with " + counted(earlier->parameters.size()) + " on line " +
                 std::to_string(earlier->location.line) + " and with " + counted(g.parameters.size()) + " here: a generic class cannot be overloaded by its parameter count",
                 earlier->location, /*always_error=*/true);
            return;
        }
        diag(diag_level::error, g.location, "generic '" + generic_spelling(g) + "' is declared again: line " + std::to_string(earlier->location.line) +
             " declares it with the same parameters", earlier->location, /*always_error=*/true);
    }
    // The unit's earlier generic that `g` clashes with: one of its signature (`g` declares it again), or for a generic class one of
    // another parameter count, as a class's name is one class (functions overload by their parameters, a generic class by nothing)
    const generic_definition* clashing_declaration(const generic_definition& g) const {
        const auto declared = generics_.find(g.qualified_name());
        if (declared == generics_.end()) { return nullptr; }
        for (const auto& earlier : declared->second) {
            if (earlier.get() == &g) { break; }
            if (g.kind == generic_kind::class_type && earlier->kind == generic_kind::class_type && earlier->parameters.size() != g.parameters.size()) { return earlier.get(); }
            if (same_generic_signature(*earlier, g)) { return earlier.get(); }
        }
        return nullptr;
    }
    // An instance of a generic the unit declares again is that declaration's one error, never a definition of the instance's
    // name beside the first declaration's
    bool made_by_declaration_again(const function_decl* fn) const {
        if (fn->instance_stamp == 0) { return false; }
        const generic_instance_decl* request = instance_request(*fn);
        const auto declared = request ? generics_.find(std::string(request->generic)) : generics_.end();
        if (declared == generics_.end()) { return false; }
        for (const auto& g : declared->second) {
            if (g->stamp == fn->instance_stamp) { return clashing_declaration(*g) != nullptr; }
        }
        return false;
    }

    // The [[warn_mode]] / [[strict_mode]] region a requested instance's generic is declared in (none: the unit's own region)
    uint8_t instance_region_mode(const generic_instance_decl* request) {
        if (!request) { return check_mode_attr_none; }
        for (const generic_definition_ptr& g : generics_named(std::string(request->generic))) {
            if (g && generic_instance_stamp(*g) == request->stamp) { return g->check_mode; }
        }
        return check_mode_attr_none;
    }

    // A generic method's instances: each a method of the class the declaration names, checked as that class's own
    void walk_method_instances(const class_decl* cd) {
        sclass* owner = find_sclass(cd->name);
        sclass engine_held;   // a class an earlier run declared: member lookups reach the engine's definition by its name
        if (!owner) {
            if (!engine_class(cd->name)) { return; }
            model_class(cd, engine_held);
            owner = &engine_held;
        }
        sclass* const enclosing = class_ctx_;
        const std::string_view enclosing_name = class_ctx_name_;
        class_ctx_ = owner;
        class_ctx_name_ = cd->name;
        for (const auto& m : cd->members) {
            if (!m.declaration || m.declaration->get_type() != node_type::function_decl) { continue; }
            auto* fn = static_cast<function_decl*>(m.declaration.get());
            const generic_instance_decl* request = instance_request(*fn);
            if (unmade_instances_.count(request)) { continue; }
            instance_scope scope(instance_ctx_, request);
            mode_guard declared(mode_stack_, instance_region_mode(request), /*scoped=*/true);
            walk_function(fn);
        }
        class_ctx_ = enclosing;
        class_ctx_name_ = enclosing_name;
    }

    // The request ahead of an instance's declarations: its bounds, checked where the instance was asked for, then whatever kept
    // the instance from being made
    void walk_instance_request(const generic_instance_decl* request) {
        const generic_instance_decl* asker = request->within.empty() ? nullptr : instance_request(request->asker());
        bool bounded = true;
        {
            instance_scope scope(instance_ctx_, asker);
            for (const auto& bound : request->bounds) {
                if (class_is_or_derives(handle_class(bound.argument), bound.requires_)) { continue; }
                bounded = false;
                std::string text = bound_failure_text(*request, bound);
                if (request->remade && remade_failures_) { note_remade_failure(*request, request->location, text); }
                diag(diag_level::error, request->location, std::move(text), 0, 0, /*always_error=*/true);
            }
        }
        if (!bounded) { unmade_instances_.insert(request); return; }
        for (const auto& failure : request->failures) {
            const bool at_request = failure.at.line == request->location.line && failure.at.column == request->location.column &&
                                    failure.at.filename == request->location.filename;
            instance_scope scope(instance_ctx_, at_request ? asker : request);
            if (at_request && request->remade && remade_failures_) { note_remade_failure(*request, failure.at, failure.message); }
            diag(diag_level::error, failure.at, failure.message, 0, 0, /*always_error=*/true);
        }
    }

    // The class this unit declares under a name that is no generic's instance; null when it declares none
    const class_decl* unit_plain_class(std::string_view name) const {
        const auto held = classes_.find(name);
        return held != classes_.end() && held->second.decl && held->second.decl->instance_stamp == 0 && !held->second.decl->method_instances ? held->second.decl : nullptr;
    }
    // The generics a qualified name declares: this unit's, then those of the rest of the program this unit does not declare again.
    // A class's name is one class: the unit's generic class, or its class of the name, stands for the program's generic class;
    // a function or a method of one shape is one generic, and the unit's own stands for the program's.
    std::vector<generic_definition_ptr> generics_named(const std::string& qualified) {
        std::vector<generic_definition_ptr> found;
        if (auto own = generics_.find(qualified); own != generics_.end()) { found = own->second; }
        const bool class_here = unit_plain_class(qualified) || std::any_of(found.begin(), found.end(), [](const generic_definition_ptr& f) { return f->kind == generic_kind::class_type; });
        if (const auto* held = eng_.find_generics(qualified)) {
            for (const auto& g : *held) {
                if (class_here && g->kind == generic_kind::class_type) { continue; }
                const bool declared_here = std::any_of(found.begin(), found.end(), [&](const generic_definition_ptr& f) {
                    return f == g || same_generic_signature(*f, *g);
                });
                if (!declared_here) { found.push_back(g); }
            }
        }
        return found;
    }
    // The generic class that holds a name: where one does, the bare name is no class's (a class the name had before stays
    // for the code and the objects that use it already)
    generic_definition_ptr generic_class_named(std::string_view name) {
        for (const auto& g : generics_named(std::string(name))) { if (g->kind == generic_kind::class_type) { return g; } }
        return nullptr;
    }
    // A generic function or method a bare name reaches where a value stands, by the lookup a call of it takes: what to write
    // instead, or empty when the name reaches none
    JAI_NOINLINE std::string generic_named_as_value(std::string_view name) {
        if (!generics_live_) { return {}; }
        std::vector<generic_definition_ptr> found;
        const bool in_class = class_ctx_ && !(found = class_generics(class_ctx_name_, name)).empty();
        for (std::string space(namespace_walk_names_.empty() ? std::string_view() : namespace_walk_names_.back()); found.empty() && !space.empty();) {
            found = generics_named(space + "::" + std::string(name));
            const size_t cut = space.rfind("::");
            space.resize(cut == std::string::npos ? 0 : cut);
        }
        if (found.empty()) { found = generics_named(std::string(name)); }
        return generic_value_spelled(found, name, in_class ? "this" : "");
    }
    // `spelled` names `found` where a value stands; through `receiver` when it is a method reached through one
    static std::string generic_value_spelled(const std::vector<generic_definition_ptr>& found, std::string_view spelled, const std::string& receiver) {
        const auto callable = std::find_if(found.begin(), found.end(), [](const generic_definition_ptr& g) { return g->kind != generic_kind::class_type; });
        if (callable == found.end()) { return {}; }
        const generic_definition& g = **callable;
        const std::string call = std::string(spelled) + generic_spelling(g).substr(g.qualified_name().size());
        if (!receiver.empty() && !g.is_static) { return detail::generic_method_value_text(spelled, {}, receiver, call); }
        return detail::generic_value_text(spelled, {}, g.kind == generic_kind::method, call);
    }
    // What a name is that takes no type arguments (detail/generics.hpp): a builtin type, an enum, a host class, a script class
    plain_type_kind plain_type_named(const std::string& name) {
        if (name.empty() || find_local(name)) { return plain_type_kind::none; }
        if (builtin_type_name(name)) { return plain_type_kind::builtin; }
        if (find_enum(name) || (program_enums_ && program_enums_->count(name) != 0) || eng_.is_enum_name(name)) { return plain_type_kind::enumeration; }
        if (find_sclass(name)) { return plain_type_kind::script_class; }
        const auto held = engine_class(name);
        return held ? (held->is_cpp_class() ? plain_type_kind::host_class : plain_type_kind::script_class) : plain_type_kind::none;
    }
    // Where a class is spelled by a bare name a generic class holds: the error, and true
    bool refuse_bare_generic_class(std::string_view name, const source_location& loc) {
        if (!generics_live_ || name.empty()) { return false; }
        const generic_definition_ptr g = generic_class_named(name);
        if (!g) { return false; }
        diag(diag_level::error, loc, "'" + std::string(name) + "' is a generic class: name its type arguments (" + generic_spelling(*g) + ")");
        return true;
    }
    // A class's generic methods of a name: its own, else the nearest base's that declares one (a derived class's hides a base's)
    std::vector<generic_definition_ptr> class_generics(std::string_view cls, std::string_view name, int depth = 0) {
        if (depth > 32 || cls.empty()) { return {}; }
        auto found = generics_named(std::string(cls) + "::" + std::string(name));
        if (!found.empty()) { return found; }
        if (const sclass* c = find_sclass(cls)) {
            for (auto base : c->decl->base_classes) {
                found = class_generics(base, name, depth + 1);
                if (!found.empty()) { return found; }
            }
            return {};
        }
        if (auto def = engine_class(cls)) {
            for (const auto& parent : def->get_parent_classes()) {
                found = class_generics(parent->get_name(), name, depth + 1);
                if (!found.empty()) { return found; }
            }
        }
        return {};
    }

    // The class a program-scope variable of another unit is declared with (Counter c; shared_ptr<Counter> c;), when one unit
    // declares the name once: what picks a generic method on it, as a declared type does in C++
    std::string_view declared_global_class(const identifier_expr* id) {
        if (!program_globals_ || find_local(id->name)) { return {}; }
        eng_.note(id->name);
        const auto held = program_globals_->find(std::string(id->name));
        if (held == program_globals_->end() || held->second.size() != 1 || held->second.begin()->second.size() != 1) { return {}; }
        const variable_decl* g = held->second.begin()->second.front().get();
        const type_info* t = g ? g->type.get() : nullptr;
        while (t && t->base_type == script_value_type::jai_reference_type && !t->type_params.empty()) { t = t->type_params[0].get(); }
        if (!t || (t->base_type != script_value_type::jai_object_type && t->base_type != script_value_type::jai_shared_ptr_type)) { return {}; }
        return t->type_name == "shared_ptr" ? std::string_view{} : std::string_view(t->type_name);
    }

    // What a call's callee names among the generics, by the lookup an ordinary call of that spelling takes: a method of the
    // receiver's class, a member of an enclosing namespace, a program-scope name
    std::vector<generic_definition_ptr> generic_callee(call_expr* c, bool& receiver_unknown) {
        receiver_unknown = false;
        expression* callee = c->callee.get();
        if (callee->get_type() == node_type::identifier_expr) {
            auto* id = static_cast<identifier_expr*>(callee);
            if (!id->global_qualified) {
                if (size_t scope_index = 0; callable_variable(id, &scope_index)) { return {}; }
                if (class_ctx_) {
                    auto found = class_generics(class_ctx_name_, id->name);
                    if (!found.empty()) { return found; }
                }
                if (!namespace_walk_names_.empty()) {
                    for (std::string space(namespace_walk_names_.back());;) {
                        auto found = generics_named(space + "::" + std::string(id->name));
                        if (!found.empty()) { return found; }
                        const size_t cut = space.rfind("::");
                        if (cut == std::string::npos) { break; }
                        space.resize(cut);
                    }
                }
            }
            return generics_named(std::string(id->name));
        }
        if (callee->get_type() != node_type::member_expr) { return {}; }
        auto* m = static_cast<member_expr*>(callee);
        if (m->is_static) {
            const std::string qualified = qualified_spelling(m);
            if (qualified.empty()) { return {}; }
            auto found = generics_named(qualified);
            if (!found.empty()) { return found; }
            const std::string owner = qualified.substr(0, qualified.size() - m->member.size() - 2);
            return find_sclass(owner) || engine_class(owner) ? class_generics(owner, m->member) : std::vector<generic_definition_ptr>{};
        }
        if (!generic_method_names_.count(m->member) && !eng_.names_generic_method(std::string(m->member))) { return {}; }
        const std::string_view cls = receiver_class(m, receiver_unknown);
        return cls.empty() ? std::vector<generic_definition_ptr>{} : class_generics(cls, m->member);
    }
    // The class a method call's receiver has as the load knows it: its own type, a reference's declared class (Base& b), a
    // global another unit declares with a class; empty when the load knows none (`unknown` when it knows nothing of it)
    std::string_view receiver_class(const member_expr* m, bool& unknown) {
        unknown = false;
        const ctype recv = infer(m->object.get());
        if ((recv.kind == ctype::object_k || recv.kind == ctype::class_ref_k) && !recv.cls.empty()) { return recv.cls; }
        if (recv.is_unknown()) {
            if (const ctype held = referent_receiver(m->object.get()); held.kind == ctype::object_k && !held.cls.empty()) { return held.cls; }
            if (m->object->get_type() == node_type::identifier_expr) {
                const auto* id = static_cast<const identifier_expr*>(m->object.get());
                const std::string_view declared = declared_global_class(id);
                if (!declared.empty()) { return declared; }
                // a name of the frame the debugger evaluates in (its instance variables, which no declaration names): the class of
                // the object it holds now
                if (const std::string_view held = eng_.evaluation_receiver_class(std::string(id->name)); !held.empty()) { return held; }
            }
        }
        unknown = recv.is_unknown();
        return {};
    }

    static std::string generic_spelling(const generic_definition& g) {
        std::string text = g.qualified_name() + "<";
        for (size_t i = 0; i < g.parameters.size(); ++i) { text += (i ? ", " : "") + std::string(g.parameters[i].name); }
        return text + ">";
    }
    // Whether the unit declares the instance of `g` under `instance`: the name alone for a class, and for a function or a
    // method the one made from `g` among the name's overloads
    bool instance_declared(const generic_definition& g, const std::string& instance) {
        const auto made_from = [&](const std::vector<const function_decl*>& overloads) {
            return std::any_of(overloads.begin(), overloads.end(), [&](const function_decl* fn) { return fn->instance_stamp == g.stamp; });
        };
        switch (g.kind) {
            case generic_kind::class_type: return classes_.count(instance) != 0;
            case generic_kind::method: {
                const auto held = method_instances_.find(instance);
                return held != method_instances_.end() && made_from(held->second.fns);
            }
            default: {
                if (g.space.empty()) {
                    const auto held = functions_.find(instance);
                    return held != functions_.end() && made_from(held->second);
                }
                const auto held = namespace_fns_.find(instance);
                return held != namespace_fns_.end() && made_from(held->second);
            }
        }
    }

    // ---- deduction (docs/site/guide/21-generics.html "Deduction")

    // The one type a lambda's returns yield where it declares none: what the load is certain of, else the one its returns give
    // with the results declarations state read
    const type_info* lambda_yield(const lambda_expr* l) const {
        if (const auto yields = lambda_return_types_.find(l); yields != lambda_return_types_.end()) { return yields->second; }
        const auto states = lambda_stated_return_types_.find(l);
        return states == lambda_stated_return_types_.end() ? nullptr : states->second;
    }
    // What a value argument's load-time type is, as a type an instance can be made with; null when the load does not know it.
    // declared_results: a result a declaration states counts (ctype::declared_result), as it does for every call's deduction
    const type_info* deduction_type(const ctype& arg) { return deduction_type(arg, true); }
    const type_info* deduction_type(const ctype& arg, bool declared_results) {
        const type_info* t = arg.ti;
        while (t && t->base_type == script_value_type::jai_reference_type && !t->type_params.empty()) { t = t->type_params[0].get(); }
        // a container spelled without its element (a literal's own type) is read from the literal below
        const bool open_container = t && (t->base_type == script_value_type::jai_array_type || t->base_type == script_value_type::jai_map_type) &&
                                    std::any_of(t->type_params.begin(), t->type_params.end(), [](const type_info_ptr& p) { return !p || p->base_type == script_value_type::jai_any_type; });
        if (t && t->base_type != script_value_type::jai_any_type && !open_container) { return t; }
        if (arg.tentative && !(arg.declared_result && declared_results)) { return nullptr; }
        switch (arg.kind) {
            case ctype::int_k: return arg.cls.empty() ? eng_.get_type_info_int() : eng_.get_type_info_object(std::string(arg.cls));
            case ctype::float_k: return eng_.get_type_info_float();
            case ctype::bool_k: return eng_.get_type_info_bool();
            case ctype::string_k: return eng_.get_type_info_string();
            case ctype::char_k: return eng_.get_type_info_char();
            case ctype::utf8_k: return eng_.get_type_info_utf8_string();
            case ctype::math_k: return eng_.get_type_info_math(math_type_from_name(arg.cls));   // a math constructor's result, a math-typed value
            case ctype::coroutine_k: return arg.yields ? eng_.get_type_info_coroutine(const_cast<type_info*>(arg.yields)) : nullptr;
            case ctype::object_k: return arg.cls.empty() ? nullptr : eng_.get_type_info_object(std::string(arg.cls));
            case ctype::array_k: {   // a literal of one element type: [1, 2] is an array<int>
                if (!arg.origin || arg.origin->get_type() != node_type::array_literal_expr) { return nullptr; }
                const type_info* element = nullptr;
                for (const auto& el : static_cast<const array_literal_expr*>(arg.origin)->elements) {
                    const type_info* t = deduction_type(recorded_literal_element(el.get()), declared_results);
                    if (!t || (element && t->canonical_name() != element->canonical_name())) { return nullptr; }
                    element = t;
                }
                return element ? eng_.get_type_info_array(const_cast<type_info*>(element)) : nullptr;
            }
            case ctype::map_k: {
                if (!arg.origin || arg.origin->get_type() != node_type::map_literal_expr) { return nullptr; }
                const type_info* key = nullptr;
                const type_info* value = nullptr;
                for (const auto& [k, v] : static_cast<const map_literal_expr*>(arg.origin)->entries) {
                    const type_info* kt = deduction_type(recorded_literal_element(k.get()), declared_results);
                    const type_info* vt = deduction_type(recorded_literal_element(v.get()), declared_results);
                    if (!kt || !vt || (key && kt->canonical_name() != key->canonical_name()) || (value && vt->canonical_name() != value->canonical_name())) { return nullptr; }
                    key = kt;
                    value = vt;
                }
                return key ? eng_.get_type_info_map(const_cast<type_info*>(key), const_cast<type_info*>(value)) : nullptr;
            }
            default: return nullptr;
        }
    }
    static size_t type_parameter_index(const generic_definition& g, const type_info& t) {
        if (t.base_type != script_value_type::jai_object_type || !t.type_params.empty()) { return SIZE_MAX; }
        for (size_t i = 0; i < g.parameters.size(); ++i) { if (g.parameters[i].name == t.type_name) { return i; } }
        return SIZE_MAX;
    }
    static bool names_type_parameter(const generic_definition& g, const type_info* pattern, size_t parameter, int depth = 0) {
        if (!pattern || depth > 64) { return false; }
        if (type_parameter_index(g, *pattern) == parameter) { return true; }
        for (const auto& inner : pattern->type_params) { if (names_type_parameter(g, inner.get(), parameter, depth + 1)) { return true; } }
        return false;
    }
    // The generic and arguments a class is an instance of, or derives an instance of, when the generic is `generic`
    bool instance_arguments(std::string_view cls, std::string_view generic, std::vector<type_info_ptr>& arguments, int depth = 0) {
        if (depth > 32 || cls.empty()) { return false; }
        if (const generic_instance_decl* request = class_instance_request(cls)) {
            if (request->generic == generic) { arguments = request->arguments; return true; }
        } else {
            std::string made_from;
            std::vector<type_info_ptr> made_with;
            if (eng_.generic_instance(std::string(cls), made_from, made_with) && made_from == generic) { arguments = std::move(made_with); return true; }
            // a host template's instance is made by the host and named by its arguments (template_instance_name)
            std::string_view base;
            std::vector<std::string_view> spelled;
            if (template_name_arguments(cls, base, spelled) && base == generic && eng_.is_template_type(std::string(base))) {
                for (const std::string_view argument : spelled) { made_with.push_back(type_info_ptr(eng_.type_named(argument))); }
                if (std::none_of(made_with.begin(), made_with.end(), [](const type_info_ptr& t) { return !t; })) { arguments = std::move(made_with); return true; }
            }
        }
        if (const sclass* c = find_sclass(cls)) {
            for (auto base : c->decl->base_classes) { if (instance_arguments(base, generic, arguments, depth + 1)) { return true; } }
            return false;
        }
        if (auto def = engine_class(cls)) {
            for (const auto& parent : def->get_parent_classes()) { if (instance_arguments(parent->get_name(), generic, arguments, depth + 1)) { return true; } }
        }
        return false;
    }
    struct deduced_type {
        const type_info* type = nullptr;
        size_t argument = 0;   // the value argument it came from, counted from 1; 0: written in the call
    };
    struct deduction {
        const generic_definition& g;
        std::vector<deduced_type> found;
        std::string error;
        size_t argument = 0;
    };
    // `pattern` (a parameter's declared type, the type parameters standing in it as classes) against the type an argument has
    void unify(deduction& d, const type_info* pattern, const type_info* actual, int depth = 0) {
        if (!pattern || !actual || depth > 64 || !d.error.empty()) { return; }
        while (pattern->base_type == script_value_type::jai_reference_type && !pattern->type_params.empty() && pattern->type_params[0]) { pattern = pattern->type_params[0].get(); }
        while (actual->base_type == script_value_type::jai_reference_type && !actual->type_params.empty() && actual->type_params[0]) { actual = actual->type_params[0].get(); }
        if (const size_t p = type_parameter_index(d.g, *pattern); p != SIZE_MAX) {
            deduced_type& slot = d.found[p];
            if (!slot.type) {
                slot = {actual, d.argument};
            } else if (slot.argument != 0 && slot.type->canonical_name() != actual->canonical_name()) {
                d.error = "cannot deduce " + std::string(d.g.parameters[p].name) + " for '" + std::string(d.g.name) + "': it is " + slot.type->canonical_name() +
                          " from argument " + std::to_string(slot.argument) + " and " + actual->canonical_name() + " from argument " + std::to_string(d.argument);
            }
            return;
        }
        switch (pattern->base_type) {
            case script_value_type::jai_array_type: case script_value_type::jai_map_type: case script_value_type::jai_weak_ptr_type:
                if (actual->base_type != pattern->base_type) { return; }
                for (size_t i = 0; i < pattern->type_params.size() && i < actual->type_params.size(); ++i) {
                    unify(d, pattern->type_params[i].get(), actual->type_params[i].get(), depth + 1);
                }
                return;
            case script_value_type::jai_shared_ptr_type:
                if (actual->base_type != script_value_type::jai_shared_ptr_type || pattern->type_params.empty()) { return; }
                if (!actual->type_params.empty() && actual->type_params[0]) { unify(d, pattern->type_params[0].get(), actual->type_params[0].get(), depth + 1); }
                else if (!actual->type_name.empty() && actual->type_name != "shared_ptr") { unify(d, pattern->type_params[0].get(), eng_.get_type_info_object(actual->type_name), depth + 1); }
                return;
            case script_value_type::jai_function_type:   // function<U(T)> against a signature: the result, then each parameter
                if (!pattern->is_signature() || !actual->is_signature() || pattern->type_params.size() != actual->type_params.size()) { return; }
                for (size_t i = 0; i < pattern->type_params.size(); ++i) {
                    const type_info* named = actual->type_params[i].get();
                    if (i == 0 && (!named || named->base_type == script_value_type::jai_null_type)) { continue; }   // no result names no type
                    unify(d, pattern->type_params[i].get(), named, depth + 1);
                }
                return;
            case script_value_type::jai_object_type: {
                if (pattern->is_coroutine) {   // coroutine<T> against a coroutine's handle
                    if (actual->is_coroutine && !pattern->type_params.empty() && !actual->type_params.empty()) {
                        unify(d, pattern->type_params[0].get(), actual->type_params[0].get(), depth + 1);
                    }
                    return;
                }
                if (pattern->type_params.empty() || actual->base_type != script_value_type::jai_object_type) { return; }
                const std::string_view generic = std::string_view(pattern->type_name).substr(0, pattern->type_name.find('<'));
                std::vector<type_info_ptr> arguments;
                if (!instance_arguments(actual->type_name, generic, arguments)) { return; }
                for (size_t i = 0; i < pattern->type_params.size() && i < arguments.size(); ++i) {
                    unify(d, pattern->type_params[i].get(), arguments[i].get(), depth + 1);
                }
                return;
            }
            default:
                return;
        }
    }
    // A lambda's parameter that declares no type of its own: a bare name or auto, typed by the signature it is checked against
    static bool arrow_parameter(const parameter& p) { return !p.is_reference && (p.untyped || !p.type || p.contextual); }
    static bool untyped_arrow(const expression* e) {
        if (!e || e->get_type() != node_type::lambda_expr) { return false; }
        const auto& parameters = static_cast<const lambda_expr*>(e)->parameters;
        return std::any_of(parameters.begin(), parameters.end(), arrow_parameter);
    }
    // The signature a function value declares, which a deduction reads function<R(A...)> against: a slot's own, or a lambda's
    // or a function's whose every parameter declares its type, with the result it declares (a lambda's: the one type its
    // returns yield). Null for a callable that declares no one signature: an overloaded name, a `function` slot, an arrow.
    const type_info* declared_signature(const ctype& arg) {
        if (arg.ti && arg.ti->is_signature()) { return arg.ti; }
        const std::vector<parameter>* parameters = nullptr;
        const type_info* result = nullptr;
        if (arg.origin && arg.origin->get_type() == node_type::lambda_expr) {
            const auto* l = static_cast<const lambda_expr*>(arg.origin);
            parameters = &l->parameters;
            result = l->return_type.get();
            if (!result) { result = lambda_yield(l); }
        } else if (arg.fns && arg.fns->size() == 1) {
            const function_decl* fn = arg.fns->front();
            parameters = &fn->parameters;
            result = fn->is_coroutine && fn->return_type ? eng_.get_type_info_coroutine(fn->return_type.get()) : fn->return_type.get();
        }
        if (!parameters || !result) { return nullptr; }
        std::vector<type_info*> taken;
        for (const parameter& p : *parameters) {
            if (!p.type || p.untyped || p.contextual) { return nullptr; }
            taken.push_back(p.is_reference ? eng_.get_type_info_reference(p.type.get()) : p.type.get());
        }
        return eng_.get_type_info_function(const_cast<type_info*>(result), taken);
    }
    // `pattern` with each type parameter the deduction has found in its place; null while it names one not found yet, or an
    // instance the unit does not declare
    const type_info* substituted(const deduction& d, const type_info* pattern, int depth = 0) {
        if (!pattern || depth > 64) { return nullptr; }
        if (const size_t p = type_parameter_index(d.g, *pattern); p != SIZE_MAX) { return d.found[p].type; }
        if (pattern->type_params.empty()) { return pattern; }
        std::vector<type_info*> inner;
        for (const auto& parameter : pattern->type_params) {
            const type_info* made = substituted(d, parameter.get(), depth + 1);
            if (!made) { return nullptr; }
            inner.push_back(const_cast<type_info*>(made));
        }
        switch (pattern->base_type) {
            case script_value_type::jai_array_type: return eng_.get_type_info_array(inner[0]);
            case script_value_type::jai_map_type: return inner.size() == 2 ? eng_.get_type_info_map(inner[0], inner[1]) : nullptr;
            case script_value_type::jai_shared_ptr_type: return eng_.get_type_info_shared_ptr(inner[0]);
            case script_value_type::jai_reference_type: return eng_.get_type_info_reference(inner[0]);
            case script_value_type::jai_function_type: return eng_.get_type_info_function(inner[0], std::vector<type_info*>(inner.begin() + 1, inner.end()));
            case script_value_type::jai_object_type: {
                if (pattern->is_coroutine) { return eng_.get_type_info_coroutine(inner[0]); }   // its name holds what it yields: made, never edited
                const std::string instance = generic_instance_name(std::string_view(pattern->type_name).substr(0, pattern->type_name.find('<')),
                                                                   std::vector<type_info_ptr>(inner.begin(), inner.end()));
                return known_class(instance) ? eng_.get_type_info_object(instance) : nullptr;
            }
            default: return nullptr;
        }
    }
    // A name in a deduced type that a host template's argument spelled and no script type reads as: no class, no enum and no
    // sized number (sized_number_kind, which an instance reads as int or float)
    std::string host_only_name(const type_info* type, int depth = 0) {
        if (!type || depth > 64) { return {}; }
        if (type->base_type == script_value_type::jai_object_type && !type->is_coroutine) {
            return unknown_bare_type(type) && sized_number_kind(type->type_name) == script_value_type::jai_null_type ? type->type_name : std::string();
        }
        for (const auto& parameter : type->type_params) {
            if (std::string found = host_only_name(parameter.get(), depth + 1); !found.empty()) { return found; }
        }
        return {};
    }
    // The type arguments a call leaves to its value arguments: `types` holds the written ones and takes the rest. False with
    // `error` saying what to write.
    bool deduce_type_arguments(const call_expr* c, const generic_definition& g, const std::vector<ctype>& args, std::vector<type_info_ptr>& types, std::string& error) {
        deduction d{g, std::vector<deduced_type>(g.parameters.size()), {}, 0};
        for (size_t i = 0; i < types.size(); ++i) { d.found[i] = {types[i].get(), 0}; }
        std::vector<const type_info*> actual(args.size(), nullptr);
        std::vector<size_t> arrows;   // the lambdas that take their parameters' types from what the other arguments find
        const auto bare = [](const type_info* t) {
            while (t && t->base_type == script_value_type::jai_reference_type && !t->type_params.empty()) { t = t->type_params[0].get(); }
            return t;
        };
        for (size_t i = 0; i < args.size() && i < g.values.size(); ++i) {
            if (args[i].kind == ctype::null_k) { continue; }   // a null says nothing of a type
            if (args[i].kind == ctype::function_k) {   // nor does a function value, but through the signature it declares
                const type_info* pattern = bare(g.values[i].type.get());
                const bool through = pattern && pattern->is_signature();
                if (!through && !(args[i].ti && args[i].ti->is_signature())) { continue; }
                actual[i] = declared_signature(args[i]);
                if (!actual[i]) {
                    if (through && untyped_arrow(args[i].origin)) { arrows.push_back(i); }
                    continue;
                }
            } else {
                actual[i] = deduction_type(args[i]);
                if (!actual[i] && args[i].is_unknown()) {   // a reference names what it was declared to name
                    actual[i] = deduction_type(referent_receiver(c->arguments[c->type_argument_count + i].get()));
                }
            }
            d.argument = i + 1;
            unify(d, g.values[i].type.get(), actual[i]);
            if (!d.error.empty()) { error = std::move(d.error); return false; }
        }
        // An arrow is read last: each parameter it leaves untyped takes the type the signature gives it with what is found so
        // far, the arrow is checked so, and its result then names what the signature's result does
        for (const size_t i : arrows) {
            expression* written = c->arguments[c->type_argument_count + i].get();
            auto* l = static_cast<lambda_expr*>(written);
            const type_info* pattern = bare(g.values[i].type.get());
            if (pattern->type_params.size() - 1 != l->parameters.size()) { continue; }
            d.argument = i + 1;
            std::vector<type_info*> taken;
            for (size_t k = 0; k < l->parameters.size(); ++k) {
                const parameter& p = l->parameters[k];
                const type_info* wanted = pattern->type_params[k + 1].get();
                const type_info* found = nullptr;
                if (arrow_parameter(p)) { found = substituted(d, bare(wanted)); } else { unify(d, wanted, p.type.get()); }
                taken.push_back(found ? const_cast<type_info*>(found) : eng_.get_type_info_any());
            }
            if (!d.error.empty()) { error = std::move(d.error); return false; }
            infer_expecting(written, eng_.get_type_info_function(eng_.get_type_info_any(), taken));
            const type_info* result = l->return_type.get();
            if (!result) { result = lambda_yield(l); }
            if (result && result->base_type != script_value_type::jai_null_type) { unify(d, pattern->type_params[0].get(), result); }
            if (!d.error.empty()) { error = std::move(d.error); return false; }
        }
        const size_t given = c->type_argument_count;
        const auto spelled = [&](std::string_view parameter) {   // name<string, V>(...): what to write, the found arguments filled in
            std::string text = "write " + std::string(g.name) + "<";
            for (size_t p = 0; p < g.parameters.size(); ++p) {
                text += p ? ", " : "";
                text += d.found[p].type && g.parameters[p].name != parameter ? d.found[p].type->canonical_name() : std::string(g.parameters[p].name);
            }
            return text + (c->arguments.size() == given ? ">()" : ">(...)");
        };
        for (size_t p = types.size(); p < g.parameters.size(); ++p) {
            const std::string_view name = g.parameters[p].name;
            const std::string lead = "cannot deduce " + std::string(name) + " for '" + std::string(g.name) + "': ";
            if (d.found[p].type) {
                std::vector<token> probe;
                if (!generic_argument_tokens(*d.found[p].type, c->location, eng_.symbols(), probe)) {
                    error = lead + "argument " + std::to_string(d.found[p].argument) + " is " + d.found[p].type->canonical_name() + ", which no instance is made with; " + spelled(name);
                    return false;
                }
                // a name the host spelled for its template's argument is a type only there: the instance must not read it
                if (const std::string host = host_only_name(d.found[p].type); !host.empty() && generic_parameter_read(g, p, eng_.get_registered_template_types())) {
                    error = lead + "argument " + std::to_string(d.found[p].argument) + " names '" + host + "', a type the host binds that no script type reads as";
                    return false;
                }
                types.push_back(type_info_ptr(const_cast<type_info*>(d.found[p].type)));
                continue;
            }
            size_t named_by = 0;   // the first value parameter whose type names it, counted from 1
            for (size_t i = 0; i < g.values.size() && named_by == 0; ++i) { if (names_type_parameter(g, g.values[i].type.get(), p)) { named_by = i + 1; } }
            if (named_by == 0) {
                error = lead + "no parameter names it; " + spelled(name);
            } else if (named_by > args.size()) {
                error = lead + "argument " + std::to_string(named_by) + " is left to its default; " + spelled(name);
            } else if (args[named_by - 1].kind == ctype::null_k || (args[named_by - 1].kind == ctype::function_k && !actual[named_by - 1])) {
                error = lead + "argument " + std::to_string(named_by) + " is " + (args[named_by - 1].kind == ctype::null_k ? "null" : "a function") + ", which says nothing of a type; " + spelled(name);
            } else if (!actual[named_by - 1]) {
                error = lead + "argument " + std::to_string(named_by) + "'s type is not known at load; " + spelled(name);
            } else {
                error = lead + "argument " + std::to_string(named_by) + " is " + actual[named_by - 1]->canonical_name() + ", which '" +
                        g.values[named_by - 1].type->canonical_name() + "' does not match; " + spelled(name);
            }
            return false;
        }
        return true;
    }

    enum class generic_call : uint8_t { none, rewritten, pending };
    // A call whose callee names a generic is its instance's call: the callee takes the instance's name (largest<int>) and the
    // type arguments leave the argument list, once the unit declares that instance; until then the call asks for it and types
    // nothing (the engine declares it and checks the unit again)
    generic_call resolve_generic_call(call_expr* c, std::vector<ctype>& args) {
        if (!generics_live_ || !c->callee) { return generic_call::none; }
        bool receiver_unknown = false;
        std::vector<generic_definition_ptr> candidates = generic_callee(c, receiver_unknown);
        const size_t given = c->type_argument_count;
        const size_t values = c->arguments.size() - given;
        if (candidates.empty()) {
            if (receiver_unknown && given > 0) {
                auto* m = static_cast<const member_expr*>(c->callee.get());
                std::vector<type_info_ptr> spelled;
                for (size_t i = 0; i < given; ++i) { spelled.push_back(static_cast<const cast_expr*>(c->arguments[i].get())->target); }
                diag(diag_level::error, c->location, "cannot instantiate '" + generic_instance_name(m->member, spelled) + "': the receiver's class is not known at load");
                return generic_call::pending;
            }
            return generic_call::none;
        }
        const bool is_class = candidates.front()->kind == generic_kind::class_type;
        const std::string shown(candidates.front()->name);
        if (is_class && given == 0) {
            std::string parameters;
            for (size_t i = 0; i < candidates.front()->parameters.size(); ++i) { parameters += (i ? ", " : "") + std::string(candidates.front()->parameters[i].name); }
            diag(diag_level::error, c->location, "'" + shown + "' is a generic class: its type arguments are written, never deduced from a constructor's (" +
                 shown + "<" + parameters + ">(...))");
            return generic_call::pending;
        }
        // an ordinary function or method of the name that takes the call keeps it
        if (!is_class && given == 0 && ordinary_callee_fits(c, args)) { return generic_call::none; }
        std::vector<generic_definition_ptr> fitting;
        std::vector<std::vector<type_info_ptr>> fitting_types;
        std::string deduction_error;
        bool counted = false;
        for (const auto& g : candidates) {
            const bool counts = is_class ? g->parameters.size() == given : g->parameters.size() >= given;
            if (!counts || (!is_class && (values < g->required_values() || values > g->values.size()))) { continue; }
            counted = true;
            std::vector<type_info_ptr> types;
            for (size_t i = 0; i < given; ++i) { types.push_back(static_cast<const cast_expr*>(c->arguments[i].get())->target); }
            if (types.size() < g->parameters.size()) {
                const std::vector<ctype> value_args(args.begin() + static_cast<std::ptrdiff_t>(given), args.end());
                std::string error;
                if (!deduce_type_arguments(c, *g, value_args, types, error)) {
                    if (deduction_error.empty()) { deduction_error = std::move(error); }
                    continue;
                }
            }
            fitting.push_back(g);
            fitting_types.push_back(std::move(types));
        }
        if (fitting.empty() && counted) {
            diag(diag_level::error, c->location, deduction_error);
            return generic_call::pending;
        }
        if (fitting.empty()) {
            const generic_definition& g = *candidates.front();
            if (is_class ? g.parameters.size() != given : g.parameters.size() < given) {
                diag(diag_level::error, c->location, "'" + shown + "' takes " + std::to_string(g.parameters.size()) + (g.parameters.size() == 1 ? " type argument" : " type arguments") +
                     ", not " + std::to_string(given));
            } else {
                diag(diag_level::error, c->location, "'" + generic_spelling(g) + "' takes " + arity_text(g) + ", not " + std::to_string(values));
            }
            return generic_call::pending;
        }
        // Generics that take the same type arguments name one instance, each its own overload of that name: the call is that
        // name's, and its value arguments pick among the overloads as among functions written by hand. Generics that name
        // different instances cannot both take the call.
        const generic_definition_ptr& g = fitting.front();
        const std::string instance = generic_instance_name(g->qualified_name(), fitting_types.front());
        for (size_t i = 1; i < fitting.size(); ++i) {
            if (generic_instance_name(fitting[i]->qualified_name(), fitting_types[i]) == instance) { continue; }
            diag(diag_level::error, c->location, "call to '" + shown + "' is ambiguous: the generics '" + generic_spelling(*fitting[0]) + "' (" +
                 fitting[0]->location.to_string() + ") and '" + generic_spelling(*fitting[i]) + "' (" + fitting[i]->location.to_string() + ") both take it");
            return generic_call::pending;
        }
        bool declared = true;
        for (size_t i = fitting.size(); i-- > 0;) {   // the last first: each request stands ahead of the unit's others, so the overloads stand as declared
            const generic_definition_ptr& overload = fitting[i];
            const generic_instance_id asked{instance, generic_instance_stamp(*overload)};
            const generic_instance_decl* standing = instance_request(asked);
            const bool made = instance_declared(*overload, instance);
            declared = declared && made;
            // asked for when the unit does not declare it, and when the unit's own text names an instance it was only making
            // again (the instance is the unit's from here)
            const bool claims = remade_root(standing) && !remade_root(instance_ctx_);
            if (!generic_requests_ || (made ? !claims : standing && !claims) || !requested_now_.emplace(instance, asked.stamp).second) { continue; }
            generic_request request;
            request.name = instance;
            request.generic = overload->qualified_name();
            request.arguments = fitting_types[i];
            request.definition = overload;
            request.at = c->location;
            request.within = instance_ctx_ ? std::string(instance_ctx_->name) : std::string();
            request.within_stamp = instance_ctx_ ? instance_ctx_->stamp : 0;
            generic_requests_->push_back(std::move(request));
        }
        if (!declared) { return generic_call::pending; }
        // the name the instance declares: what follows its generic's namespace; a method's in full (Base::tag<int>), which is
        // what keeps a generic method from being virtual
        const bool bare = g->kind == generic_kind::method || g->space.empty();
        std::string_view simple;
        const uint64_t id = eng_.intern(bare ? std::string_view(instance) : std::string_view(instance).substr(g->space.size() + 2), simple);
        if (c->callee->get_type() == node_type::identifier_expr) {
            auto* was = static_cast<identifier_expr*>(c->callee.get());
            auto named = std::make_shared<identifier_expr>(was->location, simple, id);
            named->global_qualified = was->global_qualified;
            c->callee = std::move(named);
        } else {
            auto* was = static_cast<member_expr*>(c->callee.get());
            auto named = std::make_shared<member_expr>(was->location, was->object, simple, id, was->is_arrow, was->is_static);
            named->null_safe = was->null_safe;
            named->call_result_discarded = was->call_result_discarded;
            c->callee = std::move(named);
        }
        c->arguments.erase(c->arguments.begin(), c->arguments.begin() + static_cast<std::ptrdiff_t>(given));
        args.erase(args.begin(), args.begin() + static_cast<std::ptrdiff_t>(given));
        c->type_argument_count = 0;
        return generic_call::rewritten;
    }
    static std::string arity_text(const generic_definition& g) {
        const size_t least = g.required_values(), most = g.values.size();
        return (least == most ? std::to_string(most) : std::to_string(least) + " to " + std::to_string(most)) + (most == 1 ? " argument" : " arguments");
    }
    // The script methods a class holds under a name, its own or the nearest base's that declares one: a class the load
    // models by its model, a class an earlier unit declared by the declarations the engine holds of it
    std::vector<const function_decl*> class_methods_named(std::string_view cls, std::string_view member, bool want_static, int depth = 0) {
        if (depth > 32 || cls.empty()) { return {}; }
        if (const sclass* c = find_sclass(cls)) {
            if (auto held = (want_static ? c->static_methods : c->methods).find(member); held != (want_static ? c->static_methods : c->methods).end()) { return held->second; }
            if (!want_static) {   // an object and a method's bare name reach a static too
                if (auto held = c->static_methods.find(member); held != c->static_methods.end()) { return held->second; }
            }
            for (auto base : c->decl->base_classes) {
                auto found = class_methods_named(base, member, want_static, depth + 1);
                if (!found.empty()) { return found; }
            }
            return {};
        }
        const auto def = engine_class(cls);
        if (!def) { return {}; }
        std::vector<const function_decl*> found;
        const uint64_t id = eng_.symbolize(member);
        const auto take = [&](const std::unordered_map<uint64_t, std::vector<std::shared_ptr<function_decl>>>& table) {
            if (const auto held = table.find(id); held != table.end()) { for (const auto& fn : held->second) { if (fn) { found.push_back(fn.get()); } } }
        };
        take(want_static ? def->script_static_method_overloads() : def->script_method_overloads());
        if (found.empty() && !want_static) { take(def->script_static_method_overloads()); }
        for (const auto& parent : def->get_parent_classes()) {
            if (!found.empty()) { break; }
            found = class_methods_named(parent->get_name(), member, want_static, depth + 1);
        }
        return found;
    }
    // A namespace's functions of a name: every declaration a call of space::member may reach, typed or not
    std::vector<const function_decl*> namespace_functions_named(const std::string& space, std::string_view member) {
        namespace_overloads fns = namespace_function_overloads(space, member);
        fns.typed.insert(fns.typed.end(), fns.untyped.begin(), fns.untyped.end());
        return std::move(fns.typed);
    }
    // Whether the ordinary declarations a call's callee names include one that takes these arguments, by the lookup an
    // ordinary call of that spelling takes (a method of the class, a function of an enclosing namespace, a program-scope
    // function). The declarations are this unit's and those the rest of the program holds under the name: another unit of
    // the closure's, an earlier run's.
    bool ordinary_callee_fits(call_expr* c, const std::vector<ctype>& args) {
        std::vector<const function_decl*> overloads;
        expression* callee = c->callee.get();
        if (callee->get_type() == node_type::identifier_expr) {
            auto* id = static_cast<identifier_expr*>(callee);
            if (class_ctx_ && !id->global_qualified) { overloads = class_methods_named(class_ctx_name_, id->name, /*want_static=*/false); }
            if (overloads.empty() && !id->global_qualified) {
                if (const std::string space = enclosing_namespace_member(id->name); !space.empty()) { overloads = namespace_functions_named(space, id->name); }
            }
            if (overloads.empty()) {
                static const std::vector<const function_decl*> none;
                const auto own = functions_.find(id->name);
                const std::vector<const function_decl*>& declared = own != functions_.end() ? own->second : none;
                overloads = declared;
                const auto& foreign = foreign_overloads(id->name, declared);
                overloads.insert(overloads.end(), foreign.begin(), foreign.end());
            }
        } else if (callee->get_type() == node_type::member_expr) {
            auto* m = static_cast<member_expr*>(callee);
            if (m->is_static) {
                const std::string qualified = qualified_spelling(m);
                if (qualified.size() > m->member.size() + 2) {
                    const std::string owner = qualified.substr(0, qualified.size() - m->member.size() - 2);
                    overloads = find_sclass(owner) || engine_class(owner) ? class_methods_named(owner, m->member, /*want_static=*/true)
                                                                          : namespace_functions_named(owner, m->member);
                }
            } else {
                bool unknown = false;
                overloads = class_methods_named(receiver_class(m, unknown), m->member, /*want_static=*/false);
            }
        }
        return std::any_of(overloads.begin(), overloads.end(), [&](const function_decl* fn) { return script_fn_viable(fn, args, overloads.size() > 1); });
    }

    // C++ [basic.scope]: a block, a parameter list or one unit's top level declares a name once; a later run of the program
    // declares it again (each run is checked alone), and so does the definition of an extern or an inline declaration. C++26's
    // placeholder: a later '_' that names a local variable or a structured binding (name_independent) redeclares freely, and
    // the name then names none of them
    void declare_local(std::string_view name, var_entry entry, const source_location& at, bool name_independent = false) {
        entry.type.fns = nullptr;
        auto [held, fresh] = scopes_.back().emplace(name, entry);
        if (fresh) { return; }
        if (name == "_" && (name_independent || scopes_.size() == 1)) {
            held->second = std::move(entry);
            held->second.placeholders = true;
            return;
        }
        if (scopes_.size() > 1 || !(held->second.shared_name || entry.shared_name)) {
            diag(diag_level::error, at, "redeclaration of '" + std::string(name) + "' in the same scope", held->second.line, held->second.column);
        }
        if (scopes_.size() == 1) { held->second = std::move(entry); }   // the later declaration stands for the name from here on, as it does at run time
    }

    // C++ [class.mem]: a class declares a member name once (methods overload with one another, never with a field)
    void check_member_names(const class_decl* cd) {
        std::unordered_map<std::string_view, const declaration*> fields;
        std::unordered_map<std::string_view, const declaration*> methods;
        for (const auto& m : cd->members) {
            if (!m.declaration) { continue; }
            const declaration* d = m.declaration.get();
            if (d->get_type() == node_type::variable_decl) {
                const std::string_view name = static_cast<const variable_decl*>(d)->name;
                const declaration* earlier = fields.count(name) ? fields[name] : methods.count(name) ? methods[name] : nullptr;
                if (earlier) {
                    diag(diag_level::error, d->location, "redeclaration of member '" + std::string(name) + "' in class '" + std::string(cd->name) + "'",
                         earlier->location);
                } else {
                    fields.emplace(name, d);
                }
            } else if (d->get_type() == node_type::function_decl) {
                const std::string_view name = static_cast<const function_decl*>(d)->name;
                if (auto field = fields.find(name); field != fields.end()) {
                    diag(diag_level::error, d->location, "redeclaration of member '" + std::string(name) + "' in class '" + std::string(cd->name) + "'",
                         field->second->location);
                }
                methods.emplace(name, d);
            }
        }
    }

    // C++ [basic.namespace]: a namespace names no object, so its name alone is no expression (the run time finds no variable of it)
    void namespace_as_value(const source_location& loc, std::string_view name) {
        const std::string ns(name);
        diag(has_dynamic_includes_ ? diag_level::warning : diag_level::error, loc,
             "'" + ns + "' is a namespace, not a value; name one of its members (" + ns + "::name)");
    }

    void undeclared(const source_location& loc, std::string_view name, bool less_site = false) {
        if (instance_requests_.count(name)) { return; }   // an instance that could not be made: its request says why
        if (const std::string generic = generic_named_as_value(name); !generic.empty()) {
            diag(diag_level::error, loc, generic);
            return;
        }
        // Literal imports are covered by the program closure's names; a dynamic path can
        // define anything - keep the signal, drop the teeth.
        diag(has_dynamic_includes_ ? diag_level::warning : diag_level::error, loc,
             "use of undeclared identifier '" + std::string(name) + "'" + (less_site ? " (" + no_generic_text(name) + ")" : std::string()));
    }
    // what a diagnostic at the head of a '<' site that compares adds: a call with type arguments was perhaps meant
    static std::string no_generic_text(std::string_view name) {
        return "no generic '" + std::string(name) + "' is declared: '<' after a name that takes no type arguments compares";
    }

    // -------------------------------------------------------------- collection

    void collect_names(const statement* node);        // safety-net name sweep (recursive)
    void collect_names_expr(const expression* e);     // lambdas declare names inside expressions

    // One entry per signature: a same-signature redefinition (the error the redefinition pass reports, or an `override` that
    // replaces on purpose) takes the earlier one's place, so calls see one candidate
    static void add_overload(std::vector<const function_decl*>& set, const function_decl* fn) {
        for (auto& held : set) {
            if (detail::same_signature(held->parameters, fn->parameters)) { held = fn; return; }
        }
        set.push_back(fn);
    }

    // THE class model - same-unit classes and program-table classes are built by this one walk.
    static void model_class(const class_decl* cd, sclass& c) {
        c = sclass{};   // redefinition: last one wins, don't merge members
        c.decl = cd;
        for (const auto& m : cd->members) {
            if (!m.declaration) { continue; }
            const uint8_t access = m.visibility == class_decl::Private ? 1 : m.visibility == class_decl::Protected ? 2 : 0;
            if (m.declaration->get_type() == node_type::variable_decl) {
                auto* f = static_cast<const variable_decl*>(m.declaration.get());
                (f->is_static ? c.static_fields : c.fields).emplace(f->name, f);
                if (access) { c.nonpublic[f->name] = access; }
            } else if (m.declaration->get_type() == node_type::function_decl) {
                auto* fn = static_cast<const function_decl*>(m.declaration.get());
                if (access && fn->name != cd->name) { c.nonpublic[fn->name] = access; }
                if (fn->name == cd->name) {
                    add_overload(c.ctors, fn);
                    if (access) { c.private_ctors.push_back(fn); }
                } else if (fn->is_static) {
                    add_overload(c.static_methods[fn->name], fn);
                } else {
                    add_overload(c.methods[fn->name], fn);
                }
            }
        }
    }

    void collect_class(const class_decl* cd) {
        if (cd->method_instances) { return; }   // no class of its own: its members join the class it names
        sclass& c = classes_[cd->name];
        model_class(cd, c);
        attach_method_instances(c);
    }
    // the instances of a class's generic methods this unit declares are members of its model
    void attach_method_instances(sclass& c) {
        for (const auto& [name, instance] : method_instances_) {
            if (instance.owner != c.decl->name) { continue; }
            for (const function_decl* fn : instance.fns) {
                add_overload((instance.is_static ? c.static_methods : c.methods)[fn->name], fn);
                if (instance.access) { c.nonpublic[fn->name] = instance.access; }
            }
        }
    }

    // The unit's own class first, then the program table (modeled once per check; a table
    // class the engine holds under a different shape is open like a same-unit redefinition).
    sclass* find_sclass(std::string_view cls) {
        if (auto it = classes_.find(cls); it != classes_.end()) { return &it->second; }
        eng_.note(cls);
        if (auto it = program_sclasses_.find(cls); it != program_sclasses_.end()) { return &it->second; }
        if (!program_classes_) { return nullptr; }
        auto pit = program_classes_->find(std::string(cls));
        if (pit == program_classes_->end() || !pit->second) { return nullptr; }
        const class_decl* cd = pit->second.get();
        sclass& c = program_sclasses_[cd->name];
        model_class(cd, c);
        attach_method_instances(c);
        c.foreign = true;
        c.provable = engine_class(cd->name) != nullptr;
        if (!c.provable) { deferred_ = true; if (deferred_out_) { deferred_out_->push_back(std::string(cd->name)); } }
        if (unit_redefines(cd)) { open_classes_.insert(cd->name); }
        return &c;
    }

    // The proof pass alone, stamping or clearing (never a diagnostics walk), types a top-level global exactly one other unit declares
    // once with a spelled non-reference type; a class-typed one needs its class defined, or the unit's proofs wait for the next import (deferred_)
    const variable_decl* program_global(const std::string& name) {
        if (!semantic_walk() || !program_globals_) { return nullptr; }
        eng_.note(name);
        auto it = program_globals_->find(name);
        if (it == program_globals_->end() || it->second.size() != 1 || it->second.begin()->second.size() != 1) { return nullptr; }
        const variable_decl* g = it->second.begin()->second.front().get();
        if (!g || !g->type || g->type->is_reference() || g->function_spelling || g->is_static) { return nullptr; }
        return g;
    }
    // The declared type a store to a program-scope name must respect when this unit does not declare it: the engine's cell
    // once a unit has run (typed off its tag), else the one spelled declaration another unit of the program makes
    ctype program_scope_store_type(const std::string& name) {
        if (instance_names_.count(name)) { return {}; }
        if (eng_.has_variable(name)) { return engine_global_type(name); }
        if (!program_globals_) { return {}; }
        auto it = program_globals_->find(name);
        if (it == program_globals_->end() || it->second.size() != 1 || it->second.begin()->second.size() != 1) { return {}; }
        const variable_decl* g = it->second.begin()->second.front().get();
        if (!g || !g->type || g->type->is_reference() || g->function_spelling || g->is_static) { return {}; }
        ctype t = ctype_from_type_info(g->type.get());
        if (t.kind == ctype::object_k && !find_sclass(t.cls) && !engine_class(t.cls)) { return {}; }
        t.tentative = true;
        return t;
    }
    // A program-scope variable this unit does not declare, as a read of it types for diagnostics: the one spelled declaration
    // another unit of the program makes (a reload's dependents read its new text there, never the engine's cell of the old one);
    // scalars, text, containers and math values only (a class slot may hold a subclass)
    ctype program_scope_read_type(const std::string& name) {
        if (!collect_diagnostics_ || has_dynamic_includes_ || instance_names_.count(name) || extra_globals_.count(name)) { return {}; }
        ctype t;
        eng_.note(name);
        if (program_globals_) {
            if (auto it = program_globals_->find(name); it != program_globals_->end()) {
                size_t declarations = 0;
                const variable_decl* g = nullptr;
                for (const auto& [path, decls] : it->second) {
                    declarations += decls.size();
                    if (!decls.empty()) { g = decls.front().get(); }
                }
                if (declarations > 1) { return {}; }
                if (declarations == 1) {
                    if (!g || !g->type || g->type->is_reference() || g->function_spelling || g->is_static) { return {}; }
                    t = ctype_from_type_info(g->type.get());
                }
            }
        }
        switch (t.kind) {
            case ctype::int_k: case ctype::float_k: case ctype::bool_k: case ctype::char_k: case ctype::string_k: case ctype::utf8_k:
            case ctype::array_k: case ctype::map_k: case ctype::math_k:
                t.tentative = true;
                return t;
            default: return {};
        }
    }
    // A free function this unit does not declare, as a call of it checks: the one declaration exactly one other unit of the
    // program makes. A declaration the engine holds from that unit's earlier run is the one the table's replaces (a reload's
    // dependents check against the new text); any other holder of the name (another unit's run, the host) leaves the call unchecked.
    const function_decl* program_function(std::string_view name) {
        if (!collect_diagnostics_ || has_dynamic_includes_ || !program_functions_ || functions_.count(name)) { return nullptr; }
        eng_.note(name);
        auto it = program_functions_->find(std::string(name));
        if (it == program_functions_->end()) { return nullptr; }
        const function_decl* fn = nullptr;
        const std::string* unit_path = nullptr;
        size_t declarations = 0;
        for (const auto& [path, decls] : it->second) {
            declarations += decls.size();
            if (!decls.empty()) { fn = decls.front().get(); unit_path = &path; }
        }
        if (declarations != 1 || !fn || fn->is_coroutine) { return nullptr; }
        const std::string key(name);
        if (program_globals_) {
            if (auto g = program_globals_->find(key); g != program_globals_->end()) {
                for (const auto& [path, decls] : g->second) { if (!decls.empty()) { return nullptr; } }
            }
        }
        if (find_sclass(name) || eng_.is_type_name(key)) { return nullptr; }
        const auto held = eng_.held_function_declarations(name);
        if (held.empty()) { return eng_.has_function(key) || !engine_variable_only_from(key, *unit_path) ? nullptr : fn; }
        const std::string normal = std::filesystem::path(*unit_path).lexically_normal().string();
        for (const auto& h : held) {
            if (!h || eng_.unit_label(h->unit.id) != normal) { return nullptr; }
        }
        return fn;
    }
    // A call of a script function the engine holds from an earlier run, which no unit of the program declares: an error only
    // when no held declaration takes it
    void check_held_script_call(std::string_view name, const std::vector<ctype>& args, const source_location& loc) {
        if (!collect_diagnostics_ || has_dynamic_includes_) { return; }
        const auto held = eng_.held_function_declarations(name);
        if (held.empty()) { return; }
        std::vector<const function_decl*> decls;
        for (const auto& fn : held) {
            if (!fn) { return; }
            auto spelled = std::make_unique<function_decl>(source_location{}, name);
            spelled->parameters = fn->parameters();
            spelled->return_type = fn->return_type;
            spelled->body = fn->body;
            decls.push_back(spelled.get());
            held_declarations_.push_back(std::move(spelled));
        }
        const bool rivals = arity_rivals(decls, args.size());
        for (const auto* fn : decls) { if (script_fn_viable(fn, args, rivals)) { return; } }
        report_no_overload(name, decls, args, loc, /*foreign=*/true);
    }
    // A bare name in a method resolves locals, then members, then program-scope (and namespace) variables: a member of the
    // class being walked hides a program-scope name, as at run time
    bool member_hides_program_name(std::string_view name, size_t scope_index) {
        if (!class_ctx_) { return false; }
        const bool program_scope = scope_index == 0 ||
            std::find(namespace_scopes_.begin(), namespace_scopes_.end(), scope_index) != namespace_scopes_.end();
        return program_scope && lookup_member(class_ctx_name_, name, /*want_static=*/false).found;
    }
    // A bare name's entry in the walk's scopes is the variable it names unless a member of the class being walked or a nearer
    // namespace's member takes the name first; a namespace's function is its member wherever its body declared it
    bool names_variable(const var_entry* local, std::string_view name, size_t scope_index) {
        return local && !local->namespace_function && !member_hides_program_name(name, scope_index) && !namespace_hides_program_name(name, scope_index);
    }
    // a bare name nothing nearer than the program scope takes: no local, no member of the class being walked, no namespace's member
    bool reaches_program_scope(const identifier_expr* id) {
        if (id->global_qualified) { return true; }
        size_t scope_index = 0;
        if (const var_entry* local = find_local(id->name, &scope_index); scope_index != 0 && names_variable(local, id->name, scope_index)) { return false; }
        if (class_ctx_) {
            const member_hit hit = lookup_member(class_ctx_name_, id->name, /*want_static=*/false);
            if (hit.found || hit.unresolved) { return false; }
        }
        return enclosing_namespace_member(id->name).empty();
    }
    // the variable a call's bare callee names (::name: the unit's program scope only)
    const var_entry* callable_variable(const identifier_expr* id, size_t* scope_index) {
        *scope_index = 0;
        if (id->global_qualified) {
            if (scopes_.empty()) { return nullptr; }
            const auto found = scopes_.front().find(id->name);
            return found != scopes_.front().end() ? &found->second : nullptr;
        }
        const var_entry* local = find_local(id->name, scope_index);
        return names_variable(local, id->name, *scope_index) ? local : nullptr;
    }
    // ::name reaches the unit's program scope only: locals and fields never shadow it
    var_entry* find_named(const identifier_expr* id, size_t* scope_index = nullptr) {
        if (!id->global_qualified) {
            size_t at = 0;
            var_entry* local = find_local(id->name, &at);
            if (local && !names_variable(local, id->name, at)) { return nullptr; }
            if (scope_index) { *scope_index = at; }
            if (local && local->placeholders) {
                diag(diag_level::error, id->location, "'_' is declared more than once in this scope, so it names none of them: give the variable you use its own name");
                return local;
            }
            if (local && local->lw >= 0 && collect_diagnostics_) { lw_ident_[id] = local->lw; }
            return local;
        }
        if (scope_index) { *scope_index = 0; }
        if (scopes_.empty()) { return nullptr; }
        auto found = scopes_[0].find(id->name);
        return found != scopes_[0].end() ? &found->second : nullptr;
    }
    ctype program_global_type(const variable_decl* g) {
        ctype t = ctype_from_type_info(g->type.get());
        if (t.kind == ctype::object_k) { const sclass* c = find_sclass(t.cls); if (!c || !c->provable) { return {}; } }   // find_sclass noted a program class the engine lacks; a class outside the program types nothing
        return t;
    }

    void collect_scope_decls(const std::vector<declaration_ptr>& decls) {
        for (const auto& d : decls) {
            if (!d) { continue; }
            switch (d->get_type()) {
                case node_type::function_decl:
                    if (!collecting_namespace_) { add_overload(functions_[static_cast<const function_decl*>(d.get())->name], static_cast<const function_decl*>(d.get())); }
                    break;
                case node_type::variable_decl:
                    global_var_names_.insert(static_cast<const variable_decl*>(d.get())->name);
                    if (!collecting_namespace_) { ++top_var_decls_[static_cast<const variable_decl*>(d.get())->name].count; top_var_decls_[static_cast<const variable_decl*>(d.get())->name].decl = static_cast<const variable_decl*>(d.get()); }
                    if (!collecting_namespace_ && static_cast<const variable_decl*>(d.get())->is_const) {
                        top_consts_[static_cast<const variable_decl*>(d.get())->name] = static_cast<const variable_decl*>(d.get());
                    }
                    break;
                case node_type::extern_decl:
                    global_var_names_.insert(static_cast<const extern_decl*>(d.get())->written());
                    break;
                case node_type::destructuring_decl:
                    for (const auto& [name, _] : static_cast<const destructuring_decl*>(d.get())->names) {
                        global_var_names_.insert(name);
                    }
                    break;
                case node_type::class_decl:
                    collect_class(static_cast<const class_decl*>(d.get()));
                    break;
                case node_type::enum_decl: {
                    auto* e = static_cast<const enum_decl*>(d.get());
                    enums_[e->name] = e;
                    break;
                }
                case node_type::namespace_decl: {
                    auto* n = static_cast<const namespace_decl*>(d.get());
                    // `namespace a::b::c` is ONE flat name; every prefix is a path root too
                    namespaces_.insert(n->name);
                    for (size_t pos = n->name.find("::"); pos != std::string_view::npos; pos = n->name.find("::", pos + 2)) {
                        namespaces_.insert(n->name.substr(0, pos));
                    }
                    const bool was_namespace = collecting_namespace_;
                    collecting_namespace_ = true;
                    collect_scope_decls(n->declarations);   // flattened: lenient by design
                    collecting_namespace_ = was_namespace;
                    break;
                }
                default:
                    break;
            }
        }
    }

    static bool literal_unit_path(const statement* d) {
        return d->get_type() == node_type::include_decl
            ? !static_cast<const include_decl*>(d)->path_expr
            : !static_cast<const import_decl*>(d)->path_expr;
    }

    void collect_namespace_vars(const namespace_decl* n, const std::string& prefix) {
        stack_.guard(n->location);
        const std::string path = prefix.empty() ? std::string(n->name) : prefix + "::" + std::string(n->name);
        for (const auto& d : n->declarations) {
            if (!d) { continue; }
            if (d->get_type() == node_type::variable_decl) {
                auto* v = static_cast<const variable_decl*>(d.get());
                auto [it, fresh] = namespace_vars_.try_emplace(path + "::" + std::string(v->name), v);
                if (!fresh && it->second && it->second->type.get() != v->type.get()) { it->second = nullptr; }   // a reopening that retypes it: lenient
            } else if (d->get_type() == node_type::function_decl) {
                auto* f = static_cast<const function_decl*>(d.get());
                namespace_fns_[path + "::" + std::string(f->name)].push_back(f);
            } else if (d->get_type() == node_type::namespace_decl) {
                collect_namespace_vars(static_cast<const namespace_decl*>(d.get()), "");   // a nested block's name is its full path
            }
        }
    }

    // `a::b::x` as the path its member is read from, "a::b" ("" when the chain is not names joined by ::)
    static std::string member_path(const member_expr* m) {
        std::vector<std::string_view> segments;
        const expression* e = m->object.get();
        while (e && e->get_type() == node_type::member_expr) {
            segments.push_back(static_cast<const member_expr*>(e)->member);
            e = static_cast<const member_expr*>(e)->object.get();
        }
        if (!e || e->get_type() != node_type::identifier_expr) { return {}; }
        std::string path(static_cast<const identifier_expr*>(e)->name);
        for (size_t i = segments.size(); i-- > 0;) { path += "::"; path += segments[i]; }
        return path;
    }

    // The declaration of the namespace variable `path::member`: the unit's own (none when its blocks spell two types), else the
    // one another unit of the program makes
    struct namespace_variable_hit {
        const variable_decl* decl = nullptr;
        bool foreign = false;   // another unit's source: its place is not this listing's
    };
    namespace_variable_hit namespace_variable(const std::string& path, std::string_view member) {
        const std::string qualified = path + "::" + std::string(member);
        if (auto it = namespace_vars_.find(qualified); it != namespace_vars_.end()) { return {it->second, false}; }
        if (!program_globals_) { return {}; }
        eng_.note(qualified);
        auto it = program_globals_->find(qualified);
        if (it == program_globals_->end()) { return {}; }
        const variable_decl* only = nullptr;
        size_t declarations = 0;
        for (const auto& [unit, decls] : it->second) {
            declarations += decls.size();
            if (!decls.empty()) { only = decls.front().get(); }
        }
        return declarations == 1 ? namespace_variable_hit{only, true} : namespace_variable_hit{};
    }
    // the unit's own declaration of `ns::x` (a const verdict is stamped for the run: it rests on this unit's text alone)
    const variable_decl* own_namespace_variable(const member_expr* m) const {
        const std::string path = member_path(m);
        const auto it = path.empty() ? namespace_vars_.end() : namespace_vars_.find(path + "::" + std::string(m->member));
        return it == namespace_vars_.end() ? nullptr : it->second;
    }

    // The namespace variable `path::member` declared with a spelled type, `ns::x` and a bare x in the namespace's code alike: the
    // type a read gives and a store respects (diagnostics only; a class slot may hold a subclass, so it types nothing)
    ctype namespace_variable_type(const std::string& path, std::string_view member, namespace_variable_hit* declared = nullptr) {
        const namespace_variable_hit hit = namespace_variable(path, member);
        const variable_decl* nv = hit.decl;
        if (!nv || (nv->type && nv->type->is_reference()) || nv->function_spelling) { return {}; }
        ctype t = nv->type ? ctype_from_type_info(nv->type.get()) : ctype{};
        if (t.is_unknown() && !hit.foreign) {   // auto: what the walk inferred, once it has passed the declaration, as at program scope
            const auto inferred = namespace_inferred_types_.find(path + "::" + std::string(member));
            if (inferred != namespace_inferred_types_.end()) { t = inferred->second; }
        }
        if (declared) { *declared = hit; }
        return read_through_declared_slot(t);
    }
    static ctype read_through_declared_slot(ctype a_type) {
        if (a_type.is_unknown()) { return a_type; }
        a_type.tentative = true;
        a_type.declared_slot = true;
        a_type.element_proof = false;
        a_type.alias_proof = false;
        return a_type;
    }
    static bool kind_certain(const ctype& a_type) { return !a_type.tentative || a_type.declared_slot; }
    // a kind the program states: a certain one, or a result a declaration gives
    static bool kind_stated(const ctype& a_type) { return kind_certain(a_type) || a_type.declared_result; }
    // a store to it: the declared type the right side must fit, unknown when no spelled declaration is in sight
    ctype check_namespace_variable_store(const std::string& path, std::string_view member, const ctype& rhs, bool compound, token_type base_op,
                                         const source_location& loc) {
        namespace_variable_hit hit;
        const ctype declared = namespace_variable_type(path, member, &hit);
        if (declared.is_unknown()) { return {}; }
        check_store(rhs, compound, base_op, declared, declared_variable_type_name(hit.decl), "'" + path + "::" + std::string(member) + "'", loc,
                    hit.foreign ? declared_at{} : declared_at{hit.decl->location});
        return declared;
    }

    // each namespace function and variable's first declaration in this unit ("a::b::f"), for top-level code that names it earlier
    void collect_namespace_declarations(const namespace_decl* n) {
        stack_.guard(n->location);
        const auto note = [&](std::string_view member, const source_location& at) {
            auto [it, fresh] = namespace_decls_.try_emplace(std::string(n->name) + "::" + std::string(member), at);
            if (!fresh && declared_after(it->second, at)) { it->second = at; }
        };
        for (const auto& d : n->declarations) {
            if (!d) { continue; }
            switch (d->get_type()) {
                case node_type::function_decl: note(static_cast<const function_decl*>(d.get())->name, d->location); break;
                case node_type::variable_decl: note(static_cast<const variable_decl*>(d.get())->name, d->location); break;
                case node_type::destructuring_decl:
                    for (const auto& [name, _] : static_cast<const destructuring_decl*>(d.get())->names) { note(name, d->location); }
                    break;
                case node_type::namespace_decl: collect_namespace_declarations(static_cast<const namespace_decl*>(d.get())); break;
                default: break;
            }
        }
    }

    // the unit's own declaration: no instance's (those carry their generic's file)
    static bool own_text(const declaration& d) {
        switch (d.get_type()) {
            case node_type::generic_instance_decl: return false;
            case node_type::class_decl: return static_cast<const class_decl&>(d).instance_stamp == 0 && !static_cast<const class_decl&>(d).method_instances;
            case node_type::function_decl: return static_cast<const function_decl&>(d).instance_stamp == 0;
            case node_type::namespace_decl: {
                const auto& members = static_cast<const namespace_decl&>(d).declarations;
                return members.empty() || std::any_of(members.begin(), members.end(), [](const declaration_ptr& m) { return m && own_text(*m); });
            }
            default: return true;
        }
    }

    void collect_top_level() {
        // the unit's file is where its first own declaration stands; a unit of instances only takes its first request's
        for (const auto& d : decls_) {
            if (!unit_file_.empty()) { break; }
            if (d && own_text(*d)) { unit_file_ = d->location.filename; }
        }
        collect_generics(decls_);
        generics_live_ = !generics_.empty() || eng_.declares_generics();
        collect_scope_decls(decls_);
        for (const auto& d : decls_) {
            if (d && d->get_type() == node_type::namespace_decl) {
                collect_namespace_declarations(static_cast<const namespace_decl*>(d.get()));
                collect_namespace_vars(static_cast<const namespace_decl*>(d.get()), "");
                collect_namespace_members(static_cast<const namespace_decl*>(d.get()), namespace_members_);
            } else if (d && d->get_type() == node_type::extern_decl && static_cast<const extern_decl*>(d.get())->qualified()) {
                collect_qualified_extern(static_cast<const extern_decl*>(d.get()), namespace_members_);
            }
        }
        for (const auto& d : decls_) {
            if (d && (d->get_type() == node_type::include_decl || d->get_type() == node_type::import_decl)) {
                has_includes_ = true;
                if (!literal_unit_path(d.get())) { has_dynamic_includes_ = true; }
                continue;
            }
            collect_names(d.get());
        }
        for (const auto& [name, c] : classes_) {
            if (unit_redefines(c.decl)) { open_classes_.insert(name); }
        }
    }

    // The engine already holds a script class of this name with a different instance/static
    // field set: this unit hot-reloads it (an identical re-execution keeps the same set).
    bool unit_redefines(const class_decl* cd) {
        if (reload_dependent_) { return false; }
        auto def = eng_.find_script_class(std::string(cd->name));
        if (!def) { return false; }
        size_t own_fields = 0;
        for (const auto& [id, _] : def->get_all_field_defaults()) {
            if (def->find_field_declaring_class(id) == def.get()) { ++own_fields; }
        }
        size_t fields = 0, statics = 0;
        for (const auto& m : cd->members) {
            if (!m.declaration || m.declaration->get_type() != node_type::variable_decl) { continue; }
            auto* f = static_cast<const variable_decl*>(m.declaration.get());
            const uint64_t id = eng_.symbolize(f->name);
            if (f->is_static) {
                ++statics;
                if (!def->has_static_field(id) && !def->is_static_field(id)) { return true; }
            } else {
                ++fields;
                if (def->find_field_declaring_class(id) != def.get()) { return true; }
            }
        }
        return fields != own_fields || statics != def->get_static_field_names().size();
    }

    // The general redefinition rule: two definitions of one signature in the same unit
    // are an error - program-scope functions, methods, constructors and namespace functions, by their parameter types (T& and T
    // differ; const is not part of a signature). Declarations of other signatures are overloads. A
    // definition of the same signature from another unit is a redefinition, the hot-reload path: it replaces the earlier one, and
    // the checker warns when it sees the earlier definition.
    static std::string parameter_signature(const function_decl* fn) {
        std::string out = "(";
        for (size_t i = 0; i < fn->parameters.size(); ++i) {
            const parameter& p = fn->parameters[i];
            if (i) { out += ", "; }
            out += p.type ? p.type->canonical_name() : std::string("auto");
            if (p.is_reference && (!p.type || p.type->base_type != script_value_type::jai_reference_type)) { out += "&"; }
        }
        return out + ")";
    }

    // by_name: program-scope functions - a coroutine does not overload, so a function and a coroutine of one name replace each
    // other (a warning); one of the same signature is the redefinition error. An `override` declaration replaces on purpose; a
    // static and an instance method live in separate tables.
    void report_redefinitions(const std::vector<const function_decl*>& fns, bool by_name) {
        std::unordered_map<std::string, const function_decl*> first_seen;
        std::unordered_map<std::string_view, const function_decl*> first_name;
        for (const function_decl* fn : fns) {
            if (made_by_declaration_again(fn)) { continue; }
            const std::string sig = parameter_signature(fn);
            const std::string key = std::string(fn->name) + (fn->is_static ? "#static" : "") + sig;
            auto [it, inserted] = first_seen.emplace(key, fn);
            if (inserted) {   // an earlier overload of the same name and table that differs only by reference
                for (const function_decl* earlier : fns) {
                    if (earlier == fn) { break; }
                    if (earlier->name != fn->name || earlier->is_static != fn->is_static || made_by_declaration_again(earlier) ||
                        !detail::differs_only_by_reference(fn->parameters, earlier->parameters)) { continue; }
                    diag(diag_level::error, fn->location,
                         detail::reference_only_overload_text(fn->name, fn->parameters, earlier->parameters, earlier->location.line),
                         earlier->location);
                    break;
                }
            }
            if (!inserted && !fn->is_override) {
                diag(diag_level::error, fn->location,
                     "'" + std::string(fn->name) + "' is already defined" + (by_name ? std::string() : " with parameters " + sig),
                     it->second->location);
                continue;
            }
            if (!by_name) { continue; }
            auto [nit, fresh] = first_name.emplace(fn->name, fn);
            if (!fresh && inserted && (fn->is_coroutine || nit->second->is_coroutine)) {
                diag(diag_level::warning, fn->location,
                     "duplicate definition of function '" + std::string(fn->name) +
                     "' replaces the earlier one (a coroutine does not overload)",
                     nit->second->location);
            }
        }
    }

    // Every block of one namespace in the unit is one scope: a reopening's function collides like one in the first block
    void collect_namespace_functions(const namespace_decl* n, const std::string& prefix,
                                     std::map<std::string, std::vector<const function_decl*>>& by_namespace) {
        stack_.guard(n->location);
        const std::string path = prefix.empty() ? std::string(n->name) : prefix + "::" + std::string(n->name);
        auto& fns = by_namespace[path];
        for (const auto& d : n->declarations) {
            if (!d) { continue; }
            if (d->get_type() == node_type::function_decl) { fns.push_back(static_cast<const function_decl*>(d.get())); }
            else if (d->get_type() == node_type::namespace_decl) { collect_namespace_functions(static_cast<const namespace_decl*>(d.get()), path, by_namespace); }
            else if (d->get_type() == node_type::class_decl && !static_cast<const class_decl*>(d.get())->method_instances) { report_class_redefinitions(static_cast<const class_decl*>(d.get())); }
        }
    }

    void report_class_redefinitions(const class_decl* cd) {
        std::vector<const function_decl*> fns;
        for (const auto& m : cd->members) {
            if (m.declaration && m.declaration->get_type() == node_type::function_decl) { fns.push_back(static_cast<const function_decl*>(m.declaration.get())); }
        }
        report_redefinitions(fns, /*by_name=*/false);
        report_override_markers(cd, fns);
    }

    // The base class whose chain declares the instance method `name` (script_class_definition::find_method's walk); a base
    // field's synthesized _get_/_set_ accessor counts only when `accessors` (an override may take it, a plain method is not
    // redefining it); found = false with certain = false when a base is out of the checker's sight
    struct base_method { bool found = false; bool certain = true; std::string owner; };
    base_method find_base_method(const class_decl* cd, std::string_view name, bool accessors, int depth = 0) {
        base_method result;
        if (depth > 32) { result.certain = false; return result; }
        for (auto base : cd->base_classes) {
            if (const sclass* b = find_sclass(base)) {
                if (b->methods.count(name)) { return {true, true, std::string(b->decl->name)}; }
                if (accessors && name.size() > 5 && (name.substr(0, 5) == "_get_" || name.substr(0, 5) == "_set_") && b->fields.count(name.substr(5))) {
                    return {true, true, std::string(b->decl->name)};
                }
                base_method deeper = find_base_method(b->decl, name, accessors, depth + 1);
                if (deeper.found) { return deeper; }
                result.certain = result.certain && deeper.certain;
                continue;
            }
            auto def = engine_class(base);
            if (!def) { result.certain = false; continue; }
            auto info = def->find_method(name);
            if (info.owner_class && (accessors || !info.owner_class->owns_auto_accessor(eng_.symbolize(name)))) {
                return {true, true, std::string(info.owner_class->get_name())};
            }
        }
        return result;
    }

    // A method redefining a base class method says 'override', and an 'override' redefines one (the run time raises both when
    // the class declaration runs)
    void report_override_markers(const class_decl* cd, const std::vector<const function_decl*>& fns) {
        if (!final_base(cd).empty()) { return; }   // the derivation is the class's one error
        for (const function_decl* fn : fns) {
            if (fn->is_static || fn->name == cd->name || (fn->name.size() > 1 && fn->name[0] == '~')) { continue; }
            const base_method base = cd->base_classes.empty() ? base_method{} : find_base_method(cd, fn->name, fn->is_override);
            std::string text;
            if (!fn->is_override && base.found) { text = detail::missing_override_text(fn->name, base.owner); }
            else if (fn->is_override && !base.found && base.certain) { text = detail::override_without_base_text(fn->name); }
            if (text.empty()) { continue; }
            text[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[0])));
            diag(diag_level::error, fn->location, text);
        }
    }

    // A class's generic-method instances are its methods: two generic methods that make one instance of one signature define it
    // twice, as two functions do. Each instance stands in a declaration of its own, so they meet here, in declaration order.
    void report_method_instance_redefinitions() {
        std::map<std::string_view, std::vector<const function_decl*>> by_class;
        for (const auto& [name, instance] : method_instances_) {
            for (const function_decl* fn : instance.fns) { by_class[instance.owner].push_back(fn); }
        }
        for (auto& [owner, fns] : by_class) {
            std::sort(fns.begin(), fns.end(), [](const function_decl* a, const function_decl* b) {
                return std::tie(a->location.line, a->location.column, a->name) < std::tie(b->location.line, b->location.column, b->name);
            });
            report_redefinitions(fns, /*by_name=*/false);
        }
    }

    void warn_duplicate_top_level_functions() {
        std::vector<const function_decl*> fns;
        std::map<std::string, std::vector<const function_decl*>> namespace_fns;
        for (const auto& d : decls_) {
            if (!d) { continue; }
            if (d->get_type() == node_type::function_decl) { fns.push_back(static_cast<const function_decl*>(d.get())); }
            else if (d->get_type() == node_type::class_decl && !static_cast<const class_decl*>(d.get())->method_instances) { report_class_redefinitions(static_cast<const class_decl*>(d.get())); }
            else if (d->get_type() == node_type::namespace_decl) { collect_namespace_functions(static_cast<const namespace_decl*>(d.get()), {}, namespace_fns); }
        }
        for (const auto& [path, members] : namespace_fns) { report_redefinitions(members, /*by_name=*/false); }
        report_redefinitions(fns, /*by_name=*/true);
        report_method_instance_redefinitions();
        if (!program_functions_) { return; }
        std::unordered_set<std::string> warned;
        for (const function_decl* fn : fns) {
            if (fn->instance_stamp != 0) { continue; }   // every unit that uses a generic's instance declares it
            if (!warned.insert(std::string(fn->name) + parameter_signature(fn)).second) { continue; }
            eng_.note(fn->name);
            auto pit = program_functions_->find(std::string(fn->name));
            if (pit == program_functions_->end()) { continue; }
            for (const auto& [unit, decls] : pit->second) {
                const function_decl* other = nullptr;   // the other unit's declaration of this signature, which this one replaces
                for (const auto& d : decls) {
                    if (d && parameter_signature(d.get()) == parameter_signature(fn)) { other = d.get(); break; }
                }
                if (!other || other->location.filename == fn->location.filename) { continue; }
                diag(diag_level::warning, fn->location, "'" + std::string(fn->name) + "' redefines the function defined in " +
                     other->location.filename + ":" + std::to_string(other->location.line));
                break;
            }
        }
    }

    // -------------------------------------------------------------- name resolution

    var_entry* find_local(std::string_view name, size_t* scope_index = nullptr) {
        for (size_t i = scopes_.size(); i-- > 0;) {
            auto found = scopes_[i].find(name);
            if (found != scopes_[i].end()) {
                if (scope_index) { *scope_index = i; }
                return &found->second;
            }
        }
        return nullptr;
    }

    // A program-scope name (a global or a function), for the shadowing warnings; a bare name in a method resolves locals,
    // then members, then globals (docs/site/guide/07-classes.html), so a member hit is never degraded by one
    bool global_tier_name(std::string_view name) {
        if (global_var_names_.count(name) || functions_.count(name)) { return true; }
        std::string n(name);
        return extra_global(n) || eng_.has_variable(n) || eng_.has_function(n);
    }

    void note_global_use(std::string_view name) {
        if (!fn_stack_.empty()) {
            fn_stack_.back().globals_used.insert(name);
        }
    }

    // ---------------------------------------------------- shadowing warnings
    // Always warning severity - shadowing is legal; the checker just points at it.

    struct field_shadow_hit {
        std::string_view cls;
        const variable_decl* decl = nullptr;
        bool is_static = false;
    };

    // In-script hierarchy only: engine-registered ancestors are unknowable -> silent.
    std::optional<field_shadow_hit> find_shadowed_field(std::string_view cls, std::string_view name, int depth = 0) {
        if (depth > 32) { return std::nullopt; }
        auto it = classes_.find(cls);
        if (it == classes_.end()) { return std::nullopt; }
        if (auto f = it->second.fields.find(name); f != it->second.fields.end()) {
            return field_shadow_hit{cls, f->second, false};
        }
        if (auto f = it->second.static_fields.find(name); f != it->second.static_fields.end()) {
            return field_shadow_hit{cls, f->second, true};
        }
        for (auto base : it->second.decl->base_classes) {
            if (auto hit = find_shadowed_field(base, name, depth + 1)) { return hit; }
        }
        return std::nullopt;
    }

    void warn_shadow(std::string_view kind, std::string_view name, const source_location& loc,
                     bool allow_field_shape = true) {
        if (fn_stack_.empty()) { return; }   // top-level decls ARE the globals
        if (name.find('#') != std::string_view::npos) { return; }   // a desugaring's own variable (parser::hidden_name): no script spells it
        if (name == "_") { return; }   // a placeholder (C++26) is never read by its name, so hiding one hides nothing
        // The dangerous shape: shadows a field of the enclosing class (only where the
        // body can actually reach fields - methods/ctors and [this] lambdas)
        if (allow_field_shape && class_ctx_ && fn_stack_.back().sees_fields) {
            if (auto hit = find_shadowed_field(class_ctx_name_, name)) {
                std::string msg = std::string(kind) + " '" + std::string(name) + "' shadows " +
                    (hit->is_static ? "static field '" : "field '") + std::string(name) +
                    "' of class '" + std::string(hit->cls) + "'";
                if (hit->decl && hit->decl->location.line != 0) {
                    const source_location& field_at = hit->decl->location;   // a parent another file declares: that file's line
                    msg += " (field declared at " + (field_at.filename.empty() || field_at.filename == loc.filename ? std::string() : field_at.filename + ":") +
                           std::to_string(field_at.line) + ":" + std::to_string(field_at.column) + ")";
                }
                diag(diag_level::warning, loc, std::move(msg));
                return;
            }
        }
        // Shadows an outer local/param (any enclosing non-global scope)
        for (size_t i = scopes_.size() - 1; i-- > 1;) {
            auto found = scopes_[i].find(name);
            if (found != scopes_[i].end()) {
                std::string msg = std::string(kind) + " '" + std::string(name) +
                    "' shadows a declaration in an enclosing scope";
                if (found->second.line != 0) {
                    msg += " (declared at " + std::to_string(found->second.line) + ":" +
                           std::to_string(found->second.column) + ")";
                }
                diag(diag_level::warning, loc, std::move(msg));
                return;
            }
        }
        // Shadows a global this body already READ through the global tier: from here on
        // the same spelling silently means the local (declaration-point scoping).
        bool used_earlier = false;
        for (const auto& f : fn_stack_) {
            if (f.globals_used.count(name)) { used_earlier = true; break; }
        }
        if (used_earlier && (global_tier_name(name) || scopes_[0].count(name))) {
            diag(diag_level::warning, loc, std::string(kind) + " '" + std::string(name) +
                 "' shadows global '" + std::string(name) + "' used earlier in this function");
        }
    }

    bool engine_knows_name(const std::string& name) {
        if (eng_.has_function(name) || eng_.has_variable(name) || eng_.is_type_name(name)) {
            return true;
        }
        if (eng_.find_script_class(name)) {
            return true;
        }
        auto templates = eng_.get_registered_template_types();
        if (templates.count(name)) {
            return true;
        }
        return eng_.find_namespace(name) != nullptr;
    }

    // Static type of an engine global, trusted only when unambiguous.
    ctype engine_global_type(const std::string& name) {
        auto env = eng_.get_global_environment();
        if (!env) { return {}; }
        auto result = env->get(name);
        if (!result) { return {}; }
        const script_value& v = result.value();
        if (v.is_cpp_bound() || v.is_reference() || (v.get_type_info() && v.get_type_info()->dynamic_slot)) {
            return {};   // write-through bindings convert on their own terms; a var holding a typed array is a var
        }
        ctype t = ctype_from_type_info(v.get_type_info());
        t.tentative = true;   // value-tag derived (a later redeclaration retags): never a proof source
        return t;
    }

    // -------------------------------------------------------------- class lookups

    // 1 = compatible (same or src derived from dst), 0 = definitely unrelated, 2 = unknown
    int class_relation(std::string_view src, std::string_view dst, int depth = 0) {
        if (src == dst) { return 1; }
        if (depth > 32) { return 2; }
        if (const sclass* c = find_sclass(src)) {
            bool saw_unknown = false;
            for (auto base : c->decl->base_classes) {
                int r = class_relation(base, dst, depth + 1);
                if (r == 1) { return 1; }
                if (r == 2) { saw_unknown = true; }
            }
            return saw_unknown ? 2 : 0;
        }
        auto def = engine_class(src);
        if (def) {
            return def->is_subtype_of(std::string(dst)) ? 1 : 0;
        }
        return 2;   // src class not visible: relationship unknowable
    }

    // An enum's name spells the type of its values: ints the checker keeps apart from plain ints by the enum's name
    ctype ctype_from_type_info(const type_info* ti) {
        ctype t = spelled_ctype_from_type_info(ti);
        if (t.kind == ctype::object_k && t.ti && t.ti->base_type == script_value_type::jai_object_type && enum_type_name(*t.ti)) {
            t.kind = ctype::int_k;
        }
        return t;
    }
    bool enum_type_name(const type_info& ti) {
        if (ti.is_enum || find_enum(ti.type_name)) { return true; }
        if (!program_enums_) { return false; }
        eng_.note(ti.type_name);
        return program_enums_->count(ti.type_name) != 0;
    }
    static bool enum_value(const ctype& t) { return t.kind == ctype::int_k && !t.cls.empty(); }
    static ctype plain_int(ctype t) { if (t.kind == ctype::int_k) { t.cls = {}; t.ti = nullptr; } return t; }
    // Function-variable spelling is checker metadata; the runtime slot remains auto.
    ctype declared_variable_type(const variable_decl* decl) {
        return decl->function_spelling ? ctype::make(ctype::function_k) : ctype_from_type_info(decl->type.get());
    }

    std::shared_ptr<class_definition> engine_class(std::string_view name) {
        std::string n(name);
        auto def = eng_.get_class_definition(n);
        if (def) { return def; }
        auto script_def = eng_.find_script_class(n);
        return script_def ? std::static_pointer_cast<class_definition>(script_def) : nullptr;
    }

    // Member lookup across the in-script hierarchy (this unit, then the program table),
    // falling back to engine-registered ancestors. `certain` comes back false when any
    // ancestor is unresolvable — the caller must stay silent then; `unresolved` says an
    // ancestor is nowhere at all (bare names in the class's methods can then mean anything).
    struct member_hit {
        bool found = false;
        bool certain = true;
        const variable_decl* field = nullptr;              // typed in-script field
        const std::vector<const function_decl*>* methods = nullptr;
        bool foreign = false;      // declared by another closure unit: no related location in this source
        bool provable = true;      // the declaring class is this unit's or registered: its types may seed proofs
        bool unresolved = false;
        bool read_only = false;    // a getter-only binder property on a closed class: every store is the runtime raise
        uint8_t access = 0;              // 0 public, 1 private, 2 protected
        std::string_view declarer;       // the class that declares a non-public member
    };

    // A registered class's non-public member: its declarer and level (class_definition::find_nonpublic_declarer)
    static void engine_member_access(const class_definition& def, uint64_t id, member_hit& h) {
        access_level level = access_level::public_access;
        if (const class_definition* declarer = def.find_nonpublic_declarer(id, level)) {
            h.access = level == access_level::private_access ? 1 : 2;
            h.declarer = declarer->get_name();
        }
    }

    // cls is base or derives from it, through the modeled chain and registered ancestors
    // The class a type argument names for a bound: a shared_ptr's, as dynamic_cast<T> reads a handle; any other type as it is
    static std::string_view handle_class(std::string_view argument) {
        constexpr std::string_view handle = "shared_ptr<";
        if (argument.size() > handle.size() && argument.substr(0, handle.size()) == handle && argument.back() == '>') {
            return argument.substr(handle.size(), argument.size() - handle.size() - 1);
        }
        return argument;
    }
    JAI_NOINLINE std::string bound_failure_text(const generic_instance_decl& request, const generic_instance_decl::bound& bound) {
        std::string text = std::string(request.name) + ": ";
        const bool declared = classes_.count(bound.requires_) != 0 || (program_classes_ && program_classes_->count(bound.requires_) != 0) || engine_class(bound.requires_);
        if (!declared) {
            return text + "the bound of " + bound.parameter + ", '" + bound.requires_ + "', is no declared class: declare it, or bound " + bound.parameter +
                   " by a class that is (" + bound.parameter + " : Base)";
        }
        text += "'" + bound.argument + "' does not derive from '" + bound.requires_ + "', which " + bound.parameter + " requires";
        if (bound.argument.starts_with("weak_ptr<")) { text += ": a weak_ptr names no object until it is locked; pass w.lock()"; }
        return text;
    }

    bool class_is_or_derives(std::string_view cls, std::string_view base, int depth = 0) {
        if (cls == base) { return true; }
        if (depth > 32) { return false; }
        if (const sclass* c = find_sclass(cls)) {
            for (auto b : c->decl->base_classes) {
                if (class_is_or_derives(b, base, depth + 1)) { return true; }
            }
            return false;
        }
        auto def = engine_class(cls);
        auto base_def = engine_class(base);
        return def && base_def && def->derives_from(base_def.get());
    }

    // Access control as the runtime applies it (detail::enforce_member_access): a private
    // (or protected, its synonym) member is reachable inside its declaring class and every class derived from it; the
    // same text as the raise
    bool access_denied(const member_hit& hit, std::string_view member, const source_location& loc) {
        if (!hit.found || hit.access == 0) { return false; }
        const std::string_view ctx = class_ctx_ ? class_ctx_name_ : std::string_view{};
        if (!ctx.empty() && class_is_or_derives(ctx, hit.declarer)) { return false; }
        diag(diag_level::error, loc,
             "Cannot access private member '" + std::string(member_display_name(member)) + "' of class '" + std::string(hit.declarer) + "'");
        return true;
    }

    // instance_only: a dotted receiver's view (obj.name), which reaches no static of a script class
    member_hit lookup_member(std::string_view cls, std::string_view member, bool want_static, int depth = 0, bool instance_only = false) {
        member_hit miss;
        if (depth > 32) { miss.certain = false; return miss; }
        if (const sclass* c = find_sclass(cls)) {
            auto hit = [&](const variable_decl* f, const std::vector<const function_decl*>* m) {
                member_hit h{true, true, f, m};
                h.foreign = c->foreign;
                h.provable = c->provable;
                if (auto a = c->nonpublic.find(member); a != c->nonpublic.end()) {
                    h.access = a->second;
                    h.declarer = c->decl->name;
                }
                return h;
            };
            if (want_static) {
                if (auto f = c->static_fields.find(member); f != c->static_fields.end()) { return hit(f->second, nullptr); }
                if (auto m = c->static_methods.find(member); m != c->static_methods.end()) { return hit(nullptr, &m->second); }
            } else {
                if (auto f = c->fields.find(member); f != c->fields.end()) { return hit(f->second, nullptr); }
                if (auto m = c->methods.find(member); m != c->methods.end()) { return hit(nullptr, &m->second); }
                // a method's bare name reaches statics too (count++ in methods)
                if (!instance_only) {
                    if (auto f = c->static_fields.find(member); f != c->static_fields.end()) { return hit(f->second, nullptr); }
                    if (auto m = c->static_methods.find(member); m != c->static_methods.end()) { return hit(nullptr, &m->second); }
                }
            }
            member_hit result;
            for (auto base : c->decl->base_classes) {
                member_hit h = lookup_member(base, member, want_static, depth + 1, instance_only);
                if (h.found) {
                    if (h.read_only && c->methods.count("_set_" + std::string(member))) { h.read_only = false; }   // a script _set_ override takes the stores
                    return h;
                }
                result.certain = result.certain && h.certain;
                result.unresolved = result.unresolved || h.unresolved;
            }
            return result;
        }
        auto def = engine_class(cls);
        if (!def) {
            miss.certain = false;
            miss.unresolved = true;
            return miss;
        }
        // an instance of one of its generic methods, which this unit declares and the class takes when the unit runs
        if (const method_instance* instance = engine_method_instance(*def, member)) {
            if (instance->is_static == want_static || (instance->is_static && !instance_only)) {
                member_hit h{true, true, nullptr, &instance->fns};
                h.provable = false;
                h.access = instance->access;
                h.declarer = instance->owner;
                return h;
            }
        }
        uint64_t id = eng_.symbolize(member);
        // Script/vm classes from previous executes don't expose a uniform member surface
        // across backends: a hit is trusted, a miss is not. Only a cpp_class is closed, and
        // only while no script layer was ever extended over it.
        bool closed = def->is_cpp_class() && !def->extended_ever();
        if (want_static) {
            if (def->has_static_method(id) || def->is_static_field(id) || def->has_static_field(id)) {
                member_hit h{true, true, nullptr, nullptr};
                engine_member_access(*def, id, h);
                return h;
            }
            // a host static property: _get_x reads it, _set_x (when bound) stores it
            auto* sym = eng_.get_symbolizer();
            const bool static_getter = def->has_static_method(sym->get_getter_id_with_view(id).first);
            const bool static_setter = def->has_static_method(sym->get_setter_id_with_view(id).first);
            if (static_getter || static_setter) {
                member_hit h{true, true, nullptr, nullptr};
                h.read_only = closed && !static_setter;
                engine_member_access(*def, id, h);
                return h;
            }
        } else {
            uint64_t getter_id = eng_.symbolize("_get_" + std::string(member));
            if (engine_instance_member(*def, id, getter_id)) {
                member_hit h{true, true, nullptr, nullptr};
                if (closed && def->is_read_only_property(id)) {
                    h.read_only = !derived_opens_property(*def, id, eng_.symbolize("_set_" + std::string(member)));
                }
                engine_member_access(*def, id, h);
                return h;
            }
            // A binder surface is a lower bound on the C++ declaration: a virtual bound only
            // on registered subclasses still dispatches through a base-typed handle.
            if (closed && derived_defines_member(*def, id, getter_id)) { closed = false; }
        }
        miss.certain = closed;   // cpp classes: member definitely absent; others stay open
        return miss;
    }

    const method_instance* engine_method_instance(const class_definition& def, std::string_view member) {
        if (method_instances_.empty() || member.find('<') == std::string_view::npos) { return nullptr; }
        const auto held = method_instances_.find(member);
        if (held == method_instances_.end()) { return nullptr; }
        const auto owner = engine_class(held->second.owner);
        return owner && (owner.get() == &def || def.derives_from(owner.get())) ? &held->second : nullptr;
    }

    // A user _get_/_set_ for `member` anywhere in cls's chain: modeled classes through their
    // method maps, registered ancestors through the engine's definitions. Such a member is
    // known, its static type is the accessor's business (unknown), and it is never a proof.
    bool chain_has_user_accessor(std::string_view cls, std::string_view member, int depth = 0) {
        if (depth > 32) { return false; }
        if (const sclass* c = find_sclass(cls)) {
            const std::string getter = "_get_" + std::string(member), setter = "_set_" + std::string(member);
            if (c->methods.count(getter) || c->methods.count(setter)) { return true; }
            for (auto base : c->decl->base_classes) {
                if (chain_has_user_accessor(base, member, depth + 1)) { return true; }
            }
            return false;
        }
        auto def = engine_class(cls);
        if (!def) { return false; }
        const uint64_t id = eng_.symbolize(member);
        auto* sym = eng_.get_symbolizer();
        return def->user_accessor(sym->get_getter_id_with_view(id).first) != nullptr ||
               def->user_accessor(sym->get_setter_id_with_view(id).first) != nullptr;
    }

    // The ONE user accessor `name` (_get_x / _set_x) across cls's modeled chain: null when the
    // chain declares none or several, or reaches a registered ancestor carrying one (its
    // declared signature is not visible). Diagnostics only - never consulted for a proof.
    const function_decl* single_user_accessor(std::string_view cls, std::string_view name) {
        const function_decl* found = nullptr;
        int count = 0;
        if (!count_user_accessors(cls, name, found, count, 0) || count != 1) { return nullptr; }
        return found;
    }

    bool count_user_accessors(std::string_view cls, std::string_view name, const function_decl*& found, int& count, int depth) {
        if (depth > 32) { return false; }
        if (const sclass* c = find_sclass(cls)) {
            if (auto m = c->methods.find(name); m != c->methods.end()) {
                for (const auto* fn : m->second) { found = fn; ++count; }
            }
            for (auto base : c->decl->base_classes) {
                if (!count_user_accessors(base, name, found, count, depth + 1)) { return false; }
            }
            return true;
        }
        auto def = engine_class(cls);
        if (!def) { return true; }
        return def->user_accessor(eng_.symbolize(std::string(name))) == nullptr;
    }

    // An accessor-bearing member reads as the getter's declared return type when the chain has
    // exactly one user _get_x, parameterless, non-coroutine, declaring a non-var type; tentative,
    // so nothing downstream is proven from it.
    ctype accessor_read_type(std::string_view cls, std::string_view member) {
        const std::string getter = "_get_" + std::string(member);
        const function_decl* fn = single_user_accessor(cls, getter);
        if (!fn || fn->is_coroutine || !fn->parameters.empty() || !fn->return_type) { return {}; }
        ctype t = ctype_from_type_info(fn->return_type.get());
        t.tentative = true;
        return t;
    }

    // Stores through exactly one user _set_x taking one by-value parameter check against that
    // parameter's declared type; a compound store needs the getter's type too (x op= v is x = T(x op v)).
    void check_accessor_store(std::string_view cls, std::string_view member, const ctype& rhs, bool compound,
                              token_type base_op, const source_location& loc) {
        const std::string setter = "_set_" + std::string(member);
        const function_decl* fn = single_user_accessor(cls, setter);
        if (!fn || fn->parameters.size() != 1 || fn->parameters[0].is_reference || !fn->parameters[0].type) { return; }
        const type_info* pt = fn->parameters[0].type.get();
        ctype target = ctype_from_type_info(pt);
        if (target.is_unknown()) { return; }
        ctype stored = rhs;
        if (compound) {
            ctype gt = accessor_read_type(cls, member);
            if (gt.is_unknown()) { return; }
            stored = binary_result(gt, base_op, rhs);
        }
        check_store(stored, /*compound=*/false, base_op, target, pt->canonical_name(),
                    "property '" + std::string(member) + "'", loc, {});
    }

    bool chain_open(std::string_view cls, int depth = 0) {
        if (depth > 32) { return true; }
        const sclass* c = find_sclass(cls);   // models a table class (marking it open) before the test
        if (open_classes_.count(cls)) { return true; }
        if (!c) { return false; }
        for (auto base : c->decl->base_classes) {
            if (chain_open(base, depth + 1)) { return true; }
        }
        return false;
    }

    // A field's declared type as the checker types it: certain from this unit or a registered
    // class, tentative (never a proof source) while it is known from the program table alone.
    ctype member_type(const member_hit& hit) {
        ctype t = declared_variable_type(hit.field);
        if (!hit.provable) { t.tentative = true; }
        return t;
    }

    // Instances shaped by an EARLIER definition of a class this unit redefines: values
    // that predate the unit (engine globals, value-tag typed) and `this` inside the
    // class's own methods (migration hooks run on the old shape). A fresh instance of the
    // new definition is checked against it.
    bool stale_shape_receiver(const ctype& recv, const expression* object) {
        if (recv.kind != ctype::object_k || !chain_open(recv.cls)) { return false; }
        return recv.tentative || (object && object->get_type() == node_type::this_expr);
    }

    bool stale_field_context() {
        return class_ctx_ && !fn_stack_.empty() && fn_stack_.back().sees_fields && chain_open(class_ctx_name_);
    }

    static bool engine_instance_member(const class_definition& def, uint64_t id, uint64_t getter_id) {
        return def.method_ptr(id) != nullptr || def.find_field_declaring_class(id) != nullptr ||
               def.method_ptr(getter_id) != nullptr ||
               def.is_static_field(id) || def.has_static_field(id) || def.has_static_method(id);
    }

    static bool derived_defines_member(const class_definition& def, uint64_t id, uint64_t getter_id, int depth = 0) {
        if (depth > 32) { return true; }
        for (const auto& weak : def.derived_classes()) {
            auto derived = weak.lock();
            if (!derived) { continue; }
            if (engine_instance_member(*derived, id, getter_id) || derived_defines_member(*derived, id, getter_id, depth + 1)) {
                return true;
            }
        }
        return false;
    }

    // A registered subclass owning a _set_ for id that is not its own read-only rejection (a
    // writable re-registration, a script override, a shadowing field's synthesized setter):
    // the store dispatches there through a base-typed handle, so the base's read-only answer
    // is not certain.
    static bool derived_opens_property(const class_definition& def, uint64_t id, uint64_t setter_id, int depth = 0) {
        if (depth > 32) { return true; }
        for (const auto& weak : def.derived_classes()) {
            auto derived = weak.lock();
            if (!derived) { continue; }
            if ((derived->has_method(setter_id) && !derived->owns_read_only_property(id)) ||
                derived_opens_property(*derived, id, setter_id, depth + 1)) {
                return true;
            }
        }
        return false;
    }

    void read_only_store(const source_location& loc, std::string_view name) {
        diag(diag_level::error, loc, "cannot assign to read-only property '" + std::string(name) + "'");
    }

    // `kv.first` of a range-for over a statically known map: the pair variable's name, or
    // empty. Map keys are read-only (C++ `const Key`, the ordering invariant); mutating the
    // pair's key copy is a load error here and a copy-on-write no-op at runtime
    //A `var`-typed map is dynamic and leaves it to the runtime.
    std::string_view map_key_operand(const expression* e) {
        if (!e || e->get_type() != node_type::member_expr) { return {}; }
        const auto* m = static_cast<const member_expr*>(e);
        if (m->is_static || m->member != "first" || !m->object || m->object->get_type() != node_type::identifier_expr) { return {}; }
        const auto* id = static_cast<const identifier_expr*>(m->object.get());
        const var_entry* local = find_local(id->name);
        return local && local->map_pair ? id->name : std::string_view{};
    }

    void map_key_mutation(const source_location& loc, std::string_view pair_name) {
        diag(diag_level::error, loc, "cannot mutate '" + std::string(pair_name) + ".first': map keys are read-only");
    }

    // `kv.first` spelled directly, or any expression whose inferred type carries const_key
    // (a ternary over keys); the text names the spelling when there is one
    std::string map_key_text(const expression* e) {
        std::string_view pair_name = map_key_operand(e);
        return pair_name.empty() ? std::string("a map key") : "'" + std::string(pair_name) + ".first'";
    }

    void map_key_mutation(const source_location& loc, const expression* e) {
        diag(diag_level::error, loc, "cannot mutate " + map_key_text(e) + ": map keys are read-only");
    }

    // Every mutable-alias spelling: T&/auto&/var& decl, shared_ptr<T> decl or store, cast<T&>,
    // cast<shared_ptr<T>>, a T&/shared_ptr<T> parameter or return. Copying is the way out.
    void map_key_alias(const source_location& loc, const expression* e) {
        std::string_view pair_name = map_key_operand(e);
        std::string copy_hint = pair_name.empty() ? std::string("var k = kv.first") : "var k = " + std::string(pair_name) + ".first";
        diag(diag_level::error, loc, "cannot alias " + map_key_text(e) + ": map keys are read-only (copy it instead: " + copy_hint + ")");
    }

    static bool handle_type(const type_info* t) {
        return t && t->base_type == script_value_type::jai_shared_ptr_type;
    }

    static bool string_mutator_name(std::string_view name) {
        for (std::string_view m : k_string_mutator_method_names) {
            if (m == name) { return true; }
        }
        return false;
    }

    // -------------------------------------------------------------- const

    std::unordered_map<std::string_view, const variable_decl*> top_consts_;   // this unit's program-scope consts (a use may precede the declaration)
    bool collecting_namespace_ = false;

    struct const_hit {
        uint8_t verdict = const_none;   // const_raises: a write through the const, every step typed; const_runtime: through an untyped step
        std::string root;               // the const's name
        size_t line = 0, column = 0;    // its declaration
        // the path's last value (a call's receiver): its type when typed
        const type_info* final_ti = nullptr;
        std::string final_cls;
        bool final_known = false;
        bool final_library = false;   // a string, array or map: the builtin library's methods
    };

    static const type_info* const_referent(const type_info* t) {
        if (t && t->base_type == script_value_type::jai_reference_type) { return t->type_params.empty() ? nullptr : t->type_params[0].get(); }
        return t;
    }

    // the const program-scope declaration another unit of the program makes (one unit, once)
    const variable_decl* program_const_global(const std::string& name) {
        eng_.note(name);
        if (!program_globals_) { return nullptr; }
        auto it = program_globals_->find(name);
        if (it == program_globals_->end() || it->second.size() != 1 || it->second.begin()->second.size() != 1) { return nullptr; }
        const variable_decl* g = it->second.begin()->second.front().get();
        return g && g->is_const ? g : nullptr;
    }

    // Where a write lands, by C++'s top-level rule: through a const name and no handle, a write to the const
    const_hit const_path(const expression* e, bool value_too = false) {
        const_hit none;
        if (!e) { return none; }
        if (e->get_type() == node_type::ternary_expr) {   // a conditional of lvalues is const when either arm is (C++)
            auto* t = static_cast<const ternary_expr*>(e);
            const_hit a = const_path(t->then_expression.get(), value_too), b = const_path(t->else_expression.get(), value_too);
            if (a.verdict == const_raises) { return a; }
            if (b.verdict == const_raises) { return b; }
            return a.verdict ? a : b;
        }
        std::vector<const expression*> steps;
        const expression* base = e;
        while (base) {
            const node_type k = base->get_type();
            if (k == node_type::member_expr && !static_cast<const member_expr*>(base)->is_static) {
                steps.push_back(base);
                base = static_cast<const member_expr*>(base)->object.get();
            } else if (k == node_type::binary_expr && static_cast<const binary_expr*>(base)->op.type == token_type::left_bracket) {
                steps.push_back(base);
                base = static_cast<const binary_expr*>(base)->left.get();
            } else if (k == node_type::cast_expr && static_cast<const cast_expr*>(base)->implicit_ref) {
                base = static_cast<const cast_expr*>(base)->operand.get();
            } else if (k == node_type::ternary_expr) {
                return const_path(base, true).verdict ? const_hit{const_runtime, const_path(base, true).root} : none;
            } else {
                break;
            }
        }
        if (!base) { return none; }
        const_hit hit;
        const type_info* cur = nullptr;
        std::string cur_cls;
        bool known = false, handle = false, is_const = false, library = false;
        auto from_type = [&](const type_info* t) {
            t = const_referent(t);
            cur = t;
            known = t && t->base_type != script_value_type::jai_any_type;
            handle = const_handle_type(t);
            library = t && (t->base_type == script_value_type::jai_array_type || t->base_type == script_value_type::jai_map_type ||
                            t->base_type == script_value_type::jai_string_type || t->base_type == script_value_type::jai_utf8_string_type);
            if (t && t->base_type == script_value_type::jai_object_type) { cur_cls = t->type_name; }
        };
        if (base->get_type() == node_type::identifier_expr) {
            auto* id = static_cast<const identifier_expr*>(base);
            hit.root = std::string(id->name);
            if (var_entry* v = find_named(id)) {
                is_const = v->is_const;
                hit.line = v->line; hit.column = v->column;
                from_type(v->declared ? v->declared : v->type.ti);
                if (!v->declared && !v->type.ti && !v->dynamic) {
                    known = !v->type.is_unknown();
                    if (v->type.kind == ctype::object_k) { cur_cls = std::string(v->type.cls); }
                    library = v->type.kind == ctype::array_k || v->type.kind == ctype::map_k || v->type.kind == ctype::string_k || v->type.kind == ctype::utf8_k;
                }
                if (v->is_handle) { handle = true; known = true; }
                if (v->dynamic && !v->is_reference) { known = false; }
                if (v->const_element && is_const) { from_type(v->const_element); }   // an element of a known class held by value
                if (v->map_pair) { known = true; handle = false; }   // a range-for pair is a value: its halves are the pair's
            } else if (class_ctx_ && !id->global_qualified && lookup_member(class_ctx_name_, id->name, false).found) {
                member_hit h = lookup_member(class_ctx_name_, id->name, false);
                if (!h.field) { return none; }
                is_const = h.field->is_const;
                if (!h.foreign) { hit.line = h.field->location.line; hit.column = h.field->location.column; }
                from_type(h.field->type.get());
            } else if (const std::string space = id->global_qualified ? std::string() : enclosing_namespace_member(id->name); !space.empty()) {
                if (!namespace_host_entry(space, id->name)) { return none; }   // the namespace's own member: its declaration's rules
                is_const = true;   // the host's entry, read-only from script as `ns::name` is
                known = false;
            } else if (const enum_decl* en = find_enum(id->name)) {   // an enum's name: a const map<string, int>
                is_const = true;
                known = true; library = true;
                hit.line = en->location.line; hit.column = en->location.column;
            } else if (auto tc = top_consts_.find(id->name); tc != top_consts_.end() && !id->global_qualified) {
                is_const = true;
                hit.line = tc->second->location.line; hit.column = tc->second->location.column;
                from_type(tc->second->type.get());
            } else if (const variable_decl* g = program_const_global(std::string(id->name))) {
                is_const = true;
                from_type(g->type.get());
            } else if (eng_.is_const_global(std::string(id->name))) {
                is_const = true;
                known = false;
            } else {
                return none;
            }
        } else if (base->get_type() == node_type::member_expr) {   // C::s, ns::x
            auto* m = static_cast<const member_expr*>(base);
            hit.root = detail::const_spelled(m);
            if (const enum_decl* en = namespace_enum(m)) {   // ns::E, the enum's name: a const map<string, int>
                is_const = true;
                known = true; library = true;
                hit.line = en->location.line; hit.column = en->location.column;
            } else if (eng_.is_enum_name(qualified_spelling(m))) {   // ns::E an earlier run declared
                is_const = true;
                known = true; library = true;
            } else if (const variable_decl* nv = own_namespace_variable(m)) {
                is_const = nv->is_const;
                hit.line = nv->location.line; hit.column = nv->location.column;
                from_type(nv->type.get());
            } else if (const var_entry* x = qualified_extern(m)) {
                is_const = x->is_const;
                hit.line = x->line; hit.column = x->column;
                from_type(x->declared);
            } else if (m->object && m->object->get_type() == node_type::identifier_expr) {
                const std::string_view cls = static_cast<const identifier_expr*>(m->object.get())->name;
                member_hit h = lookup_member(cls, m->member, true);
                if (h.found && h.field) {
                    is_const = h.field->is_const;
                    if (!h.foreign) { hit.line = h.field->location.line; hit.column = h.field->location.column; }
                    from_type(h.field->type.get());
                } else if (auto def = engine_class(cls); def && def->is_const_static(eng_.symbolize(m->member))) {
                    is_const = true;
                    known = false;
                }
            }
        } else {
            return none;
        }
        // a class constant reached through an instance (`obj.MAX = 1`, `this.MAX`) is the constant itself
        if (!is_const && !steps.empty() && steps.front()->get_type() == node_type::member_expr && !value_too) {
            auto* last = static_cast<const member_expr*>(steps.front());
            std::string owner;
            if (last->object && last->object->get_type() == node_type::this_expr && class_ctx_) { owner = std::string(class_ctx_name_); }
            else if (steps.size() == 1) { owner = cur_cls; }
            if (!owner.empty()) {
                member_hit h = lookup_member(owner, last->member, false);
                if (h.found && h.field && h.field->is_static && h.field->is_const) {
                    hit.root = std::string(last->member);
                    hit.verdict = const_raises;
                    if (!h.foreign) { hit.line = h.field->location.line; hit.column = h.field->location.column; }
                    return hit;
                }
            }
        }
        if (!is_const) {
            if (!steps.empty() && steps.front()->get_type() == node_type::member_expr && base->get_type() == node_type::this_expr && !value_too) {
                auto* last = static_cast<const member_expr*>(steps.front());
                if (steps.size() == 1 && class_ctx_) {
                    member_hit h = lookup_member(class_ctx_name_, last->member, false);
                    if (h.found && h.field && h.field->is_static && h.field->is_const) {
                        hit.root = std::string(last->member);
                        hit.verdict = const_raises;
                        return hit;
                    }
                }
            }
            return none;
        }
        if (steps.empty() && !value_too) { hit.verdict = const_raises; return hit; }   // the const itself
        uint8_t verdict = const_raises;
        for (size_t i = steps.size(); i-- > 0;) {
            const bool last = i == 0 && !value_too;
            if (known && handle) { return none; }
            if (!known) { verdict = const_runtime; break; }
            if (last) { break; }
            const expression* step = steps[i];
            if (step->get_type() == node_type::member_expr) {
                auto* m = static_cast<const member_expr*>(step);
                const std::string owner = cur_cls;
                cur = nullptr; cur_cls.clear(); known = false; handle = false; library = false;
                if (owner.empty()) { continue; }
                member_hit h = lookup_member(owner, m->member, false);
                if (h.found && h.field) {
                    if (h.field->is_static && h.field->is_const) { hit.root = std::string(m->member); hit.verdict = const_raises; return hit; }
                    from_type(h.field->type.get());
                }
            } else {
                const type_info* el = nullptr;
                if (cur && cur->base_type == script_value_type::jai_array_type) { el = cur->element_type().get(); }
                else if (cur && cur->base_type == script_value_type::jai_map_type) { el = cur->value_type().get(); }
                else if (cur && (cur->base_type == script_value_type::jai_string_type || cur->base_type == script_value_type::jai_utf8_string_type)) {
                    cur = nullptr; cur_cls.clear(); known = true; handle = false; library = false;   // a character: no further step
                    continue;
                }
                cur = nullptr; cur_cls.clear(); known = false; handle = false; library = false;
                from_type(el);
            }
        }
        if (value_too && verdict == const_raises) {
            if (known && handle) { return none; }
            if (!known) { verdict = const_runtime; }
        }
        hit.verdict = verdict;
        hit.final_ti = cur;
        hit.final_cls = cur_cls;
        hit.final_known = known;
        hit.final_library = library;
        return hit;
    }

    void stamp_const(const_site& site, uint8_t verdict, const std::function<std::string()>& text) {
        if (verdict == const_none) {
            if (site.verdict != const_none) { site = const_site{}; }
            return;
        }
        site.verdict = verdict;
        site.text = std::make_shared<const std::string>(text());
        site.text_id = eng_.symbolize(*site.text);   // the run time raises "{0}" with it: the text outlives every error object
    }

    // a store, a compound store or ++/-- through `target`
    void check_const_store(const_site& site, const expression* target, const source_location& at) {
        const const_hit h = const_path(target);
        stamp_const(site, h.verdict, [&] { return detail::const_store_text(target, h.root); });
        if (h.verdict == const_raises) { diag(diag_level::error, at, detail::const_store_diagnostic(target, h.root), h.line, h.column); }
    }

    // the checker's receiver-writes view: this unit's and the program's classes, then the engine's
    detail::receiver_view checker_receiver_view() {
        detail::receiver_view v;
        v.field_type = [this](std::string_view cls, std::string_view field, bool& is_field) -> const type_info* {
            member_hit h = lookup_member(cls, field, false);
            is_field = h.found && !h.methods;
            return h.field ? h.field->type.get() : nullptr;
        };
        v.methods = [this](std::string_view cls, std::string_view name, std::vector<const function_decl*>& out) -> bool {
            member_hit h = lookup_member(cls, name, false);
            if (!h.found) { return h.certain && !h.unresolved; }
            if (!h.methods) { return false; }
            for (const auto* f : *h.methods) { out.push_back(f); }
            return true;
        };
        v.functions = [this](const identifier_expr& id, std::vector<ref_write_callee>& out) {
            if (auto it = functions_.find(id.name); it != functions_.end()) {
                if (it->second.size() != 1) { return false; }
                out.push_back(ref_write_callee_of(it->second.front()));
                return true;
            }
            return resolve_global_ref_write_callee(eng_.asking(id.name), id, out);
        };
        return v;
    }

    // An enum's name read as its value table (a map<string, int>)
    bool enum_table(const expression* e) {
        if (e->get_type() == node_type::identifier_expr) {
            const auto* id = static_cast<const identifier_expr*>(e);
            return find_enum(id->name) || eng_.is_enum_name(std::string(id->name));
        }
        if (e->get_type() == node_type::member_expr && static_cast<const member_expr*>(e)->is_static) {
            const auto* m = static_cast<const member_expr*>(e);
            return namespace_enum(m) || eng_.is_enum_name(qualified_spelling(m));
        }
        return false;
    }
    // A builtin method of the library value a const path ends at (no type: an enum's value table, a map)
    bool library_method(const type_info* t, std::string_view member) {
        int kind = 1;
        if (t && t->base_type == script_value_type::jai_array_type) { kind = 0; }
        else if (t && t->base_type == script_value_type::jai_string_type) { kind = 2; }
        else if (t && t->base_type == script_value_type::jai_utf8_string_type) { kind = 3; }
        return eng_.builtin_method_ids(kind).count(eng_.symbolize(member)) != 0;
    }
    // E.v(...): an enum's value read with a dot is an int, and a call of it is the run time's text
    void enum_value_called(const member_expr* m) {
        const enum_decl* e = nullptr;
        if (m->object->get_type() == node_type::identifier_expr) { e = find_enum(static_cast<const identifier_expr*>(m->object.get())->name); }
        else if (m->object->get_type() == node_type::member_expr) { e = namespace_enum(static_cast<const member_expr*>(m->object.get())); }
        if (!e) { return; }
        for (const auto& [value, _] : e->values) {
            if (value == m->member) {
                diag(diag_level::error, m->location, "cannot call '" + std::string(m->member) + "': it is an int, not a function");
                return;
            }
        }
    }

    // a method call on a const-rooted receiver: an error when the method writes its receiver
    void check_const_call(call_expr* c) {
        if (!c->callee || c->callee->get_type() != node_type::member_expr) { stamp_const(c->const_check, const_none, {}); return; }
        auto* m = static_cast<member_expr*>(c->callee.get());
        if (m->is_static) { stamp_const(c->const_check, const_none, {}); return; }
        const const_hit h = const_path(m->object.get(), /*value_too=*/true);
        if (!h.verdict) { stamp_const(c->const_check, const_none, {}); return; }
        uint8_t verdict = h.verdict;
        bool builtin = false;
        size_t line = 0;
        if (h.final_known && h.final_library) {
            builtin = true;
            if ((h.final_ti || enum_table(m->object.get())) && !library_method(h.final_ti, m->member)) {   // a key or an enum's value read with a dot and called: nothing is written
                stamp_const(c->const_check, const_none, {});
                if (!h.final_ti) { enum_value_called(m); }
                return;
            }
            if (ref_write_reading_method(m->member)) {
                verdict = const_none;
            } else if (ref_write_element_callback(m->member)) {
                const bool text = h.final_ti && (h.final_ti->base_type == script_value_type::jai_string_type || h.final_ti->base_type == script_value_type::jai_utf8_string_type);
                const callback_write w = text ? callback_write::reads : element_callback_writes(c, m->member, checker_receiver_view().functions);
                if (w == callback_write::reads) { verdict = const_none; }
                else if (w == callback_write::unseen) { verdict = const_runtime; }
            }
        } else if (h.final_known && !h.final_cls.empty()) {
            detail::receiver_write_analysis analysis(checker_receiver_view());
            detail::receiver_write w = analysis.analyze_methods(h.final_cls, m->member);
            if (!w.writes) {
                // an override in a derived class may write (a by-value slot can hold a subclass): the run time decides then
                bool any = false;
                for (const auto* f : overrides_of(h.final_cls, m->member)) {
                    if (analysis.analyze(f, h.final_cls).writes) { any = true; }
                }
                verdict = any ? const_runtime : const_none;
            } else {
                line = w.line;
                if (!w.certain) { verdict = const_runtime; }
            }
        } else if (h.final_known) {
            verdict = const_none;   // a primitive, a math value: its methods return values
        } else {
            verdict = const_runtime;
        }
        stamp_const(c->const_check, verdict, [&] { return detail::const_call_text(m->object.get(), h.root, m->member, builtin, line); });
        if (verdict == const_raises) {
            diag(diag_level::error, c->location, detail::const_lowercase(detail::const_call_text(m->object.get(), h.root, m->member, builtin, line)), h.line, h.column);
        }
    }

    // a T& / var& declaration bound to a const (an auto& or a const T& takes it as a const reference)
    void check_const_bind(variable_decl* decl, var_entry& entry) {
        if (decl->is_const) {
            entry.is_const = true;
            entry.has_const_text = decl->initializer && detail::regex_library::constant_text(decl->initializer.get(), entry.const_text);
            stamp_const(decl->const_check, const_none, {});
            return;
        }
        if (!decl->initializer || !decl->type) { stamp_const(decl->const_check, const_none, {}); return; }
        if (decl->type->base_type != script_value_type::jai_reference_type) { stamp_const(decl->const_check, const_none, {}); return; }
        const const_hit h = const_path(decl->initializer.get());
        if (!h.verdict) { stamp_const(decl->const_check, const_none, {}); return; }
        if (!const_referent(decl->type.get())) {   // auto& deduces const T& (C++)
            entry.is_const = h.verdict == const_raises;
            stamp_const(decl->const_check, const_none, {});
            return;
        }
        const std::string declared = decl->type->canonical_name();
        auto text = [&] { return detail::const_bind_text(decl->name, declared, decl->initializer.get(), h.root, false); };
        stamp_const(decl->const_check, h.verdict, text);
        if (h.verdict == const_raises) { diag(diag_level::error, decl->location, detail::const_lowercase(text()), h.line, h.column); }
    }

    // cast<T&>(c) names the const as writable (no const_cast)
    void check_const_cast(cast_expr* c) {
        if (c->implicit_ref || !c->by_reference) { stamp_const(c->const_check, const_none, {}); return; }
        const const_hit h = const_path(c->operand.get());
        if (!h.verdict) { stamp_const(c->const_check, const_none, {}); return; }
        const std::string target = c->target ? c->target->canonical_name() + (c->target->base_type != script_value_type::jai_reference_type ? "&" : "") : std::string("T&");
        auto text = [&] { return detail::const_cast_text(c->operand.get(), h.root, target); };
        stamp_const(c->const_check, h.verdict, text);
        if (h.verdict == const_raises) { diag(diag_level::error, c->location, detail::const_lowercase(text()), h.line, h.column); }
    }

    // a const returned by reference
    void check_const_return(return_stmt* r) {
        if (!r->value || fn_stack_.empty()) { stamp_const(r->const_check, const_none, {}); return; }
        const fn_ctx& fn = fn_stack_.back();
        const bool by_reference = r->binds_reference || (fn.ret && fn.ret->base_type == script_value_type::jai_reference_type);
        if (!by_reference) { stamp_const(r->const_check, const_none, {}); return; }
        const const_hit h = const_path(r->value.get());
        const std::string declared = fn.ret ? fn.ret->canonical_name() : std::string("auto&");
        auto text = [&] { return detail::const_return_text(r->value.get(), h.root, fn.name, declared); };
        stamp_const(r->const_check, h.verdict, text);
        if (h.verdict == const_raises) { diag(diag_level::error, r->location, detail::const_lowercase(text()), h.line, h.column); }
    }

    // a const passed to a reference parameter the callee writes through (a const T& parameter takes it)
    void check_const_arguments(call_expr* c, const std::vector<const function_decl*>* ov, std::string_view ov_class, bool free_callee,
                               const std::vector<ctype>& args) {
        if (c->const_check.verdict != const_none || !ov) { return; }
        for (size_t i = 0; i < c->arguments.size() && i < args.size(); ++i) {
            const const_hit h = const_path(c->arguments[i].get());
            if (h.verdict != const_raises) { continue; }
            std::vector<const function_decl*> by_ref;
            bool all_ref = false;
            for (const auto* fn : *ov) {
                if (!script_fn_viable(fn, args, !free_callee && arity_rivals(*ov, args.size()))) { continue; }
                all_ref = !fn->is_coroutine && i < fn->parameters.size() && fn->parameters[i].is_reference && !fn->parameters[i].is_const;
                if (!all_ref) { break; }
                by_ref.push_back(fn);
            }
            if (!all_ref || by_ref.empty()) { continue; }
            if (!ov_class.empty()) {
                for (const auto* fn : overrides_of(ov_class, by_ref.front()->name)) { by_ref.push_back(fn); }
            }
            for (const auto* fn : by_ref) {
                if (i >= fn->parameters.size() || !fn->parameters[i].is_reference || fn->parameters[i].is_const || fn->is_coroutine) { continue; }
                const ref_param_write w = ref_writes(fn, i);
                if (!w.writes) { continue; }
                const std::string text = detail::const_argument_text(c->arguments[i].get(), h.root, fn->parameters[i].name, fn->shown_name(), w.line);
                stamp_const(c->const_check, const_raises, [&] { return text; });
                diag(diag_level::error, c->arguments[i]->location, "'" + std::string(fn->name) + "' writes through parameter '" + fn->parameters[i].name +
                     "' (line " + std::to_string(w.line) + "); '" + h.root + "' is const", h.line, h.column);
                return;
            }
        }
    }

    // ++/-- on a bare identifier (member targets desugar to += 1 and take the assignment rule)
    void check_incdec_target(unary_expr* u) {
        check_const_store(u->const_check, u->operand.get(), u->location);
        if (!class_ctx_ || u->operand->get_type() != node_type::identifier_expr) { return; }
        auto* id = static_cast<identifier_expr*>(u->operand.get());
        if (id->global_qualified || find_named(id)) { return; }
        member_hit hit = lookup_member(class_ctx_name_, id->name, /*want_static=*/false);
        if (hit.found && access_denied(hit, id->name, id->location)) { return; }
        if (hit.found && hit.read_only) { read_only_store(u->location, id->name); }
    }

    // -------------------------------------------------------------- type proofs
    // Every rule writes its node field on EVERY visit (a stamp
    // or the no-proof default), so a re-check can add or drop proofs but never leaves a
    // stale one the pass no longer derives.

    bool stamping() const { return proofs_ == proof_mode::stamp; }
    bool semantic_walk() const { return !collect_diagnostics_ && proofs_ != proof_mode::leave; }

    template <class F, class V>
    void stamp(F& field, V value, V def) {
        if (proofs_ == proof_mode::stamp) { field = static_cast<F>(value); }
        else if (proofs_ == proof_mode::clear) { field = static_cast<F>(def); }
    }

    // Proof certainty: the payload guard (detail::same_type_payload) is TOTAL for these kinds -
    // every dynamic value the site can hold is either the identity case or falls to the ladder.
    bool known_class(std::string_view name) { return classes_.count(name) || engine_class(name) != nullptr; }

    // A class, enum or host type of that name anywhere the program or the host declares one
    bool known_type_name(std::string_view name) {
        if (known_class(name) || find_sclass(name) || enums_.count(name)) { return true; }
        const std::string n(name);
        if (program_enums_) {
            eng_.note(n);
            if (program_enums_->count(n)) { return true; }
        }
        return eng_.is_type_name(n) || eng_.is_enum_name(n) || (n.find("::") != std::string::npos && extra_global(n));   // a class another unit's namespace declares
    }

    // ns::E where this unit declares enum E in namespace ns: the name is the enum's value table, and ns::E::v one of its values
    const enum_decl* namespace_enum(const member_expr* m) {
        if (!m->is_static || enums_.empty()) { return nullptr; }
        const auto it = enums_.find(qualified_spelling(m));
        return it != enums_.end() ? it->second : nullptr;
    }

    // C++ [temp.inst] has the compiler make any instantiation a program names; a host binds the ones it compiled, so a
    // spelling of its template it did not bind names no class, wherever a type is written
    bool unbound_instantiation(const type_info* ti, const source_location& loc) {
        if (ti->base_type != script_value_type::jai_object_type || ti->type_params.empty() || known_class(ti->type_name)) { return false; }
        const std::string text = eng_.unbound_instantiation_text(ti->type_name);
        if (text.empty()) { return false; }
        diag(diag_level::error, loc, text);
        return true;
    }
    // The same through a type an expression spells (new, make_shared, a construction, a cast): its arguments, pointee and elements
    bool report_unbound_instantiation(const type_info* ti, const source_location& loc) {
        if (!ti) { return false; }
        if (unbound_instantiation(ti, loc)) { return true; }
        bool reported = false;
        for (const auto& param : ti->type_params) { reported = report_unbound_instantiation(param.get(), loc) || reported; }
        return reported;
    }

    // C++ [basic.lookup]: every type a declaration spells names a type; a template instance's own name is the host's (one
    // the host bound), its arguments are checked. A dynamic-path include can declare anything, as for an undeclared identifier.
    bool unknown_bare_type(const type_info* ti) {
        return ti && ti->base_type == script_value_type::jai_object_type && ti->type_params.empty() && !ti->type_name.empty() && !known_type_name(ti->type_name);
    }
    void check_spelled_type(const type_info* ti, const source_location& loc) {
        if (!ti) { return; }
        const bool object = ti->base_type == script_value_type::jai_object_type;
        if (object && unbound_instantiation(ti, loc)) { return; }
        if (object && ti->type_params.empty() && refuse_bare_generic_class(ti->type_name, loc)) { return; }
        if (unknown_bare_type(ti)) {
            const std::string name(ti->type_name);
            const std::string math = "math::" + name;
            const std::string hint = math_type_from_name(math) != script_value_type::jai_null_type
                ? "; did you mean '" + math + "' (using " + math + ";)?" : "";
            diag(has_dynamic_includes_ ? diag_level::warning : diag_level::error, loc, "unknown type '" + name + "'" + hint);
            return;
        }
        if (ti->base_type == script_value_type::jai_shared_ptr_type && !ti->type_params.empty() && ti->type_params[0] &&
            ti->type_params[0]->base_type == script_value_type::jai_object_type) {
            note_handle_held(ti->type_params[0]->type_name, loc);
        }
        for (const auto& param : ti->type_params) { check_spelled_type(param.get(), loc); }
    }

    // A class method named like a shared_ptr builtin (reset, use_count, ...) wins on that class's handles, so the builtin is
    // lost only where the program holds the class by a handle: a class held by value alone keeps its reset() unwarned
    std::vector<std::pair<std::string, source_location>> handle_held_classes_;   // first handle spelling of each class, in walk order
    std::unordered_set<std::string> handle_held_names_;
    void note_handle_held(std::string_view cls, const source_location& at) {
        if (!cls.empty() && handle_held_names_.emplace(cls).second) { handle_held_classes_.emplace_back(std::string(cls), at); }
    }

    void warn_handle_shadowed_builtins() {
        std::unordered_set<std::string> warned;
        for (const auto& [cls, at] : handle_held_classes_) { warn_handle_shadows_along(cls, at, warned, 0); }
    }

    // The warning stands at the shadowing method when this unit declares the class, else at the unit's first handle of it
    void warn_handle_shadows_along(std::string_view cls, const source_location& held_at, std::unordered_set<std::string>& warned, int depth) {
        if (depth > 32) { return; }
        auto warn = [&](std::string_view method, const source_location& at) {
            if (!warned.insert(std::string(cls) + "::" + std::string(method)).second) { return; }
            const std::string m(method);
            diag(diag_level::warning, at, "class method '" + m + "' shadows the builtin shared_ptr " + m +
                 "() - the builtin is unreachable on " + std::string(cls) + " instances; rename the class method to reach it");
        };
        if (const sclass* c = find_sclass(cls)) {
            for (std::string_view b : k_shadowable_handle_method_names) {
                auto m = c->methods.find(b);
                if (m != c->methods.end() && !m->second.empty()) { warn(b, c->foreign ? held_at : m->second.front()->location); }
            }
            for (const auto& base : c->decl->base_classes) { warn_handle_shadows_along(base, held_at, warned, depth + 1); }
            return;
        }
        auto def = engine_class(cls);
        if (!def || !def->is_script_class()) { return; }
        for (std::string_view b : k_shadowable_handle_method_names) {
            if (def->has_method(eng_.symbolize(b))) { warn(b, held_at); }
        }
        for (const auto& parent : def->get_parent_classes()) {
            if (parent) { warn_handle_shadows_along(parent->get_name(), held_at, warned, depth + 1); }
        }
    }

    // cast<C>(h) on a handle or reference operand deep-copies (C++ value semantics - the
    // runtime is right); the warning names the aliasing spellings. Warning severity: never gates.
    enum class cast_source : uint8_t { plain, handle, reference };
    var_entry* cast_operand_local(const expression* operand) {
        if (!operand || operand->get_type() != node_type::identifier_expr) { return nullptr; }
        auto* id = static_cast<const identifier_expr*>(operand);
        return id->global_qualified ? nullptr : find_local(id->name);
    }
    cast_source cast_operand_source(const expression* operand, const ctype& t) {
        if (t.ti && t.ti->base_type == script_value_type::jai_shared_ptr_type) { return cast_source::handle; }
        var_entry* local = cast_operand_local(operand);
        if (!local || (!local->type.is_unknown() && local->type.kind != ctype::object_k)) { return cast_source::plain; }
        if (local->is_reference) { return cast_source::reference; }
        return local->is_handle ? cast_source::handle : cast_source::plain;
    }
    static std::string cast_operand_spelling(const expression* e) {
        std::string tail;
        while (e && e->get_type() == node_type::member_expr) {
            auto* m = static_cast<const member_expr*>(e);
            if (m->is_static) { return "..."; }
            tail = std::string(m->null_safe ? "?." : m->is_arrow ? "->" : ".") + std::string(m->member) + tail;
            e = m->object.get();
        }
        if (e && e->get_type() == node_type::identifier_expr) {
            auto* id = static_cast<const identifier_expr*>(e);
            return (id->global_qualified ? "::" : "") + std::string(id->name) + tail;
        }
        if (e && e->get_type() == node_type::this_expr) { return "this" + tail; }
        return "...";
    }
    void warn_cast_copy(const cast_expr* c, const ctype& operand_t) {
        const expression* operand = c->operand.get();
        if (cast_operand_source(operand, operand_t) == cast_source::plain) { return; }
        size_t related_line = 0, related_column = 0;
        if (var_entry* local = cast_operand_local(operand)) {
            related_line = local->line;
            related_column = local->column;
        }
        const std::string cls = c->target->canonical_name();
        const std::string src = cast_operand_spelling(operand);
        const bool handle_operand = cast_operand_source(operand, operand_t) == cast_source::handle;
        diag(diag_level::warning, c->location,
             "cast<" + cls + ">(" + src + ") copies the instance - use cast<" + cls + "&>(" + src + ") to alias it" +
             (handle_operand ? " or cast<shared_ptr<" + cls + ">>(" + src + ") to share it" : ""),
             related_line, related_column);
    }
    // A handle form over a class value a slot holds, used as an object: a handle never aliases a value slot. Read for its truth it is
    // the test; a var or a reference may hold a handle, and the run decides
    void refuse_handle_of_value(const cast_expr* c, const ctype& operand) {
        if (c->by_reference || c->truth_read || !handle_type(c->target.get()) || !c->target->element_type()) { return; }
        if (operand.kind != ctype::object_k || operand.cls.empty() || operand.tentative || operand.narrowed) { return; }
        if (operand.ti && (operand.ti->base_type == script_value_type::jai_shared_ptr_type || operand.ti->base_type == script_value_type::jai_weak_ptr_type)) { return; }
        if (!detail::cast_lvalue_source(c->operand.get()) || cast_operand_source(c->operand.get(), operand) != cast_source::plain) { return; }
        const std::string name(c->target->element_type()->type_name);
        const std::string src = cast_operand_spelling(c->operand.get());
        const std::string test = "dynamic_cast<" + name + ">(" + src + ")";
        diag(diag_level::error, c->location, (c->dynamic ? test : "cast<shared_ptr<" + name + ">>(" + src + ")") + ": '" + src +
             "' is a class value, and a handle never aliases one; test it with if (" + test + "), or take a reference with dynamic_cast<" + name + "&>(" + src + ")");
    }
    static bool certain_scalar(const ctype& t) {
        return (!t.tentative || t.element_proof) && (t.kind == ctype::int_k || t.kind == ctype::float_k || t.kind == ctype::bool_k ||
                                t.kind == ctype::char_k || t.kind == ctype::string_k);
    }
    bool certain_object(const ctype& t) {
        return !t.tentative && t.kind == ctype::object_k && t.ti &&
               t.ti->base_type == script_value_type::jai_object_type && !t.ti->type_name.empty() && known_class(t.cls);
    }
    bool certain(const ctype& t) { return certain_scalar(t) || certain_object(t); }
    // same_type: certain on both sides, identical kind, and for objects the SAME interned spelling.
    bool same_type(const ctype& target, const ctype& rhs) {
        if (!certain(target) || !certain(rhs) || target.kind != rhs.kind) { return false; }
        if (target.kind == ctype::int_k) { return target.cls.empty() && rhs.cls.empty(); }   // an enum's value sheds or takes its stamp in the table
        return target.kind != ctype::object_k || target.ti == rhs.ti;
    }
    // Constructor-call result: the parser's interned object spelling for `name`.
    ctype object_of(std::string_view name) {
        ctype t = ctype::object(name);
        t.ti = eng_.get_type_info_object(std::string(name));
        return t;
    }
    static uint8_t operand_class_of(const ctype& l, const ctype& r) {
        if ((l.tentative && !l.element_proof) || (r.tentative && !r.element_proof)) { return operand_class_unknown; }
        const bool li = l.kind == ctype::int_k, lf = l.kind == ctype::float_k;
        const bool ri = r.kind == ctype::int_k, rf = r.kind == ctype::float_k;
        if (li && ri) { return operand_class_int_int; }
        if (lf && rf) { return operand_class_float_float; }
        if (li && rf) { return operand_class_int_float; }
        if (lf && ri) { return operand_class_float_int; }
        if (l.kind == ctype::string_k && r.kind == ctype::string_k) { return operand_class_string_string; }
        if (l.kind == ctype::bool_k && r.kind == ctype::bool_k) { return operand_class_bool_bool; }
        return operand_class_unknown;
    }

    // Bare names reach this-fields here: method/ctor body or [this] lambda, not a static
    // method, not a ctor initializer list, no includes (which can define anything).
    bool bare_field_context() const {
        return class_ctx_ && !fn_stack_.empty() && fn_stack_.back().sees_fields &&
               !fn_stack_.back().is_static_method && !fn_stack_.back().in_ctor_initializers && !has_includes_;
    }

    // a scalar (int, float, bool) as its base type, an array of scalars as jai_array_type | the element's base type << 8, else 0
    static uint16_t certain_kind(const type_info* t) {
        if (!t || t->is_reference()) return 0;
        auto scalar = [](script_value_type b) { return b == script_value_type::jai_int_type || b == script_value_type::jai_float_type || b == script_value_type::jai_bool_type; };
        if (scalar(t->base_type)) return static_cast<uint16_t>(t->base_type);
        if (t->base_type != script_value_type::jai_array_type) return 0;
        const type_info* el = t->element_type().get();
        if (!el || el->is_reference() || !scalar(el->base_type)) return 0;
        return static_cast<uint16_t>(static_cast<uint16_t>(script_value_type::jai_array_type) | (static_cast<uint16_t>(el->base_type) << 8));
    }
    // a declared type a proof may rest on: spelled, not a reference, not var, auto or a function
    static bool certain_type(const type_info* t) {
        if (!t || t->is_reference()) return false;
        switch (t->base_type) {
        case script_value_type::jai_any_type: case script_value_type::jai_function_type: case script_value_type::jai_null_type: return false;
        default: return t->type_name != "auto";
        }
    }
    static proof_global proof_of(uint64_t symbol, const type_info* t) {
        proof_global g; g.symbol = symbol;
        const uint16_t k = certain_kind(t);
        if (k != 0) { g.kind = k; } else { g.kind = proof_global::k_type; g.type = t; }
        return g;
    }
    // a program-scope name whose declared type changes only when its declaring unit runs again: this unit's own typed top-level
    // declaration, or the one spelled declaration another unit of the program makes, of any type a proof may rest on; a name a field
    // could shadow (inside a class) or a runtime include could declare stays uncertain. The name and its proof join deps; declared
    // receives the declared type
    bool certain_global(const identifier_expr* id, std::vector<proof_global>& deps, const type_info** declared = nullptr) {
        if (has_dynamic_includes_ || class_ctx_ || id->symbol_id == UINT64_MAX) return false;
        size_t scope = SIZE_MAX;
        const type_info* t = nullptr;
        if (var_entry* v = find_named(id, &scope)) {
            if (scope != 0 || v->dynamic || v->is_reference || v->is_handle) return false;
            t = v->declared;
        } else {
            if (functions_.count(id->name) || enums_.count(id->name)) return false;
            const variable_decl* g = program_global(std::string(id->name));
            if (!g) return false;
            t = g->type.get();
        }
        if (!certain_type(t)) return false;
        deps.push_back(proof_of(id->symbol_id, t));
        if (declared) { *declared = t; }
        return true;
    }
    // an argument whose static type an include cannot change: a literal, a local or a parameter (a frame slot), a certain program-scope
    // name (certain_global; its kind recorded in deps), an element of a local or certain array, and arithmetic, a unary, a ternary, a cast
    // or a math intrinsic over those
    bool certain_argument(const expression* e, std::vector<proof_global>& deps) {
        if (!e) return false;
        switch (e->get_type()) {
        case node_type::literal_expr: return true;
        case node_type::identifier_expr: {
            auto* id = static_cast<const identifier_expr*>(e);
            return id->slot_index != SIZE_MAX || certain_global(id, deps);
        }
        case node_type::binary_expr: {
            auto* b = static_cast<const binary_expr*>(e);
            if (b->op.type == token_type::left_bracket) {
                if (!b->left || b->left->get_type() != node_type::identifier_expr) return false;
                auto* base = static_cast<const identifier_expr*>(b->left.get());
                const type_info* declared = nullptr;
                if (base->slot_index == SIZE_MAX) {
                    if (!certain_global(base, deps, &declared)) return false;
                } else {
                    const var_entry* v = find_named(base);   // a local declared an array by value: a reference's declared type no bind checks
                    if (!v || v->dynamic || v->is_reference || !certain_type(v->declared)) return false;
                    declared = v->declared;
                }
                if (declared->base_type != script_value_type::jai_array_type) return false;
                return certain_argument(b->right.get(), deps);
            }
            return certain_argument(b->left.get(), deps) && certain_argument(b->right.get(), deps);
        }
        case node_type::unary_expr: return certain_argument(static_cast<const unary_expr*>(e)->operand.get(), deps);
        case node_type::ternary_expr: { auto* x = static_cast<const ternary_expr*>(e); return certain_argument(x->condition.get(), deps) && certain_argument(x->then_expression.get(), deps) && certain_argument(x->else_expression.get(), deps); }
        case node_type::cast_expr: return certain_argument(static_cast<const cast_expr*>(e)->operand.get(), deps);
        case node_type::call_expr: {
            auto* ce = static_cast<const call_expr*>(e);
            if (ce->callee && ce->callee->get_type() == node_type::identifier_expr) {
                // a free script function with a declared return: its result converts to that type whatever the arguments; the name and the
                // return join deps (proof_global::k_function), since a redefinition may declare another return type
                auto* id = static_cast<const identifier_expr*>(ce->callee.get());
                if (has_dynamic_includes_ || class_ctx_ || id->slot_index != SIZE_MAX || id->global_qualified || id->symbol_id == UINT64_MAX) return false;
                const auto fn = ce->proven_callee_decl.lock();
                if (!fn || fn->is_coroutine) return false;
                if (!certain_type(fn->return_type.get())) return false;
                proof_global g = proof_of(id->symbol_id, fn->return_type.get());
                if ((g.kind & 0xFF) == static_cast<uint16_t>(script_value_type::jai_array_type) && g.type == nullptr) { g.kind = proof_global::k_type; g.type = fn->return_type.get(); }
                g.kind = static_cast<uint16_t>(g.kind | proof_global::k_function);
                deps.push_back(g);
                return true;
            }
            if (!ce->callee || ce->callee->get_type() != node_type::member_expr || detail::math_intrinsic_for_call(static_cast<const member_expr*>(ce->callee.get())) == detail::math_fn::none) return false;
            for (const auto& a : ce->arguments) { if (!certain_argument(a.get(), deps)) return false; }
            return true;
        }
        default: return false;
        }
    }

    // C1: one statically resolved same-script callee, exact arity, no defaults in play: the inliner's
    // identity and per-argument same_type (an untyped/var parameter binds as identity, a reference none).
    void stamp_call_proofs(call_expr* c, const std::vector<const function_decl*>* ov, const std::vector<ctype>& args, bool free_callee) {
        if (proofs_ == proof_mode::leave) { return; }
        const size_t n = c->arguments.size();
        const function_decl* fn = nullptr;
        const bool certain_program = !has_includes_;   // the statistics keep the includes-free gate; the inliner's decl and its argument proofs go by the argument
        if (stamping() && ov && ov->size() == 1) {
            fn = (*ov)[0];
            auto [lo, hi] = arity_range(fn);
            if (!(fn->body && !fn->is_coroutine && lo == n && hi == n && args.size() == n)) { fn = nullptr; }
        }
        c->proven_callee_decl = fn && free_callee ? fn->weak_from_this() : std::weak_ptr<const function_decl>();
        if (!fn) {
            c->static_arg_proofs.clear();
            c->proof_globals.clear();
            return;
        }
        c->static_arg_proofs.assign(n, proof_none);
        c->proof_globals.clear();
        std::vector<proof_global> deps;
        for (size_t i = 0; i < n; ++i) {
            const parameter& p = fn->parameters[i];
            if (p.is_reference) { continue; }   // a reference parameter binds, never converts: no proof, no count
            const bool untyped = !p.type || p.type->base_type == script_value_type::jai_any_type;
            deps.clear();
            if (!(certain_program || certain_argument(c->arguments[i].get(), deps))) { continue; }
            if (untyped || same_type(ctype_from_type_info(p.type.get()), args[i])) {
                c->static_arg_proofs[i] = proof_same_type;
                if (certain_program) { ++counts_.args; }
                for (const auto& d : deps) { if (std::find(c->proof_globals.begin(), c->proof_globals.end(), d) == c->proof_globals.end()) { c->proof_globals.push_back(d); } }
            }
        }
    }

    // -------------------------------------------------------------- assignability

    // Mirrors detail::convert_for_typed_slot (the one runtime table for declarations,
    // assignment, parameters, returns and fields): what a store into a `dst`-typed slot
    // accepts (with conversion). Unknown on either side accepts.
    // References alias storage: conversions that create a value cannot bind T&.
    bool reference_type_compatible(const ctype& src, const ctype& dst) {
        if (src.is_unknown() || dst.is_unknown()) { return true; }
        if (src.origin && mixed_ternaries_.count(src.origin)) { return true; }   // a conditional binds an arm, never its common type (its own diagnostic)
        if (src.kind != dst.kind) { return false; }
        if (src.ti && dst.ti) {
            if (src.ti->base_type == script_value_type::jai_shared_ptr_type && dst.ti->base_type == script_value_type::jai_object_type) {
                return class_relation(src.cls, dst.cls) != 0;   // a handle names its object: a T& binds through it, as through a handle variable
            }
            if (src.ti->base_type != dst.ti->base_type) { return false; }
            if (src.ti->base_type == script_value_type::jai_object_type) {
                return class_relation(src.cls, dst.cls) != 0;
            }
            return src.ti->equals(*dst.ti);
        }
        if (src.kind == ctype::object_k) { return class_relation(src.cls, dst.cls) != 0; }
        return true;   // same scalar kind, or unavailable container parameters
    }

    bool weak_initializer_compatible(const ctype& src, const ctype& dst) {
        if (src.is_unknown() || src.kind == ctype::null_k) { return true; }
        if (!src.ti) {
            // An object spelling can lose its value/handle distinction at a mixed
            // conditional. Do not reject a handle whose wrapper is unknown.
            return src.kind == ctype::weak_k || src.kind == ctype::object_k;
        }
        if (src.ti->base_type != script_value_type::jai_shared_ptr_type &&
            src.ti->base_type != script_value_type::jai_weak_ptr_type) {
            return false;
        }
        const type_info* from = src.ti->element_type();
        const type_info* to = dst.ti ? dst.ti->element_type().get() : nullptr;
        return reference_type_compatible(ctype_from_type_info(from), ctype_from_type_info(to));
    }

    // The origin is the expression at this use, never a variable's old initializer.
    // Only literal elements need a saved ctype; revisiting their AST could duplicate
    // lambda walks, diagnostics and proof stamps.
    std::unordered_map<const expression*, ctype> literal_element_types_;

    static std::optional<script_float> literal_float(const expression* expr) {
        if (!expr) { return std::nullopt; }
        if (expr->get_type() == node_type::literal_expr) {
            const auto* lit = static_cast<const literal_expr*>(expr);
            if (lit->value.is_float()) { return lit->value.unchecked_as_float(); }
            return std::nullopt;
        }
        if (expr->get_type() == node_type::unary_expr) {
            const auto* unary = static_cast<const unary_expr*>(expr);
            if (unary->op.type == token_type::plus || unary->op.type == token_type::minus) {
                if (auto value = literal_float(unary->operand.get())) {
                    return unary->op.type == token_type::minus ? -*value : *value;
                }
            }
        }
        return std::nullopt;
    }

    static std::optional<script_int> literal_int(const expression* expr) {
        if (!expr || expr->get_type() != node_type::literal_expr) { return std::nullopt; }
        const auto* lit = static_cast<const literal_expr*>(expr);
        if (lit->value.is_int()) { return lit->value.unchecked_as_int(); }
        return std::nullopt;
    }

    // an int literal outside a uint8 / int8 element's range: the store raises, so the checker says so at load
    static std::string byte_literal_issue(const ctype& src, const type_info* target) {
        if (!target || !target->is_byte_element() || src.kind != ctype::int_k) { return {}; }
        const auto v = literal_int(src.origin);
        if (!v) { return {}; }
        const bool fits = target->is_signed ? (*v >= -128 && *v <= 127) : (*v >= 0 && *v <= 255);
        if (fits) { return {}; }
        return std::to_string(*v) + " is outside " + target->canonical_name() + " (" + (target->is_signed ? "-128..127" : "0..255") + ")";
    }

    // a string literal into a char slot holds exactly one character (the conversion's text otherwise, detail/numeric_conversion.hpp)
    static bool literal_char_text_refused(const ctype& src, const ctype& dst) {
        if (dst.kind != ctype::char_k || src.kind != ctype::string_k || !src.origin || src.origin->get_type() != node_type::literal_expr) { return false; }
        const script_value& value = static_cast<const literal_expr*>(src.origin)->value;
        return value.is_string() && value.unchecked_as_string().size() != 1;
    }
    static constexpr const char* float_to_int_fix = "cast<int>(x) truncates toward zero, math::ifloor(x) rounds down";
    static bool vec_narrows(const ctype& src, const ctype& dst) {
        return src.kind == ctype::math_k && dst.kind == ctype::math_k && detail::math::narrows(math_type_from_name(src.cls), math_type_from_name(dst.cls));
    }
    static std::string vec_narrowing_fix(const ctype& dst) { return ": cast<" + std::string(dst.cls) + ">(v) truncates each component toward zero"; }
    static std::string char_text_reason() { return std::string(detail::char_of_text_refused.substr(detail::char_of_text_refused.find(": ") + 2)); }

    ctype declared_container_receiver(const expression* expr, ctype inferred) {
        if (!inferred.is_unknown() || !expr || expr->get_type() != node_type::identifier_expr) {
            return inferred;
        }
        const auto* id = static_cast<const identifier_expr*>(expr);
        const auto* local = find_local(id->name);
        if (!local || !local->is_reference || !local->declared) { return inferred; }
        ctype result = ctype_from_type_info(local->declared);
        if (result.kind != ctype::array_k && result.kind != ctype::map_k) { return inferred; }
        result.tentative = true;
        result.alias_proof = true;   // a typed reference: the binder holds it to its type, the typed rows guard the kind
        return result;
    }

    ctype container_value_type(const ctype& container) {
        // a string's element is what its subscript gives: a char, and a utf8_string's a utf8_string
        if ((container.kind == ctype::string_k || container.kind == ctype::utf8_k) && kind_stated(container)) {
            ctype character = ctype::make(container.kind == ctype::string_k ? ctype::char_k : ctype::utf8_k);
            character.tentative = true;
            character.declared_result = true;
            character.declared_slot = container.declared_slot;
            return character;
        }
        if (!container.ti) { return {}; }
        const type_info* element = container.kind == ctype::array_k
            ? container.ti->element_type().get()
            : (container.kind == ctype::map_k ? container.ti->value_type().get() : nullptr);
        ctype result = ctype_from_type_info(element);
        // Tentative for diagnostics; a scalar element of a declared, non-tentative typed array is a
        // proof source for the operand class (the typed rows guard it at runtime)
        result.tentative = true;
        // only an array's element proves (a typed map's value could, a missing key raising, but it is not claimed yet)
        result.element_proof = container.kind == ctype::array_k && (!container.tentative || container.alias_proof) &&
                               (result.kind == ctype::int_k || result.kind == ctype::float_k);
        result.declared_slot = container.declared_slot;
        return result;
    }

    ctype recorded_literal_element(const expression* expr) const {
        const auto found = literal_element_types_.find(expr);
        return found == literal_element_types_.end() ? ctype{} : found->second;
    }

    // resize(size, value), assign(size, value), reserve(size) and fill(value) on an array (array<T>(size, value) is array::resize):
    // the size is an int and the value fits the element type, as push's argument does
    void check_array_sizing_arguments(std::string_view a_method, const ctype& a_array, const ctype* a_args, size_t a_count, const source_location& a_at) {
        const bool sized = a_method == "resize" || a_method == "assign" || a_method == "reserve";
        if (!sized && a_method != "fill") { return; }
        const std::string method(a_method);
        if (sized && a_count >= 1 && !a_args[0].is_unknown() && a_args[0].kind != ctype::int_k) {
            diag(diag_level::error, a_at, method + "() size must be an int, got '" + ctype_name(a_args[0]) + "'");
        }
        const size_t value_at = a_method == "fill" ? 0 : 1;
        if (!a_array.ti || a_method == "reserve" || a_count <= value_at) { return; }
        const auto issue = container_element_issue(a_args[value_at], a_array.ti->element_type().get());
        if (!issue.empty()) { diag(diag_level::error, a_at, method + "() cannot store into '" + ctype_name(a_array) + "': " + issue); }
    }

    // An enum's name read as a value is its value table, a map<string,int> (the enum's run-time value)
    ctype enum_table(const ctype& t) {
        if (t.kind != ctype::enum_ref_k) { return t; }
        ctype result = ctype::make(ctype::map_k);
        result.ti = eng_.get_type_info_map(eng_.get_type_info_string(), eng_.get_type_info_int());
        result.tentative = t.tentative;
        return result;
    }

    std::string container_element_issue(const ctype& a_src, const type_info* target, int depth = 0) {
        const ctype src = enum_table(a_src);
        const ctype dst = ctype_from_type_info(target);
        if (src.is_unknown() || dst.is_unknown() || depth > 512) { return {}; }
        if (literal_char_text_refused(src, dst)) { return char_text_reason(); }
        if (auto issue = byte_literal_issue(src, target); !issue.empty()) { return issue; }
        if (dst.kind == ctype::array_k || dst.kind == ctype::map_k) {
            return container_issue(src, dst, depth + 1);
        }
        if (std::string issue = value_into_handle_issue(src, target); !issue.empty()) { return issue; }
        bool accepted = src.kind == dst.kind || char_text_slot(src, dst);
        if (vec_narrows(src, dst)) { return "expected '" + ctype_name(dst) + "', got '" + ctype_name(src) + "'" + vec_narrowing_fix(dst); }
        if (enum_value(dst)) {
            if (src.kind == ctype::int_k && src.cls == dst.cls) { return {}; }
            return "expected '" + ctype_name(dst) + "', got '" + ctype_name(src) + "'";
        }
        if (src.kind == ctype::int_k && dst.kind == ctype::float_k) { accepted = true; }
        if ((src.kind == ctype::bool_k && dst.kind == ctype::int_k) ||
            (src.kind == ctype::int_k && dst.kind == ctype::bool_k)) { accepted = true; }
        if (dst.kind == ctype::object_k && target->base_type == script_value_type::jai_object_type) {
            accepted = src.kind == ctype::object_k && class_relation(src.cls, dst.cls) != 0;
        } else if (target->base_type == script_value_type::jai_shared_ptr_type) {
            // A shared-handle element takes null or a same/derived live instance,
            // including an instance already carried by a shared handle.
            accepted = src.kind == ctype::null_k ||
                (src.kind == ctype::object_k && class_relation(src.cls, dst.cls) != 0);
        } else if (target->base_type == script_value_type::jai_weak_ptr_type) {
            // A weak element takes null, a weak handle, or a shared handle of T or a subclass (C++: weak = shared, the declaration's row)
            accepted = src.kind == ctype::null_k || src.kind == ctype::weak_k ||
                (src.kind == ctype::object_k && (!src.ti || src.ti->base_type == script_value_type::jai_shared_ptr_type) &&
                 weak_initializer_compatible(src, dst));
        }
        if (accepted && dst.kind == ctype::function_k) {
            if (const std::string issue = signature_issue(src, dst); !issue.empty()) {
                return "expected '" + target->canonical_name() + "', got '" + ctype_name(src) + "': " + issue;
            }
        }
        if (accepted && dst.kind == ctype::coroutine_k) { accepted = assignable(src, dst); }
        if (accepted) { return {}; }
        std::string result = "expected '" + target->canonical_name() + "', got '" + ctype_name(src) + "'";
        if (dst.kind == ctype::int_k && src.kind == ctype::float_k) { result += std::string(": ") + float_to_int_fix; }
        return result;
    }

    static script_value_type scalar_key_kind(ctype::kind_t kind) {
        switch (kind) {
            case ctype::int_k: return script_value_type::jai_int_type;
            case ctype::float_k: return script_value_type::jai_float_type;
            case ctype::bool_k: return script_value_type::jai_bool_type;
            case ctype::char_k: return script_value_type::jai_char_type;
            case ctype::string_k: return script_value_type::jai_string_type;
            default: return script_value_type::jai_invalid_type;
        }
    }

    // m[k] on a map whose declared key type is a scalar: the key boundary checks the kind before any lookup (detail::map_key_kind_admits),
    // and a char key takes a string of one character, so a known key the boundary refuses is the run time's text
    void check_map_subscript_key(const binary_expr* b, const ctype& cont, const ctype& key) {
        if (cont.kind != ctype::map_k || cont.tentative || cont.diagnostic_only || !cont.ti || key.tentative || key.diagnostic_only) { return; }
        const type_info_ptr& declared = cont.ti->key_type();
        if (!declared) { return; }
        if (const ctype slot_key = ctype_from_type_info(declared.get()); enum_value(slot_key)) {
            if (!key.is_unknown() && (key.kind != ctype::int_k || key.cls != slot_key.cls)) {
                diag(diag_level::error, b->right->location, "Cannot use '" + ctype_name(key) + "' as a key of type '" + ctype_name(slot_key) + "'");
            }
            return;
        }
        const script_value_type slot = declared->base_type;
        const script_value_type given = scalar_key_kind(key.kind);
        if (given == script_value_type::jai_invalid_type || scalar_key_kind(ctype_from_type_info(declared.get()).kind) != slot) { return; }
        if (!detail::map_key_kind_admits(given, slot)) {
            diag(diag_level::error, b->right->location, "Cannot use '" + std::string(base_type_spelling(given)) + "' as a key of type '" + detail::type_info_display_name(declared) + "'");
            return;
        }
        if (slot == script_value_type::jai_char_type && given == script_value_type::jai_string_type && b->right->get_type() == node_type::literal_expr) {
            const script_value& text = static_cast<const literal_expr*>(b->right.get())->value;
            if (text.is_string() && text.unchecked_as_string().size() != 1) { diag(diag_level::error, b->right->location, std::string(detail::char_of_text_refused)); }
        }
    }

    std::string map_key_issue(const ctype& a_src, const type_info* target) {
        const ctype src = enum_table(a_src);
        const ctype dst = ctype_from_type_info(target);
        if (src.is_unknown() || dst.is_unknown()) { return {}; }
        // Keys never coerce numerically: a conversion could merge distinct entries. A char and its one-character string are one
        // key already, so each converts to the other's kind. The runtime key boundary checks the kind, not a pointee or element type.
        if (literal_char_text_refused(src, dst)) { return char_text_reason(); }
        if ((src.kind == ctype::char_k && dst.kind == ctype::string_k) || (src.kind == ctype::string_k && dst.kind == ctype::char_k)) { return {}; }
        const bool accepted = enum_value(dst) ? src.kind == ctype::int_k && src.cls == dst.cls
                            : src.kind == dst.kind && (!src.ti || enum_value(src) || src.ti->base_type == target->base_type);
        if (accepted) { return {}; }
        return "expected '" + target->canonical_name() + "', got '" + ctype_name(src) + "'";
    }

    std::string container_issue(const ctype& a_src, const ctype& dst, int depth = 0) {
        const ctype src = enum_table(a_src);
        if (src.is_unknown() || dst.is_unknown() || depth > 512) { return {}; }
        if (src.kind != dst.kind) {
            return "expected '" + ctype_name(dst) + "', got '" + ctype_name(src) + "'";
        }
        if (!dst.ti) { return {}; }
        if (dst.kind == ctype::array_k) {
            const auto* target = dst.ti->element_type().get();
            if (src.origin && src.origin->get_type() == node_type::array_literal_expr) {
                const auto* literal = static_cast<const array_literal_expr*>(src.origin);
                for (size_t i = 0; i < literal->elements.size(); ++i) {
                    auto issue = container_element_issue(recorded_literal_element(literal->elements[i].get()), target, depth + 1);
                    if (!issue.empty()) { return "array element " + std::to_string(i) + ": " + issue; }
                }
            } else if (src.ti) {
                auto issue = container_element_issue(ctype_from_type_info(src.ti->element_type().get()), target, depth + 1);
                if (!issue.empty()) { return "array element type: " + issue; }
            }
            return {};
        }
        if (dst.kind == ctype::map_k) {
            const auto* key = dst.ti->key_type().get();
            const auto* value = dst.ti->value_type().get();
            if (src.origin && src.origin->get_type() == node_type::map_literal_expr) {
                const auto* literal = static_cast<const map_literal_expr*>(src.origin);
                for (size_t i = 0; i < literal->entries.size(); ++i) {
                    const auto& entry = literal->entries[i];
                    auto issue = map_key_issue(recorded_literal_element(entry.first.get()), key);
                    if (!issue.empty()) { return "map key " + std::to_string(i) + ": " + issue; }
                    issue = container_element_issue(recorded_literal_element(entry.second.get()), value, depth + 1);
                    if (!issue.empty()) { return "map value " + std::to_string(i) + ": " + issue; }
                }
            } else if (src.ti) {
                auto issue = map_key_issue(ctype_from_type_info(src.ti->key_type().get()), key);
                if (!issue.empty()) { return "map key type: " + issue; }
                issue = container_element_issue(ctype_from_type_info(src.ti->value_type().get()), value, depth + 1);
                if (!issue.empty()) { return "map value type: " + issue; }
            }
        }
        return {};
    }

    std::string enum_table_detail(const ctype& e, const ctype& dst) {
        std::string result = ": an enum's name is its value table, a map<string,int>";
        if (dst.kind == ctype::map_k) {
            if (const std::string issue = container_issue(e, dst); !issue.empty()) { result += " (" + issue + ")"; }
        } else if (const enum_decl* decl = find_enum(e.cls); decl && !decl->values.empty()) {
            result += "; name one of its values (" + std::string(e.cls) + "::" + std::string(decl->values.front().first) + ")";
        }
        return result;
    }

    std::string assignment_detail(const ctype& src, const ctype& dst) {
        if (src.kind == ctype::enum_ref_k) { return enum_table_detail(src, dst); }
        if (enum_value(dst)) { return src.kind == ctype::int_k ? ": cast<" + std::string(dst.cls) + ">(n) converts an int that is one of its values" : std::string(); }
        if (dst.kind == ctype::int_k && src.kind == ctype::float_k) { return std::string(": ") + float_to_int_fix; }
        if (vec_narrows(src, dst)) { return vec_narrowing_fix(dst); }
        if (dst.kind == ctype::bool_k) { return ": write !x.empty() or an explicit comparison"; }
        if (src.kind == ctype::null_k && class_value(dst)) { return ": " + never_null_fix(dst); }
        if (literal_char_text_refused(src, dst)) { return ": " + char_text_reason(); }
        if ((dst.kind == ctype::string_k || dst.kind == ctype::utf8_k) && src.kind != ctype::string_k && src.kind != ctype::utf8_k) {
            return ": stringify it with to_string(x) or `${x}`";
        }
        if (dst.kind == ctype::array_k || dst.kind == ctype::map_k) {
            auto issue = container_issue(src, dst);
            if (!issue.empty()) { return ": " + issue; }
        }
        if (dst.kind == ctype::function_k && src.kind == ctype::function_k) {
            auto issue = signature_issue(src, dst);
            if (!issue.empty()) { return ": " + issue; }
        }
        if (dst.kind == ctype::coroutine_k && src.kind == ctype::coroutine_k && dst.yields) {
            return ": a coroutine<" + dst.yields->canonical_name() + "> holds a coroutine declared to yield '" + dst.yields->canonical_name() + "'";
        }
        return {};
    }

    // ---------------------------------------------------------------- signatures

    // What a callable expression declares, as a signature slot judges it: one or more declarations (a function's name), a
    // lambda literal, or another slot's signature
    struct callable_view {
        const std::vector<parameter>* parameters = nullptr;
        const type_info* result = nullptr;      // the declared result; null when none is declared
        ctype inferred{};                       // an undeclared lambda's result where every return agrees
        bool coroutine = false;
        std::string spelled;
    };

    static std::string argument_count_text(size_t required, size_t total) {
        return (required == total ? std::to_string(total) : std::to_string(required) + " to " + std::to_string(total)) +
               (total == 1 ? " argument" : " arguments");
    }
    static bool dynamic_type(const type_info* t) { return !t || t->base_type == script_value_type::jai_any_type; }
    static std::string signature_argument_spelling(const type_info* argument) {
        if (argument && argument->base_type == script_value_type::jai_reference_type) {
            return signature_argument_spelling(argument->element_type().get()) + "&";
        }
        return dynamic_type(argument) ? std::string("var") : argument->canonical_name();
    }

    // A result as a call through a signature delivers it: no conversion runs, so it is the signature's own type, a class the
    // signature's class takes as it stands, or anything where the signature returns void or var
    bool result_fits(const ctype& made, const ctype& wanted) {
        if (wanted.is_unknown() || wanted.kind == ctype::null_k || made.is_unknown()) { return true; }
        if (made.kind != wanted.kind) { return false; }
        switch (wanted.kind) {
            case ctype::object_k: return class_relation(made.cls, wanted.cls) != 0;
            case ctype::int_k: return made.cls == wanted.cls;
            case ctype::math_k: return made.cls == wanted.cls;
            case ctype::array_k: case ctype::map_k: case ctype::weak_k:
                return !made.ti || !wanted.ti || made.ti->id == wanted.ti->id;
            case ctype::function_k:
                return !wanted.ti || !wanted.ti->is_signature() || (made.ti && made.ti->id == wanted.ti->id);
            case ctype::coroutine_k: return !wanted.yields || (made.yields && made.yields->id == wanted.yields->id);
            default: return true;
        }
    }

    // Why `held` cannot stand behind `signature`; empty when it can
    std::string callable_issue(const callable_view& held, const type_info* signature) {
        const std::vector<parameter>& params = *held.parameters;
        const size_t passed = signature->type_params.size() - 1;
        size_t required = 0;
        while (required < params.size() && !params[required].default_value) { ++required; }
        if (passed < required || passed > params.size()) {
            return held.spelled + " takes " + argument_count_text(required, params.size()) + ", the signature passes " + std::to_string(passed);
        }
        for (size_t i = 0; i < passed; ++i) {
            const parameter& p = params[i];
            const type_info* argument = signature->type_params[i + 1].get();
            const bool by_reference = argument && argument->base_type == script_value_type::jai_reference_type;
            const ctype given = ctype_from_type_info(argument);
            const std::string declared = (p.is_const ? "const " : "") + (p.type ? p.type->canonical_name() : std::string("auto")) + (p.is_reference ? "&" : "");
            if (p.is_reference && !p.is_const) {
                if (!by_reference) {
                    return held.spelled + " takes parameter '" + p.name + "' by reference ('" + declared + "'), the signature passes '" +
                           signature_argument_spelling(argument) + "' by value";
                }
                if (!reference_parameter_binds(p, given)) {
                    return held.spelled + " declares parameter '" + p.name + "' as '" + declared + "', which does not bind the signature's '" +
                           signature_argument_spelling(argument) + "'";
                }
                continue;
            }
            const ctype slot = ctype_from_type_info(p.type.get());
            if (!assignable(given, slot)) {
                return held.spelled + " declares parameter '" + p.name + "' as '" + declared + "', which does not take the signature's '" +
                       signature_argument_spelling(argument) + "'";
            }
        }
        const ctype wanted = ctype_from_type_info(signature->type_params[0].get());
        ctype made = held.result ? ctype_from_type_info(held.result->base_type == script_value_type::jai_reference_type ? held.result->element_type().get() : held.result)
                                 : held.inferred;
        if (held.coroutine) {
            made = ctype::make(ctype::coroutine_k);
            made.yields = dynamic_type(held.result) ? nullptr : held.result;
        }
        if (!result_fits(made, wanted)) {
            return held.spelled + " returns '" + (made.kind == ctype::null_k ? std::string("void") : ctype_name(made)) + "', the signature returns '" +
                   ctype_name(wanted) + "': a result is not converted on the way out; wrap the call in a lambda that converts it";
        }
        return {};
    }

    // Why a callable of static type `src` cannot be stored in the signature slot `dst`; empty when it can, or when the load
    // cannot see what the callable declares (a var, a host function, a bare `function`: the store judges it when it runs)
    std::string signature_issue(const ctype& src, const ctype& dst) {
        const type_info* signature = dst.ti;
        if (!signature || !signature->is_signature() || src.kind != ctype::function_k) { return {}; }
        const expression* origin = src.origin;
        if (origin && origin->get_type() == node_type::lambda_expr) {
            const auto* l = static_cast<const lambda_expr*>(origin);
            callable_view held{&l->parameters, l->return_type.get(), {}, false, "the lambda"};
            if (auto it = lambda_results_.find(l); it != lambda_results_.end() && !l->return_type) { held.inferred = it->second; }
            return callable_issue(held, signature);
        }
        const bool named = origin && (origin->get_type() == node_type::identifier_expr || origin->get_type() == node_type::member_expr);
        if (named && src.fns && !src.fns->empty()) {
            std::string spelled(src.fns->front()->name);
            for (const auto& [qualified, declared] : namespace_fns_) { if (&declared == src.fns) { spelled = qualified; } }   // a namespace's function by its qualified name
            std::string first;
            for (const function_decl* fn : *src.fns) {
                const callable_view held{&fn->parameters, fn->return_type.get(), {}, fn->is_coroutine, "'" + spelled + "'"};
                std::string issue = callable_issue(held, signature);
                if (issue.empty()) { return {}; }
                if (first.empty()) { first = std::move(issue); }
            }
            return src.fns->size() == 1 ? first : "no overload of '" + spelled + "' fits the signature";
        }
        if (src.ti && src.ti->is_signature() && src.ti->id != signature->id) {
            const type_info* from = src.ti;
            const size_t passed = signature->type_params.size() - 1, taken = from->type_params.size() - 1;
            if (passed != taken) {
                return "a '" + from->canonical_name() + "' takes " + argument_count_text(taken, taken) + ", the signature passes " + std::to_string(passed);
            }
            for (size_t i = 1; i <= passed; ++i) {
                const type_info* given = signature->type_params[i].get();
                const type_info* taken_type = from->type_params[i].get();
                const bool given_ref = given && given->base_type == script_value_type::jai_reference_type;
                const bool taken_ref = taken_type && taken_type->base_type == script_value_type::jai_reference_type;
                const bool fits = taken_ref ? given_ref && reference_type_compatible(ctype_from_type_info(given), ctype_from_type_info(taken_type))
                                            : assignable(ctype_from_type_info(given), ctype_from_type_info(taken_type));
                if (!fits) {
                    return "parameter " + std::to_string(i) + " of a '" + from->canonical_name() + "' is '" + signature_argument_spelling(taken_type) +
                           "', which does not take the signature's '" + signature_argument_spelling(given) + "'";
                }
            }
            const ctype made = ctype_from_type_info(from->type_params[0].get());
            const ctype wanted = ctype_from_type_info(signature->type_params[0].get());
            const bool nothing = made.kind == ctype::null_k && !wanted.is_unknown() && wanted.kind != ctype::null_k;
            if (nothing || !result_fits(made, wanted)) {
                return "a '" + from->canonical_name() + "' returns '" + (made.kind == ctype::null_k ? std::string("void") : made.is_unknown() ? std::string("var") : ctype_name(made)) +
                       "', the signature returns '" + ctype_name(wanted) + "': a result is not converted on the way out; wrap the call in a lambda that converts it";
            }
        }
        return {};
    }

    // A call through a signature slot: the count, each argument as a parameter of that type takes it, and the signature's result
    // (for diagnostics: the slot's callable is whatever was stored, so no proof rests on it)
    ctype check_signature_call(const std::string& shown, const type_info* signature, const call_expr* c, const std::vector<ctype>& args) {
        const size_t taken = signature->type_params.size() - 1;
        if (declared_void(signature->type_params[0].get())) { void_hit_ = {shown, {}, nullptr, {}, /*typed_slot=*/true}; }
        const ctype result_type = ctype_from_type_info(signature->type_params[0].get());
        ctype result = result_type;
        result.tentative = !result.is_unknown();
        if (args.size() != taken) {
            diag(diag_level::error, c->location, shown + " expects " + std::to_string(taken) + " argument(s), got " + std::to_string(args.size()) +
                 " (it is a '" + signature->canonical_name() + "')");
            return result;
        }
        for (size_t i = 0; i < taken; ++i) {
            const type_info* declared = signature->type_params[i + 1].get();
            const bool by_reference = declared && declared->base_type == script_value_type::jai_reference_type;
            const ctype slot = ctype_from_type_info(declared);
            const std::string position = "argument " + std::to_string(i + 1) + " of " + shown;
            if (by_reference) {
                const node_type k = c->arguments[i]->get_type();
                if (k == node_type::literal_expr || k == node_type::array_literal_expr || k == node_type::map_literal_expr || k == node_type::lambda_expr) {
                    diag(diag_level::error, c->arguments[i]->location, position + ": the signature takes '" + signature_argument_spelling(declared) + "'; pass a variable");
                } else if (!args[i].is_unknown() && !slot.is_unknown() && !reference_type_compatible(args[i], slot)) {
                    diag(diag_level::error, c->arguments[i]->location, position + ": cannot bind '" + ctype_name(args[i]) + "' to '" + signature_argument_spelling(declared) + "'");
                }
                continue;
            }
            if (!slot.is_unknown() && !assignable(args[i], slot)) {
                diag(diag_level::error, c->arguments[i]->location, position + ": cannot pass '" + ctype_name(args[i]) + "' where the signature takes '" +
                     signature_argument_spelling(declared) + "'" + assignment_detail(args[i], slot));
            }
        }
        return result;
    }

    // `this` stored into a handle slot: it is the handle's object where the receiver is one (a method called through a handle, a
    // constructor new runs), and on a class value the run raises rather than copy it
    static void note_this_into_handle(const ctype& src, const type_info* target) {
        if (src.origin && src.origin->get_type() == node_type::this_expr && handle_type(target)) {
            static_cast<this_expr*>(const_cast<expression*>(src.origin))->into_handle = true;
        }
    }
    // The declared type of the slot an expression names, read from the declarations rather than the walk: a program-scope variable
    // declared below its use or in another unit (the program declares the name once), an element or entry of one
    const type_info* declared_slot_type(const expression* e) {
        if (!e) { return nullptr; }
        if (e->get_type() == node_type::identifier_expr) {
            const auto* id = static_cast<const identifier_expr*>(e);
            if (find_local(id->name)) { return nullptr; }
            if (auto top = top_var_decls_.find(id->name); top != top_var_decls_.end()) {
                return top->second.count == 1 && top->second.decl ? top->second.decl->type.get() : nullptr;
            }
            if (!program_globals_) { return nullptr; }
            const auto held = program_globals_->find(std::string(id->name));
            if (held == program_globals_->end() || held->second.size() != 1 || held->second.begin()->second.size() != 1) { return nullptr; }
            const variable_decl* g = held->second.begin()->second.front().get();
            return g ? g->type.get() : nullptr;
        }
        if (e->get_type() == node_type::binary_expr && static_cast<const binary_expr*>(e)->op.type == token_type::left_bracket) {
            const type_info* container = declared_slot_type(static_cast<const binary_expr*>(e)->left.get());
            if (!container) { return nullptr; }
            if (container->base_type == script_value_type::jai_array_type) { return container->element_type().get(); }
            if (container->base_type == script_value_type::jai_map_type) { return container->value_type().get(); }
        }
        return nullptr;
    }
    // a `this` stored where the walk has no type for the slot, which its declaration may give
    void note_this_into_declared_slot(const expression* value, const expression* slot, bool element) {
        if (!value || value->get_type() != node_type::this_expr) { return; }
        const type_info* t = declared_slot_type(slot);
        if (t && element) { t = t->base_type == script_value_type::jai_array_type ? t->element_type().get() : nullptr; }
        if (handle_type(t)) { static_cast<this_expr*>(const_cast<expression*>(value))->into_handle = true; }
    }
    // a class held by value (never null); a handle, a var and an unknown wrapper are not
    static bool class_value(const ctype& t) {
        return t.kind == ctype::object_k && t.ti && t.ti->base_type == script_value_type::jai_object_type && !t.ti->type_name.empty();
    }
    // A class held by value copies out of the handle a dynamic_cast<T>(e) gives, which is null where e is no T: the warning beside
    // cast<T>(handle)'s names the tested form and the reference form
    // A class value never becomes a handle on its own (C++): make_shared<T>(value) is the copy into a new one
    std::string value_into_handle_issue(const ctype& src, const type_info* slot) {
        if (!handle_type(slot) || !class_value(src)) { return {}; }
        std::string text(detail::value_into_handle_text);
        const std::string cls = ctype_name(src);
        for (size_t at = text.find("{0}"); at != std::string::npos; at = text.find("{0}", at + cls.size())) { text.replace(at, 3, cls); }
        return detail::const_lowercase(text);
    }
    void warn_dynamic_handle_copy(const ctype& src, const ctype& dst) {
        note_this_into_handle(src, dst.ti);
        if (src.origin && !dst.tentative) {
            if (const std::string issue = value_into_handle_issue(src, dst.ti); !issue.empty()) { diag(diag_level::error, src.origin->location, issue); }
        }
        if (!collect_diagnostics_ || !class_value(dst) || !src.origin || src.origin->get_type() != node_type::cast_expr) { return; }
        const auto* c = static_cast<const cast_expr*>(src.origin);
        if (!c->dynamic || c->by_reference || !c->target || !c->target->element_type() || class_relation(src.cls, dst.cls) == 0) { return; }
        size_t related_line = 0, related_column = 0;
        if (var_entry* local = cast_operand_local(c->operand.get())) {
            related_line = local->line;
            related_column = local->column;
        }
        const std::string cls = c->target->element_type()->canonical_name();
        const std::string cast = "dynamic_cast<" + cls + ">(" + cast_operand_spelling(c->operand.get()) + ")";
        diag(diag_level::warning, c->location, cast + " is copied into a value here and is null when the object is no " + cls + " - test it first with if (auto x = " + cast +
             "), or use dynamic_cast<" + cls + "&>(" + cast_operand_spelling(c->operand.get()) + ") when it must be one", related_line, related_column);
    }
    // the one declaration that takes a call: each by-value class parameter an argument copies into
    void warn_dynamic_handle_arguments(const function_decl* fn, const std::vector<ctype>& args) {
        for (size_t i = 0; fn && i < args.size() && i < fn->parameters.size(); ++i) {
            if (!fn->parameters[i].is_reference) { warn_dynamic_handle_copy(args[i], ctype_from_type_info(fn->parameters[i].type.get())); }
        }
    }
    static std::string never_null_fix(const ctype& t) {
        return "a '" + ctype_name(t) + "' is never null; declare 'shared_ptr<" + ctype_name(t) + ">' (or var) for a slot that can be empty";
    }

    bool class_name_constructs(std::string_view cls) {
        const std::string name(cls);
        return find_sclass(cls) || eng_.find_script_class(name) || eng_.has_function(name);
    }

    bool assignable(const ctype& src, const ctype& dst) {
        if (dst.is_unknown() || src.is_unknown()) { return true; }
        if (literal_char_text_refused(src, dst)) { return false; }
        if (src.kind == ctype::class_ref_k && dst.kind != ctype::class_ref_k && dst.kind != ctype::enum_ref_k && class_name_constructs(src.cls)) {
            return assignable(ctype::make(ctype::function_k), dst);   // a class's name as a value is its constructor
        }
        if (src.kind == ctype::enum_ref_k && dst.kind != ctype::class_ref_k && dst.kind != ctype::enum_ref_k) { return assignable(enum_table(src), dst); }
        if (dst.kind == ctype::class_ref_k || dst.kind == ctype::enum_ref_k ||
            src.kind == ctype::class_ref_k || src.kind == ctype::enum_ref_k) {
            return true;   // exotic; stay lenient
        }
        if (enum_value(dst)) { return src.kind == ctype::int_k && src.cls == dst.cls; }   // its own enum's values, no other int
        switch (dst.kind) {
            case ctype::int_k:
                return src.kind == ctype::int_k || src.kind == ctype::bool_k || may_convert_via_method(src, "to_int");
            case ctype::float_k:
                return src.kind == ctype::float_k || src.kind == ctype::int_k || src.kind == ctype::bool_k ||
                       may_convert_via_method(src, "to_float");
            // truthiness, except text and containers: a condition tests those, a bool slot takes an explicit test
            case ctype::bool_k: return src.kind != ctype::string_k && src.kind != ctype::utf8_k && src.kind != ctype::array_k && src.kind != ctype::map_k;
            case ctype::string_k:
            case ctype::utf8_k: return src.kind == ctype::string_k || src.kind == ctype::utf8_k || src.kind == ctype::char_k || may_convert_via_method(src, "to_string");
            case ctype::char_k: return src.kind == ctype::char_k || src.kind == ctype::string_k || may_convert_via_method(src, "to_char");
            case ctype::array_k:
            case ctype::map_k: return container_issue(src, dst).empty();
            case ctype::function_k:
                if (src.kind == ctype::null_k) { return true; }
                return src.kind == ctype::function_k && signature_issue(src, dst).empty();
            case ctype::coroutine_k:
                if (src.kind == ctype::null_k) { return true; }
                return src.kind == ctype::coroutine_k && (!dst.yields || (src.yields && src.yields->id == dst.yields->id));
            case ctype::weak_k: return weak_initializer_compatible(src, dst);
            case ctype::null_k: return src.kind == ctype::null_k;   // void returns
            case ctype::math_k: {
                // the same kind, or the other precision of the same dimensions (the typed-slot table's row)
                if (src.kind != ctype::math_k) { return false; }
                const script_value_type from = math_type_from_name(src.cls), to = math_type_from_name(dst.cls);
                return detail::math::converts(from, to) && !detail::math::narrows(from, to);
            }
            case ctype::object_k: {
                if (src.kind == ctype::null_k) { return !class_value(dst); }
                if (src.kind == ctype::object_k) {
                    int rel = class_relation(src.cls, dst.cls);
                    if (rel != 0) { return true; }
                    return may_construct_from(dst.cls, src);
                }
                if (src.kind == ctype::int_k || src.kind == ctype::float_k || src.kind == ctype::string_k ||
                    src.kind == ctype::bool_k || src.kind == ctype::char_k) {
                    return may_construct_from(dst.cls, src);   // primitive-to-object ctor conversion
                }
                return false;   // array/map/function into an object slot: always a runtime error
            }
            default:
                return true;
        }
    }

    // A bound constructor's or operator's parameter against the one value a conversion hands it (engine.cpp
    // object_param_cost): a parameter of a bound class takes that class or one derived from it, as a script constructor's does
    bool host_constructor_param_takes(const ctype& src, const param_type_info& p) {
        const bool object_param = p.base_type == script_value_type::jai_object_type || p.base_type == script_value_type::jai_shared_ptr_type ||
                                  p.base_type == script_value_type::jai_weak_ptr_type;
        auto bound = object_param && p.cpp_type != std::type_index(typeid(void)) ? eng_.get_class_definition_by_type(p.cpp_type) : nullptr;
        if (!bound) { return host_param_accepts(src, p); }
        if (src.is_unknown() || src.kind == ctype::null_k) { return true; }
        return src.kind == ctype::object_k && (src.cls.empty() || class_relation(src.cls, bound->get_name()) != 0);
    }

    // Target(src) through a C++ class's bound constructors, as the run time's conversion calls them: one of exactly one
    // parameter that may take the source (a variadic or untyped binding may take anything)
    bool host_class_constructs_from(const class_definition& target, const ctype& src) {
        const std::string& name = target.get_name();
        if (src.kind == ctype::object_k && src.cls.empty()) { return true; }
        std::vector<engine::host_overload_signature> signatures;
        if (!eng_.host_function_signatures(name, signatures)) { return eng_.has_function(name); }
        for (const auto& signature : signatures) {
            if (signature.arity == SIZE_MAX) { return true; }
            if (signature.arity != 1) { continue; }
            if (signature.param_types.size() != 1 || host_constructor_param_takes(src, signature.param_types[0])) { return true; }
        }
        return false;
    }

    // Constructor-conversion fallback Target(src): a script class's declared constructors, a C++ class's bound ones; a script
    // class only the engine holds stays lenient.
    bool may_construct_from(std::string_view target_cls, const ctype& src) {
        const sclass* c = find_sclass(target_cls);
        if (!c) {
            auto def = engine_class(target_cls);
            return def && (!def->is_cpp_class() || host_class_constructs_from(*def, src));
        }
        for (const auto* ctor : c->ctors) {
            // A defaulted-param ctor is NOT a converting ctor -
            // only TRUE one-parameter constructors convert (matches the runtime gate)
            if (ctor->parameters.size() != 1) { continue; }
            const parameter& p = ctor->parameters[0];
            if (ctor_param_takes(src, ctype_from_type_info(p.type.get()))) {
                return true;
            }
        }
        return false;
    }

    // The runtime's single-step gate: the converting ctor's declared parameter takes the
    // source DIRECTLY - untyped, its own class or a base of it, the same primitive base or
    // an int into a float. Never another conversion (no chaining, no to_X() on the way in).
    bool ctor_param_takes(const ctype& src, const ctype& param) {
        if (param.is_unknown() || src.is_unknown()) { return true; }
        if (param.kind == ctype::object_k) {
            return src.kind == ctype::object_k && class_relation(src.cls, param.cls) != 0;
        }
        if (src.kind == ctype::object_k) { return false; }
        if (param.kind == src.kind) { return true; }
        return param.kind == ctype::float_k && src.kind == ctype::int_k;
    }

    // Object -> primitive through a script to_int/to_float/to_char (the table's to_X() row):
    // checkable for in-script sources; engine-registered classes stay lenient.
    bool may_convert_via_method(const ctype& src, std::string_view method) {
        if (src.kind != ctype::object_k) { return false; }
        if (!find_sclass(src.cls)) {
            return engine_class(src.cls) != nullptr;
        }
        member_hit hit = lookup_member(src.cls, method, false);
        return !hit.certain || (hit.found && hit.methods != nullptr);
    }

    static std::pair<size_t, size_t> arity_range(const function_decl* fn) {
        size_t min_a = 0;
        for (const auto& p : fn->parameters) {
            if (p.default_value) { break; }
            ++min_a;
        }
        return {min_a, fn->parameters.size()};
    }

    // -------------------------------------------------------------- language builtins

    // Result kind of a math:: intrinsic (detail/math_intrinsics.hpp kernel): int for the
    // integer family, float for the transcendental/interpolation family, operand-typed for
    // abs/min/max/clamp/random (uniform int -> int, uniform float -> float, else unknown).
    static ctype math_intrinsic_result(math_fn fn, const std::vector<ctype>& args) {
        auto all = [&](ctype::kind_t k) {
            for (const auto& a : args) { if (a.kind != k) { return false; } }
            return !args.empty();
        };
        switch (fn) {
            case math_fn::floor_: case math_fn::ceil_: case math_fn::round_: case math_fn::abs_: case math_fn::min_:
            case math_fn::max_: case math_fn::clamp_: case math_fn::lerp_: case math_fn::mix_: case math_fn::unmix_:
            case math_fn::remap_: case math_fn::mix_in_: case math_fn::mix_out_: case math_fn::mix_in_out_:
            case math_fn::mix_out_in_: case math_fn::unmix_in_: case math_fn::unmix_out_: case math_fn::unmix_in_out_:
            case math_fn::unmix_out_in_: {
                // the component-wise family (math_intrinsics.hpp eval_vec_math): a vector argument makes a vector result
                // (a vec of its dimensions; abs/min/max/clamp keep ivec when every argument is integral), and an argument
                // of unknown type leaves the result unknown
                const ctype* vec = nullptr;
                bool unknown = false, integral = true;
                for (const auto& a : args) {
                    if (a.kind == ctype::math_k) {
                        if (!vec) { vec = &a; }
                        integral = integral && detail::math::integral(math_type_from_name(a.cls));
                    } else if (!is_numeric(a.kind)) {
                        unknown = true;
                    } else if (a.kind != ctype::int_k) {
                        integral = false;
                    }
                }
                if (vec) {
                    const script_value_type vk = math_type_from_name(vec->cls);
                    if (detail::math::is_color(vk)) { return unknown ? ctype{} : math_ctype(vk); }
                    const size_t dims = detail::math::dims(vk);
                    const bool exact = integral && (fn == math_fn::abs_ || fn == math_fn::min_ || fn == math_fn::max_ || fn == math_fn::clamp_);
                    return unknown ? ctype{} : math_ctype(detail::math::family_kind(exact, dims));
                }
                if (unknown) { return {}; }
                break;
            }
            case math_fn::dot_: case math_fn::cross_: case math_fn::length_: case math_fn::length_squared_:
            case math_fn::normalize_: case math_fn::reflect_: case math_fn::project_: case math_fn::angle_between_:
            case math_fn::perpendicular_: case math_fn::rotate_: case math_fn::distance: case math_fn::distance_squared:
            case math_fn::angle_: case math_fn::rotate_around_: {
                // the geometry library (math_intrinsics.hpp eval_geometry): the vector arguments' kinds decide; the scalar
                // distance form (four numbers) stays float
                if (args.empty() || args[0].kind != ctype::math_k) {
                    if (fn == math_fn::distance || fn == math_fn::distance_squared) { break; }
                    return {};
                }
                const script_value_type vk = math_type_from_name(args[0].cls);
                if (detail::math::is_color(vk)) { return {}; }
                const size_t dims = detail::math::dims(vk);
                bool exact = detail::math::integral(vk);
                const bool two = fn != math_fn::length_ && fn != math_fn::length_squared_ && fn != math_fn::normalize_ &&
                                 fn != math_fn::perpendicular_ && fn != math_fn::angle_ && !(fn == math_fn::rotate_ && args.size() == 2);
                if (two) {
                    if (args.size() < 2 || args[1].kind != ctype::math_k) { return {}; }
                    exact = exact && detail::math::integral(math_type_from_name(args[1].cls));
                }
                switch (fn) {
                    case math_fn::dot_: case math_fn::length_squared_: case math_fn::distance_squared:
                        return ctype::make(exact ? ctype::int_k : ctype::float_k);
                    case math_fn::cross_:
                        if (dims == 2) { return ctype::make(exact ? ctype::int_k : ctype::float_k); }
                        return math_ctype(exact ? script_value_type::jai_ivec3_type : script_value_type::jai_vec3_type);
                    case math_fn::perpendicular_:
                        return math_ctype(vk);
                    case math_fn::length_: case math_fn::angle_between_: case math_fn::distance: case math_fn::angle_:
                        return ctype::make(ctype::float_k);
                    default:
                        return math_ctype(detail::math::family_kind(false, dims));
                }
            }
            default:
                break;
        }
        switch (fn) {
            case math_fn::itrunc: case math_fn::ifloor:
            case math_fn::wrap_add_: case math_fn::wrap_sub_: case math_fn::wrap_mul_:
            case math_fn::i32_:
            case math_fn::i32_add_: case math_fn::i32_sub_: case math_fn::i32_mul_: case math_fn::i32_div_: case math_fn::i32_neg_:
                return ctype::make(ctype::int_k);
            case math_fn::cstore_: case math_fn::vcomp_:
                return {};   // the call site reads the kind from its literal code (math_step_result)
            case math_fn::abs_: case math_fn::min_: case math_fn::max_: case math_fn::clamp_:
                if (all(ctype::int_k)) { return ctype::make(ctype::int_k); }
                if (all(ctype::float_k)) { return ctype::make(ctype::float_k); }
                return {};
            case math_fn::vec2_: case math_fn::from_angle_: return math_ctype(script_value_type::jai_vec2_type);
            case math_fn::vec3_: return math_ctype(script_value_type::jai_vec3_type);
            case math_fn::ivec2_: return math_ctype(script_value_type::jai_ivec2_type);
            case math_fn::sign_: return ctype::make(ctype::int_k);
            case math_fn::is_nan_: case math_fn::is_inf_: return ctype::make(ctype::bool_k);
            case math_fn::ivec3_: return math_ctype(script_value_type::jai_ivec3_type);
            case math_fn::vec4_: return math_ctype(script_value_type::jai_vec4_type);
            case math_fn::color_: case math_fn::mix_rgb_: case math_fn::mix_hsv_: return math_ctype(script_value_type::jai_color_type);
            default:
                return ctype::make(ctype::float_k);
        }
    }

    // `a::b::c` chains resolve through namespaces registered under their FULL path ("a::b"),
    // so a static receiver is a namespace path when any prefix of it names one (this unit's
    // declarations or the engine's).
    bool namespace_path_receiver(const member_expr* m) {
        std::vector<std::string_view> segments;
        const expression* e = m->object.get();
        while (e && e->get_type() == node_type::member_expr) {
            auto* inner = static_cast<const member_expr*>(e);
            if (!inner->is_static) { return false; }
            segments.push_back(inner->member);
            e = inner->object.get();
        }
        if (!e || e->get_type() != node_type::identifier_expr) { return false; }
        std::string path(static_cast<const identifier_expr*>(e)->name);
        if (namespace_known(path)) { return true; }
        for (size_t i = segments.size(); i-- > 0;) {
            path += "::";
            path += segments[i];
            if (namespace_known(path)) { return true; }
        }
        return false;
    }

    void note_host_nodiscard(const std::string& callable) {
        if (eng_.host_nodiscard(callable)) { nodiscard_hit_ = {callable, {}}; }
        if (type_argument_call_) { type_argument_host_callee_ = callable; }
    }
    // a host function every registration of which returns void in C++ (engine::host_function_result)
    void note_host_void(const std::string& callable) {
        script_value_type k;
        if (eng_.host_function_result(callable, k) && k == script_value_type::jai_invalid_type) { void_hit_ = {"'" + callable + "'"}; }
    }
    // a script callee of a call with type arguments: the call's one error, in place of the overload verdict
    bool script_callee_takes_type_arguments(std::string_view fname, std::string_view kind) {
        if (!type_argument_call_) { return false; }
        type_argument_script_callee_ = std::string(fname);
        type_argument_script_kind_ = kind;
        return true;
    }
    JAI_NOINLINE static std::string no_type_parameters_text(const std::string& callee, std::string_view kind) {
        if (kind == "lambda") { return "'" + callee + "' takes no type arguments: it is a lambda, and a lambda takes no type parameters"; }
        return "'" + callee + "' takes no type arguments: it is not a generic " + std::string(kind);
    }
    // A host registration's typed result (core/permission.hpp jai::returns<jai::type_arg> and its siblings): the first type
    // argument T, array<T> or shared_ptr<T>
    ctype type_argument_result(const call_expr* c, uint8_t shape) {
        const type_info* t = static_cast<const cast_expr*>(c->arguments[0].get())->target.get();
        switch (shape) {
            case 1: return ctype_from_type_info(t);
            case 2: return ctype_from_type_info(eng_.array_type_of(t));
            case 3: return ctype_from_type_info(eng_.shared_type_of(t));
            default: return {};
        }
    }

    // A call whose result is used, of a callee declared to return nothing: the builtin methods' text where the method has a copying twin
    // A call through a name something may rebind: a bare name (a function, a host function, a lambda variable) or a namespace path
    bool callee_rebindable(const call_expr* c) const {
        if (!c->callee) { return false; }
        if (c->callee->get_type() == node_type::identifier_expr) {
            const auto* id = static_cast<const identifier_expr*>(c->callee.get());
            if (class_ctx_ && void_hit_.shown != "'" + std::string(id->name) + "'" && void_hit_.shown.find("::") == std::string::npos) { return false; }   // a method by its bare name
            return rebindable(id->name) || rebindable_paths_.count(void_hit_.shown.size() > 2 ? void_hit_.shown.substr(1, void_hit_.shown.size() - 2) : std::string());
        }
        if (c->callee->get_type() == node_type::member_expr && static_cast<const member_expr*>(c->callee.get())->is_static) {
            const std::string path = qualified_spelling(static_cast<const member_expr*>(c->callee.get()));
            return !path.empty() && rebindable_paths_.count(path);
        }
        return false;
    }
    static std::string void_result_text(const void_callee& hit) {
        if (hit.member) { return detail::in_place_result_text(hit.member->member, detail::method_value_receiver(hit.member), hit.twin); }
        return "the result of " + hit.shown + " is used, but " + hit.shown + " returns nothing: " + (hit.reason.empty() ? std::string("call it as a statement") : hit.reason);
    }
    JAI_NOINLINE void report_void_result(const call_expr* c, const void_callee& hit) { diag(diag_level::error, c->location, void_result_text(hit)); }

    // An expression statement drops its value: a call to a [[nodiscard]] callee there (or in either arm of a ternary there) fails the load
    void check_discarded(const expression* e) {
        if (!e || nodiscard_calls_.empty()) { return; }
        if (e->get_type() == node_type::ternary_expr) {
            check_discarded(static_cast<const ternary_expr*>(e)->then_expression.get());
            check_discarded(static_cast<const ternary_expr*>(e)->else_expression.get());
            return;
        }
        if (e->get_type() != node_type::call_expr) { return; }
        auto* c = static_cast<const call_expr*>(e);
        auto it = nodiscard_calls_.find(c);
        if (it == nodiscard_calls_.end()) { return; }
        const nodiscard_callee& hit = it->second;
        const source_location& at = c->callee ? c->callee->location : c->location;
        if (hit.twin.empty()) {
            std::string spelled = hit.name;
            if (c->callee && c->callee->get_type() == node_type::member_expr) {
                auto* m = static_cast<const member_expr*>(c->callee.get());
                if (!m->is_static && m->object && m->object->get_type() == node_type::identifier_expr) {
                    spelled = std::string(static_cast<const identifier_expr*>(m->object.get())->name) + "." + std::string(m->member);
                }
            }
            diag(diag_level::error, at, "the result of '" + hit.name + "' is discarded, but '" + hit.name +
                 "' is [[nodiscard]]: use it, or keep it in the placeholder (auto _ = " + spelled + "(...);)");
            return;
        }
        std::string receiver = "x";
        if (!c->arguments.empty() && c->arguments[0]->get_type() == node_type::identifier_expr) {
            receiver = std::string(static_cast<const identifier_expr*>(c->arguments[0].get())->name);
        }
        diag(diag_level::error, at, "the result of '" + hit.name + "' is discarded: it returns a new value and leaves its argument "
             "unchanged, so the statement does nothing; keep the result, or call the in-place method (" + receiver + "." + hit.twin + "(...))");
    }

    // The declarations of the namespace function `path::member` a call or a read of it may reach, `ns::f` and a bare f in the
    // namespace's code alike. typed: the ones whose declared result types the call, the unit's own (a later one of a signature
    // replaces the earlier) and another unit's of the program that nothing else of its signature can stand in for. untyped: the
    // rest another unit of the program makes or the engine holds from a run, which only widen what the call may pass.
    struct namespace_overloads {
        const std::vector<const function_decl*>* own = nullptr;   // every declaration the unit makes, in order
        std::vector<const function_decl*> typed, untyped;
        size_t own_typed = 0;   // typed leads with this many of the unit's own
        bool empty() const { return typed.empty() && untyped.empty(); }
        bool only_own() const { return untyped.empty() && own_typed == typed.size(); }
        bool all_nodiscard() const {
            const auto marked = [](const function_decl* fn) { return fn->is_nodiscard; };
            return !empty() && (!own || std::all_of(own->begin(), own->end(), marked)) && std::all_of(typed.begin(), typed.end(), marked) &&
                   std::all_of(untyped.begin(), untyped.end(), marked);
        }
    };
    namespace_overloads namespace_function_overloads(const std::string& path, std::string_view member) {
        namespace_overloads result;
        const std::string qualified = path + "::" + std::string(member);
        const auto same = [](const function_decl* a, const function_decl* b) {
            return a == b || (a->body && a->body == b->body) || same_signature(a->parameters, b->parameters) || differs_only_by_reference(a->parameters, b->parameters);
        };
        if (auto fit = namespace_fns_.find(qualified); fit != namespace_fns_.end() && !fit->second.empty()) {
            result.own = &fit->second;
            for (size_t i = 0; i < fit->second.size(); ++i) {
                bool replaced = false;
                for (size_t k = i + 1; k < fit->second.size() && !replaced; ++k) { replaced = same_signature(fit->second[i]->parameters, fit->second[k]->parameters); }
                if (!replaced) { result.typed.push_back(fit->second[i]); }
            }
        } else if (has_dynamic_includes_ || (!program_functions_ && extra_global(qualified))) {
            return result;   // a file the load cannot read, or a program whose declarations it was not given, may declare it
        }
        const size_t own_count = result.typed.size();
        std::vector<bool> stands(own_count, true);   // typed[i] is the one declaration of its signature the program makes
        std::vector<std::string> units(own_count);   // the unit that makes typed[i], for another unit's
        const auto declared_again = [&](const function_decl* fn) {
            for (size_t i = 0; i < result.typed.size(); ++i) {
                if (!same(result.typed[i], fn)) { continue; }
                // the table's copy of this very declaration, or of the file's earlier text, is no second declaration
                stands[i] = stands[i] && (result.typed[i] == fn || (fn->body && result.typed[i]->body == fn->body) ||
                                          (i < own_count && fn->location.filename == result.typed[i]->location.filename));
                return true;
            }
            return false;
        };
        if (program_functions_) {
            eng_.note(qualified);
            if (auto pit = program_functions_->find(qualified); pit != program_functions_->end()) {
                for (const auto& [unit, decls] : pit->second) {
                    for (const auto& d : decls) {
                        if (!d || declared_again(d.get())) { continue; }
                        result.typed.push_back(d.get());
                        stands.push_back(collect_diagnostics_ && !has_dynamic_includes_);
                        units.push_back(std::filesystem::path(unit).lexically_normal().string());
                    }
                }
            }
        }
        if (script_namespace_data* held = eng_.find_namespace(path)) {
            eng_.note(qualified);
            if (auto hit = held->functions.find(eng_.symbolize(member)); hit != held->functions.end()) {
                for (const auto& d : hit->second) {
                    if (!d) { continue; }
                    bool known = false;
                    for (size_t i = 0; i < result.typed.size() && !known; ++i) {
                        if (!same(result.typed[i], d.get())) { continue; }
                        known = true;
                        // the unit's own declaration replaces it when the unit runs; another unit's stands for it when it is that
                        // declaration or the unit's earlier text, which its reload replaces
                        if (i >= own_count && result.typed[i] != d.get() && eng_.unit_label(d->unit.id) != units[i]) { stands[i] = false; }
                    }
                    if (!known && std::none_of(result.untyped.begin(), result.untyped.end(), [&](const function_decl* fn) { return same(fn, d.get()); })) {
                        result.untyped.push_back(d.get());
                    }
                }
            }
        }
        std::vector<const function_decl*> typed;
        for (size_t i = 0; i < result.typed.size(); ++i) {
            (stands[i] ? typed : result.untyped).push_back(result.typed[i]);
            result.own_typed += stands[i] && i < own_count;
        }
        result.typed = std::move(typed);
        return result;
    }

    // A call of the namespace function `path::member`: an error only when no declaration in sight takes it, typed by the one
    // that does when its result is certain. False when none is in sight (a host entry, a variable, a file the load cannot read).
    bool check_namespace_function_call(const std::string& path, std::string_view member, const std::vector<ctype>& args, const source_location& loc,
                                       narrow_callee& seen, ctype& result) {
        const namespace_overloads fns = namespace_function_overloads(path, member);
        if (fns.empty()) { return false; }
        const std::string qualified = path + "::" + std::string(member);
        const bool class_too = find_sclass(path) || known_class(path);   // a class of the namespace's name answers with its statics too
        seen.resolved = !class_too;
        seen.declarations = fns.typed;
        seen.declarations.insert(seen.declarations.end(), fns.untyped.begin(), fns.untyped.end());
        result = {};
        if (!class_too) {
            result = fns.only_own() ? check_script_overloads(qualified, fns.typed, args, loc).result
                                    : check_program_overloads(qualified, fns.typed, fns.untyped, args, loc, /*foreign_first=*/fns.own_typed == 0);
            result = read_through_declared_slot(result);
        }
        if (fns.all_nodiscard()) { nodiscard_hit_ = {qualified, {}}; }
        return true;
    }

    // The class `qualified` names, by its own spelling (the program's or the engine's); empty when it names none
    std::string_view namespace_class_named(const std::string& qualified) {
        if (const sclass* sc = find_sclass(qualified)) { return sc->decl->name; }
        const auto held = engine_class(qualified);
        return held ? std::string_view(held->get_name()) : std::string_view{};
    }
    // `ns::C::member`: the receiver is a namespace's class, read as any class's statics are (a namespace of C's path keeps its own lookup)
    bool namespace_class_receiver(const member_expr* m) {
        const std::string path = member_path(m);
        return !path.empty() && path.find("::") != std::string::npos && !namespace_known(path) && !namespace_class_named(path).empty();
    }

    // A value a host registered in its namespace (math::pi): script cannot assign it, so its kind is the one it was registered with
    ctype host_namespace_value_type(const std::string& path, std::string_view member) {
        const std::string qualified = path + "::" + std::string(member);
        if (namespaces_.count(std::string_view(path)) || (program_globals_ && program_globals_->count(qualified))) { return {}; }
        script_namespace_data* space = eng_.find_namespace(path);
        if (!space) { return {}; }
        eng_.note(qualified);
        const auto found = space->variables.find(eng_.symbolize(member));
        if (found == space->variables.end()) { return {}; }
        const script_value& value = found->second.deref();
        ctype t;
        if (value.is_int()) { t = ctype::make(ctype::int_k); }
        else if (value.is_float()) { t = ctype::make(ctype::float_k); }
        else if (value.is_bool()) { t = ctype::make(ctype::bool_k); }
        else if (value.is_string()) { t = ctype::make(ctype::string_k); }
        else if (value.is_char()) { t = ctype::make(ctype::char_k); }
        else { return {}; }
        t.tentative = true;
        t.declared_result = true;
        return t;
    }

    // What a read of the namespace member `path::member` gives: a function (its declarations, when the unit makes every one a
    // call could reach), else the declared type of a variable
    ctype namespace_member_read(const std::string& path, std::string_view member) {
        const namespace_overloads fns = namespace_function_overloads(path, member);
        if (fns.empty()) {
            if (const std::string_view cls = namespace_class_named(path + "::" + std::string(member)); !cls.empty()) {
                return read_through_declared_slot(ctype{ctype::class_ref_k, cls});
            }
            const ctype declared = namespace_variable_type(path, member);
            return declared.is_unknown() ? host_namespace_value_type(path, member) : declared;
        }
        ctype named = ctype::make(ctype::function_k);
        if (fns.only_own() && fns.typed.size() == fns.own->size() && !find_sclass(path) && !known_class(path)) { named.fns = fns.own; }
        return named;
    }

    // A host namespace entry with a declared signature (script_namespace_data::signatures): arity and argument kinds are
    // certainties the entry's own run-time check would raise; a unit that declares the namespace, or a script function of
    // the name, keeps the call lenient
    ctype check_namespace_entry(const member_expr* m, const std::vector<ctype>& args, const source_location& loc, narrow_callee& seen, const call_expr* c) {
        const std::string path = member_path(m);
        return path.empty() ? ctype{} : check_namespace_call(path, m->member, args, loc, seen, c);
    }
    ctype check_namespace_call(const std::string& path, std::string_view member, const std::vector<ctype>& args, const source_location& loc, narrow_callee& seen, const call_expr* c) {
        if (c && c->arguments.size() >= 2 && (path == "array" || path == "map") && library_namespace(path)) {
            if (const auto wants = callback_result_wanted(path == "array" ? 0 : 1, member)) { callback_result_read(path + "::" + std::string(member), *wants, c->arguments[1].get()); }
        }
        if (ctype result; check_namespace_function_call(path, member, args, loc, seen, result)) { return result; }
        const std::string qualified = path + "::" + std::string(member);
        if (const std::string_view cls = namespace_class_named(qualified); !cls.empty()) {   // a construction, typed as the bare spelling is
            if (script_callee_takes_type_arguments(cls, "class")) { return {}; }
            if (const sclass* sc = find_sclass(cls)) { check_ctor_call(cls, *sc, args, loc); }
            return read_through_declared_slot(object_of(cls));
        }
        if (namespace_variable_called(path, qualified, member, c && c->callee ? c->callee->location : loc)) { return {}; }
        if (namespaces_.count(std::string_view(path))) { return {}; }
        script_namespace_data* space = eng_.find_namespace(path);
        if (!space) { return {}; }
        eng_.note(qualified);
        const uint64_t member_id = eng_.symbolize(member);
        if (space->functions.count(member_id)) { return {}; }
        if (auto vit = space->variables.find(member_id); vit != space->variables.end() && vit->second.is_function()) {
            const auto* w = vit->second.as_function().target<host_writeback_function>();
            seen.resolved = true;
            seen.writeback = w ? w->mask : 0;
        }
        auto sit = space->signatures.find(member_id);
        if (sit == space->signatures.end()) {   // a qualified host registration is held to its bound signatures, as a global host function is
            if (!eng_.has_function(qualified)) { return {}; }
            check_host_call(qualified, args, loc, c);
            note_host_void(qualified);
            return host_result_type(qualified);
        }
        const host_entry_signature& sig = sit->second;
        if (sig.nodiscard) { nodiscard_hit_ = {qualified, sig.in_place_twin ? std::string(member) : std::string()}; }
        if (type_argument_call_) { type_argument_host_callee_ = qualified; }
        if (args.size() < sig.min_args || args.size() > sig.max_args) {
            const std::string expected = sig.min_args == sig.max_args ? std::to_string(sig.min_args)
                : std::to_string(sig.min_args) + " to " + std::to_string(sig.max_args);
            diag(diag_level::error, loc, "'" + qualified + "' expects " + expected + " argument(s), got " + std::to_string(args.size()));
            return {};
        }
        for (size_t i = 0; i < args.size() && i < sig.params.size(); ++i) {
            if (!namespace_arg_accepts(args[i], sig.params[i])) {
                diag(diag_level::error, loc, "argument " + std::to_string(i + 1) + " of '" + qualified + "': cannot pass '" +
                     ctype_name(args[i]) + "' where " + namespace_param_spelling(sig.params[i]) + " is expected");
                return {};
            }
        }
        if (path == "array" && !args.empty() && args[0].kind == ctype::array_k) {
            check_array_sizing_arguments(member, args[0], args.data() + 1, args.size() - 1, loc);
        }
        ctype t;
        if (sig.result_follows_first && !args.empty() && (args[0].kind == ctype::string_k || args[0].kind == ctype::utf8_k || args[0].kind == ctype::array_k)) { t = args[0]; }
        else if (sig.result == script_value_type::jai_array_type) { t = ctype::make(ctype::array_k); }
        else if (sig.result == script_value_type::jai_string_type) { t = ctype::make(ctype::string_k); }
        else if (sig.result == script_value_type::jai_bool_type) { t = ctype::make(ctype::bool_k); }
        else if (sig.result == script_value_type::jai_int_type) { t = ctype::make(ctype::int_k); }
        else if (sig.result == script_value_type::jai_float_type) { t = ctype::make(ctype::float_k); }
        else if (sig.result == script_value_type::jai_object_type && !sig.result_class.empty()) {
            const type_info* ti = eng_.get_type_info_object(sig.result_class);
            t = ctype::object(ti->type_name);
            t.ti = ti;
        }
        else { return {}; }
        t.tentative = path != "array";
        t.declared_result = t.tentative;
        return t;
    }
    static bool namespace_arg_accepts(const ctype& arg, script_value_type p) {
        if (arg.is_unknown() || arg.kind == ctype::null_k || p == script_value_type::jai_null_type) { return true; }
        switch (p) {
            case script_value_type::jai_string_type: return arg.kind == ctype::string_k || arg.kind == ctype::utf8_k;
            case script_value_type::jai_int_type: return arg.kind == ctype::int_k;
            case script_value_type::jai_float_type: return arg.kind == ctype::int_k || arg.kind == ctype::float_k;
            case script_value_type::jai_bool_type: return arg.kind == ctype::bool_k;
            case script_value_type::jai_array_type: return arg.kind == ctype::array_k;
            case script_value_type::jai_map_type: return arg.kind == ctype::map_k;
            default: return true;
        }
    }
    static std::string namespace_param_spelling(script_value_type p) {
        switch (p) {
            case script_value_type::jai_string_type: return "a string";
            case script_value_type::jai_int_type: return "an int";
            case script_value_type::jai_float_type: return "a number";
            case script_value_type::jai_bool_type: return "a bool";
            case script_value_type::jai_array_type: return "an array";
            case script_value_type::jai_map_type: return "a map";
            default: return "a value";
        }
    }

    bool namespace_known(const std::string& path) {
        const std::string opened = path + "::";
        return namespaces_.count(std::string_view(path)) != 0 || namespace_members_.count(opened) != 0 ||
               eng_.find_namespace(path) != nullptr || extra_global(opened);
    }

    // C++ [namespace.qual]: a qualified name names what a declaration of its namespace declares (a unit of the program, an earlier
    // run, the host's registrations, the math:: intrinsics), a namespace nested in it, or a class or enum of that name; a
    // namespace that is also a class answers with the class's statics too
    bool namespace_declares(const std::string& space, std::string_view member) {
        const std::string qualified = space + "::" + std::string(member);
        if (namespace_members_.count(qualified) || extra_global(qualified) || namespace_known(qualified) || known_type_name(qualified)) { return true; }
        if (eng_.has_variable(qualified) || eng_.has_function(qualified) || known_class(space) || find_sclass(space)) { return true; }
        if (space == "math" && math_intrinsic_from_name(member) != math_fn::none) { return true; }
        eng_.note(qualified);
        const script_namespace_data* ns = eng_.find_namespace(space);
        if (!ns) { return false; }
        const uint64_t id = eng_.symbolize(member);
        return ns->functions.count(id) || ns->variables.count(id) || ns->classes.count(id) || ns->signatures.count(id) ||
               (ns->scope && ns->scope->get_own_value_ptr(id) != nullptr);
    }

    // what a bare name in the namespace's code finds there: a member some declaration of the namespace declares (a unit of the
    // program, an earlier run, the host's registrations), never a class's statics or a nested namespace
    bool namespace_member_declared(const std::string& space, std::string_view member) {
        const std::string qualified = space + "::" + std::string(member);
        if (known_class(qualified) || find_sclass(qualified) || eng_.is_type_name(qualified)) { return false; }   // the parser reads a namespace's bare class name qualified
        if (namespace_members_.count(qualified) || eng_.has_variable(qualified) || eng_.has_function(qualified)) { return true; }
        if (extra_global(qualified) && !extra_global(qualified + "::")) { return true; }   // another unit's member; a nested namespace is named by its path
        eng_.note(qualified);
        const script_namespace_data* ns = eng_.find_namespace(space);
        if (!ns) { return false; }
        const uint64_t id = eng_.symbolize(member);
        return ns->functions.count(id) || ns->variables.count(id) || (ns->scope && ns->scope->get_own_value_ptr(id) != nullptr);
    }

    // a member the host registered in the namespace (script_namespace_data::variables, a qualified add_global or add_function),
    // which no script declaration of the namespace declares
    bool namespace_host_entry(const std::string& space, std::string_view member) {
        const std::string qualified = space + "::" + std::string(member);
        if (namespace_members_.count(qualified)) { return false; }
        if (const script_namespace_data* ns = eng_.find_namespace(space)) {
            const uint64_t id = eng_.symbolize(member);
            if (ns->scope && ns->scope->get_own_value_ptr(id) != nullptr) { return false; }
            if (ns->variables.count(id)) { return true; }
        }
        return eng_.has_variable(qualified) || eng_.has_function(qualified);
    }

    // C++ [basic.lookup.unqual]: a bare name in a namespace's code searches that namespace, then each enclosing one ("a::b", then
    // "a"), then the program scope; the innermost namespace longer than `nearer_than` characters that declares it, else empty
    std::string enclosing_namespace_member(std::string_view name, size_t nearer_than = 0) {
        if (namespace_walk_names_.empty()) { return {}; }
        std::string space(namespace_walk_names_.back());
        while (space.size() > nearer_than) {
            if (namespace_member_declared(space, name)) { return space; }
            const size_t cut = space.rfind("::");
            if (cut == std::string::npos) { break; }
            space.resize(cut);
        }
        return {};
    }

    // A name the walk holds at program scope or in an enclosing namespace's body, which a nearer namespace declares again
    bool namespace_hides_program_name(std::string_view name, size_t scope_index) {
        if (namespace_walk_names_.empty()) { return false; }
        size_t nearer_than = 0;
        if (scope_index != 0) {
            auto at = std::find(namespace_scopes_.begin(), namespace_scopes_.end(), scope_index);
            if (at == namespace_scopes_.end()) { return false; }   // a function's, a block's or a lambda's own name
            nearer_than = namespace_walk_names_[static_cast<size_t>(at - namespace_scopes_.begin())].size();
        }
        return !enclosing_namespace_member(name, nearer_than).empty();
    }

    // the longest prefix of a qualified name that names a namespace must declare the segment after it
    void check_namespace_member(const member_expr* m) {
        std::vector<std::string_view> parts{m->member};
        const expression* e = m->object.get();
        while (e && e->get_type() == node_type::member_expr && static_cast<const member_expr*>(e)->is_static) {
            parts.push_back(static_cast<const member_expr*>(e)->member);
            e = static_cast<const member_expr*>(e)->object.get();
        }
        if (!e || e->get_type() != node_type::identifier_expr) { return; }
        parts.push_back(static_cast<const identifier_expr*>(e)->name);
        std::reverse(parts.begin(), parts.end());
        std::string path, space;
        size_t next = 0;
        for (size_t k = 0; k + 1 < parts.size(); ++k) {
            if (k) { path += "::"; }
            path += parts[k];
            if (namespace_known(path)) { space = path; next = k + 1; }
        }
        // a namespace's generic class by its bare name (shop::Box, shop::Box::made): no class answers to it
        for (std::string named = path + "::" + std::string(parts.back()); named.size() > parts.front().size(); named.resize(named.rfind("::"))) {
            if (refuse_bare_generic_class(named, m->location)) { return; }
        }
        if (space.empty() || namespace_declares(space, parts[next])) { return; }
        const std::string member = space + "::" + std::string(parts[next]);
        if (instance_requests_.count(member)) { return; }   // an instance that could not be made: its request says why
        if (const std::string generic = generic_value_spelled(generics_named(member), path + "::" + std::string(parts.back()), ""); !generic.empty()) {
            diag(diag_level::error, m->location, generic);
            return;
        }
        diag(has_dynamic_includes_ ? diag_level::warning : diag_level::error, m->location,
             "no member '" + std::string(parts[next]) + "' in namespace '" + space + "'");
    }

    // A method call on a declared container or string: the name must be one the builtin registry dispatches (the runtime's
    // family-B text otherwise; a map's own text names the key spelling, m["f"](x))
    void check_builtin_member(const ctype& recv, const member_expr* m) {
        int kind = -1;
        switch (recv.kind) {
            case ctype::array_k: kind = 0; break;
            case ctype::map_k: kind = 1; break;
            case ctype::string_k: kind = 2; break;
            case ctype::utf8_k: kind = 3; break;
            default: return;
        }
        const auto& ids = eng_.builtin_method_ids(kind);
        if (ids.count(eng_.symbolize(m->member))) { return; }
        if (kind == 1) { map_dot_key(m); return; }
        if (const std::string_view hint = detail::foreign_member_text(ctype_name(recv), m->member); !hint.empty()) {
            diag(diag_level::error, m->location, "no member '" + std::string(m->member) + "' on '" + ctype_name(recv) + "': " + std::string(hint));
            return;
        }
        std::string best;
        size_t best_distance = 3;
        for (const uint64_t id : ids) {
            const std::string_view name = eng_.get_symbolizer()->get_string(id);
            const size_t d = edit_distance(m->member, name);
            if (d < best_distance || (d == best_distance && !best.empty() && name < best)) { best_distance = d; best = std::string(name); }
        }
        diag(diag_level::error, m->location, "no member '" + std::string(m->member) + "' on '" + ctype_name(recv) + "'" +
             (best.empty() ? std::string() : " (did you mean '" + best + "'?)"));
    }
    // A coroutine handle answers next(), done(), advance() and result() only (the runtime's texts); resume is the retired spelling
    // of next
    void coroutine_member_missing(const ctype& a_receiver, const member_expr* a_member) {
        std::string best;
        if (a_member->member == "resume") {
            best = "next";
        } else {
            size_t bestDistance = 3;
            for (const std::string_view name : {std::string_view{"advance"}, std::string_view{"done"}, std::string_view{"next"}, std::string_view{"result"}}) {
                const size_t distance = edit_distance(a_member->member, name);
                if (distance < bestDistance) { bestDistance = distance; best = std::string(name); }
            }
        }
        diag(diag_level::error, a_member->location, "no member '" + std::string(a_member->member) + "' on '" + ctype_name(a_receiver) + "'" +
            (best.empty() ? std::string() : " (did you mean '" + best + "'?)"));
    }
    ctype coroutine_method_call(const ctype& a_receiver, const member_expr* a_member, const call_expr* a_call, const std::vector<ctype>& a_args) {
        if (a_receiver.tentative) { return {}; }
        if (a_member->member == "done") {
            if (!a_args.empty()) { diag(diag_level::error, a_call->location, "done() takes no arguments"); }
            return ctype::make(ctype::bool_k);
        }
        if (a_member->member == "next") {
            if (a_args.size() > 1) {
                diag(diag_level::error, a_call->location, "next() takes 0 or 1 arguments ([count])");
            } else if (a_args.size() == 1 && !a_args[0].is_unknown() && !a_args[0].tentative && a_args[0].kind != ctype::int_k) {
                diag(diag_level::error, a_call->arguments[0]->location, "next() count must be an integer");
            }
            // the declared type, for diagnostics: the step that ends a body which returns nothing hands out null
            ctype stepped = a_receiver.yields ? ctype_from_type_info(a_receiver.yields) : ctype{};
            stepped.tentative = !stepped.is_unknown();
            stepped.nullable_result = true;
            return stepped;
        }
        if (a_member->member == "result") {
            if (!a_args.empty()) { diag(diag_level::error, a_call->location, "result() takes no arguments"); }
            // the declared return type, tentative: a body that falls off its end or fails gives null
            ctype result = a_receiver.yields ? ctype_from_type_info(a_receiver.yields) : ctype{};
            result.tentative = !result.is_unknown();
            return result;
        }
        if (a_member->member == "advance") {
            if (a_args.size() != 1) {
                diag(diag_level::error, a_call->location, "advance() takes 1 argument (the variable that receives the yielded value)");
                return ctype::make(ctype::bool_k);
            }
            const expression* out = a_call->arguments[0].get();
            const node_type k = out->get_type();
            if (k == node_type::literal_expr || k == node_type::array_literal_expr || k == node_type::map_literal_expr) {
                diag(diag_level::error, out->location, "advance() writes the yielded value through its argument; pass a variable");
                return ctype::make(ctype::bool_k);
            }
            if (const const_hit h = const_path(out); h.verdict == const_raises) {
                const std::string text = detail::const_argument_text(out, h.root, "v", "advance", 0);
                stamp_const(const_cast<call_expr*>(a_call)->const_check, const_raises, [&] { return text; });
                diag(diag_level::error, out->location, detail::const_lowercase(text), h.line, h.column);
                return ctype::make(ctype::bool_k);
            }
            if (a_receiver.yields && !a_args[0].is_unknown() && !a_args[0].tentative) {
                const ctype yielded = ctype_from_type_info(a_receiver.yields);
                if (!yielded.is_unknown() && !assignable(yielded, a_args[0])) {
                    diag(diag_level::error, out->location, "advance() cannot store the yielded '" + ctype_name(yielded) + "' into '" + ctype_name(a_args[0]) + "'");
                }
            }
            return ctype::make(ctype::bool_k);
        }
        coroutine_member_missing(a_receiver, a_member);
        return {};
    }
    // An argument's shape where the load is sure of it: a number, text, an array, a map, a function or a math value (the api
    // catalog's shapes, BuiltinShape); 0 for anything else
    static uint8_t argument_shape(const ctype& a_type) {
        switch (a_type.kind) {
            case ctype::int_k: return a_type.cls.empty() ? shape_number : 0;
            case ctype::float_k: return shape_number;
            case ctype::string_k: case ctype::utf8_k: return shape_text;
            case ctype::array_k: return shape_array;
            case ctype::map_k: return shape_map;
            case ctype::function_k: return shape_function;
            case ctype::math_k: return shape_math;
            default: return 0;
        }
    }
    static std::string shape_spelling(uint8_t a_shapes) {
        static constexpr std::pair<uint8_t, const char*> names[] = {{shape_number, "a number"}, {shape_text, "text"}, {shape_array, "an array"},
            {shape_map, "a map"}, {shape_function, "a function"}, {shape_math, "a vector or color"}};
        std::string result;
        for (const auto& [shape, name] : names) {
            if (a_shapes & shape) { result += (result.empty() ? "" : " or ") + std::string(name); }
        }
        return result;
    }
    // the first argument whose shape the catalog's signature does not take (a_shapes: per argument, BUILTIN_SHAPE_SLOTS wide)
    void checkBuiltinShapes(const std::string& a_callee, const uint8_t* a_shapes, const std::vector<ctype>& a_args, const call_expr* a_call) {
        for (size_t i = 0; i < a_args.size() && i < a_call->arguments.size(); ++i) {
            const uint8_t takes = a_shapes[std::min(i, BUILTIN_SHAPE_SLOTS - 1)];
            const uint8_t shape = argument_shape(a_args[i]);
            if (shape == 0 || (takes & shape)) { continue; }
            diag(diag_level::error, a_call->arguments[i]->location, "argument " + std::to_string(i + 1) + " of " + a_callee + ": cannot pass '" +
                 ctype_name(a_args[i]) + "' where " + shape_spelling(takes) + " is expected");
            return;
        }
    }
    void checkBuiltinMethodShapes(const ctype& a_receiver, const member_expr* a_member, const call_expr* a_call, const std::vector<ctype>& a_args) {
        uint8_t receiver;
        switch (a_receiver.kind) {
            case ctype::array_k: receiver = 0; break;
            case ctype::map_k: receiver = 1; break;
            case ctype::string_k: receiver = 2; break;
            case ctype::utf8_k: receiver = 3; break;
            default: return;
        }
        const auto* signature = findBuiltinArity(receiver, a_member->member);
        if (!signature || a_args.size() < signature->minimum || a_args.size() > signature->maximum) { return; }
        checkBuiltinShapes("'" + std::string(a_member->member) + "' on '" + ctype_name(a_receiver) + "'", signature->shapes, a_args, a_call);
    }
    // math::name(args): each argument against the signatures of the name whose counts take the call
    void checkMathShapes(const member_expr* a_member, const call_expr* a_call, const std::vector<ctype>& a_args) {
        uint8_t takes[BUILTIN_SHAPE_SLOTS] = {};
        bool found = false;
        for (const auto& row : MATH_FUNCTION_SHAPES) {
            if (row.name != a_member->member || a_args.size() < row.minimum || a_args.size() > row.maximum) { continue; }
            found = true;
            for (size_t i = 0; i < BUILTIN_SHAPE_SLOTS; ++i) { takes[i] |= row.shapes[i]; }
        }
        if (found) { checkBuiltinShapes("'math::" + std::string(a_member->member) + "'", takes, a_args, a_call); }
    }
    void checkBuiltinArity(const ctype& a_receiver, const member_expr* a_member, const call_expr* a_call) {
        uint8_t receiver;
        switch (a_receiver.kind) {
            case ctype::array_k: receiver = 0; break;
            case ctype::map_k: receiver = 1; break;
            case ctype::string_k: receiver = 2; break;
            case ctype::utf8_k: receiver = 3; break;
            default: return;
        }
        const auto* signature = findBuiltinArity(receiver, a_member->member);
        if (!signature) { return; }
        const size_t count = a_call->arguments.size();
        if (count >= signature->minimum && count <= signature->maximum) { return; }
        const std::string expected = signature->minimum == signature->maximum ? detail::arity_detail(signature->minimum, count)
            : std::to_string(signature->minimum) + " to " + std::to_string(signature->maximum) + " arguments, got " + std::to_string(count);
        diag(diag_level::error, a_call->location, "'" + std::string(a_member->member) + "' on '" + ctype_name(a_receiver) + "' expects " + expected);
    }

    // A builtin method's result where the registry's catalog states one type: a diagnostics fact (tentative), never a proof source.
    // to_int / to_float answer their fallback on a failed parse, so only a fallback of the result's own type settles it
    ctype builtin_result(const ctype& a_receiver, const member_expr* a_member, const std::vector<ctype>& a_args) {
        uint8_t receiver;
        switch (a_receiver.kind) {
            case ctype::array_k: receiver = 0; break;
            case ctype::map_k: receiver = 1; break;
            case ctype::string_k: receiver = 2; break;
            case ctype::utf8_k: receiver = 3; break;
            default: return {};
        }
        ctype result;
        if (receiver >= 2 && a_args.size() == 1 && ((a_member->member == "to_int" && a_args[0].kind == ctype::int_k) ||
                                                    (a_member->member == "to_float" && a_args[0].kind == ctype::float_k))) {
            result = ctype::make(a_args[0].kind);
        } else if (const auto* signature = findBuiltinArity(receiver, a_member->member)) {
            switch (signature->result) {
                case BuiltinResult::int_result: result = ctype::make(ctype::int_k); break;
                case BuiltinResult::float_result: result = ctype::make(ctype::float_k); break;
                case BuiltinResult::bool_result: result = ctype::make(ctype::bool_k); break;
                case BuiltinResult::string_result: result = ctype::make(ctype::string_k); break;
                case BuiltinResult::char_result: result = ctype::make(ctype::char_k); break;
                case BuiltinResult::self_text: result = ctype::make(a_receiver.kind); break;
                case BuiltinResult::element: result = container_value_type(a_receiver); result.element_proof = false; break;
                default: break;
            }
        }
        result.tentative = true;
        result.declared_result = !result.is_unknown();
        return result;
    }

    // A call that uses the result of a builtin that changes its known receiver in place and returns nothing (detail/in_place_result.hpp)
    void in_place_result_used(const ctype& a_receiver, const member_expr* a_member, const call_expr* a_call) {
        std::optional<std::string_view> twin;
        switch (a_receiver.kind) {
            case ctype::array_k: twin = in_place_method_twin(0, a_member->member); break;
            case ctype::map_k: twin = in_place_method_twin(1, a_member->member); break;
            case ctype::string_k: case ctype::utf8_k: twin = in_place_method_twin(2, a_member->member); break;
            case ctype::weak_k: case ctype::object_k: if (a_member->member == "reset") { twin = std::string_view{}; } break;
            default: break;
        }
        if (!twin) { return; }
        std::string text = detail::in_place_result_text(a_member->member, detail::method_value_receiver(a_member), *twin);
        if (a_member->call_result_discarded) {
            nothing_calls_[a_call] = {text, text};
            return;
        }
        diag(diag_level::error, a_call->location, text);
    }

    // A returned expression all of whose calls return nothing (either arm of a conditional): a return of nothing, its calls in out
    bool calls_nothing(const expression* e, std::vector<const call_expr*>* out = nullptr) const {
        if (!e) { return false; }
        if (e->get_type() == node_type::ternary_expr) {
            const auto* t = static_cast<const ternary_expr*>(e);
            return calls_nothing(t->then_expression.get(), out) && calls_nothing(t->else_expression.get(), out);
        }
        if (e->get_type() != node_type::call_expr || !nothing_calls_.count(static_cast<const call_expr*>(e))) { return false; }
        if (out) { out->push_back(static_cast<const call_expr*>(e)); }
        return true;
    }
    // A body with a deduced result has walked its returns: one that gives a value makes each return of nothing the use it is
    // (C++ deduces no result from both); none makes the body a callee that returns nothing, passing on the first reason, unless
    // the body never returns (every path throws: its call has no result to use or to miss)
    bool close_deduced_body(const fn_ctx& done, const statement* body, std::string& reason) {
        if (done.gives_value) {
            for (const call_expr* c : done.nothing_returns) {
                if (auto it = nothing_calls_.find(c); it != nothing_calls_.end()) { diag(diag_level::error, c->location, it->second.used); }
            }
            return false;
        }
        if (!body || !(return_flow().analyze(body).paths & (return_flow::normal | return_flow::maybe_normal | return_flow::returns | return_flow::bare_return))) {
            return false;
        }
        for (const call_expr* c : done.nothing_returns) {
            if (auto it = nothing_calls_.find(c); it != nothing_calls_.end() && !it->second.reason.empty()) { reason = it->second.reason; break; }
        }
        return !has_includes_;
    }

    // A callback whose first parameter is a reference: a lambda's, or that of every overload of a script function the name declares
    bool callback_takes_first_by_reference(const expression* callback) const {
        if (!callback) { return false; }
        if (callback->get_type() == node_type::lambda_expr) {
            const auto* l = static_cast<const lambda_expr*>(callback);
            return !l->parameters.empty() && l->parameters[0].is_reference;
        }
        if (callback->get_type() != node_type::identifier_expr) { return false; }
        auto it = functions_.find(static_cast<const identifier_expr*>(callback)->name);
        if (it == functions_.end() || it->second.empty()) { return false; }
        return std::all_of(it->second.begin(), it->second.end(), [](const function_decl* fn) { return !fn->parameters.empty() && fn->parameters[0].is_reference; });
    }

    // A builtin that reads its callback's result (detail::callback_result_wanted), handed a callback that returns nothing: a lambda
    // whose returns give no value, a function or lambda declared void
    void callback_result_read(std::string_view builtin, std::string_view wants, const expression* callback) {
        if (!callback) { return; }
        std::string shown;
        if (callback->get_type() == node_type::lambda_expr) {
            const auto* l = static_cast<const lambda_expr*>(callback);
            if (nothing_lambdas_.count(l) || declared_void(l->return_type.get())) { shown = "the lambda"; }
        } else if (callback->get_type() == node_type::identifier_expr) {
            const auto* id = static_cast<const identifier_expr*>(callback);
            bool nothing = declared_void(callback_declared_return(callback));
            if (rebindable(id->name)) { return; }
            if (auto it = functions_.find(id->name); !nothing && it != functions_.end() && !it->second.empty()) {
                nothing = std::all_of(it->second.begin(), it->second.end(), [&](const function_decl* fn) { return returns_nothing(fn); });
            }
            if (!nothing) { nothing = lambda_variable_returns_nothing(id) != nullptr; }
            if (nothing) { shown = "'" + std::string(id->name) + "'"; }
        }
        if (shown.empty()) { return; }
        diag(diag_level::error, callback->location, shown + " returns nothing, but " + std::string(builtin) + " needs " + std::string(wants) + " from it");
    }

    // The return type a callback argument declares: a lambda's spelled one, or the one overload of a named script function
    const type_info* callback_declared_return(const expression* arg) const {
        if (!arg) { return nullptr; }
        if (arg->get_type() == node_type::lambda_expr) { return static_cast<const lambda_expr*>(arg)->return_type.get(); }
        if (arg->get_type() == node_type::identifier_expr) {
            auto it = functions_.find(static_cast<const identifier_expr*>(arg)->name);
            if (it != functions_.end() && it->second.size() == 1 && it->second[0]) { return it->second[0]->return_type.get(); }
        }
        return nullptr;
    }

    // The array helpers' results (the registry's map, reduce, for_each, find, find_index, some, every, concat, insert): map is array<T>
    // when its callback declares T, as the runtime builds it; find's element may be null, so it stays open
    bool array_helper_result(const ctype& recv, std::string_view member, const call_expr* c, const std::vector<ctype>& args, ctype& out) {
        if (member == "some" || member == "every") { out = ctype::make(ctype::bool_k); return true; }
        if (member == "find_index") { out = ctype::make(ctype::int_k); return true; }
        if (member == "for_each" || member == "insert") { out = ctype::make(ctype::null_k); return true; }
        if (member == "concat" && args.size() == 1) { out = recv; out.const_key = false; out.origin = nullptr; return true; }
        if (member == "map" && args.size() == 1) {
            out = ctype::make(ctype::array_k);   // an array; its element type is the runtime's (array<T> for a callback declaring T)
            return true;
        }
        if (member == "reduce" && (args.size() == 1 || args.size() == 2)) {
            // the callback's declared type when the initial value has it too (an empty array returns the initial value)
            const type_info* r = callback_declared_return(c->arguments[0].get());
            out = {};
            if (r && r->base_type != script_value_type::jai_reference_type && args.size() == 2) {
                const ctype declared = ctype_from_type_info(r);
                if (!declared.is_unknown() && declared.kind == args[1].kind && declared.cls == args[1].cls) { out = declared; }
            }
            return true;
        }
        return false;
    }

    // A builtin method of a declared container or string read without a call (the runtime's text, detail::method_value_text): a
    // builtin name wins over a map's key of the same name, so a map is certain too
    void builtin_method_value(const ctype& recv, const member_expr* m) {
        int kind = -1;
        const char* owner = nullptr;
        switch (recv.kind) {
            case ctype::array_k: kind = 0; owner = "array"; break;
            case ctype::map_k: kind = 1; owner = "map"; break;
            case ctype::string_k: kind = 2; owner = "string"; break;
            case ctype::utf8_k: kind = 3; owner = "utf8_string"; break;
            default: return;
        }
        if (!eng_.builtin_method_ids(kind).count(eng_.symbolize(m->member))) {
            if (kind == 1) { map_dot_key(m); }
            return;
        }
        diag(diag_level::error, m->location, detail::method_value_text(m->member, owner, detail::method_value_receiver(m), true) + (m->less_compared ? " (" + no_generic_text(m->member) + ")" : std::string()));
    }
    // A member of a known int, float, bool or char: the registry (BUILTIN_ARITIES) gives these kinds no methods, so any name is the
    // run time's text
    void scalar_member_missing(const ctype& recv, const member_expr* m) {
        script_value_type kind;
        switch (recv.kind) {
            case ctype::int_k: kind = script_value_type::jai_int_type; break;
            case ctype::float_k: kind = script_value_type::jai_float_type; break;
            case ctype::bool_k: kind = script_value_type::jai_bool_type; break;
            case ctype::char_k: kind = script_value_type::jai_char_type; break;
            default: return;
        }
        if (recv.tentative || recv.diagnostic_only || m->is_static) { return; }
        diag(diag_level::error, m->location, no_member_text(static_cast<engine*>(nullptr), base_type_spelling(kind), m->member));
    }
    // a map's key is read with [] (only an enum's value table reads its keys with a dot, and an enum's name is no map here): the run time's text
    void map_dot_key(const member_expr* m) {
        diag(diag_level::error, m->location, "map has no member '" + std::string(m->member) + "'; read the key as [\"" + std::string(m->member) + "\"]");
    }
    static size_t edit_distance(std::string_view a, std::string_view b) {
        std::vector<size_t> row(b.size() + 1);
        for (size_t j = 0; j <= b.size(); ++j) { row[j] = j; }
        for (size_t i = 1; i <= a.size(); ++i) {
            size_t diagonal = row[0];
            row[0] = i;
            for (size_t j = 1; j <= b.size(); ++j) {
                const size_t up = row[j];
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
                diagonal = up;
            }
        }
        return row[b.size()];
    }

    // Handle builtins every object value answers (the shared_ptr registry): a class method of the same name wins first, so this
    // runs on misses only.
    static bool handle_builtin_result(std::string_view member, ctype& out) {
        if (member == "use_count" || member == "cpp_ref_count") { out = ctype::make(ctype::int_k); return true; }
        if (member == "unique") { out = ctype::make(ctype::bool_k); return true; }
        if (member == "reset") { out = {}; return true; }
        return false;
    }

    // -------------------------------------------------------------- binary data

    // The engine's binary:: library (detail/binary_data.hpp) and string::from_bytes, unless the program declares its own
    bool library_namespace(std::string_view ns) const { return namespaces_.count(ns) == 0; }
    static bool static_path(const member_expr* m, std::string_view ns) {
        return m && m->is_static && m->object && m->object->get_type() == node_type::identifier_expr &&
               static_cast<const identifier_expr*>(m->object.get())->name == ns;
    }
    static bool binary_width_name(std::string_view n) {
        return n == "int8" || n == "int16" || n == "int32" || n == "int64" || n == "uint8" || n == "uint16" || n == "uint32" ||
               n == "float32" || n == "float64";
    }
    bool binary_width_token(const member_expr* m) const {
        return static_path(m, "binary") && library_namespace("binary") && binary_width_name(m->member);
    }
    bool binary_call(const expression* e, std::string_view member) const {
        if (!e || e->get_type() != node_type::call_expr) { return false; }
        auto* c = static_cast<const call_expr*>(e);
        return c->callee && c->callee->get_type() == node_type::member_expr &&
               static_path(static_cast<const member_expr*>(c->callee.get()), "binary") &&
               static_cast<const member_expr*>(c->callee.get())->member == member && library_namespace("binary");
    }
    static bool is_class_kind(const ctype& t, std::string_view cls) { return t.kind == ctype::object_k && t.cls == cls; }

    // name, width pairs (binary::layout, reader.unpack): an odd count, a name that is not a string or a width that is not a
    // binary width token is certain at load; unknown kinds stay lenient
    void check_binary_pairs(const call_expr* c, const std::vector<ctype>& args, const char* who) {
        const size_t n = args.size();
        if (n == 0 || n % 2 != 0) {
            diag(diag_level::error, c->location, std::string(who) + " expects name, width pairs, got " + std::to_string(n) +
                 (n == 1 ? " argument" : " arguments"));
            return;
        }
        for (size_t i = 0; i < n; i += 2) {
            const ctype& name = args[i];
            if (!name.is_unknown() && name.kind != ctype::string_k && name.kind != ctype::utf8_k) {
                diag(diag_level::error, c->arguments[i]->location, std::string(who) + " argument " + std::to_string(i + 1) +
                     " must be a field name string, got " + ctype_name(name));
            }
            const ctype& w = args[i + 1];
            if (!w.is_unknown() && !is_class_kind(w, "binary::width")) {
                diag(diag_level::error, c->arguments[i + 1]->location, std::string(who) + " argument " + std::to_string(i + 2) +
                     " must be a binary width (binary::int32, binary::text(8), ...), got " + ctype_name(w));
            }
        }
    }

    // binary::layout(...), binary::text(n), binary::bytes(n), binary::reader(...), binary::writer(), string::from_bytes(...)
    bool library_namespace_call(const call_expr* c, const member_expr* m, const std::vector<ctype>& args, ctype& out) {
        if (static_path(m, "binary") && library_namespace("binary")) {
            if (m->member == "layout") { check_binary_pairs(c, args, "binary::layout"); out = object_of("binary::layout"); return true; }
            if (m->member == "text" || m->member == "bytes") { out = object_of("binary::width"); return true; }
            if (m->member == "reader") { out = object_of("binary::reader"); return true; }
            if (m->member == "writer") { out = object_of("binary::writer"); return true; }
            return false;
        }
        if (static_path(m, "string") && library_namespace("string") && m->member == "from_bytes") {
            out = ctype::make(ctype::string_k);
            return true;
        }
        return false;
    }

    // the layout a call argument statically is: an inline binary::layout(...) call, or a variable declared once with one and
    // never assigned again
    const call_expr* static_layout(const expression* e, int depth = 0) {
        if (!e || depth > 8) { return nullptr; }
        if (binary_call(e, "layout")) { return static_cast<const call_expr*>(e); }
        if (e->get_type() != node_type::identifier_expr) { return nullptr; }
        auto* id = static_cast<const identifier_expr*>(e);
        auto it = layout_inits_.find(id->name);
        if (it == layout_inits_.end() || it->second.size() != 1 || !it->second[0] || assigned_names_.count(id->name)) { return nullptr; }
        return static_layout(it->second[0], depth + 1);
    }

    // a statically known layout's field kinds: 1 integer, 2 float, 3 text, 4 bytes; empty when any width is not a token
    std::vector<uint8_t> layout_kinds(const call_expr* l) {
        std::vector<uint8_t> kinds;
        if (l->arguments.size() % 2 != 0) { return {}; }
        for (size_t i = 1; i < l->arguments.size(); i += 2) {
            const expression* w = l->arguments[i].get();
            if (w->get_type() == node_type::member_expr && binary_width_token(static_cast<const member_expr*>(w))) {
                const std::string_view n = static_cast<const member_expr*>(w)->member;
                kinds.push_back(n == "float32" || n == "float64" ? 2 : 1);
            } else if (binary_call(w, "text")) {
                kinds.push_back(3);
            } else if (binary_call(w, "bytes")) {
                kinds.push_back(4);
            } else {
                return {};
            }
        }
        return kinds;
    }

    // r.read(Class, layout) with a class this program declares and a statically known layout: the field counts and each
    // field's declared kind against its width (the runtime's check, at load)
    void check_binary_read(const call_expr* c) {
        if (c->arguments.size() != 2 || c->arguments[0]->get_type() != node_type::identifier_expr) { return; }
        const call_expr* l = static_layout(c->arguments[1].get());
        if (!l) { return; }
        const std::vector<uint8_t> kinds = layout_kinds(l);
        if (kinds.empty()) { return; }
        const std::string_view cname = static_cast<const identifier_expr*>(c->arguments[0].get())->name;
        std::vector<std::string_view> chain;
        for (std::string_view at = cname; !at.empty();) {
            const sclass* sc = find_sclass(at);
            if (!sc || chain.size() > 32 || sc->decl->base_classes.size() > 1) { return; }
            chain.push_back(at);
            at = sc->decl->base_classes.empty() ? std::string_view() : sc->decl->base_classes[0];
        }
        std::vector<const variable_decl*> fields;
        for (size_t i = chain.size(); i-- > 0;) {
            const sclass* sc = find_sclass(chain[i]);
            for (const auto& mem : sc->decl->members) {
                if (!mem.declaration || mem.declaration->get_type() != node_type::variable_decl) { continue; }
                auto* v = static_cast<const variable_decl*>(mem.declaration.get());
                if (sc->static_fields.count(v->name)) { continue; }
                fields.push_back(v);
            }
        }
        if (fields.size() != kinds.size()) {
            diag(diag_level::error, c->location, "Cannot read " + std::string(cname) + " from a layout of " + std::to_string(kinds.size()) +
                 (kinds.size() == 1 ? " field: " : " fields: ") + std::string(cname) + " has " + std::to_string(fields.size()) +
                 (fields.size() == 1 ? " field" : " fields"));
            return;
        }
        for (size_t i = 0; i < fields.size(); ++i) {
            const type_info* t = fields[i]->type.get();
            if (!t || t->base_type == script_value_type::jai_any_type || fields[i]->function_spelling) { continue; }
            const bool ok = (kinds[i] == 1 && t->base_type == script_value_type::jai_int_type) ||
                            (kinds[i] == 2 && t->base_type == script_value_type::jai_float_type) ||
                            (kinds[i] == 3 && (t->base_type == script_value_type::jai_string_type || t->base_type == script_value_type::jai_utf8_string_type)) ||
                            (kinds[i] == 4 && t->base_type == script_value_type::jai_array_type);
            if (!ok) {
                diag(diag_level::error, c->location, "Cannot read layout field " + std::to_string(i + 1) + " into " + std::string(cname) +
                     "." + std::string(fields[i]->name) + " (declared " + t->canonical_name() + ")");
            }
        }
    }

    // s.to_bytes() / s.reader() on a proven string, and the binary::reader / binary::writer members whose arguments the
    // bound signatures cannot describe (unpack's pairs, read's class and layout, pack's layout)
    bool binary_member_call(const call_expr* c, const member_expr* m, const ctype& recv, const std::vector<ctype>& args, ctype& out) {
        if (m->is_static) { return false; }
        if ((recv.kind == ctype::string_k || recv.kind == ctype::utf8_k) && (!recv.tentative || recv.element_proof)) {
            if (m->member == "to_bytes") { out = ctype_from_type_info(eng_.get_type_info_array(eng_.get_type_info_uint8())); return true; }
            if (m->member == "reader" && args.empty()) { out = object_of("binary::reader"); return true; }
            return false;
        }
        if (is_class_kind(recv, "binary::reader")) {
            if (m->member == "unpack") {
                if (!(args.size() == 1 && (args[0].is_unknown() || is_class_kind(args[0], "binary::layout")))) { check_binary_pairs(c, args, "unpack"); }
                out = ctype::make(ctype::map_k);
                return true;
            }
            if (m->member == "read" && args.size() == 2) {
                if (!args[1].is_unknown() && !is_class_kind(args[1], "binary::layout")) {
                    diag(diag_level::error, c->arguments[1]->location, "read expects a binary::layout, got " + ctype_name(args[1]));
                }
                check_binary_read(c);
                const expression* cls = c->arguments[0].get();
                out = cls->get_type() == node_type::identifier_expr && find_sclass(static_cast<const identifier_expr*>(cls)->name)
                    ? object_of(static_cast<const identifier_expr*>(cls)->name) : ctype{};
                return true;
            }
            return false;
        }
        if (is_class_kind(recv, "binary::writer") && m->member == "pack" && args.size() == 2) {
            if (!args[0].is_unknown() && !is_class_kind(args[0], "binary::layout")) {
                diag(diag_level::error, c->arguments[0]->location, "pack expects a binary::layout, got " + ctype_name(args[0]));
            }
            if (!args[1].is_unknown() && args[1].kind != ctype::map_k) {
                diag(diag_level::error, c->arguments[1]->location, "pack expects a map of field values, got " + ctype_name(args[1]));
            }
            out = object_of("binary::writer");
            return true;
        }
        return false;
    }

    // the result kind a strict host class's bound method declares (class_definition::strict_signatures)
    ctype host_result(const param_type_info& r) {
        switch (r.base_type) {
            case script_value_type::jai_int_type: return ctype::make(ctype::int_k);
            case script_value_type::jai_float_type: return ctype::make(ctype::float_k);
            case script_value_type::jai_bool_type: return ctype::make(ctype::bool_k);
            case script_value_type::jai_char_type: return ctype::make(ctype::char_k);
            case script_value_type::jai_string_type: return ctype::make(ctype::string_k);
            case script_value_type::jai_array_type: return ctype::make(ctype::array_k);
            case script_value_type::jai_map_type: return ctype::make(ctype::map_k);
            case script_value_type::jai_object_type:
                if (auto def = eng_.get_class_definition_by_type(r.cpp_type)) { return object_of(def->get_name()); }
                return {};
            default: return {};
        }
    }

    // a method call on a host class that opted into strict signatures: the arity and argument kinds its overloads accept, and
    // the result they agree on; other host classes stay lenient
    ctype check_strict_host_method(std::string_view cls, std::string_view member, const std::vector<ctype>& args, const source_location& loc,
                                   const call_expr* c = nullptr, const member_expr* m = nullptr) {
        auto def = engine_class(cls);
        if (!def || !def->is_cpp_class() || def->extended_ever()) { return {}; }
        const auto& table = def->cpp_method_overload_table();
        const uint64_t member_id = eng_.symbolize(member);
        auto it = table.find(member_id);
        if (it == table.end()) { return {}; }
        // a class the binder did not mark strict is held to the signatures it bound (registration records them) unless something
        // else may take the call: an untyped binding of the name, or a class below it that declares the method
        const bool strict = def->strict_signatures();
        if (!strict) {
            if (derived_defines_member(*def, member_id, member_id) || !overrides_of(cls, member).empty()) { return {}; }
            for (const auto& [arity, entries] : it->second) {
                for (const auto& entry : entries) { if (entry.param_types.size() != arity) { return {}; } }
            }
        }
        const std::string fname = std::string(cls) + "." + std::string(member);
        auto bucket = it->second.find(args.size());
        if (bucket == it->second.end() || bucket->second.empty()) {
            std::vector<size_t> arities;
            for (const auto& [n, entries] : it->second) { if (!entries.empty()) { arities.push_back(n); } }
            std::sort(arities.begin(), arities.end());
            std::string text;
            for (size_t n : arities) { text += (text.empty() ? "" : " or ") + std::to_string(n); }
            diag(diag_level::error, loc, "'" + fname + "' expects " + text + " argument(s), got " + std::to_string(args.size()));
            return {};
        }
        {
            std::vector<std::vector<param_type_info>> overloads;
            for (const auto& entry : bucket->second) { overloads.push_back(entry.param_types); }
            if (report_host_callback(fname, overloads, args, c)) { return {}; }
        }
        std::optional<ctype> result;
        bool agree = true;
        bool viable = false;
        for (const auto& entry : bucket->second) {
            bool ok = true;
            for (size_t i = 0; i < args.size() && i < entry.param_types.size(); ++i) {
                if (!host_param_accepts(args[i], entry.param_types[i]) || !host_callback_issue(args[i], entry.param_types[i]).empty()) { ok = false; break; }
            }
            if (!ok) { continue; }
            viable = true;
            ctype r = host_result(entry.result);
            if (!result) { result = r; }
            else if (result->kind != r.kind || result->cls != r.cls) { agree = false; }
        }
        if (!viable) {
            diag(diag_level::error, loc, "no overload of '" + fname + "' accepts these argument types");
            return {};
        }
        return strict && agree && result ? *result : ctype{};
    }

    // Cls::m(args) on a host class: a count the live binding does not take is the binder's run-time throw; a binding of
    // unknown arity, or a script static of the name, keeps the call lenient
    void check_host_static_arity(std::string_view cls, std::string_view member, size_t argc, const source_location& loc) {
        auto def = engine_class(cls);
        if (!def || !def->is_cpp_class() || def->extended_ever()) { return; }
        const uint64_t id = eng_.symbolize(member);
        if (def->script_static_method_overloads().count(id)) { return; }
        const size_t arity = def->static_method_arity(id);
        if (arity == SIZE_MAX || arity == argc) { return; }
        diag(diag_level::error, loc, "'" + std::string(cls) + "::" + std::string(member) + "' expects " + std::to_string(arity) +
             " argument(s), got " + std::to_string(argc));
    }

    // a host class that opted into strict signatures holds its constructions to the constructors its binder registered
    void check_strict_host_constructor(const std::string& cls, const std::vector<ctype>& args, const source_location& loc) {
        auto def = engine_class(cls);
        if (def && def->is_cpp_class() && def->strict_signatures() && !def->extended_ever()) { check_host_call(cls, args, loc); }
    }

    // -------------------------------------------------------------- calls

    struct call_check {
        bool checked = false;   // a statically known callee was actually validated
        ctype result{};
    };

    // The declarations of a program-scope function name this unit declares that other units hold: the program's other units and
    // what earlier executes left in the engine (detail::declare_function_overload's set), less any signature this unit declares again
    const std::vector<const function_decl*>& foreign_overloads(std::string_view name, const std::vector<const function_decl*>& own) {
        auto [entry, fresh] = foreign_overloads_.try_emplace(name);
        std::vector<const function_decl*>& result = entry->second;
        if (!fresh) { return result; }
        const auto redeclared = [&](const std::vector<parameter>& params) {
            for (const auto* fn : own) {
                if (same_signature(fn->parameters, params) || differs_only_by_reference(fn->parameters, params)) { return true; }
            }
            for (const auto* fn : result) {
                if (same_signature(fn->parameters, params)) { return true; }
            }
            return false;
        };
        const auto own_body = [&](const block_stmt* body) {
            for (const auto* fn : own) { if (body && fn->body.get() == body) { return true; } }
            return false;
        };
        if (program_functions_) {
            eng_.note(name);
            if (auto pit = program_functions_->find(std::string(name)); pit != program_functions_->end()) {
                for (const auto& [path, decls] : pit->second) {
                    for (const auto& d : decls) {
                        if (d && !own_body(d->body.get()) && !redeclared(d->parameters)) { result.push_back(d.get()); }
                    }
                }
            }
        }
        for (const auto& fn : eng_.held_function_declarations(name)) {
            if (!fn || own_body(fn->body.get()) || redeclared(fn->parameters())) { continue; }
            auto spelled = std::make_unique<function_decl>(source_location{}, name);
            spelled->parameters = fn->parameters();
            spelled->return_type = fn->return_type;
            spelled->body = fn->body;
            result.push_back(spelled.get());
            held_declarations_.push_back(std::move(spelled));
        }
        return result;
    }

    // A call to a program-scope function whose overloads other units hold as well: an error only when no declaration of the
    // program takes it; the result is typed only when the one that does is this unit's own, and no proof rests on the set
    ctype check_program_overloads(std::string_view fname, const std::vector<const function_decl*>& own, const std::vector<const function_decl*>& foreign,
                                  const std::vector<ctype>& args, const source_location& loc, bool foreign_first = false) {
        if (script_callee_takes_type_arguments(fname, "function")) { return {}; }
        std::vector<const function_decl*> all(own);
        all.insert(all.end(), foreign.begin(), foreign.end());
        const bool rivals = arity_rivals(all, args.size());
        const function_decl* only_viable = nullptr;
        int viable_count = 0;
        int nodiscard_viable = 0;
        int void_viable = 0;
        std::string reason;
        for (const auto* fn : all) {
            if (!script_fn_viable(fn, args, rivals)) { continue; }
            ++viable_count;
            nodiscard_viable += fn->is_nodiscard;
            void_viable += returns_nothing(fn);
            if (returns_nothing(fn) && reason.empty()) { reason = nothing_reason(fn); }
            only_viable = fn;
        }
        if (viable_count == 0) {
            report_no_overload(fname, all, args, loc, foreign_first);
            return {};
        }
        if (viable_count == 1) { warn_dynamic_handle_arguments(only_viable, args); }
        if (nodiscard_viable == viable_count) { nodiscard_hit_ = {std::string(fname), {}}; }
        if (void_viable == viable_count) { void_hit_ = {"'" + std::string(fname) + "'", {}, nullptr, reason}; }
        if (viable_count > 1 || std::find(own.begin(), own.end(), only_viable) == own.end()) { return {}; }
        ctype result = only_viable->is_coroutine ? ctype::make(ctype::coroutine_k) : ctype_from_type_info(only_viable->return_type.get());
        if (only_viable->is_coroutine) { result.yields = only_viable->return_type.get(); }
        return result;
    }

    // A function declared void: its call has no value (a coroutine's call is the coroutine)
    bool returns_nothing(const function_decl* fn) const {
        return !fn->is_coroutine && (declared_void(fn->return_type.get()) || (!fn->return_type && nothing_functions_.count(fn)));
    }
    // the reason a deduced body that returns nothing passes on (empty for a void declaration or a plain call)
    std::string nothing_reason(const function_decl* fn) const {
        auto it = fn->return_type ? nothing_functions_.end() : nothing_functions_.find(fn);
        return it == nothing_functions_.end() ? std::string() : it->second;
    }
    // The library's forms that take their arguments as values: what they are passed cannot be rebound through them
    static bool value_taking_library_call(const call_expr* c) {
        if (!c->callee) { return false; }
        if (c->callee->get_type() == node_type::identifier_expr) {
            const std::string_view name = static_cast<const identifier_expr*>(c->callee.get())->name;
            return name == "parallel_transform" || name == "parallel_reduce";
        }
        return static_path(static_cast<const member_expr*>(c->callee->get_type() == node_type::member_expr ? c->callee.get() : nullptr), "array") ||
               static_path(static_cast<const member_expr*>(c->callee->get_type() == node_type::member_expr ? c->callee.get() : nullptr), "map");
    }
    // A name something may rebind (rebinding_names): assigned, passed where a reference may take it, bound by a reference
    bool rebindable(std::string_view name) const { return assigned_names_.count(name) || passed_names_.count(name); }
    // A local declared from a lambda literal and never rebound (rebindable), whose body returns nothing: its reason
    const std::string* lambda_variable_returns_nothing(const identifier_expr* id) {
        const var_entry* local = find_local(id->name);
        if (!local || !local->declared_lambda || rebindable(id->name)) { return nullptr; }
        auto it = nothing_lambdas_.find(local->declared_lambda);
        return it == nothing_lambdas_.end() ? nullptr : &it->second;
    }
    static bool declared_void(const type_info* t) { return t && t->base_type == script_value_type::jai_null_type && t->type_name == "void"; }

    // args already inferred by the caller; foreign = the overloads belong to another unit's source; scored = the runtime picks
    // among them by type (methods); a program's free functions of one name are redefinitions, the last one taking the call
    call_check check_script_overloads(std::string_view fname, const std::vector<const function_decl*>& overloads,
                                      const std::vector<ctype>& args, const source_location& loc, bool foreign = false,
                                      bool scored = true, std::string_view kind = "function") {
        if (script_callee_takes_type_arguments(fname, kind)) { return {}; }
        const function_decl* only_viable = nullptr;
        int viable_count = 0;
        int nodiscard_viable = 0;
        int void_viable = 0;
        std::string reason;
        for (const auto* fn : overloads) {
            if (script_fn_viable(fn, args, scored && arity_rivals(overloads, args.size()))) {
                ++viable_count;
                nodiscard_viable += fn->is_nodiscard;
                void_viable += returns_nothing(fn);
                if (returns_nothing(fn) && reason.empty()) { reason = nothing_reason(fn); }
                only_viable = fn;
            }
        }
        if (viable_count > 0 && nodiscard_viable == viable_count) { nodiscard_hit_ = {std::string(fname), {}}; }
        if (viable_count > 0 && void_viable == viable_count) { void_hit_ = {"'" + std::string(fname) + "'", {}, nullptr, reason}; }
        if (viable_count == 1) { warn_dynamic_handle_arguments(only_viable, args); }
        call_check out;
        out.checked = true;
        if (viable_count == 0) {
            report_no_overload(fname, overloads, args, loc, foreign);
            // recovery: assume the first overload's return type
            if (!overloads.empty() && overloads[0]->return_type && !overloads[0]->is_coroutine) {
                out.result = ctype_from_type_info(overloads[0]->return_type.get());
            }
            return out;
        }
        if (viable_count == 1) {
            out.result = only_viable->is_coroutine ? ctype::make(ctype::coroutine_k) : ctype_from_type_info(only_viable->return_type.get());
            if (only_viable->is_coroutine) { out.result.yields = only_viable->return_type.get(); }
        }
        if (viable_count > 1 && scored) { report_ambiguous_call(fname, overloads, args, loc); }
        return out;
    }

    // Steps from cls up its single-parent chain to base (0 = itself); SIZE_MAX when base is no ancestor or the chain is not modeled
    size_t class_distance(std::string_view cls, std::string_view base) {
        size_t steps = 0;
        std::string_view at = cls;
        while (steps <= 32) {
            if (at == base) { return steps; }
            const sclass* c = find_sclass(at);
            if (!c || c->decl->base_classes.size() != 1) { break; }
            at = c->decl->base_classes[0];
            ++steps;
        }
        auto def = engine_class(at);
        if (!def) { return SIZE_MAX; }
        const size_t runtime = detail_base_distance(def.get(), base);
        return runtime == SIZE_MAX ? SIZE_MAX : steps + runtime;
    }

    static size_t detail_base_distance(const class_definition* def, std::string_view name) { return base_distance(def, name); }

    // The runtime scorer's keys (core/overload_resolution.hpp score_overloads) over static argument types; nullopt when an argument
    // or a parameter is outside what the static view ranks exactly (an unknown kind, an object a parameter may convert, a container,
    // a handle). viable=false: a typed parameter the argument matches neither exactly nor by conversion, with no var slot
    struct static_rank {
        bool viable = true;
        size_t mismatches = 0, var_count = 0, defaults = 0, conversions = 0, distance = 0;
        int score = 0;
        auto key() const { return std::tie(mismatches, score, var_count, defaults, conversions, distance); }
    };
    std::optional<static_rank> static_overload_rank(const function_decl* fn, const std::vector<ctype>& args, bool in_set) {
        auto [min_a, max_a] = arity_range(fn);
        if (args.size() < min_a || args.size() > max_a) { return static_rank{false}; }
        static_rank r;
        r.defaults = fn->parameters.size() - args.size();
        bool all_exact = true, all_convertible = true, has_var = false, loose = false;
        for (size_t i = 0; i < args.size(); ++i) {
            const parameter& p = fn->parameters[i];
            if (!p.type || p.type->type_name.empty() || p.type->type_name == "any" || p.type->base_type == script_value_type::jai_any_type) {
                has_var = true;
                ++r.var_count;
                continue;
            }
            const ctype& a = args[i];
            if (a.is_unknown()) { return std::nullopt; }
            const ctype pt = ctype_from_type_info(p.type.get());
            auto cost = [&](size_t c) {
                all_exact = false;
                if (c) { r.conversions += c; loose = loose || c >= k_loose_conversion; } else { all_convertible = false; ++r.mismatches; }
            };
            const bool primitive_a = a.kind == ctype::int_k || a.kind == ctype::float_k || a.kind == ctype::bool_k || a.kind == ctype::char_k ||
                                     a.kind == ctype::string_k || a.kind == ctype::utf8_k;
            if (pt.kind == ctype::object_k) {
                if (a.kind != ctype::object_k) { return std::nullopt; }   // a converting constructor may take it
                if (a.cls == pt.cls) { continue; }
                const size_t d = class_distance(a.cls, pt.cls);
                if (d == SIZE_MAX) { return std::nullopt; }
                all_exact = false;
                ++r.conversions;
                r.distance += d;
                continue;
            }
            if (!primitive_a) { return std::nullopt; }
            if (enum_value(pt)) { if (a.kind != ctype::int_k || a.cls != pt.cls) { cost(0); } continue; }
            if (a.kind == pt.kind && enum_value(a)) { cost(1); continue; }   // an enum's value into an int parameter converts
            if (a.kind == pt.kind) { continue; }
            if (pt.kind == ctype::float_k && a.kind == ctype::int_k) { cost(1); continue; }
            if (pt.kind == ctype::int_k && a.kind == ctype::float_k) { cost(0); continue; }
            if (p.is_reference) { cost(0); continue; }
            switch (pt.kind) {
                case ctype::int_k: case ctype::float_k: cost(a.kind == ctype::bool_k ? 1 : 0); break;
                case ctype::bool_k: cost(in_set && (a.kind == ctype::string_k || a.kind == ctype::utf8_k) ? 0 : 2); break;
                case ctype::string_k: case ctype::utf8_k: cost(a.kind == ctype::char_k ? 1 : 2); break;
                case ctype::char_k: cost(a.kind == ctype::string_k && !literal_char_text_refused(a, pt) ? 2 : 0); break;
                default: return std::nullopt;
            }
        }
        if (!all_convertible && !has_var) { r.viable = false; return r; }
        const bool all_typed = !has_var;
        if (all_typed && all_exact) { r.score = 0; }
        else if (loose) { r.score = static_cast<int>(2000 + r.conversions); }
        else if (all_typed) { r.score = static_cast<int>(1 + r.conversions); }
        else { r.score = 1000; }
        return r;
    }

    // Two overloads the runtime scorer could not tell apart on these argument types: the runtime's ambiguous-call text
    void report_ambiguous_call(std::string_view fname, const std::vector<const function_decl*>& overloads, const std::vector<ctype>& args,
                               const source_location& loc) {
        const bool in_set = arity_rivals(overloads, args.size());
        const function_decl* best = nullptr;
        const function_decl* rival = nullptr;
        std::optional<static_rank> best_rank;
        for (const auto* fn : overloads) {
            const auto rank = static_overload_rank(fn, args, in_set);
            if (!rank) { return; }
            if (!rank->viable) { continue; }
            if (!best_rank || rank->key() < best_rank->key()) { best = fn; best_rank = rank; rival = nullptr; }
            else if (rank->key() == best_rank->key() && !rival) { rival = fn; }
        }
        if (!best || !rival) { return; }
        std::string list = "(";
        for (size_t i = 0; i < args.size() && i < 6; ++i) { if (i) { list += ", "; } list += ctype_name(args[i]); }
        if (args.size() > 6) { list += ", ..."; }
        list += ")";
        const std::string name(fname);
        diag(diag_level::error, loc, "ambiguous call to '" + name + "' with " + list + ": '" + name + detail::parameter_signature(best->parameters) +
             "' and '" + name + detail::parameter_signature(rival->parameters) + "' match equally",
             best->location);
    }

    // A T& parameter's argument: a primitive of exactly that kind, a class T or a subclass; unknown and var& / auto& pass; a const T&
    // binds a temporary T made from whatever a T parameter takes (C++)
    bool reference_parameter_binds(const parameter& p, const ctype& narrowed_arg) {
        const ctype arg = narrow_declared_view(narrowed_arg);
        if (!p.type || arg.is_unknown()) { return true; }
        if (p.is_const && assignable(arg, ctype_from_type_info(p.type.get()))) { return true; }
        switch (p.type->base_type) {
            case script_value_type::jai_int_type: case script_value_type::jai_float_type: case script_value_type::jai_bool_type:
            case script_value_type::jai_char_type: case script_value_type::jai_string_type: case script_value_type::jai_utf8_string_type:
            case script_value_type::jai_object_type:
            case script_value_type::jai_vec2_type: case script_value_type::jai_vec3_type: case script_value_type::jai_vec4_type:
            case script_value_type::jai_ivec2_type: case script_value_type::jai_ivec3_type: case script_value_type::jai_color_type:
                return reference_type_compatible(arg, ctype_from_type_info(p.type.get()));
            case script_value_type::jai_array_type: case script_value_type::jai_map_type:
            case script_value_type::jai_shared_ptr_type: case script_value_type::jai_weak_ptr_type:
                if (!arg.ti || !concrete_reference_type(arg.ti) || !concrete_reference_type(p.type.get())) { return true; }
                return reference_type_compatible(arg, ctype_from_type_info(p.type.get()));
            default:
                return true;
        }
    }

    // Several overloads admit this argument count: the runtime scores them, else the one the arity admits binds and converts
    template <typename Overloads>
    static bool arity_rivals(const Overloads& overloads, size_t argc) {
        size_t admitted = 0;
        for (const auto& fn : overloads) {
            auto [min_a, max_a] = arity_range(&*fn);
            if (argc >= min_a && argc <= max_a && ++admitted > 1) { return true; }
        }
        return false;
    }

    // in_set: fn is one of several overloads, where a string's truthiness never makes a bool parameter viable (the
    // runtime scorer's rule); a lone function's bool slot still takes it by assignment
    bool script_fn_viable(const function_decl* fn, const std::vector<ctype>& args, bool in_set = false) {
        auto [min_a, max_a] = arity_range(fn);
        if (args.size() < min_a || args.size() > max_a) { return false; }
        for (size_t i = 0; i < args.size(); ++i) {
            const parameter& p = fn->parameters[i];
            if (p.is_reference) {   // a T& binds a T (a class: T or a subclass); var& and auto& bind any
                if (!reference_parameter_binds(p, args[i])) { return false; }
                continue;
            }
            if (in_set && p.type && p.type->base_type == script_value_type::jai_bool_type &&
                (args[i].kind == ctype::string_k || args[i].kind == ctype::utf8_k)) {
                return false;
            }
            if (!assignable(args[i], ctype_from_type_info(p.type.get()))) {
                return false;
            }
        }
        return true;
    }

    // A bare call in a method that a member of the name takes and refuses, while the function the member hides (C++ lookup:
    // locals, members, then globals) accepts the arguments: what the refusal adds, naming the hidden function's qualified call
    bool hidden_function_probe_ = false;
    std::string hidden_function_hint(std::string_view fname, const std::vector<ctype>& args) {
        if (!hidden_function_probe_) { return {}; }
        const auto accepts = [&](const std::vector<const function_decl*>& fns) {
            for (const auto* fn : fns) {
                if (script_fn_viable(fn, args)) { return true; }
            }
            return false;
        };
        const std::string name(fname);
        std::string qualified;
        if (const std::string space = enclosing_namespace_member(fname); !space.empty()) {
            const auto fit = namespace_fns_.find(space + "::" + name);
            if (fit == namespace_fns_.end() || !accepts(fit->second)) { return {}; }
            qualified = space + "::" + name;
        } else if (const auto it = functions_.find(fname); it != functions_.end()) {
            if (!accepts(it->second)) { return {}; }
            qualified = "::" + name;
        } else if (eng_.has_function(name)) {
            std::vector<engine::host_overload_signature> sigs;
            bool viable = !eng_.host_function_signatures(name, sigs) || sigs.empty();
            for (size_t i = 0; i < sigs.size() && !viable; ++i) {
                viable = (sigs[i].arity == SIZE_MAX || sigs[i].arity == args.size()) && host_overload_viable(sigs[i], args);
            }
            if (!viable) { return {}; }
            qualified = "::" + name;
        } else {
            return {};
        }
        return ": the method hides the function '" + name + "' here; call that one as " + qualified + "(...)";
    }

    void report_no_overload(std::string_view fname, const std::vector<const function_decl*>& overloads,
                            const std::vector<ctype>& args, const source_location& loc, bool foreign = false) {
        const declared_at declared = (overloads.empty() || foreign) ? declared_at{} : declared_at{overloads[0]->location};
        const std::string hint = hidden_function_hint(fname, args);
        if (!overloads.empty() && overloads[0]->method_instance) { fname = member_display_name(fname); }   // function_decl::shown_name
        if (overloads.size() == 1) {
            const function_decl* fn = overloads[0];
            auto [min_a, max_a] = arity_range(fn);
            if (args.size() < min_a || args.size() > max_a) {
                std::string expected = (min_a == max_a)
                    ? std::to_string(min_a)
                    : std::to_string(min_a) + " to " + std::to_string(max_a);
                diag(diag_level::error, loc, "'" + std::string(fname) + "' expects " + expected +
                     " argument(s), got " + std::to_string(args.size()) + hint, declared);
                return;
            }
            for (size_t i = 0; i < args.size(); ++i) {
                const parameter& p = fn->parameters[i];
                if (p.is_reference) {
                    if (!reference_parameter_binds(p, args[i])) {
                        diag(diag_level::error, loc, "argument " + std::to_string(i + 1) + " of '" + std::string(fname) + "': cannot bind '" +
                             ctype_name(args[i]) + "' to '" + p.name + "' declared '" + (p.is_const ? "const " : "") + p.type->canonical_name() + "&'" + hint, declared);
                        return;
                    }
                    continue;
                }
                ctype pt = ctype_from_type_info(p.type.get());
                if (!assignable(args[i], pt)) {
                    diag(diag_level::error, loc, "argument " + std::to_string(i + 1) + " of '" +
                         std::string(fname) + "': cannot pass '" + ctype_name(args[i]) + "' to '" +
                         p.name + "' declared '" + (p.type ? p.type->canonical_name() : "auto") + "'" + assignment_detail(args[i], pt) + hint,
                         declared);
                    return;
                }
            }
            return;
        }
        diag(diag_level::error, loc, "no overload of '" + std::string(fname) + "' matches this call (" +
             std::to_string(args.size()) + " argument(s))" + hint, declared);
    }

    // Host (C++) function surface: reject only certainties — arity misses across every
    // overload, or a known-primitive argument no builtin conversion reaches.
    void check_host_call(const std::string& fname, const std::vector<ctype>& args, const source_location& loc, const call_expr* c = nullptr) {
        std::vector<engine::host_overload_signature> sigs;
        if (!eng_.host_function_signatures(fname, sigs) || sigs.empty()) {
            return;
        }
        {
            std::vector<std::vector<param_type_info>> overloads;
            for (const auto& sig : sigs) { overloads.push_back(sig.param_types); }
            if (report_host_callback(fname, overloads, args, c)) { return; }
        }
        bool arity_ok = false;
        bool viable = false;
        for (const auto& sig : sigs) {
            if (sig.arity != SIZE_MAX && sig.arity != args.size()) { continue; }
            arity_ok = true;
            if (host_overload_viable(sig, args)) { viable = true; break; }
        }
        if (!arity_ok) {
            std::string arities;
            for (const auto& sig : sigs) {
                if (sig.arity == SIZE_MAX) { return; }   // variadic somewhere: anything goes
                if (!arities.empty()) { arities += " or "; }
                arities += std::to_string(sig.arity);
            }
            diag(diag_level::error, loc, "'" + fname + "' expects " + arities +
                 " argument(s), got " + std::to_string(args.size()));
            return;
        }
        if (!viable) {
            diag(diag_level::error, loc, "no overload of '" + fname + "' accepts these argument types");
        }
    }

    // parallel_reduce(xs, init, step, combine): a step or combine the program declares takes two parameters
    void check_reduce_callbacks(const call_expr* c) {
        static const char* const roles[] = {"step", "combine"};
        static const char* const shapes[] = {"(acc, x)", "(a, b)"};
        for (size_t k = 2; k < 4 && k < c->arguments.size(); ++k) {
            const expression* a = c->arguments[k].get();
            if (a->get_type() != node_type::identifier_expr) { continue; }
            const std::string_view fname = static_cast<const identifier_expr*>(a)->name;
            auto it = functions_.find(fname);
            if (it == functions_.end() || it->second.empty()) { continue; }
            bool takes_two = false;
            for (const auto* fn : it->second) { takes_two = takes_two || fn->parameters.size() == 2; }
            if (!takes_two) {
                diag(diag_level::error, a->location, "argument " + std::to_string(k + 1) + " of 'parallel_reduce': " + roles[k - 2] + " '" +
                     std::string(fname) + "' must take two parameters " + shapes[k - 2]);
            }
        }
    }

    bool host_overload_viable(const engine::host_overload_signature& sig, const std::vector<ctype>& args) {
        if (sig.param_types.empty()) { return true; }   // untyped legacy binding
        if (sig.param_types.size() != args.size()) { return true; }   // defensive: don't guess
        for (size_t i = 0; i < args.size(); ++i) {
            if (!host_param_accepts(args[i], sig.param_types[i]) || !host_callback_issue(args[i], sig.param_types[i]).empty()) {
                return false;
            }
        }
        return true;
    }

    // A host function's declared scalar result (every registration agrees): tentative, a diagnostics fact only
    ctype host_result_type(const std::string& name) {
        script_value_type k;
        if (!eng_.host_function_result(name, k)) { return {}; }
        ctype t;
        switch (k) {
            case script_value_type::jai_int_type: t = ctype::make(ctype::int_k); break;
            case script_value_type::jai_float_type: t = ctype::make(ctype::float_k); break;
            case script_value_type::jai_bool_type: t = ctype::make(ctype::bool_k); break;
            case script_value_type::jai_char_type: t = ctype::make(ctype::char_k); break;
            case script_value_type::jai_string_type: t = ctype::make(ctype::string_k); break;
            default: return {};
        }
        t.tentative = true;
        t.declared_result = true;
        return t;
    }

    static bool host_param_accepts(const ctype& arg, const param_type_info& p) {
        if (p.base_type == script_value_type::jai_null_type) { return true; }   // script_value wildcard
        if (arg.is_unknown() || arg.kind == ctype::null_k) { return true; }
        auto k = arg.kind;
        switch (p.base_type) {
            case script_value_type::jai_int_type:
            case script_value_type::jai_float_type:
                return k == ctype::int_k || k == ctype::float_k || k == ctype::bool_k || k == ctype::char_k;
            case script_value_type::jai_bool_type:
                return k == ctype::bool_k || k == ctype::int_k || k == ctype::float_k;
            case script_value_type::jai_char_type:
                return k == ctype::char_k || k == ctype::int_k || k == ctype::float_k || k == ctype::string_k;   // a string of one character
            case script_value_type::jai_string_type:
                return k == ctype::string_k || k == ctype::utf8_k;   // no builtin numeric->string conversion at the C++ boundary
            case script_value_type::jai_array_type:
                return k == ctype::array_k;
            case script_value_type::jai_map_type:
                return k == ctype::map_k;
            default:
                return true;   // objects/pointers: conversion registry territory, lenient
        }
    }

    // The runtime's aggregate rule (detail::collect_aggregate_fields): every class in the chain is a script class declaring
    // no constructor, no instance field is private or a reference; the instance fields in declaration order, bases first
    enum class aggregate_shape : uint8_t { aggregate, not_aggregate, unknown };
    aggregate_shape aggregate_fields(const sclass& c, std::vector<const variable_decl*>& out, int depth = 0) {
        if (depth > 64 || !c.decl) { return aggregate_shape::unknown; }
        if (!c.ctors.empty()) { return aggregate_shape::not_aggregate; }
        for (auto base : c.decl->base_classes) {
            const sclass* b = find_sclass(base);
            if (!b) {
                auto def = engine_class(base);
                return def && !def->is_script_class() ? aggregate_shape::not_aggregate : aggregate_shape::unknown;
            }
            const aggregate_shape shape = aggregate_fields(*b, out, depth + 1);
            if (shape != aggregate_shape::aggregate) { return shape; }
        }
        for (const auto& m : c.decl->members) {
            if (!m.declaration || m.declaration->get_type() != node_type::variable_decl) { continue; }
            auto* f = static_cast<const variable_decl*>(m.declaration.get());
            if (f->is_static) { continue; }
            if (c.nonpublic.count(f->name) || (f->type && f->type->base_type == script_value_type::jai_reference_type)) {
                return aggregate_shape::not_aggregate;
            }
            out.push_back(f);
        }
        return aggregate_shape::aggregate;
    }

    // The base an empty super() default-constructs (C++ implicit construction): a modeled script class whose constructors
    // all need arguments, reached through constructor-less script bases; empty when it can or a host class decides
    std::string_view base_without_default(std::string_view cls, int depth = 0) {
        if (depth > 32) { return {}; }
        const sclass* c = find_sclass(cls);
        if (!c || !c->decl) { return {}; }
        if (!c->ctors.empty()) {
            const std::vector<ctype> none;
            for (const auto* ctor : c->ctors) {
                if (script_fn_viable(ctor, none)) { return {}; }
            }
            return c->decl->name;
        }
        if (c->decl->base_classes.empty()) { return {}; }
        return base_without_default(c->decl->base_classes.front(), depth + 1);   // the base super() reaches
    }

    // cls default-constructs its bases (an empty super(), or the implicit default constructor)
    void report_base_without_default(std::string_view cls, const source_location& loc) {
        const sclass* c = find_sclass(cls);
        if (!c || !c->decl || c->decl->base_classes.empty()) { return; }
        if (auto missing = base_without_default(c->decl->base_classes.front()); !missing.empty()) {
            diag(diag_level::error, loc, "Base '" + std::string(missing) + "' has no default constructor; call super(...)");
        }
    }

    // A constructor's initializer list against the class's bases (C++ [class.base.init]): each name a base, named once; an unnamed
    // base default-constructs (the first one's implicit `: super()`)
    void check_base_initializers(const function_decl* fn) {
        const sclass* c = find_sclass(class_ctx_name_);
        if (!c || !c->decl) { return; }
        const auto& bases = c->decl->base_classes;
        std::vector<int> named(bases.size(), 0);
        bool delegates = false;
        using kind_t = constructor_initializer::kind_t;
        std::vector<const constructor_initializer*> fields_named;
        for (const auto& init : fn->initializers) {
            if (init.kind == kind_t::delegation) {
                delegates = true;
                if (fn->initializers.size() > 1) {
                    const constructor_initializer& other = &init == &fn->initializers.front() ? fn->initializers[1] : fn->initializers.front();
                    diag(diag_level::error, fn->location, format_error_message(detail::k_delegation_alone, class_ctx_name_, other.target));
                }
                continue;
            }
            if (init.kind == kind_t::field) {
                const bool twice = std::any_of(fields_named.begin(), fields_named.end(), [&](const constructor_initializer* f) { return f->target == init.target; });
                if (twice) { diag(diag_level::error, init.location, format_error_message(detail::k_field_named_twice, class_ctx_name_, init.target)); }
                else { fields_named.push_back(&init); }
                continue;
            }
            if (init.kind == kind_t::unresolved) {
                const bool is_static = c->static_fields.count(init.target) != 0;
                diag(diag_level::error, init.location, format_error_message(is_static ? detail::k_static_in_initializer_list : detail::k_not_a_base, class_ctx_name_, init.target));
                continue;
            }
            size_t index = init.target == "super" && !bases.empty() ? 0 : SIZE_MAX;
            for (size_t i = 0; index == SIZE_MAX && i < bases.size(); ++i) {
                if (detail::initializer_names(init.target, bases[i])) { index = i; }
            }
            if (index == SIZE_MAX) {
                if (init.target != "super") { diag(diag_level::error, init.location, format_error_message(detail::k_not_a_base, class_ctx_name_, init.target)); }
                continue;
            }
            if (++named[index] == 2) { diag(diag_level::error, fn->location, format_error_message(detail::k_base_named_twice, class_ctx_name_, bases[index])); }
        }
        if (delegates || bases.empty()) { return; }
        if (!named[0]) { report_base_without_default(class_ctx_name_, fn->location); }
        for (size_t i = 1; i < bases.size(); ++i) {
            if (!named[i] && !base_without_default(bases[i]).empty()) {
                diag(diag_level::error, fn->location, format_error_message(detail::k_base_needs_naming, class_ctx_name_, bases[i]));
            }
        }
    }

    // The implicit copy constructor may take a one-argument construction (detail::implicit_copy_construction): the argument is an
    // object of the class or of a class derived from it, a value or a handle, and no declared constructor is from the class itself
    bool implicit_copy_may_take(std::string_view cls_name, const sclass& c, const std::vector<ctype>& args) {
        if (c.ctors.empty() || args.size() != 1 || args[0].kind != ctype::object_k || class_relation(args[0].cls, cls_name) == 0) { return false; }
        for (const auto* ctor : c.ctors) {
            auto [lo, hi] = arity_range(ctor);
            if (ctor->parameters.empty() || lo > 1 || hi < 1) { continue; }
            const type_info* first = ctor->parameters[0].type.get();
            if (first && first->base_type == script_value_type::jai_reference_type) { first = first->type_params.empty() ? nullptr : first->type_params[0].get(); }
            if (first && first->base_type == script_value_type::jai_object_type && first->type_name == cls_name) { return false; }
        }
        return true;
    }

    // true: the implicit copy constructor may be the one that runs (which constructor does is the run's pick)
    // A constructor's member initializers (C++ [class.base.init]): each value stores into its field as `=` would; the fields
    // initialize in declaration order, so a list written in another order warns (as C++'s -Wreorder) and a value that reads a field
    // initialized after it is the field initializer's error
    void check_member_initializers(const function_decl* fn) {
        const sclass* c = class_ctx_;
        if (!c || !c->decl) { return; }
        std::vector<const variable_decl*> order;
        for (const auto& m : c->decl->members) {
            if (m.declaration && m.declaration->get_type() == node_type::variable_decl) {
                const auto* f = static_cast<const variable_decl*>(m.declaration.get());
                if (!f->is_static) { order.push_back(f); }
            }
        }
        const auto position = [&](std::string_view name) {
            for (size_t i = 0; i < order.size(); ++i) { if (order[i]->name == name) { return i; } }
            return order.size();
        };
        const constructor_initializer* previous = nullptr;
        bool warned = false;
        for (const auto& init : fn->initializers) {
            if (init.kind != constructor_initializer::kind_t::field || !init.value) { continue; }
            const size_t at = position(init.target);
            if (at == order.size()) { continue; }
            const variable_decl* field = order[at];
            initializing_field initializing{field->name, {}};
            initializing.unready = fields_from(c->decl, field);
            initializing_field_ = &initializing;
            const ctype value = infer_expecting(init.value.get(), field->auto_spelled ? nullptr : field->type.get());
            initializing_field_ = nullptr;
            if (field->type && !field->auto_spelled && field->type->base_type != script_value_type::jai_any_type) {
                check_store(value, false, token_type::equal, declared_variable_type(field), declared_variable_type_name(field),
                            "field '" + std::string(field->name) + "'", init.location, field->location);
            }
            if (previous && !warned && position(previous->target) > at) {
                diag(diag_level::warning, init.location, "member initializer of '" + std::string(field->name) + "' follows '" + previous->target +
                     "', but fields initialize in declaration order: '" + std::string(field->name) + "' first; write the list in that order");
                warned = true;
            }
            previous = &init;
        }
    }

    // a class value field with no initializer of its own that every constructor names in its list (or delegates to one that
    // does): its default construction never runs, so the class need not have a default constructor
    bool every_constructor_initializes(const sclass& c, std::string_view field) {
        if (c.ctors.empty()) { return false; }
        for (const auto* ctor : c.ctors) {
            const bool names = std::any_of(ctor->initializers.begin(), ctor->initializers.end(), [&](const constructor_initializer& init) {
                return init.kind == constructor_initializer::kind_t::delegation || (init.kind == constructor_initializer::kind_t::field && init.target == field);
            });
            if (!names) { return false; }
        }
        return true;
    }

    bool check_ctor_call(std::string_view cls_name, const sclass& c, const std::vector<ctype>& args,
                         const source_location& loc) {
        if (implicit_copy_may_take(cls_name, c, args)) { return true; }
        check_declared_ctor_call(cls_name, c, args, loc);
        return false;
    }

    void check_declared_ctor_call(std::string_view cls_name, const sclass& c, const std::vector<ctype>& args,
                                  const source_location& loc) {
        if (c.ctors.empty() && args.empty()) {
            report_base_without_default(cls_name, loc);
            for (size_t i = 1; c.decl && i < c.decl->base_classes.size(); ++i) {
                if (!base_without_default(c.decl->base_classes[i]).empty()) {
                    diag(diag_level::error, loc, format_error_message(detail::k_base_needs_naming, cls_name, c.decl->base_classes[i]));
                }
            }
        }
        if (c.ctors.empty()) {
            // implicit default + implicit copy, else aggregate initialization (detail/aggregate_init.hpp)
            bool copy_ok = args.size() == 1 &&
                (args[0].is_unknown() || (args[0].kind == ctype::object_k && class_relation(args[0].cls, cls_name) != 0));
            if (args.empty() || copy_ok) { return; }
            std::vector<const variable_decl*> fields;
            switch (aggregate_fields(c, fields)) {
                case aggregate_shape::unknown: return;
                case aggregate_shape::aggregate:
                    if (args.size() > fields.size()) {   // the runtime's text (detail::too_many_initializers)
                        diag(diag_level::error, loc, "too many initializers for '" + std::string(cls_name) + "': it has " +
                             std::to_string(fields.size()) + (fields.size() == 1 ? " field" : " fields") + ", got " + std::to_string(args.size()),
                             c.foreign ? declared_at{} : declared_at{c.decl->location});
                        return;
                    }
                    for (size_t i = 0; i < args.size(); ++i) {
                        const variable_decl* f = fields[i];
                        if (!f->type || f->type->base_type == script_value_type::jai_any_type) { continue; }
                        check_store(args[i], false, token_type::equal, ctype_from_type_info(f->type.get()), declared_variable_type_name(f),
                                    "field '" + std::string(f->name) + "'", loc, {});
                    }
                    return;
                case aggregate_shape::not_aggregate:
                    break;
            }
            {   // the runtime's text (detail::overload_miss)
                std::string list = "(";
                for (size_t i = 0; i < args.size() && i < 6; ++i) { if (i) { list += ", "; } list += ctype_name(args[i]); }
                if (args.size() > 6) { list += ", ..."; }
                diag(diag_level::error, loc, "no constructor of '" + std::string(cls_name) + "' takes " + list + ")",
                     c.foreign ? declared_at{} : declared_at{c.decl->location});
            }
            return;
        }
        size_t viable = 0;
        size_t viable_private = 0;
        for (const auto* ctor : c.ctors) {
            if (script_fn_viable(ctor, args, arity_rivals(c.ctors, args.size()))) {
                ++viable;
                if (std::find(c.private_ctors.begin(), c.private_ctors.end(), ctor) != c.private_ctors.end()) { ++viable_private; }
            }
        }
        if (viable > 1) { report_ambiguous_call(cls_name, c.ctors, args, loc); }
        if (viable) {
            // whichever overload the runtime picks is private: the raise detail::enforce_constructor_access gives
            const std::string_view ctx = class_ctx_ ? class_ctx_name_ : std::string_view{};
            if (viable_private == viable && (ctx.empty() || !class_is_or_derives(ctx, cls_name))) {
                diag(diag_level::error, loc, "Cannot access private constructor of class '" + std::string(cls_name) + "'");
            }
            return;
        }
        report_no_overload(cls_name, c.ctors, args, loc, c.foreign);
    }

    // -------------------------------------------------------------- flow narrowing
    //
    // A test of what a local holds (reflect::is_instance_of(x, "T"), a dynamic handle cast of x, reflect::type_of(x) == "int") gives the name that type where
    // the test is known to have held: a fact on narrow_facts_, read by infer_identifier (a reference's by referent_receiver).
    // narrow_end is everything that can give the variable another value. A fact changes no declaration and no run-time path:
    // the proofs a narrowed read feeds select lanes that keep their run-time guards.

    struct narrow_fact {
        const void* key = nullptr;   // var_entry::narrow_key
        ctype type;
        bool referent = false;       // a reference's: member diagnostics read it and the proof view stays unknown
        bool proof_only = false;     // a var's: the proof walk reads it, diagnostics keep the var (no load-time text changes)
        bool dead = false;
    };
    struct narrow_event { const void* key; bool escape; };
    enum class narrow_bound : uint8_t { alias, copy };
    struct narrow_frame { size_t base; narrow_bound kind; };   // a body whose names at a lower scope index belong to an enclosing frame
    std::vector<narrow_fact> narrow_facts_;
    std::vector<narrow_event> narrow_ended_;               // every end, in walk order: a region reads what ended since its mark
    std::unordered_set<const void*> narrow_escaped_;       // variables a reference may still reach
    std::vector<narrow_frame> narrow_frames_;
    std::unordered_set<const void*>* narrow_declared_ = nullptr;   // the declarations of a body being walked ahead
    bool narrow_dry_ = false;                              // a loop or lambda body walked ahead for what it ends: nothing narrows, nothing is reported
    bool narrow_in_list_ = false;                          // the statement about to be walked stands in a statement list
    bool narrow_derived_ = false;                          // the initializer or return value just inferred read a narrowed name
    size_t narrow_reads_ = 0;
    const ctype* narrow_observed_ = nullptr;               // a var's narrowing the diagnostics walk does not read: the observer still sees it

    // Program-scope and namespace variables are any function's to write
    void narrow_declare(var_entry& entry, const void* key) {
        if (scopes_.size() <= 1 || (!namespace_scopes_.empty() && namespace_scopes_.back() == scopes_.size() - 1)) { return; }
        entry.narrow_key = key;
        if (narrow_declared_) { narrow_declared_->insert(key); }
    }

    // Through a by-reference capture or a parallel body a name is the enclosing frame's storage, which that frame may write
    // between two of this body's steps; through by-value captures alone it is a snapshot taken where the lambda was made
    bool narrow_aliased(const var_entry& e, size_t scope_index) const {
        if (e.capture_alias) { return true; }
        return std::any_of(narrow_frames_.begin(), narrow_frames_.end(), [&](const narrow_frame& f) { return f.base > scope_index && f.kind == narrow_bound::alias; });
    }
    bool narrow_crosses(const var_entry& e, size_t scope_index) const {
        if (e.capture_alias || e.capture_copy) { return true; }
        for (const narrow_frame& f : narrow_frames_) { if (f.base > scope_index) { return true; } }
        return false;
    }

    void narrow_apply(const narrow_event& ev) {
        for (narrow_fact& f : narrow_facts_) { if (f.key == ev.key) { f.dead = true; } }
        if (ev.escape) { narrow_escaped_.insert(ev.key); }
    }
    // The variable may hold another value from here on. escape: something that can write it may outlive this point (a reference
    // a callee kept, a lambda that runs again), so it does not narrow again
    void narrow_end(const var_entry& e, size_t scope_index, bool escape) {
        if (!e.narrow_key) { return; }
        const narrow_event ev{e.narrow_key, escape || narrow_crosses(e, scope_index)};
        narrow_ended_.push_back(ev);
        if (!narrow_dry_) { narrow_apply(ev); }
    }
    // The variables an expression names as storage: a name, a cast of one, an arm of a conditional, an assignment's target
    template <class F>
    void narrow_each_root(const expression* e, F&& f) {
        if (!e) { return; }
        switch (e->get_type()) {
            case node_type::identifier_expr: {
                auto* id = static_cast<const identifier_expr*>(e);
                if (id->global_qualified) { return; }
                size_t scope = 0;
                if (const var_entry* v = find_local(id->name, &scope); v && v->narrow_key) { f(*v, scope); }
                return;
            }
            case node_type::cast_expr: {
                narrow_each_root(static_cast<const cast_expr*>(e)->operand.get(), f);
                return;
            }
            case node_type::ternary_expr:
                narrow_each_root(static_cast<const ternary_expr*>(e)->then_expression.get(), f);
                narrow_each_root(static_cast<const ternary_expr*>(e)->else_expression.get(), f);
                return;
            case node_type::assignment_expr:
                narrow_each_root(static_cast<const assignment_expr*>(e)->target.get(), f);
                return;
            default:
                return;
        }
    }
    void narrow_end_roots(const expression* e, bool escape) {
        narrow_each_root(e, [&](const var_entry& v, size_t scope) { narrow_end(v, scope, escape); });
    }

    const narrow_fact* narrow_live_fact(const var_entry& e, size_t scope_index) const {
        if (narrow_facts_.empty() || !e.narrow_key) { return nullptr; }
        for (auto it = narrow_facts_.rbegin(); it != narrow_facts_.rend(); ++it) {
            if (it->key != e.narrow_key || it->dead) { continue; }
            return narrow_aliased(e, scope_index) ? nullptr : &*it;
        }
        return nullptr;
    }
    const ctype* narrow_read(const var_entry& e, size_t scope_index, bool referent) {
        const narrow_fact* fact = narrow_live_fact(e, scope_index);
        if (!fact || fact->referent != referent) { return nullptr; }
        if (fact->proof_only && !semantic_walk()) {
            narrow_observed_ = &fact->type;
            return nullptr;
        }
        ++narrow_reads_;
        return &fact->type;
    }

    // A reference binds the variable, not the value it holds: a narrowed name binds as it is declared
    ctype narrow_declared_view(const ctype& t) {
        if (!t.narrowed || !t.origin || t.origin->get_type() != node_type::identifier_expr) { return t; }
        const var_entry* e = find_local(static_cast<const identifier_expr*>(t.origin)->name);
        if (!e) { return t; }
        ctype result = e->dynamic ? ctype{} : e->type;
        result.origin = t.origin;
        return result;
    }

    // A local, parameter or loop variable this body may narrow: its own, with no reference left that could write it
    const var_entry* narrow_target(const expression* operand, size_t& scope) {
        if (!operand || operand->get_type() != node_type::identifier_expr) { return nullptr; }
        auto* id = static_cast<const identifier_expr*>(operand);
        if (id->global_qualified || extended_names_.count(id->name)) { return nullptr; }
        const var_entry* e = find_local(id->name, &scope);
        if (!e || !e->narrow_key || narrow_escaped_.count(e->narrow_key) || narrow_aliased(*e, scope)) { return nullptr; }
        return e;
    }

    // What a passed class test tells of a variable, given what it is declared and already narrowed to. A declared class or handle
    // narrows to the tested class as if declared so; a var or a reference to one may hold more than the test named (open)
    bool narrow_class_fact(const var_entry& e, size_t scope, std::string_view cls, const std::vector<narrow_fact>& pending, narrow_fact& out) {
        if (!find_sclass(cls) && !engine_class(cls)) { return false; }
        // a reference, or an auto loop variable over class elements: its class is the referent member diagnostics read
        const bool reference = e.is_reference || (e.dynamic && e.referent.kind == ctype::object_k);
        if (!e.dynamic && e.type.kind != ctype::object_k) { return false; }
        const ctype* held = nullptr;   // the narrowing in force: one this condition already gathered, else the live one
        for (auto it = pending.rbegin(); it != pending.rend() && !held; ++it) { if (it->key == e.narrow_key) { held = &it->type; } }
        if (!held) { if (const narrow_fact* live = narrow_live_fact(e, scope)) { held = &live->type; } }
        if (held && held->kind != ctype::object_k) { return false; }
        const ctype& declared = reference ? e.referent : e.type;
        const bool declared_class = declared.kind == ctype::object_k && !declared.cls.empty() && (reference || !e.dynamic);
        bool open = held ? held->open : !declared_class;
        const std::string_view from = held ? held->cls : declared_class ? declared.cls : std::string_view{};
        if (!from.empty()) {
            if (from == cls) { return false; }
            if (class_relation(cls, from) != 1) {
                if (class_relation(from, cls) == 1) { return false; }   // tested for an ancestor: nothing new
                open = true;   // an object of both: the first class's members stay possible
            }
        }
        const type_info* object_type = eng_.get_type_info_object(std::string(cls));
        if (!object_type) { return false; }
        ctype t = ctype::object(object_type->type_name);
        t.narrowed = true;
        t.open = open;
        out = narrow_fact{};
        out.key = e.narrow_key;
        if (reference) {
            t.tentative = true;
            t.diagnostic_only = true;
            out.referent = true;
        } else if (e.dynamic) {
            t.tentative = true;   // a value or a handle, of this class or one derived from it
            out.proof_only = true;
        } else if (e.is_handle || (e.type.ti && e.type.ti->base_type == script_value_type::jai_shared_ptr_type)) {
            t.ti = eng_.get_type_info_shared_ptr(const_cast<type_info*>(object_type));
            t.tentative = e.type.tentative || open;
        } else {
            t.ti = object_type;
            t.tentative = e.type.tentative || open;
        }
        out.type = t;
        return true;
    }
    // reflect::type_of(x) == "int": a var holds exactly that kind
    bool narrow_kind_fact(const var_entry& e, ctype::kind_t kind, narrow_fact& out) {
        if (!e.dynamic || e.is_reference || e.alias.kind != ctype::unknown) { return false; }
        out = narrow_fact{};
        out.key = e.narrow_key;
        out.type = ctype::make(kind);
        out.type.narrowed = true;
        out.proof_only = true;
        return true;
    }
    static ctype::kind_t narrow_kind_name(std::string_view name) {
        return name == "int" ? ctype::int_k : name == "float" ? ctype::float_k : name == "bool" ? ctype::bool_k
             : name == "string" ? ctype::string_k : name == "char" ? ctype::char_k : ctype::unknown;
    }
    static bool narrow_text_literal(const expression* e, std::string& out) {
        if (!e || e->get_type() != node_type::literal_expr) { return false; }
        const script_value& v = static_cast<const literal_expr*>(e)->value;
        if (v.type() != script_value_type::jai_string_type) { return false; }
        out = std::string(v.unchecked_as_string());
        return true;
    }
    void narrow_named_test(const expression* operand, const std::string& name, bool kinds, std::vector<narrow_fact>& out) {
        size_t scope = 0;
        const var_entry* e = narrow_target(operand, scope);
        if (!e) { return; }
        narrow_fact f;
        const ctype::kind_t kind = kinds ? narrow_kind_name(name) : ctype::unknown;
        if (kind != ctype::unknown ? narrow_kind_fact(*e, kind, f) : narrow_class_fact(*e, scope, name, out, f)) { out.push_back(f); }
    }
    static bool narrow_same(const narrow_fact& a, const narrow_fact& b) {
        return a.key == b.key && a.referent == b.referent && a.type.kind == b.type.kind && a.type.cls == b.type.cls && a.type.ti == b.type.ti && a.type.open == b.type.open;
    }
    // The facts an expression's value implies when it is `sense`
    void narrow_collect(const expression* e, bool sense, std::vector<narrow_fact>& out) {
        if (!e) { return; }
        stack_.guard(e->location);
        switch (e->get_type()) {
            case node_type::unary_expr: {
                auto* u = static_cast<const unary_expr*>(e);
                if (u->op.type == token_type::bang) { narrow_collect(u->operand.get(), !sense, out); }
                return;
            }
            case node_type::binary_expr: {
                auto* b = static_cast<const binary_expr*>(e);
                const token_type op = b->op.type;
                if (op == token_type::ampersand_ampersand || op == token_type::pipe_pipe) {
                    if ((op == token_type::ampersand_ampersand) == sense) {   // both operands had that value
                        narrow_collect(b->left.get(), sense, out);
                        narrow_collect(b->right.get(), sense, out);
                        return;
                    }
                    std::vector<narrow_fact> l, r;   // either may have: what both tell
                    narrow_collect(b->left.get(), sense, l);
                    if (!l.empty()) { narrow_collect(b->right.get(), sense, r); }
                    for (const narrow_fact& a : l) {
                        if (std::any_of(r.begin(), r.end(), [&](const narrow_fact& c) { return narrow_same(a, c); })) { out.push_back(a); }
                    }
                    return;
                }
                if ((op != token_type::equal_equal && op != token_type::bang_equal) || (op == token_type::equal_equal) != sense) { return; }
                const expression* call = b->left.get();
                const expression* text = b->right.get();
                if (call && call->get_type() != node_type::call_expr) { std::swap(call, text); }
                std::string name;
                if (!call || call->get_type() != node_type::call_expr || !narrow_text_literal(text, name)) { return; }
                auto* c = static_cast<const call_expr*>(call);
                if (c->arguments.size() != 1 || !c->callee || c->callee->get_type() != node_type::member_expr) { return; }
                auto* callee = static_cast<const member_expr*>(c->callee.get());
                if (static_path(callee, "reflect") && callee->member == "type_of") { narrow_named_test(c->arguments[0].get(), name, /*kinds=*/true, out); }
                return;
            }
            case node_type::cast_expr: {
                auto* c = static_cast<const cast_expr*>(e);
                // dynamic_cast<T>(x) as the condition: x is a T where the handle was not null
                if (c->dynamic && !c->by_reference && sense && c->target && c->target->element_type()) {
                    narrow_named_test(c->operand.get(), c->target->element_type()->type_name, /*kinds=*/false, out);
                }
                return;
            }
            case node_type::call_expr: {
                auto* c = static_cast<const call_expr*>(e);
                std::string name;
                if (!sense || c->arguments.size() != 2 || !c->callee || c->callee->get_type() != node_type::member_expr) { return; }
                auto* m = static_cast<const member_expr*>(c->callee.get());
                if (static_path(m, "reflect") && m->member == "is_instance_of" && narrow_text_literal(c->arguments[1].get(), name)) {
                    narrow_named_test(c->arguments[0].get(), name, /*kinds=*/false, out);
                }
                return;
            }
            default:
                return;
        }
    }
    bool narrow_ended_since(size_t mark, const void* key) const {
        for (size_t i = mark; i < narrow_ended_.size(); ++i) { if (narrow_ended_[i].key == key) { return true; } }
        return false;
    }
    void narrow_drop_ended(std::vector<narrow_fact>& facts, size_t mark) {
        std::erase_if(facts, [&](const narrow_fact& f) { return narrow_escaped_.count(f.key) != 0 || narrow_ended_since(mark, f.key); });
    }
    // What a condition evaluated since `mark` tells where it was true and where it was false; a variable the condition itself
    // ended tells nothing
    void narrow_conclude(const expression* condition, size_t mark, std::vector<narrow_fact>& when_true, std::vector<narrow_fact>& when_false) {
        if (narrow_dry_ || !condition) { return; }
        narrow_collect(condition, true, when_true);
        narrow_collect(condition, false, when_false);
        narrow_drop_ended(when_true, mark);
        narrow_drop_ended(when_false, mark);
    }
    void narrow_push(const std::vector<narrow_fact>& facts) {
        if (narrow_dry_) { return; }
        for (const narrow_fact& f : facts) { if (!narrow_escaped_.count(f.key)) { narrow_facts_.push_back(f); } }
    }
    void narrow_pop(size_t held) {
        if (narrow_facts_.size() > held) { narrow_facts_.erase(narrow_facts_.begin() + static_cast<std::ptrdiff_t>(held), narrow_facts_.end()); }
    }
    bool narrow_live() const {
        return std::any_of(narrow_facts_.begin(), narrow_facts_.end(), [](const narrow_fact& f) { return !f.dead; });
    }
    static bool narrow_completes(const statement* s) {
        return !s || (return_flow().analyze(s).paths & (return_flow::normal | return_flow::maybe_normal)) != 0;
    }
    // a break that leaves this loop: its condition was not false on the way out
    bool narrow_breaks(const ast_node* n) {
        if (!n) { return false; }
        stack_.guard(n->location);
        switch (n->get_type()) {
            case node_type::break_stmt: return true;
            case node_type::while_stmt: case node_type::for_stmt: case node_type::range_for_stmt: case node_type::parallel_for_stmt:
            case node_type::function_decl: case node_type::class_decl: case node_type::namespace_decl:
                return false;
            default: break;
        }
        if (n->get_type() < node_type::expression_stmt) { return false; }
        bool found = false;
        lw_each_child(n, [&](const ast_node* k) { found = found || narrow_breaks(k); });
        return found;
    }
    // whether a body tests what a variable holds
    bool narrow_has_test(const ast_node* n) {
        if (!n) { return false; }
        stack_.guard(n->location);
        const node_type t = n->get_type();
        bool found = false;
        if (t < node_type::expression_stmt) {
            auto* e = static_cast<const expression*>(n);
            if (t == node_type::cast_expr && static_cast<const cast_expr*>(e)->dynamic) { return true; }
            if (t == node_type::call_expr) {
                const expression* callee = static_cast<const call_expr*>(e)->callee.get();
                if (callee && callee->get_type() == node_type::member_expr &&
                    (static_cast<const member_expr*>(callee)->member == "is_instance_of" || static_cast<const member_expr*>(callee)->member == "type_of")) { return true; }
            }
            if (t == node_type::lambda_expr && narrow_has_test(static_cast<const lambda_expr*>(e)->body.get())) { return true; }
            for_each_child_expression(e, [&](const expression* k) { found = found || narrow_has_test(k); });
            return found;
        }
        if (t == node_type::class_decl || t == node_type::namespace_decl) { return false; }
        if (t == node_type::try_stmt && !static_cast<const try_stmt*>(n)->clause_types.empty()) { return true; }
        lw_each_child(n, [&](const ast_node* k) { found = found || narrow_has_test(k); });
        return found;
    }
    bool narrow_wants_ahead(const ast_node* body) { return !narrow_dry_ && (narrow_live() || narrow_has_test(body)); }

    // A loop's body runs again after anything in it, and a lambda's body runs later and more than once: what the body ends for a
    // variable declared outside it is ended before the body is walked. The body is walked ahead with nothing narrowed, so every
    // call resolves by declared types alone, and nothing reported or stamped.
    template <class F>
    std::vector<narrow_event> narrow_walk_ahead(const void* own_variable, F&& walk) {
        std::vector<narrow_event> ended;
        std::unordered_set<const void*> declared;
        {
            const proof_mode proofs = std::exchange(proofs_, proof_mode::leave);
            const bool collecting = std::exchange(collect_diagnostics_, false);
            check_observer* const observer = std::exchange(observer_, nullptr);
            std::vector<narrow_fact> facts = std::exchange(narrow_facts_, {});
            std::vector<narrow_event> log = std::exchange(narrow_ended_, {});
            std::unordered_set<const void*>* const outer_declared = std::exchange(narrow_declared_, &declared);
            const mixed_ternary_note mixed = last_mixed_ternary_;
            std::optional<fn_ctx> function;
            if (!fn_stack_.empty()) { function = fn_stack_.back(); }
            const bool in_list = std::exchange(narrow_in_list_, false);
            const bool derived = narrow_derived_;
            const size_t reads = narrow_reads_;
            narrow_dry_ = true;
            scopes_.emplace_back();
            walk();
            scopes_.pop_back();
            narrow_dry_ = false;
            narrow_reads_ = reads;
            narrow_derived_ = derived;
            narrow_in_list_ = in_list;
            if (function) { fn_stack_.back() = std::move(*function); }
            last_mixed_ternary_ = mixed;
            narrow_declared_ = outer_declared;
            ended = std::exchange(narrow_ended_, std::move(log));
            narrow_facts_ = std::move(facts);
            observer_ = observer;
            collect_diagnostics_ = collecting;
            proofs_ = proofs;
        }
        std::erase_if(ended, [&](const narrow_event& ev) { return ev.key == own_variable || declared.count(ev.key) != 0; });
        for (const narrow_event& ev : ended) { narrow_apply(ev); }
        return ended;
    }

    // An argument that names a variable outright may bind to a reference the callee, or something the callee keeps, writes
    // through. A parameter declared by value or const&, a host parameter that writes nothing back and a library builtin take
    // the value; a callee the checker cannot see may take anything.
    bool narrow_binds_reference(const call_expr* c, size_t i, const std::vector<const function_decl*>* ov, const narrow_callee& seen) {
        if (seen.copies) { return false; }
        const auto by_reference = [i](const function_decl* fn) {
            return fn && !fn->is_coroutine && i < fn->parameters.size() && fn->parameters[i].is_reference && !fn->parameters[i].is_const;
        };
        if (ov) {
            if (std::any_of(ov->begin(), ov->end(), by_reference)) { return true; }
            if (ov_class_.empty() || !c->callee) { return false; }
            const std::string_view name = c->callee->get_type() == node_type::member_expr ? std::string_view(static_cast<const member_expr*>(c->callee.get())->member)
                                        : c->callee->get_type() == node_type::identifier_expr ? std::string_view(static_cast<const identifier_expr*>(c->callee.get())->name)
                                        : std::string_view{};
            const auto overrides = overrides_of(ov_class_, name);
            return std::any_of(overrides.begin(), overrides.end(), by_reference);
        }
        if (seen.resolved) {
            return std::any_of(seen.declarations.begin(), seen.declarations.end(), by_reference) || i >= 64 || ((seen.writeback >> i) & 1) != 0;
        }
        if (seen.host.empty()) { return true; }
        for (const auto& held : eng_.held_function_declarations(seen.host)) {
            if (!held) { return true; }
            const auto& parameters = held->parameters();
            if (i < parameters.size() && parameters[i].is_reference && !parameters[i].is_const) { return true; }
        }
        return i >= 64 || ((eng_.host_function_writeback(std::string(seen.host)) >> i) & 1) != 0;
    }
    // A call may write the variable a reference aliases (a global, a field, a container element): a reference's narrowing ends
    void narrow_end_references() {
        for (narrow_fact& f : narrow_facts_) {
            if (!f.referent || f.dead) { continue; }
            narrow_ended_.push_back({f.key, false});
            if (!narrow_dry_) { f.dead = true; }
        }
    }
    void narrow_call_arguments(const call_expr* c, const std::vector<const function_decl*>* ov, const narrow_callee& seen) {
        if (!seen.copies) { narrow_end_references(); }
        for (size_t i = 0; i < c->arguments.size(); ++i) {
            int binds = -1;
            narrow_each_root(c->arguments[i].get(), [&](const var_entry& v, size_t scope) {
                if (binds < 0) { binds = narrow_binds_reference(c, i, ov, seen) ? 1 : 0; }
                if (binds == 1) { narrow_end(v, scope, /*escape=*/true); }
            });
        }
    }

    // -------------------------------------------------------------- expressions

    // The declared type the next expression inferred initializes (a declaration's, a parameter's, a slot's): a lambda literal
    // takes its untyped parameters and its result from a signature there, and an array or map literal hands its element type on
    const type_info* expected_ = nullptr;
    ctype infer_expecting(expression* e, const type_info* expected) {
        expected_ = expected;
        return infer(e);
    }

    JAI_NOINLINE ctype infer(expression* e) {
        if (e) { stack_.guard(e->location); }
        narrow_observed_ = nullptr;
        const type_info* expected = std::exchange(expected_, nullptr);
        ctype result = infer_impl(e, expected);
        result.origin = e;
        if (observer_ && e) { observe(e, narrow_observed_ && e->get_type() == node_type::identifier_expr ? *narrow_observed_ : result); }
        narrow_observed_ = nullptr;
        if (e && lw_on()) { lw_types_[e] = result; }
        return result;
    }

    JAI_NOINLINE void observe(const expression* e, const ctype& t) {
        static_assert(static_cast<uint8_t>(check_expression_type::kind_t::utf8_k) == ctype::utf8_k);
        check_expression_type view;
        view.kind = static_cast<check_expression_type::kind_t>(t.kind);
        view.cls = t.cls;
        view.ti = t.ti;
        view.tentative = t.tentative;
        view.narrowed = t.narrowed;
        view.name = ctype_name(t);
        observer_->expression_type(e, view);
    }

    JAI_NOINLINE ctype infer_impl(expression* e, const type_info* expected) {
        if (!e) { return {}; }
        switch (e->get_type()) {
            case node_type::literal_expr: {
                auto* lit = static_cast<literal_expr*>(e);
                switch (lit->value.type()) {
                    case script_value_type::jai_null_type: return ctype::make(ctype::null_k);
                    case script_value_type::jai_int_type: return ctype::make(ctype::int_k);
                    case script_value_type::jai_float_type: return ctype::make(ctype::float_k);
                    case script_value_type::jai_string_type: return ctype::make(ctype::string_k);
                    case script_value_type::jai_char_type: return ctype::make(ctype::char_k);
                    case script_value_type::jai_bool_type: return ctype::make(ctype::bool_k);
                    default: return {};
                }
            }
            case node_type::identifier_expr:
                return infer_identifier(static_cast<identifier_expr*>(e));
            case node_type::binary_expr:
                return infer_binary(static_cast<binary_expr*>(e));
            case node_type::unary_expr:
                return infer_unary(static_cast<unary_expr*>(e));
            case node_type::assignment_expr:
                return infer_assignment(static_cast<assignment_expr*>(e));
            case node_type::call_expr:
                return infer_call(static_cast<call_expr*>(e));
            case node_type::member_expr:
                return infer_member_expression(static_cast<member_expr*>(e));
            case node_type::lambda_expr: {
                auto* l = static_cast<lambda_expr*>(e);
                const lambda_context context = open_lambda_context(l, expected);
                walk_lambda(l);
                close_lambda_context(l, context);
                return ctype::make(ctype::function_k);
            }
            case node_type::new_expr:
                return infer_new(static_cast<new_expr*>(e));
            case node_type::include_expr: {
                auto* inc = static_cast<include_expr*>(e);
                if (inc->path_expr) { infer(inc->path_expr.get()); }
                return ctype::make(ctype::unknown);   // include's value is the file's result
            }
            case node_type::import_expr:
                return infer_import(static_cast<import_expr*>(e));
            case node_type::ternary_expr:
                return infer_ternary(static_cast<ternary_expr*>(e), expected);
            case node_type::array_literal_expr:
                return infer_array_literal(static_cast<array_literal_expr*>(e), expected);
            case node_type::map_literal_expr:
                return infer_map_literal(static_cast<map_literal_expr*>(e), expected);
            case node_type::this_expr:
                return infer_this(e);
            case node_type::super_expr:
                if (class_ctx_ && !class_ctx_->decl->base_classes.empty()) {
                    return ctype::object(class_ctx_->decl->base_classes[0]);
                }
                return {};
            case node_type::throw_expr: {
                auto* t = static_cast<throw_expr*>(e);
                if (t->value) { infer(t->value.get()); }
                return {};
            }
            case node_type::cast_expr:
                return infer_cast(static_cast<cast_expr*>(e));
            case node_type::yield_expr: {
                auto* y = static_cast<yield_expr*>(e);
                if (y->value) { check_coroutine_value(infer(y->value.get()), y->location, "yield"); }
                return {};
            }
            default:
                return {};
        }
    }

    JAI_NOINLINE ctype infer_binary(binary_expr* b) {
        if (b->op.type == token_type::greater && value_as_type_argument(b)) { return {}; }
        const size_t mark = narrow_ended_.size();
        ctype l = infer(b->left.get());
        const size_t held = narrow_facts_.size();
        if ((b->op.type == token_type::ampersand_ampersand || b->op.type == token_type::pipe_pipe) && !narrow_dry_) {
            std::vector<narrow_fact> decided;   // the right operand runs only where the left was true (&&) or false (||)
            narrow_collect(b->left.get(), b->op.type == token_type::ampersand_ampersand, decided);
            narrow_drop_ended(decided, mark);
            narrow_push(decided);
        }
        ctype r = infer(b->right.get());
        narrow_pop(held);
        return binary_type(b, l, r);
    }

    // Box<3> x; reads as (Box < 3) > x, where a generic class's bare name is an error anyway: its one error says a type
    // argument is a type
    JAI_NOINLINE bool value_as_type_argument(const binary_expr* b) {
        if (!generics_live_ || !b->left || b->left->get_type() != node_type::binary_expr) { return false; }
        const auto* less = static_cast<const binary_expr*>(b->left.get());
        if (less->op.type != token_type::less || !less->left || !less->right || less->right->get_type() != node_type::literal_expr) { return false; }
        std::string name;
        if (less->left->get_type() == node_type::identifier_expr) {
            const auto* id = static_cast<const identifier_expr*>(less->left.get());
            if (find_local(id->name)) { return false; }
            name = std::string(id->name);
        } else if (less->left->get_type() == node_type::member_expr && static_cast<const member_expr*>(less->left.get())->is_static) {
            name = qualified_spelling(static_cast<const member_expr*>(less->left.get()));
        }
        const generic_definition_ptr g = name.empty() ? nullptr : generic_class_named(name);
        if (!g) { return false; }
        diag(diag_level::error, less->right->location,
             "a type argument names a type: a value is not one an instance can be made with; '" + name + "' takes types (" + generic_spelling(*g) + ")");
        return true;
    }

    JAI_NOINLINE ctype binary_type(binary_expr* b, ctype& l, ctype& r) {
        if (b->op.type == token_type::left_bracket) {
            function_subscripted(b, l);
            // the proven element kind rides the node's class (int_int / float_float) for the compiler's temps
            const ctype cont = declared_container_receiver(b->left.get(), l);
            const ctype el = container_value_type(cont);
            const bool object_elements = el.kind == ctype::object_k && cont.kind == ctype::array_k && (!cont.tentative || cont.alias_proof);
            const bool vec_elements = el.kind == ctype::math_k && cont.kind == ctype::array_k && (!cont.tentative || cont.alias_proof);
            const uint8_t cls = object_elements ? operand_class_object_element
                              : vec_elements ? operand_class_vec_element
                              : !el.element_proof ? operand_class_unknown
                              : el.kind == ctype::int_k ? operand_class_int_int
                              : el.kind == ctype::float_k ? operand_class_float_float : operand_class_unknown;
            stamp(b->static_operand_class, cls, operand_class_unknown);
            if (cont.kind == ctype::map_k && !cont.tentative && r.kind == ctype::object_k && !r.tentative && !r.cls.empty()) { warn_identity_map_key(b, r.cls); }
            check_map_subscript_key(b, cont, r);
            const uint8_t receiver = cont.tentative ? 0 : cont.kind == ctype::string_k ? 1 : cont.kind == ctype::array_k ? 2 : cont.kind == ctype::map_k ? 3 : 0;
            stamp(b->subscript_receiver, receiver, uint8_t(0));
            return el;
        }
        if (b->op.type == token_type::question_question) {
            // the left value unless it is null, else the right: one type when both sides have it, the right's when the left is null
            if (l.kind == ctype::null_k) { return r; }
            if (l.kind == r.kind && l.cls == r.cls && !l.is_unknown()) {
                if (l.ti != r.ti) { l.ti = nullptr; }
                l.tentative = l.tentative || r.tentative;
                return l;
            }
            return {};
        }
        stamp(b->static_operand_class, operand_class_of(l, r), operand_class_unknown);   // annotation only
        check_comparison(b, l, r);
        check_builtin_binary(b, l, r);
        if (l.kind == ctype::object_k) {
            if (auto user = class_binary_operator(l.cls, b->op.type, r)) { return *user; }
        }
        if (r.kind == ctype::object_k) { warn_missing_reflected(b, l, r); }
        if (b->op.type == token_type::plus && !b->op.joins_text) { check_string_plus(b, l, r); }
        return binary_result(l, b->op.type, r);
    }

    // The builtin unary - + ~ over an operand no row takes and no operator of its class answers: the run time's text, at load
    void check_unary_operand(const unary_expr* u, const ctype& operand) {
        const token_type op = u->op.type;
        if (operand.tentative || operand.diagnostic_only || (op != token_type::minus && op != token_type::plus && op != token_type::tilde)) { return; }
        bool refused = false;
        switch (operand.kind) {
            case ctype::bool_k: case ctype::string_k: case ctype::utf8_k: case ctype::null_k:
            case ctype::array_k: case ctype::map_k: case ctype::function_k: refused = true; break;
            case ctype::float_k: refused = op == token_type::tilde; break;
            case ctype::math_k: refused = op == token_type::tilde; break;
            case ctype::object_k: {
                if (operand.cls.empty() || (operand.ti && operand.ti->base_type == script_value_type::jai_shared_ptr_type)) { break; }
                const std::string_view name = op == token_type::minus ? "-" : op == token_type::plus ? "+" : "~";
                const member_hit hit = lookup_member(operand.cls, name, /*want_static=*/false);
                refused = !hit.found && hit.certain && !extended_members_.count(name) && !chain_open(operand.cls);
                break;
            }
            default: break;
        }
        if (!refused) { return; }
        diag(diag_level::error, u->location, op == token_type::minus ? "Unary minus requires numeric operand"
                                           : op == token_type::plus ? "Unary plus requires numeric operand" : "Bitwise NOT requires integer operand");
    }

    // a string's + joins text (detail/string_join.hpp, the run-time text): a number, bool or null beside it is the load error
    void check_string_plus(const binary_expr* b, const ctype& l, const ctype& r) {
        const auto text = [](const ctype& t) { return !t.tentative && (t.kind == ctype::string_k || t.kind == ctype::utf8_k); };
        const auto scalar = [](const ctype& t) {
            return !t.tentative && (t.kind == ctype::int_k || t.kind == ctype::float_k || t.kind == ctype::bool_k || t.kind == ctype::null_k);
        };
        if (!(text(l) && scalar(r)) && !(scalar(l) && text(r))) { return; }
        if (eng_.has_function("+")) { return; }   // a registered global operator answers every +
        const auto name = [](const ctype& t) { return t.kind == ctype::utf8_k ? std::string("string") : ctype_name(t); };
        diag(diag_level::error, b->location, "cannot apply '+' to '" + name(l) + "' and '" + name(r) + "': stringify it with to_string(x) or `${x}`");
    }

    // A class whose whole chain this unit models as script, none of it open: no host operator, reflected binding or wrapper answers for it
    bool script_chain_closed(std::string_view cls, int depth = 0) {
        const sclass* c = depth > 32 ? nullptr : find_sclass(cls);
        if (!c || open_classes_.count(cls)) { return false; }
        for (auto base : c->decl->base_classes) {
            if (!script_chain_closed(base, depth + 1)) { return false; }
        }
        return true;
    }

    // An operand whose run-time kind the checker knows whole; var, math values, handles and host or open classes stay with the run time
    bool settled_operand(const ctype& t) {
        if (t.tentative || t.diagnostic_only) { return false; }
        switch (t.kind) {
            case ctype::int_k: case ctype::float_k: case ctype::char_k: case ctype::bool_k: case ctype::string_k: case ctype::utf8_k:
            case ctype::null_k: case ctype::array_k: case ctype::map_k: case ctype::function_k: return true;
            case ctype::object_k:
                return !t.cls.empty() && !(t.ti && t.ti->base_type == script_value_type::jai_shared_ptr_type) && script_chain_closed(t.cls);
            default: return false;
        }
    }

    // The builtin binary rows (detail/binary_handlers.hpp): + - * / % take numbers (a char promotes) and + a string on either side;
    // & | ^ << >> take ints and chars; past them only the left operand's class operator answers. A pair nothing takes gets the
    // operator's spelling and the run time's text; op stays null when something may take it
    struct binary_refusal { const char* op = nullptr; const char* text = nullptr; };
    binary_refusal builtin_binary_refusal(const ctype& l, token_type op, const ctype& r) {
        binary_refusal result;
        bool bitwise = false;
        switch (op) {
            case token_type::plus: result = {"+", "Invalid operands for + operator"}; break;
            case token_type::minus: result = {"-", "Invalid operands for - operator"}; break;
            case token_type::star: result = {"*", "Invalid operands for * operator"}; break;
            case token_type::slash: result = {"/", "Invalid operands for / operator"}; break;
            case token_type::percent: result = {"%", "Invalid operands for % operator"}; break;
            case token_type::ampersand: result = {"&", "Bitwise & requires integer operands"}; bitwise = true; break;
            case token_type::pipe: result = {"|", "Bitwise | requires integer operands"}; bitwise = true; break;
            case token_type::caret: result = {"^", "Bitwise ^ requires integer operands"}; bitwise = true; break;
            case token_type::left_shift: result = {"<<", "Left shift requires integer operands"}; bitwise = true; break;
            case token_type::right_shift: result = {">>", "Right shift requires integer operands"}; bitwise = true; break;
            default: return {};
        }
        if (!settled_operand(l) || !settled_operand(r) || eng_.has_function(result.op)) { return {}; }
        if (l.kind == ctype::object_k) {
            const member_hit hit = lookup_member(l.cls, result.op, /*want_static=*/false);
            if (hit.found || !hit.certain || extended_members_.count(result.op)) { return {}; }
        }
        const auto text_kind = [](const ctype& t) { return t.kind == ctype::string_k || t.kind == ctype::utf8_k; };
        const auto number = [bitwise](const ctype& t) { return t.kind == ctype::int_k || t.kind == ctype::char_k || (!bitwise && t.kind == ctype::float_k); };
        if (!bitwise && op == token_type::plus && (text_kind(l) || text_kind(r))) { return {}; }
        if (number(l) && number(r)) { return {}; }
        return result;
    }

    // spelled is the operator as written (a compound's "*="); a compound's own run-time text names it, so only binary carries the text
    std::string builtin_binary_message(const std::string& spelled, const binary_refusal& refusal, const ctype& l, const ctype& r, bool compound) {
        std::string result = "cannot apply '" + spelled + "' to '" + ctype_name(l) + "' and '" + ctype_name(r) + "'";
        if (l.kind == ctype::object_k) { result += ": class '" + std::string(l.cls) + "' declares no operator '" + refusal.op + "'"; }
        if (!compound) { result += std::string(" (it raises '") + refusal.text + "')"; }
        if (l.kind == ctype::object_k) {
            result += "; declare the operator in the class or convert the operand";
        } else if (r.kind == ctype::object_k && lookup_member(r.cls, refusal.op, /*want_static=*/false).found) {
            result += "; a class's operator runs only with an instance of it on the left";
        } else if (!compound) {
            result += "; convert one operand";
        }
        return result;
    }

    void check_builtin_binary(const binary_expr* b, const ctype& l, const ctype& r) {
        if (const binary_refusal refusal = builtin_binary_refusal(l, b->op.type, r); refusal.op) {
            diag(diag_level::error, b->location, builtin_binary_message(refusal.op, refusal, l, r, /*compound=*/false));
        }
    }

    // The class of an object target declares an in-place operator (op: "+=", "++", ...) that may take the operation, as
    // detail::object_operator_in_place asks: a compound one taking rhs, ++/-- in the form written (prefix none, postfix the int
    // marker); an uncertain member answers yes, a script class's overloads none of which takes it no
    bool in_place_operator_may_take(const ctype& t, std::string_view op, const ctype* rhs, bool postfix) {
        if (t.kind != ctype::object_k || t.cls.empty()) { return false; }
        const member_hit hit = lookup_member(t.cls, op, /*want_static=*/false);
        if (!hit.certain || extended_members_.count(std::string(op))) { return true; }
        if (!hit.found) { return false; }
        if (!hit.methods) { return true; }
        for (const auto* fn : *hit.methods) {
            auto [lo, hi] = arity_range(fn);
            if (rhs) {
                if (lo <= 1 && hi >= 1 && script_fn_viable(fn, std::vector<ctype>{*rhs})) { return true; }
            } else if (postfix ? (lo <= 1 && hi >= 1) : lo == 0) {
                return true;
            }
        }
        return false;
    }

    // ++ and -- take numbers and chars, and an object whose class declares the operator in the form written
    bool incdec_refused(const ctype& t, const char* op = nullptr, bool postfix = false) {
        if (op && in_place_operator_may_take(t, op, nullptr, postfix)) { return false; }
        return settled_operand(t) && t.kind != ctype::int_k && t.kind != ctype::float_k && t.kind != ctype::char_k;
    }

    // x op= rhs runs the class's operator op= when it declares one taking rhs, else x op rhs: the binary rows above, then the store;
    // the diagnostic names the compound spelling
    bool check_compound_operands(const ctype& target, token_type base_op, const ctype& rhs, const source_location& loc, const declared_at& declared) {
        if (incdec_op_) {
            if (!incdec_refused(target, incdec_op_, incdec_postfix_)) { return false; }
            diag(diag_level::error, loc, std::string("cannot apply '") + incdec_op_ + "' to '" + ctype_name(target) + "'", declared);
            return true;
        }
        const binary_refusal refusal = builtin_binary_refusal(target, base_op, rhs);
        if (!refusal.op) { return false; }
        diag(diag_level::error, loc, builtin_binary_message(std::string(refusal.op) + "=", refusal, target, rhs, /*compound=*/true), declared);
        return true;
    }

    JAI_NOINLINE ctype infer_unary(unary_expr* u) {
        ctype operand = infer(u->operand.get());
        if (operand.kind == ctype::object_k && (u->op.type == token_type::minus || u->op.type == token_type::plus ||
                                                u->op.type == token_type::tilde)) {
            if (auto user = class_unary_operator(operand.cls, u->op.type)) { return *user; }
        }
        check_unary_operand(u, operand);
        switch (u->op.type) {
            case token_type::bang: return ctype::make(ctype::bool_k);
            case token_type::minus:
            case token_type::plus:
                return is_numeric(operand.kind) || (operand.kind == ctype::math_k && u->op.type == token_type::plus) ? plain_int(operand) : ctype{};
            case token_type::plus_plus:
            case token_type::minus_minus:
                narrow_end_roots(u->operand.get(), /*escape=*/false);
                check_incdec_target(u);
                if (in_place_operator_may_take(operand, u->op.type == token_type::plus_plus ? "++" : "--", nullptr, u->is_postfix)) { return {}; }
                if (incdec_refused(operand) || (enum_value(operand) && !operand.tentative)) {
                    diag(diag_level::error, u->location, std::string("cannot apply '") + (u->op.type == token_type::plus_plus ? "++" : "--") + "' to '" + ctype_name(operand) + "'");
                } else if (operand.kind == ctype::char_k && !operand.tentative && !operand.diagnostic_only && u->operand->get_type() == node_type::identifier_expr) {
                    // a char promotes, so the int it steps to reaches a slot declared char (a non-name target checks as its += 1 store)
                    auto* id = static_cast<identifier_expr*>(u->operand.get());
                    const bool field = class_ctx_ && !id->global_qualified && !find_named(id) && lookup_member(class_ctx_name_, id->name, /*want_static=*/false).field;
                    diag(diag_level::error, u->location, "cannot assign 'int' to " + std::string(field ? "field '" : "'") + std::string(id->name) + "' declared 'char'");
                }
                return is_numeric(operand.kind) ? plain_int(operand) : ctype{};
            case token_type::tilde:
                return operand.kind == ctype::int_k ? plain_int(operand) : ctype{};
            default: return {};
        }
    }

    JAI_NOINLINE ctype infer_member_expression(member_expr* m) {
        ctype t = infer_member(m, /*writing=*/false);
        if (!map_key_operand(m).empty()) { t.const_key = true; }
        return t;
    }

    JAI_NOINLINE ctype infer_new(new_expr* n) {
        std::vector<ctype> args;
        for (auto& a : n->arguments) { args.push_back(infer(a.get())); }
        ctype t = ctype_from_type_info(n->type.get());
        if (t.kind == ctype::object_k && refuse_bare_generic_class(t.cls, n->location)) { return t; }
        report_unbound_instantiation(n->type.get(), n->location);
        const sclass* made = t.kind == ctype::object_k ? find_sclass(t.cls) : nullptr;
        narrow_end_references();   // a constructor runs code
        for (size_t i = 0; i < n->arguments.size(); ++i) {   // a constructor's reference parameter, or a host constructor's
            const bool binds = !made || std::any_of(made->ctors.begin(), made->ctors.end(), [i](const function_decl* fn) {
                return i < fn->parameters.size() && fn->parameters[i].is_reference && !fn->parameters[i].is_const;
            });
            if (binds) { narrow_end_roots(n->arguments[i].get(), /*escape=*/true); }
        }
        if (t.kind == ctype::object_k) {
            if (n->type && n->type->base_type == script_value_type::jai_shared_ptr_type) { note_handle_held(t.cls, n->location); }
            if (made) {
                check_ctor_call(t.cls, *made, args, n->location);
            } else {
                check_strict_host_constructor(std::string(t.cls), args, n->location);
            }
        }
        return t;
    }

    JAI_NOINLINE ctype infer_import(import_expr* e) {
        for (const auto& a : static_cast<import_expr*>(e)->path_exprs) {
            const ctype t = infer(a.get());
            if (!t.is_unknown() && t.kind != ctype::string_k && t.kind != ctype::utf8_k && t.kind != ctype::array_k) {
                diag(diag_level::error, a->location, "an import path is a string or an array of strings, got '" + ctype_name(t) + "'");
            }
        }
        return ctype::make(ctype::bool_k);   // true when a module (re)ran
    }

    JAI_NOINLINE ctype infer_ternary(ternary_expr* t, const type_info* expected) {
        const size_t mark = narrow_ended_.size();
        const expression* outer_chain_test = std::exchange(null_chain_test_, t->null_chain ? t->condition.get() : nullptr);
        infer(t->condition.get());
        null_chain_test_ = outer_chain_test;
        std::vector<narrow_fact> when_true, when_false;
        narrow_conclude(t->condition.get(), mark, when_true, when_false);
        const size_t held = narrow_facts_.size();
        narrow_push(when_true);
        ctype a = infer_expecting(t->then_expression.get(), expected);
        narrow_pop(held);
        narrow_push(when_false);
        ctype b = infer_expecting(t->else_expression.get(), expected);
        narrow_pop(held);
        if (a.kind == b.kind && a.cls == b.cls) {
            if (!mixed_ternaries_.empty()) { mixed_ternaries_.erase(t); }   // a walk ahead judged the arms by their declared types
            if (a.ti != b.ti) { a.ti = nullptr; }   // two spellings (value vs handle) are not one certain type
            a.tentative = a.tentative || b.tentative;
            a.const_key = a.const_key || b.const_key;
            a.narrowed = a.narrowed && b.narrowed;
            a.open = a.open || b.open;
            return a;
        }
        if (!a.is_unknown() && !b.is_unknown() && a.kind != ctype::null_k && b.kind != ctype::null_k) {   // a T-or-null arm pair is a nullable value, never a type mix
            last_mixed_ternary_ = {t, a, b};
            mixed_ternaries_[t] = last_mixed_ternary_;
            if ((a.kind == ctype::int_k && b.kind == ctype::float_k) || (a.kind == ctype::float_k && b.kind == ctype::int_k)) {
                // C++'s common type, for diagnostics alone: the chosen arm keeps its own kind at run time, so it proves nothing
                ctype common = ctype::make(ctype::float_k);
                common.tentative = true;
                return common;
            }
        }
        ctype mixed{};
        mixed.const_key = a.const_key || b.const_key;
        return mixed;
    }

    JAI_NOINLINE ctype infer_array_literal(array_literal_expr* a, const type_info* expected) {
        const type_info* element = expected && expected->base_type == script_value_type::jai_array_type ? expected->element_type().get() : nullptr;
        for (auto& el : a->elements) { literal_element_types_[el.get()] = infer_expecting(el.get(), element); }
        return ctype::make(ctype::array_k);
    }

    JAI_NOINLINE ctype infer_map_literal(map_literal_expr* m, const type_info* expected) {
        const type_info* value = expected && expected->base_type == script_value_type::jai_map_type ? expected->value_type().get() : nullptr;
        for (auto& [k, v] : m->entries) {
            literal_element_types_[k.get()] = infer(k.get());
            literal_element_types_[v.get()] = infer_expecting(v.get(), value);
        }
        return ctype::make(ctype::map_k);
    }

    // an expression whose checking takes the slot's declared type (infer_expecting)
    static bool contextual_literal(const expression* e) {
        if (!e) { return false; }
        const node_type k = e->get_type();
        return k == node_type::lambda_expr || k == node_type::array_literal_expr || k == node_type::map_literal_expr || k == node_type::ternary_expr;
    }

    // The static type of a name, `this` or a field of either, read without walking it (no diagnostic, no stamp)
    ctype peek_type(const expression* e) {
        if (!e) { return {}; }
        if (e->get_type() == node_type::this_expr) { return class_ctx_ ? ctype::object(class_ctx_name_) : ctype{}; }
        if (e->get_type() == node_type::identifier_expr) {
            const auto* id = static_cast<const identifier_expr*>(e);
            if (const var_entry* local = find_local(id->name)) { return local->dynamic ? ctype{} : local->type; }
            if (class_ctx_ && !id->global_qualified) {
                const member_hit hit = lookup_member(class_ctx_name_, id->name, false);
                if (hit.found) { return hit.field && !hit.methods && hit.field->type ? member_type(hit) : ctype{}; }
            }
            if (find_sclass(id->name)) { return ctype{ctype::class_ref_k, id->name}; }
            return {};
        }
        if (e->get_type() == node_type::member_expr) {
            const auto* m = static_cast<const member_expr*>(e);
            if (m->is_static) { return {}; }
            const ctype recv = peek_type(m->object.get());
            if (recv.kind != ctype::object_k || recv.cls.empty()) { return {}; }
            const member_hit hit = lookup_member(recv.cls, m->member, false);
            return hit.found && hit.field && !hit.methods && hit.field->type ? member_type(hit) : ctype{};
        }
        return {};
    }

    // The declared type of the storage an assignment's target names, where the load sees it: a variable, a field, an element
    const type_info* peek_declared_type(const expression* e) {
        if (!e) { return nullptr; }
        if (e->get_type() == node_type::binary_expr) {
            const auto* b = static_cast<const binary_expr*>(e);
            if (b->op.type != token_type::left_bracket) { return nullptr; }
            const type_info* container = peek_declared_type(b->left.get());
            if (container && container->base_type == script_value_type::jai_array_type) { return container->element_type().get(); }
            if (container && container->base_type == script_value_type::jai_map_type) { return container->value_type().get(); }
            return nullptr;
        }
        const type_info* declared = peek_type(e).ti;
        return declared && declared->base_type == script_value_type::jai_reference_type ? declared->element_type().get() : declared;
    }

    // A C++ parameter as a script type: the scalars, a registered class (by value or shared_ptr), a math kind, a std::function's
    // signature; null (var) for a script_value and for what the binding does not name
    type_info* host_parameter_type(const param_type_info& p) {
        using k = script_value_type;
        switch (p.base_type) {
            case k::jai_int_type: return eng_.get_type_info_int();
            case k::jai_float_type: return eng_.get_type_info_float();
            case k::jai_bool_type: return eng_.get_type_info_bool();
            case k::jai_char_type: return eng_.get_type_info_char();
            case k::jai_string_type: return eng_.get_type_info_string();
            case k::jai_invalid_type: return eng_.get_type_info_void();
            case k::jai_object_type:
            case k::jai_shared_ptr_type: {
                auto def = eng_.get_class_definition_by_type(p.cpp_type);
                if (!def) { return nullptr; }
                type_info* object = eng_.get_type_info_object(def->get_name());
                return p.base_type == k::jai_object_type ? object : eng_.get_type_info_shared_ptr(object);
            }
            case k::jai_function_type: {
                if (!p.signature) { return eng_.get_type_info_untyped_function(); }
                std::vector<type_info*> arguments;
                for (size_t i = 0; i < p.signature->parameters.size(); ++i) {
                    type_info* argument = host_parameter_type(p.signature->parameters[i]);
                    if (p.signature->references[i] && argument) { argument = eng_.get_type_info_reference(argument); }
                    arguments.push_back(argument ? argument : eng_.get_type_info_any());
                }
                type_info* result = host_parameter_type(p.signature->result);
                return eng_.get_type_info_function(result ? result : eng_.get_type_info_any(), arguments);
            }
            default:
                return is_math_type(p.base_type) ? eng_.get_type_info_math(p.base_type) : nullptr;
        }
    }

    // The type each argument of a host call initializes where every registration the count admits agrees on it
    void host_arguments_agreed(const std::vector<std::vector<param_type_info>>& overloads, size_t argc, std::vector<const type_info*>& out) {
        std::vector<const type_info*> agreed(argc, nullptr);
        bool any = false;
        for (const auto& params : overloads) {
            if (params.size() != argc) { continue; }
            for (size_t i = 0; i < argc; ++i) {
                const type_info* declared = params[i].base_type == script_value_type::jai_function_type ? host_parameter_type(params[i]) : nullptr;
                if (!any) { agreed[i] = declared; } else if (agreed[i] != declared) { agreed[i] = nullptr; }
            }
            any = true;
        }
        if (any) { out = std::move(agreed); }
    }
    void host_argument_types(const std::string& name, size_t argc, std::vector<const type_info*>& out) {
        std::vector<engine::host_overload_signature> sigs;
        if (!eng_.host_function_signatures(name, sigs)) { return; }
        std::vector<std::vector<param_type_info>> overloads;
        for (auto& sig : sigs) { overloads.push_back(std::move(sig.param_types)); }
        host_arguments_agreed(overloads, argc, out);
    }
    void host_method_argument_types(std::string_view cls, std::string_view member, size_t argc, std::vector<const type_info*>& out) {
        auto def = engine_class(cls);
        if (!def || !def->is_cpp_class()) { return; }
        const auto& table = def->cpp_method_overload_table();
        auto it = table.find(eng_.symbolize(member));
        if (it == table.end()) { return; }
        auto bucket = it->second.find(argc);
        if (bucket == it->second.end()) { return; }
        std::vector<std::vector<param_type_info>> overloads;
        for (const auto& entry : bucket->second) { overloads.push_back(entry.param_types); }
        host_arguments_agreed(overloads, argc, out);
    }

    // A script callable handed to a std::function parameter: what keeps it from standing behind the signature (empty: nothing)
    std::string host_callback_issue(const ctype& arg, const param_type_info& p) {
        if (p.base_type != script_value_type::jai_function_type || !p.signature || arg.kind != ctype::function_k) { return {}; }
        ctype slot = ctype::make(ctype::function_k);
        slot.ti = host_parameter_type(p);
        return signature_issue(arg, slot);
    }
    // the first argument of a host call whose callable the one registration of that count refuses, reported with its reason
    bool report_host_callback(const std::string& fname, const std::vector<std::vector<param_type_info>>& overloads, const std::vector<ctype>& args,
                              const call_expr* c) {
        const std::vector<param_type_info>* only = nullptr;
        for (const auto& params : overloads) {
            if (params.size() != args.size()) { continue; }
            if (only) { return false; }
            only = &params;
        }
        if (!only || !c) { return false; }
        for (size_t i = 0; i < args.size(); ++i) {
            if (const std::string issue = host_callback_issue(args[i], (*only)[i]); !issue.empty()) {
                diag(diag_level::error, c->arguments[i]->location, "argument " + std::to_string(i + 1) + " of '" + fname + "': cannot pass '" +
                     ctype_name(args[i]) + "' where it takes '" + host_parameter_type((*only)[i])->canonical_name() + "': " + issue);
                return true;
            }
        }
        return false;
    }

    // What each argument of a call initializes, where one declaration answers the call: the parameter types every overload the
    // count admits agrees on, a signature slot's, a typed array's element for push. Asked only for a call that passes a literal
    // whose checking takes it.
    void expected_argument_types(const call_expr* c, std::vector<const type_info*>& out) {
        const size_t argc = c->arguments.size();
        if (!std::any_of(c->arguments.begin(), c->arguments.end(), [](const expression_ptr& a) { return contextual_literal(a.get()); })) { return; }
        const auto from_signature = [&](const type_info* signature) {
            if (!signature || !signature->is_signature() || signature->type_params.size() - 1 != argc) { return; }
            for (size_t i = 1; i <= argc; ++i) { out.push_back(signature->type_params[i].get()); }
        };
        const auto from_overloads = [&](const std::vector<const function_decl*>& fns) {
            std::vector<const type_info*> agreed(argc, nullptr);
            bool any = false;
            for (const function_decl* fn : fns) {
                auto [min_a, max_a] = arity_range(fn);
                if (argc < min_a || argc > max_a) { continue; }
                for (size_t i = 0; i < argc; ++i) {
                    const type_info* declared = fn->parameters[i].is_reference ? nullptr : fn->parameters[i].type.get();
                    if (!any) { agreed[i] = declared; } else if (agreed[i] != declared) { agreed[i] = nullptr; }
                }
                any = true;
            }
            if (any) { out = std::move(agreed); }
        };
        const auto from_member = [&](const member_hit& hit) {
            if (!hit.found) { return; }
            if (hit.methods) { from_overloads(*hit.methods); } else if (hit.field) { from_signature(hit.field->type.get()); }
        };
        const expression* callee = c->callee.get();
        if (callee && callee->get_type() == node_type::identifier_expr) {
            const auto* id = static_cast<const identifier_expr*>(callee);
            size_t scope_index = 0;
            if (const var_entry* local = callable_variable(id, &scope_index)) {
                if (!local->dynamic) { from_signature(local->type.ti); }
                return;
            }
            if (class_ctx_ && !id->global_qualified) {
                const member_hit hit = lookup_member(class_ctx_name_, id->name, false);
                if (hit.found || hit.unresolved) { from_member(hit); return; }
            }
            if (const std::string space = id->global_qualified ? std::string() : enclosing_namespace_member(id->name); !space.empty()) {
                if (auto it = namespace_fns_.find(space + "::" + std::string(id->name)); it != namespace_fns_.end() && namespace_member_settled(space, id->name, c->location)) {
                    from_overloads(it->second);
                }
                return;
            }
            if (auto it = functions_.find(id->name); it != functions_.end()) { from_overloads(it->second); return; }
            if (const sclass* sc = find_sclass(id->name)) { from_overloads(sc->ctors); return; }
            if (const function_decl* declared = program_function(id->name)) { from_overloads({declared}); return; }
            host_argument_types(std::string(id->name), argc, out);
            return;
        }
        if (callee && callee->get_type() == node_type::member_expr) {
            const auto* m = static_cast<const member_expr*>(callee);
            if (m->is_static) {
                if (auto it = namespace_fns_.find(qualified_spelling(m)); it != namespace_fns_.end()) { from_overloads(it->second); return; }
                const ctype recv = peek_type(m->object.get());
                if (recv.kind == ctype::class_ref_k) { from_member(lookup_member(recv.cls, m->member, true)); }
                return;
            }
            const ctype recv = peek_type(m->object.get());
            if (recv.kind == ctype::object_k && !recv.cls.empty()) {
                const member_hit hit = lookup_member(recv.cls, m->member, false);
                if (!hit.methods && !hit.field) { host_method_argument_types(recv.cls, m->member, argc, out); } else { from_member(hit); }
            } else if (recv.kind == ctype::array_k && recv.ti && ((m->member == "push" && argc == 1) || (m->member == "insert" && argc == 2))) {
                out.assign(argc, nullptr);
                out.back() = recv.ti->element_type().get();
            }
        }
    }

    // ---------------------------------------------------------------- contextual lambda types

    // What a lambda's parameters and result were before a walk typed them from the signature it is checked against: a walk that
    // only reports puts them back, the walk that stamps the program keeps them (the backends bind and convert by them)
    struct lambda_context {
        std::vector<std::pair<type_info_ptr, bool>> parameters;
        type_info_ptr result;
        bool contextual_result = false;
    };

    lambda_context open_lambda_context(lambda_expr* l, const type_info* signature) {
        lambda_context was;
        was.parameters.reserve(l->parameters.size());
        for (parameter& p : l->parameters) {
            was.parameters.emplace_back(p.type, p.contextual);
            if (p.contextual) {   // as parsed: a bare name is a var, an auto has no type
                p.type = p.untyped ? type_info_ptr{eng_.get_type_info_any()} : type_info_ptr{};
                p.contextual = false;
            }
        }
        was.result = l->return_type;
        was.contextual_result = l->contextual_return;
        if (l->contextual_return) {
            l->return_type = type_info_ptr{};
            l->contextual_return = false;
        }
        if (!signature || !signature->is_signature() || signature->type_params.size() - 1 != l->parameters.size()) { return was; }
        for (size_t i = 0; i < l->parameters.size(); ++i) {
            parameter& p = l->parameters[i];
            const type_info* declared = signature->type_params[i + 1].get();
            if (declared && declared->base_type == script_value_type::jai_reference_type) { declared = declared->element_type().get(); }
            if (p.is_reference || !(p.untyped || !p.type) || dynamic_type(declared)) { continue; }
            p.type = type_info_ptr{const_cast<type_info*>(declared)};
            p.contextual = true;
        }
        const type_info* result = signature->type_params[0].get();
        if (!l->return_type && !dynamic_type(result) && result->base_type != script_value_type::jai_null_type) {
            l->return_type = type_info_ptr{const_cast<type_info*>(result)};
            l->contextual_return = true;
        }
        return was;
    }

    void close_lambda_context(lambda_expr* l, const lambda_context& was) {
        if (!semantic_walk()) {
            for (size_t i = 0; i < l->parameters.size(); ++i) {
                l->parameters[i].type = was.parameters[i].first;
                l->parameters[i].contextual = was.parameters[i].second;
            }
            l->return_type = was.result;
            l->contextual_return = was.contextual_result;
            return;
        }
        bool moved = l->return_type.get() != was.result.get();
        for (size_t i = 0; i < l->parameters.size(); ++i) {
            const parameter& p = l->parameters[i];
            if (p.type.get() == was.parameters[i].first.get()) { continue; }
            moved = true;
            // a store to the name rebinds a handle only while the parameter is a var (identifier_expr::names_var_decl, the parser's)
            detail::body_walker names;
            names.symbolizer = nullptr;
            names.on_identifier = [&p](identifier_expr* id) {
                if (id->symbol_id == p.symbol_id && id->slot_index == p.slot_index && id->slot_index != SIZE_MAX) { id->names_var_decl = !p.contextual; }
            };
            names.stmt(l->body);
        }
        if (moved) {   // the functions made from here on bind by the new declaration
            l->shared_params_cache.reset();
            l->mint_cache.reset();
            l->graft_decl.reset();
            l->graft_overloads.clear();
        }
    }

    JAI_NOINLINE ctype infer_this(expression* e) {
        if (!capture_stack_.empty() && !fn_stack_.back().sees_fields &&
            capture_stack_.back().lambda->default_capture == lambda_expr::capture_default::none) {
            diag(diag_level::error, e->location, "lambda capture list does not capture 'this'; use [this], [=] or [&]");
        }
        return class_ctx_ ? ctype::object(class_ctx_name_) : ctype{};
    }

    // cast<T>(e): certain T (proofs flow); cast<T&>(e): T for member lookup, never a store proof (tentative, like every reference);
    // cast<shared_ptr<T>>(e): the handle spelling. The operand is the explicit typed boundary - never checked against T.
    // dynamic_cast names a class and takes an operand that can hold an object: an operand whose declared type holds none is the
    // load's error, and two classes neither of which derives from the other meet only in a class derived from both
    bool dynamic_cast_of_enum(const cast_expr* c) {
        const type_info* named = c->by_reference ? c->target.get() : c->target->element_type().get();
        if (!named || (!enum_type_name(*named) && !find_enum(named->type_name))) { return false; }
        const std::string name(named->type_name);
        diag(diag_level::error, c->location, "dynamic_cast<" + name + (c->by_reference ? "&>" : ">") +
             " names a class, and '" + name + "' is an enum: cast<" + name + ">(n) converts an int that is one of its values");
        return true;
    }
    void check_dynamic_cast(const cast_expr* c, const ctype& operand) {
        const type_info* named = c->by_reference ? c->target.get() : c->target->element_type().get();
        if (!named) { return; }
        const std::string name(named->type_name);
        const std::string form = "dynamic_cast<" + name + (c->by_reference ? "&>" : ">");
        if (operand.tentative || operand.is_unknown() || operand.narrowed) { return; }
        switch (operand.kind) {
            case ctype::int_k: case ctype::float_k: case ctype::bool_k: case ctype::char_k: case ctype::string_k: case ctype::utf8_k:
            case ctype::array_k: case ctype::map_k: case ctype::math_k: case ctype::function_k:
                diag(diag_level::error, c->location, form + (c->by_reference ? " always raises" : " is always null") + ": the operand is '" + ctype_name(operand) + "', never an object of a class");
                break;
            case ctype::object_k:
                if (!operand.cls.empty() && class_relation(operand.cls, name) == 0 && class_relation(name, operand.cls) == 0) {
                    diag(diag_level::warning, c->location, form + (c->by_reference ? " raises" : " is null") + " for every '" + std::string(operand.cls) +
                         "' but one whose class derives from '" + name + "' too");
                }
                break;
            default: break;
        }
    }

    JAI_NOINLINE ctype infer_cast(cast_expr* c) {
        if (c->implicit_ref) {   // a piece of an assignment target read as a reference: its operand's type
            ctype operand_t = infer(c->operand.get());
            operand_t.tentative = true;
            return operand_t;
        }
        ctype operand_t = infer(c->operand.get());
        if (c->type_value) {   // a type argument: its type names a type, as a declaration's does
            stamp_const(c->const_check, const_none, {});
            if (unknown_type_arguments_ && unknown_bare_type(c->target.get())) {   // its call reports it, once the callee is known
                unknown_type_arguments_->emplace_back(c->target, c->location);
                return ctype::object("reflect::type");
            }
            check_spelled_type(c->target.get(), c->location);
            return ctype::object("reflect::type");
        }
        if (c->by_reference) { report_mixed_reference_conditional(c->operand.get()); }
        check_const_cast(c);
        ctype t = ctype_from_type_info(c->target.get());
        if (t.kind == ctype::object_k && refuse_bare_generic_class(t.cls, c->location)) { return {}; }
        if (report_unbound_instantiation(c->target.get(), c->location)) { return {}; }
        if (c->dynamic && dynamic_cast_of_enum(c)) { return {}; }
        if (t.kind == ctype::object_k && !known_class(t.cls) && !extra_global(std::string(t.cls))) {
            diag(diag_level::error, c->location, "unknown type in cast: '" + std::string(t.cls) + "'");
            return {};
        }
        if (c->dynamic) { check_dynamic_cast(c, operand_t); }
        refuse_handle_of_value(c, operand_t);
        if (!c->by_reference && !c->truth_read) { note_this_into_handle(operand_t, c->target.get()); }
        if (collect_diagnostics_ && !c->by_reference && c->target->base_type == script_value_type::jai_object_type) {
            warn_cast_copy(c, operand_t);
        }
        if (operand_t.const_key && (c->by_reference || handle_type(c->target.get()))) {
            map_key_alias(c->location, c->operand.get());
        }
        if (c->by_reference) { t.tentative = true; }
        return t;
    }

    bool bare_this_field_ = false;
    // a bare name of a method body no local and no member of the modeled class chain takes (every ancestor resolved)
    bool bare_program_name(const identifier_expr* id) {
        if (!bare_field_context() || id->global_qualified || id->slot_index != SIZE_MAX || !namespace_walk_names_.empty()) { return false; }
        size_t scope_index = 0;
        if (find_named(id, &scope_index) && scope_index != 0) { return false; }   // a local or a namespace's name; the unit's own program scope reads as any program-scope name
        const member_hit hit = lookup_member(class_ctx_name_, id->name, /*want_static=*/false);
        return !hit.found && !hit.unresolved && hit.certain;
    }
    JAI_NOINLINE ctype infer_identifier(identifier_expr* id) {
        bare_this_field_ = false;
        ctype t = infer_identifier_impl(id);
        stamp(id->this_field, bare_this_field_, false);
        stamp(id->program_name, semantic_walk() && bare_program_name(id), false);
        if (extended_names_.count(id->name)) { return {}; }
        return t;
    }

    ctype infer_identifier_impl(identifier_expr* id) {
        size_t scope_index = 0;
        if (var_entry* local = find_named(id, &scope_index)) {
            if (scope_index == 0) { note_global_use(id->name); }   // top-level var read
            for (const auto& cap : capture_stack_) {
                const bool program_scope = scope_index == 0 ||
                    std::find(namespace_scopes_.begin(), namespace_scopes_.end(), scope_index) != namespace_scopes_.end();
                if (program_scope || scope_index >= cap.base || cap.lambda->default_capture != lambda_expr::capture_default::none) { continue; }
                bool listed = std::any_of(cap.lambda->captures.begin(), cap.lambda->captures.end(),
                    [&](const auto& c) { return c.name == id->name; });
                if (!listed) {
                    diag(diag_level::error, id->location, "lambda capture list does not capture enclosing local '" +
                        std::string(id->name) + "'; name it or use [=] or [&]", local->line, local->column);
                }
            }
            if (const ctype* narrowed = narrow_read(*local, scope_index, /*referent=*/false)) { return *narrowed; }
            return local->dynamic ? (local->alias.kind != ctype::unknown ? local->alias : ctype{}) : local->type;
        }
        if (class_ctx_ && !id->global_qualified) {
            member_hit hit = lookup_member(class_ctx_name_, id->name, /*want_static=*/false);
            if (hit.found) {
                if (!capture_stack_.empty() && !fn_stack_.back().sees_fields &&
                    capture_stack_.back().lambda->default_capture == lambda_expr::capture_default::none) {
                    diag(diag_level::error, id->location, "lambda capture list does not capture 'this' for member '" + std::string(id->name) + "'; use [this], [=] or [&]");
                    return {};
                }
                if (access_denied(hit, id->name, id->location)) { return {}; }
                if (hit.methods) {
                    if (!hit.methods->empty() && !hit.methods->front()->is_static && bare_field_context()) {   // a method is called or bound, never read bare
                        diag(diag_level::error, id->location, detail::bare_method_value_text(id->name, class_ctx_name_));
                    }
                    return ctype::make(ctype::function_k);
                }
                if (hit.field && !hit.field->is_static && !hit.methods && bare_field_context() && !chain_has_user_accessor(class_ctx_name_, id->name)) { bare_this_field_ = true; }
                report_unready_field_read(hit.field, id->location);
                if (hit.field && (hit.field->type || hit.field->function_spelling)) { return member_type(hit); }
                return {};
            }
            if (hit.unresolved) { return {}; }   // an ancestor nowhere to be found can define anything
        }
        if (!id->global_qualified) {
            const std::string space = enclosing_namespace_member(id->name);
            if (!space.empty()) {
                if (namespace_member_unready(space, id->name, id->location, false) || !namespace_member_settled(space, id->name, id->location)) { return {}; }
                return namespace_member_read(space, id->name);
            }
        }
        if (auto declared = functions_.find(id->name); declared != functions_.end()) {
            note_global_use(id->name);
            report_function_before_declaration(id->name, id->location, false);
            report_instance_before_declaration(id->name, id->location, false);
            ctype named = ctype::make(ctype::function_k);
            if (foreign_overloads(id->name, declared->second).empty()) { named.fns = &declared->second; }   // another unit's overloads answer the name too
            return named;
        }
        if (find_sclass(id->name)) {
            if (refuse_bare_generic_class(id->name, id->location)) { return {}; }
            report_class_before_declaration(id->name, id->location);
            report_instance_before_declaration(id->name, id->location, false);
            return ctype{ctype::class_ref_k, id->name};
        }
        if (enums_.count(id->name)) { return ctype{ctype::enum_ref_k, id->name}; }
        std::string name(id->name);
        if (namespaces_.count(id->name)) {
            if (!program_global(name) && !extra_global(name)) { namespace_as_value(id->location, id->name); }
            return {};
        }
        report_use_before_declaration(id->name, id->location, false, [&] { return eng_.has_variable(name); });
        if (!bare_name_reaches_members(id) && program_function(id->name)) { note_global_use(id->name); return ctype::make(ctype::function_k); }
        if (const variable_decl* g = program_global(name)) { note_global_use(id->name); return program_global_type(g); }   // the proof pass alone types a global another unit declares
        if (extra_global(name)) { note_global_use(id->name); return program_scope_read_type(name); }
        if (eng_.is_type_name(name) || eng_.find_script_class(name)) {
            if (refuse_bare_generic_class(id->name, id->location)) { return {}; }
            return ctype{ctype::class_ref_k, id->name};
        }
        if (eng_.has_function(name)) { note_global_use(id->name); return ctype::make(ctype::function_k); }
        if (eng_.has_variable(name)) { note_global_use(id->name); return engine_global_type(name); }
        if (eng_.find_namespace(name) && !eng_.get_registered_template_types().count(name) && !derived_class_member(id->name)) {
            namespace_as_value(id->location, id->name);
            return {};
        }
        if (engine_knows_name(name)) { return {}; }
        if (program_or_derived_name(id->name)) { return {}; }
        if (stale_field_context()) { return {}; }   // bare old-shape field in a redefined class's method
        if (refuse_bare_generic_class(id->name, id->location)) { return {}; }   // a generic class's name and nothing else's
        undeclared(id->location, id->name, id->less_compared);
        return {};
    }

    // an enum's value by the enum's own name (this unit's enum, or the one enum of that name another unit of the program declares):
    // no local, field or variable of the program takes the name and no runtime include could
    bool enum_value_read(const member_expr* m, const ctype& recv, const ctype& t) {
        if (m->is_static || m->null_safe || !m->object || m->object->get_type() != node_type::identifier_expr || has_dynamic_includes_) return false;
        auto* id = static_cast<const identifier_expr*>(m->object.get());
        const std::string name(id->name);
        if (program_global(name)) return false;
        if (recv.kind == ctype::enum_ref_k) return t.kind == ctype::int_k;
        if (!recv.is_unknown() || class_ctx_ || id->slot_index != SIZE_MAX || id->global_qualified || !program_enums_) return false;
        if (find_named(id) || functions_.count(id->name) || enums_.count(id->name)) return false;
        eng_.note(name);
        const auto it = program_enums_->find(name);
        if (it == program_enums_->end() || it->second.size() != 1 || !it->second.begin()->second) return false;
        for (const auto& [vname, _] : it->second.begin()->second->values) { if (vname == m->member) return true; }
        return false;
    }

    // a member read through a reference local declared of a class (`Body& b = bodies[i]; b.x`): the binding admits that class or a
    // subclass, so a scalar field's declared kind is a proof source the typed rows guard, tentative for diagnostics (an element read's standing)
    ctype reference_field_read(const member_expr* m) {
        if (m->is_static || m->null_safe || !m->object || m->object->get_type() != node_type::identifier_expr) { return {}; }
        const auto* local = find_local(static_cast<const identifier_expr*>(m->object.get())->name);
        if (!local || !local->is_reference || !local->declared) { return {}; }
        const ctype declared = ctype_from_type_info(local->declared);
        if (declared.kind != ctype::object_k) { return {}; }
        const member_hit h = lookup_member(declared.cls, m->member, false);
        if (!h.found || h.methods || !h.field || h.field->is_static || !h.field->type || chain_has_user_accessor(declared.cls, m->member)) { return {}; }
        ctype t = ctype_from_type_info(h.field->type.get());
        if (t.kind != ctype::int_k && t.kind != ctype::float_k) { return {}; }
        t.tentative = true;
        t.element_proof = true;
        return t;
    }

    // What a reference names, for member diagnostics: a reference local, parameter or loop variable of a known class or typed
    // container, then the fields and elements reached through it. The proof pass never reads it.
    ctype referent_receiver(const expression* e) {
        if (!e) { return {}; }
        switch (e->get_type()) {
            case node_type::identifier_expr: {
                const auto* id = static_cast<const identifier_expr*>(e);
                if (extended_names_.count(id->name)) { return {}; }
                size_t scope_index = 0;
                const var_entry* local = find_named(id, &scope_index);
                if (!local) { return {}; }
                if (const ctype* narrowed = narrow_read(*local, scope_index, /*referent=*/true)) { return *narrowed; }
                return local->referent;
            }
            case node_type::member_expr: {
                const auto* m = static_cast<const member_expr*>(e);
                if (m->is_static || m->null_safe || extended_members_.count(m->member)) { return {}; }
                const ctype held = referent_receiver(m->object.get());
                if (held.kind != ctype::object_k) { return {}; }
                const member_hit hit = lookup_member(held.cls, m->member, false);
                if (!hit.found || hit.methods || !hit.field || hit.field->is_static || !hit.field->type || chain_has_user_accessor(held.cls, m->member)) { return {}; }
                return referent_type(member_type(hit));
            }
            case node_type::binary_expr: {
                const auto* b = static_cast<const binary_expr*>(e);
                if (b->op.type != token_type::left_bracket) { return {}; }
                const ctype held = referent_receiver(b->left.get());
                if (held.kind != ctype::array_k && held.kind != ctype::map_k) { return {}; }
                return referent_type(container_value_type(held));
            }
            default:
                return {};
        }
    }

    ctype infer_member(member_expr* m, bool writing, const variable_decl** typed_field_out = nullptr,
                       ctype* recv_out = nullptr, member_hit* hit_out = nullptr) {
        ctype recv;
        ctype t = infer_member_impl(m, writing, typed_field_out, &recv, hit_out);
        if (recv_out) { *recv_out = recv; }
        stamp(m->enum_constant, !writing && enum_value_read(m, recv, t), false);
        return t;
    }

    ctype infer_member_impl(member_expr* m, bool writing, const variable_decl** typed_field_out,
                            ctype* recv_out, member_hit* hit_out) {
        if (m->is_static && binary_width_token(m)) { return object_of("binary::width"); }
        if (m->is_static && namespace_path_receiver(m) && !namespace_class_receiver(m)) {
            if (const enum_decl* named = namespace_enum(m)) { return ctype{ctype::enum_ref_k, named->name}; }
            const bool enum_value = m->object->get_type() == node_type::member_expr && namespace_enum(static_cast<const member_expr*>(m->object.get()));
            if (!enum_value) {
                check_namespace_member(m);
                if (const var_entry* x = qualified_extern(m); x && !x->dynamic) { return x->type; }
                const std::string path = member_path(m);
                return path.empty() ? ctype{} : namespace_member_read(path, m->member);
            }
        }
        ctype recv = infer(m->object.get());
        if (recv_out) { *recv_out = recv; }
        if (function_member(m, recv)) { return {}; }
        if (recv.kind == ctype::math_k) {
            // a component (x / width, y / height, z / depth): an int or a float for diagnostics, never a proof source (the
            // VM's component rows are a later phase); anything else is the runtime's no-member text, statically
            const script_value_type kind = math_type_from_name(recv.cls);
            if (detail::math::component_of(kind, m->member) < 0) {
                if (detail::vec_method_fn(kind, m->member) != math_fn::none) { return ctype::make(ctype::function_k); }
                diag(diag_level::error, m->location, std::string(recv.cls) + " has no member '" + std::string(m->member) + "'");
                return {};
            }
            ctype c = ctype::make(detail::math::integral(kind) ? ctype::int_k : ctype::float_k);
            c.tentative = true;
            c.declared_result = true;
            return c;
        }
        if (recv.kind == ctype::coroutine_k) {
            if (!recv.tentative && m->member != "next" && m->member != "done" && m->member != "advance" && m->member != "result") { coroutine_member_missing(recv, m); }
            return {};
        }
        if (recv.is_unknown() && !m->is_static) {
            if (const ctype held = referent_receiver(m->object.get()); held.kind == ctype::object_k) {
                infer_class_member(m, held, writing, typed_field_out, hit_out);
                if (recv_out) { *recv_out = held; }
            }
        }
        if (recv.is_unknown() && !writing) {
            if (ctype t = reference_field_read(m); !t.is_unknown()) { return t; }
        }
        if (recv.is_unknown() || recv.kind == ctype::map_k || recv.kind == ctype::array_k ||
            recv.kind == ctype::string_k || recv.kind == ctype::utf8_k || recv.kind == ctype::function_k || recv.kind == ctype::weak_k || recv.kind == ctype::null_k ||
            recv.kind == ctype::int_k || recv.kind == ctype::float_k || recv.kind == ctype::char_k ||
            recv.kind == ctype::bool_k) {
            if (!writing && kind_certain(recv) && !m->is_static) {
                builtin_method_value(recv, m);
                scalar_member_missing(recv, m);
            }
            if (writing && recv.kind == ctype::map_k && kind_certain(recv) && !m->is_static) {
                diag(diag_level::error, m->location, "cannot assign to member '" + std::string(m->member) + "' of a map; store the key as [\"" + std::string(m->member) + "\"]");
            }
            return {};   // builtin methods / map keys / dynamic: lenient
        }
        const ctype member = infer_class_member(m, recv, writing, typed_field_out, hit_out);
        return recv.declared_slot ? read_through_declared_slot(member) : member;
    }

    // obj.name where name is a static of obj's script class and no instance member: the run finds no such member on an instance,
    // so the load reports it and names the qualified spelling
    bool static_through_instance(const member_expr* m, const ctype& recv, const member_hit& hit, bool call) {
        if (!hit.found || recv.kind != ctype::object_k) { return false; }
        bool is_static = hit.field && hit.field->is_static;
        if (hit.methods) {
            is_static = !hit.methods->empty();
            for (const auto* fn : *hit.methods) { is_static = is_static && fn->is_static; }
        }
        if (!is_static || chain_has_user_accessor(recv.cls, m->member) || stale_shape_receiver(recv, m->object.get())) { return false; }
        const member_hit instance = lookup_member(recv.cls, m->member, /*want_static=*/false, 0, /*instance_only=*/true);
        if (instance.found || !instance.certain) { return false; }
        diag(diag_level::error, m->location, "'" + std::string(m->member) + "' is a static member of class '" + std::string(recv.cls) +
             "': name it " + std::string(recv.cls) + "::" + std::string(m->member) + (call ? "(...)" : ""));
        return true;
    }

    // A member of a class, an enum or a class used as a value; a reference's referent (diagnostic_only) is checked alike
    ctype infer_class_member(member_expr* m, const ctype& recv, bool writing, const variable_decl** typed_field_out,
                             member_hit* hit_out) {
        if (extended_members_.count(m->member)) { return {}; }
        if (class_dot_receiver(m, recv)) { return {}; }
        if (recv.kind == ctype::enum_ref_k) {
            if (const enum_decl* e = find_enum(recv.cls)) {
                for (const auto& [vname, _] : e->values) {
                    if (vname == m->member) { return ctype{ctype::int_k, e->name}; }
                }
                diag(diag_level::error, m->location, "no value '" + std::string(m->member) +
                     "' in enum '" + std::string(recv.cls) + "'",
                     e->location);
            }
            return {};
        }
        bool want_static = (recv.kind == ctype::class_ref_k);
        std::string_view cls = recv.cls;
        member_hit hit = lookup_member(cls, m->member, want_static);
        if (access_denied(hit, m->member, m->location)) { return {}; }
        if (hit_out) { *hit_out = hit; }
        if (!want_static && chain_has_user_accessor(cls, m->member)) { return accessor_read_type(cls, m->member); }   // a user _get_/_set_ answers: known member, typed only by a single declared getter, never a proof
        if (!want_static && static_through_instance(m, recv, hit, false)) { return {}; }
        if (hit.found) {
            if (!writing && !want_static && m->object->get_type() == node_type::this_expr) { report_unready_field_read(hit.field, m->location); }
            if (typed_field_out && hit.field && (hit.field->type || hit.field->function_spelling)) { *typed_field_out = hit.field; }
            if (hit.methods) {
                if (!writing && !want_static) {   // a method is called or bound, never read bare
                    diag(diag_level::error, m->location, detail::method_value_text(m->member, cls, detail::method_value_receiver(m), false) + (m->less_compared ? " (" + no_generic_text(m->member) + ")" : std::string()));
                }
                ctype named = ctype::make(ctype::function_k);
                if (want_static && hit.provable) { named.fns = hit.methods; }
                return named;
            }
            if (hit.field && (hit.field->type || hit.field->function_spelling)) { return member_type(hit); }
            return {};
        }
        // a generic method is no member: it is named, called or bound with its type arguments
        if (want_static && instance_requests_.count(std::string(cls) + "::" + std::string(m->member))) { return {}; }   // its request says why
        if (const std::string generic = generic_value_spelled(class_generics(cls, m->member), want_static ? std::string(cls) + "::" + std::string(m->member) : std::string(m->member),
                                                              want_static ? std::string() : detail::method_value_receiver(m));
            !generic.empty()) {
            diag(diag_level::error, m->location, generic);
            return {};
        }
        if (!hit.certain || recv.open) { return {}; }
        if (stale_shape_receiver(recv, m->object.get())) { return {}; }
        if (!want_static) {
            if (ctype t; handle_builtin_result(m->member, t)) { return {}; }   // builtin as a value: lenient
        }
        declared_at declared;
        if (auto it = classes_.find(cls); it != classes_.end()) { declared = it->second.decl->location; }
        diag(diag_level::error, m->location,
             std::string(want_static ? "no static member '" : "no member '") + std::string(m->member) +
             "' in class '" + std::string(cls) + "'", declared);
        (void)writing;
        return {};
    }

    // engine::constant_argument: an argument only the program's constants may fill must be a constant form
    void check_constant_arguments(const std::string& callable, const std::string& shown, const call_expr* c) {
        if (!eng_.has_constant_arguments()) { return; }
        for (const auto& [index, refusal] : eng_.constant_argument_indexes(callable)) {
            const size_t at = index + c->type_argument_count;   // an index counts the value arguments
            if (at >= c->arguments.size()) { continue; }
            std::vector<std::string> texts;
            if (detail::constant_texts(c->arguments[at].get(), eng_.constant_texts_by_name(), texts)) { continue; }
            diag(diag_level::error, c->arguments[at]->location, "argument " + std::to_string(index + 1) + " of '" + shown + "' is not a constant: " + *refusal);
        }
    }

    // The callee of a call with type arguments as its text spells it: a name, or a name's member; empty for any other
    static std::string type_argument_callee(const call_expr* c) {
        if (!c->callee) { return {}; }
        if (c->callee->get_type() == node_type::identifier_expr) { return std::string(static_cast<const identifier_expr*>(c->callee.get())->name); }
        if (c->callee->get_type() != node_type::member_expr) { return {}; }
        const auto* m = static_cast<const member_expr*>(c->callee.get());
        if (!m->object || m->object->get_type() != node_type::identifier_expr) { return {}; }
        return std::string(static_cast<const identifier_expr*>(m->object.get())->name) + (m->is_static ? "::" : m->is_arrow ? "->" : ".") + std::string(m->member);
    }

    JAI_NOINLINE ctype infer_call(call_expr* c) {
        std::vector<ctype> args;
        std::vector<std::pair<type_info_ptr, source_location>> unknown_types;   // type arguments that name no type: one error, once the callee is known
        args.reserve(c->arguments.size());
        const std::string_view callee_name = c->callee && c->callee->get_type() == node_type::identifier_expr
            ? std::string_view(static_cast<const identifier_expr*>(c->callee.get())->name) : std::string_view{};
        const bool parallel_callee = callee_name == "parallel_transform" || callee_name == "parallel_reduce";
        const bool template_body = callee_name == detail::template_library::k_joiner;
        if (callee_name == detail::template_library::k_splice && c->arguments.size() == 3 && c->arguments[0]->get_type() == node_type::literal_expr &&
            c->arguments[2]->get_type() == node_type::literal_expr &&
            static_cast<const literal_expr*>(c->arguments[0].get())->value.raw_storage_index() == script_value::TYPEID_NULL) {
            diag(diag_level::error, c->arguments[0]->location, "a template splice is null: " +
                std::string(static_cast<const literal_expr*>(c->arguments[2].get())->value.unchecked_as_string()));
        }
        template_files_ += template_body;
        struct template_scope { int& depth; bool on; ~template_scope() { depth -= on; } } in_template{template_files_, template_body};
        std::vector<const type_info*> expected;
        expected_argument_types(c, expected);
        // An arrow that declares no parameter types, passed to a generic, takes them from the call's other arguments: it is
        // walked once the callee is known (deduce_type_arguments types it on the way), by the instance's own signature
        std::vector<expression*> arrows;
        bool generic_named = false;
        if (generics_live_ && c->callee && std::any_of(c->arguments.begin(), c->arguments.end(), [](const expression_ptr& a) { return untyped_arrow(a.get()); })) {
            bool receiver_unknown = false;
            generic_named = !generic_callee(c, receiver_unknown).empty();
        }
        for (size_t k = 0; k < c->arguments.size(); ++k) {
            auto& a = c->arguments[k];
            if (generic_named && untyped_arrow(a.get())) {
                ctype held = ctype::make(ctype::function_k);
                held.origin = a.get();
                args.push_back(held);
                arrows.push_back(a.get());
                continue;
            }
            const bool combine = callee_name == "parallel_reduce" && k == 3;   // runs serially after the join
            const bool body = parallel_callee && !combine && a->get_type() == node_type::lambda_expr;
            parallel_body_depth_ += body;
            auto* const outer_unknown = unknown_type_arguments_;
            unknown_type_arguments_ = k < c->type_argument_count ? &unknown_types : nullptr;
            args.push_back(infer_expecting(a.get(), k < expected.size() ? expected[k] : nullptr));
            unknown_type_arguments_ = outer_unknown;
            parallel_body_depth_ -= body;
        }
        const std::string spelled_callee = unknown_types.empty() ? std::string() : type_argument_callee(c);
        const generic_call generic = resolve_generic_call(c, args);
        if (!arrows.empty()) {
            if (generic == generic_call::rewritten) {
                expected.clear();
                expected_argument_types(c, expected);
            }
            for (size_t k = 0; k < c->arguments.size(); ++k) {
                expression* a = c->arguments[k].get();
                if (std::find(arrows.begin(), arrows.end(), a) != arrows.end()) { args[k] = infer_expecting(a, k < expected.size() ? expected[k] : nullptr); }
            }
        }
        if (generic != generic_call::none || spelled_callee.empty()) {   // a generic's own type arguments: the declaration's error
            for (const auto& [type, at] : unknown_types) { check_spelled_type(type.get(), at); }
            unknown_types.clear();
        }
        if (generic == generic_call::pending) { return {}; }   // a generic's call waits for its instance
        if (generic == generic_call::none && c->type_argument_count > 0) {   // Name<Args>(...) on a type that takes none: the declaration's error
            std::string named;   // a name alone or a qualified one: what a type's spelling is
            if (c->callee->get_type() == node_type::identifier_expr) {
                named = std::string(static_cast<const identifier_expr*>(c->callee.get())->name);
            } else if (c->callee->get_type() == node_type::member_expr && static_cast<const member_expr*>(c->callee.get())->is_static) {
                named = qualified_spelling(static_cast<const member_expr*>(c->callee.get()));
            }
            if (const plain_type_kind kind = plain_type_named(named); kind != plain_type_kind::none) {
                for (const auto& [type, at] : unknown_types) { check_spelled_type(type.get(), at); }
                diag(diag_level::error, c->location, no_type_arguments_text(named, kind), 0, 0, /*always_error=*/true);
                return {};
            }
        }
        const std::vector<const function_decl*>* ov = nullptr;   // the statically resolved same-script overload set
        ov_class_ = {};
        nodiscard_hit_ = {};
        void_hit_ = {};
        // a call in the callee's receiver resolves its own type arguments: the enclosing call's state comes back after it
        const bool outer_type_argument_call = type_argument_call_;
        std::string outer_script_callee = std::move(type_argument_script_callee_), outer_host_callee = std::move(type_argument_host_callee_);
        const std::string_view outer_script_kind = type_argument_script_kind_;
        type_argument_call_ = c->type_argument_count > 0;
        type_argument_script_callee_.clear();
        type_argument_host_callee_.clear();
        narrow_callee seen;
        ctype t = infer_call_impl(c, args, ov, seen);
        if (!unknown_types.empty() && !type_argument_host_callee_.empty()) {   // a host's type arguments: the declaration's error
            for (const auto& [type, at] : unknown_types) { check_spelled_type(type.get(), at); }
            unknown_types.clear();
        }
        if (!unknown_types.empty()) {
            // C#'s rule read `a < b, c > (d)` as a call with type arguments; the callee takes none and a type argument names no
            // type, so a comparison was most likely meant: the call's one error says how to write it
            const std::string first = unknown_types.front().first->canonical_name();
            const std::string first_given = static_cast<const cast_expr*>(c->arguments[0].get())->target->canonical_name();
            std::string given;
            for (size_t i = 0; i < c->type_argument_count; ++i) { given += (i ? ", " : "") + static_cast<const cast_expr*>(c->arguments[i].get())->target->canonical_name(); }
            diag(has_dynamic_includes_ ? diag_level::warning : diag_level::error, unknown_types.front().second,
                 "unknown type '" + first + "': '" + spelled_callee + "<" + given + ">(...)' reads as a call with type arguments, which '" + spelled_callee +
                 "' does not take; if this is a comparison, write (" + spelled_callee + " < " + first_given + ")");
            ov = nullptr;
            t = {};
        } else if (c->type_argument_count > 0) {
            if (!type_argument_script_callee_.empty()) {
                diag(diag_level::error, c->location, no_type_parameters_text(type_argument_script_callee_, type_argument_script_kind_));
                ov = nullptr;
                t = {};
            } else if (!type_argument_host_callee_.empty()) {
                if (const uint8_t shape = eng_.type_argument_result(type_argument_host_callee_)) { t = type_argument_result(c, shape); }
            }
        }
        type_argument_call_ = outer_type_argument_call;
        type_argument_script_callee_ = std::move(outer_script_callee);
        type_argument_script_kind_ = outer_script_kind;
        type_argument_host_callee_ = std::move(outer_host_callee);
        narrow_call_arguments(c, ov, seen);
        t = check_call(c, args, ov, std::move(t));
        if (!nodiscard_hit_.name.empty() && collect_diagnostics_) { nodiscard_calls_[c] = std::move(nodiscard_hit_); }
        nodiscard_hit_ = {};
        if (!void_hit_.shown.empty() && !void_hit_.typed_slot && callee_rebindable(c)) { void_hit_ = {}; }
        if (!void_hit_.shown.empty() && !c->result_discarded) {
            report_void_result(c, void_hit_);
            t = {};   // the one error: no null to initialize, pass or compare
        } else if (!void_hit_.shown.empty()) {
            nothing_calls_[c] = {void_result_text(void_hit_), void_hit_.reason};
        }
        void_hit_ = {};
        call_results_[c] = t;
        return t;
    }

    JAI_NOINLINE ctype check_call(call_expr* c, const std::vector<ctype>& args, const std::vector<const function_decl*>* ov, ctype t) {
        const std::string_view ov_class = ov ? ov_class_ : std::string_view{};
        bool free_callee = false;   // resolved through the program's free functions (a method by its bare name, a constructor: never inlined)
        if (ov && c->callee && c->callee->get_type() == node_type::identifier_expr) {
            auto it = functions_.find(static_cast<identifier_expr*>(c->callee.get())->name);
            free_callee = it != functions_.end() && ov == &it->second;
        }
        check_const_call(c);
        check_const_arguments(c, ov, ov_class, free_callee, args);
        // a free function another unit of the program declares (one unit, once): the inliner's decl and the argument proofs; the
        // unit's own diagnostics keep their view (ov stays the unit's)
        std::vector<const function_decl*> program_ov;
        if (!ov && program_functions_ && c->callee && c->callee->get_type() == node_type::identifier_expr && reaches_program_scope(static_cast<identifier_expr*>(c->callee.get()))) {
            const std::string_view name = static_cast<identifier_expr*>(c->callee.get())->name;
            if (functions_.find(name) == functions_.end()) {
                eng_.note(name);
                auto pit = program_functions_->find(std::string(name));
                if (pit != program_functions_->end() && pit->second.size() == 1 && pit->second.begin()->second.size() == 1) { program_ov.push_back(pit->second.begin()->second[0].get()); }
                if (program_ov.size() == 1 && program_ov[0]->is_nodiscard) { nodiscard_hit_ = {std::string(name), {}}; }
                if (program_ov.size() == 1 && returns_nothing(program_ov[0])) { void_hit_ = {"'" + std::string(name) + "'", {}, nullptr, nothing_reason(program_ov[0])}; }
            }
            // the proof pass alone types the call by that declaration's scalar return (the consumers guard it; a graft re-checks the
            // callee's declared return per definition epoch, call_expr::k_proof_function)
            if (t.is_unknown() && program_ov.size() == 1 && semantic_walk()) {
                const uint16_t k = certain_kind(program_ov[0]->return_type.get());
                if (k != 0 && (k & 0xFF) != static_cast<uint16_t>(script_value_type::jai_array_type)) { t = ctype_from_type_info(program_ov[0]->return_type.get()); }
            }
        }
        // a local declared with a lambda literal: the lambda as a one-overload declaration (the guard checks the binding at run time)
        const std::vector<const function_decl*>* lambda_ov = nullptr;
        if (!ov && program_ov.empty() && c->callee && c->callee->get_type() == node_type::identifier_expr) {
            auto* id = static_cast<identifier_expr*>(c->callee.get());
            const var_entry* local = id->slot_index != SIZE_MAX && !id->global_qualified ? find_local(id->name) : nullptr;
            if (local && local->lambda && !lambda_copies_at_creation(local->lambda)) {
                auto* lam = const_cast<lambda_expr*>(local->lambda);
                if (!lam->graft_decl && lam->body && lam->body->get_type() == node_type::block_stmt) {
                    auto d = std::make_shared<function_decl>(lam->location, std::string_view("<lambda>"));
                    d->parameters = lam->parameters; d->return_type = lam->return_type;
                    d->body = std::static_pointer_cast<block_stmt>(lam->body); d->local_count = lam->local_count;
                    lam->graft_decl = d;
                    lam->graft_overloads.assign(1, d.get());
                }
                if (lam->graft_decl) {
                    // an untyped lambda whose returns are one certain kind declares that kind with the engine's canonical stamp
                    const uint8_t k = lam->return_type ? 0 : lam->inferred_return_kind;
                    lam->graft_decl->graft_return_kind = k;
                    lam->graft_decl->return_type = k == 1 ? type_info_ptr{eng_.get_type_info_int()} : k == 2 ? type_info_ptr{eng_.get_type_info_float()}
                                                 : k == 3 ? type_info_ptr{eng_.get_type_info_bool()} : lam->return_type;
                    lambda_ov = &lam->graft_overloads;
                }
            }
        }
        stamp_call_proofs(c, lambda_ov ? lambda_ov : program_ov.empty() ? ov : &program_ov, args, free_callee || !program_ov.empty() || lambda_ov != nullptr);
        {
            const function_decl* md = nullptr;
            if (stamping() && ov && ov->size() == 1 && c->callee) {
                const function_decl* fn = (*ov)[0];
                auto [lo, hi] = arity_range(fn);
                const bool graftable = fn->body && !fn->is_coroutine && !fn->is_static && lo == c->arguments.size() && hi == c->arguments.size();
                // a frame receiver's method a derived class overrides: the graft's guard would send most receivers to the real call
                const bool overridden = !overrides_of(ov_class, fn->name).empty();
                // a receiver of the method's own frame (`this`, a bare field, a sibling's bare call) only from a method body outside construction
                const bool own_frame = bare_field_context() && cdtor_ctx_ == cdtor_ctx::none;
                if (c->callee->get_type() == node_type::member_expr) {
                    auto* m = static_cast<member_expr*>(c->callee.get());
                    const expression* object = m->object.get();
                    auto* recv = object && object->get_type() == node_type::identifier_expr ? static_cast<const identifier_expr*>(object) : nullptr;
                    const bool slot_receiver = recv && recv->slot_index != SIZE_MAX && !recv->global_qualified;
                    const bool frame_receiver = own_frame && object &&
                        (object->get_type() == node_type::this_expr || (recv && recv->slot_index == SIZE_MAX && recv->this_field && !recv->global_qualified));
                    if (!m->is_static && !m->null_safe && graftable && (slot_receiver || (frame_receiver && !overridden))) { md = fn; }
                } else if (c->callee->get_type() == node_type::identifier_expr && own_frame && !free_callee && ov_class == class_ctx_name_) {
                    auto* id = static_cast<const identifier_expr*>(c->callee.get());
                    if (id->slot_index == SIZE_MAX && !id->global_qualified && graftable && !overridden) { md = fn; }
                }
            }
            if (proofs_ == proof_mode::stamp) { c->proven_method_decl = md ? md->weak_from_this() : std::weak_ptr<const function_decl>(); }
            else if (proofs_ == proof_mode::clear) { c->proven_method_decl.reset(); }
        }
        if (ov) {
            // a map key passed where every viable overload binds the parameter by reference
            // or as a shared_ptr handle (a by-value parameter copies)
            for (size_t i = 0; i < c->arguments.size() && i < args.size(); ++i) {
                if (!args[i].const_key) { continue; }
                bool aliasing = false, viable = false;
                for (const auto* fn : *ov) {
                    if (!script_fn_viable(fn, args, !free_callee && arity_rivals(*ov, args.size()))) { continue; }
                    viable = true;
                    aliasing = i < fn->parameters.size() &&
                               (fn->parameters[i].is_reference || handle_type(fn->parameters[i].type.get()));
                    if (!aliasing) { break; }
                }
                if (viable && aliasing) { map_key_alias(c->arguments[i]->location, c->arguments[i].get()); }
            }
            // a conditional of two arm types where every viable overload binds the parameter by non-const reference
            for (size_t i = 0; i < c->arguments.size() && i < args.size(); ++i) {
                if (!detail::conditional_names_storage(c->arguments[i].get())) { continue; }
                bool all_ref = false;
                for (const auto* fn : *ov) {
                    if (!script_fn_viable(fn, args, !free_callee && arity_rivals(*ov, args.size()))) { continue; }
                    all_ref = !fn->is_coroutine && i < fn->parameters.size() && fn->parameters[i].is_reference && !fn->parameters[i].is_const;
                    if (!all_ref) { break; }
                }
                if (all_ref) { report_mixed_reference_conditional(c->arguments[i].get()); }
            }
            // a literal where every viable overload binds the parameter by reference: a string literal binds a writable
            // temporary for a string& overload; otherwise it binds a temporary when the callee (every override of a method)
            // never writes through the parameter, else the load reports the write
            for (size_t i = 0; i < c->arguments.size() && i < args.size(); ++i) {
                if (!certainly_temporary(c->arguments[i].get()) && !rooted_in_temporary(c->arguments[i].get())) { continue; }
                std::vector<const function_decl*> by_ref;
                bool all_ref = false;
                for (const auto* fn : *ov) {
                    if (!script_fn_viable(fn, args, !free_callee && arity_rivals(*ov, args.size()))) { continue; }
                    all_ref = !fn->is_coroutine && i < fn->parameters.size() && fn->parameters[i].is_reference;   // a coroutine binds a copy
                    if (!all_ref) { break; }
                    by_ref.push_back(fn);
                }
                if (!all_ref || by_ref.empty()) { continue; }
                if (args[i].kind == ctype::string_k && std::any_of(by_ref.begin(), by_ref.end(), [i](const function_decl* fn) {
                        return fn->parameters[i].type && fn->parameters[i].type->base_type == script_value_type::jai_string_type;
                    })) {
                    continue;
                }
                if (!ov_class.empty()) {
                    for (const auto* fn : overrides_of(ov_class, by_ref.front()->name)) { by_ref.push_back(fn); }
                }
                for (const auto* fn : by_ref) {
                    if (i >= fn->parameters.size() || !fn->parameters[i].is_reference || fn->parameters[i].is_const || fn->is_coroutine) { continue; }
                    const ref_param_write w = ref_writes(fn, i);
                    if (w.writes) {
                        diag(diag_level::error, c->arguments[i]->location, "'" + std::string(fn->name) + "' writes through parameter '" +
                             fn->parameters[i].name + "' (line " + std::to_string(w.line) + "); pass a variable");
                        break;
                    }
                }
            }
        }
        return t;
    }

    // regex(pattern[, flags]) naming the engine's constructor with texts the load can see (literals, or consts declared with
    // them): an invalid pattern is the load's error, and literal texts compile here once for both backends (call_expr::regex_constant)
    void check_regex_constant(call_expr* c) {
        const size_t n = c->arguments.size();
        std::string texts[2];
        bool literal = n == 1 || n == 2;
        bool known = literal;
        for (size_t k = 0; k < n && known; ++k) {
            const expression* a = c->arguments[k].get();
            if (detail::regex_library::constant_text(a, texts[k])) { continue; }
            literal = false;
            const var_entry* v = a->get_type() == node_type::identifier_expr && !static_cast<const identifier_expr*>(a)->global_qualified
                ? find_local(static_cast<const identifier_expr*>(a)->name) : nullptr;
            known = v && v->has_const_text;
            if (known) { texts[k] = v->const_text; }
        }
        detail::regex::compiled_ptr re;
        if (known && detail::regex_library::is_constructor(eng_.get_variable("regex"))) {
            if (c->regex_constant && detail::regex::pattern_of(*c->regex_constant) == texts[0] && detail::regex::flags_of(*c->regex_constant) == texts[1]) {
                re = c->regex_constant;
            } else {
                std::string error;
                re = detail::regex::compile(texts[0], texts[1], error);
                if (!re) { diag(diag_level::error, c->arguments[0]->location, error); }
            }
        }
        if (proofs_ == proof_mode::stamp) { c->regex_constant = literal ? re : nullptr; }
        else if (proofs_ == proof_mode::clear) { c->regex_constant.reset(); }
    }

    // Top-level code runs in order: a use there of a name this file declares at program scope only further down finds nothing yet (a
    // name the engine already holds, from an earlier run of the unit or another unit, is there)
    bool top_level_code() const { return fn_stack_.empty() && !class_ctx_; }
    static bool declared_after(const source_location& decl, const source_location& at) {
        return decl.filename == at.filename && (decl.line > at.line || (decl.line == at.line && decl.column > at.column));
    }
    // held: the engine already holds the name
    template <typename Held>
    void report_use_before_declaration(std::string_view name, const source_location& at, bool call, Held&& held) {
        const auto first = program_decls_.find(name);
        if (!top_level_code() || first == program_decls_.end() || !declared_after(first->second, at) || held()) { return; }
        const char* use = call ? "call" : "use";
        diag(diag_level::error, at, "'" + std::string(name) + "' is " + (call ? "called" : "used") + " before its declaration (line " + std::to_string(first->second.line) +
                                    "): top-level code runs in order, so declare it above the " + use + " or make the " + use + " after it");
    }
    // A generic's instance is declared the moment everything it is built on is (class_registry's linker), so a use of one in
    // top-level code stands below what this file declares of that: the generic, and the classes the instance waits for (the
    // request's needs, the very names the linker waits on). The texts are a plain name's.
    void report_instance_before_declaration(std::string_view instance, const source_location& at, bool call) {
        if (!top_level_code()) { return; }
        const auto named = instance_requests_.find(instance);   // the requests of a name share its generic and what it waits for
        if (named == instance_requests_.end()) { return; }
        const generic_instance_decl* request = named->second.front();
        const std::string generic(request->generic);
        if (const auto declared = generics_.find(generic); declared != generics_.end() && !declared->second.empty() &&
            declared->second.front()->kind != generic_kind::method && declared_after(declared->second.front()->location, at) && !eng_.generic_declared(generic)) {
            const char* use = call ? "call" : "use";
            diag(diag_level::error, at, "'" + generic + "' is " + (call ? "called" : "used") + " before its declaration (line " + std::to_string(declared->second.front()->location.line) +
                                        "): top-level code runs in order, so declare it above the " + use + " or make the " + use + " after it");
        }
        for (const std::string_view need : request->needs) { report_class_before_declaration(need, at); }
    }
    void report_function_before_declaration(std::string_view name, const source_location& at, bool call) {
        report_use_before_declaration(name, at, call, [&] { return eng_.has_function(std::string(name)); });
    }
    void report_class_before_declaration(std::string_view name, const source_location& at) {
        report_use_before_declaration(name, at, false, [&] { return eng_.find_script_class(std::string(name)) != nullptr; });
    }
    // A namespace's top-level code admits its members in order, as program scope does: a bare name the namespace (space) declares
    // only further down is not its member yet, so it reaches what an enclosing namespace or the globals hold under the name, and
    // nothing there is the program-scope rule's error. True when the member is not there yet (the caller stays lenient).
    bool namespace_member_unready(const std::string& space, std::string_view name, const source_location& at, bool call) {
        if (!top_level_code()) { return false; }
        const auto first = namespace_decls_.find(space + "::" + std::string(name));
        if (first == namespace_decls_.end() || !declared_after(first->second, at)) { return false; }
        if (namespace_member_held(space, name)) { return false; }
        if (!named_outside_namespace(space, name)) {
            const char* use = call ? "call" : "use";
            diag(diag_level::error, at, "'" + std::string(name) + "' is " + (call ? "called" : "used") + " before its declaration (line " + std::to_string(first->second.line) +
                                        "): top-level code runs in order, so declare it above the " + use + " or make the " + use + " after it");
        }
        return true;
    }
    // the engine holds the member from an earlier run or a host registration
    bool namespace_member_held(const std::string& space, std::string_view name) {
        const std::string qualified = space + "::" + std::string(name);
        if (eng_.has_variable(qualified) || eng_.has_function(qualified)) { return true; }
        const script_namespace_data* ns = eng_.find_namespace(space);
        if (!ns) { return false; }
        const uint64_t id = eng_.symbolize(name);
        return ns->functions.count(id) || ns->variables.count(id) || (ns->scope && ns->scope->get_own_value_ptr(id) != nullptr);
    }
    // what a bare name reaches past the namespace: a member of an enclosing namespace, or a name of the program scope
    bool named_outside_namespace(const std::string& space, std::string_view name) {
        const std::string bare(name);
        bool result = functions_.count(name) || all_names_.count(name) || find_sclass(name) || enums_.count(name) || extra_global(bare) || engine_knows_name(bare);
        for (std::string outer = space; !result && outer.rfind("::") != std::string::npos;) {
            outer.resize(outer.rfind("::"));
            result = namespace_member_declared(outer, name);
        }
        return result;
    }
    // A bare name is its namespace's member whenever the code that names it runs, unless the name reaches something outside the
    // namespace too and the member's declaration may not have run by then (a function called from top-level code above it, a file
    // imported later): a declaration above the use has run, and so has one the engine holds. False: the caller stays lenient.
    bool namespace_member_settled(const std::string& space, std::string_view name, const source_location& at) {
        const auto first = namespace_decls_.find(space + "::" + std::string(name));
        if (first != namespace_decls_.end() && !declared_after(first->second, at)) { return true; }
        return namespace_member_held(space, name) || !named_outside_namespace(space, name);
    }

    // -------------------------------------------------------------- uses that can never succeed
    // (docs/site/guide/13-strict-checking.html "A use that can never succeed"): a call of a name declared with a type no call
    // takes, and a member or a subscript of a declared function. Each is refused only where the load knows the one thing
    // the name is: whatever may answer it another way when the line runs (a function or a class of the name anywhere in the
    // program or the engine, a member of the object a method runs on, a file the load cannot read) leaves it to the run.
    static std::string with_article(std::string_view noun) {
        const bool vowel = !noun.empty() && noun.substr(0, 4) != "utf8" && std::string_view("aeiouAEIOU").find(noun.front()) != std::string_view::npos;
        return (vowel ? "an " : "a ") + std::string(noun);
    }
    static std::string declaration_place(const source_location& declared, const source_location& use) {
        const std::string at = std::to_string(declared.line) + ":" + std::to_string(declared.column);
        return declared.filename.empty() || declared.filename == use.filename ? at : declared.filename + ":" + at;
    }
    // a declared type no call takes: a number, text, a bool, an array, a map, a math value (an enum's value is an int)
    static bool never_called(const ctype& t) {
        if (t.tentative || t.diagnostic_only) { return false; }
        switch (t.kind) {
            case ctype::int_k: case ctype::float_k: case ctype::bool_k: case ctype::char_k: case ctype::string_k: case ctype::utf8_k:
            case ctype::array_k: case ctype::map_k: case ctype::math_k: return true;
            default: return false;
        }
    }
    bool never_called(const variable_decl* g) {
        return g && g->type && !g->type->is_reference() && !g->function_spelling && never_called(ctype_from_type_info(g->type.get()));
    }
    void never_a_function(const source_location& at, std::string_view shown, std::string_view what, const std::string& where) {
        diag(diag_level::error, at, "'" + std::string(shown) + "' is " + with_article(what) + " (declared at " + where + "), not a function");
    }
    // the engine's variable of the name, if it holds one, was declared by `declarer`'s earlier run alone: what its new text replaces
    bool engine_variable_only_from(const std::string& name, const std::string& declarer) {
        if (!eng_.has_variable(name) || !eng_.held_function_declarations(name).empty()) { return true; }
        const std::vector<uint32_t> units = eng_.variable_units(name);
        if (units.empty()) { return false; }   // the host's
        const std::string normal = std::filesystem::path(declarer).lexically_normal().string();
        for (const uint32_t unit : units) { if (eng_.unit_label(unit) != normal) { return false; } }
        return true;
    }
    // Something a call of a program-scope name may reach besides the one variable or enum the load found: a function, a generic
    // or a class of the name in this unit, in another unit of the program or in the engine. `declarer` is the unit whose
    // earlier run the pooled text replaces (null: none): a function the engine holds from that run is no second answer.
    bool callable_elsewhere(const std::string& name, const std::string* declarer) {
        if (has_dynamic_includes_ || functions_.count(std::string_view(name)) || find_sclass(name) || eng_.is_type_name(name) || eng_.find_script_class(name)) { return true; }
        if (generics_live_ && !generics_named(name).empty()) { return true; }
        if (program_functions_) {
            if (const auto pooled = program_functions_->find(name); pooled != program_functions_->end()) {
                for (const auto& [path, decls] : pooled->second) { if (!decls.empty()) { return true; } }
            }
        }
        const auto held = eng_.held_function_declarations(name);
        if (held.empty()) { return eng_.has_function(name); }
        if (!declarer) { return true; }
        const std::string normal = std::filesystem::path(*declarer).lexically_normal().string();
        for (const auto& h : held) { if (!h || eng_.unit_label(h->unit.id) != normal) { return true; } }
        return false;
    }
    // how many variable declarations the program's units make under a program-scope name
    size_t program_variable_declarations(const std::string& name) {
        if (!program_globals_) { return 0; }
        eng_.note(name);
        const auto pooled = program_globals_->find(name);
        size_t declarations = 0;
        if (pooled != program_globals_->end()) { for (const auto& [path, decls] : pooled->second) { declarations += decls.size(); } }
        return declarations;
    }
    // The one variable exactly one other unit of the program declares under a program-scope or namespace name, with a spelled
    // type, and the unit that declares it; null when anything else may answer the name
    const variable_decl* program_variable(const std::string& name, const std::string*& declarer) {
        if (!collect_diagnostics_ || has_dynamic_includes_ || !program_globals_ || instance_names_.count(name) || extra_globals_.count(name)) { return nullptr; }
        eng_.note(name);
        const auto pooled = program_globals_->find(name);
        if (pooled == program_globals_->end()) { return nullptr; }
        const variable_decl* g = nullptr;
        size_t declarations = 0;
        for (const auto& [path, decls] : pooled->second) {
            declarations += decls.size();
            if (!decls.empty()) { g = decls.front().get(); declarer = &path; }
        }
        if (declarations != 1 || !g || g->is_static) { return nullptr; }
        if (callable_elsewhere(name, declarer) || !engine_variable_only_from(name, *declarer)) { return nullptr; }
        return g;
    }
    // the enum exactly one other unit of the program declares under a name, and that unit
    const enum_decl* program_enum(const std::string& name, const std::string*& declarer) {
        if (!collect_diagnostics_ || has_dynamic_includes_ || !program_enums_) { return nullptr; }
        eng_.note(name);
        const auto pooled = program_enums_->find(name);
        if (pooled == program_enums_->end() || pooled->second.size() != 1 || !pooled->second.begin()->second) { return nullptr; }
        declarer = &pooled->second.begin()->first;
        return pooled->second.begin()->second.get();
    }
    // a bare name inside a method reaches the members of the object the method runs on before the program's names, and a
    // class derived from this one may declare any of them
    bool bare_name_reaches_members(const identifier_expr* id) const { return class_ctx_ && !id->global_qualified; }
    // a call of a variable declared with a type no call takes, or of an enum a function declares
    void uncallable_variable(const var_entry& v, size_t scope_index, const identifier_expr* id) {
        // a reference names whatever it was bound to, and a var it borrows may hold a function by the time the call runs
        if (v.dynamic || v.is_reference || v.shared_name || v.namespace_function || v.placeholders) { return; }
        if (const auto space = std::find(namespace_scopes_.begin(), namespace_scopes_.end(), scope_index); space != namespace_scopes_.end()) {
            if (bare_name_reaches_members(id) || v.enum_def) { return; }
            const std::string path(namespace_walk_names_[static_cast<size_t>(space - namespace_scopes_.begin())]);
            namespace_variable_called(path, path + "::" + std::string(id->name), id->name, id->location);
            return;
        }
        if (scope_index != 0 && instance_ctx_) { return; }   // a generic's local: its type may be a type argument, and an instance may never reach the line
        std::string what;
        if (v.enum_def) { what = "enum"; }
        else if (never_called(v.type)) { what = !v.display.empty() && v.display.rfind("auto", 0) != 0 ? v.display : ctype_name(v.type); }
        else { return; }
        if (scope_index == 0) {   // the unit's program scope: a local hides every other answer, and here each of them counts
            const std::string name(id->name);
            if (bare_name_reaches_members(id) || callable_elsewhere(name, nullptr) || program_variable_declarations(name) > 1) { return; }
        }
        never_a_function(id->location, id->name, what, std::to_string(v.line) + ":" + std::to_string(v.column));
    }
    // a call of a namespace's variable, the unit's own or the one exactly one other unit declares
    bool namespace_variable_called(const std::string& path, const std::string& qualified, std::string_view member, const source_location& loc) {
        if (bare_member_reach_) { return false; }
        const variable_decl* g = nullptr;
        const std::string* declarer = nullptr;
        if (const auto own = namespace_vars_.find(qualified); own != namespace_vars_.end()) {
            g = own->second;
            if (!g || callable_elsewhere(qualified, nullptr) || program_variable_declarations(qualified) > 1) { return false; }
        } else {
            g = program_variable(qualified, declarer);
        }
        if (!never_called(g)) { return false; }
        if (script_namespace_data* space = eng_.find_namespace(path)) {   // a function the engine's namespace holds under the name answers the call
            const uint64_t member_id = eng_.symbolize(member);
            if (space->functions.count(member_id) || space->signatures.count(member_id)) { return false; }
            if (const auto held = space->variables.find(member_id); held != space->variables.end() && held->second.is_function()) { return false; }
        }
        never_a_function(loc, qualified, g->type->spelling(), declaration_place(g->location, loc));
        return true;
    }
    // Enum::value(...): a value of an enum is an int. The enum is the unit's own, which the receiver's type names, or the one
    // exactly one other unit declares, which a receiver nothing in this unit declares names; a namespace's enum stays the run's
    bool enum_value_called(const member_expr* m, const ctype& recv) {
        if (has_dynamic_includes_ || instance_ctx_ || !m->is_static || !m->object || m->object->get_type() != node_type::identifier_expr) { return false; }
        const auto* id = static_cast<const identifier_expr*>(m->object.get());
        const std::string name(id->name);
        const std::string* declarer = nullptr;
        const enum_decl* named = nullptr;
        if (recv.kind == ctype::enum_ref_k) {
            if (recv.tentative || recv.cls != id->name) { return false; }
            named = find_enum(recv.cls);
        } else if (recv.is_unknown() && !find_local(id->name) && !functions_.count(id->name) && !enums_.count(id->name)) {
            named = program_enum(name, declarer);
        }
        if (!named) { return false; }
        if (namespaces_.count(id->name) || eng_.find_namespace(name) || find_sclass(id->name) || eng_.is_type_name(name) || eng_.find_script_class(name) ||
            (program_classes_ && program_classes_->count(name))) { return false; }
        const std::string qualified = name + "::" + std::string(m->member);
        if ((program_functions_ && program_functions_->count(qualified)) || (program_globals_ && program_globals_->count(qualified))) { return false; }
        if (std::none_of(named->values.begin(), named->values.end(), [&](const auto& value) { return value.first == m->member; })) { return false; }
        never_a_function(m->location, qualified, "value of the enum '" + name + "'", declaration_place(named->location, m->location));
        return true;
    }
    // Class::name(...): a static field declared with a type no call takes
    void static_field_called(const member_expr* m, const ctype& recv, const member_hit& hit) {
        if (has_dynamic_includes_ || instance_ctx_ || !hit.certain || hit.unresolved || !hit.field || !hit.field->is_static || chain_open(recv.cls) || chain_has_user_accessor(recv.cls, m->member)) { return; }
        if (!never_called(hit.field)) { return; }
        never_a_function(m->location, std::string(recv.cls) + "::" + std::string(m->member), hit.field->type->spelling(), declaration_place(hit.field->location, m->location));
    }
    // The declared function a bare name names when nothing nearer answers the name: the unit's own, which no other unit adds
    // an overload to, else the one exactly one other unit declares
    const function_decl* named_function(const expression* e) {
        if (!e || has_dynamic_includes_ || e->get_type() != node_type::identifier_expr) { return nullptr; }
        const auto* id = static_cast<const identifier_expr*>(e);
        size_t scope_index = 0;
        if (callable_variable(id, &scope_index) || bare_name_reaches_members(id)) { return nullptr; }
        if (!id->global_qualified && !enclosing_namespace_member(id->name).empty()) { return nullptr; }
        if (const auto own = functions_.find(id->name); own != functions_.end()) {
            return !own->second.empty() && foreign_overloads(id->name, own->second).empty() ? own->second.front() : nullptr;
        }
        return program_function(id->name);
    }
    // name.member where the name is a declared function; true when it was reported
    bool function_member(const member_expr* m, const ctype& recv) {
        if (recv.kind != ctype::function_k || m->is_static) { return false; }
        const function_decl* fn = named_function(m->object.get());
        if (!fn) { return false; }
        diag(diag_level::error, m->location, "'" + std::string(fn->name) + "' is a function (declared at " + declaration_place(fn->location, m->location) +
             "): it has no member '" + std::string(m->member) + "'");
        return true;
    }
    // name[key] where the name is a declared function
    void function_subscripted(const binary_expr* b, const ctype& left) {
        if (left.kind != ctype::function_k) { return; }
        const function_decl* fn = named_function(b->left.get());
        if (!fn) { return; }
        const source_location& at = b->left->location;
        diag(diag_level::error, at, "'" + std::string(fn->name) + "' is a function (declared at " + declaration_place(fn->location, at) + "): it cannot be subscripted");
    }
    bool bare_member_reach_ = false;   // the namespace member a call names was written bare inside a method (bare_name_reaches_members)

    ctype infer_call_impl(call_expr* c, const std::vector<ctype>& args, const std::vector<const function_decl*>*& ov, narrow_callee& seen) {
        expression* callee = c->callee.get();
        if (callee->get_type() == node_type::identifier_expr) {
            auto* id = static_cast<identifier_expr*>(callee);
            if (id->name == "weak_from_this" || id->name == "shared_from_this") { return {}; }   // call-shape builtins, resolved before any lookup
            stamp(id->program_name, semantic_walk() && bare_program_name(id), false);
            size_t scope_index = 0;
            if (const var_entry* local = callable_variable(id, &scope_index)) {
                if (scope_index == 0) { note_global_use(id->name); }
                if (local->lambda && script_callee_takes_type_arguments(id->name, "lambda")) { return {}; }
                if (!local->dynamic && signature_slot(local->type)) { return check_signature_call("'" + std::string(id->name) + "'", local->type.ti, c, args); }
                if (const std::string* reason = lambda_variable_returns_nothing(id)) { void_hit_ = {"'" + std::string(id->name) + "'", {}, nullptr, *reason}; }
                uncallable_variable(*local, scope_index, id);
                return {};   // a callable local without a signature
            }
            if (class_ctx_ && !id->global_qualified) {
                member_hit hit = lookup_member(class_ctx_name_, id->name, /*want_static=*/false);
                if (hit.found) {
                    if (access_denied(hit, id->name, id->location)) { return {}; }
                    if (hit.methods) {
                        if (hit.provable) { ov = hit.methods; ov_class_ = class_ctx_name_; }
                        hidden_function_probe_ = true;
                        ctype r = check_script_overloads(id->name, *hit.methods, args, c->location, hit.foreign, true, "method").result;
                        hidden_function_probe_ = false;
                        if (!nodiscard_hit_.name.empty()) { nodiscard_hit_.name = std::string(class_ctx_name_) + "." + nodiscard_hit_.name; }
                        if (!void_hit_.shown.empty()) {
                            const bool statics = std::all_of(hit.methods->begin(), hit.methods->end(), [](const function_decl* fn) { return fn->is_static; });
                            if (statics) { void_hit_.shown = "'" + std::string(class_ctx_name_) + "." + std::string(method_spelling(id->name)) + "'"; }
                            else { void_hit_ = {}; }
                        }
                        if (!hit.provable) { r.tentative = true; }
                        return r;
                    }
                    if (hit.field && hit.field->type) {
                        if (const ctype held = member_type(hit); signature_slot(held)) { return check_signature_call("'" + std::string(id->name) + "'", held.ti, c, args); }
                    }
                    return {};
                }
                if (hit.unresolved) { return {}; }
            }
            if (!id->global_qualified) {
                if (const std::string space = enclosing_namespace_member(id->name); !space.empty()) {
                    if (namespace_member_unready(space, id->name, c->location, true) || !namespace_member_settled(space, id->name, c->location)) { return {}; }
                    struct reach { bool& flag; bool outer; ~reach() { flag = outer; } } bare{bare_member_reach_, bare_member_reach_};
                    bare_member_reach_ = bare_name_reaches_members(id);
                    return check_namespace_call(space, id->name, args, c->location, seen, c);
                }
            }
            if (auto it = functions_.find(id->name); it != functions_.end()) {
                note_global_use(id->name);
                report_function_before_declaration(id->name, c->location, true);
                report_instance_before_declaration(id->name, c->location, true);
                if (const auto& foreign = foreign_overloads(id->name, it->second); !foreign.empty()) {
                    return check_program_overloads(id->name, it->second, foreign, args, c->location);
                }
                ov = &it->second;
                return check_script_overloads(id->name, it->second, args, c->location).result;
            }
            if (const sclass* sc = find_sclass(id->name)) {
                if (script_callee_takes_type_arguments(id->name, "class")) { return {}; }
                report_class_before_declaration(id->name, c->location);
                report_instance_before_declaration(id->name, c->location, false);
                const bool may_copy = check_ctor_call(id->name, *sc, args, c->location);
                if (sc->provable && !may_copy) { ov = &sc->ctors; }
                return object_of(id->name);
            }
            std::string name(id->name);
            if (const auto named = enums_.find(id->name); named != enums_.end()) {
                if (!bare_name_reaches_members(id) && !callable_elsewhere(name, nullptr)) { never_a_function(id->location, id->name, "enum", declaration_place(named->second->location, id->location)); }
                return {};
            }
            if (const function_decl* declared = program_function(id->name)) {
                note_global_use(id->name);
                const std::vector<const function_decl*> one{declared};
                ctype r = check_script_overloads(id->name, one, args, c->location, /*foreign=*/true, /*scored=*/false).result;
                r.tentative = true;
                return r;
            }
            if (!bare_name_reaches_members(id)) {
                const std::string* declarer = nullptr;
                if (const variable_decl* g = program_variable(name, declarer); never_called(g)) {
                    note_global_use(id->name);
                    never_a_function(id->location, id->name, g->type->spelling(), declaration_place(g->location, id->location));
                    return {};
                }
                if (const enum_decl* named = program_enum(name, declarer); named && !callable_elsewhere(name, declarer)) {
                    never_a_function(id->location, id->name, "enum", declaration_place(named->location, id->location));
                    return {};
                }
            }
            if (extra_global(name)) { note_global_use(id->name); return {}; }
            if (name == "regex") { check_regex_constant(c); }
            if (eng_.is_type_name(name) || eng_.find_script_class(name)) {
                note_host_nodiscard(name);
                check_strict_host_constructor(name, args, c->location);
                return object_of(id->name);   // registered class ctor: args lenient unless the host class is strict
            }
            if (eng_.has_function(name)) {
                seen.host = id->name;
                note_global_use(id->name);
                note_host_nodiscard(name);
                check_constant_arguments(name, name, c);
                if (name == "sleep" && core_sleep_bound()) {
                    check_sleep_call(args, c->location);
                    return ctype::make(ctype::null_k);
                }
                check_host_call(name, args, c->location, c);
                note_host_void(name);
                check_held_script_call(id->name, args, c->location);
                if (name == "parallel_reduce") { check_reduce_callbacks(c); }
                // the engine's parallel forms read their callbacks' results as array::map and reduce do, but a step that takes its
                // accumulator by reference folds in place (void step(T& acc, x))
                if (name == "parallel_transform" && c->arguments.size() >= 2) { callback_result_read(name, "a value", c->arguments[1].get()); }
                if (name == "parallel_reduce") {
                    if (c->arguments.size() >= 3 && !callback_takes_first_by_reference(c->arguments[2].get())) { callback_result_read(name, "a value", c->arguments[2].get()); }
                    if (c->arguments.size() >= 4) { callback_result_read(name, "a value", c->arguments[3].get()); }
                }
                if (name == "format") { check_format_text(c, args); }
                if (name == "print" || name == "format") {
                    check_format_arguments(c, name, args);
                    if (lw_on() && formatter_bound(name)) { lw_formatter_calls_.insert(c); }
                }
                // a stdlib math alias yields the builtin's float wherever its name binds the builtin: a proof every consumer guards at run time
                if (args.size() == 1 && eng_.math_alias_fn(id->symbol_id) < engine_access::k_math_alias_rows) { return ctype::make(ctype::float_k); }
                return host_result_type(name);
            }
            if (eng_.has_variable(name)) { note_global_use(id->name); return {}; }
            if (engine_knows_name(name)) { return {}; }
            if (!id->global_qualified && report_cdtor_call(id->name, id->location)) { return {}; }
            if (program_or_derived_name(id->name)) { return {}; }
            if (stale_field_context()) { return {}; }
            undeclared(id->location, id->name, id->less_compared);
            return {};
        }
        if (callee->get_type() == node_type::member_expr) {
            auto* m = static_cast<member_expr*>(callee);
            if (m->is_static) {
                // math:: intrinsics take the kernel lane (same shape gate as both backends)
                const math_fn fn = math_intrinsic_for_call(m);
                if ((fn == math_fn::cstore_ || fn == math_fn::vcomp_) && !c->arguments.empty()) {
                    // the lowering's steps: the component type follows from the literal kind (cstore$) or code (vcomp$)
                    const expression* lit = c->arguments[fn == math_fn::cstore_ ? c->arguments.size() - 1 : 0].get();
                    if (lit->get_type() == node_type::literal_expr && static_cast<const literal_expr*>(lit)->value.raw_storage_index() == script_value::TYPEID_INT) {
                        const script_int code = static_cast<const literal_expr*>(lit)->value.unchecked_as_int();
                        const bool integral = fn == math_fn::cstore_ ? detail::math::integral(static_cast<script_value_type>(code)) : (code & detail::k_vcomp_exact) != 0;
                        return ctype::make(integral ? ctype::int_k : ctype::float_k);
                    }
                }
                if (fn != math_fn::none && !detail::math_arity_accepts(fn, args.size())) { return {}; }   // the kernel raises the table's text
                if (fn != math_fn::none && args.size() <= 8) {
                    seen.copies = true;
                    checkMathShapes(m, c, args);
                    return math_intrinsic_result(fn, args);
                }
                if (ctype t; library_namespace_call(c, m, args, t)) { return t; }
                if (namespace_path_receiver(m) && !namespace_class_receiver(m)) {
                    check_namespace_member(m);
                    return check_namespace_entry(m, args, c->location, seen, c);
                }
            }
            ctype recv = declared_container_receiver(m->object.get(), infer(m->object.get()));
            if (function_member(m, recv)) { return {}; }
            if (!m->is_static && recv.kind == ctype::object_k && !recv.cls.empty() && eng_.has_constant_arguments()) {
                check_constant_arguments(std::string(recv.cls) + "." + std::string(m->member), std::string(m->member), c);
            }
            if (recv.kind == ctype::math_k && !m->is_static) {
                // a vector's method: the free function with the receiver first (detail::vec_method_fn); anything else is the
                // runtime's no-member text
                const script_value_type kind = math_type_from_name(recv.cls);
                const math_fn fn = detail::vec_method_fn(kind, m->member);
                if (fn == math_fn::none) {
                    if (!recv.tentative) { diag(diag_level::error, m->location, std::string(recv.cls) + " has no member '" + std::string(m->member) + "'"); }
                    return {};
                }
                if (args.size() != detail::vec_method_arity(fn, kind)) {
                    if (!recv.tentative) { diag(diag_level::error, c->location, detail::vec_method_arity_text(fn, kind)); }
                    return {};
                }
                std::vector<ctype> full;
                full.reserve(args.size() + 1);
                full.push_back(recv);
                full.insert(full.end(), args.begin(), args.end());
                return math_intrinsic_result(fn, full);
            }
            if (recv.kind == ctype::coroutine_k && !m->is_static) { return coroutine_method_call(recv, m, c, args); }
            if (ctype t; binary_member_call(c, m, recv, args, t)) { return t; }
            if ((m->member == "push" && c->arguments.size() == 1) || (m->member == "insert" && c->arguments.size() == 2)) {
                note_this_into_declared_slot(c->arguments.back().get(), m->object.get(), true);
            }
            if (recv.kind == ctype::array_k && recv.ti && m->member == "insert" && args.size() == 2) {
                note_this_into_handle(args[1], recv.ti->element_type().get());
                const auto issue = container_element_issue(args[1], recv.ti->element_type().get());
                if (!issue.empty()) { diag(diag_level::error, c->location, "cannot insert into '" + ctype_name(recv) + "': " + issue); }
            }
            if (recv.kind == ctype::array_k && recv.ti && m->member == "push" && args.size() == 1) {
                note_this_into_handle(args[0], recv.ti->element_type().get());
                const auto issue = container_element_issue(args[0], recv.ti->element_type().get());
                if (!issue.empty()) {
                    diag(diag_level::error, c->location, "cannot push into '" + ctype_name(recv) + "': " + issue);
                }
            }
            if (recv.kind == ctype::array_k && !m->is_static) { check_array_sizing_arguments(m->member, recv, args.data(), args.size(), c->location); }
            if (recv.const_key && string_mutator_name(m->member)) { map_key_mutation(m->location, m->object.get()); }
            if (class_dot_receiver(m, recv)) { return {}; }
            if (recv.kind == ctype::object_k || recv.kind == ctype::class_ref_k) {
                if (recv.declared_slot) { return read_through_declared_slot(class_member_call(c, m, recv, args, nullptr)); }
                return class_member_call(c, m, recv, args, &ov);
            }
            if (recv.is_unknown() && !m->is_static) {
                if (const ctype held = referent_receiver(m->object.get()); held.kind == ctype::object_k) { class_member_call(c, m, held, args, nullptr); }
            }
            if (recv.kind == ctype::enum_ref_k) {
                enum_value_called(m, recv);
                return {};
            }
            if (recv.is_unknown() && enum_value_called(m, recv)) { return {}; }
            uint8_t proven = 0;
            if (!recv.tentative || recv.element_proof) {
                proven = recv.kind == ctype::string_k ? 1 : recv.kind == ctype::array_k ? 2 : recv.kind == ctype::map_k ? 3 : 0;
            }
            if (proven == 0 && recv.is_unknown() && m->object->get_type() == node_type::identifier_expr) {
                if (const var_entry* v = find_local(static_cast<const identifier_expr*>(m->object.get())->name)) {
                    proven = v->builtin_alias == ctype::string_k ? 1 : v->builtin_alias == ctype::array_k ? 2 : v->builtin_alias == ctype::map_k ? 3 : 0;
                }
            }
            seen.copies = proven != 0;   // the library's string, array and map methods
            if (kind_certain(recv) && !m->is_static) {
                scalar_member_missing(recv, m);
                check_builtin_member(recv, m);
                checkBuiltinArity(recv, m, c);
                checkBuiltinMethodShapes(recv, m, c, args);
                in_place_result_used(recv, m, c);
                call_result_mutation(m, c);
                if (recv.kind == ctype::array_k || recv.kind == ctype::map_k) {
                    if (const auto wants = callback_result_wanted(recv.kind == ctype::array_k ? 0 : 1, m->member); wants && !c->arguments.empty()) {
                        callback_result_read(m->member, *wants, c->arguments[0].get());
                    }
                }
            }
            stamp(c->builtin_receiver, proven, uint8_t(0));
            stamp(c->builtin_receiver_open, uint8_t(!proven && (recv.kind == ctype::unknown || recv.kind == ctype::string_k || recv.kind == ctype::array_k || recv.kind == ctype::map_k)), uint8_t(0));
            {
                std::vector<proof_global> deps;
                auto* rid = m->object && m->object->get_type() == node_type::identifier_expr ? static_cast<identifier_expr*>(m->object.get()) : nullptr;
                const bool certain = stamping() && proven && rid && rid->slot_index == SIZE_MAX && certain_global(rid, deps);
                c->receiver_global = certain ? deps.back() : proof_global{};
            }
            if (proven && c->arguments.empty() && (m->member == "size" || m->member == "length")) { return ctype::make(ctype::int_k); }   // every kind's size is its count
            if (recv.kind == ctype::array_k && !recv.tentative) {
                if (ctype t; array_helper_result(recv, m->member, c, args, t)) { return t; }
            }
            if (proven) {
                for (auto& a : c->arguments) {   // a builtin reads its arguments as values: a subscript argument needs no element reference
                    if (a->get_type() == node_type::binary_expr && static_cast<binary_expr*>(a.get())->op.type == token_type::left_bracket) {
                        stamp(static_cast<binary_expr*>(a.get())->transient_element_read, true, false);
                    }
                }
            }
            if (recv.declared_slot && !m->is_static) { return read_through_declared_slot(builtin_result(recv, m, args)); }
            return kind_stated(recv) && !m->is_static ? builtin_result(recv, m, args) : ctype{};   // builtin container/string methods etc.
        }
        if (const ctype held = infer(callee); signature_slot(held)) { return check_signature_call("the callee", held.ti, c, args); }
        return {};
    }

    static bool signature_slot(const ctype& t) { return t.kind == ctype::function_k && t.ti && t.ti->is_signature(); }

    // p.reset(h) as the run time judges it, where the load can see it: one argument, a handle (a value or a primitive is not), of a
    // class a typed shared_ptr<T> receiver admits
    void check_handle_reset(call_expr* c, member_expr* m, const ctype& recv, const std::vector<ctype>& args) {
        if (c->arguments.size() > 1) {
            diag(diag_level::error, c->location, "reset() takes at most one argument");
            return;
        }
        const ctype& a = args.empty() ? ctype{} : args[0];
        const expression* arg = c->arguments[0].get();
        std::string not_handle;
        switch (a.kind) {
            case ctype::int_k: case ctype::float_k: case ctype::string_k: case ctype::utf8_k: case ctype::char_k: case ctype::bool_k:
            case ctype::array_k: case ctype::map_k: case ctype::function_k: case ctype::math_k:
                not_handle = ctype_name(a);
                break;
            default: break;
        }
        if (not_handle.empty() && a.kind == ctype::object_k && !a.cls.empty() && arg->get_type() == node_type::call_expr) {
            const expression* callee = static_cast<const call_expr*>(arg)->callee.get();
            if (callee && callee->get_type() == node_type::identifier_expr && static_cast<const identifier_expr*>(callee)->name == a.cls) {
                not_handle = std::string(a.cls);   // a constructor call is a value
            }
        }
        if (!not_handle.empty()) {
            diag(diag_level::error, arg->location, "reset(h) takes a handle (new T(), make_shared<T>() or a shared_ptr), not " + not_handle);
            return;
        }
        if (arg->get_type() != node_type::new_expr || a.kind != ctype::object_k || a.cls.empty() || recv.cls.empty()) { return; }
        if (m->object->get_type() != node_type::identifier_expr) { return; }
        const var_entry* local = find_local(static_cast<const identifier_expr*>(m->object.get())->name);
        if (!local || !local->is_handle || local->dynamic || local->is_reference) { return; }
        if (a.cls != recv.cls && find_sclass(a.cls) && find_sclass(recv.cls) && !class_is_or_derives(a.cls, recv.cls)) {
            diag(diag_level::error, arg->location, "Cannot reset shared_ptr<" + std::string(recv.cls) + "> to a shared_ptr<" + std::string(a.cls) + ">");
        }
    }

    // A method's name as a call spells it: a generic method's instance declares its class in full (C::g<int>), which a call omits
    static std::string_view method_spelling(std::string_view name) {
        const size_t scope = name.substr(0, name.find('<')).rfind("::");
        return scope == std::string_view::npos ? name : name.substr(scope + 2);
    }
    // recv.m(args) on a class or a class used as a value; ov (null for a reference's referent: diagnostics only) takes the overloads
    // a proof may rest on
    ctype class_member_call(call_expr* c, member_expr* m, const ctype& recv, const std::vector<ctype>& args,
                            const std::vector<const function_decl*>** ov) {
        bool want_static = (recv.kind == ctype::class_ref_k);
        member_hit hit = lookup_member(recv.cls, m->member, want_static);
        if (!hit.found && !want_static && chain_has_user_accessor(recv.cls, m->member)) { return {}; }   // calling what a user _get_ returns
        if (!want_static && static_through_instance(m, recv, hit, true)) { return {}; }
        if (hit.found) {
            if (hit.methods) {
                if (hit.provable && ov) { *ov = hit.methods; ov_class_ = recv.cls; }
                ctype result = check_script_overloads(m->member, *hit.methods, args, c->location, hit.foreign, true, "method").result;
                if (!nodiscard_hit_.name.empty()) { nodiscard_hit_.name = std::string(recv.cls) + "." + nodiscard_hit_.name; }
                // a method dispatches on the object, so only a static's result is known (an override may give a value)
                if (!void_hit_.shown.empty()) {
                    if (want_static) { void_hit_.shown = "'" + std::string(recv.cls) + "." + std::string(method_spelling(m->member)) + "'"; }
                    else { void_hit_ = {}; }
                }
                if (!hit.provable) { result.tentative = true; }
                return result;
            }
            if (hit.field && hit.field->type) {
                if (const ctype held = member_type(hit); signature_slot(held)) { return check_signature_call("'" + std::string(m->member) + "'", held.ti, c, args); }
                if (want_static) { static_field_called(m, recv, hit); }
            }
            note_host_nodiscard(std::string(recv.cls) + "." + std::string(m->member));
            if (type_argument_call_ && (find_sclass(recv.cls) || eng_.find_script_class(std::string(recv.cls)))) { type_argument_host_callee_.clear(); }   // a script class's field: no host method
            if (!want_static) { return check_strict_host_method(recv.cls, m->member, args, c->location, c, m); }
            check_host_static_arity(recv.cls, m->member, args.size(), c->location);
            return {};
        }
        if (!want_static) {
            if (ctype t; handle_builtin_result(m->member, t)) {
                if (m->member == "reset") { narrow_end_roots(m->object.get(), /*escape=*/false); }   // the handle names another object, or none
                if (m->member == "reset" && !c->arguments.empty()) { check_handle_reset(c, m, recv, args); }
                if (recv.ti && recv.ti->base_type == script_value_type::jai_shared_ptr_type && !recv.tentative) { in_place_result_used(recv, m, c); }
                return t;
            }
        }
        if (stale_shape_receiver(recv, m->object.get())) { return {}; }
        if (hit.certain && !want_static && m->object->get_type() == node_type::this_expr && recv.cls == class_ctx_name_ &&
            report_cdtor_call(m->member, m->location)) {
            return {};
        }
        if (hit.certain && !recv.open) {
            declared_at declared;
            if (auto it = classes_.find(recv.cls); it != classes_.end()) { declared = it->second.decl->location; }
            diag(diag_level::error, m->location,
                 std::string(want_static ? "no static member '" : "no member '") + std::string(m->member) +
                 "' in class '" + std::string(recv.cls) + "'", declared);
        }
        return {};
    }

    // -------------------------------------------------------------- assignment

    ctype infer_assignment(assignment_expr* a) {
        uint8_t store_proof = proof_none;
        check_const_store(a->const_check, a->target.get(), a->location);
        ctype t = infer_assignment_impl(a, store_proof);
        stamp(a->static_store_proof, store_proof, proof_none);
        if (store_proof != proof_none && stamping()) { ++counts_.stores; }
        return t;
    }

    ctype infer_assignment_impl(assignment_expr* a, uint8_t& store_proof) {
        if (a->op.type == token_type::equal) { note_this_into_declared_slot(a->value.get(), a->target.get(), false); }
        if (a->bare_condition) {
            diag(diag_level::error, a->location, "assignment used as a condition: write '==' to compare, or '((a = b))' with parentheses of its own to assign");
        }
        const type_info* slot = a->op.type == token_type::equal && contextual_literal(a->value.get()) ? peek_declared_type(a->target.get()) : nullptr;
        ctype rhs = infer_expecting(a->value.get(), slot);
        // a plain store of a string-valued right side: the compiler's text-builder store (`s = s + ...`) takes it, behind its run-time guards
        if (a->op.type == token_type::equal) {
            stamp(a->static_operand_class, rhs.kind == ctype::string_k ? operand_class_string_string : operand_class_unknown, operand_class_unknown);
        }
        struct incdec_scope { const char*& slot; const char* prev; ~incdec_scope() { slot = prev; } } incdec_guard{incdec_op_, incdec_op_};
        incdec_op_ = !a->incdec_desugar ? nullptr : a->op.type == token_type::plus_equal ? "++" : "--";
        const bool enclosing_postfix = std::exchange(incdec_postfix_, a->incdec_desugar && a->postfix_old_result);
        struct postfix_scope { bool& slot; bool prev; ~postfix_scope() { slot = prev; } } postfix_guard{incdec_postfix_, enclosing_postfix};
        bool compound = (a->op.type != token_type::equal);
        token_type base_op = compound_base(a->op.type);

        expression* target = a->target.get();
        if (target->get_type() == node_type::identifier_expr) {
            auto* id = static_cast<identifier_expr*>(target);
            bool this_field = false;
            auto done = [&](ctype t) { stamp(id->this_field, this_field, false); return t; };
            stamp(id->program_name, semantic_walk() && bare_program_name(id), false);
            size_t scope_index = 0;
            if (var_entry* local = find_named(id, &scope_index)) {
                if (scope_index == 0) { note_global_use(id->name); }   // top-level var write
                narrow_end(*local, scope_index, /*escape=*/false);
                if (rhs.const_key && local->is_handle && !compound) { map_key_alias(a->location, a->value.get()); return done(rhs); }
                if (const type_info* referent = !local->is_reference || !local->declared ? nullptr
                                                : local->declared->is_reference() ? local->declared->element_type().get() : local->declared) {
                    // a T& stores by T's table, whatever it binds: a value the table refuses is refused where the checker sees both
                    const ctype slot = ctype_from_type_info(referent);
                    if (reference_store_checked(slot) && !rhs.is_unknown() && !rhs.tentative) {
                        check_store(rhs, compound, base_op, slot, referent->canonical_name() + "&",
                                    "'" + std::string(id->name) + "'", a->location, declared_at{local->line, local->column});
                    }
                }
                if (local->reference_parameter && stamping()) {
                    const ctype stored = compound ? binary_result(ctype::make(ctype::int_k), base_op, rhs) : rhs;
                    if (stored.kind == ctype::float_k) { local->reference_parameter->ref_store_unproven = true; }
                }
                if (local->dynamic) { return done(rhs); }
                const auto space = std::find(namespace_scopes_.begin(), namespace_scopes_.end(), scope_index);
                const std::string spelled = space == namespace_scopes_.end() ? std::string(id->name)
                    : std::string(namespace_walk_names_[static_cast<size_t>(space - namespace_scopes_.begin())]) + "::" + std::string(id->name);
                check_store(rhs, compound, base_op, local->type, local->display, "'" + spelled + "'", a->location, declared_at{local->line, local->column});
                if (!compound && same_type(local->type, rhs)) { store_proof = proof_same_type; }
                if (compound) { stamp(a->static_operand_class, operand_class_of(local->type, rhs), operand_class_unknown); }   // the fused compound's typed row
                return done(local->type);
            }
            if (class_ctx_ && !id->global_qualified) {
                member_hit hit = lookup_member(class_ctx_name_, id->name, /*want_static=*/false);
                if (hit.found) {
                    if (access_denied(hit, id->name, id->location)) { return done({}); }
                    if (hit.read_only) { read_only_store(a->location, id->name); return done({}); }
                    if (hit.field && !hit.field->is_static && !hit.methods && bare_field_context() && !chain_has_user_accessor(class_ctx_name_, id->name)) { this_field = true; }
                    if (hit.field && (hit.field->type || hit.field->function_spelling)) {
                        ctype ft = member_type(hit);
                        if (!ft.is_unknown()) {
                            check_store(rhs, compound, base_op, ft, declared_variable_type_name(hit.field),
                                        "field '" + std::string(id->name) + "'", a->location,
                                        hit.foreign ? declared_at{} : declared_at{hit.field->location});
                        }
                        return done(ft);
                    }
                    return done({});
                }
                if (hit.unresolved) { return done(rhs); }
            }
            if (!id->global_qualified) {
                if (const std::string space = enclosing_namespace_member(id->name); !space.empty()) {
                    // a namespace's top-level code stores to what the name reaches until the member's declaration has run
                    const auto first = top_level_code() ? namespace_decls_.find(space + "::" + std::string(id->name)) : namespace_decls_.end();
                    if ((first != namespace_decls_.end() && declared_after(first->second, a->location)) || !namespace_member_settled(space, id->name, a->location)) { return done(rhs); }
                    const ctype declared = check_namespace_variable_store(space, id->name, rhs, compound, base_op, a->location);
                    return done(declared.is_unknown() ? rhs : declared);
                }
            }
            std::string name(id->name);
            if (extra_global(name)) {
                note_global_use(id->name);
                ctype gt = program_scope_store_type(name);
                if (gt.is_unknown()) { return done(rhs); }
                check_store(rhs, compound, base_op, gt, ctype_name(gt), "'" + name + "'", a->location, {});
                return done(gt);
            }
            if (functions_.count(id->name) || find_sclass(id->name) || enums_.count(id->name) ||
                namespaces_.count(id->name)) {
                return done(rhs);
            }
            if (eng_.has_variable(name)) {
                note_global_use(id->name);
                ctype gt = engine_global_type(name);
                if (!gt.is_unknown()) {
                    check_store(rhs, compound, base_op, gt, ctype_name(gt),
                                "'" + std::string(id->name) + "'", a->location, {});
                }
                return done(gt);   // engine globals: never proven (S3)
            }
            if (eng_.has_function(name) || engine_knows_name(name) || program_or_derived_name(id->name) || stale_field_context()) {
                return done(rhs);
            }
            undeclared(id->location, id->name, id->less_compared);
            return done(rhs);
        }
        if (target->get_type() == node_type::member_expr) {
            const variable_decl* typed_field = nullptr;
            member_hit hit;
            auto* m = static_cast<member_expr*>(target);
            if (std::string_view pair_name = map_key_operand(m); !pair_name.empty()) {
                map_key_mutation(a->location, pair_name);
                return rhs;
            }
            ctype recv;
            const ctype read_type = infer_member(m, /*writing=*/true, &typed_field, &recv, &hit);
            // a shared_ptr<T> namespace variable admits a handle by its static pointee at load; other namespace stores
            // stay run-time checks under strict (spec: a_typed_namespace_store_is_checked_at_run_time_under_strict)
            if (!typed_field && m->is_static) {
                if (const std::string path = member_path(m); !path.empty()) {
                    if (const ctype nt = check_namespace_variable_store(path, m->member, rhs, compound, base_op, a->location); !nt.is_unknown()) { return nt; }
                }
                if (const var_entry* x = qualified_extern(m); x && !x->dynamic) {
                    check_store(rhs, compound, base_op, x->type, x->display, "'" + qualified_spelling(m) + "'", a->location, declared_at{x->line, x->column});
                    return x->type;
                }
            }
            // An assignment expression's value is the destination's stored value:
            // the member's type when the checker knows it; for a member it cannot
            // read (no field, no getter) the runtime hands back the rhs or the compound's result.
            // Keep assignment results' existing non-key provenance.
            ctype result_type = !read_type.is_unknown() ? read_type : compound ? binary_result(read_type, base_op, rhs) : rhs;
            result_type.const_key = false;
            if (hit.found && hit.read_only && recv.kind == ctype::object_k && !m->is_static) {
                read_only_store(a->location, m->member);
                return rhs;
            }
            if (typed_field && (typed_field->type || typed_field->function_spelling)) {
                ctype ft = member_type(hit);
                if (!ft.is_unknown()) {
                    check_store(rhs, compound, base_op, ft, declared_variable_type_name(typed_field),
                                "field '" + std::string(typed_field->name) + "'", a->location,
                                hit.foreign ? declared_at{} : declared_at{typed_field->location});
                }
                return recv.diagnostic_only ? result_type : ft;
            }
            if (recv.kind == ctype::object_k && !m->is_static) { check_accessor_store(recv.cls, m->member, rhs, compound, base_op, a->location); }
            return result_type;
        }
        if (target->get_type() == node_type::binary_expr) {
            auto* subscript = static_cast<binary_expr*>(target);
            if (subscript->op.type == token_type::left_bracket) {
                const ctype stored_in = infer(subscript->left.get());
                function_subscripted(subscript, stored_in);
                const ctype recv = declared_container_receiver(subscript->left.get(), stored_in);
                const ctype key = infer(subscript->right.get());
                if (recv.kind == ctype::map_k && !recv.tentative && key.kind == ctype::object_k && !key.tentative && !key.cls.empty()) { warn_identity_map_key(subscript, key.cls); }
                check_map_subscript_key(subscript, recv, key);
                stamp(subscript->static_operand_class, operand_class_unknown, operand_class_unknown);
                if (const expression* base = lw_strip(subscript->left.get()); base->get_type() == node_type::call_expr &&
                    by_value_container_result(static_cast<const call_expr*>(base))) {
                    non_lvalue_target(a->location, base);   // an element of a container no storage holds: the store raises when it runs
                    return rhs;
                }
                if (recv.kind == ctype::object_k && !recv.tentative && subscript_returns_by_value(recv.cls)) {
                    std::string text(detail::by_value_subscript_store_text);
                    text[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[0])));
                    diag(diag_level::error, a->location, text);
                    return rhs;
                }
                if (recv.kind == ctype::string_k || recv.kind == ctype::utf8_k) {
                    // a string character takes a char or a string (its text replaces the character)
                    const ctype character = ctype::make(recv.kind == ctype::string_k ? ctype::char_k : ctype::utf8_k);
                    const ctype stored = compound ? binary_result(character, base_op, rhs) : rhs;
                    if (!stored.is_unknown() && stored.kind != ctype::char_k && stored.kind != ctype::string_k && stored.kind != ctype::utf8_k) {
                        diag(diag_level::error, a->location, "cannot store into '" + ctype_name(recv) +
                             "': a string character takes a char or a string, got '" + ctype_name(stored) + "'");
                    }
                    return stored;
                }
                const ctype element = container_value_type(recv);
                if (compound && check_compound_operands(element, base_op, rhs, a->location, {})) { return element; }
                const ctype stored = compound ? binary_result(element, base_op, rhs) : rhs;
                const bool assigns = !compound && element.kind == ctype::object_k && !element.cls.empty() && assign_operator_accepts(element.cls, stored);
                const auto issue = assigns ? std::string() : container_element_issue(stored, element.ti);
                if (!issue.empty()) {
                    diag(diag_level::error, a->location, "cannot store into '" + ctype_name(recv) + "': " + issue);
                }
                return element.is_unknown() ? stored : element;
            }
        }
        if (detail::reference_target(target)) {
            narrow_end_roots(target, /*escape=*/true);
            // cast<T&>(e) (its operand already is a T), f() returning T&, a conditional of lvalues, an assignment: the store
            // checks against the referent's type; a call the checker resolves to by-value overloads only names no storage
            if (target->get_type() == node_type::call_expr && call_returns_by_value(static_cast<call_expr*>(target))) {
                non_lvalue_target(a->location, target);
                infer(target);
                return rhs;
            }
            ctype t = infer(target);
            if (t.is_unknown()) { return rhs; }
            t.tentative = false;
            const std::string name = ctype_name(t);
            const bool spelled_cast = target->get_type() == node_type::cast_expr && !static_cast<cast_expr*>(target)->implicit_ref;
            check_store(rhs, compound, base_op, t, name, spelled_cast ? "a cast<" + name + "&> target" : std::string("the reference"), a->location, {});
            return t;
        }
        non_lvalue_target(a->location, target);
        infer(target);
        return rhs;
    }

    void non_lvalue_target(const source_location& loc, const expression* target) {
        std::string text = detail::non_lvalue_target_text(target);
        if (!text.empty()) { text[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[0]))); }
        diag(diag_level::error, loc, text);
    }

    // `C.s` on a class name (the runtime's text, class_dot_member_text): reported, true
    bool class_dot_receiver(const member_expr* m, const ctype& recv) {
        if (recv.kind != ctype::class_ref_k || m->is_static || !m->object || m->object->get_type() != node_type::identifier_expr) { return false; }
        diag(diag_level::error, m->location, class_dot_member_text(recv.cls, m->member));
        return true;
    }

    // The class's operator[] (its chain's, and every override a derived receiver could reach) returns by value only
    bool subscript_returns_by_value(std::string_view cls) {
        member_hit hit = lookup_member(cls, "[]", /*want_static=*/false);
        if (!hit.found || !hit.methods || hit.methods->empty()) { return false; }
        const auto by_value = [](const function_decl* fn) { return !fn->return_type || !fn->return_type->is_reference(); };
        for (const auto* fn : *hit.methods) { if (!by_value(fn)) { return false; } }
        for (const auto* fn : overrides_of(cls, "[]")) { if (!by_value(fn)) { return false; } }
        return true;
    }

    // The array, map or text type every function a call may reach declares it returns by value (a free function's overloads; a
    // method's and its overrides' on a receiver whose class the walk knows): the result is a copy no storage holds. Null for a
    // reference, a class, a var or an undeclared result, a host function, a callee the walk does not resolve.
    const type_info* by_value_container_result(const call_expr* c) {
        if (!c->callee) { return nullptr; }
        const type_info* result = nullptr;
        const auto admit = [&](const function_decl* fn) {
            const type_info* t = fn->return_type.get();
            if (!t || fn->is_coroutine || t->is_reference()) { return false; }
            switch (t->base_type) {
                case script_value_type::jai_array_type: case script_value_type::jai_map_type: case script_value_type::jai_string_type:
                case script_value_type::jai_utf8_string_type:
                    break;
                default:
                    return false;
            }
            if (result && result->canonical_name() != t->canonical_name()) { return false; }
            result = t;
            return true;
        };
        if (c->callee->get_type() == node_type::identifier_expr) {
            auto* id = static_cast<const identifier_expr*>(c->callee.get());
            if (id->global_qualified || find_local(id->name) || class_ctx_ || !enclosing_namespace_member(id->name).empty()) { return nullptr; }
            const auto it = functions_.find(id->name);
            if (it == functions_.end() || it->second.empty()) { return nullptr; }
            for (const auto* fn : it->second) { if (!admit(fn)) { return nullptr; } }
            return result;
        }
        if (c->callee->get_type() != node_type::member_expr) { return nullptr; }
        auto* m = static_cast<const member_expr*>(c->callee.get());
        if (m->is_static || m->null_safe) { return nullptr; }
        const auto found = lw_types_.find(m->object.get());
        if (found == lw_types_.end() || found->second.kind != ctype::object_k || found->second.tentative || found->second.cls.empty()) { return nullptr; }
        member_hit hit = lookup_member(found->second.cls, m->member, /*want_static=*/false);
        if (!hit.found || !hit.provable || !hit.methods || hit.methods->empty()) { return nullptr; }
        for (const auto* fn : *hit.methods) { if (!admit(fn)) { return nullptr; } }
        for (const auto* fn : overrides_of(found->second.cls, m->member)) { if (!admit(fn)) { return nullptr; } }
        return result;
    }
    // A builtin that changes its receiver in place and cannot raise, run on a container or text a call returned by value: the
    // change dies with the copy
    void call_result_mutation(const member_expr* m, const call_expr* c) {
        const expression* base = lw_on() && !m->is_arrow && !m->null_safe ? lw_strip(m->object.get()) : nullptr;
        if (!base || base->get_type() != node_type::call_expr) { return; }
        const type_info* returned = by_value_container_result(static_cast<const call_expr*>(base));
        if (!returned) { return; }
        const ctype receiver = ctype_from_type_info(returned);
        if (!lw_value_kind(receiver) || !lw_mutator(receiver, m->member, c)) { return; }
        const std::string callee = detail::target_spelling(static_cast<const call_expr*>(base)->callee.get());
        diag(diag_level::error, c->location, std::string(m->member) + " changes a call result in place and nothing reads it: '" + callee + "' returns by value");
    }

    // A call whose callee resolves to the program's free functions or a class's constructors, none returning a reference
    bool call_returns_by_value(const call_expr* c) {
        if (!c->callee || c->callee->get_type() != node_type::identifier_expr) { return false; }
        auto* id = static_cast<const identifier_expr*>(c->callee.get());
        if (id->global_qualified || find_local(id->name) || class_ctx_ || !enclosing_namespace_member(id->name).empty()) { return false; }
        if (auto it = functions_.find(id->name); it != functions_.end()) {
            if (it->second.empty()) { return false; }
            for (const auto* fn : it->second) {
                if (fn->return_type && fn->return_type->is_reference()) { return false; }
            }
            return true;
        }
        return find_sclass(id->name) != nullptr;
    }

    // the referent kinds whose store the table alone decides (a class referent's store may run its operator= or keep a subclass)
    static bool reference_store_checked(const ctype& slot) {
        switch (slot.kind) {
            case ctype::int_k: case ctype::float_k: case ctype::bool_k: case ctype::char_k: case ctype::string_k: case ctype::utf8_k:
            case ctype::array_k: case ctype::map_k: case ctype::math_k:
                return !slot.tentative;
            default: return false;
        }
    }
    // Compound assign on a typed target behaves like C++ —
    // `x op= rhs` is `x = T(x op rhs)` — so it checks exactly like plain assignment
    // of the operation's result. var targets keep the dynamic behavior (unchecked).
    const char* incdec_op_ = nullptr;   // the ++/-- spelling while checking its += 1 / -= 1 desugar
    bool incdec_postfix_ = false;       // that desugar's form
    void check_store(const ctype& rhs, bool compound, token_type base_op, const ctype& target,
                     const std::string& target_display, const std::string& what,
                     const source_location& loc, const declared_at& declared) {
        if (target.is_unknown()) { return; }
        if (compound && incdec_op_ && target.kind == ctype::string_k) {
            diag(diag_level::error, loc, std::string("cannot apply '") + incdec_op_ + "' to 'string'", declared);
            return;
        }
        if (compound && base_op == token_type::plus && target.kind == ctype::string_k && !rhs.is_unknown() &&
            rhs.kind != ctype::string_k && rhs.kind != ctype::char_k && rhs.kind != ctype::utf8_k && !eng_.has_function("+")) {
            // a string target appends text only (a char is text; string += int raises, unlike binary +)
            diag(diag_level::error, loc, "cannot apply '+=' to 'string' and '" + ctype_name(rhs) + "'", declared);
            return;
        }
        if (compound && !rhs.is_unknown() && target.kind != ctype::object_k) {   // a class target's operator decides
            const char* bitwise = base_op == token_type::ampersand ? "&=" : base_op == token_type::pipe ? "|=" : base_op == token_type::caret ? "^="
                                : base_op == token_type::left_shift ? "<<=" : base_op == token_type::right_shift ? ">>=" : nullptr;
            const auto integral = [](const ctype& t) { return t.kind == ctype::int_k || t.kind == ctype::char_k; };
            if (bitwise && (!integral(target) || !integral(rhs))) {
                diag(diag_level::error, loc, std::string("cannot apply '") + bitwise + "' to '" + ctype_name(target) + "' and '" + ctype_name(rhs) + "'",
                     declared);
                return;
            }
        }
        if (compound && target.kind == ctype::object_k &&
            (incdec_op_ ? in_place_operator_may_take(target, incdec_op_, nullptr, incdec_postfix_)
                        : in_place_operator_may_take(target, std::string(detail::binary_op_name(base_op)) + "=", &rhs, false))) {
            return;   // the class's in-place operator runs on the object: nothing is stored
        }
        if (compound && check_compound_operands(target, base_op, rhs, loc, declared)) { return; }
        ctype stored = compound ? binary_result(target, base_op, rhs) : rhs;
        if (assignable(stored, target)) {
            if (!compound) { warn_dynamic_handle_copy(rhs, target); }
            return;
        }
        if (!compound && target.kind == ctype::object_k && assign_operator_accepts(target.cls, stored)) { return; }
        diag(diag_level::error, loc, "cannot assign '" + ctype_name(stored) + "' to " + what +
             " declared '" + target_display + "'" + assignment_detail(stored, target), declared);
    }

    // `target = rhs` on a class-typed target runs a user operator=(rhs) ahead of the
    // conversion ladder (both backends); a C++ class's bound "=" takes what its recorded signatures may take, any other
    // registered operator= stays lenient.
    bool assign_operator_accepts(std::string_view cls, const ctype& src, int depth = 0) {
        if (depth > 32) { return true; }
        const sclass* c = find_sclass(cls);
        if (!c) {
            auto def = engine_class(cls);
            if (!def) { return true; }
            const uint64_t assign_id = eng_.symbolize("=");
            if (def->get_method(assign_id, false).is_invalid()) { return false; }
            if (!def->is_cpp_class() || def->extended_ever()) { return true; }
            const auto& table = def->cpp_method_overload_table();
            const auto bound = table.find(assign_id);
            if (bound == table.end()) { return true; }
            const auto unary = bound->second.find(1);
            if (unary == bound->second.end()) { return true; }
            for (const auto& entry : unary->second) {
                if (entry.param_types.size() != 1 || host_constructor_param_takes(src, entry.param_types[0])) { return true; }
            }
            return false;
        }
        if (auto m = c->methods.find("="); m != c->methods.end()) {
            const std::vector<ctype> one{src};
            for (const auto* fn : m->second) {
                if (script_fn_viable(fn, one)) { return true; }
            }
        }
        for (auto base : c->decl->base_classes) {
            if (assign_operator_accepts(base, src, depth + 1)) { return true; }
        }
        return false;
    }

    // A class operand's unary - + ~ runs the chain's overload taking no argument (both backends): its declared result, or
    // unknown; nullopt when the chain declares none (the builtin rule and its text apply)
    std::optional<ctype> class_unary_operator(std::string_view cls, token_type op) {
        const std::string_view name = op == token_type::minus ? "-" : op == token_type::plus ? "+" : "~";
        member_hit hit = lookup_member(cls, name, /*want_static=*/false);
        if (!hit.found) { return std::nullopt; }
        if (hit.methods) {
            for (const auto* fn : *hit.methods) {
                if (fn->parameters.empty() || fn->parameters.front().default_value) {
                    return fn->return_type && !fn->is_coroutine ? ctype_from_type_info(fn->return_type.get()) : ctype{};
                }
            }
        }
        return ctype{};
    }

    // A class operand's binary arithmetic, bitwise or <=> operator runs when an overload of the chain's nearest set takes the
    // right operand (both backends): its declared result, or unknown; nullopt when none takes it (the builtin rule applies)
    // A number or bool on the left of a bound class's arithmetic or bitwise operator, which the class binds no reflected
    // operator for: the run time raises "No bound ..."; the warning names the binding to add
    void warn_missing_reflected(const binary_expr* b, const ctype& l, const ctype& r) {
        if (r.tentative || l.tentative || r.cls.empty()) { return; }
        if (l.kind != ctype::int_k && l.kind != ctype::float_k && l.kind != ctype::bool_k) { return; }
        const char* op = nullptr;
        switch (b->op.type) {
            case token_type::plus: op = "+"; break;
            case token_type::minus: op = "-"; break;
            case token_type::star: op = "*"; break;
            case token_type::slash: op = "/"; break;
            case token_type::percent: op = "%"; break;
            case token_type::ampersand: op = "&"; break;
            case token_type::pipe: op = "|"; break;
            case token_type::caret: op = "^"; break;
            case token_type::left_shift: op = "<<"; break;
            case token_type::right_shift: op = ">>"; break;
            default: return;
        }
        if (find_sclass(r.cls)) { return; }
        auto def = eng_.get_class_definition(std::string(r.cls));
        if (!def || def->is_script_class() || def->get_method(eng_.symbolize(engine_access::reflected_operator_name(op)), false).is_function()) { return; }
        const std::string lt = ctype_name(l);
        diag(diag_level::warning, b->location, "No bound '" + lt + " " + op + " " + std::string(r.cls) + "': bind operator \"" + op + "\" as (" + lt + ", " +
             std::string(r.cls) + ") on " + std::string(r.cls) + ", or convert the operand");
    }

    // A value of a class with no <, <=> or hash() keys a map by identity (core/script_map.hpp): the run time warns once, the
    // checker at the site
    void warn_identity_map_key(const binary_expr* b, std::string_view cls) {
        for (std::string_view name : {std::string_view("<"), std::string_view("<=>"), std::string_view("hash")}) {
            const member_hit hit = lookup_member(cls, name, /*want_static=*/false);
            if (hit.found || !hit.certain) { return; }
        }
        diag(diag_level::warning, b->location, "map keys of class '" + std::string(cls) +
             "' compare by identity: declare operator<=> (or operator<) or hash() to key its values by value");
    }

    static const char* comparison_spelling(token_type op) {
        switch (op) {
            case token_type::equal_equal: return "==";
            case token_type::bang_equal: return "!=";
            case token_type::less: return "<";
            case token_type::less_equal: return "<=";
            case token_type::greater: return ">";
            case token_type::greater_equal: return ">=";
            case token_type::spaceship: return "<=>";
            default: return nullptr;
        }
    }

    // A class whose chain declares (or a host binds) any comparison operator, either way round, or one the checker cannot see whole
    bool class_may_compare(std::string_view cls) {
        for (std::string_view name : {"==", "!=", "<", "<=", ">", ">=", "<=>"}) {
            if (extended_members_.count(name)) { return true; }
            const member_hit hit = lookup_member(cls, name, /*want_static=*/false);
            if (hit.found || !hit.certain) { return true; }
            const member_hit reflected = lookup_member(cls, engine_access::reflected_operator_name(name), /*want_static=*/false);
            if (reflected.found || !reflected.certain) { return true; }
        }
        return chain_open(cls);
    }

    // Two declared types with no comparison between them: == and != are constant, the orderings raise at run time
    void check_comparison(const binary_expr* b, const ctype& l, const ctype& r) {
        const char* op = comparison_spelling(b->op.type);
        if (!op || l.tentative || r.tentative || eng_.has_function(op)) { return; }
        if ((l.kind == ctype::null_k) != (r.kind == ctype::null_k)) {
            check_null_comparison(b, l.kind == ctype::null_k ? r : l, l.kind == ctype::null_k ? b->right.get() : b->left.get(), op);
            return;
        }
        const auto comparable_kind = [](const ctype& t) {
            switch (t.kind) {
                case ctype::int_k: case ctype::float_k: case ctype::char_k: case ctype::bool_k: case ctype::string_k: case ctype::utf8_k:
                case ctype::array_k: case ctype::map_k: case ctype::function_k: return true;
                case ctype::object_k: return !t.cls.empty();
                default: return false;
            }
        };
        if (!comparable_kind(l) || !comparable_kind(r)) { return; }
        const auto family = [](ctype::kind_t k) {
            return is_numeric_or_char(k) ? 1 : (k == ctype::string_k || k == ctype::utf8_k) ? 2 : 3 + static_cast<int>(k);
        };
        if (l.kind == ctype::object_k && r.kind == ctype::object_k) { return; }
        const ctype* object = l.kind == ctype::object_k ? &l : r.kind == ctype::object_k ? &r : nullptr;
        if (object && class_may_compare(object->cls)) { return; }
        if (!object && (family(l.kind) == family(r.kind) || char_beside_text(l, r))) { return; }
        const bool equality = b->op.type == token_type::equal_equal || b->op.type == token_type::bang_equal;
        std::string text = "cannot compare '" + ctype_name(l) + "' with '" + ctype_name(r) + "' using '" + op + "': ";
        text += object ? "class '" + std::string(object->cls) + "' declares no comparison operator" : std::string("the types have no comparison");
        text += equality ? std::string(" (it is always ") + (b->op.type == token_type::equal_equal ? "false" : "true") + ")"
                         : std::string(" (it raises 'Invalid operands for ") + op + " operator')";
        text += object ? "; declare the operator in the class or convert the operand" : "; convert one operand";
        diag(diag_level::error, b->location, text);
    }

    // null against a declared slot that never holds it (a store of null raises or converts there); handles and
    // functions hold null, and a call's container result is null when the body falls off its end
    // a `?.` chain's own test (parser::null_chain_rest): `?.` over a value that is never null reads it as `.` does
    const expression* null_chain_test_ = nullptr;
    void check_null_comparison(const binary_expr* b, const ctype& t, const expression* operand, const char* op) {
        if (b == null_chain_test_) { return; }
        switch (t.kind) {
            case ctype::int_k: case ctype::float_k: case ctype::char_k: case ctype::bool_k: case ctype::string_k: case ctype::utf8_k: break;
            case ctype::array_k: case ctype::map_k: case ctype::math_k:
                if (operand && operand->get_type() == node_type::call_expr) { return; }
                break;
            case ctype::object_k:
                if (!class_value(t) || t.tentative || t.diagnostic_only) { return; }
                break;
            default: return;
        }
        const bool equality = b->op.type == token_type::equal_equal || b->op.type == token_type::bang_equal;
        const std::string name = ctype_name(t);
        diag(diag_level::error, b->location, "cannot compare '" + name + "' with null using '" + op + "': '" + name + "' is never null" +
             (equality ? std::string(" (it is always ") + (b->op.type == token_type::equal_equal ? "false" : "true") + ")"
                       : std::string(" (it raises 'Invalid operands for ") + op + " operator')") +
             (t.kind == ctype::object_k ? "; declare the slot shared_ptr<" + name + "> (or var) to hold null"
                                        : std::string("; compare with a value of its type, or declare the slot var to hold null")));
    }

    std::optional<ctype> class_binary_operator(std::string_view cls, token_type op, const ctype& r) {
        std::string_view name;
        switch (op) {
            case token_type::plus: name = "+"; break;
            case token_type::minus: name = "-"; break;
            case token_type::star: name = "*"; break;
            case token_type::slash: name = "/"; break;
            case token_type::percent: name = "%"; break;
            case token_type::ampersand: name = "&"; break;
            case token_type::pipe: name = "|"; break;
            case token_type::caret: name = "^"; break;
            case token_type::left_shift: name = "<<"; break;
            case token_type::right_shift: name = ">>"; break;
            case token_type::spaceship: name = "<=>"; break;
            default: return std::nullopt;
        }
        member_hit hit = lookup_member(cls, name, /*want_static=*/false);
        if (!hit.found) { return std::nullopt; }
        if (!hit.methods) { return ctype{}; }
        const std::vector<ctype> one{r};
        const function_decl* taken = nullptr;
        size_t takers = 0;
        for (const auto* fn : *hit.methods) {
            if (script_fn_viable(fn, one)) { taken = fn; ++takers; }
        }
        if (takers == 0) { return std::nullopt; }
        if (takers > 1 || !taken->return_type || taken->is_coroutine) { return ctype{}; }
        return ctype_from_type_info(taken->return_type.get());
    }

    // -------------------------------------------------------------- lost writes

    // A store into a by-value copy that nothing reads afterwards on any path is a load error (docs/site/guide/03-value-semantics.html
    // "Lost writes"). Candidates are the by-value locals, parameters and range-for variables of a statically known value type; each
    // function, lambda and the unit's top level is one region, analysed backward once the walk has typed its expressions. A copy
    // whose value tree holds script class instances is "sensitive": any code that may run before it dies (a call, an operator on a
    // class, a destructor of something else) may observe it (reflect::instances), so it counts as a read there.
    enum class lw_origin : uint8_t { local, parameter, range_for };
    struct lw_candidate {
        std::string_view name;
        lw_origin origin = lw_origin::local;
        bool class_holding = false;
        std::string type_text;
        ctype type;
    };
    using lw_set = std::vector<uint64_t>;
    struct lw_jump { lw_set brk, cont; };
    struct lw_report { int cand; const expression* target; source_location at; uint8_t mode; };
    struct lw_region_state {
        std::vector<int> cands;            // global candidate indices, by region index
        std::unordered_map<std::string_view, std::vector<int>> by_name;
        std::vector<uint8_t> escaped, sensitive;
        lw_set sensitive_set;
        size_t words = 0;
        bool disabled = false;
        bool reporting = false;
        int opaque_exit = 0;
        std::vector<lw_jump> jumps;
        lw_set catch_live;
        lw_set in_scope;   // the candidates whose scope encloses the point being analysed: nothing reads one past its death
        std::vector<lw_report> reports;
    };
    std::vector<lw_candidate> lw_cands_;
    std::vector<std::vector<int>> lw_regions_;
    std::vector<int> lw_local_;   // global candidate -> index in the region being analysed
    std::unordered_map<const identifier_expr*, int> lw_ident_;
    std::unordered_map<const expression*, ctype> lw_types_;
    std::unordered_map<const void*, uint8_t> lw_modes_;
    std::unordered_set<const void*> lw_opaque_locals_;   // locals whose death may run code (a destructor, a dying handle)
    std::unordered_set<const call_expr*> lw_formatter_calls_;   // calls the walk resolved to the engine's own print or format
    std::unordered_map<std::string_view, uint8_t> lw_quiet_memo_;   // 1 quiet, 2 not, 3 in progress
    lw_region_state* lw_ = nullptr;

    bool lw_on() const { return collect_diagnostics_ && !lw_regions_.empty(); }

    // A script class whose instances run no code when a field is stored or the instance dies, and no subclass the program or the
    // engine knows does either: no destructor, no user accessor, no operator=, no host base, fields of such types only
    bool lw_quiet_class(std::string_view cls) {
        if (auto it = lw_quiet_memo_.find(cls); it != lw_quiet_memo_.end()) { return it->second != 2; }
        lw_quiet_memo_[cls] = 3;
        bool quiet = lw_quiet_level(cls, 0);
        if (quiet) {
            auto derives = [&](const class_decl* cd) { return lw_derives(cd, cls, 0); };
            for (const auto& [name, c] : classes_) {
                if (name != cls && derives(c.decl) && !lw_quiet_class(name)) { quiet = false; break; }
            }
            std::unordered_set<std::string_view> asked;   // the program's classes the unit does not model, each asked once
            if (quiet && program_classes_) {
                for (const auto& [name, cd] : *program_classes_) {
                    if (!cd || classes_.count(cd->name) || !asked.insert(cd->name).second) { continue; }
                    if (cd->name != cls && derives(cd.get()) && !lw_quiet_class(cd->name)) { quiet = false; break; }
                }
            }
            if (quiet) {
                if (auto def = engine_class(cls)) {
                    for (const auto& name : eng_.registered_class_names()) {
                        if (classes_.count(name) || asked.count(name)) { continue; }
                        auto other = engine_class(name);
                        if (other && other.get() != def.get() && other->derives_from(def.get())) { quiet = false; break; }
                    }
                }
            }
        }
        lw_quiet_memo_[cls] = quiet ? 1 : 2;
        return quiet;
    }
    bool lw_derives(const class_decl* cd, std::string_view base, int depth) {
        if (!cd || depth > 32) { return false; }
        for (const auto& b : cd->base_classes) {
            if (b == base) { return true; }
            const sclass* c = find_sclass(b);
            if (c && lw_derives(c->decl, base, depth + 1)) { return true; }
        }
        return false;
    }
    bool lw_quiet_level(std::string_view cls, int depth) {
        const sclass* c = depth < 32 ? find_sclass(cls) : nullptr;
        if (!c) { return false; }   // a host class, or a class the walk cannot see
        for (const auto& [name, overloads] : c->methods) {
            if (name.starts_with("~") || name.starts_with("_get_") || name.starts_with("_set_") || name == "operator=") { return false; }
        }
        for (const auto& m : c->decl->members) {
            if (m.declaration && m.declaration->get_type() == node_type::function_decl &&
                static_cast<const function_decl*>(m.declaration.get())->name.starts_with("~")) { return false; }
        }
        for (const auto& [name, f] : c->fields) {
            if (f->function_spelling || lw_type_kind(f->type.get()) == 0) { return false; }
        }
        for (const auto& base : c->decl->base_classes) {
            if (!lw_quiet_level(base, depth + 1)) { return false; }
        }
        return true;
    }
    // 0: not a value a store can be judged on; 1: plain data (scalars, text, math values, containers of them); 2: holds quiet class values
    uint8_t lw_type_kind(const type_info* ti, int depth = 0) {
        if (!ti || depth > 16) { return 0; }
        switch (ti->base_type) {
            case script_value_type::jai_int_type: case script_value_type::jai_float_type: case script_value_type::jai_string_type:
            case script_value_type::jai_char_type: case script_value_type::jai_bool_type: case script_value_type::jai_utf8_string_type:
            case script_value_type::jai_vec2_type: case script_value_type::jai_vec3_type: case script_value_type::jai_vec4_type:
            case script_value_type::jai_ivec2_type: case script_value_type::jai_ivec3_type: case script_value_type::jai_color_type:
            case script_value_type::jai_weak_ptr_type:
                return 1;
            case script_value_type::jai_array_type:
                return lw_type_kind(ti->element_type().get(), depth + 1);
            case script_value_type::jai_map_type: {
                if (ti->type_params.size() != 2) { return 0; }
                const uint8_t k = lw_type_kind(ti->type_params[0].get(), depth + 1);
                const uint8_t v = lw_type_kind(ti->type_params[1].get(), depth + 1);
                return k == 1 && v != 0 ? v : 0;   // a class key runs its comparison on every lookup
            }
            case script_value_type::jai_object_type:
                if (ti->type_name.empty()) { return 0; }
                if (auto it = lw_quiet_memo_.find(std::string_view(ti->type_name)); it != lw_quiet_memo_.end() && it->second == 3) { return 2; }   // a class holding its own kind
                return lw_quiet_class(ti->type_name) ? 2 : 0;
            default:
                return 0;
        }
    }
    uint8_t lw_value_kind(const ctype& t) {
        if (t.tentative && !t.element_proof && !t.alias_proof) { return 0; }
        switch (t.kind) {
            case ctype::int_k: case ctype::float_k: case ctype::bool_k: case ctype::char_k: case ctype::string_k: case ctype::utf8_k:
            case ctype::math_k:
                return 1;
            case ctype::array_k: case ctype::map_k: case ctype::object_k:
                return t.ti && t.ti->base_type != script_value_type::jai_shared_ptr_type ? lw_type_kind(t.ti) : 0;
            default:
                return 0;
        }
    }
    // A value no code runs for: a scalar, text, a math value or a container of those (what an operator, a copy or a death of it runs)
    bool lw_plain(const ctype& t) { return t.kind == ctype::null_k || lw_value_kind(t) == 1; }
    bool lw_scalar(const ctype& t) {
        if (t.tentative && !t.element_proof && !t.alias_proof) { return false; }
        switch (t.kind) {
            case ctype::null_k: case ctype::int_k: case ctype::float_k: case ctype::bool_k: case ctype::char_k: case ctype::string_k:
            case ctype::utf8_k: case ctype::math_k:
                return true;
            default:
                return false;
        }
    }
    // An expression's static type for the pass: a chain rooted at a candidate is typed from the candidate's own type (a range-for
    // auto variable reads as a var to the walk), anything else as the walk recorded it
    std::optional<ctype> lw_type(const expression* e) {
        if (!e) { return std::nullopt; }
        if (const identifier_expr* root = lw_root(e)) {
            if (auto it = lw_ident_.find(root); it != lw_ident_.end()) {
                ctype t;
                if (lw_chain_type(e, lw_cands_[size_t(it->second)].type, t)) { return t; }
                return std::nullopt;
            }
        }
        auto it = lw_types_.find(e);
        if (it == lw_types_.end()) { return std::nullopt; }
        return it->second;
    }
    // what an assignment stores into: the target's own type, else the walk's verdict on the assignment (the target's type)
    std::optional<ctype> lw_target_type(const assignment_expr* a) {
        if (auto t = lw_type(a->target.get())) { return t; }
        return lw_root(a->target.get()) && lw_ident_.count(lw_root(a->target.get())) ? std::nullopt : lw_type(a);
    }
    bool lw_chain_type(const expression* e, const ctype& root, ctype& out, int depth = 0) {
        if (!e || depth > 64) { return false; }
        switch (e->get_type()) {
            case node_type::identifier_expr:
                out = root;
                return true;
            case node_type::cast_expr:
                return lw_chain_type(static_cast<const cast_expr*>(e)->operand.get(), root, out, depth + 1);
            case node_type::member_expr: {
                auto* m = static_cast<const member_expr*>(e);
                ctype o;
                if (!lw_chain_type(m->object.get(), root, o, depth + 1) || o.kind != ctype::object_k || o.cls.empty() || !o.ti ||
                    o.ti->base_type != script_value_type::jai_object_type) { return false; }
                const member_hit hit = lookup_member(o.cls, m->member, /*want_static=*/false);
                if (!hit.found || !hit.field || !hit.field->type || hit.field->is_static || hit.methods) { return false; }
                out = ctype_from_type_info(hit.field->type.get());
                return !out.is_unknown();
            }
            case node_type::binary_expr: {
                auto* b = static_cast<const binary_expr*>(e);
                ctype c;
                if (b->op.type != token_type::left_bracket || !lw_chain_type(b->left.get(), root, c, depth + 1)) { return false; }
                if (c.kind == ctype::string_k || c.kind == ctype::utf8_k) { out = ctype::make(c.kind == ctype::string_k ? ctype::char_k : ctype::utf8_k); return true; }
                if (!c.ti) { return false; }
                if (c.kind == ctype::array_k) { out = ctype_from_type_info(c.ti->element_type().get()); }
                else if (c.kind == ctype::map_k && c.ti->type_params.size() == 2) { out = ctype_from_type_info(c.ti->type_params[1].get()); }
                else { return false; }
                return !out.is_unknown();
            }
            default:
                return false;
        }
    }

    // A by-value copy of a judged type becomes a candidate of the open region (judged = false: a const, or a slot that may hold a
    // handle); a local of any other type marks its block, whose exits may then run code (its destructor, a dying handle's)
    void lw_register(var_entry& entry, const ctype& t, const void* node, std::string_view name, lw_origin origin, std::string type_text, bool judged) {
        if (!lw_on() || scopes_.size() < 2 || (!namespace_scopes_.empty() && namespace_scopes_.back() == scopes_.size() - 1)) { return; }
        const uint8_t kind = lw_value_kind(t);
        if (kind == 0 && node) { lw_opaque_locals_.insert(node); }
        if (!judged || kind == 0) { return; }
        entry.lw = static_cast<int>(lw_cands_.size());
        lw_cands_.push_back({name, origin, kind == 2, std::move(type_text), t});
        lw_regions_.back().push_back(entry.lw);
        if (node) { lw_local_decls_[node] = entry.lw; }
    }

    // the identifier an lvalue chain (x, x.f, x[i], x.f[i].g) is rooted at
    static const identifier_expr* lw_root(const expression* e) {
        for (int guard = 0; e && guard < 256; ++guard) {
            switch (e->get_type()) {
                case node_type::identifier_expr: {
                    auto* id = static_cast<const identifier_expr*>(e);
                    return id->global_qualified ? nullptr : id;
                }
                case node_type::member_expr: {
                    auto* m = static_cast<const member_expr*>(e);
                    if (m->is_static || m->is_arrow || m->null_safe) { return nullptr; }
                    e = m->object.get();
                    break;
                }
                case node_type::binary_expr: {
                    auto* b = static_cast<const binary_expr*>(e);
                    if (b->op.type != token_type::left_bracket) { return nullptr; }
                    e = b->left.get();
                    break;
                }
                case node_type::cast_expr: {
                    auto* c = static_cast<const cast_expr*>(e);
                    if (!c->implicit_ref) { return nullptr; }
                    e = c->operand.get();
                    break;
                }
                default:
                    return nullptr;
            }
        }
        return nullptr;
    }
    int lw_candidate_of(const identifier_expr* id) const {
        if (!id) { return -1; }
        auto it = lw_ident_.find(id);
        return it == lw_ident_.end() || it->second >= static_cast<int>(lw_local_.size()) ? -1 : lw_local_[it->second];
    }
    // Every step from the target down to the root stores into a value the copy owns and none of them can raise or run code: fields
    // of value classes (no accessor, no host property, no handle in between), and at the end at most one map insert whose key needs
    // no conversion. An element or a character store is never judged: its index may be out of range, and the raise is observable.
    bool lw_value_path(const expression* e) {
        bool last = true;
        for (int guard = 0; e && guard < 256; ++guard) {
            switch (e->get_type()) {
                case node_type::identifier_expr:
                    return true;
                case node_type::member_expr: {
                    auto* m = static_cast<const member_expr*>(e);
                    const auto t = lw_type(m->object.get());
                    if (!t || t->kind != ctype::object_k || lw_value_kind(*t) != 2 || chain_has_user_accessor(t->cls, m->member)) { return false; }
                    e = m->object.get();
                    break;
                }
                case node_type::binary_expr: {
                    auto* b = static_cast<const binary_expr*>(e);
                    const auto t = lw_type(b->left.get());
                    const auto k = lw_type(b->right.get());
                    if (!last || !t || !k || t->kind != ctype::map_k || !t->ti || t->ti->type_params.size() != 2 || lw_value_kind(*t) == 0 ||
                        !lw_store_certain(ctype_from_type_info(t->ti->type_params[0].get()), *k)) { return false; }
                    e = b->left.get();
                    break;
                }
                case node_type::cast_expr:
                    e = static_cast<const cast_expr*>(e)->operand.get();
                    continue;
                default:
                    return false;
            }
            last = false;
        }
        return false;
    }
    // A store of v into a slot of type t that converts nothing that could raise: the same scalar (an int into a float widens), the
    // same container type, a value of the same quiet class; a narrow integer element (uint8, int16, ...) range-checks every store
    static bool lw_wide_int(const type_info* ti) {
        return !ti || ti->type_name.empty() || ti->type_name == "int" || ti->type_name == "int64";
    }
    bool lw_store_certain(const ctype& t, const ctype& v) {
        if ((v.tentative && !v.element_proof && !v.alias_proof) || (t.tentative && !t.element_proof && !t.alias_proof)) { return false; }
        switch (t.kind) {
            case ctype::int_k:
                return v.kind == ctype::int_k && lw_wide_int(t.ti);
            case ctype::float_k:
                return v.kind == ctype::float_k || v.kind == ctype::int_k;
            case ctype::bool_k: case ctype::char_k: case ctype::string_k: case ctype::utf8_k:
                return v.kind == t.kind;
            case ctype::math_k:
                return v.kind == ctype::math_k && v.cls == t.cls;
            case ctype::array_k: case ctype::map_k:
                return t.ti && v.ti == t.ti && lw_value_kind(t) != 0;
            case ctype::object_k:
                return v.kind == ctype::object_k && v.cls == t.cls && v.ti && v.ti->base_type == script_value_type::jai_object_type && lw_value_kind(t) == 2;
            default:
                return false;
        }
    }
    // A builtin method that changes its receiver in place and cannot raise for these arguments (a count, an index or a conversion
    // could: those methods are never judged)
    bool lw_mutator(const ctype& receiver, std::string_view name, const call_expr* c) {
        static constexpr std::string_view k_bare[] = {"to_lower", "to_upper", "trim", "trim_left", "trim_right", "reverse", "clear"};
        for (auto m : k_bare) { if (m == name) { return c->arguments.empty(); } }
        if (receiver.kind == ctype::string_k || receiver.kind == ctype::utf8_k) {
            static constexpr std::string_view k_text[] = {"append", "replace", "replace_first", "replace_last", "remove_prefix", "remove_suffix"};
            for (auto m : k_text) {
                if (m != name) { continue; }
                for (const auto& a : c->arguments) {
                    const auto at = lw_type(a.get());
                    if (!at || (at->kind != ctype::string_k && at->kind != ctype::utf8_k) || at->tentative) { return false; }
                }
                return !c->arguments.empty();
            }
            return false;
        }
        if (receiver.kind != ctype::array_k || !receiver.ti) { return false; }
        if (name == "sort") { return c->arguments.empty() && lw_value_kind(receiver) == 1; }
        if (name == "push" && c->arguments.size() == 1) {
            const auto at = lw_type(c->arguments[0].get());
            return at && lw_store_certain(ctype_from_type_info(receiver.ti->element_type().get()), *at);
        }
        if ((name == "append" || name == "concat") && c->arguments.size() == 1) {
            const auto at = lw_type(c->arguments[0].get());
            return at && lw_store_certain(receiver, *at);
        }
        return false;
    }

    struct lw_store {
        int local = -1;
        bool whole = false;   // the variable itself is replaced (x = v)
        bool reads = false;   // the store reads the old value first (x += v, ++x, a mutating method)
        const expression* target = nullptr;
        const identifier_expr* root = nullptr;
    };
    lw_store lw_store_of(const expression* e) {
        lw_store st;
        if (!e) { return st; }
        if (e->get_type() == node_type::assignment_expr) {
            auto* a = static_cast<const assignment_expr*>(e);
            const auto t = lw_target_type(a);
            const bool compound = a->op.type != token_type::equal;
            const auto v = lw_type(a->value.get());
            if (!t || !v || !(compound ? lw_scalar(*t) && lw_scalar(*v) && t->kind != ctype::math_k : lw_store_certain(*t, *v))) { return st; }
            if (compound && t->kind == ctype::int_k && !lw_wide_int(t->ti)) { return st; }   // a narrow integer range-checks
            st.root = lw_root(a->target.get());
            if (!st.root || !lw_value_path(a->target.get())) { return st; }
            st.target = a->target.get();
            st.whole = st.root == lw_strip(a->target.get()) && !compound;
            st.reads = compound;
        } else if (e->get_type() == node_type::unary_expr) {
            auto* u = static_cast<const unary_expr*>(e);
            if (u->op.type != token_type::plus_plus && u->op.type != token_type::minus_minus) { return st; }
            const auto t = lw_type(u->operand.get());
            if (!t || (t->kind != ctype::int_k && t->kind != ctype::float_k) || (t->kind == ctype::int_k && !lw_wide_int(t->ti))) { return st; }
            st.root = lw_root(u->operand.get());
            if (!st.root || !lw_value_path(u->operand.get())) { return st; }
            st.target = u->operand.get();
            st.reads = true;
        } else if (e->get_type() == node_type::call_expr) {
            auto* c = static_cast<const call_expr*>(e);
            if (!c->callee || c->callee->get_type() != node_type::member_expr) { return st; }
            auto* m = static_cast<const member_expr*>(c->callee.get());
            if (m->is_static || m->is_arrow || m->null_safe) { return st; }
            const auto r = lw_type(m->object.get());
            if (!r || (r->kind != ctype::array_k && r->kind != ctype::map_k && r->kind != ctype::string_k && r->kind != ctype::utf8_k) ||
                !lw_value_kind(*r) || !lw_mutator(*r, m->member, c)) { return st; }
            st.root = lw_root(m->object.get());
            if (!st.root || !lw_value_path(m->object.get())) { return st; }
            st.target = e;
            st.reads = true;
        }
        if (st.root) { st.local = lw_candidate_of(st.root); }
        return st;
    }
    // An auto local of class type copies its initializer's value only when that names a value: a field, an element, a by-value
    // result, or a local that holds a value (a handle in an auto or a class-typed parameter shares its object)
    bool lw_value_initializer(const expression* e, int depth = 0) {
        if (!e || depth > 16) { return false; }
        switch (e->get_type()) {
            case node_type::member_expr: return !static_cast<const member_expr*>(e)->is_static;
            case node_type::call_expr: return true;
            case node_type::binary_expr: return static_cast<const binary_expr*>(e)->op.type == token_type::left_bracket;
            case node_type::ternary_expr: {
                auto* t = static_cast<const ternary_expr*>(e);
                return lw_value_initializer(t->then_expression.get(), depth + 1) && lw_value_initializer(t->else_expression.get(), depth + 1);
            }
            case node_type::identifier_expr: {
                const var_entry* v = find_named(static_cast<const identifier_expr*>(e));
                return v && !v->dynamic && !v->is_handle && !v->lw_handle && !v->is_reference;
            }
            default: return false;
        }
    }
    // An auto copy of an element or field of a declared variable (auto e = es[i], auto b = w.boss): the declared chain types the
    // copy, as the typed spelling does (an element read's kind stays tentative for every other use)
    ctype lw_declared_copy_type(const expression* init, const ctype& inferred) {
        const expression* e = lw_strip(init);
        const identifier_expr* root = e && e->get_type() != node_type::identifier_expr ? lw_root(e) : nullptr;
        const var_entry* v = root ? find_named(root) : nullptr;
        ctype result;
        if (!v || v->dynamic || v->is_handle || v->lw_handle || v->is_reference || v->type.tentative ||
            !lw_chain_type(e, v->type, result) || result.kind != inferred.kind || result.cls != inferred.cls) { return inferred; }
        return result;
    }
    static const expression* lw_strip(const expression* e) {
        while (e && e->get_type() == node_type::cast_expr && static_cast<const cast_expr*>(e)->implicit_ref) { e = static_cast<const cast_expr*>(e)->operand.get(); }
        return e;
    }

    // Whether evaluating the node itself may run code (user or host): its children are judged on their own
    bool lw_node_opaque(const expression* e) {
        switch (e->get_type()) {
            case node_type::literal_expr: case node_type::identifier_expr: case node_type::this_expr: case node_type::super_expr:
            case node_type::ternary_expr:
                return false;
            case node_type::member_expr: {
                auto* m = static_cast<const member_expr*>(e);
                if (m->is_static) { return false; }
                const auto t = lw_type(m->object.get());
                if (!t) { return true; }
                if (t->kind == ctype::math_k) { return false; }
                return !(t->kind == ctype::object_k && !t->tentative && !t->cls.empty() && lw_quiet_class(t->cls));
            }
            case node_type::binary_expr: {
                auto* b = static_cast<const binary_expr*>(e);
                const auto l = lw_type(b->left.get());
                const auto r = lw_type(b->right.get());
                if (!l || !r) { return true; }
                if (b->op.type == token_type::left_bracket) {
                    if (l->kind == ctype::array_k || l->kind == ctype::string_k || l->kind == ctype::utf8_k) { return !lw_scalar(*r); }
                    return !(l->kind == ctype::map_k && lw_scalar(*r));
                }
                return !lw_scalar(*l) || !lw_scalar(*r);
            }
            case node_type::unary_expr: {
                const auto t = lw_type(static_cast<const unary_expr*>(e)->operand.get());
                return !t || !lw_scalar(*t);
            }
            case node_type::assignment_expr: {
                auto* a = static_cast<const assignment_expr*>(e);
                const auto t = lw_target_type(a);
                const auto v = lw_type(a->value.get());
                return !t || !v || !lw_plain(*t) || !lw_plain(*v);
            }
            case node_type::call_expr: {
                auto* c = static_cast<const call_expr*>(e);
                if (!c->callee) { return true; }
                if (c->callee->get_type() != node_type::member_expr) {
                    // the engine's print and format over plain values write text and run no script: no instance's text, no callback
                    if (!lw_formatter_calls_.count(c)) { return true; }
                    for (const auto& a : c->arguments) {
                        const auto at = lw_type(a.get());
                        if (!at || !lw_plain(*at)) { return true; }
                    }
                    return false;
                }
                auto* m = static_cast<const member_expr*>(c->callee.get());
                for (const auto& a : c->arguments) {
                    const auto at = lw_type(a.get());
                    if (!at || !lw_plain(*at)) { return true; }
                }
                if (m->is_static) { return math_intrinsic_for_call(m) == math_fn::none; }
                const auto r = lw_type(m->object.get());
                if (!r || !lw_plain(*r)) { return true; }
                return !(r->kind == ctype::array_k || r->kind == ctype::map_k || r->kind == ctype::string_k || r->kind == ctype::utf8_k ||
                         r->kind == ctype::math_k);
            }
            case node_type::array_literal_expr:
            case node_type::map_literal_expr:
            case node_type::cast_expr: {
                const auto t = lw_type(e);
                bool children_plain = true;
                for_each_child_expression(e, [&](const expression* k) {
                    const auto kt = lw_type(k);
                    if (!kt || !lw_plain(*kt)) { children_plain = false; }
                });
                return !t || !lw_plain(*t) || !children_plain;
            }
            default:
                return true;   // a lambda (its captures copy), new, throw, yield, include, import
        }
    }

    void lw_set_bit(lw_set& s, int i) const { s[size_t(i) / 64] |= uint64_t(1) << (size_t(i) % 64); }
    void lw_reset_bit(lw_set& s, int i) const { s[size_t(i) / 64] &= ~(uint64_t(1) << (size_t(i) % 64)); }
    static bool lw_test(const lw_set& s, int i) { return (s[size_t(i) / 64] >> (size_t(i) % 64)) & 1; }
    static void lw_union(lw_set& into, const lw_set& from) { for (size_t i = 0; i < into.size(); ++i) { into[i] |= from[i]; } }

    // A mention the walk did not resolve, by the name of a candidate: whatever it is, that candidate is never judged
    void lw_unresolved(std::string_view name) {
        if (auto it = lw_->by_name.find(name); it != lw_->by_name.end()) {
            for (int i : it->second) { lw_->escaped[size_t(i)] = 1; }
        }
    }
    void lw_mark_mentions(const expression* e, std::vector<uint8_t>& flags) {
        if (!e) { return; }
        stack_.guard(e->location);
        if (e->get_type() == node_type::identifier_expr) {
            auto* id = static_cast<const identifier_expr*>(e);
            const int i = lw_candidate_of(id);
            if (i >= 0) { flags[size_t(i)] = 1; } else if (!lw_ident_.count(id)) { lw_unresolved(id->name); }
            return;
        }
        if (e->get_type() == node_type::lambda_expr) { lw_lambda(static_cast<const lambda_expr*>(e), &flags, nullptr); return; }
        for_each_child_expression(e, [&](const expression* k) { lw_mark_mentions(k, flags); });
    }
    // A lambda body names candidates of the enclosing region: a capture copies them at creation (a read there), a reference capture
    // keeps them, so every later call may read them
    void lw_lambda(const lambda_expr* l, std::vector<uint8_t>* flags, lw_set* uses) {
        for (const auto& cap : l->captures) {
            if (cap.initializer) { if (flags) { lw_mark_mentions(cap.initializer.get(), *flags); } continue; }
            if (auto it = lw_->by_name.find(cap.name); it != lw_->by_name.end()) {
                for (int i : it->second) {
                    lw_->sensitive[size_t(i)] = 1;
                    if (uses) { lw_set_bit(*uses, i); }
                }
            }
        }
        std::vector<const expression*> found;
        lw_collect_identifiers(l->body.get(), found);
        for (auto* e : found) {
            auto* id = static_cast<const identifier_expr*>(e);
            const int i = lw_candidate_of(id);
            if (i >= 0) {
                lw_->sensitive[size_t(i)] = 1;
                if (uses) { lw_set_bit(*uses, i); }
            } else if (!lw_ident_.count(id)) {
                lw_unresolved(id->name);
            }
        }
    }
    // every identifier a statement subtree names, nested lambdas and bodies included
    void lw_collect_identifiers(const ast_node* n, std::vector<const expression*>& out) {
        if (!n) { return; }
        stack_.guard(n->location);
        const node_type t = n->get_type();
        if (t < node_type::expression_stmt) {
            auto* e = static_cast<const expression*>(n);
            if (t == node_type::identifier_expr) { out.push_back(e); return; }
            if (t == node_type::lambda_expr) {
                auto* l = static_cast<const lambda_expr*>(e);
                for (const auto& cap : l->captures) { if (cap.initializer) { lw_collect_identifiers(cap.initializer.get(), out); } else { lw_unresolved_capture(cap.name); } }
                lw_collect_identifiers(l->body.get(), out);
                return;
            }
            for_each_child_expression(e, [&](const expression* k) { lw_collect_identifiers(k, out); });
            return;
        }
        lw_each_child(n, [&](const ast_node* k) { lw_collect_identifiers(k, out); });
    }
    void lw_unresolved_capture(std::string_view name) {
        if (auto it = lw_->by_name.find(name); it != lw_->by_name.end()) {
            for (int i : it->second) { lw_->sensitive[size_t(i)] = 1; }
        }
    }
    // every direct child of a statement or declaration: statements, declarations and expressions
    template <typename F>
    void lw_each_child(const ast_node* n, F&& f) {
        auto st = [&](const auto& p) { if (p) { f(p.get()); } };
        switch (n->get_type()) {
            case node_type::expression_stmt: st(static_cast<const expression_stmt*>(n)->expression); return;
            case node_type::block_stmt: for (const auto& d : static_cast<const block_stmt*>(n)->declarations) { st(d); } return;
            case node_type::if_stmt: { auto* i = static_cast<const if_stmt*>(n); st(i->condition); st(i->then_statement); st(i->else_statement); return; }
            case node_type::while_stmt: { auto* w = static_cast<const while_stmt*>(n); st(w->condition); st(w->body); return; }
            case node_type::for_stmt: { auto* f2 = static_cast<const for_stmt*>(n); st(f2->initializer); st(f2->condition); st(f2->update); st(f2->body); return; }
            case node_type::range_for_stmt: { auto* r = static_cast<const range_for_stmt*>(n); st(r->container); st(r->body); return; }
            case node_type::parallel_for_stmt: {
                auto* p = static_cast<const parallel_for_stmt*>(n);
                st(p->container);
                for (const auto& c : p->extra_containers) { st(c); }
                st(p->body);
                return;
            }
            case node_type::return_stmt: st(static_cast<const return_stmt*>(n)->value); return;
            case node_type::try_stmt: { auto* t = static_cast<const try_stmt*>(n); st(t->try_block); st(t->catch_block); return; }
            case node_type::switch_stmt: {
                auto* s = static_cast<const switch_stmt*>(n);
                st(s->condition);
                for (const auto& c : s->cases) { st(c); }
                st(s->default_case);
                return;
            }
            case node_type::case_stmt: { auto* c = static_cast<const case_stmt*>(n); st(c->value); for (const auto& b : c->body) { st(b); } return; }
            case node_type::default_stmt: for (const auto& b : static_cast<const default_stmt*>(n)->body) { st(b); } return;
            case node_type::variable_decl: st(static_cast<const variable_decl*>(n)->initializer); return;
            case node_type::expression_decl: st(static_cast<const expression_decl*>(n)->expression); return;
            case node_type::statement_decl: st(static_cast<const statement_decl*>(n)->statement); return;
            case node_type::destructuring_decl: st(static_cast<const destructuring_decl*>(n)->initializer); return;
            case node_type::function_decl: {
                auto* fn = static_cast<const function_decl*>(n);
                for (const auto& p : fn->parameters) { st(p.default_value); }
                for (const auto& init : fn->initializers) { for (const auto& a : init.arguments) { st(a); } }
                st(fn->body);
                return;
            }
            default:
                return;   // break, continue, fallthrough; a class, enum or namespace declares nothing a body names
        }
    }

    // The pre-pass: which candidates escape (a reference binds them, a name the walk did not resolve) and which become sensitive
    // (a lambda names them, a call binds them by reference or as a receiver); an include or import gives the region up
    void lw_prepass(const ast_node* n) {
        if (!n || lw_->disabled) { return; }
        stack_.guard(n->location);
        const node_type t = n->get_type();
        if (t == node_type::include_decl || t == node_type::import_decl) { lw_->disabled = true; return; }
        if (t < node_type::expression_stmt) { lw_prepass_expr(static_cast<const expression*>(n)); return; }
        if (t == node_type::function_decl || t == node_type::class_decl || t == node_type::namespace_decl) { return; }   // their own regions
        if (t == node_type::variable_decl) {
            auto* d = static_cast<const variable_decl*>(n);
            if (d->type && d->type->base_type == script_value_type::jai_reference_type && d->initializer) { lw_mark_mentions(d->initializer.get(), lw_->escaped); }
        } else if (t == node_type::range_for_stmt) {
            auto* r = static_cast<const range_for_stmt*>(n);
            if (r->is_reference) { lw_mark_mentions(r->container.get(), lw_->escaped); }
        } else if (t == node_type::parallel_for_stmt) {
            auto* p = static_cast<const parallel_for_stmt*>(n);
            lw_mark_mentions(p->container.get(), lw_->escaped);
            for (const auto& c : p->extra_containers) { lw_mark_mentions(c.get(), lw_->escaped); }
        } else if (t == node_type::return_stmt) {
            auto* r = static_cast<const return_stmt*>(n);
            if (r->binds_reference && r->value) { lw_mark_mentions(r->value.get(), lw_->escaped); }
        }
        lw_each_child(n, [&](const ast_node* k) { lw_prepass(k); });
    }
    void lw_prepass_expr(const expression* e) {
        if (!e) { return; }
        stack_.guard(e->location);
        switch (e->get_type()) {
            case node_type::identifier_expr: {
                auto* id = static_cast<const identifier_expr*>(e);
                if (lw_candidate_of(id) < 0 && !lw_ident_.count(id)) { lw_unresolved(id->name); }
                return;
            }
            case node_type::lambda_expr:
                lw_lambda(static_cast<const lambda_expr*>(e), nullptr, nullptr);
                return;
            case node_type::include_expr: case node_type::import_expr:
                lw_->disabled = true;
                return;
            case node_type::cast_expr: {
                auto* c = static_cast<const cast_expr*>(e);
                if (c->by_reference && !c->implicit_ref) { lw_mark_mentions(c->operand.get(), lw_->escaped); }
                break;
            }
            case node_type::call_expr: {
                auto* c = static_cast<const call_expr*>(e);
                const bool builtin = !lw_node_opaque(e);
                if (!builtin) {
                    for (const auto& a : c->arguments) {
                        if (const int i = lw_candidate_of(lw_root(a.get())); i >= 0) { lw_->sensitive[size_t(i)] = 1; }
                    }
                    if (c->callee && c->callee->get_type() == node_type::member_expr) {
                        if (const int i = lw_candidate_of(lw_root(static_cast<const member_expr*>(c->callee.get())->object.get())); i >= 0) { lw_->sensitive[size_t(i)] = 1; }
                    }
                }
                break;
            }
            case node_type::new_expr:
                for (const auto& a : static_cast<const new_expr*>(e)->arguments) {
                    if (const int i = lw_candidate_of(lw_root(a.get())); i >= 0) { lw_->sensitive[size_t(i)] = 1; }
                }
                break;
            default:
                break;
        }
        for_each_child_expression(e, [&](const expression* k) { lw_prepass_expr(k); });
    }

    // The candidates an expression reads and whether any of its nodes may run code; skip is a store target's root the store does
    // not read
    void lw_scan(const expression* e, lw_set& uses, bool& opaque, const identifier_expr* skip = nullptr) {
        if (!e) { return; }
        stack_.guard(e->location);
        if (e->get_type() == node_type::identifier_expr) {
            const int i = e == skip ? -1 : lw_candidate_of(static_cast<const identifier_expr*>(e));
            if (i >= 0) { lw_set_bit(uses, i); }
            return;
        }
        if (lw_node_opaque(e)) { opaque = true; }
        if (e->get_type() == node_type::lambda_expr) { lw_lambda(static_cast<const lambda_expr*>(e), nullptr, &uses); return; }
        if (e->get_type() == node_type::call_expr) {   // a method's name is not a member read: the call judged it
            auto* c = static_cast<const call_expr*>(e);
            const bool method = c->callee && c->callee->get_type() == node_type::member_expr;
            lw_scan(method ? static_cast<const member_expr*>(c->callee.get())->object.get() : c->callee.get(), uses, opaque, skip);
            for (const auto& a : c->arguments) { lw_scan(a.get(), uses, opaque, skip); }
            return;
        }
        for_each_child_expression(e, [&](const expression* k) { lw_scan(k, uses, opaque, skip); });
    }
    lw_set lw_empty() const { return lw_set(lw_->words, 0); }
    void lw_jump_extra(lw_set& s) const {
        if (lw_->opaque_exit > 0) { lw_union(s, lw_->sensitive_set); }
    }
    lw_set lw_reads(const expression* e, lw_set in) {
        if (!e) { return in; }
        lw_set uses = lw_empty();
        bool opaque = false;
        lw_scan(e, uses, opaque);
        lw_union(in, uses);
        if (opaque) { lw_union(in, lw_->sensitive_set); }
        return in;
    }

    // An expression whose value nothing uses: a store into a candidate is judged against what is live after it
    lw_set lw_effect(const expression* e, lw_set out, const void* mode_key) {
        const lw_store st = lw_store_of(e);
        lw_set uses = lw_empty();
        bool opaque = false;
        lw_scan(e, uses, opaque, st.local >= 0 && !st.reads ? st.root : nullptr);
        if (st.local >= 0 && lw_->reporting && !lw_test(out, st.local) && !lw_->escaped[size_t(st.local)] &&
            !(opaque && lw_->sensitive[size_t(st.local)])) {
            auto mit = lw_modes_.find(mode_key);
            lw_->reports.push_back({lw_->cands[size_t(st.local)], st.target, st.root->location, mit == lw_modes_.end() ? uint8_t(check_mode_attr_none) : mit->second});
        }
        lw_set in = std::move(out);
        if (st.local >= 0 && st.whole) { lw_reset_bit(in, st.local); }
        lw_union(in, uses);
        if (opaque) { lw_union(in, lw_->sensitive_set); }
        return in;
    }

    void lw_mask(lw_set& s) const { for (size_t i = 0; i < s.size(); ++i) { s[i] &= lw_->in_scope[i]; } }
    lw_set lw_block(const std::vector<declaration_ptr>& decls, lw_set out) {
        bool opaque_locals = false;
        lw_set own = lw_empty();
        for (const auto& d : decls) {
            if (!d) { continue; }
            if (lw_opaque_locals_.count(d.get()) || d->get_type() == node_type::destructuring_decl) { opaque_locals = true; }
            if (const int c = lw_declared(d.get()); c >= 0) { lw_set_bit(own, c); }
        }
        const lw_set outer_scope = lw_->in_scope;
        lw_union(lw_->in_scope, own);
        if (opaque_locals) { lw_union(out, lw_->sensitive_set); ++lw_->opaque_exit; }
        lw_mask(out);
        for (size_t k = decls.size(); k-- > 0;) { out = lw_decl(decls[k].get(), std::move(out)); }
        if (opaque_locals) { --lw_->opaque_exit; }
        lw_->in_scope = outer_scope;
        lw_mask(out);
        return out;
    }
    lw_set lw_decl(const declaration* d, lw_set out) {
        lw_set in = lw_decl_in(d, std::move(out));
        lw_mask(in);
        return in;
    }
    lw_set lw_decl_in(const declaration* d, lw_set out) {
        if (!d) { return out; }
        stack_.guard(d->location);
        switch (d->get_type()) {
            case node_type::variable_decl: {
                auto* v = static_cast<const variable_decl*>(d);
                lw_set in = std::move(out);
                if (const int own = lw_declared(v); own >= 0) { lw_reset_bit(in, own); }
                in = lw_reads(v->initializer.get(), std::move(in));
                lw_union(in, lw_->catch_live);
                return in;
            }
            case node_type::expression_decl: {
                lw_set in = lw_effect(static_cast<const expression_decl*>(d)->expression.get(), std::move(out), d);
                lw_union(in, lw_->catch_live);
                return in;
            }
            case node_type::statement_decl:
                return lw_stmt(static_cast<const statement_decl*>(d)->statement.get(), std::move(out));
            case node_type::destructuring_decl: {
                lw_set in = lw_reads(static_cast<const destructuring_decl*>(d)->initializer.get(), std::move(out));
                lw_union(in, lw_->catch_live);
                return in;
            }
            default:
                return out;
        }
    }
    lw_set lw_loop(const expression* cond, const statement* body, const expression* update, const void* update_key, lw_set out,
                   int loop_var, bool exit_opaque) {
        if (exit_opaque) { lw_union(out, lw_->sensitive_set); }
        lw_mask(out);
        const lw_set outer_scope = lw_->in_scope;
        if (loop_var >= 0) { lw_set_bit(lw_->in_scope, loop_var); }
        struct restore_scope { lw_region_state& r; const lw_set& was; ~restore_scope() { r.in_scope = was; } } restore{*lw_, outer_scope};
        const bool reporting = lw_->reporting;
        lw_->reporting = false;
        lw_set head = lw_reads(cond, out);
        for (int round = 0;; ++round) {
            lw_set cont = update ? lw_effect(update, head, update_key) : head;
            lw_->jumps.push_back({out, cont});
            lw_set body_in = lw_stmt(body, cont);
            lw_->jumps.pop_back();
            if (loop_var >= 0) { lw_reset_bit(body_in, loop_var); }
            lw_union(body_in, out);
            lw_set next = lw_reads(cond, std::move(body_in));
            if (next == head || round > 64) {
                lw_->reporting = reporting;
                if (reporting) {
                    lw_set final_cont = update ? lw_effect(update, head, update_key) : head;
                    lw_->jumps.push_back({out, final_cont});
                    lw_stmt(body, final_cont);
                    lw_->jumps.pop_back();
                }
                return head;
            }
            lw_union(head, next);
        }
    }
    lw_set lw_stmt(const statement* s, lw_set out) {
        if (!s) { return out; }
        stack_.guard(s->location);
        lw_set in;
        switch (s->get_type()) {
            case node_type::expression_stmt:
                in = lw_effect(static_cast<const expression_stmt*>(s)->expression.get(), std::move(out), s);
                break;
            case node_type::block_stmt:
                in = lw_block(static_cast<const block_stmt*>(s)->declarations, std::move(out));
                break;
            case node_type::if_stmt: {
                auto* i = static_cast<const if_stmt*>(s);
                in = lw_stmt(i->then_statement.get(), out);
                lw_union(in, i->else_statement ? lw_stmt(i->else_statement.get(), out) : out);
                in = lw_reads(i->condition.get(), std::move(in));
                break;
            }
            case node_type::while_stmt: {
                auto* w = static_cast<const while_stmt*>(s);
                in = lw_loop(w->condition.get(), w->body.get(), nullptr, nullptr, std::move(out), -1, false);
                break;
            }
            case node_type::for_stmt: {
                auto* f = static_cast<const for_stmt*>(s);
                const bool opaque_init = f->initializer && lw_opaque_locals_.count(f->initializer.get());
                const int init_var = f->initializer ? lw_declared(f->initializer.get()) : -1;
                const lw_set outer_scope = lw_->in_scope;
                if (init_var >= 0) { lw_set_bit(lw_->in_scope, init_var); }
                if (opaque_init) { ++lw_->opaque_exit; }
                in = lw_loop(f->condition.get(), f->body.get(), f->update.get(), f, std::move(out), -1, opaque_init);
                if (opaque_init) { --lw_->opaque_exit; }
                in = lw_decl(f->initializer.get(), std::move(in));
                lw_->in_scope = outer_scope;
                break;
            }
            case node_type::range_for_stmt: {
                auto* r = static_cast<const range_for_stmt*>(s);
                const int loop_var = lw_declared(r);
                lw_set probe = lw_empty();
                bool container_opaque = false;
                lw_scan(r->container.get(), probe, container_opaque);
                const bool var_opaque = lw_opaque_locals_.count(r) != 0;
                if (var_opaque) { ++lw_->opaque_exit; }
                in = lw_loop(nullptr, r->body.get(), nullptr, nullptr, std::move(out), loop_var, container_opaque || var_opaque);
                if (var_opaque) { --lw_->opaque_exit; }
                in = lw_reads(r->container.get(), std::move(in));
                break;
            }
            case node_type::parallel_for_stmt: {
                auto* p = static_cast<const parallel_for_stmt*>(s);
                in = lw_loop(nullptr, p->body.get(), nullptr, nullptr, std::move(out), -1, true);
                in = lw_reads(p->container.get(), std::move(in));
                for (const auto& c : p->extra_containers) { in = lw_reads(c.get(), std::move(in)); }
                break;
            }
            case node_type::return_stmt: {
                in = lw_empty();
                lw_jump_extra(in);
                in = lw_reads(static_cast<const return_stmt*>(s)->value.get(), std::move(in));
                break;
            }
            case node_type::break_stmt:
                in = lw_->jumps.empty() ? lw_empty() : lw_->jumps.back().brk;
                lw_jump_extra(in);
                break;
            case node_type::continue_stmt:
                in = lw_->jumps.empty() ? lw_empty() : lw_->jumps.back().cont;
                lw_jump_extra(in);
                break;
            case node_type::fallthrough_stmt:
                in = std::move(out);
                break;
            case node_type::try_stmt: {
                auto* t = static_cast<const try_stmt*>(s);
                ++lw_->opaque_exit;   // a caught value dies at the handler's end
                lw_set handler = lw_stmt(t->catch_block.get(), out);
                --lw_->opaque_exit;
                lw_union(handler, lw_->sensitive_set);
                lw_set saved = lw_->catch_live;
                lw_union(lw_->catch_live, handler);
                in = lw_stmt(t->try_block.get(), std::move(out));
                lw_->catch_live = std::move(saved);
                break;
            }
            case node_type::switch_stmt: {
                auto* sw = static_cast<const switch_stmt*>(s);
                lw_->jumps.push_back({out, lw_->jumps.empty() ? lw_empty() : lw_->jumps.back().cont});
                lw_set all = out;
                lw_set next = out;
                std::vector<const std::vector<statement_ptr>*> bodies;
                for (const auto& c : sw->cases) { if (c) { bodies.push_back(&c->body); } }
                if (sw->default_case) { bodies.push_back(&sw->default_case->body); }
                for (size_t k = bodies.size(); k-- > 0;) {
                    lw_set case_out = out;
                    lw_union(case_out, next);
                    for (size_t j = bodies[k]->size(); j-- > 0;) { case_out = lw_stmt((*bodies[k])[j].get(), std::move(case_out)); }
                    lw_union(all, case_out);
                    next = std::move(case_out);
                }
                lw_->jumps.pop_back();
                for (const auto& c : sw->cases) { if (c) { all = lw_reads(c->value.get(), std::move(all)); } }
                in = lw_reads(sw->condition.get(), std::move(all));
                break;
            }
            default:
                in = s->get_type() >= node_type::variable_decl ? lw_decl(static_cast<const declaration*>(s), std::move(out)) : std::move(out);
                break;
        }
        lw_union(in, lw_->catch_live);
        lw_mask(in);
        return in;
    }

    std::unordered_map<const void*, int> lw_local_decls_;   // a declaring node (variable_decl, range_for_stmt) -> its candidate
    int lw_declared(const void* node) const {
        auto it = lw_local_decls_.find(node);
        return it == lw_local_decls_.end() || it->second >= static_cast<int>(lw_local_.size()) ? -1 : lw_local_[size_t(it->second)];
    }

    void lw_open_region() { if (collect_diagnostics_) { lw_regions_.emplace_back(); } }
    // Analyse the region just walked: body is a function body, a lambda body or the unit's top level
    void lw_close_region(const std::vector<parameter>* params, const std::vector<declaration_ptr>* decls, const statement* body) {
        if (!collect_diagnostics_ || lw_regions_.empty()) { return; }
        std::vector<int> cands = std::move(lw_regions_.back());
        lw_regions_.pop_back();
        if (cands.empty()) { return; }
        lw_region_state state;
        state.cands = cands;
        state.words = (cands.size() + 63) / 64;
        state.escaped.assign(cands.size(), 0);
        state.sensitive.assign(cands.size(), 0);
        state.catch_live.assign(state.words, 0);
        state.in_scope.assign(state.words, 0);
        if (lw_local_.size() < lw_cands_.size()) { lw_local_.resize(lw_cands_.size(), -1); }
        for (size_t i = 0; i < cands.size(); ++i) {
            lw_local_[size_t(cands[i])] = static_cast<int>(i);
            state.by_name[lw_cands_[size_t(cands[i])].name].push_back(static_cast<int>(i));
        }
        lw_region_state* outer = std::exchange(lw_, &state);
        if (decls) { for (const auto& d : *decls) { lw_prepass(d.get()); } }
        if (body) { lw_prepass(body); }
        if (params) { for (const auto& p : *params) { if (p.default_value) { lw_prepass_expr(p.default_value.get()); } } }
        if (!state.disabled) {
            state.sensitive_set.assign(state.words, 0);
            for (size_t i = 0; i < cands.size(); ++i) {
                if (lw_cands_[size_t(cands[i])].class_holding) { state.sensitive[i] = 1; }
                if (state.sensitive[i]) { lw_set_bit(state.sensitive_set, static_cast<int>(i)); }
            }
            bool opaque_params = false;
            if (params) {
                for (const auto& p : *params) {
                    if (lw_opaque_locals_.count(&p)) { opaque_params = true; }
                    if (const int c = lw_declared(&p); c >= 0) { lw_set_bit(state.in_scope, c); }
                }
            }
            state.reporting = true;
            lw_set end = lw_empty();
            if (opaque_params) { lw_union(end, state.sensitive_set); ++state.opaque_exit; }
            if (decls) {
                std::vector<declaration_ptr> top;
                for (const auto& d : *decls) {
                    if (d && d->get_type() != node_type::function_decl && d->get_type() != node_type::class_decl && d->get_type() != node_type::namespace_decl) { top.push_back(d); }
                }
                lw_block(top, std::move(end));
            } else if (body) {
                lw_stmt(body, std::move(end));
            }
            for (const auto& r : state.reports) { lw_report_lost(r); }
        }
        lw_ = outer;
        for (int c : cands) { lw_local_[size_t(c)] = -1; }
        if (outer) {
            for (size_t i = 0; i < outer->cands.size(); ++i) { lw_local_[size_t(outer->cands[i])] = static_cast<int>(i); }
        }
    }

    std::string lw_text(const expression* e, int depth = 0) {
        if (!e || depth > 16) { return "..."; }
        switch (e->get_type()) {
            case node_type::identifier_expr: return std::string(static_cast<const identifier_expr*>(e)->name);
            case node_type::member_expr: {
                auto* m = static_cast<const member_expr*>(e);
                return lw_text(m->object.get(), depth + 1) + "." + std::string(m->member);
            }
            case node_type::binary_expr: return lw_text(static_cast<const binary_expr*>(e)->left.get(), depth + 1) + "[...]";
            case node_type::cast_expr: return lw_text(static_cast<const cast_expr*>(e)->operand.get(), depth + 1);
            case node_type::call_expr: {
                auto* c = static_cast<const call_expr*>(e);
                return lw_text(c->callee.get(), depth + 1) + "(" + (c->arguments.empty() ? "" : "...") + ")";
            }
            default: return "...";
        }
    }
    void lw_report_lost(const lw_report& r) {
        const lw_candidate& c = lw_cands_[size_t(r.cand)];
        const std::string name(c.name);
        const std::string target = lw_text(r.target);
        std::string text;
        switch (c.origin) {
            case lw_origin::range_for:
                text = "'" + target + "' writes to '" + name + "', a copy of the element that is never read afterwards, so the write is lost; "
                       "iterate with '" + c.type_text + "& " + name + "' to change the element";
                break;
            case lw_origin::parameter:
                text = "'" + target + "' writes to parameter '" + name + "', a copy of the argument that is never read afterwards, so the write "
                       "is lost; take '" + c.type_text + "& " + name + "' to change the caller's value";
                break;
            case lw_origin::local:
                text = "'" + target + "' writes to '" + name + "', a copy that is never read afterwards, so the write is lost; declare '" +
                       c.type_text + "& " + name + "' to change the original";
                break;
        }
        mode_guard region(mode_stack_, r.mode, /*scoped=*/false, /*always=*/true);
        diag(diag_level::error, r.at, std::move(text));
    }

    // -------------------------------------------------------------- statements

    // A statement of a list: an if or a loop that leaves the list on one outcome narrows the statements after it
    void walk_listed(declaration* d) {
        narrow_in_list_ = true;
        walk_decl(d);
        narrow_in_list_ = false;
    }

    // A string's truth is no question C++ lets a condition ask: the test is spelled
    void check_condition_type(const expression* condition, const ctype& type) {
        if (type.tentative || (type.kind != ctype::string_k && type.kind != ctype::utf8_k)) { return; }
        const std::string name = condition->get_type() == node_type::identifier_expr ? std::string(static_cast<const identifier_expr*>(condition)->name) : std::string("s");
        diag(diag_level::error, condition->location, "a string is not a condition: write !" + name + ".empty()");
    }

    JAI_NOINLINE void walk_while(while_stmt* w, bool in_list) {
        const size_t mark = narrow_ended_.size();
        check_condition_type(w->condition.get(), infer(w->condition.get()));
        const size_t tested = narrow_ended_.size();
        std::vector<narrow_fact> when_true, when_false;
        narrow_conclude(w->condition.get(), mark, when_true, when_false);
        const size_t held = narrow_facts_.size();
        narrow_push(when_true);
        walk_stmt(w->body.get());
        narrow_pop(held);
        narrow_after_loop(w->body.get(), in_list, mark, tested, when_false);
    }
    // After a loop its condition was false, unless a break left it; the body's own ends came before the last test
    void narrow_after_loop(const statement* body, bool in_list, size_t mark, size_t tested, std::vector<narrow_fact>& when_false) {
        if (!in_list || when_false.empty() || narrow_dry_ || narrow_breaks(body)) { return; }
        std::erase_if(when_false, [&](const narrow_fact& f) {
            for (size_t i = mark; i < tested && i < narrow_ended_.size(); ++i) { if (narrow_ended_[i].key == f.key) { return true; } }
            return false;
        });
        narrow_push(when_false);
    }

    JAI_NOINLINE void walk_if(if_stmt* i, bool in_list) {
        const size_t mark = narrow_ended_.size();
        check_condition_type(i->condition.get(), infer(i->condition.get()));
        std::vector<narrow_fact> when_true, when_false;
        narrow_conclude(i->condition.get(), mark, when_true, when_false);
        const size_t held = narrow_facts_.size();
        narrow_push(when_true);
        walk_stmt(i->then_statement.get());
        narrow_pop(held);
        narrow_push(when_false);
        walk_stmt(i->else_statement.get());
        narrow_pop(held);
        if (!in_list || (when_true.empty() && when_false.empty())) { return; }
        // one branch leaves (return, throw, break, continue): the statements after the if run only on the other outcome
        const bool then_leaves = !narrow_completes(i->then_statement.get());
        const bool else_leaves = i->else_statement && !narrow_completes(i->else_statement.get());
        if (then_leaves == else_leaves) { return; }
        std::vector<narrow_fact>& after = then_leaves ? when_false : when_true;
        narrow_drop_ended(after, mark);
        narrow_push(after);
    }

    JAI_NOINLINE void walk_stmt(statement* s) {
        if (!s) { return; }
        stack_.guard(s->location);
        const bool in_list = std::exchange(narrow_in_list_, false);
        mode_guard region(mode_stack_, s->check_mode_attr, /*scoped=*/false);
        switch (s->get_type()) {
            case node_type::expression_stmt:
                if (lw_on()) { lw_modes_[s] = region_mode(); }
                infer(static_cast<expression_stmt*>(s)->expression.get());
                check_discarded(static_cast<expression_stmt*>(s)->expression.get());
                break;
            case node_type::block_stmt: {
                auto* b = static_cast<block_stmt*>(s);
                mode_guard block(mode_stack_, leading_mode(b->declarations), /*scoped=*/true);
                const size_t held = narrow_facts_.size();
                scopes_.emplace_back();
                for (auto& d : b->declarations) { walk_listed(d.get()); }
                scopes_.pop_back();
                narrow_pop(held);
                break;
            }
            case node_type::if_stmt:
                walk_if(static_cast<if_stmt*>(s), in_list);
                break;
            case node_type::while_stmt: {
                auto* w = static_cast<while_stmt*>(s);
                if (narrow_wants_ahead(w)) { narrow_walk_ahead(nullptr, [&] { walk_while(w, false); }); }
                walk_while(w, in_list);
                break;
            }
            case node_type::for_stmt:
                walk_for(static_cast<for_stmt*>(s), in_list);
                break;
            case node_type::range_for_stmt:
                walk_range_for(static_cast<range_for_stmt*>(s));
                break;
            case node_type::parallel_for_stmt:
                walk_parallel_for(static_cast<parallel_for_stmt*>(s));
                break;
            case node_type::return_stmt:
                check_return(static_cast<return_stmt*>(s));
                break;
            case node_type::try_stmt:
                walk_try(static_cast<try_stmt*>(s));
                break;
            case node_type::switch_stmt:
                walk_switch(static_cast<switch_stmt*>(s));
                break;
            case node_type::break_stmt:
            case node_type::continue_stmt:
            case node_type::fallthrough_stmt:
                break;
            default:
                if (s->get_type() >= node_type::variable_decl) {
                    walk_decl(static_cast<declaration*>(s));
                }
                break;
        }
    }

    JAI_NOINLINE void walk_for(for_stmt* f, bool in_list) {
        scopes_.emplace_back();
        if (lw_on()) { lw_modes_[f] = region_mode(); }
        if (f->initializer) { walk_decl(f->initializer.get()); }
        std::vector<narrow_event> ended;   // what the loop ends anywhere: the update runs after the body
        const auto turns = [&](bool listed) {
            const size_t mark = narrow_ended_.size();
            if (f->condition) { infer(f->condition.get()); }
            const size_t tested = narrow_ended_.size();
            std::vector<narrow_fact> when_true, when_false;
            narrow_conclude(f->condition.get(), mark, when_true, when_false);
            const size_t held = narrow_facts_.size();
            if (f->update) {
                for (const narrow_fact& fact : when_true) {
                    const bool kept = std::none_of(ended.begin(), ended.end(), [&](const narrow_event& ev) { return ev.key == fact.key; });
                    if (kept) { narrow_push({fact}); }
                }
                infer(f->update.get());
                narrow_pop(held);
            }
            narrow_push(when_true);
            walk_stmt(f->body.get());
            narrow_pop(held);
            narrow_after_loop(f->body.get(), listed, mark, tested, when_false);
        };
        if (narrow_wants_ahead(f)) { ended = narrow_walk_ahead(nullptr, [&] { turns(false); }); }
        turns(in_list);
        scopes_.pop_back();
    }

    // a class held by value (a write through a const holder of it is a write to the const); a handle, a var and a scalar are not
    static const type_info* value_class_type(const type_info* t) {
        return t && t->base_type == script_value_type::jai_object_type && !t->type_name.empty() ? t : nullptr;
    }
    JAI_NOINLINE void walk_range_for(range_for_stmt* f) {
        const size_t reads = narrow_reads_;
        const ctype container = enum_table(infer(f->container.get()));
        const bool narrowed_container = narrow_reads_ != reads;
        if (f->binds_map_entry && !container.tentative && !container.is_unknown() && container.kind != ctype::map_k && container.kind != ctype::coroutine_k) {   // for (auto [k, v] : x)
            diag(diag_level::error, f->container->location, "a structured binding takes a map's entries, its key and value: cannot bind '" +
                 ctype_name(container) + "' to " + detail::map_entry_binding_spelling(*f));
        }
        // the elements' static type: an array's element type, a string's char; a var container, an array<var> or anything
        // else has none
        ctype elem{};
        if (container.kind == ctype::array_k && container.ti && container.ti->element_type() &&
            container.ti->element_type()->base_type != script_value_type::jai_any_type) {
            elem = ctype_from_type_info(container.ti->element_type());
        } else if (container.kind == ctype::string_k) {
            elem = ctype::make(ctype::char_k);
        } else if (container.kind == ctype::coroutine_k && container.yields) {
            elem = ctype_from_type_info(container.yields);   // each yield converted to it; a bare yield hands out null
            elem.tentative = true;
        }
        if (!f->element_type) { f->auto_spelled = true; }
        check_spelled_type(f->element_type.get(), f->location);
        if (f->auto_spelled && !f->auto_typed && semantic_walk() && container.kind != ctype::map_k) {
            f->auto_typed = true;   // an auto over elements without a static type is a var, decided once
            if (elem.is_unknown() || container.kind == ctype::coroutine_k) { f->element_type = eng_.get_type_info_any(); }
        }
        const bool declared = f->element_type && f->element_type->base_type != script_value_type::jai_any_type && !f->auto_spelled;
        stamp(f->element_conversion_free, !declared || (!narrowed_container && !elem.is_unknown() && same_type(ctype_from_type_info(f->element_type.get()), elem)), false);
        if (declared && !f->is_reference && !elem.is_unknown() && !assignable(elem, ctype_from_type_info(f->element_type.get()))) {
            const ctype slot = ctype_from_type_info(f->element_type.get());   // each element initializes the variable as a declaration would
            diag(diag_level::error, f->location, "cannot initialize '" + std::string(f->variable_name) + "' declared '" + f->element_type->canonical_name() +
                 "' with '" + ctype_name(elem) + "'" + assignment_detail(elem, slot));
        }
        if (declared && f->is_reference && !elem.is_unknown() && !reference_type_compatible(elem, ctype_from_type_info(f->element_type.get()))) {
            diag(diag_level::error, f->location, "cannot bind '" + ctype_name(elem) + "' to '" + std::string(f->variable_name) +
                 "' declared '" + f->element_type->canonical_name() + "&'");
        }
        // over the elements of a const: auto& binds const references (C++ deduction), T& / var& are errors
        bool loop_const = f->is_const;
        {
            const const_hit h = f->is_reference && !f->is_const ? const_path(f->container.get(), /*value_too=*/true) : const_hit{};
            const bool typed_ref = f->is_reference && !f->auto_spelled && f->element_type && f->variable_name != "entry#";
            if (h.verdict && !typed_ref) { loop_const = loop_const || h.verdict == const_raises; }
            const uint8_t verdict = typed_ref ? h.verdict : uint8_t(const_none);
            const std::string declared = f->element_type ? f->element_type->canonical_name() + "&" : std::string("var&");
            stamp_const(f->const_check, verdict, [&] { return detail::const_bind_text(f->variable_name, declared, f->container.get(), h.root, true); });
            if (verdict == const_raises) {
                diag(diag_level::error, f->location, detail::const_lowercase(detail::const_bind_text(f->variable_name, declared, f->container.get(), h.root, true)), h.line, h.column);
            }
        }
        scopes_.emplace_back();
        var_entry entry;
        entry.is_const = loop_const;
        entry.type = ctype_from_type_info(f->element_type.get());
        entry.dynamic = !f->element_type || f->element_type->base_type == script_value_type::jai_any_type ||
                        f->is_reference;
        entry.is_reference = f->is_reference;
        if (entry.dynamic) {   // T& over elements, or auto (by value or reference) over class elements
            ctype held = elem;
            if (held.is_unknown() && container.is_unknown() && f->auto_spelled) {
                const ctype through = referent_receiver(f->container.get());
                if (through.kind == ctype::array_k && through.ti && through.ti->element_type()) { held = ctype_from_type_info(through.ti->element_type().get()); }
            }
            entry.referent = referent_type(f->auto_spelled ? held : entry.type);
        }
        entry.map_pair = container.kind == ctype::map_k;
        if (f->is_reference && container.kind != ctype::map_k) {
            // an alias of a typed buffer's element holds the element's kind (the buffer converts every store): the builtin receiver
            // proof reads it (the call checks the receiver's kind at run time)
            const ctype held = container.is_unknown() ? declared_container_receiver(f->container.get(), container) : container;
            if (held.kind == ctype::array_k && (!held.tentative || held.alias_proof) && held.ti && held.ti->element_type()) {
                entry.builtin_alias = ctype_from_type_info(held.ti->element_type().get()).kind;
            }
        }
        entry.is_handle = f->element_type && f->element_type->base_type == script_value_type::jai_shared_ptr_type;
        entry.declared = f->element_type.get();
        entry.display = f->element_type ? f->element_type->canonical_name() : "auto";
        entry.line = f->location.line;
        entry.column = f->location.column;
        if (!f->is_reference && !entry.map_pair) {
            // a class-typed variable copies only a value element (a handle element shares its object, whatever the spelling)
            const bool spelled = f->element_type && !f->auto_spelled && f->element_type->base_type != script_value_type::jai_any_type;
            const bool var_spelled = f->element_type && !f->auto_spelled && !spelled;   // a var takes any later value: never judged
            ctype copied = elem;   // an auto array of one scalar literal kind holds that kind, though no type spells it
            if (copied.is_unknown() && container.kind == ctype::array_k && !container.tentative && f->container->get_type() == node_type::identifier_expr) {
                const var_entry* held = find_named(static_cast<const identifier_expr*>(f->container.get()));
                if (held && !held->dynamic && !held->is_reference && held->literal_elem != ctype::unknown) { copied = ctype::make(held->literal_elem); }
            }
            const ctype judged = var_spelled ? ctype{} : spelled && entry.type.kind != ctype::object_k ? entry.type : copied;
            entry.lw_handle = judged.kind == ctype::object_k && elem.kind != ctype::object_k;
            lw_register(entry, judged, f, f->variable_name, lw_origin::range_for, spelled ? f->element_type->canonical_name() : "auto",
                        !loop_const && !entry.lw_handle);
        }
        if (loop_const && !entry.map_pair && !container.tentative) { entry.const_element = value_class_type(elem.ti); }
        if (entry.map_pair && container.ti && !container.tentative) { entry.pair_value = container.ti->value_type().get(); }
        warn_shadow("range-for variable", f->variable_name, f->location);
        if (!entry.map_pair) { narrow_declare(entry, f); }
        scopes_.back().emplace(f->variable_name, std::move(entry));
        if (narrow_wants_ahead(f->body.get())) { narrow_walk_ahead(f, [&] { walk_stmt(f->body.get()); }); }
        walk_stmt(f->body.get());
        scopes_.pop_back();
    }

    JAI_NOINLINE void walk_parallel_for(parallel_for_stmt* f) {
        if (f->counted()) {
            static const char* const roles[] = { "start", "bound", "step" };
            const auto& args = static_cast<const call_expr*>(f->container.get())->arguments;
            for (size_t k = 0; k < 3 && k < args.size(); ++k) {
                const ctype t = infer(args[k].get());
                if (!t.is_unknown() && !t.tentative && t.kind != ctype::int_k) {
                    diag(diag_level::error, args[k]->location, std::string("parallel_for: a counted loop's ") + roles[k] + " is " + ctype_name(t) + ", not int");
                }
            }
        }
        infer(f->container.get());
        scopes_.emplace_back();
        var_entry entry;
        entry.is_const = f->loop_param.is_const;
        entry.type = ctype_from_type_info(f->loop_param.type.get());
        entry.dynamic = true;   // by-ref element: dynamic like ref range-for
        entry.is_reference = f->loop_param.is_reference;
        entry.declared = f->loop_param.type.get();
        entry.display = f->loop_param.type ? f->loop_param.type->canonical_name() : "auto";
        entry.line = f->location.line;
        entry.column = f->location.column;
        warn_shadow("parallel_for variable", f->loop_param.name, f->location);
        scopes_.back().emplace(std::string_view(f->loop_param.name), std::move(entry));
        for (const auto& extra : f->extra_params) {
            var_entry more;
            more.is_const = extra.is_const;
            more.type = ctype_from_type_info(extra.type.get());
            more.dynamic = !extra.type || extra.type->base_type == script_value_type::jai_any_type || extra.is_reference;
            more.is_reference = extra.is_reference;
            more.declared = extra.type.get();
            more.display = extra.type ? extra.type->canonical_name() : "auto";
            more.line = f->location.line;
            more.column = f->location.column;
            warn_shadow("parallel_for variable", extra.name, f->location);
            scopes_.back().emplace(std::string_view(extra.name), std::move(more));
        }
        for (const auto& c : f->extra_containers) { infer(c.get()); }
        ++parallel_body_depth_;
        narrow_frames_.push_back({scopes_.size() - 1, narrow_bound::alias});   // every worker shares the enclosing frame's variables
        walk_stmt(f->body.get());
        narrow_frames_.pop_back();
        --parallel_body_depth_;
        scopes_.pop_back();
    }

    JAI_NOINLINE void walk_try(try_stmt* t) {
        walk_stmt(t->try_block.get());
        scopes_.emplace_back();
        const size_t held = narrow_facts_.size();
        if (!t->catch_var.empty()) {
            var_entry entry;
            entry.dynamic = true;
            warn_shadow("catch variable", t->catch_var, t->location);
            narrow_declare(entry, t);
            const auto caught = scopes_.back().emplace(std::string_view(t->catch_var), std::move(entry)).first;
            if (t->clause_types.size() == 1 && !narrow_dry_) {   // catch (T e): the handler runs for a T alone
                narrow_fact fact;
                const ctype::kind_t kind = narrow_kind_name(t->clause_types[0]);
                if (kind != ctype::unknown ? narrow_kind_fact(caught->second, kind, fact)
                                           : narrow_class_fact(caught->second, scopes_.size() - 1, t->clause_types[0], {}, fact)) { narrow_push({fact}); }
            }
        }
        walk_stmt(t->catch_block.get());
        narrow_pop(held);
        scopes_.pop_back();
    }

    JAI_NOINLINE void walk_switch(switch_stmt* sw) {
        const ctype condition = infer(sw->condition.get());
        for (auto& cs : sw->cases) {
            const ctype value = infer(cs->value.get());
            stamp(cs->static_operand_class, operand_class_of(condition, value), operand_class_unknown);
            const size_t held = narrow_facts_.size();
            scopes_.emplace_back();
            for (auto& st : cs->body) { narrow_in_list_ = true; walk_stmt(st.get()); narrow_in_list_ = false; }
            scopes_.pop_back();
            narrow_pop(held);
        }
        if (sw->default_case) {
            const size_t held = narrow_facts_.size();
            scopes_.emplace_back();
            for (auto& st : sw->default_case->body) { narrow_in_list_ = true; walk_stmt(st.get()); narrow_in_list_ = false; }
            scopes_.pop_back();
            narrow_pop(held);
        }
    }

    JAI_NOINLINE void check_return(return_stmt* r) {
        uint8_t proof = proof_none;
        check_return_impl(r, proof);
        check_const_return(r);
        stamp(r->static_return_proof, proof, proof_none);
        if (proof != proof_none && stamping()) { ++counts_.returns; }
    }

    JAI_NOINLINE void check_return_impl(return_stmt* r, uint8_t& proof) {
        const size_t reads = narrow_reads_;
        const type_info* declared = fn_stack_.empty() || fn_stack_.back().is_coroutine ? nullptr : fn_stack_.back().ret;
        const ctype value = r->value ? infer_expecting(r->value.get(), declared) : ctype::make(ctype::null_k);
        // the return proof has a reader with no run-time guard (parallel_transform's rewrite): a narrowed read stays out of it
        narrow_derived_ = narrow_reads_ != reads;
        std::vector<const call_expr*> calls;
        const bool nothing = r->value_discarded && calls_nothing(r->value.get(), &calls);   // `return f();` in a body that declares no value
        if (r->value && !nothing && !fn_stack_.empty()) { fn_stack_.back().gives_value = true; }
        if (nothing && !fn_stack_.empty() && fn_stack_.back().dispatched) {
            // a returned builtin in-place call is the use main reports, its twin named; a script function's nothing loads
            for (const call_expr* call : calls) {
                if (auto it = nothing_calls_.find(call); it != nothing_calls_.end() && !it->second.reason.empty() && it->second.used == it->second.reason) {
                    diag(diag_level::error, call->location, it->second.used);
                }
            }
        } else if (nothing && !fn_stack_.empty() && !fn_stack_.back().ret) {
            auto& returns = fn_stack_.back().nothing_returns;
            returns.insert(returns.end(), calls.begin(), calls.end());
        }
        check_return_value(r, nothing ? ctype::make(ctype::null_k) : value, proof, nothing);
        if (narrow_derived_) { proof = proof_none; }
        narrow_derived_ = false;
    }

    // An int coroutine's yield or return of a float the checker sees: the one store a coroutine's values are checked for at load
    void check_coroutine_value(const ctype& value, const source_location& loc, const char* verb) {
        if (fn_stack_.empty()) { return; }
        const fn_ctx& fn = fn_stack_.back();
        if (!fn.is_coroutine || !fn.ret) { return; }
        const ctype declared = ctype_from_type_info(fn.ret);
        if (declared.is_unknown() || declared.kind == ctype::null_k || value.is_unknown() || assignable(value, declared)) { return; }
        diag(diag_level::error, loc, std::string("cannot ") + verb + " '" + ctype_name(value) + "' from '" + std::string(fn.name) + "' declared to return '" +
             fn.ret->canonical_name() + "'" + assignment_detail(value, declared), fn.line, fn.column);
    }

    JAI_NOINLINE void check_return_value(return_stmt* r, ctype value, uint8_t& proof, bool nothing = false) {
        const bool valued = r->value && !nothing;   // an arrow returning a call that returns nothing returns no value
        if (r->binds_reference) { report_mixed_reference_conditional(r->value.get()); }
        if (fn_stack_.empty()) { return; }
        const fn_ctx& fn = fn_stack_.back();
        if (valued && value.const_key && (r->binds_reference || handle_type(fn.ret))) {
            map_key_alias(r->location, r->value.get());
        }
        if (!fn.is_coroutine && !fn.ret) {
            fn_ctx& open = fn_stack_.back();
            const uint8_t k = !valued || r->binds_reference || value.tentative || narrow_derived_ ? 0
                            : value.kind == ctype::int_k ? (enum_value(value) ? 0 : 1) : value.kind == ctype::float_k ? 2 : value.kind == ctype::bool_k ? 3 : 0;
            if (k == 0 || (open.inferred_return != 0 && open.inferred_return != k)) { open.inferred_mixed = true; }
            else { open.inferred_return = k; }
            open.returns_seen = true;
            if (!valued || r->binds_reference || !names_a_value(value)) { open.returns_values = false; }
            if (generics_live_) {
                const bool typed = valued && !r->binds_reference && !narrow_derived_;
                const type_info* yields = typed ? deduction_type(value, false) : nullptr;
                const type_info* states = typed ? deduction_type(value, true) : nullptr;
                if (!valued) { open.returns_nothing = true; }
                else if (!yields || (open.returned && open.returned->canonical_name() != yields->canonical_name())) { open.returned_mixed = true; }
                else { open.returned = yields; }
                if (valued && (!states || (open.stated && open.stated->canonical_name() != states->canonical_name()))) { open.stated_mixed = true; }
                else if (valued) { open.stated = states; }
            }
        }
        if (fn.is_coroutine && valued) { check_coroutine_value(value, r->location, "return"); }
        if (fn.is_coroutine || !fn.ret) { return; }   // coroutine returns/yields + auto: lenient
        if (!valued && requires_return_value(fn.ret)) {
            diag(diag_level::error, r->location, "'" + std::string(fn.name) + "' declared to return '" +
                 fn.ret->canonical_name() + "' returns no value here", fn.line, fn.column);
            return;
        }
        if (fn.ret->base_type == script_value_type::jai_reference_type && valued) {
            const type_info* referent = fn.ret->element_type().get();
            if (referent && !reference_type_compatible(narrow_declared_view(value), ctype_from_type_info(referent))) {
                diag(diag_level::error, r->location, "cannot return reference to '" + ctype_name(value) +
                    "' from '" + std::string(fn.name) + "' declared to return '" + fn.ret->canonical_name() + "'");
            }
            return;
        }
        ctype ret = ctype_from_type_info(fn.ret);
        if (ret.is_unknown()) { return; }
        if (valued && !r->binds_reference && ret.kind != ctype::null_k && same_type(ret, value)) { proof = proof_same_type; }
        if (ret.kind == ctype::null_k) {
            if (valued && value.kind != ctype::null_k && !value.is_unknown()) {
                diag(diag_level::error, r->location, "cannot return a value from '" + std::string(fn.name) +
                     "' declared 'void'", fn.line, fn.column);
            }
            return;
        }
        if (!valued) {
            // a nullable result (a handle, a container) returns null here: warning severity, like falling off the end
            diag(diag_level::warning, r->location, "'" + std::string(fn.name) + "' declared to return '" +
                 fn.ret->canonical_name() + "' returns no value here (returns null)", fn.line, fn.column);
            return;
        }
        if (!assignable(value, ret)) {
            diag(diag_level::error, r->location, "cannot return '" + ctype_name(value) + "' from '" +
                 std::string(fn.name) + "' declared to return '" + fn.ret->canonical_name() + "'" + assignment_detail(value, ret),
                 fn.line, fn.column);
        } else {
            warn_dynamic_handle_copy(value, ret);
        }
    }

    // -------------------------------------------------------------- declarations

    JAI_NOINLINE void walk_function_declaration(function_decl* fn) {
        if (scopes_.size() > 1) {
            warn_spliced_function(fn);
            // nested function: visible from here on (top-level ones were pre-collected)
            var_entry entry;
            entry.type = ctype::make(ctype::function_k);
            entry.dynamic = true;
            entry.namespace_function = !namespace_scopes_.empty() && namespace_scopes_.back() == scopes_.size() - 1;
            scopes_.back().emplace(fn->name, std::move(entry));
        }
        walk_function(fn);
    }

    JAI_NOINLINE void walk_destructuring(destructuring_decl* dd) {
        const ctype init = infer(dd->initializer.get());
        const type_info* element = dd->is_const && init.kind == ctype::array_k && !init.tentative && init.ti ? value_class_type(init.ti->element_type().get()) : nullptr;
        for (const auto& [name, _] : dd->names) {
            var_entry entry;
            entry.dynamic = true;
            entry.is_const = dd->is_const;
            entry.const_element = element;
            entry.line = dd->location.line; entry.column = dd->location.column;
            warn_shadow("local", name, dd->location);
            declare_local(name, std::move(entry), dd->location, /*name_independent=*/true);
        }
    }

    JAI_NOINLINE void walk_extern(extern_decl* x) {
        var_entry entry;
        entry.line = x->location.line;
        entry.column = x->location.column;
        entry.is_const = x->is_const;
        if (x->is_function) {
            entry.type = ctype::make(ctype::function_k);
            entry.dynamic = true;
        } else if (x->type) {
            report_unbound_instantiation(x->type.get(), x->location);
            entry.declared = x->type.get();
            entry.display = x->type->canonical_name();
            entry.type = ctype_from_type_info(x->type.get());
            entry.dynamic = entry.type.is_unknown();
        } else {
            entry.dynamic = true;
        }
        if (x->qualified()) { qualified_externs_[std::string(x->name)] = entry; }
        entry.shared_name = true;
        if (x->written().find("::") == std::string_view::npos) { scopes_.back().emplace(x->written(), std::move(entry)); }
    }

    // `ns::x` as the one qualified string its extern declares ("" when the chain is not identifiers joined by ::)
    static std::string qualified_spelling(const member_expr* m) {
        std::string result(m->member);
        const expression* e = m->object.get();
        while (e && e->get_type() == node_type::member_expr && static_cast<const member_expr*>(e)->is_static) {
            result = std::string(static_cast<const member_expr*>(e)->member) + "::" + result;
            e = static_cast<const member_expr*>(e)->object.get();
        }
        if (!e || e->get_type() != node_type::identifier_expr) { return {}; }
        return std::string(static_cast<const identifier_expr*>(e)->name) + "::" + result;
    }
    const var_entry* qualified_extern(const member_expr* m) const {
        if (qualified_externs_.empty() || !m->is_static) { return nullptr; }
        auto it = qualified_externs_.find(qualified_spelling(m));
        return it != qualified_externs_.end() ? &it->second : nullptr;
    }

    JAI_NOINLINE void walk_decl(declaration* d) {
        if (!d) { return; }
        stack_.guard(d->location);
        mode_guard region(mode_stack_, d->check_mode_attr, /*scoped=*/false);
        switch (d->get_type()) {
            case node_type::variable_decl:
                walk_variable_decl(static_cast<variable_decl*>(d));
                break;
            case node_type::function_decl: {
                auto* fn = static_cast<function_decl*>(d);
                if (fn->instance_stamp != 0) {   // a generic's instance: its diagnostics say whose
                    const generic_instance_decl* request = instance_request(*fn);
                    if (unmade_instances_.count(request)) { break; }
                    instance_scope scope(instance_ctx_, request);
                    mode_guard declared(mode_stack_, instance_region_mode(request), /*scoped=*/true);
                    walk_function_declaration(fn);
                    break;
                }
                walk_function_declaration(fn);
                break;
            }
            case node_type::class_decl: {
                auto* cd = static_cast<class_decl*>(d);
                if (cd->method_instances) {
                    walk_method_instances(cd);
                    break;
                }
                if (cd->instance_stamp != 0) {
                    const generic_instance_decl* request = class_instance_request(cd->name);
                    if (unmade_instances_.count(request)) { break; }
                    instance_scope scope(instance_ctx_, request);
                    mode_guard declared(mode_stack_, instance_region_mode(request), /*scoped=*/true);
                    if (!classes_.count(cd->name)) { collect_class(cd); }
                    walk_class(cd);
                    break;
                }
                if (!classes_.count(cd->name)) { collect_class(cd); }
                walk_class(cd);
                break;
            }
            case node_type::generic_instance_decl:
                walk_instance_request(static_cast<const generic_instance_decl*>(d));
                break;
            case node_type::generic_decl:
                walk_generic(*static_cast<const generic_decl*>(d)->definition);
                break;
            case node_type::namespace_decl: {
                auto* n = static_cast<namespace_decl*>(d);
                scopes_.emplace_back();
                namespace_scopes_.push_back(scopes_.size() - 1);
                namespace_walk_names_.push_back(n->name);
                for (auto& inner : n->declarations) { walk_decl(inner.get()); }
                for (const auto& [name, entry] : scopes_.back()) {
                    if (!entry.dynamic && !entry.declared && !entry.is_reference && !entry.type.is_unknown() && entry.type.kind != ctype::function_k) {
                        namespace_inferred_types_[std::string(n->name) + "::" + std::string(name)] = entry.type;
                    }
                }
                namespace_walk_names_.pop_back();
                namespace_scopes_.pop_back();
                scopes_.pop_back();
                break;
            }
            case node_type::expression_decl: {
                auto* ed = static_cast<expression_decl*>(d);
                if (lw_on()) { lw_modes_[d] = region_mode(); }
                infer(ed->expression.get());
                if (!ed->implicit_return) { check_discarded(ed->expression.get()); }   // the program's last expression is its result
                break;
            }
            case node_type::statement_decl:
                walk_stmt(static_cast<statement_decl*>(d)->statement.get());
                break;
            case node_type::destructuring_decl:
                walk_destructuring(static_cast<destructuring_decl*>(d));
                break;
            case node_type::extern_decl:
                walk_extern(static_cast<extern_decl*>(d));
                break;
            case node_type::include_decl:
            case node_type::import_decl: {
                // path expressions still get identifier checks
                if (d->get_type() == node_type::include_decl) {
                    auto* inc = static_cast<include_decl*>(d);
                    if (inc->path_expr) { infer(inc->path_expr.get()); }
                } else {
                    auto* imp = static_cast<import_decl*>(d);
                    if (imp->path_expr) { infer(imp->path_expr.get()); }
                }
                break;
            }
            case node_type::enum_decl:
                walk_enum(static_cast<enum_decl*>(d));
                break;
            default:
                break;
        }
    }

    JAI_NOINLINE void walk_enum(enum_decl* e) {
        // E::value reads the class of that name first, so no value of the enum could be named
        if (scopes_.size() == 1 || (!namespace_scopes_.empty() && namespace_scopes_.back() == scopes_.size() - 1)) {
            if (auto held = eng_.get_class_definition(std::string(e->name)); held && !held->is_script_class()) {
                diag(diag_level::error, e->location, "cannot declare enum '" + std::string(e->name) +
                     "': the host binds a C++ class of that name; give the enum another name", 0, 0, /*always_error=*/true);
            }
        }
        if (scopes_.size() > 1 && (namespace_scopes_.empty() || namespace_scopes_.back() != scopes_.size() - 1)) {
            warn_scoped_enum(e);
            var_entry entry;
            entry.type = ctype{ctype::enum_ref_k, e->name};
            entry.enum_def = e;
            entry.line = e->location.line;
            entry.column = e->location.column;
            scopes_.back().insert_or_assign(e->name, std::move(entry));
        }
    }

    // An auto declaration takes its initializer's static type: var or unknown makes the slot a
    // var; any other type keeps auto, which the runtime locks to the value. Semantic: the unit's first proof pass
    // decides it in every mode (stamp or clear see the same program), and it never changes under a running unit.
    void type_auto_slot(variable_decl* decl, const ctype& init) {
        if (!decl->auto_spelled || decl->auto_typed || !semantic_walk()) { return; }
        decl->auto_typed = true;
        if (init.is_unknown() || init.nullable_result) {
            decl->type = eng_.get_type_info_any();
            decl->decl_fast_flags = variable_decl::classify_decl_fast(decl->type);
        } else if (init.kind == ctype::coroutine_k) {
            decl->type = eng_.get_type_info_coroutine(const_cast<type_info*>(init.yields));   // a handle carries no stamp for the run to lock to
            decl->decl_fast_flags = variable_decl::classify_decl_fast(decl->type);
        } else if (init.kind == ctype::float_k && decl->initializer && decl->initializer->get_type() == node_type::ternary_expr &&
                   last_mixed_ternary_.node == decl->initializer.get()) {
            decl->type = eng_.get_type_info_float();   // an int/float ternary is float (C++ common type): the chosen arm converts
            decl->decl_fast_flags = variable_decl::classify_decl_fast(decl->type);
        } else if (!init.tentative && !init.diagnostic_only) {
            switch (init.kind) {
                case ctype::int_k: if (init.cls.empty()) { decl->inferred_type = eng_.get_type_info_int(); } break;
                case ctype::float_k: decl->inferred_type = eng_.get_type_info_float(); break;
                case ctype::bool_k: decl->inferred_type = eng_.get_type_info_bool(); break;
                case ctype::char_k: decl->inferred_type = eng_.get_type_info_char(); break;
                case ctype::string_k: decl->inferred_type = eng_.get_type_info_string(); break;
                default: break;
            }
        }
    }

    // An auto initializer's static type: a ternary whose arms are int and float has C++'s common type, float;
    // every other mixed ternary has none
    ctype numeric_ternary_type(const expression* init_expr, const ctype& init) {
        if (last_mixed_ternary_.node != init_expr) { return init; }
        const auto a = last_mixed_ternary_.then_type.kind, b = last_mixed_ternary_.else_type.kind;
        if ((a == ctype::int_k && b == ctype::float_k) || (a == ctype::float_k && b == ctype::int_k)) { return ctype::make(ctype::float_k); }
        return init;
    }

    // Inline and plain declarations of one program-scope name never mix across units: the runtime
    // refuses whichever runs second, so the closure reports the pair once, at the plain declaration; this unit's own mix
    // turns the cell in place
    void check_inline_redefinition(const variable_decl* decl) {
        if (decl->is_inline || scopes_.size() != 1 || !program_globals_) { return; }
        eng_.note(decl->name);
        auto it = program_globals_->find(std::string(decl->name));
        if (it == program_globals_->end()) { return; }
        for (const auto& [unit, globals] : it->second) {
            for (const auto& g : globals) {
                if (g && g->is_inline && !unit_declares(g.get())) {
                    diag(diag_level::error, decl->location, detail::inline_redefinition_text(decl, decl->name));
                    return;
                }
            }
        }
    }
    bool unit_declares(const variable_decl* g) const {
        for (const auto& d : decls_) { if (d.get() == g) { return true; } }
        return false;
    }

    JAI_NOINLINE void walk_variable_decl(variable_decl* decl) {
        if (!decl->type && !decl->function_spelling) { decl->auto_spelled = true; }
        check_spelled_type(decl->type.get(), decl->location);
        check_inline_redefinition(decl);
        const size_t reads = narrow_reads_;
        ctype init = decl->initializer ? infer_expecting(decl->initializer.get(), decl->auto_spelled ? nullptr : decl->type.get()) : ctype{};
        const ctype read = init;
        narrow_derived_ = narrow_reads_ != reads;
        // a narrowing changes no declaration: an auto takes the type its initializer has by declaration
        const bool inferred = !decl->function_spelling && (decl->auto_spelled || !decl->type ||
            (decl->type->base_type == script_value_type::jai_shared_ptr_type && !decl->type->element_type()));
        if (narrow_derived_ && inferred) { init = narrow_declared_type(decl->initializer.get()); }
        declare_variable(decl, init);
        narrow_derived_ = false;
        narrow_copy(decl, read);
    }

    ctype narrow_declared_type(expression* e) {
        ctype result;
        narrow_walk_ahead(nullptr, [&] { result = infer(e); });
        return result;
    }
    // `auto y = x;` where x is narrowed: the copy holds what x holds
    void narrow_copy(const variable_decl* decl, const ctype& read) {
        if (narrow_dry_ || !read.narrowed || !decl->initializer || decl->initializer->get_type() != node_type::identifier_expr) { return; }
        if (decl->type && decl->type->base_type == script_value_type::jai_reference_type) { return; }
        size_t scope = 0;
        const var_entry* made = find_local(decl->name, &scope);
        if (!made || made->narrow_key != decl) { return; }
        narrow_fact fact;
        if (read.kind == ctype::object_k ? !narrow_class_fact(*made, scope, read.cls, {}, fact) : !narrow_kind_fact(*made, read.kind, fact)) { return; }
        if (read.open) { fact.type.open = true; fact.type.tentative = true; }
        if (made->dynamic && !made->is_reference) { fact.proof_only = true; }
        narrow_push({fact});
    }

    JAI_NOINLINE void declare_variable(variable_decl* decl, ctype init) {
        bool has_init = decl->initializer != nullptr;
        bool is_reference_decl = decl->type && decl->type->base_type == script_value_type::jai_reference_type;
        bool is_handle_decl = handle_type(decl->type.get());
        if (has_init) {
            if (is_reference_decl && !decl->is_const) { report_mixed_reference_conditional(decl->initializer.get()); }
            if (is_reference_decl && !decl->is_const && (certainly_temporary(decl->initializer.get()) || rooted_in_temporary(decl->initializer.get()))) {
                diag(diag_level::error, decl->initializer->location, std::string(detail::non_lvalue_reference_text_lower));
            }
            if (init.const_key && (is_reference_decl || is_handle_decl)) {
                map_key_alias(decl->location, decl->initializer.get());
            }
        }

        var_entry entry;
        entry.line = decl->location.line;
        entry.column = decl->location.column;
        // `var p = kv;` / `auto& p = kv;`: the copy or alias of a map pair still holds the key
        if (has_init && decl->initializer->get_type() == node_type::identifier_expr) {
            const var_entry* src = find_local(static_cast<const identifier_expr*>(decl->initializer.get())->name);
            entry.map_pair = src && src->map_pair;
        }
        // shared_ptr<auto>: pointee INFERRED from the initializer's class then enforced
        // like the explicit spelling; null/missing initializer has nothing to infer
        // (runtime errors identically)
        if (decl->type && decl->type->base_type == script_value_type::jai_shared_ptr_type &&
            !decl->type->element_type()) {
            if (!has_init || init.kind == ctype::null_k) {
                diag(diag_level::error, decl->location, "cannot infer shared_ptr<auto> pointee for '" +
                     std::string(decl->name) + "' without a non-null initializer (use var, or an explicit shared_ptr<T>)");
                entry.dynamic = true;   // recovery: no cascades
            } else if (!init.is_unknown() && init.kind != ctype::object_k) {
                // runtime: family D's declaration shape (Cannot initialize 'p' (declared shared_ptr<auto>) with int)
                diag(diag_level::error, decl->location, "cannot initialize '" + std::string(decl->name) +
                     "' declared 'shared_ptr<auto>' with '" + ctype_name(init) + "'");
                entry.dynamic = true;   // recovery: no cascades
            } else {
                entry.type = init;      // object(cls), or unknown -> lenient
                entry.type.ti = nullptr;   // a handle, whatever spelling its initializer had (it holds null)
                entry.dynamic = init.is_unknown();
                entry.is_handle = true;
                entry.display = "shared_ptr<" + ctype_name(init) + ">";
            }
            check_const_bind(decl, entry);
            warn_shadow("local", decl->name, decl->location);
            if (lw_on()) { lw_opaque_locals_.insert(decl); }
            entry.shared_name = decl->is_inline;
            if (!decl->is_static) { narrow_declare(entry, decl); }
            declare_local(decl->name, std::move(entry), decl->location, !decl->is_static);
            return;
        }
        if (decl->function_spelling) {
            entry.type = ctype::make(ctype::function_k);
            entry.display = "function";
            entry.dynamic = false;
            if (has_init && !assignable(init, entry.type)) {
                diag(diag_level::error, decl->location, "cannot initialize '" + std::string(decl->name) +
                     "' declared 'function' with '" + ctype_name(init) + "'");
            }
        } else if (decl->type && !decl->auto_spelled) {
            entry.declared = decl->type.get();
            entry.display = decl->type->canonical_name();
            entry.type = ctype_from_type_info(decl->type.get());
            entry.dynamic = decl->type->base_type == script_value_type::jai_any_type || is_reference_decl;
            if (is_reference_decl && has_init && decl->initializer->get_type() == node_type::identifier_expr &&
                !init.tentative && init.ti && (init.kind == ctype::array_k || init.kind == ctype::map_k)) {
                // `var& pix = PIX;` with PIX a declared typed container: the alias's element reads may prove
                entry.alias = init;
                entry.alias.tentative = true;
                entry.alias.alias_proof = true;
            }
            entry.is_reference = is_reference_decl;
            if (is_reference_decl) {
                // auto& takes its initializer's class; var& stays dynamic
                const bool auto_referent = decl->type->type_params.empty() || !decl->type->type_params[0];
                const ctype source = !has_init ? ctype{} : !init.is_unknown() ? init : referent_receiver(decl->initializer.get());
                entry.referent = referent_type(auto_referent ? source : entry.type);
            }
            entry.is_handle = decl->type->base_type == script_value_type::jai_shared_ptr_type;
            ctype binding_source = init;
            if (is_reference_decl && decl->initializer &&
                decl->initializer->get_type() == node_type::identifier_expr) {
                const auto* id = static_cast<const identifier_expr*>(decl->initializer.get());
                if (const auto* source = find_local(id->name); source && source->is_reference && source->declared) {
                    binding_source = ctype_from_type_info(source->declared);
                }
            }
            binding_source = narrow_declared_view(binding_source);
            const bool reference_ok = is_reference_decl && reference_type_compatible(binding_source, entry.type);
            // a const T& binds a temporary T made from what a T declaration takes (C++ lifetime extension)
            const bool init_ok = is_reference_decl ? reference_ok || (decl->is_const && assignable(init, entry.type)) : assignable(init, entry.type);
            stamp(decl->reference_admission_proven, is_reference_decl && has_init && reference_ok && !narrow_derived_ && !binding_source.is_unknown() &&
                  !binding_source.tentative && binding_source.kind != ctype::null_k &&
                  binding_source.kind != ctype::object_k, false);   // a class binding checks at run time (a reload may change the chain)
            if (has_init && (!entry.dynamic || is_reference_decl) && !init_ok) {
                diag(diag_level::error, decl->location, "cannot initialize '" + std::string(decl->name) +
                     "' declared '" + entry.display + "' with '" + ctype_name(binding_source) + "'" + assignment_detail(binding_source, entry.type));
                // Recovery keeps the declared type; reference use remains conservative.
            } else if (has_init && !is_reference_decl) {
                warn_dynamic_handle_copy(init, entry.type);
            }
        } else if (has_init) {
            // auto with initializer: the initializer's static type, then enforced as that type; a var or unknown
            // static type makes the slot a var. null carries no type: the runtime locks at the first non-null
            // store, so a null init stays dynamic.
            init = numeric_ternary_type(decl->initializer.get(), init);
            entry.type = init;
            entry.dynamic = init.is_unknown() || init.kind == ctype::null_k || init.nullable_result;
            entry.display = ctype_name(init);
            type_auto_slot(decl, init);
            // A mixed-branch ternary has no single static type, so the auto is a var: say so where it reads like a typed slot
            if (init.is_unknown() && last_mixed_ternary_.node == decl->initializer.get()) {
                diag(diag_level::warning, decl->location, "auto '" + std::string(decl->name) +
                     "' is initialized from a ternary with branch types '" +
                     ctype_name(last_mixed_ternary_.then_type) + "' and '" +
                     ctype_name(last_mixed_ternary_.else_type) +
                     "' - it has no single static type, so '" + std::string(decl->name) +
                     "' is a var; declare the type explicitly, or make both branches the same type");
            }
        } else {
            // auto without initializer: locked by the first runtime assignment — the
            // checker can't see branch order, so it stays dynamic (documented leniency)
            entry.dynamic = true;
        }
        if (has_init && decl->slot_index != SIZE_MAX && decl->initializer->get_type() == node_type::lambda_expr && !is_reference_decl) {
            entry.lambda = static_cast<const lambda_expr*>(decl->initializer.get());
        }
        if (has_init && decl->initializer->get_type() == node_type::lambda_expr && !is_reference_decl) {
            entry.declared_lambda = static_cast<const lambda_expr*>(decl->initializer.get());
        }
        if (decl->is_const && has_init && decl->initializer->get_type() == node_type::member_expr) {   // a const structured binding's value half
            auto* half = static_cast<const member_expr*>(decl->initializer.get());
            const var_entry* pair = half->member == "second" && half->object && half->object->get_type() == node_type::identifier_expr
                ? find_local(static_cast<const identifier_expr*>(half->object.get())->name) : nullptr;
            if (pair && pair->map_pair) { entry.const_element = value_class_type(pair->pair_value); }
        }
        check_const_bind(decl, entry);
        warn_shadow("local", decl->name, decl->location);
        if (!is_reference_decl && !decl->is_static && lw_on()) {
            const bool spelled = decl->type && !decl->auto_spelled;
            entry.lw_handle = !spelled && entry.type.kind == ctype::object_k && !lw_value_initializer(decl->initializer.get());
            const ctype judged = entry.dynamic ? ctype{} : !spelled && has_init && entry.type.tentative ? lw_declared_copy_type(decl->initializer.get(), entry.type) : entry.type;
            lw_register(entry, judged, decl, decl->name, lw_origin::local, spelled ? entry.display : "auto",
                        !decl->is_const && !decl->function_spelling && !entry.lw_handle);
        }
        entry.shared_name = decl->is_inline;
        if (has_init && decl->auto_spelled && !entry.dynamic && !is_reference_decl) { entry.literal_elem = uniform_literal_element(decl->initializer.get()); }
        if (!decl->is_static) { narrow_declare(entry, decl); }   // a static local is every call's
        if (is_reference_decl && has_init && !decl->is_const) { narrow_end_roots(decl->initializer.get(), /*escape=*/true); }
        declare_local(decl->name, std::move(entry), decl->location, !decl->is_static);
    }

    // The element kind of a literal the run types as array<int>, <float>, <string>, <bool> or <char> when an auto declares it
    // (detail::infer_uniform_array_type): every element a literal of that one kind (a negated number included)
    ctype::kind_t uniform_literal_element(const expression* init) const {
        if (!init || init->get_type() != node_type::array_literal_expr) { return ctype::unknown; }
        const auto* literal = static_cast<const array_literal_expr*>(init);
        ctype::kind_t kind = ctype::unknown;
        for (const auto& el : literal->elements) {
            const expression* written = el.get();
            if (written->get_type() == node_type::unary_expr && static_cast<const unary_expr*>(written)->op.type == token_type::minus) {
                written = static_cast<const unary_expr*>(written)->operand.get();
            }
            if (written->get_type() != node_type::literal_expr) { return ctype::unknown; }
            const auto found = literal_element_types_.find(el.get());
            if (found == literal_element_types_.end() || found->second.tentative) { return ctype::unknown; }
            const ctype::kind_t k = found->second.kind;
            if (k != ctype::int_k && k != ctype::float_k && k != ctype::string_k && k != ctype::bool_k && k != ctype::char_k) { return ctype::unknown; }
            if (kind != ctype::unknown && k != kind) { return ctype::unknown; }
            kind = k;
        }
        return kind;
    }

    JAI_NOINLINE void walk_function(const function_decl* fn) {
        // Named functions capture nothing: lambda capture contexts must not leak in.
        std::vector<capture_ctx> saved_captures;
        saved_captures.swap(capture_stack_);
        scopes_.emplace_back();
        fn_ctx ctx{fn->return_type.get(), fn->is_coroutine, fn->shown_name(),
                   fn->location.line, fn->location.column};
        ctx.sees_fields = class_ctx_ != nullptr;
        ctx.is_static_method = fn->is_static;
        ctx.file = &fn->location.filename;
        // Constructor params shadowing the field they initialize (C(int hp) { this.hp = hp; })
        // is the established idiom - no field-shadow warnings on ctor params.
        bool is_ctor = class_ctx_ && fn->name == class_ctx_name_;
        const cdtor_ctx enclosing_cdtor = cdtor_ctx_;
        cdtor_ctx_ = is_ctor ? cdtor_ctx::construction
                   : (class_ctx_ && !fn->is_static && fn->name.size() > 1 && fn->name[0] == '~') ? cdtor_ctx::destruction : cdtor_ctx::none;
        ctx.dispatched = class_ctx_ && !fn->is_static && !fn->return_type && !is_ctor;
        fn_stack_.push_back(std::move(ctx));
        mode_guard enclosing(mode_stack_, scoped_region_mode(), /*scoped=*/true, /*always=*/true);
        check_spelled_type(fn->return_type.get(), fn->location);
        lw_open_region();
        narrow_frames_.push_back({scopes_.size() - 1, narrow_bound::alias});   // a named function captures nothing
        const size_t narrowed = narrow_facts_.size();
        define_params(fn->parameters, fn->location, /*warn_field_shadows=*/!is_ctor);
        fn_stack_.back().in_ctor_initializers = true;
        for (const auto& init : fn->initializers) {
            for (const auto& arg : init.arguments) { infer(arg.get()); }
            if (is_ctor && init.target == "super" && init.arguments.empty()) { report_base_without_default(class_ctx_name_, fn->location); }
        }
        if (is_ctor) { check_member_initializers(fn); }
        if (is_ctor) { check_base_initializers(fn); }
        fn_stack_.back().in_ctor_initializers = false;
        if (fn->body) {
            mode_guard body(mode_stack_, leading_mode(fn->body->declarations), /*scoped=*/true);
            for (auto& d : fn->body->declarations) { walk_listed(d.get()); }
        }
        narrow_pop(narrowed);
        narrow_frames_.pop_back();
        check_falls_off_end(fn);
        lw_close_region(&fn->parameters, nullptr, fn->body.get());
        if (!fn->return_type && !fn->is_coroutine && fn_stack_.back().returns_seen && fn_stack_.back().returns_values) { auto_value_returns_.insert(fn); }
        if (!fn->return_type && !fn->is_coroutine && fn->body && !is_ctor && !(fn->name.size() > 1 && fn->name[0] == '~') && !fn_stack_.back().dispatched) {
            std::string reason;
            if (close_deduced_body(fn_stack_.back(), fn->body.get(), reason)) { nothing_functions_[fn] = reason; } else { nothing_functions_.erase(fn); }
        }
        fn_stack_.pop_back();
        scopes_.pop_back();
        capture_stack_.swap(saved_captures);
        cdtor_ctx_ = enclosing_cdtor;
    }

    // A default no evaluation can turn into storage (a literal, a construction, a lambda, an operator's result); a conditional, a call
    // or a cast may yield a reference, and the bind judges those
    static bool default_never_names_storage(const expression* e) {
        switch (e->get_type()) {
        case node_type::literal_expr: case node_type::array_literal_expr: case node_type::map_literal_expr:
        case node_type::lambda_expr: case node_type::new_expr:
            return true;
        case node_type::unary_expr: {
            const token_type op = static_cast<const unary_expr*>(e)->op.type;
            return op != token_type::plus_plus && op != token_type::minus_minus;
        }
        case node_type::binary_expr: {
            const token_type op = static_cast<const binary_expr*>(e)->op.type;
            return op != token_type::left_bracket && op != token_type::question_question;
        }
        default:
            return false;
        }
    }

    // C++ [dcl.fct.default]: a default argument names no parameter (it is evaluated where no parameter is bound yet)
    void report_default_naming_parameter(const std::vector<parameter>& params, const parameter& of, const expression* e) {
        if (!e || e->get_type() == node_type::lambda_expr) { return; }
        stack_.guard(e->location);
        if (e->get_type() == node_type::identifier_expr) {
            const std::string_view name = static_cast<const identifier_expr*>(e)->name;
            for (const auto& p : params) {
                if (p.name == name) {
                    diag(diag_level::error, e->location, "default value of '" + of.name + "' names parameter '" + p.name + "': a default cannot read a parameter");
                    return;
                }
            }
            return;
        }
        for_each_child_expression(e, [&](const expression* k) { report_default_naming_parameter(params, of, k); });
    }

    void define_params(const std::vector<parameter>& params, const source_location& fn_loc, bool warn_field_shadows = true) {
        for (const auto& p : params) {
            check_spelled_type(p.type.get(), fn_loc);
            ctype pt = ctype_from_type_info(p.type.get());
            if (p.default_value) {
                report_default_naming_parameter(params, p, p.default_value.get());
                ctype dv = infer_expecting(p.default_value.get(), p.is_reference ? nullptr : p.type.get());
                if (!p.is_reference && !pt.is_unknown() && !assignable(dv, pt)) {
                    diag(diag_level::error, p.default_value->location,
                         "default value of '" + p.name + "' declared '" +
                         (p.type ? p.type->canonical_name() : "auto") + "' has type '" + ctype_name(dv) + "'");
                }
                if (p.is_reference && !p.is_const) {
                    if (default_never_names_storage(p.default_value.get())) {
                        diag(diag_level::error, p.default_value->location, "cannot bind non-lvalue default to reference parameter '" + p.name + "'");
                    }
                    report_mixed_reference_conditional(p.default_value.get());
                }
            }
            warn_shadow("parameter", p.name, fn_loc, warn_field_shadows);
            var_entry entry;
            entry.type = pt;
            entry.dynamic = pt.is_unknown() || p.is_reference;
            entry.is_reference = p.is_reference;
            if (p.is_reference) {
                entry.referent = referent_type(pt);
                entry.reference_parameter = const_cast<parameter*>(&p);
                if (proofs_ != proof_mode::leave) { entry.reference_parameter->ref_store_unproven = false; }
            }
            entry.is_handle = p.type && p.type->base_type == script_value_type::jai_shared_ptr_type;
            entry.declared = p.type.get();
            entry.display = p.type ? p.type->canonical_name() : "auto";
            entry.line = fn_loc.line;
            entry.column = fn_loc.column;
            entry.is_const = p.is_const;
            if (!p.is_reference) {
                entry.lw_handle = pt.kind == ctype::object_k;   // a by-value class parameter shares a handle argument's object
                lw_register(entry, entry.dynamic ? ctype{} : pt, &p, p.name, lw_origin::parameter, entry.display, !p.is_const && !entry.lw_handle);
            }
            narrow_declare(entry, &p);
            declare_local(p.name, std::move(entry), fn_loc);
        }
    }

    // Member-name / global-name collision (same source unit only - engine-registered
    // names like to_string stay quiet: shadowing them is the established
    // print-integration idiom). Warning severity like every shadowing diagnostic:
    // bare 'name' in this class's methods resolves to the MEMBER (C++ order),
    // ::name is the spelling that still reaches the global.
    void warn_member_shadows_global(std::string_view kind, std::string_view name,
                                    std::string_view cls, const source_location& loc) {
        if (global_var_names_.count(name) == 0 && functions_.count(name) == 0) { return; }
        if (lexer::is_reserved_word(name)) { return; }   // bare, the word is the keyword: no member hides a global of it
        diag(diag_level::warning, loc,
             std::string(kind) + " '" + std::string(name) + "' of class '" + std::string(cls) +
             "' shadows the global '" + std::string(name) + "' - bare '" + std::string(name) +
             "' in methods means the member; spell ::" + std::string(name) + " for the global");
    }

    // The first base of cd that is bound final (dynamic_binder::final): empty when none is
    std::string_view final_base(const class_decl* cd) {
        for (std::string_view base : cd->base_classes) {
            if (auto def = engine_class(base); def && def->is_final()) { return base; }
        }
        return {};
    }
    // A derivation from a final class is the declaration's one error, in the run time's words
    void check_final_bases(const class_decl* cd) {
        if (const std::string_view base = final_base(cd); !base.empty()) { diag(diag_level::error, cd->location, detail::final_base_text(cd->name, base)); }
    }

    // A base class no unit, include, extern or host can bring is the definition's error, reported at load in the run time's
    // words so a module that would stop half-defined applies nothing
    void check_class_bases(const class_decl* cd) {
        if (!namespace_scopes_.empty() || has_includes_) { return; }
        for (std::string_view base : cd->base_classes) {
            if (find_sclass(base) || engine_class(base) || base.find("::") != std::string_view::npos || global_var_names_.count(base) ||
                engine_knows_name(std::string(base))) { continue; }
            const std::string unbound = eng_.unbound_instantiation_text(base);
            diag(diag_level::error, cd->location, unbound.empty() ? "Base class '" + std::string(base) + "' not found" : unbound);
            return;
        }
        report_diamond(cd);
    }

    // The bases a class names, from this program's declaration or the engine's class
    std::vector<std::string> bases_of(std::string_view name) {
        std::vector<std::string> result;
        if (const sclass* sc = find_sclass(name); sc && sc->decl) {
            for (std::string_view base : sc->decl->base_classes) { result.emplace_back(base); }
        } else if (auto def = eng_.find_script_class(std::string(name))) {
            for (const auto& parent : def->get_parent_classes()) { if (parent) { result.push_back(parent->get_name()); } }
        }
        return result;
    }

    void report_diamond(const class_decl* cd) {
        if (cd->base_classes.size() < 2) { return; }
        std::unordered_map<std::string, std::string> reached_through;
        for (std::string_view base_view : cd->base_classes) {
            const std::string base(base_view);
            std::unordered_set<std::string> seen;
            std::vector<std::string> pending{base};
            while (!pending.empty()) {
                std::string current = std::move(pending.back());
                pending.pop_back();
                if (!seen.insert(current).second) { continue; }
                const auto [it, fresh] = reached_through.emplace(current, base);
                if (!fresh && it->second != base) {
                    diag(diag_level::error, cd->location, detail::diamond_inheritance_text(cd->name, current, it->second, base));
                    return;
                }
                for (auto& parent : bases_of(current)) { pending.push_back(std::move(parent)); }
            }
        }
    }

    // The instance fields of the class from `first` on, in declaration order
    static std::unordered_map<std::string_view, const variable_decl*> fields_from(const class_decl* cd, const variable_decl* first) {
        std::unordered_map<std::string_view, const variable_decl*> result;
        bool reached = false;
        for (const auto& m : cd->members) {
            if (!m.declaration || m.declaration->get_type() != node_type::variable_decl) { continue; }
            const auto* f = static_cast<const variable_decl*>(m.declaration.get());
            reached = reached || f == first;
            if (reached && !f->is_static) { result.emplace(f->name, f); }
        }
        return result;
    }

    // A field initializer reads its own field or a later one: in C++ the read is undefined, here it would hand a typed field null
    void report_unready_field_read(const variable_decl* field, const source_location& loc) {
        if (!initializing_field_ || !field) { return; }
        const auto it = initializing_field_->unready.find(field->name);
        if (it == initializing_field_->unready.end() || it->second != field) { return; }
        const std::string read(field->name), initializing(initializing_field_->name);
        diag(diag_level::error, loc, "field initializer of '" + initializing + "' reads '" + read + "' before its initialization: fields initialize in declaration order" +
             (read == initializing ? std::string() : "; declare '" + read + "' above '" + initializing + "'"), field->location);
    }

    // A class that holds itself by value, directly, through other classes' value fields or through a base, never finishes
    // constructing (a class value is never null): reported once per class, at the field or base the cycle leaves it by
    void report_value_cycle(const class_decl* cd) {
        if (!collect_diagnostics_) { return; }
        struct step { std::string_view cls; std::string path; const source_location* at; };
        std::vector<step> pending{{cd->name, std::string(), nullptr}};
        std::unordered_set<std::string_view> seen;
        while (!pending.empty()) {
            step s = std::move(pending.back());
            pending.pop_back();
            const sclass* sc = find_sclass(s.cls);
            if (!sc || !sc->decl) { continue; }
            const auto reach = [&](std::string_view to, std::string path, const source_location& via) {
                const source_location* at = s.at ? s.at : &via;
                if (to == cd->name) {
                    diag(diag_level::error, *at, "class '" + std::string(cd->name) + "' holds itself by value (" + path +
                         "): a class value is never null; make one link a 'shared_ptr<" + std::string(to) + ">' (an array or a map of them starts empty)",
                         0, 0, /*always_error=*/true);
                    return true;
                }
                if (seen.insert(to).second) { pending.push_back({to, std::move(path), at}); }
                return false;
            };
            const std::string link = (s.path.empty() ? std::string() : s.path + " -> ") + std::string(s.cls);
            for (std::string_view base : sc->decl->base_classes) {
                if (reach(base, link + " : " + std::string(base), sc->decl->location)) { return; }
            }
            for (const auto& m : sc->decl->members) {
                if (!m.declaration || m.declaration->get_type() != node_type::variable_decl) { continue; }
                const auto* field = static_cast<const variable_decl*>(m.declaration.get());
                if (field->is_static || !field->type || field->type->base_type != script_value_type::jai_object_type || field->type->type_name.empty()) { continue; }
                if (reach(field->type->type_name, link + "." + std::string(field->name), field->location)) { return; }
            }
        }
    }

    void walk_class(const class_decl* cd) {
        if (auto held = eng_.get_class_definition(std::string(cd->name)); held && !held->is_script_class()) {
            diag(diag_level::error, cd->location, format_error_message(detail::k_host_class_name_taken, cd->name, cd->name));
        }
        for (const std::string_view base : cd->base_classes) { refuse_bare_generic_class(base, cd->location); }
        check_class_bases(cd);
        check_final_bases(cd);
        report_value_cycle(cd);
        check_member_names(cd);
        auto it = classes_.find(cd->name);
        if (it == classes_.end()) { return; }
        sclass* prev_class = class_ctx_;
        std::string_view prev_name = class_ctx_name_;
        class_ctx_ = &it->second;
        class_ctx_name_ = cd->name;
        for (const auto& m : cd->members) {
            if (!m.declaration) { continue; }
            if (m.declaration->get_type() == node_type::variable_decl) {
                auto* f = static_cast<variable_decl*>(m.declaration.get());
                warn_member_shadows_global(f->is_static ? "static field" : "field",
                                           f->name, cd->name, f->location);
                if (!f->type && !f->function_spelling) { f->auto_spelled = true; }
                check_spelled_type(f->type.get(), f->location);
                const bool listed_value = !f->is_static && f->default_initialized && f->type && f->type->base_type == script_value_type::jai_object_type &&
                                          every_constructor_initializes(it->second, f->name);
                if (f->initializer && !listed_value) {
                    const cdtor_ctx enclosing_cdtor = std::exchange(cdtor_ctx_, f->is_static ? cdtor_ctx::none : cdtor_ctx::construction);
                    initializing_field initializing{f->name, {}};
                    if (!f->is_static) { initializing.unready = fields_from(cd, f); }
                    initializing_field_ = f->is_static ? nullptr : &initializing;
                    ctype init = infer_expecting(f->initializer.get(), f->auto_spelled ? nullptr : f->type.get());
                    initializing_field_ = nullptr;
                    cdtor_ctx_ = enclosing_cdtor;
                    if (f->auto_spelled) { type_auto_slot(f, numeric_ternary_type(f->initializer.get(), init)); }   // an auto field: its initializer's static type
                    if ((f->type && !f->auto_spelled) || f->function_spelling) {
                        ctype ft = declared_variable_type(f);
                        if (!ft.is_unknown() && !assignable(init, ft)) {
                            diag(diag_level::error, f->location, "cannot initialize field '" +
                                 std::string(f->name) + "' declared '" + declared_variable_type_name(f) +
                                 "' with '" + ctype_name(init) + "'" + assignment_detail(init, ft));
                        } else {
                            warn_dynamic_handle_copy(init, ft);
                        }
                    }
                }
            } else if (m.declaration->get_type() == node_type::function_decl) {
                auto* fn = static_cast<function_decl*>(m.declaration.get());
                if (fn->name != cd->name) {   // ctors carry the class name by construction
                    warn_member_shadows_global(fn->is_static ? "static method" : "method",
                                               fn->name, cd->name, fn->location);
                }
                walk_function(fn);
            } else {
                walk_decl(m.declaration.get());
            }
        }
        class_ctx_ = prev_class;
        class_ctx_name_ = prev_name;
    }

    std::unordered_map<const lambda_expr*, ctype> lambda_results_;   // an undeclared lambda whose returns are one certain scalar kind
    std::unordered_map<const lambda_expr*, const type_info*> lambda_return_types_;   // and the one type they yield, void for none (fn_ctx::returned)
    std::unordered_map<const lambda_expr*, const type_info*> lambda_stated_return_types_;   // the one type they yield with declared results read (fn_ctx::stated)

    // An init-capture, or an explicit by-value capture of a program-scope name, holds a copy taken at creation:
    // the call reads that copy, never the name, so the body is not grafted at the call site
    bool lambda_copies_at_creation(const lambda_expr* l) {
        for (const auto& cap : l->captures) {
            if (cap.initializer) { return true; }
            if (!cap.by_reference && cap.name != "this" && !find_local(cap.name)) { return true; }
        }
        return false;
    }

    JAI_NOINLINE void walk_lambda(lambda_expr* l) {
        check_lambda_captures(l);
        // the body runs later and again: what it ends for a variable it captured is ended where the lambda is made
        if (narrow_wants_ahead(l->body.get())) { narrow_walk_ahead(nullptr, [&] { walk_lambda_body(l); }); }
        walk_lambda_body(l);
    }

    JAI_NOINLINE void walk_lambda_body(lambda_expr* l) {
        capture_stack_.push_back(capture_ctx{scopes_.size(), l});
        narrow_frames_.push_back({scopes_.size(), l->default_capture == lambda_expr::capture_default::by_value ? narrow_bound::copy : narrow_bound::alias});
        const size_t narrowed = narrow_facts_.size();
        // the body may run after the constructor or destructor that creates the lambda has returned
        const cdtor_ctx enclosing_cdtor = std::exchange(cdtor_ctx_, cdtor_ctx::none);
        struct restore_cdtor { cdtor_ctx& slot; cdtor_ctx was; ~restore_cdtor() { slot = was; } } restore{cdtor_ctx_, enclosing_cdtor};
        const initializing_field* enclosing_field = std::exchange(initializing_field_, nullptr);
        struct restore_field { const initializing_field*& slot; const initializing_field* was; ~restore_field() { slot = was; } } restore_init{initializing_field_, enclosing_field};
        open_lambda_scope(l);
        mode_guard enclosing(mode_stack_, scoped_region_mode(), /*scoped=*/true, /*always=*/true);
        check_spelled_type(l->return_type.get(), l->location);
        lw_open_region();
        define_lambda_names(l);
        walk_stmt(l->body.get());
        narrow_pop(narrowed);
        narrow_frames_.pop_back();
        lw_close_region(&l->parameters, nullptr, l->body.get());
        close_lambda_scope(l);
    }

    JAI_NOINLINE void check_lambda_captures(lambda_expr* l) {
        for (const auto& cap : l->captures) {
            if (cap.initializer) { infer(cap.initializer.get()); continue; }   // an init-capture: its expression, in the enclosing scope
            if (cap.name == "this") { continue; }   // capture-list entries are not identifier uses
            if (find_local(cap.name)) { continue; }
            if (class_ctx_ && detail::is_weak_this_capture(cap)) { continue; }
            if (class_ctx_) {
                member_hit hit = lookup_member(class_ctx_name_, cap.name, false);
                if (hit.found || hit.unresolved) { continue; }
            }
            if (functions_.count(cap.name) || classes_.count(cap.name) || enums_.count(cap.name) ||
                program_or_derived_name(cap.name)) { continue; }
            std::string name(cap.name);
            if (extra_global(name) || engine_knows_name(name)) { continue; }
            undeclared(l->location, cap.name);
        }
    }

    JAI_NOINLINE void open_lambda_scope(lambda_expr* l) {
        scopes_.emplace_back();
        fn_ctx ctx{l->return_type.get(), /*is_coroutine=*/false, "lambda",
                   l->location.line, l->location.column};
        ctx.file = &l->location.filename;
        // Only a [this] capture reaches fields from a lambda body (bare [=]/[&] do not)
        for (const auto& cap : l->captures) {
            if (cap.name == "this") { ctx.sees_fields = class_ctx_ != nullptr; break; }
        }
        fn_stack_.push_back(std::move(ctx));
    }

    JAI_NOINLINE void define_lambda_names(lambda_expr* l) {
        define_params(l->parameters, l->location);
        for (const auto& cap : l->captures) {
            if (!cap.initializer) {   // a named capture is the lambda's own copy of the variable, typed as it
                if (var_entry* original = find_local(cap.name)) {
                    var_entry own = *original;
                    own.capture_alias = cap.by_reference;
                    own.capture_copy = !cap.by_reference;
                    scopes_.back().emplace(cap.name, std::move(own));
                    continue;
                }
                if (!class_ctx_ || !detail::is_weak_this_capture(cap)) { continue; }
            }
            var_entry entry;
            entry.dynamic = true;   // the lambda's own copy, typed by the value it holds
            entry.line = l->location.line; entry.column = l->location.column;
            scopes_.back().emplace(cap.name, std::move(entry));
        }
    }

    JAI_NOINLINE void close_lambda_scope(lambda_expr* l) {
        check_falls_off_end(l->return_type.get(), l->body.get(), l->location, "lambda");
        {
            const fn_ctx& done = fn_stack_.back();
            const uint8_t kind = !l->return_type && !done.inferred_mixed && !has_includes_ ? done.inferred_return : uint8_t(0);
            stamp(l->inferred_return_kind, kind, uint8_t(0));
            if (kind) { lambda_results_[l] = ctype::make(kind == 1 ? ctype::int_k : kind == 2 ? ctype::float_k : ctype::bool_k); }
            else { lambda_results_.erase(l); }
            std::string reason;
            const bool deduced = !l->return_type || l->contextual_return;
            if (deduced && (l->contextual_return ? !done.gives_value && !has_includes_ : close_deduced_body(done, l->body.get(), reason))) { nothing_lambdas_[l] = reason; }
            else { nothing_lambdas_.erase(l); }
            if (generics_live_) {   // no return of a value at all is a lambda that returns nothing
                const bool unknown = l->return_type || done.returned_mixed || has_includes_ || (done.returned && done.returns_nothing);
                if (unknown) { lambda_return_types_.erase(l); }
                else { lambda_return_types_[l] = done.returned ? done.returned : eng_.get_type_info_void(); }
                if (l->return_type || done.stated_mixed || has_includes_ || !done.stated || done.returns_nothing) { lambda_stated_return_types_.erase(l); }
                else { lambda_stated_return_types_[l] = done.stated; }
            }
        }
        fn_stack_.pop_back();
        scopes_.pop_back();
        capture_stack_.pop_back();
    }

    // ---------------------------------------------------- falling off the end

    void check_falls_off_end(const function_decl* fn) {
        if (fn->is_coroutine) { return; }
        check_falls_off_end(fn->return_type.get(), fn->body.get(), fn->location, fn->name);
    }

    // A result that holds no null is a load error when a path reaches the end of the body; the end reached only through
    // a catch the analysis cannot prove runs, or a nullable result, stays a warning
    void check_falls_off_end(const type_info* return_type, const statement* body, const source_location& location, std::string_view name) {
        if (!return_type || !body) { return; }
        ctype ret = ctype_from_type_info(return_type);
        if ((ret.is_unknown() && !requires_return_value(return_type)) || ret.kind == ctype::null_k) { return; }
        const auto flow = return_flow(return_type).analyze(body);
        if (!(flow.paths & (return_flow::normal | return_flow::maybe_normal))) { return; }
        const bool required = requires_return_value(return_type) && (flow.paths & return_flow::normal);
        diag(required ? diag_level::error : diag_level::warning, location, "'" + std::string(name) + "' declared to return '" +
             return_type->canonical_name() + "' may reach the end of its body without returning" + (required ? "" : " (returns null)"));
    }

};

// Safety-net sweep: the program scope's names (see all_names_), and what the walk reads by name at any depth.
void static_checker::collect_names(const statement* node) {
    if (!node) { return; }
    stack_.guard(node->location);
    struct nested_scope {
        int& depth;
        explicit nested_scope(int& a_depth) : depth(a_depth) { ++depth; }
        ~nested_scope() { --depth; }
    };
    switch (node->get_type()) {
        case node_type::variable_decl: {
            auto* v = static_cast<const variable_decl*>(node);
            collect_program_name(v->name);
            note_program_decl(v->name, v->location);
            layout_inits_[v->name].push_back(binary_call(v->initializer.get(), "layout") ? v->initializer.get() : nullptr);
            if (v->type && v->type->base_type == script_value_type::jai_reference_type && v->initializer &&
                v->initializer->get_type() == node_type::identifier_expr) {
                assigned_names_.insert(static_cast<const identifier_expr*>(v->initializer.get())->name);
            }
            if (v->initializer) { collect_names_expr(v->initializer.get()); }
            break;
        }
        case node_type::extern_decl:
            collect_program_name(static_cast<const extern_decl*>(node)->written());
            break;
        case node_type::function_decl: {
            auto* f = static_cast<const function_decl*>(node);
            collect_program_name(f->name);
            if (f->instance_stamp == 0) { note_program_decl(f->name, f->location); }   // an instance is declared by the linker: its order rule is report_instance_before_declaration
            nested_scope body{collect_depth_};
            collect_names(f->body.get());
            break;
        }
        case node_type::class_decl: {
            auto* c = static_cast<const class_decl*>(node);
            if (c->method_instances) { break; }
            collect_program_name(c->name);
            if (c->instance_stamp == 0) { note_program_decl(c->name, c->location); }
            nested_scope members{collect_depth_};
            for (const auto& m : c->members) { collect_names(m.declaration.get()); }
            break;
        }
        case node_type::namespace_decl: {
            auto* n = static_cast<const namespace_decl*>(node);
            collect_program_name(n->name);
            const bool enclosing = std::exchange(collecting_namespace_names_, true);
            for (const auto& d : n->declarations) { collect_names(d.get()); }
            collecting_namespace_names_ = enclosing;
            break;
        }
        case node_type::enum_decl: {
            auto* e = static_cast<const enum_decl*>(node);
            collect_program_name(e->name);
            break;
        }
        case node_type::destructuring_decl: {
            auto* dd = static_cast<const destructuring_decl*>(node);
            for (const auto& [name, _] : dd->names) { collect_program_name(name); }
            if (dd->initializer) { collect_names_expr(dd->initializer.get()); }
            break;
        }
        case node_type::include_decl:
            has_includes_ = true;
            has_dynamic_includes_ = true;   // nested: a runtime include, beyond the closure
            break;
        case node_type::import_decl:
            has_includes_ = true;
            if (static_cast<const import_decl*>(node)->path_expr) { has_dynamic_includes_ = true; }   // a literal one is a closure unit (collect_unit_imports)
            break;
        case node_type::expression_stmt:
            collect_names_expr(static_cast<const expression_stmt*>(node)->expression.get());
            break;
        case node_type::expression_decl:
            collect_names_expr(static_cast<const expression_decl*>(node)->expression.get());
            break;
        case node_type::statement_decl:
            collect_names(static_cast<const statement_decl*>(node)->statement.get());
            break;
        case node_type::block_stmt: {
            nested_scope block{collect_depth_};
            for (const auto& d : static_cast<const block_stmt*>(node)->declarations) { collect_names(d.get()); }
            break;
        }
        case node_type::if_stmt: {
            auto* i = static_cast<const if_stmt*>(node);
            nested_scope branches{collect_depth_};
            collect_names_expr(i->condition.get());
            collect_names(i->then_statement.get());
            collect_names(i->else_statement.get());
            break;
        }
        case node_type::while_stmt: {
            auto* w = static_cast<const while_stmt*>(node);
            nested_scope body{collect_depth_};
            collect_names_expr(w->condition.get());
            collect_names(w->body.get());
            break;
        }
        case node_type::for_stmt: {
            auto* f = static_cast<const for_stmt*>(node);
            nested_scope loop{collect_depth_};
            collect_names(f->initializer.get());
            collect_names_expr(f->condition.get());
            collect_names_expr(f->update.get());
            collect_names(f->body.get());
            break;
        }
        case node_type::range_for_stmt: {
            auto* f = static_cast<const range_for_stmt*>(node);
            collect_names_expr(f->container.get());
            nested_scope loop{collect_depth_};
            collect_names(f->body.get());
            break;
        }
        case node_type::parallel_for_stmt: {
            auto* f = static_cast<const parallel_for_stmt*>(node);
            collect_names_expr(f->container.get());
            nested_scope loop{collect_depth_};
            collect_names(f->body.get());
            break;
        }
        case node_type::try_stmt: {
            auto* t = static_cast<const try_stmt*>(node);
            nested_scope blocks{collect_depth_};
            collect_names(t->try_block.get());
            collect_names(t->catch_block.get());
            break;
        }
        case node_type::switch_stmt: {
            auto* sw = static_cast<const switch_stmt*>(node);
            collect_names_expr(sw->condition.get());
            nested_scope cases{collect_depth_};
            for (const auto& cs : sw->cases) {
                for (const auto& st : cs->body) { collect_names(st.get()); }
            }
            if (sw->default_case) {
                for (const auto& st : sw->default_case->body) { collect_names(st.get()); }
            }
            break;
        }
        case node_type::return_stmt:
            collect_names_expr(static_cast<const return_stmt*>(node)->value.get());
            break;
        default:
            break;
    }
}

void static_checker::collect_names_expr(const expression* e) {
    if (!e) { return; }
    stack_.guard(e->location);
    switch (e->get_type()) {
        case node_type::lambda_expr: {
            auto* l = static_cast<const lambda_expr*>(e);
            for (const auto& cap : l->captures) { if (cap.initializer) { collect_names_expr(cap.initializer.get()); } }
            ++collect_depth_;
            collect_names(l->body.get());
            --collect_depth_;
            break;
        }
        case node_type::binary_expr: {
            auto* b = static_cast<const binary_expr*>(e);
            collect_names_expr(b->left.get());
            collect_names_expr(b->right.get());
            break;
        }
        case node_type::unary_expr:
            collect_names_expr(static_cast<const unary_expr*>(e)->operand.get());
            break;
        case node_type::assignment_expr: {
            auto* a = static_cast<const assignment_expr*>(e);
            if (a->target && a->target->get_type() == node_type::identifier_expr) {
                assigned_names_.insert(static_cast<const identifier_expr*>(a->target.get())->name);
            } else if (a->target && a->target->get_type() == node_type::member_expr && static_cast<const member_expr*>(a->target.get())->is_static) {
                rebindable_paths_.insert(qualified_spelling(static_cast<const member_expr*>(a->target.get())));
            }
            collect_names_expr(a->target.get());
            collect_names_expr(a->value.get());
            break;
        }
        case node_type::call_expr: {
            auto* c = static_cast<const call_expr*>(e);
            collect_names_expr(c->callee.get());
            const bool is_extend = c->callee->get_type() == node_type::identifier_expr &&
                                   static_cast<const identifier_expr*>(c->callee.get())->name == "extend";
            const bool takes_values = value_taking_library_call(c);
            for (const auto& arg : c->arguments) {
                collect_names_expr(arg.get());
                if (arg && !takes_values && arg->get_type() == node_type::identifier_expr) { passed_names_.insert(static_cast<const identifier_expr*>(arg.get())->name); }
                if (arg && !takes_values && arg->get_type() == node_type::member_expr && static_cast<const member_expr*>(arg.get())->is_static) {
                    rebindable_paths_.insert(qualified_spelling(static_cast<const member_expr*>(arg.get())));
                }
                if (!is_extend || !arg) { continue; }
                if (arg->get_type() == node_type::identifier_expr) {
                    extended_names_.insert(static_cast<const identifier_expr*>(arg.get())->name);
                } else if (arg->get_type() == node_type::member_expr) {
                    extended_members_.insert(static_cast<const member_expr*>(arg.get())->member);
                }
            }
            break;
        }
        case node_type::member_expr:
            collect_names_expr(static_cast<const member_expr*>(e)->object.get());
            break;
        case node_type::new_expr:
            for (const auto& arg : static_cast<const new_expr*>(e)->arguments) { collect_names_expr(arg.get()); }
            break;
        case node_type::include_expr:
            collect_names_expr(static_cast<const include_expr*>(e)->path_expr.get());
            break;
        case node_type::import_expr: {
            auto* imp = static_cast<const import_expr*>(e);
            has_includes_ = true;
            if (!imp->path_exprs.empty()) { has_dynamic_includes_ = true; }   // literal paths are closure units (collect_unit_imports)
            for (const auto& a : imp->path_exprs) { collect_names_expr(a.get()); }
            break;
        }
        case node_type::ternary_expr: {
            auto* t = static_cast<const ternary_expr*>(e);
            collect_names_expr(t->condition.get());
            collect_names_expr(t->then_expression.get());
            collect_names_expr(t->else_expression.get());
            break;
        }
        case node_type::array_literal_expr:
            for (const auto& el : static_cast<const array_literal_expr*>(e)->elements) { collect_names_expr(el.get()); }
            break;
        case node_type::map_literal_expr:
            for (const auto& [k, v] : static_cast<const map_literal_expr*>(e)->entries) {
                collect_names_expr(k.get());
                collect_names_expr(v.get());
            }
            break;
        case node_type::throw_expr:
            collect_names_expr(static_cast<const throw_expr*>(e)->value.get());
            break;
        case node_type::cast_expr:
            collect_names_expr(static_cast<const cast_expr*>(e)->operand.get());
            break;
        case node_type::yield_expr:
            collect_names_expr(static_cast<const yield_expr*>(e)->value.get());
            break;
        default:
            break;
    }
}

} // namespace

// Every literal import below the top level: an import runs at program scope wherever it is written, so its module is a unit
// of the program like a top-level one (lambda bodies included: they are statements too)
void collect_unit_imports(const std::vector<declaration_ptr>& decls, std::vector<std::string>& out) {
    auto on_statement = [&out](const statement* s) {
        if (s->get_type() != node_type::import_decl) { return; }
        auto* imp = static_cast<const import_decl*>(s);
        if (!imp->path_expr) { out.push_back(imp->path); }
    };
    auto on_expression = [&out](const expression* e) {
        if (e->get_type() != node_type::import_expr) { return; }
        for (const auto& path : static_cast<const import_expr*>(e)->paths) { out.push_back(path); }
    };
    detail::unit_walker<decltype(on_statement), decltype(on_expression)> walker{on_statement, on_expression};
    for (const auto& d : decls) {
        if (d && d->get_type() != node_type::import_decl) { walker.stmt(d.get()); }
    }
}

check_report run_static_check(engine& eng, const std::vector<declaration_ptr>& decls,
                              const std::vector<std::string>* extra_globals, check_options opts) {
    return static_checker(eng, decls, extra_globals, opts).run();
}

std::string less_than_answers(engine& eng, const std::vector<declaration_ptr>& decls, const std::vector<std::string>& queries, check_options opts) {
    opts.diagnostics = false;
    opts.proofs = check_options::proofs_t::leave;
    opts.outside = nullptr;
    opts.generic_requests = nullptr;
    return static_checker(eng, decls, nullptr, opts).less_answers(queries);
}

// KEEP-PARALLEL with static_checker::collect_scope_decls (same categories, flat spelling).
void collect_unit_names(const std::vector<declaration_ptr>& decls, std::unordered_set<std::string>& out) {
    for (const auto& d : decls) {
        if (!d) { continue; }
        switch (d->get_type()) {
            case node_type::function_decl:
                out.emplace(static_cast<const function_decl*>(d.get())->name);
                break;
            case node_type::variable_decl:
                out.emplace(static_cast<const variable_decl*>(d.get())->name);
                break;
            case node_type::extern_decl: {
                auto* x = static_cast<const extern_decl*>(d.get());
                out.emplace(x->written());
                if (x->qualified()) { collect_qualified_extern(x, out); }
                break;
            }
            case node_type::destructuring_decl:
                for (const auto& [name, _] : static_cast<const destructuring_decl*>(d.get())->names) { out.emplace(name); }
                break;
            case node_type::class_decl:
                out.emplace(static_cast<const class_decl*>(d.get())->name);
                break;
            case node_type::enum_decl: {
                auto* e = static_cast<const enum_decl*>(d.get());
                out.emplace(e->name);
                for (const auto& [vname, _] : e->values) { out.emplace(vname); }
                break;
            }
            case node_type::namespace_decl: {
                auto* n = static_cast<const namespace_decl*>(d.get());
                out.emplace(n->name);
                for (size_t pos = n->name.find("::"); pos != std::string_view::npos; pos = n->name.find("::", pos + 2)) {
                    out.emplace(n->name.substr(0, pos));
                }
                std::vector<declaration_ptr> qualified_members;   // its classes, enums and nested namespaces carry qualified names
                for (const auto& member : n->declarations) {
                    if (member && (member->get_type() == node_type::class_decl || member->get_type() == node_type::enum_decl ||
                                   member->get_type() == node_type::namespace_decl)) { qualified_members.push_back(member); }
                }
                collect_unit_names(qualified_members, out);
                collect_namespace_members(n, out);
                break;
            }
            default:
                break;
        }
    }
}

// a namespace's functions and variables pool under their qualified names ("a::b::f"): a nested block's name is its full path
static void pool_unit_functions(const std::vector<declaration_ptr>& decls, const std::string& space, const std::string& unit_path, program_function_map& out) {
    for (const auto& d : decls) {
        if (d && d->get_type() == node_type::function_decl) {
            auto fd = std::static_pointer_cast<const function_decl>(d);
            if (fd->instance_stamp != 0) { continue; }   // a generic's instance is each unit's own declaration
            out[space + std::string(fd->name)][unit_path].push_back(std::move(fd));
        } else if (d && d->get_type() == node_type::namespace_decl) {
            auto* n = static_cast<const namespace_decl*>(d.get());
            pool_unit_functions(n->declarations, std::string(n->name) + "::", unit_path, out);
        }
    }
}

void collect_unit_functions(const std::vector<declaration_ptr>& decls, const std::string& unit_path, program_function_map& out) {
    for (auto& [name, units] : out) { units.erase(unit_path); }   // a re-gather replaces the unit's own entries, the names it dropped too
    pool_unit_functions(decls, {}, unit_path, out);
}

static void pool_unit_globals(const std::vector<declaration_ptr>& decls, const std::string& space, const std::string& unit_path, program_global_map& out) {
    for (const auto& d : decls) {   // every spelling counts toward "declared once": the lookup asks the one declaration for its type
        if (d && d->get_type() == node_type::variable_decl) {
            auto vd = std::static_pointer_cast<const variable_decl>(d);
            out[space + std::string(vd->name)][unit_path].push_back(std::move(vd));
        } else if (d && d->get_type() == node_type::namespace_decl) {
            auto* n = static_cast<const namespace_decl*>(d.get());
            pool_unit_globals(n->declarations, std::string(n->name) + "::", unit_path, out);
        }
    }
}

void collect_unit_globals(const std::vector<declaration_ptr>& decls, const std::string& unit_path, program_global_map& out) {
    for (auto& [name, units] : out) { units.erase(unit_path); }   // a re-gather replaces the unit's own entries, the names it dropped too
    pool_unit_globals(decls, {}, unit_path, out);
}

// a namespace's enum is named by its qualified spelling (ns::Name), as its classes are
static void collect_enums_of(const std::vector<declaration_ptr>& decls, const std::string& unit_path, program_enum_map& out) {
    for (const auto& d : decls) {
        if (d && d->get_type() == node_type::enum_decl) {
            auto ed = std::static_pointer_cast<const enum_decl>(d);
            out[std::string(ed->name)][unit_path] = std::move(ed);
        } else if (d && d->get_type() == node_type::namespace_decl) {
            collect_enums_of(static_cast<const namespace_decl*>(d.get())->declarations, unit_path, out);
        }
    }
}

void collect_unit_enums(const std::vector<declaration_ptr>& decls, const std::string& unit_path, program_enum_map& out) {
    for (auto& [name, units] : out) { units.erase(unit_path); }   // a re-gather replaces the unit's own entries
    collect_enums_of(decls, unit_path, out);
}

// a namespace's class is named by its qualified spelling (ns::Name)
bool collect_unit_classes(const std::vector<declaration_ptr>& decls, program_class_map& out) {
    bool result = false;
    for (const auto& d : decls) {
        if (!d) { continue; }
        if (d->get_type() == node_type::class_decl) {
            if (static_cast<const class_decl*>(d.get())->method_instances) { continue; }   // a generic method's instances: no class of their own
            auto& slot = out[std::string(static_cast<const class_decl*>(d.get())->name)];
            if (slot != d) {
                slot = std::static_pointer_cast<class_decl>(d);
                result = true;
            }
        } else if (d->get_type() == node_type::namespace_decl) {
            result = collect_unit_classes(static_cast<const namespace_decl*>(d.get())->declarations, out) || result;
        }
    }
    return result;
}

} // namespace detail
} // namespace jai
