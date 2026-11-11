// Spec oracles for the robustness pass: every expected value comes from the specification (docs/, the guide under
// docs/site/guide/, C++ where the docs name it), never from a backend. Every row runs on the interpreter and the VM, proofs on and off, VM passes off,
// the copy elisions off, and with injected cell misses when the build defines JAISCRIPT_MISS_INJECTION.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

struct robustness_probe_host { int x = 0; int get() const { return x; } };

namespace robustness {

struct run_config {
	bool vm = false;
	bool proofs = true;
	bool passes = true;
	bool alias = true;
	uint32_t miss = 0;
	const char* label = "";
};

inline std::vector<run_config> configs() {
	std::vector<run_config> out = {
		{false, true, true, true, 0, "interp"},
		{false, false, true, true, 0, "interp proofs off"},
		{false, true, true, false, 0, "interp alias off"},
		{true, true, true, true, 0, "vm"},
		{true, false, true, true, 0, "vm proofs off"},
		{true, true, false, true, 0, "vm passes off"},
		{true, true, true, false, 0, "vm alias off"},
	};
	if (engine::miss_injection_available()) {
		out.push_back({true, true, true, true, 1, "vm miss 1"});
		out.push_back({true, true, true, true, 3, "vm miss 3"});
	}
	return out;
}

inline std::shared_ptr<engine> make(const run_config& c) {
	auto e = jai::foundry::make_default_engine();
	e->set_backend(c.vm ? backend_type::vm : backend_type::interpreter);
	stdlib::register_all(*e);
	if (!c.proofs) { e->type_proofs(false); }
	if (!c.passes) { e->ir_passes(false); }
	if (!c.alias) { e->parameter_alias(false); e->local_alias(false); }
	if (c.miss) { e->cells_miss_every(c.miss); }
	return e;
}

inline std::string render(const script_value& v) {
	if (v.is_string()) { return v.as_string(); }
	if (v.is_null()) { return "<null>"; }
	return "<" + v.to_string() + ">";
}

} // namespace robustness

class robustness_oracle_tests : public suite {
public:
	robustness_oracle_tests() : suite("Robustness Oracles") {}

	// warn_region: the run time's verdict, the unit under [[warn_mode]]
	void expect(const std::string& label, const std::string& src, const std::string& expected, bool warn_region = false) {
		for (const auto& c : robustness::configs()) {
			auto e = robustness::make(c);
			std::string got;
			try { got = robustness::render(e->execute(warn_region ? "[[warn_mode]]; " + src : src)); }
			catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
			check_eq(expected, got, label + " [" + c.label + "]");
		}
	}

	void forge_tests() override {
		destructors();
		destruction_order();
		sorting();
		method_conversions();
		uncaught_texts();
		map_transforms();
		second_audit();
		spaceship_rewriting();
		local_alias_soundness();
		import_trees();
		stored_element_results();
		engine_teardown();
		cell_cycles();
	}

private:
	// strict load verdict: "" when the program loads, else the static check's text
	static std::string strict_load_error(const std::string& src) {
		auto e = robustness::make(robustness::configs()[0]);
		try { e->execute(src); } catch (const std::exception& ex) { return ex.what(); }
		return "";
	}

