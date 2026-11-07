#include <jaiscript/detail/regex.hpp>
#include <jaiscript/detail/utf8.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <atomic>
#include <cstddef>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The pattern parses to a tree that holds every construct of the syntax, the ones a linear-time matcher cannot run included
// (so a bounded backtracking matcher could take those later without a new parser); the tree compiles to a byte program per
// subject and direction. A search runs the forward program on a lazily built DFA for the match's end, the reverse program for
// its start, and the Pike VM over the match alone when capture groups are wanted. Every walk keeps its own stack.

namespace jai::detail::regex_engine {

	using regex::npos;
	using regex::subject;

	inline constexpr uint32_t k_none = UINT32_MAX;
	inline constexpr uint32_t k_unbounded = UINT32_MAX;
	inline constexpr uint32_t k_repeat_limit = 1000;
	inline constexpr size_t k_program_limit = 250000;
	inline constexpr size_t k_capture_table_limit = size_t(1) << 20;   // Pike VM capture cells: rows times slots
	inline constexpr size_t k_dfa_budget = size_t(8) << 20;   // the states one search may build
	inline constexpr size_t k_dfa_kept = size_t(1) << 20;     // the states a regex keeps between searches
	inline constexpr uint32_t k_max_codepoint = 0x10FFFF;

	// not_word_boundary_char is \B on a utf8_string, which never holds inside a character (between a byte of 0x80 or above and a
	// continuation byte after it); its _reverse form reads the same position with the bytes' roles swapped, for the reverse program
	enum class assertion : uint8_t { begin_text, end_text, begin_line, end_line, word_boundary, not_word_boundary, not_word_boundary_char,
	                                 not_word_boundary_char_reverse };

	inline assertion mirrored(assertion a) {
		switch (a) {
			case assertion::begin_text: return assertion::end_text;
			case assertion::end_text: return assertion::begin_text;
			case assertion::begin_line: return assertion::end_line;
			case assertion::end_line: return assertion::begin_line;
			case assertion::not_word_boundary_char: return assertion::not_word_boundary_char_reverse;
			case assertion::not_word_boundary_char_reverse: return assertion::not_word_boundary_char;
			default: return a;
		}
	}

	inline bool word_byte(int c) {
		return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
	}

	// prev and next are bytes, or -1 at an edge of the text
	inline bool assertion_holds(assertion a, int prev, int next) {
		switch (a) {
			case assertion::begin_text: return prev < 0;
			case assertion::end_text: return next < 0;
			case assertion::begin_line: return prev < 0 || prev == '\n';
			case assertion::end_line: return next < 0 || next == '\n';
			case assertion::word_boundary: return word_byte(prev) != word_byte(next);
			case assertion::not_word_boundary: return word_byte(prev) == word_byte(next);
			case assertion::not_word_boundary_char:
				return word_byte(prev) == word_byte(next) && !(prev >= 0x80 && next >= 0x80 && next <= 0xBF);
			case assertion::not_word_boundary_char_reverse:
				return word_byte(prev) == word_byte(next) && !(next >= 0x80 && prev >= 0x80 && prev <= 0xBF);
		}
		return false;
	}

	// ===== the tree =====

	enum class node_kind : uint8_t {
		empty, literal, set, any, test, group, concat, alternate, repeat,
		backreference, lookaround, atomic, recursion   // parsed, refused by the linear compiler
	};

	struct node {
		node_kind kind = node_kind::empty;
		bool fold = false;          // literal, set: ASCII letters match either case
		bool dotall = false;        // any: matches a newline too
		bool raw = false;           // literal: a \xNN escape (a byte on a string, U+00NN on a utf8_string)
		bool greedy = true;
		bool possessive = false;    // repeat: refused
		assertion test = assertion::begin_text;
		uint32_t value = 0;         // literal: code point or byte; set: index; group: capture index or k_none; lookaround: 0 ahead, 1 negative ahead, 2 behind, 3 negative behind; backreference: group number (k_none by name)
		uint32_t min = 0, max = 0;  // repeat bounds
		uint32_t pos = 0, end = 0;  // the construct's span in the pattern, in characters
		std::vector<uint32_t> kids;
	};

	// kind 0: characters lo..hi; 1: a class escape, lo = 'd', 'w' or 's' and hi = 1 when negated. A \xNN end is raw.
	struct set_item { uint32_t lo = 0, hi = 0; uint8_t kind = 0; bool lo_raw = false, hi_raw = false; uint32_t pos = 0; };
	struct char_set { bool negated = false; bool fold = false; uint32_t pos = 0; std::vector<set_item> items; };

	struct tree {
		std::vector<node> nodes;
		std::vector<char_set> sets;
		uint32_t root = 0;
		uint32_t groups = 0;
		std::vector<std::string> names{std::string()};   // by capture index; 0 is the whole match
		uint32_t unsupported = k_none;
	};

	inline std::string encode_text(const std::vector<uint32_t>& cps, size_t from, size_t to) {
		std::string out;
		for (size_t i = from; i < to && i < cps.size(); ++i) {
			if (!utf8::encode(cps[i], out)) { out.push_back('?'); }
		}
		return out;
	}

	struct parse_flags { bool fold = false, multiline = false, dotall = false, extended = false; };

	class pattern_parser {
	public:
		pattern_parser(const std::vector<uint32_t>& cps, parse_flags flags, tree& out, std::string& error)
			: cps_(cps), flags_(flags), t_(out), error_(error) {}

		bool run() {
			stack_.push_back(open_group{open_kind::top, k_none, 0, flags_, {}});
			stack_.back().alts.emplace_back();
			size_t i = 0;
			const size_t n = cps_.size();
			while (true) {
				if (flags_.extended) { skip_extended(i); }
				if (i >= n) { break; }
				const uint32_t c = cps_[i];
				const uint32_t at = static_cast<uint32_t>(i);
				switch (c) {
					case '|':
						stack_.back().alts.emplace_back();
						++i;
						break;
					case '(':
						if (!open(i)) { return false; }
						break;
					case ')':
						if (stack_.size() == 1) { return fail("unmatched ')'", at); }
						close(at);
						++i;
						break;
					case '*': case '+': case '?':
						++i;
						if (!quantify(c == '+' ? 1 : 0, c == '?' ? 1 : k_unbounded, at, i)) { return false; }
						break;
					case '{': {
						uint32_t lo = 0, hi = 0;
						size_t after = i;
						bool too_big = false;
						if (!braces(i, lo, hi, after, too_big)) {
							add_literal(c, false, at, at + 1);
							++i;
							break;
						}
						if (too_big) {
							return fail("repetition '" + encode_text(cps_, at, after) + "' counts past " + std::to_string(k_repeat_limit), at);
						}
						if (lo > hi) { return fail("repetition '" + encode_text(cps_, at, after) + "' has its minimum above its maximum", at); }
						i = after;
						if (!quantify(lo, hi, at, i)) { return false; }
						break;
					}
					case '[':
						if (!parse_set(i)) { return false; }
						break;
					case '.': {
						node nd;
						nd.kind = node_kind::any;
						nd.dotall = flags_.dotall;
						add(std::move(nd), at, at + 1);
						++i;
						break;
					}
					case '^': add_test(flags_.multiline ? assertion::begin_line : assertion::begin_text, at, at + 1); ++i; break;
					case '$': add_test(flags_.multiline ? assertion::end_line : assertion::end_text, at, at + 1); ++i; break;
					case '\\':
						if (!escape(i)) { return false; }
						break;
					default:
						add_literal(c, false, at, at + 1);
						++i;
						break;
				}
			}
			if (stack_.size() > 1) { return fail("missing ')' to close the group", stack_.back().pos); }
			t_.root = build(stack_.back().alts, 0, static_cast<uint32_t>(n));
			return true;
		}

	private:
		enum class open_kind : uint8_t { top, capture, plain, lookaround, atomic };
		struct open_group {
			open_kind kind;
			uint32_t value;
			uint32_t pos;
			parse_flags saved;
			std::vector<std::vector<uint32_t>> alts;
		};

		const std::vector<uint32_t>& cps_;
		parse_flags flags_;
		tree& t_;
		std::string& error_;
		std::vector<open_group> stack_;

		bool fail(const std::string& what, uint32_t pos) {
			error_ = "regex: " + what + " at position " + std::to_string(pos);
			return false;
		}

		uint32_t peek(size_t i) const { return i < cps_.size() ? cps_[i] : k_none; }
		std::string spelled(size_t from, size_t to) const { return encode_text(cps_, from, to); }

		void skip_extended(size_t& i) const {
			while (i < cps_.size()) {
				const uint32_t c = cps_[i];
				if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') { ++i; continue; }
				if (c != '#') { break; }
				while (i < cps_.size() && cps_[i] != '\n') { ++i; }
			}
		}

