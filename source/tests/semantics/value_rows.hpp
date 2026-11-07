#pragma once

#include "../case_table.hpp"

#include <string>
#include <variant>

namespace jai::foundry::tests {

struct text { const char* value; };      // expects result.as<std::string>()
struct listing { const char* value; };   // expects rendered(result): a host's view of a returned array, map or scalar
using expected_value = std::variant<int, double, bool, text, listing>;

struct value_row {
    const char* code;
    expected_value expected;
};

// [a, b] for an array, {k: v} for a map in its iteration order, "s" for a string, ? for anything else a host reads opaquely
inline std::string rendered(const script_value& a_value) {
    std::string result;
    if (a_value.is_array()) {
        result = "[";
        for (const auto& element : a_value.as<std::vector<script_value>>()) {
            result += (result.size() > 1 ? ", " : "") + rendered(element);
        }
        result += "]";
    } else if (a_value.is_map()) {
        result = "{";
        for (const auto& [key, value] : a_value.as<script_map>()) {
            result += (result.size() > 1 ? ", " : "") + rendered(key) + ": " + rendered(value);
        }
        result += "}";
    } else if (a_value.is_int()) {
        result = std::to_string(a_value.as<int64_t>());
    } else if (a_value.is_float()) {
        std::ostringstream out;
        out << a_value.as<double>();
        result = out.str();
    } else if (a_value.is_string()) {
        result = "\"" + a_value.as<std::string>() + "\"";
    } else if (a_value.is_bool()) {
        result = a_value.as<bool>() ? "true" : "false";
    } else {
        result = "?";
    }
    return result;
}

inline void check_value(const script_value& a_result, const expected_value& a_expected) {
    std::visit([&](const auto& expected) {
        using T = std::decay_t<decltype(expected)>;
        if constexpr (std::is_same_v<T, text>) {
            check_eq(std::string(expected.value), a_result.as<std::string>());
        } else if constexpr (std::is_same_v<T, listing>) {
            check_eq(std::string(expected.value), rendered(a_result));
        } else {
            check_eq(expected, a_result.as<T>());
        }
    }, a_expected);
}

// Each row runs on a fresh make_engine() prepared by a_setup.
template<typename Rows, typename Setup>
void check_values(const Rows& a_rows, Setup&& a_setup) {
    check_each(a_rows, [&](const value_row& row) {
        auto e = make_engine();
        a_setup(*e);
        check_value(e->execute(row.code), row.expected);
    });
}

template<typename Rows>
void check_values(const Rows& a_rows) {
    check_values(a_rows, [](engine&) {});
}

} // namespace jai::foundry::tests
