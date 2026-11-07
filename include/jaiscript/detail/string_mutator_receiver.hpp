#pragma once

#ifndef __JAISCRIPT_DETAIL_STRING_MUTATOR_RECEIVER_HPP__
#define __JAISCRIPT_DETAIL_STRING_MUTATOR_RECEIVER_HPP__

// Receiver binding for the in-place string builtins (k_string_mutator_method_names),
// shared VERBATIM by both backends. C++ `string& to_upper()`: the receiver binds like a
// reference parameter and the write lands in that storage - a subscript read or ref
// alias (the popped receiver IS a reference) or a field / C++ property (resolved through
// the ref-lvalue kernel, exactly how `f(obj.field)` binds a `string&` parameter). Anything
// else is a temporary (a mutator returns nothing, so a call result never names storage):
// the builtin mutates the popped copy, and the copy-on-write gate in unchecked_as_string_ref keeps that write out of
// every other holder. Identifier receivers never come here - each backend resolves them
// to the variable cell on its own fast path.

#include <jaiscript/detail/ref_lvalue.hpp>

namespace jai::detail {

	struct string_mutator_binding {
		script_value pin;   // the reference pinning the storage (null = unbound)

		explicit string_mutator_binding(engine* eng) : pin(std::monostate{}, eng) {}

		// The storage behind the pin: reference chains resolve to their final referent (a
		// PROPERTY scratch may itself hold a map-entry reference - `kv.second`)
		script_value* pinned_storage() {
			if (!pin.is_reference()) { return nullptr; }
			script_value* p = &pin.deref();
			while (p->is_reference()) { p = &p->deref(); }
			return p;
		}
		// Scratch-backed pins (typed element, PROPERTY) write the mutated scratch back
		void commit() {
			if (auto* sr = pin.scratch_ref_holder()) { sr->commit_scratch(); }
		}
	};

	// Binds the receiver of a string mutator on a non-identifier receiver expression.
	// `receiver` is the evaluated receiver as the call site popped it (a reference for
	// subscript reads and ref aliases - moved into the pin - a copy for member reads and
	// call results). Pure resolution over already-evaluated state: never runs script,
	// never throws.
	inline string_mutator_binding bind_string_mutator_receiver(script_value& receiver, const expression* receiver_expr,
	                                                          const caller_frame_view& caller, environment* caller_env,
	                                                          engine* eng, string_symbolizer* symbolizer) {
		string_mutator_binding b(eng);
		if (receiver.is_reference()) {
			b.pin = std::move(receiver);
			return b;
		}
		if (is_member_final_ref_lvalue(receiver_expr)) {
			auto resolved = resolve_ref_lvalue(receiver_expr, caller, caller_env, eng, symbolizer);
			if (resolved && resolved.value().is_reference()) { b.pin = std::move(resolved.value()); }
		}
		return b;   // a call result, a computed property, a non-lvalue verdict: the copy is a temporary
	}

	// The storage self binds to: the pinned storage when its referent is a string of the receiver's kind, else null - the
	// caller then mutates its copy
	inline script_value* string_mutator_self(string_mutator_binding& b, const script_value& receiver_copy) {
		script_value* storage = b.pinned_storage();
		if (!storage || storage->is_utf8_string() != receiver_copy.is_utf8_string() ||
		    !(storage->is_string() || storage->is_utf8_string())) { return nullptr; }
		return storage;
	}

	// A handle's reset() (C++ `p.reset()`) binds its receiver the same way and writes null into it, keeping the storage's type
	// tag: an identifier receiver's variable cell (ident_cell, resolved by the backend), a reference, field or property
	// receiver's pinned storage. A call result is a temporary the reset leaves as it is.
	inline script_value* handle_reset_storage(string_mutator_binding& b, script_value* ident_cell) {
		if (ident_cell) { return &ident_cell->deref(); }
		return b.pinned_storage();
	}

	inline bool is_resettable_handle(const script_value& v) noexcept {
		if (v.raw_storage_index() == script_value::TYPEID_WEAK_PTR) { return true; }
		return v.get_type_info() && v.get_type_info()->base_type == script_value_type::jai_shared_ptr_type;
	}

