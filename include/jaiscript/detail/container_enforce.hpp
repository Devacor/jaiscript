#pragma once

#include <optional>
#include <string>
#include <utility>

#include <jaiscript/core/value.hpp>
#include <jaiscript/core/type_info.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/numeric_conversion.hpp>
#include <jaiscript/detail/arity_diagnostic.hpp>
#include <jaiscript/detail/math_values.hpp>
#include <jaiscript/detail/uniquely_held.hpp>

// Typed-container enforcement kernel, shared VERBATIM by both backends and the field
// kernel (parity by construction). Two layers:
//  - the element predicate/converter pair push and subscript stores use (consolidated
//    from the interpreter/vm twins),
//  - the whole-value BOUNDARY check: an array/map value crossing into a concretely
//    typed container binding (decl, assign, field write) validates every element with
//    push's rules, coerces int<->float payloads, and retags the result to the declared
//    type so later pushes stay enforced. Pre-fix, the same-base fast path admitted ANY
//    array behind an array<T> tag — the hole that silently broke the parallel
//    all-primitive proof.
// Scope: concrete PRIMITIVE element targets get full validate+convert semantics;
// nested container/object element targets keep push's one-level tag-based compat.

namespace jai::detail {

	inline std::string container_base_type_name(script_value_type type) { return std::string(base_type_spelling(type)); }

	// engine.cpp: the class rows of the element boundary judge the instance behind the value's
	// tag (a var-held Animal, a shared_ptr<Dog> handle) with the scalar table's own admission
	bool object_element_admits(const script_value& v, const type_info& element_type, engine* eng);
	// a handle into a shared_ptr<T> element: a variable's rule (the static pointee of a typed handle, a var-held one's class)
	bool shared_element_admits(const script_value& v, const type_info_ptr& element_type, engine* eng);
	// a shared handle into a weak_ptr<T> element: the declaration's row (C++: weak = shared), the handle downgrades
	bool weak_element_takes_shared(const script_value& v, const type_info_ptr& element_type, engine* eng);
	checked_result<script_value> weak_element_from_shared(const script_value& v, engine* eng);
	// The class a tag names for the subclass walk: a handle tag's pointee, else the tag itself
	inline uint64_t class_tag_id(const type_info& t) {
		const bool handle = t.base_type == script_value_type::jai_shared_ptr_type || t.base_type == script_value_type::jai_weak_ptr_type;
		return handle && t.element_type() ? t.element_type()->id : t.id;
	}
	// engine.cpp: the instance's own class tag, for a copy stored under a var tag (null = none)
	type_info_ptr object_element_own_tag(const script_value& v, engine* eng);

	// The source spelling of every element and key text: the payload's type, never a var tag
	inline std::string container_value_type_name(const script_value& val) {
		const script_value& v = val.is_reference() ? val.deref() : val;
		auto info = v.get_type_info();
		if (info && (info->base_type == script_value_type::jai_shared_ptr_type || info->base_type == script_value_type::jai_weak_ptr_type)) {
			return info->canonical_name();
		}
		if (info && !info->type_name.empty() && info->base_type != script_value_type::jai_any_type) {
			return info->type_name;
		}
		if (auto holder = v.get_object_holder(); holder && !holder->type_name().empty()) {
			return std::string(holder->type_name());
		}
		return container_base_type_name(v.current_type());
	}

	// The target spelling of every element, key and slot text: the declared type in full
	inline std::string type_info_display_name(const type_info_ptr& info) {
		return info ? info->canonical_name() : "unknown";
	}

	// A typed map's key boundary: the declared kind, or a char for a string key and a string for a char key (one key either way,
	// stored as the declared kind); any other kind refuses, since a numeric conversion could merge distinct entries
	inline bool map_key_kind_admits(script_value_type key, script_value_type declared) noexcept {
		return key == declared || (key == script_value_type::jai_char_type && declared == script_value_type::jai_string_type) ||
		       (key == script_value_type::jai_string_type && declared == script_value_type::jai_char_type);
	}
	// the key in the declared kind once map_key_kind_admits holds: a string of other than one character raises the char slot's text
	inline checked_result<script_value> map_key_in_kind(const script_value& key, script_value_type declared, engine* eng) {
		if (key.type() == declared) { return key; }
		if (declared == script_value_type::jai_string_type) { return script_value(std::string(1, key.unchecked_as_char()), eng); }
		auto converted = key.raw_storage_index() == script_value::TYPEID_STRING ? char_of_text(key.unchecked_as_string()) : char_of_text(key.to_string());
		if (!converted) { return converted.error_value(); }
		return script_value(converted.value(), eng);
	}

