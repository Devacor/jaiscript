#pragma once

#ifndef __JAISCRIPT_DETAIL_ENUM_VALUE_HPP__
#define __JAISCRIPT_DETAIL_ENUM_VALUE_HPP__

// `E::v` on an enum (C++ scoped access): the enum declaration defines E as a map<string, int> of its values, and both backends
// read the value through it with this kernel

#include <jaiscript/core/value.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/engine_access.hpp>
#include <string>
#include <string_view>

namespace jai::detail {

	// The declaration's run: the enum's name becomes a type, and its value table (a map<string,int>) holds each value stamped with it
	inline script_value enum_value_table(const enum_decl& decl, engine* eng) {
		type_info* type = engine_access::note_enum(*eng, decl.name_id, decl.name, decl.numbers);
		script_value table = script_value::make_map(engine_access::get_type_info_string(*eng), engine_access::get_type_info_int(*eng), eng);
		auto& entries = const_cast<script_map&>(table.as_map());
		for (size_t i = 0; i < decl.values.size(); ++i) {
			script_value number(static_cast<script_int>(decl.numbers[i]), eng);
			number.set_type_info(type);
			entries[script_value(std::string(decl.values[i].first), eng)] = std::move(number);
		}
		entries.mark_enum_values();
		return table;
	}

	// An enum slot's default: the first declared value
	inline script_value enum_default_value(const type_info_ptr& type, engine* eng) {
		script_value number(static_cast<script_int>(type->enum_numbers.empty() ? 0 : type->enum_numbers.front()), eng);
		number.set_type_info(type->enum_value_type);
		return number;
	}

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
