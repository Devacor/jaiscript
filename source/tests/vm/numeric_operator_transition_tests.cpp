#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/testing/foundry.hpp>
#include <string>
#include <tuple>
#include "../case_table.hpp"

namespace jai::foundry::tests {

class numeric_operator_transition_tests : public suite {
	void install_operator(engine& a_engine) {
		a_engine.add_function("+", [](script_int a_left, script_int a_right) {
			return a_left * a_right;
		});
		detail::engine_access::set_has_custom_numeric_operators(a_engine, true);
	}

public:
	numeric_operator_transition_tests() : suite("Numeric Operator Transitions") {}

	void forge_tests() override {
		// The native callback changes the operator table while this same script frame
		// is active. The next typed operation must observe the new operator, including
		// when a future region keeps scalar temporaries unboxed around calls.
		// A host-registered global <, ==, % or <=> over numbers, typed or raw, reaches every numeric lane (int and float
		// locals, typed slots, var operands, loop conditions, top-level code) on every backend as the interpreter reads it
		test("a_global_numeric_comparison_or_modulo_operator_reaches_every_lane", [this]() {
			const std::string script =
				"function shapes(int a, int b, var x, var y) {\n"
				"    int m = a % b; var dm = x % y; bool l = a < b; bool e = a == b; var dl = x < y; var de = x == y;\n"
				"    float fa = a; float fb = b; bool fl = fa < fb; var s = a <=> b; int loops = 0;\n"
				"    for (int i = 0; i < 3; i++) { loops++; }\n"
				"    int w = 0; while (w < 2 && loops < 9) { w++; loops++; }\n"
				"    if (a == b) { loops += 100; }\n"
				"    return to_string(m) + \",\" + to_string(dm) + \",\" + to_string(l) + to_string(e) + to_string(dl) + to_string(de) + to_string(fl) + \",\" + to_string(s) + \",\" + to_string(loops);\n"
				"}\n"
				"int ta = 7; int tb = 3; int tm = ta % tb; bool tl = ta < tb;\n"
				"shapes(7, 3, 7, 3) + \"|\" + shapes(2, 2, 2, 2) + \"|\" + to_string(tm) + to_string(tl);\n";
			auto run = [&](backend_type type, bool reference, bool raw) {
				auto eng = jai::foundry::make_default_engine();
				eng->set_backend(type);
				eng->vm_reference_mode(reference);
				jai::stdlib::register_all(*eng);
				engine* raw_engine = eng.get();
				if (raw) {
					auto num = [](const script_value& v) { return v.deref().is_int() ? static_cast<double>(v.deref().as_int()) : v.deref().as_float(); };
					eng->add_function_with_arity("<", [raw_engine, num](const std::vector<script_value>& a) -> checked_result<script_value> {
						return script_value(num(a[0]) > num(a[1]), raw_engine);
					}, 2);
					eng->add_function_with_arity("==", [raw_engine, num](const std::vector<script_value>& a) -> checked_result<script_value> {
						return script_value(num(a[0]) != num(a[1]), raw_engine);
					}, 2);
					eng->add_function_with_arity("%", [raw_engine, num](const std::vector<script_value>& a) -> checked_result<script_value> {
						return script_value(static_cast<script_int>(num(a[0]) + num(a[1])) + 1000, raw_engine);
					}, 2);
					eng->add_function_with_arity("<=>", [raw_engine](const std::vector<script_value>&) -> checked_result<script_value> {
						return script_value(static_cast<script_int>(77), raw_engine);
					}, 2);
				} else {
					eng->add_function("<", [](script_int a, script_int b) { return a > b; });
					eng->add_function("==", [](script_int a, script_int b) { return a != b; });
					eng->add_function("%", [](script_int a, script_int b) { return a + b + 1000; });
					eng->add_function("<=>", [](script_int, script_int) { return static_cast<script_int>(77); });
				}
				try {
					auto result = eng->execute(script);
					return result.is_string() ? result.as_string() : std::string("<not a string>");
				} catch (const std::exception& e) {
					return std::string("threw: ") + e.what();
				}
			};
			for (bool raw : {false, true}) {
				const std::string oracle = run(backend_type::interpreter, false, raw);
				check_true(oracle.find("1010") != std::string::npos, oracle);
				check_eq(oracle, run(backend_type::vm, false, raw), raw ? "raw vm" : "typed vm");
				check_eq(oracle, run(backend_type::vm, true, raw), raw ? "raw vm-reference" : "typed vm-reference");
			}
		});
		test("registered_mid_function_before_next_typed_operation", [this]() {
			for (bool proofs : {true, false}) {
				for (bool temporary : {false, true}) {
					auto eng = make_engine();
					eng->type_proofs(proofs);
					eng->add_function("install", [this, raw = eng.get()]() {
						install_operator(*raw);
					});
					eng->add_function("pack", [](script_int a_first, script_int a_second) {
						return a_first * 100 + a_second;
					});
					const std::string expression = temporary ? "(a + b) * 10" : "a + b";
					const std::string script = "int calc(int a, int b) { int first = " +
						expression + "; install(); int second = " + expression +
						"; return pack(first, second); } calc(1, 2);";
					const script_int expected = temporary ? 3020 : 302;
					check_eq(expected, eng->execute(script).as_int(), script);
				}
			}
		});
		// every row runs on the interpreter (the oracle) and on the vm with proofs on and off; the oracle shows the failure rows ran
		test("numeric_operators_match_the_interpreter_on_every_operand_path", [this]() {
			const std::tuple<const char*, const char*, const char*> cases[] = {
				// var operands in frame slots take the dynamic slot-operand ops (op_d_*): every kind pair, landing and failure
				// reads as the interpreter reads it, with proofs and without
				{"dynamic_slot_operand_ops_match_the_interpreter",
					"function f(var a, var b) {\n"
					"    var t = a + b;\n"
					"    var u = 0; u = a * b;\n"
					"    var w = (a - b) * 2;\n"
					"    float g = a + b;\n"
					"    if (a < b) { t = t + 1; }\n"
					"    if (a == b) { t = t - 1; }\n"
					"    return to_string(t) + \",\" + to_string(u) + \",\" + to_string(w) + \",\" + to_string(g) + \",\" + type_of(u);\n"
					"}\n"
					"function bits(var a, var b) { var r = (a & b) | (a ^ b); r = r << 2; var s = r >> 1; return to_string(r) + \",\" + to_string(s); }\n"
					"function fail(var a, var b, var k) {\n"
					"    try {\n"
					"        if (k == 0) { var q = a / b; return to_string(q); }\n"
					"        if (k == 1) { var q = a % b; return to_string(q); }\n"
					"        if (k == 2) { var q = a * b; return to_string(q); }\n"
					"        if (k == 3) { var q = a << b; return to_string(q); }\n"
					"        if (k == 5) { var q = a + b; return to_string(q); }\n"
					"        var q = a - b; return to_string(q);\n"
					"    } catch (e) { return \"caught: \" + e; }\n"
					"}\n"
					"var out = f(3, 4) + \"|\" + f(2.5, 4) + \"|\" + f(7, 2.0) + \"|\" + f(6, 6) + \"|\" + bits(12, 10) + \"|\" + fail(\"x\", \"y\", 5);\n"
					"out = out + \"|\" + fail(1, 0, 0) + \"|\" + fail(1.0, 0.0, 0) + \"|\" + fail(5, 0, 1) + \"|\" + fail(9223372036854775807, 2, 2);\n"
					"out = out + \"|\" + fail(1, 64, 3) + \"|\" + fail(-9223372036854775807, 2, 4) + \"|\" + fail(\"s\", 1, 4);\n"
					"out;\n",
					"caught"},
				// operands that reach a binary through the stack (call results, member reads): the numeric rows, the failures and an
				// operator registered mid-run read as the interpreter reads them
				{"stack_operand_binaries_match_the_interpreter",
					"class P { var a = 0; var b = 0; P(var x, var y) { a = x; b = y; } }\n"
					"function id(var v) { return v; }\n"
					"function rows(var x, var y) {\n"
					"    var p = P(x, y);\n"
					"    var out = [];\n"
					"    out.push(p.a + p.b); out.push(p.a - p.b); out.push(p.a * p.b); out.push(id(x) < id(y)); out.push(id(x) >= id(y));\n"
					"    out.push(id(x) == id(y)); out.push(id(x) != id(y)); out.push(p.a <= p.b); out.push(p.a > p.b);\n"
					"    try { out.push(id(x) / id(y)); } catch (e) { out.push(\"caught: \" + e); }\n"
					"    try { out.push(id(x) % id(y)); } catch (e) { out.push(\"caught: \" + e); }\n"
					"    return to_string(out);\n"
					"}\n"
					"function big(var x) { try { return to_string(id(x) * id(x)); } catch (e) { return \"caught: \" + e; } }\n"
					"rows(7, 3) + \"|\" + rows(2.5, 4) + \"|\" + rows(9, 0) + \"|\" + rows(1.0, 0.0) + \"|\" + rows(-4, 2.0) + \"|\" + big(4000000000) + \"|\" + big(3.0);\n",
					"caught"},
				// the census rows: float compounds, string compares and concats through the dynamic ops, string appends, environment stores
				{"general_form_string_float_and_environment_rows_match_the_interpreter",
					"var g = 0; int gi = 1; float gf = 0.5; var gs = \"a\"; string gt = \"t\";\n"
					"function floats(float x, float y) { float s = 0.0; for (int i = 0; i < 4; ++i) { s += x; s -= y * 0.5; s *= 1.5; } return s; }\n"
					"function widen(int k) { float w = 1.5; float v = 0.25; for (int i = 0; i < 3; ++i) { w = k; v = i; } "
					"var q = (w = k + 1); var big = 9007199254740993; w = big; "
					"return `${to_string(w)},${to_string(v)},${to_string(q)},${type_of(q)}${type_of(w)}`; }\n"
					"function strs(var a, var b) {\n"
					"    var out = \"\";\n"
					"    if (a < b) { out += \"lt\"; } if (a == b) { out += \"eq\"; } if (a != b) { out += \"ne\"; } if (a >= b) { out += \"ge\"; }\n"
					"    var c = a + b; string d = a + b; var e = \"\"; e = c + d; var n = null;\n"
					"    if (n == a) { out += \"null\"; }\n"
					"    for (int i = 0; i < 3; ++i) { out += a; out += \"-\"; }\n"
					"    return `${out}|${c}|${d}|${e}`;\n"
					"}\n"
					"function globals() {\n"
					"    for (int i = 0; i < 5; ++i) { g = i; gi = i * 2; gf = i + 0.25; gs = `s${to_string(i)}`; gt = gs; }\n"
					"    g = 2.5; g = true; g = 'c';\n"
					"    var r = `${to_string(g)},${to_string(gi)},${to_string(gf)},${gs},${gt},${type_of(g)}${type_of(gi)}${type_of(gf)}`;\n"
					"    var bad = \"no\"; var frac = 2.75;\n"
					"    try { gi = bad; } catch (e) { r = `${r},caught: ${e}`; }\n"
					"    try { gi = frac; } catch (e) { r = `${r},caught: ${e}`; }\n"
					"    return `${r},${to_string(gi)}`;\n"
					"}\n"
					"class Body { var v = 0; int n = 0; float w = 0.0; }\n"
					"function sq(var k) { return k * k; }\n"
					"function mixed(var a, var b) {\n"
					"    var s = 0.5; var t = 1; var arr = [1, 2.5, 3]; var m = {\"k\": 4}; var acc = 0.0;\n"
					"    var o = Body();\n"
					"    for (var i = 0; i < 3; ++i) {\n"
					"        s += a * b; s -= a; s *= 1.25; s /= b; s += i; s += 2; t += a;\n"
					"        acc = acc + arr[i] * sq(i); acc = acc + m[\"k\"] * sq(i);\n"
					"        o.v = i; o.v = s; o.v = \"str\"; o.n = i; o.w = s; var d = i; var e = s; d = d + 1;\n"
					"    }\n"
					"    try { s /= 0; } catch (e) { s = s + 1000; }\n"
					"    return `${to_string(s)},${to_string(t)},${to_string(acc)},${to_string(o.v)}${to_string(o.n)}${to_string(o.w)}`;\n"
					"}\n"
					"class Node2 { var v = 0; var next = null; }\n"
					"function setr(var& r, var v) { r = v; }\n"
					"function refs() {\n"
					"    var a = 1; var b = null; var arr = [1, 2]; int ti = 3;\n"
					"    setr(a, 2.5); setr(b, Node2()); var n = b; n.v = 7; setr(arr[0], \"e\"); setr(a, b);\n"
					"    var& ra = a; ra = 11; ra = \"s\"; setr(b, null);\n"
					"    try { setr(ti, \"bad\"); } catch (e) { ra = `${ra} caught: ${e}`; }\n"
					"    return `${to_string(a)},${to_string(b)},${to_string(arr)},${to_string(n.v)},${to_string(ti)}`;\n"
					"}\n"
					"g = 9; gi = 3;\n"
					"`${floats(2.0, 1.0)}|${widen(7)}|${strs(\"x\", \"y\")}|${strs(\"q\", \"q\")}|${globals()}|${to_string(g)}${to_string(gi)}|${mixed(2.0, 4)}|${mixed(3, 2)}|${refs()}`;\n",
					"caught"},
				// `return a op b` over every numeric row, the declining rows (overflow, a zero divisor) and the non-numeric operands
				{"return_binary_rows_match_the_interpreter",
					"function id(var v) { return v; }\n"
					"int ti(int v) { return v; }\n"
					"function r(var a, var b, int k) {\n"
					"    if (k == 0) { return id(a) + id(b); } if (k == 1) { return id(a) - id(b); } if (k == 2) { return id(a) * id(b); } if (k == 3) { return id(a) / id(b); }\n"
					"    if (k == 4) { return id(a) % id(b); } if (k == 5) { return id(a) < id(b); } if (k == 6) { return id(a) <= id(b); } if (k == 7) { return id(a) > id(b); }\n"
					"    if (k == 8) { return id(a) >= id(b); } if (k == 9) { return id(a) == id(b); } return id(a) != id(b);\n"
					"}\n"
					"int typed(int a, int b) { return ti(a) * ti(b); }\n"
					"float mixed(int a, float b) { return id(a) / id(b); }\n"
					"function row(var a, var b) { var out = \"\"; for (int k = 0; k < 11; ++k) { try { out += to_string(r(a, b, k)) + \",\"; } catch (e) { out += \"[\" + e + \"],\"; } } return out; }\n"
					"var out = row(7, 3) + \"|\" + row(2.5, 4) + \"|\" + row(9, 0) + \"|\" + row(1.0, 0.0) + \"|\" + row(-7, 2.0) + \"|\" + row(\"a\", \"b\");\n"
					"out += \"|\" + row(9223372036854775807, 2) + \"|\" + to_string(typed(6, 7)) + \",\" + to_string(mixed(1, 4.0));\n"
					"try { typed(4611686018427387904, 4); } catch (e) { out += \",\" + e; }\n"
					"bool lt(var x, var y) { return id(x) < id(y); }\n"
					"var xs = [3, 1.5, 2, -4]; xs.sort(lt); out += \",\" + to_string(xs);\n"
					"out;\n",
					"["},
			};
			check_each(cases, [&](const auto& c) {
				auto run = [&](backend_type type, bool proofs) {
					auto eng = jai::foundry::make_default_engine();
					eng->set_backend(type);
					eng->type_proofs(proofs);
					jai::stdlib::register_all(*eng);
					return eng->execute(std::get<1>(c)).as_string();
				};
				const std::string oracle = run(backend_type::interpreter, true);
				check_true(oracle.find(std::get<2>(c)) != std::string::npos, oracle);
				for (bool proofs : {true, false}) {
					check_eq(oracle, run(backend_type::vm, proofs));
				}
			});
		});
		// a numeric operator registered while a dynamic op's frame runs is consulted by its next execution
		test("dynamic_slot_operand_ops_see_an_operator_registered_mid_function", [this]() {
			for (bool proofs : {true, false}) {
				auto eng = make_engine();
				eng->type_proofs(proofs);
				eng->add_function("install", [this, raw = eng.get()]() { install_operator(*raw); });
				eng->add_function("pack", [](script_int a_first, script_int a_second) { return a_first * 100 + a_second; });
				const std::string script = "function calc(var a, var b) { var first = a + b; install(); var second = a + b; return pack(first, second); } calc(3, 4);";
				check_eq(static_cast<script_int>(712), eng->execute(script).as_int(), script);
			}
		});
	}
};

} // namespace jai::foundry::tests

using numeric_operator_transition_tests_alias = jai::foundry::tests::numeric_operator_transition_tests;
FOUNDRY_REGISTER(numeric_operator_transition_tests_alias)
