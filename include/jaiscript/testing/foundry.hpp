#pragma once

#include <iostream>
#include <functional>
#include <vector>
#include <memory>
#include <chrono>
#include <algorithm>
#include <string>
#include <sstream>
#include <format>
#include <exception>
#include <type_traits>
#include <cmath>
#include <limits>

namespace jai { class engine; }

namespace jai::foundry {

// Engine factory honoring the runner's --backend= selection (defined in main_test_runner.cpp).
// Tests create engines through this so the whole suite can run on a chosen backend.
std::shared_ptr<jai::engine> make_engine();
// The execution budget every test engine runs under: the wall-clock budget is a hang rail, so a loaded machine never decides a test
inline constexpr double k_test_budget_seconds = 120.0;
// engine::make() with the default backend under the test budget
std::shared_ptr<jai::engine> make_default_engine();
// --backend=vm-reference: make_engine's engines run engine::vm_reference_mode (a test that asserts an optimization fired skips)
bool vm_reference_run();
// The test quench is running (defined in main_test_runner.cpp): a crash report names it
void note_running_test(const char* suite_name, const char* test_name);

// Test failure exception
class test_failure : public std::exception {
public:
    test_failure(const std::string& msg) : message_(msg) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

// Info about a failed test
struct failure_info {
    std::string suite_name;
    std::string test_name;
    std::string error_message;
};

// Test suite results
struct suite_result {
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    std::vector<failure_info> failures;

    int total() const { return passed + failed; }
};

// Base test suite class
class suite {
public:
    suite(const std::string& name) : suite_name_(name) {}
    virtual ~suite() = default;
    
    // Override to define your tests
    virtual void forge_tests() = 0;
    
    // Optional: Override for per-test setup/teardown
    virtual void pre_test() {}
    virtual void post_test() {}
    
    // Add a test
    void test(const std::string& name, std::function<void()> test_func) {
        tests_.emplace_back(name, test_func);
    }
    
    // Add a benchmark
    void benchmark(const std::string& name, std::function<void()> bench_func, int32_t iterations = 1000) {
        benchmarks_.emplace_back(name, bench_func, iterations);
    }
    
    // Get the name of this test suite
    const std::string& get_name() const { return suite_name_; }
    
    // Simple wildcard matching (* matches any sequence, no * does substring)
    static bool matches_filter(const std::string& pattern, const std::string& text) {
        if (pattern.empty() || pattern == "*") return true;

        // If no wildcards, do substring match
        if (pattern.find('*') == std::string::npos) {
            return text.find(pattern) != std::string::npos;
        }

        // Simple wildcard matching
        size_t pi = 0, ti = 0;
        size_t star_pi = std::string::npos, star_ti = 0;

        while (ti < text.size()) {
            if (pi < pattern.size() && (pattern[pi] == text[ti] || pattern[pi] == '?')) {
                pi++;
                ti++;
            } else if (pi < pattern.size() && pattern[pi] == '*') {
                star_pi = pi++;
                star_ti = ti;
            } else if (star_pi != std::string::npos) {
                pi = star_pi + 1;
                ti = ++star_ti;
            } else {
                return false;
            }
        }

        while (pi < pattern.size() && pattern[pi] == '*') {
            pi++;
        }

        return pi == pattern.size();
    }

    // Run all tests and benchmarks (with optional test name filter)
    suite_result quench(const std::string& test_filter = "", bool verbose = false) {
        if (verbose) {
            std::cout << "\n||*** " << suite_name_ << " ***||\n";
        }

        // Let derived class register its tests
        note_running_test(suite_name_.c_str(), "(forge_tests)");
        forge_tests();

        suite_result result;

        // Run tests
        for (const auto& [name, func] : tests_) {
            // Skip if filter doesn't match
            if (!test_filter.empty() && !matches_filter(test_filter, name)) {
                result.skipped++;
                continue;
            }

            if (verbose) {
                std::cout << "  " << name << " ... " << std::flush;
            }

            note_running_test(suite_name_.c_str(), name.c_str());
            auto start = std::chrono::steady_clock::now();

            try {
                // Call pre_test before each test
                pre_test();

                func();

                // Call post_test after each test
                post_test();

                auto end = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

                if (verbose) {
                    std::cout << "<3 (" << duration.count() << "us)\n";
                }
                result.passed++;
            } catch (const std::exception& e) {
                post_test();  // Clean up even on error
                if (verbose) {
                    std::cout << "x\n";
                    std::cout << "    Error: " << e.what() << "\n";
                }
                result.failures.push_back({suite_name_, name, e.what()});
                result.failed++;
            }
        }

        note_running_test(nullptr, nullptr);

        // Run benchmarks if any (and no filter is active or filter matches)
        if (verbose && !benchmarks_.empty() && (test_filter.empty() || test_filter == "*")) {
            std::cout << "\n  Benchmarks:\n";
            for (const auto& [name, func, iterations] : benchmarks_) {
                run_benchmark(name, func, iterations);
            }
        }

        if (verbose) {
            std::cout << "\n  Summary: " << result.passed << " passed";
            if (result.failed > 0) {
                std::cout << ", " << result.failed << " failed";
            }
            if (result.skipped > 0) {
                std::cout << " (" << result.skipped << " skipped)";
            }
            std::cout << "\n";
        }

        return result;
    }
    
private:
    void run_benchmark(const std::string& name, std::function<void()> func, int32_t iterations = 1000) {
        
        // Setup for benchmark
        pre_test();
        
        try {
            // Warmup
            for (int i = 0; i < 10; ++i) {
                func();
            }
            
            // Measure
            auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < iterations; ++i) {
                func();
            }
            auto end = std::chrono::steady_clock::now();
            
            auto total = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            auto per_iteration = total.count() / iterations;
            
            std::cout << "    " << name << ": " << per_iteration << "uS/iteration\n";

            post_test();
        } catch (const std::exception& e) {
            post_test();
            std::cerr << "\n*** BENCHMARK ERROR in \"" << name << "\": " << e.what() << std::endl;
            throw;
        } catch (...) {
            post_test();
            std::cerr << "\n*** BENCHMARK ERROR in \"" << name << "\": Unknown exception" << std::endl;
            throw;
        }
    }
    
