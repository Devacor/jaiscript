#pragma once

#ifndef __JAISCRIPT_DETAIL_REF_LVALUE_HPP__
#define __JAISCRIPT_DETAIL_REF_LVALUE_HPP__

// Reference-argument lvalue machinery shared VERBATIM by the vm and the interpreter
// (parity by construction): classification of ref-bindable argument shapes
// (f(obj.field), f(arr[i]), chains), bind-time resolution to an owner-pinned
// field/element reference, and constrained store-through enforcement.

#include <jaiscript/core/value.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/lvalue_shape.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/member_scope.hpp>
#include <jaiscript/detail/operator_table.hpp>
#include <jaiscript/detail/numeric_conversion.hpp>
#include <jaiscript/detail/container_enforce.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/detail/scalar_ops.hpp>
#include <vector>

namespace jai::detail {

	// cast<T>(e) / cast<T&>(e) error texts (cast_kernel.hpp casts; a cast<T&> link of a reference chain checks here too)
	inline constexpr std::string_view cast_error_text = "Cannot cast '{0}' to '{1}'";

	inline std::string cast_target_spelling(const cast_expr& c) {
		std::string out;
		if (c.target->base_type == script_value_type::jai_shared_ptr_type) {
			const type_info* pointee = c.target->element_type().get();
			out = "shared_ptr<" + (pointee ? pointee->canonical_name() : std::string("auto")) + ">";
		} else {
			out = c.target->canonical_name();
		}
		if (c.by_reference) {
			out += "&";
		}
		return out;
	}

	// The operand's DYNAMIC class (the instance, never the value tag: a T stored in an
	// S-typed field carries S's tag and is still a T)
	inline const class_definition* cast_dynamic_class(const script_value& seen) {
		auto holder = seen.get_object_holder();
		if (!holder || !holder->is_instance()) {
			return nullptr;
		}
		auto* instance = holder->unchecked_instance();
		return instance ? instance->get_class_definition() : nullptr;
	}

	inline std::string cast_source_spelling(const script_value& seen) {
		if (seen.is_null()) {
			return "null";
		}
		if (const class_definition* def = cast_dynamic_class(seen)) {
			return def->get_name();
		}
		const type_info_ptr tag = seen.get_type_info();
		if (tag && (tag->base_type == script_value_type::jai_object_type ||
		            tag->base_type == script_value_type::jai_shared_ptr_type) && !tag->type_name.empty()) {
			return tag->type_name;
		}
		return container_base_type_name(seen.type());
	}

	// a parallel worker cannot intern a spelling it has not seen: the same text without them
	inline constexpr std::string_view cast_error_text_slotless = "Cannot cast the value to the target type";

	JAI_NOINLINE inline error_propagator cast_error_propagator(const script_value& seen, const cast_expr& c, string_symbolizer* sym) {
		const std::string source = cast_source_spelling(seen);
		const std::string target = cast_target_spelling(c);
		if (sym->frozen()) {
			const auto source_id = sym->find(source);
			const auto target_id = sym->find(target);
			if (!source_id || !target_id) {
				return error_propagator{make_error_code(runtime_error_code::type_mismatch), cast_error_text_slotless, 0, 0};
			}
			return error_propagator{make_error_code(runtime_error_code::type_mismatch), cast_error_text, *source_id, *target_id};
		}
		return error_propagator{make_error_code(runtime_error_code::type_mismatch), cast_error_text, sym->intern(source), sym->intern(target)};
	}

	template <typename T = script_value>
	inline checked_result<T> cast_error(const script_value& seen, const cast_expr& c, string_symbolizer* sym) {
		return cast_error_propagator(seen, c, sym);
	}

	// a string& takes any string temporary, written or not, as a private cell (an lvalue argument binds before this)
	inline bool is_string_reference_temporary(const parameter& param, const script_value& value) {
		return param.is_reference && param.type && param.type->base_type == script_value_type::jai_string_type && value.is_string();
	}

	// a string temporary shares its copy-on-write node, a C++-bound one detaches from native storage
	inline script_value make_reference_argument_temporary(const parameter& param, const script_value& value, engine* eng) {
		return script_value::make_cell_reference(
			is_string_reference_temporary(param, value) ? clone_for_assignment(value) : script_value(value.deref()), eng);
	}

	inline bool concrete_reference_type(const type_info* t) {
		if (!t || t->base_type == script_value_type::jai_any_type || t->type_name.empty() || t->type_name == "auto") { return false; }
		for (const auto& parameter : t->type_params) {
			if (!concrete_reference_type(parameter.get())) { return false; }
		}
		return true;
	}

	// A T& binds an lvalue that holds a T and never converts (C++): a primitive or math value its exact
	// kind, a class T or a subclass (a typed null of one too), a container or handle exactly its type
	// when both types are concrete; var& and auto& bind any lvalue, and a container of unknown element type stays lenient
	inline bool reference_binding_admits(const type_info* declared, const script_value& current_in, engine* eng) {
		if (!declared) { return true; }
		const script_value* cur = &current_in;
		for (int depth = 0; depth < 8 && cur->is_reference(); ++depth) { cur = &cur->deref(); }   // a property's value can be a reference
		std::optional<script_value> decoded;
		if (cur->raw_storage_index() == script_value::TYPEID_CPP_BOUND) { decoded.emplace(cur->bound_decoded_temp()); cur = &*decoded; }
		const script_value& value = *cur;
		switch (declared->base_type) {
			case script_value_type::jai_int_type: case script_value_type::jai_float_type: case script_value_type::jai_bool_type:
			case script_value_type::jai_char_type: case script_value_type::jai_string_type: case script_value_type::jai_utf8_string_type:
			case script_value_type::jai_vec2_type: case script_value_type::jai_vec3_type: case script_value_type::jai_vec4_type:
			case script_value_type::jai_ivec2_type: case script_value_type::jai_ivec3_type: case script_value_type::jai_color_type:
				return value.current_type() == declared->base_type;
			case script_value_type::jai_object_type: {
				if (declared->type_name.empty()) { return true; }
				if (value.is_null()) {
					const auto* tag = value.get_type_info().get();
					if (!tag || tag->base_type != script_value_type::jai_object_type || tag->type_name.empty()) { return true; }
					return tag->type_name == declared->type_name || class_derives_from(eng, tag->id, declared->id);
				}
				if (!value.is_object()) { return false; }
				if (value.raw_storage_index() == script_value::TYPEID_OBJECT) {
					// an instance of exactly the declared class, read in place (no handle copies)
					const auto& holder = value.unchecked_object_handle();
					if (holder && holder->is_instance()) {
						const auto* inst = holder->unchecked_instance();
						const class_definition* def = inst->get_class_definition();
						if (def && def->get_type_id() == declared->id && !def->wrapped_object_has_layer(*inst)) { return true; }
					}
				}
				auto view = resolve_slot_class(value, eng);
				if (!view.def) { return true; }
				return view.def->get_name() == declared->type_name || class_derives_from(eng, view.def->get_type_id(), declared->id);
			}
			case script_value_type::jai_array_type:
			case script_value_type::jai_map_type:
			case script_value_type::jai_shared_ptr_type:
			case script_value_type::jai_weak_ptr_type: {
				const type_info* held = value.get_type_info().get();
				if (held == declared) { return true; }
				if (!concrete_reference_type(declared) || !concrete_reference_type(held)) { return true; }
				return held->equals(*declared);
			}
			default: return true;
		}
	}

	// `T& name = e;` binds e's storage only when it holds a T (a var too - a reference must reference):
	// the declaration's text names the value's type and T&
	inline std::optional<error_propagator> reference_decl_rejection(const variable_decl& decl, const script_value& value, engine* eng) {
		const type_info* t = decl.type.get();
		if (!t || t->base_type != script_value_type::jai_reference_type || t->type_params.empty()) { return std::nullopt; }
		if (reference_binding_admits(t->type_params[0].get(), value, eng)) { return std::nullopt; }
		return typed_slot_rejection(value.deref(), *t, slot_site{slot_kind::declaration, decl.name, {}}, eng);
	}

	// `const T& r = e;` over a temporary: the temporary, converted to T as a declaration of T would convert it, held in a cell only r
	// holds, so it dies when r does (C++ lifetime extension); convert(value, T) is the backend's declaration conversion
	template <class Convert>
	inline checked_result<script_value> const_reference_temporary(const variable_decl& decl, script_value value, engine* eng, Convert&& convert) {
		const type_info* t = decl.type.get();
		const type_info* referent = t && t->base_type == script_value_type::jai_reference_type && !t->type_params.empty() ? t->type_params[0].get() : nullptr;
		if (referent && referent->base_type != script_value_type::jai_any_type && referent->type_name != "auto") {
			auto converted = convert(std::move(value), t->type_params[0]);
			if (!converted) { return converted; }
			value = std::move(converted.value());
		}
		return script_value::make_cell_reference(std::move(value), eng);
	}

