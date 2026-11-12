// parallel_transform.
// Determinism: same inputs => identical output at every thread count, both backends.
// Admission violations ERROR with location text - never silently serial.

#include <jaiscript/testing/foundry.hpp>
#include "../case_table.hpp"
#include <map>
#include <set>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/core/class_registry.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/detail/parallel_transform.hpp>   // parallel_capture_kind (classification pins)
#include <jaiscript/stdlib/stdlib.hpp>
#include "fuzz/fuzz_parallel_band.hpp"
#include <jaiscript/detail/engine_access.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/stdlib/spider.hpp>
#include <jaiscript/stdlib/web_text.hpp>
#include <jaiscript/core/jaibite.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/thread_pool.hpp>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <new>
#include <thread>
#include <atomic>
#include <unordered_set>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// Host objects the hybrid test makes and destroys, from any worker
inline std::atomic<int64_t> parallel_hosts_made{0};
inline std::atomic<int64_t> parallel_hosts_died{0};
// C++ objects of the deferral test, in the order they die (one thread)
inline std::vector<int64_t> parallel_serial_deaths;

class parallel_transform_tests : public suite {
public:
	parallel_transform_tests() : suite("Parallel Transform") {}

	// Deep structural equality: script_value operator== compares container types by
	// held-pointer identity, so nested arrays/maps need an element-wise recursion.
	static bool deep_equal(const script_value& a, const script_value& b) {
		if (a.is_array() && b.is_array()) {
			const auto& av = a.as_array();
			const auto& bv = b.as_array();
			if (av.size() != bv.size()) { return false; }
			for (size_t i = 0; i < av.size(); ++i) {
				if (!deep_equal(av[i], bv[i])) { return false; }
			}
			return true;
		}
		if (a.is_map() && b.is_map()) {
			const auto& am = a.as_map();
			const auto& bm = b.as_map();
			if (am.size() != bm.size()) { return false; }
			auto bi = bm.begin();
			for (auto ai = am.begin(); ai != am.end(); ++ai, ++bi) {
				if (!deep_equal(ai->first, bi->first) || !deep_equal(ai->second, bi->second)) { return false; }
			}
			return true;
		}
		return a == b;
	}

	static bool arrays_equal(const script_value& a, const script_value& b) {
		return deep_equal(a, b);
	}

	// Runs `source` (which must return {parallel, serial} results as a two-element
	// array) at several worker counts and checks parallel == serial for each.
	void check_matches_serial_at(std::shared_ptr<engine> e, const std::string& source,
	                             std::initializer_list<size_t> worker_counts) {
		for (size_t workers : worker_counts) {
			e->parallel_thread_count(workers);
			auto r = e->execute(source);
			check_true(r.is_array());
			const auto& pair = r.as_array();
			check_eq((size_t)2, pair.size());
			check_true(arrays_equal(pair[0], pair[1]));
		}
	}

