// Invariant attacks on the VM's fast paths (straight-line regions, grafts,
// typed rows): an event the fast path assumes away happens inside it, and the result must be the
// one the specification gives for the plain program. Expected values come from the spec, never from a backend. Every
// row runs on the interpreter and the VM, proofs on and off, passes off, and with injected cell misses when the build
// defines JAISCRIPT_MISS_INJECTION.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/debug/controller.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

namespace attack {

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

inline std::string render(const script_value& v) {
	if (v.is_string()) { return v.as_string(); }
	if (v.is_null()) { return "<null>"; }
	return "<" + v.to_string() + ">";
}

} // namespace attack

class spec_attack_tests : public suite {
public:
	spec_attack_tests() : suite("Spec Attacks") {}

	void every_config(const std::string& label, const std::string& expected, const std::function<std::string(engine&)>& body) {
		for (const auto& c : attack::configs()) {
			auto e = attack::make(c);
			std::string got;
			try { got = body(*e); }
			catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
			check_eq(expected, got, label + " [" + c.label + "]");
		}
	}

	void expect(const std::string& label, const std::string& src, const std::string& expected) {
		every_config(label, expected, [&](engine& e) { return attack::render(e.execute(src)); });
	}

	void forge_tests() override {
		exceptions_inside_fast_paths();
		coroutines_over_fast_paths();
		host_callbacks_mid_loop();
		rebinding_and_reload_mid_loop();
		containers_changing_under_a_loop();
		operators_and_compounds_in_loops();
		shifts_and_budget();
		debugger_armed_mid_run();
		parallel_bodies_with_leaves();
		removed_element_aliases();
		census_paths();
	}

private:
	// VM lowerings no other registered test executed (Clang source coverage of the whole suite): each runs its general form's semantics
	void census_paths() {
		test("a_ternary_store_with_binary_arms_lands_through_the_fused_store", [this]() {
			expect("ternary store arms", R"(
				function run() -> string {
					int a = 3; int b = 4; int x = 0; string s = "";
					for (int i = 0; i < 4; i++) { x = i % 2 == 0 ? a + b * i : a - i; s += `${x},`; }
					float f = 0.0; float h = 1.5;
					for (int k = 0; k < 2; k++) { f = k == 0 ? h * 2.0 : h - 1.0; s += `${f},`; }
					return s;
				}
				function ovf() -> string { int big = 9223372036854775807; int x = 0; string r = "no"; try { x = big > 0 ? big + 1 : 0; } catch (e) { r = e; } return `${r}|${x}`; }
				run() + ovf();
			)", "3,2,11,0,3.0,0.5,Integer overflow in '+'|0");
		});
		test("a_unary_over_a_binary_lands_in_a_temp_in_every_operand_position", [this]() {
			expect("unary temps", R"(
				function neg(int v) -> int { return v; }
				function run() -> string {
					int a = 3; int b = 4; string s = "";
					for (int i = 0; i < 2; i++) {
						int y = -(a + b) * 2 + i; s += `${y},`;
						s += `${neg(-(a * b))},`;
						int z = i == 0 ? -(a - b) : -(b * i); s += `${z},`;
					}
					return s;
				}
				run();
			)", "-14,-12,1,-13,-12,-4,");
		});
		test("an_index_chain_whose_step_runs_a_user_operator_takes_the_boxed_tail", [this]() {
			expect("boxed index tail", R"(
				class N { int v = 0; N(int x) { v = x; } function operator+(int k) -> int { return v + k; } }
				function run() -> string {
					array<int> a = [10, 20, 30, 40]; array<array<int>> g = [[1, 2], [3, 4]]; N n = N(1); string s = "";
					for (int i = 0; i < 2; i++) { s += `${a[n + i]},${g[i][n + 0]},`; }
					return s;
				}
				run();
			)", "20,2,30,4,");
		});
		test("loop_bodies_without_braces", [this]() {
			expect("braceless bodies", R"(
				function run() -> string { string s = ""; for (int i = 0; i < 3; i++) s += `${i}`; int k = 0; while (k < 3) k++; for (auto v : [7, 8]) s += `${v}`; return `${s}|${k}`; }
				run();
			)", "01278|3");
		});
		test("from_this_with_arguments_raises_its_arity_text", [this]() {
			// weak_from_this() takes no arguments
			expect("from_this arity", R"(
				class C { function f() { var w = weak_from_this(1); return 1; } }
				string r = ""; try { C().f(); } catch (e) { r = e; } r;
			)", "weak_from_this() takes no arguments");
		});
		test("destructured_names_shared_with_a_closure_live_in_the_environment", [this]() {
			expect("destructure", R"(
				function run() -> string { auto [x, y] = [10, 20]; auto f = [&]() { x += 1; }; f(); return `${x},${y}`; }
				auto [p, q] = [1, 2];
				`${run()}|${(p + q)}`;
			)", "11,20|3");
		});
	}

