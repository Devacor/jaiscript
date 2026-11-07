#pragma once

#include <jaiscript/core/checked_result.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/core/runtime_errors.hpp>

#include <string_view>

namespace jai::detail {

// Range-for texts shared by both backends. A string
// iterates by value only: its chars are reads like s[i], never cells.
inline constexpr std::string_view string_range_for_by_ref_text =
    "Cannot iterate a string by reference: its chars are not lvalues (use for (auto c : s))";
inline constexpr std::string_view utf8_range_for_by_ref_text =
    "Cannot iterate a utf8_string by reference: its characters are not lvalues (use for (auto c : s))";

// {0} = the payload's type, spelled like family D's source (class names for objects and
// handles, the base names otherwise - every spelling is already interned, so the error
// path interns nothing new)
inline error_propagator not_iterable(const script_value& v, engine* eng) {
    return {make_error_code(runtime_error_code::type_mismatch),
            "Cannot iterate {0}: not an array, map, string or coroutine",
            engine_access::get_symbolizer(*eng)->intern(slot_source_type_name(v, eng)), 0};
}

} // namespace jai::detail
