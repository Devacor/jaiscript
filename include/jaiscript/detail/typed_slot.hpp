#pragma once

#ifndef __JAISCRIPT_DETAIL_TYPED_SLOT_HPP__
#define __JAISCRIPT_DETAIL_TYPED_SLOT_HPP__

// The script half of the typed-slot conversion table (class_definition.hpp
// convert_for_typed_slot_core owns the rows and the verdicts): a TRUE one-parameter
// constructor whose declared parameter takes the source DIRECTLY (no chaining, no to_X()
// on the way in), to_int/to_float/to_char/to_string, and to_bool(). Both backends and the
// field store run detail::convert_for_typed_slot verbatim; each site maps the verdict onto
// its own texts (docs/site/embedding/06-static-checking.html "One conversion table"). A user conversion that fails
// propagates its own error.

#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/core/script_class.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/container_enforce.hpp>
#include <jaiscript/detail/parallel_transform.hpp>
#include <jaiscript/detail/return_conv.hpp>
#include <string>
#include <utility>
#include <vector>

namespace jai::detail {

    // Source spelling for rejection texts: detail::slot_source_type_name (class_definition.hpp,
    // next to the slot texts both wrappers raise through).

    // v's conversion method name (to_bool, to_int, ...), nullopt when its class binds none. A parallel chunk runs it through the
    // region's pin (an unpinned one raises what the region refused), never through the class's shared method value
    inline std::optional<checked_result<script_value>> call_conversion_method(const script_value& v, const class_definition* def, const char* name,
                                                                              engine* eng) {
        const uint64_t id = engine_access::get_symbolizer(*eng)->intern(name);
        const script_value* method = def ? def->method_ptr(id) : nullptr;
        if (!method || !method->is_function()) { return std::nullopt; }
        if (auto pinned = engine_access::parallel_operator_call(*eng, v, id, {})) { return pinned; }
        return make_bound_method(v, *method).as_function()(std::vector<script_value>{});
    }