		uint32_t make(node nd) {
			t_.nodes.push_back(std::move(nd));
			return static_cast<uint32_t>(t_.nodes.size() - 1);
		}
		uint32_t add(node nd, uint32_t pos, uint32_t end) {
			nd.pos = pos;
			nd.end = end;
			const uint32_t id = make(std::move(nd));
			stack_.back().alts.back().push_back(id);
			return id;
		}
		void note_unsupported(uint32_t id) {
			if (t_.unsupported == k_none) { t_.unsupported = id; }
		}
		void add_literal(uint32_t cp, bool raw, uint32_t pos, uint32_t end) {
			node nd;
			nd.kind = node_kind::literal;
			nd.value = cp;
			nd.raw = raw;
			nd.fold = flags_.fold;
			add(std::move(nd), pos, end);
		}
		void add_test(assertion a, uint32_t pos, uint32_t end) {
			node nd;
			nd.kind = node_kind::test;
			nd.test = a;
			add(std::move(nd), pos, end);
		}
		void add_class_escape(uint32_t letter, bool negated, uint32_t pos) {
			char_set s;
			s.pos = pos;
			s.items.push_back(set_item{letter, negated ? 1u : 0u, 1, false, false, pos});
			t_.sets.push_back(std::move(s));
			node nd;
			nd.kind = node_kind::set;
			nd.value = static_cast<uint32_t>(t_.sets.size() - 1);
			add(std::move(nd), pos, pos + 2);
		}

		uint32_t build(const std::vector<std::vector<uint32_t>>& alts, uint32_t pos, uint32_t end) {
			std::vector<uint32_t> branches;
			branches.reserve(alts.size());
			for (const auto& items : alts) {
				if (items.size() == 1) {
					branches.push_back(items[0]);
					continue;
				}
				node nd;
				nd.kind = items.empty() ? node_kind::empty : node_kind::concat;
				nd.kids = items;
				nd.pos = pos;
				nd.end = end;
				branches.push_back(make(std::move(nd)));
			}
			if (branches.size() == 1) { return branches[0]; }
			node nd;
			nd.kind = node_kind::alternate;
			nd.kids = std::move(branches);
			nd.pos = pos;
			nd.end = end;
			return make(std::move(nd));
		}

