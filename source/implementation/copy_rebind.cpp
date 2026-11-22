#include "../../include/jaiscript/detail/copy_rebind.hpp"
#include "../../include/jaiscript/core/coroutine.hpp"
#include "../../include/jaiscript/core/engine.hpp"
#include "../../include/jaiscript/core/execution_backend.hpp"
#include "../../include/jaiscript/detail/class_runtime.hpp"
#include "../../include/jaiscript/detail/coroutine_snapshot.hpp"
#include "../../include/jaiscript/detail/engine_access.hpp"
#include "../../include/jaiscript/detail/environment.hpp"

#include <unordered_map>

namespace jai::detail {

namespace copy_rebind_impl {

const class_instance* instance_of(const script_value& a_value) noexcept { return a_value.deref().peek_class_instance(); }

const class_instance* weak_instance_of(const script_value& a_value) noexcept {
	const script_value& value = a_value.deref();
	if (value.raw_storage_index() != script_value::TYPEID_WEAK_PTR) { return nullptr; }
	const auto holder = value.get_weak_ptr().lock();
	return holder && holder->is_instance() ? holder->unchecked_instance() : nullptr;
}

// Keys to values, searched in place while few (a copy usually pairs a handful of instances), hashed past that
template <class Key>
class small_table {
	static constexpr size_t k_linear = 16;
	std::vector<std::pair<Key, script_value>> entries_;
	std::unordered_map<Key, size_t> index_;

public:
	const script_value* find(Key a_key) const {
		if (entries_.size() <= k_linear) {
			for (const auto& [key, value] : entries_) { if (key == a_key) { return &value; } }
			return nullptr;
		}
		auto found = index_.find(a_key);
		return found == index_.end() ? nullptr : &entries_[found->second].second;
	}
	bool insert(Key a_key, const script_value& a_value) {
		if (find(a_key)) { return false; }
		entries_.emplace_back(a_key, a_value);
		if (entries_.size() == k_linear + 1) {
			for (size_t i = 0; i < entries_.size(); ++i) { index_.emplace(entries_[i].first, i); }
		} else if (entries_.size() > k_linear + 1) {
			index_.emplace(a_key, entries_.size() - 1);
		}
		return true;
	}
	bool empty() const noexcept { return entries_.empty(); }
};

bool handle_stamped(const type_info* a_tag) noexcept { return a_tag && a_tag->base_type == script_value_type::jai_shared_ptr_type; }

// Keys a copied map keeps in its source's order: the same scalar, the same text, the same shared object
bool same_plain_key(const script_value& a_left, const script_value& a_right) noexcept {
	const size_t kind = a_left.raw_storage_index();
	if (kind != a_right.raw_storage_index()) { return false; }
	switch (kind) {
		case script_value::TYPEID_INT: return a_left.unchecked_as_int() == a_right.unchecked_as_int();
		case script_value::TYPEID_FLOAT: return a_left.as_float() == a_right.as_float();
		case script_value::TYPEID_BOOL: return a_left.as_bool() == a_right.as_bool();
		case script_value::TYPEID_CHAR: return a_left.as_char() == a_right.as_char();
		case script_value::TYPEID_STRING: return a_left.as_string() == a_right.as_string();
		case script_value::TYPEID_OBJECT: return a_left.unchecked_object_handle().get() == a_right.unchecked_object_handle().get();
		default: return false;
	}
}

struct rebinder {
	// Where the copy holds a closure or a coroutine: a value in place, or an object array's slot
	struct site {
		script_value* value = nullptr;
		script_array* objects = nullptr;
		size_t index = 0;
	};
	struct value_pair {
		const script_value* source;
		script_value* copy;
	};
	// A coroutine's copy between its record's values being copied and its frame being rebuilt from them
	struct coroutine_rebuild {
		script_value copy{std::monostate{}, nullptr};
		script_value receiver{std::monostate{}, nullptr};
		std::shared_ptr<coroutine_handle> source;
		coroutine_record record;
		std::vector<coroutine_site> sites;
	};
	struct step {
		site at;
		std::unique_ptr<coroutine_rebuild> rebuild;
	};

