#pragma once

#ifndef __JAISCRIPT_DETAIL_HOST_TIME_HPP__
#define __JAISCRIPT_DETAIL_HOST_TIME_HPP__

#include <jaiscript/jaiscript_fwd.hpp>
#include <jaiscript/detail/execution_limits.hpp>
#include <chrono>
#include <cstdint>

#if !defined(_WIN32)
#include <time.h>
#endif

namespace jai {
	class execution_backend;
}

namespace jai::detail {

	// The wall-clock execution budget counts everything a script's entry runs, a host's bound code included, except a
	// binding the host marked jai::exclude_execution_time (a load, a wait on a service). A marked binding stores one
	// callable around its own (engine::time_excluded_function), which reads a coarse clock as the call starts and as it
	// returns; the ticks between move the running entry's deadline later. The clock is one the platform keeps current by
	// itself, so a read is a load; its tick is a millisecond or more, which makes the credit exact to a tick for a long call
	// and, over many short ones, the tick boundaries that fell inside them: the call's time on average, never more. Script
	// such a call runs (a callback, a run it steps) stops the call's clock for as long as it runs.

#if defined(_WIN32)
	// KUSER_SHARED_DATA.InterruptTime, mapped read-only at this address in every process: 100 ns units, advanced by the
	// clock interrupt (what QueryInterruptTime reads, without the call)
	inline constexpr int64_t k_host_tick_nanoseconds = 100;
	JAI_FORCEINLINE uint64_t host_clock_ticks() noexcept {
#if defined(_WIN64)
		return *reinterpret_cast<const volatile uint64_t*>(static_cast<uintptr_t>(0x7FFE0008));
#else
		const volatile uint32_t* const time = reinterpret_cast<const volatile uint32_t*>(static_cast<uintptr_t>(0x7FFE0008));
		for (;;) {   // low, high, high again: the kernel writes the second high last
			const uint32_t high = time[1];
			const uint32_t low = time[0];
			if (high == time[2]) { return (static_cast<uint64_t>(high) << 32) | low; }
		}
#endif
	}
#elif defined(__APPLE__)
	inline constexpr int64_t k_host_tick_nanoseconds = 1;
	JAI_FORCEINLINE uint64_t host_clock_ticks() noexcept { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW_APPROX); }
#elif defined(CLOCK_MONOTONIC_COARSE) && !defined(__EMSCRIPTEN__)
	inline constexpr int64_t k_host_tick_nanoseconds = 1;
	JAI_FORCEINLINE uint64_t host_clock_ticks() noexcept {
		timespec now{};
		clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
		return static_cast<uint64_t>(now.tv_sec) * 1000000000u + static_cast<uint64_t>(now.tv_nsec);
	}
#else
	// no clock cheaper than the precise one (Emscripten's is a call into the page): a timed call pays two reads of it
	inline constexpr int64_t k_host_tick_nanoseconds = 1;
	JAI_FORCEINLINE uint64_t host_clock_ticks() noexcept {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}
#endif

	JAI_FORCEINLINE uint64_t host_ticks_now(const execution_limits& a_limits) noexcept { return host_clock_ticks() + a_limits.host_tick_skew; }

	inline std::chrono::nanoseconds host_ticks_span(uint64_t a_ticks) noexcept {
		return std::chrono::nanoseconds(static_cast<int64_t>(a_ticks) * k_host_tick_nanoseconds);
	}

	// The ticks the host call open on a_limits has taken, closing it; 0 when none is open or no tick passed
	JAI_FORCEINLINE uint64_t host_ticks_taken(execution_limits& a_limits) noexcept {
		const uint64_t since = a_limits.host_since;
		a_limits.host_since = 0;
		return since ? host_ticks_now(a_limits) - since : 0;
	}

	// Moves a_backend's running deadline later by a_ticks of host time (engine.cpp)
	void credit_host_ticks(execution_backend& a_backend, execution_limits& a_limits, uint64_t a_ticks) noexcept;
	// The limits a marked call times against: the calling thread's, with its backend, or null when that backend has no
	// wall-clock budget armed
	execution_limits* host_call_limits(engine& a_engine, execution_backend*& a_backend) noexcept;

	// One call of a binding marked jai::exclude_execution_time
	class host_call_time {
	public:
		explicit host_call_time(engine& a_engine) noexcept : limits_(host_call_limits(a_engine, backend_)) {
			if (limits_) { limits_->host_since = host_ticks_now(*limits_); }
		}
		JAI_FORCEINLINE ~host_call_time() {
			if (limits_) {
				if (const uint64_t ticks = host_ticks_taken(*limits_)) [[unlikely]] { credit_host_ticks(*backend_, *limits_, ticks); }
			}
		}
		host_call_time(const host_call_time&) = delete;
		host_call_time& operator=(const host_call_time&) = delete;

	private:
		execution_backend* backend_ = nullptr;
		execution_limits* limits_;
	};

	// Script running inside a host call (the host called back, executed, resumed a run): the call's time so far is credited
	// before the script's first sample, and the call's clock starts again when the script returns
	class host_time_pause {
	public:
		JAI_FORCEINLINE host_time_pause(execution_limits& a_limits, execution_backend& a_backend) noexcept : limits_(a_limits.host_since ? &a_limits : nullptr) {
			if (limits_) [[unlikely]] {
				if (const uint64_t ticks = host_ticks_taken(*limits_)) { credit_host_ticks(a_backend, *limits_, ticks); }
			}
		}
		JAI_FORCEINLINE ~host_time_pause() {
			if (limits_) [[unlikely]] { limits_->host_since = host_ticks_now(*limits_); }
		}
		host_time_pause(const host_time_pause&) = delete;
		host_time_pause& operator=(const host_time_pause&) = delete;

	private:
		execution_limits* limits_;
	};

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_HOST_TIME_HPP__
