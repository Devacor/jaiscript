#include "jaiscript/core/class_registry.hpp"
#include "jaiscript/core/dynamic_binder.hpp"
#include "jaiscript/core/engine.hpp"
#include "jaiscript/detail/class_runtime.hpp"
#include "jaiscript/detail/namespace_scope.hpp"
#include "jaiscript/detail/release_machine.hpp"
#include "jaiscript/detail/ast_serializer.hpp"
#include "jaiscript/detail/builtin_methods.hpp"
#include "jaiscript/detail/parallel_transform.hpp"
#include "jaiscript/detail/type_name_key.hpp"
#include "jaiscript/detail/function_overloads.hpp"
#include "jaiscript/detail/namespace_function.hpp"
#include "jaiscript/detail/generics.hpp"
#include <stdexcept>
#include <thread>
#include <tuple>
#include <set>
#include <algorithm>
#include <unordered_set>
#include <span>

#include <jaiscript/core/registrar.hpp>
#include <jaiscript/properties/property_manager.hpp>

namespace jai {

type_name_registry& type_name_registry::instance() {
    static type_name_registry registry;
    return registry;
}

void type_name_registry::register_type(std::type_index ti, std::string_view name) {
    auto& stored = type_names_[ti] = std::string(name);
    name_to_type_[stored] = ti;
}

std::optional<std::string_view> type_name_registry::get_name(std::type_index ti) const {
    if (auto it = type_names_.find(ti); it != type_names_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::type_index> type_name_registry::get_type(const std::string& name) const {
    if (auto it = name_to_type_.find(name); it != name_to_type_.end()) {
        return *it->second;
    }
    return std::nullopt;
}

bool type_name_registry::has_name(std::type_index ti) const {
    return type_names_.contains(ti);
}

namespace detail {

namespace {

struct declared_binding {
    std::string name;
    std::type_index context;
    declared_binding_fn bind;
};

// Program-wide, filled at program start: the property_owner classes that queue their own binding
struct declared_binding_store {
    std::unordered_map<std::type_index, declared_binding> queued;
};

declared_binding_store& declared_bindings() {
    static declared_binding_store store;
    return store;
}

[[noreturn]] void throw_script_name_collision(const std::string& a_name, std::type_index a_first, std::type_index a_second) {
    std::string first = readable_cpp_type_name(a_first);
    std::string second = readable_cpp_type_name(a_second);
    if (second < first) { std::swap(first, second); }
    throw std::runtime_error("bind_registrar: the script name '" + a_name + "' belongs to two C++ types, '" + first + "' and '" + second +
        "'; name one another way (JAI_PROPERTY_OWNER_AS, or a jai::registrar with its own name) or leave one out with jai::except<>()");
}

} // namespace

void queue_declared_binding(std::type_index a_type, std::string_view a_name, std::type_index a_context, declared_binding_fn a_bind) {
    declared_bindings().queued.insert_or_assign(a_type, declared_binding{ std::string(a_name), a_context, a_bind });
}

[[noreturn]] void throw_registrar_renames_owner(std::type_index a_type, const std::string& a_name, std::string_view a_serialization_name) {
    throw std::runtime_error("bind_registrar: the jai::registrar of the property_owner class '" + readable_cpp_type_name(a_type) + "' names it '" + a_name +
        "'; it binds under its serialization name '" + std::string(a_serialization_name) + "'");
}

registration_order::registration_order() = default;

registration_order::~registration_order() = default;

void registration_order::add(std::type_index ti, const std::string& name, erased_fn fn) {
    if (auto it = names_.find(ti); it != names_.end()) {
        auto range = entries_.equal_range(it->second);
        for (auto entry = range.first; entry != range.second; ++entry) {
            if (entry->second.first == ti) { entries_.erase(entry); break; }
        }
    }
    names_.insert_or_assign(ti, name);
    entries_.emplace(name, std::pair<std::type_index, erased_fn>{ ti, std::move(fn) });
}

void registration_order::check_filter(const registrar_filter* filter, std::span<const registration_order* const> orders) {
    if (!filter) { return; }
    const declared_binding_store& store = declared_bindings();
    for (const std::type_index& listed : filter->listed()) {
        const bool registered = std::any_of(orders.begin(), orders.end(), [&](const registration_order* a_order) { return a_order->names_.contains(listed); });
        if (!registered && !store.queued.contains(listed)) {
            throw std::runtime_error("bind_registrar: the filter names the C++ type '" + readable_cpp_type_name(listed) + "', which has no registrar for this context");
        }
    }
}

void registration_order::run_all(engine& eng, const void* context, std::type_index context_type, const registrar_filter* filter) {
    const context_run run{ this, context, context_type };
    run_chain(eng, std::span(&run, 1), filter);
}

void registration_order::run_chain(engine& eng, std::span<const context_run> runs, const registrar_filter* filter) {
    std::vector<const registration_order*> orders;
    orders.reserve(runs.size());
    for (const context_run& run : runs) { orders.push_back(run.order); }
    check_filter(filter, orders);
    for (const context_run& run : runs) { run.order->run_checked(eng, run, orders, filter); }
}

void registration_order::run_checked(engine& eng, const context_run& run, std::span<const registration_order* const> chain, const registrar_filter* filter) {
    const void* context = run.context;
    struct step {
        const std::string* name;
        std::type_index type;
        erased_fn* registered;
        declared_binding_fn declared;
    };
    const declared_binding_store& store = declared_bindings();
    std::vector<step> steps;
    steps.reserve(entries_.size() + store.queued.size());
    for (auto& [name, entry] : entries_) {
        steps.push_back({ &name, entry.first, &entry.second, nullptr });
    }
    const size_t registered_count = steps.size();
    // a class binds itself where no registrar of the call's contexts adds to it: untagged in every call, tagged in the run of its context
    const std::type_index untagged(typeid(void));
    for (const auto& [type, declared] : store.queued) {
        const bool context_matches = declared.context == untagged || declared.context == run.context_type;
        const bool claimed = std::any_of(chain.begin(), chain.end(), [&](const registration_order* a_order) { return a_order->names_.contains(type); });
        if (context_matches && !claimed) { steps.push_back({ &declared.name, type, nullptr, declared.bind }); }
    }
    // one name never runs two types (the check below), so name order alone is the whole order
    const auto by_name = [](const step& a_left, const step& a_right) { return *a_left.name < *a_right.name; };
    std::sort(steps.begin() + static_cast<std::ptrdiff_t>(registered_count), steps.end(), by_name);
    std::inplace_merge(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(registered_count), steps.end(), by_name);

    const auto admitted = [&](const step& a_step) { return filter == nullptr || filter->admits(a_step.type); };

    // two types under one name would bind in static-initialization order; a declared class never replaces what the host bound
    std::unordered_map<std::string_view, std::type_index> claimants;
    for (const step& candidate : steps) {
        if (!admitted(candidate)) { continue; }
        if (auto [it, inserted] = claimants.try_emplace(*candidate.name, candidate.type); !inserted && it->second != candidate.type) {
            throw_script_name_collision(*candidate.name, it->second, candidate.type);
        }
        if (candidate.declared) {
            if (auto bound = engine_access::get_class_definition_by_type(eng, candidate.type); bound && !bound->auto_registered()) { continue; }
            if (auto named = engine_access::get_class_definition(eng, *candidate.name); named && !named->auto_registered()) {
                if (!named->cpp_type()) {
                    throw std::runtime_error("bind_registrar: the script name '" + *candidate.name + "' already names a script class, so the C++ type '" +
                        readable_cpp_type_name(candidate.type) + "' cannot bind under it");
                }
                throw_script_name_collision(*candidate.name, std::type_index(*named->cpp_type()), candidate.type);
            }
        }
    }

    // a left-out class an admitted one derives from would bind as a bare base, or not at all: what is bound beforehand is the host's
    std::vector<std::type_index> left_out;
    if (filter) {
        for (const auto& [type, name] : names_) {
            if (!filter->admits(type) && !engine_access::get_class_definition_by_type(eng, type)) { left_out.push_back(type); }
        }
    }
    std::vector<std::type_index> admitted_types;
    for (const step& current : steps) {
        if (!admitted(current)) { continue; }
        admitted_types.push_back(current.type);
        if (current.registered) {
            (*current.registered)(eng, context);
        } else if (auto bound = engine_access::get_class_definition_by_type(eng, current.type); !bound || bound->auto_registered()) {
            current.declared(eng, context);
        }
    }
    for (const std::type_index& type : left_out) {
        std::vector<std::string> derived = engine_access::classes_awaiting_base(eng, type);
        if (auto base = engine_access::get_class_definition_by_type(eng, type)) {
            for (const std::type_index& admitted_type : admitted_types) {
                if (auto bound = engine_access::get_class_definition_by_type(eng, admitted_type)) {
                    const auto& parents = bound->get_parent_classes();
                    if (std::find(parents.begin(), parents.end(), base) != parents.end()) { derived.push_back(bound->get_name()); }
                }
            }
        }
        if (!derived.empty()) {
            throw std::runtime_error("bind_registrar: '" + derived.front() + "' derives from '" + names_.at(type) +
                "', which the filter leaves out; bind both or leave out '" + derived.front() + "' too");
        }
    }
}

bool registration_order::has(std::type_index ti) const {
    return names_.contains(ti);
}

} // namespace detail

} // namespace jai

namespace jai {

// ===== flat_value_semantics (class_definition.hpp doc) =====
// Out of line: nested field classes resolve through the engine's registry, which the
// class_definition header cannot see.

bool class_definition::flat_value_semantics() const {
    if (!engine_) {
        return false;
    }
    const uint64_t epoch = detail::engine_access::class_definition_epoch(*engine_);
    if (flat_value_epoch_ == epoch) {
        return flat_value_cached_;
    }
    std::vector<const class_definition*> in_progress;
    flat_value_cached_ = flat_value_semantics_impl(in_progress);
    flat_value_epoch_ = epoch;
    return flat_value_cached_;
}

bool class_definition::flat_over_host_values() const {
    if (!engine_) {
        return false;
    }
    const uint64_t epoch = detail::engine_access::class_definition_epoch(*engine_);
    if (flat_host_epoch_ != epoch) {
        std::vector<const class_definition*> in_progress;
        flat_host_cached_ = flat_value_semantics_impl(in_progress, true);
        flat_host_epoch_ = epoch;
    }
    return flat_host_cached_;
}

const std::vector<uint32_t>& class_definition::handle_bearing_slots() const {
    const uint64_t epoch = engine_ ? detail::engine_access::class_definition_epoch(*engine_) : 0;
    if (handle_slots_epoch_ == epoch && engine_) { return handle_slots_cached_; }
    handle_slots_cached_.clear();
    const uint32_t n = field_slot_count();
    for (uint32_t slot = 0; slot < n; ++slot) {
        const type_info_ptr t = get_field_declared_type(slot_field(slot));
        bool scalar = false;
        if (t) {
            switch (t->base_type) {
            case script_value_type::jai_int_type: case script_value_type::jai_float_type:
            case script_value_type::jai_bool_type: case script_value_type::jai_char_type:
                scalar = true; break;
            default: break;
            }
        }
        if (!scalar) { handle_slots_cached_.push_back(slot); }
    }
    handle_slots_epoch_ = epoch;
    return handle_slots_cached_;
}
bool class_definition::type_can_hold_string(const type_info_ptr& t, std::vector<const class_definition*>& in_progress) const {
    if (!t) { return true; }   // auto/var: no store-enforced shape
    switch (t->base_type) {
    case script_value_type::jai_string_type: return true;
    case script_value_type::jai_int_type: case script_value_type::jai_float_type:
    case script_value_type::jai_bool_type: case script_value_type::jai_char_type: return false;
    case script_value_type::jai_array_type: return type_can_hold_string(t->element_type(), in_progress);
    case script_value_type::jai_map_type: return type_can_hold_string(t->key_type(), in_progress) || type_can_hold_string(t->value_type(), in_progress);
    case script_value_type::jai_object_type: {
        if (!engine_ || t->type_name.empty()) { return true; }
        auto nested = detail::engine_access::get_class_registry(*engine_).find_script_class(std::string(t->type_name));
        if (!nested) { return true; }
        for (const class_definition* visiting : in_progress) { if (visiting == nested.get()) { return false; } }
        in_progress.push_back(nested.get());
        bool any = false;
        const uint32_t n = nested->field_slot_count();
        for (uint32_t slot = 0; slot < n && !any; ++slot) {
            any = nested->type_can_hold_string(nested->get_field_declared_type(nested->slot_field(slot)), in_progress);
        }
        in_progress.pop_back();
        return any;
    }
    default: return true;
    }
}

bool class_definition::type_tree_runs_destructor(const type_info_ptr& t, std::vector<const class_definition*>& in_progress, uint8_t mode) const {
    if (!t || t->base_type == script_value_type::jai_any_type || t->is_coroutine) {
        // an untyped slot holds any value: it runs what some script class runs (a coroutine<Y> slot's handle owns a frame of any locals)
        if (mode == 0 || mode == 3 || !engine_) { return true; }
        for (const auto& entry : detail::engine_access::get_class_registry(*engine_).script_classes_) {
            const class_definition* cls = entry.second.get();
            if (cls && ((mode == 1 && cls->chain_declares_destructor(*engine_)) || cls->find_copy_constructor_level())) { return true; }
        }
        return false;
    }
    switch (t->base_type) {
    case script_value_type::jai_null_type:
    case script_value_type::jai_int_type: case script_value_type::jai_float_type: case script_value_type::jai_bool_type:
    case script_value_type::jai_char_type: case script_value_type::jai_string_type: case script_value_type::jai_utf8_string_type:
    case script_value_type::jai_shared_ptr_type: case script_value_type::jai_weak_ptr_type:
    case script_value_type::jai_vec2_type: case script_value_type::jai_vec3_type: case script_value_type::jai_ivec2_type:
    case script_value_type::jai_ivec3_type: case script_value_type::jai_vec4_type: case script_value_type::jai_color_type:
        return false;
    case script_value_type::jai_function_type: return mode == 3;
    case script_value_type::jai_array_type: return mode != 2 && type_tree_runs_destructor(t->element_type(), in_progress, mode);
    case script_value_type::jai_map_type:
        return mode != 2 && (type_tree_runs_destructor(t->key_type(), in_progress, mode) || type_tree_runs_destructor(t->value_type(), in_progress, mode));
    case script_value_type::jai_object_type: {
        if (!engine_ || t->type_name.empty()) { return true; }
        auto nested = detail::engine_access::get_class_registry(*engine_).find_script_class(std::string(t->type_name));
        return nested && nested->value_tree_runs_destructor_impl(in_progress, mode);   // a host class runs no script destructor
    }
    default: return true;
    }
}

bool class_definition::value_tree_runs_destructor_impl(std::vector<const class_definition*>& in_progress, uint8_t mode) const {
    for (const class_definition* visiting : in_progress) { if (visiting == this) { return false; } }
    in_progress.push_back(this);
    bool runs = engine_ && ((mode < 2 && chain_declares_destructor(*engine_)) || ((mode == 1 || mode == 2) && find_copy_constructor_level()));
    const uint32_t n = field_slot_count();
    for (uint32_t slot = 0; slot < n && !runs; ++slot) {
        runs = type_tree_runs_destructor(get_field_declared_type(slot_field(slot)), in_progress, mode);
    }
    if (!runs && engine_) {
        for (const auto& entry : detail::engine_access::get_class_registry(*engine_).script_classes_) {
            const class_definition* other = entry.second.get();
            if (other && other != this && other->derives_from(this) && other->value_tree_runs_destructor_impl(in_progress, mode)) { runs = true; break; }
        }
    }
    in_progress.pop_back();
    return runs;
}

namespace {
// A death on a parallel region's chunk: the destructor runs on the chunk's own backend in its method sandbox (the barrier admitted
// it), and what a failing one reports waits for the join
bool run_destructor_body_in_region(engine& eng, class_instance* instance, const class_definition* level, const script_value& body) {
    const detail::parallel_region_table* region = detail::engine_access::parallel_state(eng).active_region;
    const detail::parallel_region_table::entry* chunk = region ? region->find_entry(std::this_thread::get_id()) : nullptr;
    if (!chunk || !chunk->backend || !chunk->definition_env || !level->is_script_class()) { return false; }
    const auto* shared = body.is_function() ? body.as_function().target<script_method_dispatch>() : nullptr;
    auto ast = static_cast<const script_class_definition*>(level)->get_destructor_ast();
    if (!shared || !ast) { return false; }
    script_method_dispatch dispatch;
    dispatch.eng = shared->eng;
    dispatch.cls = shared->cls;
    dispatch.name_id = shared->name_id;
    dispatch.definition_env = *chunk->definition_env;
    detail::dying_instance_pin self(instance);
    const std::vector<script_value> arguments{ script_value::make_class_object(*level, self.handle(), &eng) };
    try {
        auto ran = chunk->backend->execute_method_from(dispatch, ast, arguments[0], arguments, 1);
        if (!ran && chunk->deferred_reports) { chunk->deferred_reports->push_back(detail::deferred_report{ chunk->element(), ran.error(), format_error(ran, *detail::engine_access::get_symbolizer(eng)) }); }
    } catch (...) {
    }
    return true;
}
}

void class_definition::run_destructor_body(class_instance* instance, const class_definition* level, const script_value& body) const {
    if (detail::engine_access::parallel_region_active(*engine_)) [[unlikely]] {
        if (run_destructor_body_in_region(*engine_, instance, level, body)) { return; }
    }
    detail::release_machine* m = detail::engine_access::release_machine_here(*engine_);
    detail::engine_access::run_pin pin(engine_);   // a destructor runs where a release happens: a sleep in it blocks
    std::vector<script_value> arguments;
    if (m && !m->spare_arguments.empty()) {
        arguments = std::move(m->spare_arguments.back());
        m->spare_arguments.pop_back();
    }
    detail::dying_instance_pin self(instance);   // outlives the arguments, cleared on every path below
    try {
        arguments.push_back(script_value::make_class_object(*level, self.handle(), engine_));
        auto ran = body.as_function()(arguments);
        arguments.clear();
        if (!ran) { detail::engine_access::report_script_error(*engine_, format_error(ran, *detail::engine_access::get_symbolizer(*engine_)), ran.error()); }   // reported, never propagated
    } catch (...) {
        arguments.clear();
    }
    if (m && m->spare_arguments.size() < 8) { m->spare_arguments.push_back(std::move(arguments)); }
}

void class_definition::expand_destroy_level(class_instance* instance, const class_definition* level, const class_definition* kept,
                                            std::vector<detail::release_action>& actions, const class_definition* constructed) const {
    if (!level || (kept && (kept == level || kept->derives_from(level)))) { return; }
    if (engine_ && (!constructed || instance->level_constructed(level))) {
        if (const script_value* body = level->destructor_body(*engine_)) {
            detail::dispatch_level_scope level_dispatch(*instance, const_cast<class_definition*>(level), class_instance::cdtor_phase::destruction);
            run_destructor_body(instance, level, *body);
        }
    }
    for (const auto& parent : level->parent_classes_) {
        if (parent) { actions.push_back({parent.get(), 0}); }
    }
    level->for_each_own_field_id([&](uint64_t field) { actions.push_back({nullptr, field}); });
}

script_value class_definition::take_field_for_destroy(class_instance* instance, uint64_t field) const {
    const class_definition* layout = instance->get_class_definition() ? instance->get_class_definition() : this;
    const uint32_t slot = layout->field_slot(field);
    return slot != k_no_field_slot ? instance->take_field_slot(slot) : script_value(std::monostate{}, engine_);
}

const class_definition* class_instance::constructed_level() const {
    return constructed_levels_ == k_nothing_constructed || constructed_levels_ == 0 ? nullptr : class_def_;
}

bool class_instance::level_constructed(const class_definition* level) const {
    if (!class_def_ || constructed_levels_ == k_nothing_constructed) { return false; }
    const auto& order = class_def_->construction_order();
    const auto it = std::find(order.begin(), order.end(), level);
    return it != order.end() && static_cast<size_t>(it - order.begin()) < constructed_levels_;
}

void class_definition::destroy_levels(class_instance* instance, const class_definition* level, const class_definition* kept,
                                      const class_definition* constructed) const {
    if (!level) { return; }
    detail::release_sync sync(engine_);
    detail::release_machine* m = engine_ ? detail::engine_access::release_machine_here(*engine_) : nullptr;
    std::vector<detail::release_action> actions;
    if (m && !m->spare_actions.empty()) {
        actions = std::move(m->spare_actions.back());
        m->spare_actions.pop_back();
    }
    actions.push_back({level, 0});
    while (!actions.empty()) {
        const detail::release_action action = actions.back();
        actions.pop_back();
        if (action.level) {
            expand_destroy_level(instance, action.level, kept, actions, constructed);
        } else {
            script_value dying = take_field_for_destroy(instance, action.field);
        }
    }
    if (m && m->spare_actions.size() < 8) { m->spare_actions.push_back(std::move(actions)); }
}

void class_instance::defer_serial_native() noexcept {
    if (!class_def_ || !class_def_->serial_only_chain()) { return; }
    const detail::parallel_region_table* region = detail::engine_access::parallel_state(*owner_).active_region;
    const detail::parallel_region_table::entry* chunk = region ? region->find_entry(std::this_thread::get_id()) : nullptr;
    if (!chunk || !chunk->deferred_releases) { return; }
    const size_t element = chunk->cursors && !chunk->cursors->empty() ? chunk->cursors->front().element_cursor_index() : SIZE_MAX;
    try {
        chunk->deferred_releases->push_back({ element, std::move(*extras_->native) });
        extras_->native.reset();
    } catch (...) {
    }
}

void class_instance::release_fields() noexcept {
    engine* eng = class_def_->get_engine();
    detail::release_machine* m = eng ? detail::engine_access::release_machine_here(*eng) : nullptr;
    if (!m) { return; }
    if (!m->deferring && m->depth < m->depth_limit) {
        ++m->depth;
        while (!flat_fields_.empty()) { flat_fields_.pop_back(); }
        if (extras_) { extras_->overflow_fields.reset(); }
        extras_.reset();
        --m->depth;
        return;
    }
    const bool driver = !m->deferring;
    const size_t mark = m->stack.size();
    for (size_t slot = flat_fields_.size(); slot-- > 0;) {
        script_value* cell = flat_fields_.cell(slot);
        if (cell && cell->release_cascades()) { m->stack.push_back({std::move(*cell), detail::release_machine::k_no_frame}); }
    }
    if (overflow_fields()) {
        for (auto& [id, value] : *overflow_fields()) {
            if (value.release_cascades()) { m->stack.push_back({std::move(value), detail::release_machine::k_no_frame}); }
        }
    }
    if (const script_value* held = native(); held && held->release_cascades()) {
        m->stack.push_back({std::move(*extras_->native), detail::release_machine::k_no_frame});
    }
    if (driver) {
        std::reverse(m->stack.begin() + static_cast<ptrdiff_t>(mark), m->stack.end());
        detail::release_drive(*eng, *m, mark);
    }
}

void class_instance::release_orphan_fields() noexcept {
    std::vector<script_value> work;
    auto take_cells = [&work](class_instance& from) {
        for (size_t slot = 0; slot < from.flat_fields_.size(); ++slot) {
            script_value* cell = from.flat_fields_.cell(slot);
            if (cell && cell->release_cascades()) { work.push_back(std::move(*cell)); }
        }
        if (from.overflow_fields()) {
            for (auto& [id, value] : *from.overflow_fields()) {
                if (value.release_cascades()) { work.push_back(std::move(value)); }
            }
        }
        if (const script_value* held = from.native(); held && held->release_cascades()) { work.push_back(std::move(*from.extras_->native)); }
    };
    take_cells(*this);
    while (!work.empty()) {
        script_value v = std::move(work.back());
        work.pop_back();
        if (v.raw_storage_index() == script_value::TYPEID_OBJECT) {
            const auto& holder = v.unchecked_object_handle();
            if (holder.unique() && holder->is_instance() && holder->object_use_count() == 1) {
                auto* inner = holder->unchecked_instance();
                if (!inner->class_def_ && !inner->destructor_armed_) { take_cells(*inner); }
            }
        } else if (v.release_flattens()) {
            std::vector<script_value> nested;
            nested.push_back(std::move(v));
            detail::release_work(nested);
        }
    }
}

namespace {
// The instances `value` alone holds, in the order its release would destroy them: the instance it names, or the ones in the
// arrays and maps only it holds (an array's elements last to first, a map's entries in key order, each value ahead of its key).
// A pinned instance counts the teardown's own handle
void solely_held_instances(const script_value& value, const std::unordered_set<const class_instance*>& pinned,
                           std::vector<const script_value*>& pending, std::vector<class_instance*>& found) {
    const auto consider = [&](const strong_ptr<script_value::object_holder>& holder) {
        if (!holder || !holder->is_instance()) { return; }
        class_instance* held = holder->unchecked_instance();
        if (holder->object_use_count() == (pinned.count(held) ? 2 : 1)) { found.push_back(held); }
    };
    pending.clear();
    pending.push_back(&value);
    while (!pending.empty()) {
        const script_value& next = *pending.back();
        pending.pop_back();
        switch (next.raw_storage_index()) {
        case script_value::TYPEID_OBJECT:
            consider(next.unchecked_object_handle());
            break;
        case script_value::TYPEID_ARRAY: {
            if (!next.release_cascades()) { break; }
            const script_array& elements = *next.unchecked_get_array_storage();
            if (elements.kind() == script_array::kind_t::obj) {
                for (size_t i = elements.objs().size(); i-- > 0;) { consider(elements.objs()[i].handle); }
            } else {
                for (const script_value& element : elements.values()) { pending.push_back(&element); }
            }
            break;
        }
        case script_value::TYPEID_MAP: {
            if (!next.release_cascades()) { break; }
            const size_t base = pending.size();
            for (const auto& [key, held] : *next.unchecked_get_map_storage()) {
                pending.push_back(&held);
                pending.push_back(&key);
            }
            std::reverse(pending.begin() + static_cast<ptrdiff_t>(base), pending.end());
            break;
        }
        default: break;
        }
    }
}
}

void class_definition::run_surviving_destructors(engine& eng, std::vector<strong_ptr<class_instance>>& survivors) {
    std::unordered_set<const class_instance*> pinned;
    std::unordered_map<const class_instance*, class_instance*> sole_holder;
    std::unordered_set<const class_instance*> destroyed;
    std::vector<const script_value*> pending;
    std::vector<class_instance*> found;
    const auto birth_of = [](const class_instance* instance) {
        const uint64_t* birth = detail::object_block::birth(detail::cb_from_object(const_cast<class_instance*>(instance)));
        return birth ? *birth : uint64_t{0};
    };
    for (const auto& survivor : survivors) { pinned.insert(survivor.get()); }
    for (const auto& survivor : survivors) {
        class_instance* holder = survivor.get();
        const auto note = [&](const script_value& member) {
            found.clear();
            solely_held_instances(member, pinned, pending, found);
            for (class_instance* held : found) { if (pinned.count(held)) { sole_holder[held] = holder; } }
        };
        for (size_t slot = 0; slot < holder->flat_fields_.size(); ++slot) {
            if (const script_value* member = holder->flat_fields_.present_cell(slot)) { note(*member); }
        }
        if (const auto* overflow = holder->overflow_fields()) {
            for (const auto& [id, member] : *overflow) { note(member); }
        }
    }
    std::vector<class_instance*> order;
    for (size_t i = survivors.size(); i-- > 0;) {
        if (survivors[i]->destructor_armed_) { order.push_back(survivors[i].get()); }
    }
    std::stable_sort(order.begin(), order.end(), [&](const class_instance* a, const class_instance* b) { return birth_of(a) > birth_of(b); });

    struct step {
        strong_ptr<class_instance> instance;
        const class_definition* level = nullptr;   // null: the instance's death begins
    };
    std::vector<step> steps;
    const auto destroy = [&](class_instance* first) {
        steps.push_back({first->strong_from_this(), nullptr});
        while (!steps.empty()) {
            step now = std::move(steps.back());
            steps.pop_back();
            class_instance* instance = now.instance.get();
            if (!instance) { continue; }
            const class_definition* level = now.level;
            if (!level) {
                if (!destroyed.insert(instance).second) { continue; }
                level = instance->class_def_;
                if (!level) { continue; }
            }
            const class_definition* layout = instance->class_def_ ? instance->class_def_ : level;
            if (instance->destructor_armed_ && (!instance->construction_pending_ || instance->level_constructed(level))) {
                if (const script_value* body = level->destructor_body(eng)) {
                    detail::dispatch_level_scope level_dispatch(*instance, const_cast<class_definition*>(level), class_instance::cdtor_phase::destruction);
                    layout->run_destructor_body(instance, level, *body);
                }
            }
            for (const auto& parent : level->parent_classes_) {
                if (parent) { steps.push_back({now.instance, parent.get()}); }
            }
            // the members as the body left them: the last declared one's instances are the next to go
            level->for_each_own_field_id([&](uint64_t field) {
                const class_definition* current = instance->class_def_ ? instance->class_def_ : level;
                const uint32_t slot = current->field_slot(field);
                const script_value* member = slot != k_no_field_slot ? instance->field_slot_value(slot) : nullptr;
                if (!member) { return; }
                found.clear();
                solely_held_instances(*member, pinned, pending, found);
                for (size_t i = found.size(); i-- > 0;) {
                    strong_ptr<class_instance> held = found[i]->strong_from_this();
                    if (!held || destroyed.count(found[i])) { continue; }
                    if (pinned.insert(found[i]).second) { survivors.push_back(held); }
                    steps.push_back({std::move(held), nullptr});
                }
            });
        }
    };
    std::unordered_set<const class_instance*> climbed;
    for (class_instance* candidate : order) {
        if (destroyed.count(candidate)) { continue; }
        // up to the instance no member alone holds; a ring of sole holders starts at its last made member
        class_instance* first = candidate;
        climbed.clear();
        for (class_instance* at = candidate;;) {
            climbed.insert(at);
            const auto holder = sole_holder.find(at);
            if (holder == sole_holder.end() || destroyed.count(holder->second)) { first = at; break; }
            class_instance* up = holder->second;
            if (!climbed.count(up)) { at = up; continue; }
            first = nullptr;
            class_instance* member = up;
            do {
                if (member->destructor_armed_ && !destroyed.count(member) && (!first || birth_of(member) > birth_of(first))) { first = member; }
                member = sole_holder.find(member)->second;
            } while (member != up);
            if (!first) { first = up; }
            break;
        }
        destroy(first);
        if (!destroyed.count(candidate)) { destroy(candidate); }
    }
}

void class_definition::release_survivor_members(class_instance& instance) noexcept {
    std::vector<script_value> work;
    if (instance.extras_ && instance.extras_->native) { work.push_back(std::move(*instance.extras_->native)); }
    if (auto* overflow = instance.overflow_fields()) {
        for (auto& [id, member] : *overflow) { work.push_back(std::move(member)); }
    }
    for (size_t slot = 0; slot < instance.flat_fields_.size(); ++slot) {
        if (script_value* member = instance.flat_fields_.cell(slot)) { work.push_back(std::move(*member)); }
    }
    detail::release_work(work);
}

namespace detail {

void strong_last_release<class_instance>::run(class_instance* object, control_block_base* cb) noexcept {
#ifndef NDEBUG
    // An instance's death runs its destructors, the class's registry and the release machine on the engine: dropping the
    // last handle on another thread while the engine runs is a race (a value belongs to its engine's thread)
    if (const class_definition* def = object->get_class_definition(); def && def->get_engine()) {
        assert(engine_access::debug_releases_here(*def->get_engine()) && "a script instance died on another thread while its engine runs");
    }
#endif
    if (std::shared_ptr<class_definition>* pin = object_block::definition_pin(cb); pin && *pin) {
        instance_deleter{std::move(*pin)}(object);
        return;
    }
    class_instance::free_allocation(object);
}

void instance_deleter::operator()(class_instance* ptr) const noexcept {
    if (!ptr) { return; }
    engine* eng = def->get_engine();
    release_machine* m = eng ? engine_access::release_machine_here(*eng) : nullptr;
    const class_definition* constructed = ptr->construction_pending() ? ptr->constructed_level() : nullptr;
    const bool run_chain = !ptr->construction_pending() || constructed;
    if (m && (m->deferring || m->depth >= m->depth_limit)) {
        release_instance_frame(*eng, *m, ptr, def, run_chain, constructed);
        return;
    }
    if (m) { ++m->depth; }
    if (run_chain) {
        const class_definition* now = ptr->get_class_definition();   // a re-classed instance dies as its class now
        if (!now) { now = def.get(); }
        now->destroy_levels(ptr, now, nullptr, constructed);
    }
    class_instance::free_allocation(ptr);
    if (m) { --m->depth; }
}

namespace {
template <typename F>
void release_step(release_machine& m, F&& drop) {
    const size_t mark = m.stack.size();
    const bool deferring = m.deferring;
    const size_t step_mark = m.step_mark;
    m.deferring = true;
    m.step_mark = mark;
    drop();
    m.deferring = deferring;
    m.step_mark = step_mark;
    if (m.stack.size() - mark > 1) { std::reverse(m.stack.begin() + static_cast<ptrdiff_t>(mark), m.stack.end()); }
}
}

void release_instance_frame(engine& eng, release_machine& m, class_instance* instance, std::shared_ptr<class_definition> owner,
                            bool run_chain, const class_definition* constructed) {
    const bool driver = !m.deferring;
    const size_t mark = m.stack.size();
    uint32_t index;
    if (!m.free_frames.empty()) {
        index = m.free_frames.back();
        m.free_frames.pop_back();
    } else {
        index = static_cast<uint32_t>(m.frames.size());
        m.frames.emplace_back();
    }
    release_frame& frame = m.frames[index];
    const class_definition* now = instance->get_class_definition();
    frame.instance = instance;
    frame.owner = std::move(owner);
    frame.allocation = instance->weak_from_this();
    frame.root = now ? now : frame.owner.get();
    frame.kept = nullptr;
    frame.constructed = constructed;
    frame.actions.clear();
    if (run_chain) { frame.actions.push_back({frame.root, 0}); }
    frame.delete_at_end = true;
    m.stack.push_back({script_value(std::monostate{}, &eng), index});
    if (driver) { release_drive(eng, m, mark); }
}

void release_drive(engine& eng, release_machine& m, size_t mark) {
    std::vector<release_action> expanded;
    while (m.stack.size() > mark) {
        const uint32_t index = m.stack.back().frame;
        if (index == release_machine::k_no_frame) {
            script_value dying = std::move(m.stack.back().value);
            m.stack.pop_back();
            release_step(m, [&] { script_value gone = std::move(dying); });
            continue;
        }
        release_frame& frame = m.frames[index];
        class_instance* instance = frame.instance;
        if (frame.actions.empty()) {
            m.stack.pop_back();
            const bool del = frame.delete_at_end;
            std::shared_ptr<class_definition> owner = std::move(frame.owner);
            weaker_ptr<class_instance> allocation = std::move(frame.allocation);
            frame.instance = nullptr;
            m.free_frames.push_back(index);
            if (del) { release_step(m, [&] { class_instance::free_allocation(instance); }); }
            continue;
        }
        const release_action action = frame.actions.back();
        frame.actions.pop_back();
        const class_definition* root = frame.root;
        if (action.level) {
            const class_definition* kept = frame.kept;
            expanded.clear();
            root->expand_destroy_level(instance, action.level, kept, expanded, m.frames[index].constructed);   // the body may grow the frames: re-index after
            auto& actions = m.frames[index].actions;
            actions.insert(actions.end(), expanded.begin(), expanded.end());
        } else {
            script_value dying = root->take_field_for_destroy(instance, action.field);
            if (dying.release_cascades()) { release_step(m, [&] { script_value gone = std::move(dying); }); }
        }
    }
}

release_sync::release_sync(engine* eng) noexcept {
    release_machine* m = eng ? &engine_access::release_machine(*eng) : nullptr;
    if (!m || !m->deferring) { return; }
    m_ = m;
    const size_t step_mark = m->step_mark;
    if (m->stack.size() - step_mark > 1) { std::reverse(m->stack.begin() + static_cast<ptrdiff_t>(step_mark), m->stack.end()); }
    m->deferring = false;
    release_drive(*eng, *m, step_mark);
}

release_sync::~release_sync() {
    if (m_) { m_->deferring = true; }
}

} // namespace detail

// A copy fact under the destructor fact's epoch (value_tree_runs_destructor refreshes it and forgets the copy facts): mode 1 and 2
// walk the value tree, mode 0 asks the chain for a copy constructor, mode 4 for one or a tree that can hold a callable
bool class_definition::copy_fact(uint16_t fact, uint8_t mode) const {
    const uint64_t epoch = engine_ ? detail::engine_access::definition_epoch(*engine_) : 0;
    if (destructor_tree_epoch_ != epoch || !engine_) { value_tree_runs_destructor(); }
    if (!(copy_facts_ & fact)) {
        bool answer;
        if (mode == 0) {
            answer = find_copy_constructor_level() != nullptr;
        } else if (mode == 4) {
            const bool constructed = copy_fact(copy_fact_level, 0);
            answer = copy_fact(copy_fact_callable, 3) || constructed;
        } else {
            std::vector<const class_definition*> in_progress;
            answer = value_tree_runs_destructor_impl(in_progress, mode);
        }
        copy_facts_ = static_cast<uint16_t>(copy_facts_ | fact | (answer ? fact << 1 : 0));
    }
    return (copy_facts_ & (fact << 1)) != 0;
}

bool class_definition::value_tree_copies_observably() const { return copy_fact(copy_fact_tree, 1); }

bool class_definition::value_tree_runs_copy_constructor() const { return copy_fact(copy_fact_ctor_tree, 2); }

bool class_definition::value_tree_holds_callable() const { return copy_fact(copy_fact_callable, 3); }

uint8_t class_definition::copy_care() const {
    if (engine_ && destructor_tree_epoch_ == detail::engine_access::definition_epoch(*engine_) && (copy_facts_ & copy_fact_care)) [[likely]] { return copy_care_; }
    copy_care_ = 0;
    if (copy_fact(copy_fact_care, 4)) {
        copy_care_ = static_cast<uint8_t>(((copy_facts_ & (copy_fact_callable << 1)) ? copy_care_watch : 0) | ((copy_facts_ & (copy_fact_level << 1)) ? copy_care_constructor : 0));
    }
    return copy_care_;
}

namespace {
// A constructor's one parameter names the class: T(const T&) or T(T&)
bool declares_copy_constructor(const class_definition& cls, const function_decl& ctor) {
    if (ctor.parameters.size() != 1) { return false; }
    const parameter& p = ctor.parameters[0];
    const type_info* t = p.type.get();
    if (t && t->base_type == script_value_type::jai_reference_type) { t = t->type_params.empty() ? nullptr : t->type_params[0].get(); }
    else if (!p.is_reference) { return false; }
    return t && t->base_type == script_value_type::jai_object_type && cls.get_name() == t->type_name;
}
}

const class_definition* class_definition::find_copy_constructor_level() const {
    if (is_script_class()) {
        for (const auto& ctor : static_cast<const script_class_definition*>(this)->get_constructor_asts()) {
            if (ctor && declares_copy_constructor(*this, *ctor)) { return this; }
        }
    }
    for (const auto& parent : get_parent_classes()) {
        if (const class_definition* level = parent ? parent->find_copy_constructor_level() : nullptr) { return level; }
    }
    return nullptr;
}

const class_definition* class_definition::copy_constructor_level() const {
    return copy_fact(copy_fact_level, 0) ? find_copy_constructor_level() : nullptr;
}

namespace detail {
namespace {
// a copy of the value runs a destructor or a user copy constructor (move false: containers answer for their elements and
// entries), or a move of it runs a user copy constructor (move true: a container moves its storage); a handle copies nothing
bool object_copy_runs(const type_info* tag, const strong_ptr<script_value::object_holder>& holder, bool move) {
    if (tag && tag->base_type == script_value_type::jai_shared_ptr_type) { return false; }
    if (!holder || !holder->is_instance()) { return false; }
    const class_definition* cls = holder->unchecked_instance()->get_class_definition();
    if (!cls || !cls->is_script_class()) { return false; }
    return move ? cls->value_tree_runs_copy_constructor() : cls->value_tree_copies_observably();
}

bool value_copy_runs(const script_value& in, bool move, int depth) {
    const script_value& v = in.deref();
    switch (v.raw_storage_index()) {
    case script_value::TYPEID_OBJECT: return object_copy_runs(v.type_info_raw(), v.unchecked_object_handle(), move);
    case script_value::TYPEID_ARRAY: {
        const auto& node = v.unchecked_get_array_storage();
        if (!node || move) { return false; }
        if (node->kind() == script_array::kind_t::obj) {
            for (const obj_slot& element : node->objs()) { if (object_copy_runs(element.tag.get(), element.handle, false)) { return true; } }
            return false;
        }
        if (node->kind() != script_array::kind_t::hetero) { return false; }
        if (depth > 32) { return true; }
        for (const script_value& element : node->values()) { if (value_copy_runs(element, false, depth + 1)) { return true; } }
        return false;
    }
    case script_value::TYPEID_MAP: {
        if (move) { return false; }
        if (depth > 32) { return true; }
        for (const auto& [key, value] : v.as_map()) {
            if (value_copy_runs(key, false, depth + 1) || value_copy_runs(value, false, depth + 1)) { return true; }
        }
        return false;
    }
    default: return false;
    }
}
}

bool value_copies_observably(const script_value& v) { return value_copy_runs(v, false, 0); }
bool move_runs_copy_constructor(const script_value& v) { return value_copy_runs(v, true, 0); }

bool call_returns_class_value(const call_expr& call, engine& eng) {
    const auto by_value = [](const type_info* t) { return t && t->base_type == script_value_type::jai_object_type && !t->type_name.empty(); };
    std::shared_ptr<const function_decl> decl = call.proven_callee_decl.lock();
    if (!decl) { decl = call.proven_method_decl.lock(); }
    if (decl) { return by_value(decl->return_type.get()); }
    if (call.callee && call.callee->get_type() == node_type::identifier_expr) {
        const auto& name = static_cast<const identifier_expr*>(call.callee.get())->name;
        if (auto cls = engine_access::get_class_definition(eng, std::string(name)); cls && cls->is_script_class()) { return true; }   // a construction
        // a program-scope function the checker left unresolved here: its declared return type
        if (const auto id = engine_access::get_symbolizer(eng)->find(name)) {
            if (const script_value* fn = engine_access::get_global_environment(eng)->get_value_ptr(*id); fn && fn->is_function()) {
                const auto* thunk = fn->unchecked_as_function().target<script_callable_thunk>();
                if (thunk && thunk->payload.kind == script_callable::kind_type::function && thunk->payload.fn) { return by_value(thunk->payload.fn->return_type.get()); }
            }
        }
    }
    return false;
}

strong_ptr<class_instance> copy_construct_instance(const class_instance& source, const class_definition* level) {
    class_definition* cls = source.get_class_definition();
    engine* eng = source.get_engine();
    const auto inert = [&]() {
        auto made = cls->allocate_instance();
        made->set_class_definition(cls);
        made->construction_pending(true);
        return made;
    };
    execution_backend* backend = eng ? engine_access::calling_backend(*eng) : nullptr;
    if (!backend || backend->is_unwinding()) { return inert(); }
    auto constructor = engine_access::get_global_environment(*eng)->get(level->get_name());
    if (!constructor || !constructor.value().is_function()) { return inert(); }
    // the source binds the constructor's const T& as itself: a reference, never a copy
    std::vector<script_value> args{ script_value::make_cell_reference(script_value::make_class_object(*cls, source.weak_from_this().lock(), eng), eng) };
    auto made = constructor.value().as_function()(args);
    if (!made) { throw runtime_error(eng->format_error(made)); }
    if (backend->is_unwinding() || !made.value().is_object()) { return inert(); }
    strong_ptr<class_instance> copy = made.value().get_class_instance();
    if (!copy) { return inert(); }
    if (level != cls) {
        // C++'s implicit copy constructor below `level`: the object becomes the source's class and the fields the levels below
        // `level` declare are copies of the source's
        copy->reclass(std::const_pointer_cast<class_definition>(cls->shared_from_this()), false);
        const uint32_t n = cls->field_slot_count();
        for (uint32_t slot = 0; slot < n; ++slot) {
            const uint64_t id = cls->slot_field(slot);
            if (level->field_slot(id) != class_definition::k_no_field_slot || cls->slot_is_cpp_property(slot)) { continue; }
            if (const script_value* from = source.find_field_value(id)) { copy->set_field(id, from->deref().clone_element()); }
        }
    }
    return copy;
}
}

bool class_definition::value_tree_runs_destructor() const {
    const uint64_t epoch = engine_ ? detail::engine_access::definition_epoch(*engine_) : 0;
    if (destructor_tree_epoch_ == epoch && engine_) { return destructor_tree_cached_; }
    std::vector<const class_definition*> in_progress;
    destructor_tree_cached_ = value_tree_runs_destructor_impl(in_progress);
    destructor_tree_epoch_ = epoch;
    copy_facts_ = 0;
    return destructor_tree_cached_;
}

const std::vector<uint32_t>& class_definition::string_reachable_slots() const {
    const uint64_t epoch = engine_ ? detail::engine_access::class_definition_epoch(*engine_) : 0;
    if (string_slots_epoch_ == epoch && engine_) { return string_slots_cached_; }
    string_slots_cached_.clear();
    std::vector<const class_definition*> in_progress;
    const uint32_t n = field_slot_count();
    for (uint32_t slot = 0; slot < n; ++slot) {
        if (type_can_hold_string(get_field_declared_type(slot_field(slot)), in_progress)) { string_slots_cached_.push_back(slot); }
    }
    string_slots_epoch_ = epoch;
    return string_slots_cached_;
}
bool class_definition::flat_value_semantics_impl(std::vector<const class_definition*>& in_progress, bool host_values) const {
    if (class_type_ != script_class && class_type_ != vm_class) {
        return false;
    }
    if (cpp_base_class_) {
        return false;
    }
    for (const class_definition* visiting : in_progress) {
        if (visiting == this) {
            return false;   // declared-type cycle: unconstructible by value, never flat
        }
    }
    in_progress.push_back(this);
    bool flat = true;
    for (const auto& parent : parent_classes_) {
        if (!parent || !parent->flat_value_semantics_impl(in_progress, host_values)) {
            flat = false;
            break;
        }
    }
    if (flat) {
        for (const auto& [field_id, field_default] : field_defaults_) {
            (void)field_default;
            if (!flat_field_type(get_field_declared_type(field_id), in_progress, host_values)) {
                flat = false;
                break;
            }
        }
    }
    in_progress.pop_back();
    return flat;
}

// Transitive value-closure (any type that cannot point at something else
// qualifies - primitives, strings, containers OF value-closed types to any depth,
// nested flat classes). What poisons: shared_ptr/weak_ptr/function/reference (their
// semantics IS pointing elsewhere) and auto/var (no store-enforced shape).
bool class_definition::flat_field_type(const type_info_ptr& t, std::vector<const class_definition*>& in_progress, bool host_values) const {
    if (!t) {
        return false;   // auto/untyped field: no store-enforced shape to lean on
    }
    switch (t->base_type) {
    case script_value_type::jai_int_type:
    case script_value_type::jai_float_type:
    case script_value_type::jai_bool_type:
    case script_value_type::jai_char_type:
    case script_value_type::jai_string_type:
    case script_value_type::jai_vec2_type:
    case script_value_type::jai_ivec2_type:
    case script_value_type::jai_vec3_type:
    case script_value_type::jai_ivec3_type:
    case script_value_type::jai_vec4_type:
    case script_value_type::jai_color_type:
        return true;
    case script_value_type::jai_array_type:
        return flat_field_type(t->element_type(), in_progress, host_values);
    case script_value_type::jai_map_type:
        return flat_field_type(t->key_type(), in_progress, host_values) &&
               flat_field_type(t->value_type(), in_progress, host_values);
    case script_value_type::jai_object_type: {
        if (!engine_ || t->type_name.empty()) {
            return false;
        }
        const std::string name(t->type_name);
        auto nested = detail::engine_access::get_class_registry(*engine_).find_script_class(name);
        if (!nested && host_values) {
            const auto& hosts = detail::engine_access::get_class_registry(*engine_).cpp_classes_;
            return hosts.find(name) != hosts.end();   // a bound C++ class held by value: its object is proven by identity
        }
        return nested && nested->flat_value_semantics_impl(in_progress, host_values);
    }
    default:
        return false;   // function, shared_ptr/weak_ptr (reference semantics), reference, var
    }
}

checked_result<void> class_registry::register_script_class(std::shared_ptr<script_class_definition> class_def, const class_decl* a_declaration) {
    if (!class_def) {
        return checked_result<void>(make_error_code(runtime_error_code::class_not_found));
    }

    auto& slot = script_classes_[class_def->get_name()];
    const bool first_definition = slot != class_def;   // redefinitions re-register the same object
    slot = class_def;
    script_classes_by_type_id_[class_def->get_type_id()] = class_def;

    validate_class_hierarchy(class_def->get_name());

    if (first_definition) {
        if (engine* eng = class_def->get_engine()) { detail::engine_access::note_class_defined(*eng, class_def->get_name(), a_declaration); }
    }
    return {};
}

checked_result<void> class_registry::register_cpp_class(std::shared_ptr<class_definition> class_def) {
    if (!class_def) {
        return checked_result<void>(make_error_code(runtime_error_code::class_not_found));
    }

    cpp_classes_[class_def->get_name()] = class_def;

    return {};
}

std::shared_ptr<script_class_definition> class_registry::find_script_class(const std::string& name) {
    auto it = script_classes_.find(name);
    return (it != script_classes_.end()) ? it->second : nullptr;
}

std::shared_ptr<script_class_definition> class_registry::find_script_class(uint64_t type_id) const {
    auto it = script_classes_by_type_id_.find(type_id);
    return it != script_classes_by_type_id_.end() ? it->second : nullptr;
}

void class_registry::validate_class_hierarchy(const std::string& class_name) {
    std::set<std::string> visited;
    std::shared_ptr<class_definition> current = find_script_class(class_name);
    while (current) {
        if (visited.count(current->get_name())) {
            throw std::runtime_error("Circular inheritance detected in class: " + class_name);
        }
        visited.insert(current->get_name());
        current = current->get_parent();
    }
}

namespace detail {

namespace {
    // What each live object of a reloaded class (or of a class derived from it) held in the fields whose initializer needs the object
    struct reloaded_object {
        weaker_ptr<class_instance> instance;
        std::vector<uint8_t> held;   // per field: 0 absent, 1 null, 2 a value
    };

    std::vector<reloaded_object> live_fields_before_reload(engine& eng, const class_definition& cls, const std::vector<uint64_t>& fields) {
        std::vector<reloaded_object> result;
        if (fields.empty()) { return result; }
        for (const auto& [name, script_class] : engine_access::get_class_registry(eng).script_classes_) {
            if (!script_class || !script_class->derives_from(&cls)) { continue; }
            for (const auto& weak : script_class->registered_instances()) {
                auto instance = weak.lock();
                if (!instance) { continue; }
                reloaded_object object{weak, {}};
                for (uint64_t field : fields) {
                    const script_value* v = instance->has_field(field) ? instance->find_field_value(field) : nullptr;
                    object.held.push_back(!v ? 0 : v->is_null() ? 1 : 2);
                }
                result.push_back(std::move(object));
            }
        }
        return result;
    }

    // A field a reload added to a live object, or reset to its default, starts as construction would make it: its initializer evaluated
    // with 'this' bound to the object and the declaring class answering for it. An initializer's error stays with its object, as a
    // migrate hook's does: the field keeps its declared default.
    void initialize_reloaded_fields(const class_declaration_host& host, script_class_definition& cls, const std::vector<uint64_t>& fields,
                                    const std::vector<reloaded_object>& objects) {
        for (const auto& object : objects) {
            auto instance = object.instance.lock();
            if (!instance || !instance->get_class_definition()) { continue; }
            script_value this_value = script_value::make_class_object(*instance->get_class_definition(), instance, host.eng);
            auto env = std::make_shared<environment>(host.env, host.symbols, this_value);
            env->set_access_context(&cls);
            env->define(host.symbols->get_this_id(), this_value);
            dispatch_level_scope level_dispatch(*instance, &cls, class_instance::cdtor_phase::construction);
            for (size_t i = 0; i < fields.size(); ++i) {
                const script_value* now = instance->find_field_value(fields[i]);
                const bool fresh = object.held[i] == 0 || (object.held[i] == 2 && (!now || now->is_null()));
                if (!fresh) { continue; }
                auto initializer = cls.get_field_initializer_ast(fields[i]);
                if (!initializer) { continue; }
                auto evaluated = host.eval(initializer, env);
                if (!evaluated) {
                    host.discard_unwinding();
                    continue;
                }
                auto stored = instance->field_store(fields[i], evaluated.value(), [&] { return evaluated.value(); });
                if (stored) { instance->set_field_unchecked(fields[i], std::move(stored.value())); }
            }
        }
    }

    // The synthesized accessors of a field (a user _get_/_set_ keeps its id)
    script_function field_getter(uint64_t field_id) {
        return [field_id](const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch),
                    "Property getter requires 'this' object");
            }
            auto instance = args[0].as<strong_ptr<class_instance>>();
            return instance->get_field(field_id);
        };
    }

    script_function field_setter(uint64_t field_id) {
        return [field_id](const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch),
                    "Property setter requires 'this' and value");
            }
            auto instance = args[0].as<strong_ptr<class_instance>>();
            // Typed fields enforce like locals (declared type; auto infers from the initialized value)
            auto enforced = instance->field_store(field_id, args[1], [&] { return args[1]; });
            if (!enforced) {
                return enforced;
            }
            instance->set_field_unchecked(field_id, enforced.value());
            return std::move(enforced.value());
        };
    }
}

