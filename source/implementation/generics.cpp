// Generics: a definition's stamp and the token text of an instance (detail/generics.hpp, docs/site/guide/21-generics.html)
#include <jaiscript/detail/generics.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/detail/ast.hpp>
#include <algorithm>
#include <cstring>

namespace jai::detail {

namespace {

	void stamp_mix(uint64_t& hash, const void* data, size_t size) {
		const auto* bytes = static_cast<const unsigned char*>(data);
		for (size_t i = 0; i < size; ++i) {
			hash ^= bytes[i];
			hash *= 1099511628211ull;
		}
	}

	void stamp_mix(uint64_t& hash, uint64_t value) { stamp_mix(hash, &value, sizeof(value)); }

	token make_name(std::string_view text, const source_location& at, string_symbolizer& symbols) {
		const auto [id, view] = symbols.intern_with_view(text);
		return token(token_type::identifier, view, id, at);
	}

	// a::b::Name<x::y>: its segments, cut at each '::' outside the angle brackets
	void qualified_name_tokens(std::string_view name, const source_location& at, string_symbolizer& symbols, std::vector<token>& out) {
		size_t start = 0;
		int depth = 0;
		for (size_t i = 0; i + 1 < name.size(); ++i) {
			if (name[i] == '<') { ++depth; }
			else if (name[i] == '>') { --depth; }
			else if (depth == 0 && name[i] == ':' && name[i + 1] == ':') {
				out.push_back(make_name(name.substr(start, i - start), at, symbols));
				out.emplace_back(token_type::colon_colon, "::", at);
				start = i + 2;
				++i;
			}
		}
		out.push_back(make_name(name.substr(start), at, symbols));
	}

	bool member_position(const std::vector<token>& tokens, size_t i) {
		if (i == 0) { return false; }
		const token_type before = tokens[i - 1].type;
		if (before == token_type::dot || before == token_type::question_dot || before == token_type::colon_colon) { return true; }
		if (before != token_type::arrow) { return false; }
		// after '->' a name is a member unless a body or a reference follows (a trailing return type)
		const token_type after = i + 1 < tokens.size() ? tokens[i + 1].type : token_type::eof;
		return !(after == token_type::left_brace || after == token_type::ampersand || after == token_type::semicolon);
	}

	// `i` names the generic by its bare name: its own tokens before it spell no other qualifier than the generic's namespace
	bool names_generic(const generic_definition& definition, size_t i) {
		const std::vector<token>& tokens = definition.tokens;
		if (i == 0) { return true; }
		const token_type before = tokens[i - 1].type;
		if (before != token_type::colon_colon) { return !member_position(tokens, i); }
		std::string qualifier;
		for (size_t k = i - 1; k >= 1 && tokens[k].type == token_type::colon_colon && tokens[k - 1].type == token_type::identifier; k -= 2) {
			qualifier.insert(0, qualifier.empty() ? std::string(tokens[k - 1].lexeme) : std::string(tokens[k - 1].lexeme) + "::");
			if (k < 2) { break; }
		}
		return qualifier == definition.space;
	}

	// tokens[i] stands as a whole argument of a type-argument list (Name<..., T, ...>); `head` is the token before the list's '<'
	bool list_argument(const std::vector<token>& tokens, size_t i, size_t& head) {
		if (i == 0 || i + 1 >= tokens.size()) { return false; }
		const token_type before = tokens[i - 1].type, after = tokens[i + 1].type;
		if ((before != token_type::less && before != token_type::comma) ||
		    (after != token_type::greater && after != token_type::comma && after != token_type::right_shift)) { return false; }
		int depth = 0;
		for (size_t k = i; k-- > 0;) {
			switch (tokens[k].type) {
				case token_type::greater: ++depth; break;
				case token_type::right_shift: depth += 2; break;
				case token_type::less:
					if (depth == 0) { head = k - 1; return k > 0; }
					--depth;
					break;
				case token_type::identifier: case token_type::colon_colon: case token_type::comma: case token_type::ampersand:
				case token_type::int_keyword: case token_type::float_keyword: case token_type::string_keyword: case token_type::bool_keyword:
				case token_type::char_keyword: case token_type::var_keyword: case token_type::array_keyword: case token_type::map_keyword:
				case token_type::shared_ptr_keyword: case token_type::weak_ptr_keyword: case token_type::function_keyword:
				case token_type::coroutine_keyword:
					break;
				default:
					return false;   // a signature's or a call's '(' , a statement: no type list opens before it
			}
		}
		return false;
	}

