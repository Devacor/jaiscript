// Part of the engine's closure, which includes it in its order: a translation unit that names it first is sent there
#if !defined(__JAISCRIPT_CORE_ENGINE_HPP__)
#include "engine.hpp"
#elif !defined(__JAISCRIPT_CORE_VALUE_HPP__)
#define __JAISCRIPT_CORE_VALUE_HPP__

#include "types.hpp"
#include "type_info.hpp"
#include "conversion_registry.hpp"
#include "runtime_errors.hpp"
#include "strong_ptr.hpp"
#include <jaiscript/detail/copy_census.hpp>
#include <jaiscript/jaiscript_fwd.hpp>
#include <variant>
#include <optional>
#include <tuple>
#include <memory>
#include <compare>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <bit>
#include <jaiscript/math/types.hpp>
#include <limits>
#include <type_traits>
#include <cmath>
#include <set>

namespace jai {

    // Forward declaration
    class environment;
    class engine;
    class class_instance;
    class class_definition;
    
    // Forward declaration for conversion registry helper
    namespace conversions {
        class conversion_registry;
    }

    // Forward declarations for helper functions
    std::shared_ptr<conversions::conversion_registry> get_engine_conversion_registry(engine* eng);
    // A caught conversion exception's text as a checked_result: "{0}" over the text parked (never interned; engine is
    // incomplete here, the dependent E defers the lookup to the instantiation point)
    template<typename T, typename E>
    inline checked_result<T> conversion_exception_result(E* eng, const char* text) {
        const uint64_t id = eng ? detail::engine_access_of<E>::park_error_text(*eng, text) : 0;
        if (!id) { return checked_result<T>(make_error_code(runtime_error_code::type_mismatch), "conversion failed"); }
        return checked_result<T>(make_error_code(runtime_error_code::type_mismatch), "{0}", id);
    }

    namespace detail {
        // A value that does not convert to the C++ type a host asked for, named as the script spells what it holds
        // (value.cpp): "Cannot convert string to int"
        [[noreturn]] void throw_conversion_error(const script_value& value, const char* target);
        error_propagator conversion_error(const script_value& value, const char* target, runtime_error_code code = runtime_error_code::type_mismatch);
    }

    // shared_ptr extraction (engine.cpp): locks an extended object's weakly held owner
    // (null once the object died); an owning native object is returned as is.
    namespace detail { class object_ref; struct object_borrow_record; }
    std::shared_ptr<void> engine_lock_instance_cpp_object(engine* eng, const detail::object_ref& instance_data);
    
	namespace detail {
		checked_result<void*> checked_native_reference(const script_value& value, const std::type_info& target, uint64_t target_id = UINT64_MAX);
		// A method lane's bound object argument (conversion_registry.cpp): the object as `target`, else null; the handle a
		// shared_ptr parameter takes (empty for null), false when the ordinary call's extraction would refuse it
		void* lane_object_pointer(const script_value& value, const std::type_info& target);
		bool lane_object_handle(const script_value& value, const std::type_info& target, std::shared_ptr<void>& out);
		// a handle into an object a host method returned by reference keeps the receiver's object alive
		void anchor_to_receiver(script_value& result, const script_value& receiver);
	}

    // Forward declaration for friend access
    namespace detail {
        template<typename T> struct value_converter;
    }
    
    // Forward declarations for friend classes
    namespace serialization {
        class binary_archive_writer;
        class binary_archive_reader;
        class json_archive_writer;
        class json_archive_reader;
    }

    // Helper trait to detect shared_ptr specializations
    template<typename T, template<typename...> class Template>
    struct is_specialization : std::false_type {};
    
    template<template<typename...> class Template, typename... Args>
    struct is_specialization<Template<Args...>, Template> : std::true_type {};
    
    template<typename T, template<typename...> class Template>
    inline constexpr bool is_specialization_v = is_specialization<T, Template>::value;
    // a map a C++ binding treats as a script map: a std::map, or script_map itself
    template <typename T>
    inline constexpr bool is_script_map_like_v = is_specialization_v<T, std::map> || std::is_same_v<T, script_map>;

    // Convert a double to script_int (int64) with DEFINED behavior for NaN, +/-inf,
    // and out-of-range values. A plain static_cast of those to a 64-bit integer is
    // undefined behavior in C++ (commonly yielding INT64_MIN or a trap). NaN -> 0,
    // and out-of-range saturates to INT64_MIN/MAX.
    inline script_int float_to_script_int(script_float f) noexcept {
        if (std::isnan(f)) return 0;
        if (f >= 9223372036854775808.0) return (std::numeric_limits<script_int>::max)();   // >= 2^63
        if (f <= -9223372036854775808.0) return (std::numeric_limits<script_int>::min)();  // <= -2^63
        return static_cast<script_int>(f);
    }

namespace detail {
    struct argument_capture_storage;
    // Out of line (schema_binding.cpp) so a binding TU does not instantiate the class and instance machinery
    std::shared_ptr<class_definition> make_class_definition(std::string_view name, uint64_t type_id, engine* eng);
    strong_ptr<class_instance> make_native_instance(class_definition& def, std::string_view name, std::shared_ptr<void> object, engine* eng);
    // A C++ object the call just made (a constructor's, a copy's): on a parallel chunk the chunk owns it (class_instance::region_tag)
    void note_region_made(const script_value& value, engine* eng);

    // The counts of a strong_ptr<class_instance> where class_instance is incomplete: its control block sits a fixed distance
    // before it (class_runtime.hpp checks the distance against cb_storage_offset); the last release dies out of line (value.cpp)
    inline constexpr size_t k_instance_cb_offset = (sizeof(control_block_base) + 7) & ~size_t{7};
    inline control_block_base* instance_control_block(const class_instance* instance) noexcept {
        return reinterpret_cast<control_block_base*>(reinterpret_cast<unsigned char*>(const_cast<class_instance*>(instance)) - k_instance_cb_offset);
    }
    void release_last_instance(class_instance* instance) noexcept;
    // An instance's type name: the engine's interned text for its type id (empty without an engine or an id)
    std::string_view instance_type_name(engine* eng, uint64_t id) noexcept;
#ifndef NDEBUG
    void check_instance_type_identity(engine* eng, std::string_view name, uint64_t id) noexcept;
#endif
    inline void retain_instance(class_instance* instance) noexcept { instance_control_block(instance)->add_strong(); }
    inline void release_instance(class_instance* instance) noexcept {
        if (instance_control_block(instance)->drop_strong() == 0) { release_last_instance(instance); }
    }
    inline size_t instance_use_count(const class_instance* instance) noexcept { return instance_control_block(instance)->get_strong_count(); }

    // A std::shared_ptr owning a class instance through its strong_ptr, for a host that holds the instance as a C++ object's owner
    // (a hybrid's load, an aliasing handle): the last host handle drops the strong count, so it drops on the script's thread
    template <class I = class_instance>
    std::shared_ptr<void> shared_owner_of(strong_ptr<I> instance) {
        I* raw = instance.get();
        if (!raw) { return nullptr; }
        return std::shared_ptr<void>(raw, [keep = std::move(instance)](void*) mutable { keep.reset(); });
    }

    // What an object holder owns: a bound C++ object (a std::shared_ptr) or a class instance (a strong_ptr),
    // in the two words of a shared_ptr. An instance is its pointer then a marker word no shared_ptr's control-block word can hold.
    class object_ref {
    public:
        object_ref() noexcept { new (bytes_) std::shared_ptr<void>(); }
        object_ref(std::nullptr_t) noexcept : object_ref() {}
        object_ref(std::shared_ptr<void> object) noexcept { new (bytes_) std::shared_ptr<void>(std::move(object)); }
        template <class T> requires (!std::is_same_v<T, class_instance> && !std::is_same_v<T, void>)
        object_ref(std::shared_ptr<T> object) noexcept { new (bytes_) std::shared_ptr<void>(std::move(object)); }
        object_ref(strong_ptr<class_instance> instance) noexcept {
            class_instance* held = instance.release();
            if (held) { set_instance_words(held); } else { new (bytes_) std::shared_ptr<void>(); }
        }
        object_ref(const object_ref& other) noexcept {
            if (other.is_instance()) {
                retain_instance(other.instance_pointer());
                std::memcpy(bytes_, other.bytes_, sizeof(bytes_));
            } else {
                new (bytes_) std::shared_ptr<void>(other.shared_ref());
            }
        }
        object_ref(object_ref&& other) noexcept {
            std::memcpy(bytes_, other.bytes_, sizeof(bytes_));
            new (other.bytes_) std::shared_ptr<void>();
        }
        object_ref& operator=(const object_ref& other) noexcept {
            if (this != &other) {
                object_ref copy(other);
                swap(copy);
            }
            return *this;
        }
        object_ref& operator=(object_ref&& other) noexcept {
            if (this != &other) {
                object_ref taken(std::move(other));
                swap(taken);
            }
            return *this;
        }
        ~object_ref() { drop(); }

        void swap(object_ref& other) noexcept {
            unsigned char held[sizeof(bytes_)];
            std::memcpy(held, bytes_, sizeof(bytes_));
            std::memcpy(bytes_, other.bytes_, sizeof(bytes_));
            std::memcpy(other.bytes_, held, sizeof(bytes_));
        }
        void reset() noexcept { object_ref().swap(*this); }

        bool is_instance() const noexcept { return word(1) == k_instance_mark; }
        // the first word in both forms: an instance's pointer, the shared_ptr's stored pointer (checked once, value.cpp)
        void* get() const noexcept { return reinterpret_cast<void*>(word(0)); }
        explicit operator bool() const noexcept { return get() != nullptr; }
        bool operator==(std::nullptr_t) const noexcept { return get() == nullptr; }
        bool operator!=(std::nullptr_t) const noexcept { return get() != nullptr; }
        long use_count() const noexcept {
            return is_instance() ? static_cast<long>(instance_use_count(instance_pointer())) : shared_ref().use_count();
        }
        // The class instance this owns (null for a C++ object)
        class_instance* instance_pointer() const noexcept {
            return is_instance() ? reinterpret_cast<class_instance*>(word(0)) : nullptr;
        }
        // A new strong handle to the class instance this owns (empty for a C++ object); a template so it instantiates where
        // class_instance is complete
        template <class I = class_instance>
        strong_ptr<I> instance() const noexcept { return strong_from_object<I>(static_cast<I*>(instance_pointer())); }
        // The bound C++ object's handle; empty for a class instance
        const std::shared_ptr<void>& shared() const noexcept {
            static const std::shared_ptr<void> none;
            return is_instance() ? none : shared_ref();
        }

        // the layout get() and is_instance() read, checked against the standard library in hand (value.cpp)
        static bool layout_holds() noexcept {
            int probe = 0;
            object_ref ref(std::shared_ptr<void>(std::shared_ptr<void>(), &probe));
            return ref.word(0) == reinterpret_cast<uintptr_t>(&probe) && ref.word(1) != k_instance_mark;
        }
    private:
        static_assert(sizeof(std::shared_ptr<void>) == 2 * sizeof(uintptr_t), "object_ref packs into a shared_ptr's two words");
        static constexpr uintptr_t k_instance_mark = 1;
        uintptr_t word(size_t i) const noexcept {
            uintptr_t w;
            std::memcpy(&w, bytes_ + i * sizeof(uintptr_t), sizeof(w));
            return w;
        }
        void set_instance_words(class_instance* instance) noexcept {
            const uintptr_t words[2] = { reinterpret_cast<uintptr_t>(instance), k_instance_mark };
            std::memcpy(bytes_, words, sizeof(words));
        }
        const std::shared_ptr<void>& shared_ref() const noexcept { return *std::launder(reinterpret_cast<const std::shared_ptr<void>*>(bytes_)); }
        void drop() noexcept {
            if (is_instance()) {
                class_instance* held = instance_pointer();
                new (bytes_) std::shared_ptr<void>();
                release_instance(held);
            } else {
                std::launder(reinterpret_cast<std::shared_ptr<void>*>(bytes_))->~shared_ptr();
                new (bytes_) std::shared_ptr<void>();
            }
        }
        alignas(std::shared_ptr<void>) unsigned char bytes_[sizeof(std::shared_ptr<void>)];
    };
}

    namespace detail {
        // Containers and instances a deep copy nests natively: deeper, the rest of the subtree runs on a work list of steps
        // taken from the back in the order the native walk takes them, so a deep value spends memory, never native depth
        inline constexpr unsigned k_inline_copy_depth = 32;
        // One step of that list: a value into its placeholder, a map entry into its copied map, an object slot onto its
        // copied array, or the end of an instance's copy (its C++ object, after its fields)
        struct deep_copy_item {
            enum class step : uint8_t { value, entry, slot, finish, finish_overflow };
            step what;
            const void* source;
            void* dest;
            size_t index = 0;
        };
        using deep_copy_work = std::vector<deep_copy_item>;
    }

    class script_value {
    public:
        script_value(std::nullptr_t) = delete;
        
        // Friend std::map and std::unordered_map so they can use the private default constructor for operator[]
        template<typename K, typename V, typename C, typename A>
        friend class std::map;
        template<typename K, typename V, typename H, typename E, typename A>
        friend class std::unordered_map;
        
        // Friend std::pair and std::tuple for map's internal piecewise construction
        template<typename T1, typename T2>
        friend struct std::pair;
        template<typename... Types>
        friend class std::tuple;
        
    private:
        // Private default constructor - ONLY for std::map operator[] compatibility
        // This creates an invalid script_value that MUST be immediately assigned to
        script_value() : storage_(std::monostate{}) {}
        
    public:
        
        // ALL constructors require engine context - no exceptions!
        // Special constructor for null/void values
        explicit script_value(std::monostate, engine*) : storage_(std::monostate{}) {}
        
        // AST literal constructor - ONLY for parser to create placeholder values in AST nodes
        // These values should NEVER be used directly, only as templates for interpreter
        // type_info_ is set to nullptr since these are temporary placeholders without engine context
        struct ast_literal_tag {};
        script_value(ast_literal_tag, script_int i) : storage_(i) {}
        script_value(ast_literal_tag, script_float f) : storage_(f) {}
        script_value(ast_literal_tag, const script_string& s) : storage_(make_strong<script_string>(s)) {}
        script_value(ast_literal_tag, script_char c) : storage_(c) {}
        script_value(ast_literal_tag, script_bool b) : storage_(b) {}
        script_value(ast_literal_tag, std::monostate) : storage_(std::monostate{}) {}
        script_value(script_int i, engine* eng);
        script_value(script_float f, engine* eng);
        script_value(const script_string& s, engine* eng);
        script_value(script_string&& s, engine* eng);
        script_value(strong_ptr<script_string> shared, engine* eng);   // shares the string object
        struct object_holder;
        friend struct obj_slot;   // the class-typed buffer's slot names the holder type
        script_value(strong_ptr<object_holder> handle, type_info_ptr tag, engine* eng);   // reboxes an object slot
        script_value(const char* s, engine* eng);
        script_value(script_char c, engine* eng);
        script_value(script_bool b, engine* eng);
        script_value(const vec2& v, engine* eng);
        script_value(const vec3& v, engine* eng);
        script_value(const vec4& v, engine* eng);
        script_value(const ivec2& v, engine* eng);
        script_value(const ivec3& v, engine* eng);
        script_value(const color& v, engine* eng);
        // A vec of `kind` (jai_vec2_type ... jai_ivec3_type) from its component words (binary32 or int32 bits)
        static script_value make_vec(script_value_type kind, const uint32_t* words, engine* eng);
        // This value as a math value of `kind`'s component words: the kind itself, the other precision of the same
        // dimensions (the typed-slot conversion), a decoded bound value; false for anything else (value.cpp)
        bool math_words_as(script_value_type kind, uint32_t* words) const;
        
        // Template constructors for all numeric types - ALL require engine references
        // NOTE: Implementations are in value_impl.hpp (include after engine.hpp)

        // Engine-aware template constructors for integral types (preferred)
        template<typename T>
        requires (std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> && !std::is_same_v<T, script_int>)
        script_value(T i, engine* eng);

        // Engine-aware template constructors for floating point types (preferred)
        template<typename T>
        requires (std::is_floating_point_v<T> && !std::is_same_v<T, script_float>)
        script_value(T f, engine* eng);

        // Delete pointer constructors - use add_global_ref instead
        template<typename T>
        script_value(T* ptr, engine* eng) = delete;

        // storage's relocating move already lands the source at TYPEID_NULL (which also
        // clears any cpp_bound box — the binding lives in the storage), so no second
        // nulling assignment runs here
        script_value(script_value&& other) noexcept
            : storage_(std::move(other.storage_), storage::whole_word{}) {}

        script_value& operator=(script_value&& other) noexcept {
            storage_.assign_whole(std::move(other.storage_));   // the old payload's release, last, may re-enter
            return *this;
        }
        
        // Copy constructor (shallow copy for reference semantics)
        script_value(const script_value& other);
        script_value& operator=(const script_value& other);
        
        // Explicit deep copy method
        script_value clone() const;
#ifdef JAISCRIPT_VM_COVERAGE
        script_value clone_uncounted() const;   // the copy census's clone() is a counting wrapper (detail/copy_census.hpp)
#endif

        // Element clone: plain scalars and strings clone as their shallow copy (clone()'s
        // default case for these raw indexes IS the copy constructor - the same gate
        // clone_for_assignment applies at store boundaries), so container copies and
        // element conversions spend the out-of-line clone only where a deep copy can happen
        // The member copy an aggregate copy makes (C++ copy-constructor semantics): values
        // deep-copy, an explicit shared_ptr<T>/weak_ptr<T> handle copies as a handle and keeps
        // sharing its object. clone() on a bare handle stays the explicit pointee clone.
        script_value clone_element() const {
            return element_copies_shallow() ? *this : clone();
        }
        bool element_copies_shallow() const noexcept {
            switch (raw_storage_index()) {
                case TYPEID_NULL:
                case TYPEID_INT:
                case TYPEID_FLOAT:
                case TYPEID_STRING:
                case TYPEID_UTF8_STRING:
                case TYPEID_CHAR:
                case TYPEID_BOOL:
                case TYPEID_VEC2:
                case TYPEID_IVEC2:
                case TYPEID_VEC3:
                case TYPEID_IVEC3:
                case TYPEID_VEC4:
                case TYPEID_COLOR:
                    return true;
                default:
                    return stamp() && stamp()->base_type == script_value_type::jai_shared_ptr_type;
            }
        }
        // One level of a deep copy: nested containers and instances copy inline up to detail::k_inline_copy_depth; at it
        // the subtree's copy runs on its own work list, and past it (a level of that list) every child is a step on it
        script_value clone_level(detail::deep_copy_work& work, unsigned depth) const;
        // clone_element's rule into dest, which must not move until the work list is done
        void clone_element_into(script_value& dest, detail::deep_copy_work& work, unsigned depth) const;
        // The copy of a subtree below detail::k_inline_copy_depth, on a work list
        void clone_subtree_into(script_value& dest) const;

        // Parallel-region detach (parallel_transform v0): deep copy where EVERY heavy
        // node gets a fresh strong_ptr - including strings, which clone() and plain
        // copies deliberately share - bound primitives decode to detached values, and
        // type_info tags are preserved on every node. The result's reachable set shares
        // no control block with the source, so another thread may copy/mutate it freely.
        // This is also the region's REFCOUNT-SILENT deep clone: traversal reads elements
        // by const& and constructs fresh nodes - it never makes even a transient shallow
        // copy of a shared handle, so it may read a borrowed (shared, region-frozen)
        // container from a worker thread without racing the non-atomic strong counts.
        // Value-semantic content only (null/int/float/string/char/bool + arrays/maps of
        // the same); throws jai::runtime_error naming the offending type otherwise.
        // collected_types (optional) receives every node's type_info for the region's
        // pre-warm pass; collected_nodes (optional) receives the address of every heavy
        // node the copy MINTED (string/array/map payloads) so the region can later
        // recognize results that alias a worker-provisioned value.
        script_value parallel_detached_copy(engine* eng, std::vector<type_info*>* collected_types = nullptr,
                                            std::vector<const void*>* collected_nodes = nullptr) const;
        // One level of the detach, its nested values inline up to detail::k_inline_copy_depth and on a work list below, as
        // clone_level
        script_value parallel_detached_level(engine* eng, std::vector<type_info*>* collected_types, std::vector<const void*>* collected_nodes,
                                             detail::deep_copy_work& work, unsigned depth) const;
        void parallel_detached_subtree_into(engine* eng, script_value& dest, std::vector<type_info*>* collected_types,
                                            std::vector<const void*>* collected_nodes) const;
        // A parallel worker's view of a captured C++ class instance (the barrier makes one per worker): a new instance and
        // holders of its own over the same C++ object, so no count a worker moves is another thread's
        script_value parallel_host_view() const;

        // === Parallel captured-read borrow (captured reads) ===
        // A borrow VIEW of an enclosing container: one raw tagged pointer, so copying the
        // value never touches a refcount - the one operation a worker may not perform on
        // shared structure (strong_ptr counts are non-atomic; even a read-only element
        // copy races). Sound because during a region (a) the write wall forbids every
        // mutation of enclosing state, so the viewed container never reallocates and its
        // interior pointers are stable, and (b) the barrier holds one anchor handle per
        // borrowed container, so the count is stationary and the container outlives the
        // region. Borrows are region-internal: the store kernel (clone_for_assignment)
        // and the region's result path materialize them via the silent deep clone, so no
        // borrow survives the join. `source` must be a (deref'd) array or map.
        // A sliced borrow (an array only) hands out its elements' own handles: the region proved every element exclusive and
        // reads each one from a single worker (parallel_reduce's input, read chunk by chunk)
        static script_value make_parallel_borrow(const script_value& source, engine* eng, bool sliced = false);
        bool is_sliced_parallel_borrow() const noexcept;
        // Viewed container accessors (nullptr when the borrow is of the other kind).
        // Arrays hand back the NODE (its address is the identity; callers dispatch on its kind).
        const script_array* parallel_borrow_array() const noexcept;
        const script_map* parallel_borrow_map() const noexcept;
        // A shared object a worker reached through a host call (shared objects), read through
        // detail::parallel_object_member; `tag` is the type its handle would carry
        static script_value make_object_borrow(const detail::object_borrow_record* record, type_info_ptr tag, engine* eng);
        const detail::object_borrow_record* parallel_object_borrow() const noexcept;

