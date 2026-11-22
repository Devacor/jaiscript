#pragma once

#ifndef __JAISCRIPT_DETAIL_UNIQUELY_HELD_HPP__
#define __JAISCRIPT_DETAIL_UNIQUELY_HELD_HPP__

#include <jaiscript/core/value.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/lvalue_shape.hpp>

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

    // A value no slot holds, of a script class or a host class
    inline bool object_is_prvalue(const script_value& v) noexcept {
        if (v.raw_storage_index() != script_value::TYPEID_OBJECT) { return false; }
        const auto& holder = v.unchecked_object_handle();
        return holder && holder.use_count() == 1 && (holder->holds_object() ? holder->object_use_count() == 1 : holder->has_keep_alive());
    }

    // What a handle takes from a class value: a handle never aliases a value slot, so a prvalue is the handle's object and a value
    // a slot holds is copied, as make_shared<T>(value) copies it. A handle stays the handle
    inline void value_into_handle(script_value& value) {
        if (value.raw_storage_index() != script_value::TYPEID_OBJECT || value.is_shared_ptr_type() || object_is_prvalue(value)) { return; }
        script_value copy = clone_for_assignment(value);
        value = std::move(copy);
    }

    // A class value bound for a handle slot (a declaration, a store, an element, a field): no implicit conversion, as in C++; the
    // class's name for the refusal, empty when the value is a handle, null or no class value
    std::string value_for_handle_slot(const script_value& value_in);   // value.cpp
    [[noreturn]] JAI_NOINLINE inline void refuse_value_into_handle(const std::string& class_name) {
        std::string text(value_into_handle_text);
        for (size_t at = text.find("{0}"); at != std::string::npos; at = text.find("{0}", at + class_name.size())) { text.replace(at, 3, class_name); }
        throw runtime_error(text);
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
