#pragma once

#ifndef __JAISCRIPT_CORE_CLASS_DEFINITION_HPP__
#define __JAISCRIPT_CORE_CLASS_DEFINITION_HPP__

// engine.hpp ends with bodies that need class_definition complete: entered from here, they wait for the end of this file
#ifndef __JAISCRIPT_CORE_ENGINE_HPP__
#define JAISCRIPT_ENGINE_BODIES_AFTER_CLASS_DEFINITION
#endif
#include "engine.hpp"
#include "native_cast.hpp"
#include "script_extensible.hpp"
#include <set>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <atomic>

namespace jai {

// Which overloads of a method jai::serial marks: the non-const ones, when only non-const overloads were bound serial (a const
// getter of the same name stays parallel), or every one, once a const overload was
enum class serial_overloads : uint8_t { non_const, every };

namespace detail {
void note_class_layout_change(engine* eng) noexcept;   // engine::bump_layout_generation for a class's engine (defined in engine.cpp)
struct fault_injection;
struct host_member_lane;
struct slot_site;
}

class function_decl;
class call_expr;
class environment;
class class_definition;
class class_instance;

namespace detail {
    // A copy of `source` built by the user copy constructor `level` declares (its class or a base), as C++ copy-constructs:
    // the constructor builds `level`'s object from the source, the copy takes the source's class and the levels below
    // `level` copy their fields. An inert instance (it never runs a destructor) stands for a copy whose constructor raised
    strong_ptr<class_instance> copy_construct_instance(const class_instance& source, const class_definition* level);
    // A copy of the value can be observed: an instance, element or entry whose class runs a destructor or a user copy
    // constructor (value_tree_copies_observably), so no copy of it is skipped unless C++ skips it
    bool value_copies_observably(const script_value& v);
    // A move of the value runs a user copy constructor: an instance whose class, or a by-value field's, declares one (C++ then
    // has no move constructor); a container moves its storage
    bool move_runs_copy_constructor(const script_value& v);
    // The call yields a class instance by value (a construction, or the checker's resolved callee declared to return a class):
    // a member read off its result is C++'s xvalue
    bool call_returns_class_value(const call_expr& call, engine& eng);
    struct release_action;
    // The allocation's deleter for a class whose chain declares a destructor: the chain runs, then the instance dies
    // (natively, or as a release-stack frame past the native depth)
    struct instance_deleter {
        std::shared_ptr<class_definition> def;
        void operator()(class_instance* ptr) const noexcept;
    };
    // The object block: a destructor class's pin on its definition, the instance's strong_ptr
    // control block and instance (an object value's holder itself), then the cells. The strong family's last weak reference
    // frees the block through one of two deallocators, and which one it is says whether the pin is there.
    struct object_block {
        static constexpr size_t pin_bytes = sizeof(std::shared_ptr<class_definition>);
        inline static void release(control_block_base* cb) noexcept;
        inline static void release_pinned(control_block_base* cb) noexcept;
        inline static std::shared_ptr<class_definition>* definition_pin(control_block_base* cb) noexcept;
    };
}

namespace detail {
    // Typed-slot conversion table (defined below class_definition): the ONE table for every
    // declared-type slot; the field store and the C++ set_field API enter through these.
    inline checked_result<script_value> field_store_convert(const script_value& value, const type_info_ptr& declared, engine* eng,
                                                            uint64_t field_id = 0, const class_definition* owner = nullptr,
                                                            bool is_static = false);
    inline std::optional<script_value> field_store_convert_no_script(const script_value& value, const type_info_ptr& declared, engine* eng);
    // Overload-miss text, defined below with the slot texts
    inline error_propagator overload_miss(std::error_code ec, uint64_t name_id, const script_value* args, size_t argc,
                                          engine* eng, bool constructor = false);
}

enum class access_level {
    public_access,
    private_access,
    protected_access
};

enum class delegation_type {
    none,
    same_class,
    base_class
};

class class_definition;

// aligned to 8 everywhere, so its control block sits one fixed distance before it (detail::k_instance_cb_offset); an object value
// points at it as its holder (script_value::object_holder at offset 0)
class alignas(8) class_instance : public script_value::object_holder {
public:
    // An instance in its own object block (detail::object_block), owned by strong_ptr; a destructor_owner arms the
    // instance's destructor chain and pins the definition until the instance dies
    inline static strong_ptr<class_instance> make(uint64_t type_id, engine* eng, uint32_t slots = 0,
                                                  class_definition* destructor_owner = nullptr);
    inline static strong_ptr<class_instance> make(std::string_view class_name, engine* eng);
    // This instance's own handle (std::enable_shared_from_this's shared_from_this): empty while it dies
    strong_ptr<class_instance> strong_from_this() noexcept { return detail::strong_from_object(this); }
    weaker_ptr<class_instance> weak_from_this() noexcept { return detail::weak_from_object(this); }
    strong_ptr<class_instance> strong_from_this() const noexcept { return const_cast<class_instance*>(this)->strong_from_this(); }
    weaker_ptr<class_instance> weak_from_this() const noexcept { return const_cast<class_instance*>(this)->weak_from_this(); }
    inline class_instance(std::string_view class_name, engine* eng = nullptr);
    struct interned_name_t {};
    // a name already in the engine's interned table (a class definition's cached id): no lookup
    class_instance(interned_name_t, uint64_t type_id, engine* eng) : object_holder(k_instance, eng) { type_id_ = type_id; }
    // The slot table and cells in the instance's own allocation (class_definition::allocate_instance): `slots` table
    // entries, then `slots` cells; a slot the class gains later takes a heap table and heap cells
    struct trailing_cells {
        std::byte* memory = nullptr;
        uint32_t slots = 0;
    };
    inline static size_t trailing_bytes(uint32_t slots) noexcept;
    inline class_instance(interned_name_t, uint64_t type_id, engine* eng, const trailing_cells& trailing);
    // An instance placed in its own raw allocation (a destructor class's, allocate_instance) dies through this; the
    // allocation goes with its control block (detail::block_head_allocator)
    inline static void free_allocation(class_instance* instance) noexcept;
    inline ~class_instance();

    // Enforcing write (C++ API surface): converts to the field's declared type like '='
    // on locals, throws runtime_error on an impossible conversion. Null passes through
    // (typed-null placeholders: create_instance defaults, uninitialized fields).
    // Defined out-of-line (needs class_definition + the conversion kernel).
    inline void set_field(uint64_t id, const script_value& value);
    // A field initializer's value lands as the declaration `T f = e;` would: null included (an int field refuses it, a
    // bool field reads it false), and the refusal throws as set_field's does
    inline void set_initialized_field(uint64_t id, const script_value& value);

    inline void set_field_unchecked(uint64_t id, const script_value& value);
    // create_instance's plan landing: fresh instance, no references, no overflow - one
    // cell copy per planned slot (materialize_field's slot arm without the hash)
    inline void seed_fields_from_plan(const std::vector<std::pair<uint32_t, script_value>>& plan, uint32_t slot_count, bool detach = false);
    bool seeded_from_plan() const { return seeded_from_plan_; }

    // Records this instance as its C++ object's script layer when its native object lands on a
    // script class deriving a bound C++ class: into the object's script_extensible slot
    // when its base registered one, else the engine's weak map. Called from set_native so
    // every path — binder-ctor super() in both backends, extend, archive load — registers
    // identically.
    inline void note_script_layer_owner(const script_value& value);

    // Script-write enforcement: declared-type fields convert like '=' on locals or error
    // with the locals-identical text; 'var' (any) fields pass through; undeclared ('auto')
    // fields infer from the current value's tag then enforce. Callers store the returned
    // value via set_field_unchecked. Defined out-of-line.
    inline checked_result<script_value> enforce_field_write(uint64_t id, script_value value) const;
    // A store into field id: a by-value class field holding an instance takes the source in place (the copy assignment or a
    // user operator=; the field keeps its object, which is the result); otherwise copy() makes the stored value and the
    // field's conversion runs (enforce_field_write). The source is the store's own operand, never a copy of it, so an lvalue
    // source runs no extra destructor
    // consumable: the store's own operand, which a converting constructor may take as its parameter
    inline std::optional<checked_result<void>> assign_field_in_place(uint64_t id, const script_value& source, script_value* consumable = nullptr) const;
    template <class Copy> checked_result<script_value> field_store(uint64_t id, const script_value& source, Copy&& copy, script_value* consumable = nullptr) const;
    // field_store for a field whose slot the caller holds (a site cache under the class's epoch): the same steps by the slot, no lookup
    // of the id; id names the field only in a conversion's error text
    template <class Copy> checked_result<script_value> field_store_at(uint32_t slot, uint64_t id, const script_value& source, Copy&& copy,
                                                                      script_value* consumable = nullptr) const;

    inline const script_value& get_field(uint64_t id, bool throw_if_missing = true) const;
    inline script_value& get_field(uint64_t id, bool throw_if_missing = true);

    inline bool has_field(uint64_t id) const;

    inline bool has_field_value(uint64_t id) const;
    // Runtime-added fields (a user field can shadow a method by name)
    inline bool has_user_overflow_fields() const;

    engine* get_engine() const { return owner_; }

    // Set while a script constructor has not completed. A throw out of it destroys only the levels whose constructors
    // completed (C++ [except.ctor]): the first `constructed_levels()` of its class's construction_order()
    void construction_pending(bool pending) { construction_pending_ = pending; }
    bool construction_pending() const { return construction_pending_; }
    static constexpr uint16_t k_nothing_constructed = UINT16_MAX;
    void constructed_levels(uint16_t levels) { constructed_levels_ = levels; }
    uint16_t constructed_levels() const { return constructed_levels_; }
    // A pending instance whose destruction runs some level's body: its class; null when no level completed
    const class_definition* constructed_level() const;
    bool level_constructed(const class_definition* level) const;

    // A retired layer (engine::script_layer_detach, or a move to another engine) keeps
    // owning its object so old handles never dangle, but every C++ extraction through it
    // throws instead of reaching an object another layer now speaks for.
    void retire_script_layer() { script_layer_retired_ = true; }
    bool script_layer_retired() const { return script_layer_retired_; }
    // Live field references pinned on this instance (make_field_reference / ~reference_holder).
    // Zero = no reference can alias a field: a parallel fork may treat the instance and its
    // value-semantic subtree as exclusive without the per-node use_count walk.
    uint32_t live_field_refs() const noexcept { return live_field_refs_; }
    void note_field_ref_minted() noexcept { ++live_field_refs_; }
    inline void note_field_ref_released() noexcept;
    [[noreturn]] inline void throw_retired_script_layer() const;

