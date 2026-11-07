// Part of the class runtime's closure, which includes it in its order: a translation unit that names it first is sent there
#if !defined(__JAISCRIPT_DETAIL_CLASS_RUNTIME_HPP__)
#include <jaiscript/detail/class_runtime.hpp>
#elif !defined(__JAISCRIPT_CORE_OVERLOAD_RESOLUTION_HPP__)
#define __JAISCRIPT_CORE_OVERLOAD_RESOLUTION_HPP__

#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/ast.hpp>
#include <climits>
#include <memory>
#include <optional>
#include <vector>

namespace jai {

// Trailing-default arity window (the parser enforces defaults are trailing): a callable
// with N params of which K have defaults accepts N-K..N arguments. The binding layer on
// BOTH backends already evaluates missing defaults; these gates decide VIABILITY only -
// used uniformly by method/ctor/delegation/namespace resolution so every callable kind
// honors defaults the way free functions always have.
inline size_t required_parameter_count(const std::vector<parameter>& params) {
    size_t required = 0;
    for (const auto& p : params) {
        if (!p.default_value) { ++required; } else { break; }
    }
    return required;
}

inline bool arity_accepts(const std::vector<parameter>& params, size_t argc) {
    return argc >= required_parameter_count(params) && argc <= params.size();
}

// A unary operator the class chain declares: a script overload of the name that takes no argument
// (the class or a base), or a host class's method of that name (its binding decides the arity)
inline bool declares_unary_operator(const class_definition* def, uint64_t name_id) {
    bool script_declared = false;
    for (const class_definition* c = def; c; c = c->get_parent().get()) {
        const auto& overloads = c->script_method_overloads();
        if (auto it = overloads.find(name_id); it != overloads.end()) {
            script_declared = true;
            for (const auto& d : it->second) { if (arity_accepts(d->parameters, 0)) { return true; } }
        }
    }
    const script_value* bound = !script_declared && def ? def->method_ptr(name_id) : nullptr;
    return bound && bound->is_function();
}

// A typed parameter takes the argument with no mismatch (the scorer's rows: the class or a subclass, the same kind, int<->float)
inline bool parameter_admits(const parameter& p, const script_value& raw) {
    if (!p.type || p.type->type_name.empty() || p.type->type_name == "any" || p.type->base_type == script_value_type::jai_any_type) { return true; }
    const script_value& arg = raw.deref();
    const auto kind = arg.type();
    if (kind == script_value_type::jai_object_type || kind == script_value_type::jai_shared_ptr_type) {
        auto instance = const_cast<script_value&>(arg).get_class_instance();
        auto def = instance ? instance->get_class_definition() : nullptr;
        return def && (def->get_name() == p.type->type_name || def->is_subtype_of(p.type->type_name));
    }
    return kind == p.type->base_type ||
           (kind == script_value_type::jai_int_type && p.type->base_type == script_value_type::jai_float_type) ||
           (kind == script_value_type::jai_float_type && p.type->base_type == script_value_type::jai_int_type);
}

// A host operator: a method the binder registered with its C++ signature (its dispatch scores the arguments)
inline const class_definition::cpp_method_dispatch* host_operator_dispatch(const script_value& method) {
    return method.is_function() ? method.as_function().target<class_definition::cpp_method_dispatch>() : nullptr;
}

// A host operator method takes arg when one of its one-argument overloads scores it as the call's dispatch would; an
// untyped binding takes anything
inline bool host_operator_accepts(const script_value& method, const script_value& arg) {
    if (!method.is_function()) { return false; }
    const auto* dispatch = host_operator_dispatch(method);
    if (!dispatch) { return true; }
    const auto unary = dispatch->overloads->find(1);
    if (unary == dispatch->overloads->end() || unary->second.empty()) { return false; }
    engine* eng = dispatch->cls->get_engine();
    for (const auto& entry : unary->second) {
        if (!eng || entry.param_types.empty() || detail::engine_access::cpp_signature_cost(*eng, &arg, entry.param_types) >= 0) { return true; }
    }
    return false;
}

// The chain's binary operator `name_id` is a candidate for `arg`: the nearest script declaration has a one-argument overload
// that takes it (C++: a candidate the argument cannot reach is not viable), or a host class's overload of that name takes it;
// typed_only counts a script overload only when its parameter names a type (an untyped one takes any argument)
inline bool binary_operator_accepts(const class_definition* def, uint64_t name_id, const script_value& arg, bool typed_only = false) {
    for (const class_definition* c = def; c; c = c->get_parent().get()) {
        const auto& overloads = c->script_method_overloads();
        if (auto it = overloads.find(name_id); it != overloads.end()) {
            for (const auto& d : it->second) {
                if (!arity_accepts(d->parameters, 1) || d->parameters.empty()) { continue; }
                const parameter& p = d->parameters[0];
                if (typed_only && (!p.type || p.type->type_name.empty() || p.type->type_name == "any" || p.type->base_type == script_value_type::jai_any_type)) { continue; }
                if (parameter_admits(p, arg)) { return true; }
            }
            return false;
        }
    }
    const script_value* bound = def ? def->method_ptr(name_id) : nullptr;
    return bound && host_operator_accepts(*bound, arg);
}

// other is a value of def's class or a subclass
inline bool value_of_class(const class_definition* def, const script_value& other) {
    const script_value& d = other.deref();
    if (!d.is_object()) { return false; }
    auto holder = const_cast<script_value&>(d).get_object_holder();
    if (!holder) { return false; }
    const class_definition* other_def = nullptr;
    if (holder->is_instance()) {
        other_def = holder->holds_object() ? holder->unchecked_instance()->get_class_definition() : nullptr;
    } else if (holder->type_name() == def->get_name()) {
        return true;
    } else if (engine* eng = def->get_engine(); eng && !holder->type_name().empty()) {
        other_def = detail::engine_access::get_class_definition(*eng, std::string(holder->type_name())).get();
    }
    return other_def && (other_def == def || other_def->is_subtype_of(def->get_name()));
}

// The chain's == (or !=) is a candidate for other: with <=> in the chain an overload must take it; without, other is a value
// of the class or an overload whose parameter names a type takes it
inline bool equality_operator_candidate(const class_definition* def, uint64_t name_id, uint64_t spaceship_id, const script_value& other) {
    if (!def) { return true; }
    if (const script_value* spaceship = def->method_ptr(spaceship_id); spaceship && spaceship->is_function()) { return binary_operator_accepts(def, name_id, other); }
    return value_of_class(def, other) || binary_operator_accepts(def, name_id, other, true);
}

// A reversed comparison: a < b answered by b's <=> is (b <=> a) > 0, and so on
inline token_type reversed_comparison(token_type op) {
    switch (op) {
        case token_type::less: return token_type::greater;
        case token_type::less_equal: return token_type::greater_equal;
        case token_type::greater: return token_type::less;
        default: return token_type::less_equal;
    }
}

// 0 <=> r for a reversed a <=> b answered by b's <=>, in the builtin <=> order (signed zeros equivalent, NaN last); nullopt
// for a result that is no number
inline std::optional<script_int> reversed_spaceship_sign(const script_value& result) {
    const script_value& r = result.deref();
    if (r.is_int()) { const script_int v = r.as_int(); return v < 0 ? 1 : v > 0 ? -1 : 0; }
    if (r.is_float()) { const double v = r.as_float(); return v != v ? -1 : v < 0 ? 1 : v > 0 ? -1 : 0; }
    return std::nullopt;
}

// C++20 comparison rewriting: with no operator< in the chain, a < b is (a <=> b) < 0, and so on for
// <= > >=; the left operand's operator<=> result compares with zero (a NaN compares false), anything but a number is no answer
inline std::optional<bool> spaceship_rewritten(token_type op, const script_value& result) {
    const script_value& r = result.deref();
    double cmp = 0.0;
    if (r.is_int()) { cmp = static_cast<double>(r.as_int()); }
    else if (r.is_float()) { cmp = r.as_float(); }
    else { return std::nullopt; }
    switch (op) {
        case token_type::less: return cmp < 0;
        case token_type::less_equal: return cmp <= 0;
        case token_type::greater: return cmp > 0;
        case token_type::greater_equal: return cmp >= 0;
        default: return std::nullopt;
    }
}

inline bool object_declares_method(const script_value& arg, std::string_view name) {
    auto instance = const_cast<script_value&>(arg).get_class_instance();
    auto def = instance ? instance->get_class_definition() : nullptr;
    engine* eng = def ? def->get_engine() : nullptr;
    if (!eng) { return false; }
    const auto id = detail::engine_access::get_symbolizer(*eng)->find(name);
    return id && def->defines_method(*id);
}

// A one-parameter script constructor of the parameter's class takes the argument (the table's converting row, no chaining;
// defined beside script_class_definition)
inline bool converting_constructor_takes(const script_value& arg, const type_info& target, engine* eng);

// The assignment table's rows past the exact type that a by-value parameter takes (docs/site/embedding/06-static-checking.html "One conversion table"), as overload costs:
// int<->float, bool to a number and a char to text 1 (a conversion, ahead of a var parameter); a to_int/to_float/to_char/to_string
// method, the other text kind, a one-character string for a char and the bool row 2 and a converting constructor 3 (k_loose_conversion and up: after a var parameter, which takes the value as it
// is); 0 = not viable. Among several overloads a string never makes a bool parameter viable.
inline constexpr size_t k_loose_conversion = 2;
inline size_t table_conversion_cost(const script_value& arg, const parameter& param, bool in_set, engine* eng) {
    using t = script_value_type;
    const t to = param.type->base_type;
    const t from = arg.type();
    const bool number_to = to == t::jai_int_type || to == t::jai_float_type;
    if (number_to && (from == t::jai_int_type || from == t::jai_float_type)) { return 1; }
    if (param.is_reference && !param.is_const) { return 0; }   // a const T& binds a T made from the argument
    switch (to) {
        case t::jai_int_type:
        case t::jai_float_type:
            if (from == t::jai_bool_type) { return 1; }
            return from == t::jai_object_type && object_declares_method(arg, to == t::jai_int_type ? "to_int" : "to_float") ? 2 : 0;
        case t::jai_char_type:
            if (from == t::jai_string_type) { return 2; }
            return from == t::jai_object_type && object_declares_method(arg, "to_char") ? 2 : 0;
        case t::jai_bool_type:
            return in_set && (from == t::jai_string_type || from == t::jai_utf8_string_type) ? 0 : 2;
        case t::jai_string_type:
        case t::jai_utf8_string_type:
            if (from == t::jai_char_type) { return 1; }
            if (from == t::jai_string_type || from == t::jai_utf8_string_type) { return 2; }
            return from == t::jai_object_type && object_declares_method(arg, "to_string") ? 2 : 0;
        case t::jai_object_type:
            return converting_constructor_takes(arg, *param.type, eng) ? 3 : 0;
        default:
            return 0;
    }
}

// Steps from a class up its chain to the base named `name` (0 = the class itself); SIZE_MAX when it is no base
inline size_t base_distance(const class_definition* def, std::string_view name) {
    size_t steps = 0;
    for (const class_definition* c = def; c; c = c->get_parent().get(), ++steps) {
        if (c->get_name() == name) { return steps; }
    }
    return SIZE_MAX;
}

// The scorer's verdict over a set of candidates: the best one, and a rival that ties it on every key (an ambiguous call)
template <typename Candidate>
struct overload_verdict {
    const Candidate* best = nullptr;
    const Candidate* rival = nullptr;
};

// Shared script-overload scorer used by free-function, namespace, method, static-method and constructor dispatch.
// Priority: 1) all typed + exact match, 2) all typed + conversions (fewest first)
//           3) var/any parameters (fallback; a typed parameter the argument does not
//           match ranks the candidate below every one it matches, then fewer var
//           parameters win: typed exact > conversion > var)
// Same-score tie: the candidate using FEWER defaults wins (an exact-arity overload
// beats one that must fill trailing defaults), then fewest conversions, then the nearest base a class argument converts to.
// Candidates equal on every key are an ambiguous call: the verdict names the rival.
// Returns a null best when no overload is viable for these arguments.
// Span core (call-arg windows: the vm resolves against the caller's stack slice without collecting a vector).
template <typename Range, typename Params>
inline auto score_overloads(const Range& overloads, Params&& params_of, const script_value* args, size_t argc, engine* eng) {
    using candidate = std::remove_cv_t<std::remove_reference_t<decltype(*std::begin(overloads))>>;
    overload_verdict<candidate> verdict;
    int best_score = INT_MAX;  // Lower is better: 0=exact, 1-N=conversions, 1000=has_var
    size_t best_mismatches = SIZE_MAX;
    size_t best_var_count = SIZE_MAX;
    size_t best_defaults_used = SIZE_MAX;
    size_t best_conversion_count = SIZE_MAX;
    size_t best_distance = SIZE_MAX;

    for (const auto& overload : overloads) {
        const std::vector<parameter>& parameters = params_of(overload);
        if (!arity_accepts(parameters, argc)) {
            continue;
        }
        const size_t defaults_used = parameters.size() - argc;

        bool all_typed = true;       // All parameters have explicit types
        bool all_exact = true;       // All typed parameters match exactly
        bool all_convertible = true; // All typed parameters can be converted
        size_t conversion_count = 0; // Number of conversions needed
        bool has_var = false;        // Has any var/any parameters
        size_t var_count = 0;        // var/any parameters
        size_t mismatches = 0;       // typed parameters the argument matches neither exactly nor by conversion
        size_t distance = 0;         // steps from the class arguments up to the bases their parameters name
        bool loose = false;          // a conversion that ranks after a var parameter

        for (size_t i = 0; i < argc; ++i) {
            const auto& param = parameters[i];
            // Lvalue call args (arr[i], obj.field) arrive as references: match their targets
            const script_value& arg = args[i].deref();
            // var keyword creates type_info with type_name="any" and base_type=jai_any_type
            bool has_explicit_type = param.type &&
                                    !param.type->type_name.empty() &&
                                    param.type->type_name != "any" &&
                                    param.type->base_type != script_value_type::jai_any_type;
            if (has_explicit_type) {
                auto arg_type = arg.type();
                auto convert_or_mismatch = [&]() {
                    all_exact = false;
                    if (const size_t cost = table_conversion_cost(arg, param, overloads.size() > 1, eng)) {
                        conversion_count += cost;
                        loose = loose || cost >= k_loose_conversion;
                    } else {
                        all_convertible = false;
                        ++mismatches;
                    }
                };

                if (arg_type == script_value_type::jai_object_type ||
                    arg_type == script_value_type::jai_shared_ptr_type) {
                    // For objects and shared_ptr, check class name with inheritance support
                    auto instance = const_cast<script_value&>(arg).get_class_instance();
                    if (instance) {
                        auto arg_class_def = instance->get_class_definition();
                        if (arg_class_def) {
                            if (arg_class_def->get_name() == param.type->type_name) {
                                if (arg_type == script_value_type::jai_shared_ptr_type &&
                                    param.type->base_type == script_value_type::jai_object_type) {
                                    // shared_ptr<T> -> T requires unwrapping, count as conversion
                                    all_exact = false;
                                    conversion_count++;
                                }
                            } else if (const size_t steps = base_distance(arg_class_def, param.type->type_name); steps != SIZE_MAX) {
                                // Derived type - allow but rank as an implicit conversion, nearer bases first
                                all_exact = false;
                                conversion_count++;
                                distance += steps;
                            } else {
                                convert_or_mismatch();
                            }
                        } else {
                            // Can't get class def, fall back to name comparison
                            if (instance->get_class_name() != param.type->type_name) {
                                convert_or_mismatch();
                            }
                        }
                    } else {
                        convert_or_mismatch();
                    }
                } else if (arg_type != param.type->base_type) {
                    convert_or_mismatch();
                }
            } else {
                // Parameter has no type (var/auto) - accepts anything
                all_typed = false;
                has_var = true;
                ++var_count;
            }
        }

        int score;
        if (!all_convertible && !has_var) {
            continue;
        } else if (all_typed && all_exact) {
            score = 0;
        } else if (loose) {
            score = static_cast<int>(2000 + conversion_count);
        } else if (all_typed && all_convertible) {
            score = static_cast<int>(1 + conversion_count);
        } else if (has_var) {
            score = 1000;
        } else {
            continue;
        }

        int order;   // < 0: better than the best so far, 0: tied on every key, > 0: worse
        if (mismatches != best_mismatches) {
            order = mismatches < best_mismatches ? -1 : 1;
        } else if (score != best_score) {
            order = score < best_score ? -1 : 1;
        } else if (var_count != best_var_count) {
            order = var_count < best_var_count ? -1 : 1;
        } else if (defaults_used != best_defaults_used) {
            order = defaults_used < best_defaults_used ? -1 : 1;
        } else if (conversion_count != best_conversion_count) {
            order = conversion_count < best_conversion_count ? -1 : 1;
        } else if (distance != best_distance) {
            order = distance < best_distance ? -1 : 1;
        } else {
            order = 0;
        }
        if (order < 0) {
            verdict.best = &overload;
            verdict.rival = nullptr;
            best_score = score;
            best_mismatches = mismatches;
            best_var_count = var_count;
            best_defaults_used = defaults_used;
            best_conversion_count = conversion_count;
            best_distance = distance;
        } else if (order == 0 && !verdict.rival) {
            verdict.rival = &overload;
        }
    }

    return verdict;
}

inline overload_verdict<std::shared_ptr<function_decl>> score_declarations(
    const std::vector<std::shared_ptr<function_decl>>& overloads, const script_value* args, size_t argc, engine* eng) {
    return score_overloads(overloads, [](const std::shared_ptr<function_decl>& fn) -> const std::vector<parameter>& { return fn->parameters; }, args, argc, eng);
}

namespace detail {
    // a parameter list as a signature spells it: the declared types, `var` for an untyped slot, & for a reference
    inline std::string parameter_signature(const std::vector<parameter>& params) {
        std::string out = "(";
        for (size_t i = 0; i < params.size(); ++i) {
            if (i) { out += ", "; }
            const parameter& p = params[i];
            const bool untyped = !p.type || p.type->base_type == script_value_type::jai_any_type;
            out += untyped ? std::string("var") : p.type->canonical_name();
            if (p.is_reference && (untyped || p.type->base_type != script_value_type::jai_reference_type)) { out += "&"; }
        }
        return out + ")";
    }

