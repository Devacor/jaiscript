#include "semantic.hpp"

#include <jaiscript/core/class_definition.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/math_intrinsics.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>

#include <cctype>
#include <functional>
#include <map>
#include <set>

namespace jai_lsp {

namespace {

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

int completion_kind(symbol_kind k) {
	switch (k) {
		case symbol_kind::local:
		case symbol_kind::parameter:
		case symbol_kind::global:
		case symbol_kind::host_global: return 6;
		case symbol_kind::function:
		case symbol_kind::host_function: return 3;
		case symbol_kind::class_type:
		case symbol_kind::host_class: return 7;
		case symbol_kind::field: return 5;
		case symbol_kind::method:
		case symbol_kind::host_member: return 2;
		case symbol_kind::constructor: return 4;
		case symbol_kind::enum_type: return 13;
		case symbol_kind::enum_value: return 20;
		case symbol_kind::namespace_name:
		case symbol_kind::host_namespace: return 9;
	}
	return 1;
}

const char* kind_words(symbol_kind k) {
	switch (k) {
		case symbol_kind::local: return "local variable";
		case symbol_kind::parameter: return "parameter";
		case symbol_kind::global: return "global variable";
		case symbol_kind::function: return "function";
		case symbol_kind::class_type: return "class";
		case symbol_kind::field: return "field";
		case symbol_kind::method: return "method";
		case symbol_kind::constructor: return "constructor";
		case symbol_kind::enum_type: return "enum";
		case symbol_kind::enum_value: return "enum value";
		case symbol_kind::namespace_name: return "namespace";
		case symbol_kind::host_function: return "host function";
		case symbol_kind::host_global: return "host global";
		case symbol_kind::host_class: return "host class";
		case symbol_kind::host_member: return "member";
		case symbol_kind::host_namespace: return "namespace";
	}
	return "symbol";
}

bool matches_prefix(std::string_view name, std::string_view prefix) {
	if (prefix.empty()) { return true; }
	size_t j = 0;
	for (size_t i = 0; i < name.size() && j < prefix.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(name[i])) == std::tolower(static_cast<unsigned char>(prefix[j]))) { ++j; }
	}
	return j == prefix.size() && std::tolower(static_cast<unsigned char>(name[0])) == std::tolower(static_cast<unsigned char>(prefix[0]));
}

// The // comment block right above a declaration, or the one trailing it on its line
std::string doc_comment(const unit& owner, text_span span) {
	auto trim = [](std::string_view s) {
		while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.remove_prefix(1); }
		while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) { s.remove_suffix(1); }
		return s;
	};
	std::vector<std::string> above;
	for (uint32_t line = span.start.line; line-- > 0 && above.size() < 12;) {
		std::string_view text = trim(owner.lines.line_text(owner.text, line));
		if (text.rfind("//", 0) != 0) { break; }
		text.remove_prefix(2);
		above.emplace_back(trim(text));
	}
	std::string out;
	for (auto it = above.rbegin(); it != above.rend(); ++it) { out += (out.empty() ? "" : "\n") + *it; }
	if (!out.empty()) { return out; }
	std::string_view line = owner.lines.line_text(owner.text, span.start.line);
	size_t pos = line.find("//", span.end.col);
	if (pos != std::string_view::npos && line.substr(span.end.col, pos - span.end.col).find('"') == std::string_view::npos) {
		return std::string(trim(line.substr(pos + 2)));
	}
	return {};
}

std::string file_name(const std::string& path) {
	size_t pos = path.find_last_of("/\\");
	return pos == std::string::npos ? path : path.substr(pos + 1);
}

// Label offsets of each top-level comma-separated parameter inside "name(a, b)"
signature_info signature_from_label(const std::string& label) {
	signature_info sig;
	sig.label = label;
	size_t open = label.find('(');
	size_t close = label.rfind(')');
	if (open == std::string::npos || close == std::string::npos || close <= open + 1) { return sig; }
	int depth = 0;
	size_t start = open + 1;
	for (size_t i = open + 1; i <= close; ++i) {
		char c = label[i];
		if (c == '<' || c == '(' || c == '[') { ++depth; }
		else if (c == '>' || c == ']' || (c == ')' && i != close)) { --depth; }
		if ((c == ',' && depth == 0) || i == close) {
			size_t s = start, e = i;
			while (s < e && label[s] == ' ') { ++s; }
			if (e > s && label.substr(s, e - s) != "...") { sig.parameters.emplace_back(static_cast<uint32_t>(s), static_cast<uint32_t>(e)); }
			start = i + 1;
		}
	}
	return sig;
}

struct builtin_names {
	std::vector<std::string> array, map, string, utf8, weak;
};

// The names are the same for every engine, so the first caller's engine builds them once (a thread-safe static)
const builtin_names& builtin_method_names(jai::engine& eng) {
	static const builtin_names names = [&eng]() {
		builtin_names result;
		jai::builtin_method_registries reg;
		auto* sym = jai::detail::engine_access::get_symbolizer(eng);
		jai::init_builtin_method_registries(sym, reg);
		auto fill = [&](const auto& table, std::vector<std::string>& out) {
			for (const auto& [id, _] : table) { out.emplace_back(sym->get_string(id)); }
			std::sort(out.begin(), out.end());
		};
		fill(reg.array_methods, result.array);
		fill(reg.map_methods, result.map);
		fill(reg.string_methods, result.string);
		fill(reg.utf8_methods, result.utf8);
		fill(reg.weak_ptr_methods, result.weak);
		return result;
	}();
	return names;
}

