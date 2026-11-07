#pragma once

#ifndef __JAISCRIPT_DETAIL_LVALUE_SHAPE_HPP__
#define __JAISCRIPT_DETAIL_LVALUE_SHAPE_HPP__

// The structural lvalue shapes a reference binds from the AST alone (detail/ref_lvalue.hpp resolves them); kept apart so
// the parse-time passes classify without the runtime kernel's headers.

#include <jaiscript/detail/ast.hpp>
#include <string>

namespace jai::detail {

	// Structural classifier for ref-bindable lvalue arguments: chains of plain member
	// steps (no ::, ?., ->), cast<T&> links and subscripts with int-literal or identifier indices over
	// an identifier or `this` base, at least one step deep. Bare identifiers keep the
	// existing slot/env metadata path; everything else keeps the non-lvalue error.
	inline bool is_ref_bindable_lvalue(const expression* e) {
		bool has_step = false;
		while (e) {
			const node_type t = e->get_type();
			if (t == node_type::member_expr) {
				const auto* m = static_cast<const member_expr*>(e);
				if (m->is_static || m->null_safe || m->is_arrow) {
					return false;
				}
				e = m->object.get();
				has_step = true;
			} else if (t == node_type::binary_expr) {
				const auto* b = static_cast<const binary_expr*>(e);
				if (b->op.type != token_type::left_bracket || !b->right) {
					return false;
				}
				const node_type it = b->right->get_type();
				if (it == node_type::literal_expr) {
					if (static_cast<const literal_expr*>(b->right.get())->value.raw_storage_index() != script_value::TYPEID_INT) {
						return false;
					}
				} else if (it != node_type::identifier_expr) {
					return false;
				}
				e = b->left.get();
				has_step = true;
			} else if (t == node_type::cast_expr && static_cast<const cast_expr*>(e)->by_reference &&
			           !static_cast<const cast_expr*>(e)->implicit_ref && has_step) {
				e = static_cast<const cast_expr*>(e)->operand.get();   // cast<T&>(x).f names x's object, as x.f does
			} else if (t == node_type::identifier_expr || t == node_type::this_expr) {
				return has_step;
			} else {
				return false;
			}
		}
		return false;
	}

	// A subscript names an element when its container is an lvalue: a name, member or element by shape, or any container
	// expression that evaluated to a reference (a call returning T&, cast<T&>, a conditional of lvalues)
	inline bool subscript_base_is_lvalue(bool lvalue_shape, const script_value& container_raw) {
		return lvalue_shape || container_raw.is_reference();
	}

	// Member-FINAL subset of the classifier: the ref-DECL gate (var&/auto& = obj.field
	// chains). Field reads evaluate to COPIES, so member-final initializers need the
	// kernel resolve; subscript-final initializers stay on the evaluation path - their
	// rhs-lvalue reads already mint owner-pinned references with full index generality,
	// and routing them through the kernel would change inner-reference-element aliasing
	// (see the element-share branch in resolve_ref_lvalue).
	inline bool is_member_final_ref_lvalue(const expression* e) {
		return e && e->get_type() == node_type::member_expr && is_ref_bindable_lvalue(e);
	}

	// The storage index a value of T's kind has (int, float, bool, string), 0xFF when every store runs T's table, 0 for no
	// declared type (var, auto, an unknown referent)
	inline uint8_t declared_ref_kind(const type_info* t) {
		if (!t || t->base_type == script_value_type::jai_any_type || t->base_type == script_value_type::jai_reference_type ||
		    t->type_name == "auto") {
			return 0;
		}
		if (t->is_byte_element()) { return 0xFF; }
		switch (t->base_type) {
			case script_value_type::jai_int_type: return static_cast<uint8_t>(script_value::TYPEID_INT);
			case script_value_type::jai_float_type: return static_cast<uint8_t>(script_value::TYPEID_FLOAT);
			case script_value_type::jai_bool_type: return static_cast<uint8_t>(script_value::TYPEID_BOOL);
			case script_value_type::jai_string_type: return static_cast<uint8_t>(script_value::TYPEID_STRING);
			default: return 0xFF;
		}
	}