    // Field storage lookup WITHOUT lazy default insertion (field-reference resolution:
    // a reload-removed field must error, not silently resurrect as a default)
    script_value* find_field_value(uint64_t id);
    const script_value* find_field_value(uint64_t id) const;
    // A script field's own storage (what a save writes and a load restores); null for a C++ property's slot
    const script_value* find_script_field_value(uint64_t id) const;

    // Slot fast path (vm site ICs, tier-1 stage B): present-cell lookup by a
    // pre-resolved slot — no hash; absent (null cell / raw-invalid) returns null so
    // the caller falls back to the resolve ladder (chain defaults, errors).
    // One field's value taken out of its cell for a dying instance's member destruction; the cell stays, holding null
    inline script_value take_field_slot(uint32_t slot);

    inline script_value* field_slot_value(uint32_t slot);

    // Slot store (write-side IC): set_field_unchecked's exact semantics through a
    // pre-resolved slot — writes through a held reference, materializes absent cells.
    inline void field_slot_write(uint32_t slot, script_value&& v);

    std::string_view get_class_name() const { return type_name(); }

    // Hot-reload field migration; defined out-of-line (needs class_definition + the
    // conversion kernel for declared-type retypes).
    inline void migrate_fields(const std::set<uint64_t>& old_field_ids,
                       const std::unordered_map<uint64_t, script_value>& new_field_defaults,
                       bool preseeded = false);
    // The second half of a migration: fields the new definition no longer declares go (after the class's migrate hook read them)
    inline void drop_fields_absent_from(const std::unordered_map<uint64_t, script_value>& new_field_defaults);

    inline script_value get_method(uint64_t id, bool throw_if_missing = true) const;

    inline void set_class_definition(const std::shared_ptr<class_definition>& class_def);
    inline void set_class_definition(class_definition* class_def);

    // Re-class this instance in place (extend's re-derive, an assignment of another class): every field cell moves to the
    // new definition's slot for its id - cells never relocate, so captured field addresses
    // survive exactly as under hot reload - and ids the new class does not declare stay
    // reachable in overflow, or with drop_undeclared go absent (their cells retire holding the absent marker until the
    // instance dies). Every handle to this instance sees the new class; a chain with a destructor arms it.
    inline void reclass(const std::shared_ptr<class_definition>& class_def, bool drop_undeclared = false);
    // Armed when the instance was minted (the allocation's deleter runs the chain of its class at death)
    void note_destructor_armed() noexcept { destructor_armed_ = true; }
    // A chain that came to declare a destructor after the mint (a re-class, a reload adding one or a base with one) runs it at death
    inline void arm_late_destructor(const class_definition& cls);

    // Field iteration, slot order (first build ≈ declaration order, base-first) then
    // runtime-overflow entries; replaces get_fields()/get_fields_mutable() — flat
    // storage has no map to hand out. fn(uint64_t id, script_value& value).
    template <typename F> void for_each_field(F&& fn);
    template <typename F> void for_each_field(F&& fn) const;
    inline size_t field_value_count() const;

    class_definition* get_class_definition() const {
        return class_def_;
    }
    // C++ [class.cdtor]: the class whose constructor, field initializers or destructor body is running answers the
    // object's virtual calls, so construction and destruction point the instance at that level for the span
    enum class cdtor_phase : uint8_t { none, construction, destruction };
    inline void dispatch_as(class_definition* level, cdtor_phase phase);
    cdtor_phase dispatch_phase() const { return dispatch_phase_; }
    // The cells reordered from `from`'s slot layout into `to`'s; cells of fields `to` does not declare trail its slots in `from`'s
    // order, so the reverse call restores `from`'s layout. Cells never move, so references into them stay valid.
    inline void relayout_fields(const class_definition& from, const class_definition& to);

    bool is_script_class() const;
    bool is_cpp_class() const;

    inline std::shared_ptr<void> get_cpp_object() const;

	template<typename T>
	std::shared_ptr<T> get_cpp_object_as() const {
		if (script_layer_retired_) {
			throw_retired_script_layer();
		}
		auto holder = native_or_throw().get_object_holder();
		return holder ? std::static_pointer_cast<T>(detail::native_cast_or_throw(
			detail::checked_native_object(owner_, *holder, typeid(T)), owner_)) : nullptr;
	}

    inline bool has_cpp_object() const;

    // The C++ object's address (owning or not); null without one. A raw read - no handle copies - for member resolution.
    inline void* cpp_object_pointer() const;

    inline strong_ptr<class_instance> deep_copy() const;
    // One level of a deep copy (script_value::clone_level); past detail::k_inline_copy_depth its fields and then its end
    // (finish_deep_copy_level) are steps on the work list
    inline strong_ptr<class_instance> deep_copy_level(detail::deep_copy_work& work, unsigned depth) const;
    // The end of an instance's copy, after its fields: its C++ object, then the copy joins its class's live instances
    inline void finish_deep_copy_level(class_instance& copy, bool overflow) const;

    inline void copy_fields_from(const class_instance& other);

    // The C++ object behind the instance (a bound class's instance, a script class over a bound base, an extended
    // object): never a field, so no script name reaches it; the bound members reach it through the class
    const script_value* native() const { return extras_ && extras_->native ? &*extras_->native : nullptr; }
    inline const script_value& native_or_throw() const;
    inline void set_native(const script_value& value);
    // A worker's view of a shared object's C++ object: the C++ object's script layer stays the shared instance
    inline void set_native_view(const script_value& value);
    // The parallel region whose chunk made this instance's C++ object (0: none): a non-const C++ method runs on a parallel
    // chunk only over an object that chunk made
    inline void region_tag(uint32_t tag);
    uint32_t region_tag() const { return extras_ ? extras_->region_tag : 0; }
    inline bool native_lookup_enabled() const;

private:
    // The class name (the engine's interned text for the holder's type_id_, empty for an instance built without an engine)
    // and the engine are the holder's
    // Tier-1 flat fields: slot-indexed per class_definition's append-only registry.
    // unique_ptr pointees NEVER move (vector growth relocates the pointers only), so
    // cached field addresses — suspended coroutines' env chains, this-capturing
    // closures — survive resize AND hot-reload migration, matching the old map's
    // node-stability contract. null pointer OR raw-invalid pointee = ABSENT (raw
    // check, NOT is_invalid(): a reference field derefs). Runtime-added fields and
    // pre-definition writes (deserialization) live in the lazily-built overflow map;
    // set_class_definition re-homes them into slots.
    // One cell arena per instance: the instance allocation's trailing cells (allocate_instance), else a heap arena (in the
    // extras box) sized to the class's slot count when the definition attaches; slots the class gains afterwards (reload) get
    // heap cells. Either way a pointee never moves - reclass/migration move the POINTERS - so field_slot_value(slot) stays
    // valid until the instance dies or a reload/re-class re-seats its slots (the class pointer or the field-shape epoch
    // moves); the destructor destroys every cell before any member goes, so the arena outlives them.
    // An owning cell pointer in one word: bit 0 marks an arena cell (destroyed in place), clear a heap cell (deleted)
    class field_cell {
    public:
        field_cell() noexcept = default;
        field_cell(script_value* a_cell, bool a_heap) noexcept : bits(reinterpret_cast<uintptr_t>(a_cell) | (a_heap ? 0 : 1)) {}
        field_cell(field_cell&& a_other) noexcept : bits(a_other.bits) { a_other.bits = 0; }
        field_cell& operator=(field_cell&& a_other) noexcept {
            if (this != &a_other) {
                const uintptr_t replaced = bits;
                bits = a_other.bits;
                a_other.bits = 0;
                release_cell(replaced);
            }
            return *this;
        }
        field_cell(const field_cell&) = delete;
        field_cell& operator=(const field_cell&) = delete;
        ~field_cell() { release_cell(bits); }
        script_value* get() const noexcept { return reinterpret_cast<script_value*>(bits & ~uintptr_t{1}); }
        script_value& operator*() const noexcept { return *get(); }
        script_value* operator->() const noexcept { return get(); }
        explicit operator bool() const noexcept { return bits != 0; }
    private:
        static void release_cell(uintptr_t a_bits) noexcept {
            if (!a_bits) { return; }
            script_value* cell = reinterpret_cast<script_value*>(a_bits & ~uintptr_t{1});
            if (a_bits & 1) { cell->~script_value(); } else { delete cell; }
        }
        uintptr_t bits = 0;
    };
    // The slot table in one word: the trailing cells' count, low bit set, while implicit (slot i sits a fixed distance past the
    // instance's end, which is this word's end), else a heap table (size, capacity, one cell pointer per slot). Aligned as a cell, so
    // the word's end is the instance's end on 32-bit hosts too, where the word alone would leave tail padding after it
    class alignas(alignof(script_value)) field_table {
    public:
        field_table() = default;
        field_table(const field_table&) = delete;
        field_table& operator=(const field_table&) = delete;
        ~field_table() {
            clear();
            if (heap()) { ::operator delete(static_cast<void*>(header())); }
        }
        // The implicit table: slot i's cell is the i-th trailing cell, constructed (raw-invalid while
        // absent) for the instance's life; no word per slot until something re-seats a slot, which materializes the table over
        // the same cells
        void adopt_implicit(uint32_t slots) noexcept { word_ = (static_cast<uintptr_t>(slots) << 1) | k_implicit; }
        script_value* trailing() const noexcept { return reinterpret_cast<script_value*>(const_cast<field_table*>(this) + 1); }
        bool implicit() const noexcept { return (word_ & k_implicit) != 0; }
        // slot i's cell (null: none); an implicit slot's cell always exists, raw-invalid while the field is absent
        script_value* cell(size_t i) const noexcept { return implicit() ? trailing() + i : table()[i].get(); }
        // slot i's cell when the field is present in it as the table form reads presence (null pointer or raw-invalid pointee)
        script_value* present_cell(size_t i) const noexcept {
            script_value* c = cell(i);
            return c && c->raw_storage_index() != script_value::TYPEID_INVALID ? c : nullptr;
        }
        void materialize() const {
            if (!implicit()) { return; }
            const uint32_t n = static_cast<uint32_t>(word_ >> 1);
            script_value* arena = trailing();
            heap_header* fresh = allocate(n);
            fresh->size = n;
            for (uint32_t i = 0; i < n; ++i) { new (&fresh->cells()[i]) field_cell(arena + i, false); }
            word_ = reinterpret_cast<uintptr_t>(fresh);
        }
        size_t size() const noexcept { return implicit() ? static_cast<size_t>(word_ >> 1) : (word_ ? header()->size : 0); }
        bool empty() const noexcept { return size() == 0; }
        field_cell& operator[](size_t i) { materialize(); return table()[i]; }
        const field_cell& operator[](size_t i) const { materialize(); return table()[i]; }
        field_cell* begin() { materialize(); return table(); }
        field_cell* end() { materialize(); return table() + size(); }
        const field_cell* begin() const { materialize(); return table(); }
        const field_cell* end() const { materialize(); return table() + size(); }
        void resize(size_t n) {
            materialize();
            const uint32_t held = static_cast<uint32_t>(size());
            if (n > (word_ ? header()->cap : 0)) { grow(n); }
            if (!word_) { return; }
            field_cell* cells = table();
            for (size_t i = held; i < n; ++i) { new (&cells[i]) field_cell(); }
            for (size_t i = held; i > n;) { cells[--i].~field_cell(); }
            header()->size = static_cast<uint32_t>(n);
        }
        void pop_back() noexcept {
            if (implicit()) {
                word_ -= uintptr_t{2};
                trailing()[word_ >> 1].~script_value();
                return;
            }
            table()[--header()->size].~field_cell();
        }
        void clear() noexcept {
            if (implicit()) {
                const uint32_t n = static_cast<uint32_t>(word_ >> 1);
                word_ = k_implicit;
                for (uint32_t i = 0; i < n; ++i) { trailing()[i].~script_value(); }
                return;
            }
            if (!word_) { return; }
            const uint32_t n = header()->size;
            header()->size = 0;
            for (uint32_t i = 0; i < n; ++i) { table()[i].~field_cell(); }
        }
    private:
        static constexpr uintptr_t k_implicit = 1;   // the low bit: the word is the trailing cells' count, not a table
        struct heap_header {
            uint32_t size = 0;
            uint32_t cap = 0;
            field_cell* cells() noexcept { return reinterpret_cast<field_cell*>(this + 1); }
        };
        static heap_header* allocate(size_t cap) {
            auto* fresh = new (::operator new(sizeof(heap_header) + cap * sizeof(field_cell))) heap_header();
            fresh->cap = static_cast<uint32_t>(cap);
            return fresh;
        }
        heap_header* header() const noexcept { return reinterpret_cast<heap_header*>(word_); }
        field_cell* table() const noexcept { return word_ ? header()->cells() : nullptr; }
        bool heap() const noexcept { return word_ != 0 && !implicit(); }
        void grow(size_t n) {
            const uint32_t held = static_cast<uint32_t>(size());
            const size_t cap = (std::max)(n, static_cast<size_t>(word_ ? header()->cap : 0) * 2);
            heap_header* fresh = allocate(cap);
            fresh->size = held;
            for (uint32_t i = 0; i < held; ++i) {
                new (&fresh->cells()[i]) field_cell(std::move(table()[i]));
                table()[i].~field_cell();
            }
            if (heap()) { ::operator delete(static_cast<void*>(header())); }
            word_ = reinterpret_cast<uintptr_t>(fresh);
        }
        mutable uintptr_t word_ = 0;
    };
    inline void reserve_cell_arena(size_t n);
    inline field_cell make_cell(script_value&& v);
    // A null, int, float, char or bool cell is its bytes: the copy constructor writes exactly the tag, the engine and the payload
    inline field_cell make_trivial_cell(const script_value& v);
    // Store WITHOUT reference write-through (the old map's raw insert_or_assign shape:
    // copy_fields_from and lazy-default materialization replace a held reference value
    // itself); returns the stored cell. set_field_unchecked keeps write-through.
    inline script_value& materialize_field(uint64_t id, script_value&& v);
    inline void rehome_overflow_fields();
    // Runtime-added fields and pre-definition writes (null while there are none)
    std::unordered_map<uint64_t, script_value>* overflow_fields() const noexcept { return extras_ ? extras_->overflow_fields.get() : nullptr; }

