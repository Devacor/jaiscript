#pragma once

#ifndef __JAISCRIPT_STDLIB_RANDOM_HPP__
#define __JAISCRIPT_STDLIB_RANDOM_HPP__

#include <jaiscript/core/types.hpp>
#include <jaiscript/detail/random_generator.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jai {
class engine;
class script_value;
namespace stdlib {

	// random (guide 18-random.html): the generator type, a value holding one xoshiro256** stream. A copy holds the same state, so
	// it replays the same draws; the state round-trips through every archive as 64 hex digits.
	class random {
	public:
		random();                                  // seeded from the operating system's secure generator
		explicit random(int64_t a_seed);
		random(const random& a_other);
		random& operator=(const random& a_other);

		uint64_t next();
		int64_t next_int(int64_t a_lo, int64_t a_hi);   // both bounds inclusive; throws on an inverted range
		double next_float();                            // [0, 1)
		double next_float(double a_lo, double a_hi);    // [a_lo, a_hi); throws on an inverted or non-finite range
		void seed(int64_t a_seed);
		bool next_bool();
		size_t next_index(size_t a_count);              // [0, a_count) for a_count > 0
		std::string next_bytes(int64_t a_count);

		std::string state() const;
		bool set_state(std::string_view a_hex);         // false for anything but 64 hex digits that are not all zero

		// A parallel worker's copy of a captured random: element i draws the stream seeded from region_element_key(seed, i);
		// the cursors name the element running (their first entry's element index)
		void begin_region(const std::vector<script_value>* a_cursors, uint64_t a_seed) noexcept;

		template<class Archive>
		void save(Archive& a_archive) const {
			a_archive("state", state());
		}

		template<class Archive>
		void load(Archive& a_archive) {
			std::string hex;
			a_archive("state", hex);
			if (!set_state(hex)) { throw runtime_error(k_state_text); }
		}

		static constexpr const char* k_state_text = "random::state expects 64 hex digits, not all zero";

	private:
		void follow_region() const;

		// a region copy moves to the next element's stream when its cursor moves (logically const: a read of the state syncs)
		mutable detail::xoshiro256 core_;
		mutable size_t region_element_ = SIZE_MAX;
		const std::vector<script_value>* region_cursors_ = nullptr;
		uint64_t region_seed_ = 0;
	};

	// The random type's binding (source/implementation/stdlib/random.cpp)
	void register_random_functions(engine& eng_ref);

	namespace random_detail {
		// A captured random a parallel region provisions: true when a_value holds one
		bool is_random(const script_value& a_value);
		// One draw from the random a_value holds (the region's seed), and a worker's copy that draws per element from it
		uint64_t draw_region_seed(const script_value& a_value);
		script_value region_copy(const script_value& a_value, engine* a_engine, const std::vector<script_value>* a_cursors, uint64_t a_seed);
		void begin_region(const script_value& a_copy, const std::vector<script_value>* a_cursors, uint64_t a_seed);
	}

} // namespace stdlib
} // namespace jai

#endif
