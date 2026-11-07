// Strict-rule cases. Register accepted cases individually.
// Accepted rules are active; the two deferred proposals remain unregistered.
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/jaibite.hpp>
#include <jaiscript/detail/ast_serializer.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"

namespace jai::foundry::tests {

class strict_gaps_pending_tests : public suite {
public:
	strict_gaps_pending_tests() : suite("Strict Rule Acceptance") {}

	void forge_tests() override {
		rules_456_and_controls();
		element_rules();
		element_rule_boundaries();
		// deferred_proposals();            // not registered
	}

private:

	// the bytes of a bite no checker has seen (parsed and serialized directly), for a loader to check
	static std::vector<uint8_t> unstamped_bite_bytes(engine& a_engine, const std::string& a_source) {
		const auto templates = detail::engine_access::get_registered_template_types(a_engine);
		lexer lex(a_source, detail::engine_access::get_symbolizer(a_engine));
		auto tokens = lex.tokenize();
		parser syntax(tokens, detail::engine_access::get_symbolizer(a_engine), &a_engine, templates);
		auto declarations = syntax.parse();
		check_true(bool(declarations), "fixture must parse: " + a_source);
		return detail::serialize_jaibite(declarations.value(), *detail::engine_access::get_symbolizer(a_engine), a_engine.registration_fingerprint());
	}