    uint32_t live_field_refs_ = 0;
    uint16_t constructed_levels_ = k_nothing_constructed;
    std::atomic<bool> registered_{false};   // in the class's registry list (read unlocked by its own dying owner)
    bool script_layer_retired_ : 1 = false;
    bool seeded_from_plan_ : 1 = false;   // create_instance landed every default through the class plan
    bool construction_pending_ : 1 = false;
    bool destructor_armed_ : 1 = false;
    bool late_destructor_ : 1 = false;   // armed by a re-class, not by the allocation: ~class_instance runs the chain
    bool trailing_cells_ : 1 = false;   // the allocation carries the instance's cells (class_instance::make)
    cdtor_phase dispatch_phase_ : 2 = cdtor_phase::none;
    // What few instances hold, boxed so the rest pay one pointer: the heap cell arena of an instance whose allocation carries
    // no cells (first, so it outlives every cell the box holds), the C++ object (released after the fields, as a C++ base dies
    // after the derived members), the cells a re-class retired, runtime-added fields and the parallel region tag
    struct instance_extras {
        std::unique_ptr<std::byte[]> arena_memory;
        script_value* arena = nullptr;
        uint32_t arena_cells = 0;
        uint32_t arena_used = 0;
        std::optional<script_value> native;
        std::vector<std::pair<uint64_t, field_cell>> retired_cells;
        std::optional<script_value> missing_field;   // what get_field answers for a field the instance does not have
        std::unique_ptr<std::unordered_map<uint64_t, script_value>> overflow_fields;
        uint32_t region_tag = 0;
    };
    std::unique_ptr<instance_extras> extras_;
    inline instance_extras& extras();
    // The class's registry is a list through its instances in creation order (class_definition::register_instance): linking
    // and unlinking touch the instance and its two neighbours, under the region lock while a region runs
    class_instance* registry_prev_ = nullptr;
    class_instance* registry_next_ = nullptr;
    // Last, so a field read's class and slot words share a line with the first trailing cells, which start at the table's end
    class_definition* class_def_ = nullptr;
    field_table flat_fields_;
    friend class class_definition;

    inline static std::shared_ptr<void> extract_cpp_object_impl(const script_value& val);
    inline bool fields_cascade() const noexcept;
    // The dying instance's members past the native depth go to the release stack (class_registry.cpp)
    void release_fields() noexcept;
    // A region chunk's dying instance of a serial-only chain hands its C++ object to the join (class_registry.cpp)
    void defer_serial_native() noexcept;
    // An orphaned instance (its definition detached: engine teardown, a definition that died first) has no release stack;
    // its members and the orphans they alone hold die on a local work list, no destructor can run for them
    void release_orphan_fields() noexcept;
};

class class_definition : public std::enable_shared_from_this<class_definition> {
public:
    enum class_type { cpp_class, script_class, vm_class };

    // An instance that outlives its definition or its engine (a host object holding a script
    // value past engine teardown, a signal receiver capturing one) must not reach back into
    // either from its own destructor: orphan every live registrant (unregister is then a
    // no-op) and forget the engine. The definition's destructor does it when it dies first;
    // engine teardown does it for every definition still alive, since a captured environment
    // can keep one alive past the engine.
    inline void detach_for_teardown() noexcept;
    inline virtual ~class_definition();
    // Engine teardown: the values a definition holds can reach it back (a method's dispatch names its class, an instance's
    // deleter holds its definition, a method's scope holds the program environment): dropping them lets it die
    inline void release_values_for_teardown() noexcept;

    inline class_definition(std::string_view name, uint64_t type_id, engine* eng);

    inline class_definition(std::string_view name, uint64_t type_id, class_type type, engine* eng);

    bool is_script_class() const { return class_type_ == script_class; }
    bool is_cpp_class() const { return class_type_ == cpp_class; }

    bool has_property_getters() const { return has_property_getters_; }

    // Method-dispatch epoch: bumps on any change that can alter method resolution for
    // this class (method add/replace, hot reload rebuild, access labels, parent wiring)
    // and propagates through derived_classes_. Per-call-site inline caches validate
    // (class identity, epoch) against it; sound because script class_definitions are
    // pinned for the engine's lifetime (methods_ -> dispatcher -> class cycle), so a
    // cached identity can never be a recycled address with a coincidental epoch.
    uint64_t method_epoch() const { return method_epoch_; }
    unit_stamp unit() const { return unit_; }
    void set_unit(unit_stamp unit) { unit_ = unit; }
    inline void bump_method_epoch();

    // Field-shape epoch: moves only when a field's facts can change - field set, declared types,
    // resolution chain, access labels, custom accessors. Unlike method_epoch it does NOT move on
    // an identical reload. Propagates to derived like bump_method_epoch (same engine-lifetime/no-ABA
    // contract).
    uint64_t field_shape_epoch() const { return field_shape_epoch_; }
    inline void bump_field_shape_epoch();

    type_info_ptr get_type_info() const { return type_info_; }

    void set_type_info(type_info_ptr type_info) { type_info_ = type_info; }

    // shared_ptr<this class>: the tag a host std::shared_ptr of this class carries into script (engine::marked_shared_handle)
    type_info* handle_type() const { return handle_type_; }
    void set_handle_type(type_info* a_handle_type) { handle_type_ = a_handle_type; }

    // is_auto_accessor: the registration is a SYNTHESIZED field accessor (_get_/_set_
    // minted by class build — verbatim get_field / enforce+set_field), which the
    // member site ICs may bypass. The only callers are the two backends' class-build
    // accessor loops; everything else registers unflagged and reclaims the id.
    inline void add_method_by_id(uint64_t name_id, script_function func, bool is_auto_accessor = false);

    // The nearest USER-defined _get_/_set_ for id in get_method order (own methods_, parents
    // in declaration order, the C++ base), skipping entries flagged synthesized in their
    // owning class; null when none. On a C++ class (empty auto set) this is get_method. A
    // synthesized entry anywhere in the script chain is a script field declaration, which
    // shadows a same-named C++ .property().
    inline const script_value* user_accessor(uint64_t id) const;
    inline const script_value* user_accessor_walk(uint64_t id, bool& script_field_seen) const;
    inline script_value find_user_accessor(uint64_t id) const;
    // A user accessor the script chain declared, never a host .property() reached through the
    // C++ base: the archive walks propagate its errors, which are the program's
    bool script_declares_user_accessor(uint64_t id) const;

