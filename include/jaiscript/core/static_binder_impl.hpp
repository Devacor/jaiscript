#pragma once

#ifndef __JAISCRIPT_CORE_STATIC_BINDER_IMPL_HPP__
#define __JAISCRIPT_CORE_STATIC_BINDER_IMPL_HPP__

#include <jaiscript/core/static_binder.hpp>
#include <jaiscript/core/template_binder.hpp>
#include <jaiscript/serialization/archive_impl.hpp>
#include <jaiscript/properties/property_serialization.hpp>

namespace jai {

template<typename T, typename P>
template<typename Archive>
void static_property<T, P>::save(Archive& ar, const T& obj) const {
    ar.write_property_name(name);

    const P& value = obj.*member;

    if constexpr (property_serialization::is_direct_serializable_v<P>) {
        property_serialization::write_primitive_templated(ar, value);
    }
    else if constexpr (property_serialization::is_std_vector_v<P>) {
        ar.begin_array(value.size());
        for (const auto& elem : value) {
            ar(elem);
        }
        ar.end_array();
    }
    else if constexpr (property_serialization::is_std_map_v<P>) {
        ar.begin_map(value.size());
        for (const auto& [k, v] : value) {
            ar.write_map_key(k);
            ar(v);
        }
        ar.end_map();
    }
    else if constexpr (has_static_type_v<P>) {
        ar.begin_object();
        jai_static_type<P>::save(ar, value);
        ar.end_object();
    }
    else if constexpr (has_any_template_binder_v<P>) {
        ar.begin_object();
        template_binder_accessor<P>::save(ar, value);
        ar.end_object();
    }
    else {
        ar(value);
    }
}

template<typename T, typename P>
template<typename Archive>
void static_property<T, P>::load(Archive& ar, T& obj) const {
    // a missing key keeps the member's value, as a property_owner's load does
    if (!ar.seek_property(name)) {
        return;
    }

    P& value = obj.*member;

    if constexpr (property_serialization::is_direct_serializable_v<P>) {
        property_serialization::read_primitive_templated(ar, value);
    }
    else if constexpr (property_serialization::is_std_vector_v<P>) {
        using elem_t = typename P::value_type;
        size_t count = ar.begin_array();
        value.clear();
        serialization::reserve_for_load(value, count);
        for (size_t i = 0; i < count; ++i) {
            elem_t elem{};
            ar(elem);
            value.push_back(std::move(elem));
        }
        ar.end_array();
    }
    else if constexpr (property_serialization::is_std_map_v<P>) {
        static_assert(std::is_same_v<typename P::key_type, std::string>, "a JAI_STATIC_BINDER map property needs string keys");
        size_t count = ar.begin_map();
        value.clear();
        for (size_t i = 0; i < count; ++i) {
            std::string key;
            if (!ar.read_map_key(key)) break;
            typename P::mapped_type val{};
            ar(val);
            value[std::move(key)] = std::move(val);
        }
        ar.end_map();
    }
    else if constexpr (has_static_type_v<P>) {
        ar.begin_object();
        jai_static_type<P>::load(ar, value);
        ar.end_object();
    }
    else if constexpr (has_any_template_binder_v<P>) {
        ar.begin_object();
        template_binder_accessor<P>::load(ar, value);
        ar.end_object();
    }
    else {
        ar(value);
    }
}

} // namespace jai

// after the namespace close to avoid circular includes
#include <jaiscript/core/dynamic_binder.hpp>

namespace jai {

template<typename T, typename Props, typename Methods>
void static_binder<T, Props, Methods>::bind_to(engine& eng, const char* type_name) const {
    if (eng.is_type_registered(type_name)) {
        return;
    }

    dynamic_binder<T> builder(eng, type_name);

    std::apply([&](const auto&... props) {
        (builder.property(props.name, props.member), ...);
    }, properties);

    std::apply([&](const auto&... meths) {
        (builder.method(meths.name, meths.method), ...);
    }, methods);

    builder.build();
}

// Two C++ types the script spells alike (int and long on MSVC are both int32) cannot share one registered name
template<typename T>
void reject_static_type_name_collision(const engine& eng, const std::string& type_name) {
    if (!eng.has_registered_class<T>()) {
        throw runtime_error("'" + type_name + "' is already bound to another C++ type; give this instantiation its own script name");
    }
}

template<typename T>
void engine::bind_static_type() {
    if constexpr (has_static_type_v<T>) {
        if (is_type_registered(jai_static_type<T>::name)) {
            reject_static_type_name_collision<T>(*this, jai_static_type<T>::name);
            return;
        }
        jai_static_type<T>::bind_to(*this);
    }
    else if constexpr (has_any_template_binder_v<T>) {
        auto type_name = template_binder_accessor<T>::type_name();
        if (is_type_registered(type_name)) {
            reject_static_type_name_collision<T>(*this, type_name);
            return;
        }
        template_binder_accessor<T>::bind_to(*this);
        width_variant_binder<T>::bind(*this);
    }
    else {
        static_assert(has_static_type_v<T> || has_any_template_binder_v<T>,
            "Type is not registered with JAI_STATIC_BINDER or JAI_TEMPLATE_BINDER. "
            "Use JAI_STATIC_BINDER(Type, \"Name\", .property(...)) or "
            "JAI_TEMPLATE_BINDER(Template, \"Name\", ...) to register.");
    }
}

} // namespace jai

#endif // __JAISCRIPT_CORE_STATIC_BINDER_IMPL_HPP__
