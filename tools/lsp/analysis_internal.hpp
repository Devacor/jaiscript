#pragma once

#include "analysis.hpp"

#include <jaiscript/core/class_definition.hpp>
#include <jaiscript/core/type_info.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace jai_lsp {

inline constexpr std::string_view k_placeholder = "__jai_lsp_complete__";

// What navigation needs to know about an expression's static type
struct nav_type {
	enum kind_t : uint8_t { unknown, object, class_ref, enum_ref, array, map, string, weak, function, scalar } kind = unknown;
	std::string cls;
	const jai::type_info* ti = nullptr;

	bool known() const { return kind != unknown; }
	static nav_type of_object(std::string_view c) { nav_type t; t.kind = c.empty() ? unknown : object; t.cls = std::string(c); return t; }
	static nav_type of_class(std::string_view c) { nav_type t; t.kind = class_ref; t.cls = std::string(c); return t; }
	static nav_type of_enum(std::string_view c) { nav_type t; t.kind = enum_ref; t.cls = std::string(c); return t; }
};

inline nav_type nav_from_type_info(const jai::type_info* ti) {
	nav_type t;
	if (!ti) { return t; }
	using vt = jai::script_value_type;
	switch (ti->base_type) {
		case vt::jai_reference_type:
			return nav_from_type_info(ti->type_params.empty() ? nullptr : ti->type_params[0].get());
		case vt::jai_shared_ptr_type:
			if (ti->element_type()) { return nav_from_type_info(ti->element_type().get()); }
			return nav_type::of_object(ti->type_name == "shared_ptr" ? std::string_view{} : std::string_view(ti->type_name));
		case vt::jai_object_type: return nav_type::of_object(ti->type_name);
		case vt::jai_array_type: t.kind = nav_type::array; t.ti = ti; return t;
		case vt::jai_map_type: t.kind = nav_type::map; t.ti = ti; return t;
		case vt::jai_string_type:
		case vt::jai_utf8_string_type: t.kind = nav_type::string; t.ti = ti; return t;
		case vt::jai_weak_ptr_type: t.kind = nav_type::weak; t.ti = ti; return t;
		case vt::jai_function_type: t.kind = nav_type::function; return t;
		case vt::jai_int_type:
		case vt::jai_float_type:
		case vt::jai_bool_type:
		case vt::jai_char_type: t.kind = nav_type::scalar; t.ti = ti; return t;
		default: return t;
	}
}

inline nav_type nav_from_check(const jai::check_expression_type& c) {
	using k = jai::check_expression_type::kind_t;
	nav_type t;
	switch (c.kind) {
		case k::object_k: return nav_type::of_object(c.cls);
		case k::class_ref_k: return nav_type::of_class(c.cls);
		case k::enum_ref_k: return nav_type::of_enum(c.cls);
		case k::array_k: t.kind = nav_type::array; t.ti = c.ti; return t;
		case k::map_k: t.kind = nav_type::map; t.ti = c.ti; return t;
		case k::string_k:
		case k::utf8_k: t.kind = nav_type::string; return t;
		case k::weak_k: t.kind = nav_type::weak; t.ti = c.ti; return t;
		case k::function_k: t.kind = nav_type::function; return t;
		case k::int_k:
		case k::float_k:
		case k::bool_k:
		case k::char_k: t.kind = nav_type::scalar; return t;
		default: return t;
	}
}

inline nav_type element_of(const nav_type& container) {
	if (!container.ti) { return {}; }
	if (container.kind == nav_type::array) { return nav_from_type_info(container.ti->element_type().get()); }
	if (container.kind == nav_type::map) { return nav_from_type_info(container.ti->value_type().get()); }
	return {};
}

