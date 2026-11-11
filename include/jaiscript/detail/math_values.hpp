#pragma once

#ifndef __JAISCRIPT_DETAIL_MATH_VALUES_HPP__
#define __JAISCRIPT_DETAIL_MATH_VALUES_HPP__

// The math vectors' semantics, shared VERBATIM by both backends and by the value
// layer: components, construction, conversions, the operator rows, equality, ordering, hashing and text. math::vec2 /
// vec3 / vec4 hold binary32 components, math::ivec2 / ivec3 int32 components, all one boxed node type under one storage index per kind, whose
// node carries the count and the element type; every component operation is one binary32 (or checked int32)
// operation, so the interpreter, the VM and any lowering of typed vectors compute the same bits.

#include <jaiscript/core/value.hpp>
#include <jaiscript/core/checked_result.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/detail/numeric_conversion.hpp>
#include <jaiscript/detail/float_text.hpp>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

// The kernel's entry points stay out of line: the backends' hot handlers reach them only for a vector operand, and an
// inlined copy would grow every handler that never sees one
#if defined(_MSC_VER)
#define JAI_MATH_COLD __declspec(noinline)
#else
#define JAI_MATH_COLD __attribute__((noinline, cold))
#endif

namespace jai::detail {
	// A float as a map key: every NaN is one +qNaN (the last group of the total order) and -0 is +0. The key order and the
	// key hash of floats and of float vector components both read it
	inline double canonical_key_float(double x) noexcept { return x != x ? std::numeric_limits<double>::quiet_NaN() : x == 0.0 ? 0.0 : x; }
	inline float canonical_key_float(float x) noexcept { return x != x ? std::numeric_limits<float>::quiet_NaN() : x == 0.0f ? 0.0f : x; }
}

namespace jai::detail::math {

	// The build's integer-overflow policy (integer_ops.hpp's kCheckedOverflow, restated so this header stays off
	// <intrin.h>; the backends static_assert the two agree)
#if defined(JAISCRIPT_CHECKED_OVERFLOW)
	inline constexpr bool k_checked_overflow = (JAISCRIPT_CHECKED_OVERFLOW != 0);
#elif defined(JAISCRIPT_WRAP_ON_OVERFLOW)
	inline constexpr bool k_checked_overflow = false;
#else
	inline constexpr bool k_checked_overflow = true;
#endif

	using sv = script_value;
	using vk = script_value_type;
	inline constexpr size_t k_max_dims = 4;

	// The storage test (a raw index) and the kind: the script type of a vec value, jai_invalid_type for any other value
	JAI_FORCEINLINE bool is_kind(size_t raw) noexcept { return sv::is_math_index(raw); }
	JAI_FORCEINLINE bool any_kind(size_t a, size_t b) noexcept { return sv::is_math_index(a) || sv::is_math_index(b); }
	inline vk kind_of(const script_value& v) noexcept { return is_kind(v.raw_storage_index()) ? v.unchecked_vec_kind() : vk::jai_invalid_type; }
	inline bool is_vec_type(vk k) noexcept {
		switch (k) {
			case vk::jai_vec2_type: case vk::jai_vec3_type: case vk::jai_vec4_type: case vk::jai_ivec2_type: case vk::jai_ivec3_type:
			case vk::jai_color_type: return true;
			default: return false;
		}
	}
	inline size_t dims(vk k) noexcept {
		switch (k) {
			case vk::jai_vec2_type: case vk::jai_ivec2_type: return 2;
			case vk::jai_vec3_type: case vk::jai_ivec3_type: return 3;
			case vk::jai_vec4_type: case vk::jai_color_type: return 4;
			default: return 0;
		}
	}
	inline bool integral(vk k) noexcept { return k == vk::jai_ivec2_type || k == vk::jai_ivec3_type; }
	inline bool is_color(vk k) noexcept { return k == vk::jai_color_type; }
	// The typed-slot row between two kinds: the same kind, or the other precision of the same dimensions; a
	// color and a vector never convert into each other
	inline bool converts(vk from, vk to) noexcept {
		return from == to || (is_vec_type(from) && is_vec_type(to) && dims(from) == dims(to) && is_color(from) == is_color(to));
	}
	// A float vector into an integer vector of its dimensions: no slot takes it, cast<math::ivecN>(v) truncates each component
	inline bool narrows(vk from, vk to) noexcept { return from != to && integral(to) && !integral(from) && converts(from, to); }
	inline const char* kind_name(vk k) noexcept { return math_type_name(k); }
	// The kind with `n` components of the family `integral_family` (jai_invalid_type when the family has none)
	inline vk family_kind(bool integral_family, size_t n) noexcept {
		if (integral_family) { return n == 2 ? vk::jai_ivec2_type : n == 3 ? vk::jai_ivec3_type : vk::jai_invalid_type; }
		return n == 2 ? vk::jai_vec2_type : n == 3 ? vk::jai_vec3_type : n == 4 ? vk::jai_vec4_type : vk::jai_invalid_type;
	}

