#pragma once

// Read-only local band: functions declaring `T x = <lvalue>;` (a class instance from a program-scope array element, a
// program-scope instance, a local, a handle's member, an array field) followed by reads of x mixed with everything that
// can write the source behind it - a helper writing program state, a writing reference argument, a store through a
// handle copied from the source or from program state, a store into the local the source was copied from - and with the
// shapes that keep the copy (a store into x, an alias, a comparison, passing it on). The local elision binds x to the
// source only where none of that can run, so a program's results must not depend on engine::local_alias. Its own RNG
// stream and name prefix `zl`; generate_program's default (never) keeps the programs of the gates that replay seeds.

#include "fuzz_harness.hpp"

#include <string>
#include <vector>

namespace jai::fuzz {

	class local_alias_band {
	public:
		explicit local_alias_band(uint64_t seed) : rng_(mix64(seed + 0x3C6EF372FE94F82BULL)) {}

		bool roll() { return rng_.chance(35); }

		std::string emit() {
			std::vector<std::string> out{
				"class zlC { int a = 0; int b = 0; string s = \"q\"; array<int> xs = [1, 2]; zlC() {} zlC(int p, int q) { a = p; b = q; } }",
				"class zlH { zlC c = zlC(5, 6); zlH() {} }",
				"array<zlC> zlA = [zlC(" + lit() + ", " + lit() + "), zlC(" + lit() + ", " + lit() + "), zlC(" + lit() + ", " + lit() + ")];",
				"zlC zlG = zlC(" + lit() + ", " + lit() + ");",
				"shared_ptr<zlH> zlP = zlH();",
				"shared_ptr<zlC> zlQ = zlG;",
				"int zlpure(int v) { return v * 3 + 1; }",
				"int zlpeek(zlC c) { return c.a + c.xs.size(); }",
				"void zlbump() { zlG.a = zlG.a + 1; zlA[0].b = zlA[0].b + 2; }",
				"void zlref(int& r) { r = r + 5; }",
				"void zlrefc(zlC& c) { c.a = c.a + 7; }",
				"void zlhandle() { shared_ptr<zlH> h = zlP; h.c.a = h.c.a + 11; }",
				"void zlq() { zlQ.a = zlQ.a + 13; }",
				"void zlpush() { zlG.xs.push(4); }",
			};
			const int fns = static_cast<int>(rng_.range(2, 5));
			std::string calls;
			for (int f = 0; f < fns; ++f) {
				const std::string name = "zlf" + std::to_string(f);
				for (auto& l : function(name)) out.push_back(std::move(l));
				calls += " + " + name + "(zi)";
			}
			out.push_back("int zt = 0;");
			out.push_back("for (int zi = 0; zi < 3; zi++) { zt = zt" + calls + "; }");
			out.push_back("[zt, zlG.a, zlG.b, zlG.xs.size(), zlA[0].a, zlA[0].b, zlA[1].a, zlA[2].b, zlP.c.a, zlQ.a];");
			std::string text;
			for (const auto& l : out) text += l + "\n";
			return text;
		}

	private:
		fuzz_rng rng_;

		std::string lit() { return std::to_string(rng_.range(-4, 9)); }

		std::vector<std::string> function(const std::string& name) {
			std::vector<std::string> body;
			body.push_back("int " + name + "(int i) {");
			body.push_back("\tint acc = " + lit() + ";");
			const bool own = rng_.chance(30);
			const bool handle = rng_.chance(25);
			const bool out_array = rng_.chance(30);
			if (own) body.push_back("\tzlC own = zlC(" + lit() + ", " + lit() + ");");
			if (handle) body.push_back(std::string("\tshared_ptr<zlC> hh = ") + (rng_.chance(50) ? "zlA[i]" : "zlG") + ";");
			if (out_array) body.push_back("\tarray<int> out;");
			std::string source;
			bool array_local = false;
			switch (rng_.below(own ? 7 : 6)) {
				case 0: case 1: source = "zlA[i]"; break;
				case 2: source = "zlG"; break;
				case 3: source = "zlA[(i + 1) % 3]"; break;
				case 4: source = "zlP.c"; break;
				case 5: array_local = true; source = "zlG.xs"; break;
				default: source = "own"; break;
			}
			const bool nested = rng_.chance(25);
			std::string indent = "\t";
			if (nested) { body.push_back("\tfor (int k = 0; k < 2; k++) {"); indent = "\t\t"; }
			body.push_back(indent + (array_local ? "array<int> x = " : "zlC x = ") + source + ";");
			const int stmts = static_cast<int>(rng_.range(2, 5));
			for (int k = 0; k < stmts; ++k) body.push_back(indent + statement(array_local, own, handle, out_array));
			body.push_back(indent + (array_local ? "acc = acc + x.size() + x[0];" : "acc = acc + x.a * 3 + x.b + x.xs.size();"));
			if (nested) body.push_back("\t}");
			if (out_array) body.push_back("\tacc = acc + out.size();");
			body.push_back("\treturn acc;");
			body.push_back("}");
			return body;
		}

		std::string statement(bool array_local, bool own, bool handle, bool out_array) {
			switch (rng_.below(20)) {
				case 0: return array_local ? "acc = acc + x[1];" : "acc = acc + x.a;";
				case 1: return array_local ? "acc = acc + x.size();" : "acc = acc + x.xs[0] + x.s.size();";
				case 2: return array_local ? "acc = acc + zlpure(x[0]);" : "acc = acc + zlpure(x.b);";
				case 3: return "zlbump();";
				case 4: return "zlref(zlG.a);";
				case 5: return "zlref(zlA[i].b);";
				case 6: return "zlrefc(zlA[i]);";
				case 7: return "zlhandle();";
				case 8: return "zlq();";
				case 9: return "zlpush();";
				case 10: return own ? "own.a = own.a + 3;" : "acc = acc + 1;";
				case 11: return handle ? "hh.a = hh.a + 17;" : "acc = acc + 2;";
				case 12: return out_array ? (array_local ? "out.push(x[0]);" : "out.push(x.a);") : "acc = acc + 3;";
				case 13: return array_local ? "x.push(9);" : "x.a = x.a + 1;";
				case 14: return array_local ? "{ var& r = x; r[0] = 8; }" : "{ var& r = x; r.b = 9; }";
				case 15: return array_local ? "if (x == zlG.xs) { acc = acc + 1; }" : "if (x == zlG) { acc = acc + 1; }";
				case 16: return array_local ? "acc = acc + x[0];" : "acc = acc + zlpeek(x);";
				case 17: return "zlG = zlC(" + lit() + ", " + lit() + ");";
				case 18: return "zlA[i].a = zlA[i].a + 1;";
				default: return array_local ? "acc = acc + x[0] * 2;" : "acc = acc + x.b * 2;";
			}
		}
	};

} // namespace jai::fuzz
