// The serial vm's fast paths run on workers through each worker's own rows of the shared chunks: call-site callees, inline
// guards, method grafts, constructors entered in the loop, this-field stores in methods. Each case holds both backends to one
// result and to the serial loop's, across regions and across a redefinition between them.
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>

using namespace jai;
using namespace jai::foundry;

namespace parallel_lanes_hosts {
	struct gauge { int64_t n = 0; gauge() = default; explicit gauge(int64_t a_n) : n(a_n) {} };
}

namespace jai::foundry::tests {

class parallel_lanes_tests : public suite {
public:
	parallel_lanes_tests() : suite("Parallel Lanes") {}

	static std::string run(bool vm, size_t workers, const std::vector<std::string>& steps) {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
		stdlib::register_all(*e);
		e->parallel_thread_count(workers);
		std::string result;
		for (const auto& step : steps) {
			try { result += e->execute(step).to_string() + "|"; } catch (const std::exception& ex) { result += std::string("throw:") + ex.what() + "|"; }
		}
		return result;
	}

	// each step's value on both backends with 1 and 4 workers: one answer everywhere, and it is the expected one when given
	void same_everywhere(const std::vector<std::string>& steps, const std::string& expected, const std::string& what) {
		const std::string reference = run(false, 1, steps);
		if (!expected.empty()) { check_eq(expected, reference, what + ": the value"); }
		for (size_t workers : { size_t(1), size_t(4) }) {
			for (bool vm : { false, true }) {
				check_eq(reference, run(vm, workers, steps), what + (vm ? " (vm, " : " (interpreter, ") + std::to_string(workers) + " workers)");
			}
		}
	}

	void forge_tests() override {
		test("a_workers_call_rows_follow_a_redefinition_between_regions", [this]() {
			same_everywhere({ R"JAI(
function sq(int x) -> int { return x * x; }
function cube(int x) -> int { return x * x * x; }
function body(var& v) -> void { int acc = 0; for (int k = 0; k < 6; ++k) { if (v % 3 == 0) { acc = acc + cube(k + v % 5); } else { acc = acc + sq(k + v % 5); } } v = acc; }
function serial(int n) -> string { array<int> xs = []; for (int i = 0; i < n; ++i) { xs.push(i); } for (auto& v : xs) { body(v); } return to_string(xs); }
function par(int n) -> string { array<int> xs = []; for (int i = 0; i < n; ++i) { xs.push(i); } parallel_for (auto& v : xs) { body(v); } return to_string(xs); }
0;)JAI", "par(300) == serial(300);", "par(300) == serial(300);", "function sq(int x) -> int { return x + 1000; } 0;", "par(300) == serial(300);" },
				"0|true|true|0|true|", "callee rows");
		});

		test("field_layouts_and_declared_types_follow_redefinitions_under_many_workers", [this]() {
			// each round redefines the class (a field added or retyped, so the layout and its declared-type table move) and runs a region
			// that stores into typed fields from every worker: the workers read the tables the definition built, never build them
			std::vector<std::string> steps{ R"JAI(
class P { int a = 0; float b = 0.0; }
function par(int n) -> float { array<P> ps = []; for (int i = 0; i < n; ++i) { ps.push(P()); } parallel_for (auto& p : ps) { for (int k = 0; k < 8; ++k) { p.a = p.a + k; p.b = p.b + 0.5; } } float s = 0.0; for (auto& p : ps) { s = s + p.a + p.b; } return s; }
0;)JAI" };
			std::string expected = "0|";
			for (int round = 0; round < 24; ++round) {
				steps.push_back(round % 2 == 0 ? "class P { int a = 0; float b = 0.0; int c" + std::to_string(round) + " = 1; } 0;" : "class P { int a = 0; float b = 0.0; } 0;");
				steps.push_back("par(512);");
				expected += "0|16384.0|";
			}
			for (int repeat = 0; repeat < 4; ++repeat) {
				for (bool vm : { false, true }) { check_eq(expected, run(vm, 8, steps), std::string("typed field stores across redefinitions (") + (vm ? "vm" : "interpreter") + ")"); }
			}
		});

		test("a_workers_method_graft_follows_a_class_reload_between_regions", [this]() {
			same_everywhere({ R"JAI(
class V { float x = 0.0; float y = 0.0; function len2() -> float { return x * x + y * y; } }
function fill(int n) -> array<V> { array<V> vs = []; for (int i = 0; i < n; ++i) { V v = V(); v.x = i % 7; v.y = i % 5; vs.push(v); } return vs; }
function serial(int n) -> float { array<V> vs = fill(n); float s = 0.0; for (auto& v : vs) { for (int k = 0; k < 4; ++k) { v.x = v.x + v.len2() * 0.001; } s = s + v.x; } return s; }
function par(int n) -> float { array<V> vs = fill(n); parallel_for (auto& v : vs) { for (int k = 0; k < 4; ++k) { v.x = v.x + v.len2() * 0.001; } } float s = 0.0; for (auto& v : vs) { s = s + v.x; } return s; }
0;)JAI", "par(400) == serial(400);", "par(400) == serial(400);",
				"class V { float x = 0.0; float y = 0.0; function len2() -> float { return x - y; } } 0;", "par(400) == serial(400);", "par(400) == serial(400);" },
				"0|true|true|0|true|true|", "method grafts");
		});