    // No user accessor anywhere in the chain: the accessor for id (if any) is the synthesized
    // field accessor, whose body is verbatim get_field / enforce+set_field, so member-site
    // caches and field proofs may bypass it.
    inline bool is_auto_accessor(uint64_t id) const;
    inline bool owns_auto_accessor(uint64_t id) const;
    // Replaces the synthesized set after redefine_class installs new_methods (no epoch move:
    // identical reloads must keep field proofs armed)
    inline void set_auto_accessor_ids(std::unordered_set<uint64_t> ids);

    // One bound C++ method overload: its parameter-type signature (empty = untyped/legacy) and the
    // type-erased call. Multiple entries per (name, arity) are same-arity overloads resolved by type.
    struct cpp_overload_entry {
        std::vector<param_type_info> param_types;
        script_function func;
        param_type_info result;   // the C++ result kind (jai_null_type: void, script_value or unrecorded)
    };
    using cpp_overloads_by_arity = std::unordered_map<size_t, std::vector<cpp_overload_entry>>;
    // A host method name's callable: the C++ overload the arguments select
    struct cpp_method_dispatch {
        class_definition* cls;
        uint64_t name_id;
        const cpp_overloads_by_arity* overloads;   // the name's table, which registration never erases
        inline checked_result<script_value> operator()(const std::vector<script_value>& raw_args) const;
        // how the overloads taking argc arguments bind argument i (detail::arg_binding bits: 4 = the lvalue, for a C++ T& the
        // call writes back; 8 = the host's)
        inline uint8_t argument_binding(size_t i, size_t argc) const;
        inline std::optional<checked_result<script_value>> single_overload_miss(const std::vector<script_value>& args) const;
    };

    void add_method(const std::string& name, script_function func, size_t arity = SIZE_MAX);

    // A host callable's C++ exception is the script's catchable runtime error with its text, wherever the runtime
    // calls it (a method, an operator, a property getter or setter, a static)
    inline static checked_result<script_value> host_exception_result(engine* eng, const std::exception& e);

    // Typed overload: param_types is the C++ parameter signature (one entry per script argument,
    // excluding 'this'). Same-arity overloads with DISTINCT signatures coexist and are resolved by
    // argument type at call time; an identical signature re-binds. Empty param_types is a legacy
    // untyped binding (single overload per arity, as before).
    void add_method(const std::string& name, script_function func, size_t arity,
                    std::vector<param_type_info> param_types, param_type_info result = {});

    void add_static_method(const std::string& name, script_function func, size_t arity = SIZE_MAX);

    // C++-only registration door (dynamic_binder statics; script statics take
    // add_static_script_method) — args normalize like add_method's.
    void add_static_method(uint64_t name_id, script_function func, size_t arity = SIZE_MAX);

    inline void add_script_method(std::string_view name, std::shared_ptr<function_decl> ast, std::shared_ptr<environment> definition_env);

    inline void add_static_script_method(std::string_view name, std::shared_ptr<function_decl> ast, std::shared_ptr<environment> definition_env);

    // Shared cache-aware overload resolution for script methods (defined in
    // overload_resolution.hpp). Span core resolves against a caller stack slice
    // (call-arg windows); the vector overload forwards.
    inline checked_result<std::shared_ptr<function_decl>> resolve_method_overload(uint64_t name_id, const script_value* args, size_t argc) const;
    inline checked_result<std::shared_ptr<function_decl>> resolve_method_overload(uint64_t name_id, const std::vector<script_value>& args) const;
    // Same resolution, answered as a pointer to the class-owned handle (overload table
    // or resolution cache): the in-loop call paths pin through the record instead of
    // paying a shared_ptr copy per call
    inline checked_result<const std::shared_ptr<function_decl>*> resolve_method_overload_ref(uint64_t name_id, const script_value* args, size_t argc) const;
    // resolve_method_overload_ref that only reads the resolution cache (a parallel worker resolves while other workers read it)
    inline checked_result<const std::shared_ptr<function_decl>*> resolve_method_overload_shared(uint64_t name_id, const script_value* args, size_t argc) const;
    inline const std::shared_ptr<function_decl>* resolve_method_overload_core(uint64_t name_id, const script_value* args, size_t argc, std::error_code& ec, bool may_cache = true) const;
    inline error_propagator method_overload_miss(std::error_code ec, uint64_t name_id, const script_value* args, size_t argc) const;

    // Single-overload probe for inline-cache fill: the lone overload for name_id, or
    // nullptr when the name has zero or multiple overloads.
    inline function_decl* single_method_overload(uint64_t name_id) const;
    inline std::shared_ptr<function_decl> single_method_overload_ptr(uint64_t name_id) const;
    // argc alone picks name_id's overload (one overload, or one that takes argc): its resolution ignores argument types
    inline bool method_arity_decides(uint64_t name_id, size_t argc) const;

    // Reflection enumeration (reflect::methods / method_arguments): this class's OWN
    // tables — script overload sets (AST decls carry names/types/defaults), the typed
    // C++ overload table, and the raw method-value maps for names in neither
    const auto& script_method_overloads() const { return method_overloads_; }
    const auto& script_static_method_overloads() const { return static_method_overloads_; }
    const auto& cpp_method_overload_table() const { return cpp_method_overloads_; }
    const auto& method_values() const { return methods_; }
    const auto& static_method_values() const { return static_methods_; }
    // the argument count name_id's live host binding takes (a later binding replaces it); SIZE_MAX when unknown
    size_t static_method_arity(uint64_t name_id) const {
        auto it = static_method_arities_.find(name_id);
        return it == static_method_arities_.end() ? SIZE_MAX : it->second;
    }

    void add_field(std::string_view name, const script_value& default_value);

    void add_field(const std::string& name);

    // Declared field types (typed fields enforce like locals). 'auto'
    // fields store no entry (they infer from the initialized value); 'var' stores the
    // any tag explicitly (dynamic stays dynamic). Replaced wholesale on definition and
    // on hot reload; a semantic change on a REDEFINITION (mark_changes) flags the next
    // redefine_class as fields_changed so instances re-migrate (converting per the
    // hot-reload retype rule). The initial definition must NOT flag: a spurious first-reload
    // migration would rebuild fields_ and dangle field pointers cached by suspended
    // coroutines' env chains.
    inline void replace_field_declared_types(std::unordered_map<uint64_t, type_info_ptr> new_types, bool mark_changes);

    // TRUE when instances are value-closed (parallel_for v1):
    // a script class whose every instance field is store-enforced to a value-semantic
    // shape — primitive (int/float/bool/char), STRING (engine value type, per-instance
    // by design; the rare shared string node is detached by the region barrier's
    // normalization pass, not banned here), typed primitive/string container, or a
    // nested script class that is itself flat — with a flat parent chain and no C++
    // base/data. Because those stores all deep-clone, two live instances can never
    // share reachable mutable structure (strings excepted, hence the barrier detach),
    // so a parallel worker owning an instance may mutate it in place with no alias
    // walk. What excludes a class: shared_ptr/weak_ptr/function/reference fields
    // (reference semantics IS sharing) and auto/var fields (no enforced shape).
    // Cached against engine::class_definition_epoch (a nested field class redefining
    // must invalidate containing classes). Defined out of line in class_registry.cpp.
    bool flat_value_semantics() const;
    // Flat apart from fields (at any depth) holding objects of bound C++ classes by value: a parallel element proves each such
    // object its own by identity at the barrier. Memoized like flat_value_semantics.
    bool flat_over_host_values() const;
    // The flat slots whose declared type can reach a refcount (strings, containers, objects);
    // a parallel fork visits only these when normalizing an element of this class. Scalar-only
    // classes answer an empty list. Memoized under the class-definition epoch like flat_value_semantics.
    const std::vector<uint32_t>& handle_bearing_slots() const;
    // The flat slots whose declared type can hold a STRING anywhere in its value closure (a
    // string, a container of strings, a nested value class with one): the only alias a
    // reference-free value-semantic subtree can still carry is a shared string buffer.
    const std::vector<uint32_t>& string_reachable_slots() const;
    bool type_can_hold_string(const type_info_ptr& t, std::vector<const class_definition*>& in_progress) const;
    // A value of this class (or of a subclass held where this class is declared) can destroy an instance whose chain
    // declares a script destructor: the chain or a subclass's declares one, or a by-value field's type can hold such an
    // instance (handles share, never destroy a copy). Cached against engine::definition_epoch.
    bool value_tree_runs_destructor() const;
    // The level whose user-declared copy constructor (T(const T&) or T(T&)) builds a copy of this class: the class itself, else
    // the nearest base declaring one; null when the chain declares none (a copy is memberwise). Cached against
    // engine::definition_epoch, so a reload that adds or drops a copy constructor takes effect at the next copy.
    const class_definition* copy_constructor_level() const;
    // A copy of a value of this class can be observed: its value tree (as value_tree_runs_destructor walks it) runs a
    // destructor or a user copy constructor, so no copy of it may be skipped (moved or aliased instead)
    bool value_tree_copies_observably() const;
    // A move of a value of this class runs a user copy constructor (the chain's, or a by-value field's through its own
    // fields); a container field moves its storage
    bool value_tree_runs_copy_constructor() const;
    // C++ destruction of a dying instance from `level` down: the level's destructor body, its own fields in reverse
    // declaration order, then its bases in reverse order
    inline void destroy_chain(class_instance* instance, const class_definition* level) const;
    // destroy_chain for the levels `kept` does not share (a re-class drops them): kept and its bases stay alive.
    // constructed (non-null): only it and its bases run their destructor bodies; every level's fields still die
    void destroy_levels(class_instance* instance, const class_definition* level, const class_definition* kept,
                        const class_definition* constructed = nullptr) const;
    // destroy_levels one level at a time (the release stack's frames run the same actions): `level`'s body, then its own
    // fields and its bases onto `actions` (back = next)
    void expand_destroy_level(class_instance* instance, const class_definition* level, const class_definition* kept,
                              std::vector<detail::release_action>& actions, const class_definition* constructed = nullptr) const;
    script_value take_field_for_destroy(class_instance* instance, uint64_t field) const;
    void run_destructor_body(class_instance* instance, const class_definition* level, const script_value& body) const;
    // The same question for a declared type (null, var and unknown kinds can hold anything)
    bool type_tree_runs_destructor(const type_info_ptr& t, std::vector<const class_definition*>& in_progress, uint8_t mode = 0) const;

