#pragma once

// Const-guard band: a write the run time decides (through a `const var` holding an instance or an array: a field store, a compound, an
// increment, a push, an element store, a method that writes its receiver) sits in a callee small enough to graft, under a condition some
// inputs take and some do not, and is reached through zero to two more small callees: functions, a method, a static method. A caller's
// counted loop catches each raise and keeps its text and line, and the const's state joins the result, so a guard a graft drops shows as a
// missing error and as a written const. Its own RNG stream and name prefix `zk`: the main program's draws are unchanged. The result is
// a string.

#include "fuzz_harness.hpp"

#include <string>

namespace jai::fuzz {

	class const_guard_band {
	public:
		explicit const_guard_band(uint64_t seed) : rng_(mix64(seed + 0x9E6C63D0876A9A47ULL)) {}

		bool roll() { return rng_.chance(25); }

		std::string emit() {
			std::string out = "class zkBox { var items = [1, 2]; int n = 0; void set(int zkx) { n = zkx; } int get() { return n; } }\n"
			                  "const var zkcv = zkBox();\nconst var zkarr = [1, 2, 3];\nint zkfree = 0;\n";
			const int leaves = static_cast<int>(rng_.range(2, 4));
			std::string calls;
			for (int i = 0; i < leaves; ++i) {
				const std::string leaf = "zkw" + std::to_string(i);
				const int modulus = static_cast<int>(rng_.range(2, 4));
				const bool returns = rng_.chance(60);
				out += std::string(returns ? "int " : "void ") + leaf + "(int zka) {\n\tzkfree += zka;\n\tif (zka % " + std::to_string(modulus) + " == " +
				       std::to_string(rng_.below(static_cast<uint32_t>(modulus))) + ") { " + write() + " }\n" + (returns ? "\treturn zka * " + std::to_string(rng_.range(2, 9)) + " + zkfree;\n" : "") + "}\n";
				std::string callee = leaf;
				const int depth = static_cast<int>(rng_.below(3));
				for (int d = 0; d < depth; ++d) {
					const std::string name = "zkm" + std::to_string(i) + "_" + std::to_string(d);
					const std::string inner = callee + "(zka" + (rng_.chance(50) ? "" : " + " + std::to_string(rng_.range(1, 3))) + ")";
					const unsigned kind = rng_.below(3);
					if (kind == 0) {
						out += std::string(returns ? "int " : "void ") + name + "(int zka) { " + (returns ? "return " + inner + " + 1;" : inner + ";") + " }\n";
						callee = name;
					} else {
						const std::string cls = "zkH" + std::to_string(i) + "_" + std::to_string(d);
						out += "class " + cls + " { " + (kind == 2 ? "static " : "") + (returns ? "int " : "void ") + "go(int zka) { " + (returns ? "return " + inner + " + 1;" : inner + ";") + " } }\n";
						out += std::string(returns ? "int " : "void ") + name + "(int zka) { " + (kind == 2 ? "" : cls + " zkh; ") + (returns ? "return " : "") +
						       (kind == 2 ? cls + "::go" : "zkh.go") + "(zka); }\n";
						callee = name;
					}
				}
				const std::string call = callee + "(zki)";
				calls += "\t\ttry { " + (returns ? "zkout += `${" + (rng_.chance(50) ? call : call + " + zki") + "},`;" : call + "; zkout += \"v,\";") +
				         " } catch (zke) { zkout += `!${zke}@${zke.line},`; }\n";
			}
			out += "string zkrun(int zkn) {\n\tstring zkout = \"\";\n\tfor (int zki = 0; zki < zkn; ++zki) {\n" + calls + "\t}\n\treturn zkout;\n}\n";
			out += "zkrun(" + std::to_string(rng_.range(3, 6)) + ") + `|${zkcv.n} ${zkcv.items} ${zkarr} ${zkfree}`;\n";
			return out;
		}

	private:
		std::string value() { return rng_.chance(50) ? "zka" : std::to_string(rng_.range(1, 9)); }

		std::string write() {
			switch (rng_.below(7)) {
				case 0: return "zkcv.n = " + value() + ";";
				case 1: return "++zkcv.n;";
				case 2: return "zkcv.n += " + value() + ";";
				case 3: return "zkcv.items.push(" + value() + ");";
				case 4: return "zkarr[" + std::to_string(rng_.below(3)) + "] = " + value() + ";";
				case 5: return "zkcv.set(" + value() + ");";
				default: return "zkfree += zkcv.get();";   // a read: no guard raises, and the callee grafts
			}
		}

		fuzz_rng rng_;
	};

} // namespace jai::fuzz
