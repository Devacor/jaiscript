#pragma once

#ifndef __JAISCRIPT_DETAIL_BODY_WALKER_HPP__
#define __JAISCRIPT_DETAIL_BODY_WALKER_HPP__

#include "ast.hpp"
#include "string_symbolizer.hpp"
#include "native_stack.hpp"
#include <functional>

namespace jai::detail {

	// Shared body walker used by lambda/coroutine capture analysis (vm compiler and
	// interpreter). Never descends into nested lambda_expr or function_decl bodies.
	struct body_walker {
		std::function<void(identifier_expr*)> on_identifier;
		std::function<void(uint64_t)> on_declared;
		string_symbolizer* symbolizer;
		std::function<void()> on_instance;
		native_stack_mark stack;

		void expr(const expression_ptr& e) const {
			if (!e) return;
			stack.guard(e->location);
			switch (e->get_type()) {
			case node_type::this_expr:
			case node_type::super_expr:
				if (on_instance) { on_instance(); }
				return;
			case node_type::identifier_expr:
				if (on_identifier) on_identifier(static_cast<identifier_expr*>(e.get()));
				return;
			case node_type::binary_expr: {
				auto* b = static_cast<binary_expr*>(e.get());
				expr(b->left);
				expr(b->right);
				return;
			}
			case node_type::unary_expr:
				expr(static_cast<unary_expr*>(e.get())->operand);
				return;
			case node_type::call_expr: {
				auto* c = static_cast<call_expr*>(e.get());
				expr(c->callee);
				for (const auto& a : c->arguments) expr(a);
				return;
			}
			case node_type::member_expr:
				expr(static_cast<member_expr*>(e.get())->object);
				return;
			case node_type::assignment_expr: {
				auto* a = static_cast<assignment_expr*>(e.get());
				expr(a->target);
				expr(a->value);
				return;
			}
			case node_type::ternary_expr: {
				auto* t = static_cast<ternary_expr*>(e.get());
				expr(t->condition);
				expr(t->then_expression);
				expr(t->else_expression);
				return;
			}
			case node_type::array_literal_expr:
				for (const auto& el : static_cast<array_literal_expr*>(e.get())->elements) expr(el);
				return;
			case node_type::map_literal_expr:
				for (const auto& kv : static_cast<map_literal_expr*>(e.get())->entries) {
					expr(kv.first);
					expr(kv.second);
				}
				return;
			case node_type::new_expr:
				for (const auto& a : static_cast<new_expr*>(e.get())->arguments) expr(a);
				return;
			case node_type::include_expr:
				expr(static_cast<include_expr*>(e.get())->path_expr);
				return;
			case node_type::import_expr:
				for (const auto& a : static_cast<import_expr*>(e.get())->path_exprs) expr(a);
				return;
			case node_type::throw_expr:
				expr(static_cast<throw_expr*>(e.get())->value);
				return;
			case node_type::yield_expr:
				expr(static_cast<yield_expr*>(e.get())->value);
				return;
			case node_type::cast_expr:
				expr(static_cast<cast_expr*>(e.get())->operand);
				return;
			case node_type::lambda_expr:
				// a nested lambda's body is its own; its init-captures evaluate here
				for (const auto& cap : static_cast<lambda_expr*>(e.get())->captures) { expr(cap.initializer); }
				return;
			default:
				return;
			}
		}

		void stmt(const statement_ptr& s) const {
			if (!s) return;
			stack.guard(s->location);
			switch (s->get_type()) {
			case node_type::function_decl: {
				auto* value = static_cast<function_decl*>(s.get());
				if (on_declared) { on_declared(value->name_id); }
				return;
			}
			case node_type::enum_decl: {
				auto* value = static_cast<enum_decl*>(s.get());
				if (on_declared) { on_declared(value->name_id); }
				return;
			}
			case node_type::expression_stmt:
				expr(static_cast<expression_stmt*>(s.get())->expression);
				return;
			case node_type::expression_decl:
				expr(static_cast<expression_decl*>(s.get())->expression);
				return;
			case node_type::statement_decl:
				stmt(static_cast<statement_decl*>(s.get())->statement);
				return;
			case node_type::block_stmt:
				for (const auto& d : static_cast<block_stmt*>(s.get())->declarations) stmt(d);
				return;
			case node_type::if_stmt: {
				auto* is = static_cast<if_stmt*>(s.get());
				expr(is->condition);
				stmt(is->then_statement);
				stmt(is->else_statement);
				return;
			}
			case node_type::while_stmt: {
				auto* ws = static_cast<while_stmt*>(s.get());
				expr(ws->condition);
				stmt(ws->body);
				return;
			}
			case node_type::for_stmt: {
				auto* fs = static_cast<for_stmt*>(s.get());
				stmt(fs->initializer);
				expr(fs->condition);
				expr(fs->update);
				stmt(fs->body);
				return;
			}
			case node_type::range_for_stmt: {
				auto* rf = static_cast<range_for_stmt*>(s.get());
				if (on_declared && rf->variable_name_id != UINT64_MAX) on_declared(rf->variable_name_id);
				expr(rf->container);
				stmt(rf->body);
				return;
			}
			case node_type::parallel_for_stmt: {
				// Body names an enclosing lambda must still capture (workers resolve
				// them through the provisioned env); the loop variable is body-local.
				auto* pf = static_cast<parallel_for_stmt*>(s.get());
				if (on_declared && pf->loop_param.symbol_id != UINT64_MAX) on_declared(pf->loop_param.symbol_id);
				for (const auto& extra : pf->extra_params) { if (on_declared && extra.symbol_id != UINT64_MAX) on_declared(extra.symbol_id); }
				expr(pf->container);
				stmt(pf->body);
				return;
			}
			case node_type::return_stmt:
				expr(static_cast<return_stmt*>(s.get())->value);
				return;
			case node_type::switch_stmt: {
				auto* sw = static_cast<switch_stmt*>(s.get());
				expr(sw->condition);
				for (const auto& c : sw->cases) {
					expr(c->value);
					for (const auto& st : c->body) stmt(st);
				}
				if (sw->default_case) {
					for (const auto& st : sw->default_case->body) stmt(st);
				}
				return;
			}
			case node_type::try_stmt: {
				auto* tr = static_cast<try_stmt*>(s.get());
				stmt(tr->try_block);
				if (on_declared && tr->catch_var_id != UINT64_MAX) on_declared(tr->catch_var_id);
				stmt(tr->catch_block);
				return;
			}
			case node_type::variable_decl: {
				auto* vd = static_cast<variable_decl*>(s.get());
				if (on_declared) {
					uint64_t id = vd->name_id != UINT64_MAX ? vd->name_id : symbolizer->intern(std::string(vd->name));
					on_declared(id);
				}
				expr(vd->initializer);
				return;
			}
			case node_type::destructuring_decl: {
				auto* dd = static_cast<destructuring_decl*>(s.get());
				if (on_declared) {
					for (const auto& [name, id] : dd->names) on_declared(id);
				}
				expr(dd->initializer);
				return;
			}
			default:
				return;
			}
		}
	};

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_BODY_WALKER_HPP__
