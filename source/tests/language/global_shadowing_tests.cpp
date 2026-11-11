// A program-scope variable redeclared by a later execute shadows the earlier declaration the way a
// nested block shadows in C++: code that ran under the earlier unit keeps its binding, code that runs
// under a later unit sees the new one; the same unit (a reload, a re-run hook) writes in place;
// functions and classes replace as before. Every row runs both backends
// with proofs on and off against one expected string.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>
#include <functional>
#include <filesystem>
#include <fstream>
#include <jaiscript/testing/temporary_directory.hpp>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class global_shadowing_tests : public suite {
public:
	global_shadowing_tests() : suite("Global Shadowing") {}

	void every_backend(const std::string& label, const std::string& expected, const std::function<std::string(engine&)>& body) {
		for (bool vm : { false, true }) {
			for (bool proofs : { true, false }) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e);
				if (!proofs) { e->type_proofs(false); }
				std::string got;
				try { got = body(*e); }
				catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
				check_eq(expected, got, label + (vm ? " vm" : " interp") + (proofs ? "" : " proofs off"));
			}
		}
	}

	void forge_tests() override {
		test("code_compiled_before_a_redeclaration_keeps_its_binding_and_code_after_sees_the_new_one", [this]() {
			// the last reader is the first reader's text executed again: the same unit, resolving as of its current run
			every_backend("before and after", "1 5 5 5|7 5 5|host=9|7 5 5 9", [](engine& e) {
				e.execute("var G = 1; function f() { return G; } function set(v) { G = v; }");
				e.execute("int G = 5; function g() { return G; }");
				e.execute("function h() { return G; }");
				std::string out = e.execute("to_string(f()) + \" \" + to_string(g()) + \" \" + to_string(h()) + \" \" + to_string(G)").as_string();
				e.execute("set(7);");
				out += "|" + e.execute("to_string(f()) + \" \" + to_string(G) + \" \" + to_string(g())").as_string();
				e.execute("var G = 9;");
				auto host = detail::engine_access::get_global_environment(e)->get("G");
				out += "|host=" + std::to_string(host ? host.value().as_int() : -1);
				out += "|" + e.execute("to_string(f()) + \" \" + to_string(g()) + \" \" + to_string(h()) + \" \" + to_string(G)").as_string();
				return out;
			});
		});
		test("the_same_unit_redeclares_in_place_and_a_forward_reference_keeps_late_binding", [this]() {
			every_backend("same unit", "2 2|3 2 3", [](engine& e) {
				const std::string second = "var G = 2; function g2() { return G; }";
				e.execute("extern var G; function f() { return G; }");
				e.execute(second);
				std::string out = e.execute("to_string(f()) + \" \" + to_string(g2())").as_string();
				e.execute(second);
				e.execute("var G = 3;");
				out += "|" + e.execute("to_string(f()) + \" \" + to_string(g2()) + \" \" + to_string(G)").as_string();
				return out;
			});
		});
		test("compound_stores_and_increments_go_through_the_old_binding", [this]() {
			every_backend("compound", "11 12 5", [](engine& e) {
				e.execute("var G = 1; function bump() { G += 10; return G; } function inc() { ++G; return G; }");
				e.execute("int G = 5;");
				return e.execute("to_string(bump()) + \" \" + to_string(inc()) + \" \" + to_string(G)").as_string();
			});
		});
		test("a_running_loop_keeps_its_cell_when_a_host_callback_redeclares_the_name", [this]() {
			// the reader's run began before the callback declared its G, so the reader's own read is the old cell; a later run sees 100
			every_backend("host shadow", "4 1 3|100 3", [](engine& e) {
				engine* raw = &e;
				e.add_function("host_shadow", [raw]() { raw->execute("var G = 100;"); });
				e.execute("var G = 1;"
				          " function loop(int n) { int acc = 0; for (int i = 0; i < n; ++i) { acc = acc + G; if (i == 1) { host_shadow(); } } return acc; }"
				          " function sum(int n) { int acc = 0; for (int i = 0; i < n; ++i) { acc = acc + G; } return acc; }");
				const std::string during = e.execute("to_string(loop(4)) + \" \" + to_string(G) + \" \" + to_string(sum(3))").as_string();
				return during + "|" + e.execute("to_string(G) + \" \" + to_string(sum(3))").as_string();
			});
		});
		test("the_global_qualifier_resolves_the_unit_binding_and_the_host_reads_and_assigns_the_newest", [this]() {
			every_backend("host api", "1|5|1 9", [](engine& e) {
				e.execute("var G = 1; function f() { return ::G; }");
				e.execute("int G = 5;");
				std::string out = e.execute("to_string(f())").as_string();
				auto host = detail::engine_access::get_global_environment(e)->get("G");
				out += "|" + std::to_string(host ? host.value().as_int() : -1);
				e.add_global("G", script_value(static_cast<int64_t>(9), &e));
				out += "|" + e.execute("to_string(f()) + \" \" + to_string(G)").as_string();
				return out;
			});
		});
		test("classes_lambdas_and_coroutines_resolve_in_the_unit_that_declared_them", [this]() {
			every_backend("declaring unit", "1 1 1 1 5 1 1", [](engine& e) {
				e.execute("var G = 1; class C { var x = G; function get() { return G; } function qualified() { return ::G; } static function stat() { return G; } } var lam = function() { return G; }; coroutine co() { yield G; }");
				e.execute("int G = 5; class D { var x = G; } var c = C(); var d = D(); var h = co();");
				return e.execute("to_string(c.x) + \" \" + to_string(c.get()) + \" \" + to_string(c.qualified()) + \" \" + to_string(C::stat()) + \" \" + to_string(d.x) + \" \" + to_string(lam()) + \" \" + to_string(h.next())").as_string();
			});
		});
		test("a_reference_bound_before_the_redeclaration_pins_the_old_cell", [this]() {
			every_backend("reference", "7 7 5", [](engine& e) {
				e.execute("var G = 1; function f() { return G; }");
				e.execute("var& R = G;");
				e.execute("int G = 5;");
				return e.execute("R = 7; to_string(f()) + \" \" + to_string(R) + \" \" + to_string(G)").as_string();
			});
		});
		test("an_inline_declaration_shares_the_visible_global_or_creates_it_when_absent", [this]() {
			// An inline variable is one variable every inline declaration shares; the first to run creates it
			// (unit 0) and a later one adopts it, as it adopts a host global
			every_backend("inline", "5 5|8 8|7 7 7", [](engine& e) {
				e.execute("inline var G; G = 5; function f() { return G; }");
				std::string out = e.execute("inline var G; to_string(G) + \" \" + to_string(f())").as_string();
				e.add_global("Q", script_value(static_cast<int64_t>(8), &e));
				out += "|" + e.execute("inline int Q = 1; function q() { return Q; } to_string(Q) + \" \" + to_string(q())").as_string();
				e.execute("inline int H; H = 7;");
				auto host = detail::engine_access::get_global_environment(e)->get("H");
				out += "|" + e.execute("inline int H; to_string(H)").as_string() + " " + std::to_string(host ? host.value().as_int() : -1);
				out += " " + e.execute("function h() { return H; } h();").to_string();
				return out;
			});
		});
		test("an_inline_initializer_runs_once_and_every_type_takes_its_default", [this]() {
			every_backend("inline init", "5 5 6|0 0.0 '' 0 0 3 null|3 2", [](engine& e) {
				const std::string first = "inline int N = 5;";
				e.execute(first);
				std::string out = e.execute("to_string(N)").as_string();
				e.execute("inline int N = 9;");
				out += " " + e.execute("to_string(N)").as_string();
				e.execute("N = 6;");
				e.execute(first);
				out += " " + e.execute("to_string(N)").as_string();
				e.execute("class P { int x = 3; } inline int a; inline float b; inline string s; inline array<int> arr; inline map<string, int> m; inline P p; inline v;");
				out += "|" + e.execute("to_string(a) + \" \" + to_string(b) + \" '\" + s + \"' \" + to_string(arr.size()) + \" \" + to_string(m.size()) + \" \" + to_string(p.x) + \" \" + to_string(v)").as_string();
				e.execute("namespace cfg { inline int depth = 3; int width = 1; }");
				e.execute("namespace cfg { inline int depth = 7; int width = 2; }");
				out += "|" + e.execute("to_string(cfg::depth) + \" \" + to_string(cfg::width)").as_string();
				return out;
			});
		});
		test("inline_and_plain_declarations_of_a_name_never_mix_across_units", [this]() {
			// In either order a mix of inline and plain declarations across units raises; the texts spell the fix
			const std::string expected =
				"'G' is already defined inline; declare it 'inline G;' to share it|"
				"'N' is already defined inline; declare it 'inline int N;' to share it|"
				"'A' is already defined inline; declare it 'inline array<int> A;' to share it|"
				"'Z' is already defined inline; declare it 'inline auto Z;' to share it|"
				"'Z' is already defined inline; declare it 'inline auto Z;' to share it|"
				"'depth' is already defined inline; declare it 'inline int depth;' to share it|"
				"'P' is already defined (not inline) in <script>; declare every definition 'inline' to share it|"
				"'w' is already defined (not inline) in <script>; declare every definition 'inline' to share it|"
				"no error|5 3 0 1 3 2 1 2|6";
			every_backend("inline plain", expected, [](engine& e) {
				auto raised = [&](const std::string& src) {
					try { e.execute(src); } catch (const std::exception& ex) { std::string w = ex.what(); return w.substr(0, w.find(": Invalid operation")); }
					return std::string("no error");
				};
				e.execute("inline var G = 5; inline int N = 3; inline array<int> A; inline Z; namespace cfg { inline int depth = 1; }");
				std::string out = raised("var G = 9;") + "|" + raised("int N = 1;") + "|" + raised("array<int> A = [1];") + "|" + raised("auto Z = 2;")
					+ "|" + raised("auto [Z, W] = [1, 2];") + "|" + raised("namespace cfg { int depth = 7; }");
				e.execute("int P = 2; namespace ns2 { int w = 1; }");
				out += "|" + raised("inline int P = 4;") + "|" + raised("namespace ns2 { inline int w; }");
				e.execute("int K = 1;");
				out += "|" + raised("int K = 2;");
				out += "|" + e.execute("to_string(G) + \" \" + to_string(N) + \" \" + to_string(A.size()) + \" \" + to_string(cfg::depth) + \" \" + to_string(N) + \" \" + to_string(P) + \" \" + to_string(ns2::w) + \" \" + to_string(K)").as_string();
				const std::string rerun = "inline int R = 6; R";
				e.execute(rerun);
				out += "|" + e.execute(rerun).to_string();
				return out;
			});
		});
		test("a_same_unit_reload_flips_a_declaration_between_plain_and_inline", [this]() {
			// the edit converts the cell in place: plain to inline keeps the value (an inline declaration adopts), inline to plain follows
			// the same-unit rule (an initializer re-runs; without one the value stays unless the declared type changed); another unit
			// then sees the cell's current kind
			const auto dir = jai::foundry::test_temp_directory() / "inline_flip";
			std::filesystem::create_directories(dir);
			const std::string file = (dir / "flip.jai").string();
			every_backend("flip", "4 4 55 55 55 55 0.0 conflict flip.jai|1 1 3", [&](engine& e) {
				auto run = [&](const std::string& text) { { std::ofstream(file, std::ios::trunc) << text; } return e.execute_file(file).to_string(); };
				std::string out = run("int F = 4; F");
				out += " " + run("inline int F = 9; F");
				out += " " + run("int F = 55; F");
				out += " " + run("inline int F = 1; F");
				out += " " + run("int F; F");
				out += " " + run("inline int F; F");
				out += " " + run("float F; F");
				try { e.execute("inline int F;"); out += " no error"; }
				catch (const std::exception& ex) {
					const std::string w = ex.what();
					out += w.rfind("'F' is already defined (not inline) in ", 0) == 0 && w.find("flip.jai; declare every definition 'inline' to share it") != std::string::npos ? " conflict flip.jai" : " " + w;
				}
				out += "|" + run("namespace fl { int G = 1; } fl::G");
				out += " " + run("namespace fl { inline int G = 7; } fl::G");
				out += " " + run("namespace fl { int G = 3; } fl::G");
				return out;
			});
			std::filesystem::remove_all(dir);
		});
		test("a_plain_extern_declares_nothing_and_raises_until_something_defines_it", [this]() {
			const std::string undefined = "'G' is declared extern but never defined (an 'inline' declaration creates it)";
			std::string every_use;
			for (int i = 0; i < 7; ++i) { every_use += undefined + "|"; }
			every_backend("extern", every_use + "4 4|3|11|'hf' is declared extern but never defined", [&](engine& e) {
				e.execute("extern int G; function f() { return G; } function set(int v) { G = v; } function bump() { G += 1; } function inc() { ++G; }"
				          " function alias() { int& r = G; return r; } function takes(int& x) { x = 1; } function pass() { takes(G); }"
				          " function cap() { var l = [G]() { return G; }; return l(); }");
				auto caught = [&](const std::string& call) {
					return e.execute("var t = \"\"; try { " + call + "; } catch (ex) { t = ex; } t").as_string();
				};
				std::string out;
				for (const char* call : { "f()", "set(3)", "bump()", "inc()", "alias()", "pass()", "cap()" }) { out += caught(call) + "|"; }
				out.pop_back();
				e.execute("int G = 4;");
				out += "|" + e.execute("to_string(f()) + \" \" + to_string(G)").as_string();
				e.execute("int K = 2;");
				out += "|" + e.execute("extern int K; to_string(K + 1)").as_string();
				e.execute("extern int H; function h() { return H; }");
				e.add_global("H", script_value(static_cast<int64_t>(11), &e));
				out += "|" + e.execute("to_string(h())").as_string();
				out += "|" + e.execute("extern function hf; var t = \"\"; try { hf(1); } catch (ex) { t = ex; } t").as_string();
				return out;
			});
		});
		test("inline_is_a_program_scope_or_namespace_variable", [this]() {
			for (const char* src : { "function f() { inline int x; }", "{ inline int y; }", "int G = 1; inline int& r = G;",
			                         "namespace n { inline function f() { } }", "class C { inline int x; }" }) {
				auto e = jai::foundry::make_engine();
				check_throws([&]() { e->execute(src); }, src);
			}
		});
		test("a_reference_argument_resolves_in_the_callers_unit", [this]() {
			// A by-reference argument is the caller's code, so it binds the caller's unit's cell; the call enters the
			// callee's unit ahead of the binding and the binding resolves under the caller's (the sol2 precompiled BST benchmark's
			// abort: a literal and a jaibite of the same text both declaring ref_root)
			every_backend("ref args", "2 1 11 1", [](engine& e) {
				e.execute("class T { int v = 0; shared_ptr<T> left = null; shared_ptr<T> right = null; T(int x) { v = x; } }"
				          " function ins(shared_ptr<T>& n, int v) { if (n == null) { n = new T(v); return; } if (v < n.v) { ins(n.left, v); } else { ins(n.right, v); } }"
				          " function cnt(shared_ptr<T>& n) -> int { if (n == null) { return 0; } return 1 + cnt(n.left) + cnt(n.right); }"
				          " function bump(int& x) { x += 1; }"
				          " var root = null; ins(root, 8); ins(root, 4); var g = 1;"
				          " function a_cnt() { return cnt(root); } function a_g() { return g; }");
				std::string b = e.execute("var root = null; ins(root, 1); var g = 10; bump(g); to_string(cnt(root)) + \" \" + to_string(g);").as_string();
				return e.execute("to_string(a_cnt()) + \" " + b + " \" + to_string(a_g());").as_string();
			});
		});
		test("a_jaibite_of_the_same_text_is_another_unit_for_reference_arguments", [this]() {
			every_backend("jaibite refs", "2 2 2 2", [](engine& e) {
				e.execute("class T { int v = 0; shared_ptr<T> left = null; shared_ptr<T> right = null; T(int x) { v = x; } }"
				          " function ins(shared_ptr<T>& n, int v) { if (n == null) { n = new T(v); return; } if (v < n.v) { ins(n.left, v); } else { ins(n.right, v); } }"
				          " function cnt(shared_ptr<T>& n) -> int { if (n == null) { return 0; } return 1 + cnt(n.left) + cnt(n.right); }");
				const char* text = "var root = null; ins(root, 8); ins(root, 4); cnt(root);";
				const auto lit = e.execute(text).as_int();
				auto bite = e.jaibite(text);
				const auto j1 = e.execute(bite).as_int();
				const auto j2 = e.execute(bite).as_int();
				const auto lit2 = e.execute(text).as_int();
				return std::to_string(lit) + " " + std::to_string(j1) + " " + std::to_string(j2) + " " + std::to_string(lit2);
			});
		});
		test("the_first_shadow_from_a_host_callback_leaves_the_active_frame_on_its_unit", [this]() {
			// The ambient stamp is written only once a name has versions; a frame active when the first shadow appears
			// (a host callback declaring another unit's global) keeps resolving as its own unit's code
			every_backend("first shadow under a frame", "11 11 22", [](engine& e) {
				engine* raw = &e;
				e.add_function("host_declare", [raw]() { raw->execute("var g = 22;"); });
				e.execute("var g = 11; function probe() { var before = g; host_declare(); return to_string(before) + \" \" + to_string(g); }");
				const std::string inside = e.execute("probe();").as_string();
				return inside + " " + e.execute("to_string(g)").as_string();
			});
		});
		test("a_lambda_minted_before_the_first_shadow_keeps_its_functions_unit", [this]() {
			// the unit of a definition is derived from the executing frame, never from an ambient that is not yet maintained
			every_backend("stamp before shadow", "1 1", [](engine& e) {
				e.execute("var g = 1; function mk() { return function() { return g; }; }");
				e.execute("var f = mk();");
				e.execute("var g = 2;");
				return e.execute("to_string(f()) + \" \" + to_string(mk()());").as_string();
			});
		});
		test("a_definition_made_by_a_function_called_from_another_unit_is_the_functions_units", [this]() {
			// a lambda, a nested function and a nested coroutine minted while another unit's top-level code is the caller and no
			// name has versions yet carry the function's unit (derived from its frame), not the caller's run: the caller declared g
			// and would read its own 2, the function's unit never declared it and reads the newest
			every_backend("nested definitions", "3 3 3", [](engine& e) {
				e.execute("[[warn_mode]] function mk() { return function() { return g; }; }"   // g is declared by no unit yet
				          " function mkf() { function inner() { return g; } return inner; }"
				          " function mkc() { coroutine co() { yield g; } return co; }");
				e.execute("var g = 2; var f = mk(); var h = mkf(); var c = mkc();");
				e.execute("var g = 3;");
				return e.execute("to_string(f()) + \" \" + to_string(h()) + \" \" + to_string(c().next())").as_string();
			});
		});
		test("a_callable_minted_by_a_shared_include_keeps_its_includers_unit", [this]() {
			// Saved-function matrix: one file's declaration node runs under two includers; the callable each
			// includer saved is that includer's (a cached mint is reused only under the unit it was minted for), for a named
			// function, a lambda and a coroutine alike
			every_backend("shared include", "11 22 11 22|11 22|11 22", [](engine& e) {
				auto write_tmp = [](const std::string& name, const std::string& body) {
					std::filesystem::path p = jai::foundry::test_temp_directory() / name;
					std::ofstream(p) << body;
					return p.generic_string();
				};
				const std::string fn = write_tmp("shadow_factory.jai", "function read() { return x; } read;");
				const std::string lam = write_tmp("shadow_lambda.jai", "var r = function() { return x; }; r;");
				const std::string gen = write_tmp("shadow_gen.jai", "coroutine gen() { yield x; } gen;");
				e.execute("var x = 11; var fa = include \"" + fn + "\"; var la = include \"" + lam + "\"; var ga = include \"" + gen + "\";");
				e.execute("var x = 22; var fb = include \"" + fn + "\"; var lb = include \"" + lam + "\"; var gb = include \"" + gen + "\";");
				return e.execute("to_string(fa()) + \" \" + to_string(fb()) + \" \" + to_string(fa()) + \" \" + to_string(fb()) + \"|\""
				                 " + to_string(la()) + \" \" + to_string(lb()) + \"|\" + to_string(ga().next()) + \" \" + to_string(gb().next())").as_string();
			});
		});
		test("a_block_is_a_scope_inside_a_unit", [this]() {
			// Scope is also identified by {}. A block declaration shadows for the block and vanishes after it, at
			// program scope and in a function; a callee still sees the outer binding; a block in another unit shadows only inside
			// the block and is not that unit's declaration of the name
			every_backend("blocks", "2 1 2 1 1 5 3 1 20 1", [](engine& e) {
				e.execute("var G = 1; string out = \"\";"
				          " { var G = 2; out = out + to_string(G); } out = out + \" \" + to_string(G);"
				          " function g() { return G; }"
				          " function f() { var x = 1; { var x = 2; out = out + \" \" + to_string(x); } out = out + \" \" + to_string(x);"
				          "   { var G = 5; out = out + \" \" + to_string(g()) + \" \" + to_string(G); } }"
				          " f();"
				          " if (true) { var G = 3; out = out + \" \" + to_string(G); } out = out + \" \" + to_string(G);");
				return e.execute("{ var G = 20; out = out + \" \" + to_string(G); } out = out + \" \" + to_string(G); out;").as_string();
			});
		});
		test("a_file_reloaded_at_its_path_is_the_same_unit_and_writes_in_place", [this]() {
			every_backend("reload", "102 5", [](engine& e) {
				e.disable(jai::filesystem);
				const std::string path = "D:/virtual/shadowing/unit_a.jai";
				e.execute_file_source(path, "var G = 1; function f() { return G; }");
				e.execute("int G = 5;");
				e.execute_file_source(path, "var G = 2; function f() { return G + 100; }");
				return e.execute("to_string(f()) + \" \" + to_string(G)").as_string();
			});
		});
	}
};

} // namespace jai::foundry::tests

using global_shadowing_tests = jai::foundry::tests::global_shadowing_tests;
FOUNDRY_REGISTER(global_shadowing_tests)
