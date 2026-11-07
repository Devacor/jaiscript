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
    }
};

} // namespace jai::foundry::tests

// Auto-register with the test framework
using control_flow_tests = jai::foundry::tests::control_flow_tests;
FOUNDRY_REGISTER(control_flow_tests)