#pragma once

#ifndef __JAISCRIPT_DETAIL_VALUE_TEXT_HPP__
#define __JAISCRIPT_DETAIL_VALUE_TEXT_HPP__

// The display text of a value: the one rule print, to_string, string +, template splices and format's {} share
// (docs/site/guide/09-containers-strings.html "Printing values"). Library translation units only.

#include <jaiscript/core/value.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

namespace jai::detail {

	// What an object's class said for it: its to_string() text was appended (spoke), it has none and the walk shows its
	// fields (silent), or the method threw and the throw is in flight (unwinding: the walk stops)
	enum class own_text_verdict : uint8_t { spoke, silent, unwinding };

	// A backend's door to an object's to_string(). pin holds every container and object the walk is inside, since script
	// may drop the caller's own hold on them; a parallel worker never pins (its methods write nothing shared)
	struct own_text_hook {
		checked_result<own_text_verdict> (*speak)(void* backend, const script_value& object, std::string& out) = nullptr;
		void* backend = nullptr;
		bool pin = false;
	};

	// True when v's display text is its scalar text (no walk, no script): everything but containers, objects (a shared one too), handles and functions
	inline bool plain_text_value(const script_value& v) {
		constexpr uint32_t walked = (1u << script_value::TYPEID_ARRAY) | (1u << script_value::TYPEID_MAP) |
		                            (1u << script_value::TYPEID_OBJECT) | (1u << script_value::TYPEID_FUNCTION) |
		                            (1u << script_value::TYPEID_WEAK_PTR) | (1u << script_value::TYPEID_REFERENCE) |
		                            (1u << script_value::TYPEID_PARALLEL_BORROW);
		const size_t index = v.raw_storage_index();
		if (index >= 32 || !((walked >> index) & 1u)) { return true; }
		return index == script_value::TYPEID_REFERENCE && plain_text_value(v.deref());
	}

	// A top-level string is its bytes; inside a container a string or char is quoted and escaped as a literal; arrays and
	// maps show their elements in their order; an instance answers through hook (its class's to_string()), else
	// ClassName{field: value, ...} with base fields first; a handle shows its pointee, an expired weak_ptr null, an
	// instance already on the path <cycle ClassName>. Walks on an explicit stack; past limit bytes the text ends in "..."
	checked_result<std::string> value_text(const script_value& v, const own_text_hook& hook, size_t limit = SIZE_MAX);

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_VALUE_TEXT_HPP__
