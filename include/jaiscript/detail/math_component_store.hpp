#pragma once

#ifndef __JAISCRIPT_DETAIL_MATH_COMPONENT_STORE_HPP__
#define __JAISCRIPT_DETAIL_MATH_COMPONENT_STORE_HPP__

// `base.x = v` on a math value, shared VERBATIM by both backends: a math value is
// a value, so the store lands in the storage the base NAMES, never in the evaluated copy. The base resolves by
// shape: an evaluated reference (a subscript read, a ref alias) writes through; an identifier is its variable's
// cell (the backend's own ladder: slot, member scope, environment); a pure lvalue path (members and literal or
// identifier subscripts over an identifier or this) binds through the ref-lvalue kernel exactly like `f(obj.pos)`
// binds a reference parameter - pure resolution, so nothing the base names runs twice; anything else is a
// temporary, which C++ rejects ([expr.ass]: assignment to a member of a class prvalue).

#include <jaiscript/detail/math_values.hpp>
#include <jaiscript/detail/namespace_scope.hpp>
#include <jaiscript/detail/ref_lvalue.hpp>
#include <jaiscript/detail/static_store.hpp>

namespace jai::detail::math {

	// An arithmetic or compound assignment token as its operator row (only + - * / % and their compounds reach here)
	inline bop bop_of(token_type t) noexcept {
		switch (t) {
			case token_type::minus: case token_type::minus_equal: return bop::sub;
			case token_type::star: case token_type::star_equal: return bop::mul;
			case token_type::slash: case token_type::slash_equal: return bop::div;
			case token_type::percent: case token_type::percent_equal: return bop::mod;
			default: return bop::add;
		}
	}

	struct component_target {
		script_value pin;                 // the reference pinning the storage (null for an identifier's cell)
		script_value* storage = nullptr;  // the math value the store writes (null = a temporary)

		explicit component_target(engine* eng) : pin(std::monostate{}, eng) {}
		// Scratch-backed pins (a typed element, a PROPERTY reference) write the updated scratch back
		void commit() {
			if (auto* sr = pin.scratch_ref_holder()) { sr->commit_scratch(); }
		}
	};

	inline script_value* final_referent(script_value& v) {
		script_value* p = &v.deref();
		while (p->is_reference()) { p = &p->deref(); }
		return p;
	}

	// base: the evaluated base as the store site holds it (a reference for subscript reads and aliases);
	// ident_cell: the backend's identifier ladder (returns the variable's storage or null)
	template <typename IdentCell>
	inline component_target resolve_component_target(script_value& base, const expression* base_expr, IdentCell&& ident_cell,
	                                                 const caller_frame_view& caller, environment* caller_env,
	                                                 engine* eng, string_symbolizer* symbolizer) {
		component_target t(eng);
		if (base.is_reference()) {
			t.pin = base;
			t.storage = final_referent(t.pin);
		} else if (base_expr && base_expr->get_type() == node_type::identifier_expr) {
			if (script_value* cell = ident_cell(static_cast<const identifier_expr*>(base_expr))) {
				if (cell->scratch_ref_holder()) {   // an alias of a typed element or a property: the store lands in its scratch, then commits
					t.pin = *cell;
					t.storage = final_referent(t.pin);
				} else {
					t.storage = &cell->deref();
				}
			}
		} else if (base_expr && is_ref_bindable_lvalue(base_expr)) {
			auto resolved = resolve_ref_lvalue(base_expr, caller, caller_env, eng, symbolizer);
			if (resolved && resolved.value().is_reference()) {
				t.pin = std::move(resolved.value());
				t.storage = final_referent(t.pin);
			}
		}
		if (t.storage && !is_kind(t.storage->raw_storage_index())) { t.storage = nullptr; }
		return t;
	}