inline std::string type_text(const jai::type_info* ti) {
	if (!ti) { return "auto"; }
	using vt = jai::script_value_type;
	switch (ti->base_type) {
		case vt::jai_any_type: return "var";
		case vt::jai_reference_type: return type_text(ti->type_params.empty() ? nullptr : ti->type_params[0].get()) + "&";
		case vt::jai_array_type: return "array<" + (ti->element_type() ? type_text(ti->element_type().get()) : std::string("var")) + ">";
		case vt::jai_map_type: return "map<" + (ti->key_type() ? type_text(ti->key_type().get()) : std::string("var")) + ", " +
			(ti->value_type() ? type_text(ti->value_type().get()) : std::string("var")) + ">";
		case vt::jai_shared_ptr_type:
			return "shared_ptr<" + (ti->element_type() ? type_text(ti->element_type().get()) : ti->type_name == "shared_ptr" ? std::string("auto") : ti->type_name) + ">";
		case vt::jai_weak_ptr_type: return "weak_ptr<" + (ti->element_type() ? type_text(ti->element_type().get()) : std::string("auto")) + ">";
		case vt::jai_function_type: return "function";
		default: return ti->canonical_name();
	}
}

inline std::string nav_text(const nav_type& t) {
	switch (t.kind) {
		case nav_type::object: return t.cls;
		case nav_type::class_ref: return "class " + t.cls;
		case nav_type::enum_ref: return "enum " + t.cls;
		case nav_type::string: return t.ti ? type_text(t.ti) : std::string("string");
		default: return t.ti ? type_text(t.ti) : std::string();
	}
}

inline std::string cpp_param_text(jai::engine& eng, const jai::param_type_info& p) {
	using vt = jai::script_value_type;
	switch (p.base_type) {
		case vt::jai_int_type: return "int";
		case vt::jai_float_type: return "float";
		case vt::jai_bool_type: return "bool";
		case vt::jai_string_type: return "string";
		case vt::jai_char_type: return "char";
		case vt::jai_array_type: return "array";
		case vt::jai_map_type: return "map";
		case vt::jai_function_type: return "function";
		case vt::jai_object_type:
			if (auto def = jai::detail::engine_access::get_class_definition_by_type(eng, p.cpp_type)) { return def->get_name(); }
			return "object";
		default: return "var";
	}
}

inline std::string literal_text(const jai::expression* e) {
	if (!e || e->get_type() != jai::node_type::literal_expr) { return "..."; }
	const auto& v = static_cast<const jai::literal_expr*>(e)->value;
	if (v.is_null()) { return "null"; }
	if (v.is_bool()) { return v.unchecked_as_bool() ? "true" : "false"; }
	if (v.is_int()) { return std::to_string(v.unchecked_as_int()); }
	if (v.is_string()) { return "\"" + std::string(v.unchecked_as_string()) + "\""; }
	return "...";
}

// "int heal(Character& c, int amount = 5)" with each parameter's offsets in the label
inline signature_info function_signature(const jai::function_decl& fn, std::string_view shown_name, bool constructor) {
	signature_info sig;
	std::string label;
	if (!constructor) {
		if (fn.is_static) { label += "static "; }
		if (fn.is_coroutine) { label += "coroutine "; }
		label += fn.return_type ? type_text(fn.return_type.get()) : std::string("auto");
		label += ' ';
	}
	label += shown_name;
	label += '(';
	for (size_t i = 0; i < fn.parameters.size(); ++i) {
		const auto& p = fn.parameters[i];
		if (i) { label += ", "; }
		uint32_t start = static_cast<uint32_t>(label.size());
		std::string t = p.type ? type_text(p.type.get()) : std::string("auto");
		if (p.is_reference && (t.empty() || t.back() != '&')) { t += '&'; }
		if (p.is_const) { label += "const "; }
		label += t + " " + p.name;
		if (p.default_value) { label += " = " + literal_text(p.default_value.get()); }
		sig.parameters.emplace_back(start, static_cast<uint32_t>(label.size()));
	}
	label += ')';
	sig.label = std::move(label);
	return sig;
}