	// a raise inside a block leaves the general form's post-state
	void exceptions_inside_fast_paths() {
		test("a_raise_inside_a_closed_typed_loop_is_caught_in_the_same_function", [this]() {
			expect("same function", R"(
				function run() -> string {
					array<int> a = [1, 2, 3, 0, 5]; int s = 0; int at = -1; string err = "";
					try { for (int j = 0; j < 5; j++) { at = j; s += 10 / a[j]; } } catch (e) { err = e; }
					return `${s}|${at}|${to_string(err.starts_with("Division by zero"))}`;
				}
				run();
			)", "18|3|true");
		});
		test("a_raise_inside_a_closed_typed_loop_is_caught_in_a_caller_with_globals_written", [this]() {
			expect("caller", R"(
				var G = 0;
				function inner(array<int> a) -> int { int s = 0; for (int j = 0; j < a.size(); j++) { s += 100 / a[j]; G = s; } return s; }
				function outer() -> string { string err = ""; int r = -1; try { r = inner([4, 2, 0, 1]); } catch (e) { err = e; } return `${r}|${G}|${to_string(err.starts_with("Division by zero"))}`; }
				outer();
			)", "-1|75|true");
		});
		test("a_raise_inside_an_inlined_leaf_unwinds_the_loop_with_the_prior_iterations_kept", [this]() {
			expect("graft raise", R"(
				function leaf(int x) -> int { if (x == 3) { throw `leaf${x}`; } return x * 2; }
				function tleaf(int x) -> int { if (x == 4) { throw x * 10; } return x; }
				function run() -> string {
					int s = 0; int last = -1; string err = "";
					try { for (int i = 0; i < 6; i++) { s += leaf(i); last = i; } } catch (e) { err = e; }
					int t = 0; var code = null;
					try { for (int i = 0; i < 6; i++) { t += tleaf(i); } } catch (e) { code = e.value; }
					return `${s}|${last}|${err}|${t}|${code}${type_of(code)}`;
				}
				run();
			)", "6|2|leaf3|6|40int");
		});
		test("a_try_inside_a_loop_body_catches_per_iteration", [this]() {
			expect("try in loop", R"(
				function run() -> string {
					int caught = 0; int s = 0;
					for (int i = 0; i < 10; i++) { try { if (i % 3 == 0) { throw i; } s += i; } catch (e) { caught += e.value; } }
					return `${s}|${caught}`;
				}
				run();
			)", "27|18");
		});
		test("a_graft_binds_no_argument_before_every_argument_is_evaluated", [this]() {
			// the graft evaluates every argument before it binds the first
			expect("graft args", R"(
				int N = 0;
				function take(int a, int b) -> int { return a + b; }
				function bad() { return "x"; }
				function side() -> int { N = N + 1; return 1; }
				function run() -> string {
					string out = "";
					for (int i = 0; i < 2; i++) { try { out += `${take(bad(), side())}`; } catch (e) { out += "caught,"; } }
					return out + to_string(N);
				}
				run();
			)", "caught,caught,2");
		});
	}

