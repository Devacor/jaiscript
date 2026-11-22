#pragma once

#ifndef __JAISCRIPT_DETAIL_TYPE_VALUE_HPP__
#define __JAISCRIPT_DETAIL_TYPE_VALUE_HPP__

// A type argument's value (cast_expr::type_value, docs/site/guide/06-functions.html "Type arguments"): both backends push what
// make_type_value answers, the host class reflect::type over a jai::script_type (source/implementation/records.cpp)

#include <jaiscript/core/type_info.hpp>
#include <jaiscript/core/value.hpp>
#include <jaiscript/core/checked_result.hpp>
#include <string_view>

namespace jai {
class engine;
}

namespace jai::detail {

	script_value make_type_value(const type_info_ptr& type, engine* eng);
	// from_json<T> and from_binary<T>: a loaded document as T, a class built by jai::make_record's rules (each value widened as a
	// record field's is); `document` names it in the refusal ("the JSON", "the data")
	checked_result<script_value> json_to_type(engine& eng, const script_value& value, const type_info_ptr& type, std::string_view who, std::string_view document);
	// A typed load's store of one saved member (stdlib/json.cpp, a record carrying a $type): by the record's rule, as the
	// same member of an object written without one is stored. A value the field refuses throws stdlib::typed_load_refusal.
	void store_typed_member(engine& eng, class_instance& instance, const class_definition& def, uint64_t field_id, const script_value& value, std::string_view who);
	// The same refusal for a member a binding stores (a C++ property): `declared` is the type it holds, null for any
	[[noreturn]] void throw_typed_member_refusal(engine& eng, const class_definition& def, uint64_t field_id, const type_info* declared, const script_value& value, std::string_view who);
	// reflect::type, on every engine at construction
	void register_type_library(engine& eng);

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_TYPE_VALUE_HPP__
