#pragma once

#ifndef __JAISCRIPT_DETAIL_IN_PLACE_RESULT_HPP__
#define __JAISCRIPT_DETAIL_IN_PLACE_RESULT_HPP__

// A builtin that changes its receiver in place returns nothing (C++ `void sort()`), so a call whose result is used is an error
// rather than a mutation that reads like a copy: the checker's load error where it knows the receiver, the same text raised by
// both backends when the call runs. A call in a statement's place discards its result (member_expr::call_result_discarded).

#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/method_value.hpp>
#include <jaiscript/detail/string_mutator_receiver.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace jai::detail {

	// "filter changes xs in place and returns nothing; array::filter(xs, f) returns a filtered copy"
	inline std::string in_place_result_text(std::string_view a_member, std::string_view a_receiver, std::string_view a_twin) {
		std::string result = std::string(a_member) + " changes " + std::string(a_receiver) + " in place and returns nothing";
		if (a_twin.empty()) { return result; }
		result += "; ";
		for (const char c : a_twin) {
			if (c == '@') { result += a_receiver; } else { result += c; }
		}
		return result;
	}

	// The twin text of the in-place builtin a call on this receiver (dereferenced) runs, or nullopt when the call runs none
	inline std::optional<std::string_view> in_place_builtin(const builtin_method_registries::in_place_ids& a_ids, const script_value& a_receiver, uint64_t a_member) {
		const std::unordered_map<uint64_t, std::string_view>* table = nullptr;
		switch (a_receiver.raw_storage_index()) {
			case script_value::TYPEID_ARRAY: table = &a_ids.array; break;
			case script_value::TYPEID_MAP: table = &a_ids.map; break;
			case script_value::TYPEID_STRING: case script_value::TYPEID_UTF8_STRING: table = &a_ids.string; break;
			default:
				if (a_member == a_ids.reset && handle_reset_applies(a_receiver, a_ids.reset)) { return std::string_view{}; }
				return std::nullopt;
		}
		if (auto found = table->find(a_member); found != table->end()) { return found->second; }
		return std::nullopt;
	}

	// The run-time error; a parallel worker, whose symbols are frozen, names no receiver unless an earlier raise interned its text
	JAI_NOINLINE inline checked_result<script_value> in_place_result_error(string_symbolizer* a_symbols, const member_expr* a_member, std::string_view a_twin) {
		const std::string text = in_place_result_text(a_member->member, method_value_receiver(a_member), a_twin);
		if (!a_symbols->frozen()) {
			return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation), "{0}", a_symbols->intern(text));
		}
		if (auto known = a_symbols->find(text)) {
			return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation), "{0}", *known);
		}
		return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation), "{0} changes its receiver in place and returns nothing",
			a_member->member_id);
	}

}

#endif // __JAISCRIPT_DETAIL_IN_PLACE_RESULT_HPP__
