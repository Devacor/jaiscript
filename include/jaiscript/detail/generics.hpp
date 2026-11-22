#pragma once

#ifndef __JAISCRIPT_DETAIL_GENERICS_HPP__
#define __JAISCRIPT_DETAIL_GENERICS_HPP__

// Generics (docs/site/guide/21-generics.html). A generic is a declaration with type parameters, kept as its tokens; an instance
// is those tokens with each parameter replaced by its argument, parsed as ordinary program text and declared in the unit that
// uses it. Both backends see ordinary classes and functions: the one rule they add is the stamp (generic_instance_current).

#include "lexer.hpp"
#include <jaiscript/detail/member_display_name.hpp>
#include <jaiscript/core/type_info.hpp>
#include <jaiscript/core/types.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace jai {
	class class_decl;
	class generic_decl;
	class string_symbolizer;
}

namespace jai::detail {

	// instantiation nests at most this deep (class A<T> { A<array<T>> next; } is an error, never a hang)
	inline constexpr size_t k_generic_depth_limit = 64;
	// an instance's name is at most this long: a chain whose instances multiply per level (Grow<Pair<T, T>>) ends in a few levels
	inline constexpr size_t k_generic_name_limit = 4096;
	// one use in a unit's own text makes at most this many instances, those its instances' text names counted: a chain that
	// branches per level (Tree<L<T>> beside Tree<R<T>>) ends before its count does the damage
	inline constexpr size_t k_generic_instance_limit = 4096;

	// A type parameter: its name and, for `T : Base`, the bound's tokens
	struct generic_parameter {
		std::string_view name;   // symbolizer storage
		uint64_t name_id = 0;
		std::vector<token> bound;
		source_location location;
	};

	// A generic function's value parameter as its declaration spells it: a type parameter reads as a class of its name
	struct generic_value_parameter {
		type_info_ptr type;   // null for auto
		bool is_reference = false;
		bool has_default = false;
	};

	enum class generic_kind : uint8_t { class_type, function, method };

	struct generic_definition {
		generic_kind kind = generic_kind::class_type;
		std::string_view name;   // as declared: Stack, largest, json
		std::string space;       // the namespace that declares it: "" at program scope, else "a::b"
		std::string owner;       // a method's class, by its qualified name
		bool is_static = false;  // a static method
		uint8_t visibility = 0;  // a method's: class_decl::member_visibility
		std::vector<generic_parameter> parameters;
		std::vector<token> tokens;   // the declaration, from its head to its closing brace
		uint32_t name_token = 0;     // tokens[name_token] is the name; tokens[name_token + 1 .. list_end) its parameter list, brackets included
		uint32_t list_end = 0;
		uint64_t stamp = 0;          // what an instance's definition is compared by: the tokens and where they stand
		uint8_t check_mode = 0;      // the [[warn_mode]] / [[strict_mode]] region it is declared in, which its instances check under
		std::vector<generic_value_parameter> values;   // a function's or a method's parameters
		// a class's members by name (its fields and statics; its methods) and its bases without their type arguments: what a
		// '<' after a name reads through where a class derives from an instance (detail/less_than.hpp)
		std::vector<std::string_view> member_variables;   // symbolizer storage
		std::vector<std::string_view> member_functions;
		std::vector<std::string> bases;
		source_location location;    // the name's
		// the stamp its file's text was loaded with (the engine sets it at every load of that text); unset = the host
		// supplied the text, and the engine never read the file
		mutable uint64_t file_mtime = 0, file_size = 0;
		mutable bool file_stamped = false;

		// "Stack", "shop::Stack", "wire::decode", "Request::json"
		std::string qualified_name() const {
			std::string result = kind == generic_kind::method ? owner : space;
			if (!result.empty()) { result += "::"; }
			result += name;
			return result;
		}
		size_t required_values() const {
			size_t n = 0;
			for (const auto& v : values) { if (!v.has_default) { ++n; } }
			return n;
		}
	};
	using generic_definition_ptr = std::shared_ptr<const generic_definition>;
	// The declaration is written `override` (string f<T>(T x) override { ... }): it replaces a namespace's function of its
	// name and value parameters that another unit declared, as a plain function's does
	bool generic_declared_override(const generic_definition& definition);

	// What identifies an instance: its name and the declaration it is made from. Two generic functions (or methods) of a name
	// that take the same type arguments are two instances of that one name, overloads of it as two functions written by hand
	// are, so a function's or a method's instance carries its definition's stamp. A class's name is one class: its stamp here is 0.
	struct generic_instance_id {
		std::string_view name;
		uint64_t stamp = 0;
		friend bool operator==(const generic_instance_id&, const generic_instance_id&) = default;
	};
	struct generic_instance_id_hash {
		size_t operator()(const generic_instance_id& id) const noexcept { return std::hash<std::string_view>()(id.name) ^ (static_cast<size_t>(id.stamp) * 0x9e3779b97f4a7c15ull); }
	};
	// the stamp a definition's instances are identified by (generic_instance_id)
	inline uint64_t generic_instance_stamp(const generic_definition& definition) { return definition.kind == generic_kind::class_type ? 0 : definition.stamp; }