	void forge_tests() override {
		test("worker_global_reads_through_blocks_see_a_redefinition_between_regions", [this]() {
			// a worker's frames find their chunk's lookup rows once: globals read through cell blocks (entry cells, container sites)
			// and typed operands must see a captured global the caller rebinds between two regions
			auto e = make_engine();
			const std::string src = R"(
				array<float> G = [1.0, 2.0, 3.0, 4.0];
				float K = 2.0;
				int N = 4;
				function row(int y) -> float {
					float s = 0.0;
					for (int i = 0; i < N; ++i) { s += G[i] * K + y; }
					return s;
				}
				array<int> ys = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11];
				var pa = parallel_transform(ys, row);
				var sa = []; for (auto y : ys) { sa.push(row(y)); }
				K = 3.0;
				G = [10.0, 20.0, 30.0, 40.0];
				var pb = parallel_transform(ys, row);
				var sb = []; for (auto y : ys) { sb.push(row(y)); }
				[[pa, pb], [sa, sb]];
			)";
			check_matches_serial_at(e, src, {1, 2, 4});
			e->parallel_thread_count(2);
			check_true(e->execute(src + "pb[11] == 344.0;").as_bool(), "the second region reads the rebound globals");
		});
		// every body shape (strings, float math, nested containers, recursion, captured scalars, maps and read-only methods,
		// a weight fn) lands what the serial loop lands at every worker count
		test("parallel_transform_lands_what_the_serial_loop_lands", [this]() {
			struct serial_case { const char* code; std::vector<size_t> workers; bool stdlib = false; };
			const serial_case cases[] = {
				// each call pushes a frame of the other chunk: its first lookup has no rows yet, a reused record carries another chunk's
				{R"(
				array<float> G = [0.5, 1.5, 2.5];
				int N = 3;
				function fa(float x) -> float { float s = 0.0; for (int i = 0; i < N; ++i) { s += G[i] * x; } return s; }
				function fb(float x) -> float { float s = 1.0; for (int i = 0; i < N; ++i) { s += G[i] - x; } return s; }
				function row(int y) -> float {
					float t = 0.0;
					for (int k = 0; k < 6; ++k) { if (k % 2 == 0) { t += fa(t * 0.25 + y); } else { t += fb(t * 0.5 - y); } }
					return t;
				}
				array<int> ys = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
				var p = parallel_transform(ys, row);
				var s = []; for (auto y : ys) { s.push(row(y)); }
				[p, s];
				)", {1, 2, 4}},
				{R"(
				var a = [];
				for (var i = 0; i < 200; i++) { a.push(i); }
				int f(int x) { return x * 3 - 1; }
				var pout = parallel_transform(a, f);
				var sout = [];
				for (auto x : a) { sout.push(f(x)); }
				return [pout, sout];
				)", {1, 2, 8}},
				{R"(
				var a = [];
				for (var i = 0; i < 64; i++) { a.push("item_" + to_string(i * 7)); }
				string f(string s) {
					var upper_len = s.size() * 2;
					return s.substr(0, 4) + "|" + to_string(upper_len);
				}
				var pout = parallel_transform(a, f);
				var sout = [];
				for (auto x : a) { sout.push(f(x)); }
				return [pout, sout];
				)", {1, 2, 8}, true},
				{R"(
				var a = [];
				for (var i = 0; i < 128; i++) { a.push(1.0 + i * 0.37); }
				float f(float x) { return math::sqrt(x) * 2.0 + math::pow(x, 0.25) - math::floor(x); }
				var pout = parallel_transform(a, f);
				var sout = [];
				for (auto x : a) { sout.push(f(x)); }
				return [pout, sout];
				)", {1, 2, 8}, true},
				{R"(
				var a = [];
				for (var i = 0; i < 40; i++) { a.push([i, i * 2, i * 3]); }
				int f(var row) {
					var total = 0;
					for (auto& v : row) { total += v; }
					return total;
				}
				var pout = parallel_transform(a, f);
				var sout = [];
				for (auto x : a) { sout.push(f(x)); }
				return [pout, sout];
				)", {1, 4}},
				{R"(
				var a = [];
				for (var i = 0; i < 32; i++) { a.push({"x": i, "y": i * i}); }
				int f(var m) { return m["x"] + m["y"]; }
				var pout = parallel_transform(a, f);
				var sout = [];
				for (auto x : a) { sout.push(f(x)); }
				return [pout, sout];
				)", {1, 4}},
				{R"(
				int helper(int x) { return x % 7 + 1; }
				int fib(int n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }
				int f(int x) { return fib(helper(x)) * 10 + helper(x); }
				var a = [];
				for (var i = 0; i < 60; i++) { a.push(i); }
				var pout = parallel_transform(a, f);
				var sout = [];
				for (auto x : a) { sout.push(f(x)); }
				return [pout, sout];
				)", {1, 4}},
				{R"(
				var a = [];
				for (var i = 0; i < 24; i++) { a.push(i); }
				var f(int x) { return [x, x * x, "n" + to_string(x)]; }
				var pout = parallel_transform(a, f);
				var sout = [];
				for (auto x : a) { sout.push(f(x)); }
				return [pout, sout];
				)", {1, 4}, true},
				{R"(
				var scale = 4;
				var bias = 0.5;
				var enabled = true;
				var tag = "v";
				string f(int x) {
					var v = enabled ? x * scale + bias : 0.0;
					return tag + to_string(v);
				}
				var a = [];
				for (var i = 0; i < 48; i++) { a.push(i); }
				var pout = parallel_transform(a, f);
				var sout = [];
				for (auto x : a) { sout.push(f(x)); }
				return [pout, sout];
				)", {1, 2, 8}, true},
				// Primitive-keyed primitive map -> borrow-eligible; string keys -> snapshot
				{R"(
				var lut = {};
				lut[1] = 10; lut[2] = 20; lut[3] = 30; lut[4] = 40;
				var names = {"a": 1, "b": 2};
				int f(int x) {
					var hit = lut[x % 4 + 1];
					return hit + names["a"] + names["b"];
				}
				var a = [];
				for (var i = 0; i < 40; i++) { a.push(i); }
				var pout = parallel_transform(a, f);
				var sout = [];
				for (auto x : a) { sout.push(f(x)); }
				return [pout, sout];
				)", {1, 4}},
				// Nested containers (array of arrays) and by-VALUE iteration over a captured
				// container: both provision as snapshots and match serial exactly
				{R"(
				var table = [[1, 2, 3], [4, 5, 6], [7, 8, 9]];
				int f(int x) {
					var total = table[x % 3][x % 2] * 100;
					for (auto row : table) {
						for (auto v : row) { total += v; }
					}
					return total + table.size();
				}
				var a = [];
				for (var i = 0; i < 32; i++) { a.push(i); }
				var pout = parallel_transform(a, f);
				var sout = [];
				for (auto x : a) { sout.push(f(x)); }
				return [pout, sout];
				)", {1, 4}},
				// Read-only builtin methods on captured receivers are admitted (size/contains);
				// the weight fn runs at the barrier on the engine backend, so its enclosing
				// reads are plain global reads
				{R"(
				var pool = [5, 6, 7, 8];
				var heavy = 3.0;
				int f(int x) {
					if (pool.contains(x % 10)) { return x + pool.size(); }
					return x;
				}
				float w(int x) { return heavy; }
				var a = [];
				for (var i = 0; i < 40; i++) { a.push(i); }
				var pout = parallel_transform(a, f, w);
				var sout = [];
				for (auto x : a) { sout.push(f(x)); }
				return [pout, sout];
				)", {1, 4}},
			};
			check_each(cases, [this](const serial_case& c) {
				auto e = make_engine();
				if (c.stdlib) { jai::stdlib::register_all(*e); }
				for (size_t workers : c.workers) { check_matches_serial_at(e, c.code, {workers}); }
			});
		});

		// A worker body that copies its element string and mutates the copy: the copy-on-write
		// gate unshares on the worker (plain make_strong, no pool) and the element stays intact
		test("string_copy_mutation_in_body_matches_serial", [this]() {
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			const std::string src = R"(
				var a = [];
				for (var i = 0; i < 64; i++) { a.push("item_" + to_string(i)); }
				string f(string s) {
					string up = s;
					up.to_upper();
					up += "!";
					return s + "|" + up;
				}
				var pout = parallel_transform(a, f);
				var sout = [];
				for (auto x : a) { sout.push(f(x)); }
				return [pout, sout, a];
			)";
			for (size_t workers : {size_t(1), size_t(2), size_t(8)}) {
				e->parallel_thread_count(workers);
				auto r = e->execute(src);
				const auto& triple = r.as_array();
				check_eq((size_t)3, triple.size());
				check_true(arrays_equal(triple[0], triple[1]));
				check_eq(std::string("item_5|ITEM_5!"), triple[0].as_array()[5].as<std::string>());
				check_eq(std::string("item_5"), triple[2].as_array()[5].as<std::string>());
			}
		});

		test("byte_identical_across_thread_counts", [this]() {
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			const std::string src = R"(
				var a = [];
				for (var i = 0; i < 100; i++) { a.push(0.1 + i * 1.7); }
				float f(float x) { return math::sqrt(x) / (x + 0.5) * 3.14159; }
				return parallel_transform(a, f);
			)";
			e->parallel_thread_count(1);
			auto r1 = e->execute(src);
			e->parallel_thread_count(2);
			auto r2 = e->execute(src);
			e->parallel_thread_count(8);
			auto r8 = e->execute(src);
			check_true(arrays_equal(r1, r2));
			check_true(arrays_equal(r1, r8));
		});

		test("thread_count_builtin", [this]() {
			auto e = make_engine();
			auto r = e->execute("return thread_count();");
			check_ge(r.as_int(), (int64_t)1);
			e->parallel_thread_count(3);
			check_eq((int64_t)3, e->execute("return thread_count();").as_int());
		});

		test("performance_cores_lie_within_the_hardware_threads_and_stay_stable", [this]() {
			const size_t hardware = std::max<size_t>(1, std::thread::hardware_concurrency());
			const size_t cores = jai::thread_pool::performance_core_count();
			check_ge(cores, (size_t)1);
			check_true(cores <= hardware);
			check_eq(cores, jai::thread_pool::performance_core_count());
		});

		test("auto_thread_count_is_the_performance_cores_below_the_hardware_threads", [this]() {
			const size_t hardware = std::max<size_t>(1, std::thread::hardware_concurrency());
			const size_t automatic = std::max<size_t>(1, std::min(jai::thread_pool::performance_core_count(), hardware - 1));
			auto e = make_engine();
			check_eq(automatic, e->parallel_thread_count());
			check_eq((int64_t)automatic, e->execute("return thread_count();").as_int());
			e->parallel_thread_count(automatic + 5);
			check_eq(automatic + 5, e->parallel_thread_count());
			check_eq((int64_t)(automatic + 5), e->execute("return thread_count();").as_int());
			e->parallel_thread_count(1);
			check_eq((size_t)1, e->parallel_thread_count());
			e->parallel_thread_count(0);
			check_eq(automatic, e->parallel_thread_count());
			check_eq((int64_t)automatic, e->execute("return thread_count();").as_int());
		});

		test("empty_and_small_arrays", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(8);
			auto r = e->execute(R"(
				int f(int x) { return x + 1; }
				var empty_out = parallel_transform([], f);
				var tiny_out = parallel_transform([1, 2, 3], f);
				return [empty_out.size(), tiny_out[0], tiny_out[2]];
			)");
			const auto& vals = r.as_array();
			check_eq((int64_t)0, vals[0].as_int());
			check_eq((int64_t)2, vals[1].as_int());
			check_eq((int64_t)4, vals[2].as_int());
		});

		// === Admission: contract A - violations ERROR (never silent-serial) ===

		// a body the region cannot admit is refused with its verdict (the run returns the caught error text)
		test("parallel_transform_refuses_what_it_cannot_admit_with_its_verdict", [this]() {
			struct verdict_case { const char* code; std::vector<const char*> verdict; };
			const verdict_case cases[] = {
				{R"(
				var g = 0;
				int f(int x) { g = x; return x; }
				try { parallel_transform([1, 2, 3], f); } catch (e) { return e; }
				return "no-error";
				)", {"parallel_transform: body writes enclosing state 'g'", " at "}},
				{R"(
				int f(int x) { print(x); return x; }
				try { parallel_transform([1, 2, 3], f); } catch (e) { return e; }
				return "no-error";
				)", {"host function 'print' is marked jai::serial and cannot be called in a parallel body"}},
				// a coroutine's minted value is not a plain script function payload
				{R"(
				coroutine int f(int x) { yield x; return x; }
				try { parallel_transform([1, 2, 3], f); } catch (e) { return e; }
				return "no-error";
				)", {"parallel_transform"}},
				{R"(
				int f(int x) {
					auto g = [](int y) { return y * 2; };
					return g(x);
				}
				try { parallel_transform([1, 2, 3], f); } catch (e) { return e; }
				return "no-error";
				)", {"lambdas are not allowed in a parallel body"}},
				{R"(
				int f(int x) { var h = 5; return h(x); }
				try { parallel_transform([1, 2, 3], f); } catch (e) { return e; }
				return "no-error";
				)", {"call target 'h' is not statically resolvable"}},
				{R"(
				int g(int& x) { return x; }
				try { parallel_transform([1, 2, 3], g); } catch (e) { return e; }
				return "no-error";
				)", {"may not take its element by reference"}},
				{R"(
				int g(int y) { return y; }
				int f(int x) { parallel_transform([1], g); return x; }
				try { parallel_transform([1, 2, 3], f); } catch (e) { return e; }
				return "no-error";
				)", {"host function 'parallel_transform' is marked jai::serial and cannot be called in a parallel body"}},
				{R"(
				try { parallel_transform(5, thread_count); } catch (e) { return e; }
				return "no-error";
				)", {"first argument must be an array"}},
				{R"(
				try { parallel_transform([1, 2], 7); } catch (e) { return e; }
				return "no-error";
				)", {"fn must be a script-defined function"}},
			};
			check_each(cases, [](const verdict_case& c) {
				auto e = make_engine();
				const auto msg = e->execute(c.code).as<std::string>();
				for (const char* part : c.verdict) { check_true(msg.find(part) != std::string::npos, msg); }
			});
		});

		// === Captured reads (v0.5): enclosing state is READABLE, proven-read-only ===

		test("captured_grid_subscript_borrow", [this]() {
			auto e = make_engine();
			// The motivating shape: a flat all-primitive grid consumed by subscript reads.
			// On the vm this provisions as a BORROW (zero-copy raw reads); elsewhere as a
			// snapshot - semantics identical either way (pinned below).
			const std::string src = R"(
				var grid = [];
				for (var i = 0; i < 900; i++) { grid.push(i * 7 % 256); }
				var w = 30;
				int f(int row) {
					var total = 0;
					for (var x = 0; x < w; x++) { total += grid[row * w + x]; }
					return total * 2 + grid[row];
				}
				var rows = [];
				for (var r = 0; r < 30; r++) { rows.push(r); }
				var pout = parallel_transform(rows, f);
				var sout = [];
				for (auto r : rows) { sout.push(f(r)); }
				return [pout, sout];
			)";
			check_matches_serial_at(e, src, {1, 2, 8});
			// Classification pin: an all-primitive, subscript-only grid rides the borrow
			// tier (zero-copy raw reads) on BOTH backends; the scalar copies per worker
			const auto& caps = detail::engine_access::last_parallel_captures(*e);
			const uint64_t grid_id = detail::engine_access::symbolize(*e, "grid");
			const uint64_t w_id = detail::engine_access::symbolize(*e, "w");
			bool saw_grid = false, saw_w = false;
			for (const auto& [id, kind] : caps) {
				if (id == grid_id) {
					saw_grid = true;
					check_eq((int)detail::parallel_capture_kind::borrow, (int)kind);
				}
				if (id == w_id) {
					saw_w = true;
					check_eq((int)detail::parallel_capture_kind::scalar, (int)kind);
				}
			}
			check_true(saw_grid);
			check_true(saw_w);
		});

		test("captured_string_table_borrows", [this]() {
			// A string table touched only by subscript reads is a BORROW like a numeric one
			// (the glyph-palette workload): workers read the live root storage and a string
			// read mints the bytes, never the handle, so nothing is copied at the barrier.
			// Both the var table and the typed array<string> table take the tier
			for (const char* decl : {"var pal = [];", "array<string> pal = [];"}) {
				auto e = make_engine();
				jai::stdlib::register_all(*e);
				const std::string src = std::string(decl) + R"(
					for (var i = 0; i < 16; i++) { pal.push("c" + to_string(i * 3)); }
					string f(int x) { return pal[x % 16] + "|" + pal[(x * 5) % 16]; }
					var a = [];
					for (var i = 0; i < 64; i++) { a.push(i); }
					var pout = parallel_transform(a, f);
					var sout = [];
					for (auto x : a) { sout.push(f(x)); }
					return [pout, sout];
				)";
				check_matches_serial_at(e, src, {1, 2, 8});
				const auto& caps = detail::engine_access::last_parallel_captures(*e);
				const uint64_t pal_id = detail::engine_access::symbolize(*e, "pal");
				bool saw = false;
				for (const auto& [id, kind] : caps) {
					if (id == pal_id) {
						saw = true;
						check_eq((int)detail::parallel_capture_kind::borrow, (int)kind);
					}
				}
				check_true(saw);
			}
		});

		test("borrowed_string_cells_reread_across_rebuilt_tables", [this]() {
			// A worker serves repeated reads of one table cell from its own copy of the bytes.
			// The copy must stay the worker's own (a body that appends to its local never
			// reaches the table or the next read), and a table rebuilt between regions with
			// different text at the same cells must be read fresh; string literals evaluated
			// in the body come from the worker's per-chunk copy the same way.
			for (const char* decl : {"var pal = [];", "array<string> pal = [];"}) {
				auto e = make_engine();
				jai::stdlib::register_all(*e);
				const std::string src = std::string(decl) + R"(
					var a = [];
					for (var i = 0; i < 96; i++) { a.push(i); }
					string f(int x) {
						string s = pal[x % 8];
						s += "!";
						string t = pal[x % 8];
						string lit = "<";
						lit += pal[(x * 3) % 8];
						return s + t + lit + ">" + pal[x % 8];
					}
					var pout = [];
					var sout = [];
					for (int round = 0; round < 4; round++) {
						pal.clear();
						for (var i = 0; i < 8; i++) { pal.push("r" + to_string(round) + "c" + to_string(i)); }
						var p = parallel_transform(a, f);
						for (auto v : p) { pout.push(v); }
						for (auto x : a) { sout.push(f(x)); }
					}
					return [pout, sout];
				)";
				check_matches_serial_at(e, src, {1, 2, 8});
			}
		});

		test("published_worker_strings_are_not_touched_by_later_regions", [this]() {
			// A literal or a borrowed table cell returned from a region lands in the parent's
			// output as the worker's own node; the worker releases every cached handle at the
			// end of its chunk, so the main thread copying that node races nothing in a later
			// region. Bytes stay right under copy-on-write either way; this pins the values and
			// the sharing shape (the XTHREAD canary build pins the counts).
			for (size_t workers : {size_t(1), size_t(8)}) {
				auto e = make_engine();
				jai::stdlib::register_all(*e);
				e->parallel_thread_count(workers);
				auto r = e->execute(R"(
					array<string> pal = ["p0", "p1", "p2", "p3"];
					var a = [];
					for (var i = 0; i < 64; i++) { a.push(i); }
					string lit(int x) { return "L"; }
					string cell(int x) { return pal[x % 4]; }
					var first = parallel_transform(a, lit);
					var cells = parallel_transform(a, cell);
					first[3] += "!";
					cells[5] += "?";
					var second = parallel_transform(a, lit);
					var cells2 = parallel_transform(a, cell);
					string held = first[7];
					var third = parallel_transform(a, lit);
					held += "#";
					return [first[3], first[4], second[3], cells[5], cells[6], cells2[5], held, third[7], pal[1]];
				)");
				const auto& out = r.as_array();
				check_eq(std::string("L!"), out[0].as_string());
				check_eq(std::string("L"), out[1].as_string());
				check_eq(std::string("L"), out[2].as_string());
				check_eq(std::string("p1?"), out[3].as_string());
				check_eq(std::string("p2"), out[4].as_string());
				check_eq(std::string("p1"), out[5].as_string());
				check_eq(std::string("L#"), out[6].as_string());
				check_eq(std::string("L"), out[7].as_string());
				check_eq(std::string("p1"), out[8].as_string());
			}
		});

		test("typed_decls_read_borrowed_tables_like_the_kernel", [this]() {
			// A worker reads a captured table through the borrow; `T x = tbl[i]` lands through
			// the decl lanes (op_t_load, the fused decl's lane) on a raw scalar slot exactly as the
			// borrow kernel would mint it, strings stay the kernel's (bytes, never the main
			// thread's handle), and every miss (out of range, a mismatched kind) keeps the
			// kernel's text. The same body serial against the live arrays is the oracle.
			for (bool proofs : {true, false}) {
				for (size_t workers : {size_t(1), size_t(8)}) {
					auto e = make_engine();
					jai::stdlib::register_all(*e);
					e->type_proofs(proofs);
					e->parallel_thread_count(workers);
					auto r = e->execute(R"(
						array<int> ints = [10, 20, 30, 40];
						array<float> fls = [0.5, 1.5, 2.5, 3.5];
						var het = [1, 2.5, true, 'c', "s"];
						array<string> strs = ["a", "b", "c", "d"];
						var mixed = ["x", 7];
						var idx = [];
						for (var i = 0; i < 16; i++) { idx.push(i); }
						string body(int i) {
							int a = ints[i % 4];
							int a2 = ints[(i + 1) % 4];
							float f = fls[i % 4];
							float w = ints[i % 4];
							int h0 = het[0];
							float h1 = het[1];
							bool h2 = het[2];
							char h3 = het[3];
							string h4 = het[4];
							string s = strs[i % 4];
							var v = ints[(i + 2) % 4];
							return `${to_string(a)},${to_string(a2)},${to_string(f)},${to_string(w)},${to_string(h0)},${to_string(h1)},${to_string(h2)},${h3},${h4},${s},${to_string(v)}`;
						}
						int oob(int i) { int a = ints[i + 4]; return a; }
						int wrong(int i) { int a = mixed[i % 2]; return a; }
						var serial = [];
						for (var i = 0; i < 16; i++) { serial.push(body(i)); }
						var out = parallel_transform(idx, body);
						bool same = out.size() == serial.size();
						for (var i = 0; i < 16; i++) { if (out[i] != serial[i]) { same = false; } }
						string oob_msg = ""; string wrong_msg = ""; string wrong_serial = "";
						try { parallel_transform(idx, oob); } catch (err) { oob_msg = err; }
						try { parallel_transform(idx, wrong); } catch (err) { wrong_msg = err; }
						try { wrong(0); } catch (err) { wrong_serial = err; }
						return [same, out[1], oob_msg, wrong_msg, wrong_serial, ints[1], het[0]];
					)");
					const auto& out = r.as_array();
					const std::string label = std::string(proofs ? "proofs" : "no-proofs") + " workers=" + std::to_string(workers);
					check_true(out[0].as<bool>(), label + ": the region matches the serial body");
					check_eq(std::string("20,30,1.5,20.0,1,2.5,true,c,s,b,40"), out[1].as_string(), label + ": row 1");
					check_true(out[2].as_string().find("reading a captured array in a parallel body") != std::string::npos, label + ": captured out-of-range text: " + out[2].as_string());
					check_eq(std::string("Cannot initialize 'a' (declared int) with string"), out[4].as_string(), label + ": the serial mismatch text");
					// a worker never interns the slot's name, so it raises the slot-less shape of the same text
					check_true(out[3].as_string().find("Cannot initialize a variable declared int with string") != std::string::npos, label + ": the region's mismatch text is the kernel's slot-less shape: " + out[3].as_string());
					check_eq((int64_t)20, out[5].as_int(), label + ": the table is untouched");
					check_eq((int64_t)1, out[6].as_int(), label + ": the hetero table is untouched");
				}
			}
		});

		test("shared_literal_strings_inside_elements_detach_before_workers_copy", [this]() {
			// Every Node built from one class holds the SAME string node for a literal-seeded
			// map key ("a"), a string field default ("leaf" inside the nested Leaf) and a typed
			// string buffer pushed from a literal; a worker that copies its element (a by-value
			// parameter the read-only proof cannot elide: f passes it whole to side) copies those
			// handles, so the barrier must have detached every one of them first (the counts are
			// not atomic; the XTHREAD canary build aborts at the racing touch). Pinned by the
			// post-region counts: each instance owns its strings alone.
			auto e = make_engine();
			e->parallel_thread_count(8);
			auto r = e->execute(R"(
				class Leaf { int v = 0; string tag = "leaf"; Leaf() {} Leaf(int n) { v = n; } }
				class Node {
					int id = 0; Leaf leaf = Leaf(0); array<string> names = []; map<string, int> counts = {};
					Node() {}
					Node(int n) { id = n; leaf = Leaf(n * 2); names.push("nm"); names.push("nm"); counts["a"] = n * 3; }
				}
				array<Node> nodes = [];
				for (int i = 0; i < 48; i++) { nodes.push(Node(i)); }
				int side(Node n) { return n.names.size(); }
				int f(Node n) { return n.id + n.leaf.v + n.counts["a"] + n.leaf.tag.size() + side(n); }
				var pout = parallel_transform(nodes, f);
				var sout = [];
				for (auto x : nodes) { sout.push(f(x)); }
				return [pout, sout];
			)");
			const auto& pair = r.as_array();
			check_true(arrays_equal(pair[0], pair[1]), "the region matches the serial loop");
			check_eq((int64_t)(5 + 10 + 15 + 4 + 2), pair[0].as_array()[5].as_int());
			script_value nodes = e->get_variable("nodes");
			const script_array* arr = nodes.deref().unchecked_array_node();
			check_true(arr && arr->is_typed() && arr->kind() == script_array::kind_t::obj, "typed object array");
			auto cd = detail::engine_access::get_class_registry(*e).find_script_class("Node");
			auto leaf_cd = detail::engine_access::get_class_registry(*e).find_script_class("Leaf");
			check_not_null(cd); check_not_null(leaf_cd);
			const uint32_t counts_slot = cd->field_slot(detail::engine_access::symbolize(*e, "counts"));
			const uint32_t names_slot = cd->field_slot(detail::engine_access::symbolize(*e, "names"));
			const uint32_t leaf_slot = cd->field_slot(detail::engine_access::symbolize(*e, "leaf"));
			const uint32_t tag_slot = leaf_cd->field_slot(detail::engine_access::symbolize(*e, "tag"));
			for (size_t i = 0; i < arr->size(); ++i) {
				auto* inst = static_cast<class_instance*>(arr->objs()[i].handle->object());
				const script_value* counts = inst->field_slot_value(counts_slot);
				check_true(counts && counts->is_map() && !counts->unchecked_as_map().empty(), "counts present");
				const script_value& key = counts->unchecked_as_map().begin()->first;
				check_eq((long)1, (long)const_cast<script_value&>(key).unchecked_get_string_storage().use_count(), "map key detached for element " + std::to_string(i));
				const script_value* names = inst->field_slot_value(names_slot);
				const script_array* nn = names ? names->unchecked_array_node() : nullptr;
				check_true(nn && nn->is_typed() && nn->kind() == script_array::kind_t::str && nn->size() == 2, "names is a string buffer");
				check_eq((long)1, (long)nn->strs()[0].use_count(), "string buffer slot 0 detached for element " + std::to_string(i));
				check_eq((long)1, (long)nn->strs()[1].use_count(), "string buffer slot 1 detached for element " + std::to_string(i));
				const script_value* leaf = inst->field_slot_value(leaf_slot);
				auto* leaf_inst = leaf ? static_cast<class_instance*>(leaf->get_object_holder()->object()) : nullptr;
				const script_value* tag = leaf_inst ? leaf_inst->field_slot_value(tag_slot) : nullptr;
				check_true(tag && tag->is_string(), "tag present");
				check_eq((long)1, (long)const_cast<script_value*>(tag)->unchecked_get_string_storage().use_count(), "nested string field detached for element " + std::to_string(i));
			}
		});

		test("string_input_sharing_one_literal_node_matches_serial", [this]() {
			// A transform over strings that all share one node (a literal pushed in a loop):
			// the by-value element copy in each worker touches that node's count, so the source
			// is never "exclusive by construction" and the barrier detaches the boxed range
			for (int round = 0; round < 6; ++round) {
				auto e = make_engine();
				e->parallel_thread_count(8);
				auto r = e->execute(R"(
					array<string> typed = [];
					var hetero = [];
					for (int i = 0; i < 400; i++) { typed.push("lit"); hetero.push("lit"); }
					string f(string s) { return s + "!"; }
					var p1 = parallel_transform(typed, f);
					var p2 = parallel_transform(hetero, f);
					var s1 = []; var s2 = [];
					for (auto x : typed) { s1.push(f(x)); }
					for (auto x : hetero) { s2.push(f(x)); }
					return [p1, s1, p2, s2, typed[7], hetero[7]];
				)");
				const auto& out = r.as_array();
				check_true(arrays_equal(out[0], out[1]), "typed string source");
				check_true(arrays_equal(out[2], out[3]), "hetero string source");
				check_eq(std::string("lit"), out[4].as_string());
				check_eq(std::string("lit"), out[5].as_string());
			}
		});

		test("spin_wait_budget_changes_no_result", [this]() {
			// The pool's spin-before-park budget is a wake-latency knob: dense regions with the
			// default budget, the same regions with every wake through the kernel (0), and a
			// budget flipped between regions all produce the serial loop's results at 8 workers
			const char* src = R"(
				class P { int v = 0; float w = 0.0; }
				array<P> ps = [];
				for (int i = 0; i < 64; i++) { P p = P(); p.v = i; ps.push(p); }
				var acc = [];
				for (int round = 0; round < 12; round++) {
					parallel_for (auto& p : ps) { p.v = p.v * 3 + round; p.w = p.w + p.v * 0.5; }
					int f(P p) { return p.v * 2; }
					var t = parallel_transform(ps, f);
					acc.push(t[63]);
				}
				to_json([acc, ps[63].v, ps[63].w]);
			)";
			std::vector<std::string> outs;
			for (size_t budget : {size_t(1000), size_t(0), size_t(50)}) {
				auto e = make_engine();
				jai::stdlib::register_all(*e);
				e->parallel_thread_count(8);
				check_eq((size_t)1000, detail::engine_access::parallel_spin_wait_us(*e), "default budget");
				detail::engine_access::parallel_spin_wait_us(*e, budget);
				check_eq(budget, detail::engine_access::parallel_spin_wait_us(*e));
				outs.push_back(e->execute(src).as_string());
				if (budget == 50) { detail::engine_access::parallel_spin_wait_us(*e, 1000); check_eq(outs.back(), e->execute(src).as_string(), "budget flipped on a live pool"); }
			}
			auto s = make_engine();
			jai::stdlib::register_all(*s);
			s->parallel_thread_count(1);
			const std::string serial = s->execute(src).as_string();
			for (const auto& o : outs) { check_eq(serial, o); }
		});

		test("shared_block_partition_covers_every_element_once", [this]() {
			// Without a weight hint the elements go out in blocks of max(1, n / 8W), each worker's first block its own and every later
			// block to whichever worker finishes first. Every element is visited exactly once, the zip's arrays step together, a
			// by-value transform lands every result at its own index, the first failing element is still the lowest index, and the
			// published chunk counts cover the range (the blocks each worker took, whatever the timing)
			for (size_t workers : {size_t(1), size_t(2), size_t(3), size_t(7), size_t(8)}) {
				auto e = make_engine();
				e->execution_budget(0);   // timing-free: a loaded machine must not trip the default budget
				jai::stdlib::register_all(*e);
				e->parallel_thread_count(workers);
				auto r = e->execute(R"(
					array<int> xs = []; array<int> ys = []; var seen = [];
					for (int i = 0; i < 37; i++) { xs.push(i); ys.push(100 + i); seen.push(0); }
					parallel_for (auto& x : xs) { x = x * 3 + 1; }
					parallel_for (auto& x, auto& y : xs, ys) { y = y + x; }
					int f(int v) { return v - 1; }
					var t = parallel_transform(xs, f);
					var sx = []; var sy = []; var st = [];
					for (int i = 0; i < 37; i++) { sx.push(i * 3 + 1); sy.push(100 + i + i * 3 + 1); st.push(i * 3); }
					return [xs == sx, ys == sy, t == st];
				)");
				const auto& out = r.as_array();
				const std::string label = "workers=" + std::to_string(workers);
				check_true(out[0].as<bool>(), label + " in-place range");
				check_true(out[1].as<bool>(), label + " zip");
				check_true(out[2].as<bool>(), label + " transform");
				const auto& b = detail::engine_access::last_parallel_chunk_bounds(*e);
				check_eq(workers + 1, b.size(), label + " chunk count");
				check_eq((size_t)37, b.back(), label + " counts sum to n");
				for (size_t k = 0; k + 1 < b.size(); ++k) { check(b[k] <= b[k + 1], label + " counts are cumulative"); }
				auto failed = e->execute(R"(
					string msg = "";
					array<int> bad = []; for (int i = 0; i < 37; i++) { bad.push(i); }
					try { parallel_for (auto& b : bad) { if (b >= 5 && b % 2 == 1) { throw "odd " + to_string(b); } } } catch (err) { msg = err; }
					return msg;
				)");
				check_eq(std::string("odd 5"), failed.as_string(), label + " lowest failing element wins");
			}
			// many elements with uneven costs over many workers: every element once, and the lowest failing element still wins
			for (int round = 0; round < 10; ++round) {
				auto e = make_engine();
				e->execution_budget(0);   // the Debug interpreter's 3000-element passes outrun the default on a loaded machine
				jai::stdlib::register_all(*e);
				e->parallel_thread_count(6);
				auto r = e->execute(R"(
					array<int> xs = []; array<int> ys = []; for (int i = 0; i < 3000; i++) { xs.push(i); ys.push(i); }
					parallel_for (auto& x : xs) { int s = 0; for (int k = 0; k < x % 97; k++) { s = s + k; } x = x * 2 + s % 2; }
					for (auto& y : ys) { int c = y % 97; y = y * 2 + (c * (c - 1) / 2) % 2; }
					array<int> zs = []; for (int i = 0; i < 3000; i++) { zs.push(i); }
					string msg = "";
					try { parallel_for (auto& z : zs) { if (z >= 2400 && z % 7 == 0) { throw "at " + to_string(z); } } } catch (err) { msg = err; }
					return to_string(xs == ys) + " " + msg;
				)");
				check_eq(std::string("true at 2401"), r.as_string(), "round " + std::to_string(round));
			}
		});

		test("captured_snapshot_result_never_aliases", [this]() {
			auto e = make_engine();
			// `return s` shares the worker's snapshot handle - the result path must
			// materialize it, or elements produced by ONE worker would alias each other
			// while elements from different workers don't (worker-count-visible!)
			for (size_t workers : {size_t(1), size_t(8)}) {
				e->parallel_thread_count(workers);
				auto r = e->execute(R"(
					var s = [1, 2, 3];
					var f(int i) { return s; }
					var a = [];
					for (var i = 0; i < 24; i++) { a.push(i); }
					var pout = parallel_transform(a, f);
					pout[0].push(99);
					return [pout[0].size(), pout[1].size(), pout[23].size()];
				)");
				const auto& sizes = r.as_array();
				check_eq((int64_t)4, sizes[0].as_int());
				check_eq((int64_t)3, sizes[1].as_int());
				check_eq((int64_t)3, sizes[2].as_int());
			}
		});

		test("captured_snapshot_charges_memory_cap_borrow_does_not", [this]() {
			auto e = make_engine();
			e->execution_budget(0);   // Debug-interpreter array build outruns the 1s default
			e->execute(R"(
				var big = [];
				for (var i = 0; i < 8000; i++) { big.push(i); }
			)");
			e->memory_cap(32 * 1024);
			e->parallel_thread_count(4);
			// Whole-value read -> snapshot -> barrier detach charges the enclosing
			// execute's memory accounting -> catchable memory-cap raise at a back-edge
			auto r = e->execute(R"(
				var f(int i) { return big; }
				var a = [];
				for (var i = 0; i < 32; i++) { a.push(i); }
				try {
					var pout = parallel_transform(a, f);
					for (var i = 0; i < 4; i++) { var nudge = i; }
					return "no-error";
				} catch (e) { return e; }
			)");
			check_true(r.as<std::string>().find("memory cap") != std::string::npos);

			// Borrow tier: subscript-only reads of the same container copy NOTHING, so
			// the same cap stays untouched (both backends)
			{
				auto e2 = make_engine();
				e2->execution_budget(0);
				e2->execute(R"(
					var big = [];
					for (var i = 0; i < 8000; i++) { big.push(i); }
				)");
				e2->memory_cap(32 * 1024);
				e2->parallel_thread_count(4);
				auto r2 = e2->execute(R"(
					int f(int i) { return big[i * 7 % 8000]; }
					var a = [];
					for (var i = 0; i < 64; i++) { a.push(i); }
					try {
						var pout = parallel_transform(a, f);
						for (var i = 0; i < 4; i++) { var nudge = i; }
						return pout[0];
					} catch (e) { return e; }
				)");
				check_true(r2.is_int());
			}
		});

		test("captured_write_rejection_matrix", [this]() {
			auto e = make_engine();
			auto expect_error = [&](const char* body_line, const char* fragment) {
				const std::string src = std::string(R"(
					var g = [1, 2, 3];
					int helper_ref(int& a) { return a; }
					int f(int x) { )") + body_line + R"( return x; }
					try { parallel_transform([1, 2, 3, 4], f); } catch (e) { return e; }
					return "no-error";
				)";
				auto r = e->execute(src);
				auto msg = r.as<std::string>();
				check_true(msg.find(fragment) != std::string::npos);
				check_true(msg.find(" at ") != std::string::npos);   // line:col position
			};
			expect_error("g = [9];", "body writes enclosing state 'g'");
			expect_error("g[0] = 5;", "body writes enclosing state 'g'");
			expect_error("g[0] += 1;", "body writes enclosing state 'g'");
			expect_error("g[0]++;", "body writes enclosing state 'g'");
			expect_error("g.push(4);", "method 'push' may mutate captured state 'g'");
			expect_error("g.sort();", "method 'sort' may mutate captured state 'g'");
			expect_error("var& r = g;", "reference declaration would alias enclosing state 'g'");
			expect_error("helper_ref(g[0]);", "argument passes enclosing state 'g' to reference parameter 'a' of 'helper_ref' (captured reads are read-only; 'helper_ref' only reads it: declare it const int& a)");
			expect_error("for (auto& v : g) { }", "cannot iterate captured state 'g' by reference");
			// ::g is the program's g whatever a local of the name says
			expect_error("var g = 0; ::g = [9];", "body writes enclosing state 'g'");
			expect_error("var g = [0]; ::g[0] = 5;", "body writes enclosing state 'g'");
			expect_error("array<int> mine = [1]; parallel_for (auto& v : mine) { v = 2; }", "a nested parallel_for is not allowed in a parallel body");
		});

		test("captured_reads_deterministic_1_2_8", [this]() {
			auto e = make_engine();
			// Grid-consumption determinism: byte-identical output at every worker count
			const std::string src = R"(
				var grid = [];
				for (var i = 0; i < 2048; i++) { grid.push((i * 76241) % 65536); }
				int f(int row) {
					var acc = 0;
					for (var x = 0; x < 64; x++) { acc = (acc * 31 + grid[row * 64 + x]) % 1000003; }
					return acc;
				}
				var rows = [];
				for (var r = 0; r < 32; r++) { rows.push(r); }
				return parallel_transform(rows, f);
			)";
			e->parallel_thread_count(1);
			auto r1 = e->execute(src);
			e->parallel_thread_count(2);
			auto r2 = e->execute(src);
			e->parallel_thread_count(8);
			auto r8 = e->execute(src);
			check_true(arrays_equal(r1, r2));
			check_true(arrays_equal(r1, r8));
			// a ::name reads the worker's copy of the program's variable or function, a local of the name notwithstanding
			const std::string qualified = R"(
				array<int> grid = [3, 5, 7];
				int sq(int v) { return v * v; }
				int K = 100;
				class W { int v = 0; int K = 1; void m() { v = ::K + K; } }
				array<W> ws = [W(), W()];
				array<int> idx = [];
				for (int i = 0; i < 64; ++i) { idx.push(i); }
				array<int> out = idx;
				parallel_for (int i : idx) { int sq = 1; int grid = 2; out[i] = ::sq(i) + ::grid[i % 3] + sq + grid; }
				parallel_for (auto& w : ws) { w.m(); }
				int scoped() { int K = 2; array<int> local = [0, 0]; parallel_for (int i : [0, 1]) { local[i] = ::K + i; } return local[0] + local[1]; }
				return to_json(out[0]) + " " + to_json(out[63]) + " " + to_json(ws[1].v) + " " + to_json(scoped());
			)";
			auto q = make_engine();
			jai::stdlib::register_all(*q);
			for (size_t workers : {size_t(1), size_t(2), size_t(8)}) {
				q->parallel_thread_count(workers);
				check_eq(std::string("6 3975 101 201"), q->execute(qualified).as<std::string>(), "::name reads, workers " + std::to_string(workers));
			}
			auto conflict = q->execute("int K = 5; array<int> out = [0, 0]; var f() { int K = 2; try { parallel_for (int i : [0, 1]) { out[i] = ::K + K; } } catch (e) { return e; } return \"no-error\"; } f();");
			check_true(conflict.as<std::string>().rfind("parallel_for: '::K' names the program's 'K' and the body also reads a nearer 'K'", 0) == 0, conflict.as<std::string>());
			// a const reference to captured state reads the worker's own copy: a declaration, a by-reference loop, a field
			const std::string const_refs = R"(
				array<array<int>> grid = [[1, 2], [3, 4], [5, 6]];
				array<int> table = [10, 20, 30];
				class Cfg { array<int> items = [7, 8, 9]; }
				Cfg cfg;
				array<int> idx = [0, 1, 2, 3, 4, 5];
				array<int> out = [0, 0, 0, 0, 0, 0];
				parallel_for (int i : idx) {
					const auto& row = grid[i % 3]; const int& t = table[i % 3]; const auto& items = cfg.items;
					int s = 0;
					for (const auto& v : table) { s += v; }
					for (const auto& r : grid) { s += r[1]; }
					out[i] = row[0] + row[1] + t + items[i % 3] + s;
				}
				return to_json(out);
			)";
			for (size_t workers : {size_t(1), size_t(2), size_t(8)}) {
				q->parallel_thread_count(workers);
				check_eq(std::string("[92,107,122,92,107,122]"), q->execute(const_refs).as<std::string>(), "const references, workers " + std::to_string(workers));
			}
			// a writing method through a const loop variable raises as the serial loop does, on every worker
			const std::string const_call = "class V { int a = 7; void bump() { a += 1; } } array<V> ws = [V(), V()]; try { parallel_for (const auto& w : ws) { w.bump(); } } catch (e) { return e; } return \"no-error\";";
			check_true(q->execute(const_call).as<std::string>().rfind("Cannot call 'bump' on 'w': 'w' is const and 'bump' writes its receiver", 0) == 0, q->execute(const_call).as<std::string>());
		});

		test("captured_snapshot_equals_barrier_content", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(4);
			// The region sees exactly the barrier-time content, every call (pin: content
			// changes BETWEEN calls are visible, content is frozen DURING one)
			auto r = e->execute(R"(
				var cfg = [10];
				int f(int x) { return cfg[0] + x; }
				var a = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];
				var first = parallel_transform(a, f);
				cfg[0] = 20;
				var second = parallel_transform(a, f);
				return [first[0], second[0]];
			)");
			const auto& vals = r.as_array();
			check_eq((int64_t)11, vals[0].as_int());
			check_eq((int64_t)21, vals[1].as_int());
		});

		// Elements must be value-semantic: a class with an open (var) field is not a flat
		// value class, and a shared_ptr element with a live alias would diverge from the
		// serial loop (the barrier may not clone it apart) - both name the offending leaf
		// parallel_reduce: chunks sized by the array's size alone fold in element order and combine in chunk order, so a float sum is
		// bit-identical at every worker count; an object or container accumulator starts from init's copy in every chunk; step is
		// admitted as a parallel_for body and its throws propagate; empty input yields init
		test("parallel_reduce_is_the_same_at_every_worker_count", [this]() {
			const std::string prelude = R"(
				array<float> xs = [];
				for (int i = 0; i < 10000; i++) { xs.push(1.0 / (i + 1) - 0.0001 * (i % 7)); }
				float add(float a, float b) { return a + b; }
				class Stats { int count = 0; float sum = 0.0; float hi = -1000.0; }
				Stats fold(Stats s, float x) { s.count += 1; s.sum += x; if (x > s.hi) { s.hi = x; } return s; }
				Stats merge(Stats a, Stats b) { a.count += b.count; a.sum += b.sum; if (b.hi > a.hi) { a.hi = b.hi; } return a; }
				array<int> tally(array<int> acc, float x) { acc[x > 0.01 ? 1 : 0] += 1; return acc; }
				array<int> both(array<int> a, array<int> b) { a[0] += b[0]; a[1] += b[1]; return a; }
				void fold_in(Stats& s, float x) { s.count += 1; s.sum += x; if (x > s.hi) { s.hi = x; } }
				Stats& grow(Stats& s, float x) { s.count += 1; s.sum += x; return s; }
				float scaled() { float k = 2.0; return parallel_reduce(xs, 0.0, [k](float a, float x) { return a + x * k; }, add); }
				int combines = 0;
				float counting_add(float a, float b) { combines++; return a + b; }
				int combine_calls() { array<float> few = []; for (int i = 0; i < 200; i++) { few.push(1.0); } parallel_reduce(few, 0.0, add, counting_add); return combines; }
			)";
			const std::string body = R"(
				Stats s = parallel_reduce(xs, Stats(), fold, merge);
				Stats t = parallel_reduce(xs, Stats(), fold_in, merge);
				Stats u = parallel_reduce(xs, Stats(), grow, merge);
				float lam = parallel_reduce(xs, 0.0, [](float a, float x) { return a + x; }, [](float a, float b) { return a + b; });
				return `${format("{:.17f}", parallel_reduce(xs, 0.0, add, add))} ${s.count} ${format("{:.17f}", s.sum)} ${to_json(parallel_reduce(xs, [0, 0], tally, both))} ${parallel_reduce([], 7, add, add)} ${format("{:.17f}", t.sum)} ${format("{:.17f}", u.sum)} ${format("{:.17f}", lam)} ${format("{:.17f}", scaled())} combines ${combine_calls()}`;
			)";
			std::string first;
			for (bool vm : {false, true}) {
				for (size_t workers : {size_t(1), size_t(2), size_t(3), size_t(8)}) {
					auto e = make_engine();
					e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					jai::stdlib::register_all(e);
					e->parallel_thread_count(workers);
					const std::string got = e->execute(prelude + body).to_string();
					if (first.empty()) { first = got; }
					check_eq(first, got, std::string(vm ? "vm " : "interp ") + std::to_string(workers) + " workers");
				}
			}
			check_true(first.find(" 10000 ") != std::string::npos && first.find("[9903,97] 7") != std::string::npos, first);
			check_true(first.find(" combines 12") != std::string::npos, first);   // combine is serial code after the join: it counts its calls

			const std::vector<std::pair<std::string, std::string>> errors {
				{"float total = 0.0; float bad(float a, float x) { total += x; return a; } try { parallel_reduce(xs, 0.0, bad, add); } catch (err) { return err; } return \"ran\";",
				 "parallel_reduce: body writes enclosing state 'total'"},
				{"float boom(float a, float x) { if (x < 0.001) { throw \"small\"; } return a + x; } try { parallel_reduce(xs, 0.0, boom, add); } catch (err) { return err; } return \"ran\";",
				 "small"},
				{"float one(float a) { return a; } try { parallel_reduce(xs, 0.0, one, add); } catch (err) { return err; } return \"ran\";",
				 "parallel_reduce: step must take two parameters (acc, x)"},
				{"float total2 = 0.0; try { parallel_reduce(xs, 0.0, [](float a, float x) { total2 += x; return a; }, add); } catch (err) { return err; } return \"ran\";",
				 "parallel_reduce: body writes enclosing state 'total2'"},
				{"try { return parallel_reduce(xs, 0.0, [](float a, float& x) { return a; }, add); } catch (err) { return err; } return \"ran\";",
				 "parallel_reduce: step takes its element by value"},
			};
			for (const auto& [src, expected] : errors) {
				for (bool vm : {false, true}) {
					auto e = make_engine();
					jai::stdlib::register_all(e);
					e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					e->parallel_thread_count(4);
					const std::string got = e->execute("[[warn_mode]]; " + prelude + src).to_string();
					check_true(got.rfind(expected, 0) == 0, std::string(vm ? "vm: " : "interp: ") + src + " -> " + got);
				}
			}
			auto checked = make_engine();
			jai::stdlib::register_all(checked);
			auto has_error = [](const check_report& report, std::string_view text) {
				for (const auto& d : report.diagnostics) { if (d.severity == check_diagnostic::level::error && d.message.find(text) != std::string::npos) { return true; } }
				return false;
			};
			auto r = checked->check(prelude + "float one(float a) { return a; }\nvar v = parallel_reduce(xs, 0.0, one, add);\nvar w = parallel_reduce(xs, 0.0, add);");
			check_true(has_error(r, "argument 3 of 'parallel_reduce': step 'one' must take two parameters (acc, x)"), r.format());
			check_true(has_error(r, "'parallel_reduce' expects 4 argument(s), got 3"), r.format());
		});

		test("rejects_non_value_semantic_elements", [this]() {
			auto e = make_engine();
			auto r = e->execute(R"(
				class P { var x = 1; }
				int f(var v) { return 1; }
				var a = [];
				a.push(P());
				try { parallel_transform(a, f); } catch (e) { return e; }
				return "no-error";
			)");
			auto msg = r.as<std::string>();
			check_true(msg.find("parallel_transform: element 0: value is not value-semantic (object)") != std::string::npos, msg);

			auto r2 = e->execute(R"(
				class Q { int x = 1; }
				int g(var v) { return 1; }
				var q = new Q();
				var b = [1];
				b.push(q);
				b.push(q);
				try { parallel_transform(b, g); } catch (e) { return e; }
				return "no-error";
			)");
			auto msg2 = r2.as<std::string>();
			check_true(msg2.find("parallel_transform: elements 1 and 2 hold the same object") != std::string::npos, msg2);

			auto r3 = e->execute(R"(
				class R { var x = 1; }
				int h(var v) { return 1; }
				array<R> c = [];
				c.push(R());
				c.push(R());
				try { parallel_transform(c, h); } catch (e) { return e; }
				return "no-error";
			)");
			auto msg3 = r3.as<std::string>();
			check_true(msg3.find("parallel_transform: element 0: value is not value-semantic (object)") != std::string::npos, msg3);
		});

		// Member chains root at the element (the worker's private by-value copy), a local or a captured object (read only);
		// a store through a captured name, and a member of a captured value that is no object, are refused
		test("rejects_member_access_on_captured_name", [this]() {
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			auto r = e->execute(R"(
				class P { int a = 1; }
				var g = P();
				int f(int x) { return g.a + x; }
				return to_json(parallel_transform([1, 2, 3], f));
			)");
			check_eq(std::string("[2,3,4]"), r.as<std::string>());

			auto r2 = e->execute(R"(
				class P2 { int a = 1; }
				var g2 = P2();
				int f2(int x) { g2.a = x; return x; }
				try { parallel_transform([1, 2, 3], f2); } catch (e) { return e; }
				return "no-error";
			)");
			check_true(r2.as<std::string>().find("member store reaches captured state 'g2'") != std::string::npos, r2.as<std::string>());

			auto r3 = e->execute(R"(
				var m = {"k": 1};
				int f3(int x) { return m.k + x; }
				try { parallel_transform([1, 2, 3], f3); } catch (e) { return e; }
				return "no-error";
			)");
			check_true(r3.as<std::string>().find("parallel_transform: member access reaches captured state 'm' (captured reads are read-only, and only a captured object has members) at 3:") != std::string::npos, r3.as<std::string>());

			// a chain on a computed value: a call's result and a conditional read as any expression, and a store or a reference
			// through a conditional names the arm it picks; a call that may return a reference stays refused as a store root
			const std::string computed = R"(
				class V { int a = 1; }
				V g1; V g2; g2.a = 5;
				V& pick() { return g1; }
				V mk(int k) { V v; v.a = k; return v; }
				array<int> idx = [0, 1, 2, 3, 4, 5, 6, 7];
				array<int> out = [0, 0, 0, 0, 0, 0, 0, 0];
				parallel_for (int i : idx) {
					V p; V q; int a = 0; int b = 0; array<int> s = [0]; array<int> t = [0];
					(i > 3 ? p : q).a = 4; (i > 3 ? a : b) = 5; (i > 3 ? s : t)[0] = 6; ++a = a + 1;
					auto& r = i > 5 ? a : b; r = r + 1;
					out[i] = mk(i).a + (i > 3 ? g1 : g2).a + pick().a + p.a * 10 + q.a * 100 + a * 1000 + b * 10000 + s[0] * 100000 + t[0] * 1000000;
				}
				to_json(out);
			)";
			for (size_t workers : {size_t(1), size_t(8)}) {
				e->parallel_thread_count(workers);
				check_eq(std::string("[6061416,6061417,6061418,6061419,616146,616147,607148,607149]"), e->execute(computed).as<std::string>(), "computed roots, workers " + std::to_string(workers));
			}
			const auto refused = [&](const char* body) {
				auto got = e->execute(std::string("class V { int a = 1; } V g1; V& pick() { return g1; } int ga = 0; array<int> idx = [0, 1]; try { parallel_for (int i : idx) { ") + body + " } } catch (e) { return e; } return \"no-error\";");
				return got.as<std::string>();
			};
			check_true(refused("pick().a = 9;").rfind("parallel_for: member store must root at a local variable in a parallel body", 0) == 0, refused("pick().a = 9;"));
			check_true(refused("int b = 0; (i > 0 ? ga : b) = 5;").rfind("parallel_for: body writes enclosing state 'ga'", 0) == 0, refused("int b = 0; (i > 0 ? ga : b) = 5;"));
			check_true(refused("int b = 0; auto& r = i > 0 ? ga : b; r = 7;").rfind("parallel_for: reference declaration would alias enclosing state 'ga'", 0) == 0,
			           refused("int b = 0; auto& r = i > 0 ? ga : b; r = 7;"));
			check_true(refused("++ga = 7;").rfind("parallel_for: body writes enclosing state 'ga'", 0) == 0, refused("++ga = 7;"));
		});

		// === Element members (transformmembers): a body reads members of its own element ===

		test("element_member_reads_field_nested_array_and_map", [this]() {
			auto e = make_engine();
			const std::string src = R"(
				class Leaf { int v = 0; string tag = "leaf"; Leaf() {} Leaf(int n) { v = n; } }
				class Node {
					int id = 0; Leaf leaf = Leaf(0); array<int> xs = []; map<string, int> counts = {};
					Node() {}
					Node(int n) { id = n; leaf = Leaf(n * 2); xs = [n, n + 1, n + 2]; counts["a"] = n * 3; }
				}
				array<Node> nodes = [];
				for (int i = 0; i < 48; i++) { nodes.push(Node(i)); }
				int side(Node n) { return n.xs[0]; }
				int f(Node n) { return n.id + n.leaf.v + n.xs[1] + n.counts["a"] + n.leaf.tag.size() + side(n); }
				var pout = parallel_transform(nodes, f);
				var sout = [];
				for (auto x : nodes) { sout.push(f(x)); }
				return [pout, sout];
			)";
			check_matches_serial_at(e, src, {1, 2, 8});
			check_eq((int64_t)(5 + 10 + 6 + 15 + 4 + 5), e->execute(src).as_array()[0].as_array()[5].as_int());

			// The same shape through an untyped array (per-element walk instead of the declared proof)
			const std::string hetero = R"(
				class Leaf2 { int v = 0; string tag = "leaf"; Leaf2() {} Leaf2(int n) { v = n; } }
				class Node2 { int id = 0; Leaf2 leaf = Leaf2(0); array<int> xs = []; Node2() {} Node2(int n) { id = n; leaf = Leaf2(n * 2); xs = [n, n + 1]; } }
				var nodes = [];
				for (int i = 0; i < 40; i++) { nodes.push(Node2(i)); }
				int g(var n) { return n.id + n.leaf.v + n.xs[1] + n.leaf.tag.size(); }
				var pout = parallel_transform(nodes, g);
				var sout = [];
				for (auto x : nodes) { sout.push(g(x)); }
				return [pout, sout];
			)";
			check_matches_serial_at(e, hetero, {1, 2, 8});
		});

		test("prewarm_classes_reached_through_typed_containers", [this]() {
			// A worker's first member touch on a class it only reaches through a typed container
			// (array<Leaf> field, array<Mid> -> array<Leaf>, map<string, array<Leaf>>) must find every
			// shape interned at the barrier; a fresh engine per run keeps the first region cold
			const std::string src = R"(
				class Leaf { int v = 3; }
				class Mid { array<Leaf> leaves = [Leaf(), Leaf()]; }
				class Node {
					Leaf leaf = Leaf(); array<int> xs = [1, 2]; array<Leaf> extras = [Leaf()];
					array<Mid> mids = [Mid()]; map<string, array<Leaf>> groups = {"a": [Leaf()]};
				}
				array<Node> nodes = [];
				for (int i = 0; i < 40; i++) { nodes.push(Node()); }
				int f(Node n) { return n.leaf.v + n.xs[0] + n.extras[0].v * 10 + n.mids[0].leaves[1].v * 100 + n.groups["a"][0].v * 1000; }
				var pout = parallel_transform(nodes, f);
				var sout = [];
				for (auto x : nodes) { sout.push(f(x)); }
				return [pout, sout];
			)";
			for (size_t workers : {size_t(1), size_t(2), size_t(8)}) {
				auto e = make_engine();
				e->parallel_thread_count(workers);
				auto r = e->execute(src);
				const auto& pair = r.as_array();
				check_true(arrays_equal(pair[0], pair[1]));
				check_eq((int64_t)(3 + 1 + 30 + 300 + 3000), pair[0].as_array()[7].as_int());
			}
		});

		test("vec3_transform_returns_x_times_y_and_keeps_node_typed", [this]() {
			auto e = make_engine();
			const std::string src = R"(
				class Vec3 { float x = 0.0; float y = 0.0; float z = 0.0; Vec3() {} Vec3(float a) { x = a; y = a * 2.0; z = a * 3.0; } }
				array<Vec3> vecs = [];
				for (int i = 0; i < 64; i++) { vecs.push(Vec3(i)); }
				float f(Vec3 v) { return v.x * v.y; }
				var pout = parallel_transform(vecs, f);
				var sout = [];
				for (auto v : vecs) { sout.push(f(v)); }
				return [pout, sout, vecs];
			)";
			for (size_t workers : {size_t(1), size_t(2), size_t(8)}) {
				e->parallel_thread_count(workers);
				auto r = e->execute(src);
				const auto& triple = r.as_array();
				check_eq((size_t)3, triple.size());
				check_true(arrays_equal(triple[0], triple[1]));
				check_near(7.0 * 14.0, triple[0].as_array()[7].as<double>(), 1e-9);
				check_true(triple[2].unchecked_array_node()->is_typed(), "the copy carries the source's object kind");
			}
			auto vecs = e->execute("vecs");
			check_true(vecs.unchecked_array_node()->is_typed(), "source keeps its object kind");
			check_true(vecs.unchecked_array_node()->kind() == script_array::kind_t::obj, "kind obj");
			check_eq((uint32_t)0, vecs.unchecked_array_node()->live_elem_refs);
		});

		test("read_only_fn_binds_in_place_mutating_shapes_still_copy", [this]() {
			// A by-value fn parameter the body only reads binds the element in place on the worker
			// (unobservable, no copy); any write through it, a by-ref hand-off, a mutating field
			// method or a whole use keeps the private copy - the source never changes either way.
			auto e = make_engine();
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				class P { int a = 1; string s = "x"; array<int> xs = [1]; P() {} P(int v) { a = v; xs = [v]; } }
				array<P> ps = [];
				for (int i = 0; i < 200; i++) { ps.push(P(i)); }
				int reads(P p) { return p.a + p.xs[0] + p.s.size(); }
				int writes(P p) { p.a = p.a + 1000; p.s = "w"; return p.a; }
				void bump(P& q) { q.a = q.a + 5000; }
				int by_ref_helper(P p) { bump(p); return p.a; }
				int field_mutator(P p) { p.xs.push(9); return p.xs.size(); }
				int whole(P p) { var q = p; q.a = q.a + 1; return q.a; }   // a whole use keeps the copy
				var r1 = parallel_transform(ps, reads);
				var r2 = parallel_transform(ps, writes);
				var r3 = parallel_transform(ps, by_ref_helper);
				var r4 = parallel_transform(ps, field_mutator);
				var r5 = parallel_transform(ps, whole);
				int sum_a = 0; int sum_xs = 0;
				for (auto& p : ps) { sum_a = sum_a + p.a; sum_xs = sum_xs + p.xs.size(); }
				return [r1[7], r2[7], r3[7], r4[7], r5[7], sum_a, sum_xs, ps[3].a, ps[3].xs.size(), ps[7].s];
			)");
			const auto& v = r.as_array();
			check_eq((int64_t)(7 + 7 + 1), v[0].as_int());
			check_eq((int64_t)1007, v[1].as_int());
			check_eq((int64_t)5007, v[2].as_int());
			check_eq((int64_t)2, v[3].as_int());
			check_eq((int64_t)8, v[4].as_int());
			check_eq((int64_t)(199 * 200 / 2), v[5].as_int());
			check_eq((int64_t)200, v[6].as_int());
			check_eq((int64_t)3, v[7].as_int());
			check_eq((int64_t)1, v[8].as_int());
			check_eq(std::string("x"), v[9].as<std::string>());
		});

		test("worker_copied_instances_register_at_the_join", [this]() {
			// An instance a worker copies (a by-value store into a fresh array: a field holding an instance copy-assigns) parks in the worker's
			// pending list and joins the class registry after the region: a later hot reload must
			// still migrate it (the registry is how migration finds live instances)
			auto e = make_engine();
			e->parallel_thread_count(4);
			e->execute(R"(
				class Leaf { int v = 0; Leaf() {} Leaf(int n) { v = n; } }
				class Node { Leaf a = Leaf(0); array<Leaf> b = [Leaf(0)]; Node() {} Node(int n) { a = Leaf(n); b = [Leaf(-1)]; } }
				array<Node> nodes = [];
				for (int i = 0; i < 100; i++) { nodes.push(Node(i)); }
				parallel_for (auto& n : nodes) { n.b = [n.a]; }
			)");
			e->execute(R"(
				class Leaf { int v = 0; int extra = 7; Leaf() {} Leaf(int n) { v = n; } }
			)");
			auto r = e->execute("return [nodes[5].b[0].v, nodes[5].b[0].extra, nodes[99].b[0].extra, nodes[5].a.extra];");
			const auto& v = r.as_array();
			check_eq((int64_t)5, v[0].as_int());
			check_eq((int64_t)7, v[1].as_int());
			check_eq((int64_t)7, v[2].as_int());
			check_eq((int64_t)7, v[3].as_int());
		});

		test("transform_returns_struct_copies_and_typed_zip_out", [this]() {
			// parallel_transform IS the zip `o = fn(x)`: an object result is the ordinary store of a
			// call result (a temporary; the return-site rule copies anything fn could name), so
			// structs come out, independent of the source, on both backends
			auto e = make_engine();
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				class P { int a = 1; string s = "x"; array<int> xs = [1]; P() {} P(int v) { a = v; xs = [v, v]; } }
				array<P> ps = [];
				for (int i = 0; i < 200; i++) { ps.push(P(i)); }
				P identity(P p) { return p; }
				P doubled(P p) { p.a = p.a * 2; p.xs.push(9); return p; }
				var out1 = parallel_transform(ps, identity);
				var out2 = parallel_transform(ps, doubled);
				out1[3].a = -1; out1[3].xs.push(77); out1[3].s = "changed";
				int sum = 0; for (auto& p : ps) { sum = sum + p.a + p.xs.size(); }
				return [out1.size(), out1[7].a, out2[7].a, out2[7].xs.size(), ps[3].a, ps[3].xs.size(), ps[3].s, sum, out1[7].s];
			)");
			const auto& v = r.as_array();
			check_eq((int64_t)200, v[0].as_int());
			check_eq((int64_t)7, v[1].as_int());
			check_eq((int64_t)14, v[2].as_int());
			check_eq((int64_t)3, v[3].as_int());
			check_eq((int64_t)3, v[4].as_int());
			check_eq((int64_t)2, v[5].as_int());
			check_eq(std::string("x"), v[6].as<std::string>());
			check_eq((int64_t)(199 * 200 / 2 + 200 * 2), v[7].as_int());
			check_eq(std::string("x"), v[8].as<std::string>());
		});

		test("grafted_fn_body_keeps_return_conversion_and_early_returns", [this]() {
			// A read-only fn's body runs grafted into the chunk loop (no call per element): a
			// converting typed return still converts (float from an int expression widens, string
			// takes a utf8_string's bytes), a proven same-type return lands as is, `var` stays dynamic, an early
			// top-level return picks its value, a return inside a nested loop takes the call form
			auto e = make_engine();
			e->parallel_thread_count(3);
			auto r = e->execute(R"(
				class P { float a = 0.0; int n = 0; P() {} P(int v) { a = v * 0.5; n = v; } }
				array<P> ps = [];
				for (int i = 0; i < 90; i++) { ps.push(P(i)); }
				float widened(P p) { return p.n * 3; }
				float same(P p) { return p.a; }
				string text(P p) { utf8_string u = `${p.n}`; return u; }
				var dynamic(P p) { if (p.n % 2 == 0) { return p.n; } return p.a; }
				int early(P p) { if (p.n < 10) { return -1; } if (p.n < 20) { return -2; } return p.n; }
				int nested(P p) { for (int k = 0; k < 5; k++) { if (k == p.n % 5) { return k * 100 + p.n; } } return 0; }
				var t = parallel_transform(ps, widened);
				var s = parallel_transform(ps, same);
				var x = parallel_transform(ps, text);
				var d = parallel_transform(ps, dynamic);
				var y = parallel_transform(ps, early);
				var z = parallel_transform(ps, nested);
				return [t[7], s[7], x[7], d[6], d[7], y[5], y[15], y[25], z[13], z[89]];
			)");
			const auto& v = r.as_array();
			check_true(v[0].is_float()); check_near(21.0, v[0].as<double>(), 1e-12);
			check_true(v[1].is_float()); check_near(3.5, v[1].as<double>(), 1e-12);
			check_true(v[2].is_string()); check_eq(std::string("7"), v[2].as<std::string>());
			check_true(v[3].is_int()); check_eq((int64_t)6, v[3].as_int());
			check_true(v[4].is_float()); check_near(3.5, v[4].as<double>(), 1e-12);
			check_eq((int64_t)-1, v[5].as_int());
			check_eq((int64_t)-2, v[6].as_int());
			check_eq((int64_t)25, v[7].as_int());
			check_eq((int64_t)313, v[8].as_int());
			check_eq((int64_t)489, v[9].as_int());
			// a grafted return that cannot convert raises the function's return text, as the call form of the transform does, proofs on and
			// off (get of a missing key is null)
			for (const bool proofs : {true, false}) {
				auto m = make_engine();
				m->type_proofs(proofs);
				auto raised = [&](const char* fn) {
					std::string got = "no error";
					try { m->execute(std::string(R"(map<string, int> M = {"a": 1}; int pick(int x) { return M.get("k"); })"
					                             R"( int pick_call(int x) { for (int i = 0; i < 2; ++i) { if (i == 1) { return M.get("k"); } } return 0; })"
					                             " parallel_transform([0, 1, 2], ") + fn + ");"); }
					catch (const std::exception& ex) { got = ex.what(); }
					return got;
				};
				const std::string grafted = raised("pick");
				check_true(grafted.rfind("Cannot return null from ", 0) == 0, std::string(proofs ? "proofs on: " : "proofs off: ") + grafted);
				check_eq(raised("pick_call"), grafted, proofs ? "the graft raises the call form's text, proofs on" : "the graft raises the call form's text, proofs off");
			}
		});

		test("by_ref_loop_cursor_dies_with_its_loop", [this]() {
			// A slotted by-reference range-for used to leave its last element cursor in the loop
			// variable's slot until the frame returned; a parallel region over the same typed
			// object array then saw a live reference and refused the last element. The cursor
			// now dies with the loop on a normal exit, a break, and an unwind caught in the frame
			auto e = make_engine();
			e->parallel_thread_count(3);
			auto r = e->execute(R"(
				class C { int a = 0; array<int> xs = [1, 2]; C() {} }
				array<C> cs = [];
				for (int i = 0; i < 6; i++) { cs.push(new C()); }
				function step(var& t) -> void { t.a = t.a + 1; }
				function plain(int tick) -> int {
					for (auto& t : cs) { t.a = tick; }
					parallel_for (auto& t : cs) { step(t); }
					return cs[5].a;
				}
				function broken(int tick) -> int {
					for (auto& t : cs) { t.a = tick; if (t.a == tick) { break; } }
					parallel_for (auto& t : cs) { step(t); }
					return cs[0].a;
				}
				function thrown(int tick) -> int {
					try { for (auto& t : cs) { t.a = tick; throw "stop"; } } catch (err) { }
					parallel_for (auto& t : cs) { step(t); }
					return cs[0].a;
				}
				return [plain(1), plain(2), broken(10), thrown(20)];
			)");
			const auto& v = r.as_array();
			check_eq((int64_t)2, v[0].as_int());
			check_eq((int64_t)3, v[1].as_int());
			check_eq((int64_t)11, v[2].as_int());
			check_eq((int64_t)21, v[3].as_int());
		});

		test("typed_string_capture_detaches_every_string", [this]() {
			// A typed array<string> captured by a body is provisioned per worker through
			// parallel_detached_copy. The str buffer holds refcounted handles, so the copy must
			// own fresh strings (the buffer copy shared them across threads: GLOOM's palettes
			// rendered short rows under workers once they were declared array<string>)
			auto e = make_engine();
			e->execution_budget(0);
			e->parallel_thread_count(4);
			script_value table = e->execute(R"(
				array<string> PAL = [];
				PAL = [" "];
				for (int i = 1; i < 40; i++) { PAL.push(`[38;2;${i};${i * 2};${i * 3}m`); }
				return PAL;
			)");
			const script_array* src = table.deref().unchecked_array_node();
			check_true(src->is_typed());
			script_value copy = table.deref().parallel_detached_copy(e.get());
			const script_array* dst = copy.unchecked_array_node();
			check_true(dst->is_typed());
			check_eq(src->size(), dst->size());
			for (size_t i = 0; i < src->size(); ++i) {
				check_true(src->strs()[i].get() != dst->strs()[i].get());
				check_eq(*src->strs()[i], *dst->strs()[i]);
			}
			// and the GLOOM shape end to end: rows built from captured typed string tables
			// match the serial rows on every worker's chunk
			auto r = e->execute(R"(
				array<string> GLYPH = [" ", "â", "â", "â", "â"];
				array<int> PIXV = [];
				for (int i = 0; i < 32 * 40; i++) { PIXV.push((i * 7 + i / 32) % 40); }
				array<int> idx = [];
				for (int i = 0; i < 40; i++) { idx.push(i); }
				string row(int r) {
					var parts = [];
					int prev = -1;
					for (int cx = 0; cx < 32; ++cx) {
						int p = PIXV[r * 32 + cx];
						if (p != prev) { parts.push(PAL[p]); prev = p; }
						parts.push(GLYPH[(p + cx) % 5]);
					}
					parts.push("[0m");
					return parts.join("");
				}
				int bad = 0;
				for (int rep = 0; rep < 6; rep++) {
					var par = parallel_transform(idx, row);
					for (int i = 0; i < 40; i++) { if (par[i] != row(i)) { bad++; } }
				}
				return bad;
			)");
			check_eq((int64_t)0, r.as_int());
		});

		test("store_into_parameter_copy_leaves_source_unchanged", [this]() {
			auto e = make_engine();
			const std::string src = R"(
				class P { int a = 1; string s = "x"; P() {} P(int v) { a = v; } }
				array<P> ps = [];
				for (int i = 0; i < 40; i++) { ps.push(P(i)); }
				int f(P p) { p.a = p.a * 10; p.s = "changed"; p.a++; return p.a; }
				var pout = parallel_transform(ps, f);
				var total = 0;
				for (auto p : ps) { total += p.a; }
				return [pout[3], total, ps[3].s, pout.size()];
			)";
			for (size_t workers : {size_t(1), size_t(8)}) {
				e->parallel_thread_count(workers);
				auto r = e->execute(src);
				const auto& vals = r.as_array();
				check_eq((int64_t)31, vals[0].as_int());
				check_eq((int64_t)780, vals[1].as_int());
				check_eq(std::string("x"), vals[2].as<std::string>());
				check_eq((int64_t)40, vals[3].as_int());
			}
		});

		test("rejects_impure_weight_fn", [this]() {
			auto e = make_engine();
			auto r = e->execute(R"(
				var seen = 0;
				int f(int x) { return x; }
				float w(int x) { seen = seen + 1; return 1.0; }
				try { parallel_transform([1, 2, 3], f, w); } catch (e) { return e; }
				return "no-error";
			)");
			check_true(r.as<std::string>().find("body writes enclosing state 'seen'") != std::string::npos);
			// a weight runs at every worker count, so what it raises never depends on the count, and its own throw arrives intact
			for (size_t workers : {size_t(1), size_t(8)}) {
				e->parallel_thread_count(workers);
				auto got = e->execute(R"(
					int inc(int v) { return v + 1; }
					float neg(int v) { return -1.0; }
					float boom(int v) { if (v == 3) { throw "weight boom"; } return 1.0; }
					string named(int v) { return "x"; }
					float by_ref(int& v) { return 1.0; }
					var xs = [1, 2, 3, 4, 5];
					string out = "";
					try { parallel_transform(xs, inc, by_ref); } catch (e) { out += e + "|"; }
					try { parallel_transform(xs, inc, neg); } catch (e) { out += e + "|"; }
					try { parallel_transform(xs, inc, boom); } catch (e) { out += e + "|"; }
					try { parallel_transform(xs, inc, named); } catch (e) { out += e; }
					return out;
				)").as<std::string>();
				check_eq(std::string("parallel_transform: weight_fn must take exactly one parameter by value|"
				                     "parallel_transform: weight_fn must return a finite non-negative number (element 0)|weight boom|"
				                     "parallel_transform: weight_fn must return a number (element 0)"), got, std::to_string(workers) + " workers");
			}
		});

		// === Errors inside the region ===

		test("first_error_in_iteration_order_wins", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(8);
			auto r = e->execute(R"(
				int f(int x) {
					if (x == 90) { throw "boom90"; }
					if (x == 5) { throw "boom5"; }
					return x;
				}
				var a = [];
				for (var i = 0; i < 100; i++) { a.push(i); }
				try { parallel_transform(a, f); } catch (e) { return e; }
				return "no-error";
			)");
			auto msg = r.as<std::string>();
			check_true(msg.find("boom5") != std::string::npos);
			check_true(msg.find("boom90") == std::string::npos);
		});

		test("fn_internal_catch_keeps_region_alive", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				int f(int x) {
					try { if (x % 2 == 0) { throw "even"; } return x; }
					catch (e) { return -x; }
				}
				var a = [];
				for (var i = 0; i < 40; i++) { a.push(i); }
				var pout = parallel_transform(a, f);
				var sout = [];
				for (auto x : a) { sout.push(f(x)); }
				if (pout.size() != sout.size()) { return false; }
				for (var i = 0; i < pout.size(); i++) { if (pout[i] != sout[i]) { return false; } }
				return true;
			)");
			check_true(r.as<bool>());
		});

		test("worker_budget_overrun_is_terminal", [this]() {
			auto e = make_engine();
			e->execution_budget(0.05);
			e->parallel_thread_count(4);
			// One worker's infinite fn fails the whole call terminally: the script-level
			// catch must NOT swallow it (budget overruns latch the terminal rail).
			bool host_saw_failure = false;
			try {
				auto r = e->execute(R"(
					int f(int x) { while (true) { var y = x + 1; } return x; }
					var a = [];
					for (var i = 0; i < 32; i++) { a.push(i); }
					try { parallel_transform(a, f); } catch (e) { return "swallowed"; }
					return "done";
				)");
				host_saw_failure = r.is_null() || (r.is_string() && r.as<std::string>() != "swallowed" && r.as<std::string>() != "done");
			} catch (const std::exception&) {
				host_saw_failure = true;
			}
			check_true(host_saw_failure);
		});

		test("worker_memory_cap_fails_region", [this]() {
			auto e = make_engine();
			e->memory_cap(64 * 1024);
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				string f(int x) {
					var s = "block";
					for (var i = 0; i < 20; i++) { s = s + s; }
					return s;
				}
				var a = [];
				for (var i = 0; i < 32; i++) { a.push(i); }
				try { parallel_transform(a, f); } catch (e) { return e; }
				return "no-error";
			)");
			check_true(r.as<std::string>().find("memory cap") != std::string::npos);
		});

		// === Weight hint (the builtin's third argument IS the hint - no new syntax) ===

		test("weight_hint_shifts_chunk_bounds_output_identical", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(4);
			const std::string flat_src = R"(
				int f(int x) { return x * 2; }
				var a = [];
				for (var i = 0; i < 64; i++) { a.push(i); }
				return parallel_transform(a, f);
			)";
			const std::string weighted_src = R"(
				int f(int x) { return x * 2; }
				float w(int x) { if (x < 8) { return 1000.0; } return 1.0; }
				var a = [];
				for (var i = 0; i < 64; i++) { a.push(i); }
				return parallel_transform(a, f, w);
			)";
			// adaptive fan-out is a performance hint the measured cost drives: without it the worker count is the engine's, and the
			// hint's bounds are the cumulative-weight split of the weights alone
			const auto without_measured_cost = [&] {
				auto& state = detail::engine_access::parallel_state(*e);
				state.worker_overhead_ns = 0.0;
				for (auto& pool : state.region_pools) { pool.ns_per_element = 0.0; }
			};
			without_measured_cost();
			auto flat = e->execute(flat_src);
			auto flat_bounds = detail::engine_access::last_parallel_chunk_bounds(*e);   // what each thread took: follows timing, so only its shape is asserted
			without_measured_cost();
			auto weighted = e->execute(weighted_src);
			auto weighted_bounds = detail::engine_access::last_parallel_chunk_bounds(*e);
			check_true(arrays_equal(flat, weighted), "observable output identical");
			check_eq((size_t)5, flat_bounds.size());
			check_eq((size_t)0, flat_bounds.front());
			check_eq((size_t)64, flat_bounds.back());
			check_true(std::is_sorted(flat_bounds.begin(), flat_bounds.end()), "each thread's count is non-negative");
			// weights 1000 x 8 then 1 x 56 (total 8056): each boundary is the first element whose prefix reaches k / 4 of the total,
			// all inside the heavy prefix where an even split would put the first at 16
			check_true(weighted_bounds == std::vector<size_t>{0, 3, 5, 7, 64}, "the weighted split is the weights' cumulative split");
			without_measured_cost();
			e->execute(weighted_src);
			check_true(weighted_bounds == detail::engine_access::last_parallel_chunk_bounds(*e), "the same region chooses the same bounds again");
		});

		// A worker remembers which function each of its call sites validated (vm_backend::worker_callee_memo_): rows hold raw node
		// addresses, written and read only by the worker's own thread, cleared before every region; a provisioned callee lives in
		// the worker's root for the whole region, so an address cannot name another function until the next clear. The shared
		// sites' caches stay main-thread. Inlined leaves and parked calls, many regions, and a redefinition between regions, with
		// one worker and several (the XTHREAD canary build checks that no worker touches a shared count).
		test("worker_callee_memo_follows_redefinitions_between_regions", [this]() {
			for (size_t workers : {size_t(1), size_t(4)}) {
				auto e = make_engine();
				e->parallel_thread_count(workers);
				e->execute(R"(
					int leaf(int x) { return x * 3 + 1; }
					int big(int x) { int s = 0; for (int i = 0; i < 3; ++i) { s += x + i; } return s; }
					int f(int x) { return leaf(x) + big(x) + leaf(x + 1); }
					var a = [];
					for (var i = 0; i < 64; i++) { a.push(i); }
				)");
				auto run = [&]() { return e->execute("var r = parallel_transform(a, f); return [r[0], r[5], r[63]];").as_array(); };
				for (int round = 0; round < 3; ++round) {
					auto r = run();
					check_eq((int64_t)(1 + 3 + 4), r[0].as_int(), "leaf + big + leaf at 0");
					check_eq((int64_t)(16 + 18 + 19), r[1].as_int(), "at 5");
					check_eq((int64_t)(190 + 192 + 193), r[2].as_int(), "at 63");
				}
				e->execute("int leaf(int x) { return x - 2; }");
				for (int round = 0; round < 2; ++round) {
					auto r = run();
					check_eq((int64_t)(-2 + 3 - 1), r[0].as_int(), "the redefined leaf at 0");
					check_eq((int64_t)(3 + 18 + 4), r[1].as_int(), "the redefined leaf at 5");
				}
				e->execute("int big(int x) { return 100; }");
				auto r = run();
				check_eq((int64_t)(-2 + 100 - 1), r[0].as_int(), "the redefined big at 0");
				check_eq((int64_t)(61 + 100 + 62), r[2].as_int(), "the redefined big at 63");
			}
		});

		test("admission_recheck_after_function_redefine", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(2);
			e->execute("int helper(int x) { return x + 1; }");
			auto r1 = e->execute(R"(
				int f(int x) { return helper(x); }
				return parallel_transform([10, 20, 30, 40, 50, 60, 70, 80,
				                           11, 21, 31, 41, 51, 61, 71, 81], f);
			)");
			check_eq((int64_t)11, r1.as_array()[0].as_int());
			// Hot-reload the transitive target: the cached admission graph must notice
			e->execute("int helper(int x) { return x + 5; }");
			auto r2 = e->execute(R"(
				int f2(int x) { return helper(x); }
				return parallel_transform([10, 20, 30, 40, 50, 60, 70, 80,
				                           11, 21, 31, 41, 51, 61, 71, 81], f2);
			)");
			check_eq((int64_t)15, r2.as_array()[0].as_int());
		});

		// Fuzz-style battery: seed-derived pure fns over int and string arrays, run at
		// one worker vs many, element-wise compared against each other AND a hand serial
		// map. Fully deterministic (constants derive from the seed, no wall clock).
		test("fuzz_pure_fns_1_vs_N", [this]() {
			auto e = make_engine();
			e->execution_budget(0);
			jai::stdlib::register_all(*e);
			auto mix = [](uint64_t z) {
				z ^= z >> 33; z *= 0xFF51AFD7ED558CCDULL;
				z ^= z >> 33; z *= 0xC4CEB9FE1A85EC53ULL;
				z ^= z >> 33; return z;
			};
			for (uint64_t seed = 1; seed <= 32; ++seed) {
				const uint64_t h = mix(seed);
				const int64_t k1 = static_cast<int64_t>(h % 13) + 2;
				const int64_t k2 = static_cast<int64_t>((h >> 8) % 29) - 14;
				const int64_t k3 = static_cast<int64_t>((h >> 16) % 7) + 1;
				const int shape = static_cast<int>((h >> 24) % 3);
				std::string body;
				switch (shape) {
				case 0:
					body = "var y = x * " + std::to_string(k1) + " + " + std::to_string(k2) + ";"
					       "var z = (x % " + std::to_string(k3) + ") - y;"
					       "return z * 2 + y % " + std::to_string(k1) + ";";
					break;
				case 1:
					body = "var y = x < " + std::to_string(k2) + " ? x + " + std::to_string(k1) + " : x - " + std::to_string(k1) + ";"
					       "var total = 0;"
					       "for (var j = 0; j < (x % 5 + 1); j++) { total += y + j; }"
					       "return total;";
					break;
				default:
					body = "var s = to_string(x * " + std::to_string(k1) + ");"
					       "return s.size() + x % " + std::to_string(k3) + ";";
					break;
				}
				const std::string src =
					"var a = [];"
					"for (var i = -20; i < 44; i++) { a.push(i * " + std::to_string(k3) + "); }"
					"int f(int x) { " + body + " }"
					"var pout = parallel_transform(a, f);"
					"var sout = [];"
					"for (auto x : a) { sout.push(f(x)); }"
					"return [pout, sout];";
				e->parallel_thread_count(1);
				auto r1 = e->execute(src);
				e->parallel_thread_count(7);
				auto rn = e->execute(src);
				check_true(r1.is_array());
				check_true(rn.is_array());
				check_true(deep_equal(r1.as_array()[0], r1.as_array()[1]));   // parallel == serial (1 worker)
				check_true(deep_equal(rn.as_array()[0], rn.as_array()[1]));   // parallel == serial (7 workers)
				check_true(deep_equal(r1.as_array()[0], rn.as_array()[0]));   // 1-vs-N byte parity
			}
			// String-array shapes
			for (uint64_t seed = 1; seed <= 8; ++seed) {
				const uint64_t h = mix(seed * 977);
				const int64_t k = static_cast<int64_t>(h % 5) + 2;
				const std::string src =
					"var a = [];"
					"for (var i = 0; i < 48; i++) { a.push(\"w\" + to_string(i * " + std::to_string(k) + ") + \"_tail\"); }"
					"string g(string s) {"
					"  var t = s + \"|\" + to_string(s.size() % " + std::to_string(k) + ");"
					"  return t.substr(0, t.size() % 9 + 3);"
					"}"
					"var pout = parallel_transform(a, g);"
					"var sout = [];"
					"for (auto x : a) { sout.push(g(x)); }"
					"return [pout, sout];";
				e->parallel_thread_count(1);
				auto r1 = e->execute(src);
				e->parallel_thread_count(6);
				auto rn = e->execute(src);
				check_true(deep_equal(r1.as_array()[0], r1.as_array()[1]));
				check_true(deep_equal(rn.as_array()[0], rn.as_array()[1]));
				check_true(deep_equal(r1.as_array()[0], rn.as_array()[0]));
			}
		});

		test("slot_reuse_across_calls_consistent", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(4);
			const std::string src = R"(
				int f(int x) { return x * 7 - 3; }
				var a = [];
				for (var i = 0; i < 64; i++) { a.push(i); }
				return parallel_transform(a, f);
			)";
			auto r1 = e->execute(src);
			auto r2 = e->execute(src);   // slots reused (same fingerprint)
			auto r3 = e->execute(src);
			check_true(arrays_equal(r1, r2));
			check_true(arrays_equal(r1, r3));
		});

		test("slot_reuse_after_error_is_pristine", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(4);
			// First call fails mid-chunk (residual unwinding state in some slot)
			auto r1 = e->execute(R"(
				int f(int x) { if (x == 40) { throw "mid"; } return x; }
				var a = [];
				for (var i = 0; i < 64; i++) { a.push(i); }
				try { parallel_transform(a, f); } catch (e) { return e; }
				return "no-error";
			)");
			check_true(r1.as<std::string>().find("mid") != std::string::npos);
			// Second call on the SAME engine reuses the slots and must run clean
			auto r2 = e->execute(R"(
				int g(int x) { return x + 1; }
				var a = [];
				for (var i = 0; i < 64; i++) { a.push(i); }
				var out = parallel_transform(a, g);
				return out[63];
			)");
			check_eq((int64_t)64, r2.as_int());
		});

	}
};