	// ---- components ---------------------------------------------------------------------------------------------
	// A member name's component slot, whatever the kind: x / width 0, y / height 1, z / depth 2, w 3; -1 names none
	inline int component_slot(std::string_view name) noexcept {
		if (name.size() == 1) {
			switch (name[0]) {
				case 'x': return 0;
				case 'y': return 1;
				case 'z': return 2;
				case 'w': return 3;
				default: return -1;
			}
		}
		if (name == "width") return 0;
		if (name == "height") return 1;
		if (name == "depth") return 2;
		return -1;
	}
	inline int component_of(vk k, std::string_view name) noexcept {
		if (is_color(k)) {   // a color's channels are r, g, b, a
			if (name.size() != 1) return -1;
			switch (name[0]) {
				case 'r': return 0;
				case 'g': return 1;
				case 'b': return 2;
				case 'a': return 3;
				default: return -1;
			}
		}
		const int c = component_slot(name);
		return (c >= 0 && static_cast<size_t>(c) < dims(k)) ? c : -1;
	}
	// A name some kind reads as a component (a vector's or a color's)
	inline bool any_component_name(std::string_view name) noexcept {
		return component_slot(name) >= 0 || component_of(vk::jai_color_type, name) >= 0;
	}
	// The canonical component name (the archives' keys)
	inline const char* component_name(vk k, size_t c) noexcept {
		static constexpr const char* const vec_names[4] = {"x", "y", "z", "w"};
		static constexpr const char* const color_names[4] = {"r", "g", "b", "a"};
		return is_color(k) ? color_names[c] : vec_names[c];
	}
	inline const char* no_member_text(vk k) noexcept {
		switch (k) {
			case vk::jai_vec2_type: return "math::vec2 has no member '{0}'";
			case vk::jai_vec3_type: return "math::vec3 has no member '{0}'";
			case vk::jai_vec4_type: return "math::vec4 has no member '{0}'";
			case vk::jai_color_type: return "math::color has no member '{0}'";
			case vk::jai_ivec2_type: return "math::ivec2 has no member '{0}'";
			default: return "math::ivec3 has no member '{0}'";
		}
	}

	inline float f32_at(const script_value& v, size_t i) noexcept {
		float f;
		std::memcpy(&f, &v.unchecked_vec_node().words[i], 4);
		return f;
	}
	inline int32_t i32_at(const script_value& v, size_t i) noexcept {
		int32_t c;
		std::memcpy(&c, &v.unchecked_vec_node().words[i], 4);
		return c;
	}
	// The component as the script scalar a read yields: a binary32 widens exactly to a script float, an int32 to a script int
	inline script_value component_value(const script_value& v, size_t i, engine* eng) {
		if (v.unchecked_vec_node().integral) { return script_value(static_cast<script_int>(i32_at(v, i)), eng); }
		return script_value(static_cast<script_float>(f32_at(v, i)), eng);
	}

