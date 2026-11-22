#pragma once

#ifndef __JAISCRIPT_CORE_RECORDS_HPP__
#define __JAISCRIPT_CORE_RECORDS_HPP__

#include <jaiscript/core/script_type.hpp>
#include <jaiscript/core/checked_result.hpp>
#include <jaiscript/core/value.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jai {

class engine;

// Named field values, in order: a database row's columns, a record's fields
using record_field_list = std::vector<std::pair<std::string, script_value>>;

// Records: a script class instance built from named values, and an instance's fields read back as named values
// (docs/site/embedding/17-host-api.html "Records"). `who` leads every error text ("database.query: ...").
//
// make_record constructs the class T `type` names with T() (a class whose constructors all take arguments is refused), then
// stores each named value in the field of that name by the rules a script store follows. A name the class has no field for
// is skipped, and a field nothing names keeps its initializer. Beyond a script store, it converts what a row or a JSON
// document holds: an int 0 or 1 into a bool field, an int into a float field, text into an array, map or class field (parsed
// as JSON, from_json's rules), a map into a class field (its own record) and each element of an array or map field the same
// way. A value that still does not fit raises naming the class, the field and both types; null into a field that cannot be
// empty raises.
checked_result<script_value> make_record(engine& eng, const script_type& type, const record_field_list& fields, std::string_view who = "make_record");
// The same from a map's string keys
checked_result<script_value> make_record(engine& eng, const script_type& type, const script_value& fields, std::string_view who = "make_record");
// make_record's object answered as a shared handle (shared_ptr<T>), as make_shared<T>() answers it
checked_result<script_value> make_shared_record(engine& eng, const script_type& type, const record_field_list& fields, std::string_view who = "make_record");

// An instance's fields (a handle's object's), bases first and in declaration order; a map's string-keyed entries in key
// order. An array, map or class value is answered as its JSON text (to_json), for storage; every other value as it is.
// Anything that is neither an instance of a script class nor a map raises.
checked_result<record_field_list> record_fields(engine& eng, const script_value& record, std::string_view who = "record_fields");

} // namespace jai

#endif // __JAISCRIPT_CORE_RECORDS_HPP__
