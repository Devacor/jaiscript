#pragma once

#ifndef __JAISCRIPT_VM_CHUNK_MANIFEST_HPP__
#define __JAISCRIPT_VM_CHUNK_MANIFEST_HPP__

// A deterministic manifest of a compiled chunk's every semantic field: the
// instructions with their operand fields, the constants' exact bits, the symbols by spelling, every proto table,
// the typed sites, the cell loops with their cells, origins and live masks, the refusals and every nested chunk.
// Runtime caches (env lookup entries, member ICs, call-site ICs, profile counters) are excluded. Two chunks with
// the same manifest compile to the same thing; the listing (disassembler.hpp) is for reading, this is for the
// identity gate of a compiler change.

#include "chunk.hpp"
#include <jaiscript/detail/string_symbolizer.hpp>
#include <string>

namespace jai::vm {

	std::string chunk_manifest(const chunk& ch, const string_symbolizer* symbolizer);

} // namespace jai::vm

#endif // __JAISCRIPT_VM_CHUNK_MANIFEST_HPP__