namespace {

    // The instances of generic methods a class holds under one name onto the class, each an overload of that name: the
    // name's earlier overloads give way, a private one keeps its label
    checked_result<void> install_method_instances(class_definition& def, uint64_t name_id, string_symbolizer& symbols) {
        const auto held = def.generic_method_instances().find(name_id);
        if (held == def.generic_method_instances().end()) { return {}; }
        for (const bool is_static : {false, true}) {
            if (std::any_of(held->second.begin(), held->second.end(), [&](const class_definition::generic_method_instance& i) { return i.ast->is_static == is_static; })) {
                def.drop_method_overloads(name_id, is_static);
            }
        }
        for (const auto& instance : held->second) {
            try {
                if (instance.ast->is_static) {
                    def.add_static_script_method(instance.ast->name, instance.ast, instance.env);
                } else {
                    def.add_script_method(instance.ast->name, instance.ast, instance.env);
                }
            } catch (const runtime_error& e) {
                return checked_result<void>(make_error_code(runtime_error_code::type_mismatch), "{0}", symbols.intern(e.what()));
            }
            if (instance.access != access_level::public_access) { def.set_nonpublic_member(name_id, instance.access); }
            symbols.intern(std::string(member_display_name(instance.ast->name)));   // the name its texts show (listed_member_id)
        }
        return {};
    }

