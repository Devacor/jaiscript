#pragma once

#ifndef __JAISCRIPT_DETAIL_UTF8_TEXT_HPP__
#define __JAISCRIPT_DETAIL_UTF8_TEXT_HPP__

// utf8_string positions: character counts and character-to-byte translation over the node's caches (ASCII flag, character count,
// two remembered positions, breadcrumbs). cache = false reads without writing (a parallel region runs).

#include <jaiscript/core/value.hpp>
#include <jaiscript/detail/utf8.hpp>
#include <algorithm>

namespace jai::detail::utf8 {

    inline bool ascii(const script_utf8& n, bool cache) {
        if (n.ascii >= 0) return n.ascii == 1;
        bool a = true;
        for (const char c : n.bytes) { if (static_cast<unsigned char>(c) >= 0x80) { a = false; break; } }
        if (cache) n.ascii = a ? 1 : 0;
        return a;
    }
    inline size_t length(const script_utf8& n, bool cache) {
        if (n.chars >= 0) return static_cast<size_t>(n.chars);
        size_t count = 0;
        if (ascii(n, cache)) count = n.bytes.size();
        else for (size_t at = 0; at < n.bytes.size(); ++count) { at += decode(n.bytes, at).length; }
        if (cache) n.chars = static_cast<int64_t>(count);
        return count;
    }
    // the byte offset of character ci (ci == length: the end); the nearest remembered position or breadcrumb at or before ci, or a
    // remembered position a few characters past it (a backward walk), is the start
    inline size_t byte_of(const script_utf8& n, size_t ci, bool cache) {
        const std::string& s = n.bytes;
        if (ascii(n, cache)) return std::min(ci, s.size());
        size_t c = 0, b = 0;
        for (int k = 0; k < 2; ++k) { if (n.hint_char[k] <= ci && n.hint_char[k] >= c) { c = n.hint_char[k]; b = n.hint_byte[k]; } }
        for (int k = 0; k < 2; ++k) {   // a short step back from a remembered position past ci beats a long walk forward
            if (n.hint_char[k] > ci && n.hint_char[k] - ci <= 8 && n.hint_char[k] - ci < ci - c) {
                size_t cc = n.hint_char[k], bb = n.hint_byte[k];
                while (cc > ci && bb > 0) { --bb; while (bb > 0 && (static_cast<unsigned char>(s[bb]) & 0xC0) == 0x80 && decode(s, bb).length == 1) { --bb; } --cc; }
                if (cc == ci) { c = cc; b = bb; }
                break;
            }
        }
        if (cache && n.crumbs.empty() && s.size() >= 512 && ci - c >= 256) {   // long text indexed far from any position: breadcrumbs, once
            n.crumbs.push_back(0);
            size_t cc = 0;
            for (size_t at = 0; at < s.size();) { at += decode(s, at).length; if (++cc % 64 == 0) n.crumbs.push_back(static_cast<uint32_t>(at)); }
        }
        if (!n.crumbs.empty()) { const size_t k = ci / 64; if (k < n.crumbs.size() && k * 64 >= c) { c = k * 64; b = n.crumbs[k]; } }
        while (c < ci && b < s.size()) { b += decode(s, b).length; ++c; }
        if (cache) { const uint8_t slot = n.hint_next; n.hint_char[slot] = static_cast<uint32_t>(c); n.hint_byte[slot] = static_cast<uint32_t>(b); n.hint_next = slot ^ 1; }
        return b;
    }
    // the character index of byte offset b (the start of a character, or the end)
    inline size_t char_of(const script_utf8& n, size_t b, bool cache) {
        if (ascii(n, cache)) return b;
        size_t c = 0;
        for (size_t at = 0; at < b && at < n.bytes.size(); ++c) { at += decode(n.bytes, at).length; }
        return c;
    }
    // a script position (negative counts from the end) as a character index, clamped to [0, len]; -1 when it stays below 0
    inline script_int normalize(script_int pos, size_t len) { if (pos < 0) pos += static_cast<script_int>(len); return pos; }

} // namespace jai::detail::utf8

namespace jai::detail::utf8 {
    // character ci (below length) as a one-character utf8_string: u[i], range-for, at()
    inline script_value char_value(const script_utf8& n, size_t ci, engine* eng, bool cache) {
        const size_t b = byte_of(n, ci, cache);
        return script_value::make_utf8(n.bytes.substr(b, decode(n.bytes, b).length), eng);
    }
}

#endif