const std::vector<std::string>& math_intrinsic_names() {
	static const std::vector<std::string> names = [] {
		std::vector<std::string> out;
		for (const char* n : {"abs", "atan2", "cos", "ceil", "clamp", "distance", "distance_squared", "degrees_to_radians", "floor",
		                      "itrunc", "ifloor", "lerp", "min", "max", "mix", "mix_in", "mix_out", "mix_in_out", "mix_out_in",
		                      "unmix", "unmix_in", "unmix_out", "unmix_in_out", "unmix_out_in", "pow",
		                      "radians_to_degrees", "sin", "sqrt", "tan", "wrap_add", "wrap_sub", "wrap_mul"}) {
			if (jai::detail::math_intrinsic_from_name(n) != jai::detail::math_fn::none) { out.emplace_back(n); }
		}
		return out;
	}();
	return names;
}

class item_list {
public:
	explicit item_list(std::string prefix) : prefix_(std::move(prefix)) {}
	void add(const std::string& label, int kind, const std::string& detail, int rank) {
		if (label.empty() || label == k_placeholder || !matches_prefix(label, prefix_)) { return; }
		if (!seen_.insert(label).second) { return; }
		completion_item item;
		item.label = label;
		item.kind = kind;
		item.detail = detail;
		item.sort = std::to_string(rank) + label;
		items_.push_back(std::move(item));
	}
	std::vector<completion_item> take() { return std::move(items_); }
private:
	std::string prefix_;
	std::unordered_set<std::string> seen_;
	std::vector<completion_item> items_;
};

void add_class_members(program_view& view, item_list& items, std::string_view cls, bool statics_only, int depth = 0) {
	if (depth > 32) { return; }
	auto& eng = view.ws.engine();
	if (const jai::class_decl* cd = view.find_class(cls)) {
		for (const auto& m : cd->members) {
			if (!m.declaration) { continue; }
			if (m.declaration->get_type() == jai::node_type::variable_decl) {
				auto* f = static_cast<const jai::variable_decl*>(m.declaration.get());
				if (statics_only && !f->is_static) { continue; }
				items.add(std::string(f->name), 5, type_text(f->type.get()) + " " + std::string(cd->name) + "." + std::string(f->name), 1 + depth);
			} else if (m.declaration->get_type() == jai::node_type::function_decl) {
				auto* fn = static_cast<const jai::function_decl*>(m.declaration.get());
				if (fn->name == cd->name || fn->name.rfind("~", 0) == 0 || (statics_only && !fn->is_static)) { continue; }
				items.add(std::string(fn->name), 2, function_signature(*fn, fn->name, false).label, 1 + depth);
			}
		}
		for (auto base : cd->base_classes) { add_class_members(view, items, base, statics_only, depth + 1); }
		return;
	}
	auto def = jai::detail::engine_access::get_class_definition(eng, std::string(cls));
	if (!def) { return; }
	auto* sym = jai::detail::engine_access::get_symbolizer(eng);
	std::vector<const jai::class_definition*> chain{def.get()};
	for (size_t i = 0; i < chain.size() && i < 32; ++i) {
		const jai::class_definition* d = chain[i];
		auto add_name = [&](uint64_t id, bool is_static) {
			std::string name(sym->get_string(id));
			if (name.empty() || name == d->get_name() || name.rfind("_set_", 0) == 0) { return; }
			if (statics_only && !is_static) { return; }
			if (name.rfind("_get_", 0) == 0) { items.add(name.substr(5), 10, d->get_name() + "." + name.substr(5), 1); return; }
			items.add(name, 2, d->get_name() + "." + name, 1);
		};
		for (const auto& [id, _] : d->method_values()) { add_name(id, false); }
		for (const auto& [id, _] : d->cpp_method_overload_table()) { add_name(id, false); }
		for (const auto& [id, _] : d->script_method_overloads()) { add_name(id, false); }
		for (const auto& [id, _] : d->static_method_values()) { add_name(id, true); }
		for (const auto& [id, _] : d->script_static_method_overloads()) { add_name(id, true); }
		if (!statics_only) {
			for (const auto& [id, _] : d->get_all_field_defaults()) {
				std::string name(sym->get_string(id));
				if (!name.empty()) { items.add(name, 5, d->get_name() + "." + name, 1); }
			}
		}
		for (const auto& parent : d->get_parent_classes()) { if (parent) { chain.push_back(parent.get()); } }
		if (auto base = d->get_cpp_base_class()) { chain.push_back(base.get()); }
	}
}