    public:
        // Engine-aware factory methods (preferred - ALWAYS use these)
        static script_value make_array(type_info_ptr element_type, engine* eng);
        static script_value make_map(type_info_ptr keyType, type_info_ptr valueType, engine* eng);
        static script_value make_object(std::string_view type_name, detail::object_ref data, engine* eng);
        // Optimized version with cached type_id
        static script_value make_object(std::string_view type_name, uint64_t type_id, detail::object_ref data, engine* eng);
        static script_value make_class_object(const class_definition& cd, strong_ptr<class_instance> data, engine* eng);   // an instance of the class in hand (no registry lookup)
        // Internal factory method for raw C++ objects - always requires type_id to avoid re-interning
        static script_value make_cpp_object(std::string_view type_name, uint64_t type_id, std::shared_ptr<void> data, engine* eng);
        // Non-owning C++ object holder that still knows its owner weakly (an extended object's
        // native object): T& extraction reads the pointer, shared_ptr extraction locks the weak one.
        static script_value make_cpp_reference(std::string_view type_name, uint64_t type_id, void* object,
                                               std::weak_ptr<void> owner, engine* eng);
    public:
        // Reallocation-safe reference to a vector element (range-for auto&, arr[i] lvalue):
        // holds the owning container + index so deref/assign-through recompute the element
        // address each time and bounds-check it (see reference_holder::container).
        static script_value make_element_reference(const strong_ptr<script_array>& container, size_t index,
                                                   engine* eng, type_info_ptr element_type);
        // The element reference a binding made while the element existed, after the array shrank past it: every touch raises
        // "Reference to a removed array element", as the reference minted in range would (the VM's cell flush of an alias)
        static script_value make_removed_element_reference(const strong_ptr<script_array>& container, size_t index,
                                                           engine* eng, type_info_ptr element_type);
        // Instance-pinned reference to a class field: deref/assign-through re-resolve the
        // field node by id (never a cached address), so hot reload cannot dangle it.
        static script_value make_field_reference(strong_ptr<class_instance> owner, uint64_t field_id,
                                                 engine* eng, type_info_ptr field_type);
        // the same over the field's current cell the caller found (present: a removed field never reaches it), at the slot the
        // owner's class assigned the field (UINT32_MAX: none known), which the reference remembers
        static script_value make_field_reference(strong_ptr<class_instance> owner, uint64_t field_id,
                                                 engine* eng, type_info_ptr field_type, const script_value& field_value,
                                                 uint32_t field_slot = UINT32_MAX);
        // A reference minted through an element of `root` (a chain rooted at arr[i]: nested
        // field, nested element) also counts in root->live_elem_refs, so a parallel fork over
        // `root` can prove its elements reference-free without touching one (value.cpp).
        static void pin_element_root(script_value& reference, const strong_ptr<script_array>& root);
        // Cell reference (Lua-upvalue box): the reference OWNS the boxed value. Escape-marked
        // variables hold one from declaration; ref binding/aliasing shares the handle, and the
        // cell keeps the value alive for as long as any handle exists (escape-legal semantics).
        // `boxed` must not itself be a reference (callers deref first).
        static script_value make_cell_reference(script_value&& boxed, engine* eng);
        // Cells a lambda shares by reference close a cycle through the closures they hold (cell -> function -> capture
        // environment -> cell). A frame that shares cells gathers its cell references before its locals die, and after
        // they die release_cell_cycles clears the cells only that cycle still holds: refcount arithmetic over those cells
        // and the closures they reach, never a heap scan. A cell an escaped closure keeps is checked again later (a weak
        // list swept as it grows) and at engine teardown (release_deferred_cell_cycles).
        static void gather_shared_cells(const script_value* values, size_t count, std::vector<script_value>& out);
        static void release_cell_cycles(std::vector<script_value>& cells, engine* eng);
        static bool release_plain_cells(std::vector<script_value>& cells, engine* eng);
        static void release_deferred_cell_cycles(engine* eng);
        // Map-entry reference: pins the owning map (strong) and re-resolves find(key) on
        // every deref/assign-through - an erased entry errors instead of dangling, and a
        // reference into a temporary map keeps the map alive. entry: the key's value as the site found it (null when absent).
        static script_value make_map_entry_reference(const strong_ptr<script_map>& map_storage, const script_value& key,
                                                     const script_value* entry, engine* eng, type_info_ptr value_type);
        // the memory charge a map-entry reference's mint takes: a site that elides the mint charges it in its place
        static size_t map_entry_reference_charge() noexcept;
        // CPP-MEMBER reference (a binder member-pointer property the cpp_bound box can decode):
        // the referent IS the C++ member at `address`; object_pin keeps the object alive,
        // `receiver` is the handle the property was bound on.
        // A reference to the bound object a non-owning handle reaches (a host method's T& result): reads see the object, an
        // assignment copy-assigns into it, and the handle's anchor keeps its owner alive
        static script_value make_referent_reference(const script_value& handle, engine* eng);
        static script_value make_cpp_member_reference(const script_value& receiver, detail::object_ref object_pin,
                                                      void* address, uint8_t size_and_sign, uint8_t semantic_index,
                                                      uint64_t field_id, engine* eng);
        // PROPERTY reference (every other binder property): deref reads through the getter
        // on every touch, assign-through writes through the setter (a setter-less property
        // stays the silent no-op `receiver.name = v` is today).
        static script_value make_property_reference(const script_value& receiver, uint64_t field_id, engine* eng);
        // STRING-ELEMENT reference (a string subscript as a store target): `pin` is a reference
        // to the string's storage, `index` a byte of a string or a character of a utf8_string;
        // deref reads that character, stores replace it (detail/string_element.hpp)
        static script_value make_string_element_reference(const script_value& pin, size_t index, engine* eng);
        // COMPONENT reference (`v.x` of a math value): pin is the reference to the vector's storage
        // (a cell, a field, an element, a property), component the component it names
        static script_value make_component_reference(const script_value& pin, size_t component, engine* eng);
        static script_value make_function(const script_function& func, engine* eng);
        // Rvalue mint: moves the std::function into its strong_ptr box (every backend
        // mint passes a temporary - the copy was pure churn)
        static script_value make_function(script_function&& func, engine* eng);

        // Mints an unregistered object holder wrapping a coroutine_handle; bypasses the
        // class-registry check the other make_object factories enforce (handles have no class).
        static script_value make_coroutine_handle(uint64_t type_id, std::shared_ptr<void> handle, engine* eng);

        // Factory method for C++ bound values
        // NOTE: Implementation is in value_impl.hpp (include after engine.hpp)
        template<typename T>
        static script_value make_cpp_bound(T* target, engine* eng);
        // The engine tag a bound box of that semantic index carries (int/float/bool/string)
        static type_info_ptr cpp_bound_type_info(engine* eng, uint8_t semantic_index);
        
        // Type information
        type_info_ptr get_type_info() const { return stamp(); }
        // Borrowed view of the stamp for hot guards: no handle copy. Valid while this value is
        // (type_info objects are engine-owned and outlive every value of that engine)
        type_info* type_info_raw() const noexcept { return stamp(); }

        // Get interned type ID for error messages (falls back to enum value if type_info not set)
        uint64_t type_id() const {
            return stamp() ? stamp()->id : static_cast<uint64_t>(current_type());
        }

        // current_type() - What type is ACTUALLY stored in the variant right now
        // Use this for runtime type checks (is_null, is_object, etc.)
        // FORCED inline (WPR: MSVC kept out-of-line instances that cost 262k µs in the
        // vm window): almost every caller compares the result against ONE enum value,
        // so inlining lets the switch constant-fold to a raw index compare.
        JAI_FORCEINLINE script_value_type current_type() const {
            switch (storage_.index()) {
                case 0: return script_value_type::jai_null_type;        // std::monostate
                case 1: return script_value_type::jai_int_type;         // script_int
                case 2: return script_value_type::jai_float_type;       // script_float
                case 3: return script_value_type::jai_string_type;      // script_string
                case 4: return script_value_type::jai_char_type;        // script_char
                case 5: return script_value_type::jai_bool_type;        // script_bool
                case 6: return script_value_type::jai_array_type;       // Array
                case 7: return script_value_type::jai_map_type;         // Map
                case 8: return script_value_type::jai_object_type;      // Object
                case 9: return script_value_type::jai_function_type;    // Function
                case 10: return script_value_type::jai_reference_type;  // Reference
                case 11: return script_value_type::jai_shared_ptr_type; // shared_ptr
                case 12: return script_value_type::jai_weak_ptr_type;   // weak_ptr
                case 13: return script_value_type::jai_invalid_type;    // invalid_tag
                case 14: {                                              // cpp_bound box: report the semantic type (the old shadow index)
                    switch (bound_semantic_index()) {
                        case TYPEID_INT: return script_value_type::jai_int_type;
                        case TYPEID_FLOAT: return script_value_type::jai_float_type;
                        case TYPEID_STRING: return script_value_type::jai_string_type;
                        case TYPEID_CHAR: return script_value_type::jai_char_type;
                        case TYPEID_BOOL: return script_value_type::jai_bool_type;
                        default: return script_value_type::jai_null_type;
                    }
                }
                case 15: return script_value_type::jai_invalid_type;    // parallel borrow: region-internal, never a script-visible type
                case 16: return script_value_type::jai_utf8_string_type;   // utf8_string
                case 17: return script_value_type::jai_vec2_type;
                case 18: return script_value_type::jai_ivec2_type;
                case 19: return script_value_type::jai_vec3_type;
                case 20: return script_value_type::jai_ivec3_type;
                case 21: return script_value_type::jai_vec4_type;
                case 22: return script_value_type::jai_color_type;
                default: return script_value_type::jai_invalid_type;
            }
        }

        // defined_type() - What type the variable was DECLARED as (from type_info)
        // For any_type (var), returns current_type() since it's dynamically typed
        // Use this for type enforcement, assignment compatibility checks
        script_value_type defined_type() const {
            if (stamp()) {
                if (stamp()->base_type == script_value_type::jai_any_type) {
                    return current_type();  // Dynamic typing - use actual value type
                }
                return stamp()->base_type;
            }
            return current_type();
        }

        // type() is defined_type()
        script_value_type type() const { return defined_type(); }

        // Ultra-fast raw storage index - no pointer chasing, single integer read
        // Use this in hot paths like is_truthy() to avoid type_info_ pointer dereference
        // PURE index read: bound primitives report TYPEID_CPP_BOUND (14), never their semantic type
        inline size_t raw_storage_index() const noexcept { return storage_.index(); }

        // Dropping this value can destroy other values or run code (the last handle to a node that owns some): the
        // release stack's test (detail/release_machine.hpp); a leaf (string, math vector, scalar buffer) never does
        bool release_cascades() const noexcept;
        // The node this value can flatten into a container's release (the last handle to an array<var> or a map)
        bool release_flattens() const noexcept;

#ifdef JAISCRIPT_DIAG_XTHREAD_RC
        // Canary build: tag this value's string block single-thread (AST literals) so a
        // cross-thread refcount touch aborts AT the racing site
        void diag_tag_single_thread() {
            if (storage_.index() == TYPEID_STRING) {
                if (auto* s = storage_.get<strong_ptr<script_string>>().get()) {
                    detail::cb_from_object(s)->diag_tag();
                }
            }
        }
#endif
        // Type checking methods use raw_storage_index() for fastest possible type checks
        // Deref-first, then translate the cpp_bound box through its semantic_index so bound
        // primitives (and refs to them) keep answering as their semantic type
        bool is_null() const {
            const script_value& d = deref();
            const size_t idx = d.raw_storage_index();
            // Opaque host pointers (make_value(T*) on an unregistered type) are NON-null
            // while the pointer is live — they ARE objects to script;
            // a null host pointer still reads as null.
            return idx == TYPEID_NULL ||
                   (idx == TYPEID_CPP_BOUND && d.bound_semantic_is(TYPEID_NULL) &&
                    d.get_cpp_bound_ptr() == nullptr);
        }
        bool is_invalid() const { return deref().raw_storage_index() == TYPEID_INVALID; }
        // A held value whose engine was destroyed (held_value): the invalid index under an engine-only tag that names no engine,
        // a word no live value carries; it reads no engine and no type
        static script_value outlived() noexcept {
            script_value result;
            result.storage_ = invalid_tag{};
            result.storage_.set_tag(k_outlived);
            return result;
        }
        bool is_outlived() const noexcept { return raw_storage_index() == TYPEID_INVALID && storage_.tag() == k_outlived; }
        bool is_int() const {
            const script_value& d = deref();
            const size_t idx = d.raw_storage_index();
            return idx == TYPEID_INT || (idx == TYPEID_CPP_BOUND && d.bound_semantic_is(TYPEID_INT));
        }
        bool is_float() const {
            const script_value& d = deref();
            const size_t idx = d.raw_storage_index();
            return idx == TYPEID_FLOAT || (idx == TYPEID_CPP_BOUND && d.bound_semantic_is(TYPEID_FLOAT));
        }
        // utf8_string (TYPEID_UTF8_STRING): its own kind, so no byte-string lane takes it; text_bytes() is the bytes of either kind
        bool is_utf8_string() const { return deref().raw_storage_index() == TYPEID_UTF8_STRING; }
        inline const script_utf8& unchecked_as_utf8() const noexcept { return *storage_.get<strong_ptr<script_utf8>>(); }
        // the in-place utf8_string write gate: a node anyone else holds is replaced by a private copy first (copy-on-write, as
        // strings do), and the node's character caches clear: the write is about to move characters
        inline script_utf8& unchecked_as_utf8_ref() {
            auto& node = *storage_.get_if<TYPEID_UTF8_STRING>();
            if (!node.unique()) [[unlikely]] { node = make_strong<script_utf8>(node->bytes); }
            node->changed();
            return *node;
        }
        // the bytes of a string or a utf8_string (the raw kind is one of the two): the builtin method bodies' receiver
        inline const std::string& unchecked_text() const noexcept {
            return storage_.index() == TYPEID_UTF8_STRING ? storage_.get<strong_ptr<script_utf8>>()->bytes : unchecked_as_string();
        }
        inline std::string& unchecked_text_ref(engine* a_engine) {
            return storage_.index() == TYPEID_UTF8_STRING ? unchecked_as_utf8_ref().bytes : unchecked_as_string_ref(a_engine);
        }
        // the bytes of a string or a utf8_string (a C++-bound string box excluded); null for anything else
        const std::string* text_bytes() const noexcept {
            const script_value& d = deref();
            const size_t idx = d.raw_storage_index();
            if (idx == TYPEID_STRING) return &d.unchecked_as_string();
            if (idx == TYPEID_UTF8_STRING) return &d.unchecked_as_utf8().bytes;
            return nullptr;
        }
        static script_value make_utf8(std::string bytes, engine* eng);
        bool is_text() const { return is_string() || is_utf8_string(); }   // a string or a utf8_string
        bool is_string() const {
            const script_value& d = deref();
            const size_t idx = d.raw_storage_index();
            return idx == TYPEID_STRING || (idx == TYPEID_CPP_BOUND && d.bound_semantic_is(TYPEID_STRING));
        }
        bool is_char() const {
            const script_value& d = deref();
            const size_t idx = d.raw_storage_index();
            return idx == TYPEID_CHAR || (idx == TYPEID_CPP_BOUND && d.bound_semantic_is(TYPEID_CHAR));
        }
        bool is_bool() const {
            const script_value& d = deref();
            const size_t idx = d.raw_storage_index();
            return idx == TYPEID_BOOL || (idx == TYPEID_CPP_BOUND && d.bound_semantic_is(TYPEID_BOOL));
        }
        bool is_array() const { return deref().raw_storage_index() == TYPEID_ARRAY; }
        bool is_map() const { return deref().raw_storage_index() == TYPEID_MAP; }
        bool is_object() const {
            auto idx = deref().raw_storage_index();
            return idx == TYPEID_OBJECT;
        }
        bool is_function() const { return deref().raw_storage_index() == TYPEID_FUNCTION; }
        bool is_reference() const { return raw_storage_index() == TYPEID_REFERENCE; }  // Don't deref for this check!
        // No deref (matches the old member test): a reference TO a bound global answers false
        bool is_cpp_bound() const {
            const size_t idx = raw_storage_index();
            if (idx == TYPEID_CPP_BOUND) return true;
            if (idx == TYPEID_OBJECT) {
                auto* h = storage_.get_if<TYPEID_OBJECT>();
                return *h && (*h)->bound_object() != nullptr;
            }
            return false;
        }
        // Pure index test for hot guards (bound primitive/string/opaque box only, no holder chase)
        bool is_cpp_bound_primitive() const noexcept { return raw_storage_index() == TYPEID_CPP_BOUND; }

        bool is_non_owning_object() const {
            auto* h = storage_.get_if<TYPEID_OBJECT>();
            return h && *h && (*h)->bound_object() != nullptr && !(*h)->holds_object();
        }

        // Returns nullptr if not cpp_bound. Caller is responsible for type safety.
        void* get_cpp_bound_ptr() const {
            if (auto* b = storage_.get_if<TYPEID_CPP_BOUND>()) return *b ? (*b)->ptr : nullptr;
            if (auto* h = storage_.get_if<TYPEID_OBJECT>()) return *h ? (*h)->bound_object() : nullptr;
            return nullptr;
        }

        template<typename T>
        T* get_cpp_bound_as() const {
            return static_cast<T*>(get_cpp_bound_ptr());
        }

        // Precondition: raw_storage_index() == TYPEID_CPP_BOUND. Returns a DETACHED temporary
        // that behaves exactly like the old shadow-decoded operand:
        //   INT/FLOAT/BOOL/CHAR -> decoding accessor into a fresh primitive;
        //   STRING              -> fresh script_value copying the LIVE string;
        //   NULL (opaque)       -> null value (same "Invalid operands" type_mismatch as before).
        script_value bound_decoded_temp() const {
            switch (bound_semantic_index()) {
                case TYPEID_INT: return script_value(unchecked_as_int(), payload_engine());
                case TYPEID_FLOAT: return script_value(unchecked_as_float(), payload_engine());
                case TYPEID_BOOL: return script_value(unchecked_as_bool(), payload_engine());
                case TYPEID_CHAR: return script_value(unchecked_as_char(), payload_engine());
                case TYPEID_STRING: return script_value(unchecked_as_string(), payload_engine());
                default: return script_value(std::monostate{}, payload_engine());
            }
        }

        // Store-boundary normalization (all-detach): a bound
        // PRIMITIVE stored into a container slot or member detaches to a snapshot —
        // write-through belongs to the registered NAME (and refs/captures/by-ref params
        // bound to it), never to stored copies. Opaque host tokens (semantic NULL) keep
        // their box (object-like handles; decoding would flatten them to null).
        // Call sites guard on raw_storage_index() == TYPEID_CPP_BOUND first (hot paths).
        script_value detached_for_store() const {
            if (raw_storage_index() == TYPEID_CPP_BOUND && bound_semantic_index() != TYPEID_NULL) {
                return bound_decoded_temp();
            }
            return *this;
        }

    public:
        // Invoke ON THE DEREF'D value (a reference-to-bound holds a reference_holder, so a
        // bare-this lookup would answer TYPEID_NULL). Public: ONE cached semantic-index read
        // then compare beats repeated is_int()/is_float()/... probes (each re-derefs) -
        // classification sites cache this once.
        size_t bound_semantic_index() const noexcept {
            auto* box = bound_box();
            return box ? box->semantic_index : TYPEID_NULL;
        }
    private:
        struct cpp_bound_holder;  // defined next to object_holder below
        // Invoke these ON THE DEREF'D value: a reference-to-bound holds a reference_holder
        // (index 10), so a bare-this lookup would answer null/false.
        const cpp_bound_holder* bound_box() const noexcept {
            auto* b = storage_.get_if<TYPEID_CPP_BOUND>();
            return b ? b->get() : nullptr;
        }
        bool bound_semantic_is(size_t idx) const noexcept {
            auto* box = bound_box();
            return box && box->semantic_index == idx;
        }
        // Two-shape: box ptr for index 14, holder->bound_object() for object-bound (index 8)
        void* bound_target_ptr() const noexcept {
            return get_cpp_bound_ptr();
        }
        uint8_t bound_size_and_sign() const noexcept {
            auto* box = bound_box();
            return box ? box->size_and_sign : 0;
        }
    public:

        bool is_weak_ptr() const { return deref().raw_storage_index() == TYPEID_WEAK_PTR; }

        // Type marker helpers - check if value has shared_ptr<T> or weak_ptr<T> type info
        bool is_shared_ptr_type() const {
            return stamp() && stamp()->base_type == script_value_type::jai_shared_ptr_type;
        }

        bool is_weak_ptr_type() const {
            return stamp() && stamp()->base_type == script_value_type::jai_weak_ptr_type;
        }

        // script_value extraction (inlined for performance)
        // These throwing versions delegate to checked_* versions for consistency
        inline script_int as_int() const {
            auto result = checked_as_int();
            if (!result) [[unlikely]] {
                throw_numeric_error(result.message(), result.symbol_id(), result.symbol_id2());
            }
            return result.value();
        }

        inline script_float as_float() const {
            auto result = checked_as_float();
            if (!result) [[unlikely]] {
                throw_numeric_error(result.message(), result.symbol_id(), result.symbol_id2());
            }
            return result.value();
        }

        inline const script_string& as_string() const {
            auto result = checked_as_string();
            if (!result) [[unlikely]] {
                throw_numeric_error(result.message(), result.symbol_id(), result.symbol_id2());
            }
            return *result.value();
        }

        inline script_bool as_bool() const {
            auto result = checked_as_bool();
            if (!result) [[unlikely]] {
                throw_numeric_error(result.message(), result.symbol_id(), result.symbol_id2());
            }
            return result.value();
        }

        inline script_char as_char() const {
            auto result = checked_as_char();
            if (!result) [[unlikely]] {
                throw_numeric_error(result.message(), result.symbol_id(), result.symbol_id2());
            }
            return result.value();
        }

        // ============================================================================
        // UNCHECKED ACCESSORS - Fast direct access without type checking
        // ONLY use these when you've already verified the type (e.g., via type() switch)
        // These check for cpp_bound values and fall back to storage_ otherwise
        // Uses std::get_if which returns a pointer without throwing (faster than std::get)
        // ============================================================================

        inline script_bool unchecked_as_bool() const noexcept {
            if (auto* b = storage_.get_if<TYPEID_CPP_BOUND>()) [[unlikely]] {
                return *static_cast<const bool*>((*b)->ptr);
            }
            return *storage_.get_if<TYPEID_BOOL>();
        }

