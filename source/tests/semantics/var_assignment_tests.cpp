// docs/site/guide/03-value-semantics.html "A var holding a handle" and "A var holding an object": '=' into a var asks the object
// it holds first. A handle on the right rebinds; a value of the held class or a subclass assigns into the shared object (its
// operator= when one takes the value); any other value rebinds. A var holding an object by value runs the class's operator= when
// one of its overloads takes the value, else takes the new value. Every place a var lives answers alike.
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <memory>
#include <string>

namespace var_assignment_hosts {
	struct host_node {
		int64_t value = 0;
		std::shared_ptr<host_node> left;
		explicit host_node(int64_t a_value) : value(a_value) {}
	};
	struct host_gauge {
		int64_t value = 0;
		explicit host_gauge(int64_t a_value) : value(a_value) {}
	};
}

namespace jai::foundry::tests {

class var_assignment_tests : public suite {
public:
	var_assignment_tests() : suite("Var Assignment") {}

	static constexpr const char* k_classes = R"(
		class P { int x = 1; }
		class D : P { int z = 3; }
		class B { int y = 2; }
		class Q { int x = 1; void operator=(int v) { x = v; } }
		class R { int x = 1; void operator=(R o) { x = o.x + 100; } }
		class Holder { var m = null; var v = null; void bare() { v = 7; m = "s"; } }
		class Takes { string t = ""; Takes(var x) { x = "s"; t = type_of(x); } string m(var x) { x = 2.5; return to_string(x); } static string sm(x) { x = "s"; return type_of(x); } }
		class Converts { int v = 0; Converts(var x) { v = 1; } }
		coroutine string takes_gen(var x) { x = "s"; yield type_of(x); }
		void rebind_ref(var& r) { r = "s"; }
		function var_to_s(var x) { x = "s"; return type_of(x); }
		function untyped_to_s(x) { x = "s"; return type_of(x); }
		function var_to_int(var x) { x = 1; return type_of(x); }
		string run(function f) { string out = ""; try { out = f(); } catch (e) { out = "ERR " + e; } return out; }
	)";

	static void bind_hosts(const std::shared_ptr<jai::engine>& a_engine) {
		using var_assignment_hosts::host_node;
		using var_assignment_hosts::host_gauge;
		jai::dynamic_binder<host_node>(*a_engine, "HostNode")
			.constructor<int64_t>()
			.property("value", &host_node::value)
			.property("left", &host_node::left, jai::skip_type_check)
			.build();
		jai::dynamic_binder<host_gauge>(*a_engine, "HostGauge")
			.constructor<int64_t>()
			.property("value", &host_gauge::value)
			.method("=", [](host_gauge& a_self, const host_gauge& a_other) -> host_gauge& { a_self.value = a_other.value + 100; return a_self; })
			.build();
		a_engine->add_function("make_node", [](int64_t a_value) { return std::make_shared<host_node>(a_value); });
		a_engine->add_function("make_gauge", [](int64_t a_value) { return std::make_shared<host_gauge>(a_value); });
		a_engine->add_function("grow", [](std::shared_ptr<host_node> a_root, int64_t a_value) {
			a_root->left = std::make_shared<host_node>(a_value);
			return a_root;
		});
	}

	void expect(const std::string& a_what, const std::string& a_body, const std::string& a_expected) {
		auto eng = make_engine();
		jai::stdlib::register_all(eng);
		bind_hosts(eng);
		eng->execute(k_classes);
		auto value = eng->execute("run([]() { " + a_body + " })");
		check_eq(a_expected, value.is_null() ? std::string("<null>") : value.as_string(), a_what + ": " + a_body);
	}