	inline bool reference_decl_checks_admission(const variable_decl& decl) {
		const type_info* t = decl.type.get();
		if (!t || t->base_type != script_value_type::jai_reference_type || t->type_params.empty()) { return false; }
		const type_info* referent = t->type_params[0].get();
		return referent && referent->base_type != script_value_type::jai_any_type && referent->type_name != "auto" &&
		       !decl.declared_ref_storage_enforced && !decl.reference_admission_proven;
	}

	inline checked_result<script_value> ref_non_lvalue_error() {
		return checked_result<script_value>(
			make_error_code(runtime_error_code::invalid_reference),
			"Cannot pass non-lvalue to reference parameter");
	}

	// A fields_ node is the member's TRUTH only for script-declared fields. C++
	// .property() members (dynamic_binder) keep a dead null placeholder in fields_ -
	// reads/writes route through the bound accessors - so a ref bound to the node
	// would silently swallow writes (they take the CPP-MEMBER / PROPERTY mint instead).
	// Instance-local fields with no class declarer have no accessor to bypass and stay
	// bindable.
	inline bool ref_field_is_script_backed(const strong_ptr<class_instance>& instance, uint64_t member_id) {
		const class_definition* class_def = instance->get_class_definition();
		if (!class_def) {
			return true;
		}
		const class_definition* declarer = class_def->find_field_declaring_class(member_id);
		return !declarer || !declarer->is_cpp_class();
	}

	// A field with a user _get_/_set_ anywhere in its chain is a computed property from the
	// outside: an alias to the backing cell would bypass the accessor. A frozen symbolizer is
	// a parallel region, whose workers never reach accessors (worker walls read the cell).
	inline bool ref_field_has_user_accessor(const strong_ptr<class_instance>& instance, const member_expr* m,
	                                        uint64_t member_id, string_symbolizer* symbolizer) {
		const class_definition* class_def = instance->get_class_definition();
		if (!class_def || !class_def->has_property_getters() || symbolizer->frozen()) {
			return false;
		}
		if (m->getter_id == UINT64_MAX) {
			m->getter_id = symbolizer->get_getter_id_with_view(member_id).first;
		}
		if (m->setter_id == UINT64_MAX) {
			m->setter_id = symbolizer->get_setter_id_with_view(member_id).first;
		}
		return class_def->user_accessor(m->getter_id) != nullptr || class_def->user_accessor(m->setter_id) != nullptr;
	}

	// The instance behind a member step's receiver when the site's verdict for its class still holds: the member is a plain script
	// field (the full ladder found no C++ property, a script-backed declaration, no user accessor, a public chain), so only the
	// instance's own field is left to read. Never consulted or written during a parallel region (a frozen symbolizer).
	inline class_instance* ref_plain_script_field(const script_value& receiver, const member_expr* m, string_symbolizer* symbolizer) {
		if (receiver.raw_storage_index() != script_value::TYPEID_OBJECT) { return nullptr; }
		const auto* holder = receiver.peek_object_holder();
		if (!holder || !holder->is_instance()) { return nullptr; }
		auto* instance = holder->unchecked_instance();
		const class_definition* def = instance->get_class_definition();
		if (!def || def != m->ref_field_class || def->field_shape_epoch() != m->ref_field_shape ||
		    def->method_epoch() != m->ref_field_methods || symbolizer->frozen()) {
			return nullptr;
		}
		return instance;
	}

	inline void ref_note_plain_script_field(const class_definition* def, const member_expr* m, uint64_t member_id, string_symbolizer* symbolizer) {
		if (!def || !def->is_script_class() || def->chain_has_nonpublic() || symbolizer->frozen()) { return; }
		m->ref_field_class = def;
		m->ref_field_shape = def->field_shape_epoch();
		m->ref_field_methods = def->method_epoch();
		m->ref_field_declared = def->get_field_declared_type(member_id).get();
		m->ref_field_slot = def->field_slot(member_id);
	}

	// The reference a T& binds to a plain script field of `plain` (the instance behind `receiver`, whose class the site's verdict
	// holds for): the cell's own reference when it holds one, else a field reference over the slot the site remembers
	inline checked_result<script_value> plain_field_reference(const script_value& receiver, class_instance* plain, const member_expr* m,
	                                                          uint64_t member_id, engine* eng) {
		const uint32_t slot = m->ref_field_slot;
		script_value* field_value = slot != class_definition::k_no_field_slot ? plain->field_slot_value(slot) : plain->find_field_value(member_id);
		if (!field_value) {
			return ref_non_lvalue_error();
		}
		if (field_value->is_reference()) {
			return script_value(*field_value);
		}
		type_info_ptr tag = m->ref_field_declared ? type_info_ptr{m->ref_field_declared} : field_value->get_type_info();
		type_info_ptr constraint = nullptr;
		if (tag && tag->base_type != script_value_type::jai_any_type && tag->base_type != script_value_type::jai_null_type) {
			constraint = tag;
		}
		return script_value::make_field_reference(plain->strong_from_this(), member_id, eng, std::move(constraint), *field_value, slot);
	}

	// Mirrors script_value::deref() with checked errors instead of throws (bind-time
	// resolution must never throw). Returns a shallow copy of the dereferenced value.
	inline checked_result<script_value> ref_checked_deref(const script_value& v) {
		const script_value* cur = &v;
		while (cur->is_reference()) {
			const auto* holder = cur->get_reference_holder();
			if (holder->has_cell) {
				cur = holder->cell();
				continue;
			}
			if (holder->has_map_key) {
				auto it = holder->container_map->find(*holder->cell());
				if (it == holder->container_map->end()) {
					return checked_result<script_value>(make_error_code(runtime_error_code::invalid_reference), "Reference to a removed map entry");
				}
				cur = &it->second;
				continue;
			}
			if (holder->container) {
				if (holder->container_index >= holder->container->size()) {
					return checked_result<script_value>(make_error_code(runtime_error_code::invalid_reference), "Reference to a removed array element");
				}
				if (holder->container->is_typed()) {
					// materialized view per touch (no script_value element exists)
					return holder->container->get(holder->container_index, holder->owner);
				}
				cur = &holder->container->values()[holder->container_index];
				continue;
			}
			if (holder->owner_instance) {
				const script_value* field_value = holder->field_cell();
				if (!field_value) {
					return checked_result<script_value>(make_error_code(runtime_error_code::invalid_reference), "Reference to a removed field");
				}
				cur = field_value;
				continue;
			}
			if (holder->property) {
				// CPP-MEMBER: the live box; PROPERTY: the getter (native only at a bind point)
				if (holder->property->live_member) {
					return script_value(*holder->cell());
				}
				return holder->read_property(holder->owner, /*bind_point*/ true);
			}
			if (holder->component_base) {
				try {
					return script_value(const_cast<std::remove_const_t<std::remove_reference_t<decltype(*holder)>>*>(holder)->materialize_component(holder->owner));
				} catch (const std::exception&) {
					return checked_result<script_value>(make_error_code(runtime_error_code::invalid_reference), "Reference to a component of a value that is no longer a vector");
				}
			}
			// unreachable: every factory sets a mode
			return checked_result<script_value>(make_error_code(runtime_error_code::invalid_reference), "Null reference");
		}
		return script_value(*cur);
	}

	// C++ property reference mint - the bare name, this.name and obj.name spellings on
	// every receiver kind (hybrid layer, plain wrapper, raw handle). A member-pointer
	// property the cpp_bound box decodes binds the MEMBER ITSELF (CPP-MEMBER) unless a
	// script _get_/_set_ override sits in the chain; everything else binds through the
	// accessors (PROPERTY). Parallel workers reach neither accessors nor raw member
	// memory: a frozen symbolizer keeps the non-lvalue verdict. Never throws.
	inline checked_result<script_value> bind_cpp_property_ref(const script_value& receiver, const cpp_property_site& site,
	                                                          uint64_t member_id, engine* eng, string_symbolizer* symbolizer) {
		if (!site || symbolizer->frozen()) {
			return ref_non_lvalue_error();
		}
		class_instance* instance = site.view.instance.get();
		if (instance && instance->script_layer_retired()) {
			return checked_result<script_value>(make_error_code(runtime_error_code::script_layer_retired),
				"Script layer '{0}' was detached (script_layer_detach, or a move to another engine): this handle no longer reaches its object",
				site.view.def->get_type_id());
		}
		const auto* member = site.accessor_class ? site.accessor_class->get_property_member_accessor(member_id) : nullptr;
		if (member && !cpp_property_has_script_override(site)) {
			void* address = nullptr;
			try {
				address = member->address(receiver);
			} catch (const std::exception&) {
				address = nullptr;
			}
			if (!address) {
				return ref_non_lvalue_error();
			}
			return script_value::make_cpp_member_reference(receiver, pin_cpp_object(receiver, instance), address,
			                                               member->size_and_sign, member->semantic_index, member_id, eng);
		}
		return script_value::make_property_reference(receiver, member_id, eng);
	}

