// The portable math kernel (include/jaiscript/detail/portable_math.hpp): every function built from IEEE basic operations
// (+ - * / sqrt, exact on every conforming target) in a fixed order, with its data in portable_math_tables.hpp. No fused
// multiply-add, no reassociation: the pragmas keep both off under any flags a host builds with, fast-math included, save
// Clang's -ffp-contract=fast, which overrides pragmas (and which -ffast-math implies on targets without float_control,
// such as 32-bit ARM). Every special value is told apart by its bits, so an assumption of finite math changes nothing.
#include <jaiscript/detail/portable_math.hpp>
#include <jaiscript/jaiscript_fwd.hpp>
#include "portable_math_tables.hpp"

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>

#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0 && FLT_EVAL_METHOD != -1
#error "the portable math kernel needs double expressions evaluated in double (FLT_EVAL_METHOD 0)"
#endif

#if defined(__clang__)
// float_control is not available on every target (32-bit ARM ignores it); there the clang fp pragmas still hold
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-pragmas"
#pragma float_control(precise, on, push)
#pragma clang diagnostic pop
#pragma clang fp contract(off) reassociate(off)
#elif defined(_MSC_VER)
#pragma float_control(precise, on, push)
#pragma fp_contract(off)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("no-fast-math", "fp-contract=off")
#endif