	// The declared referent of a `T& p` parameter (the parser keeps T and the reference flag apart; a `var&` spells T as
	// var), null for a by-value one
	inline const type_info* parameter_declared_ref(const parameter& p) {
		if (!p.is_reference || !p.type) { return nullptr; }
		const type_info* t = p.type->base_type == script_value_type::jai_reference_type ? p.type->element_type().get() : p.type.get();
		return declared_ref_kind(t) ? t : nullptr;
	}

	inline bool is_prefix_incdec(const expression* e) {
		if (e->get_type() != node_type::unary_expr) { return false; }
		const auto* u = static_cast<const unary_expr*>(e);
		return !u->is_postfix && (u->op.type == token_type::plus_plus || u->op.type == token_type::minus_minus);
	}

	// What can name storage as an assignment target (C++ lvalues): a name, a member, an element, cast<T&>, a call (a T&
	// result, decided when it runs), a conditional of lvalues, an assignment and a prefix ++/--
	inline bool lvalue_capable(const expression* e) {
		if (!e) { return false; }
		switch (e->get_type()) {
			case node_type::identifier_expr:
			case node_type::member_expr:
			case node_type::call_expr:
				return true;
			case node_type::binary_expr:
				return static_cast<const binary_expr*>(e)->op.type == token_type::left_bracket;
			case node_type::cast_expr:
				return static_cast<const cast_expr*>(e)->by_reference;
			case node_type::ternary_expr: {
				const auto* t = static_cast<const ternary_expr*>(e);
				return lvalue_capable(t->then_expression.get()) && lvalue_capable(t->else_expression.get());
			}
			case node_type::assignment_expr: {
				const auto* a = static_cast<const assignment_expr*>(e);
				return !a->postfix_old_result && lvalue_capable(a->target.get());
			}
			default:
				return is_prefix_incdec(e) && static_cast<const unary_expr*>(e)->operand->get_type() == node_type::identifier_expr;
		}
	}

	// What a reference binding can name without running code that may return by value: a variable, a member, an element,
	// cast<T&>, a conditional of those
	inline bool names_storage(const expression* e) {
		if (!e) { return false; }
		switch (e->get_type()) {
			case node_type::identifier_expr:
				return true;
			case node_type::member_expr: {
				const auto* m = static_cast<const member_expr*>(e);
				return !m->null_safe && !m->is_arrow;
			}
			case node_type::binary_expr:
				return static_cast<const binary_expr*>(e)->op.type == token_type::left_bracket;
			case node_type::cast_expr:
				return static_cast<const cast_expr*>(e)->by_reference;
			case node_type::ternary_expr: {
				const auto* t = static_cast<const ternary_expr*>(e);
				return names_storage(t->then_expression.get()) && names_storage(t->else_expression.get());
			}
			default:
				return false;
		}
	}

	// A conditional whose arms both name storage is the lvalue it picks (C++): a reference binding names the chosen arm
	inline bool conditional_names_storage(const expression* e) {
		return e && e->get_type() == node_type::ternary_expr && names_storage(e);
	}

	// An assignment target that evaluates to the reference it stores through (after the parser's rewrite): cast<T&>(e), a
	// call, a conditional whose arms are (implicit casts), an assignment that yields its destination
	inline bool reference_target(const expression* t) {
		switch (t->get_type()) {
			case node_type::cast_expr:
				return static_cast<const cast_expr*>(t)->by_reference;
			case node_type::call_expr:
				return true;
			case node_type::ternary_expr: {
				const auto* c = static_cast<const ternary_expr*>(t);
				return reference_target(c->then_expression.get()) && reference_target(c->else_expression.get());
			}
			case node_type::assignment_expr:
				return static_cast<const assignment_expr*>(t)->yields_reference;
			default:
				return false;
		}
	}

