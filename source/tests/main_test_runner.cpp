// JaiScript Foundry Test Runner
// Auto-discovers and runs all registered Foundry test suites

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstring>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <cwchar>
#if defined(_MSC_VER)
#pragma comment(lib, "dbghelp.lib")
#endif
#else
#include <unistd.h>
#endif
#include <iostream>
#include <string>
#include <chrono>
#include <sstream>
#include <jaiscript/testing/unattended.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <cstdio>
#include <cstdlib>

namespace jai::foundry {

namespace {
    // a host linked without the interpreter (mv_tests) defaults to the vm
    jai::backend_type g_test_backend = jai::detail::interpreter_compiled() ? jai::backend_type::interpreter : jai::backend_type::vm;
    bool g_test_vm_reference = false;   // --backend=vm-reference: the vm in engine::vm_reference_mode
    std::atomic<const char*> g_running_suite{nullptr};
    std::atomic<const char*> g_running_test{nullptr};
    bool g_progress = false;   // --progress: a line per suite and a dot per test on stdout, so a watchdog sees a slow run move
    const char* g_progress_suite = nullptr;
}

void note_running_test(const char* suite_name, const char* test_name) {
    g_running_suite.store(suite_name, std::memory_order_relaxed);
    g_running_test.store(test_name, std::memory_order_relaxed);
    if (!g_progress || !suite_name || !test_name || std::strcmp(test_name, "(forge_tests)") == 0) {
        return;
    }
    if (suite_name != g_progress_suite) {
        g_progress_suite = suite_name;
        std::fprintf(stdout, "\n%s ", suite_name);
    }
    std::fputc('.', stdout);
    std::fflush(stdout);
}

// the run's own directory becomes the process's temp directory: a test's fixed temp name never meets another run's (shards
// and backends run side by side, and other checkouts share the machine's temp)
void use_run_temp_directory() {
    const std::filesystem::path& run_directory = test_temp_directory();
#if defined(_WIN32)
    for (const wchar_t* name : {L"TMP", L"TEMP"}) {
        _wputenv_s(name, run_directory.c_str());
    }
#else
    setenv("TMPDIR", run_directory.c_str(), 1);
#endif
}

std::shared_ptr<jai::engine> make_default_engine() {
    auto eng = jai::engine::make();
    eng->execution_budget(k_test_budget_seconds);
    return eng;
}

std::shared_ptr<jai::engine> make_engine() {
    auto eng = make_default_engine();
    eng->set_backend(g_test_backend);
    eng->vm_reference_mode(g_test_vm_reference);
    eng->import_recheck_interval(0);   // test engines touch a file and re-import at once: every reach checks (the throttle has its own tests)
    return eng;
}

bool vm_reference_run() { return g_test_vm_reference; }

} // namespace jai::foundry

// Parse filter from command line arguments
// Supports:
//   --gtest_filter=<pattern>  (gtest style)
//   --jaitest_filter=<pattern> (jaitest style, synonym)
//   --filter=<pattern>        (explicit)
//   <pattern>                 (positional)
// Pattern format: suite_filter.test_filter or just pattern for both
// Wildcard * matches anything
struct test_filter_config {
    std::string suite_pattern;
    std::string test_pattern;
    bool has_dot = false;   // true only for an explicit "suite.test" filter

    test_filter_config() : suite_pattern("*"), test_pattern("*") {}

    static test_filter_config parse(const std::string& filter_str) {
        test_filter_config result;
        if (filter_str.empty()) {
            return result;
        }

        // Check for dot separator (suite.test pattern)
        size_t dot_pos = filter_str.find('.');
        if (dot_pos != std::string::npos) {
            result.has_dot = true;
            result.suite_pattern = filter_str.substr(0, dot_pos);
            result.test_pattern = filter_str.substr(dot_pos + 1);
        } else {
            // No dot - ADDITIVE bare pattern: match the suite name OR a test name (handled in the
            // run loop). Both fields hold the bare pattern; the loop decides per suite.
            result.suite_pattern = filter_str;
            result.test_pattern = filter_str;
        }

        // Handle empty parts as wildcards
        if (result.suite_pattern.empty()) result.suite_pattern = "*";
        if (result.test_pattern.empty()) result.test_pattern = "*";

        return result;
    }