    // A unit's instances of a class's generic methods (class_decl::method_instances): each joins the class the declaration
    // names, where that class does not hold it already under the same stamp (detail/generics.hpp), beside the instances
    // other generic methods of the name have under that instance's name. The linker runs it once the class is declared.
    checked_result<void> declare_method_instances(class_decl* decl, const class_declaration_host& host) {
        string_symbolizer* symbols = host.symbols;
        auto global_env = engine_access::get_global_environment(host.eng);
        if (!global_env) { return checked_result<void>(make_error_code(runtime_error_code::engine_destroyed)); }
        std::shared_ptr<class_definition> class_def;
        if (auto existing = global_env->get(symbols->get_class_var_id_with_view(decl->name_id).first); existing && existing.value().is_object()) {
            auto holder = existing.value().get_object_holder();
            if (holder && holder->type_id() == host.class_definition_type_id) { class_def = std::static_pointer_cast<class_definition>(holder->host_object()); }
        }
        for (const auto& member : decl->members) {
            if (!member.declaration || member.declaration->get_type() != node_type::function_decl) { continue; }
            auto ast = std::static_pointer_cast<function_decl>(member.declaration);
            if (!class_def) {   // the linker defines an instance once its class is declared: a host class holds no script method
                return checked_result<void>(make_error_code(runtime_error_code::class_not_found), "Class '{0}' not found", decl->name_id);
            }
            const auto held = class_def->generic_method_instances().find(ast->name_id);
            if (held != class_def->generic_method_instances().end() &&
                std::any_of(held->second.begin(), held->second.end(), [&](const class_definition::generic_method_instance& i) { return i.stamp == ast->instance_stamp; })) { continue; }
            class_definition::generic_method_instance instance;
            instance.stamp = ast->instance_stamp;
            instance.ast = ast;
            instance.env = host.env;
            instance.access = member.visibility == class_decl::Private ? access_level::private_access
                            : member.visibility == class_decl::Protected ? access_level::protected_access : access_level::public_access;
            class_def->hold_generic_method_instance(ast->name_id, std::move(instance));
            JAISCRIPT_TRY(install_method_instances(*class_def, ast->name_id, *symbols));
        }
        return {};
    }

} // namespace

