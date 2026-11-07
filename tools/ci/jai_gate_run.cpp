// jai_gate_run: runs one gate program under a watchdog (tools/ci/ci_lib.sh runs every gate under it).
//
//   jai_gate_run [--idle=<seconds>] [--timeout=<seconds>] [--dump-dir=<dir>] -- <program> <args...>
//
// The program's stdout and stderr pass through the watchdog unchanged (through one pipe when both name the same file, so
// their relative order holds too). The exit code is the program's, or 124 when it printed nothing for --idle seconds or
// ran past --timeout seconds (0, the default of each: no limit). On Windows the program and every process it starts run in
// a job object: at a limit each one is written as a minidump to --dump-dir and the job is killed, processes left running
// after the program exits are killed, and a watchdog that dies takes the job with it. Elsewhere they run in a process
// group: at a limit it gets SIGTERM and 10 s later SIGKILL, and processes left running after the program exits are killed.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <atomic>
#include <thread>
#include <windows.h>
#include <dbghelp.h>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr int timed_out_exit = 124;
constexpr int check_period_ms = 250;
constexpr long long drain_grace_ms = 10000;

struct gate_limits {
	unsigned long idle_seconds = 0;
	unsigned long timeout_seconds = 0;
};

template<class String>
struct gate_run_options {
	gate_limits limits;
	String dump_dir;
	std::vector<String> command;
};

template<class String>
String string_of(const char* a_text) {
	return String(a_text, a_text + std::char_traits<char>::length(a_text));
}

template<class String>
bool parse_seconds(const String& a_digits, unsigned long& a_seconds) {
	if (a_digits.empty() || a_digits.size() > 9 || a_digits.find_first_not_of(string_of<String>("0123456789")) != String::npos) {
		return false;
	}
	a_seconds = std::stoul(a_digits);
	return true;
}

template<class String>
bool parse_gate_run_options(const std::vector<String>& a_args, gate_run_options<String>& a_options) {
	const String idle_flag = string_of<String>("--idle=");
	const String timeout_flag = string_of<String>("--timeout=");
	const String dump_dir_flag = string_of<String>("--dump-dir=");
	const String separator = string_of<String>("--");
	size_t index = 1;
	for (; index < a_args.size() && a_args[index] != separator; ++index) {
		const String& arg = a_args[index];
		if (arg.compare(0, idle_flag.size(), idle_flag) == 0) {
			if (!parse_seconds(arg.substr(idle_flag.size()), a_options.limits.idle_seconds)) {
				return false;
			}
		} else if (arg.compare(0, timeout_flag.size(), timeout_flag) == 0) {
			if (!parse_seconds(arg.substr(timeout_flag.size()), a_options.limits.timeout_seconds)) {
				return false;
			}
		} else if (arg.compare(0, dump_dir_flag.size(), dump_dir_flag) == 0) {
			a_options.dump_dir = arg.substr(dump_dir_flag.size());
		} else {
			return false;
		}
	}
	if (index + 1 >= a_args.size()) {
		return false;
	}
	a_options.command.assign(a_args.begin() + static_cast<std::ptrdiff_t>(index) + 1, a_args.end());
	return true;
}

int usage() {
	std::fputs("usage: jai_gate_run [--idle=<seconds>] [--timeout=<seconds>] [--dump-dir=<dir>] -- <program> <args...>\n", stderr);
	return 2;
}

long long steady_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// the limit a run has passed, as the end of the watchdog's message, or empty
std::string limit_passed(const gate_limits& a_limits, long long a_started_ms, long long a_last_output_ms) {
	std::string result;
	long long now = steady_ms();
	if (a_limits.timeout_seconds > 0 && now - a_started_ms >= static_cast<long long>(a_limits.timeout_seconds) * 1000) {
		result = "exceeded " + std::to_string(a_limits.timeout_seconds) + " s";
	} else if (a_limits.idle_seconds > 0 && now - a_last_output_ms >= static_cast<long long>(a_limits.idle_seconds) * 1000) {
		result = "printed nothing for " + std::to_string(a_limits.idle_seconds) + " s";
	}
	return result;
}

void report_limit(std::string a_command, const std::string& a_passed) {
	if (a_command.size() > 200) {
		a_command = a_command.substr(0, 200) + "...";
	}
	std::fprintf(stderr, "watchdog: %s %s\n", a_command.c_str(), a_passed.c_str());
}

#ifdef _WIN32

