#pragma once

#ifndef __JAISCRIPT_DETAIL_ARG_CAPTURE_HPP__
#define __JAISCRIPT_DETAIL_ARG_CAPTURE_HPP__

// A by-value argument takes its value when it is evaluated, and a reference parameter binds its argument then.
// The parse-time stamp (transient_read_marker::stamp_arg_captures) names the arguments a later argument may write;
// each backend captures such an argument right after evaluating it, by how the resolved callee binds that position, and mints
// the reference of a stamped lvalue a reference parameter takes. Shared verbatim by both backends.

#include <jaiscript/core/script_class.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/return_conv.hpp>
#include <jaiscript/detail/transient_read.hpp>
#include <bit>

namespace jai::detail {

    // copy: a by-value parameter the binding copies; share: a handle parameter (shared_ptr/weak_ptr) the binding shares;
    // reference: the lvalue binds, nothing is taken; opaque: a host callable, whose C++ parameter decides (the value is
    // dereferenced, a heavy handle stays the object the argument named)
    enum class arg_binding : uint8_t { copy = 1, share = 2, reference = 4, opaque = 8 };

    inline bool deferred_binding(arg_binding b) {
        const auto bits = static_cast<uint8_t>(b);
        return (bits & (bits - 1)) != 0;
    }

    inline arg_binding parameter_binding(const parameter& p) {
        if (p.is_reference) return arg_binding::reference;
        if (p.type && (p.type->base_type == script_value_type::jai_shared_ptr_type || p.type->base_type == script_value_type::jai_weak_ptr_type)) return arg_binding::share;
        return arg_binding::copy;
    }

    inline arg_binding parameters_binding(const std::vector<parameter>& params, size_t i) {
        return i < params.size() ? parameter_binding(params[i]) : arg_binding::copy;
    }

    // Different viable parameter modes need both interpretations until dispatch selects an overload.
    // The common single-mode case does not allocate or copy anything here.
    inline arg_binding overloads_binding(const std::vector<std::shared_ptr<function_decl>>& overloads, size_t i, size_t argc = SIZE_MAX) {
        uint8_t result = 0;
        for (const auto& o : overloads) {
            if (!o || (argc != SIZE_MAX && !arity_accepts(o->parameters, argc))) continue;
            const arg_binding b = parameters_binding(o->parameters, i);
            result |= static_cast<uint8_t>(b);
        }
        return result ? static_cast<arg_binding>(result) : arg_binding::opaque;
    }

    inline arg_binding method_binding(const class_definition* cls, uint64_t name_id, size_t i, size_t argc = SIZE_MAX) {
        if (!cls) return arg_binding::opaque;
        const auto& table = cls->script_method_overloads();
        auto it = table.find(name_id);
        return it == table.end() ? arg_binding::opaque : overloads_binding(it->second, i, argc);
    }

    inline arg_binding constructor_binding(const class_definition* cls, size_t i, size_t argc = SIZE_MAX) {
        if (!cls || !cls->is_script_class()) return arg_binding::opaque;
        const auto& ctors = static_cast<const script_class_definition*>(cls)->get_constructor_asts();
        return ctors.empty() ? arg_binding::copy : overloads_binding(ctors, i, argc);
    }

    inline arg_binding callee_binding(const script_value& callee, size_t i, bool through_bound = false, size_t argc = SIZE_MAX);