		static bool name_start(uint32_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
		static bool name_char(uint32_t c) { return name_start(c) || (c >= '0' && c <= '9'); }
		static bool flag_char(uint32_t c) { return c == 'i' || c == 'm' || c == 's' || c == 'x'; }
		static bool digit(uint32_t c) { return c >= '0' && c <= '9'; }

		bool open(size_t& i) {
			const uint32_t at = static_cast<uint32_t>(i);
			open_group g{open_kind::capture, 0, at, flags_, {}};
			g.alts.emplace_back();
			if (peek(i + 1) != '?') {
				g.value = ++t_.groups;
				t_.names.emplace_back();
				i += 1;
				stack_.push_back(std::move(g));
				return true;
			}
			const uint32_t c = peek(i + 2);
			if (c == ':') {
				g.kind = open_kind::plain;
				i += 3;
			} else if (c == '=' || c == '!') {
				g.kind = open_kind::lookaround;
				g.value = c == '=' ? 0 : 1;
				i += 3;
			} else if (c == '<' && (peek(i + 3) == '=' || peek(i + 3) == '!')) {
				g.kind = open_kind::lookaround;
				g.value = peek(i + 3) == '=' ? 2 : 3;
				i += 4;
			} else if (c == '<') {
				size_t j = i + 3;
				if (!name_start(peek(j))) { return fail("invalid group name", static_cast<uint32_t>(j)); }
				while (name_char(peek(j))) { ++j; }
				if (peek(j) != '>') { return fail("invalid group name", static_cast<uint32_t>(j)); }
				std::string name = spelled(i + 3, j);
				for (const auto& existing : t_.names) {
					if (existing == name) { return fail("duplicate group name '" + name + "'", static_cast<uint32_t>(i + 3)); }
				}
				g.value = ++t_.groups;
				t_.names.push_back(std::move(name));
				i = j + 1;
			} else if (c == '>') {
				g.kind = open_kind::atomic;
				i += 3;
			} else if (c == 'R' || c == '&' || digit(c) || ((c == '+' || c == '-') && digit(peek(i + 3))) || (c == 'P' && peek(i + 3) == '>')) {
				size_t j = i + 2;
				while (j < cps_.size() && cps_[j] != ')') { ++j; }
				if (j >= cps_.size()) { return fail("missing ')' to close the group", at); }
				node nd;
				nd.kind = node_kind::recursion;
				note_unsupported(add(std::move(nd), at, static_cast<uint32_t>(j + 1)));
				i = j + 1;
				return true;
			} else if (flag_char(c) || c == '-') {
				parse_flags next = flags_;
				size_t j = i + 2;
				bool on = true, any = false;
				while (j < cps_.size() && (flag_char(cps_[j]) || cps_[j] == '-')) {
					const uint32_t f = cps_[j];
					if (f == '-') {
						if (!on) { return fail("unknown group syntax '" + spelled(i, j + 1) + "'", at); }
						on = false;
					} else {
						any = true;
						(f == 'i' ? next.fold : f == 'm' ? next.multiline : f == 's' ? next.dotall : next.extended) = on;
					}
					++j;
				}
				if (!any) { return fail("unknown group syntax '" + spelled(i, std::min(j + 1, cps_.size())) + "'", at); }
				if (peek(j) == ')') {
					flags_ = next;
					i = j + 1;
					return true;
				}
				if (peek(j) != ':') {
					if (j >= cps_.size()) { return fail("missing ')' to close the group", at); }
					return fail("unknown flag '" + spelled(j, j + 1) + "' in a group (the flags are i, m, s and x)", static_cast<uint32_t>(j));
				}
				g.kind = open_kind::plain;
				flags_ = next;
				i = j + 1;
			} else {
				return fail("unknown group syntax '" + spelled(i, std::min(i + 3, cps_.size())) + "'", at);
			}
			stack_.push_back(std::move(g));
			return true;
		}

		void close(uint32_t at) {
			open_group g = std::move(stack_.back());
			stack_.pop_back();
			flags_ = g.saved;
			node nd;
			nd.kids.push_back(build(g.alts, g.pos, at + 1));
			switch (g.kind) {
				case open_kind::capture: nd.kind = node_kind::group; nd.value = g.value; break;
				case open_kind::lookaround: nd.kind = node_kind::lookaround; nd.value = g.value; break;
				case open_kind::atomic: nd.kind = node_kind::atomic; break;
				default: nd.kind = node_kind::group; nd.value = k_none; break;
			}
			const uint32_t id = add(std::move(nd), g.pos, at + 1);
			if (g.kind == open_kind::lookaround || g.kind == open_kind::atomic) { note_unsupported(id); }
		}

		// {n}, {n,} or {n,m}; anything else is a literal brace
		bool braces(size_t i, uint32_t& lo, uint32_t& hi, size_t& after, bool& too_big) const {
			size_t j = i + 1;
			auto number = [&](uint32_t& out) {
				const size_t start = j;
				uint32_t v = 0;
				while (j < cps_.size() && digit(cps_[j])) {
					v = v * 10 + (cps_[j] - '0');
					if (v > k_repeat_limit) { too_big = true; v = k_repeat_limit + 1; }
					++j;
				}
				out = v;
				return j > start;
			};
			if (!number(lo)) { return false; }
			if (peek(j) == '}') { hi = lo; after = j + 1; return true; }
			if (peek(j) != ',') { return false; }
			++j;
			if (peek(j) == '}') { hi = k_unbounded; after = j + 1; return true; }
			if (!number(hi) || peek(j) != '}') { return false; }
			after = j + 1;
			return true;
		}

		bool quantify(uint32_t lo, uint32_t hi, uint32_t at, size_t& i) {
			auto& items = stack_.back().alts.back();
			if (items.empty()) { return fail("nothing to repeat before '" + spelled(at, i) + "'", at); }
			const node& target = t_.nodes[items.back()];
			if (target.kind == node_kind::repeat) { return fail("'" + spelled(at, i) + "' follows another quantifier", at); }
			if (target.kind == node_kind::test) { return fail("nothing to repeat before '" + spelled(at, i) + "' (an anchor matches no characters)", at); }
			node nd;
			nd.kind = node_kind::repeat;
			nd.min = lo;
			nd.max = hi;
			nd.kids.push_back(items.back());
			nd.pos = target.pos;
			if (peek(i) == '?') { nd.greedy = false; ++i; }
			else if (peek(i) == '+') { nd.possessive = true; nd.pos = at; ++i; }
			nd.end = static_cast<uint32_t>(i);
			const bool possessive = nd.possessive;
			const uint32_t id = make(std::move(nd));
			if (possessive) { note_unsupported(id); }
			items.back() = id;
			return true;
		}

		static int hex(uint32_t c) {
			if (c >= '0' && c <= '9') { return static_cast<int>(c - '0'); }
			if (c >= 'a' && c <= 'f') { return static_cast<int>(c - 'a' + 10); }
			if (c >= 'A' && c <= 'F') { return static_cast<int>(c - 'A' + 10); }
			return -1;
		}

		enum class escaped : uint8_t { none, character, failed };

		// The character escapes a class and the pattern share: \n \r \t \f \v \0, \xNN, \u{...} / \uXXXX and escaped punctuation
		escaped char_escape(size_t& i, uint32_t& cp, bool& raw) {
			const uint32_t at = static_cast<uint32_t>(i);
			const uint32_t c = peek(i + 1);
			raw = false;
			switch (c) {
				case 'n': cp = '\n'; i += 2; return escaped::character;
				case 'r': cp = '\r'; i += 2; return escaped::character;
				case 't': cp = '\t'; i += 2; return escaped::character;
				case 'f': cp = '\f'; i += 2; return escaped::character;
				case 'v': cp = '\v'; i += 2; return escaped::character;
				case '0':
					if (digit(peek(i + 2))) {
						fail("octal escape '" + spelled(i, i + 3) + "' is not supported (write \\xNN)", at);
						return escaped::failed;
					}
					cp = 0;
					i += 2;
					return escaped::character;
				case 'x': {
					const int h = hex(peek(i + 2)), l = hex(peek(i + 3));
					if (h < 0 || l < 0) {
						fail("invalid \\x escape: expected two hex digits", at);
						return escaped::failed;
					}
					cp = static_cast<uint32_t>(h * 16 + l);
					raw = true;
					i += 4;
					return escaped::character;
				}
				case 'u': {
					uint32_t v = 0;
					size_t j = i + 2;
					bool ok = true;
					if (peek(j) == '{') {
						++j;
						size_t digits = 0;
						while (hex(peek(j)) >= 0 && digits < 7) { v = v * 16 + static_cast<uint32_t>(hex(peek(j))); ++j; ++digits; }
						ok = digits > 0 && digits <= 6 && peek(j) == '}';
						++j;
					} else {
						for (int k = 0; k < 4 && ok; ++k, ++j) {
							const int h = hex(peek(j));
							ok = h >= 0;
							v = v * 16 + static_cast<uint32_t>(ok ? h : 0);
						}
					}
					if (!ok) {
						fail("invalid \\u escape: expected \\u{hex} or four hex digits", at);
						return escaped::failed;
					}
					if (v > k_max_codepoint || (v >= 0xD800 && v <= 0xDFFF)) {
						fail("\\u escape '" + spelled(i, j) + "' names no character (a surrogate, or past U+10FFFF)", at);
						return escaped::failed;
					}
					cp = v;
					i = j;
					return escaped::character;
				}
				default:
					break;
			}
			if (c != k_none && !digit(c) && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
				cp = c;
				i += 2;
				return escaped::character;
			}
			return escaped::none;
		}

		bool escape(size_t& i) {
			const uint32_t at = static_cast<uint32_t>(i);
			const uint32_t c = peek(i + 1);
			if (c == k_none) { return fail("trailing '\\'", at); }
			switch (c) {
				case 'd': case 'w': case 's': add_class_escape(c, false, at); i += 2; return true;
				case 'D': case 'W': case 'S': add_class_escape(c + ('a' - 'A'), true, at); i += 2; return true;
				case 'b': add_test(assertion::word_boundary, at, at + 2); i += 2; return true;
				case 'B': add_test(assertion::not_word_boundary, at, at + 2); i += 2; return true;
				case 'A': add_test(assertion::begin_text, at, at + 2); i += 2; return true;
				case 'z': add_test(assertion::end_text, at, at + 2); i += 2; return true;
				case 'p': case 'P': return fail("Unicode property class '\\" + spelled(i + 1, i + 2) + "' is not supported", at);
				case 'k': {
					if (peek(i + 2) != '<') { return fail("unknown escape '\\k'", at); }
					size_t j = i + 3;
					while (j < cps_.size() && cps_[j] != '>') { ++j; }
					if (j >= cps_.size()) { return fail("invalid group name", static_cast<uint32_t>(i + 3)); }
					node nd;
					nd.kind = node_kind::backreference;
					nd.value = k_none;
					note_unsupported(add(std::move(nd), at, static_cast<uint32_t>(j + 1)));
					i = j + 1;
					return true;
				}
				default:
					break;
			}
			if (c >= '1' && c <= '9') {
				size_t j = i + 1;
				uint32_t v = 0;
				while (digit(peek(j)) && v < 100000) { v = v * 10 + (cps_[j] - '0'); ++j; }
				node nd;
				nd.kind = node_kind::backreference;
				nd.value = v;
				note_unsupported(add(std::move(nd), at, static_cast<uint32_t>(j)));
				i = j;
				return true;
			}
			uint32_t cp = 0;
			bool raw = false;
			const escaped r = char_escape(i, cp, raw);
			if (r == escaped::failed) { return false; }
			if (r == escaped::none) { return fail("unknown escape '\\" + spelled(at + 1, at + 2) + "'", at); }
			add_literal(cp, raw, at, static_cast<uint32_t>(i));
			return true;
		}

		// One member of a class: a character, or a class escape (kind 1)
		bool set_atom(size_t& i, set_item& out) {
			const uint32_t at = static_cast<uint32_t>(i);
			out = set_item{};
			out.pos = at;
			const uint32_t c = peek(i);
			if (c != '\\') {
				out.lo = out.hi = c;
				++i;
				return true;
			}
			const uint32_t e = peek(i + 1);
			if (e == k_none) { return fail("trailing '\\'", at); }
			switch (e) {
				case 'd': case 'w': case 's': out.lo = e; out.kind = 1; i += 2; return true;
				case 'D': case 'W': case 'S': out.lo = e + ('a' - 'A'); out.hi = 1; out.kind = 1; i += 2; return true;
				case 'b': out.lo = out.hi = 0x08; i += 2; return true;
				case 'p': case 'P': return fail("Unicode property class '\\" + spelled(i + 1, i + 2) + "' is not supported", at);
				default: break;
			}
			uint32_t cp = 0;
			bool raw = false;
			const escaped r = char_escape(i, cp, raw);
			if (r == escaped::failed) { return false; }
			if (r == escaped::none) { return fail("unknown escape '\\" + spelled(at + 1, at + 2) + "' in a class", at); }
			out.lo = out.hi = cp;
			out.lo_raw = out.hi_raw = raw;
			return true;
		}

		bool parse_set(size_t& i) {
			const uint32_t at = static_cast<uint32_t>(i);
			char_set s;
			s.pos = at;
			s.fold = flags_.fold;
			++i;
			if (peek(i) == '^') { s.negated = true; ++i; }
			while (true) {
				if (i >= cps_.size()) { return fail("missing ']' to close the class", at); }
				if (cps_[i] == ']') { ++i; break; }
				set_item lo;
				if (!set_atom(i, lo)) { return false; }
				if (lo.kind == 0 && peek(i) == '-' && peek(i + 1) != ']' && peek(i + 1) != k_none) {
					size_t j = i + 1;
					set_item hi;
					if (!set_atom(j, hi)) { return false; }
					if (hi.kind == 1) {   // [a-\d] holds a, '-' and the digits
						s.items.push_back(lo);
						s.items.push_back(set_item{'-', '-', 0, false, false, static_cast<uint32_t>(i)});
						s.items.push_back(hi);
						i = j;
						continue;
					}
					if (hi.lo < lo.lo) { return fail("range '" + spelled(lo.pos, j) + "' is out of order", lo.pos); }
					s.items.push_back(set_item{lo.lo, hi.lo, 0, lo.lo_raw, hi.lo_raw, lo.pos});
					i = j;
					continue;
				}
				s.items.push_back(lo);
			}
			t_.sets.push_back(std::move(s));
			node nd;
			nd.kind = node_kind::set;
			nd.value = static_cast<uint32_t>(t_.sets.size() - 1);
			add(std::move(nd), at, static_cast<uint32_t>(i));
			return true;
		}
	};

	// ===== programs =====

	enum class op : uint8_t { range, set, split, jump, save, test, match, fail };

	// range: bytes lo..hi then x; set: sets[y] then x; split: x first, then y; jump: x; save: slot y then x; test: assertion then x
	struct inst {
		op code = op::fail;
		uint8_t lo = 0, hi = 0;
		assertion test = assertion::begin_text;
		uint32_t x = 0, y = 0;
	};

