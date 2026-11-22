// The portable math kernel (detail/portable_math.hpp) against a reference computed here: double-double arithmetic (about
// 2^-100) for every function, and for trig arguments of any size a reduction by 2/pi from integer arithmetic (pi from
// Machin's formula, 2/pi by long division). Each function stays within 1 ulp over dense sweeps and its hard cases, the
// special values follow C's Annex F, and tools/ci/math_determinism.jai prints its committed hash on each backend.
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/detail/portable_math.hpp>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace jai::foundry::tests {

namespace {

namespace pm = jai::detail::portable_math;

// ---- double-double -------------------------------------------------------------------------------------------------------

struct dd {
	double hi = 0.0;
	double lo = 0.0;
};

dd quick_sum(double a, double b) {
	const double s = a + b;
	return {s, b - (s - a)};
}
dd exact_sum(double a, double b) {
	const double s = a + b;
	const double v = s - a;
	return {s, (a - (s - v)) + (b - v)};
}
dd exact_product(double a, double b) {
	const double p = a * b;
	double t = 134217729.0 * a;
	const double ah = t - (t - a), al = a - ah;
	t = 134217729.0 * b;
	const double bh = t - (t - b), bl = b - bh;
	return {p, ((ah * bh - p) + ah * bl + al * bh) + al * bl};
}
dd operator+(dd a, dd b) {
	dd s = exact_sum(a.hi, b.hi);
	const dd t = exact_sum(a.lo, b.lo);
	s.lo += t.hi;
	s = quick_sum(s.hi, s.lo);
	s.lo += t.lo;
	return quick_sum(s.hi, s.lo);
}
dd operator-(dd a) { return {-a.hi, -a.lo}; }
dd operator-(dd a, dd b) { return a + (-b); }
dd operator*(dd a, dd b) {
	dd p = exact_product(a.hi, b.hi);
	p.lo += a.hi * b.lo + a.lo * b.hi;
	return quick_sum(p.hi, p.lo);
}
dd operator/(dd a, dd b) {
	const double q1 = a.hi / b.hi;
	dd r = a - b * dd{q1};
	const double q2 = r.hi / b.hi;
	r = r - b * dd{q2};
	const double q3 = r.hi / b.hi;
	return quick_sum(q1, q2) + dd{q3};
}
dd scaled(dd a, int e) { return {std::ldexp(a.hi, e), std::ldexp(a.lo, e)}; }
dd root(dd a) {
	if (a.hi <= 0.0) {
		return {};
	}
	const double x = std::sqrt(a.hi);
	return dd{x} + (a - exact_product(x, x)) / dd{2.0 * x};
}

// a reference value: (m.hi + m.lo) 2^k
struct reference_value {
	dd m;
	int k = 0;
};

// ---- integers for the reduction ------------------------------------------------------------------------------------------

using big = std::vector<std::uint32_t>;   // little-endian

void big_add(big& a, const big& b) {
	std::uint64_t carry = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		const std::uint64_t t = std::uint64_t{a[i]} + (i < b.size() ? b[i] : 0u) + carry;
		a[i] = static_cast<std::uint32_t>(t);
		carry = t >> 32;
	}
}
void big_subtract(big& a, const big& b) {
	std::int64_t borrow = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		std::int64_t t = static_cast<std::int64_t>(a[i]) - (i < b.size() ? b[i] : 0u) - borrow;
		borrow = t < 0 ? 1 : 0;
		a[i] = static_cast<std::uint32_t>(t + (borrow << 32));
	}
}
void big_divide(big& a, std::uint32_t d) {
	std::uint64_t rest = 0;
	for (size_t i = a.size(); i-- > 0;) {
		const std::uint64_t t = (rest << 32) | a[i];
		a[i] = static_cast<std::uint32_t>(t / d);
		rest = t % d;
	}
}
void big_multiply(big& a, std::uint32_t m) {
	std::uint64_t carry = 0;
	for (auto& limb : a) {
		const std::uint64_t t = std::uint64_t{limb} * m + carry;
		limb = static_cast<std::uint32_t>(t);
		carry = t >> 32;
	}
}
bool big_zero(const big& a) {
	for (auto limb : a) {
		if (limb != 0) {
			return false;
		}
	}
	return true;
}
bool big_at_least(const big& a, const big& b) {
	for (size_t i = a.size(); i-- > 0;) {
		if (a[i] != b[i]) {
			return a[i] > b[i];
		}
	}
	return true;
}
bool big_bit(const big& a, long long b) { return b >= 0 && static_cast<size_t>(b >> 5) < a.size() && ((a[b >> 5] >> (b & 31)) & 1u) != 0; }

// floor(atan(1/n) 2^bits) by its alternating series
big atan_inverse_fixed(std::uint32_t n, int bits, size_t limbs) {
	big term(limbs, 0), positive(limbs, 0), negative(limbs, 0);
	term[bits / 32] = 1u << (bits % 32);
	big_divide(term, n);
	for (std::uint32_t k = 0;; ++k) {
		big t = term;
		big_divide(t, 2 * k + 1);
		if (big_zero(t)) {
			break;
		}
		big_add((k & 1) != 0 ? negative : positive, t);
		big_divide(term, n * n);
	}
	big_subtract(positive, negative);
	return positive;
}

