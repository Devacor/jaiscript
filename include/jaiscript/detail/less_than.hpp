#pragma once

#ifndef __JAISCRIPT_DETAIL_LESS_THAN_HPP__
#define __JAISCRIPT_DETAIL_LESS_THAN_HPP__

// '<' after a name: a site is read by what its head names where it stands. The parser asks
// with a query per site, the checker's own lookup answers, and a parse stands wherever each answer gives the reading it took.

#include <cstdint>
#include <string>
#include <string_view>

namespace jai::detail {

	enum class less_head : uint8_t { bare, global, qualified, member };

	// What a site asks: its head and where it stands
	struct less_query {
		less_head form = less_head::bare;   // global: ::x, and a name a template body's barrier hides its scopes from
		std::string head;       // x, A::B::x, a member's name
		std::string cls;        // the enclosing class, by its qualified name
		std::string space;      // the enclosing namespace's path

		// form, class, namespace and head in one text (no name holds '|')
		std::string text() const {
			std::string result;
			result.push_back(static_cast<char>('0' + static_cast<int>(form)));
			result += '|';
			result += cls;
			result += '|';
			result += space;
			result += '|';
			result += head;
			return result;
		}
		static less_query parse(std::string_view text) {
			less_query result;
			if (text.size() < 2) { return result; }
			result.form = static_cast<less_head>(text[0] - '0');
			const size_t first = 1;
			const size_t second = text.find('|', first + 1);
			const size_t third = second == std::string_view::npos ? second : text.find('|', second + 1);
			if (third == std::string_view::npos) { return result; }
			result.cls = std::string(text.substr(first + 1, second - first - 1));
			result.space = std::string(text.substr(second + 1, third - second - 1));
			result.head = std::string(text.substr(third + 1));
			return result;
		}
	};

	inline constexpr char k_less_variable = 'v';   // the head names a variable
	inline constexpr char k_less_declared = 'd';   // something declared that is no variable: a generic, a function, a class, a host name
	inline constexpr char k_less_nothing = 'n';    // nothing the load knows declares it

	// The reading an answer gives: type arguments need something declared that is no variable. A unit parsed before the files it
	// names are pooled (not settled) reads a head nothing declares yet as type arguments, the reading every generic use parses under.
	inline bool less_opens(char answer, bool settled) { return settled ? answer == k_less_declared : answer != k_less_variable; }

	// Whether two answer lists give every site the reading it had
	inline bool less_readings_equal(std::string_view was, bool was_settled, std::string_view now, bool now_settled) {
		if (was.size() != now.size()) { return false; }
		for (size_t i = 0; i < was.size(); ++i) {
			if (less_opens(was[i], was_settled) != less_opens(now[i], now_settled)) { return false; }
		}
		return true;
	}

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_LESS_THAN_HPP__