    inline type_info_ptr get_field_declared_type(uint64_t id) const;
    inline type_info_ptr field_declared_type_at(uint32_t slot, uint64_t id) const;   // the slot's declared type (the id answers a stale table)
    inline type_info_ptr field_declared_type_walk(uint64_t id) const;   // the chain's maps, own first

    inline void add_static_field(uint64_t id, const script_value& initial_value);

    // Declared static types, replaced wholesale at every script (re)definition: 'auto'
    // stores a null entry (infers from the held value's tag), 'var' the any tag; a static
    // with NO entry (C++ static_property) stays lenient. No epoch: statics carry no proofs.
    inline void replace_static_declared_types(std::unordered_map<uint64_t, type_info_ptr> new_types);

    inline type_info_ptr get_static_declared_type(uint64_t id) const;

    // Script-write enforcement for statics: enforce_field_write's ladder over the static's
    // declared type (its held tag when 'auto'). Callers store the returned value via
    // set_static_field. Defined out-of-line.
    inline checked_result<script_value> enforce_static_write(uint64_t id, script_value value) const;
    // The static twin of class_instance::field_store
    template <class Copy> checked_result<script_value> static_store(uint64_t id, const script_value& source, Copy&& copy) const;

    // Hot reload: clear the instance-method overload sets + resolution cache so a reload
    // rebuilds them fresh (re-adding each method via add_script_method) instead of appending
    // to the previous definition's overloads.
    inline void clear_instance_method_overloads();

    // Hot reload: same for script static-method overload sets (C++ static arities untouched).
    inline void clear_static_method_overloads();

    // Hot reload: drop statics absent from the new definition so they're no longer accessible.
    inline void retain_static_fields(const std::set<uint64_t>& keep);

    inline const script_value* get_static_field_ptr(uint64_t id) const;

    inline script_value* get_static_field_ptr(uint64_t id);

    inline const script_value& get_static_field(uint64_t id) const;

    inline script_value& get_static_field(uint64_t id);

    [[nodiscard]] inline bool set_static_field(uint64_t id, const script_value& value);

    [[nodiscard]] inline bool set_static_field(uint64_t id, script_value&& value);

    inline bool is_static_field(uint64_t id) const;

    // a class constant (`const T s = e;` in the body): marked when the class is defined; the chain answers
    inline void mark_const_static(uint64_t id, bool is_const);
    inline bool is_const_static(uint64_t id) const;

    inline bool has_static_field(uint64_t id) const;

    inline script_value get_method(uint64_t id, bool throw_if_missing = true) const;
    // get_method's binding without a copy (a parallel worker reads the class's method values and never touches their counts); null when
    // the chain binds none
    inline const script_value* method_ptr(uint64_t id) const;

    // kind + name + "' not found in class '" + class + "'", out of line so the chain walks stay lean
    [[noreturn]] inline JAI_NOINLINE void throw_method_missing(const char* kind, uint64_t id) const;

    inline bool has_method(uint64_t id) const;

    // "~Name", interned once per class (the name never changes)
    inline uint64_t destructor_id(engine& eng) const;

    // This level's own destructor body, null when it declares none (a pointer into methods_, valid while method_epoch_ holds)
    inline const script_value* destructor_body(engine& eng) const;

    // A class without its own destructor still runs its bases' (C++'s implicit destructor)
    inline bool chain_declares_destructor(engine& eng) const;

    // Chain-aware (this class + script parents + cpp base): does ANY resolvable user
    // method with this name exist? Used to let a user class method WIN over a same-named
    // builtin handle method. Cheap: no symbolizer / no throw.
    // A script class of the chain (this one or a script ancestor) defines a method of the name: it answers before the C++ base's
    inline bool script_chain_defines_method(uint64_t id) const;
    // true the first time a map keys a value of this class by identity (the warning is given once per class)
    bool first_identity_key() const noexcept { return !identity_key_warned_.exchange(true, std::memory_order_relaxed); }

    bool defines_method(uint64_t id) const {
        if (methods_.find(id) != methods_.end()) { return true; }
        for (const auto& parent : parent_classes_) {
            if (parent && parent->defines_method(id)) { return true; }
        }
        if (cpp_base_class_ && cpp_base_class_->defines_method(id)) { return true; }
        return false;
    }

    // Truthiness probe memo (is_truthy on objects): does the chain define to_bool()?
    // get_method's exact chain (defines_method); valid while method_epoch_ holds
    inline bool may_define_to_bool() const;
    // A script class of the chain declares operator=: the id it is interned as, 0 when none (valid while method_epoch_ holds)
    inline uint64_t script_assign_operator_id() const;

    inline script_value get_static_method(uint64_t id, bool throw_if_missing = true) const;

    inline bool has_static_method(uint64_t id) const;

    inline bool has_static_method_with_arity(uint64_t name_id, size_t arity) const;

    // C++ name lookup of a static through the inheritance chain: this class, then its
    // bases depth-first; an instance member of the name at a level hides the bases' statics.
    // The class that declares it (owner) is the one stores convert through.
    inline bool declares_instance_member(uint64_t id) const;

    inline script_value* find_static_field_in_chain(uint64_t id, class_definition** owner = nullptr);

    inline const script_value* find_static_method_in_chain(uint64_t id, class_definition** owner = nullptr);

    inline script_value static_method_in_chain(uint64_t id);

    inline std::vector<std::string_view> get_static_field_names() const;

    inline uint64_t chain_defaults_version() const;
    // The plan's values: the chain's merged defaults (a typed-null placeholder for an
    // instance field - its initializer is per-construction code) with every LITERAL
    // initializer baked in, each through the enforcing store's conversion. The script layer
    // supplies the literals (script_class.hpp); a plain class_definition has none.
    virtual const script_value* literal_initializer_for(uint64_t) const { return nullptr; }
    virtual bool chain_literal_plan_ok() const { return true; }
    inline const std::vector<std::pair<uint32_t, script_value>>& instance_plan() const;
    // An instance of this class, armed with the chain's script destructor when it declares one: every instance runs it,
    // a minted one and every copy alike (C++)
    inline strong_ptr<class_instance> allocate_instance();
    // Every memo a construction, copy or death of an instance fills, filled ahead of a parallel region whose chunks make or
    // destroy instances (they only read them)
    inline void prime_region_memos();

    inline strong_ptr<class_instance> create_instance();

    inline const std::unordered_map<uint64_t, script_value>& get_all_field_defaults() const;

    // Flat-field slots (see registry members): base-first chain walk appends unseen
    // ids; existing assignments never move. k_no_field_slot = never declared anywhere
    // in the chain (runtime-added fields live in the instance overflow map).
    static constexpr uint32_t k_no_field_slot = UINT32_MAX;
    void ensure_field_slots() const {
        if (!field_slots_current_.load(std::memory_order_acquire)) [[unlikely]] { rebuild_field_slots_once(); }
    }
    // the layout's build under the class's build flag: parallel workers reading a class whose layout is stale build it once, never
    // concurrently (a definition invalidates it only on the main thread, between regions)
    void rebuild_field_slots_once() const;
    void rebuild_field_slots() const;
    void rebuild_declared_slot_types() const;   // slot_declared_types_ from the chain walk, under the current field shape
    uint32_t field_slot(uint64_t id) const {
        ensure_field_slots();
        const size_t n = slot_to_field_.size();
#ifdef JAISCRIPT_VM_COVERAGE
        census::hit(n > 16 ? census::site::field_slot_hash : census::site::field_slot_scan, static_cast<uint32_t>(n));
#endif
        if (n <= 16) {   // a scan of a small class's slots beats hashing the id (slot_to_field_ is the map's inverse)
            for (size_t i = 0; i < n; ++i) {
                if (slot_to_field_[i] == id) return static_cast<uint32_t>(i);
            }
            return k_no_field_slot;
        }
        auto it = field_slots_.find(id);
        return it != field_slots_.end() ? it->second : k_no_field_slot;
    }
    // The slot holds a C++ property's placeholder (its truth lives in the C++ object,
    // reached through _get_/_set_) and no script class in the chain shadows the name.
    // Valid right after field_slot() (the same recompute fills both).
    bool slot_is_cpp_property(uint32_t slot) const {
        return has_cpp_property_slots_ && cpp_property_slots_[slot] != 0;
    }
    uint64_t slot_field(uint32_t slot) const { return slot_to_field_[slot]; }
    // Every slot this class assigns holds the same field in `derived`'s layout (single inheritance, no later appends)
    inline bool slot_prefix_of(const class_definition& derived) const;
    inline uint32_t field_slot_count() const;
    // The class-level half of class_instance::has_field (declared anywhere in the
    // chain, script or bound C++ base)
    inline bool chain_declares_field(uint64_t id) const;

    /// Class that DECLARED field `id` (own defaults first, then parents, then the C++
    /// base). Field-reference bindability needs the declarer's kind: a C++ .property()
    /// member keeps a dead fields_ placeholder whose truth lives in the C++ object.
    inline const class_definition* find_field_declaring_class(uint64_t id) const;

    inline void set_parent(std::shared_ptr<class_definition> parent);

    [[nodiscard]] bool add_parent(std::shared_ptr<class_definition> parent);

    [[nodiscard]] inline bool set_parents(const std::vector<std::shared_ptr<class_definition>>& parents);
    // the last set_parents changed the chain: the next redefinition migrates the inherited fields too
    bool parents_changed() const { return reparented_from_fields_.has_value(); }

    const std::vector<std::shared_ptr<class_definition>>& get_parent_classes() const {
        return parent_classes_;
    }

    inline std::shared_ptr<class_definition> get_parent() const;

    // get_parent() without the shared_ptr copy (construction paths ask it per instance)
    inline const class_definition* parent_ptr() const;

    // C++ construction order: every base's levels in declaration order, then the class itself (no diamond, so each level once)
    inline const std::vector<const class_definition*>& construction_order() const;
    // How many levels have constructed once `level` has
    inline uint16_t constructed_through(const class_definition* level) const;

