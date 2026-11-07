// Part of the engine's closure, which includes it in its order: a translation unit that names it first is sent there
#if !defined(__JAISCRIPT_CORE_ENGINE_HPP__)
#include "engine.hpp"
#elif !defined(__JAISCRIPT_CORE_CONVERSION_REGISTRY_IMPL_HPP__)
#define __JAISCRIPT_CORE_CONVERSION_REGISTRY_IMPL_HPP__

#include "conversion_registry.hpp"
#include "value.hpp"
#include "native_cast.hpp"

namespace jai {
    class engine;

    // Must be defined after engine.hpp is included
    std::shared_ptr<conversions::conversion_registry> get_engine_conversion_registry(engine* eng);
    // engine.cpp: the class type_info registered for a C++ type (null when unbound), kept out of this header's type set
    type_info* get_engine_class_type_info_for(engine* eng, const std::type_info& cpp_type);
    namespace detail { bool class_derives_from(engine* eng, uint64_t derived_id, uint64_t base_id); }

namespace conversions {

template<typename T>
std::vector<T> convert_script_array_to_vector(const script_value& v, engine* eng) {
    if (v.type() != script_value_type::jai_array_type) {
        throw runtime_error("Cannot convert non-array to vector");
    }
    
    std::vector<T> result;
    const script_array* node = v.unchecked_array_node();
    if (!node) {
        throw runtime_error("Cannot convert non-array to vector");
    }
    result.reserve(node->size());

    // The registry is probed once per array, not once per element
    std::shared_ptr<conversions::conversion_registry> registry;
    if constexpr (!detail::is_script_value_v<T>) {
        if (eng) {
            registry = get_engine_conversion_registry(eng);
            if (registry && !registry->template has_conversion<T>()) { registry.reset(); }
        }
    }
	// Prove the declared array class once, but cast each holder's ACTUAL native class:
	// array<Base> may contain multiple subclasses with different C++ base offsets.
	// Exact-class slots remain lookup-free; no boxed element or copy-out is needed.
    if constexpr (is_specialization_v<T, std::shared_ptr>) {
        using pointee = typename T::element_type;
        if constexpr (!std::is_void_v<pointee> && std::is_class_v<pointee>) {
            if (registry && node->kind() == script_array::kind_t::obj) {
                type_info* bound_class = get_engine_class_type_info_for(eng, typeid(pointee));
                type_info_ptr array_tag = v.deref().get_type_info();
                type_info_ptr slot_class = array_tag ? array_tag->element_type() : nullptr;
                if (bound_class && slot_class && detail::class_derives_from(eng, slot_class->id, bound_class->id)) {
                    for (const auto& slot : node->objs()) {
                        const auto& holder = slot.handle;
                        if (!holder) { result.push_back(nullptr); continue; }
						result.push_back(std::static_pointer_cast<pointee>(detail::native_cast_or_throw(
							detail::checked_native_object(eng, *holder, typeid(pointee), bound_class->id), eng)));
                    }
                    return result;
                }
            }
        }
    }
    // Exact-width payloads leave a typed buffer without boxing; narrower targets keep
    // as<T>()'s per-element range checks
    if constexpr (std::is_same_v<T, script_int>) {
        if (!registry && node->kind() == script_array::kind_t::i64) {
            result.insert(result.end(), node->ints().begin(), node->ints().end());
            return result;
        }
    } else if constexpr (std::is_same_v<T, script_float>) {
        if (!registry && node->kind() == script_array::kind_t::f64) {
            result.insert(result.end(), node->floats().begin(), node->floats().end());
            return result;
        }
    } else if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, int8_t>) {
        if (!registry && node->kind() == (std::is_same_v<T, uint8_t> ? script_array::kind_t::u8 : script_array::kind_t::i8)) {
            result.resize(node->bytes().size());
            if (!result.empty()) { std::memcpy(result.data(), node->bytes().data(), result.size()); }
            return result;
        }
    }
    node->for_each(eng, [&](const script_value& elem) {
        if constexpr (detail::is_script_value_v<T>) {
            result.push_back(elem);
        } else {
            if (registry) {
                try {
                    result.push_back(registry->template convert_from_script<T>(elem));
                    return;
                } catch (...) {
                    // Fall through to direct conversion on any error
                }
            }
            result.push_back(elem.as<T>());
        }
    });
    return result;
}

// Implementation in engine_impl.hpp to avoid circular dependencies
template<typename T>
script_value convert_vector_to_script_array(const std::vector<T>& vec, engine* eng);

template<typename K, typename V>
std::map<K, V> convert_script_map_to_stdmap(const script_value& v, engine* eng) {
    if (v.type() != script_value_type::jai_map_type) {
        throw runtime_error("Cannot convert non-map to std::map");
    }

    std::map<K, V> result;
    const auto& m = v.as_map();

    // Construct entries in place: operator[] would require V to be default-constructible AND
    // copy-assignable, and the assignability trait lies for types holding vector<unique_ptr<...>>
    // members (hard error inside vector::operator=). Copy construction is all we require.
    auto registry = eng ? get_engine_conversion_registry(eng) : nullptr;
    auto converted_key = [&](const script_value& k) -> K {
        if constexpr (detail::is_script_value_v<K>) {
            return k;
        } else {
            if (registry && registry->template has_conversion<K>()) {
                return registry->template convert_from_script<K>(k);
            }
            return k.as<K>();
        }
    };
    auto converted_value = [&](const script_value& val) -> V {
        if constexpr (detail::is_script_value_v<V>) {
            return val;
        } else {
            if (registry && registry->template has_conversion<V>()) {
                return registry->template convert_from_script<V>(val);
            }
            return val.as<V>();
        }
    };
    for (const auto& [k, val] : m) {
        K key = converted_key(k);
        result.erase(key); // last-wins if two script keys convert to the same K
        result.emplace(std::move(key), converted_value(val));
    }
    return result;
}

// Implementation in engine_impl.hpp to avoid circular dependencies
template<typename K, typename V>
script_value convert_stdmap_to_script_map(const std::map<K, V>& stdmap, engine* eng);

} // namespace conversions
} // namespace jai

#endif // __JAISCRIPT_CORE_CONVERSION_REGISTRY_IMPL_HPP__
