#pragma once

#ifndef __JAISCRIPT_DETAIL_UTF8_OPERANDS_HPP__
#define __JAISCRIPT_DETAIL_UTF8_OPERANDS_HPP__

// A utf8_string operand enters every operator as its string (the bytes compare and join exactly as a string's), and an
// operator that produced a string from a utf8_string operand hands back a utf8_string. Both backends normalize at the
// head of each general operator entry, before the char promotion (a char joins a utf8_string as text).

#include <jaiscript/core/value.hpp>

namespace jai::detail {

	inline bool utf8_operands(size_t left_index, size_t right_index) noexcept {
		return left_index == script_value::TYPEID_UTF8_STRING || right_index == script_value::TYPEID_UTF8_STRING;
	}

	inline script_value utf8_unwrapped(const script_value& value, engine* eng) {
		if (value.raw_storage_index() == script_value::TYPEID_UTF8_STRING) {
			return script_value(value.unchecked_as_utf8().bytes, eng);
		}
		return value;
	}

	inline checked_result<script_value> utf8_rewrapped(checked_result<script_value> result, engine* eng) {
		if (result && result.value().raw_storage_index() == script_value::TYPEID_STRING) {
			return script_value::make_utf8(std::move(result.value().unchecked_as_string_ref(eng)), eng);
		}
		return result;
	}

}

#endif
