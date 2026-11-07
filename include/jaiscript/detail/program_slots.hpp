#pragma once

#ifndef __JAISCRIPT_DETAIL_PROGRAM_SLOTS_HPP__
#define __JAISCRIPT_DETAIL_PROGRAM_SLOTS_HPP__

#include "ast.hpp"
#include "body_walker.hpp"
#include "native_stack.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jai::detail {

	// Frame slots for a program's block-scoped declarations (a top-level loop's variable and the locals of its body, a top-level
	// block's locals), the slots a function body gets: the VM keeps them in the program frame's window, where its typed rows and cell
	// blocks take them, and the interpreter, which runs a program with no call frame, keeps reading them through the environment.
	// Program-scope declarations stay in the environment (a function reads a global at call time, a re-run, an import or the host
	// sees it there). A statement whose code could see a block-scoped name other than by its own reads keeps every name in the
	// environment: a lambda or a nested declaration (a function, a class, a namespace, an enum, an extern) that may capture it, an
	// include or an import that runs code in its scope, a parallel_for whose body is a function of its own, a try, a yield, a
	// structured binding or a reference declaration. Every read of a declared name must be a resolved read, or none is slotted.
	// Returns the program frame's slot count.
	class program_slot_pass {
	public:
		std::unordered_set<const ast_node*> slotted;   // the top-level declarations whose statements took slots
		std::unordered_set<uint64_t> names;            // the names they declare

		size_t run(const std::vector<declaration_ptr>& declarations) {
			size_t result = 0;
			for (const auto& d : declarations) {
				const statement* s = d.get();
				if (s->get_type() == node_type::statement_decl) { s = static_cast<const statement_decl*>(s)->statement.get(); }
				if (!s) continue;
				switch (s->get_type()) {
				case node_type::for_stmt: case node_type::while_stmt: case node_type::range_for_stmt: case node_type::block_stmt:
				case node_type::if_stmt: case node_type::switch_stmt:
					if (const size_t count = slot_statement(const_cast<statement*>(s)); count > 0) {
						result = std::max(result, count);
						slotted.insert(d.get());
						names.insert(declared_.begin(), declared_.end());
					}
					break;
				default:
					break;
				}
			}
			return result;
		}

	private:
		struct binding { uint64_t symbol; size_t slot; };
		std::vector<std::vector<binding>> scopes_;
		size_t next_slot_ = 0;
		bool ok_ = true;
		std::vector<identifier_expr*> reads_;
		std::vector<size_t*> decl_slots_;
		std::unordered_set<const identifier_expr*> resolved_;
		std::unordered_set<uint64_t> declared_;
		native_stack_mark stack_;

		size_t slot_statement(statement* s) {
			scopes_.clear();
			next_slot_ = 0;
			ok_ = true;
			reads_.clear();
			decl_slots_.clear();
			resolved_.clear();
			declared_.clear();
			scopes_.emplace_back();
			stmt(s);
			if (ok_) {
				// a read of a declared name the resolution did not see (a node kind it does not walk) keeps the statement unslotted
				body_walker walker;
				walker.on_identifier = [&](identifier_expr* id) { if (declared_.count(id->symbol_id) && !resolved_.count(id)) { ok_ = false; } };
				walker.stmt(statement_ptr(s, [](statement*) {}));
			}
			if (!ok_ || next_slot_ == 0) {
				for (identifier_expr* id : reads_) { id->slot_index = SIZE_MAX; }
				for (size_t* slot : decl_slots_) { *slot = SIZE_MAX; }
				return 0;
			}
			return next_slot_;
		}

		size_t declare(uint64_t symbol) {
			const size_t slot = next_slot_++;
			scopes_.back().push_back(binding{ symbol, slot });
			declared_.insert(symbol);
			return slot;
		}

		void read(identifier_expr* id) {
			resolved_.insert(id);
			if (id->global_qualified) return;
			for (size_t k = scopes_.size(); k-- > 0;) {
				const auto& sc = scopes_[k];
				for (size_t j = sc.size(); j-- > 0;) {
					if (sc[j].symbol == id->symbol_id) { id->slot_index = sc[j].slot; reads_.push_back(id); return; }
				}
			}
			id->slot_index = SIZE_MAX;
		}

		void block(const std::vector<declaration_ptr>& body) {
			scopes_.emplace_back();
			for (const auto& d : body) { stmt(d.get()); }
			scopes_.pop_back();
		}

		void stmt(statement* s) {
			if (!s || !ok_) return;
			if (stack_.low()) { ok_ = false; return; }   // unslotted is always correct
			switch (s->get_type()) {
			case node_type::expression_stmt: expr(static_cast<expression_stmt*>(s)->expression.get()); return;
			case node_type::expression_decl: expr(static_cast<expression_decl*>(s)->expression.get()); return;
			case node_type::statement_decl: stmt(static_cast<statement_decl*>(s)->statement.get()); return;
			case node_type::block_stmt: block(static_cast<block_stmt*>(s)->declarations); return;
			case node_type::if_stmt: {
				auto* i = static_cast<if_stmt*>(s);
				expr(i->condition.get());
				scopes_.emplace_back(); stmt(i->then_statement.get()); scopes_.pop_back();
				scopes_.emplace_back(); stmt(i->else_statement.get()); scopes_.pop_back();
				return;
			}
			case node_type::while_stmt: {
				auto* w = static_cast<while_stmt*>(s);
				expr(w->condition.get());
				scopes_.emplace_back(); stmt(w->body.get()); scopes_.pop_back();
				return;
			}
			case node_type::for_stmt: {
				auto* f = static_cast<for_stmt*>(s);
				scopes_.emplace_back();
				stmt(f->initializer.get());
				expr(f->condition.get());
				expr(f->update.get());
				scopes_.emplace_back(); stmt(f->body.get()); scopes_.pop_back();
				scopes_.pop_back();
				return;
			}
			case node_type::range_for_stmt: {
				auto* r = static_cast<range_for_stmt*>(s);
				if (r->is_reference || r->binds_map_entry || r->variable_name_id == UINT64_MAX) { ok_ = false; return; }
				expr(r->container.get());
				scopes_.emplace_back();
				r->variable_slot_index = declare(r->variable_name_id);
				decl_slots_.push_back(&r->variable_slot_index);
				scopes_.emplace_back(); stmt(r->body.get()); scopes_.pop_back();
				scopes_.pop_back();
				return;
			}
			case node_type::switch_stmt: {
				auto* sw = static_cast<switch_stmt*>(s);
				expr(sw->condition.get());
				for (const auto& c : sw->cases) {
					expr(c->value.get());
					scopes_.emplace_back();
					for (const auto& b : c->body) { stmt(b.get()); }
					scopes_.pop_back();
				}
				if (sw->default_case) {
					scopes_.emplace_back();
					for (const auto& b : sw->default_case->body) { stmt(b.get()); }
					scopes_.pop_back();
				}
				return;
			}
			case node_type::return_stmt: expr(static_cast<return_stmt*>(s)->value.get()); return;
			case node_type::break_stmt: case node_type::continue_stmt: case node_type::fallthrough_stmt: return;
			case node_type::variable_decl: {
				auto* v = static_cast<variable_decl*>(s);
				if (v->is_static || v->is_inline || v->name_id == UINT64_MAX || (v->type && v->type->is_reference())) { ok_ = false; return; }
				expr(v->initializer.get());   // the initializer reads the names outside the declaration
				v->slot_index = declare(v->name_id);
				decl_slots_.push_back(&v->slot_index);
				return;
			}
			default:
				ok_ = false;
				return;
			}
		}

		void expr(expression* e) {
			if (!e || !ok_) return;
			if (stack_.low()) { ok_ = false; return; }
			switch (e->get_type()) {
			case node_type::literal_expr: return;
			case node_type::identifier_expr: read(static_cast<identifier_expr*>(e)); return;
			case node_type::binary_expr: { auto* b = static_cast<binary_expr*>(e); expr(b->left.get()); expr(b->right.get()); return; }
			case node_type::unary_expr: expr(static_cast<unary_expr*>(e)->operand.get()); return;
			case node_type::call_expr: { auto* c = static_cast<call_expr*>(e); expr(c->callee.get()); for (const auto& a : c->arguments) { expr(a.get()); } return; }
			case node_type::member_expr: expr(static_cast<member_expr*>(e)->object.get()); return;
			case node_type::assignment_expr: { auto* a = static_cast<assignment_expr*>(e); expr(a->target.get()); expr(a->value.get()); return; }
			case node_type::ternary_expr: { auto* t = static_cast<ternary_expr*>(e); expr(t->condition.get()); expr(t->then_expression.get()); expr(t->else_expression.get()); return; }
			case node_type::array_literal_expr: for (const auto& el : static_cast<array_literal_expr*>(e)->elements) { expr(el.get()); } return;
			case node_type::map_literal_expr: for (const auto& kv : static_cast<map_literal_expr*>(e)->entries) { expr(kv.first.get()); expr(kv.second.get()); } return;
			case node_type::new_expr: for (const auto& a : static_cast<new_expr*>(e)->arguments) { expr(a.get()); } return;
			case node_type::throw_expr: expr(static_cast<throw_expr*>(e)->value.get()); return;
			case node_type::cast_expr: expr(static_cast<cast_expr*>(e)->operand.get()); return;
			default: ok_ = false; return;   // this/super, a lambda, a yield, an include or an import
			}
		}
	};

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_PROGRAM_SLOTS_HPP__