	void rules_456_and_controls() {
		test("typed_slot_controls_reject_known_mismatches_and_leave_dynamic_values_alone", [this]() {
			const std::pair<const char*, bool> cases[] = {
				{R"jai(bool b = [1]; b;)jai", false},
				{R"jai(string s = 42; s;)jai", true},
				{R"jai(shared_ptr<int> p = "bad"; p;)jai", true},
				{R"jai(weak_ptr<int> p = "bad"; p;)jai", true},
				{R"jai(string s = "bad"; int& r = s; r;)jai", true},
				{R"jai(var v = 1; v = "bad"; v;)jai", false},
				{R"jai(array<int> a = [1]; var v = a; string x = v[0]; x;)jai", false},
				{R"jai(array<int> a = [1]; var v = a; v[0] = "bad"; v[0];)jai", false},
				{R"jai(auto x = null; x = 1; x = "bad"; x;)jai", false},
				{R"jai(int x = "bad"; x;)jai", true},
				{R"jai(float x = "bad"; x;)jai", true},
				{R"jai(char c = 65; c;)jai", true},
				{R"jai(class C {} class D {} C c = D(); c;)jai", true},
				{R"jai(auto x = 1; x = "bad"; x;)jai", true},
				{R"jai(array<int> a = 1; a;)jai", true},
				{R"jai(array<int> a = [3.9]; a[0];)jai", false},
				{R"jai(function f = 42; f;)jai", true},
				{R"jai(string s = "bad"; int& r = s; r = 1; s;)jai", true},
				{R"jai(array<int> a = [1]; var v = a; v[0] = "bad"; a[0];)jai", false},
				{R"jai(array<int> a = [1]; var v = a; v[0] = "bad"; array<int> b = v; b;)jai", false},
				{R"jai(array<array<int>> a = [[1]]; var v = a; v[0][0] = "bad"; v;)jai", false},
			};
			check_each(cases, [&](const auto& c) {
				auto engine = make_engine();
				check_eq(c.second, engine->check(c.first).has_errors());
			});
		});

		test("rules_456_known_type_rejections", [this]() {
			const char* sources[] = {
				"class A {} class B {} shared_ptr<B> h = new B(); weak_ptr<A> w = h;",
				"class A {} A a = A(); weak_ptr<A> w = a;",
				"shared_ptr<float> h = new float(1.0); weak_ptr<int> w = h;",
				"int i = 1; float& f = i;",
				"int i = 1; string& s = i;",
				"string s = \"s\"; string& a = s; int& b = a;",
				"class A {} class B { B(A a) {} } A a = A(); B& b = a;",
				"array<int> a = [1]; array<float>& b = a;",
				"function f = null; int i = 4; f = i;",
				"function f; f = 42;",
				"class C { function f = 42; }",
				"class C { function f = null; } C c = C(); c.f = 42;",
				"namespace N { function f = 42; }",
				"class C { static function f = null; } C::f = 42;",
				"class C { function f = null; void bad() { f = 42; } }",
				"class A {} function f = A();",
				"void g(function cb) {} g(42);"
			};
			for (const char* source : sources) {
				auto eng = make_engine();
				check_report report;
				try { report = eng->check(source); }
				catch (const std::exception& error) { throw test_failure(std::string(source) + "\n" + error.what()); }
				check_true(report.has_errors(), source);
				bool rejected = false;
				try { eng->execute(source); }
				catch (const static_check_error&) { rejected = true; }
				check_true(rejected, "strict checking rejects before execution");
			}
		});

		test("rules_456_valid_and_dynamic_sources", [this]() {
			const char* sources[] = {
				"class A {} class B : A {} shared_ptr<B> h = new B(); weak_ptr<A> w = h; weak_ptr<A> copy = w;",
				"weak_ptr<int> w = null;",
				"class A {} weak_ptr<A> w = new A();",
				"class A {} weak_ptr<A> w = shared_ptr<A>();",
				"int i = 1; int& r = i; int& s = r;",
				"class A {} class B : A {} B b = B(); A& a = b;",
				"array<int> a = [1]; array<int>& b = a;",
				"var v = \"dynamic\"; int& r = v;",
				"var v = \"dynamic\"; weak_ptr<int> w = v;",
				"function f = null; f = []() { return 1; };",
				"int original() { return 1; } function f = original;",
				"class C { int value() { return 1; } } C c = C(); function f = []() { return c.value(); };",   // a method is bound, never read bare
				"var v = 42; function f = v;",
				"auto f = 42; f = 43;",
				"function f; f = []() { return 1; };",
				"namespace N { function f = []() { return 1; }; }",
				"class C { function f = null; } C c = C(); c.f = []() { return 1; }; function copy = c.f;",
				"function f = [](int x) -> int { return x; }; f(\"bad\");"
			};
			for (const char* source : sources) {
				auto eng = make_engine();
				const auto report = eng->check(source);
				check_false(report.has_errors(), std::string(source) + "\n" + report.format(source));
			}
		});

		test("rules_456_warn_regions_and_unchecked_runtime", [this]() {
			for (const char* source : {
				"weak_ptr<int> w = \"bad\";",
				"string s = \"s\"; int& r = s;",
				"function f = 42;"
			}) {
				for (bool block : {false, true}) {
					auto eng = make_engine();
					const std::string unit = block
						? std::string("{ [[warn_mode]]; ") + source + " }"
						: std::string("[[warn_mode]];\n") + source;
					const auto report = eng->check(unit);
					check_false(report.has_errors(), report.format(unit));
					check_true(!report.diagnostics.empty(), "rule is downgraded, not omitted");
				}
			}
			auto eng = make_engine();
			check_eq(std::string("Cannot initialize 'r' (declared int&) with string"),
			         eng->execute("[[warn_mode]]; string s = \"s\"; var c = \"\"; try { int& r = s; } catch (e) { c = e; } c;").as_string());   // a reference must reference: the binding raises
			check_eq(std::string("Cannot initialize 'f' (declared function) with int"),
			         eng->execute("[[warn_mode]]; var c = \"\"; try { function f = 42; } catch (e) { c = e; } c;").as_string());   // a function slot takes a callable or null (std::function's rule)
			check_eq(script_int{1}, eng->execute(
				"[[warn_mode]]; class A {} class B {} shared_ptr<B> owner = new B(); weak_ptr<B> wb = owner; weak_ptr<A> wa = wb; 1;").as_int());
		});

		test("rules_456_valid_runtime_boundaries", [this]() {
			for (const char* source : {
				"int i = 1; int& r = i; r = 7; i;",
				"class A { int x = 1; } class B : A {} B b = B(); A& r = b; r.x = 7; b.x;",
				"class A { int x = 7; } class B : A {} shared_ptr<B> owner = new B(); weak_ptr<A> weak = owner; auto locked = weak.lock(); locked.x;",
				"function f = []() { return 1; }; f = null; f = []() { return 7; }; f();"
			}) {
				auto eng = make_engine();
				const auto report = eng->check(source);
				check_false(report.has_errors(), std::string(source) + "\n" + report.format(source));
				check_eq(script_int{7}, eng->execute(source).as_int(), source);
			}
		});

		test("rules_456_member_assignment_result_types", [this]() {
			for (const char* source : {
				"class C { int n = 0; } C c = C(); auto x = (c.n = 3.9); int& r = x; r == 3;",
				"class C { int n = 1; } C c = C(); auto x = (c.n += 2.9); int& r = x; r == 3;",
				"class C { static int n = 1; } auto x = (C::n += 2.9); int& r = x; r == 3;",
				"class C { static int n = 0; } auto x = (C::n = 3.9); int& r = x; r == 3;",
				"int n = 0; auto x = (n = 3.9); int& r = x; r == 3;",
				"int n = 1; auto x = (n += 2.9); int& r = x; r == 3;",
				"class C { int n = 1; bool test() { auto x = (n += 2.9); int& r = x; return r == 3; } } C c = C(); c.test();",
				"class C { int n = 1; int _get_x() { return n; } void _set_x(int v) { n = v; } } C c = C(); auto x = (c.x += 2.9); int& r = x; r == 3;",
				"class C { string s = \"\"; } C c = C(); map<string,int> m = {{\"a\",1}}; for (auto kv : m) { auto x = (c.s = kv.first); string& r = x; r = \"b\"; return kv.first == \"a\" && r == \"b\"; } false;"
			}) {
				auto eng = make_engine();
				jai::stdlib::register_all(*eng);
				const auto report = eng->check(source);
				check_false(report.has_errors(), std::string(source) + "\n" + report.format(source));
				check_true(eng->execute(source).as<bool>(), source);
			}
		});

		test("rules_456_saved_declarations_recheck", [this]() {
			for (const char* source : {
				"function f = 42;",
				"class C { function f = 42; }",
				"class C { function f = null; } C c = C(); c.f = 42;",
				"namespace N { function f = 42; }",
				"class C { static function f = null; } C::f = 42;"
			}) {
				auto author = make_engine();
				auto bytes = unstamped_bite_bytes(*author, source);
				auto loader = make_engine();
				bool rejected = false;
				try { auto bite = loader->jaibite_load_bytes(bytes); }
				catch (const static_check_error&) { rejected = true; }
				check_true(rejected, source);
			}
		});

	}