	// fiber frames take the general form; a resumer may change what a
	// suspended loop reads between resumes
	void coroutines_over_fast_paths() {
		test("a_coroutine_yields_between_calls_to_an_inlinable_leaf", [this]() {
			expect("coroutine leaf", R"(
				function leaf(int x) -> int { return x * 3 + 1; }
				coroutine int gen(int n) { int acc = 0; for (int i = 0; i < n; i++) { acc += leaf(i); if (i % 2 == 1) { yield acc; } } yield acc * 10; }
				function run() -> string { string out = ""; for (auto v : gen(6)) { out += `${v},`; } return out; }
				run();
			)", "5,22,51,510,");
		});
		test("a_suspended_loop_reads_what_the_resumer_wrote", [this]() {
			expect("resumer write", R"(
				array<int> xs = [1, 2, 3, 4, 5, 6];
				coroutine int walk() { int s = 0; for (int i = 0; i < xs.size(); i++) { s += xs[i]; if (i == 2) { yield s; } } yield s; }
				function run() -> string { var h = walk(); var a = h.next(); xs[4] = 100; var b = h.next(); return `${a}|${b}`; }
				run();
			)", "6|116");
		});
		test("interleaved_coroutines_calling_a_looped_callee", [this]() {
			// inner(i) = i(i-1)/2; one round sums it over i < 20: C(20, 3) = 1140
			expect("interleaved", R"(
				function inner(int n) -> int { int s = 0; int i = 0; while (i < n) { s = s + i; i = i + 1; } return s; }
				coroutine int gen(int k) { int t = 0; for (int r = 0; r < k; ++r) { int i = 0; while (i < 20) { t = t + inner(i); i = i + 1; } yield t; } return t; }
				function run() -> string { var a = gen(3); var b = gen(2); string out = ""; for (int round = 0; round < 2; round++) { out += `${a.next()}/${b.next()};`; } return out; }
				run();
			)", "1140/1140;2280/2280;");
		});
	}

	// a C++ callback runs script (or changes script state) while a loop is inside a block
	void host_callbacks_mid_loop() {
		test("a_host_callback_writes_the_array_the_loop_is_reading", [this]() {
			every_config("callback write", "116|10|33|9", [](engine& e) {
				engine* raw = &e;
				e.add_function("poke", [raw](script_int i) -> script_int { if (i == 2) { raw->execute("GA[4] = 100;"); } return i; });
				e.add_function("shrink", [raw](script_int i) -> script_int { if (i == 1) { raw->execute("GB.pop(); GB.pop();"); } return i; });
				e.add_function("setk", [raw](script_int i) -> script_int { if (i == 3) { raw->execute("K = 10;"); } return i; });
				e.add_function("retype", [raw](script_int i) -> script_int { if (i == 3) { raw->execute("KF = 2.5;"); } return i; });
				return e.execute(R"(
					array<int> GA = [1, 2, 3, 4, 5, 6];
					array<int> GB = [1, 2, 3, 4, 5, 6];
					var K = 1; var KF = 1;
					function r1() -> int { int s = 0; for (int i = 0; i < 6; i++) { poke(i); s += GA[i]; } return s; }
					function r2() -> int { int s = 0; for (int i = 0; i < GB.size(); i++) { shrink(i); s += GB[i]; } return s; }
					function r3() -> int { int s = 0; for (int i = 0; i < 6; i++) { setk(i); s += K; } return s; }
					function r4() -> int { int s = 0; for (int i = 0; i < 6; i++) { retype(i); s += KF; } return s; }
					`${r1()}|${r2()}|${r3()}|${r4()}`;
				)").as_string();
			});
		});
		test("a_cpp_bound_global_changes_under_a_loop", [this]() {
			every_config("cpp bound", "27,8,7|3", [](engine& e) {
				auto hp = std::make_shared<int64_t>(10);
				e.add_global_ref("HP", *hp);
				e.add_function("hurt", [hp]() { *hp -= 1; });
				e.add_function("peek", [hp]() -> script_int { return *hp; });
				std::string r = e.execute(R"(
					function drain(int n) -> int { int s = 0; for (int i = 0; i < n; i++) { s = s + HP; hurt(); } return s; }
					function write(int n) -> int { int s = 0; for (int i = 0; i < n; i++) { HP = HP - 2; s = s + peek(); } return s; }
					function typed(int x) -> int { int a = x - HP; int b = a * 2 + HP; return b; }
					`${drain(3)},${write(2)},${typed(5)}`;
				)").as_string();
				return r + "|" + std::to_string(*hp);
			});
		});
		test("a_host_callback_re_enters_the_function_running_the_loop", [this]() {
			// t(0) = 3, t(1) = 12, t(2) = 39
			every_config("reentry", "3,12,39|3,12,39", [](engine& e) {
				engine* raw = &e;
				e.add_function("again", [raw](script_int d) -> script_int { return raw->execute("t2(" + std::to_string(d) + ")").as_int(); });
				return e.execute(R"(
					function t(int d) -> int { int s = 0; for (int i = 0; i < 3; ++i) { s = s + i; if (d > 0) { s = s + t(d - 1); } } return s; }
					function t2(int d) -> int { int s = 0; for (int i = 0; i < 3; ++i) { s = s + i; if (d > 0) { s = s + again(d - 1); } } return s; }
					`${t(0)},${t(1)},${t(2)}|${t2(0)},${t2(1)},${t2(2)}`;
				)").as_string();
			});
		});
	}