    inline bool untyped_parameter(const parameter& p) { return !p.type || p.type->base_type == script_value_type::jai_any_type; }

    inline bool same_parameter_type(const parameter& a, const parameter& b) {
        if (untyped_parameter(a) != untyped_parameter(b)) { return false; }
        return untyped_parameter(a) || a.type.get() == b.type.get() || a.type->canonical_name() == b.type->canonical_name();
    }

    // One signature: the same parameter count and, at each position, the same declared type (an untyped slot is var) and the same
    // reference flag (C++: T& and T are two signatures). Defaults are not part of it.
    inline bool same_signature(const std::vector<parameter>& a, const std::vector<parameter>& b) {
        if (a.size() != b.size()) { return false; }
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].is_reference != b[i].is_reference || !same_parameter_type(a[i], b[i])) { return false; }
        }
        return true;
    }

    // Two parameter lists of the same count and types that differ only in which parameters are references
    // (such overloads are an error at the second definition, a call could only tie)
    inline bool differs_only_by_reference(const std::vector<parameter>& a, const std::vector<parameter>& b) {
        if (a.size() != b.size()) { return false; }
        bool differs = false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (!same_parameter_type(a[i], b[i])) { return false; }
            differs = differs || a[i].is_reference != b[i].is_reference;
        }
        return differs;
    }

    // a declaration as its source spells it: the name, then each parameter's type (var when untyped), & for a reference, its name
    inline std::string declaration_spelling(std::string_view name, const std::vector<parameter>& params) {
        std::string out(name);
        out += "(";
        for (size_t i = 0; i < params.size(); ++i) {
            if (i) { out += ", "; }
            const parameter& p = params[i];
            out += untyped_parameter(p) ? std::string("var") : p.type->canonical_name();
            if (p.is_reference && (untyped_parameter(p) || p.type->base_type != script_value_type::jai_reference_type)) { out += "&"; }
            out += " ";
            out += p.name;
        }
        return out + ")";
    }

    // the second of two overloads that differ only by reference (both backends and the checker: one text)
    inline std::string reference_only_overload_text(std::string_view name, const std::vector<parameter>& second, const std::vector<parameter>& first,
                                                    size_t first_line) {
        return "'" + declaration_spelling(name, second) + "' differs from '" + declaration_spelling(name, first) + "' (line " +
               std::to_string(first_line) + ") only by reference; overloads must differ in parameter types or count";
    }

    // a second definition of one signature in the same run of one program text (a function, a method)
    inline std::string duplicate_definition_text(std::string_view name, const std::vector<parameter>& params, size_t first_line) {
        return "'" + declaration_spelling(name, params) + "' is already defined (line " + std::to_string(first_line) + ")";
    }
    template <class Earlier, class Later>
    inline bool same_run_redefinition(const Earlier& earlier, const Later& later) {
        return earlier.unit.id == later.unit.id && earlier.unit.seq == later.unit.seq && same_signature(earlier.parameters, later.parameters);
    }

    // two overloads tie on these arguments: {0} = the callee, {1} = the arguments and the two signatures,
    // composed only while the symbolizer is unfrozen (a parallel worker gets the list-free shape)
    JAI_NOINLINE inline error_propagator ambiguous_call(uint64_t name_id, const std::vector<parameter>& best, const std::vector<parameter>& rival,
                                                       const script_value* args, size_t argc, engine* eng) {
        const std::error_code ec = make_error_code(runtime_error_code::ambiguous_call);
        string_symbolizer* symbols = eng ? engine_access::get_symbolizer(*eng) : nullptr;
        if (symbols && !symbols->frozen()) {
            const std::string spelled(symbols->get_string(name_id));
            return {ec, "Ambiguous call to '{0}' with {1}", name_id,
                    symbols->intern(argument_type_list(args, argc, eng) + ": '" + spelled + parameter_signature(best) + "' and '" + spelled +
                                    parameter_signature(rival) + "' match equally")};
        }
        return {ec, "Ambiguous call to '{0}'", name_id, 0};
    }
}