    // A class two of `parents` both reach (a base itself or one of its bases), with the first two bases it is reached through
    struct diamond {
        const class_definition* shared = nullptr;
        const class_definition* first = nullptr;
        const class_definition* second = nullptr;
    };
    static inline diamond find_diamond(const std::vector<std::shared_ptr<class_definition>>& parents);
    inline bool has_diamond_inheritance() const;

    inline void add_derived_class(std::weak_ptr<class_definition> derived);
    const std::vector<std::weak_ptr<class_definition>>& derived_classes() const { return derived_classes_; }

    inline void set_cpp_base_class(std::shared_ptr<class_definition> cpp_base);

    std::shared_ptr<class_definition> get_cpp_base_class() const { return cpp_base_class_; }

    // Slot access for a bound C++ type deriving script_extensible, installed by its
    // dynamic_binder; the void* is the native object's pointer, a T* by the holder contract.
    struct script_slot_hooks {
        void (*attach)(void* object, const weaker_ptr<class_instance>& layer) = nullptr;
        void (*own)(void* object, const strong_ptr<class_instance>& layer) = nullptr;
        void (*detach)(void* object) = nullptr;
        strong_ptr<class_instance> (*read)(const void* object) = nullptr;
        const script_extensible* (*slot)(const void* object) = nullptr;
    };
    void set_script_slot_hooks(const script_slot_hooks& hooks) { script_slot_hooks_ = hooks; }
    const script_slot_hooks* slot_hooks() const { return script_slot_hooks_.attach ? &script_slot_hooks_ : nullptr; }
    // A plain wrapper's object that carries a script layer: the layer is the object's identity, so the handle re-points to it.
    // Asked of the object itself (its slot), never of the class's history: extending one object changes nothing class-wide.
    inline bool wrapped_object_has_layer(const class_instance& inst) const;
    // Set once an object of this C++ class has been extended; only the static checker reads it (a later-checked script may
    // reach layer members through the class's static type)
    void note_extended() { extended_ever_ = true; }
    bool extended_ever() const { return extended_ever_; }

    // Nearest C++ base anywhere up the script parent chain — the class an instance's
    // native object is typed as. cpp_base_class_ is only set on the script class whose DIRECT
    // first base is C++, so multi-level hybrids (script : script : C++) must walk.
    std::shared_ptr<class_definition> find_cpp_base_class() const {
        if (cpp_base_class_) { return cpp_base_class_; }
        for (const auto& parent : parent_classes_) {
            if (parent) {
                if (auto found = parent->find_cpp_base_class()) { return found; }
            }
        }
        return nullptr;
    }

    void register_property_setter(uint64_t field_id, uint64_t setter_id);

    // A getter-only .property() registers a REJECTING setter (dynamic_binder
    // register_read_only_setter), so its setter id is non-zero like any other and every
    // store spelling raises through it; this set is the checker's writability answer.
    void register_property_read_only(uint64_t field_id);

    inline bool owns_read_only_property(uint64_t field_id) const;
    // The class whose setter answers for field_id (own table, then the bases in
    // get_property_setter_id order) decides: a derived binding re-registering the
    // property writable is writable
    inline bool is_read_only_property(uint64_t field_id) const;

    // Chain-aware (this class, then its bound C++ bases): a property registered on a
    // binder base_class<> answers for the derived binding too
    inline uint64_t get_property_setter_id(uint64_t field_id) const;

    // Binder .property() registry (the getter side): the "this name is a C++ property of
    // this class" test for the bare-name ladder (detail/member_scope.hpp)
    void register_property_getter(uint64_t field_id, uint64_t getter_id);

    inline uint64_t get_property_getter_id(uint64_t field_id) const;

    // Member-pointer property whose type the cpp_bound box decodes: the address a
    // by-reference binding aims its box at. Encoded
    // exactly as script_value::cpp_bound_encoding / make_cpp_bound.
    struct cpp_member_accessor {
        std::function<void*(const script_value& receiver)> address;
        uint8_t size_and_sign = 0;
        uint8_t semantic_index = 0;
    };
    void register_property_member(uint64_t field_id, cpp_member_accessor accessor);
    // Chain-aware like the getter/setter ids
    inline const cpp_member_accessor* get_property_member_accessor(uint64_t field_id) const;

    // A bound data member's in-place lane (detail/host_member.hpp); chain-aware like the accessors. Registering bumps the
    // method epoch, so every cached hit refills.
    void register_host_member_lane(uint64_t field_id, std::shared_ptr<const detail::host_member_lane> lane);
    inline const detail::host_member_lane* host_member_lane_for(uint64_t field_id) const;
    // The bound C++ type has a vtable: its objects carry their dynamic type in their first word
    void set_cpp_polymorphic(bool polymorphic) { cpp_polymorphic_ = polymorphic; }
    bool cpp_polymorphic() const { return cpp_polymorphic_; }

    bool is_transparent_wrapper() const { return is_transparent_wrapper_; }

    inline script_value unwrap(script_value& wrapper_value) const;

    inline void set_unwrap_function(std::function<script_value(script_value&, engine*)> fn);

    // The value the script archives write in place of an object of this class: a bound C++ vector's elements
    using archive_form_function = script_value (*)(const script_value& object, const class_definition& cls, engine* eng);
    void set_archive_form(archive_form_function fn) { archive_form_ = fn; }
    archive_form_function archive_form() const { return archive_form_; }

    const std::string& get_name() const { return name_; }
    // the name's view in the engine's interned table, looked up once (instances and handles name their type by it)
    std::string_view interned_name() const { intern_name_once(); return interned_name_; }
    uint64_t interned_name_id() const { intern_name_once(); return interned_name_id_; }
    void intern_name_once() const {
        if (interned_name_.data() == nullptr && engine_) { intern_name(); }
    }
    void intern_name() const;
    uint64_t get_type_id() const { return type_id_; }

    // Auto-registration lifecycle: base_class<B>() registers an unregistered
    // property_owner base under its C++ identity so the chain edge always exists;
    // a later explicit dynamic_binder for the same type ADOPTS this definition and
    // renames it (registration phase — no live instances carry the old identity).
    bool auto_registered() const { return auto_registered_; }
    void set_auto_registered(bool v) { auto_registered_ = v; }
    void rename(const std::string& name, uint64_t type_id);
    bool has_live_instances() const;
    // The registered instances in creation order, weakly (a walk that can run script takes this snapshot first)
    inline std::vector<weaker_ptr<class_instance>> registered_instances() const;

    // Reflection surface (docs/site/guide/12-reflection.html): dynamic_binder marks host-backed classes;
    // the generation counter bumps on every redefinition so tools poll one integer and
    // re-query only when it moved (the live-query/recapture model).
    bool cpp_backed() const { return cpp_backed_; }
    void set_cpp_backed(bool v) { cpp_backed_ = v; }
    // A host class whose bound methods the strict checker holds to their C++ signatures: arity, argument kinds and
    // the declared result (dynamic_binder::strict_signatures)
    bool strict_signatures() const { return strict_signatures_; }
    void set_strict_signatures(bool v) { strict_signatures_ = v; }
    // A host class's direct call lane for a bound method (the vm's exec_call_method): the same body on the receiver's
    // C++ object, reached without minting a bound method. It declines (false) any argument shape it does not take,
    // and the ordinary call runs instead; what it throws raises as the ordinary call's would.
    using native_method = bool (*)(void* self, const script_value& receiver, const script_value* args, size_t argc, script_value& out);
    // a lane the binder derives from a bound method's C++ signature: the same conversions on the method it bound (state), one per name.
    // The arguments are the caller's own temporaries: a lane may move one out (a reference parameter must not point into a buffer the
    // host's callbacks can move)
    using native_lane = bool (*)(const void* state, void* self, const script_value& receiver, script_value* args, size_t argc, script_value& out);
    struct native_entry {
        native_method plain = nullptr;
        native_lane lane = nullptr;
        std::shared_ptr<const void> state;
        bool operator()(void* self, const script_value& receiver, script_value* args, size_t argc, script_value& out) const {
            return plain ? plain(self, receiver, args, argc, out) : lane(state.get(), self, receiver, args, argc, out);
        }
    };
    void add_native_method(uint64_t id, native_method fn);
    // An overload's claim on a call: every argument is exactly its parameter's kind (an int for an integral parameter in range,
    // a float for a floating one, a bool, a string). A call exactly one overload claims is the one the ordinary call's scorer
    // picks with cost 0; a call no overload or several claim takes the ordinary call.
    using native_exact = bool (*)(const script_value* args, size_t argc);
    struct native_overload {
        native_lane lane = nullptr;
        std::shared_ptr<const void> state;
        native_exact exact = nullptr;
    };
    using native_overload_set = std::vector<native_overload>;
    // Every binding of the name with a lane joins its set; one binding without a lane (a guarded method, another parameter
    // kind) withdraws the name's lane for good
    void add_native_lane(uint64_t id, native_lane fn, std::shared_ptr<const void> state, native_exact exact = nullptr);
    void withdraw_native_lane(uint64_t id);
    inline bool has_native_methods() const noexcept;
    const native_entry* find_native_method(uint64_t id) const {
        auto it = native_methods_.find(id);
        return it == native_methods_.end() ? nullptr : &it->second;
    }
    // Moves on every change to the native lanes (and on extend) to a value unique in the engine, so a call site that
    // cached an entry of this class can never match a recycled class_definition at the same address
    uint64_t native_stamp() const noexcept { return native_stamp_; }
    inline void restamp_native_methods();
    uint64_t reflection_generation() const { return reflection_generation_; }
    // The C++ identity behind a host-backed class (nullptr for script classes) — lets
    // reflection reach the type_registry schema (property kinds: field/observable/signal)
    const std::type_info* cpp_type() const { return cpp_type_; }
    void set_cpp_type(const std::type_info* t) { cpp_type_ = t; }

	// Engine-local native base edges. Registration preserves the C++ cast before
	// erasing the pointer; transitive casts compose edges without heap allocation.
	template<typename Derived, typename Base>
	void add_cpp_base_cast() {
		const std::type_index base{typeid(Base)};
		for (const auto& edge : cpp_base_casts_) {
			if (edge.base == base) {
				return;
			}
		}
		cpp_base_casts_.push_back({base, +[](void* pointer) -> void* {
			return static_cast<Base*>(static_cast<Derived*>(pointer));
		}});
	}

