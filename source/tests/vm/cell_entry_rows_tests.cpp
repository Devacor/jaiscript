// A cell loop's block-entry row (chunk::entry_memos) answers later entries - other calls, other loops' entries in between - while
// the environment and its epoch hold, and every entry it answers proves the row against the frame it runs in. Each case changes
// what a row recorded between entries and holds both backends to one result, the vm entering its blocks.
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/debug/controller.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <functional>
#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class cell_entry_rows_tests : public suite {
public:
	cell_entry_rows_tests() : suite("Cell Entry Rows") {}

	struct step_result { std::string text; cell_counters counters; };
	using setup_fn = std::function<void(engine&)>;

	// the first step defines the unit at a virtual path; "reload:" re-runs that unit with new source; any other step runs as its own execute
	static std::vector<step_result> run_steps(bool vm, const std::vector<std::string>& steps, bool proofs, const setup_fn& setup = {}) {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
		stdlib::register_all(*e);
		if (!proofs) { e->type_proofs(false); }
		e->disable(jai::filesystem);
		if (setup) { setup(*e); }
		const std::string path = "D:/virtual/cell_entry_rows/unit.jai";
		std::vector<step_result> result;
		for (size_t i = 0; i < steps.size(); ++i) {
			const std::string& step = steps[i];
			std::string text;
			try {
				script_value v = i == 0 ? e->execute_file_source(path, step)
					: step.rfind("reload:", 0) == 0 ? e->execute_file_source(path, step.substr(7))
					: e->execute(step);
				text = v.to_string();
			} catch (const std::exception& ex) { text = std::string("throw:") + ex.what(); }
			result.push_back(step_result{ text, detail::engine_access::cells_counters(*e) });
		}
		return result;
	}

	// both backends, proofs on and off, step by step; on the vm with proofs the blocks enter, and rows answer when want_rows
	void both_steps(const std::vector<std::string>& steps, const std::vector<std::string>& expected, bool want_rows, const std::string& what, const setup_fn& setup = {}) {
		for (bool proofs : { true, false }) {
			const auto interp = run_steps(false, steps, proofs, setup);
			const auto vm = run_steps(true, steps, proofs, setup);
			const std::string tag = what + (proofs ? "" : " (proofs off)");
			for (size_t i = 0; i < steps.size(); ++i) {
				check_eq(interp[i].text, vm[i].text, tag + ": step " + std::to_string(i) + " on both backends");
				if (i < expected.size() && !expected[i].empty()) { check_eq(expected[i], vm[i].text, tag + ": step " + std::to_string(i) + "'s value"); }
			}
			if (proofs) {
				check(vm.back().counters.entries > 0, tag + ": the loops enter their blocks");
				if (want_rows) { check(vm.back().counters.row_entries > 0, tag + ": a row answers a later entry"); }
			}
		}
	}

	void forge_tests() override {
		test("rows_answer_later_calls_and_the_other_loops_of_a_call", [this]() {
			// two loops in one function alternate within each call, and every call after the first enters both from their rows
			both_steps({ R"JAI(
var A = [1, 2, 3, 4, 5, 6];
var B = [10, 20, 30, 40, 50, 60];
int K = 3;
function two(int n) -> int {
	int s = 0;
	for (int i = 0; i < n; ++i) { s = s + A[i] * K; }
	for (int j = 0; j < n; ++j) { s = s + B[j]; }
	return s;
}
int total = 0;
for (int r = 0; r < 20; ++r) { total = total + two(6) + r; }
to_string(total);
)JAI" }, { "5650" }, true, "two loops over globals");
		});

		test("a_container_rebound_between_calls_is_read_through_its_new_node", [this]() {
			both_steps({ R"JAI(
var A = [1, 2, 3, 4];
function sum(int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s = s + A[i]; } return s; }
function psum(var& a, int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s = s + a[i]; } return s; }
var X = [5, 6, 7];
var Y = [100, 200, 300];
string out = to_string(sum(4)) + " " + to_string(sum(4));
A = [10, 20, 30, 40, 50];
out = out + " " + to_string(sum(5));
out = out + " " + to_string(psum(X, 3)) + " " + to_string(psum(Y, 3)) + " " + to_string(psum(X, 3));
X = [1, 1, 1];
out = out + " " + to_string(psum(X, 3));
out;
)JAI", "A = [7, 7]; to_string(sum(2));", "A[0] = 1000; to_string(sum(2));" }, { "10 10 150 18 600 18 3", "14", "1007" }, true, "rebound containers");
		});

		test("store_and_alias_sites_whose_container_changed_resolve_again", [this]() {
			// a load site takes another call's array through its row; a store site, a push receiver and an alias's source table that name
			// another node run the full resolution (its pins and store checks)
			both_steps({ R"JAI(
var X = [1, 2, 3, 4];
var Y = [10, 20, 30, 40];
var P = [];
function fill(var& a, int n, int k) -> int { int s = 0; for (int i = 0; i < n; ++i) { a[i] = a[i] + k; s = s + a[i]; } return s; }
function pushes(var& a, int n) -> int { for (int i = 0; i < n; ++i) { a.push(i); } return a.size(); }
function aliased(var& rows, int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { var& r = rows[i]; s = s + r[0] * 2; } return s; }
var RA = [[1], [2], [3]];
var RB = [[5], [6], [7]];
string out = "";
for (int r = 0; r < 6; ++r) {
	if (r % 2 == 0) { out = out + to_string(fill(X, 4, r)) + " " + to_string(pushes(P, 3)) + " "; } else { out = out + to_string(fill(Y, 4, r)) + " " + to_string(pushes(X, 3)) + " "; }
	out = out + to_string(fill(X, 4, 1)) + " ";
	if (r % 3 == 0) { out = out + to_string(aliased(RA, 3)) + ";"; } else { out = out + to_string(aliased(RB, 3)) + ";"; }
}
out + " " + to_string(X.size()) + " " + to_string(P.size());
)JAI" }, { "" }, true, "changed store sites");
		});

		test("a_global_redeclared_between_calls_is_resolved_again", [this]() {
			// a reload re-runs the unit: the globals are declared again (new bindings), so the rows' epoch is past
			both_steps({ R"JAI(
var A = [1, 2, 3];
int K = 2;
function sum(int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s = s + A[i] * K; } return s; }
to_string(sum(3)) + " " + to_string(sum(3));
)JAI", "to_string(sum(3));", R"JAI(reload:
var A = [4, 5, 6];
int K = 10;
function sum(int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s = s + A[i] * K; } return s; }
to_string(sum(3)) + " " + to_string(sum(3));
)JAI", "to_string(sum(3));" }, { "12 12", "12", "150 150", "150" }, true, "redeclared globals");
		});

		test("an_array_reallocated_between_calls_keeps_its_node", [this]() {
			both_steps({ R"JAI(
var A = [1, 2];
function sum() -> int { int s = 0; for (int i = 0; i < A.size(); ++i) { s = s + A[i]; } return s; }
function grow(int n) -> void { for (int i = 0; i < n; ++i) { A.push(i); } }
string out = to_string(sum());
grow(5000);
out = out + " " + to_string(sum());
A.clear(); A.push(9);
out = out + " " + to_string(sum());
out;
)JAI", "grow(3); to_string(sum());" }, { "3 12497503 9", "12" }, true, "reallocated arrays");
		});

		test("a_frame_record_reused_by_another_function_holds_its_own_rows", [this]() {
			// f and g run on the same record depth in turn (and under h, one deeper), each with its own sites
			both_steps({ R"JAI(
var X = [1, 2, 3, 4];
var Y = [10, 20, 30, 40];
int C = 7;
function f(var& a, int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s = s + a[i]; } return s; }
function g(var& a, var& b, int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s = s + a[i] * b[i] + C; } return s; }
function h(int n) -> int { return f(Y, n) + g(X, Y, n); }
int total = 0;
for (int r = 0; r < 30; ++r) { total = total + f(X, 4) + f(X, 4) + g(Y, X, 4) + g(Y, X, 4) + h(4); if (r == 15) { X = [2, 2, 2, 2]; } }
to_string(total);
)JAI" }, { "" }, true, "two functions on one record");
		});

		test("a_coroutine_suspended_inside_a_loop_resumes_after_the_environment_changed", [this]() {
			// the coroutine's frame runs its loop on the general form; a function's rows around it stay their own
			both_steps({ R"JAI(
var A = [1, 2, 3, 4];
int K = 1;
function sum() -> int { int s = 0; int n = A.size(); for (int i = 0; i < n; ++i) { s = s + A[i] * K; } return s; }
coroutine walker(int n) -> int {
	int s = 0;
	for (int i = 0; i < n; ++i) { s = s + A[i % A.size()] * K; if (i % 3 == 2) { yield s; } }
	return s;
}
var w = walker(12);
string out = to_string(sum()) + " " + to_string(w.next());
A = [100, 200];
out = out + " " + to_string(w.next()) + " " + to_string(sum());
K = 5;
out = out + " " + to_string(w.next()) + " " + to_string(sum());
out;
)JAI", "host_retype(); to_string(sum()) + \" \" + to_string(w.next());" }, { "10 6 506 300 2506 1500" }, true, "a coroutine across changes",
				[](engine& e) { engine* raw = &e; e.add_function("host_retype", [raw]() { raw->add_global("K", script_value(script_int{2}, raw)); }); });
		});

		test("a_hot_reload_of_the_function_or_of_a_globals_type_between_calls", [this]() {
			both_steps({ R"JAI(
var A = [1, 2, 3];
int K = 2;
function sum(int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s = s + A[i] * K; } return s; }
to_string(sum(3)) + " " + to_string(sum(3));
)JAI", R"JAI(reload:
var A = [1, 2, 3];
int K = 2;
function sum(int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s = s + A[i] + K; } return s; }
to_string(sum(3)) + " " + to_string(sum(3));
)JAI", R"JAI(reload:
var A = [1, 2, 3];
float K = 2.5;
function sum(int n) -> float { float s = 0.0; for (int i = 0; i < n; ++i) { s = s + A[i] * K; } return s; }
to_string(sum(3)) + " " + to_string(sum(3));
)JAI", "to_string(sum(3));" }, { "12 12", "12 12", "", "" }, true, "hot reloads");
		});

		test("a_debugger_set_variable_on_a_captured_cell_between_entries", [this]() {
			// the debugger's setVariable writes the program global a lambda names through its cell: an int, then a float the int refuses (the cell keeps its value), then an int
			both_steps({ R"JAI(
int G = 3;
var reads = function(int n) { int acc = 0; for (int i = 0; i < n; ++i) { if (i < G) { acc = acc + 1; } } return acc; };
function nreads(int n) -> int { int acc = 0; for (int i = 0; i < n; ++i) { if (i < G) { acc = acc + 1; } } return acc; }
to_string(reads(8)) + " " + to_string(reads(8)) + " " + to_string(nreads(8)) + " " + to_string(nreads(8));
)JAI", "dbg_set(1); to_string(reads(8)) + \" \" + to_string(nreads(8)) + \" \" + to_string(reads(8));",
				"to_string(dbg_set(2)) + \" \" + to_string(reads(8)) + \" \" + to_string(nreads(8)) + \" \" + type_of(G);",
				"dbg_set(3); to_string(reads(8)) + \" \" + to_string(nreads(8));" }, { "3 3 3 3", "5 5 5", "false 5 5 int", "1 1" }, true, "debugger writes",
				[](engine& e) {
					engine* raw = &e;
					e.add_function("dbg_set", [raw](int how) -> bool {
						if (how == 1) { raw->debugger().set_variable("G", script_value(script_int{5}, raw)); }
						else if (how == 2) {
							try { raw->debugger().set_variable("G", script_value(script_float{6.5}, raw)); }
							catch (const std::exception&) { return false; }
						}
						else { raw->debugger().set_variable("G", script_value(script_int{1}, raw)); }
						return true;
					});
				});
		});

		test("parallel_workers_entering_one_function_keep_their_own_entries", [this]() {
			// the workers share the chunk and never read or write its rows; serial calls around the region keep theirs
			both_steps({ R"JAI(
function score(var& row) -> int { int s = 0; int n = row.size(); for (int i = 0; i < n; ++i) { s = s + row[i] * (i + 1); } return s; }
var rows = [];
var outs = [];
for (int i = 0; i < 64; ++i) { rows.push([i, i + 1, i + 2, i + 3]); outs.push(0); }
int before = 0;
for (int i = 0; i < 64; ++i) { before = before + score(rows[i]); }
var kept = [3, 4, 5, 6];
for (int i = 0; i < 16; ++i) { before = before + score(kept); }
parallel_for (auto& r, auto& o : rows, outs) { o = score(r); }
int after = 0;
for (int i = 0; i < 64; ++i) { after = after + score(rows[i]); }
int par = 0;
for (int i = 0; i < 64; ++i) { par = par + outs[i]; }
to_string(before) + " " + to_string(par) + " " + to_string(after);
)JAI" }, { "" }, true, "parallel workers", [](engine& e) { e.parallel_thread_count(4); });
		});

		test("recursion_reenters_a_function_while_an_outer_activation_is_inside_its_loop", [this]() {
			both_steps({ R"JAI(
var A = [1, 2, 3, 4, 5];
var B = [10, 20, 30];
function rec(int depth, var& a) -> int {
	int s = 0;
	int n = a.size();
	for (int i = 0; i < n; ++i) {
		s = s + a[i];
		if (depth > 0 && i == 1) { if (depth % 2 == 0) { s = s + rec(depth - 1, B); } else { s = s + rec(depth - 1, A); } }
	}
	return s;
}
int total = 0;
for (int r = 0; r < 10; ++r) { total = total + rec(4, A) + rec(3, B); }
to_string(total);
)JAI" }, { "" }, true, "recursion");
		});

		test("a_host_define_retyping_a_global_misses_rather_than_reading_stale", [this]() {
			// a define between calls: the entry reads the global's new value or kind, never the row's last one; a retype inside a run misses at
			// the landing
			const std::vector<std::string> steps = { R"JAI(
int G = 3;
function reads(int n) -> int { int acc = 0; for (int i = 0; i < n; ++i) { if (i < G) { acc = acc + 1; } } return acc; }
function retyped(int n, int at) -> string { int acc = 0; for (int i = 0; i < n; ++i) { if (i == at) { host_retype(); } acc = acc + 1; if (G > 2) { acc = acc + 100; } } return to_string(acc) + " " + type_of(G); }
to_string(reads(8)) + " " + to_string(reads(8));
)JAI", "to_string(reads(8));", "host_define(); to_string(reads(8)) + \" \" + to_string(reads(8));", "host_retype(); to_string(reads(8)) + \" \" + type_of(G);",
				"host_define(); to_string(reads(8));", "retyped(6, 2);" };
			const setup_fn setup = [](engine& e) {
				engine* raw = &e;
				e.add_function("host_define", [raw]() { raw->add_global("G", script_value(script_int{6}, raw)); });
				e.add_function("host_retype", [raw]() { raw->add_global("G", script_value(script_float{7.5}, raw)); });
			};
			both_steps(steps, { "3 3", "3", "6 6", "8 float", "6", "606 float" }, true, "host defines", setup);
			const auto vm = run_steps(true, steps, true, setup);
			check(vm[1].counters.row_entries > vm[0].counters.row_entries, "an unchanged call enters from its row");
			check(vm[5].counters.misses > vm[4].counters.misses, "a retype inside the run misses at the landing");
		});

		test("a_method_frame_with_a_member_scope_never_takes_another_receivers_bindings", [this]() {
			// the base method's loop names G; a subclass field G shadows it for that receiver only
			both_steps({ R"JAI(
int G = 2;
var A = [1, 2, 3, 4];
class Base { int bias = 0; function total(int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s = s + A[i] * G + bias; } return s; } }
class Shadow : Base { int G = 100; }
Base b = Base();
Shadow h = Shadow();
string out = "";
for (int r = 0; r < 3; ++r) { out = out + to_string(b.total(4)) + "/" + to_string(h.total(4)) + " "; }
out;
)JAI" }, { "" }, false, "member scopes");
		});
	}
};

}

using cell_entry_rows_tests = jai::foundry::tests::cell_entry_rows_tests;
FOUNDRY_REGISTER(cell_entry_rows_tests)
