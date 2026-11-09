#pragma once

// Parallel band (jaiscript_fuzz --parallel-band, and the suite's Parallel For generated_bodies_run_as_the_serial_loop): data-parallel
// bodies drawn from the shapes ordinary code writes (captured scalars, tables, grids, objects, tables of objects and maps, ::names,
// const references, computed chain roots, lvalue conditionals, script calls with value, const T& and writing T& parameters, format,
// math, local containers, switch, try, do-while), each run as the serial `for` and as `parallel_for` at several worker counts on the
// interpreter, the vm and the vm's reference mode. An admitted body's output is the serial loop's, byte for byte. A hazard body (a
// write to captured state, through a reference, a default, a ::name or a conditional's arm, a serial call) is refused before any
// element runs, with a parallel_for error naming what the body wrote, never a node the parser inserted.

#include "fuzz_harness.hpp"

#include <jaiscript/stdlib/stdlib.hpp>

#include <string>
#include <vector>

namespace jai::fuzz::parallel {

	struct generated {
		std::string program;        // `PFOR` marks the loop keyword: `for` is the serial oracle, `parallel_for` the subject
		bool hazard = false;
		std::string refused_name;   // a hazard's planted name, quoted in the refusal
	};

	inline std::string list_text(fuzz_rng& rng, int n, int lo, int hi) {
		std::string result = "[";
		for (int i = 0; i < n; ++i) { result += (i ? ", " : "") + std::to_string(rng.range(lo, hi)); }
		return result + "]";
	}

	inline generated generate(uint64_t seed, bool hazard) {
		fuzz_rng rng(mix64(seed + 0x5DEECE66DULL));
		generated result;
		result.hazard = hazard;
		const int n = static_cast<int>(rng.range(4, 24));
		const int nt = static_cast<int>(rng.range(2, 6));
		const int ng = static_cast<int>(rng.range(2, 4));
		const int np = static_cast<int>(rng.range(2, 4));
		const int c = static_cast<int>(rng.range(1, 9));
		std::string& p = result.program;
		p += "int K = " + std::to_string(rng.range(-9, 40)) + ";\nfloat F = " + std::to_string(rng.range(1, 9)) + ".5;\nstring S = \"pq" + std::to_string(rng.range(0, 99)) + "\";\n";
		p += "array<int> T = " + list_text(rng, nt, -20, 90) + ";\n";
		p += "array<array<int>> G = [";
		for (int g = 0; g < ng; ++g) { p += (g ? ", " : "") + list_text(rng, 3, 0, 50); }
		p += "];\n";
		p += "class P { int a = 1; float b = 0.5; string s = \"p\"; array<int> xs = [1, 2, 3]; int sum() { return a + xs.size(); } void bump() { a += 1; } }\n";
		p += "P mkp(int k) { P q; q.a = k; q.b = k * 0.25; q.s = to_string(k); q.xs.push(k); return q; }\n";
		p += "P PC = mkp(" + std::to_string(rng.range(1, 30)) + ");\narray<P> PS = [";
		for (int i = 0; i < np; ++i) { p += (i ? ", " : "") + std::string("mkp(") + std::to_string(rng.range(-5, 25)) + ")"; }
		p += "];\nmap<string, int> M = {\"a\": " + std::to_string(rng.range(0, 9)) + ", \"b\": " + std::to_string(rng.range(0, 9)) + "};\n";
		p += "int hf(int x) { return x * " + std::to_string(c) + " + K; }\n";
		p += "int hr(const array<int>& t, int i) { return t[i % t.size()]; }\n";
		p += "int pick(int a, int b, bool w) { return w ? a : b; }\n";
		p += "void inc(int& v) { v += 1; }\n";
		p += "void setd(int& x = K) { x = 1; }\n";
		p += "string tag(int x) { return format(\"<{0}>\", x); }\n";
		p += "array<int> idx = [];\narray<int> out = [];\narray<string> outs = [];\n";
		p += "for (int k = 0; k < " + std::to_string(n) + "; ++k) { idx.push(k); out.push(0); outs.push(\"\"); }\n";

		const std::string ci = std::to_string(c);
		const std::string ti = "i % " + std::to_string(nt);
		const std::string gi = "i % " + std::to_string(ng);
		const std::string pi = "i % " + std::to_string(np);
		const std::vector<std::string> reads = {
			"acc += T[" + ti + "] * " + ci + ";",
			"acc += G[" + gi + "][i % 3];",
			"acc += PS[" + pi + "].a + PS[" + pi + "].sum();",
			"acc += PC.a + PC.xs[i % 3];",
			"acc += M[\"a\"] + (M.contains(\"b\") ? 1 : 0);",
			"{ const auto& row = G[" + gi + "]; const int& t = T[" + ti + "]; acc += row[0] + t; }",
			"for (const auto& v : T) { acc += v; }",
			"for (auto q : PS) { acc += q.a; }",
			"acc += hf(i) + hr(T, i);",
			"acc += ::K + ::T[" + ti + "];",
			"{ int a = i; int b = " + ci + "; acc += pick(a, b, i > " + ci + ") + hf(i > " + ci + " ? a : b); }",
			"{ int a = 0; int b = 0; (i % 2 == 0 ? a : b) = i; auto& r = i > " + ci + " ? a : b; r += 1; acc += a * 3 + b; }",
			"{ P q = mkp(i); q.a += 1; acc += q.a + mkp(i).a + (i > " + ci + " ? PC : q).a; }",
			"{ int v = i; inc(v); setd(v); acc += v; }",
			"{ array<int> t = []; for (int k = 0; k < i % 4; ++k) { t.push(k); } acc += t.size(); }",
			"{ map<string, int> mm; mm[\"x\"] = i; acc += mm[\"x\"]; }",
			"switch (i % 3) { case 0: acc += 1; break; case 1: acc += 2; fallthrough; default: acc += 3; }",
			"try { if (i % 5 == 0) { throw \"e\"; } acc += 1; } catch (e) { acc += 7; }",
			"{ int k = 0; do { k += 1; } while (k < i % 3); acc += k; }",
			"acc += cast<int>(math::sqrt(cast<float>(i * i))) + math::abs(-i) + cast<int>(F);",
			"{ P q = PS[" + pi + "]; q.bump(); acc += q.a; }",
			"acc += (i > " + ci + " ? PS[0] : PC).a;",
		};
		std::string body = "\tint acc = i;\n";
		const int statements = static_cast<int>(rng.range(2, 7));
		for (int s = 0; s < statements; ++s) { body += "\t" + rng.pick(reads) + "\n"; }
		if (hazard) {
			struct planted { const char* statement; const char* name; };
			const planted hazards[] = {
				{ "T[0] = acc;", "T" }, { "K = acc;", "K" }, { "PC.a = acc;", "PC" }, { "PS[0].a = acc;", "PS" },
				{ "inc(K);", "K" }, { "inc(PC.a);", "PC" }, { "setd();", "K" }, { "::K = acc;", "K" },
				{ "{ int b = 0; (i > 0 ? K : b) = 1; }", "K" }, { "T.push(acc);", "T" }, { "PS[0].bump();", "PS" },
				{ "PC.bump();", "PC" }, { "print(acc);", "print" }, { "{ int b = 0; auto& r = i > 0 ? K : b; r = 2; }", "K" },
				{ "for (auto& v : T) { v = 0; }", "T" }, { "{ int b = 0; inc(i > 0 ? K : b); }", "K" },
			};
			const planted& h = hazards[rng.below(static_cast<uint32_t>(sizeof(hazards) / sizeof(hazards[0])))];
			body += std::string("\t") + h.statement + "\n";
			result.refused_name = h.name;
		}
		body += "\tout[i] = acc;\n\touts[i] = format(\"{0}:{1}\", i, acc) + tag(i) + S.substr(0, 2) + PS[" + pi + "].s;\n";
		p += "PFOR (int i : idx) {\n" + body + "}\n";
		p += "to_json(out) + \" \" + to_json(outs) + \" \" + to_json(T) + \" \" + to_string(K) + \" \" + to_string(PC.a) + \" \" + to_string(PS[0].a);\n";
		return result;
	}

