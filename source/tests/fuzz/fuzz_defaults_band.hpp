#pragma once

// Default-argument band: functions, a method, a static method and a constructor whose defaults call a function that throws on
// some inputs, beside defaults and arguments that construct a class whose constructor and destructor write a trace, and calls
// that pass a value its parameter's type refuses. A counted loop catches each raise and keeps its text and line, and the
// trace joins the result, so a call that binds past a throwing default, runs a callee instruction, or builds or destroys an
// argument the other backend does not shows in the string. Its own RNG stream and name prefix `zd`: the main program's draws
// are unchanged. The result is a string.

#include "fuzz_harness.hpp"

#include <string>
#include <vector>

namespace jai::fuzz {

	class defaults_band {
	public:
		explicit defaults_band(uint64_t seed) : rng_(mix64(seed + 0x9FB21C651E98DF25ULL)) {}

		bool roll() { return rng_.chance(25); }

		std::string emit() {
			const std::string modulus = std::to_string(rng_.range(2, 4));
			const std::string scale = std::to_string(rng_.range(2, 7));
			std::string out = "string zdtrace = \"\";\nint zdcur = 0;\n"
			                  "class zdT { int id = 0; zdT(int i) { id = i; zdtrace += `+${i}`; } ~zdT() { zdtrace += `-${id}`; } }\n"
			                  "int zdboom(int x) { if (x % " + modulus + " == 0) { throw `zdboom ${x}`; } return x * " + scale + "; }\n";
			struct callee {
				std::string declaration;
				std::string call;
			};
			std::vector<callee> callees;
			const int functions = static_cast<int>(rng_.range(2, 4));
			for (int i = 0; i < functions; ++i) {
				const std::string name = "zdf" + std::to_string(i);
				std::string params = "int zda";
				const int defaults = static_cast<int>(rng_.range(1, 3));
				std::string uses = "zda";
				for (int d = 0; d < defaults; ++d) {
					const std::string p = "zdp" + std::to_string(d);
					params += ", " + default_parameter(p, d);
					uses += " + " + (params.rfind("zdT " + p) != std::string::npos ? p + ".id" : p);
				}
				callees.push_back({"int " + name + "(" + params + ") { zdtrace += `" + name + "`; return " + uses + "; }\n", name + "(zdi)"});
			}
			const std::string offset = std::to_string(rng_.range(1, 5));
			callees.push_back({"class zdK { int base = " + offset + "; zdK() {} zdK(int zda, " + default_parameter("zdq", 1) + ") { base = zda; zdtrace += \"K\"; }\n"
			                   "\tint m(int zda, int zdb = zdboom(zdcur + " + offset + ")) { zdtrace += \"m\"; return zda + zdb + base; }\n"
			                   "\tstatic int s(int zda, var zdb = zdboom(zdcur)) { zdtrace += \"s\"; return zda + zdb; } }\n",
			                   "zdk.m(zdi) + zdK::s(zdi) + zdK(zdi).base"});
			callees.push_back({"int zdg(zdT zdt, int zdb = zdboom(zdcur + 1)) { zdtrace += \"g\"; return zdb + zdt.id; }\n", "zdg(zdT(zdi * 10))"});
			callees.push_back({"int zdh(int zda) { zdtrace += \"h\"; return zda; }\n", "zdh(zdbad) + zdf0(zdbad)"});
			std::string calls;
			for (const auto& c : callees) {
				out += c.declaration;
				calls += "\t\ttry { zdout += `${" + c.call + "},`; } catch (zde) { zdout += `!${zde}@${zde.line},`; }\n";
			}
			const std::string rounds = std::to_string(rng_.range(3, 6));
			out += "string zdrun(int zdn) {\n\tstring zdout = \"\";\n\tzdK zdk;\n\tfor (int zdi = 0; zdi < zdn; ++zdi) {\n\t\tzdcur = zdi;\n"
			       "\t\tvar zdbad = zdi % 2 == 0 ? \"x\" : zdi;\n" + calls + "\t}\n\treturn zdout + \"|\" + zdtrace;\n}\n";
			out += "zdrun(" + rounds + ");\n";
			return out;
		}

	private:
		// a parameter with a default that may throw, build a traced argument, or both
		std::string default_parameter(const std::string& name, int at) {
			switch (rng_.below(3)) {
				case 0: return "int " + name + " = zdboom(zdcur + " + std::to_string(at) + ")";
				case 1: return "zdT " + name + " = zdT(zdcur * 10 + " + std::to_string(at) + ")";
				default: return "zdT " + name + " = zdT(zdboom(zdcur + " + std::to_string(at) + "))";
			}
		}

		fuzz_rng rng_;
	};

} // namespace jai::fuzz
