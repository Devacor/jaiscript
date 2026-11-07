#pragma once

#include <jaiscript/core/checked_result.hpp>
#include <jaiscript/core/runtime_errors.hpp>

#include <cstdint>
#include <string>

namespace jai::detail {

// Arity texts: "'fib' expects 1 argument, got 2" / "lambda expects
// 1 argument, got 0". {0} = the callee (its name view points into symbolizer storage, so the
// intern is a hit even while frozen), {1} = "N argument(s), got M", composed only while the
// symbolizer is unfrozen; a parallel worker gets the declared count alone from the table below.
// Viability itself is jai::arity_accepts (overload_resolution.hpp).
inline constexpr std::string_view arity_named = "'{0}' expects {1}";
inline constexpr std::string_view arity_named_one = "'{0}' expects {1} argument";
inline constexpr std::string_view arity_named_many = "'{0}' expects {1} arguments";
inline constexpr std::string_view arity_lambda = "lambda expects {0}";
inline constexpr std::string_view arity_lambda_one = "lambda expects {0} argument";
inline constexpr std::string_view arity_lambda_many = "lambda expects {0} arguments";
inline constexpr std::string_view arity_named_count_free = "'{0}' was called with the wrong number of arguments";
inline constexpr std::string_view arity_lambda_count_free = "lambda was called with the wrong number of arguments";
inline constexpr std::string_view lambda_callee_name = "<lambda>";

// Counts ride the checked_result symbol channel, so their decimal spellings are interned once
// at backend init: error paths never intern (parallel workers run against a frozen symbolizer)
struct arity_count_symbols {
    static constexpr size_t count = 64;
    uint64_t ids[count] = {};
    template <typename Symbolizer>
    void intern(const Symbolizer& symbolizer) {
        for (size_t i = 0; i < count; ++i) { ids[i] = symbolizer.intern(std::to_string(i)); }
    }
    template <typename Symbolizer>
    uint64_t operator()(size_t n, const Symbolizer& symbolizer) const {
        return n < count ? ids[n] : symbolizer.intern(std::to_string(n));
    }
};

inline std::string arity_detail(size_t expected, size_t got) {
    return std::to_string(expected) + (expected == 1 ? " argument, got " : " arguments, got ") + std::to_string(got);
}

template <typename Symbolizer>
inline error_propagator arity_error(std::string_view callee, size_t expected, size_t got,
                                    const arity_count_symbols& symbols, const Symbolizer& symbolizer) {
    const std::error_code ec = make_error_code(runtime_error_code::argument_count_mismatch);
    const bool anonymous = callee.empty() || callee == lambda_callee_name;
    if (!symbolizer.frozen()) {
        const uint64_t detail = symbolizer.intern(arity_detail(expected, got));
        if (anonymous) { return {ec, arity_lambda, detail, 0}; }
        return {ec, arity_named, symbolizer.intern(callee), detail};
    }
    if (expected >= arity_count_symbols::count) {   // past the table a count would be a new string: the count-free twin
        if (anonymous) { return {ec, arity_lambda_count_free, 0, 0}; }
        return {ec, arity_named_count_free, symbolizer.intern(callee), 0};
    }
    const uint64_t count = symbols.ids[expected];
    if (anonymous) { return {ec, expected == 1 ? arity_lambda_one : arity_lambda_many, count, 0}; }
    return {ec, expected == 1 ? arity_named_one : arity_named_many, symbolizer.intern(callee), count};
}

// 'auto' container homogeneity (both backends' validators): {0} = the offending index on
// the symbol channel, the count table's spelling; past the table it is interned on the main
// thread only, and a worker takes the index-free twin
inline constexpr std::string_view auto_array_elements = "Array 'auto' requires homogeneous elements - type mismatch";
inline constexpr std::string_view auto_array_elements_at = "Array 'auto' requires homogeneous elements - type mismatch at index {0}";
inline constexpr std::string_view auto_array_classes = "Array 'auto' requires homogeneous class types - mismatch";
inline constexpr std::string_view auto_array_classes_at = "Array 'auto' requires homogeneous class types - mismatch at index {0}";
inline constexpr std::string_view auto_map_values = "Map 'auto' requires homogeneous values - type mismatch";
inline constexpr std::string_view auto_map_values_at = "Map 'auto' requires homogeneous values - type mismatch at index {0}";
inline constexpr std::string_view auto_map_classes = "Map 'auto' requires homogeneous class types - mismatch";
inline constexpr std::string_view auto_map_classes_at = "Map 'auto' requires homogeneous class types - mismatch at index {0}";

template <typename Symbolizer>
inline error_propagator homogeneity_error(runtime_error_code code, std::string_view text, std::string_view text_at, size_t index,
                                          const arity_count_symbols& symbols, const Symbolizer& symbolizer) {
    const uint64_t at = index < arity_count_symbols::count ? symbols.ids[index]
                      : symbolizer.frozen() ? 0 : symbolizer.intern(std::to_string(index));
    return {make_error_code(code), at ? text_at : text, at, 0};
}

} // namespace jai::detail