void add_namespace_members(program_view& view, item_list& items, const std::string& ns) {
	const std::string prefix = ns + "::";
	for (unit* u : view.units()) {
		for (const auto& [name, entries] : view.index_of(*u).names) {
			if (name.rfind(prefix, 0) != 0) { continue; }
			std::string rest = name.substr(prefix.size());
			if (rest.find("::") != std::string::npos || entries.empty()) { continue; }
			items.add(rest, completion_kind(entries.front().kind), name, 1);
		}
		for (const auto& n : view.index_of(*u).namespaces) {
			if (n.rfind(prefix, 0) == 0 && n.find("::", prefix.size()) == std::string::npos) { items.add(n.substr(prefix.size()), 9, "namespace " + n, 2); }
		}
	}
	auto& eng = view.ws.engine();
	if (ns == "math") {
		for (const auto& n : math_intrinsic_names()) { items.add(n, 3, "math::" + n, 1); }
	}
	auto it = jai::detail::engine_access::script_namespaces(eng).find(jai::detail::engine_access::symbolize(eng, ns));
	if (it == jai::detail::engine_access::script_namespaces(eng).end() || !it->second) { return; }
	auto* sym = jai::detail::engine_access::get_symbolizer(eng);
	for (const auto& [id, _] : it->second->functions) { items.add(std::string(sym->get_string(id)), 3, prefix + std::string(sym->get_string(id)), 1); }
	for (const auto& [id, value] : it->second->variables) {
		items.add(std::string(sym->get_string(id)), value.is_function() ? 3 : 6, prefix + std::string(sym->get_string(id)), 1);
	}
	if (it->second->scope) {
		for (const auto& [name, value] : it->second->scope->get_all_variables()) {
			items.add(std::string(name), value.deref().is_function() ? 3 : 6, prefix + std::string(name), 1);
		}
	}
}

void add_host_names(program_view& view, item_list& items) {
	auto& eng = view.ws.engine();
	if (auto env = jai::detail::engine_access::get_global_environment(eng)) {
		for (const auto& [name, value] : env->get_all_variables()) {
			std::string n(name);
			if (value.is_object()) {
				auto holder = value.get_object_holder();
				if (holder && holder->type_name() == "class_definition") { continue; }
			}
			if (jai::detail::engine_access::get_class_definition(eng, n)) { continue; }
			if (value.is_function()) {
				auto sigs = view.host_signatures(n);
				items.add(n, 3, sigs.empty() ? n + "(...)" : sigs.front(), 4);
			} else {
				items.add(n, 6, value.get_type_info() ? type_text(value.get_type_info()) + " " + n : n, 4);
			}
		}
	}
	for (const auto& c : eng.registered_class_names()) { items.add(c, 7, "class " + c, 4); }
	auto* sym = jai::detail::engine_access::get_symbolizer(eng);
	for (const auto& [id, data] : jai::detail::engine_access::script_namespaces(eng)) {
		std::string n(sym->get_string(id));
		if (n.find("::") == std::string::npos) { items.add(n, 9, "namespace " + n, 5); }
	}
	items.add("math", 9, "namespace math", 5);
}

} // namespace

const symbol* workspace::symbol_at(unit& u, text_pos pos, uint32_t* index) {
	const unit_model& m = model(u);
	auto it = std::upper_bound(m.occurrences.begin(), m.occurrences.end(), pos,
		[](text_pos p, const occurrence& o) { return p < o.span.start; });
	while (it != m.occurrences.begin()) {
		--it;
		if (it->span.contains(pos)) {
			if (index) { *index = it->symbol; }
			return &m.symbols[it->symbol];
		}
		if (it->span.start.line < pos.line) { break; }
	}
	return nullptr;
}

std::optional<location> workspace::definition(unit& u, text_pos pos) {
	for (size_t i = 0; i < u.tokens.size(); ++i) {
		const auto& t = u.tokens[i];
		if (t.type != jai::token_type::string_literal || t.pos.line != pos.line || pos.col < t.pos.col || pos.col > t.pos.col + t.length) { continue; }
		if (i == 0 || (u.tokens[i - 1].type != jai::token_type::import_keyword && u.tokens[i - 1].type != jai::token_type::include_keyword)) { break; }
		std::string_view line = u.lines.line_text(u.text, t.pos.line);
		if (t.pos.col + t.length > line.size() || t.length < 2) { break; }
		std::string target(line.substr(t.pos.col + 1, t.length - 2));
		auto r = jai::resolve_include_path(target, engine_.get(), u.path);
		if (r) { return location{canonical_path(r.value()), {}}; }
		break;
	}
	const symbol* s = symbol_at(u, pos);
	if (!s || s->path.empty()) { return std::nullopt; }
	return location{s->path, s->span};
}

std::vector<std::string> workspace::reference_scope(unit& u, const symbol& s) {
	if (s.key.rfind("l:", 0) == 0) { return {u.path}; }
	std::vector<std::string> scope = program_units(u.path);
	if (!s.path.empty() && s.path != u.path) {
		for (const auto& p : program_units(s.path)) {
			if (std::find(scope.begin(), scope.end(), p) == scope.end()) { scope.push_back(p); }
		}
	}
	return scope;
}

