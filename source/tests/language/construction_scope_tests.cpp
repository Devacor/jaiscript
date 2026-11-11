// A construction runs one activation per constructor, as C++ does: its parameters bound, each base in declaration order, then the
// level's field initializers and body, all in that activation. A level's initializer-list arguments see its constructor's parameters
// by name as bound, then `this`, then the definition environment; its field initializers resolve as C++ default member initializers
// do, in the class (`this` and the members), then the definition environment, never the parameters. Every case holds both backends
// to one result, and to the stated value where it is fixed.
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class construction_scope_tests : public suite {
public:
	construction_scope_tests() : suite("Construction Scope") {}

	static std::string run(bool vm, const std::string& program, bool warn_region = false) {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
		stdlib::register_all(*e);
		try { return e->execute(warn_region ? "[[warn_mode]]; " + program : program).to_string(); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
	}

	void both(const std::string& program, const std::string& expected, const std::string& what, bool warn_region = false) {
		const std::string interp = run(false, program, warn_region);
		const std::string vm = run(true, program, warn_region);
		check_eq(interp, vm, what + ": both backends");
		if (!expected.empty()) { check_eq(expected, vm, what); }
		const std::string again = run(true, program + "\n", warn_region);   // a second engine compiles the chunks afresh
		check_eq(vm, again, what + ": repeatable");
	}

	// the program fails its load with the checker's text, and in a [[warn_mode]] region raises `raised` on both backends
	void refused(const std::string& program, const std::string& diagnostic, const std::string& raised, const std::string& what) {
		for (bool vm : {false, true}) {
			const std::string checked = run(vm, program);
			check(checked.rfind("throw:", 0) == 0 && checked.find(diagnostic) != std::string::npos, what + ": the checker reports " + diagnostic + ", got " + checked);
		}
		both(program, "throw:" + raised, what + " (a warn region)", true);
	}

	void forge_tests() override {
		test("a_field_initializer_never_sees_a_constructor_parameter", [this]() {
			// C++ [class.mem]: a default member initializer resolves in the class, then the enclosing scope
			both("int n = 40; class C { int x = n; C(int n = 5) {} } to_string(C(7).x) + \" \" + to_string(C().x);", "40 40", "a global a parameter shadows");
			both("class B { int v = 1; int w = v; B(int v) {} } B(7).w;", "1", "a member a parameter shadows");
			both("int v = 100; class B { int v = 1; int w = v; B(int v) {} } B(7).w;", "1", "a member over a global and a parameter");
			both("int n = 40; class P { int p = n; P(int n) {} } class D : P { D(int n) : super(n + 1) {} } D(5).p;", "40", "a base's field initializer");
			both("class Q { int k = 3; Q() {} } class R : Q { int d = k * 2; R(int k) {} } R(9).d;", "6", "an inherited member a parameter shadows");
			both("class T { int x = 2; int y = this.x + 1; T(int x) {} } T(50).y;", "3", "this.x");
			both("int k = 1; class E { int b = k; E(int k) {} E() : this(10) {} } E().b;", "1", "a delegated-to constructor's parameter");
			both("class C { int x = n; C(int n) {} } int n = 40; C(5).x;", "40", "a global declared after the class");
			both("int n = 40; class H { int x = n; var f = null; H(int n) { f = [&]() { return n; }; } } auto h = H(5); [h.x, h.f()];", "[40, 5]",
			     "a body that captures the parameter by reference");
		});

		test("a_field_initializer_reads_a_member_over_a_global_in_every_construction", [this]() {
			const std::string global = "int v = 100; ";
			both(global + "class B { int v = 1; int w = v; } B().w;", "1", "the implicit default constructor");
			both(global + "class B { int v = 1; int w = v; } B{5}.w;", "5", "aggregate initialization");
			both(global + "class B { int v = 1; int w = v; B() {} } B().w;", "1", "a constructor without parameters");
			both(global + "class P { int v = 1; } class D : P { int w = v; D() : super() {} } D().w;", "1", "an inherited member");
			both(global + "class B { int v = 1; int w = ::v; B(int v) {} } B(7).w;", "100", "::v names the global");
		});

		test("a_name_only_a_parameter_has_is_undeclared_in_a_field_initializer", [this]() {
			refused("class A { int x = count_in + 1; A(int count_in) {} } A(5).x;", "use of undeclared identifier 'count_in'", "Undefined variable 'count_in'",
			        "a bare read");
			refused("class P { int p = 0; P(int v) { p = v; } } class D : P { int d = arg; D(int arg) : super(arg) {} } D(5).d;", "use of undeclared identifier 'arg'",
			        "Undefined variable 'arg'", "the parameter the initializer list passes on");
			refused("class F { var g = [count_in](int q) { return q + count_in; }; F(int count_in) {} } F(5).g(1);", "use of undeclared identifier 'count_in'",
			        "Cannot capture undefined variable 'count_in'", "a lambda's capture");
			refused("class K { var h = [=]() { return count_in; }; K(int count_in) {} } K(1).h();", "use of undeclared identifier 'count_in'",
			        "Undefined variable 'count_in'", "a lambda's body");
		});

		test("a_field_initializer_reads_only_the_fields_declared_above_it", [this]() {
			// C++ [class.base.init]: fields initialize in declaration order, so a read of the field itself or a later one is a load error
			const std::string later = "field initializer of 'spd' reads 'slices' before its initialization: fields initialize in declaration order; declare 'slices' above 'spd'";
			refused("class Gator { int spd = slices; int slices = 0; } Gator().spd;", later, "Cannot assign null to field 'spd' of Gator (declared int)", "a bare read");
			refused("class Gator { int spd = this.slices + 1; int slices = 0; } Gator().spd;", later, "Invalid operands for + operator", "a read through this");
			refused("class G { int x = x; G() {} } G().x;", "field initializer of 'x' reads 'x' before its initialization: fields initialize in declaration order",
			        "Cannot assign null to field 'x' of G (declared int)", "its own field");
			both("class B { int s = 4; } class G : B { int spd = s; int t = 0; var f = [this]() { return t; }; static int k = 2; int u = k; } auto g = G(); [g.spd, g.f(), g.u];",
			     "[4, 0, 2]", "a base's field, a lambda that runs later, a static");
		});

		test("a_field_initializer_hands_its_field_only_a_value_of_its_type", [this]() {
			// null lands in a typed field as `T f = e;` would: an int or a class value refuses it, a bool reads it false, a handle holds it
			both("var n = null; class G { int x = 1; int y = n; } G().y;", "throw:Cannot assign null to field 'y' of G (declared int)", "an int field", true);
			both("class G { int spd = get(); int slices = 4; var get() { return slices; } } G().spd;", "throw:Cannot assign null to field 'spd' of G (declared int)",
			     "a method that reads a later field");
			both("class G { string s = null; } G().s;", "throw:Cannot assign null to field 's' of G (declared string)", "a null literal", true);
			both("var n = null; class F {} class G { F f = n; } G g; 1;", "throw:Cannot assign null to field 'f' of G (declared F)", "a class value field");
			both("var n = null; class F {} class G { bool b = n; bool c = null; shared_ptr<F> p = n; var v = n; } G g; [g.b, g.c, g.p == null, g.v];",
			     "[false, false, true, null]", "the slots that take null");
		});

		test("a_lambda_in_a_field_initializer_captures_in_the_class", [this]() {
			both("int n = 40; class F { var g = [n](int q) { return q + n; }; F(int n) {} } F(5).g(1);", "41", "a global a parameter shadows");
			both("class G { int m = 3; var g = [this](int q) { return q + m; }; G(int m) {} } G(10).g(1);", "4", "a member a parameter shadows");
			both("class L { var g = [](int count_in) { return count_in * 2; }; L(int count_in) {} } L(1).g(4);", "8", "the lambda's own parameter");
		});

		test("the_initializer_list_and_the_body_still_see_the_parameters", [this]() {
			both("int n = 40; class L { int l = 0; L(int v) { l = v; } } class M : L { int x = n; int y = 0; M(int n) : super(n) { y = n; } } auto m = M(5); [m.l, m.x, m.y];",
			     "[5, 40, 5]", "a field initializer beside the list and the body");
			both("class C { int x = 0; C(int n) { x = n; } } class D : C { int y = 0; D(int n) : super(n + 1) { y = n * 2; } } auto d = D(4); to_string(d.x) + \" \" + to_string(d.y);",
			     "5 8", "the class's parameters at its own level, the argument at the base");
		});

		test("an_omitted_parameter_is_bound_to_its_default", [this]() {
			both("class C { int x = 0; C(int n = 5) { x = n; } } to_string(C().x) + \" \" + to_string(C(9).x);", "5 9", "a defaulted parameter");
			both("int calls = 0; int next() { return ++calls; } class B0 { int b = 0; B0(int v) { b = v; } } "
			     "class C : B0 { int x = 0; C(int n = next()) : super(n) { x = b * 10 + n; } } C().x * 100 + calls;", "1101",
			     "a default evaluates once, for the whole activation");
		});

		test("a_base_level_sees_its_parameter_as_bound", [this]() {
			both("class Base { var f = 0; Base(float x) { f = x * 2; } } class D : Base { D() : super(3) {} } to_string(D().f);", "6.0", "an int argument to a float parameter");
			both("class P { int a = 0; P(int x) { a = x; } } class Q : P { int b = 0; Q(int k) : super(k * 10) { b = k; } } auto q = Q(2); to_string(q.a) + \" \" + to_string(q.b);",
			     "20 2", "each level's own parameters");
		});

		test("delegation_and_further_bases_take_their_arguments_in_the_level", [this]() {
			both("class E { int a = 1; int b = 0; E(int k) { b = a + k; } E() : this(10) {} } E().b;", "11", "a delegated-to constructor's parameter");
			both("class L { int l = 0; L(int v) { l = v; } } class R { int r = 0; R(int v) { r = v; } } "
			     "class M : L, R { int m = 0; M(int p, int q) : L(p), R(q * 2) { m = p + q; } } auto x = M(3, 4); to_string(x.l) + \" \" + to_string(x.r) + \" \" + to_string(x.m);",
			     "3 8 7", "two bases named in the list");
		});

		test("a_parameter_the_initializer_list_stores_is_the_bodys", [this]() {
			// C++: one activation per constructor, so a store to a parameter in the initializer list is what the level's field
			// initializers and body read, at every level of a chain
			both("string log = \"\"; class A { int a = 0; A(int x) { a = x; log = `${log}A${x};`; } } "
			     "class B : A { int b = 0; B(int x) : super(x = x + 10) { b = x; log = `${log}B${x};`; } } "
			     "class C : B { int c = 0; C(int x) : super(x = x + 100) { c = x; log = `${log}C${x};`; } } "
			     "C c = C(1); `${log} | ${c.a} ${c.b} ${c.c}`;", "A111;B111;C101; | 111 111 101", "a three-level chain");
			both("class P { int p = 0; P(int v) { p = v; } } class Q : P { int q = 0; Q(int v) : super(v = v * 3) { q = v; } } auto x = Q(2); [x.p, x.q];",
			     "[6, 6]", "a two-level chain");
			both("class E { int a = 0; E(int k) { a = k; } E(int k, int j) : this(k = k + j) { a = a * 100 + k; } } E(1, 2).a;", "303", "a delegating constructor");
			both("class L { int l = 0; L(int v) { l = v; } } class R { int r = 0; R(int v) { r = v; } } "
			     "class M : L, R { int m = 0; M(int p) : L(p = p + 1), R(p = p * 10) { m = p; } } auto x = M(1); [x.l, x.r, x.m];", "[2, 20, 20]",
			     "bases in declaration order, each store seen by the next and by the body");
			both("class B0 { B0(int v) {} } class C : B0 { int f = 0; C(int n) : super(n = n + 1) { f = n; } } C(1).f;", "2", "the body after the list");
		});

		test("a_base_without_a_constructor_is_default_constructed_under_every_level", [this]() {
			const std::string note = "string log = \"\"; int note(string s, int v) { log = log + s + \";\"; return v; } ";
			both(note + "class P { int p = note(\"P.p\", 7); } class B : P { int b = note(\"B.b\", 2); B(int x) { b = b + x; note(\"B()\", 0); } } "
			     "class C : B { int c = note(\"C.c\", 3); C() : super(1) { note(\"C()\", 0); } } C c = C(); `${log} | ${c.p} ${c.b} ${c.c}`;",
			     "P.p;B.b;B();C.c;C(); | 7 3 3", "a plain base under a base level");
			both(note + "class L { int l = note(\"L.l\", 1); } class R { int r = note(\"R.r\", 2); R() { note(\"R()\", 0); } } "
			     "class M : L, R { int m = note(\"M.m\", 3); M(int k) { note(\"M()\", 0); } } class D : M { D() : super(1) { note(\"D()\", 0); } } D(); log;",
			     "L.l;R.r;R();M.m;M();D();", "both bases of a base level, in declaration order");
			both(note + "class X { X(int v) { note(`X${v}`, 0); } } class Y : X { Y() : super(1) { note(\"Y()\", 0); } } class Z : Y { Z() { note(\"Z()\", 0); } } Z(); log;",
			     "X1;Y();Z();", "an implicit super() over a base level");
			both("int n = 40; class P { int p = n; } class C : P { C(int n) {} } C(5).p;", "40", "a base's initializers do not see the derived constructor's parameters");
		});

		test("an_initializer_list_a_level_cannot_honor_raises_at_every_level", [this]() {
			both("class P { int p = 1; } class B : P { B() : super(7) {} } class C : B { C() : super() {} } string r = \"made\"; try { C(); } catch (e) { r = to_string(e); } r;",
			     "No matching parent constructor found for super()", "arguments to a base without a constructor");
			both("class B { B() : super() {} } class C : B { C() : super() {} } string r = \"made\"; try { C(); } catch (e) { r = to_string(e); } r;",
			     "Cannot call super() - class has no base class", "super() in a base-less base");
		});

		test("a_throw_at_any_level_destroys_what_it_constructed_in_reverse", [this]() {
			const std::string prelude = "string log = \"\"; int note(string s, int v) { log = log + s + \";\"; return v; } int at = 0; "
			                            "int boom(string where, int v) { if (at == v) { throw where; } return v; } string out = \"\"; ";
			both(prelude + "class A { int a = boom(\"A.a\", 1); A() { note(\"A()\", 0); boom(\"A()\", 2); } ~A() { note(\"~A\", 0); } } "
			     "class B : A { int b = boom(\"B.b\", 3); B(int x) : super() { note(\"B()\", 0); boom(\"B()\", 4); } ~B() { note(\"~B\", 0); } } "
			     "class C : B { int c = boom(\"C.c\", 5); C() : super(boom(\"C.super\", 6)) { note(\"C()\", 0); boom(\"C()\", 7); } ~C() { note(\"~C\", 0); } } "
			     "for (int i = 1; i <= 8; ++i) { at = i; log = \"\"; try { C c = C(); note(\"made\", 0); } catch (e) { log = `${log}!${e}`; } out = `${out}${i}:${log} `; } out;",
			     "1:!A.a 2:A();!A() 3:A();~A;!B.b 4:A();B();~A;!B() 5:A();B();~B;~A;!C.c 6:!C.super 7:A();B();C();~B;~A;!C() 8:A();B();C();made;~C;~B;~A; ",
			     "a three-level chain");
			both(prelude + "class P { int p = boom(\"P.p\", 1); ~P() { note(\"~P\", 0); } } class Q { int q = 0; Q() { boom(\"Q()\", 2); note(\"Q()\", 0); } ~Q() { note(\"~Q\", 0); } } "
			     "class M : P, Q { int m = boom(\"M.m\", 3); M() { note(\"M()\", 0); boom(\"M()\", 4); } ~M() { note(\"~M\", 0); } } "
			     "class D : M { D() : super() { note(\"D()\", 0); boom(\"D()\", 5); } ~D() { note(\"~D\", 0); } } "
			     "for (int i = 1; i <= 6; ++i) { at = i; log = \"\"; try { D d = D(); note(\"made\", 0); } catch (e) { log = `${log}!${e}`; } out = `${out}${i}:${log} `; } out;",
			     "1:!P.p 2:~P;!Q() 3:Q();~Q;~P;!M.m 4:Q();M();~Q;~P;!M() 5:Q();M();D();~M;~Q;~P;!D() 6:Q();M();D();made;~D;~M;~Q;~P; ",
			     "two bases under a base level");
		});

		test("a_reference_parameter_binds_the_callers_place_at_every_level", [this]() {
			both("int h = 1; class D { int d = 0; D(int& r) { r = r + 1; d = r; } } D x = D(h); [h, x.d];", "[2, 2]", "the class's own constructor");
			both("int g = 1; class A { int a = 0; A(int x) { a = x; } } class B : A { B(int& r) : super(r = r + 5) { r = r * 2; } } B b = B(g); [g, b.a];",
			     "[12, 6]", "the initializer list writes through the parameter");
			both("int k = 1; class P { int a = 0; P(int& x) { x = x + 10; a = x; } } class Q : P { int b = 0; Q(int& r) : super(r) { r = r + 1; b = r; } } "
			     "Q q = Q(k); [k, q.a, q.b];", "[12, 11, 12]", "a base level's parameter through super(r) from the class's own");
			both("int m = 5; class S { S(int& x) { x = x * 2; } } class T : S { T(int v) : super(m) {} } T(0); m;", "10", "a base level's parameter bound to a global");
			both("int n = 4; class G { int g = 0; G(int v) { g = v; } } class F : G { F(int& r) : super(r * 3) { r = 0; } } F x = F(n); [n, x.g];", "[0, 12]",
			     "an initializer-list argument reads the parameter");
			both("array<int> arr = [1, 2]; class D { int d = 0; D(int& r) { r = r + 1; d = r; } } D x = D(arr[1]); `${to_string(arr)} ${x.d}`;", "[1, 3] 3",
			     "an element place");
			both("int h = 1; class D { int d = 0; D(int& r) { r = r + 1; d = r; } } shared_ptr<D> p = new D(h); [h, p.d];", "[2, 2]", "a new expression");
			both("int f() { return 3; } class W { int w = 0; W(const int& c) { w = c; } } W(f()).w;", "3", "a const reference takes a temporary");
		});

		test("a_reference_parameter_without_a_place_raises", [this]() {
			both("int f() { return 3; } class D { int d = 0; D(int& r) { r = r + 1; d = r; } } string s = \"made\"; try { D(f()); } catch (e) { s = to_string(e); } s;",
			     "Cannot pass non-lvalue to reference parameter", "the class's own constructor");
			both("int f() { return 3; } class S { S(int& x) { x = x * 2; } } class U : S { U() : super(f()) {} } string s = \"made\"; try { U(); } catch (e) { s = to_string(e); } s;",
			     "Cannot pass non-lvalue to reference parameter", "a base level's constructor");
		});

		test("every_construction_shape_agrees_across_the_backends", [this]() {
			const std::string note = "string log = \"\"; int note(string s, int v) { log = log + s + \";\"; return v; } ";
			const char* shapes[] = {
				"class P { int p = note(\"P\", 1); } class Q : P { int q = note(\"Q\", 2); } class R : Q { int r = note(\"R\", 3); } R x = R(); log + x.p + x.q + x.r;",
				"class A { int a = 0; A(int v) { a = v; note(\"A\" + v, 0); } A() : this(7) { note(\"A()\", 0); } } class B : A { B() : super() { note(\"B\", 0); } } B x = B(); log + x.a;",
				"class L { int l = note(\"L\", 1); } class R { int r = 0; R(int v) { r = v; note(\"R\", 0); } } class M : L, R { int m = note(\"M\", 3); M() : R(5) {} } M x = M(); log + x.l + x.r + x.m;",
				"class V { int v = id(); int id() { return 1; } V() {} } class W : V { int w = id(); int id() override { return 2; } W() {} } W x = W(); to_string(x.v) + x.w;",
				"class A { int a = 1; A() {} } class B : A { int b = a + 1; B() : super() {} } class C : B { int c = a + b; C() {} } C x = C(); to_string(x.a) + x.b + x.c;",
				"class Inner { int i = note(\"I\", 4); Inner() {} } class Outer { Inner held = Inner(); int o = note(\"O\", 5); Outer() { note(\"O()\", 0); } } Outer x = Outer(); log + x.held.i + x.o;",
				"class Base { int seen = 0; Base() { Base other = Base(1); seen = other.seen + 1; } Base(int v) { seen = v * 10; } } Base x = Base(); x.seen;",
				"class T { int t = 0; T(int a, int b = 4) { t = a * 10 + b; } } class U : T { U(int k) : super(k) {} } U(4).t;",
				"class S { array<int> items = [1]; S(array<int> more) { more.push(2); items.push(more.size()); } } class D : S { D(array<int> a) : super(a) { a.push(7); } } array<int> keep = [5]; D x = D(keep); to_string(x.items) + to_string(keep);",
				"class K { var f = [this](int q) { return q + base; }; int base = 3; K() {} } K().f(4);",
				"class N { int n = 0; N(int v) { n = v; } } class O : N { O() : super(note(\"arg\", 6)) { note(\"O\", 0); } } O x = O(); log + x.n;",
			};
			for (const char* shape : shapes) { both(note + shape, "", shape); }
		});

		test("an_initializer_runs_against_the_class_under_construction", [this]() {
			both("class G { int v = who(); int who() { return 1; } } class H : G { int who() override { return 2; } H() : super() {} } H().v;", "1",
			     "a virtual call in a base's field initializer");
			both("class I { int a = 3; int b = a * 2; I(int z) {} } I(0).b;", "6", "a field read in a later initializer");
		});

		test("an_initializer_that_raises_raises_as_before", [this]() {
			both("class T { int i = 7; int x = [1, 2][i]; T(int n) {} } string r = \"made\"; try { T(7); } catch (err) { r = to_string(err); } r;", "", "an index past the end in a field initializer");
			both("function fail(int n) -> int { throw \"no \" + to_string(n); } class U { int x = 0; U(int v) { x = v; } } class V : U { V(int n) : super(fail(n)) {} } "
			     "string r = \"made\"; try { V(4); } catch (err) { r = to_string(err); } r;", "no 4", "a throw in an initializer-list argument");
			both("class W { int x = q; W(int n) {} } W(1).x;", "", "an unknown name in a field initializer");
		});

		test("a_closure_in_an_initializer_list_keeps_the_level_environment", [this]() {
			both("class P { var g = null; P(var f) { g = f; } } class F : P { F(int n) : super([n](int q) { return q + n; }) {} } F(5).g(1);", "6",
			     "a lambda capturing a parameter");
		});

		test("an_argument_is_captured_where_the_level_takes_it", [this]() {
			both("class P { array<int> a = []; P(array<int> x) { x.push(1); a = x; } } class Q : P { array<int> keep = []; Q(array<int> y) : super(y) { keep = y; } } "
			     "auto q = Q([0]); to_string(q.a.size()) + \" \" + to_string(q.keep.size());", "", "a by-value argument to the base");
		});

		test("constructions_repeat_with_their_own_parameters", [this]() {
			both("class B0 { int s = 0; B0(int v) { s = v; } } class S : B0 { S(int k) : super(k * k) {} } int t = 0; for (int i = 0; i < 200; ++i) { t += S(i).s; } t;", "2646700", "two hundred constructions");
		});
	}
};

}

using construction_scope_tests = jai::foundry::tests::construction_scope_tests;
FOUNDRY_REGISTER(construction_scope_tests)
