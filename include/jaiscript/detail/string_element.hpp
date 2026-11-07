#pragma once

#ifndef __JAISCRIPT_DETAIL_STRING_ELEMENT_HPP__
#define __JAISCRIPT_DETAIL_STRING_ELEMENT_HPP__

// A string subscript as a store target (s[i] = v, s[i] op= v), shared VERBATIM by both
// backends: the target read mints a STRING-ELEMENT reference pinning the string's storage
// (the base's own reference, a field or property reference, or the boxed variable cell)
// and the store replaces character i with the stored text - a char is one byte, a string
// or utf8_string its bytes, so s[2] = "ll" grows the string. A string counts bytes, a
// utf8_string characters. The write goes through the copy-on-write gates, so a copy of
// the string never sees it.

#include <jaiscript/core/value.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/container_enforce.hpp>
#include <jaiscript/detail/utf8_text.hpp>

namespace jai::detail {

	JAI_NOINLINE inline checked_result<void> string_element_out_of_bounds(size_t index, size_t size, engine* eng) {
		string_symbolizer* symbolizer = engine_access::get_symbolizer(*eng);
		if (symbolizer->frozen()) {   // a parallel worker cannot intern the numbers: the same text without them
			return checked_result<void>(make_error_code(runtime_error_code::index_out_of_bounds), "String index out of bounds");
		}
		return checked_result<void>(make_error_code(runtime_error_code::index_out_of_bounds),
			"String index {0} out of bounds for string of size {1}",
			symbolizer->intern(std::to_string(index)), symbolizer->intern(std::to_string(size)));
	}

	JAI_NOINLINE inline checked_result<void> string_element_kind_error(const script_value& value, engine* eng) {
		string_symbolizer* symbolizer = engine_access::get_symbolizer(*eng);
		const std::string spelled = container_value_type_name(value);
		const auto known = symbolizer->frozen() ? symbolizer->find(spelled) : std::optional<uint64_t>(symbolizer->intern(spelled));
		if (!known) {
			return checked_result<void>(make_error_code(runtime_error_code::array_element_type_mismatch),
				"Cannot assign to a string character (it takes a char or a string)");
		}
		return checked_result<void>(make_error_code(runtime_error_code::array_element_type_mismatch),
			"Cannot assign {0} to a string character (it takes a char or a string)", *known);
	}

	// Character `index` of `storage` becomes the text of `value_in`
	inline checked_result<void> replace_string_element(script_value& storage, size_t index, const script_value& value_in, engine* eng) {
		const script_value& value = value_in.deref();
		std::string text;
		switch (value.raw_storage_index()) {
			case script_value::TYPEID_CHAR: text.assign(1, static_cast<char>(value.unchecked_as_char())); break;
			case script_value::TYPEID_STRING: text = value.unchecked_as_string(); break;
			case script_value::TYPEID_UTF8_STRING: text = value.unchecked_as_utf8().bytes; break;
			default:
				if (value.is_char()) { text.assign(1, static_cast<char>(value.as_char())); break; }
				if (value.is_string()) { text = value.as_string(); break; }
				return string_element_kind_error(value, eng);
		}
		if (storage.raw_storage_index() == script_value::TYPEID_UTF8_STRING) {
			const bool cache = !engine_access::parallel_region_active(*eng);
			const script_utf8& view = storage.unchecked_as_utf8();
			const size_t len = utf8::length(view, cache);
			if (index >= len) { return string_element_out_of_bounds(index, len, eng); }
			const size_t at = utf8::byte_of(view, index, cache);
			const size_t width = utf8::decode(view.bytes, at).length;
			storage.unchecked_as_utf8_ref().bytes.replace(at, width, text);
			return {};
		}
		if (!storage.is_string()) {
			return checked_result<void>(make_error_code(runtime_error_code::invalid_reference), "Invalid reference in assignment");
		}
		const size_t size = storage.raw_storage_index() == script_value::TYPEID_STRING ? storage.unchecked_as_string().size()
		                                                                                 : storage.unchecked_as_string_ref(eng).size();
		if (index >= size) { return string_element_out_of_bounds(index, size, eng); }
		script_string& bytes = storage.unchecked_as_string_ref(eng);
		if (text.size() == 1) {
			bytes[index] = text[0];
		} else {
			bytes.replace(index, 1, text);
		}
		return {};
	}

	// The store through a STRING-ELEMENT reference: the pin's final referent takes the
	// replacement, and a scratch-backed pin (a typed array<string> element, a C++ property)
	// writes its scratch back
	template <class Holder>   // script_value::reference_holder (private: deduced, never named)
	checked_result<void> store_string_element(Holder& holder, const script_value& value, engine* eng) {
		script_value pin = script_value::make_string_element_pin(holder, eng);
		script_value* storage = &pin.deref();
		while (storage->is_reference()) { storage = &storage->deref(); }
		auto replaced = replace_string_element(*storage, holder.container_index, value, eng);
		if (!replaced) { return replaced; }
		if (auto* scratch = pin.scratch_ref_holder()) { scratch->commit_scratch(); }
		return {};
	}

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_STRING_ELEMENT_HPP__
