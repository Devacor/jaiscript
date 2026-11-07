#pragma once

// Block shapes: loops the vm's region compiler closes into cell blocks, drawn as sub-rolls of the generator's other parts (a bug
// that fires only inside a block is invisible to a part whose loops never close). The main program's statements run inside a
// function (program-scope loops are never translated), counted loops over typed locals, typed local and program-scope arrays read
// and written at plain [i], typed accumulators, branches, math intrinsics, nested counted loops and calls to grafted and kept
// helpers land among its statements, and the math band gains a function whose math locals the vm lowers to scalars. Its own RNG
// stream and name prefix `zb`: the parts' own draws and text are unchanged when its roll is off, and generate_program's default
// (never) keeps the programs of the gates that replay seeds.

#include "fuzz_harness.hpp"

#include <string>
#include <vector>

namespace jai::fuzz {

	class block_band {
	public:
		explicit block_band(uint64_t seed) : rng_(mix64(seed + 0x6A09E667F3BCC909ULL)) {}

		bool roll() { return rng_.chance(50); }
		bool loop_here() { return rng_.chance(15); }

		// program-scope declarations the loops read (typed arrays, a call counter, the helpers); they go ahead of everything else
		const std::vector<std::string>& prelude() const { return prelude_; }
		int loops() const { return loops_; }

		// a closable loop as statements of a function body; outer: typed int locals in scope it may read
		std::vector<std::string> loop(const std::vector<std::string>& outer) {
			const std::string id = std::to_string(loops_++);
			const int len = static_cast<int>(rng_.range(4, 9));
			std::vector<std::string> out;
			arrays_.clear();
			const int locals = static_cast<int>(rng_.range(1, 2));
			for (int a = 0; a < locals; ++a) {
				const std::string name = "zbA" + id + "_" + std::to_string(a);
				out.push_back("array<int> " + name + " = " + int_list(len) + ";");
				arrays_.push_back(name);
			}
			if (rng_.chance(45)) arrays_.push_back(global_int_array());
			floats_.clear();
			if (rng_.chance(40)) {
				const std::string name = "zbF" + id;
				out.push_back("array<float> " + name + " = " + float_list(len) + ";");
				floats_.push_back(name);
				if (rng_.chance(30)) floats_.push_back(global_float_array());
			}
			acc_ = "zbs" + id;
			facc_ = floats_.empty() ? std::string() : "zbf" + id;
			iv_ = "zbi" + id;
			out.push_back("int " + acc_ + " = " + std::to_string(rng_.range(-3, 9)) + ";");
			if (!facc_.empty()) out.push_back("float " + facc_ + " = " + float_lit() + ";");
			outer_ = outer;
			len_ = len;
			const uint32_t head = rng_.below(10);
			const bool cfor = head < 7;
			if (cfor) {
				const char* step = rng_.chance(50) ? "++" : "";
				out.push_back("for (int " + iv_ + " = 0; " + iv_ + " < " + std::to_string(len) + "; " + (step[0] ? "++" + iv_ : rng_.chance(60) ? iv_ + "++" : iv_ + " += 1") + ") {");
			} else if (head < 9) {
				out.push_back("int " + iv_ + " = 0;");
				out.push_back("while (" + iv_ + " < " + std::to_string(len) + ") {");
			} else {
				out.push_back("for (int " + iv_ + " = 0; " + iv_ + " < " + arrays_[0] + ".size(); " + iv_ + "++) {");   // a size head (a refused head form today)
			}
			const int stmts = static_cast<int>(rng_.range(2, 5));
			for (int k = 0; k < stmts; ++k) out.push_back("\t" + statement(cfor, 0));
			if (cfor && rng_.chance(8)) out.push_back(std::string("\tif (") + iv_ + " == " + std::to_string(rng_.range(1, len - 1)) + ") { " + (rng_.chance(50) ? "continue" : "break") + "; }");
			if (!cfor) out.push_back("\t" + iv_ + " = " + iv_ + " + 1;");
			out.push_back("}");
			std::string shown = "print(" + acc_;
			if (!facc_.empty()) shown += ", " + facc_;
			shown += ", " + arrays_[rng_.below(static_cast<uint32_t>(arrays_.size()))] + "[" + std::to_string(rng_.range(0, len - 1)) + "]);";
			out.push_back(shown);
			return out;
		}