	inline void* adjust_cpp_pointer(void* pointer, const std::type_info& target) const;

    // Field DECLARATION order, this class's own fields (instance fields first, then
    // statics, each in source order) — replaced wholesale on every (re)definition by
    // both backends' class-decl builders (same-commit twins). Required for reflection's
    // determinism contract: symbol ids are interning-order-dependent per engine, so
    // unordered_map iteration is not reproducible across engines or backends.
    inline void set_field_order(std::vector<uint64_t> order);
    const std::vector<uint64_t>& field_order() const { return field_order_; }

    // Creation order (the registry is a list in creation order; reflect::instances answers in it)
    inline std::vector<strong_ptr<class_instance>> live_instances() const;

    engine* get_engine() const { return engine_; }

    inline std::vector<std::string> get_property_names(bool include_inherited = true) const;

    // This class's own instance fields in declaration order (field_order_), then the fields a binder .property() added in
    // the order it added them, then any other in symbol order: the table's own order differs between standard libraries
    template <class F>
    void for_each_own_field_id(F&& visit) const {
        size_t listed = 0;
        for (uint64_t id : field_order_) {
            if (field_defaults_.count(id)) { visit(id); ++listed; }
        }
        if (listed == field_defaults_.size()) { return; }
        auto unlisted = [&](uint64_t id) {
            return field_defaults_.count(id) && std::find(field_order_.begin(), field_order_.end(), id) == field_order_.end();
        };
        for (uint64_t id : bound_field_order_) {
            if (unlisted(id)) { visit(id); ++listed; }
        }
        if (listed == field_defaults_.size()) { return; }
        std::vector<uint64_t> rest;
        for (const auto& [id, value] : field_defaults_) {
            (void)value;
            if (unlisted(id) && std::find(bound_field_order_.begin(), bound_field_order_.end(), id) == bound_field_order_.end()) { rest.push_back(id); }
        }
        std::sort(rest.begin(), rest.end());
        for (uint64_t id : rest) { visit(id); }
    }

    inline bool is_subtype_of(const std::string& base_class_name) const;

    // === Script member access enforcement (public:/private:/protected: labels) ===
    // nonpublic_members_ holds ONLY the private/protected member ids THIS class declares
    // (fields, methods, statics). chain_has_nonpublic_ is the hot-path guard: true when
    // this class or any ancestor declares a nonpublic member — maintained eagerly at
    // class (re)definition and parent wiring, propagated down through derived_classes_,
    // so the common all-public case costs one bool load per member access.
    inline void set_nonpublic_members(std::unordered_map<uint64_t, access_level> members);

    bool chain_has_nonpublic() const { return chain_has_nonpublic_; }

    // Walks the inheritance chain for the class DECLARING member_id as nonpublic.
    // nullptr = the member is public everywhere (or unknown — access checks pass).
    inline const class_definition* find_nonpublic_declarer(uint64_t member_id, access_level& level_out) const;

    bool derives_from(const class_definition* base) const {
        if (this == base) return true;
        for (const auto& parent : parent_classes_) {
            if (parent && parent->derives_from(base)) return true;
        }
        return cpp_base_class_ && cpp_base_class_->derives_from(base);
    }

    inline void refresh_access_chain_flag();

    using copy_function = std::function<std::shared_ptr<void>(const void*)>;

    void set_copy_function(copy_function copier);

    bool has_copy_function() const {
        return copy_function_ != nullptr;
    }

    // 'nocopy class': instances refuse by-value copies, like a deleted C++ copy constructor.
    // Inherited: a class derived from a nocopy class is nocopy, and so is a script class over
    // a C++ base that has no copy function (a deleted copy constructor reads the same way).
    // Consulted on every object copy, so the chain verdict is memoized against method_epoch_,
    // which every base-class change and hot reload cascades into derived classes.
    inline void nocopy(bool value);
    inline bool nocopy() const;

    // A host class whose constructor or destructor touches state other objects share (a registry, a cache, a service): a
    // parallel body may not construct it, nor any script class over it (dynamic_binder::serial_only_constructor)
    // A host method or static method that touches state other objects share (bound jai::serial): a parallel body may not call
    // it. A method's mark covers its non-const overloads, or every one
    void mark_serial_method(uint64_t name_id, serial_overloads which) { serial_methods_[name_id] = which; }
    const serial_overloads* serial_method(uint64_t name_id) const {
        const auto found = serial_methods_.find(name_id);
        return found != serial_methods_.end() ? &found->second : nullptr;
    }
    void mark_serial_static_method(uint64_t name_id) { serial_static_methods_.insert(name_id); }
    bool serial_static_method(uint64_t name_id) const { return serial_static_methods_.count(name_id) != 0; }
    // A bound host method's overloads by script arity, and whether each takes a const receiver (a const member function, a
    // const T& self): a parallel chunk runs a non-const one only over an object it owns
    struct host_overload { size_t arity = 0; bool is_const = false; };
    void note_host_overload(uint64_t name_id, size_t arity, bool is_const) { host_overloads_[name_id].push_back({ arity, is_const }); }
    const std::vector<host_overload>* host_overloads(uint64_t name_id) const {
        const auto found = host_overloads_.find(name_id);
        return found != host_overloads_.end() ? &found->second : nullptr;
    }
    void serial_only_construction(bool value) { serial_only_construction_ = value; }
    bool serial_only_construction() const { return serial_only_construction_; }
    bool serial_only_chain() const {
        if (serial_only_construction_) { return true; }
        for (const auto& parent : parent_classes_) {
            if (parent && parent->serial_only_chain()) { return true; }
        }
        return false;
    }

    inline std::shared_ptr<void> copy_object(const void* src) const;

    struct method_metadata {
        bool is_virtual = false;
        access_level access = access_level::public_access;
        size_t vtable_index = 0;
    };

    inline void set_method_metadata(std::string_view name, const method_metadata& metadata);

    inline const method_metadata* get_method_metadata(std::string_view name) const;

    inline method_metadata* get_method_metadata_mutable(std::string_view name);

    struct method_info {
        std::shared_ptr<class_definition> owner_class;
        method_metadata* metadata;
    };

    inline method_info find_method(std::string_view name);
    // Every base in declaration order, each one's own bases before the next (get_method's order)
    inline method_info find_inherited_method(uint64_t name_id, std::string_view name);

    // Structural identity for hot reload: the class_decl subtree serialized WITHOUT
    // source locations (detail::structural_node_key). Exact compare — no collision
    // risk — and comment/whitespace/line-shift edits still match. Stored lazily by the
    // backends after each redefinition; empty = no key yet (first definition).
    const std::vector<uint8_t>& structural_key() const { return structural_key_; }
    inline void set_structural_key(std::vector<uint8_t> key, size_t member_count, size_t base_count);
    // O(1) discriminators — only walk the new AST when these already agree.
    inline bool structural_key_shape_matches(size_t member_count, size_t base_count) const;
    // Deterministic fast-path observability (tests assert this instead of wall-clock).
    size_t identical_redefinitions() const { return identical_redefinitions_; }

    // new_field_defaults are the class's defaults exactly as a first declaration stores them (a field with an initializer holds
    // a placeholder its initializer fills per instance); migration_values are what each initializer evaluated to at the reload,
    // the values a live instance's added, reset or retyped fields take
    inline void redefine_class(const std::unordered_map<uint64_t, script_value>& new_field_defaults,
                        const std::unordered_map<uint64_t, script_value>& migration_values,
                        const std::unordered_map<uint64_t, script_value>& new_methods,
                        const std::unordered_map<uint64_t, script_value>& new_static_methods,
                        engine* engine_ref,
                        bool structurally_identical = false,
                        const std::function<void()>& discard_unwinding = {});

    // A reloaded base's live derived instances migrate against the chain's defaults, the base's reload values over them
    inline void update_instances_from_base(const std::unordered_map<uint64_t, script_value>& base_migration_values);
    // one instance's hot_reload_migrate: a failure goes to the engine's script-error handler and the reload goes on
    void run_migrate_hook(script_value instance_value, const script_value& hook, engine* engine_ref, const std::function<void()>& discard_unwinding);

    // Parallel workers construct and copy instances: registry mutations serialize on the
    // engine's region lock while a region runs (a never-taken branch otherwise).
    inline std::unique_lock<std::mutex> registry_lock() const;

    // An instance its caller owns: the weak handle exists only for a worker's parked registration
    inline void register_live_instance(class_instance* live);
    inline JAI_NOINLINE void register_live_instance_in_region(class_instance* live);

    inline void register_instance(weaker_ptr<class_instance> instance, class_instance* live = nullptr);

    // The join (main thread, region over: the plain unlocked path): a worker's surviving instance
    inline void register_instance_after_region(weaker_ptr<class_instance> instance,
                                        const detail::fault_injection* faults = nullptr, class_instance* live_hint = nullptr);

    // O(1) unlink, called from ~class_instance (and a re-class). An instance never registered, or parked by a worker, skips
    // without the lock; a link is only ever changed under the lock while a region runs, so a neighbour cannot move under it.
    inline void unregister_instance(class_instance* instance);

private:
    std::string name_;
    mutable std::string_view interned_name_;
    mutable uint64_t interned_name_id_ = UINT64_MAX;
    uint64_t type_id_;
    bool auto_registered_ = false;
    bool cpp_backed_ = false;
    bool strict_signatures_ = false;
    std::unordered_map<uint64_t, native_entry> native_methods_;
    std::unordered_set<uint64_t> native_lane_names_;   // names a binding without a lane withdrew
    std::unordered_map<uint64_t, std::shared_ptr<native_overload_set>> native_overload_sets_;
    uint64_t native_stamp_ = 0;
    uint64_t reflection_generation_ = 0;
    std::vector<uint64_t> field_order_;
    std::vector<uint64_t> bound_field_order_;
    const std::type_info* cpp_type_ = nullptr;
	struct cpp_base_cast {
		std::type_index base;
		void* (*adjust)(void*);
	};
	std::vector<cpp_base_cast> cpp_base_casts_;
    type_info_ptr type_info_;
    engine* engine_ = nullptr;
    std::unordered_map<uint64_t, script_value> methods_;
    std::unordered_map<uint64_t, std::vector<size_t>> method_arities_;
    std::unordered_map<uint64_t, cpp_overloads_by_arity> cpp_method_overloads_;
    std::unordered_map<uint64_t, std::vector<std::shared_ptr<function_decl>>> method_overloads_;

