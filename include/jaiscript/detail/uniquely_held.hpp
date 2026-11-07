#pragma once

#ifndef __JAISCRIPT_DETAIL_UNIQUELY_HELD_HPP__
#define __JAISCRIPT_DETAIL_UNIQUELY_HELD_HPP__

#include <jaiscript/core/value.hpp>
#include <jaiscript/detail/environment.hpp>

namespace jai::detail {

    // A prvalue: an aggregate no other handle, slot, field or element holds. C++17 initializes the destination from it
    // directly (a by-value parameter, a replaced slot, a fresh element), so moving it there runs one destructor where a
    // copy would run two
    inline bool uniquely_held(const script_value& v) noexcept {
        switch (v.raw_storage_index()) {
        case script_value::TYPEID_OBJECT: {
            const auto& holder = v.unchecked_object_handle();
            return holder && holder.use_count() == 1 && holder->is_instance() && holder->object_use_count() == 1;
        }
        case script_value::TYPEID_ARRAY: return v.unchecked_get_array_storage() && v.unchecked_get_array_storage().use_count() == 1;
        case script_value::TYPEID_MAP: return v.unchecked_get_map_storage() && v.unchecked_get_map_storage().use_count() == 1;
        default: return false;
        }
    }

    // A replacing store's value: a prvalue (uniquely held) moves in and is the stored object, anything else is the
    // assignment copy
    inline script_value take_for_assignment(script_value& value) {
        if (uniquely_held(value)) { JAI_CENSUS_F(move_hit, 6); return std::move(value); }
        return clone_for_assignment(value);
    }

    // A slot declared a scalar kind takes an object through its conversion (to_int(), to_string(), ...), which reads the object itself
    // as C++'s conversion operator does: the store never copies the object first (a non-copyable one converts too)
    inline bool slot_converts_object(const script_value& value, const type_info* declared) noexcept {
        if (!declared || value.raw_storage_index() != script_value::TYPEID_OBJECT) { return false; }
        switch (declared->base_type) {
        case script_value_type::jai_int_type: case script_value_type::jai_float_type: case script_value_type::jai_bool_type:
        case script_value_type::jai_char_type: case script_value_type::jai_string_type: case script_value_type::jai_utf8_string_type:
            return true;
        default:
            return false;
        }
    }

}

#endif