	using byte_bits = std::array<uint64_t, 4>;
	inline bool bit(const byte_bits& b, unsigned c) { return (b[c >> 6] >> (c & 63)) & 1; }
	inline void set_bit(byte_bits& b, unsigned c) { b[c >> 6] |= uint64_t(1) << (c & 63); }

	struct program {
		std::vector<inst> insts;
		std::vector<byte_bits> sets;
		std::vector<uint32_t> row_of;   // a consuming or match instruction's row in the Pike VM's capture table
		uint32_t rows = 0;
		uint32_t start_unanchored = 0, start_anchored = 0;
		uint32_t slots = 2;
		bool needs_line = false, needs_word = false, needs_edge = false, needs_high = false;
		std::array<uint8_t, 256> class_of{};
		std::array<uint8_t, 257> class_byte{};
		uint32_t classes = 1;

		bool accepts(const inst& in, unsigned c) const {
			return in.code == op::range ? (c >= in.lo && c <= in.hi) : bit(sets[in.y], c);
		}

		void finish() {
			std::array<bool, 257> cut{};
			row_of.assign(insts.size(), k_none);
			rows = 0;
			for (size_t pc = 0; pc < insts.size(); ++pc) {
				const inst& in = insts[pc];
				if (in.code == op::range || in.code == op::set || in.code == op::match) { row_of[pc] = rows++; }
				if (in.code == op::range) { cut[in.lo] = true; cut[in.hi + 1u] = true; }
				if (in.code == op::test) {
					switch (in.test) {
						case assertion::begin_line: case assertion::end_line: needs_line = true; needs_edge = true; break;
						case assertion::word_boundary: case assertion::not_word_boundary: needs_word = true; break;
						case assertion::not_word_boundary_char: case assertion::not_word_boundary_char_reverse: needs_word = true; needs_high = true; break;
						default: needs_edge = true; break;
					}
				}
			}
			for (const byte_bits& b : sets) {
				for (unsigned c = 1; c < 256; ++c) { if (bit(b, c) != bit(b, c - 1)) { cut[c] = true; } }
			}
			if (needs_word) {
				for (unsigned c : {unsigned('0'), unsigned('9') + 1, unsigned('A'), unsigned('Z') + 1, unsigned('_'), unsigned('_') + 1, unsigned('a'), unsigned('z') + 1}) { cut[c] = true; }
			}
			if (needs_line) { cut['\n'] = true; cut['\n' + 1] = true; }
			if (needs_high) { cut[0x80] = true; cut[0xC0] = true; }
			uint32_t id = 0;
			class_byte[0] = 0;
			for (unsigned c = 0; c < 256; ++c) {
				if (c > 0 && cut[c]) { class_byte[++id] = static_cast<uint8_t>(c); }
				class_of[c] = static_cast<uint8_t>(id);
			}
			classes = id + 1;
		}
	};

	struct cp_range { uint32_t lo, hi; };

	inline void normalize(std::vector<cp_range>& r) {
		std::sort(r.begin(), r.end(), [](const cp_range& a, const cp_range& b) { return a.lo < b.lo; });
		std::vector<cp_range> out;
		for (const cp_range& x : r) {
			if (!out.empty() && x.lo <= out.back().hi + 1) { out.back().hi = std::max(out.back().hi, x.hi); }
			else { out.push_back(x); }
		}
		r.swap(out);
	}
	inline std::vector<cp_range> complement(const std::vector<cp_range>& r, uint32_t top) {
		std::vector<cp_range> out;
		uint32_t next = 0;
		for (const cp_range& x : r) {
			if (x.lo > next) { out.push_back({next, x.lo - 1}); }
			next = x.hi + 1;
		}
		if (next <= top) { out.push_back({next, top}); }
		return out;
	}
	inline void class_escape_ranges(uint32_t letter, std::vector<cp_range>& out) {
		switch (letter) {
			case 'd': out.push_back({'0', '9'}); break;
			case 'w': out.push_back({'0', '9'}); out.push_back({'A', 'Z'}); out.push_back({'_', '_'}); out.push_back({'a', 'z'}); break;
			default: out.push_back({'\t', '\r'}); out.push_back({' ', ' '}); break;   // string::whitespace: \t \n \v \f \r and space
		}
	}
	inline void add_ascii_folds(std::vector<cp_range>& r) {
		const size_t n = r.size();
		for (size_t k = 0; k < n; ++k) {
			const cp_range x = r[k];
			const uint32_t lo1 = std::max<uint32_t>(x.lo, 'a'), hi1 = std::min<uint32_t>(x.hi, 'z');
			if (lo1 <= hi1) { r.push_back({lo1 - 32, hi1 - 32}); }
			const uint32_t lo2 = std::max<uint32_t>(x.lo, 'A'), hi2 = std::min<uint32_t>(x.hi, 'Z');
			if (lo2 <= hi2) { r.push_back({lo2 + 32, hi2 + 32}); }
		}
	}
	inline std::vector<cp_range> without_surrogates(const std::vector<cp_range>& r) {
		std::vector<cp_range> out;
		for (const cp_range& x : r) {
			if (x.hi < 0xD800 || x.lo > 0xDFFF) { out.push_back(x); continue; }
			if (x.lo < 0xD800) { out.push_back({x.lo, 0xD7FF}); }
			if (x.hi > 0xDFFF) { out.push_back({0xE000, x.hi}); }
		}
		return out;
	}

	struct byte_span { uint8_t lo, hi; };
	using byte_sequence = std::vector<byte_span>;

	inline size_t encode_bytes(uint32_t cp, uint8_t* out) {
		std::string s;
		utf8::encode(cp, s);
		for (size_t k = 0; k < s.size(); ++k) { out[k] = static_cast<uint8_t>(s[k]); }
		return s.size();
	}

	// Code point ranges (no surrogates, none below U+0080) as UTF-8 byte-range sequences, each of one encoded length
	inline std::vector<byte_sequence> utf8_sequences(const std::vector<cp_range>& ranges) {
		std::vector<byte_sequence> out;
		std::vector<cp_range> work(ranges.rbegin(), ranges.rend());
		while (!work.empty()) {
			cp_range r = work.back();
			work.pop_back();
			for (bool split = true; split;) {
				split = false;
				for (uint32_t top : {0x7FFu, 0xFFFFu}) {
					if (r.lo <= top && top < r.hi) {
						work.push_back({top + 1, r.hi});
						r.hi = top;
						split = true;
						break;
					}
				}
				for (uint32_t k = 1; k < 4 && !split; ++k) {
					const uint32_t m = (uint32_t(1) << (6 * k)) - 1;
					if ((r.lo & ~m) == (r.hi & ~m)) { continue; }
					if ((r.lo & m) != 0) {
						work.push_back({(r.lo | m) + 1, r.hi});
						r.hi = r.lo | m;
						split = true;
					} else if ((r.hi & m) != m) {
						work.push_back({r.hi & ~m, r.hi});
						r.hi = (r.hi & ~m) - 1;
						split = true;
					}
				}
			}
			uint8_t a[4], b[4];
			const size_t na = encode_bytes(r.lo, a);
			const size_t nb = encode_bytes(r.hi, b);
			byte_sequence seq;
			for (size_t k = 0; k < na && k < nb; ++k) { seq.push_back({a[k], b[k]}); }
			out.push_back(std::move(seq));
		}
		return out;
	}

	struct mode_refusal {
		uint8_t kind = 0;   // 0 none, 1 a string's class holds a multi-byte character, 2 the program is too large
		uint32_t pos = 0;
		uint32_t cp = 0;
		bool refused() const { return kind != 0; }
	};

	inline std::string refusal_text(const mode_refusal& r, subject s) {
		if (r.kind == 1) {
			std::string ch;
			utf8::encode(r.cp, ch);
			return "regex: the class at position " + std::to_string(r.pos) + " holds '" + ch +
			       "', a character of more than one byte, but a string matches by bytes (match a utf8_string, or write its bytes as \\xNN)";
		}
		return std::string("regex: the pattern is too large to match ") + (s == subject::utf8 ? "a utf8_string" : "a string") +
		       " (its program passes " + std::to_string(k_program_limit) + " instructions)";
	}

	class program_compiler {
	public:
		program_compiler(const tree& t, subject s, bool reverse, program& out) : t_(t), s_(s), reverse_(reverse), p_(out) {}

