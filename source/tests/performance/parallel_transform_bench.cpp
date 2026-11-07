// Parallel Transform scaling benchmarks.
// Same body as a serial for-loop baseline, then parallel_transform at 1/2/4/8 workers.
// Foundry reports integer µs/iteration with ±50% variance: compare
// min-of-3 whole-suite runs and note machine load; the SHAPE of the scaling table is
// the claim, not sub-µs deltas.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <map>
#include <optional>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class parallel_transform_bench : public suite {
public:
	parallel_transform_bench() : suite("Parallel Transform Bench") {}

	std::shared_ptr<jai::engine> test_engine;
	std::map<std::string, jai::jaibite> bites;

	void pre_test() override {
		test_engine = make_engine();
		// The 100k serial row runs 323-460ms honest and machine contention stretches it without
		// bound, so measurement suites disable the budget like the perf twins do.
		test_engine->execution_budget(0);
		jai::stdlib::register_all(test_engine);
		bites.clear();
		// Inputs + bodies once; the benchmark iterations re-run only the call itself.
		test_engine->execute(R"(
			var a_math = [];
			for (var i = 0; i < 100000; i++) { a_math.push(0.5 + i * 0.173); }
			float f_math(float x) { return math::sqrt(x) * 2.0 + math::sqrt(x + 1.0) - math::pow(x, 0.25); }

			var a_small = [];
			for (var i = 0; i < 1000; i++) { a_small.push(0.5 + i * 0.173); }

			var a_str = [];
			for (var i = 0; i < 10000; i++) { a_str.push("payload_" + to_string(i * 37) + "_tail"); }
			string f_str(string s) {
				var t = s + "|" + to_string(s.size() * 3);
				return t.substr(0, t.size() % 11 + 6) + to_string(t.size());
			}

			// Captured-read grid consumption (the GLOOM glyph shape): a shared read-only
			// grid consumed by row, elements are just row indices - the grid rides the
			// borrow tier (all-primitive + subscript-only reads => zero-copy raw reads)
			var grid = [];
			for (var i = 0; i < 65536; i++) { grid.push((i * 2654435) % 65536); }
			var gw = 256;
			int f_row(int row) {
				var acc = 0;
				for (var x = 0; x < gw; x++) { acc = (acc * 31 + grid[row * gw + x]) % 1000003; }
				return acc;
			}
			var grid_rows = [];
			for (var r = 0; r < 256; r++) { grid_rows.push(r); }
		)");
	}

	void run_bite(const char* key, const char* source) {
		auto it = bites.find(key);
		if (it == bites.end()) {
			it = bites.emplace(key, test_engine->jaibite(source)).first;
		}
		it->second.execute();
	}

	void bench_at(const char* name, size_t workers, const char* key, const char* source) {
		bench_at_n(name, workers, key, source, 3);
	}

	void bench_at_n(const char* name, size_t workers, const char* key, const char* source, int32_t iterations) {
		benchmark(name, [this, workers, key, source]() {
			test_engine->parallel_thread_count(workers);
			run_bite(key, source);
		}, iterations);
	}

	void forge_tests() override {
		static constexpr const char* k_math_serial =
			"var out = []; for (auto x : a_math) { out.push(f_math(x)); } out.size();";
		static constexpr const char* k_math_parallel =
			"parallel_transform(a_math, f_math).size();";
		static constexpr const char* k_str_serial =
			"var out = []; for (auto x : a_str) { out.push(f_str(x)); } out.size();";
		static constexpr const char* k_str_parallel =
			"parallel_transform(a_str, f_str).size();";

		benchmark("Math 100k doubles - serial for", [this]() {
			run_bite("ms", k_math_serial);
		}, 3);
		bench_at("Math 100k doubles - parallel W=1", 1, "mp", k_math_parallel);
		bench_at("Math 100k doubles - parallel W=2", 2, "mp", k_math_parallel);
		bench_at("Math 100k doubles - parallel W=4", 4, "mp", k_math_parallel);
		bench_at("Math 100k doubles - parallel W=8", 8, "mp", k_math_parallel);

		// Small-array row: context ACQUISITION dominates here - with slot reuse the
		// per-call overhead collapses to the reset + detach of 1k doubles. More
		// iterations: each call is short.
		static constexpr const char* k_small_serial =
			"var out = []; for (auto x : a_small) { out.push(f_math(x)); } out.size();";
		static constexpr const char* k_small_parallel =
			"parallel_transform(a_small, f_math).size();";
		benchmark("Math 1k doubles - serial for", [this]() {
			run_bite("sms", k_small_serial);
		}, 20);
		bench_at_n("Math 1k doubles - parallel W=1", 1, "smp", k_small_parallel, 20);
		bench_at_n("Math 1k doubles - parallel W=4", 4, "smp", k_small_parallel, 20);
		bench_at_n("Math 1k doubles - parallel W=8", 8, "smp", k_small_parallel, 20);

		benchmark("String 10k - serial for", [this]() {
			run_bite("ss", k_str_serial);
		}, 3);
		bench_at("String 10k - parallel W=1", 1, "sp", k_str_parallel);
		bench_at("String 10k - parallel W=2", 2, "sp", k_str_parallel);
		bench_at("String 10k - parallel W=4", 4, "sp", k_str_parallel);
		bench_at("String 10k - parallel W=8", 8, "sp", k_str_parallel);

		// Captured reads: 256 rows x 256 borrowed-grid element reads per row (65k reads
		// total). Serial baseline runs the same fn over the same rows in a plain loop.
		static constexpr const char* k_grid_serial =
			"var out = []; for (auto r : grid_rows) { out.push(f_row(r)); } out.size();";
		static constexpr const char* k_grid_parallel =
			"parallel_transform(grid_rows, f_row).size();";
		benchmark("Captured grid 256x256 - serial for", [this]() {
			run_bite("gs", k_grid_serial);
		}, 5);
		bench_at_n("Captured grid 256x256 - parallel W=1", 1, "gp", k_grid_parallel, 5);
		bench_at_n("Captured grid 256x256 - parallel W=4", 4, "gp", k_grid_parallel, 5);
		bench_at_n("Captured grid 256x256 - parallel W=8", 8, "gp", k_grid_parallel, 5);
	}
};

} // namespace jai::foundry::tests

using parallel_transform_bench = jai::foundry::tests::parallel_transform_bench;
FOUNDRY_REGISTER(parallel_transform_bench)