    struct overload_cache_key {
        uint64_t name_id;
        size_t arity;
        bool operator==(const overload_cache_key& other) const noexcept {
            return name_id == other.name_id && arity == other.arity;
        }
    };
    struct overload_cache_hash {
        size_t operator()(const overload_cache_key& k) const noexcept {
            return std::hash<uint64_t>{}(k.name_id ^ (static_cast<uint64_t>(k.arity) << 32));
        }
    };
    mutable std::unordered_map<overload_cache_key, std::shared_ptr<function_decl>, overload_cache_hash> overload_resolution_cache_;

    std::unordered_map<uint64_t, script_value> static_methods_;
    std::unordered_map<uint64_t, std::vector<std::shared_ptr<function_decl>>> static_method_overloads_;
    std::unordered_set<uint64_t> const_statics_;
    std::unordered_map<uint64_t, size_t> static_method_arities_;
    std::unordered_map<uint64_t, script_value> field_defaults_;
    std::unordered_map<uint64_t, type_info_ptr> field_declared_types_;
    mutable std::vector<type_info_ptr> slot_declared_types_;   // per field slot, the chain walk's answer under slot_declared_epoch_
    mutable uint64_t slot_declared_epoch_ = UINT64_MAX;
    bool declared_types_dirty_ = false;
    std::optional<std::set<uint64_t>> reparented_from_fields_;   // the whole chain's field ids before the last chain change
    std::unordered_map<uint64_t, script_value> static_field_values_;
    std::unordered_set<uint64_t> static_fields_;
    std::unordered_map<uint64_t, type_info_ptr> static_declared_types_;
    std::vector<std::shared_ptr<class_definition>> parent_classes_;
    mutable std::unordered_map<uint64_t, script_value> all_field_defaults_cache_;
    uint64_t defaults_version_ = 1;                       // bumps with every defaults-cache invalidation (own class)
    // Instance plan: the chain's merged defaults as (slot, value) in slot order, so
    // create_instance is one resize + one cell copy per slot instead of three hashes
    // and an enforcing store per field. Valid while every class in the chain keeps the
    // defaults_version_ it was built under (chain_defaults_version sums them).
    mutable std::vector<std::pair<uint32_t, script_value>> instance_plan_;
    mutable uint64_t instance_plan_version_ = 0;
    mutable bool instance_plan_absent_ = false;   // the version's verdict was no plan (the slow construction path)
    mutable bool field_defaults_cache_valid_ = false;
    // Ids whose methods_ entry is the SYNTHESIZED field accessor (see user_accessor)
    std::unordered_set<uint64_t> auto_accessor_ids_;
    // Flat-field slot registry (tier-1 flat classes): APPEND-ONLY — a field id keeps
    // its slot for the class's lifetime (re-added fields REUSE their retired slot), so
    // instance storage never re-indexes across hot reloads. Slot existence is NOT
    // liveness (get_all_field_defaults is); ensure_field_slots walks the chain
    // base-first, so first-build slot order ≈ declaration order.
    mutable std::unordered_map<uint64_t, uint32_t> field_slots_;
    mutable std::vector<uint64_t> slot_to_field_;
    mutable std::vector<uint8_t> cpp_property_slots_;   // see slot_is_cpp_property
    mutable bool has_cpp_property_slots_ = false;
    mutable std::atomic<bool> field_slots_current_{false};   // re-walk the chain on next query
    mutable std::atomic_flag field_slots_building_;
    mutable bool destructor_tree_cached_ = false;
    mutable uint8_t copy_facts_ = 0;   // in the destructor fact's padding: the class keeps its layout
    mutable uint64_t destructor_tree_epoch_ = UINT64_MAX;
    mutable script_value null_field_value_;
    mutable uint64_t to_bool_memo_epoch_ = UINT64_MAX;
    mutable uint64_t destructor_id_ = 0;
    mutable bool may_define_to_bool_ = true;
    mutable uint64_t assign_memo_epoch_ = UINT64_MAX;
    mutable uint64_t script_assign_id_ = 0;

    class_type class_type_;
    std::unordered_map<uint64_t, access_level> nonpublic_members_;
    bool chain_has_nonpublic_ = false;

    std::shared_ptr<class_definition> cpp_base_class_;
    script_slot_hooks script_slot_hooks_;
    bool extended_ever_ = false;
    // the script_extensible slot's offset in this class's objects, measured on the first plain wrapper asked (atomic: workers
    // may ask first)
    mutable std::atomic<int32_t> wrapper_slot_offset_{INT32_MIN};

    std::unordered_map<uint64_t, uint64_t> property_setter_ids_;
    std::unordered_map<uint64_t, uint64_t> property_getter_ids_;
    std::unordered_set<uint64_t> read_only_property_ids_;
    std::unordered_map<uint64_t, cpp_member_accessor> property_member_accessors_;
    std::unordered_map<uint64_t, std::shared_ptr<const detail::host_member_lane>> host_member_lanes_;
    bool cpp_polymorphic_ = false;

    bool has_property_getters_ = false;

    // See method_epoch(): per-call-site inline-cache validity stamp
    uint64_t method_epoch_ = 1;
    unit_stamp unit_{};   // the unit and execution of the class declaration: field initializers resolve in it
    uint64_t field_shape_epoch_ = 1;   // see field_shape_epoch()

    // Destructor probe memo (see create_instance): valid while dtor_memo_epoch_
    // matches method_epoch_. mutable: create_instance is the hot mint path.
    mutable uint64_t dtor_body_epoch_ = 0;
    mutable const script_value* dtor_body_ = nullptr;
    mutable uint64_t dtor_memo_epoch_ = 0;   // method_epoch_ starts at 1 => first mint fills
    mutable std::vector<const class_definition*> construction_order_;
    mutable uint64_t construction_order_epoch_ = 0;
    mutable bool has_script_dtor_ = false;

    bool is_transparent_wrapper_ = false;
    std::function<script_value(script_value&, engine*)> unwrap_function_;
    archive_form_function archive_form_ = nullptr;

    copy_function copy_function_;
    bool nocopy_ = false;
    bool serial_only_construction_ = false;
    std::unordered_map<uint64_t, serial_overloads> serial_methods_;
    std::unordered_set<uint64_t> serial_static_methods_;
    std::unordered_map<uint64_t, std::vector<host_overload>> host_overloads_;
    mutable std::atomic<bool> identity_key_warned_{false};
    mutable bool nocopy_memo_ = false;
    mutable uint64_t nocopy_memo_epoch_ = 0;

    std::unordered_map<uint64_t, method_metadata> method_metadata_;

    // The live instances, linked through themselves in creation order (an instance unlinks itself as it dies)
    class_instance* registry_head_ = nullptr;
    class_instance* registry_tail_ = nullptr;
    size_t registry_count_ = 0;

    std::vector<std::weak_ptr<class_definition>> derived_classes_;

    std::vector<uint8_t> structural_key_;
    size_t key_member_count_ = 0;
    size_t key_base_count_ = 0;
    size_t identical_redefinitions_ = 0;

    // flat_value_semantics() cache + recursion internals (class_registry.cpp)
    mutable bool flat_value_cached_ = false;
    mutable uint64_t flat_value_epoch_ = 0;
    mutable bool flat_host_cached_ = false;
    mutable uint64_t flat_host_epoch_ = UINT64_MAX;
    bool flat_value_semantics_impl(std::vector<const class_definition*>& in_progress, bool host_values = false) const;
    mutable std::vector<uint32_t> handle_slots_cached_;
    mutable std::vector<uint32_t> string_slots_cached_;
    mutable uint64_t string_slots_epoch_ = 0;
    mutable uint64_t handle_slots_epoch_ = 0;
    type_info* handle_type_ = nullptr;
    bool flat_field_type(const type_info_ptr& t, std::vector<const class_definition*>& in_progress, bool host_values = false) const;
    // mode 0: a destructor runs; 1: a destructor or a user copy constructor; 2: a user copy constructor
    bool value_tree_runs_destructor_impl(std::vector<const class_definition*>& in_progress, uint8_t mode = 0) const;
    const class_definition* find_copy_constructor_level() const;
    // the copy facts known under destructor_tree_epoch_ (copy_fact_* bits): a known bit, then its answer
    static constexpr uint8_t copy_fact_tree = 1, copy_fact_ctor_tree = 4, copy_fact_level = 16;
    bool copy_fact(uint8_t fact, uint8_t mode) const;

    inline void recompute_property_getters(const std::unordered_map<uint64_t, script_value>& new_methods, engine* engine_ref);

    inline void store_field_defaults(const std::unordered_map<uint64_t, script_value>& new_field_defaults, engine* engine_ref);
};

// Out-of-line class_instance implementations (need class_definition to be complete)

inline script_value* class_instance::find_field_value(uint64_t id) {
    if (class_def_) {
        const uint32_t slot = class_def_->field_slot(id);
        if (slot != class_definition::k_no_field_slot) {
            if (slot >= flat_fields_.size()) {
                return nullptr;
            }
            return flat_fields_.present_cell(slot);
        }
    }
    if (overflow_fields()) {
        auto it = overflow_fields()->find(id);
        if (it != overflow_fields()->end()) {
            return &it->second;
        }
    }
    return nullptr;
}

inline const script_value* class_instance::find_field_value(uint64_t id) const {
    return const_cast<class_instance*>(this)->find_field_value(id);
}

inline const script_value* class_instance::find_script_field_value(uint64_t id) const {
    if (class_def_) {
        const uint32_t slot = class_def_->field_slot(id);
        if (slot != class_definition::k_no_field_slot && class_def_->slot_is_cpp_property(slot)) { return nullptr; }
    }
    return find_field_value(id);
}

inline bool class_instance::is_script_class() const {
    return class_def_ && class_def_->is_script_class();
}

inline bool class_instance::is_cpp_class() const {
    return class_def_ && class_def_->is_cpp_class();
}

} // namespace jai

#ifdef JAISCRIPT_ENGINE_BODIES_AFTER_CLASS_DEFINITION
#undef JAISCRIPT_ENGINE_BODIES_AFTER_CLASS_DEFINITION
#include "engine_impl.hpp"
#include "conversion_registry_templates.hpp"
#include "value_impl.hpp"
#endif

#endif // __JAISCRIPT_CORE_CLASS_DEFINITION_HPP__
