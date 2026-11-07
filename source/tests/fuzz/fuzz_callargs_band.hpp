#pragma once

// Call-argument band: typed program-scope arrays read by index as two to four arguments of one kept call inside a counted loop (the
// block's call door), the elements drawn from DIFFERENT arrays and mixed with the loop counter, a local and literals, so a read that
// resolves another argument's array diverges. The callee keeps a call in its body (no graft). Its own RNG stream and name prefix `zc`:
// the main program's draws are unchanged. The result is an int.

#include "fuzz_harness.hpp"

#include <string>
#include <vector>

namespace jai::fuzz {

	class call_args_band {
	public:
		explicit call_args_band(uint64_t seed) : rng_(mix64(seed + 0x9E6C63D0676A9A99ULL)) {}

		bool roll() { return rng_.chance(25); }

		std::string emit() {
			const int n = static_cast<int>(rng_.range(5, 10));
			const int arrays = static_cast<int>(rng_.range(2, 4));
			std::string out;
			for (int a = 0; a < arrays; ++a) {
				out += "array<int> zcA" + std::to_string(a) + " = [";
				for (int i = 0; i < n; ++i) { if (i) out += ", "; out += std::to_string(rng_.range(-5, 60)); }
				out += "];\n";
			}
			out += "int zcN = 0;\nvoid zcbump() { zcN = zcN + 1; }\n";
			const int calls = static_cast<int>(rng_.range(1, 3));
			std::string body;
			for (int k = 0; k < calls; ++k) {
				const int argc = static_cast<int>(rng_.range(2, 4));
				std::vector<std::string> args;
				std::vector<int> used;
				for (int p = 0; p < argc; ++p) {
					const uint32_t pick = rng_.below(10);
					if (pick < 6 || static_cast<int>(used.size()) < 2) {
						int a = static_cast<int>(rng_.below(static_cast<uint32_t>(arrays)));
						if (used.size() == 1 && used[0] == a) { a = (a + 1) % arrays; }   // at least two different arrays
						used.push_back(a);
						const int off = rng_.chance(70) ? 0 : static_cast<int>(rng_.range(1, 3));   // a computed index opens the argument with its own arithmetic
						const std::string idx = off == 0 ? "zci" : "(zci + " + std::to_string(off) + ") % " + std::to_string(n);
						args.push_back("zcA" + std::to_string(a) + "[" + idx + "]");
					} else if (pick < 8) {
						args.push_back(pick == 6 ? "zci" : "zcl");
					} else {
						args.push_back(std::to_string(rng_.range(0, 9)));
					}
				}
				const std::string f = "zcf" + std::to_string(k);
				out += "int " + f + "(";
				std::string expr;
				for (int p = 0; p < argc; ++p) {
					if (p) { out += ", "; expr += " + "; }
					out += "int zcp" + std::to_string(p);
					expr += "zcp" + std::to_string(p) + " * " + std::to_string(rng_.range(1, 13));
				}
				out += ") { zcbump(); return " + expr + "; }\n";
				std::string call = f + "(";
				for (int p = 0; p < argc; ++p) { if (p) call += ", "; call += args[static_cast<size_t>(p)]; }
				call += ")";
				const std::string weight = std::to_string(rng_.range(1, 7));
				const std::string v = "zcv" + std::to_string(k);
				switch (rng_.below(4)) {   // the door's consumers: a declaration, a store, a temporary, and a temporary under % (the loop stays open)
				case 0: body += "\t\tint " + v + " = " + call + ";\n\t\tzcacc = zcacc + " + v + " * " + weight + ";\n"; break;
				case 1: body += "\t\tzcw = " + call + ";\n\t\tzcacc = zcacc + zcw * " + weight + ";\n"; break;
				case 2: body += "\t\tzcacc = zcacc + " + call + " * " + weight + ";\n"; break;
				default: body += "\t\tzcacc = (zcacc * 31 + " + call + ") % 1000003;\n"; break;
				}
			}
			out += "int zcrun() {\n\tint zcacc = 0;\n\tint zcw = 0;\n\tint zcl = " + std::to_string(rng_.range(0, 9)) + ";\n\tfor (int zci = 0; zci < " + std::to_string(n) + "; ++zci) {\n" + body +
			       "\t\tzcl = zcl + 1;\n\t}\n\treturn zcacc;\n}\n";
			out += "zcrun() * 1000 + zcN % 1000;\n";
			return out;
		}

	private:
		fuzz_rng rng_;
	};

} // namespace jai::fuzz