	inline checked_result<script_value> bind_cpp_property_ref(const script_value& receiver, uint64_t member_id,
	                                                          engine* eng, string_symbolizer* symbolizer) {
		return bind_cpp_property_ref(receiver, resolve_cpp_property(receiver, member_id, eng), member_id, eng, symbolizer);
	}

	// Caller-frame view for the kernel's base-identifier / this resolution. The
	// interpreter's frames keep slot storage in call_frame; the vm's stage-2 frames keep
	// it in value-stack windows — the view abstracts ONLY the slot read (window_data +
	// window_live set by the vm; resolution never runs script, so the raw window pointer
	// cannot be invalidated by stack growth mid-resolve). Method metadata (is_method /
	// this) stays on call_frame either way.
	struct caller_frame_view {
		call_frame* metadata = nullptr;
		const script_value* window_data = nullptr;
		size_t window_live = 0;

		const script_value* slot(size_t idx) const {
			if (window_data) { return idx < window_live ? window_data + idx : nullptr; }
			return metadata ? metadata->get_local(idx) : nullptr;
		}
		const script_value* this_ptr() const {
			return (metadata && metadata->is_method) ? &metadata->get_this() : nullptr;
		}
	};

	// The storage a bare identifier names at a reference bind point: the caller's frame slot, its member scope (a field or
	// static outranks a same-named global), its environment, a method-lazy caller's receiver field. Null when out of reach;
	// an access failure lands in access_error.
	inline const script_value* ref_identifier_storage(const identifier_expr* ident, const caller_frame_view& caller,
	                                                  environment* caller_env, engine* eng, string_symbolizer* symbolizer,
	                                                  std::optional<checked_result<void>>& base_access_error) {
		if (ident->slot_index != SIZE_MAX) {
			if (const script_value* slot_ptr = caller.slot(ident->slot_index)) {
				return slot_ptr;
			}
		}
		uint64_t symbol_id = ident->symbol_id;
		if (symbol_id == UINT64_MAX) {
			symbol_id = symbolizer->intern(ident->name);
			const_cast<identifier_expr*>(ident)->symbol_id = symbol_id;
		}
		// ::name bases resolve against the GLOBAL env only
		if (ident->global_qualified) {
			return eng ? engine_access::get_global_environment(*eng)->get_value_ptr(symbol_id) : nullptr;
		}
		// Caller member scope (call_frame stamp): fields/statics outrank globals
		// between the function's own scopes and the definition/global chain
		const auto scope_state = caller.metadata
			? static_cast<member_scope_state>(caller.metadata->scope_state)
			: member_scope_state::none;
		if (scope_state == member_scope_state::instance ||
		    scope_state == member_scope_state::statics) {
			if (caller_env) {
				if (script_value* own = own_scope_lookup(caller_env, caller.metadata->scope_floor, symbol_id)) {
					return own;
				}
			}
			auto member = probe_member_scope(scope_state, caller.metadata->scope_this,
			                                 caller.metadata->scope_static, symbol_id,
			                                 /*want_methods*/ false);
			if (member) {
				if (auto access = bare_member_access(member, symbol_id, caller_env ? caller_env->find_access_context() : nullptr); !access) {
					base_access_error = std::move(access);
					return nullptr;
				}
			}
			if (member.value_ptr) { return member.value_ptr; }
			if (member_scope_has_method(scope_state, caller.metadata->scope_this,
			                            caller.metadata->scope_static, symbol_id)) {
				return nullptr;   // a member METHOD shadows: never fall past the class scope
			}
			if (caller.metadata->scope_floor) {
				return caller.metadata->scope_floor->get_value_ptr(symbol_id);
			}
			return caller_env ? caller_env->get_value_ptr(symbol_id) : nullptr;
		}
		if (caller_env) {
			if (const script_value* env_ptr = caller_env->get_value_ptr(symbol_id)) {
				return env_ptr;
			}
		}
		// Method-LAZY caller frames carry no method env: bare field names resolve
		// through the frame's receiver (twin of environment::get_value_ptr's
		// method-kind fallback — env-resolved eager frames never reach this)
		if (const script_value* tp = caller.this_ptr()) {
			if (tp->is_object() && !tp->is_null()) {
				if (auto instance = const_cast<script_value*>(tp)->get_class_instance()) {
					script_value& field_ref = instance->get_field(symbol_id, false);
					if (!field_ref.is_invalid()) { return &field_ref; }
					class_definition* declaring = caller.metadata && caller.metadata->is_method ? caller.metadata->scope_static : nullptr;
					if (class_definition* owner = bare_static_owner(declaring, instance->get_class_definition(), symbol_id)) {
						return owner->get_static_field_ptr(symbol_id);
					}
				}
			}
		}
		return nullptr;
	}

	// One member step of a reference chain over `cur`, the dereferenced value its base names: the final step's reference (or
	// its error), else nullopt with cur advanced to the member's value. `rooted` pins what a final step mints under the chain's
	// array root.
	template <class Rooted>
	inline std::optional<checked_result<script_value>> ref_member_step(script_value& cur, const member_expr* m, bool is_final,
	                                                                 environment* caller_env, engine* eng, string_symbolizer* symbolizer,
	                                                                 Rooted&& rooted) {
		const uint64_t member_id = m->member_id != UINT64_MAX ? m->member_id : symbolizer->intern(m->member);
		if (class_instance* plain = ref_plain_script_field(cur, m, symbolizer)) {
			if (is_final) {
				auto ref = plain_field_reference(cur, plain, m, member_id, eng);
				if (!ref) { return ref; }
				return rooted(std::move(ref.value()));
			}
			// the site's slot for this class (ref_plain_script_field proved the class and its epochs), with find_field_value's
			// ladder kept for a field outside the slots
			const uint32_t slot = m->ref_field_slot;
			script_value* field_value = slot != class_definition::k_no_field_slot ? plain->field_slot_value(slot) : plain->find_field_value(member_id);
			if (!field_value) {
				return ref_non_lvalue_error();
			}
			auto derefed = ref_checked_deref(*field_value);
			if (!derefed) { return derefed; }
			cur = std::move(derefed.value());
			return std::nullopt;
		}
		// Binder .property() members bind on every receiver kind (a same-named
		// script field shadows: getter_id 0 keeps the FIELD path below); a
		// non-final property step reads through a native getter only
		auto property = resolve_cpp_property(cur, member_id, eng);
		if (property.view.def && property.view.def->chain_has_nonpublic()) [[unlikely]] {
			// Access enforcement at the bind site: the ref is minted in the CALLER's
			// context, so a private field can't escape by-reference either
			auto access_ok = enforce_member_access(property.view.def, member_id,
				caller_env ? caller_env->find_access_context() : nullptr);
			if (!access_ok) { return access_ok.error_value(); }
		}
		if (property) {
			if (is_final) {
				return bind_cpp_property_ref(cur, property, member_id, eng, symbolizer);
			}
			if (symbolizer->frozen()) {
				return ref_non_lvalue_error();
			}
			auto read = read_cpp_property_site(cur, property, /*bind_point*/ true);
			if (!read) { return read; }
			cur = std::move(read.value());
			return std::nullopt;
		}
		auto instance = property.view.instance;
		if (!instance) {
			return ref_non_lvalue_error();   // raw C++ handles without such a property, non-objects: not supported
		}
		script_value* field_value = instance->find_field_value(member_id);
		if (!field_value || !ref_field_is_script_backed(instance, member_id) ||
		    ref_field_has_user_accessor(instance, m, member_id, symbolizer)) {
			return ref_non_lvalue_error();
		}
		ref_note_plain_script_field(property.view.def, m, member_id, symbolizer);
		if (is_final) {
			if (field_value->is_reference()) {
				// Field already holds a reference: share its holder (matches
				// set_field's write-through semantics)
				return script_value(*field_value);
			}
			// DECLARED field type wins as the constraint (typed fields enforce like
			// locals; 'var' declares any and stays unconstrained); undeclared ('auto')
			// fields fall back to the bind-time value tag
			type_info_ptr tag = nullptr;
			if (const class_definition* field_class_def = instance->get_class_definition()) {
				tag = field_class_def->get_field_declared_type(member_id);
			}
			if (!tag) {
				tag = field_value->get_type_info();
			}
			type_info_ptr constraint = nullptr;
			if (tag && tag->base_type != script_value_type::jai_any_type &&
			    tag->base_type != script_value_type::jai_null_type) {
				constraint = tag;
			}
			return rooted(script_value::make_field_reference(instance, member_id, eng, std::move(constraint), *field_value));
		}
		auto derefed = ref_checked_deref(*field_value);
		if (!derefed) { return derefed; }
		cur = std::move(derefed.value());
		return std::nullopt;
	}