        // WPR at 99deff47: 56k µs of out-of-line calls in the vm window
        JAI_FORCEINLINE script_int unchecked_as_int() const noexcept {
            if (auto* b = storage_.get_if<TYPEID_CPP_BOUND>()) [[unlikely]] {
                const void* p = (*b)->ptr;
                const uint8_t ss = (*b)->size_and_sign;
                const uint8_t size = ss & 0x7F;
                if (ss & 0x80) {
                    switch (size) {
                        case 1: return static_cast<script_int>(*static_cast<const uint8_t*>(p));
                        case 2: return static_cast<script_int>(*static_cast<const uint16_t*>(p));
                        case 4: return static_cast<script_int>(*static_cast<const uint32_t*>(p));
                        default: return static_cast<script_int>(*static_cast<const uint64_t*>(p));
                    }
                }
                switch (size) {
                    case 1: return static_cast<script_int>(*static_cast<const int8_t*>(p));
                    case 2: return static_cast<script_int>(*static_cast<const int16_t*>(p));
                    case 4: return static_cast<script_int>(*static_cast<const int32_t*>(p));
                    default: return *static_cast<const script_int*>(p);
                }
            }
            return *storage_.get_if<TYPEID_INT>();
        }

        // the payload of a value whose raw storage index is known to be INT / FLOAT (a chunk constant a typed instruction was emitted for): no bound decode
        JAI_FORCEINLINE script_int unchecked_int_payload() const noexcept { return storage_.get<script_int>(); }
        JAI_FORCEINLINE script_float unchecked_float_payload() const noexcept { return storage_.get<script_float>(); }

        // Mutable accessor for in-place modification (avoids make_value() overhead in loops)
        // Caller MUST be dominated by a raw-index gate: no bound decode is possible through a script_int&
        inline script_int& unchecked_as_int_ref() noexcept {
            return *storage_.get_if<TYPEID_INT>();
        }

        JAI_FORCEINLINE script_float unchecked_as_float() const noexcept {
            if (auto* b = storage_.get_if<TYPEID_CPP_BOUND>()) [[unlikely]] {
                if (((*b)->size_and_sign & 0x7F) == sizeof(script_float))
                    return *static_cast<const script_float*>((*b)->ptr);
                return static_cast<script_float>(*static_cast<const float*>((*b)->ptr));
            }
            return *storage_.get_if<TYPEID_FLOAT>();
        }

        // Mutable accessor for in-place modification (avoids make_value() overhead in loops)
        // Caller MUST be dominated by a raw-index gate: no bound decode is possible through a script_float&
        inline script_float& unchecked_as_float_ref() noexcept {
            return *storage_.get_if<TYPEID_FLOAT>();
        }

        inline const script_string& unchecked_as_string() const noexcept {
            if (auto* b = storage_.get_if<TYPEID_CPP_BOUND>()) [[unlikely]] {
                // string bindings are only ever created over std::string/script_string - exact type
                return *static_cast<const script_string*>((*b)->ptr);
            }
            return **storage_.get_if<TYPEID_STRING>();
        }
        // The string HANDLE (raw storage index STRING only): typed string buffers store this
        inline const strong_ptr<script_string>& unchecked_string_handle() const noexcept {
            return *storage_.get_if<TYPEID_STRING>();
        }
        // The object HANDLE (raw storage index OBJECT only): class-typed buffers store this
        inline const strong_ptr<object_holder>& unchecked_object_handle() const noexcept {
            return *storage_.get_if<TYPEID_OBJECT>();
        }

        // The in-place string WRITE gate: string copies share one
        // node (O(1) copies), so a write through a shared node would reach every holder.
        // A node anyone else holds is replaced by a private copy first (copy-on-write) -
        // the write lands in *this alone, which must therefore be the storage the script
        // named (a variable cell, element, field, map value, holder scratch), never a
        // transient copy. Bound strings return the LIVE C++ string (write-through).
        inline script_string& unchecked_as_string_ref(engine* a_engine) {
            if (auto* b = storage_.get_if<TYPEID_CPP_BOUND>()) [[unlikely]] {
                return *static_cast<script_string*>((*b)->ptr);
            }
            auto& node = *storage_.get_if<TYPEID_STRING>();
            if (!node.unique()) [[unlikely]] {
                return unshare_string_node(a_engine);
            }
            return *node;
        }
        script_string& unshare_string_node(engine* a_engine);   // value.cpp: the fresh node, charged to a_engine's memory cap
        void unshare_vec_node();

        inline script_char unchecked_as_char() const noexcept {
            if (auto* b = storage_.get_if<TYPEID_CPP_BOUND>()) [[unlikely]] {
                return *static_cast<const script_char*>((*b)->ptr);
            }
            return *storage_.get_if<TYPEID_CHAR>();
        }

#define JAI_TYPED_ARRAY_VIEW_TEXT(script, cpp) \
            "array<" script "> is a typed buffer: the script_value vector view exists only for array<var> " \
            "(iterate with script_array::for_each, extract as<std::vector<" cpp ">>, or assign to an array<var> first)"
#define JAI_VECTOR_ARRAY_VIEW_TEXT(script) \
            "array<" script "> is a typed buffer: the script_value vector view exists only for array<var> " \
            "(iterate with script_array::for_each, or assign to an array<var> first)"
        // The one error the array<var> view gives for a typed node, naming the node's element type
        static constexpr const char* typed_array_vector_view_message(script_array::kind_t kind) noexcept {
            switch (kind) {
                case script_array::kind_t::i64: return JAI_TYPED_ARRAY_VIEW_TEXT("int", "int64_t");
                case script_array::kind_t::f64: return JAI_TYPED_ARRAY_VIEW_TEXT("float", "double");
                case script_array::kind_t::b8: return JAI_TYPED_ARRAY_VIEW_TEXT("bool", "bool");
                case script_array::kind_t::c8: return JAI_TYPED_ARRAY_VIEW_TEXT("char", "char");
                case script_array::kind_t::str: return JAI_TYPED_ARRAY_VIEW_TEXT("string", "std::string");
                case script_array::kind_t::u8: return JAI_TYPED_ARRAY_VIEW_TEXT("uint8", "uint8_t");
                case script_array::kind_t::i8: return JAI_TYPED_ARRAY_VIEW_TEXT("int8", "int8_t");
                case script_array::kind_t::obj:
                    return "an array of a class type is a typed buffer: the script_value vector view exists only for array<var> "
                           "(iterate with script_array::for_each, or assign to an array<var> first)";
                case script_array::kind_t::v2: return JAI_VECTOR_ARRAY_VIEW_TEXT("math::vec2");
                case script_array::kind_t::iv2: return JAI_VECTOR_ARRAY_VIEW_TEXT("math::ivec2");
                case script_array::kind_t::v3: return JAI_VECTOR_ARRAY_VIEW_TEXT("math::vec3");
                case script_array::kind_t::iv3: return JAI_VECTOR_ARRAY_VIEW_TEXT("math::ivec3");
                case script_array::kind_t::v4: return JAI_VECTOR_ARRAY_VIEW_TEXT("math::vec4");
                case script_array::kind_t::rgba: return JAI_VECTOR_ARRAY_VIEW_TEXT("math::color");
                case script_array::kind_t::hetero: break;
            }
            return "";
        }
#undef JAI_VECTOR_ARRAY_VIEW_TEXT
#undef JAI_TYPED_ARRAY_VIEW_TEXT
        // Unchecked array accessor - caller must verify is_array() AND that the node is
        // dynamic (array<var>) first: array<int>/array<float> are raw buffers by their
        // declared type and have no script_value vector (iterate with
        // script_array::for_each / unchecked_array_node, extract as<std::vector<T>>).
        inline const std::vector<script_value>& unchecked_as_array() const noexcept {
            assert(!(*storage_.get_if<TYPEID_ARRAY>())->is_typed() &&
                   "typed array reached the array<var>-only script_value vector surface");
            return (*storage_.get_if<TYPEID_ARRAY>())->values();
        }

        // Unchecked mutable array NODE handle - caller must verify is_array() first
		inline const strong_ptr<script_array>& unchecked_get_array_storage() const noexcept {
			return *storage_.get_if<TYPEID_ARRAY>();
		}

        inline strong_ptr<script_array>& unchecked_get_array_storage() noexcept {
            return *storage_.get_if<TYPEID_ARRAY>();
        }

        // Unchecked const array NODE pointer - the array's identity (node-alias keys,
        // borrow tags). Caller must verify is_array() first.
        inline const script_array* unchecked_array_node() const noexcept {
            return storage_.get_if<TYPEID_ARRAY>()->get();
        }


        // Unchecked function accessor - caller must verify is_function() first
        inline const script_function& unchecked_as_function() const noexcept {
            return **storage_.get_if<TYPEID_FUNCTION>();
        }

        // Unchecked map accessor - caller must verify is_map() first
        inline const script_map& unchecked_as_map() const noexcept {
            return **storage_.get_if<TYPEID_MAP>();
        }

        // Unchecked mutable map storage accessor - caller must verify is_map() first
		inline const strong_ptr<script_map>& unchecked_get_map_storage() const noexcept {
			return *storage_.get_if<TYPEID_MAP>();
		}

        inline strong_ptr<script_map>& unchecked_get_map_storage() noexcept {
            return *storage_.get_if<TYPEID_MAP>();
        }

        // Unchecked mutable string storage accessor - caller must verify raw string storage first
        inline strong_ptr<script_string>& unchecked_get_string_storage() noexcept {
            return *storage_.get_if<TYPEID_STRING>();
        }

        inline const std::vector<script_value>& as_array() const {
            auto result = checked_as_array();
            if (!result) [[unlikely]] {
                throw_runtime_error(result.message());
            }
            return *result.value();
        }
        
        inline std::vector<script_value>& as_array() {
            // For non-const, we can't use checked_as_array() which returns const&
            // So we check type and return mutable reference directly
            if (type() != script_value_type::jai_array_type) {
                detail::throw_conversion_error(deref(), "array");
            }
            const auto& node = storage_.get<strong_ptr<script_array>>();
            if (node->is_typed()) {
                // Consistent in every build: the vector view is array<var>-only; converting
                // an array<int> to it is an explicit language step (array<var> x = ints)
                throw runtime_error(typed_array_vector_view_message(node->kind()));
            }
            return node->values();
        }
        
        inline const script_map& as_map() const {
            auto result = checked_as_map();
            if (!result) [[unlikely]] {
                throw_runtime_error(result.message());
            }
            return *result.value();
        }
        
        inline script_map& as_map() {
            if (type() != script_value_type::jai_map_type) {
                detail::throw_conversion_error(deref(), "map");
            }
            return *storage_.get<strong_ptr<script_map>>();
        }

        const script_function& as_function() const;

        // Safe mutable reference accessors for zero-copy parameter binding
        // These encapsulate direct storage access and handle deref() properly
        // Bound targets: alias the LIVE C++ variable when the binding's width/signedness exactly
        // matches the script type; otherwise a catchable contract error (never bad_variant_access).
        inline script_int& as_int_ref() {
            script_value& val = deref();
            if (val.type() != script_value_type::jai_int_type) {
                detail::throw_conversion_error(deref(), "int");
            }
            if (auto* box = val.bound_box()) {
                if (box->size_and_sign == sizeof(script_int)) {  // size 8, signed (no 0x80 bit)
                    return *static_cast<script_int*>(box->ptr);
                }
                throw runtime_error("cannot bind a non-const script reference to a C++-bound variable of a different width");
            }
            return val.storage_.get<script_int>();
        }

        inline script_float& as_float_ref() {
            script_value& val = deref();
            if (val.type() != script_value_type::jai_float_type) {
                detail::throw_conversion_error(deref(), "float");
            }
            if (auto* box = val.bound_box()) {
                if ((box->size_and_sign & 0x7F) == sizeof(script_float)) {
                    return *static_cast<script_float*>(box->ptr);
                }
                throw runtime_error("cannot bind a non-const script reference to a C++-bound variable of a different width");
            }
            return val.storage_.get<script_float>();
        }

        inline script_bool& as_bool_ref() {
            script_value& val = deref();
            if (val.type() != script_value_type::jai_bool_type) {
                detail::throw_conversion_error(deref(), "bool");
            }
            if (auto* box = val.bound_box()) {
                if ((box->size_and_sign & 0x7F) == sizeof(script_bool)) {
                    return *static_cast<script_bool*>(box->ptr);
                }
                throw runtime_error("cannot bind a non-const script reference to a C++-bound variable of a different width");
            }
            return val.storage_.get<script_bool>();
        }

        inline script_char& as_char_ref() {
            script_value& val = deref();
            if (val.type() != script_value_type::jai_char_type) {
                detail::throw_conversion_error(deref(), "char");
            }
            if (auto* box = val.bound_box()) {
                if ((box->size_and_sign & 0x7F) == sizeof(script_char)) {
                    return *static_cast<script_char*>(box->ptr);
                }
                throw runtime_error("cannot bind a non-const script reference to a C++-bound variable of a different width");
            }
            return val.storage_.get<script_char>();
        }

        // a_engine takes the copy-on-write's memory charge (a bound function's string& parameter passes its engine)
        inline script_string& as_string_ref(engine* a_engine = nullptr) {
            script_value& val = deref();
            if (val.type() != script_value_type::jai_string_type) {
                detail::throw_conversion_error(deref(), "string");
            }
            if (auto* box = val.bound_box()) {
                // string bindings are only ever created over std::string/script_string - exact type
                return *static_cast<script_string*>(box->ptr);
            }
            return val.unchecked_as_string_ref(a_engine);   // the copy-on-write gate: a host write stays in this value
        }
        
        // Generic extraction with type checking
        // Thin wrapper around checked_as<T>() that throws on error
        // NOTE: This throws for compatibility with C++ bindings (dynamic_binder, function_binder)
        // Internal interpreter code should use checked_as<T>() directly
        template<typename T>
        T as() const {
            auto result = checked_as<T>();
            if (!result) [[unlikely]] {
                if (result.message().empty()) {
                    throw_runtime_error("Type conversion failed");
                }
                throw_checked_error(payload_engine(), result.message(), result.symbol_id(), result.symbol_id2());
            }
            return std::move(result.value());
        }
        
        // Non-const version of as() for extracting non-const references
        template<typename T>
        T as() {
            // Handle reference types
            if constexpr (std::is_reference_v<T>) {
                using base_type = std::remove_cv_t<std::remove_reference_t<T>>;
                
                // For const references, delegate to const version
                if constexpr (std::is_const_v<std::remove_reference_t<T>>) {
                    return const_cast<const script_value*>(this)->as<T>();
                } else {
                    // For non-const references - use safe accessors that handle deref()
                    if constexpr (std::is_same_v<base_type, script_int>) {
                        return as_int_ref();
                    } else if constexpr (std::is_same_v<base_type, script_float> || std::is_same_v<base_type, double>) {
                        return as_float_ref();
                    } else if constexpr (std::is_same_v<base_type, script_bool>) {
                        return as_bool_ref();
                    } else if constexpr (std::is_same_v<base_type, script_char>) {
                        return as_char_ref();
                    } else if constexpr (std::is_same_v<base_type, script_string> || std::is_same_v<base_type, std::string>) {
                        return as_string_ref();
                    } else if constexpr (std::is_same_v<base_type, std::vector<script_value>>) {
                        return as_array();
                    } else if constexpr (std::is_same_v<base_type, script_map>) {
                        return as_map();
                    } else if constexpr (std::is_same_v<base_type, script_value>) {
                        // For script_value&, just return a reference to the dereferenced value
                        return deref();
                    } else {
                        // References resolve first (cells reach C++ boundaries raw); the
                        // object gate is the STORAGE, not type_info_ — var-stored objects
                        // legally carry an 'any' declared tag (decl flattening), so
                        // only the holder's registered-type check below guards the cast.
                        if (is_reference()) {
                            return deref().as<T>();
                        }
                        // Unregistered-class binding (opaque box): the pointer IS the object.
                        // No holder exists so there is no name check (matches the old behavior);
                        // bound values are not OBJECT storage, so probe before the storage gate.
                        if (auto* box = bound_box()) {
                            return *static_cast<base_type*>(box->ptr);
                        }
						if (is_object()) {
							// Smart pointers past this point would reinterpret the holder's
							// T as shared_ptr<T> — no aliasable handle exists in a holder
							// (only a bound_box, handled above, can legitimately be one).
							if constexpr (is_specialization_v<base_type, std::shared_ptr>) {
								throw runtime_error("Object type mismatch");
							}
							auto result = detail::checked_native_reference(*this, typeid(base_type));
							if (!result) [[unlikely]] {
								throw_checked_error(payload_engine(), result.message(), result.symbol_id(), result.symbol_id2());
							}
							return *static_cast<base_type*>(result.value());
						}
                        throw runtime_error("Unsupported type for non-const reference extraction");
                    }
                }
            }
            // For non-reference types, delegate to const version
            else {
                return const_cast<const script_value*>(this)->as<T>();
            }
        }

        // ============================================================================
        // CHECKED HELPER METHODS - Return checked_result instead of throwing
        // Using pointers to avoid reference_wrapper complexity and try/catch overhead
        // ============================================================================

        inline checked_result<const script_string*> checked_as_string() const {
            const script_value& val = deref();
            if (val.storage_.index() == TYPEID_UTF8_STRING) {   // C++ reads a utf8_string as its bytes
                return checked_result<const script_string*>(&val.storage_.get<strong_ptr<script_utf8>>()->bytes);
            }
            if (val.type() != script_value_type::jai_string_type) {
                return detail::conversion_error(deref(), "string");
            }
            if (auto* box = val.bound_box()) {
                return checked_result<const script_string*>(static_cast<const script_string*>(box->ptr));
            }
            return checked_result<const script_string*>(val.storage_.get<strong_ptr<script_string>>().get());
        }

        inline checked_result<const std::vector<script_value>*> checked_as_array() const {
            if (type() != script_value_type::jai_array_type) {
                return detail::conversion_error(deref(), "array", runtime_error_code::not_an_array);
            }
            const auto& node = storage_.get<strong_ptr<script_array>>();
            if (node->is_typed()) {
                return checked_result<const std::vector<script_value>*>(
                    make_error_code(runtime_error_code::type_mismatch), typed_array_vector_view_message(node->kind()));
            }
            return checked_result<const std::vector<script_value>*>(&node->values());
        }

        inline checked_result<const script_map*> checked_as_map() const {
            if (type() != script_value_type::jai_map_type) {
                return detail::conversion_error(deref(), "map", runtime_error_code::not_a_map);
            }
            return checked_result<const script_map*>(storage_.get<strong_ptr<script_map>>().get());
        }

        inline checked_result<script_int> checked_as_int() const {
            const script_value& val = deref();
            if (val.type() == script_value_type::jai_int_type) {
                if (val.is_cpp_bound_primitive()) {
                    return checked_result<script_int>(val.unchecked_as_int());
                }
                return checked_result<script_int>(val.storage_.get<script_int>());
            } else if (val.type() == script_value_type::jai_float_type) {
                if (val.is_cpp_bound_primitive()) {
                    return checked_result<script_int>(float_to_script_int(val.unchecked_as_float()));
                }
                return checked_result<script_int>(float_to_script_int(val.storage_.get<script_float>()));
            }
            return detail::conversion_error(deref(), "int");
        }

        inline checked_result<script_float> checked_as_float() const {
            const script_value& val = deref();
            if (val.type() == script_value_type::jai_float_type) {
                // unchecked_as_float() decodes the bound size (4-byte float vs
                // 8-byte double). Reading a bound float as a raw script_float
                // (double) reinterpreted 8 bytes from a 4-byte object -> garbage + OOB.
                if (val.is_cpp_bound_primitive()) {
                    return checked_result<script_float>(val.unchecked_as_float());
                }
                return checked_result<script_float>(val.storage_.get<script_float>());
            } else if (val.type() == script_value_type::jai_int_type) {
                // Int to float conversion. unchecked_as_int() decodes the bound
                // integer's true size/signedness; the old `const int*` cast read a
                // fixed 4-byte signed value, truncating int64 and misreading other widths.
                if (val.is_cpp_bound_primitive()) {
                    return checked_result<script_float>(static_cast<script_float>(val.unchecked_as_int()));
                }
                return checked_result<script_float>(static_cast<script_float>(val.storage_.get<script_int>()));
            }
            return detail::conversion_error(deref(), "float");
        }

        inline checked_result<script_bool> checked_as_bool() const {
            const script_value& val = deref();
            if (val.type() != script_value_type::jai_bool_type) {
                return detail::conversion_error(deref(), "bool");
            }
            if (auto* box = val.bound_box()) {
                return checked_result<script_bool>(*static_cast<const script_bool*>(box->ptr));
            }
            return checked_result<script_bool>(val.storage_.get<script_bool>());
        }

        inline checked_result<script_char> checked_as_char() const {
            const script_value& val = deref();
            if (val.type() != script_value_type::jai_char_type) {
                return detail::conversion_error(deref(), "char");
            }
            if (auto* box = val.bound_box()) {
                return checked_result<script_char>(*static_cast<const script_char*>(box->ptr));
            }
            return checked_result<script_char>(val.storage_.get<script_char>());
        }

        // The conversion the engine's registry holds for T, its failures as checked errors; nullopt when none is registered
        template<typename T>
        std::optional<checked_result<T>> registered_conversion() const {
            if (!payload_engine()) {
                return std::nullopt;
            }
            auto registry = get_engine_conversion_registry(payload_engine());
            if (!registry || !registry->template has_conversion<T>()) {
                return std::nullopt;
            }
            try {
                return checked_result<T>(registry->template convert_from_script<T>(*this));
            } catch (const runtime_error& e) {
                return conversion_exception_result<T>(payload_engine(), e.what());
            } catch (...) {
                return checked_result<T>(make_error_code(runtime_error_code::type_mismatch), "Custom conversion failed");
            }
        }

