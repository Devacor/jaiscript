#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/timing.hpp>

#include <string>
#include <thread>

using namespace jai::foundry;

namespace jai::foundry::tests {

class timing_tests : public suite {
public:
    timing_tests() : suite("Foundry Timing") {}

    void forge_tests() override {
        test("compare_speed_interleaves_and_takes_the_median_ratio", [this]() {
            // sides that report fixed seconds: the ratio is exact, and the call order shows the warm-up and the alternation
            std::string order;
            int measured_calls = 0;
            const double ratios[] = {5.0, 2.0, 3.0, 9.0, 1.0};
            const speed_ratio speed = compare_speed("fixed sides",
                [&] { order += 'r'; return 0.5; },
                [&] { order += 'm'; return measured_calls++ == 0 ? 100.0 : 0.5 * ratios[measured_calls - 2]; }, 5);
            check_eq(std::string("rmrmmrrmmrrm"), order, "one warm-up of each, then the order alternates");
            check_eq(5, speed.rounds);
            check_near(3.0, speed.ratio, 1e-12);
            check_near(1.0, speed.lowest_ratio, 1e-12);
            check_near(9.0, speed.highest_ratio, 1e-12);
            check_near(0.5, speed.reference_seconds, 1e-12);
            check_near(1.5, speed.measured_seconds, 1e-12);
            check_speed_ratio(speed, 3.0);
            bool failed = false;
            try { check_speed_ratio(speed, 2.5); } catch (const test_failure&) { failed = true; }
            check_true(failed, "a ratio past the bound fails");
        });

        test("stall_watch_trips_on_stillness_never_on_slow_progress", [this]() {
            stall_watch patient(k_hang_guard);
            check_false(patient.stalled(0), "a fresh watch");
            check_false(patient.stalled(1), "a move");
            stall_watch eager(std::chrono::milliseconds(1));
            for (uint64_t step = 1; step <= 3; ++step) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                check_false(eager.stalled(step), "a move after longer than the guard");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            check_true(eager.stalled(3), "a reading that stood still past the guard");
        });
    }
};

} // namespace jai::foundry::tests

using timing_tests = jai::foundry::tests::timing_tests;
FOUNDRY_REGISTER(timing_tests)
