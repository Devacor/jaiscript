#pragma once

// Less-than band: `name < ... > (` in the shapes a comparison chain and a call with type arguments share,
// over heads of every kind: a local, a parameter, a field declared above or below its method, an inherited field, a static, a namespace's
// variable, a program-scope variable declared below the function that reads it, beside calls of a generic function, a generic class and a
// generic method that take class-type arguments and are declared above or below their use. A site read the wrong way fails the load or
// changes the count. Its own RNG stream and name prefix `zl`: the main program's draws are unchanged. The result is an int.

#include "fuzz_harness.hpp"

#include <string>
#include <vector>

namespace jai::fuzz {

	class less_than_band {
	public:
		explicit less_than_band(uint64_t seed) : rng_(mix64(seed + 0x3C3E28293C3E2829ULL)) {}

		bool roll() { return rng_.chance(25); }

		std::string emit() {
			const std::string generic_function = "T zlpick<T>(T zlx) { return zlx; }\n";
			const std::string generic_class = "class zlBox<T> { T zlheld; static int zlcount(int zln) { return zln + 1; } bool zlin() { return " +
			                                  pair("zllow", "zlhigh", "zlmid") + "; } int zllow = 1; int zlmid = 3; int zlhigh = 6; }\n";
			const bool generics_first = rng_.chance(50);
			std::string out = "class zlPet { int zlv = " + std::to_string(rng_.range(2, 9)) + "; }\nint zlboth(bool zla, bool zlb) { return zla && zlb ? 1 : 0; }\n";
			if (generics_first) { out += generic_function + generic_class; }
			const bool fields_first = rng_.chance(50);
			const std::string fields = "int zlfa = " + std::to_string(rng_.range(1, 3)) + "; int zlfb = " + std::to_string(rng_.range(4, 6)) + "; int zlfc = " +
			                           std::to_string(rng_.range(7, 9)) + "; static int zlsa = 1; static int zlsb = 2; static int zlsc = 3; ";
			out += "class zlBase { int zlia = 2; int zlib = 5; int zlic = 8; }\n";
			out += "class zlHolder : zlBase { " + (fields_first ? fields : std::string());
			out += "int zlown() { return " + site("zlfa", "zlfb", "zlfc") + "; } ";
			out += "int zlinherited() { return " + site("zlia", "zlib", "zlic") + "; } ";
			out += "int zlstatics() { return " + site("zlsa", "zlsb", "zlsc") + "; } ";
			out += "int zllambda() { auto zlg = [this]() { return " + site("zlfa", "zlfb", "zlfc") + "; }; return zlg(); } ";
			out += "int zlmade() { return zlconv<zlPet, int>(zlPet(), zlfa) + this.zlconv<zlPet, int>(zlPet(), zlfb); } ";
			out += "T2 zlconv<T, T2>(T zlx, T2 zly) { return zly; } ";
			out += (fields_first ? std::string() : fields) + "}\n";
			out += "namespace zlspace { int zlna = 1; int zlnb = 4; int zlnc = 7; int zlnear() { return " + site("zlna", "zlnb", "zlnc") + "; } }\n";
			out += "int zlglobal() { return " + site("zlga", "zlgb", "zlgc") + " + " + site("zlspace::zlna", "zlspace::zlnb", "zlspace::zlnc") + " + " +
			       site("zlHolder::zlsa", "zlHolder::zlsb", "zlHolder::zlsc") + "; }\n";
			out += "int zllocals(int zlpa, int zlpb) { int zlpc = zlpb + " + std::to_string(rng_.range(1, 4)) + "; zlHolder zlh; return " + site("zlpa", "zlpb", "zlpc") + " + " +
			       site("zlh.zlfa", "zlpb", "zlpc") + "; }\n";
			out += "int zlcalls() { zlBox<zlPet> zlb = zlBox<zlPet>{}; zlHolder zlh; return zlpick<zlPet>(zlPet()).zlv + zlBox<zlPet>::zlcount(" + std::to_string(rng_.range(1, 9)) +
			       ") + zlboth(zlb.zlin(), zlpick<zlPet>(zlb.zlheld).zlv > (0)) + zlh.zlconv<zlPet, int>(zlPet(), " + std::to_string(rng_.range(1, 9)) + ") + zlh.zlmade(); }\n";
			out += "int zlga = 1; int zlgb = " + std::to_string(rng_.range(3, 5)) + "; int zlgc = " + std::to_string(rng_.range(6, 9)) + ";\n";
			if (!generics_first) { out += generic_function + generic_class; }
			out += "zlHolder zlone;\nint zlout = zlone.zlown() + zlone.zlinherited() * 3 + zlone.zlstatics() * 5 + zlone.zllambda() * 7 + zlspace::zlnear() * 11 + zlglobal() * 13 + "
			       "zllocals(" + std::to_string(rng_.range(0, 3)) + ", " + std::to_string(rng_.range(2, 6)) + ") * 17 + zlcalls() * 19;\nzlout;\n";
			return out;
		}

	private:
		// two comparisons a call with type arguments would spell as one argument: a < b, c > (a)
		std::string pair(const std::string& a, const std::string& b, const std::string& c) {
			switch (rng_.below(3)) {
				case 0: return "zlboth(" + a + " < " + b + ", " + c + " > (" + a + ")) == 1";
				case 1: return "zlboth(" + b + " < " + c + ", " + b + " > (" + a + " + 1)) == 1";
				default: return "zlboth(" + a + " < " + c + ", " + b + " > (" + c + ")) == 0";
			}
		}
		// an int from one of the shapes
		std::string site(const std::string& a, const std::string& b, const std::string& c) {
			switch (rng_.below(5)) {
				case 0: return "zlboth(" + a + " < " + b + ", " + c + " > (" + a + "))";
				case 1: return "zlboth(" + c + " < " + b + ", " + c + " > (" + a + "))";
				case 2: return "(" + a + " < " + b + " > (" + a + " > " + c + ") ? 1 : 0)";
				case 3: return "([" + a + " < " + c + ", " + b + " > (" + c + ")][" + std::to_string(rng_.below(2)) + "] ? 1 : 0)";
				default: return "zlboth(" + a + " < " + b + ", " + b + " > (" + a + ")) + zlboth(" + b + " < " + a + ", " + c + " > (" + b + "))";
			}
		}

		fuzz_rng rng_;
	};

} // namespace jai::fuzz
