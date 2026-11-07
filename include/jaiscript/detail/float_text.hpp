#pragma once

#ifndef __JAISCRIPT_DETAIL_FLOAT_TEXT_HPP__
#define __JAISCRIPT_DETAIL_FLOAT_TEXT_HPP__

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace jai::detail {

    // Every NaN prints "nan": the language orders all NaNs as one group (no sign, no payload), while a C++ runtime spells a
    // NaN its own way (MSVC's "-nan(ind)")
    inline constexpr std::string_view k_nan_text = "nan";

    // strtod with the range verdict every C runtime agrees on: glibc and MinGW's CRT raise ERANGE for any
    // subnormal result (5e-324) while MSVC's does not, so an ERANGE parse is out of range only when it
    // produced zero or infinity (underflow past the smallest subnormal, overflow)
    struct float_text_result {
        double value = 0.0;
        const char* end = nullptr;
        bool out_of_range = false;
    };

    inline float_text_result parse_float_text_strtod(const char* text) noexcept {
        const int saved = errno;
        errno = 0;
        char* end = nullptr;
        const double value = std::strtod(text, &end);
        const bool out_of_range = errno == ERANGE && (value == 0.0 || std::isinf(value));
        errno = saved;
        return { value, end, out_of_range };
    }

    // the same result for text[0..length) (NUL-terminated at length): a decimal spelling std::from_chars reads in range is
    // its value and end (both round correctly), everything else is strtod's (value.cpp)
    float_text_result parse_float_text(const char* text, size_t length) noexcept;
    inline float_text_result parse_float_text(const char* text) noexcept { return parse_float_text(text, std::char_traits<char>::length(text)); }

    // the whole text a float (trailing whitespace allowed; isspace takes an unsigned char), nullopt otherwise
    inline std::optional<double> parse_complete_float_text(const std::string& text) noexcept {
        const auto result = parse_float_text(text.c_str(), text.size());
        if (result.end == text.c_str() || result.out_of_range) return std::nullopt;
        size_t pos = static_cast<size_t>(result.end - text.c_str());
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
        if (pos != text.size()) return std::nullopt;
        return result.value;
    }

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_FLOAT_TEXT_HPP__