	// An instance a check found the unit to need (check_options::generic_requests): the engine adds its request to the unit's others
	struct generic_request {
		std::string name;      // the instance's qualified name
		std::string generic;   // the generic's
		std::vector<type_info_ptr> arguments;
		generic_definition_ptr definition;
		source_location at;
		std::string within;    // the instance whose text asked
		uint64_t within_stamp = 0;   // and its stamp (generic_instance_id)
	};

	// An instance the engine holds that a unit's declaration of its generic cannot make again (docs/site/guide/21-generics.html
	// "Declaring it again"): the instance keeps the declaration it has and the unit's check warns, naming it and the reason
	struct generic_kept_instance {
		std::string name;      // the instance's qualified name
		std::string generic;   // the generic's
		std::string reason;    // the error that keeps the declaration from making it
		source_location at;    // the declaration
		uint64_t stamp = 0;    // the instance's (generic_instance_id): 0 for a class's
	};

	// What a name that takes no type arguments is: Name<Args> on it fails the load once, wherever a type can stand
	enum class plain_type_kind : uint8_t { none, script_class, host_class, enumeration, builtin, type_parameter };
	// int, float, string, bool, char, var, utf8_string, double and the math types (math::vec2)
	bool builtin_type_name(std::string_view name);
	// "'Hero' takes no type arguments: it is not a generic class", and for a host class "it is a host class, not a host
	// template", for an enum "it is an enum", for a builtin "it is a builtin type", for a generic's type parameter "it is a
	// type parameter of 'Stack'" (`generic` names it)
	std::string no_type_arguments_text(std::string_view name, plain_type_kind kind, std::string_view generic = {});

	// an instance's name as a note or a chain of instances shows it: one longer than 80 characters keeps its two ends
	std::string shown_instance_name(std::string_view name);
	// "'A<int>' needs 'A<array<int>>' needs 'A<array<array<int>>>'": the first links of the chain that asked for `request`, from
	// the unit's own use (`askers` runs outward from the request's asker); `more` when the chain goes on past them
	std::string instance_chain_text(const std::vector<std::string_view>& askers, std::string_view request, bool& more);

	// Two declarations of one generic: the same kind, as many type parameters, and value parameters of the same types (a type
	// parameter matching the one at its place: largest<T>(array<T>) and largest<U>(array<U>) are one declaration)
	bool same_generic_signature(const generic_definition& a, const generic_definition& b);

	// The stamp of a definition's tokens: kinds, spellings, literal values, lines and columns
	uint64_t generic_stamp(const std::vector<token>& tokens);

	// A definition keeps its tokens after the lexer that made them is gone, and a literal's lexeme points into that lexer's copy
	// of the source: every kept spelling moves into the symbolizer, so each lexeme is symbolizer storage or a static string as a
	// token's lexeme must be (lexer.hpp), for the instances, the stamps and the jaibite writer that read them later
	void own_token_text(generic_definition& definition, string_symbolizer& symbols);

	// A type as the tokens that spell it where a type parameter stood (each at `at`): `shop::Stack<int>` is `shop` `::` and the one
	// name `Stack<int>`, the class an instance declares. False for a type no argument may be (auto, void, a reference).
	// With `read`, the type as an instance reads it where the parameter stands on its own: a sized number (a name in `read`, the
	// uint8 or int8 of an array's element) is the int or float a value of it is; array<uint8> itself keeps its element (`element`:
	// the type stands as one).
	bool generic_argument_tokens(const type_info& type, const source_location& at, string_symbolizer& symbols, std::vector<token>& out,
	                             const std::vector<std::string_view>* read = nullptr, bool element = false);

	// What an instance's text takes from the host to spell a sized number a host template is bound with (Box<int32>, Box<float32>:
	// jai::type_name_helper's spellings, sized_number_kind). Such a name is a type only as a host template's argument, so a
	// parameter keeps it there (Box<T> is Box<int32>) and reads as int or float anywhere else (T x; array<T>; a result).
	struct generic_spelling {
		const std::unordered_set<std::string>* templates = nullptr;   // the host's templates, by name
		std::vector<std::string_view> sized;                          // the arguments' names that are sized numbers, no class having taken one
	};
	// Whether the parameter stands anywhere in the definition but as a host template's argument: an instance then reads its
	// argument as a script type
	bool generic_parameter_read(const generic_definition& definition, size_t parameter, const std::unordered_set<std::string>& templates);

	// The instance's name: template_instance_name over the generic's qualified name and each argument's canonical spelling
	std::string generic_instance_name(std::string_view generic, const std::vector<type_info_ptr>& arguments);

	// An instance's text: the definition's tokens with each parameter replaced by its argument and the declared name by
	// `instance` (the name without its namespace), wrapped in the definition's namespace or, for a method, its class; eof ends it
	std::vector<token> generic_instance_tokens(const generic_definition& definition, const std::vector<type_info_ptr>& arguments,
	                                           std::string_view instance, string_symbolizer& symbols, const generic_spelling* host = nullptr);

	// What a generic's declaration runs (generic_decl::marker): made once, null for a method's generic
	std::shared_ptr<class_decl> generic_marker(const generic_decl& declared, string_symbolizer& symbols);

	// `T : Base` with the arguments in place: the bound's tokens (empty when the parameter has none)
	std::vector<token> generic_bound_tokens(const generic_definition& definition, size_t parameter, const std::vector<type_info_ptr>& arguments,
	                                        string_symbolizer& symbols, const generic_spelling* host = nullptr);

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_GENERICS_HPP__