	engine* eng = nullptr;
	uint64_t this_id = UINT64_MAX;
	uint64_t weak_this_id = UINT64_MAX;
	small_table<const class_instance*> copies;   // an instance the source holds by value, and its copy
	small_table<const void*> made;               // a closure or a coroutine bound to the source, and the copy's
	std::vector<value_pair> pairs;
	std::vector<site> found;
	std::vector<step> steps;

	const script_value* copy_of(const class_instance* a_instance) const {
		return a_instance ? copies.find(a_instance) : nullptr;
	}

	void pair_instances(const class_instance& a_source, class_instance& a_copy, const script_value& a_copy_value) {
		if (&a_source == &a_copy || !copies.insert(&a_source, a_copy_value)) { return; }
		if (!eng) { eng = a_copy.get_engine(); }
		a_copy.for_each_field([&](uint64_t a_field, script_value& a_held) {
			if (const script_value* from = a_source.find_field_value(a_field)) { pairs.push_back({from, &a_held}); }
		});
	}

	void visit(const script_value& a_source, script_value& a_copy) {
		switch (a_copy.raw_storage_index()) {
			case script_value::TYPEID_FUNCTION:
				found.push_back({&a_copy});
				return;
			case script_value::TYPEID_OBJECT: {
				const auto& held = a_copy.unchecked_object_handle();
				if (!held || handle_stamped(a_copy.type_info_raw())) { return; }
				if (held->is_coroutine_handle()) {
					found.push_back({&a_copy});
					return;
				}
				if (!held->is_instance() || a_source.raw_storage_index() != script_value::TYPEID_OBJECT || handle_stamped(a_source.type_info_raw())) { return; }
				const auto& from = a_source.unchecked_object_handle();
				if (from && from->is_instance()) { pair_instances(*from->unchecked_instance(), *held->unchecked_instance(), a_copy); }
				return;
			}
			case script_value::TYPEID_ARRAY: {
				if (a_source.raw_storage_index() != script_value::TYPEID_ARRAY) { return; }
				const script_array* from = a_source.unchecked_array_node();
				script_array* to = const_cast<script_array*>(a_copy.unchecked_array_node());
				if (!from || !to || from == to || from->kind() != to->kind()) { return; }
				if (to->kind() == script_array::kind_t::obj) {
					const size_t count = std::min(from->size(), to->size());
					for (size_t i = 0; i < count; ++i) {
						const obj_slot& slot = to->objs()[i];
						if (!slot.handle || handle_stamped(slot.tag.get())) { continue; }
						if (slot.handle->is_coroutine_handle()) {
							found.push_back({nullptr, to, i});
							continue;
						}
						const obj_slot& source = from->objs()[i];
						if (slot.handle->is_instance() && source.handle && source.handle->is_instance() && !handle_stamped(source.tag.get())) {
							pair_instances(*source.handle->unchecked_instance(), *slot.handle->unchecked_instance(), to->get(i, to->owner));
						}
					}
				} else if (to->kind() == script_array::kind_t::hetero) {
					const size_t count = std::min(from->size(), to->size());
					for (size_t i = 0; i < count; ++i) { pairs.push_back({&from->values()[i], &to->values()[i]}); }
				}
				return;
			}
			case script_value::TYPEID_MAP: {
				if (a_source.raw_storage_index() != script_value::TYPEID_MAP) { return; }
				const script_map& from = a_source.unchecked_as_map();
				script_map& to = const_cast<script_map&>(a_copy.unchecked_as_map());
				if (&from == &to) { return; }
				auto placed = to.begin();
				for (auto entry = from.begin(); entry != from.end() && placed != to.end(); ++entry, ++placed) {
					if (!same_plain_key(entry->first, placed->first)) { break; }   // keys ordered by identity: the copy's order is its own
					pairs.push_back({&entry->second, &placed->second});
				}
				return;
			}
			default:
				return;
		}
	}