	inline bool container_element_compatible(const script_value& element, const type_info_ptr& element_type, engine* eng = nullptr) {
		const script_value& actual_element = element.is_reference() ? element.deref() : element;

		if (element_type && element_type->base_type == script_value_type::jai_any_type) {
			return true;
		}
		if (!element_type) {
			return true;
		}

		auto elem_type = actual_element.type();
		auto target_type = element_type->base_type;

		if (target_type == script_value_type::jai_weak_ptr_type && elem_type == script_value_type::jai_shared_ptr_type) {
			return weak_element_takes_shared(actual_element, element_type, eng);
		}

		// A handle-typed element takes null (C++: a vector of handles
		// holds empty handles); by-value elements stay non-nullable like std::vector<T>
		if (actual_element.raw_storage_index() == script_value::TYPEID_NULL) {
			return target_type == script_value_type::jai_shared_ptr_type || target_type == script_value_type::jai_weak_ptr_type;
		}

		if (elem_type == target_type) {
			if (target_type == script_value_type::jai_object_type) {
				auto elem_type_info = actual_element.get_type_info();
				if (elem_type_info && !element_type->type_name.empty()) {
					// exact class by interned id (one compare), else the instance's class (a var
					// tag says nothing about it) and the subclass walk
					return elem_type_info->id == element_type->id ||
					       elem_type_info->type_name == element_type->type_name ||
					       object_element_admits(actual_element, *element_type, eng);
				}
			}
			if (target_type == script_value_type::jai_shared_ptr_type && !element_type->type_name.empty()) {
				return shared_element_admits(actual_element, element_type, eng);
			}
			return true;
		}
		// A live instance stores into a shared_ptr<T> element the way it
		// stores into a shared_ptr<T> variable or field (the slot owns the handle)
		if (target_type == script_value_type::jai_shared_ptr_type && elem_type == script_value_type::jai_object_type) {
			return element_type->type_name.empty() || object_element_admits(actual_element, *element_type, eng);
		}

		if (target_type == script_value_type::jai_int_type && elem_type == script_value_type::jai_float_type) {
			return true;
		}
		if (target_type == script_value_type::jai_float_type && elem_type == script_value_type::jai_int_type) {
			return true;
		}
		// Bool and int convert both ways at the element boundary, like the
		// scalar slots (a number never becomes a string element: to_string / cast<string> spell it)
		if ((target_type == script_value_type::jai_int_type && elem_type == script_value_type::jai_bool_type) ||
		    (target_type == script_value_type::jai_bool_type && elem_type == script_value_type::jai_int_type)) {
			return true;
		}
		// math kinds: the other precision of the same dimensions converts, as at a typed slot
		if (is_math_type(target_type) && is_math_type(elem_type)) {
			return math::converts(elem_type, target_type);
		}
		// a char element takes a string (of one character: the conversion raises for any other) and a string element a char
		if ((target_type == script_value_type::jai_char_type && elem_type == script_value_type::jai_string_type) ||
		    (target_type == script_value_type::jai_string_type && elem_type == script_value_type::jai_char_type)) {
			return true;
		}

		// A shared_ptr-tagged instance stores into an object-typed element slot exactly
		// like field writes accept it (shared, not cloned — convert_container_element's
		// shared_ptr branch). Same exact-class-name rule the object==object branch uses.
		if (target_type == script_value_type::jai_object_type &&
		    elem_type == script_value_type::jai_shared_ptr_type) {
			if (element_type->type_name.empty()) {
				return true;
			}
			auto elem_type_info = actual_element.get_type_info();
			return elem_type_info && (elem_type_info->type_name == element_type->type_name ||
			                          object_element_admits(actual_element, *element_type, eng));
		}

		if (target_type == script_value_type::jai_array_type && elem_type == script_value_type::jai_array_type) {
			auto inner_target = element_type->element_type();
			auto elem_type_info = actual_element.get_type_info();
			auto inner_elem = elem_type_info ? elem_type_info->element_type() : nullptr;
			if (inner_target && inner_elem) {
				return inner_target->base_type == inner_elem->base_type;
			}
			if (inner_target && !inner_elem) {
				return false;
			}
			return true;
		}

		if (target_type == script_value_type::jai_map_type && elem_type == script_value_type::jai_map_type) {
			auto target_key = element_type->key_type();
			auto target_val = element_type->value_type();
			auto elem_type_info = actual_element.get_type_info();
			auto elem_key = elem_type_info ? elem_type_info->key_type() : nullptr;
			auto elem_val = elem_type_info ? elem_type_info->value_type() : nullptr;
			if (target_key && elem_key && target_key->base_type != elem_key->base_type) {
				return false;
			}
			if (target_val && elem_val && target_val->base_type != elem_val->base_type) {
				return false;
			}
			return true;
		}

		return false;
	}

	inline constexpr const char* container_boundary_mismatch_text = "Cannot store {0} in {1}";
	inline constexpr const char* uint8_range_text = "Cannot store {0} in array<uint8> (0..255)";
	inline constexpr const char* int8_range_text = "Cannot store {0} in array<int8> (-128..127)";
	// a parallel worker cannot intern the value: the same text without it
	inline constexpr const char* uint8_range_text_slotless = "Cannot store a value outside 0..255 in array<uint8>";
	inline constexpr const char* int8_range_text_slotless = "Cannot store a value outside -128..127 in array<int8>";

	inline bool byte_element_fits(bool is_signed, script_int v) noexcept {
		return is_signed ? (v >= -128 && v <= 127) : (v >= 0 && v <= 255);
	}

	// The byte range kernel's verdict (array<uint8> / array<int8> elements), the one text every store path raises
	inline error_propagator byte_range_error(engine* eng, bool is_signed, script_int v) {
		auto* sym = eng ? engine_access::get_symbolizer(*eng) : nullptr;
		const auto code = make_error_code(runtime_error_code::array_element_type_mismatch);
		if (!sym || sym->frozen()) {
			return error_propagator{ code, is_signed ? int8_range_text_slotless : uint8_range_text_slotless, 0, 0 };
		}
		return error_propagator{ code, is_signed ? int8_range_text : uint8_range_text, sym->intern(std::to_string(v)), 0 };
	}

	struct container_boundary_outcome {
		std::optional<script_value> value;      // engaged on success
		std::string offending;                  // failure: the mismatched element/key/value type
		std::string expected;                   // failure: the target container's canonical name
		std::optional<error_propagator> error;  // failure already worded by a kernel: a float outside the int range, a nested boundary's verdict
	};

	inline container_boundary_outcome enforce_container_boundary(script_value value, const type_info_ptr& target_type, engine* eng);

	// The one text for an element that does not belong, interned the way the backends do
	JAI_NOINLINE inline error_propagator container_mismatch_error(engine* eng, const std::string& offending, const std::string& expected) {
		auto* sym = eng ? engine_access::get_symbolizer(*eng) : nullptr;
		return error_propagator{ make_error_code(runtime_error_code::array_element_type_mismatch), container_boundary_mismatch_text,
		                         sym ? sym->intern(offending) : 0, sym ? sym->intern(expected) : 0 };
	}

