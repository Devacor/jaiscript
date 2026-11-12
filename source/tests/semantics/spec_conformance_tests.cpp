// Spec conformance: every expected value below is derived from the specification (the documentation under
// docs/site/: the guide, the embedding pages, the reference and docs/site/stability.html; the design notes under
// internal/; and C++ where the docs say C++ semantics), never from running a backend. Each group names the source
// of the claims it pins. Every row runs on the interpreter and the VM, with proofs on and off and the VM's passes off (and, in a
// build that defines JAISCRIPT_MISS_INJECTION, with injected cell misses), against one expected string.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/coroutine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

enum class spec_permission : std::int32_t { app_b = jai::permissions::count, app_a };

namespace spec {

// A host class whose accessors, methods and operators throw (docs/site/guide/05-control-flow.html "try / catch / throw": a C++ exception a bound
// function, getter or setter raises becomes the script's exception)
struct boundary_probe {
	int v = 1;
	int get_v() const { return v; }
	void set_v(int x) { if (x < 0) { throw std::invalid_argument("negative v"); } v = x; }
	int get_bad() const { throw std::runtime_error("getter failed"); }
	int poke(int x) { if (x == 0) { throw std::invalid_argument("poke zero"); } return x; }
	boundary_probe operator+(const boundary_probe& o) const { boundary_probe r; r.v = v + o.v; return r; }
	boundary_probe operator*(int k) const { if (k < 0) { throw std::domain_error("negative scale"); } boundary_probe r; r.v = v * k; return r; }
};

inline void bind_boundary_probe(engine& e) {
	dynamic_binder<boundary_probe>(e, "Probe")
		.constructor<>()
		.property("v", &boundary_probe::get_v, &boundary_probe::set_v)
		.property("bad", &boundary_probe::get_bad, nullptr)
		.property("lv", [](boundary_probe& p) { return p.v; }, [](boundary_probe& p, int x) { if (x > 100) { throw std::out_of_range("lv too big"); } p.v = x; })
		.method("poke", &boundary_probe::poke)
		.method("+", &boundary_probe::operator+)
		.method("*", &boundary_probe::operator*)
		.build();
}

struct run_config {
	bool vm = false;
	bool proofs = true;
	bool passes = true;
	uint32_t miss = 0;
	const char* label = "";
};

inline std::vector<run_config> configs() {
	std::vector<run_config> out = {
		{false, true, true, 0, "interp"},
		{false, false, true, 0, "interp proofs off"},
		{true, true, true, 0, "vm"},
		{true, false, true, 0, "vm proofs off"},
		{true, true, false, 0, "vm passes off"},
	};
	if (engine::miss_injection_available()) {
		out.push_back({true, true, true, 1, "vm miss 1"});
		out.push_back({true, true, true, 3, "vm miss 3"});
	}
	return out;
}

inline std::shared_ptr<engine> make(const run_config& c) {
	auto e = jai::foundry::make_default_engine();
	e->set_backend(c.vm ? backend_type::vm : backend_type::interpreter);
	stdlib::register_all(*e);
	if (!c.proofs) { e->type_proofs(false); }
	if (!c.passes) { e->ir_passes(false); }
	if (c.miss) { e->cells_miss_every(c.miss); }
	return e;
}

// A program the checker rejects, run for its run-time behavior: a [[warn_mode]] unit reports its errors as warnings and loads.
// The prefix shares the first line, so every line number stays.
inline std::string in_warn_region(const std::string& src) { return "[[warn_mode]]; " + src; }

inline std::string render(const script_value& v) {
	if (v.is_string()) { return v.as_string(); }
	if (v.is_null()) { return "<null>"; }
	return "<" + v.to_string() + ">";
}

// "i:3" / "f:3.9" / "s:x" - the host-side view of an execute result, type included
inline std::string typed_render(const script_value& v) {
	if (v.is_int()) { return "i:" + std::to_string(v.as_int()); }
	if (v.is_float()) { std::ostringstream os; os << v.as_float(); return "f:" + os.str(); }
	if (v.is_bool()) { return std::string("b:") + (v.as_bool() ? "true" : "false"); }
	if (v.is_string()) { return "s:" + v.as_string(); }
	if (v.is_null()) { return "null"; }
	return "?:" + v.to_string();
}

} // namespace spec

class spec_conformance_tests : public suite {
public:
	spec_conformance_tests() : suite("Spec Conformance") {}

	static constexpr bool warn_region = true;

	void every_config(const std::string& label, const std::string& expected, const std::function<std::string(engine&)>& body) {
		for (const auto& c : spec::configs()) {
			auto e = spec::make(c);
			std::string got;
			try { got = body(*e); }
			catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
			check_eq(expected, got, label + " [" + c.label + "]");
		}
	}

	void expect(const std::string& label, const std::string& src, const std::string& expected, bool lax = false) {
		const std::string unit = lax ? spec::in_warn_region(src) : src;
		every_config(label, expected, [&](engine& e) { return spec::render(e.execute(unit)); });
	}

	void forge_tests() override {
		// docs/site/guide/04-operators.html "Operator overloading": the left operand's operator runs when one of its overloads
		// takes the right operand, ahead of any builtin meaning (C++ overload resolution: a user operator beats the builtin)
		test("a_class_operator_that_takes_the_operand_beats_the_builtin", [this]() {
			expect("string right operands", R"(
				class S {
					string operator+(string s) { return "S+" + s; }
					int operator-(string s) { return 7; }
					int operator&(string s) { return 9; }
					int operator<=>(string s) { return -1; }
				}
				S s = S();
				int k = s - "x";
				s + "x" + "|" + to_string(k) + "|" + to_string(s & "x") + "|" + to_string(s <=> "x") + "|" + to_string(s < "x");
			)", "S+x|7|9|-1|true");
			expect("the checker types the operator's result", R"(
				class N { int operator+(string s) { return s.size(); } }
				int k = N() + "abcd";
				to_string(k);
			)", "4");
			expect("no overload takes the operand: the builtin meaning stands", R"(
				class W { W operator+(W o) { return W(); } string to_string() { return "W"; } }
				W w = W();
				(w + "!") + "|" + ("!" + w);
			)", "W!|!W");
			expect("an untyped parameter takes a string", R"(
				class A { int operator+(var o) { return 42; } }
				var a = A();
				to_string(A() + "s") + "|" + to_string(a + "s");
			)", "42|42");
			expect("compound and binary agree", R"(
				class S { string operator+(string s) { return "S+" + s; } }
				var t = S(); t += "y";
				t + "|" + (S() + "y");
			)", "S+y|S+y");
		});
		// docs/site/guide/03-value-semantics.html "Escape hatch 6": cast<T&>(h) is a reference to the object h holds (C++ *h), an
		// lvalue whose store copy-assigns that object, and a member of it binds by reference
		test("cast_reference_of_a_handle_names_the_shared_object", [this]() {
			expect("a store reaches every alias", R"(
				class C { int x = 1; }
				shared_ptr<C> h = new C(); shared_ptr<C> alias = h;
				C n = C(); n.x = 5;
				cast<C&>(h) = n;
				n.x = 6;
				to_string(alias.x) + "|" + to_string(h.x) + "|" + to_string(same_object(h, alias));
			)", "5|5|true");
			expect("a var holding a handle", R"(
				class C { int x = 1; }
				var w = new C(); var w2 = w;
				C m = C(); m.x = 7;
				cast<C&>(w) = m;
				to_string(w2.x) + "|" + to_string(same_object(w, w2));
			)", "7|true");
			expect("members bind by reference through the cast", R"(
				class C { int x = 1; }
				class D : C { int y = 2; }
				void bump(int& v) { v += 100; }
				shared_ptr<C> h = new C(); shared_ptr<C> a = h;
				bump(cast<C&>(h).x);
				auto& rx = cast<C&>(a).x; rx += 3;
				shared_ptr<C> hd = new D(); shared_ptr<C> ad = hd;
				auto& ry = cast<D&>(hd).y; ry = 41;
				string bad = "";
				try { shared_ptr<C> hc = new C(); auto& r = cast<D&>(hc).y; } catch (e) { bad = e; }
				to_string(h.x) + "|" + to_string(cast<D&>(ad).y) + "|" + bad;
			)", "104|41|Cannot cast 'C' to 'D&'");
			expect("a value variable keeps its copy assignment", R"(
				class C { int x = 1; }
				C v = C(); C& alias = v;
				C m = C(); m.x = 7;
				cast<C&>(v) = m;
				to_string(alias.x);
			)", "7");
		});
		// docs/site/guide/03-value-semantics.html: a call returning T& and a conditional of lvalues are lvalues, so an element
		// or member of them is one too (C++: getref()["a"] = 5); a by-value result stays a temporary
		test("an_element_of_an_lvalue_container_expression_is_an_lvalue", [this]() {
			expect("calls returning references", R"(
				map<string,int> m = {"a": 1};
				map<string,int>& getref() { return m; }
				array<int> arr = [1,2,3];
				array<int>& geta() { return arr; }
				array<array<int>> grid = [[1,2],[3,4]];
				array<array<int>>& gg() { return grid; }
				string s = "abc";
				string& gs() { return s; }
				getref()["a"] = 5; getref()["new"] = 3;
				geta()[0] = 7; geta()[1] += 10;
				gg()[1][0] = 30;
				gs()[0] = 'x';
				to_string(m["a"]) + "|" + to_string(m["new"]) + "|" + to_string(arr[0]) + "|" + to_string(arr[1]) + "|" +
					to_string(grid[1][0]) + "|" + s;
			)", "5|3|7|12|30|xbc");
			expect("a method returning a reference", R"(
				class Box { array<int> items = [0,0]; array<int>& all() { return items; } }
				Box b = Box();
				b.all()[1] = 9;
				to_string(b.items[1]);
			)", "9");
			expect("a conditional of lvalues", R"(
				class P { int x = 1; }
				array<int> a1 = [1]; array<int> a2 = [2];
				P o1 = P(); P o2 = P();
				bool pick = true;
				(pick ? a1 : a2)[0] = 50;
				(pick ? o1 : o2).x = 5;
				to_string(a1[0]) + "|" + to_string(o1.x);
			)", "50|5");
			expect("a by-value result stays a temporary", R"(
				array<int> arr = [1,2,3];
				array<int> byval() { return arr; }
				string r = "";
				try { byval()[0] = 7; } catch (e) { r = e; }
				r + "|" + to_string(arr[0]);
			)", "Cannot assign to rvalue expression|1", warn_region);   // the load's error where the checker judges it
		});
		// docs/site/guide/03-value-semantics.html and C++ [expr.ass]: built-in assignment to a member of a class prvalue is
		// ill-formed, so a store into a member of a class value no storage holds raises the math temporary's text
		test("a_member_store_into_a_class_temporary_raises", [this]() {
			expect("every temporary shape", R"(
				class P { int x = 1; P operator+(P o) { return P(); } }
				P make() { return P(); }
				function untyped() { return P(); }
				P keep = P();
				string t(function f) { string r = "stored"; try { f(); } catch (e) { r = e; } return r; }
				t([]() { make().x = 4; }) + "|" + t([]() { P().x = 4; }) + "|" + t([]() { make().x += 4; }) + "|" +
					t([]() { make().x++; }) + "|" + t([]() { (P() + P()).x = 4; }) + "|" + t([]() { untyped().x = 4; }) + "|" +
					t([]() { cast<P>(keep).x = 4; });
			)", "Cannot assign to a member of a temporary|Cannot assign to a member of a temporary|Cannot assign to a member of a temporary|"
			    "Cannot assign to a member of a temporary|Cannot assign to a member of a temporary|Cannot assign to a member of a temporary|"
			    "Cannot assign to a member of a temporary");
			expect("handles, references and storage store", R"(
				class P { int x = 1; }
				shared_ptr<P> mkh() { return new P(); }
				P keep = P();
				P& mkr() { return keep; }
				mkh().x = 4;
				(new P()).x = 4;
				mkr().x = 9;
				var v = P(); v.x = 3;
				to_string(keep.x) + "|" + to_string(v.x);
			)", "9|3");
		});
		// docs/site/guide/07-classes.html and 06-functions.html: a method redefining a base method says override (reported at load
		// under strict, raised when the declaration runs without the checker); a second definition of one signature in one program
		// text raises on both backends, while another execute redefining it is a hot reload
		test("override_markers_and_same_signature_definitions", [this]() {
			auto load_error = [](engine& e, const std::string& src) {
				try { e.execute(src); } catch (const static_check_error& ex) { return std::string(ex.what()); } catch (const std::exception& ex) { return std::string("runtime:") + ex.what(); }
				return std::string("loaded");
			};
			every_config("the checker reports override markers at load", "1|1|0", [&](engine& e) {
				const std::string missing = load_error(e, "class B { void f() {} }\nclass D : B { void f() {} }");
				const std::string unmatched = load_error(e, "class B2 { void f() {} }\nclass D2 : B2 { void g() override {} }");
				const std::string duplicate = load_error(e, "int g(int a) { return 1; }\nint g(int a) { return 2; }\nint r = g(1);");
				return std::to_string(missing.find("error: method 'f' redefines 'B::f': mark it 'override'") != std::string::npos) + "|" +
					std::to_string(unmatched.find("error: method 'g' marked as override but no base class method found") != std::string::npos) + "|" +
					std::to_string(duplicate.find("ambiguous") != std::string::npos);
			});
			auto raised = [](engine& e, const std::string& src) {
				try { e.execute(spec::in_warn_region(src)); } catch (const std::exception& ex) { return std::string(ex.what()); }
				return std::string("ran");
			};
			every_config("the run time raises the same verdicts", "Method 'f' redefines 'B::f': mark it 'override'|"
			             "Method 'g' marked as override but no base class method found|'g(int a)' is already defined (line 1)|"
			             "'m(int a)' is already defined (line 1)|'s(int a)' is already defined (line 1)", [&](engine& e) {
				return raised(e, "class B { void f() {} }\nclass D : B { void f() {} }") + "|" +
					raised(e, "class B2 { void f() {} }\nclass D2 : B2 { void g() override {} }") + "|" +
					raised(e, "int g(int a) { return 1; }\nint g(int a) { return 2; }") + "|" +
					raised(e, "class K { int m(int a) { return 1; } int m(int a) { return 2; } }") + "|" +
					raised(e, "class S { static int s(int a) { return 1; } static int s(int a) { return 2; } }");
			});
			every_config("another execute redefines (hot reload)", "2|2", [&](engine& e) {
				e.execute("int g(int a) { return 1; }\nclass K { int m(int a) { return 1; } }");
				e.execute("int g(int a) { return 2; }\nclass K { int m(int a) { return 2; } }");
				return std::to_string(e.execute("g(0)").as_int()) + "|" + std::to_string(e.execute("K().m(0)").as_int());
			});
		});
		// docs/site/reference/errors.html "Run-time errors": a store through null names the member and the receiver as the read
		// does; a store to a member a namespace lacks is the read's undefined name; a vector constructor names the shape it takes
		test("store_and_constructor_texts_name_what_failed", [this]() {
			expect("null receivers", R"(
				class V { int x = 1; }
				shared_ptr<V> v = null;
				var w = null;
				var nothing() { return null; }
				string t(function f) { string r = "stored"; try { f(); } catch (e) { r = e; } return r; }
				t([]() { w.x = 4; }) + "|" + t([]() { v.x = 4; }) + "|" + t([]() { nothing().x = 4; }) + "|" + t([]() { w.x += 4; });
			)", "Cannot assign to 'x' on null ('w' is null)|Cannot assign to 'x' on null ('v' is null)|"
			    "Cannot assign to 'x' on null ('nothing(...)' returned null)|Cannot access 'x' on null ('w' is null)", warn_region);
			expect("namespace members", R"(
				namespace ns { int x = 1; }
				string t(function f) { string r = "stored"; try { f(); } catch (e) { r = e; } return r; }
				t([]() { math::pi = 3.0; }) + "|" + t([]() { ns::y = 3; }) + "|" + t([]() { math::PI = 3.0; });
			)", "Undefined variable 'math::pi'|Undefined variable 'ns::y'|Cannot assign to 'PI': it is read-only", warn_region);
			expect("vector constructors", R"(
				string t(function f) { string r = "built"; try { f(); } catch (e) { r = e; } return r; }
				t([]() { math::vec3(5); }) + "|" + t([]() { math::vec3(math::vec2(1, 2)); }) + "|" + t([]() { math::vec4(math::vec2(1, 2), 3); }) + "|" +
					t([]() { math::color(5); }) + "|" + t([]() { math::vec2("a", 1); });
			)", "math::vec3 expects one vector of 3 components|math::vec3 expects one vector of 3 components|"
			    "math::vec4 expects a narrower vector followed by scalars that complete 4 components|math::color expects one color|"
			    "math::vec2 expects numeric components", warn_region);
			every_config("spellings from other languages", "1|1|1", [&](engine& e) {
				auto parse_error_of = [&](const std::string& src) {
					try { e.execute(src); } catch (const std::exception& ex) { return std::string(ex.what()); }
					return std::string("parsed");
				};
				const std::string lambda = parse_error_of("int n = 1; auto f = [n]() mutable { return n; };");
				const std::string dot = parse_error_of("class B { void m() {} }\nclass D : B { void m() override { super.m(); } }");
				const std::string call = parse_error_of("class B { void m() {} }\nclass D : B { void m() override { super(1); } }");
				return std::to_string(lambda.find("a lambda takes no 'mutable': its copied captures are already its own, writable and kept between calls") != std::string::npos &&
				                      lambda.find("expected expression") == std::string::npos) + "|" +
					std::to_string(dot.find("a base class method is called with super::name(args), not super.name") != std::string::npos) + "|" +
					std::to_string(call.find("super(...) constructs the base only in a constructor's initializer list (: super(args)); call a base method with super::name(args)") != std::string::npos);
			});
		});
		// docs/site/guide/09-containers-strings.html and 01-hello.html: a dot transform and its string:: twin raise one text; every
		// NaN prints nan (the language orders all NaNs as one group), whatever produced it and on every platform
		test("repeat_counts_and_nan_text", [this]() {
			expect("a negative repeat count", R"(
				string t(function f) { string r = "ran"; try { f(); } catch (e) { r = e; } return r; }
				string z = "ab"; z.repeat(0);
				t([]() { string s = "abc"; s.repeat(-1); }) + "|" + t([]() { auto r = string::repeat("a", -1); }) + "|" + string::repeat("ab", 0) + "|" + z + "|";
			)", "string::repeat count cannot be negative|string::repeat count cannot be negative|||");
			expect("nan text", R"(
				float inf = 1e308 * 10.0;
				float q = inf - inf;
				to_string(q) + "|" + to_string(-q) + "|" + to_string(math::sqrt(-1.0)) + "|" + to_string(0.0 * inf) + "|" + to_string(math::NAN_VALUE) + "|" +
					format("{:.2f}", q) + "|" + format("{:e}", -q) + "|" + to_string(math::vec2(q, 1.0)) + "|" + to_string(inf) + "|" + to_string(-inf);
			)", "nan|nan|nan|nan|nan|nan|nan|(nan, 1.0)|inf|-inf");
		});
		test("nested_member_regions_preserve_reads_stores_and_callbacks", [this]() {
			expect("nested scalar updates", R"(
				class Position { float x = 1.0; }
				class Item { Position pos = Position(); Position vel = Position(); }
				void update(array<Item>& items) { for (auto& item : items) { item.pos.x = item.pos.x + item.vel.x; } }
				array<Item> items = [Item(),Item()]; update(items); update(items);
				to_string(items[0].pos.x) + "|" + to_string(items[1].pos.x);
			)", "3.0|3.0");
			expect("prefix replaced by a callback", R"(
				class Position { int x = 1; }
				class Item { Position pos = Position(); }
				void replace(var& item) { item.pos = Position(); item.pos.x = 8; }
				int update(array<Item>& items) {
					int result = 0;
					for (auto& item : items) { item.pos.x = 2; replace(item); result += item.pos.x; }
					return result;
				}
				array<Item> items = [Item(),Item()]; to_string(update(items));
			)", "16");
			expect("prefix getters run for each access", R"(
				int reads = 0;
				class Position { int x = 1; }
				class Item { Position backing = Position(); Position _get_pos() { ++reads; return backing; } }
				void update(array<Item>& items) { for (auto& item : items) { item.pos.x = item.pos.x + 1; } }
				array<Item> items = [Item(),Item()]; update(items);
				to_string(reads) + "|" + to_string(items[0].backing.x);
			)", "4|1");
			expect("nested sites on a local whose class has the final name", R"(
				class Position { float x = 1.0; }
				class Item { float x = 100.0; Position pos = Position(); Position vel = Position(); }
				float run() {
					Item it = Item(); float total = 0.0;
					for (int i = 0; i < 4; ++i) { it.pos.x = it.pos.x + it.vel.x; total = total + it.pos.x; }
					return total + it.x;
				}
				to_string(run());
			)", "114.0");
			expect("prefix slots follow the element's class", R"(
				class Position { int x = 1; }
				class A { Position pos = Position(); Position vel = Position(); }
				class B { int pad = 0; Position vel = Position(); Position pos = Position(); }
				int update(array<var>& items) {
					int total = 0;
					for (var& item : items) { item.pos.x = item.pos.x + item.vel.x + 1; total += item.pos.x; }
					return total;
				}
				array<var> items = [A(), B(), A()]; items[1].vel.x = 5;
				to_string(update(items)) + "|" + to_string(update(items));
			)", "13|23");
			every_config("prefix slots after a class reload", "4|6|1", [&](engine& e) {
				e.execute(R"(
					class Position { int x = 1; }
					class Item { Position pos = Position(); Position vel = Position(); }
					int update(array<Item>& items) { int t = 0; for (auto& item : items) { item.pos.x = item.pos.x + item.vel.x; t += item.pos.x; } return t; }
					array<Item> items = [Item(), Item()]; int first = update(items);
				)");
				e.execute("class Item { Position vel = Position(); int extra = 0; Position pos = Position(); }");
				return e.execute("to_string(first) + \"|\" + to_string(update(items)) + \"|\" + to_string(items[0].vel.x);").as_string();
			});
			expect("nested vector component stores", R"(
				class Item { math::vec2 pos = math::vec2(1,2); }
				void update(array<Item>& items) { for (auto& item : items) { item.pos.x = item.pos.x + 2.0; } }
				array<Item> items = [Item()]; update(items); to_string(items[0].pos.x);
			)", "3.0");
		});
		test("local_member_reads_pin_getter_receivers", [this]() {
			expect("getter rebinds its source", R"(
				class Item {
					int n = 3;
					int _get_value() { current = null; return n; }
				}
				shared_ptr<Item> current = new Item();
				int read(shared_ptr<Item>& item) { return item.value; }
				int result = read(current);
				to_string(result) + "|" + to_string(current == null);
			)", "3|true");
			expect("cached fields remain live", R"(
				class Item { int n = 1; }
				int read(var& item) { return item.n; }
				Item item; int first = read(item); item.n = 7; int second = read(item);
				to_string(first) + "|" + to_string(second);
			)", "1|7");
		});
		test("typed_element_reads_observe_live_storage", [this]() {
			expect("string replacement", R"(
				string read(array<string>& items) {
					string& item = items[0]; string before = item;
					items[0] = "after"; items.push("extra");
					return before + "|" + item;
				}
				array<string> items = ["before"]; read(items);
			)", "before|after");
			expect("object replacement and identity", R"(
				class Item { int n = 1; }
				string read(array<Item>& items) {
					Item& item = items[0]; int before = item.n;
					items[0] = Item(); items[0].n = 7;
					item.n = 9;
					return to_string(before) + "|" + to_string(item.n) + "|" + to_string(items[0].n);
				}
				array<Item> items = [Item()]; read(items);
			)", "1|9|9");
			expect("scalar replacement and removal", R"(
				string read(array<int>& items) {
					int& item = items[0]; int before = item; items[0] = 7;
					string result = to_string(before) + "|" + to_string(item);
					items.clear();
					try { int removed = item; } catch (e) { result += "|" + e; }
					return result;
				}
				array<int> items = [1]; read(items);
			)", "1|7|Reference to a removed array element");
			// docs/site/guide/03-value-semantics.html: an element reference names an index, a map entry reference its key
			expect("an element reference names its index", R"(
				var a = [10, 20, 30]; auto& x = a[1]; a.insert(0, 99); x = 5;
				var b = [3, 1, 2]; auto& y = b[0]; b.sort(); y = 9;
				var m = {"a": 1, "b": 2}; auto& e = m["b"]; m["0"] = 0; m.erase("a"); e = 7;
				`${a}|${b}|${m}`;
			)", "[99, 5, 20, 30]|[9, 2, 3]|{\"0\": 0, \"b\": 7}");
		});
		test("inline_float_parse_preserves_fallback_and_text_contract", [this]() {
			expect("complete parse and fallback", R"(
				string good = " \t2.5\r\n"; string bad = "2.5suffix"; string huge = "1e9999";
				class A {} shared_ptr<A> fallback = new A();
				var value = bad.to_float(fallback);
				to_string(good.to_float()) + "|" + to_string(bad.to_float() == null) + "|" +
				to_string(huge.to_float(7)) + "|" + to_string(same_object(value, fallback)) + "|" +
				to_string(math::is_nan("NaN".to_float()));
			)", "2.5|true|7|true|true");
			expect("fallback arguments evaluate on successful parse", R"(
				int count = 0; int fallback() { count += 1; return 7; }
				string text = "3.5"; float parsed = text.to_float(fallback());
				to_string(parsed) + "|" + to_string(count);
			)", "3.5|1");
		});
		test("cached_scalar_field_stores_obey_retypes_and_accessors", [this]() {
			every_config("retype and accessor invalidate store cache", "int|float|2.5|7.0|70.0", [&](engine& e) {
				e.execute(R"(
					class A { int n = 0; }
					var object = new A();
					void put(var& target, var value) { target.n = value; }
					for (int i=0; i<4; ++i) { put(object, i); }
				)");
				std::string before = e.execute("type_of(object.n);").as_string();
				e.execute("class A { float n = 0.0; } put(object, 2.5);");
				std::string after = e.execute("type_of(object.n) + \"|\" + to_string(object.n);").as_string();
				e.execute(R"(
					class A { float n = 0.0; float stored = 0.0; float _get_n() { return stored / 10.0; } void _set_n(float value) { stored = value * 10.0; } }
					put(object, 7.0);
				)");
				return before + "|" + after + "|" + e.execute("to_string(object.n) + \"|\" + to_string(object.stored);").as_string();
			});
		});
		test("typed_handle_parameter_binding_preserves_identity_and_escape", [this]() {
			expect("by-value handle and escaping local alias", R"(
				class A { int n = 1; }
				int change(shared_ptr<A> value) {
					value.n += 1; var& alias = value; alias = new A(); alias.n = 9; return value.n;
				}
				shared_ptr<A> original = new A();
				int changed = change(original); to_string(changed) + "|" + to_string(original.n);
			)", "9|2");
			expect("typed null and static pointee rules", R"(
				class A { int n = 1; } class D : A {}
				shared_ptr<A> empty = null;
				shared_ptr<A> identity(shared_ptr<A> value) { return value; }
				int derived(shared_ptr<D> value) { return value.n; }
				shared_ptr<A> base = new D(); bool rejected = false;
				try { derived(base); } catch (e) { rejected = true; }
				to_string(identity(empty) == null) + "|" + to_string(rejected);
			)", "true|true", warn_region);
		});
		test("typed_class_element_aliases_keep_admission_and_store_constraints", [this]() {
			expect("incompatible class at a later binding", R"(
				class A { int n = 1; } class B { int n = 2; }
				array<var> items = [A(), B()]; int total = 0; bool rejected = false;
				function collect(array<var>& input) -> void {
					int size = input.size(); for (int i=0; i<size; ++i) { A& item = input[i]; total += item.n; }
				}
				try { collect(items); } catch (e) { rejected = e.contains("Cannot initialize"); }
				to_string(total) + "|" + to_string(rejected);
			)", "1|true");
			expect("derived class is admitted", R"(
				class A { int n = 1; } class D : A { D() { n = 3; } }
				array<var> items = [A(), D()]; int total = 0;
				for (int i=0; i<items.size(); ++i) { A& item = items[i]; total += item.n; }
				to_string(total);
			)", "4");
			expect("escaped reference keeps its declared class", R"(
				class A { int n = 1; } class B { int n = 2; }
				array<var> items = [A(), A()];
				function pick(array<var>& input) -> A& {
					int size = input.size(); for (int i=0; i<size; ++i) { A& item = input[i]; item.n += 1; if (i==1) { return item; } }
					return input[0];
				}
				var& item = pick(items); bool rejected = false;
				try { item = B(); } catch (e) { rejected = true; }
				to_string(rejected) + "|" + to_string(items[1].n);
			)", "true|2");
			expect("a later storage replacement does not replay admission", R"(
				class A { int n = 1; } class B { int n = 4; }
				array<var> items = [A()];
				void replace(array<var>& input) { input[0] = B(); }
				int collect(array<var>& input) {
					int total = 0;
					int size = input.size(); for (int i=0; i<size; ++i) { A& item = input[i]; int before = item.n; replace(input); total += item.n; }
					return total;
				}
				to_string(collect(items));
			)", "4");
			expect("materialized alias keeps the store constraint", R"(
				class A { int n = 1; } class B { int n = 2; }
				array<var> items = [A(), A()]; int count = 0;
				void replace(var& value) { value = B(); }
				void collect(array<var>& input) {
					int size = input.size(); for (int i=0; i<size; ++i) {
						A& item = input[i]; item.n += 1;
						try { replace(item); } catch (e) { count += 1; }
					}
				}
				collect(items); to_string(count) + "|" + to_string(items[0].n + items[1].n);
			)", "2|4");
		});
		test("api_consistency_byte_chars_whitespace_and_map_default", [this]() {
			expect("byte observers", R"(
				string s = "abc"; char a = s.at(0); char b = s.front(); char c = s.back();
				type_of(a) + "|" + type_of(b) + "|" + type_of(c) + "|" + to_string(s.at(-1) == s[2]);
			)", "char|char|char|true");
			expect("shared whitespace", R"(
				string s = " \x0b\x0c\tvalue\r\n "; string t = string::trim(s); s.trim();
				s + "|" + t + "|" + to_string(string::whitespace.contains("\x0b")) + "|" + to_json("a\x0bb\x0cc".split());
			)", "value|value|true|[\"a\",\"b\",\"c\"]");
			expect("get does not insert", R"(
				map<string,int> m = {"a":3}; to_string(m.get("a")) + "|" + to_string(m.get("missing") == null) + "|" + to_string(m.get("missing", 7)) + "|" + to_string(m.size());
			)", "3|true|7|1");
		});
		test("api_consistency_array_and_string_transform_pairs", [this]() {
			expect("array pairs", R"(
				array<int> a = [1,2,3]; array<int> selected = array::filter(a, x => x > 1);
				string before = to_json(a) + "|" + to_json(selected);
				a.filter(x => x > 1); a.append([4]); a.slice(1); a.map(x => x * 2);
				array<string> mapped = array::map(a, [](int x) -> string { return to_string(x); });
				before + "|" + to_json(a) + "|" + to_json(mapped) + "|" + to_json(array::reverse(a));
			)", "[1,2,3]|[2,3]|[6,8]|[\"6\",\"8\"]|[8,6]");
			expect("self append", "array<uint8> a = [1,2]; a.append(a); to_json(a);", "[1,2,1,2]");
			expect("string pair", R"(
				string a = "a"; string b = string::append(a,"b"); a.append("c"); a + "|" + b;
			)", "ac|ab");
			expect("typed map filter", R"(
				map<string,int> m = {"a":1,"b":2}; map<string,int> f = map::filter(m, [](string k,int& v) -> bool { v += 1; return v > 2; });
				map<string,int> g = {"a":1,"b":2}; g.filter([](string k,int& v) -> bool { v += 1; return v > 2; });
				to_json(m) + "|" + to_json(f) + "|" + to_json(g);
			)", "{\"a\":2,\"b\":3}|{\"b\":3}|{\"b\":3}");
		});
		// docs/site/guide/09-containers-strings.html "In place or a copy": a builtin that changes its receiver in place returns
		// nothing, so a call that uses its result raises before it changes anything (a chain included); a statement, a for's
		// update and a conditional's arms discard it, and a class's method of the same name returns what it declares
		test("api_consistency_in_place_builtin_results_cannot_be_used", [this]() {
			expect("on var receivers", R"(
				class Box { int filter(int x) { return x + 1; } string trim() { return "own"; } }
				var xs = [3, 1, 2];
				var s = " a ";
				var m = {"k": 1};
				var u = utf8_string("é");
				string out = "";
				try { var ys = xs.filter(x => x > 1); } catch (e) { out += `${e.message}|`; }
				try { var t = s.trim().to_upper(); } catch (e) { out += `${e.message}|`; }
				try { var n = m.filter((k, v) => false); } catch (e) { out += `${e.message}|`; }
				try { var w = u.append("x"); } catch (e) { out += `${e.message}|`; }
				try { var p = xs.push(4); } catch (e) { out += `${e.message}|`; }
				for (int i = 0; i < 1; xs.sort()) { i++; }
				true ? xs.reverse() : xs.sort();
				var none = null;
				none?.sort();
				var b = Box();
				`${out}${to_json(xs)}|[${s}]|${to_json(m)}|${to_string(u.size())}|${b.filter(1)}|${b.trim()}`;
			)", "filter changes xs in place and returns nothing; array::filter(xs, f) returns a filtered copy|"
			    "trim changes s in place and returns nothing; string::trim(s) returns a trimmed copy|"
			    "filter changes m in place and returns nothing; map::filter(m, f) returns a filtered copy|"
			    "append changes u in place and returns nothing; string::append(u, text) returns a joined copy|"
			    "push changes xs in place and returns nothing|[3,2,1]|[ a ]|{\"k\":1}|1|2|own");
			expect("typed receivers raise the load's text in a warn_mode region", R"(
				class Node { int v = 1; }
				array<int> xs = [1, 2];
				string name = "ada";
				shared_ptr<Node> p = new Node();
				string out = "";
				try { auto r = name.to_upper(); } catch (e) { out += `${e.message}|`; }
				try { auto r = xs.map(x => x * 2).size(); } catch (e) { out += `${e.message}|`; }
				try { auto r = p.reset(); } catch (e) { out += `${e.message}|`; }
				`${out}${name}|${to_json(xs)}|${to_string(p != null)}`;
			)", "to_upper changes name in place and returns nothing; string::to_upper(name) returns an uppercased copy|"
			    "map changes xs in place and returns nothing; array::map(xs, f) returns a mapped copy|"
			    "reset changes p in place and returns nothing|ada|[1,2]|true", warn_region);
			auto load_error = [](engine& e, const std::string& src) {
				try { e.execute(src); } catch (const static_check_error& ex) { return std::string(ex.what()); } catch (const std::exception& ex) { return std::string("runtime:") + ex.what(); }
				return std::string("loaded");
			};
			every_config("the checker reports a use on a known receiver at load", "1|1|1|1|1", [&](engine& e) {
				auto has = [](const std::string& text, const char* want) { return std::to_string(text.find(want) != std::string::npos); };
				return has(load_error(e, "array<int> xs = [1]; auto ys = xs.slice(0, 1);"), "slice changes xs in place and returns nothing; array::slice(xs, start, end) returns a copy of the range") + "|" +
				       has(load_error(e, "map<string, int> m = {}; int n = m.filter((k, v) => true).size();"), "filter changes m in place and returns nothing; map::filter(m, f) returns a filtered copy") + "|" +
				       has(load_error(e, "string s = \"a\"; string t = s.trim_left();"), "trim_left changes s in place and returns nothing") + "|" +
				       has(load_error(e, "class N {} weak_ptr<N> w; var r = w.reset();"), "reset changes w in place and returns nothing") + "|" +
				       has(load_error(e, "class Q { string s = \"\"; } Q q = Q(); print(q.s.repeat(2));"), "repeat changes q.s in place and returns nothing; string::repeat(q.s, n) returns a repeated copy");
			});
		});
		test("api_consistency_read_only_inputs_are_not_preemptively_cloned", [this]() {
			expect("nocopy selection", R"(
				nocopy class Item { int n = 1; }
				array<Item> items = [Item(), Item()];
				array<Item> empty = array::filter(items, [](Item& item) -> bool { return false; });
				array<int> mapped = array::map(items, [](Item& item) -> int { return item.n; });
				items.filter([](Item& item) -> bool { return true; }); items.slice(1);
				to_string(empty.size()) + "|" + to_json(mapped) + "|" + to_string(items.size());
			)", "0|[1,1]|1");
			expect("merge avoids overwritten copies and keeps constraints", R"(
				nocopy class Item { int n = 1; }
				map<string,var> left = {"x":Item()}; map<string,var> right = {"x":2};
				var replaced = merge(left,right);
				map<string,int> integers = {"a":1}; var flags = {"b":true}; auto joined = merge(integers,flags); bool rejects = false;
				try { joined["a"] = "bad"; } catch (e) { rejects = true; }
				var floats = {"c":2.5}; string refused = "";
				try { merge(integers,floats); } catch (e) { refused = e; }
				to_json(replaced) + "|" + to_json(joined) + "|" + to_string(rejects) + "|" + refused;
			)", "{\"x\":2}|{\"a\":1,\"b\":1}|true|Cannot store 'float' in map value of type 'int'");
			expect("handle projections", R"(
				class Item { int n = 1; }
				array<shared_ptr<Item>> a = [new Item(), null];
				array<shared_ptr<Item>> b = array::filter(a, x => x != null); b[0].n = 7;
				map<string,shared_ptr<Item>> m = {"x": a[0]}; m.filter((k,v) => true);
				to_string(same_object(a[0],b[0])) + "|" + to_string(same_object(a[0],m.get("x"))) + "|" + to_string(a[0].n);
			)", "true|true|7");
			expect("remove_if pins the original receiver", R"(
				var a = [1,2,3];
				int removed = a.remove_if([&a](int x) -> bool { a = []; return x == 2; });
				to_string(removed) + "|" + to_json(a);
			)", "1|[]");
			expect("remove_if preserves script throw", R"(
				var a = [1,2,3]; string error = "";
				try { a.remove_if([](int x) -> bool { throw "stop"; return false; }); }
				catch (e) { error = e; }
				error + "|" + to_json(a);
			)", "stop|[1,2,3]");
		});
		test("api_consistency_coroutine_arguments_are_validated", [this]() {
			expect("coroutine arity", R"(
				coroutine function produce() { yield 3; }
				var h = produce(); string errors = "";
				try { h.next(1, 2); } catch (e) { errors += e; }
				try { h.next("2"); } catch (e) { errors += "|" + e; }
				try { h.next(2.0); } catch (e) { errors += "|" + e; }
				try { h.next(0); } catch (e) { errors += "|" + e; }
				try { h.next(-3); } catch (e) { errors += "|" + e; }
				try { h.done(7); } catch (e) { errors += "|" + e; }
				errors + "|" + to_string(h.done()) + "|" + to_string(h.next());
			)", "next() takes 0 or 1 arguments ([count])|next() count must be an integer|next() count must be an integer|"
			    "next() count must be at least 1|next() count must be at least 1|done() takes no arguments|false|3");
		});
		test("api_consistency_standard_builtin_three_way_order", [this]() {
			expect("value orders", R"(
				array<int> a = [1,2]; array<int> b = [1,3];
				map<string,int> m = {"a":1}; map<string,int> n = {"a":2};
				to_string(a <=> b) + "|" + to_string(m <=> n) + "|" + to_string(false <=> true) + "|" + to_string(null <=> null) + "|" +
				to_string(math::vec2(1,2) <=> math::vec2(1,3)) + "|" + to_string(math::color(1,0,0) <=> math::color(1,1,0));
			)", "-1|-1|-1|0|-1|-1");
			expect("handle and callable identity", R"(
				class Item { int n = 1; }
				shared_ptr<Item> a = new Item(); shared_ptr<Item> b = a; weak_ptr<Item> wa = a; weak_ptr<Item> wb = b;
				auto f = math::sqrt; auto g = f;
				to_string(a <=> b) + "|" + to_string(wa <=> wb) + "|" + to_string(f <=> g) + "|" + to_string(f == g) + "|" + to_string(wa == wb);
			)", "0|0|0|true|true");
			expect("weak ownership keys remain distinct after expiry", R"(
				class Item { int n = 1; }
				shared_ptr<Item> a = new Item(); shared_ptr<Item> b = new Item(); weak_ptr<Item> wa = a; weak_ptr<Item> wb = b;
				map<weak_ptr<Item>,int> values = {}; values[wa] = 1; values[wb] = 2; a = null; b = null;
				to_string(values.size()) + "|" + to_string(values[wa]) + "|" + to_string(values[wb]) + "|" + to_string((wa <=> wb) != 0);
			)", "2|1|2|true");
			expect("comparison throw survives nested arrays", R"(
				class Item { int operator<=>(Item& other) { throw "cmp"; return 0; } }
				array<Item> a = [Item()]; array<Item> b = [Item()]; string failure = "";
				try { var result = a <=> b; } catch (e) { failure = e; } failure;
			)", "cmp");
			expect("float sort NaN group", R"(
				array<float> a = [math::NAN_VALUE, 2.0, -1.0]; a.sort();
				to_string(a[0]) + "|" + to_string(a[1]) + "|" + to_string(math::is_nan(a[2])) + "|" + to_string(math::NAN_VALUE <=> math::NAN_VALUE);
			)", "-1.0|2.0|true|0");
		});
		test("api_consistency_math_namespace_is_the_standard_api", [this]() {
			every_config("math namespace", "true|0.0|1.0|1.0|0.0|3|4.0", [](engine& e) {
				const bool noGlobals = !e.has_function("sqrt") && !e.has_function("floor") && !e.has_variable("PI");
				return std::string(noGlobals ? "true|" : "false|") + e.execute(R"(
					to_string(math::acos(1)) + "|" + to_string(math::exp(0)) + "|" + to_string(math::hypot(0,1)) + "|" + to_string(math::trunc(0.5)) + "|" + to_string(math::itrunc(3.7)) + "|" + to_string(math::sqrt(16));
				)").to_string();
			});
		});

		assignment_expression_values();
		declarations_and_constructors();
		by_value_returns_and_copies();
		memberwise_equality();
		map_keys();
		casts();
		catch_variables_and_switch();
		namespaces();
		utf8_caches();
		typed_containers();
		increments();
		parallel_locals();
		grammar_and_operators();
		chars();
		map_key_edges();
		scopes_and_units();
		references();
		namespace_forms();
		exceptions_more();
		closures();
		diagnostic_texts();
		callback_errors();
		user_assignment_operator();
		textless_errors();
		checker_verdicts();
		instance_variable_overlays();
		file_locations();
		permission_order();
		guide_examples();
		parallel_contracts();
		native_exceptions();
		access_control();
		private_constructors();
		lambda_globals_and_init_captures();
		compound_member_order();
		compound_subscript_order();
		auto_static_type();
		call_argument_capture();
		simplification_audit_fixes();
		library_and_edge_rules();
		comparisons_host_routes_and_class_scope();
		handle_field_admission();
		reentrant_destructors_and_operand_order();
		handle_admission();
		recursion_depth_parity();
