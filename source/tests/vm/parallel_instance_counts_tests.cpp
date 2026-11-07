// Class instances workers reach while a parallel region runs: element objects read, called, built and landed in outputs, and one
// C++ object (its wrapper, its script layer) every worker wraps through a host call. Each case runs many regions on 8 workers and
// holds the result to the serial loop's.
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/script_extensible.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace {
struct shared_probe : jai::script_extensible {
	virtual ~shared_probe() = default;
	int64_t v = 5;
	int64_t read() const { return v; }
};
}

namespace jai::foundry::tests {

class parallel_instance_counts_tests : public suite {
public:
	parallel_instance_counts_tests() : suite("Parallel Instance Counts") {}

	static std::string run(const std::string& program, size_t workers, const std::function<void(engine&)>& bind = {}) {
		auto e = make_engine();
		stdlib::register_all(*e);
		if (bind) { bind(*e); }
		e->parallel_thread_count(workers);
		e->execution_budget(0);
		try { return e->execute(program).to_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
	}

	void forge_tests() override {
		test("an_object_holders_two_words_read_as_the_standard_library_lays_them_out", [this]() {
			check(detail::object_ref::layout_holds(), "a shared_ptr is (stored pointer, control block) here");
		});

		test("element_objects_read_call_build_and_return_through_many_regions", [this]() {
			// a zipped region reads each element's fields, runs its methods, builds and drops temporaries and lands a new object
			// in its output element; 150 regions of 512 elements, the serial loop's checksum on 1 and 8 workers
			const std::string defs = R"JAI(
class Vec { int x = 0; int y = 0; Vec(int a, int b) { x = a; y = b; } int dot(const Vec& o) { return x * o.x + y * o.y; } }
class Body {
	int id = 0; Vec p = Vec(0, 0); Vec v = Vec(1, 2);
	int step() { Vec t = Vec(p.x + v.x, p.y + v.y); p = t; if (p.x > 1000) { p = Vec(0, 0); } return p.dot(v) + id; }
}
function make_bodies(int n) -> array<Body> { array<Body> bs = []; for (int i = 0; i < n; ++i) { Body b = Body(); b.id = i; b.v = Vec(i % 7, i % 5); bs.push(b); } return bs; }
function make_outs(int n) -> array<Vec> { array<Vec> os = []; for (int i = 0; i < n; ++i) { os.push(Vec(0, 0)); } return os; }
function sum(var& os) -> int { int s = 0; for (auto& o : os) { s = (s * 31 + o.x + o.y) % 1000000007; } return s; }
)JAI";
			const std::string serial = defs + R"JAI(
array<Body> bodies = make_bodies(512); array<Vec> outs = make_outs(512);
for (int r = 0; r < 150; ++r) { for (int i = 0; i < 512; ++i) { int s = bodies[i].step(); Vec made = Vec(s, bodies[i].id); outs[i] = made; } }
sum(outs);
)JAI";
			const std::string parallel = defs + R"JAI(
array<Body> bodies = make_bodies(512); array<Vec> outs = make_outs(512);
for (int r = 0; r < 150; ++r) { parallel_for (auto& b, auto& o : bodies, outs) { int s = b.step(); Vec made = Vec(s, b.id); o = made; } }
sum(outs);
)JAI";
			const std::string expected = run(serial, 1);
			check(expected.rfind("throw:", 0) != 0, "the serial loop runs: " + expected);
			for (size_t workers : { size_t(1), size_t(8) }) {
				check_eq(expected, run(parallel, workers), "zipped objects on " + std::to_string(workers) + " workers");
			}
		});

