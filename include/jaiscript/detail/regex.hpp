#pragma once

#ifndef __JAISCRIPT_DETAIL_REGEX_HPP__
#define __JAISCRIPT_DETAIL_REGEX_HPP__

// The regex engine (source/implementation/regex/regex_engine.cpp): a pattern parses to a tree, compiles to byte programs
// and runs on a lazily built DFA (match bounds) and a Pike VM (capture groups). Every walk is iterative and every search
// is linear in the text: no backtracking, so no pattern is catastrophic. A string is matched by bytes, a utf8_string by
// UTF-8 characters; one compiled regex holds a program for each. Library-only: hosts never include it.

#include <jaiscript/jaiscript_fwd.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jai::detail::regex {

	class compiled;
	using compiled_ptr = std::shared_ptr<const compiled>;
	inline constexpr size_t npos = SIZE_MAX;

	enum class subject : uint8_t { bytes = 0, utf8 = 1 };

	// The pattern and its flags string (i, m, s, x), compiled for both subjects; null with the error text on failure
	compiled_ptr compile(std::string_view pattern, std::string_view flags, std::string& error);

	const std::string& pattern_of(const compiled& re);
	const std::string& flags_of(const compiled& re);
	size_t group_count(const compiled& re);   // capture groups, the whole match not counted
	size_t group_index(const compiled& re, std::string_view name);   // npos for no such name
	const std::string& group_name(const compiled& re, size_t index);   // empty for an unnamed group
	// Why the pattern cannot match that subject (a string's class holds bytes only), empty when it can
	std::string subject_refusal(const compiled& re, subject s);
	// The bytes its programs take (the DFA caches come and go)
	size_t footprint(const compiled& re);

	// A pattern's text as a pattern that matches it literally
	std::string escape(std::string_view text);

	// One subject's searches over a compiled regex; holds the regex's caches (or private ones while another search holds
	// them) until it is destroyed. Slots are byte offsets, two per group (group 0 is the whole match), npos when unmatched.
	class searcher {
	public:
		searcher(const compiled& re, subject s);
		~searcher();
		searcher(const searcher&) = delete;
		searcher& operator=(const searcher&) = delete;

		// The leftmost match starting at or after `from` (assertions see the text before it)
		bool find(std::string_view text, size_t from, bool groups, std::vector<size_t>& slots);
		// The whole text matches
		bool full(std::string_view text);

		struct state;
		static constexpr size_t k_state_size = 160;
	private:
		alignas(std::max_align_t) unsigned char storage_[k_state_size];
		state& st();
	};

	// The byte offset one character past `at` (a byte that starts no valid sequence is a character of its own)
	size_t next_character(std::string_view text, size_t at, subject s);

} // namespace jai::detail::regex

#endif
