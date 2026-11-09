// jaiscript_fuzz: differential fuzzer for the two JaiScript backends.
//
// Generates random-but-valid programs from a weighted grammar, runs each on
// BOTH the tree-walking interpreter and the bytecode VM (or, with --vm-reference,
// the optimized VM and the VM in reference mode), and reports any divergence in
// results / printed output / error text / engine liveness.
// Fully deterministic from the seed.
//
// jaiscript_fuzz --help prints every flag.

#include "fuzz_census.hpp"
#include "fuzz_generator.hpp"
#include "fuzz_harness.hpp"
#include "fuzz_jaibite.hpp"
#include "fuzz_order_band.hpp"
#include "fuzz_parallel_band.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#include <jaiscript/testing/timing.hpp>
#include <jaiscript/testing/unattended.hpp>

using namespace jai::fuzz;

namespace {

	struct options {
		uint64_t seed = 0;
		bool seed_given = false;
		uint64_t seed_start = 0;
		uint64_t iterations = 1000;
		int max_depth = 4;
		bool minimize_flag = false;
		std::string corpus;
		double timeout_ms = 250.0;
		bool child = false;
		bool isolate = false;
		bool dump = false;
		bool use_suppressions = true;
		bool proofs = true;
		bool param_alias = true;
		bool passes = true;
		bool vm_reference = false;   // the pair: the optimized vm against the reference vm
		int lowering = 0;   // compare_program's: 0 each backend's default, 1 both boxed, 2 both lowered
		mutable uint64_t lowered_locals = 0, lowered_programs = 0;   // the census: what the vm side lowered
		uint32_t miss_every = 0;
		band_mode band = band_mode::sometimes;
		band_mode math_band = band_mode::sometimes;
		band_mode call_args_band = band_mode::sometimes;
		band_mode blocks = band_mode::sometimes;
		band_mode local_alias_band = band_mode::sometimes;
		band_mode regex_band = band_mode::sometimes;
		bool order = false;
		bool order_args = false;
		bool parallel = false;
		bool jaibite_mutate = false;
		bool census = false;
		bool rename = false;   // the metamorphic renaming oracle on top of the differential
		std::string run_file; // triage: compare an on-disk program instead of a generated one
		std::string exe_path;
	};

	void print_usage() {
		std::cout <<
		"usage:\n"
		"   jaiscript_fuzz                          run seeds 0..999 (default --iterations 1000)\n"
		"   jaiscript_fuzz --iterations 10000       run seeds 0..9999\n"
		"   jaiscript_fuzz --seed 1234              run exactly one seed (verbose)\n"
		"   jaiscript_fuzz --seed-start 5000        first seed of the run's range\n"
		"   jaiscript_fuzz --minimize               auto-minimize each divergence\n"
		"   jaiscript_fuzz --corpus DIR             write failing programs + minimized forms + repro skeletons\n"
		"   jaiscript_fuzz --timeout-ms 250         per-execute engine budget (default 250)\n"
		"   jaiscript_fuzz --max-depth 4            expression depth cap\n"
		"   jaiscript_fuzz --dump N                 print the generated program for seed N and exit\n"
		"   jaiscript_fuzz --child --seed N         one seed, machine-readable verdict via exit code\n"
		"                                           (0 agree, 20 diverge, 21 budget-skip)\n"
		"   jaiscript_fuzz --isolate                spawn a --child process per seed (crash isolation:\n"
		"                                           a VM crash doesn't kill the run)\n"
		"   jaiscript_fuzz --no-suppressions        report known (suppressed) divergences too\n"
		"   jaiscript_fuzz --vm-reference           the pair is the optimized vm against the vm in engine::vm_reference_mode (every\n"
		"                                           optimization off): a divergence is a bug of an optimized lane (the everyday pair;\n"
		"                                           every other flag composes with it, the switches applying to the optimized side)\n"
		"   jaiscript_fuzz --no-proofs              run both backends with engine::type_proofs(false)\n"
		"   jaiscript_fuzz --no-param-alias         run both backends with engine::parameter_alias(false) and engine::local_alias(false)\n"
		"   jaiscript_fuzz --local-alias-band       append the read-only local band (by-value locals beside writes of their sources) to\n"
		"                                           every program (default: its own roll, ~35%); --no-local-alias-band never appends it\n"
		"   jaiscript_fuzz --regex-band             append the regex band (generated patterns through every regex call on strings and\n"
		"                                           utf8_strings, literal and built at run time) to every program (default: its own roll,\n"
		"                                           ~25%); --no-regex-band never appends it\n"
		"   jaiscript_fuzz --no-passes              run both backends with engine::ir_passes(false)\n"
		"   jaiscript_fuzz --no-lowering            run both backends with engine::aggregate_lowering(false) (by default the interpreter\n"
		"                                           runs boxed and the vm lowered, so every run is the lowering's differential)\n"
		"   jaiscript_fuzz --lowered-interp         run both backends lowered (the interpreter's lowered tree against the vm's)\n"
		"   jaiscript_fuzz --element-band           prepend the element-argument band to every program (default: its own roll, ~30%)\n"
		"   jaiscript_fuzz --no-element-band        never prepend it (the programs of the generator before the band)\n"
		"   jaiscript_fuzz --math-band              prepend the math band (math::vec2/vec3/ivec2/ivec3 through every lvalue kind) to every\n"
		"                                           program (default: its own roll, ~25%); --no-math-band never prepends it\n"
		"   jaiscript_fuzz --call-args-band         prepend the call-argument band (typed arrays read by index as the arguments of one kept call\n"
		"                                           in a counted loop, two to four from different arrays, mixed with locals) to every program\n"
		"                                           (default: its own roll, ~25%); --no-call-args-band never prepends it\n"
		"   jaiscript_fuzz --block-shapes           block shapes in every program (default: their own roll, ~50%): the main program's\n"
		"                                           statements run inside a function among closable counted loops (typed locals, typed local\n"
		"                                           and program-scope arrays at plain [i], branches, math intrinsics, nested loops, grafted and\n"
		"                                           kept helper calls), and the math band gains a lowered-math loop; --no-block-shapes never\n"
		"   jaiscript_fuzz --parallel-band          data-parallel bodies run as the serial for and as parallel_for at 1, 3 and 8 workers on\n"
		"                                           the interpreter, the vm and its reference mode: equal output, or a hazard body refused\n"
		"                                           naming what it wrote (each seed an admitted and a hazard program; --dump prints both)\n"
		"   jaiscript_fuzz --order-band             ordering programs (writing calls beside reads of what they write), three oracles: the\n"
		"                                           backends agree, the program agrees with its metamorphic variant (operands evaluated into\n"
		"                                           locals left to right), and every run prints what the generator's spec model computes\n"
		"   jaiscript_fuzz --order-args             with --order-band: call arguments read what a sibling argument's call writes too (the\n"
		"                                           model captures each by-value argument where it is evaluated)\n"
		"   jaiscript_fuzz --miss-every N           the vm side misses at every Nth plain cell instruction (a build with\n"
		"                                           JAISCRIPT_MISS_INJECTION defined: the flush and the continuation contract at every position)\n"
		"   jaiscript_fuzz --rename                 the metamorphic renaming oracle too: every generated name gains a suffix, and each\n"
		"                                           backend's outcome (names restored) must equal its run of the original\n"
		"   jaiscript_fuzz --census                the block census instead of the differential: per segment label (each band, the main\n"
		"                                           program, the reload; --order-band / --order-args for the ordering programs) the share of\n"
		"                                           programs that compiled a closed loop, entered a block and entered a closed block, the refused\n"
		"                                           loops by class and instruction, and in a JAISCRIPT_VM_COVERAGE build the executed operations\n"
		"                                           in cells and the general form's reasons (vm side only; the band flags pick the segments)\n"
		"   jaiscript_fuzz --jaibite-mutate --iterations 10000\n"
		"                                         isolated loader-only byte mutants, BOTH backends\n"
		"                                         (0 accepted, 24 clean rejection, 20 finding, 23 hang)\n"
		"\n"
		"Exit code: 0 = no new divergences, 1 = new divergences found, 20/21 = child verdicts.\n";
	}

