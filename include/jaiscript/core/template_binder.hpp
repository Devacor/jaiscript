#pragma once

#ifndef __JAISCRIPT_CORE_TEMPLATE_BINDER_HPP__
#define __JAISCRIPT_CORE_TEMPLATE_BINDER_HPP__

#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/static_binder.hpp>
#include <jaiscript/core/type_info.hpp>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace jai {

// Specialized by JAI_TEMPLATE_BINDER to provide type_name() and make_binder()
template<template<typename> class Template, typename T>
struct template_binder_traits;

template<template<typename, typename> class Template, typename T1, typename T2>
struct template_binder_traits_2;

template<template<typename, typename, typename> class Template, typename T1, typename T2, typename T3>
struct template_binder_traits_3;

template<typename T, typename = void>
struct has_template_binder : std::false_type {};

template<template<typename> class Template, typename Arg>
struct has_template_binder<Template<Arg>,
    std::void_t<decltype(template_binder_traits<Template, Arg>::type_name())>
> : std::true_type {};

template<typename T>
inline constexpr bool has_template_binder_v = has_template_binder<T>::value;

template<typename T, typename = void>
struct has_template_binder_2 : std::false_type {};

template<template<typename, typename> class Template, typename Arg1, typename Arg2>
struct has_template_binder_2<Template<Arg1, Arg2>,
    std::void_t<decltype(template_binder_traits_2<Template, Arg1, Arg2>::type_name())>
> : std::true_type {};

template<typename T>
inline constexpr bool has_template_binder_2_v = has_template_binder_2<T>::value;

template<typename T>
inline constexpr bool has_any_template_binder_v =
    has_template_binder_v<T> || has_template_binder_2_v<T>;

template<typename T, typename = void>
struct template_binder_accessor;

template<template<typename> class Template, typename Arg>
struct template_binder_accessor<Template<Arg>,
    std::enable_if_t<has_template_binder_v<Template<Arg>>>
> {
    using traits = template_binder_traits<Template, Arg>;

    static std::string type_name() { return traits::type_name(); }
    static auto make_binder() { return traits::make_binder(); }

    template<typename Archive>
    static void save(Archive& ar, const Template<Arg>& obj) {
        make_binder().save_all(ar, obj);
    }

    template<typename Archive>
    static void load(Archive& ar, Template<Arg>& obj) {
        make_binder().load_all(ar, obj);
    }

    static void bind_to(engine& eng) {
        auto binder = make_binder();
        auto name = type_name();
        binder.bind_to(eng, name.c_str());
    }
};

template<template<typename, typename> class Template, typename Arg1, typename Arg2>
struct template_binder_accessor<Template<Arg1, Arg2>,
    std::enable_if_t<has_template_binder_2_v<Template<Arg1, Arg2>>>
> {
    using traits = template_binder_traits_2<Template, Arg1, Arg2>;

    static std::string type_name() { return traits::type_name(); }
    static auto make_binder() { return traits::make_binder(); }

    template<typename Archive>
    static void save(Archive& ar, const Template<Arg1, Arg2>& obj) {
        make_binder().save_all(ar, obj);
    }

    template<typename Archive>
    static void load(Archive& ar, Template<Arg1, Arg2>& obj) {
        make_binder().load_all(ar, obj);
    }

    static void bind_to(engine& eng) {
        auto binder = make_binder();
        auto name = type_name();
        binder.bind_to(eng, name.c_str());
    }
};

namespace detail {
    template<typename> inline constexpr bool no_script_spelling_v = false;
    template<typename> struct std_vector_of { using type = void; };
    template<typename E, typename A> struct std_vector_of<std::vector<E, A>> { using type = E; };
    template<typename> struct std_map_of { using key = void; using value = void; };
    template<typename K, typename V, typename C, typename A> struct std_map_of<std::map<K, V, C, A>> { using key = K; using value = V; };
    template<typename K, typename V, typename H, typename E, typename A> struct std_map_of<std::unordered_map<K, V, H, E, A>> { using key = K; using value = V; };
    template<typename> struct std_handle_of { using type = void; static constexpr const char* name = nullptr; };
    template<typename E> struct std_handle_of<std::shared_ptr<E>> { using type = E; static constexpr const char* name = "shared_ptr"; };
    template<typename E> struct std_handle_of<std::weak_ptr<E>> { using type = E; static constexpr const char* name = "weak_ptr"; };
    template<typename U> inline constexpr bool is_wide_character_v =
        std::is_same_v<U, wchar_t> || std::is_same_v<U, char16_t> || std::is_same_v<U, char32_t>
#if defined(__cpp_char8_t)
        || std::is_same_v<U, char8_t>
#endif
        ;
}

