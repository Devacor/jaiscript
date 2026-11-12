#pragma once
#define __JAISCRIPT_DETAIL_CLASS_RUNTIME_HPP__

// The class runtime: the member bodies, slot stores, assignments and copies the library's own translation units (the
// interpreter, the VM, the engine) compile and inline. Hosts include core/class_definition.hpp, which declares them.
#include <jaiscript/core/class_definition.hpp>
#include <jaiscript/detail/fault_injection.hpp>
#include <jaiscript/detail/numeric_conversion.hpp>
#include <jaiscript/detail/math_values.hpp>
#include <jaiscript/detail/container_enforce.hpp>
#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/slot_site.hpp>
#include <jaiscript/detail/arity_diagnostic.hpp>
#include <jaiscript/detail/host_member.hpp>
#include <jaiscript/detail/object_borrow.hpp>
#include <jaiscript/detail/uniquely_held.hpp>
#include <jaiscript/detail/truthiness.hpp>

namespace jai {

namespace detail {
// A region's copy of a class's shared default: a string or a container gets nodes of its own (a plain copy would touch the
// shared node's count from several threads)
inline script_value region_field_default(const script_value& value, engine* eng) {
    switch (value.raw_storage_index()) {
    case script_value::TYPEID_NULL: case script_value::TYPEID_INT: case script_value::TYPEID_FLOAT:
    case script_value::TYPEID_BOOL: case script_value::TYPEID_CHAR:
        return value;
    default:
        return value.parallel_detached_copy(eng);
    }
}
}

inline size_t class_instance::trailing_bytes(uint32_t slots) noexcept { return slots * sizeof(script_value); }

inline class_instance::class_instance(interned_name_t, uint64_t type_id, engine* eng, const trailing_cells& trailing) : class_instance(interned_name_t{}, type_id, eng) {
    if (trailing.memory && trailing.slots) {
        auto* cells = reinterpret_cast<script_value*>(trailing.memory);
        for (uint32_t i = 0; i < trailing.slots; ++i) { new (cells + i) script_value(script_value::make_invalid(eng)); }
        assert(cells == flat_fields_.trailing() && "the trailing cells start where the instance ends");
        trailing_cells_ = true;
        flat_fields_.adopt_implicit(trailing.slots);
    }
}

inline void class_instance::free_allocation(class_instance* instance) noexcept {
    detail::control_block_base* cb = detail::cb_from_object(instance);
    instance->~class_instance();
    if (cb->drop_weak() == 0 && cb->strong_count == 0) { cb->dealloc_fn(cb); }
}

namespace detail {
static_assert(cb_storage_offset<class_instance> == k_instance_cb_offset, "object_ref finds an instance's control block a fixed distance before it");
static_assert(sizeof(void*) != 8 || sizeof(class_instance) <= 72, "an instance's header stays within 72 bytes");

// An instance handed to its own destructor body as `this`: a dying one (its last strong reference gone, its chain running) holds
// a count for the span that goes back without a second death; a living one (a re-class) is pinned as usual
class dying_instance_pin {
public:
    explicit dying_instance_pin(class_instance* instance) noexcept : instance_(instance) {
        control_block_base* cb = cb_from_object(instance);
        revived_ = cb->get_strong_count() == 0;
        cb->add_strong();
    }
    dying_instance_pin(const dying_instance_pin&) = delete;
    dying_instance_pin& operator=(const dying_instance_pin&) = delete;
    ~dying_instance_pin() {
        if (revived_) {
            (void)cb_from_object(instance_)->drop_strong();
        } else {
            strong_ptr<class_instance> pinned = adopt_pooled(instance_);
        }
    }
    strong_ptr<class_instance> handle() const noexcept {
        cb_from_object(instance_)->add_strong();
        return adopt_pooled(instance_);
    }
private:
    class_instance* instance_;
    bool revived_ = false;
};

// The instance's object block, given back when its strong family's last weak reference goes
inline void object_block::release(control_block_base* cb) noexcept {
#ifndef NDEBUG
    cb->magic = cb_magic_dead;
#endif
    static_cast<control_block<class_instance>*>(cb)->~control_block();
    ::operator delete(static_cast<void*>(cb));
}

inline void object_block::release_pinned(control_block_base* cb) noexcept {
#ifndef NDEBUG
    cb->magic = cb_magic_dead;
#endif
    auto* memory = reinterpret_cast<std::byte*>(cb) - pin_bytes;
    reinterpret_cast<std::shared_ptr<class_definition>*>(memory)->~shared_ptr();
    static_cast<control_block<class_instance>*>(cb)->~control_block();
    ::operator delete(static_cast<void*>(memory));
}

inline std::shared_ptr<class_definition>* object_block::definition_pin(control_block_base* cb) noexcept {
    return cb->dealloc_fn == &release_pinned
        ? reinterpret_cast<std::shared_ptr<class_definition>*>(reinterpret_cast<std::byte*>(cb) - pin_bytes) : nullptr;
}
}

inline strong_ptr<class_instance> class_instance::make(std::string_view class_name, engine* eng) {
    return make(eng ? detail::engine_access::get_symbolizer(*eng)->intern(class_name) : UINT64_MAX, eng);
}

inline strong_ptr<class_instance> class_instance::make(uint64_t type_id, engine* eng, uint32_t slots, class_definition* destructor_owner) {
    using block_cb = detail::control_block<class_instance>;
    static_assert(sizeof(block_cb) == detail::k_instance_cb_offset + sizeof(class_instance), "the cells follow the instance");
    const size_t prefix = destructor_owner ? detail::object_block::pin_bytes : 0;
    auto* memory = static_cast<std::byte*>(::operator new(prefix + sizeof(block_cb) + trailing_bytes(slots)));
    if (destructor_owner) { new (memory) std::shared_ptr<class_definition>(destructor_owner->shared_from_this()); }
    auto* cb = new (memory + prefix) block_cb();
    cb->dealloc_fn = destructor_owner ? &detail::object_block::release_pinned : &detail::object_block::release;
    const trailing_cells trailing{memory + prefix + sizeof(block_cb), slots};
    auto* instance = new (cb->storage) class_instance(interned_name_t{}, type_id, eng, trailing);
    if (destructor_owner) { instance->note_destructor_armed(); }
    return detail::adopt_pooled(instance);
}

inline bool class_instance::has_field_value(uint64_t id) const { return find_field_value(id) != nullptr; }

inline bool class_instance::has_user_overflow_fields() const { return overflow_fields() && !overflow_fields()->empty(); }

inline void class_instance::note_field_ref_released() noexcept { if (live_field_refs_) { --live_field_refs_; } }

[[noreturn]] inline void class_instance::throw_retired_script_layer() const {
    throw runtime_error("Script layer '" + std::string(get_class_name()) + "' was detached (script_layer_detach, or a move to another engine): this handle no longer reaches its object");
}

inline script_value class_instance::take_field_slot(uint32_t slot) {
    if (slot < flat_fields_.size()) {
        if (flat_fields_.implicit()) {
            if (script_value* c = flat_fields_.present_cell(slot)) { return std::move(*c); }
        } else if (flat_fields_[slot]) {
            return std::move(*flat_fields_[slot]);
        }
    }
    return script_value(std::monostate{}, owner_);
}

inline script_value* class_instance::field_slot_value(uint32_t slot) {
    if (slot >= flat_fields_.size()) {
        return nullptr;
    }
    return flat_fields_.present_cell(slot);
}

inline void class_instance::field_slot_write(uint32_t slot, script_value&& v) {
    if (slot < flat_fields_.size() && flat_fields_.implicit()) {
        script_value* c = flat_fields_.cell(slot);
        if (c->is_reference()) { c->deref() = std::move(v); } else { *c = std::move(v); }
        return;
    }
    if (slot >= flat_fields_.size()) {
        flat_fields_.resize(static_cast<size_t>(slot) + 1);
    }
    auto& cell = flat_fields_[slot];
    if (!cell) {
        cell = make_cell(std::move(v));
    } else if (cell->is_reference()) {
        cell->deref() = std::move(v);
    } else {
        *cell = std::move(v);
    }
}

inline void class_instance::set_class_definition(const std::shared_ptr<class_definition>& class_def) { set_class_definition(class_def.get()); }

inline void class_instance::dispatch_as(class_definition* level, cdtor_phase phase) {
    class_def_ = level;
    dispatch_phase_ = phase;
}

inline std::shared_ptr<void> class_instance::get_cpp_object() const {
    if (!native_lookup_enabled()) return nullptr;
    if (script_layer_retired_) { throw_retired_script_layer(); }

    const script_value& cpp_field = native_or_throw();
    if (!cpp_field.is_null() && cpp_field.is_object()) {
        return extract_cpp_object_impl(cpp_field);
    }
    return nullptr;
}

inline bool class_instance::has_cpp_object() const {
    const script_value* held = native();
    return held && !held->is_null();
}

inline void* class_instance::cpp_object_pointer() const {
    const script_value* held = native();
    if (!held || !held->is_object()) return nullptr;
    auto holder = held->get_object_holder();
    if (!holder) return nullptr;
    return holder->holds_object() ? holder->object() : holder->bound_object();
}

inline void class_instance::reserve_cell_arena(size_t n) {
    if (trailing_cells_ || !n || (extras_ && extras_->arena)) { return; }
    instance_extras& box = extras();
    box.arena_memory.reset(new std::byte[n * sizeof(script_value)]);
    box.arena = reinterpret_cast<script_value*>(box.arena_memory.get());
    box.arena_cells = static_cast<uint32_t>(n);
}

inline auto class_instance::make_cell(script_value&& v) -> field_cell {
    if (extras_ && extras_->arena_used < extras_->arena_cells) {
        auto* p = extras_->arena + extras_->arena_used++;
        new (p) script_value(std::move(v));
        return field_cell(p, false);
    }
    return field_cell(new script_value(std::move(v)), true);
}

inline auto class_instance::make_trivial_cell(const script_value& v) -> field_cell {
    if (extras_ && extras_->arena_used < extras_->arena_cells) {
        auto* p = extras_->arena + extras_->arena_used++;
        std::memcpy(static_cast<void*>(p), static_cast<const void*>(&v), sizeof(script_value));
        return field_cell(p, false);
    }
    return field_cell(new script_value(v), true);
}

inline auto class_instance::extras() -> instance_extras& {
    if (!extras_) { extras_ = std::make_unique<instance_extras>(); }
    return *extras_;
}

inline void class_instance::region_tag(uint32_t tag) { if (tag || extras_) { extras().region_tag = tag; } }

inline bool class_instance::fields_cascade() const noexcept {
    for (size_t slot = 0; slot < flat_fields_.size(); ++slot) {
        const script_value* cell = flat_fields_.cell(slot);
        if (cell && cell->release_cascades()) { return true; }
    }
    return (overflow_fields() && !overflow_fields()->empty()) || native();
}
inline void class_definition::detach_for_teardown() noexcept {
    for (class_instance* i = registry_head_; i;) {
        class_instance* next = i->registry_next_;
        i->class_def_ = nullptr;
        i->registry_prev_ = i->registry_next_ = nullptr;
        i->registered_.store(false, std::memory_order_relaxed);
        i = next;
    }
    registry_head_ = registry_tail_ = nullptr;
    engine_ = nullptr;
}

inline class_definition::~class_definition() { detach_for_teardown(); }

inline void class_definition::release_values_for_teardown() noexcept {
    methods_.clear();
    ++method_epoch_;
    static_methods_.clear();
    field_defaults_.clear();
    static_field_values_.clear();
    all_field_defaults_cache_.clear();
    instance_plan_.clear();
}

inline class_definition::class_definition(std::string_view name, uint64_t type_id, engine* eng) : name_(name), type_id_(type_id), type_info_(nullptr), engine_(eng),
          null_field_value_(std::monostate{}, eng), class_type_(cpp_class) {
    if (eng) {
        type_info_ = detail::engine_access::get_type_info_object(*eng, type_id_);
    }
}

inline class_definition::class_definition(std::string_view name, uint64_t type_id, class_type type, engine* eng) : name_(name), type_id_(type_id), type_info_(nullptr), engine_(eng),
          null_field_value_(std::monostate{}, eng), class_type_(type) {
    if (eng) {
        type_info_ = detail::engine_access::get_type_info_object(*eng, type_id_);
    }
}

inline void class_definition::bump_method_epoch() {
    ++method_epoch_;
    detail::note_class_layout_change(engine_);
    for (auto& weak_derived : derived_classes_) {
        if (auto derived = weak_derived.lock()) {
            derived->bump_method_epoch();
        }
    }
}

inline void class_definition::bump_field_shape_epoch() {
    ++field_shape_epoch_;
    detail::note_class_layout_change(engine_);
    if (field_slots_current_.load(std::memory_order_acquire)) { rebuild_declared_slot_types(); }   // a definition's thread, between regions
    for (auto& weak_derived : derived_classes_) {
        if (auto derived = weak_derived.lock()) {
            derived->bump_field_shape_epoch();
        }
    }
}

inline void class_definition::add_method_by_id(uint64_t name_id, script_function func, bool is_auto_accessor) {
    auto eng = engine_;
    if (!eng) return;

    if (is_auto_accessor) {
        has_property_getters_ = true;
    }

    methods_.insert_or_assign(name_id, script_value::make_function(func, engine_));
    if (is_auto_accessor) {
        auto_accessor_ids_.insert(name_id);
    } else {
        auto_accessor_ids_.erase(name_id);
        bump_field_shape_epoch();   // a custom _get_/_set_ or a field-named method changes the field ladder
    }
    bump_method_epoch();
}

inline const script_value* class_definition::user_accessor(uint64_t id) const {
    JAI_CENSUS(user_accessor);
    bool script_field_seen = false;
    return user_accessor_walk(id, script_field_seen);
}

inline const script_value* class_definition::user_accessor_walk(uint64_t id, bool& script_field_seen) const {
    if (is_cpp_class() && script_field_seen) { return nullptr; }   // the C++ base sits in parent_classes_ too
    if (auto it = methods_.find(id); it != methods_.end()) {
        if (auto_accessor_ids_.find(id) == auto_accessor_ids_.end()) { return &it->second; }
        script_field_seen = true;
    }
    for (const auto& parent : parent_classes_) {
        if (parent) {
            if (const script_value* found = parent->user_accessor_walk(id, script_field_seen)) { return found; }
        }
    }
    if (cpp_base_class_) { return cpp_base_class_->user_accessor_walk(id, script_field_seen); }
    return nullptr;
}

inline script_value class_definition::find_user_accessor(uint64_t id) const {
    const script_value* found = user_accessor(id);
    return found ? *found : script_value::make_invalid(engine_);
}

inline bool class_definition::is_auto_accessor(uint64_t id) const { return user_accessor(id) == nullptr; }

inline bool class_definition::owns_auto_accessor(uint64_t id) const { return auto_accessor_ids_.find(id) != auto_accessor_ids_.end(); }

inline void class_definition::set_auto_accessor_ids(std::unordered_set<uint64_t> ids) { auto_accessor_ids_ = std::move(ids); }

inline checked_result<script_value> class_definition::host_exception_result(engine* eng, const std::exception& e) {
    return host_exception_error(eng, detail::native_exception_text(e));
}

inline function_decl* class_definition::single_method_overload(uint64_t name_id) const {
    JAI_CENSUS(single_overload);
    auto it = method_overloads_.find(name_id);
    return (it != method_overloads_.end() && it->second.size() == 1) ? it->second[0].get() : nullptr;
}

inline std::shared_ptr<function_decl> class_definition::single_method_overload_ptr(uint64_t name_id) const {
    auto it = method_overloads_.find(name_id);
    return (it != method_overloads_.end() && it->second.size() == 1) ? it->second[0] : nullptr;
}

inline void class_definition::replace_field_declared_types(std::unordered_map<uint64_t, type_info_ptr> new_types, bool mark_changes) {
    auto same_type = [](type_info_ptr a, type_info_ptr b) {
        if (!a || !b) return a == b;
        return a->equals(*b);
    };
    bool changed = field_declared_types_.size() != new_types.size();
    if (!changed) {
        for (const auto& [id, t] : new_types) {
            auto it = field_declared_types_.find(id);
            if (it == field_declared_types_.end() || !same_type(it->second, t)) {
                changed = true;
                break;
            }
        }
    }
    field_declared_types_ = std::move(new_types);
    if (changed) { bump_field_shape_epoch(); }   // declared type changed => epoch moved
    if (changed && mark_changes) {
        declared_types_dirty_ = true;
    }
}

inline void class_definition::destroy_chain(class_instance* instance, const class_definition* level) const { destroy_levels(instance, level, nullptr); }

inline type_info_ptr class_definition::field_declared_type_at(uint32_t slot, uint64_t id) const {
    if (slot_declared_epoch_ == field_shape_epoch_ && slot < slot_declared_types_.size()) { return slot_declared_types_[slot]; }
    return field_declared_type_walk(id);
}

inline type_info_ptr class_definition::get_field_declared_type(uint64_t id) const {
    // a declared field's type by its slot, from the table the layout build and every field-shape change fill (never built here, so a
    // parallel reader only reads); a table behind the current shape answers by the chain walk
    const uint32_t slot = field_slot(id);
    if (slot != k_no_field_slot && slot_declared_epoch_ == field_shape_epoch_ && slot < slot_declared_types_.size()) {
        return slot_declared_types_[slot];
    }
    return field_declared_type_walk(id);
}

inline type_info_ptr class_definition::field_declared_type_walk(uint64_t id) const {
    auto it = field_declared_types_.find(id);
    if (it != field_declared_types_.end()) {
        return it->second;
    }
    for (const auto& parent : parent_classes_) {
        if (parent) {
            if (type_info_ptr t = parent->field_declared_type_walk(id)) {
                return t;
            }
        }
    }
    if (cpp_base_class_) {
        return cpp_base_class_->field_declared_type_walk(id);
    }
    return nullptr;
}

inline void class_definition::add_static_field(uint64_t id, const script_value& initial_value) {
    if (static_fields_.insert(id).second) {
        bump_method_epoch();   // a new static can shadow a global for bare names in the chain
        if (engine_) { detail::engine_access::note_static_field(*engine_, this, id); }
    }
    static_field_values_.insert_or_assign(id, initial_value);
}

inline void class_definition::replace_static_declared_types(std::unordered_map<uint64_t, type_info_ptr> new_types) {
    static_declared_types_ = std::move(new_types);
}

inline type_info_ptr class_definition::get_static_declared_type(uint64_t id) const {
    auto it = static_declared_types_.find(id);
    return it != static_declared_types_.end() ? it->second : nullptr;
}

inline void class_definition::clear_instance_method_overloads() {
    method_overloads_.clear();
    overload_resolution_cache_.clear();
    bump_method_epoch();
}

inline void class_definition::clear_static_method_overloads() {
    static_method_overloads_.clear();
}

inline void class_definition::retain_static_fields(const std::set<uint64_t>& keep) {
    for (auto it = static_fields_.begin(); it != static_fields_.end();) {
        if (keep.find(*it) == keep.end()) {
            static_field_values_.erase(*it);
            static_declared_types_.erase(*it);
            it = static_fields_.erase(it);
        } else {
            ++it;
        }
    }
}

inline const script_value* class_definition::get_static_field_ptr(uint64_t id) const {
    auto it = static_field_values_.find(id);
    if (it != static_field_values_.end()) {
        return &it->second;
    }
    return nullptr;
}

inline script_value* class_definition::get_static_field_ptr(uint64_t id) {
    auto it = static_field_values_.find(id);
    if (it != static_field_values_.end()) {
        return &it->second;
    }
    return nullptr;
}

inline const script_value& class_definition::get_static_field(uint64_t id) const {
    const script_value* ptr = get_static_field_ptr(id);
    if (ptr) {
        return *ptr;
    }
    null_field_value_ = script_value::make_null(engine_);
    return null_field_value_;
}

inline script_value& class_definition::get_static_field(uint64_t id) {
    script_value* ptr = get_static_field_ptr(id);
    if (ptr) {
        return *ptr;
    }
    null_field_value_ = script_value::make_null(engine_);
    return null_field_value_;
}

[[nodiscard]] inline bool class_definition::set_static_field(uint64_t id, const script_value& value) {
    auto it = static_field_values_.find(id);
    if (it != static_field_values_.end()) {
        it->second = value;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool class_definition::set_static_field(uint64_t id, script_value&& value) {
    auto it = static_field_values_.find(id);
    if (it != static_field_values_.end()) {
        it->second = std::move(value);
        return true;
    }
    return false;
}

inline bool class_definition::is_static_field(uint64_t id) const {
    return static_fields_.find(id) != static_fields_.end();
}

inline void class_definition::mark_const_static(uint64_t id, bool is_const) {
    if (is_const) { const_statics_.insert(id); } else { const_statics_.erase(id); }
}

inline bool class_definition::is_const_static(uint64_t id) const {
    if (const_statics_.count(id)) { return true; }
    if (static_fields_.find(id) != static_fields_.end()) { return false; }
    for (const auto& parent : parent_classes_) { if (parent && parent->is_const_static(id)) { return true; } }
    return false;
}

inline bool class_definition::has_static_field(uint64_t id) const {
    return static_field_values_.find(id) != static_field_values_.end();
}

inline script_value class_definition::get_method(uint64_t id, bool throw_if_missing) const {
    JAI_CENSUS(get_method);
    if (const script_value* found = method_ptr(id)) { return *found; }
    if (throw_if_missing) {
        throw_method_missing("Method '", id);
    }
    return script_value::make_invalid(engine_);
}

inline const script_value* class_definition::method_ptr(uint64_t id) const {
    JAI_CENSUS(method_ptr);
    if (auto it = methods_.find(id); it != methods_.end()) { return &it->second; }
    for (const auto& parent : parent_classes_) {
        if (parent) {
            if (const script_value* found = parent->method_ptr(id)) { return found; }
        }
    }
    return cpp_base_class_ ? cpp_base_class_->method_ptr(id) : nullptr;
}

inline const script_value* class_definition::assignment_operator_ptr(uint64_t id) const {
    if (!is_script_class()) { return method_ptr(id); }
    if (auto it = methods_.find(id); it != methods_.end()) { return &it->second; }
    for (const auto& parent : parent_classes_) {
        if (parent && parent->is_script_class()) {
            if (const script_value* found = parent->assignment_operator_ptr(id)) { return found; }
        }
    }
    return nullptr;
}

[[noreturn]] void class_definition::throw_method_missing(const char* kind, uint64_t id) const {
    std::string name;
    if (auto eng = engine_) {
        name = detail::engine_access::get_symbolizer(*eng)->get_string(id);
    } else {
        name = std::to_string(id);
    }
    throw runtime_error(kind + name + "' not found in class '" + name_ + "'");
}

inline bool class_definition::has_method(uint64_t id) const {
    return methods_.find(id) != methods_.end();
}

inline uint64_t class_definition::destructor_id(engine& eng) const {
    if (destructor_id_ == 0) { destructor_id_ = detail::engine_access::symbolize(eng, "~" + name_); }
    return destructor_id_;
}

inline const script_value* class_definition::destructor_body(engine& eng) const {
    if (dtor_body_epoch_ != method_epoch_) {
        auto it = methods_.find(destructor_id(eng));
        dtor_body_ = it != methods_.end() ? &it->second : nullptr;
        dtor_body_epoch_ = method_epoch_;
    }
    return dtor_body_;
}

inline bool class_definition::chain_declares_destructor(engine& eng) const {
    if (has_method(destructor_id(eng))) { return true; }
    for (const auto& parent : parent_classes_) {
        if (parent && parent->chain_declares_destructor(eng)) { return true; }
    }
    return false;
}

inline bool class_definition::script_chain_defines_method(uint64_t id) const {
    if (!is_script_class()) { return false; }
    if (methods_.find(id) != methods_.end() && auto_accessor_ids_.find(id) == auto_accessor_ids_.end()) { return true; }
    for (const auto& parent : parent_classes_) {
        if (parent && parent->script_chain_defines_method(id)) { return true; }
    }
    return false;
}

inline bool class_definition::may_define_to_bool() const {
    if (to_bool_memo_epoch_ != method_epoch_) {
        assert(!(engine_ && detail::engine_access::parallel_region_active(*engine_)) && "a parallel worker writes the class's to_bool memo (prime it at region start)");
        uint64_t id = 0;
        if (auto eng = engine_) { id = detail::engine_access::symbolize(*eng, "to_bool"); }
        may_define_to_bool_ = id == 0 || defines_method(id);
        to_bool_memo_epoch_ = method_epoch_;
    }
    return may_define_to_bool_;
}

inline uint64_t class_definition::script_assign_operator_id() const {
    if (assign_memo_epoch_ != method_epoch_) {
        uint64_t id = 0;
        if (auto eng = engine_) { id = detail::engine_access::symbolize(*eng, "="); }
        script_assign_id_ = id != 0 && script_chain_defines_method(id) ? id : 0;
        assign_memo_epoch_ = method_epoch_;
    }
    return script_assign_id_;
}

inline script_value class_definition::get_static_method(uint64_t id, bool throw_if_missing) const {
    auto it = static_methods_.find(id);
    if (it != static_methods_.end()) {
        return it->second;
    }

    if (throw_if_missing) {
        throw_method_missing("Static method '", id);
    }
    return script_value::make_null(engine_);
}

inline bool class_definition::has_static_method(uint64_t id) const {
    return static_methods_.find(id) != static_methods_.end();
}

inline bool class_definition::declares_instance_member(uint64_t id) const {
    return methods_.find(id) != methods_.end() || field_defaults_.find(id) != field_defaults_.end();
}

inline script_value* class_definition::find_static_field_in_chain(uint64_t id, class_definition** owner) {
    JAI_CENSUS(static_chain);
    if (auto it = static_field_values_.find(id); it != static_field_values_.end()) {
        if (owner) { *owner = this; }
        return &it->second;
    }
    if (declares_instance_member(id)) { return nullptr; }
    for (const auto& parent : parent_classes_) {
        if (parent) {
            if (script_value* found = parent->find_static_field_in_chain(id, owner)) { return found; }
        }
    }
    if (cpp_base_class_) { return cpp_base_class_->find_static_field_in_chain(id, owner); }
    return nullptr;
}

inline const script_value* class_definition::find_static_method_in_chain(uint64_t id, class_definition** owner) {
    JAI_CENSUS(static_chain);
    if (auto it = static_methods_.find(id); it != static_methods_.end()) {
        if (owner) { *owner = this; }
        return &it->second;
    }
    if (declares_instance_member(id)) { return nullptr; }
    for (const auto& parent : parent_classes_) {
        if (parent) {
            if (const script_value* found = parent->find_static_method_in_chain(id, owner)) { return found; }
        }
    }
    if (cpp_base_class_) { return cpp_base_class_->find_static_method_in_chain(id, owner); }
    return nullptr;
}

inline script_value class_definition::static_method_in_chain(uint64_t id) {
    const script_value* found = find_static_method_in_chain(id);
    return found ? *found : script_value::make_null(engine_);
}

inline std::vector<std::string_view> class_definition::get_static_field_names() const {
    std::vector<std::string_view> names;
    names.reserve(static_fields_.size());
    if (auto eng = engine_) {
        for (const auto& field_id : static_fields_) {
            names.push_back(detail::engine_access::get_symbolizer(*eng)->get_string(field_id));
        }
    }
    return names;
}

inline uint64_t class_definition::chain_defaults_version() const {
    uint64_t v = defaults_version_;
    for (const auto& parent : parent_classes_) { if (parent) { v += parent->chain_defaults_version() * 1315423911u; } }
    return v;
}

inline const std::vector<std::pair<uint32_t, script_value>>& class_definition::instance_plan() const {
    const uint64_t want = chain_defaults_version() ^ (field_shape_epoch_ << 32) ^ (method_epoch_ << 48);
    if (instance_plan_version_ == want && (!instance_plan_.empty() || instance_plan_absent_)) { return instance_plan_; }
    instance_plan_.clear();
    instance_plan_version_ = want;
    instance_plan_absent_ = true;
    if (!chain_literal_plan_ok()) { return instance_plan_; }
    std::unordered_map<uint64_t, script_value> merged;
    std::vector<const class_definition*> chain;
    std::function<void(const class_definition&)> walk = [&](const class_definition& cd) {
        for (const auto& parent : cd.parent_classes_) { if (parent) { walk(*parent); } }
        chain.push_back(&cd);
    };
    walk(*this);
    // base-first: a child's default (and literal) overrides its parent's, like the
    // create_instance loops and the initializer passes that follow them
    for (const class_definition* cd : chain) {
        for (const auto& [id, value] : cd->field_defaults_) {
            const script_value* lit = cd->literal_initializer_for(id);
            if (lit) {
                // the stamp's materialize_constant: engine-tagged, strings re-minted so
                // instances never share a literal's buffer
                // exactly visit_literal_expr / materialize_constant: a parser literal has no
                // engine or tag; the value is recreated with the canonical primitive tag
                script_value baked(std::monostate{}, engine_);
                switch (lit->raw_storage_index()) {
                    case script_value::TYPEID_INT: baked = script_value(lit->unchecked_as_int(), engine_); break;
                    case script_value::TYPEID_FLOAT: baked = script_value(lit->unchecked_as_float(), engine_); break;
                    case script_value::TYPEID_STRING: baked = script_value(lit->unchecked_as_string(), engine_); break;
                    case script_value::TYPEID_CHAR: baked = script_value(lit->unchecked_as_char(), engine_); break;
                    case script_value::TYPEID_BOOL: baked = script_value(lit->unchecked_as_bool(), engine_); break;
                    case script_value::TYPEID_NULL: break;
                    default: instance_plan_.clear(); return instance_plan_;   // not a scalar literal: the slow path
                }
                merged.insert_or_assign(id, std::move(baked));
            } else {
                merged.insert_or_assign(id, value);
            }
        }
    }
    instance_plan_.reserve(merged.size());
    for (const auto& [id, value] : merged) {
        const uint32_t slot = field_slot(id);
        if (slot == k_no_field_slot) { instance_plan_.clear(); return instance_plan_; }
        script_value stored = value;
        if (!value.is_null()) {
            type_info_ptr declared;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                if (auto d = (*it)->field_declared_types_.find(id); d != (*it)->field_declared_types_.end()) { declared = d->second; break; }
            }
            if (declared && declared->base_type != script_value_type::jai_any_type) {
                if (auto converted = detail::field_store_convert_no_script(value, declared, engine_)) { stored = std::move(*converted); }
                else { instance_plan_.clear(); return instance_plan_; }
            }
        }
        instance_plan_.emplace_back(slot, std::move(stored));
    }
    std::sort(instance_plan_.begin(), instance_plan_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    instance_plan_absent_ = instance_plan_.empty();
    return instance_plan_;
}

inline void class_definition::prime_region_memos() {
    (void)interned_name();
    (void)field_slot_count();
    (void)instance_plan();
    for (const auto& parent : parent_classes_) {
        if (parent) { parent->prime_region_memos(); }
    }
    (void)get_all_field_defaults();
    (void)construction_order();
    if (dtor_memo_epoch_ != method_epoch_) {
        has_script_dtor_ = false;
        if (auto eng = engine_) {
            has_script_dtor_ = is_script_class() && chain_declares_destructor(*eng);
        }
        dtor_memo_epoch_ = method_epoch_;
    }
    if (auto eng = engine_) {
        (void)destructor_id(*eng);
        (void)destructor_body(*eng);
    }
    (void)value_tree_runs_destructor();
    (void)nocopy();
    (void)flat_value_semantics();
    (void)handle_bearing_slots();
    (void)string_reachable_slots();
    (void)may_define_to_bool();
    (void)script_assign_operator_id();
}

inline strong_ptr<class_instance> class_definition::allocate_instance() {
    // Destructor probe memo: "~"+name_ alloc + symbolizer intern + has_method are
    // class-static facts, recomputed per MINT before this (measured in
    // the construct_instance section profile). method_epoch_ already cascades to
    // derived classes on every method-set mutation, so it is the exact validity key.
    if (dtor_memo_epoch_ != method_epoch_) {
        has_script_dtor_ = false;
        if (auto eng = engine_) {
            has_script_dtor_ = is_script_class() && chain_declares_destructor(*eng);
        }
        dtor_memo_epoch_ = method_epoch_;
    }
    return class_instance::make(interned_name_id(), engine_, field_slot_count(), has_script_dtor_ ? this : nullptr);
}

inline strong_ptr<class_instance> class_definition::create_instance() {
    strong_ptr<class_instance> instance = allocate_instance();

    instance->set_class_definition(this);

    register_live_instance(instance.get());

    // a region's constructions (workers and main's chunk alike) copy no shared default node
    const bool in_region = engine_ && detail::engine_access::parallel_region_active(*engine_);
    if (const auto& plan = instance_plan(); !plan.empty()) {
        instance->seed_fields_from_plan(plan, field_slot_count(), in_region);
        return instance;
    }
    for (const auto& parent : parent_classes_) {
        if (parent) {
            const auto& parent_fields = parent->get_all_field_defaults();
            for (const auto& [field_id, default_value] : parent_fields) {
                if (!instance->has_field(field_id)) {
                    instance->set_field(field_id, in_region ? detail::region_field_default(default_value, engine_) : default_value);
                }
            }
        }
    }

    for (const auto& [field_id, default_value] : field_defaults_) {
        instance->set_field(field_id, in_region ? detail::region_field_default(default_value, engine_) : default_value);
    }

    return instance;
}

inline const std::unordered_map<uint64_t, script_value>& class_definition::get_all_field_defaults() const {
    if (!field_defaults_cache_valid_) {
        all_field_defaults_cache_.clear();

        for (const auto& parent : parent_classes_) {
            if (parent) {
                const auto& parent_fields = parent->get_all_field_defaults();
                for (const auto& [id, value] : parent_fields) {
                    if (all_field_defaults_cache_.find(id) == all_field_defaults_cache_.end()) {
                        all_field_defaults_cache_[id] = value;
                    }
                }
            }
        }

        for (const auto& [id, value] : field_defaults_) {
            all_field_defaults_cache_.insert_or_assign(id, value);
        }

        field_defaults_cache_valid_ = true;
    }

    return all_field_defaults_cache_;
}

inline bool class_definition::slot_prefix_of(const class_definition& derived) const {
    ensure_field_slots();
    derived.ensure_field_slots();
    return slot_to_field_.size() <= derived.slot_to_field_.size() &&
           std::equal(slot_to_field_.begin(), slot_to_field_.end(), derived.slot_to_field_.begin());
}

inline uint32_t class_definition::field_slot_count() const {
    ensure_field_slots();
    return static_cast<uint32_t>(slot_to_field_.size());
}

inline bool class_definition::chain_declares_field(uint64_t id) const {
    JAI_CENSUS(chain_field);
    const auto& all_fields = get_all_field_defaults();
    if (all_fields.find(id) != all_fields.end()) {
        return true;
    }
    if (auto cpp_base = find_cpp_base_class()) {
        const auto& cpp_fields = cpp_base->get_all_field_defaults();
        return cpp_fields.find(id) != cpp_fields.end();
    }
    return false;
}

inline const class_definition* class_definition::find_field_declaring_class(uint64_t id) const {
    if (field_defaults_.find(id) != field_defaults_.end()) {
        return this;
    }
    for (const auto& parent : parent_classes_) {
        if (parent) {
            if (const class_definition* found = parent->find_field_declaring_class(id)) {
                return found;
            }
        }
    }
    if (cpp_base_class_) {
        return cpp_base_class_->find_field_declaring_class(id);
    }
    return nullptr;
}

inline void class_definition::set_parent(std::shared_ptr<class_definition> parent) {
    const bool chain_changed = parent_classes_.size() != (parent ? 1u : 0u) || (parent && parent_classes_[0] != parent);
    parent_classes_.clear();
    if (parent) {
        parent_classes_.push_back(parent);
        parent->add_derived_class(shared_from_this());
        if (parent->has_property_getters()) {
            has_property_getters_ = true;
        }
    }
    field_defaults_cache_valid_ = false; ++defaults_version_;
    field_slots_current_ = false;
    refresh_access_chain_flag();
    if (chain_changed) { bump_field_shape_epoch(); }
    bump_method_epoch();   // resolution chain changed
}

[[nodiscard]] inline bool class_definition::set_parents(const std::vector<std::shared_ptr<class_definition>>& parents) {
    const bool chain_changed = parent_classes_ != parents;   // every (re)definition rewires; identical reloads must not move the shape epoch
    reparented_from_fields_.reset();
    if (chain_changed) {
        std::set<uint64_t> before;
        for (const auto& [id, _] : get_all_field_defaults()) { before.insert(id); }
        reparented_from_fields_ = std::move(before);
    }
    parent_classes_ = parents;
    field_defaults_cache_valid_ = false; ++defaults_version_;
    field_slots_current_ = false;

    if (has_diamond_inheritance()) {
        parent_classes_.clear();
        return false;
    }

    for (auto& parent : parent_classes_) {
        if (parent) {
            parent->add_derived_class(shared_from_this());
            if (parent->has_property_getters()) {
                has_property_getters_ = true;
            }
        }
    }
    refresh_access_chain_flag();
    if (chain_changed) { bump_field_shape_epoch(); }
    bump_method_epoch();   // resolution chain changed

    return true;
}

inline std::shared_ptr<class_definition> class_definition::get_parent() const {
    if (!parent_classes_.empty()) {
        return parent_classes_[0];
    }
    if (cpp_base_class_) {
        return cpp_base_class_;
    }
    return nullptr;
}

inline const class_definition* class_definition::parent_ptr() const {
    return !parent_classes_.empty() ? parent_classes_[0].get() : cpp_base_class_.get();
}

inline const std::vector<const class_definition*>& class_definition::construction_order() const {
    if (construction_order_epoch_ != method_epoch_) {
        construction_order_.clear();
        std::function<void(const class_definition&)> walk = [&](const class_definition& cd) {
            for (const auto& parent : cd.parent_classes_) {
                if (parent) { walk(*parent); }
            }
            construction_order_.push_back(&cd);
        };
        walk(*this);
        construction_order_epoch_ = method_epoch_;
    }
    return construction_order_;
}

inline uint16_t class_definition::constructed_through(const class_definition* level) const {
    const auto& order = construction_order();
    const auto it = std::find(order.begin(), order.end(), level);
    return static_cast<uint16_t>(it == order.end() ? order.size() : (it - order.begin()) + 1);
}

inline auto class_definition::find_diamond(const std::vector<std::shared_ptr<class_definition>>& parents) -> diamond {
    std::unordered_map<const class_definition*, const class_definition*> reached_through;
    for (const auto& base : parents) {
        if (!base) { continue; }
        std::unordered_set<const class_definition*> seen;
        std::vector<const class_definition*> pending{base.get()};
        while (!pending.empty()) {
            const class_definition* current = pending.back();
            pending.pop_back();
            if (!seen.insert(current).second) { continue; }
            const auto [it, fresh] = reached_through.emplace(current, base.get());
            if (!fresh && it->second != base.get()) { return {current, it->second, base.get()}; }
            for (const auto& parent : current->parent_classes_) {
                if (parent) { pending.push_back(parent.get()); }
            }
        }
    }
    return {};
}

inline bool class_definition::has_diamond_inheritance() const { return find_diamond(parent_classes_).shared != nullptr; }

inline void class_definition::add_derived_class(std::weak_ptr<class_definition> derived) {
    derived_classes_.push_back(derived);
}

inline void class_definition::set_cpp_base_class(std::shared_ptr<class_definition> cpp_base) {
    if (cpp_base_class_ != cpp_base) { bump_field_shape_epoch(); }
    cpp_base_class_ = cpp_base;
    if (cpp_base && cpp_base->has_property_getters()) {
        has_property_getters_ = true;
    }
    bump_method_epoch();   // resolution chain changed
}

inline bool class_definition::owns_read_only_property(uint64_t field_id) const {
    return read_only_property_ids_.find(field_id) != read_only_property_ids_.end();
}

inline bool class_definition::is_read_only_property(uint64_t field_id) const {
    if (property_setter_ids_.find(field_id) != property_setter_ids_.end()) { return owns_read_only_property(field_id); }
    for (const auto& parent : parent_classes_) {
        if (parent && parent->get_property_setter_id(field_id)) { return parent->is_read_only_property(field_id); }
    }
    return false;
}

inline uint64_t class_definition::get_property_setter_id(uint64_t field_id) const {
    auto it = property_setter_ids_.find(field_id);
    if (it != property_setter_ids_.end()) {
        return it->second;
    }
    for (const auto& parent : parent_classes_) {
        if (parent) {
            if (uint64_t inherited = parent->get_property_setter_id(field_id)) { return inherited; }
        }
    }
    return 0;
}

inline uint64_t class_definition::get_property_getter_id(uint64_t field_id) const {
    auto it = property_getter_ids_.find(field_id);
    if (it != property_getter_ids_.end()) {
        return it->second;
    }
    for (const auto& parent : parent_classes_) {
        if (parent) {
            if (uint64_t inherited = parent->get_property_getter_id(field_id)) { return inherited; }
        }
    }
    return 0;
}

inline auto class_definition::get_property_member_accessor(uint64_t field_id) const -> const cpp_member_accessor* {
    auto it = property_member_accessors_.find(field_id);
    if (it != property_member_accessors_.end()) {
        return &it->second;
    }
    for (const auto& parent : parent_classes_) {
        if (parent) {
            if (const auto* inherited = parent->get_property_member_accessor(field_id)) { return inherited; }
        }
    }
    return nullptr;
}

inline const detail::host_member_lane* class_definition::host_member_lane_for(uint64_t field_id) const {
    auto it = host_member_lanes_.find(field_id);
    if (it != host_member_lanes_.end()) {
        return it->second.get();
    }
    for (const auto& parent : parent_classes_) {
        if (parent) {
            if (const auto* inherited = parent->host_member_lane_for(field_id)) { return inherited; }
        }
    }
    return nullptr;
}

inline script_value class_definition::unwrap(script_value& wrapper_value) const {
    if (!is_transparent_wrapper_ || !unwrap_function_) {
        return script_value(std::monostate{}, engine_);
    }
    return unwrap_function_(wrapper_value, engine_);
}

inline void class_definition::set_unwrap_function(std::function<script_value(script_value&, engine*)> fn) {
    is_transparent_wrapper_ = true;
    unwrap_function_ = std::move(fn);
}

inline std::vector<weaker_ptr<class_instance>> class_definition::registered_instances() const {
    std::vector<weaker_ptr<class_instance>> out;
    for (class_instance* i = registry_head_; i; i = i->registry_next_) { out.push_back(i->weak_from_this()); }
    return out;
}

inline bool class_definition::has_native_methods() const noexcept { return !native_methods_.empty(); }

inline void class_definition::restamp_native_methods() {
    if (engine_) { detail::engine_access::bump_class_definition_epoch(*engine_); native_stamp_ = detail::engine_access::class_definition_epoch(*engine_); }
    else { ++native_stamp_; }
}

inline void* class_definition::adjust_cpp_pointer(void* pointer, const std::type_info& target) const {
	if (cpp_type_ && *cpp_type_ == target) {
		return pointer;
	}
	for (const auto& edge : cpp_base_casts_) {
		if (edge.base == std::type_index(target)) {
			return edge.adjust(pointer);
		}
	}
	for (const auto& edge : cpp_base_casts_) {
		auto base = detail::engine_access::get_class_definition_by_type(*engine_, edge.base);
		if (base) {
			if (auto* result = base->adjust_cpp_pointer(edge.adjust(pointer), target)) {
				return result;
			}
		}
	}
	return nullptr;
}

inline void class_definition::set_field_order(std::vector<uint64_t> order) { field_order_ = std::move(order); }

inline std::vector<strong_ptr<class_instance>> class_definition::live_instances() const {
    std::vector<strong_ptr<class_instance>> live;
    for (class_instance* i = registry_head_; i; i = i->registry_next_) {
        if (auto locked = i->weak_from_this().lock()) { live.push_back(std::move(locked)); }
    }
    return live;
}

inline std::vector<std::string> class_definition::get_property_names(bool include_inherited) const {
    std::vector<std::string> properties;
    std::unordered_set<std::string> seen;

    if (auto eng = engine_) {
        if (include_inherited) {
            for (const auto& parent : parent_classes_) {
                if (parent) {
                    for (const auto& prop : parent->get_property_names(true)) {
                        if (seen.insert(prop).second) {
                            properties.push_back(prop);
                        }
                    }
                }
            }
            if (cpp_base_class_) {
                for (const auto& prop : cpp_base_class_->get_property_names(true)) {
                    if (seen.insert(prop).second) {
                        properties.push_back(prop);
                    }
                }
            }
        }

        for_each_own_field_id([&](uint64_t id) {
            std::string prop_name(detail::engine_access::get_symbolizer(*eng)->get_string(id));
            if (seen.insert(prop_name).second) {
                properties.push_back(prop_name);
            }
        });
    }
    return properties;
}

inline bool class_definition::is_subtype_of(const std::string& base_class_name) const {
    if (name_ == base_class_name) {
        return true;
    }

    for (const auto& parent : parent_classes_) {
        if (parent && parent->is_subtype_of(base_class_name)) {
            return true;
        }
    }

    if (cpp_base_class_ && cpp_base_class_->is_subtype_of(base_class_name)) {
        return true;
    }

    return false;
}

inline void class_definition::set_nonpublic_members(std::unordered_map<uint64_t, access_level> members) {
    if (nonpublic_members_ != members) { bump_field_shape_epoch(); }   // labels reapply on every (re)definition
    nonpublic_members_ = std::move(members);
    refresh_access_chain_flag();
    bump_method_epoch();   // inline caches stamp a needs-access-check flag at fill
}

inline const class_definition* class_definition::find_nonpublic_declarer(uint64_t member_id, access_level& level_out) const {
    auto it = nonpublic_members_.find(member_id);
    if (it != nonpublic_members_.end()) {
        level_out = it->second;
        return this;
    }
    for (const auto& parent : parent_classes_) {
        if (parent) {
            if (const auto* declarer = parent->find_nonpublic_declarer(member_id, level_out)) {
                return declarer;
            }
        }
    }
    if (cpp_base_class_) {
        return cpp_base_class_->find_nonpublic_declarer(member_id, level_out);
    }
    return nullptr;
}

inline void class_definition::refresh_access_chain_flag() {
    bool flag = !nonpublic_members_.empty();
    if (!flag) {
        for (const auto& parent : parent_classes_) {
            if (parent && parent->chain_has_nonpublic_) { flag = true; break; }
        }
        if (!flag && cpp_base_class_ && cpp_base_class_->chain_has_nonpublic_) flag = true;
    }
    chain_has_nonpublic_ = flag;
    for (const auto& weak_derived : derived_classes_) {
        if (auto derived = weak_derived.lock()) {
            derived->refresh_access_chain_flag();
        }
    }
}

inline void class_definition::nocopy(bool value) { nocopy_ = value; nocopy_memo_epoch_ = 0; }

inline bool class_definition::nocopy() const {
    if (nocopy_memo_epoch_ != method_epoch_) {
        bool verdict = nocopy_;
        for (const auto& parent : parent_classes_) {
            if (verdict) { break; }
            verdict = parent && parent->nocopy();
        }
        if (!verdict && cpp_base_class_) { verdict = !cpp_base_class_->has_copy_function(); }
        nocopy_memo_ = verdict;
        nocopy_memo_epoch_ = method_epoch_;
    }
    return nocopy_memo_;
}

inline std::shared_ptr<void> class_definition::copy_object(const void* src) const {
    if (!copy_function_) {
        throw runtime_error("No copy constructor available for class " + name_);
    }
    return copy_function_(src);
}

inline void class_definition::set_method_metadata(std::string_view name, const method_metadata& metadata) {
    auto eng = engine_;
    if (!eng) return;
    uint64_t name_id = detail::engine_access::symbolize(*eng, name);
    method_metadata_[name_id] = metadata;
}

inline auto class_definition::get_method_metadata(std::string_view name) const -> const method_metadata* {
    auto eng = engine_;
    if (!eng) return nullptr;
    uint64_t name_id = detail::engine_access::symbolize(*eng, name);
    auto it = method_metadata_.find(name_id);
    return it != method_metadata_.end() ? &it->second : nullptr;
}

inline auto class_definition::get_method_metadata_mutable(std::string_view name) -> method_metadata* {
    auto eng = engine_;
    if (!eng) return nullptr;
    uint64_t name_id = detail::engine_access::symbolize(*eng, name);
    auto it = method_metadata_.find(name_id);
    return it != method_metadata_.end() ? &it->second : nullptr;
}

inline auto class_definition::find_method(std::string_view name) -> method_info {
    auto eng = engine_;
    if (!eng) return { nullptr, nullptr };

    uint64_t name_id = detail::engine_access::symbolize(*eng, name);

    if (has_method(name_id)) {
        return { shared_from_this(), get_method_metadata_mutable(name) };
    }
    return find_inherited_method(name_id, name);
}

inline auto class_definition::find_inherited_method(uint64_t name_id, std::string_view name) -> method_info {
    for (const auto& parent : parent_classes_) {
        if (!parent) { continue; }
        if (parent->has_method(name_id)) { return { parent, parent->get_method_metadata_mutable(name) }; }
        if (auto deeper = parent->find_inherited_method(name_id, name); deeper.owner_class) { return deeper; }
    }
    return { nullptr, nullptr };
}

inline void class_definition::set_structural_key(std::vector<uint8_t> key, size_t member_count, size_t base_count) {
    structural_key_ = std::move(key);
    key_member_count_ = member_count;
    key_base_count_ = base_count;
}

inline bool class_definition::structural_key_shape_matches(size_t member_count, size_t base_count) const {
    return !structural_key_.empty() && key_member_count_ == member_count && key_base_count_ == base_count;
}

inline void class_definition::redefine_class(const std::unordered_map<uint64_t, script_value>& new_field_defaults, const std::unordered_map<uint64_t, script_value>& migration_values,
                                             const std::unordered_map<uint64_t, script_value>& new_methods, const std::unordered_map<uint64_t, script_value>& new_static_methods,
                                             engine* engine_ref, bool structurally_identical, const std::function<void()>& discard_unwinding) {
    if (engine_ref) { detail::engine_access::recover_parallel_registrations(*engine_ref); }
    field_defaults_cache_valid_ = false; ++defaults_version_;
    field_slots_current_ = false;
    ++reflection_generation_;
    if (engine_ref) { detail::engine_access::bump_class_definition_epoch(*engine_ref); }
    // a changed base list migrates the whole chain: inherited fields come and go as the class's own do
    const std::optional<std::set<uint64_t>> reparented = std::exchange(reparented_from_fields_, std::nullopt);

    if (structurally_identical && !reparented) {
        // Identical reload (structural AST equality, established by the caller):
        // ADOPT the new ASTs — methods were re-minted from the fresh parse, so
        // debugger/error positions track cosmetic shifts — and skip all
        // per-instance and derived-class work (no field or body changed;
        // set_class_definition would re-point instances at this same object).
        methods_ = new_methods;
        static_methods_ = new_static_methods;
        bump_method_epoch();   // dispatchers re-minted: per-site inline caches must refill
        recompute_property_getters(new_methods, engine_ref);
        store_field_defaults(new_field_defaults, engine_ref);
        method_metadata_.clear();
        ++identical_redefinitions_;
        return;
    }
    bool fields_changed = reparented.has_value() || field_defaults_.size() != new_field_defaults.size();
    if (!fields_changed) {
        for (const auto& [id, _] : field_defaults_) {
            if (new_field_defaults.find(id) == new_field_defaults.end()) {
                fields_changed = true;
                break;
            }
        }
        if (!fields_changed) {
            for (const auto& [id, _] : new_field_defaults) {
                if (field_defaults_.find(id) == field_defaults_.end()) {
                    fields_changed = true;
                    break;
                }
            }
        }
    }

    // A field that kept its id but changed declared type (int -> string) leaves the id-set
    // identical, so the comparison above stays false and migration would be skipped. The old
    // stored default can't reveal this (instance-field defaults are null placeholders), so
    // detect it against a live instance: a held value whose kind no longer matches the
    // initializer's reload value means the type changed. migrate_fields then
    // resets exactly those fields.
    if (!fields_changed) {
        for (const auto& weak : registered_instances()) {
            auto instance = weak.lock();
            if (!instance) continue;
            for (const auto& [id, new_default] : migration_values) {
                if (new_default.is_null() || !instance->has_field_value(id)) continue;
                if (instance->get_field(id).deref().current_type() != new_default.deref().current_type()) {
                    fields_changed = true;
                    break;
                }
            }
            if (fields_changed) break;
        }
    }

    // A declared-type change (int f -> string f) with an identical id-set is invisible
    // to the comparisons above; replace_field_declared_types flagged it.
    if (declared_types_dirty_) {
        fields_changed = true;
        declared_types_dirty_ = false;
    }

    std::set<uint64_t> old_fields;   // the whole chain's: an instance holds its inherited fields beside the class's own
    if (reparented) {
        old_fields = *reparented;
    } else if (fields_changed) {
        for (const auto& [id, _] : get_all_field_defaults()) {
            old_fields.insert(id);
        }
    }

    uint64_t migrate_method_id = 0;
    if (engine_ref) {
        migrate_method_id = detail::engine_access::symbolize(*engine_ref, "hot_reload_migrate");
    }
    auto migrate_method_it = new_methods.find(migrate_method_id);

    methods_ = new_methods;
    static_methods_ = new_static_methods;
    bump_method_epoch();   // dispatchers replaced: per-site inline caches must refill
    bump_field_shape_epoch();
    if (engine_ref) { detail::engine_access::note_class_shape_change(*engine_ref); }

    recompute_property_getters(new_methods, engine_ref);

    store_field_defaults(new_field_defaults, engine_ref);
    std::unordered_map<uint64_t, script_value> migrated_fields = get_all_field_defaults();   // the whole chain's, this class's reload values over its placeholders
    for (const auto& [id, value] : migration_values) { migrated_fields.insert_or_assign(id, value); }

    // Migrate against a SNAPSHOT of the current instances. A hot_reload_migrate hook can
    // construct new same-class instances, which register mid-migration;
    // iterating the live vector would migrate those too (re-invoking the hook) and loop
    // unboundedly. Snapshotted instances built against the new definition need no
    // migration, so skipping them is also correct.
    std::vector<weaker_ptr<class_instance>> to_migrate = registered_instances();
    for (auto& weak_instance : to_migrate) {
        auto instance = weak_instance.lock();
        if (!instance) {
            continue;
        }
        if (fields_changed) {
            for (const auto& [name, default_value] : migrated_fields) {
                if (!instance->has_field(name)) {
                    instance->set_field(name, default_value.clone());
                }
            }
            // retyped fields convert before the hook (it is checked against the new types); removed ones go after it reads them
            instance->migrate_fields(old_fields, migrated_fields, true);
            if (migrate_method_it != new_methods.end() && !migrate_method_it->second.is_null()) {
                run_migrate_hook(script_value::make_object(name_, instance, engine_ref), migrate_method_it->second, engine_ref, discard_unwinding);
            }
            instance->drop_fields_absent_from(migrated_fields);
            if (engine_) { detail::engine_access::count_hot_reload_field_migration(*engine_); }
        }
        instance->set_class_definition(shared_from_this());
        instance->arm_late_destructor(*this);
    }
    method_metadata_.clear();

    // Propagate to derived classes (their inherited fields changed), snapshotting for the
    // same reason as above, then drop any dead derived refs.
    std::vector<std::weak_ptr<class_definition>> derived_snapshot = derived_classes_;
    for (auto& weak_derived : derived_snapshot) {
        if (auto derived = weak_derived.lock()) {
            derived->update_instances_from_base(migration_values);
        }
    }
    derived_classes_.erase(
        std::remove_if(derived_classes_.begin(), derived_classes_.end(),
            [](const std::weak_ptr<class_definition>& w) { return w.expired(); }),
        derived_classes_.end());
}

inline void class_definition::update_instances_from_base(const std::unordered_map<uint64_t, script_value>& base_migration_values) {
    field_defaults_cache_valid_ = false; ++defaults_version_;
    field_slots_current_ = false;
    std::unordered_map<uint64_t, script_value> all_fields = get_all_field_defaults();
    for (const auto& [id, value] : base_migration_values) {
        if (auto own = field_defaults_.find(id); own == field_defaults_.end() && all_fields.count(id)) { all_fields.insert_or_assign(id, value); }   // the class's own field shadows the base's
    }
    std::set<uint64_t> dummy_old_fields;

    // migrate_fields keeps each instance's runtime value but resets any field whose declared
    // type changed against its new non-null default — so a base reload that retypes an
    // inherited field resets it on derived instances (the base's reload values stand in for its
    // null placeholders), while every other field leaves derived runtime data untouched.
    for (const auto& weak : registered_instances()) {
        if (auto instance = weak.lock()) {
            instance->migrate_fields(dummy_old_fields, all_fields);
            instance->drop_fields_absent_from(all_fields);
            instance->arm_late_destructor(*this);
            if (engine_) { detail::engine_access::count_hot_reload_field_migration(*engine_); }
        }
    }

    for (auto& weak_derived : derived_classes_) {
        if (auto derived = weak_derived.lock()) {
            derived->update_instances_from_base(base_migration_values);
        }
    }
}

inline std::unique_lock<std::mutex> class_definition::registry_lock() const {
    if (engine_ && detail::engine_access::parallel_region_active(*engine_)) { return detail::engine_access::parallel_instance_registry_lock(*engine_); }
    return std::unique_lock<std::mutex>();
}

inline void class_definition::register_live_instance(class_instance* live) {
    if (engine_ && detail::engine_access::parallel_region_active(*engine_)) [[unlikely]] {
        register_live_instance_in_region(live);
        return;
    }
    register_instance_after_region(weaker_ptr<class_instance>(), nullptr, live);
}

void class_definition::register_live_instance_in_region(class_instance* live) { register_instance(live->weak_from_this(), live); }

inline void class_definition::register_instance(weaker_ptr<class_instance> instance, class_instance* live) {
    if (engine_ && detail::engine_access::parallel_region_active(*engine_)) {
        // A chunk parks the registration (no lock; the join registers survivors in element order)
        if (detail::engine_access::park_parallel_registration(*engine_, instance)) { return; }
    }
    auto lock = registry_lock();
    register_instance_after_region(std::move(instance), nullptr, live);
}

inline void class_definition::register_instance_after_region(weaker_ptr<class_instance> instance, const detail::fault_injection* faults, class_instance* live_hint) {
    if (faults) { faults->checkpoint("registry.instance_append", registry_count_); }
    class_instance* live = live_hint;
    if (!live) {
        if (auto owner = instance.lock()) { live = owner.get(); }   // the caller owns it
    }
    if (!live || live->registered_.load(std::memory_order_relaxed)) { return; }
    live->registry_prev_ = registry_tail_;
    live->registry_next_ = nullptr;
    if (registry_tail_) { registry_tail_->registry_next_ = live; } else { registry_head_ = live; }
    registry_tail_ = live;
    ++registry_count_;
    live->registered_.store(true, std::memory_order_relaxed);
}

inline void class_definition::unregister_instance(class_instance* instance) {
    if (!instance->registered_.load(std::memory_order_relaxed)) { return; }
    auto lock = registry_lock();
    if (!instance->registered_.load(std::memory_order_relaxed)) { return; }
    if (instance->registry_prev_) { instance->registry_prev_->registry_next_ = instance->registry_next_; } else { registry_head_ = instance->registry_next_; }
    if (instance->registry_next_) { instance->registry_next_->registry_prev_ = instance->registry_prev_; } else { registry_tail_ = instance->registry_prev_; }
    instance->registry_prev_ = instance->registry_next_ = nullptr;
    --registry_count_;
    instance->registered_.store(false, std::memory_order_relaxed);
}

inline void class_definition::recompute_property_getters(const std::unordered_map<uint64_t, script_value>& new_methods, engine* engine_ref) {
    has_property_getters_ = false;
    for (const auto& parent : parent_classes_) {
        if (parent && parent->has_property_getters()) {
            has_property_getters_ = true;
            break;
        }
    }
    if (!has_property_getters_ && cpp_base_class_ && cpp_base_class_->has_property_getters()) {
        has_property_getters_ = true;
    }
    if (!has_property_getters_ && engine_ref) {
        for (const auto& [method_id, _] : new_methods) {
            std::string_view name = detail::engine_access::get_symbolizer(*engine_ref)->get_string(method_id);
            if (name.size() > 5 && name.substr(0, 5) == "_get_") {
                has_property_getters_ = true;
                break;
            }
        }
    }
}

inline void class_definition::store_field_defaults(const std::unordered_map<uint64_t, script_value>& new_field_defaults, engine* engine_ref) {
    field_defaults_.clear();
    field_defaults_cache_valid_ = false; ++defaults_version_;
    field_slots_current_ = false;
    bump_method_epoch();
    for (const auto& [id, value] : new_field_defaults) {
        field_defaults_[id] = value;
    }
}
inline checked_result<script_value> class_definition::cpp_method_dispatch::operator()(const std::vector<script_value>& raw_args) const {
    engine* const eng = cls->get_engine();
    if (raw_args.empty() || !eng) {
        return checked_result<script_value>(make_error_code(runtime_error_code::class_not_found),
                                            "Class definition no longer exists");
    }
    try {
    // C++ boundary: escape-boxed locals arrive as CELL references (script
    // parameter binding derefs them; the bound-method mint path does not) —
    // normalize so overload scoring and converters see hoisted-temp values.
    std::vector<script_value> scratch;
    const std::vector<script_value>& args = detail::deref_builtin_args(raw_args, scratch);

    size_t script_arg_count = args.size() - 1;

    auto it = overloads->find(script_arg_count);
    if (it == overloads->end() || it->second.empty()) {
        return detail::overload_miss(make_error_code(runtime_error_code::argument_count_mismatch),
                                     name_id, args.data() + 1, script_arg_count, eng);
    }

    auto& entries = it->second;
    if (entries.size() == 1) {
        auto result = entries.front().func(args);
        if (!result) [[unlikely]] {
            if (auto miss = single_overload_miss(args)) { return *miss; }
        }
        return result;
    }

    const cpp_overload_entry* best = nullptr;
    int best_cost = 0;
    for (const auto& e : entries) {
        const int cost = detail::engine_access::cpp_signature_cost(*eng, args.data() + 1, e.param_types);
        if (cost >= 0 && (!best || cost < best_cost)) { best = &e; best_cost = cost; }
    }
    if (!best) {
        return detail::overload_miss(make_error_code(runtime_error_code::argument_count_mismatch),
                                     name_id, args.data() + 1, script_arg_count, eng);
    }
    return best->func(args);
    } catch (const std::exception& e) {
        std::vector<script_value> scratch;
        if (auto miss = single_overload_miss(detail::deref_builtin_args(raw_args, scratch))) { return *miss; }
        return host_exception_result(eng, e);
    }
}

// A lone overload that failed on arguments its C++ signature does not take: the overload miss names them, in place of the
// argument conversion's own text
inline std::optional<checked_result<script_value>> class_definition::cpp_method_dispatch::single_overload_miss(const std::vector<script_value>& args) const {
    if (args.empty()) { return std::nullopt; }
    auto it = overloads->find(args.size() - 1);
    if (it == overloads->end() || it->second.size() != 1 || it->second.front().param_types.empty()) { return std::nullopt; }
    engine* eng = cls->get_engine();
    if (!eng || detail::engine_access::cpp_signature_cost(*eng, args.data() + 1, it->second.front().param_types) >= 0) { return std::nullopt; }
    return checked_result<script_value>(detail::overload_miss(make_error_code(runtime_error_code::argument_count_mismatch), name_id, args.data() + 1, args.size() - 1, eng));
}

inline uint8_t class_definition::cpp_method_dispatch::argument_binding(size_t i, size_t argc) const {
    uint8_t bits = 0;
    for (const auto& [arity, entries] : *overloads) {
        if (argc != SIZE_MAX && arity != argc) { continue; }
        for (const auto& e : entries) {
            const auto* w = e.func.target<host_writeback_function>();
            bits |= w && i < 64 && ((w->mask >> i) & 1) ? 4 : 8;
        }
    }
    return bits ? bits : 8;
}


inline script_value& class_instance::materialize_field(uint64_t id, script_value&& v) {
    if (class_def_) {
        const uint32_t slot = class_def_->field_slot(id);
        if (slot != class_definition::k_no_field_slot) {
            if (slot < flat_fields_.size() && flat_fields_.implicit()) {
                script_value* c = flat_fields_.cell(slot);
                *c = std::move(v);
                return *c;
            }
            if (slot >= flat_fields_.size()) {
                flat_fields_.resize((std::max)(static_cast<size_t>(class_def_->field_slot_count()),
                                               static_cast<size_t>(slot) + 1));
            }
            auto& cell = flat_fields_[slot];
            if (!cell) {
                cell = make_cell(std::move(v));
            } else {
                *cell = std::move(v);
            }
            return *cell;
        }
    }
    if (!overflow_fields()) {
        extras().overflow_fields = std::make_unique<std::unordered_map<uint64_t, script_value>>();
    }
    return overflow_fields()->insert_or_assign(id, std::move(v)).first->second;
}

inline void class_instance::seed_fields_from_plan(const std::vector<std::pair<uint32_t, script_value>>& plan, uint32_t slot_count, bool detach) {
    seeded_from_plan_ = true;
    if (flat_fields_.implicit() && flat_fields_.size() >= slot_count) {
        for (const auto& [slot, value] : plan) {
            script_value* c = flat_fields_.cell(slot);
            if (detach) [[unlikely]] { *c = detail::region_field_default(value, owner_); } else { *c = value; }
        }
        return;
    }
    if (flat_fields_.size() < slot_count) { flat_fields_.resize(slot_count); }
    for (const auto& [slot, value] : plan) {
        auto& cell = flat_fields_[slot];
        if (detach) [[unlikely]] {
            if (!cell) { cell = make_cell(detail::region_field_default(value, owner_)); } else { *cell = detail::region_field_default(value, owner_); }
            continue;
        }
        if (!cell) { cell = make_cell(script_value(value)); } else { *cell = value; }
    }
}

inline void class_instance::set_field_unchecked(uint64_t id, const script_value& value) {
    if (script_value* existing = find_field_value(id); existing && existing->is_reference()) {
        existing->deref() = value;
        return;
    }
    materialize_field(id, script_value(value));
}

inline void class_instance::rehome_overflow_fields() {
    // Declared ids written before the definition arrived (deserialization constructs,
    // then classifies) move into their slots; runtime-only ids stay in overflow.
    if (!class_def_ || !overflow_fields()) {
        return;
    }
    for (auto it = overflow_fields()->begin(); it != overflow_fields()->end();) {
        if (class_def_->field_slot(it->first) != class_definition::k_no_field_slot) {
            materialize_field(it->first, std::move(it->second));
            it = overflow_fields()->erase(it);
        } else {
            ++it;
        }
    }
    if (overflow_fields()->empty()) {
        extras_->overflow_fields.reset();
    }
}

inline void class_instance::set_class_definition(class_definition* class_def) {
    class_def_ = class_def;
    if (class_def_) {
        set_type(class_def_->interned_name(), class_def_->get_type_id());
        reserve_cell_arena(class_def_->field_slot_count());
    }
    rehome_overflow_fields();
}

inline void class_instance::arm_late_destructor(const class_definition& cls) {
    if (!destructor_armed_ && owner_ && cls.is_script_class() && cls.chain_declares_destructor(*owner_)) {
        destructor_armed_ = late_destructor_ = true;
    }
}

inline void class_instance::reclass(const std::shared_ptr<class_definition>& class_def, bool drop_undeclared) {
    if (!class_def || class_def.get() == class_def_) { return; }
    arm_late_destructor(*class_def);
    std::vector<std::pair<uint64_t, field_cell>> cells;
    if (class_def_) {
        cells.reserve(flat_fields_.size());
        for (size_t slot = 0; slot < flat_fields_.size(); ++slot) {
            if (flat_fields_[slot]) {
                cells.emplace_back(class_def_->slot_field(static_cast<uint32_t>(slot)), std::move(flat_fields_[slot]));
            }
        }
        class_def_->unregister_instance(this);
    }
    flat_fields_.clear();
    class_def_ = class_def.get();
    set_type(class_def->interned_name(), class_def->get_type_id());
    flat_fields_.resize(class_def_->field_slot_count());
    for (auto& [id, cell] : cells) {
        const uint32_t slot = class_def_->field_slot(id);
        if (slot != class_definition::k_no_field_slot) {
            if (slot >= flat_fields_.size()) { flat_fields_.resize(static_cast<size_t>(slot) + 1); }
            flat_fields_[slot] = std::move(cell);
        } else if (drop_undeclared) {
            *cell = script_value::make_invalid(owner_);
            extras().retired_cells.emplace_back(id, std::move(cell));
        } else if (cell->raw_storage_index() != script_value::TYPEID_INVALID) {
            if (!overflow_fields()) {
                extras().overflow_fields = std::make_unique<std::unordered_map<uint64_t, script_value>>();
            }
            overflow_fields()->insert_or_assign(id, std::move(*cell));
        }
    }
    if (extras_) {   // a field the object regains takes back its retired cell (repeated re-classes allocate nothing)
        auto& retired = extras_->retired_cells;
        for (auto it = retired.begin(); it != retired.end();) {
            const uint32_t slot = class_def_->field_slot(it->first);
            if (slot != class_definition::k_no_field_slot && slot < flat_fields_.size() && !flat_fields_[slot]) {
                flat_fields_[slot] = std::move(it->second);
                it = retired.erase(it);
            } else {
                ++it;
            }
        }
    }
    rehome_overflow_fields();
    class_def_->register_live_instance(this);
}

template <typename F>
inline void class_instance::for_each_field(F&& fn) {
    if (class_def_) {
        // a base answering for the object during its constructor or destructor knows its own slots only
        const size_t n = std::min(flat_fields_.size(), static_cast<size_t>(class_def_->field_slot_count()));
        for (size_t slot = 0; slot < n; ++slot) {
            if (script_value* v = flat_fields_.present_cell(slot)) {
                fn(class_def_->slot_field(static_cast<uint32_t>(slot)), *v);
            }
        }
    }
    if (overflow_fields()) {
        for (auto& [id, value] : *overflow_fields()) {
            fn(id, value);
        }
    }
}

template <typename F>
inline void class_instance::for_each_field(F&& fn) const {
    if (class_def_) {
        // a base answering for the object during its constructor or destructor knows its own slots only
        const size_t n = std::min(flat_fields_.size(), static_cast<size_t>(class_def_->field_slot_count()));
        for (size_t slot = 0; slot < n; ++slot) {
            if (const script_value* v = flat_fields_.present_cell(slot)) {
                fn(class_def_->slot_field(static_cast<uint32_t>(slot)), *v);
            }
        }
    }
    if (overflow_fields()) {
        for (const auto& [id, value] : *overflow_fields()) {
            fn(id, value);
        }
    }
}

inline size_t class_instance::field_value_count() const {
    size_t n = 0;
    for_each_field([&](uint64_t, const script_value&) { ++n; });
    return n;
}

inline void class_instance::relayout_fields(const class_definition& from, const class_definition& to) {
    const size_t held = flat_fields_.size();
    const size_t from_slots = from.field_slot_count();
    const size_t to_slots = to.field_slot_count();
    std::vector<field_cell> result(to_slots);
    std::vector<field_cell> foreign;
    for (size_t slot = 0; slot < held; ++slot) {
        const uint32_t target = slot < from_slots ? to.field_slot(from.slot_field(static_cast<uint32_t>(slot))) : class_definition::k_no_field_slot;
        if (target != class_definition::k_no_field_slot && target < to_slots) {
            result[target] = std::move(flat_fields_[slot]);
        } else {
            foreign.push_back(std::move(flat_fields_[slot]));
        }
    }
    size_t next = 0;
    for (size_t slot = 0; slot < to_slots && next < foreign.size(); ++slot) {
        if (from.field_slot(to.slot_field(static_cast<uint32_t>(slot))) == class_definition::k_no_field_slot) {
            result[slot] = std::move(foreign[next++]);
        }
    }
    for (; next < foreign.size(); ++next) { result.push_back(std::move(foreign[next])); }
    flat_fields_.resize(result.size());
    for (size_t slot = 0; slot < result.size(); ++slot) { flat_fields_[slot] = std::move(result[slot]); }
}

inline void class_instance::copy_fields_from(const class_instance& other) {
    other.for_each_field([&](uint64_t id, const script_value& value) {
        materialize_field(id, value.clone_element());
    });
    if (const script_value* held = other.native()) { extras().native = held->clone_element(); }
}

namespace detail {

// class_instance::dispatch_as for a scope: the object answers as `level` until the scope ends. Code compiled against the level reads
// its slots, so an instance whose layout does not extend the level's (a base that gained fields on reload, a second base) is laid
// out as the level for the span.
class dispatch_level_scope {
public:
    dispatch_level_scope(class_instance& a_instance, class_definition* a_level, class_instance::cdtor_phase a_phase) :
        instance(a_instance),
        restore(a_instance.get_class_definition()),
        restorePhase(a_instance.dispatch_phase()) {
        if (a_level && restore && a_level != restore && a_level->is_script_class()) {
            level = a_level;
            relaid = !a_level->slot_prefix_of(*restore);
            if (relaid) { instance.relayout_fields(*restore, *a_level); }
        }
        instance.dispatch_as(level ? level : restore, a_phase);
    }
    ~dispatch_level_scope() {
        if (relaid) { instance.relayout_fields(*level, *restore); }
        instance.dispatch_as(restore, restorePhase);
    }
    dispatch_level_scope(const dispatch_level_scope&) = delete;
    dispatch_level_scope& operator=(const dispatch_level_scope&) = delete;
private:
    class_instance& instance;
    class_definition* restore;
    class_instance::cdtor_phase restorePhase;
    class_definition* level {nullptr};
    bool relaid {false};
};

// The ONE conversion table for every typed slot (docs/site/embedding/06-static-checking.html "One conversion table"):
// declaration/initializer, `=`, compound store-back, the cast<T> value form, by-value
// parameter binding, typed return and field store all classify the source by STORAGE
// (never by its tag - a shared_ptr<C> handle IS a C) and run these rows. This is the
// script-free half; rows that must run script (a converting ctor, to_X(), object
// to_bool()) come back as needs_* verdicts for detail::convert_for_typed_slot
// (typed_slot.hpp) to resolve. The C++ set_field API and hot-reload migration stop
// here by policy. Identity never touches the value, so a parameter site keeps its
// arg / derefed-arg return shapes; converted payloads come back UNTAGGED (each site
// tags as it always did). Each site owns its rejection texts.
struct slot_conversion {
    enum verdict : uint8_t {
        identity,            // the slot holds the value as it is
        identity_exact,      // identity by an exact class-name tag (parameter sites return the derefed arg)
        converted,           // `value` holds the converted payload
        reject,              // no row (non-class target; primitive into a class without a viable ctor)
        reject_class,        // class into an unrelated class without a viable ctor
        container_mismatch,  // element boundary failed: offending / expected filled
        handle_target,       // non-null into shared_ptr<T> / weak_ptr<T>: the site's own rows (outside the table)
        needs_ctor,          // class target: only a true 1-param ctor can answer (script)
        needs_to_x,          // primitive target from an object: to_int/to_float/to_char/to_string (script)
        needs_to_bool        // bool target from an object: to_bool() or truthy (script)
    };
    verdict v = reject;
    std::optional<script_value> value;
    std::unique_ptr<std::pair<std::string, std::string>> mismatch;   // container_mismatch only: offending / expected

    bool needs_script() const { return v == needs_ctor || v == needs_to_x || v == needs_to_bool; }
    static slot_conversion of(verdict verdict_) { slot_conversion out; out.v = verdict_; return out; }
    static slot_conversion converted_to(script_value v_) { slot_conversion out; out.v = converted; out.value = std::move(v_); return out; }
};

// The class behind an object-storage value: class_instance wrappers resolve through the
// instance (a plain wrapper of an extended object re-points to its script layer, exactly
// like the backends' resolve_member_target), raw holders through the engine registry.
struct slot_class_view {
    strong_ptr<class_instance> instance;
    std::shared_ptr<class_definition> engine_def;
    class_definition* def = nullptr;
};

inline slot_class_view resolve_slot_class(const script_value& v, engine* eng) {
    slot_class_view view;
    auto holder = v.get_object_holder();
    if (!holder) {
        return view;
    }
    if (holder->is_instance()) {
        view.instance = holder->instance_handle();
        if (view.instance) {
            view.def = view.instance->get_class_definition();
            if (view.def && view.def->wrapped_object_has_layer(*view.instance)) {
                if (auto hooks = view.def->slot_hooks()) {
                    auto layer = hooks->read(view.instance->cpp_object_pointer());
                    if (layer && layer != view.instance && layer->get_engine() == eng) {
                        view.instance = std::move(layer);
                        view.def = view.instance->get_class_definition();
                    }
                }
            }
        }
    } else if (eng) {
        if (holder->type_id() != UINT64_MAX) { view.engine_def = engine_access::get_class_definition(*eng, holder->type_id()); }
        else { view.engine_def = engine_access::get_class_definition(*eng, std::string(holder->type_name())); }
        view.def = view.engine_def.get();
    }
    return view;
}

// Does an object-storage value already satisfy a `target`-classed slot? Empty handles
// answer by their tag's class; live objects by their class (subtypes included), the
// holder's registered name, the tag, or - unknown host objects without a tag - by the
// permissive fallback the field-reference gate keeps.
inline bool class_slot_admits(const slot_class_view& view, const script_value& v, const type_info& target, engine* eng) {
    const std::string& want = target.type_name;
    if (want.empty()) {
        return true;
    }
    const type_info* tag = v.get_type_info().get();
    const bool tagged = tag && tag->base_type != script_value_type::jai_any_type;
    if (tagged && tag->type_name == want) {
        return true;
    }
    auto holder = v.get_object_holder();
    if (!holder) {
        if (!tagged || tag->type_name.empty()) {
            return true;   // an untagged empty handle is null-like
        }
        auto tag_def = engine_access::get_class_definition(eng, tag->type_name);
        return tag_def && tag_def->is_subtype_of(want);
    }
    if (view.def && view.def->is_subtype_of(want)) {
        return true;
    }
    if (holder->type_name() == want) {
        return true;
    }
    return !view.def && !tagged;
}

// A stored member an assignment expression can hand back: a field, a getter (`_get_x`, user or
// synthesized) or a method of that name somewhere in the chain. A setter-only member has no
// stored value to read, so its assignment yields the right-hand side. Host objects and unknowns
// answer yes (the read path decides).
inline bool member_reads_back(const script_value& v, uint64_t member_id, uint64_t getter_id, engine* eng) {
    auto view = resolve_slot_class(v, eng);
    if (!view.def) { return true; }
    std::vector<const class_definition*> stack{ view.def };
    int budget = 64;
    while (!stack.empty() && budget-- > 0) {
        const class_definition* cd = stack.back();
        stack.pop_back();
        if (cd->field_slot(member_id) != class_definition::k_no_field_slot || cd->has_method(getter_id) || cd->has_method(member_id)) { return true; }
        for (const auto& parent : cd->get_parent_classes()) { if (parent) { stack.push_back(parent.get()); } }
    }
    return false;
}

// weak_ptr<T> slot taking a shared_ptr handle (C++: weak = shared): the pointee class must be
// T or derive from it, then the handle downgrades to a weak one. false = not this row.
inline bool weak_slot_takes_shared(const script_value& value, const type_info_ptr& target_type, engine* eng) {
    if (!target_type || target_type->base_type != script_value_type::jai_weak_ptr_type ||
        value.type() != script_value_type::jai_shared_ptr_type) {
        return false;
    }
    auto expected_type = target_type->element_type();
    auto value_type_info = value.get_type_info();
    if (!expected_type || !value_type_info || expected_type->base_type == script_value_type::jai_any_type) {
        return true;
    }
    const std::string& expected_class = expected_type->type_name;
    const std::string& actual_class = value_type_info->element_type() ? value_type_info->element_type()->type_name
                                                                       : value_type_info->type_name;
    if (expected_class == actual_class) { return true; }
    auto actual_def = engine_access::get_class_definition(eng, actual_class);
    return actual_def && actual_def->is_subtype_of(expected_class);
}

#ifdef JAISCRIPT_VM_COVERAGE
inline checked_result<slot_conversion> convert_for_typed_slot_core_uncounted(const script_value& value_in, const type_info_ptr& target_type, engine* eng);
// the copy census's twin: the table's outcome at its caller (flags: the verdict + 1; 0x80 a failure; 0x40 an untyped target)
inline checked_result<slot_conversion> convert_for_typed_slot_core(const script_value& value_in, const type_info_ptr& target_type, engine* eng) {
    auto result = convert_for_typed_slot_core_uncounted(value_in, target_type, eng);
    const uint32_t untyped = (!target_type || target_type->base_type == script_value_type::jai_any_type) ? 0x40u : 0u;
    census::hit(census::site::typed_slot_convert, (result ? static_cast<uint32_t>(result.value().v) + 1 : 0x80u) | untyped);
    return result;
}
inline checked_result<slot_conversion> convert_for_typed_slot_core_uncounted(const script_value& value_in, const type_info_ptr& target_type, engine* eng) {
#else
inline checked_result<slot_conversion> convert_for_typed_slot_core(const script_value& value_in, const type_info_ptr& target_type, engine* eng) {
#endif
    using verdict = slot_conversion::verdict;
    if (!target_type || target_type->base_type == script_value_type::jai_any_type) {
        return slot_conversion::of(verdict::identity);
    }
    const script_value& v = value_in.is_reference() ? value_in.deref() : value_in;
    const size_t ri = v.raw_storage_index();
    const auto target = target_type->base_type;
    // bound primitives classify by their semantic type; every other kind by storage
    const script_value_type src = v.current_type();
    const bool primitive_src = ri == script_value::TYPEID_INT || ri == script_value::TYPEID_FLOAT ||
                               ri == script_value::TYPEID_STRING || ri == script_value::TYPEID_BOOL ||
                               ri == script_value::TYPEID_CHAR ||
                               (ri == script_value::TYPEID_CPP_BOUND && src != script_value_type::jai_null_type);
    const bool object_src = ri == script_value::TYPEID_OBJECT ||
                            (ri == script_value::TYPEID_CPP_BOUND && src == script_value_type::jai_null_type);

    auto typed_null = [&]() {
        script_value null_val(std::monostate{}, eng);
        null_val.set_type_info(target_type);
        return slot_conversion::converted_to(std::move(null_val));
    };
    auto boundary = [&]() -> checked_result<slot_conversion> {
        if (v.get_type_info().get() == target_type.get()) {
            return slot_conversion::of(verdict::identity);   // the declared representation already
        }
        auto outcome = enforce_container_boundary(v, target_type, eng);
        if (outcome.error) {
            return checked_result<slot_conversion>(outcome.error->ec, outcome.error->static_msg, outcome.error->symbol_id, outcome.error->symbol_id2);
        }
        if (!outcome.value) {
            slot_conversion out = slot_conversion::of(verdict::container_mismatch);
            out.mismatch = std::make_unique<std::pair<std::string, std::string>>(std::move(outcome.offending), std::move(outcome.expected));
            return out;
        }
        return slot_conversion::converted_to(std::move(*outcome.value));
    };

    if (target_type->is_enum) [[unlikely]] {
        // an enum slot holds its own enum's values: one stamped with it stays, an unstamped int that is one of them takes
        // the stamp (a var or a host hands those over), another enum's value and any other int are refused
        if (ri != script_value::TYPEID_INT) return slot_conversion::of(verdict::reject);
        const type_info* tag = v.type_info_raw();
        if (tag == target_type->enum_value_type) return slot_conversion::of(verdict::identity);
        if ((tag && tag->is_enum) || !target_type->enum_holds(v.unchecked_as_int())) return slot_conversion::of(verdict::reject);
        script_value stamped(v.unchecked_as_int(), eng);
        stamped.set_type_info(target_type->enum_value_type);
        return slot_conversion::converted_to(std::move(stamped));
    }
    switch (target) {
        case script_value_type::jai_int_type:
            if (src == script_value_type::jai_int_type && primitive_src) {
                // an enum's value stores as the plain int it is
                if (const type_info* tag = v.type_info_raw(); ri == script_value::TYPEID_INT && tag && tag->is_enum) [[unlikely]] {
                    return slot_conversion::converted_to(script_value(v.unchecked_as_int(), eng));
                }
                return slot_conversion::of(verdict::identity);
            }
            if (src == script_value_type::jai_bool_type && primitive_src) return slot_conversion::converted_to(script_value(static_cast<script_int>(v.unchecked_as_bool() ? 1 : 0), eng));
            if (object_src) return slot_conversion::of(verdict::needs_to_x);
            return slot_conversion::of(verdict::reject);
        case script_value_type::jai_float_type:
            if (src == script_value_type::jai_float_type && primitive_src) return slot_conversion::of(verdict::identity);
            if (src == script_value_type::jai_int_type && primitive_src) return slot_conversion::converted_to(script_value(static_cast<script_float>(v.unchecked_as_int()), eng));
            if (src == script_value_type::jai_bool_type && primitive_src) return slot_conversion::converted_to(script_value(static_cast<script_float>(v.unchecked_as_bool() ? 1.0 : 0.0), eng));
            if (object_src) return slot_conversion::of(verdict::needs_to_x);
            return slot_conversion::of(verdict::reject);
        case script_value_type::jai_char_type:
            if (src == script_value_type::jai_char_type && primitive_src) return slot_conversion::of(verdict::identity);
            if (src == script_value_type::jai_string_type && primitive_src) {
                auto converted = ri == script_value::TYPEID_STRING ? char_of_text(v.unchecked_as_string()) : char_of_text(v.to_string());
                if (!converted) { return converted.error_value(); }
                return slot_conversion::converted_to(script_value(converted.value(), eng));
            }
            if (object_src) return slot_conversion::of(verdict::needs_to_x);
            return slot_conversion::of(verdict::reject);
        case script_value_type::jai_string_type:
            if (src == script_value_type::jai_string_type && primitive_src) return slot_conversion::of(verdict::identity);
            if (ri == script_value::TYPEID_UTF8_STRING) return slot_conversion::converted_to(script_value(v.to_string(), eng));
            if (src == script_value_type::jai_char_type && primitive_src) return slot_conversion::converted_to(script_value(std::string(1, v.unchecked_as_char()), eng));
            if (object_src) return slot_conversion::of(verdict::needs_to_x);
            return slot_conversion::of(verdict::reject);
        case script_value_type::jai_utf8_string_type:
            if (ri == script_value::TYPEID_UTF8_STRING) return slot_conversion::of(verdict::identity);
            if (src == script_value_type::jai_string_type && primitive_src) return slot_conversion::converted_to(script_value::make_utf8(v.to_string(), eng));
            if (src == script_value_type::jai_char_type && primitive_src) return slot_conversion::converted_to(script_value::make_utf8(std::string(1, v.unchecked_as_char()), eng));
            if (object_src) return slot_conversion::of(verdict::needs_to_x);
            return slot_conversion::of(verdict::reject);
        case script_value_type::jai_bool_type: {
            // is_truthy minus its object tail
            if (src == script_value_type::jai_bool_type && primitive_src) return slot_conversion::of(verdict::identity);
            if (object_src) return slot_conversion::of(verdict::needs_to_bool);
            // a bool storage that is not a primitive bool source, and a reference left after one deref, stay truthy
            const bool truthy = ri == script_value::TYPEID_BOOL || ri == script_value::TYPEID_REFERENCE ||
                                truthy_of(v, [](const script_value&) {return true;});
            return slot_conversion::converted_to(script_value(truthy, eng));
        }
        case script_value_type::jai_array_type:
            if (ri == script_value::TYPEID_ARRAY) return boundary();
            return slot_conversion::of(verdict::reject);
        case script_value_type::jai_map_type:
            if (ri == script_value::TYPEID_MAP) return boundary();
            return slot_conversion::of(verdict::reject);
        case script_value_type::jai_object_type: {
            if (object_src) {
                const type_info* tag = v.get_type_info().get();
                if (tag && !tag->type_name.empty() && !target_type->type_name.empty() && tag->type_name == target_type->type_name) {
                    return slot_conversion::of(verdict::identity_exact);
                }
                auto view = resolve_slot_class(v, eng);
                if (class_slot_admits(view, v, *target_type, eng)) {
                    return slot_conversion::of(verdict::identity);
                }
                return slot_conversion::of(v.get_object_holder() ? verdict::needs_ctor : verdict::reject_class);
            }
            if (primitive_src && !target_type->type_name.empty()) return slot_conversion::of(verdict::needs_ctor);
            return slot_conversion::of(verdict::reject);
        }
        case script_value_type::jai_shared_ptr_type:
        case script_value_type::jai_weak_ptr_type:
            if (ri == script_value::TYPEID_NULL) return typed_null();
            return slot_conversion::of(verdict::handle_target);
        case script_value_type::jai_function_type:
            // a callable slot resets to null and takes a callable again (a std::function's rule)
            if (ri == script_value::TYPEID_NULL) return typed_null();
            return slot_conversion::of(v.type() == target ? verdict::identity : verdict::reject);
        case script_value_type::jai_vec2_type:
        case script_value_type::jai_vec3_type:
        case script_value_type::jai_vec4_type:
        case script_value_type::jai_color_type:
        case script_value_type::jai_ivec2_type:
        case script_value_type::jai_ivec3_type: {
            // the same kind; an ivec widens into the vec of its dimensions (int32 to binary32 in one rounding); anything else,
            // a vec into an ivec too (cast<math::ivecN> truncates), rejects with the site's text
            const script_value_type have = detail::math::kind_of(v);
            if (have == target) return slot_conversion::of(verdict::identity);
            if (!detail::math::converts(have, target) || detail::math::narrows(have, target)) return slot_conversion::of(verdict::reject);
            auto converted = detail::math::convert(v, target, eng);
            if (!converted) { return converted.error_value(); }
            return slot_conversion::converted_to(std::move(converted.value()));
        }
        default:
            return slot_conversion::of(v.type() == target ? verdict::identity : verdict::reject);
    }
}

// The script rows (needs_* verdicts): a TRUE one-parameter ctor whose declared parameter
// accepts the source directly (no chaining), to_int/to_float/to_char/to_string, to_bool().
// Declared here so the field kernel below can reach it from class_definition.hpp; the body
// lives in typed_slot.hpp (defined out of line in environment.cpp).
checked_result<slot_conversion> convert_typed_slot_script_rows(slot_conversion pending, const script_value& value, const type_info_ptr& target_type, engine* eng);
// A class slot assigned a value its class converts from (a converting constructor): the converted temporary copy-assigns into the
// slot's object, then dies, as C++ assigns T into an L. consumable: the store's own temporary, the constructor's parameter.
// nullopt when no converting constructor applies (environment.cpp)
std::optional<checked_result<void>> converting_copy_assign(const script_value& current, const script_value& value, engine* eng,
                                                           const type_info* declared, script_value* consumable);

// The pointee class a shared_ptr<T> slot judges a source by: a typed handle's static pointee (a
// C++ shared_ptr<U> converts to shared_ptr<T> only upward; a downcast needs cast<shared_ptr<T>>), else - a var-held handle, a plain
// object - the class the object really has. Null when neither is known (opaque host flows stay lenient).
inline const std::string* shared_handle_judged_class_name(const script_value& v, engine* eng) {
    const type_info* tag = v.get_type_info().get();
    if (tag && tag->base_type == script_value_type::jai_shared_ptr_type && !tag->dynamic_pointee) {
        const type_info* pointee = tag->element_type().get();
        if (pointee && !pointee->type_name.empty() && pointee->base_type != script_value_type::jai_any_type) {
            return &pointee->type_name;
        }
    }
    auto view = resolve_slot_class(v, eng);
    return view.instance && view.def ? &view.def->get_name() : nullptr;
}
inline std::string shared_handle_judged_class(const script_value& v, engine* eng) {
    const std::string* name = shared_handle_judged_class_name(v, eng);
    return name ? *name : std::string();
}

// Does a shared_ptr<T> slot take this handle or object: its judged class is T or a subclass. A var-declared holder
// (dynamic pointee) rebinds to anything, and an unresolvable class stays lenient.
inline bool shared_handle_admits(const script_value& v, const type_info_ptr& declared, engine* eng) {
    const type_info* tag = v.get_type_info().get();
    if (tag == declared.get() || declared->dynamic_pointee) {
        return true;   // the slot's own static type
    }
    const type_info* expected = declared->element_type().get();
    if (!expected || expected->type_name.empty() || expected->base_type == script_value_type::jai_any_type) {
        return true;
    }
    if (tag && tag->base_type == script_value_type::jai_shared_ptr_type && !tag->dynamic_pointee && tag->element_type().get() == expected) {
        return true;   // the same static pointee
    }
    const bool static_pointee = tag && tag->base_type == script_value_type::jai_shared_ptr_type && !tag->dynamic_pointee && tag->element_type() &&
                                !tag->element_type()->type_name.empty() && tag->element_type()->base_type != script_value_type::jai_any_type;
    if (!static_pointee) {
        // the class the object really has, judged by its own definition (no registry lookup by name)
        auto view = resolve_slot_class(v, eng);
        if (!view.instance || !view.def || view.def->get_name() == expected->type_name) { return true; }
        return view.def->is_subtype_of(expected->type_name);
    }
    const std::string* judged = shared_handle_judged_class_name(v, eng);
    if (!judged || *judged == expected->type_name) {
        return true;
    }
    auto judged_def = engine_access::get_class_definition(eng, *judged);
    return !judged_def || judged_def->is_subtype_of(expected->type_name);
}

// A typed shared_ptr<T> variable keeps its own static type whatever handle it takes (a var-declared holder keeps the source's)
inline void keep_handle_slot_type(script_value& value, const type_info_ptr& slot_type) {
    if (slot_type && slot_type->base_type == script_value_type::jai_shared_ptr_type && !slot_type->dynamic_pointee &&
        value.get_type_info() && value.get_type_info()->base_type == script_value_type::jai_shared_ptr_type) {
        value.set_type_info(slot_type);
    } else if (slot_type && slot_type->base_type == script_value_type::jai_weak_ptr_type && slot_type->element_type() && value.is_weak_ptr()) {
        value.set_type_info(slot_type);
    }
}

// The handle rows' common case without the table: a statically typed shared_ptr handle bound into a shared_ptr<T> slot whose
// pointee it already names takes the slot's static type in place, exactly as handle admission plus keep_handle_slot_type
// leave it; false = the full table decides
inline bool shared_handle_binds_in_place(script_value& value, const type_info_ptr& slot_type) {
    if (value.raw_storage_index() != script_value::TYPEID_OBJECT || !slot_type || slot_type->base_type != script_value_type::jai_shared_ptr_type) {
        return false;
    }
    const type_info* tag = value.get_type_info().get();
    if (tag == slot_type.get()) {
        return true;
    }
    if (!tag || tag->base_type != script_value_type::jai_shared_ptr_type || tag->dynamic_pointee || slot_type->dynamic_pointee) {
        return false;
    }
    const type_info* expected = slot_type->element_type().get();
    const type_info* pointee = tag->element_type().get();
    const bool admitted = !expected || expected->type_name.empty() || expected->base_type == script_value_type::jai_any_type || pointee == expected ||
        (pointee && !pointee->type_name.empty() && pointee->base_type != script_value_type::jai_any_type && pointee->type_name == expected->type_name);
    if (!admitted) {
        return false;
    }
    value.set_type_info(slot_type);
    return true;
}

// Field-store rows for shared_ptr<T> / weak_ptr<T> declared fields (the handle_target
// verdict): a same-kind tag stores as-is (its own tag) when a shared handle holds T or a
// subclass, a live object or handle of T or a subclass stores into a shared_ptr<T> field,
// a dynamic holder is ground truth there.
inline std::optional<script_value> field_handle_rows(const script_value& v, const type_info_ptr& declared, engine* eng) {
    const auto target = declared->base_type;
    if (v.type() == target) {
        if (target == script_value_type::jai_shared_ptr_type) {
            if (!shared_handle_admits(v, declared, eng)) {
                return std::nullopt;
            }
            script_value stored = v;   // the field's static type, not the source's
            if (!declared->dynamic_pointee) { stored.set_type_info(declared); }
            return stored;
        }
        return v;
    }
    if (weak_slot_takes_shared(v, declared, eng)) {   // weak_ptr<T> field = shared handle (C++: weak = shared)
        auto weak = script_value::make_weak_ptr(v, eng);
        if (weak) { return std::move(weak.value()); }
        return std::nullopt;
    }
    if (target != script_value_type::jai_shared_ptr_type) {
        return std::nullopt;
    }
    const size_t ri = v.raw_storage_index();
    if (ri != script_value::TYPEID_OBJECT && ri != script_value::TYPEID_CPP_BOUND) {
        return std::nullopt;
    }
    if (v.get_object_holder() == nullptr || declared->type_name.empty()) {
        return v;
    }
    auto tag = v.get_type_info();
    if (!tag || tag->base_type == script_value_type::jai_any_type) {
        return v;
    }
    auto view = resolve_slot_class(v, eng);
    if (class_slot_admits(view, v, *declared, eng)) {
        return v;
    }
    return std::nullopt;
}

// STORE-READY field value for a resolved verdict: scalars/strings re-tag to the declared
// type, pointer-ish and container values keep
// their own tag (objects travel as object OR shared_ptr wrappers over one holder, so a
// retag would lie about the storage). A needs_* verdict here means the caller runs
// without script rows: no conversion. nullopt = no conversion.
inline std::optional<script_value> field_store_value(slot_conversion& out, const script_value& value_in, const type_info_ptr& declared, engine* eng) {
    using verdict = slot_conversion::verdict;
    const script_value& v = value_in.is_reference() ? value_in.deref() : value_in;
    std::optional<script_value> stored;
    switch (out.v) {
        case verdict::identity:
        case verdict::identity_exact:
            stored = v;
            break;
        case verdict::converted:
            stored = std::move(*out.value);
            break;
        case verdict::handle_target:
            stored = field_handle_rows(v, declared, eng);
            break;
        default:
            break;
    }
    if (stored) {
        switch (declared->base_type) {
            case script_value_type::jai_int_type: case script_value_type::jai_float_type:
            case script_value_type::jai_bool_type: case script_value_type::jai_char_type:
            case script_value_type::jai_string_type:
                stored->set_type_info(declared);
                break;
            default:
                break;
        }
    }
    return stored;
}

// ---- Diagnostic spellings shared by every typed-boundary and overload site ----

// Source spelling for rejection texts: the payload's own type - a class name for objects
// and handles, "null", the primitive names, "array" / "map" - never a var tag. Every spelling
// is already interned (base names at backend init, class names at definition), so an error
// path may intern it even while the symbolizer is frozen.
inline std::string slot_source_type_name(const script_value& value_in, engine* eng);

// A class slot whose class has no converting constructor for the source
JAI_NOINLINE inline error_propagator no_converting_constructor_error(string_symbolizer* symbols, const script_value& source, const type_info& target,
                                                                     engine* eng) {
    return error_propagator{make_error_code(runtime_error_code::type_mismatch), "Cannot convert {0} to {1} (no suitable single-argument constructor)",
                            symbols->intern(slot_source_type_name(source, eng)), target.id};
}

inline std::string slot_source_type_name(const script_value& value_in, engine* eng) {
    const script_value& v = value_in.is_reference() ? value_in.deref() : value_in;
    const type_info* tag = v.get_type_info().get();
    const bool named_tag = tag && tag->base_type != script_value_type::jai_any_type && !tag->type_name.empty();
    switch (v.raw_storage_index()) {
        case script_value::TYPEID_NULL: return "null";
        case script_value::TYPEID_INT: return tag && tag->is_enum ? tag->type_name : std::string("int");
        case script_value::TYPEID_OBJECT:
        case script_value::TYPEID_CPP_BOUND: {
            if (v.raw_storage_index() == script_value::TYPEID_CPP_BOUND && v.current_type() != script_value_type::jai_null_type) {
                return container_base_type_name(v.current_type());
            }
            if (named_tag) return tag->type_name;
            auto view = resolve_slot_class(v, eng);
            if (view.def) return view.def->get_name();
            if (auto holder = v.get_object_holder(); holder && !holder->type_name().empty()) return std::string(holder->type_name());
            return "object";
        }
        case script_value::TYPEID_PARALLEL_BORROW:
            if (const object_borrow_record* record = v.parallel_object_borrow()) return std::string(record->source->get_class_name());
            return container_base_type_name(v.current_type());
        default:
            return container_base_type_name(v.current_type());
    }
}

// Declared-type spelling: the canonical name, shared_ptr<T> spelled in full
inline std::string slot_type_spelling(const type_info& t) {
    if (t.base_type == script_value_type::jai_shared_ptr_type) { return "shared_ptr<" + t.type_name + ">"; }
    return t.canonical_name();
}

// "(int, string)" for an overload miss: at most 6 spelled, then "..."
inline std::string argument_type_list(const script_value* args, size_t argc, engine* eng) {
    std::string out = "(";
    const size_t shown = argc < 6 ? argc : 6;
    for (size_t i = 0; i < shown; ++i) {
        if (i) { out += ", "; }
        out += slot_source_type_name(args[i], eng);
    }
    if (argc > shown) { out += ", ..."; }
    return out + ")";
}

// No overload / constructor takes these arguments: {0} = the callee, {1} = the argument list,
// composed only while the symbolizer is unfrozen (a parallel worker gets the list-free shape)
inline error_propagator overload_miss(std::error_code ec, uint64_t name_id, const script_value* args, size_t argc,
                                      engine* eng, bool constructor) {
    string_symbolizer* symbols = eng ? engine_access::get_symbolizer(*eng) : nullptr;
    if (symbols && !symbols->frozen()) {
        return {ec, constructor ? "No constructor of '{0}' takes {1}" : "No overload of '{0}' takes {1}",
                name_id, symbols->intern(argument_type_list(args, argc, eng))};
    }
    return {ec, constructor ? "No constructor of '{0}' matches this call" : "No overload of '{0}' matches this call", name_id, 0};
}

// A method with one declaration called with a count its parameters do not accept: the function's arity text, the method by
// name; a parallel worker's frozen symbolizer keeps the list-free overload shape
inline error_propagator method_arity_miss(uint64_t name_id, size_t expected, size_t argc, engine* eng) {
    string_symbolizer* symbols = eng ? engine_access::get_symbolizer(*eng) : nullptr;
    const std::error_code ec = make_error_code(runtime_error_code::argument_count_mismatch);
    if (symbols && !symbols->frozen()) {
        return {ec, arity_named, name_id, symbols->intern(arity_detail(expected, argc))};
    }
    return {ec, "No overload of '{0}' matches this call", name_id, 0};
}

// Typed-boundary rejections (detail/slot_site.hpp): the site names its slot; the shape
// follows the site kind.
inline std::string callee_spelling(std::string_view callee) {
    return callee == "<lambda>" ? std::string("lambda") : "'" + std::string(callee) + "'";
}

inline std::string slot_description(const slot_site& site, const type_info& target) {
    const std::string declared = slot_type_spelling(target) + (site.by_reference ? "&" : "");
    switch (site.kind) {
        case slot_kind::parameter: {
            std::string out = "parameter '" + std::string(site.name) + "'";
            if (!site.owner.empty()) { out += " of " + callee_spelling(site.owner); }
            return out + " (declared " + declared + ")";
        }
        case slot_kind::return_value:
        case slot_kind::yield_value:
            return callee_spelling(site.owner) + " (declared -> " + declared + ")";
        case slot_kind::field:
        case slot_kind::static_field: {
            std::string out = std::string(site.kind == slot_kind::field ? "field '" : "static '") + std::string(site.name) + "'";
            if (!site.owner.empty()) { out += " of " + std::string(site.owner); }
            return out + " (declared " + declared + ")";
        }
        default:
            return "'" + std::string(site.name) + "' (declared " + declared + ")";
    }
}

// {0} = the source type, {1} = the slot description
inline std::string_view slot_rejection_message(slot_kind kind) {
    switch (kind) {
        case slot_kind::parameter: return "Cannot pass {0} to {1}";
        case slot_kind::return_value: return "Cannot return {0} from {1}";
        case slot_kind::yield_value: return "Cannot yield {0} from {1}";
        case slot_kind::declaration: return "Cannot initialize {1} with {0}";
        default: return "Cannot assign {0} to {1}";
    }
}

// Slot-less shapes ({1} = the declared type): an unnamed site, or a frozen symbolizer
inline std::string_view slot_rejection_message_unnamed(slot_kind kind, std::string_view declared) {
    const char c = declared.empty() ? 'x' : static_cast<char>(declared.front() | 0x20);
    const bool vowel = c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
    switch (kind) {
        case slot_kind::parameter: return vowel ? "Cannot pass {0} to an {1} parameter" : "Cannot pass {0} to a {1} parameter";
        case slot_kind::return_value: return "Cannot return {0} from a function declared -> {1}";
        case slot_kind::yield_value: return "Cannot yield {0} from a coroutine declared -> {1}";
        case slot_kind::declaration: return "Cannot initialize a variable declared {1} with {0}";
        case slot_kind::field: return "Cannot assign {0} to a field declared {1}";
        case slot_kind::static_field: return "Cannot assign {0} to a static declared {1}";
        default: return "Cannot assign {0} to a variable declared {1}";
    }
}

inline bool slot_site_named(const slot_site& site) {
    return site.kind == slot_kind::return_value || site.kind == slot_kind::yield_value ? !site.owner.empty() : !site.name.empty();
}

inline error_propagator typed_slot_rejection(const script_value& source, const type_info& target, const slot_site& site, engine* eng) {
    const std::error_code ec = make_error_code(runtime_error_code::type_mismatch);
    string_symbolizer* symbols = eng ? engine_access::get_symbolizer(*eng) : nullptr;
    if (!symbols) {
        return {ec, slot_rejection_message_unnamed(site.kind, target.canonical_name()), 0, target.id};
    }
    const uint64_t source_id = symbols->intern(slot_source_type_name(source, eng));
    if (slot_site_named(site) && !symbols->frozen()) {
        return {ec, slot_rejection_message(site.kind), source_id, symbols->intern(slot_description(site, target))};
    }
    return {ec, slot_rejection_message_unnamed(site.kind, target.canonical_name()), source_id, target.id};
}

// A store into a handle variable that re-points it: a weak_ptr takes only handles, and a shared_ptr re-points for null, a weak_ptr
// (refused) or another shared handle; any other value is the held object's to take
inline bool handle_rebinds(const script_value& current, const script_value& value) {
    if (current.is_weak_ptr()) { return true; }
    const type_info* tag = current.type_info_raw();
    if (!tag || tag->base_type != script_value_type::jai_shared_ptr_type) { return false; }
    const type_info* incoming = value.type_info_raw();
    return value.is_null() || value.is_weak_ptr() || (incoming && incoming->base_type == script_value_type::jai_shared_ptr_type);
}

// A weak_ptr<T> declaration's value: an empty handle for null, a shared_ptr<T> (or a subclass's) downgraded, another weak_ptr as
// it is, each under the declared type, which every later store keeps; anything else is the declaration's error
inline checked_result<script_value> weak_declaration_value(const type_info_ptr& declared, script_value value, engine* eng, const slot_site& site) {
    if (value.is_null()) {
        return script_value::make_empty_weak_ptr(declared, eng);
    }
    if (value.is_weak_ptr()) {
        value.set_type_info(declared);
        return value;
    }
    if (!weak_slot_takes_shared(value, declared, eng)) {
        return typed_slot_rejection(value, *declared, site, eng);
    }
    auto weak = script_value::make_weak_ptr(value, eng);
    if (weak) { weak.value().set_type_info(declared); }
    return weak;
}

// A compound assignment whose operator does not take its operands (compound assignment): op 0..4 is
// += -= *= /= %=, {0} = the target's type, {1} = the operand's. Every spelling is already interned (see above).
inline constexpr std::string_view compound_mismatch_texts[10] = {
    "Cannot apply '+=' to {0} and {1}", "Cannot apply '-=' to {0} and {1}", "Cannot apply '*=' to {0} and {1}",
    "Cannot apply '/=' to {0} and {1}", "Cannot apply '%=' to {0} and {1}", "Cannot apply '&=' to {0} and {1}",
    "Cannot apply '|=' to {0} and {1}", "Cannot apply '^=' to {0} and {1}", "Cannot apply '<<=' to {0} and {1}",
    "Cannot apply '>>=' to {0} and {1}"};

inline error_propagator compound_mismatch(size_t op, const script_value& target, const script_value& operand, engine* eng) {
    const std::error_code ec = make_error_code(runtime_error_code::type_mismatch);
    const std::string_view text = compound_mismatch_texts[op < 10 ? op : 0];
    string_symbolizer* symbols = eng ? engine_access::get_symbolizer(*eng) : nullptr;
    if (!symbols) { return {ec, text}; }
    return {ec, text, symbols->intern(slot_source_type_name(target, eng)), symbols->intern(slot_source_type_name(operand, eng))};
}

// `+=` with a text side, one table for every destination (a local, a field, this's field, a static, an element, a map entry, a
// reference): a string or utf8_string target appends a string, a char or a utf8_string; any other pairing with a text side is the
// compound's operand error ('+=' never stringifies the way binary + does)
enum class compound_text { none, append, mismatch };
// A string subscript's element (a char read through a STRING-ELEMENT reference) is a text slot: it takes text back
inline compound_text compound_plus_text(const script_value& target, const script_value& operand, bool text_slot = false) {
    const auto text = [](const script_value& v) { return v.is_string() || v.is_utf8_string(); };
    if (!text(target) && !text(operand)) { return compound_text::none; }
    if ((text_slot || text(target)) && (text(operand) || operand.raw_storage_index() == script_value::TYPEID_CHAR)) { return compound_text::append; }
    return compound_text::mismatch;
}

// ++/-- on a target that is not a number: {0} = the target's type
inline error_propagator incdec_mismatch(bool increment, const script_value& target, engine* eng) {
    const std::error_code ec = make_error_code(runtime_error_code::invalid_numeric_operand);
    const std::string_view text = increment ? "Cannot apply '++' to {0}" : "Cannot apply '--' to {0}";
    string_symbolizer* symbols = eng ? engine_access::get_symbolizer(*eng) : nullptr;
    if (!symbols) { return {ec, text}; }
    return {ec, text, symbols->intern(slot_source_type_name(target, eng))};
}

// The same text as one string (host-side throws)
inline std::string typed_slot_rejection_text(const script_value& source, const type_info& target, const slot_site& site, engine* eng) {
    const std::string src = slot_source_type_name(source, eng);
    if (slot_site_named(site)) {
        return format_error_message(slot_rejection_message(site.kind), src, slot_description(site, target));
    }
    const std::string declared = target.canonical_name();
    return format_error_message(slot_rejection_message_unnamed(site.kind, declared), src, declared);
}

// A var slot's tag: any, or a typed container's twin (contents keep their constraints; the slot stays dynamic)
inline bool var_slot(const type_info* t) noexcept {
	return t && (t->base_type == script_value_type::jai_any_type || t->dynamic_slot || t->dynamic_pointee);
}

// The tag a value takes in a var slot: a handle and a typed container keep their types through the dynamic
// twins, anything else takes `any_tag` (the slot's own any)
inline void var_stamp(script_value& value, const type_info_ptr& any_tag, engine* eng) {
	const type_info_ptr& tag = value.get_type_info();
	if (tag && tag->base_type == script_value_type::jai_shared_ptr_type) {
		if (!tag->dynamic_pointee && tag->element_type() && eng) {
			value.set_type_info(engine_access::get_type_info_shared_ptr_dynamic(*eng, tag->element_type().get()));
		}
		return;
	}
	if (tag && tag->base_type == script_value_type::jai_array_type && eng && value.raw_storage_index() == script_value::TYPEID_ARRAY) {
		if (tag->dynamic_slot) { return; }
		const type_info_ptr element = tag->element_type();
		if (element && element->base_type != script_value_type::jai_any_type && element->base_type != script_value_type::jai_null_type) {
			value.set_type_info(engine_access::get_type_info_array_dynamic(*eng, tag.get()));
			return;
		}
	}
	if (tag && tag->base_type == script_value_type::jai_map_type && eng && value.raw_storage_index() == script_value::TYPEID_MAP) {
		if (tag->dynamic_slot) { return; }
		const auto key = tag->key_type(), mapped = tag->value_type();
		auto concrete = [](const type_info_ptr& a_type) { return a_type && a_type->base_type != script_value_type::jai_any_type && a_type->base_type != script_value_type::jai_null_type; };
		if (concrete(key) || concrete(mapped)) { value.set_type_info(engine_access::get_type_info_map_dynamic(*eng, tag.get())); return; }
	}
	value.set_type_info(any_tag && any_tag->base_type == script_value_type::jai_any_type ? any_tag
						: type_info_ptr(eng ? engine_access::get_type_info_any(*eng) : nullptr));
}

// A by-value var parameter (an untyped one is a var) binds as a var declaration does, whatever tag its argument carried; an
// escaping parameter's cell holds the value
inline void var_parameter_stamp(script_value& bound, const type_info_ptr& declared, engine* eng) {
	script_value* value = &bound;
	if (auto* holder = bound.get_reference_holder()) {
		if (!holder->plain_cell()) { return; }
		value = holder->cell();
	}
	if (value->type_info_raw() != declared.get()) { var_stamp(*value, declared, eng); }
}

// A typed declaration's copy of a var-held typed container: the plain spelling again (auto locks to it)
inline void drop_var_slot(script_value& value, engine* eng) {
	const type_info_ptr& tag = value.get_type_info();
	if (tag && tag->dynamic_slot && eng) {
		if (tag->base_type == script_value_type::jai_map_type) { value.set_type_info(engine_access::get_type_info_map(*eng, tag->key_type().get(),tag->value_type().get())); }
		else { value.set_type_info(engine_access::get_type_info_array(*eng, tag->element_type().get())); }
	}
}

// The typed store: the full table (script rows included) plus the store-ready shaping. KEEP the same-base head inline -
// every member store runs it. site_fn names the slot for the rejection text (called on the error path alone).
template <typename SiteFn>
inline checked_result<script_value> typed_store_convert(const script_value& value, const type_info_ptr& declared, engine* eng, SiteFn&& site_fn) {
    {
        const script_value& v = value.is_reference() ? value.deref() : value;
        if (declared && declared->base_type == script_value_type::jai_shared_ptr_type && !declared->dynamic_pointee &&
            (v.raw_storage_index() == script_value::TYPEID_NULL ||
             (v.raw_storage_index() == script_value::TYPEID_OBJECT && v.type_info_raw() == declared.get()))) {
            script_value result = v;
            result.set_type_info(declared);
            return result;
        }
        size_t want = script_value::TYPEID_INVALID;
        switch (declared ? declared->base_type : script_value_type::jai_any_type) {
            case script_value_type::jai_int_type: want = script_value::TYPEID_INT; break;
            case script_value_type::jai_float_type: want = script_value::TYPEID_FLOAT; break;
            case script_value_type::jai_string_type: want = script_value::TYPEID_STRING; break;
            case script_value_type::jai_utf8_string_type: want = script_value::TYPEID_UTF8_STRING; break;
            case script_value_type::jai_bool_type: want = script_value::TYPEID_BOOL; break;
            case script_value_type::jai_char_type: want = script_value::TYPEID_CHAR; break;
            default: break;
        }
        if (want != script_value::TYPEID_INVALID && v.raw_storage_index() == want && !(declared->is_enum || (v.type_info_raw() && v.type_info_raw()->is_enum))) {
            script_value stored = v;
            stored.set_type_info(declared);
            return stored;
        }
    }
	auto result = convert_for_typed_slot_core(value, declared, eng);
	if (!result) { return result.error_value(); }
	slot_conversion out = std::move(result.value());
    if (out.needs_script()) {
        auto resolved = convert_typed_slot_script_rows(std::move(out), value, declared, eng);
        if (!resolved) {
            return resolved.error_value();
        }
        out = std::move(resolved.value());
    }
    auto stored = field_store_value(out, value, declared, eng);
    if (!stored) {
        return typed_slot_rejection(value, *declared, site_fn(), eng);
    }
    return std::move(*stored);
}


// The field store (field_id / owner / is_static only name the slot in the rejection text)
inline checked_result<script_value> field_store_convert(const script_value& value, const type_info_ptr& declared, engine* eng,
                                                        uint64_t field_id, const class_definition* owner, bool is_static) {
    return typed_store_convert(value, declared, eng, [&]() {
        string_symbolizer* symbols = eng ? engine_access::get_symbolizer(*eng) : nullptr;
        return slot_site{is_static ? slot_kind::static_field : slot_kind::field,
                         field_id && symbols ? symbols->get_string(field_id) : std::string_view{},
                         owner ? std::string_view(owner->get_name()) : std::string_view{}};
    });
}

// The no-script table (set_field, hot-reload migration, a load): nullopt where a conversion would
// have to run script or does not exist. A float into an int field truncates as cast<int> does (no script store is there to spell it).
inline std::optional<script_value> field_store_convert_no_script(const script_value& value, const type_info_ptr& declared, engine* eng) {
	if (declared && declared->base_type == script_value_type::jai_int_type && value.deref().raw_storage_index() == script_value::TYPEID_FLOAT) {
		auto truncated = checked_float_to_integer(value.deref().unchecked_as_float());
		if (!truncated) { return std::nullopt; }
		return script_value(truncated.value(), eng);
	}
	if (const script_value& held = value.deref(); declared && detail::math::is_kind(held.raw_storage_index()) && detail::math::narrows(detail::math::kind_of(held), declared->base_type)) {
		auto truncated = detail::math::convert(held, declared->base_type, eng);
		if (!truncated) { return std::nullopt; }
		return std::move(truncated.value());
	}
	auto result = convert_for_typed_slot_core(value, declared, eng);
	if (!result) { return std::nullopt; }
	return field_store_value(result.value(), value, declared, eng);
}

// A reload that retypes a field to text keeps the text a primitive or a container shows, as cast<string> would; an object's
// to_string() is script, which a reload never runs
inline std::optional<script_value> reload_retype_text(const script_value& value, const type_info& declared, engine* eng) {
	const script_value_type target = declared.base_type;
	const size_t ri = value.raw_storage_index();
	if ((target != script_value_type::jai_string_type && target != script_value_type::jai_utf8_string_type) || ri == script_value::TYPEID_OBJECT ||
	    (ri == script_value::TYPEID_CPP_BOUND && value.current_type() == script_value_type::jai_null_type)) {
		return std::nullopt;
	}
	std::string text = value.to_string();
	return target == script_value_type::jai_string_type ? script_value(std::move(text), eng) : script_value::make_utf8(std::move(text), eng);
}

// Script-surface member access enforcement kernel — the ONE check both backends call
// verbatim at every member get/set/compound/call site (parity by construction).
// ctx = the class whose method body is currently executing (nullptr at top level /
// free functions). Two levels: public = everyone; private = the
// declaring class and every class derived from it, through any receiver of the family;
// protected is a synonym of private. Host C++ APIs (get_field/set_field/get_fields,
// serialization) never call this — the host side stays unrestricted by design.
inline bool member_access_allowed(const class_definition* cls, uint64_t member_id, const class_definition* ctx) {
    if (!cls->chain_has_nonpublic()) { return true; }
    access_level level = access_level::public_access;
    const class_definition* declarer = cls->find_nonpublic_declarer(member_id, level);
    return !declarer || declarer == ctx || (ctx && ctx->derives_from(declarer));
}

inline checked_result<void> enforce_member_access(const class_definition* cls, uint64_t member_id,
                                                  const class_definition* ctx) {
    if (member_access_allowed(cls, member_id, ctx)) {
        return {};
    }
    access_level level = access_level::public_access;
    const class_definition* declarer = cls->find_nonpublic_declarer(member_id, level);
    return checked_result<void>(make_error_code(runtime_error_code::access_violation),
        "Cannot access private member '{0}' of class '{1}'", member_id, declarer->get_type_id());
}

// operator= on a plain store (docs/site/embedding/06-static-checking.html "One conversion table"; C++ assignment; no slicing).
// A destination slot holding a class instance by value keeps its object: a source of the held instance's class
// assigns every member into it (the implicit copy assignment: a by-value class member assigns in turn, any other member
// takes the member-copy rule, clone_element); a source of another class the slot's declared class admits (that class or a
// subclass) first destroys the levels the object loses, re-classes it in place to the source's class, then assigns every
// member; a source outside the slot's family goes through a user operator= on the held instance. nullopt when none applies
// (a handle slot rebinds, a host class keeps its replace). declared: the slot's declared type where the value's tag does not
// carry it (a field, a static, an element, a map value); bound: a reference's declared class the source must also derive.
// A parallel region never dispatches a user operator or re-classes (the operator-dispatch verdict). KEEP the one kernel for
// every plain-store site of both backends.
inline std::optional<checked_result<void>> user_assign_operator(const script_value& current_in, const script_value& value_in, engine* eng,
                                                                const type_info* declared = nullptr, const type_info* bound = nullptr,
                                                                script_value* consumable = nullptr);

// '=' of a value into a slot whose held object decides the store (docs/site/guide/03-value-semantics.html "A var holding a
// handle", "A var holding an object"). A slot holding a handle (shared_ptr<T> or a var holding one) and a value that is no
// handle: a value of the held class or a subclass assigns into the shared object, through the class's operator= when one of
// its overloads takes the value, else the implicit copy assignment; a var rebinds to any other value, a typed holder runs the
// class's operator= (its own error when it takes nothing). A var holding a class instance by value: the class's operator=
// assigns into the object when one of its overloads takes the value (never for a handle or null). nullopt: none applies (a
// typed holder's rejection, a var's plain store). declared_var: the slot is declared var whatever its value's tag (a field, an
// element, a map value). Defined out of line (environment.cpp).
enum class value_store : uint8_t { assigned, rebind };
std::optional<checked_result<value_store>> slot_value_assign(const script_value& storage, const script_value& value, engine* eng, bool declared_var = false);

// The cheap head every store site asks first: storage holding an object (a handle or an instance)
inline bool slot_value_assign_candidate(const script_value& storage) noexcept {
    const size_t si = storage.raw_storage_index();
    return si == script_value::TYPEID_OBJECT;
}

// A var field, element or map value whose held object takes the value in place (slot_value_assign's assigned verdict); nullopt:
// the store replaces (a rebind included)
inline std::optional<checked_result<void>> var_slot_assign_in_place(const script_value& current, const script_value& source, engine* eng) {
    if (!slot_value_assign_candidate(current.is_reference() ? current.deref() : current)) { return std::nullopt; }
    auto stored = slot_value_assign(current, source, eng, true);
    if (!stored || (*stored && stored->value() == value_store::rebind)) { return std::nullopt; }
    if (!*stored) {
        checked_result<void> failed = stored->error_value();
        return failed;
    }
    return checked_result<void>();
}

// A field lane writing slot directly steps aside when the field holds an object a store asks first (var_slot_assign_in_place)
inline bool field_slot_holds_object(class_instance* inst, uint32_t slot);

// The members `layout` declares (its whole chain, base first) copied from source into dest
inline checked_result<void> implicit_copy_assign(class_instance* dest, const class_definition* layout, class_instance* source, engine* eng) {
    const uint32_t n = layout->field_slot_count();
    const class_definition* dest_layout = dest->get_class_definition();
    for (uint32_t slot = 0; slot < n; ++slot) {
        if (layout->slot_is_cpp_property(slot)) { continue; }
        const uint64_t id = layout->slot_field(slot);
        const script_value* from = source->find_field_value(id);
        if (!from) { continue; }
        const script_value& member = from->is_reference() ? from->deref() : *from;
        const type_info_ptr declared = layout->get_field_declared_type(id);
        script_value* to = dest->find_field_value(id);
        if (to && declared && declared->base_type == script_value_type::jai_object_type && !declared->type_name.empty()) {
            if (auto nested = user_assign_operator(*to, member, eng, declared.get())) {
                if (!*nested) { return *nested; }
                continue;
            }
        }
        const uint32_t dest_slot = dest_layout ? dest_layout->field_slot(id) : class_definition::k_no_field_slot;
        if (dest_slot == class_definition::k_no_field_slot) { continue; }
        dest->field_slot_write(dest_slot, member.clone_element());
    }
    return {};
}

// The class named `name` in c's chain (c itself or a base), null when none
inline const class_definition* chain_class_named(const class_definition* c, std::string_view name) {
    if (!c) { return nullptr; }
    if (c->get_name() == name) { return c; }
    for (const auto& parent : c->get_parent_classes()) {
        if (const class_definition* found = chain_class_named(parent.get(), name)) { return found; }
    }
    return nullptr;
}

inline bool names_class(const type_info* t) noexcept {
    return t && t->base_type == script_value_type::jai_object_type && !t->type_name.empty();
}

// An assignment whose source is of another class than the slot's object: the levels the object loses die (their destructor
// bodies, then their members, as a dying instance runs them), the object takes the source's class in place (the dropped
// members go absent, a reference to one raises), every member assigns from the source and a member the source does not
// hold takes its default. Every handle to the object sees the new class; the slot's own tag follows when it named the old
// class.
inline checked_result<void> reclass_assign(script_value& current, auto& holder, const class_definition* slot_class,
                                           class_instance* source, const class_definition* source_def, engine* eng) {
    if (engine_access::parallel_region_active(*eng)) {
        return checked_result<void>(make_error_code(runtime_error_code::unsupported_operation),
            "an assignment that changes an object's class is not admitted in a parallel body");
    }
    strong_ptr<class_instance> self = holder.instance_handle();   // a destructor body may drop the slot or the source
    strong_ptr<class_instance> source_pin = source->weak_from_this().lock();
    class_instance* instance = self.get();
    const class_definition* def = instance->get_class_definition();
    def->destroy_levels(instance, def, source_def);
    instance->reclass(std::const_pointer_cast<class_definition>(source_def->shared_from_this()), true);
    holder.set_type(source_def->interned_name(), source_def->get_type_id());
    if (current.type_info_raw() == def->get_type_info().get() && def != slot_class) { current.set_type_info(source_def->get_type_info()); }
    auto copied = implicit_copy_assign(instance, source_def, source, eng);
    if (!copied) { return copied; }
    for (const auto& [id, default_value] : source_def->get_all_field_defaults()) {
        if (!instance->has_field_value(id)) { instance->set_field(id, default_value.clone_element()); }
    }
    return {};
}

// The implicit copy assignment alone: a slot holding a class instance by value takes a source of its declared class or a
// subclass, the slot keeping its object (an element or a map value assigns this way; nullopt otherwise)
inline std::optional<checked_result<void>> class_copy_assign(const script_value& current_in, const script_value& value_in, engine* eng,
                                                             const type_info* declared = nullptr, const type_info* bound = nullptr) {
    if (!eng) { return std::nullopt; }
    const script_value& current = current_in.is_reference() ? current_in.deref() : current_in;
    if (current.raw_storage_index() != script_value::TYPEID_OBJECT) { return std::nullopt; }
    const type_info_ptr tag = current.get_type_info();
    if (tag && tag->base_type == script_value_type::jai_shared_ptr_type) { return std::nullopt; }   // a handle rebinds
    auto holder = const_cast<script_value&>(current).get_object_holder();
    if (!holder || !holder->is_instance()) { return std::nullopt; }
    auto* instance = holder->unchecked_instance();
    const class_definition* def = instance->get_class_definition();
    if (!def) { return std::nullopt; }
    const script_value& value = value_in.is_reference() ? value_in.deref() : value_in;
    const size_t vi = value.raw_storage_index();
    if (vi != script_value::TYPEID_OBJECT) { return std::nullopt; }
    auto source_holder = const_cast<script_value&>(value).get_object_holder();
    if (!source_holder || !source_holder->is_instance()) { return std::nullopt; }
    auto* source = source_holder->unchecked_instance();
    const class_definition* source_def = source->get_class_definition();
    if (!source_def) { return std::nullopt; }
    // the slot's class: its declared type names the held instance's class or one of its bases
    const type_info* slot_type = names_class(declared) ? declared : tag.get();
    const class_definition* slot_class = def;
    if (names_class(slot_type) && def->get_name() != slot_type->type_name) {
        if (const class_definition* found = chain_class_named(def, slot_type->type_name)) { slot_class = found; }
    }
    if (source_def != slot_class && !source_def->derives_from(slot_class)) { return std::nullopt; }
    if (!slot_class->is_script_class() || slot_class->find_cpp_base_class()) { return std::nullopt; }   // a host object keeps its replace
    if (source == instance) { return checked_result<void>(); }   // self-assignment
#ifdef JAISCRIPT_VM_COVERAGE
    census::copy_scope census_scope(census::site::copy_assign, nullptr, source_holder.get(),
        (source_holder.use_count() == 2 && source_holder->object_use_count() == 1) ? census::k_src_unique : 0u);
    census::add_weight(source_def->field_slot_count());
#endif
    if (source_def == def) { return implicit_copy_assign(instance, def, source, eng); }
    if (source_def->find_cpp_base_class() || (names_class(bound) && !chain_class_named(source_def, bound->type_name))) { return std::nullopt; }
    return reclass_assign(const_cast<script_value&>(current), *holder, slot_class, source, source_def, eng);
}

// The class's operator= called on the slot's object with the value (a parallel chunk's through its region pin)
inline checked_result<void> call_assign_operator(const script_value& current, const script_value* method, const script_value& value, uint64_t assign_id, engine* eng) {
    if (engine_access::parallel_region_active(*eng)) {
        // a chunk calls the operator through its region pin, on its own backend; an unpinned one keeps the verdict
        if (auto called = engine_access::parallel_operator_call(*eng, current, assign_id, { value })) {
            if (!*called) {
                checked_result<void> failed = called->error_value();
                return failed;
            }
            return checked_result<void>();
        }
        return checked_result<void>(make_error_code(runtime_error_code::unsupported_operation),
            "custom operator dispatch on class instances is not admitted in a parallel body");
    }
    script_value function = *method;
    std::vector<script_value> args{current, value};
    auto result = function.as_function()(args);
    if (!result) {
        checked_result<void> failed = result.error_value();
        return failed;
    }
    return checked_result<void>();
}

// The overload choice and the call of script_assign_operator, out of line (environment.cpp)
std::optional<checked_result<void>> run_script_assign_operator(const script_value& current, const class_definition* def, uint64_t assign_id,
                                                               const script_value& value, engine* eng);

// '=' into a slot holding a class instance by value whose class declares a script operator= with an overload that takes the
// value: the operator assigns, self-assignment included (C++ assignment); nullopt otherwise. A class declaring none costs one
// memoized check.
inline std::optional<checked_result<void>> script_assign_operator(const script_value& current_in, const script_value& value_in, engine* eng) {
    const script_value& current = current_in.is_reference() ? current_in.deref() : current_in;
    if (current.raw_storage_index() != script_value::TYPEID_OBJECT || !eng) { return std::nullopt; }
    const type_info* tag = current.type_info_raw();
    if (tag && tag->base_type == script_value_type::jai_shared_ptr_type) { return std::nullopt; }   // a handle rebinds
    auto holder = const_cast<script_value&>(current).get_object_holder();
    if (!holder || !holder->is_instance()) { return std::nullopt; }
    const class_definition* def = holder->unchecked_instance()->get_class_definition();
    const uint64_t assign_id = def ? def->script_assign_operator_id() : 0;
    if (assign_id == 0) { return std::nullopt; }
    return run_script_assign_operator(current, def, assign_id, value_in.is_reference() ? value_in.deref() : value_in, eng);
}

inline std::optional<checked_result<void>> user_assign_operator(const script_value& current_in, const script_value& value_in, engine* eng,
                                                                const type_info* declared, const type_info* bound, script_value* consumable) {
    if (auto assigned = script_assign_operator(current_in, value_in, eng)) { return assigned; }
    if (auto copied = class_copy_assign(current_in, value_in, eng, declared, bound)) { return copied; }
    if (consumable || names_class(declared)) {
        if (auto converted = converting_copy_assign(current_in, value_in, eng, declared, consumable)) { return converted; }
    }
    if (!eng) { return std::nullopt; }
    const script_value& current = current_in.is_reference() ? current_in.deref() : current_in;
    if (current.raw_storage_index() != script_value::TYPEID_OBJECT) { return std::nullopt; }
    const type_info_ptr tag = current.get_type_info();
    if (tag && tag->base_type == script_value_type::jai_shared_ptr_type) { return std::nullopt; }   // a handle rebinds
    auto holder = const_cast<script_value&>(current).get_object_holder();
    if (!holder || !holder->is_instance()) { return std::nullopt; }
    auto* instance = holder->unchecked_instance();
    const class_definition* def = instance->get_class_definition();
    if (!def) { return std::nullopt; }
    const script_value& value = value_in.is_reference() ? value_in.deref() : value_in;
    if (value.is_null()) { return std::nullopt; }
    const uint64_t assign_id = engine_access::get_symbolizer(*eng)->intern("=");
    if (value.raw_storage_index() == script_value::TYPEID_OBJECT) {
        auto source_holder = const_cast<script_value&>(value).get_object_holder();
        if (source_holder && source_holder->is_instance()) {
            const class_definition* source_def = source_holder->unchecked_instance()->get_class_definition();
            // a bound class's own copy runs the operator= its binding binds ("="), as C++ assigns into the held object; without one
            // the store replaces the object with a copy-constructed one (script owns the slot's storage)
            if (source_def && (source_def == def || source_def->derives_from(def)) && !(def->is_cpp_class() && def->has_method(assign_id))) {
                return std::nullopt;
            }
        }
    }
    const script_value* bound_assign = def->assignment_operator_ptr(assign_id);
    if (!bound_assign || !bound_assign->is_function()) { return std::nullopt; }
    return call_assign_operator(current, bound_assign, value, assign_id, eng);
}

// A by-value capture copies as a declaration from the captured variable does (C++): a value, a C++ object included, copies
// through its copy constructor; a shared_ptr handle copies the handle and shares the object
inline script_value clone_for_capture(const script_value& value) {
    return clone_for_assignment(value);
}

// A declaration of an object, after its initializer's copy rule, as C++ declares: a host object a T& result lends copies
// (var, auto and T alike), and a value slot (T x = handle) copies the object the handle names unless the handle is a script
// temporary nothing else holds. KEEP BYTE-PARALLEL: both backends' declarations call it
JAI_NOINLINE inline void object_declaration_copies(script_value& value, const type_info* declared) {
    if (borrows_host_object(value)) {
        value = value.clone();
        return;
    }
    if (!declared || declared->base_type != script_value_type::jai_object_type) { return; }
    const type_info* tag = value.type_info_raw();
    if (!tag || tag->base_type != script_value_type::jai_shared_ptr_type) { return; }
    const auto& holder = value.unchecked_object_handle();
    const bool sole_script_object = uniquely_held(value) && !holder->unchecked_instance()->has_cpp_object();
    if (!sole_script_object) {
        value = value.clone();
    }
}

// An ordinary object declaration (no lent host object, no handle entering a class slot) returns without a call
inline void object_declaration_takes_value(script_value& value, const type_info* declared) {
    if (!value.unchecked_object_handle()->bound_object()) {
        if (!declared || declared->base_type != script_value_type::jai_object_type) { return; }
        const type_info* tag = value.type_info_raw();
        if (!tag || tag->base_type != script_value_type::jai_shared_ptr_type) { return; }
    }
    object_declaration_copies(value, declared);
}

// A store into a declared field or static: an auto slot takes the type its initialized value carries, then enforces it
// like a local; a class slot runs its user operator=, else the one conversion table
template <class CurrentSlot>
inline checked_result<script_value> declared_slot_write(type_info_ptr declared, CurrentSlot&& current_slot, script_value value, engine* eng,
                                                        uint64_t id, const class_definition* owner, bool is_static) {
    if (!declared) {
        const script_value* current = current_slot();
        type_info_ptr tag = (current && !current->is_reference()) ? current->get_type_info() : nullptr;
        if (!tag || tag->base_type == script_value_type::jai_any_type ||
            tag->base_type == script_value_type::jai_null_type) {
            return std::move(value);
        }
        declared = tag;
    } else if (declared->base_type == script_value_type::jai_any_type) {
        return std::move(value);
    }
    if (declared->base_type == script_value_type::jai_object_type) {
        if (const script_value* current = current_slot()) {
            if (auto assigned = user_assign_operator(*current, value, eng, declared.get())) {
                if (!*assigned) { return assigned->error_value(); }
                return *current;   // the operator wrote the held instance: the store keeps it
            }
        }
    }
    return field_store_convert(value, declared, eng, id, owner, is_static);
}

// The class_instance lookups' failure texts, out of line so the lookups stay lean
[[noreturn]] JAI_NOINLINE inline void throw_field_missing(const class_definition* def, uint64_t id) {
    std::string field_name;
    if (def) {
        if (auto eng = def->get_engine()) {
            field_name = engine_access::get_symbolizer(*eng)->get_string(id);
        } else {
            field_name = std::to_string(id);
        }
    } else {
        field_name = std::to_string(id);
    }
    throw runtime_error("Field '" + field_name + "' not found and no default value available");
}

[[noreturn]] JAI_NOINLINE inline void throw_no_class_definition(uint64_t id) {
    throw runtime_error("Class definition not available for method with ID " + std::to_string(id) + " lookup");
}

[[noreturn]] JAI_NOINLINE inline void throw_field_store_rejection(const script_value& value, const type_info& declared, uint64_t id,
                                                                  const class_definition& def, engine* eng) {
    string_symbolizer* symbols = eng ? engine_access::get_symbolizer(*eng) : nullptr;
    throw runtime_error(typed_slot_rejection_text(value, declared,
        slot_site{slot_kind::field, symbols ? symbols->get_string(id) : std::string_view{}, def.get_name()}, eng));
}

} // namespace detail

inline checked_result<script_value> class_instance::enforce_field_write(uint64_t id, script_value value) const {
    if (!class_def_) {
        return std::move(value);
    }
    return detail::declared_slot_write(class_def_->get_field_declared_type(id), [&] { return find_field_value(id); }, std::move(value), owner_, id, class_def_, false);
}

inline std::optional<checked_result<void>> class_instance::assign_field_in_place(uint64_t id, const script_value& source, script_value* consumable) const {
    if (!class_def_) { return std::nullopt; }
    const type_info_ptr declared = class_def_->get_field_declared_type(id);
    if (!declared || declared->base_type == script_value_type::jai_any_type) {
        const script_value* current = find_field_value(id);
        return current ? detail::var_slot_assign_in_place(*current, source, owner_) : std::nullopt;
    }
    if (declared->base_type != script_value_type::jai_object_type || declared->type_name.empty()) { return std::nullopt; }
    const script_value* current = find_field_value(id);
    if (!current) { return std::nullopt; }
    return detail::user_assign_operator(*current, source, owner_, declared.get(), nullptr, consumable);
}

inline bool detail::field_slot_holds_object(class_instance* inst, uint32_t slot) {
    const script_value* current = inst->field_slot_value(slot);
    return current && slot_value_assign_candidate(*current);
}

template <class Copy>
inline checked_result<script_value> class_instance::field_store(uint64_t id, const script_value& source, Copy&& copy, script_value* consumable) const {
    if (auto in_place = assign_field_in_place(id, source, consumable)) {
        if (!*in_place) { return in_place->error_value(); }
        const script_value* kept = find_field_value(id);
        return kept ? script_value(kept->deref()) : script_value(std::monostate{}, owner_);
    }
    if (class_def_ && detail::slot_converts_object(source.deref(), class_def_->get_field_declared_type(id).get())) { return enforce_field_write(id, source.deref()); }
    return enforce_field_write(id, copy());
}

template <class Copy>
inline checked_result<script_value> class_instance::field_store_at(uint32_t slot, uint64_t id, const script_value& source, Copy&& copy,
                                                                     script_value* consumable) const {
    if (!class_def_) { return field_store(id, source, copy, consumable); }
    const type_info_ptr declared = class_def_->field_declared_type_at(slot, id);
    script_value* current = const_cast<class_instance*>(this)->field_slot_value(slot);
    if (!declared || declared->base_type == script_value_type::jai_any_type) {
        if (current) {
            if (auto in_place = detail::var_slot_assign_in_place(*current, source, owner_)) {
                if (!*in_place) { return in_place->error_value(); }
                const script_value* kept = const_cast<class_instance*>(this)->field_slot_value(slot);
                return kept ? script_value(kept->deref()) : script_value(std::monostate{}, owner_);
            }
        }
    } else if (declared->base_type == script_value_type::jai_object_type && !declared->type_name.empty() && current) {
        if (auto in_place = detail::user_assign_operator(*current, source, owner_, declared.get(), nullptr, consumable)) {
            if (!*in_place) { return in_place->error_value(); }
            const script_value* kept = const_cast<class_instance*>(this)->field_slot_value(slot);   // the operator may have run code that moved the fields
            return kept ? script_value(kept->deref()) : script_value(std::monostate{}, owner_);
        }
    }
    script_value stored = detail::slot_converts_object(source.deref(), declared.get()) ? script_value(source.deref()) : copy();
    return detail::declared_slot_write(declared, [&] { return const_cast<class_instance*>(this)->field_slot_value(slot); }, std::move(stored), owner_, id, class_def_, false);
}

template <class Copy>
inline checked_result<script_value> class_definition::static_store(uint64_t id, const script_value& source, Copy&& copy) const {
    auto it = static_declared_types_.find(id);
    if (it == static_declared_types_.end() || !it->second || it->second->base_type == script_value_type::jai_any_type) {
        if (const script_value* current = get_static_field_ptr(id)) {
            if (auto in_place = detail::var_slot_assign_in_place(*current, source, engine_)) {
                if (!*in_place) { return in_place->error_value(); }
                return script_value(current->deref());
            }
        }
    }
    if (it != static_declared_types_.end() && it->second && it->second->base_type == script_value_type::jai_object_type && !it->second->type_name.empty()) {
        if (const script_value* current = get_static_field_ptr(id)) {
            if (auto in_place = detail::user_assign_operator(*current, source, engine_, it->second.get())) {
                if (!*in_place) { return in_place->error_value(); }
                return script_value(current->deref());
            }
        }
    }
    if (it != static_declared_types_.end() && detail::slot_converts_object(source.deref(), it->second.get())) { return enforce_static_write(id, source.deref()); }
    return enforce_static_write(id, copy());
}

namespace detail {
// A slot declared a class by value: a store into it assigns into the held instance (the proven field lanes step aside for it)
inline bool by_value_class_type(const type_info* t) noexcept {
    return t && t->base_type == script_value_type::jai_object_type && !t->type_name.empty();
}

// An operator's result stored back by a compound assignment: a slot holding a class by value copy-assigns it (the slot keeps
// its object, the result dies after), any other slot takes it
inline checked_result<void> compound_slot_store(script_value& target, script_value&& result, engine* eng) {
    if (by_value_class_type(target.type_info_raw())) {
        if (auto assigned = user_assign_operator(target, result, eng)) { return *assigned; }
    }
    target = std::move(result);
    return {};
}

// An element or map value declared a class (array<T>, map<K, T>) holding an instance takes a source of that class by the copy
// assignment (a bound class's bound "="), the element keeping its object; nullopt for a var element or any other store, which
// replaces
inline std::optional<checked_result<void>> element_copy_assign(const script_value& current, const script_value& source, const type_info_ptr& element_type, engine* eng) {
    if (!element_type || element_type->base_type == script_value_type::jai_any_type) { return var_slot_assign_in_place(current, source, eng); }
    if (!element_type || element_type->base_type != script_value_type::jai_object_type || element_type->type_name.empty()) { return std::nullopt; }
    if (auto assigned = script_assign_operator(current, source, eng)) { return assigned; }
    if (auto copied = class_copy_assign(current, source, eng, element_type.get())) { return copied; }
    const script_value& held = current.is_reference() ? current.deref() : current;
    if (!eng || held.raw_storage_index() != script_value::TYPEID_OBJECT) { return std::nullopt; }
    auto holder = const_cast<script_value&>(held).get_object_holder();
    if (!holder || !holder->is_instance()) { return std::nullopt; }
    const class_definition* def = holder->unchecked_instance()->get_class_definition();
    if (!def || !def->is_cpp_class() || !def->has_method(engine_access::get_symbolizer(*eng)->intern("="))) { return std::nullopt; }
    return user_assign_operator(current, source, eng, element_type.get());
}
}

inline checked_result<script_value> class_definition::enforce_static_write(uint64_t id, script_value value) const {
    if (!const_statics_.empty() && is_const_static(id)) [[unlikely]] {   // a class constant
        return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "Cannot assign to '{0}': it is read-only", id);
    }
    auto it = static_declared_types_.find(id);
    if (it == static_declared_types_.end()) {
        return std::move(value);
    }
    return detail::declared_slot_write(it->second, [&] { return get_static_field_ptr(id); }, std::move(value), engine_, id, this, true);
}

inline bool class_definition::wrapped_object_has_layer(const class_instance& inst) const {
    if (!script_slot_hooks_.slot || is_script_class()) { return false; }
    const char* object = static_cast<const char*>(inst.cpp_object_pointer());
    if (!object) { return false; }
    int32_t at = wrapper_slot_offset_.load(std::memory_order_relaxed);
    if (at == INT32_MIN) [[unlikely]] {
        at = static_cast<int32_t>(reinterpret_cast<const char*>(script_slot_hooks_.slot(object)) - object);
        wrapper_slot_offset_.store(at, std::memory_order_relaxed);
    }
    return reinterpret_cast<const script_extensible*>(object + at)->has_script_layer();
}

inline void class_instance::note_script_layer_owner(const script_value& value) {
    if (!owner_ || !class_def_) { return; }
    auto cpp_base = class_def_->find_cpp_base_class();
    if (!cpp_base) { return; }
    auto holder = value.get_object_holder();
    if (!holder) { return; }
    void* object = holder->holds_object() ? holder->object() : holder->bound_object();
    if (!object) { return; }
    auto weak_self = weak_from_this();
    if (weak_self.expired()) { return; }
    if (auto hooks = cpp_base->slot_hooks()) {
        hooks->attach(object, weak_self);
        return;
    }
    detail::engine_access::note_script_layer(*owner_, object, weak_self);
}

inline void class_instance::set_field(uint64_t id, const script_value& value) {
    // Null passes through: typed-null placeholders (create_instance defaults,
    // uninitialized fields) must remain storable from C++.
    if (class_def_ && !value.is_null()) {
        if (type_info_ptr declared = class_def_->get_field_declared_type(id);
            declared && declared->base_type != script_value_type::jai_any_type) {
            auto converted = detail::field_store_convert_no_script(value, declared, owner_);
            if (!converted) [[unlikely]] {
                detail::throw_field_store_rejection(value, *declared, id, *class_def_, owner_);
            }
            set_field_unchecked(id, *converted);
            return;
        }
    }
    set_field_unchecked(id, value);
}

inline void class_instance::set_initialized_field(uint64_t id, const script_value& value) {
    if (class_def_ && value.is_null()) [[unlikely]] {
        if (type_info_ptr declared = class_def_->get_field_declared_type(id);
            declared && declared->base_type != script_value_type::jai_any_type) {
            auto converted = detail::field_store_convert_no_script(value, declared, owner_);
            if (!converted) [[unlikely]] {
                detail::throw_field_store_rejection(value, *declared, id, *class_def_, owner_);
            }
            if (!converted->is_null()) {
                set_field_unchecked(id, *converted);
                return;
            }
        }
    }
    set_field(id, value);
}

inline void class_instance::migrate_fields(const std::set<uint64_t>& old_field_ids,
                                           const std::unordered_map<uint64_t, script_value>& new_field_defaults,
                                           bool preseeded) {
    const bool reporting = owner_ && owner_->hot_reload_reporting();
    auto record = [&](uint64_t field_id, field_migration_outcome outcome, const script_value* before, const script_value* after) {
        if (!reporting) { return; }
        // a field the reload seeded before this pass is new to the instance whatever path finds it
        if (preseeded && outcome != field_migration_outcome::removed && !old_field_ids.count(field_id)) {
            outcome = field_migration_outcome::added;
            before = nullptr;
        }
        detail::engine_access::record_field_migration(*owner_, type_name(), field_id, outcome, before, after);
    };
    // IN-PLACE per-id migration: kept fields keep their storage CELL, so script_value
    // addresses stay stable — suspended coroutines' env chains and this-capturing
    // closures cache pointers to field cells, and a wholesale rebuild would dangle
    // every one of them. (Flat slots are append-only per class lifetime; a unique_ptr
    // pointee never moves.)
    for (const auto& [id, default_value] : new_field_defaults) {
        script_value* existing = find_field_value(id);
        type_info_ptr declared = class_def_ ? class_def_->get_field_declared_type(id) : nullptr;
        if (existing && !existing->is_reference() && !existing->is_null() &&
            declared && declared->base_type != script_value_type::jai_any_type) {
            // Hot-reload retype rule: a declared-type change CONVERTS the live
            // value wherever the assignment table allows (float->int truncates, subtype objects
            // stay) and a retype to text keeps a primitive's or container's text (int->string as
            // cast<string>); a value with NO conversion (object into int, string into int) or one
            // that converts only through script (a converting ctor, to_X()) falls back to the new
            // initializer default. Reloads stay permissive, never run script and never brick an
            // instance. Same-type fields pass the kernel's same-type fast path and keep their value.
            auto converted = detail::field_store_convert_no_script(*existing, declared, owner_);
            if (!converted) { converted = detail::reload_retype_text(existing->deref(), *declared, owner_); }
            if (converted) {
                record(id, field_migration_outcome::converted, existing, &*converted);
                *existing = std::move(*converted);
            } else {
                record(id, field_migration_outcome::reset, existing, &default_value);
                *existing = default_value.clone();
            }
            continue;
        }
        // Undeclared ('auto') / var / reference / null-valued fields keep the tag heuristic:
        // keep the existing runtime value, EXCEPT when a non-null new default's kind differs
        // from the held value (an auto field reloaded with a different-typed initializer), so
        // adopt the new default. A null default carries no type and never triggers a reset.
        if (existing) {
            bool retyped = !default_value.is_null() &&
                existing->deref().current_type() != default_value.deref().current_type();
            if (retyped) {
                record(id, field_migration_outcome::reset, existing, &default_value);
                *existing = default_value.clone();
            } else {
                record(id, field_migration_outcome::preserved, existing, existing);
            }
        } else {
            materialize_field(id, default_value.clone());
            record(id, field_migration_outcome::added, nullptr, find_field_value(id));
        }
    }

}

inline void class_instance::drop_fields_absent_from(const std::unordered_map<uint64_t, script_value>& new_field_defaults) {
    const bool reporting = owner_ && owner_->hot_reload_reporting();
    auto record = [&](uint64_t field_id, const script_value* before) {
        if (reporting) { detail::engine_access::record_field_migration(*owner_, type_name(), field_id, field_migration_outcome::removed, before, nullptr); }
    };
    // Slots drop IN PLACE (value overwritten with the invalid absent marker: the cell
    // address survives, the dropped value's resources free NOW like the old map erase).
    // The C++ object is no field and stays.
    if (class_def_) {
        const size_t n = flat_fields_.size();
        for (size_t slot = 0; slot < n; ++slot) {
            script_value* v = flat_fields_.present_cell(slot);
            if (!v) {
                continue;
            }
            const uint64_t id = class_def_->slot_field(static_cast<uint32_t>(slot));
            if (new_field_defaults.find(id) == new_field_defaults.end()) {
                record(id, v);
                *v = script_value::make_invalid(owner_);
            }
        }
    }
    if (overflow_fields()) {
        for (auto it = overflow_fields()->begin(); it != overflow_fields()->end();) {
            if (new_field_defaults.find(it->first) == new_field_defaults.end()) {
                record(it->first, &it->second);
                it = overflow_fields()->erase(it);
            } else {
                ++it;
            }
        }
    }
}

inline std::shared_ptr<void> class_instance::extract_cpp_object_impl(const script_value& val) {
    if (val.type() == script_value_type::jai_object_type) {
        auto obj_holder = val.get_object_holder();
        if (!obj_holder) {
            detail::throw_conversion_error(val, "object");
        }
        if (obj_holder->holds_object()) { return obj_holder->host_object(); }
        // an extended object's non-owning holder: hand the pointer out as an unowned alias
        if (obj_holder->bound_object()) { return std::shared_ptr<void>(std::shared_ptr<void>(), obj_holder->bound_object()); }
        return nullptr;
    }
    return nullptr;
}

inline void class_instance::set_native(const script_value& value) {
    if (class_def_ && !value.is_null()) { note_script_layer_owner(value); }
    extras().native = value;
}

inline void class_instance::set_native_view(const script_value& value) { extras().native = value; }

inline bool class_instance::native_lookup_enabled() const { return class_def_ && class_def_->get_engine(); }

inline const script_value& class_instance::native_or_throw() const {
    const script_value* held = native();
    if (!held) [[unlikely]] { throw runtime_error("Field '_cpp_object' not found and no default value available"); }
    return *held;
}

inline strong_ptr<class_instance> class_instance::deep_copy() const {
    detail::deep_copy_work work;
    return deep_copy_level(work, 0);
}

inline void class_instance::finish_deep_copy_level(class_instance& copy, bool overflow) const {
    if (overflow) {
        // the copies landed in overflow (no definition yet, matching the old no-enforcement order); adopt it, then slot them
        copy.class_def_ = class_def_;
        copy.rehome_overflow_fields();
    }
    // The copied C++ object is a new identity (its registered copy function, else the value's own copy); the copy registers
    // as its script layer once the definition is attached
    if (const script_value* held = native()) {
        const script_value& value = *held;
        bool copied = false;
        if (!value.is_null() && class_def_ && class_def_->has_copy_function()) {
            if (auto cpp_obj = extract_cpp_object_impl(value)) {
                auto new_cpp_obj = class_def_->copy_object(cpp_obj.get());
                if (!new_cpp_obj) {
                    throw runtime_error("Failed to copy C++ object of type '" + std::string(get_class_name()) +
                        "'. The registered copy function returned null.");
                }
                copy.set_native(script_value::make_cpp_object(get_class_name(), class_def_->get_type_id(), new_cpp_obj, owner_));
                copied = true;
                if (owner_ && detail::engine_access::parallel_region_active(*owner_)) [[unlikely]] { copy.region_tag(detail::engine_access::parallel_region_serial(*owner_)); }
            }
        }
        if (!copied) { copy.set_native(value.clone_element()); }
    }
    if (class_def_) {
        class_def_->register_live_instance(&copy);
    }
}

inline strong_ptr<class_instance> class_instance::deep_copy_level(detail::deep_copy_work& work, unsigned depth) const {
    if (class_def_ && class_def_->nocopy()) {
        throw runtime_error("Cannot copy an instance of nocopy class '" + std::string(get_class_name()) + "': hold it by reference (auto&/var&) or share it (shared_ptr)");
    }
    if (class_def_ && dispatch_phase_ == cdtor_phase::none) {
        if (const class_definition* level = class_def_->copy_constructor_level()) { return detail::copy_construct_instance(*this, level); }
    }
    auto new_instance = class_def_ ? class_def_->allocate_instance() : class_instance::make(type_id_, owner_);
#ifdef JAISCRIPT_VM_COVERAGE
    census::add_weight(flat_fields_.size());
#endif
    const bool flat = class_def_ && !overflow_fields();
    const bool stepped = depth > detail::k_inline_copy_depth;
    size_t first_step = work.size();
    if (stepped) {
        work.push_back({flat ? detail::deep_copy_item::step::finish : detail::deep_copy_item::step::finish_overflow, this, new_instance.get()});
        first_step = work.size();
    }
    if (flat) {
        // Slot-to-slot copy with the definition attached up front: the general path
        // below routes every field through the overflow map and rehomes it (one map
        // node + one cell per field); flat instances copy their cells directly. Cells
        // clone through references exactly as set_field(id, value.clone()) did.
        // a copy taken while a base answers for the object is that base's object: its slots only
        const size_t slots = dispatch_phase_ == cdtor_phase::none ? flat_fields_.size()
                                                                  : (std::min)(flat_fields_.size(), static_cast<size_t>(class_def_->field_slot_count()));
        new_instance->class_def_ = class_def_;
        const bool implicit = new_instance->flat_fields_.implicit() && new_instance->flat_fields_.size() >= slots;
        if (!implicit) {
            new_instance->reserve_cell_arena(slots);
            new_instance->flat_fields_.resize(slots);
        }
        for (size_t slot = 0; slot < slots; ++slot) {
            const script_value* cell = flat_fields_.present_cell(slot);
            if (!cell) { continue; }
            const size_t kind = cell->raw_storage_index();
            const bool trivial = kind == script_value::TYPEID_INT || kind == script_value::TYPEID_FLOAT || kind == script_value::TYPEID_BOOL ||
                                 kind == script_value::TYPEID_CHAR || kind == script_value::TYPEID_NULL;
            if (implicit) {   // the copy's own slot cell, in its block
                script_value* target = new_instance->flat_fields_.cell(slot);
                if (trivial || cell->element_copies_shallow()) { *target = *cell; continue; }
                *target = script_value(std::monostate{}, owner_);
                cell->clone_element_into(*target, work, depth);
                continue;
            }
            if (trivial) {
                new_instance->flat_fields_[slot] = new_instance->make_trivial_cell(*cell);
                continue;
            }
            if (cell->element_copies_shallow()) {
                new_instance->flat_fields_[slot] = new_instance->make_cell(script_value(*cell));
                continue;
            }
            new_instance->flat_fields_[slot] = new_instance->make_cell(script_value(std::monostate{}, owner_));
            cell->clone_element_into(*new_instance->flat_fields_[slot], work, depth);
        }
    } else {
        for_each_field([&](uint64_t id, const script_value& value) {
            value.clone_element_into(new_instance->materialize_field(id, script_value(std::monostate{}, owner_)), work, depth);
        });
    }
    if (stepped) {
        std::reverse(work.begin() + static_cast<ptrdiff_t>(first_step), work.end());
    } else {
        finish_deep_copy_level(*new_instance, !flat);
    }
    return new_instance;
}

inline const script_value& class_instance::get_field(uint64_t id, bool throw_if_missing) const {
    if (const script_value* v = find_field_value(id)) {
        return *v;
    }

    if (class_def_) {
        auto& field_defaults = class_def_->get_all_field_defaults();
        auto default_it = field_defaults.find(id);
        if (default_it != field_defaults.end()) {
            return default_it->second;
        }

        for (const auto& parent : class_def_->get_parent_classes()) {
            if (parent) {
                auto& parent_defaults = parent->get_all_field_defaults();
                auto parent_it = parent_defaults.find(id);
                if (parent_it != parent_defaults.end()) {
                    return parent_it->second;
                }
            }
        }
    }

    if (throw_if_missing) {
        detail::throw_field_missing(class_def_, id);
    }

    auto& missing = const_cast<class_instance*>(this)->extras().missing_field;
    missing = script_value::make_invalid(class_def_ ? class_def_->get_engine() : nullptr);
    return *missing;
}

inline script_value& class_instance::get_field(uint64_t id, bool throw_if_missing) {
    if (script_value* v = find_field_value(id)) {
        return *v;
    }

    if (class_def_) {
        auto& field_defaults = class_def_->get_all_field_defaults();
        auto default_it = field_defaults.find(id);
        if (default_it != field_defaults.end()) {
            return materialize_field(id, default_it->second.clone());
        }

        for (const auto& parent : class_def_->get_parent_classes()) {
            if (parent) {
                auto& parent_defaults = parent->get_all_field_defaults();
                auto parent_it = parent_defaults.find(id);
                if (parent_it != parent_defaults.end()) {
                    return materialize_field(id, parent_it->second.clone());
                }
            }
        }

        auto cpp_base = class_def_->find_cpp_base_class();
        if (cpp_base) {
            auto& cpp_defaults = cpp_base->get_all_field_defaults();
            auto cpp_it = cpp_defaults.find(id);
            if (cpp_it != cpp_defaults.end()) {
                return materialize_field(id, cpp_it->second.clone());
            }
        }
    }

    if (throw_if_missing) {
        detail::throw_field_missing(class_def_, id);
    }

    auto& missing = const_cast<class_instance*>(this)->extras().missing_field;
    missing = script_value::make_invalid(class_def_ ? class_def_->get_engine() : nullptr);
    return *missing;
}

inline bool class_instance::has_field(uint64_t id) const {
    if (find_field_value(id) != nullptr) {
        return true;
    }
    return class_def_ && class_def_->chain_declares_field(id);
}

inline script_value class_instance::get_method(uint64_t id, bool throw_if_missing) const {
    if (class_def_) {
        return class_def_->get_method(id, throw_if_missing);
    }

    if (throw_if_missing) {
        detail::throw_no_class_definition(id);
    }
    return script_value::make_invalid(nullptr);
}

inline class_instance::class_instance(std::string_view class_name, engine* eng)
    : object_holder(k_instance, eng) {
    if (eng) { type_id_ = detail::engine_access::get_symbolizer(*eng)->intern(class_name); }
}

inline class_instance::~class_instance() {
    if (late_destructor_ && class_def_) {
        if (!construction_pending_) {
            class_def_->destroy_chain(this, class_def_);
        } else if (const class_definition* constructed = constructed_level()) {
            class_def_->destroy_levels(this, class_def_, nullptr, constructed);
        }
    }
    // an instance the engine's teardown orphaned (class_def_ cleared) outlives its engine: no region can be running
    if (extras_ && extras_->native && class_def_ && owner_ && detail::engine_access::parallel_region_active(*owner_)) [[unlikely]] { defer_serial_native(); }
    if (class_def_) {
        class_def_->unregister_instance(this);
        if (fields_cascade()) { release_fields(); }
    } else if (!destructor_armed_ && fields_cascade()) {
        release_orphan_fields();
    }
    while (!flat_fields_.empty()) { flat_fields_.pop_back(); }   // members die in reverse declaration order (C++)
}

} // namespace jai

#include <jaiscript/core/script_class.hpp>
