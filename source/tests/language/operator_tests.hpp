#pragma once

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <memory>
#include <cmath>

namespace jai::foundry::tests {

class operator_tests : public suite {
public:
    operator_tests() : suite("Operators") {}
    
    void forge_tests() override {
        // Arithmetic operators
        test("basic_arithmetic", [this]() {
            auto eng = make_engine();
            check_eq(eng->execute("10 + 5").as<int>(), 15);
            check_eq(eng->execute("10 - 5").as<int>(), 5);
            check_eq(eng->execute("10 * 5").as<int>(), 50);
            check_eq(eng->execute("10 / 5").as<int>(), 2);
            check_eq(eng->execute("10 % 3").as<int>(), 1);
        });
        
        test("float_arithmetic", [this]() {
            auto eng = make_engine();
            check_near(eng->execute("10.5 + 5.5").as<double>(), 16.0, 0.0001);
            check_near(eng->execute("10.5 - 5.5").as<double>(), 5.0, 0.0001);
            check_near(eng->execute("10.5 * 2.0").as<double>(), 21.0, 0.0001);
            check_near(eng->execute("10.5 / 2.0").as<double>(), 5.25, 0.0001);
        });
        
        test("mixed_type_arithmetic", [this]() {
            auto eng = make_engine();
            check_near(eng->execute("10 + 5.5").as<double>(), 15.5, 0.0001);
            check_near(eng->execute("10.5 - 5").as<double>(), 5.5, 0.0001);
        });
        
        test("unary_operators", [this]() {
            auto eng = make_engine();
            check_eq(eng->execute("-5").as<int>(), -5);
            check_eq(eng->execute("-(-5)").as<int>(), 5);
            check_eq(eng->execute("!true").as<bool>(), false);
            check_eq(eng->execute("!false").as<bool>(), true);
            check_eq(eng->execute("~5").as<int>(), ~5);
        });
        
        test("increment_decrement", [this]() {
            auto eng = make_engine();
            eng->execute("var x = 5;");
            check_eq(eng->execute("x++").as<int>(), 5);  // Postfix returns old value
            check_eq(eng->execute("x").as<int>(), 6);
            check_eq(eng->execute("++x").as<int>(), 7);  // Prefix returns new value
            check_eq(eng->execute("x--").as<int>(), 7);  // Postfix returns old value
            check_eq(eng->execute("x").as<int>(), 6);
            check_eq(eng->execute("--x").as<int>(), 5);  // Prefix returns new value
        });
        
        test("compound_assignments", [this]() {
            auto eng = make_engine();
            eng->execute("var x = 10;");
            check_eq(eng->execute("x += 5").as<int>(), 15);
            check_eq(eng->execute("x -= 3").as<int>(), 12);
            check_eq(eng->execute("x *= 2").as<int>(), 24);
            check_eq(eng->execute("x /= 4").as<int>(), 6);
        });
        
        test("string_concatenation", [this]() {
            auto eng = make_engine();
            eng->execute("var s = \"Hello\";");
            check_eq(eng->execute("s += \", World!\"").as<std::string>(), "Hello, World!");
            // a number, bool or null beside a string raises the checker's text at run time, in either order and on every shape the VM
            // compiles a + to (a chain, a builder store, a fused binary, a compound); a char joins, and a template string's splice is text
            auto run = make_engine();
            check_eq(std::string(
                "cannot apply '+' to 'string' and 'int': stringify it with to_string(x) or `${x}`|"
                "cannot apply '+' to 'int' and 'string': stringify it with to_string(x) or `${x}`|"
                "cannot apply '+' to 'string' and 'float': stringify it with to_string(x) or `${x}`|"
                "cannot apply '+' to 'bool' and 'string': stringify it with to_string(x) or `${x}`|"
                "ss|"
                "cannot apply '+' to 'null' and 'string': stringify it with to_string(x) or `${x}`|"
                "cannot apply '+' to 'string' and 'int': stringify it with to_string(x) or `${x}`|"
                "cannot apply '+' to 'string' and 'int': stringify it with to_string(x) or `${x}`|"
                "cannot apply '+' to 'string' and 'int': stringify it with to_string(x) or `${x}`|"
                "Cannot apply '+=' to int and string|"
                "s3 1.5 true s null [1] 3:4 a7b k1=1 x0,1,2, 5!"), run->execute(R"(
                var n = 3; var f = 1.5; var b = true; var s = "s"; var c = s[0]; var z = null;
                string out = "";
                function shapes(var x) -> string {
                    string acc = "a";
                    string r = "";
                    try { r = acc + x; } catch (e) { return e; }
                    return r;
                }
                for (var pair : [[s, n], [n, s], [s, f], [b, s], [s, c], [z, s]]) {
                    try { var r = pair[0] + pair[1]; out += r + "|"; } catch (e) { out += e + "|"; }
                }
                out += shapes(n) + "|";
                try { string t = "t"; t = t + "x" + n; out += t; } catch (e) { out += e + "|"; }
                try { string t = "t"; t = t + n; out += t; } catch (e) { out += e + "|"; }
                try { var m = 5; m += "x"; out += m; } catch (e) { out += e + "|"; }
                function joined(int i, int j) -> string {
                    map<string, int> h;
                    h[`k${i}`] = 1;
                    string acc = "";
                    for (int k = 0; k < 3; ++k) { acc += `${k},`; }
                    return "" + `${i}:${j}` + " a" + `${i + j * 0 + 4}b` + " " + `k${i - 2}=${h[`k${i}`]}` + " x" + acc;
                }
                out += `${s}${n} ${f} ${b} ${c} ${z} ${[1]} ` + joined(3, 4);
                out + " " + `${n + 2}!`
            )").as_string());
        });
        
        test("comparison_operators", [this]() {
            auto eng = make_engine();
            check_eq(eng->execute("5 < 10").as<bool>(), true);
            check_eq(eng->execute("5 > 10").as<bool>(), false);
            check_eq(eng->execute("5 <= 5").as<bool>(), true);
            check_eq(eng->execute("5 >= 5").as<bool>(), true);
            check_eq(eng->execute("5 == 5").as<bool>(), true);
            check_eq(eng->execute("5 != 3").as<bool>(), true);
        });
        
        test("spaceship_operator", [this]() {
            auto eng = make_engine();
            check_eq(eng->execute("5 <=> 10").as<int>(), -1);
            check_eq(eng->execute("10 <=> 5").as<int>(), 1);
            check_eq(eng->execute("5 <=> 5").as<int>(), 0);
            
            // String comparison
            check_eq(eng->execute("\"apple\" <=> \"banana\"").as<int>(), -1);
            check_eq(eng->execute("\"banana\" <=> \"apple\"").as<int>(), 1);
            check_eq(eng->execute("\"apple\" <=> \"apple\"").as<int>(), 0);
        });
        
        test("logical_operators", [this]() {
            auto eng = make_engine();
            check_eq(eng->execute("true && true").as<bool>(), true);
            check_eq(eng->execute("true && false").as<bool>(), false);
            check_eq(eng->execute("false || true").as<bool>(), true);
            check_eq(eng->execute("false || false").as<bool>(), false);
        });
        
        test("logical_short_circuit", [this]() {
            auto eng = make_engine();
            eng->execute("var counter = 0;");
            eng->execute("function inc() -> auto { counter = counter + 1; return true; }");
            
            // && short-circuits on false
            eng->execute("false && inc()");
            check_eq(eng->execute("counter").as<int>(), 0);
            
            // || short-circuits on true
            eng->execute("true || inc()");
            check_eq(eng->execute("counter").as<int>(), 0);
            
            // Should evaluate second operand
            eng->execute("true && inc()");
            check_eq(eng->execute("counter").as<int>(), 1);
        });
        
        test("bitwise_operators", [this]() {
            auto eng = make_engine();
            check_eq(eng->execute("5 & 3").as<int>(), 1);
            check_eq(eng->execute("5 | 3").as<int>(), 7);
            check_eq(eng->execute("5 ^ 3").as<int>(), 6);
            check_eq(eng->execute("5 << 2").as<int>(), 20);
            check_eq(eng->execute("20 >> 2").as<int>(), 5);
        });
        
        test("ternary_operator", [this]() {
            auto eng = make_engine();
            check_eq(eng->execute("true ? 1 : 2").as<int>(), 1);
            check_eq(eng->execute("false ? 1 : 2").as<int>(), 2);
            check_eq(eng->execute("5 > 3 ? \"yes\" : \"no\"").as<std::string>(), "yes");
        });
        
        test("operator_precedence", [this]() {
            auto eng = make_engine();
            check_eq(eng->execute("2 + 3 * 4").as<int>(), 14);  // Not 20
            check_eq(eng->execute("(2 + 3) * 4").as<int>(), 20);
            check_eq(eng->execute("10 - 2 - 3").as<int>(), 5);  // Left associative
            check_eq(eng->execute("2 * 3 + 4 * 5").as<int>(), 26);
        });
        
        test("division_by_zero", [this]() {
            auto eng = make_engine();
            check_throws([&]() {
                eng->execute("5 / 0");
            }, "Integer division by zero should throw");
            
            check_throws([&]() {
                eng->execute("5.0 / 0.0");
            }, "Float division by zero should throw");
        });
        
        // Regression: identifier+literal binary fast path fed the engine-less AST literal to the
        // operator-method arg path -> uncatchable "Cannot clone script_value: missing engine pointer"
        test("script_class_operator_primitive_rhs", [this]() {
            auto eng = make_engine();
            check_eq(3, eng->execute(
                "class S { int value = 0; S(int v) { value = v; } function operator+(var o) -> S { return S(value + o); } }\n"
                "var a = S(2); var c = a + 1; c.value;").as<int>());
            check_eq(true, eng->execute(
                "class T { int value = 0; T(int v) { value = v; } bool operator<(var o) { return value < o; } }\n"
                "var a = T(1); a < 5;").as<bool>());
            check_eq(true, eng->execute(
                "class U { int value = 0; U(int v) { value = v; } bool operator<(var o) { return value < o; } }\n"
                "var a = U(1); var lim = 5; a < lim;").as<bool>());
            check_eq(std::string("T"), eng->execute(
                "class V { int value = 0; V(int v) { value = v; } bool operator<(var o) { return value < o; } }\n"
                "var a = V(1); var out = \"\"; if (a < 5) { out = \"T\"; } else { out = \"F\"; } out;").as<std::string>());
            // untyped operator return, plain int result (w4 shape)
            check_eq(7, eng->execute(
                "class W { int value = 0; W(int v) { value = v; } function operator+(var o) { return value + o; } }\n"
                "var a = W(3); var r = a + 4; r;").as<int>());
        });

        test("operator_overloading_with_custom_types", [this]() {
            auto eng = make_engine();

            // Define a custom + operator that doubles the result
            bool has_custom_ops = false;
            eng->add_function("+", [&has_custom_ops](int a, int b) -> int {
                has_custom_ops = true;
                return (a + b) * 2;
            });
            // Global numeric operators are opt-in: fast paths stay active until enabled
            detail::engine_access::set_has_custom_numeric_operators(*eng, true);

            // Literal operands constant-fold at parse time, so use variables
            auto result = eng->execute("var a = 5; var b = 3; a + b");
            check_eq(16, result.as<int>());  // (5 + 3) * 2
            check_true(has_custom_ops);
        });

        // Typed operator methods accept named-class
        // return types — `Money operator+(Money o)` parses inside class Money (the
        // own-class-name member branch consumes the operator symbol like the general
        // typed path). Parity with the function-keyword spelling.
        test("operator_named_class_return_type", [this]() {
            const char* src = R"(
                class Money {
                    int cents = 0;
                    Money() { cents = 0; }
                    Money(int c) { cents = c; }
                    Money operator+(Money o) { return Money(cents + o.cents); }
                    bool operator<(Money o) { return cents < o.cents; }
                }
                var a = Money(25);
                var b = Money(50);
                var c = a + b;
                var chained = a + b + Money(100);
                to_string(c.cents) + "|" + to_string(chained.cents) + "|" + to_string(a < b);
            )";
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                stdlib::register_all(*e);
                check_eq(std::string("75|175|true"), e->execute(src).as<std::string>(),
                         use_vm ? "vm class-return operator" : "interp class-return operator");
            }
        });

