#pragma once

#ifndef __JAISCRIPT_DETAIL_MEMBER_DISPLAY_NAME_HPP__
#define __JAISCRIPT_DETAIL_MEMBER_DISPLAY_NAME_HPP__

#include <string_view>

namespace jai::detail {

	// A member as its class lists it and as a text about a call names it: a generic method's instance is declared under its
	// class's name (Pair<string, int>::conv<int>, so no derived class's instance overrides it) and shown bare with its type
	// arguments (conv<int>), as a plain method is; any other member's name as it is. It reads a member's name only: a
	// function's may be a namespaced class's (kit::Crate<int>, its constructor), which function_decl::shown_name tells apart
	inline std::string_view member_display_name(std::string_view name) {
		size_t cut = std::string_view::npos;
		int angles = 0;
		for (size_t i = 0; i + 1 < name.size(); ++i) {
			if (name[i] == '<') { ++angles; }
			else if (name[i] == '>') { --angles; }
			else if (angles == 0 && name[i] == ':' && name[i + 1] == ':') { cut = i + 2; }
		}
		return cut == std::string_view::npos || name.find('<', cut) == std::string_view::npos ? name : name.substr(cut);
	}

}

#endif
