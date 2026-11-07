// Optional ETW markers. Each smoke execution owns its provider registration.
#pragma once

#ifdef GLOOM_PROFILE_CAPTURE
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <evntprov.h>
#include <string>
#include <cstdint>
#include <cstdio>

#pragma comment(lib, "advapi32.lib")

namespace gloom {
class profile_capture {
public:
	explicit profile_capture(const wchar_t* a_label = L"GLOOM measured loop") :
		label_(a_label) {
		const GUID provider{0x744b3f90, 0x7d83, 0x4b7b, {0xa8, 0x61, 0x40, 0x63, 0x76, 0x23, 0xc5, 0x92}};
		EventRegister(&provider, nullptr, nullptr, &registration_);
	}
	~profile_capture() {
		stop();
		if (registration_) { EventUnregister(registration_); }
	}
	void begin() {
		if (!registration_) { return; }
		active_ = true;
		cpu_begin_ = read_cpu_times();
		EventWriteString(registration_, 4, 1, (label_ + L" begin").c_str());
	}
	void stop() {
		if (registration_ && active_) {
			EventWriteString(registration_, 4, 1, (label_ + L" end").c_str());
			const auto cpu_end = read_cpu_times();
			if (cpu_begin_.valid && cpu_end.valid) {
				fprintf(stderr, "[profile-cpu] process_user_100ns=%llu process_kernel_100ns=%llu thread_user_100ns=%llu thread_kernel_100ns=%llu\n",
					static_cast<unsigned long long>(cpu_end.process_user - cpu_begin_.process_user),
					static_cast<unsigned long long>(cpu_end.process_kernel - cpu_begin_.process_kernel),
					static_cast<unsigned long long>(cpu_end.thread_user - cpu_begin_.thread_user),
					static_cast<unsigned long long>(cpu_end.thread_kernel - cpu_begin_.thread_kernel));
			} else {
				fprintf(stderr, "[profile-cpu] unavailable\n");
			}
			active_ = false;
		}
	}
private:
	struct cpu_times {
		uint64_t process_user = 0;
		uint64_t process_kernel = 0;
		uint64_t thread_user = 0;
		uint64_t thread_kernel = 0;
		bool valid = false;
	};
	static uint64_t filetime_ticks(FILETIME a_value) {
		return (static_cast<uint64_t>(a_value.dwHighDateTime) << 32) | a_value.dwLowDateTime;
	}
	static cpu_times read_cpu_times() {
		FILETIME created{}, exited{}, process_kernel{}, process_user{}, thread_kernel{}, thread_user{};
		cpu_times result;
		result.valid = GetProcessTimes(GetCurrentProcess(), &created, &exited, &process_kernel, &process_user)
			&& GetThreadTimes(GetCurrentThread(), &created, &exited, &thread_kernel, &thread_user);
		if (result.valid) {
			result.process_user = filetime_ticks(process_user);
			result.process_kernel = filetime_ticks(process_kernel);
			result.thread_user = filetime_ticks(thread_user);
			result.thread_kernel = filetime_ticks(thread_kernel);
		}
		return result;
	}
	cpu_times cpu_begin_;
	std::wstring label_;
	REGHANDLE registration_ = 0;
	bool active_ = false;
};
} // namespace gloom
#endif