    // Simple wildcard matching (* matches any sequence)
    static bool matches(const std::string& pattern, const std::string& text) {
        if (pattern == "*") return true;

        // If no wildcards, do substring match
        if (pattern.find('*') == std::string::npos) {
            return text.find(pattern) != std::string::npos;
        }

        // Simple wildcard matching
        size_t pi = 0, ti = 0;
        size_t star_pi = std::string::npos, star_ti = 0;

        while (ti < text.size()) {
            if (pi < pattern.size() && (pattern[pi] == text[ti] || pattern[pi] == '?')) {
                pi++;
                ti++;
            } else if (pi < pattern.size() && pattern[pi] == '*') {
                star_pi = pi++;
                star_ti = ti;
            } else if (star_pi != std::string::npos) {
                pi = star_pi + 1;
                ti = ++star_ti;
            } else {
                return false;
            }
        }

        while (pi < pattern.size() && pattern[pi] == '*') {
            pi++;
        }

        return pi == pattern.size();
    }

    bool matches_suite(const std::string& suite_name) const {
        return matches(suite_pattern, suite_name);
    }

    bool matches_test(const std::string& test_name) const {
        return matches(test_pattern, test_name);
    }
};

#if defined(_MSC_VER) && !defined(NDEBUG)
static void print_native_backtrace() {
    static std::atomic<bool> printing{false};
    bool expected = false;
    if (!printing.compare_exchange_strong(expected, true)) {
        return;   // racing losers continue; the winner prints and terminates below
    }
    void* frames[62];
    const USHORT n = CaptureStackBackTrace(0, 62, frames, nullptr);
    HANDLE proc = GetCurrentProcess();
    SymInitialize(proc, nullptr, TRUE);
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    fprintf(stderr, "--- abort backtrace (%u frames) ---\n", n);
    for (USHORT i = 0; i < n; ++i) {
        DWORD64 disp = 0;
        if (SymFromAddr(proc, reinterpret_cast<DWORD64>(frames[i]), &disp, sym)) {
            fprintf(stderr, "  %02u %s +0x%llx\n", i, sym->Name, static_cast<unsigned long long>(disp));
        } else {
            fprintf(stderr, "  %02u %p\n", i, frames[i]);
        }
    }
    fflush(stderr);
#ifdef JAISCRIPT_DIAG_XTHREAD_RC
    TerminateProcess(GetCurrentProcess(), 3);   // canary hook owns termination
#endif
}
#endif

// A crash names the running test even without --verbose (which hides the run's streams); the text is built without
// allocating, so a corrupt heap still reports
namespace crash_report {

struct report_text {
    char bytes[1024];
    std::size_t length = 0;

    void append(const char* text) {
        for (; text && *text && length < sizeof(bytes); ++text) {
            bytes[length++] = *text;
        }
    }

    void append_number(unsigned long long value, unsigned base) {
        char digits[24];
        int count = 0;
        do {
            digits[count++] = "0123456789ABCDEF"[value % base];
            value /= base;
        } while (value != 0);
        while (count > 0 && length < sizeof(bytes)) {
            bytes[length++] = digits[--count];
        }
    }

    void append_running_test() {
        const char* suite_name = jai::foundry::g_running_suite.load(std::memory_order_relaxed);
        const char* test_name = jai::foundry::g_running_test.load(std::memory_order_relaxed);
        if (!suite_name) {
            append("no test (outside the suites)");
            return;
        }
        append(suite_name);
        append(" :: ");
        append(test_name);
    }
};

#if defined(_WIN32)
constexpr DWORD k_msvc_cxx_exception_code = 0xE06D7363;

wchar_t g_dump_directory[MAX_PATH];
wchar_t g_dump_stem[MAX_PATH];
const char* g_dump_skipped_reason = nullptr;
report_text g_crash_line;   // static storage: an overflowed stack has room for the write and little else
std::atomic<bool> g_dump_thread_started{false};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_unhandled_exception_filter = nullptr;
void (*g_previous_abort_handler)(int) = SIG_DFL;

struct crash_context {
    DWORD code;
    EXCEPTION_POINTERS* exception;
    DWORD thread_id;
};

void write_to_stderr(const char* bytes, std::size_t length) {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), bytes, static_cast<DWORD>(length), &written, nullptr);
}