std::vector<location> workspace::collect_references(const std::vector<std::string>& paths, const std::vector<std::string>& keys, bool include_declaration) {
	std::vector<location> out;
	for (const auto& p : paths) {
		unit* x = get(p);
		if (!x) { continue; }
		const unit_model& m = model(*x);
		for (const auto& o : m.occurrences) {
			if (!include_declaration && o.declaration) { continue; }
			const std::string& key = m.symbols[o.symbol].key;
			if (std::find(keys.begin(), keys.end(), key) != keys.end()) { out.push_back({p, o.span}); }
		}
	}
	return out;
}

std::vector<location> workspace::references(unit& u, text_pos pos, bool include_declaration) {
	const symbol* s = symbol_at(u, pos);
	if (!s) { return {}; }
	symbol copy = *s;
	return collect_references(reference_scope(u, copy), {copy.key}, include_declaration);
}

std::vector<occurrence> workspace::highlights(unit& u, text_pos pos) {
	uint32_t index = 0;
	if (!symbol_at(u, pos, &index)) { return {}; }
	std::vector<occurrence> out;
	for (const auto& o : model(u).occurrences) {
		if (o.symbol == index) { out.push_back(o); }
	}
	return out;
}

std::optional<std::string> workspace::hover(unit& u, text_pos pos, text_span& range) {
	uint32_t index = 0;
	const symbol* s = symbol_at(u, pos, &index);
	if (!s) { return std::nullopt; }
	const unit_model& m = model(u);
	for (const auto& o : m.occurrences) {
		if (o.symbol == index && o.span.contains(pos)) { range = o.span; break; }
	}
	std::string code;
	if (!s->signatures.empty()) {
		for (const auto& sig : s->signatures) { code += sig + "\n"; }
	} else {
		code = s->detail + "\n";
	}
	if (s->kind == symbol_kind::class_type) {
		program_view view(*this, program_of(u.path), u);
		if (const jai::class_decl* cd = view.find_class(s->name)) {
			code = view.class_detail(cd) + " {\n";
			size_t shown = 0;
			for (const auto& mem : cd->members) {
				if (!mem.declaration || shown >= 40) { continue; }
				if (mem.declaration->get_type() == jai::node_type::variable_decl) {
					auto* f = static_cast<const jai::variable_decl*>(mem.declaration.get());
					code += std::string("    ") + (f->is_static ? "static " : "") + (f->function_spelling ? std::string("function") : type_text(f->type.get())) + " " + std::string(f->name) + ";\n";
					++shown;
				} else if (mem.declaration->get_type() == jai::node_type::function_decl) {
					auto* fn = static_cast<const jai::function_decl*>(mem.declaration.get());
					code += "    " + function_signature(*fn, fn->name, fn->name == cd->name).label + ";\n";
					++shown;
				}
			}
			if (cd->members.size() > shown) { code += "    // ...\n"; }
			code += "}\n";
		}
	}
	std::string md = "```jaiscript\n" + code + "```";
	std::string about = kind_words(s->kind);
	if ((s->kind == symbol_kind::field || s->kind == symbol_kind::method) && !s->container.empty()) { about += " of `" + s->container + "`"; }
	else if (s->kind == symbol_kind::parameter && !s->container.empty()) { about += " of `" + s->container + "`"; }
	else if (s->kind == symbol_kind::local && !s->container.empty() && s->container != "lambda") { about += " in `" + s->container + "`"; }
	else if (s->kind == symbol_kind::enum_value) { about += " of `" + s->container + "`"; }
	else if (!s->container.empty() && (s->kind == symbol_kind::function || s->kind == symbol_kind::global)) { about += " in namespace `" + s->container + "`"; }
	if (!s->type.empty() && s->kind != symbol_kind::function && s->kind != symbol_kind::method && s->detail.find(s->type) == std::string::npos) { about += ", type `" + s->type + "`"; }
	if (!s->path.empty() && s->path != u.path) {
		about += " - declared in `" + file_name(s->path) + ":" + std::to_string(s->span.start.line + 1) + "`";
	}
	md += "\n\n" + about;
	if (!s->path.empty() && s->kind != symbol_kind::local && s->kind != symbol_kind::parameter) {
		const std::string path = s->path;
		const text_span span = s->span;
		if (unit* owner = path == u.path ? &u : get(path)) {
			if (std::string doc = doc_comment(*owner, span); !doc.empty()) { md += "\n\n" + doc; }
		}
	}
	return md;
}

