#pragma once

// Generics band: generic classes, functions and methods, used through deduction, bounds and written type arguments, with
// instances as fields, as elements of an array and a map, and nested in one another; half the seeds declare two of the
// generics in a unit the band's segment imports. Some seeds add a segment that fails the load on its own (a deduction that
// cannot settle, a bound an argument does not meet, too many type arguments, an instance whose body does not check), whose
// error text the backends must agree on. Its own RNG stream and name prefix `zg`: the main program's draws are unchanged.
// The result is a string.

#include "fuzz_harness.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace jai::fuzz {

	class generics_band {
	public:
		explicit generics_band(uint64_t seed) : rng_(mix64(seed + 0x2545F4914F6CDD1DULL)), seed_(seed) {}

		bool roll() { return rng_.chance(25); }

		// The band's segments, the main one first, and the unit it imports (empty path: none)
		struct emitted {
			std::vector<std::string> segments;
			std::vector<std::string> labels;
			std::string import_path;
			std::string import_text;
		};

		emitted emit() {
			emitted out;
			const int k = static_cast<int>(rng_.range(2, 9));
			const std::string box = "class zgBox<T> { T v; int hits = 0; static int made = 0; zgBox() { made += 1; } zgBox(T a) { v = a; made += 1; } "
			                        "T get() { hits += 1; return v; } U conv<U>(U seed) { return seed; } static T zero() { return T(); } }\n";
			const std::string pick = "T zgpick<T>(T a, T b, bool f) { return f ? a : b; }\n";
			std::string text;
			if (rng_.chance(50)) {
				out.import_path = (std::filesystem::temp_directory_path() / "jaiscript_fuzz" / ("zg" + std::to_string(seed_)) / "zglib.jai").generic_string();
				out.import_text = box + pick;
				text += "import \"" + out.import_path + "\";\n";
			} else {
				text += box + pick;
			}
			const std::string id = std::to_string(rng_.range(1, 9));   // one draw per statement: every compiler draws in the code's order
			const std::string extra = std::to_string(rng_.range(1, 9));
			text += "class zgBase { int id = " + id + "; int tag() { return id * " + std::to_string(k) + "; } }\n"
			        "class zgKid : zgBase { int extra = " + extra + "; int more() { return extra + id; } }\n"
			        "class zgPair<K, V> { K first; V second; }\n"
			        "class zgPool<T : zgBase> { array<T> xs; int total() { int s = 0; for (auto x : xs) { s += x.tag(); } return s; } void add(T x) { xs.push(x); } }\n"
			        "array<T> zgrep<T>(T x, int n) { array<T> out; for (int i = 0; i < n; ++i) { out.push(x); } return out; }\n"
			        "U zgconv<U, T>(T x) { return cast<U>(x); }\n"
			        "int zgsum<T : zgBase>(array<T> xs) { int s = 0; for (auto x : xs) { s += x.tag(); } return s; }\n"
			        "class zgHolder { zgBox<int> a; array<zgBox<string>> list; map<string, zgBox<int>> byname; zgBox<zgBox<int>> nested; }\n";
			std::vector<std::string> uses = {
				"zgout += `${zgpick(zgn, " + std::to_string(k) + ", zgn % " + std::to_string(rng_.range(2, 4)) + " == 0)},`;",
				"zgout += `${zgpick<string>(\"a\", \"b\", zgn > " + std::to_string(rng_.range(0, 3)) + ")},`;",
				"zgout += `${zgrep(zgn, " + std::to_string(rng_.range(0, 3)) + ")}${zgrep<string>(\"r\", 2)},`;",
				"zgout += `${zgconv<float>(zgn)}${zgconv<int>(zgn * 1.5)},`;",
				"zgHolder zgh;\n\tzgh.a.v = zgn;\n\tzgh.list.push(zgBox<string>(`s${zgn}`));\n\tzgh.byname[\"k\"] = zgBox<int>(zgn * " + std::to_string(k) +
					");\n\tzgh.nested.v.v = zgn + 1;\n\tzgout += `${zgh.a.get()}${zgh.list[0].get()}${zgh.byname[\"k\"].get()}${zgh.nested.get().get()}${zgh.a.hits},`;",
				"zgPool<zgKid> zgp;\n\tzgp.add(zgKid());\n\tzgp.add(zgKid());\n\tzgout += `${zgp.total()}${zgp.xs[0].more()},`;",
				"zgout += `${zgsum([zgBase(), zgBase()])}${zgsum([zgKid()])},`;",
				"zgout += `${zgb.conv<int>(zgn)}${zgb.conv(\"q\")}${zgBox<string>::zero().size()}${zgBox<int>::zero()},`;",
				"zgPair<string, zgBox<int>> zgq;\n\tzgq.second.v = zgn;\n\tzgq.first = `f${zgn}`;\n\tzgout += `${zgq.second.get()}${zgq.first},`;",
				"array<zgBox<int>> zgarr = [zgBox<int>(" + std::to_string(k) + "), zgBox<int>(zgn)];\n\tzgout += `${zgarr[1].get() + zgarr[0].get()}${zgarr.size()},`;",
				"zgBox<int> zgcopy = zgb;\n\tzgcopy.v += 1;\n\tzgout += `${zgb.v}${zgcopy.v}${zgBox<int>::made > 0},`;",
			};
			std::string body;
			for (const auto& use : uses) {
				if (rng_.chance(70)) { body += "\t" + use + "\n"; }
			}
			text += "string zgrun(int zgn) {\n\tstring zgout = \"\";\n\tzgBox<int> zgb = zgBox<int>(zgn);\n" + body + "\treturn zgout;\n}\n";
			const std::string rounds = std::to_string(rng_.range(2, 5));
			text += "string zgall = \"\";\nfor (int zgi = 0; zgi < " + rounds + "; ++zgi) { zgall += zgrun(zgi); }\nzgall;\n";
			out.segments.push_back(std::move(text));
			out.labels.push_back("generics");
			const std::vector<std::string> failing = {
				"T zgd<T>(T a, T b) { return a; }\nzgd(1, " + std::string(rng_.chance(50) ? "2.5" : "\"s\"") + ");\n",
				"zgPool<" + std::string(rng_.chance(50) ? "int" : "string") + "> zgbad;\n1;\n",
				"zgrep<int, int>(1, 2);\n",
				"class zgE<T> { T v; int n() { return v.size(); } }\nzgE<string> zgfine;\nzgE<int> zgworse;\n1;\n",
			};
			for (const auto& f : failing) {
				if (rng_.chance(30)) {
					out.segments.push_back(f);
					out.labels.push_back("generics-error");
				}
			}
			return out;
		}

	private:
		fuzz_rng rng_;
		uint64_t seed_;
	};

} // namespace jai::fuzz