		bool run(mode_refusal& refusal) {
			if (!reverse_) {
				p_.insts.push_back(inst{op::split, 0, 0, assertion::begin_text, 2, 1});   // the unanchored start: a lazy any-byte loop
				p_.insts.push_back(inst{op::range, 0, 255, assertion::begin_text, 0, 0});
				p_.start_unanchored = 0;
				p_.start_anchored = 2;
				emit(inst{op::save, 0, 0, assertion::begin_text, 0, 0});
			}
			if (!emit_tree(t_.root, refusal)) { return false; }
			if (!reverse_) { emit(inst{op::save, 0, 0, assertion::begin_text, 0, 1}); }
			emit(inst{op::match});
			p_.slots = 2 * (t_.groups + 1);
			p_.finish();
			if (p_.insts.size() > k_program_limit || static_cast<size_t>(p_.rows) * p_.slots > k_capture_table_limit) {
				refusal = mode_refusal{2, 0, 0};
				return false;
			}
			return true;
		}

	private:
		const tree& t_;
		subject s_;
		bool reverse_;
		program& p_;

		uint32_t here() const { return static_cast<uint32_t>(p_.insts.size()); }
		uint32_t emit(inst in) {
			const uint32_t pc = here();
			if (in.code != op::split && in.code != op::jump && in.code != op::match && in.code != op::fail) { in.x = pc + 1; }
			p_.insts.push_back(in);
			return pc;
		}
		void emit_range(uint8_t lo, uint8_t hi) { emit(inst{op::range, lo, hi}); }
		void emit_set(const byte_bits& b) {
			p_.sets.push_back(b);
			inst in{op::set};
			in.y = static_cast<uint32_t>(p_.sets.size() - 1);
			emit(in);
		}

		void emit_literal(const node& nd) {
			const uint32_t cp = nd.value;
			if (cp < 0x80 && nd.fold && ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))) {
				byte_bits b{};
				set_bit(b, cp | 0x20);
				set_bit(b, cp & ~0x20u);
				emit_set(b);
				return;
			}
			if (cp < 0x80 || (s_ == subject::bytes && nd.raw)) {
				emit_range(static_cast<uint8_t>(cp), static_cast<uint8_t>(cp));
				return;
			}
			uint8_t enc[4];
			const size_t n = encode_bytes(cp, enc);
			for (size_t k = 0; k < n; ++k) {
				const uint8_t b = enc[reverse_ ? n - 1 - k : k];
				emit_range(b, b);
			}
		}

		// One code point of the ranges: the ASCII part as one byte set, then a byte chain per UTF-8 sequence, as alternatives
		void emit_codepoints(std::vector<cp_range> ranges) {
			normalize(ranges);
			ranges = without_surrogates(ranges);
			byte_bits ascii{};
			bool has_ascii = false;
			std::vector<cp_range> wide;
			for (const cp_range& r : ranges) {
				if (r.lo <= 0x7F) {
					for (uint32_t c = r.lo; c <= std::min<uint32_t>(r.hi, 0x7F); ++c) { set_bit(ascii, c); }
					has_ascii = true;
				}
				if (r.hi > 0x7F) { wide.push_back({std::max<uint32_t>(r.lo, 0x80), r.hi}); }
			}
			const std::vector<byte_sequence> seqs = utf8_sequences(wide);
			const size_t total = seqs.size() + (has_ascii ? 1 : 0);
			if (total == 0) { emit(inst{op::fail}); return; }
			std::vector<uint32_t> ends;
			size_t done = 0;
			auto alternative = [&](auto&& body) {
				const bool more = done + 1 < total;
				uint32_t split = 0;
				if (more) { split = emit(inst{op::split}); p_.insts[split].x = split + 1; }
				body();
				if (more) {
					ends.push_back(emit(inst{op::jump}));
					p_.insts[split].y = here();
				}
				++done;
			};
			if (has_ascii) { alternative([&] { emit_set(ascii); }); }
			for (const byte_sequence& seq : seqs) {
				alternative([&] {
					const size_t n = seq.size();
					for (size_t k = 0; k < n; ++k) {
						const byte_span& b = seq[reverse_ ? n - 1 - k : k];
						emit_range(b.lo, b.hi);
					}
				});
			}
			for (uint32_t pc : ends) { p_.insts[pc].x = here(); }
		}

		bool emit_char_set(const char_set& cs, mode_refusal& refusal) {
			std::vector<cp_range> r;
			const uint32_t top = s_ == subject::bytes ? 255 : k_max_codepoint;
			for (const set_item& item : cs.items) {
				if (item.kind == 1) {
					std::vector<cp_range> e;
					class_escape_ranges(item.lo, e);
					if (item.hi) { normalize(e); e = complement(e, top); }
					r.insert(r.end(), e.begin(), e.end());
					continue;
				}
				if (s_ == subject::bytes && ((!item.lo_raw && item.lo >= 0x80) || (!item.hi_raw && item.hi >= 0x80))) {
					refusal = mode_refusal{1, cs.pos, !item.lo_raw && item.lo >= 0x80 ? item.lo : item.hi};
					return false;
				}
				r.push_back({item.lo, item.hi});
			}
			if (cs.fold) { add_ascii_folds(r); }
			normalize(r);
			if (cs.negated) { r = complement(r, top); }
			if (s_ == subject::utf8) {
				emit_codepoints(std::move(r));
				return true;
			}
			byte_bits b{};
			for (const cp_range& x : r) { for (uint32_t c = x.lo; c <= x.hi && c < 256; ++c) { set_bit(b, c); } }
			emit_set(b);
			return true;
		}

		void emit_any(const node& nd) {
			if (s_ == subject::bytes) {
				if (nd.dotall) { emit_range(0, 255); return; }
				byte_bits b{};
				for (unsigned c = 0; c < 256; ++c) { if (c != '\n') { set_bit(b, c); } }
				emit_set(b);
				return;
			}
			std::vector<cp_range> r;
			if (nd.dotall) { r.push_back({0, k_max_codepoint}); }
			else { r.push_back({0, '\n' - 1}); r.push_back({'\n' + 1, k_max_codepoint}); }
			emit_codepoints(std::move(r));
		}

		struct frame {
			explicit frame(uint32_t n) : node(n) {}
			uint32_t node;
			uint32_t stage = 0;
			uint32_t count = 0;
			uint32_t mark = 0;
			std::vector<uint32_t> pending;   // pc << 1 | 1 when the instruction's x waits for the exit, pc << 1 when its y does
		};

		void patch(uint32_t entry, uint32_t to) {
			inst& in = p_.insts[entry >> 1];
			if (entry & 1) { in.x = to; } else { in.y = to; }
		}
		// A split that takes `body` first (greedy) or the exit first (lazy); the exit is patched later
		uint32_t emit_loop_split(bool greedy, uint32_t body, std::vector<uint32_t>& pending) {
			const uint32_t pc = emit(inst{op::split});
			if (greedy) { p_.insts[pc].x = body; pending.push_back(pc << 1); }
			else { p_.insts[pc].y = body; pending.push_back((pc << 1) | 1); }
			return pc;
		}

