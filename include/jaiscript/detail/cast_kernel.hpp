#pragma once

#ifndef __JAISCRIPT_DETAIL_CAST_KERNEL_HPP__
#define __JAISCRIPT_DETAIL_CAST_KERNEL_HPP__

// cast<T>(e) / cast<T&>(e) / cast<shared_ptr<T>>(e) semantics shared VERBATIM by the vm
// and the interpreter (parity by construction): operand-shape classification, the
// class-derivation verdict and its error text, the class-target value/handle/reference
// outcomes, the char rules and the text rule (the primitive pairs the assignment table refuses).
// Other primitive and container targets convert through the typed-slot table
// (detail::convert_for_typed_slot via enforce_type_compatibility) and only borrow the error text here.

#include <jaiscript/detail/ref_lvalue.hpp>
#include <jaiscript/detail/container_enforce.hpp>
#include <string>
#include <string_view>

namespace jai::detail {

	// How a cast evaluates its operand: value = ordinary evaluation (the reference form
	// aliases an evaluated reference or pins a temporary), identifier = bind the named
	// storage by reference (ref-decl identifier arm), lvalue_chain = member-final chain
	// resolved through resolve_ref_lvalue (ref-decl member-final arm). Subscript-final
	// operands stay on the value path: their reads already mint owner-pinned references.
	enum class cast_operand_mode : uint8_t { value = 0, identifier = 1, lvalue_chain = 2 };

	inline cast_operand_mode cast_operand_mode_of(const cast_expr* c) {
		if (!c->by_reference || !c->operand || c->binding_ref) {   // a binding arm reads its value, then looks for its storage
			return cast_operand_mode::value;
		}
		if (c->operand->get_type() == node_type::identifier_expr) {
			return cast_operand_mode::identifier;
		}
		if (is_member_final_ref_lvalue(c->operand.get())) {
			return cast_operand_mode::lvalue_chain;
		}
		return cast_operand_mode::value;
	}

	// Copy-on-cast gate for the value form: identifier/member/subscript operands are
	// existing storage (a typed declaration clones those), everything else is a temporary.
	inline bool cast_lvalue_source(const expression* e) {
		if (!e) {
			return false;
		}
		const node_type t = e->get_type();
		if (t == node_type::identifier_expr || t == node_type::member_expr) {
			return true;
		}
		return t == node_type::binary_expr && static_cast<const binary_expr*>(e)->op.type == token_type::left_bracket;
	}

	inline constexpr std::string_view cast_non_lvalue_text =
		"Cannot take reference of non-lvalue expression in cast<T&> (computed properties, "
		"C++-backed members and non-object receivers are not ref-bindable)";

	inline bool cast_target_is_class(const type_info& t) {
		return t.base_type == script_value_type::jai_object_type ||
		       t.base_type == script_value_type::jai_shared_ptr_type;
	}

	// The class a class target names: T itself, or shared_ptr<T>'s pointee
	inline const type_info* cast_target_class(const type_info& t) {
		if (t.base_type == script_value_type::jai_shared_ptr_type) {
			return t.element_type().get();
		}
		return &t;
	}

	// Class target on a non-null operand: an object or handle whose dynamic class is T or
	// derives from T (exact name first, registry parent walk otherwise)
	inline bool cast_class_admits(const script_value& seen, const type_info& target, engine* eng) {
		const type_info* cls = cast_target_class(target);
		if (!cls) {
			return false;
		}
		const class_definition* def = cast_dynamic_class(seen);
		if (!def) {
			return false;
		}
		return def->get_name() == cls->type_name || class_derives_from(eng, def->get_type_id(), cls->id);
	}

	// Shared reference admission is in ref_lvalue.hpp; casts, parameters, loops and returns use it.

	// A typed range-for variable's element: by value it takes the element as `T x = element;` would, by
	// reference it binds an element that holds a T (a class: T or a subclass). auto and var bind the element as it is
	inline bool range_for_declared(const type_info* declared) {
		return declared && declared->base_type != script_value_type::jai_any_type && declared->base_type != script_value_type::jai_null_type;
	}
	inline checked_result<script_value> range_for_bind(script_value element, const type_info_ptr& declared, bool by_ref, std::string_view name, engine* eng) {
		if (!declared) { return element; }
		if (declared->base_type == script_value_type::jai_any_type) {   // var: takes the element as var x = element; would
			// a handle keeps its own type, so a copy of the loop variable shares the object as a copy of the handle does
			const type_info_ptr& tag = element.get_type_info();
			const bool handle = tag && (tag->base_type == script_value_type::jai_shared_ptr_type || tag->base_type == script_value_type::jai_weak_ptr_type);
			if (!by_ref && !handle) { var_stamp(element, declared, eng); }
			return element;
		}
		if (!range_for_declared(declared.get())) { return element; }
		const slot_site site{slot_kind::declaration, name, {}};
		if (by_ref) {
			if (!reference_binding_admits(declared.get(), element, eng)) {
				return typed_slot_rejection(element.deref(), *declared, site, eng);
			}
			return element;
		}
		auto stored = typed_store_convert(element, declared, eng, [&]() { return site; });
		if (!stored) { return stored.error_value(); }
		script_value v = std::move(stored.value());
		v.set_type_info(declared);
		return v;
	}

