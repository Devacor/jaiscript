#include "analysis_internal.hpp"
#include "semantic.hpp"

#include <jaiscript/detail/less_than.hpp>
#include <jaiscript/detail/parser.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace jai_lsp {

namespace fs = std::filesystem;

namespace {

bool read_text_file(const std::string& path, std::string& out) {
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) { return false; }
	std::stringstream buffer;
	buffer << file.rdbuf();
	out = buffer.str();
	if (out.rfind("\xEF\xBB\xBF", 0) == 0) { out.erase(0, 3); }
	return true;
}

uint64_t disk_stamp_of(const std::string& path) {
	std::error_code ec;
	auto size = fs::file_size(path, ec);
	if (ec) { return 0; }
	auto time = fs::last_write_time(path, ec);
	if (ec) { return 0; }
	return static_cast<uint64_t>(time.time_since_epoch().count()) ^ (static_cast<uint64_t>(size) << 1);
}

bool punctuation(jai::token_type t) {
	return t >= jai::token_type::plus && t <= jai::token_type::comma;
}

// Top-level literal import/include paths, read without a parse (the folder scan)
std::vector<std::string> scan_imports(std::string_view text) {
	std::vector<std::string> out;
	int depth = 0;
	size_t i = 0;
	auto ident = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
	while (i < text.size()) {
		char c = text[i];
		if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
			while (i < text.size() && text[i] != '\n') { ++i; }
		} else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
			size_t end = text.find("*/", i + 2);
			i = end == std::string_view::npos ? text.size() : end + 2;
		} else if (c == '"' || c == '\'' || c == '`') {
			++i;
			while (i < text.size() && text[i] != c) { i += text[i] == '\\' ? 2 : 1; }
			++i;
		} else if (c == '{') {
			++depth;
			++i;
		} else if (c == '}') {
			--depth;
			++i;
		} else if (ident(c)) {
			size_t start = i;
			while (i < text.size() && ident(text[i])) { ++i; }
			std::string_view word = text.substr(start, i - start);
			if (depth == 0 && (word == "import" || word == "include") && (start == 0 || !ident(text[start - 1]))) {
				size_t j = i;
				while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) { ++j; }
				if (j < text.size() && (text[j] == '"' || text[j] == '<')) {
					char close = text[j] == '"' ? '"' : '>';
					size_t end = text.find(close, j + 1);
					if (end != std::string_view::npos) { out.emplace_back(text.substr(j + 1, end - j - 1)); }
				}
			}
		} else {
			++i;
		}
	}
	return out;
}

void surface_of(const std::vector<jai::declaration_ptr>& decls, std::string& out) {
	auto params = [&](const jai::function_decl* fn) {
		out += '(';
		for (const auto& p : fn->parameters) {
			out += type_text(p.type.get());
			out += p.is_reference ? "&" : "";
			out += p.default_value ? "=" : "";
			out += ',';
		}
		out += ')';
		out += type_text(fn->return_type.get());
		out += fn->is_static ? "s" : "";
		out += fn->is_coroutine ? "c" : "";
	};
	for (const auto& d : decls) {
		if (!d) { continue; }
		switch (d->get_type()) {
			case jai::node_type::function_decl: {
				auto* fn = static_cast<const jai::function_decl*>(d.get());
				out += "f " + std::string(fn->name);
				params(fn);
				out += ';';
				break;
			}
			case jai::node_type::variable_decl: {
				auto* v = static_cast<const jai::variable_decl*>(d.get());
				out += "v " + std::string(v->name) + " " + type_text(v->type.get()) + (v->function_spelling ? "F" : "") + ";";
				break;
			}
			case jai::node_type::extern_decl:
				out += "x " + std::string(static_cast<const jai::extern_decl*>(d.get())->name) + ";";
				break;
			case jai::node_type::destructuring_decl:
				for (const auto& [n, _] : static_cast<const jai::destructuring_decl*>(d.get())->names) { out += "d " + std::string(n) + ";"; }
				break;
			case jai::node_type::enum_decl: {
				auto* e = static_cast<const jai::enum_decl*>(d.get());
				out += "e " + std::string(e->name) + "{";
				for (size_t i = 0; i < e->values.size(); ++i) { out += std::string(e->values[i].first) + "=" + std::to_string(e->numbers[i]) + ","; }
				out += "}";
				break;
			}
			case jai::node_type::class_decl: {
				auto* c = static_cast<const jai::class_decl*>(d.get());
				out += "c " + std::string(c->name) + (c->nocopy ? "!" : "") + ":";
				for (auto b : c->base_classes) { out += std::string(b) + ","; }
				out += "{";
				for (const auto& m : c->members) {
					if (!m.declaration) { continue; }
					out += std::to_string(static_cast<int>(m.visibility));
					if (m.declaration->get_type() == jai::node_type::variable_decl) {
						auto* f = static_cast<const jai::variable_decl*>(m.declaration.get());
						out += std::string(f->name) + ":" + type_text(f->type.get()) + (f->is_static ? "s" : "") + (f->function_spelling ? "F" : "") + ";";
					} else if (m.declaration->get_type() == jai::node_type::function_decl) {
						auto* fn = static_cast<const jai::function_decl*>(m.declaration.get());
						out += std::string(fn->name);
						params(fn);
						out += ';';
					} else if (m.declaration->get_type() == jai::node_type::generic_decl) {
						const auto& g = *static_cast<const jai::generic_decl*>(m.declaration.get())->definition;
						out += std::string(g.name) + "<>" + std::to_string(g.stamp) + ";";
					}
				}
				out += "}";
				break;
			}
			case jai::node_type::namespace_decl: {
				auto* n = static_cast<const jai::namespace_decl*>(d.get());
				out += "n " + std::string(n->name) + "{";
				surface_of(n->declarations, out);
				out += "}";
				break;
			}
			case jai::node_type::generic_decl: {   // an importer makes its instances from the text: the whole of it is surface
				const auto& g = *static_cast<const jai::generic_decl*>(d.get())->definition;
				out += "g " + g.qualified_name() + ":" + std::to_string(g.stamp) + ";";
				break;
			}
			default:
				break;
		}
	}
}