std::vector<completion_item> workspace::completion(unit& u, text_pos pos) {
	const size_t offset = u.lines.offset(u.text, pos);
	std::string_view line = u.lines.line_text(u.text, pos.line);
	std::string_view before_on_line = line.substr(0, std::min<size_t>(pos.col, line.size()));
	bool in_string = false;
	for (size_t i = 0; i < before_on_line.size(); ++i) {
		char c = before_on_line[i];
		if (c == '\\') { ++i; continue; }
		if (c == '"') { in_string = !in_string; }
		if (!in_string && c == '/' && i + 1 < before_on_line.size() && before_on_line[i + 1] == '/') { return {}; }
	}
	if (in_string) { return {}; }

	size_t word_start = offset;
	while (word_start > 0 && ident_char(u.text[word_start - 1])) { --word_start; }
	std::string prefix = u.text.substr(word_start, offset - word_start);
	if (!prefix.empty() && std::isdigit(static_cast<unsigned char>(prefix[0]))) { return {}; }
	char c1 = word_start > 0 ? u.text[word_start - 1] : '\0';
	char c2 = word_start > 1 ? u.text[word_start - 2] : '\0';
	const bool member_ctx = c1 == '.' || (c1 == '>' && c2 == '-');
	const bool scope_ctx = c1 == ':' && c2 == ':';

	// A throwaway parse of the text with a placeholder where the name is being typed: the scope
	// and the receiver come from the same walk that builds the unit's model.
	text_pos word_pos = u.lines.position(word_start);
	std::string head = u.text.substr(0, word_start) + std::string(k_placeholder);
	std::string tail = u.text.substr(offset);
	std::string closers;
	{
		std::vector<jai::token_type> open;
		for (const auto& t : u.tokens) {
			if (!(t.pos < word_pos)) { break; }
			if (t.type == jai::token_type::left_paren || t.type == jai::token_type::left_bracket || t.type == jai::token_type::left_brace) { open.push_back(t.type); }
			else if ((t.type == jai::token_type::right_paren || t.type == jai::token_type::right_bracket || t.type == jai::token_type::right_brace) && !open.empty()) { open.pop_back(); }
		}
		size_t i = open.size();
		while (i > 0 && open[i - 1] != jai::token_type::left_brace) {
			closers += open[i - 1] == jai::token_type::left_paren ? ")" : "]";
			--i;
		}
		closers += ";";
		while (i > 0) { closers += "\n}"; --i; }
	}
	std::shared_ptr<unit> keep;
	std::shared_ptr<unit_model> captured;
	for (const std::string& text : {head + tail, head + ";" + tail, head + closers}) {
		auto m = analyze_text(u, text, keep);
		if (m->captured) { captured = m; break; }
	}

	const program& prog = program_of(u.path);
	program_view view(*this, prog, keep ? *keep : u);
	item_list items(prefix);
	auto& eng = *engine_;

	if (member_ctx || scope_ctx) {
		if (!captured) { return {}; }
		const std::string& kind = captured->capture_member_kind;
		const std::string& recv = captured->capture_member_receiver;
		const auto& builtins = builtin_method_names(eng);
		if (kind == "object" || kind == "class") {
			add_class_members(view, items, recv, kind == "class");
			if (kind == "object") {
				for (const char* n : {"use_count", "unique", "same_as", "reset"}) { items.add(n, 2, std::string("shared_ptr builtin ") + n, 8); }
			}
		} else if (kind == "enum") {
			if (const jai::enum_decl* e = view.find_enum(recv)) {
				for (size_t i = 0; i < e->values.size(); ++i) { items.add(std::string(e->values[i].first), 20, recv + "." + std::string(e->values[i].first) + " = " + std::to_string(e->numbers[i]), 1); }
			}
		} else if (kind == "namespace") {
			add_namespace_members(view, items, recv);
		} else if (kind == "array") {
			for (const auto& n : builtins.array) { items.add(n, 2, "array." + n, 1); }
		} else if (kind == "map") {
			for (const auto& n : builtins.map) { items.add(n, 2, "map." + n, 1); }
		} else if (kind == "string") {
			for (const auto& n : builtins.string) { items.add(n, 2, "string." + n, 1); }
		} else if (kind == "weak") {
			for (const auto& n : builtins.weak) { items.add(n, 2, "weak_ptr." + n, 1); }
		}
		return items.take();
	}

	if (captured) {
		for (uint32_t index : captured->capture_locals) {
			const symbol& s = captured->symbols[index];
			items.add(s.name, s.kind == symbol_kind::function ? 3 : 6, s.detail, 0);
		}
		if (!captured->class_at_capture.empty()) { add_class_members(view, items, captured->class_at_capture, false); }
		for (auto it = captured->namespaces_at_capture.rbegin(); it != captured->namespaces_at_capture.rend(); ++it) { add_namespace_members(view, items, *it); }
	}
	for (unit* x : view.units()) {
		const unit_index& idx = view.index_of(*x);
		for (const auto& [name, entries] : idx.names) {
			if (entries.empty() || name.find("::") != std::string::npos) { continue; }
			const auto& e = entries.front();
			std::string detail;
			switch (e.node->get_type()) {
				case jai::node_type::function_decl: detail = function_signature(*static_cast<const jai::function_decl*>(e.node), name, false).label; break;
				case jai::node_type::variable_decl: detail = type_text(static_cast<const jai::variable_decl*>(e.node)->type.get()) + " " + name; break;
				case jai::node_type::class_decl: detail = view.class_detail(static_cast<const jai::class_decl*>(e.node)); break;
				case jai::node_type::enum_decl: detail = "enum " + name; break;
				case jai::node_type::namespace_decl: detail = "namespace " + name; break;
				default: detail = name; break;
			}
			items.add(name, completion_kind(e.kind), detail, x == &view.current ? 2 : 3);
		}
		for (const auto& [value, e] : idx.enum_values) { items.add(value, 20, std::string(e->name) + "." + value, 3); }
	}
	add_host_names(view, items);
	for (auto word : language_keywords()) { items.add(std::string(word), 14, "keyword", 6); }
	items.add("instanceof", 14, "keyword", 6);   // an operator after an operand, a name anywhere else
	items.add("make_shared", 14, "make_shared<T>(args): the same expression as new T(args)", 6);
	return items.take();
}

