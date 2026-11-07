#pragma once

#ifndef __JAISCRIPT_DETAIL_COPY_CENSUS_HPP__
#define __JAISCRIPT_DETAIL_COPY_CENSUS_HPP__

// The copy and front-loading census (a diagnostic build: JAISCRIPT_VM_COVERAGE). Every
// counted event records the address it ran at (the physical function and its inline chain, symbolized at exit), the vm
// instruction that was executing and the script line behind it. A deep copy also records whether its source was uniquely
// held and how soon the source and the copy died, in vm instructions. JAI_COPY_CENSUS=<file> writes the process totals at
// exit. A build without the define compiles every hook to nothing.

#ifdef JAISCRIPT_VM_COVERAGE

#include <cstddef>
#include <cstdint>
#if defined(_MSC_VER)
#define JAI_CENSUS_RA() _ReturnAddress()   // the using .cpp includes <intrin.h> (kept off this widely included header)
#else
#define JAI_CENSUS_RA() __builtin_return_address(0)
#endif

#define JAI_CENSUS_SITES(X) \
    X(deep_clone, "copy", "script_value::clone, top level (a deep copy; nested clones add weight)") \
    X(copy_assign, "copy", "class_copy_assign: a by-value class slot takes a source's members") \
    X(string_detach, "copy", "string copy-on-write detach (unshare_string_node)") \
    X(param_clone, "copy", "vm by-value parameter bind that deep-copies (flags: why no alias or elision)") \
    X(decl_clone, "copy", "vm declaration from an lvalue that deep-copies (flags: why no local alias)") \
    X(value_copy_ctor, "refcount", "script_value copy constructor of a refcounted kind (flags: raw index)") \
    X(value_copy_assign, "refcount", "script_value copy assignment of a refcounted kind (flags: raw index)") \
    X(rc_add, "refcount", "strong_ptr copy (a non-atomic increment and its later decrement)") \
    X(sym_intern, "lookup", "string_symbolizer::intern / intern_with_view (hash of the text)") \
    X(sym_find, "lookup", "string_symbolizer::find (hash of the text)") \
    X(sym_get_string, "lookup", "string_symbolizer::get_string (id to text)") \
    X(env_get_id, "lookup", "environment::get(id)") \
    X(env_get_name, "lookup", "environment::get / get_ref / assign / define / contains by name") \
    X(env_get_ref_id, "lookup", "environment::get_ref(id)") \
    X(env_assign_id, "lookup", "environment::assign(id)") \
    X(env_define_id, "lookup", "environment::define / define_variable (id)") \
    X(env_value_ptr, "lookup", "environment::get_value_ptr(id)") \
    X(env_vm_lookup, "lookup", "environment::vm_storage_lookup (the vm's uncached walk)") \
    X(env_contains, "lookup", "environment::contains(id)") \
    X(env_reset, "lookup", "environment::reset / reset_as_method / reset_as_static_method (a scope made per call)") \
    X(class_by_name, "lookup", "engine::get_class_definition(name)") \
    X(class_by_id, "lookup", "engine::get_class_definition(type id)") \
    X(type_info_intern, "lookup", "engine::get_type_info(temp) (a type interned by structure)") \
    X(type_info_by_name, "lookup", "engine::get_type_info_by_name") \
    X(field_slot_scan, "lookup", "class_definition::field_slot, small-class scan") \
    X(field_slot_hash, "lookup", "class_definition::field_slot, hashed") \
    X(get_method, "lookup", "class_definition::get_method(id) (flags: 1 found on a base)") \
    X(method_ptr, "lookup", "class_definition::method_ptr(id)") \
    X(user_accessor, "lookup", "class_definition::user_accessor(id) (a chain walk of hashed tables)") \
    X(single_overload, "lookup", "class_definition::single_method_overload(name)") \
    X(overload_resolve, "lookup", "class_definition::resolve_method_overload_core (hashed table, maybe a scoring pass)") \
    X(static_chain, "lookup", "class_definition::find_static_field_in_chain / find_static_method_in_chain") \
    X(chain_field, "lookup", "class_definition::chain_declares_field (a chain walk)") \
    X(call_function_object, "indirect", "a script_function (std::function) invoked through call_value") \
    X(call_native_lane, "indirect", "a host method's direct lane or a host direct entry, called through a function pointer") \
    X(map_key_hash, "lookup", "script_value::map_key_hash (flags: raw index of the key)") \
    X(typed_slot_convert, "lookup", "convert_for_typed_slot_core (flags: 1 identity, 2 converted, 3 rejected)") \
    X(container_validate, "lookup", "validate_container_homogeneous (a whole-container walk)") \
    X(compile_chunk, "lookup", "vm_compiler compile_* entry (a chunk built at run time)") \
    X(move_hit, "elision", "a copy a proof or a move avoided (flags: which)") \
    X(holder_inspect, "refcount", "get_class_instance: a holder and an atomic std::shared_ptr copied to read the instance")