	// The element stores' mismatch texts, spelled out of line (the per-element store stays free of string temporaries)
	JAI_NOINLINE inline error_propagator element_kind_mismatch_error(engine* eng, const script_value& actual, const type_info& element_type) {
		return container_mismatch_error(eng, container_value_type_name(actual), element_type.canonical_name());
	}
	JAI_NOINLINE inline error_propagator byte_element_mismatch_error(engine* eng, const script_value& actual, const type_info& element_type) {
		return container_mismatch_error(eng, container_value_type_name(actual), "array<" + element_type.canonical_name() + ">");
	}

	// A typed container's element or key text: {0} the value's spelling, {1} the declared type's
	JAI_NOINLINE inline error_propagator element_type_error(string_symbolizer* sym, runtime_error_code code, std::string_view text,
	                                                        const script_value& value, const type_info_ptr& declared) {
		const uint64_t value_id = sym->intern(container_value_type_name(value));
		const uint64_t declared_id = sym->intern(type_info_display_name(declared));
		return error_propagator{make_error_code(code), text, value_id, declared_id};
	}

	inline constexpr std::string_view array_index_out_of_bounds_text = "Array index {0} out of bounds for array of size {1}";
	inline constexpr std::string_view string_index_out_of_bounds_text = "String index {0} out of bounds for string of size {1}";
	inline constexpr std::string_view array_index_out_of_bounds_slotless = "Array index out of bounds";
	inline constexpr std::string_view string_index_out_of_bounds_slotless = "String index out of bounds";
	// An index past the end: the numbers ride interned spellings; a parallel worker cannot intern them and gets the text without
	JAI_NOINLINE inline error_propagator index_out_of_bounds_error(string_symbolizer* sym, std::string_view text, std::string_view text_slotless,
	                                                               script_int index, size_t size) {
		const auto code = make_error_code(runtime_error_code::index_out_of_bounds);
		if (sym->frozen()) {   // a parallel region: the barrier interned the out-of-range indices its index writes can name
			const auto index_id = sym->find(std::to_string(index));
			const auto size_id = sym->find(std::to_string(size));
			if (index_id && size_id) { return error_propagator{code, text, *index_id, *size_id}; }
			return error_propagator{code, text_slotless, 0, 0};
		}
		const uint64_t index_id = sym->intern(std::to_string(index));
		const uint64_t size_id = sym->intern(std::to_string(size));
		return error_propagator{code, text, index_id, size_id};
	}

	inline constexpr std::string_view map_key_not_found_text = "Key {0} not found in map";
	inline constexpr std::string_view map_key_not_found_slotless = "Key not found in map";
	// A read of a key the map does not hold (value_text.cpp): a key of the wrong type for a typed map says that, any other key is
	// shown as a container shows it (a string quoted); a parallel worker cannot intern the text and gets the slotless form
	error_propagator map_key_missing_error(string_symbolizer* sym, const type_info* map_tag, const script_value& key);

	// The class tag a class-typed element stores: the object's own class (a Derived in an array<Base> stays a Derived)
	inline type_info_ptr object_element_value_tag(const script_value& object, engine* eng) {
		if (type_info_ptr own = object_element_own_tag(object, eng)) { return own; }
		const auto* holder = object.peek_object_holder();
		return holder ? type_info_ptr{engine_access::get_type_info_object(*eng, holder->type_id())} : type_info_ptr{};
	}

	// A handle stored into a class-typed element: the element takes a copy of the object the handle names, as
	// std::vector<T>{*h} does in C++ (a shared_ptr<T> element or an untyped one keeps the handle)
	JAI_NOINLINE inline script_value handle_pointee_element(const script_value& handle, engine* eng) {
		script_value out = handle.clone();
		out.set_type_info(object_element_value_tag(handle, eng));
		return out;
	}

