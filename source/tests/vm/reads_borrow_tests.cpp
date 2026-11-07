// A read looks through its chain: `a.b.c`, `grid[y][x].cost` and `w.items[i].pos.x` borrow each inner
// link where it lives and copy only the final value, so a read moves no count and runs no destructor. A link the walk cannot serve
// (a getter, an operator, null, a reload, a worker) reads as before, with its protective copy. Each case holds the interpreter, the
// vm and the vm-reference backend to one result.
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class reads_borrow_tests : public suite {
public:
	reads_borrow_tests() : suite("Reads Borrow") {}

	enum class backend { interpreter, vm, vm_reference };

	static std::string run(backend which, const std::vector<std::string>& steps, size_t workers = 1) {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(which == backend::interpreter ? backend_type::interpreter : backend_type::vm);
		if (which == backend::vm_reference) { e->vm_reference_mode(true); }
		stdlib::register_all(*e);
		e->parallel_thread_count(workers);
		std::string result;
		for (const auto& step : steps) {
			try { result += e->execute(step).to_string() + "|"; } catch (const std::exception& ex) { result += std::string("throw:") + ex.what() + "|"; }
		}
		return result;
	}

	void same_everywhere(const std::vector<std::string>& steps, const std::string& expected, const std::string& what, size_t workers = 1) {
		const std::string reference = run(backend::interpreter, steps, workers);
		if (!expected.empty()) { check_eq(expected, reference, what + " (interpreter)"); }
		check_true(reference.find("throw:") == std::string::npos, what + " runs: " + reference);
		check_eq(reference, run(backend::vm, steps, workers), what + " (vm)");
		check_eq(reference, run(backend::vm_reference, steps, workers), what + " (vm-reference)");
	}

	void forge_tests() override {
		test("reads_through_member_and_index_chains_leave_use_count", [this]() {
			same_everywhere({ R"JAI(
class Leaf { int v = 3; }
class Node { var left = null; var right = null; int v = 1; }
class World { var items = []; var grid = []; }
shared_ptr<Leaf> L = Leaf();
var n = Node(); n.left = L; var m = Node(); m.left = L; n.right = m;
var w = World(); w.items.push(L); var row = []; row.push(L); w.grid.push(row);
function reads(var n, var w) -> int {
	int s = 0;
	for (int i = 0; i < 50; ++i) { int k = 0; s = s + n.left.v + n.right.left.v + w.items[k].v + w.grid[k][0].v + w.grid[0][k].v; }
	return s;
}
0;)JAI", "var before = L.use_count(); var s = reads(n, w); to_string(s) + \":\" + to_string(L.use_count() - before);" },
				"0|750:0|", "chains");
		});

		test("a_getter_mid_chain_reads_through_the_general_path_with_its_copy", [this]() {
			same_everywhere({ R"JAI(
class Leaf { int v = 4; }
class Box {
	var inner = null;
	int seen = 0;
	function _get_peek() { seen = seen + 1; return inner; }
}
class Holder { var box = null; }
shared_ptr<Box> B = Box(); B.inner = Leaf();
var h = Holder(); h.box = B;
var counts = [];
class Probe { function _get_count() { counts.push(B.use_count()); return 7; } }
class Outer { var box = null; var probe = Probe(); }
var o = Outer(); o.box = B;
function reads(var h, var o) -> int { int s = 0; for (int i = 0; i < 3; ++i) { s = s + h.box.peek.v + o.probe.count; } return s; }
0;)JAI", "to_string(reads(h, o)) + \":\" + to_string(B.seen) + \":\" + to_string(counts);" },
				"", "getter");
		});

		test("an_operator_mid_chain_reads_through_the_general_path", [this]() {
			same_everywhere({ R"JAI(
class Leaf { int v = 5; }
class Grid {
	var cells = [];
	int calls = 0;
	var operator[](int i) { calls = calls + 1; return cells[i]; }
}
class World { var grid = Grid(); }
var w = World(); w.grid.cells.push(Leaf()); w.grid.cells.push(Leaf());
function reads(var w) -> int { int s = 0; for (int i = 0; i < 4; ++i) { int k = i % 2; s = s + w.grid[k].v; } return s; }
0;)JAI", "to_string(reads(w)) + \":\" + to_string(w.grid.calls);" },
				"", "operator");
		});

		test("null_mid_chain_raises_the_member_text", [this]() {
			const std::vector<std::string> steps{ R"JAI(
class Leaf { int v = 1; }
class Node { var left = null; var right = null; }
var n = Node(); n.right = Node();
var g = [[null]];
function member_read(var n) -> int { return n.right.left.v; }
function index_read(var g) -> int { int k = 0; return g[k][0].v; }
0;)JAI", "member_read(n);", "index_read(g);", "n.right = null; member_read(n);" };
			const std::string reference = run(backend::interpreter, steps);
			check_true(reference.find("throw:") != std::string::npos, "the reads raise: " + reference);
			check_eq(reference, run(backend::vm, steps), "vm texts");
			check_eq(reference, run(backend::vm_reference, steps), "vm-reference texts");
		});

		test("a_destructor_runs_after_the_read_that_drops_its_last_owner", [this]() {
			same_everywhere({ R"JAI(
var log = [];
class Leaf { int v = 6; }
class Mid {
	var leaf = Leaf();
	function _get_drop() { HOLD.mid = null; log.push("drop"); return leaf; }
	~Mid() { log.push("~Mid"); }
}
class Top { shared_ptr<Mid> mid; }
shared_ptr<Top> HOLD = Top();
function setup() { shared_ptr<Mid> m = Mid(); HOLD.mid = m; }
setup();
function reads(shared_ptr<Top> t) -> int { int s = t.mid.drop.v; log.push("read"); return s; }
0;)JAI", "to_string(reads(HOLD)) + \":\" + to_string(log);" },
				"0|6:[\"drop\", \"~Mid\", \"read\"]|", "drop");
		});

		test("a_getter_or_method_receiver_dies_when_the_call_returns", [this]() {
			same_everywhere({ R"JAI(
var log = [];
class Mid {
	int v = 6;
	function _get_drop() { HOLD.mid = null; log.push("drop"); return v; }
	function dropm() { HOLD.mid = null; log.push("dropm"); return v; }
	~Mid() { log.push("~Mid"); }
}
class Top { shared_ptr<Mid> mid; }
shared_ptr<Top> HOLD = Top();
function setup() { shared_ptr<Mid> m = Mid(); HOLD.mid = m; }
function f() { log.push("f"); }
function r1(shared_ptr<Top> t) -> int { int s = t.mid.drop; log.push("r1"); return s; }
function r2(shared_ptr<Top> t) -> int { int s = t.mid.dropm(); log.push("r2"); return s; }
setup(); r1(HOLD); setup(); r2(HOLD); setup();
var x = HOLD.mid.drop; log.push("top");
f();
0;)JAI", "to_string(log);" },
				"0|[\"drop\", \"~Mid\", \"r1\", \"dropm\", \"~Mid\", \"r2\", \"drop\", \"~Mid\", \"top\", \"f\"]|", "receivers");
		});

		test("a_chain_over_a_temporary_holds_it_until_the_outer_read_completes", [this]() {
			same_everywhere({ R"JAI(
var log = [];
class Leaf { int v = 6; ~Leaf() { log.push("~Leaf"); } }
class Mid {
	string name = "m";
	Leaf leaf;
	function _get_peek() { log.push("peek"); return leaf; }
	~Mid() { log.push("~Mid:" + name); }
}
class Grid { array<Mid> cells; var operator[](int i) { log.push("op[]"); return cells[i]; } }
class Top { Mid mid; array<Mid> mids; Grid grid; ~Top() { log.push("~Top"); } }
function make_mid(string n) -> Mid { Mid m; m.name = n; return m; }
function make_top() -> Top { Top t; t.mid.name = "t"; t.mids.push(make_mid("x")); t.mids.push(make_mid("y")); t.grid.cells.push(make_mid("z")); log.push("made"); return t; }
function reads(int k, var j) {
	log.push("a"); int a = make_top().mid.peek.v;
	log.push("b"); int b = make_mid("b").peek.v;
	log.push("c"); int c = make_top().mids[k + 1].peek.v;
	log.push("d"); int d = make_top().mids[j].peek.v;
	log.push("e"); int e = make_top().grid[0].peek.v;
	log.push("f"); int f2 = make_top().mid.leaf.v + make_mid("g").peek.v;
	log.push("h"); int h = [make_top()][0].mid.peek.v;
	log.push("i"); int i2 = make_top().mid?.peek.v;
	log.push("end");
	return a + b + c + d + e + f2 + h + i2;
}
0;)JAI", "var r = reads(0, 1); to_string(r) + \":\" + to_string(log);", "log = []; int t = make_top().mid.peek.v; to_string(log);" },
				"0|54:[\"a\", \"~Mid:x\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:z\", \"~Leaf\", \"made\", \"peek\", \"~Leaf\", \"~Top\", \"~Mid:z\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:x\", \"~Leaf\", \"~Mid:t\", \"~Leaf\", "
				"\"b\", \"peek\", \"~Leaf\", \"~Mid:b\", \"~Leaf\", "
				"\"c\", \"~Mid:x\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:z\", \"~Leaf\", \"made\", \"peek\", \"~Leaf\", \"~Top\", \"~Mid:z\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:x\", \"~Leaf\", \"~Mid:t\", \"~Leaf\", "
				"\"d\", \"~Mid:x\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:z\", \"~Leaf\", \"made\", \"peek\", \"~Leaf\", \"~Top\", \"~Mid:z\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:x\", \"~Leaf\", \"~Mid:t\", \"~Leaf\", "
				"\"e\", \"~Mid:x\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:z\", \"~Leaf\", \"made\", \"op[]\", \"peek\", \"~Mid:z\", \"~Leaf\", \"~Leaf\", \"~Top\", \"~Mid:z\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:x\", \"~Leaf\", \"~Mid:t\", \"~Leaf\", "
				"\"f\", \"~Mid:x\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:z\", \"~Leaf\", \"made\", \"~Top\", \"~Mid:z\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:x\", \"~Leaf\", \"~Mid:t\", \"~Leaf\", \"peek\", \"~Leaf\", \"~Mid:g\", \"~Leaf\", "
				"\"h\", \"~Mid:x\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:z\", \"~Leaf\", \"made\", \"peek\", \"~Leaf\", \"~Top\", \"~Mid:z\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:x\", \"~Leaf\", \"~Mid:t\", \"~Leaf\", "
				"\"i\", \"~Mid:x\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:z\", \"~Leaf\", \"made\", \"~Top\", \"~Mid:z\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:x\", \"~Leaf\", \"peek\", \"~Leaf\", \"~Mid:t\", \"~Leaf\", \"end\"]|"
				"[\"~Mid:x\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:z\", \"~Leaf\", \"made\", \"peek\", \"~Leaf\", \"~Top\", \"~Mid:z\", \"~Leaf\", \"~Mid:y\", \"~Leaf\", \"~Mid:x\", \"~Leaf\", \"~Mid:t\", \"~Leaf\"]|", "temporaries");
		});

		test("an_argument_temporary_with_a_destructor_is_the_parameter_of_a_call_through_a_value", [this]() {
			same_everywhere({ R"JAI(
var log = [];
class Mid {
	int v = 6;
	string n = "m";
	~Mid() { log.push("~Mid:" + n); }
}
function mk(string n) -> Mid { Mid m; m.n = n; return m; }
function id(var p) { log.push("id"); return p.v; }
function idm(Mid p) { log.push("idm"); return p.v; }
function calls() {
	var fs = [id];
	var a = fs[0](mk("a")); log.push("a.");
	var d = ((p) => p.v)(mk("d")); log.push("d.");
	auto fn = [](var p) { log.push("fn"); return p.v; };
	var e = fn(mk("e")); log.push("e.");
	var gs = [idm];
	var h = gs[0](mk("h")); log.push("h.");
	return to_string(a) + to_string(d) + to_string(e) + to_string(h);
}
0;)JAI", "calls() + \":\" + to_string(log);" },
				"0|6666:[\"id\", \"~Mid:a\", \"a.\", \"~Mid:d\", \"d.\", \"fn\", \"~Mid:e\", \"e.\", \"idm\", \"~Mid:h\", \"h.\"]|", "call temporaries");
		});

		test("a_class_reload_between_reads_moves_the_fields_the_chain_reads", [this]() {
			same_everywhere({ R"JAI(
class Leaf { int v = 2; int w = 9; }
class Node { var left = null; }
var n = Node(); n.left = Leaf();
function reads(var n) -> int { return n.left.v * 100 + n.left.w; }
0;)JAI", "reads(n);", "class Leaf { int pad = 0; int w = 9; int v = 2; } 0;", "n.left = Leaf(); reads(n);", "reads(n);" },
				"0|209|0|209|209|", "reload");
		});

		test("chains_read_on_workers_as_they_read_serially", [this]() {
			for (size_t workers : { size_t(1), size_t(4) }) {
				same_everywhere({ R"JAI(
class P { float x = 0.0; float y = 0.0; }
class E { P pos = P(); int k = 0; }
function fill(int n) -> array<E> { array<E> es = []; for (int i = 0; i < n; ++i) { E e = E(); e.pos.x = i % 7; e.pos.y = i % 3; es.push(e); } return es; }
function serial(int n) -> float { array<E> es = fill(n); float s = 0.0; for (auto& e : es) { s = s + e.pos.x * 2.0 + e.pos.y; } return s; }
function par(int n) -> float {
	array<E> es = fill(n); array<float> out = []; for (int i = 0; i < n; ++i) { out.push(0.0); }
	parallel_for (auto& e, auto& o : es, out) { o = e.pos.x * 2.0 + e.pos.y; }
	float s = 0.0; for (auto& o : out) { s = s + o; } return s;
}
0;)JAI", "par(300) == serial(300);" }, "0|true|", "workers " + std::to_string(workers), workers);
			}
		});
	}
};

}

using reads_borrow_tests = jai::foundry::tests::reads_borrow_tests;
FOUNDRY_REGISTER(reads_borrow_tests)