namespace {

    checked_result<void> define_script_class(class_decl* decl, const class_declaration_host& host);

    // The declaration an instance waits for has run: a declaration of its generic's name, and for an instance its unit makes
    // again (class_decl::instance_remade) the declaration of the very definition it was made from, the one that unit runs
    bool generic_declared(engine& eng, const std::string& generic, uint64_t stamp, bool remade) {
        const auto& declared = engine_access::get_class_registry(eng).declared_generics_;
        const auto ran = declared.find(generic);
        return ran != declared.end() && (remade ? ran->second.count(stamp) != 0 : !ran->second.empty());
    }

    // What an instance still waits for: its generic's declaration, then the first class it is built on that is not declared;
    // empty when everything is
    std::string instance_waits_for(engine& eng, const class_decl& decl, std::string_view being_declared = {}) {
        if (!decl.instance_generic.empty() && !generic_declared(eng, std::string(decl.instance_generic), decl.instance_stamp, decl.instance_remade)) {
            return std::string(decl.instance_generic);
        }
        for (const std::string_view need : decl.instance_needs) {
            if (need != being_declared && !engine_access::get_class_definition(eng, std::string(need))) { return std::string(need); }
        }
        if (!decl.method_instances || !decl.instance_remade) { return {}; }
        for (const auto& member : decl.members) {   // a method's instance made again: its class as the declaration that holds the new generic leaves it
            if (!member.declaration || member.declaration->get_type() != node_type::function_decl) { continue; }
            const auto& fn = static_cast<const function_decl&>(*member.declaration);
            std::string generic(instance_generic_name(fn.instance_name));
            if (!generic_declared(eng, generic, fn.instance_stamp, true)) { return generic; }
        }
        return {};
    }