		// a function whose math locals the vm lowers to scalars, updated in a counted loop; returns a float (an int for ivec)
		std::string math_function(const std::string& name) {
			const int k = static_cast<int>(rng_.below(4));   // 0 vec2, 1 vec3, 2 ivec2, 3 ivec3
			const bool ints = k >= 2;
			const int dims = (k == 1 || k == 3) ? 3 : 2;
			const std::string ty = k == 0 ? "math::vec2" : k == 1 ? "math::vec3" : k == 2 ? "math::ivec2" : "math::ivec3";
			const char* comps[] = {"x", "y", "z"};
			auto component = [&]() { return std::string(ints ? std::to_string(rng_.range(-4, 9)) : float_lit()); };
			auto ctor = [&]() { std::string s = ty + "("; for (int d = 0; d < dims; ++d) { if (d) s += ", "; s += component(); } return s + ")"; };
			const int len = static_cast<int>(rng_.range(3, 8));
			std::string out = "function " + name + "() -> " + (ints ? "int" : "float") + " {\n";
			out += "\t" + ty + " zbp = " + ctor() + ";\n\t" + ty + " zbq = " + ctor() + ";\n";
			out += std::string("\tarray<") + (ints ? "int" : "float") + "> zbE = " + (ints ? int_list(len) : float_list(len)) + ";\n";
			out += std::string("\t") + (ints ? "int zbacc = 0;" : "float zbacc = 0.0;") + "\n";
			out += "\tfor (int zbi = 0; zbi < " + std::to_string(len) + "; zbi++) {\n";
			const int stmts = static_cast<int>(rng_.range(2, 5));
			for (int s = 0; s < stmts; ++s) {
				const std::string c = comps[rng_.below(static_cast<uint32_t>(dims))];
				switch (rng_.below(ints ? 6 : 9)) {
					case 0: out += "\t\tzbp." + c + " += zbE[zbi];\n"; break;
					case 1: out += "\t\tzbp = zbp + zbq;\n"; break;
					case 2: out += "\t\tzbq." + c + " = zbq." + c + " - zbE[zbi]" + (ints ? " % 5" : " * 0.25") + ";\n"; break;
					case 3: out += "\t\tzbacc = zbacc + zbp." + c + " * " + (ints ? "2" : "0.5") + ";\n"; break;
					case 4: out += "\t\tif (zbp." + c + (ints ? " > 20) { zbp." + c + " = zbp." + c + " - 17; }" : " > 3.0) { zbp." + c + " = zbp." + c + " - 2.5; }") + "\n"; break;
					case 5: out += "\t\tzbacc = zbacc + zbq." + c + " - zbp." + c + ";\n"; break;
					case 6: out += "\t\tzbacc = zbacc + math::dot(zbp, zbq);\n"; break;
					case 7: out += "\t\tzbacc = zbacc + math::length(zbq);\n"; break;
					default: out += "\t\tzbp = zbp * 0.5;\n"; break;
				}
			}
			out += "\t}\n\treturn zbacc";
			for (int d = 0; d < dims; ++d) out += std::string(" + zbp.") + comps[d];
			return out + ";\n}\n";
		}

	private:
		fuzz_rng rng_;
		int loops_ = 0, globals_ = 0, helpers_ = 0;
		bool counter_ = false;
		std::vector<std::string> prelude_, arrays_, floats_, outer_;
		std::string acc_, facc_, iv_;
		int len_ = 0;

		static constexpr int k_global_len = 12;   // every loop's trip count is at most 9: [i] stays in range