namespace jai::census {

    enum class site : uint16_t {
#define JAI_CENSUS_ENUM(id, group, text) id,
        JAI_CENSUS_SITES(JAI_CENSUS_ENUM)
#undef JAI_CENSUS_ENUM
        count
    };

    // one event, attributed to the caller of hit (the physical function where the hook expanded)
    void hit(site s, uint32_t flags = 0) noexcept;
    // one event at an explicit address (a hook inside an out-of-line function passes its own return address)
    void hit_at(site s, const void* where, uint32_t flags) noexcept;
    // one event attributed to a name as well (the callee of a parameter bind, the declared name of a local)
    void hit_named(site s, const char* name, size_t name_size, uint32_t flags) noexcept;
    // the vm's position: the instruction about to run and its chunk row (coverage_general, coverage_cell)
    void set_position(uint16_t op, const void* row, uint32_t ip, const char* name, const char* file, uint32_t line) noexcept;

    // A deep copy in flight: the outermost scope is the event, nested scopes and add_weight add to its weight (the values
    // the copy produced). finish names the copy's node so its death can be timed; src is the source's node (null: none).
    struct copy_scope {
        site s;
        const void* where;
        const void* src;
        uint32_t flags;
        bool outer;
        copy_scope(site s, const void* where, const void* src, uint32_t flags) noexcept;
        ~copy_scope();
        void finish(const void* dst) noexcept;
        copy_scope(const copy_scope&) = delete;
        copy_scope& operator=(const copy_scope&) = delete;
    private:
        const void* dst_ = nullptr;
    };
    void add_weight(uint64_t n) noexcept;

    // flags of a deep copy: bit 0 the source was uniquely held, bits 4..7 the copied kind
    inline constexpr uint32_t k_src_unique = 1;
    enum class copied_kind : uint32_t { other = 0, array_obj, array_typed, array_values, map, instance, cpp_object, shared_deep, reference, string };
    inline constexpr uint32_t kind_flag(copied_kind k) noexcept { return static_cast<uint32_t>(k) << 4; }

    // param_clone / decl_clone reasons (the low byte)
    inline constexpr uint32_t k_no_alias_fn = 1, k_ref_escaping = 2, k_alias_disallowed = 3, k_alias_unproven = 4,
                              k_no_alias_home = 5, k_local_alias_refused = 6;
    // param_clone elision reasons (bits 8..11)
    inline constexpr uint32_t k_elide_not_stack = 1 << 8, k_elide_reference = 2 << 8, k_elide_shared = 3 << 8, k_elide_other_node = 4 << 8;

} // namespace jai::census

#define JAI_CENSUS(s) ::jai::census::hit(::jai::census::site::s)
#define JAI_CENSUS_F(s, f) ::jai::census::hit(::jai::census::site::s, static_cast<uint32_t>(f))

#else

#define JAI_CENSUS(s) ((void)0)
#define JAI_CENSUS_F(s, f) ((void)0)

#endif // JAISCRIPT_VM_COVERAGE

#endif // __JAISCRIPT_DETAIL_COPY_CENSUS_HPP__