	// ---- scalar narrowing (one kernel for every landing) -------------------------------------------------------------
	// A script float rounds to nearest binary32; a finite value beyond binary32's range becomes +-infinity (the IEEE
	// conversion); an int converts in one rounding
	inline float to_f32(script_float d) noexcept { return static_cast<float>(d); }
	inline float to_f32(script_int i) noexcept { return static_cast<float>(i); }

	inline constexpr std::string_view int32_overflow_text = "Integer overflow: the value does not fit an int32 component";
	// An int64 into an int32 component: in range stores, out of range follows the build's integer policy
	inline checked_result<int32_t> to_i32(script_int v) noexcept {
		if (v >= std::numeric_limits<int32_t>::min() && v <= std::numeric_limits<int32_t>::max()) {
			return static_cast<int32_t>(v);
		}
		if constexpr (!k_checked_overflow) {
			return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(v)));
		}
		return checked_result<int32_t>(make_error_code(runtime_error_code::invalid_numeric_operand), int32_overflow_text);
	}
	inline checked_result<int32_t> to_i32(script_float d) noexcept {
		auto i = checked_float_to_integer(d);
		if (!i) { return checked_result<int32_t>(make_error_code(runtime_error_code::invalid_numeric_operand), float_to_integer_error); }
		return to_i32(i.value());
	}

	// A scalar operand as the component operations take it: an int, a float or a bool (C++'s arithmetic conversions);
	// anything else is not a scalar here
	struct scalar {
		bool ok = false;
		bool is_int = false;
		script_int i = 0;
		script_float f = 0.0;
	};
	inline scalar read_scalar(const script_value& v) noexcept {
		scalar s;
		switch (v.raw_storage_index()) {
			case sv::TYPEID_INT: s.ok = true; s.is_int = true; s.i = v.unchecked_as_int(); break;
			case sv::TYPEID_FLOAT: s.ok = true; s.f = v.unchecked_as_float(); break;
			case sv::TYPEID_BOOL: s.ok = true; s.is_int = true; s.i = v.unchecked_as_bool() ? 1 : 0; break;
			default: break;
		}
		return s;
	}
	inline float scalar_f32(const scalar& s) noexcept { return s.is_int ? to_f32(s.i) : to_f32(s.f); }
	inline checked_result<int32_t> scalar_i32(const scalar& s) noexcept { return s.is_int ? to_i32(s.i) : to_i32(s.f); }

	// ---- construction -----------------------------------------------------------------------------------------------
	inline script_value make(vk k, const float* f, const int32_t* i, engine* eng) {
		uint32_t words[k_max_dims] = {0, 0, 0, 0};
		std::memcpy(words, integral(k) ? static_cast<const void*>(i) : static_cast<const void*>(f), 4 * dims(k));
		return script_value::make_vec(k, words, eng);
	}

	// A vec converted to another kind of the same dimensions (the typed-slot row): int32 to binary32 in
	// one rounding, binary32 to int32 through the checked float-to-integer kernel; the same kind passes
	JAI_MATH_COLD inline checked_result<script_value> convert(const script_value& v, vk target, engine* eng) {
		const vk k = kind_of(v);
		if (k == target) { return v; }
		if (!converts(k, target)) {
			return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "math kinds of different dimensions do not convert");
		}
		float f[k_max_dims] = {0, 0, 0, 0};
		int32_t i[k_max_dims] = {0, 0, 0, 0};
		for (size_t c = 0; c < dims(k); ++c) {
			if (integral(target)) {
				auto n = to_i32(static_cast<script_float>(f32_at(v, c)));
				if (!n) { return n.error_value(); }
				i[c] = n.value();
			} else {
				f[c] = static_cast<float>(i32_at(v, c));
			}
		}
		return make(target, f, i, eng);
	}

	// The constructor math::K(args): () zero; scalars for the first components (the rest default to 0);
	// (v, scalars...) a narrower vector of the same family followed by scalars that complete the count exactly
	// (vec3(v2, z), vec4(v3, w), vec4(v2, z, w)); (v) a vector of the same dimensions (converted)
	inline const char* ctor_arity_text(vk k) noexcept {
		switch (k) {
			case vk::jai_vec2_type: return "math::vec2 expects 0, 1 or 2 arguments";
			case vk::jai_vec3_type: return "math::vec3 expects 0 to 3 arguments";
			case vk::jai_vec4_type: return "math::vec4 expects 0 to 4 arguments";
			case vk::jai_color_type: return "math::color expects 0, 1, 3 or 4 arguments";
			case vk::jai_ivec2_type: return "math::ivec2 expects 0, 1 or 2 arguments";
			default: return "math::ivec3 expects 0 to 3 arguments";
		}
	}
	inline const char* ctor_type_text(vk k) noexcept {
		switch (k) {
			case vk::jai_vec2_type: return "math::vec2 expects numeric components";
			case vk::jai_vec3_type: return "math::vec3 expects numeric components";
			case vk::jai_vec4_type: return "math::vec4 expects numeric components";
			case vk::jai_color_type: return "math::color expects numeric components";
			case vk::jai_ivec2_type: return "math::ivec2 expects numeric components";
			default: return "math::ivec3 expects numeric components";
		}
	}
	// a lone argument converts a vector of the same dimensions (a color copies a color); a scalar is no vector
	inline const char* ctor_single_text(vk k) noexcept {
		switch (k) {
			case vk::jai_vec2_type: return "math::vec2 expects one vector of 2 components";
			case vk::jai_vec3_type: return "math::vec3 expects one vector of 3 components";
			case vk::jai_vec4_type: return "math::vec4 expects one vector of 4 components";
			case vk::jai_color_type: return "math::color expects one color";
			case vk::jai_ivec2_type: return "math::ivec2 expects one vector of 2 components";
			default: return "math::ivec3 expects one vector of 3 components";
		}
	}
	// a leading vector widens only when the scalars after it complete the components
	inline const char* ctor_widen_text(vk k) noexcept {
		switch (k) {
			case vk::jai_vec2_type: return "math::vec2 expects a narrower vector followed by scalars that complete 2 components";
			case vk::jai_vec3_type: return "math::vec3 expects a narrower vector followed by scalars that complete 3 components";
			case vk::jai_vec4_type: return "math::vec4 expects a narrower vector followed by scalars that complete 4 components";
			case vk::jai_ivec2_type: return "math::ivec2 expects a narrower vector followed by scalars that complete 2 components";
			default: return "math::ivec3 expects a narrower vector followed by scalars that complete 3 components";
		}
	}
	JAI_MATH_COLD inline checked_result<script_value> construct(vk k, const script_value* const* args, size_t argc, engine* eng) {
		const size_t n = dims(k);
		float f[k_max_dims] = {0, 0, 0, 0};
		int32_t i[k_max_dims] = {0, 0, 0, 0};
		auto bad_type = [&] { return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), ctor_type_text(k)); };
		auto bad_shape = [&](const char* text) { return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), text); };
		auto put = [&](size_t c, const script_value& a) -> checked_result<void> {
			const scalar s = read_scalar(a);
			if (!s.ok) { return checked_result<void>(make_error_code(runtime_error_code::type_mismatch), ctor_type_text(k)); }
			if (integral(k)) {
				auto v = scalar_i32(s);
				if (!v) { return v.error_value(); }
				i[c] = v.value();
			} else {
				f[c] = scalar_f32(s);
			}
			return {};
		};
		if (is_color(k)) {   // () opaque white; (c) a color; (r, g, b) opaque; (r, g, b, a)
			float rgba[k_max_dims] = {1.0f, 1.0f, 1.0f, 1.0f};
			if (argc == 1) {
				if (!is_kind(args[0]->raw_storage_index()) || kind_of(*args[0]) != k) { return bad_shape(ctor_single_text(k)); }
				return *args[0];
			}
			if (argc != 0 && argc != 3 && argc != 4) {
				return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), ctor_arity_text(k));
			}
			for (size_t a = 0; a < argc; ++a) {
				const scalar sc = read_scalar(*args[a]);
				if (!sc.ok) { return bad_type(); }
				rgba[a] = scalar_f32(sc);
			}
			return make(k, rgba, i, eng);
		}
		if (argc == 0) { return make(k, f, i, eng); }
		if (argc == 1) {
			if (!is_kind(args[0]->raw_storage_index()) || dims(kind_of(*args[0])) != n) { return bad_shape(ctor_single_text(k)); }
			return convert(*args[0], k, eng);
		}
		if (argc > n) {
			return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), ctor_arity_text(k));
		}
		if (is_kind(args[0]->raw_storage_index())) {
			const size_t d = dims(kind_of(*args[0]));
			const vk narrow = family_kind(integral(k), d);
			if (d >= n || d + (argc - 1) != n || narrow == vk::jai_invalid_type) { return bad_shape(ctor_widen_text(k)); }
			auto head = convert(*args[0], narrow, eng);
			if (!head) { return head.error_value(); }
			for (size_t c = 0; c < d; ++c) {
				if (integral(k)) { i[c] = i32_at(head.value(), c); } else { f[c] = f32_at(head.value(), c); }
			}
			for (size_t a = 1; a < argc; ++a) {
				if (auto r = put(d + a - 1, *args[a]); !r) { return r.error_value(); }
			}
			return make(k, f, i, eng);
		}
		for (size_t a = 0; a < argc; ++a) {
			if (auto r = put(a, *args[a]); !r) { return r.error_value(); }
		}
		return make(k, f, i, eng);
	}

	// ---- component store --------------------------------------------------------------------------------------------
	inline const char* component_store_text(vk k) noexcept {
		switch (k) {
			case vk::jai_vec2_type: return "Cannot store a non-numeric value in a math::vec2 component";
			case vk::jai_vec3_type: return "Cannot store a non-numeric value in a math::vec3 component";
			case vk::jai_vec4_type: return "Cannot store a non-numeric value in a math::vec4 component";
			case vk::jai_color_type: return "Cannot store a non-numeric value in a math::color component";
			case vk::jai_ivec2_type: return "Cannot store a non-numeric value in a math::ivec2 component";
			default: return "Cannot store a non-numeric value in a math::ivec3 component";
		}
	}
	inline constexpr std::string_view temporary_member_text = "Cannot assign to a member of a temporary";
	// Writes component c of the vec held in `target` (live storage) from `value` (dereferenced, bound decoded): an int, a
	// float or a bool narrows by the kind's rule; returns the stored component as the scalar a read yields
	// The value a component of kind k holds after a store of `value` (its word in `bits`): the store's narrowing, shared by
	// the component store and the lowered component slots
	inline checked_result<script_value> narrow_component(vk k, const script_value& value, uint32_t& bits, engine* eng) {
		const scalar s = read_scalar(value);
		if (!s.ok) {
			return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), component_store_text(k));
		}
		if (integral(k)) {
			auto v = scalar_i32(s);
			if (!v) { return v.error_value(); }
			bits = static_cast<uint32_t>(v.value());
			return script_value(static_cast<script_int>(v.value()), eng);
		}
		const float f = scalar_f32(s);
		std::memcpy(&bits, &f, 4);
		return script_value(static_cast<script_float>(f), eng);
	}
	JAI_MATH_COLD inline checked_result<script_value> store_component(script_value& target, int c, const script_value& value, engine* eng) {
		uint32_t bits = 0;
		auto stored = narrow_component(target.unchecked_vec_kind(), value, bits, eng);
		if (stored) { target.unchecked_set_vec_word(static_cast<size_t>(c), bits); }
		return stored;
	}

	// ---- operators --------------------------------------------------------------------------------------------------
	enum class bop : uint8_t { add, sub, mul, div, mod };
	inline const char* invalid_operands_text(bop op) noexcept {
		switch (op) {
			case bop::add: return "Invalid operands for + operator";
			case bop::sub: return "Invalid operands for - operator";
			case bop::mul: return "Invalid operands for * operator";
			case bop::div: return "Invalid operands for / operator";
			default: return "Invalid operands for % operator";
		}
	}
	inline const char* overflow_text(bop op) noexcept {
		switch (op) {
			case bop::add: return "Integer overflow in '+'";
			case bop::sub: return "Integer overflow in '-'";
			case bop::mul: return "Integer overflow in '*'";
			case bop::div: return "Integer overflow in '/'";
			default: return "Integer overflow in '%'";
		}
	}

	inline constexpr const char* negate_overflow_text = "Integer overflow in unary '-'";
	// An ivec operator's exact int64 result into its int32 component: out of range raises the operator's text, or wraps
	// under the wrap policy
	inline checked_result<int32_t> narrow_int_component(int64_t v, const char* text) noexcept {
		if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max()) {
			if constexpr (k_checked_overflow) {
				return checked_result<int32_t>(make_error_code(runtime_error_code::invalid_numeric_operand), text);
			}
		}
		return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(v)));
	}
	// One ivec component of `a op b`: two int32 operands (or a narrowed scalar) computed exactly in int64 and narrowed to
	// int32 under the policy; a zero divisor raises the scalar kernels' texts
	inline checked_result<int32_t> int_component(bop op, int64_t a, int64_t b) noexcept {
		int64_t v = 0;
		switch (op) {
			case bop::add: v = a + b; break;
			case bop::sub: v = a - b; break;
			case bop::mul: v = a * b; break;
			case bop::div:
				if (b == 0) { return checked_result<int32_t>(make_error_code(runtime_error_code::division_by_zero), "Division by zero"); }
				v = a / b;
				break;
			case bop::mod:
				if (b == 0) { return checked_result<int32_t>(make_error_code(runtime_error_code::modulo_by_zero), "Modulo by zero"); }
				v = a % b;
				break;
		}
		return narrow_int_component(v, overflow_text(op));
	}
	inline checked_result<int32_t> int_component_negate(int32_t x) noexcept {
		return narrow_int_component(-static_cast<int64_t>(x), negate_overflow_text);
	}

	// `l op r` where at least one side is a vec: vec op vec (the same kind, componentwise), vec op scalar and scalar op
	// vec (the scalar applied to every component). The scalar converts to the component type first, as C++'s
	// `Point<T> * T` does: vec kinds take int and float scalars and compute in binary32; ivec kinds take int scalars (a
	// scalar outside int32 follows the integer policy) and compute each component exactly in int64 - two int32 operands
	// cannot overflow it - narrowing the result to int32 under the policy. An ivec's zero divisor raises the scalar
	// kernels' texts; a vec's is IEEE. `%` is the ivec kinds' only.
	JAI_MATH_COLD inline checked_result<script_value> binary(bop op, const script_value& l, const script_value& r, engine* eng) {
		const bool lm = is_kind(l.raw_storage_index()), rm = is_kind(r.raw_storage_index());
		const vk k = lm ? l.unchecked_vec_kind() : r.unchecked_vec_kind();
		auto invalid = [&] { return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), invalid_operands_text(op)); };
		if (lm && rm && r.unchecked_vec_kind() != k) { return invalid(); }
		const size_t n = dims(k);
		scalar s;
		if (!(lm && rm)) {
			s = read_scalar(lm ? r : l);
			if (!s.ok) { return invalid(); }
		}
		float f[k_max_dims] = {0, 0, 0, 0};
		int32_t out[k_max_dims] = {0, 0, 0, 0};
		if (integral(k)) {
			int32_t si = 0;
			if (!(lm && rm)) {
				if (!s.is_int) { return invalid(); }
				auto narrowed = to_i32(s.i);
				if (!narrowed) { return narrowed.error_value(); }
				si = narrowed.value();
			}
			for (size_t c = 0; c < n; ++c) {
				auto v = int_component(op, lm ? i32_at(l, c) : si, rm ? i32_at(r, c) : si);
				if (!v) { return v.error_value(); }
				out[c] = v.value();
			}
			return make(k, f, out, eng);
		}
		if (op == bop::mod) { return invalid(); }
		const float sf = (lm && rm) ? 0.0f : scalar_f32(s);
		for (size_t c = 0; c < n; ++c) {
			const float a = lm ? f32_at(l, c) : sf;
			const float b = rm ? f32_at(r, c) : sf;
			switch (op) {
				case bop::add: f[c] = a + b; break;
				case bop::sub: f[c] = a - b; break;
				case bop::mul: f[c] = a * b; break;
				default: f[c] = a / b; break;
			}
		}
		return make(k, f, out, eng);
	}

	JAI_MATH_COLD inline checked_result<script_value> negate(const script_value& v, engine* eng) {
		const vk k = v.unchecked_vec_kind();
		float f[k_max_dims] = {0, 0, 0, 0};
		int32_t i[k_max_dims] = {0, 0, 0, 0};
		for (size_t c = 0; c < dims(k); ++c) {
			if (integral(k)) {
				auto x = int_component_negate(i32_at(v, c));
				if (!x) { return x.error_value(); }
				i[c] = x.value();
			} else {
				f[c] = -f32_at(v, c);
			}
		}
		return make(k, f, i, eng);
	}

	// Script ==: the same dimensions compare componentwise as numbers (a binary32 and an int32 widen exactly, so a vec
	// and an ivec holding the same values are equal; NaN is unequal, as for floats); anything else is unequal
	JAI_MATH_COLD inline bool equal(const script_value& l, const script_value& r) noexcept {
		const vk lk = kind_of(l), rk = kind_of(r);
		if (!is_vec_type(lk) || !is_vec_type(rk) || dims(lk) != dims(rk) || is_color(lk) != is_color(rk)) { return false; }
		for (size_t c = 0; c < dims(lk); ++c) {
			const double a = integral(lk) ? static_cast<double>(i32_at(l, c)) : static_cast<double>(f32_at(l, c));
			const double b = integral(rk) ? static_cast<double>(i32_at(r, c)) : static_cast<double>(f32_at(r, c));
			if (!(a == b)) { return false; }
		}
		return true;
	}
	inline const char* no_ordering_text(vk k) noexcept {
		switch (k) {
			case vk::jai_vec2_type: return "math::vec2 has no ordering";
			case vk::jai_vec3_type: return "math::vec3 has no ordering";
			case vk::jai_vec4_type: return "math::vec4 has no ordering";
			case vk::jai_color_type: return "math::color has no ordering";
			case vk::jai_ivec2_type: return "math::ivec2 has no ordering";
			default: return "math::ivec3 has no ordering";
		}
	}

	// The handlers' prologues (both backends' arithmetic and comparison twins): the math rows own `l op r` when a vec
	// operand meets no class object (its user operators keep their place) and, for arithmetic, no string (`"p = " + v`
	// concatenates the text)
	JAI_FORCEINLINE bool owns_comparison(size_t lr, size_t rr) noexcept {
		return any_kind(lr, rr) && lr != sv::TYPEID_OBJECT && rr != sv::TYPEID_OBJECT;
	}
	JAI_FORCEINLINE bool owns_arithmetic(size_t lr, size_t rr) noexcept {
		return owns_comparison(lr, rr) && lr != sv::TYPEID_STRING && rr != sv::TYPEID_STRING;
	}
	// `target /= right` and `target %= right` raise on a zero divisor ahead of the operands' kinds only where the
	// arithmetic is integer: an int zero over a target that is neither a float nor a vec (their division is IEEE)
	inline bool integer_zero_divisor(const script_value& target, const script_value& right) noexcept {
		if (right.raw_storage_index() != sv::TYPEID_INT || right.unchecked_as_int() != 0) { return false; }
		const script_value& t = target.deref();
		return !t.is_float() && !(is_kind(t.raw_storage_index()) && !integral(t.unchecked_vec_kind()));
	}
	JAI_MATH_COLD inline checked_result<script_value> ordering_error(const script_value& l, const script_value& r) {
		return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch),
			no_ordering_text(is_kind(l.raw_storage_index()) ? l.unchecked_vec_kind() : r.unchecked_vec_kind()));
	}

	// Truthiness: not all zero (the numeric rule; NaN is nonzero, as for floats)
	inline bool truthy(const script_value& v) noexcept { return v.math_truthy(); }

	// ---- the C++-level identity rows (map keys) -------------------------------------------------------------------
	// operator==: the same kind, componentwise with the component's == (float's, like jai_float_type's row);
	// operator<=> and map_key_hash: by kind, then the components, a float one as canonical_key_float (as the float row)
	JAI_MATH_COLD inline bool same_kind_equal(const script_value& l, const script_value& r) noexcept {
		const vk k = l.unchecked_vec_kind();
		if (r.unchecked_vec_kind() != k) { return false; }
		for (size_t c = 0; c < dims(k); ++c) {
			if (integral(k) ? i32_at(l, c) != i32_at(r, c) : !(f32_at(l, c) == f32_at(r, c))) { return false; }
		}
		return true;
	}
	JAI_MATH_COLD inline std::strong_ordering same_kind_order(const script_value& l, const script_value& r) noexcept {
		const vk k = l.unchecked_vec_kind(), rk = r.unchecked_vec_kind();
		if (k != rk) { return static_cast<int>(k) <=> static_cast<int>(rk); }
		for (size_t c = 0; c < dims(k); ++c) {
			const std::strong_ordering o = integral(k) ? i32_at(l, c) <=> i32_at(r, c)
			                                           : std::strong_order(canonical_key_float(f32_at(l, c)), canonical_key_float(f32_at(r, c)));
			if (o != 0) { return o; }
		}
		return std::strong_ordering::equal;
	}
	JAI_MATH_COLD inline uint64_t hash_bits(const script_value& v) noexcept {
		const vk k = v.unchecked_vec_kind();
		uint64_t h = (0xcbf29ce484222325ull ^ static_cast<uint64_t>(k)) * 0x100000001b3ull;
		for (size_t c = 0; c < dims(k); ++c) {
			uint32_t bits = 0;
			if (integral(k)) {
				bits = static_cast<uint32_t>(i32_at(v, c));
			} else {
				const float x = canonical_key_float(f32_at(v, c));
				std::memcpy(&bits, &x, 4);
			}
			h = (h ^ bits) * 0x100000001b3ull;
		}
		return h;
	}

	// ---- text -------------------------------------------------------------------------------------------------------
	// Components in parentheses: a binary32 component in the shortest text that round-trips it (a whole number keeps
	// ".0", as float_text does), an int32 component in decimal: (1.0, 2.5, 3.0), (4, -2, 0)
	inline void append_f32(std::string& out, float v) {
		if (std::isnan(v)) { out += k_nan_text; return; }
		char buf[32];
		auto res = std::to_chars(buf, buf + sizeof(buf), v);
		std::string_view s(buf, static_cast<size_t>(res.ptr - buf));
		out += s;
		if (s.find_first_of(".en") == std::string_view::npos) { out += ".0"; }
	}
	JAI_MATH_COLD inline std::string text(const script_value& v) {
		const vk k = v.unchecked_vec_kind();
		std::string out = "(";
		for (size_t c = 0; c < dims(k); ++c) {
			if (c) { out += ", "; }
			if (integral(k)) { out += std::to_string(i32_at(v, c)); } else { append_f32(out, f32_at(v, c)); }
		}
		out += ")";
		return out;
	}

} // namespace jai::detail::math

#endif // __JAISCRIPT_DETAIL_MATH_VALUES_HPP__
