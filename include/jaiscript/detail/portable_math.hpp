#pragma once

#ifndef __JAISCRIPT_DETAIL_PORTABLE_MATH_HPP__
#define __JAISCRIPT_DETAIL_PORTABLE_MATH_HPP__

// The language's transcendental functions, the same bits on every platform, compiler and backend: argument reduction and
// polynomials in IEEE basic operations only, in a fixed order (source/implementation/portable_math.cpp). Every result is
// within 1 ulp of the exact value; special values follow C's Annex F. A NaN result is the one quiet NaN 0x7FF8000000000000.
namespace jai::detail::portable_math {

	double sin(double x) noexcept;
	double cos(double x) noexcept;
	double tan(double x) noexcept;
	// sin and cos of one angle, reduced once
	void sincos(double x, double& sin_out, double& cos_out) noexcept;
	double asin(double x) noexcept;
	double acos(double x) noexcept;
	double atan(double x) noexcept;
	double atan2(double y, double x) noexcept;
	double sinh(double x) noexcept;
	double cosh(double x) noexcept;
	double tanh(double x) noexcept;
	double exp(double x) noexcept;
	double exp2(double x) noexcept;
	double log(double x) noexcept;
	double log2(double x) noexcept;
	double log10(double x) noexcept;
	double pow(double x, double y) noexcept;
	double cbrt(double x) noexcept;
	double hypot(double x, double y) noexcept;

} // namespace jai::detail::portable_math

#endif // __JAISCRIPT_DETAIL_PORTABLE_MATH_HPP__