    // What a waiting instance is known by: a class's name, or a method's with its class's and its definition's shape (two
    // generic methods that take the same type arguments are two instances of one name; one declared again is the same instance)
    std::string instance_key(const class_decl& decl) {
        std::string key(decl.name);
        if (!decl.method_instances) { return key; }
        for (const auto& member : decl.members) {
            if (!member.declaration || member.declaration->get_type() != node_type::function_decl) { continue; }
            const auto& fn = static_cast<const function_decl&>(*member.declaration);
            key += ' ';
            key += fn.instance_name;
            key += parameter_signature(fn.parameters);
        }
        return key;
    }

    // An instance with all it is built on declared: a method's joins its class, a class's is declared as a class written
    // here would be (in a namespace, it is the namespace's class too, as the backends make a class their namespace_decl runs)
    checked_result<void> define_instance(class_decl* decl, const class_declaration_host& host) {
        if (decl->method_instances) { return declare_method_instances(decl, host); }
        JAISCRIPT_TRY(define_script_class(decl, host));
        const size_t cut = decl->instance_generic.rfind("::");
        if (cut != std::string_view::npos) {
            auto& spaces = engine_access::script_namespaces(*host.eng);
            if (const auto space = spaces.find(host.symbols->intern(decl->instance_generic.substr(0, cut))); space != spaces.end() && space->second) {
                register_namespace_class(*host.eng, *space->second, *decl);
            }
        }
        return {};
    }

    // Every waiting instance that everything it is built on is declared for now, in the order offered; defining one may
    // declare what another waited for
    checked_result<void> link_instances(engine& eng) {
        class_registry& registry = engine_access::get_class_registry(eng);
        for (;;) {
            auto& waiting = registry.waiting_instances_;
            const auto ready = std::find_if(waiting.begin(), waiting.end(), [&](const class_registry::waiting_instance& w) { return instance_waits_for(eng, *w.decl).empty(); });
            if (ready == waiting.end()) { return {}; }
            const class_registry::waiting_instance held = std::move(*ready);
            waiting.erase(ready);
            JAISCRIPT_TRY(define_instance(held.decl, class_declaration_host{held.eng, held.symbols, held.env, held.class_definition_type_id, held.class_lookup, held.unit,
                                                                           held.eval, held.discard_unwinding}));
        }
    }

    // A unit offers an instance: it waits when something it is built on is not declared yet (a later offer of it replaces
    // the earlier one), else it is defined now
    checked_result<void> offer_instance(class_decl* decl, const class_declaration_host& host) {
        class_registry& registry = engine_access::get_class_registry(*host.eng);
        std::string key = instance_key(*decl);
        auto& waiting = registry.waiting_instances_;
        const auto earlier = std::find_if(waiting.begin(), waiting.end(), [&](const class_registry::waiting_instance& w) { return w.key == key; });
        if (instance_waits_for(*host.eng, *decl).empty()) {
            if (earlier != waiting.end()) { waiting.erase(earlier); }
            return define_instance(decl, host);
        }
        class_registry::waiting_instance parked{std::move(key), engine_access::instance_owner(*host.eng, decl), decl, host.eng, host.symbols, host.env,
                                                host.class_definition_type_id, host.class_lookup, host.unit, host.eval, host.discard_unwinding};
        if (earlier != waiting.end()) { *earlier = std::move(parked); } else { waiting.push_back(std::move(parked)); }
        return {};
    }

} // namespace

namespace {
    // An instance waits for a generic no unit has declared, and a plain class or function holds that name here: no later
    // declaration is what the use lacks, so the text says what stands in the generic's place
    std::string held_plain_text(engine& eng, std::string_view instance, const std::string& generic, std::string_view kind) {
        if (engine_access::get_class_registry(eng).declared_generics_.count(generic) != 0) { return {}; }
        const bool held = kind == "class" ? engine_access::get_class_definition(eng, generic) != nullptr : eng.has_function(generic);
        if (!held) { return {}; }
        return "'" + std::string(instance) + "' cannot be made: '" + generic + "' is a plain " + std::string(kind) + " here, not a generic";
    }
}

std::string waiting_instance_text(engine& eng, const std::string& name) {
    const std::string key = type_name_key(name);   // the name arrives as text
    for (const auto& w : engine_access::get_class_registry(eng).waiting_instances_) {
        if (w.decl->method_instances || type_name_key(w.decl->name) != key) { continue; }
        const std::string waits_for = instance_waits_for(eng, *w.decl);
        if (waits_for == w.decl->instance_generic) {
            if (std::string plain = held_plain_text(eng, w.decl->name, waits_for, "class"); !plain.empty()) { return plain; }
        }
        return "'" + std::string(w.decl->name) + "' is not declared yet: it waits for '" + waits_for + "'";
    }
    return {};
}

std::string_view undeclared_instance_text(engine& eng, uint64_t id) {
    class_registry& registry = engine_access::get_class_registry(eng);
    if (registry.waiting_instances_.empty() && registry.waiting_functions_.empty()) { return {}; }
    string_symbolizer& symbols = *engine_access::get_symbolizer(eng);
    if (symbols.frozen()) { return {}; }   // a parallel region interns nothing: its error keeps the plain text
    std::string_view name = symbols.get_string(id);
    if (name.starts_with("__class_")) { name.remove_prefix(8); }   // a class is looked up by its variable (get_class_var_id_with_view)
    if (name.find('<') == std::string_view::npos) { return {}; }
    std::string text = waiting_instance_text(eng, std::string(name));
    if (text.empty()) {   // a function's instance, bound when its generic's declaration runs
        for (const auto& w : registry.waiting_functions_) {
            if (symbols.get_string(w.name_id) != name) { continue; }
            text = w.space ? std::string() : held_plain_text(eng, name, w.generic, "function");
            if (text.empty()) {
                text = "'" + (w.space ? std::string(symbols.get_string(w.space_id)) + "::" : std::string()) + std::string(name) + "' is not declared yet: it waits for '" + w.generic + "'";
            }
            break;
        }
    }
    return text.empty() ? std::string_view() : symbols.intern_with_view(text).second;
}

namespace {

    // the '::' that ends a qualified name's scope, outside any angle brackets; npos for a bare name
    size_t scope_cut(std::string_view name) {
        size_t cut = std::string_view::npos;
        int depth = 0;
        for (size_t i = 0; i + 1 < name.size(); ++i) {
            if (name[i] == '<') { ++depth; }
            else if (name[i] == '>') { --depth; }
            else if (depth == 0 && name[i] == ':' && name[i + 1] == ':') { cut = i++; }
        }
        return cut;
    }

} // namespace

std::string_view instance_generic_name(std::string_view instance) {
    if (instance.empty() || instance.back() != '>') { return instance; }
    int depth = 0;
    for (size_t i = instance.size(); i-- > 0;) {
        if (instance[i] == '>') { ++depth; }
        else if (instance[i] == '<' && --depth == 0) { return instance.substr(0, i); }
    }
    return instance;
}

std::vector<uint64_t> declared_instance_stamps(engine& eng, const std::string& name) {
    std::vector<uint64_t> stamps;
    if (const auto cls = engine_access::get_class_definition(eng, name)) {
        if (cls->generic_stamp() != 0) { stamps.push_back(cls->generic_stamp()); }
        return stamps;
    }
    const std::string_view generic = instance_generic_name(name);
    const size_t cut = scope_cut(generic);
    if (cut == std::string_view::npos) {   // a program-scope function, alone or in its name's overload set
        const auto global = engine_access::get_global_environment(eng);
        const script_value* held = global ? global->get_own_value_ptr(engine_access::symbolize(eng, name)) : nullptr;
        const script_callable* callable = held ? script_callable_of(*held, &eng) : nullptr;
        if (!callable) { return stamps; }
        const auto note = [&](const std::shared_ptr<script_defined_function>& fn) { if (fn && fn->generic_stamp != 0) { stamps.push_back(fn->generic_stamp); } };
        if (callable->kind == script_callable::kind_type::function) { note(callable->fn); }
        else if (callable->kind == script_callable::kind_type::overload_set && callable->overloads()) { for (const auto& member : callable->overloads()->members) { note(member); } }
        return stamps;
    }
    const std::string scope(generic.substr(0, cut));
    if (const auto owner = engine_access::get_class_definition(eng, scope)) {   // a generic method's, on its class: one per overload
        const auto held = owner->generic_method_instances().find(engine_access::symbolize(eng, name));
        if (held != owner->generic_method_instances().end()) { for (const auto& instance : held->second) { stamps.push_back(instance.stamp); } }
        return stamps;
    }
    auto& spaces = engine_access::script_namespaces(eng);   // a namespace's function
    const auto space = spaces.find(engine_access::symbolize(eng, scope));
    if (space == spaces.end() || !space->second) { return stamps; }
    const auto overloads = space->second->functions.find(engine_access::symbolize(eng, std::string_view(name).substr(cut + 2)));
    if (overloads == space->second->functions.end()) { return stamps; }
    for (const auto& fn : overloads->second) { if (fn && fn->instance_stamp != 0) { stamps.push_back(fn->instance_stamp); } }
    return stamps;
}

