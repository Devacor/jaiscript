#include <charconv>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>
#include <jaiscript/detail/execution_limits.hpp>
#include <jaiscript/detail/member_scope.hpp>
#include <jaiscript/detail/string_element.hpp>
#include <jaiscript/detail/math_values.hpp>
#include <jaiscript/detail/float_text.hpp>
#include <jaiscript/detail/value_text.hpp>
#include <jaiscript/detail/object_borrow.hpp>
#include <jaiscript/detail/container_enforce.hpp>
#include <jaiscript/detail/arity_diagnostic.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <random>
#include <optional>
#include <thread>
#ifdef JAISCRIPT_VM_COVERAGE
#include <jaiscript/detail/copy_census.hpp>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

// The library's configuration (strong_ptr.hpp): a host compiled with another one names a symbol this does not define
const int jai::detail::JAI_BUILD_CONFIG = 1;

namespace jai {

[[noreturn]] JAI_NOINLINE void throw_runtime_error(std::string_view message) {
    throw runtime_error(std::string(message));
}

[[noreturn]] JAI_NOINLINE void throw_numeric_error(std::string_view message, uint64_t id1, uint64_t id2) {
    throw runtime_error(format_error_message_numeric(message, id1, id2));
}

[[noreturn]] JAI_NOINLINE void throw_checked_error(engine* eng, std::string_view message, uint64_t id1, uint64_t id2) {
    if (eng && message == "{0}") { throw runtime_error(std::string(detail::engine_access::get_symbolizer(*eng)->get_string(id1))); }
    throw runtime_error(format_error_message_numeric(message, id1, id2));
}

namespace detail {

namespace {
    uint64_t draw_process_hash_seed() {
        std::random_device device;
        uint64_t seed = (static_cast<uint64_t>(device()) << 32) ^ device();
        seed ^= static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) * 0x9E3779B97F4A7C15ull;
        return seed ^ reinterpret_cast<uintptr_t>(&seed);
    }
}

extern const uint64_t k_process_hash_seed = draw_process_hash_seed();

namespace {
    constexpr std::string_view k_conversion_targets[] = {
        "int", "float", "bool", "char", "string", "array", "map", "object", "script class instance", "weak_ptr", "function",
        "math::vec2", "math::vec3", "math::vec4", "math::color", "math::ivec2", "math::ivec3"};
    constexpr size_t k_conversion_sources = static_cast<size_t>(script_value_type::jai_color_type) + 1;
    struct conversion_text {
        char bytes[64]{};
        size_t size = 0;
    };
    // Every builtin source spelling against every target, built at compile time: a conversion error of a value with no class name
    // needs no engine to name it
    constexpr auto k_conversion_texts = [] {
        std::array<std::array<conversion_text, std::size(k_conversion_targets)>, k_conversion_sources> result{};
        for (size_t source = 0; source < k_conversion_sources; ++source) {
            for (size_t target = 0; target < std::size(k_conversion_targets); ++target) {
                conversion_text& text = result[source][target];
                for (std::string_view part : {std::string_view("Cannot convert "), base_type_spelling(static_cast<script_value_type>(source)),
                                              std::string_view(" to "), k_conversion_targets[target]}) {
                    for (char c : part) { text.bytes[text.size++] = c; }
                }
            }
        }
        return result;
    }();

    size_t conversion_target_index(const char* target) {
        for (size_t i = 0; i < std::size(k_conversion_targets); ++i) {
            if (k_conversion_targets[i] == target) { return i; }
        }
        return 0;
    }

    std::string_view builtin_conversion_text(script_value_type source, const char* target) {
        const auto& text = k_conversion_texts[static_cast<size_t>(source) < k_conversion_sources ? static_cast<size_t>(source) : 0][conversion_target_index(target)];
        return std::string_view(text.bytes, text.size);
    }
}

[[noreturn]] JAI_NOINLINE void throw_conversion_error(const script_value& value, const char* target) {
    throw runtime_error("Cannot convert " + container_value_type_name(value) + " to " + target);
}

// A value whose type has a class name (or a typed container's spelling) parks its text in the error's {0}; a builtin type's text is static
JAI_NOINLINE error_propagator conversion_error(const script_value& value, const char* target, runtime_error_code code) {
    const script_value& held = value.deref();
    const std::string name = container_value_type_name(held);
    const std::string_view builtin = builtin_conversion_text(held.current_type(), target);
    engine* eng = held.payload_engine();
    if (name != base_type_spelling(held.current_type()) && eng) {
        if (const uint64_t id = engine_access::park_error_text(*eng, "Cannot convert " + name + " to " + target)) {
            return error_propagator{make_error_code(code), "{0}", id, 0};
        }
    }
    return error_propagator{make_error_code(code), builtin, 0, 0};
}

} // namespace detail

[[noreturn]] JAI_NOINLINE void throw_binder_arity(const char* prefix, size_t expected, size_t got) {
    throw runtime_error(prefix + detail::arity_detail(expected, got));
}

[[noreturn]] JAI_NOINLINE void throw_text_with_count(const char* text, size_t count) {
    throw runtime_error(text + std::to_string(count));
}

[[noreturn]] JAI_NOINLINE void throw_concat(std::string_view a, std::string_view b, std::string_view c) {
    std::string text;
    text.reserve(a.size() + b.size() + c.size());
    text.append(a).append(b).append(c);
    throw runtime_error(text);
}

script_value::script_value(script_int i, engine* eng) : storage_(i) {
    if (eng) {
        set_stamp(detail::engine_access::get_type_info_int(*eng));
    }
}

script_value::script_value(script_float f, engine* eng) : storage_(f) {
    if (eng) {
        set_stamp(detail::engine_access::get_type_info_float(*eng));
    }
}

script_value::script_value(const script_string& s, engine* eng) : storage_(make_strong<script_string>(s)) {
    if (eng) {
        set_stamp(detail::engine_access::get_type_info_string(*eng));
    }
}

script_value::script_value(script_string&& s, engine* eng) : storage_(make_strong<script_string>(std::move(s))) {
    if (eng) {
        set_stamp(detail::engine_access::get_type_info_string(*eng));
    }
}

script_value::script_value(strong_ptr<object_holder> handle, type_info_ptr tag, engine*) : storage_(std::move(handle)) {
    if (tag) { set_stamp(tag); }
}

script_value script_value::make_utf8(std::string bytes, engine* eng) {
    script_value v(std::monostate{}, eng);
    v.storage_ = make_strong<script_utf8>(std::move(bytes));
    v.set_stamp(eng ? detail::engine_access::get_type_info_utf8_string(*eng) : nullptr);
    return v;
}

script_value::script_value(strong_ptr<script_string> shared, engine* eng) : storage_(std::move(shared)) {
    if (eng) {
        set_stamp(detail::engine_access::get_type_info_string(*eng));
    }
}

script_value::script_value(const char* s, engine* eng) : storage_(make_strong<script_string>(s)) {
    if (eng) {
        set_stamp(detail::engine_access::get_type_info_string(*eng));
    }
}

script_value::script_value(script_char c, engine* eng) : storage_(c) {
    if (eng) {
        set_stamp(detail::engine_access::get_type_info_char(*eng));
    }
}

script_value::script_value(script_bool b, engine* eng) : storage_(b) {
    if (eng) {
        set_stamp(detail::engine_access::get_type_info_bool(*eng));
    }
}

bool script_value::math_truthy() const noexcept {
    const script_vec_node& n = unchecked_vec_node();
    for (uint8_t k = 0; k < n.count; ++k) {
        if (n.integral) { if (n.words[k] != 0) { return true; } continue; }
        float f;
        std::memcpy(&f, &n.words[k], 4);
        if (f != 0.0f) { return true; }
    }
    return false;
}

script_value script_value::make_vec(script_value_type kind, const uint32_t* words, engine* eng) {
    auto node = acquire_vec_node(eng);
    switch (kind) {
        case script_value_type::jai_vec2_type: node->count = 2; break;
        case script_value_type::jai_vec3_type: node->count = 3; break;
        case script_value_type::jai_vec4_type: node->count = 4; break;
        case script_value_type::jai_ivec2_type: node->count = 2; node->integral = 1; break;
        case script_value_type::jai_color_type: node->count = 4; node->color = 1; break;
        default: node->count = 3; node->integral = 1; break;
    }
    std::memcpy(node->words, words, sizeof(uint32_t) * node->count);
    script_value v(std::monostate{}, eng);
    v.storage_ = std::move(node);
    if (eng) { v.set_stamp(detail::engine_access::get_type_info_math(*eng, kind)); }
    return v;
}

bool script_value::math_words_as(script_value_type kind, uint32_t* words) const {
    const script_value* v = &deref();
    script_value decoded(std::monostate{}, nullptr);
    if (v->raw_storage_index() == TYPEID_CPP_BOUND) {
        decoded = v->bound_decoded_temp();
        v = &decoded;
    }
    if (!is_math_index(v->raw_storage_index())) { return false; }
    const script_value_type from = v->current_type();
    if (from == kind) {
        std::memcpy(words, v->unchecked_vec_node().words, sizeof(script_vec_node::words));
        return true;
    }
    if (!detail::math::converts(from, kind)) { return false; }
    auto converted = detail::math::convert(*v, kind, nullptr);
    if (!converted) { return false; }
    std::memcpy(words, converted.value().unchecked_vec_node().words, sizeof(script_vec_node::words));
    return true;
}

namespace {
    template <typename M> script_value vec_from(const M& m, script_value_type kind, engine* eng) {
        uint32_t words[4] = {0, 0, 0, 0};
        std::memcpy(words, &m, sizeof(M));
        return script_value::make_vec(kind, words, eng);
    }
}

script_value::script_value(const vec2& v, engine* eng) : script_value(vec_from(v, script_value_type::jai_vec2_type, eng)) {}
script_value::script_value(const vec3& v, engine* eng) : script_value(vec_from(v, script_value_type::jai_vec3_type, eng)) {}
script_value::script_value(const vec4& v, engine* eng) : script_value(vec_from(v, script_value_type::jai_vec4_type, eng)) {}
script_value::script_value(const ivec2& v, engine* eng) : script_value(vec_from(v, script_value_type::jai_ivec2_type, eng)) {}
script_value::script_value(const ivec3& v, engine* eng) : script_value(vec_from(v, script_value_type::jai_ivec3_type, eng)) {}
script_value::script_value(const color& v, engine* eng) : script_value(vec_from(v, script_value_type::jai_color_type, eng)) {}


script_value::reference_holder::~reference_holder() {
    if (container && container->live_elem_refs) { --container->live_elem_refs; }
    if (elem_root && elem_root->live_elem_refs) { --elem_root->live_elem_refs; }
    if (owner_instance) { owner_instance->note_field_ref_released(); }
    if (has_cell || has_map_key || has_elem_scratch ||
        (property && (property->live_member || property->has_scratch))) {
        cell()->~script_value();
        has_cell = false;
        has_map_key = false;
        has_elem_scratch = false;
    }
}

const script_value& script_value::reference_holder::materialize_typed_element(engine* eng) {
    if (has_elem_scratch && container->view_current(container_index, *cell())) {
        return *cell();   // the slot still holds this handle: no refcount round trip per touch
    }
    script_value v = container->get(container_index, eng);
    if (has_elem_scratch) {
        *cell() = std::move(v);
    } else {
        new (cell_storage) script_value(std::move(v));
        has_elem_scratch = true;
    }
    return *cell();
}

const script_value& script_value::reference_holder::materialize_string_element(engine* eng) {
    script_value pin = make_string_element_pin(*this, eng);
    script_value* storage = &pin.deref();
    while (storage->is_reference()) { storage = &storage->deref(); }
    script_value v(std::monostate{}, eng);
    if (storage->raw_storage_index() == TYPEID_UTF8_STRING) {
        const bool cache = !detail::engine_access::parallel_region_active(*eng);
        if (container_index >= detail::utf8::length(storage->unchecked_as_utf8(), cache)) {
            throw runtime_error("Reference to a removed string character");
        }
        v = detail::utf8::char_value(storage->unchecked_as_utf8(), container_index, eng, cache);
    } else if (storage->is_string()) {
        const script_string& bytes = storage->raw_storage_index() == TYPEID_STRING ? storage->unchecked_as_string()
                                                                                    : storage->unchecked_as_string_ref(eng);
        if (container_index >= bytes.size()) {
            throw runtime_error("Reference to a removed string character");
        }
        v = script_value(static_cast<script_char>(bytes[container_index]), eng);
    } else {
        throw runtime_error("Reference to a removed string character");
    }
    if (has_elem_scratch) {
        *cell() = std::move(v);
    } else {
        new (cell_storage) script_value(std::move(v));
        has_elem_scratch = true;
    }
    return *cell();
}

const script_value& script_value::reference_holder::materialize_component(engine* eng) {
    script_value pin = make_component_pin(*this, eng);
    script_value* storage = &pin.deref();
    while (storage->is_reference()) { storage = &storage->deref(); }
    if (!detail::math::is_kind(storage->raw_storage_index())) {
        throw runtime_error("Reference to a component of a value that is no longer a vector");
    }
    script_value v = detail::math::component_value(*storage, container_index, eng);
    if (has_elem_scratch) {
        *cell() = std::move(v);
    } else {
        new (cell_storage) script_value(std::move(v));
        has_elem_scratch = true;
    }
    return *cell();
}

void script_value::reference_holder::store_component(const script_value& value, engine* eng) {
    script_value pin = make_component_pin(*this, eng);
    script_value* storage = &pin.deref();
    while (storage->is_reference()) { storage = &storage->deref(); }
    if (!detail::math::is_kind(storage->raw_storage_index())) {
        throw runtime_error("Reference to a component of a value that is no longer a vector");
    }
    const script_value& rhs = value.deref();
    auto stored = rhs.raw_storage_index() == TYPEID_CPP_BOUND
        ? detail::math::store_component(*storage, static_cast<int>(container_index), rhs.bound_decoded_temp(), eng)
        : detail::math::store_component(*storage, static_cast<int>(container_index), rhs, eng);
    if (!stored) {
        throw runtime_error(format_error(stored, *detail::engine_access::get_symbolizer(*eng)));
    }
    if (auto* sr = pin.scratch_ref_holder()) { sr->commit_scratch(); }
}

// PROPERTY mode touches re-resolve the receiver's class and accessor every time (a
// reload that adds a same-named script field re-points the reference at that field; a
// retired layer reports). Touch points throw like the other modes do; bind points take
// the checked twin.
checked_result<script_value> script_value::reference_holder::read_property(engine* eng, bool bind_point) const {
    script_value receiver(property->receiver, property->receiver_tag, eng);
    auto site = detail::resolve_cpp_property(receiver, property->field_id, eng);
    if (!site) {
        if (site.view.instance) {
            if (const script_value* field = site.view.instance->find_field_value(property->field_id)) {
                return script_value(field->deref());
            }
        }
        return checked_result<script_value>(make_error_code(runtime_error_code::invalid_reference), "Reference to a removed field");
    }
    return detail::read_cpp_property_site(receiver, site, bind_point);
}

const script_value& script_value::reference_holder::materialize_property(engine* eng) {
    auto read = read_property(eng, /*bind_point*/ false);
    if (!read) {
        throw runtime_error(format_error(read, *detail::engine_access::get_symbolizer(*eng)));
    }
    if (property->has_scratch) {
        *cell() = std::move(read.value());
    } else {
        new (cell_storage) script_value(std::move(read.value()));
        property->has_scratch = true;
    }
    return *cell();
}