	// A static base (`C::s.x = 1`, `ns::v.x = 1`): the namespace scope's cell or the class's static field, by the ladder
	// and texts the whole-value stores take
	inline checked_result<script_value*> static_component_cell(const member_expr* m, engine& eng, const environment& env,
	                                                           string_symbolizer& symbols, const class_definition* access_context) {
		if (auto ns = resolve_namespace_store(eng, m, symbols)) {
			if (ns->missing) {
				return checked_result<script_value*>(namespace_member_missing(env, m, symbols));
			}
			if (!ns->cell) {
				return checked_result<script_value*>(make_error_code(runtime_error_code::type_mismatch), "Cannot assign to '{0}': it is read-only", m->member_id);
			}
			return ns->cell;
		}
		auto cls = resolve_static_store_class(m, env, symbols, access_context);
		if (!cls) {
			return cls.error_value();
		}
		script_value* cell = cls.value()->get_static_field_ptr(m->member_id);
		if (!cell) {
			return checked_result<script_value*>(make_error_code(runtime_error_code::undefined_variable),
				"Cannot assign to static member: field '{0}' not found", m->member_id);
		}
		return cell;
	}

	// The whole store: the component named, the target resolved, the value narrowed into place. Returns the stored
	// component (the value a consumed assignment expression yields)
	template <typename IdentCell, typename StaticCell>
	inline checked_result<script_value> assign_component(script_value& base, const member_expr* member, const script_value& value,
	                                                     IdentCell&& ident_cell, StaticCell&& static_cell, const caller_frame_view& caller,
	                                                     environment* caller_env, engine* eng, string_symbolizer* symbolizer) {
		const vk k = base.deref().unchecked_vec_kind();
		const int c = component_of(k, member->member);
		if (c < 0) {
			return checked_result<script_value>(make_error_code(runtime_error_code::member_not_found), no_member_text(k), member->member_id);
		}
		const expression* base_expr = member->object.get();
		component_target t(eng);
		if (!base.is_reference() && base_expr->get_type() == node_type::member_expr && static_cast<const member_expr*>(base_expr)->is_static) {
			auto cell = static_cell(static_cast<const member_expr*>(base_expr));
			if (!cell) {
				return cell.error_value();
			}
			t.storage = final_referent(*cell.value());
			if (!is_kind(t.storage->raw_storage_index())) { t.storage = nullptr; }
		} else {
			t = resolve_component_target(base, base_expr, ident_cell, caller, caller_env, eng, symbolizer);
		}
		if (!t.storage) {
			return checked_result<script_value>(make_error_code(runtime_error_code::invalid_assignment_target), temporary_member_text);
		}
		const script_value& rhs = value.deref();
		auto stored = rhs.raw_storage_index() == script_value::TYPEID_CPP_BOUND
			? store_component(*t.storage, c, rhs.bound_decoded_temp(), eng)
			: store_component(*t.storage, c, rhs, eng);
		if (stored) { t.commit(); }
		return stored;
	}

