#pragma once

// Global-store band: a function's counted loop stores a program-scope scalar into a program-scope array through a computed index (the
// counter plus a parameter or a global, either side, or two globals), plainly or compounded, in an arm the earlier calls never take, so
// the arm runs on the general form after a block entry resolved the loop's environment values. Its own RNG stream and name prefix `zs`:
// the main program's draws are unchanged. The result is an int.

#include "fuzz_harness.hpp"

#include <string>

namespace jai::fuzz {

	class global_store_band {
	public:
		explicit global_store_band(uint64_t seed) : rng_(mix64(seed + 0xC2B2AE3D27D4EB4FULL)) {}

		bool roll() { return rng_.chance(25); }

		std::string emit() {
			const int m = static_cast<int>(rng_.range(3, 6));
			const int n = m + 4;
			std::string out = "array<int> zsA = [";
			for (int i = 0; i < n; ++i) { if (i) out += ", "; out += std::to_string(rng_.range(0, 9)); }
			out += "];\nint zsW = " + std::to_string(rng_.range(10, 99)) + ";\nint zsK = " + std::to_string(rng_.range(0, 3)) + ";\n";
			static constexpr const char* const indexes[] = { "zsx + zss", "zss + zsx", "zsx + zsK", "zsK + zsx", "zsK + zsK" };
			const std::string idx = indexes[rng_.below(5)];
			const std::string op = rng_.chance(50) ? " = " : " += ";
			const std::string other = rng_.chance(50) ? std::to_string(rng_.range(0, 9)) : "zsx";
			out += "int zsf(int zss, int zsmode) {\n\tint zsacc = 0;\n\tfor (int zsx = 0; zsx < " + std::to_string(m) + "; ++zsx) {\n";
			out += "\t\tif (zsmode == 1) { zsA[" + idx + "]" + op + "zsW; } else { zsA[" + idx + "]" + op + other + "; }\n";
			out += "\t\tzsacc = (zsacc * 7 + zsA[" + idx + "]) % 100003;\n\t}\n\treturn zsacc;\n}\n";
			std::string calls;
			const int rounds = static_cast<int>(rng_.range(3, 6));
			for (int r = 0; r < rounds; ++r) { calls += "zsout = (zsout * 31 + zsf(" + std::to_string(rng_.range(0, 3)) + ", " + std::to_string(r == 0 ? 0 : static_cast<int>(rng_.below(2))) + ")) % 1000003;\n"; }
			out += "int zsout = 0;\n" + calls + "for (int zsi = 0; zsi < " + std::to_string(n) + "; ++zsi) { zsout = (zsout * 13 + zsA[zsi]) % 1000003; }\nzsout;\n";
			return out;
		}

	private:
		fuzz_rng rng_;
	};

} // namespace jai::fuzz