// ---- the reference functions ---------------------------------------------------------------------------------------------

class reference {
public:
	reference() {
		// pi in fixed point with k_pi_bits fraction bits, then 2/pi with k_g_bits by long division of 2^(k_g_bits + 1 + k_pi_bits)
		const size_t limbs = (k_pi_bits + 64) / 32;
		big a = atan_inverse_fixed(5, k_pi_bits, limbs), b = atan_inverse_fixed(239, k_pi_bits, limbs);
		big_multiply(a, 16);
		big_multiply(b, 4);
		big_subtract(a, b);
		const big pi_fixed = a;
		pi_ = to_dd(pi_fixed, k_pi_bits);
		pio2_ = scaled(pi_, -1);
		const int numerator_bit = k_g_bits + 1 + k_pi_bits;
		big rest(limbs + 1, 0), divisor = pi_fixed;
		divisor.resize(limbs + 1, 0);
		two_over_pi_.assign(static_cast<size_t>(k_g_bits / 32 + 2), 0);
		for (int bit = numerator_bit; bit >= 0; --bit) {
			std::uint32_t carry = bit == numerator_bit ? 1u : 0u;
			for (auto& limb : rest) {
				const std::uint32_t next = limb >> 31;
				limb = (limb << 1) | carry;
				carry = next;
			}
			if (big_at_least(rest, divisor)) {
				big_subtract(rest, divisor);
				if (bit <= k_g_bits) {
					two_over_pi_[bit >> 5] |= 1u << (bit & 31);
				}
			}
		}
		// ln 2 = 2 atanh(1/3)
		dd third = dd{1.0} / dd{3.0}, term = third, sum{};
		for (int k = 0; k < 80; ++k) {
			sum = sum + term / dd{2.0 * k + 1};
			term = term * third * third;
		}
		ln2_ = dd{2.0} * sum;
		ln10_ = log(10.0);
	}

	dd pi() const { return pi_; }

	reference_value exp(dd a) const {
		const double kd = std::nearbyint(a.hi / ln2_.hi);
		dd r = scaled(a - ln2_ * dd{kd}, -8);
		dd term{1.0}, sum{1.0};
		for (int n = 1; n < 40; ++n) {
			term = term * r / dd{static_cast<double>(n)};
			sum = sum + term;
			if (std::fabs(term.hi) < 1e-40) {
				break;
			}
		}
		for (int i = 0; i < 8; ++i) {
			sum = sum * sum;
		}
		return {sum, static_cast<int>(kd)};
	}
	reference_value exp2(double x) const { return exp(dd{x} * ln2_); }
	dd log(double x) const {
		int e = 0;
		double m = std::frexp(x, &e);
		if (m < 0.70710678118654752) {
			m *= 2.0;
			--e;
		}
		const dd s = (dd{m} - dd{1.0}) / (dd{m} + dd{1.0});
		const dd s2 = s * s;
		dd term = s, sum{};
		for (int k = 0; k < 60; ++k) {
			sum = sum + term / dd{2.0 * k + 1};
			term = term * s2;
		}
		return dd{static_cast<double>(e)} * ln2_ + dd{2.0} * sum;
	}
	dd log2(double x) const { return log(x) / ln2_; }
	dd log10(double x) const { return log(x) / ln10_; }

	static dd sin_series(dd r) {
		const dd r2 = r * r;
		dd term = r, sum = r;
		for (int n = 1; n < 40; ++n) {
			term = -(term * r2) / dd{static_cast<double>((2 * n) * (2 * n + 1))};
			sum = sum + term;
		}
		return sum;
	}
	static dd cos_series(dd r) {
		const dd r2 = r * r;
		dd term{1.0}, sum{1.0};
		for (int n = 1; n < 40; ++n) {
			term = -(term * r2) / dd{static_cast<double>((2 * n - 1) * (2 * n))};
			sum = sum + term;
		}
		return sum;
	}
	// x = quadrant pi/2 + r, |r| <= pi/4, from the 2/pi bits
	void reduce(double x, int& quadrant, dd& r) const {
		const double ax = std::fabs(x);
		if (ax <= 0.78) {
			quadrant = 0;
			r = dd{x};
			return;
		}
		int e = 0;
		const double m = std::frexp(ax, &e);
		const std::uint64_t mantissa = static_cast<std::uint64_t>(std::ldexp(m, 53));   // ax = mantissa 2^(e - 53)
		big product(two_over_pi_.size() + 3, 0);
		for (int part = 0; part < 2; ++part) {
			const std::uint64_t limb = part == 0 ? (mantissa & 0xFFFFFFFFu) : (mantissa >> 32);
			std::uint64_t carry = 0;
			for (size_t i = 0; i < two_over_pi_.size(); ++i) {
				const std::uint64_t t = limb * two_over_pi_[i] + product[i + part] + carry;
				product[i + part] = static_cast<std::uint32_t>(t);
				carry = t >> 32;
			}
			for (size_t i = two_over_pi_.size() + part; carry != 0; ++i) {
				const std::uint64_t t = std::uint64_t{product[i]} + carry;
				product[i] = static_cast<std::uint32_t>(t);
				carry = t >> 32;
			}
		}
		// ax 2/pi = product 2^(e - 53 - k_g_bits): the binary point sits at bit point
		const long long point = static_cast<long long>(k_g_bits) + 53 - e;
		quadrant = (big_bit(product, point) ? 1 : 0) + (big_bit(product, point + 1) ? 2 : 0);
		dd f{};
		for (int i = 1; i <= 160; ++i) {
			if (big_bit(product, point - i)) {
				f = f + dd{std::ldexp(1.0, -i)};
			}
		}
		if (f.hi >= 0.5) {
			f = f - dd{1.0};
			++quadrant;
		}
		r = f * pio2_;
		if (x < 0) {
			r = -r;
			quadrant = -quadrant;
		}
	}
	dd sin(double x) const {
		int q = 0;
		dd r;
		reduce(x, q, r);
		switch (q & 3) {
			case 0: return sin_series(r);
			case 1: return cos_series(r);
			case 2: return -sin_series(r);
			default: return -cos_series(r);
		}
	}
	dd cos(double x) const {
		int q = 0;
		dd r;
		reduce(x, q, r);
		switch (q & 3) {
			case 0: return cos_series(r);
			case 1: return -sin_series(r);
			case 2: return -cos_series(r);
			default: return sin_series(r);
		}
	}
	dd tan(double x) const { return sin(x) / cos(x); }