	// functions and classes always replace in place (hot reload); grafts: a rebind or a
	// reload takes the fallback call
	void rebinding_and_reload_mid_loop() {
		test("a_leaf_redefined_by_a_host_callback_mid_loop_is_called_with_its_new_body", [this]() {
			every_config("reload leaf", "1206|21|1500", [](engine& e) {
				engine* raw = &e;
				e.add_function("swap", [raw]() { raw->execute("function leaf(int x) -> int { return x * 100; }"); });
				e.execute(R"(
					function leaf(int x) -> int { return x + 1; }
					function run() -> int { int s = 0; for (int i = 0; i < 6; i++) { if (i == 3) { swap(); } s += leaf(i); } return s; }
					function plain() -> int { int s = 0; for (int i = 0; i < 6; i++) { s += leaf(i); } return s; }
				)");
				std::string out = std::to_string(e.execute("run()").as_int());
				e.execute("function leaf(int x) -> int { return x + 1; }");
				out += "|" + std::to_string(e.execute("plain()").as_int());
				e.execute("function leaf(int x) -> int { return x * 100; }");
				out += "|" + std::to_string(e.execute("plain()").as_int());
				return out;
			});
		});
		test("a_function_valued_global_rebound_mid_loop", [this]() {
			expect("rebind var fn", R"(
				var fn = [](int x) { return x + 1; };
				function run() -> int { int s = 0; for (int i = 0; i < 6; i++) { if (i == 3) { fn = [](int x) { return x * 100; }; } s += fn(i); } return s; }
				`${run()}`;
			)", "1206");
		});
		test("a_script_function_name_rebound_by_a_callee_mid_loop", [this]() {
			expect("rebind leaf name", R"(
				[[warn_mode]]
				function leaf(int a) -> int { return a * 2; }
				function swap_it(int i) { if (i == 3) { leaf = [](int a) { return a + 1000; }; } }
				function run() -> int { int s = 0; for (int i = 0; i < 6; i++) { swap_it(i); s = s + leaf(i); } return s; }
				`${run()}`;
			)", "3018");
		});
		test("a_local_or_parameter_named_like_a_math_alias_shadows_it", [this]() {
			// docs/site/guide/07-classes.html "Name resolution in methods": locals before globals
			expect("alias shadow", R"(
				function f(int x) -> float { var sqrt = [](float v) { return -v; }; return sqrt(x * 1.0) + sqrt(x); }
				function g(var sqrt) -> string { try { return `${sqrt(4.0)}`; } catch (e) { return `E:${e}`; } }
				function h() -> float { var abs = [](float v) { return 100.0 + v; }; float s = 0.0; for (int i = 0; i < 3; i++) { s += abs(i * 1.0); } return s; }
				`${f(4)},${g(2.5)},${h()},${math::sqrt(16.0)}`;
			)", "-8.0,E:Cannot call 'sqrt': it is a float, not a function,303.0,4.0");
		});
		test("a_class_reloaded_mid_loop_takes_effect_at_the_member_site", [this]() {
			// docs/site/guide/07-classes.html "Hot reload": an accessor a reload adds applies to existing instances
			every_config("reload member", "606|153", [](engine& e) {
				engine* raw = &e;
				e.add_function("host_reload", [raw](script_int i) { if (i == 2) { raw->execute("class P { int x = 3; P() {} P(int v) { x = v; } int _get_x() { return x * 100; } }"); } });
				e.execute(R"(
					class P { int x = 3; P() {} P(int v) { x = v; } }
					P p = P();
					function run() -> int { int s = 0; for (int i = 0; i < 4; i++) { host_reload(i); s = s + p.x; } return s; }
				)");
				std::string out = std::to_string(e.execute("run()").as_int());
				e.execute(R"(
					class Q { int x = 3; Q() {} Q(int v) { x = v; } }
					function swap_q(Q& q, int i) { if (i == 1) { q = Q(50); } }
					function run2() -> int { Q q = Q(); int s = 0; for (int i = 0; i < 4; i++) { swap_q(q, i); s = s + q.x; } return s; }
				)");
				out += "|" + std::to_string(e.execute("run2()").as_int());
				return out;
			});
		});
		test("a_method_redefined_mid_recursion_resolves_the_new_body", [this]() {
			// a reload redefines a class for every caller and every instance; the function analogue
			// (hot_reload_correctness_tests) finishes old frames with their old bodies and resolves the new one on the next call
			every_config("reload method", "103|400", [](engine& e) {
				engine* raw = &e;
				e.add_function("swap_rec", [raw]() { raw->execute("class R { int rec(int k) { if (k <= 0) { return 0; } return rec(k - 1) + 100; } }"); });
				e.execute("class R { int rec(int k) { if (k == 2) { swap_rec(); } if (k <= 0) { return 0; } return rec(k - 1) + 1; } } auto r = R();");
				std::string out = std::to_string(e.execute("r.rec(4)").as_int());
				out += "|" + std::to_string(e.execute("r.rec(4)").as_int());
				return out;
			});
		});
		test("a_coroutine_resumed_after_its_callee_was_redefined_calls_the_new_body", [this]() {
			every_config("reload mid coroutine", "2,4,300,400,", [](engine& e) {
				e.execute(R"(
					function leaf(int x) -> int { return x * 2; }
					coroutine int gen() { for (int i = 1; i <= 4; i++) { yield leaf(i); } }
					var h = gen();
					var out = "";
					out += `${h.next()},`;
					out += `${h.next()},`;
				)");
				e.execute("function leaf(int x) -> int { return x * 100; }");
				return e.execute("out += `${h.next()},`; out += `${h.next()},`; out;").as_string();
			});
		});
	}