	// Value and shared_ptr forms with a CLASS target: null passes tagged; an admitted object is
	// tagged - the value form copies like a typed declaration (existing storage clones, a handle
	// deep-copies into a value, a temporary moves), the shared_ptr form shares the instance.
	inline checked_result<script_value> cast_class_value(script_value seen, const cast_expr& c, bool lvalue_source,
	                                                     engine* eng, string_symbolizer* sym) {
		if (seen.is_null()) {
			script_value out(std::monostate{}, eng);
			out.set_type_info(c.target);
			return out;
		}
		if (!cast_class_admits(seen, *c.target, eng)) {
			return cast_error(seen, c, sym);
		}
		if (c.target->base_type != script_value_type::jai_shared_ptr_type) {
			const type_info_ptr tag = seen.get_type_info();
			if (tag && tag->base_type == script_value_type::jai_shared_ptr_type) {
				script_value copy = seen.clone();
				seen = std::move(copy);
			} else if (lvalue_source) {
				script_value copy = clone_for_assignment(seen);
				seen = std::move(copy);
			}
		}
		seen.set_type_info(c.target);
		return seen;
	}

	// Char rules exist in the cast alone (`char c = 65` stays a type mismatch): a char casts
	// to int/float as its promoted code point (0..255) and to string as one text unit; an
	// int casts to char when it lies in 0..255, a string when it is exactly one character.
	inline bool cast_char_rule_applies(const script_value& seen, const type_info& target) {
		const script_value_type src = seen.type();
		const script_value_type dst = target.base_type;
		if (src == script_value_type::jai_char_type) {
			return dst == script_value_type::jai_int_type || dst == script_value_type::jai_float_type ||
			       dst == script_value_type::jai_string_type;
		}
		return dst == script_value_type::jai_char_type &&
		       (src == script_value_type::jai_int_type || src == script_value_type::jai_string_type);
	}

	inline checked_result<script_value> cast_char_value(const script_value& seen, const cast_expr& c, engine* eng,
	                                                    string_symbolizer* sym) {
		const script_value_type dst = c.target->base_type;
		if (seen.type() == script_value_type::jai_char_type) {
			const script_char ch = seen.unchecked_as_char();
			if (dst == script_value_type::jai_string_type) {
				return script_value(script_string(1, ch), eng);
			}
			const script_int code = static_cast<script_int>(static_cast<unsigned char>(ch));
			if (dst == script_value_type::jai_float_type) {
				return script_value(static_cast<script_float>(code), eng);
			}
			return script_value(code, eng);
		}
		if (seen.type() == script_value_type::jai_int_type) {
			const script_int code = seen.unchecked_as_int();
			if (code < 0 || code > 255) {
				return cast_error(seen, c, sym);
			}
			return script_value(static_cast<script_char>(static_cast<unsigned char>(code)), eng);
		}
		const script_string& text = seen.unchecked_as_string();
		if (text.size() != 1) {
			return cast_error(seen, c, sym);
		}
		return script_value(static_cast<script_char>(text[0]), eng);
	}

	// cast<string>(e) and cast<utf8_string>(e) are the explicit stringify: e's display text, as to_string(e) gives it (a string slot
	// takes only text, or an object's own to_string()); text itself converts through the table
	inline bool cast_text_rule_applies(const script_value& seen, const type_info& target) {
		const script_value_type src = seen.type();
		return (target.base_type == script_value_type::jai_string_type || target.base_type == script_value_type::jai_utf8_string_type) &&
		       src != script_value_type::jai_string_type && src != script_value_type::jai_utf8_string_type;
	}

	// a to_string() that throws leaves its throw in flight and the text empty: the caller checks its unwinding
	inline checked_result<script_value> cast_text_value(const script_value& seen, const type_info& target, engine* eng) {
		auto text = eng->text_of(seen);
		if (!text) { return text.error_value(); }
		return target.base_type == script_value_type::jai_string_type ? script_value(std::move(text.value()), eng)
		                                                              : script_value::make_utf8(std::move(text.value()), eng);
	}

	// Reference form: `ref` is the bound reference (or a pinned temporary); the verdict is on
	// its referent. Class targets take the derivation rule, every other target requires the
	// referent's base type (a reference never converts). The reference itself is the result -
	// its bind-time store constraint stays untouched.
	inline checked_result<script_value> cast_reference(script_value ref, const cast_expr& c, engine* eng,
	                                                   string_symbolizer* sym) {
		const script_value& seen = ref.deref();
		if (seen.is_null()) {
			return cast_error(seen, c, sym);
		}
		const bool ok = reference_binding_admits(c.target.get(), seen, eng);   // the one admission kernel of every binding form
		if (!ok) {
			return cast_error(seen, c, sym);
		}
		// a handle's referent is the object it holds (C++ *h): a store copy-assigns that object, which every alias sees,
		// where a store through the handle's own variable would rebind it
		if (c.target->base_type == script_value_type::jai_object_type && seen.raw_storage_index() == script_value::TYPEID_OBJECT) {
			const type_info* tag = seen.type_info_raw();
			if (tag && tag->base_type == script_value_type::jai_shared_ptr_type) {
				script_value object = seen;
				object.set_type_info(c.target);
				return script_value::make_cell_reference(std::move(object), eng);
			}
		}
		return ref;
	}

	// A temporary bound by cast<T&>: the reference holder owns it
	inline script_value cast_pin_temporary(script_value value, engine* eng) {
		if (value.is_reference()) {
			return value;
		}
		return script_value::make_cell_reference(std::move(value), eng);
	}

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_CAST_KERNEL_HPP__
