#pragma once

#ifndef __JAISCRIPT_DETAIL_STRING_JOIN_HPP__
#define __JAISCRIPT_DETAIL_STRING_JOIN_HPP__

// A string's `+` joins text, as std::string's does: a char beside a string joins as its character, and a number, bool or null
// beside a string is an error with one text, at load where the checker knows both types and at run time otherwise. A template
// string's own `+` (token::joins_text) makes its splice text instead. Shared by both backends and the scoped templates.

#include <jaiscript/core/value.hpp>
#include <jaiscript/core/checked_result.hpp>
#include <jaiscript/core/runtime_errors.hpp>

namespace jai::detail {

	inline bool string_beside_scalar(size_t li, size_t ri) noexcept {
		const auto scalar = [](size_t k) {
			return k == script_value::TYPEID_INT || k == script_value::TYPEID_FLOAT || k == script_value::TYPEID_BOOL ||
			       k == script_value::TYPEID_NULL;
		};
		return (li == script_value::TYPEID_STRING && scalar(ri)) || (ri == script_value::TYPEID_STRING && scalar(li));
	}

	JAI_NOINLINE inline checked_result<script_value> string_plus_refused(size_t li, size_t ri) {
		static constexpr const char* k_string_left[] = {
			"cannot apply '+' to 'string' and 'null': stringify it with to_string(x) or `${x}`",
			"cannot apply '+' to 'string' and 'int': stringify it with to_string(x) or `${x}`",
			"cannot apply '+' to 'string' and 'float': stringify it with to_string(x) or `${x}`",
			"",
			"",
			"cannot apply '+' to 'string' and 'bool': stringify it with to_string(x) or `${x}`"};
		static constexpr const char* k_string_right[] = {
			"cannot apply '+' to 'null' and 'string': stringify it with to_string(x) or `${x}`",
			"cannot apply '+' to 'int' and 'string': stringify it with to_string(x) or `${x}`",
			"cannot apply '+' to 'float' and 'string': stringify it with to_string(x) or `${x}`",
			"",
			"",
			"cannot apply '+' to 'bool' and 'string': stringify it with to_string(x) or `${x}`"};
		const char* text = li == script_value::TYPEID_STRING ? k_string_left[ri] : k_string_right[li];
		return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), text);
	}

} // namespace jai::detail

#endif