    // how a callable binds argument i
    inline arg_binding function_binding(const script_function& fn, size_t i, bool through_bound = false, size_t argc = SIZE_MAX) {
        if (const auto* thunk = fn.target<script_callable_thunk>()) {
            const script_callable& p = thunk->payload;
            switch (p.kind) {
                case script_callable::kind_type::function: return p.fn ? parameters_binding(p.fn->parameters(), i) : arg_binding::opaque;
                case script_callable::kind_type::method:
                case script_callable::kind_type::static_method: return p.ast() ? parameters_binding(p.ast()->parameters, i) : arg_binding::opaque;
                case script_callable::kind_type::constructor: return constructor_binding(p.cls().get(), i, argc);
                case script_callable::kind_type::bound_method: return p.bound_dispatch() && !through_bound ? callee_binding(*p.bound_dispatch(), i, true, argc) : arg_binding::opaque;
                case script_callable::kind_type::overload_set: {
                    uint8_t modes = 0;
                    if (p.overloads()) {
                        for (const auto& m : p.overloads()->members) {
                            if (argc == SIZE_MAX || arity_accepts(m->parameters(), argc)) { modes |= static_cast<uint8_t>(parameters_binding(m->parameters(), i)); }
                        }
                    }
                    return modes ? static_cast<arg_binding>(modes) : arg_binding::opaque;
                }
            }
        }
        if (const auto* d = fn.target<script_static_method_dispatch>()) {
            const auto& table = d->cls->script_static_method_overloads();
            const auto it = table.find(d->name_id);
            return it == table.end() ? parameters_binding(d->fallback->parameters, i) : overloads_binding(it->second, i, argc);
        }
        if (const auto* d = fn.target<script_method_dispatch>()) return method_binding(d->cls.get(), d->name_id, i, argc);
        if (const auto* bm = fn.target<bound_method_fn>(); bm && bm->method.raw_storage_index() == script_value::TYPEID_FUNCTION) {
            if (const auto* d = bm->method.unchecked_as_function().target<script_method_dispatch>()) return method_binding(d->cls.get(), d->name_id, i, argc);
        }
        // a host callable's non-const T& (a C++ T&, which the call writes back) binds the lvalue
        if (const auto* w = fn.target<host_writeback_function>()) return i < 64 && ((w->mask >> i) & 1) ? arg_binding::reference : arg_binding::opaque;
        if (const auto* d = fn.target<engine_access::overload_dispatch>()) return static_cast<arg_binding>(d->argument_binding(i, argc));
        if (const auto* d = fn.target<class_definition::cpp_method_dispatch>()) return static_cast<arg_binding>(d->argument_binding(i, argc));
        return arg_binding::opaque;
    }

    inline arg_binding callee_binding(const script_value& callee, size_t i, bool through_bound, size_t argc) {
        const script_value& c = callee.deref();
        if (c.raw_storage_index() != script_value::TYPEID_FUNCTION) return arg_binding::opaque;
        return function_binding(c.unchecked_as_function(), i, through_bound, argc);
    }

    // `recv.m(args)` resolved by lookup alone (no getter runs): a field's callable, else the class's method; `super.m` the
    // receiver's parent's method; any other receiver is a host or builtin callee
    inline arg_binding member_call_binding(const script_value& receiver, uint64_t member_id, bool super_call, size_t i, size_t argc = SIZE_MAX) {
        const script_value& recv = receiver.deref();
        const size_t ri = recv.raw_storage_index();
        if (ri != script_value::TYPEID_OBJECT) return arg_binding::opaque;
        const auto holder = recv.get_object_holder();
        if (!holder || !holder->is_instance()) return arg_binding::opaque;
        auto* inst = holder->unchecked_instance();
        const class_definition* cd = inst->get_class_definition();
        if (!cd) return arg_binding::opaque;
        if (super_call) {
            const auto parent = cd->get_parent();
            return parent ? callee_binding(parent->get_method(member_id, false), i, false, argc) : arg_binding::opaque;
        }
        if (const script_value* field = inst->find_field_value(member_id)) return callee_binding(*field, i, false, argc);
        return callee_binding(cd->get_method(member_id, false), i, false, argc);
    }