// The declarations other units see: the parse, plus what the last clean parse declared that a failing parse lost
std::vector<jai::declaration_ptr> program_decls(const unit& u) {
	if (u.parse_ok || u.good_decls.empty()) { return u.decls; }
	std::unordered_set<std::string> present;
	jai::detail::collect_unit_names(u.decls, present);
	std::vector<jai::declaration_ptr> out = u.decls;
	for (const auto& d : u.good_decls) {
		std::unordered_set<std::string> names;
		jai::detail::collect_unit_names({d}, names);
		bool missing = !names.empty();
		for (const auto& n : names) { if (present.count(n)) { missing = false; break; } }
		if (missing) { out.push_back(d); }
	}
	return out;
}

// Top-level names a failing parse declares, read off the tokens: `class X`, `enum X`, `namespace X`,
// and `<type> name (`, `<type> name =`, `<type> name ;` at brace depth 0
std::vector<std::string> salvage_names(const std::vector<lex_token>& tokens) {
	using tt = jai::token_type;
	auto type_like = [](const lex_token& t) {
		switch (t.type) {
			case tt::identifier: case tt::int_keyword: case tt::float_keyword: case tt::string_keyword: case tt::bool_keyword:
			case tt::char_keyword: case tt::void_keyword: case tt::auto_keyword: case tt::var_keyword: case tt::function_keyword:
			case tt::greater: case tt::right_angle: case tt::right_shift: case tt::ampersand:
				return true;
			default:
				return false;
		}
	};
	std::vector<std::string> out;
	int depth = 0;
	for (size_t i = 0; i < tokens.size(); ++i) {
		const lex_token& t = tokens[i];
		if (t.type == tt::left_brace) { ++depth; continue; }
		if (t.type == tt::right_brace) { if (depth > 0) { --depth; } continue; }
		if (depth != 0 || i + 1 >= tokens.size()) { continue; }
		const lex_token& next = tokens[i + 1];
		if ((t.type == tt::class_keyword || t.type == tt::enum_keyword || t.type == tt::namespace_keyword) && next.type == tt::identifier) {
			out.emplace_back(next.lexeme);
			continue;
		}
		if (t.type == tt::identifier && i > 0 && type_like(tokens[i - 1]) &&
		    (next.type == tt::left_paren || next.type == tt::equal || next.type == tt::semicolon)) {
			out.emplace_back(t.lexeme);
		}
	}
	return out;
}

// A check declared generic instances among the declarations: their requests stand ahead of the unit's own
bool holds_instance_requests(const std::vector<jai::declaration_ptr>& decls) {
	return std::any_of(decls.begin(), decls.end(), [](const jai::declaration_ptr& d) { return d && d->get_type() == jai::node_type::generic_instance_decl; });
}