	dd atan(dd x) const {
		const bool negative = x.hi < 0;
		if (negative) {
			x = -x;
		}
		const bool invert = x.hi > 1.0;
		if (invert) {
			x = dd{1.0} / x;
		}
		for (int i = 0; i < 3; ++i) {
			x = x / (dd{1.0} + root(dd{1.0} + x * x));
		}
		const dd x2 = x * x;
		dd term = x, sum{};
		for (int k = 0; k < 40; ++k) {
			const dd t = term / dd{2.0 * k + 1};
			sum = (k & 1) != 0 ? sum - t : sum + t;
			term = term * x2;
		}
		sum = dd{8.0} * sum;
		if (invert) {
			sum = pio2_ - sum;
		}
		return negative ? -sum : sum;
	}
	dd asin(double x) const { return atan(dd{x} / root(dd{1.0} - exact_product(x, x))); }
	dd acos(double x) const { return dd{2.0} * atan(root((dd{1.0} - dd{x}) / (dd{1.0} + dd{x}))); }
	dd atan2(double y, double x) const {
		int ey = 0, ex = 0;
		const double my = std::frexp(std::fabs(y), &ey), mx = std::frexp(std::fabs(x), &ex);
		dd a = atan(scaled(dd{my} / dd{mx}, ey - ex));
		if (x < 0) {
			a = pi_ - a;
		}
		return y < 0 ? -a : a;
	}

	reference_value sinh(double x) const {
		const double ax = std::fabs(x);
		const double sign = x < 0 ? -1.0 : 1.0;
		if (ax < 0.5) {
			const dd x2 = dd{x} * dd{x};
			dd term{x}, sum{x};
			for (int n = 1; n < 40; ++n) {
				term = term * x2 / dd{static_cast<double>((2 * n) * (2 * n + 1))};
				sum = sum + term;
			}
			return {sum, 0};
		}
		reference_value e = exp(dd{ax});
		if (ax > 40.0) {
			return {dd{sign} * e.m, e.k - 1};
		}
		const dd v = scaled(e.m, e.k);
		return {dd{0.5 * sign} * (v - dd{1.0} / v), 0};
	}
	reference_value cosh(double x) const {
		const double ax = std::fabs(x);
		reference_value e = exp(dd{ax});
		if (ax > 40.0) {
			return {e.m, e.k - 1};
		}
		const dd v = scaled(e.m, e.k);
		return {dd{0.5} * (v + dd{1.0} / v), 0};
	}
	dd tanh(double x) const {
		const double ax = std::fabs(x);
		dd t;
		if (ax < 0.5) {
			t = sinh(ax).m / cosh_small(ax);
		} else if (ax < 25.0) {
			const reference_value e = exp(dd{2.0 * ax});
			const dd v = scaled(e.m, e.k);
			t = (v - dd{1.0}) / (v + dd{1.0});
		} else {
			const reference_value e = exp(dd{-2.0 * ax});
			t = dd{1.0} - dd{2.0} * scaled(e.m, e.k);
		}
		return x < 0 ? -t : t;
	}
	static dd cosh_small(double x) {
		const dd x2 = dd{x} * dd{x};
		dd term{1.0}, sum{1.0};
		for (int n = 1; n < 40; ++n) {
			term = term * x2 / dd{static_cast<double>((2 * n - 1) * (2 * n))};
			sum = sum + term;
		}
		return sum;
	}

