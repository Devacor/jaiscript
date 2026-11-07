#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"
#include <format>

namespace jai::foundry::tests {

class lambda_capture_tests : public suite {
public:
    lambda_capture_tests() : suite("Lambda Captures") {}
    
    void forge_tests() override {
        test("capture_lists_copy_or_share_the_locals_they_name", [this]() {
            const std::pair<const char*, int> cases[] = {
                {"[](auto x) -> auto { return x + 1; }(5)", 6},
                {"[=](auto x) -> auto { return x + 1; }(5)", 6},
                // function locals: [=] copies them (a top-level variable is a global, read when the lambda runs)
                {"function run() -> auto { auto x = 10; auto y = 20; auto z = 30; "
                 "auto lambda = [=](auto bonus) -> auto { return x + y + z + bonus; }; x = 100; y = 200; z = 300; "
                 "return lambda(5); } run();", 65},
                {"auto a = 1; auto b = 2; auto lambda = [&](auto multiplier) -> auto { a = a * multiplier; b = b * multiplier; "
                 "return a + b; }; auto result1 = lambda(10); auto result2 = lambda(2); result1 * 100 + result2;", 3060},
                {"function run() -> auto { auto x = 10; auto y = 20; auto counter = 0; "
                 "auto lambda = [=, &counter](auto bonus) -> auto { counter = counter + 1; return x + y + bonus + counter; }; "
                 "x = 100; y = 200; auto result1 = lambda(1); auto result2 = lambda(2); return result1 * 100 + result2; } run();", 3234},
                {"auto x = 10; auto y = 20; auto lambda = [x, y](auto bonus) -> auto { return x + y + bonus; }; x = 100; y = 200; lambda(5);", 35},
                {"auto counter = 0; auto increment = [&counter]() -> auto { counter = counter + 1; return counter; }; auto val1 = increment(); auto val2 = increment(); counter;", 2},
                {"auto f = [=]() -> auto { return 42; }; f()", 42},
                {"auto f = [=](auto x) -> auto { return x; }; f(99)", 99},
            };
            check_each(cases, [&](const auto& c) {
                auto eng = make_engine();
                check_eq(eng->execute(c.first).template as<int>(), c.second);
            });
        });

        test("nested_lambda_reads_the_declaring_frame_not_the_creating_frame", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var stored = null;
                function stash(var cb) -> void { stored = cb; }
                function with_probe(var cb) -> void { cb(7); }
                var run = [=](int outer) -> void {
                    with_probe([=](var probe) -> void {
                        stash([outer, probe]() -> int { return probe * 10 + outer; });
                    });
                };
                run(3);
                stored()
            )");
            check_eq((int64_t)73, result.as_int());
        });

