#pragma once

#ifndef __JAISCRIPT_DETAIL_SLOT_SITE_HPP__
#define __JAISCRIPT_DETAIL_SLOT_SITE_HPP__

// The slot a typed-boundary rejection names. Sites hand one
// to detail::typed_slot_rejection (class_definition.hpp); the wrappers' callers describe the
// slot they bind and the text follows the kind. Dependency-free so both backends' headers
// can default it in their signatures.

#include <cstdint>
#include <string_view>

namespace jai::detail {

    enum class slot_kind : uint8_t { none, parameter, return_value, yield_value, assignment, declaration, field, static_field };

    struct slot_site {
        slot_kind kind = slot_kind::assignment;
        std::string_view name;    // parameter / variable / field; empty = unknown
        std::string_view owner;   // callee / class; empty = unknown
        bool by_reference = false;   // a T& slot: the description spells the declared type T&
    };

    // A name no source can spell (operator++(int)'s marker parameter, a compiler's own value): it starts with \x01, and a text or a
    // listing that names a slot treats it as no name
    inline bool unspellable_name(std::string_view name) { return !name.empty() && name.front() == '\x01'; }

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_SLOT_SITE_HPP__
