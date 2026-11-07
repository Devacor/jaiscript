#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "foundry.hpp"

// How a test reads the clock. It never asserts an absolute time: a speed is a ratio to a reference operation timed in the
// same process, interleaved with it, and a hang rail trips on a stall, never on slow progress.
namespace jai::foundry {

// The stillness a hang rail allows: nothing a test waits on stands still this long unless it is hung
inline constexpr std::chrono::seconds k_hang_guard{600};

// The seconds one call of work takes
inline double time_seconds(const std::function<void()>& work) {
    const auto start = std::chrono::steady_clock::now();
    work();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

struct speed_ratio {
    std::string what;
    int rounds = 0;
    double reference_seconds = 0;   // each side's median round
    double measured_seconds = 0;
    double ratio = 0;               // the median of the rounds' measured / reference
    double lowest_ratio = 0;
    double highest_ratio = 0;

    std::string report() const {
        return std::format("{}: {:.3f}x the reference (median of {} interleaved rounds, {:.3f}x..{:.3f}x; measured {:.1f} us, reference {:.1f} us)",
                           what, ratio, rounds, lowest_ratio, highest_ratio, measured_seconds * 1e6, reference_seconds * 1e6);
    }
};

namespace timing_detail {

inline double median_of(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

} // namespace timing_detail

// Times reference and measured in alternating order for `rounds` rounds after one untimed warm-up of each, so load and
// drift fall on both sides alike, and prints the report. A side returns the seconds of the part it measures (setup stays out).
// Give both sides about the same duration: a side that runs ten times longer catches ten times the bursts of load.
inline speed_ratio compare_speed(const std::string& what, const std::function<double()>& reference, const std::function<double()>& measured,
                                 int rounds = 7) {
    reference();
    measured();
    speed_ratio result;
    result.what = what;
    result.rounds = std::max(rounds, 1);
    std::vector<double> reference_rounds, measured_rounds, ratios;
    for (int round = 0; round < result.rounds; ++round) {
        double reference_seconds = 0, measured_seconds = 0;
        if (round % 2 == 0) {
            reference_seconds = reference();
            measured_seconds = measured();
        } else {
            measured_seconds = measured();
            reference_seconds = reference();
        }
        reference_rounds.push_back(reference_seconds);
        measured_rounds.push_back(measured_seconds);
        ratios.push_back(measured_seconds / std::max(reference_seconds, 1e-9));
    }
    result.reference_seconds = timing_detail::median_of(reference_rounds);
    result.measured_seconds = timing_detail::median_of(measured_rounds);
    result.ratio = timing_detail::median_of(ratios);
    result.lowest_ratio = *std::min_element(ratios.begin(), ratios.end());
    result.highest_ratio = *std::max_element(ratios.begin(), ratios.end());
    std::printf("  [speed] %s\n", result.report().c_str());
    std::fflush(stdout);
    return result;
}

// Fails when the measured side ran more than max_ratio times the reference. The bound is a regression's signature (a lost
// complexity class, a fast path gone) with room for load and other machines, never a tuned expectation.
inline void check_speed_ratio(const speed_ratio& speed, double max_ratio) {
    if (!(speed.ratio <= max_ratio)) {
        throw test_failure(std::format("{} exceeds the {:.1f}x bound", speed.report(), max_ratio));
    }
}

// A hang rail over a progress reading: stalled() turns true only once the reading has stood still for the whole guard
class stall_watch {
public:
    explicit stall_watch(std::chrono::steady_clock::duration guard = k_hang_guard) : guard_(guard), last_move_(std::chrono::steady_clock::now()) {}

    bool stalled(uint64_t progress) {
        const auto now = std::chrono::steady_clock::now();
        if (progress != last_progress_) {
            last_progress_ = progress;
            last_move_ = now;
        }
        return now - last_move_ >= guard_;
    }

private:
    std::chrono::steady_clock::duration guard_;
    std::chrono::steady_clock::time_point last_move_;
    uint64_t last_progress_ = 0;
};

} // namespace jai::foundry