// The declaration a call binds: the scorer's best, else the one the arity admits (it converts or raises its own text); null
// when none does (the caller raises its miss); a tie is the ambiguous-call error
inline checked_result<std::shared_ptr<function_decl>> select_declaration(const std::vector<std::shared_ptr<function_decl>>& overloads,
                                                                       const script_value* args, size_t argc, uint64_t name_id, engine* eng);

// Constructor and method selection when the scorer finds no viable overload: the one overload the arity admits binds (its parameter
// converts the argument its slot's way or raises its own text); with several, none does and the caller raises the overload miss
inline const std::shared_ptr<function_decl>* sole_arity_candidate(const std::vector<std::shared_ptr<function_decl>>& overloads,
                                                                  const script_value* args, size_t argc) {
    const std::shared_ptr<function_decl>* sole = nullptr;
    for (const auto& o : overloads) {
        if (!arity_accepts(o->parameters, argc)) { continue; }
        if (sole) { return nullptr; }
        sole = &o;
    }
    return sole;
}

inline std::shared_ptr<function_decl> sole_arity_candidate(const std::vector<std::shared_ptr<function_decl>>& overloads,
                                                           const std::vector<script_value>& args) {
    const auto* sole = sole_arity_candidate(overloads, args.data(), args.size());
    return sole ? *sole : nullptr;
}