class type_collector : public jai::check_observer {
public:
	explicit type_collector(std::unordered_map<const jai::expression*, jai::check_expression_type>& out) : out_(out) {}
	void expression_type(const jai::expression* e, const jai::check_expression_type& t) override { out_[e] = t; }
private:
	std::unordered_map<const jai::expression*, jai::check_expression_type>& out_;
};

} // namespace

const generic_view& generic_view_of(jai::engine& eng, const std::shared_ptr<const jai::detail::generic_definition>& definition, generic_view_map& out) {
	if (auto held = out.find(definition.get()); held != out.end()) { return held->second; }
	generic_view& view = out[definition.get()];
	view.definition = definition;
	auto* symbols = jai::detail::engine_access::get_symbolizer(eng);
	try {
		std::vector<jai::token> text = jai::detail::generic_instance_tokens(*definition, {}, definition->name, *symbols);
		jai::parser prs(text, symbols, &eng, jai::detail::engine_access::get_registered_template_types(eng), std::string(definition->location.filename));
		auto parsed = prs.parse();
		if (!parsed || parsed.value().empty()) { return view; }
		view.holder = parsed.value().front();
	} catch (const std::exception&) {
		return view;
	}
	jai::declaration_ptr d = view.holder;
	while (d && d->get_type() == jai::node_type::namespace_decl && !static_cast<const jai::namespace_decl*>(d.get())->declarations.empty()) {
		d = static_cast<const jai::namespace_decl*>(d.get())->declarations.front();
	}
	if (d && definition->kind == jai::detail::generic_kind::method && d->get_type() == jai::node_type::class_decl) {
		const auto& members = static_cast<const jai::class_decl*>(d.get())->members;
		d = members.empty() ? nullptr : members.front().declaration;
	}
	const bool shaped = d && (definition->kind == jai::detail::generic_kind::class_type ? d->get_type() == jai::node_type::class_decl : d->get_type() == jai::node_type::function_decl);
	view.node = shaped ? d : nullptr;
	view.decl = view.node.get();
	if (view.decl && view.decl->get_type() == jai::node_type::class_decl) {
		std::vector<std::shared_ptr<const jai::detail::generic_definition>> methods;
		for (const auto& m : static_cast<const jai::class_decl*>(view.decl)->members) {
			if (m.declaration && m.declaration->get_type() == jai::node_type::generic_decl) { methods.push_back(static_cast<const jai::generic_decl*>(m.declaration.get())->definition); }
		}
		for (const auto& method : methods) { generic_view_of(eng, method, out); }   // a map's elements stay where they are as it grows
	}
	return view;
}

void collect_generic_views(jai::engine& eng, const std::vector<jai::declaration_ptr>& decls, generic_view_map& out) {
	for (const auto& d : decls) {
		if (!d) { continue; }
		switch (d->get_type()) {
			case jai::node_type::generic_decl: generic_view_of(eng, static_cast<const jai::generic_decl*>(d.get())->definition, out); break;
			case jai::node_type::namespace_decl: collect_generic_views(eng, static_cast<const jai::namespace_decl*>(d.get())->declarations, out); break;
			case jai::node_type::class_decl:
				for (const auto& m : static_cast<const jai::class_decl*>(d.get())->members) {
					if (m.declaration && m.declaration->get_type() == jai::node_type::generic_decl) {
						generic_view_of(eng, static_cast<const jai::generic_decl*>(m.declaration.get())->definition, out);
					}
				}
				break;
			default:
				break;
		}
	}
}