        // ============================================================================
        // COMPREHENSIVE checked_as<T>() - Returns checked_result instead of throwing
        // ============================================================================
        template<typename T>
        checked_result<T> checked_as() const {
            if constexpr (std::is_same_v<T, script_value>) {
                return checked_result<T>(deref());
            } else if constexpr (math_adapted<T> && !std::is_reference_v<T> && !std::is_pointer_v<T>) {
                using adapter = math_adapter<std::remove_cv_t<T>>;
                typename adapter::native_type native{};
                static_assert(sizeof(native) <= 16 && std::is_trivially_copyable_v<decltype(native)>);
                uint32_t words[4] = {0, 0, 0, 0};
                if (!math_words_as(adapter::kind, words)) {
                    return detail::conversion_error(deref(), math_type_name(adapter::kind));
                }
                std::memcpy(&native, words, sizeof(native));
                return checked_result<T>(adapter::from_native(native));
            }
            // Try to unwrap transparent wrappers for basic types (int, float, bool, string, char)
            // This allows observable_property_ref<int> to be used as int in C++ code
            if constexpr (std::is_same_v<T, script_int> || std::is_same_v<T, int> || std::is_same_v<T, int64_t> ||
                          std::is_same_v<T, script_float> || std::is_same_v<T, double> || std::is_same_v<T, float> ||
                          std::is_same_v<T, script_bool> || std::is_same_v<T, script_char> ||
                          std::is_same_v<T, script_string> || std::is_same_v<T, std::string>) {
                if (auto unwrapped = deref().try_unwrap_transparent_wrapper(); unwrapped && !unwrapped->is_object()) {
                    return unwrapped->checked_as<T>();
                }
            }

            // FAST PATH: Direct type specializations for hot types
            if constexpr (std::is_same_v<T, script_int>) {
                const script_value& val = deref();
                if (val.is_cpp_bound_primitive()) {
                    return checked_result<T>(val.unchecked_as_int());
                }
                if (val.type() == script_value_type::jai_int_type) {
                    return checked_result<T>(val.storage_.get<script_int>());
                } else if (val.type() == script_value_type::jai_float_type) {
                    return checked_result<T>(float_to_script_int(val.storage_.get<script_float>()));
                }
                return detail::conversion_error(deref(), "int");
            }
            else if constexpr (std::is_same_v<T, script_float> || std::is_same_v<T, double>) {
                const script_value& val = deref();
                // Decode bound values via the size/sign-aware accessors. The old
                // blanket `*static_cast<const script_float*>(cpp_bound_ptr_)` read 8
                // bytes for ANY bound value, reinterpreting bound ints and 4-byte
                // floats as doubles (wrong value, and an out-of-bounds read for float).
                if (val.type() == script_value_type::jai_float_type) {
                    return checked_result<T>(val.is_cpp_bound_primitive() ? val.unchecked_as_float()
                                                                          : val.storage_.get<script_float>());
                } else if (val.type() == script_value_type::jai_int_type) {
                    return checked_result<T>(static_cast<script_float>(
                        val.is_cpp_bound_primitive() ? val.unchecked_as_int()
                                                     : val.storage_.get<script_int>()));
                }
                return detail::conversion_error(deref(), "float");
            }
            else if constexpr (std::is_same_v<T, script_bool>) {
                const script_value& val = deref();
                if (auto* box = val.bound_box()) {
                    return checked_result<T>(*static_cast<const script_bool*>(box->ptr));
                }
                if (val.type() == script_value_type::jai_bool_type) {
                    return checked_result<T>(val.storage_.get<script_bool>());
                }
                return detail::conversion_error(deref(), "bool");
            }
            else if constexpr (std::is_same_v<T, script_char>) {
                const script_value& val = deref();
                if (auto* box = val.bound_box()) {
                    return checked_result<T>(*static_cast<const script_char*>(box->ptr));
                }
                if (val.type() == script_value_type::jai_char_type) {
                    return checked_result<T>(val.storage_.get<script_char>());
                }
                return detail::conversion_error(deref(), "char");
            }
            // MEDIUM PATH: Common integral conversions
            else if constexpr (std::is_same_v<T, int>) {
                auto val_result = checked_as<script_int>();
                if (!val_result) {
                    return val_result.error_value();  // Preserves symbol IDs for formatting
                }
                script_int val = val_result.value();
                if (val < std::numeric_limits<int>::min() || val > std::numeric_limits<int>::max()) {
                    return checked_result<T>(
                        make_error_code(runtime_error_code::type_mismatch),
                        "Integer value out of range for int"
                    );
                }
                return checked_result<T>(static_cast<int>(val));
            }
            else if constexpr (std::is_same_v<T, int64_t>) {
                auto val_result = checked_as<script_int>();
                if (!val_result) {
                    return val_result.error_value();  // Preserves symbol IDs for formatting
                }
                return checked_result<T>(static_cast<int64_t>(val_result.value()));
            }
            else if constexpr (std::is_same_v<T, float>) {
                const script_value& val = deref();
                if (val.type() == script_value_type::jai_float_type) {
                    if (val.is_cpp_bound_primitive()) {
                        return checked_result<T>(static_cast<float>(val.unchecked_as_float()));
                    }
                    return checked_result<T>(static_cast<float>(val.storage_.get<script_float>()));
                } else if (val.type() == script_value_type::jai_int_type) {
                    if (val.is_cpp_bound_primitive()) {
                        return checked_result<T>(static_cast<float>(val.unchecked_as_int()));
                    }
                    return checked_result<T>(static_cast<float>(val.storage_.get<script_int>()));
                }
                return checked_result<T>(
                    make_error_code(runtime_error_code::type_mismatch),
                    "Cannot convert script_value to float"
                );
            }
            // String types
            else if constexpr (std::is_same_v<T, script_string> || std::is_same_v<T, std::string>) {
                auto str_result = checked_as_string();
                if (!str_result) {
                    return str_result.error_value();  // Preserves symbol IDs for formatting
                }
                return checked_result<T>(*str_result.value());
            }
            // Handle reference types (now supported via checked_result<T&> specialization)
            else if constexpr (std::is_reference_v<T>) {
                using base_type = std::remove_cv_t<std::remove_reference_t<T>>;

                if constexpr (std::is_const_v<std::remove_reference_t<T>>) {
                    if constexpr (std::is_same_v<base_type, script_int>) {
                        if (type() != script_value_type::jai_int_type) {
                            return detail::conversion_error(deref(), "int");
                        }
                        return checked_result<T>(storage_.get<script_int>());
                    } else if constexpr (std::is_same_v<base_type, script_float>) {
                        if (type() != script_value_type::jai_float_type) {
                            return detail::conversion_error(deref(), "float");
                        }
                        return checked_result<T>(storage_.get<script_float>());
                    } else if constexpr (std::is_same_v<base_type, script_bool>) {
                        if (type() != script_value_type::jai_bool_type) {
                            return detail::conversion_error(deref(), "bool");
                        }
                        return checked_result<T>(storage_.get<script_bool>());
                    } else if constexpr (std::is_same_v<base_type, script_char>) {
                        if (type() != script_value_type::jai_char_type) {
                            return detail::conversion_error(deref(), "char");
                        }
                        return checked_result<T>(storage_.get<script_char>());
                    } else if constexpr (std::is_same_v<base_type, script_string> || std::is_same_v<base_type, std::string>) {
                        auto str_result = checked_as_string();
                        if (!str_result) {
                            return str_result.error_value();  // Preserves symbol IDs for formatting
                        }
                        return checked_result<T>(*str_result.value());
                    } else if constexpr (std::is_same_v<base_type, std::vector<script_value>>) {
                        if (type() != script_value_type::jai_array_type || raw_storage_index() != TYPEID_ARRAY) {
                            return detail::conversion_error(deref(), "array", runtime_error_code::not_an_array);
                        }
                        const script_array* node = unchecked_array_node();
                        if constexpr (std::is_reference_v<T>) {
                            // A reference to the script_value vector exists only for array<var>
                            if (node->is_typed()) {
                                return checked_result<T>(make_error_code(runtime_error_code::type_mismatch),
                                                         typed_array_vector_view_message(node->kind()));
                            }
                            return checked_result<T>(const_cast<std::vector<script_value>&>(node->values()));
                        } else {
                            // By-value request: a typed buffer boxes into the copy the caller asked for
                            return checked_result<T>(node->materialize_values(node->owner));
                        }
                    } else if constexpr (std::is_same_v<base_type, script_map>) {
                        auto map_result = checked_as_map();
                        if (!map_result) {
                            return map_result.error_value();
                        }
                        return checked_result<T>(*map_result.value());
                    } else {
						auto result = detail::checked_native_reference(*this, typeid(base_type));
						if (!result) {
							return result.error_value();
						}
						return checked_result<T>(*static_cast<const base_type*>(result.value()));
                    }
                } else {
                    return checked_result<T>(
                        make_error_code(runtime_error_code::type_mismatch),
                        "Cannot extract non-const reference from const script_value"
                    );
                }
            }
            // Signed integer types with bounds checking
            else if constexpr (std::is_same_v<T, int8_t>) {
                auto val_result = checked_as<script_int>();
                if (!val_result) {
                    return val_result.error_value();  // Preserves symbol IDs for formatting
                }
                script_int val = val_result.value();
                if (val < std::numeric_limits<int8_t>::min() || val > std::numeric_limits<int8_t>::max()) {
                    return checked_result<T>(
                        make_error_code(runtime_error_code::type_mismatch),
                        "Integer value out of range for int8_t"
                    );
                }
                return checked_result<T>(static_cast<int8_t>(val));
            }
            else if constexpr (std::is_same_v<T, int16_t>) {
                auto val_result = checked_as<script_int>();
                if (!val_result) {
                    return val_result.error_value();  // Preserves symbol IDs for formatting
                }
                script_int val = val_result.value();
                if (val < std::numeric_limits<int16_t>::min() || val > std::numeric_limits<int16_t>::max()) {
                    return checked_result<T>(
                        make_error_code(runtime_error_code::type_mismatch),
                        "Integer value out of range for int16_t"
                    );
                }
                return checked_result<T>(static_cast<int16_t>(val));
            }
            else if constexpr (std::is_same_v<T, int32_t>) {
                auto val_result = checked_as<script_int>();
                if (!val_result) {
                    return val_result.error_value();  // Preserves symbol IDs for formatting
                }
                script_int val = val_result.value();
                if (val < std::numeric_limits<int32_t>::min() || val > std::numeric_limits<int32_t>::max()) {
                    return checked_result<T>(
                        make_error_code(runtime_error_code::type_mismatch),
                        "Integer value out of range for int32_t"
                    );
                }
                return checked_result<T>(static_cast<int32_t>(val));
            }
            // Unsigned integer types with bounds checking
            else if constexpr (std::is_same_v<T, uint8_t>) {
                auto val_result = checked_as<script_int>();
                if (!val_result) {
                    return val_result.error_value();  // Preserves symbol IDs for formatting
                }
                script_int val = val_result.value();
                if (val < 0 || val > std::numeric_limits<uint8_t>::max()) {
                    return checked_result<T>(
                        make_error_code(runtime_error_code::type_mismatch),
                        "Integer value out of range for uint8_t (must be 0-255)"
                    );
                }
                return checked_result<T>(static_cast<uint8_t>(val));
            }
            else if constexpr (std::is_same_v<T, uint16_t>) {
                auto val_result = checked_as<script_int>();
                if (!val_result) {
                    return val_result.error_value();  // Preserves symbol IDs for formatting
                }
                script_int val = val_result.value();
                if (val < 0 || val > std::numeric_limits<uint16_t>::max()) {
                    return checked_result<T>(
                        make_error_code(runtime_error_code::type_mismatch),
                        "Integer value out of range for uint16_t (must be non-negative)"
                    );
                }
                return checked_result<T>(static_cast<uint16_t>(val));
            }
            else if constexpr (std::is_same_v<T, uint32_t>) {
                auto val_result = checked_as<script_int>();
                if (!val_result) {
                    return val_result.error_value();  // Preserves symbol IDs for formatting
                }
                script_int val = val_result.value();
                if (val < 0 || val > std::numeric_limits<uint32_t>::max()) {
                    return checked_result<T>(
                        make_error_code(runtime_error_code::type_mismatch),
                        "Integer value out of range for uint32_t (must be non-negative)"
                    );
                }
                return checked_result<T>(static_cast<uint32_t>(val));
            }
            else if constexpr (std::is_same_v<T, uint64_t>) {
                auto val_result = checked_as<script_int>();
                if (!val_result) {
                    return val_result.error_value();  // Preserves symbol IDs for formatting
                }
                script_int val = val_result.value();
                if (val < 0) {
                    return checked_result<T>(
                        make_error_code(runtime_error_code::type_mismatch),
                        "Integer value must be non-negative for uint64_t"
                    );
                }
                return checked_result<T>(static_cast<uint64_t>(val));
            }
            else if constexpr (std::is_same_v<T, size_t>) {
                auto val_result = checked_as<script_int>();
                if (!val_result) {
                    return val_result.error_value();  // Preserves symbol IDs for formatting
                }
                script_int val = val_result.value();
                if (val < 0) {
                    return checked_result<T>(
                        make_error_code(runtime_error_code::type_mismatch),
                        "Integer value must be non-negative for size_t"
                    );
                }
                return checked_result<T>(static_cast<size_t>(val));
            }
            // Vector types
            else if constexpr (is_specialization_v<T, std::vector>) {
                // Use engine's conversion registry first
                if (auto converted = registered_conversion<T>()) {
                    return std::move(*converted);
                }

                // Built-in vector handling
                if (type() != script_value_type::jai_array_type) {
                    return checked_result<T>(
                        make_error_code(runtime_error_code::not_an_array),
                        "Cannot convert non-array to vector"
                    );
                }

                using element_type = typename T::value_type;
                T result;
                if (raw_storage_index() != TYPEID_ARRAY) {
                    return detail::conversion_error(deref(), "array", runtime_error_code::not_an_array);
                }
                const script_array* node = unchecked_array_node();
                result.reserve(node->size());
                // Exact-width payloads leave a typed buffer without boxing; every other
                // element type keeps the per-element checked conversion (range checks intact)
                if constexpr (std::is_same_v<element_type, script_int>) {
                    if (node->kind() == script_array::kind_t::i64) {
                        result.insert(result.end(), node->ints().begin(), node->ints().end());
                        return checked_result<T>(std::move(result));
                    }
                } else if constexpr (std::is_same_v<element_type, script_float>) {
                    if (node->kind() == script_array::kind_t::f64) {
                        result.insert(result.end(), node->floats().begin(), node->floats().end());
                        return checked_result<T>(std::move(result));
                    }
                } else if constexpr (std::is_same_v<element_type, uint8_t> || std::is_same_v<element_type, int8_t>) {
                    if (node->kind() == (std::is_same_v<element_type, uint8_t> ? script_array::kind_t::u8 : script_array::kind_t::i8)) {
                        result.resize(node->bytes().size());
                        if (!result.empty()) { std::memcpy(result.data(), node->bytes().data(), result.size()); }
                        return checked_result<T>(std::move(result));
                    }
                }
                bool failed = false;
                error_propagator failure;
                node->for_each(node->owner, [&](const script_value& elem) {
                    if (failed) { return; }
                    if constexpr (std::is_same_v<element_type, script_value>) {
                        result.push_back(elem);
                    } else {
                        auto elem_result = elem.checked_as<element_type>();
                        if (!elem_result) { failed = true; failure = elem_result.error_value(); return; }
                        result.push_back(elem_result.value());
                    }
                });
                if (failed) { return failure; }
                return checked_result<T>(std::move(result));
            }
            // Map types
            else if constexpr (is_script_map_like_v<T>) {
                // Use engine's conversion registry first
                if (auto converted = registered_conversion<T>()) {
                    return std::move(*converted);
                }

                // Built-in map handling
                if (type() != script_value_type::jai_map_type) {
                    return checked_result<T>(
                        make_error_code(runtime_error_code::not_a_map),
                        "Cannot convert non-map to std::map"
                    );
                }

                using key_type = typename T::key_type;
                using mapped_type = typename T::mapped_type;
                T result;
                auto map_result = checked_as_map();
                if (!map_result) {
                    return map_result.error_value();
                }
                // dependent on T: script_map is complete only where the conversion is instantiated (two-phase lookup)
                using map_view = std::conditional_t<sizeof(T) != 0, script_map, void>;
                const map_view& m = *map_result.value();

                for (const auto& [k, v] : m) {
                    if constexpr (std::is_same_v<key_type, script_value> && std::is_same_v<mapped_type, script_value>) {
                        result.emplace(k, v);
                    } else if constexpr (std::is_same_v<key_type, script_value>) {
                        auto val_result = v.template checked_as<mapped_type>();
                        if (!val_result) {
                            return val_result.error_value();
                        }
                        result.emplace(k, val_result.value());
                    } else if constexpr (std::is_same_v<mapped_type, script_value>) {
                        auto key_result = k.template checked_as<key_type>();
                        if (!key_result) {
                            return key_result.error_value();
                        }
                        result.emplace(key_result.value(), v);
                    } else {
                        auto key_result = k.template checked_as<key_type>();
                        if (!key_result) {
                            return key_result.error_value();
                        }
                        auto val_result = v.template checked_as<mapped_type>();
                        if (!val_result) {
                            return val_result.error_value();
                        }
                        result.emplace(key_result.value(), val_result.value());
                    }
                }
                return checked_result<T>(std::move(result));
            }
            // Support for shared_ptr<class_instance> extraction from objects
            else if constexpr (std::is_same_v<T, jai::strong_ptr<jai::class_instance>>) {
                auto t = type();
                if (t != script_value_type::jai_object_type && t != script_value_type::jai_shared_ptr_type) {
                    return detail::conversion_error(deref(), "object", runtime_error_code::not_an_object);
                }
                // type() reports the DECLARED type: a class-typed variable legally holds
                // null (and references wrap), so probe the deref'd storage — the old
                // std::get threw bad_variant_access through the host boundary here
                auto* objHolder = deref().storage_.get_if<strong_ptr<object_holder>>();
                if (!objHolder) {
                    return detail::conversion_error(deref(), "object", runtime_error_code::not_an_object);
                }
                if (!(*objHolder)->is_instance()) {
                    return detail::conversion_error(deref(), "script class instance", runtime_error_code::not_a_class);
                }
                return checked_result<T>((*objHolder)->instance_handle());
            }
            // Support for shared_ptr<void> extraction
            else if constexpr (std::is_same_v<T, std::shared_ptr<void>>) {
                if (type() != script_value_type::jai_object_type) {
                    return detail::conversion_error(deref(), "object", runtime_error_code::not_an_object);
                }
                // Declared-type guard: null-valued / reference-wrapped variables probe
                // the deref'd storage (see the class_instance branch above)
                auto* objHolder = deref().storage_.get_if<strong_ptr<object_holder>>();
                if (!objHolder) {
                    return detail::conversion_error(deref(), "object", runtime_error_code::not_an_object);
                }
                return checked_result<T>((*objHolder)->held());
            }
            // Support for other shared_ptr types
            else if constexpr (is_specialization_v<T, std::shared_ptr>) {
                if (is_null()) {
                    return checked_result<T>(T{});
                }
                // First check if there's a registered conversion
                if (auto converted = registered_conversion<T>()) {
                    return std::move(*converted);
                }

                // Fall back to default shared_ptr extraction. Declared-type gates lie for
                // var-stored ('any' tag) and reference-wrapped values: probe the deref'd
                // STORAGE (see the class_instance branch above).
                if (auto* holderPtr = deref().storage_.get_if<strong_ptr<object_holder>>(); holderPtr && *holderPtr) {
                    const auto& objHolder = *holderPtr;

                    // Check for non-owning C++ reference (holder bound_ptr set, data null)
                    // Cannot safely return shared_ptr from non-owning reference
                    if (objHolder->bound_object() && !objHolder->holds_object()) {
                        return checked_result<T>(
                            make_error_code(runtime_error_code::type_mismatch),
                            "Cannot extract shared_ptr from non-owning C++ reference. "
                            "The object is bound by reference, not owned. Use T& or T* extraction."
                        );
                    }

                    // Check if we need to use the custom extractor
                    if (objHolder->is_instance()) {
                        if (payload_engine()) {
                            auto registry = get_engine_conversion_registry(payload_engine());
                            if (registry) {
                                auto extracted = registry->extract_custom_object(objHolder->type_name(), objHolder->held());
                                if (extracted && extracted.use_count() == 0) {
                                    // an extended object's layer references it without owning it:
                                    // ownership comes from the object's own shared_ptr, weakly held
                                    extracted = engine_lock_instance_cpp_object(payload_engine(), objHolder->held());
                                    if (!extracted) {
                                        return checked_result<T>(
                                            make_error_code(runtime_error_code::type_mismatch),
                                            "Cannot extract shared_ptr: the extended C++ object no longer exists");
                                    }
                                }
                                if (extracted) {
                                    return checked_result<T>(std::static_pointer_cast<typename T::element_type>(extracted));
                                }
                            }
                        }
                    }

                    // Otherwise use static cast
                    return checked_result<T>(std::static_pointer_cast<typename T::element_type>(objHolder->host_object()));
                }

                return detail::conversion_error(deref(), "object", runtime_error_code::not_an_object);
            }
            // weak_ptr<T>: from null, a shared handle, or a script weak_ptr (an expired one is empty)
            else if constexpr (is_specialization_v<T, std::weak_ptr>) {
                using shared_type = std::shared_ptr<typename T::element_type>;
                const script_value& d = deref();
                if (d.is_null()) {
                    return checked_result<T>(T{});
                }
                if (d.raw_storage_index() == TYPEID_WEAK_PTR) {
                    auto locked = d.storage_.template get<jai::weaker_ptr<object_holder>>().lock();
                    if (!locked) {
                        return checked_result<T>(T{});
                    }
                    script_value target(std::monostate{}, locked->owner());
                    target.set_object_holder(locked);
                    auto shared = target.template checked_as<shared_type>();
                    if (!shared) {
                        return shared.error_value();
                    }
                    return checked_result<T>(T(shared.value()));
                }
                auto shared = d.template checked_as<shared_type>();
                if (!shared) {
                    return shared.error_value();
                }
                return checked_result<T>(T(shared.value()));
            }
            // Support for extracting custom objects by value (dereference shared_ptr)
            else if constexpr (std::is_class_v<T> &&
                             !std::is_same_v<T, std::string> &&
                             !is_specialization_v<T, std::vector> &&
                             !is_script_map_like_v<T>) {
                // Check custom converter first
                if (auto converted = registered_conversion<T>()) {
                    return std::move(*converted);
                }

                // For custom classes, try to extract shared_ptr and dereference. Storage
                // probe, not declared-type gate (var/'any' tags and references lie).
                if (auto* holderPtr = deref().storage_.get_if<strong_ptr<object_holder>>(); holderPtr && *holderPtr) {
                    const auto& objHolder = *holderPtr;

                    // Handle non-owning C++ reference via the holder's bound_ptr
                    if (objHolder->bound_object() && !objHolder->holds_object()) {
                        // Copy from non-owning pointer (only if copyable)
                        if constexpr (std::is_copy_constructible_v<T>) {
                            return checked_result<T>(*static_cast<T*>(objHolder->bound_object()));
                        } else {
                            return checked_result<T>(
                                make_error_code(runtime_error_code::type_mismatch),
                                "Cannot copy non-copyable type from non-owning C++ reference"
                            );
                        }
                    }

                    // Owning path: extract shared_ptr and dereference
                    auto ptr_result = checked_as<std::shared_ptr<T>>();
                    if (!ptr_result) {
                        return ptr_result.error_value();  // Preserves symbol IDs for formatting
                    }
                    return checked_result<T>(*ptr_result.value());
                }
                return checked_result<T>(
                    make_error_code(runtime_error_code::type_mismatch),
                    "Cannot extract custom type by value from non-object"
                );
            }
            else {
                return checked_result<T>(
                    make_error_code(runtime_error_code::type_mismatch),
                    "Unsupported type conversion"
                );
            }
        }

        // Conversion to string for debugging
        std::string to_string() const;
        // a float's text: the shortest that reads back to the same double, ".0" kept on a whole number
        static std::string float_text(double v);

