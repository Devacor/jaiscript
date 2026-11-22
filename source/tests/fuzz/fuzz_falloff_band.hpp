#pragma once

// Fall-off band: methods, a static method, free functions and lambdas that end without returning a value on some inputs,
// and void methods, each called for its result: directly, in a counted loop, through a callback a builtin calls and through
// a function value. Every such result is null; a backend that yields a method's receiver (or anything else) shows in the
// string. Its own RNG stream and name prefix `zf`: the main program's draws are unchanged. The result is a string.

#include "fuzz_harness.hpp"

#include <string>

namespace jai::fuzz {

	class falloff_band {
	public:
		explicit falloff_band(uint64_t seed) : rng_(mix64(seed + 0xD6E8FEB86659FD93ULL)) {}

		bool roll() { return rng_.chance(25); }

		std::string emit() {
			const std::string cut = std::to_string(rng_.range(1, 3));
			const std::string modulus = std::to_string(rng_.range(2, 4));
			const std::string step = std::to_string(rng_.range(1, 5));
			std::string out = "class zfC { int n = 1;\n"
			                  "\tvar bump(int zfk) { n += zfk; if (zfk > " + cut + ") { return n; } }\n"
			                  "\tvoid quiet(int zfk) { n += zfk * " + step + "; }\n"
			                  "\tvar self_or_not(int zfk) { if (zfk % " + modulus + " == 0) { return this; } }\n"
			                  "\tstatic var half(int zfk) { if (zfk % 2 == 0) { return zfk / 2; } }\n"
			                  "\tvar keep(int zfk) { if (zfk % " + modulus + " == 1) { return true; } }\n}\n"
			                  "var zff(int zfk) { if (zfk % " + modulus + " == 0) { return zfk; } }\n"
			                  "var zfnest(zfC zfc, int zfk) { if (zfk > 0) { return zfc.bump(zfk - 1); } }\n";
			std::string body;
			const std::string lambda_cut = std::to_string(rng_.range(0, 2));
			const std::string lambda = "\tauto zfl = (int zfk) => { if (zfk > " + lambda_cut + ") { return zfk * 3; } };\n";
			const char* uses[] = {
				"var zfa = zfc.bump(zfi);",
				"var zfb = zff(zfi);",
				"var zfx = zfl(zfi);",
				"var zfq = zfc.quiet(zfi);",
				"var zfs = zfc.self_or_not(zfi);",
				"var zfh = zfC::half(zfi);",
				"var zfm = [zfi, zfi + 1, zfi + 2, zfi + 3];\n\t\tzfm.filter((int zfv) => zfc.keep(zfv));",
				"function<var(int)> zffn = zff;\n\t\tvar zfv = zffn(zfi + 1);",
				"var zfn = zfnest(zfc, zfi);",
			};
			std::string shown;
			for (const char* use : uses) {
				if (!rng_.chance(70)) { continue; }
				body += std::string("\t\t") + use + "\n";
				const std::string text(use);
				const size_t name_end = text.rfind(" = ");
				const size_t name_start = text.rfind("var ", name_end) + 4;
				shown += "${" + text.substr(name_start, name_end - name_start) + "}|";
			}
			const std::string rounds = std::to_string(rng_.range(3, 6));
			out += "string zfrun(int zfcount) {\n\tstring zfout = \"\";\n\tzfC zfc;\n" + lambda + "\tfor (int zfi = 0; zfi < zfcount; ++zfi) {\n" + body +
			       "\t\tzfout += `" + shown + ",`;\n\t}\n\treturn zfout + `${zfc.n}`;\n}\n";
			out += "zfrun(" + rounds + ");\n";
			return out;
		}

	private:
		fuzz_rng rng_;
	};

} // namespace jai::fuzz
