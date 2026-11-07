#pragma once

#ifndef __JAISCRIPT_DETAIL_FIRST_TYPE_HPP__
#define __JAISCRIPT_DETAIL_FIRST_TYPE_HPP__

namespace jai::detail {

    // first_type_t<U, T...> is U named through the pack: a declaration spelled with it depends on T, so
    // a member access through it is checked at instantiation, where a class that a header cycle leaves
    // incomplete at the template's definition is complete (Clang checks non-dependent accesses at the
    // definition; GCC and MSVC wait)
    template<typename First, typename...>
    struct first_type { using type = First; };

    template<typename... Ts>
    using first_type_t = typename first_type<Ts...>::type;

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_FIRST_TYPE_HPP__