inline checked_result<std::shared_ptr<function_decl>> select_declaration(const std::vector<std::shared_ptr<function_decl>>& overloads,
                                                                       const script_value* args, size_t argc, uint64_t name_id, engine* eng) {
    const auto verdict = score_declarations(overloads, args, argc, eng);
    if (verdict.rival) [[unlikely]] {
        return detail::ambiguous_call(name_id, (*verdict.best)->parameters, (*verdict.rival)->parameters, args, argc, eng);
    }
    if (verdict.best) { return *verdict.best; }
    const auto* sole = sole_arity_candidate(overloads, args, argc);
    return sole ? *sole : std::shared_ptr<function_decl>();
}

inline checked_result<std::shared_ptr<function_decl>> select_declaration(const std::vector<std::shared_ptr<function_decl>>& overloads,
                                                                       const std::vector<script_value>& args, uint64_t name_id, engine* eng) {
    return select_declaration(overloads, args.data(), args.size(), name_id, eng);
}

// Cache-aware instance-method overload resolution over method_overloads_.
// Cache rules preserved exactly: entries keyed (name_id, arity); read only when
// there is more than one overload; written only when more than one overload exists
// AND exactly one matches this arity (same-arity overloads depend on argument types).
// Span core; the vector overload forwards (call-arg windows resolve on the stack slice).
inline const std::shared_ptr<function_decl>* class_definition::resolve_method_overload_core(
    uint64_t name_id, const script_value* args, size_t argc, std::error_code& ec, bool may_cache) const {
    JAI_CENSUS(overload_resolve);
    auto it = method_overloads_.find(name_id);
    if (it == method_overloads_.end() || it->second.empty()) {
        ec = make_error_code(runtime_error_code::member_not_found);
        return nullptr;
    }

    const auto& overloads = it->second;

    // one declaration binds by arity and converts like a free function's parameters (the one table)
    if (overloads.size() == 1) {
        if (arity_accepts(overloads[0]->parameters, argc)) { return &overloads[0]; }
        ec = make_error_code(runtime_error_code::argument_count_mismatch);
        return nullptr;
    }

    {
        overload_cache_key cache_key{name_id, argc};
        auto cache_it = overload_resolution_cache_.find(cache_key);
        if (cache_it != overload_resolution_cache_.end()) {
            return &cache_it->second;
        }
    }

    const auto verdict = score_declarations(overloads, args, argc, get_engine());
    if (verdict.rival) [[unlikely]] {
        ec = make_error_code(runtime_error_code::ambiguous_call);
        return nullptr;
    }
    const auto* best_ast = verdict.best;
    if (!best_ast) { best_ast = sole_arity_candidate(overloads, args, argc); }
    if (!best_ast) {
        ec = make_error_code(runtime_error_code::argument_count_mismatch);
        return nullptr;
    }

    {
        size_t arity_match_count = 0;
        for (const auto& o : overloads) {
            if (arity_accepts(o->parameters, argc)) {
                ++arity_match_count;
            }
        }
        if (arity_match_count == 1 && may_cache) {
            assert(!(engine_ && detail::engine_access::parallel_region_active(*engine_)) && "a parallel worker writes the class's shared overload cache");
            overload_cache_key cache_key{name_id, argc};
            return &overload_resolution_cache_.insert_or_assign(cache_key, *best_ast).first->second;
        }
    }

    return best_ast;
}