// parallel_for v1: the in-place fork-join statement.
// auto& mutates the owned element in place (no output array, no merge); plain auto is
// a side-effect-free worker copy. Elements must be value-closed (primitives, strings,
// value containers, flat classes); the barrier detaches shared string nodes instead of
// rejecting them. Determinism: identical results at every worker count.
class parallel_for_tests : public suite {
public:
	parallel_for_tests() : suite("Parallel For") {}

	// Runs `source` (returns [parallel_result, serial_expectation]) at several worker
	// counts and checks equality each time.
	void check_matches_serial_at(std::shared_ptr<engine> e, const std::string& source,
	                             std::initializer_list<size_t> worker_counts) {
		for (size_t workers : worker_counts) {
			e->parallel_thread_count(workers);
			auto r = e->execute(source);
			check_true(r.is_array());
			const auto& pair = r.as_array();
			check_eq((size_t)2, pair.size());
			check_true(parallel_transform_tests::deep_equal(pair[0], pair[1]));
		}
	}

	detail::parallel_capture_kind capture_kind(const std::shared_ptr<engine>& e, const char* name) {
		const uint64_t id = detail::engine_access::symbolize(*e, name);
		for (const auto& [seen, kind] : detail::engine_access::last_parallel_captures(*e)) {
			if (seen == id) { return kind; }
		}
		check_true(false, std::string("no capture named ") + name);
		return detail::parallel_capture_kind::written;
	}
	std::string error_of(const std::shared_ptr<engine>& e, const std::string& source) {
		std::string result = "no error";
		try { e->execute(source); } catch (const std::exception& ex) { result = ex.what(); }
		return result;
	}

	void forge_tests() override {
		test("const_ref_parameters_take_captured_state_as_a_captured_read", [this]() {
			// Enclosing state passed to a const T& parameter is read in place: a callee that only subscripts it (directly, through
			// another const T& parameter, recursively, or with the count builtins) keeps the borrow tier, so no worker copies it
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			const std::string src = R"(
				array<int> grid = []; array<int> idx = []; array<int> out = []; array<int> serial = [];
				for (int i = 0; i < 1000; i++) { grid.push(i * 3 % 997); idx.push(i); out.push(0); serial.push(0); }
				map<string, int> weights = {"a": 2, "b": 5};
				string label = "abc";
				int at_c(const array<int>& g, int i) { return g[i] * 2; }
				int inner(const array<int>& h, int i) { return h[i] + 1; }
				int outer(const array<int>& g, int i) { return inner(g, i) * 2; }
				int rec(const array<int>& g, int i) { return i <= 0 ? g[0] : g[i] + rec(g, i - 1); }
				int last(const array<int>& g, int i) { return g.empty() ? -1 : g[g.size() - 1 - i % g.length()]; }
				int wt(const map<string, int>& m) { return m["a"] + m["b"] + m.size(); }
				int ln(const string& s) { return s.size(); }
				int el(const int& v) { return v * 10; }
				int f(int i) { return at_c(grid, i) + outer(grid, i) + rec(grid, i % 5) + last(grid, i) + wt(weights) + ln(label) + el(grid[i]); }
				parallel_for (int i : idx) { out[i] = f(i); }
				for (int i : idx) { serial[i] = f(i); }
				return [to_json(out), to_json(serial)];
			)";
			check_matches_serial_at(e, src, {1, 4, 8});
			check_eq((int)detail::parallel_capture_kind::borrow, (int)capture_kind(e, "grid"));
			check_eq((int)detail::parallel_capture_kind::borrow, (int)capture_kind(e, "weights"));
		});