#ifdef JAISCRIPT_SPEC_KNOWN_BUGS
		known_bugs();
#endif
	}

private:
	// docs/site/stability.html "Statements and expressions": an assignment expression's value is the value the destination stored
	void assignment_expression_values() {
		test("assignment_value_is_the_stored_value_for_every_destination_kind", [this]() {
			expect("stored value", R"(
				class A { int raw = 0; int _get_x() { return raw * 10; } void _set_x(int v) { raw = v; } }
				class C { float n = 0.0; static float s = 0.0; var set_bare() { return (n = 7); } var set_this() { return (this.n = 8); } }
				function run() -> string {
					float t = 0.0; C c = C(); A a = A();
					array<float> xs = [0.0]; map<string, float> m = {"k": 0.0};
					array<int> flags = [0];
					var r1 = (t = 3);
					var r2 = (c.n = 4);
					var r3 = (C::s = 5);
					var r4 = (a.x = 4);
					var r5 = (xs[0] = 2);
					var r6 = (m["k"] = 1);
					var r7 = c.set_bare();
					var r8 = c.set_this();
					var r9 = (flags[0] = true);
					return `${r1},${r2},${r3},${r4},${r5},${r6},${r7},${r8},${r9}|${type_of(r1)}${type_of(r5)}${type_of(r6)}${type_of(r9)}`;
				}
				run();
			)", "3.0,4.0,5.0,40,2.0,1.0,7.0,8.0,1|floatfloatfloatint");
		});
		test("compound_and_postfix_assignment_values_follow_the_destination", [this]() {
			// compound x op= y is x = T(x op y); postfix yields the old stored value
			expect("compound/postfix", R"(
				class C { float n = 1.5; }
				function run() -> string {
					float t = 1.0; C c = C(); array<int> xs = [5]; map<string, int> m = {"k": 5};
					var r1 = (t += 2);
					var r2 = (c.n *= 2);
					var r3 = xs[0]++;
					var r4 = m["k"]--;
					var r5 = ++xs[0];
					var r6 = (xs[0] -= 1);
					var half = 0.5; string refused = "";
					try { xs[0] -= half; } catch (e) { refused = e; }
					return `${r1},${r2},${r3},${r4},${r5},${r6}|${t},${c.n},${xs[0]},${m["k"]}|${type_of(r1)}${type_of(r2)}|${refused}`;
				}
				run();
			)", "3.0,3.0,5,5,7,6|3.0,3.0,6,4|floatfloat|Cannot assign 'float' to element of type 'int'");
		});
		test("setter_only_member_assignment_yields_its_right_hand_side", [this]() {
			// "A setter-only member (no field, no getter in the chain) yields its right-hand side"; the int parameter converts
			expect("setter only", R"(
				class W { var got = null; void _set_v(int x) { got = x; } }
				function run() -> string { W w = W(); var r = (w.v = true); return `${r}|${w.got}|${type_of(r)}`; }
				run();
			)", "true|1|bool");
		});
		test("top_level_implicit_result_is_never_read_back", [this]() {
			// a typed local or plain static store converts before producing anything; an explicit field, accessor, element or
			// map store hands the host its right-hand side
			struct row { const char* src; const char* expected; };
			const row rows[] = {
				{"int n = 0; n = true;", "i:1"},
				{"class S { static int s = 0; } S::s = true;", "i:1"},
				{"class C { int n = 0; } C c = C(); c.n = true;", "b:true"},
				{"array<int> a = [0]; a[0] = true;", "b:true"},
				{"map<string, int> m = {}; m[\"k\"] = true;", "b:true"},
				{"class A { int raw = 0; int _get_x() { return raw * 10; } void _set_x(int v) { raw = v; } } A a = A(); a.x = true;", "b:true"},
			};
			for (const auto& r : rows) {
				every_config(r.src, r.expected, [&](engine& e) { return spec::typed_render(e.execute(r.src)); });
			}
		});
		test("assignment_value_inside_a_ternary_in_a_leaf_called_from_a_counted_loop", [this]() {
			// element, typed local and field destinations, each read back as the stored float
			expect("ternary leaf", R"(
				array<float> xs = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0];
				class Acc { float v = 0.0; }
				Acc A = Acc();
				function leaf(int i) -> var { return i % 2 == 0 ? (xs[i] = i * 3) : (xs[i] = i - 1); }
				function leaf2(int i) -> var { float t = 0.0; return i > 2 ? (t = i * 2) : (t = -i * 2); }
				function leaf3(int i) -> var { return (A.v = i * 3); }
				function run() -> string {
					var acc = 0; var acc2 = 0; var acc3 = 0;
					for (int i = 0; i < 6; i++) { acc += leaf(i); acc2 += leaf2(i); acc3 += leaf3(i); }
					string s = "";
					for (int i = 0; i < 6; i++) { s += `${xs[i]},`; }
					return `${acc}|${s}|${acc2}|${acc3},${A.v}`;
				}
				run();
			)", "24.0|0.0,0.0,6.0,2.0,12.0,4.0,|18.0|45.0,15.0");
		});
	}

	// docs/site/guide/02-type-ladder.html "Declarations without an initializer" and docs/site/guide/07-classes.html
	// "Constructor exits and destructors", crossed with coroutines and delegating constructors
	void declarations_and_constructors() {
		test("defaulted_declarations_inside_a_coroutine_re_default_on_every_execution", [this]() {
			expect("coroutine decls", R"(
				class P { int x = 7; P() { x = 9; } }
				coroutine int gen() {
					for (int i = 0; i < 3; i++) { int k; string s; k += i * 10; s += "ab"; yield k + s.size(); }
					P p; p.x += 1; yield p.x;
					array<int> xs; xs.push(4); yield xs.size() + xs[0];
					int q{5}; yield q;
				}
				function run() -> string { string out = ""; for (auto v : gen()) { out += `${v},`; } return out; }
				run();
			)", "2,12,22,10,5,5,");
		});
		test("defaulted_class_fields_take_their_type_defaults", [this]() {
			expect("field defaults", R"(
				class Q { int a; string b; array<int> c; bool d; float f; var v; }
				function run() -> string { Q q = Q(); return `${q.a}|${q.b}|${q.c.size()}|${q.d}|${q.f}|${type_of(q.v)}`; }
				run();
			)", "0||0|false|0.0|null");
		});
		test("bare_return_in_a_delegated_constructor_ends_only_that_body", [this]() {
			// C++: a delegating constructor runs the whole target constructor (its base initializer included), then its own body
			expect("delegation", R"(
				class B { int b = 0; B(int x) { b = x; } }
				class D : B {
					int d = 0; string path = "";
					D(int x) : super(x) { path += "I"; if (x < 0) { return; } d = x * 10; path += "J"; }
					D() : this(-1) { path += "K"; d = 5; }
					D(string s) : this(7) { if (s == "x") { return; } path += "L"; }
				}
				function show(D v) -> string { return `${v.b},${v.d},${v.path}`; }
				show(D()) + "|" + show(D(3)) + "|" + show(D("x")) + "|" + show(D("y"));
			)", "-1,5,IK|3,30,IJ|7,70,IJ|7,70,IJL");
		});
		test("a_delegation_chain_runs_every_target_whole_and_a_cycle_raises", [this]() {
			// C++: D() -> D(int) -> D(int, int) : super(a + b); field initializers run once, in the innermost target;
			// a delegation cycle is ill-formed in C++ and raises the recursion text instead of recursing without end
			const std::string src = R"(
				class B { int b = 0; string log = ""; B(int x) { b = x; log += "B"; } }
				class D : B {
					int d = 1;
					D(int x, int y) : super(x + y) { log += "2"; d += 10; }
					D(int x) : this(x, 100) { log += "1"; d += 100; }
					D() : this(5) { log += "0"; d += 1000; }
				}
				class Loop { int v = 0; Loop() : this(1) { } Loop(int x) : this() { } }
				function run() -> string {
					D v = D();
					string err = "";
					try { var l = Loop(); } catch (e) { err = e; }
					return `${v.b},${v.d},${v.log}|${err}`;
				}
				run();
			)";
			// a Debug run to the depth limit takes seconds, and the default budget would cut it short on a loaded machine
			every_config("delegation chain", "105,1111,B210|Maximum recursion depth exceeded - possible infinite recursion", [&](engine& e) {
				e.execution_budget(0);
				return spec::render(e.execute(src));
			});
		});
		test("a_throw_in_a_delegated_constructor_keeps_the_target_and_runs_no_destructor", [this]() {
			expect("delegated throw", R"(
				var LOG = [];
				class R { int id = 0; R(int i) { id = i; if (i == 2) { throw "boom"; } } R() : this(2) { id = 99; } ~R() { LOG.push(id); } }
				function run() -> string { R keep = R(1); string err = ""; try { keep = R(); } catch (e) { err = e; } return `${keep.id}|${err}|${to_json(LOG)}`; }
				run();
			)", "1|boom|[]");
		});
	}

	// By-value returns copy, own locals move (docs/site/guide/03-value-semantics.html "The ambush: mutations on copies vanish silently"); aggregate copies copy values and share handles
	// (docs/site/stability.html "Value semantics")
	void by_value_returns_and_copies() {
		test("by_value_returns_never_alias_what_the_callee_could_name", [this]() {
			expect("returns", R"(
				var G = [1, 2, 3];
				class H { array<int> items = [1]; array<int> get() { return items; } }
				var rows = [[1], [2]];
				function fg() { return G; }
				function frow(int i) { return rows[i]; }
				function fref() -> auto& { return G; }
				function run() -> string {
					H h = H();
					fg().push(4); h.get().push(2); frow(0).push(9);
					fref().push(5);
					var loc = [7];
					var cl = [&]() { return loc; };
					cl().push(8);
					return `${G.size()},${h.items.size()},${rows[0].size()},${loc.size()}`;
				}
				run();
			)", "4,1,1,1", warn_region);   // h.get().push(2) changes a copy: the load's error where the checker judges it
		});
		test("a_by_value_return_of_an_aggregate_shares_its_shared_ptr_members", [this]() {
			expect("aggregate return", R"(
				class In { int x = 1; }
				class Out { shared_ptr<In> p = new In(); array<int> v = [1]; In w; }
				Out O = Out();
				function get() -> Out { return O; }
				function run() -> string {
					Out c = get(); c.p.x = 42; c.v.push(2); c.w.x = 5;
					get().p.x += 1; get().v.push(3);
					return `${O.p.x},${O.v.size()},${O.w.x},${to_string(same_object(c.p, O.p))},${to_string(c.p == O.p)}`;
				}
				run();
			)", "43,1,1,true,true");
		});
		test("returning_an_own_local_moves_it_without_an_extra_destructor", [this]() {
			expect("nrvo", R"(
				var TRACE = "";
				class T { int id = 0; T(int i) { id = i; } ~T() { TRACE += "d" + to_string(id); } }
				function make() -> T { T t(1); return t; }
				function make2() -> T { return T(2); }
				function run1() { T got = make(); TRACE += "|"; }
				function run2() { T got = make2(); TRACE += "|"; }
				run1(); run2();
				TRACE;
			)", "|d1|d2");
		});
		// A var or auto loop variable takes a handle element as var x = element; would: it stays a handle, so a copy of it shares
		// the object as a copy of the handle does (the loop stamped it var, and the copy copied the object)
		test("a_range_for_variable_keeps_a_handle_elements_type", [this]() {
			expect("loop handles", R"(
				class C { int x = 0; }
				var h = new C();
				for (var v : [h]) { var u = v; u.x = 5; }
				for (auto v : [h]) { var u = v; u.x += 1; }
				var arr = [h];
				for (var v : arr) { var u = v; u.x += 1; }
				function bump(var c) { c.x += 10; }
				for (var v : [h]) { bump(v); }
				`${h.x}`;
			)", "17", warn_region);
		});
		// A var loop variable takes a typed container element as var x = element; would: its value keeps the element type
		// while the slot stays a var (the loop stamped it a plain var, and a row of array<array<int>> took a string)
		test("a_range_for_var_variable_keeps_a_typed_containers_element_type", [this]() {
			expect("loop typed containers", R"(
				array<array<int>> grid = [[1]];
				array<map<string, int>> ms = [{"a": 1}];
				string out = "";
				for (var row : grid) { try { row.push("s"); out += "taken "; } catch (e) { out += "refused "; } row = "whole"; out += `${row} `; }
				for (var m : ms) { try { m["b"] = "s"; out += "taken "; } catch (e) { out += "refused "; } m = 3; out += `${m} `; }
				var direct = grid[0];
				try { direct.push("s"); out += "taken"; } catch (e) { out += "refused"; }
				out;
			)", "refused whole refused 3 refused");
		});
		// A conditional or ?? over an existing object yields that object, so a store from it copies as a store from the name does
		// (the declaration, the literal and the var-slot assignment took it as a temporary and aliased the source)
		test("a_store_from_a_conditional_over_an_existing_object_copies_it", [this]() {
			expect("conditional stores", R"(
				class C { int x = 0; }
				C c = C();
				C d = C();
				bool flag = true;
				var a = flag ? c : d; a.x = 1;
				C b = flag ? c : C(); b.x = 2;
				auto e = flag ? (flag ? c : d) : d; e.x = 3;
				var l = [flag ? c : d]; l[0].x = 4;
				var m = {"k": flag ? c : d}; m["k"].x = 5;
				var g = null; g = flag ? c : d; g.x = 6;
				var n = c ?? d; n.x = 7;
				function local() { C mine = C(); var t = flag ? mine : mine; t.x = 8; return mine.x; }
				`${c.x}${d.x}${local()}${a.x}${b.x}${e.x}${l[0].x}${m["k"].x}${g.x}${n.x}`;
			)", "0001234567");
		});
		// A yield hands out what a return of the same operand would: an existing object (a global, the coroutine's own local, an
		// element) copies, so the receiver never aliases it (the yielded value shared the source's storage)
		test("a_yield_hands_out_a_copy_of_an_existing_object", [this]() {
			expect("yield copies", R"(
				class C { int x = 0; }
				C c = C();
				string out = "";
				coroutine global_gen() { yield c; }
				var g = global_gen();
				var y1 = g.next(); y1.x = 7;
				out += `${c.x}`;
				coroutine local_gen() { C mine = C(); yield mine; out += `${mine.x}`; mine.x = 3; yield mine; }
				var l = local_gen();
				var y2 = l.next(); y2.x = 9;
				var y3 = l.next();
				out += `${y2.x}${y3.x}`;
				coroutine element_gen() { array<C> cs = [C()]; yield cs[0]; out += `${cs[0].x}`; }
				var el = element_gen();
				var y4 = el.next(); y4.x = 5; el.next();
				coroutine temp_gen() { yield C(); }
				var tg = temp_gen();
				var y5 = tg.next(); y5.x = 4;
				out += `${y5.x}`;
				out;
			)", "009304");
		});
		// A default argument binds as the argument it stands in for: a by-value parameter takes its converted copy (an existing object
		// copies, int p = 2.7 holds 2), a reference parameter binds the storage it names (the default was bound raw: aliased, unconverted,
		// and int& p = g wrote a private copy)
		test("a_default_argument_binds_as_the_argument_it_stands_in_for", [this]() {
			expect("defaults", R"(
				class C { int x = 0; }
				C c = C();
				array<C> cs = [C()];
				int g = 1;
				bool flag = true;
				function by_value(C p = c) { p.x = 7; }
				function untyped(var p = c) { p.x = 7; }
				function element(C p = cs[0]) { p.x = 7; }
				function conditional(C p = flag ? c : c) { p.x = 7; }
				class K { void m(C p = c) { p.x = 7; } }
				var lam = [](C p = c) { p.x = 7; };
				by_value(); untyped(); element(); conditional(); K().m(); lam();
				function converted(int p = true) { return p; }
				function widened(float p = 1) { return p; }
				function typed_row(array<int> p = [1, 2]) { return p; }
				var row = typed_row();
				string pushed = "taken";
				try { row.push("s"); } catch (e) { pushed = "refused"; }
				function bump(int& p = g) { p += 10; }
				bump();
				function touch(C& p = c) { p.x = 3; }
				touch();
				`${cs[0].x} ${converted()} ${widened()} ${pushed} ${g} ${c.x}`;
			)", "0 1 1.0 refused 11 3");
			every_config("a default the parameter refuses is a load error", "1|1", [](engine& e) {
				const auto has = [&](const char* src, const char* text) {
					const auto rep = e.check(src);
					for (const auto& d : rep.diagnostics) { if (d.severity == check_diagnostic::level::error && d.message.find(text) != std::string::npos) { return std::string("1"); } }
					return rep.format(src);
				};
				return has("function truncated(int p = 2.7) { return p; }", "default value of 'p' declared 'int' has type 'float'") + "|" +
				       has("class C { int x = 0; } function empty(C d = null) { return d; }", "default value of 'd' declared 'C' has type 'null'");
			});
		});
		// A conditional whose arms both name storage is the lvalue it picks (C++): a T& argument, an auto& or T& declaration, a T&
		// default, a -> T& return, a cast<T&> operand and an assignment target bind or write the chosen variable and never the other,
		// whichever arm is chosen; a by-value store, return or yield of it still copies, and a mixed arm stays a temporary
		test("a_conditional_of_lvalues_binds_as_a_reference", [this]() {
			expect("conditional lvalues", R"(
				class C { int x = 0; }
				string run(bool pick) {
					int g = 1;
					int h = 2;
					C c = C();
					C d = C();
					array<int> xs = [1, 2];
					void set(int& p, int v) { p = v; }
					set(pick ? g : h, 3);
					auto& r = pick ? g : h; r += 10;
					int& q = pick ? xs[0] : xs[1]; q = 7;
					void bump(C& p) { p.x += 5; }
					bump(pick ? c : d);
					cast<int&>(pick ? g : h) += 100;
					(pick ? c.x : d.x) = 9;
					return `${g},${h},${xs[0]},${xs[1]},${c.x},${d.x}`;
				}
				int G = 1;
				int H = 2;
				bool PICK = true;
				function pick_ref() -> int& { return PICK ? G : H; }
				void defaulted(int& p = PICK ? G : H) { p = 40; }
				pick_ref() = 30;
				PICK = false;
				defaulted();
				function by_value() -> int { return PICK ? G : H; }
				int copied = by_value(); copied = 0;
				var held = PICK ? G : H; held = 0;
				coroutine int gen() { yield PICK ? G : H; }
				var co = gen();
				int yielded = co.next(); yielded = 0;
				string mixed = "bound";
				try { int& m = PICK ? G : 5; m = 0; } catch (e) { mixed = "refused"; }
				`${run(true)} | ${run(false)} | ${G},${H} ${mixed}`;
			)", "113,2,7,2,9,0 | 1,113,1,7,0,9 | 30,40 refused", warn_region);
		});
		// A member arm may name no storage (an enumerator, a computed property): passed by value the conditional yields its value, and a
		// reference binds only the arm that names storage (the arm raised Cannot take reference of non-lvalue expression in cast<T&>)
		test("a_conditional_arm_that_names_no_storage_keeps_its_value", [this]() {
			expect("non-storage arms", R"(
				enum Fx { bones, blood }
				class P { int v = 3; int _get_twice() { return v * 2; } }
				P p = P();
				int show(int x) { return x; }
				void set9(int& r) { r = 9; }
				bool flag = true;
				string out = `${show(flag ? Fx.blood : Fx.bones)}${show(flag ? p.twice : p.v)}`;
				try { set9(flag ? p.twice : p.v); out += " bound"; } catch (e) { out += " refused"; }
				set9(!flag ? p.twice : p.v);
				`${out} ${p.v}`;
			)", "16 refused 9", warn_region);
		});
		// A range-for variable may be spelled with any type, template types included (shared_ptr<C>, weak_ptr<C>, array<int>,
		// map<string, int>, const and & forms): the loop parsed them as a for(;;) initializer
		test("a_range_for_variable_takes_every_type_spelling", [this]() {
			expect("template-typed loops", R"(
				class C { int x = 1; }
				shared_ptr<C> h = new C();
				array<shared_ptr<C>> hs = [h];
				for (shared_ptr<C> v : hs) { v.x = 5; }
				for (const shared_ptr<C>& v : hs) { v.x += 1; }
				weak_ptr<C> w = h;
				array<weak_ptr<C>> ws = [w];
				int alive = 0;
				for (weak_ptr<C> v : ws) { if (v.lock()) { alive += 1; } }
				array<array<int>> grid = [[1, 2], [3]];
				int cells = 0;
				for (array<int> row : grid) { cells += row.size(); row.push(9); }
				for (array<int>& row : grid) { row.push(7); }
				array<map<string, int>> ms = [{"a": 1}];
				for (map<string, int> m : ms) { [[warn_mode]] m["a"] = 50; }
				return `${h.x} ${alive} ${cells} ${grid[0].size()} ${ms[0]["a"]}`;
			)", "6 1 3 3 1");
		});
		test("aggregate_copies_copy_values_and_share_handles", [this]() {
			expect("aggregate copy", R"(
				class In { int x = 1; }
				class Box { shared_ptr<In> h = new In(); In v; array<shared_ptr<In>> hs = [new In()]; array<In> vs = [In()]; }
				function run() -> string {
					Box a = Box(); Box b = a;
					b.h.x = 5; b.v.x = 9; b.hs[0].x = 6; b.vs[0].x = 7;
					array<shared_ptr<In>> xs = [new In()]; var ys = xs; ys[0].x = 3;
					array<In> zs = [In()]; array<In> ws = zs; ws[0].x = 4;
					map<string, shared_ptr<In>> m = {"a": new In()}; var m2 = m; m2["a"].x = 8;
					return `${a.h.x},${a.v.x},${a.hs[0].x},${a.vs[0].x}|${xs[0].x},${zs[0].x},${m["a"].x}`;
				}
				run();
			)", "5,1,6,1|3,1,8");
		});
	}

	// docs/site/guide/04-operators.html "Equality": == is memberwise on heavy values
	void memberwise_equality() {
		test("memberwise_equality_with_user_operators_at_every_depth", [this]() {
			expect("memberwise", R"(
				class P { int x = 0; int y = 0; P(int a, int b) { x = a; y = b; } }
				class Q { int x = 0; int y = 0; Q(int a, int b) { x = a; y = b; } }
				class E { int v = 0; E(int a) { v = a; } bool operator==(E o) { return v % 10 == o.v % 10; } }
				class HasE { E e = E(0); array<E> es = []; }
				function run() -> string {
					string s = "";
					s += to_string([1, 2, 3] == [1, 2, 3]) + ",";
					s += to_string([1, 2] == [1, 2, 3]) + ",";
					s += to_string({"a": 1, "b": 2} == {"b": 2, "a": 1}) + ",";
					s += to_string({"a": 1} == {"a": 2}) + ",";
					s += to_string(P(1, 2) == P(1, 2)) + ",";
					s += to_string(P(1, 2) == P(1, 3)) + ",";
					var p = P(1, 2); var q = Q(1, 2);
					s += to_string(p == q) + ",";
					s += to_string([E(1), E(22)] == [E(11), E(2)]) + ",";
					HasE a = HasE(); HasE b = HasE(); a.e = E(3); b.e = E(13);
					s += to_string(a == b) + ",";
					a.es.push(E(4)); b.es.push(E(15));
					s += to_string(a == b) + ",";
					s += to_string({"k": [E(5)]} == {"k": [E(25)]});
					return s;
				}
				run();
			)", "true,false,true,false,true,false,false,true,true,false,true");
		});
		test("handles_compare_by_pointer_and_same_object_is_identity", [this]() {
			expect("handles", R"(
				class In { int x = 1; }
				function run() -> string {
					shared_ptr<In> p = new In(); shared_ptr<In> q = new In(); shared_ptr<In> r = p;
					var a = [1]; var b = a; var& c = a;
					return to_string(p == q) + "," + to_string(p == r) + "," + to_string([p] == [q]) + "," + to_string([p] == [r]) + "," +
						to_string(same_object(a, b)) + "," + to_string(same_object(a, c)) + "," + to_string(same_object(1, 1)) + "," +
						to_string(same_object("s", "s")) + "," + to_string(a == b);
				}
				run();
			)", "false,true,false,true,false,true,true,true,true");
		});
		test("builtin_containers_order_and_records_require_an_operator", [this]() {
			expect("ordering", R"(
				class P { int x = 0; }
				function run() -> string {
					string s = ""; var a = [1]; var b = [2]; var p = P(); var q = P();
					try { var r = a < b; s += "ordered,"; } catch (e) { s += "raised,"; }
					try { var r = p < q; s += "ordered"; } catch (e) { s += "raised"; }
					return s;
				}
				run();
			)", "ordered,raised");
		});
		test("memberwise_equality_and_order_at_any_depth", [this]() {
			expect("any depth", R"(
				function nest(int n, int leaf) { var a = leaf; for (int i = 0; i < n; i++) { a = [a]; } return a; }
				function run() -> string {
					string s = to_string(nest(150, 0) == nest(150, 0)) + ",";
					s += to_string(nest(300, 0) == nest(300, 0)) + "," + to_string(nest(300, 0) == nest(300, 1)) + ",";
					s += to_string(nest(300, 0) <=> nest(300, 1)) + "," + to_string(nest(300, 2) > nest(300, 1));
					return s;
				}
				run();
			)", "true,true,false,-1,true");
		});
	}

	// docs/site/guide/09-containers-strings.html "Key types": the rules and their texts
	void map_keys() {
		test("map_key_types_are_enforced_and_auto_keys_infer", [this]() {
			expect("keys", R"(
				function run() -> string {
					string s = "";
					map<string, int> m = {}; var k = 5;
					try { m[k] = 1; s += "stored"; } catch (e) { s += e; }
					s += "|";
					map<var, int> mv = {}; mv[5] = 1; mv["5"] = 2;
					s += `${mv.size()}|`;
					map<auto, int> ma = {5: 1}; var ks = "x";
					try { ma[ks] = 2; s += "stored"; } catch (e) { s += e; }
					s += "|";
					map<auto, int> me = {}; me[1] = 1; me["a"] = 2;
					s += `${me.size()}`;
					return s;
				}
				run();
			)", "Cannot use 'int' as a key of type 'string'|2|Cannot use 'string' as a key of type 'int'|2");
		});
		test("an_inferred_key_type_rejection_mid_loop_keeps_the_entries_stored_before_it", [this]() {
			expect("hot loop", R"(
				function run() -> string {
					map<auto, int> m = {0: 0};
					array<var> keys = [1, 2, 3, "four", 5];
					string err = ""; int i = 0;
					try { while (i < 5) { m[keys[i]] = i * i; i++; } } catch (e) { err = e; }
					int total = 0;
					for (auto kv : m) { total += kv.first * 100 + kv.second; }
					return `${m.size()}|${i}|${err}|${total}`;
				}
				run();
			)", "4|3|Cannot use 'string' as a key of type 'int'|605");
		});
		test("inferred_int_keys_accumulate_in_a_counted_loop", [this]() {
			std::string expected;
			for (int k = 0; k < 7; ++k) {
				int64_t sum = 0;
				for (int j = 0; j < 1000; ++j) { if (j % 7 == k) { sum += j; } }
				expected += std::to_string(sum) + ",";
			}
			expect("accumulate", R"(
				function run() -> string {
					map<auto, int> h = {0: 0, 1: 0, 2: 0, 3: 0, 4: 0, 5: 0, 6: 0};
					for (int j = 0; j < 1000; j++) { h[j % 7] += j; }
					string s = "";
					for (int k = 0; k < 7; k++) { s += `${h[k]},`; }
					return s;
				}
				run();
			)", expected);
		});
	}

	// docs/site/guide/02-type-ladder.html "cast<T>(e): the explicit boundary" (cast<T>, cast<T&>, cast<shared_ptr<T>>)
	void casts() {
		test("cast_reference_member_stores_through_a_base_typed_receiver", [this]() {
			expect("cast ref", R"(
				class Base { int b = 1; }
				class Der : Base { int t = 0; }
				class Holder { Base p = Der(); }
				function run() -> string {
					Holder h = Holder();
					cast<Der&>(h.p).t = 5;
					cast<Der&>(h.p).t += 2;
					var copy = cast<Der>(h.p); copy.t = 99;
					shared_ptr<Base> sp = new Der();
					cast<Der&>(sp).t = 7;
					shared_ptr<Der> sd = cast<shared_ptr<Der>>(sp); sd.t += 1;
					string err = "";
					Base plain = Base();
					try { cast<Der&>(plain).t = 1; } catch (e) { err = e; }
					int before = cast<Der&>(h.p).t;
					for (int i = 0; i < 100; i++) { cast<Der&>(h.p).t += i; }
					return `${before},${cast<Der&>(sp).t},${err},${cast<Der&>(h.p).t}`;
				}
				run();
			)", "7,8,Cannot cast 'Base' to 'Der&',4957");
		});
		test("null_passes_the_handle_cast_and_fails_the_value_and_reference_casts", [this]() {
			expect("cast null", R"(
				class Base { int b = 1; }
				class Der : Base { int t = 0; }
				function run() -> string {
					var n = null; string s = "";
					try { var d = cast<Der>(n); s += "cast,"; } catch (e) { s += `${e},`; }
					s += to_string(cast<shared_ptr<Der>>(n) == null) + ",";
					try { cast<Der&>(n).t = 1; s += "bound"; } catch (e) { s += "raised"; }
					return s;
				}
				run();
			)", "Cannot cast 'null' to 'Der',true,raised");
		});
	}

	// The catch variable is a block-scoped local; coroutines park the exception trio
	// and docs/site/guide/05-control-flow.html "switch: break by default, explicit fallthrough"
	void catch_variables_and_switch() {
		test("catch_variables_inside_switch_cases_with_fallthrough", [this]() {
			expect("switch catch", R"(
				function f(int k) -> string {
					string o = "";
					switch (k) {
						case 0:
							try { throw "A"; } catch (e) { o += e; }
							fallthrough;
						case 1:
							try { throw 7; } catch (e) { o += to_string(e.value + 1); }
							fallthrough;
						case 2:
							o += "c";
						default:
							o += "d";
					}
					return o;
				}
				f(0) + "|" + f(1) + "|" + f(2) + "|" + f(3);
			)", "A8c|8c|c|d");
		});
		test("a_catch_variable_is_invisible_after_its_block_and_to_callees", [this]() {
			expect("catch scope", R"(
				var e = "outer";
				function peek() -> string { return e; }
				function run() -> string {
					string o = "";
					try { throw "inner"; } catch (e) { o += e + "," + peek() + ","; }
					o += e;
					return o;
				}
				run();
			)", "inner,outer,outer");
		});
		test("a_catch_variable_read_after_its_block_raises_undefined_at_run_time", [this]() {
			// the strict checker rejects the read at load (C++ scope); warn mode reaches the runtime text
			expect("catch gone", R"(
				function gone() -> string { try { throw 1; } catch (err) { } return to_string(err); }
				string o = "";
				try { gone(); } catch (x) { o = x; }
				o;
			)", "Undefined variable 'err'", warn_region);
		});
		test("a_catch_variable_is_captured_and_survives_yields", [this]() {
			expect("catch capture", R"(
				coroutine int g() { try { throw 3; } catch (e) { yield e.value; var f = [=]() { return e.value * 2; }; yield f(); yield e.value + 1; } }
				function mk() { var f = null; try { throw 5; } catch (e) { f = [&]() { return e.value * 2; }; } return f; }
				function run() -> string { string o = ""; for (auto v : g()) { o += `${v},`; } var f = mk(); o += `${f()}`; return o; }
				run();
			)", "3,6,4,10");
		});
	}

	void namespaces() {
		test("namespace_document_example", [this]() {
			expect("geometry", R"(
				namespace geometry {
					var scale = 2;
					int calls = 0;
					function area(w, h) { calls += 1; return w * h * scale; }
					class Rect { int w = 1; int h = 1; }
				}
				var a1 = geometry::area(3, 4);
				geometry::scale = 10;
				var a2 = geometry::area(3, 4);
				string bare = "";
				try { var r = Rect(); } catch (e) { bare = e; }
				`${a1}|${a2}|${geometry::calls}|${geometry::Rect().w}${geometry::Rect().h}|${bare}`;
			)", "24|120|2|11|Undefined variable 'Rect'", warn_region);
		});
		test("a_namespace_class_is_named_by_its_qualified_name", [this]() {
			// C++: a::K, b::K and a global K are three classes. Inside a namespace's blocks the bare name reads as the innermost
			// namespace's class (a reopened namespace too, in a later unit); a redeclaration reloads only its own class
			every_config("qualified class identity", "1271|a::K b::K K a::K|3 outer::inner::D 3|11 2 7|11 2", [&](engine& e) {
				e.execute(R"(
					namespace a { class K { int v = 1; int who() { return 1; } K self() { return K(); } } K build() { return K(); } }
					namespace b { class K { string s = "b"; int who() { return 2; } } }
					class K { int g = 7; }
					namespace outer { class P { int p = 3; } namespace inner { int read() { return P().p; } class D : P { D() {} ~D() {} } } }
					var x = a::K(); var y = b::K(); K z;
				)");
				e.execute("namespace a { int again() { return K().who(); } }");
				std::string out = spec::render(e.execute(R"(
					`${x.who()}${y.who()}${z.g}${a::again()}|${type_of(x)} ${type_of(y)} ${type_of(z)} ${type_of(a::build().self())}|${outer::inner::read()} ${type_of(outer::inner::D())} ${outer::inner::D().p}`;
				)"));
				e.execute("namespace a { class K { int v = 1; int who() { return 11; } } }");
				out += "|" + spec::render(e.execute(R"(`${x.who()} ${y.who()} ${z.g}`;)"));
				return out + "|" + spec::render(e.execute(R"(`${a::K().who()} ${b::K().who()}`;)"));
			});
		});
		test("namespace_functions_write_the_namespace_cell_by_bare_name", [this]() {
			expect("bare name", R"(
				namespace ns { var x = 1; function set(v) { x = v; } function get() { return x; } }
				namespace ns2 { var x = 3; var y = x + 1; }
				var x = 100;
				ns::set(5);
				namespace ns { var x = 6; }
				`${ns::x}|${x}|${ns2::y}|${ns::get()}`;
			)", "6|100|4|6");
		});
		test("namespace_typed_variables_store_through_the_assignment_table", [this]() {
			expect("typed ns", R"(
				namespace n4 { int y = 0; }
				function run() -> string {
					string s = ""; var str = "str";
					try { n4::y = str; } catch (e) { s += `${e}|`; }
					var half = 2.9;
					try { n4::y = half; } catch (e) { s += `${e}|`; }
					var r = (n4::y = true);
					n4::y += 2; n4::y++;
					return `${s}${type_of(r)}:${r}|${n4::y}`;
				}
				run();
			)", "Cannot assign string to 'y' (declared int)|Cannot assign float to 'y' (declared int)|int:1|4");
		});
		test("a_qualified_namespace_variable_binds_a_reference", [this]() {
			expect("ns ref", R"(
				namespace n5 { var x = 1; }
				function take(var& r) { r = 9; }
				function run() -> string {
					string s = "";
					try { take(n5::x); s += "bound"; } catch (e) { s += e; }
					return `${s}|${n5::x}`;
				}
				run();
			)", "bound|9");
		});
		test("a_namespace_function_does_not_see_the_callers_locals", [this]() {
			expect("ns env", R"(
				namespace n6 { function f() { return q; } }
				var q = "global";
				function g() { var q = "local"; return n6::f(); }
				g();
			)", "global");
		});
		test("a_units_program_scope_variable_never_touches_the_namespace_cell", [this]() {
			every_config("ns units", "7|50|7", [](engine& e) {
				e.execute("namespace n7 { var x = 1; function get() { return x; } }");
				e.execute("var x = 50; n7::x = 7;");
				return e.execute("`${n7::x}|${x}|${n7::get()}`").as_string();
			});
		});
	}

	// docs/site/guide/09-containers-strings.html "utf8_string: text by character": characters are counted, and the
	// character caches clear on every write, so no read sees a stale count or position
	void utf8_caches() {
		test("utf8_caches_follow_writes_through_references_parameters_fields_and_copies", [this]() {
			// e = e-acute, j = the two CJK characters, o = omega, u = u-umlaut
			const std::string E = "\xC3\xA9", J1 = "\xE6\x97\xA5", J2 = "\xE6\x9C\xAC", J3 = "\xE8\xAA\x9E", O = "\xCE\xA9", U = "\xC3\xBC";
			const std::string src =
				"function ed(utf8_string& t) { t.erase(0, 1); }\n"
				"class H { utf8_string t = \"" + U + "\"; }\n"
				"function run() -> string {\n"
				"  utf8_string u = \"a" + E + J1 + "b\";\n"
				"  int s1 = u.size(); var c2 = u[2];\n"
				"  auto& r = u; r.insert(1, \"" + O + "\");\n"
				"  string o = `${s1},${c2},${u.size()},${u[2]},${u[3]}`;\n"
				"  ed(u);\n"
				"  o += `|${u.size()},${u[0]},${u.find(\"" + J1 + "\")}`;\n"
				"  H h = H(); int hs = h.t.size(); h.t += \"" + J1 + J2 + "\"; o += `|${hs},${h.t.size()},${h.t[2]}`;\n"
				"  utf8_string a = \"" + J1 + J2 + J3 + "\"; var b = a; int as = a.size(); b.insert(0, \"x\");\n"
				"  o += `|${as},${a.size()},${b.size()},${a[0]},${b[1]}`;\n"
				"  return o;\n"
				"}\n"
				"run();\n";
			expect("utf8 caches", src, "4," + J1 + ",5," + E + "," + J1 + "|4," + O + ",2|1,3," + J2 + "|3,3,4," + J1 + "," + J1);
		});
		test("utf8_breadcrumbs_follow_an_insert_through_a_reference", [this]() {
			const std::string E = "\xC3\xA9", J1 = "\xE6\x97\xA5";
			const std::string src =
				"function run() -> string {\n"
				"  string s = \"\"; for (int i = 0; i < 100; i++) { s += \"a" + E + J1 + "\"; }\n"
				"  utf8_string u = s; var before = u[200]; var last = u[299]; int n = u.size();\n"
				"  var& r = u; r.insert(0, \"" + E + "\");\n"
				"  return `${n},${u.size()},${u[201] == before},${u[300] == last},${u[0] == \"" + E + "\"},${u[1] == \"a\"}`;\n"
				"}\n"
				"run();\n";
			expect("breadcrumbs", src, "300,301,true,true,true,true");
		});
	}

	// docs/site/guide/09-containers-strings.html "Typed containers" (representation follows the declared type, element
	// conversions); handle-typed elements take null (docs/site/stability.html "Typed containers")
	void typed_containers() {
		test("element_boundaries_on_every_typed_container", [this]() {
			expect("elements", R"(
				class In { int x = 1; }
				function run() -> string {
					string s = "";
					array<array<int>> g = [[1], [2]]; var row = [2]; g.push(row); s += `${g[2][0]},`;
					map<string, array<int>> m = {}; m["a"] = [true, 2]; s += `${m["a"][0]},`;
					var floats = [2.9];
					try { g.push(floats); } catch (e) { s += `${e},`; }
					try { m["a"] = floats; } catch (e) { s += `${e},`; }
					array<int> ai = []; var str = "x";
					try { ai.push(str); } catch (e) { s += `${e},`; }
					array<string> as = []; var one = 1;
					try { as.push(one); } catch (e) { s += `${e},`; }
					array<bool> ab = []; ab.push(one); ai.push(true); s += `${to_string(ab[0])},${ai[0]},`;
					array<shared_ptr<In>> hs = [null]; hs.push(null); s += `${hs.size()},`;
					array<In> vs = []; var n = null;
					try { array<In> lit = [In(), n]; } catch (e) { s += `${e},`; }
					try { vs.push(n); } catch (e) { s += `${e},`; }
					var box = In();
					try { ai.push(box); } catch (e) { s += e; }
					return s;
				}
				run();
			)", "2,1,Cannot store float in array<int>,Cannot store float in array<int>,Cannot push 'string' to array<int>,Cannot push 'int' to array<string>,true,1,2,Cannot store null in array<In>,Cannot push 'null' to array<In>,Cannot push 'In' to array<int>");
		});
	}

	// docs/site/reference/rules.html "Operators": ++/-- work on every lvalue
	void increments() {
		test("increments_on_members_elements_map_entries_and_nested_lvalues", [this]() {
			expect("incdec", R"(
				class C { int n = 1; array<int> a = [1, 2]; map<string, int> m = {"k": 3}; void bump() { this.n++; ++a[1]; m["k"]--; } }
				function run() -> string {
					C c = C(); c.bump(); var r = c.a[0]++;
					var g = [[1, 2], [3]]; g[1][0]++; ++g[0][1];
					return `${c.n}${c.a[0]}${c.a[1]}${c.m["k"]}${r}|${(g[0][1] + g[1][0])}`;
				}
				run();
			)", "22321|7");
		});
	}

	// parallel bodies see the enclosing function's locals
	void parallel_locals() {
		test("a_parallel_body_reads_the_enclosing_frames_local_over_a_same_named_global", [this]() {
			expect("parallel locals", R"(
				var k = 100;
				function leaf(int a) -> int { return a + 1; }
				function f() -> string {
					int k = 3; array<int> xs = [1, 2, 3, 4]; array<int> ys = [1, 2, 3, 4];
					parallel_for (auto& x : xs) { x = x * k; }
					parallel_for (auto& y : ys) { y = leaf(y) * k; }
					return `${xs[0]},${xs[1]},${xs[2]},${xs[3]}|${ys[0]},${ys[1]},${ys[2]},${ys[3]}`;
				}
				f();
			)", "3,6,9,12|6,9,12,15");
		});
	}

	// The for statement and the compound assignments, docs/site/embedding/05-safety-rails.html
	// "Checked integer overflow"
	void grammar_and_operators() {
		test("a_for_initializer_may_be_an_expression", [this]() {
			// forStatement = "for" "(" (variableDeclaration | expression)? ";" ...
			expect("for init", R"(
				class P { int x = 0; P(int v) { x = v; } }
				function run() -> string {
					int i = 0; int s = 0;
					for (i = 0; i < 3; i++) { s += i; }
					var o = [0];
					for (o[0] = 1; o[0] < 3; o[0]++) { s += 10; }
					int j = 5;
					for (j += 1; j < 8; j++) { s += 100; }
					for (++j; j < 11; j++) { s += 1000; }
					for (P p = P(2); p.x < 4; p.x++) { s += 10000; }
					return `${i},${s},${o[0]},${j}`;
				}
				run();
			)", "3,22223,3,11");
		});
		test("binary_minus_overflow_follows_the_build_policy", [this]() {
			auto probe = jai::foundry::make_default_engine();
			const std::string expected = probe->throw_on_overflow() ? "Integer overflow in '-'" : "9223372036854775807";
			expect("minus overflow", R"(
				var a = -9223372036854775807 - 1; var b = 1; string m = "";
				try { var r = a - b; m = `${r}`; } catch (e) { m = e; }
				m;
			)", expected);
		});
		test("a_literal_without_an_element_type_held_by_var_is_the_dynamic_form", [this]() {
			expect("var literal", "var v = [1, 2]; v.push(\"s\"); `${v.size()}`;", "3");
		});
	}

	// docs/site/guide/04-operators.html "Chars promote like C++"
	void chars() {
		test("char_promotion_in_division_bitwise_and_map_keys", [this]() {
			expect("chars", R"(
				class C { string got = ""; C operator+(var x) { got = type_of(x); return this; } }
				function run() -> string {
					string s = "";
					s += `${('d' / 2)},${('a' / 2.0)},`;
					s += `${('a' | 1)},${('a' ^ ' ')},${('b' >> 1)},`;
					var m = {}; m['a'] = 1; m['b'] = 2;
					s += `${(m['a'] + m.size())},${to_string(m.has('a'))},`;
					var u = {}; u['\xC3'] = 1; u['a'] = 2;
					s += to_string(u.keys()[0] == 'a') + ",";
					var c = C(); var r = c + 'a';
					s += c.got + ",";
					var k = {}; k[98] = 1; k['a'] = 2; k[1] = 3;
					string order = ""; for (auto kv : k) { order += type_of(kv.first) + " "; }
					s += to_string(order == "int int char " || order == "char int int ");
					return s;
				}
				run();
			)", "50,48.5,97,65,49,3,true,true,char,true");
		});
		test("math_intrinsics_are_numeric_only_and_char_is_not_an_int_slot", [this]() {
			expect("char edges", R"(
				function run() -> string {
					string s = "";
					try { var r = math::abs('a'); s += "ok,"; } catch (e) { s += "raised,"; }
					try { var r = math::wrap_add('a', 1); s += "ok,"; } catch (e) { s += "raised,"; }
					var v = 'c';
					try { int x = v; s += "ok"; } catch (e) { s += e; }
					return s;
				}
				run();
			)", "raised,raised,Cannot initialize 'x' (declared int) with char");
		});
	}

	// docs/site/guide/09-containers-strings.html "Key types": one total key order incl. NaN, equivalent keys hash alike;
	// complex keys order and hash by the identity of the object they hold
	void map_key_edges() {
		test("a_nan_key_is_storable_and_findable", [this]() {
			every_config("nan key", "12", [](engine& e) {
				e.add_global("nan", script_value(std::numeric_limits<double>::quiet_NaN(), &e));
				return e.execute("var m = {}; m[nan] = 1; m[nan] = 2; `${(m.size() * 10 + m[nan])}`;").as_string();
			});
		});
		test("equal_content_arrays_are_distinct_keys_and_cpp_equality_is_identity", [this]() {
			every_config("identity keys", "2|false|true", [](engine& e) {
				std::string out = e.execute("var a = [1]; var b = [1]; var m = {}; m[a] = 1; m[b] = 2; `${m.size()}`;").as_string();
				const script_value x = e.execute("[1];");
				const script_value y = e.execute("[1];");
				out += std::string("|") + (x == y ? "true" : "false");
				out += std::string("|") + (e.execute("[1] == [1];").as_bool() ? "true" : "false");
				return out;
			});
		});
	}

	// program-scope redeclarations shadow by unit and {} blocks scope as in C++
	void scopes_and_units() {
		test("block_scopes_in_loops_tries_and_bare_blocks", [this]() {
			expect("blocks", R"(
				var x = 1;
				for (int i = 0; i < 2; i++) { var x = 10 + i; }
				function f() -> int { int x = 1; for (int i = 0; i < 2; i++) { int x = 10 + i; } return x; }
				var r = 0;
				try { var x = 2; throw 1; } catch (e) { r = x; }
				var G = 1;
				function g() { return G; }
				function h() -> int { var rg = 0; { var G = 5; rg = g(); } return rg; }
				var e = "g"; string rc = "";
				try { throw "x"; } catch (e) { rc = ::e + e; }
				`${x}${f()}|${r}${x}|${h()}${G}|${rc}`;
			)", "11|11|11|gx");
		});
		test("default_arguments_constructors_and_namespace_functions_resolve_in_their_declaring_unit", [this]() {
			every_config("declaring unit", "1|1|1|5", [](engine& e) {
				e.execute("var D = 1; function f(int a = D) { return a; } class C { int v = 0; C() { v = D; } } namespace ns { function h() { return D; } } function k() { return D; }");
				e.execute("int D = 5;");
				e.execute("function k() { return D; }");
				return e.execute("`${f()}|${C().v}|${ns::h()}|${k()}`;").as_string();
			});
		});
		test("a_host_callback_and_a_parallel_region_resolve_as_the_code_that_runs_them", [this]() {
			every_config("callback unit", "1|1", [](engine& e) {
				engine* raw = &e;
				e.add_function("peek", [raw]() -> script_value { return raw->get_variable("G"); });
				e.execute("var G = 1; function f() { return peek(); } function fill(var& a) { parallel_for (auto& x : a) { x = G; } }");
				e.execute("int G = 5;");
				return e.execute("var A = [0, 0]; fill(A); `${f()}|${A[0]}`;").as_string();
			});
		});
		test("an_import_is_its_own_unit_and_an_include_is_the_includers", [this]() {
			const auto dir = jai::foundry::test_temp_directory() / "spec_conformance_units";
			std::filesystem::create_directories(dir);
			std::ofstream(dir / "lib.jai") << "var X = 1; function getx() { return X; }\n";
			every_config("import unit", "1|2", [&](engine& e) {
				e.add_include_path(dir.string());
				return e.execute("import \"lib.jai\"; var X = 2; `${getx()}|${X}`;").as_string();
			});
			every_config("include unit", "2|2", [&](engine& e) {
				e.add_include_path(dir.string());
				return e.execute("include \"lib.jai\"; var X = 2; `${getx()}|${X}`;").as_string();
			});
		});
		test("engines_share_no_script_state", [this]() {
			every_config("two engines", "Undefined variable 'g'", [](engine& e) {
				e.execute("var g = 1;");
				auto other = jai::foundry::make_default_engine();
				other->set_backend(e.get_backend_type());
				return other->execute(spec::in_warn_region("var r = \"\"; try { r = `${g}`; } catch (x) { r = x; } r;")).as_string();
			});
		});
	}

	// docs/site/guide/03-value-semantics.html (returns, reference declarations)
	void references() {
		test("reference_model_boundaries", [this]() {
			expect("refs", R"(
				int G = 1;
				function g() -> int& { return G; }
				function f() -> int& { return g(); }
				var log = "";
				function i1() -> int { log += "a"; return 0; }
				function i2() -> int { log += "b"; return 1; }
				function two(int& p, int& q) { }
				function typed_name() -> string { int x = 1; int& r = x; var s = "s"; string m = ""; try { x = s; } catch (e) { m = e; } var f = 2.7; try { r = f; } catch (e) { m += "|" + e; } x = true; return m + "|" + to_string(x); }
				function run() -> string {
					auto& r = f(); r = 5;
					var A = [1, 2]; two(A[i1()], A[i2()]);
					int a = 1; auto& ra = a; int x = 0; [[warn_mode]] x = ra; [[warn_mode]] x = 5;
					var arr = [1]; auto& re = arr[0]; var m = {}; m["k"] = re; m["k"] = 9; re = 3;
					map<string, int> mm = {"k": 1}; var s = "x"; string d = ""; string o = "";
					try { mm["k"] = s; } catch (e) { d = e; }
					var& rm = mm["k"];
					try { rm = s; } catch (e) { o = e; }
					return `${G}|${log}|${a}|${arr[0]}${m["k"]}|${to_string(o == d && o != "")}${mm["k"]}|${typed_name()}`;
				}
				run();
			)", "5|ab|1|39|true1|Cannot assign string to 'x' (declared int)|Cannot assign float to 'r' (declared int&)|1");
		});
		test("a_reference_held_across_yields_follows_its_element", [this]() {
			expect("ref across yield", R"(
				array<int> A = [1];
				coroutine int co() { auto& r = A[0]; yield 0; r = 7; yield 1; }
				var h = co(); h.next(); A.push(2); h.next();
				`${A[0]}`;
			)", "7");
		});
		test("coroutine_handles_returned_by_value_share", [this]() {
			expect("handle return", R"(
				coroutine int co() { yield 1; yield 2; }
				var H = co();
				function get() { return H; }
				get().next();
				`${H.next()}`;
			)", "2");
		});
		// a const reference holds the temporary instead (C++ lifetime extension)
		test("a_reference_to_a_by_value_call_result_binds_only_when_const", [this]() {
			expect("ref to rvalue", R"(
				function fv() { return [1]; }
				string r = "";
				try { var& p = fv(); r = "bound"; } catch (e) { r = e.starts_with("Cannot take reference of non-lvalue expression") ? "raised" : e; }
				const auto& c = fv();
				const var& v = fv();
				`${r}|${c.size()}${v[0]}`;
			)", "raised|11", warn_region);
			// a reference names a variable from its declaration on (C++): one without an initializer never loads, warn region or not
			for (const char* src : {"void f() { int& r; print(\"x\"); } f();", "[[warn_mode]]; const var& r;", "namespace ns { int& r; }"}) {
				auto e = make_engine();
				std::string err;
				try { e->execute(src); } catch (const std::exception& ex) { err = ex.what(); }
				check_true(err.find("reference variable 'r' must be initialized") != std::string::npos, std::string(src) + ": " + err);
			}
		});
	}

	// Namespace variable forms
	void namespace_forms() {
		test("namespace_variable_spellings_and_captures", [this]() {
			expect("ns forms", R"(
				class T { int v = 1; }
				namespace a { namespace b { var x = 1; } }
				namespace a::b { function get() { return x; } }
				namespace hp { shared_ptr<T> p = new T(); weak_ptr<T> w = p; }
				namespace dyn { var x = 1; }
				var G = 1;
				var x = 100;
				namespace cap { var x = 1; function mk() { return [&]() { x += 1; return x; }; } }
				namespace bump { var x = 1; function go() { var& r = x; r += 1; return x; } }
				var v = 1;
				namespace nf { function f() { return v; } }
				class Cm { int v = 7; int m() { return nf::f(); } }
				hp::p.v = 5;
				dyn::x = "s";
				G = 5;
				var fc = cap::mk(); fc(); fc();
				bump::go();
				`${a::b::get()}|${hp::w.lock().v}|${dyn::x}|${G}|${cap::x},${x}|${bump::x}|${Cm().m()}`;
			)", "1|5|s|5|3,100|2|1");
		});
		test("a_namespace_variable_may_be_reference_declared", [this]() {
			// a namespace variable takes the spellings a program-scope variable takes, a reference
			// included, and a reference-declared variable stores through to what it refers to
			expect("ns ref", R"(
				var G = 1;
				namespace rf { var& r = G; int& get() { return r; } }
				rf::r = 5;
				rf::r += 1;
				auto& q = rf::get(); q = 9;
				`${G}|${rf::r}`;
			)", "9|9");
		});
		test("a_namespace_redeclared_by_another_unit_writes_in_place", [this]() {
			every_config("ns other unit", "2", [](engine& e) {
				e.execute("namespace ns { var x = 1; function get() { return x; } }");
				e.execute("namespace ns { var x = 2; }");
				return e.execute("`${ns::get()}`;").as_string();
			});
		});
		test("a_typed_namespace_store_through_var_is_checked_at_run_time", [this]() {
			expect("ns strict", R"(
				namespace ns { int y = 0; }
				string m = "";
				var text = "s";
				try { ns::y = text; } catch (e) { m = e; }
				m;
			)", "Cannot assign string to 'y' (declared int)");
		});
	}

	void exceptions_more() {
		test("a_bare_rethrow_outside_any_catch_raises", [this]() {
			every_config("rethrow nothing", "Bare throw with no active exception thrown.", [](engine& e) {
				try { e.execute("function f() { throw; } f();"); } catch (const std::exception& ex) { return std::string(ex.what()).substr(0, 43); }
				return std::string("no error");
			});
		});
		test("rethrow_after_a_nested_try_inside_a_catch_rethrows_the_outer_exception", [this]() {
			// a nested try inside a catch keeps the caught exception live for throw;
			// (C++: a completed inner handler hands the exception back to the enclosing one)
			expect("rethrow", R"(
				function run() -> string {
					string r = "";
					try { try { throw "x"; } catch (e) { try { throw "y"; } catch (e2) { r += e2; } throw; } } catch (z) { r += z; }
					try { try { throw "p"; } catch { try { throw "q"; } catch (e3) { r += e3; } throw; } } catch (z2) { r += z2; }
					try { try { throw 1; } catch (e) { try { try { throw 2; } catch (e4) { r += `${e4}`; } throw; } catch (e5) { r += `${e5}`; } throw; } } catch (z3) { r += `${z3}`; }
					return r;
				}
				run();
			)", "yxqp211");
		});
		test("a_fresh_coroutine_starts_with_no_active_exception", [this]() {
			// a bare throw; in a fresh coroutine raises even when the resumer sits inside a catch
			expect("coroutine rethrow", R"(
				coroutine int c() { throw; yield 1; }
				function run() -> string {
					string r = "";
					try { throw "outer"; } catch (e) { try { var h = c(); h.next(); } catch (e2) { r = e2; } r += "|" + e; }
					return r;
				}
				run();
			)", "Bare throw with no active exception thrown.|outer");
		});
		test("an_error_out_of_a_typed_callee_leaves_the_callers_frame_intact", [this]() {
			// the caller's own typed return still converts against the caller's declaration
			expect("frame pop", R"(
				function f() -> int { throw; }
				function a() -> string { string r = ""; try { f(); } catch (e) { r = "caught:" + e; } return r; }
				a();
			)", "caught:Bare throw with no active exception thrown.");
		});
		test("a_finished_coroutine_restores_the_resumers_exception", [this]() {
			expect("resumer trio", R"(
				coroutine int g() { yield 1; }
				var r = "";
				try { throw "outer"; } catch (e) { var h = g(); h.next(); try { throw; } catch (x) { r = x; } }
				r;
			)", "outer");
		});
		test("a_throw_expression_composes_inside_a_ternary", [this]() {
			// docs/site/guide/05-control-flow.html "try / catch / throw": throw is an expression, so it composes (x > 0 ? x : throw "neg")
			expect("throw in ternary", R"(
				function g(int x) { return x > 0 ? x : throw "neg"; }
				function k(int x) -> int { int y = x > 0 ? x : throw "neg3"; return y; }
				string r = "";
				try { g(-1); } catch (e) { r += e; }
				try { k(-1); } catch (e) { r += `|${e}`; }
				try { var q = true ? throw "neg4" : 1; } catch (e) { r += `|${e}`; }
				try { int s = 1 + (false ? 1 : throw "neg5"); } catch (e) { r += `|${e}`; }
				`${r}|${g(2)}`;
			)", "neg|neg3|neg4|neg5|2", warn_region);
		});
		test("a_native_exception_in_a_call_argument_converts_in_the_calling_frame", [this]() {
			every_config("native arg", "own:n", [](engine& e) {
				e.add_function("bad", []() -> script_int { throw std::runtime_error("n"); });
				return e.execute(R"(
					function pass(a, b) { return a; }
					function probe() { try { return pass(1, bad()); } catch (e) { return "own:" + e; } }
					probe();
				)").as_string();
			});
		});
	}

	// docs/site/guide/06-functions.html "Lambdas and captures"
	void closures() {
		test("value_captures_copy_objects_and_share_handles_and_loop_variables_snapshot", [this]() {
			expect("closures", R"(
				class T { int v = 1; }
				function sh() -> int { shared_ptr<T> s = new T(); auto l = [=]() { s.v = 5; }; l(); return s.v; }
				function ob() -> int { T p = T(); auto l = [=]() { return p.v; }; p.v = 9; return l(); }
				function mk() -> string { var fs = []; for (int i = 0; i < 3; i++) { fs.push([=]() { return i; }); } string s = ""; for (auto f : fs) { s += to_string(f()); } return s; }
				`${sh()}|${ob()}|${mk()}`;
			)", "5|1|012");
		});
		test("utf8_erase_counts_characters", [this]() {
			const std::string src = "utf8_string u = \"a\xC3\xB1\xE6\x97\xA5" "b\"; u.erase(1, 2); string s = u; s;";
			expect("utf8 erase", src, "ab");
		});
	}

	// the texts a script catch sees, one per row; `setup` runs first, the failing statement in a try
	struct diag_row { const char* label; const char* setup; const char* failing; const char* expected; bool lax = false; };
	void expect_texts(const std::vector<diag_row>& rows) {
		for (const auto& r : rows) {
			const std::string src = std::string(r.setup) + "\nstring __r = \"no error\";\ntry { " + r.failing + " } catch (__e) { __r = __e; }\n__r;";
			expect(r.label, src, r.expected, r.lax);
		}
	}

	void diagnostic_texts() {
		test("family_a_null_receiver_texts", [this]() {
			expect_texts({
				{"call result access", "function f() { return null; }", "var z = f().x;", "Cannot access 'x' on null ('f(...)' returned null)", warn_region},
				{"call result call", "function f() { return null; }", "f().go();", "Cannot call 'go' on null ('f(...)' returned null)", warn_region},
				{"unnamed access", "var a = [null];", "var z = a[0].x;", "Cannot access 'x' on null", warn_region},
				{"unnamed ternary access", "var a = [null];", "var z = (true ? a[0] : a[0]).x;", "Cannot access 'x' on null", warn_region},
				{"unnamed call", "var a = [null];", "a[0].go();", "Cannot call 'go' on null", warn_region},
				{"named call", "var v8 = null;", "v8.push(1);", "Cannot call 'push' on null ('v8' is null)", warn_region},
				{"named access", "var v8 = null;", "var z = v8.x;", "Cannot access 'x' on null ('v8' is null)", warn_region},
			});
		});
		test("family_b_no_member_and_not_a_function_texts", [this]() {
			expect_texts({
				{"coroutine handle", "coroutine int g() { yield 1; } var h = g();", "var z = h.nosuch;", "coroutine_handle has no member 'nosuch'", warn_region},
				{"string", "var s = \"s\";", "s.foo();", "string has no member 'foo'", warn_region},
				{"array", "var a = [1];", "a.foo();", "array has no member 'foo'", warn_region},
				{"map", "var m = {\"a\": 1};", "m.foo();", "map has no member 'foo'; read the key as [\"foo\"]", warn_region},
				{"map key read", "var m = {\"a\": 1};", "var z = m.a;", "map has no member 'a'; read the key as [\"a\"]", warn_region},
				{"map key call", "var m = {\"f\": () => 1};", "m.f();", "map has no member 'f'; read the key as [\"f\"]", warn_region},
				{"enum table copy", "enum Dir { n, s } var d = Dir;", "var z = d.s;", "map has no member 's'; read the key as [\"s\"]", warn_region},
				{"object", "class Vec2 { int y = 0; } var v = Vec2();", "v.nosuch();", "Vec2 has no member 'nosuch'", warn_region},
				{"int", "var i = 1;", "var z = i.nosuch;", "int has no member 'nosuch'", warn_region},
				{"super", "class Base { int b = 1; } class D : Base { var f() { return super::x(); } }", "var z = D().f();", "parent class Base has no member 'x'", warn_region},
				{"static", "class C { static int s = 0; }", "var z = C::nope;", "class C has no static member 'nope'", warn_region},
				{"call an instance", "class Vec2 { int y = 0; } var v = Vec2();", "v();", "Cannot call 'v': it is a Vec2, not a function", warn_region},
				{"call a string", "var s = \"x\";", "s();", "Cannot call 's': it is a string, not a function", warn_region},
				{"call an int member", "class H { int x = 1; } var h = H();", "h.x(1);", "Cannot call 'x': it is an int, not a function", warn_region},
				{"call null", "var f = null;", "f();", "Cannot call 'f': it is null, not a function", warn_region},
				{"call a computed null", "var fs = [null];", "fs[0]();", "Cannot call null: it is not a function", warn_region},
				{"call a computed int", "var fs = [5];", "fs[0](1);", "Cannot call a value of type int: it is not a function", warn_region},
			});
		});
		test("family_c_arity_and_overload_texts", [this]() {
			expect_texts({
				{"method arity", "class V { int dot(int a) { return a; } }", "V().dot();", "'dot' expects 1 argument, got 0", warn_region},
				{"function arity", "function fib(int n) { return n; }", "fib(1, 2);", "'fib' expects 1 argument, got 2", warn_region},
				{"lambda arity", "var l = [](int a) { return a; };", "l();", "lambda expects 1 argument, got 0", warn_region},
				{"overload list", "class V { int dot(int a) { return a; } int dot(int a, int b) { return a + b; } }", "V().dot(V(), null, [1], {\"k\": 1});", "No overload of 'dot' takes (V, null, array, map)", warn_region},
				{"overload list cap", "class M { int m(int a) { return a; } int m(int a, int b) { return b; } }", "M().m(1, 2, 3, 4, 5, 6, 7);", "No overload of 'm' takes (int, int, int, int, int, int, ...)", warn_region},
				{"namespace overloads", "namespace ns { function f(int a, int b) { return 1; } function f(string s) { return 2; } }", "ns::f(1, 2, 3);", "No overload of 'f' takes (int, int, int)", warn_region},
				{"constructor overloads", "class Vec2 { Vec2(int x) {} Vec2(int x, int y) {} }", "Vec2(\"s\", 1, 2);", "No constructor of 'Vec2' takes (string, int, int)", warn_region},
				{"direct super", "class Base { Base(int x) {} } class D : Base { D() : super(\"s\", 2) {} }", "D();", "No constructor of 'Base' takes (string, int)", warn_region},
				{"ancestor initializer", "class A { A(int x) {} } class B : A { B(int y) : super(\"s\", 1) {} } class C : B { C() : super(1) {} }", "C();", "No constructor of 'A' takes (string, int)", warn_region},
				{"delegation", "class Vec2 { Vec2(int x) {} Vec2() : this(\"s\", 1) {} }", "Vec2();", "No constructor of 'Vec2' takes (string, int)", warn_region},
				{"builtin push", "var a = [];", "a.push();", "push() takes exactly one argument", warn_region},
				{"builtin size", "var a = [];", "a.size(1);", "size() takes no arguments", warn_region},
				{"math math::pow", "", "math::pow(2);", "math::pow expects 2 arguments", warn_region},
			});
		});
		test("family_d_typed_boundary_texts", [this]() {
			expect_texts({
				{"lambda parameter", "var l = [](int n) { return n; }; var s = \"x\";", "l(s);", "Cannot pass string to parameter 'n' of lambda (declared int)"},
				{"function parameter", "function fib(int n) { return n; } var s = \"x\";", "fib(s);", "Cannot pass string to parameter 'n' of 'fib' (declared int)"},
				{"typed return", "function f() -> int { var s = \"x\"; return s; }", "f();", "Cannot return string from 'f' (declared -> int)"},
				{"assignment", "int x = 0; var s = \"s\";", "x = s;", "Cannot assign string to 'x' (declared int)"},
				{"declaration", "var s = \"s\";", "int x = s;", "Cannot initialize 'x' (declared int) with string"},
				{"field", "class Vec2 { int s = 0; } var v = Vec2(); var str = \"q\";", "v.s = str;", "Cannot assign string to field 's' of Vec2 (declared int)"},
				{"static", "class C { static int s = 0; } var str = \"q\";", "C::s = str;", "Cannot assign string to static 's' of C (declared int)"},
				{"class into class", "class A { int a = 0; } class B { int b = 0; } A x = A(); var b = B();", "x = b;", "Cannot assign B to 'x' (declared A)"},
				{"weak handle declaration", "class Foo { int v = 0; } var f = Foo();", "weak_ptr<Foo> w = f;", "Cannot initialize 'w' (declared weak_ptr<Foo>) with Foo", warn_region},
				{"weak handle assignment", "class Foo { int v = 0; } var f = Foo(); weak_ptr<Foo> w;", "w = f;", "Cannot assign Foo to 'w' (declared weak_ptr<Foo>)", warn_region},
				{"shared handle assignment", "class Foo { int v = 0; } shared_ptr<Foo> p = new Foo(); var s = \"x\";", "p = s;", "Cannot assign string to 'p' (declared shared_ptr<Foo>)", warn_region},
				{"class conversion", "class A { A(int x) {} } class B { int b = 0; } function f(A a) { return 1; } var b = B();", "f(b);", "Cannot convert B to A (no suitable single-argument constructor)", warn_region},
			});
		});
		test("checked_float_to_int_texts_leave_the_destination_unwritten", [this]() {
			const std::string text = "Cannot convert float to integer: non-finite or out of range";
			expect_texts({
				{"assignment", "var f = 1e100; int x = 0;", "x = f;", "Cannot assign float to 'x' (declared int)"},
				{"itrunc", "var f = 1e100;", "var z = math::itrunc(f);", text.c_str()},
				{"ifloor", "var f = -1e100;", "var z = math::ifloor(f);", text.c_str()},
				{"cast", "var f = 1e100;", "var z = cast<int>(f);", text.c_str()},
			});
			expect("unwritten", "var f = 1e100; int x = 7; try { x = f; } catch (e) { } `${x}`;", "7");
		});
		test("untouched_family_texts", [this]() {
			expect_texts({
				{"class not found", "", "var z = Nope::x;", "Class 'Nope' not found", warn_region},
				{"integer overflow in division", "var a = -9223372036854775807 - 1; var b = -1;", "var z = a / b;", "Integer overflow in '/'", warn_region},
				{"invalid plus", "var m = {}; var a = [1];", "var z = a + m;", "Invalid operands for + operator", warn_region},
				{"unary minus", "var s = \"x\";", "var z = -s;", "Unary minus requires numeric operand", warn_region},
				{"array index type", "var a = [1];", "var z = a[\"k\"];", "Array index must be an integer", warn_region},
				{"subscript on int", "var n = 5;", "var z = n[0];", "Subscript can only be used on arrays, maps, or types with [] operator", warn_region},
				{"pop empty", "var a = [];", "a.pop();", "Cannot pop from empty array", warn_region},
				{"front empty", "var a = [];", "var z = a.front();", "Cannot get front of empty array", warn_region},
				{"sort comparator", "var a = [2, 1];", "a.sort(5);", "sort() comparator must be a function", warn_region},
				{"pad_left fill", "var s = \"x\";", "s.pad_left(3, \"\");", "pad_left() padding character cannot be empty", warn_region},
				{"to_int base", "var s = \"1\";", "var z = s.to_int(0, 1);", "to_int() base must be 0 or between 2 and 36", warn_region},
				{"code", "var s = \"\";", "var z = s.code();", "code() needs a string of exactly one UTF-8 character", warn_region},
				{"partition", "var s = \"a\";", "var z = s.partition(\"\");", "partition() separator cannot be empty", warn_region},
				{"split count", "var s = \"a\";", "var z = s.split(\",\", \"x\");", "split() second argument must be an integer", warn_region},
				{"from_code", "", "var z = string::from_code(-1);", "string::from_code expects a Unicode code point (0 to 0x10FFFF, no surrogate)", warn_region},
				{"join", "", "var z = string::join([1]);", "string::join expects an array of strings", warn_region},
				{"repeat", "", "var z = string::repeat(\"a\", -1);", "string::repeat count cannot be negative", warn_region},
				{"namespace pad_left", "", "var z = string::pad_left(\"a\", 3, \"\");", "string::pad_left fill must be a char or a non-empty string", warn_region},
				{"unknown namespace member", "", "var z = string::zzz_nope;", "Undefined variable 'string::zzz_nope'", warn_region},
				{"super without base", "class A { A() : super() {} }", "var z = A();", "Cannot call super() - class has no base class", warn_region},
				{"iterate int", "", "for (auto x : 5) { }", "Cannot iterate int: not an array, map, string or coroutine", warn_region},
				{"iterate null", "var n = null;", "for (auto x : n) { }", "Cannot iterate null: not an array, map, string or coroutine", warn_region},
				{"auto array homogeneity", "var s = \"x\";", "auto a = [1, 2, 3, s];", "Array 'auto' requires homogeneous elements - type mismatch at index 3", warn_region},
				{"this at top level", "", "var z = this;", "'this' can only be used inside methods", warn_region},
			});
		});
		test("host_configuration_and_uncaught_suffix_texts", [this]() {
			every_config("host texts", "max_call_depth must be between 1 and INT_MAX|Maximum recursion depth exceeded - possible infinite recursion|Cannot access 'x' on null ('v8' is null)", [](engine& e) {
				std::string out;
				try { e.max_call_depth(backend_type::interpreter, 0); out += "accepted"; } catch (const std::exception& ex) { out += ex.what(); }
				out += "|";
				// not a tail call: a VM self tail call reuses its frame and runs into the budget instead (docs/site/reference/errors.html "Run-time errors")
				e.execution_budget(0);
				try { e.execute(spec::in_warn_region("function r() { return r() + 1; } r();")); out += "no error"; } catch (const std::exception& ex) { out += ex.what(); }
				out += "|";
				try { e.execute(spec::in_warn_region("var v8 = null; v8.x;")); out += "no error"; } catch (const std::exception& ex) { out += ex.what(); }
				return out;
			});
		});
	}

	// An error raised by script a builtin or an operator runs on the program's behalf (a comparator, a predicate, a user
	// operator, to_bool(), to_string(), an archive hook) is that script's error: it propagates with its own text, catchable,
	// and the builtin stops calling back after it
	void callback_errors() {
		test("a_sort_comparator_error_propagates_stops_the_calls_and_leaves_the_array", [this]() {
			expect("sort error", R"(
				int calls = 0;
				function inner() { int z = 0; calls += 1; int q = 5 / z; }
				array<int> v = [3, 1, 2];
				string r = "no error";
				try { v.sort([](int a, int b) { inner(); return a < b; }); } catch (e) { r = `caught:${e}`; }
				`${r}|calls=${calls}|${v[0]}${v[1]}${v[2]}`;
			)", "caught:Division by zero|calls=1|312");
			expect("sort throw", R"(
				array<int> v = [3, 1, 2];
				string r = "no error";
				try { v.sort([](int a, int b) -> bool { if (a > -100) { throw "cmp"; } return a < b; }); } catch (e) { r = `caught:${e}`; }
				`${r}|${v[0]}${v[1]}${v[2]}`;
			)", "caught:cmp|312");
		});
		test("an_uncaught_comparator_error_ends_the_program_with_the_callbacks_trace", [this]() {
			every_config("sort uncaught", "Division by zero|at inner|at <lambda>", [](engine& e) {
				try {
					e.execute(R"(
						int calls = 0;
						void inner() { int z = 0; calls += 1; int q = 5 / z; }
						array<int> v = [3, 1, 2];
						v.sort([](int a, int b) { inner(); return a < b; });
					)");
				} catch (const std::exception& ex) {
					const std::string w = ex.what();
					const std::string trace = e.format_stack_trace();
					std::string out = w.find("Division by zero") != std::string::npos ? "Division by zero" : w;
					out += trace.find("at inner") != std::string::npos ? "|at inner" : "|no inner frame: " + trace;
					out += trace.find("at <lambda>") != std::string::npos ? "|at <lambda>" : "|no lambda frame";
					return out;
				}
				return std::string("no error");
			});
		});
		test("user_operator_errors_propagate_with_their_own_text", [this]() {
			const char* K = R"(
				class K {
					int x = 1;
					K(int v) { x = v; }
					function operator+(int n) -> K { int z = 0; return K(x / z); }
					function operator<(K o) -> bool { int z = 0; return x / z < 1; }
					function operator==(K o) -> bool { int z = 0; return x / z == 1; }
				}
				K k = K(4);
				K k2 = K(5);
			)";
			const char* div = "Division by zero";
			const std::string KH = std::string(K) + "class H { K m = K(2); } H h = H();";
			expect_texts({
				{"operator+", K, "K k3 = k + 1;", div},
				{"operator<", K, "bool b = k < k2;", div},
				{"operator==", K, "bool b = k == k2;", div},
				{"operator!= through ==", K, "bool b = k != k2;", div},
				{"compound on a global", K, "k += 1;", div},
				{"compound on a local", K, "function f() { K a = K(4); a += 1; } f();", div},
				{"compound on a field", KH.c_str(), "h.m += 1;", div},
			});
		});
		test("to_bool_and_to_string_errors_propagate", [this]() {
			const char* Q = R"(
				class Q { bool to_bool() { int z = 0; return 1 / z > 0; } }
				class TS { string to_string() { int z = 0; return `t${(1 / z)}`; } }
				auto q = Q();
				auto ts = TS();
			)";
			const char* div = "Division by zero";
			expect_texts({
				{"if (q)", Q, "if (q) { int w = 1; }", div},
				{"!q", Q, "bool b = !q;", div},
				{"q ? :", Q, "int w = q ? 1 : 2;", div},
				{"bool slot", Q, "bool b = q;", div},
				{"string + object", Q, "string s = `x${ts}`;", div},
				{"object + string", Q, "string s = `${ts}x`;", div},
			});
		});
		test("a_user_operator_assign_error_propagates_with_its_own_text", [this]() {
			const char* A2 = "class A2 { int v = 1; function operator=(int x) -> A2 { int z = 0; v = x / z; return this; } } A2 g = A2();";
			expect_texts({
				{"operator= on a global", A2, "g = 5;", "Division by zero"},
				{"operator= on a local", A2, "function f() { A2 a = A2(); a = 5; } f();", "Division by zero"},
			});
		});
		// A save reads a field's storage and a load writes it, so no accessor the script declares runs during either (an
		// accessor with no field holds no state and is not saved); a post_load's error leaves the load
		test("archive_hooks_the_script_declares_propagate_their_errors", [this]() {
			const char* H = R"(
				class L { int v = 1; void post_load(int version) { int z = 0; v = v / z; } }
			)";
			expect_texts({
				{"from_json post_load", H, "var o = from_json(\"{\\\"$type\\\":\\\"L\\\",\\\"$val\\\":{\\\"v\\\":3}}\");", "Division by zero"},
			});
			expect("hooks that succeed", R"(
				class L2 { int v = 1; void post_load(int version) { v = v + 10 * version; } }
				class S2 { int v = 1; void _set_v(int x) { int z = 0; v = x / z; } int _get_v() { return v * 100; } }
				class P { int raw = 1; int _get_v() { int z = 0; return raw / z; } }
				var a = from_json("{\"$type\":\"L2\",\"$val\":{\"v\":3}}");
				var b = from_json("{\"$type\":\"S2\",\"$val\":{\"v\":3}}");
				`${a.v}|${b.v}|${to_json(b)}|${to_json(P())}`;
			)", "13|300|{\"$type\":\"S2\",\"$val\":{\"v\":3}}|{\"$type\":\"P\",\"$val\":{\"raw\":1}}");
		});
		test("array_slice_defaults_and_removal_presence", [this]() {
			expect("slice defaults", R"(
				array<int> values = [1,2,3];
				array<int> copy = array::slice(values); copy[0] = 9;
				to_json(values) + "|" + to_json(copy) + "|" + to_json(array::slice(values, 1)) + "|" + to_json(array::slice(values, -1));
			)", "[1,2,3]|[9,2,3]|[2,3]|[3]");
			expect("slice copies values and shares handles", R"(
				class Cell { int value = 1; }
				array<Cell> values = [Cell()]; array<Cell> valueCopy = array::slice(values); valueCopy[0].value = 9;
				array<shared_ptr<Cell>> handles = [new Cell(), null];
				array<shared_ptr<Cell>> handleCopy = array::slice(handles); handleCopy[0].value = 7;
				to_string(values[0].value) + "|" + to_string(valueCopy[0].value) + "|" +
				to_string(handles[0].value) + "|" + to_string(same_object(handles[0], handleCopy[0])) + "|" + to_string(handleCopy[1] == null);
			)", "1|9|7|true|true");
			expect("array removal aliases", R"(
				array<int> values = [7,8,9];
				bool removed = values.erase(0); bool missing = values.erase(8);
				to_string(removed) + "|" + to_string(missing) + "|" + to_string(values.remove(0)) + "|" + to_json(values);
			)", "true|false|true|[9]");
			expect("map erase presence", R"(
				map<string,int> values = {"a":1,"b":2};
				bool removed = values.erase("a"); bool missing = values.erase("a");
				to_string(removed) + "|" + to_string(missing) + "|" + to_string(values.remove("b")) + "|" + to_json(values);
			)", "true|false|true|{}");
		});

		test("array_sort_uses_language_order_and_optional_predicate", [this]() {
			expect("large int default sort", R"(
				array<var> values = [9007199254740993, 9007199254740992, 9007199254740991];
				values.sort(); to_json(values);
			)", "[9007199254740991,9007199254740992,9007199254740993]");
			expect("spaceship sort", R"(
				class Item {
					int key;
					Item(int a_key) { key = a_key; }
					int operator<=>(Item& a_other) { return key <=> a_other.key; }
				}
				array<Item> items = [Item(3), Item(1), Item(2)];
				items.sort(); to_string(items[0].key) + "," + to_string(items[1].key) + "," + to_string(items[2].key);
			)", "1,2,3");
			expect("less beats spaceship", R"(
				class Item {
					int key;
					Item(int a_key) { key = a_key; }
					bool operator<(Item& a_other) { return key > a_other.key; }
					int operator<=>(Item& a_other) { return key <=> a_other.key; }
				}
				array<Item> items = [Item(3), Item(1), Item(2)];
				items.sort(); to_string(items[0].key) + "," + to_string(items[1].key) + "," + to_string(items[2].key);
			)", "3,2,1");
			expect("descending predicate", R"(
				array<int> values = [2, 1, 3]; values.sort((a, b) => a > b); to_json(values);
			)", "[3,2,1]");
			expect("char default order", R"(
				array<char> values = ['\xFF', '\x01', '\x80']; values.sort();
				to_string(cast<int>(values[0])) + "," + to_string(cast<int>(values[1])) + "," + to_string(cast<int>(values[2]));
			)", "1,128,255");
			expect("predicate return contract", R"(
				array<int> values = [2, 1]; var failure = "";
				try { values.sort((a, b) => a - b); } catch (e) { failure = e; }
				failure + ":" + to_json(values);
			)", "sort() comparator must return bool:[2,1]");
		});

		test("array_sort_pins_its_receiver_across_callback_reentry", [this]() {
			expect("replacement during sort", R"(
				array<int> values = [3, 1, 2]; int calls = 0;
				values.sort([&](int a, int b) -> bool { ++calls; values = [9]; return a < b; });
				to_json(values) + "|" + to_string(calls > 0);
			)", "[9]|true");
		});

		test("array_sort_honors_a_global_less_override", [this]() {
			every_config("global less sort", "[3,2,1]", [](engine& a_engine) {
				a_engine.add_function("<", [](script_int a_left, script_int a_right) { return a_left > a_right; });
				return a_engine.execute("array<int> values = [2, 1, 3]; values.sort(); to_json(values);").to_string();
			});
		});

		test("a_default_sort_of_instances_orders_by_the_languages_less_than", [this]() {
			// docs/site/guide/04-operators.html "Equality": sort() orders through the language's < - an instance by its user operator<
			// (C++ std::sort's operator<), arrays lexicographically, and an element with no order raises the < operator's rejection
			// instead of sorting by address or a rank assigned to its type
			expect("default sort", R"(
				class K { int v = 0; K(int x) { v = x; } }
				class L { int v = 0; L(int x) { v = x; } bool operator<(L o) { return v < o.v; } }
				function t(function f) -> string { string r = ""; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				t([]() { array<L> ls = [L(3), L(1), L(2)]; ls.sort(); return `${ls[0].v}${ls[1].v}${ls[2].v}`; }) + "|" +
				t([]() { var ls = [L(3), L(1), L(2)]; ls.sort(); return `${ls[0].v}${ls[1].v}${ls[2].v}`; }) + "|" +
				t([]() { array<K> ks = [K(3), K(1)]; ks.sort(); return "sorted"; }) + "|" +
				t([]() { var a = [[3], [1]]; a.sort(); return "sorted"; }) + "|" +
				t([]() { var a = [3, "a", 1.5, true]; a.sort(); return to_json(a); });
			)", "123|123|Invalid operands for < operator|sorted|Invalid operands for < operator", warn_region);
		});
		test("predicate_and_parallel_body_errors_propagate", [this]() {
			const char* div = "Division by zero";
			expect_texts({
				{"array filter", "", "var b = array::filter([1, 2], [](var x) { int z = 0; return x / z > 0; });", div, warn_region},
				{"array remove_if", "var a = [1, 2];", "a.remove_if([](var x) { int z = 0; return x / z > 0; });", div, warn_region},
				{"map filter", "var m = {\"a\": 1};", "m.filter([](var k, var v) { int z = 0; return v / z > 0; });", div, warn_region},
				{"map remove_if", "var m = {\"a\": 1};", "m.remove_if([](var k, var v) { int z = 0; return v / z > 0; });", div, warn_region},
				{"parallel_for body", "array<int> xs = [1, 2, 3, 4];", "parallel_for (auto& x : xs) { int z = 0; x = x / z; }", div},
			});
		});
	}

	// docs/site/guide/02-type-ladder.html "The one conversion table": on plain assignment an object slot takes a source through a user operator= of its
	// class (C++ assignment: the operator runs on the held instance, exact match ahead of a converting constructor); the
	// expression's value is the destination's (docs/site/stability.html "Statements and expressions"); container elements
	// need the exact type (docs/site/guide/09-containers-strings.html "Typed containers"), so an element store never dispatches it
	void user_assignment_operator() {
		const char* OK = R"(
			class OK2 { int v = 1; function operator=(int x) -> OK2 { v = x * 10; return this; } }
			class H { OK2 m = OK2(); static OK2 s = OK2(); void set_bare() { m = 7; } void set_this() { this.m = 8; } }
			namespace ns { OK2 n = OK2(); }
			OK2 g = OK2();
		)";
		test("operator_assign_runs_at_every_plain_store", [this, OK]() {
			expect("destinations", std::string(OK) + R"(
				function local() -> int { OK2 a = OK2(); a = 5; return a.v; }
				function auto_local() -> int { auto a = OK2(); a = 4; return a.v; }
				function field() -> int { H h = H(); h.m = 5; return h.m.v; }
				function bare() -> int { H h = H(); h.set_bare(); return h.m.v; }
				function this_field() -> int { H h = H(); h.set_this(); return h.m.v; }
				g = 3;
				H::s = 2;
				ns::n = 6;
				`${local()}|${auto_local()}|${field()}|${bare()}|${this_field()}|${g.v}|${H::s.v}|${ns::n.v}`;
			)", "50|40|50|70|80|30|20|60");
		});
		test("operator_assign_value_is_the_destination_and_a_copy", [this, OK]() {
			expect("expression value", std::string(OK) + R"(
				function local() -> string { OK2 a = OK2(); var r = (a = 6); r.v = 1; return `${type_of(r)}:${a.v}`; }
				function global() -> string { var r = (g = 9); r.v = 2; return `${type_of(r)}:${g.v}`; }
				local() + "|" + global();
			)", "OK2:60|OK2:90");
		});
		test("same_class_and_subclass_sources_copy_and_exact_operator_beats_the_converting_constructor", [this]() {
			expect("copy and precedence", R"(
				class B2 { int v = 1; B2() {} B2(int x) { v = x; } function operator=(int x) -> B2 { v = x * 100; return this; } }
				class D2 : B2 { D2() { v = 42; } }
				function run() -> string {
					B2 a = B2(); B2 b = B2(); b.v = 7; a = b;
					B2 c = B2(); c = D2();
					B2 d = B2(); d = 3;
					B2 e = 4;
					return `${a.v}|${c.v}|${d.v}|${e.v}`;
				}
				run();
			)", "7|42|300|4");
		});
		test("an_element_store_runs_an_operator_that_takes_the_value", [this, OK]() {
			expect_texts({
				{"array element", OK, "array<OK2> xs = [OK2()]; xs[0] = 5; __r = to_string(xs[0].v);", "50", warn_region},
				{"array element no overload takes", OK, "array<OK2> xs = [OK2()]; var s = \"s\"; xs[0] = s;", "Cannot assign 'string' to element of type 'OK2'", warn_region},
			});
		});
	}

	// every runtime error a catch sees has a text; one raised without its own reads as its code's
	// message, the text the host boundary shows (`what()` of the uncaught error)
	void textless_errors() {
		test("a_compound_error_on_a_variable_has_a_text", [this]() {
			expect_texts({
				{"local /= 0", "", "function f() { int x = 5; int z = 0; x /= z; } f();", "Division by zero", warn_region},
				{"global /= 0", "int gx = 5; int gz = 0;", "gx /= gz;", "Division by zero", warn_region},
				{"local -= string", "", "function f() { int x = 5; var s = \"s\"; x -= s; } f();", "Cannot apply '-=' to int and string", warn_region},
				{"string += int", "", "function f() { string s = \"a\"; s += 5; } f();", "Cannot apply '+=' to string and int", warn_region},
				{"bool += int", "", "function f() { bool b = true; b += 1; } f();", "Cannot apply '+=' to bool and int", warn_region},
			});
		});
		test("the_catch_text_matches_the_uncaught_text", [this]() {
			every_config("host text", "Cannot apply '+=' to string and int|Cannot apply '+=' to string and int", [](engine& e) {
				const std::string caught = e.execute(spec::in_warn_region("string r = \"\"; function f() { string s = \"a\"; s += 5; } try { f(); } catch (x) { r = x; } r;")).as_string();
				std::string uncaught;
				try { e.execute(spec::in_warn_region("f();")); } catch (const std::exception& ex) { uncaught = ex.what(); }
				return caught + "|" + uncaught.substr(0, caught.size());
			});
		});
	}

	// docs/site/embedding/06-static-checking.html "One conversion table" and "What still fails at run time (the precision
	// map)": load verdicts (engine::check reports, nothing runs)
	void checker_verdicts() {
		test("checker_verdicts_follow_the_rule_table", [this]() {
			struct row { const char* label; const char* src; size_t errors; };
			const row rows[] = {
				{"map into int", "int x = {\"k\": 1};", 1},
				{"typed array into string array", "array<int> a = [1]; array<string> b = a;", 1},
				{"typed array into float array", "array<int> a = [1]; array<float> c = a;", 0},
				{"containers and callables into a class slot", "class C { int v = 0; } C a = [1]; C b = {\"k\": 1}; C d = [](){ return 1; };", 3},
				{"auto from null stays dynamic", "auto x = null; x = 1; x = \"s\";", 0},
				{"a typed reference parameter binds only its type", "function f(int& r) { } string s = \"x\"; f(s);", 1},
				{"itrunc is int", "char c = math::itrunc(1.5);", 1},
				{"sin is float", "char c = math::sin(1.0);", 1},
				{"sin into int is refused", "int i = math::sin(1.0);", 1},
				{"itrunc and ifloor into int", "int i = math::itrunc(1.5); int j = math::ifloor(1.5); int k = cast<int>(math::sin(1.0));", 0},
				{"abs and max follow their operands", "char a = math::abs(1); char b = math::abs(1.5); char c = math::max(1, 2.5);", 2},
				{"an unknown math name is a load error", "char c = math::no_such(1);", 1},
				{"math arity is a runtime error", "var z = math::pow(2);", 0},
				{"a namespace path names a member", "namespace a::b { int c = 1; } var x = a::b::zzz; var y = a::b::c;", 1},
				{"handle builtins on any object", "class C { int v = 0; } C c = C(); int n = c.use_count(); bool u = c.unique(); c.reset();", 0},
				{"a class method wins over the handle builtin", "class D { string use_count() { return \"m\"; } } int n = D().use_count();", 1},
				{"builtin results stay lenient", "map<string, int> m = {\"a\": 1}; var k = m.keys()[0].to_upper();", 0},
				{"element stores through a range reference are runtime", "array<int> a = [1]; for (auto& x : a) { x = \"s\"; }", 0},
				{"out of bounds is runtime", "array<int> a = [1]; int x = a[5];", 0},
				{"a namespace variable reads by its declared type", "namespace ns { int x = 1; } char c = ns::x;", 1},
				{"missing map key is runtime", "map<string, int> m = {\"a\": 1}; int x = m[\"missing\"];", 0},
			};
			for (const auto& r : rows) {
				every_config(r.label, std::to_string(r.errors), [&](engine& e) { return std::to_string(e.check(r.src).error_count()); });
			}
		});
		test("a_bare_return_is_an_error_where_the_result_holds_no_null", [this]() {
			every_config("bare return", "1|0 1|0", [](engine& e) {
				auto shape = [&](const char* src) {
					const auto rep = e.check(src);
					size_t warnings = 0;
					for (const auto& d : rep.diagnostics) { if (d.severity != check_diagnostic::level::error) { ++warnings; } }
					return std::to_string(rep.error_count()) + "|" + std::to_string(warnings > 0 ? 1 : 0);
				};
				return shape("function f() -> int { return; }") + " " + shape("class P {} function g() -> P { return; }");
			});
		});
		test("a_host_global_types_its_uses", [this]() {
			every_config("host global", "1", [](engine& e) {
				e.add_global("g", script_value(script_int(5), &e));
				return std::to_string(e.check("char c = g;").error_count());
			});
		});
		test("strict_loads_value_dependent_failures_and_the_runtime_raises_them", [this]() {
			expect("runtime", R"(
				map<string, int> m = {"a": 1};
				string r = "";
				try { int x = m["missing"]; } catch (e) { r += e + "|"; }
				array<int> a = [1];
				try { for (auto& x : a) { x = "s"; } } catch (e) { r += "raised|"; }
				try { int y = a[5]; } catch (e) { r += e; }
				r;
			)", "Key \"missing\" not found in map|raised|Array index 5 out of bounds for array of size 1");
		});
		test("bitwise_compound_assignments_are_x_op_equals_y_with_x_evaluated_once", [this]() {
			// &= |= ^= <<= >>= on every lvalue (a local, a field, this's field, a static, a namespace
			// variable, an element, a map entry), the stored value as the expression's value, the operand text
			expect("bitwise compound", R"(
				class Flags { int f = 1; static int s = 1; void bump() { f |= 8; } }
				namespace ns { int m = 12; }
				int calls = 0;
				function at() -> int { calls += 1; return 0; }
				function t(function f) -> string { string r = ""; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				t([]() { int x = 6; x &= 3; x |= 8; x ^= 1; x <<= 2; x >>= 1; return x; }) + "|" +
				t([]() { Flags o; o.f |= 6; o.bump(); Flags::s <<= 4; ns::m &= 4; return `${o.f} ${Flags::s} ${ns::m}`; }) + "|" +
				t([]() { array<int> a = [5, 3]; var v = [12]; map<string, int> m = {"k": 0}; a[at()] &= 4; v[0] >>= 2; m["k"] |= 3; return `${a[0]} ${v[0]} ${m["k"]} ${calls}`; }) + "|" +
				t([]() { int x = 5; int y = (x ^= 3); return y; }) + "|" +
				t([]() { var s = 1.5; s &= 1; return s; }) + "|" +
				t([]() { int x = 1; x <<= 64; return x; }) + "|" +
				t([]() { var w = [1.5]; w[0] |= 1; return w[0]; });
			)", "22|15 16 4|4 3 3 1|6|Cannot apply '&=' to float and int|Shift amount must be between 0 and 63|Cannot apply '|=' to float and int", warn_region);
			every_config("checker", "1|0", [](engine& e) {
				auto bad = e.check("float f = 1.5; f &= 1;");
				auto ok = e.check("int x = 6; x &= 3; char c = 'a'; int y = 1; y |= c;");
				return std::to_string(bad.error_count()) + "|" + std::to_string(ok.error_count());
			});
		});
	}

	// instance variables are an overlay for one execution, not dynamic scope
	void instance_variable_overlays() {
		test("an_instance_variable_overlay_lives_for_one_execution", [this]() {
			every_config("overlay", "5|1|raised|raised|5", [](engine& e) {
				e.add_global("G", script_value(script_int(1), &e));
				instance_variables five{{"G", script_value(script_int(5), &e)}};
				std::string out = std::to_string(e.execute("G", five).as_int());
				out += "|" + std::to_string(e.execute("G").as_int());
				instance_variables n0{{"n", script_value(script_int(0), &e)}};
				e.execute("var temp = 1;", n0);
				try { e.execute(spec::in_warn_region("temp")); out += "|visible"; } catch (const std::exception&) { out += "|raised"; }
				try { e.execute(spec::in_warn_region("n")); out += "|visible"; } catch (const std::exception&) { out += "|raised"; }
				instance_variables n5{{"n", script_value(script_int(5), &e)}};
				out += "|" + std::to_string(e.execute("function f8() { return n; } f8()", n5).as_int());
				return out;
			});
		});
	}

	// docs/site/guide/10-modules.html "Where files are"
	void file_locations() {
		test("file_name_is_lexical_and_parent_dir_canonicalizes", [this]() {
			expect("locations", R"(
				string d = working_dir();
				`${file_name("no/such/dir/x.jai")}|${to_string(parent_dir(d + "/x.jai") == parent_dir(d + "/sub/../x.jai"))}`;
			)", "x.jai|true");
		});
		test("a_bare_engine_has_no_host_file_functions", [this]() {
			every_config("bare engine", "Undefined variable 'read_file'", [](engine& e) {
				auto bare = jai::foundry::make_default_engine();
				bare->set_backend(e.get_backend_type());
				return bare->execute(spec::in_warn_region("var r = \"\"; try { read_file(\"x\"); } catch (e) { r = e; } r;")).as_string();
			});
		});
		test("a_spliced_include_keeps_its_declarations_in_the_function_and_a_computed_include_sees_globals", [this]() {
			const auto dir = jai::foundry::test_temp_directory() / "spec_conformance_includes";
			std::filesystem::create_directories(dir);
			std::ofstream(dir / "twice.jai") << "function twice(int v) -> int { return v * 2; }\n";
			std::ofstream(dir / "reads_g.jai") << "G * 2;\n";
			every_config("includes", "2|Undefined variable 'twice'|8", [&](engine& e) {
				e.add_include_path(dir.string());
				std::string out = std::to_string(e.execute("function run(int x) { include \"twice.jai\"; return twice(x); } run(1);").as_int());
				out += "|" + e.execute(spec::in_warn_region("var r = \"\"; try { twice(3); } catch (err) { r = err; } r;")).as_string();
				out += "|" + std::to_string(e.execute("var G = 4; function run2() { var p = \"reads_g.jai\"; return include(p); } run2();").as_int());
				return out;
			});
		});
	}

	// docs/site/embedding/05-safety-rails.html: a denial names the lowest missing slot, the engine's own before a host's
	void permission_order() {
		test("a_denial_names_the_first_missing_permission_in_slot_order", [this]() {
			every_config("denial order", "Insufficient permission: app.b.|Insufficient permission: filesystem.", [](engine& e) {
				e.register_permission(spec_permission::app_a, "app.a");
				e.register_permission(spec_permission::app_b, "app.b");
				e.add_function("f", []() -> script_int { return 1; }, spec_permission::app_a, spec_permission::app_b);
				e.add_function("g2", []() -> script_int { return 2; }, jai::permissions::filesystem, spec_permission::app_a);
				e.disable(jai::permissions::filesystem);
				std::string out = e.execute("var r = \"\"; try { f(); } catch (x) { r = x; } r;").as_string();
				out += "|" + e.execute("var r2 = \"\"; try { g2(); } catch (x) { r2 = x; } r2;").as_string();
				return out;
			});
		});
	}

	// docs/site/guide examples with their stated outputs
	void guide_examples() {
		test("guide_operator_precedence_and_associativity", [this]() {
			// docs/site/guide/04-operators.html, C++ precedence (lowest to highest): assignment (right), ternary (right), ||, &&, |, ^, &, equality, relational, shift, additive
			expect("precedence", R"(
				var a = 0; var b = 0; a = b = 5;
				`${a}${b}|${to_string(true || false && false)}|${(1 | 2 ^ 3)}|${to_string(1 < 2 == true)}|${(1 << 2 + 1)}|${(-2 * 3)}|${to_string(!false == true)}|${(2 | 1 ^ 3 & 1)}|${(8 >> 1 + 1)}|${(true ? 1 : false ? 2 : 3)}${(false ? 1 : true ? 2 : 3)}`;
			)", "55|true|1|true|8|-6|true|2|2|12");
		});
		test("guide_function_and_lambda_examples", [this]() {
			expect("functions", R"(
				var mul4(var a, var b) { return a * b; }
				var pick(bool flag) { return flag ? 42 : "none"; }
				auto ops = {"double": [](x) { return x * 2; }, "square": [](x) { return x * x; }};
				function g(int x) { return x > 0 ? x : throw "neg"; }
				function f() -> int { int a = 1; int m = 10; auto h = [&, m]() { a = 7; }; h(); return a * 100 + m; }
				string e = ""; try { g(-1); } catch (x) { e = x; }
				`${mul4(2.5, 2)}|${pick(true)}|${pick(false)}|${ops["square"](7)}|${g(2)}|${e}|${f()}`;
			)", "5.0|42|none|49|2|neg|710");
		});
		test("guide_class_examples", [this]() {
			expect("classes", R"(
				class Cat { int a = 0; } class Tiger : Cat { int stripes = 12; }
				int g = 5;
				class C { int m() { int a = g; int g = 9; return a * 10 + g; } }
				class S { int f = 3; static int s = 4; int use() { return f + s; } static int st() { return s; } }
				class A { int v = 1; };
				class Vec { float x = 0.0; Vec(float a) { x = a; } Vec operator+(Vec o) { return Vec(x + o.x); } }
				string err = ""; try { var t = cast<Tiger&>(Cat()).stripes; } catch (x) { err = x; }
				`${err}|${C().m()}|${S().use()} ${S::st()}|${A().v}|${(Vec(1.0) + Vec{3.0}).x}`;
			)", "Cannot cast 'Cat' to 'Tiger&'|59|7 4|1|4.0");
		});
		test("guide_container_and_string_examples", [this]() {
			expect("containers", R"(
				auto g = [1, 2, 3];
				string c = "";
				try { g.push("s"); } catch (e) { } try { g.push("t"); } catch (e) { c = e; }
				map<var, int> anything = {}; anything[5] = 1; anything["five"] = 5;
				var s = "Jai\x21"; var word = (s[0] + 0) | ((s[1] + 0) << 8);
				var e = ""; try { var z = 10 / 0; } catch (x) { e = x; }
				c + "|" + to_json(g) + "|" + to_json(anything) + "|" + to_string("b".compare("a")) + "|" + to_string(word) + "|" + e + "|" + `[${"ab":^5}] [${7:*^4}]`;
			)", "Cannot push 'string' to array<int>|[1,2,3]|{\"$map\":[[5,1],[\"five\",5]]}|1|24906|Division by zero|[ ab  ] [*7**]");
		});
		test("guide_parallel_transform_rows", [this]() {
			expect("transform", R"(
				var rows = [[1, 10], [2, 20]];
				var f(var p) { return [p[1], p[0] * 2]; }
				to_json(parallel_transform(rows, f));
			)", "[[10,2],[20,4]]");
		});
	}

	// Parallel contracts
	void parallel_contracts() {
		test("a_helper_that_writes_a_global_hits_the_parallel_write_wall", [this]() {
			// a callee that mutates a global hits the same wall; the source is unchanged
			expect("transitive write", R"(
				var g = 0;
				function helper(int v) -> int { g = v; return v; }
				array<int> a = [1, 2, 3];
				string r = "ok";
				try { parallel_for (auto& x : a) { x = helper(x); } } catch (e) { r = e; }
				`${to_string(r.contains("enclosing state 'g'"))}|${to_json(a)}|${g}`;
			)", "true|[1,2,3]|0");
		});
		test("cells_in_a_parallel_worker_match_the_serial_rows", [this]() {
			expect("worker cells", R"(
				array<int> G = [3, 1, 4, 1, 5, 9, 2, 6];
				function f(int row) -> int { int acc = 0; for (int k = 0; k < 8; ++k) { acc = acc + G[(row + k) % 8] * k; } return acc; }
				var rows = []; for (int i = 0; i < 64; i++) { rows.push(i); }
				var par = parallel_transform(rows, f);
				int bad = 0; for (int i = 0; i < 64; i++) { if (par[i] != f(i)) { bad++; } }
				`${bad}|${par[0]}`;
			)", "0|131");
		});
		test("parallel_for_iterates_arrays_only", [this]() {
			expect("map range", R"(
				var m = {"a": 1};
				string r = "ran";
				try { parallel_for (auto& kv : m) { } } catch (e) { r = "raised"; }
				r;
			)", "raised");
		});
	}

	void native_exceptions() {
		test("a_bound_accessor_method_or_operator_that_throws_is_catchable_where_it_is_called", [this]() {
			// every host exception at the boundary is the catchable runtime error with its text, at top level and inside a
			// function, and an operand the operator cannot take raises the overload text instead of escaping
			const std::string src = R"(
				function t(function f) -> string { string r = "none"; try { f(); } catch (e) { r = e; } return r; }
				string top = "none";
				try { Probe q = Probe(); q.v = -1; } catch (e) { top = e; }
				string in_fn() { Probe p = Probe(); string r = "none"; try { p.v = -2; } catch (e) { r = e; } return `${r}:${p.v}`; }
				Probe p = Probe();
				`${top}|${in_fn()}|${t([]() { Probe a = Probe(); a.v = -3; })}|${t([]() { Probe a = Probe(); a.lv = 500; })}|${t([]() { Probe a = Probe(); var x = a.bad; })}|${t([]() { Probe a = Probe(); a.poke(0); })}|${t([]() { Probe a = Probe(); a.v += -9; })}|${t([]() { Probe a = Probe(); var b = a * -1; })}|${(t([]() { Probe a = Probe(); var b = a + 5; }) != "none" ? "operand raised" : "operand accepted")}|${p.v}`;
			)";
			every_config("native boundary", "negative v|negative v:1|negative v|lv too big|getter failed|poke zero|negative v|negative scale|operand raised|1",
				[&](engine& e) { spec::bind_boundary_probe(e); return spec::render(e.execute(spec::in_warn_region(src))); });
		});
		test("a_host_script_exception_is_catchable_with_its_message", [this]() {
			every_config("host script_exception", "Random failure!", [](engine& e) {
				e.add_function("raise_it", []() -> script_int { throw jai::script_exception("Random failure!"); });
				return e.execute("var r = \"\"; try { raise_it(); } catch (x) { r = x; } r;").as_string();
			});
		});
	}

	// Access control (two levels, as simple as possible): public: = everyone; private: = the declaring
	// class and every class derived from it, by any spelling and through any receiver of the family; protected: is a synonym
	// of private: (one message). Outside the family every read, write, call, compound and ++/-- raises
	// "Cannot access private member 'x' of class 'C'"; the strict checker reports it at load when the receiver class is
	// statically known (a warning in warn mode), with the same text
	void access_control() {
		const char* W = R"(
			class Wallet {
			private:
				int secret = 42;
				int hidden() { return 7; }
				static int vault = 5;
			protected:
				int prot = 3;
			public:
				int own_other(Wallet w) { return w.secret; }
				int peek_sub() { return tip; }
			}
			class Tipper : Wallet { private: int tip = 9; }
			class Clip : Wallet { }
			function bump(int& r) { r = r + 1; }
			class Purse : Wallet {
				int peek_secret() { return secret; }
				int poke_secret() { secret = 9; return secret; }
				int compound_secret() { secret += 1; return secret; }
				int inc_secret() { secret++; return secret; }
				int call_hidden() { return hidden() + super::hidden(); }
				int ref_secret() { auto& r = secret; r = 50; return secret; }
				int pass_ref() { bump(secret); return secret; }
				int lambda_secret() { auto l = [=]() { return secret; }; return l(); }
				int binary_secret() { return secret + 1; }
				int this_secret() { return this.secret; }
				int other(Wallet w) { return w.secret; }
				int sibling(Clip c) { return c.secret; }
				int statics() { return Wallet::vault; }
				int prot_synonym() { prot += 1; return prot; }
			}
			class Thief { int steal(Wallet w) { return w.secret; } }
			function t(function f) -> string {
				string r = "";
				try { r = `${f()}`; }
				catch (e) {
					r = e == "Cannot access private member 'secret' of class 'Wallet'" ? "S"
					  : e == "Cannot access private member 'hidden' of class 'Wallet'" ? "H"
					  : e == "Cannot access private member 'vault' of class 'Wallet'" ? "V"
					  : e == "Cannot access private member 'prot' of class 'Wallet'" ? "P" : e;
				}
				return r + ",";
			}
		)";
		test("the_declaring_class_and_every_derived_class_reach_a_private_member_by_any_spelling", [this, W]() {
			expect("family", std::string(W) + R"(
				t([]() { return Purse().peek_secret(); }) + t([]() { return Purse().poke_secret(); }) +
				t([]() { return Purse().compound_secret(); }) + t([]() { return Purse().inc_secret(); }) +
				t([]() { return Purse().call_hidden(); }) + t([]() { return Purse().ref_secret(); }) +
				t([]() { return Purse().pass_ref(); }) + t([]() { return Purse().lambda_secret(); }) +
				t([]() { return Purse().binary_secret(); }) + t([]() { return Purse().this_secret(); }) +
				t([]() { return Purse().other(Wallet()); }) + t([]() { return Purse().sibling(Clip()); }) +
				t([]() { return Purse().statics(); }) + t([]() { return Purse().prot_synonym(); }) +
				t([]() { return Wallet().own_other(Purse()); });
			)", "42,9,43,43,14,50,43,42,43,42,42,42,5,4,42,", warn_region);
		});
		test("outside_the_family_every_spelling_raises_the_one_private_text", [this, W]() {
			expect("outside", std::string(W) + R"(
				t([]() { return Wallet().secret; }) + t([]() { Wallet w = Wallet(); w.secret = 1; return "w"; }) +
				t([]() { Wallet w = Wallet(); w.secret += 1; return "c"; }) + t([]() { Wallet w = Wallet(); w.secret++; return "i"; }) +
				t([]() { return Wallet().hidden(); }) + t([]() { return Wallet::vault; }) + t([]() { return Wallet().prot; }) +
				t([]() { return Thief().steal(Wallet()); }) + t([]() { var v = Purse(); return v.secret; }) +
				t([]() { return Tipper().peek_sub(); });
			)", "S,S,S,S,H,V,P,S,S,Cannot access private member 'tip' of class 'Tipper',", warn_region);
		});
		test("the_strict_checker_reports_an_outside_access_at_load_with_the_runtime_text", [this]() {
			every_config("checker", "1:Cannot access private member 'secret' of class 'Wallet'|0|0|0|1:Cannot access private member 'prot' of class 'Wallet'|"
			             "1:Cannot access private member 'secret' of class 'Wallet'|1:Cannot access private member 'vault' of class 'Wallet'|0|0", [](engine& e) {
				const std::string base = "class Wallet { private: int secret = 42; int hidden() { return 7; } static int vault = 5; protected: int prot = 3; } ";
				auto one = [&](const std::string& src) {
					const auto rep = e.check(base + src);
					std::string first;
					for (const auto& d : rep.diagnostics) { if (d.severity == check_diagnostic::level::error) { first = d.message; break; } }
					return std::to_string(rep.error_count()) + (first.empty() ? "" : ":" + first);
				};
				return one("int peek(Wallet w) { return w.secret; }") + "|" +
				       one("class Purse : Wallet { int peek() { return secret + hidden() + prot; } }") + "|" +
				       one("class Purse : Wallet { int peek(Wallet w) { return w.secret + this.secret + Wallet::vault; } }") + "|" +
				       one("class Purse : Wallet { } int f() { return 1; }") + "|" +
				       one("int peek(Wallet w) { return w.prot; }") + "|" +
				       one("class Thief { int steal(Wallet w) { return w.secret; } }") + "|" +
				       one("int f() { return Wallet::vault; }") + "|" +
				       one("int f() { var v = Wallet(); return v.secret; }") + "|" +
				       one("class Other { int n = 1; } int g(Other o) { return o.n; }");
			});
		});
		test("a_strict_load_with_an_outside_access_runs_nothing_and_warn_mode_runs_to_the_raise", [this]() {
			every_config("modes", "raised|0|S", [](engine& e) {
				e.add_global("ran", script_value(script_int(0), &e));
				const std::string src = "class Wallet { private: int secret = 42; } ran = 1; int peek(Wallet w) { return w.secret; } ";
				std::string out;
				try { e.execute(src); out = "loaded"; } catch (const std::exception&) { out = "raised"; }
				out += "|" + std::to_string(e.execute("ran").as_int());
				out += "|" + e.execute(spec::in_warn_region(src) + "string r = \"\"; try { peek(Wallet()); } catch (x) { r = x == \"Cannot access private member 'secret' of class 'Wallet'\" ? \"S\" : x; } r;").as_string();
				return out;
			});
		});
	}

	// Access control reaches constructors: an overload declared under private: (or protected:) is callable from its class and
	// every class derived from it (a static factory, a method, super(...), a derived class's implicit base construction), and
	// from nowhere else, by any spelling: Name(...), new, a declaration, an implicit conversion, reflect::construct. Outside
	// the family it raises "Cannot access private constructor of class 'C'", checked after overload resolution, so a public
	// overload of the same class stays reachable. An archive load constructs regardless, as reads see private state
	void private_constructors() {
		const char* K = R"(
			class Token {
			private:
				Token(int v) { value = v; }
				Token() { value = -1; }
			public:
				int value = 0;
				static Token make(int v) { return Token(v); }
				Token twin() { return Token(value + 1); }
			}
			class Special : Token {
				Special() : super(7) {}
				Token peer() { return Token(3); }
			}
			class Kid : Token { }
			class Stranger { Token grab() { return Token(1); } }
			class Mixed {
			public:
				Mixed(int a) { n = a; }
			protected:
				Mixed(string s) { n = 99; }
			public:
				int n = 0;
			}
			class Money {
			private:
				Money(int c) { cents = c; }
			public:
				int cents = 0;
				static Money of(int c) { Money m = c; return m; }
			}
			class Saved {
			private:
				Saved() {}
			public:
				int n = 1;
				static Saved make() { Saved s = Saved(); s.n = 5; return s; }
			}
			class SavedChild : Saved {
				string name = "kid";
				SavedChild() : super() { n = 4; }
			}
			class OnlyArgs {
			private:
				OnlyArgs(int v) { value = v; }
			public:
				int value = 0;
				string tag = "x";
				static OnlyArgs make(int v) { OnlyArgs o = OnlyArgs(v); o.tag = "made"; return o; }
			}
			class Hidden {
			private:
				Hidden() { hp = 1; }
			public:
				int hp = 0;
			}
			class Shown : Hidden { int dmg = 3; }
			function trip(var v) -> string { return to_json(from_json(to_json(v))) + " " + to_json(from_binary(to_binary(v))); }
			function t(function f) -> string {
				string r = "";
				try { r = `${f()}`; }
				catch (e) {
					r = e == "Cannot access private constructor of class 'Token'" ? "X"
					  : e == "Cannot access private constructor of class 'Mixed'" ? "M"
					  : e == "Cannot access private constructor of class 'Money'" ? "Y" : e;
				}
				return r + ",";
			}
		)";
		test("the_class_family_reaches_a_private_constructor", [this, K]() {
			expect("family", std::string(K) + R"(
				t([]() { return Token::make(5).value; }) + t([]() { return Token::make(1).twin().value; }) +
				t([]() { return Special().value; }) + t([]() { return Special().peer().value; }) + t([]() { return Kid().value; }) +
				t([]() { return Mixed(4).n; }) + t([]() { return Money::of(250).cents; });
			)", "5,2,7,3,-1,4,250,", warn_region);
		});
		test("outside_the_family_every_direct_construction_spelling_raises_the_private_constructor_text", [this, K]() {
			expect("outside", std::string(K) + R"(
				t([]() { return Token(1).value; }) + t([]() { auto p = new Token(1); return p.value; }) +
				t([]() { Token k(2); return k.value; }) + t([]() { Token k; return k.value; }) +
				t([]() { return Stranger().grab().value; }) + t([]() { return reflect::construct("Token", 1).value; }) +
				t([]() { return reflect::construct_shared("Token", 1).value; }) + t([]() { return Mixed("x").n; }) +
				t([]() { Money m = 5; return m.cents; });
			)", "X,X,X,X,X,1,1,M,Y,", warn_region);   // reflect::construct reaches private constructors by design
		});
		// a load reconstitutes the state whatever the constructors' access, and a class no constructor builds without
		// arguments is created without one (its field initializers, then the archive's fields)
		test("an_archive_load_reconstitutes_a_class_whatever_its_constructors", [this, K]() {
			expect("loads", std::string(K) + R"(
				Saved s = Saved::make();
				SavedChild c = SavedChild();
				c.name = "late";
				c.n = 6;
				Shown h = Shown();
				h.hp = 9;
				h.dmg = 5;
				trip(s) + "|" + trip(c) + "|" + trip(OnlyArgs::make(7)) + "|" + trip(h) + "|" + t([]() { return Saved().n; });
			)", "{\"$type\":\"Saved\",\"$val\":{\"n\":5}} {\"$type\":\"Saved\",\"$val\":{\"n\":5}}|"
			    "{\"$type\":\"SavedChild\",\"$val\":{\"n\":6,\"name\":\"late\"}} {\"$type\":\"SavedChild\",\"$val\":{\"n\":6,\"name\":\"late\"}}|"
			    "{\"$type\":\"OnlyArgs\",\"$val\":{\"value\":7,\"tag\":\"made\"}} {\"$type\":\"OnlyArgs\",\"$val\":{\"value\":7,\"tag\":\"made\"}}|"
			    "{\"$type\":\"Shown\",\"$val\":{\"hp\":9,\"dmg\":5}} {\"$type\":\"Shown\",\"$val\":{\"hp\":9,\"dmg\":5}}|"
			    "Cannot access private constructor of class 'Saved',", warn_region);
		});
		test("the_strict_checker_reports_an_outside_private_construction_at_load_with_the_runtime_text", [this]() {
			every_config("checker", "1:Cannot access private constructor of class 'Token'|0|0|1:Cannot access private constructor of class 'Token'|"
			             "0|1:Cannot access private constructor of class 'Mixed'|0", [](engine& e) {
				const std::string base = "class Token { private: Token(int v) { value = v; } public: int value = 0; static Token make(int v) { return Token(v); } } "
				                         "class Mixed { public: Mixed(int a) { n = a; } private: Mixed(string s) { n = 2; } public: int n = 0; } ";
				auto one = [&](const std::string& src) {
					const auto rep = e.check(base + src);
					std::string first;
					for (const auto& d : rep.diagnostics) { if (d.severity == check_diagnostic::level::error) { first = d.message; break; } }
					return std::to_string(rep.error_count()) + (first.empty() ? "" : ":" + first);
				};
				return one("int f() { return Token(1).value; }") + "|" +
				       one("class Sub : Token { Sub() : super(1) {} int g() { return Token(2).value; } }") + "|" +
				       one("int f() { return Token::make(1).value; }") + "|" +
				       one("class Thief { int steal() { return Token(3).value; } }") + "|" +
				       one("int f() { return Mixed(4).n; }") + "|" +
				       one("int f() { return Mixed(\"s\").n; }") + "|" +
				       one("int f() { return 1; }");
			});
		});
	}

	// docs/site/guide/06-functions.html "Lambdas and captures": a program-scope variable is never captured by default - [=],
	// [&] and bare [] read it when the lambda runs, [&g] is the same as not listing it, an explicit [g] copies it at creation;
	// an init-capture [x = e] copies e, evaluated at creation in the enclosing scope, into the lambda's own x. Locals and
	// parameters keep their contract (bare [] and [=] copy, [&] shares the cell)
	void lambda_globals_and_init_captures() {
		test("a_global_resolves_when_the_lambda_runs_unless_an_explicit_capture_copies_it", [this]() {
			expect("globals", R"(
				var g = 1;
				function t1() -> int { auto l = [=]() { return g; }; g = 5; return l(); }
				function t2() -> int { auto l = []() { return g; }; g = 6; return l(); }
				function t3() -> int { auto l = [g]() { return g; }; g = 7; return l(); }
				function t4() -> int { auto l = [&g]() { return g; }; g = 8; return l(); }
				function t5() -> int { auto l = [x = g]() { return x; }; g = 9; return l(); }
				function t6() -> int { auto l = [&]() { g = 10; }; l(); return g; }
				function t7() -> int { auto l = [=]() { g = 11; }; l(); return g; }
				function t8() -> int { int k = 1; auto l = [=]() { return k; }; k = 2; return l(); }
				function t9() -> int { int k = 1; auto l = [&]() { return k; }; k = 2; return l(); }
				`${t1()},${t2()},${t3()},${t4()},${t5()},${t6()},${t7()},${t8()},${t9()}`;
			)", "5,6,6,8,8,10,11,1,2");
		});
		test("a_top_level_lambda_reads_a_global_when_it_runs", [this]() {
			expect("top level", R"(
				var h = 1; auto lh = [=]() { return h; }; h = 2;
				var h2 = 1; auto lc = [h2]() { return h2; }; h2 = 2;
				function make() { return [=]() { return h; }; }
				auto late = make(); h = 3;
				`${lh()},${lc()},${late()}`;
			)", "3,1,3");
		});
		test("an_init_capture_copies_its_expression_at_creation", [this]() {
			expect("init captures", R"(
				function run() -> string {
					int x = 1; int y = 2;
					auto a = [x = y * 10]() { return x; };
					auto b = [z = x + y, &y]() { y = 100; return z; };
					int bz = b();
					string s = "q";
					auto c = [s2 = s + "!"]() { return s2; };
					[[warn_mode]] s = "changed";
					array<int> arr = [1];
					auto d = [cp = arr]() { return cp.size(); };
					[[warn_mode]] arr.push(2);
					auto e = [=, w = 5]() { return w + x; };
					return `${a()},${bz},${y},${c()},${x},${d()},${e()}`;
				}
				run();
			)", "20,3,100,q!,1,1,6");
		});
		test("a_reference_capture_takes_no_initializer", [this]() {
			every_config("ref init", "parse error", [](engine& e) {
				try { e.execute("int x = 1; auto l = [&r = x]() { return r; };"); return std::string("accepted"); }
				catch (const std::exception& ex) {
					const std::string w = ex.what();
					return std::string(w.find("a reference capture takes no initializer") != std::string::npos ? "parse error" : w);
				}
			});
		});
	}

	// C++17 [expr.ass]: in E1 op= E2 every value computation and side effect of E2 is sequenced before
	// E1 - the right side runs first, then the target's receiver, its read (a user getter included), the arithmetic and the store
	void compound_member_order() {
		test("a_member_compound_evaluates_its_right_side_before_the_target", [this]() {
			expect("member order", R"(
				string LOG = ""; int K = 0;
				class O { int f = 1; static int s = 1; int self_add() { this.f += (f = 10); return f; } }
				namespace ns { int x = 1; }
				O G = O();
				O& obj() { LOG += `t${K}`; return G; }
				class H { int f = 1; int _get_g() { LOG += `g${K}`; return f; } void _set_g(int v) { LOG += `s${K}`; f = v; } }
				H hh = H();
				function t(function f) -> string { LOG = ""; K = 0; f(); return LOG; }
				function field() -> int { O o = O(); o.f += (o.f = 10); return o.f; }
				t([]() { obj().f += ++K; }) + "|" + t([]() { hh.g += ++K; }) + "|" +
				t([]() { O::s += (K = 3); LOG += `${O::s}`; }) + "|" + t([]() { ns::x += (K = 4); LOG += `${ns::x}`; }) + "|" +
				to_string(field()) + "|" + to_string(O().self_add());
			)", "t1|g1s1|4|5|20|20", warn_region);
		});
		test("a_right_side_error_comes_before_the_targets", [this]() {
			expect("member error order", R"(
				function bad() -> int { throw "rhs"; return 1; }
				namespace ns { int x = 1; }
				function t(function f) -> string { string r = "none"; try { f(); } catch (e) { r = e; } return r; }
				t([]() { var nv = null; nv.f += bad(); }) + "|" + t([]() { ns::missing += bad(); }) + "|" + t([]() { var nv = null; nv.f += 1; });
			)", "rhs|rhs|Cannot access 'f' on null ('nv' is null)", warn_region);
		});
	}

	// C++17 [expr.ass]: a[j] op= e evaluates e before a and j (as the plain store does)
	void compound_subscript_order() {
		test("a_subscript_compound_evaluates_its_right_side_before_the_container_and_index", [this]() {
			expect("subscript order", R"(
				string LOG = ""; int K = 0;
				array<int> A = [1, 2, 3];
				map<string, int> M = {"k": 1};
				function idx() -> int { LOG += `j${K}`; return 1; }
				function key() -> string { LOG += `k${K}`; return "k"; }
				function t(function f) -> string { LOG = ""; K = 0; f(); return LOG; }
				function self_write() -> int { array<int> a = [1, 2, 3]; a[1] += (a[1] = 10); return a[1]; }
				function index_write() -> string { array<int> a = [1, 2, 3]; int i = 0; a[i] += (i = 2); return `${a[0]}${a[2]}`; }
				`${t([]() { A[idx()] += ++K; })}|${t([]() { A[idx()] = ++K; })}|${t([]() { M[key()] += ++K; })}|${self_write()}|${index_write()}`;
			)", "j1|j1|k1|20|15", warn_region);
		});
		test("a_right_side_error_comes_before_the_subscripts", [this]() {
			expect("subscript error order", R"(
				string LOG = "";
				function bad() -> int { throw "rhs"; return 1; }
				function idx() -> int { LOG += "j"; return 1; }
				array<int> A = [1, 2, 3];
				function t(function f) -> string { string r = "none"; LOG = ""; try { f(); } catch (e) { r = e; } return r + LOG; }
				t([]() { var na = null; na[idx()] += bad(); }) + "|" + t([]() { A[5] += bad(); }) + "|" + t([]() { A[5] += 1; });
			)", "rhs|rhs|Array index 5 out of bounds for array of size 3", warn_region);
		});
	}

	void auto_static_type() {
		test("an_auto_from_a_var_or_unknown_static_type_is_a_var", [this]() {
			// `auto x = e` takes e's static type; a var element, a var local, a var-returning call and a
			// mixed-branch ternary other than int with float have none, so x is a var (a later store of another type lands as is)
			const char* src = R"(
				var arr = [1, 2];
				array<var> av = [1];
				var v = 5;
				function f() -> var { return 5; }
				bool c = true;
				auto x = arr[0]; x = "s";
				auto y = av[0]; y = 2.5;
				auto z = v; z = "t";
				auto w = f(); w = 1.5;
				auto m = c ? 1 : "q"; m = 2.5;
				`${type_of(x)}:${x}|${type_of(y)}:${y}|${type_of(z)}:${z}|${type_of(w)}:${w}|${type_of(m)}:${m}`;
			)";
			const std::string want = "string:s|float:2.5|string:t|float:1.5|float:2.5";
			expect("auto from var (warn region)", src, want, warn_region);
			expect("auto from var (strict)", src, want);
		});
		test("an_auto_from_a_known_static_type_keeps_that_type", [this]() {
			const char* src = R"(
				array<int> ai = [1, 2];
				class P { int n = 0; }
				auto n = 5; n = true;
				auto e = ai[0]; e = false;
				auto s = "a"; s = `b${5}`;
				auto p = P();
				function t(function g) -> string { string r = "none"; try { g(); } catch (err) { r = err; } return r; }
				`${type_of(n)}:${n}|${type_of(e)}:${e}|${s}|${t([]() { auto q = P(); var o = 5; q = o; })}|${t([]() { auto k = 5; var h = 1.5; k = h; })}|${t([]() { auto k = ai[0]; var h = 2.9; k = h; })}`;
			)";
			expect("auto from known", src, "int:1|int:0|b5|Cannot assign int to 'q' (declared P)|Cannot assign float to 'k' (declared int)|Cannot assign float to 'k' (declared int)", warn_region);
		});
		test("an_auto_in_a_counted_loop_follows_the_static_type_on_every_lane", [this]() {
			expect("auto in loops", R"(
				function from_var() -> string {
					var arr = [1, 2, 3]; string s = "";
					for (int i = 0; i < 3; i++) { auto x = arr[i]; x = x + 0.5; s += `${x} `; }
					return s;
				}
				function from_ints() -> string {
					array<int> arr = [1, 2, 3]; string s = "";
					for (int i = 0; i < 3; i++) { auto x = arr[i]; string kept = ""; try { x = x + 0.5; } catch (e) { kept = "!"; } s += `${x}${kept} `; }
					return s;
				}
				function sum_var() -> float {
					array<var> arr = [1, 2, 3]; float total = 0.0;
					for (int i = 0; i < 3; i++) { auto x = arr[i]; x += 0.25; total += x; }
					return total;
				}
				`${from_var()}|${from_ints()}|${sum_var()}`;
			)", "1.5 2.5 3.5 |1! 2! 3! |6.75", warn_region);
		});
		test("a_jaibite_keeps_the_auto_spelling_and_types_it_again_at_load", [this]() {
			every_config("auto through jaibite", "string:s|int:1", [](engine& e) {
				auto bite = e.jaibite(spec::in_warn_region("var arr = [1]; auto x = arr[0]; x = \"s\"; auto n = 5; n = true; `${type_of(x)}:${x}|${type_of(n)}:${n}`;"));
				std::string first = spec::render(e.execute(bite));
				auto loaded = e.jaibite_load_bytes(bite.save_bytes());
				std::string second = spec::render(e.execute(loaded));
				return first == second ? first : first + " vs " + second;
			});
		});
	}

	void known_bugs_simplification_audit() {
		test("f2_a_weak_store_yields_the_stored_weak_handle", [this]() {
			// the vm hands back the right-hand side (the shared handle) of a consumed store into a weak_ptr
			expect("weak store value", R"(
				class Foo { int v = 1; }
				shared_ptr<Foo> sp = Foo();
				weak_ptr<Foo> w = sp;
				auto h = (w = sp);
				type_of(h);
			)", "weak_ptr", warn_region);
		});
		test("f3_a_store_takes_its_right_sides_value_before_the_target", [this]() {
			// C++17: the right operand of every assignment operator, its value included, before the target
			expect("store value timing", R"(
				array<int> a = [0, 0];
				array<int> b = [1, 2];
				function g() -> int { b[1] = 99; return 0; }
				a[g()] = b[1];
				string s = `${a[0]}`;
				b[1] = 2; a[0] = 0;
				a[g()] += b[1];
				s + " " + a[0];
			)", "2 2", warn_region);
		});
		test("f4_a_container_literal_reads_its_elements_left_to_right", [this]() {
			expect("literal order", R"(
				var A = [1];
				class O { int operator+(int x) { A[0] = 100; return 0; } }
				var o = O();
				function f() { var r = [A[0], o + 1]; return r[0]; }
				"" + f();
			)", "1", warn_region);
		});
		test("f5_a_user_operator_assign_runs_at_every_store_into_a_bare_field", [this]() {
			expect("operator= in a method loop", R"(
				var assigns = 0;
				class V { int v = 0; function operator=(int x) -> V { assigns = assigns + 1; v = x; return this; } }
				class H { V f = V(); void set_many() { for (int i = 0; i < 4; i = i + 1) { f = i; } } }
				var h = H();
				h.set_many();
				`${assigns} ${h.f.v}`;
			)", "4 3", warn_region);
		});
		test("f7_an_argument_ahead_of_a_class_operator_takes_its_value_when_evaluated", [this]() {
			// invariant 13: a later argument that runs user code does not change an earlier by-value argument
			expect("capture ahead of operator", R"(
				var G = [1, 2];
				class O { int operator+(int x) { G.push(9); return 0; } }
				var o = O();
				function sz(var a, int b) { return a.size() + b; }
				"" + sz(G, o + 1);
			)", "2", warn_region);
		});
	}

	// Divergences from the specification, each pinned here by the change that fixes it
	void simplification_audit_fixes() {
		test("f1_a_nested_assignment_yields_the_destinations_stored_value", [this]() {
			// The value of an assignment is the destination's stored value, consumed anywhere
			expect("nested stored value", R"(
				class C { float n = 0.0; static float s = 0.0; }
				function id(var v) { return v; }
				C c = C(); array<float> a = [0.0];
				var r1 = 0; r1 = (c.n = 3);
				var r2 = 0; r2 = (a[0] = 3);
				string r3 = to_string(a[0] = 4);
				var r4 = id(C::s = 5);
				`${type_of(r1)}:${r1} ${type_of(r2)}:${r2} ${r3} ${type_of(r4)}:${r4}`;
			)", "float:3.0 float:3.0 4.0 float:5.0", warn_region);
		});
		test("f2_a_handle_store_yields_a_value", [this]() {
			expect("handle store value", R"(
				class Foo { int v = 1; }
				shared_ptr<Foo> sp = Foo();
				weak_ptr<Foo> w = sp;
				auto r1 = (w = null);
				auto r2 = (w = sp);
				shared_ptr<Foo> s2 = Foo();
				auto r3 = (s2 = null);
				`${(r1 == null)} ${(r2 != null)} ${(r3 == null)}`;
			)", "true true true", warn_region);
		});
		test("f6_a_coroutine_whose_body_threw_is_completed", [this]() {
			// a native exception escaping the body fails the handle like a script throw; the resumer's state comes back
			expect("coroutine after a throw", R"(
				coroutine g() { throw; yield 1; }
				coroutine k() { throw "boom"; yield 1; }
				coroutine m() { yield 1; throw; yield 2; }
				function t(var h) -> string { string r = ""; try { h.next(); } catch (e) { r = e; } try { h.next(); } catch (e) { r = r + "|" + e; } return r; }
				function u(var h) -> string { h.next(); return t(h); }
				string out = t(g()) + " / " + t(k()) + " / " + u(m());
				try { throw "outer"; } catch (e) { out += " / " + e; }
				out;
			)", "Bare throw with no active exception thrown.|Cannot resume a completed coroutine / boom|Cannot resume a completed coroutine / "
			    "Bare throw with no active exception thrown.|Cannot resume a completed coroutine / outer", warn_region);
		});
		test("f8_a_by_value_parameter_does_not_see_user_code_write_its_argument", [this]() {
			// a by-value parameter holds a copy: user code the callee runs (a method on another local, a getter of the
			// declared class or of a subclass the argument is, an operator, a conversion) never writes through it
			expect("parameter alias proof", R"(
				class Box { int n = 1; }
				Box G = Box();
				class P { void poke() { G.n = 5; } }
				P GQ = P();
				function f(Box p) -> int { var q = GQ; q.poke(); return p.n; }
				function f2(Box p) -> int { P q = GQ; q.poke(); return p.n; }
				class B { int x = 1; int y = 1; int _get_x() { GB.y = 5; return 3; } }
				B GB = B();
				function g(B p) -> int { int a = p.x; return a * 10 + p.y; }
				class C { int x = 1; int y = 1; }
				class D : C { int _get_x() { GC.y = 5; return 3; } }
				C GC = D();
				function h(C p) -> int { int a = p.x; return a * 10 + p.y; }
				class K { int v = 0; int operator+(int k) { G.n = 9; return k; } bool to_bool() { G.n = 8; return true; } }
				K GK = K();
				function i1(Box p) -> int { K k = GK; int t = k + 1; return p.n; }
				function i2(Box p) -> int { K k = GK; if (k) { return p.n; } return 0; }
				string s = `${f(G)}`;
				G.n = 1; s += ` ${f2(G)}`;
				s += ` ${g(GB)}`;
				s += ` ${h(GC)}`;
				G.n = 1; s += ` ${i1(G)}`;
				G.n = 1; s += ` ${i2(G)}`;
				s;
			)", "1 1 31 31 1 1", warn_region);
		});
	}

	// Re-entrant destructors, a parked callee's release, the operand order a later reference binding sees, and the
	// destructors a throw's unwinding releases
	void reentrant_destructors_and_operand_order() {
		test("a_destructor_that_reenters_while_a_frame_releases_its_loop_state_runs_to_completion", [this]() {
			// C++: the last handle's release runs the destructor to completion and the caller continues intact. The released
			// loop state leaves the VM's vector before it dies: the destructor's own range-for pushes onto that vector, and at
			// every depth where its size equals its capacity (1 2 3 4 6 9) the push reallocates it
			for (int depth = 1; depth <= 10; ++depth) {
				std::string src = "var log = [];\n"
					"class Probe { int id = 0; Probe(int i) { id = i; } ~Probe() { for (auto y : [1, 2]) { log.push(id * 10 + y); } } }\n"
					"function mk() { return [new Probe(1)]; }\n"
					"function f() {\n\tfor (auto a : mk()) {\n";
				for (int i = 1; i < depth; ++i) { src += "\tfor (auto b" + std::to_string(i) + " : [7]) {\n"; }
				src += "\treturn 5;\n";
				for (int i = 1; i < depth; ++i) { src += "\t}\n"; }
				src += "\t}\n\treturn 0;\n}\nlog.push(f());\nto_json(log);";
				expect("loop state, depth " + std::to_string(depth), src, "[11,12,5]");
			}
		});
		test("a_destructor_that_reenters_while_the_value_stack_truncates_runs_to_completion", [this]() {
			// a popped window's heavy slot leaves the value stack before it dies: the destructor's call grows the stack at a
			// recursion depth that leaves the count just under a capacity step (4096, 8192, 16384); the range crosses each step
			// (the unfixed fault shows under ASan only)
			std::string wide = "function wide() -> int { ";
			std::string rec = "function rec(int n) -> int { ";
			for (int i = 0; i < 400; ++i) {
				wide += "int w" + std::to_string(i) + " = " + std::to_string(i) + "; ";
				rec += "int v" + std::to_string(i) + " = n; ";
			}
			wide += "return w399; }\n";
			rec += "if (n == 0) { auto p = new Probe(1); return v1; } return rec(n - 1) + v399; }\n";
			for (int depth = 6; depth <= 44; ++depth) {
				const std::string src = "var log = [];\n" + wide +
					"class Probe { int id = 0; Probe(int i) { id = i; } ~Probe() { log.push(wide()); } }\n" + rec +
					"log.push(rec(" + std::to_string(depth) + "));\nto_json(log);";
				expect("value stack, depth " + std::to_string(depth), src, "[399," + std::to_string(depth * (depth + 1) / 2) + "]");
			}
		});
		test("a_callee_parked_by_an_execute_that_failed_is_released", [this]() {
			// the closure a call parked when the execute ended uncaught is released with the run, so the captured instance dies
			// with the program's last handle
			every_config("pending callee", "[1]", [](engine& e) {
				e.execute("var log = []; class Probe { ~Probe() { log.push(1); } } var p = new Probe(); var f = [p]() { return 1; }; p = null;"
				          " function boom() { throw \"x\"; return 0; }");
				try { e.execute("f(boom());"); } catch (const std::exception&) {}
				e.execute("f = null;");
				return e.execute("to_json(log);").as_string();
			});
		});
		test("an_operand_a_later_call_binds_by_reference_is_read_first", [this]() {
			// invariant 12 / C++17: the left operand takes its value before the right side runs. The VM reads a private local
			// inside the fused op after the call on its right unless that call's arguments may write it: a reference-binding
			// call under an assignment, a literal or a new expression, and a store or ++/-- naming it directly
			expect("slot argument", R"(
				function bump(int& r) -> int { r = 100; return 1; }
				function g(v) -> int { return 0; }
				class C { int v = 0; C(int& r) { r = 100; v = 1; } }
				function b1() -> int { int x = 1; int t = 0; return x + g(t = bump(x)); }
				function b2() -> int { int x = 1; return x + g([bump(x)]); }
				function b3() -> int { int x = 1; return x + g(new C(x)); }
				function b4() -> int { int x = 1; return x + g({"k": bump(x)}); }
				function b5() -> int { int x = 1; int t = 0; return x + g(t += bump(x)); }
				function d1() -> int { int x = 1; return x + g(x = 5); }
				function d2() -> int { int x = 1; return x + g(x++); }
				function d3() -> int { int x = 1; return x + g(++x); }
				function d4() -> int { int x = 1; return x + g(x += 7); }
				function d5() -> int { int x = 1; return x + g([x = 5]); }
				`${b1()},${b2()},${b3()},${b4()},${b5()}|${d1()},${d2()},${d3()},${d4()},${d5()}`;
			)", "1,1,1,1,1|1,1,1,1,1");
		});
		test("a_throw_runs_the_destructors_of_the_frames_it_unwinds", [this]() {
			// C++ [except.ctor]: stack unwinding destroys the automatic objects of every frame it leaves, as a run-time error
			// does; the destructor runs under its own exception state (its own try/catch works) and the throw resumes after it
			expect("throw unwind", R"(
				var log = [];
				class Probe { int id = 0; Probe(int i) { id = i; } ~Probe() { log.push(id); } }
				class Guarded { ~Guarded() { try { throw "inner"; } catch (e) { log.push("caught " + e); } } }
				class Raiser { ~Raiser() { log.push("raiser"); throw "from dtor"; } }
				function f(int n) -> int { auto p = new Probe(n); throw n; return 0; }
				function k(int n) -> int { auto p = new Probe(n); int z = [1][5]; return 0; }
				function nested(int n) -> int { auto q = new Probe(n); return f(n + 1); }
				function guarded() -> int { auto g = new Guarded(); throw "outer"; return 0; }
				function raiser() -> int { auto r = new Raiser(); throw "outer"; return 0; }
				try { k(1); } catch (e) { log.push(-1); }
				try { f(2); } catch (e) { log.push(-2); }
				try { nested(10); } catch (e) { log.push(-10); }
				try { guarded(); } catch (e) { log.push("then " + e); }
				try { raiser(); } catch (e) { log.push("then " + e); }
				to_json(log);
			)", "[1,-1,2,-2,11,10,-10,\"caught inner\",\"then outer\",\"raiser\",\"then outer\"]");
		});
	}

	// A typed shared_ptr<U> enters shared_ptr<T> by its STATIC pointee, C++-style - an upcast
	// converts, a downcast needs cast<shared_ptr<T>>; a var source by the class its object really has (T or a subclass);
	// an unrelated class never. One rule at every site, both backends
	void handle_admission() {
		test("a_shared_ptr_slot_admits_by_the_static_type_or_a_var_sources_class_at_every_site", [this]() {
			const char* sources[][2] = {
				{"down", "shared_ptr<B> s = new D();"},
				{"var_d", "var s = new D();"},
				{"var_e", "var s = new E();"},
				{"cast", "shared_ptr<B> b0 = new D(); auto s = cast<shared_ptr<D>>(b0);"},
			};
			const char* d_sites[] = {
				"shared_ptr<D> d = s;", "shared_ptr<D> d = null; d = s;", "GD = s;", "ns::d = s;", "H h = H(); h.d = s;",
				"take_d(s);", "array<shared_ptr<D>> a = [null]; a[0] = s;",
			};
			const char* b_sites[] = {
				"shared_ptr<B> d = s;", "shared_ptr<B> d = null; d = s;", "GB = s;", "ns::b = s;", "H h = H(); h.b = s;",
				"take_b(s);", "array<shared_ptr<B>> a = [null]; a[0] = s;",
			};
			std::string src = R"(
				class B { int x = 1; }
				class D : B { int y = 2; }
				class E : B { int z = 3; }
				class H { shared_ptr<D> d = null; shared_ptr<B> b = null; }
				namespace ns { shared_ptr<D> d = null; shared_ptr<B> b = null; }
				shared_ptr<D> GD = null;
				shared_ptr<B> GB = null;
				function take_d(shared_ptr<D> p) -> int { return 1; }
				function take_b(shared_ptr<B> p) -> int { return 1; }
				function t(function f) -> string { string r = "+"; try { f(); } catch (e) { r = "-"; } return r; }
				function ret_down() -> shared_ptr<D> { shared_ptr<B> b = new D(); return b; }
				function ret_up() -> shared_ptr<B> { shared_ptr<D> d = new D(); return d; }
				string out = "";
			)";
			for (const auto& s : sources) {
				src += std::string("out += \"") + s[0] + ":\";\n";
				for (const char* site : d_sites) { src += std::string("out += t([]() { ") + s[1] + " " + site + " });\n"; }
				src += "out += \"|\";\n";
			}
			src += "out += \"up:\";\n";
			for (const char* site : b_sites) { src += std::string("out += t([]() { shared_ptr<D> s = new D(); ") + site + " });\n"; }
			// a slot keeps its declared static type after taking a derived handle; returns convert like the other sites
			src += R"(
				out += "|keep:" + t([]() { shared_ptr<B> bb = null; bb = new D(); shared_ptr<D> d = bb; }) +
				       t([]() { H h = H(); h.b = new D(); shared_ptr<D> d = h.b; }) +
				       t([]() { ns::b = new D(); shared_ptr<D> d = ns::b; });
				out += "|ret:" + t([]() { ret_down(); }) + t([]() { ret_up(); });
				out;
			)";
			expect("handle admission", src, "down:-------|var_d:+++++++|var_e:-------|cast:+++++++|up:+++++++|keep:---|ret:-+", warn_region);
		});
		test("a_shared_ptr_declaration_and_assignment_admit_the_same_handles", [this]() {
			// at program scope as in a function: a statically Base-typed handle holding a Derived enters shared_ptr<Derived> by
			// neither form, and a var holding one enters by both
			expect("handle admission", R"(
				class B { int x = 1; }
				class D : B { int y = 2; }
				shared_ptr<B> gb = new D();
				var gv = gb;
				string out = "";
				try { shared_ptr<D> g1 = gb; out += "+"; } catch (e) { out += "-"; }
				shared_ptr<D> g2 = null;
				try { g2 = gb; out += "+"; } catch (e) { out += "-"; }
				try { shared_ptr<D> g3 = gv; out += "+"; } catch (e) { out += "-"; }
				try { g2 = gv; out += "+"; } catch (e) { out += "-"; }
				out;
			)", "--++", warn_region);
		});
		test("a_handle_admission_rejection_names_the_class_it_judged", [this]() {
			expect("handle texts", R"(
				class B { int x = 1; }
				class D : B { int y = 2; }
				class E : B { int z = 3; }
				class H { shared_ptr<D> d = null; }
				shared_ptr<D> GD = null;
				function t(function f) -> string { string r = ""; try { f(); } catch (e) { r = e; } return r; }
				t([]() { shared_ptr<B> s = new D(); shared_ptr<D> d = s; }) + "|" +
				t([]() { shared_ptr<B> s = new D(); shared_ptr<D> d = null; d = s; }) + "|" +
				t([]() { shared_ptr<B> s = new D(); GD = s; }) + "|" +
				t([]() { var s = new E(); H h = H(); h.d = s; });
			)", "Cannot initialize shared_ptr<D> with B: type must match or be a subclass|Cannot assign B to 'd' (declared shared_ptr<D>)|"
			    "Cannot assign B to 'GD' (declared shared_ptr<D>)|Cannot assign E to field 'd' of H (declared shared_ptr<D>)", warn_region);
		});
		test("the_checker_reports_a_static_downcast_or_an_unrelated_handle_at_load", [this]() {
			// strict: a load error at every site (a shared_ptr<T> namespace variable store included); warn: the unit loads, the runtime raises
			const std::string P = "class B { int x = 1; } class D : B { int y = 2; } class E : B { int z = 3; } class H { shared_ptr<D> hd = null; }\n"
				"namespace ns { shared_ptr<D> nd = null; int n = 0; } shared_ptr<D> GD = null; function take_d(shared_ptr<D> p) -> int { return 1; }\n";
			struct row { const char* label; const char* src; size_t errors; };
			const row rows[] = {
				{"a downcast at every site",
				 "function s1() { shared_ptr<B> s = new D(); shared_ptr<D> d = s; }"
				 "function s2() { shared_ptr<B> s = new D(); shared_ptr<D> d = null; d = s; }"
				 "function s3() { shared_ptr<B> s = new D(); GD = s; }"
				 "function s4() { shared_ptr<B> s = new D(); ns::nd = s; }"
				 "function s5() { shared_ptr<B> s = new D(); H h = H(); h.hd = s; }"
				 "function s6() { shared_ptr<B> s = new D(); take_d(s); }"
				 "function s7() { shared_ptr<B> s = new D(); array<shared_ptr<D>> a = [null]; a[0] = s; }"
				 "function s8() -> shared_ptr<D> { shared_ptr<B> s = new D(); return s; }", 8},
				{"an unrelated class", "function u1() { shared_ptr<E> s = new E(); shared_ptr<D> d = s; } function u2() { shared_ptr<E> s = new E(); ns::nd = s; }", 2},
				{"a cast, a var source and an upcast", "function ok() { shared_ptr<B> s = new D(); shared_ptr<D> d = cast<shared_ptr<D>>(s); var v = s; shared_ptr<D> d2 = v;"
				 " shared_ptr<D> dd = new D(); shared_ptr<B> b = dd; }", 0},
				{"a namespace variable's store checks against its declared type", "function f() { ns::n = \"text\"; ns::n = 2.5; }", 2},
			};
			for (const auto& r : rows) {
				every_config(r.label, std::to_string(r.errors), [&](engine& e) { return std::to_string(e.check(P + r.src).error_count()); });
			}
			every_config("a warn region loads", "loaded|-", [&](engine& e) {
				return e.execute(spec::in_warn_region(P + "function f() { shared_ptr<B> s = new D(); ns::nd = s; }"
				                     " string r = \"loaded|\"; try { f(); r += \"+\"; } catch (ex) { r += \"-\"; } r;")).as_string();
			});
		});
	}

	// guide ch.6 "Recursion": each engine owns its call-depth caps (defaults 256 on the interpreter, 10,000 on the vm); under
	// equal caps both backends raise the same catchable error at the same call (fuzz seed 28571 compared the defaults)
	void recursion_depth_parity() {
		test("equal_call_depth_caps_raise_at_the_same_call_on_both_backends", [this]() {
			struct row { const char* label; const char* src; const char* expected; };
			const row rows[] = {
				{"self at program scope", "function rec6(int n) -> int { if (n <= 0) { return 0; } return n + rec6(n - 1); }\n"
				 "string s = \"\"; for (int k = 250; k <= 258; k++) { try { rec6(k); s += \"+\"; } catch (e) { s += \"-\"; } } s;", "++++++---"},
				{"self inside a function", "function rec6(int n) -> int { if (n <= 0) { return 0; } return n + rec6(n - 1); }\n"
				 "function run() -> string { string s = \"\"; for (int k = 250; k <= 258; k++) { try { rec6(k); s += \"+\"; } catch (e) { s += \"-\"; } } return s; } run();", "+++++----"},
				{"nested in an argument", "function rec6(int n) -> int { if (n <= 0) { return 0; } return n + rec6(n - 1); }\n"
				 "string s = \"\"; for (int k = 18; k <= 24; k++) { try { rec6(rec6(k)); s += \"+\"; } catch (e) { s += \"-\"; } } s;", "+++++--"},
				{"a method", "class R { int go(int n) { if (n <= 0) { return 0; } return 1 + go(n - 1); } }\n"
				 "R r = R(); string s = \"\"; for (int k = 250; k <= 258; k++) { try { r.go(k); s += \"+\"; } catch (e) { s += \"-\"; } } s;", "++++++---"},
				{"a lambda through a global", "var f = null; f = [](int n) -> int { if (n <= 0) { return 0; } return 1 + f(n - 1); };\n"
				 "string s = \"\"; for (int k = 250; k <= 258; k++) { try { f(k); s += \"+\"; } catch (e) { s += \"-\"; } } s;", "++++++---"},
				{"mutual", "function a(int n) -> int { if (n <= 0) { return 0; } return b(n - 1) + 1; } function b(int n) -> int { if (n <= 0) { return 0; } return a(n - 1) + 1; }\n"
				 "string s = \"\"; for (int k = 250; k <= 258; k++) { try { a(k); s += \"+\"; } catch (e) { s += \"-\"; } } s;", "++++++---"},
				// a leaf the vm grafts into its caller makes no frame: called at the cap it raises where the call would, never one call deeper
				{"a leaf grafted at the cap", "function leaf(int x) -> int { return x + 1; }\n"
				 "function rec7(int n) -> int { if (n <= 0) { int v = leaf(n); return v; } return rec7(n - 1) + 1; }\n"
				 "function run() -> string { string s = \"\"; for (int k = 250; k <= 258; k++) { try { rec7(k); s += \"+\"; } catch (e) { s += \"-\"; } } return s; } run();", "++++-----"},
				{"the fuzz seed's shape", "function rec6(int n7) -> int { if (n7 <= 0) { return 0; } return n7 + rec6(n7 - 1); }\n"
				 "string s = \"\"; try { s += `${rec6(rec6(41))}`; } catch (e) { s += \"caught\"; } try { s += \"|\" + rec6(9223372036854775807); } catch (e) { s += \"|caught\"; } s;",
				 "caught|caught"},
			};
			for (const auto& r : rows) {
				every_config(r.label, r.expected, [&](engine& e) {
					e.max_call_depth(backend_type::interpreter, 256);
					e.max_call_depth(backend_type::vm, 256);
					return spec::render(e.execute(spec::in_warn_region(r.src)));
				});
			}
		});
	}

	void known_bugs() {
		known_bugs_simplification_audit();
	}

	// Library rules and language edges
	void library_and_edge_rules() {
		test("a_bare_throw_with_nothing_active_is_a_catchable_runtime_error", [this]() {
			expect("bare throw", R"(
				string r = "none"; try { throw; } catch (e) { r = "top:" + e; }
				function f() -> string { string s = "none"; try { throw; } catch (e) { s = "own:" + e; } return s; }
				function g() -> int { throw; return 1; }
				function h() -> string { string s = "none"; try { g(); } catch (e) { s = "caller:" + e; } return s; }
				r + "|" + f() + "|" + h();
			)", "top:Bare throw with no active exception thrown.|own:Bare throw with no active exception thrown.|caller:Bare throw with no active exception thrown.", warn_region);
		});
		test("a_native_exception_is_caught_by_the_running_functions_own_try", [this]() {
			// docs/site/guide/05-control-flow.html "try / catch / throw": a store through a property reference whose host setter refuses raises natively inside the
			// statement; the function's own try catches it, else it unwinds to the caller's
			const std::string src = R"(
				function f() -> string { Probe a = Probe(); var& r = a.lv; string s = "none"; try { r = 500; } catch (e) { s = `own:${e}`; } return `${s}:${a.lv}`; }
				Probe b = Probe(); var& rb = b.lv; string top = "none"; try { rb = 700; } catch (e) { top = `top:${e}`; }
				function g() -> string { Probe a = Probe(); var& r = a.lv; r = 900; return "unreached"; }
				function h() -> string { string s = "none"; try { g(); } catch (e) { s = `caller:${e}`; } return s; }
				f() + "|" + top + "|" + h();
			)";
			every_config("own try", "own:lv too big:1|top:lv too big|caller:lv too big",
				[&](engine& e) { spec::bind_boundary_probe(e); return spec::render(e.execute(spec::in_warn_region(src))); });
		});
		switch_equality();
		membership_equality();
		compound_operand_text();
		this_capture();
		to_string_builtin();
		host_callbacks();
		signed_zero_keys();
		typed_slice();
		missing_key_compound();
		constructorless_call_text();
		typed_reference_parameters();
		static_members_bind_references();
		reference_store_types();
		typed_range_for();
		auto_fields();
		numeric_ternary_auto();
		from_json_constructor_errors();
		json_tagged_values();
		base_slots_keep_the_class();
		block_callees();
		var_typed_arrays();
		constructor_overloads_rank_like_calls();
		compound_operators_have_one_table();
		script_class_operator_overloads();
		builtin_methods_as_values();
		reflect_bind();
		field_reference_stores();
		nan_comparisons();
	}

	// Comparison rewriting, <=> everywhere, host call failure routes, host-bound references and class scope
	void comparisons_host_routes_and_class_scope() {
		comparison_rewriting();
		host_call_failure_routes();
		host_global_references();
		spaceship_everywhere();
		spaceship_drives_mixed_operands();
		classes_in_function_bodies();
	}

	void spaceship_everywhere() {
		test("a_right_operand_spaceship_answers_reversed_comparisons", [this]() {
			// With no operator for the left operand, a right operand whose chain declares an applicable
			// <=> answers: 5 < v is 0 < (v <=> 5), 5 <=> v is 0 <=> (v <=> 5); an overload that cannot take the left operand
			// is no candidate, so the builtin text stands
			expect("reversed", R"(
				class V { int n = 0; V(int x) { n = x; } int operator<=>(V o) { return n - o.n; } int operator<=>(int k) { return n - k; } }
				class S { int n = 0; S(int x) { n = x; } int operator<=>(S o) { return n - o.n; } }
				class Plain { int n = 1; }
				function t(function fn) -> string { string r = ""; try { r = `${fn()}`; } catch (e) { r = e; } return r; }
				string rev = `${(5 < V(7))} ${(5 <= V(5))} ${(5 > V(7))} ${(9 >= V(7))} ${(5 <=> V(7))} ${(9 <=> V(7))} ${(7 <=> V(7))}`;
				string fwd = `${(V(7) > 5)} ${(V(7) <=> 5)}`;
				string loops = "";
				for (int i = 0; i < V(3); i += 1) { loops += to_string(i); }
				rev + "|" + fwd + "|" + loops + "|" + t([]() { return 5 < S(1); }) + "|" + t([]() { Plain p; return p < V(1); }) + "|" + t([]() { return 5 <=> S(1); });
			)", "true true false true -1 1 0|true 2|012|Invalid operands for < operator|Invalid operands for < operator|Invalid operands for <=> operator", warn_region);
		});
		test("equality_comes_from_spaceship_when_no_operator_equal_is_declared", [this]() {
			// == and != come from <=> ((a <=> b) == 0) for a class value whose chain declares <=> and no
			// operator==, the left operand's first, then the right one's; a declared operator== wins, x == null stays a null
			// test, a class without <=> compares memberwise, an overload that cannot take the other operand is no candidate
			expect("== from <=>", R"(
				class Z { int n = 0; Z(int x) { n = x; } int operator<=>(Z o) { return (n % 10) - (o.n % 10); } }
				class E { int n = 0; E(int x) { n = x; } bool operator==(E o) { return false; } int operator<=>(E o) { return 0; } }
				class Any { int k = 0; int operator<=>(var o) { return 0; } }
				class Strict { int k = 0; int operator<=>(Strict o) { return 0; } }
				class Plain { int n = 1; }
				class M { int n = 0; M(int x) { n = x; } }
				var nul = null;
				string z = `${(Z(13) == Z(3))} ${(Z(13) != Z(3))} ${(Z(13) == Z(4))} ${(Z(13) != Z(4))} ${(Z(1) == nul)} ${(Z(1) != nul)}`;
				string declared = `${(E(1) == E(1))} ${(E(1) != E(1))}`;
				Plain p; Any a; Strict s;
				string reversed = `${(p == a)} ${(p != a)} ${(a == p)} ${(p == s)} ${(s == p)}`;
				string plain = `${(M(1) == M(1))} ${(M(1) == M(2))}`;
				array<var> xs = [Z(13), Z(24)]; array<var> ys = [Z(3), Z(4)];
				string nested = `${(xs == ys)} ${(xs != ys)}`;
				z + "|" + declared + "|" + reversed + "|" + plain + "|" + nested;
			)", "true false false true false true|false true|true false true false false|true false|true false", warn_region);
		});
	}

	void spaceship_drives_mixed_operands() {
		test("spaceship_drives_mixed_operands", [this]() {
			// <=> drives everything it can: a <=> overload that takes the other operand
			// answers == != < <= > >= <=> against any operand, both orders; a declared operator that takes it wins; null tests,
			// classes without <=>, and overloads that cannot take the operand keep their meaning
			expect("mixed", R"(
				class Money { int cents = 0; Money(int c) { cents = c; } int operator<=>(Money o) { return cents - o.cents; } int operator<=>(int c) { return cents - c; } }
				class Tagged { int n = 0; Tagged(int x) { n = x; } bool operator==(int k) { return k == 42; } int operator<=>(int k) { return n - k; } }
				class OnlyEq { int n = 0; bool operator==(var o) { return true; } }
				class S { int n = 0; S(int x) { n = x; } int operator<=>(S o) { return n - o.n; } }
				function t(function fn) -> string { string r = ""; try { r = `${fn()}`; } catch (e) { r = e; } return r; }
				Money m = Money(500); var nul = null; OnlyEq oe;
				string money = `${(m == 500)} ${(500 == m)} ${(m != 500)} ${(500 != m)} ${(m == 501)} ${(m < 600)} ${(600 > m)} ${(m >= 500)} ${(499 < m)} ${(m <=> 500)} ${(400 <=> m)}`;
				string tagged = `${(Tagged(1) == 42)} ${(Tagged(1) == 1)} ${(42 == Tagged(1))} ${(Tagged(1) != 42)} ${(Tagged(3) < 4)}`;
				string plain = `${(oe == 5)} ${(5 == oe)}`;
				string strict = `${(S(1) == 1)} ${(1 == S(1))} ${t([]() { return S(1) < 1; })} ${t([]() { return 1 < S(1); })}`;
				string nulls = `${(m == nul)} ${(m != nul)}`;
				class N { int k = 0; float operator<=>(int x) { return math::NAN_VALUE; } }
				N nn;
				nulls += ` ${(5 <=> nn)} ${(5 < nn)} ${(nn == 5)}`;   // the reversed <=> orders like the builtin one: NaN last
				array<var> prices = [Money(300), 100, Money(200)];
				string found = `${(prices.contains(100))} ${(prices.index_of(200))}`;
				array<var> ms = [Money(300), Money(100), Money(200)]; ms.sort();
				array<var> mix = [Money(300), 100, Money(200)]; mix.sort();
				string sorted = `${ms[0].cents}${ms[1].cents}${ms[2].cents} ${(mix[0] == 100)} ${(mix[1] == 200)} ${(mix[2] == 300)}`;
				`${money}|${tagged}|${plain}|${strict}|${nulls}|${found}|${sorted}`;
			)", "true true false false false true true true true 0 -1|true false true false true|false false|false false Invalid operands for < operator Invalid operands for < operator|false true -1 false false|true 2|100200300 true true true", warn_region);
		});
	}

	void classes_in_function_bodies() {
		test("a_class_declared_in_a_function_body_is_a_parse_error", [this]() {
			// A class is a program-scope type, so a function or lambda body cannot declare one
			const std::string text = "a class cannot be declared inside a function body (a class is a program-scope type); declare it at program scope or import its file";
			for (const char* src : {"function f() -> int { class L { int q = 3; } return 1; }",
			                        "var g = []() { class L { int q = 3; } return 1; };",
			                        "class C { int m() { class L { int q = 3; } return 1; } }"}) {
				every_config(std::string("class in body: ") + src, "rejected", [&](engine& e) {
					try { e.execute(src); return std::string("loaded"); }
					catch (const std::exception& ex) { const std::string what = ex.what(); return what.find(text) != std::string::npos ? std::string("rejected") : what; }
				});
			}
			expect("program scope stands", "class L { int q = 3; } function f() -> int { L l; return l.q; } f();", "<3>");
		});
	}

	// "<kind>:<key>" when the text holds the key, else "<kind>:<text>"
	static std::string route(const std::string& kind, const std::string& text, const std::string& key) {
		return kind + ":" + (text.find(key) != std::string::npos ? key : text);
	}

	void host_call_failure_routes() {
		// A checked_result-returning call reports every failure in its result (a run-time error, an
		// uncaught script throw, a missing module, a failed reload; a function, a bound method, a static method, a coroutine
		// resume); a throwing call (execute, convert_script_function's std::function) throws for every failure; the engine
		// stays usable after each
		const std::filesystem::path dir = test_temp_directory() / "host_call_routes";
		std::filesystem::create_directories(dir);
		const std::string mod = (dir / "reloaded.jai").generic_string();
		const std::string prog = R"(
			function runtime_fail() -> int { array<int> a = [1]; return a[5]; }
			function throws_value() -> int { throw "boom"; return 0; }
			function missing_module() -> int { import "no_such_module_for_host_routes.jai"; return 1; }
			function reload_module() -> int { import ")" + mod + R"("; return reloaded_value(); }
			class C { int m() { throw "method boom"; return 0; } static int s() { throw "static boom"; return 0; } }
			coroutine co() { yield 1; throw "co boom"; }
			C c; var bm = reflect::bind(c, "m"); var sm = C::s; var h = co();
		)";
		const std::vector<std::pair<std::string, std::string>> calls = {
			{"runtime_fail", "out of bounds"}, {"throws_value", "boom"}, {"missing_module", "Failed to open import file: no_such_module_for_host_routes.jai"},
			{"reload_module", "reloaded"}, {"bm", "method boom"}, {"sm", "static boom"}};
		test("a_checked_host_call_reports_every_failure_in_its_result", [=, this]() {
			every_config("checked routes", "err:out of bounds|err:boom|err:Failed to open import file: no_such_module_for_host_routes.jai|ok:<1>|err:reloaded|"
			             "err:method boom|err:static boom|ok:<1>|err:co boom|<42>", [&](engine& e) {
				e.import_recheck_interval(0);
				std::ofstream(mod) << "function reloaded_value() -> int { return 1; }\n";
				e.execute(spec::in_warn_region(prog));
				auto call = [&](const std::string& name, const std::string& key) -> std::string {
					try {
						auto r = e.get_variable(name).as_function()({});
						if (r) { return "ok:" + spec::render(r.value()); }
						return route("err", e.format_error(r), key);
					} catch (const std::exception& ex) { return route("throw", ex.what(), key); }
				};
				std::string out;
				for (const auto& [name, key] : calls) {
					if (name == "reload_module") {
						out += call(name, key) + "|";
						std::ofstream(mod) << "function reloaded_value() -> int { return ; \n";
					}
					out += call(name, key) + "|";
				}
				auto handle = std::static_pointer_cast<coroutine_handle>(e.get_variable("h").get_object_holder()->host_object());
				for (int i = 0; i < 2; ++i) {
					try {
						auto r = handle->next(&e);
						out += r ? "ok:" + spec::render(r.value()) + "|" : route("err", e.format_error(r), "co boom") + "|";
					} catch (const std::exception& ex) { out += route("throw", ex.what(), "co boom") + "|"; }
				}
				return out + spec::render(e.execute("40 + 2"));
			});
		});
		test("a_throwing_host_call_throws_for_every_failure", [=, this]() {
			every_config("throwing routes", "throw:out of bounds|throw:boom|throw:Failed to open import file: no_such_module_for_host_routes.jai|ok|throw:reloaded|"
			             "throw:method boom|throw:static boom|exec throw:boom|exec throw:Failed to open import file: no_such_module_for_host_routes.jai|null:empty|<42>", [&](engine& e) {
				e.import_recheck_interval(0);
				std::ofstream(mod) << "function reloaded_value() -> int { return 1; }\n";
				e.execute(spec::in_warn_region(prog));
				std::string out;
				for (const auto& [name, key] : calls) {
					auto fn = convert_script_function<int64_t()>(e.get_variable(name));
					if (name == "reload_module") {
						try { fn(); out += "ok|"; } catch (const std::exception& ex) { out += route("throw", ex.what(), key) + "|"; }
						std::ofstream(mod) << "function reloaded_value() -> int { return ; \n";
					}
					try { fn(); out += "ok|"; } catch (const std::exception& ex) { out += route("throw", ex.what(), key) + "|"; }
				}
				for (const auto& [src, key] : std::vector<std::pair<std::string, std::string>>{{"throws_value();", "boom"}, {"missing_module();", "Failed to open import file: no_such_module_for_host_routes.jai"}}) {
					try { e.execute(src); out += "exec ok|"; } catch (const std::exception& ex) { out += route("exec throw", ex.what(), key) + "|"; }
				}
				auto none = convert_script_function<void()>(script_value(std::monostate{}, &e));
				out += none ? "null:callable|" : "null:empty|";
				return out + spec::render(e.execute("40 + 2"));
			});
		});
		// A script_value crosses a callback's parameters and result as itself, as make_value hands one back unchanged
		test("a_callback_passes_script_values_through", [this]() {
			auto eng = make_engine();
			eng->execute("function count_of(var v) -> int { return v.size(); } function echo(var v) { return v; }");
			script_value arr = eng->execute("var arr = [1, 2, 3]; arr");
			auto count_of = convert_script_function<int64_t(script_value)>(eng->get_variable("count_of"));
			check_eq((int64_t)3, count_of(arr), "a script_value argument");
			auto echo = convert_script_function<script_value(const script_value&)>(eng->get_variable("echo"));
			script_value text = eng->make_value(std::string("x"));
			check_eq(std::string("x"), echo(text).as_string(), "a script_value result");
			check_eq(std::string("x"), eng->make_value(text).as_string(), "make_value of a script_value is that value");
		});
		test("a_std_function_parameter_of_a_host_function_takes_a_script_callable", [this]() {
			every_config("std::function parameter", "12 12 12 ran 8", [&](engine& e) {
				e.add_function("apply", [](std::function<int64_t(int64_t)> f, int64_t v) { return f(v); });
				e.add_function("run0", [](std::function<void()> f) { f(); });
				e.add_function("twice", [](const std::function<int64_t(int64_t)>& f, int64_t v) { return f(f(v)); });
				return spec::render(e.execute(R"(
					string ran = "no";
					int triple(int x) { return x * 3; }
					run0([&]() { ran = "ran"; });
					`${apply(x => x * 3, 4)} ${apply([](int x) { return x * 3; }, 4)} ${apply(triple, 4)} ${ran} ${twice(x => x * 2, 2)}`;
				)"));
			});
		});
		test("an_enum_parameter_of_a_host_function_takes_an_int", [this]() {
			enum class mood { calm = 0, angry = 1 };
			every_config("enum parameter", "1 0 1", [&](engine& e) {
				e.add_function("mood_of", [](mood m) { return static_cast<int64_t>(m); });
				e.add_function("give_mood", []() { return mood::angry; });
				return spec::render(e.execute("`${mood_of(1)} ${mood_of(0)} ${give_mood()}`;"));
			});
		});
	}

	struct ref_player { int64_t hp = 0; std::string tag; };

	void host_global_references() {
		// A reference to an add_global_ref variable aliases the C++ storage: a store through it (=, op=,
		// ++, a script T& parameter that writes) converts through the bound type and writes C++, and the name stays bound
		test("a_reference_to_a_host_bound_int_stores_through_to_cpp", [this]() {
			int64_t hp = 0;
			every_config("int", "5 42,42 43 44 45 145 7 3 9 1 Cannot assign float to 'fresh' (declared int&) 1 11 same 11", [&](engine& e) {
				auto lax = [&e](const std::string& src) { return e.execute(spec::in_warn_region(src)); };
				hp = 10;
				e.add_global_ref("hp", hp);
				std::string out;
				lax("auto& r = hp; r = 5;"); out += std::to_string(hp);
				hp = 42; out += " " + spec::render(lax("`${hp},${r}`;"));
				lax("r += 1;"); out += " " + std::to_string(hp);
				lax("++r;"); out += " " + std::to_string(hp);
				lax("r++;"); out += " " + std::to_string(hp);
				lax("function bump(int& x) { x += 100; } bump(hp);"); out += " " + std::to_string(hp);
				lax("hp = 7;"); out += " " + std::to_string(hp);
				lax("function set3(int& x) { x = 3; } set3(r);"); out += " " + std::to_string(hp);
				hp = 9; out += " " + spec::render(lax("to_string(r);"));
				lax("int& typed = hp; typed = true;"); out += " " + std::to_string(hp);
				out += " " + spec::render(lax("string refused = \"\"; var f = 2.9; int& fresh = hp; try { fresh = f; } catch (err) { refused = err; } refused;")); out += " " + std::to_string(hp);
				lax("function local_ref() { auto& lr = hp; lr = 11; } local_ref();"); out += " " + std::to_string(hp);
				out += " " + spec::render(lax(R"(
					string a = ""; try { hp = "x"; } catch (err) { a = err; }
					string b = ""; try { r = "x"; } catch (err) { b = err; }
					a == b ? "same" : a + " vs " + b;
				)"));
				return out + " " + std::to_string(hp);
			});
		});
		test("a_reference_to_a_host_bound_float_stores_through_to_cpp", [this]() {
			double speed = 0;
			float scale = 0;
			every_config("float", "5.000000 1.25 0.625 0.750000 2.000000", [&](engine& e) {
				auto lax = [&e](const std::string& src) { return e.execute(spec::in_warn_region(src)); };
				speed = 0; scale = 0;
				e.add_global_ref("speed", speed);
				e.add_global_ref("scale", scale);
				std::string out;
				lax("auto& s = speed; s = 2.5; s *= 2;"); out += std::to_string(speed);
				speed = 1.25; out += " " + spec::render(lax("to_string(s);"));
				lax("function half(float& x) { x /= 2; } half(speed);"); out += " " + spec::render(lax("to_string(speed);"));
				lax("auto& k = scale; k = 0.5; k += 0.25;"); out += " " + std::to_string(scale);
				lax("half(k); k = k * 4 + 0.5;"); out += " " + std::to_string(scale);
				return out;
			});
		});
		test("a_reference_to_a_host_bound_string_stores_through_to_cpp", [this]() {
			std::string name;
			every_config("string", "abc zz zz! zz!! zz!!? set", [&](engine& e) {
				auto lax = [&e](const std::string& src) { return e.execute(spec::in_warn_region(src)); };
				name = "";
				e.add_global_ref("name", name);
				std::string out;
				lax("auto& n = name; n = \"ab\"; n += \"c\";"); out += name;
				name = "zz"; out += " " + spec::render(lax("n;"));
				lax("function shout(string& t) { t += \"!\"; } shout(name);"); out += " " + name;
				lax("shout(n);"); out += " " + name;
				lax("string& sn = name; sn += \"?\";"); out += " " + name;
				name = "set"; out += " " + spec::render(lax("name;"));
				return out;
			});
		});
		test("a_reference_to_a_bound_class_field_stores_through_to_cpp", [this]() {
			ref_player player;
			every_config("class field", "6 106 8 xy 9", [&](engine& e) {
				auto lax = [&e](const std::string& src) { return e.execute(spec::in_warn_region(src)); };
				player = ref_player{};
				dynamic_binder<ref_player>(e, "RefPlayer").property("hp", &ref_player::hp).property("tag", &ref_player::tag).build();
				e.add_global_ref("player", player);
				std::string out;
				lax("auto& h = player.hp; h = 5; h += 1;"); out += std::to_string(player.hp);
				lax("function bump(int& x) { x += 100; } bump(player.hp);"); out += " " + std::to_string(player.hp);
				lax("auto& pl = player; pl.hp = 8;"); out += " " + std::to_string(player.hp);
				lax("auto& t = player.tag; t = \"x\"; t += \"y\";"); out += " " + player.tag;
				player.hp = 9; out += " " + spec::render(lax("to_string(h);"));
				return out;
			});
		});
	}

	void comparison_rewriting() {
		test("a_class_with_only_spaceship_gets_the_four_relational_operators", [this]() {
			// C++20 rewriting: a < b is (a <=> b) < 0, a <= b is (a <=> b) <= 0, a > b is (a <=> b) > 0, a >= b is (a <=> b) >= 0;
			// the class chain answers it (a subclass inherits), a float result compares with zero too
			expect("rewritten relational", R"(
				class V { int n = 0; V(int x) { n = x; } int operator<=>(V o) { return n < o.n ? -1 : n > o.n ? 1 : 0; } }
				class D : V { D(int x) : super(x) {} }
				class G { float n = 0.0; G(float x) { n = x; } float operator<=>(G o) { return n - o.n; } }
				V a = V(1); V b = V(2);
				string rel = `${(a < b)} ${(a <= b)} ${(a > b)} ${(a >= b)} ${(b > a)} ${(a <= V(1))} ${(a >= V(1))} ${(a < V(1))}`;
				string sub = `${(D(3) < D(4))} ${(D(3) >= D(4))}`;
				string fl = `${(G(0.5) < G(0.75))} ${(G(0.75) > G(0.5))} ${(G(0.5) >= G(0.5))}`;
				string branch = "";
				if (a < b) { branch += "lt"; }
				if (!(a > b)) { branch += "!gt"; }
				int k = 0;
				for (V i = V(0); i < V(3); i = V(i.n + 1)) { k += 1; }
				`${rel}|${sub}|${fl}|${branch} ${k}`;
			)", "true true false false true true true false|true false|true true true|lt!gt 3");
		});
		test("a_declared_relational_operator_wins_over_the_rewrite", [this]() {
			// C++20: a non-rewritten candidate beats the rewritten one; only the missing operators come from <=>
			expect("declared wins", R"(
				class W { int n = 0; W(int x) { n = x; } int operator<=>(W o) { return n - o.n; } bool operator<(W o) { return true; } }
				class B { int n = 0; B(int x) { n = x; } bool operator>=(B o) { return false; } }
				class C : B { C(int x) : super(x) {} int operator<=>(C o) { return n - o.n; } }
				`${(W(5) < W(1))} ${(W(5) > W(1))} ${(W(1) > W(5))} ${(W(1) <= W(5))}|${(C(5) >= C(1))} ${(C(5) > C(1))}`;
			)", "true true false true|false true");
		});
		test("spaceship_equality_replaces_the_memberwise_default", [this]() {
			// A class with <=> and no operator== compares by (a <=> b) == 0,
			// never fieldwise
			expect("== from <=>", R"(
				class Z { int n = 0; Z(int x) { n = x; } int operator<=>(Z o) { return 0; } }
				`${(Z(1) == Z(2))} ${(Z(1) != Z(2))} ${(Z(1) <= Z(2))} ${(Z(1) >= Z(2))} ${(Z(3) == Z(3))} ${(Z(1) <=> Z(2))}`;
			)", "true false true true true 0");
		});
		test("inequality_rewrites_from_equality_and_a_declared_inequality_wins", [this]() {
			// C++20: a != b is !(a == b) through operator==; a declared operator!= is asked first, where operator== would be
			// (a class instance against a non-null value of its kind), so x != null stays the null test
			expect("!= from ==", R"(
				class E { int n = 0; E(int x) { n = x; } bool operator==(E o) { return true; } }
				class F { int n = 0; F(int x) { n = x; } bool operator==(F o) { return true; } bool operator!=(F o) { return true; } }
				class H : F { H(int x) : super(x) {} }
				F f = F(1); var nul = null;
				`${(E(1) != E(2))} ${(E(1) == E(2))} ${(F(1) != F(1))} ${(F(1) == F(1))} ${(H(1) != H(2))} ${(f != nul)} ${(f != 5)}`;
			)", "false true true true true true true", warn_region);
		});
		test("a_spaceship_rewrite_raises_what_the_operator_raises", [this]() {
			// the operator's own error or throw is the comparison's; a result that is no number leaves the invalid-operands text;
			// a builtin left operand keeps the builtin text (the left operand's class decides, as for every operator)
			expect("rewrite errors", R"(
				class T { int n = 0; T(int x) { n = x; } int operator<=>(T o) { if (o.n == 0) { throw "zero"; } return n - o.n; } }
				class S { int n = 0; S(int x) { n = x; } string operator<=>(S o) { return "x"; } }
				function t(function fn) -> string { string r = ""; try { r = `${fn()}`; } catch (e) { r = e; } return r; }
				t([]() { return T(1) < T(0); }) + "|" + t([]() { return T(1) < T(2); }) + "|" + t([]() { return S(1) < S(2); }) + "|" + t([]() { return 1 < T(2); });
			)", "zero|true|Invalid operands for < operator|Invalid operands for < operator", warn_region);
		});
	}

	void nan_comparisons() {
		test("nan_relations_are_ieee_and_spaceship_has_a_total_order", [this]() {
			// == != < <= > >= follow IEEE (a NaN operand compares false, and unequal) on every lane
			// (typed, dynamic, mixed int/float, a negated condition, a typed loop head, container equality); <=> with a NaN operand
			// groups NaNs after numbers; a map keeps its total key order (a NaN key is found again)
			expect("nan", R"(
				function t(function fn) -> string { string r = ""; try { r = `${fn()}`; } catch (e) { r = e; } return r; }
				float n = math::NAN_VALUE; float one = 1.0; var vn = math::NAN_VALUE; int i = 1;
				string typed = `${(n == n)}${(n != n)}${(n < one)}${(n <= one)}${(n > one)}${(n >= one)}${(one < n)}${(n == one)}`;
				string dyn = `${(vn == vn)}${(vn != vn)}${(vn < 1)}${(vn >= 1)}${(1 > vn)}`;
				string mixed = `${(n == i)}${(i < n)}${(n != i)}${(i >= n)}`;
				string br = "";
				if (n < one) { br += "a"; }
				if (!(n < one)) { br += "b"; }
				if (n >= one) { br += "c"; }
				if (!(n >= one)) { br += "d"; }
				if (!(n == n)) { br += "e"; }
				while (n > one) { br += "w"; break; }
				for (float x = 0.0; x < n; x += 1.0) { br += "L"; }
				int k = 0;
				for (float x = 0.0; !(x >= n) && k < 3; x += 1.0) { k += 1; }
				br += to_string(k);
				string sp = t([]() { float z = math::NAN_VALUE; return z <=> 1.0; }) + "/" + t([]() { var z = math::NAN_VALUE; return 1 <=> z; });
				array<float> fa = [math::NAN_VALUE, 1.0]; array<float> fb = fa; var va = [math::NAN_VALUE];
				string arr = `${(fa == fb)}${(fa != fb)}${(va == va)}`;
				map<float, int> m = {}; m[math::NAN_VALUE] = 7; m[math::NAN_VALUE] = 8;
				string mk = `${m[math::NAN_VALUE]} ${m.size()}`;
				typed + "|" + dyn + "|" + mixed + "|" + br + "|" + sp + "|" + arr + "|" + mk;
			)", "falsetruefalsefalsefalsefalsefalsefalse|falsetruefalsefalsefalse|falsefalsetruefalse|bde3|"
			    "1/-1|falsetruefalse|8 1", warn_region);
		});
	}

	void field_reference_stores() {
		test("a_store_through_a_field_reference_runs_the_fields_table", [this]() {
			// auto& r = o.f; r = v stores exactly what o.f = v stores, plain and compound, and
			// refuses with the direct store's text
			expect("field reference", R"(
				class O { int f = 0; float g = 0.0; bool b = false; string s = ""; }
				function t(function fn) -> string { string r = ""; try { r = `${fn()}`; } catch (e) { r = e; } return r; }
				O a; O d;
				auto& rf = a.f; rf = true; d.f = true;
				auto& rg = a.g; rg = 3; d.g = 3;
				auto& rb = a.b; rb = 5; d.b = 5;
				auto& rs = a.s; rs = "x"; d.s = "x";
				string plain = (`${d.f} ${d.g} ${d.b} ${d.s}`) == (`${a.f} ${a.g} ${a.b} ${a.s}`) ? "match" : "differ";
				rf += 2; d.f += 2;
				rb = 0; d.b = 0;
				string compound = (`${d.f} ${d.b}`) == (`${a.f} ${a.b}`) ? `match ${a.f}` : `differ ${a.f} vs ${d.f}`;
				string e1 = t([]() { O o; o.f = "x"; return 1; });
				string e2 = t([]() { O o; auto& r = o.f; r = "x"; return 1; });
				string e3 = t([]() { O o; var f = 2.7; o.f = f; return 1; });
				string e4 = t([]() { O o; var f = 2.7; auto& r = o.f; r = f; return 1; });
				string e5 = t([]() { O o; auto& r = o.f; r += 2.7; return 1; });
				`${plain}|${compound}|${(e1 == e2 ? "same text" : e1 + " vs " + e2)}|${(e3 == e4 && e4 == e5 ? e3 : e3 + " vs " + e4 + " vs " + e5)}`;
			)", "match|match 3|same text|Cannot assign float to field 'f' of O (declared int)", warn_region);
		});
	}

	void reflect_bind() {
		test("reflect_bind_binds_a_method_to_its_object", [this]() {
			// reflect::bind(obj, "name") is the spelling of a method taken as a value: the callable holds the
			// object (a value instance, a handle, a host object) and keeps it alive; a value type's methods run on the value (a
			// lambda wraps them); reflection ignores the class's access control; c2 = C() copy-assigns into the object the callable holds
			expect("reflect::bind", R"(
				class C { int n = 0; void bump() { n += 1; } int add(int k) { n += k; return n; } private: void hidden() {} }
				function t(function f) -> string { string r = ""; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				C c; var b = reflect::bind(c, "bump"); b(); b();
				var add = reflect::bind(c, "add");
				shared_ptr<C> h = new C(); var hb = reflect::bind(h, "bump"); hb();
				C c2; var b2 = reflect::bind(c2, "bump"); c2 = C(); b2();
				var kept = reflect::bind(C(), "add");
				`${c.n} ${add(5)} ${h.n} ${c2.n} ${kept(3)}|${t([]() { var f = reflect::bind([1], "push"); return 1; })}|${t([]() { string s = "ab"; var f = reflect::bind(s, "to_upper"); return 1; })}|${t([]() { C k; var f = reflect::bind(k, "nope"); return 1; })}|${t([]() { C k; var f = reflect::bind(k, "hidden"); return 1; })}`;
			)", "2 7 1 1 3|reflect::bind takes a class instance, a handle or a host object; the methods of array run on the value: wrap the call in a lambda ([&]() { ... })|"
			    "reflect::bind takes a class instance, a handle or a host object; the methods of string run on the value: wrap the call in a lambda ([&]() { ... })|"
			    "reflect::bind: 'C' has no method 'nope'|1", warn_region);
		});
	}

	void builtin_methods_as_values() {
		test("a_bare_method_name_read_without_a_call_is_an_error", [this]() {
			// A bare method name read without a call inside a method is the same violation, the receiver
			// spelled this; the lookup order stands (locals -> members -> globals): only a resolution that lands on an instance
			// method errors, so a local or parameter of that name wins and a static method stays a function value
			expect("bare method values", R"(
				class Player {
					int hp = 5;
					void heal() { hp += 1; }
					function get() { return heal; }
					function call_it() { heal(); return hp; }
					function local_wins() { var heal = 7; return heal; }
					function param_wins(int heal) { return heal; }
					function in_lambda() { var f = [this]() { heal(); return hp; }; return f(); }
					static int twice(int x) { return x * 2; }
					function static_value() { var f = twice; return f(4); }
					function ref_decl() { auto& r = heal; return 1; }
					function var_ref_decl() { var& r = heal; return 1; }
					function cast_ref() { return cast<int&>(heal); }
					function ref_return() -> int& { return heal; }
				}
				function t(function f) -> string { string r = ""; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				Player q;
				`${q.call_it()} ${q.local_wins()} ${q.param_wins(3)} ${q.in_lambda()} ${q.static_value()}|${t([]() { Player p; var f = p.get(); return 1; })}|${t([]() { Player p; return p.ref_decl(); })}|${t([]() { Player p; return p.var_ref_decl(); })}|${t([]() { Player p; return p.cast_ref(); })}|${t([]() { Player p; return p.ref_return(); })}`;
			)", "6 7 3 7 8|'heal' is a method of Player; call it with heal() or bind it with reflect::bind(this, \"heal\")|"
			    "'heal' is a method of Player; call it with heal() or bind it with reflect::bind(this, \"heal\")|"
			    "'heal' is a method of Player; call it with heal() or bind it with reflect::bind(this, \"heal\")|"
			    "'heal' is a method of Player; call it with heal() or bind it with reflect::bind(this, \"heal\")|"
			    "'heal' is a method of Player; call it with heal() or bind it with reflect::bind(this, \"heal\")", warn_region);
			every_config("bare method value at load", "rejected", [&](engine& e) {
				try {
					e.execute("class Player { void heal() {} function get() { return heal; } }");
					return std::string("loaded");
				} catch (const std::exception& ex) {
					const std::string what = ex.what();
					return what.find("'heal' is a method of Player; call it with heal() or bind it with reflect::bind(this, \"heal\")") != std::string::npos
					    ? std::string("rejected") : what;
				}
			});
			every_config("reference to a bare method at load", "rejected", [&](engine& e) {
				try {
					e.execute("class Player { void heal() {} function get() { auto& r = heal; return 1; } }");
					return std::string("loaded");
				} catch (const std::exception& ex) {
					const std::string what = ex.what();
					return what.find("'heal' is a method of Player; call it with heal() or bind it with reflect::bind(this, \"heal\")") != std::string::npos
					    ? std::string("rejected") : what;
				}
			});
		});
		test("a_method_read_without_a_call_is_an_error", [this]() {
			// obj.method without a call reads too easily as a call, so it is an error for user, builtin and
			// host methods: a class instance, handle or host object binds with reflect::bind, a value type's methods take a lambda;
			// a field holding a callable stays a value
			expect("method values", R"(
				class Player { int hp = 5; void heal() { hp += 1; } }
				class Team { Player lead; }
				class Q { void a() {} function get() { return this.a; } }
				class H { var cb = null; }
				coroutine gen() { yield 1; }
				function t(function f) -> string { string r = ""; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				Player player; player.heal();
				H hh; hh.cb = []() { return 5; }; var g = hh.cb;
				var co = gen();
				`${player.hp} ${g()} ${co.next()}|${t([]() { Player p; var f = p.heal; return 1; })}|${t([]() { shared_ptr<Player> h = new Player(); var f = h.heal; return 1; })}|${t([]() { Team team; var f = team.lead.heal; return 1; })}|${t([]() { Q q; return q.get(); })}|${t([]() { var a = [1]; var f = a.push; return 1; })}|${t([]() { string s = "x"; var f = s.to_upper; return 1; })}|${t([]() { map<string, int> m = {}; var f = m.clear; return 1; })}|${t([]() { var c = gen(); var f = c.next; return 1; })}`;
			)", "6 5 1|'heal' is a method of Player; call it with p.heal() or bind it with reflect::bind(p, \"heal\")|"
			    "'heal' is a method of Player; call it with h.heal() or bind it with reflect::bind(h, \"heal\")|"
			    "'heal' is a method of Player; call it with team.lead.heal() or bind it with reflect::bind(team.lead, \"heal\")|"
			    "'a' is a method of Q; call it with this.a() or bind it with reflect::bind(this, \"a\")|"
			    "'push' is a method of array; call it with a.push() or wrap the call in a lambda|"
			    "'to_upper' is a method of string; call it with s.to_upper() or wrap the call in a lambda|"
			    "'clear' is a method of map; call it with m.clear() or wrap the call in a lambda|"
			    "'next' is a method of coroutine; call it with c.next() or wrap the call in a lambda", warn_region);
			every_config("method value at load", "rejected", [&](engine& e) {
				try {
					e.execute("class Player { void heal() {} } Player player; var f = player.heal;");
					return std::string("loaded");
				} catch (const std::exception& ex) {
					const std::string what = ex.what();
					return what.find("'heal' is a method of Player; call it with player.heal() or bind it with reflect::bind(player, \"heal\")") != std::string::npos
					    ? std::string("rejected") : what;
				}
			});
		});
	}

	void script_class_operator_overloads() {
		test("a_script_class_overloads_every_cpp_overloadable_operator", [this]() {
			// Unary - + ~ and binary % & | ^ << >> <=> join the declarable operators (unary vs binary by
			// the parameter count, like C++ member operators); a compound form is x = x op y through the class's operator; the
			// class chain resolves them; a builtin operand keeps its builtin meaning
			const std::string src = R"(
				class V {
					int n = 0;
					V(int x) { n = x; }
					V operator%(V o) { return V(n % o.n); }
					V operator&(V o) { return V(n & o.n); }
					V operator|(V o) { return V(n | o.n); }
					V operator^(int k) { return V(n ^ k); }
					V operator<<(int k) { return V(n << k); }
					V operator>>(int k) { return V(n >> k); }
					int operator<=>(V o) { return n < o.n ? -1 : n > o.n ? 1 : 0; }
					V operator-() { return V(0 - n); }
					V operator+() { return V(n + 100); }
					V operator~() { return V(~n); }
					V operator-(V o) { return V(n - o.n); }
				}
				class W : V { W(int x) : super(x) {} }
				class P { int n = 0; P(int x) { n = x; } int operator%(Probe p) { return n % p.v; } }
				V a = V(12); V b = V(5);
				string direct = `${(a % b).n} ${(a & b).n} ${(a | b).n} ${(a ^ 1).n} ${(a << 2).n} ${(a >> 2).n} ${(a <=> b)} ${(-a).n} ${(+a).n} ${(~a).n} ${(a - b).n}`;
				V c = V(12);
				c %= b; string comp = `${c.n}`; c &= V(3); comp += ` ${c.n}`; c |= V(4); comp += ` ${c.n}`; c ^= 3; comp += ` ${c.n}`;
				c <<= 1; comp += ` ${c.n}`; c >>= 2; comp += ` ${c.n}`;
				W w = W(12);
				string inherited = `${(w % b).n} ${(-w).n} ${(~w).n}`;
				Probe pr = Probe(); pr.v = 5;
				string mixed = `${(P(12) % pr)}`;
				string builtin = `${(7 % 3)} ${(6 & 3)} ${(-4)} ${(+4)} ${(+2.5)} ${!0} ${(~0)}`;
				`${direct}|${comp}|${inherited}|${mixed}|${builtin}`;
			)";
			every_config("script operators", "2 4 13 13 48 3 1 -12 112 -13 7|2 2 6 5 10 2|2 -12 -13|2|1 2 -4 4 2.5 true -1",
				[&](engine& e) { spec::bind_boundary_probe(e); return spec::render(e.execute(src)); });
		});
		test("an_operator_the_class_does_not_declare_keeps_the_builtin_text", [this]() {
			// a unary operator the class lacks (only the binary form declared) raises the builtin operand text; && || stay short-circuit
			expect("missing operators", R"(
				class V { int n = 1; V operator-(V o) { return V(); } }
				function t(function f) -> string { string r = ""; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				t([]() { V v; var w = -v; return 1; }) + "|" + t([]() { V v; var w = ~v; return 1; }) + "|" + t([]() { V v; var w = v % 2; return 1; });
			)", "Unary minus requires numeric operand|Bitwise NOT requires integer operand|Invalid operands for % operator", warn_region);
		});
		test("a_class_cannot_declare_operator_bang_and_not_reads_to_bool", [this]() {
			// Like C++ with an operator bool, !v and if (v) go through the one contextual conversion:
			// !v is !to_bool(v), if (v) is to_bool(v); declaring operator! on a script class is a parse error that says so
			expect("to_bool contexts", R"(
				class Z { int n = 0; Z(int x) { n = x; } bool to_bool() { return n != 0; } }
				Z z0 = Z(0); Z z1 = Z(1);
				string r = "";
				if (!z0) { r += "a"; }
				if (!z1) { r += "b"; }
				if (z1) { r += "c"; }
				if (z0) { r += "d"; }
				r += !z0 ? "e" : "f";
				bool b0 = z0; bool b1 = z1;
				int k = 0;
				while (!Z(k)) { k += 1; }
				`${r} ${!z0} ${!z1} ${b0} ${b1} ${k} ${(z1 && true)} ${(z0 || false)}`;
			)", "ace true false false true 1 true false");
			every_config("operator! declaration", "rejected", [&](engine& e) {
				try {
					e.execute("class Z { int n = 0; bool operator!() { return n == 0; } }");
					return std::string("loaded");
				} catch (const std::exception& ex) {
					const std::string what = ex.what();
					return what.find("operator! cannot be declared: !v is !to_bool(v); declare bool to_bool()") != std::string::npos
					    ? std::string("rejected") : what;
				}
			});
		});
		test("a_class_with_operator_call_is_callable_wherever_a_function_is_called", [this]() {
			// A call whose callee holds a class instance runs its operator() (any arity, the usual
			// overload scorer, the class chain), through a local, a global, a field, an element, a computed callee and a handle
			const std::string src = R"(
				class Adder {
					int base = 0;
					int calls = 0;
					Adder(int b) { base = b; }
					int operator()(int x) { calls += 1; return base + x; }
					int operator()(int x, int y) { calls += 1; return base + x + y; }
					string operator()(string s) { calls += 1; return s + "!"; }
				}
				class Twice : Adder { Twice() : super(1) {} }
				class Holder { Adder f = Adder(100); }
				Adder g = Adder(10);
				function make() -> Adder { return Adder(1000); }
				function run() -> string {
					Adder a = Adder(5);
					Holder h;
					array<Adder> arr = [Adder(20)];
					shared_ptr<Adder> sp = new Adder(7);
					Twice t;
					string r = `${a(1)} ${a(1, 2)} ${a("hi")} ${a.calls} ${g(1)} ${h.f(3)} ${arr[0](2)} ${(make())(4)} ${sp(1)} ${sp.calls} ${t(1)}`;
					return r;
				}
				run();
			)";
			every_config("callable objects", "6 8 hi! 3 11 103 22 1004 8 1 2",
				[&](engine& e) { return spec::render(e.execute(src)); });
		});
		test("a_class_without_operator_call_keeps_the_not_callable_text", [this]() {
			// a function slot takes a function or null (the one conversion table): a callable object passes through a lambda
			expect("not callable", R"(
				class Plain { int n = 1; }
				class F { int operator()(int x) { return x * 2; } }
				function t(function f) -> string { string r = ""; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				function apply(function f, int x) -> int { return f(x); }
				t([]() { Plain p; return p(1); }) + "|" + t([]() { F f; return apply([f](int x) { return f(x); }, 3); });
			)", "Cannot call 'p': it is a Plain, not a function|6");
			// the table's function row takes a function or null: the strict checker refuses an instance at a function declaration
			every_config("function slot", "rejected", [&](engine& e) {
				try {
					e.execute(R"(
						class F { int operator()(int x) { return x * 2; } }
						F f;
						function g = f;
					)");
					return std::string("loaded");
				} catch (const std::exception& ex) {
					const std::string what = ex.what();
					return what.find("cannot initialize 'g' declared 'function' with 'F'") != std::string::npos ? std::string("rejected") : what;
				}
			});
		});
	}

	void compound_operators_have_one_table() {
		test("a_compound_operator_has_one_table_at_every_destination", [this]() {
			// the simplification audit's F9-F12 (one semantics per compound operator, one zero-divisor text per operator family):
			// member '/=' divides like the local's, '+=' appends text only to a text target at every destination (a local, a field,
			// this's field, an element, a map entry, a reference), an integer '/' and '/=' raise 'Division by zero', '%' and '%=' 'Modulo by zero'
			expect("one compound table", R"(
				class T { string s = "a"; var n = 7; int big = 9007199254740993; float f = 1.0; void half() { n /= 2; } void addc() { s += 'c'; } }
				function t(function f) -> string { string r = ""; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				t([]() { T o; o.half(); return `${type_of(o.n)}:${o.n}`; }) + "|" +
				t([]() { T o; o.n /= 2; return o.n; }) + "|" +
				t([]() { T o; o.big /= 1; return o.big; }) + "|" +
				t([]() { T o; o.s += 'c'; o.addc(); return o.s; }) + "|" +
				t([]() { string gs = "a"; gs += 'c'; return gs; }) + "|" +
				t([]() { map<string, string> m = {"k": "v"}; m["k"] += 'z'; return m["k"]; }) + "|" +
				t([]() { array<string> as = ["x"]; as[0] += 5; return as[0]; }) + "|" +
				t([]() { var vs = "x"; vs += 5; return vs; }) + "|" +
				t([]() { T o; o.s += 5; return o.s; }) + "|" +
				t([]() { var a = [5]; a[0] += "s"; return a[0]; }) + "|" +
				t([]() { array<string> ss = ["a"]; auto& e1 = ss[0]; e1 += 5; return e1; }) + "|" +
				t([]() { int x = 5; x %= 0; return x; }) + "|" +
				t([]() { int x = 5; int z = 0; return x % z; }) + "|" +
				t([]() { int x = 5; x /= 0; return x; }) + "|" +
				t([]() { int x = 5; int z = 0; return x / z; }) + "|" +
				t([]() { array<int> a = [4]; a[0] %= 0; return 1; }) + "|" +
				t([]() { array<int> ai = [3]; auto& e0 = ai[0]; e0 %= 0; return 1; });
			)", "int:3|3|9007199254740993|acc|ac|vz|"
			    "Cannot apply '+=' to string and int|Cannot apply '+=' to string and int|Cannot apply '+=' to string and int|"
			    "Cannot apply '+=' to int and string|Cannot apply '+=' to string and int|"
			    "Modulo by zero|Modulo by zero|Division by zero|Division by zero|"
			    "Modulo by zero|Modulo by zero", warn_region);
		});
		test("float_division_and_modulo_by_zero_are_ieee", [this]() {
			// docs/site/guide/04-operators.html "Arithmetic": a float '/' or '%' (an int operand promoted beside a float included) gives
			// an infinity or NaN at every destination and on every path (literals, typed locals, var, loops, elements, members, a
			// reference, a vec); two ints still raise, and a float result never enters an int slot
			expect("ieee float division", R"(
				class T { float f = 1.0; var n = 2.5; }
				function t(function f) -> string { string r = ""; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				function loops() -> string {
					array<float> w = [1.0, -1.0, 0.0]; float z = 0.0; float acc = 0.0; string s = "";
					for (int i = 0; i < 3; i++) { float q = w[i] / z; float m = w[i] % z; s += `${q},${m};`; w[i] /= z; acc = acc + 1.0 / z; }
					var d = 0.0; var v = 3.0;
					for (int i = 0; i < 2; i++) { v = v / d; }
					return `${s}${w[0]},${w[1]},${w[2]};${acc};${v}`;
				}
				t([]() { return 1.0 / 0.0; }) + "|" + t([]() { return -1.0 / 0.0; }) + "|" + t([]() { return 0.0 / 0.0; }) + "|" +
				t([]() { return 1.5 % 0.0; }) + "|" + t([]() { return 1 / 0.0; }) + "|" + t([]() { return 1.0 / 0; }) + "|" + t([]() { return 1.0 % 0; }) + "|" +
				t([]() { float x = 5.0; x %= 0.0; return x; }) + "|" +
				t([]() { float x = 5.0; float z = 0.0; return x / z; }) + "|" +
				t([]() { float x = 5.0; x /= 0; return x; }) + "|" +
				t([]() { var x = 5; x /= 0.0; return `${type_of(x)}:${x}`; }) + "|" +
				t([]() { var x = 5.0; int z = 0; return x % z; }) + "|" +
				t([]() { var z = 0.0; var h = 0.5; return `${0.5 / z},${0.5 % z},${h % 0.0}`; }) + "|" +
				t([]() { T o; o.f /= 0.0; o.n %= 0; return `${o.f},${o.n}`; }) + "|" +
				t([]() { map<string, float> m = {"k": 2.0}; m["k"] /= 0; return m["k"]; }) + "|" +
				t([]() { array<float> a = [3.0]; auto& e0 = a[0]; e0 /= 0.0; return a[0]; }) + "|" +
				t([]() { var a = [3.0, 4]; a[0] /= 0; a[1] /= 0; return a; }) + "|" +
				t([]() { return math::vec2(1, -1) / 0; }) + "|" + t([]() { math::vec2 v = math::vec2(4, 0); v /= math::vec2(0, 0); return v.x; }) + "|" +
				t([]() { return math::is_inf(1.0 / 0.0) && math::is_nan(0.0 % 0.0); }) + "|" +
				t([]() { int x = 5; x /= 0.0; return x; }) + "|" + t([]() { int z = 0; return 1 / z; }) + "|" + t([]() { int z = 0; return 1 % z; }) + "|" + loops();
			)", "inf|-inf|nan|nan|inf|inf|nan|nan|inf|inf|float:inf|nan|inf,nan,nan|inf,nan|inf|inf|Division by zero|(inf, -inf)|inf|true|"
			    "Cannot assign float to 'x' (declared int)|Division by zero|Modulo by zero|inf,nan;-inf,nan;nan,nan;inf,-inf,nan;inf;inf", warn_region);
		});
	}

	void constructor_overloads_rank_like_calls() {
		test("a_constructor_overload_set_ranks_like_a_call", [this]() {
			// the one script-overload scorer picks a constructor (a call, : this(...) and : super(...)): typed exact > conversion > var;
			// a typed parameter the argument does not match ranks below every candidate it matches
			expect("constructor ranking", R"(
				class R { int got = 0; R(var& a, string tag) { got = -1; } R(var a, int tag) { got = a[0]; } }
				class F { string which = ""; F(var x) { which = "var"; } F(float x) { which = "float"; } }
				class D { string which = ""; D() : this(1) {} D(var x) { which = "var"; } D(int x) { which = "int"; } }
				class B { string which = ""; B(var x) { which = "var"; } B(int x) { which = "int"; } }
				class C : B { C() : super(5) {} }
				var a = [7];
				R r = R(a, 0); F f = F(2); D d = D(); C c = C(); B v = B("s");
				`${r.got} ${f.which} ${d.which} ${c.which} ${v.which}`;
			)", "7 float int int var", warn_region);
		});
	}

	void var_typed_arrays() {
		test("a_var_holding_a_copy_of_a_typed_array_keeps_the_typed_representation", [this]() {
			// the array keeps its element type (every element boundary), the variable stays a var ('=' takes any value)
			expect("var typed array", R"(
				array<int> a = [1];
				var G = a;
				function t(function f) -> string { string r = ""; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				t([]() { var v = a; v.push("s"); return v.size(); }) + "|" +
				t([]() { var v = a; v.push(2); v[0] = true; return `${v.size()} ${a.size()} ${type_of(v[0])}:${v[0]}`; }) + "|" +
				t([]() { var v = a; var f = 2.9; v[0] = f; return v[0]; }) + "|" +
				t([]() { var v = a; v = "hello"; v = 5; return `${type_of(v)}:${v}`; }) + "|" +
				t([]() { var v = 1; v = a; v.push("s"); return 1; }) + "|" +
				t([]() { G.push("s"); return 1; }) + "|" +
				t([]() { G = "g"; G = a; array<int> c = G; c.push(3); return `${c.size()} ${G.size()}`; }) + "|" +
				t([]() { var e = []; e.push("x"); e.push(1); return e.size(); }) + "|" +
				t([]() { var v = a; var& r = v; r = "via ref"; return r; });
			)", "Cannot push 'string' to array<int>|2 1 int:1|Cannot assign 'float' to element of type 'int'|int:5|Cannot push 'string' to array<int>|Cannot push 'string' to array<int>|2 1|2|via ref", warn_region);
		});
	}

	void block_callees() {
		test("a_callee_of_a_top_level_block_sees_the_outer_binding", [this]() {
			// a function called from inside a block sees the binding of the scope it was declared in
			// (a {} block, an if body, a for body, a range-for body, a catch body, a nested block); a function declared inside
			// the block keeps the block's names
			expect("top block", R"(
				var G = 1;
				function g() { return G; }
				var rb = 0; var ri = 0; var rf = 0; var rr = 0; var rc = 0; var rn = 0; var rd = 0;
				{ var G = 5; rb = g(); }
				if (true) { var G = 9; ri = g(); }
				for (int i = 0; i < 1; i++) { var G = 11; rf = g(); }
				for (auto v : [1]) { var G = 12; rr = g(); }
				try { throw 1; } catch (e) { var G = 13; rc = g(); }
				{ var G = 14; { var G = 15; rn = g(); } }
				{ var H = 3; function h() { return H; } rd = h(); }
				`${rb}${ri}${rf}${rr}${rc}${rn} ${rd} ${G}`;
			)", "111111 3 1");
		});
	}

	void base_slots_keep_the_class() {
		test("a_derived_value_in_a_base_typed_slot_keeps_its_class", [this]() {
			// a deliberate deviation from C++ slicing: the slot's static type is the base, the value stays whole
			expect("no slicing", R"(
				class B { int a = 1; } class D : B { int d = 2; }
				B b = D(); B c = b; array<B> xs = [D()];
				function by_value(B p) -> string { return type_of(p); }
				`${type_of(b)} ${type_of(c)} ${type_of(cast<B>(D()))} ${cast<D&>(b).d} ${type_of(xs[0])} ${by_value(D())}`;
			)", "D D D 2 D D", warn_region);
		});
	}

	void json_tagged_values() {
		test("a_json_round_trip_keeps_chars_and_non_string_map_keys", [this]() {
			// {"$char": n} and {"$map": [[k, v], ...]} carry what a JSON object cannot; any other shape of those keys stays a map
			expect("json tags", R"(
				class Inv { char grade = 'a'; map<int, string> slots; map<char, int> counts; map<int, string> none; var loose = {1: "x", "k": 2}; }
				Inv a; a.grade = 'z'; a.slots[3] = "sword"; a.slots[10] = "bow"; a.counts['q'] = 2;
				var b = from_json(to_json(a));
				`${to_json('z')}|${to_json({1: "a", 2: "b"})}|${to_json({"s": 'c'})}|${type_of(b.grade)}:${b.grade}|${b.slots[3]},${b.slots[10]}|${b.counts['q']}|${b.none.size()}|${b.loose[1]}${b.loose["k"]}|${type_of(from_json("{\"$char\": 300}"))}|${type_of(from_json("{\"$map\": [1]}"))}|${type_of(from_json("{\"$map\": [[1, 2]], \"x\": 1}"))}`;
			)", "{\"$char\":122}|{\"$map\":[[1,\"a\"],[2,\"b\"]]}|{\"s\":{\"$char\":99}}|char:z|sword,bow|2|0|x2|map|map|map", warn_region);
			// a float no JSON number spells is {"$float": "inf" | "-inf" | "nan"}: a value, a map key and a vector component alike
			expect("json float tags", R"(
				map<float, int> m = {}; m[-math::INF] = 3; m[math::NAN_VALUE] = 4; m[1.0] = 1;
				var back = from_json(to_json(m));
				var v = from_json(to_json(math::vec2(math::INF, 1.0)));
				`${to_json([math::INF, -math::INF, math::NAN_VALUE])}|${to_json(m)}|${back.size()} ${back[math::NAN_VALUE]} ${back[-math::INF]}|${type_of(v)} ${v.x}|${type_of(from_json("{\"$float\": \"big\"}"))}`;
			)", "[{\"$float\":\"inf\"},{\"$float\":\"-inf\"},{\"$float\":\"nan\"}]|{\"$map\":[[{\"$float\":\"-inf\"},3],[1.0,1],[{\"$float\":\"nan\"},4]]}|3 4 3|math::vec2 inf|map", warn_region);
		});
	}

	void auto_fields() {
		test("an_auto_field_takes_its_initializers_static_type", [this]() {
			// auto = static type, for class fields: an int-initialized auto field is an int field, an auto without a static
			// type is a var
			expect("auto fields", R"(
				var FLAG = true;
				class C { auto n = 5; auto v = FLAG ? 1 : "s"; auto f = FLAG ? 1 : 2.5; }
				function t(function f) -> string { string r = "none"; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				t([]() { C c; c.n = true; return `${type_of(c.n)}:${c.n}`; }) + "|" +
				t([]() { C c; var f = 2.9; c.n = f; return c.n; }) + "|" +
				t([]() { C c; var s = "s"; c.n = s; return c.n; }) + "|" +
				t([]() { C c; c.v = 2.5; c.v = "t"; return `${type_of(c.v)}:${c.v}`; }) + "|" +
				t([]() { C c; return `${type_of(c.f)}:${c.f}`; });
			)", "int:1|Cannot assign float to field 'n' of C (declared int)|Cannot assign string to field 'n' of C (declared int)|string:t|float:1.0", warn_region);
		});
	}

	void numeric_ternary_auto() {
		test("an_auto_initialized_from_an_int_float_ternary_is_float", [this]() {
			// C++'s common type of int and float is float: the chosen arm converts; any other mixed ternary has no static type
			expect("numeric ternary", R"(
				var FLAG = true;
				function t(function f) -> string { string r = "none"; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				t([]() { auto x = FLAG ? 1 : 2.5; return `${type_of(x)}:${x}`; }) + "|" +
				t([]() { auto x = FLAG ? 2.5 : 1; x = 3; return `${type_of(x)}:${x}`; }) + "|" +
				t([]() { auto x = FLAG ? 1 : 2.5; var s = "s"; x = s; return x; }) + "|" +
				t([]() { auto y = FLAG ? 1 : "s"; y = 2.5; return type_of(y); });
			)", "float:1.0|float:3.0|Cannot assign string to 'x' (declared float)|float", warn_region);
			every_config("no warning for the numeric pair", "0|1", [](engine& e) {
				auto numeric = e.check("var F = true; auto x = F ? 1 : 2.5;");
				auto mixed = e.check("var F = true; auto y = F ? 1 : \"s\";");
				auto ternary = [](const auto& r) { size_t n = 0; for (const auto& d : r.diagnostics) { if (d.message.find("ternary") != std::string::npos) { ++n; } } return std::to_string(n); };
				return ternary(numeric) + "|" + ternary(mixed);
			});
		});
	}

	void from_json_constructor_errors() {
		test("from_json_propagates_a_raising_constructor", [this]() {
			// the load constructs the class as Name() would; that call's throw or error is the load's
			expect("from_json ctor", R"(
				class R { int v = 0; R() { throw "no R"; } }
				class Ok { int v = 0; }
				function t(function f) -> string { string r = "none"; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				t([]() { var x = from_json("{\"$type\": \"R\", \"$val\": {\"v\": 3}}"); return "loaded"; }) + "|" +
				t([]() { var x = from_json("{\"$type\": \"Ok\", \"$val\": {\"v\": 3}}"); return `${type_of(x)}:${x.v}`; });
			)", "no R|Ok:3", warn_region);
		});
	}

	void typed_range_for() {
		test("a_typed_range_for_variable_takes_each_element_as_its_declaration_would", [this]() {
			// by value: T x = element; by reference: an element that holds a T; auto over elements without a static type is a var
			expect("typed range-for", R"(
				class A { int v = 1; } class B : A { }
				function t(function f) -> string { string r = ""; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				t([]() { var a = [true, 2]; string s = ""; for (int v : a) { s += `${type_of(v)}:${v} `; } return s; }) + "|" +
				t([]() { var a = [1.5, 2.5]; string s = ""; for (int v : a) { s += `${type_of(v)}:${v} `; } return s; }) + "|" +
				t([]() { var a = [1, 2]; string s = ""; for (string v : a) { s += `${type_of(v)}:${v} `; } return s; }) + "|" +
				t([]() { var a = ["x"]; string s = ""; for (int v : a) { s += type_of(v); } return s; }) + "|" +
				t([]() { var a = [1, 2.5]; int n = 0; for (int& v : a) { v = v + 1; n += 1; } return n; }) + "|" +
				t([]() { var a = [B(), A()]; int n = 0; for (A& r : a) { r.v = 7; n += r.v; } return n; }) + "|" +
				t([]() { var a = [1, 2]; string s = ""; for (auto x : a) { x = "s"; s += x; } return s; }) + "|" +
				t([]() { array<int> b = [1]; string s = ""; for (auto x : b) { x = true; s += `${type_of(x)}:${x}`; } return s; });
			)", "int:1 int:2 |Cannot initialize 'v' (declared int) with float|Cannot initialize 'v' (declared string) with int|Cannot initialize 'v' (declared int) with string|Cannot initialize 'v' (declared int) with float|"
			    "14|ss|int:1", warn_region);
			every_config("checker", "2|0", [](engine& e) {
				auto bad = e.check("array<float> a = [1.5]; for (int& v : a) { } for (int v : a) { }");
				auto ok = e.check("array<int> a = [1]; for (int& v : a) { } for (float v : a) { }");
				return std::to_string(bad.error_count()) + "|" + std::to_string(ok.error_count());
			});
		});
	}

	void reference_store_types() {
		test("a_store_through_a_reference_obeys_the_variables_declared_type", [this]() {
			expect("ref store", R"(
				function t(function f) -> string { string r = "none"; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				function set37(int& a) { a = 3.7; }
				t([]() { int x = 1; var& r = x; var s = "s"; r = s; return type_of(x); }) + "|" +
				t([]() { int x = 1; var& r = x; r = 2.9; return `${type_of(x)}:${x}`; }) + "|" +
				t([]() { int x = 2; var& r = x; r = true; return `${type_of(x)}:${x}`; }) + "|" +
				t([]() { string x = "a"; var& r = x; r = 5; return `${type_of(x)}:${x}`; }) + "|" +
				t([]() { var x = 1; var& r = x; r = "free"; return type_of(x); }) + "|" +
				t([]() { int u = 1; set37(u); return `${type_of(u)}:${u}`; });
			)", "Cannot assign string to 'r' (declared int&)|Cannot assign float to 'r' (declared int&)|int:1|Cannot assign int to 'r' (declared string&)|string|Cannot assign float to 'a' (declared int&)", warn_region);
		});
		test("a_store_through_a_reference_copies_like_the_variables_own_store", [this]() {
			// value semantics hold through the reference: the typed variable takes a copy, never the source's node
			expect("ref store copies", R"(
				array<int> src = [1, 2]; array<int> dst = [9, 9];
				function fill(var& r) { r = src; r[0] = 5; }
				fill(dst);
				`${src[0]} ${dst[0]} ${dst.size()}`;
			)", "1 5 2", warn_region);
		});
	}

	void static_members_bind_references() {
		test("a_static_member_and_a_namespace_variable_bind_a_reference_parameter", [this]() {
			// C++: S::s and ns::x are lvalues; a reference parameter or a reference declaration binds their storage
			expect("static ref", R"(
				class S { static int s = 1; static string name = "a"; }
				namespace ns { int x = 1; }
				function bump(int& r) { r = r + 10; }
				function rename(string& r) { r += "b"; }
				function run() -> string { bump(S::s); bump(ns::x); rename(S::name); bump(S::s); return `${S::s} ${ns::x} ${S::name}`; }
				string first = run();
				S::s = 5; ns::x = 6; bump(S::s); bump(ns::x);
				string second = `${S::s} ${ns::x}`;
				function decls() -> string { var& a = S::s; int& b = ns::x; a = 40; b = 41; return `${S::s} ${ns::x}`; }
				first + "|" + second + "|" + decls();
			)", "21 11 ab|15 16|40 41", warn_region);
		});
	}

	void typed_reference_parameters() {
		test("a_typed_reference_parameter_binds_only_its_own_type", [this]() {
			expect("int& binding", R"(
				class A { int v = 1; } class B : A { }
				function set(int& a) { a = 7; }
				function seta(A& r) { r.v = 9; }
				function any(var& r) { r = "s"; }
				function t(function f) -> string { string r = "none"; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				var vi = 1; var vf = 1.5;
				t([]() { set(vf); return vf; }) + "|" + t([]() { set(vi); return vi; }) + "|" + t([]() { B b = B(); seta(b); return b.v; }) + "|" +
				t([]() { var x = 5; any(x); return x; }) + "|" + t([]() { var s = "x"; seta(s); return 1; });
			)", "Cannot pass float to parameter 'a' of 'set' (declared int&)|7|9|s|Cannot pass string to parameter 'r' of 'seta' (declared A&)", warn_region);
			every_config("checker", "1|0", [](engine& e) {
				auto bad = e.check("function set(int& a) { a = 7; } float u = 1.5; set(u);");
				auto ok = e.check("function set(int& a) { a = 7; } int n = 1; set(n); var v = 2.5; set(v);");
				return std::to_string(bad.error_count()) + "|" + std::to_string(ok.error_count());
			});
		});
	}

	void constructorless_call_text() {
		test("a_constructorless_class_called_with_arguments_has_one_text", [this]() {
			// an aggregate takes at most one initializer per field; a class with a private field is no aggregate
			expect("runtime text", R"(
				class Bare { int v = 1; }
				function t(function f) -> string { string r = "none"; try { f(); } catch (e) { r = e; } return r; }
				var one = 1; var two = "s";
				t([]() { var b = Bare(one, two); });
			)", "Too many initializers for 'Bare': it has 1 field, got 2", warn_region);
			every_config("checker text", "too many initializers for 'Bare': it has 1 field, got 2", [](engine& e) {
				auto r = e.check("class Bare { int v = 1; } var b = Bare(1, \"s\");");
				for (const auto& d : r.diagnostics) { if (d.message.find("initializers") != std::string::npos) { return d.message; } }
				return std::string("no diagnostic");
			});
			expect("runtime text, not an aggregate", R"(
				class Hidden { private: int v = 1; }
				function t(function f) -> string { string r = "none"; try { f(); } catch (e) { r = e; } return r; }
				var one = 1; var two = "s";
				t([]() { var b = Hidden(one, two); });
			)", "No constructor of 'Hidden' takes (int, string)", warn_region);
			every_config("checker text, not an aggregate", "no constructor of 'Hidden' takes (int, string)", [](engine& e) {
				auto r = e.check("class Hidden { private: int v = 1; } var b = Hidden(1, \"s\");");
				for (const auto& d : r.diagnostics) { if (d.message.find("constructor") != std::string::npos) { return d.message; } }
				return std::string("no diagnostic");
			});
		});
	}

	void typed_slice() {
		test("a_slice_of_a_typed_array_keeps_its_element_type", [this]() {
			expect("typed slice", R"(
				function t(function f) -> string { string r = "none"; try { r = `${f()}`; } catch (e) { r = "raised:" + e; } return r; }
				array<int> a = [1, 2, 3];
				array<string> s = ["x", "y"];
				var v = [1, "two", 3.0];
				t([]() { array::slice(a, 0, 2).push("x"); return 1; }) + "|" + t([]() { array<int> sl = array::slice(a, 1, 3); return sl.size(); }) + "|" +
				t([]() { array::slice(s, 0, 1).push(5); return 1; }) + "|" + t([]() { var sl = array::slice(v, 0, 2); sl.push("ok"); return sl.size(); });
			)", "raised:Cannot push 'string' to array<int>|2|raised:Cannot push 'int' to array<string>|3", warn_region);
		});
	}

	void missing_key_compound() {
		test("a_compound_on_a_missing_key_raises_its_read", [this]() {
			// a compound reads its target first, so a missing key raises as a read does, on every map, and nothing is inserted
			expect("missing key compound", R"(
				function t(function f) -> string { string r = "none"; try { r = `${f()}`; } catch (e) { r = "raised:" + e; } return r; }
				t([]() { map<string, int> m = {}; m["k"] += 5; return m.size(); }) + "|" +
				t([]() { map<string, int> m = {}; m["j"]++; return m.size(); }) + "|" +
				t([]() { map<string, float> m = {}; m["k"] -= 1.5; return m.size(); }) + "|" +
				t([]() { map<string, string> m = {}; m["k"] += "s"; return m.size(); }) + "|" +
				t([]() { map<int, int> m = {3: 0}; for (int x : [3, 1, 3]) { m[x] += 1; } return m.size(); }) + "|" +
				t([]() { var m = {}; m["k"] += 1; return m.size(); }) + "|" +
				t([]() { map<string, int> m = {}; m[5] += 1; return m.size(); }) + "|" +
				t([]() { map<int, int> m = {}; for (int x : [3, 1, 3]) { m[x] = m.get(x, 0) + 1; } return `${m[3]}${m[1]}${m.size()}`; });
			)", "raised:Key \"k\" not found in map|raised:Key \"j\" not found in map|raised:Key \"k\" not found in map|raised:Key \"k\" not found in map|"
			    "raised:Key 1 not found in map|raised:Key \"k\" not found in map|raised:Cannot use 'int' as a key of type 'string'|212", warn_region);
		});
		test("a_compound_on_a_temporary_map_reads_before_it_meets_the_rvalue_target", [this]() {
			expect("temporary map compound", R"(
				function t(function f) -> string { string r = "none"; try { f(); } catch (e) { r = "raised:" + e; } return r; }
				function fresh() -> map<string, int> { map<string, int> m = {}; return m; }
				function held() -> map<string, int> { map<string, int> m = {"k": 1}; return m; }
				t([]() { fresh()["k"] += 1; }) + "|" + t([]() { fresh()["k"]++; }) + "|" + t([]() { held()["k"] += 1; }) + "|" +
				t([]() { fresh()[5] += 1; });
			)", "raised:Key \"k\" not found in map|raised:Key \"k\" not found in map|raised:Cannot assign to rvalue expression|"
			    "raised:Cannot use 'int' as a key of type 'string'", warn_region);
		});
	}

	void signed_zero_keys() {
		test("zero_and_negative_zero_are_one_map_key", [this]() {
			expect("signed zero", R"(
				var m = {}; m[0.0] = 1; m[-0.0] = 2;
				map<float, int> t = {}; t[-0.0] = 5; t[0.0] += 1;
				float nan = math::INF - math::INF; var n = {}; n[nan] = 1; n[nan] = 2;
				var pz = {}; pz[0.0] = 1; var nz = {}; nz[-0.0] = 1;
				`${m.size()}${m[0.0]}${m[-0.0]}|${t.size()}${t[0.0]}|${n.size()}|${m.has(-0.0)}${(pz == nz)}`;
			)", "122|16|1|truetrue", warn_region);
		});
	}

	void to_string_builtin() {
		test("the_to_string_builtin_uses_a_classs_own_to_string", [this]() {
			expect("to_string(x)", R"(
				class T { int n = 3; string to_string() { return `t${n}`; } }
				class U { int n = 1; }
				class Bad { string to_string() { int z = 0; return `${(1 / z)}`; } }
				class Thrower { string to_string() { throw "nope"; return ""; } }
				function t(function f) -> string { string r = "none"; try { r = f(); } catch (e) { r = `raised:${e}`; } return r; }
				to_string(T()) + "|" + (to_string(T()) == (`${T()}`) ? "same" : "differs") + "|" + t([]() { return to_string(Bad()); }) + "|" +
				t([]() { return to_string(Thrower()); }) + "|" + to_string(5) + to_string("s");
			)", "t3|same|raised:Division by zero|raised:nope|5s", warn_region);
		});
	}

	void host_callbacks() {
		test("a_script_callback_behind_a_host_function_propagates_its_error", [this]() {
			every_config("host callback", "raised:Division by zero|raised:boom|7", [](engine& e) {
				e.add_function("apply", [](script_value cb) -> script_int { return convert_script_function<script_int(script_int)>(cb)(3); });
				return spec::render(e.execute(spec::in_warn_region(R"(
					function t(function f) -> string { string r = "none"; try { r = `${f()}`; } catch (e) { r = `raised:${e}`; } return r; }
					`${t([]() { return apply([](int x) { int z = 0; return x / z; }); })}|${t([]() { return apply([](int x) { throw "boom"; return x; }); })}|${apply([](int x) { return x + 4; })}`;
				)")));
			});
		});
		test("a_host_calling_a_script_callback_sees_the_error_as_an_exception", [this]() {
			every_config("host sees", "threw:Division by zero", [](engine& e) {
				e.execute(spec::in_warn_region("var cb = [](int x) { int z = 0; return x / z; };"));
				auto f = convert_script_function<int(int)>(e.get_variable("cb"));
				try { (void)f(3); return std::string("no throw"); }
				catch (const std::exception& ex) { return std::string("threw:") + ex.what(); }
			});
		});
	}

	void this_capture() {
		test("a_this_capture_is_the_instance_itself", [this]() {
			expect("[this]", R"(
				class C {
					int v = 1;
					int by_this() { auto l = [this]() { this.v = 8; }; l(); return v; }
					int bare() { auto l = [this]() { v = 9; }; l(); return v; }
					int later() { auto l = [this]() { return v; }; v = 4; return l(); }
					function keep() { return [this]() { v = v + 1; return v; }; }
				}
				C c = C();
				var inc = c.keep(); inc(); inc();
				`${C().by_this()}${C().bare()}${C().later()}${c.v}`;
			)", "8943", warn_region);
		});
	}

	void compound_operand_text() {
		test("a_compound_operand_error_has_one_text_at_every_destination", [this]() {
			// docs/site/reference/errors.html "Run-time errors", compound assignment: Cannot apply 'op=' to <target> and <operand>, a variable, a field, a static,
			// an element and a map entry alike
			expect("compound text", R"(
				class F { int n = 5; static int s = 5; }
				function t(function f) -> string { string r = "none"; try { f(); } catch (e) { r = e; } return r; }
				var s = "s";
				t([]() { int x = 5; x -= s; }) + "|" + t([]() { F f = F(); f.n -= s; }) + "|" + t([]() { F::s *= s; }) + "|" +
				t([]() { array<int> a = [5]; a[0] -= s; }) + "|" + t([]() { var a = [5]; a[0] %= s; }) + "|" +
				t([]() { map<string, int> m = {"k": 1}; m["k"] /= s; }) + "|" + t([]() { var m = {"k": 1.5}; m["k"] -= s; });
			)", "Cannot apply '-=' to int and string|Cannot apply '-=' to int and string|Cannot apply '*=' to int and string|"
			    "Cannot apply '-=' to int and string|Cannot apply '%=' to int and string|Cannot apply '/=' to int and string|"
			    "Cannot apply '-=' to float and string", warn_region);
		});
	}

	void membership_equality() {
		test("contains_index_of_and_has_use_the_languages_equality", [this]() {
			expect("membership ==", R"(
				class E { int v = 0; E(int x) { v = x; } bool operator==(E o) { return v == o.v; } }
				class P { int a = 1; int b = 2; }
				class B { bool operator==(B o) { throw "cmp"; return true; } }
				var es = [E(1), E(2)];
				var ps = [P()];
				var nums = [1, 2.5, "x", [1, 2]];
				array<int> ints = [3, 4];
				string r = `${es.contains(E(2))}${es.index_of(E(2))}${es.has(E(3))}|${ps.contains(P())}|${nums.index_of(2.5)}${nums.contains(1.0)}${nums.index_of([1, 2])}${ints.index_of(4.0)}${ints.contains(5)}`;
				string t = "none"; try { [B()].contains(B()); } catch (e) { t = "raised:" + e; }
				r + "|" + t;
			)", "true1false|true|1true31false|raised:cmp", warn_region);
		});
	}

	void switch_equality() {
		test("a_switch_case_matches_when_the_languages_equality_holds", [this]() {
			expect("switch ==", R"(
				class E { int v = 0; E(int x) { v = x; } bool operator==(E o) { return v == o.v; } }
				class P { int a = 1; int b = 2; }
				class B { bool operator==(B o) { throw "cmp"; return true; } }
				function pick(var x) -> string { switch (x) { case 1.0: return "one"; case "s": return "s"; default: return "none"; } }
				function inst(var x) -> string { switch (x) { case E(2): return "E2"; default: return "none"; } }
				function memberwise(var x) -> string { switch (x) { case P(): return "P"; default: return "none"; } }
				string r = pick(1) + pick(1.0) + pick("s") + pick(2) + "|" + inst(E(2)) + inst(E(3)) + memberwise(P());
				string t = "none"; try { switch (B()) { case B(): t = "hit"; } } catch (e) { t = "raised:" + e; }
				r + "|" + t;
			)", "oneonesnone|E2noneP|raised:cmp", warn_region);
		});
	}

	// A by-value argument takes its value when it is evaluated, left to right
	void call_argument_capture() {
		test("by_value_arguments_capture_their_values_left_to_right", [this]() {
			expect("call args", R"(
				var GA = [1, 2];
				var M = {"k": 2};
				var GG = [[2]];
				function f() { GA[1] = 100; return 1; }
				function fm() { M["k"] = 100; return 1; }
				function fg() { GG[0][0] = 100; return 1; }
				function add2(a, b) { return a + b; }
				function add2t(int a, int b) -> int { return a + b; }
				class C { int m(int a, int b) { return a + b; } }
				class P { int a = 0; int b = 0; P(int x, int y) { a = x; b = y; } }
				function run() -> string {
					string s = "";
					GA[1] = 2; s += `${add2(GA[1], f())},`;
					GA[1] = 2; s += `${add2t(GA[1], f())},`;
					GA[1] = 2; C c = C(); s += `${c.m(GA[1], f())},`;
					int t = 0; for (int i = 0; i < 3; i++) { GA[1] = 2; t += add2t(GA[1], f()); } s += `${t},`;
					GA[1] = 2; P p = P(GA[1], f()); s += `${(p.a + p.b)},`;
					s += `${add2(M["k"], fm())},${add2(GG[0][0], fg())}`;
					return s;
				}
				run();
			)", "3,3,3,9,3,3,3");
		});
	}

	// A shared_ptr<T> field holds T or a subclass, the rule a shared_ptr<T> declaration enforces (docs/site/guide/02-type-ladder.html
	// "The one conversion table", shared_ptr<T> row: one rule at a declaration, a field, a parameter, a return and a container element)
	void handle_field_admission() {
		test("a_shared_ptr_field_rejects_a_handle_holding_an_unrelated_class", [this]() {
			expect("field handle", R"(
				class B { int x = 1; }
				class D : B { int y = 2; }
				class E : B { int z = 3; }
				class H {
					shared_ptr<D> d = null;
					shared_ptr<B> b = null;
					void put(var v) { d = v; }
					void put_this(var v) { this.d = v; }
				}
				function t(function f) -> string { string r = ""; try { r = `${f()}`; } catch (e) { r = e; } return r; }
				t([]() { var v = new E(); H h = H(); h.d = v; return type_of(h.d); }) + "|" +
				t([]() { var v = new D(); H h = H(); h.d = v; return type_of(h.d); }) + "|" +
				t([]() { var v = new E(); H h = H(); h.b = v; return type_of(h.b); }) + "|" +
				t([]() { var v = new E(); H h = H(); h.put(v); return type_of(h.d); }) + "|" +
				t([]() { var v = new E(); H h = H(); h.put_this(v); return type_of(h.d); }) + "|" +
				t([]() { H h = H(); string s = ""; for (int i = 0; i < 4; i++) { var v = i < 3 ? new D() : new E(); try { h.put(v); s += "D"; } catch (e) { s += "x"; } } return s + type_of(h.d); });
			)", "Cannot assign E to field 'd' of H (declared shared_ptr<D>)|D|E|Cannot assign E to field 'd' of H (declared shared_ptr<D>)|"
			    "Cannot assign E to field 'd' of H (declared shared_ptr<D>)|DDDxD");
		});
	}
};

} // namespace jai::foundry::tests

using spec_conformance_tests = jai::foundry::tests::spec_conformance_tests;
FOUNDRY_REGISTER(spec_conformance_tests)