	bool parse_args(int argc, char** argv, options& opt) {
		opt.exe_path = argv[0];
		for (int i = 1; i < argc; ++i) {
			std::string a = argv[i];
			auto next = [&](const char* what) -> const char* {
				if (i + 1 >= argc) { std::cerr << "missing value for " << what << "\n"; return nullptr; }
				return argv[++i];
			};
			if (a == "--seed") { const char* v = next("--seed"); if (!v) return false; opt.seed = std::stoull(v); opt.seed_given = true; }
			else if (a == "--seed-start") { const char* v = next("--seed-start"); if (!v) return false; opt.seed_start = std::stoull(v); }
			else if (a == "--iterations") { const char* v = next("--iterations"); if (!v) return false; opt.iterations = std::stoull(v); }
			else if (a == "--max-depth") { const char* v = next("--max-depth"); if (!v) return false; opt.max_depth = std::stoi(v); }
			else if (a == "--minimize") { opt.minimize_flag = true; }
			else if (a == "--rename") { opt.rename = true; }
			else if (a == "--corpus") { const char* v = next("--corpus"); if (!v) return false; opt.corpus = v; }
			else if (a == "--timeout-ms") { const char* v = next("--timeout-ms"); if (!v) return false; opt.timeout_ms = std::stod(v); }
			else if (a == "--child") { opt.child = true; }
			else if (a == "--isolate") { opt.isolate = true; }
			else if (a == "--jaibite-mutate") { opt.jaibite_mutate = true; opt.isolate = true; }
			else if (a == "--dump") { const char* v = next("--dump"); if (!v) return false; opt.seed = std::stoull(v); opt.seed_given = true; opt.dump = true; }
			else if (a == "--no-suppressions") { opt.use_suppressions = false; }
			else if (a == "--no-proofs") { opt.proofs = false; }
			else if (a == "--no-param-alias") { opt.param_alias = false; }
			else if (a == "--no-passes") { opt.passes = false; }
			else if (a == "--vm-reference") { opt.vm_reference = true; }
			else if (a == "--no-lowering") { opt.lowering = 1; }
			else if (a == "--lowered-interp") { opt.lowering = 2; }
			else if (a == "--element-band") { opt.band = band_mode::always; }
			else if (a == "--no-element-band") { opt.band = band_mode::never; }
			else if (a == "--math-band") { opt.math_band = band_mode::always; }
			else if (a == "--no-math-band") { opt.math_band = band_mode::never; }
			else if (a == "--call-args-band") { opt.call_args_band = band_mode::always; }
			else if (a == "--no-call-args-band") { opt.call_args_band = band_mode::never; }
			else if (a == "--census") { opt.census = true; }
			else if (a == "--block-shapes") { opt.blocks = band_mode::always; }
			else if (a == "--no-block-shapes") { opt.blocks = band_mode::never; }
			else if (a == "--local-alias-band") { opt.local_alias_band = band_mode::always; }
			else if (a == "--no-local-alias-band") { opt.local_alias_band = band_mode::never; }
			else if (a == "--regex-band") { opt.regex_band = band_mode::always; }
			else if (a == "--no-regex-band") { opt.regex_band = band_mode::never; }
			else if (a == "--order-band") { opt.order = true; }
			else if (a == "--parallel-band") { opt.parallel = true; }
			else if (a == "--order-args") { opt.order = true; opt.order_args = true; }
			else if (a == "--miss-every") { const char* v = next("--miss-every"); if (!v) return false; opt.miss_every = static_cast<uint32_t>(std::stoul(v)); }
			else if (a == "--run") { const char* v = next("--run"); if (!v) return false; opt.run_file = v; }
			else if (a == "--help" || a == "-h") { print_usage(); std::exit(0); }
			else { std::cerr << "unknown argument: " << a << "\n"; return false; }
		}
		if (opt.jaibite_mutate && (opt.dump || !opt.run_file.empty() || opt.minimize_flag)) {
			std::cerr << "--jaibite-mutate cannot be combined with --dump, --run, or --minimize\n";
			return false;
		}
		return true;
	}