	inline std::string target_spelling(const expression* e) {
		if (!e) { return "..."; }
		switch (e->get_type()) {
			case node_type::identifier_expr: {
				const auto* id = static_cast<const identifier_expr*>(e);
				return (id->global_qualified ? "::" : "") + std::string(id->name);
			}
			case node_type::this_expr:
				return "this";
			case node_type::member_expr: {
				const auto* m = static_cast<const member_expr*>(e);
				return target_spelling(m->object.get()) + (m->is_static ? "::" : m->null_safe ? "?." : m->is_arrow ? "->" : ".") + std::string(m->member);
			}
			case node_type::binary_expr: {
				const auto* b = static_cast<const binary_expr*>(e);
				if (b->op.type != token_type::left_bracket) { return "..."; }
				const node_type k = b->right->get_type();
				return target_spelling(b->left.get()) + "[" + (k == node_type::identifier_expr || k == node_type::literal_expr ? target_spelling(b->right.get()) : "...") + "]";
			}
			case node_type::literal_expr: {
				const script_value& v = static_cast<const literal_expr*>(e)->value;
				return v.raw_storage_index() == script_value::TYPEID_INT ? std::to_string(v.unchecked_as_int()) : "...";
			}
			case node_type::call_expr:
				return target_spelling(static_cast<const call_expr*>(e)->callee.get()) + "(...)";
			default:
				return "...";
		}
	}

	// A subscript store whose operator[] (the class's or the global one) returned a value, not a T&: raised once the call ran
	inline constexpr std::string_view by_value_subscript_store_text = "Cannot assign to a call result: 'operator[]' returns by value";

	// The text an assignment to an expression that names no storage raises (docs/site/reference/index.html "Run-time errors"); the
	// checker reports it at load with a lowercase first letter
	inline std::string non_lvalue_target_text(const expression* t) {
		switch (t->get_type()) {
			case node_type::literal_expr:
				return "Cannot assign to a literal";
			case node_type::cast_expr: {
				const auto* c = static_cast<const cast_expr*>(t);
				if (c->implicit_ref) { return non_lvalue_target_text(c->operand.get()); }
				const std::string type = c->target ? c->target->canonical_name() : std::string("T");
				const std::string operand = target_spelling(c->operand.get());
				std::string text = "Cannot assign to the result of cast<" + type + ">(" + operand + "): it is a temporary value";
				const node_type k = c->operand->get_type();
				if (operand.find("...") == std::string::npos && (k == node_type::identifier_expr || k == node_type::member_expr || k == node_type::binary_expr)) {
					text += " (cast<" + type + "&>(" + operand + ") names the " + (k == node_type::identifier_expr ? "variable" : "storage") + ")";
				}
				return text;
			}
			case node_type::call_expr: {
				const std::string callee = target_spelling(static_cast<const call_expr*>(t)->callee.get());
				return "Cannot assign to a call result: '" + callee + "' returns by value";
			}
			case node_type::unary_expr: {
				const auto* u = static_cast<const unary_expr*>(t);
				if (u->is_postfix) { return "Cannot assign to " + target_spelling(u->operand.get()) + std::string(u->op.lexeme) + ": it yields the old value"; }
				return "Cannot assign to the result of '" + std::string(u->op.lexeme) + "': it is a temporary value";
			}
			case node_type::assignment_expr: {
				const auto* a = static_cast<const assignment_expr*>(t);
				if (a->postfix_old_result) {
					return "Cannot assign to " + target_spelling(a->target.get()) + (a->op.type == token_type::plus_equal ? "++" : "--") + ": it yields the old value";
				}
				return lvalue_capable(a->target.get()) ? "Cannot assign to this expression: it names no storage" : non_lvalue_target_text(a->target.get());
			}
			case node_type::binary_expr:
				return "Cannot assign to the result of '" + std::string(static_cast<const binary_expr*>(t)->op.lexeme) + "': it is a temporary value";
			case node_type::ternary_expr: {
				const auto* c = static_cast<const ternary_expr*>(t);
				const expression* arm = !lvalue_capable(c->then_expression.get()) ? c->then_expression.get() : c->else_expression.get();
				return "Cannot assign to a conditional: its arm '" + target_spelling(arm) + "' names no storage";
			}
			case node_type::this_expr:
				return "Cannot assign to 'this'";
			case node_type::new_expr:
				return "Cannot assign to a new object: it is a temporary value";
			case node_type::lambda_expr:
				return "Cannot assign to a lambda";
			case node_type::array_literal_expr:
				return "Cannot assign to an array literal";
			case node_type::map_literal_expr:
				return "Cannot assign to a map literal";
			default:
				return "Cannot assign to this expression: it names no storage";
		}
	}

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_LVALUE_SHAPE_HPP__
