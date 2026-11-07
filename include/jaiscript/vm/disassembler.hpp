#pragma once

#ifndef __JAISCRIPT_VM_DISASSEMBLER_HPP__
#define __JAISCRIPT_VM_DISASSEMBLER_HPP__

#include "chunk.hpp"
#include <jaiscript/detail/string_symbolizer.hpp>
#include <string>

namespace jai::vm {

    std::string disassemble(const chunk& ch, const string_symbolizer* symbolizer);

} // namespace jai::vm

#endif // __JAISCRIPT_VM_DISASSEMBLER_HPP__