        // A transparent wrapper's (observable_property_ref's) unwrapped value; none for any other value, which copies nothing
        std::optional<script_value> try_unwrap_transparent_wrapper() const;

        // Dereference method - returns this for non-references, dereferences for
        // references. The non-reference fast path is INLINE (one raw index compare):
        // WPR showed the out-of-line call and the current_type() it made per call as
        // two of the vm's hottest rows. Reference resolution stays in value.cpp.
        JAI_FORCEINLINE const script_value& deref() const {
            if (storage_.index() != TYPEID_REFERENCE) [[likely]] {
                return *this;
            }
            return deref_slow();
        }
        JAI_FORCEINLINE script_value& deref() {
            if (storage_.index() != TYPEID_REFERENCE) [[likely]] {
                return *this;
            }
            return deref_slow();
        }
        // Assignment through reference - assigns to target if this is a reference
        void assign_through(const script_value& value);
        void assign_through(script_value&& value);
    private:
        const script_value& deref_slow() const;
        script_value& deref_slow();
        template<typename V>
        void assign_through_reference(V&& value);
    public:
        
        // C++ equality: both sides dereferenced, then the same kind (a string is never a utf8_string here, a char is its string) and the payload
        // equal; floats by IEEE ==, heavy kinds by the identity operator<=> orders them by. Script == is structural
        bool operator==(const script_value& other) const;
        // A char and a string are one key kind: the char is its one-character string to ==, <=> and the map-key hash
        bool char_beside_string(const script_value& other) const noexcept {
            const script_value_type a = type(), b = other.type();
            return (a == script_value_type::jai_char_type && b == script_value_type::jai_string_type) ||
                   (a == script_value_type::jai_string_type && b == script_value_type::jai_char_type);
        }
        // a string's text, or a char's one character (a view into this value)
        std::string_view key_text() const noexcept {
            if (type() != script_value_type::jai_char_type) { return unchecked_as_string(); }
            if (auto* b = storage_.get_if<TYPEID_CPP_BOUND>()) [[unlikely]] { return {static_cast<const script_char*>((*b)->ptr), 1}; }
            return {storage_.get_if<TYPEID_CHAR>(), 1};
        }
        // the map-key hash: equal for keys operator<=> calls equivalent (core/script_map.hpp)
        uint64_t map_key_hash() const noexcept;
        static uint64_t map_string_key_hash(std::string_view text) noexcept;
        bool operator!=(const script_value& other) const { return !(*this == other); }
        
        // C++20 spaceship operator for complete ordering
        std::strong_ordering operator<=>(const script_value& other) const;
        
        // Get engine pointer for creating new script_values
        // the (declared type, engine) pair in one compare of the tag word: exactly this stamp (whose owner is eng), or no stamp and eng
        JAI_FORCEINLINE bool stamped_as(const type_info* stamp) const noexcept {
            return storage_.tag() == reinterpret_cast<uintptr_t>(stamp);
        }
        // The engine a heap payload belongs to (a container, object, reference or bound box records the engine that made it);
        // scalars, strings and math values name none: code that mints from them takes its engine from context
        engine* payload_engine() const noexcept;
        // null under this value's tag word: its stamp, or the engine-made flag
        script_value tagged_null() const noexcept;

        // Implicit conversions for common C++ types (natural overload resolution in C++ calls): as<T>()'s extraction, a bound
        // value's included
        operator int() const { return as<int>(); }
        operator int64_t() const { return as<int64_t>(); }
        operator double() const { return as<double>(); }
        operator float() const { return as<float>(); }

    private:
        // The storage's tag word: the declared type (512-aligned, so the low 9 bits hold the
        // index and the flag) or zero; k_outlived marks a held value whose engine is gone
        static constexpr uintptr_t k_outlived = 0x100;
        static constexpr uintptr_t k_tag_pointer_mask = ~uintptr_t{k_value_tag_alignment - 1};
        static_assert(alignof(type_info) >= k_value_tag_alignment && (k_outlived & 0xFF) == 0 && k_outlived < k_value_tag_alignment,
                      "the tag word's pointer bits sit above the index byte and the outlived flag");
        JAI_FORCEINLINE type_info* stamp() const noexcept {
            const uintptr_t tag = storage_.tag();
            return (tag & k_outlived) ? nullptr : reinterpret_cast<type_info*>(tag);
        }
        JAI_FORCEINLINE void set_stamp(type_info* stamped) noexcept {
            if (stamped) [[likely]] {
                assert((reinterpret_cast<uintptr_t>(stamped) & ~k_tag_pointer_mask) == 0 && "a stamp is tag-aligned");
                storage_.set_tag(reinterpret_cast<uintptr_t>(stamped));
            } else {
                clear_stamp();
            }
        }
        void clear_stamp() noexcept { storage_.set_tag(0); }

	public: // Match the public forward declaration and holder accessors.
        // What an object value points at: a class instance itself (class_instance derives this, at
        // offset 0, so the value's handle is the instance's own), or a host_holder over a bound C++ object
        struct host_holder;
        struct object_holder {
        public:
            // an instance names its type by id alone; a host holder keeps the name, since a C++ type may have no id
            std::string_view type_name() const noexcept {
                return kind_ == k_instance ? detail::instance_type_name(owner_, type_id_) : static_cast<const host_holder*>(this)->type_name_;
            }
            uint64_t type_id() const noexcept { return type_id_; }
            void set_type(std::string_view name, uint64_t id) noexcept {
                type_id_ = id;
                if (kind_ != k_instance) {
                    static_cast<host_holder*>(this)->type_name_ = name;
                } else {
#ifndef NDEBUG
                    detail::check_instance_type_identity(owner_, name, id);
#endif
                }
            }
            engine* owner() const noexcept { return owner_; }
            void set_owner(engine* eng) noexcept { owner_ = eng; }
            bool is_instance() const noexcept { return kind_ == k_instance; }
            class_instance* instance() const noexcept { return is_instance() ? unchecked_instance() : nullptr; }   // null for a C++ object
            class_instance* unchecked_instance() const noexcept {   // is_instance() holds
                return reinterpret_cast<class_instance*>(const_cast<object_holder*>(this));
            }
            template <class I = class_instance>
            strong_ptr<I> instance_handle() const noexcept {
                return is_instance() ? detail::strong_from_object<I>(reinterpret_cast<I*>(unchecked_instance())) : strong_ptr<I>();
            }
            inline void* object() const noexcept;   // the instance itself or the bound C++ object
            inline bool holds_object() const noexcept;
            inline long object_use_count() const noexcept;
            inline const std::shared_ptr<void>& host_object() const noexcept;   // empty for an instance
            inline detail::object_ref held() const noexcept;
            // A coroutine handle IS a reference to a running computation: clone() shares it (reference semantics, like
            // shared_ptr) instead of deep-copying
            inline bool is_coroutine_handle() const noexcept;
            // Registered-class T& binding: the live C++ object (the held object stays null). Non-owning!
            inline void* bound_object() const noexcept;
            inline detail::object_ref keep_alive() const;
            inline bool has_keep_alive() const noexcept;
            inline std::shared_ptr<void> lock_weak_data() const;
            // A host holder's own (its C++ object, its anchors): an instance has none of them
            inline host_holder& host() noexcept;
            inline const host_holder& host() const noexcept;
            void set_keep_alive(detail::object_ref pin);
            void set_weak_data(std::weak_ptr<void> object);
            void hold(detail::object_ref object) noexcept;
            void mark_coroutine_handle() noexcept;
            void bind_object(void* object) noexcept;

            static constexpr uint8_t k_host = 0;
            static constexpr uint8_t k_instance = 1;
            // The last strong handle's release (detail::strong_last_release): an instance dies as one, a host holder destroys
            // and goes back to its pool
            static void last_release(object_holder* holder, detail::control_block_base* cb) noexcept;
        protected:
            object_holder() = default;
            object_holder(uint8_t kind, engine* eng) noexcept : owner_(eng), kind_(kind) {}
            object_holder(const object_holder&) = delete;
            object_holder& operator=(const object_holder&) = delete;
            ~object_holder() = default;

            uint64_t type_id_ = UINT64_MAX;  // the type name's interned id (UINT64_MAX = not set)
            engine* owner_ = nullptr;   // the engine that made this object: copies and class lookups run against it
            uint8_t kind_ = k_host;
        };
        // A bound C++ object's holder: the object (owned, or bound by address), and the non-owning forms' anchors
        struct host_holder : object_holder {
            host_holder() = default;
            std::string_view type_name_;   // the type's name: the engine's interned text or a literal (never an owned string)
            std::shared_ptr<void> data;
            bool is_coroutine_handle = false;
            void* bound_ptr = nullptr;
            // The non-owning forms' anchors, boxed so an owning object pays one pointer. weak_data: an extended object's
            // native object, reachable weakly so shared_ptr extraction can still hand out
            // ownership while it lives. keep_alive: a non-owning reference's receiver (a C++ method returning a reference
            // into it, `return *this`), pinned without taking part in extraction (the bound object stays the target).
            struct host_links {
                std::weak_ptr<void> weak_data;
                detail::object_ref keep_alive;
            };
            std::unique_ptr<host_links> links;
            host_links& linked() {
                if (!links) { links = std::make_unique<host_links>(); }
                return *links;
            }
        };

	private:
        // C++ primitive/string/opaque binding box (variant alternative 14).
        // IMMUTABLE after construction - copies of bound values stay aliases.
        struct cpp_bound_holder {
            void*   ptr = nullptr;        // address of the live C++ variable
            uint8_t size_and_sign = 0;    // low 7 bits: sizeof(T); bit 7: unsigned
            uint8_t semantic_index = 0;   // TYPEID_NULL/INT/FLOAT/STRING/CHAR/BOOL - the old shadow index
            engine* owner = nullptr;      // the engine that bound it: a decoded temporary is stamped from it
            cpp_bound_holder(void* p, uint8_t ss, uint8_t sem, engine* o) noexcept
                : ptr(p), size_and_sign(ss), semantic_index(sem), owner(o) {}
        };

        // Reference wrapper for reference types. Owner-pinned modes, mutually exclusive -
        // a reference NEVER holds a raw address + lifetime guess, so escape is legal (CELL,
        // MAP-ENTRY and CPP-MEMBER / PROPERTY here; ELEMENT, FIELD, STRING-ELEMENT and
        // COMPONENT at their members below):
        //
        // CELL (Lua-upvalue box): the holder OWNS the referenced value in inline
        // storage - one make_strong allocation is the whole box. Escape-marked
        // variables store a cell reference from declaration; unmarked storage boxes on
        // demand at the first ref bind. Binding/aliasing is a handle copy and an
        // escaped reference keeps its target alive.
        //
        // MAP-ENTRY: container_map pins the owning map (strong) and the inline storage
        // holds the KEY; deref/assign-through re-resolve via find(key) each time, so an
        // erased entry errors cleanly and a reference into a temporary map keeps the
        // map alive.
        //
        // CPP-MEMBER / PROPERTY (binder .property() members): the
        // holder keeps the receiver handle and the property's field id. CPP-MEMBER aims a
        // cpp_bound box in the inline storage at the C++ member itself (object_pin is the
        // object's lifetime); PROPERTY re-runs the getter into the inline scratch on every
        // deref and the setter on every assign-through.
        struct property_binding {
            strong_ptr<object_holder> receiver;
            type_info_ptr receiver_tag = nullptr;
            detail::object_ref object_pin;
            uint64_t field_id = 0;
            bool live_member = false;   // CPP-MEMBER: inline storage holds the bound box
            bool has_scratch = false;   // PROPERTY: inline storage holds the last getter result
            // REFERENT (live_member): inline storage holds the handle to a bound object a host returned by reference; an
            // assignment copy-assigns into the object
            void* referent = nullptr;
            const class_definition* referent_class = nullptr;
        };
        struct reference_holder {
            type_info_ptr container_element_type = nullptr;  // For subscript/field refs: the element/field type constraint

            static constexpr size_t cell_storage_size = 16;   // == sizeof(script_value), gated in value.cpp
            alignas(8) unsigned char cell_storage[cell_storage_size];
            // Only an unresolved mixed-mode call uses this: cell() is its evaluated value snapshot, while this
            // retained source supplies reference/shared-handle binding after overload selection. Never script-visible.
            std::unique_ptr<detail::argument_capture_storage> argument_source;
            bool has_cell = false;      // inline storage holds the OWNED value
            bool has_map_key = false;   // inline storage holds the map KEY (container_map set)
            strong_ptr<script_map> container_map;
            // the key's entry as the last touch found it, valid while the map's generation holds (script_map::recall)
            mutable uint64_t map_memo_gen = 0;
            mutable const void* map_memo_entry = nullptr;
            script_value* map_entry_value() const;   // value.cpp: the entry's live value, null when the key is gone
            script_value* cell() noexcept { return reinterpret_cast<script_value*>(cell_storage); }
            const script_value* cell() const noexcept { return reinterpret_cast<const script_value*>(cell_storage); }

            reference_holder() = default;
            reference_holder(const reference_holder&) = delete;
            reference_holder& operator=(const reference_holder&) = delete;
            ~reference_holder();   // destroys the inline value/key (value.cpp: script_value complete there)

            // ELEMENT mode: for references into a std::vector element (range-for
            // `auto&`, `arr[i]` lvalue): hold the OWNING container + index instead of a
            // raw element pointer. A raw pointer into a vector dangles when the vector
            // reallocates (e.g. a push inside the loop body) -> heap corruption on
            // write-through. When `container` is set, deref()/assign_through()
            // recompute the element address from container+index each time (surviving
            // reallocation) and bounds-check the index (a shrink throws instead of
            // reading freed memory). The strong_ptr also keeps the vector alive.
            strong_ptr<script_array> container;
            size_t container_index = SIZE_MAX;

            // TYPED element mode (raw-buffer nodes): there is no script_value element to
            // reference, so deref() materializes container[index] into cell_storage
            // (free in element mode) refreshed on EVERY touch — same re-resolve contract
            // as the other modes; callers already may not cache the address. Writers
            // never go through deref on typed elements: assign_through()/the typed
            // chokepoints store into the raw buffer (a scratch write-back would be
            // silent data loss — audited).
            bool has_elem_scratch = false;
            bool typed_element() const noexcept { return container && container->is_typed(); }
            const script_value& materialize_typed_element(engine* eng);   // value.cpp

            // FIELD mode (ref bind of obj.field): pins the owning instance and
            // re-resolves the field node by id on every deref/assign-through, so it
            // survives hot-reload field migration (a removed field errors instead of
            // dangling).
            // The instance is pinned by its shared ownership
            class_instance* owner_instance = nullptr;
            strong_ptr<class_instance> owner_shared;
            uint64_t field_id = UINT64_MAX;
            // the field's slot as the owner's class assigned it (a slot never moves within a class; a re-class changes the class, a
            // reload bumps its field-shape epoch): field_cell() reads the slot while both match, else finds the field by id and
            // remembers its slot (never from a parallel worker)
            mutable const class_definition* field_class = nullptr;
            mutable uint64_t field_class_epoch = 0;
            mutable uint32_t field_slot = UINT32_MAX;
            script_value* field_cell() const;   // value.cpp: the field's live cell, null when the field is gone
            // The array this reference was minted THROUGH (chain rooted at arr[i]; null when
            // not): counted in its live_elem_refs like a direct element reference.
            strong_ptr<script_array> elem_root;

            // CPP-MEMBER / PROPERTY modes (null in every other mode; checked last on
            // every touch so those pay nothing). resolve_target() answers null for both:
            // a missed consumer fails loudly instead of detaching the box or writing the
            // scratch. value.cpp owns the touch logic (materialize/store re-resolve the
            // receiver's class and accessor per touch: reload-safe, shadow-aware).
            std::unique_ptr<property_binding> property;
            // a reference whose dereference cannot raise: the owned cell, no container, field, map key or property behind it
            bool plain_cell() const noexcept { return has_cell && !container && !container_map && !owner_instance && !property; }
            bool property_ref() const noexcept { return property && !property->live_member; }
            const script_value& materialize_property(engine* eng);                       // throws on a failed getter
            checked_result<script_value> read_property(engine* eng, bool bind_point) const;   // checked twin
            void store_property(const script_value& value, engine* eng);                // setter; no setter = no-op
            void store_referent(const script_value& value, engine* eng);                // copy assignment into the referent
            // STRING-ELEMENT mode: string_base pins the string's storage (another holder),
            // container_index names the byte / utf8 character; deref() materializes it
            // into the inline scratch, stores replace it through detail/string_element.hpp.
            strong_ptr<reference_holder> string_base;
            bool string_element() const noexcept { return static_cast<bool>(string_base); }
            const script_value& materialize_string_element(engine* eng);   // value.cpp; throws when the character is gone
            // COMPONENT mode: component_base pins the math value's storage (another holder), container_index names the
            // component; deref() materializes it into the inline scratch, stores go through the component store
            // (detail/math_values.hpp store_component) into the value the base names, then commit the base.
            strong_ptr<reference_holder> component_base;
            bool component_element() const noexcept { return static_cast<bool>(component_base); }
            const script_value& materialize_component(engine* eng);   // value.cpp; throws when the base is no math value
            void store_component(const script_value& value, engine* eng);   // value.cpp; throws the component store's text
            // Scratch-backed modes (typed element, PROPERTY, STRING-ELEMENT, COMPONENT): deref() hands
            // back the holder's inline scratch, so in-place mutators (++/--) must
            // commit_scratch() after writing - the buffer store / the setter call.
            bool scratch_backed() const noexcept { return typed_element() || property_ref() || string_element() || component_element(); }
            void commit_scratch();

            // Mode-based write-target resolution for the immediate-use assignment
            // paths: re-resolves every time (realloc/erase safe), single level - never
            // follows a chained reference (matching the old raw-target twins). Returns
            // null when the element/entry/field is gone. Defined in value.cpp
            // (class_instance complete there).
            script_value* resolve_target();

            engine* owner = nullptr;   // the engine that minted this reference (after the hot fields): materializing and storing run against it
        };

        // Per-engine block pools for reference_holder and object_holder mints (value.cpp, state behind the engine's
        // pool slots): a mint is a pop + placement new instead of a heap allocation. While a parallel region's workers
        // run, acquire takes plain make_strong blocks that never touch the pool.
        static strong_ptr<reference_holder> acquire_reference_holder(engine* eng);
        static strong_ptr<object_holder> acquire_object_holder(engine* eng);   // a host holder
        // An instance's handle as an object value's storage: the same object, the same count
        static strong_ptr<object_holder> instance_storage(strong_ptr<class_instance> instance) noexcept;
        struct cell_cycle_registry;
        
        
        // Tag type for invalid values
        struct invalid_tag {};

        // Parallel captured-read borrow payload (variant alternative 15): ONE tagged raw
        // pointer to an engine-owned container that a parallel region proved read-only.
        // Trivially copyable ON PURPOSE - copying a borrow must never touch a refcount
        // (see make_parallel_borrow). Low bit = kind (0 array, 1 map), the next = sliced, the third = a shared object's
        // instance; container and instance allocations are 8+ aligned so the bits are free.
        // Inside a region no worker touches a shared instance's count: every read through an object borrow yields a borrow
        // or a worker-local copy, never the instance's handle.
        struct parallel_borrow_tag {
            uintptr_t bits = 0;
            static constexpr uintptr_t k_map_bit = 1;
            static constexpr uintptr_t k_sliced_bit = 2;
            static constexpr uintptr_t k_object_bit = 4;
            const void* pointer() const noexcept { return reinterpret_cast<const void*>(bits & ~(k_map_bit | k_sliced_bit | k_object_bit)); }
            bool is_map_kind() const noexcept { return (bits & k_map_bit) != 0; }
            bool is_sliced() const noexcept { return (bits & k_sliced_bit) != 0; }
            bool is_object_kind() const noexcept { return (bits & k_object_bit) != 0; }
        };

        // Variant indices for ultra-fast unchecked access
        // IMPORTANT: Keep this enum in sync with the storage variant below!
        enum class storage_index : size_t {
            jai_null = 0,
            jai_int = 1,
            jai_float = 2,
            jai_string = 3,
            jai_char = 4,
            jai_bool = 5,
            jai_array = 6,
            jai_map = 7,
            jai_object = 8,
            jai_function = 9,
            jai_reference = 10,
            jai_shared_ptr = 11,
            jai_weak_ptr = 12,
            jai_invalid = 13,
            jai_cpp_bound = 14,
            jai_parallel_borrow = 15,
            jai_utf8_string = 16
        };

    public:
        // Constexpr type index constants for fast type checking in hot paths
        // Use with raw_storage_index() to avoid switch statements
        static constexpr size_t TYPEID_NULL = 0;
        static constexpr size_t TYPEID_INT = 1;
        static constexpr size_t TYPEID_FLOAT = 2;
        static constexpr size_t TYPEID_STRING = 3;
        static constexpr size_t TYPEID_CHAR = 4;
        static constexpr size_t TYPEID_BOOL = 5;
        static constexpr size_t TYPEID_ARRAY = 6;
        static constexpr size_t TYPEID_MAP = 7;
        static constexpr size_t TYPEID_OBJECT = 8;
        static constexpr size_t TYPEID_FUNCTION = 9;
        static constexpr size_t TYPEID_REFERENCE = 10;
        // 11 is reserved (a retired alternative), so the indices after it keep their numbers
        static constexpr size_t TYPEID_WEAK_PTR = 12;
        static constexpr size_t TYPEID_INVALID = 13;
        static constexpr size_t TYPEID_CPP_BOUND = 14;
        static constexpr size_t TYPEID_PARALLEL_BORROW = 15;
        static constexpr size_t TYPEID_UTF8_STRING = 16;
        // math::vec2 / vec3 / vec4 / ivec2 / ivec3: one pooled copy-on-write node type,
        // script_vec_node (component count, element type, components), under one storage index per kind, so the index
        // names the kind and current_type() stays a constant per row
        static constexpr size_t TYPEID_VEC2 = 17;
        static constexpr size_t TYPEID_IVEC2 = 18;
        static constexpr size_t TYPEID_VEC3 = 19;
        static constexpr size_t TYPEID_IVEC3 = 20;
        static constexpr size_t TYPEID_VEC4 = 21;
        static constexpr size_t TYPEID_COLOR = 22;   // math::color: the same node, r g b a
        static constexpr bool is_math_index(size_t idx) noexcept { return idx >= TYPEID_VEC2 && idx <= TYPEID_COLOR; }
        static constexpr uint8_t vec_index_of(uint8_t count, uint8_t integral, uint8_t color) noexcept {
            if (color) { return static_cast<uint8_t>(TYPEID_COLOR); }
            if (integral) { return static_cast<uint8_t>(count == 2 ? TYPEID_IVEC2 : TYPEID_IVEC3); }
            return static_cast<uint8_t>(count == 2 ? TYPEID_VEC2 : count == 3 ? TYPEID_VEC3 : TYPEID_VEC4);
        }