	void element_rules() {
		test("element_reads_and_writes_report_known_type_mismatches", [this]() {
			const char* const sources[] = {
				{R"jai(array<string> a = ["bad"]; int x = a[0]; x;)jai"},
				{R"jai(array<string> a = ["bad"]; float x = a[0]; x;)jai"},
				{R"jai(array<int> a = [65]; char c = a[0]; c;)jai"},
				{R"jai(array<int> a = ["bad"]; a;)jai"},
				{R"jai(map<string, int> m = {{"key", "bad"}}; m;)jai"},
				{R"jai(class C {} class D {} array<D> a = [D()]; C c = a[0]; c;)jai"},
				{R"jai(auto x = 1; array<string> a = ["bad"]; x = a[0]; x;)jai"},
				{R"jai(array<array<int>> a = [[1, "bad"]]; a;)jai"},
				{R"jai(map<string, array<int>> m = {{"key", [1, "bad"]}}; m;)jai"},
				{R"jai(map<int, array<int>> m = {{"bad", [1]}}; m;)jai"},
				{R"jai(array<array<int>> a = [[1]]; a.push(["bad"]); a;)jai"},
				{R"jai(array<array<int>> a = [[1]]; a[0][0] = "bad"; a;)jai"},
				{R"jai(array<int> a = [1e30]; a;)jai"},
				{R"jai(array<string> a = ["bad"]; int f() { return a[0]; } f();)jai"},
				{R"jai(array<string> a = ["bad"]; int f(int x) { return x; } f(a[0]);)jai"},
				{R"jai(class C { int x = 0; } array<string> a = ["bad"]; C c = C(); c.x = a[0]; c.x;)jai"},
			};
			check_each(sources, [&](const auto& c) {
				auto engine = make_engine();
				check_true(engine->check(c).has_errors());
			});
		});

	}

