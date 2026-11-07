#pragma once
// Every program gates and agents run unattended calls route_faults_to_stderr() first: a fault prints and exits, never a dialog.
// Include it last: it brings in windows.h, which must follow any winsock2.h.

#if defined(_WIN32)
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace jai::foundry {

#if defined(_WIN32)
namespace unattended_detail {

#if defined(_MSC_VER)
// file mode alone prints an _ASSERTE and carries on past it
inline int abort_on_crt_assert(int report_type, char* message, int*) {
    if (report_type != _CRT_ASSERT) {
        return FALSE;
    }
    std::fputs(message, stderr);
    std::abort();
}

inline void abort_on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {
    std::fputs("invalid parameter passed to a C runtime function\n", stderr);
    std::abort();
}

// SIGABRT handling is process-wide where set_terminate is per thread, so a worker's uncaught exception is named too
inline void print_live_exception_on_abort(int) {
    if (auto pending = std::current_exception()) {
        try {
            std::rethrow_exception(pending);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "\nabort while an exception is live: %s\n", e.what());
        } catch (...) {
            std::fputs("\nabort while an exception of a non-standard type is live\n", stderr);
        }
    }
}
#endif

} // namespace unattended_detail
#endif

inline void route_faults_to_stderr() {
#if defined(_WIN32)
    if (IsDebuggerPresent()) {
        return;   // a developer at a debugger wants the break, not a printout
    }
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#if defined(_MSC_VER)
    std::signal(SIGABRT, &unattended_detail::print_live_exception_on_abort);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(&unattended_detail::abort_on_invalid_parameter);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, &unattended_detail::abort_on_crt_assert);
#endif
#endif
}

} // namespace jai::foundry
