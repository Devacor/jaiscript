#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include "../case_table.hpp"

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class control_flow_tests : public suite {
public:
    control_flow_tests() : suite("Control Flow") {}
    
    void forge_tests() override {
        test("if_and_else_run_the_branch_their_condition_picks", [this]() {
            const std::pair<const char*, int> cases[] = {
                {"var x = 5; if (x > 0) { x = 10; } x;", 10},
                {"var x = 5; if (x < 0) { x = 10; } x;", 5},
                {"var x = 5; if (x > 0) { x = 10; } else { x = -10; } x;", 10},
                {"var x = -5; if (x > 0) { x = 10; } else { x = -10; } x;", -10},
                {"var x = 5; var y = 10; if (x > 0) { if (y > 5) { x = x + y; } } x;", 15},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_eq(engine->execute(c.first).template as<int>(), c.second);
            });
        });

        test("while_loops_run_until_their_condition_fails_or_a_break", [this]() {
            const std::pair<const char*, int> cases[] = {
                {"var counter = 0; while (counter < 5) { counter = counter + 1; } counter;", 5},
                {"var sum = 0; var i = 1; while (i <= 4) { sum = sum + i; i = i + 1; } sum;", 10},
                {"var i = 0; while (true) { i = i + 1; if (i >= 5) { break; } } i;", 5},
                {"var sum = 0; var i = 0; while (i < 10) { i = i + 1; if (i % 2 == 0) { continue; } sum = sum + i; } sum;", 25},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_eq(engine->execute(c.first).template as<int>(), c.second);
            });
        });

        test("for_loops_count_break_continue_and_nest", [this]() {
            const std::pair<const char*, int> cases[] = {
                {"var sum = 0; for (var i = 1; i <= 5; i = i + 1) { sum = sum + i; } sum;", 15},
                {"var sum = 0; for (int i = 0; i < 5; ++i) { sum += i; } sum;", 10},
                {"var sum = 0; for (int i = 1; i <= 10; ++i) { if (i > 5) { break; } sum += i; } sum;", 15},
                {"var sum = 0; for (int i = 1; i <= 10; ++i) { if (i % 2 == 0) { continue; } sum += i; } sum;", 25},
                {"var sum = 0; for (int i = 1; i <= 3; ++i) { for (int j = 1; j <= 3; ++j) { sum += i * j; } } sum;", 36},
                {"var result = 0; for (int i = 1; i <= 10; ++i) { if (i % 2 == 0) { continue; } var j = 0; while (j < i) { result += 1; j++; if (j > 3) { break; } } } result;", 16},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_eq(engine->execute(c.first).template as<int>(), c.second);
            });
        });

        test("a_condition_declares_the_variable_it_tests", [this]() {
            const std::pair<const char*, int> cases[] = {
                {"var out = 0; if (auto n = 5) { out = n; } out;", 5},
                {"var out = 0; if (int n = 0) { out = 1; } else { out = n - 1; } out;", -1},
                {"int f(int x) { if (const y = x * 2; y > 4) { return y; } else if (auto z = y + 1) { return -z; } return 0; } f(3) * 100 + f(1);", 597},
                {"int f() { int total = 0; int i = 0; while (auto left = 3 - i) { total += left; i += 1; } return total; } f();", 6},
                {"int f() { int t = 0; int i = 0; while (int n = 5 - i) { i += 1; if (n == 4) { continue; } if (n == 1) { break; } t += n; } return t; } f();", 10},
                {"var t = 0; var i = 0; while (var n = 3 - i) { t += n; i += 1; } t;", 6},
                {"var s = 0; if (s = 1; s) { s = 7; } s;", 7},
                {"var s = 0; if (; s == 0) { s = 2; } s;", 2},
                {"int f() { int a = 3; if (int& r = a) { r = 9; } return a; } f();", 9},
                {"int a = 6; int b = 3; var out = 0; if (a & b) { out = 1; } out;", 1},
                {"class P { int v = 4; } shared_ptr<P> none = null; var out = 0; if (shared_ptr<P> p = new P()) { out = p.v; } if (shared_ptr<P> q = none) { out = -1; } out;", 4},
                {"class P { int v = 4; } int f(shared_ptr<P> given) { if (P& p = given; p.v > 3) { p.v += 1; } return given.v; } f(new P());", 5},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_eq(engine->execute(c.first).template as<int>(), c.second);
            });
            const auto load_error = [](const std::string& code) {
                auto fresh = make_engine();
                try { fresh->execute(code); } catch (const std::exception& ex) { return std::string(ex.what()); }
                return std::string("loaded");
            };
            check_true(load_error("if (auto x) { }").find("error: a condition's declaration needs an initializer: if (auto x = e)") != std::string::npos);
            check_true(load_error("if (auto n = 1) { } n;").find("error: use of undeclared identifier 'n'") != std::string::npos);
            check_true(load_error("void f() { while (int n = 0) { } n = 1; }").find("error: use of undeclared identifier 'n'") != std::string::npos);
            check_true(load_error("int a = 1; if (int b = 2; a) { } b;").find("error: use of undeclared identifier 'b'") != std::string::npos);
        });
    }
};

} // namespace jai::foundry::tests

// Auto-register with the test framework
using control_flow_tests = jai::foundry::tests::control_flow_tests;
FOUNDRY_REGISTER(control_flow_tests)