		test("const_ref_parameters_used_whole_read_a_worker_copy", [this]() {
			// A callee that iterates its const T& parameter reads it whole: the capture is a per-worker snapshot, as a direct
			// whole-value read is, and the results match the serial loop
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			const std::string src = R"(
				array<int> grid = []; array<int> idx = []; array<int> out = []; array<int> serial = [];
				for (int i = 0; i < 300; i++) { grid.push(i % 17); idx.push(i); out.push(0); serial.push(0); }
				int count_below(const array<int>& g, int limit) { int c = 0; for (int v : g) { if (v < limit) { c++; } } return c; }
				parallel_for (int i : idx) { out[i] = count_below(grid, i % 17); }
				for (int i : idx) { serial[i] = count_below(grid, i % 17); }
				return [to_json(out), to_json(serial)];
			)";
			check_matches_serial_at(e, src, {1, 4, 8});
			check_eq((int)detail::parallel_capture_kind::snapshot, (int)capture_kind(e, "grid"));
			// a whole use reached through mutual recursion (the forwarding call meets its target mid-walk) settles the same way
			const std::string mutual = R"(
				array<int> grid = []; array<int> idx = []; array<int> out = []; array<int> serial = [];
				for (int i = 0; i < 300; i++) { grid.push(i % 17); idx.push(i); out.push(0); serial.push(0); }
				int fwd(const array<int>& g, int i) { return i > 0 ? whole(g, i - 1) : g[0]; }
				int whole(const array<int>& x, int i) { if (i > 0) { return fwd(x, i - 1); } int c = 0; for (int v : x) { c += v; } return c; }
				parallel_for (int i : idx) { out[i] = fwd(grid, i % 4); }
				for (int i : idx) { serial[i] = fwd(grid, i % 4); }
				return [to_json(out), to_json(serial)];
			)";
			check_matches_serial_at(e, mutual, {1, 4, 8});
			check_eq((int)detail::parallel_capture_kind::snapshot, (int)capture_kind(e, "grid"));
		});

		test("const_ref_of_state_the_region_writes_is_refused", [this]() {
			// The index-written array and the range itself change during the region: a const T& of either would read slots
			// other elements write
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			e->parallel_thread_count(4);
			const std::string prelude = "array<int> idx = [0, 1, 2, 3]; array<int> out = [0, 0, 0, 0]; int r(const array<int>& g, int i) { return g[i]; }\n";
			const std::string written = error_of(e, prelude + "parallel_for (int i : idx) { out[i] = r(out, i); }");
			check_true(written.rfind("parallel_for: body touches 'out' beyond the slot it writes, out[i] at 2:", 0) == 0, written);
			const std::string range = error_of(e, prelude + "parallel_for (auto& x : idx) { x = r(idx, 0); }");
			check_true(range.rfind("parallel_for: captured name 'idx' is the container being mutated (reads through it would race the element writes)", 0) == 0, range);
		});

		test("non_const_ref_parameters_refuse_captured_state_and_name_the_fix", [this]() {
			// A non-const T& parameter is a write channel: refused, with const T& suggested when the callee only reads it; a
			// script method's parameter alike (its receiver class is known at the statement)
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			e->parallel_thread_count(4);
			const std::string prelude = R"(
				array<int> grid = [1, 2, 3]; array<int> idx = [0, 1, 2]; array<int> out = [0, 0, 0];
				int reads(array<int>& g, int i) { return g[i]; }
				int writes(array<int>& g, int i) { g[i] = 0; return i; }
				class W { int v = 0; void m(array<int>& g) { g.push(1); v = g.size(); } void n(const array<int>& g) { v = g[0] + g.size(); } }
				array<W> ws = [W(), W()];
			)";
			const std::string reads = error_of(e, prelude + "parallel_for (int i : idx) { out[i] = reads(grid, i); }");
			check_true(reads.rfind("parallel_for: argument passes enclosing state 'grid' to reference parameter 'g' of 'reads' (captured reads are read-only;"
			                       " 'reads' only reads it: declare it const array<int>& g) at 7:", 0) == 0, reads);
			const std::string writes = error_of(e, prelude + "parallel_for (int i : idx) { out[i] = writes(grid, i); }");
			check_true(writes.rfind("parallel_for: argument passes enclosing state 'grid' to reference parameter 'g' of 'writes', which writes it (captured reads"
			                        " are read-only; pass a copy, or take it as const T& where it is only read) at 7:", 0) == 0, writes);
			const std::string method = error_of(e, prelude + "parallel_for (auto& w : ws) { w.m(grid); }");
			check_true(method.rfind("parallel_for: argument passes enclosing state 'grid' to reference parameter 'g' of 'W::m', which writes it", 0) == 0, method);
			check_eq((int64_t)4, e->execute(prelude + "parallel_for (auto& w : ws) { w.n(grid); } return ws[0].v + ws[1].v - 4;").as_int());
			// a conditional's arms bind a reference only where the parameter is one: a by-value parameter reads the arm picked
			check_eq(std::string("[34,12,18]"), e->execute(prelude + "int twice(int v) { return v * 2; } int a0 = 5; int b0 = 7;"
			                                               " parallel_for (int i : idx) { int lo = i; int hi = i + 10; out[i] = twice(i > 0 ? lo : hi) + twice(i == 1 ? a0 : b0); }"
			                                               " return to_json(out);").as<std::string>());
			const std::string arm = error_of(e, prelude + "int bump(int& v) { v += 1; return v; } int a0 = 5;"
			                                    " parallel_for (int i : idx) { int mine = 0; out[i] = bump(i > 0 ? mine : a0); }");
			check_true(arm.rfind("parallel_for: argument passes enclosing state 'a0' to reference parameter 'v' of 'bump', which writes it", 0) == 0, arm);
			const std::string method_arm = error_of(e, prelude + "parallel_for (auto& w : ws) { array<int> mine = [1]; w.m(w.v > 0 ? mine : grid); }");
			check_true(method_arm.rfind("parallel_for: argument passes enclosing state 'grid' to reference parameter 'g' of 'W::m', which writes it", 0) == 0, method_arm);
			const std::string cast = error_of(e, prelude + "parallel_for (int i : idx) { out[i] = cast<int&>(grid[i]); }");
			check_true(cast.rfind("parallel_for: cast<T&> is not allowed in a parallel body at 7:", 0) == 0, cast);
			// a member of captured state is that state's storage: a writing T& bound to it, by an argument or a default, refuses
			const std::string objects = prelude + "int ga = 1; int gb = 2; class C { int f = 1; array<int> a = [1]; C(int& x = ga) {} } C gc;"
			                                      " void setf(int& x) { x = 9; } void setd(int& x = ga) { x = 9; } int readd(int& x = ga) { return x; }"
			                                      " class D { int v = 0; void m(int& x = gc.f) { x = 9; } } array<D> ds = [D(), D()];";
			const std::string member = error_of(e, objects + " parallel_for (int i : idx) { setf(gc.f); }");
			check_true(member.rfind("parallel_for: argument passes enclosing state 'gc' to reference parameter 'x' of 'setf', which writes it", 0) == 0, member);
			const std::string member_element = error_of(e, objects + " parallel_for (int i : idx) { setf(gc.a[0]); }");
			check_true(member_element.rfind("parallel_for: argument passes enclosing state 'gc' to reference parameter 'x' of 'setf', which writes it", 0) == 0, member_element);
			const std::string member_method = error_of(e, objects + " parallel_for (auto& w : ws) { w.m(gc.a); }");
			check_true(member_method.rfind("parallel_for: argument passes enclosing state 'gc' to reference parameter 'g' of 'W::m', which writes it", 0) == 0, member_method);
			const std::string nested = error_of(e, objects + " void outer() { setf(gc.f); } parallel_for (int i : idx) { outer(); }");
			check_true(nested.rfind("parallel_for: argument passes enclosing state 'gc' to reference parameter 'x' of 'setf', which writes it", 0) == 0, nested);
			const std::string defaulted = error_of(e, objects + " parallel_for (int i : idx) { setd(); }");
			check_true(defaulted.rfind("parallel_for: reference parameter 'x' of 'setd' defaults to enclosing state 'ga', which writes it", 0) == 0, defaulted);
			const std::string default_read = error_of(e, objects + " parallel_for (int i : idx) { out[i] = readd(); }");
			check_true(default_read.rfind("parallel_for: reference parameter 'x' of 'readd' defaults to enclosing state 'ga' (captured reads are read-only;"
			                              " 'readd' only reads it: declare it const int& x)", 0) == 0, default_read);
			const std::string default_member = error_of(e, objects + " parallel_for (auto& d : ds) { d.m(); }");
			check_true(default_member.rfind("parallel_for: reference parameter 'x' of 'D::m' defaults to enclosing state 'gc', which writes it", 0) == 0, default_member);
			const std::string default_arm = error_of(e, objects + " bool pick = true; void setc(int& x = pick ? ga : gb) { x = 9; } parallel_for (int i : idx) { setc(); }");
			check_true(default_arm.rfind("parallel_for: reference parameter 'x' of 'setc' defaults to enclosing state 'ga', which writes it", 0) == 0, default_arm);
			const std::string default_ctor = error_of(e, objects + " parallel_for (int i : idx) { C made(); }");
			check_true(default_ctor.rfind("parallel_for: reference parameter 'x' of 'C' defaults to enclosing state 'ga'", 0) == 0, default_ctor);
			// a default a call overrides binds nothing, and a const T& default is a read
			check_eq(std::string("[1,1,1]"), e->execute(objects + " int keepd(const int& x = ga) { return x; }"
			                                            " parallel_for (int i : idx) { int mine = 0; setd(mine); out[i] = mine / 9 * keepd(); } return to_json(out);").as<std::string>());
		});

		test("a_loop_variable_a_reference_parameter_writes_is_written", [this]() {
			// An index write's loop variable changed through a function's or a method's writing T& parameter would point
			// iterations at one slot; a T& the callee only reads leaves it alone
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			e->parallel_thread_count(4);
			const std::string prelude = R"(
				void zero(int& x) { x = 0; }
				int peek(int& x) { return x; }
				class B { int v = 0; void reset(int& x) { x = 0; } }
				array<int> idx = [0, 1, 2, 3, 4, 5, 6, 7]; array<int> out = [0, 0, 0, 0, 0, 0, 0, 0]; array<B> bs = [B(), B(), B(), B(), B(), B(), B(), B()];
			)";
			const std::string function = error_of(e, prelude + "parallel_for (int i : idx) { zero(i); out[i] = out[i] + 1; }");
			check_true(function.rfind("parallel_for: body writes enclosing state 'out' at an index the body changes at 6:", 0) == 0, function);
			const std::string method = error_of(e, prelude + "parallel_for (auto& b, int i : bs, idx) { b.reset(i); out[i] = 1; }");
			check_true(method.rfind("parallel_for: body writes enclosing state 'out' at an index the body changes at 6:", 0) == 0, method);
			const std::string arm = error_of(e, prelude + "parallel_for (int i : idx) { int other = 0; zero(i > 3 ? i : other); out[i] = out[i] + 1; }");
			check_true(arm.rfind("parallel_for: body writes enclosing state 'out' at an index the body changes at 6:", 0) == 0, arm);
			const std::string method_arm = error_of(e, prelude + "parallel_for (auto& b, int i : bs, idx) { int other = 0; b.reset(i > 3 ? i : other); out[i] = 1; }");
			check_true(method_arm.rfind("parallel_for: body writes enclosing state 'out' at an index the body changes at 6:", 0) == 0, method_arm);
			// a by-value element a conditional's arm hands a writing T& is the iteration's copy, never the array's slot
			check_eq(std::string("[1,2,3,4,5,6,7,8]"), e->execute(prelude + "array<int> vals = [1, 2, 3, 4, 5, 6, 7, 8];"
			                                                      " parallel_for (int v : vals) { int other = 0; zero(v > 2 ? v : other); } return to_json(vals);").as<std::string>());
			check_eq(std::string("[1,2,3,4,5,6,7,8]"), e->execute(prelude + "parallel_for (int i : idx) { out[i] = peek(i) + 1; } return to_json(out);").as<std::string>());
			// a callee's own variable of the loop variable's name is not the loop variable
			check_eq(std::string("[0,0,1,3,6,10,15,21]"), e->execute(prelude + "int sum_below(int x) { int s = 0; for (int i = 0; i < x; i++) { s += i; } return s; }"
			                                                          " parallel_for (int i : idx) { out[i] = sum_below(i); } return to_json(out);").as<std::string>());
		});

		test("counted_parallel_for_matches_the_serial_loop", [this]() {
			// parallel_for (int i = a; i < b; i++) visits the values the serial loop gives i: forward, backward, strided, <=, >=,
			// a step read from a variable, and none when the serial loop runs zero times
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			const std::string src = R"(
				int n = 50; int stride = 3; int down = 7;
				array<int> out = []; array<int> serial = [];
				for (int k = 0; k < 64; k++) { out.push(-1); serial.push(-1); }
				int f(int i) { return i * i - 3 * i; }
				parallel_for (int i = 0; i < n; i++) { out[i] = f(i); }
				for (int i = 0; i < n; i++) { serial[i] = f(i); }
				parallel_for (int i = n; i > 40; i--) { out[i] = f(i) + 1; }
				for (int i = n; i > 40; i--) { serial[i] = f(i) + 1; }
				parallel_for (auto i = 1; i <= 61; i += stride) { out[i] = i; }
				for (auto i = 1; i <= 61; i += stride) { serial[i] = i; }
				parallel_for (int i = 63; i >= 0; i -= down) { out[i] = 2 * i; }
				for (int i = 63; i >= 0; i -= down) { serial[i] = 2 * i; }
				parallel_for (int i = 0; i <= 5; ++i) { out[i] = out[i] + 100; }
				for (int i = 0; i <= 5; ++i) { serial[i] = serial[i] + 100; }
				parallel_for (int i = 5; i < 5; i++) { out[i] = 0; }
				parallel_for (int i = 9; i < 2; i += stride) { out[i] = 0; }
				parallel_for (int i = 9; i < 2; i -= stride) { out[i] = 0; }
				return [to_json(out), to_json(serial)];
			)";
			check_matches_serial_at(e, src, {1, 4, 8});
			// the range is the sequence itself, never an index array: a loop the memory cap could never hold the indices of runs, and
			// a store past the written array raises the serial loop's first out-of-bounds text whichever end the sequence leaves by
			e->memory_cap(64 * 1024);
			const std::string bounds = R"(
				array<int> small = [0, 0, 0, 0];
				int ran = 0;
				parallel_for (int i = 0; i < 20000; i++) { int j = i; }
				string texts = "";
				try { parallel_for (int i = -2; i < 9; i++) { small[i] = i; } } catch (e) { texts += e + "|"; }
				try { parallel_for (int i = 0; i < 900; i += 3) { small[i] = i; } } catch (e) { texts += e + "|"; }
				try { parallel_for (int i = 6; i >= -30; i -= 3) { small[i] = i; } } catch (e) { texts += e; }
				texts;
			)";
			for (size_t workers : {size_t(1), size_t(8)}) {
				e->parallel_thread_count(workers);
				check_eq(std::string("Array index -2 out of bounds for array of size 4|Array index 6 out of bounds for array of size 4|"
				                     "Array index 6 out of bounds for array of size 4"), e->execute(bounds).as<std::string>(), std::to_string(workers) + " workers");
			}
		});

		test("counted_parallel_for_refuses_what_it_cannot_prove", [this]() {
			// Only one int variable stepped by a fixed amount toward a bound: every other shape is a load-time error, a step that
			// never ends the serial loop or overflows its last step is refused when the statement is reached, and the body may
			// not write the variable
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			e->parallel_thread_count(4);
			const char* steps = "parallel_for: a counted loop must step its variable by a fixed amount toward a bound (write i++, i--, i += k or i -= k)";
			const char* compares = "parallel_for: a counted loop compares its variable with a bound (i < b, i <= b, i > b or i >= b)";
			const char* declares = "parallel_for: a counted loop declares one int variable (int i = a or auto i = a)";
			const char* toward = "parallel_for: a counted loop must step toward its bound (i < b and i <= b count up, i > b and i >= b count down)";
			struct refusal { const char* source; const char* text; };
			const refusal refusals[] = {
				{ "int n = 10; parallel_for (int i = 0; i < n; i *= 10) { }", steps },
				{ "int n = 10; parallel_for (int i = 0; i < n; i = i + 1) { }", steps },
				{ "int n = 10; int g(int x) { return x + 1; } parallel_for (int i = 0; i < n; i += g(1)) { }", steps },
				{ "int n = 10; int j = 0; parallel_for (int i = 0; j < n; i++) { }", compares },
				{ "int n = 10; parallel_for (int i = 0; ; i++) { }", compares },
				{ "int n = 10; parallel_for (int i = 0; i != n; i++) { }", compares },
				{ "int n = 10; parallel_for (int i = 0, j = 0; i < n; i++) { }", declares },
				{ "int n = 10; parallel_for (float i = 0; i < n; i++) { }", declares },
				{ "int n = 10; parallel_for (int i = 0; i < n; i--) { }", toward },
				{ "int n = 10; parallel_for (int i = n; i >= 0; i += 2) { }", toward },
				{ "int n = 10; parallel_for (int i = 0; i < n; i += -2) { }", toward },
				{ "int n = 10; parallel_for (int i = 0; i < n; i += 0) { }", "parallel_for: a counted loop's step must not be zero" },
				{ "int z = 0; parallel_for (int i = 0; i < 10; i += z) { }", "parallel_for: a counted loop's step of 0 never reaches its bound (the serial loop would not end)" },
				{ "int k = -2; parallel_for (int i = 0; i < 10; i += k) { }", "parallel_for: a counted loop's step of -2 never reaches its bound (the serial loop would not end)" },
				{ "parallel_for (int i = 9223372036854775800; i <= 9223372036854775807; i++) { }", "Integer overflow in '++'" },
				{ "parallel_for (int i = -9223372036854775807; i >= -9223372036854775807; i -= 5) { }", "Integer overflow in '-='" },
				{ "parallel_for (int i = 0; i < 4; i++) { int x = i; i += 1; x = i; }", "parallel_for: the body writes the counted loop's variable 'i' (the loop steps it; copy it into a local to change it) at 1:" },
				{ "float b = 3.5; parallel_for (int i = 0; i < b; i++) { }", "parallel_for: a counted loop's bound is float, not int" },
				{ "var b = 3.5; parallel_for (int i = 0; i < b; i++) { }", "parallel_for: a counted loop's bound is float, not int" },
			};
			for (const refusal& r : refusals) {
				const std::string got = error_of(e, r.source);
				check_true(got.find(r.text) != std::string::npos, std::string(r.source) + ": expected '" + r.text + "', got '" + got + "'");
			}
		});

		test("captured_value_objects_read_through_fields_and_read_only_methods", [this]() {
			// A captured instance of a flat value class (nested ones and strings included) reads through its fields and through
			// methods that write no part of it; each worker reads its own view, so results match the serial loop
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			const std::string src = R"(
				class Vec3 {
					float x = 0.0; float y = 0.0; float z = 0.0;
					Vec3() {}
					Vec3(float a, float b, float c) { x = a; y = b; z = c; }
					float dot(Vec3 o) { return x * o.x + y * o.y + z * o.z; }
					float len2() { return x * x + y * y + z * z; }
					float len2_twice() { return len2() * 2.0; }
				}
				class Ray { Vec3 origin; Vec3 dir; string tag = "ray"; array<int> weights = [1, 2, 3]; }
				Vec3 cam = Vec3(1.0, 2.0, 3.0);
				Ray ray; ray.origin = Vec3(0.5, 0.25, 0.125); ray.dir = Vec3(0.0, 1.0, 0.0);
				shared_ptr<Vec3> light = new Vec3(4.0, 5.0, 6.0);
				array<float> out = []; array<float> serial = [];
				for (int i = 0; i < 200; i++) { out.push(0.0); serial.push(0.0); }
				float shade(int i) {
					Vec3 p = Vec3(i * 0.5, i * 0.25, 1.0);
					return cam.dot(p) + cam.len2_twice() + ray.origin.y * i + ray.dir.len2() + ray.tag.size() + ray.weights[i % 3] + light.dot(p) + light.z;
				}
				parallel_for (int i = 0; i < 200; i++) { out[i] = shade(i); }
				for (int i = 0; i < 200; i++) { serial[i] = shade(i); }
				return [to_json(out), to_json(serial)];
			)";
			check_matches_serial_at(e, src, {1, 4, 8});
			check_eq((int)detail::parallel_capture_kind::object, (int)capture_kind(e, "cam"));
			check_eq((int)detail::parallel_capture_kind::object, (int)capture_kind(e, "ray"));
			check_eq((int)detail::parallel_capture_kind::object, (int)capture_kind(e, "light"));
			check_eq(std::string("1 2 3"), e->execute("return `${cam.x:.0f} ${cam.y:.0f} ${cam.z:.0f}`;").as<std::string>());   // never written
		});

		test("captured_objects_refuse_writes_and_aliases", [this]() {
			// A captured object is read-only: a field store, a method that writes its object and a container mutator on a field
			// are refused; a captured handle reads through members only; and no element may reach a captured object, since its
			// lane may write it while every worker reads its own view
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			e->parallel_thread_count(4);
			const std::string prelude = R"(
				class C { int n = 0; array<int> seen = []; int bump() { n++; return n; } int peek() { return n; } void grow() { seen.push(n); } }
				class Holder { C inner; int touch() { inner.bump(); return 0; } }
				class Bad { shared_ptr<C> link; }
				C c; Holder holder; Bad bad; bad.link = new C();
				shared_ptr<C> h = new C();
				array<shared_ptr<C>> hs = [new C(), h, new C()];
				array<int> idx = [0, 1, 2, 3]; array<int> out = [0, 0, 0, 0];
			)";
			struct refusal { const char* body; const char* text; };
			const refusal refusals[] = {
				{ "parallel_for (int i : idx) { out[i] = c.bump(); }", "parallel_for: method 'bump' of 'C' writes its object, and captured 'c' is read-only (write a local copy, or the element) at 9:" },
				{ "parallel_for (int i : idx) { c.grow(); }", "parallel_for: method 'grow' of 'C' writes its object, and captured 'c' is read-only" },
				{ "parallel_for (int i : idx) { out[i] = holder.touch(); }", "parallel_for: method 'touch' of 'Holder' writes its object, and captured 'holder' is read-only" },
				{ "parallel_for (int i : idx) { out[i] = holder.inner.bump(); }", "parallel_for: method 'bump' of 'C' writes its object, and captured 'holder' is read-only" },
				{ "parallel_for (int i : idx) { c.n = i; }", "parallel_for: member store reaches captured state 'c' (captured reads are read-only) at 9:" },
				{ "parallel_for (int i : idx) { c.seen.push(i); }", "parallel_for: method 'push' may mutate captured state 'c' at 9:" },
				{ "parallel_for (int i : idx) { for (auto& v : c.seen) { v += 1; } }", "parallel_for: cannot iterate captured state 'c' by reference in a parallel body (captured reads are read-only) at 9:" },
				{ "parallel_for (int i : idx) { auto& r = c.n; out[i] = r; }", "parallel_for: reference declaration would alias enclosing state 'c' (captured reads are read-only) at 9:" },
				{ "parallel_for (int i : idx) { out[i] = bad.link.n; }", "parallel_for: captured name 'bad' is not value-semantic (an object holding a handle, a function, a var field or a C++ object)" },
				{ "parallel_for (int i : idx) { shared_ptr<C> mine = h; out[i] = mine.n; }", "parallel_for: captured handle 'h' is read through its members and methods only (a worker reads its own view of the object) at 9:" },
				{ "parallel_for (auto& e : hs) { e.n = h.peek() + 1; }", "parallel_for: element 1 reaches the object captured as 'h' (a captured object is read-only, and an element's lane may write it)" },
			};
			for (const refusal& r : refusals) {
				const std::string got = error_of(e, prelude + r.body);
				check_true(got.rfind(r.text, 0) == 0, std::string(r.body) + ": expected '" + r.text + "', got '" + got + "'");
			}
			check_eq(std::string("[1,1,1,1]"), e->execute(prelude + "parallel_for (int i : idx) { out[i] = h.peek() + c.peek() + holder.inner.peek() + 1; } return to_json(out);").as<std::string>());
		});

		test("captured_cpp_objects_run_their_parallel_methods", [this]() {
			// A captured C++ object runs the methods its binding leaves parallel; a serial one raises the binder's text, and a
			// non-const one is refused on an object the body did not make
			struct gauge {
				double level = 0.0;
				void add(double amount) { level += amount; }
				double reading() const { return level; }
				void publish() const {}
			};
			const std::string src = "Gauge g; g.add(7.0);\n"
			                        "array<float> xs = [1.0, 2.0, 3.0, 4.0];\n"
			                        "parallel_for (auto& x : xs) { x = x * 10.0 + g.reading(); }\n"
			                        "print(to_json(xs));\n"
			                        "try { parallel_for (auto& x : xs) { g.publish(); } } catch (e) { print(e); }\n"
			                        "try { parallel_for (auto& x : xs) { g.add(x); } } catch (e) { print(e); }\n"
			                        "print(g.reading());\n";
			const std::string expected =
				"[17.0,27.0,37.0,47.0]\n"
				"C++ method 'publish' of 'Gauge' is marked jai::serial and cannot be called in a parallel body\n"
				"C++ method 'add' of 'Gauge' changes an object this parallel body did not make (only its const methods may run on it)\n"
				"7.0\n";
			for (size_t workers : {size_t(1), size_t(4), size_t(8)}) {
				auto e = make_engine();
				jai::stdlib::register_all(*e);
				auto out = std::make_shared<std::ostringstream>();
				e->set_output_stream(out);
				jai::dynamic_binder<gauge>(*e, "Gauge").constructor<>().method("add", &gauge::add).method("reading", &gauge::reading)
					.method("publish", &gauge::publish, jai::serial).build();
				e->parallel_thread_count(workers);
				try { e->execute(src); } catch (const std::exception& ex) { *out << "threw: " << ex.what(); }
				check_eq(expected, out->str(), std::to_string(workers));
				check_eq((int)detail::parallel_capture_kind::object, (int)capture_kind(e, "g"));
			}
		});

		test("captured_containers_answer_their_count_without_a_copy", [this]() {
			// size(), length() and empty() read a borrowed container's count, so a body (or a by-value lambda capture) bounding
			// its reads by the size keeps the borrow tier: no worker copies the container
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			const std::string src = R"(
				array<int> grid = []; array<int> xs = [];
				for (int i = 0; i < 500; i++) { grid.push(i * 7 % 101); }
				for (int i = 0; i < 64; i++) { xs.push(i * 9); }
				array<int> none = [];
				int f(int x) { int s = 0; for (int k = x % 7; k < grid.size(); k += 7) { s += grid[k]; } return s + (none.empty() ? 1 : 0) + grid.length(); }
				auto lam = [=](int x) { return x < grid.size() ? grid[x] : -1; };
				var serial_f = []; var serial_lam = [];
				for (int x : xs) { serial_f.push(f(x)); serial_lam.push(lam(x * 2)); }
				var doubled = parallel_transform(xs, [](int x) { return x * 2; });
				return [[parallel_transform(xs, f), parallel_transform(doubled, lam)], [serial_f, serial_lam]];
			)";
			check_matches_serial_at(e, src, {1, 4, 8});
			check_eq((int)detail::parallel_capture_kind::borrow, (int)capture_kind(e, "grid"));
			auto direct = e->execute(R"(
				var out = [0, 0];
				parallel_for (auto& o, int i : out, [0, 1]) { o = grid.size() + none.size() + i; }
				return out[1];
			)");
			check_eq((int64_t)501, direct.as_int());
			check_eq((int)detail::parallel_capture_kind::borrow, (int)capture_kind(e, "grid"));
		});

		// in-place element writes (rows, typed objects and their nested fields, subclasses in a base array, references to members
		// and map pairs) with captured reads, helper calls and grafted leaves land what the serial loop lands at every worker count
		test("parallel_for_lands_what_the_serial_loop_lands", [this]() {
			struct serial_case { const char* code; std::vector<size_t> workers; bool stdlib = false; };
			const serial_case cases[] = {
				// A leaf grafted into a parallel_for body runs in the workers too (the worker's copy of the callee closes over the
				// worker's root, which the guard accepts); the values match the serial loop at every worker count
				{R"(
				class P { int kind = 1; float x = 0.0; float vx = 0.1; int life = 10; }
				int CAP = 4;
				void step(var& p) { int kind = p.kind; if (kind == 0) { return; } float x = p.x; float vx = p.vx; int life = p.life; life = life - 1;
				  if (life <= 0) { p.kind = 0; p.x = 0.0; p.life = 0; return; } if (kind == 1) { vx = vx * 0.9; } else { vx = vx * 0.5; } x = x + vx * 0.5;
				  if (x > 2.0) { x = 2.0; if (kind == 1 || kind == 2) { life = life < CAP ? life : CAP; } } p.x = x; p.vx = vx; p.life = life; }
				array<P> a = []; array<P> b = [];
				for (int i = 0; i < 64; i++) { P p = P(); p.kind = i % 3; p.x = i * 0.3; a.push(p); P q = P(); q.kind = i % 3; q.x = i * 0.3; b.push(q); }
				for (int t = 0; t < 12; t++) { parallel_for (auto& p : a) { step(p); } }
				for (int t = 0; t < 12; t++) { for (auto& p : b) { step(p); } }
				var ra = []; var rb = [];
				for (auto& p : a) { ra.push(p.x); ra.push(p.life); ra.push(p.kind); }
				for (auto& p : b) { rb.push(p.x); rb.push(p.life); rb.push(p.kind); }
				return [ra, rb];
				)", {1, 2, 8}},
				{R"(
				var a = [];
				var b = [];
				for (var i = 0; i < 200; i++) { a.push([i, 0]); b.push([i, 0]); }
				parallel_for (auto& p : a) { p[1] = p[0] * 3 - 1; }
				for (auto& p : b) { p[1] = p[0] * 3 - 1; }
				return [a, b];
				)", {1, 2, 8}},
				// a reference bound to a member of the element or of a body local, a structured binding over the element's map and a
				// map pair's .second write through as the serial loop's do
				{R"(
				class P { float x = 0.0; }
				class B { P pos; array<int> hist; map<string, int> m = {"a": 1, "b": 2}; }
				array<B> par = []; array<B> ser = [];
				for (int k = 0; k < 40; k++) { par.push(B()); ser.push(B()); }
				parallel_for (auto& b : par) {
					auto& p = b.pos; p.x += 1.5;
					auto& h = b.hist; h.push(cast<int>(p.x));
					B local; auto& q = local.pos; q.x = 3.0; b.pos.x += q.x;
					for (auto& [k, v] : b.m) { v += 10; }
					for (auto& pr : b.m) { pr.second += 1; }
					map<string, int> mine = {"z": 1}; for (auto& pr : mine) { pr.second += 5; } b.hist.push(mine["z"]);
				}
				for (auto& b : ser) {
					auto& p = b.pos; p.x += 1.5;
					auto& h = b.hist; h.push(cast<int>(p.x));
					B local; auto& q = local.pos; q.x = 3.0; b.pos.x += q.x;
					for (auto& [k, v] : b.m) { v += 10; }
					for (auto& pr : b.m) { pr.second += 1; }
					map<string, int> mine = {"z": 1}; for (auto& pr : mine) { pr.second += 5; } b.hist.push(mine["z"]);
				}
				return [to_json(par), to_json(ser)];
				)", {1, 4, 16}, true},
				{R"(
				var scale = 7;
				var lut = [];
				for (var i = 0; i < 256; i++) { lut.push(i * 2); }
				var a = [];
				var b = [];
				for (var i = 0; i < 128; i++) { a.push([i, 0]); b.push([i, 0]); }
				parallel_for (auto& p : a) { p[1] = lut[p[0]] * scale; }
				for (auto& p : b) { p[1] = lut[p[0]] * scale; }
				return [a, b];
				)", {1, 2, 8}},
				{R"(
				int shape(int v) { return v * v - 3; }
				var a = [];
				var b = [];
				for (var i = 0; i < 96; i++) { a.push([i, 0]); b.push([i, 0]); }
				parallel_for (auto& p : a) { p[1] = shape(p[0]) + math::itrunc(math::sqrt(1.0 * p[0])); }
				for (auto& p : b) { p[1] = shape(p[0]) + math::itrunc(math::sqrt(1.0 * p[0])); }
				return [a, b];
				)", {1, 2, 8}},
				{R"(
				class K { int a = 1; int b = 0; K() {} K(int v) { a = v; } }
				array<K> ks = [];
				array<K> serial = [];
				for (int i = 0; i < 300; i++) { ks.push(K(i)); serial.push(K(i)); }
				parallel_for (auto k : ks) { k.a = 999; k.b = 999; }
				parallel_for (auto& k : ks) { k.b = k.a * 2; k.a = k.a + 1; }
				for (auto& k : serial) { k.b = k.a * 2; k.a = k.a + 1; }
				var p = []; var q = [];
				for (auto k : ks) { p.push(k.a); p.push(k.b); }
				for (auto k : serial) { q.push(k.a); q.push(k.b); }
				return [p, q];
				)", {1, 2, 8}},
				{R"(
				class Leaf { int v = 0; string tag = "leaf"; Leaf() {} Leaf(int n) { v = n; } }
				class Inner { Leaf a = Leaf(0); Leaf b = Leaf(0); array<int> xs = []; string name = "inner"; Inner() {} Inner(int n) { a = Leaf(n); b = Leaf(n + 1); xs = [n, n + 1, n + 2]; } }
				class Node { Inner first = Inner(0); array<Leaf> kids = []; map<string, int> counts = {}; Node() {} Node(int n) { first = Inner(n); kids = [Leaf(n), Leaf(n * 2)]; counts["a"] = n; } }
				array<Node> nodes = [];
				array<Node> serial = [];
				for (int i = 0; i < 100; i++) { nodes.push(Node(i)); serial.push(Node(i)); }
				parallel_for (auto& n : nodes) {
					n.first.a.v = n.first.a.v + 1;
					n.first.xs[0] = n.first.xs[0] * 3;
					n.kids[1].v = n.kids[1].v + 7;
					n.counts["a"] = n.counts["a"] + 100;
					n.first.b.v = n.first.a.v * 2;
				}
				for (auto& n : serial) {
					n.first.a.v = n.first.a.v + 1;
					n.first.xs[0] = n.first.xs[0] * 3;
					n.kids[1].v = n.kids[1].v + 7;
					n.counts["a"] = n.counts["a"] + 100;
					n.first.b.v = n.first.a.v * 2;
				}
				var p = []; var q = [];
				for (auto n : nodes) { p.push([n.first.a.v, n.first.b.v, n.first.xs[0], n.kids[1].v, n.counts["a"], n.first.b.tag]); }
				for (auto n : serial) { q.push([n.first.a.v, n.first.b.v, n.first.xs[0], n.kids[1].v, n.counts["a"], n.first.b.tag]); }
				return [p, q];
				)", {1, 2, 8}},
				{R"(
				class B { int a = 1; B() {} B(int v) { a = v; } }
				class D : B { int b = 2; string tag = "d"; D() {} D(int v) : super(v) { b = v * 2; } }
				array<B> bs = [];
				array<B> serial = [];
				for (int i = 0; i < 90; i++) {
					if (i % 3 == 0) { bs.push(D(i)); serial.push(D(i)); } else { bs.push(B(i)); serial.push(B(i)); }
				}
				parallel_for (auto& x : bs) { x.a = x.a + 10; }
				for (auto& x : serial) { x.a = x.a + 10; }
				var p = []; var q = [];
				for (auto x : bs) { p.push(x.a); p.push(type_of(x)); }
				for (auto x : serial) { q.push(x.a); q.push(type_of(x)); }
				p.push(cast<D>(bs[3]).b); p.push(cast<D>(bs[3]).tag);
				q.push(cast<D>(serial[3]).b); q.push(cast<D>(serial[3]).tag);
				return [p, q];
				)", {1, 2, 8}, true},
			};
			check_each(cases, [this](const serial_case& c) {
				auto e = make_engine();
				if (c.stdlib) { jai::stdlib::register_all(*e); }
				for (size_t workers : c.workers) { check_matches_serial_at(e, c.code, {workers}); }
			});
		});

		test("primitive_elements_mutate_in_place", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				var a = [];
				for (var i = 0; i < 100; i++) { a.push(i); }
				parallel_for (auto& x : a) { x = x * x; }
				return [a[0], a[7], a[99]];
			)");
			const auto& vals = r.as_array();
			check_eq((int64_t)0, vals[0].as_int());
			check_eq((int64_t)49, vals[1].as_int());
			check_eq((int64_t)9801, vals[2].as_int());
		});

		test("flat_object_elements_mutate_in_place", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				class Particle { float x = 0.0; float vx = 0.0; int life = 0; }
				var pool = [];
				for (var i = 0; i < 64; i++) {
					auto p = Particle();
					p.x = 1.0 * i;
					p.vx = 0.5;
					p.life = i;
					pool.push(p);
				}
				parallel_for (auto& p : pool) {
					p.x = p.x + p.vx;
					p.life = p.life - 1;
				}
				return [pool[0].x, pool[10].x, pool[63].life];
			)");
			const auto& vals = r.as_array();
			check_near(0.5, vals[0].as_float(), 0.0001);
			check_near(10.5, vals[1].as_float(), 0.0001);
			check_eq((int64_t)62, vals[2].as_int());
		});

		// Strings are value-closed by ruling: fields AND elements. The shared-node case
		// (two elements assigned from one variable) must normalize at the barrier, not
		// error - and the mutation must not corrupt the sibling.
		test("string_fields_and_shared_nodes_normalize", [this]() {
			auto e = make_engine();
			jai::stdlib::register_all(e);
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				class Row { int idx = 0; string out = ""; }
				var rows = [];
				string shared_seed = "seed";
				for (var i = 0; i < 32; i++) {
					auto rec = Row();
					rec.idx = i;
					rec.out = shared_seed;
					rows.push(rec);
				}
				parallel_for (auto& rec : rows) {
					rec.out = "row_" + to_string(rec.idx);
				}
				return [rows[0].out, rows[31].out, shared_seed];
			)");
			const auto& vals = r.as_array();
			check_eq(std::string("row_0"), vals[0].as<std::string>());
			check_eq(std::string("row_31"), vals[1].as<std::string>());
			check_eq(std::string("seed"), vals[2].as<std::string>());
		});

		test("by_value_element_is_side_effect_free", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				var a = [];
				for (var i = 0; i < 64; i++) { a.push([i, 0]); }
				parallel_for (auto p : a) { p[1] = 999; }
				return [a[0][1], a[63][1]];
			)");
			const auto& vals = r.as_array();
			check_eq((int64_t)0, vals[0].as_int());
			check_eq((int64_t)0, vals[1].as_int());
		});

		// The GLOOM shape: an in-place helper taking the OWNED ELEMENT by reference
		// (gloom_particle_step). Caught red by GLOOM tick 8 when worker invokers became
		// thunks: the in-loop call path must bind the by-ref param to the element
		// reference exactly like the opaque path did.
		test("by_ref_helper_mutates_element", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				void bump(var& q) { q[1] = q[0] * 5; }
				var a = [];
				for (var i = 0; i < 64; i++) { a.push([i, 0]); }
				parallel_for (auto& p : a) { bump(p); }
				return [a[0][1], a[63][1]];
			)");
			const auto& vals = r.as_array();
			check_eq((int64_t)0, vals[0].as_int());
			check_eq((int64_t)315, vals[1].as_int());
		});

		// === Contract A: violations ERROR ===
		// a body that writes enclosing or captured state, a container it mutates, an element it cannot prove exclusive, a shared
		// write inside a method, an overloaded method or a class-changing assignment is refused with its verdict
		test("parallel_for_refuses_what_it_cannot_admit_with_its_verdict", [this]() {
			struct verdict_case { const char* code; std::vector<const char*> verdict; size_t workers = 0; };
			const verdict_case cases[] = {
				{R"(
				var g = 0;
				var a = [];
				for (var i = 0; i < 32; i++) { a.push(i); }
				try { parallel_for (auto& x : a) { g = x; } } catch (err) { return err; }
				return "no-error";
				)", {"parallel_for: body writes enclosing state 'g'"}},
				{R"(
				class World { int hits = 0; }
				var w = World();
				var a = [];
				for (var i = 0; i < 32; i++) { a.push(i); }
				try { parallel_for (auto& x : a) { w.hits = x; } } catch (err) { return err; }
				return "no-error";
				)", {"captured state 'w'"}},
				{R"(
				var a = [];
				for (var i = 0; i < 64; i++) { a.push(i); }
				try { parallel_for (auto& x : a) { x = a[0] + x; } } catch (err) { return err; }
				return "no-error";
				)", {"container being mutated"}},
				{R"(
				class Node { var payload = 0; }
				var a = [];
				for (var i = 0; i < 32; i++) { a.push(Node()); }
				try { parallel_for (auto& x : a) { x.payload = 1; } } catch (err) { return err; }
				return "no-error";
				)", {"cannot be proven exclusive"}},
				{R"(
				var total = 0;
				class Cell { int v = 0; void sink() { total = total + v; } }
				var a = [];
				for (var i = 0; i < 32; i++) { a.push(Cell()); }
				try { parallel_for (auto& c : a) { c.sink(); } } catch (err) { return err; }
				return "no-error";
				)", {"body writes enclosing state 'total'"}},
				{R"(
				class Cell {
					int v = 0;
					void set(int x) { v = x; }
					void set(int x, int y) { v = x + y; }
				}
				var a = [];
				for (var i = 0; i < 32; i++) { a.push(Cell()); }
				try { parallel_for (auto& c : a) { c.set(1); } } catch (err) { return err; }
				return "no-error";
				)", {"only single-overload methods of the element's class or its bases"}},
				// An assignment of another class into a by-value slot re-classes the slot's object (no
				// slicing): its dropped levels' destructors and the class registry are not worker territory
				{R"(
				class Leaf { int v = 0; Leaf() {} }
				class Leaf2 : Leaf { int w = 1; Leaf2() {} }
				class Node { Leaf a = Leaf(); Leaf b = Leaf2(); Node() {} }
				array<Node> nodes = [];
				for (int i = 0; i < 64; i++) { nodes.push(Node()); }
				try { parallel_for (auto& n : nodes) { n.a = n.b; } } catch (err) { return err; }
				return "no-error";
				)", {"an assignment that changes an object's class is not admitted in a parallel body"}, 4},
				{R"(
				class Node { var payload = 0; }
				array<Node> a = [];
				for (int i = 0; i < 32; i++) { a.push(Node()); }
				try { parallel_for (auto& x : a) { x.payload = 1; } } catch (err) { return err; }
				return "no-error";
				)", {"cannot be proven exclusive"}},
			};
			check_each(cases, [](const verdict_case& c) {
				auto e = make_engine();
				if (c.workers) { e->parallel_thread_count(c.workers); }
				const auto msg = e->execute(c.code).as<std::string>();
				for (const char* part : c.verdict) { check_true(msg.find(part) != std::string::npos, msg); }
			});
		});

		// shared_ptr-tagged elements alias by script-visible design: one object at two elements could be touched by two workers,
		// and the barrier may not clone it apart; distinct objects are exactly the serial loop's in-place mutation
		test("aliased_shared_ptr_elements_rejected_unaliased_mutate_in_place", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				class Q { int x = 0; }
				var q = new Q();
				var a = [];
				for (var i = 0; i < 32; i++) { a.push(q); }
				try { parallel_for (auto& el : a) { el.x = el.x + 1; } } catch (err) { return err; }
				return "no-error";
			)");
			auto msg = r.as<std::string>();
			check_true(msg.find("parallel_for: elements 0 and 1 hold the same object (an object a body reaches through a handle element belongs to one element)") != std::string::npos, msg);

			auto r2 = e->execute(R"(
				class Q2 { int x = 0; }
				array<Q2> b = [];
				for (var i = 0; i < 32; i++) { b.push(new Q2()); }
				parallel_for (auto& el : b) { el.x = el.x + 5; }
				var total = 0;
				for (auto el : b) { total += el.x; }
				return total;
			)");
			check_eq((int64_t)160, r2.as_int());
		});

		// The distinctness check: handle elements run when no object appears at two elements (range and zipped arrays alike), whatever
		// else holds the objects; the same object at one element of several arrays is that element's worker alone. A handle nested
		// inside an element must still be its object's only holder
		test("handle_elements_run_when_their_objects_are_distinct", [this]() {
			const std::string prelude = R"(
				class C { int x = 0; }
				array<shared_ptr<C>> registry = [];
				array<shared_ptr<C>> hs = [];
				array<weak_ptr<C>> ws = [];
				for (int i = 0; i < 64; i++) { shared_ptr<C> h = new C(); registry.push(h); hs.push(h); ws.push(h); }
				int total() { int t = 0; for (auto h : registry) { t += h.x; } return t; }
			)";
			const std::vector<std::pair<std::string, std::string>> cases {
				{"parallel_for (auto& h : hs) { h.x = h.x + 1; } return total();", "64"},
				{"parallel_for (var w : ws) { var l = w.lock(); l.x = l.x + 2; } return total();", "128"},
				{"parallel_for (auto& h, auto& w : hs, ws) { h.x = h.x + 1; } return total();", "64"},
				{"array<shared_ptr<C>> other = hs; other[3] = hs[1]; try { parallel_for (auto& h, auto& o : hs, other) { h.x = 1; } } catch (err) { return err; } return \"ran\";",
				 "parallel_for: elements 1 and 3 hold the same object (an object a body reaches through a handle element belongs to one element)"},
				{"var rows = []; for (int i = 0; i < 64; i++) { rows.push([registry[i]]); } try { parallel_for (auto& r : rows) { r[0].x = 1; } } catch (err) { return err; } return \"ran\";",
				 "parallel_for: element 0 cannot be proven exclusive (only value-semantic shapes are: primitives, strings, math vectors, value containers, flat value-class instances and distinct handles to them)"},
				{"int bump(shared_ptr<C> h) { return h.x + 5; } var out = parallel_transform(hs, bump); return out[63];", "5"},
			};
			for (const auto& [body, expected] : cases) {
				for (bool vm : {false, true}) {
					auto e = make_engine();
					e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					e->parallel_thread_count(4);
					check_eq(expected, e->execute(prelude + body).to_string(), std::string(vm ? "vm: " : "interp: ") + body);
				}
			}
		});

		// out[i] with i a loop variable writes disjoint slots: each worker writes a private copy and the join copies every element's
		// slot back in element order, up to the first failing element (the serial loop's stores); anything else writing or reading
		// the written array elsewhere raises, and so do two elements carrying one index
		test("index_writes_land_on_disjoint_slots", [this]() {
			const std::string prelude = R"(
				array<int> order = [];
				for (int i = 0; i < 256; i++) { order.push((i * 37) % 256); }
				array<int> out = [];
				for (int i = 0; i < 256; i++) { out.push(0); }
				int sum() { int s = 0; for (int i = 0; i < 256; i++) { s += out[i] * (i + 1); } return s; }
			)";
			const std::vector<std::pair<std::string, std::string>> cases {
				{"parallel_for (int k : order) { out[k] = k * 2; } return sum();", "11184640"},
				{"parallel_for (int k : order) { out[k] += k; out[k]++; } return sum();", "5625216"},
				{"array<int> ks = order; parallel_for (auto& o, int k : order, ks) { out[k] = o + 1; } return sum();", "5625216"},
				{"int run() { array<int> local = [0, 0, 0, 0]; parallel_for (int k : [3, 1, 0, 2]) { local[k] = k + 10; } return local[0] * 1000 + local[3]; } return run();", "10013"},
				{"try { parallel_for (int k : order) { if (k == 100) { throw \"stop\"; } out[k] = 1; } } catch (err) { } int before = 0; for (int p = 0; p < 256; p++) { if (order[p] == 100) { break; } before += out[order[p]]; } int all = 0; for (int i = 0; i < 256; i++) { all += out[i]; } return before == all ? \"serial\" : `diverged ${before} ${all}`;", "serial"},
				{"array<int> dup = [0, 1, 1, 2]; try { parallel_for (int k : dup) { out[k] = 1; } } catch (err) { return err; } return \"ran\";",
				 "parallel_for: elements 1 and 2 write 'out' at the same index 1"},
				{"try { parallel_for (int k : order) { out[k] = out[0] + 1; } } catch (err) { return err; } return \"ran\";",
				 "parallel_for: body touches 'out' beyond the slot it writes, out[k]"},
				{"try { parallel_for (int k : order) { k = k + 1; out[k] = 1; } } catch (err) { return err; } return \"ran\";",
				 "parallel_for: body writes enclosing state 'out' at an index the body changes"},
				{"map<int, int> m = {0: 0}; try { parallel_for (int k : order) { m[k] = 1; } } catch (err) { return err; } return \"ran\";",
				 "parallel_for: captured name 'm' is written at an index but is not an array"},
				{"try { parallel_for (int k : order) { order[k] = 1; } } catch (err) { return err; } return \"ran\";",
				 "parallel_for: captured name 'order' is the container being mutated"},
				{"try { parallel_for (int k : order) { int j = k; out[j] = 1; } } catch (err) { return err; } return \"ran\";",
				 "parallel_for: body writes enclosing state 'out'"},
				// an index past the array raises the serial loop's text, and the slots before the failing element land
				{"array<int> past = [0, 1, 300, 2]; array<int> small = [0, 0, 0]; try { parallel_for (int k : past) { small[k] = 7; } } catch (err) { return `${err}|${(small[0] * 100 + small[1] * 10 + small[2])}`; } return \"ran\";",
				 "Array index 300 out of bounds for array of size 3|770"},
				{"array<int> small = [0, 0, 0]; try { parallel_for (int k = 0; k < 4; k++) { small[k] = 7; } } catch (err) { return `${err}|${(small[0] * 100 + small[1] * 10 + small[2])}`; } return \"ran\";",
				 "Array index 3 out of bounds for array of size 3|777"},
			};
			for (const auto& [body, expected] : cases) {
				for (bool vm : {false, true}) {
					for (size_t workers : {size_t(1), size_t(4)}) {
						auto e = make_engine();
						e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
						e->parallel_thread_count(workers);
						const std::string got = e->execute(prelude + body).to_string();
						check_true(got.rfind(expected, 0) == 0, std::string(vm ? "vm " : "interp ") + std::to_string(workers) + ": " + body + " -> " + got);
					}
				}
			}
		});

		test("admitted_self_method_call", [this]() {
			// parallel-method-admission ruling: a single-overload public method touching
			// only element state is admitted in SAFE mode and dispatches via pinned
			// worker-private state (no wall error)
			auto e = make_engine();
			auto r = e->execute(R"(
				class Cell { int v = 0; int doubled() { return v * 2; } }
				var a = [];
				for (var i = 0; i < 32; i++) { auto c = Cell(); c.v = i; a.push(c); }
				parallel_for (auto& c : a) { c.v = c.doubled(); }
				return a[3].v * 1000 + a[31].v;
			)");
			check_eq((int64_t)6062, r.as_int());
		});

		test("admitted_method_deterministic_across_worker_counts", [this]() {
			int64_t first = 0;
			bool have_first = false;
			for (size_t workers : {1, 2, 4, 8}) {
				auto e = make_engine();
				e->parallel_thread_count(workers);
				auto r = e->execute(R"(
					class Acc {
						int v = 0; int extra = 0;
						void bump(int by) { v = v + by; extra = this.helper(); }
						int helper() { return v * 3; }
					}
					var a = [];
					for (var i = 0; i < 100; i++) { auto c = Acc(); c.v = i; a.push(c); }
					parallel_for (auto& c : a) { c.bump(7); c.bump(1); }
					var t = 0;
					for (auto c : a) { t = t + c.v * 31 + c.extra; }
					return t;
				)");
				const int64_t s = r.as_int();
				if (!have_first) { first = s; have_first = true; }
				check_eq(first, s);
			}
		});

		test("custom_operator_refused_with_its_violation", [this]() {
			// an operator whose body the region cannot admit (here it writes enclosing state) stays unpinned: dispatching it raises
			// what its walk refused, and a custom op== never silently falls back to structural equality
			auto e = make_engine();
			auto r = e->execute(R"(
				int calls = 0;
				class Vec { int x = 0; function operator==(Vec other) { calls = calls + 1; return x == other.x; } }
				var a = [];
				for (var i = 0; i < 32; i++) { auto v = Vec(); v.x = i % 2; a.push(v); }
				auto probe = Vec();
				try { parallel_for (auto& v : a) { if (v == v) { v.x = 1; } } } catch (err) { return err; }
				return "no-error";
			)");
			auto msg = r.as<std::string>();
			check_true(msg.rfind("body writes enclosing state 'calls' in operator '==' of 'Vec' at 3:", 0) == 0, msg);
			// an implicit conversion into a typed slot is the class's to_int(), refused alike
			auto conversion = make_engine();
			auto c = conversion->execute(R"(
				int calls = 0;
				class Num { int v = 0; int to_int() { calls = calls + 1; return v; } }
				var a = [];
				for (var i = 0; i < 32; i++) { a.push(Num()); }
				try { parallel_for (auto& n : a) { int k = n; n.v = k + 1; } } catch (err) { return err; }
				return "no-error";
			)");
			auto conversion_msg = c.as<std::string>();
			check_true(conversion_msg.rfind("body writes enclosing state 'calls' in operator 'to_int' of 'Num' at 3:", 0) == 0, conversion_msg);
		});

		test("custom_arithmetic_comparison_and_unary_operators_run_through_their_pins", [this]() {
			// the region pins each operator of the element's class (its body walked like a method's, the objects it constructs
			// admitted with it): a worker dispatches it as the serial loop does
			auto e = make_engine();
			auto r = e->execute(R"(
				class Vo {
					int v = 1;
					Vo operator+(Vo o) { Vo r; r.v = v + o.v; return r; }
					bool operator<(Vo o) { return v < o.v; }
					Vo operator-() { Vo r; r.v = -v; return r; }
				}
				class Plain { int v = 1; }
				array<Vo> vs = [Vo(), Vo()];
				function t(function f) -> string { string r = "none"; try { f(); } catch (err) { r = err; } return r; }
				string a = t([&]() { parallel_for (auto& v : vs) { v = v + v; } });
				string c = t([&]() { parallel_for (auto& v : vs) { if (v < v) { v.v = 3; } } });
				string d = t([&]() { parallel_for (auto& v : vs) { v = -v; } });
				array<Plain> ps = [Plain(), Plain()];
				string p = t([&]() { parallel_for (auto& q : ps) { q.v = q.v + 1; } });
				`${a}|${c}|${d}|${p}|${ps[0].v}|${vs[0].v}|${vs[1].v}`;
			)");
			check_eq(std::string("none|none|none|none|2|-2|-2"), r.as<std::string>());
		});

		test("method_calls_fan_out_on_warm_names", [this]() {
			// A region of eight elements fans out to eight workers since the cost model took over the small-region cutoff; the
			// method call's user-accessor probe once interned _get_<name> from every worker at once and tore the interner
			// (0xC0000005 on both backends). The names are warm at admission and the interner is frozen, so Debug's frozen-miss
			// assert is the detector; the results are the serial loop's
			for (int round = 0; round < 8; ++round) {
				auto e = make_engine();
				e->parallel_thread_count(8);
				auto r = e->execute(R"(
					class Cell3 { int v = 0; int doubled() { return v * 2; } int plus(int k) { return v + k; } }
					var a3 = [];
					for (var i = 0; i < 8; i++) { auto c = Cell3(); c.v = i; a3.push(c); }
					parallel_for (auto& c : a3) { c.v = c.doubled() + c.plus(1); }
					var out = [];
					for (auto& c : a3) { out.push(c.v); }
					return out;
				)");
				const auto& out = r.as_array();
				check_eq((size_t)8, out.size());
				for (int64_t i = 0; i < 8; ++i) { check_eq(i * 2 + i + 1, out[static_cast<size_t>(i)].as_int(), "element " + std::to_string(i)); }
			}
		});

		test("parallel_bodies_read_the_enclosing_functions_locals", [this]() {
			// A body names locals of the function its statement runs in (a table, a scalar, a
			// string): the parser records their frame slots on the statement and the barrier
			// provisions from that frame, since no environment chain can see frame slots. No
			// global shares these names. A local that is the mutated range itself keeps the
			// aliasing rejection. Both backends.
			const char* src = R"(
				int run() {
					array<int> table = [10, 20, 30];
					int bias = 5;
					string tag = "ab";
					array<int> out = [0, 0, 0, 0];
					parallel_for (auto& o : out) { o = table[1] + bias + tag.size(); }
					return out[0] + out[3];
				}
				run();
			)";
			const char* aliased = R"(
				int run() {
					array<int> xs = [1, 2, 3, 4];
					parallel_for (auto& x : xs) { x = xs[0]; }
					return xs[3];
				}
				run();
			)";
			for (bool vm : {false, true}) {
				auto e = make_engine();
				e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->parallel_thread_count(4);
				check_eq((int64_t)54, e->execute(src).as_int(), vm ? "vm" : "interp");
				std::string text;
				try { e->execute(aliased); } catch (const std::exception& ex) { text = ex.what(); }
				check_true(text.find("captured name 'xs' is the container being mutated") != std::string::npos,
				           std::string(vm ? "vm" : "interp") + ": aliasing the range through a local is rejected, got '" + text + "'");
			}
		});

		test("captured_names_keep_their_scope_in_method_pins", [this]() {
			// Binding control: a global palette read inside Cell.calc() and a
			// caller-local palette of the same name read in the parallel body. A pinned method
			// resolves its captures in the worker's method environment, the body in its own, so
			// both bindings survive: v gains calc()'s 1 + 1 and the body's 3 per round, 63 -> 75.
			// Both modes, both backends; the serial loop is the oracle.
			const char* src = R"(
				array<string> palette = ["x"];
				class Cell { int v = 0; Cell(int n) { v = n; } int calc() { return v + palette[0].size(); } }
				int run() {
					array<string> palette = ["yyy"];
					array<Cell> cells = [];
					for (int i = 0; i < 64; ++i) { cells.push(Cell(i)); }
					for (int round = 0; round < 3; ++round) {
						parallel_for (auto& cell : cells) { cell.v = cell.calc() + palette[0].size(); }
					}
					return cells[63].v;
				}
				run();
			)";
			for (bool vm : {false, true}) {
				auto e = make_engine();
				e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->parallel_thread_count(8);
				e->execution_budget(0);
				check_eq((int64_t)75, e->execute(src).as_int(), vm ? "vm" : "interp");
			}
		});

		test("pinned_methods_read_provisioned_captures_in_safe_mode", [this]() {
			// The safe-mode half of the string-table finding: calc() reads a captured string table
			// through the pin's environment, which must be the worker's provisioned borrow (the
			// live global's handle copied across threads tore the literal's count). The XTHREAD
			// canary build turns that copy into a deterministic abort, so this shape rides the
			// parallel suites there too.
			const char* src = R"(
				array<string> palette = ["x"];
				class Cell { int v = 0; Cell(int n) { v = n; } int calc() { return v + palette[0].size(); } }
				array<Cell> cells = [];
				for (int i = 0; i < 64; ++i) { cells.push(Cell(i)); }
				for (int round = 0; round < 3; ++round) { parallel_for (auto& cell : cells) { cell.v = cell.calc(); } }
				int total = 0; for (auto& cell : cells) { total += cell.v; }
				return total;
			)";
			for (bool vm : {false, true}) {
				for (int round = 0; round < 4; ++round) {
					auto e = make_engine();
					e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					e->parallel_thread_count(8);
					e->execution_budget(0);
					check_eq((int64_t)2208, e->execute(src).as_int(), vm ? "vm" : "interp");
				}
			}
		});

		test("method_bodies_keep_the_runtime_walls", [this]() {
			// At 8 workers a sibling call spelled this.inner(), a field's method leaf.read() and an accessor this.x once asserted a
			// getter-id miss on the frozen interner, and a bare sibling call died on a dead strong_ptr block. The walk warms and pins,
			// the element proof detaches and the worker walls apply; each shape lands the same on both backends
			struct shape { const char* name; const char* definition; };
			const shape shapes[] = {
				{ "own_method", "class Cell { int v = 0; Cell(int n) { v = n; } int calc() { return v + 1; } }" },
				{ "nested_own_method", "class Cell { int v = 0; Cell(int n) { v = n; } int inner() { return v + 1; } int calc() { return inner(); } }" },
				{ "explicit_sibling_method", "class Cell { int v = 0; Cell(int n) { v = n; } int inner() { return v + 1; } int calc() { return this.inner(); } }" },
				{ "nested_field_method", "class Leaf { int n = 1; int read() { return n; } } class Cell { int v = 0; Leaf leaf = Leaf(); Cell(int n) { v = n; } int calc() { return v + leaf.read(); } }" },
				{ "chained_field_method", "class Leaf { int n = 1; int read() { return n; } } class Cell { int v = 0; Leaf leaf = Leaf(); Cell(int n) { v = n; } int calc() { return v + this.leaf.read(); } }" },
				{ "getter", "class Cell { int v = 0; Cell(int n) { v = n; } int _get_x() { return v; } int calc() { return this.x + 1; } }" },
				{ "literal_string", "class Cell { int v = 0; Cell(int n) { v = n; } int calc() { string text = \"x\"; return v + text.size(); } }" },
				{ "captured_string_table", "array<string> palette = [\"x\"]; class Cell { int v = 0; Cell(int n) { v = n; } int calc() { return v + palette[0].size(); } }" },
				{ "skipped_subtree", "array<int> steps = [1, 2]; class Cell { int v = 0; Cell(int n) { v = n; } int twice() { return v * 2; } int calc() { int s = v; for (auto p : steps) { s = s + this.twice(); } return s; } }" },
				{ "new_argument", "class Box { int n = 0; Box(int k) { n = k; } } class Cell { int v = 0; Cell(int n) { v = n; } int twice() { return v * 2; } int calc() { Box b = new Box(this.twice()); return b.n; } }" },
			};
			const std::map<std::string, std::string> expected = {
				{ "own_method", "value 2208" },
				{ "nested_own_method", "value 2208" },
				{ "explicit_sibling_method", "value 2208" },
				{ "nested_field_method", "error method 'read' was not admitted in this parallel body (admitted: single-overload public methods of the element's class or its bases touching only element, local, or captured-read state)" },
				{ "chained_field_method", "error method 'read' was not admitted in this parallel body (admitted: single-overload public methods of the element's class or its bases touching only element, local, or captured-read state)" },
				{ "getter", "error only direct field access on class instances is allowed in a parallel body" },
				{ "literal_string", "value 2208" },
				{ "captured_string_table", "value 2208" },
				{ "skipped_subtree", "value 252000" },
				{ "new_argument", "value 16128" },
			};
			const char* driver = " array<Cell> cells = []; for (int i = 0; i < 64; ++i) { cells.push(Cell(i)); }"
			                     " for (int round = 0; round < 3; ++round) { parallel_for (auto& cell : cells) { cell.v = cell.calc(); } }"
			                     " int total = 0; for (auto& cell : cells) { total += cell.v; } return total;";
			for (const shape& sh : shapes) {
				std::string seen[2];
				for (bool vm : {false, true}) {
					auto e = make_engine();
					e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					e->parallel_thread_count(8);
					e->execution_budget(0);
					std::string out;
					try { out = "value " + std::to_string(e->execute(std::string(sh.definition) + driver).as_int()); }
					catch (const std::exception& ex) { out = std::string("error ") + ex.what(); }
					seen[vm ? 1 : 0] = out;
				}
				check_eq(seen[0], seen[1], std::string(sh.name) + ": both backends land the same");
				auto want = expected.find(sh.name);
				check_true(want != expected.end(), std::string(sh.name) + " has an expectation");
				if (want != expected.end()) {
					check_true(seen[0].rfind(want->second, 0) == 0, std::string(sh.name) + ": expected '" + want->second + "', got '" + seen[0] + "'");
				}
			}
		});

		// Runs src serially (parallel_for spelled for) on the interpreter, then in parallel at 1, 3 and 8 workers on both backends:
		// every run returns the serial value
		auto parallel_matches_serial = [this](const std::string& src, const std::string& label) {
			std::string serial_src = src;
			for (size_t at = serial_src.find("parallel_for"); at != std::string::npos; at = serial_src.find("parallel_for", at)) { serial_src.replace(at, 12, "for"); }
			std::string expected;
			{
				auto e = make_engine();
				jai::stdlib::register_all(*e);
				e->set_backend(jai::backend_type::interpreter);
				expected = e->execute(serial_src).to_string();
			}
			for (bool vm : {false, true}) {
				for (size_t workers : {size_t(1), size_t(3), size_t(8)}) {
					auto e = make_engine();
					jai::stdlib::register_all(*e);
					e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					e->parallel_thread_count(workers);
					e->execution_budget(0);
					std::string got;
					try { got = e->execute(src).to_string(); } catch (const std::exception& ex) { got = std::string("error ") + ex.what(); }
					check_eq(expected, got, label + (vm ? " vm " : " interp ") + std::to_string(workers) + " workers");
				}
			}
		};
		auto parallel_error = [this](const std::string& src, bool vm) -> std::string {
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
			e->parallel_thread_count(8);
			e->execution_budget(0);
			try { e->execute(src); } catch (const std::exception& ex) { return ex.what(); }
			return "no error";
		};

		test("bodies_construct_script_classes", [parallel_matches_serial]() {
			// hits[r] = Hit(...): every worker constructs into the slot its element names; the constructor builds a nested object,
			// reads a captured global and calls a script function and a math builtin, and the strings it mints are its own
			parallel_matches_serial(R"(
				float SCALE = 0.5;
				string PREFIX = "hit-";
				float spread(float d) { return d * SCALE + math::sqrt(d); }
				class Vec { float x = 0.0; float y = 0.0; Vec() {} Vec(float a, float b) { x = a; y = b; } }
				class Hit {
					int id = -1;
					float dist = 0.0;
					Vec at;
					string tag = PREFIX;
					Hit() {}
					Hit(int i, float d) { at = Vec(d, spread(d)); id = i; dist = d; tag = tag + to_string(i); }
					float score() { return dist + at.y + tag.size(); }
				}
				array<int> rays;
				for (int i = 0; i < 300; ++i) { rays.push(i); }
				array<Hit> hits;
				for (int i = 0; i < 300; ++i) { hits.push(Hit()); }
				float total = 0.0;
				for (int round = 0; round < 3; ++round) {
					parallel_for (int r : rays) { hits[r] = Hit(r + round, r * 0.25); }
					for (auto& h : hits) { total += h.score(); }
				}
				to_string(total) + " " + hits[17].tag + " " + to_string(hits[299].at.y);
			)", "Hit example");
		});

		test("every_construction_form_runs_in_a_body", [parallel_matches_serial]() {
			// Name n(args), Name n;, Name(args), new Name(args), a class without a constructor whose field initializers read
			// captured globals, a delegating constructor, a base constructed with arguments and one default-constructed, a
			// method on the new object, a construction nested in another constructor, and the implicit conversions into typed
			// slots: a converting constructor (a declaration, an argument, a return), to_int, to_float, to_char, to_bool and to_string, and
			// an object's text after a string literal
			parallel_matches_serial(R"(
				float SCALE = 3.0;
				string LABEL = "cell";
				float twice(float v) { return v * 2.0; }
				class Base { int base_v = 1; string name = LABEL; Base() {} Base(int b) { base_v = b; } }
				class Mixin { int m = 4; }
				class Inner : Base, Mixin {
					float w = SCALE * 2.0;
					Inner() : super(5) {}
					Inner(float a) : super(cast<int>(a)) { w = twice(a) + m; }
					Inner(float a, float b) : this(a + b) { w = w + 1.0; }
					float score() { return w + base_v + name.size(); }
				}
				class Plain { float p = SCALE; array<int> list = [1, 2, 3]; string s = `${LABEL}!`; }
				class Outer {
					Inner in;
					Plain pl;
					int tally = 0;
					Outer(int k) { in = Inner(cast<float>(k), 1.0); tally = k + in.base_v; }
				}
				class Box { int n = 0; Box(int a) { n = a * 2; } }
				int unbox(Box b) { return b.n; }
				Box boxed(int a) { return a + 1; }
				class Num {
					int v = 0;
					Num(int a) { v = a; }
					int to_int() { return v + 100; }
					float to_float() { return v * 0.5; }
					char to_char() { return 'a'; }
					bool to_bool() { return v % 2 == 0; }
					string to_string() { return `n${v}`; }
				}
				array<int> xs;
				for (int i = 0; i < 200; ++i) { xs.push(i); }
				array<float> out;
				for (int i = 0; i < 200; ++i) { out.push(0.0); }
				array<shared_ptr<Inner>> handles;
				for (int i = 0; i < 200; ++i) { handles.push(null); }
				parallel_for (auto& x : xs) {
					Outer o(x);
					Plain p;
					Inner q;
					shared_ptr<Inner> h = new Inner(cast<float>(x));
					handles[x] = new Inner(cast<float>(x), 2.0);
					Box bx = x;
					Box rb = boxed(x);
					Num nm = Num(x);
					int ni = nm;
					float nf = nm;
					char nc = nm;
					bool nb = nm;
					string ns = nm;
					string label = `#${nm}`;
					out[x] = o.in.score() + o.tally + p.p + p.list.size() + p.s.size() + q.score() + h->score() + Inner(1.0).score() +
					         bx.n + unbox(x) + rb.n + ni + nf + (nc == 'a' ? 1 : 0) + (nb ? 1 : 0) + ns.size() + label.size();
				}
				float total = 0.0;
				for (float v : out) { total += v; }
				for (auto& h : handles) { total += h->score(); }
				to_string(total);
			)", "construction forms");
		});

		test("conversions_reach_every_class_a_body_can_hold", [this, parallel_matches_serial, parallel_error]() {
			// a body converts an instance it reaches by any road: a captured object, a captured var holding one, a captured
			// subclass instance, a field of a field of the element (a condition too), a value an admitted function returns and an element of a var
			// array mixing classes, and an instance a captured array or map holds; each converts as the serial loop does. An element
			// whose var field holds one is refused before any element runs
			const std::string classes = R"(
				class Num {
					int v = 3;
					Num() {}
					Num(int a) { v = a; }
					int to_int() { return v + 100; }
					bool to_bool() { return v > 4; }
					string to_string() { return `n${v}`; }
				}
				class Sub : Num { Sub() { v = 9; } }
				class Tip { int t = 1; bool to_bool() { return t > 0; } }
				class Inner { Num n = Num(4); Tip tip; }
				class Outer { Inner in; }
				Num make(int a) { Num r(a); return r; }
				Num capnum = Num(8);
				var capvar = Num(2);
				Sub capsub;
				array<int> xs; for (int i = 0; i < 200; ++i) { xs.push(i); }
				array<int> out; for (int i = 0; i < 200; ++i) { out.push(0); }
				array<Outer> outers; for (int i = 0; i < 200; ++i) { outers.push(Outer()); }
				var mixed = []; for (int i = 0; i < 200; ++i) { if (i % 2 == 0) { mixed.push(Num(i)); } else { mixed.push(Sub()); } }
			)";
			parallel_matches_serial(classes + R"(
				parallel_for (auto& x : xs) {
					int a = capnum; int b = capvar; int c = capsub; int d = make(x); bool e = make(x); string f = capvar;
					x = a + b + c + d + (e ? 1 : 0) + f.size();
				}
				parallel_for (auto& w : outers) { int k = w.in.n; bool t = w.in.n; if (w.in.tip) { k = k + 2; } w.in.n.v = k + (t ? 1 : 0); }
				parallel_for (auto& m : mixed) { int k = m; string s = m; m.v = k + s.size(); }
				int total = 0;
				for (int v : xs) { total += v; }
				for (auto& w : outers) { total += w.in.n.v; }
				for (auto& m : mixed) { total += m.v; }
				to_string(total);
			)", "conversion roads");
			parallel_matches_serial(classes + R"(
				array<Num> caps = [Num(5), Num(6)];
				map<string, Num> named = {"a": Num(7)};
				parallel_for (int x : xs) { int k = caps[x % 2]; int j = named["a"]; out[x] = k + j + caps[1].v + named["a"].to_int(); }
				int total = 0;
				for (int v : out) { total += v; }
				to_string(total);
			)", "captured containers of instances");
			struct refusal { const char* name; const char* source; const char* text; };
			const refusal refusals[] = {
				{ "captured handles", "array<shared_ptr<Num>> hs = [make_shared<Num>(1)]; parallel_for (int x : xs) { out[x] = hs[0].v; }",
				  "parallel_for: captured name 'hs' is not value-semantic (a shared_ptr in a container)" },
				{ "captured table store", "array<Num> caps = [Num(5)]; parallel_for (int x : xs) { caps[0].v = x; }",
				  "parallel_for: member store reaches captured state 'caps' (captured reads are read-only)" },
				{ "var field", "class Holder { var f = 0; } array<Holder> hs; for (int i = 0; i < 8; ++i) { Holder h; h.f = Num(i); hs.push(h); } parallel_for (auto& h : hs) { int k = h.f; h.f = k; }",
				  "parallel_for: element 0 cannot be proven exclusive" },
			};
			for (const refusal& r : refusals) {
				for (bool vm : {false, true}) {
					const std::string got = parallel_error(classes + r.source, vm);
					check_true(got.rfind(r.text, 0) == 0, std::string(r.name) + (vm ? " vm" : " interp") + ": expected '" + r.text + "', got '" + got + "'");
				}
			}
		});

		test("math_vectors_are_values_to_the_element_and_capture_proofs", [this, parallel_matches_serial, parallel_error]() {
			// A math vector is a value: an array of them, a class holding them (at any depth) and a var array of them bind in place,
			// even when every element shares one node (pushed copies of one instance), and a captured vector, or a container of
			// them, reads its components through a worker's own copy; a captured vector is still read-only
			const std::string decls = R"(
				using math::vec2; using math::vec3; using math::ivec2; using math::color;
				class P { vec2 pos; vec2 vel = vec2(1.0, 0.5); }
				class Holder { P p; vec3 v3; ivec2 cell = ivec2(1, 2); color c = color(0.1, 0.2, 0.3, 1.0); }
				vec2 gravity = vec2(0.0, -2.0);
				array<vec2> table = [vec2(1, 2), vec2(3, 4)];
				map<string, vec2> named = {"a": vec2(5, 6)};
				P proto;
				Holder h0;
				vec2 one = vec2(1.0, 1.0);
				array<P> ps; array<Holder> hs; array<vec2> pts; var boxed = []; array<float> outs;
				for (int i = 0; i < 200; ++i) { ps.push(proto); hs.push(h0); pts.push(one); boxed.push(one); outs.push(0.0); }
			)";
			parallel_matches_serial(decls + R"(
				parallel_for (auto& p : ps) { p.vel += gravity; p.pos += p.vel; }
				parallel_for (auto& q : pts) { q.x += 1.0; q += table[1]; }
				parallel_for (auto& h : hs) { h.p.pos += h.p.vel; h.v3 = h.v3 + vec3(1.0, 2.0, 3.0); h.cell.y += 1; h.c.r = 0.9; }
				parallel_for (auto& b : boxed) { b += vec2(0.5, 0.5); }
				parallel_for (int i = 0; i < 200; ++i) { outs[i] = pts[i].x + gravity.y + table[0].x + named["a"].y; }
				to_string(ps[199].pos) + to_string(proto.pos) + to_string(pts[0]) + to_string(one) + to_string(hs[7].p.pos) + to_string(hs[7].cell) +
					to_string(hs[7].c) + to_string(h0.c) + to_string(boxed[3]) + to_string(outs[5]);
			)", "math vectors");
			for (bool vm : {false, true}) {
				const std::string got = parallel_error(decls + "parallel_for (auto& o : outs) { gravity.x = o; }", vm);
				check_eq(std::string("parallel_for: member store reaches captured state 'gravity' (captured reads are read-only) at 13:44"), got, vm ? "vm" : "interp");
			}
		});

		test("reduce_accumulators_are_constructed_objects", [parallel_matches_serial]() {
			// step and combine construct the accumulator they return; the fold is the serial one at every worker count
			parallel_matches_serial(R"(
				class Stats {
					int n = 0;
					float sum = 0.0;
					string tag = "s";
					Stats() {}
					Stats(float s, int c) { sum = s; n = c; tag = "s" + to_string(c); }
				}
				array<int> xs;
				for (int i = 0; i < 1000; ++i) { xs.push(i * 3 % 17); }
				Stats by_value = parallel_reduce(xs, Stats(), [](Stats acc, int x) -> Stats { return Stats(acc.sum + x * 0.5, acc.n + 1); },
				                                 [](Stats a, Stats b) -> Stats { return Stats(a.sum + b.sum, a.n + b.n); });
				Stats in_place = parallel_reduce(xs, Stats(), [](Stats& acc, int x) { acc = Stats(acc.sum + x, acc.n + 1); },
				                                 [](Stats a, Stats b) -> Stats { return Stats(a.sum + b.sum, a.n + b.n); });
				to_string(by_value.sum) + " " + by_value.tag + " " + to_string(in_place.sum) + " " + in_place.tag;
			)", "reduce accumulators");
		});

		test("destructors_of_objects_dying_on_a_worker_run_there", [this, parallel_matches_serial, parallel_error]() {
			// An object constructed in the body dies in it, and an element a body replaces dies on its worker: each destructor
			// runs on that worker's backend (it once ran on the engine's from a pool thread); one that throws fails the statement
			parallel_matches_serial(R"(
				class Res { int v = 0; array<int> held = [1, 2]; Res() {} Res(int a) { v = a; } ~Res() { v = -1; held = []; } }
				class Slot { Res r; int k = 0; Slot() {} Slot(int a) { r = Res(a); k = a; } ~Slot() { k = 0; } }
				array<Slot> slots;
				for (int i = 0; i < 200; ++i) { slots.push(Slot(i)); }
				parallel_for (auto& s : slots) {
					Res temp(s.k);
					Slot other = Slot(s.k + 1);
					s = other;
					s.k = s.k + temp.v;
				}
				int total = 0;
				for (auto& s : slots) { total += s.k + s.r.v; }
				total;
			)", "worker destructors");
			// an index-written array of objects, typed and untyped: the body reads its slot's object and replaces it
			parallel_matches_serial(R"(
				class Res { int v = 0; string s = "r"; Res() {} Res(int a) { v = a; s = s + to_string(a); } ~Res() { v = -1; } }
				array<int> xs;
				for (int i = 0; i < 100; ++i) { xs.push(i); }
				array<Res> out;
				for (int i = 0; i < 100; ++i) { out.push(Res(i)); }
				var loose = [];
				for (int i = 0; i < 100; ++i) { loose.push(Res(i)); }
				parallel_for (int x : xs) {
					Res old = out[x];
					out[x] = Res(old.v * 2);
					var l = loose[x];
					loose[x] = Res(l.v + 1);
				}
				int total = 0;
				for (auto& r : out) { total += r.v + r.s.size(); }
				for (var r : loose) { total += r.v; }
				to_string(total) + " " + out[9].s + " " + loose[9].s;
			)", "index-written objects");
			const char* throwing = R"(
				class T { int v = 0; T(int a) { v = a; } ~T() { if (v == 3) { throw "destructor " + to_string(v); } } }
				array<int> xs = [1, 2, 3, 4];
				parallel_for (auto& x : xs) { T t(x); x = t.v * 10; }
				0;
			)";
			for (bool vm : {false, true}) {
				check_true(parallel_error(throwing, vm).find("destructor 3") != std::string::npos, std::string("a throwing destructor fails the statement ") + (vm ? "vm" : "interp"));
			}
		});

		test("constructions_that_reach_unprovable_code_are_refused", [this, parallel_error]() {
			struct refusal { const char* name; const char* source; const char* text; };
			const refusal refusals[] = {
				{ "enclosing write", "int next_id = 0; class Tick { int id = 0; Tick() { id = next_id; next_id++; } } array<int> xs = [1, 2]; parallel_for (auto& x : xs) { Tick t; x = t.id; }",
				  "parallel_for: body writes enclosing state 'next_id' in the constructor of 'Tick' at 1:" },
				{ "static write", "class T { static int count = 0; int v = 0; T() { count++; v = 1; } } array<int> xs = [1, 2]; parallel_for (auto& x : xs) { T t; x = t.v; }",
				  "parallel_for: body writes enclosing state 'count' in the constructor of 'T' at 1:" },
				{ "field initializer", "int next = 0; int take_id() { next++; return next; } class T { int id = take_id(); } array<int> xs = [1, 2]; parallel_for (auto& x : xs) { T t; x = t.id; }",
				  "parallel_for: body writes enclosing state 'next' in a field initializer of 'T' at 1:" },
				{ "base constructor", "int made = 0; class Base { int b = 0; Base() { made = made + 1; } } class D : Base { int d = 2; D(int a) : super() { d = a; } } array<int> xs = [1, 2]; parallel_for (auto& x : xs) { D t(x); x = t.d; }",
				  "parallel_for: body writes enclosing state 'made' in the constructor of 'Base' at 1:" },
				{ "converting constructor", "int made = 0; class Box { int n = 0; Box(int a) { made++; n = a; } } array<int> xs = [1, 2]; parallel_for (auto& x : xs) { Box b = x; x = b.n; }",
				  "parallel_for: body writes enclosing state 'made' in the constructor of 'Box' at 1:" },
				{ "destructor", "int deaths = 0; class H { int v = 0; ~H() { deaths++; } } array<int> xs = [1, 2]; parallel_for (auto& x : xs) { H h; x = h.v; }",
				  "parallel_for: body writes enclosing state 'deaths' in the destructor of 'H' at 1:" },
				{ "written array destructor", "int deaths = 0; class Res { int v = 0; ~Res() { deaths++; } } array<int> xs = [0, 1]; var loose = [Res(), Res()]; parallel_for (int x : xs) { loose[x] = 5; }",
				  "parallel_for: body writes enclosing state 'deaths' in the destructor of 'Res' at 1:" },
				{ "unprovable call", "class T { int v = 0; T(int a) { print(a); v = a; } } array<int> xs = [1, 2]; parallel_for (auto& x : xs) { T t(x); x = t.v; }",
				  "parallel_for: host function 'print' is marked jai::serial and cannot be called in a parallel body in the constructor of 'T' at 1:" },
				{ "reduce step", "int calls = 0; class Acc { int n = 0; Acc() {} Acc(int a) { calls++; n = a; } } array<int> xs = [1, 2]; parallel_reduce(xs, Acc(), [](Acc a, int x) -> Acc { return Acc(a.n + x); }, [](Acc a, Acc b) -> Acc { return a; });",
				  "parallel_reduce: body writes enclosing state 'calls' in the constructor of 'Acc' at 1:" },
			};
			for (const refusal& r : refusals) {
				for (bool vm : {false, true}) {
					const std::string got = parallel_error(r.source, vm);
					check_true(got.rfind(r.text, 0) == 0, std::string(r.name) + (vm ? " vm" : " interp") + ": expected '" + r.text + "', got '" + got + "'");
				}
			}
			struct host_registry { int64_t v = 0; };
			for (bool vm : {false, true}) {
				auto e = make_engine();
				jai::stdlib::register_all(*e);
				e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				dynamic_binder<host_registry>(*e, "HostRegistry").constructor<>(jai::serial).property("v", &host_registry::v).build();
				e->parallel_thread_count(8);
				auto refused = [&](const char* src) -> std::string {
					try { e->execute(src); } catch (const std::exception& ex) { return ex.what(); }
					return "no error";
				};
				const std::string derived = refused("class Script : HostRegistry { int w = 1; Script() : super() {} } array<int> xs = [1, 2]; parallel_for (auto& x : xs) { Script s; x = s.w; }");
				check_true(derived.rfind("parallel_for: class 'Script' has a C++ base 'HostRegistry' with a constructor marked jai::serial and cannot be constructed in a parallel body at 1:", 0) == 0,
				           std::string("serial-only base ") + (vm ? "vm" : "interp") + ": " + derived);
				const std::string host = refused("array<int> xs = [1, 2]; parallel_for (auto& x : xs) { HostRegistry r; x = 1; }");
				check_true(host.rfind("parallel_for: C++ class 'HostRegistry' has a constructor marked jai::serial and cannot be constructed in a parallel body at 1:", 0) == 0,
				           std::string("serial-only class ") + (vm ? "vm" : "interp") + ": " + host);
			}
		});

		test("hybrids_and_host_classes_construct_and_die_on_workers", [this]() {
			// Host construction is safe on any thread unless the binder says otherwise: a script class over a host class, and the
			// host class itself, construct on every worker; each C++ object dies there too, and the script level's destructor runs
			// on the worker's backend
			struct counted_host {
				int64_t v = 7;
				counted_host() { ++parallel_hosts_made; }
				counted_host(const counted_host& other) : v(other.v) { ++parallel_hosts_made; }
				~counted_host() { ++parallel_hosts_died; }
			};
			const char* src = R"(
				class Hyb : CountedHost {
					int w = 1;
					string tag = "h";
					Hyb(int k) : super() { w = k; tag = tag + to_string(k); }
					~Hyb() { w = 0; tag = ""; }
				}
				array<int> xs;
				for (int i = 0; i < 400; ++i) { xs.push(i); }
				array<int> out;
				for (int i = 0; i < 400; ++i) { out.push(0); }
				parallel_for (int x : xs) {
					Hyb h(x);
					CountedHost bare;
					Hyb second = Hyb(x + 1);
					out[x] = h.w + h.tag.size() + second.w;
				}
				int total = 0;
				for (int v : out) { total += v; }
				total;
			)";
			int64_t expected = 0;
			for (int64_t x = 0; x < 400; ++x) { expected += x + 1 + static_cast<int64_t>(std::to_string(x).size()) + x + 1; }
			for (bool vm : {false, true}) {
				for (size_t workers : {size_t(1), size_t(8)}) {
					parallel_hosts_made = 0;
					parallel_hosts_died = 0;
					{
						auto e = make_engine();
						jai::stdlib::register_all(*e);
						e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
						dynamic_binder<counted_host>(*e, "CountedHost").constructor<>().property("v", &counted_host::v).build();
						e->parallel_thread_count(workers);
						e->execution_budget(0);
						std::string got;
						try { got = std::to_string(e->execute(src).as_int()); } catch (const std::exception& ex) { got = std::string("error ") + ex.what(); }
						check_eq(std::to_string(expected), got, std::string("hybrid total ") + (vm ? "vm " : "interp ") + std::to_string(workers));
					}
					check_true(parallel_hosts_made.load() >= 1200, "every host object was made");
					check_eq(parallel_hosts_made.load(), parallel_hosts_died.load(), std::string("every host object died ") + (vm ? "vm" : "interp"));
				}
			}
		});

		test("the_binder_guide_s_serial_only_sample_prints_what_the_page_shows", [this]() {
			// docs/site/embedding/02-dynamic-binder.html#serial-only-constructors: the host sample the site runner skips
			struct doc_sprite {
				static std::vector<doc_sprite*>& registry() { static std::vector<doc_sprite*> all; return all; }
				double alpha = 1.0;
				doc_sprite() { registry().push_back(this); }
				~doc_sprite() { std::erase(registry(), this); }
			};
			const char* src = "class Glowing : Sprite { float glow = 0.5; Glowing() : super() {} }\n"
			                  "array<int> lights = [1, 2, 3];\n"
			                  "try { parallel_for (auto& n : lights) { Sprite s; n = 0; } } catch (e) { print(e); }\n"
			                  "try { parallel_for (auto& n : lights) { Glowing g; n = 0; } } catch (e) { print(e); }\n"
			                  "Glowing one;                              // a serial construction is unaffected\n"
			                  "print(\"{} {}\", one.alpha, one.glow);\n";
			const std::string expected =
				"parallel_for: C++ class 'Sprite' has a constructor marked jai::serial and cannot be constructed in a parallel body at 3:48\n"
				"parallel_for: class 'Glowing' has a C++ base 'Sprite' with a constructor marked jai::serial and cannot be constructed in a parallel body at 4:49\n"
				"1.0 0.5\n";
			for (bool vm : {false, true}) {
				auto e = make_engine();
				jai::stdlib::register_all(*e);
				e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				auto out = std::make_shared<std::ostringstream>();
				e->set_output_stream(out);
				jai::dynamic_binder<doc_sprite>(*e, "Sprite").constructor<>(jai::serial).property("alpha", &doc_sprite::alpha).build();
				try { e->execute(src); } catch (const std::exception& ex) { *out << "threw: " << ex.what(); }
				check_eq(expected, out->str(), vm ? "vm" : "interp");
			}
		});

		test("a_serial_only_object_dying_on_a_chunk_is_released_by_the_join", [this]() {
			// No admitted body reaches such an object (construction is refused, and elements and captures hold no host object), so
			// the region is stood up by hand: on a chunk thread the C++ object waits for the join, which releases every chunk's
			// deferred objects on the calling thread in element order, then in the order they died
			struct serial_host {
				int64_t id = 0;
				~serial_host() { if (id) { parallel_serial_deaths.push_back(id); } }
			};
			struct plain_host {
				int64_t id = 0;
				~plain_host() { if (id) { parallel_serial_deaths.push_back(id); } }
			};
			for (bool vm : {false, true}) {
				parallel_serial_deaths.clear();
				auto e = make_engine();
				e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				dynamic_binder<serial_host>(*e, "SerialHost").constructor<>(jai::serial).property("id", &serial_host::id).build();
				dynamic_binder<plain_host>(*e, "PlainHost").constructor<>().property("id", &plain_host::id).build();
				e->execute("class Glow : SerialHost { int glow = 1; Glow() : super() {} }\n"
				           "SerialHost serial(int k) { SerialHost s; s.id = k; return s; }\n"
				           "Glow glowing(int k) { Glow g; g.id = k; return g; }\n"
				           "PlainHost plain(int k) { PlainHost p; p.id = k; return p; }");
				auto made = [&](const char* src) { return e->execute(src); };
				script_value first = made("serial(1);");
				script_value hybrid = made("glowing(2);");
				script_value later = made("serial(3);");
				script_value earlier = made("serial(4);");
				script_value plain = made("plain(5);");
				parallel_serial_deaths.clear();   // the temporaries the copies came from

				detail::execution_limits limits;
				std::vector<detail::deferred_release> deferred;
				detail::parallel_region_table table;
				table.entries.push_back({ std::this_thread::get_id(), &limits });
				table.entries.back().deferred_releases = &deferred;
				detail::engine_access::parallel_state(*e).active_region = &table;
				detail::engine_access::note_parallel_region(*e, true);
				first = script_value(std::monostate{}, e.get());
				hybrid = script_value(std::monostate{}, e.get());
				plain = script_value(std::monostate{}, e.get());
				deferred.push_back({ 7, std::move(later) });
				deferred.push_back({ 3, std::move(earlier) });
				const std::vector<int64_t> during = parallel_serial_deaths;
				detail::engine_access::note_parallel_region(*e, false);
				detail::engine_access::parallel_state(*e).active_region = nullptr;
				detail::release_deferred_in_order({ &deferred });

				auto spelled = [](const std::vector<int64_t>& ids) {
					std::string result;
					for (int64_t id : ids) { result += std::to_string(id) + " "; }
					return result;
				};
				check_eq(std::string("5 "), spelled(during), std::string("only the plain host object died on the chunk ") + (vm ? "vm" : "interp"));
				check_eq(std::string("5 4 3 1 2 "), spelled(parallel_serial_deaths),
				         std::string("the join released by element, then in death order ") + (vm ? "vm" : "interp"));
				check_true(deferred.empty(), "the join empties the list");
			}
		});

		test("constructions_with_field_initializers_on_workers_leave_the_engine_env_epoch_alone", [this]() {
			// The engine's env epoch is not atomic: a worker's environments take their serials from its own sink (Debug asserts a
			// frozen symbolizer's epoch never moves)
			const std::string program = R"(
				class Inner { int k = 2; }
				class P { array<int> a = [1, 2]; int n = 3; Inner inner; string s = "x" + "y"; }
				class Q : P { int m = n + 1; }
				array<int> xs;
				for (int i = 0; i < 2000; ++i) { xs.push(i); }
				array<int> out;
				for (int i = 0; i < 2000; ++i) { out.push(0); }
				parallel_for (int x : xs) {
					Q q;
					out[x] = q.n + q.a.size() + q.inner.k + q.m + q.s.size() + x;
				}
				int total = 0;
				for (int v : out) { total += v; }
				total;
			)";
			for (bool vm : {false, true}) {
				auto e = make_engine();
				jai::stdlib::register_all(*e);
				e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->parallel_thread_count(8);
				std::string got;
				try { got = std::to_string(e->execute(program).as_int()); } catch (const std::exception& ex) { got = std::string("error ") + ex.what(); }
				check_eq(std::to_string(2000 * (3 + 2 + 2 + 4 + 2) + 1999 * 2000 / 2), got, vm ? "vm" : "interp");
			}
		});

		test("first_constructions_on_workers_share_the_implicit_default_constructor_the_declaration_made", [this]() {
			// Each class's first constructions run on every worker at once: its implicit default constructor is declared with the
			// class (workers built it on first use, one freeing another's mid-construction), so the region only reads it
			constexpr int classes = 32;
			for (bool vm : {false, true}) {
				auto e = make_engine();
				jai::stdlib::register_all(*e);
				e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				e->parallel_thread_count(8);
				e->execute("array<int> xs; for (int i = 0; i < 64; ++i) { xs.push(i); } array<int> out; for (int i = 0; i < 64; ++i) { out.push(0); } int total = 0;");
				std::string got;
				for (int c = 0; c < classes && got.empty(); ++c) {
					const std::string n = std::to_string(c);
					const std::string program = "class B" + n + " { array<int> a = [1, 2]; int n = 3; }\n"
					                            "class D" + n + " : B" + n + " { int m = n + 1; }\n"
					                            "parallel_for (int x : xs) { D" + n + " d; out[x] = d.m + d.a.size(); }\n"
					                            "total = 0; for (int v : out) { total += v; } total;";
					try {
						const int64_t total = e->execute(program).as_int();
						if (total != 64 * (4 + 2)) { got = "class " + n + " totals " + std::to_string(total); }
					} catch (const std::exception& ex) { got = "class " + n + " error " + ex.what(); }
				}
				// a reload declares the classes again, one holding another by value: every region constructs the current ones
				const std::string reloaded = "class RP { float x = 1.0; }\n"
				                             "class RB { RP pos; int n = 2; }\n"
				                             "parallel_for (int x : xs) { RB b; out[x] = cast<int>(b.pos.x) + b.n; }\n"
				                             "total = 0; for (int v : out) { total += v; } total;";
				for (int run = 0; run < 3 && got.empty(); ++run) {
					try {
						const int64_t total = e->execute(reloaded).as_int();
						if (total != 64 * 3) { got = "reload " + std::to_string(run) + " totals " + std::to_string(total); }
					} catch (const std::exception& ex) { got = "reload " + std::to_string(run) + " error " + ex.what(); }
				}
				check_eq(std::string(), got, vm ? "vm" : "interp");
			}
		});

		test("host_calls_with_reference_parameters_keep_their_temporaries_per_worker", [this]() {
			// A bound call converts a const std::vector<int>& (or a const int&) argument into a temporary in the call's parameter
			// storage; workers calling at once each keep their own (the engine-wide storage pointer was shared: one worker's
			// temporary landed in another's stack frame, which destroyed it, and the pointer was left aimed at a dead frame)
			struct summer {
				int64_t sum(const std::vector<int64_t>& values, const int64_t& bias) const {
					int64_t result = bias;
					for (int64_t v : values) { result += v; }
					return result;
				}
			};
			const std::string program = R"(
				array<int> xs;
				for (int i = 0; i < 4000; ++i) { xs.push(i); }
				array<int> out;
				for (int i = 0; i < 4000; ++i) { out.push(0); }
				for (int round = 0; round < 8; ++round) {
					parallel_for (int x : xs) {
						Summer s;
						out[x] = s.sum([x, x, 1], x);
					}
				}
				int total = 0;
				for (int v : out) { total += v; }
				total;
			)";
			int64_t expected = 0;
			for (int64_t x = 0; x < 4000; ++x) { expected += 3 * x + 1; }
			for (bool vm : {false, true}) {
				auto e = make_engine();
				jai::stdlib::register_all(*e);
				e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				dynamic_binder<summer>(*e, "Summer").constructor<>().method("sum", &summer::sum).build();
				e->parallel_thread_count(8);
				e->execution_budget(0);
				std::string got;
				try { got = std::to_string(e->execute(program).as_int()); } catch (const std::exception& ex) { got = std::string("error ") + ex.what(); }
				check_eq(std::to_string(expected), got, std::string("reference parameters ") + (vm ? "vm" : "interp"));
				check_true(detail::engine_access::get_current_parameter_storage(*e) == nullptr, std::string("no call's storage outlives the region ") + (vm ? "vm" : "interp"));
			}
		});

		test("generated_bodies_run_as_the_serial_loop", [this]() {
			// the parallel band's seeds: an admitted body prints the serial loop's output at 1, 3 and 8 workers on the interpreter,
			// the vm and its reference mode; a hazard body is refused naming what it wrote (source/tests/fuzz/fuzz_parallel_band.hpp)
			for (uint64_t seed = 0; seed < 24; ++seed) {
				for (bool hazard : {false, true}) {
					const jai::fuzz::parallel::generated g = jai::fuzz::parallel::generate(seed, hazard);
					check_eq(std::string(), jai::fuzz::parallel::check(g), "seed " + std::to_string(seed) + (hazard ? " (hazard)" : "") + "\n" + g.program);
				}
			}
		});

		// Each library builtin is parallel unless the library marks it serial; a new one takes its row here, so the choice is made
		test("every_library_builtin_is_classified_parallel_or_serial", [this]() {
			auto e = make_engine();
			jai::stdlib::register_all(*e);
			jai::stdlib::register_web(*e);
			jai::stdlib::register_spider(*e);
			std::set<std::string> names;
			for (const std::string& name : detail::engine_access::host_function_names(*e)) { names.insert(name); }
			auto* sym = detail::engine_access::get_symbolizer(*e);
			for (const auto& [space_id, space] : detail::engine_access::script_namespaces(*e)) {
				if (!space) { continue; }
				for (const auto& [member_id, value] : space->variables) {
					if (value.is_function() && !space->functions.count(member_id)) {
						names.insert(std::string(sym->get_string(space_id)) + "::" + std::string(sym->get_string(member_id)));
					}
				}
			}
			const std::set<std::string> serial = {
				"__jai_parallel_range",
				"app_cache_dir", "app_data_dir", "array::filter", "array::map", "array::sort", "crypto::sha256_file", "extend",
				"file_data", "from_base64", "from_binary", "from_json", "import_recheck_interval", "map::filter", "parallel_reduce",
				"parallel_transform", "print", "reflect::bases", "reflect::bind", "reflect::call", "reflect::classes",
				"reflect::construct", "reflect::construct_shared", "reflect::fields", "reflect::function_arguments",
				"reflect::functions", "reflect::generation", "reflect::get", "reflect::globals", "reflect::has_field",
				"reflect::has_method", "reflect::instances", "reflect::invoke", "reflect::is_a", "reflect::is_cpp_bound",
				"reflect::method_arguments", "reflect::methods", "reflect::scoped_template_dirs",
				"reflect::scoped_template_output_limit", "reflect::set", "reflect::template_output_limit", "reflect::type_name",
				"resolve_path", "sleep", "spider::Response", "spider::catch", "spider::get", "spider::post", "spider::sling",
				"template#dynamic", "template#html", "template#raw", "template#scoped", "template#scoped_file", "template#splice",
				"time::date", "time::date_from_unix", "time::date_now", "time::engine_elapsed", "time::execute_elapsed",
				"time::local_date", "time::local_now", "time::now", "time::stopwatch", "time::unix_ms", "time::unix_time",
				"time::utc_date", "time::utc_now", "to_base64", "to_binary", "to_json"
			};
			const std::set<std::string> parallel = {
				"array::append", "array::concat", "array::fill", "array::resize", "array::reverse", "array::slice", "base64_decode",
				"base64_encode", "bin_dir", "binary::bytes", "binary::layout", "binary::reader", "binary::text", "binary::width",
				"binary::writer", "catch_takes#", "catch_value#", "crypto::equals", "crypto::hmac_sha256", "crypto::hmac_sha256_bytes", "crypto::hmac_sha512",
				"crypto::hmac_sha512_bytes", "crypto::md5", "crypto::md5_bytes", "crypto::password_hash", "crypto::password_verify",
				"crypto::random_bytes", "crypto::random_hex", "crypto::random_int", "crypto::sha1", "crypto::sha1_bytes",
				"crypto::sha256", "crypto::sha256_bytes", "crypto::sha512", "crypto::sha512_bytes", "current_script_dir",
				"current_script_path", "executable_dir", "executable_path", "file_name", "format", "format_value",
				"is_registered_type", "jai::permissions::is_enabled", "math::abs", "math::acos", "math::angle", "math::angle_between", "math::asin", "math::atan", "math::atan2",
				"math::cbrt", "math::ceil", "math::clamp", "math::color", "math::cos", "math::cosh", "math::cross", "math::degrees",
				"math::degrees_to_radians", "math::distance", "math::distance_squared", "math::dot", "math::exp", "math::exp2",
				"math::floor", "math::fmod", "math::from_angle", "math::hypot", "math::ifloor", "math::is_inf", "math::is_nan", "math::itrunc",
				"math::ivec2", "math::ivec3", "math::length", "math::length_squared", "math::lerp", "math::log", "math::log10",
				"math::log2", "math::max", "math::min", "math::mix", "math::mix_hsv", "math::mix_in", "math::mix_in_out",
				"math::mix_out", "math::mix_out_in", "math::mix_rgb", "math::normalize", "math::perpendicular", "math::pow",
				"math::project", "math::radians", "math::radians_to_degrees", "math::reflect", "math::remap", "math::rotate", "math::rotate_around",
				"math::round", "math::saturate", "math::sign", "math::sin", "math::sinh", "math::sqrt", "math::tan", "math::tanh",
				"math::trunc", "math::unmix", "math::unmix_in", "math::unmix_in_out", "math::unmix_out", "math::unmix_out_in",
				"math::vec2", "math::vec3", "math::vec4", "math::wrap", "math::wrap_add", "math::wrap_mul", "math::wrap_sub", "merge",
				"pair", "parent_dir", "random", "regex", "regex::match", "root_script_dir", "root_script_path", "same_object",
				"skip_flush_t", "skip_newline_t", "string::append", "string::from_bytes", "string::from_code", "string::is_valid_utf8",
				"string::join", "string::pad_left", "string::pad_right", "string::repeat", "string::replace", "string::to_lower",
				"string::to_upper", "string::trim", "thread_count", "to_char", "to_string", "type_of", "utf8_string", "vformat",
				"web::build_query", "web::form_decode", "web::form_encode", "web::html", "web::html_escape", "web::html_unescape",
				"web::parse_query", "web::parse_query_all", "web::parse_url", "web::url", "web::url_decode", "web::url_encode",
				"working_dir"
			};
			std::string table;
			for (const std::string& name : names) {
				const bool is_serial = detail::engine_access::host_function_serial(*e, name);
				const bool listed = is_serial ? serial.count(name) != 0 : parallel.count(name) != 0;
				if (!listed) { table += std::string(is_serial ? "serial   " : "parallel ") + name + "\n"; }
			}
			for (const auto* listed : {&serial, &parallel}) {
				for (const std::string& name : *listed) {
					if (!names.count(name)) { table += "gone     " + name + "\n"; }
				}
			}
			check_eq(std::string(), table, "library builtins whose row is missing or wrong");
			for (size_t workers : {size_t(1), size_t(8)}) {
				auto run = make_engine();
				jai::stdlib::register_all(*run);
				run->parallel_thread_count(workers);
				const std::string got = run->execute(R"(
					string glyph(int x) { return string::repeat(to_string(to_char(65 + x % 26)), 2); }
					array<int> xs;
					for (int i = 0; i < 520; ++i) { xs.push(i); }
					array<string> out = parallel_transform(xs, glyph);
					out[0] + out[25] + out[519];
				)").as<std::string>();
				check_eq(std::string("AAZZZZ"), got, "to_char and string:: in a parallel body, workers " + std::to_string(workers));
				// a worker's copy of format still formats its literal: in the body, a callee, an element's method, a reference argument
				const std::string formatted = run->execute(R"(
					string tag(int x) { return format("<{0}>", x); }
					class Cell { int v = 0; string label = ""; void name() { label = format("c{0}", v); } }
					array<int> idx = [0, 1, 2];
					array<string> out = ["", "", ""];
					array<Cell> cells = [Cell(), Cell(), Cell()];
					array<int> vals = [7, 8, 9];
					parallel_for (int i : idx) { out[i] = format("{0}-{1}", i, i * 2) + tag(i); }
					parallel_for (auto& c, int i : cells, idx) { c.v = i; c.name(); }
					parallel_for (auto& v, int i : vals, idx) { out[i] = out[i] + format("={0}", v); }
					to_json(out) + cells[2].label;
				)").as<std::string>();
				check_eq(std::string(R"(["0-0<0>=7","1-2<1>=8","2-4<2>=9"]c2)"), formatted, "format in a parallel body, workers " + std::to_string(workers));
			}
		});

		test("host_calls_run_in_parallel_unless_marked_serial", [this]() {
			// C++ calls are parallel unless registered serial: free functions, static methods and methods; a non-const method (or a
			// property store) only over an object the chunk made, a const one over any object
			struct host_counter {
				int64_t v = 0;
				void bump() { ++v; }
				int64_t value() const { return v; }
				void reset() { v = 0; }
				static int64_t twice(int64_t k) { return k * 2; }
				static int64_t halve(int64_t k) { return k / 2; }
			};
			auto shared = std::make_shared<host_counter>();
			shared->v = 5;
			auto bind = [&](jai::engine& e) {
				dynamic_binder<host_counter>(e, "Counter").constructor<>()
					.method("bump", &host_counter::bump).method("value", &host_counter::value).method("reset", &host_counter::reset, jai::serial)
					.static_method("twice", &host_counter::twice).static_method("halve", &host_counter::halve, jai::serial)
					.property("v", &host_counter::v).build();
				e.add_function("scale", [](int64_t x) { return x * 3; });
				e.add_function("log_it", [](int64_t) {}, jai::serial);
				e.add_function("try_log", [](int64_t) { return true; }, jai::serial, jai::nodiscard);
				e.add_variadic_function("log_all", [&e](const std::vector<script_value>&) -> script_value { return e.make_null(); }, jai::serial);
				e.add_function("fill", [](int64_t& out) { out = 7; });
				e.add_function("shared_counter", [shared]() { return shared; });
			};
			const std::string program = R"(
				array<int> xs;
				for (int i = 0; i < 200; ++i) { xs.push(i); }
				array<int> out;
				for (int i = 0; i < 200; ++i) { out.push(0); }
				parallel_for (int x : xs) {
					Counter c;
					c.bump();
					c.bump();
					c.v = c.v + x;
					out[x] = c.value() + scale(x) + Counter::twice(x) + shared_counter().value();
				}
				int total = 0;
				for (int v : out) { total += v; }
				total;
			)";
			int64_t expected = 0;
			for (int64_t x = 0; x < 200; ++x) { expected += (2 + x) + x * 3 + x * 2 + 5; }
			for (bool vm : {false, true}) {
				for (size_t workers : {size_t(1), size_t(8)}) {
					auto e = make_engine();
					jai::stdlib::register_all(*e);
					e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					bind(*e);
					e->parallel_thread_count(workers);
					e->execution_budget(0);
					std::string got;
					try { got = std::to_string(e->execute(program).as_int()); } catch (const std::exception& ex) { got = std::string("error ") + ex.what(); }
					check_eq(std::to_string(expected), got, std::string("host calls ") + (vm ? "vm " : "interp ") + std::to_string(workers));
				}
			}
			struct refusal { const char* name; const char* source; const char* text; };
			const refusal refusals[] = {
				{ "serial function", "array<int> xs = [1, 2]; parallel_for (int x : xs) { log_it(x); }",
				  "parallel_for: host function 'log_it' is marked jai::serial and cannot be called in a parallel body at 1:" },
				{ "serial and nodiscard", "array<int> xs = [1, 2]; parallel_for (int x : xs) { bool ok = try_log(x); }",
				  "parallel_for: host function 'try_log' is marked jai::serial and cannot be called in a parallel body at 1:" },
				{ "serial variadic function", "array<int> xs = [1, 2]; parallel_for (int x : xs) { log_all(x, x); }",
				  "parallel_for: host function 'log_all' is marked jai::serial and cannot be called in a parallel body at 1:" },
				{ "library builtin", "array<int> xs = [1, 2]; parallel_for (int x : xs) { print(x); }",
				  "parallel_for: host function 'print' is marked jai::serial and cannot be called in a parallel body at 1:" },
				{ "serial static method", "array<int> xs = [1, 2]; parallel_for (auto& x : xs) { x = Counter::halve(x); }",
				  "parallel_for: C++ static method 'Counter::halve' is marked jai::serial and cannot be called in a parallel body at 1:" },
				{ "writeback argument", "int g = 0; array<int> xs = [1, 2]; parallel_for (int x : xs) { fill(g); }",
				  "parallel_for: argument passes enclosing state 'g' by reference (captured reads are read-only) at 1:" },
				{ "serial method", "array<int> xs = [1, 2]; parallel_for (int x : xs) { Counter c; c.reset(); }",
				  "C++ method 'reset' of 'Counter' is marked jai::serial and cannot be called in a parallel body" },
				{ "non-const method on a shared object", "array<int> xs = [1, 2]; parallel_for (int x : xs) { var s = shared_counter(); s.bump(); }",
				  "C++ method 'bump' of 'Counter' changes an object this parallel body did not make (only its const methods may run on it)" },
				{ "property store on a shared object", "array<int> xs = [1, 2]; parallel_for (int x : xs) { var s = shared_counter(); s.v = x; }",
				  "C++ property 'v' of 'Counter' is stored on an object this parallel body did not make (only an object it made takes stores)" },
			};
			for (const refusal& r : refusals) {
				for (bool vm : {false, true}) {
					auto e = make_engine();
					jai::stdlib::register_all(*e);
					e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					bind(*e);
					e->parallel_thread_count(8);
					std::string got = "no error";
					try { e->execute(r.source); } catch (const std::exception& ex) { got = ex.what(); }
					check_true(got.rfind(r.text, 0) == 0, std::string(r.name) + (vm ? " vm" : " interp") + ": expected '" + r.text + "', got '" + got + "'");
				}
			}
			check_eq((int64_t)5, shared->v, "the shared object was never written");
			auto marked = make_engine();
			bind(*marked);
			check_true(marked->is_nodiscard_function("try_log") && !marked->is_nodiscard_function("log_it"), "serial | nodiscard marks both");
			const auto dropped = marked->check("try_log(1);\nint after = 0;");
			check_true(dropped.format().find("the result of 'try_log' is discarded") != std::string::npos, dropped.format());
		});

		test("the_binder_guide_s_parallel_call_sample_prints_what_the_page_shows", [this]() {
			// docs/site/embedding/02-dynamic-binder.html#parallel-calls: the host sample the site runner skips
			struct doc_meter {
				double level = 0.0;
				void add(double amount) { level += amount; }
				double reading() const { return level; }
				void publish() const {}
			};
			auto master = std::make_shared<doc_meter>();
			master->level = 100.0;
			const char* src = "array<float> xs = [1.0, 2.0, 3.0];\n"
			                  "parallel_for (auto& x : xs) {\n"
			                  "    Meter m;\n"
			                  "    m.add(x * 10.0);                       // the worker's own object\n"
			                  "    x = m.reading() + master().reading();  // a const method on the shared one\n"
			                  "}\n"
			                  "print(\"{}\", to_json(xs));\n"
			                  "try { parallel_for (auto& x : xs) { var shared = master(); shared.add(1.0); } } catch (e) { print(e); }\n"
			                  "try { parallel_for (auto& x : xs) { Meter m; m.publish(); } } catch (e) { print(e); }\n"
			                  "try { parallel_for (auto& x : xs) { log_line(\"x\"); } } catch (e) { print(e); }\n";
			const std::string expected =
				"[110.0,120.0,130.0]\n"
				"C++ method 'add' of 'Meter' changes an object this parallel body did not make (only its const methods may run on it)\n"
				"C++ method 'publish' of 'Meter' is marked jai::serial and cannot be called in a parallel body\n"
				"parallel_for: host function 'log_line' is marked jai::serial and cannot be called in a parallel body at 10:37\n";
			for (bool vm : {false, true}) {
				auto e = make_engine();
				jai::stdlib::register_all(*e);
				e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				auto out = std::make_shared<std::ostringstream>();
				e->set_output_stream(out);
				jai::dynamic_binder<doc_meter>(*e, "Meter").constructor<>().method("add", &doc_meter::add).method("reading", &doc_meter::reading)
					.method("publish", &doc_meter::publish, jai::serial).build();
				e->add_function("master", [master]() { return master; });
				e->add_function("log_line", [](const std::string&) {}, jai::serial);
				e->parallel_thread_count(8);
				try { e->execute(src); } catch (const std::exception& ex) { *out << "threw: " << ex.what(); }
				check_eq(expected, out->str(), vm ? "vm" : "interp");
			}
			check_eq(100.0, master->level, "the shared meter was never written");
		});

		test("draws_from_a_captured_random_are_the_same_at_every_worker_count_and_advance_it_once_per_region", [this]() {
			// A draw from a captured random is keyed by one draw of it, the element and the element's own count: the body, a function
			// it calls, an element's method, a reduce step and a transform give one result at 1 to 6 workers, and the generator moves
			// one draw for each statement that captures it
			const std::string src = R"(
				random R(2026);
				class Die { int face = 0; void roll() { face = R.next_int(1, 6) * 100 + R.next_int(0, 99); } }
				int noise(int i) { return R.next_int(0, 1000000) + i; }
				int sum(int a, int b) { return a + b; }
				int step(int acc, int x) { return acc + R.next_int(0, 9) + x % 7; }
				int jitter(int i) { return R.next_int(0, 99) * 1000 + i; }
				array<int> xs; array<float> fs; array<int> ids; array<Die> dice;
				for (int i = 0; i < 400; ++i) { xs.push(0); fs.push(0.0); ids.push(i); }
				for (int i = 0; i < 300; ++i) { dice.push(Die()); }
				parallel_for (auto& x : xs) { x = noise(0) * 10 + R.next_int(0, 9); }
				array<int> first = xs;
				parallel_for (auto& x : xs) { x = noise(0) * 10 + R.next_int(0, 9); }
				parallel_for (auto& f, int id : fs, ids) { f = R.next_float() + R.next_float(1.0, 2.0) + id; }
				parallel_for (auto& d : dice) { d.roll(); }
				int total = parallel_reduce(xs, 0, step, sum);
				array<int> jittered = parallel_transform(ids, jitter);
				parallel_for (auto& x : xs) { x = x + 1; }
				int after = R.next_int(0, 1000000000);
				random replay(2026);
				for (int k = 0; k < 6; ++k) { replay.next_int(0, 1); }
				bool advanced = replay.next_int(0, 1000000000) == after;
				int faces = 0;
				for (int i = 1; i < 300; ++i) { if (dice[i].face != dice[0].face) { faces++; } }
				string out = to_string(advanced) + " " + to_string(first[0] != xs[0] - 1) + " " + to_string(faces > 250) + " " + to_string(total) + " " + to_string(after);
				for (int i = 0; i < 400; ++i) { out = out + " " + to_string(xs[i]) + ":" + to_string(fs[i]) + ":" + to_string(jittered[i]); }
				for (int i = 0; i < 300; ++i) { out = out + " " + to_string(dice[i].face); }
				out;
			)";
			std::string expected;
			std::shared_ptr<engine> e;
			for (size_t workers : {size_t(1), size_t(2), size_t(3), size_t(4), size_t(5), size_t(6), size_t(6), size_t(6), size_t(6)}) {
				e = make_engine();   // a fresh program each time: a second run's global declarations keep the first run's arrays
				jai::stdlib::register_all(*e);
				e->parallel_thread_count(workers);
				const std::string got = e->execute(src).as_string();
				check_true(got.rfind("true true true ", 0) == 0, "the regions advance the generator once each and draw distinct values");
				if (expected.empty()) {
					expected = got;
				} else {
					check_eq(expected, got, "workers " + std::to_string(workers));
				}
			}
			std::string refused;
			try { e->execute("array<int> ys = [1, 2, 3]; random G(1); parallel_for (auto& y : ys) { G.seed(y); }"); } catch (const std::exception& ex) { refused = ex.what(); }
			check_true(refused.find("parallel_for: random::seed cannot be called on captured 'G' in a parallel body (seed before the loop: each "
			                        "element draws its own stream from it) at 1:") != std::string::npos, refused);
		});

		test("elements_are_their_chunks_own_receivers_and_marks_cover_non_const_overloads", [this]() {
			// A parallel_for element (and a zipped one, and a parallel_reduce accumulator) the distinctness check proved the only one
			// of its C++ object takes non-const methods and property stores; jai::serial on a non-const overload covers the name's
			// non-const overloads, and on a const one (a const member function, a lambda over const T&) every overload, which a
			// later non-const one keeps
			struct host_unit {
				int64_t hp = 10;
				void update(int64_t by) { hp += by; }
				int64_t level() const { return hp; }
				void level(int64_t value) { hp = value; }
				int64_t label() const { return 1; }
			};
			auto bind = [](jai::engine& e) {
				dynamic_binder<host_unit>(e, "Unit").constructor<>()
					.method("update", &host_unit::update)
					.method("level", static_cast<int64_t(host_unit::*)() const>(&host_unit::level))
					.method("level", static_cast<void(host_unit::*)(int64_t)>(&host_unit::level), jai::serial)
					.method("label", &host_unit::label, jai::serial)
					.method("label", [](host_unit& self, int64_t by) { self.hp += by; }, jai::serial)
					.method("tag", [](const host_unit& self) { return self.hp; }, jai::serial)
					.property("hp", &host_unit::hp).build();
			};
			const std::string program = R"(
				array<shared_ptr<Unit>> handles;
				array<Unit> units;
				array<int> ids;
				for (int i = 0; i < 300; ++i) { shared_ptr<Unit> u = new Unit(); handles.push(u); units.push(Unit()); ids.push(i); }
				parallel_for (auto& h, auto& u, int i : handles, units, ids) {
					h.update(i);
					u.hp = u.hp + h.level() + i;
				}
				Unit summed = parallel_reduce(ids, Unit(), [](Unit& acc, int i) { acc.update(i); }, [](Unit a, Unit b) -> Unit { a.hp = a.hp + b.hp; return a; });
				int total = summed.hp;
				for (auto& h : handles) { total += h.hp; }
				for (auto& u : units) { total += u.hp; }
				total;
			)";
			int64_t expected = 0;
			const int64_t chunks = (300 + 15) / 16;   // the reduce's chunk count at 300 elements: one init per chunk
			expected += 10 * chunks + 299 * 300 / 2;
			for (int64_t i = 0; i < 300; ++i) { expected += (10 + i) + (10 + (10 + i) + i); }
			for (bool vm : {false, true}) {
				for (size_t workers : {size_t(1), size_t(8)}) {
					auto e = make_engine();
					jai::stdlib::register_all(*e);
					e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					bind(*e);
					e->parallel_thread_count(workers);
					e->execution_budget(0);
					std::string got;
					try { got = std::to_string(e->execute(program).as_int()); } catch (const std::exception& ex) { got = std::string("error ") + ex.what(); }
					check_eq(std::to_string(expected), got, std::string("element receivers ") + (vm ? "vm " : "interp ") + std::to_string(workers));
				}
			}
			struct refusal { const char* name; const char* source; const char* text; };
			const refusal refusals[] = {
				{ "the non-const overload of a marked name", "array<Unit> us = [Unit(), Unit()]; parallel_for (auto& u : us) { u.level(3); }",
				  "C++ method 'level' of 'Unit' is marked jai::serial and cannot be called in a parallel body" },
				{ "a const overload marked every", "array<Unit> us = [Unit(), Unit()]; array<int> out = [0, 0]; parallel_for (auto& u, auto& o : us, out) { o = u.label(); }",
				  "C++ method 'label' of 'Unit' is marked jai::serial and cannot be called in a parallel body" },
				{ "a const lambda marked serial", "array<Unit> us = [Unit(), Unit()]; array<int> out = [0, 0]; parallel_for (auto& u, auto& o : us, out) { o = u.tag(); }",
				  "C++ method 'tag' of 'Unit' is marked jai::serial and cannot be called in a parallel body" },
				{ "one object at two elements", "shared_ptr<Unit> u = new Unit(); array<shared_ptr<Unit>> us = [u, u]; parallel_for (auto& x : us) { x.update(1); }",
				  "parallel_for: elements 0 and 1 hold the same object" },
			};
			for (const refusal& r : refusals) {
				for (bool vm : {false, true}) {
					auto e = make_engine();
					jai::stdlib::register_all(*e);
					e->set_backend(vm ? jai::backend_type::vm : jai::backend_type::interpreter);
					bind(*e);
					e->parallel_thread_count(8);
					std::string got = "no error";
					try { e->execute(r.source); } catch (const std::exception& ex) { got = ex.what(); }
					check_true(got.rfind(r.text, 0) == 0, std::string(r.name) + (vm ? " vm" : " interp") + ": expected '" + r.text + "', got '" + got + "'");
				}
			}
		});

		test("own_class_method_call_mutates_its_element", [this]() {
			auto e = make_engine();
			auto r2 = e->execute(R"(
				class Cell2 { int v = 0; int doubled() { return v * 2; } }
				var a2 = [];
				for (var i = 0; i < 8; i++) { auto c = Cell2(); c.v = i; a2.push(c); }
				parallel_for (auto& c : a2) { c.v = c.doubled(); }
				return a2[3].v;
			)");
			check_eq((int64_t)6, r2.as_int());
		});

		test("determinism_across_worker_counts", [this]() {
			// Byte-identical results at 1/2/4/8 workers from identical initial state
			std::string first;
			for (size_t workers : {1, 2, 4, 8}) {
				auto e = make_engine();
				jai::stdlib::register_all(e);
				e->parallel_thread_count(workers);
				auto r = e->execute(R"(
					var a = [];
					for (var i = 0; i < 300; i++) { a.push([i, 0.0]); }
					parallel_for (auto& p : a) { p[1] = math::sqrt(1.0 * p[0]) * 3.7; }
					return to_json(a);
				)");
				if (first.empty()) { first = r.as<std::string>(); }
				else { check_eq(first, r.as<std::string>()); }
			}
		});

		test("second_region_reuses_slots_cleanly", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				var a = [];
				for (var i = 0; i < 64; i++) { a.push(i); }
				parallel_for (auto& x : a) { x = x + 1; }
				parallel_for (auto& x : a) { x = x * 2; }
				return a[63];
			)");
			check_eq((int64_t)128, r.as_int());
		});

		// === Object-kind typed arrays ride their slots (nodemote): the declared class is the
		// exclusivity proof, the node is never demoted ===

		test("typed_object_array_stays_typed_after_region", [this]() {
			auto e = make_engine();
			jai::stdlib::register_all(e);
			e->parallel_thread_count(4);
			e->execute(R"(
				class K { int a = 1; K() {} K(int v) { a = v; } }
				array<K> ks = [];
				for (int i = 0; i < 64; i++) { ks.push(K(i)); }
				parallel_for (auto& k : ks) { k.a = k.a * 10; }
				parallel_for (auto k : ks) { k.a = 999; }
			)");
			auto ks = e->execute("ks");
			check_true(ks.is_array());
			const script_array* node = ks.unchecked_array_node();
			check_true(node->is_typed(), "node keeps its object kind");
			check_true(node->kind() == script_array::kind_t::obj, "kind obj");
			check_eq((size_t)64, node->size());
			check_false(ks.checked_as_array().has_value(), "the array<var> view stays unavailable (not demoted)");
			auto r = e->execute(R"(
				var out = [ks[0].a, ks[7].a, ks[63].a, type_of(ks) + "/" + type_of(ks[63])];
				[[warn_mode]] try { ks.push(5); out.push("accepted"); } catch (err) { out.push(err); }
				[[warn_mode]] try { ks[0] = "s"; out.push("accepted"); } catch (err) { out.push(err); }
				ks.push(K(3));
				out.push(ks.size());
				return out;
			)");
			const auto& vals = r.as_array();
			check_eq((int64_t)0, vals[0].as_int());
			check_eq((int64_t)70, vals[1].as_int());
			check_eq((int64_t)630, vals[2].as_int());
			check_eq(std::string("array/K"), vals[3].as<std::string>());
			check_eq(std::string("Cannot push 'int' to array<K>"), vals[4].as<std::string>());
			check_eq(std::string("Cannot assign 'string' to element of type 'K'"), vals[5].as<std::string>());
			check_eq((int64_t)65, vals[6].as_int());
		});

		test("typed_object_by_value_copies_never_race", [this]() {
			// Workers clone the element (by-value binding) and destroy the copy: instance
			// registration on the shared class definition must serialize, both node kinds
			for (const char* decl : {"array<K> ks = [];", "var ks = [];"}) {
				auto e = make_engine();
				e->execution_budget(0);
				e->parallel_thread_count(8);
				auto r = e->execute(std::string(R"(
					class K { int a = 1; string s = "x"; K() {} K(int v) { a = v; } }
				)") + decl + R"(
					for (int i = 0; i < 4000; i++) { ks.push(K(i)); }
					int rounds = 0;
					for (int r = 0; r < 5; r++) { parallel_for (auto k : ks) { k.a = 999; k.s = "y"; } rounds = rounds + 1; }
					return [rounds, ks[0].a, ks[3999].a, ks[17].s];
				)");
				const auto& vals = r.as_array();
				check_eq((int64_t)5, vals[0].as_int());
				check_eq((int64_t)0, vals[1].as_int());
				check_eq((int64_t)3999, vals[2].as_int());
				check_eq(std::string("x"), vals[3].as<std::string>());
			}
		});

		test("body_control_flow_continue_return_break", [this]() {
			// A body runs once per element: a top-level continue means the next element on both
			// backends, return does the same, a loop-level break is a parse error; nested loops and
			// switches keep their own break/continue
			auto e = make_engine();
			auto r = e->execute(R"(
				class C { int n = 0; }
				function mk(int k) -> array<C> { array<C> a = []; for (int i = 0; i < k; i++) { a.push(C()); } return a; }
				function sum(array<C>& a) -> int { int s = 0; for (auto& c : a) { s = s + c.n; } return s; }
				array<C> a1 = mk(100); parallel_for (auto& c : a1) { c.n = 1; continue; c.n = 5; }
				array<C> a2 = mk(100); parallel_for (auto& c : a2) { c.n = 1; return; c.n = 5; }
				array<C> a3 = mk(100); parallel_for (auto& c : a3) { for (int i = 0; i < 3; i++) { if (i == 1) { break; } c.n = c.n + 1; } }
				array<C> a4 = mk(100); parallel_for (auto& c : a4) { for (int i = 0; i < 3; i++) { if (i == 1) { continue; } c.n = c.n + 1; } }
				array<C> a5 = mk(100); parallel_for (auto& c : a5) { switch (c.n) { case 0: c.n = 1; continue; default: c.n = 9; } c.n = 5; }
				array<C> a6 = mk(100); parallel_for (auto& c : a6) { switch (c.n) { case 0: c.n = 1; break; default: c.n = 9; } c.n = c.n + 10; }
				array<C> a7 = mk(100); parallel_for (auto& c : a7) { if (c.n == 0) { continue; } c.n = 5; }
				return [sum(a1), sum(a2), sum(a3), sum(a4), sum(a5), sum(a6), sum(a7)];
			)");
			const auto& v = r.as_array();
			check_eq((int64_t)100, v[0].as_int());
			check_eq((int64_t)100, v[1].as_int());
			check_eq((int64_t)100, v[2].as_int());
			check_eq((int64_t)200, v[3].as_int());
			check_eq((int64_t)100, v[4].as_int());
			check_eq((int64_t)1100, v[5].as_int());
			check_eq((int64_t)0, v[6].as_int());
			std::string broke;
			try { e->execute("array<int> a8 = [1, 2];\nparallel_for (auto& c : a8) { c = 1; break; }"); } catch (const std::exception& err) { broke = err.what(); }
			check_true(broke.find("2:38: error: 'break' is not allowed in a parallel_for body: iterations are independent; use 'continue' for the next element") != std::string::npos, broke);
		});

		test("chunk_loop_modes_nested_return_and_typed_by_value", [this]() {
			// Inline mode grafts the body into the worker's chunk loop (zero calls per element);
			// a body that returns from inside a NESTED loop keeps its meaning through call mode
			// (one call per element). Both must read the same on both backends and match serial.
			auto e = make_engine();
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				class C { int n = 0; }
				function mk(int k) -> array<C> { array<C> a = []; for (int i = 0; i < k; i++) { a.push(C()); } return a; }
				function sum(array<C>& a) -> int { int s = 0; for (auto& c : a) { s = s + c.n; } return s; }
				array<C> a1 = mk(300);
				parallel_for (auto& c : a1) { for (int i = 0; i < 3; i++) { if (i == 1) { return; } c.n = c.n + 1; } c.n = c.n + 100; }
				array<C> a2 = mk(300);
				parallel_for (auto& c : a2) { int k = 0; while (true) { k = k + 1; if (k > 2) { return 5; } } c.n = 99; }
				array<C> a3 = mk(300);
				parallel_for (auto& c : a3) { c.n = 7; if (c.n == 7) { return; } c.n = 99; }
				array<int> ints = [];
				for (int i = 0; i < 300; i++) { ints.push(i); }
				parallel_for (int v : ints) { v = v + 1; }
				array<string> names = ["a", "b", "c"];
				parallel_for (string s : names) { s = s + "!"; }
				return [sum(a1), sum(a2), sum(a3), ints[5], ints[299], names[0]];
			)");
			const auto& v = r.as_array();
			check_eq((int64_t)300, v[0].as_int());
			check_eq((int64_t)0, v[1].as_int());
			check_eq((int64_t)2100, v[2].as_int());
			check_eq((int64_t)5, v[3].as_int());
			check_eq((int64_t)299, v[4].as_int());
			check_eq(std::string("a"), v[5].as<std::string>());
		});

		test("zip_parallel_for_writes_preallocated_structs_in_place", [this]() {
			// The emitter pattern: `parallel_for (auto& v, auto& o : vecs, outs)` walks the arrays in
			// lockstep, thread n owning segment n of BOTH; the body writes the pre-allocated output
			// struct's fields in place (no construction, no marshalling). Same values at 1/2/8
			// workers and as the serial loop; by-value inputs, a float output array, three arrays.
			// (Whole-struct assignment from a constructing call waits on constructor admission.)
			const std::string src = R"(
				class Vec3 { float x = 0.0; float y = 0.0; float z = 0.0; Vec3() {} Vec3(float a) { x = a; y = a * 2; z = a * 3; } }
				array<Vec3> vecs = []; array<Vec3> outs = []; array<float> sums = []; array<Vec3> outs2 = [];
				for (int i = 0; i < 1000; i++) { vecs.push(Vec3(i)); outs.push(Vec3()); sums.push(0.0); outs2.push(Vec3()); }
				parallel_for (auto& v, auto& o : vecs, outs) { o.x = v.x * v.y; o.y = v.z; o.z = o.x + o.y; }
				parallel_for (Vec3 v, auto& s, auto& o2 : vecs, sums, outs2) { s = v.x + v.y + v.z; o2.x = v.x * 10.0; o2.y = o2.x * 2.0; }
				float acc = 0.0;
				for (auto& o : outs) { acc = acc + o.z; }
				return [outs[7].x, outs[7].y, outs[7].z, sums[7], outs2[7].x, outs2[999].y, acc, vecs[7].x];
			)";
			std::vector<double> serial;
			for (size_t workers : {1, 2, 8}) {
				auto e = make_engine();
				e->execution_budget(0);
				e->parallel_thread_count(workers);
				auto r = e->execute(src);
				const auto& v = r.as_array();
				check_near(98.0, v[0].as_float(), 1e-9);
				check_near(21.0, v[1].as_float(), 1e-9);
				check_near(119.0, v[2].as_float(), 1e-9);
				check_near(42.0, v[3].as_float(), 1e-9);
				check_near(70.0, v[4].as_float(), 1e-9);
				check_near(999.0 * 10.0 * 2.0, v[5].as_float(), 1e-9);
				check_near(7.0, v[7].as_float(), 1e-9);
				if (serial.empty()) { serial.push_back(v[6].as_float()); } else { check_near(serial[0], v[6].as_float(), 1e-9); }
			}
			auto e = make_engine();
			auto r = e->execute(R"(
				class Vec3 { float x = 0.0; float y = 0.0; float z = 0.0; Vec3() {} Vec3(float a) { x = a; y = a * 2; z = a * 3; } }
				array<Vec3> vecs = []; array<Vec3> outs = [];
				for (int i = 0; i < 1000; i++) { vecs.push(Vec3(i)); outs.push(Vec3()); }
				for (int i = 0; i < 1000; i++) { outs[i].x = vecs[i].x * vecs[i].y; outs[i].y = vecs[i].z; outs[i].z = outs[i].x + outs[i].y; }
				float acc = 0.0;
				for (auto& o : outs) { acc = acc + o.z; }
				return acc;
			)");
			check_near(serial[0], r.as_float(), 1e-9);
		});

		test("zip_parallel_for_rejects_mismatch_and_self", [this]() {
			auto e = make_engine();
			auto r = e->execute(R"(
				array<float> a = [1.0, 2.0, 3.0]; array<float> b = [1.0]; array<float> c = [0.0, 0.0, 0.0];
				string e1 = ""; string e2 = ""; string e3 = "";
				try { parallel_for (auto& x, auto& y : a, b) { y = x; } } catch (err) { e1 = err; }
				try { parallel_for (auto& x, auto& y : a, a) { y = x; } } catch (err) { e2 = err; }
				try { parallel_for (auto& x, auto& y : a, 5) { y = x; } } catch (err) { e3 = err; }
				parallel_for (auto& x, auto& y : a, c) { y = x * 2.0; }
				return [e1, e2, e3, c[2]];
			)");
			const auto& v = r.as_array();
			check_eq(std::string("parallel_for: zipped array 2 has 1 elements, the range has 3 (zipped arrays must have the same length)"), v[0].as<std::string>());
			check_eq(std::string("parallel_for: zipped array 2 is the range itself"), v[1].as<std::string>());
			check_eq(std::string("parallel_for: zipped array 2 is not an array"), v[2].as<std::string>());
			check_near(6.0, v[3].as_float(), 1e-9);
		});

		test("by_ref_cursor_reference_and_typed_view", [this]() {
			// The by-ref loop variable is ONE holder re-pointed per iteration (frame-slot loops):
			// a retained alias keeps its element (the loop mints a fresh holder instead), a slot
			// replaced mid-loop is seen through the reference on the next touch (typed view
			// refresh), workers re-point their chunk cursor the same way; typed and boxed arrays
			for (const char* decl : {"array<C> a = [];", "var a = [];"}) {
				auto e = make_engine();
				e->parallel_thread_count(4);
				auto r = e->execute(std::string(R"(
					class C { int n = 0; string s = "s"; C() {} C(int v) { n = v; } }
					var keep = null;
					function run() -> array<var> {
				)") + decl + R"(
						for (int i = 0; i < 200; i++) { a.push(C(i)); }
						int first_seen = -1;
						bool got = false;
						for (auto& c : a) {
							if (!got) { keep = [&]() { return c.n; }; got = true; }
							if (c.n == 3) { a[3] = C(300); c.n = c.n + 1; }
							c.s = c.s + "!";
						}
						int kept = keep();
						a[3].n = a[3].n + 1;
						int kept2 = keep();
						parallel_for (auto& c : a) { c.n = c.n * 2; }
						int total = 0; string tail = "";
						for (auto& c : a) { total = total + c.n; }
						for (auto& c : a) { tail = c.s; }
						return [kept, kept2, a[3].n, a[5].n, total, tail];
					}
					return run();
				)");
				const auto& v = r.as_array();
				check_eq((int64_t)0, v[0].as_int());
				check_eq((int64_t)0, v[1].as_int());
				check_eq((int64_t)604, v[2].as_int());
				check_eq((int64_t)10, v[3].as_int());
				check_eq((int64_t)40398, v[4].as_int());   // pinned on the pre-cursor runner, both backends, both kinds
				check_eq(std::string("s!"), v[5].as<std::string>());
			}
		});

		test("typed_object_live_reference_pins_current_behavior", [this]() {
			// A field reference minted THROUGH an element (auto& r = ks[3].a) counts as a live
			// reference into the array: the fork takes the per-element walk, which detaches the
			// pinned instance - the reference keeps the pre-region value, the element moves on.
			// A direct element reference re-resolves through container+index: coherent.
			auto e = make_engine();
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				class K { int a = 1; K() {} K(int v) { a = v; } }
				array<K> ks = [];
				for (int i = 0; i < 64; i++) { ks.push(K(i)); }
				auto& r = ks[3].a;
				auto& el = ks[4];
				parallel_for (auto& k : ks) { k.a = k.a + 1000; }
				var out = [r, ks[3].a, el.a, ks[4].a];
				r = -1;
				out.push(ks[3].a);
				return out;
			)");
			const auto& vals = r.as_array();
			check_eq((int64_t)3, vals[0].as_int(), "field ref detached from the element");
			check_eq((int64_t)1003, vals[1].as_int());
			check_eq((int64_t)1004, vals[2].as_int(), "element ref stays coherent");
			check_eq((int64_t)1004, vals[3].as_int());
			check_eq((int64_t)1003, vals[4].as_int(), "writes through the detached ref never reach the array");
		});

		test("typed_object_live_reference_then_released", [this]() {
			// The reference dies with its scope: the next fork proves from the declared type again
			auto e = make_engine();
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				class K { int a = 1; K() {} K(int v) { a = v; } }
				array<K> ks = [];
				for (int i = 0; i < 64; i++) { ks.push(K(i)); }
				void poke(int& x) { x = x + 5; }
				poke(ks[2].a);
				parallel_for (auto& k : ks) { k.a = k.a * 2; }
				return [ks[2].a, ks[63].a];
			)");
			const auto& vals = r.as_array();
			check_eq((int64_t)14, vals[0].as_int());
			check_eq((int64_t)126, vals[1].as_int());
			check_eq((uint32_t)0, e->execute("ks").unchecked_array_node()->live_elem_refs);
		});

		test("typed_object_empty_and_single_element", [this]() {
			auto e = make_engine();
			jai::stdlib::register_all(e);
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				class K { int a = 1; K() {} K(int v) { a = v; } }
				array<K> none = [];
				parallel_for (auto& k : none) { k.a = 5; }
				array<K> one = [K(9)];
				parallel_for (auto& k : one) { k.a = k.a + 1; }
				parallel_for (auto k : one) { k.a = 77; }
				return [none.size(), one.size(), one[0].a, type_of(one[0])];
			)");
			const auto& vals = r.as_array();
			check_eq((int64_t)0, vals[0].as_int());
			check_eq((int64_t)1, vals[1].as_int());
			check_eq((int64_t)10, vals[2].as_int());
			check_eq(std::string("K"), vals[3].as<std::string>());
		});

		test("typed_object_string_field_named_by_body_detaches", [this]() {
			// Every instance's field initializer shares one literal node; a body that names the
			// string field forces the per-element detach (the one fact a declared type cannot
			// prove) - mutation stays per element, siblings and outsiders keep the literal
			auto e = make_engine();
			jai::stdlib::register_all(e);
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				class S { int i = 0; string s = "lit"; S() {} S(int v) { i = v; } }
				array<S> ss = [];
				for (int k = 0; k < 64; k++) { ss.push(S(k)); }
				S outside = S(3);
				parallel_for (auto& x : ss) { x.s = x.s + to_string(x.i); }
				return [ss[0].s, ss[63].s, outside.s, ss[5].s];
			)");
			const auto& vals = r.as_array();
			check_eq(std::string("lit0"), vals[0].as<std::string>());
			check_eq(std::string("lit63"), vals[1].as<std::string>());
			check_eq(std::string("lit"), vals[2].as<std::string>());
			check_eq(std::string("lit5"), vals[3].as<std::string>());
		});

		test("typed_object_whole_read_of_nested_object_detaches_its_strings", [this]() {
			// Copying a nested object by value copies every handle below it: the reach filter
			// must open the whole subtree (a shared literal string would race otherwise)
			auto e = make_engine();
			jai::stdlib::register_all(e);
			e->execution_budget(0);
			e->parallel_thread_count(8);
			auto r = e->execute(R"(
				class Leaf { int v = 0; string tag = "leaf"; Leaf() {} Leaf(int n) { v = n; } }
				class Node { Leaf a = Leaf(0); Node() {} Node(int n) { a = Leaf(n); } }
				array<Node> nodes = [];
				for (int i = 0; i < 2000; i++) { nodes.push(Node(i)); }
				for (int r = 0; r < 4; r++) {
					parallel_for (auto& n : nodes) { auto l = n.a; l.tag = l.tag + "!"; n.a.v = l.v + 1; }
				}
				return [nodes[0].a.v, nodes[1999].a.v, nodes[7].a.tag];
			)");
			const auto& vals = r.as_array();
			check_eq((int64_t)4, vals[0].as_int());
			check_eq((int64_t)2003, vals[1].as_int());
			check_eq(std::string("leaf"), vals[2].as<std::string>());
		});

		test("typed_object_admitted_method_call", [this]() {
			auto e = make_engine();
			e->parallel_thread_count(4);
			auto r = e->execute(R"(
				class Cell { int v = 0; Cell() {} Cell(int n) { v = n; } int doubled() { return v * 2; } }
				array<Cell> a = [];
				for (int i = 0; i < 32; i++) { a.push(Cell(i)); }
				parallel_for (auto& c : a) { c.v = c.doubled(); }
				return a[3].v * 1000 + a[31].v;
			)");
			check_eq((int64_t)6062, r.as_int());
		});

		test("prewarm_class_reached_through_typed_array_field", [this]() {
			// n.extras[0].v: Leaf is reached only through an array<Leaf> field - its shapes (field
			// symbols, the Leaf& twin) must be warm before the first worker touch. Fresh engine per
			// worker count so the first region is cold; leaf/xs siblings pin the shapes that worked
			const std::string src = R"(
				class Leaf { int v = 0; }
				class Node { Leaf leaf = Leaf(); array<int> xs = [0]; array<Leaf> extras = [Leaf(), Leaf()]; }
				array<Node> nodes = [];
				array<Node> serial = [];
				for (int i = 0; i < 64; i++) { nodes.push(Node()); serial.push(Node()); }
				parallel_for (auto& n : nodes) { n.extras[0].v = n.extras[0].v + 1; n.leaf.v = n.leaf.v + 2; n.xs[0] = n.xs[0] + 3; n.extras[1].v = n.extras[0].v * 5; }
				for (auto& n : serial) { n.extras[0].v = n.extras[0].v + 1; n.leaf.v = n.leaf.v + 2; n.xs[0] = n.xs[0] + 3; n.extras[1].v = n.extras[0].v * 5; }
				var p = []; var q = [];
				for (auto n : nodes) { p.push([n.extras[0].v, n.extras[1].v, n.leaf.v, n.xs[0]]); }
				for (auto n : serial) { q.push([n.extras[0].v, n.extras[1].v, n.leaf.v, n.xs[0]]); }
				return [p, q];
			)";
			for (size_t workers : {size_t(1), size_t(2), size_t(8)}) {
				auto e = make_engine();
				check_matches_serial_at(e, src, {workers});
				auto r = e->execute(src);
				const auto& row = r.as_array()[0].as_array()[5].as_array();
				check_eq((int64_t)1, row[0].as_int());
				check_eq((int64_t)5, row[1].as_int());
			}
		});

		test("prewarm_two_container_levels", [this]() {
			// array<Node> -> array<Mid> -> array<Leaf> -> field: every class below the element is
			// reached through a typed container only
			const std::string src = R"(
				class Leaf { int v = 1; }
				class Mid { array<Leaf> leaves = [Leaf(), Leaf(), Leaf()]; }
				class Node { array<Mid> mids = [Mid(), Mid()]; }
				array<Node> nodes = [];
				array<Node> serial = [];
				for (int i = 0; i < 64; i++) { nodes.push(Node()); serial.push(Node()); }
				parallel_for (auto& n : nodes) { n.mids[1].leaves[2].v = n.mids[1].leaves[2].v + n.mids[0].leaves[0].v * 4; }
				for (auto& n : serial) { n.mids[1].leaves[2].v = n.mids[1].leaves[2].v + n.mids[0].leaves[0].v * 4; }
				var p = []; var q = [];
				for (auto n : nodes) { p.push([n.mids[1].leaves[2].v, n.mids[0].leaves[0].v]); }
				for (auto n : serial) { q.push([n.mids[1].leaves[2].v, n.mids[0].leaves[0].v]); }
				return [p, q];
			)";
			for (size_t workers : {size_t(1), size_t(2), size_t(8)}) {
				auto e = make_engine();
				check_matches_serial_at(e, src, {workers});
				check_eq((int64_t)5, e->execute(src).as_array()[0].as_array()[9].as_array()[0].as_int());
			}
		});

		test("prewarm_map_of_typed_arrays_field", [this]() {
			// map<string, array<Leaf>>: the class sits below a map value type's element type
			const std::string src = R"(
				class Leaf { int v = 2; }
				class Node { map<string, array<Leaf>> groups = {"a": [Leaf()], "b": [Leaf(), Leaf()]}; }
				array<Node> nodes = [];
				array<Node> serial = [];
				for (int i = 0; i < 64; i++) { nodes.push(Node()); serial.push(Node()); }
				parallel_for (auto& n : nodes) { n.groups["b"][1].v = n.groups["b"][1].v + n.groups["a"][0].v * 3; }
				for (auto& n : serial) { n.groups["b"][1].v = n.groups["b"][1].v + n.groups["a"][0].v * 3; }
				var p = []; var q = [];
				for (auto n : nodes) { p.push([n.groups["b"][1].v, n.groups["a"][0].v]); }
				for (auto n : serial) { q.push([n.groups["b"][1].v, n.groups["a"][0].v]); }
				return [p, q];
			)";
			for (size_t workers : {size_t(1), size_t(2), size_t(8)}) {
				auto e = make_engine();
				check_matches_serial_at(e, src, {workers});
				check_eq((int64_t)8, e->execute(src).as_array()[0].as_array()[3].as_array()[0].as_int());
			}
		});

		test("member_path_string_subscript_reads_on_workers", [this]() {
			// a member path's general walk pushes its constant key as a fresh string, never the chunk's shared literal (the XTHREAD
			// canary build aborts at the first worker touch of the literal's count; plain builds tore it ~30% of runs)
			const std::string src = R"(
				class Leaf { int v = 1; }
				class Node { map<string, array<Leaf>> groups = {"a": [Leaf()], "b": [Leaf(), Leaf()]}; }
				array<Node> nodes = [];
				for (int i = 0; i < 512; i++) { nodes.push(Node()); }
				for (int r = 0; r < 8; r++) {
					parallel_for (auto& n : nodes) { n.groups["b"][1].v = n.groups["b"][1].v + n.groups["a"][0].v; }
				}
				return nodes[511].groups["b"][1].v;
			)";
			for (int round = 0; round < 8; ++round) {
				auto e = make_engine();
				e->parallel_thread_count(8);
				check_eq((int64_t)9, e->execute(src).as_int());
			}
		});

		test("prewarm_reaches_nested_classes_without_the_element_proof", [this]() {
			// An untyped source never scans the slot tags for element classes: the classes the body reaches must come from the
			// seed shapes and the members the body names
			const std::string typed = R"(
				class Leaf { int v = 0; }
				class Mid { array<Leaf> leaves = [Leaf()]; }
				class Node { array<Leaf> extras = [Leaf()]; array<Mid> mids = [Mid()]; map<string, array<Leaf>> groups = {"a": [Leaf()]}; }
				array<Node> nodes = [];
				for (int i = 0; i < 64; i++) { nodes.push(Node()); }
				parallel_for (auto& n : nodes) { n.extras[0].v = n.extras[0].v + 1; n.mids[0].leaves[0].v = n.mids[0].leaves[0].v + 2; n.groups["a"][0].v = n.groups["a"][0].v + 3; }
				return [nodes[0].extras[0].v, nodes[63].mids[0].leaves[0].v, nodes[31].groups["a"][0].v];
			)";
			const std::string hetero = R"(
				class Leaf { int v = 0; }
				class Mid { array<Leaf> leaves = [Leaf()]; }
				class Node { array<Leaf> extras = [Leaf()]; array<Mid> mids = [Mid()]; map<string, array<Leaf>> groups = {"a": [Leaf()]}; }
				var nodes = [];
				for (int i = 0; i < 64; i++) { nodes.push(Node()); }
				parallel_for (auto& n : nodes) { n.extras[0].v = n.extras[0].v + 1; n.mids[0].leaves[0].v = n.mids[0].leaves[0].v + 2; n.groups["a"][0].v = n.groups["a"][0].v + 3; }
				return [nodes[0].extras[0].v, nodes[63].mids[0].leaves[0].v, nodes[31].groups["a"][0].v];
			)";
			for (const std::string* src : {&typed, &hetero}) {
				auto e = make_engine();
				e->parallel_thread_count(4);
				auto r = e->execute(*src);
				const auto& vals = r.as_array();
				check_eq((int64_t)1, vals[0].as_int());
				check_eq((int64_t)2, vals[1].as_int());
				check_eq((int64_t)3, vals[2].as_int());
			}
		});

		test("prewarm_covers_var_and_auto_copies_of_handles_and_typed_containers", [this]() {
			// a var copy of a handle takes its rebindable twin, of a typed container its dynamic twin, auto takes the plain
			// spelling back and lock() a shared_ptr: every shape is interned at the barrier, never by a worker
			const std::string prelude = R"(
				class C { int x = 0; }
				array<shared_ptr<C>> hs = [];
				array<weak_ptr<C>> ws = [];
				array<array<int>> grid = [];
				array<map<string, int>> ms = [];
				for (int i = 0; i < 64; i++) { shared_ptr<C> h = new C(); hs.push(h); ws.push(h); grid.push([i]); ms.push({"a": i}); }
			)";
			const std::string tally = R"(
				int total = 0;
				for (auto h : hs) { total += h.x; }
				return total;
			)";
			const std::vector<std::pair<std::string, int64_t>> bodies {
				{"parallel_for (var p : hs) { var u = p; u.x = u.x + 1; }", 64},
				{"parallel_for (auto p : hs) { var u = p; auto v = u; v.x = v.x + 2; }", 128},
				{"parallel_for (var w : ws) { var l = w.lock(); l.x = l.x + 3; }", 192},   // distinct objects: each weak element's lock() is its own
				{"parallel_for (var r : grid) { var c = r; int n = c.size(); }", 0},
				{"parallel_for (var m : ms) { var c = m; int n = c[\"a\"]; }", 0},
			};
			for (const auto& [body, expected] : bodies) {
				auto e = make_engine();
				e->parallel_thread_count(4);
				auto r = e->execute(prelude + body + tally);
				check_eq(expected, r.as_int(), body);
			}
		});

		test("typed_object_determinism_across_worker_counts", [this]() {
			std::string first;
			for (size_t workers : {1, 2, 4, 8}) {
				auto e = make_engine();
				jai::stdlib::register_all(e);
				e->parallel_thread_count(workers);
				auto r = e->execute(R"(
					class P { float x = 0.0; float y = 0.0; P() {} P(int i) { x = 1.0 * i; y = 0.5 * i; } }
					array<P> ps = [];
					for (int i = 0; i < 300; i++) { ps.push(P(i)); }
					parallel_for (auto& p : ps) { p.y = math::sqrt(p.x) * 3.7 + p.y; }
					var out = [];
					for (auto p : ps) { out.push(p.y); }
					return to_json(out);
				)");
				if (first.empty()) { first = r.as<std::string>(); }
				else { check_eq(first, r.as<std::string>()); }
			}
		});
	}
};