void script_value::reference_holder::store_property(const script_value& value, engine* eng) {
    script_value receiver(property->receiver, property->receiver_tag, eng);
    auto site = detail::resolve_cpp_property(receiver, property->field_id, eng);
    if (!site) {
        if (site.view.instance) {
            if (script_value* field = site.view.instance->find_field_value(property->field_id)) {
                *field = value;
                return;
            }
        }
        throw runtime_error("Reference to a removed field");
    }
    if (site.view.instance && site.view.instance->script_layer_retired()) {
        site.view.instance->throw_retired_script_layer();
    }
    if (!site.setter_id) {   // unreachable for binder properties (a getter-only one registers the rejecting setter)
        throw runtime_error(format_error_message("Cannot assign to '{0}': it is read-only",
                                                 detail::engine_access::get_symbolizer(*eng)->get_string(property->field_id)));
    }
    auto stored = detail::write_cpp_property(receiver, site.view.def, site.setter_id, value);
    if (!stored) {
        throw runtime_error(format_error(stored, *detail::engine_access::get_symbolizer(*eng)));
    }
}

void script_value::reference_holder::commit_scratch() {
    if (has_elem_scratch && container && container_index < container->size()) {
        container->set(container_index, *cell());
        return;
    }
    if (property && property->has_scratch) {
        store_property(*cell(), owner);
        return;
    }
    if (string_base && has_elem_scratch) {
        engine* eng = owner;
        auto stored = detail::store_string_element(*this, *cell(), eng);
        if (!stored) {
            throw runtime_error(format_error(stored, *detail::engine_access::get_symbolizer(*eng)));
        }
        return;
    }
    if (component_base && has_elem_scratch) {
        store_component(*cell(), owner);
    }
}

script_value* script_value::reference_holder::map_entry_value() const {
    script_map::entry_memo memo{container_map.get(), map_memo_gen, map_memo_entry};
    auto* kv = container_map->recall(memo, *cell());
    map_memo_gen = memo.gen;
    map_memo_entry = memo.e;
    return kv ? &kv->second : nullptr;
}

script_value* script_value::reference_holder::resolve_target() {
    if (has_cell) {
        return cell();
    }
    if (has_map_key) {
        return map_entry_value();
    }
    if (container) {
        if (container->is_typed()) {
            // no script_value element exists - callers pre-check typed_element() and
            // take the scratch+commit path; null here means a missed caller fails
            // loudly ("Invalid reference") instead of corrupting memory
            return nullptr;
        }
        return container_index < container->size() ? &container->values()[container_index] : nullptr;
    }
    if (owner_instance) {
        return field_cell();
    }
    return nullptr;   // CPP-MEMBER / PROPERTY (no addressable cell); else unreachable: every factory sets a mode
}

script_value* script_value::reference_holder::field_cell() const {
    class_instance* inst = owner_instance;
    const class_definition* cd = inst->get_class_definition();
    if (cd && cd == field_class && cd->field_shape_epoch() == field_class_epoch) {
        return inst->field_slot_value(field_slot);
    }
    // find_field_value's ladder with the slot kept: a slot field's cell (present or gone), else the overflow fields
    const uint32_t slot = cd ? cd->field_slot(field_id) : class_definition::k_no_field_slot;
    if (slot == class_definition::k_no_field_slot) { return inst->find_field_value(field_id); }
    script_value* found = inst->field_slot_value(slot);
    if (found) {
        engine* eng = inst->get_engine();
        if (!eng || !detail::engine_access::parallel_region_active(*eng)) {
            field_class = cd;
            field_class_epoch = cd->field_shape_epoch();
            field_slot = slot;
        }
    }
    return found;
}

namespace detail {

// An engine's free list of control blocks for one holder type: a mint is a pop + placement new instead of a heap
// allocation. Blocks self-describe their release through dealloc_fn, so plain make_strong blocks and pooled ones
// coexist. The free list is owner-thread only: a parallel region's workers mint plain blocks, and a block released off
// the owner thread is deleted and counted as a foreign return. Checked-out blocks are counted in two parts, owned
// (acquires minus owner-thread returns, touched only by the owner thread while the engine lives) and foreign; the
// engine's teardown orphans the pool, and an orphaned pool frees itself when the two counts meet (owned is stable once
// orphaned is published, so other threads read it after it).
template<typename T, size_t MaxFree>
struct holder_block_pool {
    struct block : control_block<T> {
        holder_block_pool* home = nullptr;
    };
    static_assert(offsetof(block, storage) == cb_storage_offset<T>,
                  "pooled blocks must keep storage at control_block<T>'s head offset (cb_from_object)");

    std::vector<block*> free_blocks;
    size_t owned = 0;
    std::atomic<size_t> foreign{0};
    std::atomic<bool> orphaned{false};
    std::atomic<bool> freed{false};
    std::thread::id owner = std::this_thread::get_id();

    // return_block runs inside ~strong_ptr (noexcept): the free-list push must never allocate
    holder_block_pool() { free_blocks.reserve(MaxFree); }

    void free_if_drained() {
        if (owned == foreign.load(std::memory_order_acquire)) {
            bool expected = false;
            if (freed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) { delete this; }
        }
    }

    static void return_block(control_block_base* base) {
        block* b = static_cast<block*>(static_cast<control_block<T>*>(base));
        holder_block_pool* pool = b->home;
#ifndef NDEBUG
        base->magic = cb_magic_dead;   // scrambled parked OR freed (use-after-free canary)
#endif
        if (!pool->orphaned.load(std::memory_order_acquire) && std::this_thread::get_id() == pool->owner) {
            --pool->owned;
            if (pool->free_blocks.size() < MaxFree) {
                pool->free_blocks.push_back(b);
            } else {
                delete b;
            }
            return;
        }
        delete b;
        pool->foreign.fetch_add(1, std::memory_order_acq_rel);
        if (pool->orphaned.load(std::memory_order_acquire)) {
            pool->free_if_drained();
        }
    }

    template <class... A>
    strong_ptr<T> acquire(A&&... args) {
        if (free_blocks.empty()) [[unlikely]] { return acquire_fresh(std::forward<A>(args)...); }
        block* b = free_blocks.back();
        free_blocks.pop_back();
        b->strong_count = 1;
        b->weak_count = 1;
#ifndef NDEBUG
        b->magic = cb_magic_live;
#endif
        ++owned;
        return adopt_pooled(new (b->storage) T(std::forward<A>(args)...));
    }

    template <class... A>
    JAI_NOINLINE strong_ptr<T> acquire_fresh(A&&... args) {
        block* b = new block();
        b->home = this;
        b->dealloc_fn = &return_block;
        ++owned;
        return adopt_pooled(new (b->storage) T(std::forward<A>(args)...));
    }

    template <class... A>
    JAI_NOINLINE static strong_ptr<T> acquire_plain(A&&... args) { return make_strong<T>(std::forward<A>(args)...); }

    static void teardown(void* p) {
        auto* pool = static_cast<holder_block_pool*>(p);
        for (block* b : pool->free_blocks) {
            delete b;
        }
        pool->free_blocks.clear();
        pool->orphaned.store(true, std::memory_order_release);
        pool->free_if_drained();
    }

    JAI_NOINLINE static void install(engine* eng, void*& slot, void (*set_teardown)(engine&, void (*)(void*))) {
        slot = new holder_block_pool();
        set_teardown(*eng, &teardown);
    }

    template <class... A>
    static strong_ptr<T> acquire_from(engine* eng, void*& slot, void (*set_teardown)(engine&, void (*)(void*)), A&&... args) {
        if (!slot) [[unlikely]] { install(eng, slot, set_teardown); }
        return static_cast<holder_block_pool*>(slot)->acquire(std::forward<A>(args)...);
    }
};

}

namespace {
template <typename Handle>
engine* owner_of(const Handle& a_handle) noexcept {
    if (!a_handle) { return nullptr; }
    if constexpr (requires { a_handle->owner(); }) { return a_handle->owner(); } else { return a_handle->owner; }
}
// engine::memory_cap: a copy's fresh storage counts toward its engine's allowance (raised at the next loop back-edge)
void charge_deferred(engine* a_engine, size_t a_bytes) {
    if (a_engine) { detail::engine_access::execution_limits(*a_engine).memory_charge_deferred(a_bytes); }
}
}

script_value script_value::tagged_null() const noexcept {
    script_value result(std::monostate{}, nullptr);
    result.storage_.set_tag(storage_.tag());
    return result;
}

engine* script_value::payload_engine() const noexcept {
    switch (raw_storage_index()) {
        case TYPEID_ARRAY: return owner_of(storage_.get<strong_ptr<script_array>>());
        case TYPEID_MAP: return owner_of(storage_.get<strong_ptr<script_map>>());
        case TYPEID_OBJECT: return owner_of(storage_.get<strong_ptr<object_holder>>());
        case TYPEID_REFERENCE: return owner_of(storage_.get<strong_ptr<reference_holder>>());
        case TYPEID_CPP_BOUND: return owner_of(storage_.get<strong_ptr<cpp_bound_holder>>());
        case TYPEID_FUNCTION: return owner_of(storage_.get<strong_ptr<script_function_node>>());
        case TYPEID_PARALLEL_BORROW: {
            if (const script_array* borrowed = parallel_borrow_array()) { return borrowed->owner; }
            if (const auto* record = parallel_object_borrow()) { return record->eng; }
            const script_map* borrowed = parallel_borrow_map();
            return borrowed ? borrowed->owner : nullptr;
        }
        default: return nullptr;
    }
}

strong_ptr<script_value::reference_holder> script_value::acquire_reference_holder(engine* eng) {
    auto result = detail::engine_access::parallel_region_active(*eng) ? jai::detail::holder_block_pool<reference_holder, 256>::acquire_plain()
        : jai::detail::holder_block_pool<reference_holder, 256>::acquire_from(eng, detail::engine_access::reference_holder_pool_slot(*eng), &detail::engine_access::set_reference_holder_pool_teardown);
    result->owner = eng;
    return result;
}

static_assert(jai::detail::cb_storage_offset<script_value::object_holder> == jai::detail::cb_storage_offset<script_value::host_holder> &&
              jai::detail::cb_storage_offset<script_value::object_holder> == jai::detail::cb_storage_offset<class_instance>,
              "an object value's handle finds its holder's control block where the holder's own family put it");

strong_ptr<script_value::object_holder> script_value::acquire_object_holder(engine* eng) {
    auto result = !eng || detail::engine_access::parallel_region_active(*eng) ? jai::detail::holder_block_pool<host_holder, 1024>::acquire_plain()
        : jai::detail::holder_block_pool<host_holder, 1024>::acquire_from(eng, detail::engine_access::object_holder_pool_slot(*eng), &detail::engine_access::set_object_holder_pool_teardown);
    result->set_owner(eng);
    return jai::detail::adopt_pooled<object_holder>(static_cast<object_holder*>(result.release()));
}

strong_ptr<script_value::object_holder> script_value::instance_storage(strong_ptr<class_instance> instance) noexcept {
    class_instance* raw = instance.release();
    return raw ? jai::detail::adopt_pooled<object_holder>(static_cast<object_holder*>(raw)) : strong_ptr<object_holder>();
}

void script_value::object_holder::last_release(object_holder* holder, jai::detail::control_block_base* cb) noexcept {
    if (holder->is_instance()) {
        detail::release_last_instance(holder->unchecked_instance());
        return;
    }
    static_cast<host_holder*>(holder)->~host_holder();
    if (cb->drop_weak() == 0 && cb->strong_count == 0) { cb->dealloc_fn(cb); }
}

strong_ptr<script_vec_node> script_value::acquire_vec_node(engine* eng) {
    if (!eng || detail::engine_access::parallel_region_active(*eng)) [[unlikely]] {
        return jai::detail::holder_block_pool<script_vec_node, 1024>::acquire_plain();
    }
    return jai::detail::holder_block_pool<script_vec_node, 1024>::acquire_from(eng, detail::engine_access::vec_node_pool_slot(*eng),
        &detail::engine_access::set_vec_node_pool_teardown);
}

void script_value::unshare_vec_node() {
    strong_ptr<script_vec_node>& node = storage_.vec_node();
    auto fresh = acquire_vec_node(nullptr);
    *fresh = *node;
    node = std::move(fresh);
}

size_t script_value::map_entry_reference_charge() noexcept { return sizeof(reference_holder); }

script_value script_value::make_cell_reference(script_value&& boxed, engine* eng) {
    static_assert(sizeof(script_value) == reference_holder::cell_storage_size &&
                  alignof(script_value) <= 8, "cell inline storage must fit a script_value");
    if (!eng) {
        throw runtime_error("Cannot create reference: null engine pointer");
    }
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_reference(*eng, boxed.get_type_info()));
    // engine::memory_cap: cells are heap boxes (raised at the next loop back-edge)
    detail::engine_access::execution_limits(*eng).memory_charge_deferred(sizeof(reference_holder));
    auto ref = acquire_reference_holder(eng);
    new (ref->cell_storage) script_value(std::move(boxed));
    ref->has_cell = true;
    v.storage_ = ref;
    return v;
}

struct script_value::cell_cycle_registry {
    std::vector<jai::weaker_ptr<reference_holder>> deferred;
    size_t sweep_at = 64;
    bool sweeping = false;
    static void teardown(void* p) { delete static_cast<cell_cycle_registry*>(p); }
};

bool script_value::is_shared_cell(const script_value& v) noexcept {
    if (v.raw_storage_index() != TYPEID_REFERENCE) return false;
    const auto& holder = *v.storage_.get_if<TYPEID_REFERENCE>();
    return holder && holder->plain_cell() && holder.use_count() > 1;
}

void script_value::gather_shared_cells(const script_value* values, size_t count, std::vector<script_value>& out) {
    for (size_t i = 0; i < count; ++i) {
        if (is_shared_cell(values[i])) out.push_back(values[i]);
    }
}

// release_cell_cycles when no cell holds a function: no edges leave a cell, so a cell is garbage exactly when the list
// holds every handle to it; the same verdicts, list removals and value releases as the general walk, without its graph
bool script_value::release_plain_cells(std::vector<script_value>& cells, engine* eng) {
    const size_t n = cells.size();
    for (const script_value& cell : cells) {
        const auto& holder = *cell.storage_.get_if<TYPEID_REFERENCE>();
        if (holder->plain_cell() && holder->cell()->raw_storage_index() == TYPEID_FUNCTION) return false;
    }
    bool garbage[16] = {};   // per entry: its cell's verdict (a cell listed twice has one)
    bool first[16] = {};
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        const reference_holder* holder = cells[i].storage_.get_if<TYPEID_REFERENCE>()->get();
        size_t at = i;
        size_t held = 0;
        for (size_t j = 0; j < n; ++j) {
            if (cells[j].storage_.get_if<TYPEID_REFERENCE>()->get() == holder) {
                if (j < at) { at = j; }
                ++held;
            }
        }
        first[i] = at == i;
        garbage[i] = at == i ? cells[i].storage_.get_if<TYPEID_REFERENCE>()->use_count() == held : garbage[at];
        any = any || garbage[i];
    }
    void*& slot = detail::engine_access::cell_cycle_registry_slot(*eng);
    if (any && slot) {
        auto& deferred = static_cast<cell_cycle_registry*>(slot)->deferred;
        for (size_t i = 0; i < n; ++i) {
            if (!garbage[i]) continue;
            const reference_holder* holder = cells[i].storage_.get_if<TYPEID_REFERENCE>()->get();
            const size_t floor = deferred.size() > 8 ? deferred.size() - 8 : 0;
            for (size_t k = deferred.size(); k-- > floor;) {
                if (deferred[k].owner_identity() == holder) {
                    deferred.erase(deferred.begin() + static_cast<std::ptrdiff_t>(k));
                    break;
                }
            }
        }
    }
    for (size_t i = 0; any && i < n; ++i) {
        if (!garbage[i] || !first[i]) continue;
        auto* holder = const_cast<reference_holder*>(cells[i].storage_.get_if<TYPEID_REFERENCE>()->get());
        if (holder->plain_cell()) { script_value dying = std::move(*holder->cell()); }
    }
    cells.clear();
    return true;
}