	// `v.x` where a reference binds (a component is an lvalue, as in C++): a COMPONENT reference over
	// the storage the vector lives in. The base binds as a reference base does - a bare identifier's variable (share turns
	// its storage into a reference: the backend's cell), a static's cell, the pure lvalue path (a field, an element, a C++
	// property). A non-reference when the expression is no component of a math value.
	template <typename Share>
	inline checked_result<script_value> component_reference(const expression* e, const caller_frame_view& caller, environment* caller_env,
	                                                        const class_definition* access, engine* eng, string_symbolizer* symbolizer,
	                                                        Share&& share) {
		script_value none(std::monostate{}, eng);
		if (!e || e->get_type() != node_type::member_expr || symbolizer->frozen()) {
			return none;
		}
		const auto* m = static_cast<const member_expr*>(e);
		const expression* base_expr = m->object.get();
		if (!base_expr || m->is_static || m->null_safe || m->is_arrow || !any_component_name(m->member)) {
			return none;
		}
		script_value base(std::monostate{}, eng);
		if (base_expr->get_type() == node_type::identifier_expr) {
			std::optional<checked_result<void>> access_error;
			const script_value* storage = ref_identifier_storage(static_cast<const identifier_expr*>(base_expr), caller, caller_env, eng,
			                                                     symbolizer, access_error);
			if (access_error) {
				return access_error->error_value();
			}
			if (!storage) {
				return none;
			}
			base = share(*const_cast<script_value*>(storage));
		} else if (base_expr->get_type() == node_type::member_expr && static_cast<const member_expr*>(base_expr)->is_static) {
			if (!caller_env) {
				return none;
			}
			auto cell = static_member_lvalue(*eng, static_cast<const member_expr*>(base_expr), *caller_env, *symbolizer, access);
			if (!cell) {
				return none;
			}
			if (!*cell) {
				return cell->error_value();
			}
			base = share(*cell->value());
		} else if (is_ref_bindable_lvalue(base_expr)) {
			auto resolved = resolve_ref_lvalue(base_expr, caller, caller_env, eng, symbolizer);
			if (!resolved) {
				return resolved.error() == make_error_code(runtime_error_code::invalid_reference) ? checked_result<script_value>(none) : resolved;
			}
			base = std::move(resolved.value());
		}
		if (!base.is_reference()) {
			return none;
		}
		auto value = ref_checked_deref(base);
		if (!value || !is_kind(value.value().raw_storage_index())) {
			return none;
		}
		const int c = component_of(value.value().unchecked_vec_kind(), m->member);
		if (c < 0) {
			return none;
		}
		return script_value::make_component_reference(base, static_cast<size_t>(c), eng);
	}

	// A stamped component read (member_expr::place_read) whose base evaluated to a reference: the COMPONENT reference
	// over that reference, else a non-reference and the ordinary read
	inline script_value evaluated_component_reference(const script_value& object, const member_expr* m, engine* eng) {
		script_value none(std::monostate{}, eng);
		if (!object.is_reference()) {
			return none;
		}
		auto value = ref_checked_deref(object);
		if (!value || !is_kind(value.value().raw_storage_index())) {
			return none;
		}
		const int c = component_of(value.value().unchecked_vec_kind(), m->member);
		return c < 0 ? none : script_value::make_component_reference(object, static_cast<size_t>(c), eng);
	}

} // namespace jai::detail::math

namespace jai::detail {

	// `base.f = v` where base is a script class VALUE no storage holds (a by-value call result, a constructor call, an
	// operator's or a value cast's result): C++ rejects built-in assignment to a member of a class prvalue, so the store
	// raises the math temporary's text. A handle, a reference or an object something else owns (a host shared_ptr's
	// layer) names its object; a name, member, element or conditional of lvalues (its arms read as references) names storage.
	// held: the copies of the value the caller itself holds (the object's one count is every holder's)
	inline bool class_temporary_member_store(const member_expr* m, const script_value& base_raw, long held = 1) {
		const expression* base = m->object.get();
		switch (base->get_type()) {
			case node_type::identifier_expr: case node_type::member_expr: case node_type::this_expr: return false;
			case node_type::binary_expr:
				if (static_cast<const binary_expr*>(base)->op.type == token_type::left_bracket) { return false; }
				break;
			default: break;
		}
		if (base_raw.raw_storage_index() != script_value::TYPEID_OBJECT) { return false; }
		const type_info* tag = base_raw.type_info_raw();
		if (tag && tag->base_type == script_value_type::jai_shared_ptr_type) { return false; }
		const auto& holder = base_raw.unchecked_object_handle();
		return holder && holder->is_instance() && holder->object_use_count() == held;
	}

	JAI_NOINLINE inline checked_result<void> class_temporary_member_error() {
		return checked_result<void>(make_error_code(runtime_error_code::invalid_assignment_target), math::temporary_member_text);
	}

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_MATH_COMPONENT_STORE_HPP__