	// Binds a field/subscript/chain lvalue argument to a reference parameter. Resolution
	// is pure pointer/value chasing over already-evaluated state - it never runs script,
	// never throws, and never mutates the instance (a NATIVE property getter may run for
	// a non-final property step). Bindability = a binder .property() member (CPP-MEMBER /
	// PROPERTY mint, every receiver kind) or an INSTANCE fields_ node (find_field_value,
	// never has_field/get_field: those consult class defaults and lazy-insert) whose
	// declaring class is script-side (ref_field_is_script_backed). Script-class fields
	// qualify - their synthesized accessors transparently wrap the node - while computed
	// script properties (a user _get_/_set_ in the chain) keep the non-lvalue error.
	// Field refs pin the instance, element refs pin the vector; typed fields/elements
	// carry their current tag as the assign-through constraint.
	inline checked_result<script_value> resolve_ref_lvalue(const expression* arg, const caller_frame_view& caller,
	                                                       environment* caller_env,
	                                                       engine* eng, string_symbolizer* symbolizer) {
		// Collect steps outermost-first; applied base-first (reverse)
		const expression* inline_steps[8];
		std::vector<const expression*> heap_steps;
		size_t step_count = 0;
		const expression* base = arg;
		while (base) {
			const node_type t = base->get_type();
			if (t == node_type::member_expr) {
				if (step_count < 8) { inline_steps[step_count] = base; } else { heap_steps.push_back(base); }
				++step_count;
				base = static_cast<const member_expr*>(base)->object.get();
			} else if (t == node_type::binary_expr) {
				if (step_count < 8) { inline_steps[step_count] = base; } else { heap_steps.push_back(base); }
				++step_count;
				base = static_cast<const binary_expr*>(base)->left.get();
			} else if (t == node_type::cast_expr && static_cast<const cast_expr*>(base)->by_reference && step_count > 0) {
				if (step_count < 8) { inline_steps[step_count] = base; } else { heap_steps.push_back(base); }
				++step_count;
				base = static_cast<const cast_expr*>(base)->operand.get();
			} else {
				break;
			}
		}
		if (!base || step_count == 0) {
			return ref_non_lvalue_error();
		}
		auto step_at = [&](size_t i) -> const expression* {
			return i < 8 ? inline_steps[i] : heap_steps[i - 8];
		};

		std::optional<checked_result<void>> base_access_error;   // a bare base out of reach (bare_member_access)
		auto resolve_identifier_ptr = [&](const identifier_expr* ident) -> const script_value* {
			return ref_identifier_storage(ident, caller, caller_env, eng, symbolizer, base_access_error);
		};

		// A bare C++ property as the chain's BASE (f(child.x) inside a hybrid method): no
		// cell to point at, so the caller's member scope reads it through a native getter
		auto bare_property_base = [&](const identifier_expr* ident) -> checked_result<script_value> {
			const auto scope_state = caller.metadata
				? static_cast<member_scope_state>(caller.metadata->scope_state)
				: member_scope_state::none;
			if (scope_state != member_scope_state::instance || !caller.metadata->scope_this ||
			    ident->global_qualified || symbolizer->frozen()) {
				return ref_non_lvalue_error();
			}
			auto member = probe_member_scope(scope_state, caller.metadata->scope_this, nullptr,
			                                 ident->symbol_id, /*want_methods*/ false);
			if (member.what != member_probe_hit::kind::cpp_property) {
				return ref_non_lvalue_error();
			}
			const script_value& receiver = *caller.metadata->scope_this;
			return read_cpp_property_site(receiver, resolve_cpp_property(receiver, ident->symbol_id, eng), /*bind_point*/ true);
		};

		script_value cur(std::monostate{}, eng);
		if (base->get_type() == node_type::identifier_expr) {
			const auto* ident = static_cast<const identifier_expr*>(base);
			const script_value* base_ptr = resolve_identifier_ptr(ident);
			if (base_access_error) { return base_access_error->error_value(); }
			if (!base_ptr) {
				auto read = bare_property_base(ident);
				if (!read) { return read; }
				cur = std::move(read.value());
			} else {
				auto derefed = ref_checked_deref(*base_ptr);
				if (!derefed) { return derefed; }
				cur = std::move(derefed.value());
			}
		} else {   // this_expr
			const script_value* this_ptr = caller.this_ptr();
			if (!this_ptr && caller_env) {
				this_ptr = caller_env->get_value_ptr(symbolizer->get_this_id());
			}
			if (!this_ptr) {
				return ref_non_lvalue_error();
			}
			auto derefed = ref_checked_deref(*this_ptr);
			if (!derefed) { return derefed; }
			cur = std::move(derefed.value());
		}

		// The outermost array a non-final subscript descended into: every reference minted
		// below it counts in that array's live_elem_refs (parallel forks prove exclusivity
		// from the declared type when it is zero)
		strong_ptr<script_array> chain_root;
		auto rooted = [&chain_root](script_value ref) -> checked_result<script_value> {
			if (chain_root) { script_value::pin_element_root(ref, chain_root); }
			return ref;
		};
		for (size_t i = step_count; i-- > 0;) {
			const bool is_final = (i == 0);
			const expression* step = step_at(i);
			if (step->get_type() == node_type::cast_expr) {
				const auto* c = static_cast<const cast_expr*>(step);
				if (cur.is_null() || !reference_binding_admits(c->target.get(), cur, eng)) {
					return cast_error(cur, *c, symbolizer);
				}
				continue;
			}
			if (step->get_type() == node_type::member_expr) {
				if (auto done = ref_member_step(cur, static_cast<const member_expr*>(step), is_final, caller_env, eng, symbolizer, rooted)) {
					return std::move(*done);
				}
			} else {
				const auto* b = static_cast<const binary_expr*>(step);
				script_value key(std::monostate{}, eng);
				if (b->right->get_type() == node_type::literal_expr) {
					key = static_cast<const literal_expr*>(b->right.get())->value;
				} else {
					const script_value* index_ptr = resolve_identifier_ptr(static_cast<const identifier_expr*>(b->right.get()));
					if (!index_ptr) {
						return ref_non_lvalue_error();
					}
					auto index_derefed = ref_checked_deref(*index_ptr);
					if (!index_derefed) { return index_derefed; }
					key = std::move(index_derefed.value());
				}
				if (cur.is_map()) {
					// an existing entry only: a reference binding reads the map, and a read never inserts
					const strong_ptr<script_map> map_storage = cur.get_map_storage();
					auto entry = map_storage->find(key);
					if (entry == map_storage->end()) {
						return ref_non_lvalue_error();
					}
					script_value& value_ref = const_cast<script_value&>(entry->second);
					if (!is_final) {
						auto derefed = ref_checked_deref(value_ref);
						if (!derefed) { return derefed; }
						cur = std::move(derefed.value());
						continue;
					}
					if (value_ref.is_reference()) {
						return script_value(value_ref);
					}
					auto map_type_info = cur.get_type_info();
					return rooted(script_value::make_map_entry_reference(map_storage, entry->first, &value_ref, eng,
						map_type_info ? map_type_info->value_type() : nullptr));
				}
				// is_int (not raw index) so a bound-int index stays a valid lvalue index
				if (!cur.is_array() || !key.is_int()) {
					return ref_non_lvalue_error();   // strings keep the non-lvalue error
				}
				const script_int index = key.unchecked_as_int();
				auto storage = cur.get_array_storage();
				if (index < 0 || static_cast<size_t>(index) >= storage->size()) {
					return checked_result<script_value>(make_error_code(runtime_error_code::invalid_reference), "Reference to a removed array element");
				}
				if (is_final) {
					// typed nodes: elements are never references, straight to the mint
					if (!storage->is_typed()) {
						script_value& element = storage->values()[static_cast<size_t>(index)];
						if (element.is_reference()) {
							return script_value(element);
						}
					}
					auto array_type_info = cur.get_type_info();
					type_info_ptr element_type = array_type_info ? array_type_info->element_type() : nullptr;
					return rooted(script_value::make_element_reference(storage, static_cast<size_t>(index), eng, element_type));
				}
				if (!chain_root) { chain_root = storage; }
				if (storage->is_typed()) {
					// no element cell: a boxed read (an object slot's handle) carries the chain on
					cur = storage->get(static_cast<size_t>(index), eng);
					continue;
				}
				auto derefed = ref_checked_deref(storage->values()[static_cast<size_t>(index)]);
				if (!derefed) { return derefed; }
				cur = std::move(derefed.value());
			}
		}
		return ref_non_lvalue_error();   // unreachable: step_count >= 1
	}