	// The element boundary's numeric rows are the scalar slot's rows: a float into an int
	// element runs the same checked kernel (`int x = 1e100` and `array<int> x = [1e100]`
	// reject with one text), an int into a float element widens.
	inline checked_result<script_value> convert_container_element(engine* eng, const script_value& element, const type_info_ptr& element_type) {
		const script_value& actual_element = element.is_reference() ? element.deref() : element;

		auto actual_type_info = actual_element.get_type_info();
		if (actual_type_info && actual_type_info->base_type == script_value_type::jai_shared_ptr_type) {
			if (element_type && element_type->base_type == script_value_type::jai_weak_ptr_type &&
			    actual_element.raw_storage_index() != script_value::TYPEID_NULL) {
				return weak_element_from_shared(actual_element, eng);
			}
			if (element_type && element_type->base_type == script_value_type::jai_object_type &&
			    actual_element.raw_storage_index() == script_value::TYPEID_OBJECT) {
				return handle_pointee_element(actual_element, eng);
			}
			return actual_element;
		}

		if (!element_type || element_type->base_type == script_value_type::jai_any_type) {
			return actual_element.clone_element();
		}

		auto elem_type = actual_element.type();
		auto target_type = element_type->base_type;

		if (target_type == script_value_type::jai_array_type || target_type == script_value_type::jai_map_type) {
			// a nested container element: the same boundary one level down, reading the
			// source in place. A rebuild (inner literal -> typed node) IS the store's copy;
			// a node that already fits comes back as the same handle and gets the one copy
			// value semantics owe the store, exactly like a flat element.
			if (elem_type != target_type) {
				return element_kind_mismatch_error(eng, actual_element, *element_type);
			}
			auto inner = enforce_container_boundary(actual_element, element_type, eng);
			if (inner.error) { return *inner.error; }
			if (!inner.value) { return container_mismatch_error(eng, inner.offending, inner.expected); }
			const bool rebuilt = inner.value->raw_storage_index() != actual_element.raw_storage_index() ||
			                     (target_type == script_value_type::jai_array_type
			                          ? inner.value->unchecked_array_node() != actual_element.unchecked_array_node()
			                          : &inner.value->unchecked_as_map() != &actual_element.unchecked_as_map());
			return rebuilt ? std::move(*inner.value) : inner.value->clone_element();
		}

		if (target_type == script_value_type::jai_int_type && element_type->is_byte_element()) {
			// the int rows, then the kind's range
			script_int v = 0;
			if (elem_type == script_value_type::jai_int_type) {
				v = actual_element.unchecked_as_int();
			} else if (elem_type == script_value_type::jai_float_type) {
				auto converted = checked_float_to_integer(actual_element.unchecked_as_float());
				if (!converted) { return converted.error_value(); }
				v = converted.value();
			} else if (elem_type == script_value_type::jai_bool_type) {
				v = actual_element.unchecked_as_bool() ? 1 : 0;
			} else {
				return byte_element_mismatch_error(eng, actual_element, *element_type);
			}
			if (!byte_element_fits(element_type->is_signed, v)) { return byte_range_error(eng, element_type->is_signed, v); }
			return script_value(v, eng);
		}
		if (target_type == script_value_type::jai_int_type && elem_type == script_value_type::jai_float_type) {
			auto converted = checked_float_to_integer(actual_element.unchecked_as_float());
			if (!converted) { return converted.error_value(); }
			return script_value(converted.value(), eng);
		}
		if (target_type == script_value_type::jai_float_type && elem_type == script_value_type::jai_int_type) {
			return script_value(static_cast<script_float>(actual_element.unchecked_as_int()), eng);
		}
		if (target_type == script_value_type::jai_int_type && elem_type == script_value_type::jai_bool_type) {
			return script_value(static_cast<script_int>(actual_element.unchecked_as_bool() ? 1 : 0), eng);
		}
		if (target_type == script_value_type::jai_bool_type && elem_type == script_value_type::jai_int_type) {
			return script_value(actual_element.unchecked_as_int() != 0, eng);
		}
		if (target_type == script_value_type::jai_char_type && elem_type == script_value_type::jai_string_type) {
			auto converted = actual_element.raw_storage_index() == script_value::TYPEID_STRING ? char_of_text(actual_element.unchecked_as_string())
			                                                                                    : char_of_text(actual_element.to_string());
			if (!converted) { return converted.error_value(); }
			return script_value(converted.value(), eng);
		}
		if (target_type == script_value_type::jai_string_type && elem_type == script_value_type::jai_char_type) {
			return script_value(std::string(1, actual_element.unchecked_as_char()), eng);
		}
		if (target_type == script_value_type::jai_shared_ptr_type && elem_type == script_value_type::jai_object_type) {
			return actual_element;   // the handle rows' store: the object travels as the handle, its own tag
		}
		if (is_math_type(target_type) && is_math_type(elem_type)) {
			auto converted = math::convert(actual_element, target_type, eng);
			if (!converted) { return converted.error_value(); }
			script_value out = std::move(converted.value());
			out.set_type_info(element_type);
			return out;
		}
		if (target_type == script_value_type::jai_object_type && elem_type == script_value_type::jai_object_type) {
			// the stored copy carries its class, not the var tag it arrived under
			script_value out = actual_element.clone_element();
			const type_info* tag = out.get_type_info().get();
			if (!tag || tag->base_type == script_value_type::jai_any_type) {
				if (auto own = object_element_own_tag(actual_element, eng)) { out.set_type_info(std::move(own)); }
			}
			return out;
		}

		return actual_element.clone_element();
	}

	// A replacing element store: a prvalue class instance (uniquely held) moves in and is the stored object, carrying its
	// class as the object row's copy would; a prvalue array or map the element boundary leaves as it is moves in as the
	// handle the boundary hands back (the copy's node would hold the same elements); anything else takes the converting copy
	inline checked_result<script_value> convert_container_element_taking(engine* eng, script_value& element, const type_info_ptr& element_type) {
		if (element_type && element_type->base_type == script_value_type::jai_object_type &&
		    element.raw_storage_index() == script_value::TYPEID_OBJECT && !element.is_shared_ptr_type() && uniquely_held(element)) {
			const type_info* tag = element.get_type_info().get();
			if (!tag || tag->base_type == script_value_type::jai_any_type) {
				if (auto own = object_element_own_tag(element, eng)) { element.set_type_info(std::move(own)); }
			}
			JAI_CENSUS_F(move_hit, 7);
			return std::move(element);
		}
		if (element_type && (element_type->base_type == script_value_type::jai_array_type || element_type->base_type == script_value_type::jai_map_type) &&
		    element.type() == element_type->base_type && uniquely_held(element)) {
			auto inner = enforce_container_boundary(element, element_type, eng);
			if (!inner.error && inner.value) {
				script_value taken = std::move(*inner.value);
				element = script_value(std::monostate{}, eng);
				return taken;
			}
		}
		return convert_container_element(eng, element, element_type);
	}

	// engine.cpp: subclass admission walk
	bool class_derives_from(engine* eng, uint64_t derived_id, uint64_t base_id);

	inline bool byte_kind(script_array::kind_t k) noexcept {
		return k == script_array::kind_t::u8 || k == script_array::kind_t::i8;
	}