std::vector<file_diagnostic> parse_error_lines(const std::string& all, const std::string& path) {
	std::vector<file_diagnostic> out;
	std::istringstream in(all);
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty()) { continue; }
		file_diagnostic d;
		d.source = "parse";
		d.message = line;
		// "<file>:<line>:<col>: <message>" - the file may itself hold ':' (a drive letter)
		for (size_t pos = line.find(':'); pos != std::string::npos; pos = line.find(':', pos + 1)) {
			size_t a = pos + 1, b = a;
			while (b < line.size() && std::isdigit(static_cast<unsigned char>(line[b]))) { ++b; }
			if (b == a || b >= line.size() || line[b] != ':') { continue; }
			size_t c = b + 1, e = c;
			while (e < line.size() && std::isdigit(static_cast<unsigned char>(line[e]))) { ++e; }
			if (e == c || e >= line.size() || line[e] != ':') { continue; }
			std::string file = line.substr(0, pos);
			if (!file.empty() && file != path && canonical_path(file) != path) { break; }
			d.line = static_cast<uint32_t>(std::max(1, std::stoi(line.substr(a, b - a))) - 1);
			d.col = static_cast<uint32_t>(std::max(1, std::stoi(line.substr(c, e - c))) - 1);
			size_t msg = e + 1;
			while (msg < line.size() && line[msg] == ' ') { ++msg; }
			d.message = line.substr(msg);
			break;
		}
		const std::string prefix = "Parse error: ";
		if (d.message.rfind(prefix, 0) == 0) { d.message.erase(0, prefix.size()); }
		out.push_back(std::move(d));
	}
	return out;
}

workspace::workspace(std::shared_ptr<jai::engine> eng) : engine_(std::move(eng)) {}

void workspace::add_folder(const std::string& folder) {
	std::string canon = canonical_path(folder);
	if (std::find(folders_.begin(), folders_.end(), canon) == folders_.end()) { folders_.push_back(canon); }
}

void workspace::add_include_path(const std::string& path) {
	engine_->add_include_path(canonical_path(path));
}

void workspace::scan_folders() {
	static const std::unordered_set<std::string> skipped = {".git", "out", "build", "node_modules", ".vs", ".vscode", "dist", "obj"};
	workspace_files_.clear();
	for (const auto& folder : folders_) {
		std::error_code ec;
		fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
		for (; !ec && it != end; it.increment(ec)) {
			const auto& entry = *it;
			if (entry.is_directory(ec)) {
				if (skipped.count(entry.path().filename().string())) { it.disable_recursion_pending(); }
				continue;
			}
			if (entry.path().extension() != ".jai") { continue; }
			if (workspace_files_.size() >= 20000) { break; }
			std::string path = canonical_path(entry.path().string());
			workspace_files_.push_back(path);
			if (units_.count(path)) { continue; }
			std::string text;
			if (!read_text_file(path, text)) { continue; }
			std::vector<std::string> resolved;
			for (const auto& target : scan_imports(text)) {
				auto r = jai::resolve_include_path(target, engine_.get(), path);
				if (r) { resolved.push_back(canonical_path(r.value())); }
			}
			scanned_imports_[path] = resolved;
			set_imports(path, resolved);
		}
	}
	std::sort(workspace_files_.begin(), workspace_files_.end());
}

void workspace::set_imports(const std::string& path, const std::vector<std::string>& imports) {
	for (auto& [target, from] : importers_) { from.erase(path); }
	for (const auto& target : imports) {
		if (target != path) { importers_[target].insert(path); }
	}
}

