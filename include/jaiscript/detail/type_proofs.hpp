#pragma once

#ifndef __JAISCRIPT_DETAIL_TYPE_PROOFS_HPP__
#define __JAISCRIPT_DETAIL_TYPE_PROOFS_HPP__

// The VM's identity guard for typed stores, declarations, arguments and returns: TRUE only
// when the skipped conversion is provably the identity on the value actually in hand. Guard failure = the conversion runs.

#include <jaiscript/core/value.hpp>
#include <cstdint>

namespace jai::detail {

    static_assert(script_value::TYPEID_INT == 1 && script_value::TYPEID_FLOAT == 2 && script_value::TYPEID_STRING == 3 &&
                  script_value::TYPEID_CHAR == 4 && script_value::TYPEID_BOOL == 5 && script_value::TYPEID_OBJECT == 8,
                  "type_proofs: storage indices moved - re-audit same_type_payload");

    // TRUE iff the unproven conversion for `target` is the identity on `v` (v deref-normalized):
    // mirrors the typed-slot table's identity rows (detail::convert_for_typed_slot_core: same-base
    // scalars/strings, the exact class-name tag) as every site consumes them. tag_agrees reproduces
    // defined_type(): a non-any tag must name the base.
    inline bool same_type_payload(const script_value& v, const type_info* target) noexcept {
        if (!target) return false;
        const size_t ri = v.raw_storage_index();
        const type_info* tag = v.get_type_info().get();
        const bool tag_agrees = !tag || tag->base_type == script_value_type::jai_any_type ||
                                tag->base_type == target->base_type;
        switch (target->base_type) {
            case script_value_type::jai_int_type:    return ri == script_value::TYPEID_INT && tag_agrees;
            case script_value_type::jai_float_type:  return ri == script_value::TYPEID_FLOAT && tag_agrees;
            case script_value_type::jai_bool_type:   return ri == script_value::TYPEID_BOOL && tag_agrees;
            case script_value_type::jai_char_type:   return ri == script_value::TYPEID_CHAR && tag_agrees;
            case script_value_type::jai_string_type: return ri == script_value::TYPEID_STRING && tag_agrees;
            case script_value_type::jai_object_type: return ri == script_value::TYPEID_OBJECT &&
                                                            tag == target && !target->type_name.empty();
            default: return false;   // any/null/array/map/shared_ptr/weak_ptr/reference/function: never
        }
    }

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_TYPE_PROOFS_HPP__
