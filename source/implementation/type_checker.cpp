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
#include <jaiscript/detail/inline_variable.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/native_stack.hpp>
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
#include <jaiscript/detail/constant_arguments.hpp>
#include <jaiscript/detail/regex_library.hpp>
#include <jaiscript/detail/template_library.hpp>

#include <algorithm>
#include <optional>
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
            out += " (declared at " + std::to_string(d.related_line) + ":" + std::to_string(d.related_column) + ")";
        }
        out += '\n';
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
    std::string_view cls{};   // object_k / class_ref_k / enum_ref_k
    // Proof pass only: the interned type_info this ctype came from (null = kind only), and
    // whether the kind was read off a runtime value tag rather than a declaration (engine
    // globals) - tentative kinds propagate through operators and are never proof sources.
    const type_info* ti = nullptr;
    bool tentative = false;
    // An element read from a declared, non-tentative typed container (array<int> x; x[i]): the kind is a
    // proof source for the operand class stamp (the typed rows guard it at runtime) while it stays
    // tentative for diagnostics, so no load-time text changes
    bool element_proof = false;
    // a reference alias of a declared typed container (var& x = G): its element reads may prove while
    // diagnostics still see a reference
    bool alias_proof = false;
    // A map key read through a range-for pair (`kv.first`, C++ `const Key`): copies freely,
    // never mutated or aliased (reference, shared_ptr, T& parameter, T& return, cast<T&>)
    bool const_key = false;
    // The class a reference names (T&, auto& of a known class, a range-for variable over class elements): member diagnostics
    // read it, while the receiver keeps the proof pass's view
    bool diagnostic_only = false;
    const expression* origin = nullptr; // this use's expression, for literal-only checks
    const type_info* yields = nullptr;  // coroutine_k: the declared type each yield converts to (null: undeclared)

    bool is_unknown() const { return kind == unknown; }
    static ctype make(kind_t k) { return ctype{k, {}}; }
    static ctype object(std::string_view name) { return ctype{object_k, name}; }
};

