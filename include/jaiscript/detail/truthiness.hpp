#pragma once
// Script truthiness, one rule for both backends' is_truthy and the typed-slot bool row; a class instance answers
// through object_truthy (its to_bool, which only the backend can call).
#include <jaiscript/core/value.hpp>

namespace jai::detail {

	script_value parallel_by_value(const script_value& value);   // detail/object_borrow.hpp

	template<class ObjectTruthy>
	inline bool truthy_of(const script_value& a_value, ObjectTruthy&& a_objectTruthy) {
		switch (a_value.raw_storage_index()) {
			case script_value::TYPEID_BOOL: return a_value.unchecked_as_bool();
			case script_value::TYPEID_NULL: return false;
			case script_value::TYPEID_INT: return a_value.unchecked_as_int() != 0;
			case script_value::TYPEID_FLOAT: return a_value.unchecked_as_float() != 0.0;
			case script_value::TYPEID_STRING: return !a_value.unchecked_as_string().empty();
			case script_value::TYPEID_UTF8_STRING: return !a_value.unchecked_as_utf8().bytes.empty();
			case script_value::TYPEID_VEC2: case script_value::TYPEID_IVEC2: case script_value::TYPEID_VEC3: case script_value::TYPEID_IVEC3: case script_value::TYPEID_VEC4: case script_value::TYPEID_COLOR: return a_value.math_truthy();
			case script_value::TYPEID_CHAR: return true;
			case script_value::TYPEID_ARRAY: {
				auto& arr = const_cast<script_value&>(a_value).get_array_storage();
				return arr && !arr->empty();
			}
			case script_value::TYPEID_MAP: {
				auto& map = const_cast<script_value&>(a_value).get_map_storage();
				return map && !map->empty();
			}
			case script_value::TYPEID_OBJECT: return a_objectTruthy(a_value);
			case script_value::TYPEID_CPP_BOUND:
				if (a_value.is_bool()) { return a_value.unchecked_as_bool(); }
				if (a_value.is_int()) { return a_value.unchecked_as_int() != 0; }
				if (a_value.is_float()) { return a_value.unchecked_as_float() != 0.0; }
				if (a_value.is_string()) { return !a_value.unchecked_as_string().empty(); }
				if (a_value.is_char()) { return true; }
				return a_value.get_cpp_bound_ptr() != nullptr;
			case script_value::TYPEID_PARALLEL_BORROW:   // a shared object answers as the worker's own copy of it
				return a_value.parallel_object_borrow() ? a_objectTruthy(parallel_by_value(a_value)) : true;
			case script_value::TYPEID_REFERENCE:
				// element reads reach conditions as reference wrappers
				return truthy_of(a_value.deref(), a_objectTruthy);
			default: return true;
		}
	}

}