namespace {

    checked_result<void> bind_function(engine& eng, environment& env, uint64_t name_id, const std::shared_ptr<script_defined_function>& fn, script_value value, bool is_override) {
        if (!fn) { env.define(name_id, std::move(value)); return {}; }   // a coroutine's function joins no overload set
        auto declared = declare_function_overload(env, name_id, fn, std::move(value), &eng, is_override);
        if (!declared) { return declared.error_value(); }
        env.define(name_id, std::move(declared.value()));
        engine_access::bump_definition_epoch(eng);
        return {};
    }

} // namespace

bool namespace_function_waits(engine& eng, const std::shared_ptr<script_namespace_data>& space, uint64_t space_id, const function_decl& decl) {
    if (decl.instance_stamp == 0 || decl.method_instance) { return false; }
    class_registry& registry = engine_access::get_class_registry(eng);
    std::string generic(instance_generic_name(decl.instance_name));
    // a namespace's instance is declared as the declaration it was made from runs: one made from a declaration that collided
    // (declare_script_class) never is, and what the namespace holds stays
    if (generic_declared(eng, generic, decl.instance_stamp, true)) { return false; }
    auto& waiting = registry.waiting_functions_;
    const auto earlier = std::find_if(waiting.begin(), waiting.end(), [&](const class_registry::waiting_function& w) {
        return w.space == space && w.name_id == decl.name_id && w.declaration && same_signature(w.declaration->parameters, decl.parameters);
    });
    class_registry::waiting_function parked{std::move(generic), decl.instance_stamp, true, nullptr, decl.name_id, nullptr, script_value(std::monostate{}, &eng), false, space, space_id,
                                            std::make_shared<function_decl>(decl)};
    if (earlier != waiting.end()) { *earlier = std::move(parked); } else { waiting.push_back(std::move(parked)); }
    return true;
}

checked_result<void> declare_script_function(engine& eng, const function_decl& decl, const std::shared_ptr<environment>& env,
                                             const std::shared_ptr<script_defined_function>& fn, script_value value) {
    if (decl.instance_stamp != 0 && !decl.method_instance) {
        class_registry& registry = engine_access::get_class_registry(eng);
        std::string generic(instance_generic_name(decl.instance_name));
        if (!generic_declared(eng, generic, decl.instance_stamp, decl.instance_remade)) {   // a later offer of the instance replaces the earlier one
            auto& waiting = registry.waiting_functions_;
            const auto earlier = std::find_if(waiting.begin(), waiting.end(), [&](const class_registry::waiting_function& w) {
                return w.env == env && w.name_id == decl.name_id && (w.fn && fn ? same_signature(w.fn->parameters(), fn->parameters()) : !w.fn && !fn);
            });
            class_registry::waiting_function parked{std::move(generic), decl.instance_stamp, decl.instance_remade, env, decl.name_id, fn, std::move(value), decl.is_override};
            if (earlier != waiting.end()) { *earlier = std::move(parked); } else { waiting.push_back(std::move(parked)); }
            return {};
        }
    }
    return bind_function(eng, *env, decl.name_id, fn, std::move(value), decl.is_override);
}

checked_result<void> declare_script_class(class_decl* decl, const class_declaration_host& host) {
    if (decl->generic_marker) {   // a generic's declaration ran: the instances that waited for it
        class_registry& registry = engine_access::get_class_registry(*host.eng);
        if (const auto& declared = decl->marker_definition; declared && declared->kind == generic_kind::function && !declared->space.empty()) {
            // a namespace's function: the declaration another unit ran for these value parameters stands unless this one overrides
            auto& standing = registry.namespace_generics_[std::string(decl->name)];
            const auto held = std::find_if(standing.begin(), standing.end(), [&](const class_registry::namespace_generic& g) { return same_generic_signature(*g.definition, *declared); });
            if (held != standing.end() && held->unit != host.unit.id && !generic_declared_override(*declared)) {
                const size_t cut = declared->space.rfind("::");
                return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
                    "Function '{0}' already exists in namespace '{1}'. Use 'override' keyword to replace it.",
                    host.symbols->intern(declared->name), host.symbols->intern(cut == std::string::npos ? declared->space : declared->space.substr(cut + 2)));
            }
            if (held != standing.end()) { *held = {declared, host.unit.id}; } else { standing.push_back({declared, host.unit.id}); }
        }
        registry.declared_generics_[std::string(decl->name)].insert(decl->marker_stamp);
        for (size_t i = 0; i < registry.waiting_functions_.size();) {
            if (const auto& w = registry.waiting_functions_[i]; w.generic != decl->name || !generic_declared(*host.eng, w.generic, w.stamp, w.remade)) { ++i; continue; }
            const class_registry::waiting_function held = std::move(registry.waiting_functions_[i]);
            registry.waiting_functions_.erase(registry.waiting_functions_.begin() + static_cast<std::ptrdiff_t>(i));
            if (held.space) {   // a namespace's: as its namespace_decl would have declared it here
                auto& overloads = held.space->functions[held.name_id];
                std::string refusal;
                if (admit_namespace_function(overloads, *held.declaration, false, refusal) != namespace_admission::admitted) { continue; }
                overloads.push_back(held.declaration);
                publish_namespace_function(*host.eng, *held.space, held.space_id, held.name_id);
                continue;
            }
            JAISCRIPT_TRY(bind_function(*host.eng, *held.env, held.name_id, held.fn, held.value, held.is_override));
        }
        return link_instances(*host.eng);
    }
    if (decl->instance_stamp != 0 || decl->method_instances) {
        JAISCRIPT_TRY(offer_instance(decl, host));
        return link_instances(*host.eng);
    }
    // class Num : Comparable<Num>: a base that is an instance waiting for this very class is declared as this declaration
    // begins (the class is named, which is all its base needs of it; neither could be declared first otherwise)
    for (const std::string_view base : decl->base_classes) {
        auto& waiting = engine_access::get_class_registry(*host.eng).waiting_instances_;
        const auto held = std::find_if(waiting.begin(), waiting.end(), [&](const class_registry::waiting_instance& w) {
            return !w.decl->method_instances && w.decl->name == base && instance_waits_for(*host.eng, *w.decl, decl->name).empty();
        });
        if (held == waiting.end()) { continue; }
        const class_registry::waiting_instance instance = std::move(*held);
        waiting.erase(held);
        JAISCRIPT_TRY(define_instance(instance.decl, class_declaration_host{instance.eng, instance.symbols, instance.env, instance.class_definition_type_id,
                                                                              instance.class_lookup, instance.unit, instance.eval, instance.discard_unwinding}));
    }
    JAISCRIPT_TRY(define_script_class(decl, host));
    return link_instances(*host.eng);   // the instances that waited for this class
}

namespace {

// A base no class names: an unbound spelling of a host template lists the bound instances, as every other use of it does
JAI_NOINLINE checked_result<void> base_class_not_found(engine* eng, string_symbolizer* symbols, std::string_view base_name, uint64_t base_name_id) {
    if (const std::string unbound = eng ? detail::unbound_instantiation_text(*eng, base_name) : std::string(); !unbound.empty()) {
        return checked_result<void>(make_error_code(runtime_error_code::class_not_found), "{0}", symbols->intern(unbound));
    }
    return checked_result<void>(make_error_code(runtime_error_code::class_not_found), "Base class '{0}' not found", base_name_id);
}

checked_result<void> define_script_class(class_decl* decl, const class_declaration_host& host) {
    engine* eng = host.eng;
    string_symbolizer* symbols = host.symbols;
    const std::shared_ptr<environment>& env = host.env;

    std::shared_ptr<script_class_definition> class_def;
    bool is_redefinition = false;

    auto [class_var_id, class_var_name_view] = symbols->get_class_var_id_with_view(decl->name_id);

    auto global_env = engine_access::get_global_environment(eng);
    if (!global_env) {
        return checked_result<void>(make_error_code(runtime_error_code::engine_destroyed));
    }
    auto existing_result = global_env->get(class_var_id);
    if (existing_result) {
        script_value existing = std::move(existing_result.value());
        if (!existing.is_null() && existing.is_object()) {
            auto objHolder = existing.get_object_holder();
            if (objHolder && objHolder->type_id() == host.class_definition_type_id) {
                auto held = std::static_pointer_cast<class_definition>(objHolder->host_object());
                if (!held->is_script_class()) {
                    const uint64_t name_id = (decl->name_id != UINT64_MAX) ? decl->name_id : symbols->intern(decl->name);
                    return checked_result<void>(make_error_code(runtime_error_code::type_mismatch), k_host_class_name_taken, name_id, name_id);
                }
                class_def = std::static_pointer_cast<script_class_definition>(held);
                is_redefinition = true;
            }
        }
    }

    // A generic's instance (detail/generics.hpp): every unit that uses it declares it, and where the class is already defined
    // from the same definition this declaration does nothing (its statics keep their values)
    if (class_def && decl->instance_stamp != 0 && class_def->generic_stamp() == decl->instance_stamp) { return {}; }

    if (!class_def) {
        uint64_t type_id = (decl->name_id != UINT64_MAX) ? decl->name_id : symbols->intern(decl->name);
        class_def = std::make_shared<script_class_definition>(decl->name, type_id, eng);
    } else {
        class_def->clear_asts();
        class_def->clear_instance_method_overloads();
        class_def->clear_static_method_overloads();
    }

    class_def->nocopy(decl->nocopy);
    class_def->set_unit(host.unit);

    std::unordered_map<uint64_t, script_value> new_field_defaults;
    std::unordered_map<uint64_t, type_info_ptr> new_field_types;    // declared types (typed fields enforce like locals)
    std::unordered_map<uint64_t, type_info_ptr> new_static_types;   // declared static types ('auto' = null entry)
    std::unordered_map<uint64_t, script_value> new_methods;
    std::unordered_map<uint64_t, script_value> new_static_methods;
    std::set<uint64_t> new_static_field_ids;

    if (!decl->members.empty()) {
        new_field_defaults.reserve(decl->members.size());
        new_methods.reserve(decl->members.size());
        new_static_methods.reserve(decl->members.size());
    }

    if (!decl->base_classes.empty()) {
        std::vector<std::shared_ptr<class_definition>> parent_defs;
        parent_defs.reserve(decl->base_classes.size());

        for (std::string_view base_name : decl->base_classes) {
            uint64_t base_name_id = symbols->intern(base_name);
            auto [base_class_var_id, base_class_var_name] = symbols->get_class_var_id_with_view(base_name_id);
            script_value base_class_var(std::monostate{}, eng);
            auto base_result = env->get(base_class_var_id);
            if (base_result) {
                base_class_var = std::move(base_result.value());
            }

            std::shared_ptr<class_definition> base_class_def;

            if (!base_class_var.is_null() && base_class_var.is_object()) {
                auto objHolder = base_class_var.get_object_holder();
                if (objHolder && objHolder->type_id() == host.class_definition_type_id) {
                    base_class_def = std::static_pointer_cast<class_definition>(objHolder->host_object());
                } else {
                    return checked_result<void>(make_error_code(runtime_error_code::type_mismatch));
                }
            } else if (host.class_lookup) {
                if (auto cpp_class_def = host.class_lookup(std::string(base_name))) {
                    base_class_def = cpp_class_def;
                } else if (env->contains(base_name_id)) {
                    return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
                        "Constructor found for '{0}' but no class definition available", base_name_id);
                } else {
                    return base_class_not_found(eng, symbols, base_name, base_name_id);
                }
            } else if (env->contains(base_name_id)) {
                return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
                    "Script class inheriting from C++ class '{0}' requires engine integration", base_name_id);
            } else {
                return base_class_not_found(eng, symbols, base_name, base_name_id);
            }

            if (base_class_def && base_class_def->is_final()) {
                return checked_result<void>(make_error_code(runtime_error_code::type_mismatch), "{0}",
                    symbols->intern(detail::final_base_text(decl->name, base_class_def->get_name())));
            }
            if (base_class_def) {
                parent_defs.push_back(base_class_def);
                auto script_class = std::dynamic_pointer_cast<script_class_definition>(base_class_def);
                if (!script_class && parent_defs.size() == 1) {
                    class_def->set_cpp_base_class(base_class_def);
                }
            }
        }