	// a callee changes the container a closed loop reads
	void containers_changing_under_a_loop() {
		test("a_callee_shrinks_the_array_the_loop_indexes", [this]() {
			expect("shrink", R"(
				var A = [1, 2, 3, 4, 5, 6];
				function shrink(int i) { if (i == 2) { A.pop(); A.pop(); A.pop(); } }
				function run() -> string {
					int acc = 0; string err = "";
					try { for (int i = 0; i < 6; i++) { shrink(i); acc = acc + A[i]; } } catch (e) { err = e; }
					return `${acc}|${err}`;
				}
				run();
			)", "6|Array index 3 out of bounds for array of size 3");
		});
		test("a_callee_clears_the_array_a_range_for_walks", [this]() {
			// the range-for reads the array's size every iteration
			expect("clear", R"(
				var A = [1, 2, 3, 4];
				function clear_a(int v) { if (v == 2) { A.clear(); } }
				function run() -> int { int s = 0; for (auto v : A) { clear_a(v); s = s + v; } return s; }
				`${run()}|${A.size()}`;
			)", "3|0");
		});
		test("a_destructor_runs_at_frame_pop_after_a_closed_loop", [this]() {
			expect("dtor", R"(
				var LOG = "";
				class D { int id = 0; D(int n) { id = n; } ~D() { LOG = LOG + to_string(id); } }
				function f(int n) -> int { D d = D(n); int s = 0; for (int i = 0; i < 3; ++i) { s = s + i; } return s; }
				int r = f(7);
				LOG + "|" + to_string(r);
			)", "7|3");
		});
	}