	void write_corpus_entry(const options& opt, const program& original, const program* minimized,
	                        const std::string& diff, const std::string& suppression_id) {
		if (opt.corpus.empty()) return;
		namespace fs = std::filesystem;
		fs::create_directories(opt.corpus);
		std::string stem = (opt.corpus + "/div_seed") + std::to_string(original.seed);
		{
			std::ofstream f(stem + ".jai");
			f << "// fuzz seed " << original.seed << (suppression_id.empty() ? "" : (" [suppressed: " + suppression_id + "]")) << "\n";
			f << original.joined();
		}
		if (minimized) {
			std::ofstream f(stem + ".min.jai");
			f << "// fuzz seed " << original.seed << " (minimized)\n";
			f << minimized->joined();
		}
		{
			std::ofstream f(stem + ".report.md");
			f << "# Divergence: fuzz seed " << original.seed << "\n\n";
			if (!suppression_id.empty()) f << "Suppressed as known divergence: `" << suppression_id << "`\n\n";
			f << "Command: `jaiscript_fuzz --seed " << original.seed
			  << " --max-depth " << opt.max_depth << " --timeout-ms " << opt.timeout_ms << (opt.vm_reference ? " --vm-reference" : "") << "`\n\n";
			f << "## Diff\n\n```\n" << diff << "```\n\n";
			const program& repro = minimized ? *minimized : original;
			f << "## Minimized program\n\n```\n" << repro.joined() << "```\n\n";
			f << "## Foundry test skeleton\n\n```cpp\n" << foundry_skeleton(repro, diff, opt.vm_reference) << "```\n";
		}
	}

	// Verdict for one seed. Returns the classification and (optionally) reports.
	enum class verdict { agree, diverge_new, diverge_suppressed, budget_skip };

	verdict run_seed(const options& opt, uint64_t seed, bool verbose) {
		program p = generate_program(seed, opt.max_depth, opt.band, opt.math_band, opt.call_args_band, opt.blocks, opt.local_alias_band, opt.regex_band);
		double budget_seconds = opt.timeout_ms / 1000.0;
		compare_result cr = compare_program(p, budget_seconds, opt.proofs, opt.param_alias, opt.passes, opt.miss_every, opt.lowering, opt.vm_reference);
		const run_outcome& optimized_vm = opt.vm_reference ? cr.interp : cr.vm;
		opt.lowered_locals += optimized_vm.lowered_locals;
		if (optimized_vm.lowered_locals) ++opt.lowered_programs;

		if (cr.kind == compare_result::budget_skip) {
			if (verbose || cr.budget_suspect) {
				std::cout << "[seed " << seed << "] budget-skip"
				          << (cr.budget_suspect ? " SUSPECT (only one backend hit the budget - possible hang divergence, likely wall-clock noise)" : "")
				          << "\n";
			}
			return verdict::budget_skip;
		}
		if (cr.kind == compare_result::agree && opt.rename) {
			// a renamed program must answer as the original did, on each backend (names carry no meaning)
			const program q = renamed(p);
			run_config rc;
			rc.budget_seconds = budget_seconds;
			rc.proofs = opt.proofs;
			rc.param_alias = opt.param_alias;
			rc.passes = opt.passes;
			rc.miss_every = opt.miss_every;
			rc.lowering = opt.lowering == 0 ? -1 : opt.lowering == 2 ? 1 : 0;
			std::string diff;
			for (int side = 0; side < 2; ++side) {
				rc.use_vm = opt.vm_reference || side == 1;
				rc.vm_reference = opt.vm_reference && side == 1;
				const run_outcome r = run_program(q, rc);
				if (r.budget_hit) { return verdict::budget_skip; }
				const std::string d = rename_difference(side ? cr.vm : cr.interp, r);
				if (!d.empty()) { diff += std::string("  [") + (side ? right_label(opt.vm_reference) : left_label(opt.vm_reference)) + "]\n" + d; }
			}
			if (!diff.empty()) {
				std::cout << "\n=== RENAME DIVERGENCE seed " << seed << " ===\n" << diff;
				write_corpus_entry(opt, p, &q, diff, std::string());
				return verdict::diverge_new;
			}
		}
		if (cr.kind == compare_result::agree) {
			if (verbose) {
				std::cout << "[seed " << seed << "] agree\n";
				for (size_t i = 0; i < cr.interp.segment_results.size(); ++i)
					std::cout << "  segment " << i << ": " << cr.interp.segment_results[i] << "\n";
				if (!cr.interp.printed.empty()) std::cout << "  printed:\n" << cr.interp.printed;
			}
			return verdict::agree;
		}

		std::string supp = opt.use_suppressions ? match_suppression(p, cr.diff) : std::string();

		std::cout << "\n=== DIVERGENCE seed " << seed << (supp.empty() ? "" : (" [suppressed: " + supp + "]")) << " ===\n";
		std::cout << cr.diff;

		program minimized = p;
		bool have_min = false;
		// Only minimize UNRECOGNIZED divergences: suppressed ones are already
		// triaged, and minimization is the expensive step of the run.
		if (opt.minimize_flag && supp.empty()) {
			minimize_stats stats;
			auto still = [&](const program& cand) {
				// Any divergence keeps the reduction: drifting into a KNOWN shape is
				// handled by re-matching suppressions on the minimized form below.
				return compare_program(cand, budget_seconds, opt.proofs, opt.param_alias, opt.passes, opt.miss_every, opt.lowering, opt.vm_reference).kind == compare_result::diverge;
			};
			minimized = minimize(p, still, stats);
			have_min = true;
			std::cout << "--- minimized (" << stats.reductions << " reductions, " << stats.checks << " checks) ---\n";
			std::cout << minimized.joined() << "\n";
			compare_result mc = compare_program(minimized, budget_seconds, opt.proofs, opt.param_alias, opt.passes, opt.miss_every, opt.lowering, opt.vm_reference);
			std::cout << "--- minimized diff ---\n" << mc.diff;
			// Some shapes only become recognizable after minimization (e.g. an
			// eval-order divergence buried under printed-output noise): re-match
			// the suppressions against the minimized form.
			if (opt.use_suppressions && supp.empty() && mc.kind == compare_result::diverge) {
				supp = match_suppression(minimized, mc.diff);
				if (!supp.empty())
					std::cout << "--- reclassified as suppressed after minimization: " << supp << " ---\n";
			}
		} else if (verbose) {
			std::cout << "--- program ---\n" << p.joined() << "\n";
		}
		write_corpus_entry(opt, p, have_min ? &minimized : nullptr, cr.diff, supp);
		std::cout.flush();
		return supp.empty() ? verdict::diverge_new : verdict::diverge_suppressed;
	}