	// A stamped member read (member_expr::place_read_field) whose base evaluated to a reference: the reference the chain's
	// member step mints over the referent, pinned under the array the base names; else a non-reference and the ordinary read
	inline checked_result<script_value> evaluated_member_reference(const script_value& object, const member_expr* m, environment* caller_env,
	                                                               engine* eng, string_symbolizer* symbolizer) {
		script_value none(std::monostate{}, eng);
		const auto* holder = object.is_reference() ? object.get_reference_holder() : nullptr;
		if (!holder) { return none; }
		auto value = ref_checked_deref(object);
		if (!value) { return none; }
		const strong_ptr<script_array> root = holder->elem_root ? holder->elem_root : holder->container;
		auto rooted = [&root](script_value ref) -> checked_result<script_value> {
			if (root) { script_value::pin_element_root(ref, root); }
			return ref;
		};
		auto bound = ref_member_step(value.value(), m, true, caller_env, eng, symbolizer, rooted);
		if (!bound || (!*bound && bound->error() == make_error_code(runtime_error_code::invalid_reference))) { return none; }
		return std::move(*bound);
	}

	// ---- Constrained store-through enforcement ----
	// An element reference runs the direct subscript store's kernel and a field reference the direct field store's table:
	// the same store passes or fails with one text whether spelled directly or through a bound ref.

	// engine.cpp: subclass admission walk (same symbol as container_enforce.hpp)
	bool class_derives_from(engine* eng, uint64_t derived_id, uint64_t base_id);

	inline checked_result<script_value> ref_convert_for_element(engine* eng, const script_value& element, type_info_ptr element_type) {
		const script_value& actual_element = element.is_reference() ? element.deref() : element;
		auto actual_type_info = actual_element.get_type_info();
		if (actual_type_info && actual_type_info->base_type == script_value_type::jai_shared_ptr_type) {
			return actual_element;
		}
		if (!element_type || element_type->base_type == script_value_type::jai_any_type) {
			return actual_element.clone();
		}
		const auto elem_type = actual_element.type();
		const auto target_type = element_type->base_type;
		// the direct store's rows: the class copy and tag, int/float/bool conversions, the byte range
		if ((target_type == script_value_type::jai_object_type && elem_type == script_value_type::jai_object_type) ||
		    target_type == script_value_type::jai_int_type || target_type == script_value_type::jai_float_type ||
		    target_type == script_value_type::jai_bool_type) {
			return convert_container_element(eng, actual_element, element_type);
		}
		return actual_element.clone();
	}

	JAI_NOINLINE inline checked_result<void> ref_element_error(string_symbolizer* symbolizer, const script_value& value, type_info_ptr constraint) {
		uint64_t value_type_id = symbolizer->intern(container_value_type_name(value));
		uint64_t expected_type_id = symbolizer->intern(type_info_display_name(constraint));
		return checked_result<void>(make_error_code(runtime_error_code::array_element_type_mismatch),
			"Cannot assign '{0}' to element of type '{1}'", value_type_id, expected_type_id);
	}

	// A field reference stores what the direct field store stores, with its text
	inline checked_result<script_value> ref_field_store(const auto& holder, const script_value& value, engine* eng) {
		const class_definition* owner = holder.owner_instance ? holder.owner_instance->get_class_definition() : nullptr;
		if (holder.owner_instance && holder.container_element_type &&
		    holder.container_element_type->base_type == script_value_type::jai_object_type) {
			if (const script_value* current = holder.field_cell()) {
				if (auto assigned = user_assign_operator(*current, value, eng, holder.container_element_type.get())) {
					if (!*assigned) { return assigned->error_value(); }
					return *current;
				}
			}
		}
		return field_store_convert(clone_for_assignment(value), holder.container_element_type, eng,
		                           holder.field_id == UINT64_MAX ? 0 : holder.field_id, owner);
	}

	JAI_NOINLINE inline error_propagator ref_return_mismatch(string_symbolizer* symbolizer, const script_value& actual, const type_info& return_type) {
		uint64_t got = symbolizer->intern(container_value_type_name(actual));
		uint64_t want = symbolizer->intern(return_type.type_name);
		return error_propagator{make_error_code(runtime_error_code::type_mismatch),
			"Cannot return reference to '{0}' from a function returning '{1}'", got, want};
	}

	// Typed reference-return epilogue (int& f() / -> auto&): the result passes through
	// as the reference HANDLE - the cell/element/field/map-entry pin is the lifetime,
	// so a returned reference outliving its frame is legal. Non-reference results
	// reject (a reference cannot bind a temporary). A declared referent type must match
	// the referent's current base type (no conversion can apply through a reference;
	// null referents pass - typed object refs stay nullable like typed fields do).
	inline checked_result<script_value> ref_return_pass_through(script_value&& result, type_info_ptr return_type,
	                                                            string_symbolizer* symbolizer, engine* eng) {
		if (!result.is_reference()) {
			return checked_result<script_value>(make_error_code(runtime_error_code::invalid_reference),
				"Function with reference return type must return an lvalue reference");
		}
		type_info_ptr referent = return_type->type_params.empty() ? nullptr : return_type->type_params[0];
		if (referent && referent->base_type != script_value_type::jai_any_type &&
		    referent->base_type != script_value_type::jai_null_type &&
		    referent->type_name != "auto") {
			const script_value& actual = result.deref();
			if (!reference_binding_admits(referent.get(), actual, eng)) {
				return ref_return_mismatch(symbolizer, actual, *return_type);
			}
		}
		return std::move(result);
	}

	// Write-through for a (pre-converted) value: TYPED elements store into the raw
	// buffer (a mutable deref would hand back the holder SCRATCH and the write would be
	// silently lost); every other mode keeps the exact deref()= line. Bounds text
	// matches deref's own re-resolve error.
	inline void ref_write_through(script_value& refLocal, script_value&& v) {
		const auto* holder = refLocal.get_reference_holder();
		if (holder && (holder->property || holder->component_base)) [[unlikely]] {
			// CPP-MEMBER writes the box's memory (a deref()= would replace the box and
			// detach); PROPERTY runs the setter and COMPONENT the component store (their deref is a scratch)
			refLocal.assign_through(std::move(v));
			return;
		}
		if (holder && holder->typed_element()) {
			if (holder->container_index >= holder->container->size()) {
				throw runtime_error("Reference to a removed array element");
			}
			holder->container->set(holder->container_index, std::move(v));
			return;
		}
		refLocal.deref() = std::move(v);
	}