	// Pairs every instance under the two values and notes each closure and coroutine the copy holds
	void walk(const script_value& a_source, script_value& a_copy) {
		const size_t floor = pairs.size();
		pairs.push_back({&a_source, &a_copy});
		drain(floor);
	}

	void drain(size_t floor) {
		while (pairs.size() > floor) {
			const value_pair next = pairs.back();
			pairs.pop_back();
			visit(next.source->deref(), next.copy->is_reference() ? const_cast<script_value&>(next.copy->deref()) : *next.copy);
		}
	}

	void take_found() {
		for (size_t i = found.size(); i-- > 0;) { steps.push_back({found[i], nullptr}); }
		found.clear();
	}

	script_value held_at(const site& a_site) const {
		return a_site.value ? *a_site.value : a_site.objects->get(a_site.index, a_site.objects->owner);
	}

	void place(const site& a_site, const script_value& a_made) {
		if (a_site.value) {
			*a_site.value = a_made;
		} else {
			a_site.objects->objs()[a_site.index].handle = a_made.unchecked_object_handle();
		}
	}

	static script_value stamped_as(script_value a_made, const script_value& a_original) {
		a_made.set_type_info(a_original.get_type_info());
		return a_made;
	}

	// A lambda's scope over `a_parent` for the copy: a captured this or weak_this names the instance's copy, a value the
	// lambda captured is copied, a variable it shares stays shared
	std::shared_ptr<environment> scope_copy(environment& a_scope, std::shared_ptr<environment> a_parent) {
		string_symbolizer* symbols = engine_access::get_symbolizer(*eng);
		const bool instance_scope = a_scope.is_method_env();
		const script_value* receiver = instance_scope ? copy_of(instance_of(a_scope.get_this_object())) : nullptr;
		std::shared_ptr<environment> result = instance_scope
			? std::make_shared<environment>(std::move(a_parent), symbols, receiver ? *receiver : a_scope.get_this_object())
			: std::make_shared<environment>(std::move(a_parent), symbols);
		result->set_access_context(a_scope.get_access_context());
		for (const auto& [name, cell] : a_scope.own_definitions()) {
			if (instance_scope && name == this_id) {
				result->define(name, receiver ? *receiver : *cell);
				continue;
			}
			if (name == weak_this_id) {
				if (const script_value* to = copy_of(weak_instance_of(*cell))) {
					result->define(name, script_value::make_weak_instance(*to, eng));
					continue;
				}
			}
			if (cell->is_reference()) {
				result->define(name, *cell);
				continue;
			}
			result->define(name, clone_for_capture(*cell));
			if (script_value* placed = result->get_own_value_ptr(name)) { walk(*cell, *placed); }
		}
		return result;
	}

	std::optional<script_value> rebound_closure(const script_value& a_function, const script_callable_thunk& a_thunk) {
		const std::shared_ptr<script_defined_function>& function = a_thunk.payload.fn;
		if (const script_value* known = made.find(function.get())) { return *known; }
		// the scopes the lambda owns: its capture scope, then the instance scope of a captured this (the creating frame's lies past it)
		std::vector<environment*> scopes;
		size_t outermost = SIZE_MAX;
		for (environment* scope = function->closure_env.get(); scope && !scope->is_program_scope(); scope = scope->parent_raw()) {
			scopes.push_back(scope);
			if (scope->is_method_env()) {
				if (copy_of(instance_of(scope->get_this_object()))) { outermost = scopes.size() - 1; }
				break;
			}
			if (weak_this_id != UINT64_MAX) {
				const script_value* weak = scope->get_own_value_ptr(weak_this_id);
				if (weak && copy_of(weak_instance_of(*weak))) { outermost = scopes.size() - 1; }
			}
		}
		if (outermost == SIZE_MAX) { return std::nullopt; }
		std::shared_ptr<environment> scope = scopes[outermost]->get_parent();
		for (size_t i = outermost + 1; i-- > 0;) { scope = scope_copy(*scopes[i], std::move(scope)); }
		auto rebound = std::make_shared<script_defined_function>(function->name, function->shared_parameters, function->return_type, function->body,
		                                                          std::move(scope), function->local_count);
		rebound->backend_body_cache = function->backend_body_cache;
		rebound->unit = function->unit;
		rebound->block_defined = function->block_defined;
		rebound->declared = function->declared;
		rebound->line = function->line;
		rebound->context = function->context;
		rebound->saved_source = function->saved_source;
		rebound->decl = function->decl;
		script_callable payload;
		payload.kind = script_callable::kind_type::function;
		payload.fn = std::move(rebound);
		script_value result = stamped_as(script_value::make_function(script_callable_thunk{a_thunk.eng, std::move(payload)}, eng), a_function);
		made.insert(function.get(), result);
		return result;
	}