void write_to_stderr(const report_text& text) {
    write_to_stderr(text.bytes, text.length);
}

void create_directory_and_parents(const wchar_t* directory) {
    wchar_t prefix[MAX_PATH];
    std::size_t length = 0;
    for (; directory[length] != 0 && length + 1 < MAX_PATH; ++length) {
        if ((directory[length] == L'\\' || directory[length] == L'/') && length > 0) {
            prefix[length] = 0;
            CreateDirectoryW(prefix, nullptr);
        }
        prefix[length] = directory[length];
    }
    prefix[length] = 0;
    CreateDirectoryW(prefix, nullptr);
}

void report_dump_outcome(const char* outcome, const wchar_t* path, const char* failed_call, DWORD error) {
    report_text text;
    text.append(outcome);
    char path_text[MAX_PATH * 3];
    if (WideCharToMultiByte(CP_UTF8, 0, path, -1, path_text, sizeof(path_text), nullptr, nullptr) > 0) {
        text.append(path_text);
    }
    if (failed_call) {
        text.append(" (");
        text.append(failed_call);
        text.append(" error 0x");
        text.append_number(error, 16);
        text.append(")");
    }
    text.append("\n");
    write_to_stderr(text);
}

DWORD WINAPI write_minidump(void* context_pointer) {
    g_dump_thread_started.store(true);
    const auto& context = *static_cast<crash_context*>(context_pointer);
    wchar_t path[MAX_PATH];
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; file == INVALID_HANDLE_VALUE && attempt < 100; ++attempt) {
        const unsigned long process_id = GetCurrentProcessId();
        const int length = attempt == 0
            ? std::swprintf(path, MAX_PATH, L"%ls\\%ls_%lu.dmp", g_dump_directory, g_dump_stem, process_id)
            : std::swprintf(path, MAX_PATH, L"%ls\\%ls_%lu_%u.dmp", g_dump_directory, g_dump_stem, process_id, attempt);
        if (length < 0) {
            const char reason[] = "*** minidump skipped: the dump path in JAI_CRASH_DUMP_DIR would exceed MAX_PATH\n";
            write_to_stderr(reason, sizeof(reason) - 1);
            return 0;
        }
        if (attempt == 0) {
            create_directory_and_parents(g_dump_directory);
        }
        file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD create_error = file == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        if (file == INVALID_HANDLE_VALUE && create_error != ERROR_FILE_EXISTS) {
            report_dump_outcome("*** minidump failed: ", path, "CreateFileW", create_error);
            return 0;
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        report_dump_outcome("*** minidump failed: ", path, "CreateFileW", ERROR_FILE_EXISTS);
        return 0;
    }
    MINIDUMP_EXCEPTION_INFORMATION exception_information{context.thread_id, context.exception, FALSE};
    const bool dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
        static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
        context.exception ? &exception_information : nullptr, nullptr, nullptr);
    const DWORD dump_error = dumped ? 0 : GetLastError();
    CloseHandle(file);
    report_dump_outcome(dumped ? "*** minidump written: " : "*** minidump failed: ", path, dumped ? nullptr : "MiniDumpWriteDump",
        dump_error);
    return 0;
}

void write_dump_on_fresh_thread(crash_context& context) {
    HANDLE dumper = CreateThread(nullptr, 0, &write_minidump, &context, 0, nullptr);
    if (!dumper) {
        write_minidump(&context);
        return;
    }
    // a new thread cannot start while the crashing one holds the loader or heap lock
    const ULONGLONG start_deadline = GetTickCount64() + 3000;
    while (!g_dump_thread_started.load() && GetTickCount64() < start_deadline) {
        Sleep(10);
    }
    if (g_dump_thread_started.load()) {
        WaitForSingleObject(dumper, 60000);
    } else {
        const char reason[] = "*** minidump skipped: its thread could not start (the crash holds the loader or heap lock)\n";
        write_to_stderr(reason, sizeof(reason) - 1);
    }
    CloseHandle(dumper);
}