// Trial deletion over the cells, the function values a cell holds, their definitions and capture environments, and the
// cells and functions those environments hold. A node referenced from outside that set (a count above its edges from
// inside plus the handles in `cells`) is live, and so is everything it reaches; a cell left over is held only by the
// cycle, and clearing its value lets the cycle die.
void script_value::release_cell_cycles(std::vector<script_value>& cells, engine* eng) {
    if (cells.empty()) return;
    if (!eng || detail::engine_access::parallel_region_active(*eng)) { cells.clear(); return; }
    if (cells.size() <= 16 && release_plain_cells(cells, eng)) return;
    enum : uint8_t { node_cell, node_function, node_definition, node_environment };
    struct node { const void* key; size_t count; size_t internal; uint32_t held; uint32_t first_edge; uint32_t edge_count; uint8_t kind; bool live; };
    std::vector<node> nodes;
    nodes.reserve(cells.size() * 4);
    std::vector<uint32_t> edges;
    std::optional<std::unordered_map<const void*, uint32_t>> index;   // past a few dozen nodes
    auto find = [&](const void* key) -> uint32_t {
        if (index) {
            auto it = index->find(key);
            return it == index->end() ? UINT32_MAX : it->second;
        }
        for (uint32_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].key == key) return i;
        }
        return UINT32_MAX;
    };
    auto add = [&](const void* key, size_t count, uint8_t kind) -> uint32_t {
        const uint32_t id = static_cast<uint32_t>(nodes.size());
        nodes.push_back({key, count, 0, 0, 0, 0, kind, false});
        if (index) {
            index->emplace(key, id);
        } else if (nodes.size() > 32) {
            index.emplace();
            for (uint32_t i = 0; i < nodes.size(); ++i) index->emplace(nodes[i].key, i);
        }
        return id;
    };
    auto link = [&](const void* key, size_t count, uint8_t kind) {
        uint32_t id = find(key);
        if (id == UINT32_MAX) id = add(key, count, kind);
        ++nodes[id].internal;
        edges.push_back(id);
    };
    auto link_value = [&](const script_value& v) {
        if (v.raw_storage_index() == TYPEID_REFERENCE) {
            const auto& holder = *v.storage_.get_if<TYPEID_REFERENCE>();
            if (holder && holder->plain_cell()) link(holder.get(), holder.use_count(), node_cell);
        } else if (v.raw_storage_index() == TYPEID_FUNCTION) {
            const auto& fn = *v.storage_.get_if<TYPEID_FUNCTION>();
            if (fn) link(fn.get(), fn.use_count(), node_function);
        }
    };
    for (const script_value& cell : cells) {
        const auto& holder = *cell.storage_.get_if<TYPEID_REFERENCE>();
        uint32_t id = find(holder.get());
        if (id == UINT32_MAX) id = add(holder.get(), holder.use_count(), node_cell);
        ++nodes[id].held;
    }
    for (uint32_t i = 0; i < nodes.size(); ++i) {
        nodes[i].first_edge = static_cast<uint32_t>(edges.size());
        const void* key = nodes[i].key;
        switch (nodes[i].kind) {
            case node_cell: {
                auto* holder = static_cast<const reference_holder*>(key);
                if (holder->plain_cell() && holder->cell()->raw_storage_index() == TYPEID_FUNCTION) link_value(*holder->cell());
                break;
            }
            case node_function: {
                auto* fn = const_cast<script_function*>(static_cast<const script_function*>(key));
                const auto* thunk = fn->target<script_callable_thunk>();
                if (thunk && thunk->payload.kind == script_callable::kind_type::function && thunk->payload.fn) {
                    link(thunk->payload.fn.get(), thunk->payload.fn.use_count(), node_definition);
                }
                break;
            }
            case node_definition: {
                const auto* definition = static_cast<const script_defined_function*>(key);
                if (const auto& env = definition->closure_env) link(env.get(), env.use_count(), node_environment);
                if (definition->captures) {
                    for (const script_value& cell : definition->captures->cells) { link_value(cell); }
                    if (const auto& twin = definition->captures->environment_twin) link(twin.get(), twin.use_count(), node_definition);
                }
                break;
            }
            case node_environment: {
                for (const script_value& v : static_cast<const environment*>(key)->own_values()) link_value(v);
                break;
            }
        }
        nodes[i].edge_count = static_cast<uint32_t>(edges.size()) - nodes[i].first_edge;
    }
    std::vector<uint32_t> work;
    for (uint32_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].count != nodes[i].internal + nodes[i].held) {
            nodes[i].live = true;
            work.push_back(i);
        }
    }
    while (!work.empty()) {
        const uint32_t i = work.back();
        work.pop_back();
        for (uint32_t e = nodes[i].first_edge; e < nodes[i].first_edge + nodes[i].edge_count; ++e) {
            if (!nodes[edges[e]].live) {
                nodes[edges[e]].live = true;
                work.push_back(edges[e]);
            }
        }
    }
    std::vector<script_value> trash;
    for (const node& n : nodes) {
        if (n.kind == node_cell && !n.live) {
            auto* holder = const_cast<reference_holder*>(static_cast<const reference_holder*>(n.key));
            if (holder->plain_cell()) trash.push_back(std::move(*holder->cell()));
        }
    }
    // A live cell a closure holds is listed for a later check; a listed cell found garbage leaves the list (a callee
    // listed its caller's cell, the caller's exit clears it). Only the newest entries are searched.
    void*& slot = detail::engine_access::cell_cycle_registry_slot(*eng);
    auto listed_at = [&](const reference_holder* holder) -> size_t {
        auto& deferred = static_cast<cell_cycle_registry*>(slot)->deferred;
        const size_t floor = deferred.size() > 8 ? deferred.size() - 8 : 0;
        for (size_t i = deferred.size(); i-- > floor;) {
            if (deferred[i].owner_identity() == holder) return i;
        }
        return SIZE_MAX;
    };
    for (const script_value& cell : cells) {
        const auto& holder = *cell.storage_.get_if<TYPEID_REFERENCE>();
        const node& n = nodes[find(holder.get())];
        if (n.live && n.internal > 0) {
            if (!slot) {
                slot = new cell_cycle_registry();
                detail::engine_access::set_cell_cycle_registry_teardown(*eng, &cell_cycle_registry::teardown);
            }
            if (listed_at(holder.get()) == SIZE_MAX) static_cast<cell_cycle_registry*>(slot)->deferred.emplace_back(holder);
        } else if (slot && !n.live) {
            const size_t at = listed_at(holder.get());
            if (at != SIZE_MAX) {
                auto& deferred = static_cast<cell_cycle_registry*>(slot)->deferred;
                deferred.erase(deferred.begin() + static_cast<std::ptrdiff_t>(at));
            }
        }
    }
    trash.clear();
    cells.clear();
    auto* registry = static_cast<cell_cycle_registry*>(slot);
    if (registry && !registry->sweeping && registry->deferred.size() >= registry->sweep_at) {
        registry->sweeping = true;
        std::vector<script_value> roots;
        {
            auto deferred = std::move(registry->deferred);
            registry->deferred.clear();
            for (const auto& weak : deferred) {
                if (auto holder = weak.lock()) {
                    script_value v(std::monostate{}, eng);
                    v.storage_ = std::move(holder);
                    roots.push_back(std::move(v));
                }
            }
        }
        release_cell_cycles(roots, eng);
        registry->sweep_at = std::max<size_t>(64, registry->deferred.size() * 2);
        registry->sweeping = false;
    }
}

void script_value::release_deferred_cell_cycles(engine* eng) {
    void*& slot = detail::engine_access::cell_cycle_registry_slot(*eng);
    auto* registry = static_cast<cell_cycle_registry*>(slot);
    if (!registry) return;
    registry->sweeping = true;
    std::vector<script_value> roots;
    for (const auto& weak : registry->deferred) {
        if (auto holder = weak.lock()) {
            script_value v(std::monostate{}, eng);
            v.storage_ = std::move(holder);
            roots.push_back(std::move(v));
        }
    }
    registry->deferred.clear();
    release_cell_cycles(roots, eng);
    slot = nullptr;
    delete registry;
}

script_value script_value::make_map_entry_reference(const strong_ptr<script_map>& map_storage, const script_value& key,
                                                    const script_value* entry, engine* eng, type_info_ptr value_type) {
    if (!map_storage) {
        throw runtime_error("Cannot create reference to null map");
    }
    if (!eng) {
        throw runtime_error("Cannot create reference: null engine pointer");
    }
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_reference(*eng, entry ? entry->get_type_info() : nullptr));
    detail::engine_access::execution_limits(*eng).memory_charge_deferred(sizeof(reference_holder));
    auto ref = acquire_reference_holder(eng);
    new (ref->cell_storage) script_value(key.is_reference() ? key.deref() : key);
    ref->has_map_key = true;
    ref->container_map = map_storage;
    ref->container_element_type = value_type;
    if (entry) {
        // the entry the caller just found or inserted: its first touch does not run the key's operators again
        const script_map::entry_memo memo = map_storage->memo_at(*ref->cell(), entry);
        ref->map_memo_gen = memo.gen;
        ref->map_memo_entry = memo.e;
    }
    v.storage_ = ref;
    return v;
}

script_value script_value::make_array(type_info_ptr element_type, engine* eng) {
    if (!eng) {
        throw runtime_error("Cannot create array with null engine pointer");
    }
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_array(*eng, element_type));

    // The declared element type IS the representation: int/float element types mint
    // raw-buffer nodes, array<var> (null element type) the script_value vector. The
    // airtight tag is what makes this sound.
    script_array::kind_t node_kind = script_array::kind_t::hetero;
    if (element_type) {
        switch (element_type->base_type) {
            case script_value_type::jai_int_type:
                node_kind = !element_type->is_byte_element() ? script_array::kind_t::i64
                          : element_type->is_signed ? script_array::kind_t::i8 : script_array::kind_t::u8;
                break;
            case script_value_type::jai_float_type: node_kind = script_array::kind_t::f64; break;
            case script_value_type::jai_bool_type: node_kind = script_array::kind_t::b8; break;
            case script_value_type::jai_char_type: node_kind = script_array::kind_t::c8; break;
            case script_value_type::jai_string_type: node_kind = script_array::kind_t::str; break;
            case script_value_type::jai_object_type: node_kind = element_type->is_enum ? script_array::kind_t::hetero : script_array::kind_t::obj; break;
            default: node_kind = script_array::vec_kind_of(element_type->base_type); break;
        }
    }

    // Create node with small default capacity to avoid first few reallocations
    auto node = detail::engine_access::parallel_region_active(*eng) ? jai::detail::holder_block_pool<script_array, 1024>::acquire_plain(node_kind)
        : jai::detail::holder_block_pool<script_array, 1024>::acquire_from(eng, detail::engine_access::array_node_pool_slot(*eng), &detail::engine_access::set_array_node_pool_teardown, node_kind);
    node->owner = eng;
    node->reserve(8);
    v.storage_ = node;
    return v;
}

script_value script_value::make_map(type_info_ptr keyType, type_info_ptr valueType, engine* eng) {
    if (!eng) {
        throw runtime_error("Cannot create map with null engine pointer");
    }
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_map(*eng, keyType, valueType));
    v.storage_ = make_strong<script_map>(eng);
    return v;
}

namespace {
// a type's name as the engine's interned text: read by the interned id the caller already holds (an index, no hash), interning only
// when the id is missing or names other text
std::string_view interned_type_text(engine* eng, std::string_view name, uint64_t type_id) {
    auto* symbols = detail::engine_access::get_symbolizer(*eng);
    if (type_id != UINT64_MAX) {
        const std::string_view text = symbols->get_string(type_id);
        if (text == name) { return text; }
    }
    return symbols->intern_with_view(name).second;
}
}

script_value script_value::make_object(std::string_view type_name, detail::object_ref data, engine* eng) {
    if (!eng) {
        throw runtime_error("Cannot create object with null engine pointer");
    }

    uint64_t type_id = detail::engine_access::get_symbolizer(*eng)->intern(type_name);
    return make_object(type_name, type_id, std::move(data), eng);
}

script_value script_value::make_object(std::string_view type_name, uint64_t type_id, detail::object_ref data, engine* eng) {
    if (!eng) {
        throw runtime_error("Cannot create object with null engine pointer");
    }
    // the registry's class names the object: a type_info::make_object() temporary would die before the value
    auto class_def = detail::engine_access::get_class_definition(*eng, type_id);
    if (!class_def) {
        throw runtime_error("Cannot create object of unregistered class '" + std::string(type_name) +
            "'. Classes must be registered with engine.register_class() before instantiation.");
    }

    script_value v(std::monostate{}, eng);
    v.set_stamp(class_def->get_type_info());

    if (data.is_instance()) {
        strong_ptr<class_instance> instance = data.instance();
        if (instance->type_id() == UINT64_MAX) { instance->set_type(interned_type_text(eng, type_name, type_id), type_id); }
        v.storage_ = instance_storage(std::move(instance));
        return v;
    }
    auto obj = acquire_object_holder(eng);
    obj->set_type(interned_type_text(eng, type_name, type_id), type_id);
    obj->hold(std::move(data));
    v.storage_ = obj;
    return v;
}

namespace detail {
void release_last_instance(class_instance* instance) noexcept {
    JAI_SP_CENSUS_FREED(instance);
    strong_last_release<class_instance>::run(instance, cb_from_object(instance));
}

std::string_view instance_type_name(engine* eng, uint64_t id) noexcept {
    return eng && id != UINT64_MAX ? engine_access::get_symbolizer(*eng)->get_string(id) : std::string_view();
}

#ifndef NDEBUG
void check_instance_type_identity(engine* eng, std::string_view name, uint64_t id) noexcept {
    const bool unnamed = id == UINT64_MAX || !eng;
    assert((unnamed ? name.empty() : engine_access::get_symbolizer(*eng)->get_string(id) == name) && "an instance's type id names its type name");
}
#endif
}

script_value script_value::make_class_object(const class_definition& cd, strong_ptr<class_instance> data, engine* eng) {
    script_value v(std::monostate{}, eng);
    v.set_stamp(cd.get_type_info());
    v.storage_ = instance_storage(std::move(data));
    return v;
}

script_value script_value::make_coroutine_handle(uint64_t type_id, std::shared_ptr<void> handle, engine* eng) {
    script_value v(std::monostate{}, eng);
    auto obj = acquire_object_holder(eng);
    obj->set_type(interned_type_text(eng, "coroutine_handle", type_id), type_id);
    obj->hold(std::move(handle));
    obj->mark_coroutine_handle();
    v.storage_ = obj;
    return v;
}

script_value script_value::make_cpp_object(std::string_view type_name, uint64_t type_id, std::shared_ptr<void> data, engine* eng) {
    if (!eng) {
        throw runtime_error("Cannot create cpp_object with null engine pointer");
    }

    // CRITICAL: Get persistent type_info from class registry (fast O(1) lookup by type_id)

    auto class_def = detail::engine_access::get_class_definition(*eng, type_id);
    if (!class_def) {
        throw runtime_error("Cannot create cpp_object of unregistered class '" + std::string(type_name) +
            "'. Classes must be registered with engine.register_class() before instantiation.");
    }

    script_value v(std::monostate{}, eng);
    v.set_stamp(class_def->get_type_info());

    auto obj = acquire_object_holder(eng);
    obj->set_type(interned_type_text(eng, type_name, type_id), type_id);
    obj->hold(data);
    v.storage_ = obj;
    return v;
}

script_value script_value::make_cpp_reference(std::string_view type_name, uint64_t type_id, void* object,
                                              std::weak_ptr<void> owner, engine* eng) {
    if (!eng) {
        throw runtime_error("Cannot create cpp_reference with null engine pointer");
    }
    auto class_def = detail::engine_access::get_class_definition(*eng, type_id);
    if (!class_def) {
        throw runtime_error("Cannot create cpp_reference of unregistered class '" + std::string(type_name) + "'");
    }
    script_value v(std::monostate{}, eng);
    v.set_stamp(class_def->get_type_info());
    auto obj = acquire_object_holder(eng);
    obj->set_type(interned_type_text(eng, type_name, type_id), type_id);
    obj->bind_object(object);
    obj->set_weak_data(std::move(owner));
    v.storage_ = obj;
    return v;
}