    // consumable: a prvalue source the caller gives up (it may alias value_in): a converting constructor takes it as its parameter
    inline checked_result<slot_conversion> convert_typed_slot_script_rows_impl(slot_conversion pending, const script_value& value_in,
                                                                                const type_info_ptr& target_type, engine* eng,
                                                                                script_value* consumable = nullptr) {
        using verdict = slot_conversion::verdict;
        const script_value& v = value_in.is_reference() ? value_in.deref() : value_in;
        const auto target = target_type->base_type;
        switch (pending.v) {
            case verdict::needs_to_bool: {
                // KEEP BYTE-PARALLEL with the backends' object_to_bool_via_method
                auto view = resolve_slot_class(v, eng);
                bool truthy = true;
                if (auto result = call_conversion_method(v, view.def, "to_bool", eng)) {
                    if (!*result) {
                        return result->error_value();   // to_bool()'s runtime error is the conversion's, like to_int()'s below
                    }
                    if (result->value().is_bool()) {
                        truthy = result->value().unchecked_as_bool();
                    }
                }
                return slot_conversion::converted_to(script_value(truthy, eng));
            }
            case verdict::needs_to_x: {
                const char* name = target == script_value_type::jai_int_type ? "to_int"
                                 : target == script_value_type::jai_float_type ? "to_float"
                                 : target == script_value_type::jai_char_type ? "to_char" : "to_string";
                auto view = resolve_slot_class(v, eng);
                auto called = call_conversion_method(v, view.def, name, eng);
                if (!called) {
                    return slot_conversion::of(verdict::reject);
                }
                checked_result<script_value>& result = *called;
                if (!result) {
                    return result.error_value();
                }
                if (auto* backend = engine_access::calling_backend(*eng); backend && backend->is_unwinding()) {
                    // the method threw: the in-flight exception wins, the payload is never inspected
                    return slot_conversion::converted_to(std::move(result.value()));
                }
                // the result converts once more through the primitive rows, never through script again
                script_value produced = result.value().is_reference() ? result.value().deref() : std::move(result.value());
				auto converted = convert_for_typed_slot_core(produced, target_type, eng);
				if (!converted) { return converted.error_value(); }
				slot_conversion again = std::move(converted.value());
                if (again.v == verdict::identity || again.v == verdict::identity_exact) {
                    return slot_conversion::converted_to(std::move(produced));
                }
                if (again.v == verdict::converted) {
                    return again;
                }
                return slot_conversion::of(verdict::reject);
            }
            case verdict::needs_ctor: {
                const std::string& target_class_name = target_type->type_name;
                const size_t ri = v.raw_storage_index();
                const script_value_type src = v.current_type();
                const bool object_src = ri == script_value::TYPEID_OBJECT ||
                                        (ri == script_value::TYPEID_CPP_BOUND && src == script_value_type::jai_null_type);
                const verdict no = object_src ? verdict::reject_class : verdict::reject;
                auto conv_class_def = engine_access::get_class_definition(eng, target_class_name);
                // Ruling: a defaulted-param ctor is NOT a converting ctor - implicit conversion needs a
                // TRUE 1-param ctor (a delegating overload opts in); host classes: the dispatcher decides
                if (conv_class_def && !has_true_single_param_conversion_ctor(conv_class_def)) {
                    return slot_conversion::of(no);
                }
                if (auto script_class = std::dynamic_pointer_cast<script_class_definition>(conv_class_def)) {
                    // exactly one user conversion: the ctor's declared parameter must take the source itself
                    std::string source_name;
                    class_definition* source_def = nullptr;
                    if (object_src) {
                        auto view = resolve_slot_class(v, eng);
                        if (view.def) {
                            source_name = view.def->get_name();
                            source_def = view.def;
                        }
                    }
                    bool matching = false;
                    for (const auto& ctor_ast : script_class->get_constructor_asts()) {
                        if (ctor_ast->parameters.size() != 1) continue;
                        const auto& param = ctor_ast->parameters[0];
                        if (!param.type || param.type->type_name.empty() || param.type->base_type == script_value_type::jai_any_type) {
                            matching = true;
                            break;
                        }
                        if (object_src) {
                            if (param.type->type_name == source_name ||
                                (source_def && source_def->is_subtype_of(param.type->type_name))) {
                                matching = true;
                                break;
                            }
                        } else if (param.type->base_type == src ||
                                   (src == script_value_type::jai_int_type && param.type->base_type == script_value_type::jai_float_type)) {
                            matching = true;
                            break;
                        }
                    }
                    if (!matching) {
                        return slot_conversion::of(no);
                    }
                }
                if (!eng) {
                    return slot_conversion::of(no);
                }
                const uint64_t class_name_id = engine_access::symbolize(*eng, target_class_name);
                script_value ctor(std::monostate{}, eng);
                if (auto chunk_ctor = parallel_chunk_binding(*eng, class_name_id)) {   // a chunk constructs with its own constructor
                    if (!chunk_ctor->is_function()) {
                        return parallel_conversion_not_admitted(class_name_id).error_value();
                    }
                    ctor = std::move(*chunk_ctor);
                } else {
                    auto global = engine_access::get_global_environment(*eng);
                    auto ctor_result = global ? global->get(class_name_id) : checked_result<script_value>(script_value(std::monostate{}, eng));
                    if (!ctor_result || !ctor_result.value().is_function()) {
                        return slot_conversion::of(no);
                    }
                    ctor = std::move(ctor_result.value());
                }
                std::vector<script_value> ctor_args;
                execution_backend* owner = consumable && !consumable->is_reference() ? engine_access::calling_backend(*eng) : nullptr;
                if (owner) {
                    ctor_args.push_back(std::move(*consumable));
                } else {
                    ctor_args.push_back(value_in);
                }
                struct owned_arguments_scope {
                    execution_backend* backend;
                    std::vector<script_value>* previous;
                    ~owned_arguments_scope() { if (backend) { backend->exchange_owned_arguments(previous); } }
                } owned{owner, owner ? owner->exchange_owned_arguments(&ctor_args) : nullptr};
                auto result = ctor.as_function()(ctor_args);
                if (!result) {
                    if (result.error() == make_error_code(runtime_error_code::no_constructor_found)) {
                        if (owner) { *consumable = std::move(ctor_args[0]); }
                        return slot_conversion::of(no);
                    }
                    return result.error_value();
                }
                return slot_conversion::converted_to(std::move(result.value()));
            }
            default:
                return pending;
        }
    }

    inline bool primitive_slot_type(script_value_type t) noexcept {
        return t == script_value_type::jai_int_type || t == script_value_type::jai_float_type || t == script_value_type::jai_char_type ||
               t == script_value_type::jai_bool_type || t == script_value_type::jai_string_type || t == script_value_type::jai_utf8_string_type;
    }

    // The full table: the script-free core, then the script rows where the core asked.
    inline checked_result<slot_conversion> convert_for_typed_slot(const script_value& value, const type_info_ptr& target_type, engine* eng,
                                                                  script_value* consumable = nullptr) {
        if (value.deref().raw_storage_index() == script_value::TYPEID_PARALLEL_BORROW && target_type && primitive_slot_type(target_type->base_type) &&
            value.deref().parallel_object_borrow()) {
            return convert_for_typed_slot(parallel_by_value(value.deref()), target_type, eng);   // a shared object converts as the worker's copy
        }
		auto result = convert_for_typed_slot_core(value, target_type, eng);
		if (!result) { return result.error_value(); }
		slot_conversion out = std::move(result.value());
        if (out.needs_script()) {
            return convert_typed_slot_script_rows_impl(std::move(out), value, target_type, eng, consumable);
        }
        return out;
    }

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_TYPED_SLOT_HPP__
