#pragma once

#include <jaiscript/core/engine.hpp>
#include <string>

namespace jai {
namespace stdlib {

    // THE object row, shared by type_of and reflect::type_name: the instance's own class first, then the value tag, then the
    // bare kind - a value tagged with a base class still answers the class it actually holds
    std::string object_type_name(const script_value& d);

    // print, input and formatting (source/implementation/stdlib/io.cpp)
    void register_io_functions(engine& eng_ref);

} // namespace stdlib
} // namespace jai