	void operators_and_compounds_in_loops() {
		test("a_class_operator_on_a_var_element_inside_a_loop", [this]() {
			expect("operator", R"(
				int CALLS = 0;
				class V { int v = 0; V(int n) { v = n; } int operator+(int o) { CALLS = CALLS + 1; return v + o; } }
				var XS = [1, V(10), 2, V(20)];
				function run() -> string { var acc = 0; for (int i = 0; i < 4; i++) { acc = XS[i] + acc; } return `${acc},${CALLS}`; }
				run();
			)", "33,2");
		});
		test("a_compound_target_is_read_after_its_right_side_inside_a_loop", [this]() {
			// docs/site/guide/04-operators.html "Evaluation order is a contract": x += mutate(x) is 22, x + mutate(x) is 5
			expect("compound order", R"(
				function mutate(int& v) -> int { v = 20; return 2; }
				function c1() -> int { int s = 0; for (int i = 0; i < 4; i++) { int x = 3; x += mutate(x); s += x; } return s; }
				function c2() -> int { int r = 0; for (int i = 0; i < 4; i++) { int x = 3; int y = x + mutate(x); r += y; } return r; }
				`${c1()},${c2()}`;
			)", "88,20");
		});
	}

	// docs/site/guide/04-operators.html shifts, the count's text in docs/site/reference/index.html "Run-time errors";
	// docs/site/guide/05-control-flow.html "Terminal errors": a budget overrun is terminal
	void shifts_and_budget() {
		test("shift_counts_out_of_range_raise_inside_a_counted_loop", [this]() {
			expect("shifts", R"(
				function sh(int a, int s) -> string { try { int r = a << s; return to_string(r); } catch (e) { return `E:${e}`; } }
				function run() -> string {
					int acc = 0; string err = "";
					try { for (int i = 60; i < 66; i++) { acc = acc + (1 << i); } } catch (e) { err = e; }
					return `${sh(1, 3)},${sh(1, 63)},${sh(1, 64)},${sh(1, -1)}|${err}|${acc}`;
				}
				run();
			)", "8,-9223372036854775808,E:Shift amount must be between 0 and 63,E:Shift amount must be between 0 and 63|Shift amount must be between 0 and 63|-1152921504606846976");
		});
		test("a_budget_overrun_inside_a_block_is_terminal_and_leaves_the_flushed_state", [this]() {
			const std::string text = "Script execution budget exceeded - raise engine::execution_budget or break up the work";
			every_config("budget", "terminal|true|caught=0", [&](engine& e) {
				e.execute("int G = 0; int H = 0; int CAUGHT = 0; function spin() -> int { int i = 0; while (i >= 0) { G = G + 1; H = H + 1; i = i + 1; } return i; }");
				e.execution_budget(0.05);
				std::string what;
				try { e.execute("try { spin(); } catch (err) { CAUGHT = 1; }"); } catch (const std::exception& ex) { what = ex.what(); }
				e.execution_budget(0);
				std::string out = what.rfind(text, 0) == 0 ? "terminal" : ("other:" + what);
				out += "|" + std::string(e.execute("G == H && G > 0").as_bool() ? "true" : "false");
				out += "|caught=" + std::to_string(e.execute("CAUGHT").as_int());
				return out;
			});
		});
	}