// These hooks model exceptions at named region/pool stages. They do not replace
// the allocator or claim coverage of every nested allocation inside the STL.
class parallel_region_fault_tests : public suite {
public:
	parallel_region_fault_tests() : suite("Parallel Region Faults") {}

	struct fault_trace {
		std::thread::id main_thread = std::this_thread::get_id();
		std::vector<std::pair<std::string, size_t>> visits;
		size_t fail_ordinal = SIZE_MAX;
		const char* fail_point = nullptr;
		bool fired = false;

		static void visit(void* context, const char* point, size_t index) {
			auto& trace = *static_cast<fault_trace*>(context);
			// Worker checkpoints have their own pool/worker tests. This sweep's
			// ordinal is deterministic and its trace needs no shared mutable state.
			if (std::this_thread::get_id() != trace.main_thread) { return; }
			const size_t ordinal = trace.visits.size();
			trace.visits.emplace_back(point, index);
			if (!trace.fired && (ordinal == trace.fail_ordinal ||
			    (trace.fail_point && std::strcmp(point, trace.fail_point) == 0))) {
				trace.fired = true;
				throw std::bad_alloc();
			}
		}
	};

	struct fixture {
		std::shared_ptr<engine> eng;
		jai::jaibite call;
		const script_array* input = nullptr;
	};