script_value script_value::make_empty_weak_ptr(type_info_ptr weak_ptr_type, engine* eng) {
    script_value v(std::monostate{}, eng);
    if (weak_ptr_type) {
        v.set_stamp(weak_ptr_type);
    } else if (eng) {
        v.set_stamp(detail::engine_access::get_type_info_weak_ptr(*eng, nullptr));
    }
    v.storage_ = jai::weaker_ptr<object_holder>();
    return v;
}

script_value script_value::make_invalid(engine* eng) {
    script_value val(std::monostate{}, eng);  // Start with null
    val.storage_ = invalid_tag{};  // Change to invalid
    if (eng) {
        val.set_stamp(detail::engine_access::get_type_info_invalid(*eng));  // Set proper type info
    }
    return val;
}

script_value script_value::make_weak_instance(const script_value& instance, engine* eng) {
    const script_value& object = instance.deref();
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_weak_ptr(*eng, object.get_type_info()));
    v.storage_ = jai::weaker_ptr<object_holder>(object.get_object_holder());
    return v;
}

checked_result<script_value> script_value::make_weak_ptr(const script_value& value, engine* eng) {
    if (!eng) {
        return checked_result<script_value>(
            make_error_code(runtime_error_code::unsupported_operation),
            "Cannot create weak_ptr with null engine pointer");
    }
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_weak_ptr(*eng, value.get_type_info()));

    // Only accept shared_ptr types for weak_ptr creation
    // Regular objects have value semantics and get cloned when passed as parameters,
    // so creating a weak_ptr from them doesn't work correctly
    if (value.type() == script_value_type::jai_shared_ptr_type) {
        auto holder = value.get_object_holder();
        if (!holder) {
            return checked_result<script_value>(
                make_error_code(runtime_error_code::unsupported_operation),
                "Failed to get object_holder from script_value");
        }

        jai::weaker_ptr<object_holder> weak = holder;
        v.storage_ = weak;
    } else if (value.type() == script_value_type::jai_object_type) {
        return checked_result<script_value>(
            make_error_code(runtime_error_code::type_mismatch),
            "Cannot create weak_ptr from a value-semantic object. Use shared_ptr<T> to enable reference semantics: auto obj = shared_ptr<T>(...); auto weak = weak_ptr<T>(obj);");
    } else {
        return checked_result<script_value>(
            make_error_code(runtime_error_code::type_mismatch),
            "weak_ptr can only be created from shared_ptr<T>");
    }

    return checked_result<script_value>(v);
}

script_value script_value::make_element_reference(const strong_ptr<script_array>& container, size_t index,
                                                  engine* eng, type_info_ptr element_type) {
    if (!container || index >= container->size()) {
        throw runtime_error("Cannot create reference to out-of-range array element");
    }
    if (!eng) {
        throw runtime_error("Cannot create reference: null engine pointer");
    }
    script_value v(std::monostate{}, eng);
    type_info_ptr referent_tag;
    if (container->is_typed()) {
        switch (container->kind()) {
            case script_array::kind_t::i64:
            case script_array::kind_t::u8:
            case script_array::kind_t::i8: referent_tag = detail::engine_access::get_type_info_int(*eng); break;
            case script_array::kind_t::f64: referent_tag = detail::engine_access::get_type_info_float(*eng); break;
            case script_array::kind_t::b8: referent_tag = detail::engine_access::get_type_info_bool(*eng); break;
            case script_array::kind_t::c8: referent_tag = detail::engine_access::get_type_info_char(*eng); break;
            case script_array::kind_t::obj: referent_tag = container->objs()[index].tag; break;
            case script_array::kind_t::v2: case script_array::kind_t::iv2: case script_array::kind_t::v3:
            case script_array::kind_t::iv3: case script_array::kind_t::v4: case script_array::kind_t::rgba:
                referent_tag = detail::engine_access::get_type_info_math(*eng, script_array::vec_value_type(container->kind()));
                break;
            default: referent_tag = detail::engine_access::get_type_info_string(*eng); break;
        }
    } else {
        referent_tag = container->values()[index].get_type_info();
    }
    v.set_stamp(detail::engine_access::get_type_info_reference(*eng, referent_tag));
    auto ref = acquire_reference_holder(eng);
    ref->container_element_type = element_type;
    ref->container = container;          // owns the node (keeps it alive) + enables re-resolve
    ref->container_index = index;
    ++container->live_elem_refs;
    v.storage_ = ref;
    return v;
}

script_value script_value::make_removed_element_reference(const strong_ptr<script_array>& container, size_t index,
                                                          engine* eng, type_info_ptr element_type) {
    if (!container || !eng) {
        throw runtime_error("Cannot create reference: null engine pointer");
    }
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_reference(*eng, element_type));
    auto ref = acquire_reference_holder(eng);
    ref->container_element_type = element_type;
    ref->container = container;
    ref->container_index = index;
    ++container->live_elem_refs;
    v.storage_ = ref;
    return v;
}

script_value script_value::make_field_reference(strong_ptr<class_instance> owner, uint64_t field_id,
                                                engine* eng, type_info_ptr field_type) {
    if (!owner) {
        throw runtime_error("Cannot create reference to null");
    }
    if (!eng) {
        throw runtime_error("Cannot create reference: null engine pointer");
    }
    script_value* field_value = owner->find_field_value(field_id);
    if (!field_value) {
        throw runtime_error("Reference to a removed field");
    }
    return make_field_reference(std::move(owner), field_id, eng, std::move(field_type), *field_value);
}

script_value script_value::make_field_reference(strong_ptr<class_instance> owner, uint64_t field_id,
                                                engine* eng, type_info_ptr field_type, const script_value& field_value,
                                                uint32_t field_slot) {
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_reference(*eng, field_value.get_type_info()));
    auto ref = acquire_reference_holder(eng);
    if (field_slot != UINT32_MAX && !detail::engine_access::parallel_region_active(*eng)) {
        if (const class_definition* cd = owner->get_class_definition()) {
            ref->field_class = cd;
            ref->field_class_epoch = cd->field_shape_epoch();
            ref->field_slot = field_slot;
        }
    }
    owner->note_field_ref_minted();
    ref->owner_instance = owner.get();
    ref->field_id = field_id;
    ref->container_element_type = std::move(field_type);
    ref->owner_shared = std::move(owner);          // pins the instance for the reference's lifetime
    v.storage_ = std::move(ref);
    return v;
}

void script_value::pin_element_root(script_value& reference, const strong_ptr<script_array>& root) {
    auto holder = reference.get_reference_holder();
    if (!holder || !root || holder->elem_root || holder->container == root) {
        return;   // a direct element reference on root already counts
    }
    holder->elem_root = root;
    ++root->live_elem_refs;
}

type_info_ptr script_value::cpp_bound_type_info(engine* eng, uint8_t semantic_index) {
    switch (semantic_index) {
        case TYPEID_INT: return detail::engine_access::get_type_info_int(*eng);
        case TYPEID_FLOAT: return detail::engine_access::get_type_info_float(*eng);
        case TYPEID_BOOL: return detail::engine_access::get_type_info_bool(*eng);
        case TYPEID_STRING: return detail::engine_access::get_type_info_string(*eng);
        case TYPEID_CHAR: return detail::engine_access::get_type_info_char(*eng);
        default: return nullptr;
    }
}

script_value script_value::make_referent_reference(const script_value& handle, engine* eng) {
    if (!eng) {
        throw runtime_error("Cannot create reference: null engine pointer");
    }
    auto holder = handle.get_object_holder();
    if (!holder || !holder->bound_object()) {
        throw runtime_error("Cannot create reference to null");
    }
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_reference(*eng, handle.stamp()));
    auto ref = acquire_reference_holder(eng);
    ref->property = std::make_unique<property_binding>();
    ref->property->receiver = holder;
    ref->property->receiver_tag = handle.get_type_info();
    ref->property->object_pin = holder->keep_alive();
    ref->property->live_member = true;
    ref->property->referent = holder->bound_object();
    ref->property->referent_class = holder->type_id() != UINT64_MAX ? detail::engine_access::get_class_definition(*eng, holder->type_id()).get() : nullptr;
    new (ref->cell_storage) script_value(handle);
    v.storage_ = ref;
    return v;
}

// an assignment through a reference to a bound object: the operator= the referent's class binds ("="), as C++ calls the class's own
void script_value::reference_holder::store_referent(const script_value& value, engine* eng) {
    const class_definition* def = property->referent_class;
    const uint64_t assign_id = detail::engine_access::get_symbolizer(*eng)->intern("=");
    if (!def || !def->has_method(assign_id)) {
        throw runtime_error("Cannot assign to a reference to '" + (def ? def->get_name() : std::string("object")) + "': its binding has no operator=");
    }
    script_value assign = def->get_method(assign_id, false);
    std::vector<script_value> args{ *cell(), value.is_reference() ? value.deref() : value };
    auto assigned = assign.as_function()(args);
    if (!assigned) {
        throw runtime_error(format_error(assigned, *detail::engine_access::get_symbolizer(*eng)));
    }
}

script_value script_value::make_cpp_member_reference(const script_value& receiver, detail::object_ref object_pin,
                                                     void* address, uint8_t size_and_sign, uint8_t semantic_index,
                                                     uint64_t field_id, engine* eng) {
    if (!eng) {
        throw runtime_error("Cannot create reference: null engine pointer");
    }
    if (!address || !receiver.get_object_holder()) {
        throw runtime_error("Cannot create reference to null");
    }
    script_value box(std::monostate{}, eng);
    box.set_stamp(cpp_bound_type_info(eng, semantic_index));
    box.storage_ = make_strong<cpp_bound_holder>(address, size_and_sign, semantic_index, eng);
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_reference(*eng, box.stamp()));
    auto ref = acquire_reference_holder(eng);
    ref->property = std::make_unique<property_binding>();
    ref->property->receiver = receiver.get_object_holder();
    ref->property->receiver_tag = receiver.get_type_info();
    ref->property->object_pin = std::move(object_pin);
    ref->property->field_id = field_id;
    ref->property->live_member = true;
    new (ref->cell_storage) script_value(std::move(box));
    v.storage_ = ref;
    return v;
}

script_value script_value::make_property_reference(const script_value& receiver, uint64_t field_id, engine* eng) {
    if (!eng) {
        throw runtime_error("Cannot create reference: null engine pointer");
    }
    if (!receiver.get_object_holder()) {
        throw runtime_error("Cannot create reference to null");
    }
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_reference(*eng, nullptr));
    auto ref = acquire_reference_holder(eng);
    ref->property = std::make_unique<property_binding>();
    ref->property->receiver = receiver.get_object_holder();
    ref->property->receiver_tag = receiver.get_type_info();
    ref->property->field_id = field_id;
    v.storage_ = ref;
    return v;
}

script_value script_value::make_string_element_reference(const script_value& pin, size_t index, engine* eng) {
    if (!eng) {
        throw runtime_error("Cannot create reference: null engine pointer");
    }
    if (!pin.get_reference_holder()) {
        throw runtime_error("Cannot create reference to null");
    }
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_reference(*eng, nullptr));
    auto ref = acquire_reference_holder(eng);
    ref->string_base = pin.storage_.get<strong_ptr<reference_holder>>();
    ref->container_index = index;
    v.storage_ = ref;
    return v;
}

script_value script_value::make_component_reference(const script_value& pin, size_t component, engine* eng) {
    if (!eng) {
        throw runtime_error("Cannot create reference: null engine pointer");
    }
    if (!pin.get_reference_holder()) {
        throw runtime_error("Cannot create reference to null");
    }
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_reference(*eng, nullptr));
    auto ref = acquire_reference_holder(eng);
    ref->component_base = pin.storage_.get<strong_ptr<reference_holder>>();
    ref->container_index = component;
    v.storage_ = ref;
    return v;
}

script_value script_value::make_component_pin(const reference_holder& holder, engine* eng) {
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_reference(*eng, nullptr));
    v.storage_ = holder.component_base;
    return v;
}

script_value script_value::make_string_element_pin(const reference_holder& holder, engine* eng) {
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_reference(*eng, nullptr));
    v.storage_ = holder.string_base;
    return v;
}

script_value script_value::make_function(const script_function& func, engine* eng) {
    return make_function(script_function(func), eng);
}

script_value script_value::make_function(script_function&& func, engine* eng) {
    if (!eng) {
        throw runtime_error("Cannot create function with null engine pointer");
    }
    script_value v(std::monostate{}, eng);
    v.set_stamp(detail::engine_access::get_type_info_untyped_function(*eng));
    v.storage_ = make_strong<script_function_node>(std::move(func), eng);
    return v;
}

#ifdef JAISCRIPT_VM_COVERAGE
namespace {
    // the raw indexes whose copy touches a refcount
    inline bool census_refcounted(size_t idx) noexcept {
        return idx != script_value::TYPEID_NULL && idx != script_value::TYPEID_INT && idx != script_value::TYPEID_FLOAT &&
               idx != script_value::TYPEID_CHAR && idx != script_value::TYPEID_BOOL && idx != script_value::TYPEID_INVALID;
    }
}
#endif

script_value::script_value(const script_value& other)
    : storage_(other.storage_) {
    // The variant copy bumps the (immutable) binding box refcount, so copies of bound values remain aliases
    // NOTE: Raw engine* is much faster to copy than weak_ptr (no atomic ops)
#ifdef JAISCRIPT_VM_COVERAGE
    if (census_refcounted(other.raw_storage_index())) { census::hit_at(census::site::value_copy_ctor, JAI_CENSUS_RA(), static_cast<uint32_t>(other.raw_storage_index())); }
#endif
}

// NOTE: Intentionally shallow; the interpreter handles cloning based on JaiScript value/reference semantics.
script_value& script_value::operator=(const script_value& other) {
    if (this != &other) {
#ifdef JAISCRIPT_VM_COVERAGE
        if (census_refcounted(other.raw_storage_index())) { census::hit_at(census::site::value_copy_assign, JAI_CENSUS_RA(), static_cast<uint32_t>(other.raw_storage_index())); }
#endif
        storage_.assign_whole(other.storage_);
    }
    return *this;
}

#ifdef JAISCRIPT_VM_COVERAGE
// The counting clone: the outermost call is one deep-copy event at its caller, with the source's node, whether it was
// uniquely held (uniquely_held's test) and the copied kind; the copy's node is timed to its death
script_value script_value::clone() const {
    const script_value& src = *this;   // a reference is not dereferenced here: a property read would run its getter twice
    const void* node = nullptr;
    bool unique = false;
    census::copied_kind kind = census::copied_kind::other;
    switch (src.raw_storage_index()) {
        case TYPEID_ARRAY: {
            const auto& a = src.unchecked_get_array_storage();
            node = a.get(); unique = a.use_count() == 1;
            kind = a->kind() == script_array::kind_t::obj ? census::copied_kind::array_obj : a->is_typed() ? census::copied_kind::array_typed : census::copied_kind::array_values;
            break;
        }
        case TYPEID_MAP: { const auto& m = src.unchecked_get_map_storage(); node = m.get(); unique = m.use_count() == 1; kind = census::copied_kind::map; break; }
        case TYPEID_OBJECT: {
            const auto& h = src.unchecked_object_handle();
            node = h.get();
            unique = h && h.use_count() == 1 && h->holds_object() && h->object_use_count() == 1;
            const bool shared = src.stamp() && src.stamp()->base_type == script_value_type::jai_shared_ptr_type;
            kind = shared ? census::copied_kind::shared_deep : (h && h->is_instance()) ? census::copied_kind::instance : census::copied_kind::cpp_object;
            break;
        }
        case TYPEID_STRING: kind = census::copied_kind::string; break;
        default: break;
    }
    if (raw_storage_index() == TYPEID_REFERENCE) { kind = census::copied_kind::reference; }
    census::copy_scope scope(census::site::deep_clone, JAI_CENSUS_RA(), node, (unique ? census::k_src_unique : 0u) | census::kind_flag(kind));
    script_value result = clone_uncounted();
    const void* made = nullptr;
    switch (result.raw_storage_index()) {
        case TYPEID_ARRAY: made = result.unchecked_get_array_storage().get(); census::add_weight(result.unchecked_get_array_storage()->size()); break;
        case TYPEID_MAP: made = result.unchecked_get_map_storage().get(); census::add_weight(2 * result.unchecked_get_map_storage()->size()); break;
        case TYPEID_OBJECT: made = result.unchecked_object_handle().get(); break;
        default: break;
    }
    scope.finish(made != node ? made : nullptr);
    return result;
}