	// A store into a variable's own storage (a cell's value, a frame slot) obeys the variable's declared type: the assignment
	// table and its text (the slot-less shape unless the caller names the slot, a store through `int& r`)
	template <class SiteFn>
	inline checked_result<void> variable_store_with(script_value& storage, script_value& value, engine* eng, SiteFn&& site_of) {
		if (storage.raw_storage_index() == script_value::TYPEID_CPP_BOUND) [[unlikely]] {   // a host-bound variable (add_global_ref): the C++ memory
			auto stored = typed_store_convert(clone_for_assignment(value.deref()), storage.get_type_info(), eng, site_of);
			if (!stored) { return stored.error_value(); }
			storage.assign_through(stored.value());
			return {};
		}
		type_info_ptr tag = storage.get_type_info();
		if (tag && tag->base_type == script_value_type::jai_shared_ptr_type && !tag->dynamic_pointee &&
			(value.raw_storage_index() == script_value::TYPEID_NULL ||
			 (value.raw_storage_index() == script_value::TYPEID_OBJECT && value.type_info_raw() == tag.get()))) {
			// a handle tagged with the slot's exact type (or null) shares as it is; a reference operand takes the path below
			script_value result = value;
			result.set_type_info(tag);
			storage = std::move(result);
			return {};
		}
		if (slot_value_assign_candidate(storage)) {   // the held object decides
			if (auto stored = slot_value_assign(storage, value, eng)) {
				if (!*stored) { return stored->error_value(); }
				if (stored->value() == value_store::rebind) {
					script_value v = clone_for_assignment(value);
					var_stamp(v, nullptr, eng);
					storage = std::move(v);
				}
				return {};
			}
		}
		if (tag && var_slot(tag.get())) {   // a var stays a var (a typed array keeps its element type)
			script_value v = clone_for_assignment(value);
			var_stamp(v, tag, eng);
			storage = std::move(v);
			return {};
		}
		if (tag && tag->base_type == script_value_type::jai_object_type) {
			if (auto assigned = user_assign_operator(storage, value, eng)) {
				if (!*assigned) { return assigned->error_value(); }
				return {};
			}
		}
		if (tag && tag->base_type != script_value_type::jai_reference_type) {
			// the value copy first (value semantics: an array store never aliases its source), then the variable's table
			auto stored = typed_store_convert(clone_for_assignment(value), tag, eng, site_of);
			if (!stored) { return stored.error_value(); }
			script_value v = std::move(stored.value());
			v.set_type_info(tag);
			storage = std::move(v);
			return {};
		}
		storage = clone_for_assignment(value);
		return {};
	}
	inline checked_result<void> variable_store(script_value& storage, script_value& value, engine* eng, slot_site site = {}) {
		return variable_store_with(storage, value, eng, [site]() { return site; });
	}

	// The declared type of a reference (`int& r = v`, cast<int&>(v)): null for var&/auto& and every minted reference
	inline type_info* ref_declared_type(const script_value& ref) {
		const type_info* t = ref.type_info_raw();
		return t ? t->declared_referent : nullptr;
	}

	// The storage behind the reference applies T's table itself (a typed element of T's kind, a variable declared T): the declared
	// type adds nothing, and the storage's own rule and text stand
	inline bool ref_storage_enforces(const script_value& ref, const type_info* declared) {
		const auto* holder = ref.get_reference_holder();
		if (!holder || holder->property || holder->component_base) { return false; }
		const type_info* enforced = holder->container_element_type ? holder->container_element_type.get()
		                          : holder->plain_cell() ? holder->cell()->type_info_raw() : nullptr;
		if (!enforced || var_slot(enforced)) { return false; }
		const uint8_t kind = declared_ref_kind(declared);
		return kind && kind != 0xFF && declared_ref_kind(enforced) == kind;
	}

	// null or a handle tagged T stored through a shared_ptr<T>& into a variable or field declared exactly T: T's step is the
	// identity (typed_store_convert's handle head) and the storage's own step, the same table, stamps T
	inline bool ref_identity_handle_store(const script_value& ref, const type_info* declared, const script_value& value) {
		if (!declared || declared->base_type != script_value_type::jai_shared_ptr_type || declared->dynamic_pointee) { return false; }
		const size_t vi = value.raw_storage_index();
		if (vi != script_value::TYPEID_NULL && !(vi == script_value::TYPEID_OBJECT && value.type_info_raw() == declared)) { return false; }
		const auto* holder = ref.get_reference_holder();
		if (!holder || holder->property || holder->component_base) { return false; }
		if (holder->owner_instance) { return holder->container_element_type.get() == declared; }
		return !holder->container_element_type && holder->plain_cell() && holder->cell()->type_info_raw() == declared;
	}

	// The value a declared reference names, a variable's cell read directly (no out-of-line dereference)
	inline const script_value& ref_declared_current(const script_value& ref) {
		const auto* holder = ref.get_reference_holder();
		return holder && holder->plain_cell() ? *holder->cell() : ref.deref();
	}

	// Stamps the declared type where the storage would not already apply T's table (a var, an untyped element), so a store
	// through a reference to typed storage keeps its fast paths
	inline void ref_declare(script_value& ref, type_info* declared, engine* eng) {
		if (!declared || !ref.is_reference() || declared->base_type == script_value_type::jai_any_type ||
		    declared->base_type == script_value_type::jai_reference_type || declared->type_name == "auto") {
			return;
		}
		const auto* holder = ref.get_reference_holder();
		if (!holder || holder->property || holder->component_base) { return; }
		const type_info* enforced = holder->container_element_type ? holder->container_element_type.get()
		                          : holder->plain_cell() ? holder->cell()->type_info_raw() : nullptr;
		if (enforced && !var_slot(enforced) && enforced->base_type == declared->base_type) { return; }
		type_info* twin = declared->cached_declared_reference_type;
		ref.set_type_info(twin ? twin : engine_access::get_type_info_declared_reference(*eng, declared));
	}

	// The declared type is decided at the store: a value already of T's kind passes T's table unchanged, so the storage's own
	// path stores it with no extra work
	inline bool ref_declared_matches(const type_info* declared, const script_value& v) {
		switch (v.raw_storage_index()) {
			case script_value::TYPEID_INT: return declared->base_type == script_value_type::jai_int_type && !declared->is_byte_element();
			case script_value::TYPEID_FLOAT: return declared->base_type == script_value_type::jai_float_type;
			case script_value::TYPEID_BOOL: return declared->base_type == script_value_type::jai_bool_type;
			case script_value::TYPEID_STRING: return declared->base_type == script_value_type::jai_string_type;
			default: return false;
		}
	}

	// The hot form of the store-time decision, from the reference value's own stamp: 0 = no declared type, else whether the value
	// is of another kind than T's
	inline uint8_t ref_declared_kind(const script_value& ref) {
		const type_info* t = ref.type_info_raw();
		return t ? t->declared_kind : uint8_t{0};
	}

	// x op= v through a declared reference whose value has T's kind and keeps it (int op int, float op number, string += string or
	// char): the storage's in-place path applies
	inline bool ref_declared_compound_keeps(uint8_t kind, const script_value& current, const script_value& right, token_type base_op) {
		const size_t ci = current.raw_storage_index();
		const size_t ri = right.raw_storage_index();
		if (kind == script_value::TYPEID_INT) {
			return ci == script_value::TYPEID_INT && ri == script_value::TYPEID_INT;
		}
		if (kind == script_value::TYPEID_STRING) {
			return base_op == token_type::plus && ci == script_value::TYPEID_STRING && (ri == script_value::TYPEID_STRING || ri == script_value::TYPEID_CHAR);
		}
		if (kind == script_value::TYPEID_FLOAT) {
			return ci == script_value::TYPEID_FLOAT && (ri == script_value::TYPEID_INT || ri == script_value::TYPEID_FLOAT) &&
			       (base_op == token_type::plus || base_op == token_type::minus || base_op == token_type::star ||
			        base_op == token_type::slash || base_op == token_type::percent);
		}
		return false;
	}

	// A store through a reference with a declared type T runs T's table first (`var v; int& r = v;
	// r = 2.5` stores int 2), a class T its operator= on the referent; the storage's own rules follow. nullopt when the
	// operator wrote it.
	template <class SiteFn>
	inline std::optional<checked_result<script_value>> ref_declared_convert_with(const script_value& refLocal, type_info* declared,
	                                                                             const script_value& value, engine* eng, SiteFn&& site_of) {
		if (declared->base_type == script_value_type::jai_object_type) {
			auto current = ref_checked_deref(refLocal);
			if (current) {
				const auto* holder = refLocal.get_reference_holder();
				const type_info* storage = holder && holder->container_element_type ? holder->container_element_type.get() : nullptr;
				if (auto assigned = user_assign_operator(current.value(), value, eng, storage, declared)) {
					if (!*assigned) { return checked_result<script_value>(assigned->error_value()); }
					return std::nullopt;
				}
			}
		}
		const script_value& plain = value.deref();
		if (ref_declared_matches(declared, plain)) { return script_value(plain); }
		return typed_store_convert(clone_for_assignment(plain), type_info_ptr{declared}, eng, site_of);
	}
	inline std::optional<checked_result<script_value>> ref_declared_convert(const script_value& refLocal, type_info* declared,
	                                                                        const script_value& value, engine* eng, slot_site site = {}) {
		return ref_declared_convert_with(refLocal, declared, value, eng, [site]() { return site; });
	}