void report_crash(DWORD code, EXCEPTION_POINTERS* exception) {
    static std::atomic<bool> reported{false};
    if (reported.exchange(true)) {
        Sleep(INFINITE);   // the first crash reports and ends the process
    }
    // the line goes out on the crashing thread: no allocation, no lock, almost no stack
    g_crash_line.append("\n*** crash 0x");
    g_crash_line.append_number(code, 16);
    g_crash_line.append(" while running ");
    g_crash_line.append_running_test();
    g_crash_line.append("\n");
    write_to_stderr(g_crash_line);
    if (g_dump_skipped_reason) {
        const char prefix[] = "*** minidump skipped: ";
        write_to_stderr(prefix, sizeof(prefix) - 1);
        write_to_stderr(g_dump_skipped_reason, std::strlen(g_dump_skipped_reason));
    } else if (g_dump_directory[0] != 0) {
        crash_context context{code, exception, GetCurrentThreadId()};
        write_dump_on_fresh_thread(context);   // a fresh thread has the stack an overflowed one lacks
    }
}

LONG WINAPI report_unhandled_exception(EXCEPTION_POINTERS* exception) {
    const DWORD code = exception->ExceptionRecord->ExceptionCode;
    if (code == k_msvc_cxx_exception_code && g_previous_unhandled_exception_filter) {
        g_previous_unhandled_exception_filter(exception);   // the CRT's filter sends an escaped C++ exception to std::terminate
    }
    report_crash(code, exception);
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

void report_abort(int signal_number) {
    if (g_previous_abort_handler != SIG_DFL && g_previous_abort_handler != SIG_IGN && g_previous_abort_handler != SIG_ERR) {
        g_previous_abort_handler(signal_number);
    }
#if defined(_MSC_VER) && !defined(NDEBUG)
    print_native_backtrace();
#endif
    report_crash(3, nullptr);   // the CRT exits with 3 once the handler returns
}

void install() {
    const DWORD directory_length = GetEnvironmentVariableW(L"JAI_CRASH_DUMP_DIR", g_dump_directory, MAX_PATH);
    if (directory_length >= MAX_PATH) {
        g_dump_directory[0] = 0;
        g_dump_skipped_reason = "JAI_CRASH_DUMP_DIR is longer than MAX_PATH\n";
    } else if (directory_length > 0) {
        DWORD length = directory_length;
        while (length > 1 && (g_dump_directory[length - 1] == L'\\' || g_dump_directory[length - 1] == L'/')) {
            g_dump_directory[--length] = 0;
        }
        wchar_t executable[MAX_PATH];
        const DWORD executable_length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
        const wchar_t* stem = L"crash";
        if (executable_length > 0 && executable_length < MAX_PATH) {
            wchar_t* name = executable;
            if (wchar_t* separator = std::wcsrchr(executable, L'\\')) {
                name = separator + 1;
            }
            if (wchar_t* extension = std::wcsrchr(name, L'.')) {
                *extension = 0;
            }
            if (name[0] != 0) {
                stem = name;
            }
        }
        lstrcpynW(g_dump_stem, stem, MAX_PATH);
    }
    g_previous_unhandled_exception_filter = SetUnhandledExceptionFilter(&report_unhandled_exception);
    g_previous_abort_handler = signal(SIGABRT, &report_abort);
}
#else
alignas(16) char g_alternate_stack[1 << 16];

void report_crash_signal(int signal_number) {
    report_text text;
    text.append("\n*** crash signal ");
    text.append_number(static_cast<unsigned long long>(signal_number), 10);
    text.append(" while running ");
    text.append_running_test();
    text.append("\n");
    const ssize_t written = write(STDERR_FILENO, text.bytes, text.length);
    (void)written;
    raise(signal_number);   // SA_RESETHAND restored the default action: the process dies of this signal
}

void install() {
    stack_t alternate_stack{};
    alternate_stack.ss_sp = g_alternate_stack;
    alternate_stack.ss_size = sizeof(g_alternate_stack);
    sigaltstack(&alternate_stack, nullptr);   // a main-thread stack overflow still reports
    struct sigaction action{};
    action.sa_handler = &report_crash_signal;
    action.sa_flags = SA_RESETHAND | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int signal_number : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
        sigaction(signal_number, &action, nullptr);
    }
}
#endif

} // namespace crash_report

