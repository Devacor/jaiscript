#pragma once

#ifndef __JAISCRIPT_DETAIL_EXECUTION_LIMITS_HPP__
#define __JAISCRIPT_DETAIL_EXECUTION_LIMITS_HPP__

#include <jaiscript/jaiscript_fwd.hpp>
#include <jaiscript/core/checked_result.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace jai::detail {

	// Execution-limit state shared VERBATIM by both backends and the shared builtin
	// methods (parity by construction). The engine owns one instance; the active
	// backend caches a pointer to it, so reentrant executes share the outer run's
	// allowance. Instance state only - never static (zero-statics/multi-engine rule).
	struct execution_limits {
		// Per-engine configuration, not process-wide mutable state; hosts can override either.
		int interpreter_call_depth_limit = 256;
		int vm_call_depth_limit = 250000;   // a backstop: the memory cap and the execution budget bound a recursion first
		// The vm's call path compares the depth against this gate: the cap, or the next frame-storage charge below it
		int vm_depth_gate = 0;
		size_t vm_frame_bytes = 0;   // frame storage charged to memory_used this execute

		// Terminal-error latch: while set, EVERY script catch handler is skipped -
		// try/catch, catch-all, nested, catches inside coroutines/lambdas, and catches
		// in outer scripts around a failing reentrant execute(). The failure unwinds
		// to the outermost host execute()/next() boundary, which reports it normally.
		// Budget overruns latch this ALWAYS (scripts can never catch a timeout).
		// Cleared only by a non-reentrant prepare_for_execution or a host-level resume.
		bool terminal_error = false;

		// The longest NUL scan one binary::reader read runs (engine::binary_scan_limit); only the host sets it
		size_t binary_scan_limit = size_t(1) << 20;
		// The bytes one template() render may write (engine::template_output_limit) and one scoped_template render
		// (engine::scoped_template_output_limit); 0 = no limit
		size_t template_output_limit = size_t(10) << 20;
		size_t scoped_template_output_limit = size_t(1) << 20;
		// The engine's clock sample: a real steady_clock reading (ticks) written where the engine reads that clock anyway -
		// every host entry (the reading the budget deadline starts from), the budget check's periodic samples (every 1024
		// ticks, budget or not) and a time:: read of the system clock; never by a host engine_time::source. The import window reads
		// it instead of the clock; clock_samples counts the writes
		int64_t sampled_now = 0;
		uint64_t clock_samples = 0;
		void note_clock_sample(std::chrono::steady_clock::time_point now) noexcept {
			sampled_now = now.time_since_epoch().count();
			++clock_samples;
		}
		// The engine-wide import check pass (engine::import_skips): open while every import reached stats its modules; no
		// new pass opens while sampled_now is before import_window_until. A top-level execute or resume closes it.
		bool import_pass_open = false;
		int64_t import_window_until = 0;

		// Approximate script memory accounting (engine::memory_cap). HIGH-WATER MARK:
		// charges only grow during one top-level execute (releases are not credited
		// back - v1 simplification) and the counter resets per top-level execute. On
		// each raise the counter re-arms to zero so a script that catches the first
		// denial can free its caches and keep working; the SECOND raise in the same
		// execute latches terminal_error (uncatchable).
		size_t memory_used = 0;
		size_t memory_limit = SIZE_MAX;     // effective limit (SIZE_MAX = unlimited)
		size_t memory_cap = 0;              // configured cap in bytes (0 = unlimited)
		uint32_t memory_raises = 0;
		uint64_t memory_cap_symbol_id = 0;  // interned decimal cap size for error text

		// Denial-style charge: false = the allocation would exceed the cap and nothing
		// was added (the value must not come into existence - the caller raises).
		bool memory_charge(size_t bytes) noexcept {
			if (memory_used + bytes > memory_limit) [[unlikely]] { return false; }
			memory_used += bytes;
			return true;
		}
		// The denial-style charge for count items of `each` bytes. A total past size_t is past any cap; with no cap the
		// allocation itself reports it.
		bool memory_charge_items(size_t count, size_t each) noexcept {
			if (each != 0 && count > (SIZE_MAX - memory_used) / each) [[unlikely]] { return memory_limit == SIZE_MAX; }
			return memory_charge(count * each);
		}
		// Count-only charge for sites that cannot deny in place (environment growth,
		// internal clones); the raise happens at the next loop back-edge / call entry.
		void memory_charge_deferred(size_t bytes) noexcept { memory_used += bytes; }
		bool memory_tripped() const noexcept { return memory_used > memory_limit; }

		// engine::execution_step_budget: budget checks one host entry may take (0 = unlimited), counted on the engine thread
		uint64_t step_budget = 0;
		uint64_t steps_taken = 0;
		bool step_budget_spent() noexcept { return step_budget != 0 && ++steps_taken > step_budget; }

		void reset_for_execute() noexcept {
			terminal_error = false;
			steps_taken = 0;
			vm_depth_gate = 0;
			vm_frame_bytes = 0;
			memory_used = 0;
			memory_raises = 0;
			import_pass_open = false;
		}
	};

	// Number-free because backend caps and available native stacks can differ.
	// Allocation-free, including on workers whose symbolizer is frozen.
	inline error_propagator raise_call_depth() noexcept {
		return error_propagator{ make_error_code(runtime_error_code::max_recursion_depth),
			"Maximum recursion depth exceeded - possible infinite recursion" };
	}

	// The host thread's stack ran out below the call-depth cap (the interpreter recurses natively per script call, the vm on
	// host reentry and constructor phases): its own text, so the cap and the stack read apart. Allocation-free like the cap's.
	inline error_propagator raise_native_stack() noexcept {
		return error_propagator{ make_error_code(runtime_error_code::max_recursion_depth),
			"Native stack exhausted before the call-depth cap: the host thread's stack bounds this recursion" };
	}

	// Evaluation nested past the host thread's stack within one call (the source's own nesting, never a recursion of calls)
	inline error_propagator raise_nesting_depth() noexcept {
		return error_propagator{ make_error_code(runtime_error_code::max_recursion_depth), "Nesting too deep for the host thread's stack" };
	}

	// A started coroutine keeps its state (frames, environment, stack) while it is suspended: each start counts this much
	// against the memory cap on both backends (the vm's fiber stack growth past its first block counts as well)
	inline constexpr size_t k_coroutine_state_charge = 2048;

	// The vm charges its frames' storage to the memory allowance once per this many levels of depth
	inline constexpr int k_frame_charge_levels = 10000;

	// A `: this(...)` chain deeper than this is a delegation cycle (ill-formed in C++): both backends raise the recursion text
	inline constexpr size_t k_max_constructor_delegation_depth = 256;

	struct call_depth_guard {
		int& depth;
		explicit call_depth_guard(int& value) noexcept : depth(value) { ++depth; }
		~call_depth_guard() { --depth; }
		call_depth_guard(const call_depth_guard&) = delete;
		call_depth_guard& operator=(const call_depth_guard&) = delete;
	};

	// The budget that ran out names the knob that raises it: the step budget (engine::execution_step_budget) once its count is
	// spent, else the wall-clock one (engine::execution_budget). Both backends raise through this.
	inline error_propagator raise_execution_budget(const execution_limits& limits) noexcept {
		const bool steps = limits.step_budget != 0 && limits.steps_taken > limits.step_budget;
		return error_propagator{ make_error_code(runtime_error_code::execution_budget_exceeded),
			steps ? std::string_view("Script execution budget exceeded - raise engine::execution_step_budget or break up the work")
			      : std::string_view("Script execution budget exceeded - raise engine::execution_budget or break up the work") };
	}

	// Style-matched to the budget error text; {0} resolves to the interned cap size.
	inline constexpr std::string_view k_memory_cap_message =
		"Script memory cap exceeded ({0} bytes) - raise engine::memory_cap or break up the work";

	// The single raise point for memory-cap errors (identical text/semantics on both
	// backends and in the shared builtin methods). Re-arms the counter so a catching
	// script gets a fresh allowance to clean up; the second raise per top-level execute
	// latches terminal (ONE script catch per execute, then terminal).
	inline error_propagator raise_memory_cap(execution_limits& limits) noexcept {
		limits.memory_used = 0;
		if (++limits.memory_raises >= 2) { limits.terminal_error = true; }
		return error_propagator{ make_error_code(runtime_error_code::memory_cap_exceeded),
			k_memory_cap_message, limits.memory_cap_symbol_id };
	}

	// The same raise for a native writer that reports by throwing (the text a script catches is the same)
	[[noreturn]] JAI_NOINLINE inline void throw_memory_cap(execution_limits& limits) {
		const size_t cap = limits.memory_cap;
		(void)raise_memory_cap(limits);
		std::string text(k_memory_cap_message);
		text.replace(text.find("{0}"), 3, std::to_string(cap));
		throw std::runtime_error(text);
	}

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_EXECUTION_LIMITS_HPP__