	struct order_stats {
		uint64_t programs = 0, backends = 0, metamorphic = 0, model_checked = 0, model_agree = 0, abstained = 0;
	};

	std::string labeled_difference(const std::string& a, const std::string& b, const std::string& la, const std::string& lb) {
		size_t i = 0;
		while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
		auto excerpt = [i](const std::string& s) {
			std::string e = s.substr(i > 40 ? i - 40 : 0, 120);
			for (auto& c : e) if (c == '\n') c = '|';
			return e;
		};
		return "at byte " + std::to_string(i) + "\n      " + la + ": ..." + excerpt(a) + "\n      " + lb + ": ..." + excerpt(b);
	}

	std::string order_outcome(const run_outcome& r) {
		std::string s = r.printed;
		for (const auto& x : r.segment_results) { s += (x.rfind("ERROR", 0) == 0 ? std::string("ERROR") : x) + "\n"; }
		return s;
	}

	void write_order_corpus(const options& opt, uint64_t seed, const order::generated& g, const std::string& report) {
		if (opt.corpus.empty()) return;
		namespace fs = std::filesystem;
		fs::create_directories(opt.corpus);
		const std::string stem = opt.corpus + "/order_seed" + std::to_string(seed);
		std::ofstream(stem + ".jai") << g.original.segments[0];
		std::ofstream(stem + ".variant.jai") << g.variant.segments[0];
		std::ofstream(stem + ".expected.txt") << (g.model_ok ? g.expected : "model abstained: " + g.abstained + "\n");
		std::ofstream(stem + ".report.txt") << report;
	}

