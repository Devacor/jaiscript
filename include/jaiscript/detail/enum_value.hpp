#pragma once

#ifndef __JAISCRIPT_DETAIL_ENUM_VALUE_HPP__
#define __JAISCRIPT_DETAIL_ENUM_VALUE_HPP__

// `E::v` on an enum (C++ scoped access): the enum declaration defines E as a map<string, int> of its values, and both backends
// read the value through it with this kernel

#include <jaiscript/core/value.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <string>
#include <string_view>

namespace jai::detail {

	inline checked_result<script_value> enum_scoped_value(const script_map& values, uint64_t enum_id, std::string_view member,
	                                                      uint64_t member_id, engine* eng) {
		auto it = values.find(script_value(std::string(member), eng));
		if (it == values.end()) {
			return checked_result<script_value>(make_error_code(runtime_error_code::member_not_found), "No value '{1}' in enum '{0}'",
			                                    enum_id, member_id);
		}
		return it->second;
	}

}

#endif // __JAISCRIPT_DETAIL_ENUM_VALUE_HPP__
