#pragma once

#include <string>
#include <string_view>

namespace jai {

class script_value;

namespace detail {

// Format-spec mini-language for template-string splices (`${hp:.1f}`) and the
// stdlib format()/print spec placeholders ({:.1f} / {0:>6}). Deliberate
// std::format subset — documented in docs/site/guide/09-containers-strings.html
// "Template strings & format specs":
//
//   spec      = [[fill] align] [sign] ["0"] [width] [group] ["." precision] [type]
//   align     = "<" | ">" | "^"
//   fill      = any char except "{" and "}" (default " "; requires align)
//   sign      = "+" | " "            (numeric values only: "+" marks non-negatives, " " leaves them a space)
//   "0"       = zero padding after the sign (numeric values only; ignored with an explicit align, as std::format does)
//   width     = [1-9][0-9]*          (max 1024)
//   group     = "," | "_"            (numeric values only: the integer digits in groups of three, "_" in groups of four with
//                                    x / X / b; "," with x / X / b is rejected; with "0" the padding zeros are grouped too)
//   precision = "." [0-9]+           (numeric values only; max 1024)
//   type      = "d" | "x" | "X" | "b" (integers) | "f" | "e" | "g" (numbers)
//
// Everything else ('#', '-' sign, 'o', '%', 'c', 's', nested braces) is rejected.
struct format_spec {
    char fill = ' ';
    char align = 0;      // 0 = default: '>' for int/float, '<' for everything else
    char sign = 0;       // 0, '+', ' '
    bool zero = false;
    int width = 0;
    char group = 0;      // 0, ',', '_'
    int precision = -1;  // -1 = unset
    char type = 0;       // 0, 'd', 'x', 'X', 'b', 'f', 'e', 'g'
};

inline constexpr int format_spec_max_width = 1024;
inline constexpr int format_spec_max_precision = 1024;

inline bool parse_format_spec(std::string_view s, format_spec& out) {
    if (s.empty()) return false;
    size_t i = 0;
    auto is_align = [](char c) { return c == '<' || c == '>' || c == '^'; };
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    if (s.size() >= 2 && is_align(s[1]) && s[0] != '{' && s[0] != '}') {
        out.fill = s[0];
        out.align = s[1];
        i = 2;
    } else if (is_align(s[0])) {
        out.align = s[0];
        i = 1;
    }
    if (i < s.size() && (s[i] == '+' || s[i] == ' ')) {
        out.sign = s[i];
        ++i;
    }
    if (i < s.size() && s[i] == '0') {
        out.zero = true;
        ++i;
    }
    if (i < s.size() && is_digit(s[i])) {
        long w = 0;
        while (i < s.size() && is_digit(s[i])) {
            w = w * 10 + (s[i] - '0');
            if (w > format_spec_max_width) return false;
            ++i;
        }
        out.width = static_cast<int>(w);
    }
    if (i < s.size() && (s[i] == ',' || s[i] == '_')) {
        out.group = s[i];
        ++i;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        if (i >= s.size() || !is_digit(s[i])) return false;
        long p = 0;
        while (i < s.size() && is_digit(s[i])) {
            p = p * 10 + (s[i] - '0');
            if (p > format_spec_max_precision) return false;
            ++i;
        }
        out.precision = static_cast<int>(p);
    }
    if (i < s.size()) {
        char t = s[i];
        if (t != 'f' && t != 'x' && t != 'X' && t != 'b' && t != 'd' && t != 'e' && t != 'g') return false;
        out.type = t;
        ++i;
        if (out.group == ',' && (t == 'x' || t == 'X' || t == 'b')) return false;
    }
    return i == s.size();
}

// ONE runtime formatter shared by the template-string desugar (the engine-core
// `format_value` builtin) and the stdlib format()/print spec placeholders, so
// both execution backends produce byte-identical output and error text by
// construction. Defined in engine.cpp to keep the <charconv> numeric work off
// the universal header path. Throws jai::runtime_error on a type-vs-spec
// mismatch (e.g. `${str:.2f}`).
std::string format_value_with_spec(const script_value& value, const format_spec& spec, std::string_view raw_spec);

// A spec that applies to numbers only (sign, zero padding, precision, a type); width, fill and align apply to any text
inline bool format_spec_needs_number(const format_spec& spec) {
    return spec.sign || spec.zero || spec.precision >= 0 || spec.type != 0;
}

} // namespace detail
} // namespace jai