	// A call of reset() on this receiver value is the handle's: a weak_ptr, or a live object held through shared_ptr<T> whose
	// class declares no reset() of its own (a class's method wins over the builtin)
	inline bool handle_reset_applies(const script_value& v, uint64_t reset_id) {
		if (v.raw_storage_index() == script_value::TYPEID_WEAK_PTR) { return true; }
		if (v.is_null()) { return is_resettable_handle(v); }   // an empty shared_ptr<T> resets as C++'s does
		if (!is_resettable_handle(v)) { return false; }
		if (auto instance = const_cast<script_value&>(v).get_class_instance()) {
			if (const class_definition* cd = instance->get_class_definition(); cd && cd->defines_method(reset_id)) { return false; }
		}
		return true;
	}

	inline void reset_handle_storage(script_value* storage, engine* eng) {
		if (!storage || !is_resettable_handle(*storage)) { return; }
		if (storage->raw_storage_index() == script_value::TYPEID_WEAK_PTR) {   // an empty weak_ptr, as the weak builtin leaves it
			storage->get_weak_ptr_storage().reset();
			return;
		}
		const type_info_ptr tag = storage->get_type_info();
		*storage = script_value(std::monostate{}, eng);
		storage->set_type_info(tag);
	}

	// The failing texts of p.reset(h), built on the failing path alone
	JAI_NOINLINE inline checked_result<void> reset_argument_not_handle(const script_value& arg, engine* eng) {
		return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
			"reset(h) takes a handle (new T(), make_shared<T>() or a shared_ptr), not {0}", engine_access::get_symbolizer(*eng)->intern(slot_source_type_name(arg, eng)));
	}
	JAI_NOINLINE inline checked_result<void> reset_class_refused(const std::string& held, const script_value& arg, engine* eng) {
		string_symbolizer* symbols = engine_access::get_symbolizer(*eng);
		return checked_result<void>(make_error_code(runtime_error_code::type_mismatch), "Cannot reset shared_ptr<{0}> to a shared_ptr<{1}>",
			symbols->intern(held), symbols->intern(shared_handle_judged_class(arg, eng)));
	}

	// p.reset(h), C++ p.reset(new T()): the receiver's storage takes h's handle (an empty one when h is null), and the object p held
	// stays with its other owners; nothing writes through it and no operator= runs. h is a handle whose class the receiver admits
	// as a shared_ptr<T> assignment does (an upcast, never a downcast or an unrelated class); a var holding a handle admits the
	// class of the object it holds. The receiver keeps its own tag (a var stays rebindable). A receiver that names no storage
	// (a call result) is a temporary: nothing is stored
	inline checked_result<void> reset_handle_to(script_value* storage, const script_value& receiver, const script_value& arg_in, engine* eng) {
		const script_value& arg = arg_in.is_reference() ? arg_in.deref() : arg_in;
		if (arg.is_null()) {
			reset_handle_storage(storage, eng);
			return {};
		}
		const type_info* arg_tag = arg.type_info_raw();
		if (arg.raw_storage_index() != script_value::TYPEID_OBJECT || !arg_tag || arg_tag->base_type != script_value_type::jai_shared_ptr_type) {
			return reset_argument_not_handle(arg, eng);
		}
		const script_value& held = storage ? *storage : receiver;
		const type_info_ptr tag = held.get_type_info();
		if (tag && !tag->dynamic_pointee) {
			if (!shared_handle_admits(arg, tag, eng)) {
				const type_info* pointee = tag->element_type().get();
				return reset_class_refused(pointee ? pointee->type_name : std::string(), arg, eng);
			}
		} else if (!held.is_null()) {
			const std::string held_class = shared_handle_judged_class(held, eng);
			const std::string* judged = shared_handle_judged_class_name(arg, eng);
			if (!held_class.empty() && judged && *judged != held_class) {
				auto judged_def = engine_access::get_class_definition(*eng, *judged);
				if (judged_def && !judged_def->is_subtype_of(held_class)) { return reset_class_refused(held_class, arg, eng); }
			}
		}
		if (!storage) { return {}; }
		script_value repointed = arg;   // the handle itself: the new object is shared, never copied
		if (tag && !tag->dynamic_pointee) {
			repointed.set_type_info(tag);
		} else if (!arg_tag->dynamic_pointee && arg_tag->element_type()) {
			repointed.set_type_info(engine_access::get_type_info_shared_ptr_dynamic(*eng, arg_tag->element_type().get()));
		}
		*storage = std::move(repointed);
		return {};
	}

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_STRING_MUTATOR_RECEIVER_HPP__