		bool emit_tree(uint32_t root, mode_refusal& refusal) {
			std::vector<frame> stack;
			stack.push_back(frame{root});
			while (!stack.empty()) {
				if (p_.insts.size() > k_program_limit) { refusal = mode_refusal{2, 0, 0}; return false; }
				frame& f = stack.back();
				const node& nd = t_.nodes[f.node];
				switch (nd.kind) {
					case node_kind::literal: emit_literal(nd); stack.pop_back(); break;
					case node_kind::set:
						if (!emit_char_set(t_.sets[nd.value], refusal)) { return false; }
						stack.pop_back();
						break;
					case node_kind::any: emit_any(nd); stack.pop_back(); break;
					case node_kind::test: {
						inst in{op::test};
						const assertion a = nd.test == assertion::not_word_boundary && s_ == subject::utf8 ? assertion::not_word_boundary_char : nd.test;
						in.test = reverse_ ? mirrored(a) : a;
						emit(in);
						stack.pop_back();
						break;
					}
					case node_kind::group: {
						const bool capture = nd.value != k_none && !reverse_;
						inst save{op::save};
						if (f.stage == 0) {
							if (capture) { save.y = 2 * nd.value; emit(save); }
							f.stage = 1;
							const uint32_t kid = nd.kids[0];
							stack.push_back(frame{kid});
						} else {
							if (capture) { save.y = 2 * nd.value + 1; emit(save); }
							stack.pop_back();
						}
						break;
					}
					case node_kind::concat: {
						const size_t n = nd.kids.size();
						if (f.stage < n) {
							const uint32_t kid = nd.kids[reverse_ ? n - 1 - f.stage : f.stage];
							++f.stage;
							stack.push_back(frame{kid});
						} else {
							stack.pop_back();
						}
						break;
					}
					case node_kind::alternate: {
						const size_t n = nd.kids.size();
						if (f.stage > 0 && f.stage < n) {
							f.pending.push_back(emit(inst{op::jump}) << 1 | 1);
							p_.insts[f.mark].y = here();
						}
						if (f.stage < n) {
							if (f.stage + 1 < n) {
								f.mark = emit(inst{op::split});
								p_.insts[f.mark].x = f.mark + 1;
							}
							const uint32_t kid = nd.kids[f.stage++];
							stack.push_back(frame{kid});
						} else {
							for (uint32_t e : f.pending) { patch(e, here()); }
							stack.pop_back();
						}
						break;
					}
					case node_kind::repeat: {
						const uint32_t kid = nd.kids[0];
						const bool unbounded = nd.max == k_unbounded;
						const uint32_t plain = unbounded ? (nd.min == 0 ? 0 : nd.min - 1) : nd.min;
						if (f.stage == 0) {
							if (f.count < plain) { ++f.count; stack.push_back(frame{kid}); break; }
							f.stage = 1;
							f.count = 0;
						}
						if (unbounded && nd.min == 0) {   // L: split body, exit; body; jump L
							if (f.stage == 1) {
								f.mark = emit_loop_split(nd.greedy, here() + 1, f.pending);
								f.stage = 2;
								stack.push_back(frame{kid});
							} else {
								inst back{op::jump};
								back.x = f.mark;
								emit(back);
								for (uint32_t e : f.pending) { patch(e, here()); }
								stack.pop_back();
							}
						} else if (unbounded) {   // body; split body, exit
							if (f.stage == 1) {
								f.mark = here();
								f.stage = 2;
								stack.push_back(frame{kid});
							} else {
								emit_loop_split(nd.greedy, f.mark, f.pending);
								for (uint32_t e : f.pending) { patch(e, here()); }
								stack.pop_back();
							}
						} else if (f.count < nd.max - nd.min) {   // nested optional copies, every exit to the end
							++f.count;
							emit_loop_split(nd.greedy, here() + 1, f.pending);
							stack.push_back(frame{kid});
						} else {
							for (uint32_t e : f.pending) { patch(e, here()); }
							stack.pop_back();
						}
						break;
					}
					default:   // empty (the refused kinds never compile)
						stack.pop_back();
						break;
				}
			}
			return true;
		}
	};

	// ===== matching =====

	struct sparse_set {
		std::vector<uint32_t> dense, sparse;
		uint32_t size = 0;
		void resize(size_t n) { dense.assign(n, 0); sparse.assign(n, 0); size = 0; }
		bool contains(uint32_t v) const { const uint32_t k = sparse[v]; return k < size && dense[k] == v; }
		void insert(uint32_t v) { sparse[v] = size; dense[size++] = v; }
		void clear() { size = 0; }
	};

	struct pike_scratch {
		sparse_set lists[2];
		std::vector<size_t> caps[2];
		std::vector<size_t> work;
		struct step { uint32_t pc; uint32_t slot; size_t old; bool restore; };
		std::vector<step> stack;
		const program* prepared = nullptr;
		void prepare(const program& p) {
			if (prepared == &p) { return; }
			prepared = &p;
			for (int k = 0; k < 2; ++k) {
				lists[k].resize(p.insts.size());
				caps[k].assign(static_cast<size_t>(p.rows) * p.slots, npos);
			}
			work.assign(p.slots, npos);
		}
		void trim() {
			if ((caps[0].size() + caps[1].size()) * sizeof(size_t) + (lists[0].dense.size() + lists[1].dense.size()) * 2 * sizeof(uint32_t) <= k_dfa_kept) { return; }
			*this = pike_scratch{};
		}
	};

	// Adds the thread at pc (and every instruction its empty moves reach) to the list in priority order; caps is restored
	inline void pike_add(const program& p, std::string_view text, size_t at, uint32_t start, std::vector<size_t>& caps,
	                     sparse_set& list, std::vector<size_t>& table, pike_scratch& s) {
		const int prev = at > 0 ? static_cast<uint8_t>(text[at - 1]) : -1;
		const int next = at < text.size() ? static_cast<uint8_t>(text[at]) : -1;
		s.stack.clear();
		s.stack.push_back({start, 0, 0, false});
		while (!s.stack.empty()) {
			const pike_scratch::step st = s.stack.back();
			s.stack.pop_back();
			if (st.restore) { caps[st.slot] = st.old; continue; }
			const uint32_t pc = st.pc;
			if (list.contains(pc)) { continue; }
			list.insert(pc);
			const inst& in = p.insts[pc];
			switch (in.code) {
				case op::jump: s.stack.push_back({in.x, 0, 0, false}); break;
				case op::split:
					s.stack.push_back({in.y, 0, 0, false});
					s.stack.push_back({in.x, 0, 0, false});
					break;
				case op::save:
					s.stack.push_back({0, in.y, caps[in.y], true});
					caps[in.y] = at;
					s.stack.push_back({in.x, 0, 0, false});
					break;
				case op::test:
					if (assertion_holds(in.test, prev, next)) { s.stack.push_back({in.x, 0, 0, false}); }
					break;
				case op::fail: break;
				default:
					std::copy(caps.begin(), caps.end(), table.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(p.row_of[pc]) * p.slots));
					break;
			}
		}
	}

	// The leftmost-first match from `start` (only there when anchored); the scan stops at `stop`, where a bounded run knows
	// the match ends. full: a match counts only at the end of the text.
	inline bool pike_run(const program& p, std::string_view text, size_t start, size_t stop, bool anchored, bool full,
	                     std::vector<size_t>& out, pike_scratch& s) {
		s.prepare(p);
		const size_t nslots = p.slots;
		sparse_set* clist = &s.lists[0];
		sparse_set* nlist = &s.lists[1];
		std::vector<size_t>* ccaps = &s.caps[0];
		std::vector<size_t>* ncaps = &s.caps[1];
		clist->clear();
		bool matched = false;
		out.assign(nslots, npos);
		std::fill(s.work.begin(), s.work.end(), npos);
		pike_add(p, text, start, anchored ? p.start_anchored : p.start_unanchored, s.work, *clist, *ccaps, s);
		for (size_t at = start; clist->size != 0; ++at) {
			const bool more = at < stop;
			const unsigned c = more ? static_cast<uint8_t>(text[at]) : 0;
			nlist->clear();
			for (uint32_t k = 0; k < clist->size; ++k) {
				const uint32_t pc = clist->dense[k];
				const inst& in = p.insts[pc];
				if (in.code == op::match) {
					if (full && at != text.size()) { continue; }
					const auto row = ccaps->begin() + static_cast<ptrdiff_t>(static_cast<size_t>(p.row_of[pc]) * nslots);
					std::copy(row, row + static_cast<ptrdiff_t>(nslots), out.begin());
					matched = true;
					break;   // the threads after a match have lower priority
				}
				if ((in.code == op::range || in.code == op::set) && more && p.accepts(in, c)) {
					const auto row = ccaps->begin() + static_cast<ptrdiff_t>(static_cast<size_t>(p.row_of[pc]) * nslots);
					std::copy(row, row + static_cast<ptrdiff_t>(nslots), s.work.begin());
					pike_add(p, text, at + 1, in.x, s.work, *nlist, *ncaps, s);
				}
			}
			if (!more) { break; }
			std::swap(clist, nlist);
			std::swap(ccaps, ncaps);
		}
		return matched;
	}

	class dfa {
	public:
		enum class kind : uint8_t { leftmost, longest };
		static constexpr uint8_t k_match = 1, k_dead = 2;
		static constexpr uint8_t k_prev_edge = 1, k_prev_line = 2, k_prev_word = 4, k_prev_high = 8, k_prev_continuation = 16;
		static constexpr int32_t k_unknown = -1;

		dfa(const program& p, kind k) : p_(p), kind_(k), stride_(p.classes + 1) {
			visited_.resize(p.insts.size());
			reset();
		}

		uint32_t eot() const { return p_.classes; }
		uint32_t class_of(uint8_t b) const { return p_.class_of[b]; }
		bool is_match(int32_t s) const { return (info_[static_cast<size_t>(s)] & k_match) != 0; }
		bool is_dead(int32_t s) const { return (info_[static_cast<size_t>(s)] & k_dead) != 0; }
		uint64_t resets() const { return resets_; }
		size_t states_before_reset() const { return states_before_reset_; }
		// Between searches a cache past k_dfa_kept starts over, so a regex keeps at most that much
		void trim() {
			if (memory_ > k_dfa_kept) { reset(); }
		}

		int32_t start(bool anchored, int prev) {
			const uint8_t ctx = context_of(prev);
			int32_t& slot = starts_[anchored ? 1 : 0][ctx];
			if (slot != k_unknown) { return slot; }
			kernel_.assign(1, anchored ? p_.start_anchored : p_.start_unanchored);
			const int32_t s = intern(kernel_, static_cast<uint8_t>(ctx << 2));
			starts_[anchored ? 1 : 0][ctx] = s;
			return s;
		}

		int32_t step(int32_t s, uint32_t cls) {
			const int32_t t = trans_[static_cast<size_t>(s) * stride_ + cls];
			return t >= 0 ? t : compute(s, cls);
		}

	private:
		const program& p_;
		kind kind_;
		uint32_t stride_;
		std::vector<std::vector<uint32_t>> states_;
		std::vector<uint8_t> flags_;   // the prev byte's context << 2 | k_match
		std::vector<uint8_t> info_;
		std::vector<int32_t> trans_;
		std::unordered_map<std::string, int32_t> index_;
		int32_t starts_[2][32] = {};
		size_t memory_ = 0;
		uint64_t resets_ = 0;
		size_t states_before_reset_ = 0;
		sparse_set visited_;
		std::vector<uint32_t> stack_, closure_, kernel_;
		std::string key_;

		uint8_t context_of(int prev) const {
			if (prev < 0) { return p_.needs_edge ? k_prev_edge : 0; }
			uint8_t ctx = 0;
			if (p_.needs_line && prev == '\n') { ctx |= k_prev_line; }
			if (p_.needs_word && word_byte(prev)) { ctx |= k_prev_word; }
			if (p_.needs_high && prev >= 0x80) { ctx |= prev <= 0xBF ? (k_prev_high | k_prev_continuation) : k_prev_high; }
			return ctx;
		}
		static int prev_of(uint8_t ctx) {
			if (ctx & k_prev_edge) { return -1; }
			if (ctx & k_prev_line) { return '\n'; }
			if (ctx & k_prev_high) { return (ctx & k_prev_continuation) ? 0x80 : 0xC2; }
			return (ctx & k_prev_word) ? 'a' : 1;
		}

		void reset() {
			states_before_reset_ = states_.size();
			states_.clear();
			flags_.clear();
			info_.clear();
			trans_.clear();
			index_.clear();
			memory_ = 0;
			for (auto& row : starts_) { for (int32_t& v : row) { v = k_unknown; } }
		}

		int32_t intern(const std::vector<uint32_t>& kernel, uint8_t flags) {
			key_.assign(1, static_cast<char>(flags));
			key_.append(reinterpret_cast<const char*>(kernel.data()), kernel.size() * sizeof(uint32_t));
			if (auto it = index_.find(key_); it != index_.end()) { return it->second; }
			const size_t cost = sizeof(std::vector<uint32_t>) + kernel.size() * sizeof(uint32_t) * 3 + stride_ * sizeof(int32_t) + 64;
			if (memory_ + cost > k_dfa_budget && !states_.empty()) {
				++resets_;
				reset();
			}
			memory_ += cost;
			const int32_t id = static_cast<int32_t>(states_.size());
			states_.push_back(kernel);
			flags_.push_back(flags);
			info_.push_back(static_cast<uint8_t>((flags & k_match) | (kernel.empty() ? k_dead : 0)));
			trans_.resize(trans_.size() + stride_, k_unknown);
			index_.emplace(key_, id);
			return id;
		}

		int32_t compute(int32_t s, uint32_t cls) {
			const int prev = prev_of(static_cast<uint8_t>(flags_[static_cast<size_t>(s)] >> 2));
			const bool at_end = cls == eot();
			const int next = at_end ? -1 : p_.class_byte[cls];
			visited_.clear();
			closure_.clear();
			bool matched = false;
			const std::vector<uint32_t>& from = states_[static_cast<size_t>(s)];
			for (size_t k = 0; k < from.size() && !(matched && kind_ == kind::leftmost); ++k) {
				stack_.clear();
				stack_.push_back(from[k]);
				while (!stack_.empty()) {
					const uint32_t pc = stack_.back();
					stack_.pop_back();
					if (visited_.contains(pc)) { continue; }
					visited_.insert(pc);
					const inst& in = p_.insts[pc];
					switch (in.code) {
						case op::jump: case op::save: stack_.push_back(in.x); break;
						case op::split: stack_.push_back(in.y); stack_.push_back(in.x); break;
						case op::test: if (assertion_holds(in.test, prev, next)) { stack_.push_back(in.x); } break;
						case op::fail: break;
						case op::match:
							matched = true;
							if (kind_ == kind::leftmost) { stack_.clear(); }   // lower-priority threads never win
							break;
						default: closure_.push_back(pc); break;
					}
				}
			}
			kernel_.clear();
			uint8_t ctx = 0;
			if (!at_end) {
				const unsigned c = static_cast<unsigned>(next);
				visited_.clear();
				for (uint32_t pc : closure_) {
					const inst& in = p_.insts[pc];
					if (p_.accepts(in, c) && !visited_.contains(in.x)) {
						visited_.insert(in.x);
						kernel_.push_back(in.x);
					}
				}
				ctx = context_of(next);
			}
			const uint64_t before = resets_;
			const int32_t t = intern(kernel_, static_cast<uint8_t>((ctx << 2) | (matched ? k_match : 0)));
			if (resets_ == before) { trans_[static_cast<size_t>(s) * stride_ + cls] = t; }
			return t;
		}
	};

	enum class scan { found, none, gave_up };

	// A second cache reset before the bytes since the first paid for the states it held hands the search to the Pike VM
	struct reset_watch {
		uint64_t seen;
		size_t last = npos;
		explicit reset_watch(const dfa& d) : seen(d.resets()) {}
		bool thrashing(const dfa& d, size_t at) {
			if (d.resets() == seen) { return false; }
			seen = d.resets();
			const bool slow = last != npos && (at > last ? at - last : last - at) < 10 * d.states_before_reset();
			last = at;
			return slow;
		}
	};

	inline scan scan_forward(dfa& d, std::string_view text, size_t from, size_t& end) {
		const size_t n = text.size();
		int32_t s = d.start(false, from > 0 ? static_cast<int>(static_cast<uint8_t>(text[from - 1])) : -1);
		reset_watch watch(d);
		bool found = false;
		for (size_t i = from; i < n; ++i) {
			s = d.step(s, d.class_of(static_cast<uint8_t>(text[i])));
			if (watch.thrashing(d, i)) { return scan::gave_up; }
			if (d.is_match(s)) { found = true; end = i; }
			if (d.is_dead(s)) { return found ? scan::found : scan::none; }
		}
		s = d.step(s, d.eot());
		if (d.is_match(s)) { found = true; end = n; }
		return found ? scan::found : scan::none;
	}

	inline scan scan_reverse(dfa& d, std::string_view text, size_t end, size_t lo, size_t& begin) {
		const size_t n = text.size();
		int32_t s = d.start(true, end < n ? static_cast<int>(static_cast<uint8_t>(text[end])) : -1);
		reset_watch watch(d);
		bool found = false;
		for (size_t i = end; i > lo; --i) {
			s = d.step(s, d.class_of(static_cast<uint8_t>(text[i - 1])));
			if (watch.thrashing(d, i)) { return scan::gave_up; }
			if (d.is_match(s)) { found = true; begin = i; }
			if (d.is_dead(s)) { return found ? scan::found : scan::none; }
		}
		s = d.step(s, lo > 0 ? d.class_of(static_cast<uint8_t>(text[lo - 1])) : d.eot());
		if (d.is_match(s)) { found = true; begin = lo; }
		return found ? scan::found : scan::none;
	}

	inline scan scan_full(dfa& d, std::string_view text) {
		const size_t n = text.size();
		int32_t s = d.start(true, -1);
		reset_watch watch(d);
		for (size_t i = 0; i < n; ++i) {
			s = d.step(s, d.class_of(static_cast<uint8_t>(text[i])));
			if (watch.thrashing(d, i)) { return scan::gave_up; }
			if (d.is_dead(s)) { return scan::none; }
		}
		s = d.step(s, d.eot());
		return d.is_match(s) ? scan::found : scan::none;
	}

	// A search claims a slot's DFA for its whole run; searches on other threads at once take the next slots, so each keeps
	// its states across calls, and a search that finds every slot taken builds a private one
	struct dfa_slot {
		std::atomic<bool> busy{false};
		std::unique_ptr<dfa> cached;
		pike_scratch pike;   // the Pike VM's lists for the forward program, kept with the forward slots
	};
	inline constexpr size_t k_dfa_slots = 4;
	using dfa_slots = std::array<dfa_slot, k_dfa_slots>;

	struct subject_programs {
		program forward, reverse;
		mode_refusal refusal;
		mutable dfa_slots leftmost, longest, backward;
	};

	inline std::string unsupported_text(const tree& t, const std::vector<uint32_t>& cps) {
		const node& nd = t.nodes[t.unsupported];
		std::string what;
		switch (nd.kind) {
			case node_kind::backreference: what = "the backreference '" + encode_text(cps, nd.pos, nd.end) + "'"; break;
			case node_kind::lookaround:
				what = nd.value == 0 ? "the lookahead '(?='" : nd.value == 1 ? "the negative lookahead '(?!'"
				     : nd.value == 2 ? "the lookbehind '(?<='" : "the negative lookbehind '(?<!'";
				break;
			case node_kind::atomic: what = "the atomic group '(?>'"; break;
			case node_kind::recursion: what = "the recursion '" + encode_text(cps, nd.pos, nd.end) + "'"; break;
			default: what = "the possessive quantifier '" + encode_text(cps, nd.pos, nd.end) + "'"; break;
		}
		return "regex: " + what + " at position " + std::to_string(nd.pos) + " is not supported: matching runs in linear time, without backtracking";
	}

	inline const std::string& empty_name() {
		static const std::string none;
		return none;
	}

} // namespace jai::detail::regex_engine