script_value script_value::clone_uncounted() const {
#else
script_value script_value::clone() const {
#endif
    detail::deep_copy_work work;
    return clone_level(work, 0);
}

void script_value::clone_element_into(script_value& dest, detail::deep_copy_work& work, unsigned depth) const {
    if (element_copies_shallow()) {
        dest = *this;
    } else if (depth < detail::k_inline_copy_depth) {
        dest = clone_level(work, depth + 1);
    } else if (depth == detail::k_inline_copy_depth) {
        clone_subtree_into(dest);
    } else {
        work.push_back({detail::deep_copy_item::step::value, this, &dest});
    }
}

// Each level pushes its steps reversed, so the back of the list is always the next step the native walk would take
void script_value::clone_subtree_into(script_value& dest) const {
    using step = detail::deep_copy_item::step;
    constexpr unsigned stepped = detail::k_inline_copy_depth + 1;
    detail::deep_copy_work work;
    dest = clone_level(work, stepped);
    while (!work.empty()) {
        const detail::deep_copy_item item = work.back();
        work.pop_back();
        switch (item.what) {
            case step::value:
                *static_cast<script_value*>(item.dest) = static_cast<const script_value*>(item.source)->clone_level(work, stepped);
                break;
            case step::entry: {
                const auto& [key, value] = *static_cast<const script_map::value_type*>(item.source);
                auto& copied = *static_cast<script_map*>(item.dest);
                if (value.element_copies_shallow()) {
                    copied.emplace(key.clone_element(), value);
                } else {
                    auto placed = copied.emplace(key.clone_element(), script_value(std::monostate{}, copied.owner));
                    work.push_back({step::value, &value, &placed.first->second});
                }
                break;
            }
            case step::slot: {
                const auto* source = static_cast<const script_array*>(item.source);
                script_value element = source->get(item.index, source->owner);
                static_cast<script_array*>(item.dest)->push(element.element_copies_shallow() ? std::move(element) : element.clone_level(work, stepped));
                break;
            }
            case step::finish:
            case step::finish_overflow:
                static_cast<const class_instance*>(item.source)->finish_deep_copy_level(*static_cast<class_instance*>(item.dest), item.what == step::finish_overflow);
                break;
        }
    }
}

script_value script_value::clone_level(detail::deep_copy_work& work, unsigned depth) const {
    // Parallel captured-read borrow: cloning IS the tier-3 materialization boundary
    // (store kernel / explicit clone inside a region). The silent deep clone reads the
    // shared container by const& only - no transient handle copies, no refcount race.
    if (raw_storage_index() == TYPEID_PARALLEL_BORROW) {
        if (const auto* record = parallel_object_borrow()) { return detail::parallel_object_copy(*this, *record); }
        return parallel_detached_copy(payload_engine());
    }

    // shared_ptr<T> is a TYPE MARKER that indicates reference semantics for normal operations,
    // but clone() should perform a deep copy to create an independent instance
    if (stamp() && stamp()->base_type == script_value_type::jai_shared_ptr_type) {
        auto obj_holder = storage_.get<strong_ptr<object_holder>>();
        if (!obj_holder) {
            throw runtime_error("Cannot clone shared_ptr: null object_holder");
        }

        if (obj_holder->is_instance()) {
            auto new_instance = obj_holder->unchecked_instance()->deep_copy_level(work, depth);
            script_value result = tagged_null();
            result.storage_ = instance_storage(std::move(new_instance));
            return result;
        }

        auto class_def = detail::engine_access::get_class_definition(obj_holder->owner(), obj_holder->type_id());
        if (class_def && class_def->has_copy_function()) {
            auto new_cpp_obj = class_def->copy_object(obj_holder->object());
            if (new_cpp_obj) {
                auto new_holder = acquire_object_holder(obj_holder->owner());
                new_holder->set_type(obj_holder->type_name(), obj_holder->type_id());
                new_holder->hold(new_cpp_obj);

                script_value result = tagged_null();
                result.storage_ = new_holder;
                return result;
            }
        }

        throw runtime_error(
            "Cannot clone shared_ptr<" + std::string(obj_holder->type_name()) + ">: type is non-copyable. "
            "Register a copy constructor with dynamic_binder<" + std::string(obj_holder->type_name()) + ">::copy_constructor() "
            "to enable deep copying.");
    }

    script_value result = tagged_null();

    // Use current_type() to check what's actually stored, not the declared type
    switch (current_type()) {
        case script_value_type::jai_array_type: {
            const auto& other_node = *storage_.get<strong_ptr<script_array>>();
            if (other_node.kind() == script_array::kind_t::obj) {
                // object slots keep value semantics: every element deep-clones (shared_ptr
                // elements share, as clone_element already decides), the tag rides along
                const size_t n = other_node.size();
                charge_deferred(other_node.owner, sizeof(obj_slot) * (n + 1));
                auto new_node = make_strong<script_array>(script_array::kind_t::obj);
                new_node->owner = other_node.owner;
                new_node->reserve(n);
                if (depth > detail::k_inline_copy_depth) {
                    // a slot holds the object itself, never a placeholder: each one is a step that copies and pushes it
                    for (size_t i = n; i-- > 0;) { work.push_back({detail::deep_copy_item::step::slot, &other_node, new_node.get(), i}); }
                } else {
                    for (size_t i = 0; i < n; ++i) {
                        script_value element = other_node.get(i, other_node.owner);
                        if (element.element_copies_shallow()) {
                            new_node->push(std::move(element));
                        } else if (depth < detail::k_inline_copy_depth) {
                            new_node->push(element.clone_level(work, depth + 1));
                        } else {
                            script_value copied(std::monostate{}, other_node.owner);
                            element.clone_subtree_into(copied);
                            new_node->push(std::move(copied));
                        }
                    }
                }
                result.storage_ = new_node;
                break;
            }
            if (other_node.is_typed()) {
                // kind-preserving deep copy = one buffer copy (8B/element)
                charge_deferred(other_node.owner, sizeof(script_int) * (other_node.size() + 1));
                auto new_node = make_strong<script_array>(other_node.kind());
                new_node->owner = other_node.owner;
                new_node->assign_buffer(other_node);
                result.storage_ = new_node;
                break;
            }
            const auto& other_array = other_node.values();
            // element clones charge themselves recursively
            charge_deferred(other_node.owner, sizeof(script_value) * (other_array.size() + 1));
            auto new_node = make_strong<script_array>();
            new_node->owner = other_node.owner;
            auto& copied = new_node->values();
            copied.reserve(other_array.size());   // no reallocation: a deferred element's placeholder stays where it is
            const size_t first_step = work.size();
            for (const auto& elem : other_array) {
                if (elem.element_copies_shallow()) {
                    copied.push_back(elem);
                } else {
                    copied.push_back(script_value(std::monostate{}, other_node.owner));
                    elem.clone_element_into(copied.back(), work, depth);
                }
            }
            std::reverse(work.begin() + static_cast<ptrdiff_t>(first_step), work.end());
            result.storage_ = new_node;
            break;
        }
        case script_value_type::jai_map_type: {
            auto& other_map = *storage_.get<strong_ptr<script_map>>();
            // key/value clones charge themselves recursively
            charge_deferred(other_map.owner, 2 * sizeof(script_value) * (other_map.size() + 1));
            auto new_map = make_strong<script_map>(other_map.owner);
            if (depth > detail::k_inline_copy_depth) {
                // an entry is one step (its key, then its value's whole copy) so keys and values interleave as below
                const size_t first_step = work.size();
                for (const auto& entry : other_map) { work.push_back({detail::deep_copy_item::step::entry, &entry, new_map.get()}); }
                std::reverse(work.begin() + static_cast<ptrdiff_t>(first_step), work.end());
                result.storage_ = new_map;
                break;
            }
            for (const auto& [key, value] : other_map) {
                if (value.element_copies_shallow()) {
                    new_map->emplace(key.clone_element(), value);
                } else {
                    auto placed = new_map->emplace(key.clone_element(), script_value(std::monostate{}, other_map.owner));
                    value.clone_element_into(placed.first->second, work, depth);   // map entries never move
                }
            }
            result.storage_ = new_map;
            break;
        }
        case script_value_type::jai_object_type: {
            // Regular objects have VALUE semantics by default (deep copy)
            // Only shared_ptr<T> has reference semantics (handled by early return above)
            auto obj_holder = storage_.get<strong_ptr<object_holder>>();

            // ...and coroutine handles: a handle references a RUNNING computation, so
            // every copy shares it (field stores, aliases, params, containers all see
            // the same resume/done state). Deep-copying a suspended coroutine is
            // meaningless - this is the ruled reference-semantics-on-copy design.
            if (obj_holder->is_coroutine_handle()) {
                return *this;
            }

            if (obj_holder->is_instance()) {
                auto new_instance = obj_holder->unchecked_instance()->deep_copy_level(work, depth);
                result.storage_ = instance_storage(std::move(new_instance));
            } else {
                bool copied = false;
                if (engine* owner = obj_holder->owner()) {
                    auto class_def = detail::engine_access::get_class_definition(*owner, obj_holder->type_id());
                    if (class_def && class_def->has_copy_function()) {
                        // cpp_bound references own nothing (data null) - the live object is
                        // bound_ptr. Deep copy from it and drop the binding so the
                        // clone is an independent owned object, not an alias.
                        const void* copy_src = obj_holder->holds_object() ? obj_holder->object() : obj_holder->bound_object();
                        auto new_cpp_obj = copy_src ? class_def->copy_object(copy_src) : nullptr;
                        if (new_cpp_obj) {
                            auto new_holder = acquire_object_holder(owner);
                            new_holder->set_type(obj_holder->type_name(), obj_holder->type_id());
                            new_holder->hold(new_cpp_obj);
                            result.storage_ = new_holder;  // fresh holder: bound_ptr stays null (clone detaches)
                            copied = true;
                        }
                    }
                }

                if (!copied) {
                    throw runtime_error(
                        "Cannot copy C++ object of type '" + std::string(obj_holder->type_name()) +
                        "': the type is not copy-constructible. Hold it by reference (auto&/var&) "
                        "or share it (shared_ptr<" + std::string(obj_holder->type_name()) + ">).");
                }
            }
            break;
        }
        case script_value_type::jai_reference_type: {
            // When cloning a reference, we want to clone the referenced value,
            // not create another reference to the same value
            return deref().clone_level(work, depth);
        }
        case script_value_type::jai_weak_ptr_type: {
            // For weak_ptr, shallow copy the weak_ptr itself (copy the weak reference)
            result.storage_ = storage_;
            break;
        }
        case script_value_type::jai_shared_ptr_type: {
            // For shared_ptr, shallow copy the shared_ptr itself (copy the shared reference)
            result.storage_ = storage_;
            break;
        }
        default:
            // For cpp_bound values, we need to read the actual value and create an independent copy
            if (is_cpp_bound()) {
                // Read the actual value from the C++ variable and create a new independent value
                // Assigning fresh storage inherently detaches the clone from the C++ variable
                if (is_int()) {
                    result.storage_ = as_int();
                } else if (is_float()) {
                    result.storage_ = as_float();
                } else if (is_bool()) {
                    result.storage_ = as_bool();
                } else if (is_char()) {
                    result.storage_ = as_char();
                } else if (is_string()) {
                    result.storage_ = make_strong<script_string>(as_string());
                } else {
                    // For other cpp_bound types, fall back to shallow copy (opaque bounds stay bound)
                    result.storage_ = storage_;
                }
            } else {
                // For primitive types and functions, shallow copy is fine
                result.storage_ = storage_;
            }
            break;
    }

    return result;
}

// The copy-on-write half of unchecked_as_string_ref (value.hpp): the caller proved the
// node is shared. Deferred charge like every internal clone; a null engine (a parse-time
// literal) is never written through this gate.
script_string& script_value::unshare_string_node(engine* a_engine) {
    auto& node = *storage_.get_if<TYPEID_STRING>();
#ifdef JAISCRIPT_VM_COVERAGE
    census::copy_scope census_scope(census::site::string_detach, JAI_CENSUS_RA(), node.get(), 0);
    census::add_weight(node->size());
#endif
    charge_deferred(a_engine, node->size() + sizeof(script_string));
    node = make_strong<script_string>(*node);
    return *node;
}

// Parallel-region detach (see value.hpp): fresh strong_ptr for EVERY heavy node
// (strings included), bound primitives decoded, type_info preserved, value-semantic
// content only. The one kernel the parallel_transform barrier trusts for exclusivity,
// and the region's REFCOUNT-SILENT deep clone: every element is read by const& and
// rebuilt fresh - no transient shallow copy of a shared handle ever exists, so this
// may traverse a borrowed (region-frozen) container from a worker thread.
namespace {
    // A worker-private copy of a typed array. Scalar kinds are one buffer copy; the str kind
    // holds refcounted handles (assign_buffer would share them across threads, and the counts
    // are not atomic), so it gets one fresh string per slot.
    strong_ptr<script_array> detached_typed_array(const script_array& src, engine* eng) {
        auto fresh = make_strong<script_array>(src.kind());
        fresh->owner = eng;
        if (src.kind() == script_array::kind_t::str) {
            size_t bytes = sizeof(script_array);
            fresh->strs().reserve(src.size());
            for (const auto& str : src.strs()) {
                if (!str) { fresh->strs().push_back(make_strong<script_string>()); bytes += sizeof(script_string); continue; }
                bytes += str->size() + sizeof(script_string);
                fresh->strs().push_back(make_strong<script_string>(*str));
            }
            detail::engine_access::execution_limits(*eng).memory_charge_deferred(bytes);
        } else {
            detail::engine_access::execution_limits(*eng).memory_charge_deferred(sizeof(script_int) * (src.size() + 1));
            fresh->assign_buffer(src);
        }
        return fresh;
    }
}

script_value script_value::parallel_host_view() const {
    const auto& holder = storage_.get<strong_ptr<object_holder>>();
    if (!holder || !holder->is_instance()) { throw runtime_error("parallel_host_view: not a class instance"); }
    const auto* inst = holder->unchecked_instance();
    class_definition* cd = inst->get_class_definition();
    const script_value* native = inst->native();
    const auto native_holder = native ? native->get_object_holder() : nullptr;
    if (!cd || !native_holder || !native_holder->holds_object()) { throw runtime_error("parallel_host_view: the instance owns no C++ object"); }
    auto view = cd->allocate_instance();
    view->set_class_definition(cd);
    view->set_native(make_cpp_object(native_holder->type_name(), native_holder->type_id(), native_holder->host_object(), holder->owner()));
    cd->register_live_instance(view.get());
    if (view->type_id() == UINT64_MAX) { view->set_type(holder->type_name(), holder->type_id()); }
    script_value result = tagged_null();
    result.storage_ = instance_storage(std::move(view));
    return result;
}

script_value script_value::parallel_detached_copy(engine* eng, std::vector<type_info*>* collected_types,
                                                  std::vector<const void*>* collected_nodes) const {
    detail::deep_copy_work work;
    return parallel_detached_level(eng, collected_types, collected_nodes, work, 0);
}

// As clone_subtree_into: the steps read the source by pointer, never by a handle copy
void script_value::parallel_detached_subtree_into(engine* eng, script_value& dest, std::vector<type_info*>* collected_types,
                                                  std::vector<const void*>* collected_nodes) const {
    using step = detail::deep_copy_item::step;
    constexpr unsigned stepped = detail::k_inline_copy_depth + 1;
    detail::deep_copy_work work;
    dest = parallel_detached_level(eng, collected_types, collected_nodes, work, stepped);
    while (!work.empty()) {
        const detail::deep_copy_item item = work.back();
        work.pop_back();
        if (item.what == step::entry) {
            const auto& [key, value] = *static_cast<const script_map::value_type*>(item.source);
            auto placed = static_cast<script_map*>(item.dest)->emplace(key.parallel_detached_copy(eng, collected_types, collected_nodes), script_value(std::monostate{}, eng));
            work.push_back({step::value, &value, &placed.first->second});
        } else {
            *static_cast<script_value*>(item.dest) = static_cast<const script_value*>(item.source)->parallel_detached_level(eng, collected_types, collected_nodes, work, stepped);
        }
    }
}

script_value script_value::parallel_detached_level(engine* eng, std::vector<type_info*>* collected_types, std::vector<const void*>* collected_nodes,
                                                   detail::deep_copy_work& work, unsigned depth) const {
    auto detach_into = [&](const script_value& element, script_value& dest) {
        if (depth < detail::k_inline_copy_depth) {
            dest = element.parallel_detached_level(eng, collected_types, collected_nodes, work, depth + 1);
        } else if (depth == detail::k_inline_copy_depth) {
            element.parallel_detached_subtree_into(eng, dest, collected_types, collected_nodes);
        } else {
            work.push_back({detail::deep_copy_item::step::value, &element, &dest});
        }
    };
    const size_t first_step = work.size();
    auto steps_in_order = [&]() { std::reverse(work.begin() + static_cast<ptrdiff_t>(first_step), work.end()); };
    auto detach_entries = [&](const script_map& from, script_map& fresh) {
        if (depth > detail::k_inline_copy_depth) {
            for (const auto& entry : from) { work.push_back({detail::deep_copy_item::step::entry, &entry, &fresh}); }
            steps_in_order();
            return;
        }
        for (const auto& [key, val] : from) {
            auto placed = fresh.emplace(key.parallel_detached_copy(eng, collected_types, collected_nodes), script_value(std::monostate{}, eng));
            detach_into(val, placed.first->second);
        }
    };
    const size_t idx = raw_storage_index();
    if (idx == TYPEID_REFERENCE) {
        return deref().parallel_detached_level(eng, collected_types, collected_nodes, work, depth);
    }
    if (collected_types && stamp()) {
        collected_types->push_back(stamp());
    }
    script_value result = tagged_null();
    switch (idx) {
        case TYPEID_NULL:
        case TYPEID_INT:
        case TYPEID_FLOAT:
        case TYPEID_CHAR:
        case TYPEID_BOOL:
            result.storage_ = storage_;
            break;
        case TYPEID_VEC2:
        case TYPEID_IVEC2:
        case TYPEID_VEC3:
        case TYPEID_IVEC3:
        case TYPEID_VEC4:
        case TYPEID_COLOR: {
            auto fresh = make_strong<script_vec_node>(*storage_.vec_node());
            if (collected_nodes) { collected_nodes->push_back(fresh.get()); }
            result.storage_ = std::move(fresh);
            break;
        }
        case TYPEID_STRING: {
            const auto& handle = storage_.get<strong_ptr<script_string>>();
            if (!handle) { break; }   // null handle shares nothing
            charge_deferred(eng, handle->size() + sizeof(script_string));
            auto fresh = make_strong<script_string>(*handle);
            if (collected_nodes) { collected_nodes->push_back(fresh.get()); }
            result.storage_ = fresh;
            break;
        }
        case TYPEID_UTF8_STRING: {
            const auto& handle = storage_.get<strong_ptr<script_utf8>>();
            if (!handle) { break; }
            charge_deferred(eng, handle->bytes.size() + sizeof(script_utf8));
            auto fresh = make_strong<script_utf8>(handle->bytes);
            if (collected_nodes) { collected_nodes->push_back(fresh.get()); }
            result.storage_ = fresh;
            break;
        }
        case TYPEID_ARRAY: {
            const auto& handle = storage_.get<strong_ptr<script_array>>();
            if (!handle) { break; }
            if (handle->kind() == script_array::kind_t::obj) {
                const size_t n = handle->size();
                charge_deferred(eng, sizeof(obj_slot) * (n + 1));
                auto fresh = make_strong<script_array>(script_array::kind_t::obj);
                fresh->owner = eng;
                fresh->reserve(n);
                for (size_t i = 0; i < n; ++i) {
                    fresh->push(handle->get(i, eng).parallel_detached_level(eng, collected_types, collected_nodes, work, depth + 1));
                }
                if (collected_nodes) { collected_nodes->push_back(fresh.get()); }
                result.storage_ = fresh;
                break;
            }
            if (handle->is_typed()) {
                auto fresh = detached_typed_array(*handle, eng);
                if (collected_nodes) { collected_nodes->push_back(fresh.get()); }
                result.storage_ = fresh;
                break;
            }
            charge_deferred(eng, sizeof(script_value) * (handle->size() + 1));
            auto fresh = make_strong<script_array>();
                fresh->owner = eng;
            fresh->values().reserve(handle->size());   // no reallocation: a deferred element's placeholder stays where it is
            for (const auto& elem : handle->values()) {
                fresh->values().push_back(script_value(std::monostate{}, eng));
                detach_into(elem, fresh->values().back());
            }
            steps_in_order();
            if (collected_nodes) { collected_nodes->push_back(fresh.get()); }
            result.storage_ = fresh;
            break;
        }
        case TYPEID_MAP: {
            const auto& handle = storage_.get<strong_ptr<script_map>>();
            if (!handle) { break; }
            charge_deferred(eng, 2 * sizeof(script_value) * (handle->size() + 1));
            auto fresh = make_strong<script_map>(eng);
            detach_entries(*handle, *fresh);
            if (collected_nodes) { collected_nodes->push_back(fresh.get()); }
            result.storage_ = fresh;
            break;
        }
        case TYPEID_CPP_BOUND: {
            if (bound_semantic_index() != TYPEID_NULL) {
                // Decode, then re-detach so bound strings get fresh storage too; below the inline depth the decoded temporary
                // detaches whole here, since steps would outlive it
                const script_value decoded = bound_decoded_temp();
                if (depth <= detail::k_inline_copy_depth) {
                    return decoded.parallel_detached_level(eng, collected_types, collected_nodes, work, depth);
                }
                decoded.parallel_detached_subtree_into(eng, result, collected_types, collected_nodes);
                return result;
            }
            throw runtime_error("value is not value-semantic (bound host object)");
        }
        case TYPEID_PARALLEL_BORROW: {
            // Materialize the borrow: silent deep clone of the VIEWED container. The
            // viewed structure is all-primitive by the barrier's classification, and the
            // traversal below reads it by const& only.
            const auto& tag = storage_.get<parallel_borrow_tag>();
            if (const auto* record = parallel_object_borrow()) { return detail::parallel_object_copy(*this, *record); }
            if (const auto* arr = parallel_borrow_array()) {
                if (arr->is_typed()) {
                    auto fresh = detached_typed_array(*arr, eng);
                    if (collected_nodes) { collected_nodes->push_back(fresh.get()); }
                    result.storage_ = fresh;
                    break;
                }
                charge_deferred(eng, sizeof(script_value) * (arr->size() + 1));
                auto fresh = make_strong<script_array>();
                fresh->owner = eng;
                fresh->values().reserve(arr->size());
                for (const auto& elem : arr->values()) {
                    fresh->values().push_back(script_value(std::monostate{}, eng));
                    detach_into(elem, fresh->values().back());
                }
                steps_in_order();
                if (collected_nodes) { collected_nodes->push_back(fresh.get()); }
                result.storage_ = fresh;
            } else if (const auto* map = parallel_borrow_map()) {
                charge_deferred(eng, 2 * sizeof(script_value) * (map->size() + 1));
                auto fresh = make_strong<script_map>(eng);
                detach_entries(*map, *fresh);
                if (collected_nodes) { collected_nodes->push_back(fresh.get()); }
                result.storage_ = fresh;
            }
            (void)tag;
            break;
        }
        default: {
            // a worker's read through a shared object reaches its objects as borrows, never their handles
            if (idx == TYPEID_OBJECT && eng && detail::engine_access::parallel_region_active(*eng)) { return detail::parallel_shared_object(*this, *eng); }
            const char* what = idx == TYPEID_OBJECT ? "object"
                : idx == TYPEID_FUNCTION ? "function"
                : idx == TYPEID_WEAK_PTR ? "weak_ptr"
                : "unsupported type";
            throw runtime_error(std::string("value is not value-semantic (") + what + ")");
        }
    }
    return result;
}

// === Parallel captured-read borrow (see value.hpp) ===

static_assert(alignof(script_array) >= 8 && alignof(script_map) >= 8 && alignof(detail::object_borrow_record) >= 8,
              "a borrow's kind bits sit in the low three bits of the borrowed pointer");

script_value script_value::make_parallel_borrow(const script_value& source, engine* eng, bool sliced) {
    const script_value& v = source.deref();
    script_value result(std::monostate{}, eng);
    result.set_stamp(v.stamp());
    parallel_borrow_tag tag;
    if (v.raw_storage_index() == TYPEID_ARRAY) {
        const auto& handle = v.storage_.get<strong_ptr<script_array>>();
        tag.bits = reinterpret_cast<uintptr_t>(static_cast<const void*>(handle.get())) | (sliced ? parallel_borrow_tag::k_sliced_bit : 0);
    } else if (v.raw_storage_index() == TYPEID_MAP) {
        const auto& handle = v.storage_.get<strong_ptr<script_map>>();
        handle->prepare_order();   // workers read the borrow concurrently: its order view is built here, serially
        tag.bits = reinterpret_cast<uintptr_t>(static_cast<const void*>(handle.get())) | parallel_borrow_tag::k_map_bit;
    } else {
        throw runtime_error("make_parallel_borrow: source must be an array or map");
    }
    result.storage_ = tag;
    return result;
}

const script_array* script_value::parallel_borrow_array() const noexcept {
    if (const auto* tag = storage_.get_if<TYPEID_PARALLEL_BORROW>()) {
        if (!tag->is_map_kind() && !tag->is_object_kind()) {
            return static_cast<const script_array*>(tag->pointer());
        }
    }
    return nullptr;
}

bool script_value::is_sliced_parallel_borrow() const noexcept {
    const auto* tag = storage_.get_if<TYPEID_PARALLEL_BORROW>();
    return tag && tag->is_sliced();
}

script_value script_value::make_object_borrow(const detail::object_borrow_record* record, type_info_ptr tag, engine* eng) {
    script_value result(std::monostate{}, eng);
    result.set_stamp(tag.get());
    parallel_borrow_tag bits;
    bits.bits = reinterpret_cast<uintptr_t>(static_cast<const void*>(record)) | parallel_borrow_tag::k_object_bit;
    result.storage_ = bits;
    return result;
}

const detail::object_borrow_record* script_value::parallel_object_borrow() const noexcept {
    const auto* tag = storage_.get_if<TYPEID_PARALLEL_BORROW>();
    return tag && tag->is_object_kind() ? static_cast<const detail::object_borrow_record*>(tag->pointer()) : nullptr;
}

const script_map* script_value::parallel_borrow_map() const noexcept {
    if (const auto* tag = storage_.get_if<TYPEID_PARALLEL_BORROW>()) {
        if (tag->is_map_kind()) {
            return static_cast<const script_map*>(tag->pointer());
        }
    }
    return nullptr;
}

const script_function& script_value::as_function() const {
    const script_value& val = deref();
    if (val.current_type() != script_value_type::jai_function_type) {
        detail::throw_conversion_error(val, "function");
    }
    return *val.storage_.get<strong_ptr<script_function_node>>();
}

std::optional<script_value> script_value::try_unwrap_transparent_wrapper() const {
    if (!is_object() || !payload_engine()) {
        return std::nullopt;
    }
    auto holder = get_object_holder();
    if (!holder) {
        return std::nullopt;
    }
    auto class_def = detail::engine_access::get_class_definition(*payload_engine(), holder->type_id());
    if (!class_def || !class_def->is_transparent_wrapper()) {
        return std::nullopt;
    }
    script_value mutable_self = *this;
    script_value unwrapped = class_def->unwrap(mutable_self);
    if (unwrapped.is_null()) {
        return std::nullopt;
    }
    return unwrapped;
}

std::string script_value::float_text(double v) {
    if (std::isnan(v)) { return std::string(detail::k_nan_text); }
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);   // shortest round-trip
    std::string s(buf, res.ptr);
    if (s.find_first_of(".en") == std::string::npos) { s += ".0"; }   // a whole number stays a float ("inf" and "nan" carry an 'n')
    return s;
}