	std::optional<script_value> rebound_function(const script_value& a_function) {
		const script_function& callable = a_function.unchecked_as_function();
		if (const auto* thunk = callable.target<script_callable_thunk>()) {
			const script_callable& payload = thunk->payload;
			if (payload.kind == script_callable::kind_type::function) {
				if (!payload.fn || !payload.fn->closure_env) { return std::nullopt; }
				return rebound_closure(a_function, *thunk);
			}
			const script_value* to = payload.this_obj() ? copy_of(instance_of(*payload.this_obj())) : nullptr;
			if (!to) { return std::nullopt; }
			script_callable rebound(payload);
			rebound.more().this_obj = *to;
			return stamped_as(script_value::make_function(script_callable_thunk{thunk->eng, std::move(rebound)}, eng), a_function);
		}
		if (const auto* bound = callable.target<bound_method_fn>()) {
			if (const script_value* to = copy_of(instance_of(bound->this_obj))) { return stamped_as(make_bound_method(*to, bound->method), a_function); }
		}
		return std::nullopt;
	}

	// The coroutine's copy takes the handle's place at once (a handle inside its own frame finds it); its frame is rebuilt
	// once every closure and coroutine its record holds has its copy. A coroutine whose state has no record leaves the copy
	// a handle that cannot be resumed
	void copy_coroutine(const site& a_site, const script_value& a_held, const std::shared_ptr<coroutine_handle>& a_handle) {
		const script_value* receiver = copy_of(instance_of(a_handle->receiver()));
		if (!receiver) { return; }
		if (const script_value* known = made.find(a_handle.get())) {
			place(a_site, *known);
			return;
		}
		coroutine_record original;
		std::vector<coroutine_site> sites;
		std::string refused = coroutine_record_into(*eng, *a_handle, coroutine_capture::copy, original, &sites);
		auto rebuild = std::make_unique<coroutine_rebuild>();
		rebuild->copy = stamped_as(make_unloaded_coroutine(*eng), a_held);
		made.insert(a_handle.get(), rebuild->copy);
		place(a_site, rebuild->copy);
		if (!refused.empty()) {
			coroutine_of(rebuild->copy)->mark_uncopied(coroutine_display_name(*a_handle), std::move(refused));
			return;
		}
		rebuild->receiver = *receiver;
		rebuild->source = a_handle;
		rebuild->sites = std::move(sites);
		rebuild->record.reserve(original.size());
		for (const auto& [key, value] : original) { rebuild->record.emplace_back(key, value.clone_element()); }
		coroutine_rebuild& pending = *rebuild;
		steps.push_back({site{}, std::move(rebuild)});
		for (size_t i = 0; i < original.size(); ++i) { walk(original[i].second, pending.record[i].second); }
		take_found();
	}

	void resolve(const site& a_site) {
		const script_value held = held_at(a_site);
		if (held.raw_storage_index() == script_value::TYPEID_FUNCTION) {
			if (auto rebound = rebound_function(held)) { place(a_site, *rebound); }
			take_found();
			return;
		}
		if (const std::shared_ptr<coroutine_handle> handle = coroutine_of(held)) { copy_coroutine(a_site, held, handle); }
	}