	// Kind <-> primitive element type (the representation rule's one table)
	inline script_array::kind_t kind_for_element(const type_info& element) {
		switch (element.base_type) {
			case script_value_type::jai_int_type:
				return !element.is_byte_element() ? script_array::kind_t::i64 : element.is_signed ? script_array::kind_t::i8 : script_array::kind_t::u8;
			case script_value_type::jai_float_type: return script_array::kind_t::f64;
			case script_value_type::jai_bool_type: return script_array::kind_t::b8;
			case script_value_type::jai_char_type: return script_array::kind_t::c8;
			case script_value_type::jai_string_type: return script_array::kind_t::str;
			case script_value_type::jai_object_type: return script_array::kind_t::obj;
			default: return script_array::vec_kind_of(element.base_type);
		}
	}
	inline script_value_type typed_kind_element_base(script_array::kind_t kind) {
		switch (kind) {
			case script_array::kind_t::i64:
			case script_array::kind_t::u8:
			case script_array::kind_t::i8: return script_value_type::jai_int_type;
			case script_array::kind_t::f64: return script_value_type::jai_float_type;
			case script_array::kind_t::b8: return script_value_type::jai_bool_type;
			case script_array::kind_t::c8: return script_value_type::jai_char_type;
			case script_array::kind_t::str: return script_value_type::jai_string_type;
			case script_array::kind_t::obj: return script_value_type::jai_object_type;
			case script_array::kind_t::hetero: return script_value_type::jai_any_type;
			default: return script_array::vec_value_type(kind);
		}
	}
	// An object buffer is homogeneous by EXACT class (the element rule today); the class
	// lives in the array value's tag, not in the kind
	inline bool same_object_class(const type_info_ptr& array_tag, const type_info_ptr& element_type, engine* eng = nullptr) {
		type_info_ptr src_elem = array_tag ? array_tag->element_type() : nullptr;
		return src_elem && element_type && src_elem->base_type == script_value_type::jai_object_type &&
		       (src_elem->type_name == element_type->type_name ||
		        class_derives_from(eng, class_tag_id(*src_elem), class_tag_id(*element_type)));
	}
	inline type_info* typed_kind_element_type(script_array::kind_t kind, engine* eng) {
		switch (kind) {
			case script_array::kind_t::i64: return engine_access::get_type_info_int(*eng);
			case script_array::kind_t::f64: return engine_access::get_type_info_float(*eng);
			case script_array::kind_t::b8: return engine_access::get_type_info_bool(*eng);
			case script_array::kind_t::c8: return engine_access::get_type_info_char(*eng);
			case script_array::kind_t::u8: return engine_access::get_type_info_uint8(*eng);
			case script_array::kind_t::i8: return engine_access::get_type_info_int8(*eng);
			case script_array::kind_t::str: return engine_access::get_type_info_string(*eng);
			default: return engine_access::get_type_info_math(*eng, script_array::vec_value_type(kind));
		}
	}

	// Uniform-literal inference: `auto a = [1, 2, 3]` and
	// `array<auto> a = [...]` infer their element type from a homogeneous primitive
	// literal and take that type's representation (array<int>/array<float> = raw buffer,
	// array<string>/bool/char = the locked tag). Empty, nested, object, or bound-primitive
	// literals answer null and stay dynamic. Callers have already validated homogeneity.
	inline type_info_ptr infer_uniform_array_type(const script_value& array_value, engine* eng) {
		if (!eng || array_value.raw_storage_index() != script_value::TYPEID_ARRAY) {
			return nullptr;
		}
		const script_array* node = array_value.unchecked_array_node();
		if (node->is_typed()) {
			if (node->kind() == script_array::kind_t::obj) { return array_value.get_type_info(); }   // the tag names the class
			return type_info_ptr{engine_access::get_type_info_array(*eng, typed_kind_element_type(node->kind(), eng))};
		}
		const auto& elems = node->values();
		if (elems.empty()) {
			return nullptr;
		}
		const size_t first = elems[0].deref().raw_storage_index();
		switch (first) {
			case script_value::TYPEID_INT:
			case script_value::TYPEID_FLOAT:
			case script_value::TYPEID_STRING:
			case script_value::TYPEID_BOOL:
			case script_value::TYPEID_CHAR:
				break;
			default:
				return nullptr;
		}
		for (const script_value& e : elems) {
			if (e.deref().raw_storage_index() != first) {
				return nullptr;
			}
		}
		type_info* element = nullptr;
		switch (first) {
			case script_value::TYPEID_INT: element = engine_access::get_type_info_int(*eng); break;
			case script_value::TYPEID_FLOAT: element = engine_access::get_type_info_float(*eng); break;
			case script_value::TYPEID_STRING: element = engine_access::get_type_info_string(*eng); break;
			case script_value::TYPEID_BOOL: element = engine_access::get_type_info_bool(*eng); break;
			default: element = engine_access::get_type_info_char(*eng); break;
		}
		return type_info_ptr{engine_access::get_type_info_array(*eng, element)};
	}