	// docs/site/guide/02-type-ladder.html "The one conversion table", docs/site/guide/07-classes.html "Choosing an overload": a by-value parameter runs the assignment table; a lone method
	// converts like a lone free function, among overloads the scorer takes the table's rows and a string never makes a bool
	// parameter viable; the strict checker agrees at load
	void method_conversions() {
		test("a_lone_method_converts_its_arguments_like_a_free_function", [this]() {
			expect("lone method", R"(
				class C {
					int f(int v) { return v; }
					string h(string s) { return s; }
					float g(float x) { return x; }
					bool b(bool x) { return x; }
					static int sf(int v) { return v + 100; }
				}
				int free_f(int v) { return v; }
				C c; var text = "x";
				`${c.f(true)},${c.h("5")},${c.g(true)},${c.b(text)},${c.b(0)},${C::sf(true)},${free_f(true)}`;
			)", "1,5,1.0,true,false,101,1");
			const std::string verdict = strict_load_error("class C { bool b(bool x) { return x; } int f(int v) { return v; } } C c; c.b(\"x\"); c.f(2.5);");
			check_true(verdict.find("argument 1 of 'b': cannot pass 'string' to 'x' declared 'bool': write !x.empty() or an explicit comparison") != std::string::npos, "strict rejects b(string): " + verdict);
			check_true(verdict.find("argument 1 of 'f': cannot pass 'float' to 'v' declared 'int': cast<int>(x) truncates toward zero, math::ifloor(x) rounds down") != std::string::npos, "strict rejects f(float): " + verdict);
		});
		test("overloaded_methods_score_the_conversion_table", [this]() {
			expect("method overload table", R"(
				class P { int v = 0; P(int x) { v = x; } }
				class D {
					string k(int v) { return "int"; }
					string k(string s) { return "string"; }
					string m(bool b) { return "bool"; }
					string m(int a, int b2) { return "pair"; }
					string n(float x) { return "float"; }
					string n(string s) { return "string"; }
					string w(P p) { return `P${p.v}`; }
					string w(string s, int z) { return "s"; }
				}
				D d;
				`${d.k(true)},${d.k("c")},${d.n(true)},${d.n(3)},${d.w(5)},${d.m(true)}`;
			)", "int,string,float,float,P5,bool");
			const std::string float_rivals = R"(
				class D { string k(int v) { return "int"; } string k(string s) { return "string"; } }
				D d; string out = "";
				try { out = d.k(2.5); } catch (e) { out = e; }
				out;
			)";
			expect("a float takes no int overload", float_rivals, "No overload of 'k' takes (float)", true);
			const std::string verdict = strict_load_error(float_rivals);
			check_true(verdict.find("no overload of 'k' matches this call") != std::string::npos, "strict rejects k(float): " + verdict);
		});
		test("a_string_never_makes_a_bool_parameter_viable_among_overloads", [this]() {
			const std::string rivals = R"(
				class D { string m(bool b) { return "bool"; } string m(int a) { return "int"; } }
				class E { E(bool b) { } E(int a) { } }
				class F { int f(int v) { return v; } }
				string run() {
					string out = "";
					D d; F ff;
					try { out += d.m("s"); } catch (e) { out += e; }
					out += "|";
					try { E x = E("s"); out += "built"; } catch (e) { out += e; }
					out += "|";
					try { out += ff.f("s"); } catch (e) { out += e; }
					return out;
				}
				run();
			)";
			expect("string to bool among rivals", rivals,
			       "No overload of 'm' takes (string)|No constructor of 'E' takes (string)|Cannot pass string to parameter 'v' of 'f' (declared int)",
			       true);
			const std::string verdict = strict_load_error(rivals);
			check_true(verdict.find("no overload of 'm' matches this call") != std::string::npos, "strict rejects m(string): " + verdict);
			check_true(verdict.find("no overload of 'E' matches this call") != std::string::npos, "strict rejects E(string): " + verdict);
			check_true(verdict.find("cannot pass 'string' to 'v' declared 'int'") != std::string::npos, "strict rejects f(string): " + verdict);
			// the one overload the argument count admits binds a var's string by truthiness; a string the checker sees is refused at load
			expect("the arity's one overload converts", R"(
				class D { string m(bool b) { return `bool:${b}`; } string m(int a, int b2) { return "pair"; } }
				class E { bool v = false; E(bool b) { v = b; } E(int a, int b) { } }
				var s = "s";
				D d; E x = E(s);
				`${d.m(s)},${x.v}`;
			)", "bool:true,true");
			const std::string lone_arity = strict_load_error(R"(
				class D { string m(bool b) { return `bool:${b}`; } string m(int a, int b2) { return "pair"; } }
				class E { bool v = false; E(bool b) { v = b; } E(int a, int b) { } }
				D d; E x = E("s");
				`${d.m("s")},${x.v}`;
			)");
			check_true(lone_arity.find("no overload of 'm' matches this call") != std::string::npos, "strict rejects the lone arity's m(string): " + lone_arity);
			check_true(lone_arity.find("no overload of 'E' matches this call") != std::string::npos, "strict rejects the lone arity's E(string): " + lone_arity);
		});
		test("a_cached_arity_answer_never_lets_a_string_reach_a_bool_overload", [this]() {
			expect("arity cache", R"(
				class D { string m(bool b) { return "bool"; } string m(int a) { return "int"; } string m(int a, int b) { return "pair"; } }
				string run() {
					D d; string out = d.m(true) + d.m(false) + d.m(1, 2) + d.m(1, 2);
					var s = "s";
					try { out += d.m(s); } catch (e) { out += "|" + e; }
					return out;
				}
				run();
			)", "boolboolpairpair|No overload of 'm' takes (string)", true);
		});
		test("a_var_parameter_takes_what_only_a_loose_row_would_convert", [this]() {
			expect("var ahead of loose rows", R"(
				class W { int k = 0; }
				class C {
					string p(int x) { return "int"; }
					string p(string s) { return "string"; }
					string p(var v) { return "var"; }
				}
				C c;
				c.p(5) + "," + c.p("a") + "," + c.p(2.5) + "," + c.p(true) + "," + c.p(W()) + "," + c.p([1]);
			)", "int,string,var,int,var,var");
		});
	}

	// C++ [class.dtor]: a class without a user destructor has an implicit one that destroys its bases
	void destructors() {
		test("a_derived_class_without_a_destructor_runs_its_bases_destructors", [this]() {
			expect("implicit destructor", R"(
				string log = "";
				class A { ~A() { log += "~A"; } }
				class B : A { }
				class C : A { ~C() { log += "~C"; } }
				class E : A { E() { } }
				class F : B { int k = 0; }
				{ A a; } log += "|";
				{ B b; } log += "|";
				{ B b = B(); } log += "|";
				{ C c; } log += "|";
				{ E e; } log += "|";
				{ F f; } log += "|";
				{ auto p = new B(); } log += "|";
				function scoped() { B b; B copy = b; } scoped(); log += "|";
				log;
			)", "~A|~A|~A|~C~A|~A|~A|~A|~A~A|");
		});
	}

	// C++ [stmt.dcl], [class.dtor]: locals die in reverse declaration order when their frame ends (a return, a throw), a
	// vector's elements first to last when it dies, an object's members after its destructor body in reverse order
	void destruction_order() {
		test("every_copy_runs_its_destructor_once_whatever_the_elisions", [this]() {
			expect("copy destructor counts", R"(
				int gone = 0;
				class T { int v; T(int a) { v = a; } ~T() { gone++; } }
				T id(T t) { return t; }
				void take(T t) { }
				int read(T t) { return t.v; }
				function run() {
					T a = T(1);
					T b = a;
					take(a);
					T c = id(a);
					array<T> arr = [a, b];
					arr.push(c);
					T d = arr[0];
					T e = d;
					int r = read(e) + read(arr[1]);
				}
				run();
				`${gone}`;
			)", "11");
		});
		test("frame_locals_die_in_reverse_declaration_order", [this]() {
			expect("frame order", R"(
				string log = "";
				class Token { string n; Token(string s) { n = s; } ~Token() { log += n; } }
				function f() { Token a = Token("a"); Token b = Token("b"); array<Token> l = [Token("x"), Token("y")]; Token c = Token("c"); }
				function g() { Token a = Token("a"); Token b = Token("b"); throw "boom"; }
				function h(int k) -> int { Token a = Token("a"); if (k > 0) { Token b = Token("b"); return k; } Token c = Token("c"); return 0; }
				class M { void run() { Token a = Token("a"); Token b = Token("b"); } }
				class Holder { Token first = Token("1"); Token second = Token("2"); ~Holder() { log += "H"; } }
				f(); log += "|";
				try { g(); } catch (e) { log += "!"; } log += "|";
				h(1); log += ","; h(0); log += "|";
				M m; m.run(); log += "|";
				auto lam = () => { Token a = Token("a"); Token b = Token("b"); };
				lam(); log += "|";
				{ Holder hh; } log += "|";
				{ Token a = Token("a"); Token b = Token("b"); { Token i = Token("i"); Token j = Token("j"); } log += "."; }
				class Key { int v; Key(int a) { v = a; } ~Key() { log += `k${v}`; } int operator<=>(const Key& o) { return v - o.v; } }
				function keyed() { map<Key, int> m; m[Key(5)] = 1; m[Key(3)] = 2; m[Key(9)] = 3; log += ":"; map<int, Token> t; t[2] = Token("q"); t[1] = Token("p"); log += ":"; }
				log += "|"; keyed();
				log;
			)", "cyxba|ba!|ba,ca|ba|ba|H21|ji.ba|::qpk9k5k3");
		});
	}

	// an uncaught error's what() is its text alone; the category message stands in only for an error
	// that carries no text
	void uncaught_texts() {
		test("an_uncaught_error_reads_its_text_without_a_category_suffix", [this]() {
			for (const auto& c : robustness::configs()) {
				auto uncaught = [&](const std::string& src) {
					auto e = robustness::make(c);
					try { e->execute("[[warn_mode]]; " + src); } catch (const std::exception& ex) { return std::string(ex.what()); }
					return std::string("no error");
				};
				const std::string label = std::string(" [") + c.label + "]";
				check_eq(std::string("No overload of 'add' takes (bool)"),
				         uncaught("class C { int add(int a, int b) { return a + b; } int add(string a, string b, string c) { return 0; } } C k; k.add(true);"),
				         "overload miss" + label);
				check_eq(std::string("Cannot resume a completed coroutine"),
				         uncaught("coroutine int g() { yield 1; } auto h = g(); h.next(); h.next(); h.next();"), "next past the end" + label);
				check_eq(std::string("Division by zero"), uncaught("int z = 0; int q = 5 / z;"), "division" + label);
				check_eq(std::string("boom: Type mismatch"), uncaught("throw \"boom: Type mismatch\";"), "a thrown text stays whole" + label);
			}
		});
	}

	// map.filter(pred) keeps the accepted entries in the receiver and returns nothing, map::filter(m, pred)
	// returns them as a new map and leaves m alone (the array dot / namespace pairs)
	void map_transforms() {
		test("map_dot_filter_mutates_and_map_namespace_filter_copies", [this]() {
			expect("map filter", R"(
				string run() {
					map<string, int> m = {"a": 1, "b": 2, "c": 3, "d": 4};
					m.filter((k, v) => v != 2);
					m.filter((k, v) => v != 4);
					string out = `${m.size()},${m.contains("b")},${m.contains("d")},`;
					map<string, int> m2 = {"a": 1, "b": 2};
					map<string, int> c = map::filter(m2, (k, v) => v == 1);
					out += `${m2.size()},${c.size()},${c.contains("a")},`;
					map<string, int> m3 = {"x": 1, "y": 2, "z": 3};
					m3.filter((k, v) => { if (k == "x") { m3.remove("z"); } return v != 2; });
					out += `${m3.size()},${m3.contains("x")}`;
					return out;
				}
				run();
			)", "2,false,false,2,1,true,1,true");
		});
		test("map_remove_if_whose_predicate_writes_the_map_visits_a_snapshot", [this]() {
			expect("map remove_if writes", R"(
				string run() {
					map<int, int> m = {};
					for (int i = 0; i < 64; i++) { m[i] = i; }
					int removed = m.remove_if((k, v) => { if (k % 8 == 0) { m.remove(k + 1); m[1000 + k] = k; } return k % 2 == 0; });
					return `${removed},${m.size()}`;
				}
				run();
			)", "32,32");
		});
	}

	// Extend identity, thrown objects' text, parse texts that name the fix, top-level call
	// order under strict, host overload misses
	void second_audit() {
		// extend re-classes in place and its result names the argument (C++'s T& extend(T&, Class)):
		// `var w = extend(v, B)` copies, `var& w = extend(v, B)` aliases, a chained call reaches v, a handle's copy shares
		test("extend_reclasses_in_place_and_its_result_follows_the_call_result_rule", [this]() {
			expect("extend result binding", R"(
				class A { int x = 1; int bump() { x++; return x; } } class B : A { int d = 0; }
				string run() {
					var v = A(); var w = extend(v, B); w.d = 3;
					string out = `${same_object(v, w)},${v.d},${reflect::type_name(v)}${reflect::type_name(w)}`;
					var v1 = A(); var& w1 = extend(v1, B); w1.d = 4;
					out += `|${same_object(v1, w1)},${v1.d}`;
					var v2 = A(); extend(v2, B).bump(); out += `|${v2.x}`;
					A loc = A(); var c = extend(loc, B); c.d = 5; var& r = extend(loc, B); r.d = 6;
					out += `|${loc.d},${c.d}`;
					var p = new A(); var q = extend(p, B); q.d = 7;
					out += `|${same_object(p, q)},${p.d}`;
					return out;
				}
				run();
			)", "false,0,BB|true,4|2|6,5|true,7", true);
		});
		test("a_thrown_object_reads_as_its_to_string", [this]() {
			expect("thrown object text", R"(
				class Oops { int code = 7; string to_string() { return `oops ${code}`; } }
				class Bad { string to_string() { throw "inner"; } }
				string out = "";
				try { throw Oops(); } catch (e) { out += `${e}|${e.message}|${e.code}`; }
				try { throw Bad(); } catch (e) { out += `|${e}`; }
				out;
			)", "oops 7|oops 7|7|inner");
			for (const auto& c : robustness::configs()) {
				auto e = robustness::make(c);
				std::string what;
				try { e->execute("class Oops { string to_string() { return \"oops\"; } } throw Oops();"); } catch (const std::exception& ex) { what = ex.what(); }
				check_eq(std::string("oops"), what, std::string("uncaught [") + c.label + "]");
			}
		});
		test("parse_errors_name_the_fix", [this]() {
			auto parse_text = [](const std::string& src) {
				auto e = jai::foundry::make_engine();
				try { e->execute(src); } catch (const std::exception& ex) { return std::string(ex.what()); }
				return std::string("no error");
			};
			const std::string en = parse_text("enum F { a, b = c }");
			check_true(en.find("enum value 'b' names 'c', which is not an earlier value of 'F'") != std::string::npos, en);
			const std::string shift = parse_text("enum F { a = 1 << 64 }");
			check_true(shift.find("enum value 'a' shifts by 64: the shift amount must be between 0 and 63") != std::string::npos, shift);
			const std::string text = parse_text("enum F { a = \"one\" }");
			check_true(text.find("enum value 'a' needs a constant integer") != std::string::npos, text);
			const std::string last = parse_text("enum F { a = 9223372036854775807, b }");
			check_true(last.find("enum value 'b' overflows the int range: the value before it is the largest int") != std::string::npos, last);
			const std::string sp = parse_text("array<int> a = [1, 2]; auto b = [...a, 3];");
			check_true(sp.find("JaiScript has no spread '...': join arrays with array::concat(a, b)") != std::string::npos, sp);
			// a lambda body that fails to parse leaves no scope behind: the enclosing function's own local reads as its own
			// (ASAN: the stale scope pointed at the freed lambda, a heap use after free in resolve_outer_use)
			const std::string lam = parse_text("function f() {\n\tint a = 1;\n\tauto l = []() { int b = ; };\n\treturn a;\n}\n");
			check_true(lam.find("expected expression") != std::string::npos && lam.find("does not capture enclosing local 'a'") == std::string::npos, lam);
			const std::string fn = parse_text("function g() {\n\tint a = 1;\n\tfunction h() { int b = ; }\n\treturn a;\n}\n");
			check_true(fn.find("expected expression") != std::string::npos && fn.find("cannot use enclosing local 'a'") == std::string::npos, fn);
			const std::string sc = parse_text("int f(int x, int y) { return x + y; } array<int> a = [1, 2]; f(...a);");
			check_true(sc.find("JaiScript has no spread '...'") != std::string::npos, sc);
		});
		test("a_namespace_declares_coroutines", [this]() {
			expect("namespace coroutine", R"(
				namespace ns {
					coroutine int co(int n) { for (int i = 0; i < n; i++) { yield i; } }
					coroutine function names() { yield "a"; yield "b"; }
					int plain() { return 1; }
				}
				string s = "";
				for (auto v : ns::co(3)) { s += `${v}`; }
				auto h = ns::names(); s += h.next(); s += h.next();
				s + to_string(ns::plain());
			)", "012ab1");
		});
		test("a_top_level_call_before_the_declaration_is_a_strict_load_error", [this]() {
			const std::string verdict = strict_load_error("print(later(2));\nint later(int x) { return x; }\n");
			check_true(verdict.find("'later' is called before its declaration (line 2): top-level code runs in order") != std::string::npos, verdict);
			check_eq(std::string(""), strict_load_error("int f() { return later(1); }\nint later(int x) { return x; }\nf();\n"), "a body may call further down");
			check_eq(std::string(""), strict_load_error("int later(int x) { return x; }\nlater(2);\n"), "declared first");
			expect("warn keeps the run-time text", "string out = \"\"; try { later(2); } catch (e) { out = e; } int later(int x) { return x; } out;",
			       "Undefined variable 'later'", true);
			// a variable, a class or a function value is the same: top-level code runs in order
			const std::pair<const char*, const char*> uses[] = {
				{"print(G);\nint G = 4;\n", "'G' is used before its declaration (line 2): top-level code runs in order, so declare it above the use or make the use after it"},
				{"auto a = A();\nclass A { int v = 1; }\n", "'A' is used before its declaration (line 2)"},
				{"var f = fib2;\nint fib2() { return 1; }\n", "'fib2' is used before its declaration (line 2)"},
				{"if (true) { print(G); }\nint G = 4;\n", "'G' is used before its declaration (line 2)"},
				// a namespace's top-level code admits its members in order too, across its reopenings and nested blocks
				{"namespace N {\n\tint x = f();\n\tint f() { return 3; }\n}\n", "'f' is called before its declaration (line 3): top-level code runs in order"},
				{"namespace N {\n\tint x = y;\n\tint y = 3;\n}\n", "'y' is used before its declaration (line 3)"},
				{"namespace N {\n\tvar k = f;\n\tint f() { return 3; }\n}\n", "'f' is used before its declaration (line 3)"},
				{"namespace N { int x = f(); }\nnamespace N { int f() { return 3; } }\n", "'f' is called before its declaration (line 2)"},
				{"namespace M { namespace N {\n\tint x = f();\n\tint f() { return 3; }\n} }\n", "'f' is called before its declaration (line 3)"},
			};
			for (const auto& [src, text] : uses) { check_true(strict_load_error(src).find(text) != std::string::npos, std::string(src) + ": " + strict_load_error(src)); }
			expect("a namespace member is not there before its declaration runs",
			       "namespace Q { int x = g(); int g() { return 3; } } Q::x;", "throw:Undefined variable 'g'", true);
			check_eq(std::string(""), strict_load_error("int f() { return G + A().v; }\nint G = 4;\nclass A { int v = 1; }\nf();\n"), "a body may use names further down");
			// before the namespace declares it, a bare name reaches an enclosing namespace's member or the global (C++); a body runs later
			check_eq(std::string(""), strict_load_error("int f() { return 9; }\nnamespace N { int x = f(); int f() { return 3; } }\n"), "the global answers first");
			check_eq(std::string(""), strict_load_error("namespace M { int f() { return 5; } namespace N { int x = f(); int f() { return 3; } } }\n"), "the enclosing namespace answers first");
			check_eq(std::string(""), strict_load_error("namespace N { int f() { return g(); } int g() { return 3; } int x = f(); auto l = []() { return h(); }; int h() { return 1; } }\n"), "bodies run later");
			check_eq(std::string(""), strict_load_error("namespace N { int a = 1; }\nnamespace N { int x = a + 1; }\n"), "an earlier block declared it");
			// a name the engine already holds (an earlier run, another unit) is there when the use runs
			auto e = robustness::make(robustness::configs()[0]);
			e->execute("int G = 1; int fib2() { return 1; } class A { int v = 1; }");
			check_eq(std::string("1|1|1"), e->execute("string r = `${G}|${fib2()}|${A().v}`; int G = 2; int fib2() { return 2; } class A { int v = 2; } r;").as_string());
		});
		test("a_host_overload_miss_names_the_function_and_the_arguments", [this]() {
			for (const auto& c : robustness::configs()) {
				auto e = robustness::make(c);
				e->add_function("h", [](double a) -> double { return a; });
				e->add_function("h", [](double a, double b) -> double { return a + b; });
				std::string got;
				try { got = robustness::render(e->execute("[[warn_mode]]; string m = \"\"; var s = \"x\"; try { h(s); } catch (err) { m = err; } m")); }
				catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(std::string("No overload of 'h' takes (string)"), got, std::string("host miss [") + c.label + "]");
			}
		});
	}

	// C++20 [over.match.oper]: with no operator< in the chain a < b is (a <=> b) < 0, reversed 5 < v is 0 < (v <=> 5), == and !=
	// come from (a <=> b) == 0 when no operator== is declared; each comparison calls <=> once, operands evaluated once, left first;
	// sort, contains and index_of use the same operators (metamorphic: sorted order agrees with pairwise <)
	void spaceship_rewriting() {
		test("spaceship_drives_every_comparison_once_per_use", [this]() {
			expect("spaceship rewriting", R"(
				int calls = 0;
				string order = "";
				class V {
					int x;
					V(int a) { x = a; }
					int operator<=>(V o) { calls++; return x - o.x; }
					int operator<=>(int o) { calls++; return x - o; }
				}
				V mk(string tag, int v) { order += tag; return V(v); }
				string run() {
					V a = V(1); V b = V(2); V c = V(2);
					string out = `${(a < b)}${(b <= c)}${(b > a)}${(c >= b)}${(a > b)}${(b < c)}${calls},`;
					calls = 0;
					out += `${(b == c)}${(a == b)}${(b != c)}${(a != b)}${calls},`;
					calls = 0;
					out += `${(5 < a)}${(0 < a)}${(1 <= a)}${(2 > a)}${calls},`;
					calls = 0;
					out += `${(5 <=> a)}${(a <=> 5)}${(1 <=> a)}${(a == 1)}${(1 == a)}${(a != 1)}${(2 != a)}${calls},`;
					calls = 0;
					bool lt = mk("L", 1) < mk("R", 2);
					out += `${lt}${order}${calls},`;
					array<V> arr = [V(3), V(1), V(2), V(1), V(5), V(0)];
					arr.sort();
					bool sorted = true;
					for (int i = 1; i < arr.size(); i++) { if (arr[i] < arr[i - 1]) { sorted = false; } }
					string xs = ""; for (auto& v : arr) { xs += `${v.x}`; }
					out += `${xs}${sorted}${arr.contains(V(2))}${arr.contains(V(9))}${arr.index_of(V(2))}`;
					return out;
				}
				run();
			)", "truetruetruetruefalsefalse6,truefalsefalsetrue4,falsetruetruetrue4,1-40truetruefalsetrue7,trueLR1,011235truetruefalse3");
		});
	}

	// a read-only by-value local binds its source only when nothing after it can tell; every
	// route that writes the source after the copy (operators, conversions, accessors, callbacks, coroutines, reflection,
	// references) leaves the copy unchanged, elisions on or off (C++: T x = lvalue copies)
	void local_alias_soundness() {
		test("a_by_value_local_never_sees_a_later_write_of_its_source", [this]() {
			const std::string prelude = R"(
				class T { int v = 1; array<int> a = [1]; T() {} T(int k) { v = k; } }
				T G;
				array<T> GA = [T(1)];
				class Evil {
					int k = 0;
					string to_string() { G.v = 7; return "evil"; }
					int to_int() { G.v = 7; return 3; }
					bool operator==(Evil o) { G.v = 7; return true; }
					int operator<=>(Evil o) { G.v = 7; return 0; }
					Evil operator+(Evil o) { G.v = 7; return o; }
					int _get_q() { G.v = 7; return 1; }
				}
				class Conv { int z = 0; Conv(int i) { G.v = 7; z = i; } }
				int takeconv(Conv c) { return c.z; }
				void wr(T& t) { t.v = 7; }
				void wri(int& i) { i = 7; }
				coroutine int co() { G.v = 7; yield 1; }
			)";
			struct row { const char* name; const char* setup; const char* src; const char* attack; };
			const row rows[] = {
				{"to_string", "", "G", "Evil e; string s = to_string(e);"},
				{"print", "", "G", "Evil e; string s = `${e}`;"},
				{"to_int", "", "G", "Evil e; int i = e;"},
				{"operator==", "", "G", "Evil e; Evil e2; bool b = e == e2;"},
				{"spaceship", "", "G", "Evil e; Evil e2; bool b = e < e2;"},
				{"operator+", "", "G", "Evil e; Evil e2; Evil e3 = e + e2;"},
				{"accessor", "", "G", "Evil e; int q = e.q;"},
				{"sort", "", "G", "array<Evil> es = [Evil(), Evil()]; es.sort();"},
				{"converting ctor", "", "G", "int r = takeconv(5);"},
				{"reference parameter", "", "G", "wr(G);"},
				{"field reference", "", "G", "wri(G.v);"},
				{"reference before", "auto& r = G;", "G", "r.v = 7;"},
				{"lambda", "auto fn = [&]() { G.v = 7; };", "G", "fn();"},
				{"callback", "", "G", "array<int> xs = [1]; xs.for_each((k) => { G.v = 7; });"},
				{"coroutine", "", "G", "auto c = co(); c.next();"},
				{"reflect", "", "G", "reflect::set(G, \"v\", 7);"},
				{"element", "", "GA[0]", "for (auto& t : GA) { t.v = 7; }"},
				{"local swap", "T s = T(1); T t2 = T(7);", "s", "T tmp = s; s = t2; t2 = tmp;"},
				{"local lambda", "T s = T(1); auto fn = () => { s.v = 7; };", "s", "fn();"},
				{"local element", "array<T> la = [T(1)];", "la[0]", "la[0].a.push(5); la[0].v = la[0].a.size() + 5;"},
			};
			for (const auto& r : rows) {
				const std::string src = prelude + "string f() {\n" + r.setup + "\nT x = " + r.src + ";\n" + r.attack +
					"\nreturn `${x.v}${x.a.size()}`;\n}\nf();";
				expect(std::string("alias ") + r.name, src, "11", true);
			}
		});
	}

	// docs/site/guide/10-modules.html "import: cached module declaration" and "A file's own reload hook": an import re-runs only the module whose closure changed (its
	// importer does not), a vanished module raises once and then waits, a returning one re-runs, a group applies all or
	// nothing, and a unit's reload hook sees its variables as they were
	void import_trees() {
		test("import_trees_rerun_only_what_changed_and_groups_apply_all_or_nothing", [this]() {
			int run = 0;
			for (const auto& c : robustness::configs()) {
				const std::string label = std::string(" [") + c.label + "]";
				const std::filesystem::path dir = test_temp_directory() / ("robustness_imports_" + std::to_string(run++));
				std::filesystem::create_directories(dir);
				auto write = [&](const char* name, const std::string& text) { std::ofstream(dir / name, std::ios::binary) << text; };
				auto path = [&](const char* name) { return (dir / name).generic_string(); };
				auto e = robustness::make(c);
				e->import_recheck_interval(0);
				std::string log;
				e->add_function("note", [&log](std::string t) { log += t; });
				write("A.jai", "note(\"A\");\nimport \"B.jai\";\n");
				write("B.jai", "note(\"B\");\n");
				const std::string importA = "import \"" + path("A.jai") + "\";";
				auto step = [&](const std::string& src) {
					log.clear();
					try { e->execute(src); } catch (const std::exception& ex) { log += std::string("!") + (std::string(ex.what()).find("Failed to open import file") != std::string::npos ? "missing" : ex.what()); }
					return log;
				};
				check_eq(std::string("AB"), step(importA), "first load" + label);
				check_eq(std::string(""), step(importA), "nothing changed" + label);
				write("B.jai", "note(\"B2\");\n");
				check_eq(std::string("B2"), step(importA), "a changed child re-runs alone" + label);
				std::filesystem::remove(dir / "B.jai");
				check_eq(std::string("!missing"), step(importA), "a vanished child raises" + label);
				check_eq(std::string(""), step(importA), "then waits quietly" + label);
				write("B.jai", "note(\"B33\");\n");
				check_eq(std::string("B33"), step(importA), "a returning child re-runs" + label);
				write("X.jai", "note(\"X1\");\n");
				write("Y.jai", "note(\"Y1\");\n");
				const std::string group = "import([\"" + path("X.jai") + "\", \"" + path("Y.jai") + "\"]);";
				check_eq(std::string("X1Y1"), step(group), "a group loads" + label);
				write("X.jai", "note(\"X22\");\n");
				write("Y.jai", "int y = \"s\";\nnote(\"Y2\");\n");
				const std::string failed = step(group);
				check_true(failed.size() > 1 && failed[0] == '!' && failed.find("X22") == std::string::npos, "a failing group applies nothing: " + failed + label);
				write("Y.jai", "note(\"Y22\");\n");
				check_eq(std::string("X22Y22"), step(group), "the fixed group applies every change" + label);
				write("M.jai", "int count = 1;\nvoid hot_reload_migrate(map<string, var> previous) { count = previous[\"count\"] + 10; }\n");
				const std::string importM = "import \"" + path("M.jai") + "\";";
				step(importM);
				write("M.jai", "int count = 2;\nvoid hot_reload_migrate(map<string, var> previous) { count = previous[\"count\"] + 100; }\n");
				step(importM);
				check_eq((int64_t)101, e->execute("count").as_int(), "the reload hook sees the previous value" + label);
			}
		});
	}

	// The call-result rule for builtins that hand back a stored element (C++'s reference returns):
	// binding the result to var/auto/T copies, var&/T& and a member call reach the element; reduce's accumulator is a copy
	void stored_element_results() {
		test("stored_element_results_copy_by_value_and_alias_by_reference", [this]() {
			expect("stored element results", R"(
				class T { int v = 1; void bump() { v++; } }
				string run() {
					array<T> arr = [T(), T()];
					map<string, T> m = {"k": T()};
					var a = arr.front(); a.v = 2;
					string out = `${arr[0].v}`;
					var& b = arr.front(); b.v = 3; out += `,${arr[0].v}`;
					arr.back().bump(); out += `,${arr[1].v}`;
					auto c = arr.last(); c.v = 9; out += `,${arr[1].v}`;
					T d = arr.first(); d.v = 9; out += `,${arr[0].v}`;
					var e = m.get("k"); e.v = 5; out += `|${m["k"].v}`;
					var& f = m.get("k"); f.v = 6; out += `,${m["k"].v}`;
					m.get("k").bump(); out += `,${m["k"].v}`;
					T def = T(); var g = m.get("none", def); g.v = 8; out += `,${def.v}`;
					var acc = arr.reduce((var& x, var y) => { x.v += y.v; return x; });
					out += `|${arr[0].v},${acc.v}`;
					T init = T();
					var acc2 = arr.reduce((var& x, var y) => { x.v += y.v; return x; }, init);
					out += `,${init.v},${acc2.v}`;
					array<array<int>> nested = [[1], [2]];
					var n = nested.front(); n.push(5); out += `|${nested[0].size()}`;
					var& n2 = nested.front(); n2.push(5); out += `,${nested[0].size()}`;
					array<int> ints = [4, 5]; var i = ints.back(); i = 9; out += `,${ints[1]}`;
					return out;
				}
				run();
			)", "1,3,2,2,3|1,6,7,1|3,5,1,6|1,2,5", true);
		});
	}

	// An engine frees what it made: the conversion registry's standard converters reached it through a captured shared_ptr
	// (every engine's registry and the host classes its converters name lived forever), and a script class's methods and a
	// closure held the program environment that held them
	// A frame that shared local cells with [&] lambdas checks those cells at its exit; a cycle of
	// cells and the closures they hold that nothing else reaches is cleared (its destructors run at the frame exit), an
	// escaped closure keeps everything alive and working, and nothing still reachable is destroyed
	void cell_cycles() {
		const std::string counter = "int dead = 0;\nclass D { int v = 1; ~D() { dead++; } }\n";
		test("a_recursive_lambda_over_its_own_cell_dies_with_its_frame", [this, counter]() {
			expect("self cycle", counter + R"(
				function f() -> int { var d = D(); var g; g = [&](int n) -> int { return n < 1 ? (d != null ? 0 : -1) : g(n - 1) + 1; }; return g(3); }
				int r = f(); int one = dead; f(); int two = dead;
				to_string(r) + " " + to_string(one) + " " + to_string(two)
			)", "3 1 2");
		});
		test("lambdas_capturing_each_others_cells_die_with_their_frame", [this, counter]() {
			expect("mutual cycle", counter + R"(
				function f() -> int {
					var d = D(); var a; var b;
					a = [&](int n) -> int { return n < 1 ? (d != null ? 0 : -1) : b(n - 1) + 1; };
					b = [&](int n) -> int { return n < 1 ? 0 : a(n - 1) + 1; };
					return a(5);
				}
				int r = f(); int one = dead; f();
				to_string(r) + " " + to_string(one) + " " + to_string(dead)
			)", "5 1 2");
		});
		test("a_nested_lambda_sharing_the_outer_cells_dies_with_the_frame", [this, counter]() {
			expect("nested", counter + R"(
				function f() -> int {
					var d = D(); var g;
					g = [&](int n) -> int { auto inner = [&]() -> int { return n < 1 ? (d != null ? 0 : -1) : g(n - 1) + 1; }; return inner(); };
					return g(4);
				}
				int r = f(); int one = dead; f();
				to_string(r) + " " + to_string(one) + " " + to_string(dead)
			)", "4 1 2");
		});
		test("an_escaped_closure_keeps_its_cycle_alive_and_working", [this, counter]() {
			expect("escaped", counter + R"(
				function make() -> var { var d = D(); int count = 0; var g; g = [&]() -> int { count++; return (d != null && g != null) ? count : -1; }; return g; }
				var h = make();
				int a = h(); int b = h(); int alive = dead;
				array<var> kept = [];
				function stash() { var d = D(); var g; g = [&]() -> int { return d != null && g != null ? 7 : -1; }; kept.push(g); }
				stash(); stash();
				int stashed = dead;
				int c = kept[0]() + kept[1]();
				to_string(a) + " " + to_string(b) + " " + to_string(alive) + " " + to_string(stashed) + " " + to_string(c) + " " + to_string(h())
			)", "1 2 0 0 14 3");
		});
		test("dropped_escaped_cycles_are_reclaimed_as_the_deferred_list_grows", [this, counter]() {
			expect("deferred sweep", counter + R"(
				function make() -> var { var d = D(); var g; g = [&]() -> int { return d != null && g != null ? 1 : 0; }; return g; }
				int sum = 0;
				for (int i = 0; i < 300; i++) { var h = make(); sum += h(); }
				to_string(sum) + " " + (dead >= 200 ? "reclaimed" : "kept " + to_string(dead))
			)", "300 reclaimed");
		});
		test("a_loop_iteration_drops_the_cycle_it_shared", [this, counter]() {
			expect("loop", counter + R"(
				function f() -> int {
					int ran = 0;
					for (int i = 0; i < 5; i++) { var d = D(); var g; g = [&]() -> int { return d != null && g != null ? 1 : 0; }; ran += g(); }
					return ran * 100 + dead;
				}
				int r = f();
				to_string(r) + " " + to_string(dead)
			)", "505 5");
		});
		test("a_coroutine_frame_drops_its_cycle_when_it_finishes_or_dies", [this, counter]() {
			expect("coroutine", counter + R"(
				coroutine gen() -> int { var d = D(); var g; g = [&](int n) -> int { return n < 1 ? (d != null ? 0 : -1) : g(n - 1) + 1; }; yield g(2); yield g(3); }
				int total = 0;
				for (auto v : gen()) { total += v; }
				int finished = dead;
				function abandon() -> int { auto c = gen(); return c.next(); }
				int first = abandon();
				to_string(total) + " " + to_string(finished) + " " + to_string(first) + " " + to_string(dead)
			)", "5 1 2 2");
		});
		test("a_reference_argument_a_callee_captures_dies_with_the_callers_frame", [this, counter]() {
			expect("boxed on demand", counter + R"(
				function set(var& r, var& keep) { r = [&](int n) -> int { return n < 1 ? (keep != null ? 0 : -1) : r(n - 1) + 1; }; }
				function f() -> int { var d = D(); var g; set(g, d); return g(3); }
				int total = 0;
				string after = "";
				for (int i = 0; i < 10; i++) { total += f(); after = after + to_string(dead) + " "; }
				to_string(total) + ": " + after
			)", "30: 1 2 3 4 5 6 7 8 9 10 ");
		});
		test("a_value_still_reachable_from_outside_the_cycle_survives_it", [this, counter]() {
			expect("reachable", counter + R"(
				var outside;
				function f() -> int { var d = D(); var g; g = [&]() -> int { return d != null && g != null ? 1 : 0; }; outside = d; return g(); }
				int r = f();
				to_string(r) + " " + to_string(dead) + " " + to_string(outside.v)
			)", "1 1 1");
		});
		test("a_cycle_an_escaped_closure_kept_dies_with_its_engine", [this]() {
			for (const auto& c : robustness::configs()) {
				std::weak_ptr<robustness_probe_host> token;
				{
					auto e = robustness::make(c);
					dynamic_binder<robustness_probe_host>(*e, "RobustnessProbeHost").method("get", &robustness_probe_host::get).build();
					e->add_function("token", [&token]() { auto t = std::make_shared<robustness_probe_host>(); token = t; return t; });
					e->execute(R"(
						function make() -> var { var t = token(); var g; g = [&]() -> int { return t.get() + (g != null ? 1 : 0); }; return g; }
						var h = make();
						int r = h();
						h = null;
					)");
					check_eq((int64_t)1, e->execute("r").as_int(), std::string("ran [") + c.label + "]");
				}
				check_true(token.expired(), std::string("the cycle's host value freed with the engine [") + c.label + "]");
			}
		});
		test("a_cycle_in_a_frame_a_builtin_or_host_entered_dies_with_that_frame", [this, counter]() {
			expect("native entries", counter + R"(
				function f(int k) -> int { var d = D(); var g; g = [&](int n) -> int { return n < 1 ? (d != null ? k : -1) : g(n - 1); }; return g(2); }
				class M { int m(int k) { var d = D(); var g; g = [&](int n) -> int { return n < 1 ? (d != null ? k : -1) : g(n - 1); }; return g(2); } }
				string log = "";
				[1, 2].for_each([](int k) { var d = D(); var g; g = [&](int n) -> int { return n < 1 ? (d != null ? k : -1) : g(n - 1); }; g(2); });
				log += to_string(dead);
				[3].for_each(f);
				log += " " + to_string(dead);
				array<int> s = [3, 1, 2];
				s.sort([](int a, int b) -> bool { var d = D(); var g; g = [&](int n) -> bool { return n < 1 ? (d != null && a < b) : g(n - 1); }; return g(1); });
				log += " " + to_string(dead > 3) + to_string(s[0]);
				int before = dead;
				var m = M();
				[4].for_each(reflect::bind(m, "m"));
				log += " " + to_string(dead - before);
				log
			)", "2 3 true1 1");
		});
	}

	void engine_teardown() {
		test("an_engine_frees_its_classes_and_registry_when_it_dies", [this]() {
			for (const auto& c : robustness::configs()) {
				std::weak_ptr<class_definition> host, binary, script;
				std::weak_ptr<conversions::conversion_registry> registry;
				{
					auto e = robustness::make(c);
					dynamic_binder<robustness_probe_host>(*e, "RobustnessProbeHost").method("get", &robustness_probe_host::get).build();
					e->execute(R"(
						class Q { int v = 1; Q() {} int twice() { return v * 2; } static int s = 3; }
						Q keep = Q();
						auto fn = [&]() { return keep.twice(); };
						int r = fn();
					)");
					host = detail::engine_access::get_class_definition(*e, "RobustnessProbeHost");
					binary = detail::engine_access::get_class_definition(*e, "binary::reader");
					script = detail::engine_access::get_class_definition(*e, "Q");
					registry = detail::engine_access::get_conversion_registry(*e);
				}
				const std::string label = std::string(" [") + c.label + "]";
				check_true(host.expired(), "host class freed" + label);
				check_true(binary.expired(), "engine-core host class freed" + label);
				check_true(script.expired(), "script class freed" + label);
				check_true(registry.expired(), "conversion registry freed" + label);
			}
		});
		// The VM's call sites pin the callee they cached: a recursive function's body pinned the function itself, so the
		// function, its chunk and everything the chunk's sites pinned (here a host function's state) outlived the engine
		test("a_recursive_function_dies_with_its_engine", [this]() {
			for (const auto& c : robustness::configs()) {
				std::weak_ptr<int> state;
				{
					auto token = std::make_shared<int>(0);
					state = token;
					auto e = robustness::make(c);
					e->add_function("touch", [token]() -> script_int { return ++*token; });
					token.reset();
					e->execute("function rec(int n) -> int { touch(); return n <= 0 ? 0 : 1 + rec(n - 1); } int r = rec(4);");
					check_eq((int64_t)4, e->execute("r").as_int(), std::string("ran [") + c.label + "]");
				}
				check_true(state.expired(), std::string("the host function's state freed [") + c.label + "]");
			}
		});
	}

	// docs/site/reference/api.html: sort(predicate) orders by the predicate, equal items may be reordered; a predicate that
	// answers inconsistently still leaves a permutation of the elements (never a crash or a lost element)
	void sorting() {
		test("a_predicate_writing_the_elements_it_compares_leaves_a_permutation", [this]() {
			expect("writing predicate", R"(
				class T { int v = 0; T(int k) { v = k; } }
				function run() -> string {
					array<T> la = [];
					for (int i = 0; i < 40; i++) { la.push(T(i % 9)); }
					int calls = 0;
					la.sort((a, b) => { calls++; la[calls % 40].v = (calls * 7) % 13; return a.v < b.v; });
					return `${la.size()}`;
				}
				function run2() -> string {
					array<T> la = [T(1), T(2)];
					la.sort((a, b) => { la[0].v = 7; return a.v < b.v; });
					return `${la.size()}`;
				}
				run() + "|" + run2();
			)", "40|2");
		});
		test("a_stateful_predicate_leaves_the_same_multiset", [this]() {
			expect("stateful predicate", R"(
				function run() -> string {
					array<int> xs = [];
					for (int i = 0; i < 300; i++) { xs.push((i * 37) % 17); }
					int sum = 0; for (auto x : xs) { sum += x; }
					int n = 0;
					xs.sort((a, b) => { n++; return (n * 7919) % 5 < 2; });
					int after = 0; for (auto x : xs) { after += x; }
					array<int> ys = array::sort(xs, (a, b) => { n++; return n % 3 == 0; });
					int copied = 0; for (auto y : ys) { copied += y; }
					return `${xs.size()},${(sum == after)},${ys.size()},${(sum == copied)}`;
				}
				run();
			)", "300,true,300,true");
		});
	}
};

} // namespace jai::foundry::tests

using robustness_oracle_tests = jai::foundry::tests::robustness_oracle_tests;
FOUNDRY_REGISTER(robustness_oracle_tests)
