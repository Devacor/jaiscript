// The one reading of a format text (print / format with a literal, vformat with a run-time text): the formatter writes
// through it and the static checker counts through it, so the load-time and run-time argument checks cannot disagree.
#ifndef __JAISCRIPT_FORMAT_TEXT_HPP__
#define __JAISCRIPT_FORMAT_TEXT_HPP__

#include <jaiscript/jaiscript_fwd.hpp>
#include <jaiscript/detail/format_spec.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace jai::detail {

	inline bool format_all_digits(std::string_view s) noexcept {
		for (char c : s) {
			if (c < '0' || c > '9') { return false; }
		}
		return !s.empty();
	}

	// A placeholder is {}, {n}, {:spec} or {n:spec} (the spec subset of format_spec.hpp); a {n:...} whose spec the subset
	// does not have is a placeholder too: its substitution raises, naming the spec
	inline bool has_format_placeholders(std::string_view str) noexcept {
		size_t pos = 0;
		while (pos < str.length()) {
			const size_t brace = str.find('{', pos);
			if (brace == std::string_view::npos) { break; }
			if (brace + 1 < str.length() && str[brace + 1] == '{') {
				pos = brace + 2;
				continue;
			}
			const size_t close = str.find('}', brace);
			if (close != std::string_view::npos) {
				const std::string_view content = str.substr(brace + 1, close - brace - 1);
				if (content.empty()) { return true; }
				const size_t colon = content.find(':');
				const std::string_view index_part = content.substr(0, colon == std::string_view::npos ? content.size() : colon);
				if ((index_part.empty() || format_all_digits(index_part)) && (colon != std::string_view::npos || !index_part.empty())) { return true; }
			}
			pos = brace + 1;
		}
		return false;
	}

	// {n}'s index: digits, or what std::stoull reads (leading space, a sign, a digit prefix); false leaves the braces as text
	bool format_positional_index(std::string_view spec, size_t& out);

	// What a format text asks of its values (the arguments after it, counted from 0)
	struct format_text_use {
		static constexpr size_t none = static_cast<size_t>(-1);
		size_t sequential = 0;          // {} and {:spec} placeholders
		size_t positional_past = none;  // the first {n} that names no value
		uint64_t named = 0;             // the values 0..63 a {n} names
		bool positional = false;

		// the first value below `count` no placeholder takes (values past 63 count as taken), or none
		size_t first_unused(size_t count) const noexcept {
			for (size_t i = sequential; i < count && i < 64; ++i) {
				if (!(named & (uint64_t{1} << i))) { return i; }
			}
			return none;
		}
		bool agrees(size_t count) const noexcept {
			if (sequential > count || positional_past != none) { return false; }
			if (sequential >= count) { return true; }
			const uint64_t all = count >= 64 ? ~uint64_t{0} : ((uint64_t{1} << count) - 1);
			const uint64_t taken = named | (sequential >= 64 ? ~uint64_t{0} : ((uint64_t{1} << sequential) - 1));
			return (taken & all) == all;
		}
	};

	// What follows "format text " in the mismatch's error: the placeholder and argument counts, the {n} past the arguments, or
	// the argument nothing names. Called only when !use.agrees(count).
	std::string format_text_mismatch(const format_text_use& use, size_t count);

	// Walks `text` over `count` values. The sink writes: text(view), value(i) for {} and {n}, value_spec(i, parsed, spec) for
	// {:spec} and {n:spec}, bad_spec(spec) for a spec outside the subset. A placeholder naming no value writes nothing.
	template <class Sink>
	format_text_use walk_format_text(std::string_view text, size_t count, Sink&& sink) {
		format_text_use use;
		size_t pos = 0;
		const auto name = [&](size_t index) {
			use.positional = true;
			if (index >= count) {
				if (use.positional_past == format_text_use::none) { use.positional_past = index; }
				return false;
			}
			if (index < 64) { use.named |= uint64_t{1} << index; }
			return true;
		};
		while (pos < text.length()) {
			const size_t next_open = text.find('{', pos);
			const size_t next_close = text.find('}', pos);
			// }} -> }, a lone } stays
			if (next_close != std::string_view::npos && (next_open == std::string_view::npos || next_close < next_open)) {
				sink.text(text.substr(pos, next_close - pos + 1));
				pos = next_close + 1 < text.length() && text[next_close + 1] == '}' ? next_close + 2 : next_close + 1;
				continue;
			}
			if (next_open == std::string_view::npos) {
				sink.text(text.substr(pos));
				break;
			}
			sink.text(text.substr(pos, next_open - pos));
			if (next_open + 1 < text.length() && text[next_open + 1] == '{') {
				sink.text(text.substr(next_open, 1));
				pos = next_open + 2;
				continue;
			}
			const size_t close_brace = text.find('}', next_open);
			if (close_brace == std::string_view::npos) {
				sink.text(text.substr(next_open));   // an unclosed brace is text
				break;
			}
			const std::string_view spec = text.substr(next_open + 1, close_brace - next_open - 1);
			const std::string_view whole = text.substr(next_open, close_brace - next_open + 1);
			if (spec.empty()) {
				if (use.sequential < count) { sink.value(use.sequential); }
				++use.sequential;
			} else if (const size_t colon = spec.find(':'); colon != std::string_view::npos) {
				const std::string_view index_part = spec.substr(0, colon);
				const std::string_view fmt_part = spec.substr(colon + 1);
				if (index_part.empty() || format_all_digits(index_part)) {
					format_spec parsed;
					if (!parse_format_spec(fmt_part, parsed)) { sink.bad_spec(fmt_part); }
					size_t index = use.sequential;
					bool has_value = true;
					if (index_part.empty()) {
						has_value = use.sequential < count;
						++use.sequential;
					} else if (!format_positional_index(index_part, index)) {
						has_value = name(count);
					} else {
						has_value = name(index);
					}
					if (has_value) { sink.value_spec(index, parsed, fmt_part); }
				} else {
					sink.text(whole);
				}
			} else {
				size_t index = 0;
				if (format_positional_index(spec, index)) {
					if (name(index)) { sink.value(index); }
				} else {
					sink.text(whole);
				}
			}
			pos = close_brace + 1;
		}
		return use;
	}

	struct format_text_counter {
		void text(std::string_view) noexcept {}
		void value(size_t) noexcept {}
		void value_spec(size_t, const format_spec&, std::string_view) noexcept {}
		void bad_spec(std::string_view) noexcept {}
	};

}

#endif