	// docs/site/embedding/12-debugging.html: both backends debug identically (breakpoints/stepping/locals);
	// a block takes the general back edge while a session is armed
	void debugger_armed_mid_run() {
		test("a_breakpoint_inside_a_closed_loop_fires_every_iteration", [this]() {
			for (const auto& c : attack::configs()) {
				auto e = attack::make(c);
				auto& dbg = e->debugger();
				std::vector<int> lines;
				dbg.set_on_stopped([&](const jai::debug::stop_info& s) { lines.push_back(s.line); dbg.resume(); });
				dbg.set_breakpoints("<script>", {4});
				dbg.set_enabled(true);
				const auto r = e->execute(
					"function run(int n) -> int {\n"
					"  int acc = 0; int i = 0;\n"
					"  while (i < n) {\n"
					"    acc = acc + i * 2;\n"
					"    i = i + 1;\n"
					"  }\n"
					"  return acc;\n"
					"}\n"
					"run(5);\n");
				dbg.set_enabled(false);
				check_eq(int64_t(20), r.as_int(), std::string("result [") + c.label + "]");
				check_eq(size_t(5), lines.size(), std::string("hits [") + c.label + "]");
			}
		});
		test("a_breakpoint_armed_by_a_host_callback_mid_loop_fires_before_the_loop_ends", [this]() {
			// the session is noticed at the next debug sync point; a loop of 20000 iterations passes many of them
			for (const auto& c : attack::configs()) {
				auto e = attack::make(c);
				auto& dbg = e->debugger();
				int stops = 0; int stop_line = 0;
				dbg.set_on_stopped([&](const jai::debug::stop_info& s) { ++stops; stop_line = s.line; dbg.set_enabled(false); });
				jai::debug::controller* d = &dbg;
				e->add_function("arm", [d]() { d->set_breakpoints("<script>", {5}); d->set_enabled(true); });
				const auto r = e->execute(
					"function run(int n) -> int {\n"
					"  int acc = 0; int i = 0;\n"
					"  while (i < n) {\n"
					"    if (i == 2) { arm(); }\n"
					"    acc = acc + (i & 7);\n"
					"    i = i + 1;\n"
					"  }\n"
					"  return acc;\n"
					"}\n"
					"run(20000);\n");
				dbg.set_enabled(false);
				check_eq(int64_t(70000), r.as_int(), std::string("result [") + c.label + "]");
				check_eq(1, stops, std::string("stops [") + c.label + "]");
				check_eq(5, stop_line, std::string("line [") + c.label + "]");
			}
		});
	}

	void removed_element_aliases() {
		test("an_element_alias_whose_element_a_callee_removed_raises_the_removed_element_text", [this]() {
			// a shrink under the reference raises "Reference to a removed array element"; the guide
			// (ch. 3) makes it stop the script; one text on both backends
			every_config("removed alias", "Reference to a removed array element", [](engine& e) {
				e.execute(R"(
					var T = [[1, 2], [3, 4], [5, 6]];
					function drop(int i) { if (i == 1) { T.pop(); T.pop(); } }
					function run() -> int { int acc = 0; for (int i = 0; i < 2; i++) { var& r = T[i]; drop(i); acc = acc + r[0]; } return acc; }
				)");
				try { e.execute("run();"); } catch (const std::exception& ex) { return std::string(ex.what()).substr(0, 36); }
				return std::string("no error");
			});
		});
	}

	// grafts run in workers; results equal the serial program
	void parallel_bodies_with_leaves() {
		test("a_parallel_body_calling_an_inlinable_leaf_matches_the_serial_loop", [this]() {
			expect("parallel leaf", R"(
				function leaf(int a) -> int { return a * a + 1; }
				function run() -> string {
					array<int> xs = []; for (int i = 0; i < 64; i++) { xs.push(i); }
					array<int> ys = []; for (int i = 0; i < 64; i++) { ys.push(i); }
					parallel_for (auto& x : xs) { x = leaf(x) + leaf(x + 1); }
					for (int i = 0; i < 64; i++) { ys[i] = leaf(ys[i]) + leaf(ys[i] + 1); }
					int bad = 0; for (int i = 0; i < 64; i++) { if (xs[i] != ys[i]) { bad++; } }
					return `${bad}|${xs[63]}`;
				}
				run();
			)", "0|8067");
		});
	}
};

} // namespace jai::foundry::tests

using spec_attack_tests = jai::foundry::tests::spec_attack_tests;
FOUNDRY_REGISTER(spec_attack_tests)