	// the ordering band's three oracles over one seed: the backends on the program and on its variant, the program against its
	// variant on each backend, and every run against the model's output
	verdict run_order_seed(const options& opt, uint64_t seed, bool verbose, order_stats& st) {
		const order::generated g = order::generate(seed, opt.order_args);
		const double budget_seconds = opt.timeout_ms / 1000.0;
		const compare_result a = compare_program(g.original, budget_seconds, opt.proofs, opt.param_alias, opt.passes, opt.miss_every, opt.lowering, opt.vm_reference);
		const compare_result b = compare_program(g.variant, budget_seconds, opt.proofs, opt.param_alias, opt.passes, opt.miss_every, opt.lowering, opt.vm_reference);
		if (a.kind == compare_result::budget_skip || b.kind == compare_result::budget_skip) {
			if (verbose) std::cout << "[seed " << seed << "] budget-skip\n";
			return verdict::budget_skip;
		}
		++st.programs;
		std::string report;
		bool backends_split = false;
		if (a.kind == compare_result::diverge) { report += "BACKENDS (program):\n" + a.diff; backends_split = true; }
		if (b.kind == compare_result::diverge) { report += "BACKENDS (metamorphic variant):\n" + b.diff; backends_split = true; }
		if (!backends_split) ++st.backends;
		const std::string oi = order_outcome(a.interp), ov = order_outcome(a.vm), vi = order_outcome(b.interp), vv = order_outcome(b.vm);
		if (oi == vi && ov == vv) {
			++st.metamorphic;
		} else {
			report += "METAMORPHIC (program vs variant):\n";
			if (oi != vi) report += "  interp " + labeled_difference(oi, vi, "program", "variant") + "\n";
			if (ov != vv) report += "  vm     " + labeled_difference(ov, vv, "program", "variant") + "\n";
		}
		if (g.model_ok) {
			++st.model_checked;
			auto run_ok = [&](const run_outcome& r) { return r.printed == g.expected && r.segment_results.size() == 1 && r.segment_results[0].rfind("VALUE string:done", 0) == 0; };
			const bool ok_oi = run_ok(a.interp), ok_ov = run_ok(a.vm), ok_vi = run_ok(b.interp), ok_vv = run_ok(b.vm);
			if (ok_oi && ok_ov && ok_vi && ok_vv) {
				++st.model_agree;
			} else {
				report += std::string("MODEL (") + (ok_oi ? "" : " interp") + (ok_ov ? "" : " vm") + (ok_vi ? "" : " interp-variant") + (ok_vv ? "" : " vm-variant") +
					" disagree" + (!ok_oi && !ok_ov && oi == ov ? "; BOTH BACKENDS AGREE WITH EACH OTHER" : "") + "):\n";
				auto show = [&](const char* who, const run_outcome& r, bool ok) {
					if (ok) return;
					report += std::string("  ") + who + " " + labeled_difference(g.expected, r.printed, "model", who) + "\n";
					if (r.segment_results.empty() || r.segment_results[0].rfind("VALUE string:done", 0) != 0) report += std::string("  ") + who + " result: " + (r.segment_results.empty() ? "" : r.segment_results[0]) + "\n";
				};
				show("interp", a.interp, ok_oi); show("vm", a.vm, ok_ov); show("interp-variant", b.interp, ok_vi); show("vm-variant", b.vm, ok_vv);
			}
		} else {
			++st.abstained;
		}
		if (report.empty()) {
			if (verbose) std::cout << "[seed " << seed << "] agree (backends, metamorphic, " << (g.model_ok ? "model" : "model abstained: " + g.abstained) << ")\n";
			return verdict::agree;
		}
		std::cout << "\n=== ORDER DIVERGENCE seed " << seed << " ===\n" << report;
		if (verbose) {
			std::cout << "--- program ---\n" << g.original.segments[0] << "--- variant ---\n" << g.variant.segments[0]
			          << "--- model ---\n" << (g.model_ok ? g.expected : "abstained: " + g.abstained + "\n");
		}
		write_order_corpus(opt, seed, g, report);
		std::cout.flush();
		return verdict::diverge_new;
	}

	int run_mutation_seed(const options& opt, uint64_t seed) {
		try {
			const auto mutant = make_jaibite_mutant(seed, opt.max_depth, opt.proofs, opt.param_alias);
			// Flushed before the first load: crash/hang output keeps the exact case.
			std::cout << "[jaibite mutant " << seed << "] source-seed " << mutant.source_seed
			          << " | " << mutant.operation << " at " << mutant.offset
			          << " | bytes " << mutant.original.size() << " -> " << mutant.bytes.size() << "\n";
			std::cout.flush();
			const auto result = compare_jaibite_mutant(mutant, opt.proofs, opt.param_alias);
			if (result.ok) return result.rejected ? 24 : 0;
			std::cout << result.report;
			if (!opt.corpus.empty()) {
				std::filesystem::create_directories(opt.corpus);
				const std::string stem = opt.corpus + "/jaibite_seed" + std::to_string(seed);
				auto write_bytes = [&](const std::string& suffix, const std::vector<uint8_t>& bytes) {
					std::ofstream file(stem + suffix, std::ios::binary);
					file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				};
				write_bytes(".original.jaibite", mutant.original);
				write_bytes(".mutant.jaibite", mutant.bytes);
				std::ofstream(stem + ".jai") << mutant.source;
				std::ofstream(stem + ".report.txt") << result.report;
			}
		} catch (const std::exception& error) {
			std::cout << "[jaibite mutant " << seed << "] harness failure: " << error.what() << "\n";
		} catch (...) {
			std::cout << "[jaibite mutant " << seed << "] non-standard harness failure\n";
		}
		std::cout.flush();
		return 20;
	}

	bool read_program_file(const options& opt, program& p) {
		std::ifstream f(opt.run_file);
		if (!f) { std::cerr << "cannot open " << opt.run_file << "\n"; return false; }
		std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		p.host_math_aliases = opt.order;
		const std::string marker = "// --- next execute() on same engine ---";
		size_t pos = 0;
		while (true) {
			size_t hit = text.find(marker, pos);
			if (hit == std::string::npos) { p.segments.push_back(text.substr(pos)); break; }
			p.segments.push_back(text.substr(pos, hit - pos));
			pos = hit + marker.size();
		}
		return true;
	}

	// the parallel band: per seed an admitted body and a hazard body, each held to its oracle on every subject
	int run_parallel_band(uint64_t begin, uint64_t end, bool verbose) {
		uint64_t held = 0;
		uint64_t broke = 0;
		for (uint64_t seed = begin; seed < end; ++seed) {
			if (!verbose && seed != begin && (seed - begin) % 100 == 0) { std::cout << "[progress] seed " << seed << " | held " << held << " | broke " << broke << "\n"; std::cout.flush(); }
			for (bool hazard : {false, true}) {
				const parallel::generated g = parallel::generate(seed, hazard);
				const std::string report = parallel::check(g);
				if (report.empty()) {
					++held;
					if (verbose) { std::cout << "[seed " << seed << (hazard ? " hazard" : "") << "] holds\n"; }
					continue;
				}
				++broke;
				std::cout << "\n=== PARALLEL BAND seed " << seed << (hazard ? " (hazard)" : "") << " ===\n" << report << "--- program ---\n" << g.program;
				std::cout.flush();
			}
		}
		std::cout << "\n===== parallel band summary =====\nprograms: " << (held + broke) << " | held: " << held << " | broke: " << broke << "\n";
		return broke ? 1 : 0;
	}