std::string script_value::to_string() const {
    // Special handling for references to show what they point to
    if (current_type() == script_value_type::jai_reference_type) {
        return deref().to_string();
    }

    switch (current_type()) {
        case script_value_type::jai_null_type:
            return "null";
        case script_value_type::jai_int_type:
            return std::to_string(as_int());
        case script_value_type::jai_float_type:
            return float_text(as_float());
        case script_value_type::jai_string_type:
            return as_string();
        case script_value_type::jai_utf8_string_type:
            return unchecked_as_utf8().bytes;
        case script_value_type::jai_vec2_type:
        case script_value_type::jai_vec3_type:
        case script_value_type::jai_vec4_type:
        case script_value_type::jai_color_type:
        case script_value_type::jai_ivec2_type:
        case script_value_type::jai_ivec3_type:
            return detail::math::is_kind(raw_storage_index()) ? detail::math::text(*this) : std::string("[unknown]");
        case script_value_type::jai_char_type:
            return std::string(1, as_char());
        case script_value_type::jai_bool_type:
            return as_bool() ? "true" : "false";
        case script_value_type::jai_array_type:
        case script_value_type::jai_map_type:
        case script_value_type::jai_object_type:
        case script_value_type::jai_function_type:
        case script_value_type::jai_weak_ptr_type:
            return detail::value_text(*this, {}).value();   // the structural form: no class's to_string() runs here (engine::text_of runs them)
        default:
            return "[unknown]";
    }
}

const script_value& script_value::deref_slow() const {
    // Bind a reference to the held strong_ptr (do NOT copy it): copying bumps and
    // then drops the (non-atomic) refcount on every deref, which is pure overhead
    // on this hot path.
    const auto& refHolder = storage_.get<strong_ptr<reference_holder>>();
    if (refHolder->has_cell) {
        // Cell reference: the holder owns the value - always alive, no liveness check
        return refHolder->cell()->deref();
    }
    if (refHolder->has_map_key) {
        // Map-entry reference: re-resolve via find(key) (pinned map, erase-safe)
        script_value* entry_value = refHolder->map_entry_value();
        if (!entry_value) {
            throw runtime_error("Reference to a removed map entry");
        }
        return entry_value->deref();
    }
    if (refHolder->container) {
        // Vector-element reference: recompute the element address from container+index
        // each time so it survives reallocation (push), and bounds-check so a shrink
        // (pop/erase/clear) throws instead of reading freed memory (#41).
        if (refHolder->container_index >= refHolder->container->size()) {
            throw runtime_error("Reference to a removed array element");
        }
        if (refHolder->container->is_typed()) {
            return refHolder->materialize_typed_element(refHolder->owner);
        }
        return refHolder->container->values()[refHolder->container_index].deref();
    }
    if (refHolder->owner_instance) {
        // Field reference: re-resolve by id (no lazy default insert - the pinned
        // instance IS the lifetime)
        const script_value* field_value = refHolder->field_cell();
        if (!field_value) {
            throw runtime_error("Reference to a removed field");
        }
        return field_value->deref();
    }
    if (refHolder->property) {
        // CPP-MEMBER: the live bound box (readers decode index 14 per touch);
        // PROPERTY: the getter result, refreshed on every touch
        return refHolder->property->live_member ? *refHolder->cell() : refHolder->materialize_property(refHolder->owner);
    }
    if (refHolder->string_base) {
        return refHolder->materialize_string_element(refHolder->owner);
    }
    if (refHolder->component_base) {
        return refHolder->materialize_component(refHolder->owner);
    }
    throw runtime_error("Null reference");   // unreachable: every factory sets a mode
}

