#pragma once

#ifndef __JAISCRIPT_DETAIL_EXPR_CHILDREN_HPP__
#define __JAISCRIPT_DETAIL_EXPR_CHILDREN_HPP__

#include "ast.hpp"

namespace jai::detail {

	// Every expression an expression evaluates in its own frame, in evaluation order: a lambda contributes its init-capture
	// initializers (its body is its own frame). One list for every walker that must not skip a node kind.
	template <typename F>
	void for_each_child_expression(const expression* e, F&& f) {
		static_assert(static_cast<int>(node_type::import_expr) == 18 && static_cast<int>(node_type::expression_stmt) == 20,
		              "a new expression kind lists its children here");
		if (!e) return;
		auto visit = [&](const expression_ptr& c) { if (c) f(c.get()); };
		switch (e->get_type()) {
			case node_type::literal_expr: case node_type::identifier_expr: case node_type::this_expr: case node_type::super_expr: return;
			case node_type::binary_expr: { auto* b = static_cast<const binary_expr*>(e); visit(b->left); visit(b->right); return; }
			case node_type::unary_expr: visit(static_cast<const unary_expr*>(e)->operand); return;
			case node_type::assignment_expr: { auto* a = static_cast<const assignment_expr*>(e); visit(a->value); visit(a->target); return; }
			case node_type::call_expr: {
				auto* c = static_cast<const call_expr*>(e);
				visit(c->callee);
				for (const auto& a : c->arguments) visit(a);
				return;
			}
			case node_type::member_expr: visit(static_cast<const member_expr*>(e)->object); return;
			case node_type::lambda_expr: for (const auto& cap : static_cast<const lambda_expr*>(e)->captures) visit(cap.initializer); return;
			case node_type::new_expr: for (const auto& a : static_cast<const new_expr*>(e)->arguments) visit(a); return;
			case node_type::ternary_expr: {
				auto* t = static_cast<const ternary_expr*>(e);
				visit(t->condition); visit(t->then_expression); visit(t->else_expression);
				return;
			}
			case node_type::array_literal_expr: for (const auto& x : static_cast<const array_literal_expr*>(e)->elements) visit(x); return;
			case node_type::map_literal_expr: for (const auto& [k, v] : static_cast<const map_literal_expr*>(e)->entries) { visit(k); visit(v); } return;
			case node_type::throw_expr: visit(static_cast<const throw_expr*>(e)->value); return;
			case node_type::yield_expr: visit(static_cast<const yield_expr*>(e)->value); return;
			case node_type::include_expr: visit(static_cast<const include_expr*>(e)->path_expr); return;
			case node_type::cast_expr: visit(static_cast<const cast_expr*>(e)->operand); return;
			case node_type::import_expr: for (const auto& a : static_cast<const import_expr*>(e)->path_exprs) visit(a); return;
			default: return;
		}
	}

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_EXPR_CHILDREN_HPP__