	reference_value pow(double x, double y) const {
		const reference_value v = exp(dd{y} * log(std::fabs(x)));
		const bool odd = x < 0 && std::fmod(std::fabs(y), 2.0) == 1.0;
		return {odd ? -v.m : v.m, v.k};
	}
	reference_value cbrt(double x) const {
		int e = 0;
		const double m = std::frexp(std::fabs(x), &e);
		const int q = (e + 3000) / 3 - 1000;
		const dd a = dd{std::ldexp(m, e - 3 * q)};
		dd y{std::cbrt(a.hi)};
		for (int i = 0; i < 3; ++i) {
			y = y - (y * y * y - a) / (dd{3.0} * y * y);
		}
		return {x < 0 ? -y : y, q};
	}
	reference_value hypot(double x, double y) const {
		int e = 0;
		std::frexp(std::fmax(std::fabs(x), std::fabs(y)), &e);
		const dd xs{std::ldexp(x, -e)}, ys{std::ldexp(y, -e)};
		return {root(xs * xs + ys * ys), e};
	}

private:
	static constexpr int k_pi_bits = 1600;
	static constexpr int k_g_bits = 1500;
	static dd to_dd(const big& fixed, int fraction_bits) {
		dd v{};
		int taken = 0;
		for (long long b = static_cast<long long>(fixed.size()) * 32 - 1; b >= 0 && taken < 130; --b) {
			if (big_bit(fixed, b) || taken > 0) {
				if (big_bit(fixed, b)) {
					v = v + dd{std::ldexp(1.0, static_cast<int>(b) - fraction_bits)};
				}
				++taken;
			}
		}
		return v;
	}
	big two_over_pi_;   // floor(2/pi 2^k_g_bits)
	dd pi_, pio2_, ln2_, ln10_;
};

// |result - reference| in ulps of the reference; a reference outside the double range counts as exact when the result
// is the infinity or zero it rounds to
double ulp_error(double result, const reference_value& ref) {
	if (ref.m.hi == 0.0) {
		return result == 0.0 ? 0.0 : 1e9;
	}
	int e = 0;
	std::frexp(ref.m.hi, &e);
	e += ref.k;   // |value| in [2^(e-1), 2^e)
	if (e > 1024) {
		return std::isinf(result) ? 0.0 : 1e9;
	}
	if (e == 1024 && std::isinf(result)) {   // past DBL_MAX by half an ulp rounds to infinity
		const dd magnitude = ref.m.hi < 0 ? -ref.m : ref.m;
		const dd excess = magnitude - dd{std::ldexp(std::numeric_limits<double>::max(), -ref.k)};
		return excess.hi >= std::ldexp(1.0, 970 - ref.k) ? 0.0 : 1e9;
	}
	if (!std::isfinite(result)) {
		return 1e9;
	}
	const int ulp_exponent = (e - 1 > -1022 ? e - 1 : -1022) - 52;
	const double rs = std::ldexp(result, -ref.k);
	const double diff = (rs - ref.m.hi) - ref.m.lo;
	return std::fabs(diff) / std::ldexp(1.0, ulp_exponent - ref.k);
}
double ulp_error(double result, dd ref) { return ulp_error(result, reference_value{ref, 0}); }

// xorshift64*: the sweeps' inputs
struct inputs {
	std::uint64_t state = 0x2545F4914F6CDD1Dull;
	std::uint64_t bits() {
		state ^= state >> 12;
		state ^= state << 25;
		state ^= state >> 27;
		return state * 0x2545F4914F6CDD1Dull;
	}
	double uniform(double lo, double hi) { return lo + (hi - lo) * (static_cast<double>(bits() >> 11) * 0x1p-53); }
	// a random sign times 2^e, e uniform in [lo, hi)
	double scattered(double lo, double hi) {
		const double v = std::exp2(uniform(lo, hi));
		return (bits() & 1) != 0 ? -v : v;
	}
};

struct worst_case {
	double ulps = 0.0;
	double x = 0.0;
	double y = 0.0;
};
std::string describe(const char* name, const worst_case& w) {
	std::ostringstream text;
	text.precision(17);
	text << name << " is " << w.ulps << " ulp off at (" << w.x << ", " << w.y << ")";
	return text.str();
}
void note(worst_case& w, double ulps, double x, double y = 0.0) {
	if (ulps > w.ulps) {
		w = {ulps, x, y};
	}
}

std::uint64_t bits_of(double x) { return std::bit_cast<std::uint64_t>(x); }
bool same_bits(double a, double b) { return bits_of(a) == bits_of(b); }

const double INF = std::numeric_limits<double>::infinity();
const double NAN_BITS = std::bit_cast<double>(std::uint64_t{0x7FF8000000000000});

}

class portable_math_tests : public suite {
public:
	portable_math_tests() : suite("Portable Math") {}