	void element_rule_boundaries() {
		test("rules_123_known_element_mismatches_reject_before_execution", [this]() {
			const char* const sources[] = {
				{R"jai(array<int> a = [1]; a.push("bad");)jai"},
				{R"jai(array<int> a = [1]; a[0] = "bad";)jai"},
				{R"jai(map<int,string> m = {{"bad","ok"}};)jai"},
				{R"jai(array<string> a = [1];)jai"},
				{R"jai(array<char> a = [65];)jai"},
				{R"jai(array<array<int>> a = [[1]]; a[0] = ["bad"];)jai"},
				{R"jai(map<string,array<int>> m = {{"a",[1]}}; m["a"][0] = "bad";)jai"},
				{R"jai(array<map<string,int>> a = [{{"k","bad"}}];)jai"},
				{R"jai(void take(array<int> a) {} take(["bad"]);)jai"},
				{R"jai(array<int> give() { return ["bad"]; })jai"},
				{R"jai(class C { array<int> a = ["bad"]; })jai"},
				{R"jai(class C { array<int> a = [1]; } C c = C(); c.a = ["bad"];)jai"},
				{R"jai(map<string,string> m = {{"k","bad"}}; int x = m["k"];)jai"},
				{R"jai(void f(array<string>& a) { int x = a[0]; })jai"},
				{R"jai(array<string> a = ["bad"]; array<string>& b = a; int x = b[0];)jai"},
			};
			check_each(sources, [&](const auto& c) {
				const std::string source = c;
				auto eng = make_engine();
				const auto report = eng->check(source);
				check_true(report.has_errors(), source);
				bool rejected = false;
				try { eng->execute(source); }
				catch (const static_check_error&) { rejected = true; }
				check_true(rejected, "known mismatch rejects before execution");
			});
		});

		test("rules_123_floats_beyond_int_range_reject_before_execution", [this]() {
			const char* const sources[] = {
				{R"jai(int x = 1e30;)jai"},
				{R"jai(int x = -1e30;)jai"},
				{R"jai(int x = 9.223372036854776e18;)jai"},
				{R"jai(int x = -9.223372036854778e18;)jai"},
				{R"jai(int x = 0; x = 1e30;)jai"},
				{R"jai(int f() { return 1e30; })jai"},
				{R"jai(void f(int x) {} f(1e30);)jai"},
				{R"jai(class C { int x = 1e30; })jai"},
				{R"jai(array<int> a = [1]; a.push(1e30);)jai"},
				{R"jai(array<int> a = [1]; a[0] = 1e30;)jai"},
				{R"jai(array<array<int>> a = [[1e30]];)jai"},
				{R"jai(map<int,string> m = {{1e30,"ok"}};)jai"},
			};
			check_each(sources, [&](const auto& c) {
				const std::string source = c;
				auto eng = make_engine();
				const auto report = eng->check(source);
				check_true(report.has_errors(), source);
				bool rejected = false;
				try { eng->execute(source); }
				catch (const static_check_error&) { rejected = true; }
				check_true(rejected, "known mismatch rejects before execution");
			});
		});

		test("rules_123_dynamic_sources_and_valid_conversions_check_clean", [this]() {
			const char* const sources[] = {
				{R"jai(var a = ["bad"]; int x = a[0];)jai"},
				{R"jai(array<var> a = ["bad"]; int x = a[0];)jai"},
				{R"jai(void f(var& a) { int x = a[0]; })jai"},
				{R"jai(var x = "bad"; array<int> a = [x];)jai"},
				{R"jai(var x = "bad"; array<int> a = [1]; a.push(x);)jai"},
				{R"jai(var x = "bad"; array<int> a = [1]; a[0] = x;)jai"},
				{R"jai(array<string> a = ["ok"]; string x = a[0];)jai"},
				{R"jai(array<int> a = [1]; float x = a[0]; bool b = a[0];)jai"},
				{R"jai(array<int> a = [3.9]; array<float> b = [1];)jai"},
				{R"jai(array<array<int>> a = [[1, 3.9]];)jai"},
				{R"jai(map<string,array<int>> m = {{"a",[1,3.9]}};)jai"},
				{R"jai(array<var> a = [1,"s",true];)jai"},
				{R"jai(array<int> a = []; map<string,int> m = {};)jai"},
				{R"jai(float x = 1e30; int y = x;)jai"},
				{R"jai(float big() { return 1e30; } int x = big();)jai"},
				{R"jai(float huge = 1e30; array<int> a = [huge];)jai"},
				{R"jai(float huge = 1e30; array<int> a = [1]; a.push(huge);)jai"},
				{R"jai(int x = -9.223372036854776e18;)jai"},
				{R"jai(int x = 9.223372036854775e18;)jai"},
				{R"jai(int x = 3.9;)jai"},
				{R"jai(int x = cast<int>("3");)jai"},
				{R"jai(class A {} class B : A {} array<A> a = [B()];)jai"},
				{R"jai(class A {} class B : A {} array<A> a = [new B()];)jai"},
				{R"jai(array<int> a = [0]; auto x = (a[0] = 3.9); int& r = x;)jai"},
				{R"jai(map<string,int> m = {{"a",0}}; auto x = (m["a"] = 3.9); int& r = x;)jai"},
				{R"jai(array<string> a = ["x"]; auto x = (a[0] += 1); string& r = x;)jai"},
				{R"jai(auto huge = 1e30; int x = huge;)jai"},
			};
			check_each(sources, [&](const auto& c) {
				const std::string source = c;
				auto eng = make_engine();
				const auto report = eng->check(source);
				check_false(report.has_errors(), source + "\n" + report.format(source));
			});
		});

		test("rules_123_warn_regions", [this]() {
			for (const char* source : {
				"array<int> a = [\"bad\"];",
				"array<array<int>> a = [[\"bad\"]];",
				"array<int> a = [1]; a.push(\"bad\");",
				"array<array<int>> a = [[1]]; a[0][0] = \"bad\";",
				"array<string> a = [\"bad\"]; int x = a[0];",
				"int x = 1e30;"
			}) {
				for (bool block : {false, true}) {
					auto eng = make_engine();
					const std::string unit = block
						? std::string("{ [[warn_mode]]; ") + source + " }"
						: std::string("[[warn_mode]];\n") + source;
					const auto report = eng->check(unit);
					check_false(report.has_errors(), report.format(unit));
					check_true(!report.diagnostics.empty(), "warning is retained");
				}
			}
		});

		test("rules_123_nested_diagnostic_path", [this]() {
			auto eng = make_engine();
			const std::string source = "map<string,array<int>> m = {{\"key\", [1, \"bad\"]}};";
			const auto report = eng->check(source);
			const auto text = report.format(source);
			check_true(report.has_errors());
			check_true(text.find("map value") != std::string::npos, text);
			check_true(text.find("array element") != std::string::npos, text);
			check_true(text.find("string") != std::string::npos, text);
		});

		test("rules_123_saved_literals_recheck", [this]() {
			for (const char* source : {
				"array<int> a = [\"bad\"];",
				"array<array<int>> a = [[\"bad\"]];",
				"array<int> a = [1e30];"
			}) {
				auto author = make_engine();
				auto bytes = unstamped_bite_bytes(*author, source);
				auto loader = make_engine();
				bool rejected = false;
				try { auto bite = loader->jaibite_load_bytes(bytes); }
				catch (const static_check_error&) { rejected = true; }
				check_true(rejected, source);
			}
		});

		test("rules_123_assignment_expression_results", [this]() {
			for (const char* source : {
				"array<int> a = [0]; auto x = (a[0] = 3.9); int& r = x; r == 3;",
				"map<string,int> m = {{\"a\",0}}; auto x = (m[\"a\"] = 3.9); int& r = x; r == 3;",
				"array<string> a = [\"x\"]; auto x = (a[0] += \"1\"); string& r = x; r == \"x1\";",
				"int x = -9.223372036854776e18; x == (-9223372036854775807 - 1);"
			}) {
				auto eng = make_engine();
				const auto report = eng->check(source);
				check_false(report.has_errors(), std::string(source) + "\n" + report.format(source));
				check_true(eng->execute(source).as<bool>(), source);
			}
		});

		test("rules_123_nullable_handle_elements", [this]() {
			for (const char* source : {
				"class C { int n = 4; } array<shared_ptr<C>> a = [null, C()]; a.push(null); a[1] = null; a[0] == null && a[1] == null && a.size() == 3;",
				"class C {} array<weak_ptr<C>> a = [null]; a.push(null); a[0] = null; a.size() == 2 && a[0] == null;",
				"class C { int n = 4; } map<string,shared_ptr<C>> m = {{\"k\",null}}; m[\"k\"] = C(); int n = m[\"k\"].n; m[\"k\"] = null; n == 4 && m[\"k\"] == null;",
				"class C {} map<string,weak_ptr<C>> m = {{\"k\",null}}; m[\"k\"] = null; m[\"k\"] == null;",
				"class C { int n = 4; } class D : C {} array<shared_ptr<D>> ds = [D()]; array<shared_ptr<C>> cs = ds; cs.push(D()); cs[0].n + cs[1].n == 8;"
			}) {
				auto eng = make_engine();
				const auto report = eng->check(source);
				check_false(report.has_errors(), std::string(source) + "\n" + report.format(source));
				check_true(eng->execute(source).as<bool>(), source);
			}
		});

		test("rules_123_handle_element_mismatches", [this]() {
			for (const char* source : {
				"class C {} class D {} array<shared_ptr<C>> a = [D()];",
				"class C {} class D {} shared_ptr<D> d = D(); array<shared_ptr<C>> a = []; a.push(d);",
				"class C {} class D {} array<shared_ptr<C>> a = [null]; a[0] = D();",
				"class C {} class D {} map<string,shared_ptr<C>> m = {{\"k\",D()}};",
				"class C {} array<C> a = [null];",
				"array<int> a = [null];",
				"array<string> a = [null];",
				"class C {} array<weak_ptr<C>> a = [C()];",
				"class C {} class D {} shared_ptr<D> d = D(); array<weak_ptr<C>> a = []; a.push(d);"
			}) {
				auto eng = make_engine();
				check_true(eng->check(source).has_errors(), source);
				bool rejected = false;
				try { eng->execute(source); }
				catch (const static_check_error&) { rejected = true; }
				check_true(rejected, source);
			}
		});

		test("rules_123_handle_types_remain_distinct", [this]() {
			auto eng = make_engine();
			const std::string source = "class C {} array<C> values = []; array<shared_ptr<C>> handles = []; handles.push(null); values.push(null);";
			const auto report = eng->check(source);
			check_eq(size_t{1}, report.error_count(), report.format(source));
			check_true(report.format(source).find("array<C>") != std::string::npos);
		});

		test("rules_123_bool_int_boundary_conversions", [this]() {
			for (const char* source : {
				"array<int> a = []; a.push(true); a.push(false); a[1] = true; a[0] + a[1] == 2;",
				"array<bool> a = []; a.push(0); a.push(-2); a[0] = 7; a[0] && a[1];",
				"array<bool> a = [false, true]; array<int> b = a; b[0] == 0 && b[1] == 1;",
				"array<int> a = [0, -2]; array<bool> b = a; !b[0] && b[1];",
				"array<array<int>> a = [[true]]; a.push([false]); a[1][0] = true; a[0][0] + a[1][0] == 2;",
				"map<string,bool> m = {{\"a\",0}}; m[\"a\"] = 2; m[\"a\"];",
				"map<string,int> m = {{\"a\",false}}; m[\"a\"] = true; m[\"a\"] == 1;",
				"array<string> a = [to_string(15), cast<string>(16)]; a.push(to_string(17)); a[0] == \"15\" && a[1] == \"16\" && a[2] == \"17\";",
				"array<int> a = []; var b = true; a.push(b); array<bool> c = []; var i = 7; c.push(i); a[0] == 1 && c[0];",
				R"jai(array<bool> a = [0, 1, -2]; !a[0] && a[1] && a[2];)jai",
				R"jai(array<int> a = [true, false]; a[0] == 1 && a[1] == 0;)jai"
			}) {
				auto eng = make_engine();
				jai::stdlib::register_all(*eng);
				const auto report = eng->check(source);
				check_false(report.has_errors(), std::string(source) + "\n" + report.format(source));
				check_true(eng->execute(source).as<bool>(), source);
			}
		});

		test("rules_123_string_conversion_stays_explicit", [this]() {
			for (const char* source : {
				"array<string> a = [15];",
				"array<string> a = []; a.push(15);",
				"map<string,string> m = {{\"a\",15}};",
				"array<bool> a = [1.5];",
				"array<float> a = [true];",
				"map<int,string> m = {{true,\"bad\"}};",
				"map<bool,string> m = {{1,\"bad\"}};",
				"map<int,string> m = {{1.5,\"bad\"}};"
			}) {
				auto eng = make_engine();
				check_true(eng->check(source).has_errors(), source);
				bool rejected = false;
				try { eng->execute(source); }
				catch (const static_check_error&) { rejected = true; }
				check_true(rejected, source);
			}
		});

	}

	void deferred_proposals() {
		test("pending_reject_auto_no_initializer", [this]() {
			auto engine = make_engine();
			const std::string source = R"jai(auto x; x = 1; x = "bad"; x;)jai";
			const auto report = engine->check(source);
			check_true(report.has_errors());
		});

		test("pending_reject_function_call_argument", [this]() {
			auto engine = make_engine();
			const std::string source = R"jai(function f = [](int x) -> int { return x; }; f("bad");)jai";
			const auto report = engine->check(source);
			check_true(report.has_errors());
		});
	}
};

} // namespace jai::foundry::tests

using strict_rule_acceptance_tests = jai::foundry::tests::strict_gaps_pending_tests;
FOUNDRY_REGISTER(strict_rule_acceptance_tests)
