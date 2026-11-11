#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/core/coroutine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"
#include <chrono>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class coroutine_tests : public suite {
public:
    coroutine_tests() : suite("Coroutine Tests") {}

    void forge_tests() override {

        test("coroutines_yield_resume_report_done_and_return", [this]() {
            const std::pair<const char*, script_int> cases[] = {
                {"coroutine int counter() { yield 1; yield 2; yield 3; return 4; } auto c = counter(); auto a = c.next(); "
                 "auto b = c.next(); auto d = c.next(); auto e = c.next(); a + b + d + e", static_cast<script_int>(10)},
                {"coroutine int gen() { yield 42; return 0; } auto c = gen(); auto before = c.done(); c.next(); "
                 "auto mid = c.done(); c.next(); auto after = c.done(); auto result = 0; "
                 "if (!before && !mid && after) { result = 1; } result", static_cast<script_int>(1)},
                {"coroutine int range_gen() { for (int i = 0; i < 5; ++i) { yield i; } return -1; } auto c = range_gen(); "
                 "auto sum = 0; while (!c.done()) { sum = sum + c.next(); } sum", static_cast<script_int>(9)},
                {"coroutine int while_gen() { auto i = 0; while (i < 3) { yield i; i = i + 1; } return 99; } "
                 "auto c = while_gen(); auto r0 = c.next(); auto r1 = c.next(); auto r2 = c.next(); auto r3 = c.next(); "
                 "r0 * 1000 + r1 * 100 + r2 * 10 + r3", static_cast<script_int>(219)},
                {"coroutine int branch_gen(int x) { if (x > 0) { yield 1; } else { yield -1; } return 0; } "
                 "auto c1 = branch_gen(5); auto c2 = branch_gen(-3); auto r1 = c1.next(); auto r2 = c2.next(); r1 + r2", static_cast<script_int>(0)},
                {"coroutine int seq() { yield 10; yield 20; yield 30; yield 40; yield 50; return 0; } auto c = seq(); "
                 "auto sum = 0; sum = sum + c.next(); sum = sum + c.next(); sum = sum + c.next(); sum = sum + c.next(); "
                 "sum = sum + c.next(); sum = sum + c.next(); sum", static_cast<script_int>(150)},
                {"coroutine int gen_a() { yield 1; yield 2; return 3; } coroutine int gen_b() { yield 10; yield 20; return 30; "
                 "} auto a = gen_a(); auto b = gen_b(); auto r1 = a.next(); auto r2 = b.next(); auto r3 = a.next(); "
                 "auto r4 = b.next(); auto r5 = a.next(); auto r6 = b.next(); r1 + r2 + r3 + r4 + r5 + r6", static_cast<script_int>(66)},
                {"coroutine int multiply_gen(int start, int factor) { int val = start; yield val; val = val * factor; "
                 "yield val; val = val * factor; return val; } auto c = multiply_gen(2, 3); auto r1 = c.next(); "
                 "auto r2 = c.next(); auto r3 = c.next(); r1 * 100 + r2 * 10 + r3", static_cast<script_int>(278)},
                {"coroutine int matrix_gen() { for (int i = 0; i < 2; ++i) { for (int j = 0; j < 3; ++j) { yield i * 10 + j; } "
                 "} return -1; } auto c = matrix_gen(); auto sum = 0; while (!c.done()) { sum = sum + c.next(); } sum", static_cast<script_int>(35)},
                {"coroutine void stepper() { yield; yield; return; } auto c = stepper(); c.next(); c.next(); c.next(); "
                 "auto result = 0; if (c.done()) { result = 1; } result", static_cast<script_int>(1)},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                jai::stdlib::register_all(engine);
                check_eq(engine->execute(c.first).template as<script_int>(), c.second);
            });
        });

        // Regression: the interpreter's continuation-replay path skipped the typed-return
        // conversion on the coroutine's FINAL return (the value leaked out unconverted; vm converts)
        test("coroutine_final_typed_return_converts", [this]() {
            auto engine = make_engine();
            engine->execute(R"(
                coroutine int g() { yield 1; return true; }
                var c = g();
                var first = c.next();
            )");
            auto final_val = engine->execute("c.next();");
            check_true(final_val.is_int());
            check_eq(static_cast<script_int>(1), final_val.as<script_int>());
        });

        // Regression: end-of-execute reset_environment_pool() reset(nullptr)'d the suspended
        // coroutine's saved env chain, so resuming in a later execute (with global redeclares
        // in between) threw "Undefined variable 'g'"; vm fibers resumed fine.
        test("coroutine_resume_across_executes_with_redeclare", [this]() {
            // the coroutine body belongs to the unit that declared it: another unit's
            // `var g` shadows and the body keeps its own g; the same unit (a reload of the file) writes in place
            auto engine = make_engine();
            engine->disable(jai::filesystem);
            const std::string path = "D:/virtual/coroutine/redeclare_unit.jai";
            engine->execute_file_source(path, "var g = 1; coroutine int co() { yield g; yield g; yield g; yield g; }");
            engine->execute("var c = co();");
            check_eq(static_cast<script_int>(1), engine->execute("c.next();").as<script_int>());
            engine->execute("var g = 2; var advnew_a = 10;");   // another unit: shadows, the body keeps g = 1
            check_eq(static_cast<script_int>(1), engine->execute("c.next();").as<script_int>());
            engine->execute_file_source(path, "var g = 2;");   // the same unit: in place
            check_eq(static_cast<script_int>(2), engine->execute("c.next();").as<script_int>());
            engine->execute_file_source(path, "g = 3;");
            check_eq(static_cast<script_int>(3), engine->execute("c.next();").as<script_int>());
        });

        test("yield_string_values", [this]() {
            auto engine = make_engine();
            jai::stdlib::register_all(engine);
            auto result = engine->execute(R"(
                coroutine string greet() {
                    yield "hello";
                    yield "world";
                    return "!";
                }
                auto c = greet();
                auto s = "";
                s = s + c.next();
                s = s + " ";
                s = s + c.next();
                s = s + c.next();
                s
            )");
            check_eq(result.template as<std::string>(), std::string("hello world!"));
        });

        test("yield_no_duplicate_side_effects", [this]() {
            auto eng = jai::foundry::make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(R"(
                auto count = 0;
                coroutine void counter() {
                    for (auto i = 0; i < 5; i++) {
                        count = count + 1;
                        yield;
                    }
                }
                auto c = counter();
                c.next();
                c.next();
                c.next();
            )");
            auto count = eng->get_variable("count").as<int64_t>();
            check_eq(count, int64_t(3));
        });

        test("range_for_over_generator", [this]() {
            auto eng = jai::foundry::make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(R"(
                coroutine int fibonacci() {
                    int a = 0;
                    int b = 1;
                    while (true) {
                        yield a;
                        int temp = a + b;
                        a = b;
                        b = temp;
                    }
                }
                auto sum = 0;
                auto count = 0;
                for (auto x : fibonacci()) {
                    sum = sum + x;
                    count = count + 1;
                    if (count == 7) { break; }
                }
            )");
            check_eq(eng->get_variable("sum").as<int64_t>(), int64_t(20));
            check_eq(eng->get_variable("count").as<int64_t>(), int64_t(7));
        });

        test("range_for_over_finite_generator", [this]() {
            auto eng = jai::foundry::make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(R"(
                coroutine int range(int n) {
                    for (int i = 0; i < n; i++) {
                        yield i;
                    }
                }
                auto total = 0;
                for (auto x : range(5)) {
                    total = total + x;
                }
            )");
            check_eq(eng->get_variable("total").as<int64_t>(), int64_t(10));
        });

        test("for_loop_yield_step_by_step", [this]() {
            auto eng = jai::foundry::make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(R"(
                coroutine int gen() {
                    for (int i = 0; i < 3; ++i) {
                        yield i;
                    }
                    return 99;
                }
                auto c = gen();
                auto r0 = c.next();
                auto r1 = c.next();
                auto r2 = c.next();
                auto r3 = c.next();
            )");
            check_eq(eng->get_variable("r0").as<int64_t>(), int64_t(0));
            check_eq(eng->get_variable("r1").as<int64_t>(), int64_t(1));
            check_eq(eng->get_variable("r2").as<int64_t>(), int64_t(2));
            check_eq(eng->get_variable("r3").as<int64_t>(), int64_t(99));
        });

        test("a_coroutine_declared_in_a_function_mints_independent_handles", [this]() {
            const std::pair<const char*, int64_t> cases[] = {
                // A coroutine declared inside a function takes what it needs as parameters (a named function captures
                // nothing); each factory call mints its own independent handle.
                {"function make_counter(auto start) -> auto { coroutine int counter(int first) { auto v = first; "
                 "while (v < first + 3) { yield v; v = v + 1; } return 0 - 1; } return counter(start); } "
                 "auto c1 = make_counter(5); auto c2 = make_counter(50); c1.next() + c2.next() + c1.next() + c2.next();", int64_t(112)},
                {"function pair_sum(int base) -> int { coroutine int gen(int b) { yield b * 10; yield b * 10 + 1; return 0; } "
                 "auto g = gen(base); return g.next() + g.next(); } pair_sum(1) + pair_sum(2);", int64_t(62)},
                {"function make_gen(int base) -> auto { coroutine int gen(int b) { yield b * 10; yield b * 10 + 1; return 0; } "
                 "return gen(base); } auto g1 = make_gen(1); auto g2 = make_gen(2); "
                 "g1.next() + g1.next() + g2.next() + g2.next();", int64_t(62)},
            };
            check_each(cases, [&](const auto& c) {
                auto eng = jai::foundry::make_engine();
                check_eq(c.second, eng->execute(c.first).template as<int64_t>());
            });
        });

        // A named coroutine declared in a function body captures nothing, like a nested named function: an
        // enclosing frame local is a parse error on both backends; a lambda or function(...) / => reads it
        test("nested_coroutine_reading_an_enclosing_local_is_a_parse_error", [this]() {
            for (const char* source : {
                "int f(int base) { coroutine int gen() { yield base; return 0; } auto g = gen(); return g.next(); } f(4);",
                "int f() { int base = 4; coroutine int gen() { base = 5; yield 1; return 0; } auto g = gen(); return g.next(); } f();"}) {
                auto eng = jai::foundry::make_engine();
                std::string message;
                try { eng->execute(source); } catch (const std::exception& e) { message = e.what(); }
                check_true(message.find("nested function 'gen' cannot use enclosing local 'base'") != std::string::npos, message);
            }
        });

        test("a_resumed_coroutine_keeps_its_callers_frame_and_its_iteration_state", [this]() {
            const std::pair<const char*, int64_t> cases[] = {
                // Suspending used to steal the CALLER's call frames into the coroutine
                // state, leaving the rest of the caller's body frameless (hang/corruption)
                {"coroutine int gen2() { yield 10; yield 11; return 0; } function pair() -> int { auto g = gen2(); "
                 "var local = 100; return g.next() + g.next() + local; } pair() + pair();", int64_t(242)},
                // Interpreter used to abort the whole process (env-cycle abort) here
                {"coroutine int inner() { for (int i = 0; i < 3; i++) { yield i; } } "
                 "coroutine int outer() { for (auto x : inner()) { yield x * 10; } return 0 - 1; } auto o = outer(); "
                 "o.next() * 100 + o.next() * 10 + o.next();", int64_t(120)},
                // The failed handle must report done() even when an unrelated coroutine
                // was resumed between its suspension and its failure
                {"function boom() -> auto { throw \"die\"; } coroutine int bad() { yield 1; boom(); yield 2; return 3; } "
                 "coroutine int good() { yield 10; yield 20; return 30; } auto vb = bad(); auto vg = good(); auto d = 0; "
                 "auto r1 = vb.next(); if (vb.done()) { d = d + 1; } auto r2 = vg.next(); if (vb.done()) { d = d + 2; } "
                 "auto caught = 0; try { vb.next(); } catch (e) { caught = 1; } if (vb.done()) { d = d + 4; } "
                 "if (!vg.done()) { d = d + 8; } d * 10 + caught;", int64_t(121)},
                // Resume used to re-evaluate the container and restart iteration at 0
                {"coroutine int walker() { auto acc = 1000; for (auto x : [3, 5, 7]) { yield x; acc = acc + x; } return acc; } "
                 "auto w = walker(); w.next() * 1000 + w.next() * 100 + w.next() * 10 + (w.next() - 1000);", int64_t(3585)},
                {"coroutine int gen() { for (int i = 1; i <= 4; i++) { yield i * i; } return 0; } "
                 "function pump(auto& h) -> auto { auto local = 100; auto v = h.next(); return local + v; } auto c = gen(); "
                 "auto s = 0; for (int i = 0; i < 4; i++) { s = s + pump(c); } s;", int64_t(430)},
            };
            check_each(cases, [&](const auto& c) {
                auto eng = jai::foundry::make_engine();
                check_eq(c.second, eng->execute(c.first).template as<int64_t>());
            });
        });

        // ===== COROUTINE METHODS =====
        // `coroutine int steps() { ... }` inside a class: calling it on an instance mints
        // a handle exactly like a free coroutine; 'this' is pinned in the handle (its own
        // method env + receiver) and survives suspension on both backends.

        auto both_backends = [](const char* src) {
            std::string out[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execution_budget(0);
                jai::stdlib::register_all(e);
                try { out[idx] = e->execute(src).to_string(); }
                catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                ++idx;
            }
            return std::make_pair(out[0], out[1]);
        };
        auto check_both = [this, both_backends](const char* src, const std::string& expected, const char* what) {
            auto [i_out, v_out] = both_backends(src);
            check_eq(expected, i_out, std::string("interp: ") + what);
            check_eq(expected, v_out, std::string("vm: ") + what);
        };
        // Separate execute() calls on one engine (the REPL / host-driven shape): the setup's
        // result is dropped, every step's result (or "ERROR: what") joins with '|'
        auto both_executes = [](const std::string& setup, const std::vector<std::string>& steps) {
            std::string out[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execution_budget(0);
                jai::stdlib::register_all(e);
                try { e->execute(setup); }
                catch (const std::exception& ex) { out[idx] = std::string("SETUP ERROR: ") + ex.what(); }
                for (const auto& step : steps) {
                    if (!out[idx].empty()) { out[idx] += "|"; }
                    try { out[idx] += e->execute(step).to_string(); }
                    catch (const std::exception& ex) { out[idx] += std::string("ERROR: ") + ex.what(); }
                }
                ++idx;
            }
            return std::make_pair(out[0], out[1]);
        };
        auto check_executes = [this, both_executes](const std::string& setup, const std::vector<std::string>& steps,
                                                    const std::string& expected, const char* what) {
            auto [i_out, v_out] = both_executes(setup, steps);
            check_eq(expected, i_out, std::string("interp: ") + what);
            check_eq(expected, v_out, std::string("vm: ") + what);
        };

        test("a_method_coroutine_pins_its_receiver_across_suspensions", [this, check_both]() {
            const std::tuple<const char*, const char*, const char*> cases[] = {
                {R"(
                class Counter {
                    int x = 10;
                    coroutine int steps() {
                        yield 1;
                        yield this.x;
                        x = x + 5;
                        yield x;
                        return true;
                    }
                }
                auto c = Counter();
                auto g = c.steps();
                var a = g.next();
                var b = g.next();
                var d = g.next();
                var e = g.next();
                to_string(a) + "|" + to_string(b) + "|" + to_string(d) + "|" +
                    type_of(e) + ":" + to_string(e) + "|" + to_string(g.done()) + "|" + to_string(c.x)
            )", "1|10|15|int:1|true|15", "this-field reads/writes across suspensions + typed final return + done()"},
                {R"(
                class Gen {
                    int n = 3;
                    coroutine int items() { for (int i = 0; i < n; ++i) { yield i * 10; } }
                }
                auto g = Gen();
                var sum = 0;
                for (auto v : g.items()) { sum += v; }
                sum
            )", "30", "range-for drives a method coroutine"},
                {R"(
                class C { int id = 0; C(int i) { id = i; } coroutine int gen() { yield id; yield id * 2; } }
                auto a = C(1); auto b = C(2);
                auto ga = a.gen(); auto gb = b.gen();
                to_string(ga.next()) + to_string(gb.next()) + to_string(ga.next()) + to_string(gb.next())
            )", "1224", "two instances' coroutines interleave independently"},
                // The only strong ref to the instance dies with make()'s frame; the handle
                // keeps it alive (receiver + method env pin), so resumes still see fields
                {R"(
                class D { int v = 7; coroutine int g() { yield v; v = 99; yield v; } }
                function make() { auto d = D(); return d.g(); }
                auto h = make();
                to_string(h.next()) + "|" + to_string(h.next())
            )", "7|99", "handle keeps the instance alive while suspended"},
                {R"(
                class Base { int bv = 5; coroutine int gen() { yield bv; yield bv * 2; } }
                class Derived : Base { int dv = 1; }
                auto d = Derived();
                d.bv = 6;
                auto g = d.gen();
                to_string(g.next()) + "|" + to_string(g.next())
            )", "6|12", "base coroutine method binds the derived receiver"},
            };
            check_each(cases, [&](const auto& c) { check_both(std::get<0>(c), std::get<1>(c), std::get<2>(c)); });
        });

        // Static coroutine methods: every spelling (either modifier order) mints a handle
        // through the static_method callable; the body reads and stores the class's statics
        test("static_coroutine_method_spellings", [this, check_both]() {
            check_both(R"(
                class S {
                    static int n = 10;
                    coroutine static int f1() { yield n; n += 1; yield n; return 0; }
                    coroutine static f2() -> int { yield n; n += 1; yield n; return 0; }
                    coroutine function static f3() { yield n; n += 1; yield n; return 0; }
                    static coroutine int f4() { yield n; n += 1; yield n; return 0; }
                    coroutine static function f5() { yield n; n += 1; yield n; return 0; }
                }
                string drain(var h) { string s = ""; for (auto v : h) { s += to_string(v) + ","; } return s; }
                drain(S::f1()) + drain(S::f2()) + drain(S::f3()) + drain(S::f4()) + drain(S::f5()) + to_string(S::n)
            )", "10,11,11,12,12,13,13,14,14,15,15", "static coroutine spellings share the class's static field");
            check_both(R"(
                class S { static int n = 0; coroutine static int f() { n += 1; yield n; n += 1; yield n; return 0; } }
                auto a = S::f(); auto b = S::f();
                to_string(a.next()) + to_string(b.next()) + to_string(a.next()) + to_string(b.next()) + "|" + to_string(S::n) + "|" + to_string(a.done())
            )", "1234|4|false", "two static coroutine handles interleave over one static");
        });

        test("static_coroutine_survives_reload", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(e);
                e->execute("class S { static int n = 5; coroutine static int f() { yield n; yield n + 1; return 0; } }\nvar h = S::f();\nvar a = h.next();");
                e->execute("class S { static int n = 5; string extra = \"e\"; coroutine static int f() { yield 99; return 0; } }");
                auto r = e->execute("to_string(a) + \"|\" + to_string(h.next()) + \"|\" + to_string(S::f().next())");
                check_eq(std::string("5|6|99"), r.to_string(), std::string(use_vm ? "vm" : "interp") + ": the suspended static handle keeps the old body; fresh calls mint the new one");
            }
        });

        test("coroutine_field_rejected", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                bool threw = false;
                try { e->execute("class K { coroutine int x = 5; }"); }
                catch (const std::exception& ex) {
                    threw = std::string(ex.what()).find("'coroutine' in a class body must be followed by a method") != std::string::npos;
                }
                check(threw, "coroutine before a field reports the dedicated error");
            }
        });

        // Every function spelling takes the `coroutine` prefix and produces the same
        // declaration node: one 3-yield generator per spelling, driven by range-for
        test("coroutine_free_spellings_same_sequence", [this, check_both]() {
            const char* heads[] = {
                "coroutine gen()", "coroutine gen() -> auto", "coroutine gen() -> int", "coroutine gen() ->",
                "coroutine function gen()", "coroutine function gen() -> auto", "coroutine function gen() -> int",
                "coroutine int gen()", "coroutine auto gen()", "coroutine var gen()", "coroutine int gen() -> int",
            };
            for (const char* head : heads) {
                std::string src = std::string(head) + " { yield 1; yield 2; yield 3; return 0; }\n"
                    "string s = \"\"; auto h = gen(); for (auto v : h) { s += to_string(v) + \"|\"; } s + to_string(h.done())";
                check_both(src.c_str(), "1|2|3|true", head);
            }
            check_both("int outer() { coroutine gen() -> auto { yield 1; yield 2; yield 3; } int n = 0; for (auto v : gen()) { n = n * 10 + v; } return n; }\nouter()",
                "123", "bare spelling nested in a function body");
        });

        test("coroutine_method_spellings_same_sequence", [this, check_both]() {
            check_both(R"(
                class C {
                    int base = 1;
                    coroutine gen1() -> auto { yield base; yield base + 1; yield base + 2; return 0; }
                    coroutine function gen2() -> int { yield base; yield base + 1; yield base + 2; return 0; }
                    coroutine int gen3() { yield base; yield base + 1; yield base + 2; return 0; }
                    coroutine gen4() { yield base; yield base + 1; yield base + 2; return 0; }
                    coroutine function gen5() { yield base; yield base + 1; yield base + 2; return 0; }
                    coroutine gen6() -> { yield base; yield base + 1; yield base + 2; return 0; }
                }
                class D : C {
                    coroutine override int gen3() { yield 7; yield 8; yield 9; return 0; }
                    coroutine override gen4() { yield 7; yield 8; yield 9; return 0; }
                }
                string drain(var h) { string s = ""; for (auto v : h) { s += to_string(v); } return s; }
                auto c = C(); auto d = D();
                drain(c.gen1()) + "|" + drain(c.gen2()) + "|" + drain(c.gen3()) + "|" + drain(c.gen4()) + "|" + drain(c.gen5()) + "|" + drain(c.gen6())
                    + "|" + drain(d.gen3()) + "|" + drain(d.gen4()) + "|" + drain(d.gen1())
            )", "123|123|123|123|123|123|789|789|123", "method spellings and overrides");
            check_both(R"(
                class V {
                    int n = 2;
                    coroutine operator+(int k) { yield n + k; yield n * k; return 0; }
                    coroutine function operator-(int k) { yield n - k; return 0; }
                }
                string s = ""; auto v = V();
                for (auto x : v + 3) { s += to_string(x) + ","; }
                for (auto x : v - 3) { s += to_string(x) + ","; }
                s
            )", "5,6,-1,", "bare coroutine operator method beside the function-keyword spelling");
        });

        // The checker stays lenient for coroutine returns (coroutines_stay_lenient): a wrong
        // completion type raises on the delivering resume, with one text and one trace on
        // every spelling and both backends
        test("coroutine_return_type_enforced_identically_across_spellings", [this]() {
            auto run = [](bool use_vm, const std::string& src) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(e);
                std::string out;
                try { out = e->execute(src).to_string(); }
                catch (const std::exception& ex) { out = std::string(ex.what()) + "\n" + e->format_stack_trace(); }
                return out;
            };
            const char* heads[] = { "coroutine gen() -> int", "coroutine function gen() -> int", "coroutine int gen()", "coroutine int gen() -> int" };
            const char* bodies[] = {
                " { yield 1; return \"x\"; }\nauto h = gen();\nvar first = h.next();\nh.next();",
                " { yield 1; var s = \"x\"; return s; }\nauto h = gen();\nvar first = h.next();\nh.next();",
            };
            for (const char* body : bodies) {
                std::string reference;
                for (const char* head : heads) {
                    for (bool use_vm : {false, true}) {
                        const std::string label = std::string(head) + (use_vm ? " vm" : " interp");
                        std::string out = run(use_vm, std::string(head) + body);
                        check_true(out.find("Cannot return string from 'gen' (declared -> int)") == 0, label + ": " + out);
                        check_true(out.find("at gen (<script>:1)") != std::string::npos, label + " attributes to the coroutine frame: " + out);
                        if (reference.empty()) { reference = out; }
                        else { check_eq(reference, out, label + ": same text and trace as the first spelling"); }
                    }
                }
            }
            for (const char* head : heads) {
                for (bool use_vm : {false, true}) {
                    check_eq(std::string("int:1"), run(use_vm, std::string(head) + " { yield 1; return true; }\nauto h = gen(); h.next(); var r = h.next(); type_of(r) + \":\" + to_string(r)"),
                        std::string(head) + (use_vm ? " vm" : " interp") + ": the completion value converts to int");
                }
            }
        });

        test("coroutine_spellings_reject_ref_return", [this, both_backends]() {
            const char* srcs[] = {
                "coroutine gen() -> int& { yield 1; }",
                "coroutine function gen() -> int& { yield 1; }",
                "coroutine int& gen() { yield 1; }",
                "class C { coroutine gen() -> int& { yield 1; } }",
                "class C { coroutine function gen() -> int& { yield 1; } }",
                "class C { coroutine static gen() -> int& { yield 1; } }",
            };
            for (const char* src : srcs) {
                auto [i_out, v_out] = both_backends(src);
                check_true(i_out.find("coroutines cannot return references") != std::string::npos, std::string("interp: ") + src + " -> " + i_out);
                check_eq(i_out, v_out, std::string("parity: ") + src);
            }
            auto e = jai::foundry::make_engine();
            try { e->execute("class C { coroutine gen() -> int& { yield 1; } }"); } catch (const std::exception&) {}
            check_eq(static_cast<script_int>(4), e->execute("2 + 2").as<script_int>(), "engine stays usable after the rejection");
        });

        // A coroutine runs on after its call returns: a reference parameter, in every spelling and position, is a parse error
        test("coroutine_spellings_reject_reference_parameters", [this, both_backends]() {
            const char* srcs[] = {
                "coroutine int g(int& n) { yield n; }",
                "coroutine g(const int& n) -> int { yield n; }",
                "coroutine function g(var& n) { yield n; }",
                "coroutine g(auto& n) { yield n; }",
                "class C { coroutine int g(int& n) { yield n; } }",
                "class C { static coroutine int g(int a, int& n) { yield n; } }",
                "namespace ns { coroutine int g(int& n) { yield n; } }",
                "int x = 1; coroutine int g(int& n = x) { yield n; }",
            };
            for (const char* src : srcs) {
                auto [i_out, v_out] = both_backends(src);
                check_true(i_out.find("cannot take 'n' by reference: it runs on after the call returns, past the argument's lifetime; take it by value (") != std::string::npos,
                           std::string("interp: ") + src + " -> " + i_out);
                check_eq(i_out, v_out, std::string("parity: ") + src);
            }
            auto [i_out, v_out] = both_backends("coroutine int count_up(int& n) { n += 1; yield n; }");
            check_true(i_out.find("coroutine 'count_up' cannot take 'n' by reference: it runs on after the call returns, past the argument's lifetime; "
                                  "take it by value (int n), or pass a shared_ptr to share an object") != std::string::npos, i_out);
            check_eq(i_out, v_out, "parity: the full text");
            check_eq(std::string("2|3"), both_backends("class Counter { int n = 1; }\ncoroutine int count_up(shared_ptr<Counter> c) { c.n += 1; yield c.n; }\n"
                "auto c = new Counter(); auto h = count_up(c); auto first = h.next(); c.n += 1; to_string(first) + \"|\" + to_string(c.n)").first,
                "a shared_ptr parameter is the shared spelling");
        });

        // No coroutine lambda / anonymous form exists: the keyword is only a declaration prefix
        test("coroutine_bare_spelling_is_not_an_expression", [this, both_backends]() {
            const char* srcs[] = {
                "var f = coroutine () { yield 1; }; 1",
                "var f = coroutine function() { yield 1; }; 1",
                "var f = coroutine gen() { yield 1; }; 1",
            };
            for (const char* src : srcs) {
                auto [i_out, v_out] = both_backends(src);
                check_true(i_out.find("ERROR: ") == 0, std::string("interp rejects: ") + src + " -> " + i_out);
                check_eq(i_out, v_out, std::string("parity: ") + src);
            }
        });

        // HOT RELOAD PRECEDENT (free coroutines): redefining the function while a handle
        // is suspended leaves the handle running the OLD body (it pins its own decl +
        // closure env); new calls mint handles over the NEW body.
        test("free_coroutine_redefine_while_suspended", [this]() {
            auto engine = make_engine();
            engine->execute("coroutine int f() { yield 1; yield 2; return 3; }");
            engine->execute("var h = f();");
            check_eq(static_cast<script_int>(1), engine->execute("h.next();").as<script_int>());
            engine->execute("coroutine int f() { yield 100; return 200; }"); // redefine mid-suspension
            check_eq(static_cast<script_int>(2), engine->execute("h.next();").as<script_int>());  // old body continues
            check_eq(static_cast<script_int>(3), engine->execute("h.next();").as<script_int>());
            engine->execute("var h2 = f();");
            check_eq(static_cast<script_int>(100), engine->execute("h2.next();").as<script_int>()); // new body for new handles
        });

        // A FIELD-CHANGING reload while a method coroutine is suspended: migration runs
        // IN PLACE (kept fields keep their storage nodes — suspended env chains cache
        // field pointers), so the old body resumes over the live, migrated instance.
        // A retype mid-suspension converts the value per the retype ruling and the old
        // body simply sees the converted value (int 40 -> "40"; "40" + 1 concatenates).
        test("method_coroutine_survives_field_changing_reload", [this, check_both]() {
            check_both(R"(
                class W { int v = 1; coroutine int gen() { yield v; yield v + 1; return 0; } }
                var w = W();
                w.v = 40;
                var h = w.gen();
                var a = h.next();
                class W { int v = 1; string extra = "e"; coroutine int gen() { yield 8; return 0; } }
                var b = h.next();
                `${to_string(a)}|${to_string(b)}|${w.extra}`
            )", "40|41|e", "field added mid-suspension: old body continues on stable storage");
            check_both(R"(
                class W { int v = 1; coroutine var gen() { yield v; yield type_of(v); return 0; } }
                var w = W();
                w.v = 40;
                var h = w.gen();
                var a = h.next();
                class W { string v = ""; coroutine var gen() { yield 8; return 0; } }
                var b = to_string(h.next());
                `${to_string(a)}|${b}|${w.v}`
            )", "40|string|40", "retype mid-suspension: old body sees the converted value");
        });

        // Method coroutines MATCH that precedent: a class hot reload while a method
        // coroutine is suspended leaves the suspended handle on the OLD body, resuming
        // against the SAME (migrated) instance; fresh calls use the NEW body.
        // HANDLE REFERENCE SEMANTICS: a handle IS a reference to a
        // running computation, so copies SHARE the live coroutine (clone() shallow-shares
        // the holder - value.cpp jai_object_type case). Field stores, aliases, by-value
        // params, and container elements all see the same resume/done state.

        test("a_coroutine_handle_is_a_reference_every_copy_shares", [this, check_both]() {
            const std::tuple<const char*, const char*, const char*> cases[] = {
                {R"(
                coroutine function co() { yield 1; yield 2; }
                class B { var h = null; }
                var b = B();
                b.h = co();
                to_string(b.h.next()) + "|" + to_string(b.h.next()) + "|" + to_string(b.h.done())
            )", "1|2|false", "handle lives in a field; resume/done through the field"},
                {R"(
                coroutine function co() { yield 1; yield 2; yield 3; return 0; }
                var h = co();
                var h2 = h;
                var a = h.next(); var b = h2.next(); var c = h.next(); h2.next();
                to_string(a) + to_string(b) + to_string(c) + "|" +
                    to_string(h.done()) + "|" + to_string(h2.done())
            )", "123|true|true", "aliases interleave one coroutine; done() agrees"},
                {R"(
                coroutine function co() { yield 7; yield 8; }
                var a = [co()];
                var m = {"k": a[0]};
                to_string(a[0].next()) + "|" + to_string(m["k"].next())
            )", "7|8", "array element and map entry share the stored handle"},
                {R"(
                coroutine function co() { yield 42; return 0; }
                function drive(var h) -> int { return h.next(); }
                var h = co();
                var a = drive(h);
                h.next();
                to_string(a) + "|" + to_string(h.done())
            )", "42|true", "by-value param resumes the caller's coroutine"},
                // Object copies stay value-semantic (deep) - but the handle FIELD shares the
                // one coroutine, exactly like a shared_ptr field would share its pointee.
                {R"(
                coroutine function co() { yield 10; yield 20; yield 30; }
                class Holder { var h = null; int n = 0; }
                var a = Holder();
                a.h = co();
                var b = a;
                b.n = 99;
                to_string(a.h.next()) + to_string(b.h.next()) + to_string(a.h.next()) +
                    "|" + to_string(a.n) + to_string(b.n)
            )", "102030|099", "copied instance shares the coroutine, not the ints"},
                // The crawler boss shape: a Game method coroutine whose handle lives in a
                // Game field (receiver pin + field-held handle = deliberate strong cycle;
                // broken by re-assignment/null like any shared structure).
                {R"(
                class Game {
                    var boss_co = null;
                    int phase = 0;
                    coroutine void brain() { phase = 1; yield; phase = 2; yield; phase = 3; }
                    function tick() -> void {
                        if (boss_co == null || boss_co.done()) { boss_co = brain(); }
                        boss_co.next();
                    }
                }
                var g = Game();
                g.tick(); g.tick(); g.tick();
                to_string(g.phase)
            )", "3", "own-method handle stored in own field drives across ticks"},
            };
            check_each(cases, [&](const auto& c) { check_both(std::get<0>(c), std::get<1>(c), std::get<2>(c)); });
        });

        test("field_held_handle_survives_field_changing_reload", [this, check_both]() {
            check_both(R"(
                class W { var co = null; int v = 1; coroutine int gen() { yield v; yield v + 1; return 0; } }
                var w = W();
                w.v = 40;
                w.co = w.gen();
                var a = w.co.next();
                class W { var co = null; int v = 1; string extra = "e"; coroutine int gen() { yield 8; return 0; } }
                var b = w.co.next();
                var h2 = w.gen();
                to_string(a) + "|" + to_string(b) + "|" + w.extra + "|" + to_string(h2.next())
            )", "40|41|e|8", "in-place migration keeps the field-held handle on the old body");
        });

        test("engine_teardown_with_field_held_suspended_handle", [this]() {
            // Debug's 0xDD sweep catches any teardown ordering bug: the engine dies while
            // a SUSPENDED handle sits in a field of its own receiver (the strong cycle).
            for (bool use_vm : {false, true}) {
                {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    e->execution_budget(0);
                    auto r = e->execute(R"(
                        class Game { var co = null; coroutine int brain() { yield 1; yield 2; yield 3; } }
                        var g = Game();
                        g.co = g.brain();
                        g.co.next()
                    )");
                    check_eq(static_cast<script_int>(1), r.as<script_int>());
                }   // engine + suspended fiber/continuation + cycle all die here
                auto fresh = jai::foundry::make_engine();
                check_eq(static_cast<script_int>(4), fresh->execute("2 + 2").as<script_int>());
            }
        });

        test("method_coroutine_hot_reload_matches_free_precedent", [this]() {
            auto engine = make_engine();
            engine->execute(R"(
                class W { int v = 1; coroutine int gen() { yield v; yield v + 1; return 0; } }
                var w = W();
                var h = w.gen();
            )");
            check_eq(static_cast<script_int>(1), engine->execute("h.next();").as<script_int>());
            engine->execute("class W { int v = 1; coroutine int gen() { yield v * 100; return 0; } }");
            // Old body continues, reading the live (migrated) instance field
            check_eq(static_cast<script_int>(2), engine->execute("h.next();").as<script_int>());
            engine->execute("var h2 = w.gen();");
            check_eq(static_cast<script_int>(100), engine->execute("h2.next();").as<script_int>());
        });

        // ===== COVERAGE BATTERIES (both backends; the vm runs fibers, the interpreter
        // replays continuations - the differential is where the bugs hide) =====
        // drain(h): every resumed value including the completion value, '|'-joined.
        const std::string prelude =
            "string drain(var h) {\n"
            "    string s = \"\";\n"
            "    while (!h.done()) {\n"
            "        var v = h.next();\n"
            "        if (s != \"\") { s += \"|\"; }\n"
            "        s += to_string(v);\n"
            "    }\n"
            "    return s;\n"
            "}\n";
        auto both_drained = [check_both, prelude](const std::string& body, const std::string& expected, const char* what) {
            check_both((prelude + body).c_str(), expected, what);
        };
        auto check_parity = [this, both_backends](const char* src, const char* what) {
            auto [i_out, v_out] = both_backends(src);
            check_eq(i_out, v_out, std::string("parity: ") + what);
            return i_out;
        };

        test("decl_spellings_free", [this, both_drained]() {
            const std::string drive = "auto h = g(); drain(h) + \"#\" + to_string(h.done())";
            both_drained("coroutine int g() { yield 1; return true; }\n" + drive, "1|1#true", "coroutine int g()");
            both_drained("coroutine function g() -> auto { yield 1; return true; }\n" + drive, "1|true#true", "coroutine function g() -> auto");
            both_drained("coroutine function g() { yield 1; return true; }\n" + drive, "1|true#true", "coroutine function g()");
            both_drained("coroutine var g() { yield 1; return true; }\n" + drive, "1|true#true", "coroutine var g()");
            both_drained("coroutine auto g() { yield 1; return true; }\n" + drive, "1|true#true", "coroutine auto g()");
            both_drained("coroutine string g() { yield \"1\"; return \"2\"; }\n" + drive, "1|2#true", "coroutine string g()");
            both_drained("coroutine void g() { yield; yield; }\n" + drive, "null|null|null#true", "coroutine void g()");
        });

        test("decl_spellings_free_arrow_only", [this, both_drained]() {
            // `coroutine name() -> T` (no leading type, no `function`): landing with the coroutine syntax pass
            const std::string drive = "auto h = g(); drain(h) + \"#\" + to_string(h.done())";
            both_drained("coroutine g() -> int { yield 1; return true; }\n" + drive, "1|1#true", "coroutine g() -> int");
            both_drained("coroutine g() -> auto { yield 1; return true; }\n" + drive, "1|true#true", "coroutine g() -> auto");
            both_drained("coroutine g() -> string { yield \"1\"; return \"2\"; }\n" + drive, "1|2#true", "coroutine g() -> string");
        });

        test("decl_spellings_methods", [this, both_drained]() {
            const std::string drive = "auto c = C(); auto h = c.g(); drain(h) + \"#\" + to_string(h.done()) + \"#\" + to_string(c.v)";
            both_drained("class C { int v = 3; coroutine int g() { yield v; v = 4; return true; } }\n" + drive, "3|1#true#4", "coroutine int g() method");
            both_drained("class C { int v = 3; coroutine function g() -> auto { yield v; v = 4; return true; } }\n" + drive, "3|true#true#4", "coroutine function g() -> auto method");
            both_drained("class C { int v = 3; coroutine function g() { yield v; v = 4; return true; } }\n" + drive, "3|true#true#4", "coroutine function g() method");
            both_drained("class C { int v = 3; coroutine var g() { yield v; v = 4; return true; } }\n" + drive, "3|true#true#4", "coroutine var g() method");
            both_drained("class C { int v = 3; coroutine auto g() { yield v; v = 4; return true; } }\n" + drive, "3|true#true#4", "coroutine auto g() method");
            both_drained("class C { int v = 3; coroutine void g() { yield; v = 4; } }\n" + drive, "null|null#true#4", "coroutine void g() method");
            both_drained("class B { int v = 3; } class C : B { coroutine int g() { yield v; v = 4; return true; } }\n" + drive, "3|1#true#4", "coroutine method over an inherited field");
            both_drained("class B { int v = 3; coroutine int g() { yield v; return 1; } } class C : B { coroutine override int g() { yield v; v = 4; return true; } }\n" + drive, "3|1#true#4", "coroutine override int g() method");
        });

        test("decl_spellings_methods_arrow_only", [this, both_drained]() {
            const std::string drive = "auto c = C(); auto h = c.g(); drain(h) + \"#\" + to_string(h.done()) + \"#\" + to_string(c.v)";
            both_drained("class C { int v = 3; coroutine g() -> int { yield v; v = 4; return true; } }\n" + drive, "3|1#true#4", "coroutine g() -> int method");
            both_drained("class C { int v = 3; coroutine g() -> auto { yield v; v = 4; return true; } }\n" + drive, "3|true#true#4", "coroutine g() -> auto method");
        });

        // A yield with no resume value evaluates to null; every expression position must
        // resume at the yield point without re-running what came before it.
        test("yield_positions_expressions", [this, both_drained]() {
            both_drained("coroutine int g() { var t = ((yield 1) == null ? 10 : 0) + 1; yield t; var u = 1 + ((yield 2) == null ? 20 : 0); return u; }\nauto h = g(); drain(h)",
                "1|11|2|21", "yield as the left and the right binary operand");
            both_drained("coroutine int g() { var t = (yield 1) ? 0 : 5; yield t; var w = (yield 2) == null ? (yield 3) : 0; return w == null ? 4 : 0; }\nauto h = g(); drain(h)",
                "1|5|2|3|4", "yield in a ternary condition and in a ternary branch");
            both_drained("coroutine int g() { var arr = [7, 8]; yield arr[(yield 1) == null ? 1 : 0]; var brr = [9]; yield ((yield 2) == null ? brr : arr)[0]; return 0; }\nauto h = g(); drain(h)",
                "1|8|2|9|0", "yield inside an index and inside an index base");
            both_drained("class P { int v = 0; P(int x) { v = x; } }\ncoroutine int g() { var p = P(3); var q = P(4); yield ((yield 1) == null ? p : q).v; yield ((yield 2) == null ? q : p).v; return 0; }\nauto h = g(); drain(h)",
                "1|3|2|4|0", "yield inside a member-chain receiver");
            both_drained("coroutine int g() { var a = [(yield 1), 7]; var m = {\"k\": (yield 2), \"j\": 5}; yield a.size() * 100 + (a[0] == null ? 10 : 0) + (m[\"k\"] == null ? 1 : 0) + a[1] + m[\"j\"]; return 0; }\nauto h = g(); drain(h)",
                "1|2|223|0", "yield inside array and map literals");
            both_drained("coroutine int g() { var x = 5; x = yield 1; var n = 10; n += (yield 2) == null ? 5 : 0; yield (x == null ? 100 : 0) + n; return 0; }\nauto h = g(); drain(h)",
                "1|2|115|0", "yield as an assignment rhs and inside a compound rhs");
            both_drained("coroutine int g() { yield 1; return (yield 2) == null ? 42 : 0; }\nauto h = g(); drain(h)",
                "1|2|42", "yield inside a return expression");
            both_drained("coroutine int g() { if ((yield 1) == null) { yield 10; } else { yield 20; } return 0; }\nauto h = g(); drain(h)",
                "1|10|0", "yield in an if condition");
            both_drained("coroutine int g() { int i = 0; while ((yield i) == null && i < 2) { i++; } return i; }\nauto h = g(); drain(h)",
                "0|1|2|2", "yield in a while condition");
            both_drained("coroutine int g() { int c = 0; for (int i = 0; (yield i) == null && i < 2; i++) { c++; } return c; }\nauto h = g(); drain(h)",
                "0|1|2|2", "yield in a for condition");
            both_drained("coroutine int g() { int s = 0; for (auto x : ((yield 1) == null ? [1, 2, 3] : [9])) { s += x; } return s; }\nauto h = g(); drain(h)",
                "1|6", "yield in a range-for source");
            both_drained("coroutine int g() { var t = ((yield 1) == null ? 10 : 0) + ((yield 2) == null ? 1 : 0); return t; }\nauto h = g(); drain(h)",
                "1|2|11", "two yields in one expression");
        });

        test("yield_in_call_arguments", [this, both_drained, check_parity]() {
            both_drained("int add(int a, int b) { return a + b; }\ncoroutine int g() { yield add(10, (yield 1) == null ? 5 : 0); yield add(add(1, (yield 2) == null ? 2 : 0), 3); return 0; }\nauto h = g(); drain(h)",
                "1|15|2|6|0", "yield inside a free call's argument, flat and nested");
            both_drained("class Acc { int t = 0; int add(int v) { t += v; return t; } }\ncoroutine int g() { var acc = Acc(); yield acc.add((yield 1) == null ? 5 : 0); yield acc.t; return 0; }\nauto h = g(); drain(h)",
                "1|5|5|0", "yield inside a method call's argument");
            both_drained("class P { int v = 0; P(int x) { v = x; } }\ncoroutine int g() { var p = P((yield 1) == null ? 7 : 0); yield p.v; return 0; }\nauto h = g(); drain(h)",
                "1|7|0", "yield inside a constructor argument");
            both_drained("class B { int tick(int x) { return x * 2; } }\nclass D : B { coroutine int g() { yield super::tick((yield 1) == null ? 3 : 0); return 0; } }\nauto d = D(); auto h = d.g(); drain(h)",
                "1|6|0", "yield inside a super method call's argument");
            both_drained("coroutine var g() { yield to_string((yield 1) == null ? 4 : 0) + \"x\"; return 0; }\nauto h = g(); drain(h)",
                "1|4x|0", "yield inside a host call's argument");
            both_drained("coroutine int inner() { yield 1; return 2; }\ncoroutine int g() { auto i = inner(); yield i.next() + ((yield 10) == null ? 100 : 0); return 0; }\nauto h = g(); drain(h)",
                "10|101|0", "another handle's resume as the sibling operand of a yield runs once");
            // constructors cannot be coroutines: `super(yield ...)` is rejected at parse time on both
            // parity pin: literal to be sharpened once the behavior is ruled
            std::string text = check_parity("class B { B(int x) { } }\nclass D : B { D() : super(yield 1) { } }\n1", "yield inside super(...) delegation");
            check_true(text.find("ERROR") == 0, "yield in a ctor initializer is rejected: " + text);
        });

        test("yield_control_flow", [this, both_drained]() {
            both_drained("coroutine int g() { try { yield 1; throw \"boom\"; } catch (e) { yield 2; } yield 3; return 0; }\nauto h = g(); drain(h)",
                "1|2|3|0", "yields inside try and inside catch");
            both_drained("coroutine int g() { yield 1; throw \"boom\"; }\nauto h = g(); var a = h.next(); string c = \"\"; try { h.next(); } catch (e) { c = to_string(e); } to_string(a) + \"|\" + c + \"|\" + to_string(h.done())",
                "1|boom|true", "a throw after a yield reaches the resumer; the handle is done");
            both_drained("coroutine int g(int k) { switch (k) { case 1: yield 10; yield 11; case 2: yield 20; fallthrough; default: yield 30; } return 0; }\ndrain(g(1)) + \"#\" + drain(g(2)) + \"#\" + drain(g(3))",
                "10|11|0#20|30|0#30|0", "yields inside switch cases, break-by-default and fallthrough");
            both_drained("coroutine int g(bool early) { yield 1; if (early) { return 99; } yield 2; return 3; }\ndrain(g(true)) + \"#\" + drain(g(false))",
                "1|99#1|2|3", "early return");
            both_drained("coroutine int g() { yield 1; }\nauto h = g(); drain(h) + \"#\" + to_string(h.done())",
                "1|null#true", "falling off the end completes with null");
            both_drained("coroutine int g() { return 5; }\nauto h = g(); var a = h.next(); string t = \"\"; try { h.next(); } catch (e) { t = to_string(e); } to_string(a) + \"|\" + t + \"|\" + to_string(h.done())",
                "5|Cannot resume a completed coroutine|true", "resume after completion");
            both_drained("coroutine int g() { throw \"x\"; }\nauto h = g(); string t = \"\"; try { h.next(); } catch (e) { } try { h.next(); } catch (e) { t = to_string(e); } t + \"|\" + to_string(h.done())",
                "Cannot resume a completed coroutine|true", "resume after failure");
            both_drained("var self = null;\ncoroutine var g() { string t = \"\"; try { self.next(); } catch (e) { t = to_string(e); } yield t; return 0; }\nself = g(); to_string(self.next())",
                "Coroutine is already running", "a coroutine resuming itself");
            both_drained("coroutine int g() { for (int i = 0; i < 2; i++) { yield i; } int j = 0; while (j < 2) { yield 10 + j; j++; } for (auto x : [20, 21]) { yield x; } return 0; }\nauto h = g(); drain(h)",
                "0|1|10|11|20|21|0", "yields per iteration in for, while and range-for");
        });

        test("nested_generators_and_resume_depth", [this, both_drained]() {
            both_drained("coroutine int chain(int d) { if (d == 0) { yield 0; return 0; } auto h = chain(d - 1); yield h.next() + 1; return h.next() + 1; }\nauto h = chain(40); to_string(h.next()) + \"|\" + to_string(h.next()) + \"|\" + to_string(h.done())",
                "40|40|true", "a 40-deep chain of coroutines each resuming the next");
            both_drained("coroutine int inner(int n) { for (int i = 0; i < n; i++) { yield i; } return -1; }\ncoroutine int outer() { int s = 0; for (auto x : inner(3)) { s += x; yield s; } auto h = inner(2); while (!h.done()) { var v = h.next(); if (!h.done()) { yield v * 100; } } return s; }\nauto h = outer(); drain(h)",
                "0|1|3|0|100|3", "a generator driving another by range-for and by manual resume");
            both_drained("class G { int base = 10; coroutine int gen() { yield base; yield base + 1; return 0; } }\ncoroutine int drive() { auto g = G(); auto h = g.gen(); yield h.next(); g.base = 20; yield h.next(); return 0; }\nauto h = drive(); drain(h)",
                "10|21|0", "a method coroutine resumed from inside a coroutine sees live fields");
        });

        test("captured_locals_across_yields", [this, both_drained]() {
            both_drained("coroutine int g() { int v = 1; auto by_val = [=]() { return v; }; auto by_ref = [&]() { return v; }; yield by_val() * 10 + by_ref(); v = 5; yield by_val() * 10 + by_ref(); return 0; }\nauto h = g(); drain(h)",
                "11|15|0", "[=] snapshots and [&] shares a local across yields");
            both_drained("coroutine int g() { int v = 1; auto bump = [&]() { v += 1; return v; }; yield bump(); yield bump(); return v; }\nauto h = g(); drain(h)",
                "2|3|3", "a [&] lambda mutating a local between yields");
            both_drained("int shared = 0;\ncoroutine int g(int r) { r += 1; shared += 1; yield r; r += 1; shared += 1; return r; }\nint x = 5; auto h = g(x); string s = drain(h); s + \"|\" + to_string(x) + \"|\" + to_string(shared)",
                "6|7|5|2", "a by-value parameter is the coroutine's own; globals stay live");
        });

        test("typed_completion_values", [this, both_drained]() {
            both_drained("coroutine float g() { yield 1; return 2; }\nauto h = g(); var a = h.next(); var b = h.next(); type_of(a) + \":\" + to_string(a) + \"|\" + type_of(b) + \":\" + to_string(b)",
                "float:1.0|float:2.0", "the yields and the completion value convert to the declared type");
            both_drained("coroutine int g() { yield 1; var f = 2.7; return f; }\nauto h = g(); h.next(); string t = \"\"; try { h.next(); } catch (e) { t = to_string(e); } t + \"|\" + to_string(h.done())",
                "Cannot return float from 'g' (declared -> int)|true", "a float never completes an int coroutine");
            both_drained("coroutine string g() { yield \"a\"; var n = 2; return n; }\nauto h = g(); h.next(); string t = \"\"; try { h.next(); } catch (e) { t = to_string(e); } t",
                "Cannot return int from 'g' (declared -> string)", "a string completion takes text only");
            both_drained("coroutine int g() { yield 1; var x = \"s\"; return x; }\nauto h = g(); h.next(); string t = \"\"; try { h.next(); } catch (e) { t = to_string(e); } t + \"|\" + to_string(h.done())",
                "Cannot return string from 'g' (declared -> int)|true", "a var-sourced wrong completion type raises the same text; the handle is done");
            both_drained("class C { coroutine int g() { yield 1; var x = [1]; return x; } }\nauto h = C().g(); h.next(); string t = \"\"; try { h.next(); } catch (e) { t = to_string(e); } t",
                "Cannot return array from 'g' (declared -> int)", "method coroutine completion mismatch text");
        });

        test("yields_take_the_declared_type", [this, both_drained]() {
            // a yield hands out the declared type like a return: the slot table converts, a mismatch raises the yield's text
            both_drained("coroutine int g() { yield true; var f = 2.9; yield f; return 0; }\nauto h = g(); var a = h.next(); string t = \"\"; try { h.next(); } catch (e) { t = to_string(e); } type_of(a) + \":\" + to_string(a) + \"|\" + t",
                "int:1|Cannot yield float from 'g' (declared -> int)", "a bool converts into int, a float never does");
            both_drained("coroutine string g() { yield 1; return \"x\"; }\nauto h = g(); string t = \"\"; try { h.next(); } catch (e) { t = to_string(e); } t",
                "Cannot yield int from 'g' (declared -> string)", "an int never converts into string");
            both_drained("coroutine float g() { yield 1; yield 2; return 0.0; }\nstring s = \"\"; for (auto v : g()) { s += type_of(v); } s",
                "floatfloat", "a range-for drive sees the converted values");
            both_drained("coroutine int g() { yield \"s\"; yield 2; return 0; }\nauto h = g(); string t = \"\"; try { h.next(); } catch (e) { t = to_string(e); } t + \"|\" + to_string(h.done())",
                "Cannot yield string from 'g' (declared -> int)|true", "a mismatch raises at the resume; the handle is done");
            both_drained("coroutine int g() { var s = \"s\"; try { yield s; } catch (e) { yield 7; } return 0; }\nauto h = g(); drain(h)",
                "7|0", "a catch around the yield sees its failure");
            both_drained("class C { coroutine int g() { yield [1]; return 0; } }\nauto h = C().g(); string t = \"\"; try { h.next(); } catch (e) { t = to_string(e); } t",
                "Cannot yield array from 'g' (declared -> int)", "a method coroutine's mismatch text");
            both_drained("coroutine var g() { yield \"s\"; yield 2.9; return 0; }\nauto h = g(); drain(h)",
                "s|2.9|0", "a var coroutine yields as is");
        });

        test("range_for_break_leaves_handle_suspended", [this, both_drained]() {
            both_drained("coroutine int g() { yield 1; yield 2; yield 3; return 0; }\nauto h = g(); int seen = 0; for (auto v : h) { seen = v; if (v == 2) { break; } } to_string(seen) + \"|\" + to_string(h.done()) + \"|\" + to_string(h.next()) + \"|\" + to_string(h.next()) + \"|\" + to_string(h.done())",
                "2|false|3|0|true", "break mid-drive leaves the handle suspended at the last yield");
            both_drained("coroutine int g() { yield 1; yield 2; return 0; }\nauto h = g(); int n = 0; for (auto v : h) { n += v; } string t = \"\"; try { h.next(); } catch (e) { t = to_string(e); } to_string(n) + \"|\" + to_string(h.done()) + \"|\" + t",
                "3|true|Cannot resume a completed coroutine", "a full drive completes the handle; the completion value is not an element");
        });

        // Hot reload while a method coroutine is suspended MID range-for drive: the old body
        // finishes over the migrated instance (guide 07/08), fresh calls mint the new body.
        test("hot_reload_mid_iteration_keeps_suspended_body", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execution_budget(0);
                jai::stdlib::register_all(e);
                e->execute("class W { int v = 1; coroutine int gen() { for (int i = 0; i < 4; i++) { yield v + i; } return 0; } }\nvar w = W();\nw.v = 10;\nvar h = w.gen();\nvar first = h.next();");
                e->execute("class W { int v = 1; string extra = \"e\"; coroutine int gen() { yield 99; return 0; } }");
                auto r = e->execute("string s = to_string(first);\nfor (auto x : h) { s += \",\" + to_string(x); }\ns + \"|\" + w.extra + \"|\" + to_string(w.gen().next())");
                check_eq(std::string("10,11,12,13|e|99"), r.to_string(), std::string(use_vm ? "vm" : "interp") + ": mid-iteration reload keeps the old body on the migrated instance");
            }
        });

        test("coroutines_and_parallel_bodies", [this, both_drained, check_parity]() {
            // parity pin: literal to be sharpened once the behavior is ruled
            std::string r = check_parity("coroutine int gen(int x) { yield x; return x; }\nint f(int x) { auto h = gen(x); return h.next(); }\nstring r = \"no-error\";\ntry { parallel_transform([1, 2, 3], f); } catch (e) { r = to_string(e); }\nr",
                "a parallel body that mints and resumes a coroutine");
            check_true(r != "no-error", "no yield reaches a parallel body (docs/site/guide/09-containers-strings.html \"Parallel loops\"): " + r);
            both_drained("int dbl(int x) { return x * 2; }\ncoroutine int g() { yield 1; var r = parallel_transform([1, 2, 3], dbl); yield r[0] + r[1] + r[2]; return 0; }\nauto h = g(); drain(h)",
                "1|12|0", "a parallel_transform between two yields of a coroutine");
        });

        test("budget_exhaustion_inside_coroutine_text_parity", [this]() {
            std::string out[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execution_budget(0.05);
                try {
                    e->execute("coroutine int spin() { yield 1; while (true) { } return 0; }\nauto h = spin();\nh.next();\nstring t = \"\";\ntry { h.next(); } catch (e) { t = \"caught\"; }\nt");
                    out[idx] = "OK";
                } catch (const std::exception& ex) { out[idx] = ex.what(); }
                check_true(out[idx].find("Script execution budget exceeded") != std::string::npos,
                           std::string(use_vm ? "vm" : "interp") + ": the budget error is terminal inside a resumed coroutine (got: " + out[idx] + ")");
                check_eq(static_cast<script_int>(4), e->execute("2 + 2").as<script_int>());
                ++idx;
            }
            check_eq(out[0], out[1], "budget text parity across backends");
        });

        test("resumed_coroutine_error_trace_parity", [this]() {
            auto run_both = [](const char* src) {
                std::string out[2];
                int idx = 0;
                for (bool use_vm : {false, true}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    try { e->execute(src); out[idx] = "OK"; }
                    catch (const std::exception& ex) { out[idx] = std::string(ex.what()) + "\n" + e->format_stack_trace(); }
                    ++idx;
                }
                return std::make_pair(out[0], out[1]);
            };
            {
                auto [i_out, v_out] = run_both(
                    "class C {\n"                       // 1
                    "  int v = 3;\n"                    // 2
                    "  coroutine int gen() {\n"         // 3
                    "    yield v;\n"                    // 4
                    "    throw \"boom\";\n"             // 5
                    "  }\n"                             // 6
                    "}\n"                               // 7
                    "auto c = C();\n"                   // 8
                    "auto h = c.gen();\n"               // 9
                    "var a = h.next();\n"             // 10
                    "var b = h.next();\n");           // 11
                const std::string expected = "boom\n  at gen (<script>:5)\n  at <script> (<script>:11)\n";
                check_eq(expected, i_out, "interp: method coroutine throw attributed to the coroutine frame");
                check_eq(expected, v_out, "vm: method coroutine throw attributed to the coroutine frame");
            }
            {
                // parity pin: literal to be sharpened once the behavior is ruled
                auto [i_out, v_out] = run_both(
                    "coroutine int inner() {\n"         // 1
                    "  yield 1;\n"                      // 2
                    "  throw \"deep\";\n"               // 3
                    "}\n"                               // 4
                    "coroutine int outer() {\n"         // 5
                    "  auto h = inner();\n"             // 6
                    "  yield h.next();\n"             // 7
                    "  yield h.next();\n"             // 8
                    "  return 0;\n"                     // 9
                    "}\n"                               // 10
                    "auto o = outer();\n"               // 11
                    "var a = o.next();\n"             // 12
                    "var b = o.next();\n");           // 13
                check_eq(i_out, v_out, "nested coroutine throw: trace parity");
                check_true(i_out.find("deep\n") == 0, "text: " + i_out);
                check_true(i_out.find("at inner (<script>:3)") != std::string::npos, "inner frame: " + i_out);
                check_true(i_out.find("at outer (<script>:8)") != std::string::npos, "outer frame: " + i_out);
                check_true(i_out.find("at inner") < i_out.find("at outer") && i_out.find("at outer") < i_out.find("at <script>"), "frame order: " + i_out);
            }
        });

        // ===== YIELD INSIDE AN EXPRESSION (both backends; the vm's fibers are the oracle) =====
        // A yield suspends AT the yield point: nothing later in evaluation order runs before
        // the next resume(), the resume value is null, and everything evaluated before the
        // yield is reused - never re-evaluated.
        test("yield_in_free_call_argument", [this, both_drained]() {
            both_drained("int add2(int a) { return a + 2; }\ncoroutine int gen() { int r = add2((yield 1) ? 0 : 5); return r; }\nauto h = gen(); drain(h)",
                "1|7", "the call runs only after the resume; the yield is null");
            both_drained("int add2(int a, int b) { return a + b; }\ncoroutine int gen() { return add2((yield 1) == null ? 10 : 0, 5); }\nauto h = gen(); drain(h)",
                "1|15", "a yield in the first of two arguments");
            both_drained("int add2(int a, int b) { return a + b; }\ncoroutine int gen() { return add2(5, (yield 1) == null ? 10 : 0); }\nauto h = gen(); drain(h)",
                "1|15", "a yield in the last argument");
        });

        test("yield_as_plain_argument_raises_on_resume", [this, both_drained]() {
            // add2(yield 1, 2): the resume substitutes null, and the typed parameter rejects it
            both_drained("int add2(int a, int b) { return a + b; }\ncoroutine int gen() { return add2(yield 1, 2); }\n"
                         "auto h = gen(); var a = h.next(); string t = \"\"; try { h.next(); } catch (e) { t = to_string(e); } to_string(a) + \"|\" + t + \"|\" + to_string(h.done())",
                "1|Cannot pass null to parameter 'a' of 'add2' (declared int)|true",
                "a bare yield argument fails the typed parameter on resume; the handle is done");
        });

        test("yield_in_method_and_ctor_arguments", [this, both_drained]() {
            both_drained("class Acc { int t = 0; int add(int v) { t += v; return t; } }\ncoroutine int gen() { auto acc = Acc(); int r = acc.add((yield 1) ? 0 : 5); return r; }\nauto h = gen(); drain(h)",
                "1|5", "yield inside a method call argument");
            both_drained("class Box { int v = 0; Box(int x) { v = x; } }\ncoroutine int gen() { auto b = Box((yield 1) == null ? 4 : 0); return b.v; }\nauto h = gen(); drain(h)",
                "1|4", "yield inside a constructor argument");
            both_drained("coroutine int gen() { string s = \"abc\"; return s.starts_with((yield 1) == null ? \"ab\" : \"zz\") ? 7 : 0; }\nauto h = gen(); drain(h)",
                "1|7", "yield inside a string builtin's argument");
            both_drained("coroutine int gen() { return math::max((yield 1) == null ? 3 : 0, 2); }\nauto h = gen(); drain(h)",
                "1|3", "yield inside a math:: intrinsic argument");
            both_drained("class B { int tick(int x) { return x * 2; } }\nclass D : B { coroutine int g() { return super::tick((yield 1) == null ? 3 : 0); } }\nauto h = D().g(); drain(h)",
                "1|6", "yield inside a super method call argument");
        });

        test("yield_in_arguments_side_effects_run_once", [this, both_drained]() {
            // The argument before the yield runs in the FIRST resume, the call itself in the second
            both_drained("string log = \"\";\nint side() { log += \"S\"; return 1; }\nint f2(int a, int b) { log += \"F\"; return a + b; }\n"
                         "coroutine int gen() { int r = f2(side(), (yield 1) == null ? 5 : 0); return r; }\n"
                         "auto h = gen(); string s = \"\"; while (!h.done()) { log += \"|\"; var v = h.next(); if (s != \"\") { s += \"|\"; } s += to_string(v); } log + \"#\" + s",
                "|S|F#1|6", "side() runs once, before the suspension; the call runs after it");
            both_drained("string log = \"\";\nint inner(var x) { log += \"I\"; return x == null ? 1 : 0; }\nint outer(int a, int b) { log += \"O\"; return a * 10 + b; }\n"
                         "coroutine int gen() { return outer(inner(yield 1), (yield 2) == null ? 1 : 0); }\n"
                         "auto h = gen(); string s = \"\"; while (!h.done()) { log += \"|\"; var v = h.next(); if (s != \"\") { s += \"|\"; } s += to_string(v); } log + \"#\" + s",
                "||I|O#1|2|11", "nested inner/outer argument yields: each callee runs exactly once");
            both_drained("string log = \"\";\ncoroutine int gen() { if ((yield 1) == null) { log += \"A\"; } return 0; }\n"
                         "auto h = gen(); while (!h.done()) { log += \"|\"; h.next(); } log",
                "||A", "the branch runs in the resume that follows the yield, not before it");
        });

        test("yield_with_a_callee_resolved_before_arguments", [this, both_drained]() {
            both_drained("string log = \"\";\nint side() { log += \"S\"; return 1; }\nint f2(int a, int b) { log += \"F\"; return a + b; }\n"
                         "coroutine int gen() { return f2(side(), (yield 1) == null ? 5 : 0); }\n"
                         "auto h = gen(); string s = \"\"; while (!h.done()) { log += \"|\"; var v = h.next(); if (s != \"\") { s += \"|\"; } s += to_string(v); } log + \"#\" + s",
                "|S|F#1|6", "the callee-first probe shape with a yield in the args");
            both_drained("int orig(int x) { return 100 + x; }\nint repl(int x) { return 200 + x; }\nvar fp = orig;\n"
                         "coroutine int gen() { return fp((yield 1) == null ? 5 : 0); }\n"
                         "auto h = gen(); var a = h.next(); fp = repl; var b = h.next(); to_string(a) + \"|\" + to_string(b)",
                "1|105", "the callee parked before the yield is the one invoked");
            both_drained("coroutine int inner() { yield 1; return 2; }\ncoroutine int gen() { var hi = inner(); try { hi.done((yield 1) == null ? 0 : 0); } catch (e) { if (e == \"done() takes no arguments\") { return 3; } throw e; } return 4; }\n"
                         "auto h = gen(); var a = h.next(); var b = h.next(); to_string(a) + \"|\" + to_string(b) + \"|\" + to_string(h.done())",
                "1|3|true", "a builtin handle method with a yield in its argument list runs after the resume");
        });

        test("yield_in_method_receiver_expression", [this, both_drained]() {
            both_drained("class P { int v = 0; P(int x) { v = x; } int get() { return v; } }\ncoroutine int gen() { auto b = P(3); return ((yield 1) == null ? b : b).get(); }\nauto h = gen(); drain(h)",
                "1|3", "yield inside a method-call receiver expression");
            both_drained("string log = \"\";\nclass P { int v = 0; P(int x) { v = x; } int get() { return v; } }\nP pick(var t) { log += \"P\"; return P(9); }\n"
                         "coroutine int gen() { return pick(yield 1).get(); }\n"
                         "auto h = gen(); string s = \"\"; while (!h.done()) { log += \"|\"; var v = h.next(); if (s != \"\") { s += \"|\"; } s += to_string(v); } log + \"#\" + s",
                "||P#1|9", "the receiver call runs once, after the resume");
        });

        test("yield_in_operand_positions", [this, both_drained]() {
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { return 100 + nz(yield 1); }\nauto h = gen(); drain(h)",
                "1|101", "yield in a right binary operand under a call");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { return nz(yield 1) + 200; }\nauto h = gen(); drain(h)",
                "1|201", "yield in a left binary operand under a call");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { return nz(yield 1) * 10 + nz(yield 2) * 20; }\nauto h = gen(); drain(h)",
                "1|2|30", "two yields in one expression, each consumed once");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { return -nz(yield 1); }\nauto h = gen(); drain(h)",
                "1|-1", "yield under a unary operand");
            both_drained("string log = \"\";\nbool logged() { log += \"L\"; return true; }\ncoroutine int gen() { bool b = logged() && (yield 1) == null; return b ? 5 : 0; }\n"
                         "auto h = gen(); string s = \"\"; while (!h.done()) { log += \"|\"; var v = h.next(); if (s != \"\") { s += \"|\"; } s += to_string(v); } log + \"#\" + s",
                "|L|#1|5", "&& short-circuit: the left operand runs once, before the suspension");
            both_drained("string log = \"\";\nbool logged() { log += \"L\"; return false; }\ncoroutine int gen() { bool b = logged() || (yield 1) == null; return b ? 5 : 0; }\n"
                         "auto h = gen(); string s = \"\"; while (!h.done()) { log += \"|\"; var v = h.next(); if (s != \"\") { s += \"|\"; } s += to_string(v); } log + \"#\" + s",
                "|L|#1|5", "|| short-circuit twin");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { var arr = [7, 8]; return arr[nz(yield 1)]; }\nauto h = gen(); drain(h)",
                "1|8", "yield inside a subscript index");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\narray<int> pick_arr(var t) { return [40, 41]; }\ncoroutine int gen() { return pick_arr(yield 1)[nz(yield 2)]; }\nauto h = gen(); drain(h)",
                "1|2|41", "yields in an index base and its index");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { return cast<int>(nz(yield 1)) * 42; }\nauto h = gen(); drain(h)",
                "1|42", "yield inside a cast operand");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { string t = \"\"; try { throw nz(yield 1) * 9; } catch (e) { t = to_string(e); } return t == \"9\" ? 5 : 0; }\nauto h = gen(); drain(h)",
                "1|5", "yield inside a throw operand");
            both_drained("coroutine var gen() { var v = yield (yield 1); return v == null ? 7 : 0; }\nauto h = gen(); drain(h)",
                "1|null|7", "a yield nested inside another yield operand");
        });

        test("yield_in_literals_and_stores", [this, both_drained]() {
            both_drained("string log = \"\";\nint one() { log += \"1\"; return 1; }\nint nz(var v) { return v == null ? 5 : 0; }\n"
                         "coroutine int gen() { var a = [one(), nz(yield 1)]; return a[0] * 10 + a[1]; }\n"
                         "auto h = gen(); string s = \"\"; while (!h.done()) { log += \"|\"; var v = h.next(); if (s != \"\") { s += \"|\"; } s += to_string(v); } log + \"#\" + s",
                "|1|#1|15", "an array literal element after a logged element: each runs once");
            both_drained("int nz(var v) { return v == null ? 5 : 0; }\ncoroutine int gen() { var m = {\"k\": nz(yield 1), \"j\": 2}; return m[\"k\"] * 10 + m[\"j\"]; }\nauto h = gen(); drain(h)",
                "1|52", "yield inside a map literal value");
            both_drained("int nz(var v) { return v == null ? 42 : 0; }\ncoroutine int gen() { int a = 0; a = nz(yield 1); return a; }\nauto h = gen(); drain(h)",
                "1|42", "yield in an assignment rhs");
            both_drained("int nz(var v) { return v == null ? 42 : 0; }\ncoroutine int gen() { int a = 8; a += nz(yield 1); return a; }\nauto h = gen(); drain(h)",
                "1|50", "yield in a compound assignment rhs");
            both_drained("string log = \"\";\nint side() { log += \"S\"; return 0; }\nint nz(var v) { return v == null ? 6 : 0; }\n"
                         "coroutine int gen() { var arr = [1, 2]; arr[side()] = nz(yield 1); return arr[0]; }\n"
                         "auto h = gen(); string s = \"\"; while (!h.done()) { log += \"|\"; var v = h.next(); if (s != \"\") { s += \"|\"; } s += to_string(v); } log + \"#\" + s",
                "||S#1|6", "the store target subexpression is evaluated once, after the resume");
            both_drained("class C { int v = 1; }\nint nz(var v) { return v == null ? 4 : 0; }\ncoroutine int gen() { auto c = C(); c.v += nz(yield 1); return c.v; }\nauto h = gen(); drain(h)",
                "1|5", "yield in a member compound assignment rhs");
            both_drained("int nz(var v) { return v == null ? 42 : 0; }\ncoroutine int gen() { return nz(yield 1); }\nauto h = gen(); drain(h)",
                "1|42", "yield inside a return expression");
            both_drained("int nz(var v) { return v == null ? 3 : 0; }\ncoroutine int gen() { int x = nz(yield 1); return x; }\nauto h = gen(); drain(h)",
                "1|3", "yield inside a declaration initializer");
        });

        test("yield_in_conditions_and_range_for", [this, both_drained]() {
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { if (nz(yield 1) == 1) { return 7; } return 0; }\nauto h = gen(); drain(h)",
                "1|7", "yield inside a call in an if condition");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { int c = 0; while (nz(yield c) == 1 && c < 2) { c++; } return c; }\nauto h = gen(); drain(h)",
                "0|1|2|2", "yield inside a call in a while condition");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { int c = 0; for (int i = 0; nz(yield i) == 1 && i < 2; i++) { c++; } return c; }\nauto h = gen(); drain(h)",
                "0|1|2|2", "yield inside a call in a for condition");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { int t = 0; for (int i = 0; i < 2; i += nz(yield i)) { t += 10; } return t; }\nauto h = gen(); drain(h)",
                "0|1|20", "yield inside a for update expression");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { int t = 0; for (auto x : [nz(yield 1), 2]) { t += x; } return t; }\nauto h = gen(); drain(h)",
                "1|3", "yield inside a range-for source");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { int t = 0; for (auto x : [1, 2, 3]) { t += x * nz(yield x); } return t; }\nauto h = gen(); drain(h)",
                "1|2|3|6", "a yield inside a call in a range-for body, once per element");
            both_drained("int nz(var v) { return v == null ? 2 : 0; }\ncoroutine int inner() { yield nz(yield 1); return 9; }\n"
                         "coroutine int outer() { int t = 0; for (auto v : inner()) { t = t * 10 + v; } return t; }\nauto h = outer(); drain(h)",
                "12", "range-for over a generator whose yields sit in argument positions");
        });

        test("yield_in_switch_and_catch_replays_once", [this, both_drained]() {
            both_drained("string log = \"\";\ncoroutine int gen() { try { log += \"T\"; throw \"x\"; } catch (e) { log += \"C\"; yield 1; log += \"c\"; } return 0; }\n"
                         "auto h = gen(); while (!h.done()) { h.next(); log += \"|\"; } log",
                "TC|c|", "a yield in a catch block does not re-run the try body on resume");
            both_drained("string log = \"\";\nint cond() { log += \"S\"; return 1; }\nint cv() { log += \"V\"; return 1; }\n"
                         "coroutine int gen() { switch (cond()) { case 1: log += \"B\"; yield 1; log += \"b\"; } return 0; }\n"
                         "auto h = gen(); while (!h.done()) { h.next(); log += \"|\"; } log",
                "SB|b|", "a yield in a switch case does not re-evaluate the condition or case values");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { switch (nz(yield 1)) { case 1: return 5; default: return 9; } }\nauto h = gen(); drain(h)",
                "1|5", "yield inside a switch condition");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int gen() { switch (1) { case nz(yield 1): return 5; default: return 9; } }\nauto h = gen(); drain(h)",
                "1|5", "yield inside a case value");
        });

        test("yield_typed_slot_and_lambda_rules_unchanged", [this, both_drained, check_parity]() {
            both_drained("coroutine int gen() { int x = yield 1; return x; }\n"
                         "auto h = gen(); var a = h.next(); string t = \"\"; try { h.next(); } catch (e) { t = to_string(e); } to_string(a) + \"|\" + t",
                "1|Cannot initialize 'x' (declared int) with null", "a null resume value into a typed slot still raises the same text");
            std::string r = check_parity("coroutine int gen() { auto f = [=]() { yield 1; return 2; }; return f(); }\n1", "yield inside a lambda in a coroutine");
            check_true(r.find("yield can only be used inside a coroutine function") != std::string::npos,
                       "yield in a lambda body is a parse error on both backends: " + r);
            std::string r2 = check_parity("coroutine int gen() { int nested() { yield 1; return 2; } return nested(); }\n1", "yield inside a nested function in a coroutine");
            check_true(r2.find("yield can only be used inside a coroutine function") != std::string::npos,
                       "yield in a nested function body is a parse error on both backends: " + r2);
        });

        test("static_member_store_rhs_runs_once", [this, both_drained]() {
            // the interpreter re-dispatched the rhs for a static target (side effects twice)
            both_drained("class C { static int s = 0; }\nstring log = \"\";\nint side() { log += \"S\"; return 4; }\n"
                         "coroutine int gen() { C::s = side(); yield C::s; return 0; }\n"
                         "auto h = gen(); string s = drain(h); s + \"#\" + log",
                "4|0#S", "a static member store evaluates its rhs exactly once");
        });

        test("handle_type_of_and_to_string_parity", [this, check_parity]() {
            std::string r = check_parity("coroutine int g() { yield 1; }\nauto h = g(); auto h2 = h;\ntype_of(h) + \"|\" + to_string(to_string(h) != \"\") + \"|\" + to_string(h == null) + \"|\" + to_string(h == h2)",
                "type_of / to_string / equality of a coroutine handle");
            check_eq(std::string("coroutine_handle|true|false|true"), r, "a handle's type_of is the name its error texts use");
        });

        // A brace-less body that completes AT the yield leaves no inner frame: the enclosing
        // if/while/for/range-for must step past it on resume, never re-run it
        test("yield_in_brace_less_bodies", [this, both_drained]() {
            both_drained("coroutine int g() { if (true) yield 1; yield 2; return 3; }\nauto h = g(); drain(h)",
                "1|2|3", "brace-less if body");
            both_drained("coroutine int g() { if (false) yield 1; else yield 2; yield 3; return 4; }\nauto h = g(); drain(h)",
                "2|3|4", "brace-less else body");
            both_drained("coroutine int g() { int i = 0; while (i < 3) yield i++; return 9; }\nauto h = g(); drain(h)",
                "0|1|2|9", "brace-less while body");
            both_drained("coroutine int g() { for (int i = 0; i < 3; i++) yield i * 10; return 9; }\nauto h = g(); drain(h)",
                "0|10|20|9", "brace-less for body");
            both_drained("coroutine int g() { for (auto x : [5, 6]) yield x; return 9; }\nauto h = g(); drain(h)",
                "5|6|9", "brace-less range-for body over an array");
            both_drained("coroutine int g() { for (auto kv : {\"a\": 1, \"b\": 2}) yield kv.second; return 9; }\nauto h = g(); drain(h)",
                "1|2|9", "brace-less range-for body over a map");
            both_drained("coroutine int inner() { yield 7; yield 8; return 0; }\ncoroutine int g() { for (auto v : inner()) yield v * 2; return 9; }\nauto h = g(); drain(h)",
                "14|16|9", "brace-less range-for body over a generator");
            both_drained("coroutine int g() { int k = 0; while (k < 2) if (k++ == 0) yield 10; else yield 20; return 4; }\nauto h = g(); drain(h)",
                "10|20|4", "brace-less while body that is a brace-less if/else");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int g() { int i = 0; while (nz(yield i) == 1 && i < 2) i++; return i; }\nauto h = g(); drain(h)",
                "0|1|2|2", "brace-less while body under a condition that yields");
        });

        // Store targets: the receiver / subscript of a store is evaluated exactly once, and a
        // yield inside it suspends there (the rhs, evaluated first, is parked)
        test("yield_in_store_targets_runs_once", [this, both_drained]() {
            both_drained("class P { int v = 0; }\ncoroutine int g() { var p = P(); ((yield 1) == null ? p : p).v = 5; return p.v; }\nauto h = g(); drain(h)",
                "1|5", "yield inside a member-store receiver");
            both_drained("coroutine int g() { var arr = [1, 2]; arr[(yield 1) == null ? 1 : 0] += 5; return arr[1]; }\nauto h = g(); drain(h)",
                "1|7", "yield inside a compound-store subscript");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int g() { var arr = [[1, 2], [3, 4]]; arr[nz(yield 1)][nz(yield 2)] = 9; return arr[1][1]; }\nauto h = g(); drain(h)",
                "1|2|9", "yields inside a nested-subscript store target");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int g() { array<int> a = [1, 2]; a[0] = nz(yield 1) * 7; a[1] += nz(yield 2) * 5; return a[0] + a[1]; }\nauto h = g(); drain(h)",
                "1|2|14", "typed-array element stores with yields in the rhs");
            both_drained("coroutine int g() { var m = {\"k\": 0}; m[\"k\"]++; m[\"k\"] += (yield 1) == null ? 1 : 0; ++m[\"k\"]; return m[\"k\"]; }\nauto h = g(); drain(h)",
                "1|3", "map-entry ++ / compound with a yield in the rhs");
            // the compound-store target of the general path used to be re-dispatched for the
            // store-back (side effects twice on the interpreter): pinned on both backends
            both_drained("string log = \"\";\nint side() { log += \"S\"; return 1; }\nvar arr = [1, 2];\narr[side()] += 5;\nvar m = {\"a\": [1, 2]};\nstring key() { log += \"Y\"; return \"a\"; }\nm[key()][side()] += 5;\nlog + \" \" + to_string(arr[1]) + \" \" + to_string(m[\"a\"][1])",
                "SYS 7 7", "a compound store evaluates its subscript target exactly once");
            // a yield inside a member-compound RECEIVER suspends once and the store lands on
            // the resume (both backends used to yield twice; the interpreter also lost the store)
            both_drained("class P { int v = 0; }\ncoroutine int g() { var p = P(); ((yield 1) == null ? p : p).v += 5; return p.v; }\nauto h = g(); drain(h)",
                "1|5", "yield inside a member-compound receiver runs once");
            both_drained("class P { int v = 0; }\ncoroutine int g() { var p = P(); ((yield 1) == null ? p : p).v++; return p.v; }\nauto h = g(); drain(h)",
                "1|1", "yield inside a member-++ receiver runs once");
            both_drained("class P { int v = 0; }\nint nz(var v) { return v == null ? 1 : 0; }\ncoroutine int g() { var arr = [P(), P()]; arr[nz(yield 1)].v += 5; return arr[1].v; }\nauto h = g(); drain(h)",
                "1|5", "yield inside the index of a member-compound receiver");
            both_drained("class P { int v = 3; }\nint nz(var v) { return v == null ? 1 : 0; }\ncoroutine int g() { var p = P(); ((yield 1) == null ? p : p).v += nz(yield 2) * 4; return p.v; }\nauto h = g(); drain(h)",
                "2|1|7", "yields in both the receiver and the rhs of a member compound: the rhs first (C++17)");
        });

        test("yield_in_method_field_stores", [this, both_drained]() {
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\nclass C { int v = 0; coroutine int g() { v = nz(yield 1) * 10; v += nz(yield 2); this.v += nz(yield 3) * 100; return v; } }\nauto h = C().g(); drain(h)",
                "1|2|3|111", "bare-field, bare-compound and this.field compound stores with yields in the rhs");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int g() { int a = 1; var lam = [&]() { return a * 10; }; a = nz(yield 1) + lam(); return a + lam(); }\nauto h = g(); drain(h)",
                "1|121", "a [&] lambda reads the local after a resumed store");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int g() { var arr = [1, 2, 3]; int t = 0; for (auto& x : arr) { x += nz(yield x); t += x; } return t + arr[0]; }\nauto h = g(); drain(h)",
                "1|2|3|11", "by-reference range-for element stores with yields");
        });

        test("yield_in_nested_control_flow", [this, both_drained]() {
            both_drained("coroutine int g(int k) { switch (k) { case 1: { yield 10; yield 11; } case 2: yield 20; fallthrough; default: { yield 30; yield 31; } } return 0; }\ndrain(g(1)) + \"#\" + drain(g(2)) + \"#\" + drain(g(3))",
                "10|11|0#20|30|31|0#30|31|0", "yields inside nested blocks of switch cases, fallthrough and default");
            both_drained("coroutine int g() { int t = 0; for (int i = 0; i < 3; i++) { try { if (i == 1) { throw \"x\"; } yield i; } catch (e) { t += 100; yield 99; } } return t; }\nauto h = g(); drain(h)",
                "0|99|2|100", "a try/catch with yields in both blocks inside a for loop");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int g() { int c = 0; for (int i = nz(yield 1); i < 3; i++) { c += i; } return c; }\nauto h = g(); drain(h)",
                "1|3", "yield inside a for initializer");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int g() { int t = 0; for (int i = 0; i < 4; i++) { if (nz(yield i) == 1 && i == 1) { continue; } if (i == 2) { break; } t += 10; } return t; }\nauto h = g(); drain(h)",
                "0|1|2|10", "continue and break after a resumed yield in a loop body");
            both_drained("int add(int a, int b) { return a + b; }\nint nz(var v) { return v == null ? 1 : 0; }\ncoroutine int g() { if (add(nz(yield 1), (yield 2) == null ? 1 : 0) == 2) { return 8; } return 0; }\nauto h = g(); drain(h)",
                "1|2|8", "two yields inside a call inside an if condition");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int g() { int s = 0; while (true) { s += nz(yield s); if (s >= 3) { break; } } return s; }\nauto h = g(); drain(h)",
                "0|1|2|3", "compound store with a yield in an infinite loop broken after the resume");
            both_drained("coroutine int f3() { try { yield 1; throw \"a\"; } catch (e) { yield 2; throw \"b\"; } return 0; }\n"
                         "auto h = f3(); string s = to_string(h.next()); s += to_string(h.next()); try { h.next(); } catch (e) { s += \"caught\"; } s + to_string(h.done())",
                "12caughttrue", "a rethrow from a resumed catch block fails the handle");
        });

        test("yield_inside_string_range_for", [this, both_drained]() {
            // a string iterates one char per element by value; the suspended position and the
            // evaluated string ride the continuation like an array's do
            both_drained("coroutine string g() { string t = \"xyz\"; string a = \"\"; for (auto c : t) { yield `${a}${c}`; a += c; } return a; }\nauto h = g(); drain(h)",
                "x|xy|xyz|xyz", "yield per char inside a string range-for");
            both_drained("coroutine int g() { for (auto c : \"ab\") yield c + 0; return 9; }\nauto h = g(); drain(h)",
                "97|98|9", "brace-less range-for body over a string literal");
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\ncoroutine int g() { int t = 0; for (auto c : \"abcd\") { t += nz(yield c + 0); if (c == 'b') { break; } } return t; }\nauto h = g(); drain(h)",
                "97|98|2", "break after a resumed yield mid-string");
            both_drained("coroutine int g() { int n = 0; for (auto c : \"\") { yield 1; n++; } for (auto c : \"q\") { yield c + 0; n++; } return n; }\nauto h = g(); drain(h)",
                "113|1", "an empty string yields nothing; the next loop runs");
            both_drained("coroutine string g() { string s = \"ab\"; string r = \"\"; try { for (auto& c : s) { yield \"never\"; } } catch (e) { r = e; } yield r; return s; }\nauto h = g(); drain(h)",
                "Cannot iterate a string by reference: its chars are not lvalues (use for (auto c : s))|ab", "by-reference string iteration raises inside a coroutine body");
        });

        test("error_after_resumed_yield_fails_handle", [this, both_drained]() {
            both_drained("int nz(var v) { return v == null ? 1 : 0; }\nint boom(int x) { throw \"boom\" + to_string(x); }\ncoroutine int g() { int r = boom(nz(yield 1)); return r; }\n"
                         "auto h = g(); h.next(); string t = \"\"; try { h.next(); } catch (e) { t = to_string(e); } t + \"|\" + to_string(h.done())",
                "boom1|true", "a script throw in the continued part of the expression reaches the resumer");
            both_drained("class P { int v = 0; P(int x) { v = x; } }\nint nz(var v) { return v == null ? 1 : 0; }\ncoroutine int g() { auto p = new P(nz(yield 1) * 3); shared_ptr<P> sp = new P(nz(yield 2) * 4); return p.v + sp.v; }\nauto h = g(); drain(h)",
                "1|2|7", "yields inside new-expression arguments");
        });

        test("hot_reload_mid_expression_keeps_suspended_body", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execution_budget(0);
                jai::stdlib::register_all(e);
                e->execute("int nz(var v) { return v == null ? 1 : 0; }\nclass W { int v = 1; coroutine int g() { return nz(yield 1) + v; } }\nvar w = W(); var h = w.g(); var a = h.next();");
                e->execute("class W { int v = 1; string e = \"x\"; coroutine int g() { return 99; } }");
                auto r = e->execute("to_string(a) + \"|\" + to_string(h.next()) + \"|\" + w.e + \"|\" + to_string(w.g().next())");
                check_eq(std::string("1|2|x|99"), r.to_string(), std::string(use_vm ? "vm" : "interp") + ": a reload between resumes finishes the old expression on the migrated instance");
            }
        });

        // ---- catch variable across suspension (contract: the catch variable is an
        // ordinary block-scoped local of the catch body - a frame slot - and survives
        // suspension like any local; the exception pair + in-catch marker are parked per
        // coroutine so the resumer's own try/catch between resumes never touches them).
        // The vm used to lose 'e' after a yield (Undefined variable 'e'); both backends used
        // to resolve a catch variable dynamically through a backend-global intercept. ----
        test("catch_var_survives_yield_across_executes", [this, check_executes, both_drained]() {
            check_executes("coroutine int g() { try { throw \"abc\"; } catch (e) { yield 2; return to_string(e).size(); } } auto c = g();",
                           {"c.next();", "c.next();"}, "2|3", "size of e after the yield");
            check_executes("coroutine var g() { try { throw \"abc\"; } catch (e) { yield 2; return e; } } auto c = g();",
                           {"c.next();", "c.next();", "c.done();"}, "2|abc|true", "e itself after the yield");
            both_drained("coroutine int g() { try { throw \"abc\"; } catch (e) { yield 2; return to_string(e).size(); } }\nauto h = g(); drain(h)",
                         "2|3", "drained in one execute");
            check_executes("coroutine var g() { try { throw 42; } catch (err) { var e = err.value; yield 1; e = e + 1; yield e; return e * 2; } } auto c = g();",
                           {"c.next();", "c.next();", "c.next();"}, "1|43|86", "the catch variable is a mutable local across yields");
            check_executes("coroutine var g() { try { throw \"c\"; } catch (e) { yield 1; var l = [=]() { return e + \"!\"; }; yield l(); var m = [&]() { e = e + \"?\"; }; m(); var n = [e]() { return e; }; return n(); } } auto c = g();",
                           {"c.next();", "c.next();", "c.next();"}, "1|c!|c?", "[=], [&] and [e] lambdas created after yields capture the catch variable");
        });

        test("two_yields_inside_catch", [this, check_executes, both_drained]() {
            check_executes("coroutine var g() { try { throw \"abc\"; } catch (e) { yield 1; yield e; yield e + \"2\"; return e; } } auto c = g();",
                           {"c.next();", "c.next();", "c.next();", "c.next();"}, "1|abc|abc2|abc", "e across three yields");
            both_drained("coroutine var g() { try { throw \"abc\"; } catch (e) { yield 1; yield e; yield e + \"2\"; return e; } }\nauto h = g(); drain(h)",
                         "1|abc|abc2|abc", "drained in one execute");
        });

        test("nested_try_catch_yields_both_levels", [this, check_executes, both_drained]() {
            const std::string body =
                "coroutine var g() {\n"
                "    try { yield 1; try { yield 2; throw \"in\"; } catch (e1) { yield \"e1=\" + e1; throw \"out:\" + e1; } }\n"
                "    catch (e2) { yield \"e2=\" + e2; yield \"again:\" + e2; }\n"
                "    return \"done\";\n"
                "}\n";
            check_executes(body + "auto c = g();",
                           {"c.next();", "c.next();", "c.next();", "c.next();", "c.next();", "c.next();"},
                           "1|2|e1=in|e2=out:in|again:out:in|done", "one resume per execute");
            both_drained(body + "auto h = g(); drain(h)", "1|2|e1=in|e2=out:in|again:out:in|done", "drained in one execute");
            // the outer catch variable stays visible after the fiber resumes past a nested yield
            check_executes("coroutine var g() { try { throw \"o\"; } catch (a) { try { yield a; throw a + \"i\"; } catch (b) { yield a + \"/\" + b; return b; } } } auto c = g();",
                           {"c.next();", "c.next();", "c.next();"}, "o|o/oi|oi", "outer and inner catch variables after yields at both levels");
        });

        test("throw_after_resume_inside_try_begun_before_yield", [this, check_executes]() {
            // try_records_ park with the fiber: a throw after the resume still reaches the catch
            // opened before the yield, with the resumer's own try/catch running in between
            check_executes("coroutine var g() { try { yield 1; throw \"in\"; } catch (e) { yield \"c:\" + e; } return \"d\"; } auto c = g();",
                           {"c.next();", "try { throw \"r\"; } catch (re) { } c.next();", "c.next();"},
                           "1|c:in|d", "throw after resume inside a try begun before the yield");
            // bare rethrow after a yield inside the catch rethrows the caught value
            check_executes("coroutine var g() { try { throw \"in\"; } catch (e) { yield \"c:\" + e; throw; } } auto c = g();",
                           {"c.next();", "try { throw \"r\"; } catch (re) { } var got = \"\"; try { c.next(); } catch (err) { got = err; } got;"},
                           "c:in|in", "bare throw; after a yield inside a catch");
            // the resumer's catch variable is untouched by the fiber's throw
            check_executes("coroutine var g() { try { throw \"in\"; } catch (e) { yield 1; throw; } } auto c = g(); c.next();",
                           {"var log = \"\"; try { throw \"r\"; } catch (re) { try { c.next(); } catch (err) { log = err + \"/\" + re; } log = log + \"/\" + re; } log;"},
                           "in/r/r", "the resumer's catch variable survives a throw out of the fiber");
            // a coroutine body starts with no active exception: a bare throw; outside any catch
            // in the body raises the bare-throw error even when the resumer is inside a catch
            check_executes("coroutine var g() { yield 1; throw; } auto c = g(); c.next();",
                           {"var got = \"\"; try { throw \"r\"; } catch (re) { try { c.next(); } catch (err) { got = err; } } got;"},
                           "Bare throw with no active exception thrown.", "bare throw; in a body with no catch active");
        });

        test("resumer_try_catch_between_resumes_keeps_fiber_catch_var", [this, check_both]() {
            check_both(R"(
                coroutine var g() { try { throw "x"; } catch (e) { yield 1; yield e; return e + "!"; } }
                auto c = g();
                string log = to_string(c.next());
                try { throw "r"; } catch (err) { log = log + "/" + err; }
                log = log + "/" + to_string(c.next());
                [[warn_mode]] try { var q = e; } catch (err) { log = log + "/" + err; }
                log = log + "/" + to_string(c.next());
                log
            )", "1/r/x/Undefined variable 'e'/x!", "resumer try/catch between resumes, distinct names");
            check_both(R"(
                coroutine var g() { try { throw "x"; } catch (e) { yield 1; yield e; return e + "!"; } }
                auto c = g();
                string log = to_string(c.next());
                try { throw "r"; } catch (e) { log = log + "/" + e; }
                log = log + "/" + to_string(c.next());
                try { throw "s"; } catch (e) { log = log + "/" + e + "/" + to_string(c.next()) + "/" + e; }
                log
            )", "1/r/x/s/x!/s", "resumer catch with the SAME name, resumed from inside it");
            // nested resumes nest LIFO: a coroutine drives another from inside its own catch
            // while the host drives it from inside a third catch - every e reads its own
            check_both(R"(
                coroutine var inner() { try { throw "i"; } catch (e) { yield e; yield e + "2"; return e + "3"; } }
                coroutine var outer() { try { throw "o"; } catch (e) { auto c = inner(); yield e + "/" + to_string(c.next()); yield e + "/" + to_string(c.next()); yield e + "/" + to_string(c.next()); return e; } }
                auto h = outer();
                string log = "";
                try { throw "r"; } catch (e) { log = to_string(h.next()) + "|" + e; log = log + "|" + to_string(h.next()) + "|" + e; }
                log + "|" + to_string(h.next()) + "|" + to_string(h.next())
            )", "o/i|r|o/i2|r|o/i3|o", "nested resumes, three catch variables named e");
            // a throw escaping the inner fiber lands in the outer fiber's catch (inside ITS catch);
            // the outer's e is untouched by the inner's exception
            check_both(R"(
                coroutine var inner() { try { throw "i"; } catch (e) { yield 1; throw; } }
                coroutine var outer() { try { throw "o"; } catch (e) { auto c = inner(); c.next(); string got = ""; try { c.next(); } catch (x) { got = x; } yield got + "/" + e; return e; } }
                auto h = outer();
                to_string(h.next()) + "|" + to_string(h.next())
            )", "i/o|o", "a bare throw; out of the inner fiber lands in the outer fiber's catch");
        });

        test("catch_var_in_range_for_driving_coroutine", [this, check_both]() {
            // The resumer's e reads the resumer's exception before, during and after the loop.
            // Both backends used to leak the fiber's catch var into the resumer here
            // (x/x|x2/x|after:null) - that was the intercept, not the contract.
            check_both(R"(
                coroutine var g() { try { throw "x"; } catch (e) { yield e; yield e + "2"; } }
                string log = "";
                try { throw "outer"; } catch (e) { log = "pre:" + e + "|"; for (auto v : g()) { log = log + v + "/" + e + "|"; } log = log + "after:" + e; }
                log
            )", "pre:outer|x/outer|x2/outer|after:outer", "same name, range-for driving");
            check_both(R"(
                coroutine var g() { try { throw "x"; } catch (e) { yield e; yield e + "2"; } }
                string log = "";
                try { throw "outer"; } catch (oe) { for (auto v : g()) { log = log + v + "/" + oe + "|"; } log = log + "after:" + oe; }
                log
            )", "x/outer|x2/outer|after:outer", "distinct names, range-for driving");
            check_both(R"(
                coroutine var g() { try { throw "x"; } catch (e) { yield e; yield e + "2"; return "end"; } }
                string log = "";
                try { throw "outer"; } catch (e) { auto c = g(); log = to_string(c.next()) + "/" + e; log = log + "|" + to_string(c.next()) + "/" + e; log = log + "|" + to_string(c.next()) + "/" + e + "/" + to_string(c.done()); }
                log
            )", "x/outer|x2/outer|end/outer/true", "explicit resumes inside the resumer's catch, e read after completion");
            check_both(R"(
                coroutine var g() { try { throw "x"; } catch (e) { for (int i = 0; i < 2; i++) { yield e + to_string(i); } } }
                string log = "";
                try { throw "o"; } catch (e) { for (auto v : g()) { try { throw "n"; } catch (e) { log = log + e; } log = log + v + e + "|"; } }
                log
            )", "nx0o|nx1o|", "nested catch inside the driving loop shadows and restores");
        });

        // By-value completion values (`return x;` in a coroutine body) follow the function
        // rule: an lvalue read copies, the body's own local moves; `yield x` is unchanged.
        test("coroutine_completion_value_copies", [this, check_both]() {
            check_both(R"(
                array<int> cg = [1];
                coroutine co() { yield 1; return cg; }
                auto h = co();
                h.next();
                var done = h.next();
                done.push(2);
                class T { int v = 1; }
                T obj = T();
                coroutine cot() { yield obj; return obj; }
                auto h2 = cot();
                var y = h2.next();
                var d2 = h2.next();
                d2.v = 9;
                coroutine col() { array<int> own = [1]; yield 0; own.push(2); return own; }
                auto h3 = col();
                h3.next();
                var d3 = h3.next();
                to_string(cg.size()) + "/" + to_string(done.size()) + "/" + to_string(obj.v) + "/" + to_string(d2.v) + "/" + to_string(d3.size())
            )", "1/2/1/9/2", "completion values copy globals and move the body's own locals");
        });

        test("advance_stores_each_yield_and_answers_false_at_the_end", [this, check_both, check_executes, both_backends]() {
            check_both("coroutine int g() { yield 1; yield 2; return 3; }\nauto h = g(); int v = 0; string s = \"\"; "
                       "while (h.advance(v)) { s += to_string(v) + \",\"; } s + to_string(v) + \"|\" + to_string(h.done())",
                       "1,2,2|true", "true per yield with the value stored; the finishing step answers false, drops the return value, leaves v");
            check_both("coroutine g() { yield 1; yield null; yield 3; }\nauto h = g(); var v = 0; string s = \"\"; "
                       "while (h.advance(v)) { s += to_string(v) + \",\"; } s",
                       "1,null,3,", "a yielded null is a value like any other");
            check_both("coroutine int g() { yield 1; }\nauto h = g(); int v = 5; bool a = h.advance(v); bool b = h.advance(v); bool c = h.advance(v); "
                       "to_string(a) + \"|\" + to_string(b) + \"|\" + to_string(c) + \"|\" + to_string(v)",
                       "true|false|false|1", "advance after completion answers false and stores nothing");
            check_both("coroutine int g() { yield 1; throw \"bad\"; }\nauto h = g(); int v = 0; string t = \"\"; "
                       "try { while (h.advance(v)) { } } catch (e) { t = to_string(e); } t + \"|\" + to_string(v) + \"|\" + to_string(h.done()) + \"|\" + to_string(h.advance(v))",
                       "bad|1|true|false", "a throw from the body reaches the advance; the failed handle answers false");
            check_both("coroutine int g() { yield 4; yield 5; yield 6; }\nclass B { int x = 0; }\nB b; array<int> a = [0, 0]; auto h = g(); "
                       "h.advance(b.x); h.advance(a[1]); h.advance(a[0]); to_string(b.x) + to_json(a)",
                       "4[6,5]", "a field and an element are lvalues");
            check_both("coroutine int inner() { yield 5; yield 6; }\ncoroutine int outer() { auto h = inner(); int x = 0; while (h.advance(x)) { yield x * 10; } }\n"
                       "string s = \"\"; for (auto o : outer()) { s += to_string(o) + \",\"; } s",
                       "50,60,", "advance inside a coroutine body, the handle a local");
            check_both("class S { int base = 10; coroutine int waves(int n) { for (int k = 0; k < n; ++k) { yield base + k; } } }\n"
                       "S sp; auto h = sp.waves(2); int w = 0; string s = \"\"; while (h.advance(w)) { s += to_string(w) + \",\"; } s",
                       "10,11,", "a method coroutine");
            check_both("coroutine int g() { yield 1; }\nvar h = g(); string t = \"\"; try { h.advance(); } catch (e) { t = to_string(e); } t + \"|\" + to_string(h.done())",
                       "advance() takes 1 argument (the variable that receives the yielded value)|false", "the arity error runs nothing");
            check_both("coroutine int g() { yield 1; }\nvar h = g(); string t = \"\"; try { h.advance(math::abs(3)); } catch (e) { t = to_string(e); } t + \"|\" + to_string(h.done()) + \"|\" + to_string(h.next())",
                       "Cannot pass non-lvalue to reference parameter|false|1", "a temporary raises before the body runs");
            check_both("coroutine var g() { yield \"s\"; }\nvar h = g(); int i = 7; string t = \"\"; try { h.advance(i); } catch (e) { t = to_string(e); } t + \"|\" + to_string(i)",
                       "Cannot assign string to a variable declared int|7", "the store converts to the variable's declared type");
            check_executes("coroutine int g() { yield 1; yield 2; } auto h = g(); int v = 0;",
                           {"h.advance(v);", "v;", "h.advance(v);", "h.advance(v);", "v;"}, "true|1|true|false|2", "across separate executes");
            const auto load_error = [this, both_backends](const char* src, const std::string& text, const char* what) {
                auto [i_out, v_out] = both_backends(src);
                check_true(i_out.find(text) != std::string::npos, std::string("interp: ") + what + " (got: " + i_out + ")");
                check_true(v_out.find(text) != std::string::npos, std::string("vm: ") + what + " (got: " + v_out + ")");
            };
            load_error("coroutine int g() { yield 1; }\nauto h = g(); h.advance();", "2:25: error: advance() takes 1 argument (the variable that receives the yielded value)", "the checker knows the arity");
            load_error("coroutine int g() { yield 1; }\nauto h = g(); h.advance(5);", "2:25: error: advance() writes the yielded value through its argument; pass a variable", "a literal fails the load");
            load_error("coroutine string g() { yield \"a\"; }\nauto h = g(); int v = 0; h.advance(v);", "advance() cannot store the yielded 'string' into 'int'", "the yielded type against v's");
            load_error("coroutine int g() { yield 1; }\nconst int c = 0; auto h = g(); h.advance(c);", "cannot pass const 'c' to parameter 'v' of 'advance': 'advance' writes through it", "a const fails the load");
            load_error("coroutine int g() { yield 1; }\nauto h = g(); h.advanse(c);", "no member 'advanse' on 'coroutine_handle' (did you mean 'advance'?)", "the suggestion knows advance");
        });

        test("result_reads_the_return_value_once_done", [this, check_both, check_executes, both_backends]() {
            check_both("coroutine int g() { yield 1; yield 2; return 3; }\nauto h = g(); int v = 0; string s = \"\"; "
                       "while (h.advance(v)) { s += to_string(v) + \",\"; } s + to_string(h.result()) + \"|\" + to_string(h.result())",
                       "1,2,3|3", "after the advance that finished the body, as often as asked");
            check_both("coroutine int g() { yield 1; return true; }\nauto h = g(); var a = h.next(); var b = h.next(); "
                       "to_string(a) + \"|\" + to_string(b) + \"|\" + to_string(h.result()) + \"|\" + to_string(h.done())",
                       "1|1|1|true", "after the next() that finished the body: the same converted value");
            check_both("coroutine int g() { yield 1; }\nauto h = g(); for (auto v : h) { } to_string(h.result())",
                       "null", "a body that returns nothing gives null, here after a range-for");
            check_both("coroutine int g() { yield 1; return 5; }\nauto h = g(); string t = \"\"; try { h.result(); } catch (e) { t = to_string(e); } "
                       "h.next(); try { h.result(); } catch (e) { t += \"|\" + to_string(e); } t + \"|\" + to_string(h.next()) + \"|\" + to_string(h.result())",
                       "Coroutine has not finished: result() is available once done() is true|"
                       "Coroutine has not finished: result() is available once done() is true|5|5",
                       "before completion, created or suspended, it raises");
            check_both("coroutine int g() { yield 1; throw \"bad\"; }\nauto h = g(); h.next(); try { h.next(); } catch (e) { } to_string(h.done()) + \"|\" + to_string(h.result())",
                       "true|null", "a failed body gives null");
            check_both("coroutine array<int> g() { array<int> a = [1, 2]; yield a; return a; }\nauto h = g(); h.next(); var n = h.next(); n.push(3); "
                       "var r = h.result(); r.push(4); to_json(n) + to_json(r) + to_json(h.result())",
                       "[1,2,3][1,2,4][1,2]", "each read is a copy: neither next()'s value nor an earlier result() shares it");
            check_both("class K { int base = 7; coroutine int g() { yield 0; return base * 2; } }\nK k; auto h = k.g(); h.next(); h.next(); to_string(h.result())",
                       "14", "a method coroutine");
            check_both("coroutine int g() { return 1; }\nvar h = g(); h.next(); string t = \"\"; try { h.result(1); } catch (e) { t = to_string(e); } t",
                       "result() takes no arguments", "the arity error");
            check_executes("coroutine int g() { yield 1; return 9; } auto h = g(); int v = 0;",
                           {"h.advance(v);", "h.advance(v);", "h.result();"}, "true|false|9", "across separate executes");
            const auto load_error = [this, both_backends](const char* src, const std::string& text, const char* what) {
                auto [i_out, v_out] = both_backends(src);
                check_true(i_out.find(text) != std::string::npos, std::string("interp: ") + what + " (got: " + i_out + ")");
                check_true(v_out.find(text) != std::string::npos, std::string("vm: ") + what + " (got: " + v_out + ")");
            };
            load_error("coroutine int g() { return 1; }\nauto h = g(); h.result(1);", "error: result() takes no arguments", "the checker knows the arity");
            load_error("coroutine string g() { return \"a\"; }\nauto h = g(); h.next(); array<int> a = h.result();", "cannot initialize 'a'", "the checker knows the declared return type");
            load_error("coroutine int g() { return 1; }\nauto h = g(); h.resul();", "no member 'resul' on 'coroutine_handle' (did you mean 'result'?)", "the suggestion knows result");
        });

        test("next_and_next_count", [this, check_both, check_executes]() {
            const std::string gen = "coroutine int g() { yield 1; yield 2; yield 3; yield 4; return 5; }\n";
            check_both((gen + "auto h = g(); string s = to_string(h.next()); s += \"|\" + to_string(h.next(2)); s += \"|\" + to_string(h.next()); "
                       "s += \"|\" + to_string(h.done()); s += \"|\" + to_string(h.next(1)); s + \"|\" + to_string(h.done())").c_str(),
                       "1|3|4|false|5|true", "next() steps one yield, next(n) steps n and returns the last value");
            check_both((gen + "auto h = g(); int n = 3; to_string(h.next(n)) + \"|\" + to_string(h.next(n - 1))").c_str(),
                       "3|5", "the count is any int expression; the step that finishes the body returns the return value");
            check_both("int ran = 0;\ncoroutine int g() { ++ran; yield 1; ++ran; yield 2; ++ran; return 3; }\n"
                       "auto h = g(); string e = \"\"; try { h.next(5); } catch (err) { e = err; } e + \"|\" + to_string(ran) + \"|\" + to_string(h.done())",
                       "Cannot resume a completed coroutine|3|true", "next(n) past the end runs the steps before the end, then raises as next() at the end does");
            check_both((gen + "var h = g(); string e = \"\";\n"
                       "try { h.next(0); } catch (err) { e += err; }\n"
                       "try { h.next(-1); } catch (err) { e += \"|\" + err; }\n"
                       "try { h.next(1.0); } catch (err) { e += \"|\" + err; }\n"
                       "try { h.next(true); } catch (err) { e += \"|\" + err; }\n"
                       "try { h.next(1, 1); } catch (err) { e += \"|\" + err; }\n"
                       "e + \"|\" + to_string(h.next())").c_str(),
                       "next() count must be at least 1|next() count must be at least 1|next() count must be an integer|next() count must be an integer|"
                       "next() takes 0 or 1 arguments ([count])|1", "an invalid count raises before any step runs");
            check_both("int ran = 0;\ncoroutine int g() { ++ran; yield 1; ++ran; yield 2; }\n"
                       "auto h = g(); string s = \"\"; for (int i = 0; i < 3; ++i) { s += to_string(h.done()); } s += \"|\" + to_string(ran) + \"|\"; h.next();\n"
                       "for (int i = 0; i < 3; ++i) { s += to_string(h.done()); } s + \"|\" + to_string(ran) + \"|\" + to_string(h.next())",
                       "falsefalsefalse|0|falsefalsefalse|1|2", "done() is a query: it never starts or advances the body");
            check_both((gen + "int total = 0; for (auto v : g()) { total += v; } auto h = g(); h.next(2); for (auto v : h) { total += v * 100; } to_string(total)").c_str(),
                       "710", "range-for drives a fresh handle and continues a partly advanced one");
            check_both("int ran = 0;\ncoroutine int g() { ++ran; yield 1; }\nauto h = g(); to_string(ran)", "0", "calling a coroutine starts nothing");
            check_both("class Timer { int t = 0; int resume() { return ++t; } coroutine int next_tick() { yield resume(); yield resume(); } }\n"
                       "auto k = Timer(); auto h = k.next_tick(); to_string(k.resume()) + \"|\" + to_string(h.next()) + \"|\" + to_string(h.next())",
                       "1|2|3", "a class's own resume method is its method");
            check_executes(gen + "var h = g();", {"h.next();", "h.next(3);", "h.next(9);", "h.done();"},
                           "1|4|ERROR: Cannot resume a completed coroutine|true", "next(n) across executes");
        });

        // next(n) is one call: a throw in a step ends the steps with that throw, and from C++ the n steps run under one budget
        test("next_count_is_one_call", [this, check_both]() {
            check_both("coroutine int k() { yield 5; throw \"boom\"; yield 1; return 0; }\n"
                       "auto h = k(); string e = \"\"; try { h.next(3); } catch (err) { e = to_string(err.value); } e + \"|\" + to_string(h.done())",
                       "boom|true", "a later step's throw is what next(n) raises");
            check_both("coroutine int k() { yield 1; int z = 0; yield 10 / z; return 0; }\n"
                       "auto h = k(); string e = \"\"; try { h.next(2); } catch (err) { e = err; } e",
                       "Division by zero", "a later step's run-time error is what next(n) raises");
            for (bool use_vm : {false, true}) {
                const std::string be = use_vm ? " (vm)" : " (interpreter)";
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
                eng->execute("coroutine int forever() { while (true) { yield 1; } return 0; } var h = forever();");
                auto handle = std::static_pointer_cast<coroutine_handle>(eng->execute("h").get_object_holder()->host_object());
                eng->execution_budget(0.05);
                auto steps = handle->next(eng.get(), static_cast<script_int>(4000000000LL));
                check_true(!steps.has_value() && std::string(steps.message()).find("Script execution budget exceeded") != std::string::npos,
                           "one host next(n) is one budgeted entry" + be);
                eng->execution_budget(jai::foundry::k_test_budget_seconds);
                eng->execute("var h2 = forever();");
                auto fresh = std::static_pointer_cast<coroutine_handle>(eng->execute("h2").get_object_holder()->host_object());
                auto again = fresh->next(eng.get(), 3);
                check_true(again.has_value() && again.value().as_int() == 1, "the next host call arms its own deadline" + be);

                eng->execute("coroutine int k() { yield 1; throw \"late\"; yield 2; } var t = k();");
                auto thrower = std::static_pointer_cast<coroutine_handle>(eng->execute("t").get_object_holder()->host_object());
                auto thrown = thrower->next(eng.get(), 3);
                eng->execute("var t1 = k();");
                auto single = std::static_pointer_cast<coroutine_handle>(eng->execute("t1").get_object_holder()->host_object());
                (void)single->next(eng.get());
                auto alone = single->next(eng.get());
                check_true(!thrown.has_value() && !alone.has_value(), "a later step's uncaught throw is the host call's error" + be);
                if (!thrown.has_value() && !alone.has_value()) {
                    check_true(eng->format_error(thrown).find("late") != std::string::npos, eng->format_error(thrown) + be);
                    check_eq(eng->format_error(alone), eng->format_error(thrown), "next(3) fails as the single next() that throws does" + be);
                }
                check_eq((int64_t)3, eng->execute("1 + 2").as_int(), "engine stays usable" + be);
            }
        });

        test("method_coroutine_catch_var_across_yields", [this, check_executes, both_drained]() {
            check_executes("class K { string tag = \"T\"; coroutine var g() { try { throw \"m\"; } catch (e) { yield 1; yield tag + \":\" + e; yield this.tag + \"/\" + e; return e; } } } auto k = K(); auto c = k.g();",
                           {"c.next();", "c.next();", "c.next();", "c.next();"}, "1|T:m|T/m|m", "method coroutine, bare and this. field beside e");
            check_executes("class K { string tag = \"T\";\n"
                           "  coroutine var g() { try { yield 1; try { yield 2; throw \"in\"; } catch (e1) { yield tag + \":e1=\" + e1; throw \"out:\" + e1; } }\n"
                           "  catch (e2) { yield \"e2=\" + e2; yield \"again:\" + e2 + tag; } return \"done\"; } }\n"
                           "auto k = K(); auto c = k.g();",
                           {"c.next();", "c.next();", "c.next();", "c.next();", "c.next();", "c.next();"},
                           "1|2|T:e1=in|e2=out:in|again:out:inT|done", "nested shape as a method");
            check_executes("class K { static string tag = \"S\"; static coroutine var g() { try { throw \"m\"; } catch (e) { yield 1; yield tag + \":\" + e; return e; } } } auto c = K::g();",
                           {"c.next();", "c.next();", "c.next();"}, "1|S:m|m", "static method coroutine");
            both_drained("class K { string tag = \"T\"; coroutine var g() { try { throw \"m\"; } catch (e) { yield 1; yield tag + \":\" + e; return e; } } }\nauto k = K(); auto h = k.g(); drain(h)",
                         "1|T:m|m", "drained in one execute");
            // a hot reload of K between resumes keeps the old fiber's e (precedent
            // method_coroutine_hot_reload_matches_free_precedent)
            check_executes("class K { string tag = \"T\"; coroutine var g() { try { throw \"m\"; } catch (e) { yield 1; yield e; return e + tag; } } } auto k = K(); auto c = k.g();",
                           {"c.next();", "class K { string tag = \"T\"; string extra = \"E\"; coroutine var g() { return 99; } } c.next();", "c.next();", "k.g().next();"},
                           "1|m|mT|99", "hot reload between resumes keeps the suspended catch variable");
        });

    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::coroutine_tests)