    // A host callable's received argument is still the one the call site passed (the callee may hold a dereferenced or
    // re-wrapped copy of it): the same scalar, text or math value, the same heavy payload. A callback a builtin makes
    // with its own arguments fails it.
    inline bool same_argument(const script_value& passed_in, const script_value& received_in) {
        const script_value& passed = passed_in.deref();
        const script_value& received = received_in.deref();
        const size_t k = passed.raw_storage_index();
        if (k != received.raw_storage_index()) return false;
        switch (k) {
            case script_value::TYPEID_NULL: return true;
            case script_value::TYPEID_INT: return passed.unchecked_as_int() == received.unchecked_as_int();
            case script_value::TYPEID_FLOAT: return std::bit_cast<uint64_t>(passed.unchecked_as_float()) == std::bit_cast<uint64_t>(received.unchecked_as_float());
            case script_value::TYPEID_STRING:
            case script_value::TYPEID_UTF8_STRING:
            case script_value::TYPEID_CHAR:
            case script_value::TYPEID_BOOL:
            case script_value::TYPEID_CPP_BOUND: return passed == received;
            case script_value::TYPEID_ARRAY: return const_cast<script_value&>(passed).get_array_storage().get() == const_cast<script_value&>(received).get_array_storage().get();
            case script_value::TYPEID_MAP: return &passed.unchecked_as_map() == &received.unchecked_as_map();
            case script_value::TYPEID_OBJECT: return passed.get_object_holder().get() == received.get_object_holder().get();
            case script_value::TYPEID_FUNCTION: return &passed.unchecked_as_function() == &received.unchecked_as_function();
            default: return script_value::is_math_index(k) && passed == received;
        }
    }

    // The pending call-site context belongs to this call: the callee received what the site passed (null = a context
    // armed without its arguments). A callback a builtin or a host makes with its own arguments does not match.
    inline bool same_arguments(const std::vector<script_value>* passed, const std::vector<script_value>& received) {
        if (!passed || passed == &received) return true;
        if (passed->size() != received.size()) return false;
        for (size_t i = 0; i < received.size(); ++i) {
            if (!same_argument((*passed)[i], received[i])) return false;
        }
        return true;
    }

    inline auto deferred_argument(const script_value& v) {
        const auto* holder = v.is_reference() ? v.get_reference_holder() : nullptr;
        return holder && holder->argument_source ? holder : nullptr;
    }

    inline bool binds_reference(arg_binding b) {
        return (static_cast<uint8_t>(b) & static_cast<uint8_t>(arg_binding::reference)) != 0;
    }

    // the reference the argument's evaluation bound, while no binding has taken it
    inline const script_value* early_bound_reference(const script_value& v) {
        const auto* holder = deferred_argument(v);
        return holder && holder->argument_source->minted ? &holder->argument_source->lvalue : nullptr;
    }

    // A by-value parameter's default binds as the argument it stands in for, as `T p = default;` would hold: the parameter's
    // conversion, then a copy when the result is still an existing object's storage (a temporary is the parameter itself)
    template <typename Convert>
    inline checked_result<script_value> default_argument_value(const script_value& evaluated, const parameter& p, Convert&& convert) {
        const script_value& source = evaluated.deref();
        auto converted = convert(source);
        if (!converted) { return converted; }
        script_value result = std::move(converted.value());
        if (result.is_reference()) { result = script_value(result.deref()); }
        if ((evaluated.is_reference() || is_lvalue_shaped_expr(p.default_value.get())) && same_heavy_node(result, source)) {
            result = clone_for_assignment(result);
        }
        return result;
    }

    // A reference parameter's default must name storage, as in C++; a const one binds a temporary
    inline bool default_binds_reference(const parameter& p, const script_value& bound) noexcept {
        return !p.is_reference || p.is_const || bound.is_reference();
    }
    JAI_NOINLINE inline checked_result<script_value> non_lvalue_default_error(const parameter& p, string_symbolizer& symbols) {
        if (p.symbol_id == UINT64_MAX) { p.symbol_id = symbols.intern(p.name); }
        return checked_result<script_value>(make_error_code(runtime_error_code::invalid_reference),
            "Cannot bind non-lvalue default to reference parameter '{0}'", p.symbol_id);
    }

