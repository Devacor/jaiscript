#pragma once

#include "ast.hpp"
#include "string_symbolizer.hpp"
#include <jaiscript/core/checked_result.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <string>

namespace jai::detail {

// A plain declaration of a name an inline declaration created is refused; the text names the
// declaration it should be, its type as written (var: none; a destructuring name: auto)
inline std::string inline_spelling(const variable_decl* decl, std::string_view name) {
	std::string type;
	if (!decl || decl->auto_spelled || (!decl->type && !decl->function_spelling)) { type = "auto"; }
	else if (decl->function_spelling) { type = "function"; }
	else if (decl->type->base_type != script_value_type::jai_any_type) { type = decl->type->canonical_name(); }
	return "inline " + (type.empty() ? std::string() : type + " ") + std::string(name) + ";";
}

inline std::string inline_redefinition_text(const variable_decl* decl, std::string_view name) {
	return "'" + std::string(name) + "' is already defined inline; declare it '" + inline_spelling(decl, name) + "' to share it";
}

inline checked_result<void> inline_redefinition(string_symbolizer& symbols, uint64_t name_id, const variable_decl* decl) {
	return checked_result<void>(make_error_code(runtime_error_code::invalid_operation),
		"'{0}' is already defined inline; declare it '{1}' to share it", name_id,
		symbols.intern(inline_spelling(decl, symbols.get_string(name_id))));
}

// The reverse: an inline declaration of a name a plain declaration of another unit defined (a file by its path, a
// literal as <script>)
inline checked_result<void> inline_plain_conflict(string_symbolizer& symbols, uint64_t name_id, std::string_view unit) {
	return checked_result<void>(make_error_code(runtime_error_code::invalid_operation),
		"'{0}' is already defined (not inline) in {1}; declare every definition 'inline' to share it", name_id, symbols.intern(unit));
}

}