inline bool class_definition::method_arity_decides(uint64_t name_id, size_t argc) const {
    auto it = method_overloads_.find(name_id);
    if (it == method_overloads_.end()) { return false; }
    size_t takes = 0;
    for (const auto& o : it->second) { takes += arity_accepts(o->parameters, argc) ? 1 : 0; }
    return takes == 1;
}

inline checked_result<const std::shared_ptr<function_decl>*> class_definition::resolve_method_overload_ref(
    uint64_t name_id, const script_value* args, size_t argc) const {
    std::error_code ec;
    const auto* best = resolve_method_overload_core(name_id, args, argc, ec);
    if (best) {
        return best;
    }
    if (ec == make_error_code(runtime_error_code::member_not_found)) {
        return checked_result<const std::shared_ptr<function_decl>*>(ec, "No overload of '{0}' exists", name_id);
    }
    return method_overload_miss(ec, name_id, args, argc);
}

inline checked_result<const std::shared_ptr<function_decl>*> class_definition::resolve_method_overload_shared(
    uint64_t name_id, const script_value* args, size_t argc) const {
    std::error_code ec;
    const auto* best = resolve_method_overload_core(name_id, args, argc, ec, false);
    if (best) {
        return best;
    }
    if (ec == make_error_code(runtime_error_code::member_not_found)) {
        return checked_result<const std::shared_ptr<function_decl>*>(ec, "No overload of '{0}' exists", name_id);
    }
    return method_overload_miss(ec, name_id, args, argc);
}

