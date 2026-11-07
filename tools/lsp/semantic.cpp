#include "semantic.hpp"

#include <jaiscript/core/class_definition.hpp>
#include <jaiscript/core/class_registry.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/math_intrinsics.hpp>

namespace jai_lsp {

namespace {

text_pos pos_of(const jai::source_location& loc) {
	return {static_cast<uint32_t>(loc.line ? loc.line - 1 : 0), static_cast<uint32_t>(loc.column ? loc.column - 1 : 0)};
}

// Punctuation nodes (a block's '{', a call's '(') carry the position just past their token
text_pos punct_pos(const jai::source_location& loc) {
	text_pos p = pos_of(loc);
	if (p.col > 0) { --p.col; }
	return p;
}

std::string last_segment(std::string_view q) {
	size_t pos = q.rfind("::");
	return std::string(pos == std::string_view::npos ? q : q.substr(pos + 2));
}

std::string prefix_of(std::string_view q) {
	size_t pos = q.rfind("::");
	return pos == std::string_view::npos ? std::string() : std::string(q.substr(0, pos));
}

bool span_spells(const unit& u, text_span span, std::string_view name) {
	if (span.start.line != span.end.line || span.end.col < span.start.col) { return false; }
	std::string_view line = u.lines.line_text(u.text, span.start.line);
	return span.end.col <= line.size() && line.substr(span.start.col, span.end.col - span.start.col) == name;
}

bool engine_instance_member(jai::engine& eng, const jai::class_definition& def, std::string_view member) {
	const uint64_t id = jai::detail::engine_access::symbolize(eng, member);
	const uint64_t getter_id = jai::detail::engine_access::symbolize(eng, "_get_" + std::string(member));
	return !def.get_method(id, false).is_invalid() || def.find_field_declaring_class(id) != nullptr ||
	       !def.get_method(getter_id, false).is_invalid() ||
	       def.is_static_field(id) || def.has_static_field(id) || def.has_static_method(id);
}

bool auto_spelled(const jai::type_info* t) {
	if (!t) { return true; }
	if (t->base_type == jai::script_value_type::jai_reference_type) { return t->type_params.empty() || !t->type_params[0]; }
	return false;
}

bool var_spelled(const jai::type_info* t) {
	if (!t) { return false; }
	if (t->base_type == jai::script_value_type::jai_reference_type) { return !t->type_params.empty() && t->type_params[0] && t->type_params[0]->base_type == jai::script_value_type::jai_any_type; }
	return t->base_type == jai::script_value_type::jai_any_type;
}

} // namespace

uint32_t add_symbol(unit_model& m, symbol s) {
	auto it = m.by_key.find(s.key);
	if (it != m.by_key.end()) { return it->second; }
	uint32_t index = static_cast<uint32_t>(m.symbols.size());
	m.by_key.emplace(s.key, index);
	m.symbols.push_back(std::move(s));
	return index;
}

void build_unit_index(const std::vector<jai::declaration_ptr>& decls, const std::string& prefix, unit_index& out) {
	for (const auto& d : decls) {
		if (!d) { continue; }
		switch (d->get_type()) {
			case jai::node_type::function_decl: {
				auto* fn = static_cast<const jai::function_decl*>(d.get());
				out.names[prefix + std::string(fn->name)].push_back({fn, symbol_kind::function, fn->name});
				break;
			}
			case jai::node_type::variable_decl: {
				auto* v = static_cast<const jai::variable_decl*>(d.get());
				out.names[prefix + std::string(v->name)].push_back({v, symbol_kind::global, v->name});
				break;
			}
			case jai::node_type::extern_decl: {
				auto* x = static_cast<const jai::extern_decl*>(d.get());
				out.names[prefix + std::string(x->name)].push_back({x, x->is_function ? symbol_kind::function : symbol_kind::global, x->name, true});
				break;
			}
			case jai::node_type::destructuring_decl:
				for (const auto& [n, _] : static_cast<const jai::destructuring_decl*>(d.get())->names) {
					out.names[prefix + std::string(n)].push_back({d.get(), symbol_kind::global, n});
				}
				break;
			case jai::node_type::class_decl: {
				auto* c = static_cast<const jai::class_decl*>(d.get());
				out.names[prefix + std::string(c->name)].push_back({c, symbol_kind::class_type, c->name});
				if (!prefix.empty()) { out.names[std::string(c->name)].push_back({c, symbol_kind::class_type, c->name}); }
				out.classes[std::string(c->name)] = c;
				break;
			}
			case jai::node_type::enum_decl: {
				auto* e = static_cast<const jai::enum_decl*>(d.get());
				out.names[prefix + std::string(e->name)].push_back({e, symbol_kind::enum_type, e->name});
				for (const auto& [vname, _] : e->values) { out.enum_values.emplace(std::string(vname), e); }
				break;
			}
			case jai::node_type::namespace_decl: {
				auto* n = static_cast<const jai::namespace_decl*>(d.get());
				std::string full(n->name);
				out.namespaces.insert(full);
				for (size_t pos = full.find("::"); pos != std::string::npos; pos = full.find("::", pos + 2)) { out.namespaces.insert(full.substr(0, pos)); }
				std::string_view seg = n->name;
				if (size_t pos = seg.rfind("::"); pos != std::string_view::npos) { seg = seg.substr(pos + 2); }
				out.names[full].push_back({n, symbol_kind::namespace_name, seg});
				build_unit_index(n->declarations, full + "::", out);
				break;
			}
			default:
				break;
		}
	}
}


program_view::program_view(workspace& w, const program& p, unit& cur) : ws(w), prog(p), current(cur) {}

const unit_index& program_view::index_of(unit& u) {
	if (u.index && u.index_epoch == u.epoch) { return *std::static_pointer_cast<unit_index>(u.index); }
	auto made = std::make_shared<unit_index>();
	unit_index& idx = *made;
	u.index = made;
	u.index_epoch = u.epoch;
	build_unit_index(u.decls, "", idx);
	if (!u.parse_ok && !u.good_decls.empty()) {   // what a failing parse lost stays findable
		unit_index good;
		build_unit_index(u.good_decls, "", good);
		for (auto& [name, entries] : good.names) { if (!idx.names.count(name)) { idx.names[name] = entries; } }
		for (auto& [name, cd] : good.classes) { idx.classes.emplace(name, cd); }
		for (auto& [name, e] : good.enum_values) { idx.enum_values.emplace(name, e); }
		idx.namespaces.insert(good.namespaces.begin(), good.namespaces.end());
	}
	return idx;
}

std::vector<unit*> program_view::units() {
	std::vector<unit*> out{&current};
	for (const auto& p : prog.units) {
		if (p == current.path) { continue; }
		if (unit* u = ws.find(p)) { out.push_back(u); }
	}
	return out;
}

unit* program_view::unit_of_node(const jai::ast_node* node) {
	if (!node) { return nullptr; }
	const std::string& f = node->location.filename;
	if (f == current.path) { return &current; }
	return ws.find(f);
}

const jai::class_decl* program_view::find_class(std::string_view name) {
	std::string key(name);
	for (unit* u : units()) {
		const auto& idx = index_of(*u);
		if (auto it = idx.classes.find(key); it != idx.classes.end()) { return it->second; }
	}
	if (auto it = prog.classes.find(key); it != prog.classes.end() && it->second) { return it->second.get(); }
	return nullptr;
}

bool program_view::class_known(std::string_view name) {
	return find_class(name) != nullptr || jai::detail::engine_access::get_class_definition(ws.engine(), std::string(name)) != nullptr;
}

program_view::member_ref program_view::find_member(std::string_view cls, std::string_view member, bool want_static, int depth) {
	member_ref miss;
	if (depth > 32) { return miss; }
	if (const jai::class_decl* cd = find_class(cls)) {
		for (const auto& m : cd->members) {
			if (!m.declaration) { continue; }
			if (m.declaration->get_type() == jai::node_type::variable_decl) {
				auto* f = static_cast<const jai::variable_decl*>(m.declaration.get());
				if (f->name == member && (!want_static || f->is_static)) { return {true, cd, f, {}}; }
			} else if (m.declaration->get_type() == jai::node_type::function_decl) {
				auto* fn = static_cast<const jai::function_decl*>(m.declaration.get());
				if (fn->name == member && fn->name != cd->name && (!want_static || fn->is_static)) { return {true, cd, fn, {}}; }
			}
		}
		for (auto base : cd->base_classes) {
			member_ref r = find_member(base, member, want_static, depth + 1);
			if (r.found) { return r; }
		}
		return miss;
	}
	auto& eng = ws.engine();
	if (auto def = jai::detail::engine_access::get_class_definition(eng, std::string(cls))) {
		const uint64_t id = jai::detail::engine_access::symbolize(eng, member);
		bool hit = want_static ? (def->has_static_method(id) || def->is_static_field(id) || def->has_static_field(id))
		                       : engine_instance_member(eng, *def, member);
		if (hit) { return {true, nullptr, nullptr, std::string(cls)}; }
	}
	return miss;
}

std::vector<program_view::top_ref> program_view::top_level(const std::string& qualified) {
	std::vector<top_ref> out;
	for (unit* u : units()) {
		const auto& idx = index_of(*u);
		auto it = idx.names.find(qualified);
		if (it == idx.names.end()) { continue; }
		for (const auto& e : it->second) { out.push_back({&e, u}); }
	}
	return out;
}

bool program_view::has_top(const std::string& qualified) {
	for (unit* u : units()) {
		if (index_of(*u).names.count(qualified)) { return true; }
	}
	return false;
}

const jai::enum_decl* program_view::find_enum_value(std::string_view value) {
	std::string key(value);
	for (unit* u : units()) {
		const auto& idx = index_of(*u);
		if (auto it = idx.enum_values.find(key); it != idx.enum_values.end()) { return it->second; }
	}
	return nullptr;
}

const jai::enum_decl* program_view::find_enum(std::string_view name) {
	for (const auto& r : top_level(std::string(name))) {
		if (r.entry->kind == symbol_kind::enum_type) { return static_cast<const jai::enum_decl*>(r.entry->node); }
	}
	return nullptr;
}

bool program_view::host_namespace(const std::string& path) {
	auto& eng = ws.engine();
	return path == "math" || jai::detail::engine_access::script_namespaces(eng).count(jai::detail::engine_access::symbolize(eng, path)) != 0;
}

bool program_view::is_namespace(const std::string& path) {
	for (unit* u : units()) {
		if (index_of(*u).namespaces.count(path)) { return true; }
	}
	return host_namespace(path);
}

std::optional<text_span> program_view::decl_span(const jai::declaration* node, std::string_view name) {
	unit* owner = unit_of_node(node);
	if (!owner) { return std::nullopt; }
	text_span span = name_span(node->location, name);
	if (span_spells(*owner, span, name)) { return span; }
	return find_name_after(owner->tokens, pos_of(node->location), name);
}

std::vector<std::string> program_view::method_signatures(const jai::class_decl* owner, std::string_view name, bool constructor) {
	std::vector<std::string> out;
	for (const auto& m : owner->members) {
		if (!m.declaration || m.declaration->get_type() != jai::node_type::function_decl) { continue; }
		auto* fn = static_cast<const jai::function_decl*>(m.declaration.get());
		if (fn->name == name) { out.push_back(function_signature(*fn, name, constructor).label); }
	}
	return out;
}

std::vector<std::string> program_view::host_signatures(const std::string& name) {
	std::vector<std::string> out;
	auto& eng = ws.engine();
	std::vector<jai::engine::host_overload_signature> sigs;
	if (!eng.host_function_signatures(name, sigs)) { return out; }
	for (const auto& s : sigs) {
		std::string label = name + "(";
		if (s.arity == SIZE_MAX) {
			label += "...";
		} else if (s.param_types.empty()) {
			for (size_t i = 0; i < s.arity; ++i) { label += (i ? ", var arg" : "var arg") + std::to_string(i); }
		} else {
			for (size_t i = 0; i < s.param_types.size(); ++i) {
				if (i) { label += ", "; }
				label += cpp_param_text(eng, s.param_types[i]);
			}
		}
		label += ")";
		if (std::find(out.begin(), out.end(), label) == out.end()) { out.push_back(label); }
	}
	return out;
}

std::string program_view::class_detail(const jai::class_decl* cd) {
	std::string d = (cd->nocopy ? "nocopy class " : "class ") + std::string(cd->name);
	for (size_t i = 0; i < cd->base_classes.size(); ++i) {
		d += i ? ", " : " : ";
		d += cd->base_classes[i];
	}
	return d;
}

nav_type program_view::declared_type(const jai::declaration* d) {
	if (!d) { return {}; }
	switch (d->get_type()) {
		case jai::node_type::function_decl: return nav_from_type_info(static_cast<const jai::function_decl*>(d)->return_type.get());
		case jai::node_type::variable_decl: {
			auto* v = static_cast<const jai::variable_decl*>(d);
			if (!auto_spelled(v->type.get())) { return nav_from_type_info(v->type.get()); }
			const jai::expression* init = v->initializer.get();
			if (init && init->get_type() == jai::node_type::new_expr) { return nav_from_type_info(static_cast<const jai::new_expr*>(init)->type.get()); }
			if (init && init->get_type() == jai::node_type::call_expr) {
				auto* callee = static_cast<const jai::call_expr*>(init)->callee.get();
				if (callee && callee->get_type() == jai::node_type::identifier_expr) {
					auto name = static_cast<const jai::identifier_expr*>(callee)->name;
					if (find_class(name)) { return nav_type::of_object(name); }
				}
			}
			return {};
		}
		case jai::node_type::extern_decl: return nav_from_type_info(static_cast<const jai::extern_decl*>(d)->type.get());
		case jai::node_type::class_decl: return nav_type::of_class(static_cast<const jai::class_decl*>(d)->name);
		case jai::node_type::enum_decl: return nav_type::of_enum(static_cast<const jai::enum_decl*>(d)->name);
		default: return {};
	}
}

uint32_t program_view::global_symbol(unit_model& m, const std::string& qualified) {
	std::string key = "g:" + qualified;
	if (auto it = m.by_key.find(key); it != m.by_key.end()) { return it->second; }
	symbol s;
	s.key = key;
	s.name = last_segment(qualified);
	s.container = prefix_of(qualified);
	auto refs = top_level(qualified);
	const top_ref* chosen = nullptr;
	for (const auto& r : refs) { if (!r.entry->is_extern) { chosen = &r; break; } }
	if (!chosen && !refs.empty()) { chosen = &refs.front(); }
	if (!chosen) {
		s.kind = symbol_kind::global;
		return add_symbol(m, std::move(s));
	}
	if (chosen->entry->kind == symbol_kind::class_type && qualified != chosen->entry->name) {
		return global_symbol(m, std::string(chosen->entry->name));   // classes are one program-wide name, however they are reached
	}
	const unit_index::entry& e = *chosen->entry;
	s.kind = e.kind;
	s.path = chosen->owner->path;
	if (auto span = decl_span(e.node, s.name)) { s.span = *span; } else { s.path.clear(); }
	switch (e.node->get_type()) {
		case jai::node_type::function_decl: {
			for (const auto& r : refs) {
				if (r.owner == chosen->owner && r.entry->node->get_type() == jai::node_type::function_decl) {
					s.signatures.push_back(function_signature(*static_cast<const jai::function_decl*>(r.entry->node), qualified, false).label);
				}
			}
			s.detail = s.signatures.empty() ? qualified : s.signatures.front();
			auto* fn = static_cast<const jai::function_decl*>(e.node);
			s.type = type_text(fn->return_type.get());
			break;
		}
		case jai::node_type::variable_decl: {
			auto* v = static_cast<const jai::variable_decl*>(e.node);
			s.type = v->function_spelling ? "function" : type_text(v->type.get());
			s.detail = s.type + " " + qualified;
			break;
		}
		case jai::node_type::extern_decl: {
			auto* x = static_cast<const jai::extern_decl*>(e.node);
			s.type = x->is_function ? "function" : x->type ? type_text(x->type.get()) : "var";
			s.detail = "extern " + (x->is_function ? std::string("function ") : x->type ? type_text(x->type.get()) + " " : std::string()) + qualified;
			break;
		}
		case jai::node_type::class_decl: s.detail = class_detail(static_cast<const jai::class_decl*>(e.node)); break;
		case jai::node_type::enum_decl: {
			auto* en = static_cast<const jai::enum_decl*>(e.node);
			s.detail = "enum " + qualified + " { ";
			for (size_t i = 0; i < en->values.size(); ++i) { s.detail += (i ? ", " : "") + std::string(en->values[i].first); }
			s.detail += " }";
			break;
		}
		case jai::node_type::namespace_decl: s.detail = "namespace " + qualified; break;
		default: s.detail = "auto " + qualified; break;
	}
	return add_symbol(m, std::move(s));
}

uint32_t program_view::member_symbol(unit_model& m, const member_ref& r, std::string_view member) {
	if (!r.host_class.empty()) {
		std::string key = "hm:" + r.host_class + "." + std::string(member);
		if (auto it = m.by_key.find(key); it != m.by_key.end()) { return it->second; }
		symbol s;
		s.key = key;
		s.kind = symbol_kind::host_member;
		s.name = std::string(member);
		s.container = r.host_class;
		auto& eng = ws.engine();
		const uint64_t id = jai::detail::engine_access::symbolize(eng, member);
		std::vector<const jai::class_definition*> chain;
		if (auto def = jai::detail::engine_access::get_class_definition(eng, r.host_class)) { chain.push_back(def.get()); }
		for (size_t i = 0; i < chain.size() && i < 32; ++i) {
			const jai::class_definition* d = chain[i];
			if (auto it = d->cpp_method_overload_table().find(id); it != d->cpp_method_overload_table().end()) {
				for (const auto& [arity, overloads] : it->second) {
					for (const auto& entry : overloads) {
						std::string label = std::string(member) + "(";
						for (size_t p = 0; p < arity; ++p) {
							if (p) { label += ", "; }
							label += p < entry.param_types.size() ? cpp_param_text(eng, entry.param_types[p]) : "var";
						}
						s.signatures.push_back(label + ")");
					}
				}
			}
			if (auto it = d->script_method_overloads().find(id); it != d->script_method_overloads().end()) {
				for (const auto& fd : it->second) { s.signatures.push_back(function_signature(*fd, member, false).label); }
			}
			for (const auto& parent : d->get_parent_classes()) { if (parent) { chain.push_back(parent.get()); } }
			if (auto base = d->get_cpp_base_class()) { chain.push_back(base.get()); }
		}
		s.detail = (s.signatures.empty() ? r.host_class + "." + std::string(member) : s.signatures.front()) + "  (registered by the host)";
		return add_symbol(m, std::move(s));
	}
	std::string key = "m:" + std::string(r.owner->name) + "." + std::string(member);
	if (auto it = m.by_key.find(key); it != m.by_key.end()) { return it->second; }
	symbol s;
	s.key = key;
	s.name = std::string(member);
	s.container = std::string(r.owner->name);
	s.path = r.owner->location.filename;
	if (auto span = decl_span(r.decl, member)) { s.span = *span; } else { s.path.clear(); }
	if (r.decl->get_type() == jai::node_type::variable_decl) {
		auto* f = static_cast<const jai::variable_decl*>(r.decl);
		s.kind = symbol_kind::field;
		s.is_static = f->is_static;
		s.type = f->function_spelling ? "function" : type_text(f->type.get());
		s.detail = (f->is_static ? "static " : "") + s.type + " " + s.container + "." + s.name;
	} else {
		auto* fn = static_cast<const jai::function_decl*>(r.decl);
		s.kind = symbol_kind::method;
		s.is_static = fn->is_static;
		s.type = type_text(fn->return_type.get());
		s.signatures = method_signatures(r.owner, member, false);
		s.detail = s.signatures.empty() ? s.name : s.signatures.front();
	}
	return add_symbol(m, std::move(s));
}

uint32_t program_view::enum_value_symbol(unit_model& m, const jai::enum_decl* e, std::string_view value) {
	std::string key = "e:" + std::string(e->name) + "." + std::string(value);
	if (auto it = m.by_key.find(key); it != m.by_key.end()) { return it->second; }
	symbol s;
	s.key = key;
	s.kind = symbol_kind::enum_value;
	s.name = std::string(value);
	s.container = std::string(e->name);
	s.type = "int";
	size_t index = 0;
	for (size_t i = 0; i < e->values.size(); ++i) { if (e->values[i].first == value) { index = i; } }
	s.detail = std::string(e->name) + "." + s.name + " = " + std::to_string(index);
	if (unit* owner = unit_of_node(e)) {
		text_pos cursor = pos_of(e->location);
		for (size_t i = 0; i < e->values.size(); ++i) {
			auto span = find_name_after(owner->tokens, cursor, e->values[i].first);
			if (!span) { break; }
			cursor = span->end;
			if (e->values[i].first == value) { s.path = owner->path; s.span = *span; break; }
		}
	}
	return add_symbol(m, std::move(s));
}

uint32_t program_view::host_symbol(unit_model& m, const std::string& name, symbol_kind kind) {
	std::string key = "h:" + name;
	if (auto it = m.by_key.find(key); it != m.by_key.end()) { return it->second; }
	auto& eng = ws.engine();
	symbol s;
	s.key = key;
	s.kind = kind;
	s.name = last_segment(name);
	s.container = prefix_of(name);
	switch (kind) {
		case symbol_kind::host_function:
			s.signatures = host_signatures(name);
			s.detail = s.signatures.empty() ? name + "(...)" : s.signatures.front();
			break;
		case symbol_kind::host_global: {
			s.detail = name;
			if (auto env = jai::detail::engine_access::get_global_environment(eng)) {
				auto v = env->get(name);
				if (v && v.value().get_type_info()) {
					s.type = type_text(v.value().get_type_info());
					s.detail = s.type + " " + name;
				}
			}
			break;
		}
		case symbol_kind::host_class: s.detail = "class " + name; break;
		case symbol_kind::host_namespace: s.detail = "namespace " + name; break;
		default: s.detail = name; break;
	}
	s.detail += "  (registered by the host)";
	return add_symbol(m, std::move(s));
}

uint32_t program_view::builtin_symbol(unit_model& m, std::string_view receiver, std::string_view member) {
	std::string key = "b:" + std::string(receiver) + "." + std::string(member);
	if (auto it = m.by_key.find(key); it != m.by_key.end()) { return it->second; }
	symbol s;
	s.key = key;
	s.kind = symbol_kind::host_member;
	s.name = std::string(member);
	s.container = std::string(receiver);
	s.detail = std::string(receiver) + "." + std::string(member) + "  (builtin method)";
	return add_symbol(m, std::move(s));
}


semantic_walker::semantic_walker(workspace& ws, unit& u, const program& prog, unit_model& m)
	: u_(u), m_(m), view_(ws, prog, u), eng_(ws.engine()) {}

void semantic_walker::run() {
	walk_decls(u_.decls);
	type_name_pass();
	std::sort(m_.occurrences.begin(), m_.occurrences.end(), [](const occurrence& a, const occurrence& b) { return a.span.start < b.span.start; });
	m_.occurrences.erase(std::unique(m_.occurrences.begin(), m_.occurrences.end(), [](const occurrence& a, const occurrence& b) {
		return a.span.start == b.span.start && a.symbol == b.symbol;
	}), m_.occurrences.end());
}

void semantic_walker::occur(text_span span, uint32_t sym, bool declaration) {
	if (sym >= m_.symbols.size()) { return; }
	if (!span_spells(u_, span, m_.symbols[sym].name)) { return; }
	m_.occurrences.push_back({span, sym, declaration});
}

void semantic_walker::push_scope(text_pos start, text_pos end) {
	scopes_.emplace_back();
	regions_.push_back(m_.scopes.size());
	m_.scopes.push_back({{start, end}, {}});
}

void semantic_walker::pop_scope() {
	if (!scopes_.empty()) { scopes_.pop_back(); }
	if (!regions_.empty()) { regions_.pop_back(); }
}

uint32_t semantic_walker::declare_local(std::string_view name, text_span span, symbol_kind kind, const std::string& type_shown, nav_type type, bool with_occurrence, const std::string& inferred) {
	symbol s;
	s.key = "l:" + u_.path + "@" + std::to_string(span.start.line) + ":" + std::to_string(span.start.col) + ":" + std::string(name);
	s.kind = kind;
	s.name = std::string(name);
	s.container = fn_stack_.empty() ? std::string() : fn_stack_.back();
	s.type = inferred.empty() ? type_shown : inferred;
	s.detail = type_shown + " " + std::string(name);
	s.path = u_.path;
	s.span = span;
	uint32_t index = add_symbol(m_, std::move(s));
	symbol_type_[index] = type;
	if (!scopes_.empty()) {
		scopes_.back()[m_.symbols[index].name] = index;
		m_.scopes[regions_.back()].locals.push_back({index, span.start});
	}
	if (with_occurrence) { occur(span, index, true); }
	return index;
}

std::string semantic_walker::qualify(std::string_view name) const {
	return ns_stack_.empty() ? std::string(name) : ns_stack_.back() + "::" + std::string(name);
}

uint32_t semantic_walker::resolve_name(std::string_view name, bool global_only) {
	if (!global_only) {
		const std::string key(name);
		for (size_t i = scopes_.size(); i-- > 0;) {
			if (auto it = scopes_[i].find(key); it != scopes_[i].end()) { return it->second; }
		}
		if (!class_stack_.empty()) {
			auto r = view_.find_member(class_stack_.back(), name, false);
			if (r.found) {
				uint32_t s = view_.member_symbol(m_, r, name);
				if (!symbol_type_.count(s)) { symbol_type_[s] = view_.declared_type(r.decl); }
				return s;
			}
		}
		for (size_t i = ns_stack_.size(); i-- > 0;) {
			std::string q = ns_stack_[i] + "::" + std::string(name);
			if (view_.has_top(q)) {
				uint32_t s = view_.global_symbol(m_, q);
				if (!symbol_type_.count(s)) {
					auto refs = view_.top_level(q);
					symbol_type_[s] = refs.empty() ? nav_type{} : view_.declared_type(refs.front().entry->node);
				}
				return s;
			}
		}
	}
	std::string n(name);
	if (view_.has_top(n)) {
		uint32_t s = view_.global_symbol(m_, n);
		if (!symbol_type_.count(s)) {
			auto refs = view_.top_level(n);
			const jai::declaration* node = nullptr;
			for (const auto& r : refs) { if (!r.entry->is_extern) { node = r.entry->node; break; } }
			if (!node && !refs.empty()) { node = refs.front().entry->node; }
			symbol_type_[s] = view_.declared_type(node);
		}
		return s;
	}
	if (const jai::enum_decl* e = view_.find_enum_value(name)) { return view_.enum_value_symbol(m_, e, name); }
	if (jai::detail::engine_access::get_class_definition(eng_, n)) {
		uint32_t s = view_.host_symbol(m_, n, symbol_kind::host_class);
		symbol_type_[s] = nav_type::of_class(n);
		return s;
	}
	if (eng_.has_function(n)) { return view_.host_symbol(m_, n, symbol_kind::host_function); }
	if (eng_.has_variable(n)) {
		uint32_t s = view_.host_symbol(m_, n, symbol_kind::host_global);
		if (!symbol_type_.count(s)) {
			if (auto env = jai::detail::engine_access::get_global_environment(eng_)) {
				auto v = env->get(n);
				if (v) { symbol_type_[s] = nav_from_type_info(v.value().get_type_info()); }
			}
		}
		return s;
	}
	if (view_.host_namespace(n)) { return view_.host_symbol(m_, n, symbol_kind::host_namespace); }
	return UINT32_MAX;
}

nav_type semantic_walker::type_of(const jai::expression* e) {
	if (!e || jai::detail::native_stack_low()) { return {}; }
	if (auto it = nav_memo_.find(e); it != nav_memo_.end()) { return it->second; }
	nav_type t;
	if (auto it = m_.types.find(e); it != m_.types.end()) { t = nav_from_check(it->second); }
	if (!t.known() || ((t.kind == nav_type::array || t.kind == nav_type::map) && !t.ti)) {
		nav_type own = own_type(e);
		if (own.known()) { t = own; }
	}
	nav_memo_[e] = t;
	return t;
}

nav_type semantic_walker::own_type(const jai::expression* e) {
	switch (e->get_type()) {
		case jai::node_type::identifier_expr:
		case jai::node_type::member_expr: {
			auto it = expr_symbol_.find(e);
			if (it == expr_symbol_.end()) { return {}; }
			auto t = symbol_type_.find(it->second);
			return t == symbol_type_.end() ? nav_type{} : t->second;
		}
		case jai::node_type::call_expr: {
			auto* c = static_cast<const jai::call_expr*>(e);
			auto it = expr_symbol_.find(c->callee.get());
			if (it == expr_symbol_.end()) { return {}; }
			const symbol& s = m_.symbols[it->second];
			if (s.kind == symbol_kind::class_type || s.kind == symbol_kind::host_class) { return nav_type::of_object(s.name); }
			if (s.kind == symbol_kind::function || s.kind == symbol_kind::method) {
				auto t = symbol_type_.find(it->second);
				return t == symbol_type_.end() ? nav_type{} : t->second;
			}
			return {};
		}
		case jai::node_type::binary_expr: {
			auto* b = static_cast<const jai::binary_expr*>(e);
			if (b->op.type == jai::token_type::left_bracket) { return element_of(type_of(b->left.get())); }
			return {};
		}
		case jai::node_type::cast_expr: return nav_from_type_info(static_cast<const jai::cast_expr*>(e)->target.get());
		case jai::node_type::new_expr: return nav_from_type_info(static_cast<const jai::new_expr*>(e)->type.get());
		case jai::node_type::ternary_expr: {
			auto* t = static_cast<const jai::ternary_expr*>(e);
			nav_type a = type_of(t->then_expression.get());
			return a.known() ? a : type_of(t->else_expression.get());
		}
		case jai::node_type::assignment_expr: return type_of(static_cast<const jai::assignment_expr*>(e)->target.get());
		case jai::node_type::this_expr: return class_stack_.empty() ? nav_type{} : nav_type::of_object(class_stack_.back());
		default: return {};
	}
}

void semantic_walker::capture_scope() {
	m_.captured = true;
	std::unordered_set<std::string> seen;
	for (size_t i = scopes_.size(); i-- > 0;) {
		for (const auto& [name, sym] : scopes_[i]) {
			if (seen.insert(name).second) { m_.capture_locals.push_back(sym); }
		}
	}
	m_.class_at_capture = class_stack_.empty() ? std::string() : class_stack_.back();
	m_.namespaces_at_capture = ns_stack_;
}

text_pos semantic_walker::after_open_paren(text_pos pos) {
	for (size_t i = token_at_or_after(u_.tokens, pos), n = 0; i < u_.tokens.size() && n < 64; ++i, ++n) {
		if (u_.tokens[i].type == jai::token_type::left_paren) { return {u_.tokens[i].pos.line, u_.tokens[i].pos.col + 1}; }
	}
	return pos;
}

text_pos semantic_walker::stmt_end(const jai::statement* s) {
	if (!s) { return {}; }
	switch (s->get_type()) {
		case jai::node_type::block_stmt: {
			text_pos open = punct_pos(s->location);
			if (auto end = block_end_after(u_.tokens, open)) { return *end; }
			return open;
		}
		case jai::node_type::if_stmt: {
			auto* i = static_cast<const jai::if_stmt*>(s);
			return stmt_end(i->else_statement ? i->else_statement.get() : i->then_statement.get());
		}
		case jai::node_type::while_stmt: return stmt_end(static_cast<const jai::while_stmt*>(s)->body.get());
		case jai::node_type::for_stmt: return stmt_end(static_cast<const jai::for_stmt*>(s)->body.get());
		case jai::node_type::range_for_stmt: return stmt_end(static_cast<const jai::range_for_stmt*>(s)->body.get());
		case jai::node_type::parallel_for_stmt: return stmt_end(static_cast<const jai::parallel_for_stmt*>(s)->body.get());
		case jai::node_type::try_stmt: return stmt_end(static_cast<const jai::try_stmt*>(s)->catch_block.get());
		default: {
			text_pos p = pos_of(s->location);
			for (size_t i = token_at_or_after(u_.tokens, p); i < u_.tokens.size(); ++i) {
				if (u_.tokens[i].type == jai::token_type::semicolon) { return {u_.tokens[i].pos.line, u_.tokens[i].pos.col + 1}; }
			}
			return p;
		}
	}
}

void semantic_walker::walk_decls(const std::vector<jai::declaration_ptr>& decls) {
	for (const auto& d : decls) { walk_decl(d.get()); }
}

void semantic_walker::walk_decl(const jai::declaration* d) {
	if (!d) { return; }
	if (!d->location.filename.empty() && d->location.filename != u_.path) { return; }   // spliced from an include: its positions are another file's
	switch (d->get_type()) {
		case jai::node_type::variable_decl: walk_variable(static_cast<const jai::variable_decl*>(d)); break;
		case jai::node_type::function_decl: walk_function(static_cast<const jai::function_decl*>(d)); break;
		case jai::node_type::class_decl: walk_class(static_cast<const jai::class_decl*>(d)); break;
		case jai::node_type::namespace_decl: {
			auto* n = static_cast<const jai::namespace_decl*>(d);
			std::string full(n->name);
			uint32_t s = view_.global_symbol(m_, full);
			if (auto span = find_name_after(u_.tokens, pos_of(n->location), last_segment(full), 32)) { occur(*span, s, true); }
			ns_stack_.push_back(full);
			walk_decls(n->declarations);
			ns_stack_.pop_back();
			break;
		}
		case jai::node_type::enum_decl: {
			auto* e = static_cast<const jai::enum_decl*>(d);
			uint32_t s = view_.global_symbol(m_, qualify(e->name));
			symbol_type_[s] = nav_type::of_enum(e->name);
			occur(name_span(e->location, e->name), s, true);
			for (const auto& [vname, _] : e->values) {
				uint32_t v = view_.enum_value_symbol(m_, e, vname);
				if (m_.symbols[v].path == u_.path) { occur(m_.symbols[v].span, v, true); }
			}
			break;
		}
		case jai::node_type::extern_decl: {
			auto* x = static_cast<const jai::extern_decl*>(d);
			uint32_t s = scopes_.empty() ? view_.global_symbol(m_, qualify(x->name)) : resolve_name(x->name, false);
			if (s != UINT32_MAX) {
				if (auto span = find_name_after(u_.tokens, pos_of(x->location), x->name, 32)) { occur(*span, s, true); }
				if (!symbol_type_.count(s) && x->type) { symbol_type_[s] = nav_from_type_info(x->type.get()); }
			}
			break;
		}
		case jai::node_type::destructuring_decl: {
			auto* dd = static_cast<const jai::destructuring_decl*>(d);
			walk_expr(dd->initializer.get());
			text_pos cursor = pos_of(dd->location);
			for (const auto& [name, _] : dd->names) {
				auto span = find_name_after(u_.tokens, cursor, name, 32);
				if (!span) { continue; }
				cursor = span->end;
				if (scopes_.empty()) {
					occur(*span, view_.global_symbol(m_, qualify(name)), true);
				} else {
					declare_local(name, *span, symbol_kind::local, "auto", {});
				}
			}
			break;
		}
		case jai::node_type::expression_decl: walk_expr(static_cast<const jai::expression_decl*>(d)->expression.get()); break;
		case jai::node_type::statement_decl: walk_stmt(static_cast<const jai::statement_decl*>(d)->statement.get()); break;
		case jai::node_type::include_decl: walk_expr(static_cast<const jai::include_decl*>(d)->path_expr.get()); break;
		case jai::node_type::import_decl: walk_expr(static_cast<const jai::import_decl*>(d)->path_expr.get()); break;
		default: break;
	}
}

void semantic_walker::walk_variable(const jai::variable_decl* v) {
	if (v->initializer) { walk_expr(v->initializer.get()); }
	nav_type t;
	std::string shown = v->function_spelling ? "function" : type_text(v->type.get());
	std::string inferred;
	if (!auto_spelled(v->type.get())) {
		t = nav_from_type_info(v->type.get());
	} else if (v->initializer) {
		t = type_of(v->initializer.get());
		if (auto it = m_.types.find(v->initializer.get()); it != m_.types.end() && it->second.kind != jai::check_expression_type::kind_t::unknown) {
			inferred = it->second.name;
		}
		if (inferred.empty() || inferred == "array" || inferred == "map") {
			if (std::string own = nav_text(t); !own.empty()) { inferred = own; }
		}
		if (!inferred.empty() && v->type && v->type->is_reference()) { inferred += "&"; }
	}
	text_span span = name_span(v->location, v->name);
	if (scopes_.empty()) {
		uint32_t s = view_.global_symbol(m_, qualify(v->name));
		if (!symbol_type_.count(s) || !symbol_type_[s].known()) { symbol_type_[s] = t; }
		if (!inferred.empty() && m_.symbols[s].path == u_.path) { m_.symbols[s].type = inferred; }
		occur(span, s, true);
	} else {
		declare_local(v->name, span, symbol_kind::local, shown, var_spelled(v->type.get()) ? nav_type{} : t, true, inferred);
	}
}

void semantic_walker::walk_function(const jai::function_decl* fn) {
	text_span span = name_span(fn->location, fn->name);
	if (scopes_.empty() && class_stack_.empty()) {
		uint32_t s = view_.global_symbol(m_, qualify(fn->name));
		symbol_type_[s] = nav_from_type_info(fn->return_type.get());
		occur(span, s, true);
	} else if (!scopes_.empty()) {
		uint32_t s = declare_local(fn->name, span, symbol_kind::function, function_signature(*fn, fn->name, false).label, nav_from_type_info(fn->return_type.get()));
		m_.symbols[s].detail = m_.symbols[s].type;
		m_.symbols[s].signatures = {m_.symbols[s].type};
	}
	walk_function_body(fn);
}

void semantic_walker::walk_function_body(const jai::function_decl* fn) {
	text_pos start = pos_of(fn->location);
	text_pos end = fn->body ? stmt_end(fn->body.get()) : start;
	push_scope(start, end);
	fn_stack_.push_back(std::string(fn->name));
	declare_params(fn->parameters, after_open_paren(start));
	for (const auto& init : fn->initializers) {
		for (const auto& arg : init.arguments) { walk_expr(arg.get()); }
	}
	if (fn->body) { walk_decls(fn->body->declarations); }
	fn_stack_.pop_back();
	pop_scope();
}

void semantic_walker::declare_params(const std::vector<jai::parameter>& params, text_pos after) {
	text_pos cursor = after;
	for (const auto& p : params) {
		if (p.default_value) { walk_expr(p.default_value.get()); }
		std::string shown = p.type ? type_text(p.type.get()) : "auto";
		if (p.is_reference && (shown.empty() || shown.back() != '&')) { shown += '&'; }
		auto span = find_name_after(u_.tokens, cursor, p.name, 64);
		nav_type t = var_spelled(p.type.get()) ? nav_type{} : nav_from_type_info(p.type.get());
		if (span) {
			cursor = span->end;
			declare_local(p.name, *span, symbol_kind::parameter, shown, t);
		} else {
			declare_local(p.name, {cursor, cursor}, symbol_kind::parameter, shown, t, false);
		}
	}
}

void semantic_walker::walk_class(const jai::class_decl* cd) {
	uint32_t cs = view_.global_symbol(m_, std::string(cd->name));
	symbol_type_[cs] = nav_type::of_class(cd->name);
	text_span span = name_span(cd->location, cd->name);
	occur(span, cs, true);
	text_pos cursor = span.end;
	for (auto base : cd->base_classes) {
		auto bspan = find_name_after(u_.tokens, cursor, base, 16);
		if (!bspan) { continue; }
		cursor = bspan->end;
		uint32_t b = resolve_name(base, true);
		if (b != UINT32_MAX) { occur(*bspan, b, false); }
	}
	class_stack_.push_back(std::string(cd->name));
	for (const auto& member : cd->members) {
		const jai::declaration* d = member.declaration.get();
		if (!d) { continue; }
		if (d->get_type() == jai::node_type::variable_decl) {
			auto* f = static_cast<const jai::variable_decl*>(d);
			if (f->initializer) { walk_expr(f->initializer.get()); }
			program_view::member_ref r{true, cd, f, {}};
			uint32_t s = view_.member_symbol(m_, r, f->name);
			nav_type t = auto_spelled(f->type.get()) && f->initializer ? type_of(f->initializer.get()) : nav_from_type_info(f->type.get());
			symbol_type_[s] = var_spelled(f->type.get()) ? nav_type{} : t;
			occur(name_span(f->location, f->name), s, true);
		} else if (d->get_type() == jai::node_type::function_decl) {
			auto* fn = static_cast<const jai::function_decl*>(d);
			text_span fspan = name_span(fn->location, fn->name);
			if (fn->name == cd->name) {
				occur(fspan, cs, false);
			} else {
				program_view::member_ref r{true, cd, fn, {}};
				uint32_t s = view_.member_symbol(m_, r, fn->name);
				symbol_type_[s] = nav_from_type_info(fn->return_type.get());
				occur(fspan, s, true);
			}
			walk_function_body(fn);
		} else {
			walk_decl(d);
		}
	}
	class_stack_.pop_back();
}

// The walks recurse with the tree; past the native stack's margin they leave the rest of a subtree unindexed
void semantic_walker::walk_stmt(const jai::statement* s) {
	if (!s || jai::detail::native_stack_low()) { return; }
	switch (s->get_type()) {
		case jai::node_type::expression_stmt: walk_expr(static_cast<const jai::expression_stmt*>(s)->expression.get()); break;
		case jai::node_type::block_stmt: {
			auto* b = static_cast<const jai::block_stmt*>(s);
			push_scope(punct_pos(b->location), stmt_end(b));
			walk_decls(b->declarations);
			pop_scope();
			break;
		}
		case jai::node_type::if_stmt: {
			auto* i = static_cast<const jai::if_stmt*>(s);
			walk_expr(i->condition.get());
			walk_stmt(i->then_statement.get());
			walk_stmt(i->else_statement.get());
			break;
		}
		case jai::node_type::while_stmt: {
			auto* w = static_cast<const jai::while_stmt*>(s);
			walk_expr(w->condition.get());
			walk_stmt(w->body.get());
			break;
		}
		case jai::node_type::for_stmt: {
			auto* f = static_cast<const jai::for_stmt*>(s);
			push_scope(pos_of(f->location), stmt_end(f));
			walk_decl(f->initializer.get());
			walk_expr(f->condition.get());
			walk_expr(f->update.get());
			walk_stmt(f->body.get());
			pop_scope();
			break;
		}
		case jai::node_type::range_for_stmt: {
			auto* f = static_cast<const jai::range_for_stmt*>(s);
			walk_expr(f->container.get());
			push_scope(pos_of(f->location), stmt_end(f));
			nav_type container = type_of(f->container.get());
			nav_type t;
			std::string shown = f->element_type ? type_text(f->element_type.get()) : "auto";
			if (f->is_reference && (shown.empty() || shown.back() != '&')) { shown += '&'; }
			std::string inferred;
			if (!auto_spelled(f->element_type.get())) {
				t = var_spelled(f->element_type.get()) ? nav_type{} : nav_from_type_info(f->element_type.get());
			} else if (container.kind == nav_type::array) {
				t = element_of(container);
				if (container.ti && container.ti->element_type()) { inferred = type_text(container.ti->element_type().get()) + (f->is_reference ? "&" : ""); }
			}
			if (auto span = find_name_after(u_.tokens, pos_of(f->location), f->variable_name, 32)) {
				declare_local(f->variable_name, *span, symbol_kind::local, shown, t, true, inferred);
			}
			walk_stmt(f->body.get());
			pop_scope();
			break;
		}
		case jai::node_type::parallel_for_stmt: {
			auto* f = static_cast<const jai::parallel_for_stmt*>(s);
			walk_expr(f->container.get());
			for (const auto& c : f->extra_containers) { walk_expr(c.get()); }
			push_scope(pos_of(f->location), stmt_end(f));
			text_pos cursor = pos_of(f->location);
			auto declare = [&](const jai::parameter& p, const jai::expression* container) {
				auto span = find_name_after(u_.tokens, cursor, p.name, 32);
				if (!span) { return; }
				cursor = span->end;
				nav_type t = auto_spelled(p.type.get()) ? element_of(type_of(container)) : nav_from_type_info(p.type.get());
				declare_local(p.name, *span, symbol_kind::local, p.type ? type_text(p.type.get()) : "auto&", t);
			};
			declare(f->loop_param, f->container.get());
			for (size_t i = 0; i < f->extra_params.size(); ++i) {
				declare(f->extra_params[i], i < f->extra_containers.size() ? f->extra_containers[i].get() : nullptr);
			}
			walk_stmt(f->body.get());
			pop_scope();
			break;
		}
		case jai::node_type::return_stmt: walk_expr(static_cast<const jai::return_stmt*>(s)->value.get()); break;
		case jai::node_type::try_stmt: {
			auto* t = static_cast<const jai::try_stmt*>(s);
			walk_stmt(t->try_block.get());
			if (t->catch_block) {
				text_pos open = punct_pos(t->catch_block->location);
				push_scope(open, stmt_end(t->catch_block.get()));
				if (!t->catch_var.empty()) {
					if (auto span = find_name_before(u_.tokens, open, t->catch_var, 8)) {
						declare_local(t->catch_var, *span, symbol_kind::local, "var", {});
					}
				}
				walk_stmt(t->catch_block.get());
				pop_scope();
			}
			break;
		}
		case jai::node_type::switch_stmt: {
			auto* sw = static_cast<const jai::switch_stmt*>(s);
			walk_expr(sw->condition.get());
			text_pos start = pos_of(sw->location);
			text_pos end = block_end_after(u_.tokens, start).value_or(start);
			for (const auto& cs : sw->cases) {
				walk_expr(cs->value.get());
				push_scope(pos_of(cs->location), end);
				for (const auto& st : cs->body) { walk_stmt(st.get()); }
				pop_scope();
			}
			if (sw->default_case) {
				push_scope(pos_of(sw->default_case->location), end);
				for (const auto& st : sw->default_case->body) { walk_stmt(st.get()); }
				pop_scope();
			}
			break;
		}
		default:
			if (s->get_type() >= jai::node_type::variable_decl) { walk_decl(static_cast<const jai::declaration*>(s)); }
			break;
	}
}

void semantic_walker::walk_expr(const jai::expression* e) {
	if (!e || jai::detail::native_stack_low()) { return; }
	switch (e->get_type()) {
		case jai::node_type::identifier_expr: {
			auto* id = static_cast<const jai::identifier_expr*>(e);
			if (id->name == k_placeholder) { capture_scope(); return; }
			uint32_t s = resolve_name(id->name, id->global_qualified);
			if (s != UINT32_MAX) {
				expr_symbol_[e] = s;
				text_span span = name_span(id->location, id->name);
				if (!span_spells(u_, span, id->name) && id->global_qualified) {
					span.start.col += 2;
					span.end.col += 2;
				}
				occur(span, s, false);
			}
			break;
		}
		case jai::node_type::binary_expr: {
			auto* b = static_cast<const jai::binary_expr*>(e);
			walk_expr(b->left.get());
			walk_expr(b->right.get());
			break;
		}
		case jai::node_type::unary_expr: walk_expr(static_cast<const jai::unary_expr*>(e)->operand.get()); break;
		case jai::node_type::assignment_expr: {
			auto* a = static_cast<const jai::assignment_expr*>(e);
			walk_expr(a->target.get());
			walk_expr(a->value.get());
			break;
		}
		case jai::node_type::call_expr: {
			auto* c = static_cast<const jai::call_expr*>(e);
			walk_expr(c->callee.get());
			for (const auto& a : c->arguments) { walk_expr(a.get()); }
			break;
		}
		case jai::node_type::member_expr: walk_member(static_cast<const jai::member_expr*>(e)); break;
		case jai::node_type::lambda_expr: walk_lambda(static_cast<const jai::lambda_expr*>(e)); break;
		case jai::node_type::new_expr:
			for (const auto& a : static_cast<const jai::new_expr*>(e)->arguments) { walk_expr(a.get()); }
			break;
		case jai::node_type::ternary_expr: {
			auto* t = static_cast<const jai::ternary_expr*>(e);
			walk_expr(t->condition.get());
			walk_expr(t->then_expression.get());
			walk_expr(t->else_expression.get());
			break;
		}
		case jai::node_type::array_literal_expr:
			for (const auto& el : static_cast<const jai::array_literal_expr*>(e)->elements) { walk_expr(el.get()); }
			break;
		case jai::node_type::map_literal_expr:
			for (const auto& [k, v] : static_cast<const jai::map_literal_expr*>(e)->entries) { walk_expr(k.get()); walk_expr(v.get()); }
			break;
		case jai::node_type::throw_expr: walk_expr(static_cast<const jai::throw_expr*>(e)->value.get()); break;
		case jai::node_type::yield_expr: walk_expr(static_cast<const jai::yield_expr*>(e)->value.get()); break;
		case jai::node_type::include_expr: walk_expr(static_cast<const jai::include_expr*>(e)->path_expr.get()); break;
		case jai::node_type::cast_expr: walk_expr(static_cast<const jai::cast_expr*>(e)->operand.get()); break;
		default: break;
	}
}

void semantic_walker::walk_member(const jai::member_expr* m) {
	if (m->is_static && walk_static_path(m)) { return; }
	walk_expr(m->object.get());
	nav_type t = type_of(m->object.get());
	if (m->member == k_placeholder) {
		m_.captured = true;
		m_.capture_member_receiver = t.cls;
		m_.capture_member_ti = t.ti;
		switch (t.kind) {
			case nav_type::object: m_.capture_member_kind = "object"; break;
			case nav_type::class_ref: m_.capture_member_kind = "class"; break;
			case nav_type::enum_ref: m_.capture_member_kind = "enum"; break;
			case nav_type::array: m_.capture_member_kind = "array"; break;
			case nav_type::map: m_.capture_member_kind = "map"; break;
			case nav_type::string: m_.capture_member_kind = "string"; break;
			case nav_type::weak: m_.capture_member_kind = "weak"; break;
			default: m_.capture_member_kind = "unknown"; break;
		}
		return;
	}
	resolve_member_on(t, m);
}

void semantic_walker::resolve_member_on(const nav_type& t, const jai::member_expr* m) {
	text_span span = name_span(m->location, m->member);
	uint32_t s = UINT32_MAX;
	switch (t.kind) {
		case nav_type::object:
		case nav_type::class_ref: {
			auto r = view_.find_member(t.cls, m->member, t.kind == nav_type::class_ref);
			if (r.found) {
				s = view_.member_symbol(m_, r, m->member);
				if (!symbol_type_.count(s)) { symbol_type_[s] = view_.declared_type(r.decl); }
			} else if (t.kind == nav_type::class_ref) {
				if (const jai::enum_decl* e = view_.find_enum(t.cls)) { s = view_.enum_value_symbol(m_, e, m->member); }
			}
			if (s == UINT32_MAX && !view_.class_known(t.cls)) { m_.unresolved_members.push_back({std::string(m->member), span}); }
			break;
		}
		case nav_type::enum_ref:
			if (const jai::enum_decl* e = view_.find_enum(t.cls)) {
				for (const auto& [vname, _] : e->values) {
					if (vname == m->member) { s = view_.enum_value_symbol(m_, e, m->member); break; }
				}
			}
			break;
		case nav_type::array: s = view_.builtin_symbol(m_, "array", m->member); break;
		case nav_type::map: s = view_.builtin_symbol(m_, "map", m->member); break;
		case nav_type::string: s = view_.builtin_symbol(m_, "string", m->member); break;
		case nav_type::weak: s = view_.builtin_symbol(m_, "weak_ptr", m->member); break;
		default:
			m_.unresolved_members.push_back({std::string(m->member), span});
			break;
	}
	if (s != UINT32_MAX) {
		expr_symbol_[m] = s;
		occur(span, s, false);
	}
}

bool semantic_walker::walk_static_path(const jai::member_expr* m) {
	std::vector<const jai::member_expr*> chain;
	const jai::expression* e = m;
	while (e && e->get_type() == jai::node_type::member_expr && static_cast<const jai::member_expr*>(e)->is_static) {
		chain.push_back(static_cast<const jai::member_expr*>(e));
		e = static_cast<const jai::member_expr*>(e)->object.get();
	}
	if (!e || e->get_type() != jai::node_type::identifier_expr) { return false; }
	auto* root = static_cast<const jai::identifier_expr*>(e);
	std::reverse(chain.begin(), chain.end());   // innermost first: chain[i]->member is segment i + 1
	std::vector<std::string_view> names{root->name};
	for (auto* c : chain) { names.push_back(c->member); }
	size_t ns_len = 0;
	std::string path(root->name);
	if (view_.is_namespace(path)) { ns_len = 1; }
	for (size_t i = 1; i + 1 < names.size(); ++i) {
		path += "::" + std::string(names[i]);
		if (view_.is_namespace(path)) { ns_len = i + 1; }
	}
	if (ns_len == 0) { return false; }
	std::string ns_path;
	for (size_t i = 0; i < ns_len; ++i) {
		ns_path += (i ? "::" : "") + std::string(names[i]);
		uint32_t s = view_.has_top(ns_path) ? view_.global_symbol(m_, ns_path) : view_.host_symbol(m_, ns_path, symbol_kind::host_namespace);
		text_span span = i == 0 ? name_span(root->location, root->name) : name_span(chain[i - 1]->location, chain[i - 1]->member);
		occur(span, s, false);
	}
	if (ns_len >= names.size()) { return true; }
	const jai::member_expr* node = chain[ns_len - 1];
	if (node->member == k_placeholder) {
		m_.captured = true;
		m_.capture_member_kind = "namespace";
		m_.capture_member_receiver = ns_path;
		return true;
	}
	std::string q = ns_path + "::" + std::string(node->member);
	uint32_t s = UINT32_MAX;
	if (view_.has_top(q)) {
		s = view_.global_symbol(m_, q);
		if (!symbol_type_.count(s)) {
			auto refs = view_.top_level(q);
			symbol_type_[s] = refs.empty() ? nav_type{} : view_.declared_type(refs.front().entry->node);
		}
	} else if (view_.host_namespace(ns_path)) {
		s = view_.host_symbol(m_, q, symbol_kind::host_function);
	}
	if (s != UINT32_MAX) {
		expr_symbol_[node] = s;
		occur(name_span(node->location, node->member), s, false);
	}
	for (size_t i = ns_len; i < chain.size(); ++i) {
		if (chain[i]->member == k_placeholder) {
			nav_type t = type_of(chain[i]->object.get());
			m_.captured = true;
			m_.capture_member_receiver = t.cls;
			m_.capture_member_kind = t.kind == nav_type::enum_ref ? "enum" : "class";
			continue;
		}
		resolve_member_on(type_of(chain[i]->object.get()), chain[i]);
	}
	return true;
}

void semantic_walker::walk_lambda(const jai::lambda_expr* l) {
	text_pos start = punct_pos(l->location);
	for (const auto& cap : l->captures) {
		if (cap.name == "this") { continue; }
		auto span = find_name_after(u_.tokens, start, cap.name, 32);
		uint32_t s = resolve_name(cap.name, false);
		if (span && s != UINT32_MAX) { occur(*span, s, false); }
	}
	text_pos end = l->body ? stmt_end(l->body.get()) : start;
	push_scope(start, end);
	fn_stack_.push_back("lambda");
	declare_params(l->parameters, after_open_paren(start));
	walk_stmt(l->body.get());
	fn_stack_.pop_back();
	pop_scope();
}

void semantic_walker::type_name_pass() {
	std::unordered_set<uint64_t> covered;
	auto key = [](text_pos p) { return (static_cast<uint64_t>(p.line) << 32) | p.col; };
	for (const auto& o : m_.occurrences) { covered.insert(key(o.span.start)); }
	for (size_t i = 0; i < u_.tokens.size(); ++i) {
		const lex_token& t = u_.tokens[i];
		if (t.type != jai::token_type::identifier || covered.count(key(t.pos))) { continue; }
		if (i > 0) {
			auto prev = u_.tokens[i - 1].type;
			if (prev == jai::token_type::dot || prev == jai::token_type::arrow || prev == jai::token_type::question_dot || prev == jai::token_type::colon_colon) { continue; }
		}
		uint32_t s = UINT32_MAX;
		if (view_.find_class(t.lexeme) || view_.find_enum(t.lexeme)) {
			s = view_.global_symbol(m_, std::string(t.lexeme));
		} else if (jai::detail::engine_access::get_class_definition(eng_, std::string(t.lexeme))) {
			s = view_.host_symbol(m_, std::string(t.lexeme), symbol_kind::host_class);
		}
		if (s != UINT32_MAX) { occur({t.pos, {t.pos.line, t.pos.col + t.length}}, s, false); }
	}
}

} // namespace jai_lsp
