#pragma once

#ifndef __JAISCRIPT_DETAIL_UTF8_HPP__
#define __JAISCRIPT_DETAIL_UTF8_HPP__

#include <cstdint>
#include <string>
#include <string_view>

namespace jai::detail::utf8 {

    inline constexpr uint32_t replacement = 0xFFFD;

    // One step of a UTF-8 walk over bytes (RFC 3629: no overlong forms, no surrogates, nothing above U+10FFFF).
    // Returns the sequence's byte length (1..4) and its code point; a byte that starts no valid sequence is a
    // one-byte step whose code point is U+FFFD and whose `valid` is false.
    struct step { uint32_t length = 1; uint32_t codepoint = replacement; bool valid = false; };

    inline step decode(std::string_view s, size_t at) noexcept {
        const auto byte = [&](size_t i) { return static_cast<uint8_t>(s[i]); };
        const uint8_t b0 = byte(at);
        if (b0 < 0x80) return {1, b0, true};
        uint32_t len = 0, cp = 0, min = 0;
        if ((b0 & 0xE0) == 0xC0) { len = 2; cp = b0 & 0x1F; min = 0x80; }
        else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = b0 & 0x0F; min = 0x800; }
        else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = b0 & 0x07; min = 0x10000; }
        else return {};
        if (at + len > s.size()) return {};
        for (uint32_t k = 1; k < len; ++k) {
            const uint8_t b = byte(at + k);
            if ((b & 0xC0) != 0x80) return {};
            cp = (cp << 6) | (b & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return {};
        return {len, cp, true};
    }

    // The UTF-8 bytes of a code point; false for a surrogate or a value outside 0..U+10FFFF.
    inline bool encode(uint32_t cp, std::string& out) {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        if (cp < 0x80) { out.push_back(static_cast<char>(cp)); }
        else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else { out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        return true;
    }

} // namespace jai::detail::utf8

#endif
