#pragma once

#ifndef __JAISCRIPT_DETAIL_WORK_BUDGET_HPP__
#define __JAISCRIPT_DETAIL_WORK_BUDGET_HPP__

#include <jaiscript/jaiscript_fwd.hpp>
#include <jaiscript/core/checked_result.hpp>
#include <cstddef>
#include <cstdint>

namespace jai::detail {

	// The execution budget inside the engine's own builtins. A backend samples the clock at loop back-edges and call entries;
	// one builtin call is neither, so a builtin whose cost grows with its input counts its work here, in units of about one
	// element visited or k_work_unit_bytes bytes moved, and every k_work_chunk units are one budget check: a clock read against
	// the running entry's deadline and one step of the step budget. A builtin that finds the budget spent stops and reports the
	// terminal budget error, so an overrun inside one is bounded by a chunk of its work. Work below k_work_grain in one builtin
	// call is left to the backend's samples (the call sits in a loop or it is short), so a small call adds and compares.
	inline constexpr size_t k_work_grain = 1024;
	inline constexpr uint64_t k_work_chunk = uint64_t(1) << 16;
	inline constexpr size_t k_work_unit_bytes = 16;

	// Adds a_units to the calling thread's count (a parallel worker's own); true once the running entry's budget is spent
	// (its terminal latch set). Never true while no script runs: a host's own call of a builtin has no entry to outrun.
	bool charge_work(engine& a_engine, size_t a_units);
	error_propagator work_budget_error(engine& a_engine) noexcept;
	// The same error for a walk that reports by throwing (a deep copy): it leaves as the terminal budget error
	[[noreturn]] void throw_work_budget(engine& a_engine);

	// One builtin call's count
	class work_meter {
	public:
		explicit work_meter(engine* a_engine) noexcept : engine_(a_engine) {}

		[[nodiscard]] JAI_FORCEINLINE bool spend(size_t a_units) {
			if (a_units < k_work_grain - pending_) [[likely]] {
				pending_ += a_units;
				return false;
			}
			return hand_over(a_units);
		}
		[[nodiscard]] JAI_FORCEINLINE bool spend_bytes(size_t a_bytes) { return spend(a_bytes / k_work_unit_bytes); }

		// The items of a block operation's next run (a_remaining left, each a_bytes_each bytes): never more than a chunk of work
		static size_t run_of(size_t a_remaining, size_t a_bytes_each = k_work_unit_bytes) noexcept {
			const size_t each = a_bytes_each < k_work_unit_bytes ? k_work_unit_bytes / (a_bytes_each ? a_bytes_each : 1) : 1;
			const size_t most = static_cast<size_t>(k_work_chunk) * each;
			return a_remaining < most ? a_remaining : most;
		}

		error_propagator spent() const noexcept { return work_budget_error(*engine_); }
		[[noreturn]] void throw_spent() const { throw_work_budget(*engine_); }

	private:
		JAI_NOINLINE bool hand_over(size_t a_units) {
			const size_t units = a_units > SIZE_MAX - pending_ ? SIZE_MAX : pending_ + a_units;
			pending_ = 0;
			return engine_ && charge_work(*engine_, units);
		}

		engine* engine_;
		size_t pending_ = 0;
	};

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_WORK_BUDGET_HPP__
