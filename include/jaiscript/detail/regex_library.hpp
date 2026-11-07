#pragma once

#ifndef __JAISCRIPT_DETAIL_REGEX_LIBRARY_HPP__
#define __JAISCRIPT_DETAIL_REGEX_LIBRARY_HPP__

// The script surface of the regex engine (guide ch. 9 "Regular expressions"): the regex value, regex::match, and the
// pieces the checker and both backends share for a constant pattern, which compiles once at load (source/implementation/
// regex/regex_library.cpp). Engine-core: every engine registers it. Library-only: hosts never include it.

#include <jaiscript/jaiscript_fwd.hpp>
#include <jaiscript/detail/regex.hpp>
#include <string>

namespace jai {
	class call_expr;
	class expression;
	class script_value;
}

namespace jai::detail::regex_library {

	void register_library(engine& eng);

	// A string literal, or a + of them: its text
	bool constant_text(const expression* e, std::string& out);

	// The callee is the engine's regex constructor (a parallel worker's copy of it included)
	bool is_constructor(const script_value& callee);
	// While the callee is the engine's regex constructor: a new regex value over the compiled pattern (a constant's, at each
	// evaluation) in out, true; anything else is false
	bool make_constant(const script_value& callee, const regex::compiled_ptr& re, script_value& out);

} // namespace jai::detail::regex_library

#endif