std::optional<signature_result> workspace::signature_help(unit& u, text_pos pos) {
	size_t end = token_at_or_after(u.tokens, pos);
	int depth = 0;
	uint32_t commas = 0;
	size_t open = SIZE_MAX;
	for (size_t i = end; i-- > 0;) {
		auto t = u.tokens[i].type;
		if (t == jai::token_type::right_paren || t == jai::token_type::right_bracket) { ++depth; continue; }
		if (t == jai::token_type::left_bracket) { if (depth > 0) { --depth; } continue; }
		if (t == jai::token_type::left_paren) {
			if (depth == 0) { open = i; break; }
			--depth;
			continue;
		}
		if (depth == 0 && t == jai::token_type::comma) { ++commas; }
		if (depth == 0 && (t == jai::token_type::semicolon || t == jai::token_type::left_brace || t == jai::token_type::right_brace)) { return std::nullopt; }
	}
	if (open == SIZE_MAX || open == 0) { return std::nullopt; }
	if (u.tokens[open - 1].type != jai::token_type::identifier) {
		const auto before = open >= 2 ? u.tokens[open - 2].type : jai::token_type::eof;
		const bool qualified = before == jai::token_type::dot || before == jai::token_type::arrow || before == jai::token_type::question_dot ||
		                       before == jai::token_type::colon_colon || before == jai::token_type::super_keyword;
		if (!qualified || !is_keyword(u.tokens[open - 1].lexeme)) { return std::nullopt; }   // p.catch( is a member call
	}
	const lex_token& callee = u.tokens[open - 1];

	auto lookup = [&](const unit_model& m) -> const symbol* {
		for (const auto& o : m.occurrences) {
			if (o.span.start == callee.pos) { return &m.symbols[o.symbol]; }
		}
		return nullptr;
	};
	const symbol* s = nullptr;
	std::shared_ptr<unit> keep;
	std::shared_ptr<unit_model> temp;
	if (u.parse_ok) { s = lookup(model(u)); }
	if (!s) {
		size_t offset = u.lines.offset(u.text, pos);
		std::string head = u.text.substr(0, offset) + std::string(k_placeholder);
		std::string closers;
		std::vector<jai::token_type> stack;
		for (size_t i = 0; i < end; ++i) {
			auto t = u.tokens[i].type;
			if (t == jai::token_type::left_paren || t == jai::token_type::left_bracket || t == jai::token_type::left_brace) { stack.push_back(t); }
			else if ((t == jai::token_type::right_paren || t == jai::token_type::right_bracket || t == jai::token_type::right_brace) && !stack.empty()) { stack.pop_back(); }
		}
		size_t i = stack.size();
		while (i > 0 && stack[i - 1] != jai::token_type::left_brace) { closers += stack[i - 1] == jai::token_type::left_paren ? ")" : "]"; --i; }
		closers += ";";
		while (i > 0) { closers += "\n}"; --i; }
		temp = analyze_text(u, head + closers, keep);
		s = lookup(*temp);
	}
	if (!s) { return std::nullopt; }

	std::vector<std::string> labels = s->signatures;
	if (s->kind == symbol_kind::class_type) {
		program_view view(*this, program_of(u.path), u);
		if (const jai::class_decl* cd = view.find_class(s->name)) { labels = view.method_signatures(cd, cd->name, true); }
		if (labels.empty()) { labels.push_back(s->name + "()"); }
	} else if (s->kind == symbol_kind::host_class) {
		labels = {s->name + "(...)"};
	}
	if (labels.empty()) { return std::nullopt; }
	signature_result result;
	for (const auto& l : labels) { result.signatures.push_back(signature_from_label(l)); }
	result.active_parameter = commas;
	for (size_t i = 0; i < result.signatures.size(); ++i) {
		if (result.signatures[i].parameters.size() > commas) { result.active_signature = static_cast<uint32_t>(i); break; }
	}
	return result;
}

std::vector<std::string> workspace::rename_group(unit& u, const symbol& s) {
	std::vector<std::string> keys{s.key};
	if (s.kind != symbol_kind::method && s.kind != symbol_kind::field) { return keys; }
	program_view view(*this, program_of(s.path.empty() ? u.path : s.path), u);
	// every class of the hierarchy that declares the name: an override renames with its base
	std::set<std::string> related{s.container};
	bool grew = true;
	while (grew) {
		grew = false;
		for (unit* x : view.units()) {
			for (const auto& [name, cd] : view.index_of(*x).classes) {
				for (auto base : cd->base_classes) {
					std::string b(base), c(cd->name);
					if (related.count(b) && !related.count(c)) { related.insert(c); grew = true; }
					if (related.count(c) && !related.count(b) && view.find_class(b)) { related.insert(b); grew = true; }
				}
			}
		}
	}
	for (const auto& cls : related) {
		if (cls == s.container) { continue; }
		if (const jai::class_decl* cd = view.find_class(cls)) {
			for (const auto& m : cd->members) {
				if (!m.declaration) { continue; }
				std::string_view n = m.declaration->get_type() == jai::node_type::variable_decl ? static_cast<const jai::variable_decl*>(m.declaration.get())->name
				                   : m.declaration->get_type() == jai::node_type::function_decl ? static_cast<const jai::function_decl*>(m.declaration.get())->name : std::string_view{};
				if (n == s.name) { keys.push_back("m:" + cls + "." + s.name); break; }
			}
		}
	}
	return keys;
}