inline error_propagator class_definition::method_overload_miss(std::error_code ec, uint64_t name_id, const script_value* args, size_t argc) const {
    auto it = method_overloads_.find(name_id);
    if (ec == make_error_code(runtime_error_code::ambiguous_call) && it != method_overloads_.end()) {
        const auto verdict = score_declarations(it->second, args, argc, get_engine());
        if (verdict.rival) { return detail::ambiguous_call(name_id, (*verdict.best)->parameters, (*verdict.rival)->parameters, args, argc, get_engine()); }
    }
    if (it != method_overloads_.end() && it->second.size() == 1 && !arity_accepts(it->second.front()->parameters, argc)) {
        return detail::method_arity_miss(name_id, it->second.front()->parameters.size(), argc, get_engine());
    }
    return detail::overload_miss(ec, name_id, args, argc, get_engine());
}

inline checked_result<std::shared_ptr<function_decl>> class_definition::resolve_method_overload(
    uint64_t name_id, const script_value* args, size_t argc) const {
    std::error_code ec;
    const auto* best = resolve_method_overload_core(name_id, args, argc, ec);
    if (best) {
        return *best;
    }
    if (ec == make_error_code(runtime_error_code::member_not_found)) {
        return checked_result<std::shared_ptr<function_decl>>(ec, "No overload of '{0}' exists", name_id);
    }
    return method_overload_miss(ec, name_id, args, argc);
}

inline checked_result<std::shared_ptr<function_decl>> class_definition::resolve_method_overload(
    uint64_t name_id, const std::vector<script_value>& args) const {
    return resolve_method_overload(name_id, args.data(), args.size());
}

} // namespace jai

#endif // __JAISCRIPT_CORE_OVERLOAD_RESOLUTION_HPP__