inline bool is_identifier_text(std::string_view s) {
	if (s.empty()) { return false; }
	auto head = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
	if (!head(s[0])) { return false; }
	for (char c : s) { if (!head(c) && !(c >= '0' && c <= '9')) { return false; } }
	return true;
}

inline const std::vector<std::string_view>& language_keywords() {
	static const std::vector<std::string_view> words = {
		"bool", "break", "char", "class", "const", "continue", "else", "false", "float", "for", "function", "if",
		"int", "map", "array", "namespace", "null", "override", "private", "public", "return", "static", "string",
		"this", "true", "void", "while", "auto", "var", "super", "weak_ptr", "shared_ptr", "try", "catch", "throw",
		"switch", "case", "default", "fallthrough", "include", "import", "coroutine", "yield", "enum", "new",
		"protected", "parallel_for", "nocopy", "cast", "extern", "inline", "double", "utf8_string"
	};
	return words;
}

inline bool is_keyword(std::string_view s) {
	const auto& words = language_keywords();
	return std::find(words.begin(), words.end(), s) != words.end() || s == "nullptr";
}

inline uint64_t fnv1a(std::string_view s, uint64_t h = 1469598103934665603ull) {
	for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
	return h;
}

inline text_span name_span(const jai::source_location& loc, std::string_view name) {
	text_pos start{static_cast<uint32_t>(loc.line ? loc.line - 1 : 0), static_cast<uint32_t>(loc.column ? loc.column - 1 : 0)};
	return {start, {start.line, start.col + static_cast<uint32_t>(name.size())}};
}

// Index of the first token at or after pos
inline size_t token_at_or_after(const std::vector<lex_token>& tokens, text_pos pos) {
	return static_cast<size_t>(std::lower_bound(tokens.begin(), tokens.end(), pos,
		[](const lex_token& t, text_pos p) { return t.pos < p; }) - tokens.begin());
}

// a keyword token names a member declared or reached with its spelling (int default; p.catch())
inline bool token_spells_name(const lex_token& t, std::string_view name) {
	return t.lexeme == name && (t.type == jai::token_type::identifier || is_keyword(name));
}

// The first name token spelled `name` at or after pos, within `limit` tokens
inline std::optional<text_span> find_name_after(const std::vector<lex_token>& tokens, text_pos pos, std::string_view name, size_t limit = 400) {
	for (size_t i = token_at_or_after(tokens, pos), n = 0; i < tokens.size() && n < limit; ++i, ++n) {
		const auto& t = tokens[i];
		if (token_spells_name(t, name)) {
			return text_span{t.pos, {t.pos.line, t.pos.col + static_cast<uint32_t>(name.size())}};
		}
	}
	return std::nullopt;
}

inline std::optional<text_span> find_name_before(const std::vector<lex_token>& tokens, text_pos pos, std::string_view name, size_t limit = 64) {
	size_t i = token_at_or_after(tokens, pos);
	for (size_t n = 0; i > 0 && n < limit; ++n) {
		const auto& t = tokens[--i];
		if (token_spells_name(t, name)) {
			return text_span{t.pos, {t.pos.line, t.pos.col + static_cast<uint32_t>(name.size())}};
		}
	}
	return std::nullopt;
}

// The '}' closing the first '{' at or after pos
inline std::optional<text_pos> block_end_after(const std::vector<lex_token>& tokens, text_pos pos) {
	size_t i = token_at_or_after(tokens, pos);
	while (i < tokens.size() && tokens[i].type != jai::token_type::left_brace) {
		if (tokens[i].type == jai::token_type::semicolon) { return tokens[i].pos; }
		++i;
	}
	int depth = 0;
	for (; i < tokens.size(); ++i) {
		if (tokens[i].type == jai::token_type::left_brace) { ++depth; }
		else if (tokens[i].type == jai::token_type::right_brace && --depth == 0) { return text_pos{tokens[i].pos.line, tokens[i].pos.col + 1}; }
	}
	return std::nullopt;
}

} // namespace jai_lsp