	int run_census(const options& opt, uint64_t begin, uint64_t end) {
		std::vector<std::pair<std::string, census::band_stats>> bands;
		program from_file;
		if (!opt.run_file.empty()) {
			if (!read_program_file(opt, from_file)) return 2;
			begin = 0; end = 1;
		}
		for (uint64_t seed = begin; seed < end; ++seed) {
			if ((seed - begin) % 500 == 0 && seed != begin) { std::cout << "[census] seed " << seed << "\n"; std::cout.flush(); }
			program p = !opt.run_file.empty() ? from_file : opt.order ? order::generate(seed, opt.order_args).original : generate_program(seed, opt.max_depth, opt.band, opt.math_band, opt.call_args_band, opt.blocks, opt.local_alias_band, opt.regex_band);
			if (opt.order && opt.run_file.empty()) p.labels = { opt.order_args ? "order-args" : "order" };
			bool budget_hit = false;
			for (const auto& f : census::measure(p, opt.timeout_ms / 1000.0, opt.proofs, opt.miss_every, &budget_hit)) {
				if (opt.seed_given || !opt.run_file.empty()) {
					std::cout << "[" << f.label << "] loops " << f.loops << " regions " << f.regions << " entries " << f.entries << " misses " << f.misses
					          << (f.census ? " closed entered " + std::to_string(f.totals.closed_entered) + " ops " + std::to_string(f.totals.operations) + " cells " + std::to_string(f.totals.cells) : std::string());
					for (const auto& o : f.open) std::cout << " | open: " << o;
					std::cout << "\n";
				}
				auto it = std::find_if(bands.begin(), bands.end(), [&](const auto& b) { return b.first == f.label; });
				if (it == bands.end()) { bands.emplace_back(f.label, census::band_stats{}); it = bands.end() - 1; }
				it->second.add(f, budget_hit);
			}
		}
		std::cout << "===== block census: seeds " << begin << ".." << (end - 1) << (jai::engine::vm_coverage_available() ? " (coverage build)" : " (listing + counters; a JAISCRIPT_VM_COVERAGE build adds operations)") << " =====\n";
		std::cout << census::report(bands);
		return 0;
	}

	int run_isolated(const options& opt, uint64_t seed) {
		std::string cmd = "\"" + opt.exe_path + "\" --child --seed " + std::to_string(seed) +
			" --max-depth " + std::to_string(opt.max_depth) +
			" --timeout-ms " + std::to_string(opt.timeout_ms);
		if (opt.minimize_flag) cmd += " --minimize";
		if (!opt.corpus.empty()) cmd += " --corpus \"" + opt.corpus + "\"";
		if (!opt.use_suppressions) cmd += " --no-suppressions";
		if (!opt.proofs) cmd += " --no-proofs";
		if (!opt.param_alias) cmd += " --no-param-alias";
		if (!opt.passes) cmd += " --no-passes";
		if (opt.vm_reference) cmd += " --vm-reference";
		if (opt.lowering == 1) cmd += " --no-lowering";
		if (opt.lowering == 2) cmd += " --lowered-interp";
		if (opt.band == band_mode::always) cmd += " --element-band";
		if (opt.band == band_mode::never) cmd += " --no-element-band";
		if (opt.math_band == band_mode::always) cmd += " --math-band";
		if (opt.math_band == band_mode::never) cmd += " --no-math-band";
		if (opt.call_args_band == band_mode::always) cmd += " --call-args-band";
		if (opt.call_args_band == band_mode::never) cmd += " --no-call-args-band";
		if (opt.blocks == band_mode::always) cmd += " --block-shapes";
		if (opt.blocks == band_mode::never) cmd += " --no-block-shapes";
		if (opt.local_alias_band == band_mode::always) cmd += " --local-alias-band";
		if (opt.local_alias_band == band_mode::never) cmd += " --no-local-alias-band";
		if (opt.regex_band == band_mode::always) cmd += " --regex-band";
		if (opt.regex_band == band_mode::never) cmd += " --no-regex-band";
		if (opt.rename) cmd += " --rename";
		if (opt.order) cmd += opt.order_args ? " --order-args" : " --order-band";
		if (opt.miss_every) cmd += " --miss-every " + std::to_string(opt.miss_every);
		if (opt.jaibite_mutate) cmd += " --jaibite-mutate";
#ifdef _WIN32
		cmd = "\"" + cmd + "\""; // cmd.exe strips one layer of quotes
		FILE* pipe = _popen(cmd.c_str(), "r");
#else
		FILE* pipe = popen(cmd.c_str(), "r");
#endif
		if (!pipe) { std::cerr << "[seed " << seed << "] failed to spawn child\n"; return -1; }
		char buf[4096];
		std::string out;
		while (fgets(buf, sizeof(buf), pipe)) out += buf;
#ifdef _WIN32
		int code = _pclose(pipe);
#else
		int code = pclose(pipe);
		if (code != -1) code = WIFEXITED(code) ? WEXITSTATUS(code) : -1;
#endif
		if (!out.empty() && code != 0 && !(opt.jaibite_mutate && code == 24)) std::cout << out;
		return code;
	}

} // namespace