		std::string float_lit() { static constexpr const char* const f[] = {"0.5", "1.25", "-0.75", "2.0", "3.5", "0.125", "-2.25"}; return f[rng_.below(7)]; }
		std::string int_list(int n) {
			std::string s = "[";
			for (int i = 0; i < n; ++i) { if (i) s += ", "; s += std::to_string(rng_.range(-5, 60)); }
			return s + "]";
		}
		std::string float_list(int n) {
			std::string s = "[";
			for (int i = 0; i < n; ++i) { if (i) s += ", "; s += float_lit(); }
			return s + "]";
		}
		std::string global_int_array() {
			const std::string name = "zbG" + std::to_string(globals_++);
			prelude_.push_back("array<int> " + name + " = " + int_list(k_global_len) + ";");
			return name;
		}
		std::string global_float_array() {
			const std::string name = "zbH" + std::to_string(globals_++);
			prelude_.push_back("array<float> " + name + " = " + float_list(k_global_len) + ";");
			return name;
		}
		std::string element(const std::vector<std::string>& from) { return from[rng_.below(static_cast<uint32_t>(from.size()))] + "[" + iv_ + "]"; }
		std::string helper(int argc) {
			const std::string name = "zbh" + std::to_string(helpers_++);
			std::string sig = "int " + name + "(", expr;
			for (int p = 0; p < argc; ++p) {
				if (p) { sig += ", "; expr += " + "; }
				sig += "int zbp" + std::to_string(p);
				expr += "zbp" + std::to_string(p) + " * " + std::to_string(rng_.range(1, 7));
			}
			if (rng_.chance(50)) {   // a kept call: the body calls
				if (!counter_) { prelude_.push_back("int zbN = 0;"); prelude_.push_back("void zbcount() { zbN = zbN + 1; }"); counter_ = true; }
				prelude_.push_back(sig + ") { zbcount(); return (" + expr + ") % 9973; }");
			} else {
				prelude_.push_back(sig + ") { return (" + expr + ") % 9973; }");
			}
			return name;
		}
		std::string bounded(const std::string& e) { return "(" + e + ") % 100003"; }
		std::string statement(bool cfor, int depth) {
			const std::string& a = acc_;
			switch (rng_.below(depth == 0 && cfor ? 13 : 12)) {
				case 0: return a + " = " + bounded(a + " + " + element(arrays_) + " * " + std::to_string(rng_.range(1, 13))) + ";";
				case 1: return a + " += " + element(arrays_) + ";";
				case 2: return element(arrays_) + " = (" + a + " + " + iv_ + ") % 101;";
				case 3: return element(arrays_) + " += " + std::to_string(rng_.range(1, 4)) + ";";
				case 4: return "if (" + a + " > " + std::to_string(rng_.range(10, 90)) + ") { " + a + " = " + a + " - " + std::to_string(rng_.range(5, 40)) + "; } else { " + a + " = " + a + " + 1; }";
				case 5: return a + " = math::" + (rng_.chance(50) ? "max(" : "min(") + a + ", " + element(arrays_) + ");";
				case 6: return a + " = " + a + " + math::abs(" + element(arrays_) + " - " + std::to_string(rng_.range(0, 30)) + ");";
				case 7: {
					const int argc = static_cast<int>(rng_.range(2, 3));
					std::string call = helper(argc) + "(";
					for (int p = 0; p < argc; ++p) { if (p) call += ", "; call += rng_.chance(75) ? element(arrays_) : rng_.chance(50) ? iv_ : a; }
					return a + " = " + bounded(a + " + " + call + ")") + ";";
				}
				case 8: {
					if (outer_.empty()) return a + " -= " + element(arrays_) + " % 7;";
					return a + " = " + bounded(a + " + " + outer_[rng_.below(static_cast<uint32_t>(outer_.size()))] + " % 97") + ";";
				}
				case 9: {
					if (facc_.empty()) return a + " = " + a + " / " + std::to_string(rng_.range(2, 5)) + " + " + element(arrays_) + ";";
					return facc_ + " = " + facc_ + " * 0.5 + " + element(floats_) + ";";
				}
				case 10: {
					if (facc_.empty()) return a + " = " + bounded(a + " * 3 + " + element(arrays_)) + ";";
					return "if (" + element(floats_) + " > " + float_lit() + ") { " + facc_ + " += 1.5; }";
				}
				case 11: {
					const std::string t = "zbt" + std::to_string(rng_.below(1000000));
					return "int " + t + " = " + element(arrays_) + " + " + iv_ + "; " + a + " = " + bounded(a + " + " + t) + ";";
				}
				default: {   // a nested counted loop over the same arrays
					const std::string outer_iv = iv_;
					const std::string j = "zbj" + std::to_string(rng_.below(1000000));
					const int m = static_cast<int>(rng_.range(2, len_));
					std::string s = "for (int " + j + " = 0; " + j + " < " + std::to_string(m) + "; " + j + "++) { ";
					iv_ = j;
					s += statement(true, 1) + " ";
					if (rng_.chance(50)) s += statement(true, 1) + " ";
					iv_ = outer_iv;
					return s + "}";
				}
			}
		}
	};

} // namespace jai::fuzz