void workspace::parse(unit& u, bool publish) {
	auto& eng = *engine_;
	u.epoch = ++epoch_clock_;
	u.lines.build(u.text);
	u.tokens.clear();
	u.parse_errors.clear();
	u.imports.clear();
	u.checked_decls.clear();
	auto registered = jai::detail::engine_access::get_registered_template_types(eng);
	std::vector<jai::token> tokens;
	std::unique_ptr<jai::lexer> lex;   // literal lexemes view the lexer's own storage: it outlives the parse
	try {
		lex = std::make_unique<jai::lexer>(u.text, jai::detail::engine_access::get_symbolizer(eng), u.path);
		tokens = lex->tokenize();
	} catch (const std::exception& ex) {
		u.decls.clear();
		u.parse_ok = false;
		u.parse_errors.push_back({0, 0, 0, true, std::string("lexer: ") + ex.what(), UINT32_MAX, 0, "parse"});
		return;
	}
	u.tokens.reserve(tokens.size());
	for (const auto& t : tokens) {
		if (t.type == jai::token_type::eof) { continue; }
		lex_token lt{t.type, {static_cast<uint32_t>(t.location.line ? t.location.line - 1 : 0), static_cast<uint32_t>(t.location.column ? t.location.column - 1 : 0)}, {}, static_cast<uint32_t>(t.lexeme.size())};
		if (t.type == jai::token_type::identifier || t.is_keyword() || punctuation(t.type)) { lt.lexeme = t.lexeme; }
		u.tokens.push_back(lt);
	}
	std::stable_sort(u.tokens.begin(), u.tokens.end(), [](const lex_token& a, const lex_token& b) { return a.pos < b.pos; });
	u.salvaged.clear();
	try {
		jai::parser prs(tokens, jai::detail::engine_access::get_symbolizer(eng), &eng, registered, u.path);
		prs.less_provisional(!u.settling);
		auto result = prs.parse();
		u.less_queries = prs.less_queries();
		u.less_answers = prs.less_answers();
		u.less_settled = prs.less_settled();
		u.namespace_queries = prs.namespace_queries();
		u.namespace_answers = namespace_answers(u);
		if (result) {
			u.decls = std::move(result.value());
			u.parse_ok = true;
			u.good_decls = u.decls;
		} else {
			u.decls = std::move(prs.recovered_declarations());
			u.parse_ok = false;
			u.salvaged = salvage_names(u.tokens);
			jai::detail::run_ast_marking_passes(u.decls, jai::detail::engine_access::get_symbolizer(eng));
			std::string all;
			for (const auto& e : prs.get_errors()) { all += e + "\n"; }
			if (all.empty()) { all = eng.format_error(result); }
			u.parse_errors = parse_error_lines(all, u.path);
		}
	} catch (const std::exception& ex) {
		u.decls.clear();
		u.parse_ok = false;
		u.parse_errors.push_back({0, 0, 0, true, std::string("parser: ") + ex.what(), UINT32_MAX, 0, "parse"});
	}
	u.generic_views.clear();
	u.instance_views.clear();
	collect_generic_views(eng, u.decls, u.generic_views);
	if (!u.parse_ok) { collect_generic_views(eng, u.good_decls, u.generic_views); }
	if (!publish) { return; }
	for (const auto& d : u.decls) {
		if (!d) { continue; }
		std::string target;
		if (d->get_type() == jai::node_type::import_decl && !static_cast<const jai::import_decl*>(d.get())->path_expr) {
			target = static_cast<const jai::import_decl*>(d.get())->path;
		} else if (d->get_type() == jai::node_type::include_decl && !static_cast<const jai::include_decl*>(d.get())->path_expr) {
			target = static_cast<const jai::include_decl*>(d.get())->path;
		} else {
			continue;
		}
		auto r = jai::resolve_include_path(target, &eng, u.path);
		if (r) { u.imports.push_back(canonical_path(r.value())); }
	}
	if (!u.parse_ok) {
		for (const auto& d : u.good_decls) {   // a failing parse keeps its program: imports the clean parse had
			if (!d || (d->get_type() != jai::node_type::import_decl && d->get_type() != jai::node_type::include_decl)) { continue; }
			const std::string& target = d->get_type() == jai::node_type::import_decl ? static_cast<const jai::import_decl*>(d.get())->path : static_cast<const jai::include_decl*>(d.get())->path;
			if (target.empty()) { continue; }
			auto r = jai::resolve_include_path(target, &eng, u.path);
			if (r) {
				std::string p = canonical_path(r.value());
				if (std::find(u.imports.begin(), u.imports.end(), p) == u.imports.end()) { u.imports.push_back(p); }
			}
		}
	}
	set_imports(u.path, u.imports);
	std::string surface;
	surface_of(program_decls(u), surface);
	for (const auto& n : u.salvaged) { surface += "s " + n + ";"; }
	u.surface = fnv1a(surface);
	u.model.reset();
}

unit& workspace::open(const std::string& path, std::string text, int version) {
	auto& slot = units_[path];
	if (!slot) { slot = std::make_unique<unit>(); slot->path = path; }
	slot->open = true;
	slot->version = version;
	if (slot->text != text || slot->tokens.empty()) {
		slot->text = std::move(text);
		parse(*slot);
	}
	return *slot;
}

unit& workspace::change(const std::string& path, std::string text, int version) {
	auto& slot = units_[path];
	if (!slot) { slot = std::make_unique<unit>(); slot->path = path; slot->open = true; }
	slot->version = version;
	if (slot->text != text) {
		slot->text = std::move(text);
		parse(*slot);
	}
	return *slot;
}

void workspace::close(const std::string& path) {
	auto it = units_.find(path);
	if (it == units_.end()) { return; }
	it->second->open = false;
	it->second->disk_stamp = 0;   // the disk's text takes over at the next use
}

