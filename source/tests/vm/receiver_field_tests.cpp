// A field of the instance a frame slot or the frame's this-field holds, read where it lives: member temps fold into the consumer
// after them (op_mfold), and method grafts on `this`, on a this-field and on a local compile their bodies as the caller's own
// operands. Every shape runs the same on the interpreter, the vm and the vm-reference backend; the listing pins the vm's form.
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class receiver_field_tests : public suite {
public:
	receiver_field_tests() : suite("Receiver Fields") {}

	enum class which { interp, vm, vm_reference };

	static std::shared_ptr<engine> make_for(which w) {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(w == which::interp ? backend_type::interpreter : backend_type::vm);
		if (w == which::vm_reference) { e->vm_reference_mode(true); }
		stdlib::register_all(*e);
		return e;
	}

	static std::string run(which w, const std::string& script) {
		auto e = make_for(w);
		try { return e->execute(script).to_string(); }
		catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
	}

	static std::string listing(const std::string& script) {
		auto e = make_for(which::vm);
		std::string out;
		e->disassembly_sink([&out](const std::string& text, const std::string&) { out += text; out += "\n"; });
		try { e->execute(script); } catch (const std::exception& ex) { out += std::string("throw:") + ex.what(); }
		return out;
	}

	void all(const std::string& script, const std::string& expected, const std::string& label) {
		const std::string oracle = run(which::interp, script);
		check_eq(oracle, run(which::vm, script), label + ": vm");
		check_eq(oracle, run(which::vm_reference, script), label + ": vm-reference");
		if (!expected.empty()) { check_eq(expected, oracle, label + ": expected"); }
	}

	void forge_tests() override {
		test("member_temps_fold_into_their_consumers", [this]() {
			// a this-field base and a local base, read by typed and dynamic arithmetic, a compare-jump, a declaration's move and a member
			// store's value; a store into a this-field base's member is one instruction; the untyped call compiles the grafted method on its own
			const std::string script = R"(
				class Node { int a = 0; int b = 1; float f = 0.5; var v = 3; shared_ptr<Node> next = null; }
				class Walker {
					shared_ptr<Node> cur = null;
					int sum = 0;
					float fs = 0.0;
					void step() {
						cur.a = cur.a + cur.b;
						if (cur.a > 40) { cur.a = cur.a - 40; }
						sum = (sum + cur.a) & 0xFFFFFF;
						int seen = cur.b;
						fs = fs + cur.f * 2.0;
						shared_ptr<Node> n = cur.next;
						if (n.b < 7) { n.b = n.b + seen; } else { n.b = 1; }
						n.v = n.v + cur.a;
						cur.next.f = cur.f;
						cur = n;
					}
				}
				string go() {
					shared_ptr<Node> first = new Node();
					shared_ptr<Node> prev = first;
					for (int i = 0; i < 3; ++i) { shared_ptr<Node> n = new Node(); n.f = i * 0.25; prev.next = n; prev = n; }
					prev.next = first;
					shared_ptr<Walker> w = new Walker();
					w.cur = first;
					for (int i = 0; i < 50; ++i) { w.step(); }
					var untyped = w; untyped.step();
					string out = to_string(w.sum) + "," + to_string(w.fs) + "," + to_string(first.v) + "," + to_string(first.b);
					prev.next = null;
					return out;
				}
				go();
			)";
			all(script, "", "folded member reads and stores");
			const std::string l = listing(script);
			check_true(l.find("MFOLD") != std::string::npos, "the member temps fold: " + l);
			check_true(l.find("MEMBER_TEMP_THIS") != std::string::npos || l.find("MEMBER_TEMP_THIS .. ") != std::string::npos, "a this-field base's member temp: " + l);
		});

		test("a_fold_runs_the_member_temp_on_every_miss", [this]() {
			// a getter on a subclass in the base's slot, a var field whose kind changes, a null receiver (the member read's text), an
			// element alias as the base and an int field read where a float is declared: each runs the general read in its place
			const std::string script = R"(
				int reads = 0;
				class P { int x = 1; var k = 2; }
				class Q : P { function _get_x() -> int { reads = reads + 1; return 100; } }
				class Holder {
					shared_ptr<P> p = null;
					int total = 0;
					void add() { total = total + p.x + p.k; }
					float half() { float h = p.x; return h / 2.0; }
				}
				string go() {
					shared_ptr<Holder> h = new Holder();
					h.p = new P();
					h.add();
					h.p.k = "s";
					string first = "";
					try { h.add(); } catch (e) { first = "caught"; }
					h.p.k = 5;
					h.add();
					h.p = new Q();
					h.add();
					float hf = h.half();
					array<P> ps = [P(), P()];
					int acc = 0;
					for (auto& e : ps) { acc = acc + e.x + 1; }
					h.p = null;
					string null_text = "";
					try { h.add(); } catch (e) { null_text = "null"; }
					return to_string(h.total) + "," + first + "," + to_string(reads) + "," + to_string(hf) + "," + to_string(acc) + "," + null_text;
				}
				go();
			)";
			all(script, "", "fold misses");
		});

		test("a_reload_of_the_receivers_class_refetches_the_fields", [this]() {
			// a class reloaded with its fields in another order mid-run: the folded reads and stores follow the new layout
			const std::string prelude = R"(
				class Cell { int a = 1; int b = 2; }
				class Box { shared_ptr<Cell> c = new Cell(); int s = 0; void step() { c.a = c.a + c.b; s = s + c.a; } }
				Box box = Box();
				int run(int n) { for (int i = 0; i < n; ++i) { box.step(); } return box.s; }
			)";
			auto sequence = [&](which w) {
				auto e = make_for(w);
				try {
					e->execute(prelude);
					std::string out = e->execute("run(5);").to_string();
					e->execute("class Cell { int z = 9; int b = 3; int a = 1; }");
					e->execute("box.c = new Cell();");
					out += "|" + e->execute("run(5);").to_string();
					return out;
				} catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			const std::string oracle = sequence(which::interp);
			check_eq(oracle, sequence(which::vm), "the reload on the vm");
			check_eq(oracle, sequence(which::vm_reference), "the reload on the vm-reference backend");
		});

		test("methods_graft_on_this_on_a_field_and_on_a_local", [this]() {
			// Richards' shapes: a bool test on a this-field receiver, setters on a this-field and on a local, a sibling's bare call; the bodies
			// read program-scope constants past the receiver's members
			const std::string script = R"(
				const int HELD = 4;
				const int SUSPENDED = 2;
				const int NOT_HELD = ~HELD;
				class Tcb {
					shared_ptr<Tcb> link = null;
					int state = 0;
					int priority = 0;
					bool held_or_suspended() { return (state & HELD) != 0 || state == SUSPENDED; }
					void mark_held() { state = state | HELD; }
					void mark_not_held() { state = state & NOT_HELD; }
					void mark_suspended() { state = state | SUSPENDED; }
					int poke(int k) { mark_held(); if (k > 2) { mark_not_held(); } return state; }
				}
				class Sched {
					shared_ptr<Tcb> current = null;
					int holds = 0;
					int walk() {
						int seen = 0;
						while (current != null) {
							if (current.held_or_suspended()) { seen = seen + 1; } else { current.mark_suspended(); }
							current = current.link;
						}
						return seen;
					}
					void hold() { ++holds; current.mark_held(); }
					int release(shared_ptr<Tcb> t) { t.mark_not_held(); return t.state; }
				}
				string go() {
					shared_ptr<Tcb> a = new Tcb(); shared_ptr<Tcb> b = new Tcb(); shared_ptr<Tcb> c = new Tcb();
					a.link = b; b.link = c; b.state = HELD;
					shared_ptr<Sched> s = new Sched();
					s.current = a;
					int w1 = s.walk();
					s.current = c; s.hold();
					int r = s.release(c);
					s.current = a;
					int w2 = s.walk();
					int p = a.poke(1) * 10 + b.poke(3);
					return to_string(w1) + "," + to_string(r) + "," + to_string(w2) + "," + to_string(p) + "," + to_string(a.state) + "," + to_string(b.state) + "," + to_string(c.state);
				}
				go();
			)";
			all(script, "", "method grafts on this, a this-field and a local");
			const std::string l = listing(script);
			for (const char* name : { "-- inlined held_or_suspended", "-- inlined mark_suspended", "-- inlined mark_held", "-- inlined mark_not_held" }) {
				check_true(l.find(name) != std::string::npos, std::string(name) + " grafts: " + l);
			}
		});

		test("a_body_that_moves_its_receivers_field_keeps_the_receiver_the_call_took", [this]() {
			// the callee stores into the field the caller's receiver came from (the caller is the receiver), and a call inside the body
			// reassigns the caller's field: the rest of the body runs on the receiver the call took
			const std::string script = R"(
				class N {
					shared_ptr<N> nxt = null;
					int v = 0;
					string log = "";
					void cut() { nxt = null; v = v + 1; }
					void touch(shared_ptr<N> owner) { owner.nxt = null; v = v + 10; }
					void self_step() { nxt.cut(); log = log + to_string(v) + ";"; }
					void owner_step(shared_ptr<N> me) { nxt.touch(me); log = log + to_string(v) + ";"; }
				}
				string go() {
					shared_ptr<N> a = new N();
					a.nxt = a;
					a.self_step();
					shared_ptr<N> b = new N();
					shared_ptr<N> c = new N();
					b.nxt = c;
					b.owner_step(b);
					string out = a.log + "|" + to_string(a.v) + "|" + b.log + "|" + to_string(c.v) + "|" + to_string(b.nxt == null);
					a.nxt = null;
					return out;
				}
				go();
			)";
			all(script, "", "a body that moves its receiver's field");
		});

		test("a_bool_method_in_a_condition_grafts_as_branches", [this]() {
			// `if (r.m())` and `while (r.m())` over a one-return bool body: the return's expression branches in place; a method a subclass
			// overrides keeps the call, and `!r.m()` and `r.m() && x` keep their forms
			const std::string script = R"(
				const int LIMIT = 3;
				class Counter {
					int n = 0;
					bool more() { return n < LIMIT; }
					bool below() { return n < LIMIT; }
					bool odd() { return (n & 1) != 0; }
				}
				class Never : Counter { bool more() override { return false; } }
				class Loop {
					shared_ptr<Counter> c = null;
					int run() {
						int steps = 0;
						while (c.below()) { c.n = c.n + 1; if (c.odd()) { steps = steps + 10; } else { steps = steps + 1; } }
						if (c.more()) { steps = steps + 100; }
						if (!c.odd() && c.more()) { steps = steps + 1000; }
						return steps;
					}
					bool self_check() { return c != null; }
					int guarded() { if (self_check()) { return 1; } return 0; }
				}
				int count(shared_ptr<Counter> k) { int s = 0; while (k.below()) { k.n = k.n + 1; s = s + 2; } return s; }
				string go() {
					shared_ptr<Loop> l = new Loop();
					l.c = new Counter();
					int a = l.run();
					l.c = new Never();
					int b = l.run();
					int g = l.guarded();
					int k1 = count(new Counter());
					int k2 = count(new Never());
					return to_string(a) + "," + to_string(b) + "," + to_string(g) + "," + to_string(k1) + "," + to_string(k2);
				}
				go();
			)";
			all(script, "", "condition grafts");
			const std::string l = listing(script);
			check_true(l.find("-- inlined below") != std::string::npos && l.find("-- inlined odd") != std::string::npos && l.find("-- inlined self_check") != std::string::npos, "the conditions graft: " + l);
			check_true(l.find("-- inlined more") == std::string::npos, "an overridden method keeps its call: " + l);
		});

		test("a_raise_inside_a_grafted_condition_reports_as_the_call", [this]() {
			// a division by zero inside a grafted condition's expression keeps its text and the trace names the method's frame and line
			const std::string script = R"(
				class Gauge {
					int v = 0;
					bool high() { return 100 / v > 3; }
				}
				class Panel {
					shared_ptr<Gauge> g = new Gauge();
					int poll() {
						int seen = 0;
						for (int i = 3; i >= 0; --i) { g.v = i; if (g.high()) { seen = seen + 1; } }
						return seen;
					}
				}
				Panel p = Panel();
				p.poll();
			)";
			auto traced = [&](which w) {
				auto e = make_for(w);
				std::string out;
				try { e->execute(script); out = "no raise"; } catch (const std::exception& ex) { out = ex.what(); }
				for (const auto& frame : e->last_stack_trace()) { out += " | " + frame.function + ":" + std::to_string(frame.line); }
				return out;
			};
			const std::string oracle = traced(which::interp);
			check_eq(oracle, traced(which::vm), "the raise on the vm");
			check_eq(oracle, traced(which::vm_reference), "the raise on the vm-reference backend");
		});

		test("a_reload_of_a_grafted_method_takes_the_real_call", [this]() {
			// a class reloaded with other bodies, another field order and a field named like a program-scope constant: every graft's guard
			// fails over to the real call
			const std::string prelude = R"(
				const int FLAG = 4;
				class Node { int state = 0; bool held() { return (state & FLAG) != 0; } void mark() { state = state | FLAG; } }
				class Walker {
					shared_ptr<Node> n = new Node();
					int seen = 0;
					int step() { if (n.held()) { seen = seen + 1; } else { n.mark(); } return seen; }
				}
				Walker w = Walker();
				int run(int k) { int t = 0; for (int i = 0; i < k; ++i) { t = t + w.step(); } return t; }
			)";
			auto sequence = [&](which wb) {
				auto e = make_for(wb);
				try {
					e->execute(prelude);
					std::string out = e->execute("run(4);").to_string();
					e->execute("class Node { int FLAG = 1; int state = 0; bool held() { return state > 100; } void mark() { state = state + 60; } }");
					e->execute("w.n = new Node();");
					out += "|" + e->execute("run(4);").to_string();
					return out;
				} catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
			};
			const std::string oracle = sequence(which::interp);
			check_eq(oracle, sequence(which::vm), "the reload on the vm");
			check_eq(oracle, sequence(which::vm_reference), "the reload on the vm-reference backend");
		});

		test("polymorphic_receivers_take_the_fallback_where_the_method_differs", [this]() {
			// a base slot holding subclass instances: an override takes the real call, an inheriting subclass the graft, a field shadowing
			// a program-scope name in a subclass takes the real call
			const std::string script = R"(
				const int K = 3;
				class Shape { int w = 2; int area() { return w * K; } }
				class Square : Shape { int area() override { return w * w; } }
				class Wide : Shape { }
				class Shadowed : Shape { int K = 100; }
				class Bag {
					shared_ptr<Shape> s = null;
					int sum(array<shared_ptr<Shape>>& all) { int t = 0; for (auto& x : all) { s = x; t = t + s.area(); } return t; }
				}
				string go() {
					array<shared_ptr<Shape>> all = [new Shape(), new Square(), new Wide(), new Shadowed()];
					shared_ptr<Bag> b = new Bag();
					return to_string(b.sum(all));
				}
				go();
			)";
			all(script, "", "polymorphic receivers");
		});

		test("a_body_calling_its_siblings_grafts_with_them_on_the_receiver", [this]() {
			// GLOOM's ray march: a method calling a sibling by bare name grafts into a method of its class, the sibling a level down, and an
			// explicit `this.m()` statement grafts like a bare one; a free function's call keeps the method's frame, where its siblings graft;
			// a sibling private to the class takes the real call from outside its family; a sibling a subclass overrides keeps its call
			const std::string script = R"(
				class World {
				public:
					int mw = 8;
					int mh = 8;
					array<int> tiles;
					World() { for (int i = 0; i < 64; ++i) { tiles.push(i % 7 == 0 ? 1 : 0); } }
					int tile_at(int x, int y) {
						if (x < 0 || y < 0 || x >= mw || y >= mh) { return 1; }
						return tiles[y * mw + x];
					}
					bool solid_at(int x, int y) { return tile_at(x, y) != 0; }
					bool los_clear(float ax, float ay, float bx, float by) {
						float t = 0.0;
						while (t < 1.0) {
							if (solid_at(math::ifloor(ax + (bx - ax) * t), math::ifloor(ay + (by - ay) * t))) { return false; }
							t += 0.05;
						}
						return true;
					}
					int counted(int n) { int s = 0; for (int i = 0; i < n; ++i) { s += this.weight(i); } return s; }
					int weight(int i) { return twice(i) + bias(); }
				private:
					int twice(int i) { return i * 2; }
					int bias() { return mw - 7; }
				}
				class Ray {
				public:
					int lit(int x) { return x % 3 == 0 ? 1 : 0; }
					bool dark(int x) { return lit(x) == 0; }
					int span(int n) { int s = 0; for (int x = 0; x < n; ++x) { if (dark(x)) { ++s; } } return s; }
				}
				class Holes : Ray {
				public:
					override int lit(int x) { if (x == 4) { return 1; } return super::lit(x); }
				}
				class Peer {
				public:
					World w = World();
					int weights(int n) { int s = 0; for (int i = 0; i < n; ++i) { s += w.weight(i); } return s; }
				}
				int weights(int n) { World w = World(); int s = 0; for (int i = 0; i < n; ++i) { s += w.weight(i); } return s; }
				string go() {
					World w = World();
					string out = "";
					for (int i = 0; i < 12; ++i) { out += w.los_clear(0.5, 0.5 + i % 6, 7.5, 7.2 - i % 4) ? "1" : "0"; }
					Ray r = Ray();
					Holes h = Holes();
					Peer p = Peer();
					return out + "|" + to_string(w.counted(10)) + "|" + to_string(weights(10)) + "|" + to_string(p.weights(10)) + "|" +
					       to_string(r.span(12)) + "|" + to_string(h.span(12));
				}
				go();
			)";
			all(script, "", "a body calling its siblings");
			const std::string l = listing(script);
			for (const char* name : { "-- inlined solid_at", "-- inlined tile_at", "-- inlined weight", "-- inlined twice" }) {
				check_true(l.find(name) != std::string::npos, std::string(name) + " grafts: " + l);
			}
			const std::string raising = R"(
				class Grid {
					int div = 0;
					int cell(int x) { return 10 / (x - div); }
					int probe(int x) { return cell(x) + 1; }
					int sweep() { int s = 0; for (int x = 3; x >= 0; --x) { s += probe(x); } return s; }
				}
				Grid g = Grid();
				g.sweep();
			)";
			auto traced = [&](which w) {
				auto e = make_for(w);
				std::string out;
				try { e->execute(raising); out = "no raise"; } catch (const std::exception& ex) { out = ex.what(); }
				for (const auto& frame : e->last_stack_trace()) { out += " | " + frame.function + ":" + std::to_string(frame.line); }
				return out;
			};
			const std::string oracle = traced(which::interp);
			check_eq(oracle, traced(which::vm), "a raise in a grafted body's sibling on the vm");
			check_eq(oracle, traced(which::vm_reference), "a raise in a grafted body's sibling on the vm-reference backend");
		});
	}
};

}   // namespace jai::foundry::tests

using receiver_field_tests = jai::foundry::tests::receiver_field_tests;
FOUNDRY_REGISTER(receiver_field_tests)