// A typed element, a property, a string element or a component hands back a materialized VIEW: writers go through
// assign_through / commit_scratch and the element, string and component stores, never through this deref
script_value& script_value::deref_slow() {
    return const_cast<script_value&>(std::as_const(*this).deref_slow());
}

// A reference's store: the target its holder resolves to takes the value (moved where the target is plain storage)
template<typename V>
void script_value::assign_through_reference(V&& value) {
    auto refHolder = storage_.get<strong_ptr<reference_holder>>();
    if (refHolder->has_cell) {
        if (refHolder->cell()->is_cpp_bound_primitive()) { refHolder->cell()->assign_through(value); return; }   // a boxed host-bound variable: the C++ memory
        *refHolder->cell() = std::forward<V>(value);
        return;
    }
    if (refHolder->has_map_key) {
        script_value* entry_value = refHolder->map_entry_value();
        if (!entry_value) {
            throw runtime_error("Assignment to a removed map entry");
        }
        *entry_value = std::forward<V>(value);
        return;
    }
    if (refHolder->container) {
        // Vector-element reference: resolve fresh + bounds-check before writing,
        // so a write-through after a reallocation/shrink can't corrupt the heap (#41).
        if (refHolder->container_index >= refHolder->container->size()) {
            throw runtime_error("Assignment to a removed array element");
        }
        if (refHolder->container->is_typed()) {
            // a host's store converts as a script element store does (a script store arrives converted)
            engine* eng = payload_engine();
            auto converted = detail::convert_container_element(eng, value, refHolder->container_element_type);
            if (!converted) { throw runtime_error(eng ? format_error(converted, *detail::engine_access::get_symbolizer(*eng)) : std::string(converted.message())); }
            refHolder->container->set(refHolder->container_index, std::move(converted.value()));
            return;
        }
        refHolder->container->values()[refHolder->container_index] = std::forward<V>(value);
        return;
    }
    if (refHolder->owner_instance) {
        script_value* field_value = refHolder->field_cell();
        if (!field_value) {
            throw runtime_error("Reference to a removed field");
        }
        *field_value = std::forward<V>(value);
        return;
    }
    if (refHolder->property) {
        // CPP-MEMBER: the box's typed memory write (never a memberwise replace, which
        // would detach the box); PROPERTY: the setter
        if (refHolder->property->referent) {
            refHolder->store_referent(value, refHolder->owner);
        } else if (refHolder->property->live_member) {
            refHolder->cell()->assign_through(value);
        } else {
            refHolder->store_property(value, refHolder->owner);
        }
        return;
    }
    if (refHolder->string_base) {
        auto stored = detail::store_string_element(*refHolder, value, refHolder->owner);
        if (!stored) {
            throw runtime_error(format_error(stored, *detail::engine_access::get_symbolizer(*refHolder->owner)));
        }
        return;
    }
    if (refHolder->component_base) {
        refHolder->store_component(value, refHolder->owner);
        return;
    }
    throw runtime_error("Null reference in assign_through");   // unreachable: every factory sets a mode
}

void script_value::assign_through(const script_value& value) {
    if (type() == script_value_type::jai_reference_type) {
        assign_through_reference(value);
    } else if (is_cpp_bound()) {
        void* boundPtr = bound_target_ptr();
        const uint8_t sizeAndSign = bound_size_and_sign();
        switch (type()) {
            case script_value_type::jai_int_type: {
                auto v = value.as<script_int>();
                const uint8_t size = sizeAndSign & 0x7F;
                if (sizeAndSign & 0x80) {
                    switch (size) {
                        case 1: *static_cast<uint8_t*>(boundPtr) = static_cast<uint8_t>(v); break;
                        case 2: *static_cast<uint16_t*>(boundPtr) = static_cast<uint16_t>(v); break;
                        case 4: *static_cast<uint32_t*>(boundPtr) = static_cast<uint32_t>(v); break;
                        default: *static_cast<uint64_t*>(boundPtr) = static_cast<uint64_t>(v); break;
                    }
                } else {
                    switch (size) {
                        case 1: *static_cast<int8_t*>(boundPtr) = static_cast<int8_t>(v); break;
                        case 2: *static_cast<int16_t*>(boundPtr) = static_cast<int16_t>(v); break;
                        case 4: *static_cast<int32_t*>(boundPtr) = static_cast<int32_t>(v); break;
                        default: *static_cast<script_int*>(boundPtr) = v; break;
                    }
                }
                break;
            }
            case script_value_type::jai_float_type:
                if ((sizeAndSign & 0x7F) == sizeof(float))
                    *static_cast<float*>(boundPtr) = static_cast<float>(value.as<script_float>());
                else
                    *static_cast<script_float*>(boundPtr) = value.as<script_float>();
                break;
            case script_value_type::jai_string_type:
                *static_cast<std::string*>(boundPtr) = value.as<script_string>();
                break;
            case script_value_type::jai_bool_type:
                *static_cast<bool*>(boundPtr) = value.as<script_bool>();
                break;
            case script_value_type::jai_char_type:
                *static_cast<char*>(boundPtr) = value.as<script_char>();
                break;
            default:   // an opaque host pointer: nothing script can store converts into it
                throw runtime_error("Cannot assign through an opaque host pointer");
        }
    } else {
        *this = value;
    }
}

void script_value::assign_through(script_value&& value) {
    if (type() == script_value_type::jai_reference_type) {
        assign_through_reference(std::move(value));
    } else if (is_cpp_bound()) {
        // can't move into a C++ variable — fall back to copy
        assign_through(value);
    } else {
        *this = std::move(value);
    }
}

bool script_value::operator==(const script_value& other) const {
    const script_value& lhs = deref();
    const script_value& rhs = other.deref();

    if (lhs.type() != rhs.type()) {
        return lhs.char_beside_string(rhs) && lhs.key_text() == rhs.key_text();
    }

    switch (lhs.type()) {
        case script_value_type::jai_null_type:
            return true;
        case script_value_type::jai_int_type:
            return lhs.unchecked_as_int() == rhs.unchecked_as_int();
        case script_value_type::jai_float_type:
            return lhs.unchecked_as_float() == rhs.unchecked_as_float();
        case script_value_type::jai_string_type:
            return lhs.unchecked_as_string() == rhs.unchecked_as_string();
        case script_value_type::jai_utf8_string_type:
            return lhs.unchecked_as_utf8().bytes == rhs.unchecked_as_utf8().bytes;
        case script_value_type::jai_char_type:
            return lhs.unchecked_as_char() == rhs.unchecked_as_char();
        case script_value_type::jai_bool_type:
            return lhs.unchecked_as_bool() == rhs.unchecked_as_bool();
        case script_value_type::jai_vec2_type:
        case script_value_type::jai_vec3_type:
        case script_value_type::jai_vec4_type:
        case script_value_type::jai_color_type:
        case script_value_type::jai_ivec2_type:
        case script_value_type::jai_ivec3_type:
            return lhs.raw_storage_index() == rhs.raw_storage_index() && detail::math::is_kind(lhs.raw_storage_index()) &&
                   detail::math::same_kind_equal(lhs, rhs);
        default:
            return (lhs <=> rhs) == 0;
    }
}

// Transparent map-key ordering: MUST mirror operator<=> below exactly — type rank
// first (a probe ranks as jai_string_type), then string content (std::string <=>
// string_view is the same lexicographic order as string <=> string).
bool script_value_map_less::operator()(const script_value& a, const map_string_key_probe& p) const {
    if (auto cmp = a.type() <=> script_value_type::jai_string_type; cmp != 0) {
        return a.type() == script_value_type::jai_char_type ? a.key_text() < p.text : cmp < 0;
    }
    return a.unchecked_as_string() < p.text;
}
bool script_value_map_less::operator()(const map_string_key_probe& p, const script_value& b) const {
    if (auto cmp = script_value_type::jai_string_type <=> b.type(); cmp != 0) {
        return b.type() == script_value_type::jai_char_type ? p.text < b.key_text() : cmp < 0;
    }
    return p.text < b.unchecked_as_string();
}

std::strong_ordering script_value::operator<=>(const script_value& other) const {
    if (auto cmp = type() <=> other.type(); cmp != 0) {
        // a char key is its one-character string (the two ranks are adjacent, so no other kind sorts between them)
        if (char_beside_string(other)) { return key_text() <=> other.key_text(); }
        return cmp;
    }
    switch (type()) {
        case script_value_type::jai_null_type:
            return std::strong_ordering::equal; // All nulls are equal
        case script_value_type::jai_int_type:
            return unchecked_as_int() <=> other.unchecked_as_int();
        case script_value_type::jai_float_type:
            // a total order (IEEE totalOrder over the canonical key): every NaN one key after +inf, 0.0 and -0.0 one key
            return std::strong_order(detail::canonical_key_float(unchecked_as_float()), detail::canonical_key_float(other.unchecked_as_float()));
        case script_value_type::jai_string_type:
            return unchecked_as_string() <=> other.unchecked_as_string();
        case script_value_type::jai_utf8_string_type:
            return unchecked_as_utf8().bytes <=> other.unchecked_as_utf8().bytes;
        case script_value_type::jai_char_type:
            // char is unsigned 0..255 by spec (char_promotion.hpp); ordering here must
            // agree with promoted script-level comparisons, not platform char signedness
            return static_cast<unsigned char>(unchecked_as_char()) <=> static_cast<unsigned char>(other.unchecked_as_char());
        case script_value_type::jai_bool_type:
            return unchecked_as_bool() <=> other.unchecked_as_bool();
        case script_value_type::jai_vec2_type:
        case script_value_type::jai_vec3_type:
        case script_value_type::jai_vec4_type:
        case script_value_type::jai_color_type:
        case script_value_type::jai_ivec2_type:
        case script_value_type::jai_ivec3_type:
            if (raw_storage_index() == other.raw_storage_index() && detail::math::is_kind(raw_storage_index())) {
                return detail::math::same_kind_order(*this, other);
            }
            return raw_storage_index() <=> other.raw_storage_index();
        default: {
            // Heavy kinds order by the held object's address, the value's stable identity (std::less: a total order over pointers)
            auto identity = [](const script_value& v) -> const void* {
                switch (v.raw_storage_index()) {
                    case TYPEID_ARRAY:      return v.storage_.get_if<TYPEID_ARRAY>()->get();
                    case TYPEID_MAP:        return v.storage_.get_if<TYPEID_MAP>()->get();
                    case TYPEID_OBJECT:     return v.storage_.get_if<TYPEID_OBJECT>()->get();
                    case TYPEID_FUNCTION:   return v.storage_.get_if<TYPEID_FUNCTION>()->get();
                    case TYPEID_REFERENCE:  return v.storage_.get_if<TYPEID_REFERENCE>()->get();
                    case TYPEID_WEAK_PTR:   return v.storage_.get_if<TYPEID_WEAK_PTR>()->owner_identity();
                    case TYPEID_PARALLEL_BORROW: return v.storage_.get_if<TYPEID_PARALLEL_BORROW>()->pointer();  // viewed container = stable identity (region-internal, defensive)
                    default:                return nullptr;  // invalid values form one equivalence class
                }
            };
            const void* lp = identity(*this);
            const void* rp = identity(other);
            std::less<const void*> ptr_less;
            if (ptr_less(lp, rp)) return std::strong_ordering::less;
            if (ptr_less(rp, lp)) return std::strong_ordering::greater;
            return std::strong_ordering::equal;
        }
    }
}


// The map-key hash (core/script_map.hpp): keys operator<=> calls equivalent hash alike. The type rank seeds it, then the payload
// operator<=> compares: ints by value, a char as its one-character string, floats by their canonical key (every NaN one, both zeros one), strings and utf8_strings by bytes, bools, and every heavy kind by the identity operator<=> orders by.
uint64_t script_value::map_key_hash() const noexcept {
    const script_value_type t = type();
    const uint64_t seed = static_cast<uint64_t>(t) * 0x9E3779B97F4A7C15ull;
    auto mix = [t](uint64_t x) { return map_key_mix(t, x); };
    switch (t) {
        case script_value_type::jai_null_type: return mix(0);
        case script_value_type::jai_int_type: return mix(static_cast<uint64_t>(unchecked_as_int()));
        case script_value_type::jai_float_type: {
            const double d = detail::canonical_key_float(unchecked_as_float());
            uint64_t bits; std::memcpy(&bits, &d, sizeof(bits));
            return mix(bits);
        }
        case script_value_type::jai_string_type: return map_string_key_hash(unchecked_as_string());
        case script_value_type::jai_utf8_string_type: return detail::text_hash(unchecked_as_utf8().bytes) ^ seed;
        case script_value_type::jai_char_type: return map_string_key_hash(key_text());
        case script_value_type::jai_bool_type: return mix(unchecked_as_bool() ? 1 : 0);
        case script_value_type::jai_vec2_type:
        case script_value_type::jai_vec3_type:
        case script_value_type::jai_vec4_type:
        case script_value_type::jai_color_type:
        case script_value_type::jai_ivec2_type:
        case script_value_type::jai_ivec3_type:
            return mix(detail::math::is_kind(raw_storage_index()) ? detail::math::hash_bits(*this) : 0);
        default: {
            const void* identity = nullptr;   // KEEP IN STEP with operator<=>'s identity rows
            switch (raw_storage_index()) {
                case TYPEID_ARRAY:      identity = storage_.get_if<TYPEID_ARRAY>()->get(); break;
                case TYPEID_MAP:        identity = storage_.get_if<TYPEID_MAP>()->get(); break;
                case TYPEID_OBJECT:     identity = storage_.get_if<TYPEID_OBJECT>()->get(); break;
                case TYPEID_FUNCTION:   identity = storage_.get_if<TYPEID_FUNCTION>()->get(); break;
                case TYPEID_REFERENCE:  identity = storage_.get_if<TYPEID_REFERENCE>()->get(); break;
                case TYPEID_WEAK_PTR:   identity = storage_.get_if<TYPEID_WEAK_PTR>()->owner_identity(); break;
                case TYPEID_PARALLEL_BORROW: identity = storage_.get_if<TYPEID_PARALLEL_BORROW>()->pointer(); break;
                default: break;
            }
            return mix(reinterpret_cast<uintptr_t>(identity));
        }
    }
}


// ---------------------------------------------------------------------------------------------------------------------------
// Class value map keys (core/script_map.hpp). A class keys its values by its own operators: ordered through < or <=> (two
// keys are one when neither is less), else hashed through hash() with == (identity when it declares no ==), else by identity
// with a warning. The class's operators run on the calling thread's backend; one that fails raises out of the map operation,
// which leaves the map unchanged.
namespace {

	enum class key_policy : uint8_t { ordered, hashed, identity };

	const class_definition* by_value_object_class(const script_value& key) {
		if (key.type() != script_value_type::jai_object_type) { return nullptr; }
		const auto* holder = key.peek_object_holder();
		if (!holder) { return nullptr; }
		if (holder->is_instance()) {
			return holder->holds_object() ? holder->unchecked_instance()->get_class_definition() : nullptr;
		}
		engine* eng = key.payload_engine();
		if (!eng) { return nullptr; }
		if (holder->type_id() != UINT64_MAX) { return detail::engine_access::get_class_definition(*eng, holder->type_id()).get(); }
		return holder->type_name().empty() ? nullptr : detail::engine_access::get_class_definition(*eng, std::string(holder->type_name())).get();
	}

	key_policy policy_of(const class_definition* cls, const detail::engine_access::map_key_ids& ids) {
		if (cls->defines_method(ids.less) || cls->defines_method(ids.spaceship)) { return key_policy::ordered; }
		if (cls->defines_method(ids.hash)) { return key_policy::hashed; }
		return key_policy::identity;
	}