namespace jai::detail::regex {

	class compiled {
	public:
		std::string pattern, flags;
		uint32_t groups = 0;
		std::vector<std::string> names;
		regex_engine::subject_programs subjects[2];
	};

	compiled_ptr compile(std::string_view pattern, std::string_view flags, std::string& error) {
		regex_engine::parse_flags pf;
		for (const char f : flags) {
			bool* target = f == 'i' ? &pf.fold : f == 'm' ? &pf.multiline : f == 's' ? &pf.dotall : f == 'x' ? &pf.extended : nullptr;
			if (!target) {
				error = "regex: unknown flag '" + std::string(1, f) + "' (the flags are i, m, s and x)";
				return nullptr;
			}
			if (*target) {
				error = "regex: flag '" + std::string(1, f) + "' appears twice";
				return nullptr;
			}
			*target = true;
		}
		std::vector<uint32_t> cps;
		cps.reserve(pattern.size());
		for (size_t at = 0; at < pattern.size();) {
			const utf8::step st = utf8::decode(pattern, at);
			if (!st.valid) {
				error = "regex: the pattern is not valid UTF-8 at byte " + std::to_string(at) + " (write a byte as \\xNN)";
				return nullptr;
			}
			cps.push_back(st.codepoint);
			at += st.length;
		}
		regex_engine::tree t;
		if (!regex_engine::pattern_parser(cps, pf, t, error).run()) { return nullptr; }
		if (t.unsupported != regex_engine::k_none) {
			error = regex_engine::unsupported_text(t, cps);
			return nullptr;
		}
		auto out = std::make_shared<compiled>();
		out->pattern.assign(pattern);
		out->flags.assign(flags);
		out->groups = t.groups;
		out->names = std::move(t.names);
		for (int k = 0; k < 2; ++k) {
			regex_engine::subject_programs& sp = out->subjects[k];
			const subject s = static_cast<subject>(k);
			regex_engine::mode_refusal refusal;
			if (!regex_engine::program_compiler(t, s, false, sp.forward).run(refusal) || !regex_engine::program_compiler(t, s, true, sp.reverse).run(refusal)) {
				sp.refusal = refusal;
				sp.forward = regex_engine::program{};
				sp.reverse = regex_engine::program{};
			}
		}
		if (out->subjects[0].refusal.refused() && out->subjects[1].refusal.refused()) {
			error = regex_engine::refusal_text(out->subjects[1].refusal, subject::utf8);
			return nullptr;
		}
		return out;
	}

