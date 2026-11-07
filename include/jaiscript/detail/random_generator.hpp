#pragma once

#ifndef __JAISCRIPT_DETAIL_RANDOM_GENERATOR_HPP__
#define __JAISCRIPT_DETAIL_RANDOM_GENERATOR_HPP__

// The random type's generator (guide 18-random.html): every formula here is part of the language contract, so a seed gives the
// same bits on every platform, compiler and backend. Integer arithmetic only, except unit(), which is exact (an integer of at most
// 53 bits times a power of two).

#include <cstddef>
#include <cstdint>

namespace jai::detail {

	inline constexpr uint64_t k_splitmix64_increment = 0x9E3779B97F4A7C15ull;

	// splitmix64's output function (Steele, Lea, Flood 2014)
	constexpr uint64_t splitmix64_mix(uint64_t a_z) noexcept {
		a_z = (a_z ^ (a_z >> 30)) * 0xBF58476D1CE4E5B9ull;
		a_z = (a_z ^ (a_z >> 27)) * 0x94D049BB133111EBull;
		return a_z ^ (a_z >> 31);
	}

	// The high 64 bits of a 64 x 64 bit product
	constexpr uint64_t multiply_high_64(uint64_t a_x, uint64_t a_y) noexcept {
#if defined(__SIZEOF_INT128__)
		return static_cast<uint64_t>((static_cast<unsigned __int128>(a_x) * a_y) >> 64);
#else
		const uint64_t x_lo = a_x & 0xFFFFFFFFull, x_hi = a_x >> 32;
		const uint64_t y_lo = a_y & 0xFFFFFFFFull, y_hi = a_y >> 32;
		const uint64_t lo_lo = x_lo * y_lo, hi_lo = x_hi * y_lo, lo_hi = x_lo * y_hi, hi_hi = x_hi * y_hi;
		const uint64_t middle = (lo_lo >> 32) + (hi_lo & 0xFFFFFFFFull) + lo_hi;
		return hi_hi + (hi_lo >> 32) + (middle >> 32);
#endif
	}

	// A parallel region's key for one element (or one parallel_reduce chunk): its stream depends on the region's seed and the
	// element's index, never on the worker that runs it
	constexpr uint64_t region_element_key(uint64_t a_seed, uint64_t a_element) noexcept {
		return splitmix64_mix(a_seed ^ splitmix64_mix(a_element + k_splitmix64_increment));
	}

	// Uniform in [0, a_span) for a_span > 0 from a source of 64-bit words, unbiased (Lemire 2019, "Fast Random Integer Generation
	// in an Interval")
	template<typename Next>
	uint64_t uniform_below(uint64_t a_span, Next&& a_next) {
		uint64_t x = a_next();
		uint64_t low = x * a_span;
		if (low < a_span) {
			const uint64_t threshold = (0 - a_span) % a_span;
			while (low < threshold) {
				x = a_next();
				low = x * a_span;
			}
		}
		return multiply_high_64(x, a_span);
	}

	// Uniform over [a_lo, a_hi], both inclusive (a_lo <= a_hi); the whole int64 range is one raw word
	template<typename Next>
	int64_t uniform_between(int64_t a_lo, int64_t a_hi, Next&& a_next) {
		const uint64_t span = static_cast<uint64_t>(a_hi) - static_cast<uint64_t>(a_lo) + 1;
		const uint64_t offset = span == 0 ? a_next() : uniform_below(span, a_next);
		return static_cast<int64_t>(static_cast<uint64_t>(a_lo) + offset);
	}

	// xoshiro256** 1.0 (Blackman, Vigna 2018), seeded by four successive splitmix64 outputs
	struct xoshiro256 {
		uint64_t words[4] {};

		static constexpr uint64_t rotate_left(uint64_t a_x, int a_k) noexcept { return (a_x << a_k) | (a_x >> (64 - a_k)); }

		void seed(uint64_t a_seed) noexcept {
			for (uint64_t& word : words) {
				a_seed += k_splitmix64_increment;
				word = splitmix64_mix(a_seed);
			}
		}

		uint64_t next() noexcept {
			const uint64_t result = rotate_left(words[1] * 5, 7) * 9;
			const uint64_t shifted = words[1] << 17;
			words[2] ^= words[0];
			words[3] ^= words[1];
			words[1] ^= words[2];
			words[0] ^= words[3];
			words[2] ^= shifted;
			words[3] = rotate_left(words[3], 45);
			return result;
		}

		uint64_t below(uint64_t a_span) noexcept { return uniform_below(a_span, [this]() { return next(); }); }
		int64_t between(int64_t a_lo, int64_t a_hi) noexcept { return uniform_between(a_lo, a_hi, [this]() { return next(); }); }

		// [0, 1): the top 53 bits as a multiple of 2^-53
		double unit() noexcept { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

		bool coin() noexcept { return (next() >> 63) != 0; }

		bool is_zero() const noexcept { return (words[0] | words[1] | words[2] | words[3]) == 0; }
	};

} // namespace jai::detail

#endif