void workspace::file_changed_on_disk(const std::string& path) {
	auto it = units_.find(path);
	if (it != units_.end() && !it->second->open) { it->second->disk_stamp = 0; }
	if (it == units_.end()) {
		std::string text;
		if (read_text_file(path, text)) {
			std::vector<std::string> resolved;
			for (const auto& target : scan_imports(text)) {
				auto r = jai::resolve_include_path(target, engine_.get(), path);
				if (r) { resolved.push_back(canonical_path(r.value())); }
			}
			set_imports(path, resolved);
			if (std::find(workspace_files_.begin(), workspace_files_.end(), path) == workspace_files_.end()) { workspace_files_.push_back(path); }
		} else {
			set_imports(path, {});
		}
	}
}

unit* workspace::find(const std::string& path) {
	auto it = units_.find(path);
	return it == units_.end() ? nullptr : it->second.get();
}

unit* workspace::get(const std::string& path) {
	auto& slot = units_[path];
	if (slot && slot->open) { return slot.get(); }
	const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (slot && slot->disk_stamp != 0 && now - slot->disk_checked_at < 2.0) { return slot.get(); }   // the watcher reports edits; a stat per use is not needed
	uint64_t stamp = disk_stamp_of(path);
	if (slot) { slot->disk_checked_at = now; }
	if (slot && slot->disk_stamp == stamp && stamp != 0) { return slot.get(); }
	std::string text;
	if (!read_text_file(path, text)) {
		if (slot && !slot->tokens.empty()) { return slot.get(); }
		units_.erase(path);
		return nullptr;
	}
	if (!slot) { slot = std::make_unique<unit>(); slot->path = path; }
	slot->disk_stamp = stamp;
	slot->disk_checked_at = now;
	if (slot->text != text || slot->tokens.empty()) {
		slot->text = std::move(text);
		parse(*slot);
	}
	return slot.get();
}

std::vector<std::string> workspace::roots_of(const std::string& path) {
	std::vector<std::string> roots;
	std::unordered_set<std::string> seen{path};
	std::deque<std::string> queue{path};
	while (!queue.empty()) {
		std::string p = queue.front();
		queue.pop_front();
		auto it = importers_.find(p);
		bool imported = false;
		if (it != importers_.end()) {
			for (const auto& from : it->second) {
				imported = true;
				if (seen.insert(from).second) { queue.push_back(from); }
			}
		}
		if (!imported) { roots.push_back(p); }
	}
	if (roots.empty()) { roots.push_back(path); }
	std::sort(roots.begin(), roots.end());
	return roots;
}

const program& workspace::program_of(const std::string& path) {
	get(path);
	std::vector<std::string> roots = roots_of(path);
	std::string key;
	for (const auto& r : roots) { key += r + "|"; }
	std::vector<std::string> closure;
	std::unordered_set<std::string> visited;
	std::vector<std::string> stack;
	for (auto it = roots.rbegin(); it != roots.rend(); ++it) { stack.push_back(*it); }
	while (!stack.empty()) {
		std::string p = stack.back();
		stack.pop_back();
		if (!visited.insert(p).second) { continue; }
		unit* u = get(p);
		if (!u) { continue; }
		closure.push_back(p);
		for (auto it = u->imports.rbegin(); it != u->imports.rend(); ++it) {
			if (!visited.count(*it)) { stack.push_back(*it); }
		}
	}
	if (std::find(closure.begin(), closure.end(), path) == closure.end() && get(path)) { closure.push_back(path); }
	uint64_t signature = 1469598103934665603ull;
	for (const auto& p : closure) {
		signature = fnv1a(p, signature);
		unit* u = find(p);
		uint64_t s = u ? u->surface : 0;
		signature = fnv1a(std::string_view(reinterpret_cast<const char*>(&s), sizeof(s)), signature);
	}
	program& prog = programs_[key];
	if (prog.signature != signature || prog.units != closure) {
		prog.roots = roots;
		prog.units = closure;
		prog.signature = signature;
		prog.names.clear();
		prog.classes.clear();
		prog.enums.clear();
		prog.functions.clear();
		prog.globals.clear();
		for (const auto& p : closure) {
			unit* u = find(p);
			if (!u) { continue; }
			auto decls = program_decls(*u);
			jai::detail::collect_unit_names(decls, prog.names);
			jai::detail::collect_unit_classes(decls, prog.classes);
			jai::detail::collect_unit_enums(decls, p, prog.enums);
			jai::detail::collect_unit_functions(decls, p, prog.functions);
			jai::detail::collect_unit_globals(decls, p, prog.globals);
			prog.names.insert(u->salvaged.begin(), u->salvaged.end());
		}
	}
	return prog;
}