	struct subject {
		jai::backend_type backend = jai::backend_type::vm;
		bool reference = false;
		size_t workers = 1;
		std::string label() const {
			return std::string(backend == jai::backend_type::interpreter ? "interp" : reference ? "vm-reference" : "vm") + " x" + std::to_string(workers);
		}
	};

	inline std::string run(const std::string& source, const subject& s, bool parallel) {
		std::string text = source;
		const std::string keyword = parallel ? "parallel_for" : "for";
		for (size_t at = text.find("PFOR"); at != std::string::npos; at = text.find("PFOR", at)) { text.replace(at, 4, keyword); }
		auto eng = jai::engine::make();
		jai::stdlib::register_all(*eng);
		eng->set_backend(s.backend);
		eng->vm_reference_mode(s.reference);
		eng->parallel_thread_count(s.workers);
		eng->execution_budget(0);
		eng->execution_step_budget(50'000'000);
		try {
			return "VALUE " + eng->execute(text).to_string();
		} catch (const std::exception& e) {
			return std::string("ERROR ") + e.what();
		}
	}

	inline const std::vector<subject>& subjects() {
		static const std::vector<subject> every = {
			{ jai::backend_type::interpreter, false, 1 }, { jai::backend_type::interpreter, false, 3 }, { jai::backend_type::interpreter, false, 8 },
			{ jai::backend_type::vm, false, 1 }, { jai::backend_type::vm, false, 3 }, { jai::backend_type::vm, false, 8 },
			{ jai::backend_type::vm, true, 3 },
		};
		return every;
	}

	// Empty when the seed holds: an admitted body prints the serial loop's output on every subject, a hazard body is refused on
	// every subject with a parallel_for error quoting its planted name; else what broke, per subject
	inline std::string check(const generated& g) {
		std::string result;
		const std::string serial = g.hazard ? std::string() : run(g.program, { jai::backend_type::interpreter, false, 1 }, false);
		if (!g.hazard && serial.rfind("VALUE ", 0) != 0) { return "the serial loop failed: " + serial + "\n"; }
		static const char* const parser_nodes[] = { "cast<T&>", "statement is not allowed", "expression is not allowed", "target is not allowed" };
		for (const subject& s : subjects()) {
			const std::string got = run(g.program, s, true);
			if (!g.hazard) {
				if (got != serial) { result += s.label() + ": " + got + "\n    serial: " + serial + "\n"; }
				continue;
			}
			const bool refused = got.rfind("ERROR parallel_for: ", 0) == 0 && got.find("'" + g.refused_name + "'") != std::string::npos;
			bool names_parser_node = false;
			for (const char* node : parser_nodes) { names_parser_node = names_parser_node || got.find(node) != std::string::npos; }
			if (!refused || names_parser_node) { result += s.label() + ": expected a parallel_for refusal naming '" + g.refused_name + "', got " + got + "\n"; }
		}
		return result;
	}

} // namespace jai::fuzz::parallel
