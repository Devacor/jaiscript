#ifndef __JAISCRIPT_DETAIL_PARALLEL_SCHEDULE_HPP__
#define __JAISCRIPT_DETAIL_PARALLEL_SCHEDULE_HPP__

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace jai::detail {

// The counted form's range (parallel_for (int i = a; i < b; i += k)): element p is a + p * k, never stored. Every value is an int
// (parallel_counted_range refused a last step that would overflow), so the unsigned walk names it exactly
struct parallel_counted_sequence {
	int64_t start = 0;
	uint64_t size = 0;   // |k|
	bool up = true;
	size_t n = 0;
	int64_t at(size_t p) const noexcept {
		const uint64_t travel = static_cast<uint64_t>(p) * size;
		return static_cast<int64_t>(up ? static_cast<uint64_t>(start) + travel : static_cast<uint64_t>(start) - travel);
	}
};

// A region's blocks, handed out in increasing order to whichever worker finishes its block first: a busy core takes fewer blocks.
// Each worker starts on its own first block; owner records who ran each block (the join reads an index-written slot back from it).
// A worker stops at its first error, the least index it ran, and every lower block was taken and ran to its end or to a lower
// error, so the least failing iteration over the workers is the serial loop's
struct parallel_block_schedule {
	std::atomic<size_t> next{0};
	size_t block = 1;
	size_t blocks = 0;
	size_t n = 0;
	std::vector<uint32_t> owner;
};

// one worker's place in the schedule: the element it is on and its block's end
struct parallel_schedule_cursor {
	parallel_block_schedule* schedule = nullptr;
	uint32_t worker = 0;
	size_t index = 0;
	size_t block_end = 0;
	size_t ran = 0;   // elements this worker took (the region's cost model)

	bool take(size_t b) noexcept {
		if (b >= schedule->blocks) { return false; }
		schedule->owner[b] = worker;
		index = b * schedule->block;
		block_end = index + schedule->block < schedule->n ? index + schedule->block : schedule->n;
		ran += block_end - index;
		return true;
	}
	// false: no block is left
	bool advance() noexcept {
		if (++index < block_end) { return true; }
		return take(schedule->next.fetch_add(1, std::memory_order_relaxed));
	}
};

}

#endif
