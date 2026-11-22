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
	extends_base,
	strict_equal,
	strict_not_equal,
	generator_function,
	spread,
	template_prefix,
	dynamic_pointer_cast_call,
	struct_declaration,
	pointer_type,
	typedef_declaration,
	using_alias,
	unsigned_type,
	long_type,
	short_type,
	std_namespace,
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
		case foreign_word::template_prefix:
			return "JaiScript has no 'template<...>' prefix: a generic lists its type parameters after its name, class Stack<T> { ... } or T largest<T>(array<T> xs) { ... }";
		case foreign_word::dynamic_pointer_cast_call:
			return "JaiScript has no 'dynamic_pointer_cast': dynamic_cast<T>(x) is x as a shared_ptr<T>, null when it is not one";
		case foreign_word::struct_declaration:
			return "JaiScript has no 'struct': write class Name { ... }; its members start public";
		case foreign_word::pointer_type:
			return "JaiScript has no pointer types: hold a value as T x, share an object as shared_ptr<T>, and pass one to change it as T&";
		case foreign_word::typedef_declaration:
			return "JaiScript has no 'typedef': write the type itself, or make a class that holds it";
		case foreign_word::using_alias:
			return "JaiScript has no type aliases ('using X = T'): write the type itself, or make a class that holds it";
		case foreign_word::unsigned_type:
			return "JaiScript has no 'unsigned': int is a signed 64-bit integer, and array<uint8> holds bytes";
		case foreign_word::long_type:
			return "JaiScript has no 'long': int is already 64 bits";
		case foreign_word::short_type:
			return "JaiScript has no 'short': write int";
		case foreign_word::std_namespace:
			return "JaiScript has no 'std::': write array<T> for vector, map<K, V> for map, string for string and print(...) for cout";
	}
	return {};
}

inline constexpr std::string_view foreign_define_text =
	"JaiScript has no '#define': write const int N = 4; for a constant and a function for a macro";

// A C++ container or string method a builtin spells differently; empty for every other name. receiver is the builtin's type name.
inline constexpr std::string_view foreign_member_text(std::string_view receiver, std::string_view member) noexcept {
	if (receiver.substr(0, 5) == "array") {
		if (member == "push_back") { return "write a.push(v)"; }
		if (member == "pop_back") { return "write a.pop()"; }
		if (member == "emplace_back") { return "construct the value and push it: a.push(T(args))"; }
	}
	if (member == "c_str" && (receiver == "string" || receiver == "utf8_string")) { return "a string is already its text: pass s itself"; }
	return {};
}

} // namespace jai::detail