	template <class SiteFn>
	inline checked_result<void> ref_store_through_storage_with(script_value& refLocal, script_value& value, engine* eng, string_symbolizer* symbolizer,
	                                                           SiteFn&& site_of);


	// Identifier-assignment through a reference local: unconstrained refs keep today's
	// exact clone-through-deref line; constrained element/field refs enforce like the
	// subscript-assign paths do.
	// name: the reference variable a store names (`ry = v`), so a rejection reads `Cannot assign string to 'ry' (declared int&)`
	// NameOf: the variable's name, read only by a rejection
	template <class NameOf>
	inline checked_result<void> ref_store_through_named(script_value& refLocal, script_value& value, engine* eng, string_symbolizer* symbolizer,
	                                                    type_info* declared_static, uint8_t static_kind, NameOf&& name_of) {
		auto site_of = [&name_of]() { const std::string_view name = name_of(); return slot_site{slot_kind::assignment, name, {}, !name.empty()}; };
		if (const uint8_t kind = static_kind ? static_kind : ref_declared_kind(refLocal);
		    kind && kind != value.deref().raw_storage_index() && !ref_storage_enforces(refLocal, static_kind ? declared_static : ref_declared_type(refLocal)) &&
		    !ref_identity_handle_store(refLocal, static_kind ? declared_static : ref_declared_type(refLocal), value)) [[unlikely]] {
			auto converted = ref_declared_convert_with(refLocal, static_kind ? declared_static : ref_declared_type(refLocal), value, eng, site_of);
			if (!converted) { return {}; }
			if (!*converted) { return converted->error_value(); }
			return ref_store_through_storage_with(refLocal, converted->value(), eng, symbolizer, site_of);
		}
		return ref_store_through_storage_with(refLocal, value, eng, symbolizer, site_of);
	}
	inline checked_result<void> ref_store_through(script_value& refLocal, script_value& value,
	                                              engine* eng, string_symbolizer* symbolizer,
	                                              type_info* declared_static = nullptr, uint8_t static_kind = 0, std::string_view name = {}) {
		return ref_store_through_named(refLocal, value, eng, symbolizer, declared_static, static_kind, [name]() { return name; });
	}
	inline checked_result<void> ref_store_through_storage(script_value& refLocal, script_value& value, engine* eng, string_symbolizer* symbolizer,
	                                                      slot_site site = {}) {
		return ref_store_through_storage_with(refLocal, value, eng, symbolizer, [site]() { return site; });
	}

	template <class SiteFn>
	inline checked_result<void> ref_store_through_storage_with(script_value& refLocal, script_value& value, engine* eng, string_symbolizer* symbolizer,
	                                                           SiteFn&& site_of) {
		const auto* holder = refLocal.get_reference_holder();
		if (holder && holder->plain_cell() && !holder->container_element_type) {
			return variable_store_with(refLocal.deref(), value, eng, site_of);
		}
		if (!holder || !holder->container_element_type) {
			ref_write_through(refLocal, clone_for_assignment(value));
			return {};
		}
		type_info_ptr constraint = holder->container_element_type;
		if (holder->owner_instance) {
			auto stored = ref_field_store(*holder, value, eng);
			if (!stored) { return stored.error_value(); }
			ref_write_through(refLocal, std::move(stored.value()));
			return {};
		}
		if (auto in_place = element_copy_assign(refLocal.deref(), value, constraint, eng)) { return *in_place; }   // a class element keeps its object
		if (!container_element_compatible(value, constraint, eng)) {
			return ref_element_error(symbolizer, value, constraint);
		}
		auto converted = ref_convert_for_element(eng, value, constraint);
		if (!converted) { return converted.error_value(); }
		ref_write_through(refLocal, std::move(converted.value()));
		return {};
	}

	// `x += text` through a field, element or map-entry reference whose storage still holds the string the compound read (`current`,
	// released here): the bytes append to that storage's own node under the concatenation's charge, a node another holder shares copied
	// first, leaving what the computed concatenation and the reference's store would leave (the store's identity rows: a string, var
	// or untyped slot; a declared field stamps its type). Both backends run it, so it keeps their parity by construction. nullopt for
	// any other shape: a property, a string element or a component, a constraint that converts, storage holding another value.
	inline std::optional<checked_result<script_value>> ref_text_append_in_place(script_value& refLocal, script_value& current, const script_value& right,
	                                                                           type_info* declared_static, engine* eng) {
		const size_t ri = right.raw_storage_index();
		if (!eng || current.raw_storage_index() != script_value::TYPEID_STRING || (ri != script_value::TYPEID_STRING && ri != script_value::TYPEID_CHAR)) {
			return std::nullopt;
		}
		auto* holder = refLocal.get_reference_holder();
		if (!holder || holder->property || holder->string_element() || holder->component_element() || holder->has_cell) { return std::nullopt; }
		auto texty = [](const type_info* t) {
			return !t || t->base_type == script_value_type::jai_any_type || t->base_type == script_value_type::jai_string_type;
		};
		const type_info* constraint = holder->container_element_type.get();
		if (!texty(constraint) || !texty(declared_static ? declared_static : ref_declared_type(refLocal))) { return std::nullopt; }
		strong_ptr<script_string>* handle = nullptr;
		script_value* target = nullptr;
		if (holder->typed_element()) {
			if (holder->container->kind() != script_array::kind_t::str || holder->container_index >= holder->container->size()) { return std::nullopt; }
			handle = &holder->container->strs()[holder->container_index];
			if (!*handle || handle->get() != current.unchecked_string_handle().get()) { return std::nullopt; }
		} else {
			target = holder->owner_instance ? holder->field_cell() : holder->resolve_target();
			if (!target || target->raw_storage_index() != script_value::TYPEID_STRING ||
			    target->unchecked_string_handle().get() != current.unchecked_string_handle().get()) {
				return std::nullopt;
			}
		}
		execution_limits& limits = engine_access::execution_limits(*eng);
		const size_t added = ri == script_value::TYPEID_CHAR ? 1 : right.unchecked_as_string().size();
		if (!limits.memory_charge(sizeof(script_value) + current.unchecked_as_string().size() + added)) [[unlikely]] {
			return checked_result<script_value>(raise_memory_cap(limits));
		}
		const std::string appended = ri == script_value::TYPEID_CHAR ? std::string(1, right.unchecked_as_char()) : std::string();
		const std::string& text = ri == script_value::TYPEID_CHAR ? appended : right.unchecked_as_string();
		current = script_value(std::monostate{}, eng);
		if (holder->has_elem_scratch) { *holder->cell() = script_value(std::monostate{}, eng); }   // the scratch's hold on the node released too
		strong_ptr<script_string>& node = handle ? *handle : target->unchecked_get_string_storage();
		if (!node.unique()) { node = make_strong<script_string>(*node); }   // the concatenation's copy, which its charge counts
		if (&text == node.get()) { node->append(std::string(text)); } else { node->append(text); }
		if (handle) { return script_value(*handle, eng); }
		type_info* tag = holder->owner_instance && constraint && constraint->base_type == script_value_type::jai_string_type
			? const_cast<type_info*>(constraint) : engine_access::get_type_info_string(*eng);
		if (target->type_info_raw() != tag) { target->set_type_info(type_info_ptr{tag}); }
		return script_value(*target);
	}