	// 0: transform, 1: in-place statement, 2: class-valued transform. The third
	// shape reaches deferred registration, which primitive results cannot cover.
	// 32 elements reach every stage 256 do; each element adds two registration stages to a sweep.
	static fixture prepare(int shape, bool warm) {
		fixture result;
		result.eng = make_engine();
		result.eng->execution_budget(0);
		result.eng->parallel_thread_count(4);
		result.eng->execute(R"(
			int fault_bias = 7;
			array<int> fault_lookup = [3];
			array<string> fault_snapshot = ["a", "b"];
			string fault_label = "abc";
		)");
		if (shape == 2) {
			result.eng->execute(R"(
				class FaultCell { int value = 0; FaultCell() {} }
				array<FaultCell> fault_input = [];
				for (int i = 0; i < 32; i++) { fault_input.push(FaultCell()); }
				FaultCell fault_step(FaultCell x) {
					x.value = fault_bias + fault_lookup[0] + fault_snapshot.size() + fault_label.size();
					return x;
				}
			)");
			result.call = result.eng->jaibite(R"(
				var fault_output = parallel_transform(fault_input, fault_step);
				fault_output[31].value;
			)");
		} else {
			result.eng->execute(R"(
				var fault_input = [];
				for (int i = 0; i < 32; i++) { fault_input.push(i); }
				int fault_step(int x) {
					return x * 0 + fault_bias + fault_lookup[0] + fault_snapshot.size() + fault_label.size();
				}
			)");
			result.call = result.eng->jaibite(shape == 1 ? R"(
				parallel_for (auto& x : fault_input) {
					x = fault_bias + fault_lookup[0] + fault_snapshot.size() + fault_label.size();
				}
				fault_input[31];
			)" : R"(
				var fault_output = parallel_transform(fault_input, fault_step);
				fault_output[31];
			)");
		}
		result.input = result.eng->get_variable("fault_input").unchecked_array_node();
		if (warm) { check_eq(script_int(15), result.eng->execute(result.call).as_int()); }
		// Adaptive fan-out is a performance hint, not a semantic input. Remove the
		// measured cost from this test so fresh/warm ordinal traces use four slots.
		auto& state = detail::engine_access::parallel_state(*result.eng);
		state.worker_overhead_ns = 0.0;
		for (auto& pool : state.region_pools) { pool.ns_per_element = 0.0; }
		return result;
	}

