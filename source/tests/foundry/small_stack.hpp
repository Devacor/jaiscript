#pragma once

// A test body run on a thread whose stack the test names, so a depth check holds on every build whatever stack the test binary
// itself reserves (/STACK, ulimit, an ASan job's suite stack)

#include <cstddef>
#include <exception>
#include <functional>
#include <stdexcept>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace jai::foundry::tests {

inline constexpr size_t k_small_stack = 1024 * 1024;

// Runs body on a thread with a stack of stack_bytes and rethrows what it threw
inline void on_small_stack(const std::function<void()>& body, size_t stack_bytes = k_small_stack) {
	struct run {
		const std::function<void()>* body;
		std::exception_ptr failure;
		static void go(run* self) {
			try { (*self->body)(); } catch (...) { self->failure = std::current_exception(); }
		}
	} state{&body, nullptr};
#if defined(_WIN32)
	struct thunk { static DWORD WINAPI entry(LPVOID self) { run::go(static_cast<run*>(self)); return 0; } };
	HANDLE thread = CreateThread(nullptr, stack_bytes, &thunk::entry, &state, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
	if (!thread) { throw std::runtime_error("the small-stack thread did not start"); }
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
#else
	struct thunk { static void* entry(void* self) { run::go(static_cast<run*>(self)); return nullptr; } };
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, stack_bytes);
	pthread_t thread;
	const int started = pthread_create(&thread, &attr, &thunk::entry, &state);
	pthread_attr_destroy(&attr);
	if (started != 0) { throw std::runtime_error("the small-stack thread did not start"); }
	pthread_join(thread, nullptr);
#endif
	if (state.failure) { std::rethrow_exception(state.failure); }
}

}