std::string utf8_of(const std::wstring& a_text) {
	std::string result;
	int size = WideCharToMultiByte(CP_UTF8, 0, a_text.c_str(), static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
	if (size > 0) {
		result.resize(static_cast<size_t>(size));
		WideCharToMultiByte(CP_UTF8, 0, a_text.c_str(), static_cast<int>(a_text.size()), result.data(), size, nullptr, nullptr);
	}
	return result;
}

// an MSYS child (bash) re-reads its command line with shell rules: outside quotes only these characters mean themselves,
// and inside quotes it halves every doubled backslash, so a plain word goes bare and anything else goes quoted
bool plain_argument(const std::wstring& a_arg) {
	return !a_arg.empty() && a_arg.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-./\\:,=+%") ==
		std::wstring::npos;
}

// the quoting CommandLineToArgvW and the CRT undo: backslashes double only where a quote follows them
void append_quoted_argument(std::wstring& a_line, const std::wstring& a_arg) {
	if (!a_line.empty()) {
		a_line += L' ';
	}
	if (plain_argument(a_arg)) {
		a_line += a_arg;
		return;
	}
	a_line += L'"';
	for (auto it = a_arg.begin();; ++it) {
		size_t backslashes = 0;
		while (it != a_arg.end() && *it == L'\\') {
			++it;
			++backslashes;
		}
		if (it == a_arg.end()) {
			a_line.append(backslashes * 2, L'\\');
			break;
		}
		if (*it == L'"') {
			a_line.append(backslashes * 2 + 1, L'\\');
		} else {
			a_line.append(backslashes, L'\\');
		}
		a_line += *it;
	}
	a_line += L'"';
}

std::vector<DWORD> job_process_ids(HANDLE a_job) {
	std::vector<DWORD> result;
	std::vector<unsigned char> buffer(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 1024 * sizeof(ULONG_PTR));
	auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
	if (QueryInformationJobObject(a_job, JobObjectBasicProcessIdList, list, static_cast<DWORD>(buffer.size()), nullptr) ||
		GetLastError() == ERROR_MORE_DATA) {
		for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) {
			result.push_back(static_cast<DWORD>(list->ProcessIdList[i]));
		}
	}
	return result;
}

DWORD job_active_processes(HANDLE a_job) {
	JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting {};
	if (!QueryInformationJobObject(a_job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
		return 0;
	}
	return accounting.ActiveProcesses;
}

void dump_job_processes(HANDLE a_job, const std::wstring& a_dump_dir) {
	std::error_code created;
	std::filesystem::create_directories(a_dump_dir, created);
	if (created) {
		std::fprintf(stderr, "watchdog: no minidumps: cannot create %s (%s)\n", utf8_of(a_dump_dir).c_str(), created.message().c_str());
		return;
	}
	std::wstring written_names;
	for (DWORD pid : job_process_ids(a_job)) {
		HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
		if (!process) {
			continue;
		}
		std::wstring image(MAX_PATH, L'\0');
		DWORD image_size = MAX_PATH;
		std::wstring stem = L"process";
		if (QueryFullProcessImageNameW(process, 0, image.data(), &image_size)) {
			image.resize(image_size);
			stem = image.substr(image.find_last_of(L"\\/") + 1);
			stem = stem.substr(0, stem.rfind(L'.'));
		}
		std::wstring name = stem + L"_" + std::to_wstring(pid) + L".dmp";
		std::wstring path = a_dump_dir + L"\\" + name;
		HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			std::fprintf(stderr, "watchdog: no minidump of %lu: cannot create %s (%lu)\n", pid, utf8_of(path).c_str(), GetLastError());
		} else {
			auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);
			bool written = MiniDumpWriteDump(process, pid, file, type, nullptr, nullptr, nullptr) != FALSE;
			DWORD dump_error = written ? 0 : GetLastError();
			CloseHandle(file);
			if (written) {
				written_names += L" " + name;
			} else {
				DeleteFileW(path.c_str());
				std::fprintf(stderr, "watchdog: no minidump of %lu: MiniDumpWriteDump failed (0x%lX)\n", pid, dump_error);
			}
		}
		CloseHandle(process);
	}
	if (!written_names.empty()) {
		std::fprintf(stderr, "watchdog: minidumps in %s:%s\n", utf8_of(a_dump_dir).c_str(), utf8_of(written_names).c_str());
	}
}

