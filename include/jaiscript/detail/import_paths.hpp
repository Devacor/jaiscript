#pragma once

#ifndef __JAISCRIPT_DETAIL_IMPORT_PATHS_HPP__
#define __JAISCRIPT_DETAIL_IMPORT_PATHS_HPP__

#include <jaiscript/core/value.hpp>
#include <string>
#include <vector>

namespace jai::detail {

	// import(...)'s computed arguments as paths, shared VERBATIM by both backends: a string or utf8_string is one path, an
	// array holds them; anything else raises
	inline checked_result<void> append_import_paths(const script_value& arg, std::vector<std::string>& out, engine* eng) {
		const script_value& v = arg.deref();
		if (const std::string* text = v.text_bytes()) {
			out.push_back(*text);
			return {};
		}
		if (v.is_array()) {
			bool ok = true;
			v.unchecked_array_node()->for_each(eng, [&](const script_value& element) {
				if (const std::string* text = element.deref().text_bytes()) { out.push_back(*text); } else { ok = false; }
			});
			if (ok) { return {}; }
		}
		return checked_result<void>(make_error_code(runtime_error_code::type_mismatch), "import(...) takes paths: strings or arrays of strings");
	}

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_IMPORT_PATHS_HPP__