    inline const script_value& argument_for_binding(const script_value& v, arg_binding b) {
        if (const auto* holder = deferred_argument(v)) {
            if (b == arg_binding::copy) return *holder->cell();
            return b == arg_binding::reference ? holder->argument_source->lvalue : holder->argument_source->shared;
        }
        return v;
    }

    // what the binding would make of the argument now, in place: a copy parameter's value is dereferenced, its heavy payload
    // copied and a host-bound primitive decoded; a handle parameter's and an opaque callee's is dereferenced
    inline void capture_argument(engine* eng, script_value& v, uint8_t kind, arg_binding b) {
        if (deferred_binding(b)) {
            if (deferred_argument(v)) return;
            script_value source = v;
            script_value shared = v.deref();
            script_value snapshot = v;
            const bool needs_copy = (static_cast<uint8_t>(b) & static_cast<uint8_t>(arg_binding::copy)) != 0;
            capture_argument(eng, snapshot, kind, needs_copy ? arg_binding::copy : arg_binding::share);
            const auto type = snapshot.get_type_info();
            script_value captured = script_value::make_cell_reference(std::move(snapshot), eng);
            captured.set_type_info(type);   // overload selection sees the value's evaluated type
            captured.get_reference_holder()->argument_source = std::make_unique<argument_capture_storage>(argument_capture_storage{std::move(source), std::move(shared)});
            v = std::move(captured);
            return;
        }
        if (deferred_argument(v)) {
            if (b == arg_binding::reference && early_bound_reference(v)) return;   // the binding takes the minted reference
            script_value selected = argument_for_binding(v, b);
            v = std::move(selected);
        }
        if (b == arg_binding::reference) return;
        if (v.raw_storage_index() == script_value::TYPEID_REFERENCE) {
            script_value d = v.deref();
            v = std::move(d);
        } else if (kind != transient_read_marker::arg_capture_full) {
            return;
        }
        if (b != arg_binding::copy) return;
        const size_t ri = v.raw_storage_index();
        if (ri == script_value::TYPEID_CPP_BOUND) { v = v.detached_for_store(); return; }
        if (ri != script_value::TYPEID_OBJECT && ri != script_value::TYPEID_ARRAY && ri != script_value::TYPEID_MAP) return;
        if (v.get_type_info() && v.get_type_info()->base_type == script_value_type::jai_shared_ptr_type) return;
        if (uniquely_held(v)) return;   // a temporary nothing else reaches: no later code can write it, and it stays its parameter
        try { v = v.clone(); } catch (const std::exception&) {}   // a value that refuses the copy (nocopy) raises at the binding, as it always has
    }

    // the argument's evaluation bound its reference: carried beside the value overload selection, a copy and a handle parameter take
    inline void hold_minted_reference(engine* eng, script_value& v, arg_binding b, script_value&& minted) {
        if (auto* holder = deferred_argument(v)) {
            holder->argument_source->lvalue = std::move(minted);
            holder->argument_source->minted = true;
            return;
        }
        script_value shared = v.deref();
        script_value snapshot = v;
        const bool needs_copy = (static_cast<uint8_t>(b) & static_cast<uint8_t>(arg_binding::copy)) != 0;
        capture_argument(eng, snapshot, transient_read_marker::arg_capture_full, needs_copy ? arg_binding::copy : arg_binding::share);
        const auto type = snapshot.get_type_info();
        script_value held = script_value::make_cell_reference(std::move(snapshot), eng);
        held.set_type_info(type);
        held.get_reference_holder()->argument_source = std::make_unique<argument_capture_storage>(argument_capture_storage{std::move(minted), std::move(shared), true});
        v = std::move(held);
    }

    inline bool capture_candidate(const script_value& v, uint8_t kind) {
        const size_t ri = v.raw_storage_index();
        if (ri == script_value::TYPEID_REFERENCE) return true;
        return kind == transient_read_marker::arg_capture_full &&
               (ri == script_value::TYPEID_OBJECT || ri == script_value::TYPEID_ARRAY || ri == script_value::TYPEID_MAP || ri == script_value::TYPEID_CPP_BOUND);
    }