        if (const auto diamond = class_definition::find_diamond(parent_defs); diamond.shared) {
            return checked_result<void>(make_error_code(runtime_error_code::multiple_inheritance), "{0}",
                symbols->intern(detail::diamond_inheritance_text(decl->name, diamond.shared->get_name(), diamond.first->get_name(), diamond.second->get_name())));
        }
        if (!parent_defs.empty() && !class_def->set_parents(parent_defs)) {
            return checked_result<void>(make_error_code(runtime_error_code::type_mismatch));
        }
    } else if (is_redefinition && !class_def->get_parent_classes().empty()) {
        (void)class_def->set_parents({});
    }

    class_def->declare_implicit_default_constructor();
    apply_member_access_labels(*class_def, *decl, *symbols);

    if (decl->base_classes.size() > 1) {
        // C++ refuses an ambiguous inherited field; a field the class declares itself shadows its parents'
        std::unordered_set<uint64_t> derived_field_names;
        for (const auto& member : decl->members) {
            if (member.declaration->get_type() == node_type::variable_decl) {
                auto* var_decl = static_cast<variable_decl*>(member.declaration.get());
                if (!var_decl->is_static) {
                    derived_field_names.insert(var_decl->name_id);
                }
            }
        }
        std::unordered_map<uint64_t, std::vector<std::string>> field_sources;
        for (const auto& parent : class_def->get_parent_classes()) {
            for (const auto& [field_id, unused_default] : parent->get_all_field_defaults()) {
                if (derived_field_names.find(field_id) == derived_field_names.end()) {
                    field_sources[field_id].push_back(parent->get_name());
                }
            }
        }
        for (const auto& [field_id, sources] : field_sources) {
            if (sources.size() > 1) {
                return checked_result<void>(make_error_code(runtime_error_code::multiple_inheritance),
                    "Field inherited from multiple parents", field_id, decl->name_id);
            }
        }
    }

    bool found_constructor = false;

    // Reflection's field order: instance fields then statics, each in declaration order
    std::vector<uint64_t> reflect_field_order_instance;
    std::vector<uint64_t> reflect_field_order_static;
    for (const auto& member : decl->members) {
        auto* var_decl = member.declaration->get_type() == node_type::variable_decl
            ? static_cast<variable_decl*>(member.declaration.get()) : nullptr;
        auto* func_decl = member.declaration->get_type() == node_type::function_decl
            ? static_cast<function_decl*>(member.declaration.get()) : nullptr;

        if (var_decl) {
            script_value default_val(std::monostate{}, eng);
            uint64_t field_id = var_decl->name_id;
            expression_ptr initializer_ast = nullptr;

            if (var_decl->initializer) {
                auto* assign_expr = var_decl->initializer->get_type() == node_type::assignment_expr
                    ? static_cast<assignment_expr*>(var_decl->initializer.get()) : nullptr;
                if (assign_expr) {
                    if (assign_expr->target->get_type() == node_type::identifier_expr) {
                        field_id = static_cast<identifier_expr*>(assign_expr->target.get())->symbol_id;
                    }
                    initializer_ast = assign_expr->value;
                } else {
                    initializer_ast = var_decl->initializer;
                }
            }

            if (var_decl->is_static) {
                if (field_id != 0) {
                    new_static_field_ids.insert(field_id);
                    reflect_field_order_static.push_back(field_id);
                }
                // A reload preserves a static's runtime value; only NEW statics run their initializer
                bool preserve_existing = is_redefinition && field_id != 0 && class_def->has_static_field(field_id) && !var_decl->is_const;   // a class constant has no state to keep

                if (initializer_ast && !preserve_existing) {
                    auto init_result = host.eval(initializer_ast, nullptr);
                    if (!init_result) {
                        return init_result.error_value();
                    }
                    default_val = initializer_store_value(std::move(init_result.value()), initializer_ast.get());
                }
                // Declared type ('auto' parses as null and infers; 'var' stores the any tag); the initializer converts through
                // the field table like an instance initializer
                if (field_id != 0) {
                    new_static_types[field_id] = var_decl->type;
                }
                if (!preserve_existing && var_decl->type && var_decl->type->base_type != script_value_type::jai_any_type) {
                    if (default_val.is_null() && var_decl->type->base_type != script_value_type::jai_object_type) {   // a class value or enum static holds no null
                        default_val.set_type_info(var_decl->type);
                    } else {
                        auto converted = field_store_convert(default_val, var_decl->type, eng, field_id, class_def.get(), true);
                        if (!converted) {
                            return converted.error_value();
                        }
                        default_val = std::move(converted.value());
                    }
                }

                if (field_id != 0 && !preserve_existing) {
                    class_def->add_static_field(field_id, default_val);
                }
                if (field_id != 0) { class_def->mark_const_static(field_id, var_decl->is_const); }
            } else if (field_id != 0) {
                reflect_field_order_instance.push_back(field_id);
                if (initializer_ast) {
                    class_def->add_field_initializer_ast(field_id, initializer_ast);
                }
                new_field_defaults[field_id] = default_val;
                if (var_decl->type) {
                    new_field_types[field_id] = var_decl->type;
                }
            }
        } else if (func_decl) {
            auto method_name = func_decl->name;

            if (method_name == decl->name) {
                found_constructor = true;
                class_def->add_constructor_from_ast(std::static_pointer_cast<function_decl>(member.declaration));
            } else if (method_name.size() > 1 && method_name[0] == '~') {   // "~" alone is operator~
                class_def->add_destructor_from_ast(std::static_pointer_cast<function_decl>(member.declaration), env);
                if (is_redefinition && eng) {   // the redefinition's method set replaces the class's, so the new destructor joins it
                    const uint64_t destructor_id = class_def->destructor_id(*eng);
                    new_methods[destructor_id] = class_def->get_method(destructor_id, false);
                }
            } else {
                uint64_t method_id = func_decl->name_id;
                auto method_ast = std::static_pointer_cast<function_decl>(member.declaration);

                if (is_redefinition) {
                    if (method_ast->is_static) {
                        class_def->add_static_script_method(method_name, method_ast, env);
                        new_static_methods[method_id] = class_def->get_static_method(method_id, false);
                    } else {
                        class_def->add_script_method(method_name, method_ast, env);
                        new_methods[method_id] = class_def->get_method(method_id, false);
                    }
                } else {
                    try {
                        if (method_ast->is_static) {
                            class_def->add_static_script_method(method_name, method_ast, env);
                        } else {
                            class_def->add_method_from_ast(method_name, method_ast, env, is_redefinition);
                        }
                    } catch (const runtime_error& e) {
                        std::string error_msg = e.what();
                        if (error_msg.find("Undefined variable") == std::string::npos) {
                            throw;
                        }
                    }
                }
            }
        }
    }

    {
        // TYPED ctor value: exec_new and the call paths recover the payload through target<script_callable_thunk>() and run
        // construction and the body without a native re-entry; every other caller goes through operator()
        script_callable ctor_payload;
        ctor_payload.kind = script_callable::kind_type::constructor;
        ctor_payload.more().cls = class_def;
        const bool namespace_member = env->is_program_scope() && env != global_env;
        class_def->set_namespace_scope(namespace_member ? env : nullptr);
        if (found_constructor) {
            ctor_payload.more().definition_env = namespace_member ? env : global_env;
        }
        global_env->define(decl->name_id, script_value::make_function(script_callable_thunk{eng, std::move(ctor_payload)}, eng));
    }

    reflect_field_order_instance.insert(reflect_field_order_instance.end(), reflect_field_order_static.begin(), reflect_field_order_static.end());
    class_def->set_field_order(std::move(reflect_field_order_instance));

    // Every name an initializer list gives is a base or one of the class's instance fields, named once (super(...) is the first
    // base); a delegation stands alone
    for (const auto& member : decl->members) {
        const auto* ctor = member.declaration && member.declaration->get_type() == node_type::function_decl
            ? static_cast<const function_decl*>(member.declaration.get()) : nullptr;
        if (!ctor || ctor->name != decl->name) { continue; }
        std::vector<int> named(decl->base_classes.size(), 0);
        std::vector<std::string_view> fields_named;
        for (const auto& init : ctor->initializers) {
            using kind_t = constructor_initializer::kind_t;
            if (init.kind == kind_t::delegation) {
                if (ctor->initializers.size() > 1) {
                    const constructor_initializer& other = &init == &ctor->initializers.front() ? ctor->initializers[1] : ctor->initializers.front();
                    return checked_result<void>(make_error_code(runtime_error_code::no_constructor_found), k_delegation_alone, decl->name_id, symbols->intern(other.target));
                }
                continue;
            }
            if (init.kind == kind_t::field) {
                if (std::find(fields_named.begin(), fields_named.end(), init.target) != fields_named.end()) {
                    return checked_result<void>(make_error_code(runtime_error_code::no_constructor_found), k_field_named_twice, decl->name_id, symbols->intern(init.target));
                }
                fields_named.push_back(init.target);
                continue;
            }
            if (init.kind == kind_t::unresolved) {
                const uint64_t named_id = symbols->intern(init.target);
                return checked_result<void>(make_error_code(runtime_error_code::no_constructor_found),
                                            class_def->has_static_field(named_id) ? k_static_in_initializer_list : k_not_a_base, decl->name_id, named_id);
            }
            size_t index = init.target == "super" && !decl->base_classes.empty() ? 0 : SIZE_MAX;
            for (size_t i = 0; index == SIZE_MAX && i < decl->base_classes.size(); ++i) {
                if (initializer_names(init.target, decl->base_classes[i])) { index = i; }
            }
            if (index == SIZE_MAX) {
                if (init.target == "super") { continue; }   // a base-less class's super() raises when it runs
                return checked_result<void>(make_error_code(runtime_error_code::no_constructor_found), k_not_a_base, decl->name_id, symbols->intern(init.target));
            }
            if (++named[index] == 2) {
                return checked_result<void>(make_error_code(runtime_error_code::no_constructor_found), k_base_named_twice, decl->name_id,
                                            symbols->intern(decl->base_classes[index]));
            }
        }
    }

    if (is_redefinition) {
        std::unordered_map<uint64_t, script_value> field_defaults_with_engine;
        field_defaults_with_engine.reserve(new_field_defaults.size());
        std::vector<uint64_t> per_instance_fields;   // initializers that need the object they initialize

        for (const auto& [field_id, value] : new_field_defaults) {
            auto initializer_ast = class_def->get_field_initializer_ast(field_id);
            script_value evaluated_value = value;

            if (initializer_ast) {
                auto init_result = host.eval(initializer_ast, nullptr);
                if (init_result) {
                    evaluated_value = initializer_store_value(std::move(init_result.value()), initializer_ast.get());
                } else {
                    host.discard_unwinding();
                    per_instance_fields.push_back(field_id);
                }
            }
            field_defaults_with_engine[field_id] = evaluated_value;
        }

        std::unordered_set<uint64_t> synthesized_accessor_ids;
        for (const auto& [field_id, default_val] : field_defaults_with_engine) {
            auto [getter_id, getter_view] = symbols->get_getter_id_with_view(field_id);
            if (!new_methods.count(getter_id)) {
                new_methods[getter_id] = script_value::make_function(field_getter(field_id), eng);
                synthesized_accessor_ids.insert(getter_id);
            }
            auto [setter_id, setter_view] = symbols->get_setter_id_with_view(field_id);
            if (!new_methods.count(setter_id)) {
                new_methods[setter_id] = script_value::make_function(field_setter(field_id), eng);
                synthesized_accessor_ids.insert(setter_id);
            }
        }

        // Declared types install first, so redefine_class flags a retype as fields_changed and migrate_fields converts
        // against the new types
        class_def->replace_field_declared_types(std::move(new_field_types), true);
        class_def->replace_static_declared_types(std::move(new_static_types));

        // An identical reload (position-insensitive AST key) adopts the fresh ASTs and skips per-instance migration
        std::vector<uint8_t> structural_key;
        bool structurally_identical = false;
        if (class_def->structural_key_shape_matches(decl->members.size(), decl->base_classes.size())) {
            try {
                structural_key = structural_node_key(decl, *symbols);
                structurally_identical = (structural_key == class_def->structural_key());
            } catch (...) {
                structural_key.clear();
            }
        }

        auto reloaded_fields = live_fields_before_reload(*eng, *class_def, per_instance_fields);
        // a changed base list: each ancestor's fields a live object never held start as construction makes them
        std::vector<std::tuple<script_class_definition*, std::vector<uint64_t>, std::vector<reloaded_object>>> inherited;
        if (class_def->parents_changed()) {
            for (const class_definition* level : class_def->construction_order()) {
                if (level == class_def.get() || !level->is_script_class()) { continue; }
                std::vector<uint64_t> fields;
                level->for_each_own_field_id([&](uint64_t id) { fields.push_back(id); });
                auto objects = live_fields_before_reload(*eng, *class_def, fields);
                inherited.emplace_back(const_cast<script_class_definition*>(static_cast<const script_class_definition*>(level)), std::move(fields), std::move(objects));
            }
        }
        class_def->redefine_class(new_field_defaults, field_defaults_with_engine, new_methods, new_static_methods, eng, structurally_identical, host.discard_unwinding);
        class_def->set_auto_accessor_ids(std::move(synthesized_accessor_ids));
        initialize_reloaded_fields(host, *class_def, per_instance_fields, reloaded_fields);
        for (auto& [level, fields, objects] : inherited) { initialize_reloaded_fields(host, *level, fields, objects); }

        if (structural_key.empty()) {
            try {
                structural_key = structural_node_key(decl, *symbols);
            } catch (...) {
                structural_key.clear();
            }
        }
        if (!structural_key.empty()) {
            class_def->set_structural_key(std::move(structural_key), decl->members.size(), decl->base_classes.size());
        }

        class_def->retain_static_fields(new_static_field_ids);
        host.discard_unwinding();
        env->clear_all_parent_caches();
    } else {
        class_def->replace_field_declared_types(std::move(new_field_types), false);
        class_def->replace_static_declared_types(std::move(new_static_types));
        for (const auto& [field_id, default_val] : new_field_defaults) {
            std::string field_name(symbols->get_string(field_id));
            class_def->add_field(field_name, default_val);
            auto [getter_id, getter_view] = symbols->get_getter_id_with_view(field_id);
            if (!class_def->has_method(getter_id)) { class_def->add_method_by_id(getter_id, field_getter(field_id), true); }
            auto [setter_id, setter_view] = symbols->get_setter_id_with_view(field_id);
            if (!class_def->has_method(setter_id)) { class_def->add_method_by_id(setter_id, field_setter(field_id), true); }
        }
    }

    auto register_result = engine_access::get_class_registry(*eng).register_script_class(class_def, decl);
    if (!register_result) {
        return register_result;
    }
    engine_access::bump_class_definition_epoch(*eng);
    class_def->set_generic_stamp(decl->instance_stamp);
    for (const auto& member : decl->members) {   // its generic methods are declared as it is: their instances wait for this
        if (!member.declaration || member.declaration->get_type() != node_type::generic_decl) { continue; }
        const auto& definition = static_cast<const generic_decl&>(*member.declaration).definition;
        if (definition) { engine_access::get_class_registry(*eng).declared_generics_[definition->qualified_name()].insert(definition->stamp); }
    }
    if (is_redefinition) {   // the instances of its generic methods, which the units that call them declared
        for (const auto& [name_id, instances] : class_def->generic_method_instances()) {
            JAISCRIPT_TRY(install_method_instances(*class_def, name_id, *symbols));
        }
    }

    global_env->define(class_var_id, script_value::make_object("class_definition", host.class_definition_type_id, class_def, eng));
    return {};
}

} // namespace

} // namespace detail

