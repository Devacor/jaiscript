#pragma once

#ifndef __JAISCRIPT_MATH_ADAPTER_HPP__
#define __JAISCRIPT_MATH_ADAPTER_HPP__

// Host math types as JaiScript's native math kinds (docs/site/embedding/15-math-adapters.html). A host type whose
// math_adapter specialization exists crosses the binding layer as a math value: a parameter or return of that
// type, a bound member of it, a std::vector of it and a signal argument all exchange a math::vec3 (or the kind the
// adapter names) and never a class instance. The specialization lives beside the host type, in the header that
// defines it, so every TU that binds the type sees it:
//
//     JAI_BIND_VEC3((game::Point<float>), x, y, z)
//     JAI_BIND_IVEC3((game::Point<int>), x, y, z)
//     JAI_BIND_COLOR((game::Color), R, G, B, A)
//
// The host type is parenthesized so template arguments with commas pass through. A specialization provides the
// native type, the script kind, and the conversions to and from the native value.

#include <jaiscript/jaiscript_fwd.hpp>
#include <jaiscript/math/types.hpp>
#include <type_traits>

namespace jai {

	template <class Host> struct math_adapter;   // no primary definition

	template <class Host>
	concept math_adapted = requires { typename math_adapter<std::remove_cv_t<std::remove_reference_t<Host>>>::native_type; };

	namespace detail {
		template <class Native, script_value_type Kind>
		struct native_math_adapter {
			using native_type = Native;
			static constexpr script_value_type kind = Kind;
			static native_type to_native(const Native& h) noexcept { return h; }
			static void assign(Native& h, const native_type& n) noexcept { h = n; }
			static Native from_native(const native_type& n) noexcept { return n; }
		};
	}

	template <> struct math_adapter<vec2> : detail::native_math_adapter<vec2, script_value_type::jai_vec2_type> {};
	template <> struct math_adapter<vec3> : detail::native_math_adapter<vec3, script_value_type::jai_vec3_type> {};
	template <> struct math_adapter<vec4> : detail::native_math_adapter<vec4, script_value_type::jai_vec4_type> {};
	template <> struct math_adapter<ivec2> : detail::native_math_adapter<ivec2, script_value_type::jai_ivec2_type> {};
	template <> struct math_adapter<ivec3> : detail::native_math_adapter<ivec3, script_value_type::jai_ivec3_type> {};
	template <> struct math_adapter<color> : detail::native_math_adapter<color, script_value_type::jai_color_type> {};

} // namespace jai

#define JAI_MATH_UNPAREN_IMPL(...) __VA_ARGS__
#define JAI_MATH_UNPAREN(x) JAI_MATH_UNPAREN_IMPL x

#define JAI_BIND_MATH_HEAD_(HOST, NATIVE, KIND)                                                                          \
	template <> struct jai::math_adapter<JAI_MATH_UNPAREN(HOST)> {                                                       \
		using host_type = JAI_MATH_UNPAREN(HOST);                                                                        \
		using native_type = NATIVE;                                                                                      \
		static constexpr ::jai::script_value_type kind = ::jai::script_value_type::KIND;                                \
		static host_type from_native(const native_type& n) { host_type h{}; assign(h, n); return h; }

#define JAI_BIND_MATH_S_(M, C) h.M = static_cast<std::remove_reference_t<decltype(h.M)>>(n.C);

#define JAI_BIND_VEC2(HOST, X, Y)                                                                                        \
	JAI_BIND_MATH_HEAD_(HOST, ::jai::vec2, jai_vec2_type)                                                                \
		static native_type to_native(const host_type& h) noexcept { return {static_cast<float>(h.X), static_cast<float>(h.Y)}; } \
		static void assign(host_type& h, const native_type& n) noexcept { JAI_BIND_MATH_S_(X, x) JAI_BIND_MATH_S_(Y, y) } \
	};
#define JAI_BIND_VEC3(HOST, X, Y, Z)                                                                                     \
	JAI_BIND_MATH_HEAD_(HOST, ::jai::vec3, jai_vec3_type)                                                                \
		static native_type to_native(const host_type& h) noexcept { return {static_cast<float>(h.X), static_cast<float>(h.Y), static_cast<float>(h.Z)}; } \
		static void assign(host_type& h, const native_type& n) noexcept { JAI_BIND_MATH_S_(X, x) JAI_BIND_MATH_S_(Y, y) JAI_BIND_MATH_S_(Z, z) } \
	};
#define JAI_BIND_VEC4(HOST, X, Y, Z, W)                                                                                  \
	JAI_BIND_MATH_HEAD_(HOST, ::jai::vec4, jai_vec4_type)                                                                \
		static native_type to_native(const host_type& h) noexcept { return {static_cast<float>(h.X), static_cast<float>(h.Y), static_cast<float>(h.Z), static_cast<float>(h.W)}; } \
		static void assign(host_type& h, const native_type& n) noexcept { JAI_BIND_MATH_S_(X, x) JAI_BIND_MATH_S_(Y, y) JAI_BIND_MATH_S_(Z, z) JAI_BIND_MATH_S_(W, w) } \
	};
#define JAI_BIND_IVEC2(HOST, X, Y)                                                                                       \
	JAI_BIND_MATH_HEAD_(HOST, ::jai::ivec2, jai_ivec2_type)                                                              \
		static native_type to_native(const host_type& h) noexcept { return {static_cast<int32_t>(h.X), static_cast<int32_t>(h.Y)}; } \
		static void assign(host_type& h, const native_type& n) noexcept { JAI_BIND_MATH_S_(X, x) JAI_BIND_MATH_S_(Y, y) } \
	};
#define JAI_BIND_IVEC3(HOST, X, Y, Z)                                                                                    \
	JAI_BIND_MATH_HEAD_(HOST, ::jai::ivec3, jai_ivec3_type)                                                              \
		static native_type to_native(const host_type& h) noexcept { return {static_cast<int32_t>(h.X), static_cast<int32_t>(h.Y), static_cast<int32_t>(h.Z)}; } \
		static void assign(host_type& h, const native_type& n) noexcept { JAI_BIND_MATH_S_(X, x) JAI_BIND_MATH_S_(Y, y) JAI_BIND_MATH_S_(Z, z) } \
	};
#define JAI_BIND_COLOR(HOST, R, G, B, A)                                                                                 \
	JAI_BIND_MATH_HEAD_(HOST, ::jai::color, jai_color_type)                                                              \
		static native_type to_native(const host_type& h) noexcept { return {static_cast<float>(h.R), static_cast<float>(h.G), static_cast<float>(h.B), static_cast<float>(h.A)}; } \
		static void assign(host_type& h, const native_type& n) noexcept { JAI_BIND_MATH_S_(R, r) JAI_BIND_MATH_S_(G, g) JAI_BIND_MATH_S_(B, b) JAI_BIND_MATH_S_(A, a) } \
	};

#endif // __JAISCRIPT_MATH_ADAPTER_HPP__