	// the name at `head`, with the qualifier before it, is a host template's
	bool host_template_at(const std::vector<token>& tokens, size_t head, const std::unordered_set<std::string>* templates) {
		if (!templates || templates->empty() || tokens[head].type != token_type::identifier) { return false; }
		std::string name(tokens[head].lexeme);
		for (size_t k = head; k >= 2 && tokens[k - 1].type == token_type::colon_colon && tokens[k - 2].type == token_type::identifier; k -= 2) {
			name.insert(0, std::string(tokens[k - 2].lexeme) + "::");
		}
		return templates->count(name) != 0;
	}

	// Where a parameter's argument is spelled as it is bound and not as it is read (generic_spelling): as a host template's
	// argument, and a byte as an array's element
	bool bound_position(const std::vector<token>& tokens, size_t i, const type_info* argument, const generic_spelling* host) {
		size_t head = 0;
		if (!list_argument(tokens, i, head)) { return false; }
		if (tokens[head].type == token_type::array_keyword) { return argument && argument->is_byte_element(); }
		return host && host_template_at(tokens, head, host->templates);
	}

	// the arguments as tokens: as they are bound, and as an instance reads them where a parameter stands on its own
	struct spelled_arguments {
		const std::vector<type_info_ptr>& arguments;
		const generic_spelling* host;
		std::vector<std::vector<token>> bound, read;

		spelled_arguments(const std::vector<type_info_ptr>& a_arguments, string_symbolizer& symbols, const generic_spelling* a_host) : arguments(a_arguments), host(a_host) {
			const std::vector<std::string_view> none;
			for (const auto& argument : arguments) {
				bound.emplace_back();
				read.emplace_back();
				if (!argument) { continue; }
				generic_argument_tokens(*argument, {}, symbols, bound.back());
				generic_argument_tokens(*argument, {}, symbols, read.back(), host ? &host->sized : &none);
			}
		}
		bool empty() const { return bound.empty(); }
		// the parameter's argument where tokens[i] names it
		const std::vector<token>& at(const std::vector<token>& tokens, size_t i, size_t parameter) const {
			return bound_position(tokens, i, arguments[parameter].get(), host) ? bound[parameter] : read[parameter];
		}
	};

	void replace_parameters(const generic_definition& definition, const std::vector<token>& tokens, size_t begin, size_t end,
	                        const spelled_arguments& spelled, std::vector<token>& out) {
		for (size_t i = begin; i < end; ++i) {
			const token& t = tokens[i];
			if (t.type == token_type::identifier && !spelled.empty() && !member_position(tokens, i)) {
				const auto parameter = std::find_if(definition.parameters.begin(), definition.parameters.end(),
					[&](const generic_parameter& p) { return p.name == t.lexeme; });
				if (parameter != definition.parameters.end()) {
					for (token piece : spelled.at(tokens, i, static_cast<size_t>(parameter - definition.parameters.begin()))) {
						piece.location = t.location;
						out.push_back(std::move(piece));
					}
					continue;
				}
			}
			out.push_back(t);
		}
	}

	size_t parameter_index(const generic_definition& g, const type_info& t) {
		if (t.base_type != script_value_type::jai_object_type || !t.type_params.empty()) { return SIZE_MAX; }
		for (size_t i = 0; i < g.parameters.size(); ++i) { if (g.parameters[i].name == t.type_name) { return i; } }
		return SIZE_MAX;
	}

