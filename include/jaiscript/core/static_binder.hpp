#pragma once

#ifndef __JAISCRIPT_CORE_STATIC_BINDER_HPP__
#define __JAISCRIPT_CORE_STATIC_BINDER_HPP__

#include <cstddef>
#include <tuple>
#include <type_traits>

namespace jai {
    class engine;
}

namespace jai {

template<typename T, typename P>
struct static_property {
    const char* name;
    P T::* member;

    constexpr static_property(const char* n, P T::* m) : name(n), member(m) {}

    template<typename Archive>
    void save(Archive& ar, const T& obj) const;
    template<typename Archive>
    void load(Archive& ar, T& obj) const;
};

template<typename T, typename MethodPtr>
struct static_method {
    const char* name;
    MethodPtr method;

    constexpr static_method(const char* n, MethodPtr m) : name(n), method(m) {}
};

// Each .property() or .method() call returns a new type with the info appended
template<typename T, typename Props = std::tuple<>, typename Methods = std::tuple<>>
struct static_binder {
    Props properties{};
    Methods methods{};

    constexpr static_binder() = default;
    constexpr static_binder(Props p, Methods m) : properties(p), methods(m) {}

    template<typename P>
    constexpr auto property(const char* name, P T::* member) const {
        auto new_prop = static_property<T, P>{name, member};
        auto new_props = std::tuple_cat(properties, std::make_tuple(new_prop));
        return static_binder<T, decltype(new_props), Methods>{new_props, methods};
    }

    template<typename MethodPtr>
    constexpr auto method(const char* name, MethodPtr m) const {
        auto new_method = static_method<T, MethodPtr>{name, m};
        auto new_methods = std::tuple_cat(methods, std::make_tuple(new_method));
        return static_binder<T, Props, decltype(new_methods)>{properties, new_methods};
    }

    static constexpr size_t property_count() {
        return std::tuple_size_v<Props>;
    }

    static constexpr size_t method_count() {
        return std::tuple_size_v<Methods>;
    }

    template<typename Archive>
    void save_all(Archive& ar, const T& obj) const {
        std::apply([&](const auto&... props) {
            (props.save(ar, obj), ...);
        }, properties);
    }

    template<typename Archive>
    void load_all(Archive& ar, T& obj) const {
        std::apply([&](const auto&... props) {
            (props.load(ar, obj), ...);
        }, properties);
    }

    // Implementation in static_binder_impl.hpp (needs full engine definition)
    void bind_to(engine& eng, const char* type_name) const;
};

template<typename T, typename = void>
struct jai_static_type {
    static constexpr bool registered = false;
};

template<typename T>
inline constexpr bool has_static_type_v = jai_static_type<T>::registered;

#define JAI_STATIC_BINDER(Type, Name, ...) \
    template<> struct jai::jai_static_type<Type> { \
        static constexpr bool registered = true; \
        static constexpr const char* name = Name; \
        static constexpr auto binder = jai::static_binder<Type>{} __VA_ARGS__; \
        \
        template<typename Archive> \
        static void save(Archive& ar, const Type& obj) { \
            binder.save_all(ar, obj); \
        } \
        template<typename Archive> \
        static void load(Archive& ar, Type& obj) { \
            binder.load_all(ar, obj); \
        } \
        static void bind_to(jai::engine& eng) { \
            binder.bind_to(eng, name); \
        } \
    }

// Convenience macro when type name matches C++ type (no namespaces)
#define JAI_STATIC_BINDER_AUTO(Type, ...) \
    JAI_STATIC_BINDER(Type, #Type, __VA_ARGS__)

} // namespace jai

#endif // __JAISCRIPT_CORE_STATIC_BINDER_HPP__