	// Map twin of the array rule: an `auto` key or value slot
	// (the parser leaves it null: plain `auto m`, `map<auto,V>`, `map<K,auto>`) infers its
	// type from a uniform primitive literal; an empty literal stays dynamic; mixed kinds
	// answer null with mixed_part set (1 = keys, 2 = values) so the caller raises. Spelled
	// `var` slots (base any) are never inferred. Callers already validated value
	// homogeneity for auto-value declarations.
	inline type_info_ptr infer_uniform_map_type(const script_value& map_value, type_info_ptr declared, engine* eng, int& mixed_part) {
		mixed_part = 0;
		if (!eng || map_value.raw_storage_index() != script_value::TYPEID_MAP) {
			return nullptr;
		}
		if (declared && !declared->is_map()) {
			return nullptr;   // `var m = {...}` (any) is not an auto slot: fully dynamic
		}
		const bool key_auto = !declared || !declared->key_type();
		const bool value_auto = !declared || !declared->value_type();
		if (!key_auto && !value_auto) {
			return nullptr;
		}
		const auto& entries = map_value.unchecked_as_map();
		if (entries.empty()) {
			return nullptr;
		}
		auto primitive_of = [&](const script_value& v, size_t& kind) -> bool {
			kind = v.deref().raw_storage_index();
			return kind == script_value::TYPEID_INT || kind == script_value::TYPEID_FLOAT || kind == script_value::TYPEID_STRING ||
			       kind == script_value::TYPEID_BOOL || kind == script_value::TYPEID_CHAR;
		};
		auto type_of_kind = [&](size_t kind) -> type_info* {
			switch (kind) {
				case script_value::TYPEID_INT: return engine_access::get_type_info_int(*eng);
				case script_value::TYPEID_FLOAT: return engine_access::get_type_info_float(*eng);
				case script_value::TYPEID_STRING: return engine_access::get_type_info_string(*eng);
				case script_value::TYPEID_BOOL: return engine_access::get_type_info_bool(*eng);
				default: return engine_access::get_type_info_char(*eng);
			}
		};
		type_info* key_type = key_auto ? nullptr : declared->key_type().get();
		type_info* value_type = value_auto ? nullptr : declared->value_type().get();
		if (key_auto) {
			size_t first = 0;
			if (primitive_of(entries.begin()->first, first)) {
				bool uniform = true;
				for (const auto& entry : entries) {
					size_t k = 0;
					if (!primitive_of(entry.first, k) || k != first) { uniform = false; break; }
				}
				if (!uniform) { mixed_part = 1; return nullptr; }
				key_type = type_of_kind(first);
			}
		}
		if (value_auto) {
			size_t first = 0;
			if (primitive_of(entries.begin()->second, first)) {
				bool uniform = true;
				for (const auto& entry : entries) {
					size_t k = 0;
					if (!primitive_of(entry.second, k) || k != first) { uniform = false; break; }
				}
				if (!uniform) { mixed_part = 2; return nullptr; }
				value_type = type_of_kind(first);
			}
		}
		if (!key_type && !value_type) {
			return nullptr;   // nothing primitive to infer: stays as declared (dynamic)
		}
		return type_info_ptr{engine_access::get_type_info_map(*eng, key_type ? key_type : engine_access::get_type_info_any(*eng),
		                                            value_type ? value_type : engine_access::get_type_info_any(*eng))};
	}

	// The one message both backends raise at a failed boundary (byte-identical errors).

	// A byte buffer on either side of an int, float or byte buffer: the element rows (int into a byte range-checked,
	// float through the checked float kernel first, a byte widening into int or float) on the raw payloads; any other
	// pair is the boxed loop's
	inline std::optional<container_boundary_outcome> convert_byte_buffer(const script_array& src, script_array::kind_t expected,
	                                                                    const type_info_ptr& element_type, const type_info_ptr& target_type, engine* eng) {
		using k = script_array::kind_t;
		const k sk = src.kind();
		const bool numeric_src = sk == k::i64 || sk == k::f64 || byte_kind(sk);
		const bool numeric_dst = expected == k::i64 || expected == k::f64 || byte_kind(expected);
		if (!numeric_src || !numeric_dst || sk == expected) { return std::nullopt; }
		const size_t count = src.size();
		container_boundary_outcome out;
		script_value rebuilt = script_value::make_array(element_type, eng);
		script_array& dest = *rebuilt.get_array_storage();
		auto byte_at = [&](size_t i) -> script_int {
			return sk == k::u8 ? static_cast<script_int>(src.bytes()[i]) : static_cast<script_int>(static_cast<int8_t>(src.bytes()[i]));
		};
		if (byte_kind(expected)) {
			const bool is_signed = expected == k::i8;
			auto& bytes = dest.bytes();
			bytes.resize(count);
			for (size_t i = 0; i < count; ++i) {
				script_int v = 0;
				if (sk == k::i64) {
					v = src.ints()[i];
				} else if (sk == k::f64) {
					auto converted = checked_float_to_integer(src.floats()[i]);
					if (!converted) { out.error = converted.error_value(); return out; }
					v = converted.value();
				} else {
					v = byte_at(i);
				}
				if (!byte_element_fits(is_signed, v)) { out.error = byte_range_error(eng, is_signed, v); return out; }
				bytes[i] = static_cast<uint8_t>(v);
			}
		} else if (expected == k::i64) {
			auto& ints = dest.ints();
			ints.resize(count);
			for (size_t i = 0; i < count; ++i) { ints[i] = byte_at(i); }
		} else {
			auto& floats = dest.floats();
			floats.resize(count);
			for (size_t i = 0; i < count; ++i) { floats[i] = static_cast<script_float>(byte_at(i)); }
		}
		rebuilt.set_type_info(target_type);
		out.value = std::move(rebuilt);
		return out;
	}