	static void arm(engine& eng, fault_trace& trace) {
		auto& faults = detail::engine_access::parallel_state(eng).faults;
		faults.context = &trace;
		faults.callback = &fault_trace::visit;
	}

	static void disarm(engine& eng) {
		auto& faults = detail::engine_access::parallel_state(eng).faults;
		faults.callback = nullptr;
		faults.context = nullptr;
	}

	static bool execute_injected(fixture& fixture, fault_trace& trace) {
		arm(*fixture.eng, trace);
		bool failed = false;
		try { fixture.eng->execute(fixture.call); }
		catch (const std::exception&) { failed = true; }
		catch (...) {
			disarm(*fixture.eng);
			throw;
		}
		disarm(*fixture.eng);
		return failed;
	}

	static void check_recovered(fixture& fixture, const std::string& label) {
		auto& eng = *fixture.eng;
		check_false(detail::engine_access::parallel_region_active(eng), label + ": active region survived");
		check_false(detail::engine_access::parallel_state(eng).region_running, label + ": running flag survived");
		check_false(detail::engine_access::get_symbolizer(eng)->frozen(), label + ": symbolizer stayed frozen");
		check_eq(uint32_t(0), fixture.input->live_elem_refs, label + ": input cursor survived");
		check_eq(script_int(42), eng.execute("int after_injected_failure() { return 42; } after_injected_failure();").as_int(), label);
		check_eq(script_int(15), eng.execute(fixture.call).as_int(), label + ": following identical region");
		check_false(detail::engine_access::parallel_region_active(eng), label);
		check_false(detail::engine_access::get_symbolizer(eng)->frozen(), label);
		check_eq(uint32_t(0), fixture.input->live_elem_refs, label + ": following region retained input");
	}

