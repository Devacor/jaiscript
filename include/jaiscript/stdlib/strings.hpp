#pragma once

#include <jaiscript/core/engine.hpp>

namespace jai {
namespace stdlib {

    // string:: and the utf8_* functions (source/implementation/stdlib/strings.cpp)
    void register_string_functions(engine& eng_ref);

} // namespace stdlib
} // namespace jai
