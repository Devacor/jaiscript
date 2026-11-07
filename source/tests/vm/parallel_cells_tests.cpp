// Cell blocks run in every frame a loop can run in: a parallel_for body grafted into a worker's chunk loop, a builtin's
// callback, a host's call into a script function. Each case holds both backends to one result (and the serial loop's),
// and the vm's block counters (the workers' folded in) show the loops entered their blocks.
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class parallel_cells_tests : public suite {
public:
	parallel_cells_tests() : suite("Parallel Cells") {}

	struct outcome { std::string defs, run; unsigned long long entries = 0; };

	// defs run first; the counters are read between the two executes, so entries counts what run alone entered
	static outcome run_split(bool vm, const std::string& defs, const std::string& run, size_t workers) {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
		stdlib::register_all(*e);
		e->execution_budget(0);   // a loaded machine runs the Debug interpreter's defs past the default budget, cutting its serial loop short
		if (workers) { e->parallel_thread_count(workers); }
		outcome result;
		try { result.defs = e->execute(defs).to_string(); } catch (const std::exception& ex) { result.defs = std::string("throw:") + ex.what(); }
		const unsigned long long before = detail::engine_access::cells_counters(*e).entries;
		try { result.run = e->execute(run).to_string(); } catch (const std::exception& ex) { result.run = std::string("throw:") + ex.what(); }
		result.entries = detail::engine_access::cells_counters(*e).entries - before;
		return result;
	}

	// both backends agree on run; with 1 and 4 workers the vm's blocks enter at least min_entries times during run
	void both(const std::string& defs, const std::string& run, const std::string& expected, unsigned long long min_entries, const std::string& what) {
		for (size_t workers : { size_t(1), size_t(4) }) {
			const outcome interp = run_split(false, defs, run, workers), vm = run_split(true, defs, run, workers);
			const std::string tag = what + " (" + std::to_string(workers) + " workers)";
			check(interp.defs.rfind("throw:", 0) != 0 && vm.defs.rfind("throw:", 0) != 0, tag + ": the defs run (" + interp.defs + " | " + vm.defs + ")");
			check_eq(interp.run, vm.run, tag + ": both backends");
			if (!expected.empty()) { check_eq(expected, vm.run, tag + ": the value"); }
			check(vm.entries >= min_entries, tag + ": the loops enter their blocks (" + std::to_string(vm.entries) + " entries)");
		}
	}

	void forge_tests() override {
		test("a_parallel_bodys_grafted_loop_enters_its_block_on_every_worker", [this]() {
			const char* defs = R"JAI(
class Particle { float x = 0.0; float v = 0.0; int hits = 0; }
float GRAVITY = 0.0025;
function step(float x, float v) -> float { return x + v * 0.01 - GRAVITY; }
function update(var& p) -> void {
	float x = p.x; float v = p.v; int hits = p.hits;
	for (int k = 0; k < 60; ++k) { x = step(x, v); if (x < 0.0) { x = x + 1.0; hits = hits + 1; } if (x > 1.0) { x = x - 1.0; } }
	p.x = x; p.hits = hits;
}
function make(int n) -> array<Particle> { array<Particle> ps = []; for (int i = 0; i < n; ++i) { Particle p = Particle(); p.x = (i % 97) * 0.01; p.v = (i % 13) * 0.05; ps.push(p); } return ps; }
function sum(var& ps) -> string { int hits = 0; float xs = 0.0; for (auto& p : ps) { hits = hits + p.hits; xs = xs + p.x; } return to_string(hits) + " " + to_string(xs); }
array<Particle> A = make(512);
array<Particle> B = make(512);
for (int r = 0; r < 3; ++r) { for (auto& p : A) { update(p); } }
)JAI";
			both(defs, "for (int r = 0; r < 3; ++r) { parallel_for (auto& p : B) { update(p); } } sum(B) + \" | \" + sum(A);", "", 1536, "particles");
			const outcome vm = run_split(true, defs, "for (int r = 0; r < 3; ++r) { parallel_for (auto& p : B) { update(p); } } sum(B) == sum(A);", 4);
			check_eq(std::string("true"), vm.run, "the parallel loop lands what the serial loop lands");
		});

		test("a_worker_pushes_strings_from_a_borrowed_table_into_its_own_parts", [this]() {
			const char* defs = R"JAI(
array<string> PAL = ["<a>", "<b>", "<c>", "<d>"];
function row(int y) -> string {
	array<string> parts = [];
	int prev = -1;
	for (int x = 0; x < 40; ++x) { int p = (x * 7 + y * 3) % 4; if (p != prev) { parts.push(PAL[p]); prev = p; } parts.push(" "); }
	string text = "";
	for (int i = 0; i < parts.size(); ++i) { text = text + parts[i]; }
	return text;
}
array<string> serial = [];
for (int y = 0; y < 64; ++y) { serial.push(row(y)); }
)JAI";
			both(defs, "array<string> rows = []; for (int y = 0; y < 64; ++y) { rows.push(\"\"); } array<int> ys = []; for (int y = 0; y < 64; ++y) { ys.push(y); } "
			           "parallel_for (auto& y, auto& out : ys, rows) { out = row(y); } string::join(rows, \"/\") == string::join(serial, \"/\");", "true", 64, "string rows");
		});

		test("a_borrowed_table_changed_between_regions_is_pushed_as_it_now_reads", [this]() {
			// a worker keeps its copies of a borrowed table's strings across regions while nothing else holds them: each region's pushes
			// read the table's current bytes, a rewritten cell and a replaced table included, and the rows a region publishes stay the
			// main thread's
			const char* defs = R"JAI(
array<string> PAL = ["<a>", "<b>", "<c>", "<d>"];
function row(int y) -> string {
	array<string> parts = [];
	for (int x = 0; x < 24; ++x) { parts.push(PAL[(x + y) % 4]); }
	string text = "";
	for (int i = 0; i < parts.size(); ++i) { text = text + parts[i]; }
	return text;
}
function frame(int count) -> string {
	array<string> rows = []; array<int> ys = [];
	for (int y = 0; y < count; ++y) { rows.push(""); ys.push(y); }
	parallel_for (auto& y, auto& out : ys, rows) { out = row(y); }
	string all = "";
	for (int i = 0; i < rows.size(); ++i) { all = all + rows[i] + "/"; }
	return all;
}
function serial(int count) -> string { string all = ""; for (int y = 0; y < count; ++y) { all = all + row(y) + "/"; } return all; }
var kept = [];
)JAI";
			both(defs, "string out = \"\"; for (int r = 0; r < 6; ++r) { string f = frame(40); kept.push(f); out = out + to_string(f == serial(40)) + \" \"; "
			           "if (r == 2) { PAL[1] = \"<B>\"; } if (r == 4) { PAL = [\"<w>\", \"<x>\", \"<y>\", \"<z>\"]; } } out + to_string(kept[0] != kept[5]);",
			     "true true true true true true true", 40, "rewritten tables");
		});

		test("a_workers_member_caches_follow_a_class_reload_between_regions", [this]() {
			// each worker keeps its own field caches per chunk across regions; a reload that reorders and adds fields moves the slots,
			// and the next region reads and writes the fields by name, as the serial loop does
			const std::string path = "D:/virtual/parallel_cells/members.jai";
			const char* first = R"JAI(
class P { int a = 1; int b = 2; }
function weigh(var& p) -> void { int s = 0; for (int k = 0; k < 20; ++k) { s = s + p.a * 10 + p.b; } p.b = s % 97; }
array<P> ps = []; for (int i = 0; i < 400; ++i) { P p = P(); p.a = i % 7; p.b = i % 5; ps.push(p); }
function total() -> int { int t = 0; for (auto& p : ps) { t = t + p.a * 1000 + p.b; } return t; }
)JAI";
			const char* second = R"JAI(
class P { int b = 2; int c = 5; int a = 1; }
function weigh(var& p) -> void { int s = 0; for (int k = 0; k < 20; ++k) { s = s + p.a * 10 + p.b + p.c; } p.b = s % 89; }
array<P> ps = []; for (int i = 0; i < 400; ++i) { P p = P(); p.a = i % 7; p.b = i % 5; ps.push(p); }
function total() -> int { int t = 0; for (auto& p : ps) { t = t + p.a * 1000 + p.b + p.c * 7; } return t; }
)JAI";
			std::string results[2];
			for (bool vm : { false, true }) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e);
				e->parallel_thread_count(4);
				std::string out;
				try {
					e->execute_file_source(path, first);
					for (int r = 0; r < 3; ++r) { e->execute("parallel_for (auto& p : ps) { weigh(p); }"); }
					out = e->execute("to_string(total());").to_string();
					e->execute_file_source(path, second);
					for (int r = 0; r < 3; ++r) { e->execute("parallel_for (auto& p : ps) { weigh(p); }"); }
					out += " " + e->execute("to_string(total());").to_string();
				} catch (const std::exception& ex) { out = std::string("throw:") + ex.what(); }
				results[vm ? 1 : 0] = out;
			}
			check_eq(results[0], results[1], "both backends through the reload");
			check(results[1].find("throw") == std::string::npos, "runs: " + results[1]);
		});

		test("a_builtin_callbacks_loop_runs_as_a_block", [this]() {
			const char* defs = R"JAI(
function weigh(int v) -> int { int s = 0; for (int i = 0; i < 40; ++i) { s = s + (v * i) % 7; } return s; }
array<int> xs = []; for (int i = 0; i < 200; ++i) { xs.push(i); }
)JAI";
			both(defs, "array<int> zs = array::filter(xs, [](int v) -> bool { return weigh(v) > 50; }); int total = zs.size(); array<int> ys = array::map(xs, [](int v) -> int { int s = 0; for (int i = 0; i < 30; ++i) { s = s + v % (i + 1); } return s; }); "
			           "to_string(total) + \" \" + to_string(ys[17]) + \" \" + to_string(ys[199]);", "", 200, "callbacks");
		});

		test("nested_native_entry_frames_each_keep_their_own_area", [this]() {
			// a sort comparator (one native entry) calls a function whose loop runs, inside a map callback (another): each frame's
			// cells are its own, so an inner block never writes the outer one's
			const char* defs = R"JAI(
function key(int v) -> int { int s = 0; for (int i = 0; i < 12; ++i) { s = s + (v * (i + 3)) % 11; } return s; }
array<array<int>> groups = []; for (int g = 0; g < 20; ++g) { array<int> one = []; for (int i = 0; i < 8; ++i) { one.push((g * 31 + i * 17) % 50); } groups.push(one); }
)JAI";
			both(defs, "array<int> firsts = array::map(groups, [](array<int> one) -> int { int acc = 0; for (int i = 0; i < one.size(); ++i) { acc = acc + key(one[i]); } "
			           "one.sort([](int a, int b) -> bool { return key(a) < key(b); }); return acc * 100 + one[0]; }); to_string(firsts);", "", 20, "nested callbacks");
		});

		test("a_raise_inside_a_workers_block_reports_as_the_serial_loop_does", [this]() {
			const char* defs = R"JAI(
function risky(var& v) -> void { int s = 0; for (int i = 0; i < 20; ++i) { s = s + 100 / (v - 300 + i); } v = s; }
array<int> xs = []; for (int i = 0; i < 600; ++i) { xs.push(i); }
)JAI";
			for (size_t workers : { size_t(1), size_t(4) }) {
				const outcome interp = run_split(false, defs, "string out = \"\"; try { parallel_for (auto& v : xs) { risky(v); } } catch (err) { out = err; } out;", workers);
				const outcome vm = run_split(true, defs, "string out = \"\"; try { parallel_for (auto& v : xs) { risky(v); } } catch (err) { out = err; } out;", workers);
				check_eq(interp.run, vm.run, "the raise on both backends (" + std::to_string(workers) + " workers)");
				check(vm.run.find("ivision by zero") != std::string::npos, "the division raises: " + vm.run);
			}
		});

		test("a_hosts_call_into_a_script_function_runs_its_loop_as_a_block", [this]() {
			for (bool vm : { false, true }) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e);
				e->execute("function dot(int n) -> float { float s = 0.0; for (int i = 0; i < n; ++i) { s = s + i * 0.5; } return s; }");
				const unsigned long long before = detail::engine_access::cells_counters(*e).entries;
				script_value fn = e->get_variable("dot");
				double total = 0.0;
				for (int k = 0; k < 30; ++k) { total += fn.as_function()({ script_value(script_int{100 + k}, e.get()) }).value().as_float(); }
				check_eq(std::string("98030.000000"), std::to_string(total), std::string(vm ? "vm" : "interpreter") + ": the host's calls");
				if (vm) { check(detail::engine_access::cells_counters(*e).entries - before >= 30, "each host call enters the loop's block"); }
			}
		});
	}
};

}

using parallel_cells_tests = jai::foundry::tests::parallel_cells_tests;
FOUNDRY_REGISTER(parallel_cells_tests)