        test("explicit_capture_forwarded_only_through_a_nested_capture_list", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var stored = null;
                function stash(var cb) -> void { stored = cb; }
                function with_probe(var cb) -> void { cb(7); }
                var run = [=](int outer) -> void {
                    with_probe([outer](var probe) -> void {
                        stash([outer, probe]() -> int { return probe * 10 + outer; });
                    });
                };
                run(3);
                stored()
            )");
            check_eq((int64_t)73, result.as_int());
        });

        test("default_capture_chains_through_two_lambda_levels", [this]() {
            auto eng = make_engine();
            auto direct = eng->execute(R"(
                function f(int k) -> int { var g = [=]() -> int { var h = [=]() -> int { return k; }; return h(); }; return g(); }
                f(9)
            )");
            check_eq((int64_t)9, direct.as_int());
            auto mixed = eng->execute(R"(
                function f2(int k) -> int { int j = 1; var g = [=](int m) -> int { var h = [=]() -> int { return k + m + j; }; return h(); }; return g(2); }
                f2(9)
            )");
            check_eq((int64_t)12, mixed.as_int());
            auto bare = eng->execute(R"(
                function f3(int k) -> int { var g = [k]() -> int { var h = [k]() -> int { return k; }; return h(); }; return g(); }
                f3(4)
            )");
            check_eq((int64_t)4, bare.as_int());
            auto bound = eng->execute(R"(
                function f4(int a) -> int {
                    array<int> arr = [5, 6];
                    auto g = [=]() -> int { auto [x, y] = arr; return x + y; };
                    auto h = [arr]() -> int { auto [x, y] = arr; return x - y; };
                    auto k = [=]() -> int { auto inner = [z = a]() -> int { return z; }; return inner(); };
                    return g() * 100 + h() * 10 + k();
                }
                f4(4)
            )");
            check_eq((int64_t)1094, bound.as_int());
        });

        test("by_reference_capture_chains_writes_to_the_declaring_local", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                function f() -> int { int k = 1; var g = [&]() -> void { var h = [&]() -> void { k = 5; }; h(); }; g(); return k; }
                f()
            )");
            check_eq((int64_t)5, result.as_int());
        });

        test("nested_lambda_chain_replays_on_every_creation", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var made = [];
                function make(int seed) -> void { var g = [=]() -> int { var h = [=]() -> int { return seed; }; return h(); }; made.push(g); }
                make(1); make(2); make(3);
                made[0]() * 100 + made[1]() * 10 + made[2]()
            )");
            check_eq((int64_t)123, result.as_int());
        });

        // Capture-source ladder (detail/capture_ladder.hpp): the source of a captured name
        // is the enclosing frame's slot, then the member scope, then the environment chain,
        // so a same-named global never wins over a local, a parameter or a field. Every row
        // runs on both backends and pins byte parity; each shape also runs WITHOUT the global
        // (the control that was always green).
        auto run_both = [](const std::string& src, bool warn_region = false) {
            std::string out[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execution_budget(0);
                jai::stdlib::register_all(e);
                try { out[idx] = e->execute(warn_region ? "[[warn_mode]]; " + src : src).to_string(); }
                catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                ++idx;
            }
            return std::make_pair(out[0], out[1]);
        };
        auto expect = [this, run_both](const std::string& src, const std::string& expected, const char* what) {
            auto [i_out, v_out] = run_both(src);
            check_eq(expected, i_out, std::string(what) + " (interpreter)");
            check_eq(expected, v_out, std::string(what) + " (vm)");
        };
        const std::string with_global = "int a = 5;\n";
        const std::string report = " to_string(f()) + \"|\" + to_string(a)";

        test("default_ref_capture_local_over_same_named_global", [expect, with_global, report]() {
            const std::string f = "int f() { int a = 1; auto lam = [&]() { return a * 10; }; a = 11; return lam(); }";
            expect(with_global + f + report, "110|5", "[&] cell-shares the local, not the global");
            expect(f + " to_string(f())", "110", "[&] without a global");
        });

        test("bare_capture_local_over_same_named_global", [expect, with_global, report]() {
            const std::string f = "int f() { int a = 1; auto lam = [=]() { return a * 10; }; a = 11; return lam(); }";
            expect(with_global + f + report, "10|5", "[=] snapshots the local, not the global");
            expect(f + " to_string(f())", "10", "[=] without a global");
            const std::string f2 = "int f() { int a = 1; auto lam = [=]() { return a * 10; }; return lam(); }";
            expect(with_global + f2 + report, "10|5", "[=] without a later mutation");
        });

        test("explicit_captures_local_over_same_named_global", [expect, with_global, report]() {
            const std::string fr = "int f() { int a = 1; auto lam = [&a]() { return a * 10; }; a = 11; return lam(); }";
            const std::string fv = "int f() { int a = 1; auto lam = [a]() { return a * 10; }; a = 11; return lam(); }";
            expect(with_global + fr + report, "110|5", "[&a] cell-shares the local");
            expect(with_global + fv + report, "10|5", "[a] snapshots the local");
            expect(fr + " to_string(f())", "110", "[&a] without a global");
            expect(fv + " to_string(f())", "10", "[a] without a global");
        });

        test("captured_parameter_shadows_global", [expect, with_global]() {
            const std::string f = "int f(int a) { auto r = [&]() { return a * 10; }; auto v = [=]() { return a * 10; }; auto b = [a]() { return a * 10; }; return r() * 10000 + v() * 100 + b(); }";
            expect(with_global + f + " to_string(f(2)) + \"|\" + to_string(a)", "202020|5", "a parameter is captured over the global");
            expect(f + " to_string(f(2))", "202020", "a parameter without a global");
        });

        test("nested_lambdas_capture_local_over_global", [expect, with_global, report]() {
            const std::string rr = "int f() { int a = 1; auto outer = [&]() { auto inner = [&]() { return a * 10; }; return inner(); }; a = 11; return outer(); }";
            const std::string vv = "int f() { int a = 1; auto outer = [=]() { auto inner = [=]() { return a * 10; }; return inner(); }; a = 11; return outer(); }";
            const std::string bb = "int f() { int a = 1; auto outer = [a]() { auto inner = [a]() { return a * 10; }; return inner(); }; a = 11; return outer(); }";
            const std::string xr = "int f() { int a = 1; auto outer = [&a]() { auto inner = [&]() { return a * 10; }; return inner(); }; a = 11; return outer(); }";
            const std::string mut = "int f() { int a = 1; auto outer = [&]() { auto inner = [&]() { a = a + 1; return a; }; inner(); return a; }; int r = outer(); return r * 100 + a; }";
            const std::string own = "int f() { int a = 1; auto outer = [&]() { auto inner = [&]() { int b = 2; return a * 10 + b; }; return inner(); }; return outer(); }";
            expect(with_global + rr + report, "110|5", "[&] through two levels");
            expect(with_global + vv + report, "10|5", "[=] through two levels");
            expect(with_global + bb + report, "10|5", "[a] through two levels");
            expect(with_global + xr + report, "110|5", "[&a] outer, [&] inner");
            expect(with_global + mut + report, "202|5", "nested mutation lands in the local, the global is untouched");
            expect(with_global + own + report, "12|5", "the inner lambda's own local stays its own");
            expect(rr + " to_string(f())", "110", "[&] two levels without a global");
            expect(mut + " to_string(f())", "202", "nested mutation without a global");
        });

        test("method_local_capture_shadows_field", [expect]() {
            const std::string cls =
                "int a = 5; int g = 500;\n"
                "class C {\n"
                "  int a = 7; int g = 70;\n"
                "  int m() { int a = 1; auto lam = [&]() { a = a + 1; return a; }; int r = lam(); return r * 1000 + a * 10 + this.a; }\n"
                "  int mv() { int a = 1; auto lam = [=]() { return a; }; a = 11; return lam() * 10 + this.a; }\n"
                "  int mb() { int a = 1; auto lam = [a]() { return a; }; a = 11; return lam() * 10 + this.a; }\n"
                "  int p(int a) { auto lam = [&]() { return a; }; return lam(); }\n"
                "  int gg() { int g = 1; auto lam = [&]() { g = g + 1; return g; }; int r = lam(); return r * 1000 + g * 10; }\n"
                "}\n";
            expect(cls + "to_string(C().m()) + \"|\" + to_string(a)", "2027|5", "[&] local over field over global");
            expect(cls + "to_string(C().mv()) + \"|\" + to_string(a)", "17|5", "[=] local over field");
            expect(cls + "to_string(C().mb()) + \"|\" + to_string(a)", "17|5", "[a] local over field");
            expect(cls + "to_string(C().p(3)) + \"|\" + to_string(a)", "3|5", "[&] parameter over field");
            expect(cls + "C c = C(); to_string(c.gg()) + \"|\" + to_string(c.g) + \"|\" + to_string(g)", "2020|70|500", "[&] local over field over global, all three untouched");
        });

        test("method_ref_capture_of_bare_field_cell_shares_field_over_global", [expect]() {
            // Fields outrank globals for the default captures; an EXPLICIT [g] / [&g] names a
            // member, which is not a capturable variable (C++: ill-formed) - the same text
            // the no-global shape always raised, and the global is never touched
            const std::string cls =
                "int g = 500;\n"
                "class C {\n"
                "  int g = 70;\n"
                "  int fref() { auto l = [&]() { g = g + 1; return g; }; int r = l(); return r * 100 + g; }\n"
                "  int fval() { auto l = [=]() { return g; }; g = 71; return l(); }\n"
                "  int fex() { auto l = [g]() { return g; }; return l(); }\n"
                "  int fexr() { auto l = [&g]() { g = g + 1; return g; }; int r = l(); return r * 100 + g; }\n"
                "}\n";
            expect(cls + "to_string(C().fref()) + \"|\" + to_string(g)", "7171|500", "[&] cell-shares the field");
            expect(cls + "to_string(C().fval()) + \"|\" + to_string(g)", "70|500", "[=] snapshots the field");
            expect(cls + "string m = \"\"; try { C().fex(); } catch (e) { m = e; } m + \"|\" + to_string(g)",
                   "Cannot capture undefined variable 'g'|500", "[g] names a member: not capturable, global untouched");
            expect(cls + "string m = \"\"; try { C().fexr(); } catch (e) { m = e; } m + \"|\" + to_string(g)",
                   "Cannot capture undefined variable 'g'|500", "[&g] names a member: not capturable, global untouched");
            const std::string no_global = "[[warn_mode]];\nclass C { int g = 70; int fex() { auto l = [g]() { return g; }; return l(); } }\n";
            expect(no_global + "string m = \"\"; try { C().fex(); } catch (e) { m = e; } m",
                   "Cannot capture undefined variable 'g'", "[g] of a field without a global keeps its text");
        });

        test("mutation_through_ref_capture_visible_in_enclosing_scope_with_global", [expect, with_global, report]() {
            const std::string f = "int f() { int a = 1; auto lam = [&]() { a = a + 100; return 0; }; lam(); return a; }";
            expect(with_global + f + report, "101|5", "the enclosing local sees the write, the global does not");
            expect(f + " to_string(f())", "101", "without a global");
        });

        test("global_still_reached_when_nothing_shadows", [expect]() {
            expect("int g = 1; int f() { auto l = [&]() { g = g + 10; return g; }; return l(); } to_string(f()) + \"|\" + to_string(g)",
                   "11|11", "[&] of an unshadowed global reaches it when the lambda runs");
            expect("int g = 1; int h() { auto l = [=]() { return g; }; g = 99; return l(); } to_string(h()) + \"|\" + to_string(g)",
                   "99|99", "[=] never captures a global: it is read when the lambda runs");
            expect("int g = 1; int k() { auto l = []() { return g; }; g = 7; return l(); } to_string(k()) + \"|\" + to_string(g)",
                   "7|7", "[] of a global stays live");
            expect("int g = 1; class C { int m() { auto l = [&]() { g = g + 10; return g; }; return l(); } } to_string(C().m()) + \"|\" + to_string(g)",
                   "11|11", "[&] of an unshadowed global inside a method cell-shares it");
            expect("int g = 1; class C { int m() { auto l = [g]() { return g; }; g = 99; return l(); } } to_string(C().m()) + \"|\" + to_string(g)",
                   "1|99", "[g] of an unshadowed global inside a method snapshots it");
        });

        // A lambda made in a function over that function's locals keeps its captures as cells on the closure (the vm's converted
        // form): every capture mode keeps the environment form's results
        test("converted_closures_keep_every_capture_mode", [expect]() {
            expect("function f() -> string { int n = 1; var inc = [&n]() { n = n + 1; return n; }; var peek = [n]() { return n; }; inc(); inc();"
                   " var all = [inc, peek, n]() { return to_string(inc()) + \"/\" + to_string(peek()) + \"/\" + to_string(n); };"
                   " return all() + \" \" + all() + \" \" + to_string(n); } f()", "4/1/3 5/1/3 5", "copies, shares and a captured closure");
            expect("function f() -> string { array<int> a = [1, 2, 3]; var p = [a]() { a.push(4); return a.size(); };"
                   " var q = [=]() { return a.size(); }; var r = [&]() { a.push(9); return a.size(); };"
                   " return to_string(p()) + to_string(p()) + to_string(q()) + to_string(r()) + to_string(a.size()); } f()",
                   "45344", "a copied array is the closure's own and lasts between calls");
            expect("function f() -> string { string s = \"ab\"; var w = [s]() { s += \"!\"; return s; }; var rd = [s]() { return s + \"?\"; };"
                   " w(); return w() + \" \" + rd() + \" \" + s; } f()", "ab!! ab? ab", "a written copy and a read-only copy");
            expect("int G = 5; function f() -> int { int k = 1; var g = [=]() { return k + G; }; G = 7; return g(); } to_string(f())", "8",
                   "a global resolves when the lambda runs");
            expect("function f() -> int { int t = 0; for (int i = 0; i < 4; ++i) { int k = i * 10; var g = [k](int x) { return x + k; }; t += g(1); } return t; } to_string(f())",
                   "64", "a loop's closures each copy their iteration's local");
        });

        test("converted_closures_release_in_order_and_without_recursion", [expect]() {
            expect("var log = \"\"; class T { string n; T(string s) { n = s; } ~T() { log += n + \";\"; } }"
                   " function f() { T a = T(\"a\"); T b = T(\"b\"); var c = [a, b]() { return a.n + b.n; }; c(); } f(); log",
                   "b;a;b;a;", "the captured copies die with the closure, last capture first");
            expect("function f(int n) -> int { var g = []() { return 0; }; for (int i = 0; i < n; ++i) { var prev = g; g = [prev]() { return 1; }; } return g(); }"
                   " to_string(f(200000))", "1", "a long chain of closures each holding the last releases without native recursion");
        });

        test("explicit_capture_of_undefined_name_text_parity", [this, run_both]() {
            // A warn region: the runtime text is the one under test
            auto [i_out, v_out] = run_both("auto l = [&nosuch]() { return 1; }; l()", true);
            check_true(i_out.find("Cannot capture undefined variable 'nosuch'") != std::string::npos, i_out);
            check_eq(i_out, v_out, "byte-identical across backends");
            auto [i_val, v_val] = run_both("auto l = [nosuch]() { return 1; }; l()", true);
            check_true(i_val.find("Cannot capture undefined variable 'nosuch'") != std::string::npos, i_val);
            check_eq(i_val, v_val, "byte-identical across backends");
        });
    }
};

} // namespace jai::foundry::tests

// Auto-register with the test framework
using lambda_capture_tests = jai::foundry::tests::lambda_capture_tests;
FOUNDRY_REGISTER(lambda_capture_tests)