		test("constructions_in_a_body_enter_their_constructors_in_the_loop", [this]() {
			same_everywhere({ R"JAI(
class P { float x = 0.0; float y = 1.5; }
class Q { float a; float b; int made = 7; Q(float a0, float b0) { a = a0; b = b0 * 2.0; } function len2() -> float { return a * a + b * b; } }
class R { Q q; int k = 0; R(int n) { q = Q(n, n + 1); k = n; } }
function body(var& v) -> void {
	float s = 0.0;
	for (int k = 0; k < 6; ++k) { P p = P(); p.x = k; Q q = Q(p.x, p.y + v % 3); R r = R(k + v % 4); s = s + q.len2() + r.q.b + r.k + q.made; }
	v = math::itrunc(s);
}
function serial(int n) -> string { array<int> xs = []; for (int i = 0; i < n; ++i) { xs.push(i); } for (auto& v : xs) { body(v); } return to_string(xs); }
function par(int n) -> string { array<int> xs = []; for (int i = 0; i < n; ++i) { xs.push(i); } parallel_for (auto& v : xs) { body(v); } return to_string(xs); }
0;)JAI", "par(200) == serial(200);", "par(5);" },
				"", "constructors");
		});

		test("inherited_methods_run_on_elements_and_on_objects_a_body_makes", [this]() {
			same_everywhere({ R"JAI(
class Base { float x = 1.0; function twice() -> float { return x * 2.0; } function bump() -> void { x = x + 1.0; } }
class D : Base { float y = 0.5; function sum() -> float { return this.twice() + y; } }
class E : Base { int k = 3; }
function fill(int n) -> array<D> { array<D> ds = []; for (int i = 0; i < n; ++i) { D d = D(); d.x = i % 9; d.y = i % 4; ds.push(d); } return ds; }
function serial(int n) -> float { array<D> ds = fill(n); float s = 0.0; for (auto& d : ds) { d.bump(); E e = E(); e.x = d.y; e.bump(); s = s + d.sum() + e.twice() + e.k; } return s; }
function par(int n) -> float {
	array<D> ds = fill(n); array<float> out = []; for (int i = 0; i < n; ++i) { out.push(0.0); }
	parallel_for (auto& d, auto& o : ds, out) { d.bump(); E e = E(); e.x = d.y; e.bump(); o = d.sum() + e.twice() + e.k; }
	float s = 0.0; for (auto& o : out) { s = s + o; } return s;
}
0;)JAI", "par(200) == serial(200);", "par(200) == serial(200);" },
				"0|true|true|", "inherited methods");
		});