std::optional<text_span> workspace::prepare_rename(unit& u, text_pos pos, std::string& error) {
	uint32_t index = 0;
	const symbol* s = symbol_at(u, pos, &index);
	if (!s) { error = "No renameable symbol here."; return std::nullopt; }
	if (s->path.empty()) { error = "'" + s->name + "' is defined by the host or the language; it cannot be renamed here."; return std::nullopt; }
	const unit_model& m = model(u);
	for (const auto& o : m.occurrences) {
		if (o.symbol == index && o.span.contains(pos)) { return o.span; }
	}
	return std::nullopt;
}

rename_result workspace::rename(unit& u, text_pos pos, const std::string& new_name) {
	rename_result result;
	const symbol* found = symbol_at(u, pos);
	if (!found) { result.error = "No renameable symbol here."; return result; }
	symbol s = *found;
	if (s.path.empty()) { result.error = "'" + s.name + "' is defined by the host or the language; it cannot be renamed here."; return result; }
	if (!is_identifier_text(new_name) || is_keyword(new_name)) { result.error = "'" + new_name + "' is not a valid name."; return result; }
	if (new_name == s.name) { return result; }
	std::vector<std::string> scope = reference_scope(u, s);
	program_view view(*this, program_of(s.path), u);

	if (s.kind == symbol_kind::field || s.kind == symbol_kind::method) {
		size_t unknown = 0;
		std::string first;
		for (const auto& p : scope) {
			unit* x = get(p);
			if (!x) { continue; }
			for (const auto& um : model(*x).unresolved_members) {
				if (um.name != s.name) { continue; }
				if (unknown++ == 0) { first = file_name(p) + ":" + std::to_string(um.span.start.line + 1) + ":" + std::to_string(um.span.start.col + 1); }
			}
		}
		if (unknown) {
			result.error = "Cannot rename '" + s.name + "': " + std::to_string(unknown) + " use(s) of '." + s.name +
				"' on a receiver of unknown type (var or dynamic) cannot be proven to be this member (first at " + first + ").";
			return result;
		}
		if (view.find_member(s.container, new_name, false).found) {
			result.error = "Cannot rename '" + s.name + "': class '" + s.container + "' already has a member '" + new_name + "'.";
			return result;
		}
	} else if (s.key.rfind("g:", 0) == 0) {
		std::string qualified = s.key.substr(2);
		std::set<std::string> declaring;
		for (const auto& r : view.top_level(qualified)) { if (!r.entry->is_extern) { declaring.insert(file_name(r.owner->path)); } }
		if (declaring.size() > 1 && s.kind != symbol_kind::namespace_name) {
			std::string files;
			for (const auto& f : declaring) { files += (files.empty() ? "" : ", ") + f; }
			result.error = "Cannot rename '" + s.name + "': it is declared in more than one file (" + files + "); each file sees its own declaration.";
			return result;
		}
		std::string target = s.container.empty() ? new_name : s.container + "::" + new_name;
		if (view.has_top(target) || engine_->has_function(target) || engine_->has_variable(target) || jai::detail::engine_access::get_class_definition(*engine_, target)) {
			result.error = "Cannot rename '" + s.name + "': '" + new_name + "' is already declared in this program.";
			return result;
		}
	} else if (s.key.rfind("l:", 0) == 0) {
		const unit_model& m = model(u);
		for (const auto& region : m.scopes) {
			bool holds = false;
			for (const auto& l : region.locals) { if (m.symbols[l.symbol].key == s.key) { holds = true; break; } }
			if (!holds) { continue; }
			for (const auto& l : region.locals) {
				if (m.symbols[l.symbol].name == new_name) {
					result.error = "Cannot rename '" + s.name + "': '" + new_name + "' is already declared in the same scope.";
					return result;
				}
			}
		}
	}
	for (auto& loc : collect_references(scope, rename_group(u, s), true)) { result.edits.push_back(std::move(loc)); }
	return result;
}