        test("operator_class_return_matches_function_spelling", [this]() {
            // The typed spelling and the function-keyword spelling produce the same result
            const char* src = R"(
                class V1 {
                    int v = 0;
                    V1() { v = 0; }
                    V1(int n) { v = n; }
                    V1 operator+(V1 o) { return V1(v + o.v); }
                }
                class V2 {
                    int v = 0;
                    V2() { v = 0; }
                    V2(int n) { v = n; }
                    function operator+(V2 o) { return V2(v + o.v); }
                }
                var s1 = (V1(3) + V1(4)).v;
                var s2 = (V2(3) + V2(4)).v;
                to_string(s1) + "|" + to_string(s2);
            )";
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                stdlib::register_all(*e);
                check_eq(std::string("7|7"), e->execute(src).as<std::string>(),
                         use_vm ? "vm spelling parity" : "interp spelling parity");
            }
        });

        // Memberwise ==: arrays, maps and class instance values compare
        // like C++ containers, early out on size/class mismatch and the first unequal member;
        // shared_ptr handles compare by pointer like std::shared_ptr; same_object(x, y) is
        // identity. detail/structural_equality.hpp, one kernel for both backends.
        test("memberwise_equality_arrays_and_maps", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto r = eng->execute(R"(
                array<int> ti = [1, 2, 3]; array<int> tj = [1, 2, 3]; array<int> tk = [1, 2, 4]; array<int> shorter = [1, 2];
                var hv = [1, 2, 3]; var hf = [1.0, 2.0, 3.0];
                array<string> sa = ["a", "b"]; array<string> sb = ["a", "b"]; array<string> sc = ["a", "c"];
                var nested = [[1, 2], [3]]; var nested2 = [[1, 2], [3]]; var nested3 = [[1, 2], [4]];
                map<string, int> m1 = {"a": 1, "b": 2}; map<string, int> m2 = {"b": 2, "a": 1}; map<string, int> m3 = {"a": 1, "b": 3}; map<string, int> m4 = {"a": 1, "c": 2};
                var mk1 = {1: "x"}; var mk2 = {1.0: "x"};
                var deep = {"k": [1, {"z": 2}]}; var deep2 = {"k": [1, {"z": 2}]}; var deep3 = {"k": [1, {"z": 3}]};
                return [ti == tj, ti == tk, ti == shorter, ti != tk, ti == hv, hv == hf, sa == sb, sa == sc,
                        nested == nested2, nested == nested3, m1 == m2, m1 == m3, m1 == m4, mk1 == mk2, deep == deep2, deep == deep3,
                        [] == [], {} == {}];
            )");
            const auto& v = r.as_array();
            const bool expected[] = {true, false, false, true, true, true, true, false, true, false, true, false, false, false, true, false, true, true};
            for (size_t i = 0; i < std::size(expected); ++i) { check_eq(expected[i], v[i].as<bool>(), "row " + std::to_string(i)); }
        });

        test("memberwise_equality_instances_and_handles", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto r = eng->execute(R"(
                class Inner { int z = 0; Inner() {} Inner(int v) { z = v; } }
                class P { int a = 1; string s = "x"; array<int> xs = [1, 2]; Inner in = Inner(3); P child = null; P() {} P(int v) { a = v; } }
                class D : P { int extra = 0; D() {} }
                class Cmp { int a = 1; Cmp() {} Cmp(int v) { a = v; } bool operator==(Cmp other) { return true; } }
                nocopy class Ledger { int total = 5; Ledger() {} }
                P p1 = P(); P p2 = P(); P p3 = P(2); P p4 = P(); p4.in = Inner(9); P p5 = P(); p5.xs.push(3);
                D d = D();
                Cmp c1 = Cmp(1); Cmp c2 = Cmp(2);
                Ledger l1 = Ledger(); Ledger l2 = Ledger();
                var h1 = new P(); var h2 = h1; var h3 = new P();
                var& rp = p1;
                return [p1 == p2, p1 == p3, p1 == p4, p1 == p5, p1 == d, p1 != p2, c1 == c2, l1 == l2,
                        h1 == h2, h1 == h3, h1 != h3, rp == p1, rp == p2, p1 == null, p1.child == p2.child];
            )");
            const auto& v = r.as_array();
            const bool expected[] = {true, false, false, false, false, false, true, true, true, false, true, true, true, false, true};
            for (size_t i = 0; i < std::size(expected); ++i) { check_eq(expected[i], v[i].as<bool>(), "row " + std::to_string(i)); }
        });

        test("memberwise_equality_early_out_at_any_depth", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto r = eng->execute(R"(
                class E { int a = 0; static int calls = 0; E() {} E(int v) { a = v; } bool operator==(E o) { E::calls = E::calls + 1; return a == o.a; } }
                var xs = [E(1), E(2), E(3)]; var ys = [E(1), E(9), E(3)]; var zs = [E(1), E(2)];
                bool r1 = xs == ys; int after_mismatch = E::calls;
                bool r2 = xs == zs; int after_size = E::calls;
                bool r3 = xs == xs; int after_self = E::calls;
                var deep = []; var deep2 = [];
                for (int i = 0; i < 300; i++) { deep = [deep]; deep2 = [deep2]; }
                bool deep_equal = deep == deep2;
                deep2 = [deep2];
                bool deeper_equal = deep == deep2;
                return [r1, after_mismatch, r2, after_size, r3, after_self, deep_equal, deeper_equal];
            )");
            const auto& v = r.as_array();
            check_eq(false, v[0].as<bool>());
            check_eq((int64_t)2, v[1].as_int(), "stops at the first unequal element");
            check_eq(false, v[2].as<bool>());
            check_eq((int64_t)2, v[3].as_int(), "a size mismatch compares no element");
            check_eq(true, v[4].as<bool>());
            check_eq((int64_t)5, v[5].as_int(), "a container compared with itself compares its elements, as a C++ container does (a NaN element is unequal to itself)");
            check_eq(true, v[6].as<bool>(), "values compare at any depth");
            check_eq(false, v[7].as<bool>());
        });

        test("memberwise_equality_survives_an_operator_that_changes_the_operands", [this]() {
            // the comparison holds both containers: an operator== that empties or replaces one never frees what it walks
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto r = eng->execute(R"(
                var xs = [];
                int mode = 0;
                class E { int a = 0; E() {} bool operator==(E o) { if (mode == 0) { xs.clear(); } else { xs = []; } return true; } }
                xs = [[E(), E()], [E()]]; var ys = [[E(), E()], [E()]];
                bool emptied = xs == ys;
                mode = 1;
                xs = [[E(), E()], [E()]];
                bool replaced = xs == ys;
                return [emptied, replaced, xs.size()];
            )");
            const auto& v = r.as_array();
            check_eq(false, v[0].as<bool>(), "an emptied array no longer matches");
            check_eq(true, v[1].as<bool>(), "a replaced array is compared as it was");
            check_eq((int64_t)0, v[2].as_int());
        });

        test("same_object_is_identity", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto r = eng->execute(R"(
                class P { int a = 1; P() {} }
                P p = P(); P q = P(); var& rp = p; P copy = p;
                var h = new P(); var h2 = h; var h3 = new P();
                array<int> xs = [1]; array<int> ys = [1]; var& rxs = xs;
                map<string, int> m = {"a": 1}; map<string, int> m2 = {"a": 1};
                string s = "ab"; string t = "a" + "b";
                P n = null;
                return [same_object(p, p), same_object(p, rp), same_object(p, q), same_object(p, copy),
                        same_object(h, h2), same_object(h, h3), same_object(xs, rxs), same_object(xs, ys),
                        same_object(m, m), same_object(m, m2), same_object(s, t), same_object(1, 1), same_object(1, 2),
                        same_object(n, null), same_object(p, null), same_object(xs, m)];
            )");
            const auto& v = r.as_array();
            const bool expected[] = {true, true, false, false, true, false, true, false, true, false, true, true, false, true, false, false};
            for (size_t i = 0; i < std::size(expected); ++i) { check_eq(expected[i], v[i].as<bool>(), "row " + std::to_string(i)); }
            std::string message;
            try { eng->execute("same_object(1);"); } catch (const std::exception& e) { message = e.what(); }
            check(message.find("'same_object' expects 2 arguments, got 1") != std::string::npos, message);
        });

        test("branch_conditions_run_in_source_order_with_short_circuit", [this]() {
            // nested `&&`, `||` and `!` in if, while and ternary conditions: each operand runs once, in source order, and
            // a short-circuited operand never runs; a logical expression in value position is still a bool
            auto eng = make_engine();
            eng->execute("string log = \"\"; function p(string n, bool v) -> bool { log = log + n; return v; }");
            eng->execute("function run() -> string { log = \"\";"
                         " if ((p(\"a\", false) || p(\"b\", true)) && p(\"c\", false)) { log = log + \"T\"; } else { log = log + \"F\"; }"
                         " if (!(p(\"d\", true) && p(\"e\", false)) || p(\"f\", false)) { log = log + \"T\"; } else { log = log + \"F\"; }"
                         " if (p(\"g\", false) || p(\"h\", false) || p(\"i\", true) || p(\"j\", true)) { log = log + \"T\"; } else { log = log + \"F\"; }"
                         " int i = 0; while ((p(\"k\", i < 2) || p(\"l\", false)) && p(\"m\", true)) { i = i + 1; }"
                         " string t = (p(\"n\", false) || p(\"o\", true)) ? \"x\" : \"y\"; log = log + t;"
                         " if (!(p(\"q\", false) || p(\"r\", true))) { log = log + \"T\"; } else { log = log + \"F\"; }"
                         " int a = 1; float b = 2.5; if ((a < 2 && b > 3.0) || a == 1) { log = log + \"T\"; } else { log = log + \"F\"; }"
                         " if (!(a < 2) || !(b > 3.0)) { log = log + \"T\"; } else { log = log + \"F\"; }"
                         " for (int k = 0; !(k >= 2) && (k < 5 || p(\"z\", true)); k = k + 1) { log = log + \"w\"; }"
                         " bool v = p(\"s\", true) || p(\"t\", false); if (v) { log = log + \"V\"; }"
                         " return log; }");
            check_eq(std::string("abcFdeTghiTkmkmklnoxqrFTTwwsV"), eng->execute("run()").as_string());
            check_eq(std::string("abcFdeTghiTkmkmklnoxqrFTTwwsV"), eng->execute("run()").as_string());
        });

        test("a_ternary_initializer_lands_each_arm_in_the_declaration", [this]() {
            // `T v = c ? a : b;` declares once with the chosen arm, converted by the declaration's rule (a float takes an int
            // arm), nested ternaries and a ternary condition with short-circuit included
            auto eng = make_engine();
            eng->execute("string log = \"\"; function p(string n, bool v) -> bool { log = log + n; return v; }");
            eng->execute("function run(bool c) -> string { log = \"\";"
                         " int a = 3; int b = 7; float w = c ? a : b; string s = c ? \"x\" : \"y\";"
                         " float u = (p(\"m\", c) || p(\"n\", true)) ? (c ? a : b) : (a + b);"
                         " int q = c ? (a > b ? a : b) : (a < b ? a : b);"
                         " return log + s + (w > 5.0 ? \"W\" : \"w\") + (u > 5.0 ? \"U\" : \"u\") + (q == 7 ? \"Q\" : \"q\"); }");
            check_eq(std::string("mxwuQ"), eng->execute("run(true)").as_string());
            check_eq(std::string("mnyWUq"), eng->execute("run(false)").as_string());
        });

        test("operator_subscript_returns_class", [this]() {
            const char* src = R"(
                class Cell {
                    int payload = 0;
                    Cell() { payload = 0; }
                    Cell(int p) { payload = p; }
                }
                class Grid {
                    int base = 10;
                    Cell operator[](int i) { return Cell(base + i); }
                }
                var g = Grid();
                g[5].payload;
            )";
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                check_eq((int64_t)15, e->execute(src).as_int(),
                         use_vm ? "vm operator[] class return" : "interp operator[] class return");
            }
        });
    }
};

} // namespace jai::foundry::tests

// Auto-register with the test framework
FOUNDRY_REGISTER(jai::foundry::tests::operator_tests)