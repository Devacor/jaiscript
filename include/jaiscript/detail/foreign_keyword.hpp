#pragma once

#include <string_view>

namespace jai::detail {

// Words and spellings other languages use that JaiScript does not: the parser names the JaiScript spelling instead of
// a generic parse error. Each is recognized only where it would otherwise fail
// to parse, so a script that uses one of these words as a name keeps working.
enum class foreign_word : unsigned char {
	const_method,
	const_return,
	const_type_argument,
	pointer_to_const,
	const_cast_call,
	constexpr_declaration,
	mutable_field,
	mutable_lambda,
	const_misplaced,
	virtual_method,
	delete_statement,
	new_array,
	new_without_arguments,
	goto_statement,
	address_of,
	typed_catch,
	extends_base,
	strict_equal,
	strict_not_equal,
	generator_function,
	spread,
};

inline constexpr std::string_view foreign_word_text(foreign_word w) noexcept {
	switch (w) {
		case foreign_word::const_method:
			return "JaiScript has no const methods: a method that writes no field already works on a const object";
		case foreign_word::const_return:
			return "JaiScript has no const return types: return by value (the caller gets a copy) or by T&";
		case foreign_word::const_type_argument:
			return "const qualifies a declaration, not a type: write const array<int> xs";
		case foreign_word::pointer_to_const:
			return "JaiScript has no pointer to const: const shared_ptr<T> keeps the handle from rebinding, and the object stays writable";
		case foreign_word::const_cast_call:
			return "JaiScript has no 'const_cast': a const stays const; copy it (auto x = c) for a writable value";
		case foreign_word::constexpr_declaration:
			return "JaiScript has no 'constexpr': a const whose initializer is a constant expression is folded at compile time: const int N = 4;";
		case foreign_word::mutable_field:
			return "JaiScript has no 'mutable' fields: every field of a const object is read-only; keep state that must change behind a shared_ptr field";
		case foreign_word::mutable_lambda:
			return "a lambda takes no 'mutable': its copied captures are already its own, writable and kept between calls";
		case foreign_word::const_misplaced:
			return "'const' starts a declaration: const int x = 5;";
		case foreign_word::virtual_method:
			return "JaiScript has no 'virtual': every method dispatches dynamically; mark the subclass's method 'override'";
		case foreign_word::delete_statement:
			return "JaiScript has no 'delete': an object is freed when its last owner lets go; set the handle to null to release it";
		case foreign_word::new_array:
			return "'new' makes one shared object (new T(args)); an array is a value: array<T> a = [];";
		case foreign_word::new_without_arguments:
			return "'new T' needs its constructor's parentheses: new T() or new T(args)";
		case foreign_word::goto_statement:
			return "JaiScript has no 'goto': use break, continue, return or a flag";
		case foreign_word::address_of:
			return "JaiScript has no address-of '&x': bind a reference with auto& r = x, or pass x to a T& parameter";
		case foreign_word::typed_catch:
			return "catch takes one name and no type: catch (e) { ... }; e.value is what was thrown and type_of(e.value) its type";
		case foreign_word::extends_base:
			return "JaiScript has no 'extends': derive with class Name : Base";
		case foreign_word::strict_equal:
			return "JaiScript has no '===': == never converts between types, so a == b already compares without coercion";
		case foreign_word::strict_not_equal:
			return "JaiScript has no '!==': != never converts between types, so a != b already compares without coercion";
		case foreign_word::generator_function:
			return "JaiScript has no 'function*': a generator is a coroutine, coroutine name(...) { yield v; }";
		case foreign_word::spread:
			return "JaiScript has no spread '...': join arrays with array::concat(a, b) (a.concat(b) appends in place), and pass arguments one by one";
	}
	return {};
}

} // namespace jai::detail