        // The cpp_bound box encoding of a C++ type - make_cpp_bound and the binder's
        // member-address accessors share it so the two cannot drift. `capable` is false for
        // types the box cannot decode (the binder then registers no member accessor).
        template<typename T>
        struct cpp_bound_encoding {
            static constexpr bool capable =
                !std::is_const_v<T> &&
                (std::is_same_v<T, bool> || std::is_integral_v<T> ||
                 std::is_same_v<T, float> || std::is_same_v<T, double> ||
                 std::is_same_v<T, std::string>);
            static constexpr uint8_t size_and_sign =
                static_cast<uint8_t>(sizeof(T)) |
                ((std::is_unsigned_v<T> && !std::is_same_v<T, bool>) ? uint8_t{0x80} : uint8_t{0});
            static constexpr uint8_t semantic_index =
                std::is_same_v<T, bool> ? static_cast<uint8_t>(TYPEID_BOOL)
                : std::is_integral_v<T> ? static_cast<uint8_t>(TYPEID_INT)   // char included: a 1-byte int view
                : std::is_floating_point_v<T> ? static_cast<uint8_t>(TYPEID_FLOAT)
                : (std::is_same_v<T, std::string> || std::is_same_v<T, script_string>) ? static_cast<uint8_t>(TYPEID_STRING)
                : static_cast<uint8_t>(TYPEID_NULL);
        };
    private:

        // Type-erased storage: a hand-rolled tagged union with the EXACT layout and
        // alternative indexes std::variant had (8B max payload + index, 16B total).
        // The variant's runtime-dispatched member machinery (visitor _Destroy, generic
        // operator=) was the vm's #1 WPR bucket; here every special member is a
        // raw-index switch, scalar paths are a single mask test, and moves are word
        // relocations — every alternative is a steal-and-null single-pointer handle or
        // a trivial scalar, so relocation runs no effectful ctor/dtor. RAII on OWNED
        // payloads is exact: handle copies increment, destroys decrement, in the same
        // places the variant did them. get<I>/get_if<I> callers keep the check-then-get
        // discipline (never bad_variant_access).
        class storage {
        public:
            storage() noexcept : hdr_(TYPEID_NULL) {}
            storage(std::monostate) noexcept : hdr_(TYPEID_NULL) {}
            storage(script_int v) noexcept : hdr_(TYPEID_INT) { p_.i = v; }
            storage(script_float v) noexcept : hdr_(TYPEID_FLOAT) { p_.f = v; }
            storage(script_char v) noexcept : hdr_(TYPEID_CHAR) { p_.c = v; }
            storage(script_bool v) noexcept : hdr_(TYPEID_BOOL) { p_.b = v; }
            storage(invalid_tag) noexcept : hdr_(TYPEID_INVALID) {}
            storage(parallel_borrow_tag v) noexcept : hdr_(TYPEID_PARALLEL_BORROW) { p_.pb = v; }
            storage(strong_ptr<script_string> v) noexcept : hdr_(TYPEID_STRING) { new (&p_.s) strong_ptr<script_string>(std::move(v)); }
            storage(strong_ptr<script_array> v) noexcept : hdr_(TYPEID_ARRAY) { new (&p_.a) strong_ptr<script_array>(std::move(v)); }
            storage(strong_ptr<script_map> v) noexcept : hdr_(TYPEID_MAP) { new (&p_.m) strong_ptr<script_map>(std::move(v)); }
            storage(strong_ptr<object_holder> v) noexcept : hdr_(TYPEID_OBJECT) { new (&p_.o) strong_ptr<object_holder>(std::move(v)); }
            storage(strong_ptr<script_function_node> v) noexcept : hdr_(TYPEID_FUNCTION) { new (&p_.fn) strong_ptr<script_function_node>(std::move(v)); }
            storage(strong_ptr<reference_holder> v) noexcept : hdr_(TYPEID_REFERENCE) { new (&p_.r) strong_ptr<reference_holder>(std::move(v)); }
            storage(jai::weaker_ptr<object_holder> v) noexcept : hdr_(TYPEID_WEAK_PTR) { new (&p_.w) jai::weaker_ptr<object_holder>(std::move(v)); }
            storage(strong_ptr<cpp_bound_holder> v) noexcept : hdr_(TYPEID_CPP_BOUND) { new (&p_.cb) strong_ptr<cpp_bound_holder>(std::move(v)); }
            storage(strong_ptr<script_utf8> v) noexcept : hdr_(TYPEID_UTF8_STRING) { new (&p_.u) strong_ptr<script_utf8>(std::move(v)); }
            // precondition: a live node (its count and element type name the index)
            storage(strong_ptr<script_vec_node> v) noexcept : hdr_(vec_index_of(v->count, v->integral, v->color)) { new (&p_.vn) strong_ptr<script_vec_node>(std::move(v)); }
            JAI_FORCEINLINE strong_ptr<script_vec_node>& vec_node() noexcept { return p_.vn; }
            JAI_FORCEINLINE const strong_ptr<script_vec_node>& vec_node() const noexcept { return p_.vn; }

            JAI_FORCEINLINE storage(const storage& o) noexcept : hdr_(o.hdr_) {
                if ((k_trivial_mask >> idx_()) & 1u) { std::memcpy(static_cast<void*>(&p_), &o.p_, sizeof(p_)); }   // scalars: one test, no call
                else { copy_handle(o); }
            }
            JAI_NOINLINE void copy_handle(const storage& o) noexcept {
                switch (idx_()) {
                    case TYPEID_STRING: new (&p_.s) strong_ptr<script_string>(o.p_.s); break;
                    case TYPEID_ARRAY: new (&p_.a) strong_ptr<script_array>(o.p_.a); break;
                    case TYPEID_MAP: new (&p_.m) strong_ptr<script_map>(o.p_.m); break;
                    case TYPEID_OBJECT: new (&p_.o) strong_ptr<object_holder>(o.p_.o); break;
                    case TYPEID_FUNCTION: new (&p_.fn) strong_ptr<script_function_node>(o.p_.fn); break;
                    case TYPEID_REFERENCE: new (&p_.r) strong_ptr<reference_holder>(o.p_.r); break;
                    case TYPEID_WEAK_PTR: new (&p_.w) jai::weaker_ptr<object_holder>(o.p_.w); break;
                    case TYPEID_CPP_BOUND: new (&p_.cb) strong_ptr<cpp_bound_holder>(o.p_.cb); break;
                    case TYPEID_UTF8_STRING: new (&p_.u) strong_ptr<script_utf8>(o.p_.u); break;
                    case TYPEID_VEC2: case TYPEID_IVEC2: case TYPEID_VEC3: case TYPEID_IVEC3: case TYPEID_VEC4: case TYPEID_COLOR: new (&p_.vn) strong_ptr<script_vec_node>(o.p_.vn); break;
                    default: std::memcpy(static_cast<void*>(&p_), &o.p_, sizeof(p_)); break;   // trivial payloads
                }
            }
            // Relocating move: every alternative is a trivial scalar or a steal-and-null
            // single-pointer handle, so the payload word moves and the source demotes to
            // null — no effectful ctor/dtor is elided, refcounts are untouched (one live
            // handle before, one after). Source lands at TYPEID_NULL (script_value's
            // move contract nulls the source anyway).
            storage(storage&& o) noexcept : hdr_(o.hdr_) {
                std::memcpy(static_cast<void*>(&p_), &o.p_, sizeof(p_));
                o.set_idx_(TYPEID_NULL);
            }
            // Both assignments pin/steal o first (o may live inside our own payload: x = x.deref() through a map-entry
            // reference holding the last pin on a temporary map) and release the old payload LAST, after the new one is
            // in place: that release may run a destructor that stores to this very slot or moves the buffer holding it
            // (C++ shared_ptr assignment order), so nothing here touches *this after it.
            storage& operator=(const storage& o) noexcept {
                if (this != &o) {
                    storage pinned(o);
                    storage doomed(std::move(*this));
                    set_idx_(pinned.idx_());
                    std::memcpy(static_cast<void*>(&p_), &pinned.p_, sizeof(p_));
                    pinned.set_idx_(TYPEID_NULL);
                }
                return *this;
            }
            storage& operator=(storage&& o) noexcept {
                if (this != &o) {
                    const auto stolen_index = o.idx_();
                    payload stolen;
                    std::memcpy(static_cast<void*>(&stolen), &o.p_, sizeof(p_));
                    o.set_idx_(TYPEID_NULL);
                    storage doomed(std::move(*this));
                    set_idx_(stolen_index);
                    std::memcpy(static_cast<void*>(&p_), &stolen, sizeof(p_));
                }
                return *this;
            }
            // The value's own move and assignments: the whole word (index and tag) travels with the payload, the source
            // lands at a zero word, and the old payload still dies last
            struct whole_word {};
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"   // a scalar's unused payload bytes travel with it
#endif
            storage(storage&& o, whole_word) noexcept : hdr_(o.hdr_) {
                std::memcpy(static_cast<void*>(&p_), &o.p_, sizeof(p_));
                o.hdr_ = 0;
            }
            void assign_whole(storage&& o) noexcept {
                if (this != &o) {
                    const uint64_t word = o.hdr_;
                    payload stolen;
                    std::memcpy(static_cast<void*>(&stolen), &o.p_, sizeof(p_));
                    o.hdr_ = 0;
                    storage doomed(std::move(*this));
                    hdr_ = word;
                    std::memcpy(static_cast<void*>(&p_), &stolen, sizeof(p_));
                }
            }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
            void assign_whole(const storage& o) noexcept {
                if (this != &o) {
                    storage pinned(o);
                    storage doomed(std::move(*this));
                    hdr_ = pinned.hdr_;
                    std::memcpy(static_cast<void*>(&p_), &pinned.p_, sizeof(p_));
                    pinned.hdr_ = 0;
                }
            }
            template <typename T>
                requires (!std::is_same_v<std::remove_cvref_t<T>, storage> &&
                          std::is_constructible_v<storage, T&&>)
            storage& operator=(T&& v) noexcept {
                return *this = storage(std::forward<T>(v));
            }
            JAI_FORCEINLINE ~storage() {
                if (!((k_trivial_mask >> idx_()) & 1u)) { destroy_handle(); }   // scalars: one test, no call
            }

            JAI_FORCEINLINE size_t index() const noexcept { return idx_(); }
            JAI_FORCEINLINE uintptr_t tag() const noexcept { return static_cast<uintptr_t>(hdr_ & ~uint64_t{0xFF}); }
            JAI_FORCEINLINE void set_tag(uintptr_t bits) noexcept { hdr_ = (hdr_ & uint64_t{0xFF}) | static_cast<uint64_t>(bits); }

            // Index -> alternative TYPE map (must mirror alt()'s payload dispatch).
            // Explicit return types below instead of deduced auto: script_value's
            // non-template member bodies above use get_if/get lexically BEFORE these
            // definitions, which deduced-return functions forbid ([dcl.spec.auto]) -
            // MSVC's delayed parse tolerated it, Clang/GCC reject it.
            template <size_t I> using alt_t = std::tuple_element_t<I, std::tuple<
                std::monostate, script_int, script_float, strong_ptr<script_string>,
                script_char, script_bool, strong_ptr<script_array>, strong_ptr<script_map>,
                strong_ptr<object_holder>, strong_ptr<script_function_node>, strong_ptr<reference_holder>,
                std::monostate, jai::weaker_ptr<object_holder>, invalid_tag,
                strong_ptr<cpp_bound_holder>, parallel_borrow_tag, strong_ptr<script_utf8>,
                strong_ptr<script_vec_node>, strong_ptr<script_vec_node>, strong_ptr<script_vec_node>, strong_ptr<script_vec_node>,
                strong_ptr<script_vec_node>, strong_ptr<script_vec_node>>>;

            template <size_t I> JAI_FORCEINLINE alt_t<I>* get_if() noexcept {
                return idx_() == I ? &alt<I>() : nullptr;
            }
            template <size_t I> JAI_FORCEINLINE const alt_t<I>* get_if() const noexcept {
                return idx_() == I ? &alt<I>() : nullptr;
            }
            template <typename T> JAI_FORCEINLINE T* get_if() noexcept {
                return idx_() == index_of<T>() ? &alt<index_of<T>()>() : nullptr;
            }
            template <typename T> JAI_FORCEINLINE const T* get_if() const noexcept {
                return idx_() == index_of<T>() ? &alt<index_of<T>()>() : nullptr;
            }
            // Precondition: the alternative is engaged (callers keep check-then-get)
            template <size_t I> JAI_FORCEINLINE alt_t<I>& get() noexcept { assert(idx_() == I); return alt<I>(); }
            template <size_t I> JAI_FORCEINLINE const alt_t<I>& get() const noexcept { assert(idx_() == I); return alt<I>(); }
            template <typename T> JAI_FORCEINLINE T& get() noexcept { assert(idx_() == index_of<T>()); return alt<index_of<T>()>(); }
            template <typename T> JAI_FORCEINLINE const T& get() const noexcept { assert(idx_() == index_of<T>()); return alt<index_of<T>()>(); }
            // the payload word as raw bits (an int, a float's bits, a handle's pointer), whatever the index
            JAI_FORCEINLINE int64_t payload_bits() const noexcept { int64_t b; std::memcpy(&b, &p_, sizeof b); return b; }
            JAI_FORCEINLINE void set_payload_bits(int64_t b) noexcept { std::memcpy(static_cast<void*>(&p_), &b, sizeof b); }

        private:
            union alignas(8) payload {   // 8-aligned on 32-bit hosts too, where int64_t may align to 4
                payload() noexcept {}
                ~payload() noexcept {}
                script_int i;
                script_float f;
                script_char c;
                script_bool b;
                strong_ptr<script_string> s;
                strong_ptr<script_array> a;
                strong_ptr<script_map> m;
                strong_ptr<object_holder> o;
                strong_ptr<script_function_node> fn;
                strong_ptr<reference_holder> r;
                jai::weaker_ptr<object_holder> w;
                strong_ptr<cpp_bound_holder> cb;
                parallel_borrow_tag pb;              // tagged raw pointer, trivially copyable
                strong_ptr<script_utf8> u;
                strong_ptr<script_vec_node> vn;
            };

            static constexpr uint32_t k_trivial_mask =
                (1u << TYPEID_NULL) | (1u << TYPEID_INT) | (1u << TYPEID_FLOAT) |
                (1u << TYPEID_CHAR) | (1u << TYPEID_BOOL) | (1u << TYPEID_INVALID) |
                (1u << TYPEID_PARALLEL_BORROW);
            static_assert(TYPEID_COLOR < 32, "k_trivial_mask is one bit per storage index");

            JAI_NOINLINE void destroy_handle() noexcept {
                switch (idx_()) {
                    case TYPEID_STRING: p_.s.~strong_ptr(); break;
                    case TYPEID_ARRAY: p_.a.~strong_ptr(); break;
                    case TYPEID_MAP: p_.m.~strong_ptr(); break;
                    case TYPEID_OBJECT: p_.o.~strong_ptr(); break;
                    case TYPEID_FUNCTION: p_.fn.~strong_ptr(); break;
                    case TYPEID_REFERENCE: p_.r.~strong_ptr(); break;
                    case TYPEID_WEAK_PTR: p_.w.~weaker_ptr(); break;
                    case TYPEID_CPP_BOUND: p_.cb.~strong_ptr(); break;
                    case TYPEID_UTF8_STRING: p_.u.~strong_ptr(); break;
                    case TYPEID_VEC2: case TYPEID_IVEC2: case TYPEID_VEC3: case TYPEID_IVEC3: case TYPEID_VEC4: case TYPEID_COLOR: p_.vn.~strong_ptr(); break;
                    default: break;
                }
            }

            // The index -> alternative map (monostate and invalid_tag are EMPTY types;
            // statics stand in so get_if<0/13> stays expressible)
            static inline std::monostate null_alt_{};
            static inline invalid_tag invalid_alt_{};
            template <size_t I> JAI_FORCEINLINE auto& alt() noexcept {
                if constexpr (I == TYPEID_NULL) { return null_alt_; }
                else if constexpr (I == TYPEID_INT) { return p_.i; }
                else if constexpr (I == TYPEID_FLOAT) { return p_.f; }
                else if constexpr (I == TYPEID_STRING) { return p_.s; }
                else if constexpr (I == TYPEID_CHAR) { return p_.c; }
                else if constexpr (I == TYPEID_BOOL) { return p_.b; }
                else if constexpr (I == TYPEID_ARRAY) { return p_.a; }
                else if constexpr (I == TYPEID_MAP) { return p_.m; }
                else if constexpr (I == TYPEID_OBJECT) { return p_.o; }
                else if constexpr (I == TYPEID_FUNCTION) { return p_.fn; }
                else if constexpr (I == TYPEID_REFERENCE) { return p_.r; }
                else if constexpr (I == TYPEID_WEAK_PTR) { return p_.w; }
                else if constexpr (I == TYPEID_INVALID) { return invalid_alt_; }
                else if constexpr (I == TYPEID_CPP_BOUND) { return p_.cb; }
                else if constexpr (I == TYPEID_UTF8_STRING) { return p_.u; }
                else if constexpr (I >= TYPEID_VEC2 && I <= TYPEID_COLOR) { return p_.vn; }
                else { return p_.pb; }
            }
            template <size_t I> JAI_FORCEINLINE const auto& alt() const noexcept {
                return const_cast<storage*>(this)->alt<I>();
            }
            template <typename T> static constexpr size_t index_of() noexcept {
                if constexpr (std::is_same_v<T, std::monostate>) { return TYPEID_NULL; }
                else if constexpr (std::is_same_v<T, script_int>) { return TYPEID_INT; }
                else if constexpr (std::is_same_v<T, script_float>) { return TYPEID_FLOAT; }
                else if constexpr (std::is_same_v<T, strong_ptr<script_string>>) { return TYPEID_STRING; }
                else if constexpr (std::is_same_v<T, script_char>) { return TYPEID_CHAR; }
                else if constexpr (std::is_same_v<T, script_bool>) { return TYPEID_BOOL; }
                else if constexpr (std::is_same_v<T, strong_ptr<script_array>>) { return TYPEID_ARRAY; }
                else if constexpr (std::is_same_v<T, strong_ptr<script_map>>) { return TYPEID_MAP; }
                else if constexpr (std::is_same_v<T, strong_ptr<object_holder>>) { return TYPEID_OBJECT; }
                else if constexpr (std::is_same_v<T, strong_ptr<script_function_node>>) { return TYPEID_FUNCTION; }
                else if constexpr (std::is_same_v<T, strong_ptr<reference_holder>>) { return TYPEID_REFERENCE; }
                else if constexpr (std::is_same_v<T, jai::weaker_ptr<object_holder>>) { return TYPEID_WEAK_PTR; }
                else if constexpr (std::is_same_v<T, invalid_tag>) { return TYPEID_INVALID; }
                else if constexpr (std::is_same_v<T, strong_ptr<cpp_bound_holder>>) { return TYPEID_CPP_BOUND; }
                else if constexpr (std::is_same_v<T, strong_ptr<script_utf8>>) { return TYPEID_UTF8_STRING; }
                else { static_assert(std::is_same_v<T, parallel_borrow_tag>, "not a storage alternative"); return TYPEID_PARALLEL_BORROW; }
            }

            static_assert(std::endian::native == std::endian::little, "the index is the tag word's low byte");
            // the index is written as the whole word, never a byte store: a later 8-byte read of the word would miss store forwarding
            JAI_FORCEINLINE void set_idx_(uint8_t index) noexcept { hdr_ = (hdr_ & ~uint64_t{0xFF}) | index; }
            JAI_FORCEINLINE uint8_t idx_() const noexcept { return *reinterpret_cast<const uint8_t*>(&hdr_); }

            payload p_;
            uint64_t hdr_;   // low byte: the index; above it the owner's tag bits (script_value::stamp)
        };
        static_assert(std::is_nothrow_move_constructible_v<storage> && std::is_nothrow_move_assignable_v<storage>,
                      "every alternative must stay nothrow-move (the noexcept move contract depends on it)");
        static_assert(sizeof(storage) == 16 && alignof(storage) == 8,
                      "storage must keep the variant's exact 16-byte layout");

        storage storage_;

    public:

        // Access raw storage for AST literals (bypasses type checking)
        const storage& get_storage() const { return storage_; }

        // In-place scalar payload writes for the vm's slot-landing fast paths — the
        // caller has PROVEN raw_storage_index() matches; tag/engine/kind stay untouched
        void unchecked_set_int_payload(script_int v) noexcept { storage_.get<script_int>() = v; }
        void unchecked_set_float_payload(script_float v) noexcept { storage_.get<script_float>() = v; }
        void unchecked_set_bool_payload(script_bool v) noexcept { storage_.get<script_bool>() = v; }
        // the payload word's raw bits: a trivial payload's int, float bits or 0/1 bool (the vm's cells hold exactly these words)
        JAI_FORCEINLINE int64_t unchecked_payload_bits() const noexcept { return storage_.payload_bits(); }
        JAI_FORCEINLINE void unchecked_set_payload_bits(int64_t b) noexcept { storage_.set_payload_bits(b); }

        // A vec (caller proved the index): its node, its kind, its components read out, and one component written in place
        // (binary32 bits for vec kinds, int32 for ivec kinds; a node shared with another value is unshared first, so a copy
        // never sees the write); tag, engine and stamp stay untouched
        const script_vec_node& unchecked_vec_node() const noexcept { return *storage_.vec_node(); }
        script_value_type unchecked_vec_kind() const noexcept { return current_type(); }
        bool unchecked_vec_node_shared() const noexcept { return !storage_.vec_node().unique(); }
        void unchecked_set_vec_word(size_t i, uint32_t bits) {
            strong_ptr<script_vec_node>& node = storage_.vec_node();
            if (!node.unique()) [[unlikely]] { unshare_vec_node(); }
            node->words[i] = bits;
        }
        bool is_math() const { return is_math_index(deref().raw_storage_index()); }
        // A vec's truthiness (caller proved the index): not all zero, the numeric rule (NaN is nonzero, as for floats)
        bool math_truthy() const noexcept;   // value.cpp, out of line: the truthiness switches stay small
        // A fresh node from the engine's pool (a plain allocation without an engine or inside a parallel region)
        static strong_ptr<script_vec_node> acquire_vec_node(engine* eng);
        