	inline container_boundary_outcome enforce_container_boundary(script_value value, const type_info_ptr& target_type, engine* eng) {
		container_boundary_outcome out;
		if (!target_type) {
			out.value = std::move(value);
			return out;
		}

		const auto target_base = target_type->base_type;
		if (target_base == script_value_type::jai_array_type &&
		    value.raw_storage_index() == script_value::TYPEID_ARRAY) {
			type_info_ptr element_type = target_type->element_type();
			const script_array* src = value.unchecked_array_node();
			if (!element_type || element_type->base_type == script_value_type::jai_any_type) {
				// open element type: allow-all. array<var> IS the script_value-vector form
				// (the representation rule), so a typed source reboxes into a fresh node
				// under the declared tag - its own buffer is untouched; a hetero source
				// passes through and keeps its own (possibly tighter) tag
				if (!element_type || !src->is_typed()) {
					out.value = std::move(value);
					return out;
				}
				const size_t count = src->size();
				script_value rebuilt = script_value::make_array(element_type, eng);
				auto& dest = *rebuilt.get_array_storage();
				dest.reserve(count);
				for (size_t i = 0; i < count; ++i) {
					dest.push(src->get(i, eng));
				}
				rebuilt.set_type_info(target_type);
				out.value = std::move(rebuilt);
				return out;
			}
			const auto target_elem_base = element_type->base_type;

			// The node kind this binding wants: raw buffers for the primitive element
			// types (the boundary kernel is THE typed-node producer)
			const script_array::kind_t expected_kind = kind_for_element(*element_type);

			// Proof by kind: a same-kind typed node cannot hold a mismatched element
			// (object buffers also need the same class: the kind alone says "objects")
			if (src->is_typed() && src->kind() == expected_kind &&
			    (expected_kind != script_array::kind_t::obj || same_object_class(value.get_type_info(), element_type, eng))) {
				value.set_type_info(target_type);
				out.value = std::move(value);
				return out;
			}

			if (byte_kind(expected_kind) || byte_kind(src->kind())) {
				if (auto direct = convert_byte_buffer(*src, expected_kind, element_type, target_type, eng)) { return std::move(*direct); }
			}

			bool needs_conversion = src->is_typed() || src->kind() != expected_kind;
			const size_t count = src->size();
			if (src->is_typed() && src->kind() != script_array::kind_t::obj) {
				// typed source elements are one primitive by construction: int/float cross-
				// convert into numeric targets, everything else is a mismatch
				const script_value_type src_elem = typed_kind_element_base(src->kind());
				const bool numeric_pair = (src_elem == script_value_type::jai_int_type || src_elem == script_value_type::jai_float_type) &&
				                          (target_elem_base == script_value_type::jai_int_type || target_elem_base == script_value_type::jai_float_type);
				const bool bool_int_pair = (src_elem == script_value_type::jai_bool_type && target_elem_base == script_value_type::jai_int_type) ||
				                           (src_elem == script_value_type::jai_int_type && target_elem_base == script_value_type::jai_bool_type);
				const bool math_pair = is_math_type(src_elem) && is_math_type(target_elem_base) && math::converts(src_elem, target_elem_base);
				const bool text_pair = (src_elem == script_value_type::jai_string_type && target_elem_base == script_value_type::jai_char_type) ||
				                       (src_elem == script_value_type::jai_char_type && target_elem_base == script_value_type::jai_string_type);
				if (!numeric_pair && !bool_int_pair && !math_pair && !text_pair && src_elem != target_elem_base) {
					out.offending = container_base_type_name(src_elem);
					out.expected = target_type->canonical_name();
					return out;
				}
			} else {
				// hetero elements by reference, object slots boxed: the same per-element check
				for (size_t i = 0; i < count; ++i) {
					script_value boxed(std::monostate{}, eng);
					const script_value* elem = nullptr;
					if (src->is_typed()) { boxed = src->get(i, eng); elem = &boxed; } else { elem = &src->values()[i]; }
					const script_value& element = *elem;
					const script_value& actual = element.is_reference() ? element.deref() : element;
					if (!container_element_compatible(actual, element_type, eng)) {
						out.offending = container_value_type_name(actual);
						out.expected = target_type->canonical_name();
						return out;
					}
					if (actual.type() != target_elem_base &&
					    (target_elem_base == script_value_type::jai_int_type ||
					     target_elem_base == script_value_type::jai_float_type ||
					     target_elem_base == script_value_type::jai_bool_type || is_math_type(target_elem_base))) {
						needs_conversion = true;
					}
					if ((target_elem_base == script_value_type::jai_weak_ptr_type || target_elem_base == script_value_type::jai_object_type) &&
					    actual.type() == script_value_type::jai_shared_ptr_type) {
						needs_conversion = true;   // a shared handle downgrades, or a class element copies the object it names
					}
					if (target_elem_base == script_value_type::jai_array_type || target_elem_base == script_value_type::jai_map_type) {
						// nested: the inner tag decides; an untagged or differently tagged inner
						// node is rebuilt through its own boundary
						if (actual.get_type_info().get() != element_type.get()) { needs_conversion = true; }
					}
				}
			}
			if (!needs_conversion) {
				value.set_type_info(target_type);
				out.value = std::move(value);
				return out;
			}
			script_value rebuilt = script_value::make_array(element_type, eng);
			auto& dest = *rebuilt.get_array_storage();
			dest.reserve(count);
			for (size_t i = 0; i < count; ++i) {
				// representation change only: objects move as-is (ownership - clone or share - is decided by the
				// assignment or binding site), and a handle copies the object it names into the class element
				if (expected_kind == script_array::kind_t::obj) {
					script_value element = src->get(i, eng);
					if (element.is_shared_ptr_type()) { element = handle_pointee_element(element, eng); }
					dest.push(std::move(element));
					continue;
				}
				auto converted = convert_container_element(eng, src->get(i, eng), element_type);
				if (!converted) {
					out.error = converted.error_value();
					return out;
				}
				dest.push(std::move(converted.value()));
			}
			rebuilt.set_type_info(target_type);
			out.value = std::move(rebuilt);
			return out;
		}

		if (target_base == script_value_type::jai_map_type &&
		    value.raw_storage_index() == script_value::TYPEID_MAP) {
			type_info_ptr key_type = target_type->key_type();
			type_info_ptr value_type = target_type->value_type();
			const bool key_open = !key_type || key_type->base_type == script_value_type::jai_any_type;
			const bool value_open = !value_type || value_type->base_type == script_value_type::jai_any_type;
			if (key_open && value_open) {
				out.value = std::move(value);
				return out;
			}
			const auto& entries = value.unchecked_as_map();
			bool needs_conversion = false;
			// the offending key or value, else null; keys never coerce numerically (converting 1.5 -> 1 could merge entries)
			auto mismatch = [&](const script_map::value_type& entry) -> const script_value* {
				const script_value& actual_key = entry.first.is_reference() ? entry.first.deref() : entry.first;
				if (!key_open && !map_key_kind_admits(actual_key.type(), key_type->base_type)) { return &actual_key; }
				const script_value& actual = entry.second.is_reference() ? entry.second.deref() : entry.second;
				if (!value_open && !container_element_compatible(actual, value_type, eng)) { return &actual; }
				return nullptr;
			};
			// the checks are pure: they run in the index's order, and only a failure walks the keys in order for the first one
			bool fits = true;
			entries.for_each_unordered([&](const script_map::value_type& entry) { if (fits && mismatch(entry)) { fits = false; } });
			if (!fits) {
				for (const auto& entry : entries) {
					if (const script_value* bad = mismatch(entry)) {
						out.offending = container_value_type_name(*bad);
						out.expected = target_type->canonical_name();
						return out;
					}
				}
			}
			entries.for_each_unordered([&](const script_map::value_type& entry) {
				if (!key_open && (entry.first.is_reference() ? entry.first.deref() : entry.first).type() != key_type->base_type) {
					needs_conversion = true;   // a char key of a string map, or the reverse, stores in the declared kind
				}
				if (!value_open) {
					const script_value& actual = entry.second.is_reference() ? entry.second.deref() : entry.second;
					const auto value_base = value_type->base_type;
					if (actual.type() != value_base &&
					    (value_base == script_value_type::jai_int_type ||
					     value_base == script_value_type::jai_float_type ||
					     value_base == script_value_type::jai_bool_type || value_base == script_value_type::jai_char_type ||
					     value_base == script_value_type::jai_string_type || is_math_type(value_base))) {
						needs_conversion = true;
					}
					if ((value_base == script_value_type::jai_weak_ptr_type || value_base == script_value_type::jai_object_type) &&
					    actual.type() == script_value_type::jai_shared_ptr_type) {
						needs_conversion = true;   // a shared handle downgrades, or a class value copies the object it names
					}
					if ((value_base == script_value_type::jai_array_type || value_base == script_value_type::jai_map_type) &&
					    actual.get_type_info().get() != value_type.get()) {
						needs_conversion = true;   // nested value: rebuilt through its own boundary below
					}
				}
			});
			if (!needs_conversion) {
				value.set_type_info(target_type);
				out.value = std::move(value);
				return out;
			}
			script_value rebuilt = script_value::make_map(key_type, value_type, eng);
			auto& dest = rebuilt.get_map_storage();
			for (const auto& entry : entries) {
				const script_value& actual_key = entry.first.is_reference() ? entry.first.deref() : entry.first;
				auto converted = convert_container_element(eng, entry.second, value_type);
				if (!converted) {
					out.error = converted.error_value();
					return out;
				}
				if (!key_open && actual_key.type() != key_type->base_type) {
					auto key = map_key_in_kind(actual_key, key_type->base_type, eng);
					if (!key) {
						out.error = key.error_value();
						return out;
					}
					dest->emplace(std::move(key.value()), std::move(converted.value()));
					continue;
				}
				dest->emplace(actual_key.clone(), std::move(converted.value()));
			}
			rebuilt.set_type_info(target_type);
			out.value = std::move(rebuilt);
			return out;
		}

		// not the shapes this kernel guards (incl. reference/borrow-backed values):
		// unchanged, exactly today's behavior
		out.value = std::move(value);
		return out;
	}

