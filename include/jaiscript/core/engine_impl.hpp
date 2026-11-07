#pragma once

#ifndef __JAISCRIPT_CORE_ENGINE_IMPL_HPP__
#define __JAISCRIPT_CORE_ENGINE_IMPL_HPP__

#include "engine.hpp"
#include "conversion_registry.hpp"
#include "class_definition.hpp"
#include "script_extensible.hpp"

namespace jai {

namespace detail {
	// A script callback a C++ caller ran: every failure throws to the host (the throwing flavor), a script throw the callback
	// did not catch included; a host function called from script hands it back as that call's runtime error, text included
	inline void throw_script_callback_failure(engine* eng, const checked_result<script_value>& result) {
		if (!result) {
			throw runtime_error(eng->format_error(result));
		}
		if (engine_access::backend_unwinding(*eng)) {
			throw runtime_error(engine_access::backend_exception_text(*eng));
		}
	}

	// Sig decomposition for convert_script_function (declared in function_binder.hpp)
	template<typename Sig> struct script_function_wrapper;

	template<typename R, typename... Args>
	struct script_function_wrapper<R(Args...)> {
		static std::function<R(Args...)> wrap(script_value fn) {
			// a null callback is an empty std::function, as a null function pointer converts in C++; any other non-function is
			// the conversion's failure
			if (fn.deref().is_null()) { return {}; }
			if (!fn.is_function() || !fn.payload_engine()) { throw runtime_error("Cannot convert a value that is not a function to a std::function callback"); }
			std::weak_ptr<const void> alive = engine_access::lifetime_token(*fn.payload_engine());
			return [fn = std::move(fn), alive = std::move(alive)](Args... args) -> R {
				// a callback the host kept past its engine never reaches the dead engine
				if (alive.expired()) { throw runtime_error("Cannot call a script function after its engine was destroyed"); }
				engine* eng = fn.payload_engine();
				engine::external_call_guard scope(eng);
				std::vector<script_value> values;
				values.reserve(sizeof...(Args));
				(values.push_back(eng->make_value(args)), ...);
				auto result = fn.as_function()(values);
				throw_script_callback_failure(eng, result);
				if constexpr (!std::is_void_v<R>) {
					return result.value().template as<R>();
				}
			};
		}
	};
}

namespace detail {
	// A recorded instance stands for an object handed out under a class when its C++ class and that class derive one from the other
	inline bool same_class_family(const class_definition* a_known, const class_definition* a_handed) {
		if (!a_known || !a_handed) { return true; }
		const class_definition* known_cpp = a_known->is_script_class() ? a_known->find_cpp_base_class().get() : a_known;
		return !known_cpp || known_cpp->derives_from(a_handed) || a_handed->derives_from(known_cpp);
	}
}

template<typename Sig>
std::function<Sig> convert_script_function(const script_value& fn) {
	return detail::script_function_wrapper<Sig>::wrap(fn);
}

template<typename T>
std::string engine::get_registered_name() const {
    std::shared_ptr<detail::first_type_t<class_definition, T>> class_def = get_class_definition_by_type(std::type_index(typeid(T)));
    if (class_def) {
        return class_def->get_name();
    }
    return typeid(T).name();
}

template<typename T>
script_value engine::make_value_object(std::shared_ptr<T> data) {
    // Prefer the DYNAMIC type's registration: handles cross boundaries as their base
    // static type (signal signatures, vector elements) but script wants the most-derived
    // surface (a Derived member reached through a shared_ptr<Base>). dynamic_cast<void*>
    // yields the complete-object pointer, so registry extractors keyed by the dynamic
    // type name cast correctly regardless of base offsets.
    // An object's script layer IS its identity: hand it back instead of a fresh wrapper, so
    // script sees the derived script class (fields, overrides) through any boundary return
    // — virtual dispatch through a base pointer, carried across the boundary. Slot when the
    // type carries one (a pointer read), else the map by complete-object pointer.
    // A plain wrapper is the object's one identity too (recorded by its complete-object pointer, handed out again for an
    // owner-equivalent handle), so an extend after the object was bound re-classes the instance every holder already has.
    const void* key = nullptr;
    bool key_held_by_another_object = false;
    if (data) {
        if constexpr (std::is_polymorphic_v<T>) { key = dynamic_cast<const void*>(data.get()); } else { key = static_cast<const void*>(data.get()); }
        // While a region runs an object found here is shared: workers read it through a borrow, never its handle
        const bool region = parallel_region_active_;
        if constexpr (std::is_base_of_v<script_extensible, T>) {
            if (region) [[unlikely]] {
                if (const auto* layer = data->jai_script_slot().peek_script_layer(); layer && layer->get_engine() == this) {
                    return parallel_borrow_of(*layer, layer->get_class_definition()->get_type_info());
                }
            } else if (auto layer = data->jai_script_slot().script_layer(); layer && layer->get_engine() == this) {
                return script_value::make_object(layer->get_class_definition()->get_name(), layer, this);
            }
        }
        strong_ptr<detail::first_type_t<class_instance, T>> known_handle;
        const detail::first_type_t<class_instance, T>* known = nullptr;
        if (region) [[unlikely]] { known = object_instance_peek(key); } else { known_handle = object_instance_of(key); known = known_handle.get(); }
        if (known) {
            const class_definition* def = known->get_class_definition();
            const script_value* held = known->native();
            const auto* holder = held ? held->peek_object_holder() : nullptr;   // a read: workers reach one object's holder together
            const bool layer = def && def->is_script_class() && !std::is_base_of_v<script_extensible, T>;
            const bool same_object = holder && holder->holds_object() && !holder->host_object().owner_before(data) && !data.owner_before(holder->host_object());
            // A member at offset 0 shares its enclosing object's address (and, through an aliasing handle, its owner)
            std::type_index handed_type = std::type_index(typeid(T));
            if constexpr (std::is_polymorphic_v<T>) { handed_type = std::type_index(typeid(*data)); }
            std::shared_ptr<detail::first_type_t<class_definition, T>> handed_def = get_class_definition_by_type(handed_type);
            if (!handed_def && handed_type != std::type_index(typeid(T))) { handed_def = get_class_definition_by_type(std::type_index(typeid(T))); }
            const bool same_type_family = detail::same_class_family(def, handed_def.get());
            if (def && (layer || same_object) && same_type_family) {
                if (region) [[unlikely]] { return parallel_borrow_of(*known, def->get_type_info()); }
                return script_value::make_object(def->get_name(), known_handle, this);
            }
            key_held_by_another_object = def && !same_type_family;
        }
    }
    if constexpr (std::is_polymorphic_v<T>) {
        if (data && std::type_index(typeid(*data)) != std::type_index(typeid(T))) {
            if (std::shared_ptr<detail::first_type_t<class_definition, T>> dynamic_def = get_class_definition_by_type(std::type_index(typeid(*data)))) {
                std::shared_ptr<void> complete_object(data, dynamic_cast<void*>(data.get()));
                auto instance = detail::make_native_instance(*dynamic_def, dynamic_def->get_name(), complete_object, this);
                if (!key_held_by_another_object) { note_script_layer(key, instance); }
                return script_value::make_object(dynamic_def->get_name(), instance, this);
            }
        }
    }

    try {
        std::shared_ptr<detail::first_type_t<class_definition, T>> class_def = get_class_definition_by_type(std::type_index(typeid(T)));
        if (class_def) {
            const std::string& type_name = class_def->get_name();
            auto instance = detail::make_native_instance(*class_def, type_name, std::static_pointer_cast<void>(data), this);
            if (data && !key_held_by_another_object) { note_script_layer(key, instance); }

            return script_value::make_object(type_name, instance, this);
        }
    } catch (...) {
        // Class not registered, fall back to raw object storage
    }

    // Fallback: store as raw C++ object (properties won't work)
    std::string type_name = get_registered_name<T>();
    auto type_id = symbolize(type_name);
    return script_value::make_cpp_object(type_name, type_id, std::static_pointer_cast<void>(data), this);
}

template<typename T>
script_value engine::extend(const std::shared_ptr<T>& object, std::string_view script_class, const std::string& fields_json) {
    return extend(object ? make_value_object(object) : script_value(std::monostate{}, this), script_value(std::string(script_class), this), fields_json);
}

template<typename T>
strong_ptr<class_instance> engine::script_layer(const T& object) const {
    if constexpr (std::is_base_of_v<script_extensible, T>) {
        auto layer = object.jai_script_slot().script_layer();
        return (layer && layer->get_engine() == this) ? layer : nullptr;
    } else if constexpr (std::is_polymorphic_v<T>) {
        return script_layer_of(dynamic_cast<const void*>(&object));
    } else {
        return script_layer_of(&object);
    }
}

template<typename T>
std::string engine::script_layer_detach(const std::shared_ptr<T>& object) {
    if (!object) { return {}; }
    auto layer = script_layer(*object);
    return layer ? detach_object(layer) : std::string();
}

template<typename T>
script_value convert_custom_type_with_registry(const T& t, engine* eng)
    requires std::is_copy_constructible_v<std::remove_const_t<T>>
{
    using NonConstT = std::remove_const_t<T>;
    // Terminal funnel for every generic to-script path: a shared_ptr must share its
    // pointee's identity, never become an object of a class named "std::shared_ptr<U>"
    // (non-const lvalues reach here because the universal-reference make_value beats
    // the const shared_ptr<U>& overload).
    if constexpr (math_adapted<NonConstT>) {
        using adapter = math_adapter<NonConstT>;
        const typename adapter::native_type native = adapter::to_native(t);
        uint32_t words[4] = {0, 0, 0, 0};
        std::memcpy(words, &native, sizeof(native));
        return script_value::make_vec(adapter::kind, words, eng);
    } else if constexpr (is_specialization_v<NonConstT, std::shared_ptr>) {
        if (!t) {
            return script_value(std::monostate{}, eng);
        }
        return eng->make_handle(t);
    } else if constexpr (is_specialization_v<NonConstT, std::weak_ptr>) {
        // a script weak_ptr watches a script handle, and a host weak_ptr has none to watch
        throw runtime_error("A host function returning std::weak_ptr<T> is not supported; return shared_ptr<T>");
    } else {
        auto registry = detail::engine_access::get_conversion_registry(*eng);
        if (registry && registry->template has_conversion<NonConstT>()) {
            return registry->template convert_to_script<NonConstT>(t);
        }

        auto sharedObj = std::make_shared<NonConstT>(t);
        if (!eng) {
            throw runtime_error("Engine reference required for custom type conversion");
        }
        return detail::engine_access::make_value_object(*eng, sharedObj);
    }
}

template<typename T>
script_value convert_custom_type_with_registry(const T& t, engine* eng)
    requires (!std::is_copy_constructible_v<std::remove_const_t<T>>)
{
    using NonConstT = std::remove_const_t<T>;
    auto registry = detail::engine_access::get_conversion_registry(*eng);
    if (registry && registry->template has_conversion<NonConstT>()) {
        return registry->template convert_to_script<NonConstT>(t);
    }

    throw runtime_error("Cannot convert non-copyable type by value. "
                       "Register the type with dynamic_binder or return shared_ptr<T>.");
}

template<typename T>
std::optional<script_value> try_convert_shared_ptr_with_registry(const std::shared_ptr<T>& ptr, engine* eng) {
    if (eng && eng->template has_registered_class<T>()) {
        return detail::engine_access::make_handle(*eng, ptr);
    }
    return std::nullopt;
}

template<typename T>
script_value convert_reference_with_registry(T& t, engine* eng) {
    using NonConstT = std::remove_const_t<T>;

    // Smart-pointer twin of convert_custom_type_with_registry's routing: share the
    // pointee, never bind or wrap the handle itself.
    if constexpr (is_specialization_v<NonConstT, std::shared_ptr>) {
        if (!t) {
            return script_value(std::monostate{}, eng);
        }
        return detail::engine_access::make_handle(*eng, t);
    } else {
        if (eng && eng->has_registered_class<NonConstT>()) {
            return script_value::make_cpp_bound(&t, eng);
        }

        if constexpr (std::is_copy_constructible_v<NonConstT>) {
            return convert_custom_type_with_registry<T>(t, eng);
        } else {
            throw runtime_error("Cannot convert non-copyable type by reference. "
                               "Register the type with dynamic_binder first.");
        }
    }
}

template<typename T>
requires (!std::is_arithmetic_v<std::remove_cvref_t<T>> &&
          !std::is_same_v<std::remove_cvref_t<T>, script_string> &&
          !std::is_same_v<std::remove_cvref_t<T>, std::string> &&
          !std::is_convertible_v<T, const char*> &&
          !std::is_same_v<std::remove_cvref_t<T>, std::monostate> &&
          std::is_lvalue_reference_v<T>)
script_value engine::make_value(T&& value) {
    return convert_reference_with_registry(value, this);
}

template<typename T>
requires (!std::is_arithmetic_v<std::remove_cvref_t<T>> &&
          !std::is_same_v<std::remove_cvref_t<T>, script_string> &&
          !std::is_same_v<std::remove_cvref_t<T>, std::string> &&
          !std::is_convertible_v<T, const char*> &&
          !std::is_same_v<std::remove_cvref_t<T>, std::monostate> &&
          !std::is_pointer_v<std::remove_cvref_t<T>> &&
          std::is_rvalue_reference_v<T&&> &&
          !std::is_lvalue_reference_v<T>)
script_value engine::make_value(T&& value) {
    return convert_custom_type_with_registry(value, this);
}

template<typename T>
script_value conversions::convert_vector_to_script_array(const std::vector<T>& vec, engine* eng) {
    if constexpr (std::is_same_v<T, uint8_t>) {
        // a host byte buffer is an array<uint8>: one copy
        script_value bytes = script_value::make_array(detail::engine_access::get_type_info_uint8(*eng), eng);
        bytes.unchecked_get_array_storage()->bytes().assign(vec.begin(), vec.end());
        return bytes;
    }
    type_info_ptr element_type;
    if constexpr (std::is_arithmetic_v<T> ||
                   std::is_same_v<T, std::string> || detail::is_script_value_v<T> ||
                   is_specialization_v<T, std::shared_ptr> ||
                   is_specialization_v<T, std::vector> || is_script_map_like_v<T> ||
                   is_specialization_v<T, std::unordered_map>) {
        // nested containers convert to arrays/maps, never to objects: the element type
        // stays dynamic (a class-typed element type would mint an object buffer)
        element_type = nullptr;
    } else {
        element_type = detail::engine_access::get_type_info_for_cpp_type<T>(*eng);
    }

    auto script_array = script_value::make_array(element_type, eng);
    auto& arr = *script_array.unchecked_get_array_storage();   // typed element types mint buffer nodes: push through the node

    for (const auto& item : vec) {
        if constexpr (detail::is_script_value_v<T>) {
            arr.push(item);
        } else if constexpr (std::is_same_v<T, int> || std::is_same_v<T, int64_t> ||
                           std::is_same_v<T, float> || std::is_same_v<T, double> ||
                           std::is_same_v<T, bool> || std::is_same_v<T, char> ||
                           std::is_same_v<T, std::string>) {
            arr.push(script_value(item, eng));
        } else if constexpr (std::is_integral_v<T>) {
            arr.push(script_value(static_cast<script_int>(item), eng));   // every other integer width is a script int, like int
        } else if constexpr (std::is_floating_point_v<T>) {
            arr.push(script_value(static_cast<script_float>(item), eng));
        } else if constexpr (is_specialization_v<T, std::shared_ptr>) {
            // The element IS a handle: share identity via make_object<U>. Wrapping the
            // handle in another shared_ptr registered as "std::shared_ptr<U>" throws.
            arr.push(eng->make_value(item));
        } else if constexpr (math_adapted<T>) {
            arr.push(convert_custom_type_with_registry(item, eng));   // the vector buffer of the adapter's kind
        } else {
            if (eng) {
                auto registry = detail::engine_access::get_conversion_registry(*eng);
                if (registry && registry->template has_conversion<T>()) {
                    arr.push(registry->template convert_to_script<T>(item));
                    continue;
                }
            }
            auto sharedObj = std::make_shared<T>(item);
            arr.push(detail::engine_access::make_value_object(*eng, sharedObj));
        }
    }

    return script_array;
}

template<typename K, typename V>
script_value conversions::convert_stdmap_to_script_map(const std::map<K, V>& stdmap, engine* eng) {
    type_info_ptr key_type;
    type_info_ptr value_type;

    if constexpr (std::is_same_v<K, int> || std::is_same_v<K, int64_t> ||
                   std::is_same_v<K, float> || std::is_same_v<K, double> ||
                   std::is_same_v<K, bool> || std::is_same_v<K, char> ||
                   std::is_same_v<K, std::string> || detail::is_script_value_v<K>) {
        key_type = nullptr;
    } else {
        key_type = eng->get_type_info_for_cpp_type<K>();
    }

    if constexpr (std::is_same_v<V, int> || std::is_same_v<V, int64_t> ||
                   std::is_same_v<V, float> || std::is_same_v<V, double> ||
                   std::is_same_v<V, bool> || std::is_same_v<V, char> ||
                   std::is_same_v<V, std::string> || detail::is_script_value_v<V>) {
        value_type = nullptr;
    } else {
        value_type = detail::engine_access::get_type_info_for_cpp_type<V>(*eng);
    }

    auto map_value = script_value::make_map(key_type, value_type, eng);
    auto& map = const_cast<script_map&>(map_value.as_map());

    for (const auto& [key, value] : stdmap) {
        script_value converted_key = script_value(std::monostate{}, eng);
        script_value converted_value = script_value(std::monostate{}, eng);

        if constexpr (detail::is_script_value_v<K>) {
            converted_key = key;
        } else if constexpr (std::is_same_v<K, int> || std::is_same_v<K, int64_t> ||
                           std::is_same_v<K, float> || std::is_same_v<K, double> ||
                           std::is_same_v<K, bool> || std::is_same_v<K, char> ||
                           std::is_same_v<K, std::string>) {
            converted_key = script_value(key, eng);
        } else if constexpr (is_specialization_v<K, std::shared_ptr>) {
            converted_key = eng->make_value(key);
        } else {
            if (eng) {
                auto registry = detail::engine_access::get_conversion_registry(*eng);
                if (registry && registry->template has_conversion<K>()) {
                    converted_key = registry->template convert_to_script<K>(key);
                } else {
                    auto sharedObj = std::make_shared<K>(key);
                    converted_key = eng->make_value_object(sharedObj);
                }
            } else {
                auto sharedObj = std::make_shared<K>(key);
                converted_key = eng->make_value_object(sharedObj);
            }
        }

        if constexpr (detail::is_script_value_v<V>) {
            converted_value = value;
        } else if constexpr (std::is_same_v<V, int> || std::is_same_v<V, int64_t> ||
                           std::is_same_v<V, float> || std::is_same_v<V, double> ||
                           std::is_same_v<V, bool> || std::is_same_v<V, char> ||
                           std::is_same_v<V, std::string>) {
            converted_value = script_value(value, eng);
        } else if constexpr (is_specialization_v<V, std::shared_ptr>) {
            converted_value = eng->make_value(value);
        } else {
            if (eng) {
                auto registry = detail::engine_access::get_conversion_registry(*eng);
                if (registry && registry->template has_conversion<V>()) {
                    converted_value = registry->template convert_to_script<V>(value);
                } else {
                    auto sharedObj = std::make_shared<V>(value);
                    converted_value = detail::engine_access::make_value_object(*eng, sharedObj);
                }
            } else {
                auto sharedObj = std::make_shared<V>(value);
                converted_value = detail::engine_access::make_value_object(*eng, sharedObj);
            }
        }

        map.insert_or_assign(converted_key, converted_value);
    }

    return map_value;
}

template<typename T>
void engine::add_custom_conversion(
    std::function<T(const script_value&)> from_func,
    std::function<script_value(const T&)> to_func
) {
    conversions::conversion_manager conv_manager = get_conversion_manager();
    conv_manager.add_custom_conversion<T>(from_func, to_func);
}

inline detail::parameter_storage::scope_guard::scope_guard(engine* eng, parameter_storage* storage)
    : engine_(eng), previous_(eng ? detail::engine_access::get_current_parameter_storage(*eng) : nullptr) {
    if (eng) {
        detail::engine_access::set_current_parameter_storage(*eng, storage);
    }
}

inline detail::parameter_storage::scope_guard::~scope_guard() {
    if (engine_) {
        detail::engine_access::set_current_parameter_storage(*engine_, previous_);
    }
}

} // namespace jai

#endif // __JAISCRIPT_CORE_ENGINE_IMPL_HPP__