		test("a_script_layer_every_worker_reaches_through_a_host_call", [this]() {
			// every worker wraps the same C++ object's script layer, over and over: the layer and its counts are shared
			// a fresh object per engine: an object keeps its layer past its engine, so one object across engines tests something else
			auto bind = [](engine& e) {
				auto object = std::make_shared<shared_probe>();
				dynamic_binder<shared_probe>(e, "Probe").constructor<>().method("read", &shared_probe::read).build();
				e.add_function("probe", [object]() { return object; });
			};
			const std::string program = R"JAI(
class Layered : Probe { int extra = 2; Layered() : super() {} }
var held = probe();
extend(held, Layered);
var warm = probe();   // the wrap names shared_ptr<Layered> once before a region (a region interns no new name)
array<int> xs = []; array<int> out = [];
for (int i = 0; i < 256; ++i) { xs.push(i); out.push(0); }
int total = 0;
for (int r = 0; r < 300; ++r) {
	parallel_for (int x, auto& o : xs, out) { var p = probe(); var q = probe(); o = 5 + q.extra + x; }
	for (int v : out) { total = (total + v) % 1000000007; }
}
total;
)JAI";
			int64_t expected = 0;
			for (int r = 0; r < 300; ++r) { for (int64_t x = 0; x < 256; ++x) { expected = (expected + 5 + 2 + x) % 1000000007; } }
			for (size_t workers : { size_t(1), size_t(8) }) {
				check_eq(std::to_string(expected), run(program, workers, bind), "a shared layer on " + std::to_string(workers) + " workers");
			}
		});

		test("a_bound_object_every_worker_wraps_again_through_a_host_call", [this]() {
			// a C++ object the script already wrapped, returned by a host call on every worker: each wrap finds the one wrapper and
			// reads its native holder (a copy of that holder raced its count across workers)
			// a fresh object per engine: an object keeps its layer past its engine, so one object across engines tests something else
			auto bind = [](engine& e) {
				auto object = std::make_shared<shared_probe>();
				dynamic_binder<shared_probe>(e, "Probe").constructor<>().method("read", &shared_probe::read).build();
				e.add_function("probe", [object]() { return object; });
			};
			const std::string program = "var warm = probe(); array<int> xs = []; array<int> out = []; for (int i = 0; i < 256; ++i) { xs.push(i); out.push(0); } "
			                            "for (int r = 0; r < 300; ++r) { parallel_for (int x, auto& o : xs, out) { var p = probe(); var q = p; o = x; } } out[5] + out[255];";
			for (size_t workers : { size_t(1), size_t(8) }) {
				check_eq(std::string("260"), run(program, workers, bind), "rewrapped on " + std::to_string(workers) + " workers");
			}
			// the first wrap of an object the script never saw happens inside the region, and works as the warm one does
			const std::string cold = program.substr(std::string("var warm = probe(); ").size());
			for (size_t workers : { size_t(1), size_t(8) }) {
				check_eq(std::string("260"), run(cold, workers, bind), "first wrapped on " + std::to_string(workers) + " workers");
			}
		});

		test("objects_a_region_returns_outlive_it_and_die_on_the_calling_thread", [this]() {
			// a region builds objects (with nested objects and strings) into its outputs; they are the caller's after the join, copied,
			// dropped and rebuilt across 100 regions; the class declares a destructor, so every death runs the chain
			const std::string program = R"JAI(
class Tag { string name = ""; Tag(string n) { name = n; } }
class Box { int n = 0; Tag tag = Tag("t"); Box(int k) { n = k; tag = Tag("box" + to_string(k % 9)); } ~Box() { } }
array<int> xs = []; array<Box> out = [];
for (int i = 0; i < 300; ++i) { xs.push(i); out.push(Box(0)); }
int total = 0;
for (int r = 0; r < 100; ++r) {
	parallel_for (int x, auto& o : xs, out) { Box b = Box(x + r); Box c = b; o = c; }
	array<Box> kept = out;
	for (auto& k : kept) { total = (total + k.n + k.tag.name.size()) % 1000000007; }
}
total;
)JAI";
			int64_t expected = 0;
			for (int r = 0; r < 100; ++r) { for (int64_t x = 0; x < 300; ++x) { expected = (expected + x + r + 4) % 1000000007; } }
			for (size_t workers : { size_t(1), size_t(8) }) {
				check_eq(std::to_string(expected), run(program, workers), "returned objects on " + std::to_string(workers) + " workers");
			}
		});

		// Shared objects: a C++ object's script layer every worker reaches through a host call is read through a borrow. Its fields'
		// blocks are tagged to the main thread in a canary build (JAISCRIPT_DIAG_XTHREAD_RC), so a worker's count touch reports there.
		auto bind_shared = [](engine& e) {
			auto object = std::make_shared<shared_probe>();
			dynamic_binder<shared_probe>(e, "Probe").constructor<>().method("read", &shared_probe::read).build();
			e.add_function("probe", [object]() { return object; });
			e.add_function("tag", [](script_value v) {
#ifdef JAISCRIPT_DIAG_XTHREAD_RC
				v.diag_tag_single_thread();
#endif
				return true;
			});
		};
		const std::string shared_defs = R"JAI(