	const std::string& pattern_of(const compiled& re) { return re.pattern; }
	const std::string& flags_of(const compiled& re) { return re.flags; }
	size_t group_count(const compiled& re) { return re.groups; }
	size_t group_index(const compiled& re, std::string_view name) {
		for (size_t k = 1; k < re.names.size(); ++k) {
			if (re.names[k] == name) { return k; }
		}
		return npos;
	}
	const std::string& group_name(const compiled& re, size_t index) {
		return index < re.names.size() ? re.names[index] : regex_engine::empty_name();
	}
	size_t footprint(const compiled& re) {
		size_t bytes = sizeof(compiled) + re.pattern.size();
		for (const auto& sp : re.subjects) {
			for (const regex_engine::program* p : {&sp.forward, &sp.reverse}) {
				bytes += p->insts.size() * sizeof(regex_engine::inst) + p->sets.size() * sizeof(regex_engine::byte_bits) + p->row_of.size() * sizeof(uint32_t);
			}
		}
		return bytes;
	}

	std::string subject_refusal(const compiled& re, subject s) {
		const regex_engine::subject_programs& sp = re.subjects[static_cast<int>(s)];
		return sp.refusal.refused() ? regex_engine::refusal_text(sp.refusal, s) : std::string();
	}

	std::string escape(std::string_view text) {
		std::string out;
		out.reserve(text.size() + 8);
		for (const char ch : text) {
			switch (ch) {
				case '\\': case '^': case '$': case '.': case '|': case '?': case '*': case '+': case '(': case ')':
				case '[': case ']': case '{': case '}': case '#': case ' ': case '-':
					out.push_back('\\');
					out.push_back(ch);
					break;
				case '\t': out += "\\t"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\v': out += "\\v"; break;
				case '\f': out += "\\f"; break;
				case '\0': out += "\\x00"; break;
				default: out.push_back(ch); break;
			}
		}
		return out;
	}

	size_t next_character(std::string_view text, size_t at, subject s) {
		if (at >= text.size() || s == subject::bytes) { return at + 1; }
		return at + utf8::decode(text, at).length;
	}

	struct searcher::state {
		struct held {
			regex_engine::dfa_slot* slot = nullptr;
			std::unique_ptr<regex_engine::dfa> own;
			std::unique_ptr<regex_engine::pike_scratch> own_pike;
			regex_engine::dfa* d = nullptr;
			~held() {
				if (slot) {
					if (slot->cached) { slot->cached->trim(); }
					slot->pike.trim();
					slot->busy.store(false, std::memory_order_release);
				}
			}
			regex_engine::pike_scratch& pike() {
				if (slot) { return slot->pike; }
				if (!own_pike) { own_pike = std::make_unique<regex_engine::pike_scratch>(); }
				return *own_pike;
			}
		};
		const compiled& re;
		const regex_engine::subject_programs& sp;
		held leftmost, longest, backward;

		state(const compiled& r, subject s) : re(r), sp(r.subjects[static_cast<int>(s)]) {}

		regex_engine::dfa& acquire(held& h, regex_engine::dfa_slots& slots, const regex_engine::program& p, regex_engine::dfa::kind k) {
			if (h.d) { return *h.d; }
			for (regex_engine::dfa_slot& slot : slots) {
				if (slot.busy.exchange(true, std::memory_order_acquire)) { continue; }
				if (!slot.cached) { slot.cached = std::make_unique<regex_engine::dfa>(p, k); }
				h.slot = &slot;
				h.d = slot.cached.get();
				return *h.d;
			}
			h.own = std::make_unique<regex_engine::dfa>(p, k);
			h.d = h.own.get();
			return *h.d;
		}
	};
	static_assert(sizeof(searcher::state) <= searcher::k_state_size && alignof(searcher::state) <= alignof(std::max_align_t));

	searcher::state& searcher::st() { return *std::launder(reinterpret_cast<state*>(storage_)); }
	searcher::searcher(const compiled& re, subject s) { new (storage_) state(re, s); }
	searcher::~searcher() { st().~state(); }

	bool searcher::find(std::string_view text, size_t from, bool groups, std::vector<size_t>& slots) {
		using namespace regex_engine;
		state& st = this->st();
		const program& fwd = st.sp.forward;
		slots.assign(2 * (st.re.groups + 1), npos);
		if (from > text.size() || st.sp.refusal.refused()) { return false; }
		size_t end = 0, begin = 0;
		const scan f = scan_forward(st.acquire(st.leftmost, st.sp.leftmost, fwd, dfa::kind::leftmost), text, from, end);
		if (f == scan::none) { return false; }
		if (f == scan::found && scan_reverse(st.acquire(st.backward, st.sp.backward, st.sp.reverse, dfa::kind::longest), text, end, from, begin) == scan::found) {
			if (!groups || st.re.groups == 0) {
				slots[0] = begin;
				slots[1] = end;
				return true;
			}
			if (pike_run(fwd, text, begin, end, true, false, slots, st.leftmost.pike()) && slots[0] == begin && slots[1] == end) { return true; }
		}
		return pike_run(fwd, text, from, text.size(), false, false, slots, st.leftmost.pike());
	}

	bool searcher::full(std::string_view text) {
		using namespace regex_engine;
		state& st = this->st();
		if (st.sp.refusal.refused()) { return false; }
		const scan r = scan_full(st.acquire(st.longest, st.sp.longest, st.sp.forward, dfa::kind::longest), text);
		if (r != scan::gave_up) { return r == scan::found; }
		std::vector<size_t> slots;
		return pike_run(st.sp.forward, text, 0, text.size(), true, true, slots, st.longest.pike());
	}

} // namespace jai::detail::regex
