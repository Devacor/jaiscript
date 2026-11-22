#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <string>

using namespace jai::foundry;

namespace jai::foundry::tests {

// A reference parameter bound to a field or an element that never escapes the callee: whatever representation the
// reference takes, every touch sees the referent as it is now. A push that moves the element storage, a reload that
// retypes the field, a re-class that drops it and a destructor run while it is bound all behave as with a minted reference.
class borrowed_reference_tests : public suite {
public:
	borrowed_reference_tests() : suite("Borrowed References") {}

	std::string run(jai::engine& eng, const std::string& src) {
		try { return eng.execute(src).to_string(); }
		catch (const std::exception& e) { return std::string("threw: ") + e.what(); }
	}

	void forge_tests() override {
		// A reference declared narrower than its slot (over a var, through a downcast, a derived reference into a base slot) names
		// the slot and sees what a store puts there, but its type holds: once the slot holds another type its next use raises.
		// A reference as wide as its slot, or var&/auto&, takes whatever the slot holds
		test("a_narrower_reference_raises_once_its_slot_holds_another_type", [this]() {
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			const std::string classes = "[[warn_mode]];\n"
				"class Animal { string say() { return \"...\"; } }\n"
				"class Dog : Animal { string say() override { return \"woof\"; } string fetch() { return \"ball\"; } }\n"
				"class Cat : Animal { string say() override { return \"meow\"; } }\n";
			check_eq(std::string("Reference to a 'Dog' now names a 'Cat'|Reference to a 'Dog' now names a 'Cat'|Reference to a 'Dog' now names an 'int'|"
			                     "Reference to an 'int' now names a 'string'|Reference to a 'Dog' now names a 'Cat'"), run(*eng, classes + R"(
				string log = "";
				var v = Dog(); Dog& d = v; v = Cat();
				try { log = log + d.say(); } catch (e) { log = log + e; }
				Animal a = Dog(); Dog& d2 = dynamic_cast<Dog&>(a); a = Cat();
				try { log = log + "|" + d2.fetch(); } catch (e) { log = log + "|" + e; }
				var w = Dog(); Dog& d3 = w; w = 5;
				try { log = log + "|" + d3.say(); } catch (e) { log = log + "|" + e; }
				var n = 1; int& i = n; n = "text";
				try { log = log + "|" + to_string(i + 1); } catch (e) { log = log + "|" + e; }
				var z = Dog(); Dog& d5 = z; z = Cat();
				try { d5 = Dog(); } catch (e) { log = log + "|" + e; }
				log;
			)"));
			check_eq(std::string("woof|meow|5|meow|woof|2"), run(*eng, classes + R"(
				var s = Dog(); Dog& d4 = s; s = Dog();
				Animal b = Dog(); Animal& r = b; b = Cat();
				var x = Dog(); var& vr = x; x = 5;
				var y = Dog(); auto& ar = y; y = Cat();
				Dog dd; Animal& wide = dd;
				var n = 1; int& i = n; n = 2;
				d4.say() + "|" + r.say() + "|" + to_string(vr) + "|" + ar.say() + "|" + wide.say() + "|" + to_string(i);
			)"));
		});

		test("element_reference_across_a_reallocating_push", [this]() {
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			check_eq(std::string("43,203|x!,203"), run(*eng, R"(
				void grow_then_write(int& e, array<int>& a) { for (int i = 0; i < 200; ++i) { a.push(i); } e = 42; e = e + 1; }
				void grow_then_append(var& e, array<var>& a) { for (int i = 0; i < 200; ++i) { a.push(i); } e = e + "!"; }
				array<int> a = [1, 2, 3];
				grow_then_write(a[1], a);
				array<var> v = [1, "x", 2.5];
				grow_then_append(v[1], v);
				to_string(a[1]) + "," + to_string(a.size()) + "|" + v[1] + "," + to_string(v.size());
			)"));
		});

		test("element_reference_across_a_shrink_raises", [this]() {
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			const std::string got = run(*eng, R"(
				function shrink_then_touch(int& e, array<int>& a) -> string {
					a.clear();
					string out = "";
					try { int k = e; out = out + "read"; } catch (x) { out = out + x; }
					out = out + "/";
					try { e = 4; out = out + "write"; } catch (x) { out = out + x; }
					return out;
				}
				array<int> a = [1, 2, 3];
				shrink_then_touch(a[1], a);
			)");
			check_true(got.find("read") == std::string::npos && got.find("write") == std::string::npos, "both touches raise (got: " + got + ")");
		});

		test("field_reference_across_a_reload_that_retypes_the_field", [this]() {
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			jai::engine* raw = eng.get();
			eng->add_function("retype_x", [raw]() { raw->execute("class P { float x = 1; }"); });
			run(*eng, R"(
				class P { int x = 1; }
				function bump(var& r) -> string { r = r + 1; retype_x(); try { r = r + 0.5; } catch (e) { return e; } return to_string(r); }
				P p = P();
			)");
			check_eq(std::string("Cannot assign float to field 'x' of P (declared int)|2.0"), run(*eng, "string b = bump(p.x); b + \"|\" + to_string(p.x);"), "the bound reference keeps the type it bound across the reload");
			check_eq(std::string("4.5|4.5"), run(*eng, "p.x = 3; string c = bump(p.x); c + \"|\" + to_string(p.x);"), "a later bind takes the reloaded field");
		});

		test("field_reference_across_a_re_class_raises", [this]() {
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			check_eq(std::string("Reference to a removed field/Reference to a removed field/"), run(*eng, R"(
				class Base { int a = 0; }
				class D1 : Base { int d1 = 1; }
				function poke(int& r, Base& x) -> string {
					x = Base();
					string out = "";
					try { int k = r; out = out + "read"; } catch (e) { out = out + e; }
					out = out + "/";
					try { r = 5; out = out + "write"; } catch (e) { out = out + e; }
					return out + "/";
				}
				function f() -> string { Base x = D1(); D1& dx = cast<D1&>(x); return poke(dx.d1, x); }
				f();
			)"));
		});

		test("field_reference_raises_once_its_owners_last_handle_drops", [this]() {
			// a reference names the field without owning what a handle reached: the object dies with its last handle, at the drop
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			check_eq(std::string("Reference to a destroyed object|Nin:out:"), run(*eng, R"(
				string LOG = "";
				class Node { int v = 0; ~Node() { LOG = LOG + "N"; } }
				class Box { shared_ptr<Node> n = null; }
				function drop_then_write(int& r, Box& b) -> string { b.n = null; LOG = LOG + "in:"; try { r = 7; } catch (e) { return e; } return to_string(r); }
				function f() -> string { Box b = Box(); b.n = new Node(); string s = drop_then_write(b.n.v, b); LOG = LOG + "out:"; return s; }
				string got = f();
				got + "|" + LOG;
			)"));
		});

		test("field_reference_passed_down_a_recursion", [this]() {
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			check_eq(std::string("1,2,3,4,5"), run(*eng, R"(
				class Node { int x = 0; shared_ptr<Node> next = null; }
				void append(shared_ptr<Node>& slot, int x) { if (slot == null) { slot = new Node(); slot.x = x; } else { append(slot.next, x); } }
				shared_ptr<Node> head = null;
				for (int i = 1; i <= 5; ++i) { append(head, i); }
				string out = "";
				shared_ptr<Node> p = head;
				while (p != null) { out = out + (out == "" ? "" : ",") + to_string(p.x); p = p.next; }
				out;
			)"));
		});
	}
};

}

using borrowed_reference_tests = jai::foundry::tests::borrowed_reference_tests;
FOUNDRY_REGISTER(borrowed_reference_tests)
