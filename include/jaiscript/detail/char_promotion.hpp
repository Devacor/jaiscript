#pragma once

#ifndef __JAISCRIPT_DETAIL_CHAR_PROMOTION_HPP__
#define __JAISCRIPT_DETAIL_CHAR_PROMOTION_HPP__

// C++-style integral promotion for the char domain, shared VERBATIM by the vm and the
// interpreter (parity by construction). A char operand enters ARITHMETIC and BITWISE
// binary operators as an int64 in 0..255 — char is unsigned by language spec; signed
// char is a binary-data footgun the language opts out of.
//
// Strictly additive by construction: promotion fires only when both operands are
// numeric-or-char: a char beside a string is its one-character text (char_beside_string),
// object custom operators still receive the raw char, and comparison operators keep their
// native char rows. There is no implicit int->char.

#include <jaiscript/core/value.hpp>

namespace jai::detail {

	// the one test a general operator pays before either char rule below
	inline bool char_operand(size_t left_index, size_t right_index) noexcept {
		return left_index == script_value::TYPEID_CHAR || right_index == script_value::TYPEID_CHAR;
	}

	inline bool char_operands_promote(size_t left_index, size_t right_index) {
		if (left_index != script_value::TYPEID_CHAR && right_index != script_value::TYPEID_CHAR) {
			return false;
		}
		auto numeric_or_char = [](size_t index) {
			return index == script_value::TYPEID_INT || index == script_value::TYPEID_FLOAT ||
				index == script_value::TYPEID_CHAR;
		};
		return numeric_or_char(left_index) && numeric_or_char(right_index);
	}

	inline script_value char_promoted(const script_value& value, engine* eng) {
		if (value.raw_storage_index() == script_value::TYPEID_CHAR) {
			return script_value(static_cast<script_int>(static_cast<unsigned char>(value.unchecked_as_char())), eng);
		}
		return value;
	}

	// A char beside a string compares, orders and joins as its one-character text, so s[i] == "x" and s + 'c' read as written
	inline bool char_beside_string(size_t left_index, size_t right_index) noexcept {
		return (left_index == script_value::TYPEID_CHAR && right_index == script_value::TYPEID_STRING) ||
		       (left_index == script_value::TYPEID_STRING && right_index == script_value::TYPEID_CHAR);
	}

	inline script_value char_as_text(const script_value& value, engine* eng) {
		if (value.raw_storage_index() == script_value::TYPEID_CHAR) {
			return script_value(std::string(1, value.unchecked_as_char()), eng);
		}
		return value;
	}

}

#endif