std::string ctype_name(const ctype& t) {
    switch (t.kind) {
        case ctype::null_k: return "null";
        case ctype::int_k: return "int";
        case ctype::float_k: return "float";
        case ctype::string_k: return "string";
        case ctype::utf8_k: return "utf8_string";
        case ctype::char_k: return "char";
        case ctype::bool_k: return "bool";
        case ctype::array_k: return t.ti ? t.ti->canonical_name() : "array";
        case ctype::map_k: return t.ti ? t.ti->canonical_name() : "map";
        case ctype::function_k: return "function";
        case ctype::weak_k: return t.ti ? t.ti->canonical_name() : "weak_ptr";
        case ctype::object_k:
            if (t.ti && t.ti->base_type == script_value_type::jai_shared_ptr_type) { return t.ti->canonical_name(); }
            return t.cls.empty() ? "object" : std::string(t.cls);
        case ctype::class_ref_k: return "class " + std::string(t.cls);
        case ctype::enum_ref_k: return "enum " + std::string(t.cls);
        case ctype::math_k: return std::string(t.cls);
        case ctype::coroutine_k: return "coroutine_handle";
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
    bool has_function(const std::string& name) const { note(name); return engine_.has_function(name); }
    bool is_type_name(const std::string& name) const { note(name); return engine_access::is_type_name(engine_, name); }
    bool is_const_global(const std::string& name) const { note(name); return engine_access::is_const_global(engine_, name); }
    held_value get_variable(const std::string& name) const { note(name); return engine_.get_variable(name); }
    std::shared_ptr<class_definition> get_class_definition(const std::string& name) const { note(name); return engine_access::get_class_definition(engine_, name); }
    std::shared_ptr<script_class_definition> find_script_class(const std::string& name) const { note(name); return engine_access::get_class_registry(engine_).find_script_class(name); }
    std::shared_ptr<class_definition> get_class_definition_by_type(const std::type_index& type) const {
        auto result = engine_access::get_class_definition_by_type(engine_, type);
        if (result) { note(result->get_name()); }
        return result;
    }
    bool host_function_signatures(const std::string& name, std::vector<engine::host_overload_signature>& out) const { note(name); return engine_.host_function_signatures(name, out); }
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
    bool host_function_result(const std::string& name, script_value_type& out) const { note(name); return engine_access::host_function_result(engine_, name, out); }
    bool host_nodiscard(const std::string& callable) const { note(callable); return engine_.is_nodiscard_function(callable); }
    std::vector<std::string> registered_class_names() const { return engine_.registered_class_names(); }
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
    bool has_constant_arguments() const { return engine_access::has_constant_arguments(engine_); }
    bool sleep_callee(const void* fn) const noexcept { return engine_access::sleep_callee(engine_, fn); }
    bool format_literal_callee(const void* fn) const noexcept { return engine_access::format_literal_for(engine_, fn) != nullptr; }
    uint8_t math_alias_fn(uint64_t symbol) const { return engine_access::math_alias_fn(engine_, symbol); }
    const std::unordered_set<uint64_t>& builtin_method_ids(int kind) const { return engine_access::builtin_method_ids(engine_, kind); }
    type_info* get_type_info_object(const std::string& cls) const { return engine_access::get_type_info_object(engine_, cls); }
    type_info* get_type_info_int() const { return engine_access::get_type_info_int(engine_); }
    type_info* get_type_info_float() const { return engine_access::get_type_info_float(engine_); }
    type_info* get_type_info_bool() const { return engine_access::get_type_info_bool(engine_); }
    type_info* get_type_info_any() const { return engine_access::get_type_info_any(engine_); }
    type_info* get_type_info_uint8() const { return engine_access::get_type_info_uint8(engine_); }
    type_info* get_type_info_array(type_info* element) const { return engine_access::get_type_info_array(engine_, element); }
    type_info* get_type_info_string() const { return engine_access::get_type_info_string(engine_); }
    type_info* get_type_info_map(type_info* key, type_info* value) const { return engine_access::get_type_info_map(engine_, key, value); }

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
            case node_type::enum_decl: member(static_cast<const enum_decl*>(d.get())->name); break;
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
    }

    check_report run() {
        stack_.arm();
        try {
            collect_top_level();
            scopes_.emplace_back();
            mode_guard unit(mode_stack_, leading_mode(decls_), /*scoped=*/true);
            warn_duplicate_top_level_functions();   // under the unit's [[warn_mode]] / [[strict_mode]]
            lw_open_region();
            for (const auto& d : decls_) {
                walk_decl(d.get());
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

private:
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
    static uint8_t leading_mode(const std::vector<declaration_ptr>& decls) {
        return decls.empty() || !decls.front() ? check_mode_attr_none : decls.front()->check_mode_attr;
    }

    std::unordered_map<std::string_view, sclass> classes_;
    // Program-closure classes, modeled on first use from the engine's table (a same-unit class
    // always wins over its table entry).
    const program_class_map* program_classes_ = nullptr;
    const program_function_map* program_functions_ = nullptr;
    const program_global_map* program_globals_ = nullptr;
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
    std::unordered_map<std::string, std::vector<const function_decl*>> namespace_fns_;   // "path::name" -> the unit's declarations of it
    // [[nodiscard]] (docs/site/guide/06-functions.html): what resolving the call being inferred found, and every such call the walk met;
    // twin names the in-place method a builtin namespace copy pairs with (empty for everything else)
    struct nodiscard_callee { std::string name; std::string twin; };
    nodiscard_callee nodiscard_hit_;
    std::unordered_map<const call_expr*, nodiscard_callee> nodiscard_calls_;
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
        const enum_decl* enum_def = nullptr;   // an enum declared in a function or block: local to that scope
        bool is_const = false;            // const T x / const T& p / a const reference (auto& over a const)
        ctype referent;                   // the class (or typed container) a reference or an auto range-for variable holds: member diagnostics only
        bool has_const_text = false;      // a const declared with a literal text (a + of them): a regex pattern the load can check
        std::string const_text;
        int lw = -1;                      // its lost-write candidate (lw_cands_), -1 for none
        bool lw_handle = false;           // a class-typed slot that may hold a handle (an auto from one, a by-value class parameter)
        bool placeholders = false;        // this scope declares '_' more than once: naming it is ambiguous (C++26 [basic.scope.scope])
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

    void diag(diag_level severity, const source_location& loc, std::string message,
              size_t related_line = 0, size_t related_column = 0) {
        if (!collect_diagnostics_) { return; }
        const uint8_t mode = region_mode();
        if (mode == check_mode_attr_warn) { severity = diag_level::warning; }
        if (diags_.size() >= k_check_diagnostic_cap) {
            capped_ = true;
            return;
        }
        std::string file = template_files_ > 0 ? loc.filename : std::string();
        std::string key = file + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column) + ":" + message;
        if (!seen_.insert(std::move(key)).second) {
            return;
        }
        diags_.push_back({severity, loc.line, loc.column, std::move(message), file.empty() ? related_line : 0, file.empty() ? related_column : 0, std::move(file)});
    }

    // C++ [basic.scope]: a block or a parameter list declares a name once; a program's top level may declare one again, the later
    // declaration standing for the earlier as a re-run's does. C++26's placeholder: a later '_' that names a local variable or a
    // structured binding (name_independent) redeclares freely, and the name then names none of them
    void declare_local(std::string_view name, var_entry entry, const source_location& at, bool name_independent = false) {
        auto [held, fresh] = scopes_.back().emplace(name, entry);
        if (fresh) { return; }
        if (name == "_" && (name_independent || scopes_.size() == 1)) {
            held->second = std::move(entry);
            held->second.placeholders = true;
            return;
        }
        if (scopes_.size() > 1) {
            diag(diag_level::error, at, "redeclaration of '" + std::string(name) + "' in the same scope", held->second.line, held->second.column);
        } else {
            held->second = std::move(entry);   // a program's later declaration stands for the name from here on, as it does at run time
        }
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
                         earlier->location.line, earlier->location.column);
                } else {
                    fields.emplace(name, d);
                }
            } else if (d->get_type() == node_type::function_decl) {
                const std::string_view name = static_cast<const function_decl*>(d)->name;
                if (auto field = fields.find(name); field != fields.end()) {
                    diag(diag_level::error, d->location, "redeclaration of member '" + std::string(name) + "' in class '" + std::string(cd->name) + "'",
                         field->second->location.line, field->second->location.column);
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

    void undeclared(const source_location& loc, std::string_view name) {
        // Literal imports are covered by the program closure's names; a dynamic path can
        // define anything - keep the signal, drop the teeth.
        diag(has_dynamic_includes_ ? diag_level::warning : diag_level::error, loc,
             "use of undeclared identifier '" + std::string(name) + "'");
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
        sclass& c = classes_[cd->name];
        model_class(cd, c);
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
    // A bare name in a method resolves locals, then members, then program-scope (and namespace) variables: a member of the
    // class being walked hides a program-scope name, as at run time
    bool member_hides_program_name(std::string_view name, size_t scope_index) {
        if (!class_ctx_) { return false; }
        const bool program_scope = scope_index == 0 ||
            std::find(namespace_scopes_.begin(), namespace_scopes_.end(), scope_index) != namespace_scopes_.end();
        return program_scope && lookup_member(class_ctx_name_, name, /*want_static=*/false).found;
    }
    // ::name reaches the unit's program scope only: locals and fields never shadow it
    var_entry* find_named(const identifier_expr* id, size_t* scope_index = nullptr) {
        if (!id->global_qualified) {
            size_t at = 0;
            var_entry* local = find_local(id->name, &at);
            if (local && (member_hides_program_name(id->name, at) || namespace_hides_program_name(id->name, at))) { return nullptr; }
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
                collect_namespace_vars(static_cast<const namespace_decl*>(d.get()), path);
            }
        }
    }

    // The declaration a qualified store `ns::x` names when the unit declares that namespace variable
    const variable_decl* namespace_variable(const member_expr* m, std::string* qualified) const {
        std::vector<std::string_view> segments;
        const expression* e = m->object.get();
        while (e && e->get_type() == node_type::member_expr) {
            segments.push_back(static_cast<const member_expr*>(e)->member);
            e = static_cast<const member_expr*>(e)->object.get();
        }
        if (!e || e->get_type() != node_type::identifier_expr) { return nullptr; }
        std::string path(static_cast<const identifier_expr*>(e)->name);
        for (size_t i = segments.size(); i-- > 0;) { path += "::"; path += segments[i]; }
        if (!namespaces_.count(std::string_view(path))) { return nullptr; }
        auto it = namespace_vars_.find(path + "::" + std::string(m->member));
        if (it == namespace_vars_.end() || !it->second) { return nullptr; }
        *qualified = it->first;
        return it->second;
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

    void collect_top_level() {
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
            const std::string sig = parameter_signature(fn);
            const std::string key = std::string(fn->name) + (fn->is_static ? "#static" : "") + sig;
            auto [it, inserted] = first_seen.emplace(key, fn);
            if (inserted) {   // an earlier overload of the same name and table that differs only by reference
                for (const function_decl* earlier : fns) {
                    if (earlier == fn) { break; }
                    if (earlier->name != fn->name || earlier->is_static != fn->is_static ||
                        !detail::differs_only_by_reference(fn->parameters, earlier->parameters)) { continue; }
                    diag(diag_level::error, fn->location,
                         detail::reference_only_overload_text(fn->name, fn->parameters, earlier->parameters, earlier->location.line),
                         earlier->location.line, earlier->location.column);
                    break;
                }
            }
            if (!inserted && !fn->is_override) {
                diag(diag_level::error, fn->location,
                     "'" + std::string(fn->name) + "' is already defined" + (by_name ? std::string() : " with parameters " + sig),
                     it->second->location.line, it->second->location.column);
                continue;
            }
            if (!by_name) { continue; }
            auto [nit, fresh] = first_name.emplace(fn->name, fn);
            if (!fresh && inserted && (fn->is_coroutine || nit->second->is_coroutine)) {
                diag(diag_level::warning, fn->location,
                     "duplicate definition of function '" + std::string(fn->name) +
                     "' replaces the earlier one (a coroutine does not overload)",
                     nit->second->location.line, nit->second->location.column);
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
            else if (d->get_type() == node_type::class_decl) { report_class_redefinitions(static_cast<const class_decl*>(d.get())); }
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

    void warn_duplicate_top_level_functions() {
        std::vector<const function_decl*> fns;
        std::map<std::string, std::vector<const function_decl*>> namespace_fns;
        for (const auto& d : decls_) {
            if (!d) { continue; }
            if (d->get_type() == node_type::function_decl) { fns.push_back(static_cast<const function_decl*>(d.get())); }
            else if (d->get_type() == node_type::class_decl) { report_class_redefinitions(static_cast<const class_decl*>(d.get())); }
            else if (d->get_type() == node_type::namespace_decl) { collect_namespace_functions(static_cast<const namespace_decl*>(d.get()), {}, namespace_fns); }
        }
        for (const auto& [path, members] : namespace_fns) { report_redefinitions(members, /*by_name=*/false); }
        report_redefinitions(fns, /*by_name=*/true);
        if (!program_functions_) { return; }
        std::unordered_set<std::string> warned;
        for (const function_decl* fn : fns) {
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
                    msg += " (field declared at " + std::to_string(hit->decl->location.line) + ":" +
                           std::to_string(hit->decl->location.column) + ")";
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

    ctype ctype_from_type_info(const type_info* ti) { return spelled_ctype_from_type_info(ti); }
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
             "Cannot access private member '" + std::string(member) + "' of class '" + std::string(hit.declarer) + "'");
        return true;
    }

    member_hit lookup_member(std::string_view cls, std::string_view member, bool want_static, int depth = 0) {
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
                // instance access reaches statics too (Cat().count, count++ in methods)
                if (auto f = c->static_fields.find(member); f != c->static_fields.end()) { return hit(f->second, nullptr); }
                if (auto m = c->static_methods.find(member); m != c->static_methods.end()) { return hit(nullptr, &m->second); }
            }
            member_hit result;
            for (auto base : c->decl->base_classes) {
                member_hit h = lookup_member(base, member, want_static, depth + 1);
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
                    "property '" + std::string(member) + "'", loc, 0, 0);
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
            std::string qualified;
            if (const variable_decl* nv = namespace_variable(m, &qualified)) {
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

    // a T& / var& / shared_ptr<T> declaration bound to a const (an auto& or a const T& takes it as a const reference)
    void check_const_bind(variable_decl* decl, var_entry& entry) {
        if (decl->is_const) {
            entry.is_const = true;
            entry.has_const_text = decl->initializer && detail::regex_library::constant_text(decl->initializer.get(), entry.const_text);
            stamp_const(decl->const_check, const_none, {});
            return;
        }
        if (!decl->initializer || !decl->type) { stamp_const(decl->const_check, const_none, {}); return; }
        const bool reference = decl->type->base_type == script_value_type::jai_reference_type;
        const bool handle = handle_type(decl->type.get());
        if (!reference && !handle) { stamp_const(decl->const_check, const_none, {}); return; }
        const const_hit h = const_path(decl->initializer.get(), /*value_too=*/handle);
        if (!h.verdict) { stamp_const(decl->const_check, const_none, {}); return; }
        const type_info* referent = reference ? const_referent(decl->type.get()) : nullptr;
        if (reference && !referent) {   // auto& deduces const T& (C++)
            entry.is_const = h.verdict == const_raises;
            stamp_const(decl->const_check, const_none, {});
            return;
        }
        if (handle && h.final_known && h.final_cls.empty()) { stamp_const(decl->const_check, const_none, {}); return; }
        const std::string declared = decl->type->canonical_name();
        auto text = [&] {
            return reference ? detail::const_bind_text(decl->name, declared, decl->initializer.get(), h.root, false)
                             : detail::const_share_text(decl->initializer.get(), h.root, declared);
        };
        stamp_const(decl->const_check, h.verdict, text);
        if (h.verdict == const_raises) { diag(diag_level::error, decl->location, detail::const_lowercase(text()), h.line, h.column); }
    }

    // cast<T&>(c) and cast<shared_ptr<T>>(c) name the const as writable (no const_cast)
    void check_const_cast(cast_expr* c) {
        if (c->implicit_ref || !(c->by_reference || handle_type(c->target.get()))) { stamp_const(c->const_check, const_none, {}); return; }
        const const_hit h = const_path(c->operand.get(), handle_type(c->target.get()));
        if (!h.verdict || (handle_type(c->target.get()) && h.final_known && h.final_cls.empty())) { stamp_const(c->const_check, const_none, {}); return; }
        const std::string target = c->target ? c->target->canonical_name() + (c->by_reference && c->target->base_type != script_value_type::jai_reference_type ? "&" : "") : std::string("T&");
        auto text = [&] { return handle_type(c->target.get()) ? detail::const_share_text(c->operand.get(), h.root, target) : detail::const_cast_text(c->operand.get(), h.root, target); };
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
                const std::string text = detail::const_argument_text(c->arguments[i].get(), h.root, fn->parameters[i].name, fn->name, w.line);
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
        return eng_.is_type_name(n) || (n.find("::") != std::string::npos && extra_global(n));   // a class another unit's namespace declares
    }

    // C++ [basic.lookup]: every type a declaration spells names a type; a template instance's own name is the host's, its
    // arguments are checked. A dynamic-path include can declare anything, as for an undeclared identifier.
    void check_spelled_type(const type_info* ti, const source_location& loc) {
        if (!ti) { return; }
        const bool object = ti->base_type == script_value_type::jai_object_type;
        if (object && ti->type_params.empty() && !ti->type_name.empty() && !known_type_name(ti->type_name)) {
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
        diag(diag_level::warning, c->location,
             "cast<" + cls + ">(" + src + ") copies the instance - use cast<" + cls + "&>(" + src +
             ") to alias it or cast<shared_ptr<" + cls + ">>(" + src + ") to share it",
             related_line, related_column);
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
        if (src.kind != dst.kind) { return false; }
        if (src.ti && dst.ti) {
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

    static bool literal_int_overflow(const ctype& src, const ctype& dst) {
        if (dst.kind != ctype::int_k || src.kind != ctype::float_k) { return false; }
        const auto value = literal_float(src.origin);
        return value && !float_fits_script_int(*value);
    }

    // a string literal into a char slot holds exactly one character (the conversion's text otherwise, detail/numeric_conversion.hpp)
    static bool literal_char_text_refused(const ctype& src, const ctype& dst) {
        if (dst.kind != ctype::char_k || src.kind != ctype::string_k || !src.origin || src.origin->get_type() != node_type::literal_expr) { return false; }
        const script_value& value = static_cast<const literal_expr*>(src.origin)->value;
        return value.is_string() && value.unchecked_as_string().size() != 1;
    }
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
        if (literal_int_overflow(src, dst)) { return "float literal is outside the int range"; }
        if (literal_char_text_refused(src, dst)) { return char_text_reason(); }
        if (auto issue = byte_literal_issue(src, target); !issue.empty()) { return issue; }
        if (dst.kind == ctype::array_k || dst.kind == ctype::map_k) {
            return container_issue(src, dst, depth + 1);
        }
        bool accepted = src.kind == dst.kind || char_text_slot(src, dst);
        if (is_numeric(src.kind) && is_numeric(dst.kind)) { accepted = true; }
        if ((src.kind == ctype::bool_k && dst.kind == ctype::int_k) ||
            (src.kind == ctype::int_k && dst.kind == ctype::bool_k)) { accepted = true; }
        if (target->base_type == script_value_type::jai_object_type) {
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
        if (accepted) { return {}; }
        return "expected '" + target->canonical_name() + "', got '" + ctype_name(src) + "'";
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
        const bool accepted = src.kind == dst.kind &&
            (!src.ti || src.ti->base_type == target->base_type);
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
            result += "; name one of its values (" + std::string(e.cls) + "." + std::string(decl->values.front().first) + ")";
        }
        return result;
    }

    std::string assignment_detail(const ctype& src, const ctype& dst) {
        if (src.kind == ctype::enum_ref_k) { return enum_table_detail(src, dst); }
        if (literal_int_overflow(src, dst)) { return ": float literal is outside the int range"; }
        if (literal_char_text_refused(src, dst)) { return ": " + char_text_reason(); }
        if ((dst.kind == ctype::string_k || dst.kind == ctype::utf8_k) && src.kind != ctype::string_k && src.kind != ctype::utf8_k) {
            return ": stringify it with to_string(x) or `${x}`";
        }
        if (dst.kind == ctype::array_k || dst.kind == ctype::map_k) {
            auto issue = container_issue(src, dst);
            if (!issue.empty()) { return ": " + issue; }
        }
        return {};
    }

    bool class_name_constructs(std::string_view cls) {
        const std::string name(cls);
        return find_sclass(cls) || eng_.find_script_class(name) || eng_.has_function(name);
    }

    bool assignable(const ctype& src, const ctype& dst) {
        if (dst.is_unknown() || src.is_unknown()) { return true; }
        if (literal_int_overflow(src, dst) || literal_char_text_refused(src, dst)) { return false; }
        if (src.kind == ctype::class_ref_k && dst.kind != ctype::class_ref_k && dst.kind != ctype::enum_ref_k && class_name_constructs(src.cls)) {
            return assignable(ctype::make(ctype::function_k), dst);   // a class's name as a value is its constructor
        }
        if (src.kind == ctype::enum_ref_k && dst.kind != ctype::class_ref_k && dst.kind != ctype::enum_ref_k) { return assignable(enum_table(src), dst); }
        if (dst.kind == ctype::class_ref_k || dst.kind == ctype::enum_ref_k ||
            src.kind == ctype::class_ref_k || src.kind == ctype::enum_ref_k) {
            return true;   // exotic; stay lenient
        }
        switch (dst.kind) {
            case ctype::int_k:
                return src.kind == ctype::int_k || src.kind == ctype::float_k || src.kind == ctype::bool_k ||
                       may_convert_via_method(src, "to_int");
            case ctype::float_k:
                return src.kind == ctype::float_k || src.kind == ctype::int_k || src.kind == ctype::bool_k ||
                       may_convert_via_method(src, "to_float");
            case ctype::bool_k: return true;     // truthy conversion accepts anything
            case ctype::string_k:
            case ctype::utf8_k: return src.kind == ctype::string_k || src.kind == ctype::utf8_k || src.kind == ctype::char_k || may_convert_via_method(src, "to_string");
            case ctype::char_k: return src.kind == ctype::char_k || src.kind == ctype::string_k || may_convert_via_method(src, "to_char");
            case ctype::array_k:
            case ctype::map_k: return container_issue(src, dst).empty();
            case ctype::function_k: return src.kind == ctype::function_k || src.kind == ctype::null_k;
            case ctype::weak_k: return weak_initializer_compatible(src, dst);
            case ctype::null_k: return src.kind == ctype::null_k;   // void returns
            case ctype::math_k: {
                // the same kind, or the other precision of the same dimensions (the typed-slot table's row)
                if (src.kind != ctype::math_k) { return false; }
                return detail::math::converts(math_type_from_name(src.cls), math_type_from_name(dst.cls));
            }
            case ctype::object_k: {
                if (src.kind == ctype::null_k) { return true; }
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

    // Constructor-conversion fallback Target(src): checkable only for in-script classes;
    // engine-registered targets stay lenient.
    bool may_construct_from(std::string_view target_cls, const ctype& src) {
        const sclass* c = find_sclass(target_cls);
        if (!c) {
            return engine_class(target_cls) != nullptr;   // registered: lenient
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
    // int<->float. Never another conversion (no chaining, no to_X() on the way in).
    bool ctor_param_takes(const ctype& src, const ctype& param) {
        if (param.is_unknown() || src.is_unknown()) { return true; }
        if (param.kind == ctype::object_k) {
            return src.kind == ctype::object_k && class_relation(src.cls, param.cls) != 0;
        }
        if (src.kind == ctype::object_k) { return false; }
        if (param.kind == src.kind) { return true; }
        return (param.kind == ctype::int_k && src.kind == ctype::float_k) ||
               (param.kind == ctype::float_k && src.kind == ctype::int_k);
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
            case math_fn::perpendicular_: case math_fn::rotate_: case math_fn::distance: case math_fn::distance_squared: {
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
                                 fn != math_fn::perpendicular_ && !(fn == math_fn::rotate_ && args.size() == 2);
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
                    case math_fn::length_: case math_fn::angle_between_: case math_fn::distance:
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
            case math_fn::vec2_: return math_ctype(script_value_type::jai_vec2_type);
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
    }
    void check_namespace_function_nodiscard(const std::string& qualified, const std::vector<const function_decl*>& decls) {
        if (decls.empty()) { return; }
        for (const auto* d : decls) { if (!d->is_nodiscard) { return; } }
        nodiscard_hit_ = {qualified, {}};
    }

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

    // A host namespace entry with a declared signature (script_namespace_data::signatures): arity and argument kinds are
    // certainties the entry's own run-time check would raise; a unit that declares the namespace, or a script function of
    // the name, keeps the call lenient
    ctype check_namespace_entry(const member_expr* m, const std::vector<ctype>& args, const source_location& loc) {
        std::vector<std::string_view> segments;
        const expression* e = m->object.get();
        while (e && e->get_type() == node_type::member_expr) {
            segments.push_back(static_cast<const member_expr*>(e)->member);
            e = static_cast<const member_expr*>(e)->object.get();
        }
        if (!e || e->get_type() != node_type::identifier_expr) { return {}; }
        std::string path(static_cast<const identifier_expr*>(e)->name);
        for (size_t i = segments.size(); i-- > 0;) { path += "::"; path += segments[i]; }
        const std::string qualified = path + "::" + std::string(m->member);
        if (namespaces_.count(std::string_view(path))) {
            if (auto fit = namespace_fns_.find(qualified); fit != namespace_fns_.end()) { check_namespace_function_nodiscard(qualified, fit->second); }
            return {};
        }
        script_namespace_data* space = eng_.find_namespace(path);
        if (!space) { return {}; }
        eng_.note(qualified);
        const uint64_t member_id = eng_.symbolize(m->member);
        if (auto fit = space->functions.find(member_id); fit != space->functions.end()) {
            std::vector<const function_decl*> decls;
            for (const auto& d : fit->second) { if (d) { decls.push_back(d.get()); } }
            check_namespace_function_nodiscard(qualified, decls);
            return {};
        }
        auto sit = space->signatures.find(member_id);
        if (sit == space->signatures.end()) { return {}; }
        const host_entry_signature& sig = sit->second;
        if (sig.nodiscard) { nodiscard_hit_ = {qualified, sig.in_place_twin ? std::string(m->member) : std::string()}; }
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
            check_array_sizing_arguments(m->member, args[0], args.data() + 1, args.size() - 1, loc);
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
        if (known_type_name(qualified) || find_sclass(qualified)) { return false; }   // the parser reads a namespace's bare class name qualified
        if (namespace_members_.count(qualified) || extra_global(qualified) || eng_.has_variable(qualified) || eng_.has_function(qualified)) { return true; }
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
        if (space.empty() || namespace_declares(space, parts[next])) { return; }
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
    void coroutine_member_missing(const member_expr* a_member) {
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
        diag(diag_level::error, a_member->location, "no member '" + std::string(a_member->member) + "' on 'coroutine_handle'" +
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
            return {};
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
        coroutine_member_missing(a_member);
        return {};
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

    // A call that uses the result of a builtin that changes its known receiver in place and returns nothing (detail/in_place_result.hpp)
    void in_place_result_used(const ctype& a_receiver, const member_expr* a_member, const call_expr* a_call) {
        if (a_member->call_result_discarded) { return; }
        std::optional<std::string_view> twin;
        switch (a_receiver.kind) {
            case ctype::array_k: twin = in_place_method_twin(0, a_member->member); break;
            case ctype::map_k: twin = in_place_method_twin(1, a_member->member); break;
            case ctype::string_k: case ctype::utf8_k: twin = in_place_method_twin(2, a_member->member); break;
            case ctype::weak_k: case ctype::object_k: if (a_member->member == "reset") { twin = std::string_view{}; } break;
            default: break;
        }
        if (!twin) { return; }
        diag(diag_level::error, a_call->location, detail::in_place_result_text(a_member->member, detail::method_value_receiver(a_member), *twin));
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
        diag(diag_level::error, m->location, detail::method_value_text(m->member, owner, detail::method_value_receiver(m), true));
    }
    // A member of a known int, float, bool or char: the registry (BUILTIN_ARITIES) gives these kinds no methods and same_as answers
    // every value, so any other name is the run time's text; same_as read without a call is the method-value text
    void scalar_member_missing(const ctype& recv, const member_expr* m, bool called) {
        script_value_type kind;
        switch (recv.kind) {
            case ctype::int_k: kind = script_value_type::jai_int_type; break;
            case ctype::float_k: kind = script_value_type::jai_float_type; break;
            case ctype::bool_k: kind = script_value_type::jai_bool_type; break;
            case ctype::char_k: kind = script_value_type::jai_char_type; break;
            default: return;
        }
        if (recv.tentative || recv.diagnostic_only || m->is_static) { return; }
        if (m->member != "same_as") {
            diag(diag_level::error, m->location, no_member_text(base_type_spelling(kind), m->member));
        } else if (!called) {
            diag(diag_level::error, m->location, detail::method_value_text(m->member, base_type_spelling(kind), detail::method_value_receiver(m), true));
        }
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

    // Handle builtins every object value answers (shared_ptr registry + the universal
    // same_as): a class method of the same name wins first, so this runs on misses only.
    static bool handle_builtin_result(std::string_view member, ctype& out) {
        if (member == "use_count" || member == "cpp_ref_count") { out = ctype::make(ctype::int_k); return true; }
        if (member == "unique" || member == "same_as") { out = ctype::make(ctype::bool_k); return true; }
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
    ctype check_strict_host_method(std::string_view cls, std::string_view member, const std::vector<ctype>& args, const source_location& loc) {
        auto def = engine_class(cls);
        if (!def || !def->is_cpp_class() || !def->strict_signatures() || def->extended_ever()) { return {}; }
        const auto& table = def->cpp_method_overload_table();
        auto it = table.find(eng_.symbolize(member));
        if (it == table.end()) { return {}; }
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
        std::optional<ctype> result;
        bool agree = true;
        bool viable = false;
        for (const auto& entry : bucket->second) {
            bool ok = true;
            for (size_t i = 0; i < args.size() && i < entry.param_types.size(); ++i) {
                if (!host_param_accepts(args[i], entry.param_types[i])) { ok = false; break; }
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
        return agree && result ? *result : ctype{};
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
                                  const std::vector<ctype>& args, const source_location& loc) {
        std::vector<const function_decl*> all(own);
        all.insert(all.end(), foreign.begin(), foreign.end());
        const bool rivals = arity_rivals(all, args.size());
        const function_decl* only_viable = nullptr;
        int viable_count = 0;
        int nodiscard_viable = 0;
        for (const auto* fn : all) {
            if (!script_fn_viable(fn, args, rivals)) { continue; }
            ++viable_count;
            nodiscard_viable += fn->is_nodiscard;
            only_viable = fn;
        }
        if (viable_count == 0) {
            report_no_overload(fname, all, args, loc);
            return {};
        }
        if (nodiscard_viable == viable_count) { nodiscard_hit_ = {std::string(fname), {}}; }
        if (viable_count > 1 || std::find(own.begin(), own.end(), only_viable) == own.end()) { return {}; }
        ctype result = only_viable->is_coroutine ? ctype::make(ctype::coroutine_k) : ctype_from_type_info(only_viable->return_type.get());
        if (only_viable->is_coroutine) { result.yields = only_viable->return_type.get(); }
        return result;
    }

    // args already inferred by the caller; foreign = the overloads belong to another unit's source; scored = the runtime picks
    // among them by type (methods); a program's free functions of one name are redefinitions, the last one taking the call
    call_check check_script_overloads(std::string_view fname, const std::vector<const function_decl*>& overloads,
                                      const std::vector<ctype>& args, const source_location& loc, bool foreign = false,
                                      bool scored = true) {
        const function_decl* only_viable = nullptr;
        int viable_count = 0;
        int nodiscard_viable = 0;
        for (const auto* fn : overloads) {
            if (script_fn_viable(fn, args, scored && arity_rivals(overloads, args.size()))) {
                ++viable_count;
                nodiscard_viable += fn->is_nodiscard;
                only_viable = fn;
            }
        }
        if (viable_count > 0 && nodiscard_viable == viable_count) { nodiscard_hit_ = {std::string(fname), {}}; }
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
            if (a.kind == pt.kind) { continue; }
            const bool number_to = pt.kind == ctype::int_k || pt.kind == ctype::float_k;
            if (number_to && (a.kind == ctype::int_k || a.kind == ctype::float_k)) { cost(1); continue; }
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
             best->location.line, best->location.column);
    }

    // A T& parameter's argument: a primitive of exactly that kind, a class T or a subclass; unknown and var& / auto& pass; a const T&
    // binds a temporary T made from whatever a T parameter takes (C++)
    bool reference_parameter_binds(const parameter& p, const ctype& arg) {
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

    void report_no_overload(std::string_view fname, const std::vector<const function_decl*>& overloads,
                            const std::vector<ctype>& args, const source_location& loc, bool foreign = false) {
        size_t rl = (overloads.empty() || foreign) ? 0 : overloads[0]->location.line;
        size_t rc = (overloads.empty() || foreign) ? 0 : overloads[0]->location.column;
        if (overloads.size() == 1) {
            const function_decl* fn = overloads[0];
            auto [min_a, max_a] = arity_range(fn);
            if (args.size() < min_a || args.size() > max_a) {
                std::string expected = (min_a == max_a)
                    ? std::to_string(min_a)
                    : std::to_string(min_a) + " to " + std::to_string(max_a);
                diag(diag_level::error, loc, "'" + std::string(fname) + "' expects " + expected +
                     " argument(s), got " + std::to_string(args.size()), rl, rc);
                return;
            }
            for (size_t i = 0; i < args.size(); ++i) {
                const parameter& p = fn->parameters[i];
                if (p.is_reference) {
                    if (!reference_parameter_binds(p, args[i])) {
                        diag(diag_level::error, loc, "argument " + std::to_string(i + 1) + " of '" + std::string(fname) + "': cannot bind '" +
                             ctype_name(args[i]) + "' to '" + p.name + "' declared '" + (p.is_const ? "const " : "") + p.type->canonical_name() + "&'", rl, rc);
                        return;
                    }
                    continue;
                }
                ctype pt = ctype_from_type_info(p.type.get());
                if (!assignable(args[i], pt)) {
                    diag(diag_level::error, loc, "argument " + std::to_string(i + 1) + " of '" +
                         std::string(fname) + "': cannot pass '" + ctype_name(args[i]) + "' to '" +
                         p.name + "' declared '" + (p.type ? p.type->canonical_name() : "auto") + "'" + assignment_detail(args[i], pt),
                         rl, rc);
                    return;
                }
            }
            return;
        }
        diag(diag_level::error, loc, "no overload of '" + std::string(fname) + "' matches this call (" +
             std::to_string(args.size()) + " argument(s))", rl, rc);
    }

    // Host (C++) function surface: reject only certainties — arity misses across every
    // overload, or a known-primitive argument no builtin conversion reaches.
    void check_host_call(const std::string& fname, const std::vector<ctype>& args, const source_location& loc) {
        std::vector<engine::host_overload_signature> sigs;
        if (!eng_.host_function_signatures(fname, sigs) || sigs.empty()) {
            return;
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
            if (!host_param_accepts(args[i], sig.param_types[i])) {
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
        for (const auto& init : fn->initializers) {
            if (init.target == "this") { delegates = true; continue; }
            size_t index = init.target == "super" && !bases.empty() ? 0 : SIZE_MAX;
            for (size_t i = 0; index == SIZE_MAX && i < bases.size(); ++i) {
                if (detail::initializer_names(init.target, bases[i])) { index = i; }
            }
            if (index == SIZE_MAX) {
                if (init.target != "super") { diag(diag_level::error, fn->location, format_error_message(detail::k_not_a_base, class_ctx_name_, init.target)); }
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

    void check_ctor_call(std::string_view cls_name, const sclass& c, const std::vector<ctype>& args,
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
                             c.foreign ? 0 : c.decl->location.line, c.foreign ? 0 : c.decl->location.column);
                        return;
                    }
                    for (size_t i = 0; i < args.size(); ++i) {
                        const variable_decl* f = fields[i];
                        if (!f->type || f->type->base_type == script_value_type::jai_any_type) { continue; }
                        check_store(args[i], false, token_type::equal, ctype_from_type_info(f->type.get()), declared_variable_type_name(f),
                                    "field '" + std::string(f->name) + "'", loc, 0, 0);
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
                     c.foreign ? 0 : c.decl->location.line, c.foreign ? 0 : c.decl->location.column);
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

    // -------------------------------------------------------------- expressions

    JAI_NOINLINE ctype infer(expression* e) {
        if (e) { stack_.guard(e->location); }
        ctype result = infer_impl(e);
        result.origin = e;
        if (observer_ && e) { observe(e, result); }
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
        view.name = ctype_name(t);
        observer_->expression_type(e, view);
    }

    JAI_NOINLINE ctype infer_impl(expression* e) {
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
            case node_type::lambda_expr:
                walk_lambda(static_cast<lambda_expr*>(e));
                return ctype::make(ctype::function_k);
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
                return infer_ternary(static_cast<ternary_expr*>(e));
            case node_type::array_literal_expr:
                return infer_array_literal(static_cast<array_literal_expr*>(e));
            case node_type::map_literal_expr:
                return infer_map_literal(static_cast<map_literal_expr*>(e));
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
                if (y->value) { infer(y->value.get()); }
                return {};
            }
            default:
                return {};
        }
    }

    JAI_NOINLINE ctype infer_binary(binary_expr* b) {
        ctype l = infer(b->left.get());
        ctype r = infer(b->right.get());
        return binary_type(b, l, r);
    }

    JAI_NOINLINE ctype binary_type(binary_expr* b, ctype& l, ctype& r) {
        if (b->op.type == token_type::left_bracket) {
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

    // ++ and -- take numbers and chars; no class operator answers them
    bool incdec_refused(const ctype& t) {
        return settled_operand(t) && t.kind != ctype::int_k && t.kind != ctype::float_k && t.kind != ctype::char_k;
    }

    // x op= rhs runs x op rhs: the binary rows above, then the store; the diagnostic names the compound spelling
    bool check_compound_operands(const ctype& target, token_type base_op, const ctype& rhs, const source_location& loc, size_t decl_line, size_t decl_col) {
        if (incdec_op_) {
            if (!incdec_refused(target)) { return false; }
            diag(diag_level::error, loc, std::string("cannot apply '") + incdec_op_ + "' to '" + ctype_name(target) + "'", decl_line, decl_col);
            return true;
        }
        const binary_refusal refusal = builtin_binary_refusal(target, base_op, rhs);
        if (!refusal.op) { return false; }
        diag(diag_level::error, loc, builtin_binary_message(std::string(refusal.op) + "=", refusal, target, rhs, /*compound=*/true), decl_line, decl_col);
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
                return is_numeric(operand.kind) || (operand.kind == ctype::math_k && u->op.type == token_type::plus) ? operand : ctype{};
            case token_type::plus_plus:
            case token_type::minus_minus:
                check_incdec_target(u);
                if (incdec_refused(operand)) {
                    diag(diag_level::error, u->location, std::string("cannot apply '") + (u->op.type == token_type::plus_plus ? "++" : "--") + "' to '" + ctype_name(operand) + "'");
                } else if (operand.kind == ctype::char_k && !operand.tentative && !operand.diagnostic_only && u->operand->get_type() == node_type::identifier_expr) {
                    // a char promotes, so the int it steps to reaches a slot declared char (a non-name target checks as its += 1 store)
                    auto* id = static_cast<identifier_expr*>(u->operand.get());
                    const bool field = class_ctx_ && !id->global_qualified && !find_named(id) && lookup_member(class_ctx_name_, id->name, /*want_static=*/false).field;
                    diag(diag_level::error, u->location, "cannot assign 'int' to " + std::string(field ? "field '" : "'") + std::string(id->name) + "' declared 'char'");
                }
                return is_numeric(operand.kind) ? operand : ctype{};
            case token_type::tilde:
                return operand.kind == ctype::int_k ? operand : ctype{};
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
        if (t.kind == ctype::object_k) {
            if (n->type && n->type->base_type == script_value_type::jai_shared_ptr_type) { note_handle_held(t.cls, n->location); }
            if (const sclass* c = find_sclass(t.cls)) {
                check_ctor_call(t.cls, *c, args, n->location);
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

    JAI_NOINLINE ctype infer_ternary(ternary_expr* t) {
        infer(t->condition.get());
        ctype a = infer(t->then_expression.get());
        ctype b = infer(t->else_expression.get());
        if (a.kind == b.kind && a.cls == b.cls) {
            if (a.ti != b.ti) { a.ti = nullptr; }   // two spellings (value vs handle) are not one certain type
            a.tentative = a.tentative || b.tentative;
            a.const_key = a.const_key || b.const_key;
            return a;
        }
        if (!a.is_unknown() && !b.is_unknown() && a.kind != ctype::null_k && b.kind != ctype::null_k) {   // a T-or-null arm pair is a nullable value, never a type mix
            last_mixed_ternary_ = {t, a, b};
            mixed_ternaries_[t] = last_mixed_ternary_;
        }
        ctype mixed{};
        mixed.const_key = a.const_key || b.const_key;
        return mixed;
    }

    JAI_NOINLINE ctype infer_array_literal(array_literal_expr* a) {
        for (auto& el : a->elements) { literal_element_types_[el.get()] = infer(el.get()); }
        return ctype::make(ctype::array_k);
    }

    JAI_NOINLINE ctype infer_map_literal(map_literal_expr* m) {
        for (auto& [k, v] : m->entries) {
            literal_element_types_[k.get()] = infer(k.get());
            literal_element_types_[v.get()] = infer(v.get());
        }
        return ctype::make(ctype::map_k);
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
    JAI_NOINLINE ctype infer_cast(cast_expr* c) {
        if (c->implicit_ref) {   // a piece of an assignment target read as a reference: its operand's type
            ctype operand_t = infer(c->operand.get());
            operand_t.tentative = true;
            return operand_t;
        }
        ctype operand_t = infer(c->operand.get());
        if (c->by_reference) { report_mixed_reference_conditional(c->operand.get()); }
        check_const_cast(c);
        ctype t = ctype_from_type_info(c->target.get());
        if (t.kind == ctype::object_k && !known_class(t.cls) && !extra_global(std::string(t.cls))) {
            diag(diag_level::error, c->location, "unknown type in cast: '" + std::string(t.cls) + "'");
            return {};
        }
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
                if (namespace_member_unready(space, id->name, id->location, false)) { return {}; }
                return namespace_fns_.count(space + "::" + std::string(id->name)) ? ctype::make(ctype::function_k) : ctype{};
            }
        }
        if (functions_.count(id->name)) {
            note_global_use(id->name);
            report_function_before_declaration(id->name, id->location, false);
            return ctype::make(ctype::function_k);
        }
        if (find_sclass(id->name)) {
            report_class_before_declaration(id->name, id->location);
            return ctype{ctype::class_ref_k, id->name};
        }
        if (enums_.count(id->name)) { return ctype{ctype::enum_ref_k, id->name}; }
        std::string name(id->name);
        if (namespaces_.count(id->name)) {
            if (!program_global(name) && !extra_global(name)) { namespace_as_value(id->location, id->name); }
            return {};
        }
        report_use_before_declaration(id->name, id->location, false, [&] { return eng_.has_variable(name); });
        if (const variable_decl* g = program_global(name)) { note_global_use(id->name); return program_global_type(g); }   // the proof pass alone types a global another unit declares
        if (extra_global(name)) { note_global_use(id->name); return {}; }
        if (eng_.is_type_name(name) || eng_.find_script_class(name)) {
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
        undeclared(id->location, id->name);
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
                const var_entry* local = find_named(id);
                return local ? local->referent : ctype{};
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
        if (m->is_static && namespace_path_receiver(m)) {
            check_namespace_member(m);
            if (const var_entry* x = qualified_extern(m); x && !x->dynamic) { return x->type; }
            return {};
        }
        ctype recv = infer(m->object.get());
        if (recv_out) { *recv_out = recv; }
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
            return c;
        }
        if (recv.kind == ctype::coroutine_k) {
            if (!recv.tentative && m->member != "next" && m->member != "done" && m->member != "advance" && m->member != "result") { coroutine_member_missing(m); }
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
            if (!writing && !recv.tentative && !m->is_static) {
                builtin_method_value(recv, m);
                scalar_member_missing(recv, m, false);
            }
            return {};   // builtin methods / map keys / dynamic: lenient
        }
        return infer_class_member(m, recv, writing, typed_field_out, hit_out);
    }

    // A member of a class, an enum or a class used as a value; a reference's referent (diagnostic_only) is checked alike
    ctype infer_class_member(member_expr* m, const ctype& recv, bool writing, const variable_decl** typed_field_out,
                             member_hit* hit_out) {
        if (extended_members_.count(m->member)) { return {}; }
        if (class_dot_receiver(m, recv)) { return {}; }
        if (recv.kind == ctype::enum_ref_k) {
            if (const enum_decl* e = find_enum(recv.cls)) {
                for (const auto& [vname, _] : e->values) {
                    if (vname == m->member) { return ctype::make(ctype::int_k); }
                }
                diag(diag_level::error, m->location, "no value '" + std::string(m->member) +
                     "' in enum '" + std::string(recv.cls) + "'",
                     e->location.line, e->location.column);
            }
            return {};
        }
        bool want_static = (recv.kind == ctype::class_ref_k);
        std::string_view cls = recv.cls;
        member_hit hit = lookup_member(cls, m->member, want_static);
        if (access_denied(hit, m->member, m->location)) { return {}; }
        if (hit_out) { *hit_out = hit; }
        if (!want_static && chain_has_user_accessor(cls, m->member)) { return accessor_read_type(cls, m->member); }   // a user _get_/_set_ answers: known member, typed only by a single declared getter, never a proof
        if (hit.found) {
            if (!writing && !want_static && m->object->get_type() == node_type::this_expr) { report_unready_field_read(hit.field, m->location); }
            if (typed_field_out && hit.field && (hit.field->type || hit.field->function_spelling)) { *typed_field_out = hit.field; }
            if (hit.methods) {
                if (!writing && !want_static) {   // a method is called or bound, never read bare
                    diag(diag_level::error, m->location, detail::method_value_text(m->member, cls, detail::method_value_receiver(m), false));
                }
                return ctype::make(ctype::function_k);
            }
            if (hit.field && (hit.field->type || hit.field->function_spelling)) { return member_type(hit); }
            return {};
        }
        if (!hit.certain) { return {}; }
        if (stale_shape_receiver(recv, m->object.get())) { return {}; }
        if (!want_static) {
            if (ctype t; handle_builtin_result(m->member, t)) { return {}; }   // builtin as a value: lenient
        }
        size_t rl = 0, rc = 0;
        if (auto it = classes_.find(cls); it != classes_.end()) {
            rl = it->second.decl->location.line;
            rc = it->second.decl->location.column;
        }
        diag(diag_level::error, m->location,
             std::string(want_static ? "no static member '" : "no member '") + std::string(m->member) +
             "' in class '" + std::string(cls) + "'", rl, rc);
        (void)writing;
        return {};
    }

    // engine::constant_argument: an argument only the program's constants may fill must be a constant form
    void check_constant_arguments(const std::string& callable, const std::string& shown, const call_expr* c) {
        if (!eng_.has_constant_arguments()) { return; }
        for (const auto& [index, refusal] : eng_.constant_argument_indexes(callable)) {
            if (index >= c->arguments.size()) { continue; }
            std::vector<std::string> texts;
            if (detail::constant_texts(c->arguments[index].get(), eng_.constant_texts_by_name(), texts)) { continue; }
            diag(diag_level::error, c->arguments[index]->location, "argument " + std::to_string(index + 1) + " of '" + shown + "' is not a constant: " + *refusal);
        }
    }

    JAI_NOINLINE ctype infer_call(call_expr* c) {
        std::vector<ctype> args;
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
        for (size_t k = 0; k < c->arguments.size(); ++k) {
            auto& a = c->arguments[k];
            const bool combine = callee_name == "parallel_reduce" && k == 3;   // runs serially after the join
            const bool body = parallel_callee && !combine && a->get_type() == node_type::lambda_expr;
            parallel_body_depth_ += body;
            args.push_back(infer(a.get()));
            parallel_body_depth_ -= body;
        }
        const std::vector<const function_decl*>* ov = nullptr;   // the statically resolved same-script overload set
        ov_class_ = {};
        nodiscard_hit_ = {};
        ctype t = infer_call_impl(c, args, ov);
        t = check_call(c, args, ov, std::move(t));
        if (!nodiscard_hit_.name.empty() && collect_diagnostics_) { nodiscard_calls_[c] = std::move(nodiscard_hit_); }
        nodiscard_hit_ = {};
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
        if (!ov && program_functions_ && c->callee && c->callee->get_type() == node_type::identifier_expr) {
            const std::string_view name = static_cast<identifier_expr*>(c->callee.get())->name;
            if (functions_.find(name) == functions_.end()) {
                eng_.note(name);
                auto pit = program_functions_->find(std::string(name));
                if (pit != program_functions_->end() && pit->second.size() == 1 && pit->second.begin()->second.size() == 1) { program_ov.push_back(pit->second.begin()->second[0].get()); }
                if (program_ov.size() == 1 && program_ov[0]->is_nodiscard) { nodiscard_hit_ = {std::string(name), {}}; }
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
                const node_type k = c->arguments[i]->get_type();
                if (k != node_type::literal_expr && k != node_type::array_literal_expr && k != node_type::map_literal_expr) { continue; }
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
        const std::string qualified = space + "::" + std::string(name);
        if (eng_.has_variable(qualified) || eng_.has_function(qualified)) { return false; }
        if (const script_namespace_data* ns = eng_.find_namespace(space)) {
            const uint64_t id = eng_.symbolize(name);
            if (ns->functions.count(id) || ns->variables.count(id) || (ns->scope && ns->scope->get_own_value_ptr(id) != nullptr)) { return false; }
        }
        const std::string bare(name);
        bool outside = functions_.count(name) || all_names_.count(name) || find_sclass(name) || enums_.count(name) || extra_global(bare) || engine_knows_name(bare);
        for (std::string outer = space; !outside && outer.rfind("::") != std::string::npos;) {
            outer.resize(outer.rfind("::"));
            outside = namespace_member_declared(outer, name);
        }
        if (!outside) {
            const char* use = call ? "call" : "use";
            diag(diag_level::error, at, "'" + bare + "' is " + (call ? "called" : "used") + " before its declaration (line " + std::to_string(first->second.line) +
                                        "): top-level code runs in order, so declare it above the " + use + " or make the " + use + " after it");
        }
        return true;
    }

    ctype infer_call_impl(call_expr* c, const std::vector<ctype>& args, const std::vector<const function_decl*>*& ov) {
        expression* callee = c->callee.get();
        if (callee->get_type() == node_type::identifier_expr) {
            auto* id = static_cast<identifier_expr*>(callee);
            if (id->name == "weak_from_this" || id->name == "shared_from_this") { return {}; }   // call-shape builtins, resolved before any lookup
            stamp(id->program_name, semantic_walk() && bare_program_name(id), false);
            size_t scope_index = 0;
            if (find_local(id->name, &scope_index) && (id->global_qualified ||
                (!member_hides_program_name(id->name, scope_index) && !namespace_hides_program_name(id->name, scope_index)))) {
                if (scope_index == 0) { note_global_use(id->name); }
                return {};   // callable local: signature unknown
            }
            if (class_ctx_ && !id->global_qualified) {
                member_hit hit = lookup_member(class_ctx_name_, id->name, /*want_static=*/false);
                if (hit.found) {
                    if (access_denied(hit, id->name, id->location)) { return {}; }
                    if (hit.methods) {
                        if (hit.provable) { ov = hit.methods; ov_class_ = class_ctx_name_; }
                        ctype r = check_script_overloads(id->name, *hit.methods, args, c->location, hit.foreign).result;
                        if (!nodiscard_hit_.name.empty()) { nodiscard_hit_.name = std::string(class_ctx_name_) + "." + nodiscard_hit_.name; }
                        if (!hit.provable) { r.tentative = true; }
                        return r;
                    }
                    return {};
                }
                if (hit.unresolved) { return {}; }
            }
            if (!id->global_qualified) {
                if (const std::string space = enclosing_namespace_member(id->name); !space.empty()) {
                    if (namespace_member_unready(space, id->name, c->location, true)) { return {}; }
                    const std::string qualified = space + "::" + std::string(id->name);
                    if (auto fit = namespace_fns_.find(qualified); fit != namespace_fns_.end()) {
                        check_namespace_function_nodiscard(qualified, fit->second);
                        return check_script_overloads(id->name, fit->second, args, c->location).result;
                    }
                    return {};
                }
            }
            if (auto it = functions_.find(id->name); it != functions_.end()) {
                note_global_use(id->name);
                report_function_before_declaration(id->name, c->location, true);
                if (const auto& foreign = foreign_overloads(id->name, it->second); !foreign.empty()) {
                    return check_program_overloads(id->name, it->second, foreign, args, c->location);
                }
                ov = &it->second;
                return check_script_overloads(id->name, it->second, args, c->location).result;
            }
            if (const sclass* sc = find_sclass(id->name)) {
                report_class_before_declaration(id->name, c->location);
                check_ctor_call(id->name, *sc, args, c->location);
                if (sc->provable) { ov = &sc->ctors; }
                return object_of(id->name);
            }
            if (enums_.count(id->name)) { return {}; }
            std::string name(id->name);
            if (extra_global(name)) { note_global_use(id->name); return {}; }
            if (name == "regex") { check_regex_constant(c); }
            if (eng_.is_type_name(name) || eng_.find_script_class(name)) {
                check_strict_host_constructor(name, args, c->location);
                return object_of(id->name);   // registered class ctor: args lenient unless the host class is strict
            }
            if (eng_.has_function(name)) {
                note_global_use(id->name);
                note_host_nodiscard(name);
                check_constant_arguments(name, name, c);
                if (name == "sleep" && core_sleep_bound()) {
                    check_sleep_call(args, c->location);
                    return ctype::make(ctype::null_k);
                }
                check_host_call(name, args, c->location);
                if (name == "parallel_reduce") { check_reduce_callbacks(c); }
                if (name == "format") { check_format_text(c, args); }
                // a stdlib math alias yields the builtin's float wherever its name binds the builtin: a proof every consumer guards at run time
                if (args.size() == 1 && eng_.math_alias_fn(id->symbol_id) < engine_access::k_math_alias_rows) { return ctype::make(ctype::float_k); }
                return host_result_type(name);
            }
            if (eng_.has_variable(name)) { note_global_use(id->name); return {}; }
            if (engine_knows_name(name)) { return {}; }
            if (!id->global_qualified && report_cdtor_call(id->name, id->location)) { return {}; }
            if (program_or_derived_name(id->name)) { return {}; }
            if (stale_field_context()) { return {}; }
            undeclared(id->location, id->name);
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
                if (fn != math_fn::none && args.size() <= 8) { return math_intrinsic_result(fn, args); }
                if (ctype t; library_namespace_call(c, m, args, t)) { return t; }
                if (namespace_path_receiver(m)) {
                    check_namespace_member(m);
                    return check_namespace_entry(m, args, c->location);
                }
            }
            ctype recv = declared_container_receiver(m->object.get(), infer(m->object.get()));
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
            if (recv.kind == ctype::array_k && recv.ti && m->member == "push" && args.size() == 1) {
                const auto issue = container_element_issue(args[0], recv.ti->element_type().get());
                if (!issue.empty()) {
                    diag(diag_level::error, c->location, "cannot push into '" + ctype_name(recv) + "': " + issue);
                }
            }
            if (recv.kind == ctype::array_k && !m->is_static) { check_array_sizing_arguments(m->member, recv, args.data(), args.size(), c->location); }
            if (recv.const_key && string_mutator_name(m->member)) { map_key_mutation(m->location, m->object.get()); }
            if (class_dot_receiver(m, recv)) { return {}; }
            if (recv.kind == ctype::object_k || recv.kind == ctype::class_ref_k) { return class_member_call(c, m, recv, args, &ov); }
            if (recv.is_unknown() && !m->is_static) {
                if (const ctype held = referent_receiver(m->object.get()); held.kind == ctype::object_k) { class_member_call(c, m, held, args, nullptr); }
            }
            if (recv.kind == ctype::enum_ref_k) {
                return {};
            }
            uint8_t proven = 0;
            if (!recv.tentative || recv.element_proof) {
                proven = recv.kind == ctype::string_k ? 1 : recv.kind == ctype::array_k ? 2 : recv.kind == ctype::map_k ? 3 : 0;
            }
            if (proven == 0 && recv.is_unknown() && m->object->get_type() == node_type::identifier_expr) {
                if (const var_entry* v = find_local(static_cast<const identifier_expr*>(m->object.get())->name)) {
                    proven = v->builtin_alias == ctype::string_k ? 1 : v->builtin_alias == ctype::array_k ? 2 : v->builtin_alias == ctype::map_k ? 3 : 0;
                }
            }
            if (!recv.tentative && !m->is_static) {
                scalar_member_missing(recv, m, true);
                check_builtin_member(recv, m);
                checkBuiltinArity(recv, m, c);
                in_place_result_used(recv, m, c);
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
            return {};   // builtin container/string methods etc.
        }
        infer(callee);
        return {};
    }

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

    // recv.m(args) on a class or a class used as a value; ov (null for a reference's referent: diagnostics only) takes the overloads
    // a proof may rest on
    ctype class_member_call(call_expr* c, member_expr* m, const ctype& recv, const std::vector<ctype>& args,
                            const std::vector<const function_decl*>** ov) {
        bool want_static = (recv.kind == ctype::class_ref_k);
        member_hit hit = lookup_member(recv.cls, m->member, want_static);
        if (!hit.found && !want_static && chain_has_user_accessor(recv.cls, m->member)) { return {}; }   // calling what a user _get_ returns
        if (hit.found) {
            if (hit.methods) {
                if (hit.provable && ov) { *ov = hit.methods; ov_class_ = recv.cls; }
                ctype result = check_script_overloads(m->member, *hit.methods, args, c->location, hit.foreign).result;
                if (!nodiscard_hit_.name.empty()) { nodiscard_hit_.name = std::string(recv.cls) + "." + nodiscard_hit_.name; }
                if (!hit.provable) { result.tentative = true; }
                return result;
            }
            note_host_nodiscard(std::string(recv.cls) + "." + std::string(m->member));
            if (!want_static) { return check_strict_host_method(recv.cls, m->member, args, c->location); }
            check_host_static_arity(recv.cls, m->member, args.size(), c->location);
            return {};
        }
        if (!want_static) {
            if (ctype t; handle_builtin_result(m->member, t)) {
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
        if (hit.certain) {
            size_t rl = 0, rc = 0;
            if (auto it = classes_.find(recv.cls); it != classes_.end()) {
                rl = it->second.decl->location.line;
                rc = it->second.decl->location.column;
            }
            diag(diag_level::error, m->location,
                 std::string(want_static ? "no static member '" : "no member '") + std::string(m->member) +
                 "' in class '" + std::string(recv.cls) + "'", rl, rc);
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
        ctype rhs = infer(a->value.get());
        // a plain store of a string-valued right side: the compiler's text-builder store (`s = s + ...`) takes it, behind its run-time guards
        if (a->op.type == token_type::equal) {
            stamp(a->static_operand_class, rhs.kind == ctype::string_k ? operand_class_string_string : operand_class_unknown, operand_class_unknown);
        }
        struct incdec_scope { const char*& slot; const char* prev; ~incdec_scope() { slot = prev; } } incdec_guard{incdec_op_, incdec_op_};
        incdec_op_ = !a->incdec_desugar ? nullptr : a->op.type == token_type::plus_equal ? "++" : "--";
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
                if (rhs.const_key && local->is_handle && !compound) { map_key_alias(a->location, a->value.get()); return done(rhs); }
                if (local->dynamic) { return done(rhs); }
                check_store(rhs, compound, base_op, local->type, local->display,
                            "'" + std::string(id->name) + "'", a->location, local->line, local->column);
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
                                        hit.foreign ? 0 : hit.field->location.line, hit.foreign ? 0 : hit.field->location.column);
                        }
                        return done(ft);
                    }
                    return done({});
                }
                if (hit.unresolved) { return done(rhs); }
            }
            if (!id->global_qualified && !enclosing_namespace_member(id->name).empty()) { return done(rhs); }
            std::string name(id->name);
            if (extra_global(name)) {
                note_global_use(id->name);
                ctype gt = program_scope_store_type(name);
                if (gt.is_unknown()) { return done(rhs); }
                check_store(rhs, compound, base_op, gt, ctype_name(gt), "'" + name + "'", a->location, 0, 0);
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
                                "'" + std::string(id->name) + "'", a->location, 0, 0);
                }
                return done(gt);   // engine globals: never proven (S3)
            }
            if (eng_.has_function(name) || engine_knows_name(name) || program_or_derived_name(id->name) || stale_field_context()) {
                return done(rhs);
            }
            undeclared(id->location, id->name);
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
            std::string qualified;
            if (!typed_field && m->is_static) {
                const variable_decl* nv = namespace_variable(m, &qualified);
                if (nv && nv->type && nv->type->base_type == script_value_type::jai_shared_ptr_type) {
                    const ctype nt = program_global_type(nv);
                    check_store(rhs, compound, base_op, nt, declared_variable_type_name(nv), "'" + qualified + "'", a->location,
                                nv->location.line, nv->location.column);
                    if (!nt.is_unknown()) { return nt; }
                }
                if (const var_entry* x = qualified_extern(m); x && !x->dynamic) {
                    check_store(rhs, compound, base_op, x->type, x->display, "'" + qualified_spelling(m) + "'", a->location, x->line, x->column);
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
                                hit.foreign ? 0 : typed_field->location.line, hit.foreign ? 0 : typed_field->location.column);
                }
                return recv.diagnostic_only ? result_type : ft;
            }
            if (recv.kind == ctype::object_k && !m->is_static) { check_accessor_store(recv.cls, m->member, rhs, compound, base_op, a->location); }
            return result_type;
        }
        if (target->get_type() == node_type::binary_expr) {
            auto* subscript = static_cast<binary_expr*>(target);
            if (subscript->op.type == token_type::left_bracket) {
                const ctype recv = declared_container_receiver(subscript->left.get(), infer(subscript->left.get()));
                const ctype key = infer(subscript->right.get());
                if (recv.kind == ctype::map_k && !recv.tentative && key.kind == ctype::object_k && !key.tentative && !key.cls.empty()) { warn_identity_map_key(subscript, key.cls); }
                check_map_subscript_key(subscript, recv, key);
                stamp(subscript->static_operand_class, operand_class_unknown, operand_class_unknown);
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
                if (compound && check_compound_operands(element, base_op, rhs, a->location, 0, 0)) { return element; }
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
            check_store(rhs, compound, base_op, t, name, spelled_cast ? "a cast<" + name + "&> target" : std::string("the reference"), a->location, 0, 0);
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

    // A call whose callee resolves to the program's free functions or a class's constructors, none returning a reference
    bool call_returns_by_value(const call_expr* c) {
        if (!c->callee || c->callee->get_type() != node_type::identifier_expr) { return false; }
        auto* id = static_cast<const identifier_expr*>(c->callee.get());
        if (id->global_qualified || find_local(id->name) || class_ctx_) { return false; }
        if (auto it = functions_.find(id->name); it != functions_.end()) {
            if (it->second.empty()) { return false; }
            for (const auto* fn : it->second) {
                if (fn->return_type && fn->return_type->is_reference()) { return false; }
            }
            return true;
        }
        return find_sclass(id->name) != nullptr;
    }

    // Compound assign on a typed target behaves like C++ —
    // `x op= rhs` is `x = T(x op rhs)` — so it checks exactly like plain assignment
    // of the operation's result. var targets keep the dynamic behavior (unchecked).
    const char* incdec_op_ = nullptr;   // the ++/-- spelling while checking its += 1 / -= 1 desugar
    void check_store(const ctype& rhs, bool compound, token_type base_op, const ctype& target,
                     const std::string& target_display, const std::string& what,
                     const source_location& loc, size_t decl_line, size_t decl_col) {
        if (target.is_unknown()) { return; }
        if (compound && incdec_op_ && target.kind == ctype::string_k) {
            diag(diag_level::error, loc, std::string("cannot apply '") + incdec_op_ + "' to 'string'", decl_line, decl_col);
            return;
        }
        if (compound && base_op == token_type::plus && target.kind == ctype::string_k && !rhs.is_unknown() &&
            rhs.kind != ctype::string_k && rhs.kind != ctype::char_k && rhs.kind != ctype::utf8_k && !eng_.has_function("+")) {
            // a string target appends text only (a char is text; string += int raises, unlike binary +)
            diag(diag_level::error, loc, "cannot apply '+=' to 'string' and '" + ctype_name(rhs) + "'", decl_line, decl_col);
            return;
        }
        if (compound && !rhs.is_unknown() && target.kind != ctype::object_k) {   // a class target's operator decides
            const char* bitwise = base_op == token_type::ampersand ? "&=" : base_op == token_type::pipe ? "|=" : base_op == token_type::caret ? "^="
                                : base_op == token_type::left_shift ? "<<=" : base_op == token_type::right_shift ? ">>=" : nullptr;
            const auto integral = [](const ctype& t) { return t.kind == ctype::int_k || t.kind == ctype::char_k; };
            if (bitwise && (!integral(target) || !integral(rhs))) {
                diag(diag_level::error, loc, std::string("cannot apply '") + bitwise + "' to '" + ctype_name(target) + "' and '" + ctype_name(rhs) + "'",
                     decl_line, decl_col);
                return;
            }
        }
        if (compound && check_compound_operands(target, base_op, rhs, loc, decl_line, decl_col)) { return; }
        ctype stored = compound ? binary_result(target, base_op, rhs) : rhs;
        if (assignable(stored, target)) { return; }
        if (!compound && target.kind == ctype::object_k && assign_operator_accepts(target.cls, stored)) { return; }
        diag(diag_level::error, loc, "cannot assign '" + ctype_name(stored) + "' to " + what +
             " declared '" + target_display + "'" + assignment_detail(stored, target), decl_line, decl_col);
    }

    // `target = rhs` on a class-typed target runs a user operator=(rhs) ahead of the
    // conversion ladder (both backends); a registered ancestor's operator= stays lenient.
    bool assign_operator_accepts(std::string_view cls, const ctype& src, int depth = 0) {
        if (depth > 32) { return true; }
        const sclass* c = find_sclass(cls);
        if (!c) {
            auto def = engine_class(cls);
            return !def || !def->get_method(eng_.symbolize("="), false).is_invalid();
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

    // null against a declared slot that never holds it (a store of null raises or converts there); class values, handles and
    // functions hold null, and a call's container or class result is null when the body falls off its end
    void check_null_comparison(const binary_expr* b, const ctype& t, const expression* operand, const char* op) {
        switch (t.kind) {
            case ctype::int_k: case ctype::float_k: case ctype::char_k: case ctype::bool_k: case ctype::string_k: case ctype::utf8_k: break;
            case ctype::array_k: case ctype::map_k: case ctype::math_k:
                if (operand && operand->get_type() == node_type::call_expr) { return; }
                break;
            default: return;
        }
        const bool equality = b->op.type == token_type::equal_equal || b->op.type == token_type::bang_equal;
        const std::string name = ctype_name(t);
        diag(diag_level::error, b->location, "cannot compare '" + name + "' with null using '" + op + "': '" + name + "' is never null" +
             (equality ? std::string(" (it is always ") + (b->op.type == token_type::equal_equal ? "false" : "true") + ")"
                       : std::string(" (it raises 'Invalid operands for ") + op + " operator')") +
             "; compare with a value of its type, or declare the slot var to hold null");
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
            std::unordered_set<std::string_view> modeled;
            for (const auto& [name, c] : classes_) {
                modeled.insert(name);
                if (quiet && name != cls && derives(c.decl) && !lw_quiet_class(name)) { quiet = false; }
            }
            if (quiet && program_classes_) {
                for (const auto& [name, cd] : *program_classes_) {
                    if (!cd || modeled.count(cd->name)) { continue; }
                    modeled.insert(cd->name);
                    if (cd->name != cls && derives(cd.get()) && !lw_quiet_class(cd->name)) { quiet = false; break; }
                }
            }
            if (quiet) {
                if (auto def = engine_class(cls)) {
                    for (const auto& name : eng_.registered_class_names()) {
                        if (modeled.count(name)) { continue; }
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
                if (!c->callee || c->callee->get_type() != node_type::member_expr) { return true; }
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

    JAI_NOINLINE void walk_stmt(statement* s) {
        if (!s) { return; }
        stack_.guard(s->location);
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
                scopes_.emplace_back();
                for (auto& d : b->declarations) { walk_decl(d.get()); }
                scopes_.pop_back();
                break;
            }
            case node_type::if_stmt: {
                auto* i = static_cast<if_stmt*>(s);
                infer(i->condition.get());
                walk_stmt(i->then_statement.get());
                walk_stmt(i->else_statement.get());
                break;
            }
            case node_type::while_stmt: {
                auto* w = static_cast<while_stmt*>(s);
                infer(w->condition.get());
                walk_stmt(w->body.get());
                break;
            }
            case node_type::for_stmt:
                walk_for(static_cast<for_stmt*>(s));
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

    JAI_NOINLINE void walk_for(for_stmt* f) {
        scopes_.emplace_back();
        if (lw_on()) { lw_modes_[f] = region_mode(); }
        if (f->initializer) { walk_decl(f->initializer.get()); }
        if (f->condition) { infer(f->condition.get()); }
        if (f->update) { infer(f->update.get()); }
        walk_stmt(f->body.get());
        scopes_.pop_back();
    }

    JAI_NOINLINE void walk_range_for(range_for_stmt* f) {
        const ctype container = enum_table(infer(f->container.get()));
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
        }
        if (!f->element_type) { f->auto_spelled = true; }
        check_spelled_type(f->element_type.get(), f->location);
        if (f->auto_spelled && !f->auto_typed && semantic_walk() && container.kind != ctype::map_k) {
            f->auto_typed = true;   // an auto over elements without a static type is a var, decided once
            if (elem.is_unknown()) { f->element_type = eng_.get_type_info_any(); }
        }
        const bool declared = f->element_type && f->element_type->base_type != script_value_type::jai_any_type && !f->auto_spelled;
        stamp(f->element_conversion_free, !declared || (!elem.is_unknown() && same_type(ctype_from_type_info(f->element_type.get()), elem)), false);
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
            const ctype judged = var_spelled ? ctype{} : spelled && entry.type.kind != ctype::object_k ? entry.type : elem;
            entry.lw_handle = judged.kind == ctype::object_k && elem.kind != ctype::object_k;
            lw_register(entry, judged, f, f->variable_name, lw_origin::range_for, spelled ? f->element_type->canonical_name() : "auto",
                        !loop_const && !entry.lw_handle);
        }
        warn_shadow("range-for variable", f->variable_name, f->location);
        scopes_.back().emplace(f->variable_name, std::move(entry));
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
        walk_stmt(f->body.get());
        --parallel_body_depth_;
        scopes_.pop_back();
    }

    JAI_NOINLINE void walk_try(try_stmt* t) {
        walk_stmt(t->try_block.get());
        scopes_.emplace_back();
        if (!t->catch_var.empty()) {
            var_entry entry;
            entry.dynamic = true;
            warn_shadow("catch variable", t->catch_var, t->location);
            scopes_.back().emplace(std::string_view(t->catch_var), std::move(entry));
        }
        walk_stmt(t->catch_block.get());
        scopes_.pop_back();
    }

    JAI_NOINLINE void walk_switch(switch_stmt* sw) {
        const ctype condition = infer(sw->condition.get());
        for (auto& cs : sw->cases) {
            const ctype value = infer(cs->value.get());
            stamp(cs->static_operand_class, operand_class_of(condition, value), operand_class_unknown);
            scopes_.emplace_back();
            for (auto& st : cs->body) { walk_stmt(st.get()); }
            scopes_.pop_back();
        }
        if (sw->default_case) {
            scopes_.emplace_back();
            for (auto& st : sw->default_case->body) { walk_stmt(st.get()); }
            scopes_.pop_back();
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
        const ctype value = r->value ? infer(r->value.get()) : ctype::make(ctype::null_k);
        check_return_value(r, value, proof);
    }

    JAI_NOINLINE void check_return_value(return_stmt* r, ctype value, uint8_t& proof) {
        if (r->binds_reference) { report_mixed_reference_conditional(r->value.get()); }
        if (fn_stack_.empty()) { return; }
        const fn_ctx& fn = fn_stack_.back();
        if (r->value && value.const_key && (r->binds_reference || handle_type(fn.ret))) {
            map_key_alias(r->location, r->value.get());
        }
        if (!fn.is_coroutine && !fn.ret) {
            fn_ctx& open = fn_stack_.back();
            const uint8_t k = !r->value || r->binds_reference || value.tentative ? 0
                            : value.kind == ctype::int_k ? 1 : value.kind == ctype::float_k ? 2 : value.kind == ctype::bool_k ? 3 : 0;
            if (k == 0 || (open.inferred_return != 0 && open.inferred_return != k)) { open.inferred_mixed = true; }
            else { open.inferred_return = k; }
        }
        if (fn.is_coroutine || !fn.ret) { return; }   // coroutine returns/yields + auto: lenient
        if (!r->value && requires_return_value(fn.ret)) {
            diag(diag_level::error, r->location, "'" + std::string(fn.name) + "' declared to return '" +
                 fn.ret->canonical_name() + "' returns no value here", fn.line, fn.column);
            return;
        }
        if (fn.ret->base_type == script_value_type::jai_reference_type && r->value) {
            const type_info* referent = fn.ret->element_type().get();
            if (referent && !reference_type_compatible(value, ctype_from_type_info(referent))) {
                diag(diag_level::error, r->location, "cannot return reference to '" + ctype_name(value) +
                    "' from '" + std::string(fn.name) + "' declared to return '" + fn.ret->canonical_name() + "'");
            }
            return;
        }
        ctype ret = ctype_from_type_info(fn.ret);
        if (ret.is_unknown()) { return; }
        if (r->value && !r->binds_reference && ret.kind != ctype::null_k && same_type(ret, value)) { proof = proof_same_type; }
        if (ret.kind == ctype::null_k) {
            if (r->value && value.kind != ctype::null_k && !value.is_unknown()) {
                diag(diag_level::error, r->location, "cannot return a value from '" + std::string(fn.name) +
                     "' declared 'void'", fn.line, fn.column);
            }
            return;
        }
        if (!r->value) {
            // a nullable result (a class, a handle, a container) returns null here: warning severity, like falling off the end
            diag(diag_level::warning, r->location, "'" + std::string(fn.name) + "' declared to return '" +
                 fn.ret->canonical_name() + "' returns no value here (returns null)", fn.line, fn.column);
            return;
        }
        if (!assignable(value, ret)) {
            diag(diag_level::error, r->location, "cannot return '" + ctype_name(value) + "' from '" +
                 std::string(fn.name) + "' declared to return '" + fn.ret->canonical_name() + "'" + assignment_detail(value, ret),
                 fn.line, fn.column);
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
            scopes_.back().emplace(fn->name, std::move(entry));
        }
        walk_function(fn);
    }

    JAI_NOINLINE void walk_destructuring(destructuring_decl* dd) {
        infer(dd->initializer.get());
        for (const auto& [name, _] : dd->names) {
            var_entry entry;
            entry.dynamic = true;
            entry.is_const = dd->is_const;
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
            entry.declared = x->type.get();
            entry.display = x->type->canonical_name();
            entry.type = ctype_from_type_info(x->type.get());
            entry.dynamic = entry.type.is_unknown();
        } else {
            entry.dynamic = true;
        }
        if (x->qualified()) { qualified_externs_[std::string(x->name)] = entry; }
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
            case node_type::function_decl:
                walk_function_declaration(static_cast<function_decl*>(d));
                break;
            case node_type::class_decl: {
                auto* cd = static_cast<class_decl*>(d);
                if (!classes_.count(cd->name)) { collect_class(cd); }
                walk_class(cd);
                break;
            }
            case node_type::namespace_decl: {
                auto* n = static_cast<namespace_decl*>(d);
                scopes_.emplace_back();
                namespace_scopes_.push_back(scopes_.size() - 1);
                namespace_walk_names_.push_back(n->name);
                for (auto& inner : n->declarations) { walk_decl(inner.get()); }
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
        if (init.is_unknown() || init.kind == ctype::coroutine_k) {
            decl->type = eng_.get_type_info_any();
            decl->decl_fast_flags = variable_decl::classify_decl_fast(decl->type);
        } else if (init.kind == ctype::float_k && decl->initializer && decl->initializer->get_type() == node_type::ternary_expr &&
                   last_mixed_ternary_.node == decl->initializer.get()) {
            decl->type = eng_.get_type_info_float();   // an int/float ternary is float (C++ common type): the chosen arm converts
            decl->decl_fast_flags = variable_decl::classify_decl_fast(decl->type);
        }
    }

    // An auto initializer's static type: a ternary whose arms are int and float has C++'s common type, float;
    // every other mixed ternary has none
    ctype numeric_ternary_type(const expression* init_expr, const ctype& init) {
        if (!init.is_unknown() || last_mixed_ternary_.node != init_expr) { return init; }
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
        const ctype init = decl->initializer ? infer(decl->initializer.get()) : ctype{};
        declare_variable(decl, init);
    }

    JAI_NOINLINE void declare_variable(variable_decl* decl, ctype init) {
        bool has_init = decl->initializer != nullptr;
        bool is_reference_decl = decl->type && decl->type->base_type == script_value_type::jai_reference_type;
        bool is_handle_decl = handle_type(decl->type.get());
        if (has_init) {
            if (is_reference_decl && !decl->is_const) { report_mixed_reference_conditional(decl->initializer.get()); }
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
                entry.dynamic = init.is_unknown();
                entry.is_handle = true;
                entry.display = "shared_ptr<" + ctype_name(init) + ">";
            }
            check_const_bind(decl, entry);
            warn_shadow("local", decl->name, decl->location);
            if (lw_on()) { lw_opaque_locals_.insert(decl); }
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
            const bool reference_ok = is_reference_decl && reference_type_compatible(binding_source, entry.type);
            // a const T& binds a temporary T made from what a T declaration takes (C++ lifetime extension)
            const bool init_ok = is_reference_decl ? reference_ok || (decl->is_const && assignable(init, entry.type)) : assignable(init, entry.type);
            stamp(decl->reference_admission_proven, is_reference_decl && has_init && reference_ok && !binding_source.is_unknown() &&
                  !binding_source.tentative && binding_source.kind != ctype::null_k &&
                  binding_source.kind != ctype::object_k, false);   // a class binding checks at run time (a reload may change the chain)
            if (has_init && (!entry.dynamic || is_reference_decl) && !init_ok) {
                diag(diag_level::error, decl->location, "cannot initialize '" + std::string(decl->name) +
                     "' declared '" + entry.display + "' with '" + ctype_name(binding_source) + "'" + assignment_detail(binding_source, entry.type));
                // Recovery keeps the declared type; reference use remains conservative.
            }
        } else if (has_init) {
            // auto with initializer: the initializer's static type, then enforced as that type; a var or unknown
            // static type makes the slot a var. null carries no type: the runtime locks at the first non-null
            // store, so a null init stays dynamic.
            init = numeric_ternary_type(decl->initializer.get(), init);
            entry.type = init;
            entry.dynamic = init.is_unknown() || init.kind == ctype::null_k;
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
        check_const_bind(decl, entry);
        warn_shadow("local", decl->name, decl->location);
        if (!is_reference_decl && !decl->is_static && lw_on()) {
            const bool spelled = decl->type && !decl->auto_spelled;
            entry.lw_handle = !spelled && entry.type.kind == ctype::object_k && !lw_value_initializer(decl->initializer.get());
            const ctype judged = entry.dynamic ? ctype{} : !spelled && has_init && entry.type.tentative ? lw_declared_copy_type(decl->initializer.get(), entry.type) : entry.type;
            lw_register(entry, judged, decl, decl->name, lw_origin::local, spelled ? entry.display : "auto",
                        !decl->is_const && !decl->function_spelling && !entry.lw_handle);
        }
        declare_local(decl->name, std::move(entry), decl->location, !decl->is_static);
    }

    JAI_NOINLINE void walk_function(const function_decl* fn) {
        // Named functions capture nothing: lambda capture contexts must not leak in.
        std::vector<capture_ctx> saved_captures;
        saved_captures.swap(capture_stack_);
        scopes_.emplace_back();
        fn_ctx ctx{fn->return_type.get(), fn->is_coroutine, fn->name,
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
        fn_stack_.push_back(std::move(ctx));
        mode_guard enclosing(mode_stack_, scoped_region_mode(), /*scoped=*/true, /*always=*/true);
        check_spelled_type(fn->return_type.get(), fn->location);
        lw_open_region();
        define_params(fn->parameters, fn->location, /*warn_field_shadows=*/!is_ctor);
        fn_stack_.back().in_ctor_initializers = true;
        for (const auto& init : fn->initializers) {
            for (const auto& arg : init.arguments) { infer(arg.get()); }
            if (is_ctor && init.target == "super" && init.arguments.empty()) { report_base_without_default(class_ctx_name_, fn->location); }
        }
        if (is_ctor) { check_base_initializers(fn); }
        fn_stack_.back().in_ctor_initializers = false;
        if (fn->body) {
            mode_guard body(mode_stack_, leading_mode(fn->body->declarations), /*scoped=*/true);
            for (auto& d : fn->body->declarations) { walk_decl(d.get()); }
        }
        check_falls_off_end(fn);
        lw_close_region(&fn->parameters, nullptr, fn->body.get());
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

    void define_params(const std::vector<parameter>& params, const source_location& fn_loc, bool warn_field_shadows = true) {
        for (const auto& p : params) {
            check_spelled_type(p.type.get(), fn_loc);
            ctype pt = ctype_from_type_info(p.type.get());
            if (p.default_value) {
                ctype dv = infer(p.default_value.get());
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
            if (p.is_reference) { entry.referent = referent_type(pt); }
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

    // A base class no unit, include, extern or host can bring is the definition's error, reported at load in the run time's
    // words so a module that would stop half-defined applies nothing
    void check_class_bases(const class_decl* cd) {
        if (!namespace_scopes_.empty() || has_includes_) { return; }
        for (std::string_view base : cd->base_classes) {
            if (find_sclass(base) || engine_class(base) || base.find("::") != std::string_view::npos || global_var_names_.count(base) ||
                engine_knows_name(std::string(base))) { continue; }
            diag(diag_level::error, cd->location, "Base class '" + std::string(base) + "' not found");
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
             (read == initializing ? std::string() : "; declare '" + read + "' above '" + initializing + "'"), field->location.line, field->location.column);
    }

    void walk_class(const class_decl* cd) {
        check_class_bases(cd);
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
                if (f->initializer) {
                    const cdtor_ctx enclosing_cdtor = std::exchange(cdtor_ctx_, f->is_static ? cdtor_ctx::none : cdtor_ctx::construction);
                    initializing_field initializing{f->name, {}};
                    if (!f->is_static) { initializing.unready = fields_from(cd, f); }
                    initializing_field_ = f->is_static ? nullptr : &initializing;
                    ctype init = infer(f->initializer.get());
                    initializing_field_ = nullptr;
                    cdtor_ctx_ = enclosing_cdtor;
                    if (f->auto_spelled) { type_auto_slot(f, numeric_ternary_type(f->initializer.get(), init)); }   // an auto field: its initializer's static type
                    if ((f->type && !f->auto_spelled) || f->function_spelling) {
                        ctype ft = declared_variable_type(f);
                        if (!ft.is_unknown() && !assignable(init, ft)) {
                            diag(diag_level::error, f->location, "cannot initialize field '" +
                                 std::string(f->name) + "' declared '" + declared_variable_type_name(f) +
                                 "' with '" + ctype_name(init) + "'" + assignment_detail(init, ft));
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
        capture_stack_.push_back(capture_ctx{scopes_.size(), l});
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
        lw_close_region(&l->parameters, nullptr, l->body.get());
        close_lambda_scope(l);
    }

    JAI_NOINLINE void check_lambda_captures(lambda_expr* l) {
        for (const auto& cap : l->captures) {
            if (cap.initializer) { infer(cap.initializer.get()); continue; }   // an init-capture: its expression, in the enclosing scope
            if (cap.name == "this") { continue; }   // capture-list entries are not identifier uses
            if (find_local(cap.name)) { continue; }
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
                if (var_entry* original = find_local(cap.name)) { scopes_.back().emplace(cap.name, *original); }
                continue;
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
            note_program_decl(f->name, f->location);
            nested_scope body{collect_depth_};
            collect_names(f->body.get());
            break;
        }
        case node_type::class_decl: {
            auto* c = static_cast<const class_decl*>(node);
            collect_program_name(c->name);
            note_program_decl(c->name, c->location);
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
            for (const auto& arg : c->arguments) {
                collect_names_expr(arg.get());
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
                std::vector<declaration_ptr> qualified_members;   // its classes and nested namespaces carry qualified names
                for (const auto& member : n->declarations) {
                    if (member && (member->get_type() == node_type::class_decl || member->get_type() == node_type::namespace_decl)) { qualified_members.push_back(member); }
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

void collect_unit_functions(const std::vector<declaration_ptr>& decls, const std::string& unit_path, program_function_map& out) {
    for (const auto& d : decls) {   // the unit's earlier entries go first: a re-gather replaces, never accumulates
        if (d && d->get_type() == node_type::function_decl) { out[std::string(static_cast<const function_decl*>(d.get())->name)][unit_path].clear(); }
    }
    for (const auto& d : decls) {
        if (d && d->get_type() == node_type::function_decl) {
            auto fd = std::static_pointer_cast<const function_decl>(d);
            out[std::string(fd->name)][unit_path].push_back(std::move(fd));
        }
    }
}

void collect_unit_globals(const std::vector<declaration_ptr>& decls, const std::string& unit_path, program_global_map& out) {
    for (const auto& d : decls) {   // a re-gather replaces the unit's own entries, never accumulates
        if (d && d->get_type() == node_type::variable_decl) { out[std::string(static_cast<const variable_decl*>(d.get())->name)][unit_path].clear(); }
    }
    for (const auto& d : decls) {   // every spelling counts toward "declared once": the lookup asks the one declaration for its type
        if (d && d->get_type() == node_type::variable_decl) {
            auto vd = std::static_pointer_cast<const variable_decl>(d);
            out[std::string(vd->name)][unit_path].push_back(std::move(vd));
        }
    }
}

void collect_unit_enums(const std::vector<declaration_ptr>& decls, const std::string& unit_path, program_enum_map& out) {
    for (auto& [name, units] : out) { units.erase(unit_path); }   // a re-gather replaces the unit's own entries
    for (const auto& d : decls) {
        if (d && d->get_type() == node_type::enum_decl) {
            auto ed = std::static_pointer_cast<const enum_decl>(d);
            out[std::string(ed->name)][unit_path] = std::move(ed);
        }
    }
}

// a namespace's class is named by its qualified spelling (ns::Name)
bool collect_unit_classes(const std::vector<declaration_ptr>& decls, program_class_map& out) {
    bool result = false;
    for (const auto& d : decls) {
        if (!d) { continue; }
        if (d->get_type() == node_type::class_decl) {
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
