// Seeded differential-fuzz smoke suite: every CI run fuzzes a little.
// Runs a fixed range of generator seeds through the same harness as
// jaiscript_fuzz and asserts zero divergences outside the suppressions of fuzz_harness.hpp.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>

#include "fuzz/fuzz_census.hpp"
#include "fuzz/fuzz_generator.hpp"
#include "fuzz/fuzz_harness.hpp"
#include "fuzz/fuzz_jaibite.hpp"
#include "fuzz/fuzz_order_band.hpp"

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class fuzz_smoke_tests : public suite {
public:
	fuzz_smoke_tests() : suite("Differential Fuzz Smoke") {}

	void run_seed_range(uint64_t k_first_seed, uint64_t k_seed_count, bool proofs = true, fuzz::band_mode band = fuzz::band_mode::never,
	                    fuzz::band_mode math = fuzz::band_mode::never, int lowering = 0, fuzz::band_mode call_args = fuzz::band_mode::never,
	                    fuzz::band_mode blocks = fuzz::band_mode::never) {
			constexpr double k_budget_seconds = 0.1; // budget-skips cost 2x this; keeps the suite fast

			int divergences = 0;
			std::string first_report;
			for (uint64_t seed = k_first_seed; seed < k_first_seed + k_seed_count; ++seed) {
				fuzz::program p = fuzz::generate_program(seed, 3, band, math, call_args, blocks);
				fuzz::compare_result cr = fuzz::compare_program(p, k_budget_seconds, proofs, true, true, 0, lowering);
				if (cr.kind != fuzz::compare_result::diverge) continue;
				if (!fuzz::match_suppression(p, cr.diff).empty()) continue;
				// Rare path: some known shapes are only recognizable minimized
				// (e.g. eval-order divergences buried in printed-output noise).
				{
					fuzz::minimize_stats stats;
					fuzz::program reduced = fuzz::minimize(p, [&](const fuzz::program& cand) {
						return fuzz::compare_program(cand, k_budget_seconds, proofs, true, true, 0, lowering).kind == fuzz::compare_result::diverge;
					}, stats, 150);
					fuzz::compare_result mc = fuzz::compare_program(reduced, k_budget_seconds, proofs, true, true, 0, lowering);
					if (mc.kind == fuzz::compare_result::diverge && !fuzz::match_suppression(reduced, mc.diff).empty())
						continue;
				}
				++divergences;
				if (first_report.empty()) {
					first_report = "fuzz seed " + std::to_string(seed) +
						" diverged (repro: jaiscript_fuzz --seed " + std::to_string(seed) +
						" --max-depth 3 --minimize" + (proofs ? "" : " --no-proofs") +
						(band == fuzz::band_mode::always ? " --element-band" : "") + (math == fuzz::band_mode::always ? " --math-band" : "") +
							(call_args == fuzz::band_mode::always ? " --call-args-band" : "") + (blocks == fuzz::band_mode::always ? " --block-shapes" : "") +
							(lowering == 1 ? " --no-lowering" : lowering == 2 ? " --lowered-interp" : "") + ")\n" + cr.diff;
				}
			}
			check_eq(0, divergences, first_report.empty() ? "no divergences" : first_report);
	}

	void forge_tests() override {
		test("jaibite_mutator_is_deterministic_and_never_a_noop", [this]() {
			// Construction only: random malformed trees belong in isolated children,
			// never in the suite process. Safe to run before the validator is fixed.
			for (uint64_t seed = 0; seed < 16; ++seed) {
				const auto first = fuzz::make_jaibite_mutant(seed, 3);
				const auto again = fuzz::make_jaibite_mutant(seed, 3);
				check_true(first.bytes != first.original, "each seed counts an actual byte mutation");
				check_true(first.original == again.original, "valid original bytes are deterministic");
				check_true(first.bytes == again.bytes, "mutant bytes are deterministic");
				check_eq(first.source_seed, again.source_seed);
				check_eq(first.offset, again.offset);
				check_eq(first.operation, again.operation);
				for (bool use_vm : { false, true }) {
					const auto valid = fuzz::load_mutant_bytes(first.original, use_vm, true);
					check_true(valid.accepted, valid.loader_error + valid.unexpected_error);
					check_true(valid.stable_bytes, "valid generator bytes round-trip without rewriting");
					check_true(valid.alive, "engine remains usable after valid load");
				}
			}
		});

		// Oracle self-test (review R8): unequal values of every representation must describe
		// differently, including typed arrays (the old as_array() view raised on them and gave
		// [1] and [2] the same error text), entries past the display cap, and deep nesting
		test("oracle_tells_unequal_values_apart", [this]() {
			auto eng = jai::foundry::make_engine();
			auto d = [&](const char* src) { return fuzz::describe_value(eng->execute(src)); };
			check_ne(d("array<int> a = [1]; a;"), d("array<int> a = [2]; a;"), "typed int arrays");
			check_ne(d("array<float> a = [1.0]; a;"), d("array<float> a = [1.5]; a;"), "typed float arrays");
			check_ne(d("array<string> a = [\"x\"]; a;"), d("array<string> a = [\"y\"]; a;"), "typed string arrays");
			check_ne(d("array<bool> a = [true]; a;"), d("array<bool> a = [false]; a;"), "typed bool arrays");
			check_ne(d("array<int> a = [1]; a;"), d("var a = [1]; a;"), "typed vs var representation");
			check_ne(d("var a = []; for (int i = 0; i < 100; i++) { a.push(i); } a;"),
			         d("var a = []; for (int i = 0; i < 100; i++) { a.push(i); } a[99] = -1; a;"), "past the display cap");
			check_ne(d("var a = 1; for (int i = 0; i < 10; i++) { a = [a]; } a;"),
			         d("var a = 2; for (int i = 0; i < 10; i++) { a = [a]; } a;"), "deep nesting");
			check_ne(d("map<string, int> m = {\"a\": 1}; m;"), d("map<string, int> m = {\"a\": 2}; m;"), "maps");
			check_eq(d("array<int> a = [1, 2]; a;"), d("array<int> b = [1, 2]; b;"), "equal typed arrays describe alike");
			check_true(d("array<int> a = [7]; a;").find("array<int>[int:7]") != std::string::npos, "typed elements are spelled out");
		});

		test("fixed_seeds_no_new_divergence", [this]() {
			// Seed count is budgeted to keep this suite fast; bump it as the
			// engines get faster. Seeds here are FIXED - failures reproduce with
			// jaiscript_fuzz --seed <N>.
			run_seed_range(0, 40);
		});
		test("typed_kind_seeds_no_new_divergence", [this]() {
			// A second fixed band: the generator now draws typed buffer declarations
			// (int/float/string/bool/char) half the time an array is declared
			run_seed_range(1000, 120);
		});
		test("element_argument_band_seeds_no_new_divergence", [this]() {
			// every program carries the element-argument band: mixed-kind element reads as several
			// operands of one call inside loops the vm closes into cell blocks
			run_seed_range(80, 120, true, fuzz::band_mode::always);   // 89, 97 and 192 caught the argument-zone miss
		});
		test("math_band_seeds_no_new_divergence", [this]() {
			// every program carries the math band: math::vec2/vec3/ivec2/ivec3 through every lvalue kind, in and out of functions
			run_seed_range(300, 60, true, fuzz::band_mode::never, fuzz::band_mode::always);
			run_seed_range(300, 30, false, fuzz::band_mode::never, fuzz::band_mode::always);
			// by default the interpreter runs boxed and the vm lowered (the lowering's differential); both lowered and both boxed too
			run_seed_range(300, 40, true, fuzz::band_mode::never, fuzz::band_mode::always, 2);
			run_seed_range(300, 40, true, fuzz::band_mode::never, fuzz::band_mode::always, 1);
		});
		test("regex_band_seeds_no_new_divergence", [this]() {
			// every program carries the regex band: generated patterns through every regex call, literal and built at run time
			using fuzz::band_mode;
			for (uint64_t seed = 500; seed < 560; ++seed) {
				const fuzz::program p = fuzz::generate_program(seed, 3, band_mode::never, band_mode::never, band_mode::never, band_mode::never, band_mode::never, band_mode::always);
				const fuzz::compare_result cr = fuzz::compare_program(p, 5.0);
				check_true(cr.kind != fuzz::compare_result::diverge, "regex band seed " + std::to_string(seed) + ": " + cr.diff);
			}
		});
		test("a_budget_spent_inside_a_block_ends_the_run", [this]() {
			// seed 2004797 with the call-argument band (jaiscript_fuzz's default rolls otherwise): a loop whose body is a closed inner loop ran
			// past every budget on the vm while the interpreter raised at once; both backends must end the run with the same outcome
			using fuzz::band_mode;
			const fuzz::program p = fuzz::generate_program(2004797, 4, band_mode::sometimes, band_mode::sometimes, band_mode::always, band_mode::sometimes, band_mode::sometimes);
			const fuzz::compare_result cr = fuzz::compare_program(p, 0.25);
			check_true(cr.kind != fuzz::compare_result::diverge, "seed 2004797 agrees under the budget: " + cr.diff);
		});
		test("seeds_5792_and_9516_end_under_the_step_budget", [this]() {
			// --max-depth 3 (the CI gate's depth): an endless loop around a closed loop and a host call ran past the budget on the vm in most runs
			using fuzz::band_mode;
			for (uint64_t seed : { 5792ull, 9516ull }) {
				const fuzz::program p = fuzz::generate_program(seed, 3, band_mode::sometimes, band_mode::sometimes, band_mode::sometimes, band_mode::sometimes, band_mode::sometimes);
				for (bool vm : { false, true }) {
					for (uint64_t steps : { 1ull, 7ull, 64ull }) {
						fuzz::run_config cfg;
						cfg.use_vm = vm;
						cfg.budget_seconds = 30.0;
						cfg.step_budget = steps;
						const fuzz::run_outcome out = fuzz::run_program(p, cfg);
						check_true(out.budget_hit && out.alive, "seed " + std::to_string(seed) + (vm ? " vm" : " interpreter") + " step budget " + std::to_string(steps));
					}
				}
			}
		});
		test("call_args_band_seeds_no_new_divergence", [this]() {
			// every program carries the call-argument band: element reads of different typed arrays as the arguments of one kept call
			// in a counted loop (a block's copied read resolving another argument's array diverged in 239 of 500 band seeds)
			run_seed_range(0, 40, true, fuzz::band_mode::never, fuzz::band_mode::never, 0, fuzz::band_mode::always);
			run_seed_range(0, 20, false, fuzz::band_mode::never, fuzz::band_mode::never, 0, fuzz::band_mode::always);
		});
		test("block_shapes_seeds_no_new_divergence", [this]() {
			// every program carries the block shapes: the main program's statements inside a function among closable counted loops,
			// the math band with a lowered-math loop (repro: jaiscript_fuzz --seed <N> --max-depth 3 --block-shapes)
			run_seed_range(0, 40, true, fuzz::band_mode::never, fuzz::band_mode::never, 0, fuzz::band_mode::never, fuzz::band_mode::always);
			run_seed_range(500, 30, true, fuzz::band_mode::never, fuzz::band_mode::always, 0, fuzz::band_mode::never, fuzz::band_mode::always);
		});
		test("every_part_closes_blocks", [this]() {
			// the share of each part's programs whose vm run entered a closed cell block (a compiled closed loop and a block entry;
			// jaiscript_fuzz --census reports it per part): a part whose loops never close is blind to every bug that fires only
			// inside a block (the call-argument miss of 71ea148c survived every band whose loops stayed open). Floors sit well under
			// the measured shares (in brackets) so a generator or compiler change that opens a part's loops fails here
			using fuzz::band_mode;
			struct row { const char* label; band_mode element, math, call_args, blocks; double floor; };
			const row rows[] = {
				{"main", band_mode::never, band_mode::never, band_mode::never, band_mode::always, 50.0},   // [80%]
				{"element", band_mode::always, band_mode::never, band_mode::never, band_mode::never, 20.0},   // [40%]
				{"math", band_mode::never, band_mode::always, band_mode::never, band_mode::always, 35.0},      // [62.5%]
				{"call-args", band_mode::never, band_mode::never, band_mode::always, band_mode::never, 50.0},   // [80%]
			};
			for (const row& r : rows) {
				uint64_t segments = 0;
				const double share = fuzz::census::closed_block_share(r.label, 0, 40, r.element, r.math, r.call_args, r.blocks, 0.1, &segments);
				check_eq(uint64_t(40), segments, std::string(r.label) + ": one segment per program");
				check_ge(share, r.floor, std::string(r.label) + " programs entering a closed block: " + std::to_string(share) + "%");
			}
			const double order = fuzz::census::order_closed_block_share(0, 40, false, 0.1);   // [30%]
			check_ge(order, 15.0, "order band programs entering a closed block: " + std::to_string(order) + "%");
		});
		test("fixed_seeds_no_new_divergence_no_proofs", [this]() {
			// interp-vs-vm parity must hold in both proof states (docs/site/embedding/06-static-checking.html "Proofs")
			run_seed_range(0, 40, false);
		});
		test("order_band_seeds_hold_three_oracles", [this]() {
			// writing calls beside reads of what they write: the backends agree, the program agrees with its metamorphic variant, and
			// the output is the spec model's (repro: jaiscript_fuzz --order-band --seed <N>)
			int problems = 0;
			std::string first;
			for (uint64_t seed = 0; seed < 40; ++seed) {
				const std::string p = fuzz::order::order_seed_problem(seed, 0.1);
				if (p.empty()) continue;
				++problems;
				if (first.empty()) first = "order band seed " + std::to_string(seed) + "\n" + p;
			}
			check_eq(0, problems, first.empty() ? "no problems" : first);
		});
		test("an_argument_read_ahead_of_a_writing_sibling_holds_its_value", [this]() {
			// a math pair whose first argument a call in the second writes, inside a loop (repro: jaiscript_fuzz --order-args --seed 2051052)
			const fuzz::order::generated g = fuzz::order::generate(2051052, true);
			const fuzz::compare_result cr = fuzz::compare_program(g.original, 0.25);
			check_true(cr.kind != fuzz::compare_result::diverge, "order-args seed 2051052:\n" + cr.diff);
			if (g.model_ok && cr.kind == fuzz::compare_result::agree) { check_eq(g.expected, cr.vm.printed, "order-args seed 2051052: the model"); }
		});
	}
};

} // namespace jai::foundry::tests

using fuzz_smoke_tests = jai::foundry::tests::fuzz_smoke_tests;
FOUNDRY_REGISTER(fuzz_smoke_tests)