	template <class Symbolizer>
	checked_result<void> validate_container_homogeneous(const script_value& container, const arity_count_symbols& counts, const Symbolizer& symbols);

	// The entries of an auto container literal hold one kind (and one class) at every depth; a raw buffer is homogeneous
	// by construction
	template <bool Map, class Entries, class Symbolizer>
	checked_result<void> homogeneous_entries(const Entries& entries, const arity_count_symbols& counts, const Symbolizer& symbols) {
		const runtime_error_code code = Map ? runtime_error_code::map_value_type_mismatch : runtime_error_code::array_element_type_mismatch;
		script_value_type kind = script_value_type::jai_null_type;
		type_info_ptr class_info;   // keeps class_name's text alive
		std::string_view class_name;
		size_t index = 0;
		for (const auto& entry : entries) {
			const script_value* raw;
			if constexpr (Map) { raw = &entry.second; } else { raw = &entry; }
			const script_value& value = raw->is_reference() ? raw->deref() : *raw;
			if (index == 0) {
				kind = value.type();
				if (kind == script_value_type::jai_object_type) {
					if (auto info = value.get_type_info()) { class_info = std::move(info); class_name = class_info->type_name; }
				}
			} else {
				if (value.type() != kind) {
					return homogeneity_error(code, Map ? auto_map_values : auto_array_elements, Map ? auto_map_values_at : auto_array_elements_at, index, counts, symbols);
				}
				if (kind == script_value_type::jai_object_type && !class_name.empty()) {
					auto info = value.get_type_info();
					if ((info ? std::string_view(info->type_name) : std::string_view()) != class_name) {
						return homogeneity_error(code, Map ? auto_map_classes : auto_array_classes, Map ? auto_map_classes_at : auto_array_classes_at, index, counts, symbols);
					}
				}
			}
			if (auto nested = validate_container_homogeneous(value, counts, symbols); !nested) { return nested; }
			++index;
		}
		return {};
	}

	template <class Symbolizer>
	checked_result<void> validate_container_homogeneous(const script_value& container, const arity_count_symbols& counts, const Symbolizer& symbols) {
		JAI_CENSUS(container_validate);
		if (container.is_array()) {
			if (container.unchecked_array_node()->is_typed()) { return {}; }
			return homogeneous_entries<false>(container.as_array(), counts, symbols);
		}
		if (container.is_map()) {
			return homogeneous_entries<true>(container.as_map(), counts, symbols);
		}
		return {};
	}

} // namespace jai::detail
