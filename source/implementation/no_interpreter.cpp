// The interpreter's seam in the library hosts link: this build has none. A build that adds source/interpreter/ defines
// JAISCRIPT_INTERPRETER, which empties this file.
#ifndef JAISCRIPT_INTERPRETER
#include <jaiscript/core/class_definition.hpp>
#include <jaiscript/core/execution_backend.hpp>

namespace jai::detail {

bool interpreter_compiled() noexcept { return false; }

std::unique_ptr<execution_backend> make_interpreter_backend(string_symbolizer*, std::shared_ptr<environment>) { return nullptr; }

}
#endif