	execution_backend& key_backend(const script_value& key) {
		engine* eng = key.payload_engine();
		execution_backend* backend = eng ? detail::engine_access::calling_backend(*eng) : nullptr;
		if (!backend) { throw runtime_error("A class map key's operators need the engine that made the key"); }
		return *backend;
	}

	[[noreturn]] JAI_NOINLINE void raise_key_failure(const script_value& key, const error_propagator& error) {
		execution_backend& backend = key_backend(key);
		if (backend.is_unwinding()) { throw script_exception(backend.get_current_exception()); }
		throw_checked_error(key.payload_engine(), error.static_msg, error.symbol_id, error.symbol_id2);
	}

	bool key_less(const script_value& a, const script_value& b) {
		execution_backend& backend = key_backend(a);
		detail::engine_access::run_pin pin(a.payload_engine());   // a map's key order is the map's callback
		auto answer = backend.language_less(a, b);
		if (!answer || backend.is_unwinding()) [[unlikely]] { raise_key_failure(a, answer ? error_propagator{} : answer.error_value()); }
		return answer.value();
	}

	bool key_equal(const script_value& a, const script_value& b, const class_definition* cls, const detail::engine_access::map_key_ids& ids) {
		if (!cls->defines_method(ids.equal)) { return (a <=> b) == 0; }
		execution_backend& backend = key_backend(a);
		detail::engine_access::run_pin pin(a.payload_engine());
		auto answer = backend.language_equal(a, b);
		if (!answer || backend.is_unwinding()) [[unlikely]] { raise_key_failure(a, answer ? error_propagator{} : answer.error_value()); }
		return answer.value();
	}

	JAI_NOINLINE script_int key_hash_value(const script_value& key, const detail::engine_access::map_key_ids& ids) {
		execution_backend& backend = key_backend(key);
		detail::engine_access::run_pin pin(key.payload_engine());
		auto answer = backend.invoke_method(key, ids.hash);
		if (!answer || backend.is_unwinding()) [[unlikely]] { raise_key_failure(key, answer ? error_propagator{} : answer.error_value()); }
		const script_value& value = answer.value().deref();
		if (!value.is_int()) {
			throw runtime_error("hash() of a map key must return an int");
		}
		return value.unchecked_as_int();
	}

	const detail::engine_access::map_key_ids& key_ids(const script_value& key) { return detail::engine_access::map_key_operator_ids(*key.payload_engine()); }

}

const class_definition* map_class_key_class(const script_value& key) {
	const class_definition* cls = by_value_object_class(key);
	if (!cls) { return nullptr; }
	const key_policy policy = policy_of(cls, key_ids(key));
	if (policy != key_policy::identity) { return cls; }
	if (cls->first_identity_key()) {
		detail::engine_access::report_script_warning(*key.payload_engine(), "map keys of class '" + cls->get_name() +
			"' compare by identity: declare operator<=> (or operator<) or hash() to key its values by value");
	}
	return nullptr;
}

struct script_map::class_key_index {
	struct run {
		const class_definition* cls = nullptr;
		uint64_t epoch = 0;              // the class's method epoch the run was ordered under
		bool hashed = false;
		std::vector<entry*> keys;        // by the class's order, or by hash() value
		std::vector<script_int> hashes;  // a hashed run's hash() values, beside its keys
	};
	std::vector<run> runs;
};

void script_map::class_key_index_deleter::operator()(class_key_index* index) const noexcept { delete index; }

// A class whose operators changed since its run was ordered (a reload) re-derives the run under its current policy: re-sorted
// or re-hashed, or dropped when the class keys by identity now (its keys sit in the index under their identity already). Keys
// the new order calls one key become the first of them in that order, as an insert finds it; the others leave the map.
void script_map::refresh_class_runs() const {
	if (!class_keys_) { return; }
	auto& runs = class_keys_->runs;
	for (size_t r = 0; r < runs.size();) {
		class_key_index::run& run = runs[r];
		if (run.epoch == run.cls->method_epoch()) { ++r; continue; }
		const detail::engine_access::map_key_ids& ids = key_ids(run.keys.front()->kv.first);
		const key_policy policy = policy_of(run.cls, ids);
		if (policy == key_policy::identity) {
			runs.erase(runs.begin() + static_cast<ptrdiff_t>(r));
			const_cast<script_map&>(*this).changed();
			continue;
		}
		const bool hashed = policy == key_policy::hashed;
		std::vector<entry*> keys = run.keys;
		std::vector<script_int> hashes;
		if (hashed) {
			std::vector<std::pair<script_int, entry*>> by_hash;
			by_hash.reserve(keys.size());
			for (entry* e : keys) { by_hash.emplace_back(key_hash_value(e->kv.first, ids), e); }
			std::stable_sort(by_hash.begin(), by_hash.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			for (size_t i = 0; i < by_hash.size(); ++i) { hashes.push_back(by_hash[i].first); keys[i] = by_hash[i].second; }
		} else {
			std::stable_sort(keys.begin(), keys.end(), [](const entry* a, const entry* b) { return key_less(a->kv.first, b->kv.first); });
		}
		std::vector<entry*> kept;
		std::vector<script_int> kept_hashes;
		std::vector<entry*> merged;
		for (size_t i = 0; i < keys.size(); ++i) {
			bool same = false;
			if (!hashed) {
				same = !kept.empty() && !key_less(kept.back()->kv.first, keys[i]->kv.first);
			} else {
				for (size_t k = kept.size(); k-- > 0 && kept_hashes[k] == hashes[i];) {
					if (key_equal(kept[k]->kv.first, keys[i]->kv.first, run.cls, ids)) { same = true; break; }
				}
			}
			if (same) { merged.push_back(keys[i]); continue; }
			kept.push_back(keys[i]);
			if (hashed) { kept_hashes.push_back(hashes[i]); }
		}
		run.hashed = hashed;
		run.keys = std::move(kept);
		run.hashes = std::move(kept_hashes);
		run.epoch = run.cls->method_epoch();
		auto& self = const_cast<script_map&>(*this);
		for (entry* e : merged) { self.unlink_entry(e); }
		self.changed();
		for (entry* e : merged) { self.pool_.destroy(e); }
		++r;
	}
	if (runs.empty()) { const_cast<script_map&>(*this).class_keys_.reset(); }
}

script_map::class_probe script_map::probe_class(const script_value& key, const class_definition* cls) const {
	refresh_class_runs();
	class_probe at;
	at.hash = script_value_hash{}(key);
	const detail::engine_access::map_key_ids& ids = key_ids(key);
	const bool hashed = policy_of(cls, ids) == key_policy::hashed;
	if (hashed) { at.key_hash = key_hash_value(key, ids); }
	if (!class_keys_) { return at; }
	auto& runs = class_keys_->runs;
	at.run = runs.size();
	for (size_t r = 0; r < runs.size(); ++r) { if (runs[r].cls == cls) { at.run = r; break; } }
	if (at.run == runs.size()) { return at; }
	const auto& run = runs[at.run];
	if (!hashed) {
		size_t lo = 0, hi = run.keys.size();
		while (lo < hi) {
			const size_t mid = lo + (hi - lo) / 2;
			if (key_less(run.keys[mid]->kv.first, key)) { lo = mid + 1; } else { hi = mid; }
		}
		at.at = lo;
		if (lo < run.keys.size() && !key_less(key, run.keys[lo]->kv.first)) { at.found = run.keys[lo]; }
		return at;
	}
	const auto first = std::lower_bound(run.hashes.begin(), run.hashes.end(), at.key_hash);
	size_t i = static_cast<size_t>(first - run.hashes.begin());
	for (; i < run.hashes.size() && run.hashes[i] == at.key_hash; ++i) {
		if (key_equal(run.keys[i]->kv.first, key, cls, ids)) { at.found = run.keys[i]; at.at = i; return at; }
	}
	at.at = i;
	return at;
}

void script_map::insert_class_entry(const class_definition* cls, const class_probe& at, entry* e) {
	if (!class_keys_) { class_keys_.reset(new class_key_index()); }
	auto& runs = class_keys_->runs;
	if (at.run == runs.size()) {
		class_key_index::run fresh;
		fresh.cls = cls;
		fresh.epoch = cls->method_epoch();
		fresh.hashed = policy_of(cls, key_ids(e->kv.first)) == key_policy::hashed;
		runs.push_back(std::move(fresh));
	}
	auto& run = runs[at.run];
	run.keys.insert(run.keys.begin() + static_cast<ptrdiff_t>(at.at), e);
	if (run.hashed) { run.hashes.insert(run.hashes.begin() + static_cast<ptrdiff_t>(at.at), at.key_hash); }
}

// at: the probe that found the entry, its run and its place in the run
void script_map::erase_class_entry(const class_probe& at) noexcept {
	auto& runs = class_keys_->runs;
	auto& run = runs[at.run];
	run.keys.erase(run.keys.begin() + static_cast<ptrdiff_t>(at.at));
	if (run.hashed) { run.hashes.erase(run.hashes.begin() + static_cast<ptrdiff_t>(at.at)); }
	if (run.keys.empty()) { runs.erase(runs.begin() + static_cast<ptrdiff_t>(at.run)); }
	if (runs.empty()) { class_keys_.reset(); }
}

void script_map::copy_class_keys(const script_map& o, const std::vector<std::pair<const entry*, entry*>>& copies) {
	std::unordered_map<const entry*, entry*> to_copy(copies.begin(), copies.end());
	class_keys_.reset(new class_key_index(*o.class_keys_));
	for (auto& run : class_keys_->runs) {
		for (auto& e : run.keys) { e = to_copy.at(e); }
	}
}

namespace {
	// the order of class keys among keys: by class name (then the class's identity), each class's keys in its run's order;
	// ahead of the other values of the object type
	bool class_run_before(const class_definition* a, const class_definition* b) {
		if (a->get_name() != b->get_name()) { return a->get_name() < b->get_name(); }
		return std::less<const class_definition*>{}(a, b);
	}
}

void script_map::build_order_with_class_keys() const {
	refresh_class_runs();
	if (!class_keys_) { build_order(); return; }
	if (order_gen_ == gen_) { return; }
	order_.clear();
	order_.reserve(size_);
	for (const slot& s : slots_) { if (s.e) order_.push_back(&s.e->kv); }
	std::vector<const class_key_index::run*> runs;
	for (const auto& run : class_keys_->runs) { runs.push_back(&run); }
	std::sort(runs.begin(), runs.end(), [](const auto* a, const auto* b) { return class_run_before(a->cls, b->cls); });
	std::unordered_map<const value_type*, std::pair<size_t, size_t>> rank;
	for (size_t r = 0; r < runs.size(); ++r) {
		for (size_t i = 0; i < runs[r]->keys.size(); ++i) { rank.emplace(&runs[r]->keys[i]->kv, std::pair<size_t, size_t>{r, i}); }
	}
	std::sort(order_.begin(), order_.end(), [&rank](const value_type* a, const value_type* b) {
		const auto ra = rank.find(a);
		const auto rb = rank.find(b);
		const bool ca = ra != rank.end(), cb = rb != rank.end();
		if (ca && cb) { return ra->second < rb->second; }
		if (ca || cb) {
			const script_value_type ta = a->first.type(), tb = b->first.type();
			if (ta != tb) { return ta < tb; }
			return ca;
		}
		return script_value_map_less{}(a->first, b->first);
	});
	order_gen_ = gen_;
}

// build_order's order from what the map holds (a class key by its run, any other key by operator<=>, which is what the
// key order gives every key outside a run), so a dying map orders its entries without its keys' operators or engine
void script_map::order_by_key(std::vector<entry*>& entries) const noexcept {
	std::unordered_map<const entry*, std::pair<size_t, size_t>> rank;
	if (class_keys_) {
		std::vector<const class_key_index::run*> runs;
		for (const auto& run : class_keys_->runs) { runs.push_back(&run); }
		std::sort(runs.begin(), runs.end(), [](const auto* a, const auto* b) { return class_run_before(a->cls, b->cls); });
		for (size_t r = 0; r < runs.size(); ++r) {
			for (size_t i = 0; i < runs[r]->keys.size(); ++i) { rank.emplace(runs[r]->keys[i], std::pair<size_t, size_t>{r, i}); }
		}
	}
	std::sort(entries.begin(), entries.end(), [&rank](const entry* a, const entry* b) {
		const auto ra = rank.find(a);
		const auto rb = rank.find(b);
		const bool ca = ra != rank.end(), cb = rb != rank.end();
		if (ca && cb) { return ra->second < rb->second; }
		if (ca || cb) {
			const script_value_type ta = a->kv.first.type(), tb = b->kv.first.type();
			if (ta != tb) { return ta < tb; }
			return ca;
		}
		return (a->kv.first <=> b->kv.first) < 0;
	});
}

// The kinds ranked below objects come first in the order, then the class runs by class_run_before, then the other objects:
// the position after key is the runs ahead of its class's plus its place in its own run
size_t script_map::index_after_class_key(const script_value& key, const class_definition* cls) const {
	const detail::engine_access::map_key_ids& ids = key_ids(key);
	const bool hashed = policy_of(cls, ids) == key_policy::hashed;
	const script_int key_hash = hashed ? key_hash_value(key, ids) : 0;
	const auto objects = std::partition_point(order_.begin(), order_.end(),
		[](const value_type* e) { return e->first.type() < script_value_type::jai_object_type; });
	size_t result = static_cast<size_t>(objects - order_.begin());
	if (!class_keys_) { return result; }
	const class_key_index::run* own = nullptr;
	for (const auto& run : class_keys_->runs) {
		if (run.cls == cls) { own = &run; } else if (class_run_before(run.cls, cls)) { result += run.keys.size(); }
	}
	if (!own) { return result; }
	if (hashed) { return result + static_cast<size_t>(std::upper_bound(own->hashes.begin(), own->hashes.end(), key_hash) - own->hashes.begin()); }
	return result + static_cast<size_t>(std::partition_point(own->keys.begin(), own->keys.end(),
		[&](const entry* e) { return !key_less(key, e->kv.first); }) - own->keys.begin());
}

int map_class_key_order(const script_value& a, const script_value& b) {
	const auto identity = [&]() { const auto c = a <=> b; return c < 0 ? -1 : c > 0 ? 1 : 0; };
	if (a.type() != b.type()) { return identity(); }
	const class_definition* ca = map_class_key_class(a);
	const class_definition* cb = map_class_key_class(b);
	if (!ca && !cb) { return identity(); }
	if (!ca || !cb) { return ca ? -1 : 1; }
	if (ca != cb) { return class_run_before(ca, cb) ? -1 : 1; }
	const detail::engine_access::map_key_ids& ids = key_ids(a);
	if (policy_of(ca, ids) == key_policy::ordered) {
		if (key_less(a, b)) { return -1; }
		return key_less(b, a) ? 1 : 0;
	}
	const script_int ha = key_hash_value(a, ids), hb = key_hash_value(b, ids);
	if (ha != hb) { return ha < hb ? -1 : 1; }
	return key_equal(a, b, ca, ids) ? 0 : identity();
}

} // namespace jai

namespace jai::detail {

float_text_result parse_float_text(const char* text, size_t length) noexcept {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    const char* const last = text + length;
    const char* p = text + (length != 0 && text[0] == '-');
    const bool hex = p + 1 < last && p[0] == '0' && (p[1] == 'x' || p[1] == 'X');
    const bool digit_first = p < last && p[0] >= '0' && p[0] <= '9';
    const bool point_first = p + 1 < last && p[0] == '.' && p[1] >= '0' && p[1] <= '9';
    if ((digit_first && !hex) || point_first) {
        double value = 0.0;
        const auto [end, ec] = std::from_chars(text, last, value);
        if (ec == std::errc{}) { return { value, end, false }; }
    }
#else
    (void)length;   // libc++ ships integer-only std::from_chars
#endif
    return parse_float_text_strtod(text);
}

} // namespace jai::detail