// a killed process holds its image file until it is gone, and the next build's link needs that file
void kill_job(HANDLE a_job) {
	TerminateJobObject(a_job, timed_out_exit);
	for (int waited = 0; waited < 200 && job_active_processes(a_job) > 0; ++waited) {
		Sleep(50);
	}
}

// the caller (under MSYS, the stub process bash waits on) dying means nobody reads the verdict: the job must not outlive it
HANDLE open_parent_process() {
	HANDLE result = nullptr;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return result;
	}
	PROCESSENTRY32W entry {};
	entry.dwSize = sizeof(entry);
	DWORD self = GetCurrentProcessId();
	for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
		if (entry.th32ProcessID == self) {
			result = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ParentProcessID);
			break;
		}
	}
	CloseHandle(snapshot);
	FILETIME parent_created {}, self_created {}, unused {};
	if (result && (!GetProcessTimes(result, &parent_created, &unused, &unused, &unused) ||
		!GetProcessTimes(GetCurrentProcess(), &self_created, &unused, &unused, &unused) || CompareFileTime(&parent_created, &self_created) > 0)) {
		CloseHandle(result);
		result = nullptr;
	}
	return result;
}

bool same_disk_file(HANDLE a_first, HANDLE a_second) {
	BY_HANDLE_FILE_INFORMATION first {}, second {};
	return a_first == a_second || (GetFileType(a_first) == FILE_TYPE_DISK && GetFileType(a_second) == FILE_TYPE_DISK &&
		GetFileInformationByHandle(a_first, &first) && GetFileInformationByHandle(a_second, &second) &&
		first.dwVolumeSerialNumber == second.dwVolumeSerialNumber && first.nFileIndexHigh == second.nFileIndexHigh &&
		first.nFileIndexLow == second.nFileIndexLow);
}

