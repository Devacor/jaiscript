#pragma once

#ifndef __JAISCRIPT_DETAIL_INTEGER_OPS_HPP__
#define __JAISCRIPT_DETAIL_INTEGER_OPS_HPP__

// Centralized integer arithmetic with a compile-time overflow policy.
//
// JaiScript evaluates integer +, -, *, /, % both through the dispatch handlers
// (interpreter_dispatch.cpp) and through several inlined fast paths in
// interpreter.cpp. To keep the overflow policy correct and identical everywhere
// — and to keep undefined behavior out of ALL of them — every integer op routes
// through the helpers here.
//
// Policy (compile-time, "safe by default"):
//   JAISCRIPT_CHECKED_OVERFLOW (default 1): overflowing +, -, *, unary -, and
//     INT64_MIN/-1 division are reported as overflow (callers raise a runtime error).
//   JAISCRIPT_WRAP_ON_OVERFLOW: opt into silent two's-complement wraparound (no
//     checks, max speed) — still never undefined behavior.
// `if constexpr (kCheckedOverflow)` removes the unused policy's code entirely, so
// whichever build you ship carries no runtime cost for the other policy.
//
// This header is deliberately NOT included by the widely-included interpreter.hpp,
// so its <intrin.h> dependency (MSVC's _mul128) does not inflate a consumer's
// translation units — only the few engine .cpp files include it.

#include <jaiscript/core/types.hpp>   // script_int
#include <cstdint>
#include <limits>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>                    // _mul128 for single-instruction signed-mul overflow
#endif

#if !defined(JAISCRIPT_CHECKED_OVERFLOW) && !defined(JAISCRIPT_WRAP_ON_OVERFLOW)
#define JAISCRIPT_CHECKED_OVERFLOW 1
#endif

namespace jai {

#if defined(JAISCRIPT_CHECKED_OVERFLOW) && (JAISCRIPT_CHECKED_OVERFLOW != 0)
    inline constexpr bool kCheckedOverflow = true;
#else
    inline constexpr bool kCheckedOverflow = false;
#endif

    namespace ints {

        // --- Wraparound (UB-free) primitives. Computing in unsigned makes overflow
        //     well-defined two's-complement; these emit a single add/sub/imul — the
        //     same machine code as signed `a+b`, but never undefined behavior. ---
        inline script_int wadd(script_int a, script_int b) noexcept { return static_cast<script_int>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b)); }
        inline script_int wsub(script_int a, script_int b) noexcept { return static_cast<script_int>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b)); }
        inline script_int wmul(script_int a, script_int b) noexcept { return static_cast<script_int>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b)); }

        // --- Overflow detectors (only instantiated in checked builds). `out` always
        //     receives the wrapped result; returns true iff a true overflow occurred. ---
        inline bool add_ovf(script_int a, script_int b, script_int& out) noexcept { out = wadd(a, b); return (~(a ^ b) & (a ^ out)) < 0; }
        inline bool sub_ovf(script_int a, script_int b, script_int& out) noexcept { out = wsub(a, b); return ((a ^ b) & (a ^ out)) < 0; }
        inline bool mul_ovf(script_int a, script_int b, script_int& out) noexcept {
#if defined(__GNUC__) || defined(__clang__)
            long long ll = 0;
            const bool ov = __builtin_mul_overflow(static_cast<long long>(a), static_cast<long long>(b), &ll);
            out = static_cast<script_int>(ll);
            return ov;
#elif defined(_MSC_VER) && defined(_M_X64)   // _mul128 is x64-only
            __int64 hi = 0;
            const __int64 lo = _mul128(a, b, &hi);  // 128-bit signed product (one imul)
            out = static_cast<script_int>(lo);
            return hi != (lo >> 63);                // overflow iff high half isn't sign-extension of low
#else
            out = wmul(a, b);
            constexpr script_int kMin = std::numeric_limits<script_int>::min();
            if (a == 0) return false;
            if ((a == -1 && b == kMin) || (b == -1 && a == kMin)) return true;
            return (out / a) != b;
#endif
        }

        // --- Policy-applying ops. Return true on success; in CHECKED builds return
        //     false on overflow (callers then raise a runtime error). `out` always
        //     receives the wrapped result so wrap-builds need no extra work. ---
        inline bool try_add(script_int a, script_int b, script_int& out) noexcept {
            if constexpr (kCheckedOverflow) return !add_ovf(a, b, out);
            else { out = wadd(a, b); return true; }
        }
        inline bool try_sub(script_int a, script_int b, script_int& out) noexcept {
            if constexpr (kCheckedOverflow) return !sub_ovf(a, b, out);
            else { out = wsub(a, b); return true; }
        }
        inline bool try_mul(script_int a, script_int b, script_int& out) noexcept {
            if constexpr (kCheckedOverflow) return !mul_ovf(a, b, out);
            else { out = wmul(a, b); return true; }
        }
        // Division. Caller must ensure b != 0. The only overflow case is INT64_MIN / -1
        // (also a hardware trap): checked builds report it, wrap builds yield INT64_MIN.
        inline bool try_div(script_int a, script_int b, script_int& out) noexcept {
            if (b == -1 && a == std::numeric_limits<script_int>::min()) { out = a; return !kCheckedOverflow; }
            out = a / b;
            return true;
        }
        // Unary negation. -INT64_MIN is the only overflow (and UB); same policy as div.
        inline bool try_neg(script_int a, script_int& out) noexcept {
            if (a == std::numeric_limits<script_int>::min()) { out = a; return !kCheckedOverflow; }
            out = -a;
            return true;
        }
        // Modulo. Caller must ensure b != 0. a % -1 == 0 for all a (and avoids the
        // INT64_MIN % -1 hardware trap); never overflows otherwise.
        inline script_int mod(script_int a, script_int b) noexcept { return (b == -1) ? script_int(0) : (a % b); }

    } // namespace ints
} // namespace jai

#endif // __JAISCRIPT_DETAIL_INTEGER_OPS_HPP__