std::vector<const unit*> workspace::loaded_units() const {
	std::vector<const unit*> out;
	for (const auto& [path, u] : units_) { if (u) { out.push_back(u.get()); } }
	std::sort(out.begin(), out.end(), [](const unit* a, const unit* b) { return a->path < b->path; });
	return out;
}

std::vector<std::string> workspace::program_units(const std::string& path) {
	return program_of(path).units;
}

// A bare name in a namespace's code is that namespace's class or enum when any unit of the program declares one: the parser
// asks the engine, which answers from the program handed to it here
void workspace::pool_classes(const program& prog) {
	if (pooled_program_ == prog.signature) { return; }
	jai::detail::engine_access::pool_program_classes(*engine_, prog.classes, prog.enums, prog.names, prog.functions, prog.globals);
	// the generics too are the program's alone: two programs that each declare largest<T> are no overloads of one another
	jai::detail::engine_access::forget_generics(*engine_);
	for (const auto& p : prog.units) {
		if (unit* u = find(p)) { jai::detail::engine_access::register_generics(*engine_, program_decls(*u), u->path); }
	}
	pooled_program_ = prog.signature;
}

std::string workspace::namespace_answers(const unit& u) const {
	std::string result;
	for (const auto& query : u.namespace_queries) { result.push_back(jai::detail::engine_access::namespace_class_known(*engine_, query) ? '1' : '0'); }
	return result;
}

// A '<' site of the unit reads otherwise now that its program is pooled
bool workspace::less_moved(unit& u) {
	if (u.less_queries.empty()) { return false; }
	std::string now = jai::detail::engine_access::less_than_answers(*engine_, u.decls, u.less_queries);
	if (!jai::detail::less_readings_equal(u.less_answers, u.less_settled, now, true)) { return true; }
	u.less_answers = std::move(now);
	u.less_settled = true;
	return false;
}