void class_definition::hold_generic_method_instance(uint64_t name_id, generic_method_instance instance) {
    auto& held = generic_method_instances_[name_id];
    const auto earlier = std::find_if(held.begin(), held.end(), [&](const generic_method_instance& i) {
        return i.ast->is_static == instance.ast->is_static && detail::same_signature(i.ast->parameters, instance.ast->parameters);
    });
    if (earlier != held.end()) { *earlier = std::move(instance); } else { held.push_back(std::move(instance)); }
}

void class_definition::add_method(const std::string& name, script_function func, size_t arity,
                                  std::vector<param_type_info> param_types, param_type_info result) {
    auto eng = engine_;
    if (!eng) return;

    if (name.size() > 5 && name.substr(0, 5) == "_get_") {
        has_property_getters_ = true;
    }

    uint64_t name_id = detail::engine_access::symbolize(*eng, name);
    // A first user _get_/_set_ (or one replacing the synthesized accessor) changes the field
    // ladder: armed field proofs and member caches must miss
    const bool accessor_set_changes = (name.rfind("_get_", 0) == 0 || name.rfind("_set_", 0) == 0) &&
        (methods_.find(name_id) == methods_.end() || auto_accessor_ids_.find(name_id) != auto_accessor_ids_.end());

    if (arity != SIZE_MAX) {
        auto& bucket = cpp_method_overloads_[name_id][arity];
        auto existing = std::find_if(bucket.begin(), bucket.end(),
            [&](const cpp_overload_entry& e) { return e.param_types == param_types; });
        if (existing != bucket.end()) {
            existing->func = func;  // re-bind same signature
            existing->result = result;
        } else {
            bucket.push_back(cpp_overload_entry{ std::move(param_types), func, result });
        }
        method_arities_[name_id].push_back(arity);

        methods_.insert_or_assign(name_id, script_value::make_function(cpp_method_dispatch{ this, name_id, &cpp_method_overloads_[name_id] }, engine_));
    } else {
        methods_.insert_or_assign(name_id, script_value::make_function(
            [func, eng](const std::vector<script_value>& raw_args) -> checked_result<script_value> {
                std::vector<script_value> scratch;
                try {
                    return func(detail::deref_builtin_args(raw_args, scratch));
                } catch (const std::exception& e) {
                    return host_exception_result(eng, e);
                }
            },
            engine_
        ));
    }
    auto_accessor_ids_.erase(name_id);
    if (accessor_set_changes) { bump_field_shape_epoch(); }
    bump_method_epoch();
}

void class_definition::add_static_method(const std::string& name, script_function func, size_t arity) {
    auto eng = engine_;
    if (!eng) return;

    if (name.size() > 5 && name.substr(0, 5) == "_get_") {
        has_property_getters_ = true;
    }

    uint64_t name_id = detail::engine_access::symbolize(*eng, name);
    add_static_method(name_id, std::move(func), arity);
}

[[noreturn]] JAI_NOINLINE static void throw_static_method_arity(const std::string& qualified, size_t expected, size_t got) {
    throw runtime_error("'" + qualified + "' expects " + detail::arity_detail(expected, got));
}

void class_definition::add_static_method(uint64_t name_id, script_function func, size_t arity) {
    const auto* writeback = func.target<host_writeback_function>();
    const uint64_t writeback_mask = writeback ? writeback->mask : 0;
    std::string qualified = arity == SIZE_MAX ? std::string() : name_ + "::" + std::string(detail::engine_access::get_symbolizer(*engine_)->get_string(name_id));
    script_function call = [func, eng = engine_, arity, qualified = std::move(qualified)](const std::vector<script_value>& raw_args) -> checked_result<script_value> {
        std::vector<script_value> scratch;
        try {
            if (arity != SIZE_MAX && raw_args.size() != arity) [[unlikely]] { throw_static_method_arity(qualified, arity, raw_args.size()); }
            return func(detail::deref_builtin_args(raw_args, scratch));
        } catch (const std::exception& e) {
            return host_exception_result(eng, e);
        }
    };
    if (writeback_mask != 0) { call = host_writeback_function{ std::move(call), writeback_mask }; }
    static_methods_.insert_or_assign(name_id, script_value::make_function(std::move(call), engine_));
    bump_method_epoch();

    if (arity != SIZE_MAX) {
        static_method_arities_.insert_or_assign(name_id, arity);
    } else {
        static_method_arities_.erase(name_id);
    }
}

void class_definition::add_field(std::string_view name, const script_value& default_value) {
    auto eng = engine_;
    if (!eng) return;

    uint64_t name_id = detail::engine_access::symbolize(*eng, name);
    if (std::find(bound_field_order_.begin(), bound_field_order_.end(), name_id) == bound_field_order_.end()) { bound_field_order_.push_back(name_id); }

    field_defaults_.insert_or_assign(name_id, default_value);
    field_defaults_cache_valid_ = false; ++defaults_version_;
    field_slots_current_ = false;
    bump_field_shape_epoch();
    bump_method_epoch();   // method ICs memoize the field-shadow answer per class
}

void class_definition::add_field(const std::string& name) {
    auto eng = engine_;
    if (!eng) return;

    uint64_t name_id = detail::engine_access::symbolize(*eng, name);
    if (std::find(bound_field_order_.begin(), bound_field_order_.end(), name_id) == bound_field_order_.end()) { bound_field_order_.push_back(name_id); }
    field_defaults_.insert_or_assign(name_id, script_value(std::monostate{}, engine_));
    field_defaults_cache_valid_ = false; ++defaults_version_;
    field_slots_current_ = false;
    bump_field_shape_epoch();
    bump_method_epoch();
}

void class_definition::register_property_setter(uint64_t field_id, uint64_t setter_id) {
    property_setter_ids_[field_id] = setter_id;
    read_only_property_ids_.erase(field_id);
    bump_field_shape_epoch();
}

void class_definition::register_property_read_only(uint64_t field_id) {
    read_only_property_ids_.insert(field_id);
    bump_field_shape_epoch();
}

void class_definition::register_property_getter(uint64_t field_id, uint64_t getter_id) {
    property_getter_ids_[field_id] = getter_id;
    bump_field_shape_epoch();
}

void class_definition::register_property_member(uint64_t field_id, cpp_member_accessor accessor) {
    property_member_accessors_[field_id] = std::move(accessor);
    bump_field_shape_epoch();
}

void class_definition::register_host_member_lane(uint64_t field_id, std::shared_ptr<const detail::host_member_lane> lane) {
    if (lane) {
        host_member_lanes_[field_id] = std::move(lane);
    } else {
        host_member_lanes_.erase(field_id);
    }
    bump_method_epoch();
}

void class_definition::add_method(const std::string& name, script_function func, size_t arity) {
    add_method(name, std::move(func), arity, {});
}

void class_definition::rebuild_field_slots_once() const {
    while (field_slots_building_.test_and_set(std::memory_order_acquire)) { field_slots_building_.wait(true, std::memory_order_relaxed); }
    if (!field_slots_current_.load(std::memory_order_acquire)) { rebuild_field_slots(); }
    field_slots_building_.clear(std::memory_order_release);
    field_slots_building_.notify_all();
}

void class_definition::rebuild_declared_slot_types() const {
    std::vector<type_info_ptr> types;
    types.reserve(slot_to_field_.size());
    for (const uint64_t field : slot_to_field_) { types.push_back(field_declared_type_walk(field)); }
    slot_declared_types_ = std::move(types);
    slot_declared_epoch_ = field_shape_epoch_;
}

void class_definition::rebuild_field_slots() const {
    cpp_property_slots_.assign(slot_to_field_.size(), 0);
    std::function<void(const class_definition&)> walk = [&](const class_definition& cd) {
        for (const auto& parent : cd.parent_classes_) {
            if (parent) walk(*parent);
        }
        const uint8_t cpp_declared = cd.is_cpp_class() ? 1 : 0;
        cd.for_each_own_field_id([&](uint64_t id) {
            auto it = field_slots_.find(id);
            if (it == field_slots_.end()) {
                it = field_slots_.emplace(id, static_cast<uint32_t>(slot_to_field_.size())).first;
                slot_to_field_.push_back(id);
                cpp_property_slots_.push_back(0);
            }
            // A binder .property() placeholder marks its slot; a script declaration
            // anywhere in the chain (walked base-first) shadows it
            cpp_property_slots_[it->second] = cpp_declared;
        });
    };
    walk(*this);
    has_cpp_property_slots_ = std::find(cpp_property_slots_.begin(), cpp_property_slots_.end(), 1) != cpp_property_slots_.end();
    rebuild_declared_slot_types();
    field_slots_current_.store(true, std::memory_order_release);
}

[[nodiscard]] bool class_definition::add_parent(std::shared_ptr<class_definition> parent) {
    if (!parent) return true;

    for (const auto& existing : parent_classes_) {
        if (existing.get() == parent.get()) {
            return true;
        }
    }

    parent_classes_.push_back(parent);
    field_defaults_cache_valid_ = false; ++defaults_version_;
    field_slots_current_ = false;

    if (has_diamond_inheritance()) {
        parent_classes_.pop_back();
        return false;
    }

    parent->add_derived_class(shared_from_this());
    if (parent->has_property_getters()) {
        has_property_getters_ = true;
    }
    refresh_access_chain_flag();
    bump_field_shape_epoch();
    bump_method_epoch();   // resolution chain changed

    return true;
}

void class_definition::intern_name() const {
    const auto [id, view] = detail::engine_access::get_symbolizer(*engine_)->intern_with_view(name_);
    interned_name_id_ = id;
    interned_name_ = view;
}

void class_definition::rename(const std::string& name, uint64_t type_id) {
    name_ = name;
    type_id_ = type_id;
}

bool class_definition::has_live_instances() const {
    for (const class_instance* i = registry_head_; i; i = i->registry_next_) {
        if (!i->weak_from_this().expired()) { return true; }
    }
    return false;
}

// an overloaded name's lane: the one overload whose parameters the arguments match exactly, else the ordinary call
static bool dispatch_native_overloads(const void* state, void* self, const script_value& receiver, script_value* args, size_t argc,
                                      script_value& out) {
    const auto& overloads = *static_cast<const class_definition::native_overload_set*>(state);
    const class_definition::native_overload* pick = nullptr;
    for (const auto& overload : overloads) {
        if (overload.exact(args, argc)) {
            if (pick) { return false; }
            pick = &overload;
        }
    }
    return pick && pick->lane(pick->state.get(), self, receiver, args, argc, out);
}

bool class_definition::script_declares_user_accessor(uint64_t id) const {
    if (!is_script_class()) { return false; }
    const script_value* found = user_accessor(id);
    if (!found) { return false; }
    auto cpp_base = find_cpp_base_class();
    return !(cpp_base && cpp_base->user_accessor(id) == found);
}

void class_definition::add_native_method(uint64_t id, native_method fn) {
    native_entry& e = native_methods_[id];
    e.plain = fn;
    e.lane = nullptr;
    e.state.reset();
    restamp_native_methods();
}

void class_definition::add_native_lane(uint64_t id, native_lane fn, std::shared_ptr<const void> state, native_exact exact) {
    restamp_native_methods();
    if (native_lane_names_.count(id)) { return; }
    auto [it, fresh] = native_methods_.try_emplace(id);
    if (!fresh && it->second.plain) { return; }
    auto& overloads = native_overload_sets_[id];
    if (!overloads) { overloads = std::make_shared<native_overload_set>(); }
    overloads->push_back(native_overload{ fn, state, exact });
    if (overloads->size() == 1) {
        it->second.lane = fn;
        it->second.state = std::move(state);
        return;
    }
    if (std::any_of(overloads->begin(), overloads->end(), [](const native_overload& o) { return o.exact == nullptr; })) {
        withdraw_native_lane(id);
        return;
    }
    it->second.lane = &dispatch_native_overloads;
    it->second.state = overloads;
}

void class_definition::withdraw_native_lane(uint64_t id) {
    restamp_native_methods();
    native_lane_names_.insert(id);
    native_overload_sets_.erase(id);
    auto it = native_methods_.find(id);
    if (it != native_methods_.end() && !it->second.plain) { native_methods_.erase(it); }
}

void class_definition::set_copy_function(copy_function copier) {
    copy_function_ = std::move(copier);
    bump_method_epoch();   // nocopy verdicts of script classes over this base re-evaluate
}

} // namespace jai