	void forge_tests() override {
		test("sin_cos_and_tan_are_within_one_ulp_for_every_argument_size", []() {
			const reference ref;
			inputs in;
			worst_case s, c, t;
			auto sample = [&](double x) {
				note(s, ulp_error(pm::sin(x), ref.sin(x)), x);
				note(c, ulp_error(pm::cos(x), ref.cos(x)), x);
				note(t, ulp_error(pm::tan(x), ref.tan(x)), x);
			};
			for (int i = 0; i < 3000; ++i) { sample(in.uniform(-8.0, 8.0)); }
			for (int i = 0; i < 1000; ++i) { sample(in.scattered(-30.0, 1024.0)); }
			// the doubles nearest multiples of pi/2, their neighbors, and the closest known approach over all doubles
			for (int k = 1; k < 400; ++k) {
				const double x = static_cast<double>(k) * 1.5707963267948966;
				sample(x);
				sample(std::nextafter(x, INF));
				sample(std::nextafter(x, -INF));
			}
			sample(std::ldexp(6381956970095103.0, 797));
			sample(1e22);
			sample(std::numeric_limits<double>::max());
			sample(1048575.75);
			sample(1048576.25);
			check_le(s.ulps, 1.0, describe("sin", s));
			check_le(c.ulps, 1.0, describe("cos", c));
			check_le(t.ulps, 1.0, describe("tan", t));
		});

		test("inverse_trig_is_within_one_ulp_near_its_edges", []() {
			const reference ref;
			inputs in;
			worst_case as, ac, at, a2;
			auto sample_unit = [&](double x) {
				note(as, ulp_error(pm::asin(x), ref.asin(x)), x);
				note(ac, ulp_error(pm::acos(x), ref.acos(x)), x);
			};
			for (int i = 0; i < 3000; ++i) { sample_unit(in.uniform(-1.0, 1.0)); }
			for (int i = 1; i < 300; ++i) {   // near +-1
				const double x = 1.0 - static_cast<double>(i) * 0x1p-53 * static_cast<double>(i * i);
				sample_unit(x);
				sample_unit(-x);
			}
			for (int i = 0; i < 300; ++i) { sample_unit(in.scattered(-60.0, 0.0)); }
			for (int i = -200; i <= 200; ++i) {   // where the series meets the half-angle form
				sample_unit(0.5 + static_cast<double>(i) * 0x1p-50);
				sample_unit(-0.5 + static_cast<double>(i) * 0x1p-50);
			}
			for (int i = 0; i < 3000; ++i) {
				const double x = in.scattered(-40.0, 70.0);
				note(at, ulp_error(pm::atan(x), ref.atan(dd{x})), x);
			}
			for (int i = 0; i < 3000; ++i) {
				const double y = in.scattered(-30.0, 30.0), x = in.scattered(-30.0, 30.0);
				note(a2, ulp_error(pm::atan2(y, x), ref.atan2(y, x)), y, x);
			}
			for (int i = 0; i < 300; ++i) {   // extreme quotients and magnitudes
				const double y = in.scattered(-1000.0, 1000.0), x = in.scattered(-1000.0, 1000.0);
				int ey = 0, ex = 0;
				std::frexp(y, &ey);
				std::frexp(x, &ex);
				if (std::abs(ey - ex) < 900) {
					note(a2, ulp_error(pm::atan2(y, x), ref.atan2(y, x)), y, x);
				}
			}
			check_le(as.ulps, 1.0, describe("asin", as));
			check_le(ac.ulps, 1.0, describe("acos", ac));
			check_le(at.ulps, 1.0, describe("atan", at));
			check_le(a2.ulps, 1.0, describe("atan2", a2));
		});

		test("exp_exp2_and_the_hyperbolics_are_within_one_ulp_to_overflow_and_underflow", []() {
			const reference ref;
			inputs in;
			worst_case e, e2, sh, ch, th;
			for (int i = 0; i < 3000; ++i) {
				const double x = in.uniform(-745.2, 709.8);
				note(e, ulp_error(pm::exp(x), ref.exp(dd{x})), x);
				const double y = in.uniform(-1076.0, 1024.0);
				note(e2, ulp_error(pm::exp2(y), ref.exp2(y)), y);
			}
			for (int i = 0; i < 1000; ++i) {
				const double x = in.uniform(-1.0, 1.0);
				note(e, ulp_error(pm::exp(x), ref.exp(dd{x})), x);
				const double small = in.scattered(-60.0, 0.0);
				note(e, ulp_error(pm::exp(small), ref.exp(dd{small})), small);
				note(e2, ulp_error(pm::exp2(small), ref.exp2(small)), small);
			}
			for (double x : {709.78, 709.782712893384, 709.7827128933840, -708.39, -708.3964185322641, -744.4, -745.1332191019411, -745.13321910194122}) {
				note(e, ulp_error(pm::exp(x), ref.exp(dd{x})), x);
			}
			for (double x : {1023.0, 1023.9999999999999, -1022.0, -1022.5, -1073.9, -1074.0, -1074.5, -1074.9}) {
				note(e2, ulp_error(pm::exp2(x), ref.exp2(x)), x);
			}
			for (int i = 0; i < 2000; ++i) {
				const double x = i < 1000 ? in.uniform(-30.0, 30.0) : in.scattered(-40.0, 9.47);
				note(sh, ulp_error(pm::sinh(x), ref.sinh(x)), x);
				note(ch, ulp_error(pm::cosh(x), ref.cosh(x)), x);
				note(th, ulp_error(pm::tanh(x), ref.tanh(x)), x);
			}
			for (double x : {710.4, 710.475860073943, -710.475860073943, 22.0, 21.999999999999996, 0.5, 0.25}) {
				note(sh, ulp_error(pm::sinh(x), ref.sinh(x)), x);
				note(ch, ulp_error(pm::cosh(x), ref.cosh(x)), x);
			}
			check_le(e.ulps, 1.0, describe("exp", e));
			check_le(e2.ulps, 1.0, describe("exp2", e2));
			check_le(sh.ulps, 1.0, describe("sinh", sh));
			check_le(ch.ulps, 1.0, describe("cosh", ch));
			check_le(th.ulps, 1.0, describe("tanh", th));
		});

		test("the_logarithms_are_within_one_ulp_from_subnormals_to_near_one", []() {
			const reference ref;
			inputs in;
			worst_case l, l2, l10;
			auto sample = [&](double x) {
				note(l, ulp_error(pm::log(x), ref.log(x)), x);
				note(l2, ulp_error(pm::log2(x), ref.log2(x)), x);
				note(l10, ulp_error(pm::log10(x), ref.log10(x)), x);
			};
			for (int i = 0; i < 3000; ++i) { sample(std::fabs(in.scattered(-1074.0, 1024.0))); }
			for (int i = 0; i < 2000; ++i) { sample(in.uniform(0.5, 2.0)); }
			for (int i = 1; i < 500; ++i) {   // beside 1
				sample(1.0 + static_cast<double>(i) * 0x1p-52);
				sample(1.0 - static_cast<double>(i) * 0x1p-53);
				sample(1.0 + std::fabs(in.scattered(-52.0, -5.0)));
				sample(1.0 - std::fabs(in.scattered(-53.0, -5.0)));
			}
			for (double x : {std::numeric_limits<double>::max(), std::numeric_limits<double>::min(), std::numeric_limits<double>::denorm_min(), 1.4142135623730951, 0.70710678118654757}) {
				sample(x);
			}
			check_le(l.ulps, 1.0, describe("log", l));
			check_le(l2.ulps, 1.0, describe("log2", l2));
			check_le(l10.ulps, 1.0, describe("log10", l10));
		});

		test("pow_is_within_one_ulp_including_negative_bases_and_the_range_edges", []() {
			const reference ref;
			inputs in;
			worst_case p;
			auto sample = [&](double x, double y) { note(p, ulp_error(pm::pow(x, y), ref.pow(x, y)), x, y); };
			for (int i = 0; i < 3000; ++i) { sample(in.uniform(0.0, 50.0), in.uniform(-40.0, 40.0)); }
			for (int i = 0; i < 1000; ++i) { sample(std::fabs(in.scattered(-1000.0, 1000.0)), in.uniform(-1.0, 1.0)); }
			for (int i = 0; i < 1000; ++i) {   // near 1 with large exponents
				const double x = 1.0 + in.uniform(-1e-5, 1e-5);
				sample(x, in.uniform(-6e7, 6e7));
			}
			for (int i = 0; i < 1000; ++i) {   // results near overflow and through the subnormals
				const double x = std::fabs(in.scattered(-20.0, 20.0));
				const double target = in.uniform(-1074.5, 1023.99);
				sample(x, target / std::log2(x));
			}
			for (int i = 0; i < 1000; ++i) {   // |y log x| beside 32, where log's precise form takes over
				const double x = in.uniform(0.01, 100.0);
				sample(x, ((i & 1) != 0 ? 32.0 : -32.0) * (1.0 + in.uniform(-1e-6, 1e-6)) / std::log(x));
			}
			for (int i = 0; i < 1000; ++i) {   // integer exponents of negative bases
				sample(-in.uniform(0.0, 10.0), std::round(in.uniform(-300.0, 300.0)));
			}
			check_le(p.ulps, 1.0, describe("pow", p));
		});

		test("cbrt_and_hypot_are_within_one_ulp_across_the_range", []() {
			const reference ref;
			inputs in;
			worst_case c, h;
			for (int i = 0; i < 3000; ++i) {
				const double x = in.scattered(-1074.0, 1024.0);
				note(c, ulp_error(pm::cbrt(x), ref.cbrt(x)), x);
			}
			for (int i = 0; i < 3000; ++i) {
				const double x = in.scattered(-1000.0, 1000.0), y = i < 1500 ? x * in.scattered(-30.0, 30.0) : in.scattered(-1000.0, 1000.0);
				if (std::isfinite(y)) {
					note(h, ulp_error(pm::hypot(x, y), ref.hypot(x, y)), x, y);
				}
			}
			for (double x : {std::numeric_limits<double>::max(), std::numeric_limits<double>::denorm_min(), 1e-310, 8.0, 27.0, -64.0}) {
				note(c, ulp_error(pm::cbrt(x), ref.cbrt(x)), x);
			}
			const double big = std::numeric_limits<double>::max();
			for (auto [x, y] : std::vector<std::pair<double, double>>{{big, big}, {big, 1.0}, {big * 0.5, big * 0.5}, {1e-310, 3e-310}, {3.0, 4.0}, {1e300, 1e300}, {5e-324, 5e-324}}) {
				note(h, ulp_error(pm::hypot(x, y), ref.hypot(x, y)), x, y);
			}
			check_le(c.ulps, 1.0, describe("cbrt", c));
			check_le(h.ulps, 1.0, describe("hypot", h));
		});

		test("special_values_follow_annex_f", []() {
			const double z = 0.0, nz = -0.0, big = std::numeric_limits<double>::max();
			struct row { const char* name; double result; double expected; };
			const std::vector<row> rows = {
				{"sin(-0)", pm::sin(nz), nz}, {"sin(+0)", pm::sin(z), z}, {"sin(inf)", pm::sin(INF), NAN_BITS}, {"sin(nan)", pm::sin(-NAN_BITS), NAN_BITS},
				{"cos(-0)", pm::cos(nz), 1.0}, {"cos(-inf)", pm::cos(-INF), NAN_BITS}, {"tan(-0)", pm::tan(nz), nz}, {"tan(inf)", pm::tan(INF), NAN_BITS},
				{"asin(-0)", pm::asin(nz), nz}, {"asin(2)", pm::asin(2.0), NAN_BITS}, {"acos(1)", pm::acos(1.0), z}, {"acos(-1)", pm::acos(-1.0), 3.141592653589793},
				{"acos(1.5)", pm::acos(1.5), NAN_BITS}, {"atan(-0)", pm::atan(nz), nz}, {"atan(inf)", pm::atan(INF), 1.5707963267948966}, {"atan(-inf)", pm::atan(-INF), -1.5707963267948966},
				{"atan2(+0,+0)", pm::atan2(z, z), z}, {"atan2(-0,+0)", pm::atan2(nz, z), nz}, {"atan2(+0,-0)", pm::atan2(z, nz), 3.141592653589793}, {"atan2(-0,-0)", pm::atan2(nz, nz), -3.141592653589793},
				{"atan2(+0,-1)", pm::atan2(z, -1.0), 3.141592653589793}, {"atan2(-0,1)", pm::atan2(nz, 1.0), nz}, {"atan2(1,0)", pm::atan2(1.0, z), 1.5707963267948966}, {"atan2(-1,-0)", pm::atan2(-1.0, nz), -1.5707963267948966},
				{"atan2(1,-inf)", pm::atan2(1.0, -INF), 3.141592653589793}, {"atan2(-1,inf)", pm::atan2(-1.0, INF), nz}, {"atan2(inf,1)", pm::atan2(INF, 1.0), 1.5707963267948966},
				{"atan2(inf,-inf)", pm::atan2(INF, -INF), 2.356194490192345}, {"atan2(-inf,inf)", pm::atan2(-INF, INF), -0.7853981633974483}, {"atan2(nan,1)", pm::atan2(NAN_BITS, 1.0), NAN_BITS},
				{"sinh(-0)", pm::sinh(nz), nz}, {"sinh(-inf)", pm::sinh(-INF), -INF}, {"sinh(711)", pm::sinh(711.0), INF}, {"cosh(-0)", pm::cosh(nz), 1.0}, {"cosh(-inf)", pm::cosh(-INF), INF},
				{"tanh(-0)", pm::tanh(nz), nz}, {"tanh(inf)", pm::tanh(INF), 1.0}, {"tanh(-inf)", pm::tanh(-INF), -1.0}, {"tanh(-30)", pm::tanh(-30.0), -1.0},
				{"exp(0)", pm::exp(z), 1.0}, {"exp(-0)", pm::exp(nz), 1.0}, {"exp(inf)", pm::exp(INF), INF}, {"exp(-inf)", pm::exp(-INF), z}, {"exp(710)", pm::exp(710.0), INF}, {"exp(-746)", pm::exp(-746.0), z},
				{"exp(1)", pm::exp(1.0), 2.718281828459045}, {"exp2(10)", pm::exp2(10.0), 1024.0}, {"exp2(-1074)", pm::exp2(-1074.0), 5e-324}, {"exp2(-1075)", pm::exp2(-1075.0), z}, {"exp2(1024)", pm::exp2(1024.0), INF},
				{"exp2(-inf)", pm::exp2(-INF), z}, {"exp2(0.5)", pm::exp2(0.5), 1.4142135623730951},
				{"log(1)", pm::log(1.0), z}, {"log(+0)", pm::log(z), -INF}, {"log(-0)", pm::log(nz), -INF}, {"log(-1)", pm::log(-1.0), NAN_BITS}, {"log(inf)", pm::log(INF), INF}, {"log(-inf)", pm::log(-INF), NAN_BITS},
{"log2(1024)", pm::log2(1024.0), 10.0}, {"log2(5e-324)", pm::log2(5e-324), -1074.0}, {"log2(1)", pm::log2(1.0), z}, {"log10(1000)", pm::log10(1000.0), 3.0},
				{"log10(1e-300)", pm::log10(1e-300), -300.0}, {"log10(1e22)", pm::log10(1e22), 22.0}, {"log2(0)", pm::log2(z), -INF}, {"log10(-2)", pm::log10(-2.0), NAN_BITS},
				{"pow(nan,0)", pm::pow(NAN_BITS, z), 1.0}, {"pow(1,nan)", pm::pow(1.0, NAN_BITS), 1.0}, {"pow(1,inf)", pm::pow(1.0, INF), 1.0}, {"pow(-1,inf)", pm::pow(-1.0, INF), 1.0}, {"pow(-1,-inf)", pm::pow(-1.0, -INF), 1.0},
				{"pow(2,nan)", pm::pow(2.0, NAN_BITS), NAN_BITS}, {"pow(-0,-3)", pm::pow(nz, -3.0), -INF}, {"pow(+0,-3)", pm::pow(z, -3.0), INF}, {"pow(-0,-2)", pm::pow(nz, -2.0), INF}, {"pow(-0,-0.5)", pm::pow(nz, -0.5), INF},
				{"pow(-0,3)", pm::pow(nz, 3.0), nz}, {"pow(-0,2)", pm::pow(nz, 2.0), z}, {"pow(-0,0.5)", pm::pow(nz, 0.5), z}, {"pow(-0,-inf)", pm::pow(nz, -INF), INF},
				{"pow(0.5,-inf)", pm::pow(0.5, -INF), INF}, {"pow(2,-inf)", pm::pow(2.0, -INF), z}, {"pow(0.5,inf)", pm::pow(0.5, INF), z}, {"pow(-2,inf)", pm::pow(-2.0, INF), INF},
				{"pow(-inf,-3)", pm::pow(-INF, -3.0), nz}, {"pow(-inf,-2)", pm::pow(-INF, -2.0), z}, {"pow(-inf,3)", pm::pow(-INF, 3.0), -INF}, {"pow(-inf,2.5)", pm::pow(-INF, 2.5), INF},
				{"pow(inf,-1)", pm::pow(INF, -1.0), z}, {"pow(inf,0.5)", pm::pow(INF, 0.5), INF}, {"pow(-8,1/3)", pm::pow(-8.0, 1.0 / 3.0), NAN_BITS}, {"pow(-2,3)", pm::pow(-2.0, 3.0), -8.0},
				{"pow(-2,-1)", pm::pow(-2.0, -1.0), -0.5}, {"pow(-3,2)", pm::pow(-3.0, 2.0), 9.0}, {"pow(2,10)", pm::pow(2.0, 10.0), 1024.0}, {"pow(10,22)", pm::pow(10.0, 22.0), 1e22}, {"pow(x,1)", pm::pow(0.1, 1.0), 0.1},
				{"pow(-10,309)", pm::pow(-10.0, 309.0), -INF}, {"pow(-10,-330)", pm::pow(-10.0, -330.0), z}, {"pow(-10,-331)", pm::pow(-10.0, -331.0), nz}, {"pow(2,-1074)", pm::pow(2.0, -1074.0), 5e-324},
				{"pow(big,2)", pm::pow(big, 2.0), INF}, {"pow(1.0000000000000002,2^70)", pm::pow(1.0000000000000002, 0x1p70), INF}, {"pow(0.9999999999999999,2^70)", pm::pow(0.9999999999999999, 0x1p70), z},
				{"cbrt(-0)", pm::cbrt(nz), nz}, {"cbrt(-inf)", pm::cbrt(-INF), -INF}, {"cbrt(-27)", pm::cbrt(-27.0), -3.0}, {"cbrt(nan)", pm::cbrt(NAN_BITS), NAN_BITS}, {"cbrt(2^-1071)", pm::cbrt(0x1p-1071), 0x1p-357},
				{"hypot(inf,nan)", pm::hypot(INF, NAN_BITS), INF}, {"hypot(nan,-inf)", pm::hypot(NAN_BITS, -INF), INF}, {"hypot(nan,1)", pm::hypot(NAN_BITS, 1.0), NAN_BITS}, {"hypot(-3,4)", pm::hypot(-3.0, 4.0), 5.0},
				{"hypot(-0,-0)", pm::hypot(nz, nz), z}, {"hypot(-5,0)", pm::hypot(-5.0, z), 5.0}, {"hypot(big,big)", pm::hypot(big, big), INF},
			};
			for (const auto& r : rows) {
				check(same_bits(r.result, r.expected), std::string(r.name) + " gave the wrong value");
			}
			double s = 1.0, c = 1.0;
			pm::sincos(nz, s, c);
			check(same_bits(s, nz) && same_bits(c, 1.0), "sincos(-0)");
			for (int i = -50; i < 50; ++i) {   // one reduction for both: the same bits as the separate calls
				const double x = static_cast<double>(i) * 0.37 + 1e-3;
				pm::sincos(x, s, c);
				check(same_bits(s, pm::sin(x)) && same_bits(c, pm::cos(x)), "sincos matches sin and cos");
			}
		});

		test("the_determinism_script_prints_its_committed_hash", []() {
			const auto path = std::filesystem::path(JAISCRIPT_TESTS_SOURCE_ROOT) / "tools" / "ci" / "math_determinism.jai";
			std::ifstream file(path, std::ios::binary);
			check(file.good(), "tools/ci/math_determinism.jai is missing");
			std::stringstream source;
			source << file.rdbuf();
			auto engine = make_engine();
			stdlib::register_all(*engine);
			auto out = std::make_shared<std::ostringstream>();
			engine->set_output_stream(out);
			engine->execute(source.str());
			const std::string printed = out->str();
			check(printed.find("portable math: PASS") != std::string::npos, printed);
		});
	}
};

}

using portable_math_tests = jai::foundry::tests::portable_math_tests;
FOUNDRY_REGISTER(portable_math_tests)