    // argument i of a stamped list, just evaluated: classified (binding() resolves the callee) only when its value may still share storage
    template<typename Binding>
    inline void capture_call_argument(engine* eng, const std::vector<uint8_t>& captures, size_t i, script_value& v, Binding&& binding) {
        if (i >= captures.size() || captures[i] == 0) return;
        const uint8_t kind = captures[i] & transient_read_marker::arg_capture_kind_mask;
        if (!capture_candidate(v, kind)) return;
        capture_argument(eng, v, kind, binding());
    }

    // the same with the stamp's lvalues: a reference parameter's argument mints its reference now (mint(out) -> checked_result<bool>
    // resolves the argument's place in the caller; false = no place, the binding decides as for any evaluated argument)
    template<typename Binding, typename Mint>
    inline checked_result<void> capture_call_argument(engine* eng, const std::vector<uint8_t>& captures, size_t i, script_value& v, Binding&& binding, Mint&& mint) {
        if (i >= captures.size() || captures[i] == 0) return {};
        const uint8_t kind = captures[i] & transient_read_marker::arg_capture_kind_mask;
        if (!(captures[i] & transient_read_marker::arg_capture_lvalue)) {
            if (capture_candidate(v, kind)) { capture_argument(eng, v, kind, binding()); }
            return {};
        }
        const arg_binding b = binding();
        if (binds_reference(b)) {
            script_value minted(std::monostate{}, eng);
            auto placed = mint(minted);
            if (!placed) { return placed.error_value(); }
            if (placed.value()) {
                hold_minted_reference(eng, v, b, std::move(minted));
                return {};
            }
        }
        if (capture_candidate(v, kind)) { capture_argument(eng, v, kind, b); }
        return {};
    }

    // an initializer list's argument, as a call's: its value taken now when a later argument may write what it read (the stamp)
    template<typename Binding>
    inline void capture_initializer_argument(engine* eng, const constructor_initializer& ci, size_t i, script_value& v, Binding&& binding) {
        const uint8_t kind = i < ci.arg_captures.size() ? ci.arg_captures[i] & transient_read_marker::arg_capture_kind_mask : 0;
        if (kind == 0 || !capture_candidate(v, kind)) return;
        capture_argument(eng, v, kind, binding());
    }

    // a parameter's conversion that may run user code (a converting constructor, an object's to_int/to_float, a class element
    // conversion): the arguments after it were taken before the binding began, so they are captured ahead of it
    inline bool binding_may_run_code(const script_value& arg, const type_info* t) {
        if (!t || t->base_type == script_value_type::jai_any_type) return false;
        const script_value& d = arg.deref();
        const size_t ri = d.raw_storage_index();
        const bool object = ri == script_value::TYPEID_OBJECT;
        switch (t->base_type) {
            case script_value_type::jai_object_type: return !(object && d.get_type_info().get() == t);
            case script_value_type::jai_int_type:
            case script_value_type::jai_float_type:
            case script_value_type::jai_bool_type:
            case script_value_type::jai_char_type:
            case script_value_type::jai_string_type: return object;
            case script_value_type::jai_array_type: { const auto et = t->element_type(); return et && et->base_type == script_value_type::jai_object_type; }
            case script_value_type::jai_map_type: { const auto vt = t->value_type(); return vt && vt->base_type == script_value_type::jai_object_type; }
            default: return false;
        }
    }

    template<typename At>
    inline bool later_capture_candidate(size_t from, size_t argc, At&& at) {
        for (size_t j = from; j < argc; ++j) {
            if (capture_candidate(at(j), transient_read_marker::arg_capture_full) && !uniquely_held(at(j))) return true;
        }
        return false;
    }

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_ARG_CAPTURE_HPP__
