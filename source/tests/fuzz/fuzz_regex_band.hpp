#pragma once

// Regex band: patterns drawn from the whole supported syntax (classes, class escapes, anchors and boundaries, capturing,
// named and plain groups, alternation, greedy and lazy quantifiers, inline flags) run through every regex call on strings
// and utf8_strings over a small alphabet with a two-byte character in it: search with and without a start position,
// matches, find_all, replace with references and with a function, replace_first, split with and without a count, the
// match's texts and positions; the pattern a literal (compiled once at load: op_regex_constant beside the interpreter's
// stamp) or built at run time (the engine's cache), and mutated texts that may not compile, whose errors must read alike.
// Its own RNG stream and name prefix `zr`; generate_program's default (never) keeps the programs of the gates that replay
// seeds.

#include "fuzz_harness.hpp"

#include <string>
#include <vector>

namespace jai::fuzz {

	class regex_band {
	public:
		explicit regex_band(uint64_t seed) : rng_(mix64(seed + 0x7F4A7C159E3779B9ULL)) {}

		bool roll() { return rng_.chance(25); }

		std::string emit() {
			wrapped_ = rng_.chance(50);
			std::vector<std::string> body{"var zrout = \"\";"};
			texts_.clear();
			const int texts = static_cast<int>(rng_.range(2, 4));
			for (int i = 0; i < texts; ++i) {
				const bool utf8 = rng_.chance(35);
				const std::string name = std::string(utf8 ? "zrU" : "zrT") + std::to_string(i);
				body.push_back(std::string(utf8 ? "utf8_string " : "string ") + name + " = \"" + text() + "\";");
				texts_.push_back(name);
			}
			const int n = static_cast<int>(rng_.range(6, 14));
			for (int i = 0; i < n; ++i) { body.push_back(statement()); }
			std::string out;
			if (wrapped_) { out += "function zrmain() -> var {\n"; }
			for (const auto& l : body) { out += l + "\n"; }
			out += wrapped_ ? "return zrout;\n}\nzrmain();\n" : "zrout;\n";
			return out;
		}

	private:
		fuzz_rng rng_;
		bool wrapped_ = false;
		std::vector<std::string> texts_;
		int names_ = 0;
		int groups_ = 0;
		std::vector<std::string> group_names_;

		std::string text() {
			static const char* const pieces[] = {"a", "b", "c", "1", "2", " ", "-", "\\xC3\\xA9", "\\n", "ab", "ba"};
			std::string out;
			const int len = static_cast<int>(rng_.range(0, 10));
			for (int i = 0; i < len; ++i) { out += pieces[rng_.below(11)]; }
			return out;
		}

		std::string atom(int depth) {
			switch (rng_.below(depth > 0 ? 12 : 8)) {
				case 0: return std::string(1, "abc12- "[rng_.below(7)]);
				case 1: return rng_.chance(50) ? "[a-c]" : "[^b ]";
				case 2: return std::string("\\") + "dDwWsS"[rng_.below(6)];
				case 3: return ".";
				case 4: return rng_.chance(50) ? "\xC3\xA9" : "[\\xE9a]";
				case 5: return rng_.chance(50) ? "[\\d-]" : "\\-";
				case 6: return std::string(1, "ab"[rng_.below(2)]);
				case 7: return rng_.chance(50) ? "\\x61" : "(?:)";
				case 8: { ++groups_; return "(" + alternation(depth - 1) + ")"; }
				case 9: {
					++groups_;
					group_names_.push_back("g" + std::to_string(groups_));
					return "(?<" + group_names_.back() + ">" + alternation(depth - 1) + ")";
				}
				case 10: return "(?:" + alternation(depth - 1) + ")";
				default: return std::string("(?") + (rng_.chance(50) ? "i" : "s") + ":" + alternation(depth - 1) + ")";
			}
		}
		std::string quantified(int depth) {
			std::string a = atom(depth);
			switch (rng_.below(10)) {
				case 0: a += "*"; break;
				case 1: a += "+"; break;
				case 2: a += "?"; break;
				case 3: a += "{" + std::to_string(rng_.range(0, 2)) + "," + std::to_string(rng_.range(2, 3)) + "}"; break;
				case 4: a += "{" + std::to_string(rng_.range(1, 2)) + "}"; break;
				default: return a;
			}
			if (rng_.chance(30)) { a += "?"; }
			return a;
		}
		std::string sequence(int depth) {
			std::string out;
			if (rng_.chance(10)) { out += rng_.chance(50) ? "^" : "\\b"; }
			const int n = static_cast<int>(rng_.range(1, 3));
			for (int i = 0; i < n; ++i) { out += quantified(depth); }
			if (rng_.chance(10)) { out += rng_.chance(50) ? "$" : "\\B"; }
			return out;
		}
		std::string alternation(int depth) {
			std::string out = sequence(depth);
			if (rng_.chance(25)) { out += "|" + sequence(depth); }
			return out;
		}
		std::string pattern() {
			groups_ = 0;
			group_names_.clear();
			return alternation(2);
		}
		std::string flags() {
			static const char* const options[] = {"", "", "", "i", "m", "s", "im"};
			return options[rng_.below(7)];
		}
		std::string literal(const std::string& p) { return "R\"zr(" + p + ")zr\""; }
		std::string make(const std::string& p, const std::string& f) {
			if (rng_.chance(70)) { return f.empty() ? "regex(" + literal(p) + ")" : "regex(" + literal(p) + ", \"" + f + "\")"; }
			return "regex(to_string(" + literal(p) + ")" + (f.empty() ? std::string() : ", to_string(\"" + f + "\")") + ")";
		}
		std::string replacement() {
			std::string out;
			const int n = static_cast<int>(rng_.range(0, 3));
			for (int i = 0; i < n; ++i) {
				switch (rng_.below(6)) {
					case 0: out += "$0"; break;
					case 1: if (groups_ > 0) { out += "$" + std::to_string(rng_.range(1, groups_)); } break;
					case 2: if (!group_names_.empty()) { out += "${" + group_names_[rng_.below(static_cast<uint32_t>(group_names_.size()))] + "}"; } break;
					case 3: out += "$$"; break;
					default: out += std::string(1, "<>x"[rng_.below(3)]); break;
				}
			}
			return out;
		}
		// a pattern text a change may have broken: a dropped or doubled character, a stray metacharacter
		std::string mutated(std::string p) {
			if (p.empty()) { return "("; }
			const size_t at = rng_.below(static_cast<uint32_t>(p.size()));
			switch (rng_.below(3)) {
				case 0: p.erase(at, 1); break;
				case 1: p.insert(at, 1, p[at]); break;
				default: p.insert(at, 1, "()[]{}*+?|\\"[rng_.below(11)]); break;
			}
			return p;
		}

