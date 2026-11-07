#pragma once

#ifndef __JAISCRIPT_DETAIL_MATH_INTRINSICS_HPP__
#define __JAISCRIPT_DETAIL_MATH_INTRINSICS_HPP__

#include <jaiscript/core/value.hpp>
#include <jaiscript/core/checked_result.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/numeric_conversion.hpp>
#include <jaiscript/detail/math_values.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace jai::detail {

    // ---- math:: language intrinsics ---------------------------
    // `math::` is a RESERVED namespace owned by the language: both backends recognize
    // `math::<fn>(args)` call shapes and execute through eval_math_intrinsic below -
    // no registration, no environment lookup, no native-boundary crossing. Pure-numeric
    // by design: nobody overrides int+int-class operations, and math:: is ours; user
    // operator overloads (vec + vec, ...) are untouched - their internals decompose to
    // these numerics. ONE enum + ONE name map + ONE kernel = the single source of truth
    // (parity by construction; the lookup ladder: switch/array, no strings past
    // the first resolution, which callers cache on the AST node).
    enum class math_fn : uint8_t {
        floor_, ceil_, itrunc, ifloor,
        sqrt_, pow_, abs_, min_, max_, clamp_,
        sin_, cos_, tan_, atan2_,
        distance, distance_squared,
        radians_to_degrees, degrees_to_radians,
        // Interpolation family (formula-identical to the stdlib
        // registrations they supersede; lerp and mix are the same formula)
        lerp_, mix_, unmix_,
        mix_in_, mix_out_, mix_in_out_, mix_out_in_,
        unmix_in_, unmix_out_, unmix_in_out_, unmix_out_in_,
        // Explicit mod-2^64 integer arithmetic (policy-independent: identical in checked
        // and wrap overflow builds) - the sanctioned spelling for hash folds and RNG steps
        // that would otherwise hand-roll limb products around the checked-overflow error.
        wrap_add_, wrap_sub_, wrap_mul_,
        // The native math types' constructors (math::vec3(1, 2, 3); detail/math_values.hpp construct)
        vec2_, vec3_, ivec2_, ivec3_, vec4_,
        round_, remap_,
        // math::color and its alternative blends
        color_, mix_rgb_, mix_hsv_,
        // the geometry library on vectors (distance and distance_squared take two vectors too)
        dot_, cross_, length_, length_squared_, normalize_, reflect_, project_, angle_between_, perpendicular_, rotate_,
        // the aggregate lowering's component steps (detail/aggregate_lowering.hpp), spelled with a `$` no source can write:
        // f32$(x) a binary32 component's value, i32$(x) an int32 component's, cstore$(x, kind) a component store's
        // narrowing, vcomp$(code, ...) one component of the component-wise family, i32add$(x) .. i32neg$(x) an ivec
        // operator component computed in int64 and narrowed with the operator's text
        f32_, i32_, cstore_, vcomp_, acos_,
        i32_add_, i32_sub_, i32_mul_, i32_div_, i32_neg_,
        // Complete scalar library; appended so existing intrinsic ids stay stable.
        trunc_, cbrt_, exp_, exp2_, log_, log2_, log10_, asin_, acos_public_, atan_, sinh_, cosh_, tanh_, sign_, fmod_, hypot_, saturate_, wrap_, is_nan_, is_inf_,
        count,
        none = count
    };

    inline math_fn math_intrinsic_from_name(std::string_view name) noexcept {
        switch (name.empty() ? '\0' : name.front()) {
            case 'a':
                if (name == "asin") return math_fn::asin_;
                if (name == "acos") return math_fn::acos_public_;
                if (name == "atan") return math_fn::atan_;
                if (name == "abs") return math_fn::abs_;
                if (name == "atan2") return math_fn::atan2_;
                if (name == "angle_between") return math_fn::angle_between_;
                if (name == "acos$") return math_fn::acos_;
                return math_fn::none;
            case 'c':
                if (name == "cbrt") return math_fn::cbrt_;
                if (name == "cosh") return math_fn::cosh_;
                if (name == "cos") return math_fn::cos_;
                if (name == "ceil") return math_fn::ceil_;
                if (name == "clamp") return math_fn::clamp_;
                if (name == "color") return math_fn::color_;
                if (name == "cross") return math_fn::cross_;
                if (name == "cstore$") return math_fn::cstore_;
                return math_fn::none;
            case 'd':
                if (name == "degrees") return math_fn::radians_to_degrees;
                if (name == "distance") return math_fn::distance;
                if (name == "distance_squared") return math_fn::distance_squared;
                if (name == "degrees_to_radians") return math_fn::degrees_to_radians;
                if (name == "dot") return math_fn::dot_;
                return math_fn::none;
            case 'n':
                return name == "normalize" ? math_fn::normalize_ : math_fn::none;
            case 'f':
                if (name == "fmod") return math_fn::fmod_;
                if (name == "floor") return math_fn::floor_;
                if (name == "f32$") return math_fn::f32_;
                return math_fn::none;
            case 'i':
                if (name == "is_nan") return math_fn::is_nan_;
                if (name == "is_inf") return math_fn::is_inf_;
                if (name == "itrunc") return math_fn::itrunc;
                if (name == "ifloor") return math_fn::ifloor;
                if (name == "ivec2") return math_fn::ivec2_;
                if (name == "ivec3") return math_fn::ivec3_;
                if (name == "i32$") return math_fn::i32_;
                if (name == "i32add$") return math_fn::i32_add_;
                if (name == "i32sub$") return math_fn::i32_sub_;
                if (name == "i32mul$") return math_fn::i32_mul_;
                if (name == "i32div$") return math_fn::i32_div_;
                if (name == "i32neg$") return math_fn::i32_neg_;
                return math_fn::none;
            case 'v':
                if (name == "vec2") return math_fn::vec2_;
                if (name == "vec3") return math_fn::vec3_;
                if (name == "vec4") return math_fn::vec4_;
                if (name == "vcomp$") return math_fn::vcomp_;
                return math_fn::none;
            case 'l':
                if (name == "log") return math_fn::log_;
                if (name == "log2") return math_fn::log2_;
                if (name == "log10") return math_fn::log10_;
                if (name == "lerp") return math_fn::lerp_;
                if (name == "length") return math_fn::length_;
                if (name == "length_squared") return math_fn::length_squared_;
                return math_fn::none;
            case 'm':
                if (name == "min") return math_fn::min_;
                if (name == "max") return math_fn::max_;
                if (name == "mix") return math_fn::mix_;
                if (name == "mix_in") return math_fn::mix_in_;
                if (name == "mix_out") return math_fn::mix_out_;
                if (name == "mix_in_out") return math_fn::mix_in_out_;
                if (name == "mix_out_in") return math_fn::mix_out_in_;
                if (name == "mix_rgb") return math_fn::mix_rgb_;
                if (name == "mix_hsv") return math_fn::mix_hsv_;
                return math_fn::none;
            case 'u':
                if (name == "unmix") return math_fn::unmix_;
                if (name == "unmix_in") return math_fn::unmix_in_;
                if (name == "unmix_out") return math_fn::unmix_out_;
                if (name == "unmix_in_out") return math_fn::unmix_in_out_;
                if (name == "unmix_out_in") return math_fn::unmix_out_in_;
                return math_fn::none;
            case 'p':
                if (name == "pow") return math_fn::pow_;
                if (name == "project") return math_fn::project_;
                if (name == "perpendicular") return math_fn::perpendicular_;
                return math_fn::none;
            case 'r':
                if (name == "radians") return math_fn::degrees_to_radians;
                if (name == "radians_to_degrees") return math_fn::radians_to_degrees;
                if (name == "round") return math_fn::round_;
                if (name == "remap") return math_fn::remap_;
                if (name == "reflect") return math_fn::reflect_;
                if (name == "rotate") return math_fn::rotate_;
                return math_fn::none;
            case 's':
                if (name == "sinh") return math_fn::sinh_;
                if (name == "sign") return math_fn::sign_;
                if (name == "saturate") return math_fn::saturate_;
                if (name == "sin") return math_fn::sin_;
                if (name == "sqrt") return math_fn::sqrt_;
                return math_fn::none;
            case 't':
                if (name == "trunc") return math_fn::trunc_;
                if (name == "tanh") return math_fn::tanh_;
                return name == "tan" ? math_fn::tan_ : math_fn::none;
            case 'w':
                if (name == "wrap") return math_fn::wrap_;
                if (name == "wrap_add") return math_fn::wrap_add_;
                if (name == "wrap_sub") return math_fn::wrap_sub_;
                if (name == "wrap_mul") return math_fn::wrap_mul_;
                return math_fn::none;
            case 'h':
                if (name == "hypot") return math_fn::hypot_;
                return math_fn::none;
            case 'e':
                if (name == "exp") return math_fn::exp_;
                if (name == "exp2") return math_fn::exp2_;
                return math_fn::none;
            default:
                return math_fn::none;
        }
    }

    // Shared call-shape recognition (both backends): `math::<fn>` static access.
    // Name-based and engine-independent, so the resolution caches on the AST node
    // (255 = unresolved, 254 = not an intrinsic, else the math_fn id).
    inline math_fn math_intrinsic_for_call(const member_expr* callee) {
        if (callee->math_intrinsic_cache == 255) {
            math_fn fn = math_fn::none;
            if (callee->is_static && callee->object &&
                callee->object->get_type() == node_type::identifier_expr &&
                static_cast<const identifier_expr*>(callee->object.get())->name == "math") {
                fn = math_intrinsic_from_name(callee->member);
            }
            callee->math_intrinsic_cache = fn == math_fn::none ? 254 : static_cast<uint8_t>(fn);
        }
        return callee->math_intrinsic_cache == 254 ? math_fn::none
                                                   : static_cast<math_fn>(callee->math_intrinsic_cache);
    }

    // A stdlib math builtin the VM may evaluate here while its name binds the registered function (engine::register_math_alias):
    // the one-argument float family, whose stdlib (script_float) -> script_float bodies are these expressions verbatim, so an
    // int or float argument yields the same bits either way (abs is the stdlib's float abs: an int argument converts first, unlike
    // math::abs)
    // the math:: rows over two scalars a compiler temp carries (and a cell block runs): min, max, pow, atan2, fmod, hypot and the wrap family
    inline bool math_scalar_pair(math_fn fn) noexcept {
        switch (fn) {
        case math_fn::min_: case math_fn::max_: case math_fn::pow_: case math_fn::atan2_: case math_fn::fmod_: case math_fn::hypot_:
        case math_fn::wrap_add_: case math_fn::wrap_sub_: case math_fn::wrap_mul_:
            return true;
        default: return false;
        }
    }
    // a scalar pair row on two numbers (an int or a float each): min and max pick an operand (an int pair compares exactly, a mixed
    // pair as doubles) and keep its kind, pow, atan2, fmod and hypot compute in doubles, the wrap family takes ints only (false: not this row's
    // operands, the kernel answers with its text)
    struct scalar_num { bool is_int = false; script_int i = 0; script_float f = 0.0; script_float d() const noexcept { return is_int ? static_cast<script_float>(i) : f; } };
    inline bool eval_scalar_pair(math_fn fn, const scalar_num& a, const scalar_num& b, scalar_num& out) noexcept {
        switch (fn) {
        case math_fn::min_: case math_fn::max_: {
            const bool pick_a = a.is_int && b.is_int ? (fn == math_fn::min_ ? a.i <= b.i : a.i >= b.i)
                                                     : (fn == math_fn::min_ ? a.d() <= b.d() : a.d() >= b.d());
            out = pick_a ? a : b;
            return true;
        }
        case math_fn::pow_: out.is_int = false; out.f = std::pow(a.d(), b.d()); return true;
        case math_fn::atan2_: out.is_int = false; out.f = std::atan2(a.d(), b.d()); return true;
        case math_fn::fmod_: out.is_int = false; out.f = std::fmod(a.d(), b.d()); return true;
        case math_fn::hypot_: out.is_int = false; out.f = std::hypot(a.d(), b.d()); return true;
        case math_fn::wrap_add_: case math_fn::wrap_sub_: case math_fn::wrap_mul_: {
            if (!a.is_int || !b.is_int) return false;
            const uint64_t ua = static_cast<uint64_t>(a.i), ub = static_cast<uint64_t>(b.i);
            out.is_int = true;
            out.i = static_cast<script_int>(fn == math_fn::wrap_add_ ? ua + ub : fn == math_fn::wrap_sub_ ? ua - ub : ua * ub);
            return true;
        }
        default: return false;
        }
    }
    // the one-argument rows of the scalar library the general form and a cell block run: a number in (an int converts), a float out
    // (sign an int; abs keeps an int an int through eval_scalar_unary_num)
    inline bool math_scalar_unary(math_fn fn) noexcept {
        switch (fn) {
        case math_fn::abs_: case math_fn::floor_: case math_fn::ceil_: case math_fn::sqrt_: case math_fn::sin_: case math_fn::cos_: case math_fn::tan_:
        case math_fn::radians_to_degrees: case math_fn::degrees_to_radians:
        case math_fn::trunc_: case math_fn::cbrt_: case math_fn::exp_: case math_fn::exp2_: case math_fn::log_: case math_fn::log2_: case math_fn::log10_:
        case math_fn::asin_: case math_fn::acos_public_: case math_fn::atan_: case math_fn::sinh_: case math_fn::cosh_: case math_fn::tanh_:
        case math_fn::saturate_: case math_fn::sign_:
            return true;
        default: return false;
        }
    }
    JAI_FORCEINLINE scalar_num eval_scalar_unary(math_fn fn, script_float x) noexcept {
        scalar_num r;
        switch (fn) {
        case math_fn::abs_: r.f = std::fabs(x); break;
        case math_fn::floor_: r.f = std::floor(x); break;
        case math_fn::ceil_: r.f = std::ceil(x); break;
        case math_fn::sqrt_: r.f = std::sqrt(x); break;
        case math_fn::sin_: r.f = std::sin(x); break;
        case math_fn::cos_: r.f = std::cos(x); break;
        case math_fn::tan_: r.f = std::tan(x); break;
        case math_fn::radians_to_degrees: r.f = x * (180.0 / 3.14159265358979323846); break;
        case math_fn::degrees_to_radians: r.f = x * (3.14159265358979323846 / 180.0); break;
        case math_fn::trunc_: r.f = std::trunc(x); break;
        case math_fn::cbrt_: r.f = std::cbrt(x); break;
        case math_fn::exp_: r.f = std::exp(x); break;
        case math_fn::exp2_: r.f = std::exp2(x); break;
        case math_fn::log_: r.f = std::log(x); break;
        case math_fn::log2_: r.f = std::log2(x); break;
        case math_fn::log10_: r.f = std::log10(x); break;
        case math_fn::asin_: r.f = std::asin(x); break;
        case math_fn::acos_public_: r.f = std::acos(x); break;
        case math_fn::atan_: r.f = std::atan(x); break;
        case math_fn::sinh_: r.f = std::sinh(x); break;
        case math_fn::cosh_: r.f = std::cosh(x); break;
        case math_fn::tanh_: r.f = std::tanh(x); break;
        case math_fn::saturate_: r.f = std::fmax(0.0, std::fmin(x, 1.0)); break;
        case math_fn::sign_: r.is_int = true; r.i = static_cast<script_int>((x > 0.0) - (x < 0.0)); break;
        default: r.is_int = true; r.i = fn == math_fn::is_nan_ ? std::isnan(x) : fn == math_fn::is_inf_ ? std::isinf(x) : 0; break;
        }
        return r;
    }
    // a math_scalar_unary row on a number: abs keeps an int an int (false for INT64_MIN: the kernel raises its overflow text), every
    // other row reads the number as a double
    JAI_FORCEINLINE bool eval_scalar_unary_num(math_fn fn, const scalar_num& a, scalar_num& out) noexcept {
        if (fn == math_fn::abs_ && a.is_int) {
            if (a.i == INT64_MIN) return false;
            out.is_int = true;
            out.i = a.i < 0 ? -a.i : a.i;
            return true;
        }
        out = eval_scalar_unary(fn, a.d());
        return true;
    }
    inline bool math_alias_kernel(math_fn fn) noexcept {
        switch (fn) {
        case math_fn::floor_: case math_fn::ceil_: case math_fn::sqrt_: case math_fn::sin_: case math_fn::cos_: case math_fn::tan_:
        case math_fn::radians_to_degrees: case math_fn::degrees_to_radians: case math_fn::abs_:
            return true;
        default: return false;
        }
    }
    inline script_float eval_math_alias(math_fn fn, script_float x) noexcept { return eval_scalar_unary(fn, x).f; }   // the stdlib alias: an int argument converts first

    // The eased mix/unmix formulas (verbatim from the stdlib registrations), one definition for the scalar rows and the
    // vector rows
    inline script_float eased_value(math_fn fn, script_float start, script_float end, script_float percent, script_float strength) noexcept {
        switch (fn) {
            case math_fn::mix_in_:
                return std::pow(percent, strength) * (end - start) + start;
            case math_fn::mix_out_:
                return (1.0 - std::pow(1.0 - percent, strength)) * (end - start) + start;
            case math_fn::mix_in_out_: {
                const script_float half_range = (end - start) / 2.0 + start;
                return percent < 0.5
                    ? std::pow(percent * 2.0, strength) * (half_range - start) + start
                    : (1.0 - std::pow(1.0 - (percent - 0.5) * 2.0, strength)) * (end - half_range) + half_range;
            }
            case math_fn::mix_out_in_: {
                const script_float half_range = (end - start) / 2.0 + start;
                return percent < 0.5
                    ? (1.0 - std::pow(1.0 - percent * 2.0, strength)) * (half_range - start) + start
                    : std::pow((percent - 0.5) * 2.0, strength) * (end - half_range) + half_range;
            }
            case math_fn::unmix_in_:
                return (end == start) ? end : std::pow((percent - start) / (end - start), 1.0 / strength);
            case math_fn::unmix_out_:
                return 1.0 - std::pow(1.0 - ((percent - start) / (end - start)), 1.0 / strength);
            case math_fn::unmix_in_out_: {
                const script_float half_range = (end - start) / 2.0 + start;
                return percent < half_range
                    ? std::pow((percent - start) / (half_range - start), 1.0 / strength) / 2.0
                    : (1.0 - std::pow(1.0 - ((percent - half_range) / (end - half_range)), 1.0 / strength)) / 2.0 + 0.5;
            }
            default: {   // unmix_out_in_
                const script_float half_range = (end - start) / 2.0 + start;
                return percent < half_range
                    ? (1.0 - std::pow(1.0 - ((percent - start) / (half_range - start)), 1.0 / strength)) / 2.0
                    : std::pow((percent - half_range) / (end - half_range), 1.0 / strength) / 2.0 + 0.5;
            }
        }
    }
    inline script_float remap_value(script_float v, script_float in_lo, script_float in_hi, script_float out_lo, script_float out_hi) noexcept {
        return in_hi == in_lo ? out_lo : out_lo + (v - in_lo) * (out_hi - out_lo) / (in_hi - in_lo);
    }
    inline const char* dims_mismatch_text(math_fn fn) noexcept {
        switch (fn) {
            case math_fn::min_: return "math::min expects vectors of the same dimensions";
            case math_fn::max_: return "math::max expects vectors of the same dimensions";
            case math_fn::clamp_: return "math::clamp expects vectors of the same dimensions";
            case math_fn::lerp_: return "math::lerp expects vectors of the same dimensions";
            case math_fn::mix_: return "math::mix expects vectors of the same dimensions";
            case math_fn::unmix_: return "math::unmix expects vectors of the same dimensions";
            case math_fn::remap_: return "math::remap expects vectors of the same dimensions";
            case math_fn::mix_in_: return "math::mix_in expects vectors of the same dimensions";
            case math_fn::mix_out_: return "math::mix_out expects vectors of the same dimensions";
            case math_fn::mix_in_out_: return "math::mix_in_out expects vectors of the same dimensions";
            case math_fn::mix_out_in_: return "math::mix_out_in expects vectors of the same dimensions";
            case math_fn::unmix_in_: return "math::unmix_in expects vectors of the same dimensions";
            case math_fn::unmix_out_: return "math::unmix_out expects vectors of the same dimensions";
            case math_fn::unmix_in_out_: return "math::unmix_in_out expects vectors of the same dimensions";
            default: return "math::unmix_out_in expects vectors of the same dimensions";
        }
    }

    // ---- color blending --------------------------------------------------------------------------
    // Everything in binary64, landing binary32. OKLab is Bjorn Ottosson's published transform: gamma-encoded sRGB to linear
    // sRGB to LMS (cube root) to Lab and back; the result clamps to the sRGB gamut per channel. Alpha is straight.
    namespace color_math {
        inline double to_linear(double c) noexcept { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
        inline double to_gamma(double c) noexcept { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }
        inline double unit(double c) noexcept { return c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c); }
        inline void to_oklab(const double rgb[3], double lab[3]) noexcept {
            const double r = to_linear(rgb[0]), g = to_linear(rgb[1]), b = to_linear(rgb[2]);
            const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
            const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
            const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
            lab[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
            lab[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
            lab[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
        }
        inline void from_oklab(const double lab[3], double rgb[3]) noexcept {
            const double l_ = lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2];
            const double m_ = lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2];
            const double s_ = lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2];
            const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
            const double lin[3] = {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                                   -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                                   -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
            for (int c = 0; c < 3; ++c) { rgb[c] = unit(to_gamma(unit(lin[c]))); }
        }
        // hue in degrees [0, 360), saturation and value in [0, 1]
        inline void to_hsv(const double rgb[3], double hsv[3]) noexcept {
            const double mx = std::max(rgb[0], std::max(rgb[1], rgb[2])), mn = std::min(rgb[0], std::min(rgb[1], rgb[2])), d = mx - mn;
            double h = 0.0;
            if (d > 0.0) {
                if (mx == rgb[0]) { h = 60.0 * std::fmod((rgb[1] - rgb[2]) / d, 6.0); }
                else if (mx == rgb[1]) { h = 60.0 * ((rgb[2] - rgb[0]) / d + 2.0); }
                else { h = 60.0 * ((rgb[0] - rgb[1]) / d + 4.0); }
                if (h < 0.0) { h += 360.0; }
            }
            hsv[0] = h; hsv[1] = mx > 0.0 ? d / mx : 0.0; hsv[2] = mx;
        }
        inline void from_hsv(const double hsv[3], double rgb[3]) noexcept {
            const double c = hsv[2] * hsv[1], hp = hsv[0] / 60.0, x = c * (1.0 - std::fabs(std::fmod(hp, 2.0) - 1.0)), m = hsv[2] - c;
            double r = 0, g = 0, b = 0;
            switch (static_cast<int>(hp) % 6) {
                case 0: r = c; g = x; break;
                case 1: r = x; g = c; break;
                case 2: g = c; b = x; break;
                case 3: g = x; b = c; break;
                case 4: r = x; b = c; break;
                default: r = c; b = x; break;
            }
            rgb[0] = r + m; rgb[1] = g + m; rgb[2] = b + m;
        }
        enum class space : uint8_t { oklab, rgb, hsv };
        // start, end: r g b a; w the blend weight (already eased for the eased family)
        inline void blend(space how, const double start[4], const double end[4], double w, float out[4]) noexcept {
            double rgb[3] = {0, 0, 0};
            if (how == space::rgb) {
                for (int c = 0; c < 3; ++c) { rgb[c] = start[c] + (end[c] - start[c]) * w; }
            } else if (how == space::oklab) {
                double a[3], b[3], lab[3];
                to_oklab(start, a);
                to_oklab(end, b);
                for (int c = 0; c < 3; ++c) { lab[c] = a[c] + (b[c] - a[c]) * w; }
                from_oklab(lab, rgb);
            } else {
                double a[3], b[3];
                to_hsv(start, a);
                to_hsv(end, b);
                if (a[1] < 1e-6) { a[0] = b[0]; }   // an achromatic endpoint takes the other's hue
                if (b[1] < 1e-6) { b[0] = a[0]; }
                double d = b[0] - a[0];
                if (d > 180.0) { d -= 360.0; }
                if (d < -180.0) { d += 360.0; }
                double h = std::fmod(a[0] + d * w, 360.0);
                if (h < 0.0) { h += 360.0; }
                const double hsv[3] = {h, a[1] + (b[1] - a[1]) * w, a[2] + (b[2] - a[2]) * w};
                from_hsv(hsv, rgb);
            }
            for (int c = 0; c < 3; ++c) { out[c] = static_cast<float>(rgb[c]); }
            out[3] = static_cast<float>(start[3] + (end[3] - start[3]) * w);
        }
    }
    inline const char* color_mix_text(math_fn fn) noexcept {
        switch (fn) {
            case math_fn::mix_rgb_: return "math::mix_rgb expects (math::color, math::color, float)";
            case math_fn::mix_hsv_: return "math::mix_hsv expects (math::color, math::color, float)";
            case math_fn::lerp_: return "math::lerp on colors expects (math::color, math::color, float)";
            case math_fn::mix_: return "math::mix on colors expects (math::color, math::color, float)";
            default: return "an eased mix on colors expects (math::color, math::color, float, float)";
        }
    }
    // mix, lerp and the eased mixes with color endpoints (OKLab), mix_rgb and mix_hsv: both endpoints colors, t (and the
    // eased strength) scalars
    JAI_MATH_COLD inline checked_result<script_value> eval_color_mix(math_fn fn, const script_value* const* args, size_t argc, engine* eng) {
        const bool eased = fn != math_fn::mix_ && fn != math_fn::lerp_ && fn != math_fn::mix_rgb_ && fn != math_fn::mix_hsv_;
        auto bad = [&] { return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), color_mix_text(fn)); };
        if (argc != (eased ? 4u : 3u)) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), color_mix_text(fn));
        }
        double ends[2][4];
        for (int e = 0; e < 2; ++e) {
            if (math::kind_of(*args[e]) != script_value_type::jai_color_type) { return bad(); }
            for (size_t c = 0; c < 4; ++c) { ends[e][c] = static_cast<double>(math::f32_at(*args[e], c)); }
        }
        const math::scalar t = math::read_scalar(*args[2]);
        if (!t.ok) { return bad(); }
        double w = t.is_int ? static_cast<double>(t.i) : t.f;
        if (eased) {
            const math::scalar k = math::read_scalar(*args[3]);
            if (!k.ok) { return bad(); }
            w = eased_value(fn, 0.0, 1.0, w, k.is_int ? static_cast<double>(k.i) : k.f);
        }
        const color_math::space how = fn == math_fn::mix_rgb_ ? color_math::space::rgb
                                    : fn == math_fn::mix_hsv_ ? color_math::space::hsv : color_math::space::oklab;
        float out[4];
        color_math::blend(how, ends[0], ends[1], w, out);
        int32_t unused[4] = {0, 0, 0, 0};
        return math::make(script_value_type::jai_color_type, out, unused, eng);
    }

    // One component of the component-wise family: x holds the component's operands in argument order (a scalar argument
    // repeated), widened to binary64 (the float rows) or kept as ints (abs, min, max and clamp over integral operands)
    inline script_float vec_component_value(math_fn fn, const script_float* x) noexcept {
        const script_float a = x[0];
        switch (fn) {
            case math_fn::floor_: return std::floor(a);
            case math_fn::ceil_: return std::ceil(a);
            case math_fn::round_: return std::round(a);
            case math_fn::abs_: return std::fabs(a);
            case math_fn::min_: return a <= x[1] ? a : x[1];
            case math_fn::max_: return a >= x[1] ? a : x[1];
            case math_fn::clamp_: return a < x[1] ? x[1] : (a > x[2] ? x[2] : a);
            case math_fn::lerp_: case math_fn::mix_: return (x[2] * (x[1] - a)) + a;
            case math_fn::unmix_: return (x[1] == a) ? 0.0 : (x[2] - a) / (x[1] - a);
            case math_fn::remap_: return remap_value(a, x[1], x[2], x[3], x[4]);
            default: return eased_value(fn, a, x[1], x[2], x[3]);
        }
    }
    inline script_int vec_component_exact(math_fn fn, const script_int* x) noexcept {
        switch (fn) {
            case math_fn::abs_: return x[0] < 0 ? -x[0] : x[0];
            case math_fn::min_: return x[0] <= x[1] ? x[0] : x[1];
            case math_fn::max_: return x[0] >= x[1] ? x[0] : x[1];
            default: return x[0] < x[1] ? x[1] : (x[0] > x[2] ? x[2] : x[0]);
        }
    }

    // The vector rows of the component-wise family (floor, ceil, round, abs, min, max, clamp, lerp, mix, unmix, remap and
    // the eased mixes): reached only when a scalar row found an operand that is not a number. Every vector operand has the
    // same dimensions; a scalar operand applies to every component (a vector t gives each component its own). Each component
    // runs the scalar formula in binary64 on the widened components and lands as binary32, so a result is a vec of the
    // operands' dimensions, and an ivec operand never truncates silently. abs, min, max and clamp keep ivec exact when every
    // operand is integral (a scalar outside int32 follows the integer policy). Anything else is the scalar row's text.
    JAI_MATH_COLD inline checked_result<script_value> eval_vec_math(math_fn fn, const script_value* const* args, size_t argc,
                                                                    engine* eng, const char* type_text) {
        struct operand {
            bool vec = false;
            bool integral = false;
            size_t dims = 0;
            script_float f[math::k_max_dims] = {0, 0, 0, 0};
            script_int i[math::k_max_dims] = {0, 0, 0, 0};
        };
        auto type_error = [&] { return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), type_text); };
        constexpr size_t k_max_args = 5;
        if (argc > k_max_args) { return type_error(); }
        operand ops[k_max_args];
        size_t dims = 0;
        bool any_color = false, all_vec_color = true;
        for (size_t a = 0; a < argc; ++a) {
            script_value decoded(std::monostate{}, eng);
            const script_value* v = args[a];
            if (v->raw_storage_index() == script_value::TYPEID_CPP_BOUND) { decoded = v->bound_decoded_temp(); v = &decoded; }
            const size_t raw = v->raw_storage_index();
            operand& o = ops[a];
            if (math::is_kind(raw)) {
                const script_value_type k = v->unchecked_vec_kind();
                any_color = any_color || math::is_color(k);
                all_vec_color = all_vec_color && math::is_color(k);
                o.vec = true;
                o.integral = math::integral(k);
                o.dims = math::dims(k);
                if (dims != 0 && o.dims != dims) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), dims_mismatch_text(fn));
                }
                dims = o.dims;
                for (size_t c = 0; c < o.dims; ++c) {
                    if (o.integral) { o.i[c] = math::i32_at(*v, c); o.f[c] = static_cast<script_float>(o.i[c]); }
                    else { o.f[c] = static_cast<script_float>(math::f32_at(*v, c)); }
                }
            } else if (raw == script_value::TYPEID_INT) {
                o.integral = true;
                for (size_t c = 0; c < math::k_max_dims; ++c) { o.i[c] = v->unchecked_as_int(); o.f[c] = static_cast<script_float>(o.i[c]); }
            } else if (raw == script_value::TYPEID_FLOAT) {
                for (size_t c = 0; c < math::k_max_dims; ++c) { o.f[c] = v->unchecked_as_float(); }
            } else {
                return type_error();
            }
        }
        if (dims == 0) { return type_error(); }
        if (any_color) {
            switch (fn) {
                case math_fn::lerp_: case math_fn::mix_: case math_fn::mix_in_: case math_fn::mix_out_:
                case math_fn::mix_in_out_: case math_fn::mix_out_in_:
                    return eval_color_mix(fn, args, argc, eng);
                default: break;   // the other rows are per channel
            }
            if (!all_vec_color) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "a math::color combines only with colors and scalars");
            }
        }

        const bool exact_family = fn == math_fn::abs_ || fn == math_fn::min_ || fn == math_fn::max_ || fn == math_fn::clamp_;
        bool all_integral = true;
        for (size_t a = 0; a < argc; ++a) { all_integral = all_integral && ops[a].integral; }
        float f[math::k_max_dims] = {0, 0, 0, 0};
        int32_t out[math::k_max_dims] = {0, 0, 0, 0};
        if (exact_family && all_integral) {
            for (size_t c = 0; c < dims; ++c) {
                script_int x[k_max_args] = {0, 0, 0, 0, 0};
                for (size_t a = 0; a < argc; ++a) { x[a] = ops[a].i[c]; }
                auto narrowed = math::to_i32(vec_component_exact(fn, x));
                if (!narrowed) { return narrowed.error_value(); }
                out[c] = narrowed.value();
            }
            return math::make(math::family_kind(true, dims), f, out, eng);
        }
        for (size_t c = 0; c < dims; ++c) {
            script_float x[k_max_args] = {0, 0, 0, 0, 0};
            for (size_t a = 0; a < argc; ++a) { x[a] = ops[a].f[c]; }
            f[c] = math::to_f32(vec_component_value(fn, x));
        }
        return math::make(any_color ? script_value_type::jai_color_type : math::family_kind(false, dims), f, out, eng);
    }

    // math::vec2(...) ... math::ivec3(...): out of line, so the constructor's temporaries stay off the scalar kernel's frame
    JAI_MATH_COLD inline checked_result<script_value> eval_vec_constructor(math_fn fn, const script_value* const* args, size_t argc, engine* eng) {
        const script_value_type kind = fn == math_fn::vec2_ ? script_value_type::jai_vec2_type
                                     : fn == math_fn::vec3_ ? script_value_type::jai_vec3_type
                                     : fn == math_fn::vec4_ ? script_value_type::jai_vec4_type
                                     : fn == math_fn::color_ ? script_value_type::jai_color_type
                                     : fn == math_fn::ivec2_ ? script_value_type::jai_ivec2_type : script_value_type::jai_ivec3_type;
        if (argc > math::k_max_dims) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), math::ctor_arity_text(kind));
        }
        // a bound argument decodes once (a host variable reads its current value)
        script_value decoded[math::k_max_dims] = {script_value(std::monostate{}, eng), script_value(std::monostate{}, eng),
                                                 script_value(std::monostate{}, eng), script_value(std::monostate{}, eng)};
        const script_value* view[math::k_max_dims] = {nullptr, nullptr, nullptr, nullptr};
        for (size_t k = 0; k < argc; ++k) {
            if (args[k]->raw_storage_index() == script_value::TYPEID_CPP_BOUND) {
                decoded[k] = args[k]->bound_decoded_temp();
                view[k] = &decoded[k];
            } else {
                view[k] = args[k];
            }
        }
        return math::construct(kind, view, argc, eng);
    }

    // ---- the geometry library ------------------------------------------------------------
    // Vectors only (a color has no geometry). A scalar result is binary64 from the widened components; a vector result is
    // computed in binary64 and lands as binary32, so an ivec argument gives a vec. The integral answers stay exact when
    // every vector argument is an ivec: dot, length_squared, distance_squared, cross and perpendicular, each step with the
    // scalar operator's overflow text (integer policy) and an ivec component narrowed to int32. A zero vector has no
    // direction: normalize and project onto it give the zero vector, angle_between with it gives 0, rotate about it leaves
    // the vector as it is.
    namespace geometry {
        struct operand {
            bool integral = false;
            size_t dims = 0;
            script_float f[math::k_max_dims] = {0, 0, 0, 0};
            script_int i[math::k_max_dims] = {0, 0, 0, 0};
        };
        // a vector argument (a bound one decoded); false for anything else, a color included
        inline bool read(const script_value& arg, operand& out, engine* eng) {
            script_value decoded(std::monostate{}, eng);
            const script_value* v = &arg;
            if (v->raw_storage_index() == script_value::TYPEID_CPP_BOUND) { decoded = v->bound_decoded_temp(); v = &decoded; }
            if (!math::is_kind(v->raw_storage_index())) { return false; }
            const script_value_type k = v->unchecked_vec_kind();
            if (math::is_color(k)) { return false; }
            out.integral = math::integral(k);
            out.dims = math::dims(k);
            for (size_t c = 0; c < out.dims; ++c) {
                if (out.integral) { out.i[c] = math::i32_at(*v, c); out.f[c] = static_cast<script_float>(out.i[c]); }
                else { out.f[c] = static_cast<script_float>(math::f32_at(*v, c)); }
            }
            return true;
        }
        // the scalar operators' checked int64 steps (the texts of `+`, `-` and `*` on ints)
        inline bool add(script_int a, script_int b, script_int& r) noexcept {
            const uint64_t u = static_cast<uint64_t>(a) + static_cast<uint64_t>(b);
            r = static_cast<script_int>(u);
            return !(((a ^ r) & (b ^ r)) < 0);
        }
        inline bool sub(script_int a, script_int b, script_int& r) noexcept {
            const uint64_t u = static_cast<uint64_t>(a) - static_cast<uint64_t>(b);
            r = static_cast<script_int>(u);
            return !(((a ^ b) & (a ^ r)) < 0);
        }
        inline bool mul(script_int a, script_int b, script_int& r) noexcept {
            r = static_cast<script_int>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
            if (a == 0 || b == 0) { return true; }
            const uint64_t ua = a < 0 ? 0 - static_cast<uint64_t>(a) : static_cast<uint64_t>(a);
            const uint64_t ub = b < 0 ? 0 - static_cast<uint64_t>(b) : static_cast<uint64_t>(b);
            const uint64_t limit = (a < 0) != (b < 0) ? (uint64_t{1} << 63) : (uint64_t{1} << 63) - 1;
            return ua <= limit / ub;
        }
        inline checked_result<script_value> overflow(math::bop op) {
            return checked_result<script_value>(make_error_code(runtime_error_code::invalid_numeric_operand), math::overflow_text(op));
        }
        // angle_between's last step: the cosine clamped to [-1, 1], then acos
        inline double clamped_acos(double c) noexcept { return std::acos(c < -1.0 ? -1.0 : (c > 1.0 ? 1.0 : c)); }
        // a step that overflowed: the error in a checked build, the wrapped value otherwise
        inline bool fails(bool ok) noexcept { return !ok && math::k_checked_overflow; }

        inline double dot(const operand& a, const operand& b) noexcept {
            double s = a.f[0] * b.f[0];
            for (size_t c = 1; c < a.dims; ++c) { s = s + a.f[c] * b.f[c]; }
            return s;
        }
        inline script_value vec_of(size_t dims, const double* comps, engine* eng) {
            float f[math::k_max_dims] = {0, 0, 0, 0};
            int32_t unused[math::k_max_dims] = {0, 0, 0, 0};
            for (size_t c = 0; c < dims; ++c) { f[c] = math::to_f32(comps[c]); }
            return math::make(math::family_kind(false, dims), f, unused, eng);
        }

        constexpr const char* arity_text(math_fn fn) noexcept {
            switch (fn) {
                case math_fn::dot_: return "math::dot expects 2 arguments";
                case math_fn::cross_: return "math::cross expects 2 arguments";
                case math_fn::length_: return "math::length expects 1 argument";
                case math_fn::length_squared_: return "math::length_squared expects 1 argument";
                case math_fn::normalize_: return "math::normalize expects 1 argument";
                case math_fn::reflect_: return "math::reflect expects 2 arguments (vector, normal)";
                case math_fn::project_: return "math::project expects 2 arguments (vector, onto)";
                case math_fn::angle_between_: return "math::angle_between expects 2 arguments";
                case math_fn::perpendicular_: return "math::perpendicular expects 1 argument";
                case math_fn::rotate_: return "math::rotate expects (vec2, angle) or (vec3, axis, angle)";
                case math_fn::distance: return "math::distance expects 2 vectors or 4 arguments (x1, y1, x2, y2)";
                default: return "math::distance_squared expects 2 vectors or 4 arguments (x1, y1, x2, y2)";
            }
        }
        inline const char* type_text(math_fn fn) noexcept {
            switch (fn) {
                case math_fn::dot_: return "math::dot expects two vectors of the same dimensions";
                case math_fn::cross_: return "math::cross expects two vec2 or two vec3 vectors";
                case math_fn::length_: return "math::length expects a vector";
                case math_fn::length_squared_: return "math::length_squared expects a vector";
                case math_fn::normalize_: return "math::normalize expects a vector";
                case math_fn::reflect_: return "math::reflect expects two vectors of the same dimensions";
                case math_fn::project_: return "math::project expects two vectors of the same dimensions";
                case math_fn::angle_between_: return "math::angle_between expects two vectors of the same dimensions";
                case math_fn::perpendicular_: return "math::perpendicular expects a vec2";
                case math_fn::rotate_: return "math::rotate expects (vec2, angle) or (vec3, axis, angle)";
                case math_fn::distance: return "math::distance expects two vectors of the same dimensions";
                default: return "math::distance_squared expects two vectors of the same dimensions";
            }
        }
    }

    // The argument counts a math:: library function takes and the text any other count raises: one table for the static
    // checker (a load error), the aggregate lowering (an unmatched count keeps the boxed call) and the kernel both backends
    // run. `low`..`high` are the counts; `also` one more count outside that range (0 = none). The constructors and the
    // lowering's own steps check their arguments themselves (0..255).
    struct math_arity { uint8_t low = 0; uint8_t high = 255; uint8_t also = 0; const char* text = nullptr; };
    constexpr math_arity math_arity_of(math_fn fn) noexcept {
        switch (fn) {
            case math_fn::trunc_: return {1, 1, 0, "math::trunc expects 1 argument"};
            case math_fn::cbrt_: return {1, 1, 0, "math::cbrt expects 1 argument"};
            case math_fn::exp_: return {1, 1, 0, "math::exp expects 1 argument"};
            case math_fn::exp2_: return {1, 1, 0, "math::exp2 expects 1 argument"};
            case math_fn::log_: return {1, 1, 0, "math::log expects 1 argument"};
            case math_fn::log2_: return {1, 1, 0, "math::log2 expects 1 argument"};
            case math_fn::log10_: return {1, 1, 0, "math::log10 expects 1 argument"};
            case math_fn::asin_: return {1, 1, 0, "math::asin expects 1 argument"};
            case math_fn::acos_public_: return {1, 1, 0, "math::acos expects 1 argument"};
            case math_fn::atan_: return {1, 1, 0, "math::atan expects 1 argument"};
            case math_fn::sinh_: return {1, 1, 0, "math::sinh expects 1 argument"};
            case math_fn::cosh_: return {1, 1, 0, "math::cosh expects 1 argument"};
            case math_fn::tanh_: return {1, 1, 0, "math::tanh expects 1 argument"};
            case math_fn::sign_: return {1, 1, 0, "math::sign expects 1 argument"};
            case math_fn::fmod_: return {2, 2, 0, "math::fmod expects 2 arguments"};
            case math_fn::hypot_: return {2, 2, 0, "math::hypot expects 2 arguments"};
            case math_fn::saturate_: return {1, 1, 0, "math::saturate expects 1 argument"};
            case math_fn::wrap_: return {3, 3, 0, "math::wrap expects 3 arguments"};
            case math_fn::is_nan_: return {1, 1, 0, "math::is_nan expects 1 argument"};
            case math_fn::is_inf_: return {1, 1, 0, "math::is_inf expects 1 argument"};
            case math_fn::floor_: return {1, 1, 0, "math::floor expects 1 argument"};
            case math_fn::ceil_: return {1, 1, 0, "math::ceil expects 1 argument"};
            case math_fn::round_: return {1, 1, 0, "math::round expects 1 argument"};
            case math_fn::itrunc: return {1, 1, 0, "math::itrunc expects 1 argument"};
            case math_fn::ifloor: return {1, 1, 0, "math::ifloor expects 1 argument"};
            case math_fn::sqrt_: return {1, 1, 0, "math::sqrt expects 1 argument"};
            case math_fn::pow_: return {2, 2, 0, "math::pow expects 2 arguments"};
            case math_fn::abs_: return {1, 1, 0, "math::abs expects 1 argument"};
            case math_fn::min_: return {2, 2, 0, "math::min expects 2 arguments"};
            case math_fn::max_: return {2, 2, 0, "math::max expects 2 arguments"};
            case math_fn::clamp_: return {3, 3, 0, "math::clamp expects 3 arguments (value, lo, hi)"};
            case math_fn::sin_: return {1, 1, 0, "math::sin expects 1 argument"};
            case math_fn::cos_: return {1, 1, 0, "math::cos expects 1 argument"};
            case math_fn::tan_: return {1, 1, 0, "math::tan expects 1 argument"};
            case math_fn::atan2_: return {2, 2, 0, "math::atan2 expects 2 arguments (y, x)"};
            case math_fn::distance: return {2, 2, 4, "math::distance expects 2 vectors or 4 arguments (x1, y1, x2, y2)"};
            case math_fn::distance_squared: return {2, 2, 4, "math::distance_squared expects 2 vectors or 4 arguments (x1, y1, x2, y2)"};
            case math_fn::radians_to_degrees: return {1, 1, 0, "math::radians_to_degrees expects 1 argument"};
            case math_fn::degrees_to_radians: return {1, 1, 0, "math::degrees_to_radians expects 1 argument"};
            case math_fn::wrap_add_: return {2, 2, 0, "math::wrap_add expects 2 arguments"};
            case math_fn::wrap_sub_: return {2, 2, 0, "math::wrap_sub expects 2 arguments"};
            case math_fn::wrap_mul_: return {2, 2, 0, "math::wrap_mul expects 2 arguments"};
            case math_fn::lerp_: return {3, 3, 0, "math::lerp expects 3 arguments (start, end, percent)"};
            case math_fn::mix_: return {3, 3, 0, "math::mix expects 3 arguments (start, end, percent)"};
            case math_fn::unmix_: return {3, 3, 0, "math::unmix expects 3 arguments (start, end, value)"};
            case math_fn::mix_in_: case math_fn::mix_out_: case math_fn::mix_in_out_: case math_fn::mix_out_in_:
            case math_fn::unmix_in_: case math_fn::unmix_out_: case math_fn::unmix_in_out_: case math_fn::unmix_out_in_:
                return {4, 4, 0, "math:: eased mix/unmix expects 4 arguments (start, end, percent_or_value, strength)"};
            case math_fn::remap_: return {5, 5, 0, "math::remap expects 5 arguments (value, in_lo, in_hi, out_lo, out_hi)"};
            case math_fn::mix_rgb_: return {3, 3, 0, "math::mix_rgb expects (math::color, math::color, float)"};
            case math_fn::mix_hsv_: return {3, 3, 0, "math::mix_hsv expects (math::color, math::color, float)"};
            case math_fn::dot_: case math_fn::cross_: case math_fn::reflect_: case math_fn::project_: case math_fn::angle_between_:
                return {2, 2, 0, geometry::arity_text(fn)};
            case math_fn::length_: case math_fn::length_squared_: case math_fn::normalize_: case math_fn::perpendicular_:
                return {1, 1, 0, geometry::arity_text(fn)};
            case math_fn::rotate_: return {2, 3, 0, geometry::arity_text(fn)};
            default: return {};
        }
    }
    constexpr bool math_arity_accepts(const math_arity& a, size_t argc) noexcept {
        return (argc >= a.low && argc <= a.high) || (a.also != 0 && argc == a.also);
    }
    constexpr bool math_arity_accepts(math_fn fn, size_t argc) noexcept { return math_arity_accepts(math_arity_of(fn), argc); }
    // the kernel's per-case form: the counts fold to constants
    template <math_fn F>
    constexpr bool math_arity_ok(size_t argc) noexcept {
        constexpr math_arity arity = math_arity_of(F);
        return math_arity_accepts(arity, argc);
    }

    // dot, cross, length, length_squared, normalize, reflect, project, angle_between, perpendicular, rotate, and distance /
    // distance_squared over two vectors
    JAI_MATH_COLD inline checked_result<script_value> eval_geometry(math_fn fn, const script_value* const* args, size_t argc, engine* eng) {
        using geometry::operand;
        auto bad_type = [&] { return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), geometry::type_text(fn)); };
        size_t want = 2;
        switch (fn) {
            case math_fn::length_: case math_fn::length_squared_: case math_fn::normalize_: case math_fn::perpendicular_: want = 1; break;
            case math_fn::rotate_: want = argc == 3 ? 3 : 2; break;
            default: break;
        }
        if (argc != want) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), geometry::arity_text(fn));
        }
        operand a, b;
        if (!geometry::read(*args[0], a, eng)) { return bad_type(); }
        const bool two_vectors = fn != math_fn::length_ && fn != math_fn::length_squared_ && fn != math_fn::normalize_ &&
                                 fn != math_fn::perpendicular_ && !(fn == math_fn::rotate_ && argc == 2);
        if (two_vectors) {
            if (!geometry::read(*args[1], b, eng) || b.dims != a.dims) { return bad_type(); }
        }
        const bool exact = a.integral && (!two_vectors || b.integral);
        const size_t n = a.dims;
        switch (fn) {
            case math_fn::dot_:
            case math_fn::length_squared_: {
                const operand& r = fn == math_fn::dot_ ? b : a;
                if (exact) {
                    script_int s = a.i[0] * r.i[0];
                    for (size_t c = 1; c < n; ++c) {
                        if (geometry::fails(geometry::add(s, a.i[c] * r.i[c], s))) { return geometry::overflow(math::bop::add); }
                    }
                    return script_value(s, eng);
                }
                return script_value(geometry::dot(a, r), eng);
            }
            case math_fn::length_:
                return script_value(std::sqrt(geometry::dot(a, a)), eng);
            case math_fn::distance:
            case math_fn::distance_squared: {
                if (exact && fn == math_fn::distance_squared) {
                    script_int s = 0;
                    for (size_t c = 0; c < n; ++c) {
                        const script_int d = b.i[c] - a.i[c];
                        script_int sq = 0;
                        if (geometry::fails(geometry::mul(d, d, sq))) { return geometry::overflow(math::bop::mul); }
                        if (c == 0) { s = sq; continue; }
                        if (geometry::fails(geometry::add(s, sq, s))) { return geometry::overflow(math::bop::add); }
                    }
                    return script_value(s, eng);
                }
                double s = 0.0;
                for (size_t c = 0; c < n; ++c) {
                    const double d = b.f[c] - a.f[c];
                    s = c == 0 ? d * d : s + d * d;
                }
                return script_value(fn == math_fn::distance ? std::sqrt(s) : s, eng);
            }
            case math_fn::normalize_: {
                const double len = std::sqrt(geometry::dot(a, a));
                double out[math::k_max_dims] = {0, 0, 0, 0};
                if (len != 0.0) { for (size_t c = 0; c < n; ++c) { out[c] = a.f[c] / len; } }
                return geometry::vec_of(n, out, eng);
            }
            case math_fn::cross_: {
                if (n == 2) {
                    if (exact) {
                        script_int r = 0;
                        if (geometry::fails(geometry::sub(a.i[0] * b.i[1], a.i[1] * b.i[0], r))) { return geometry::overflow(math::bop::sub); }
                        return script_value(r, eng);
                    }
                    return script_value(a.f[0] * b.f[1] - a.f[1] * b.f[0], eng);
                }
                if (n != 3) { return bad_type(); }
                if (exact) {
                    float unused[math::k_max_dims] = {0, 0, 0, 0};
                    int32_t out[math::k_max_dims] = {0, 0, 0, 0};
                    static constexpr size_t p[3] = {1, 2, 0}, q[3] = {2, 0, 1};
                    for (size_t c = 0; c < 3; ++c) {
                        script_int r = 0;
                        if (geometry::fails(geometry::sub(a.i[p[c]] * b.i[q[c]], a.i[q[c]] * b.i[p[c]], r))) { return geometry::overflow(math::bop::sub); }
                        auto narrowed = math::to_i32(r);
                        if (!narrowed) { return narrowed.error_value(); }
                        out[c] = narrowed.value();
                    }
                    return math::make(script_value_type::jai_ivec3_type, unused, out, eng);
                }
                const double out[3] = {a.f[1] * b.f[2] - a.f[2] * b.f[1], a.f[2] * b.f[0] - a.f[0] * b.f[2], a.f[0] * b.f[1] - a.f[1] * b.f[0]};
                return geometry::vec_of(3, out, eng);
            }
            case math_fn::reflect_: {
                const double d = geometry::dot(a, b);
                double out[math::k_max_dims] = {0, 0, 0, 0};
                for (size_t c = 0; c < n; ++c) { out[c] = a.f[c] - 2.0 * d * b.f[c]; }
                return geometry::vec_of(n, out, eng);
            }
            case math_fn::project_: {
                const double d = geometry::dot(b, b);
                double out[math::k_max_dims] = {0, 0, 0, 0};
                if (d != 0.0) {
                    const double s = geometry::dot(a, b) / d;
                    for (size_t c = 0; c < n; ++c) { out[c] = b.f[c] * s; }
                }
                return geometry::vec_of(n, out, eng);
            }
            case math_fn::angle_between_: {
                const double la = std::sqrt(geometry::dot(a, a)), lb = std::sqrt(geometry::dot(b, b));
                if (la == 0.0 || lb == 0.0) { return script_value(0.0, eng); }
                return script_value(geometry::clamped_acos(geometry::dot(a, b) / (la * lb)), eng);
            }
            case math_fn::perpendicular_: {
                if (n != 2) { return bad_type(); }
                if (a.integral) {
                    auto x = math::to_i32(-a.i[1]);
                    if (!x) { return x.error_value(); }
                    float unused[math::k_max_dims] = {0, 0, 0, 0};
                    int32_t out[math::k_max_dims] = {x.value(), static_cast<int32_t>(a.i[0]), 0, 0};
                    return math::make(script_value_type::jai_ivec2_type, unused, out, eng);
                }
                const double out[2] = {-a.f[1], a.f[0]};
                return geometry::vec_of(2, out, eng);
            }
            default: {   // rotate
                const math::scalar t = math::read_scalar(*args[argc - 1]);
                if (!t.ok) { return bad_type(); }
                const double angle = t.is_int ? static_cast<double>(t.i) : t.f;
                const double cs = std::cos(angle), sn = std::sin(angle);
                if (argc == 2) {
                    if (n != 2) { return bad_type(); }
                    const double out[2] = {a.f[0] * cs - a.f[1] * sn, a.f[0] * sn + a.f[1] * cs};
                    return geometry::vec_of(2, out, eng);
                }
                if (n != 3) { return bad_type(); }
                const double len = std::sqrt(geometry::dot(b, b));
                if (len == 0.0) { return geometry::vec_of(3, a.f, eng); }
                const double k[3] = {b.f[0] / len, b.f[1] / len, b.f[2] / len};
                const double kv = k[0] * a.f[0] + k[1] * a.f[1] + k[2] * a.f[2];
                const double kx[3] = {k[1] * a.f[2] - k[2] * a.f[1], k[2] * a.f[0] - k[0] * a.f[2], k[0] * a.f[1] - k[1] * a.f[0]};
                double out[3];
                for (size_t c = 0; c < 3; ++c) { out[c] = a.f[c] * cs + kx[c] * sn + k[c] * kv * (1.0 - cs); }
                return geometry::vec_of(3, out, eng);
            }
        }
    }

    // The methods on a vector (never a color): v.length(), length_squared(), normalized(), dot(b), cross(b), distance_to(b),
    // rotated(angle) on a vec2 and rotated(axis, angle) on a vec3 - the free functions with the receiver first
    inline math_fn vec_method_fn(script_value_type kind, std::string_view name) noexcept {
        if (math::is_color(kind) || name.size() < 3) { return math_fn::none; }
        switch (name[0]) {
            case 'l':
                if (name == "length") return math_fn::length_;
                if (name == "length_squared") return math_fn::length_squared_;
                return math_fn::none;
            case 'n': return name == "normalized" ? math_fn::normalize_ : math_fn::none;
            case 'd':
                if (name == "dot") return math_fn::dot_;
                if (name == "distance_to") return math_fn::distance;
                return math_fn::none;
            case 'c': return name == "cross" ? math_fn::cross_ : math_fn::none;
            case 'r': return name == "rotated" ? math_fn::rotate_ : math_fn::none;
            default: return math_fn::none;
        }
    }
    inline const char* vec_method_arity_text(math_fn fn, script_value_type kind) noexcept {
        switch (fn) {
            case math_fn::length_: return "'length' expects 0 arguments";
            case math_fn::length_squared_: return "'length_squared' expects 0 arguments";
            case math_fn::normalize_: return "'normalized' expects 0 arguments";
            case math_fn::dot_: return "'dot' expects 1 argument";
            case math_fn::cross_: return "'cross' expects 1 argument";
            case math_fn::distance: return "'distance_to' expects 1 argument";
            default: return math::dims(kind) == 3 ? "'rotated' expects 2 arguments (axis, angle)" : "'rotated' expects 1 argument (angle)";
        }
    }
    inline size_t vec_method_arity(math_fn fn, script_value_type kind) noexcept {
        switch (fn) {
            case math_fn::length_: case math_fn::length_squared_: case math_fn::normalize_: return 0;
            case math_fn::rotate_: return math::dims(kind) == 3 ? 2 : 1;
            default: return 1;
        }
    }
    JAI_MATH_COLD inline checked_result<script_value> call_vec_method(math_fn fn, const script_value& receiver, const script_value* args, size_t argc, engine* eng) {
        const script_value_type kind = receiver.unchecked_vec_kind();
        if (argc != vec_method_arity(fn, kind)) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), vec_method_arity_text(fn, kind));
        }
        const script_value* view[3] = {&receiver, nullptr, nullptr};
        for (size_t k = 0; k < argc; ++k) { view[k + 1] = &args[k].deref(); }
        return eval_geometry(fn, view, argc + 1, eng);
    }
    inline checked_result<script_value> call_vec_method(math_fn fn, const script_value& receiver, const std::vector<script_value>& args, engine* eng) {
        return call_vec_method(fn, receiver, args.data(), args.size(), eng);
    }
    // ---- the aggregate lowering's component steps (detail/aggregate_lowering.hpp) ------------------------------------------
    // vcomp$'s code: the math_fn of the component-wise family, and whether the component is the exact int row
    inline constexpr int k_vcomp_exact = 0x100;
    inline const char* lowering_step_text() noexcept { return "a lowered math component received a value of another type (internal)"; }
    inline bool is_i32_narrowing(math_fn fn) noexcept { return fn >= math_fn::i32_add_ && fn <= math_fn::i32_neg_; }
    // the overflow text of an ivec operator component (math_values.hpp int_component, int_component_negate)
    inline const char* i32_narrowing_text(math_fn fn) noexcept {
        switch (fn) {
            case math_fn::i32_add_: return math::overflow_text(math::bop::add);
            case math_fn::i32_sub_: return math::overflow_text(math::bop::sub);
            case math_fn::i32_mul_: return math::overflow_text(math::bop::mul);
            case math_fn::i32_div_: return math::overflow_text(math::bop::div);
            default: return math::negate_overflow_text;
        }
    }
    JAI_MATH_COLD inline checked_result<script_value> eval_lowering_step(math_fn fn, const script_value* const* args, size_t argc, engine* eng) {
        auto internal = [&] { return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), lowering_step_text()); };
        switch (fn) {
            case math_fn::acos_: {
                if (argc != 1 || args[0]->raw_storage_index() != script_value::TYPEID_FLOAT) { return internal(); }
                return script_value(geometry::clamped_acos(args[0]->unchecked_as_float()), eng);
            }
            case math_fn::f32_:
            case math_fn::i32_: {
                if (argc != 1) { return internal(); }
                const math::scalar s = math::read_scalar(*args[0]);
                if (!s.ok) { return internal(); }
                if (fn == math_fn::f32_) { return script_value(static_cast<script_float>(math::scalar_f32(s)), eng); }
                auto n = math::scalar_i32(s);
                if (!n) { return n.error_value(); }
                return script_value(static_cast<script_int>(n.value()), eng);
            }
            case math_fn::cstore_: {
                if (argc != 2 || args[1]->raw_storage_index() != script_value::TYPEID_INT) { return internal(); }
                uint32_t bits = 0;
                return math::narrow_component(static_cast<script_value_type>(args[1]->unchecked_as_int()), *args[0], bits, eng);
            }
            case math_fn::i32_add_: case math_fn::i32_sub_: case math_fn::i32_mul_: case math_fn::i32_div_: case math_fn::i32_neg_: {
                if (argc != 1 || args[0]->raw_storage_index() != script_value::TYPEID_INT) { return internal(); }
                auto r = math::narrow_int_component(args[0]->unchecked_as_int(), i32_narrowing_text(fn));
                if (!r) { return r.error_value(); }
                return script_value(static_cast<script_int>(r.value()), eng);
            }
            default: {   // vcomp$
                constexpr size_t k_max_args = 5;
                if (argc < 2 || argc > k_max_args + 1 || args[0]->raw_storage_index() != script_value::TYPEID_INT) { return internal(); }
                const int code = static_cast<int>(args[0]->unchecked_as_int());
                const math_fn row = static_cast<math_fn>(code & 0xFF);
                if (code & k_vcomp_exact) {
                    script_int x[k_max_args] = {0, 0, 0, 0, 0};
                    for (size_t a = 1; a < argc; ++a) {
                        if (args[a]->raw_storage_index() != script_value::TYPEID_INT) { return internal(); }
                        x[a - 1] = args[a]->unchecked_as_int();
                    }
                    auto n = math::to_i32(vec_component_exact(row, x));
                    if (!n) { return n.error_value(); }
                    return script_value(static_cast<script_int>(n.value()), eng);
                }
                script_float x[k_max_args] = {0, 0, 0, 0, 0};
                for (size_t a = 1; a < argc; ++a) {
                    const size_t raw = args[a]->raw_storage_index();
                    if (raw == script_value::TYPEID_INT) { x[a - 1] = static_cast<script_float>(args[a]->unchecked_as_int()); }
                    else if (raw == script_value::TYPEID_FLOAT) { x[a - 1] = args[a]->unchecked_as_float(); }
                    else { return internal(); }
                }
                return script_value(static_cast<script_float>(math::to_f32(vec_component_value(row, x))), eng);
            }
        }
    }

    // A method named on a vector, as the function value a member read yields: the receiver is copied (a vector is a value)
    inline script_value vec_bound_method(math_fn fn, const script_value& receiver, engine* eng) {
        script_function bound = [fn, recv = receiver, eng](const std::vector<script_value>& args) -> checked_result<script_value> {
            return call_vec_method(fn, recv, args, eng);
        };
        return script_value::make_function(std::move(bound), eng);
    }

    // The ONE evaluation kernel, used verbatim by both backends. Args arrive as an
    // array of DEREF'D pointers (zero-copy from the caller's stack). Type discipline:
    // ONE variant read per arg, switch on it; cpp_bound decodes through
    // one up-front decode (a bound operand decodes once). Every case arity-checks BEFORE touching args.
    inline checked_result<script_value> eval_math_intrinsic(math_fn fn, const script_value* const* args, size_t argc,
                                                            engine* eng) {
        // Normalize each operand once: yields (is_number, as_double, is_int, as_int)
        struct num {
            bool ok = false;
            bool is_int = false;
            script_int i = 0;
            script_float f = 0.0;
            script_float d() const noexcept { return is_int ? static_cast<script_float>(i) : f; }
        };
        auto read = [](const script_value& v) noexcept -> num {
            num out;
            switch (v.raw_storage_index()) {
                case script_value::TYPEID_INT: out.ok = true; out.is_int = true; out.i = v.unchecked_as_int(); break;
                case script_value::TYPEID_FLOAT: out.ok = true; out.f = v.unchecked_as_float(); break;
                case script_value::TYPEID_CPP_BOUND: {
                    const script_value decoded = v.bound_decoded_temp();   // a bound operand decodes once
                    if (decoded.raw_storage_index() == script_value::TYPEID_INT) {
                        out.ok = true; out.is_int = true; out.i = decoded.unchecked_as_int();
                    } else if (decoded.raw_storage_index() == script_value::TYPEID_FLOAT) {
                        out.ok = true; out.f = decoded.unchecked_as_float();
                    }
                    break;
                }
                default: break;
            }
            return out;
        };
        auto bad_arity = [&](const char* text) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), text);
        };
        auto bad_type = [&](const char* text) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), text);
        };
        auto scalar_pair_value = [&](math_fn row, const num& a, const num& b) {
            scalar_num r;
            eval_scalar_pair(row, scalar_num{a.is_int, a.i, a.f}, scalar_num{b.is_int, b.i, b.f}, r);
            return r.is_int ? checked_result<script_value>(script_value(r.i, eng)) : checked_result<script_value>(script_value(r.f, eng));
        };

        switch (fn) {
            case math_fn::trunc_: {
                if (!math_arity_ok<math_fn::trunc_>(argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]);
                if (!a.ok) { return bad_type("math::trunc expects a numeric argument"); }
                return script_value(eval_scalar_unary(fn, a.d()).f, eng);
            }
            case math_fn::cbrt_: {
                if (!math_arity_ok<math_fn::cbrt_>(argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]);
                if (!a.ok) { return bad_type("math::cbrt expects a numeric argument"); }
                return script_value(eval_scalar_unary(fn, a.d()).f, eng);
            }
            case math_fn::exp_: {
                if (!math_arity_ok<math_fn::exp_>(argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]);
                if (!a.ok) { return bad_type("math::exp expects a numeric argument"); }
                return script_value(eval_scalar_unary(fn, a.d()).f, eng);
            }
            case math_fn::exp2_: {
                if (!math_arity_ok<math_fn::exp2_>(argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]);
                if (!a.ok) { return bad_type("math::exp2 expects a numeric argument"); }
                return script_value(eval_scalar_unary(fn, a.d()).f, eng);
            }
            case math_fn::log_: {
                if (!math_arity_ok<math_fn::log_>(argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]);
                if (!a.ok) { return bad_type("math::log expects a numeric argument"); }
                return script_value(eval_scalar_unary(fn, a.d()).f, eng);
            }
            case math_fn::log2_: {
                if (!math_arity_ok<math_fn::log2_>(argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]);
                if (!a.ok) { return bad_type("math::log2 expects a numeric argument"); }
                return script_value(eval_scalar_unary(fn, a.d()).f, eng);
            }
            case math_fn::log10_: {
                if (!math_arity_ok<math_fn::log10_>(argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]);
                if (!a.ok) { return bad_type("math::log10 expects a numeric argument"); }
                return script_value(eval_scalar_unary(fn, a.d()).f, eng);
            }
            case math_fn::asin_: {
                if (!math_arity_ok<math_fn::asin_>(argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]);
                if (!a.ok) { return bad_type("math::asin expects a numeric argument"); }
                return script_value(eval_scalar_unary(fn, a.d()).f, eng);
            }
            case math_fn::acos_public_: {
                if (!math_arity_ok<math_fn::acos_public_>(argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]);
                if (!a.ok) { return bad_type("math::acos expects a numeric argument"); }
                return script_value(eval_scalar_unary(fn, a.d()).f, eng);
            }
            case math_fn::atan_: {
                if (!math_arity_ok<math_fn::atan_>(argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]);
                if (!a.ok) { return bad_type("math::atan expects a numeric argument"); }
                return script_value(eval_scalar_unary(fn, a.d()).f, eng);
            }
            case math_fn::sinh_: {
                if (!math_arity_ok<math_fn::sinh_>(argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]);
                if (!a.ok) { return bad_type("math::sinh expects a numeric argument"); }
                return script_value(eval_scalar_unary(fn, a.d()).f, eng);
            }
            case math_fn::cosh_: {
                if (!math_arity_ok<math_fn::cosh_>(argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]);
                if (!a.ok) { return bad_type("math::cosh expects a numeric argument"); }
                return script_value(eval_scalar_unary(fn, a.d()).f, eng);
            }
            case math_fn::tanh_: {
                if (!math_arity_ok<math_fn::tanh_>(argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]);
                if (!a.ok) { return bad_type("math::tanh expects a numeric argument"); }
                return script_value(eval_scalar_unary(fn, a.d()).f, eng);
            }
            case math_fn::sign_: case math_fn::is_nan_: case math_fn::is_inf_: case math_fn::saturate_: {
                if (!math_arity_accepts(fn, argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]);
                if (!a.ok) { return bad_type("math scalar helper expects a numeric argument"); }
                const scalar_num r = eval_scalar_unary(fn, a.d());
                if (fn == math_fn::is_nan_ || fn == math_fn::is_inf_) { return script_value(r.i != 0, eng); }
                return r.is_int ? script_value(r.i, eng) : script_value(r.f, eng);
            }
            case math_fn::fmod_: case math_fn::hypot_: {
                if (!math_arity_accepts(fn, argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]), b = read(*args[1]);
                if (!a.ok || !b.ok) { return bad_type("math scalar helper expects numeric arguments"); }
                return scalar_pair_value(fn, a, b);
            }
            case math_fn::wrap_: {
                if (!math_arity_ok<math_fn::wrap_>(argc)) { return bad_arity(math_arity_of(fn).text); }
                const num a = read(*args[0]), b = read(*args[1]), v = read(*args[2]);
                if (!a.ok || !b.ok || !v.ok) { return bad_type("math::wrap expects numeric arguments"); }
                const auto lower = std::min(a.d(), b.d()), upper = std::max(a.d(), b.d());
                const auto range = upper - lower, value = v.d() - lower;
                return script_value(range == 0.0 ? upper : value - range * std::floor(value / range) + lower, eng);
            }
            case math_fn::floor_: {
                if (!math_arity_ok<math_fn::floor_>(argc)) return bad_arity(math_arity_of(math_fn::floor_).text);
                const num a = read(*args[0]);
                if (!a.ok) return eval_vec_math(fn, args, argc, eng, "math::floor expects a numeric argument");
                return checked_result<script_value>(script_value(std::floor(a.d()), eng));
            }
            case math_fn::ceil_: {
                if (!math_arity_ok<math_fn::ceil_>(argc)) return bad_arity(math_arity_of(math_fn::ceil_).text);
                const num a = read(*args[0]);
                if (!a.ok) return eval_vec_math(fn, args, argc, eng, "math::ceil expects a numeric argument");
                return checked_result<script_value>(script_value(std::ceil(a.d()), eng));
            }
            case math_fn::itrunc: {
                if (!math_arity_ok<math_fn::itrunc>(argc)) return bad_arity(math_arity_of(math_fn::itrunc).text);
                const num a = read(*args[0]);
                if (!a.ok) return bad_type("math::itrunc expects a numeric argument");
                if (a.is_int) { return script_value(a.i, eng); }
                auto result = checked_float_to_integer(a.f);
                if (!result) { return result.error_value(); }
                return script_value(result.value(), eng);
            }
            case math_fn::ifloor: {
                if (!math_arity_ok<math_fn::ifloor>(argc)) return bad_arity(math_arity_of(math_fn::ifloor).text);
                const num a = read(*args[0]);
                if (!a.ok) return bad_type("math::ifloor expects a numeric argument");
                if (a.is_int) { return script_value(a.i, eng); }
                auto result = checked_float_to_integer(std::floor(a.f));
                if (!result) { return result.error_value(); }
                return script_value(result.value(), eng);
            }
            case math_fn::sqrt_: {
                if (!math_arity_ok<math_fn::sqrt_>(argc)) return bad_arity(math_arity_of(math_fn::sqrt_).text);
                const num a = read(*args[0]);
                if (!a.ok) return bad_type("math::sqrt expects a numeric argument");
                return checked_result<script_value>(script_value(std::sqrt(a.d()), eng));
            }
            case math_fn::pow_: {
                if (!math_arity_ok<math_fn::pow_>(argc)) return bad_arity(math_arity_of(math_fn::pow_).text);
                const num a = read(*args[0]), b = read(*args[1]);
                if (!a.ok || !b.ok) return bad_type("math::pow expects numeric arguments");
                return scalar_pair_value(fn, a, b);
            }
            case math_fn::abs_: {
                if (!math_arity_ok<math_fn::abs_>(argc)) return bad_arity(math_arity_of(math_fn::abs_).text);
                const num a = read(*args[0]);
                if (!a.ok) return eval_vec_math(fn, args, argc, eng, "math::abs expects a numeric argument");
                if (a.is_int) {
                    // |INT64_MIN| overflows; surface the overflow policy's catchable error
                    if (a.i == INT64_MIN) {
                        return checked_result<script_value>(make_error_code(runtime_error_code::invalid_numeric_operand),
                            "Integer overflow in math::abs");
                    }
                    return checked_result<script_value>(script_value(a.i < 0 ? -a.i : a.i, eng));
                }
                return checked_result<script_value>(script_value(std::fabs(a.f), eng));
            }
            case math_fn::min_:
            case math_fn::max_: {
                if (!math_arity_ok<math_fn::min_>(argc)) return bad_arity(math_arity_of(fn).text);
                const num a = read(*args[0]), b = read(*args[1]);
                if (!a.ok || !b.ok) return eval_vec_math(fn, args, argc, eng, fn == math_fn::min_ ? "math::min expects numeric arguments" : "math::max expects numeric arguments");
                return scalar_pair_value(fn, a, b);
            }
            case math_fn::clamp_: {
                if (!math_arity_ok<math_fn::clamp_>(argc)) return bad_arity(math_arity_of(math_fn::clamp_).text);
                const num v = read(*args[0]), lo = read(*args[1]), hi = read(*args[2]);
                if (!v.ok || !lo.ok || !hi.ok) return eval_vec_math(fn, args, argc, eng, "math::clamp expects numeric arguments");
                const bool below = v.is_int && lo.is_int ? v.i < lo.i : v.d() < lo.d();
                const num& p = below ? lo : ((v.is_int && hi.is_int ? v.i > hi.i : v.d() > hi.d()) ? hi : v);
                if (p.is_int) return checked_result<script_value>(script_value(p.i, eng));
                return checked_result<script_value>(script_value(p.f, eng));
            }
            case math_fn::sin_: {
                if (!math_arity_ok<math_fn::sin_>(argc)) return bad_arity(math_arity_of(math_fn::sin_).text);
                const num a = read(*args[0]);
                if (!a.ok) return bad_type("math::sin expects a numeric argument");
                return checked_result<script_value>(script_value(std::sin(a.d()), eng));
            }
            case math_fn::cos_: {
                if (!math_arity_ok<math_fn::cos_>(argc)) return bad_arity(math_arity_of(math_fn::cos_).text);
                const num a = read(*args[0]);
                if (!a.ok) return bad_type("math::cos expects a numeric argument");
                return checked_result<script_value>(script_value(std::cos(a.d()), eng));
            }
            case math_fn::tan_: {
                if (!math_arity_ok<math_fn::tan_>(argc)) return bad_arity(math_arity_of(math_fn::tan_).text);
                const num a = read(*args[0]);
                if (!a.ok) return bad_type("math::tan expects a numeric argument");
                return checked_result<script_value>(script_value(std::tan(a.d()), eng));
            }
            case math_fn::atan2_: {
                if (!math_arity_ok<math_fn::atan2_>(argc)) return bad_arity(math_arity_of(math_fn::atan2_).text);
                const num y = read(*args[0]), x = read(*args[1]);
                if (!y.ok || !x.ok) return bad_type("math::atan2 expects numeric arguments");
                return scalar_pair_value(fn, y, x);
            }
            case math_fn::distance:
            case math_fn::distance_squared: {
                if (argc == 2) { return eval_geometry(fn, args, argc, eng); }
                if (argc != 4) { return bad_arity(math_arity_of(fn).text); }
                const num x1 = read(*args[0]), y1 = read(*args[1]), x2 = read(*args[2]), y2 = read(*args[3]);
                if (!x1.ok || !y1.ok || !x2.ok || !y2.ok) {
                    return bad_type(fn == math_fn::distance ? "math::distance expects numeric arguments"
                                                            : "math::distance_squared expects numeric arguments");
                }
                const script_float dx = x2.d() - x1.d();
                const script_float dy = y2.d() - y1.d();
                const script_float d2 = dx * dx + dy * dy;
                return checked_result<script_value>(script_value(fn == math_fn::distance ? std::sqrt(d2) : d2, eng));
            }
            case math_fn::radians_to_degrees: {
                if (!math_arity_ok<math_fn::radians_to_degrees>(argc)) return bad_arity(math_arity_of(math_fn::radians_to_degrees).text);
                const num a = read(*args[0]);
                if (!a.ok) return bad_type("math::radians_to_degrees expects a numeric argument");
                return checked_result<script_value>(script_value(a.d() * (180.0 / 3.14159265358979323846), eng));
            }
            case math_fn::degrees_to_radians: {
                if (!math_arity_ok<math_fn::degrees_to_radians>(argc)) return bad_arity(math_arity_of(math_fn::degrees_to_radians).text);
                const num a = read(*args[0]);
                if (!a.ok) return bad_type("math::degrees_to_radians expects a numeric argument");
                return checked_result<script_value>(script_value(a.d() * (3.14159265358979323846 / 180.0), eng));
            }
            case math_fn::wrap_add_:
            case math_fn::wrap_sub_:
            case math_fn::wrap_mul_: {
                // Two's-complement wraparound computed in unsigned (UB-free), integer-only:
                // silently truncating a float would defeat the point of explicit wrap math.
                if (!math_arity_ok<math_fn::wrap_add_>(argc)) return bad_arity(math_arity_of(fn).text);
                const num a = read(*args[0]), b = read(*args[1]);
                if (!a.ok || !b.ok || !a.is_int || !b.is_int) {
                    return bad_type(fn == math_fn::wrap_add_ ? "math::wrap_add expects integer arguments"
                                  : fn == math_fn::wrap_sub_ ? "math::wrap_sub expects integer arguments"
                                                             : "math::wrap_mul expects integer arguments");
                }
                return scalar_pair_value(fn, a, b);
            }

            // ---- interpolation family (formulas verbatim from the stdlib registrations) ----
            case math_fn::lerp_:
            case math_fn::mix_: {
                if (!math_arity_ok<math_fn::lerp_>(argc)) return bad_arity(math_arity_of(fn).text);
                const num s = read(*args[0]), e = read(*args[1]), t = read(*args[2]);
                if (!s.ok || !e.ok || !t.ok) return eval_vec_math(fn, args, argc, eng, fn == math_fn::lerp_ ? "math::lerp expects numeric arguments"
                                                                                                             : "math::mix expects numeric arguments");
                return checked_result<script_value>(script_value((t.d() * (e.d() - s.d())) + s.d(), eng));
            }
            case math_fn::unmix_: {
                if (!math_arity_ok<math_fn::unmix_>(argc)) return bad_arity(math_arity_of(math_fn::unmix_).text);
                const num s = read(*args[0]), e = read(*args[1]), v = read(*args[2]);
                if (!s.ok || !e.ok || !v.ok) return eval_vec_math(fn, args, argc, eng, "math::unmix expects numeric arguments");
                return checked_result<script_value>(script_value(
                    (e.d() == s.d()) ? 0.0 : (v.d() - s.d()) / (e.d() - s.d()), eng));
            }
            case math_fn::mix_in_:
            case math_fn::mix_out_:
            case math_fn::mix_in_out_:
            case math_fn::mix_out_in_:
            case math_fn::unmix_in_:
            case math_fn::unmix_out_:
            case math_fn::unmix_in_out_:
            case math_fn::unmix_out_in_: {
                if (!math_arity_ok<math_fn::mix_in_>(argc)) return bad_arity(math_arity_of(math_fn::mix_in_).text);
                const num s_ = read(*args[0]), e_ = read(*args[1]), p_ = read(*args[2]), k_ = read(*args[3]);
                if (!s_.ok || !e_.ok || !p_.ok || !k_.ok) return eval_vec_math(fn, args, argc, eng, "math:: eased mix/unmix expects numeric arguments");
                const script_float r = eased_value(fn, s_.d(), e_.d(), p_.d(), k_.d());
                return checked_result<script_value>(script_value(r, eng));
            }

            case math_fn::vec2_:
            case math_fn::vec3_:
            case math_fn::vec4_:
            case math_fn::ivec2_:
            case math_fn::ivec3_:
            case math_fn::color_:
                return eval_vec_constructor(fn, args, argc, eng);
            case math_fn::mix_rgb_:
            case math_fn::mix_hsv_:
                return eval_color_mix(fn, args, argc, eng);
            case math_fn::dot_:
            case math_fn::cross_:
            case math_fn::length_:
            case math_fn::length_squared_:
            case math_fn::normalize_:
            case math_fn::reflect_:
            case math_fn::project_:
            case math_fn::angle_between_:
            case math_fn::perpendicular_:
            case math_fn::rotate_:
                return eval_geometry(fn, args, argc, eng);
            case math_fn::f32_:
            case math_fn::i32_:
            case math_fn::cstore_:
            case math_fn::vcomp_:
            case math_fn::acos_:
            case math_fn::i32_add_: case math_fn::i32_sub_: case math_fn::i32_mul_: case math_fn::i32_div_: case math_fn::i32_neg_:
                return eval_lowering_step(fn, args, argc, eng);

            case math_fn::round_: {
                if (!math_arity_ok<math_fn::round_>(argc)) return bad_arity(math_arity_of(math_fn::round_).text);
                const num a = read(*args[0]);
                if (!a.ok) return eval_vec_math(fn, args, argc, eng, "math::round expects a numeric argument");
                return checked_result<script_value>(script_value(std::round(a.d()), eng));
            }
            case math_fn::remap_: {
                if (!math_arity_ok<math_fn::remap_>(argc)) return bad_arity(math_arity_of(math_fn::remap_).text);
                const num v = read(*args[0]), il = read(*args[1]), ih = read(*args[2]), ol = read(*args[3]), oh = read(*args[4]);
                if (!v.ok || !il.ok || !ih.ok || !ol.ok || !oh.ok) return eval_vec_math(fn, args, argc, eng, "math::remap expects numeric arguments");
                return checked_result<script_value>(script_value(remap_value(v.d(), il.d(), ih.d(), ol.d(), oh.d()), eng));
            }

            default:
                return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation),
                    "Unknown math intrinsic");
        }
    }

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_MATH_INTRINSICS_HPP__