		test("class_operators_run_through_their_pins_on_workers", [this]() {
			same_everywhere({ R"JAI(
class V2 {
	float x = 0.0; float y = 0.0;
	V2 operator+(V2 o) { V2 r; r.x = x + o.x; r.y = y + o.y; return r; }
	V2 operator-(V2 o) { V2 r; r.x = x - o.x; r.y = y - o.y; return r; }
	V2 operator*(float k) { V2 r; r.x = x * k; r.y = y * k; return r; }
	V2 operator-() { V2 r; r.x = -x; r.y = -y; return r; }
	bool operator==(V2 o) { return x == o.x && y == o.y; }
	int operator<=>(V2 o) { float a = x * x + y * y; float b = o.x * o.x + o.y * o.y; if (a < b) { return -1; } if (a > b) { return 1; } return 0; }
	float operator[](int i) { if (i == 0) { return x; } return y; }
	function to_string() -> string { return `(${x},${y})`; }
}
class Body { V2 p; V2 v; int near = 0; string tag = ""; }
function fill(int n) -> array<Body> { array<Body> bs = []; for (int i = 0; i < n; ++i) { Body b = Body(); b.p.x = i % 7; b.p.y = i % 3; b.v.x = 0.5; b.v.y = -0.25; bs.push(b); } return bs; }
function step(var& b) -> void {
	V2 origin;
	for (int k = 0; k < 5; ++k) { b.p = b.p + b.v * 0.1; b.v = -b.v; }
	if (b.p < b.v || b.p == origin) { b.near = b.near + 1; }
	if (b.p != b.v) { b.near = b.near + 10; }
	b.near = b.near + math::itrunc(b.p[0] * 100.0);
	b.tag = to_string(b.p);
}
function sum(var& bs) -> string { int n = 0; string t = ""; for (auto& b : bs) { n = n + b.near; t = b.tag; } return `${to_string(n)} ${t}`; }
function serial(int n) -> string { array<Body> bs = fill(n); for (auto& b : bs) { step(b); } return sum(bs); }
function par(int n) -> string { array<Body> bs = fill(n); parallel_for (auto& b : bs) { step(b); } return sum(bs); }
0;)JAI", "par(200) == serial(200);", "par(200) == serial(200);" },
				"0|true|true|", "operators");
		});