std::vector<document_symbol> workspace::document_symbols(unit& u) {
	std::vector<document_symbol> out;
	std::function<void(const std::vector<jai::declaration_ptr>&, std::vector<document_symbol>&, const std::string&)> walk;
	auto span_of = [&](const jai::declaration* d, std::string_view name) -> std::optional<text_span> {
		text_span s = name_span(d->location, name);
		std::string_view line = u.lines.line_text(u.text, s.start.line);
		if (s.end.col <= line.size() && line.substr(s.start.col, s.end.col - s.start.col) == name) { return s; }
		return find_name_after(u.tokens, s.start, name, 32);
	};
	auto range_of = [&](text_span sel) {
		text_span r = sel;
		if (auto e = block_end_after(u.tokens, sel.end)) { r.end = *e; }
		return r;
	};
	auto function_symbol = [&](const jai::function_decl* fn, int kind) -> std::optional<document_symbol> {
		auto sel = span_of(fn, fn->name);
		if (!sel) { return std::nullopt; }
		document_symbol ds;
		ds.name = std::string(fn->name);
		ds.detail = function_signature(*fn, fn->name, kind == 9).label;
		ds.kind = kind;
		ds.selection = *sel;
		ds.range = range_of(*sel);
		return ds;
	};
	walk = [&](const std::vector<jai::declaration_ptr>& decls, std::vector<document_symbol>& into, const std::string& ns) {
		for (const auto& d : decls) {
			if (!d) { continue; }
			switch (d->get_type()) {
				case jai::node_type::function_decl:
					if (auto ds = function_symbol(static_cast<const jai::function_decl*>(d.get()), 12)) { into.push_back(std::move(*ds)); }
					break;
				case jai::node_type::variable_decl: {
					auto* v = static_cast<const jai::variable_decl*>(d.get());
					if (auto sel = span_of(v, v->name)) {
						document_symbol ds{std::string(v->name), type_text(v->type.get()), 13, *sel, *sel, {}};
						into.push_back(std::move(ds));
					}
					break;
				}
				case jai::node_type::extern_decl: {
					auto* x = static_cast<const jai::extern_decl*>(d.get());
					if (auto sel = find_name_after(u.tokens, name_span(x->location, "").start, x->name, 16)) {
						into.push_back({std::string(x->name), "extern", x->is_function ? 12 : 13, *sel, *sel, {}});
					}
					break;
				}
				case jai::node_type::class_decl: {
					auto* cd = static_cast<const jai::class_decl*>(d.get());
					auto sel = span_of(cd, cd->name);
					if (!sel) { break; }
					document_symbol ds{std::string(cd->name), cd->base_classes.empty() ? std::string() : ": " + std::string(cd->base_classes[0]), 5, range_of(*sel), *sel, {}};
					for (const auto& m : cd->members) {
						if (!m.declaration) { continue; }
						if (m.declaration->get_type() == jai::node_type::variable_decl) {
							auto* f = static_cast<const jai::variable_decl*>(m.declaration.get());
							if (auto fs = span_of(f, f->name)) { ds.children.push_back({std::string(f->name), type_text(f->type.get()), 8, *fs, *fs, {}}); }
						} else if (m.declaration->get_type() == jai::node_type::function_decl) {
							auto* fn = static_cast<const jai::function_decl*>(m.declaration.get());
							if (auto ms = function_symbol(fn, fn->name == cd->name ? 9 : 6)) { ds.children.push_back(std::move(*ms)); }
						}
					}
					into.push_back(std::move(ds));
					break;
				}
				case jai::node_type::enum_decl: {
					auto* e = static_cast<const jai::enum_decl*>(d.get());
					auto sel = span_of(e, e->name);
					if (!sel) { break; }
					document_symbol ds{std::string(e->name), "enum", 10, range_of(*sel), *sel, {}};
					text_pos cursor = sel->end;
					for (const auto& [vname, _] : e->values) {
						if (auto vs = find_name_after(u.tokens, cursor, vname, 64)) {
							cursor = vs->end;
							ds.children.push_back({std::string(vname), "", 22, *vs, *vs, {}});
						}
					}
					into.push_back(std::move(ds));
					break;
				}
				case jai::node_type::namespace_decl: {
					auto* n = static_cast<const jai::namespace_decl*>(d.get());
					std::string full(n->name);
					std::string seg = full.substr(full.rfind("::") == std::string::npos ? 0 : full.rfind("::") + 2);
					auto sel = find_name_after(u.tokens, name_span(n->location, "").start, seg, 32);
					if (!sel) { break; }
					document_symbol ds{full, "namespace", 3, range_of(*sel), *sel, {}};
					walk(n->declarations, ds.children, full);
					into.push_back(std::move(ds));
					break;
				}
				default:
					break;
			}
		}
	};
	walk(u.parse_ok ? u.decls : (u.decls.empty() ? u.good_decls : u.decls), out, "");
	return out;
}

std::vector<workspace_symbol_hit> workspace::workspace_symbols(const std::string& query) {
	std::vector<workspace_symbol_hit> out;
	std::set<std::string> paths(workspace_files_.begin(), workspace_files_.end());
	for (const auto& [p, x] : units_) { if (x) { paths.insert(p); } }
	auto match = [&](std::string_view name) {
		if (query.empty()) { return true; }
		size_t j = 0;
		for (size_t i = 0; i < name.size() && j < query.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(name[i])) == std::tolower(static_cast<unsigned char>(query[j]))) { ++j; }
		}
		return j == query.size();
	};
	for (const auto& p : paths) {
		unit* x = get(p);
		if (!x) { continue; }
		for (const auto& ds : document_symbols(*x)) {
			if (match(ds.name)) { out.push_back({ds.name, "", ds.kind, {p, ds.selection}}); }
			for (const auto& child : ds.children) {
				if (match(child.name)) { out.push_back({child.name, ds.name, child.kind, {p, child.selection}}); }
			}
			if (out.size() >= 512) { return out; }
		}
	}
	return out;
}

} // namespace jai_lsp
