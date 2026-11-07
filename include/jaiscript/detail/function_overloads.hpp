#pragma once

#ifndef __JAISCRIPT_DETAIL_FUNCTION_OVERLOADS_HPP__
#define __JAISCRIPT_DETAIL_FUNCTION_OVERLOADS_HPP__

// Program-scope and namespace function overloads: declarations of one name coexist as in C++, a
// declaration of the same signature replaces, and a unit run again replaces or drops only what its earlier run declared. A call
// takes the one scorer's best (core/overload_resolution.hpp); a tie on every key raises. Shared verbatim by both backends.

#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/core/overload_resolution.hpp>

namespace jai::detail {

    // An earlier run of the unit that is declaring now: its declarations the new run does not repeat are dropped
    inline bool earlier_run(unit_stamp declared, unit_stamp now) { return declared.id == now.id && declared.seq != now.seq; }

    inline const script_callable* script_callable_of(const script_value& v, engine* eng) {
        if (v.raw_storage_index() != script_value::TYPEID_FUNCTION) { return nullptr; }
        const auto* thunk = v.unchecked_as_function().target<script_callable_thunk>();
        return thunk && thunk->eng == eng ? &thunk->payload : nullptr;
    }

    JAI_NOINLINE inline checked_result<script_value> duplicate_definition_error(const script_defined_function& fn, size_t first_line, engine* eng) {
        return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "{0}",
            engine_access::symbolize(*eng, duplicate_definition_text(fn.name, fn.parameters(), first_line)));
    }

    // The value a function declaration defines its name to in `env`: the function itself, or the overload set it joins with the
    // declarations of other signatures `env` already holds under the name (a variable, a lambda or a host callable is replaced);
    // a declaration that differs from one of them only by reference is the second definition's error, and so is one of the same
    // signature from the same run of the same unit (one program text; `override` replaces on purpose, and another execute or a
    // re-run unit is a hot reload that replaces)
    inline checked_result<script_value> declare_function_overload(environment& env, uint64_t name_id, const std::shared_ptr<script_defined_function>& fn,
                                                                  script_value value, engine* eng, bool is_override = false) {
        const script_value* existing = env.get_own_value_ptr(name_id);
        const script_callable* held = existing ? script_callable_of(*existing, eng) : nullptr;
        if (!held) { return value; }
        auto set = std::make_shared<function_overload_set>();
        set->name_id = name_id;
        const script_defined_function* duplicate = nullptr;
        auto keep = [&](const std::shared_ptr<script_defined_function>& member, const script_value& member_value) {
            if (member == fn || earlier_run(member->unit, fn->unit)) { return; }
            if (same_signature(member->parameters(), fn->parameters())) {
                if (!is_override && member->unit.id == fn->unit.id && member->unit.seq == fn->unit.seq) { duplicate = member.get(); }
                return;
            }
            set->members.push_back(member);
            set->values.push_back(member_value);
        };
        if (held->kind == script_callable::kind_type::function && held->fn && held->fn->declared) {
            keep(held->fn, *existing);
        } else if (held->kind == script_callable::kind_type::overload_set && held->overloads()) {
            for (size_t i = 0; i < held->overloads()->members.size(); ++i) { keep(held->overloads()->members[i], held->overloads()->values[i]); }
        }
        if (duplicate) [[unlikely]] { return duplicate_definition_error(*fn, duplicate->line, eng); }
        // one that differs only by reference: within one execute context the second definition's error, else a redefinition that
        // replaces it (another host entry, or a re-run of the declaring unit)
        const bool redefines = engine_access::unit_rerun(*eng, fn->unit);
        for (size_t i = 0; i < set->members.size();) {
            const auto& member = set->members[i];
            if (!differs_only_by_reference(fn->parameters(), member->parameters())) { ++i; continue; }
            if (member->context == fn->context && !redefines) [[unlikely]] {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "{0}",
                    engine_access::symbolize(*eng, reference_only_overload_text(fn->name, fn->parameters(), member->parameters(), member->line)));
            }
            set->members.erase(set->members.begin() + static_cast<std::ptrdiff_t>(i));
            set->values.erase(set->values.begin() + static_cast<std::ptrdiff_t>(i));
        }
        if (set->members.empty()) { return value; }
        set->members.push_back(fn);
        set->values.push_back(std::move(value));
        script_callable payload;
        payload.kind = script_callable::kind_type::overload_set;
        payload.more().overloads = std::move(set);
        return script_value::make_function(script_function(script_callable_thunk{eng, std::move(payload)}), eng);
    }

    // The member a call binds: the scorer's best, else the one member the arity admits (its parameters convert or raise their own
    // text); a tie raises, and no viable member is the overload miss
    inline checked_result<const script_value*> select_overload_member(const function_overload_set& set, const script_value* args, size_t argc, engine* eng) {
        const auto params_of = [](const std::shared_ptr<script_defined_function>& f) -> const std::vector<parameter>& { return f->parameters(); };
        const auto verdict = score_overloads(set.members, params_of, args, argc, eng);
        if (verdict.rival) [[unlikely]] {
            return ambiguous_call(set.name_id, (*verdict.best)->parameters(), (*verdict.rival)->parameters(), args, argc, eng);
        }
        if (verdict.best) { return &set.values[static_cast<size_t>(verdict.best - set.members.data())]; }
        const script_value* sole = nullptr;
        for (size_t i = 0; i < set.members.size(); ++i) {
            if (!arity_accepts(set.members[i]->parameters(), argc)) { continue; }
            if (sole) { sole = nullptr; break; }
            sole = &set.values[i];
        }
        if (sole) { return sole; }
        return overload_miss(make_error_code(runtime_error_code::argument_count_mismatch), set.name_id, args, argc, eng, false);
    }

    // A namespace function declaration (both backends' namespace declaration): an earlier run of the declaring unit drops what it
    // declared; a declaration of the same signature that stays collides (the caller raises unless `override`, which erases it), one
    // that differs only by reference is the second definition's error (its text in `error`)
    enum class namespace_admission : uint8_t { admitted, collision, reference_only };
    inline namespace_admission admit_namespace_function(std::vector<std::shared_ptr<function_decl>>& overloads, const function_decl& fn, bool redefines,
                                                        std::string& error) {
        std::erase_if(overloads, [&](const std::shared_ptr<function_decl>& o) { return earlier_run(o->unit, fn.unit); });
        for (auto it = overloads.begin(); it != overloads.end(); ++it) {
            if (!same_signature((*it)->parameters, fn.parameters)) { continue; }
            if (!fn.is_override) { return namespace_admission::collision; }
            overloads.erase(it);
            break;
        }
        for (auto it = overloads.begin(); it != overloads.end();) {
            if (!differs_only_by_reference(fn.parameters, (*it)->parameters)) { ++it; continue; }
            if ((*it)->context == fn.context && !redefines) [[unlikely]] {
                error = reference_only_overload_text(fn.name, fn.parameters, (*it)->parameters, (*it)->location.line);
                return namespace_admission::reference_only;
            }
            it = overloads.erase(it);   // another execute context or the unit's re-run: a redefinition
        }
        return namespace_admission::admitted;
    }

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_FUNCTION_OVERLOADS_HPP__