	void run_members(const class_instance& a_source, class_instance& a_copy) {
		script_value copy_value = script_value::make_class_object(*a_copy.get_class_definition(), a_copy.strong_from_this(), a_copy.get_engine());
		pair_instances(a_source, a_copy, copy_value);
		drain(0);
		finish();
	}

	void run_value(const script_value& a_source, script_value& a_copy) {
		pairs.push_back({&a_source, &a_copy});
		drain(0);
		finish();
	}

	void finish() {
		if (copies.empty() || found.empty() || !eng) { return; }
		string_symbolizer* symbols = engine_access::get_symbolizer(*eng);
		this_id = symbols->get_this_id();
		if (const auto id = symbols->find("weak_this")) { weak_this_id = *id; }
		take_found();
		while (!steps.empty()) {
			step next = std::move(steps.back());
			steps.pop_back();
			if (!next.rebuild) {
				resolve(next.at);
				continue;
			}
			const script_value record = coroutine_record_map(*eng, std::move(next.rebuild->record));
			if (!copy_coroutine_record(*eng, next.rebuild->copy, record.as_map(), *next.rebuild->source, next.rebuild->receiver, std::move(next.rebuild->sites))) {
				coroutine_of(next.rebuild->copy)->mark_uncopied(coroutine_display_name(*next.rebuild->source), "its frame could not be rebuilt");
			}
		}
	}
};

} // namespace copy_rebind_impl

namespace copy_rebind_impl {

// bound: the callable names an instance. settled: nothing the callable can do changes the answer (a scope that holds
// captured values may come to hold a weak_ptr)
struct function_binding {
	bool bound = false;
	bool settled = true;
};

function_binding binding_of(const script_function& a_callable) noexcept {
	if (const auto* thunk = a_callable.target<script_callable_thunk>()) {
		const script_callable& payload = thunk->payload;
		if (payload.kind != script_callable::kind_type::function) { return {payload.this_obj().has_value(), true}; }
		function_binding result;
		for (const environment* scope = payload.fn ? payload.fn->closure_env.get() : nullptr; scope && !scope->is_program_scope(); scope = scope->parent_raw()) {
			if (scope->is_method_env()) { return {true, true}; }
			for (const script_value& held : scope->own_values()) {
				if (held.raw_storage_index() == script_value::TYPEID_WEAK_PTR) { return {true, true}; }
				result.settled = false;
			}
		}
		return result;
	}
	return {a_callable.target<bound_method_fn>() != nullptr, true};
}

} // namespace copy_rebind_impl

JAI_NOINLINE bool binds_class_instance(const script_value& a_callable) noexcept {
	if (a_callable.raw_storage_index() == script_value::TYPEID_FUNCTION) {
		const script_function_node& node = a_callable.unchecked_function_node();
		if (node.binds_instance != 0) { return node.binds_instance == 2; }
		const copy_rebind_impl::function_binding binding = copy_rebind_impl::binding_of(node);
		// a parallel region's chunks read the node together: only the engine's own thread, outside a region, writes it
		if (binding.settled && node.owner && !engine_access::parallel_region_active(*node.owner)) { node.binds_instance = binding.bound ? 2 : 1; }
		return binding.bound;
	}
	if (a_callable.raw_storage_index() != script_value::TYPEID_OBJECT) { return false; }
	const auto& holder = a_callable.unchecked_object_handle();
	if (!holder || !holder->is_coroutine_handle()) { return false; }
	const auto* handle = static_cast<const coroutine_handle*>(holder->object());
	return handle && copy_rebind_impl::instance_of(handle->receiver());
}

JAI_NOINLINE void rebind_copied_callables(const script_value& a_source, script_value& a_copy) {
	copy_rebind_impl::rebinder rebinder;
	rebinder.run_value(a_source, a_copy);
}

JAI_NOINLINE void rebind_copied_callables(const class_instance& a_source, class_instance& a_copy) {
	if (!a_copy.get_class_definition()) { return; }
	copy_rebind_impl::rebinder rebinder;
	rebinder.run_members(a_source, a_copy);
}

} // namespace jai::detail