	void forge_tests() override {
		test("a_local_var_holding_a_handle_follows_the_site_rules", [this]() {
			expect("a primitive rebinds", R"(var a = new P(); var b = a; a = "s"; return type_of(a) + " " + to_string(b.x);)", "string 1");
			expect("an int rebinds", R"(var a = new P(); a = 5; return type_of(a);)", "int");
			expect("a compatible value assigns into the shared object", R"(var a = new P(); var b = a; P n = P(); n.x = 7; a = n; return to_string(b.x) + " " + to_string(same_object(a, b));)", "7 true");
			expect("a subclass value assigns into it, the object taking the subclass", R"(var a = new P(); var b = a; D n = D(); n.x = 8; a = n; return to_string(b.x) + " " + type_of(b);)", "8 D");
			expect("an unrelated value rebinds", R"(var a = new P(); var b = a; a = B(); return type_of(a) + " " + to_string(b.x);)", "B 1");
			expect("a handle rebinds", R"(var a = new P(); var b = a; a = new P(); return to_string(same_object(a, b));)", "false");
			expect("a primitive rebinds even when operator= takes it", R"(var a = new Q(); var b = a; a = 5; return type_of(a) + " " + to_string(b.x);)", "int 1");
			expect("a compatible value runs the class's operator=", R"(var a = new R(); var b = a; R n = R(); n.x = 2; a = n; return to_string(b.x) + " " + to_string(same_object(a, b));)", "102 true");
			expect("a var that took a handle later rebinds", R"(var a = 1; a = new P(); a = "s"; return type_of(a);)", "string");
			expect("in a loop", R"(var a = new P(); var b = a; for (int i = 0; i < 3; ++i) { a = i; } return type_of(a) + " " + to_string(b.x);)", "int 1");
			expect("a var parameter", R"(auto f = [](var p) { p = "s"; return type_of(p); }; return f(new P());)", "string");
			expect("a var loop variable", R"(array<shared_ptr<P>> xs = [new P()]; string o = ""; for (var x : xs) { x = 5; o = type_of(x); } return o + " " + to_string(xs[0].x);)", "int 1");
			expect("a var from a call returning a handle", R"(auto mk = []() -> shared_ptr<P> { return new P(); }; var a = mk(); a = 3; return type_of(a);)", "int");
			expect("through a var&", R"(var a = new P(); var b = a; var& r = a; r = "s"; return type_of(a) + " " + to_string(b.x);)", "string 1");
			expect("an emptied handle", R"(var a = new P(); a.reset(); a = 5; return type_of(a);)", "int");
			expect("an emptied handle reset through a var&", R"(var a = new P(); var& r = a; r.reset(); a = B(); return type_of(a);)", "B");
		});

		test("a_var_holding_a_host_handle_follows_the_site_rules", [this]() {
			expect("a host-returned handle rebinds to a value its binding cannot assign", R"(var a = make_node(1); var b = a; a = HostNode(8); return type_of(a) + " " + to_string(a.value) + " " + to_string(b.value) + " " + to_string(same_object(a, b));)", "HostNode 8 1 false");
			expect("a new handle rebinds the same way", R"(var a = new HostNode(1); a = HostNode(8); return type_of(a) + " " + to_string(a.value);)", "HostNode 8");
			expect("a binding's operator= assigns into the shared object", R"(var a = make_gauge(1); var b = a; a = HostGauge(2); return to_string(b.value) + " " + to_string(same_object(a, b));)", "102 true");
			expect("a var field", R"(Holder h = Holder(); h.m = make_node(1); h.m = HostNode(8); return type_of(h.m) + " " + to_string(h.m.value);)", "HostNode 8");
			expect("an element", R"(array<var> xs = [make_node(1)]; xs[0] = HostNode(8); return type_of(xs[0]) + " " + to_string(xs[0].value);)", "HostNode 8");
			expect("a typed handle still raises", R"(shared_ptr<HostNode> h = make_node(1); h = HostNode(8); return "stored";)", "ERR Cannot assign HostNode to 'h' (declared shared_ptr<HostNode>)");
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			bind_hosts(eng);
			eng->execute("var root = null;");
			std::string out;
			for (int i = 0; i < 3; ++i) {
				out += eng->execute("root = HostNode(8); root = grow(root, 4); to_string(root.value) + to_string(root.left.value) + \" \"").as_string();
			}
			check_eq(std::string("84 84 84 "), out, "a global var taking a value, then a host handle, each run");
		});

		test("a_typed_handle_local_assigns_a_value_into_its_object", [this]() {
			expect("shared_ptr<R> local", R"(shared_ptr<R> h = new R(); shared_ptr<R> h2 = h; R n = R(); n.x = 2; h = n; return to_string(h2.x);)", "102");
			expect("shared_ptr<P> local takes a subclass value", R"(shared_ptr<P> h = new P(); shared_ptr<P> h2 = h; D n = D(); n.x = 4; h = n; return to_string(h2.x) + " " + type_of(h2);)", "4 D");
			expect("an unrelated value stays an error", R"(shared_ptr<P> h = new P(); h = B(); return "stored";)", "ERR Cannot assign B to 'h' (declared shared_ptr<P>)");
		});

		test("a_var_holding_an_object_runs_an_operator_that_takes_the_value", [this]() {
			expect("operator=(int) takes an int", R"(var a = Q(); a = 5; return type_of(a) + " " + to_string(a.x);)", "Q 5");
			expect("no overload takes a string: the var takes it", R"(var a = Q(); a = "s"; return type_of(a);)", "string");
			expect("operator=(R) takes an R", R"(var a = R(); R n = R(); n.x = 2; a = n; return to_string(a.x);)", "102");
			expect("operator=(R) does not take an int", R"(var a = R(); a = 3; return type_of(a);)", "int");
			expect("no operator=", R"(var a = P(); a = 3; return type_of(a);)", "int");
			expect("a handle on the right rebinds", R"(var a = R(); var h = new R(); a = h; return to_string(a.x) + " " + to_string(same_object(a, h));)", "1 true");
			expect("in a loop", R"(var a = Q(); for (int i = 0; i < 3; ++i) { a = i + 10; } return type_of(a) + " " + to_string(a.x);)", "Q 12");
			expect("a computed value", R"(var a = Q(); int k = 2; a = k + 3; return to_string(a.x);)", "5");
			expect("through a var&", R"(var a = Q(); var& r = a; r = 4; return to_string(a.x);)", "4");
			expect("a [&] capture", R"(var a = Q(); auto g = [&]() { a = 3; }; g(); return type_of(a) + " " + to_string(a.x);)", "Q 3");
			expect("a var parameter", R"(auto f = [](var p) { p = 5; return to_string(p.x); }; return f(Q());)", "5");
		});

		test("var_fields_elements_and_map_values_answer_alike", [this]() {
			expect("a field holding an object", R"(Holder h = Holder(); h.v = Q(); h.v = 6; return type_of(h.v) + " " + to_string(h.v.x);)", "Q 6");
			expect("a field holding an object takes what its operator does not", R"(Holder h = Holder(); h.v = Q(); h.v = "s"; return type_of(h.v);)", "string");
			expect("bare field stores in a method", R"(Holder h = Holder(); h.v = Q(); h.m = new P(); var mm = h.m; h.bare(); return to_string(h.v.x) + " " + type_of(h.m) + " " + to_string(mm.x);)", "7 string 1");
			expect("a field holding a handle", R"(Holder h = Holder(); h.m = new P(); var mm = h.m; P n = P(); n.x = 9; h.m = n; return to_string(mm.x) + " " + to_string(same_object(mm, h.m));)", "9 true");
			expect("a field in a loop", R"(Holder h = Holder(); h.v = Q(); for (int i = 0; i < 3; ++i) { h.v = i + 40; } return to_string(h.v.x);)", "42");
			expect("an element holding an object", R"(array<var> xs = [Q(), Q()]; for (int i = 0; i < 2; ++i) { xs[i] = i + 20; } return type_of(xs[1]) + " " + to_string(xs[1].x);)", "Q 21");
			expect("an untyped array's element", R"(var xs = [Q()]; xs[0] = 5; return to_string(xs[0].x);)", "5");
			expect("an element holding a handle", R"(array<var> xs = [new P()]; var e0 = xs[0]; P n = P(); n.x = 4; xs[0] = n; return to_string(e0.x) + " " + to_string(same_object(e0, xs[0]));)", "4 true");
			expect("an element holding a handle rebinds to a primitive", R"(array<var> xs = [new P()]; xs[0] = "s"; return type_of(xs[0]);)", "string");
			expect("a map value holding an object", R"(map<string, var> m = {"k": Q()}; m["k"] = 5; return to_string(m["k"].x);)", "5");
			expect("a map value in a loop", R"(map<int, var> m = {0: Q()}; for (int i = 0; i < 3; ++i) { m[0] = i + 30; } return to_string(m[0].x);)", "32");
			expect("a map value holding a handle", R"(map<string, var> m = {"k": new P()}; var m0 = m["k"]; P n = P(); n.x = 7; m["k"] = n; return to_string(m0.x) + " " + to_string(same_object(m0, m["k"]));)", "7 true");
		});

		test("a_var_or_untyped_parameter_takes_any_value", [this]() {
			expect("a float into an int argument's var", R"(auto f = [](var x) { x = 2.5; return to_string(x); }; return f(1);)", "2.5");
			expect("a compound store", R"(auto f = [](var x) { x += 0.5; return to_string(x); }; return f(1);)", "1.5");
			expect("a string into an int argument's var", R"(return var_to_s(1);)", "string");
			expect("a float, a char and a bool argument", R"(return var_to_s(1.5) + " " + var_to_s('c') + " " + var_to_s(true);)", "string string string");
			expect("a typed local's argument", R"(int n = 1; return var_to_s(n);)", "string");
			expect("an object, an array, a typed array and a map argument", R"(array<int> a = [1]; return var_to_int(P()) + " " + var_to_int([1, 2]) + " " + var_to_int(a) + " " + var_to_int({"k": 1});)", "int int int int");
			expect("an untyped parameter", R"(return untyped_to_s(1);)", "string");
			expect("untyped lambda and arrow parameters", R"(auto f = [](p) { p = 2.5; return to_string(p); }; auto g = p => { p = "s"; return type_of(p); }; auto h = (p) => { p = "s"; return type_of(p); }; return f(1) + " " + g(1) + " " + h(1);)", "2.5 string string");
			expect("through a var& to it", R"(auto f = [](var x) { var& a = x; a = "s"; return type_of(x); }; return f(1);)", "string");
			expect("from a [&] capture", R"(auto f = [](var x) { auto g = [&]() { x = "s"; }; g(); return type_of(x); }; return f(1);)", "string");
			expect("passed on to a var& parameter", R"(auto f = [](var x) { rebind_ref(x); return type_of(x); }; return f(1);)", "string");
			expect("defaults", R"(auto f = [](var x = 1) { x = "s"; return type_of(x); }; auto g = [](x = 1) { x = "s"; return type_of(x); }; return f() + " " + g();)", "string string");
			expect("after ++", R"(auto f = [](var x) { x++; x = "s"; return type_of(x); }; return f(1);)", "string");
			expect("a constructor, a method and a static", R"(return Takes(1).t + " " + Takes(1).m(1) + " " + Takes::sm(1);)", "string 2.5 string");
			expect("a coroutine", R"(auto g = takes_gen(1); return g.next();)", "string");
			expect("each call of a loop", R"(string r = ""; for (int i = 0; i < 3; ++i) { r = r + var_to_s(i); } return r;)", "stringstringstring");
			expect("a converting constructor's var parameter", R"(Converts c = 5; return to_string(c.v);)", "1");
			expect("an auto parameter still locks to its argument", R"(auto f = [](auto x) { x = "s"; return type_of(x); }; return f(1);)", "ERR Cannot assign string to 'x' (declared int)");
			expect("an auto parameter holding a handle locks too", R"(auto f = [](auto p) { p = 5; return type_of(p); }; return f(new P());)", "ERR Cannot assign int to 'p' (declared shared_ptr<P>)");
		});

		test("a_global_var_answers_like_a_local", [this]() {
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			eng->execute(k_classes);
			check_eq(std::string("string 1|Q 5|102 true"), eng->execute(R"(
				var ga = new P(); var gb = ga; ga = "s";
				var gq = Q(); gq = 5;
				var gr = new R(); var gr2 = gr; R gn = R(); gn.x = 2; gr = gn;
				type_of(ga) + " " + to_string(gb.x) + "|" + type_of(gq) + " " + to_string(gq.x) + "|" + to_string(gr2.x) + " " + to_string(same_object(gr, gr2))
			)").as_string(), "globals");
			check_eq(std::string("int string int"), eng->execute(R"(
				var gh = new P(); gh.reset(); gh = 5;
				var gs = 1; var& gr = gs; gr = new P(); gs = "s";
				namespace gns { var h = null; }
				gns::h = new P(); gns::h = 5;
				type_of(gh) + " " + type_of(gs) + " " + type_of(gns::h)
			)").as_string(), "a global that held a handle stays a var");
		});
	}
};

}

using var_assignment_tests = jai::foundry::tests::var_assignment_tests;
FOUNDRY_REGISTER(var_assignment_tests)