// The script spelling of a C++ type as a template argument: the canonical name the parser gives the type a script writes
// for it (type_info::canonical_name), so a host instantiation and its script spelling register one class. Script int and
// float are 64-bit, so int64_t spells "int" and double "float"; a narrower width carries its size (int32, uint8, float32).
// A type with no script spelling is a compile error: specialize jai::type_name_helper<T> to name it.
template<typename T, typename = void>
struct type_name_helper {
    static std::string name() {
        using U = std::remove_cv_t<T>;
        if constexpr (std::is_same_v<U, bool>) {
            return "bool";
        } else if constexpr (std::is_same_v<U, char>) {
            return "char";
        } else if constexpr (std::is_integral_v<U> && !detail::is_wide_character_v<U>) {
            static_assert(sizeof(U) == 1 || sizeof(U) == 2 || sizeof(U) == 4 || sizeof(U) == 8,
                "jai::type_name_helper: an integer template argument must be 8, 16, 32 or 64 bits wide");
            const char* width = sizeof(U) == 1 ? "8" : sizeof(U) == 2 ? "16" : sizeof(U) == 4 ? "32" : "";
            return std::string(std::is_signed_v<U> ? "int" : "uint") + width;
        } else if constexpr (std::is_same_v<U, double>) {
            return "float";
        } else if constexpr (std::is_same_v<U, float>) {
            return "float32";
        } else if constexpr (std::is_same_v<U, std::string>) {
            return "string";
        } else if constexpr (math_adapted<U>) {
            return math_type_name(math_adapter<U>::kind);
        } else if constexpr (!std::is_void_v<typename detail::std_vector_of<U>::type>) {
            return "array<" + type_name_helper<typename detail::std_vector_of<U>::type>::name() + ">";
        } else if constexpr (!std::is_void_v<typename detail::std_map_of<U>::key>) {
            return "map<" + type_name_helper<typename detail::std_map_of<U>::key>::name() + "," +
                   type_name_helper<typename detail::std_map_of<U>::value>::name() + ">";
        } else if constexpr (detail::std_handle_of<U>::name != nullptr) {
            return std::string(detail::std_handle_of<U>::name) + "<" + type_name_helper<typename detail::std_handle_of<U>::type>::name() + ">";
        } else if constexpr (has_static_type_v<U>) {
            return std::string(jai_static_type<U>::name);
        } else if constexpr (has_any_template_binder_v<U>) {
            return template_binder_accessor<U>::type_name();
        } else {
            static_assert(detail::no_script_spelling_v<T>,
                "jai::type_name_helper: this template argument has no script spelling. Bind it with JAI_STATIC_BINDER or "
                "JAI_TEMPLATE_BINDER, or specialize jai::type_name_helper<T> with the name scripts write for it.");
            return {};
        }
    }
};

// Whether T is declared for binding (a JAI_BIND_TEMPLATE instantiation, or a template deriving auto_bind_template):
// compile-time facts, no state. What an engine has bound is engine state: is_registered<T>(eng).
template<template<typename> class Template, typename T>
struct auto_bind_template {};

template<template<typename, typename> class Template, typename T1, typename T2>
struct auto_bind_template_2 {};

template<typename T>
struct is_auto_bind_template : std::false_type {};

template<template<typename> class Template, typename Arg>
struct is_auto_bind_template<Template<Arg>> : std::is_base_of<auto_bind_template<Template, Arg>, Template<Arg>> {};

template<template<typename, typename> class Template, typename Arg1, typename Arg2>
struct is_auto_bind_template<Template<Arg1, Arg2>> : std::is_base_of<auto_bind_template_2<Template, Arg1, Arg2>, Template<Arg1, Arg2>> {};