	void forge_tests() override {
		test("task_storage_failure_restores_activation", [this]() {
			auto fixture = prepare(0, false);
			fault_trace trace;
			trace.fail_point = "region.tasks_reserve";
			const bool failed = execute_injected(fixture, trace);
			const bool inactive = !detail::engine_access::parallel_region_active(*fixture.eng);
			const bool unfrozen = !detail::engine_access::get_symbolizer(*fixture.eng)->frozen();
			const bool stopped = !detail::engine_access::parallel_state(*fixture.eng).region_running;
			// This particular point precedes every region submission. Repair only
			// after observing the flags so the test can fail safely on the old code.
			detail::engine_access::parallel_state(*fixture.eng).active_region = nullptr;
			detail::engine_access::parallel_state(*fixture.eng).region_running = false;
			detail::engine_access::get_symbolizer(*fixture.eng)->set_frozen(false);
			check_true(trace.fired, "region.tasks_reserve checkpoint was not reached");
			check_true(failed, "injected allocation failure did not reach the caller");
			check_true(inactive, "allocation failure left a dangling active-region table");
			check_true(unfrozen, "allocation failure left the symbolizer frozen");
			check_true(stopped, "allocation failure left the reentrancy flag set");
			check_recovered(fixture, "task storage");
		});

		test("building_the_worker_pool_is_not_charged_to_the_budget", [this]() {
			// an oversubscribed host can take seconds to start threads; the region's first pool build stands in for that here.
			// The caller's deadline moves later by the build's own time: read as the absolute deadline before and after the
			// region, the move covers the injected 200 ms however loaded the machine is, and a build left uncredited moves it 0
			auto eng = make_engine();
			eng->parallel_thread_count(4);
			eng->execution_budget(60.0);
			std::atomic<int> builds{0};
			auto& faults = detail::engine_access::parallel_state(*eng).faults;
			faults.context = &builds;
			faults.callback = [](void* context, const char* point, size_t) {
				if (std::strcmp(point, "region.pool_create") == 0) {
					static_cast<std::atomic<int>*>(context)->fetch_add(1);
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
				}
			};
			std::vector<std::chrono::steady_clock::time_point> deadlines;
			engine* raw_eng = eng.get();
			eng->add_function("deadline_mark", [raw_eng, &deadlines]() { deadlines.push_back(detail::engine_access::get_execution_backend(*raw_eng)->execution_deadline()); });
			eng->execute(R"(
				array<int> xs = [];
				for (int i = 0; i < 4096; i++) { xs.push(i); }
				deadline_mark();
				parallel_for (auto& x : xs) { x = x + 1; }
				deadline_mark();
			)");
			faults.callback = nullptr;
			faults.context = nullptr;
			check_eq(1, builds.load(), "the region built its pool once");
			check_eq((size_t)2, deadlines.size());
			check_true(deadlines[1] - deadlines[0] >= std::chrono::milliseconds(200), "the deadline moved later by the pool build's time");
		});

		test("worker_argument_failure_is_not_silently_swallowed", [this]() {
			auto fixture = prepare(0, false);
			auto call = fixture.eng->jaibite("parallel_transform(fault_input, fault_step).size();");
			std::atomic<bool> fired{false};
			auto& faults = detail::engine_access::parallel_state(*fixture.eng).faults;
			faults.context = &fired;
			faults.callback = [](void* context, const char* point, size_t index) {
				auto& fired = *static_cast<std::atomic<bool>*>(context);
				if (std::strcmp(point, "region.worker_arguments") == 0 && index > 0 && !fired.exchange(true)) {
					throw std::bad_alloc();
				}
			};
			bool failed = false;
			try { fixture.eng->execute(call); }
			catch (const std::exception&) { failed = true; }
			disarm(*fixture.eng);
			check_true(fired.load(), "worker argument allocation checkpoint was not reached");
			check_true(failed, "worker argument allocation failure was silently swallowed");
			check_recovered(fixture, "worker arguments");
		});

		test("an_exception_escaping_a_pool_worker_chunk_fails_the_region", [this]() {
			// A host exception the backends do not convert (no std::exception base) left the
			// pool worker's chunk; the pool swallowed it and the region returned as if it ran.
			// 256 elements over 4 threads deal blocks of 16: element 3 is the calling thread's
			// chunk, 20 a pool worker's, 50 and 70 a later worker's and the caller's again.
			struct host_failure { script_int at; };
			const std::pair<script_int, script_int> cases[] = { {3, -1}, {20, -1}, {20, 70}, {70, 20}, {50, 20} };
			for (const auto& [first, second] : cases) {
				auto eng = make_engine();
				eng->parallel_thread_count(4);
				eng->add_function("sign", [](script_int a_x, script_int a_first, script_int a_second) -> script_int {
					if (a_x == a_first || a_x == a_second) { throw host_failure{a_x}; }
					return 0;
				});
				eng->execute("array<int> xs = []; for (int i = 0; i < 256; i++) { xs.push(i); }");
				script_int thrown_at = -2;
				try {
					eng->execute("parallel_for (auto& x : xs) { x = x + sign(x, " + std::to_string(first) + ", " + std::to_string(second) + "); }");
				} catch (const host_failure& failure) {
					thrown_at = failure.at;
				}
				const script_int lowest = second < 0 ? first : std::min(first, second);
				check_eq(lowest, thrown_at, "the region raises the host exception of its first failing iteration");
				check_eq(script_int(256), eng->execute("parallel_for (auto& x : xs) { x = 1; } int s = 0; for (int i = 0; i < 256; i++) { s += xs[i]; } s;").as_int(),
					"the engine runs the next region normally");
			}
		});

		test("failed_provisioning_releases_worker_environment_cycles", [this]() {
			std::weak_ptr<int> lifetime_probe;
			{
				auto token = std::make_shared<int>(7);
				lifetime_probe = token;
				fixture fixture;
				fixture.eng = make_engine();
				fixture.eng->execution_budget(0);
				fixture.eng->parallel_thread_count(4);
				// Admission requires a whitelisted host name: a zero-argument sign overload local to this engine, the registered
				// callable the only remaining owner of the token
				fixture.eng->add_function("sign", [token]() -> script_int { return *token; });
				token.reset();
				check_false(lifetime_probe.expired(), "host callable did not retain its token");
				fixture.eng->execute(R"(
					array<int> lifetime_lookup = [1];
					var lifetime_input = [];
					for (int i = 0; i < 256; i++) { lifetime_input.push(i); }
					int lifetime_wrapper(int x) { return x + math::sign() + lifetime_lookup[0]; }
				)");
				fixture.call = fixture.eng->jaibite("parallel_transform(lifetime_input, lifetime_wrapper).size();");
				fault_trace trace;
				trace.fail_point = "region.prewarm";
				const bool failed = execute_injected(fixture, trace);
				check_true(trace.fired, "failure must follow worker provisioning and capture publication");
				check_true(failed, "prewarm failure did not reach the caller");
				check_false(detail::engine_access::parallel_region_active(*fixture.eng));
				check_false(detail::engine_access::parallel_state(*fixture.eng).region_running);
				check_false(detail::engine_access::get_symbolizer(*fixture.eng)->frozen());
				check_eq(uint32_t(0), fixture.eng->get_variable("lifetime_input").unchecked_array_node()->live_elem_refs);
				// No recovery call rebuilds the slots before this scope ends. Destroy
				// every engine, bite and local owner; only the weak probe survives.
			}
			check_true(lifetime_probe.expired(),
				"failed worker provisioning leaked a callable through its root-environment cycle");
		});

		test("every_main_thread_stage_recovers_fresh_and_reused_slots", [this]() {
			for (int shape : {0, 1, 2}) {
				for (bool warm : {false, true}) {
					auto successful = prepare(shape, warm);
					fault_trace trace;
					check_false(execute_injected(successful, trace), "checkpoint discovery failed");
					check_false(trace.visits.empty(), "no main-thread checkpoints recorded");
					const auto expected = trace.visits;
					bool saw_tasks = false;
					bool saw_capture = false;
					bool saw_registration = false;
					for (const auto& event : expected) {
						saw_tasks = saw_tasks || event.first == "region.tasks_reserve";
						saw_capture = saw_capture || event.first == "region.capture_publish";
						saw_registration = saw_registration || event.first == "region.instance_register";
					}
					check_true(saw_tasks, "task allocation checkpoint absent from trace");
					check_true(saw_capture, "capture publication checkpoint absent from trace");
					if (shape == 2) { check_true(saw_registration, "class output did not reach instance registration"); }

					for (size_t ordinal = 0; ordinal < expected.size(); ++ordinal) {
						auto fixture = prepare(shape, warm);
						fault_trace injected;
						injected.fail_ordinal = ordinal;
						const bool failed = execute_injected(fixture, injected);
						const auto& event = expected[ordinal];
						const std::string label = std::string(warm ? "warm " : "fresh ") +
							std::to_string(shape) + " " + event.first + "[" + std::to_string(event.second) +
							"] #" + std::to_string(ordinal);
						check_true(injected.fired, label + ": injection not reached");
						check_true(failed, label + ": exception was swallowed");
						check_eq(event.first, injected.visits[ordinal].first, label + ": checkpoint order drift");
						check_eq(event.second, injected.visits[ordinal].second, label + ": checkpoint index drift");
						check_recovered(fixture, label);
					}
				}
			}
		});

		test("a_type_shape_missed_by_the_barrier_fails_before_it_interns", [this]() {
			// A worker building a type the barrier did not pre-warm must fail before it inserts its name: the insert would move the
			// symbol tables the other workers read
			auto e = make_engine();
			type_info* nested = detail::engine_access::get_type_info_int(*e);
			for (int depth = 0; depth < 4; ++depth) { nested = detail::engine_access::get_type_info_array(*e, nested); }
			auto* symbols = detail::engine_access::get_symbolizer(*e);
			const size_t interned = symbols->size();
			symbols->set_frozen(true);
			std::vector<std::string> errors;
			const auto attempt = [&](const auto& build) {
				try { build(); errors.push_back("no error"); } catch (const std::exception& ex) { errors.push_back(ex.what()); }
			};
			attempt([&]() { (void)detail::engine_access::get_type_info_array(*e, nested); });
			attempt([&]() { (void)detail::engine_access::get_type_info_map(*e, nested, nested); });
			attempt([&]() { (void)detail::engine_access::get_type_info_weak_ptr(*e, nested); });
			attempt([&]() { (void)detail::engine_access::get_type_info_function(*e, nested, {nested, nested}); });
			const size_t after = symbols->size();
			symbols->set_frozen(false);
			for (const std::string& error : errors) {
				check_eq(std::string("parallel_transform: type shape was not pre-warmed at the region barrier (internal) - please report this body shape"), error);
			}
			check_eq(interned, after, "nothing was interned while frozen");
			check_true(detail::engine_access::get_type_info_array(*e, nested) != nullptr, "the shape interns once the region is over");
		});

		test("a_name_missed_by_the_barrier_throws_instead_of_interning", [this]() {
			// A frozen symbolizer only answers: a miss throws in every build (an insert would move the tables the other workers
			// read), and the accessor caches are read, never filled, while frozen
			auto e = make_engine();
			auto* symbols = detail::engine_access::get_symbolizer(*e);
			const uint64_t member = symbols->intern("frozen_probe_member");
			(void)symbols->intern("_get_frozen_probe_member");
			const size_t interned = symbols->size();
			symbols->set_frozen(true);
			std::vector<std::string> errors;
			const auto attempt = [&](const auto& run) {
				try { run(); errors.push_back("no error"); } catch (const std::exception& ex) { errors.push_back(ex.what()); }
			};
			attempt([&]() { (void)symbols->intern("frozen_probe_never_seen"); });
			attempt([&]() { (void)symbols->intern_with_view("frozen_probe_never_seen"); });
			attempt([&]() { (void)symbols->get_setter_id_with_view(member); });
			const auto getter = symbols->get_getter_id_with_view(member);
			const size_t after = symbols->size();
			symbols->set_frozen(false);
			const std::string missed = "parallel_transform: name 'frozen_probe_never_seen' was not pre-warmed at the region barrier (internal) - please report this body shape";
			check_eq(missed, errors[0]);
			check_eq(missed, errors[1]);
			check_eq(std::string("parallel_transform: name '_set_frozen_probe_member' was not pre-warmed at the region barrier (internal) - please report this body shape"), errors[2]);
			check_eq(std::string("_get_frozen_probe_member"), std::string(getter.second), "an interned accessor name answers while frozen");
			check_eq(interned, after, "nothing was interned while frozen");
			check_eq(getter.first, symbols->get_getter_id_with_view(member).first, "and caches once thawed");
		});

		test("activation_restores_an_already_frozen_symbolizer", [this]() {
			for (bool inject : {false, true}) {
				auto fixture = prepare(0, true);
				fault_trace trace;
				if (inject) { trace.fail_point = "region.tasks_reserve"; }
				detail::engine_access::get_symbolizer(*fixture.eng)->set_frozen(true);
				const bool failed = execute_injected(fixture, trace);
				const bool still_frozen = detail::engine_access::get_symbolizer(*fixture.eng)->frozen();
				// The host deliberately froze it; return the engine to normal before
				// assertion diagnostics or the fresh-symbol recovery test can run.
				detail::engine_access::get_symbolizer(*fixture.eng)->set_frozen(false);
				check_eq(inject, failed, "unexpected warmed-region result");
				check_true(still_frozen, "region discarded the host's prior frozen state");
				if (inject) { check_true(trace.fired); }
				check_recovered(fixture, "prior frozen state");
			}
		});
	}
};

class parallel_registry_fault_tests : public suite {
public:
	parallel_registry_fault_tests() : suite("Parallel Registry Faults") {}

	// A child copied into an in-place element survives an exceptional region. The
	// normal join test already pins this value-semantic copy and later migration.
	void check_pending_recovery(bool partial_submission, bool different_region, bool immediate_reload = false,
	                            bool repeated_reload_failure = false) {
		auto eng = make_engine();
		eng->execution_budget(0);
		eng->parallel_thread_count(4);
		eng->execute(R"(
			class PendingLeaf {
				int v = 0;
				PendingLeaf() {} PendingLeaf(int n) { v = n; }
				int marker() { return v + 100; }
			}
			class PendingNode {
				PendingLeaf a = PendingLeaf(0);
				array<PendingLeaf> b = [PendingLeaf(0)];
				PendingNode() {}
				PendingNode(int n) { a = PendingLeaf(n); b = [PendingLeaf(-1)]; }
			}
			array<PendingNode> pending_nodes = [];
			for (int i = 0; i < 256; i++) { pending_nodes.push(PendingNode(i)); }
			bool pending_copy_enabled = false;
		)");
		auto call = eng->jaibite(R"(
			parallel_for (auto& n : pending_nodes) {
				if (pending_copy_enabled) { n.b = [n.a]; }
			}
			pending_nodes.size();
		)");
		// Warm the pool before arming the batch hook: calibration also submits a
		// batch, but carries no class copies and is not the failure under test.
		check_eq(script_int(256), eng->execute(call).as_int());
		auto& state = detail::engine_access::parallel_state(*eng);
		state.worker_overhead_ns = 0.0;
		for (auto& pool : state.region_pools) { pool.ns_per_element = 0.0; }
		auto def = detail::engine_access::get_class_registry(*eng).find_script_class("PendingLeaf");
		check_not_null(def);
		const size_t expected_live = def->live_instances().size();
		const script_array* input = eng->get_variable("pending_nodes").unchecked_array_node();
		eng->execute("pending_copy_enabled = true;");

		struct injection {
			const char* point;
			size_t matches = 0;
			bool fired = false;
			static void visit(void* context, const char* point, size_t) {
				auto& self = *static_cast<injection*>(context);
				if (std::strcmp(self.point, point) == 0 && ++self.matches == 2) {
					self.fired = true;
					throw std::bad_alloc();
				}
			}
		} failure{ partial_submission ? "pool.batch_enqueue" : "registry.instance_append" };
		state.faults = { &failure, &injection::visit };
		bool failed = false;
		try { eng->execute(call); }
		catch (const std::exception&) { failed = true; }
		catch (...) { state.faults = {}; throw; }
		state.faults = {};
		check_true(failure.fired, "second submission/registration checkpoint was not reached");
		check_true(failed, "injected failure did not reach the caller");
		check_false(detail::engine_access::parallel_region_active(*eng), "exception left the region active");
		check_false(state.region_running, "exception left the region running");
		check_false(detail::engine_access::get_symbolizer(*eng)->frozen(), "exception left the symbolizer frozen");
		check_eq(uint32_t(0), input->live_elem_refs, "exception retained an element cursor");

		if (repeated_reload_failure) {
			// The first failed join retained a pending suffix. Fail AGAIN while an
			// immediate hot reload drains it, before executing another region. A
			// drain in redefine_class is too late: both backend builders have already
			// replaced constructor ASTs and method overloads by the time it runs.
			const std::string replacement = R"(
				class PendingLeaf {
					int v = 0;
					PendingLeaf() {} PendingLeaf(int n) { v = n + 1000; }
					int marker() { return v + 1000; }
				}
			)";
			injection retry_failure{ "registry.instance_append" };
			state.faults = { &retry_failure, &injection::visit };
			bool reload_failed = false;
			try { eng->execute(replacement); }
			catch (const std::exception&) { reload_failed = true; }
			catch (...) { state.faults = {}; throw; }
			state.faults = {};
			check_true(retry_failure.fired, "immediate reload did not retry pending registrations");
			check_true(reload_failed, "second injected registration failure did not reach the caller");
			check_true(state.pending_registration_retry, "second failure discarded the remaining retry obligation");
			check_false(detail::engine_access::parallel_region_active(*eng));
			check_false(state.region_running);
			check_false(detail::engine_access::get_symbolizer(*eng)->frozen());

			// Evaluate both observations before the assertion so the red diagnostic
			// reports constructor AND method behavior. Temporaries die at each execute;
			// the expected live-instance count must not grow just from this probe.
			const script_int old_constructor = eng->execute("PendingLeaf(5).v;").as_int();
			const script_int old_method = eng->execute("pending_nodes[0].a.marker();").as_int();
			check_eq(std::string("5|100"), std::to_string(old_constructor) + "|" + std::to_string(old_method),
			         "failed registration retry changed old constructor|method behavior");

			// A later successful retry consumes the suffix once, then installs the
			// replacement normally. This is not a blanket transactional-reload test:
			// only failure in the pre-redefinition registration drain is pinned.
			eng->execute(replacement);
			check_false(state.pending_registration_retry);
			check_eq(script_int(1005), eng->execute("PendingLeaf(5).v;").as_int());
			check_eq(script_int(1000), eng->execute("pending_nodes[0].a.marker();").as_int());
			auto live = def->live_instances();
			std::unordered_set<const class_instance*> unique;
			for (const auto& instance : live) { unique.insert(instance.get()); }
			check_eq(live.size(), unique.size(), "reload retry duplicated an already-consumed instance");
			check_eq(expected_live, live.size(), "reload retry lost surviving pending instances");
			check_eq(uint32_t(0), input->live_elem_refs);
			return;
		}

		if (immediate_reload) {
			// A host may reload immediately after the exception, without first
			// executing another region to recover the deferred registrations.
			eng->execute(R"(
				class PendingLeaf {
					int v = 0; int extra = 7;
					PendingLeaf() {} PendingLeaf(int n) { v = n; }
				}
			)");
			check_eq(script_int(1), eng->execute(R"(
				for (auto& n : pending_nodes) {
					if (n.a.extra != 7 || n.b[0].extra != 7) { return 0; }
				}
				return 1;
			)").as_int(), "immediate hot reload missed surviving worker copies");
			check_eq(expected_live, def->live_instances().size(), "immediate hot reload left survivors unregistered");
		}

		// Do not overwrite the survivors on retry: that would hide both replayed
		// registrations and entries incorrectly discarded on the exceptional exit.
		eng->execute("pending_copy_enabled = false;");
		state.worker_overhead_ns = 0.0;
		for (auto& pool : state.region_pools) { pool.ns_per_element = 0.0; }
		if (different_region) {
			// Recovery must precede pool selection, not depend on reusing the failed
			// statement's contexts; an unrelated region uses a different cache key.
			check_eq(script_int(256), eng->execute(R"(
				parallel_for (auto& n : pending_nodes) { n.a.v = n.a.v + 0; }
				pending_nodes.size();
			)").as_int());
		} else {
			check_eq(script_int(256), eng->execute(call).as_int());
		}
		auto live = def->live_instances();
		std::unordered_set<const class_instance*> unique;
		for (const auto& instance : live) { unique.insert(instance.get()); }
		check_eq(live.size(), unique.size(), "retry registered an already-consumed survivor twice");
		check_eq(expected_live, live.size(), "retry lost surviving deferred instances");
		live.clear();
		check_eq(uint32_t(0), input->live_elem_refs, "following region retained an element cursor");

		// Migration is checked AFTER the following region has drained the retry.
		eng->execute(R"(
			class PendingLeaf {
				int v = 0; int extra = 7;
				PendingLeaf() {} PendingLeaf(int n) { v = n; }
			}
		)");
		check_eq(script_int(1), eng->execute(R"(
			for (auto& n : pending_nodes) {
				if (n.a.extra != 7 || n.b[0].extra != 7) { return 0; }
			}
			return 1;
		)").as_int(), "recovered child instances must participate in hot reload");
	}

	void forge_tests() override {
		test("failed_join_registration_consumes_only_successful_prefix", [this]() {
			check_pending_recovery(false, false);
		});
		test("partial_submit_registrations_recover_before_a_different_region", [this]() {
			check_pending_recovery(true, true);
		});
		test("pending_registrations_recover_before_immediate_hot_reload", [this]() {
			check_pending_recovery(false, false, true);
			check_pending_recovery(true, true, true);
		});
		test("registry_recovery_survives_load", [this]() {
			// The swap-remove race needed contention: an expired weak entry moved
			// by a concurrent swap-remove while a fresh registration refilled the index lost one
			// live instance about once per 150 runs under CPU load. Burner threads supply the
			// contention here; the three recovery shapes must keep every survivor registered.
			std::atomic<bool> stop{false};
			std::vector<std::thread> burners;
			// stops and joins on every exit: a failed check unwinding past a joinable std::thread terminates the run
			struct join_burners {
				std::atomic<bool>& stop;
				std::vector<std::thread>& threads;
				~join_burners() { stop.store(true); for (auto& t : threads) { t.join(); } }
			} joiner{ stop, burners };
			// a few burners make the contention; one per core starved the test itself for many minutes on a loaded machine, where
			// shards running side by side each start their own
			const unsigned burner_count = std::min(3u, std::max(2u, std::thread::hardware_concurrency()) - 1);
			for (unsigned i = 0; i < burner_count; ++i) {
				burners.emplace_back([&stop] { volatile uint64_t spin = 0; while (!stop.load(std::memory_order_relaxed)) { spin = spin + 1; } });
			}
			for (int round = 0; round < 12; ++round) {
				check_pending_recovery(false, false, true);
				check_pending_recovery(true, true, true);
				check_pending_recovery(false, false, false, true);
			}
		});
		test("field_copies_stay_distinct_across_regions", [this]() {
			// Every thread swap-removes the leaves its copies replace while main's chunk registers its own
			// copies directly: no copy may end up shared by two nodes, none may drop out of the registry
			auto eng = make_engine();
			eng->execution_budget(0);
			eng->parallel_thread_count(6);
			eng->execute(R"(
				class RegistryLeaf { int v = 0; RegistryLeaf() {} RegistryLeaf(int n) { v = n; } }
				class RegistryNode {
					RegistryLeaf a = RegistryLeaf(0);
					array<RegistryLeaf> b = [RegistryLeaf(0)];
					RegistryNode() {}
					RegistryNode(int n) { a = RegistryLeaf(n); b = [RegistryLeaf(-1)]; }
				}
				array<RegistryNode> registry_nodes = [];
				for (int i = 0; i < 256; i++) { registry_nodes.push(RegistryNode(i)); }
				function registry_shared_leaves() {
					for (int i = 0; i < 256; i++) { registry_nodes[i].b[0].v = 1000 + i; }
					int shared = 0;
					for (int i = 0; i < 256; i++) { if (registry_nodes[i].b[0].v != 1000 + i) { shared++; } }
					for (int i = 0; i < 256; i++) { registry_nodes[i].b = [RegistryLeaf(-1)]; }
					return shared;
				}
			)");
			auto region = eng->jaibite(R"(
				parallel_for (auto& n : registry_nodes) { n.b = [n.a]; }
				registry_shared_leaves();
			)");
			auto& state = detail::engine_access::parallel_state(*eng);
			auto def = detail::engine_access::get_class_registry(*eng).find_script_class("RegistryLeaf");
			check_not_null(def);
			for (int round = 0; round < 40; ++round) {
				state.worker_overhead_ns = 0.0;
				for (auto& pool : state.region_pools) { pool.ns_per_element = 0.0; }
				check_eq(script_int(0), eng->execute(region).as_int(), "a leaf copied into one node is shared by another");
				check_eq(size_t(512), def->live_instances().size(), "the leaf registry lost or duplicated a live instance");
			}
		});
		test("failed_reload_retry_preserves_old_constructor_and_method", [this]() {
			check_pending_recovery(false, false, false, true);
		});
		test("failed_append_cannot_unregister_a_later_survivor", [this]() {
			auto eng = make_engine();
			eng->execute("class RegistryFaultCell {}");
			auto def = detail::engine_access::get_class_registry(*eng).find_script_class("RegistryFaultCell");
			check_not_null(def);
			check_false(def->has_live_instances());

			// This models a worker-created instance awaiting registration. Setting
			// its class definition does not put it in the live-instance registry.
			auto failed = class_instance::make("RegistryFaultCell", eng.get());
			failed->set_class_definition(def);
			parallel_region_fault_tests::fault_trace trace;
			trace.fail_point = "registry.instance_append";
			detail::fault_injection faults{ &trace, &parallel_region_fault_tests::fault_trace::visit };
			check_throws<std::bad_alloc>([&]() {
				def->register_instance_after_region(failed, &faults);
			});
			check_true(trace.fired, "registry append checkpoint was not reached");
			check_false(def->has_live_instances(), "failed append entered the registry");

			// A successful registration now occupies exactly the index the failed
			// append would have used. The failed instance must still be unregistered:
			// destroying it must not remove this unrelated survivor's entry.
			auto survivor = def->create_instance();
			check_eq(size_t(1), def->live_instances().size());
			failed.reset();
			auto live = def->live_instances();
			check_eq(size_t(1), live.size(), "failed instance destruction unregistered the survivor");
			check_true(live.front() == survivor, "wrong registry survivor");
			live.clear();
			survivor.reset();
			check_false(def->has_live_instances(), "successful registration did not unregister cleanly");
			check_eq(script_int(42), eng->execute("6 * 7;").as_int());
		});
	}
};

} // namespace jai::foundry::tests

using parallel_transform_tests = jai::foundry::tests::parallel_transform_tests;
FOUNDRY_REGISTER(parallel_transform_tests)
using parallel_for_tests = jai::foundry::tests::parallel_for_tests;
FOUNDRY_REGISTER(parallel_for_tests)
using parallel_region_fault_tests = jai::foundry::tests::parallel_region_fault_tests;
FOUNDRY_REGISTER(parallel_region_fault_tests)
using parallel_registry_fault_tests = jai::foundry::tests::parallel_registry_fault_tests;
FOUNDRY_REGISTER(parallel_registry_fault_tests)