namespace jai::detail::portable_math {

namespace {

using namespace portable_math_tables;

constexpr std::uint64_t k_sign_bit = std::uint64_t{1} << 63;
constexpr std::uint64_t k_mantissa_mask = (std::uint64_t{1} << 52) - 1;
constexpr double k_nan = std::bit_cast<double>(std::uint64_t{0x7FF8000000000000});
constexpr double k_infinity = std::bit_cast<double>(std::uint64_t{0x7FF0000000000000});
constexpr std::uint64_t k_infinity_bits = 0x7FF0000000000000;
// adding and subtracting 1.5 * 2^52 rounds a double under 2^51 to the nearest integer (ties to even)
constexpr double k_round_shift = 0x1.8p52;
constexpr double k_veltkamp = 134217729.0;   // 2^27 + 1

struct dd {
	double hi;
	double lo;
};

JAI_FORCEINLINE std::uint64_t bits_of(double x) noexcept { return std::bit_cast<std::uint64_t>(x); }
JAI_FORCEINLINE double from_bits(std::uint64_t u) noexcept { return std::bit_cast<double>(u); }
JAI_FORCEINLINE double magnitude(double x) noexcept { return from_bits(bits_of(x) & ~k_sign_bit); }
JAI_FORCEINLINE double with_sign_of(double value, double sign) noexcept { return from_bits((bits_of(value) & ~k_sign_bit) | (bits_of(sign) & k_sign_bit)); }
JAI_FORCEINLINE bool sign_set(double x) noexcept { return (bits_of(x) & k_sign_bit) != 0; }
// |x|'s bits: ordered as |x| is, an infinity at k_infinity_bits and every NaN above it
JAI_FORCEINLINE std::uint64_t magnitude_bits(double x) noexcept { return bits_of(x) & ~k_sign_bit; }
constexpr std::uint64_t bits_of_constant(double x) noexcept { return std::bit_cast<std::uint64_t>(x); }
// 2^e for a normal exponent, -1022 <= e <= 1023
JAI_FORCEINLINE double power_of_two(int e) noexcept { return from_bits(static_cast<std::uint64_t>(e + 1023) << 52); }

// a + b exactly as hi + lo, given |a| >= |b| or a == 0
JAI_FORCEINLINE dd fast_two_sum(double a, double b) noexcept {
	const double s = a + b;
	return {s, b - (s - a)};
}
JAI_FORCEINLINE dd two_sum(double a, double b) noexcept {
	const double s = a + b;
	const double bb = s - a;
	return {s, (a - (s - bb)) + (b - bb)};
}
// a * b exactly as hi + lo (Dekker, Veltkamp's split): no operand above 2^995, no partial product below 2^-969
JAI_FORCEINLINE dd two_product(double a, double b) noexcept {
	const double p = a * b;
	const double ta = k_veltkamp * a, tb = k_veltkamp * b;
	const double ah = ta - (ta - a), bh = tb - (tb - b);
	const double al = a - ah, bl = b - bh;
	return {p, ((ah * bh - p) + ah * bl + al * bh) + al * bl};
}
// x's top 26 significant bits: a product of two such is exact
JAI_FORCEINLINE double high_26(double x) noexcept { return from_bits(bits_of(x) & ~((std::uint64_t{1} << 27) - 1)); }
// a * b exactly as hi + lo through 26-bit splits (no operand above 2^995, no partial product below 2^-969)
JAI_FORCEINLINE dd exact_product(double a, double b) noexcept {
	const double ah = high_26(a), bh = high_26(b);
	const double al = a - ah, bl = b - bh;
	const double p = a * b;
	return {p, ((ah * bh - p) + ah * bl + al * bh) + al * bl};
}
JAI_FORCEINLINE dd exact_square(double a) noexcept {
	const double ah = high_26(a), al = a - ah;
	const double p = a * a;
	return {p, ((ah * ah - p) + 2.0 * ah * al) + al * al};
}
// (n.hi + n.lo) / (d.hi + d.lo)
JAI_FORCEINLINE dd divide(dd n, dd d) noexcept {
	const double q = n.hi / d.hi;
	const dd p = two_product(q, d.hi);
	const double rest = (((n.hi - p.hi) - p.lo) + n.lo) - q * d.lo;
	return fast_two_sum(q, rest / d.hi);
}
// sqrt(d.hi + d.lo), d.hi > 0
JAI_FORCEINLINE dd square_root(dd d) noexcept {
	const double h = std::sqrt(d.hi);
	const dd hh = two_product(h, h);
	return fast_two_sum(h, (((d.hi - hh.hi) - hh.lo) + d.lo) / (h + h));
}
// c - a for a constant c = c_hi + c_lo with |c_hi| >= |a.hi|
JAI_FORCEINLINE dd subtract_from(double c_hi, double c_lo, dd a) noexcept {
	const dd s = fast_two_sum(c_hi, -a.hi);
	return fast_two_sum(s.hi, s.lo + (c_lo - a.lo));
}

// ---- sin, cos, tan ---------------------------------------------------------------------------------------------------

struct reduced {
	int quadrant;
	double hi;
	double lo;
};

// the count bits of the little-endian x ending at bit top, as an integer (a bit below 0 reads 0)
std::uint64_t bits_ending_at(const std::uint32_t* x, int top, int count) noexcept {
	std::uint64_t result = 0;
	for (int b = top; b > top - count; --b) {
		result <<= 1;
		if (b >= 0) {
			result |= (x[b >> 5] >> (b & 31)) & 1u;
		}
	}
	return result;
}

// Payne-Hanek: x * 2/pi from 192 bits of 2/pi starting where x's integer part makes the earlier bits multiples of 4, in
// integer arithmetic; the fraction leaves at least 128 significant bits for the closest double to a multiple of pi/2
JAI_NOINLINE reduced reduce_large(double x) noexcept {
	const std::uint64_t u = bits_of(x);
	const int exponent = static_cast<int>((u >> 52) & 0x7FF) - 1075;   // |x| = mantissa * 2^exponent
	const std::uint64_t mantissa = (u & k_mantissa_mask) | (std::uint64_t{1} << 52);
	// 2/pi's fraction bit i (weight 2^-i) sits at bit i + 63 of TWO_OVER_PI_BITS, counted from the first word's top (two zero
	// words lead); the window starts at fraction bit exponent - 1
	const int start = exponent + 62;
	const int word = start >> 5, shift = start & 31;
	std::uint32_t window[6];   // little-endian
	for (int k = 0; k < 6; ++k) {
		const std::uint32_t high = TWO_OVER_PI_BITS[word + 5 - k], low = TWO_OVER_PI_BITS[word + 6 - k];
		window[k] = shift == 0 ? high : (high << shift) | (low >> (32 - shift));
	}
	std::uint32_t product[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	const std::uint64_t limbs[2] = {mantissa & 0xFFFFFFFFu, mantissa >> 32};
	for (int i = 0; i < 2; ++i) {
		std::uint64_t carry = 0;
		for (int k = 0; k < 6; ++k) {
			const std::uint64_t t = limbs[i] * window[k] + product[i + k] + carry;
			product[i + k] = static_cast<std::uint32_t>(t);
			carry = t >> 32;
		}
		product[i + 6] = static_cast<std::uint32_t>(carry);
	}
	// |x| * 2/pi = product / 2^190 (mod 4)
	int quadrant = static_cast<int>(product[5] >> 30);
	product[5] &= 0x3FFFFFFFu;
	bool flip = false;
	if ((product[5] >> 29) != 0) {   // a fraction of a half or more rounds the quadrant up: 2^190 - fraction
		++quadrant;
		flip = true;
		std::uint64_t borrow = 0;
		for (int k = 0; k < 6; ++k) {
			const std::uint64_t t = std::uint64_t{0} - product[k] - borrow;
			product[k] = static_cast<std::uint32_t>(t);
			borrow = (t >> 32) != 0 ? 1 : 0;
		}
		product[5] &= 0x3FFFFFFFu;
	}
	int top = 189;
	while (top >= 0 && ((product[top >> 5] >> (top & 31)) & 1u) == 0) {
		--top;
	}
	dd r{0.0, 0.0};
	if (top >= 0) {
		const double f_hi = static_cast<double>(bits_ending_at(product, top, 53)) * power_of_two(top - 52 - 190);
		const double f_lo = static_cast<double>(bits_ending_at(product, top - 53, 53)) * power_of_two(top - 105 - 190);
		const dd p = two_product(f_hi, PIO2_HI);
		r = fast_two_sum(p.hi, p.lo + (f_hi * PIO2_LO + f_lo * PIO2_HI));
	}
	if (flip != sign_set(x)) {
		r = {-r.hi, -r.lo};
	}
	return {sign_set(x) ? -quadrant : quadrant, r.hi, r.lo};
}

// x - k pi/2 with pi/2 in three parts (k < 2^20, so k times either 33-bit part is exact); Payne-Hanek when even that cancels
JAI_NOINLINE reduced reduce_medium_close(double x, double kd) noexcept {
	const double a = x - kd * PIO2_1;
	const dd s = two_sum(a, -(kd * PIO2_2));
	const dd t = two_sum(s.hi, -(kd * PIO2_2T));
	const dd r = fast_two_sum(t.hi, t.lo + s.lo);
	if (magnitude(r.hi) < magnitude(kd) * 0x1p-59) {
		return reduce_large(x);
	}
	return {static_cast<int>(static_cast<std::int64_t>(kd)), r.hi, r.lo};
}

// x finite: x = quadrant pi/2 + (hi + lo), |hi| <= pi/4 (and a rounding). Under 2^20, pi/2 as a 33-bit part and the
// rest (k = 0 included): the error is under k 2^-86, so 23 bits of cancellation still leave 62 good bits
JAI_FORCEINLINE reduced reduce(double x) noexcept {
	if (magnitude(x) < 0x1p20) {
		const double kd = (x * TWO_OVER_PI + k_round_shift) - k_round_shift;
		const dd r = fast_two_sum(x - kd * PIO2_1, -(kd * PIO2_1T));   // exact once the test passes: then |x - k p1| > 2^9 |k p1t|
		if (magnitude(r.hi) >= magnitude(kd) * 0x1p-24) {
			return {static_cast<int>(static_cast<std::int64_t>(kd)), r.hi, r.lo};
		}
		return reduce_medium_close(x, kd);
	}
	return reduce_large(x);
}

// sin(hi + lo + quadrant pi/2) as an unrounded pair, |hi| <= pi/4 (and a rounding): an odd quadrant is a cosine. Under
// 1/16, Taylor; above, the table's nearest point a = j/128, |d| <= 1/256 and
//   sin(a + d) = sin a + (cos a sin d + sin a (cos d - 1)),  cos(a + d) = cos a + (-sin a sin d + cos a (cos d - 1))
JAI_FORCEINLINE dd quadrant_sin_pair(double hi, double lo, int quadrant) noexcept {
	const int odd = quadrant & 1;
	const double negate = from_bits(bits_of(1.0) | (static_cast<std::uint64_t>(quadrant & 2) << 62));
	const double ahi = magnitude(hi);
	const double shifted = ahi * 128.0 + k_round_shift;
	const double jd = shifted - k_round_shift;
	if (jd < 8.0) {
		const double z = hi * hi;
		if (odd != 0) {
			const double* c = COS_TAYLOR;
			return {negate, negate * (z * (-0.5 + z * (c[0] + z * (c[1] + z * (c[2] + z * c[3])))) - hi * lo)};
		}
		const double* s = SIN_TAYLOR;
		return {negate * hi, negate * (lo + hi * z * (s[0] + z * (s[1] + z * (s[2] + z * (s[3] + z * s[4])))))};
	}
	const double* t = SINCOS_128[bits_of(shifted) & 0xFF];   // the shifted sum's low mantissa bits are the integer
	const double sign = from_bits(bits_of(1.0) | (bits_of(hi) & k_sign_bit));
	const double d = ahi - jd * 0x1p-7, dlo = sign * lo;
	const double z = d * d;
	const double sin_d = d + (dlo + d * z * (SIN_TAYLOR[0] + z * SIN_TAYLOR[1]));
	const double cos_d_less_1 = z * (-0.5 + z * (COS_TAYLOR[0] + z * COS_TAYLOR[1])) - d * dlo;
	// sine row: a = sin a, b = cos a, and the sign of r (sine is odd); cosine row: a = cos a, b = -sin a
	const int o = odd * 2;
	const double a = t[o], b = t[2 - o] * static_cast<double>(1 - o);
	// the sign folds into the table terms ahead of the polynomials (exact: it is +-1)
	const double m = from_bits(bits_of(negate) ^ (bits_of(hi) & k_sign_bit & (static_cast<std::uint64_t>(odd) - 1)));
	const double ma = m * a, mb = m * b;
	return {ma, m * t[o + 1] + (mb * sin_d + ma * cos_d_less_1)};
}

// |x| < 2^14 by one rounding to n = round(x 256/pi): n gives the quadrant, the table row j pi/256 (|j| <= 64) and
// r = x - n pi/256 (|r| <= pi/512, its error under 2^-72) together. False within 7.5 pi/256 of a multiple of pi/2, where
// the result is small: there the reduction by pi/2 and Taylor keep the relative accuracy.
struct pi_256_reduced {
	int quadrant;
	int j;
	dd r;
};
JAI_FORCEINLINE bool reduce_pi_256(double x, pi_256_reduced& out) noexcept {
	const double shifted = x * INV_PI_256 + k_round_shift;
	const double nd = shifted - k_round_shift;
	const std::int64_t n = static_cast<std::int64_t>(bits_of(shifted) - bits_of_constant(k_round_shift));
	const std::int64_t q = (n + 64) >> 7;
	const int j = static_cast<int>(n - q * 128);
	out = {static_cast<int>(q), j, fast_two_sum(x - nd * PI_256_1, -(nd * PI_256_1T))};
	return j <= -8 || j >= 8;
}
// sin(x + extra pi/2) as an unrounded pair from the table's identities:
//   sin(a + r) = sin a + (cos a sin r + sin a (cos r - 1)),  cos(a + r) = cos a + (-sin a sin r + cos a (cos r - 1))
// (r^7/7! and r r_lo in cos r are under 2^-63 of the result, so they are left out)
JAI_FORCEINLINE dd sin_pair_pi_256(const pi_256_reduced& p, int extra) noexcept {
	const double r = p.r.hi;
	const double z = r * r;
	const double sin_r_tail = p.r.lo + r * z * (SIN_TAYLOR[0] + z * SIN_TAYLOR[1]);
	const double cos_r_less_1 = z * (-0.5 + z * (COS_TAYLOR[0] + z * COS_TAYLOR[1]));
	// sine row: a = sin(j pi/256), b = cos; cosine row: a = cos, b = -sin(j pi/256); the quadrant's sign on both
	const int quadrant = p.quadrant + extra;
	const std::uint64_t odd = static_cast<std::uint64_t>(quadrant & 1);
	const std::uint64_t negate = static_cast<std::uint64_t>(quadrant & 2) << 62;
	const double* t = SINCOS_PI_256[p.j + 64];
	const int o = static_cast<int>(odd) * 2;
	const std::uint64_t b_sign = (odd << 63) ^ negate;
	const double a = from_bits(bits_of(t[o]) ^ negate), b = from_bits(bits_of(t[2 - o]) ^ b_sign);
	return {a, b * r + (from_bits(bits_of(t[o + 1]) ^ negate) + (b * sin_r_tail + a * cos_r_less_1))};
}
// sin(x + extra pi/2) for finite x outside the table path, as an unrounded pair
JAI_NOINLINE dd quadrant_sin_general(double x, int extra) noexcept {
	const reduced r = reduce(x);
	return quadrant_sin_pair(r.hi, r.lo, r.quadrant + extra);
}

// ---- exp ---------------------------------------------------------------------------------------------------------------

// e^(x + xlo) = 2^e (hi + lo) with hi + lo = 2^(j/128) (1 + p): k = round(x 128/ln2), r = x - k ln2/128, |r| <= ln2/256
struct scaled {
	int e;
	double hi;
	double lo;
};
JAI_FORCEINLINE scaled exp_reduce(double x, double xlo) noexcept {
	const double kd = (x * INV_LN2_128 + k_round_shift) - k_round_shift;
	const double r = ((x - kd * LN2_128_HI) - kd * LN2_128_LO) + xlo;
	const double z = r * r;
	const double* c = EXP_TAYLOR;
	const double p = r + z * ((c[0] + r * c[1]) + z * ((c[2] + r * c[3]) + z * c[4]));
	const std::int64_t k = static_cast<std::int64_t>(kd);
	const double* t = EXP2_128[k & 127];
	return {static_cast<int>(k >> 7), t[0], t[1] + t[0] * p};
}
// 2^e (hi + lo) rounded once, hi + lo in [1/2, 2): a subnormal result rounds on its own grid, not twice
JAI_NOINLINE double scale_subnormal(int e, double hi, double lo) noexcept {
	const double s = power_of_two(e + 1000);
	const double yh = hi * s, yl = lo * s;
	if (yh >= 0x1p-22) {
		return (yh + yl) * 0x1p-1000;
	}
	// adding 2^-22 rounds to multiples of 2^-74, the subnormal grid once scaled by 2^-1000
	const double bias = 0x1p-22;
	const double h = bias + yh;
	const double l = ((bias - h) + yh) + yl;
	return ((h + l) - bias) * 0x1p-1000;
}
JAI_FORCEINLINE double scale(int e, double hi, double lo) noexcept {
	if (e > 1023) {
		return (hi + lo) * power_of_two(e - 1) * 2.0;
	}
	if (e < -1021) {
		return scale_subnormal(e, hi, lo);
	}
	return (hi + lo) * power_of_two(e);
}
// e^a - 1 as a pair, 0 <= a <= 45: r carries its rounding too, since e^a - 1 can be as small as r
dd expm1_pair(double a) noexcept {
	const double kd = (a * INV_LN2_128 + k_round_shift) - k_round_shift;
	const dd r = two_sum(a - kd * LN2_128_HI, -(kd * LN2_128_LO));
	const double z = r.hi * r.hi;
	const double* c = EXP_TAYLOR;
	const double tail = z * (c[0] + r.hi * (c[1] + r.hi * (c[2] + r.hi * (c[3] + r.hi * c[4]))));
	if (kd == 0.0) {
		return fast_two_sum(r.hi, tail + r.lo);
	}
	const std::int64_t k = static_cast<std::int64_t>(kd);
	const double* t = EXP2_128[k & 127];
	const double s = power_of_two(static_cast<int>(k >> 7));
	const double th = t[0] * s;
	// 2^e (T_hi + T_hi r + T_hi tail + T_lo (1 + r + tail)) - 1, the first two terms exact
	const dd a_part = two_sum(th, -1.0);
	const dd b_part = two_product(th, r.hi);
	const double c_part = (t[1] + t[0] * (tail + r.lo) + t[1] * (r.hi + tail)) * s;
	const dd sum = two_sum(a_part.hi, b_part.hi);
	return fast_two_sum(sum.hi, ((sum.lo + a_part.lo) + b_part.lo) + c_part);
}

// ---- log ---------------------------------------------------------------------------------------------------------------

// x = 2^e m, m in [sqrt(1/2), sqrt(2)); c ~ 1/m from the table (16 bits), r = m c - 1 exact as hi + lo, |r| <= 2^-8;
// log x = e ln2 - log c + log1p(r)
struct log_parts {
	double e;
	dd r;
	const double* row;   // c, -log c hi, -log c lo
};
JAI_FORCEINLINE log_parts log_reduce(double x) noexcept {
	std::uint64_t u = bits_of(x);
	int e = -1023;
	if ((u >> 52) == 0) {   // subnormal
		u = bits_of(x * 0x1p52);
		e -= 52;
	}
	e += static_cast<int>(u >> 52);
	const std::uint64_t mantissa = u & k_mantissa_mask;
	const int halve = mantissa >= 0x6A09E667F3BCDu ? 1 : 0;   // m >= sqrt(2): halve it
	e += halve;
	const std::uint64_t m_bits = mantissa | (static_cast<std::uint64_t>(0x3FF - halve) << 52);
	const int index = static_cast<int>(mantissa >> 44) + 150 - 256 * halve;
	const double m = from_bits(m_bits);
	const double m_high = from_bits(m_bits & ~((std::uint64_t{1} << 27) - 1));   // 26 bits: m_high c and (m - m_high) c are exact
	const double* row = LOG_TABLE[index];
	const double c = row[0];
	// the sum is exact unless m_high c - 1 is below (m - m_high) c, which leaves an error under 2^-79
	return {static_cast<double>(e), fast_two_sum(m_high * c - 1.0, (m - m_high) * c), row};
}
// log1p(r) - r for |r| <= 2^-8, to 2^-59 relative
JAI_FORCEINLINE double log1p_tail(double r) noexcept {
	const double* c = LOG1P_TAYLOR;
	const double r2 = r * r;
	return r2 * (((c[0] + r * c[1]) + r2 * (c[2] + r * c[3])) + (r2 * r2) * (c[4] + r * c[5]));
}
// log(m) = -log c + log1p(r) as a pair
JAI_FORCEINLINE dd log_mantissa(const log_parts& p) noexcept {
	const dd s = fast_two_sum(p.row[1], p.r.hi);
	return fast_two_sum(s.hi, ((s.lo + p.row[2]) + p.r.lo) + log1p_tail(p.r.hi));
}
// (r.hi + r.lo) c for a constant c = c26 + c26t (c26 with 26 bits) as a pair: r.hi's top 26 bits times c26 is exact
JAI_FORCEINLINE dd scaled_by_constant(dd r, double c26, double c26t, double c) noexcept {
	const double rh = from_bits(bits_of(r.hi) & ~((std::uint64_t{1} << 27) - 1));
	return {rh * c26, (rh * c26t + (r.hi - rh) * c) + r.lo * c};
}
// log x as a pair to 2^-66 relative (pow's for |y log x| >= 8): the square term exact, log1p to r^8
JAI_NOINLINE dd log_precise(const log_parts& p) noexcept {
	const double r = p.r.hi;
	const dd t1 = fast_two_sum(p.e * LN2_HI, p.row[1]);
	const dd s = fast_two_sum(t1.hi, r);
	const dd sq = exact_square(r);
	const dd s2 = fast_two_sum(s.hi, -0.5 * sq.hi);
	const double* c = LOG1P_TAYLOR;
	// short chains (Estrin's grouping, a tree of sums): pow waits on this value
	const double r2 = sq.hi, r4 = r2 * r2;
	const double cube = r * r2 * (((c[1] + r * c[2]) + r2 * (c[3] + r * c[4])) + r4 * (c[5] + r * c[6]));
	const double lo = (((t1.lo + s.lo) + (s2.lo - 0.5 * sq.lo)) + ((p.e * LN2_LO + p.row[2]) + (p.r.lo - r * p.r.lo))) + cube;
	return fast_two_sum(s2.hi, lo);
}
// the shared special cases of log, log2 and log10; false with the result set, true for a positive finite x
JAI_FORCEINLINE bool log_argument(double x, double& result) noexcept {
	const std::uint64_t u = bits_of(x);
	if (u - 1 < k_infinity_bits - 1) {   // positive, finite and nonzero
		return true;
	}
	result = (u & ~k_sign_bit) == 0 ? -k_infinity : (u == k_infinity_bits ? k_infinity : k_nan);
	return false;
}

// e^(y log x) for x > 0 finite and 0 < |y| <= 2^64, with y log x exact as a pair. An error e in y log x is a relative
// error e in the result, so log to r^8 (2^-62) serves while |y log x| < 32; beyond, log_precise
JAI_FORCEINLINE double pow_positive(double x, double y) noexcept {
	const log_parts p = log_reduce(x);
	const dd t1 = fast_two_sum(p.e * LN2_HI, p.row[1]);
	const dd t2 = fast_two_sum(t1.hi, p.r.hi);
	const double r = p.r.hi, r2 = r * r;
	const double* c = LOG1P_TAYLOR;
	const double tail = r2 * (((c[0] + r * c[1]) + r2 * (c[2] + r * c[3])) + (r2 * r2) * ((c[4] + r * c[5]) + r2 * c[6]));
	dd l = fast_two_sum(t2.hi, ((t1.lo + t2.lo) + (p.e * LN2_LO + p.row[2])) + (p.r.lo + tail));
	if (!(magnitude(y * l.hi) < 32.0)) {
		l = log_precise(p);
	}
	const dd z = exact_product(y, l.hi);
	const double z_lo = z.lo + y * l.lo;
	if (z.hi > 709.8) {
		return k_infinity;
	}
	if (z.hi < -746.0) {
		return 0.0;
	}
	const scaled s = exp_reduce(z.hi, z_lo);
	return scale(s.e, s.hi, s.lo);
}

// ---- asin, acos -----------------------------------------------------------------------------------------------------------

// (asin s - s) / (s z) for z = s^2 <= 1/4: 1/6 + z P(z)/Q(z), a rational within 2^-53 of the exact term
JAI_FORCEINLINE double asin_series(double z) noexcept {
	const double* p = ASIN_P;
	const double* q = ASIN_Q;
	const double num = p[0] + z * (p[1] + z * (p[2] + z * (p[3] + z * p[4])));
	const double den = 1.0 + z * (q[0] + z * (q[1] + z * (q[2] + z * q[3])));
	return SIXTH + z * (num / den);
}
// asin(s) for s = sqrt(z), z = (1 - |x|) / 2, as s + rest: s's own rounding carried in rest by a 26-bit split
struct half_angle {
	double s;
	double rest;
};
JAI_FORCEINLINE half_angle asin_half_angle(double one_less_x) noexcept {
	const double z = one_less_x * 0.5;
	const double s = std::sqrt(z);
	const double sh = high_26(s), sl = s - sh;
	const double s_lo = ((z - sh * sh) - sl * (sh + s)) / (s + s);
	return {s, s_lo + s * (z * asin_series(z))};
}

// ---- atan --------------------------------------------------------------------------------------------------------------

// atan t for t <= 1/16 as a pair, t + tlo: Taylor
JAI_FORCEINLINE dd atan_small(double t, double tlo) noexcept {
	const double z = t * t, z2 = z * z;
	const double* a = ATAN_TAYLOR;
	const double p = t * z * (((a[0] + z * a[1]) + z2 * (a[2] + z * a[3])) + (z2 * z2) * ((a[4] + z * a[5]) + z2 * a[6]));
	return fast_two_sum(t, tlo + p);
}
// atan c + atan u as a pair, c = j/256 from the table, |u| <= 2^-9 (u's error under 2^-61)
JAI_FORCEINLINE dd atan_from_table(int j, double u) noexcept {
	const double uz = u * u;
	const double atan_u = u + u * uz * (ATAN_TAYLOR[0] + uz * (ATAN_TAYLOR[1] + uz * ATAN_TAYLOR[2]));
	const double* row = ATAN_256[j];
	return fast_two_sum(row[0], row[1] + atan_u);
}
// atan t for 0 <= t <= 1: under 1/16 Taylor; above, the table's nearest point c = j/256 and
// atan t = atan c + atan((t - c) / (1 + t c)), |(t - c) / (1 + t c)| <= 2^-9
JAI_FORCEINLINE dd atan_reduced(double t) noexcept {
	const double jd = (t * 256.0 + k_round_shift) - k_round_shift;
	if (jd < 16.0) {
		return atan_small(t, 0.0);
	}
	const double c = jd * 0x1p-8;
	return atan_from_table(static_cast<int>(jd), (t - c) / (1.0 + t * c));
}
// atan(n / d) for 0 < n <= d without forming the quotient exactly: with c = j/256 near n/d,
// atan(n/d) = atan c + atan((n - c d) / (d + c n)), and n - c d is exact through d's 26-bit split
dd atan_of_ratio(double n, double d) noexcept {
	const double inverse = 1.0 / d;
	const double q = n * inverse;
	const double jd = (q * 256.0 + k_round_shift) - k_round_shift;
	if (jd < 16.0) {   // atan t is about t here, so the quotient carries its rounding: n/d = q + (n - q d)/d
		const dd qd = exact_product(q, d);
		return atan_small(q, ((n - qd.hi) - qd.lo) * inverse);
	}
	const double c = jd * 0x1p-8;
	const double dh = high_26(d);
	return atan_from_table(static_cast<int>(jd), ((n - c * dh) - c * (d - dh)) / (d + c * n));
}

} // namespace

// ---- the functions -------------------------------------------------------------------------------------------------------

double sin(double x) noexcept {
	const std::uint64_t ux = magnitude_bits(x);
	pi_256_reduced p;
	if (ux < bits_of_constant(0x1p14) && reduce_pi_256(x, p)) {
		const dd s = sin_pair_pi_256(p, 0);
		return s.hi + s.lo;
	}
	if (ux < bits_of_constant(0x1p-26)) {   // sin x rounds to x (signed zeros and subnormals included)
		return x;
	}
	if (ux >= k_infinity_bits) {
		return k_nan;
	}
	const dd s = quadrant_sin_general(x, 0);
	return s.hi + s.lo;
}

double cos(double x) noexcept {
	const std::uint64_t ux = magnitude_bits(x);
	pi_256_reduced p;
	if (ux < bits_of_constant(0x1p14) && reduce_pi_256(x, p)) {
		const dd c = sin_pair_pi_256(p, 1);
		return c.hi + c.lo;
	}
	if (ux < bits_of_constant(0x1p-27)) {
		return 1.0;
	}
	if (ux >= k_infinity_bits) {
		return k_nan;
	}
	const dd c = quadrant_sin_general(x, 1);
	return c.hi + c.lo;
}

void sincos(double x, double& sin_out, double& cos_out) noexcept {
	const std::uint64_t ux = magnitude_bits(x);
	pi_256_reduced p;
	if (ux < bits_of_constant(0x1p14) && reduce_pi_256(x, p)) {
		const dd s = sin_pair_pi_256(p, 0), c = sin_pair_pi_256(p, 1);
		sin_out = s.hi + s.lo;
		cos_out = c.hi + c.lo;
		return;
	}
	sin_out = sin(x);
	cos_out = cos(x);
}

// tan through the pi/256 reduction: x - quadrant pi/2 = j pi/256 + r, and an odd quadrant is tan(. + pi/2), so x is
// m pi/256 + r modulo pi with |m| <= 120. tan(a + r) = tan a + tan r (1 + tan^2 a) / (1 - tan a tan r): the second term
// is under a sixteenth of the result, so its plain rounding stays small.
JAI_FORCEINLINE double tan_pi_256(const pi_256_reduced& p) noexcept {
	int m = p.j + ((p.quadrant & 1) << 7);
	m = m > 128 ? m - 256 : m;
	const double* t = TAN_PI_256[m < 0 ? -m : m];
	const std::uint64_t sign = static_cast<std::uint64_t>(m < 0) << 63;
	const double ta = from_bits(bits_of(t[0]) ^ sign), ta_lo = from_bits(bits_of(t[1]) ^ sign);
	const double r = p.r.hi, z = r * r;
	const double* c = TAN_TAYLOR;
	const double tan_r = r + (p.r.lo + r * z * (c[0] + z * (c[1] + z * c[2])));
	return ta + (ta_lo + tan_r * t[2] / (1.0 - ta * tan_r));
}
JAI_NOINLINE double tan_general(double x) noexcept {
	const reduced r = reduce(x);
	if ((r.quadrant & 1) == 0 && magnitude(r.hi) < 0.1) {   // Taylor: the result is small, the reduction kept it exact
		const double z = r.hi * r.hi;
		const double* c = TAN_TAYLOR;
		const double p = r.hi * z * (c[0] + z * (c[1] + z * (c[2] + z * (c[3] + z * (c[4] + z * (c[5] + z * c[6]))))));
		return r.hi + (r.lo + p);
	}
	const dd sp = quadrant_sin_pair(r.hi, r.lo, r.quadrant), cp = quadrant_sin_pair(r.hi, r.lo, r.quadrant + 1);
	const dd q = divide(fast_two_sum(sp.hi, sp.lo), fast_two_sum(cp.hi, cp.lo));
	return q.hi + q.lo;
}

double tan(double x) noexcept {
	const std::uint64_t ux = magnitude_bits(x);
	pi_256_reduced p;
	if (ux < bits_of_constant(0x1p14) && reduce_pi_256(x, p)) {
		return tan_pi_256(p);
	}
	if (ux < bits_of_constant(0x1p-27)) {   // tan x = x + x^3/3 rounds to x
		return x;
	}
	if (ux >= k_infinity_bits) {
		return k_nan;
	}
	return tan_general(x);
}

double asin(double x) noexcept {
	const std::uint64_t ux = magnitude_bits(x);
	if (ux <= bits_of_constant(0.5)) {
		if (ux < bits_of_constant(0x1p-26)) {   // asin x = x + x^3/6 rounds to x
			return x;
		}
		const double z = x * x;
		return x + x * (z * asin_series(z));
	}
	if (ux >= bits_of_constant(1.0)) {
		return ux == bits_of_constant(1.0) ? with_sign_of(PIO2_HI, x) : k_nan;
	}
	// asin |x| = pi/2 - 2 asin(s), s = sqrt((1 - |x|) / 2) <= 1/2
	const half_angle h = asin_half_angle(1.0 - magnitude(x));
	const dd d = fast_two_sum(PIO2_HI, -2.0 * h.s);
	return with_sign_of(d.hi + (d.lo + (PIO2_LO - 2.0 * h.rest)), x);
}

double acos(double x) noexcept {
	const std::uint64_t ux = magnitude_bits(x);
	if (ux <= bits_of_constant(0.5)) {   // pi/2 - asin x
		if (ux < bits_of_constant(0x1p-55)) {   // pi/2 - x rounds to pi/2
			return PIO2_HI;
		}
		const double z = x * x;
		const dd d = fast_two_sum(PIO2_HI, -x);
		return d.hi + (d.lo + (PIO2_LO - x * (z * asin_series(z))));
	}
	if (ux >= bits_of_constant(1.0)) {
		if (ux != bits_of_constant(1.0)) {
			return k_nan;
		}
		return sign_set(x) ? PI_HI : 0.0;
	}
	// 2 asin(s) with s = sqrt((1 - |x|) / 2), from pi for a negative x
	const half_angle h = asin_half_angle(1.0 - magnitude(x));
	if (!sign_set(x)) {
		return 2.0 * (h.s + h.rest);
	}
	const dd d = fast_two_sum(PI_HI, -2.0 * h.s);
	return d.hi + (d.lo + (PI_LO - 2.0 * h.rest));
}

double atan(double x) noexcept {
	const double ax = magnitude(x);
	const std::uint64_t ux = magnitude_bits(x);
	if (ux < bits_of_constant(0x1p-27)) {   // atan x = x - x^3/3 rounds to x
		return x;
	}
	if (ux > bits_of_constant(0x1p66)) {   // pi/2 - 1/x rounds to pi/2
		return ux <= k_infinity_bits ? with_sign_of(PIO2_HI, x) : k_nan;
	}
	dd a;
	if (ax <= 1.0) {
		a = atan_reduced(ax);
	} else {
		a = subtract_from(PIO2_HI, PIO2_LO, atan_of_ratio(1.0, ax));
	}
	return with_sign_of(a.hi + a.lo, x);
}

double atan2(double y, double x) noexcept {
	const std::uint64_t ux = magnitude_bits(x), uy = magnitude_bits(y);
	if (ux > k_infinity_bits || uy > k_infinity_bits) {
		return k_nan;
	}
	double ax = magnitude(x), ay = magnitude(y);
	const bool x_negative = sign_set(x);
	if (uy == 0) {
		return x_negative ? with_sign_of(PI_HI, y) : y;
	}
	if (ux == 0) {
		return with_sign_of(PIO2_HI, y);
	}
	if (ux == k_infinity_bits) {
		if (uy == k_infinity_bits) {
			return with_sign_of(x_negative ? THREE_PIO4_HI : PIO4_HI, y);
		}
		return with_sign_of(x_negative ? PI_HI : 0.0, y);
	}
	if (uy == k_infinity_bits) {
		return with_sign_of(PIO2_HI, y);
	}
	// keep the quotient's exact remainder in range: the larger operand into [2^-900, 2^900]
	const double larger = ax > ay ? ax : ay;
	if (larger > 0x1p900) {
		ax *= 0x1p-200;
		ay *= 0x1p-200;
	} else if (larger < 0x1p-900) {
		ax *= 0x1p200;
		ay *= 0x1p200;
	}
	dd a;
	const double smaller = ax > ay ? ay : ax;
	if (smaller < (ax > ay ? ax : ay) * 0x1p-60) {   // atan q rounds to q
		a = {smaller / (ax > ay ? ax : ay), 0.0};
		if (ay > ax) {
			a = subtract_from(PIO2_HI, PIO2_LO, a);
		}
	} else {
		a = ay <= ax ? atan_of_ratio(ay, ax) : subtract_from(PIO2_HI, PIO2_LO, atan_of_ratio(ax, ay));
	}
	if (x_negative) {
		a = subtract_from(PI_HI, PI_LO, a);
	}
	return with_sign_of(a.hi + a.lo, y);
}

double exp(double x) noexcept {
	const std::uint64_t ux = magnitude_bits(x);
	if (ux >= k_infinity_bits) {
		return ux > k_infinity_bits ? k_nan : (sign_set(x) ? 0.0 : k_infinity);
	}
	if (x > 709.8) {   // above ln(DBL_MAX) by more than a rounding
		return k_infinity;
	}
	if (x < -746.0) {
		return 0.0;
	}
	const scaled s = exp_reduce(x, 0.0);
	return scale(s.e, s.hi, s.lo);
}

double exp2(double x) noexcept {
	const std::uint64_t ux = magnitude_bits(x);
	if (ux >= k_infinity_bits) {
		return ux > k_infinity_bits ? k_nan : (sign_set(x) ? 0.0 : k_infinity);
	}
	if (x >= 1024.0) {
		return k_infinity;
	}
	if (x < -1076.0) {
		return 0.0;
	}
	// k = round(x 128), r = (x - k/128) ln2: the subtraction is exact
	const double kd = (x * 128.0 + k_round_shift) - k_round_shift;
	const double r = (x - kd * 0x1p-7) * LN2;
	const double z = r * r;
	const double* c = EXP_TAYLOR;
	const double p = r + z * ((c[0] + r * c[1]) + z * ((c[2] + r * c[3]) + z * c[4]));
	const std::int64_t k = static_cast<std::int64_t>(kd);
	const double* t = EXP2_128[k & 127];
	return scale(static_cast<int>(k >> 7), t[0], t[1] + t[0] * p);
}

double log(double x) noexcept {
	double special = 0.0;
	if (!log_argument(x, special)) {
		return special;
	}
	const log_parts p = log_reduce(x);
	const dd t1 = fast_two_sum(p.e * LN2_HI, p.row[1]);
	const dd s = fast_two_sum(t1.hi, p.r.hi);
	return s.hi + (((t1.lo + s.lo) + (p.e * LN2_LO + p.row[2])) + (p.r.lo + log1p_tail(p.r.hi)));
}

double log2(double x) noexcept {
	double special = 0.0;
	if (!log_argument(x, special)) {
		return special;
	}
	const log_parts p = log_reduce(x);
	const dd t1 = fast_two_sum(p.e, p.row[3]);
	const dd q = scaled_by_constant(p.r, INV_LN2_26, INV_LN2_26T, INV_LN2_HI);
	const dd s = fast_two_sum(t1.hi, q.hi);
	return s.hi + (((t1.lo + s.lo) + (p.row[4] + q.lo)) + log1p_tail(p.r.hi) * INV_LN2_HI);
}

double log10(double x) noexcept {
	double special = 0.0;
	if (!log_argument(x, special)) {
		return special;
	}
	const log_parts p = log_reduce(x);
	const dd t1 = fast_two_sum(p.e * LOG10_2_HI, p.row[5]);
	const dd q = scaled_by_constant(p.r, INV_LN10_26, INV_LN10_26T, INV_LN10_HI);
	const dd s = fast_two_sum(t1.hi, q.hi);
	return s.hi + (((t1.lo + s.lo) + (p.e * LOG10_2_LO + p.row[6])) + (q.lo + log1p_tail(p.r.hi) * INV_LN10_HI));
}

// pow off the fast path: zeros, infinities, NaNs, negative and subnormal bases, and exponents beyond 2^64
JAI_NOINLINE double pow_special(double x, double y) noexcept {
	const std::uint64_t ux = magnitude_bits(x), uy_magnitude = magnitude_bits(y);
	if (uy_magnitude == 0 || bits_of(x) == bits_of_constant(1.0)) {   // pow(x, +-0) and pow(1, y) are 1, NaN or not
		return 1.0;
	}
	if (ux > k_infinity_bits || uy_magnitude > k_infinity_bits) {
		return k_nan;
	}
	// y's kind: 0 not an integer, 1 an odd integer, 2 an even integer
	const std::uint64_t uy = bits_of(y);
	const int y_exponent = static_cast<int>((uy >> 52) & 0x7FF) - 1023;
	int y_kind = 0;
	if (y_exponent >= 53) {
		y_kind = 2;
	} else if (y_exponent >= 0) {
		const std::uint64_t fraction_bits = k_mantissa_mask >> y_exponent;
		if ((uy & fraction_bits) == 0) {
			y_kind = y_exponent == 0 ? 1 : (((uy >> (52 - y_exponent)) & 1) != 0 ? 1 : 2);
		}
	}
	const double ax = magnitude(x), ay = magnitude(y);
	const bool y_negative = sign_set(y);
	if (uy_magnitude == k_infinity_bits) {
		if (ux == bits_of_constant(1.0)) {   // pow(-1, +-inf)
			return 1.0;
		}
		return (ux < bits_of_constant(1.0)) == y_negative ? k_infinity : 0.0;
	}
	if (ux == 0) {
		const double result = y_negative ? k_infinity : 0.0;
		return y_kind == 1 ? with_sign_of(result, x) : result;
	}
	if (ux == k_infinity_bits) {
		const double result = y_negative ? 0.0 : k_infinity;
		return y_kind == 1 ? with_sign_of(result, x) : result;
	}
	double sign = 1.0;
	if (sign_set(x)) {
		if (y_kind == 0) {
			return k_nan;
		}
		sign = y_kind == 1 ? -1.0 : 1.0;
	}
	if (ay > 0x1p64) {   // |log x| >= 2^-54 for x != 1, so |y log x| > 746
		return (ax > 1.0) != y_negative ? k_infinity : 0.0;
	}
	return sign * pow_positive(ax, y);
}

double pow(double x, double y) noexcept {
	// a positive normal finite x and a nonzero y within 2^64 take the general formula directly
	if (bits_of(x) - std::uint64_t{0x0010000000000000} < k_infinity_bits - std::uint64_t{0x0010000000000000} &&
	    magnitude_bits(y) - 1 < bits_of_constant(0x1p64) - 1) {
		return pow_positive(x, y);
	}
	return pow_special(x, y);
}

double sinh(double x) noexcept {
	const double ax = magnitude(x);
	const std::uint64_t ux = magnitude_bits(x);
	if (ux < bits_of_constant(0x1p-26)) {   // sinh x = x + x^3/6 rounds to x
		return x;
	}
	if (ux > bits_of_constant(710.5)) {
		return ux <= k_infinity_bits ? with_sign_of(k_infinity, x) : k_nan;
	}
	if (ax >= 22.0) {   // e^-x is below 2^-63 of e^x
		const scaled s = exp_reduce(ax, 0.0);
		return with_sign_of(scale(s.e - 1, s.hi, s.lo), x);
	}
	// (E + E / (E + 1)) / 2 with E = e^|x| - 1: no cancellation near 0
	const dd e = expm1_pair(ax);
	const dd e_plus_1 = two_sum(e.hi, 1.0);
	const dd q = divide(e, fast_two_sum(e_plus_1.hi, e_plus_1.lo + e.lo));
	const dd s = two_sum(e.hi, q.hi);
	return with_sign_of(0.5 * (s.hi + ((s.lo + e.lo) + q.lo)), x);
}

double cosh(double x) noexcept {
	const double ax = magnitude(x);
	const std::uint64_t ux = magnitude_bits(x);
	if (ux < bits_of_constant(0x1p-27)) {   // 1 + x^2/2 rounds to 1
		return 1.0;
	}
	if (ux > bits_of_constant(710.5)) {
		return ux <= k_infinity_bits ? k_infinity : k_nan;
	}
	const scaled s = exp_reduce(ax, 0.0);
	if (ax >= 22.0) {
		return scale(s.e - 1, s.hi, s.lo);
	}
	const double f = power_of_two(s.e);
	const dd e = fast_two_sum(s.hi * f, s.lo * f);
	const dd inverse = divide({1.0, 0.0}, e);
	const dd sum = two_sum(e.hi, inverse.hi);
	return 0.5 * (sum.hi + ((sum.lo + e.lo) + inverse.lo));
}

double tanh(double x) noexcept {
	const double ax = magnitude(x);
	const std::uint64_t ux = magnitude_bits(x);
	if (ux < bits_of_constant(0x1p-27)) {   // tanh x = x - x^3/3 rounds to x
		return x;
	}
	if (ux > bits_of_constant(22.0)) {   // 1 - 2e^-2x rounds to 1
		return ux <= k_infinity_bits ? with_sign_of(1.0, x) : k_nan;
	}
	// E / (E + 2) with E = e^(2|x|) - 1
	const dd e = expm1_pair(ax + ax);
	const dd e_plus_2 = two_sum(e.hi, 2.0);
	const dd q = divide(e, fast_two_sum(e_plus_2.hi, e_plus_2.lo + e.lo));
	return with_sign_of(q.hi + q.lo, x);
}

double cbrt(double x) noexcept {
	std::uint64_t u = magnitude_bits(x);
	if (u >= k_infinity_bits || u == 0) {   // +-0 and +-inf are their own roots
		return u > k_infinity_bits ? k_nan : x;
	}
	int adjust = 0;
	if (u < 0x0010000000000000u) {   // subnormal: 2^54 x, then 2^-18 the root
		u = bits_of(magnitude(x) * 0x1p54);
		adjust = -18;
	}
	const int e = static_cast<int>(u >> 52) - 1023;   // |x| = 2^(3q + i) m, m in [1, 2)
	const int q = (e + 1200) / 3 - 400;
	const int i = e - 3 * q;
	const double m = from_bits((u & k_mantissa_mask) | (std::uint64_t{0x3FF} << 52));
	const double big_m = m * (i == 0 ? 1.0 : (i == 1 ? 2.0 : 4.0));
	const double* g = CBRT_GUESS;
	const double guess = (g[0] + m * (g[1] + m * (g[2] + m * (g[3] + m * (g[4] + m * g[5]))))) * CBRT_2_I[i];
	// the guess to 17 bits, so y^3 is exact and so is M - y^3; then the root of 1 + d by its series
	const double y = from_bits((bits_of(guess) + (std::uint64_t{1} << 35)) & ~((std::uint64_t{1} << 36) - 1));
	const double y3 = y * y * y;
	const double d = (big_m - y3) / y3;
	const double* c = CBRT_CORRECTION;
	const double root = y + y * (d * (c[0] + d * (c[1] + d * (c[2] + d * c[3]))));
	return with_sign_of(root * power_of_two(q + adjust), x);
}

// hypot outside [2^-450, 2^500]: infinities, NaNs, zeros, and squares that would overflow or underflow, scaled by 2^-+600
JAI_NOINLINE double hypot_general(double x, double y) noexcept {
	const std::uint64_t ux = magnitude_bits(x), uy = magnitude_bits(y);
	if (ux == k_infinity_bits || uy == k_infinity_bits) {
		return k_infinity;
	}
	if (ux > k_infinity_bits || uy > k_infinity_bits) {
		return k_nan;
	}
	double ax = magnitude(x), ay = magnitude(y);
	if (ax < ay) {
		const double t = ax;
		ax = ay;
		ay = t;
	}
	if (ay == 0.0 || static_cast<int>(bits_of(ax) >> 52) - static_cast<int>(bits_of(ay) >> 52) > 60) {   // ax (1 + (ay/ax)^2/2) rounds to ax
		return ax + ay;
	}
	double unscale = 1.0;
	if (ax > 0x1p500) {
		ax *= 0x1p-600;
		ay *= 0x1p-600;
		unscale = 0x1p600;
	} else if (ay < 0x1p-450) {
		ax *= 0x1p600;
		ay *= 0x1p600;
		unscale = 0x1p-600;
	}
	const dd xx = two_product(ax, ax), yy = two_product(ay, ay);
	const dd s = fast_two_sum(xx.hi, yy.hi);
	const dd root = square_root(fast_two_sum(s.hi, (s.lo + xx.lo) + yy.lo));
	return (root.hi + root.lo) * unscale;
}

double hypot(double x, double y) noexcept {
	const std::uint64_t ux = magnitude_bits(x), uy = magnitude_bits(y);
	constexpr std::uint64_t low = bits_of_constant(0x1p-450), span = bits_of_constant(0x1p500) - low;
	if (ux - low > span || uy - low > span) {
		return hypot_general(x, y);
	}
	// x^2 + y^2 exactly as a pair rounded once: the root of that rounding is off by at most 0.36 ulp, so with the root's
	// own rounding the result is within 0.86 ulp
	const dd xx = exact_square(x), yy = exact_square(y);
	const dd s = two_sum(xx.hi, yy.hi);
	return std::sqrt(s.hi + (s.lo + (xx.lo + yy.lo)));
}

} // namespace jai::detail::portable_math

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-pragmas"
#pragma float_control(pop)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma float_control(pop)
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif
