#pragma once

#ifndef __JAISCRIPT_DETAIL_CONSTANT_ARGUMENTS_HPP__
#define __JAISCRIPT_DETAIL_CONSTANT_ARGUMENTS_HPP__

#include "ast.hpp"
#include "expr_children.hpp"
#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jai::detail {

	// A host callable's argument that only a constant of the program may fill (engine::constant_argument): the refusal text,
	// and every text the loaded units spell for it
	struct constant_argument_rule {
		size_t index = 0;
		std::string refusal;
		std::unordered_set<std::string> texts;
		std::unordered_set<std::string> pending_names;   // consts an argument named before any unit declared them
	};

	// The texts each name's const declarations were initialized with, in any scope of any unit (a name declared const twice
	// holds both: every text is still the program's own)
	using constant_table = std::unordered_map<std::string, std::vector<std::string>>;

	inline constexpr size_t k_constant_text_limit = 256;

	// The texts a constant form can hold: a string literal; an identifier naming a const of the table; a ternary whose arms
	// are constant forms (the condition is free); a + of constant forms; an array literal of them (each element a text).
	// false for anything else, or for a form that spells more than k_constant_text_limit texts.
	inline bool constant_texts(const expression* e, const constant_table& consts, std::vector<std::string>& out, int depth = 0) {
		if (!e || depth > 32) { return false; }
		switch (e->get_type()) {
			case node_type::literal_expr: {
				const std::string* text = static_cast<const literal_expr*>(e)->value.text_bytes();
				if (!text) { return false; }
				out.push_back(*text);
				return true;
			}
			case node_type::identifier_expr: {
				const auto found = consts.find(std::string(static_cast<const identifier_expr*>(e)->name));
				if (found == consts.end() || found->second.empty()) { return false; }
				out.insert(out.end(), found->second.begin(), found->second.end());
				return out.size() <= k_constant_text_limit;
			}
			case node_type::ternary_expr: {
				auto* t = static_cast<const ternary_expr*>(e);
				return constant_texts(t->then_expression.get(), consts, out, depth + 1) && constant_texts(t->else_expression.get(), consts, out, depth + 1) &&
				       out.size() <= k_constant_text_limit;
			}
			case node_type::binary_expr: {
				auto* b = static_cast<const binary_expr*>(e);
				if (b->op.type != token_type::plus) { return false; }
				std::vector<std::string> left, right;
				if (!constant_texts(b->left.get(), consts, left, depth + 1) || !constant_texts(b->right.get(), consts, right, depth + 1)) { return false; }
				if (left.size() * right.size() + out.size() > k_constant_text_limit) { return false; }
				for (const auto& l : left) {
					for (const auto& r : right) { out.push_back(l + r); }
				}
				return true;
			}
			case node_type::array_literal_expr: {
				for (const auto& element : static_cast<const array_literal_expr*>(e)->elements) {
					if (!constant_texts(element.get(), consts, out, depth + 1)) { return false; }
				}
				return out.size() <= k_constant_text_limit;
			}
			default:
				return false;
		}
	}

	// Every statement and expression of a unit, lambda and function bodies, class members and namespaces included
	template <typename OnStatement, typename OnExpression>
	struct unit_walker {
		OnStatement on_statement;
		OnExpression on_expression;

		void stmt(const statement* node) {
			if (!node) { return; }
			on_statement(node);
			switch (node->get_type()) {
				case node_type::variable_decl: expr(static_cast<const variable_decl*>(node)->initializer.get()); break;
				case node_type::function_decl: {
					auto* f = static_cast<const function_decl*>(node);
					for (const auto& init : f->initializers) { for (const auto& a : init.arguments) { expr(a.get()); } }
					stmt(f->body.get());
					break;
				}
				case node_type::class_decl:
					for (const auto& m : static_cast<const class_decl*>(node)->members) { stmt(m.declaration.get()); }
					break;
				case node_type::namespace_decl:
					for (const auto& d : static_cast<const namespace_decl*>(node)->declarations) { stmt(d.get()); }
					break;
				case node_type::destructuring_decl: expr(static_cast<const destructuring_decl*>(node)->initializer.get()); break;
				case node_type::expression_stmt: expr(static_cast<const expression_stmt*>(node)->expression.get()); break;
				case node_type::expression_decl: expr(static_cast<const expression_decl*>(node)->expression.get()); break;
				case node_type::statement_decl: stmt(static_cast<const statement_decl*>(node)->statement.get()); break;
				case node_type::block_stmt:
					for (const auto& d : static_cast<const block_stmt*>(node)->declarations) { stmt(d.get()); }
					break;
				case node_type::if_stmt: {
					auto* i = static_cast<const if_stmt*>(node);
					expr(i->condition.get());
					stmt(i->then_statement.get());
					stmt(i->else_statement.get());
					break;
				}
				case node_type::while_stmt: expr(static_cast<const while_stmt*>(node)->condition.get()); stmt(static_cast<const while_stmt*>(node)->body.get()); break;
				case node_type::for_stmt: {
					auto* f = static_cast<const for_stmt*>(node);
					stmt(f->initializer.get());
					expr(f->condition.get());
					expr(f->update.get());
					stmt(f->body.get());
					break;
				}
				case node_type::range_for_stmt: expr(static_cast<const range_for_stmt*>(node)->container.get()); stmt(static_cast<const range_for_stmt*>(node)->body.get()); break;
				case node_type::parallel_for_stmt: {
					auto* p = static_cast<const parallel_for_stmt*>(node);
					expr(p->container.get());
					for (const auto& c : p->extra_containers) { expr(c.get()); }
					stmt(p->body.get());
					break;
				}
				case node_type::try_stmt: stmt(static_cast<const try_stmt*>(node)->try_block.get()); stmt(static_cast<const try_stmt*>(node)->catch_block.get()); break;
				case node_type::switch_stmt: {
					auto* sw = static_cast<const switch_stmt*>(node);
					expr(sw->condition.get());
					for (const auto& cs : sw->cases) {
						expr(cs->value.get());
						for (const auto& st : cs->body) { stmt(st.get()); }
					}
					if (sw->default_case) { for (const auto& st : sw->default_case->body) { stmt(st.get()); } }
					break;
				}
				case node_type::return_stmt: expr(static_cast<const return_stmt*>(node)->value.get()); break;
				default: break;
			}
		}

		void expr(const expression* e) {
			if (!e) { return; }
			on_expression(e);
			if (e->get_type() == node_type::lambda_expr) { stmt(static_cast<const lambda_expr*>(e)->body.get()); }
			for_each_child_expression(e, [this](const expression* child) { expr(child); });
		}
	};

	template <typename OnStatement, typename OnExpression>
	void walk_unit(const std::vector<declaration_ptr>& decls, OnStatement&& on_statement, OnExpression&& on_expression) {
		unit_walker<std::decay_t<OnStatement>, std::decay_t<OnExpression>> walker{on_statement, on_expression};
		for (const auto& d : decls) { walker.stmt(d.get()); }
	}

	// The callable a call names, as a constant-argument rule keys it: "name" for a free call, ".member" for a method call
	// (a receiver's class is not known here; every method of the name gathers, and each text is still the program's own)
	inline std::string constant_call_key(const call_expr* c) {
		if (!c->callee) { return {}; }
		if (c->callee->get_type() == node_type::identifier_expr) { return std::string(static_cast<const identifier_expr*>(c->callee.get())->name); }
		if (c->callee->get_type() == node_type::member_expr) {
			auto* m = static_cast<const member_expr*>(c->callee.get());
			if (!m->is_static) { return "." + std::string(m->member); }
		}
		return {};
	}

	// The names a bare constant-name argument spells (a ternary's arms followed), for a const declared by a later unit
	inline void pending_constant_names(const expression* e, std::unordered_set<std::string>& out, int depth = 0) {
		if (!e || depth > 32) { return; }
		if (e->get_type() == node_type::identifier_expr) { out.emplace(static_cast<const identifier_expr*>(e)->name); }
		if (e->get_type() == node_type::ternary_expr) {
			pending_constant_names(static_cast<const ternary_expr*>(e)->then_expression.get(), out, depth + 1);
			pending_constant_names(static_cast<const ternary_expr*>(e)->else_expression.get(), out, depth + 1);
		}
	}

	using constant_rule_map = std::unordered_map<std::string, std::vector<constant_argument_rule*>>;

	// A unit's const declarations into the table (and into every rule an argument already named them for)
	inline void gather_constants(const std::vector<declaration_ptr>& decls, constant_table& consts, const constant_rule_map& rules) {
		walk_unit(decls, [&](const statement* s) {
			if (s->get_type() != node_type::variable_decl) { return; }
			auto* v = static_cast<const variable_decl*>(s);
			if (!v->is_const || !v->initializer) { return; }
			std::vector<std::string> texts;
			if (!constant_texts(v->initializer.get(), consts, texts)) { return; }
			const std::string name(v->name);
			auto& slot = consts[name];
			for (auto& text : texts) {
				if (std::find(slot.begin(), slot.end(), text) == slot.end()) { slot.push_back(text); }
			}
			for (const auto& [key, list] : rules) {
				for (constant_argument_rule* rule : list) {
					if (rule->pending_names.count(name)) { rule->texts.insert(texts.begin(), texts.end()); }
				}
			}
		}, [](const expression*) {});
	}

	// The constant texts a unit's calls pass to every rule's argument (rules keyed "name" or ".member")
	inline void gather_constant_arguments(const std::vector<declaration_ptr>& decls, const constant_table& consts, const constant_rule_map& rules) {
		if (rules.empty()) { return; }
		walk_unit(decls, [](const statement*) {}, [&](const expression* e) {
			if (e->get_type() != node_type::call_expr) { return; }
			auto* c = static_cast<const call_expr*>(e);
			const auto found = rules.find(constant_call_key(c));
			if (found == rules.end()) { return; }
			for (constant_argument_rule* rule : found->second) {
				if (rule->index >= c->arguments.size()) { continue; }
				const expression* argument = c->arguments[rule->index].get();
				std::vector<std::string> texts;
				if (constant_texts(argument, consts, texts)) { rule->texts.insert(texts.begin(), texts.end()); }
				pending_constant_names(argument, rule->pending_names);
			}
		});
	}

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_CONSTANT_ARGUMENTS_HPP__