		std::string statement() {
			const std::string t = texts_[rng_.below(static_cast<uint32_t>(texts_.size()))];
			const std::string p = pattern();
			const std::string re = make(p, flags());
			const std::string m = "zrM" + std::to_string(names_++);
			switch (rng_.below(11)) {
				case 0: {
					std::string s = "var " + m + " = " + re + ".search(" + t + "); if (" + m + " == null) { zrout += \"-;\"; } else { zrout += " + m +
					                ".text() + \":\" + to_string(" + m + ".start()) + \",\" + to_string(" + m + ".end()) + \",\" + to_string(" + m + ".size())";
					for (int g = 1; g <= groups_ && g <= 3; ++g) { s += " + \"|\" + to_string(" + m + "[" + std::to_string(g) + "]) + to_string(" + m + ".start(" + std::to_string(g) + "))"; }
					if (!group_names_.empty()) { s += " + \"|\" + to_string(" + m + "[\"" + group_names_[0] + "\"])"; }
					return s + " + \";\"; }";
				}
				case 1: return "zrout += to_string(" + re + ".matches(" + t + ")) + \";\";";
				case 2: return "for (var " + m + " : " + re + ".find_all(" + t + ")) { zrout += \"[\" + " + m + ".text() + to_string(" + m + ".start()) + \"]\"; }";
				case 3: return "zrout += " + re + ".replace(" + t + ", \"" + replacement() + "\") + \";\";";
				case 4: return "zrout += " + re + ".replace_first(" + t + ", \"" + replacement() + "\") + \";\";";
				case 5: return "zrout += to_json(" + re + ".split(" + t + (rng_.chance(40) ? ", " + std::to_string(rng_.range(-1, 3)) : std::string()) + ")) + \";\";";
				case 6: return "zrout += " + re + ".replace(" + t + ", (" + m + ") => \"<\" + " + m + ".text() + \">\") + \";\";";
				case 7: {
					const std::string r = "zrR" + std::to_string(names_++);
					return "regex " + r + " = " + re + "; for (int zri = 0; zri < 3; ++zri) { try { var " + m + " = " + r + ".search(" + t + ", zri); zrout += " + m +
					       " == null ? \"-\" : " + m + ".text(); } catch (zrE) { zrout += zrE; } }";
				}
				case 8: {
					const std::string v = "zrP" + std::to_string(names_++);
					return "var " + v + " = " + literal(mutated(p)) + "; try { zrout += to_string(regex(" + v + ", \"" + flags() + "\").find_all(" + t +
					       ").size()) + \";\"; } catch (zrE) { zrout += zrE + \";\"; }";
				}
				case 9: return "try { zrout += regex(" + literal(p) + ").replace(" + t + ", \"$" + std::to_string(groups_ + 1) + "\"); } catch (zrE) { zrout += zrE + \";\"; }";
				default: return "zrout += to_string(regex(\"^\" + regex::escape(" + t + ") + \"$\").matches(" + t + ")) + \";\";";
			}
		}
	};

} // namespace jai::fuzz