	bool same_pattern(const generic_definition& a, const type_info* x, const generic_definition& b, const type_info* y, int depth = 0) {
		if (!x || !y) { return x == y; }
		if (depth > 64) { return false; }
		const size_t px = parameter_index(a, *x), py = parameter_index(b, *y);
		if (px != SIZE_MAX || py != SIZE_MAX) { return px == py; }
		if (x->base_type != y->base_type || x->type_params.size() != y->type_params.size()) { return false; }
		if (x->type_params.empty()) { return x->canonical_name() == y->canonical_name(); }
		if (x->base_type == script_value_type::jai_object_type &&
		    std::string_view(x->type_name).substr(0, x->type_name.find('<')) != std::string_view(y->type_name).substr(0, y->type_name.find('<'))) { return false; }
		for (size_t i = 0; i < x->type_params.size(); ++i) {
			if (!same_pattern(a, x->type_params[i].get(), b, y->type_params[i].get(), depth + 1)) { return false; }
		}
		return true;
	}

} // namespace

bool builtin_type_name(std::string_view name) {
	return name == "int" || name == "float" || name == "string" || name == "bool" || name == "char" || name == "var" || name == "utf8_string" || name == "double" ||
	       math_type_from_name(name) != script_value_type::jai_null_type;
}

std::string no_type_arguments_text(std::string_view name, plain_type_kind kind, std::string_view generic) {
	if (kind == plain_type_kind::type_parameter) { return "'" + std::string(name) + "' takes no type arguments: it is a type parameter of '" + std::string(generic) + "'"; }
	const char* is = kind == plain_type_kind::host_class ? "it is a host class, not a host template"
	               : kind == plain_type_kind::enumeration ? "it is an enum"
	               : kind == plain_type_kind::builtin ? "it is a builtin type" : "it is not a generic class";
	return "'" + std::string(name) + "' takes no type arguments: " + is;
}

std::string shown_instance_name(std::string_view name) {
	constexpr size_t k_longest = 80, k_end = 36;
	if (name.size() <= k_longest) { return std::string(name); }
	return std::string(name.substr(0, k_end)) + " ... " + std::string(name.substr(name.size() - k_end));
}

std::string instance_chain_text(const std::vector<std::string_view>& askers, std::string_view request, bool& more) {
	constexpr size_t k_links = 3;
	std::string text;
	for (size_t i = 0; i < k_links && i <= askers.size(); ++i) {
		text += (i ? " needs '" : "'") + shown_instance_name(i < askers.size() ? askers[askers.size() - 1 - i] : request) + "'";
	}
	more = askers.size() + 1 > k_links;
	return text;
}

bool same_generic_signature(const generic_definition& a, const generic_definition& b) {
	if (a.kind != b.kind || a.parameters.size() != b.parameters.size() || a.values.size() != b.values.size()) { return false; }
	for (size_t i = 0; i < a.values.size(); ++i) {
		if (a.values[i].is_reference != b.values[i].is_reference || !same_pattern(a, a.values[i].type.get(), b, b.values[i].type.get())) { return false; }
	}
	return true;
}

void own_token_text(generic_definition& definition, string_symbolizer& symbols) {
	const auto own = [&](std::vector<token>& tokens) {
		for (token& t : tokens) {
			if (!t.lexeme.empty()) { t.lexeme = symbols.intern_with_view(t.lexeme).second; }
		}
	};
	own(definition.tokens);
	for (generic_parameter& parameter : definition.parameters) { own(parameter.bound); }
}

bool generic_declared_override(const generic_definition& definition) {
	int depth = 0;
	for (size_t i = definition.list_end; i < definition.tokens.size(); ++i) {
		const token_type kind = definition.tokens[i].type;
		if (kind == token_type::left_paren) { ++depth; }
		else if (kind == token_type::right_paren) { --depth; }
		else if (depth == 0 && kind == token_type::left_brace) { return false; }   // the body: no marker stood before it
		else if (depth == 0 && kind == token_type::override_keyword) { return true; }
	}
	return false;
}

uint64_t generic_stamp(const std::vector<token>& tokens) {
	uint64_t hash = 14695981039346656037ull;
	for (const token& t : tokens) {
		stamp_mix(hash, static_cast<uint64_t>(t.type));
		stamp_mix(hash, t.lexeme.data(), t.lexeme.size());
		stamp_mix(hash, t.string_value.data(), t.string_value.size());
		uint64_t literal = 0;
		std::memcpy(&literal, &t.int_value, std::min(sizeof(literal), sizeof(t.int_value)));
		stamp_mix(hash, literal);
		stamp_mix(hash, static_cast<uint64_t>(t.location.line));
		stamp_mix(hash, static_cast<uint64_t>(t.location.column));
		if (t.less_reading != 0) { stamp_mix(hash, 0x3Cull + t.less_reading); }   // a '<' read another way is another text
	}
	return hash == 0 ? 1 : hash;   // 0 marks a declaration written by hand
}

bool generic_argument_tokens(const type_info& type, const source_location& at, string_symbolizer& symbols, std::vector<token>& out,
                             const std::vector<std::string_view>* read, bool element) {
	const auto word = [&](token_type kind, std::string_view text) { out.emplace_back(kind, text, at); };
	const auto parameter = [&](const type_info_ptr& p) {
		if (!p) { word(token_type::auto_keyword, "auto"); return true; }
		return generic_argument_tokens(*p, at, symbols, out, read);
	};
	switch (type.base_type) {
		case script_value_type::jai_int_type:
			// a byte is a type as an array's element; read on its own it is an int
			if (type.is_enum || (type.is_byte_element() && (!read || element))) { qualified_name_tokens(type.type_name, at, symbols, out); } else { word(token_type::int_keyword, "int"); }
			return true;
		case script_value_type::jai_float_type: word(token_type::float_keyword, "float"); return true;
		case script_value_type::jai_string_type: word(token_type::string_keyword, "string"); return true;
		case script_value_type::jai_bool_type: word(token_type::bool_keyword, "bool"); return true;
		case script_value_type::jai_char_type: word(token_type::char_keyword, "char"); return true;
		case script_value_type::jai_any_type: word(token_type::var_keyword, "var"); return true;
		case script_value_type::jai_utf8_string_type: out.push_back(make_name("utf8_string", at, symbols)); return true;
		case script_value_type::jai_vec2_type: case script_value_type::jai_vec3_type: case script_value_type::jai_vec4_type:
		case script_value_type::jai_color_type: case script_value_type::jai_ivec2_type: case script_value_type::jai_ivec3_type:
			out.push_back(make_name(type.canonical_name(), at, symbols));   // a math type is one token (lexer.cpp)
			return true;
		case script_value_type::jai_function_type: {   // function, or function<R(A, B&)> as its own name spells it
			word(token_type::function_keyword, "function");
			if (type.type_params.empty()) { return true; }
			word(token_type::less, "<");
			const type_info_ptr& result = type.type_params[0];
			if (!result || result->base_type == script_value_type::jai_null_type) { word(token_type::void_keyword, "void"); }
			else if (!generic_argument_tokens(*result, at, symbols, out, read)) { return false; }
			word(token_type::left_paren, "(");
			for (size_t i = 1; i < type.type_params.size(); ++i) {
				if (i > 1) { word(token_type::comma, ","); }
				const type_info* taken = type.type_params[i].get();
				const bool by_reference = taken && taken->base_type == script_value_type::jai_reference_type;
				if (by_reference) { taken = taken->element_type().get(); }
				if (!taken) { word(token_type::var_keyword, "var"); }
				else if (!generic_argument_tokens(*taken, at, symbols, out, read)) { return false; }
				if (by_reference) { word(token_type::ampersand, "&"); }
			}
			word(token_type::right_paren, ")");
			word(token_type::greater, ">");
			return true;
		}
		case script_value_type::jai_array_type:
			word(token_type::array_keyword, "array");
			word(token_type::less, "<");
			if (!type.element_type()) { word(token_type::auto_keyword, "auto"); }
			else if (!generic_argument_tokens(*type.element_type(), at, symbols, out, read, true)) { return false; }
			word(token_type::greater, ">");
			return true;
		case script_value_type::jai_map_type:
			word(token_type::map_keyword, "map");
			word(token_type::less, "<");
			if (!parameter(type.key_type())) { return false; }
			word(token_type::comma, ",");
			if (!parameter(type.value_type())) { return false; }
			word(token_type::greater, ">");
			return true;
		case script_value_type::jai_shared_ptr_type: case script_value_type::jai_weak_ptr_type: {
			const bool shared = type.base_type == script_value_type::jai_shared_ptr_type;
			word(shared ? token_type::shared_ptr_keyword : token_type::weak_ptr_keyword, shared ? "shared_ptr" : "weak_ptr");
			word(token_type::less, "<");
			if (type.element_type()) {
				if (!generic_argument_tokens(*type.element_type(), at, symbols, out, read)) { return false; }
			} else if (shared && !type.type_name.empty() && type.type_name != "shared_ptr") {
				qualified_name_tokens(type.type_name, at, symbols, out);
			} else {
				word(token_type::auto_keyword, "auto");
			}
			word(token_type::greater, ">");
			return true;
		}
		case script_value_type::jai_object_type:
			if (read && std::find(read->begin(), read->end(), std::string_view(type.type_name)) != read->end()) {   // int32, float32: read as what a value is
				const bool floating = sized_number_kind(type.type_name) == script_value_type::jai_float_type;
				word(floating ? token_type::float_keyword : token_type::int_keyword, floating ? "float" : "int");
				return true;
			}
			if (type.is_coroutine) {   // coroutine<Y> is spelled by its keyword, its name being no class's
				const type_info* yields = type.type_params.empty() ? nullptr : type.type_params[0].get();
				word(token_type::coroutine_keyword, "coroutine");
				word(token_type::less, "<");
				if (!yields) { word(token_type::var_keyword, "var"); }
				else if (!generic_argument_tokens(*yields, at, symbols, out, read)) { return false; }
				word(token_type::greater, ">");
				return true;
			}
			if (type.type_name.empty()) { return false; }
			qualified_name_tokens(type.type_name, at, symbols, out);
			return true;
		default:
			return false;   // void, a reference: no instance is made with one
	}
}

std::string generic_instance_name(std::string_view generic, const std::vector<type_info_ptr>& arguments) {
	return type_info::instance_name(generic, arguments);
}

bool generic_parameter_read(const generic_definition& definition, size_t parameter, const std::unordered_set<std::string>& templates) {
	if (parameter >= definition.parameters.size()) { return false; }
	const std::vector<token>& tokens = definition.tokens;
	for (size_t i = 0; i < tokens.size(); ++i) {
		if (i > definition.name_token && i < definition.list_end) { continue; }   // the type parameters
		if (tokens[i].type != token_type::identifier || tokens[i].lexeme != definition.parameters[parameter].name || member_position(tokens, i)) { continue; }
		size_t head = 0;
		if (!list_argument(tokens, i, head) || !host_template_at(tokens, head, &templates)) { return true; }
	}
	return false;
}

std::vector<token> generic_instance_tokens(const generic_definition& definition, const std::vector<type_info_ptr>& arguments,
                                           std::string_view instance, string_symbolizer& symbols, const generic_spelling* host) {
	const std::vector<token>& tokens = definition.tokens;
	const source_location at = tokens.empty() ? definition.location : tokens.front().location;
	const spelled_arguments spelled(arguments, symbols, host);
	std::vector<token> out;
	out.reserve(tokens.size() + 16);
	size_t closes = 0;
	if (!definition.space.empty()) {
		out.emplace_back(token_type::namespace_keyword, "namespace", at);
		qualified_name_tokens(definition.space, at, symbols, out);
		out.emplace_back(token_type::left_brace, "{", at);
		++closes;
	}
	if (definition.kind == generic_kind::method) {
		std::string_view owner = definition.owner;
		if (!definition.space.empty() && owner.size() > definition.space.size() + 2 && owner.substr(0, definition.space.size()) == definition.space) {
			owner.remove_prefix(definition.space.size() + 2);
		}
		out.emplace_back(token_type::class_keyword, "class", at);
		out.push_back(make_name(owner, at, symbols));
		out.emplace_back(token_type::left_brace, "{", at);
		if (definition.visibility != 0) {
			out.emplace_back(definition.visibility == 1 ? token_type::private_keyword : token_type::protected_keyword,
			                 definition.visibility == 1 ? "private" : "protected", at);
			out.emplace_back(token_type::colon, ":", at);
		}
		++closes;
	}
	const bool is_class = definition.kind == generic_kind::class_type;
	for (size_t i = 0; i < tokens.size(); ++i) {
		const token& t = tokens[i];
		if (i > definition.name_token && i < definition.list_end) { continue; }   // the type parameters
		if (i == definition.name_token) {
			out.push_back(make_name(instance, t.location, symbols));
			continue;
		}
		if (t.type == token_type::identifier && !spelled.empty()) {
			if (!member_position(tokens, i)) {
				const auto parameter = std::find_if(definition.parameters.begin(), definition.parameters.end(),
					[&](const generic_parameter& p) { return p.name == t.lexeme; });
				if (parameter != definition.parameters.end()) {
					for (token piece : spelled.at(tokens, i, static_cast<size_t>(parameter - definition.parameters.begin()))) {
						piece.location = t.location;
						out.push_back(std::move(piece));
					}
					continue;
				}
			}
			// the class's bare name inside it is the instance; with its own arguments (Stack<T>) it spells the instance already
			if (is_class && t.lexeme == definition.name && (i + 1 >= tokens.size() || tokens[i + 1].type != token_type::less) && names_generic(definition, i)) {
				out.push_back(make_name(instance, t.location, symbols));
				continue;
			}
		}
		out.push_back(t);
	}
	const source_location end = tokens.empty() ? at : tokens.back().location;
	for (; closes > 0; --closes) { out.emplace_back(token_type::right_brace, "}", end); }
	out.emplace_back(token_type::eof, "", end);
	return out;
}

std::vector<token> generic_bound_tokens(const generic_definition& definition, size_t parameter, const std::vector<type_info_ptr>& arguments,
                                        string_symbolizer& symbols, const generic_spelling* host) {
	std::vector<token> out;
	if (parameter >= definition.parameters.size()) { return out; }
	const std::vector<token>& bound = definition.parameters[parameter].bound;
	replace_parameters(definition, bound, 0, bound.size(), spelled_arguments(arguments, symbols, host), out);
	return out;
}

std::shared_ptr<class_decl> generic_marker(const generic_decl& declared, string_symbolizer& symbols) {
	if (!declared.marker_made) {
		declared.marker_made = true;
		if (declared.definition && declared.definition->kind != generic_kind::method) {   // a method's is declared with its class
			const auto [id, name] = symbols.intern_with_view(declared.definition->qualified_name());
			declared.marker = std::make_shared<class_decl>(declared.location, name, id);
			declared.marker->generic_marker = true;
			declared.marker->marker_stamp = declared.definition->stamp;
			declared.marker->marker_definition = declared.definition;
		}
	}
	return declared.marker;
}

} // namespace jai::detail
