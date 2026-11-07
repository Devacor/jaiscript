#pragma once

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace jai::foundry::tests {

template<typename Case>
std::string case_label(const Case& a_case) {
    if constexpr (std::is_convertible_v<const Case&, std::string_view>) {
        return std::string(std::string_view(a_case));
    } else if constexpr (requires { a_case.code; }) {
        return std::string(a_case.code);
    } else {
        return std::string(std::get<0>(a_case));
    }
}

// Runs a_check on every case, then fails once naming each case that failed, so a table reports all its failures.
template<typename Cases, typename Check>
void check_each(const Cases& a_cases, Check&& a_check) {
    std::string failures;
    for (const auto& c : a_cases) {
        try {
            a_check(c);
        } catch (const std::exception& error) {
            failures += case_label(c) + "\n  " + error.what() + "\n";
        }
    }
    check_true(failures.empty(), failures);
}

// What a_code prints with trailing newlines trimmed, or "ERROR <what>" when it throws.
inline std::string printed_output(jai::engine& a_engine, const std::string& a_code) {
    auto out = std::make_shared<std::ostringstream>();
    a_engine.set_output_stream(out);
    std::string result;
    try {
        a_engine.execute(a_code);
        result = out->str();
    } catch (const std::exception& error) {
        result = std::string("ERROR ") + error.what();
    }
    while (!result.empty() && result.back() == '\n') { result.pop_back(); }
    return result;
}

// Each {code, expected} runs a_prelude + code on a fresh make_engine(); expected is the printed output, or "ERROR <text>"
// for a run that throws an error whose message holds <text>.
template<typename Cases>
void check_outputs(const Cases& a_cases, const std::string& a_prelude = {}) {
    check_each(a_cases, [&](const auto& c) {
        auto e = make_engine();
        const std::string got = printed_output(*e, a_prelude + std::get<0>(c));
        const std::string want = std::get<1>(c);
        const bool error_case = want.rfind("ERROR ", 0) == 0;
        const bool ok = error_case ? got.rfind("ERROR ", 0) == 0 && got.find(want.substr(6)) != std::string::npos : got == want;
        check_true(ok, "got: " + got);
    });
}

// Each {code, expected} runs on a fresh make_engine() and checks the result read as Result.
template<typename Result>
void check_results(const std::vector<std::pair<std::string, Result>>& a_cases) {
    check_each(a_cases, [](const auto& c) {
        auto e = make_engine();
        check_eq(c.second, e->execute(c.first).template as<Result>());
    });
}

// check_results on engines with the stdlib registered.
template<typename Result>
void check_stdlib_results(const std::vector<std::pair<std::string, Result>>& a_cases) {
    check_each(a_cases, [](const auto& c) {
        auto e = make_engine();
        jai::stdlib::register_all(*e);
        check_eq(c.second, e->execute(c.first).template as<Result>());
    });
}

} // namespace jai::foundry::tests