struct static_type_registry {
    template<typename T>
    static constexpr bool is_registered() {
        return has_static_type_v<T> || is_auto_bind_template<T>::value;
    }

    template<typename T, typename Engine>
    static bool is_registered(const Engine& eng) {
        return eng.template has_registered_class<T>();
    }
};

// Binding one width of a numeric template argument also binds its native-width sibling, so a script reaches the
// instantiation through either spelling: Point<int32> with Point<int>, Point<uint32> with Point<uint>, Point<float32>
// with Point<float>. Called from engine::bind_static_type() after binding the primary type.
template<typename A> struct native_width_sibling { using type = void; };
template<> struct native_width_sibling<int32_t> { using type = int64_t; };
template<> struct native_width_sibling<int64_t> { using type = int32_t; };
template<> struct native_width_sibling<uint32_t> { using type = uint64_t; };
template<> struct native_width_sibling<uint64_t> { using type = uint32_t; };
template<> struct native_width_sibling<float> { using type = double; };
template<> struct native_width_sibling<double> { using type = float; };

template<typename T>
struct width_variant_binder {
    static void bind(engine&) {}
};

template<template<typename> class Template, typename Arg>
struct width_variant_binder<Template<Arg>> {
    static void bind(engine& eng) {
        using sibling = typename native_width_sibling<Arg>::type;
        if constexpr (!std::is_void_v<sibling>) {
            if constexpr (has_template_binder_v<Template<sibling>>) {
                if (!eng.is_type_registered(template_binder_accessor<Template<sibling>>::type_name())) {
                    template_binder_accessor<Template<sibling>>::bind_to(eng);
                }
            }
        }
    }
};

// Simple version (3 params) - TemplateClass used as binder name:
//   JAI_TEMPLATE_BINDER(Point, "Point",
//       .property("x", &Point<T>::x)
//       .property("y", &Point<T>::y)
//   );
//
// Named version (4 params) - for namespaced types:
//   JAI_TEMPLATE_BINDER_NAMED(MyPoint, MyNamespace::Point, "Point",
//       .property("x", &MyNamespace::Point<T>::x)
//   );

#define JAI_TEMPLATE_BINDER_IMPL(BinderName, TemplateClass, BaseName, ...) \
    template<typename T> \
    struct jai_template_binder_##BinderName { \
        static constexpr const char* base_name = BaseName; \
        \
        static std::string type_name() { \
            return jai::template_instance_name(BaseName, {jai::type_name_helper<T>::name()}); \
        } \
        \
        static auto make_binder() { \
            return jai::static_binder<TemplateClass<T>>{} \
                __VA_ARGS__; \
        } \
    }; \
    \
    template<typename T> \
    struct jai::template_binder_traits<TemplateClass, T> { \
        static std::string type_name() { \
            return jai_template_binder_##BinderName<T>::type_name(); \
        } \
        static auto make_binder() { \
            return jai_template_binder_##BinderName<T>::make_binder(); \
        } \
    }

// Uses TemplateClass as the binder name (must be simple identifier, no namespaces)
#define JAI_TEMPLATE_BINDER(TemplateClass, BaseName, ...) \
    JAI_TEMPLATE_BINDER_IMPL(TemplateClass, TemplateClass, BaseName, __VA_ARGS__)

// For namespaced types where TemplateClass contains ::
#define JAI_TEMPLATE_BINDER_NAMED(BinderName, TemplateClass, BaseName, ...) \
    JAI_TEMPLATE_BINDER_IMPL(BinderName, TemplateClass, BaseName, __VA_ARGS__)

#define JAI_TEMPLATE_BINDER_2_IMPL(BinderName, TemplateClass, BaseName, ...) \
    template<typename T1, typename T2> \
    struct jai_template_binder_##BinderName { \
        static constexpr const char* base_name = BaseName; \
        \
        static std::string type_name() { \
            return jai::template_instance_name(BaseName, {jai::type_name_helper<T1>::name(), \
                                                          jai::type_name_helper<T2>::name()}); \
        } \
        \
        static auto make_binder() { \
            return jai::static_binder<TemplateClass<T1, T2>>{} \
                __VA_ARGS__; \
        } \
    }; \
    \
    template<typename T1, typename T2> \
    struct jai::template_binder_traits_2<TemplateClass, T1, T2> { \
        static std::string type_name() { \
            return jai_template_binder_##BinderName<T1, T2>::type_name(); \
        } \
        static auto make_binder() { \
            return jai_template_binder_##BinderName<T1, T2>::make_binder(); \
        } \
    }

