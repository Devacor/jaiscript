#pragma once

#ifndef __JAISCRIPT_DETAIL_REDECLARATION_HPP__
#define __JAISCRIPT_DETAIL_REDECLARATION_HPP__

#include <jaiscript/detail/ast.hpp>
#include <jaiscript/core/value.hpp>

namespace jai::detail {

// The same unit running a program-scope declaration again (a module an import re-runs after a touch, the host's execute_file of
// the same file, a re-run string, a top-level include run again): a declaration written without an
// initializer keeps the value its variable holds (only a declaration that assigns resets it), unless its declared type changed,
// which resets it to the new type's default, as a class field whose declared type changes on hot reload does. Both backends ask
// here, before the declaration's default is evaluated.
inline bool redeclaration_keeps_value(const variable_decl& decl, const script_value& existing) {
	if (!decl.default_initialized) { return false; }
	if (!decl.type) { return true; }
	const script_value& held = existing.is_reference() ? existing.deref() : existing;
	const type_info* tag = held.get_type_info().get();
	return tag && tag->id == decl.type->id;
}

}

#endif