        // Extract object_holder for class_instance operations
        // Returns nullptr if not actually storing an object (uses current_type, not defined_type)
        strong_ptr<object_holder> get_object_holder() {
            auto t = current_type();  // Use actual current type, not declared type
            if (t == script_value_type::jai_object_type || t == script_value_type::jai_shared_ptr_type) {
                // After refactor: both object and shared_ptr store object_holder directly
                // shared_ptr<T> is just a type marker affecting clone behavior
                return storage_.get<strong_ptr<object_holder>>();
            }
            return nullptr;
        }

        const strong_ptr<object_holder> get_object_holder() const {
            auto t = current_type();  // Use actual current type, not declared type
            if (t == script_value_type::jai_object_type || t == script_value_type::jai_shared_ptr_type) {
                // After refactor: both object and shared_ptr store object_holder directly
                // shared_ptr<T> is just a type marker affecting clone behavior
                return storage_.get<strong_ptr<object_holder>>();
            }
            return nullptr;
        }

		// Immediate read-only access without copying the strong handle. The caller
		// must keep this value alive and must not retain the pointer across mutation.
		const object_holder* peek_object_holder() const {
			const auto& value = deref();
			auto* holder = value.storage_.get_if<strong_ptr<object_holder>>();
			return holder ? holder->get() : nullptr;
		}

        // Set object holder directly (used when restoring from weak_ptr.lock())
        void set_object_holder(strong_ptr<object_holder> holder) {
            storage_ = holder;
        }

        // Get the array NODE handle for interpreter operations
        JAI_FORCEINLINE strong_ptr<script_array>& get_array_storage() {
            if (type() != script_value_type::jai_array_type) [[unlikely]] { detail::throw_conversion_error(*this, "array"); }
            return storage_.get<strong_ptr<script_array>>();
        }

        // Get raw map storage for interpreter operations
        strong_ptr<script_map>& get_map_storage() {
            if (type() != script_value_type::jai_map_type) {
                detail::throw_conversion_error(deref(), "map");
            }
            return storage_.get<strong_ptr<script_map>>();
        }

        // Get raw weak_ptr storage for interpreter operations
        jai::weaker_ptr<object_holder>& get_weak_ptr_storage() {
            if (type() != script_value_type::jai_weak_ptr_type) {
                detail::throw_conversion_error(deref(), "weak_ptr");
            }
            return storage_.get<jai::weaker_ptr<object_holder>>();
        }
        
        // ===== Safe Public APIs for Interpreter Operations =====
        
        // Factory method for null values with engine pointer
        static script_value make_null(engine* eng) {
            return script_value(std::monostate{}, eng);
        }

        // Factory method for invalid values (used as sentinel for non-existent fields/methods)
        static script_value make_invalid(engine* eng);
        

        // Safe access to reference holder for reference types
        // current_type(): a reference's type_info may name a different base_type
        JAI_FORCEINLINE reference_holder* get_reference_holder() {
            if (raw_storage_index() == TYPEID_REFERENCE) {
                return storage_.get<strong_ptr<reference_holder>>().get();
            }
            return nullptr;
        }

        JAI_FORCEINLINE const reference_holder* get_reference_holder() const {
            if (raw_storage_index() == TYPEID_REFERENCE) {
                return storage_.get<strong_ptr<reference_holder>>().get();
            }
            return nullptr;
        }

        // A STRING-ELEMENT holder's pin as a reference value (value.cpp)
        static script_value make_string_element_pin(const reference_holder& holder, engine* eng);
        // A COMPONENT holder's pin as a reference value (value.cpp)
        static script_value make_component_pin(const reference_holder& holder, engine* eng);

        // Non-null iff *this is a reference whose deref() hands back the holder's inline
        // scratch (a TYPED array element, a C++ PROPERTY or a STRING-ELEMENT): in-place
        // mutators must commit_scratch() after writing.
        reference_holder* scratch_ref_holder() noexcept {
            if (raw_storage_index() != TYPEID_REFERENCE) { return nullptr; }
            auto& holder = *storage_.get_if<TYPEID_REFERENCE>();
            return holder && holder->scratch_backed() ? holder.get() : nullptr;
        }

        // The boxed value of a CELL reference this handle alone holds (raw index REFERENCE
        // only): a dying frame slot may move it out (detail/return_conv.hpp). Null for a
        // cell anyone else shares and for every other holder mode.
        script_value* unique_cell_payload() noexcept {
            auto& holder = *storage_.get_if<TYPEID_REFERENCE>();
            return holder && holder.unique() && holder->has_cell && !holder->container &&
                   !holder->container_map && !holder->owner_instance && !holder->property ? holder->cell() : nullptr;
        }
        
        
        // The ELEMENT-mode holder this handle ALONE owns (raw index REFERENCE; container set, no
        // map key, cell, field or property): a by-ref range-for re-points it at the next index
        // instead of minting a holder per iteration (the scratch view refreshes on the next
        // touch: view_current misses on a different slot). Null when anyone else shares the
        // holder - a retained alias keeps its element - and for every other mode.
        reference_holder* element_cursor_holder() noexcept {
            if (raw_storage_index() != TYPEID_REFERENCE) { return nullptr; }
            auto& holder = *storage_.get_if<TYPEID_REFERENCE>();
            return holder && holder.unique() && holder->container && !holder->has_map_key && !holder->has_cell &&
                   !holder->owner_instance && !holder->property ? holder.get() : nullptr;
        }
        // The MAP-ENTRY holder this handle alone owns: a structured range-for re-aims it at the next entry instead of minting a
        // holder per iteration. Null when anyone else shares the holder and for every other mode.
        reference_holder* map_entry_cursor_holder() noexcept {
            if (raw_storage_index() != TYPEID_REFERENCE) { return nullptr; }
            auto& holder = *storage_.get_if<TYPEID_REFERENCE>();
            return holder && holder.unique() && holder->has_map_key ? holder.get() : nullptr;
        }

        // The parallel_for chunk cursor: an ELEMENT-mode reference the worker's chunk loop
        // shares with the body call, re-pointed at the next slot after each element. Unlike
        // element_cursor_holder this ignores sharing - the wrapper and the body hold copies
        // by design; an admitted body cannot retain one past its iteration.
        bool element_cursor_advance() noexcept { return element_cursor_advance(0, 1); }
        // The chunk's step: blocks of `block` consecutive elements, every stride_blocks-th block
        // belonging to this cursor (block 0 = the contiguous chunk)
        bool element_cursor_advance(size_t block, size_t stride_blocks) noexcept {
            if (raw_storage_index() != TYPEID_REFERENCE) { return false; }
            auto& holder = *storage_.get_if<TYPEID_REFERENCE>();
            if (!holder || !holder->container || holder->has_map_key) { return false; }
            size_t next = holder->container_index + 1;
            if (block && stride_blocks > 1 && next % block == 0) { next += (stride_blocks - 1) * block; }
            holder->container_index = next;
            return true;
        }
        bool element_cursor_set(size_t index) noexcept {
            if (raw_storage_index() != TYPEID_REFERENCE) { return false; }
            auto& holder = *storage_.get_if<TYPEID_REFERENCE>();
            if (!holder || !holder->container || holder->has_map_key) { return false; }
            holder->container_index = index;
            return true;
        }
        size_t element_cursor_index() const noexcept {
            if (raw_storage_index() != TYPEID_REFERENCE) { return SIZE_MAX; }
            auto& holder = *storage_.get_if<TYPEID_REFERENCE>();
            return holder && holder->container ? holder->container_index : SIZE_MAX;
        }

        // Safe weak_ptr access
        jai::weaker_ptr<object_holder> get_weak_ptr() const {
            // a null typed weak_ptr<T> (`w = null`) holds no weak handle
            if (const auto* weak = storage_.get_if<jai::weaker_ptr<object_holder>>()) {
                return *weak;
            }
            return jai::weaker_ptr<object_holder>();
        }
        
        // Unchecked mutable object storage accessor - caller must verify raw object storage first
        // (count inspection without the handle copy get_object_holder() takes)
        inline strong_ptr<object_holder>& unchecked_get_object_storage() noexcept {
            return *storage_.get_if<TYPEID_OBJECT>();
        }

        // The class instance this value holds, borrowed (no handle copy): the caller keeps this value alive and does not keep
        // the pointer past it
        class_instance* peek_class_instance() const noexcept {
            const auto* holder = storage_.get_if<strong_ptr<object_holder>>();   // an object's storage alone holds one
            if (!holder || !*holder || !(*holder)->is_instance()) { return nullptr; }
            return (*holder)->unchecked_instance();
        }

        // Safe class_instance extraction from object_holder
        strong_ptr<class_instance> get_class_instance() {
            JAI_CENSUS(holder_inspect);
            auto holder = get_object_holder();
            if (holder && holder->is_instance()) {
                return holder->instance_handle();
            }
            return nullptr;
        }
        
        
        
        // Create an empty weak_ptr value
        static script_value make_empty_weak_ptr(type_info_ptr weak_ptr_type, engine* eng);

        // Create a weak_ptr from an object
        // Create a weak_ptr from an object (implemented in value.cpp)
        // Returns checked_result to avoid throwing exceptions
        static checked_result<script_value> make_weak_ptr(const script_value& obj, engine* eng);
        
        // Set type info for special cases (like weak_ptr creation)
        JAI_FORCEINLINE void set_type_info(type_info_ptr type) {
            if (!type || storage_.tag() != reinterpret_cast<uintptr_t>(type.get())) { set_stamp(type); }   // an unchanged stamp writes nothing
            if (raw_storage_index() == TYPEID_ARRAY) [[unlikely]] { demote_after_stamp(type); }
        }
        JAI_NOINLINE void demote_after_stamp(type_info_ptr type) {
            // Demote-on-stamp: re-tagging an array value 'any'
            // (var binding) or an open-element array tag (array<var>: the boundary
            // kernel rebuilds first, this is the net) demotes a typed node in place —
            // node kind must never outrun the views' tags. The stamp sites operate on
            // fresh clones/moved temps, so the node is unique here; a shared demote
            // would still be value-correct (identity + contents preserved), merely
            // losing the raw-buffer form.
            if (type &&
                (type->base_type == script_value_type::jai_any_type ||
                 (type->base_type == script_value_type::jai_array_type &&
                  (!type->element_type() || type->element_type()->base_type == script_value_type::jai_any_type)))) {
                auto& node = *storage_.get_if<TYPEID_ARRAY>();
                if (node && node->is_typed()) {
                    node->demote_to_hetero(node->owner);
                }
            }
        }
        
    private:
        // Friends only for essential access patterns
        friend class interpreter;  // For coroutine handle creation (object_holder access)
        friend class environment;  // dies_at_teardown follows a reference without reading through it
        template<typename T> friend class dynamic_binder;  // For make_cpp_object
        friend strong_ptr<class_instance> detail::make_native_instance(class_definition&, std::string_view, std::shared_ptr<void>, engine*);
        friend class serialization::binary_archive_writer;  // For storage_ access
        friend class serialization::binary_archive_reader;  // For storage_ access
        friend class serialization::json_archive_writer;    // For storage_ access
        friend class serialization::json_archive_reader;    // For storage_ access
        
        // STL container friends for direct assignment support
        template<typename K, typename V, typename C, typename A> friend class std::map;
        template<typename T1, typename T2> friend struct std::pair;
        template<typename... Types> friend class std::tuple;
        template<typename K, typename C, typename A> friend class std::set;
    };

    inline script_value::host_holder& script_value::object_holder::host() noexcept { return *static_cast<host_holder*>(this); }
    inline const script_value::host_holder& script_value::object_holder::host() const noexcept { return *static_cast<const host_holder*>(this); }
    inline void* script_value::object_holder::object() const noexcept { return is_instance() ? static_cast<void*>(unchecked_instance()) : host().data.get(); }
    inline bool script_value::object_holder::holds_object() const noexcept { return is_instance() || host().data != nullptr; }
    inline long script_value::object_holder::object_use_count() const noexcept {
        return is_instance() ? static_cast<long>(detail::instance_use_count(unchecked_instance())) : host().data.use_count();
    }
    inline const std::shared_ptr<void>& script_value::object_holder::host_object() const noexcept {
        static const std::shared_ptr<void> none;
        return is_instance() ? none : host().data;
    }
    inline detail::object_ref script_value::object_holder::held() const noexcept {
        return is_instance() ? detail::object_ref(instance_handle()) : detail::object_ref(host().data);
    }
    inline bool script_value::object_holder::is_coroutine_handle() const noexcept { return !is_instance() && host().is_coroutine_handle; }
    inline void* script_value::object_holder::bound_object() const noexcept { return is_instance() ? nullptr : host().bound_ptr; }
    inline detail::object_ref script_value::object_holder::keep_alive() const {
        return !is_instance() && host().links ? host().links->keep_alive : detail::object_ref();
    }
    inline bool script_value::object_holder::has_keep_alive() const noexcept { return !is_instance() && host().links && host().links->keep_alive; }
    inline std::shared_ptr<void> script_value::object_holder::lock_weak_data() const {
        return !is_instance() && host().links ? host().links->weak_data.lock() : nullptr;
    }
    inline void script_value::object_holder::set_keep_alive(detail::object_ref pin) {
        assert(!is_instance());
        if (pin || host().links) { host().linked().keep_alive = std::move(pin); }
    }
    inline void script_value::object_holder::set_weak_data(std::weak_ptr<void> object) { assert(!is_instance()); host().linked().weak_data = std::move(object); }
    inline void script_value::object_holder::hold(detail::object_ref object) noexcept { assert(!is_instance() && !object.is_instance()); host().data = object.shared(); }
    inline void script_value::object_holder::mark_coroutine_handle() noexcept { assert(!is_instance()); host().is_coroutine_handle = true; }
    inline void script_value::object_holder::bind_object(void* object) noexcept { assert(!is_instance()); host().bound_ptr = object; }

    namespace detail {
        struct argument_capture_storage {
            script_value lvalue;       // original evaluated reference/source for T&
            script_value shared;       // evaluated handle identity for shared_ptr<T>, even if the source is rebound
            bool minted = false;       // lvalue is the reference the argument's evaluation bound (detail::early_bound_reference)
        };
    }


    // 8B payload + 8B tag word (index byte, engine-only flag, the type_info or engine pointer) on every host
    static_assert(sizeof(script_value) == 16 && alignof(script_value) == 8,
                  "script_value must stay 16 bytes / 8-aligned");
    static_assert(std::is_nothrow_move_constructible_v<script_value>);

    // A value handed out of the engine (execute, execute_file, get_variable, hold): it keeps its payload until its engine is
    // destroyed, never the engine itself. The engine's teardown releases the payload while the engine still runs destructors and
    // leaves the held value outlived: valid() is false, get_engine() is null and every read throws "Value outlived its engine."
    // A copy into a plain script_value is a borrow, valid while the engine lives
    class held_value : public script_value {
    public:
        held_value(script_value value, engine* holder);
        held_value(const held_value& other);
        held_value(held_value&& other) noexcept;
        held_value& operator=(const held_value& other);
        held_value& operator=(held_value&& other) noexcept;
        // a plain value assigned in: a heap payload registers the held value with its engine, any other value keeps the held one's
        held_value& operator=(script_value value);
        ~held_value();

        bool valid() const noexcept { return !outlived_; }
        // the engine the held value is registered with; null once outlived or moved from
        engine* get_engine() const noexcept { return outlived_ ? nullptr : engine_; }
        const script_value& value() const { check_alive(); return *this; }

        template<typename T> T as() const { check_alive(); return script_value::template as<T>(); }
        template<typename T> T as() {
            check_alive();
            if constexpr (std::is_same_v<T, script_string&>) {
                return script_value::as_string_ref(engine_);   // a host write through the held value is charged to its engine
            } else {
                return script_value::template as<T>();
            }
        }
        script_int as_int() const { check_alive(); return script_value::as_int(); }
        script_float as_float() const { check_alive(); return script_value::as_float(); }
        const script_string& as_string() const { check_alive(); return script_value::as_string(); }
        script_bool as_bool() const { check_alive(); return script_value::as_bool(); }
        script_char as_char() const { check_alive(); return script_value::as_char(); }
        const std::vector<script_value>& as_array() const { check_alive(); return script_value::as_array(); }
        std::vector<script_value>& as_array() { check_alive(); return script_value::as_array(); }
        const script_map& as_map() const { check_alive(); return script_value::as_map(); }
        script_map& as_map() { check_alive(); return script_value::as_map(); }
        const script_function& as_function() const { check_alive(); return script_value::as_function(); }
        std::string to_string() const { check_alive(); return script_value::to_string(); }
        const script_value& deref() const { check_alive(); return script_value::deref(); }
        script_value& deref() { check_alive(); return script_value::deref(); }
        script_value_type type() const { check_alive(); return script_value::type(); }
        operator int() const { check_alive(); return script_value::operator int(); }
        operator int64_t() const { check_alive(); return script_value::operator int64_t(); }
        operator double() const { check_alive(); return script_value::operator double(); }
        operator float() const { check_alive(); return script_value::operator float(); }

    private:
        friend class engine;
        void check_alive() const { if (outlived_) [[unlikely]] { throw_outlived(); } }
        void rebind(engine* holder, bool outlived);
        [[noreturn]] JAI_NOINLINE static void throw_outlived();
        engine* engine_ = nullptr;
        held_value* previous_ = nullptr;   // the engine's registry of held values: an intrusive list under its lock
        held_value* next_ = nullptr;
        bool outlived_ = false;
    };

    // ---- script_array kind-dispatched element access (script_value complete here) ----

    // Out-of-line SPECIAL MEMBERS + size/clear/reserve (types.hpp declares them):
    // script_value is complete here, so the vector<script_value> member
    // instantiations these trigger are legal on Clang/GCC, not just under MSVC's
    // deferred instantiation.
    struct obj_slot {
        strong_ptr<script_value::object_holder> handle;
        type_info_ptr tag;
    };

    inline void script_array::construct_lane() noexcept {
        switch (lane_of(kind_)) {
            case lane_t::values: new (&values_) std::vector<script_value>(); break;
            case lane_t::ints: new (&ints_) std::vector<script_int>(); break;
            case lane_t::floats: new (&floats_) std::vector<script_float>(); break;
            case lane_t::bytes: new (&bytes_) std::vector<uint8_t>(); break;
            case lane_t::strs: new (&strs_) std::vector<strong_ptr<script_string>>(); break;
            case lane_t::objs: new (&objs_) std::vector<obj_slot>(); break;
        }
    }
    namespace detail {
        template <class Buffer>
        inline constexpr bool elements_observe_death = std::is_same_v<typename Buffer::value_type, script_value> ||
                                                       std::is_same_v<typename Buffer::value_type, obj_slot>;
        // A buffer's elements die last to first, as a C++ array's do (std::vector leaves its order to the library)
        template <class Buffer>
        inline void drop_elements_from(Buffer& buffer, size_t n) noexcept {
            while (buffer.size() > n) { buffer.pop_back(); }
        }
        // ... and a cleared buffer is detached before they die, so a destructor sees the array already empty; the capacity
        // comes back unless a destructor refilled it
        template <class Buffer>
        inline void clear_elements(Buffer& buffer) noexcept {
            Buffer dying;
            dying.swap(buffer);
            drop_elements_from(dying, 0);
            if (buffer.empty()) { buffer.swap(dying); }
        }
    }
    inline void script_array::destroy_lane() noexcept {
        switch (lane_of(kind_)) {
            case lane_t::values: detail::drop_elements_from(values_, 0); values_.~vector(); break;
            case lane_t::ints: ints_.~vector(); break;
            case lane_t::floats: floats_.~vector(); break;
            case lane_t::bytes: bytes_.~vector(); break;
            case lane_t::strs: strs_.~vector(); break;
            case lane_t::objs: detail::drop_elements_from(objs_, 0); objs_.~vector(); break;
        }
    }
    // an empty lane of kind k replaces the current one; a kind on the same lane keeps the buffer
    inline void script_array::switch_lane(kind_t k) noexcept {
        if (lane_of(k) == lane_of(kind_)) { kind_ = k; return; }
        destroy_lane();
        kind_ = k;
        construct_lane();
    }
    inline script_array::script_array() : values_() {}
    inline script_array::script_array(kind_t k) : kind_(k) { construct_lane(); }

    inline script_array::script_array(const script_array& other) : live_elem_refs(other.live_elem_refs), kind_(other.kind_), owner(other.owner) {
        switch (lane_of(kind_)) {
            case lane_t::values: new (&values_) std::vector<script_value>(other.values_); break;
            case lane_t::ints: new (&ints_) std::vector<script_int>(other.ints_); break;
            case lane_t::floats: new (&floats_) std::vector<script_float>(other.floats_); break;
            case lane_t::bytes: new (&bytes_) std::vector<uint8_t>(other.bytes_); break;
            case lane_t::strs: new (&strs_) std::vector<strong_ptr<script_string>>(other.strs_); break;
            case lane_t::objs: new (&objs_) std::vector<obj_slot>(other.objs_); break;
        }
    }
    inline script_array::script_array(script_array&& other) noexcept : live_elem_refs(other.live_elem_refs), kind_(other.kind_), owner(other.owner) {
        switch (lane_of(kind_)) {
            case lane_t::values: new (&values_) std::vector<script_value>(std::move(other.values_)); break;
            case lane_t::ints: new (&ints_) std::vector<script_int>(std::move(other.ints_)); break;
            case lane_t::floats: new (&floats_) std::vector<script_float>(std::move(other.floats_)); break;
            case lane_t::bytes: new (&bytes_) std::vector<uint8_t>(std::move(other.bytes_)); break;
            case lane_t::strs: new (&strs_) std::vector<strong_ptr<script_string>>(std::move(other.strs_)); break;
            case lane_t::objs: new (&objs_) std::vector<obj_slot>(std::move(other.objs_)); break;
        }
    }
    inline script_array& script_array::operator=(const script_array& other) {
        if (this != &other) {
            script_array copy(other);
            *this = std::move(copy);
        }
        return *this;
    }
    inline script_array& script_array::operator=(script_array&& other) noexcept {
        if (this != &other) {
            destroy_lane();
            live_elem_refs = other.live_elem_refs;
            kind_ = other.kind_;
            owner = other.owner;
            switch (lane_of(kind_)) {
                case lane_t::values: new (&values_) std::vector<script_value>(std::move(other.values_)); break;
                case lane_t::ints: new (&ints_) std::vector<script_int>(std::move(other.ints_)); break;
                case lane_t::floats: new (&floats_) std::vector<script_float>(std::move(other.floats_)); break;
                case lane_t::bytes: new (&bytes_) std::vector<uint8_t>(std::move(other.bytes_)); break;
                case lane_t::strs: new (&strs_) std::vector<strong_ptr<script_string>>(std::move(other.strs_)); break;
                case lane_t::objs: new (&objs_) std::vector<obj_slot>(std::move(other.objs_)); break;
            }
        }
        return *this;
    }
    namespace detail {
        // A container's elements die depth first through nested arrays and maps without native recursion (script_map.hpp)
        inline void release_values_flattened(std::vector<script_value>& values) noexcept;
    }
    inline script_array::~script_array() {
        if (kind_ == kind_t::hetero) {
            for (const auto& v : values_) {
                if (v.release_flattens()) { detail::release_values_flattened(values_); break; }
            }
        }
        destroy_lane();
    }