class Leaf { int n = 3; string label = ""; Leaf() {} }
class Layered : Probe { int extra = 2; string name = ""; array<int> list = []; Leaf leaf = Leaf(); Layered() : super() {} }
var held = probe();
extend(held, Layered);
held.name = "layer-" + to_string(7) + "-name-long-enough-to-own-a-heap-block";
held.list = [1, 2, 3, 4];
held.leaf.label = "leaf-" + to_string(11) + "-label-long-enough-to-own-a-heap-block";
tag(held.name);
tag(held.leaf.label);
var warm = probe();
array<int> xs = []; array<int> out = [];
for (int i = 0; i < 512; ++i) { xs.push(i); out.push(0); }
int total = 0;
)JAI";

		test("workers_copy_a_string_field_of_one_shared_layer", [this, bind_shared, shared_defs]() {
			const std::string program = shared_defs + R"JAI(
for (int r = 0; r < 200; ++r) {
	parallel_for (int x, auto& o : xs, out) { var q = probe(); string s = q.name; string t = s; o = s.size() + t.size() + q.name.size() + x; }
	for (int v : out) { total = (total + v) % 1000000007; }
}
total + held.name.size();
)JAI";
			int64_t expected = 0;
			for (int r = 0; r < 200; ++r) { for (int64_t x = 0; x < 512; ++x) { expected = (expected + 3 * 44 + x) % 1000000007; } }
			for (size_t workers : { size_t(1), size_t(8) }) {
				check_eq(std::to_string(expected + 44), run(program, workers, bind_shared), "a shared layer's string on " + std::to_string(workers) + " workers");
			}
		});

		test("workers_copy_object_and_array_fields_of_one_shared_layer", [this, bind_shared, shared_defs]() {
			const std::string program = shared_defs + R"JAI(
for (int r = 0; r < 200; ++r) {
	parallel_for (int x, auto& o : xs, out) { var q = probe(); var leaf = q.leaf; var l = q.list; string lab = leaf.label; o = leaf.n + l.size() + lab.size() + q.leaf.n + x; }
	for (int v : out) { total = (total + v) % 1000000007; }
}
total + held.leaf.label.size();
)JAI";
			int64_t expected = 0;
			for (int r = 0; r < 200; ++r) { for (int64_t x = 0; x < 512; ++x) { expected = (expected + 3 + 4 + 45 + 3 + x) % 1000000007; } }
			for (size_t workers : { size_t(1), size_t(8) }) {
				check_eq(std::to_string(expected + 45), run(program, workers, bind_shared), "a shared layer's object and array on " + std::to_string(workers) + " workers");
			}
		});

		test("a_shared_layer_returned_from_the_region_outlives_it_as_the_object_itself", [this, bind_shared, shared_defs]() {
			// each worker lands its borrow in its output; the join hands the caller the object's own handle, the serial loop's answer
			const std::string body = shared_defs + R"JAI(
array<var> got = [];
for (int i = 0; i < 512; ++i) { got.push(null); }
LOOP
int same = 0;
for (var g : got) { if (g == held) { same = same + 1; } total = total + g.extra + g.name.size(); }
held.extra = 9;
same * 1000000 + total + got[17].extra;
)JAI";
			auto with = [&](const std::string& loop) { std::string p = body; p.replace(p.find("LOOP"), 4, loop); return p; };
			const std::string expected = run(with("for (int x = 0; x < 512; ++x) { got[x] = probe(); }"), 1, bind_shared);
			check(expected.rfind("throw:", 0) != 0, "the serial loop runs: " + expected);
			for (size_t workers : { size_t(1), size_t(8) }) {
				check_eq(expected, run(with("parallel_for (int x, auto& g : xs, got) { g = probe(); }"), workers, bind_shared), "returned borrows on " + std::to_string(workers) + " workers");
			}
		});

		test("borrows_of_one_shared_layer_compare_equal", [this, bind_shared, shared_defs]() {
			const std::string body = shared_defs + R"JAI(
LOOP
int sum = 0;
for (int v : out) { sum = sum + v; }
sum;
)JAI";
			// two wraps of the host object are the one object; its field compares fieldwise, against itself and against a worker's copy
			const std::string compare = "{ var p = probe(); var q = probe(); var r = q; Leaf mine = Leaf(); mine.label = p.leaf.label; "
			                            "o = (p == q ? 1 : 0) + (r == p ? 2 : 0) + (p != q ? 4 : 0) + (p.leaf == q.leaf ? 8 : 0) + (mine == p.leaf ? 16 : 0) + x; }";
			auto with = [&](const std::string& loop) { std::string p = body; p.replace(p.find("LOOP"), 4, loop); return p; };
			const std::string expected = run(with("for (int x = 0; x < 512; ++x) { auto& o = out[x]; " + compare + " }"), 1, bind_shared);
			check_eq(std::string("144640"), expected, "the serial loop's comparisons: every wrap of a host object is the object");
			for (size_t workers : { size_t(1), size_t(8) }) {
				check_eq(expected, run(with("parallel_for (int x, auto& o : xs, out) " + compare), workers, bind_shared), "compared borrows on " + std::to_string(workers) + " workers");
			}
			// a value comparison reads each side as the worker's own copy, which runs (or fails) no destructor the serial loop does not run
			auto deaths = std::make_shared<std::atomic<int64_t>>(0);
			auto bind_dying = [deaths](engine& e) {
				auto object = std::make_shared<shared_probe>();
				dynamic_binder<shared_probe>(e, "Probe").constructor<>().method("read", &shared_probe::read).build();
				e.add_function("probe", [object]() { return object; });
				e.add_function("death", [deaths]() { deaths->fetch_add(1); return true; });
				e.set_script_error_handler([deaths](const std::string&) { deaths->fetch_add(1); });   // a destructor that fails reports
			};
			const std::string dying = R"JAI(
class Leaf { int n = 3; Leaf() {} ~Leaf() { death(); } }
class Layered : Probe { Leaf leaf = Leaf(); Layered() : super() {} }
var held = probe();
extend(held, Layered);
var warm = probe();
array<int> xs = []; array<int> out = [];
for (int i = 0; i < 512; ++i) { xs.push(i); out.push(0); }
LOOP
int sum = 0;
for (int v : out) { sum = sum + v; }
sum;
)JAI";
			const std::string leaves = "{ var p = probe(); var q = probe(); o = (p.leaf == q.leaf ? 1 : 0) + x; }";
			auto with_dying = [&](const std::string& loop) { std::string p = dying; p.replace(p.find("LOOP"), 4, loop); return p; };
			const std::string serial = run(with_dying("for (int x = 0; x < 512; ++x) { auto& o = out[x]; " + leaves + " }"), 1, bind_dying);
			const int64_t serial_deaths = deaths->exchange(0);
			for (size_t workers : { size_t(1), size_t(8) }) {
				check_eq(serial, run(with_dying("parallel_for (int x, auto& o : xs, out) " + leaves), workers, bind_dying), "compared leaves on " + std::to_string(workers) + " workers");
				check_eq(serial_deaths, deaths->exchange(0), "destructor runs on " + std::to_string(workers) + " workers");
			}
		});

		test("a_shared_layer_converts_as_the_serial_loop_does", [this, bind_shared]() {
			// a by-value use of the shared object converts the worker's own copy of it through its class: a typed slot (to_int,
			// to_float, to_bool, to_string), a condition and its text; a class without to_string shows its fields, and one that runs a
			// destructor converts too (the copy is inert). The layer is first wrapped inside the region
			const std::string body = R"JAI(
CLASS
var held = probe();
extend(held, Num);
array<int> xs = []; array<int> out = [];
for (int i = 0; i < 512; ++i) { xs.push(i); out.push(0); }
string caught = "";
try { LOOP } catch (e) { caught = e; }
int sum = 0;
for (int v : out) { sum = sum + v; }
caught + "|" + to_string(sum);
)JAI";
			const std::string converting = "class Num : Probe { int extra = 2; Num() : super() {} int to_int() { return extra + 40; } float to_float() { return extra * 0.5; } "
			                               "bool to_bool() { return extra > 5; } string to_string() { return \"num\"; } }";
			const std::string plain = "class Num : Probe { int extra = 2; Num() : super() {} }";
			const std::string convert = "{ var q = probe(); int k = q; float f = q; bool b = q; string s = q; "
			                            "o = k + cast<int>(f) + (b ? 1 : 0) + (q ? 10 : 0) + s.size() + to_string(q).size() + (`${q}`).size() + x; }";
			const std::string show = "{ var q = probe(); string s = to_string(q); o = s.size() + to_string(q).size() + (`${q}`).size() + (q ? 1 : 0) + x; }";
			auto with = [&](const std::string& cls, const std::string& loop) {
				std::string p = body;
				p.replace(p.find("CLASS"), 5, cls);
				p.replace(p.find("LOOP"), 4, loop);
				return p;
			};
			const std::string expected = run(with(converting, "for (int x = 0; x < 512; ++x) { auto& o = out[x]; " + convert + " }"), 1, bind_shared);
			check_eq(std::string("|157440"), expected, "the serial loop converts through the class");
			const std::string shown = run(with(plain, "for (int x = 0; x < 512; ++x) { auto& o = out[x]; " + show + " }"), 1, bind_shared);
			check(shown.rfind("|", 0) == 0, "the serial loop shows the fields: " + shown);
			for (size_t workers : { size_t(1), size_t(8) }) {
				check_eq(expected, run(with(converting, "parallel_for (int x, auto& o : xs, out) " + convert), workers, bind_shared), "converted borrows on " + std::to_string(workers) + " workers");
				check_eq(shown, run(with(plain, "parallel_for (int x, auto& o : xs, out) " + show), workers, bind_shared), "shown borrows on " + std::to_string(workers) + " workers");
				const std::string dying = "class Num : Probe { int extra = 2; Num() : super() {} ~Num() {} int to_int() { return extra; } }";
				check_eq(run(with(dying, "for (int x = 0; x < 512; ++x) { auto& o = out[x]; { var q = probe(); int k = q; o = k; } }"), 1, bind_shared),
				         run(with(dying, "parallel_for (int x, auto& o : xs, out) { var q = probe(); int k = q; o = k; }"), workers, bind_shared),
				         "a class with a destructor converts on " + std::to_string(workers) + " workers");
			}
		});

		test("a_store_into_a_shared_layer_is_an_error", [this, bind_shared, shared_defs]() {
			const std::string program = shared_defs + R"JAI(
string caught = "";
try { parallel_for (int x, auto& o : xs, out) { var q = probe(); q.extra = x; } } catch (e) { caught = e; }
caught + "|" + to_string(held.extra);
)JAI";
			for (size_t workers : { size_t(1), size_t(8) }) {
				const std::string got = run(program, workers, bind_shared);
				check(got.find("member 'extra' is stored on a shared object in a parallel body") != std::string::npos && got.find("|2") != std::string::npos,
				      "the store refuses and the layer keeps its field on " + std::to_string(workers) + " workers: " + got);
			}
		});

		test("a_layer_many_workers_read_dies_on_the_main_thread_when_its_object_does", [this]() {
			// the layer's destructor runs at the C++ object's release after the region, on the calling thread, not inside it
			auto log = std::make_shared<std::vector<std::string>>();
			auto slot = std::make_shared<std::shared_ptr<shared_probe>>(std::make_shared<shared_probe>());
			auto main_thread = std::this_thread::get_id();
			auto bind = [log, slot, main_thread](engine& e) {
				dynamic_binder<shared_probe>(e, "Probe").constructor<>().method("read", &shared_probe::read).build();
				e.add_function("probe", [slot]() { return *slot; });
				e.add_function("release", [slot]() { slot->reset(); return true; });
				e.add_function("note", [log, main_thread](std::string s) { log->push_back(s + (std::this_thread::get_id() == main_thread ? "@main" : "@worker")); return true; });
			};
			const std::string program = R"JAI(
class Layered : Probe { int extra = 2; Layered() : super() {} ~Layered() { note("layer dies"); } }
var held = probe();
extend(held, Layered);
var warm = probe();
held = null; warm = null;
array<int> xs = []; array<int> out = [];
for (int i = 0; i < 512; ++i) { xs.push(i); out.push(0); }
for (int r = 0; r < 50; ++r) { parallel_for (int x, auto& o : xs, out) { var q = probe(); o = q.extra + x; } }
string refused = "";
try { parallel_for (int x, auto& o : xs, out) { var q = probe(); o = q.read(); } } catch (e) { refused = e; }
note("joined");
release();
note("released");
to_string(out[511]) + "|" + refused;
)JAI";
			const std::string refusal = "method 'read' was not admitted in this parallel body";
			for (size_t workers : { size_t(1), size_t(8) }) {
				log->clear();
				*slot = std::make_shared<shared_probe>();
				const std::string got = run(program, workers, bind);
				check(got.rfind("513|", 0) == 0 && got.find(refusal) != std::string::npos, "the region's reads, and a host method refused as on master, on " + std::to_string(workers) + " workers: " + got);
				std::string seen;
				for (const auto& line : *log) { seen += line + ";"; }
				check_eq(std::string("joined@main;layer dies@main;released@main;"), seen, "destructor timing on " + std::to_string(workers) + " workers");
			}
		});
	}
};

}

using parallel_instance_counts_tests = jai::foundry::tests::parallel_instance_counts_tests;
FOUNDRY_REGISTER(parallel_instance_counts_tests)
