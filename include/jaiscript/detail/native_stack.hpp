#pragma once

#ifndef __JAISCRIPT_DETAIL_NATIVE_STACK_HPP__
#define __JAISCRIPT_DETAIL_NATIVE_STACK_HPP__

#include <jaiscript/core/types.hpp>
#include <cstddef>
#include <cstdint>

namespace jai::detail {

	inline constexpr std::size_t k_native_call_margin = 256 * 1024;
	inline constexpr std::size_t k_native_nesting_margin = 192 * 1024;

	// True when less than k_native_call_margin of the stack the caller runs on remains (the thread's, or an engine::start run's
	// fiber's; false where the platform cannot be asked): the guard of a native recursion per script call
	bool native_stack_low() noexcept;

	// The same question with a smaller margin, so a call's own guard fires first inside a recursion of calls
	bool native_stack_nesting_low() noexcept;

	// [low, high) of the engine::start fiber running on this thread, false when none runs (script_run.cpp)
	bool running_fiber_stack(std::uintptr_t& a_low, std::uintptr_t& a_high) noexcept;

	// Where the caller's frame sits on the stack it runs on: not a local's address, which ASan's use-after-return detection
	// moves to a heap-allocated fake frame
	inline std::uintptr_t native_stack_here() noexcept {
#if defined(__GNUC__) || defined(__clang__)
		return reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
#else
		char probe;
		char* volatile where = &probe;   // MSVC warns C4172 at link-time code generation on &probe returned directly
		return reinterpret_cast<std::uintptr_t>(where);
#endif
	}

	inline constexpr std::string_view k_nesting_too_deep_listing = "nesting too deep for the host thread's stack";
	inline constexpr std::string_view k_nesting_too_deep = "Nesting too deep for the host thread's stack";

	// Source nested past the host thread's stack: a walk over it unwinds to its phase's entry, which reports it once
	class nesting_exhausted : public runtime_error {
	public:
		explicit nesting_exhausted(const source_location& at) : runtime_error(std::string(k_nesting_too_deep), at) {}
	};
	[[noreturn]] JAI_NOINLINE void throw_nesting_exhausted(const source_location& at);

	// A recursive walk over source (the parser, the checker, the slot pass, the compilers, the interpreter's evaluation) arms
	// this on entry, on the thread that runs it, and stops k_native_nesting_margin above the end of that stack. Windows asks the
	// thread's bounds at each check; elsewhere arm() asks once, and where the platform cannot say the walk may take
	// k_native_stack_budget bytes below the frame that armed it.
	class native_stack_mark {
	public:
		native_stack_mark() noexcept { arm(); }
		void arm() noexcept;
		bool low() const noexcept {
#if defined(_WIN32)
			return native_stack_nesting_low();
#else
			return native_stack_here() < limit_;
#endif
		}
		void guard(const source_location& at) const {
			if (low()) [[unlikely]] { throw_nesting_exhausted(at); }
		}

		static constexpr std::size_t k_native_stack_budget = 1024 * 1024;

	private:
		std::uintptr_t limit_ = 0;
	};

}

#endif // __JAISCRIPT_DETAIL_NATIVE_STACK_HPP__
