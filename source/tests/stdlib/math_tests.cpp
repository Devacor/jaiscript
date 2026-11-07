#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"
#include <cmath>

using namespace jai::foundry;

namespace jai::foundry::tests {

namespace {

void check_near_results(double a_tolerance, const std::vector<std::pair<std::string, double>>& a_cases) {
    check_each(a_cases, [&](const auto& c) {
        auto engine = make_engine();
        stdlib::register_all(*engine);
        check(std::abs(engine->execute(c.first).template as<double>() - c.second) < a_tolerance);
    });
}

}

class math_stdlib_tests : public suite {
public:
    math_stdlib_tests() : suite("Math Stdlib Tests") {}

    void forge_tests() override {
        test("abs_min_max_clamp_and_saturate_bound_a_value", [this]() {
            check_stdlib_results<double>({
                {"math::abs(5.5);", 5.5},
                {"math::abs(-5.5);", 5.5},
                {"math::min(3.0, 7.0);", 3.0},
                {"math::max(3.0, 7.0);", 7.0},
                {"math::clamp(5.0, 0.0, 10.0);", 5.0},
                {"math::clamp(-5.0, 0.0, 10.0);", 0.0},
                {"math::clamp(15.0, 0.0, 10.0);", 10.0},
                {"math::saturate(0.5);", 0.5},
                {"math::saturate(-0.5);", 0.0},
                {"math::saturate(1.5);", 1.0},
            });
        });

        test("sign_is_minus_one_zero_or_one", [this]() {
            check_stdlib_results<script_int>({
                {"math::sign(42.0);", 1},
                {"math::sign(-42.0);", -1},
                {"math::sign(0.0);", 0},
            });
        });

        test("floor_ceil_round_and_trunc_round_to_whole_values", [this]() {
            check_stdlib_results<double>({
                {"math::floor(3.7);", 3.0},
                {"math::ceil(3.2);", 4.0},
                {"math::round(3.5);", 4.0},
                {"math::trunc(-3.7);", -3.0},
            });
        });

        test("sqrt_pow_hypot_sin_and_cos_give_exact_values", [this]() {
            check_stdlib_results<double>({
                {"math::sqrt(16.0);", 4.0},
                {"math::pow(2.0, 3.0);", 8.0},
                {"math::hypot(3.0, 4.0);", 5.0},
                {"math::sin(0.0);", 0.0},
                {"math::cos(0.0);", 1.0},
            });
        });

        test("exp_log_sin_and_the_constants_match_to_a_ten_thousandth", [this]() {
            check_near_results(0.0001, {
                {"math::exp(1.0);", 2.71828182845904523536},
                {"math::log(math::E);", 1.0},
                {"math::sin(math::PI / 2.0);", 1.0},
                {"math::PI;", 3.14159265358979323846},
                {"math::E;", 2.71828182845904523536},
                {"math::TAU;", 6.28318530717958647692},
            });
        });

        test("mix_unmix_eases_lerp_wrap_and_remap_map_between_ranges", [this]() {
            check_stdlib_results<double>({
                {"math::mix(0.0, 100.0, 0.5);", 50.0},
                {"math::mix(10.0, 20.0, 0.0);", 10.0},
                {"math::mix(10.0, 20.0, 1.0);", 20.0},
                {"math::unmix(0.0, 100.0, 50.0);", 0.5},
                {"math::unmix(10.0, 20.0, 10.0);", 0.0},
                {"math::unmix(10.0, 20.0, 20.0);", 1.0},
                {"math::mix_in(0.0, 100.0, 0.5, 2.0);", 25.0},
                {"math::mix_out(0.0, 100.0, 0.5, 2.0);", 75.0},
                {"math::lerp(0.0, 100.0, 0.25);", 25.0},
                {"math::wrap(0.0, 360.0, 450.0);", 90.0},
                {"math::remap(5.0, 0.0, 10.0, 0.0, 100.0);", 50.0},
            });
        });

        test("math_holds_no_randomness", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            for (const char* name : {"random", "random_int", "random_range", "random_seed"}) {
                std::string refused;
                try { engine->execute(std::string("var f = math::") + name + ";"); } catch (const jai::static_check_error& e) { refused = e.what(); }
                check_true(refused.find(std::string("error: no member '") + name + "' in namespace 'math'") != std::string::npos, refused);
            }
        });
    }
};

} // namespace jai::foundry::tests

// Auto-register with the test framework
FOUNDRY_REGISTER(jai::foundry::tests::math_stdlib_tests)