#define JAI_TEMPLATE_BINDER_2(TemplateClass, BaseName, ...) \
    JAI_TEMPLATE_BINDER_2_IMPL(TemplateClass, TemplateClass, BaseName, __VA_ARGS__)

#define JAI_TEMPLATE_BINDER_2_NAMED(BinderName, TemplateClass, BaseName, ...) \
    JAI_TEMPLATE_BINDER_2_IMPL(BinderName, TemplateClass, BaseName, __VA_ARGS__)

// Simple version (2 params) - TemplateClass matches binder name:
//   JAI_BIND_TEMPLATE(Point, int);      // Point<int32> (C++ int is 32-bit; int64_t spells int)
//   JAI_BIND_TEMPLATE(Point, double);   // Point<float> (script float is 64-bit; C++ float spells float32)
//
// Named version (3 params) - for namespaced types:
//   JAI_BIND_TEMPLATE_NAMED(MyPoint, NS::Point, int);   // Creates NS::Point<int> binding

#define JAI_BIND_TEMPLATE_IMPL(BinderName, TemplateClass, TypeParam) \
    template<> struct jai::jai_static_type<TemplateClass<TypeParam>> { \
        static constexpr bool registered = true; \
        static inline const std::string name = \
            jai_template_binder_##BinderName<TypeParam>::type_name(); \
        static inline const auto binder = \
            jai_template_binder_##BinderName<TypeParam>::make_binder(); \
        \
        template<typename Archive> \
        static void save(Archive& ar, \
                         const TemplateClass<TypeParam>& obj) { \
            binder.save_all(ar, obj); \
        } \
        template<typename Archive> \
        static void load(Archive& ar, \
                         TemplateClass<TypeParam>& obj) { \
            binder.load_all(ar, obj); \
        } \
        static void bind_to(jai::engine& eng) { \
            binder.bind_to(eng, name.c_str()); \
        } \
    }

// Uses TemplateClass as the binder name (must match JAI_TEMPLATE_BINDER)
#define JAI_BIND_TEMPLATE(TemplateClass, TypeParam) \
    JAI_BIND_TEMPLATE_IMPL(TemplateClass, TemplateClass, TypeParam)

// For namespaced types where TemplateClass contains ::
#define JAI_BIND_TEMPLATE_NAMED(BinderName, TemplateClass, TypeParam) \
    JAI_BIND_TEMPLATE_IMPL(BinderName, TemplateClass, TypeParam)

#define JAI_BIND_TEMPLATE_2_IMPL(BinderName, TemplateClass, T1, T2) \
    template<> struct jai::jai_static_type<TemplateClass<T1, T2>> { \
        static constexpr bool registered = true; \
        static inline const std::string name = \
            jai_template_binder_##BinderName<T1, T2>::type_name(); \
        static inline const auto binder = \
            jai_template_binder_##BinderName<T1, T2>::make_binder(); \
        \
        template<typename Archive> \
        static void save(Archive& ar, \
                         const TemplateClass<T1, T2>& obj) { \
            binder.save_all(ar, obj); \
        } \
        template<typename Archive> \
        static void load(Archive& ar, \
                         TemplateClass<T1, T2>& obj) { \
            binder.load_all(ar, obj); \
        } \
        static void bind_to(jai::engine& eng) { \
            binder.bind_to(eng, name.c_str()); \
        } \
    }

#define JAI_BIND_TEMPLATE_2(TemplateClass, T1, T2) \
    JAI_BIND_TEMPLATE_2_IMPL(TemplateClass, TemplateClass, T1, T2)

#define JAI_BIND_TEMPLATE_2_NAMED(BinderName, TemplateClass, T1, T2) \
    JAI_BIND_TEMPLATE_2_IMPL(BinderName, TemplateClass, T1, T2)

} // namespace jai

#endif // __JAISCRIPT_CORE_TEMPLATE_BINDER_HPP__
