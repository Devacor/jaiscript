#pragma once

#ifndef __JAISCRIPT_DETAIL_CONST_RULES_HPP__
#define __JAISCRIPT_DETAIL_CONST_RULES_HPP__

// const: the texts, the receiver-writes verdict of a method, and the run time's half of a const_runtime
// verdict. ONE kernel for the checker (load diagnostics and the stamps) and both backends (the guard at a stamped node).
//
// A write through a const is a write whose path from the const name crosses no handle (a shared_ptr or weak_ptr: past it the
// pointee, which the const does not own). The checker decides every path whose steps it can type; a path through a var is
// decided here, when the node runs, by walking the path's values without running code.

#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/lvalue_shape.hpp>
#include <jaiscript/detail/ref_param_writes.hpp>
#include <jaiscript/detail/ref_lvalue.hpp>
#include <jaiscript/detail/arg_capture.hpp>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jai::detail {

	// ---------------------------------------------------------------- texts

	inline std::string const_spelled(const expression* e) {
		std::string s = target_spelling(e);
		if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') { s = s.substr(1, s.size() - 2); }
		return s;
	}

	inline bool const_names_root(const expression* e) {
		return e && (e->get_type() == node_type::identifier_expr ||
		             (e->get_type() == node_type::member_expr && static_cast<const member_expr*>(e)->is_static));
	}

	inline std::string const_store_text(const expression* target, std::string_view root) {
		const std::string spelled = const_spelled(target);
		if (const_names_root(target)) { return "Cannot assign to '" + spelled + "': it is read-only"; }
		return "Cannot assign to '" + spelled + "': it is read-only ('" + std::string(root) + "' is const)";
	}
	inline std::string const_store_diagnostic(const expression* target, std::string_view root) {
		const std::string spelled = const_spelled(target);
		if (const_names_root(target)) { return "cannot assign to const '" + spelled + "'"; }
		return "cannot assign to '" + spelled + "': '" + std::string(root) + "' is const";
	}
	inline std::string const_call_text(const expression* receiver, std::string_view root, std::string_view method, bool builtin, size_t line) {
		std::string out = "Cannot call '" + std::string(method) + "' on '" + const_spelled(receiver) + "': '" + std::string(root) + "' is const and '" +
		                  std::string(method) + (builtin ? "' changes it" : "' writes its receiver");
		if (line != 0) { out += " (line " + std::to_string(line) + ")"; }
		return out;
	}
	inline std::string const_bind_text(std::string_view name, std::string_view declared, const expression* source, std::string_view root, bool element) {
		std::string what = element ? "an element of const '" + std::string(root) + "'"
		                 : const_names_root(source) ? "const '" + std::string(root) + "'"
		                 : "'" + const_spelled(source) + "' of const '" + std::string(root) + "'";
		return "Cannot bind '" + std::string(name) + "' (declared " + std::string(declared) + ") to " + what + "; declare it const " +
		       std::string(declared) + " or auto&";
	}
	inline std::string const_argument_text(const expression* argument, std::string_view root, std::string_view parameter, std::string_view callee, size_t line) {
		std::string what = const_names_root(argument) ? "const '" + std::string(root) + "'" : "'" + const_spelled(argument) + "' of const '" + std::string(root) + "'";
		std::string out = "Cannot pass " + what + " to parameter '" + std::string(parameter) + "' of '" + std::string(callee) + "': '" + std::string(callee) + "' writes through it";
		if (line != 0) { out += " (line " + std::to_string(line) + ")"; }
		return out;
	}
	inline std::string const_cast_text(const expression* operand, std::string_view root, std::string_view target) {
		std::string what = const_names_root(operand) ? "const '" + std::string(root) + "'" : "'" + const_spelled(operand) + "' of const '" + std::string(root) + "'";
		return "Cannot cast " + what + " to " + std::string(target) + ": cast<" + std::string(target.substr(0, target.size() && target.back() == '&' ? target.size() - 1 : target.size())) +
		       ">(" + const_spelled(operand) + ") copies it";
	}
	inline std::string const_share_text(const expression* source, std::string_view root, std::string_view handle) {
		std::string what = const_names_root(source) ? "const '" + std::string(root) + "'" : "'" + const_spelled(source) + "' of const '" + std::string(root) + "'";
		return "Cannot share " + what + " as " + std::string(handle) + "; copy it into a variable first";
	}
	inline std::string const_return_text(const expression* value, std::string_view root, std::string_view fn, std::string_view declared) {
		std::string what = const_names_root(value) ? "const '" + std::string(root) + "'" : "'" + const_spelled(value) + "' of const '" + std::string(root) + "'";
		return "Cannot return " + what + " from '" + std::string(fn) + "' by reference (declared -> " + std::string(declared) + "); return it by value";
	}
	// a checker message: the run time's text with a lowercase first letter
	inline std::string const_lowercase(std::string text) {
		if (!text.empty() && text[0] >= 'A' && text[0] <= 'Z') { text[0] = static_cast<char>(text[0] - 'A' + 'a'); }
		return text;
	}

	inline bool const_handle_type(const type_info* t) {
		return t && (t->base_type == script_value_type::jai_shared_ptr_type || t->base_type == script_value_type::jai_weak_ptr_type);
	}

	// ---------------------------------------------------------------- the receiver-writes verdict

	struct receiver_write {
		bool writes = false;
		bool certain = false;   // false: a write the analysis could not type (a path through a var); the run time lets it run
		size_t line = 0, column = 0;
	};

	// What the analysis may ask: a field's declared type in a class (is_field false: no such field), a class's methods of a name
	// (false: the class is out of reach), and the free functions (ref_param_writes' resolver)
	struct receiver_view {
		std::function<const type_info*(std::string_view cls, std::string_view field, bool& is_field)> field_type;
		std::function<bool(std::string_view cls, std::string_view name, std::vector<const function_decl*>& out)> methods;
		ref_write_resolver functions;
	};

	// Whether a method writes the instance it is called on: a store to a field (any spelling) or through one, a call on the
	// receiver or a field that writes it, a mutating builtin on a field, the receiver or a field passed to a writing reference
	// parameter or an unseen callee, a mutable alias of the receiver or a field written through, the same inside the lambdas
	// the body creates. Writing statics or globals is not a receiver write, nor is returning a field by reference (the
	// caller's path takes the step). Recursion is read-only until a write is found.
	class receiver_write_analysis {
	public:
		explicit receiver_write_analysis(receiver_view v) : view_(std::move(v)) {}

		receiver_write analyze(const function_decl* method, std::string_view cls) {
			receiver_write none;
			if (!method || !method->body || method->is_static) { return none; }
			for (const auto* f : in_progress_) { if (f == method) { return none; } }
			in_progress_.push_back(method);
			walk w(*this, method, cls);
			w.stmt(method->body.get());
			in_progress_.pop_back();
			return w.found;
		}

		// every overload of `name` in `cls`: certain when each certainly writes, a write when any does
		receiver_write analyze_methods(std::string_view cls, std::string_view name) {
			std::vector<const function_decl*> out;
			receiver_write r;
			if (!view_.methods || !view_.methods(cls, name, out) || out.empty()) { r.writes = true; r.certain = false; return r; }
			bool all_certain = true;
			for (const auto* f : out) {
				receiver_write w = analyze(f, cls);
				if (w.writes) {
					if (!r.writes) { r.line = w.line; r.column = w.column; }
					r.writes = true;
					all_certain = all_certain && w.certain;
				} else {
					all_certain = false;
				}
			}
			r.certain = r.writes && all_certain;
			return r;
		}

	private:
		receiver_view view_;
		std::vector<const function_decl*> in_progress_;

		struct walk {
			receiver_write_analysis& owner;
			const function_decl* fn;
			std::string_view cls;
			std::vector<std::string_view> locals;
			std::vector<std::pair<std::string_view, bool>> aliases;   // a reference to the receiver or into it: certain?
			receiver_write found;

			walk(receiver_write_analysis& o, const function_decl* f, std::string_view c) : owner(o), fn(f), cls(c) {
				for (const auto& p : f->parameters) { locals.push_back(p.name); }
			}

			void note(const ast_node* n, bool certain) {
				if (!n) { return; }
				if (found.writes && (found.certain || !certain)) { return; }
				found.writes = true;
				found.certain = certain;
				found.line = n->location.line;
				found.column = n->location.column;
			}
			bool is_local(std::string_view name) const {
				for (auto l : locals) { if (l == name) { return true; } }
				return false;
			}
			const std::pair<std::string_view, bool>* alias_of(std::string_view name) const {
				for (auto it = aliases.rbegin(); it != aliases.rend(); ++it) { if (it->first == name) { return &*it; } }
				return nullptr;
			}

			// 0: not into the receiver; 1: into it, every step typed; 2: into it through an untyped step. A step through a handle
			// leaves the receiver (0). `value_too`: the expression's own value is the receiver's storage too (a call's receiver),
			// so its type counts as an intermediate step
			uint8_t path(const expression* e, bool value_too = false) const {
				std::vector<const expression*> steps;
				const expression* base = e;
				while (base) {
					const node_type t = base->get_type();
					if (t == node_type::member_expr && !static_cast<const member_expr*>(base)->is_static) {
						steps.push_back(base);
						base = static_cast<const member_expr*>(base)->object.get();
					} else if (t == node_type::binary_expr && static_cast<const binary_expr*>(base)->op.type == token_type::left_bracket) {
						steps.push_back(base);
						base = static_cast<const binary_expr*>(base)->left.get();
					} else if (t == node_type::cast_expr) {
						base = static_cast<const cast_expr*>(base)->operand.get();
					} else {
						break;
					}
				}
				if (!base) { return 0; }
				// the receiver's own type, then each step's
				const type_info* cur = nullptr;
				bool cur_known = false;
				std::string cur_cls;
				uint8_t verdict = 1;
				if (base->get_type() == node_type::this_expr) {
					cur_cls = std::string(cls);
					cur_known = true;
				} else if (base->get_type() == node_type::identifier_expr) {
					auto* id = static_cast<const identifier_expr*>(base);
					if (id->global_qualified) { return 0; }
					if (const auto* a = alias_of(id->name)) {
						verdict = a->second ? 1 : 2;
						cur_known = false;
					} else if (is_local(id->name)) {
						return 0;
					} else {
						bool is_field = false;
						const type_info* ft = owner.view_.field_type ? owner.view_.field_type(cls, id->name, is_field) : nullptr;
						if (!is_field) { return 0; }
						// the bare field is the first step
						if (steps.empty() && !value_too) { return verdict; }
						cur = ft;
						cur_known = ft != nullptr;
					}
				} else {
					return 0;
				}
				for (size_t i = steps.size(); i-- > 0;) {
					const bool last = i == 0 && !value_too;
					// the value this step reads from is intermediate storage: a handle there leaves the receiver
					if (cur_known && cur && const_handle_type(cur)) { return 0; }
					if (!cur_known) { verdict = 2; }
					if (last) { return verdict; }
					const expression* step = steps[i];
					if (step->get_type() == node_type::member_expr) {
						auto* m = static_cast<const member_expr*>(step);
						std::string owner_cls = !cur_cls.empty() ? cur_cls
						                      : (cur && cur->base_type == script_value_type::jai_object_type ? cur->type_name : std::string());
						cur_cls.clear();
						if (owner_cls.empty()) { cur = nullptr; cur_known = false; continue; }
						bool is_field = false;
						const type_info* ft = owner.view_.field_type ? owner.view_.field_type(owner_cls, m->member, is_field) : nullptr;
						cur = ft;
						cur_known = is_field && ft != nullptr;
					} else {
						cur_cls.clear();
						if (cur && cur->base_type == script_value_type::jai_array_type) { cur = cur->element_type().get(); }
						else if (cur && cur->base_type == script_value_type::jai_map_type) { cur = cur->value_type().get(); }
						else { cur = nullptr; }
						cur_known = cur != nullptr && cur->base_type != script_value_type::jai_any_type;
					}
				}
				// value_too: the expression's own value
				if (cur_known && cur && const_handle_type(cur)) { return 0; }
				if (!cur_known && cur_cls.empty()) { verdict = 2; }
				return verdict;
			}

			// the class of a receiver expression, when typed (this, a field of a declared class type)
			std::string receiver_class(const expression* recv) const {
				if (!recv) { return {}; }
				if (recv->get_type() == node_type::this_expr) { return std::string(cls); }
				if (recv->get_type() == node_type::identifier_expr) {
					auto* id = static_cast<const identifier_expr*>(recv);
					if (is_local(id->name) || alias_of(id->name) || id->global_qualified) { return {}; }
					bool is_field = false;
					const type_info* ft = owner.view_.field_type ? owner.view_.field_type(cls, id->name, is_field) : nullptr;
					if (is_field && ft && ft->base_type == script_value_type::jai_object_type) { return ft->type_name; }
					return {};
				}
				if (recv->get_type() == node_type::member_expr && !static_cast<const member_expr*>(recv)->is_static) {
					auto* m = static_cast<const member_expr*>(recv);
					const std::string owner_cls = receiver_class(m->object.get());
					if (owner_cls.empty()) { return {}; }
					bool is_field = false;
					const type_info* ft = owner.view_.field_type ? owner.view_.field_type(owner_cls, m->member, is_field) : nullptr;
					if (is_field && ft && ft->base_type == script_value_type::jai_object_type) { return ft->type_name; }
				}
				return {};
			}
			const type_info* receiver_type(const expression* recv) const {
				if (!recv) { return nullptr; }
				if (recv->get_type() == node_type::identifier_expr) {
					auto* id = static_cast<const identifier_expr*>(recv);
					if (is_local(id->name) || alias_of(id->name) || id->global_qualified) { return nullptr; }
					bool is_field = false;
					return owner.view_.field_type ? owner.view_.field_type(cls, id->name, is_field) : nullptr;
				}
				if (recv->get_type() == node_type::member_expr && !static_cast<const member_expr*>(recv)->is_static) {
					auto* m = static_cast<const member_expr*>(recv);
					const std::string owner_cls = receiver_class(m->object.get());
					if (owner_cls.empty()) { return nullptr; }
					bool is_field = false;
					return owner.view_.field_type ? owner.view_.field_type(owner_cls, m->member, is_field) : nullptr;
				}
				return nullptr;
			}

			void method_call(const call_expr* c, const expression* recv, std::string_view name) {
				const uint8_t p = path(recv, /*value_too=*/true);
				if (p == 0) { return; }
				std::string rc = receiver_class(recv);
				if (!rc.empty()) {
					receiver_write w = owner.analyze_methods(rc, name);
					if (w.writes) { note(c, p == 1 && w.certain); }
					return;
				}
				const type_info* rt = receiver_type(recv);
				if (rt) {
					switch (rt->base_type) {
						case script_value_type::jai_array_type:
							if (!builtin_call_reads(c, name, owner.view_.functions)) { note(c, p == 1); }
							return;
						case script_value_type::jai_map_type: case script_value_type::jai_string_type: case script_value_type::jai_utf8_string_type:
							if (!ref_write_reading_method(name) && !ref_write_element_callback(name)) { note(c, p == 1); }   // a string's find takes no callback
							return;
						default: break;
					}
				}
				if (!builtin_call_reads(c, name, owner.view_.functions)) { note(c, false); }
			}

			void argument_passed(const call_expr* c, size_t i, const expression* arg) {
				const uint8_t p = path(arg);
				if (p == 0) { return; }
				const expression* callee = c->callee.get();
				if (callee->get_type() == node_type::identifier_expr) {
					auto* id = static_cast<const identifier_expr*>(callee);
					std::vector<ref_write_callee> overloads;
					if (!id->global_qualified && is_local(id->name)) { note(arg, false); return; }
					if (owner.view_.functions && owner.view_.functions(*id, overloads)) {
						for (const auto& o : overloads) {
							if (!o.params || i >= o.params->size()) { continue; }
							const parameter& prm = (*o.params)[i];
							if (const_handle_type(prm.type.get())) { note(arg, p == 1); return; }
							if (prm.is_reference && !prm.is_const) {
								ref_write_analysis rw(owner.view_.functions);
								if (rw.analyze(o, i).writes) { note(arg, p == 1); return; }
							}
						}
						return;
					}
					if (!ref_write_copying_builtin(id->name)) { note(arg, false); }
					return;
				}
				if (callee->get_type() == node_type::member_expr) {
					auto* m = static_cast<const member_expr*>(callee);
					if (m->is_static && m->object && m->object->get_type() == node_type::identifier_expr) {
						const std::string_view ns = static_cast<const identifier_expr*>(m->object.get())->name;
						if (ns == "math" || ns == "string" || ns == "array" || ns == "map") { return; }
					}
					if (!m->is_static && ref_write_builtin_method(m->member)) { return; }
				}
				note(arg, false);
			}

			void expr(const expression* e) {
				if (!e || (found.writes && found.certain)) { return; }
				switch (e->get_type()) {
					case node_type::assignment_expr: {
						auto* a = static_cast<const assignment_expr*>(e);
						if (const uint8_t p = path(a->target.get())) { note(a, p == 1); }
						expr(a->value.get());
						if (a->target->get_type() != node_type::identifier_expr) { expr(a->target.get()); }
						return;
					}
					case node_type::unary_expr: {
						auto* u = static_cast<const unary_expr*>(e);
						if (u->op.type == token_type::plus_plus || u->op.type == token_type::minus_minus) {
							if (const uint8_t p = path(u->operand.get())) { note(u, p == 1); }
						}
						expr(u->operand.get());
						return;
					}
					case node_type::call_expr: {
						auto* c = static_cast<const call_expr*>(e);
						if (c->callee && c->callee->get_type() == node_type::member_expr) {
							auto* m = static_cast<const member_expr*>(c->callee.get());
							if (!m->is_static) {
								method_call(c, m->object.get(), m->member);
								expr(m->object.get());
							}
						} else if (c->callee && c->callee->get_type() == node_type::identifier_expr) {
							auto* id = static_cast<const identifier_expr*>(c->callee.get());
							std::vector<const function_decl*> own;
							if (!id->global_qualified && !is_local(id->name) && owner.view_.methods && owner.view_.methods(cls, id->name, own) && !own.empty()) {
								bool instance = false;
								for (const auto* f : own) { if (!f->is_static) { instance = true; } }
								if (instance) {
									receiver_write w = owner.analyze_methods(cls, id->name);
									if (w.writes) { note(c, w.certain); }
								}
							}
						}
						for (size_t i = 0; i < c->arguments.size(); ++i) {
							argument_passed(c, i, c->arguments[i].get());
							expr(c->arguments[i].get());
						}
						return;
					}
					case node_type::binary_expr: {
						auto* b = static_cast<const binary_expr*>(e);
						expr(b->left.get());
						expr(b->right.get());
						return;
					}
					case node_type::member_expr: expr(static_cast<const member_expr*>(e)->object.get()); return;
					case node_type::ternary_expr: {
						auto* t = static_cast<const ternary_expr*>(e);
						expr(t->condition.get());
						expr(t->then_expression.get());
						expr(t->else_expression.get());
						return;
					}
					case node_type::array_literal_expr:
						for (const auto& el : static_cast<const array_literal_expr*>(e)->elements) { expr(el.get()); }
						return;
					case node_type::map_literal_expr:
						for (const auto& kv : static_cast<const map_literal_expr*>(e)->entries) { expr(kv.first.get()); expr(kv.second.get()); }
						return;
					case node_type::new_expr:
						for (const auto& a : static_cast<const new_expr*>(e)->arguments) {
							if (path(a.get())) { note(a.get(), false); }
							expr(a.get());
						}
						return;
					case node_type::lambda_expr: {
						auto* l = static_cast<const lambda_expr*>(e);
						const size_t mark = locals.size();
						for (const auto& p : l->parameters) { locals.push_back(p.name); }
						stmt(l->body.get());
						locals.resize(mark);
						return;
					}
					case node_type::cast_expr: {
						auto* c = static_cast<const cast_expr*>(e);
						if ((c->by_reference || const_handle_type(c->target.get())) && !c->implicit_ref) {
							if (const uint8_t p = path(c->operand.get())) { note(c, false); (void)p; }
						}
						expr(c->operand.get());
						return;
					}
					case node_type::throw_expr: expr(static_cast<const throw_expr*>(e)->value.get()); return;
					case node_type::yield_expr: expr(static_cast<const yield_expr*>(e)->value.get()); return;
					default: return;
				}
			}

			void alias(std::string_view name, const expression* source) {
				if (const uint8_t p = path(source)) { aliases.emplace_back(name, p == 1); }
			}

			void stmt(const statement* s) {
				if (!s || (found.writes && found.certain)) { return; }
				switch (s->get_type()) {
					case node_type::expression_stmt: expr(static_cast<const expression_stmt*>(s)->expression.get()); return;
					case node_type::expression_decl: expr(static_cast<const expression_decl*>(s)->expression.get()); return;
					case node_type::statement_decl: stmt(static_cast<const statement_decl*>(s)->statement.get()); return;
					case node_type::block_stmt:
						for (const auto& d : static_cast<const block_stmt*>(s)->declarations) { stmt(d.get()); }
						return;
					case node_type::if_stmt: {
						auto* is = static_cast<const if_stmt*>(s);
						expr(is->condition.get());
						stmt(is->then_statement.get());
						stmt(is->else_statement.get());
						return;
					}
					case node_type::while_stmt: {
						auto* ws = static_cast<const while_stmt*>(s);
						expr(ws->condition.get());
						stmt(ws->body.get());
						return;
					}
					case node_type::for_stmt: {
						auto* fs = static_cast<const for_stmt*>(s);
						stmt(fs->initializer.get());
						expr(fs->condition.get());
						expr(fs->update.get());
						stmt(fs->body.get());
						return;
					}
					case node_type::range_for_stmt: {
						auto* rf = static_cast<const range_for_stmt*>(s);
						expr(rf->container.get());
						locals.push_back(rf->variable_name);
						if (rf->is_reference && !rf->is_const) { alias(rf->variable_name, rf->container.get()); }
						stmt(rf->body.get());
						return;
					}
					case node_type::return_stmt: expr(static_cast<const return_stmt*>(s)->value.get()); return;
					case node_type::switch_stmt: {
						auto* sw = static_cast<const switch_stmt*>(s);
						expr(sw->condition.get());
						for (const auto& c : sw->cases) {
							expr(c->value.get());
							for (const auto& st : c->body) { stmt(st.get()); }
						}
						if (sw->default_case) {
							for (const auto& st : sw->default_case->body) { stmt(st.get()); }
						}
						return;
					}
					case node_type::try_stmt: {
						auto* tr = static_cast<const try_stmt*>(s);
						stmt(tr->try_block.get());
						stmt(tr->catch_block.get());
						return;
					}
					case node_type::variable_decl: {
						auto* vd = static_cast<const variable_decl*>(s);
						expr(vd->initializer.get());
						locals.push_back(vd->name);
						if (vd->type && vd->type->is_reference() && !vd->is_const) { alias(vd->name, vd->initializer.get()); }
						return;
					}
					case node_type::destructuring_decl: {
						auto* dd = static_cast<const destructuring_decl*>(s);
						expr(dd->initializer.get());
						for (const auto& [name, id] : dd->names) { locals.push_back(name); }
						return;
					}
					default: return;
				}
			}
		};
	};

	// ---------------------------------------------------------------- the run time

	// the verdict a node carries, null when none (the nodes the checker stamps)
	inline const const_site* const_site_of(const ast_node* node) {
		if (!node) { return nullptr; }
		const const_site* site = nullptr;
		switch (node->get_type()) {
			case node_type::assignment_expr: site = &static_cast<const assignment_expr*>(node)->const_check; break;
			case node_type::unary_expr: site = &static_cast<const unary_expr*>(node)->const_check; break;
			case node_type::call_expr: site = &static_cast<const call_expr*>(node)->const_check; break;
			case node_type::cast_expr: site = &static_cast<const cast_expr*>(node)->const_check; break;
			case node_type::variable_decl: site = &static_cast<const variable_decl*>(node)->const_check; break;
			case node_type::return_stmt: site = &static_cast<const return_stmt*>(node)->const_check; break;
			case node_type::range_for_stmt: site = &static_cast<const range_for_stmt*>(node)->const_check; break;
			default: return nullptr;
		}
		return site->verdict != const_none ? site : nullptr;
	}

	// a handle (a shared_ptr or weak_ptr, a host object held by pointer): past it the pointee, which no const owns
	inline bool const_handle_value(const script_value& v) {
		const size_t idx = v.raw_storage_index();
		if (idx == script_value::TYPEID_WEAK_PTR) { return true; }
		if (v.is_shared_ptr_type() || v.is_weak_ptr_type()) { return true; }
		if (idx == script_value::TYPEID_OBJECT && !v.is_null()) {
			if (!const_cast<script_value&>(v).get_class_instance()) { return true; }
		}
		return false;
	}

	// Walks a const-rooted path's values without running code: true when a value on the way (the root or a step before the
	// last) is a handle, false when none is; nullopt when a step needs code (a property, a computed index, a call)
	inline std::optional<bool> const_path_crosses_handle(const expression* e, const caller_frame_view& caller, environment* env,
	                                                     engine* eng, string_symbolizer* symbolizer, bool value_too = false) {
		std::vector<const expression*> steps;
		const expression* base = e;
		while (base) {
			const node_type t = base->get_type();
			if (t == node_type::member_expr && !static_cast<const member_expr*>(base)->is_static) {
				steps.push_back(base);
				base = static_cast<const member_expr*>(base)->object.get();
			} else if (t == node_type::binary_expr && static_cast<const binary_expr*>(base)->op.type == token_type::left_bracket) {
				steps.push_back(base);
				base = static_cast<const binary_expr*>(base)->left.get();
			} else if (t == node_type::cast_expr && static_cast<const cast_expr*>(base)->implicit_ref) {
				base = static_cast<const cast_expr*>(base)->operand.get();
			} else {
				break;
			}
		}
		if (!base || base->get_type() != node_type::identifier_expr) { return std::nullopt; }
		std::optional<checked_result<void>> access_error;
		const script_value* root = ref_identifier_storage(static_cast<const identifier_expr*>(base), caller, env, eng, symbolizer, access_error);
		if (!root || access_error) { return std::nullopt; }
		script_value cur = root->deref();
		auto index_value = [&](const expression* ix) -> std::optional<script_value> {
			if (ix->get_type() == node_type::literal_expr) { return static_cast<const literal_expr*>(ix)->value; }
			if (ix->get_type() == node_type::identifier_expr) {
				std::optional<checked_result<void>> err;
				const script_value* p = ref_identifier_storage(static_cast<const identifier_expr*>(ix), caller, env, eng, symbolizer, err);
				if (!p || err) { return std::nullopt; }
				return p->deref();
			}
			return std::nullopt;
		};
		for (size_t i = steps.size(); i-- > 0;) {
			const bool last = i == 0 && !value_too;
			if (const_handle_value(cur)) { return true; }
			if (last) { return false; }
			const expression* step = steps[i];
			if (step->get_type() == node_type::member_expr) {
				auto* m = static_cast<const member_expr*>(step);
				if (!cur.is_object() || cur.is_null()) { return std::nullopt; }
				auto instance = cur.get_class_instance();
				if (!instance) { return std::nullopt; }
				const uint64_t id = m->member_id != UINT64_MAX ? m->member_id : symbolizer->find(m->member).value_or(UINT64_MAX);
				if (id == UINT64_MAX) { return std::nullopt; }
				const script_value* field = instance->find_field_value(id);
				if (!field) { return std::nullopt; }
				cur = field->deref();
			} else {
				auto* b = static_cast<const binary_expr*>(step);
				auto key = index_value(b->right.get());
				if (!key) { return std::nullopt; }
				const script_value k = key->deref();
				if (cur.is_array()) {
					if (!k.is_int()) { return std::nullopt; }
					auto storage = cur.get_array_storage();
					const script_int ix = k.unchecked_as_int();
					if (ix < 0 || static_cast<size_t>(ix) >= storage->size()) { return std::nullopt; }
					cur = storage->get(static_cast<size_t>(ix), eng).deref();
				} else if (cur.is_map()) {
					const auto& map = cur.as_map();
					auto it = map.find(k);
					if (it == map.end()) { return std::nullopt; }
					cur = it->second.deref();
				} else {
					return std::nullopt;
				}
			}
		}
		return const_handle_value(cur);
	}

	// The run time's receiver-writes view: field types and methods from the engine's class definitions
	inline receiver_view runtime_receiver_view(engine* eng, string_symbolizer* symbolizer) {
		receiver_view v;
		v.field_type = [eng, symbolizer](std::string_view cls, std::string_view field, bool& is_field) -> const type_info* {
			is_field = false;
			auto def = engine_access::get_class_definition(eng, std::string(cls));
			// a lookup, never an intern: a parallel worker runs this with the interner frozen, and a name no program interned is no field
			const std::optional<uint64_t> id = def ? symbolizer->find(field) : std::nullopt;
			if (!id || !def->find_field_declaring_class(*id)) { return nullptr; }
			is_field = true;
			return def->get_field_declared_type(*id).get();
		};
		v.methods = [eng, symbolizer](std::string_view cls, std::string_view name, std::vector<const function_decl*>& out) -> bool {
			auto def = engine_access::get_class_definition(eng, std::string(cls));
			const std::optional<uint64_t> found = def ? symbolizer->find(name) : std::nullopt;
			if (!def) { return false; }
			if (!found) { return true; }   // no class declares a name no program interned
			const uint64_t id = *found;
			// the nearest class of the chain that declares the name (an override hides its base's overloads)
			std::vector<const class_definition*> stack{def.get()};
			while (!stack.empty()) {
				const class_definition* c = stack.back();
				stack.pop_back();
				auto it = c->script_method_overloads().find(id);
				if (it != c->script_method_overloads().end() && !it->second.empty()) {
					for (const auto& f : it->second) { out.push_back(f.get()); }
					return true;
				}
				for (const auto& parent : c->get_parent_classes()) { if (parent) { stack.push_back(parent.get()); } }
			}
			return true;
		};
		return v;
	}

	// The callback a builtin call names, as the run time holds it (no code runs): a script function writes an element when it
	// writes through the reference parameter the element binds, any other callable when it binds the element by reference
	inline bool named_callback_writes_element(const call_expr* call, std::string_view name, const caller_frame_view& caller, environment* env,
	                                          engine* eng, string_symbolizer* symbolizer) {
		const expression* callback = call && !call->arguments.empty() ? call->arguments[0].get() : nullptr;
		if (!callback || callback->get_type() != node_type::identifier_expr) { return false; }
		std::optional<checked_result<void>> err;
		const script_value* held = ref_identifier_storage(static_cast<const identifier_expr*>(callback), caller, env, eng, symbolizer, err);
		if (!held || err) { return false; }
		const script_value& fn = held->deref();
		if (fn.raw_storage_index() != script_value::TYPEID_FUNCTION) { return false; }
		const size_t position = name == "reduce" ? 1 : 0;
		if (const auto* thunk = fn.unchecked_as_function().target<script_callable_thunk>();
		    thunk && thunk->payload.kind == script_callable::kind_type::function && thunk->payload.fn && thunk->payload.fn->shared_parameters) {
			const script_defined_function& f = *thunk->payload.fn;
			if (position >= f.shared_parameters->size()) { return false; }
			const parameter& element = (*f.shared_parameters)[position];
			return element.is_reference && !element.is_const &&
			       ref_write_analysis(nullptr).analyze(ref_write_callee{f.shared_parameters.get(), f.body.get(), {}, false, false}, position).writes;
		}
		return (static_cast<uint8_t>(callee_binding(fn, position)) & static_cast<uint8_t>(arg_binding::reference)) != 0;
	}

	// Whether calling `name` on the receiver value writes it: a builtin's mutators on a string, array or map and an array's element
	// callback that writes an element, a script method whose body certainly writes its receiver (receiver_write_analysis); anything
	// else runs
	inline std::optional<size_t> const_call_writes(const script_value& receiver_in, const call_expr* call, std::string_view name, const caller_frame_view& caller,
	                                               environment* env, engine* eng, string_symbolizer* symbolizer, bool& builtin) {
		const script_value& receiver = receiver_in.deref();
		builtin = false;
		if (receiver.is_array() || receiver.is_map() || receiver.is_string() || receiver.current_type() == script_value_type::jai_utf8_string_type) {
			builtin = true;
			if (ref_write_reading_method(name)) { return std::nullopt; }
			if (!ref_write_element_callback(name)) { return size_t(0); }
			if (receiver.is_array() && named_callback_writes_element(call, name, caller, env, eng, symbolizer)) { return size_t(0); }
			return std::nullopt;
		}
		if (!receiver.is_object() || receiver.is_null()) { return std::nullopt; }
		auto instance = const_cast<script_value&>(receiver).get_class_instance();
		if (!instance || !instance->get_class_definition()) { return std::nullopt; }
		receiver_write_analysis analysis(runtime_receiver_view(eng, symbolizer));
		const receiver_write w = analysis.analyze_methods(instance->get_class_definition()->get_name(), name);
		if (w.writes && w.certain) { return w.line; }
		return std::nullopt;
	}

	// The guard a stamped node runs when it is reached (both backends): const_raises raises its text; const_runtime walks the
	// path and raises when it crosses no handle (a call: when the method also writes its receiver)
	inline checked_result<void> const_guard(const ast_node* node, const const_site& site, const caller_frame_view& caller, environment* env,
	                                        engine* eng, string_symbolizer* symbolizer) {
		auto raise = [&](const std::string& text) {
			if (site.text_id != 0) { return checked_result<void>(make_error_code(runtime_error_code::type_mismatch), "{0}", site.text_id); }
			return checked_result<void>(make_error_code(runtime_error_code::type_mismatch), std::string_view(text));
		};
		if (site.verdict == const_raises) { return raise(site.text ? *site.text : std::string("Cannot assign to a const: it is read-only")); }
		if (site.verdict != const_runtime) { return {}; }
		const expression* path = nullptr;
		const call_expr* call = nullptr;
		switch (node->get_type()) {
			case node_type::assignment_expr: path = static_cast<const assignment_expr*>(node)->target.get(); break;
			case node_type::unary_expr: path = static_cast<const unary_expr*>(node)->operand.get(); break;
			case node_type::call_expr: {
				call = static_cast<const call_expr*>(node);
				if (call->callee && call->callee->get_type() == node_type::member_expr) { path = static_cast<const member_expr*>(call->callee.get())->object.get(); }
				break;
			}
			case node_type::cast_expr: path = static_cast<const cast_expr*>(node)->operand.get(); break;
			case node_type::variable_decl: path = static_cast<const variable_decl*>(node)->initializer.get(); break;
			case node_type::return_stmt: path = static_cast<const return_stmt*>(node)->value.get(); break;
			case node_type::range_for_stmt: path = static_cast<const range_for_stmt*>(node)->container.get(); break;
			default: return {};
		}
		if (!path) { return {}; }
		const std::optional<bool> crosses = const_path_crosses_handle(path, caller, env, eng, symbolizer, call != nullptr);
		if (!crosses || *crosses) { return {}; }
		if (call) {
			// the receiver's value, read as the path walk read it
			auto* m = static_cast<const member_expr*>(call->callee.get());
			std::optional<checked_result<void>> err;
			// re-walk to the receiver value: the identifier root plus steps (no code runs)
			const expression* recv = m->object.get();
			script_value value(std::monostate{}, eng);
			{
				std::vector<const expression*> steps;
				const expression* base = recv;
				while (base && ((base->get_type() == node_type::member_expr && !static_cast<const member_expr*>(base)->is_static) ||
				                (base->get_type() == node_type::binary_expr && static_cast<const binary_expr*>(base)->op.type == token_type::left_bracket))) {
					steps.push_back(base);
					base = base->get_type() == node_type::member_expr ? static_cast<const member_expr*>(base)->object.get() : static_cast<const binary_expr*>(base)->left.get();
				}
				if (!base || base->get_type() != node_type::identifier_expr) { return {}; }
				const script_value* root = ref_identifier_storage(static_cast<const identifier_expr*>(base), caller, env, eng, symbolizer, err);
				if (!root || err) { return {}; }
				value = root->deref();
				for (size_t i = steps.size(); i-- > 0;) {
					if (steps[i]->get_type() == node_type::member_expr) {
						auto* sm = static_cast<const member_expr*>(steps[i]);
						auto instance = value.is_object() && !value.is_null() ? value.get_class_instance() : nullptr;
						if (!instance || sm->member_id == UINT64_MAX) { return {}; }
						const script_value* f = instance->find_field_value(sm->member_id);
						if (!f) { return {}; }
						value = f->deref();
					} else {
						return {};   // an element receiver: the checker typed it or it runs
					}
				}
			}
			bool builtin = false;
			const std::optional<size_t> line = const_call_writes(value, call, m->member, caller, env, eng, symbolizer, builtin);
			if (!line) { return {}; }
			return raise(site.text ? *site.text : const_call_text(recv, "", m->member, builtin, *line));
		}
		return raise(site.text ? *site.text : std::string("Cannot assign to a const: it is read-only"));
	}

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_CONST_RULES_HPP__