    std::string suite_name_;
    std::vector<std::pair<std::string, std::function<void()>>> tests_;
    std::vector<std::tuple<std::string, std::function<void()>, int32_t>> benchmarks_;
};

// Simple assertion helpers
template<typename T, typename U>
void check_eq(const T& expected, const U& actual, const std::string& message = "") {
    if (!(expected == actual)) {
        std::ostringstream oss;
        if (!message.empty()) {
            oss << message << " — ";
        }
        oss << "Expected: ";
        if constexpr (requires { oss << expected; }) {
            oss << expected;
        } else {
            oss << "(non-printable)";
        }
        oss << ", Actual: ";
        if constexpr (requires { oss << actual; }) {
            oss << actual;
        } else {
            oss << "(non-printable)";
        }
        throw test_failure(oss.str());
    }
}

inline void check(bool condition, const std::string& message = "Check failed") {
    if (!condition) {
        throw test_failure(message);
    }
}

// Convenience aliases for common patterns
template<typename T>
void check(const T& expected, const T& actual) {
    check_eq(expected, actual);
}

template<typename T>
void check_near(T expected, T actual, T tolerance) {
    if (std::abs(expected - actual) > tolerance) {
        std::stringstream ss;
        ss << "Expected: " << expected << " ± " << tolerance << ", Actual: " << actual;
        throw test_failure(ss.str());
    }
}

template<typename Func>
void check_throws(Func&& func, const std::string& message = "Expected exception") {
    try {
        func();
        throw test_failure(message);
    } catch (const test_failure&) {
        throw;  // Re-throw test failures
    } catch (...) {
        // Expected exception caught
    }
}

template<typename Exception, typename Func>
void check_throws(Func&& func, const std::string& message = "Expected specific exception") {
    try {
        func();
        throw test_failure(message);
    } catch (const test_failure&) {
        throw;  // Re-throw test failures
    } catch (const Exception&) {
        // Expected exception caught
    } catch (...) {
        throw test_failure("Caught unexpected exception type");
    }
}

// Additional check functions
inline void check_true(bool condition, const std::string& message = "Expected true") {
    if (!condition) {
        throw test_failure(message);
    }
}

inline void check_false(bool condition, const std::string& message = "Expected false") {
    if (condition) {
        throw test_failure(message);
    }
}

template<typename T>
void check_null(const T& ptr, const std::string& message = "Expected null") {
    if (ptr != nullptr) {
        throw test_failure(message);
    }
}

template<typename T>
void check_not_null(const T& ptr, const std::string& message = "Expected non-null") {
    if (ptr == nullptr) {
        throw test_failure(message);
    }
}

template<typename T, typename U>
void check_ne(const T& expected, const U& actual, const std::string& message = "") {
    if (expected == actual) {
        if (message.empty()) {
            throw test_failure("Values should not match");
        } else {
            throw test_failure(message);
        }
    }
}

template<typename T, typename U>
void check_gt(const T& value, const U& threshold, const std::string& message = "") {
    if (!(value > threshold)) {
        if (message.empty()) {
            throw test_failure("Value not greater than threshold");
        } else {
            throw test_failure(message);
        }
    }
}

template<typename T, typename U>
void check_lt(const T& value, const U& threshold, const std::string& message = "") {
    if (!(value < threshold)) {
        if (message.empty()) {
            throw test_failure("Value not less than threshold");
        } else {
            throw test_failure(message);
        }
    }
}

template<typename T, typename U>
void check_ge(const T& value, const U& threshold, const std::string& message = "") {
    if (!(value >= threshold)) {
        if (message.empty()) {
            throw test_failure("Value not greater than or equal to threshold");
        } else {
            throw test_failure(message);
        }
    }
}

template<typename T, typename U>
void check_le(const T& value, const U& threshold, const std::string& message = "") {
    if (!(value <= threshold)) {
        if (message.empty()) {
            throw test_failure("Value not less than or equal to threshold");
        } else {
            throw test_failure(message);
        }
    }
}

} // namespace jai::foundry

// Include auto-registration functionality
#include "auto_register.hpp"