void workspace::analyze(unit& u, bool want_model) {
	const program* program_now = &program_of(u.path);
	if (u.checked_epoch != u.epoch || u.checked_program != program_now->signature) {
		pool_classes(*program_now);
		// a check that made instances changed the parse it read (a request marked made, a call renamed to its instance): under a
		// program whose generics may have moved since, the unit is checked from a fresh parse
		const bool made_for_another = u.checked_epoch == u.epoch && holds_instance_requests(u.checked_decls);
		if (made_for_another || namespace_answers(u) != u.namespace_answers || less_moved(u)) {   // parsed before its program's classes and names were known
			u.settling = true;
			parse(u);
			u.settling = false;
			program_now = &program_of(u.path);
			pool_classes(*program_now);
		}
	}
	const program& prog = *program_now;
	const bool diagnostics_fresh = u.checked_epoch == u.epoch && u.checked_program == prog.signature;
	const bool model_fresh = u.model && u.model_epoch == u.epoch && u.model_program == prog.signature;
	if (diagnostics_fresh && (!want_model || model_fresh)) { return; }

	std::vector<std::string> extra;
	if (!u.parse_ok) {
		std::unordered_set<std::string> names(u.salvaged.begin(), u.salvaged.end());
		jai::detail::collect_unit_names(u.good_decls, names);
		extra.assign(names.begin(), names.end());
	}
	auto model = want_model ? std::make_shared<unit_model>() : nullptr;
	std::unique_ptr<type_collector> collector;
	jai::detail::check_options options;
	options.diagnostics = true;
	options.proofs = jai::detail::check_options::proofs_t::leave;
	options.program_names = &prog.names;
	options.program_classes = &prog.classes;
	options.program_enums = &prog.enums;
	if (model) {
		collector = std::make_unique<type_collector>(model->types);
		options.observer = collector.get();
	}
	auto start = std::chrono::steady_clock::now();
	jai::check_report report;
	std::string failure;
	try {
		// the check declares the generic instances the unit uses, in a copy: the features read the unit's own declarations
		if (u.checked_decls.empty()) { u.checked_decls = u.decls; }
		report = jai::detail::engine_access::check_expanding(*engine_, u.checked_decls, options, extra.empty() ? nullptr : &extra);
	} catch (const std::exception& ex) {
		failure = ex.what();
	}
	u.last_check_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	++u.checks;

	u.diagnostics = u.parse_errors;
	if (!failure.empty()) {
		u.diagnostics.push_back({0, 0, 0, true, "the checker stopped: " + failure, UINT32_MAX, 0, "check"});
	}
	// Behind a syntax error the parser resynchronizes mid-declaration: what it read there (the tail of a
	// broken function as top-level statements) says nothing about the program. Checker findings stay
	// before the first error and inside whole declarations the parser picked up after the last one.
	std::vector<std::pair<size_t, size_t>> clean;
	size_t first_error = SIZE_MAX, last_error = 0;
	for (const auto& e : u.parse_errors) {
		first_error = std::min<size_t>(first_error, e.line + 1);
		last_error = std::max<size_t>(last_error, e.line + 1);
	}
	if (!u.parse_ok && first_error != SIZE_MAX) {
		for (size_t i = 0; i < u.decls.size(); ++i) {
			const auto* d = u.decls[i].get();
			if (!d || d->location.line <= last_error) { continue; }
			auto t = d->get_type();
			if (t != jai::node_type::function_decl && t != jai::node_type::class_decl && t != jai::node_type::namespace_decl && t != jai::node_type::enum_decl) { continue; }
			size_t end = i + 1 < u.decls.size() && u.decls[i + 1] ? u.decls[i + 1]->location.line : SIZE_MAX;
			clean.emplace_back(d->location.line, end);
		}
	}
	auto keep = [&](size_t line) {
		if (u.parse_ok || first_error == SIZE_MAX || line < first_error) { return true; }
		for (const auto& [a, b] : clean) { if (line >= a && line < b) { return true; } }
		return false;
	};
	for (const auto& d : report.diagnostics) {
		if (!keep(d.line)) { continue; }
		file_diagnostic fd;
		fd.line = d.line ? static_cast<uint32_t>(d.line - 1) : 0;
		fd.col = d.column ? static_cast<uint32_t>(d.column - 1) : 0;
		fd.error = d.severity == jai::check_diagnostic::level::error;
		fd.message = d.message;
		if (!d.file.empty()) {   // a template's own line: shown at the unit's start, naming the template's place
			fd.line = 0;
			fd.col = 0;
			fd.message = d.file + ":" + std::to_string(d.line) + ":" + std::to_string(d.column) + ": " + d.message;
		}
		if (d.related_line) {
			fd.related_line = static_cast<uint32_t>(d.related_line - 1);
			fd.related_col = d.related_column ? static_cast<uint32_t>(d.related_column - 1) : 0;
		}
		fd.source = "check";
		u.diagnostics.push_back(std::move(fd));
	}
	if (report.capped) {
		u.diagnostics.push_back({0, 0, 0, false, "diagnostics capped at " + std::to_string(jai::detail::k_check_diagnostic_cap), UINT32_MAX, 0, "check"});
	}
	u.checked_epoch = u.epoch;
	u.checked_program = prog.signature;
	if (model) {
		semantic_walker(*this, u, prog, *model).run();
		u.model = model;
		u.model_epoch = u.epoch;
		u.model_program = prog.signature;
	}
}

std::shared_ptr<unit_model> workspace::analyze_text(unit& base, std::string text, std::shared_ptr<unit>& keep) {
	const program& prog = program_of(base.path);
	keep = std::make_shared<unit>();
	unit& temp = *keep;
	temp.path = base.path;
	temp.text = std::move(text);
	temp.good_decls = base.parse_ok ? base.decls : base.good_decls;
	pool_classes(prog);
	parse(temp, false);
	auto model = std::make_shared<unit_model>();
	type_collector collector(model->types);
	jai::detail::check_options options;
	options.diagnostics = false;
	options.proofs = jai::detail::check_options::proofs_t::leave;
	options.program_names = &prog.names;
	options.program_classes = &prog.classes;
	options.program_enums = &prog.enums;
	options.observer = &collector;
	try {
		temp.checked_decls = temp.decls;
		jai::detail::engine_access::check_expanding(*engine_, temp.checked_decls, options);
	} catch (const std::exception&) {
	}
	semantic_walker(*this, temp, prog, *model).run();
	return model;
}

const std::vector<file_diagnostic>& workspace::diagnostics(unit& u) {
	analyze(u, false);
	return u.diagnostics;
}

const unit_model& workspace::model(unit& u) {
	analyze(u, true);
	return *u.model;
}

} // namespace jai_lsp
