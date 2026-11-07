#pragma once

#include "conversion_registry.hpp"
#include "value.hpp"
#include "bound_array.hpp"
#include "bound_map.hpp"
#include "engine_impl.hpp"
#include <optional>

namespace jai {
namespace conversions {

// Template implementations that require full script_value definition

template<typename T>
struct function_conversion {
    std::function<T(const script_value&)> from;
    std::function<script_value(const T&)> to;

    static void from_script(const conversion_entry& entry, const script_value& v, void* out) {
        static_cast<std::optional<T>*>(out)->emplace(static_cast<const function_conversion*>(entry.state.get())->from(v));
    }
    static script_value to_script(const conversion_entry& entry, const void* value) {
        return static_cast<const function_conversion*>(entry.state.get())->to(*static_cast<const T*>(value));
    }
};

template<typename T>
inline void conversion_registry::register_conversion(
    std::function<T(const script_value&)> from_func,
    std::function<script_value(const T&)> to_func
) {
    register_entry(type_id::of<T>(), {&function_conversion<T>::from_script, &function_conversion<T>::to_script, engine_ref_,
        std::make_shared<function_conversion<T>>(function_conversion<T>{std::move(from_func), std::move(to_func)})});
}

template<typename T>
struct vector_conversion {
    static void from_script(const conversion_entry& entry, const script_value& v, void* out) {
        static_cast<std::optional<std::vector<T>>*>(out)->emplace(convert_script_array_to_vector<T>(v, entry.eng));
    }
    static script_value to_script(const conversion_entry& entry, const void* value) {
        return convert_vector_to_script_array(*static_cast<const std::vector<T>*>(value), entry.eng);
    }
};

template<typename K, typename V>
struct map_conversion {
    static void from_script(const conversion_entry& entry, const script_value& v, void* out) {
        static_cast<std::optional<std::map<K, V>>*>(out)->emplace(convert_script_map_to_stdmap<K, V>(v, entry.eng));
    }
    static script_value to_script(const conversion_entry& entry, const void* value) {
        return convert_stdmap_to_script_map(*static_cast<const std::map<K, V>*>(value), entry.eng);
    }
};

template<typename T>
struct standard_shape {
    static constexpr bool value = false;
};
template<typename E>
struct standard_shape<std::vector<E>> : vector_conversion<E> {
    static constexpr bool value = true;
};
template<typename K, typename V>
struct standard_shape<std::map<K, V>> : map_conversion<K, V> {
    static constexpr bool value = true;
};

template<typename T>
inline T conversion_registry::convert_from_script(const script_value& v) const {
    const conversion_entry* entry = find_entry(type_id::of<T>());
    if (!entry) {
        throw_unregistered_conversion("No conversion registered in convert_from_script for type: ", typeid(T).name());
    }
    std::optional<T> result;
    if constexpr (standard_shape<T>::value) {
        if (!entry->from_script) {
            standard_shape<T>::from_script(*entry, v, &result);
        } else {
            entry->from_script(*entry, v, &result);
        }
    } else {
        entry->from_script(*entry, v, &result);
    }
    if constexpr (std::is_move_constructible_v<T>) {
        return std::move(*result);
    } else {
        return *result;
    }
}

template<typename T>
inline script_value conversion_registry::convert_to_script(const T& value) const {
    const conversion_entry* entry = find_entry(type_id::of<T>());
    if (!entry) {
        throw_unregistered_conversion("No conversion registered for type: ", typeid(T).name());
    }
    if constexpr (standard_shape<T>::value) {
        if (!entry->to_script) {
            return standard_shape<T>::to_script(*entry, &value);
        }
    }
    return entry->to_script(*entry, &value);
}

// Template implementations for conversion_manager methods

template<typename T>
inline void conversion_manager::add_vector_conversion() {
    registry_->register_entry(type_id::of<std::vector<T>>(), {nullptr, nullptr, engine_, nullptr});
}

template<typename K, typename V>
inline void conversion_manager::add_map_conversion() {
    registry_->register_entry(type_id::of<std::map<K, V>>(), {nullptr, nullptr, engine_, nullptr});
}

// Implementations for bound_array and bound_map conversions

template<typename T>
inline void conversion_manager::add_bound_array_conversion() {
    // DO NOT register conversions for bound_array in the conversion registry!
    // The function_binder has specialized handling for bound_array that provides
    // proper zero-copy semantics for const& parameters while ensuring safety.
    // Registering conversions here would override those specializations and
    // cause either performance issues (deep copies) or crashes (dangling references).
    
    // The function_binder handles:
    // - bound_array<T> by value: deep copy via script_value constructor
    // - bound_array<T>&: stack-based object via create_argument() with zero-copy
    // - const bound_array<T>&: stack-based object via create_argument() with zero-copy
}

template<typename K, typename V>
inline void conversion_manager::add_bound_map_conversion() {
    // DO NOT register conversions for bound_map in the conversion registry!
    // The function_binder has specialized handling for bound_map that provides
    // proper zero-copy semantics for const& parameters while ensuring safety.
    // Registering conversions here would override those specializations and
    // cause either performance issues (deep copies) or crashes (dangling references).
    
    // The function_binder handles:
    // - bound_map<K,V> by value: deep copy via script_value constructor
    // - bound_map<K,V>&: stack-based object via create_argument() with zero-copy
    // - const bound_map<K,V>&: stack-based object via create_argument() with zero-copy
}

template<typename T>
inline void conversion_manager::add_custom_conversion(
    std::function<T(const script_value&)> from_func,
    std::function<script_value(const T&)> to_func
) {
    registry_->register_conversion<T>(from_func, to_func);
}

} // namespace conversions
} // namespace jai