#pragma once

#ifndef __JAISCRIPT_DETAIL_SCALAR_OPS_HPP__
#define __JAISCRIPT_DETAIL_SCALAR_OPS_HPP__

// The scalar arithmetic and comparison under every lane of both backends: int op int under the overflow policy
// (detail/integer_ops.hpp), float op float with its zero-divisor rule, and the one producer of their error texts. A lane
// differs from another only in how it fetches operands and lands the result; a lane that misses on a fault (a cell, a
// typed op) ignores the text.

#include <jaiscript/jaiscript_fwd.hpp>
#include <jaiscript/core/checked_result.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/detail/integer_ops.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <cmath>
#include <cstdint>

namespace jai::detail {

	enum class scalar_fault : uint8_t { none, overflow, division_by_zero, modulo_by_zero };

	constexpr bool scalar_arith_op(token_type op) noexcept {
		return op == token_type::plus || op == token_type::minus || op == token_type::star || op == token_type::slash ||
		       op == token_type::percent;
	}
	constexpr bool scalar_compare_op(token_type op) noexcept {
		return op == token_type::less || op == token_type::less_equal || op == token_type::greater ||
		       op == token_type::greater_equal || op == token_type::equal_equal || op == token_type::bang_equal;
	}

	template <class T>
	JAI_FORCEINLINE bool scalar_compare(token_type op, T a, T b) noexcept {
		switch (op) {
			case token_type::less: return a < b;
			case token_type::less_equal: return a <= b;
			case token_type::greater: return a > b;
			case token_type::greater_equal: return a >= b;
			case token_type::equal_equal: return a == b;
			default: return a != b;
		}
	}

	JAI_FORCEINLINE scalar_fault int_arith(token_type op, script_int a, script_int b, script_int& out) noexcept {
		switch (op) {
			case token_type::plus: return ints::try_add(a, b, out) ? scalar_fault::none : scalar_fault::overflow;
			case token_type::minus: return ints::try_sub(a, b, out) ? scalar_fault::none : scalar_fault::overflow;
			case token_type::star: return ints::try_mul(a, b, out) ? scalar_fault::none : scalar_fault::overflow;
			case token_type::slash:
				if (b == 0) { return scalar_fault::division_by_zero; }
				return ints::try_div(a, b, out) ? scalar_fault::none : scalar_fault::overflow;   // INT64_MIN / -1
			default:
				if (b == 0) { return scalar_fault::modulo_by_zero; }
				out = ints::mod(a, b);   // a % -1 == 0, no trap
				return scalar_fault::none;
		}
	}

	JAI_FORCEINLINE scalar_fault float_arith(token_type op, script_float a, script_float b, script_float& out) noexcept {
		switch (op) {
			case token_type::plus: out = a + b; return scalar_fault::none;
			case token_type::minus: out = a - b; return scalar_fault::none;
			case token_type::star: out = a * b; return scalar_fault::none;
			case token_type::slash:
				if (b == 0.0) { return scalar_fault::division_by_zero; }
				out = a / b;
				return scalar_fault::none;
			default:
				if (b == 0.0) { return scalar_fault::modulo_by_zero; }
				out = std::fmod(a, b);
				return scalar_fault::none;
		}
	}

	inline const char* int_overflow_text(token_type op) noexcept {
		switch (op) {
			case token_type::plus: return "Integer overflow in '+'";
			case token_type::minus: return "Integer overflow in '-'";
			case token_type::star: return "Integer overflow in '*'";
			default: return "Integer overflow in '/'";
		}
	}

	// a compound assignment's overflow names the compound (index 0..3: += -= *= /=), ++ and -- name themselves
	inline constexpr int k_compound_increment = 10;
	inline constexpr int k_compound_decrement = 11;
	inline const char* compound_overflow_text(int compound) noexcept {
		switch (compound) {
			case 1: return "Integer overflow in '-='";
			case 2: return "Integer overflow in '*='";
			case 3: return "Integer overflow in '/='";
			case k_compound_increment: return "Integer overflow in '++'";
			case k_compound_decrement: return "Integer overflow in '--'";
			default: return "Integer overflow in '+='";
		}
	}

	// The error a binary operator's fault raises (one text per operator family; a compound's overflow names the compound)
	template <class Result>
	Result scalar_fault_error(token_type op, scalar_fault fault, int compound = -1) {
		switch (fault) {
			case scalar_fault::overflow:
				return Result(make_error_code(runtime_error_code::invalid_numeric_operand), compound >= 0 ? compound_overflow_text(compound) : int_overflow_text(op));
			case scalar_fault::division_by_zero:
				return Result(make_error_code(runtime_error_code::division_by_zero),
				              "Division by zero");
			default:
				return Result(make_error_code(runtime_error_code::modulo_by_zero),
				              "Modulo by zero");
		}
	}

}

#endif
