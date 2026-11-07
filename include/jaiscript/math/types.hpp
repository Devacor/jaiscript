#pragma once

#ifndef __JAISCRIPT_MATH_TYPES_HPP__
#define __JAISCRIPT_MATH_TYPES_HPP__

// The C++ face of JaiScript's math vectors: plain aggregates with no engine, laid out as
// their components in order.

#include <cstdint>
#include <type_traits>

namespace jai {

	struct vec2 {
		float x = 0.0f, y = 0.0f;
		friend bool operator==(const vec2&, const vec2&) = default;
	};
	struct vec3 {
		float x = 0.0f, y = 0.0f, z = 0.0f;
		friend bool operator==(const vec3&, const vec3&) = default;
	};
	struct vec4 {
		float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
		friend bool operator==(const vec4&, const vec4&) = default;
	};
	// straight (not premultiplied) alpha; the default is opaque white
	struct color {
		float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
		friend bool operator==(const color&, const color&) = default;
	};
	struct ivec2 {
		int32_t x = 0, y = 0;
		friend bool operator==(const ivec2&, const ivec2&) = default;
	};
	struct ivec3 {
		int32_t x = 0, y = 0, z = 0;
		friend bool operator==(const ivec3&, const ivec3&) = default;
	};

	static_assert(sizeof(vec2) == 8 && alignof(vec2) == 4 && std::is_trivially_copyable_v<vec2>);
	static_assert(sizeof(vec3) == 12 && alignof(vec3) == 4 && std::is_trivially_copyable_v<vec3>);
	static_assert(sizeof(vec4) == 16 && alignof(vec4) == 4 && std::is_trivially_copyable_v<vec4>);
	static_assert(sizeof(color) == 16 && alignof(color) == 4 && std::is_trivially_copyable_v<color>);
	static_assert(sizeof(ivec2) == 8 && alignof(ivec2) == 4 && std::is_trivially_copyable_v<ivec2>);
	static_assert(sizeof(ivec3) == 12 && alignof(ivec3) == 4 && std::is_trivially_copyable_v<ivec3>);

} // namespace jai

#endif // __JAISCRIPT_MATH_TYPES_HPP__