    inline size_t script_array::size() const noexcept {
        switch (kind_) {
            case kind_t::i64: return ints_.size();
            case kind_t::f64: return floats_.size();
            case kind_t::b8:
            case kind_t::u8:
            case kind_t::i8:
            case kind_t::c8: return bytes_.size();
            case kind_t::str: return strs_.size();
            case kind_t::obj: return objs_.size();
            case kind_t::v2:
            case kind_t::iv2: return bytes_.size() / 8;
            case kind_t::v3:
            case kind_t::iv3: return bytes_.size() / 12;
            case kind_t::v4:
            case kind_t::rgba: return bytes_.size() / 16;
            default: return values_.size();
        }
    }
    inline void script_array::clear() noexcept {
        switch (lane_of(kind_)) {
            case lane_t::values: detail::clear_elements(values_); break;
            case lane_t::ints: ints_.clear(); break;
            case lane_t::floats: floats_.clear(); break;
            case lane_t::bytes: bytes_.clear(); break;
            case lane_t::strs: strs_.clear(); break;
            case lane_t::objs: detail::clear_elements(objs_); break;
        }
    }
    inline void script_array::reserve(size_t n) {
        switch (kind_) {
            case kind_t::i64: ints_.reserve(n); break;
            case kind_t::f64: floats_.reserve(n); break;
            case kind_t::b8:
            case kind_t::u8:
            case kind_t::i8:
            case kind_t::c8: bytes_.reserve(n); break;
            case kind_t::str: strs_.reserve(n); break;
            case kind_t::obj: objs_.reserve(n); break;
            case kind_t::v2: case kind_t::iv2: case kind_t::v3: case kind_t::iv3: case kind_t::v4: case kind_t::rgba:
                bytes_.reserve(n * vec_stride(kind_) * 4);
                break;
            default: values_.reserve(n); break;
        }
    }
	inline bool script_array::append_same_kind(const script_array& a_source) {
		if (kind_ != a_source.kind_ || kind_ == kind_t::hetero || kind_ == kind_t::obj) { return false; }
		auto append = [](auto& a_target, const auto& a_input) {
			const size_t count = a_input.size();
			if (&a_target == &a_input) {
				const size_t start = a_target.size();
				a_target.resize(start + count);
				std::copy_n(a_target.begin(), count, a_target.begin() + start);
			} else { a_target.insert(a_target.end(), a_input.begin(), a_input.end()); }
		};
		switch (kind_) {
			case kind_t::i64: append(ints_, a_source.ints_); break;
			case kind_t::f64: append(floats_, a_source.floats_); break;
			case kind_t::str: append(strs_, a_source.strs_); break;
			default: append(bytes_, a_source.bytes_); break;
		}
		return true;
	}

	inline void script_array::retain_range(size_t a_start, size_t a_end) {
		// the dropped elements die last to first: the tail, then the head
		auto retain = [&](auto& a_buffer, size_t a_stride = 1) {
			using buffer_t = std::remove_reference_t<decltype(a_buffer)>;
			detail::drop_elements_from(a_buffer, a_end * a_stride);
			if constexpr (detail::elements_observe_death<buffer_t>) {
				buffer_t head(std::make_move_iterator(a_buffer.begin()), std::make_move_iterator(a_buffer.begin() + a_start));
				a_buffer.erase(a_buffer.begin(), a_buffer.begin() + a_start);
				detail::drop_elements_from(head, 0);
			} else {
				a_buffer.erase(a_buffer.begin(), a_buffer.begin() + a_start * a_stride);
			}
		};
		switch (kind_) {
			case kind_t::i64: retain(ints_); break;
			case kind_t::f64: retain(floats_); break;
			case kind_t::str: retain(strs_); break;
			case kind_t::obj: retain(objs_); break;
			case kind_t::hetero: retain(values_); break;
			default: retain(bytes_, is_vec_kind(kind_) ? vec_stride(kind_) * 4 : 1); break;
		}
	}
	inline void script_array::retain_selected(const std::vector<uint8_t>& a_selected) {
		// the dropped elements die last to first, once the kept ones have closed ranks
		auto retain = [&](auto& a_buffer, size_t a_stride = 1) {
			using buffer_t = std::remove_reference_t<decltype(a_buffer)>;
			buffer_t dropped;
			size_t out = 0;
			const size_t count = std::min(a_selected.size(), a_buffer.size() / a_stride);
			for (size_t i = 0; i < count; ++i) {
				if (!a_selected[i]) {
					if constexpr (detail::elements_observe_death<buffer_t>) { dropped.push_back(std::move(a_buffer[i])); }
					continue;
				}
				if (out != i) {
					for (size_t lane = 0; lane < a_stride; ++lane) { a_buffer[out * a_stride + lane] = std::move(a_buffer[i * a_stride + lane]); }
				}
				++out;
			}
			if constexpr (detail::elements_observe_death<buffer_t>) {
				for (size_t i = count; i < a_buffer.size(); ++i) { dropped.push_back(std::move(a_buffer[i])); }
			}
			a_buffer.erase(a_buffer.begin() + out * a_stride, a_buffer.end());
			detail::drop_elements_from(dropped, 0);
		};
		switch (kind_) {
			case kind_t::i64: retain(ints_); break;
			case kind_t::f64: retain(floats_); break;
			case kind_t::str: retain(strs_); break;
			case kind_t::obj: retain(objs_); break;
			case kind_t::hetero: retain(values_); break;
			default: retain(bytes_, is_vec_kind(kind_) ? vec_stride(kind_) * 4 : 1); break;
		}
	}

    inline void script_array::assign_buffer(const script_array& other) {
        switch_lane(other.kind_);
        switch (other.kind_) {
            case kind_t::i64: ints_ = other.ints_; break;
            case kind_t::f64: floats_ = other.floats_; break;
            case kind_t::b8:
            case kind_t::u8:
            case kind_t::i8:
            case kind_t::c8:
            case kind_t::v2: case kind_t::iv2: case kind_t::v3: case kind_t::iv3: case kind_t::v4: case kind_t::rgba: bytes_ = other.bytes_; break;
            case kind_t::str: strs_ = other.strs_; break;   // handles share objects, like boxed copies
            case kind_t::obj: objs_ = other.objs_; break;   // handle copies; clone() deep-copies per slot (value.cpp)
            default: values_ = other.values_; break;
        }
        kind_ = other.kind_;
    }

    inline bool script_array::view_current(size_t i, const script_value& view) const noexcept {
        switch (kind_) {
            case kind_t::obj:
                return view.raw_storage_index() == script_value::TYPEID_OBJECT &&
                       view.unchecked_object_handle().get() == objs_[i].handle.get() &&
                       view.type_info_raw() == objs_[i].tag.get();
            case kind_t::str:
                return view.raw_storage_index() == script_value::TYPEID_STRING &&
                       view.unchecked_string_handle().get() == strs_[i].get();
            default: return false;
        }
    }

    inline script_value script_array::get(size_t i, engine* eng) const {
        switch (kind_) {
            case kind_t::i64: return script_value(ints_[i], eng);
            case kind_t::f64: return script_value(floats_[i], eng);
            case kind_t::b8: return script_value(static_cast<script_bool>(bytes_[i] != 0), eng);
            case kind_t::c8: return script_value(static_cast<script_char>(bytes_[i]), eng);
            case kind_t::u8: return script_value(static_cast<script_int>(bytes_[i]), eng);
            case kind_t::i8: return script_value(static_cast<script_int>(static_cast<int8_t>(bytes_[i])), eng);
            case kind_t::str: return script_value(strs_[i], eng);
            case kind_t::obj: return script_value(objs_[i].handle, objs_[i].tag, eng);
            case kind_t::v2: case kind_t::iv2: case kind_t::v3: case kind_t::iv3: case kind_t::v4: case kind_t::rgba: {
                uint32_t words[4];
                vec_words(i, words);
                return script_value::make_vec(vec_value_type(kind_), words, eng);
            }
            default: return values_[i];
        }
    }

    inline void script_array::set(size_t i, script_value v) {
        switch (kind_) {
            case kind_t::i64:
                ints_[i] = v.raw_storage_index() == script_value::TYPEID_FLOAT
                    ? static_cast<script_int>(v.unchecked_as_float()) : v.unchecked_as_int();
                break;
            case kind_t::f64:
                floats_[i] = v.raw_storage_index() == script_value::TYPEID_INT
                    ? static_cast<script_float>(v.unchecked_as_int()) : v.unchecked_as_float();
                break;
            case kind_t::b8:
                bytes_[i] = v.unchecked_as_bool() ? 1 : 0;   // pre-converted: exactly a bool
                break;
            case kind_t::c8:
                bytes_[i] = static_cast<uint8_t>(v.unchecked_as_char());   // pre-converted: exactly a char
                break;
            case kind_t::u8:
            case kind_t::i8:
                bytes_[i] = static_cast<uint8_t>(v.unchecked_as_int());   // pre-converted: an int in the kind's range
                break;
            case kind_t::str:
                strs_[i] = v.unchecked_string_handle();   // pre-converted: exactly a string
                break;
            case kind_t::obj:
                assert(v.raw_storage_index() == script_value::TYPEID_OBJECT && "object slot store: caller converts first");
                objs_[i] = obj_slot{v.unchecked_object_handle(), v.get_type_info()};   // pre-converted: exactly an object
                break;
            case kind_t::v2: case kind_t::iv2: case kind_t::v3: case kind_t::iv3: case kind_t::v4: case kind_t::rgba:
                assert(v.raw_storage_index() == vec_storage_index(kind_) && "vector slot store: caller converts first");
                set_vec_words(i, v.unchecked_vec_node().words);
                break;
            default:
                values_[i] = std::move(v);
                break;
        }
    }

    inline void script_array::push(script_value pre_converted) {
        switch (kind_) {
            case kind_t::i64:
                ints_.push_back(pre_converted.raw_storage_index() == script_value::TYPEID_FLOAT
                    ? static_cast<script_int>(pre_converted.unchecked_as_float()) : pre_converted.unchecked_as_int());
                break;
            case kind_t::f64:
                floats_.push_back(pre_converted.raw_storage_index() == script_value::TYPEID_INT
                    ? static_cast<script_float>(pre_converted.unchecked_as_int()) : pre_converted.unchecked_as_float());
                break;
            case kind_t::b8:
                bytes_.push_back(pre_converted.unchecked_as_bool() ? 1 : 0);
                break;
            case kind_t::c8:
                bytes_.push_back(static_cast<uint8_t>(pre_converted.unchecked_as_char()));
                break;
            case kind_t::u8:
            case kind_t::i8:
                bytes_.push_back(static_cast<uint8_t>(pre_converted.unchecked_as_int()));
                break;
            case kind_t::str:
                strs_.push_back(pre_converted.unchecked_string_handle());
                break;
            case kind_t::obj:
                assert(pre_converted.raw_storage_index() == script_value::TYPEID_OBJECT && "object slot push: caller converts first");
                objs_.push_back(obj_slot{pre_converted.unchecked_object_handle(), pre_converted.get_type_info()});
                break;
            case kind_t::v2: case kind_t::iv2: case kind_t::v3: case kind_t::iv3: case kind_t::v4: case kind_t::rgba:
                assert(pre_converted.raw_storage_index() == vec_storage_index(kind_) && "vector slot push: caller converts first");
                push_vec_words(pre_converted.unchecked_vec_node().words);
                break;
            default:
                values_.push_back(std::move(pre_converted));
                break;
        }
    }

    inline size_t script_array::capacity() const noexcept {
        switch (kind_) {
            case kind_t::i64: return ints_.capacity();
            case kind_t::f64: return floats_.capacity();
            case kind_t::b8:
            case kind_t::u8:
            case kind_t::i8:
            case kind_t::c8: return bytes_.capacity();
            case kind_t::str: return strs_.capacity();
            case kind_t::obj: return objs_.capacity();
            case kind_t::v2: case kind_t::iv2: case kind_t::v3: case kind_t::iv3: case kind_t::v4: case kind_t::rgba:
                return bytes_.capacity() / (vec_stride(kind_) * 4);
            default: return values_.capacity();
        }
    }

    inline void script_array::truncate(size_t n) {
        switch (kind_) {
            case kind_t::i64: if (n < ints_.size()) { ints_.resize(n); } break;
            case kind_t::f64: if (n < floats_.size()) { floats_.resize(n); } break;
            case kind_t::b8:
            case kind_t::u8:
            case kind_t::i8:
            case kind_t::c8: if (n < bytes_.size()) { bytes_.resize(n); } break;
            case kind_t::str: if (n < strs_.size()) { strs_.erase(strs_.begin() + static_cast<std::ptrdiff_t>(n), strs_.end()); } break;
            case kind_t::obj: detail::drop_elements_from(objs_, n); break;
            case kind_t::v2: case kind_t::iv2: case kind_t::v3: case kind_t::iv3: case kind_t::v4: case kind_t::rgba: {
                const size_t n_bytes = n * vec_stride(kind_) * 4;
                if (n_bytes < bytes_.size()) { bytes_.resize(n_bytes); }
                break;
            }
            default: detail::drop_elements_from(values_, n); break;
        }
    }

    inline void script_array::pop_back() {
        switch (kind_) {
            case kind_t::i64: ints_.pop_back(); break;
            case kind_t::f64: floats_.pop_back(); break;
            case kind_t::b8:
            case kind_t::u8:
            case kind_t::i8:
            case kind_t::c8: bytes_.pop_back(); break;
            case kind_t::str: strs_.pop_back(); break;
            case kind_t::obj: objs_.pop_back(); break;
            case kind_t::v2: case kind_t::iv2: case kind_t::v3: case kind_t::iv3: case kind_t::v4: case kind_t::rgba:
                bytes_.resize(bytes_.size() - vec_stride(kind_) * 4);
                break;
            default: values_.pop_back(); break;
        }
    }

    inline void script_array::erase_at(size_t i) {
        switch (kind_) {
            case kind_t::i64: ints_.erase(ints_.begin() + i); break;
            case kind_t::f64: floats_.erase(floats_.begin() + i); break;
            case kind_t::b8:
            case kind_t::u8:
            case kind_t::i8:
            case kind_t::c8: bytes_.erase(bytes_.begin() + i); break;
            case kind_t::str: strs_.erase(strs_.begin() + i); break;
            case kind_t::obj: objs_.erase(objs_.begin() + i); break;
            case kind_t::v2: case kind_t::iv2: case kind_t::v3: case kind_t::iv3: case kind_t::v4: case kind_t::rgba: {
                const size_t w = vec_stride(kind_) * 4;
                bytes_.erase(bytes_.begin() + static_cast<std::ptrdiff_t>(i * w), bytes_.begin() + static_cast<std::ptrdiff_t>((i + 1) * w));
                break;
            }
            default: values_.erase(values_.begin() + i); break;
        }
    }

    inline void script_array::reverse() {
        switch (kind_) {
            case kind_t::i64: std::reverse(ints_.begin(), ints_.end()); break;
            case kind_t::f64: std::reverse(floats_.begin(), floats_.end()); break;
            case kind_t::b8:
            case kind_t::u8:
            case kind_t::i8:
            case kind_t::c8: std::reverse(bytes_.begin(), bytes_.end()); break;
            case kind_t::str: std::reverse(strs_.begin(), strs_.end()); break;
            case kind_t::obj: std::reverse(objs_.begin(), objs_.end()); break;
            case kind_t::v2: case kind_t::iv2: case kind_t::v3: case kind_t::iv3: case kind_t::v4: case kind_t::rgba: {
                const size_t n = size();
                uint32_t a[4], b[4];
                for (size_t lo = 0, hi = n; lo + 1 < hi; ++lo, --hi) {
                    vec_words(lo, a);
                    vec_words(hi - 1, b);
                    set_vec_words(lo, b);
                    set_vec_words(hi - 1, a);
                }
                break;
            }
            default: std::reverse(values_.begin(), values_.end()); break;
        }
    }

    inline void script_array::swap_elements(size_t i, size_t j) {
        if (i == j) { return; }
        switch (kind_) {
            case kind_t::i64: std::swap(ints_[i], ints_[j]); break;
            case kind_t::f64: std::swap(floats_[i], floats_[j]); break;
            case kind_t::b8:
            case kind_t::u8:
            case kind_t::i8:
            case kind_t::c8: std::swap(bytes_[i], bytes_[j]); break;
            case kind_t::str: std::swap(strs_[i], strs_[j]); break;
            case kind_t::obj: std::swap(objs_[i], objs_[j]); break;
            case kind_t::v2: case kind_t::iv2: case kind_t::v3: case kind_t::iv3: case kind_t::v4: case kind_t::rgba: {
                uint32_t a[4], b[4];
                vec_words(i, a);
                vec_words(j, b);
                set_vec_words(i, b);
                set_vec_words(j, a);
                break;
            }
            default: std::swap(values_[i], values_[j]); break;
        }
    }

    template <typename F>
    inline void script_array::for_each(engine* eng, F&& f) const {
        switch (kind_) {
            case kind_t::i64: for (script_int n : ints_) { f(script_value(n, eng)); } break;
            case kind_t::f64: for (script_float d : floats_) { f(script_value(d, eng)); } break;
            case kind_t::b8: for (uint8_t b : bytes_) { f(script_value(static_cast<script_bool>(b != 0), eng)); } break;
            case kind_t::c8: for (uint8_t c : bytes_) { f(script_value(static_cast<script_char>(c), eng)); } break;
            case kind_t::u8: for (uint8_t b : bytes_) { f(script_value(static_cast<script_int>(b), eng)); } break;
            case kind_t::i8: for (uint8_t b : bytes_) { f(script_value(static_cast<script_int>(static_cast<int8_t>(b)), eng)); } break;
            case kind_t::str: for (const auto& h : strs_) { f(script_value(h, eng)); } break;
            case kind_t::obj: for (const auto& s : objs_) { f(script_value(s.handle, s.tag, eng)); } break;
            case kind_t::v2: case kind_t::iv2: case kind_t::v3: case kind_t::iv3: case kind_t::v4: case kind_t::rgba: {
                const size_t n = size();
                for (size_t i = 0; i < n; ++i) { f(get(i, eng)); }
                break;
            }
            default: for (const script_value& v : values_) { f(v); } break;
        }
    }

    inline std::vector<script_value> script_array::materialize_values(engine* eng) const {
        std::vector<script_value> out;
        const size_t n = size();
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) { out.push_back(get(i, eng)); }
        return out;
    }

    inline void script_array::demote_to_hetero(engine* eng) {
        if (kind_ == kind_t::hetero) { return; }
        std::vector<script_value> boxed;
        switch (kind_) {
            case kind_t::i64:
                boxed.reserve(ints_.size());
                for (script_int n : ints_) { boxed.emplace_back(n, eng); }
                break;
            case kind_t::f64:
                boxed.reserve(floats_.size());
                for (script_float f : floats_) { boxed.emplace_back(f, eng); }
                break;
            case kind_t::b8:
                boxed.reserve(bytes_.size());
                for (uint8_t b : bytes_) { boxed.emplace_back(static_cast<script_bool>(b != 0), eng); }
                break;
            case kind_t::c8:
                boxed.reserve(bytes_.size());
                for (uint8_t c : bytes_) { boxed.emplace_back(static_cast<script_char>(c), eng); }
                break;
            case kind_t::u8:
            case kind_t::i8:
                boxed.reserve(bytes_.size());
                for (uint8_t b : bytes_) {
                    boxed.emplace_back(kind_ == kind_t::u8 ? static_cast<script_int>(b) : static_cast<script_int>(static_cast<int8_t>(b)), eng);
                }
                break;
            case kind_t::str:
                boxed.reserve(strs_.size());
                for (auto& h : strs_) { boxed.emplace_back(std::move(h), eng); }
                break;
            case kind_t::obj:
                boxed.reserve(objs_.size());
                for (auto& s : objs_) { boxed.emplace_back(std::move(s.handle), s.tag, eng); }
                break;
            case kind_t::v2: case kind_t::iv2: case kind_t::v3: case kind_t::iv3: case kind_t::v4: case kind_t::rgba: {
                const size_t n = size();
                boxed.reserve(n);
                for (size_t i = 0; i < n; ++i) { boxed.push_back(get(i, eng)); }
                break;
            }
            default:
                break;
        }
        destroy_lane();
        kind_ = kind_t::hetero;
        new (&values_) std::vector<script_value>(std::move(boxed));
    }

    // Map-key order (operator<=>): two keys of the same raw int or string kind order by payload, the answer the type rank and
    // payload compare give; every other pair takes operator<=>
    inline bool script_value_map_less::operator()(const script_value& a, const script_value& b) const {
        const size_t ka = a.raw_storage_index();
        if (ka == b.raw_storage_index()) {
            if (ka == script_value::TYPEID_INT) { return a.unchecked_as_int() < b.unchecked_as_int(); }
            if (ka == script_value::TYPEID_STRING) { return a.unchecked_as_string() < b.unchecked_as_string(); }
        }
        return a < b;
    }

    inline checked_result<script_value> host_writeback_function::operator()(const std::vector<script_value>& args) const { return fn(args); }

    inline bool script_value::release_cascades() const noexcept {
        switch (storage_.index()) {
            case TYPEID_ARRAY: {
                const auto& a = storage_.get<TYPEID_ARRAY>();
                return a.unique() && (a->kind() == script_array::kind_t::hetero || a->kind() == script_array::kind_t::obj);
            }
            case TYPEID_OBJECT: {
                const auto& h = storage_.get<TYPEID_OBJECT>();
                return h.unique() && (h->holds_object() ? h->object_use_count() == 1 : h->has_keep_alive());
            }
            case TYPEID_MAP: return storage_.get<TYPEID_MAP>().unique();
            case TYPEID_FUNCTION: return storage_.get<TYPEID_FUNCTION>().unique();
            case TYPEID_REFERENCE: return storage_.get<TYPEID_REFERENCE>().unique();
            default: return false;
        }
    }

    inline bool script_value::release_flattens() const noexcept {
        switch (storage_.index()) {
            case TYPEID_ARRAY: {
                const auto& a = storage_.get<TYPEID_ARRAY>();
                return a.unique() && a->kind() == script_array::kind_t::hetero;
            }
            case TYPEID_MAP: return storage_.get<TYPEID_MAP>().unique();
            default: return false;
        }
    }

} // namespace jai

#include "script_map.hpp"

#endif // __JAISCRIPT_CORE_VALUE_HPP__