void forward_output(HANDLE a_read, HANDLE a_destination, std::atomic<long long>* a_last_output_ms) {
	std::vector<char> buffer(64 * 1024);
	DWORD got = 0;
	while (ReadFile(a_read, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
		if (got > 0) {
			a_last_output_ms->store(steady_ms());
		}
		DWORD wrote = 0;
		for (DWORD sent = 0; sent < got; sent += wrote) {
			if (!WriteFile(a_destination, buffer.data() + sent, got - sent, &wrote, nullptr) || wrote == 0) {
				break;
			}
		}
	}
	CloseHandle(a_read);
}

// every process of the job is gone by now, so a write end still open is held from outside the job: stop waiting on it
void finish_forwarding(std::vector<std::thread>& a_forwarders) {
	long long give_up_ms = steady_ms() + drain_grace_ms;
	bool gave_up = false;
	for (std::thread& forwarder : a_forwarders) {
		HANDLE thread = reinterpret_cast<HANDLE>(forwarder.native_handle());
		while (WaitForSingleObject(thread, check_period_ms) == WAIT_TIMEOUT) {
			if (steady_ms() >= give_up_ms) {
				gave_up = true;
				CancelSynchronousIo(thread);
			}
		}
		forwarder.join();
	}
	if (gave_up) {
		std::fputs("watchdog: the output stayed open after every process of the job ended; stopped reading it\n", stderr);
	}
}

int run_gate(const gate_run_options<std::wstring>& a_options) {
	HANDLE parent = open_parent_process();
	HANDLE job = CreateJobObjectW(nullptr, nullptr);
	if (!job) {
		std::fprintf(stderr, "watchdog: CreateJobObject failed (%lu)\n", GetLastError());
		return 125;
	}
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
	// DIE_ON_UNHANDLED_EXCEPTION: a crash exits at once instead of waiting on an error-report dialog
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
	if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
		std::fprintf(stderr, "watchdog: SetInformationJobObject failed (%lu)\n", GetLastError());
		return 125;
	}

	std::wstring line;
	for (const std::wstring& arg : a_options.command) {
		append_quoted_argument(line, arg);
	}
	HANDLE destinations[] {GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
	size_t streams = same_disk_file(destinations[0], destinations[1]) ? 1 : 2;
	HANDLE reads[2] {}, writes[2] {};
	for (size_t stream = 0; stream < streams; ++stream) {
		SECURITY_ATTRIBUTES inherited {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
		if (!CreatePipe(&reads[stream], &writes[stream], &inherited, 64 * 1024)) {
			std::fprintf(stderr, "watchdog: CreatePipe failed (%lu)\n", GetLastError());
			return 125;
		}
		SetHandleInformation(reads[stream], HANDLE_FLAG_INHERIT, 0);
	}
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	if (startup.hStdInput && startup.hStdInput != INVALID_HANDLE_VALUE) {
		SetHandleInformation(startup.hStdInput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	}
	startup.hStdOutput = writes[0];
	startup.hStdError = writes[streams - 1];
	PROCESS_INFORMATION child {};
	if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &child)) {
		std::fprintf(stderr, "watchdog: cannot start %s (%lu)\n", utf8_of(a_options.command[0]).c_str(), GetLastError());
		return 127;
	}
	// a reader sees the end of its pipe only once every holder of the write end has closed it, the watchdog included
	for (size_t stream = 0; stream < streams; ++stream) {
		CloseHandle(writes[stream]);
	}
	if (!AssignProcessToJobObject(job, child.hProcess)) {
		std::fprintf(stderr, "watchdog: AssignProcessToJobObject failed (%lu)\n", GetLastError());
		TerminateProcess(child.hProcess, 125);
		return 125;
	}
	long long started_ms = steady_ms();
	std::atomic<long long> last_output_ms {started_ms};
	std::vector<std::thread> forwarders;
	for (size_t stream = 0; stream < streams; ++stream) {
		forwarders.emplace_back(forward_output, reads[stream], destinations[stream], &last_output_ms);
	}
	ResumeThread(child.hThread);
	CloseHandle(child.hThread);

	HANDLE waited[] {child.hProcess, parent};
	for (;;) {
		DWORD woke = WaitForMultipleObjects(parent ? 2 : 1, waited, FALSE, check_period_ms);
		if (woke == WAIT_OBJECT_0) {
			break;
		}
		if (woke == WAIT_OBJECT_0 + 1 || woke == WAIT_FAILED) {
			std::fprintf(stderr, "watchdog: the caller of %s exited; killed it\n", utf8_of(a_options.command[0]).c_str());
			kill_job(job);
			return 125;
		}
		if (std::string passed = limit_passed(a_options.limits, started_ms, last_output_ms.load()); !passed.empty()) {
			report_limit(utf8_of(line), passed);
			if (!a_options.dump_dir.empty()) {
				dump_job_processes(job, a_options.dump_dir);
			}
			kill_job(job);
			finish_forwarding(forwarders);
			return timed_out_exit;
		}
	}
	DWORD result = 0;
	GetExitCodeProcess(child.hProcess, &result);
	CloseHandle(child.hProcess);
	DWORD left = job_active_processes(job);
	if (left > 0) {
		kill_job(job);
	}
	finish_forwarding(forwarders);
	if (left > 0) {
		std::fprintf(stderr, "watchdog: %s exited and left %lu process(es) running; killed\n", utf8_of(a_options.command[0]).c_str(), left);
	}
	return static_cast<int>(result);
}

#else

volatile std::sig_atomic_t caught_signal = 0;

void note_signal(int a_signal) {
	caught_signal = a_signal;
}

void write_all(int a_fd, const char* a_data, size_t a_size) {
	while (a_size > 0) {
		ssize_t wrote = write(a_fd, a_data, a_size);
		if (wrote < 0 && errno == EINTR) {
			continue;
		}
		if (wrote <= 0) {
			return;
		}
		a_data += wrote;
		a_size -= static_cast<size_t>(wrote);
	}
}

int run_gate(gate_run_options<std::string>& a_options) {
	struct stat out_info {}, err_info {};
	size_t streams = fstat(1, &out_info) == 0 && fstat(2, &err_info) == 0 && out_info.st_dev == err_info.st_dev &&
		out_info.st_ino == err_info.st_ino ? 1 : 2;
	int reads[2] {-1, -1}, writes[2] {-1, -1};
	for (size_t stream = 0; stream < streams; ++stream) {
		int ends[2];
		if (pipe(ends) != 0) {
			std::perror("watchdog: pipe");
			return 125;
		}
		fcntl(ends[0], F_SETFD, FD_CLOEXEC);
		reads[stream] = ends[0];
		writes[stream] = ends[1];
	}
	std::vector<char*> plain;
	std::string shown;
	for (std::string& arg : a_options.command) {
		plain.push_back(arg.data());
		shown += (shown.empty() ? "" : " ") + arg;
	}
	plain.push_back(nullptr);

	struct sigaction forwarded {};
	forwarded.sa_handler = note_signal;
	sigemptyset(&forwarded.sa_mask);
	for (int signal_number : {SIGINT, SIGTERM, SIGHUP}) {
		sigaction(signal_number, &forwarded, nullptr);
	}
	std::signal(SIGPIPE, SIG_IGN);
	pid_t caller = getppid();
	pid_t child = fork();
	if (child < 0) {
		std::perror("watchdog: fork");
		return 125;
	}
	if (child == 0) {
		setpgid(0, 0);
		std::signal(SIGPIPE, SIG_DFL);
		dup2(writes[0], 1);
		dup2(writes[streams - 1], 2);
		for (size_t stream = 0; stream < streams; ++stream) {
			if (writes[stream] > 2) {
				close(writes[stream]);
			}
		}
		execvp(plain[0], plain.data());
		std::perror(plain[0]);
		_exit(127);
	}
	// both sides make the group, so it exists before either one signals it
	setpgid(child, child);
	for (size_t stream = 0; stream < streams; ++stream) {
		close(writes[stream]);
	}

	long long started_ms = steady_ms();
	long long last_output_ms = started_ms;
	long long kill_at_ms = 0;
	long long drain_until_ms = 0;
	bool reaped = false;
	bool left_running = false;
	int status = 0;
	std::string passed;
	std::vector<char> buffer(64 * 1024);
	for (;;) {
		pollfd watched[2] {};
		size_t watched_streams[2] {};
		nfds_t count = 0;
		for (size_t stream = 0; stream < streams; ++stream) {
			if (reads[stream] >= 0) {
				watched[count].fd = reads[stream];
				watched[count].events = POLLIN;
				watched_streams[count++] = stream;
			}
		}
		if (reaped && (count == 0 || steady_ms() >= drain_until_ms)) {
			break;
		}
		if (poll(watched, count, check_period_ms) > 0) {
			for (nfds_t i = 0; i < count; ++i) {
				if (watched[i].revents == 0) {
					continue;
				}
				size_t stream = watched_streams[i];
				ssize_t got = read(reads[stream], buffer.data(), buffer.size());
				if (got > 0) {
					last_output_ms = steady_ms();
					write_all(stream == 0 ? 1 : 2, buffer.data(), static_cast<size_t>(got));
				} else if (got == 0 || errno != EINTR) {
					close(reads[stream]);
					reads[stream] = -1;
				}
			}
		}
		if (reaped) {
			continue;
		}
		if (caught_signal != 0) {
			kill(-child, caught_signal);
			caught_signal = 0;
		}
		if (waitpid(child, &status, WNOHANG) == child) {
			reaped = true;
			drain_until_ms = steady_ms() + drain_grace_ms;
			if (kill(-child, 0) == 0) {
				left_running = true;
				kill(-child, SIGKILL);
			}
			continue;
		}
		if (getppid() != caller) {
			std::fprintf(stderr, "watchdog: the caller of %s exited; killed it\n", plain[0]);
			kill(-child, SIGKILL);
			return 125;
		}
		if (passed.empty()) {
			passed = limit_passed(a_options.limits, started_ms, last_output_ms);
			if (!passed.empty()) {
				report_limit(shown, passed);
				kill(-child, SIGTERM);
				kill_at_ms = steady_ms() + drain_grace_ms;
			}
		} else if (kill_at_ms != 0 && steady_ms() >= kill_at_ms) {
			kill(-child, SIGKILL);
			kill_at_ms = 0;
		}
	}
	bool still_open = false;
	for (size_t stream = 0; stream < streams; ++stream) {
		if (reads[stream] >= 0) {
			still_open = true;
			close(reads[stream]);
		}
	}
	if (still_open) {
		std::fputs("watchdog: the output stayed open after every process of the group ended; stopped reading it\n", stderr);
	}
	if (!passed.empty()) {
		return timed_out_exit;
	}
	if (left_running) {
		std::fprintf(stderr, "watchdog: %s exited and left processes running; killed\n", plain[0]);
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

#endif

}

int main(int argc, char** argv) {
#ifdef _WIN32
	(void)argc;
	(void)argv;
	_setmode(_fileno(stderr), _O_BINARY);   // LF lines, like the logs they land in
	int count = 0;
	wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
	std::vector<std::wstring> args(wide, wide + count);
	LocalFree(wide);
	gate_run_options<std::wstring> options;
#else
	std::vector<std::string> args(argv, argv + argc);
	gate_run_options<std::string> options;
#endif
	if (!parse_gate_run_options(args, options)) {
		return usage();
	}
	return run_gate(options);
}