	// Constrained-ref compound assignment (x op= v where x is bound to a typed element
	// or field): mirrors the subscript-compound semantics - custom operator first, then
	// arithmetic, then constraint check + convert + write-through. `arith` is the
	// backend's evaluate_arithmetic. Returns the (pre-conversion) expression result.
	// The operator consult reads the engine's flat table (operator_table.hpp) - no
	// environment probe.
	template <typename Arith>
	inline checked_result<script_value> ref_compound_store_constrained(
			script_value& refLocal, const script_value& rightArg, token_type op, const char* opName,
			const engine_operator_table* op_table, engine* eng, string_symbolizer* symbolizer, Arith&& arith,
			type_info* declared_static = nullptr) {
		// Current-value read raw-derefs exactly like the unconstrained compound path: a
		// removed element/field surfaces as the same caller-frame-catchable throw whether
		// or not the bound container was typed
		script_value currentValue = refLocal.deref();
		const script_value& rightDeref = rightArg.deref();
		// a bound rhs decodes up front so the /= and %= zero pre-checks below keep
		// today's error codes/texts (shared by both backends; `arith` normalizes bound lhs)
		const bool rightBound = rightDeref.raw_storage_index() == script_value::TYPEID_CPP_BOUND;
		const script_value rightNorm = rightBound ? rightDeref.bound_decoded_temp() : script_value(std::monostate{}, eng);
		const script_value& rightValue = rightBound ? rightNorm : rightDeref;
		script_value resultValue(std::monostate{}, eng);
		const script_value* opFunc = op_table ? op_table->entry(binary_op_slot(op)) : nullptr;
		if (!opFunc && op == token_type::plus) {
			if (auto appended = ref_text_append_in_place(refLocal, currentValue, rightValue, declared_static, eng)) { return std::move(*appended); }
		}
		if (opFunc) {
			const script_function& func = opFunc->as_function();
			std::vector<script_value> args = {currentValue, rightValue};
			auto result = func(args);
			if (!result) {
				return result;
			}
			resultValue = std::move(result.value());
		} else {
			switch (op) {
				case token_type::plus: {
					// one '+=' table for every destination: a text target appends text, a text operand never stringifies
					if (compound_plus_text(currentValue.deref(), rightValue) == compound_text::mismatch) {
						return compound_mismatch(0, currentValue.deref(), rightValue, eng);
					}
					auto r = arith(currentValue, token_type::plus, rightValue);
					if (!r) { return r; }
					resultValue = std::move(r.value());
					break;
				}
				case token_type::minus:
				case token_type::star:
				case token_type::ampersand: case token_type::pipe: case token_type::caret:
				case token_type::left_shift: case token_type::right_shift: {
					auto r = arith(currentValue, op, rightValue);
					if (!r) { return r; }
					resultValue = std::move(r.value());
					break;
				}
				case token_type::slash: {
					if (math::integer_zero_divisor(currentValue, rightValue)) {
						return checked_result<script_value>(make_error_code(runtime_error_code::division_by_zero), "Division by zero");
					}
					auto r = arith(currentValue, token_type::slash, rightValue);
					if (!r) { return r; }
					resultValue = std::move(r.value());
					break;
				}
				case token_type::percent: {
					if (math::integer_zero_divisor(currentValue, rightValue)) {
						return checked_result<script_value>(make_error_code(runtime_error_code::modulo_by_zero), "Modulo by zero");
					}
					auto r = arith(currentValue, token_type::percent, rightValue);
					if (!r) { return r; }
					resultValue = std::move(r.value());
					break;
				}
				default:
					return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation),
						"Unknown compound assignment operator");
			}
		}
		if (type_info* declared = declared_static ? declared_static : ref_declared_type(refLocal); declared && !ref_storage_enforces(refLocal, declared)) [[unlikely]] {
			auto converted = ref_declared_convert(refLocal, declared, resultValue, eng);
			if (!converted) { return resultValue; }
			if (!*converted) { return converted->error_value(); }
			auto stored = ref_store_through_storage(refLocal, converted->value(), eng, symbolizer);
			if (!stored) { return stored.error_value(); }
			return resultValue;
		}
		const auto* holder = refLocal.get_reference_holder();
		type_info_ptr constraint = holder ? holder->container_element_type : nullptr;
		if (holder && holder->plain_cell() && !constraint) {
			auto stored = variable_store(refLocal.deref(), resultValue, eng);
			if (!stored) { return stored.error_value(); }
			return resultValue;
		}
		if (holder && holder->owner_instance) {
			auto stored = ref_field_store(*holder, resultValue, eng);
			if (!stored) { return stored.error_value(); }
			ref_write_through(refLocal, std::move(stored.value()));
			return resultValue;
		}
		if (auto in_place = element_copy_assign(refLocal.deref(), resultValue, constraint, eng)) {
			if (!*in_place) { return in_place->error_value(); }
			return resultValue;
		}
		if (!container_element_compatible(resultValue, constraint, eng)) {
			return ref_element_error(symbolizer, resultValue, constraint).error_value();
		}
		auto converted = ref_convert_for_element(eng, resultValue, constraint);
		if (!converted) { return converted.error_value(); }
		ref_write_through(refLocal, std::move(converted.value()));
		return resultValue;
	}

	inline script_value ref_detached_value(script_value v) {
		if (v.raw_storage_index() == script_value::TYPEID_CPP_BOUND) { return v.bound_decoded_temp(); }
		return clone_for_assignment(v);
	}

	// x++ and x--: x += 1 on an int, a float or a char (which promotes, so the result is its int); the stepped value, or the
	// operator's error on any other kind
	inline checked_result<script_value> incdec_step(const script_value& current_in, bool increment, engine* eng) {
		const script_value& current = current_in.deref();
		switch (current.raw_storage_index()) {
			case script_value::TYPEID_CPP_BOUND: {
				const script_value decoded = current.bound_decoded_temp();
				if (decoded.raw_storage_index() == script_value::TYPEID_CPP_BOUND) { return incdec_mismatch(increment, current, eng); }
				return incdec_step(decoded, increment, eng);
			}
			case script_value::TYPEID_INT: {
				script_int out;
				if (increment ? !ints::try_add(current.unchecked_as_int(), 1, out) : !ints::try_sub(current.unchecked_as_int(), 1, out)) {
					return checked_result<script_value>(make_error_code(runtime_error_code::invalid_numeric_operand),
						compound_overflow_text(increment ? k_compound_increment : k_compound_decrement));
				}
				return script_value(out, eng);
			}
			case script_value::TYPEID_FLOAT: return script_value(current.unchecked_as_float() + (increment ? 1.0 : -1.0), eng);
			case script_value::TYPEID_CHAR: {
				const script_int code = static_cast<unsigned char>(current.unchecked_as_char());
				return script_value(code + (increment ? 1 : -1), eng);
			}
			default: return incdec_mismatch(increment, current, eng);
		}
	}

	// the value a postfix ++ or -- yields: the number as it was (a bound variable's decoded), untagged
	inline script_value incdec_old_value(const script_value& current_in, engine* eng) {
		const script_value& current = current_in.deref();
		switch (current.raw_storage_index()) {
			case script_value::TYPEID_CPP_BOUND: return incdec_old_value(current.bound_decoded_temp(), eng);
			case script_value::TYPEID_INT: return script_value(current.unchecked_as_int(), eng);
			case script_value::TYPEID_FLOAT: return script_value(current.unchecked_as_float(), eng);
			case script_value::TYPEID_CHAR: return script_value(current.unchecked_as_char(), eng);
			default: return current;
		}
	}

	// cast<T&>(e) = v, op= v, ++ and --: a store through the reference the target evaluated to, by the rules of the storage
	// it names (a variable's declared type, a typed element, a field, a static, a property's setter, a component). `op` is
	// the compound's base operator, or token_type::equal. The expression's value: the old value for a postfix, else the
	// stored value read back when consumed, else what was stored.
	template <typename Arith>
	inline checked_result<script_value> ref_target_assign(script_value& ref, const script_value& value, token_type op, bool incdec,
	                                                      bool old_result, bool result_needed, const engine_operator_table* op_table,
	                                                      engine* eng, string_symbolizer* symbolizer, Arith&& arith,
	                                                      type_info* declared_static = nullptr) {
		script_value outcome(std::monostate{}, eng);
		if (op == token_type::equal) {
			script_value incoming = value.deref();
			auto stored = ref_store_through(ref, incoming, eng, symbolizer, declared_static, declared_ref_kind(declared_static));
			if (!stored) { return stored.error_value(); }
			if (!result_needed) { return incoming; }
		} else if (incdec) {
			auto current = ref_checked_deref(ref);
			if (!current) { return current; }
			auto stepped = incdec_step(current.value(), op == token_type::plus, eng);
			if (!stepped) { return stepped; }
			if (old_result) { outcome = ref_detached_value(std::move(current.value())); }
			auto stored = ref_store_through(ref, stepped.value(), eng, symbolizer, declared_static, declared_ref_kind(declared_static));
			if (!stored) { return stored.error_value(); }
			if (old_result) { return outcome; }
			if (!result_needed) { return std::move(stepped.value()); }
		} else {
			auto current = ref_checked_deref(ref);
			if (!current) { return current; }
			if (old_result) { outcome = ref_detached_value(std::move(current.value())); }
			auto result = ref_compound_store_constrained(ref, value, op, binary_op_name(op), op_table, eng, symbolizer, arith, declared_static);
			if (!result) { return result; }
			if (old_result) { return outcome; }
			if (!result_needed) { return result; }
		}
		auto back = ref_checked_deref(ref);
		if (!back) { return back; }
		return ref_detached_value(std::move(back.value()));
	}

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_REF_LVALUE_HPP__