int main(int argc, char** argv) {
    using namespace jai::foundry;

    route_faults_to_stderr();
    crash_report::install();
#if defined(_MSC_VER) && !defined(NDEBUG)
    // An access violation names its call path too (the first one prints, the process then
    // dies as it would have)
    AddVectoredExceptionHandler(1, [](PEXCEPTION_POINTERS info) -> LONG {
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) { print_native_backtrace(); }
        return EXCEPTION_CONTINUE_SEARCH;
    });
#ifdef JAISCRIPT_DIAG_XTHREAD_RC
    // Canary builds report directly at the racing touch (signals lose concurrent aborts)
    jai::detail::diag_xthread_report = &print_native_backtrace;
#endif
#endif

    // Parse command line flags
    // FOUNDRY_VERBOSE can be set at compile time (e.g., for benchmark builds)
#ifdef FOUNDRY_VERBOSE
    bool verbose = true;
#else
    bool verbose = false;
#endif
    test_filter_config filter;
    int shard_index = 0;
    int shard_count = 1;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        // Verbose flag
        if (arg == "--verbose" || arg == "-v") {
            verbose = true;
            continue;
        }
        if (arg == "--progress") {
            g_progress = true;
            continue;
        }

        // gtest style: --gtest_filter=pattern
        if (arg.rfind("--gtest_filter=", 0) == 0) {
            filter = test_filter_config::parse(arg.substr(15));
        }
        // jaitest style (synonym): --jaitest_filter=pattern
        else if (arg.rfind("--jaitest_filter=", 0) == 0) {
            filter = test_filter_config::parse(arg.substr(17));
        }
        // explicit: --filter=pattern
        else if (arg.rfind("--filter=", 0) == 0) {
            filter = test_filter_config::parse(arg.substr(9));
        }
        // backend selection: --backend=interpreter|vm|vm-reference
        else if (arg.rfind("--backend=", 0) == 0) {
            std::string backend_name = arg.substr(10);
            if (backend_name == "interpreter") {
                g_test_backend = jai::backend_type::interpreter;
            } else if (backend_name == "vm") {
                g_test_backend = jai::backend_type::vm;
            } else if (backend_name == "vm-reference") {
                g_test_backend = jai::backend_type::vm;
                g_test_vm_reference = true;
            } else {
                std::cout << "Unknown backend: \"" << backend_name << "\" (expected interpreter, vm or vm-reference)\n";
                return 1;
            }
        }
        // --shard=i/n: runs every n-th suite starting at suite i (0-based), so n processes cover the suites once
        else if (arg.rfind("--shard=", 0) == 0) {
            auto slash = arg.find('/', 8);
            try {
                shard_index = slash == std::string::npos ? -1 : std::stoi(arg.substr(8, slash - 8));
                shard_count = slash == std::string::npos ? 0 : std::stoi(arg.substr(slash + 1));
            } catch (...) {
                shard_count = 0;
            }
            if (shard_count < 1 || shard_index < 0 || shard_index >= shard_count) {
                std::cout << "Bad shard: \"" << arg.substr(8) << "\" (expected i/n with 0 <= i < n)\n";
                return 1;
            }
        }
        // positional argument (filter)
        else if (arg[0] != '-') {
            filter = test_filter_config::parse(arg);
        }
    }

    if (g_test_backend == jai::backend_type::interpreter) {
        try {
            jai::engine::make()->set_backend(g_test_backend);
        } catch (const std::exception& e) {
            std::cout << e.what() << "\n";
            return 1;
        }
    }

    use_run_temp_directory();
    g_progress = g_progress && !verbose;

    // Run all test suites
    auto suites = test_registry::instance().create_all_suites();

    std::cout << "/*----------------------------*\\\n";
    std::cout << "| JaiScript Foundry Test Suite |\n";
    std::cout << "\\*----------------------------*/\n";
    std::cout << "Backend: " << (g_test_vm_reference ? "vm-reference" : g_test_backend == jai::backend_type::vm ? "vm" : "interpreter") << "\n";

    if (!verbose) {
        std::cout << "Performing Tests, One Moment ..." << std::flush;
    }

    auto start_time = std::chrono::steady_clock::now();

    int total_failures = 0;
    int total_passed = 0;
    int suites_run = 0;
    std::vector<failure_info> all_failures;

    // Redirect stdout/stderr when not verbose to suppress test output
    std::streambuf* original_cout = nullptr;
    std::streambuf* original_cerr = nullptr;
    std::ostringstream null_stream;
    if (!verbose) {
        original_cout = std::cout.rdbuf(null_stream.rdbuf());
        original_cerr = std::cerr.rdbuf(null_stream.rdbuf());
    }

    for (size_t suite_position = 0; suite_position < suites.size(); ++suite_position) {
        auto& suite = suites[suite_position];
        if (static_cast<int>(suite_position % shard_count) != shard_index) {
            continue;
        }
        bool suite_matches = filter.matches_suite(suite->get_name());

        std::string test_pattern_for_suite;
        if (filter.has_dot) {
            // Explicit "suite.test": the suite must match, then filter tests within it.
            if (!suite_matches) {
                continue;
            }
            test_pattern_for_suite = filter.test_pattern;
        } else {
            // Bare pattern is ADDITIVE: if it names this suite, run ALL of the suite's tests;
            // otherwise fall back to matching individual test names (so e.g. a bare test-name
            // filter still selects matching tests across every suite). This never SUBTRACTS the
            // tests of a matched suite.
            test_pattern_for_suite = suite_matches ? std::string("*") : filter.test_pattern;
        }

        auto result = suite->quench(test_pattern_for_suite, verbose);
        if (result.total() > 0) {
            suites_run++;
        }
        total_failures += result.failed;
        total_passed += result.passed;

        // Collect failures
        for (const auto& f : result.failures) {
            all_failures.push_back(f);
        }
    }

    // Restore stdout/stderr
    if (original_cout) {
        std::cout.rdbuf(original_cout);
        std::cerr.rdbuf(original_cerr);
    }
    if (jai::foundry::g_progress) {
        std::cout << "\n";
    }

    // Summary
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    double seconds = duration.count() / 1000.0;

    int total_tests = total_passed + total_failures;

    // Show failures (always, if any)
    if (!all_failures.empty()) {
        std::cout << "\nFailed tests:\n";
        for (const auto& f : all_failures) {
            std::cout << "  x " << f.suite_name << " :: " << f.test_name << "\n";
            std::cout << "      " << f.error_message << "\n";
        }
    }

    std::cout << "\n-----------------------------------------\n";
    if (total_tests == 0 && filter.suite_pattern != "*") {
        std::cout << "No tests matched filter: \"" << filter.suite_pattern
                  << (filter.has_dot ? ("." + filter.test_pattern) : std::string()) << "\"\n\n";
        std::cout << "Available suites:\n";
        for (const auto& suite : suites) {
            std::cout << "  - " << suite->get_name() << "\n";
        }
        std::cout << "-----------------------------------------\n";
        return 1;
    } else if (total_failures == 0) {
        std::cout << "<3 :D All tests passed! (" << suites_run << " suites, " << total_tests << " tests, " << seconds << "s)\n";
    } else {
        std::cout << "x " << total_failures << " of " << total_tests << " test(s) failed (" << seconds << "s)\n";
    }
    std::cout << "-----------------------------------------\n";

    return total_failures > 0 ? 1 : 0;
}
