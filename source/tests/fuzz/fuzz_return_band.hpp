#pragma once

// Early-return band: a callee small enough to graft that returns from two to four branches before its last return (an arm may call a
// second small function), returned whole by its callers: one kept off frame programs by a var parameter (its body straight-line regions),
// one that can be a frame program, one returning it from inside a counted loop. Every branch of the callee is taken over the inputs, so a
// graft's early return that fails to leave its caller's region diverges. Its own RNG stream and name prefix `zr`: the main program's draws
// are unchanged. The result is an int.

#include "fuzz_harness.hpp"

#include <string>

namespace jai::fuzz {

	class early_return_band {
	public:
		explicit early_return_band(uint64_t seed) : rng_(mix64(seed + 0x5BD1E9955BD1E995ULL)) {}

		bool roll() { return rng_.chance(25); }

		std::string emit() {
			std::string out;
			const bool helper = rng_.chance(50);
			if (helper) { out += "int zrh(int zrx, int zry) { return zrx * " + std::to_string(rng_.range(2, 9)) + " - zry % " + std::to_string(rng_.range(3, 11)) + "; }\n"; }
			const int arms = static_cast<int>(rng_.range(2, 4));
			const int modulus = arms + 1;
			out += "int zrk(int zrid, int zra, int zrb, int zrc) {\n\tint zrm = zrid % " + std::to_string(modulus) + ";\n";
			for (int j = 0; j < arms; ++j) { out += "\tif (zrm == " + std::to_string(j) + ") { return " + arm(helper) + "; }\n"; }
			out += "\treturn " + arm(helper) + ";\n}\n";
			std::string prefix;   // a straight span's worth of work ahead of the branch
			const int locals = static_cast<int>(rng_.range(6, 9));
			for (int i = 0; i < locals; ++i) {
				const std::string prev = i == 0 ? "zru" : "zrv" + std::to_string(i - 1);
				prefix += "\tint zrv" + std::to_string(i) + " = (" + prev + " * " + std::to_string(rng_.range(2, 7)) + " + " + std::to_string(rng_.range(0, 9)) + ") % " +
				          std::to_string(rng_.range(50, 997)) + ";\n";
			}
			const std::string id = "zrv" + std::to_string(locals - 1) + " + zru";
			const std::string branch = "\tif (zrt == 1) { return zrk(" + id + ", zrv0, zrv1, zrv2); }\n\tif (zrt == 2) { return zrv0 - zrv1; }\n\treturn " +
			                           std::to_string(rng_.range(7000, 7999)) + ";\n";
			out += "int zrs(int zrt, int zru, var zrtag) {\n" + prefix + branch + "}\n";
			out += "int zrf(int zrt, int zru) {\n" + prefix + branch + "}\n";
			out += "int zrl(int zrn) {\n\tint zracc = 0;\n\tfor (int zri = 0; zri < zrn; ++zri) {\n\t\tzracc = (zracc * 3 + zri) % 1009;\n\t\tif (zracc > " +
			       std::to_string(rng_.range(10, 60)) + ") { return zrk(zracc, zri, zrn, zracc); }\n\t}\n\treturn zracc;\n}\n";
			const int inputs = static_cast<int>(rng_.range(6, 12));
			out += "int zrout = 0;\nfor (int zrj = 0; zrj < " + std::to_string(inputs) + "; ++zrj) { zrout = ((zrout * 31 + zrs(1, zrj, 0)) * 37 + zrf(1, zrj)) % 1000003 * 41 + zrl(zrj + 2); zrout = zrout % 1000003; }\n";
			out += "zrout;\n";
			return out;
		}

	private:
		std::string arm(bool helper) {
			const std::string operand = std::string("zr") + "abc"[rng_.below(3)];
			const std::string base = operand + " * " + std::to_string(rng_.range(2, 13)) + " + zrid";
			return helper && rng_.chance(50) ? "zrh(" + base + ", zrid)" : base;
		}

		fuzz_rng rng_;
	};

} // namespace jai::fuzz
