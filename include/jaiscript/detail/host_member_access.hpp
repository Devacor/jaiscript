#pragma once
#ifndef __JAISCRIPT_DETAIL_HOST_MEMBER_ACCESS_HPP__
#define __JAISCRIPT_DETAIL_HOST_MEMBER_ACCESS_HPP__

#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/host_member.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>

namespace jai::detail {

// The C++ object an instance holds (a bound class's wrapper, a constructed hybrid, an extended object's layer): its holder
// (the registered type it is stored under) and address
inline const script_value::object_holder* host_member_stored(const class_instance* inst, const void*& stored) {
	const script_value* native = inst->native();
	if (!native || native->raw_storage_index() != script_value::TYPEID_OBJECT) { return nullptr; }
	const auto& holder = native->unchecked_object_handle();
	if (!holder) { return nullptr; }
	stored = holder->holds_object() ? holder->object() : holder->bound_object();
	return stored ? holder.get() : nullptr;
}

// The instance a method's receiver (this) holds, read raw: null for anything but a class instance
inline class_instance* host_this_instance(const script_value& self) {
	const size_t kind = self.raw_storage_index();
	if (kind != script_value::TYPEID_OBJECT) { return nullptr; }
	const auto& holder = const_cast<script_value&>(self).get_object_holder();
	if (!holder || !holder->is_instance()) { return nullptr; }
	return holder->unchecked_instance();
}

// A script class of a layer's chain answers the name before its C++ base: a script field (not a C++ property's
// placeholder), a user _get_/_set_ accessor or a script method of the name
inline bool host_layer_answers(const class_definition* cd, uint64_t member_id, engine* eng) {
	const uint32_t slot = cd->field_slot(member_id);
	if (slot != class_definition::k_no_field_slot && !cd->slot_is_cpp_property(slot)) { return true; }
	auto* symbols = engine_access::get_symbolizer(*eng);
	if (cd->script_declares_user_accessor(symbols->get_getter_id_with_view(member_id).first) ||
	    cd->script_declares_user_accessor(symbols->get_setter_id_with_view(member_id).first)) {
		return true;
	}
	return cd->script_chain_defines_method(member_id);
}

// The layout an instance's object has now, for a lane bound on `owner`: the stored type, its vptr when polymorphic, the offset
// to owner's part, and for a plain wrapper of a script-extensible type the offset to its slot. False when the object cannot
// reach owner's part.
inline bool host_receiver_measure(const class_definition* cd, const class_instance* inst, const class_definition* owner,
                                  engine* eng, const void*& stored, std::ptrdiff_t& owner_delta, host_receiver_layout& out) {
	const script_value::object_holder* holder = host_member_stored(inst, stored);
	if (!holder || holder->type_id() == UINT64_MAX) { return false; }
	auto stored_def = engine_access::get_class_definition(*eng, holder->type_id());
	if (!stored_def || !stored_def->is_cpp_class()) { return false; }
	owner_delta = 0;
	if (owner && holder->type_id() != owner->get_type_id()) {
		if (!owner->cpp_type()) { return false; }
		void* adjusted = stored_def->adjust_cpp_pointer(const_cast<void*>(stored), *owner->cpp_type());
		if (!adjusted) { return false; }
		owner_delta = static_cast<const char*>(adjusted) - static_cast<const char*>(stored);
	}
	out.stored_type = holder->type_id();
	out.vptr = stored_def->cpp_polymorphic() ? *static_cast<const void* const*>(stored) : nullptr;
	out.slot_offset = k_no_slot_check;
	if (!cd->is_script_class()) {
		if (const auto* hooks = stored_def->slot_hooks(); hooks && hooks->slot) {
			const std::ptrdiff_t offset = reinterpret_cast<const char*>(hooks->slot(stored)) - static_cast<const char*>(stored);
			if (offset < INT32_MIN + 1 || offset > INT32_MAX) { return false; }
			out.slot_offset = static_cast<int32_t>(offset);
		}
	}
	return true;
}

// The receiver's object when it still has the layout: the instance is no retired layer, the stored type and vptr match,
// and a plain wrapper's object carries no layer. Null otherwise.
inline const char* host_receiver_object(const class_instance* inst, const host_receiver_layout& layout) {
	if (inst->script_layer_retired()) { return nullptr; }
	const void* stored = nullptr;
	const script_value::object_holder* holder = host_member_stored(inst, stored);
	if (!holder || holder->type_id() != layout.stored_type) { return nullptr; }
	if (layout.vptr && *static_cast<const void* const*>(stored) != layout.vptr) { return nullptr; }
	const char* object = static_cast<const char*>(stored);
	if (layout.slot_offset != k_no_slot_check &&
	    reinterpret_cast<const script_extensible*>(object + layout.slot_offset)->has_script_layer()) {
		return nullptr;
	}
	return object;
}

// A member's lane for this receiver when it may replace the bound accessors: a bound C++ class's wrapper, or a layer (an
// extended object, a constructed hybrid) whose script chain does not answer the name itself, on an all-public chain, whose
// member has a lane the receiver's object reaches. KEEP BYTE-PARALLEL: both backends fill through here and read through
// host_member_address.
inline bool host_member_fill(class_definition* cd, const class_instance* inst, uint64_t member_id,
                             const script_value& receiver, engine* eng, host_member_hit& out) {
	out = host_member_hit{};
	if (cd->chain_has_nonpublic()) { return false; }
	if (cd->is_script_class()) {
		if (inst->script_layer_retired() || host_layer_answers(cd, member_id, eng)) { return false; }
	} else if (!cd->is_cpp_class()) {
		return false;
	}
	const host_member_lane* lane = cd->host_member_lane_for(member_id);
	if (!lane || !lane->locate) { return false; }
	const void* stored = nullptr;
	std::ptrdiff_t owner_delta = 0;
	host_receiver_layout layout;
	if (!host_receiver_measure(cd, inst, nullptr, eng, stored, owner_delta, layout)) { return false; }
	if (layout.slot_offset != k_no_slot_check &&
	    reinterpret_cast<const script_extensible*>(static_cast<const char*>(stored) + layout.slot_offset)->has_script_layer()) {
		return false;
	}
	void* address = lane->locate(*lane, receiver);
	if (!address) { return false; }
	out.lane = lane;
	out.delta = static_cast<const char*>(address) - static_cast<const char*>(stored);
	out.layout = layout;
	return true;
}

// The member's address when the receiver's object still has the layout the hit was measured under, else null
inline void* host_member_address(const class_instance* inst, const host_member_hit& hit) {
	const char* object = host_receiver_object(inst, hit.layout);
	return object ? const_cast<char*>(object) + hit.delta : nullptr;
}

// The class whose binding of the name get_method answers with (its own table, then its parents in order, then its C++
// base) and that binding's direct lane; null when the answering binding has none (a guarded or overloaded one)
inline const class_definition* host_native_owner(const class_definition* cls, uint64_t method_id,
                                                 const class_definition::native_entry*& entry, bool& answered, int depth = 0) {
	if (!cls || answered || depth > 64) { return nullptr; }
	if (cls->has_method(method_id)) {
		answered = true;
		entry = cls->find_native_method(method_id);
		return entry ? cls : nullptr;
	}
	for (const auto& parent : cls->get_parent_classes()) {
		if (const class_definition* owner = host_native_owner(parent.get(), method_id, entry, answered, depth + 1)) { return owner; }
		if (answered) { return nullptr; }
	}
	if (auto base = cls->get_cpp_base_class()) { return host_native_owner(base.get(), method_id, entry, answered, depth + 1); }
	return nullptr;
}

// A bound method's direct lane for this receiver: the binding its class answers with (its own or an inherited one), or for
// a layer whose script chain defines no method of the name, its C++ base's; reached at that class's offset in the object
inline bool host_method_fill(class_definition* cd, const class_instance* inst, uint64_t method_id, engine* eng,
                             host_method_hit& out) {
	out = host_method_hit{};
	const class_definition* owner = cd;
	std::shared_ptr<class_definition> base;
	if (cd->is_script_class()) {
		if (inst->script_layer_retired() || cd->chain_has_nonpublic() || cd->script_chain_defines_method(method_id)) { return false; }
		base = cd->find_cpp_base_class();
		if (!base) { return false; }
		owner = base.get();
	} else if (!cd->is_cpp_class()) {
		return false;
	}
	const class_definition::native_entry* entry = nullptr;
	bool answered = false;
	owner = host_native_owner(owner, method_id, entry, answered);
	if (!owner || !entry) { return false; }
	const void* stored = nullptr;
	host_receiver_layout layout;
	if (!host_receiver_measure(cd, inst, owner, eng, stored, out.delta, layout)) { return false; }
	// the method's object sits at the registered cast from the stored type: no dynamic type moves it
	layout.vptr = nullptr;
	out.entry = entry;
	out.layout = layout;
	return true;
}

// The object the method runs on when the receiver still has the layout the hit was measured under, else null
inline void* host_method_self(const class_instance* inst, const host_method_hit& hit) {
	const char* object = host_receiver_object(inst, hit.layout);
	return object ? const_cast<char*>(object) + hit.delta : nullptr;
}

} // namespace jai::detail

#endif