int main(int argc, char** argv) {
	jai::foundry::route_faults_to_stderr();
	options opt;
	if (!parse_args(argc, argv, opt)) return 2;
	if (opt.miss_every && !jai::engine::miss_injection_available()) { std::cerr << "--miss-every needs a build with JAISCRIPT_MISS_INJECTION defined\n"; return 2; }

	if (opt.dump && opt.parallel) {
		std::cout << parallel::generate(opt.seed, false).program << "// --- hazard ---\n" << parallel::generate(opt.seed, true).program;
		return 0;
	}

	if (opt.dump && opt.order) {
		const order::generated g = order::generate(opt.seed, opt.order_args);
		std::cout << g.original.segments[0] << "// --- metamorphic variant ---\n" << g.variant.segments[0] << "// --- model ---\n"
		          << (g.model_ok ? g.expected : "abstained: " + g.abstained + "\n");
		return 0;
	}

	if (opt.dump) {
		program p = generate_program(opt.seed, opt.max_depth, opt.band, opt.math_band, opt.call_args_band, opt.blocks, opt.local_alias_band, opt.regex_band);
		std::cout << p.joined();
		return 0;
	}

	if (!opt.run_file.empty() && opt.census) return run_census(opt, 0, 1);

	if (!opt.run_file.empty()) {
		program p;
		if (!read_program_file(opt, p)) return 2;
		compare_result cr = compare_program(p, opt.timeout_ms / 1000.0, opt.proofs, opt.param_alias, opt.passes, opt.miss_every, opt.lowering, opt.vm_reference);
		if (cr.kind == compare_result::diverge) {
			std::cout << "DIVERGE\n" << cr.diff;
			for (size_t i = 0; i < cr.interp.segment_results.size(); ++i)
				std::cout << "segment " << i << " " << left_label(opt.vm_reference) << ": " << cr.interp.segment_results[i] << "\n"
				          << "segment " << i << " " << right_label(opt.vm_reference) << ": " << cr.vm.segment_results[i] << "\n";
			return 1;
		}
		std::cout << (cr.kind == compare_result::budget_skip ? "BUDGET-SKIP\n" : "AGREE\n");
		for (size_t i = 0; i < cr.interp.segment_results.size(); ++i)
			std::cout << "segment " << i << ": " << cr.interp.segment_results[i] << "\n";
		if (!cr.interp.printed.empty()) std::cout << "printed:\n" << cr.interp.printed;
		return 0;
	}

	if (opt.child) {
		// No-progress watchdog: a segment that never reaches a budget check (or hangs in native code) must not stall the run.
		// Once no segment has finished for the guard the child exits with the HANG code and the parent reports the seed; a
		// slow program that keeps finishing segments never trips it
		const auto guard = std::chrono::milliseconds(static_cast<long long>(std::max(10000.0, opt.timeout_ms * 40.0)));
		std::thread([guard]() {
			jai::foundry::stall_watch watch(guard);
			while (!watch.stalled(segments_finished.load(std::memory_order_relaxed))) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
			std::fflush(stdout);
			std::_Exit(23);
		}).detach();
		if (opt.jaibite_mutate) return run_mutation_seed(opt, opt.seed);
		order_stats child_stats;
		verdict v = opt.order ? run_order_seed(opt, opt.seed, false, child_stats) : run_seed(opt, opt.seed, false);
		switch (v) {
			case verdict::agree: return 0;
			case verdict::budget_skip: return 21;
			case verdict::diverge_suppressed: return 22;
			case verdict::diverge_new: return 20;
		}
		return 0;
	}

	uint64_t begin = opt.seed_given ? opt.seed : opt.seed_start;
	uint64_t end = opt.seed_given ? opt.seed + 1 : opt.seed_start + opt.iterations;
	if (opt.parallel) { return run_parallel_band(begin, end, opt.seed_given); }
	if (opt.census) return run_census(opt, begin, end);

	if (opt.jaibite_mutate) {
		uint64_t accepted = 0, rejected = 0, findings = 0, crashes = 0, hangs = 0;
		for (uint64_t seed = begin; seed < end; ++seed) {
			if ((seed - begin) % 100 == 0) {
				std::cout << "[jaibite progress] next mutant " << seed << " | accepted " << accepted
				          << " | rejected " << rejected << " | findings " << findings
				          << " | crashes " << crashes << " | hangs " << hangs << "\n";
				std::cout.flush();
			}
			const int code = run_isolated(opt, seed);
			switch (code) {
				case 0: ++accepted; break;
				case 24: ++rejected; break;
				case 20: ++findings; break;
				case 23: ++hangs; break;
				default: ++crashes; break;
			}
			if (code != 0 && code != 24) {
				std::cout << "[jaibite mutant " << seed << "] child exit " << code << "\n"
				          << "reproduce: jaiscript_fuzz --jaibite-mutate --seed " << seed
				          << " --max-depth " << opt.max_depth << " --timeout-ms " << opt.timeout_ms
				          << (opt.proofs ? "" : " --no-proofs")
				          << (opt.param_alias ? "" : " --no-param-alias") << (opt.passes ? "" : " --no-passes")
				          << (opt.lowering == 1 ? " --no-lowering" : opt.lowering == 2 ? " --lowered-interp" : "") << "\n";
				std::cout.flush();
			}
		}
		std::cout << "\n===== jaibite mutation summary (loader only; BOTH backends per mutant) =====\n"
		          << "requested: " << (end - begin) << " | accepted: " << accepted
		          << " | catchable identical rejections: " << rejected
		          << " | findings: " << findings << " | crashes: " << crashes << " | hangs: " << hangs << "\n"
		          << "valid-original byte-stable loads: " << 2 * (accepted + rejected) << "\n";
		return (findings || crashes || hangs || accepted + rejected != end - begin) ? 1 : 0;
	}

	uint64_t agree = 0, new_div = 0, suppressed_div = 0, budget = 0, crashes = 0, hangs = 0, parse_valid = 0;
	order_stats ost;
	std::vector<uint64_t> new_div_seeds, crash_seeds, suppressed_seeds, hang_seeds;
	auto last_progress = std::chrono::steady_clock::now();

	for (uint64_t seed = begin; seed < end; ++seed) {
		{
			// Parse-validity metric (jaibite throws on hard parse errors; recoverable
			// ones synchronize silently, so this is a lower bound on brokenness only).
			program p = opt.order ? order::generate(seed, opt.order_args).original : generate_program(seed, opt.max_depth, opt.band, opt.math_band, opt.call_args_band, opt.blocks, opt.local_alias_band, opt.regex_band);
			bool ok = true;
			try {
				auto probe = jai::engine::make();
				for (const auto& s : p.segments) (void)probe->jaibite(jai::fuzz::k_warn_region_prefix + s);
			} catch (...) { ok = false; }
			if (ok) ++parse_valid;
		}
		// at least once a minute too: a gate's watchdog (tools/ci/jai_gate_run --idle) takes a long silence for a hang
		if (!opt.seed_given && seed != begin &&
			((seed - begin) % 500 == 0 || std::chrono::steady_clock::now() - last_progress >= std::chrono::minutes(1))) {
			last_progress = std::chrono::steady_clock::now();
			std::cout << "[progress] seed " << seed << " | agree " << agree << " | new-div " << new_div
			          << " | suppressed " << suppressed_div << " | budget " << budget << " | crashes " << crashes << "\n";
			std::cout.flush();
		}
		if (opt.isolate) {
			// Print BEFORE running so a child crash is attributable.
			int code = run_isolated(opt, seed);
			switch (code) {
				case 0: ++agree; break;
				case 20: ++new_div; new_div_seeds.push_back(seed); break;
				case 21: ++budget; break;
				case 22: ++suppressed_div; suppressed_seeds.push_back(seed); break;
				case 23:
					++hangs;
					hang_seeds.push_back(seed);
					std::cout << "\n=== HANG seed " << seed << " (child killed by its watchdog) ===\n";
					std::cout << "reproduce: jaiscript_fuzz --child --seed " << seed
					          << " --max-depth " << opt.max_depth << (opt.vm_reference ? " --vm-reference" : "") << "\n";
					break;
				default:
					++crashes;
					crash_seeds.push_back(seed);
					std::cout << "\n=== CRASH seed " << seed << " (child exit code " << code << ") ===\n";
					std::cout << "reproduce: jaiscript_fuzz --child --seed " << seed
					          << " --max-depth " << opt.max_depth << (opt.vm_reference ? " --vm-reference" : "") << "\n";
					break;
			}
		} else {
			// In-process: a crash kills the run; the marker line below makes the
			// dying seed attributable so a driver can resume at seed+1.
			std::cout << "[running seed " << seed << "]\r";
			std::cout.flush();
			verdict v = opt.order ? run_order_seed(opt, seed, opt.seed_given, ost) : run_seed(opt, seed, opt.seed_given);
			switch (v) {
				case verdict::agree: ++agree; break;
				case verdict::diverge_new: ++new_div; new_div_seeds.push_back(seed); break;
				case verdict::diverge_suppressed: ++suppressed_div; suppressed_seeds.push_back(seed); break;
				case verdict::budget_skip: ++budget; break;
			}
		}
	}

	std::cout << "\n===== run summary" << (opt.vm_reference ? " (vm against vm-ref)" : "") << " =====\n";
	std::cout << "seeds:          " << begin << ".." << (end - 1) << " (" << (end - begin) << " programs)\n";
	std::cout << "parse-valid:    " << parse_valid << " (" << (100.0 * parse_valid / (end - begin)) << "%)\n";
	std::cout << "agree:          " << agree << "\n";
	std::cout << "new divergences:" << new_div;
	if (!new_div_seeds.empty()) {
		std::cout << "  seeds:";
		for (auto s : new_div_seeds) std::cout << " " << s;
	}
	std::cout << "\nsuppressed:     " << suppressed_div;
	if (!suppressed_seeds.empty() && suppressed_seeds.size() <= 50) {
		std::cout << "  seeds:";
		for (auto s : suppressed_seeds) std::cout << " " << s;
	}
	std::cout << "\nbudget-skips:   " << budget << "\n";
	if (!opt.isolate) {   // the children lower; their counts do not come back
		std::cout << "lowered locals: " << opt.lowered_locals << " in " << opt.lowered_programs << " programs (optimized vm side)\n";
	}
	std::cout << "crashes:        " << crashes;
	if (!crash_seeds.empty()) {
		std::cout << "  seeds:";
		for (auto s : crash_seeds) std::cout << " " << s;
	}
	std::cout << "\nhangs:          " << hangs;
	if (!hang_seeds.empty()) {
		std::cout << "  seeds:";
		for (auto s : hang_seeds) std::cout << " " << s;
	}
	std::cout << "\n";
	if (opt.order && !opt.isolate) {
		std::cout << "order band:     programs " << ost.programs << " | backends agree " << ost.backends << " | metamorphic agree " << ost.metamorphic
		          << " | model checked " << ost.model_checked << " (agree " << ost.model_agree << ", abstained " << ost.abstained << ")\n";
	}
	return (new_div > 0 || crashes > 0 || hangs > 0) ? 1 : 0;
}