		test("bound_operators_run_through_their_pins_on_workers", [this]() {
			// a C++ class's operators: the const ones on any object, op= on the chunk's own objects (an element, one it made)
			using parallel_lanes_hosts::gauge;
			auto run_bound = [](bool vm, size_t workers) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e);
				e->parallel_thread_count(workers);
				dynamic_binder<gauge>(*e, "Gauge")
					.constructor<int64_t>()
					.property("n", &gauge::n)
					.method("+", [](const gauge& a_self, const gauge& a_other) { return gauge(a_self.n + a_other.n); })
					.method("-", [](const gauge& a_self, const gauge& a_other) { return gauge(a_self.n - a_other.n); })
					.method("==", [](const gauge& a_self, int64_t a_other) { return a_self.n == a_other; })
					.method("<", [](const gauge& a_self, const gauge& a_other) { return a_self.n < a_other.n; })
					.method("+=", [](gauge& a_self, const gauge& a_other) -> gauge& { a_self.n += 10 * a_other.n; return a_self; })
					.build();
				std::string out;
				try {
					out = e->execute(R"JAI(
function step(var& g) -> void { Gauge one = Gauge(1); Gauge h = g + one; h += one; if (h == 23 || h < g) { g += one; } g += h - one; }
function fill(int n) -> array<Gauge> { array<Gauge> gs = []; for (int i = 0; i < n; ++i) { gs.push(Gauge(i % 9)); } return gs; }
function total(var& gs) -> int { int t = 0; for (auto& g : gs) { t = t + g.n; } return t; }
array<Gauge> a = fill(300); array<Gauge> b = fill(300);
for (auto& g : a) { step(g); }
parallel_for (auto& g : b) { step(g); }
to_string(total(a)) + " " + to_string(total(b));
)JAI").to_string();
				} catch (const std::exception& ex) { out = std::string("throw:") + ex.what(); }
				return out;
			};
			const std::string reference = run_bound(false, 1);
			check(reference.find("throw") == std::string::npos, "the bound operators run: " + reference);
			const size_t space = reference.find(' ');
			check_eq(reference.substr(0, space), reference.substr(space + 1), "the parallel loop lands the serial loop's totals");
			for (size_t workers : { size_t(1), size_t(4) }) {
				for (bool vm : { false, true }) { check_eq(reference, run_bound(vm, workers), std::string(vm ? "vm " : "interpreter ") + std::to_string(workers)); }
			}
		});

		test("bare_sibling_calls_and_enum_values_run_on_workers", [this]() {
			same_everywhere({ R"JAI(
enum Mode { Idle, Walk, Run }
class Unit {
	int mode = 0; float x = 0.0; int ticks = 0;
	function speed() -> float { if (mode == Mode::Run) { return 2.0; } if (mode == Mode::Walk) { return 1.0; } return 0.0; }
	function advance(float dt) -> void { x = x + speed() * dt; ticks = ticks + 1; }
	function run(int n) -> void { for (int k = 0; k < n; ++k) { advance(0.5); } if (ticks > 10) { mode = Mode::Idle; } }
}
function fill(int n) -> array<Unit> { array<Unit> us = []; for (int i = 0; i < n; ++i) { Unit u = Unit(); u.mode = i % 3; us.push(u); } return us; }
function sum(var& us) -> string { float x = 0.0; int idle = 0; for (auto& u : us) { x = x + u.x; if (u.mode == Mode::Idle) { idle = idle + 1; } } return to_string(x) + " " + to_string(idle); }
function serial(int n) -> string { array<Unit> us = fill(n); for (auto& u : us) { u.run(12); } return sum(us); }
function par(int n) -> string { array<Unit> us = fill(n); parallel_for (auto& u : us) { u.run(12); if (u.mode == Mode::Walk) { u.x = u.x + 1.0; } } return sum(us); }
function par_serial(int n) -> string { array<Unit> us = fill(n); for (auto& u : us) { u.run(12); if (u.mode == Mode::Walk) { u.x = u.x + 1.0; } } return sum(us); }
0;)JAI", "par(240) == par_serial(240);", "par(240) == par_serial(240);" },
				"0|true|true|", "siblings and enums");
		});

		test("scoped_const_reads_fold_before_the_region_and_follow_a_redeclaration", [this]() {
			same_everywhere({ R"JAI(
enum Mode { Idle, Walk, Run }
class Tune { static const int K = 7; }
function body(var& v) -> void { int acc = 0; for (int k = 0; k < 8; ++k) { if (v % 3 == Mode::Run) { acc = acc + Tune::K; } else { acc = acc + Mode::Walk; } } v = acc + Mode::Idle * 100; }
function serial(int n) -> string { array<int> xs = []; for (int i = 0; i < n; ++i) { xs.push(i); } for (auto& v : xs) { body(v); } return to_string(xs); }
function par(int n) -> string { array<int> xs = []; for (int i = 0; i < n; ++i) { xs.push(i); } parallel_for (auto& v : xs) { body(v); } return to_string(xs); }
0;)JAI", "par(300) == serial(300);", "par(300) == serial(300);", "enum Mode { Run, Idle, Walk } 0;", "par(300) == serial(300);", "par(300) == serial(300);",
				  "class Tune { static int K = 7; } 0;", "try { par(30); } catch (err) { return err; } return \"ran\";" },
				"0|true|true|0|true|true|0|parallel_for: 'Tune::K' is no longer a constant (a parallel body reads only const statics of a primitive value)|", "scoped const folds");
		});

		test("a_slots_kept_pins_follow_the_element_classes_between_regions", [this]() {
			same_everywhere({ R"JAI(
class Ka { int n = 0; function f(int k) -> int { return k * 2 + n; } }
class Kb { int n = 0; function f(int k) -> int { return k * 3 - n; } }
class Kc { float x = 0.0; Kc operator+(Kc o) { Kc r; r.x = x + o.x; return r; } }
class Kd { int n = 0; Kc c; function f(int k) -> int { Kc one; one.x = 1.5; c = c + one; return k + n + math::itrunc(c.x); } }
function step(var& v) -> void { int s = 0; for (int k = 0; k < 6; ++k) { s = s + v.f(k); } v.n = s; }
function sum(var& vs) -> int { int t = 0; for (auto& v : vs) { t = t + v.n; } return t; }
function make_a(int n) -> array<Ka> { array<Ka> vs = []; for (int i = 0; i < n; ++i) { Ka v = Ka(); v.n = i % 5; vs.push(v); } return vs; }
function make_b(int n) -> array<Kb> { array<Kb> vs = []; for (int i = 0; i < n; ++i) { Kb v = Kb(); v.n = i % 7; vs.push(v); } return vs; }
function make_d(int n) -> array<Kd> { array<Kd> vs = []; for (int i = 0; i < n; ++i) { Kd v = Kd(); v.n = i % 3; vs.push(v); } return vs; }
function run(var& vs) -> int { parallel_for (auto& v : vs) { step(v); } return sum(vs); }
function ser(var& vs) -> int { for (auto& v : vs) { step(v); } return sum(vs); }
function par_a(int n) -> int { var vs = make_a(n); return run(vs); }
function par_b(int n) -> int { var vs = make_b(n); return run(vs); }
function par_d(int n) -> int { var vs = make_d(n); return run(vs); }
function ser_a(int n) -> int { var vs = make_a(n); return ser(vs); }
function ser_b(int n) -> int { var vs = make_b(n); return ser(vs); }
function ser_d(int n) -> int { var vs = make_d(n); return ser(vs); }
0;)JAI", "par_a(200) == ser_a(200) && par_b(200) == ser_b(200) && par_d(200) == ser_d(200);",
				"par_d(200) == ser_d(200) && par_a(200) == ser_a(200) && par_a(200) == ser_a(200) && par_b(200) == ser_b(200);",
				"class Kb { int n = 0; function f(int k) -> int { return k * 5 + n; } } 0;", "par_b(200) == ser_b(200) && par_a(200) == ser_a(200);" },
				"0|true|true|0|true|", "kept pins");
		});

		test("method_field_stores_run_on_workers_as_on_the_engine", [this]() {
			same_everywhere({ R"JAI(
class Body {
	float x = 0.0; float v = 1.0; int hits = 0; int tag = 0; int last = 0;
	function step(float dt) -> void { x = x + v * dt; if (x > 1.0) { x = x - 1.0; hits = hits + 1; tag = hits * 2; } v = v + 1; }
	function run(int n) -> void { for (int k = 0; k < n; ++k) { this.step(0.01); } last = hits + n; }
}
function fill(int n) -> array<Body> { array<Body> bs = []; for (int i = 0; i < n; ++i) { Body b = Body(); b.v = (i % 17) * 0.3; bs.push(b); } return bs; }
function sum(var& bs) -> string { int h = 0; float xs = 0.0; int tags = 0; int t = 0; for (auto& b : bs) { h = h + b.hits; xs = xs + b.x; tags = tags + b.tag; t = t + b.last; } return to_string(h) + " " + to_string(xs) + " " + to_string(tags) + " " + to_string(t); }
function serial(int n) -> string { array<Body> bs = fill(n); for (auto& b : bs) { b.run(25); } return sum(bs); }
function par(int n) -> string { array<Body> bs = fill(n); parallel_for (auto& b : bs) { b.run(25); } return sum(bs); }
0;)JAI", "par(300) == serial(300);", "par(300) == serial(300);" },
				"0|true|true|", "field stores");
		});

		test("parallel_reduce_folds_class_instances_through_the_element_proof", [this]() {
			same_everywhere({ R"JAI(
class V2 {
	float x = 0.0; float y = 0.0;
	V2 operator+(V2 o) { V2 r; r.x = x + o.x; r.y = y + o.y; return r; }
}
class Body {
	V2 p; int mass = 1; string tag = "";
	function weight() -> int { return mass * 2 + tag.size(); }
}
function fill(int n) -> array<Body> { array<Body> bs = []; for (int i = 0; i < n; ++i) { Body b = Body(); b.p.x = i % 7; b.p.y = i % 3; b.mass = i % 11; b.tag = `t${(i % 5)}`; bs.push(b); } return bs; }
function step_w(int acc, Body b) -> int { return acc + b.weight() + b.mass; }
function add(int a, int b) -> int { return a + b; }
function step_v(V2 acc, Body b) -> V2 { return acc + b.p; }
function join(V2 a, V2 b) -> V2 { return a + b; }
function serial(int n) -> string { array<Body> bs = fill(n); int w = 0; V2 v; for (auto& b : bs) { w = step_w(w, b); v = step_v(v, b); } return `${to_string(w)} ${to_string(v.x)} ${to_string(v.y)}`; }
function par(int n) -> string { array<Body> bs = fill(n); V2 zero; V2 v = parallel_reduce(bs, zero, step_v, join); return `${to_string(parallel_reduce(bs, 0, step_w, add))} ${to_string(v.x)} ${to_string(v.y)}`; }
function untyped(int n) -> int { var bs = []; for (int i = 0; i < n; ++i) { Body b = Body(); b.mass = i; bs.push(b); } return parallel_reduce(bs, 0, [](int acc, var b) -> int { return acc + b.mass; }, add); }
function aliased() -> string { var hs = []; shared_ptr<Body> h = new Body(); for (int i = 0; i < 40; ++i) { hs.push(h); } try { parallel_reduce(hs, 0, [](int acc, var b) -> int { return acc + 1; }, add); } catch (err) { return err; } return "ran"; }
0;)JAI", "par(900) == serial(900);", "untyped(500);", "aliased();" },
				"0|true|124750|parallel_reduce: input elements 0 and 1 hold the same object (an object the step reaches through a handle element belongs to one element)|",
				"reduce over objects");
		});
	}
};

}

using parallel_lanes_tests = jai::foundry::tests::parallel_lanes_tests;
FOUNDRY_REGISTER(parallel_lanes_tests)
