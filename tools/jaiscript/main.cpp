// jaiscript — the standalone JaiScript runner/REPL, and the reference embedding.
// Three modes: file runner (`jaiscript file.jai`), interactive REPL (bare `jaiscript`),
// and live-coding watch (`jaiscript --watch file.jai`: edit in your editor, save, see
// output). The host is deliberately thin: engine + stdlib + file IO + a console; every
// engine call in here is the canonical shape from docs/site/embedding.

#include "profile_capture.hpp"
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/core/engine_time.hpp>
#include <jaiscript/core/script_run.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/signals/signal_property.hpp>
#include <jaiscript/signals/signal_impl.hpp>
#include <jaiscript/properties/property_serialization.hpp>   // property_manager::load template bodies
#ifdef JAISCRIPT_ENABLE_DEBUGGER
#include <jaiscript/debug/connector.hpp>
#include "fetch.hpp"   // Download, zip_entries, zip_extract (runner host API)
#include "sound.hpp"   // load_sound, play_sound, stop_sounds (runner host API)
#include "http_transport.hpp"   // spider::catch / sling / get / post over WinHTTP or sockets
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "file_io.hpp"      // read_file, write_file and the rest of the host file IO
#include "net_socket.hpp"   // the socket primitives NetChannel and NetServer stand on

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "winmm.lib")
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <jaiscript/testing/unattended.hpp>

namespace {

namespace fs = std::filesystem;
using namespace jai_runner;

// ---------------------------------------------------------------- console ---

// The console as the runner found it, put back at exit
struct console_found_state {
	bool out_tty = false;
#ifdef _WIN32
	HANDLE out = INVALID_HANDLE_VALUE;
	DWORD out_mode = 0;
	UINT out_code_page = 0;
	UINT in_code_page = 0;
#endif
};
console_found_state g_console_found;

struct console_state {
	bool vt = false;          // VT escape processing available on stdout
	bool out_tty = false;     // stdout is a real console (not a pipe/file)
	bool in_tty = false;      // stdin is a real console
	bool solo_console = false;  // we own a fresh console window (drag-drop / double-click launch)

	void init() {
#ifdef _WIN32
		HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD mode = 0;
		if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
			out_tty = true;
			g_console_found.out = out;
			g_console_found.out_mode = mode;
			vt = SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
		}
		HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
		DWORD in_mode = 0;
		in_tty = in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &in_mode);
		g_console_found.out_code_page = GetConsoleOutputCP();
		g_console_found.in_code_page = GetConsoleCP();
		SetConsoleOutputCP(CP_UTF8);
		SetConsoleCP(CP_UTF8);
		DWORD pids[4];
		solo_console = GetConsoleProcessList(pids, 4) == 1;
		timeBeginPeriod(1);   // 1ms Sleep granularity for sleep-paced game loops
#else
		out_tty = vt = isatty(STDOUT_FILENO) != 0;
		in_tty = isatty(STDIN_FILENO) != 0;
#endif
		g_console_found.out_tty = out_tty;
	}

	bool colors() const { return vt && out_tty; }
	std::string red(const std::string& s) const { return colors() ? "\x1b[31m" + s + "\x1b[0m" : s; }
	std::string dim(const std::string& s) const { return colors() ? "\x1b[90m" + s + "\x1b[0m" : s; }
	std::string green(const std::string& s) const { return colors() ? "\x1b[32m" + s + "\x1b[0m" : s; }

	void clear_screen() const {
		if (!out_tty) { return; }
		std::fputs("\x1b[2J\x1b[H", stdout);
		std::fflush(stdout);
	}
};

// ---------------------------------------------------------------- options ---

struct options {
	std::string script;                    // empty = REPL
	std::vector<std::string> sources;      // -e <source>, in order, one engine: a file run of text
	std::string backend = "vm";
	uint32_t miss_every = 0;               // --miss-every=<n>: the miss injection lever (a build with JAISCRIPT_MISS_INJECTION)
	size_t workers = 0;                    // --workers=<n>: the parallel regions' worker count (0: the engine's own)
	double budget = 0.0;                   // seconds per execute; 0 = unlimited (runner default —
	                                       // the ENGINE default is 5.0s, deliberately overridden:
	                                       // long-running scripts are the user's business here)
	bool watch = false;
	bool warnings = false;                 // --warnings: list a clean load's warnings on stderr before it runs
	bool time_runs = false;
	bool debug = false;                    // --debug: the DAP listener takes no password, so it is opt-in
	bool param_alias = true;               // engine::parameter_alias (--no-param-alias: every by-value parameter clones)
	bool local_alias = true;               // engine::local_alias (--no-local-alias: every by-value local declaration clones)
	bool type_proofs = true;               // engine::type_proofs (--no-proofs: no proofs, typed ops or cell blocks; the general form)
	int debug_port = 0;                    // --debug-port=N, which implies --debug; 0 = default base (see attach_debugger)
	int pause_mode = -1;                   // -1 auto (solo console), 0 never, 1 always
	bool selftest = false;
	std::string disasm;                    // --disasm=<file>: every chunk the VM compiles, disassembled with its regions' IR, in compile order
	std::string manifest;                  // --manifest=<file>: the same chunks' semantic manifests (the identity gate's text)
	std::string coverage;                  // --coverage=<file>: the vm's execution-coverage census at exit (a build with JAISCRIPT_VM_COVERAGE)
	int benchmark_main = 0;               // file execution warms main; then call it N more times
	std::vector<std::string> script_args;  // everything after `--`, exposed as global ARGS
};

void print_usage() {
#ifdef JAISCRIPT_LAYOUT_PADDING
	// Cold placement control: these stores run only for --help, never a gate.
	volatile uint64_t padding = 1;
	padding = (padding * 3ull) ^ 17ull;
	padding = (padding * 5ull) ^ 7936ull;
	padding = (padding * 7ull) ^ 15855ull;
	padding = (padding * 9ull) ^ 23774ull;
	padding = (padding * 11ull) ^ 31693ull;
	padding = (padding * 13ull) ^ 39612ull;
	padding = (padding * 15ull) ^ 47531ull;
	padding = (padding * 17ull) ^ 55450ull;
	padding = (padding * 19ull) ^ 63369ull;
	padding = (padding * 21ull) ^ 71288ull;
	padding = (padding * 23ull) ^ 79207ull;
	padding = (padding * 25ull) ^ 87126ull;
	padding = (padding * 27ull) ^ 95045ull;
	padding = (padding * 29ull) ^ 102964ull;
	padding = (padding * 31ull) ^ 110883ull;
	padding = (padding * 33ull) ^ 118802ull;
	padding = (padding * 35ull) ^ 126721ull;
	padding = (padding * 37ull) ^ 134640ull;
	padding = (padding * 39ull) ^ 142559ull;
	padding = (padding * 41ull) ^ 150478ull;
	padding = (padding * 43ull) ^ 158397ull;
	padding = (padding * 45ull) ^ 166316ull;
	padding = (padding * 47ull) ^ 174235ull;
	padding = (padding * 49ull) ^ 182154ull;
	padding = (padding * 51ull) ^ 190073ull;
	padding = (padding * 53ull) ^ 197992ull;
	padding = (padding * 55ull) ^ 205911ull;
	padding = (padding * 57ull) ^ 213830ull;
	padding = (padding * 59ull) ^ 221749ull;
	padding = (padding * 61ull) ^ 229668ull;
	padding = (padding * 63ull) ^ 237587ull;
	padding = (padding * 65ull) ^ 245506ull;
	padding = (padding * 67ull) ^ 253425ull;
	padding = (padding * 69ull) ^ 261344ull;
	padding = (padding * 71ull) ^ 269263ull;
	padding = (padding * 73ull) ^ 277182ull;
	padding = (padding * 75ull) ^ 285101ull;
	padding = (padding * 77ull) ^ 293020ull;
	padding = (padding * 79ull) ^ 300939ull;
	padding = (padding * 81ull) ^ 308858ull;
	(void)padding;
#endif
	std::puts(
		"jaiscript - standalone JaiScript runner / REPL / live-coding watcher\n"
		"\n"
		"usage:\n"
		"  jaiscript                          interactive REPL (persistent engine; :help inside)\n"
		"  jaiscript file.jai [-- args...]    run a script and exit; the args are the global ARGS, an\n"
		"                                     array of strings (drag-drop a .jai onto the exe works\n"
		"                                     too - the window pauses so you can read output)\n"
		"  jaiscript -e \"source\" [-e ...]     run the source text and exit, like a file: several -e run\n"
		"                                     in order in one engine; nothing prints unless the source\n"
		"                                     prints (piping into the REPL echoes values instead)\n"
		"  jaiscript --watch file.jai         live-coding: runs the file, then re-runs it every time\n"
		"                                     you save - edit in your editor, save, see output.\n"
		"                                     Ctrl+C quits. Each run is a fresh engine; a program that\n"
		"                                     reloads in place reaches import in its loop, inside\n"
		"                                     try/catch: a rejected reload raises at the import and\n"
		"                                     the program keeps its old code.\n"
		"\n"
		"options:\n"
		"  --backend=vm|interp|vm-reference   execution backend (default vm; interp = tree-walking interpreter; vm-reference =\n"
		"                        the vm with every optimization off, engine::vm_reference_mode)\n"
#ifdef JAISCRIPT_MISS_INJECTION
		"  --miss-every=<n>      the vm misses at every nth plain cell instruction (the flush at every position)\n"
#endif
		"  --workers=<n>         the worker count of parallel_for, parallel_transform and parallel_reduce (default: the engine's own)\n"
		"  --disasm=<file>       write every chunk the VM compiles, disassembled with its regions' IR, to <file>\n"
		"  --manifest=<file>     write the same chunks' semantic manifests (every field, exact constant bits) to <file>\n"
#ifdef JAISCRIPT_VM_COVERAGE
		"  --coverage=<file>     write the vm's execution-coverage census (cells / typed / general, the reasons and their\n"
		"                        source sites) to <file> at exit\n"
#endif
		"  --budget=N            execution budget in seconds per execute; overruns raise a script\n"
		"                        error (default 0 = unlimited - infinite loops are your business;\n"
		"                        the embedded-engine default of 5.0s is deliberately lifted here)\n"
		"  --time                print wall time for each run\n"
		"  --warnings            list the static checker's warnings on stderr before a script runs (errors\n"
		"                        always fail the load and print)\n"
		"  --benchmark-main=N    execute file as warm-up, then call its repeatable main() N times\n"
		"  --debug               stand up the DAP debug listener and name its port on stderr (it listens on\n"
		"                        127.0.0.1 with no password: any local process can attach, pause the script\n"
		"                        and read its variables)\n"
		"  --no-param-alias      bind every by-value parameter with a clone (the read-only elision off)\n"
		"  --no-local-alias      copy every by-value local declaration (the read-only local elision off)\n"
		"  --no-proofs           run without type proofs and the typed instructions they enable (the general form)\n"
		"  --debug-port=N        --debug, with N the first port the listener tries (default 52477, probing +10)\n"
		"  --pause / --no-pause  force / suppress the \"press Enter to close\" hold after a file\n"
		"                        run (auto: pauses only when the console window is exclusively\n"
		"                        ours, i.e. drag-drop or double-click launch)\n"
		"  --selftest            run the built-in smoke tests and exit\n"
		"  --version             print the JaiScript version\n"
		"  --help                this text\n"
		"\n"
		"exit code: the script's int result (0..255; outside that range its low byte), otherwise 0;\n"
		"           script errors exit 1 (message + stack trace on stderr), usage errors exit 2;\n"
		"           a piped REPL exits 1 when any input failed.\n"
		"\n"
		"environment: the full JaiScript stdlib is always registered (print/format/to_string/\n"
		"  type_of, math, to_json/from_json, containers, the random type, crypto:: hashes, HMAC,\n"
		"  secure random and passwords), the web::\n"
		"  library (html_escape, url_encode, form_encode, parse_query, build_query, parse_url, ...),\n"
		"  the spider:: HTTP client (spider::catch(url[, options]), also spelled spider::get;\n"
		"  spider::sling(content, url[, options]), also spelled spider::post(url, content[, options]);\n"
		"  each answers a spider::Response; WinHTTP on Windows, sockets elsewhere, HTTPS through OpenSSL\n"
		"  when the build found it), with the network permission granted,\n"
		"  plus host file IO: read_file(path),\n"
		"  write_file(path, text) (creates missing folders), file_exists(path), delete_file(path), Download,\n"
		"  zip_entries(zip), zip_extract(zip, entry, dest), dir_exists(path),\n"
		"  make_dirs(path), list_dir(path), writable_dir(path). Locations are language builtins:\n"
		"  executable_dir() (alias bin_dir()), root_script_dir() (the script this run started with),\n"
		"  current_script_dir() (the file whose code is running), the *_path() twins, resolve_path(name)\n"
		"  (the file import would load for that name), app_data_dir(name) / app_cache_dir(name),\n"
		"  working_dir(), parent_dir(p), file_name(p) - a script dropped onto the executable runs\n"
		"  from any working directory when its paths start from one of these; and console-game IO:\n"
		"  key_down(name), read_key(), term_cols(), term_rows(), env_var(name) (clocks are time::,\n"
		"  waits sleep()),\n"
		"  sound: load_sound(path) -> Sound (play()/play(true) loops/stop()/playing()/volume(0..1)/\n"
		"  format(); wav, midi or mp3 by the file's header), play_sound(path[, loop]), stop_sounds()\n"
		"  (each Sound mixes with the others; Windows only, null elsewhere). POSIX terminals synthesize\n"
		"  key_down from auto-repeat; there are no key-up events. Networking: NetChannel\n"
		"  (new NetChannel(); listen(port)/connect(host,port)/send(bytes)/pump()/close();\n"
		"  signals on_packet(bytes)/on_connect()/on_disconnect(reason) fire during pump();\n"
		"  4-byte big-endian length framing) and NetServer (N clients on one\n"
		"  port: listen/send(id,bytes)/broadcast/pump/kick/client_count; per-client signals\n"
		"  on_client_connect(id)/on_client_packet(id,bytes)/on_client_disconnect(id,reason)),\n"
		"  net_host_name() and net_local_addresses() (this machine's IPv4 addresses).\n"
		"  Args after `--` are the global ARGS (array of strings). include/import resolve\n"
		"  relative to the script's folder.\n"
		"\n"
		"debugging: --debug stands up a DAP listener (localhost only, costs nothing until a session\n"
		"  attaches); nothing listens without it. Attach from VS Code with the jaiscript extension\n"
		"  (\"Attach to JaiScript\") on the logged port. Works in file and REPL modes; --watch uses a\n"
		"  fresh engine per run, so the listener is off there.\n");
}

// A number flag's value, whole and inside [low, high]; nullopt for anything else (the caller reports a usage error)
template<typename Number>
std::optional<Number> number_in_range(const std::string& text, Number low, Number high) {
	Number value{};
	if constexpr (std::is_floating_point_v<Number>) {
		char* end = nullptr;
		errno = 0;
		value = static_cast<Number>(std::strtod(text.c_str(), &end));
		if (text.empty() || end != text.c_str() + text.size() || errno == ERANGE || !std::isfinite(value)) { return std::nullopt; }
	} else {
		auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (ec != std::errc{} || end != text.data() + text.size()) { return std::nullopt; }
	}
	if (value < low || value > high) { return std::nullopt; }
	return value;
}

int parse_args(int argc, char** argv, options& opt, const console_state& con) {
	bool args_section = false;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (args_section) { opt.script_args.push_back(a); continue; }
		auto usage_error = [&](const std::string& text) {
			std::fprintf(stderr, "%s\n", con.red(text).c_str());
			return 2;
		};
		// a value flag is the flag alone (its value the next argument) or flag=value; any other spelling is unknown
		auto is_flag = [&](const char* flag) { return a == flag || a.rfind(std::string(flag) + "=", 0) == 0; };
		auto value_of = [&](const char* flag) -> std::optional<std::string> {
			if (a.size() > std::strlen(flag)) { return a.substr(std::strlen(flag) + 1); }
			if (i + 1 < argc) { return std::string(argv[++i]); }
			return std::nullopt;
		};
		auto text_of = [&](const char* flag, std::string& out) {
			std::optional<std::string> text = value_of(flag);
			if (!text) {
				usage_error(std::string("missing value for ") + flag);
				return false;
			}
			out = std::move(*text);
			return true;
		};
		auto number_of = [&](const char* flag, auto low, auto high, const char* expected) -> std::optional<decltype(low)> {
			std::optional<std::string> text = value_of(flag);
			if (!text) {
				usage_error(std::string("missing value for ") + flag);
				return std::nullopt;
			}
			auto value = number_in_range(*text, low, high);
			if (!value) { usage_error(std::string("invalid value for ") + flag + ": '" + *text + "' (" + expected + ")"); }
			return value;
		};
		if (a == "--") { args_section = true; }
		else if (a == "-e") {
			if (i + 1 >= argc) { return usage_error("missing value for -e"); }
			opt.sources.push_back(argv[++i]);
		}
		else if (a == "--watch") { opt.watch = true; }
		else if (a == "--time") { opt.time_runs = true; }
		else if (a == "--warnings") { opt.warnings = true; }
		else if (a == "--debug") { opt.debug = true; }
		else if (a == "--no-param-alias") { opt.param_alias = false; }
		else if (a == "--no-local-alias") { opt.local_alias = false; }
		else if (a == "--no-proofs") { opt.type_proofs = false; }
		else if (a == "--pause") { opt.pause_mode = 1; }
		else if (a == "--no-pause") { opt.pause_mode = 0; }
		else if (a == "--selftest") { opt.selftest = true; }
		else if (is_flag("--benchmark-main")) {
			auto n = number_of("--benchmark-main", 0, INT_MAX, "a count from 0");
			if (!n) { return 2; }
			opt.benchmark_main = *n;
		}
		else if (is_flag("--workers")) {
			auto n = number_of("--workers", size_t{0}, size_t{4096}, "a count from 0 to 4096; 0 is the engine's own");
			if (!n) { return 2; }
			opt.workers = *n;
		}
		else if (is_flag("--miss-every")) {
			auto n = number_of("--miss-every", uint32_t{0}, UINT32_MAX, "a count from 0");
			if (!n) { return 2; }
			opt.miss_every = *n;
		}
		else if (is_flag("--budget")) {
			auto n = number_of("--budget", 0.0, 1e9, "seconds from 0; 0 is unlimited");
			if (!n) { return 2; }
			opt.budget = *n;
		}
		else if (is_flag("--debug-port")) {
			auto n = number_of("--debug-port", 1, 65535, "a port from 1 to 65535");
			if (!n) { return 2; }
			opt.debug_port = *n;
			opt.debug = true;
		}
		else if (is_flag("--backend")) { if (!text_of("--backend", opt.backend)) { return 2; } }
		else if (is_flag("--disasm")) { if (!text_of("--disasm", opt.disasm)) { return 2; } }
		else if (is_flag("--manifest")) { if (!text_of("--manifest", opt.manifest)) { return 2; } }
		else if (is_flag("--coverage")) { if (!text_of("--coverage", opt.coverage)) { return 2; } }
		else if (a == "--version" || a == "-v") { std::printf("JaiScript %s\n", jai::version().c_str()); std::exit(0); }
		else if (a == "--help" || a == "-h" || a == "/?") { print_usage(); std::exit(0); }
		else if (!a.empty() && a[0] == '-') { return usage_error("unknown flag: " + a + " (jaiscript --help lists the flags)"); }
		else if (opt.script.empty()) { opt.script = a; }
		else {
			std::fprintf(stderr, "%s\n", con.red("unexpected extra argument: " + a + " (script args go after --)").c_str());
			return 2;
		}
	}
	if (!opt.sources.empty() && (!opt.script.empty() || opt.watch)) {
		std::fprintf(stderr, "%s\n", con.red("-e runs its source instead of a script file (no file, no --watch)").c_str());
		return 2;
	}
	if (opt.benchmark_main < 0 || (opt.benchmark_main > 0 && (opt.script.empty() || opt.watch))) {
		std::fprintf(stderr, "--benchmark-main needs a file, a non-negative count and no --watch\n");
		return 2;
	}
	if (opt.backend == "interpreter") { opt.backend = "interp"; }
	if (opt.backend != "vm" && opt.backend != "interp" && opt.backend != "vm-reference") {
		std::fprintf(stderr, "%s\n", con.red("unknown backend: " + opt.backend + " (use vm, interp or vm-reference)").c_str());
		return 2;
	}
	if (opt.watch && opt.script.empty()) {
		std::fprintf(stderr, "%s\n", con.red("--watch needs a script file").c_str());
		return 2;
	}
	return -1;   // keep going
}

// --------------------------------------------------------- console game IO ---
// The capabilities a real-time terminal script cannot express (input and terminal
// size; clocks are the stdlib's time::, waits the sleep builtin), same shapes as the crawler/gloom hosts.
// Windows-first; elsewhere the input pair reports nothing, like those hosts.

#ifdef _WIN32
int vk_for_key_name(const std::string& name) {
	if (name.size() == 1) {
		char c = name[0];
		if (c >= 'a' && c <= 'z') { return c - 'a' + 'A'; }
		if (c >= 'A' && c <= 'Z') { return c; }
		if (c >= '0' && c <= '9') { return c; }
	}
	if (name == "space") { return VK_SPACE; }
	if (name == "left") { return VK_LEFT; }
	if (name == "right") { return VK_RIGHT; }
	if (name == "up") { return VK_UP; }
	if (name == "down") { return VK_DOWN; }
	if (name == "shift") { return VK_SHIFT; }
	if (name == "ctrl") { return VK_CONTROL; }
	if (name == "alt") { return VK_MENU; }
	if (name == "esc") { return VK_ESCAPE; }
	if (name == "enter") { return VK_RETURN; }
	if (name == "tab") { return VK_TAB; }
	return 0;
}

std::string key_name_for_vk(WORD vk) {
	if (vk >= 'A' && vk <= 'Z') { return std::string(1, static_cast<char>(vk - 'A' + 'a')); }
	if (vk >= '0' && vk <= '9') { return std::string(1, static_cast<char>(vk)); }
	switch (vk) {
		case VK_LEFT: return "left";
		case VK_RIGHT: return "right";
		case VK_UP: return "up";
		case VK_DOWN: return "down";
		case VK_SPACE: return "space";
		case VK_SHIFT: return "shift";
		case VK_CONTROL: return "ctrl";
		case VK_MENU: return "alt";
		case VK_ESCAPE: return "esc";
		case VK_RETURN: return "enter";
		case VK_TAB: return "tab";
		case VK_BACK: return "backspace";
		default: return "";
	}
}

// ReadConsoleInput pump: real down/up transitions from the console event queue.
// Works under Windows Terminal/ConPTY, where GetConsoleWindow() is a hidden
// pseudo-console window and foreground tests always fail; focus gating is
// implicit (the terminal only forwards events while focused), and focus-loss
// clears the held table so keys cannot stick.
struct win_input_state {
	std::map<std::string, bool> held;
	std::vector<std::string> edges;
};
win_input_state g_win_input;

void win_pump_input() {
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	DWORD mode = 0;
	if (in == INVALID_HANDLE_VALUE || !GetConsoleMode(in, &mode)) { return; }   // piped stdin: no game input
	DWORD avail = 0;
	while (GetNumberOfConsoleInputEvents(in, &avail) && avail > 0) {
		INPUT_RECORD records[32];
		DWORD got = 0;
		if (!ReadConsoleInput(in, records, 32, &got) || got == 0) { return; }
		for (DWORD i = 0; i < got; ++i) {
			if (records[i].EventType == FOCUS_EVENT && !records[i].Event.FocusEvent.bSetFocus) {
				g_win_input.held.clear();
			} else if (records[i].EventType == KEY_EVENT) {
				const KEY_EVENT_RECORD& k = records[i].Event.KeyEvent;
				std::string name = key_name_for_vk(k.wVirtualKeyCode);
				if (name.empty()) {
					// punctuation and numpad have no vk row: take the console's own
					// character translation ('.', ':', '-', shifted symbols, numpad digits)
					char translated = static_cast<char>(k.uChar.UnicodeChar);
					if (k.uChar.UnicodeChar > 32 && k.uChar.UnicodeChar < 127) {
						name = std::string(1, static_cast<char>(std::tolower(translated)));
					}
				}
				if (name.empty()) { continue; }
				g_win_input.held[name] = k.bKeyDown != FALSE;
				if (k.bKeyDown) {
					g_win_input.edges.push_back(name);
					if (g_win_input.edges.size() > 128) {           // key_down-only scripts must not hoard edges
						g_win_input.edges.erase(g_win_input.edges.begin());
					}
				}
			}
		}
	}
}
#else
// POSIX terminals deliver bytes, not key events: pump stdin in raw mode into
// (a) an edge queue for read_key and (b) a last-press table that auto-repeat
// keeps fresh — key_down = "pressed within the hold window". There are no
// key-up events and no modifier state (shift/ctrl/alt never read down here).
// Linux and macOS share this branch (identical termios/ioctl); macOS
// Terminal.app may send SS3 arrows (ESC O A) — both prefixes are parsed.
struct posix_input_state {
	std::atomic<bool> raw{false};   // read by the Ctrl+C exit
	termios saved{};
	std::vector<std::string> edges;
	std::map<std::string, int64_t> last_press;
};
posix_input_state g_posix_input;
constexpr int64_t posix_hold_window_ms = 500;

int64_t posix_now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

void posix_restore_terminal() {
	if (g_posix_input.raw) {
		tcsetattr(STDIN_FILENO, TCSANOW, &g_posix_input.saved);
		g_posix_input.raw = false;
	}
}

void posix_enter_raw() {
	if (g_posix_input.raw || !isatty(STDIN_FILENO)) { return; }
	tcgetattr(STDIN_FILENO, &g_posix_input.saved);
	termios raw = g_posix_input.saved;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 0;    // polling reads return immediately
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	g_posix_input.raw = true;
}

void posix_note_key(const std::string& key) {
	g_posix_input.edges.push_back(key);
	if (g_posix_input.edges.size() > 128) {              // key_down-only scripts must not hoard edges
		g_posix_input.edges.erase(g_posix_input.edges.begin());
	}
	g_posix_input.last_press[key] = posix_now_ms();
}

// one short retry distinguishes a lone ESC press from an escape sequence
// arriving split across reads (ssh latency)
bool posix_read_byte(unsigned char& b) {
	if (read(STDIN_FILENO, &b, 1) == 1) { return true; }
	std::this_thread::sleep_for(std::chrono::milliseconds(4));
	return read(STDIN_FILENO, &b, 1) == 1;
}

void posix_pump_input() {
	posix_enter_raw();
	if (!g_posix_input.raw) { return; }
	unsigned char b = 0;
	while (read(STDIN_FILENO, &b, 1) == 1) {
		if (b == 0x1B) {
			unsigned char b2 = 0;
			if (!posix_read_byte(b2)) { posix_note_key("esc"); continue; }
			if (b2 == '[' || b2 == 'O') {
				// CSI (ESC [) or SS3 (ESC O, macOS application cursor mode):
				// consume parameter/intermediate bytes so modified sequences
				// (ESC [ 1;5A etc.) never leak phantom keypresses
				unsigned char fin = 0;
				bool have = posix_read_byte(fin);
				while (have && b2 == '[' && fin >= 0x20 && fin <= 0x3F) { have = posix_read_byte(fin); }
				if (!have) { continue; }
				switch (fin) {
					case 'A': posix_note_key("up"); break;
					case 'B': posix_note_key("down"); break;
					case 'C': posix_note_key("right"); break;
					case 'D': posix_note_key("left"); break;
					default: break;                      // unknown sequence: swallowed
				}
				continue;
			}
			posix_note_key("esc");
			b = b2;                                      // ESC then a plain byte: classify it below
		}
		if (b == '\n' || b == '\r') { posix_note_key("enter"); continue; }
		if (b == '\t') { posix_note_key("tab"); continue; }
		if (b == 127 || b == 8) { posix_note_key("backspace"); continue; }
		if (b == ' ') { posix_note_key("space"); continue; }
		if (b > 32 && b < 127) { posix_note_key(std::string(1, static_cast<char>(std::tolower(b)))); }
	}
}
#endif

void register_console_game_io(jai::engine& eng) {
	// held-key state from console events, cross-checked against the physical
	// key so a release delivered to another window (alt-tab mid-press) unsticks
	eng.add_function("key_down", [](const std::string& name) -> bool {
#ifdef _WIN32
		win_pump_input();
		auto it = g_win_input.held.find(name);
		if (it == g_win_input.held.end() || !it->second) { return false; }
		int vk = vk_for_key_name(name);
		return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
		posix_pump_input();
		auto it = g_posix_input.last_press.find(name);
		return it != g_posix_input.last_press.end()
			&& posix_now_ms() - it->second < posix_hold_window_ms;
#endif
	});
	// non-blocking edge events: "" if none; arrows/esc/enter/tab named, letters lowercased
	eng.add_function("read_key", []() -> std::string {
#ifdef _WIN32
		win_pump_input();
		if (g_win_input.edges.empty()) { return ""; }
		std::string key = g_win_input.edges.front();
		g_win_input.edges.erase(g_win_input.edges.begin());
		return key;
#else
		posix_pump_input();
		if (g_posix_input.edges.empty()) { return ""; }
		std::string key = g_posix_input.edges.front();
		g_posix_input.edges.erase(g_posix_input.edges.begin());
		return key;
#endif
	});
	eng.add_function("term_cols", []() -> jai::script_int {
#ifdef _WIN32
		CONSOLE_SCREEN_BUFFER_INFO info;
		if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
			return info.srWindow.Right - info.srWindow.Left + 1;
		}
#else
		winsize ws{};
		if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) { return ws.ws_col; }
#endif
		return 80;
	});
	// an environment variable's value, "" when unset (COLORTERM says whether the terminal takes 24-bit colour)
	eng.add_function("env_var", [](const std::string& name) -> std::string {
#ifdef _WIN32
		char* value = nullptr;
		size_t size = 0;
		if (_dupenv_s(&value, &size, name.c_str()) != 0 || !value) { return std::string(); }
		std::string result(value);
		std::free(value);
		return result;
#else
		const char* value = std::getenv(name.c_str());
		return value ? std::string(value) : std::string();
#endif
	});
	eng.add_function("term_rows", []() -> jai::script_int {
#ifdef _WIN32
		CONSOLE_SCREEN_BUFFER_INFO info;
		if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
			return info.srWindow.Bottom - info.srWindow.Top + 1;
		}
#else
		winsize ws{};
		if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) { return ws.ws_row; }
#endif
		return 25;
	});
}

// ------------------------------------------------------------- exit and ^C ---
// Ctrl+C and Ctrl+Break (Windows), SIGINT, SIGTERM and SIGHUP (POSIX) end the runner at once: the terminal goes back to
// how the runner found it, then the process exits as the signal would. A runaway script is the execution budget's job.

// What the script's output changed in the terminal, read from the escape sequences it writes: a runner API would reach only
// the scripts written for it, while every terminal program already writes these.
class terminal_watch : public std::streambuf {
public:
	std::atomic<bool> alternate_screen{false};
	std::atomic<bool> cursor_hidden{false};
	std::atomic<bool> styled{false};   // a colour or attribute other than the reset is in effect

	bool changed() const { return alternate_screen || cursor_hidden || styled; }
	void forget() { alternate_screen = cursor_hidden = styled = false; }

	void read_output(const char* s, size_t n) {
		for (size_t i = 0; i < n; ++i) {
			const char c = s[i];
			if (state_ == parse::text) {
				const void* escape = std::memchr(s + i, 0x1B, n - i);
				if (!escape) { return; }
				i = static_cast<size_t>(static_cast<const char*>(escape) - s);
				state_ = parse::escape;
			} else if (state_ == parse::escape) {
				parameters_.clear();
				state_ = c == '[' ? parse::control_sequence : c == 0x1B ? parse::escape : parse::text;
			} else if (c >= 0x20 && c <= 0x3F) {
				if (parameters_.size() < 64) { parameters_ += c; }
			} else {
				if (c >= 0x40 && c <= 0x7E) { finish_control_sequence(c); }
				state_ = c == 0x1B ? parse::escape : parse::text;
			}
		}
	}

	void finish_control_sequence(char final_byte) {
		if (final_byte == 'm') {
			styled = parameters_.find_first_not_of("0;") != std::string::npos;
			return;
		}
		if ((final_byte != 'h' && final_byte != 'l') || parameters_.empty() || parameters_[0] != '?') { return; }
		const bool set = final_byte == 'h';
		size_t start = 1;
		while (start <= parameters_.size()) {
			size_t end = parameters_.find(';', start);
			if (end == std::string::npos) { end = parameters_.size(); }
			const std::string_view mode(parameters_.data() + start, end - start);
			if (mode == "1049" || mode == "1047" || mode == "47") { alternate_screen = set; }
			else if (mode == "25") { cursor_hidden = !set; }
			start = end + 1;
		}
	}

protected:
	int_type overflow(int_type ch) override {
		if (traits_type::eq_int_type(ch, traits_type::eof())) { return traits_type::not_eof(ch); }
		const char c = traits_type::to_char_type(ch);
		read_output(&c, 1);
		return std::fputc(c, stdout) == EOF ? traits_type::eof() : ch;
	}
	std::streamsize xsputn(const char* s, std::streamsize n) override {
		read_output(s, static_cast<size_t>(n));
		return static_cast<std::streamsize>(std::fwrite(s, 1, static_cast<size_t>(n), stdout));
	}
	int sync() override { return std::fflush(stdout) == 0 ? 0 : -1; }

private:
	enum class parse { text, escape, control_sequence };
	parse state_ = parse::text;
	std::string parameters_;
};

terminal_watch g_terminal_watch;
std::ostream g_watched_out(&g_terminal_watch);

// Raw writes only: the Ctrl+C exit runs in a signal handler, or beside a main thread that may hold stdio's lock
void write_raw(const char* text, size_t size) {
#ifdef _WIN32
	DWORD written = 0;
	WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, static_cast<DWORD>(size), &written, nullptr);
#else
	while (size > 0) {
		const ssize_t written = write(STDOUT_FILENO, text, size);
		if (written <= 0) { return; }
		text += written;
		size -= static_cast<size_t>(written);
	}
#endif
}

// Puts back what the script's output left changed, on a terminal only (a pipe or a file keeps the script's bytes exactly)
void restore_terminal_output() {
	if (!g_console_found.out_tty || !g_terminal_watch.changed()) { return; }
	char text[32];
	size_t size = 0;
	auto add = [&](const char* sequence) {
		const size_t length = std::strlen(sequence);
		std::memcpy(text + size, sequence, length);
		size += length;
	};
	if (g_terminal_watch.styled) { add("\x1b[0m"); }
	if (g_terminal_watch.cursor_hidden) { add("\x1b[?25h"); }
	if (g_terminal_watch.alternate_screen) { add("\x1b[?1049l"); }
	g_terminal_watch.forget();
	write_raw(text, size);
}

// The console modes and code pages, and the termios a script reading keys changed
void restore_console() {
#ifdef _WIN32
	if (g_console_found.out != INVALID_HANDLE_VALUE) { SetConsoleMode(g_console_found.out, g_console_found.out_mode); }
	if (g_console_found.out_code_page) { SetConsoleOutputCP(g_console_found.out_code_page); }
	if (g_console_found.in_code_page) { SetConsoleCP(g_console_found.in_code_page); }
#else
	posix_restore_terminal();
#endif
}

#ifdef _WIN32
// The console runs this on a thread of its own while the script's thread runs on: TerminateProcess, because ExitProcess's
// teardown can wait on a CRT lock that terminated thread held
BOOL WINAPI ctrl_handler(DWORD type) {
	if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) { return FALSE; }
	restore_terminal_output();
	restore_console();
	TerminateProcess(GetCurrentProcess(), 130);
	return TRUE;
}
#else
// Async-signal-safe throughout: write(2), tcsetattr and the default disposition re-raised, so the shell sees the signal
void posix_exit_on_signal(int sig) {
	restore_terminal_output();
	restore_console();
	signal(sig, SIG_DFL);
	sigset_t unblock;
	sigemptyset(&unblock);
	sigaddset(&unblock, sig);
	pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
	raise(sig);
	_exit(128 + sig);
}
#endif

void install_exit_handlers() {
	std::atexit([] {
		restore_terminal_output();
		restore_console();
	});
#ifdef _WIN32
	SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
	for (int sig : {SIGINT, SIGTERM, SIGHUP}) {
		struct sigaction current {};
		if (sigaction(sig, nullptr, &current) == 0 && current.sa_handler == SIG_IGN) { continue; }   // nohup, a background job
		struct sigaction action {};
		action.sa_handler = posix_exit_on_signal;
		sigemptyset(&action.sa_mask);
		sigaction(sig, &action, nullptr);
	}
#endif
}

// A mode's end: what the script left changed goes back before the exit code does
int runner_exit(int code) {
	std::fflush(stdout);
	std::fflush(stderr);
	restore_terminal_output();
	return code;
}

// -------------------------------------------------------------- networking ---
// A generic length-framed TCP channel for scripts: a 4-byte big-endian header + content
// framing on the wire, and the property_owner + JAI_SIGNAL pattern on the API. No threads: the
// channel owns non-blocking sockets and a script-called pump() drains them,
// firing signals inside the script's own call stack. Protocol semantics
// (actions, lockstep, handshakes) live entirely in script.

class net_channel : public jai::property_owner<net_channel> {
	friend jai::access;
public:
	typedef void PacketSignature(const std::string&);
	typedef void ConnectSignature();
	typedef void DisconnectSignature(const std::string&);

	JAI_SIGNAL(PacketSignature, on_packet);
	JAI_SIGNAL(ConnectSignature, on_connect);
	JAI_SIGNAL(DisconnectSignature, on_disconnect);

	net_channel() = default;
	~net_channel() { drop_sockets(); }

	bool listen(jai::script_int port) {
		if (!net_ready()) { return fail("winsock unavailable"); }
		drop_sockets();
		listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listener_ == net_bad_socket) { return fail("socket() failed"); }
		net_listen_options(listener_);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(static_cast<unsigned short>(port));
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
				|| ::listen(listener_, 1) != 0) {
			drop_sockets();
			return fail("bind/listen failed (port taken?)");
		}
		net_set_nonblocking(listener_);
		state_ = "listening";
		return true;
	}

	bool connect(const std::string& host, jai::script_int port) {
		if (!net_ready()) { return fail("winsock unavailable"); }
		drop_sockets();
		addrinfo hints{};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		addrinfo* found = nullptr;
		if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
			return fail("host not found: " + host);
		}
		peer_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (peer_ == net_bad_socket) {
			::freeaddrinfo(found);
			return fail("socket() failed");
		}
		net_set_nonblocking(peer_);
		int result = ::connect(peer_, found->ai_addr, static_cast<int>(found->ai_addrlen));
		::freeaddrinfo(found);
		if (result == 0) {
			state_ = "connected";
			announce_connect_ = true;
		} else if (net_would_block()) {
			state_ = "connecting";
		} else {
			drop_sockets();
			return fail("connect failed");
		}
		return true;
	}

	// frames and queues; pump() flushes whatever the socket won't take now
	bool send(const std::string& bytes) {
		if (state_ != "connected" && state_ != "connecting") { return false; }
		char header[4] = {
			static_cast<char>((bytes.size() >> 24) & 0xFF), static_cast<char>((bytes.size() >> 16) & 0xFF),
			static_cast<char>((bytes.size() >> 8) & 0xFF), static_cast<char>(bytes.size() & 0xFF) };
		outbox_.append(header, 4);
		outbox_.append(bytes);
		if (state_ == "connected") { flush(); }
		return true;
	}

	jai::script_int pump() {
		jai::script_int fired = 0;
		if (listener_ != net_bad_socket && peer_ == net_bad_socket) {
			net_socket accepted = ::accept(listener_, nullptr, nullptr);
			if (accepted != net_bad_socket) {
				net_set_nonblocking(accepted);
				peer_ = accepted;
				state_ = "connected";
				announce_connect_ = true;
			}
		}
		if (state_ == "connecting" && peer_ != net_bad_socket) {
			// refusal lands in the EXCEPT set on Windows (writability never fires)
			fd_set writable;
			fd_set failed;
			FD_ZERO(&writable);
			FD_ZERO(&failed);
			FD_SET(peer_, &writable);
			FD_SET(peer_, &failed);
			timeval instant{0, 0};
			if (::select(static_cast<int>(peer_ + 1), nullptr, &writable, &failed, &instant) > 0) {
				int error = 0;
				socklen_t len = sizeof(error);
				::getsockopt(peer_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len);
				if (error == 0 && !FD_ISSET(peer_, &failed)) {
					state_ = "connected";
					announce_connect_ = true;
				} else {
					disconnect("connect refused");
					return fired;
				}
			}
		}
		if (announce_connect_) {
			announce_connect_ = false;
			flush();
			on_connectSignal();
		}
		if (state_ != "connected" || peer_ == net_bad_socket) { return fired; }
		flush();
		char chunk[4096];
		std::string drop_reason;                          // close/error noted, HELD until queued
		while (peer_ != net_bad_socket) {                 // frames deliver first (a goodbye packet
			int got = static_cast<int>(::recv(peer_, chunk, sizeof(chunk), 0));   // often rides the same recv as the close)
			if (got > 0) { inbox_.append(chunk, static_cast<size_t>(got)); continue; }
			if (got == 0) { drop_reason = "closed by peer"; }
			else if (!net_would_block()) { drop_reason = "recv error"; }
			break;
		}
		while (inbox_.size() >= 4) {
			size_t length = (static_cast<size_t>(static_cast<unsigned char>(inbox_[0])) << 24)
				| (static_cast<size_t>(static_cast<unsigned char>(inbox_[1])) << 16)
				| (static_cast<size_t>(static_cast<unsigned char>(inbox_[2])) << 8)
				| static_cast<size_t>(static_cast<unsigned char>(inbox_[3]));
			if (length > 64u * 1024u * 1024u) {
				disconnect("oversized frame");
				break;
			}
			if (inbox_.size() < 4 + length) { break; }
			std::string content = inbox_.substr(4, length);
			inbox_.erase(0, 4 + length);
			++fired;
			on_packetSignal(content);              // may reentrantly send()/close(): state re-checked per loop
			if (state_ != "connected") { break; }
		}
		if (!drop_reason.empty() && state_ == "connected") { disconnect(drop_reason); }
		return fired;
	}

	void close() {
		drop_sockets();
		state_ = "idle";
	}

	bool connected() const { return state_ == "connected"; }
	std::string status() const { return state_; }

private:
	bool fail(const std::string& why) {
		state_ = "error: " + why;
		return false;
	}

	void disconnect(const std::string& reason) {
		drop_sockets();
		state_ = "closed: " + reason;
		on_disconnectSignal(reason);
	}

	void flush() {
		while (!outbox_.empty() && peer_ != net_bad_socket && state_ == "connected") {
			int sent = static_cast<int>(::send(peer_, outbox_.data(), static_cast<int>(outbox_.size()), 0));
			if (sent > 0) { outbox_.erase(0, static_cast<size_t>(sent)); continue; }
			if (!net_would_block()) { disconnect("send error"); }
			break;
		}
	}

	void drop_sockets() {
		if (peer_ != net_bad_socket) { net_close_socket(peer_); peer_ = net_bad_socket; }
		if (listener_ != net_bad_socket) { net_close_socket(listener_); listener_ = net_bad_socket; }
		inbox_.clear();
		outbox_.clear();
	}

	net_socket peer_ = net_bad_socket;
	net_socket listener_ = net_bad_socket;
	std::string inbox_;
	std::string outbox_;
	std::string state_ = "idle";
	bool announce_connect_ = false;
};

// The server shape for scripts: persistent listener, N framed connections,
// per-client signals, broadcast. Same wire format as net_channel; same no-thread
// pump contract. Client ids are stable, never reused within a server's lifetime.
class net_server : public jai::property_owner<net_server> {
	friend jai::access;
public:
	typedef void ClientSignature(jai::script_int);
	typedef void ClientPacketSignature(jai::script_int, const std::string&);
	typedef void ClientDropSignature(jai::script_int, const std::string&);

	JAI_SIGNAL(ClientSignature, on_client_connect);
	JAI_SIGNAL(ClientPacketSignature, on_client_packet);
	JAI_SIGNAL(ClientDropSignature, on_client_disconnect);

	net_server() = default;
	~net_server() { close(); }

	bool listen(jai::script_int port) {
		if (!net_ready()) { return false; }
		close();
		listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listener_ == net_bad_socket) { return false; }
		net_listen_options(listener_);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(static_cast<unsigned short>(port));
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
				|| ::listen(listener_, 8) != 0) {
			net_close_socket(listener_);
			listener_ = net_bad_socket;
			return false;
		}
		net_set_nonblocking(listener_);
		return true;
	}

	bool send(jai::script_int id, const std::string& bytes) {
		client* c = find(id);
		if (!c) { return false; }
		frame_into(c->outbox, bytes);
		flush(*c);
		return true;
	}

	jai::script_int broadcast(const std::string& bytes) {
		jai::script_int reached = 0;
		for (auto& c : clients_) {
			if (c.sock == net_bad_socket) { continue; }
			frame_into(c.outbox, bytes);
			flush(c);
			++reached;
		}
		return reached;
	}

	jai::script_int pump() {
		jai::script_int fired = 0;
		if (listener_ != net_bad_socket) {
			for (;;) {
				net_socket accepted = ::accept(listener_, nullptr, nullptr);
				if (accepted == net_bad_socket) { break; }
				net_set_nonblocking(accepted);
				clients_.push_back(client{next_id_++, accepted});
				on_client_connectSignal(clients_.back().id);
			}
		}
		for (size_t i = 0; i < clients_.size(); ++i) {   // index loop: receivers may connect more clients
			client& c = clients_[i];
			if (c.sock == net_bad_socket) { continue; }
			flush(c);
			std::string drop_reason;                      // held until queued frames deliver
			char chunk[4096];
			while (c.sock != net_bad_socket) {
				int got = static_cast<int>(::recv(c.sock, chunk, sizeof(chunk), 0));
				if (got > 0) { c.inbox.append(chunk, static_cast<size_t>(got)); continue; }
				if (got == 0) { drop_reason = "closed by peer"; }
				else if (!net_would_block()) { drop_reason = "recv error"; }
				break;
			}
			while (c.sock != net_bad_socket && c.inbox.size() >= 4) {
				size_t length = (static_cast<size_t>(static_cast<unsigned char>(c.inbox[0])) << 24)
					| (static_cast<size_t>(static_cast<unsigned char>(c.inbox[1])) << 16)
					| (static_cast<size_t>(static_cast<unsigned char>(c.inbox[2])) << 8)
					| static_cast<size_t>(static_cast<unsigned char>(c.inbox[3]));
				if (length > 64u * 1024u * 1024u) {
					drop_reason = "oversized frame";
					break;
				}
				if (c.inbox.size() < 4 + length) { break; }
				std::string content = c.inbox.substr(4, length);
				c.inbox.erase(0, 4 + length);
				++fired;
				on_client_packetSignal(c.id, content);
			}
			if (!drop_reason.empty() && c.sock != net_bad_socket) {
				drop(c, drop_reason);
			}
		}
		clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
			[](const client& c) { return c.sock == net_bad_socket; }), clients_.end());
		return fired;
	}

	void kick(jai::script_int id) {
		if (client* c = find(id)) { drop(*c, "kicked"); }
	}

	jai::script_int client_count() const {
		return static_cast<jai::script_int>(clients_.size());
	}

	bool listening() const { return listener_ != net_bad_socket; }

	void close() {
		for (auto& c : clients_) {
			if (c.sock != net_bad_socket) { net_close_socket(c.sock); c.sock = net_bad_socket; }
		}
		clients_.clear();
		if (listener_ != net_bad_socket) { net_close_socket(listener_); listener_ = net_bad_socket; }
	}

private:
	struct client {
		jai::script_int id = 0;
		net_socket sock = net_bad_socket;
		std::string inbox;
		std::string outbox;
	};

	client* find(jai::script_int id) {
		for (auto& c : clients_) {
			if (c.id == id && c.sock != net_bad_socket) { return &c; }
		}
		return nullptr;
	}

	static void frame_into(std::string& outbox, const std::string& bytes) {
		char header[4] = {
			static_cast<char>((bytes.size() >> 24) & 0xFF), static_cast<char>((bytes.size() >> 16) & 0xFF),
			static_cast<char>((bytes.size() >> 8) & 0xFF), static_cast<char>(bytes.size() & 0xFF) };
		outbox.append(header, 4);
		outbox.append(bytes);
	}

	void flush(client& c) {
		while (!c.outbox.empty() && c.sock != net_bad_socket) {
			int sent = static_cast<int>(::send(c.sock, c.outbox.data(), static_cast<int>(c.outbox.size()), 0));
			if (sent > 0) { c.outbox.erase(0, static_cast<size_t>(sent)); continue; }
			if (!net_would_block()) { drop(c, "send error"); }
			break;
		}
	}

	void drop(client& c, const std::string& reason) {
		if (c.sock == net_bad_socket) { return; }
		net_close_socket(c.sock);
		c.sock = net_bad_socket;
		on_client_disconnectSignal(c.id, reason);
	}

	net_socket listener_ = net_bad_socket;
	std::vector<client> clients_;
	jai::script_int next_id_ = 1;
};

void register_net_channel(jai::engine& eng) {
	jai::dynamic_binder<net_server>(eng, "NetServer")
		.constructor<>()
		.method("listen", &net_server::listen)
		.method("send", &net_server::send)
		.method("broadcast", &net_server::broadcast)
		.method("pump", &net_server::pump)
		.method("kick", &net_server::kick)
		.method("client_count", &net_server::client_count)
		.method("listening", &net_server::listening)
		.method("close", &net_server::close)
		.build();
	jai::dynamic_binder<net_channel>(eng, "NetChannel")
		.constructor<>()
		.method("listen", &net_channel::listen)
		.method("connect", &net_channel::connect)
		.method("send", &net_channel::send)
		.method("pump", &net_channel::pump)
		.method("close", &net_channel::close)
		.method("connected", &net_channel::connected)
		.method("status", &net_channel::status)
		.build();
	// what a host shows its players so they know what to type: the machine's name and its IPv4 addresses
	eng.add_function("net_host_name", []() -> std::string {
		if (!net_ready()) { return ""; }
		char name[256] = {0};
		if (::gethostname(name, sizeof(name) - 1) != 0) { return ""; }
		return name;
	});
	eng.add_function("net_local_addresses", []() -> std::vector<std::string> {
		std::vector<std::string> out;
		if (!net_ready()) { return out; }
		char name[256] = {0};
		if (::gethostname(name, sizeof(name) - 1) != 0) { return out; }
		addrinfo hints{};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		addrinfo* found = nullptr;
		if (::getaddrinfo(name, nullptr, &hints, &found) != 0) { return out; }
		for (addrinfo* a = found; a != nullptr; a = a->ai_next) {
			char text[INET_ADDRSTRLEN] = {0};
			const auto* in4 = reinterpret_cast<const sockaddr_in*>(a->ai_addr);
			if (::inet_ntop(AF_INET, &in4->sin_addr, text, sizeof(text)) == nullptr) { continue; }
			std::string address = text;
			if (address.rfind("127.", 0) == 0) { continue; }
			if (std::find(out.begin(), out.end(), address) == out.end()) { out.push_back(address); }
		}
		::freeaddrinfo(found);
		return out;
	});
}

// ----------------------------------------------------------------- engine ---

std::string trim(const std::string& s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) { return ""; }
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

std::string slurp_file(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) { return ""; }
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

// The reference embedding: engine + backend + budget + full stdlib + host file IO.
std::shared_ptr<jai::engine> make_runner_engine(const options& opt, std::ostream* engine_out) {
	auto eng = jai::engine::make();
	eng->set_backend(opt.backend == "interp" ? jai::backend_type::interpreter : jai::backend_type::vm);
	eng->vm_reference_mode(opt.backend == "vm-reference");
	if (opt.workers) { eng->parallel_thread_count(opt.workers); }
	if (opt.miss_every) {
		if (!jai::engine::miss_injection_available()) { std::fprintf(stderr, "--miss-every: this build carries no miss injection (configure with -DJAISCRIPT_MISS_INJECTION=ON)\n"); std::exit(2); }
		eng->cells_miss_every(opt.miss_every);
	}
	eng->execution_budget(opt.budget);
	eng->parameter_alias(opt.param_alias);
	eng->local_alias(opt.local_alias);
	eng->type_proofs(opt.type_proofs);
	if (!opt.disasm.empty() || !opt.manifest.empty()) {
		auto listing = opt.disasm.empty() ? nullptr : std::make_shared<std::ofstream>(opt.disasm, std::ios::binary | std::ios::trunc);
		auto manifest = opt.manifest.empty() ? nullptr : std::make_shared<std::ofstream>(opt.manifest, std::ios::binary | std::ios::trunc);
		eng->disassembly_sink([listing, manifest](const std::string& text, const std::string& facts) {
			if (listing) { (*listing) << text << "\n"; listing->flush(); }
			if (manifest) { (*manifest) << facts << "\n"; manifest->flush(); }
		});
	}
	eng->set_output_stream(std::shared_ptr<std::ostream>(engine_out ? engine_out : &g_watched_out, [](std::ostream*) {}));
	jai::stdlib::register_all(eng);
	jai::stdlib::register_web(eng);

	// Host file IO — a runner without it can't save anything (same shapes as the example games).
	register_file_io(*eng);
	register_console_game_io(*eng);
	register_net_channel(*eng);
	jai_runner::register_fetch(*eng);
	jai_runner::register_sound(*eng);
	jai_runner::register_http_client(*eng);

	jai::script_value args = jai::script_value::make_array(nullptr, eng.get());
	auto& arg_storage = args.as_array();
	arg_storage.reserve(opt.script_args.size());
	for (const auto& s : opt.script_args) { arg_storage.emplace_back(s, eng.get()); }
	eng->add_global("ARGS", std::move(args));
	return eng;
}

// --------------------------------------------------------------- debugger ---

#ifdef JAISCRIPT_ENABLE_DEBUGGER
// Base 52477 keeps clear of 52472-52474, which embedding hosts commonly take;
// +10 steps like they do. Returns the bound port, 0 if nothing bound.
int attach_debugger(jai::engine& eng, int requested_port) {
	int base = requested_port > 0 ? requested_port : jai::debug::default_port + 5;
	for (int candidate = base; candidate < base + 80; candidate += 10) {
		auto connector = jai::debug::listen(eng, candidate);
		if (connector->port() == candidate) { return candidate; }
		eng.set_debug_connector(nullptr);
	}
	return 0;
}
#endif

int wire_debugger(const options& opt, jai::engine& eng, const console_state& con, std::ostream& out) {
	if (!opt.debug) { return 0; }
#ifdef JAISCRIPT_ENABLE_DEBUGGER
	int port = attach_debugger(eng, opt.debug_port);
	if (port != 0) {
		out << con.dim("debugger: DAP listening on 127.0.0.1:" + std::to_string(port)
			+ " (VS Code: Attach to JaiScript)") << "\n";
	} else {
		out << con.dim("debugger: no free port; running without the listener") << "\n";
	}
	return port;
#else
	(void)eng; (void)con; (void)out;
	return 0;
#endif
}

// ------------------------------------------------------------- file runner ---

struct run_result {
	int exit_code = 0;
	bool ok = true;
	double ms = 0.0;
	std::string error;       // one-line message (stack trace already printed by the caller path)
	std::string stack_trace;
};

// An int result is the exit code; only 0..255 survive every platform, so outside it the code is the low byte, as on POSIX
int result_to_exit_code(const jai::script_value& result) {
	return result.is_int() ? static_cast<int>(static_cast<uint64_t>(result.as_int()) & 0xFF) : 0;
}

// Scripts run as suspendable runs: a sleep hands control back here, and the loop waits on the engine's clock until the run is due
jai::script_value finish_run(jai::engine& eng, jai::script_run run) {
	while (!run.done()) {
		const auto now = eng.time().monotonic_now();
		if (run.wake_at() > now) { eng.time().sleep_for(run.wake_at() - now); }
		run.resume();
	}
	const auto& result = run.result();
	if (!result) { throw std::runtime_error(eng.format_error(result)); }
	return result.value();
}

// execute_file_source keeps the file-path attribution (stack traces, debugger breakpoints)
// while letting the host strip a UTF-8 BOM — Notepad's default, PowerShell's Set-Content —
// which the lexer would otherwise reject as "Unexpected character".
// A load that fails prints its whole listing as the error; one that passes lists only its warnings, here, before it runs.
void print_check_warnings(jai::engine& eng, const std::string& content, const std::string& name) {
	try {
		jai::check_report report = eng.check(content);
		if (!report.diagnostics.empty() && !report.has_errors()) {
			std::fprintf(stderr, "jaiscript: static check warnings:\n%s", report.format(content, name).c_str());
		}
	} catch (const std::exception&) {
	}
}

jai::script_value execute_script_file(jai::engine& eng, const fs::path& abs, bool print_warnings = false) {
	std::string content = slurp_file(abs.string());
	if (content.rfind("\xEF\xBB\xBF", 0) == 0) { content.erase(0, 3); }
	if (print_warnings) { print_check_warnings(eng, content, abs.filename().string()); }
	return finish_run(eng, eng.start_file_source(abs.string(), content));
}

// Runs each -e source in order in `eng` (include/import resolve from the working directory); the first error stops the
// run, and the last source's result gives the exit code as a file's does.
run_result run_sources_in_engine(jai::engine& eng, const std::vector<std::string>& sources, bool print_warnings = false) {
	run_result r;
	eng.add_include_path(fs::current_path().string());
	auto start = std::chrono::steady_clock::now();
	try {
		for (const std::string& source : sources) {
			if (print_warnings) { print_check_warnings(eng, source, "<script>"); }
			r.exit_code = result_to_exit_code(eng.execute(source));
		}
	} catch (const std::exception& ex) {
		r.ok = false;
		r.exit_code = 1;
		r.error = ex.what();
		r.stack_trace = trim(eng.format_stack_trace());
	}
	r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	return r;
}

// Runs `path` in `eng` with include/import resolving from the script's own directory.
run_result run_file_in_engine(jai::engine& eng, const std::string& path, bool print_warnings = false) {
	run_result r;
	std::error_code ec;
	fs::path abs = fs::absolute(path, ec);
	eng.add_include_path(abs.parent_path().string());
	auto start = std::chrono::steady_clock::now();
	try {
		jai::script_value result = execute_script_file(eng, abs, print_warnings);
		r.exit_code = result_to_exit_code(result);
	} catch (const std::exception& ex) {
		r.ok = false;
		r.exit_code = 1;
		r.error = ex.what();
		r.stack_trace = trim(eng.format_stack_trace());
	}
	r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	return r;
}

int run_file_mode(const options& opt, const console_state& con) {
	if (opt.sources.empty() && !fs::exists(opt.script)) {
		std::fprintf(stderr, "%s\n", con.red("jaiscript: no such file: " + opt.script).c_str());
		return 2;
	}
	if (!opt.coverage.empty() && !jai::engine::vm_coverage_available()) {
		std::fprintf(stderr, "%s\n", con.red("jaiscript: --coverage needs a build with JAISCRIPT_VM_COVERAGE (cmake -DJAISCRIPT_VM_COVERAGE=ON)").c_str());
		return 2;
	}
	auto eng = make_runner_engine(opt, nullptr);
	std::ostringstream debug_note;
	wire_debugger(opt, *eng, con, debug_note);
	if (!debug_note.str().empty()) { std::fputs(debug_note.str().c_str(), stderr); }

	if (opt.benchmark_main > 0) { std::cout << "JAI_BENCHMARK_PHASE warmup\n"; }
	run_result r = opt.sources.empty() ? run_file_in_engine(*eng, opt.script, opt.warnings) : run_sources_in_engine(*eng, opt.sources, opt.warnings);
	if (r.ok && r.exit_code == 0 && opt.benchmark_main > 0) {
		for (int repetition = 1; repetition <= opt.benchmark_main; ++repetition) {
			std::cout << "JAI_BENCHMARK_PHASE measured " << repetition << "\n";
			if (!opt.coverage.empty()) { eng->vm_coverage_reset(); }
			try {
#ifdef GLOOM_PROFILE_CAPTURE
				gloom::profile_capture capture(L"JaiDOOM measured main");
				capture.begin();
#endif
				jai::script_value callable = eng->get_variable("main");
				const std::vector<jai::script_value> arguments;
				auto result = callable.as_function()(arguments);
				if (!result) { throw std::runtime_error(eng->format_error(result)); }
				r.exit_code = result_to_exit_code(result.value());
#ifdef GLOOM_PROFILE_CAPTURE
				capture.stop();
#endif
			} catch (const std::exception& ex) {
				r.ok = false;
				r.exit_code = 1;
				r.error = ex.what();
				r.stack_trace = trim(eng->format_stack_trace());
			}
			std::cout << "JAI_BENCHMARK_END " << repetition << " exit " << r.exit_code << "\n";
			if (!r.ok || r.exit_code != 0) { break; }
		}
	}
	std::fflush(stdout);
	restore_terminal_output();   // an error lands on the normal screen
	if (!r.ok) {
		std::fprintf(stderr, "%s\n", con.red("jaiscript: " + r.error).c_str());
		if (!r.stack_trace.empty()) { std::fprintf(stderr, "%s\n", r.stack_trace.c_str()); }
	}
	if (!opt.coverage.empty()) {
		std::ofstream out(opt.coverage, std::ios::binary | std::ios::trunc);
		out << "script " << (opt.sources.empty() ? opt.script : std::string("-e")) << "\n" << eng->vm_coverage_report();
		if (!out) { std::fprintf(stderr, "%s\n", con.red("jaiscript: cannot write " + opt.coverage).c_str()); }
	}
	if (opt.time_runs) {
		char text[64];
		std::snprintf(text, sizeof(text), "[%.3f ms]", r.ms);
		std::fprintf(stderr, "%s\n", con.dim(text).c_str());
	}
	bool pause = opt.pause_mode == 1 || (opt.pause_mode == -1 && con.solo_console);
	if (pause) {
		std::fputs(con.dim("[press Enter to close]").c_str(), stderr);
		std::fputc('\n', stderr);
		std::string line;
		std::getline(std::cin, line);
	}
	return r.exit_code;
}

// ------------------------------------------------------------- watch mode ---

int run_watch_mode(const options& opt, const console_state& con) {
	if (!fs::exists(opt.script)) {
		std::fprintf(stderr, "%s\n", con.red("jaiscript: no such file: " + opt.script).c_str());
		return 2;
	}
	options run_opt = opt;
	run_opt.debug = false;   // fresh engine per run: an attach would drop every save
	int run_count = 0;
	std::error_code ec;
	fs::file_time_type last_mtime = fs::last_write_time(opt.script, ec);
	bool first = true;
	while (true) {   // Ctrl+C ends the runner
		if (!first) {
			fs::file_time_type now_mtime = fs::last_write_time(opt.script, ec);
			if (ec || now_mtime == last_mtime) {
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				continue;
			}
			last_mtime = now_mtime;
		}
		first = false;
		++run_count;
		con.clear_screen();
		run_result r;
		{
			auto eng = make_runner_engine(run_opt, nullptr);   // fresh world every save
			r = run_file_in_engine(*eng, opt.script);
		}
		std::fflush(stdout);
		restore_terminal_output();
		if (!r.ok) {
			std::fprintf(stderr, "%s\n", con.red("jaiscript: " + r.error).c_str());
			if (!r.stack_trace.empty()) { std::fprintf(stderr, "%s\n", r.stack_trace.c_str()); }
		}
		auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm tm_buf{};
#ifdef _WIN32
		localtime_s(&tm_buf, &t);
#else
		localtime_r(&t, &tm_buf);
#endif
		char clock_text[16];
		std::strftime(clock_text, sizeof(clock_text), "%H:%M:%S", &tm_buf);
		char status[512];
		std::snprintf(status, sizeof(status), "[watch] %s  #%d  %.1f ms  %s  %s  (save to re-run, Ctrl+C quits)",
			opt.script.c_str(), run_count, r.ms, r.ok ? "ok" : "ERROR", clock_text);
		std::string line = r.ok ? con.dim(status) : con.red(status);
		std::fprintf(stdout, "%s\n", line.c_str());
		std::fflush(stdout);
	}
}

// ---------------------------------------------------------------- balancer ---

// Tracks whether accumulated REPL input is submittable: all of () [] {} closed, no open
// /* */ block comment, no string/char literal left open at end of line.
struct line_balancer {
	int depth = 0;
	bool in_block_comment = false;
	bool in_string = false;
	char string_delim = '"';

	void reset() { depth = 0; in_block_comment = false; in_string = false; }

	void feed(const std::string& line) {
		for (size_t i = 0; i < line.size(); ++i) {
			char c = line[i];
			if (in_block_comment) {
				if (c == '*' && i + 1 < line.size() && line[i + 1] == '/') { in_block_comment = false; ++i; }
				continue;
			}
			if (in_string) {
				if (c == '\\') { ++i; }
				else if (c == string_delim) { in_string = false; }
				continue;
			}
			switch (c) {
				case '/':
					if (i + 1 < line.size() && line[i + 1] == '/') { return; }   // line comment
					if (i + 1 < line.size() && line[i + 1] == '*') { in_block_comment = true; ++i; }
					break;
				case '"': case '\'': in_string = true; string_delim = c; break;
				case '(': case '[': case '{': ++depth; break;
				case ')': case ']': case '}': if (depth > 0) { --depth; } break;
				default: break;
			}
		}
	}

	bool balanced() const { return depth == 0 && !in_block_comment && !in_string; }
};

// ------------------------------------------------------------------- REPL ---

// A pasted-or-dropped script path: optionally quoted, ends in .jai, exists on disk.
bool looks_like_script_path(const std::string& line, std::string& path_out) {
	std::string t = trim(line);
	if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\''))) {
		t = t.substr(1, t.size() - 2);
	}
	if (t.size() < 5) { return false; }
	std::string ext = t.substr(t.size() - 4);
	for (auto& c : ext) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
	if (ext != ".jai") { return false; }
	std::error_code ec;
	if (!fs::exists(t, ec)) { return false; }
	path_out = t;
	return true;
}

// A pasted path to an existing file that is not a script (a CMakeLists.txt, a .lua): named as a file, never parsed as code
bool looks_like_other_file_path(const std::string& line, std::string& path_out) {
	std::string t = trim(line);
	if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\''))) {
		t = t.substr(1, t.size() - 2);
	}
	if (t.find_first_of("/\\") == std::string::npos) { return false; }
	std::error_code ec;
	if (!fs::is_regular_file(t, ec)) { return false; }
	path_out = t;
	return true;
}

const char* repl_help =
	":help                 this text\n"
	":load <path>          run a .jai file into this session (pasting/dropping a path works too)\n"
	":reset                fresh engine (keeps backend, budget, debugger)\n"
	":backend vm|interp|vm-reference   switch backend (resets the engine)\n"
	":time on|off          print wall time per execute\n"
	":quit / :exit         leave (Ctrl+Z+Enter or Ctrl+C also work)\n"
	"\n"
	"no ';':     a line's last statement needs none (int x = 5, x += 1, import \"f.jai\");\n"
	"            inside {} the language's rule stands.\n"
	"multi-line: input keeps collecting while (), [], {}, /* */ or a string is open\n"
	"            (the prompt becomes ...>); a blank line force-submits what you have.\n"
	"auto-print: a non-null result prints; strings print quoted so \"5\" and 5 differ.\n"
	"budget:     off by default in the REPL (long loops are your business); restart with\n"
	"            --budget=N to restore one, e.g. --budget=1.\n";

struct repl_session {
	options opt;
	const console_state* con = nullptr;
	std::ostream* engine_out = nullptr;    // selftest capture; null = engine default (stdout)
	std::shared_ptr<jai::engine> eng;
	bool show_time = false;
	bool debugger_wired = false;
	bool had_error = false;   // a piped session exits 1 once any input failed

	// The engine holds the connector (jai::debug::listen attaches it), so releasing the old
	// engine frees the port before the new listener binds.
	void build(std::ostream& out, bool announce) {
#ifdef JAISCRIPT_ENABLE_DEBUGGER
		if (eng) { eng->set_debug_connector(nullptr); }
#endif
		eng.reset();
		eng = make_runner_engine(opt, engine_out);
		eng->add_include_path(fs::current_path().string());
		if (opt.debug) {
#ifdef JAISCRIPT_ENABLE_DEBUGGER
			int port = attach_debugger(*eng, opt.debug_port);
			if (port != 0 && announce) {
				out << con->dim("debugger: DAP listening on 127.0.0.1:" + std::to_string(port)
					+ " (VS Code: Attach to JaiScript)") << "\n";
			}
			debugger_wired = port != 0;
#else
			if (announce) { out << con->dim("debugger: this build has JAISCRIPT_ENABLE_DEBUGGER off") << "\n"; }
#endif
		}
	}

	void print_result(const jai::script_value& result, std::ostream& out) {
		if (result.is_null()) { return; }
		if (result.is_string()) { out << '"' << result.to_string() << '"' << "\n"; }
		else if (result.is_char()) { out << '\'' << result.to_string() << '\'' << "\n"; }
		else {
			auto text = eng->text_of(result);   // print's text: a container's elements, a class's own to_string()
			out << (text ? text.value() : eng->format_error(text)) << "\n";
		}
	}

	// A line's last statement needs no ';', as the last expression of an execute needs none: a parse error asking for one
	// retries with it on a line of its own (past a trailing comment), and a retry that does not parse either gives the original
	jai::script_value execute_line(const std::string& source) {
		std::exception_ptr original;
		try {
			return finish_run(*eng, eng->start(source));
		} catch (const jai::parse_error& ex) {
			if (std::string_view(ex.what()).find("expected ';'") == std::string_view::npos) { throw; }
			original = std::current_exception();
		}
		try {
			return finish_run(*eng, eng->start(source + "\n;"));
		} catch (const jai::parse_error&) {
		}
		std::rethrow_exception(original);
	}

	void execute_chunk(const std::string& source, std::ostream& out) {
		auto start = std::chrono::steady_clock::now();
		try {
			jai::script_value result = execute_line(source);
			print_result(result, out);
		} catch (const std::exception& ex) {
			had_error = true;
			std::fflush(stdout);
			restore_terminal_output();
			out << con->red(std::string("error: ") + ex.what()) << "\n";
			std::string trace = trim(eng->format_stack_trace());
			if (!trace.empty()) { out << con->dim(trace) << "\n"; }
		}
		if (show_time) {
			double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			char text[64];
			std::snprintf(text, sizeof(text), "[%.3f ms]", ms);
			out << con->dim(text) << "\n";
		}
	}

	void load_file(const std::string& path, std::ostream& out) {
		if (!fs::exists(path)) {
			had_error = true;
			out << con->red("no such file: " + path) << "\n";
			return;
		}
		auto start = std::chrono::steady_clock::now();
		std::error_code ec;
		fs::path abs = fs::absolute(path, ec);
		eng->add_include_path(abs.parent_path().string());
		try {
			jai::script_value result = execute_script_file(*eng, abs);
			out << con->dim("[loaded " + path + "]") << "\n";
			print_result(result, out);
		} catch (const std::exception& ex) {
			had_error = true;
			std::fflush(stdout);
			restore_terminal_output();
			out << con->red(std::string("error: ") + ex.what()) << "\n";
			std::string trace = trim(eng->format_stack_trace());
			if (!trace.empty()) { out << con->dim(trace) << "\n"; }
		}
		if (show_time) {
			double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			char text[64];
			std::snprintf(text, sizeof(text), "[%.3f ms]", ms);
			out << con->dim(text) << "\n";
		}
	}

	// returns false when the session should end
	bool meta_command(const std::string& line, std::ostream& out) {
		std::istringstream ss(line);
		std::string cmd, arg;
		ss >> cmd;
		std::getline(ss, arg);
		arg = trim(arg);
		if (cmd == ":quit" || cmd == ":exit" || cmd == ":q") { return false; }
		if (cmd == ":help" || cmd == ":h") { out << repl_help; return true; }
		if (cmd == ":time") {
			if (arg == "on") { show_time = true; }
			else if (arg == "off") { show_time = false; }
			out << con->dim(std::string("time: ") + (show_time ? "on" : "off")) << "\n";
			return true;
		}
		if (cmd == ":reset") {
			build(out, true);
			out << con->dim("engine reset (" + opt.backend + " backend)") << "\n";
			return true;
		}
		if (cmd == ":backend") {
			if (arg == "interpreter") { arg = "interp"; }
			if (arg != "vm" && arg != "interp" && arg != "vm-reference") {
				out << con->red("usage: :backend vm|interp|vm-reference (switching resets the engine)") << "\n";
				return true;
			}
			opt.backend = arg;
			build(out, true);
			out << con->dim("backend: " + opt.backend + " (fresh engine)") << "\n";
			return true;
		}
		if (cmd == ":load") {
			if (arg.empty()) { out << con->red("usage: :load <path.jai>") << "\n"; return true; }
			std::string path = arg;
			std::string unquoted;
			if (looks_like_script_path(arg, unquoted)) { path = unquoted; }
			load_file(path, out);
			return true;
		}
		out << con->red("unknown command " + cmd + " (:help lists them)") << "\n";
		return true;
	}

	int run(std::istream& in, std::ostream& out, bool interactive) {
		if (interactive) {
			out << "JaiScript " << jai::version() << " - " << opt.backend
				<< " backend, stdlib loaded, budget "
				<< (opt.budget > 0 ? std::to_string(opt.budget) + "s" : std::string("off"))
				<< ". :help for commands.\n";
		}
		build(out, interactive);
		std::string buffer;
		line_balancer balance;
		while (true) {
			if (interactive) {
				out << (buffer.empty() ? "jai> " : "...> ");
				out.flush();
			}
			std::string line;
			if (!std::getline(in, line)) {
				if (interactive) { out << "\n"; }
				break;   // EOF (Ctrl+Z+Enter, Ctrl+D)
			}
			// piped input arrives with CRLF endings and sometimes a UTF-8 BOM up front
			if (!line.empty() && line.back() == '\r') { line.pop_back(); }
			if (line.rfind("\xEF\xBB\xBF", 0) == 0) { line.erase(0, 3); }
			if (buffer.empty()) {
				std::string t = trim(line);
				if (t.empty()) { continue; }
				if (t[0] == ':') {
					if (!meta_command(t, out)) { break; }
					continue;
				}
				std::string path;
				if (looks_like_script_path(t, path)) {
					load_file(path, out);
					continue;
				}
				if (looks_like_other_file_path(t, path)) {
					out << con->red("'" + path + "' is a file, not a JaiScript script: only a .jai file runs from the prompt") << "\n";
					continue;
				}
			} else if (trim(line).empty()) {
				// blank line in continuation force-submits (escape hatch for a stuck bracket)
				execute_chunk(buffer, out);
				buffer.clear();
				balance.reset();
				continue;
			}
			balance.feed(line);
			buffer += buffer.empty() ? line : "\n" + line;
			if (balance.balanced()) {
				execute_chunk(buffer, out);
				buffer.clear();
				balance.reset();
			}
		}
		return !interactive && had_error ? 1 : 0;
	}
};

int run_repl_mode(const options& opt, const console_state& con) {
	repl_session session;
	session.opt = opt;
	session.con = &con;
	return session.run(std::cin, std::cout, con.in_tty);
}

// --------------------------------------------------------------- selftest ---

int run_selftest(const console_state& con) {
	const console_state captured;   // the sessions below write into strings: no console, so no colors
	int passed = 0, failed = 0;
	auto expect = [&](bool ok, const char* name) {
		if (ok) { ++passed; std::printf("  ok    %s\n", name); }
		else { ++failed; std::printf("  %s  %s\n", con.red("FAIL").c_str(), name); }
	};

	{ // the terminal Ctrl+C or an exit restores, read from the script's escape sequences
		terminal_watch watch;
		watch.read_output("plain [?1049h text", 18);
		expect(!watch.changed(), "terminal watch: text without an escape changes nothing");
		const std::string enter = "\x1b[?1049h\x1b[?25l\x1b[38;5;196m";
		for (char c : enter) { watch.read_output(&c, 1); }   // a sequence split across writes at every byte
		expect(watch.alternate_screen && watch.cursor_hidden && watch.styled, "terminal watch: alternate screen, hidden cursor, colour");
		watch.read_output("\x1b[0m\x1b[?1049;47l\x1b[?25h", 21);
		expect(!watch.changed(), "terminal watch: the reset, a combined leave and the shown cursor restore all three");
	}

	{ // balancer
		line_balancer b;
		b.feed("if (x) {");
		expect(!b.balanced(), "balancer: open brace continues");
		b.feed("}");
		expect(b.balanced(), "balancer: closing brace submits");
		b.reset();
		b.feed("print(\"{ not a brace\");");
		expect(b.balanced(), "balancer: braces inside strings ignored");
		b.reset();
		b.feed("int x = 1; // {");
		expect(b.balanced(), "balancer: line comments ignored");
		b.reset();
		b.feed("/* open comment {");
		expect(!b.balanced(), "balancer: open block comment continues");
		b.feed("} still comment */ 1 + 1");
		expect(b.balanced(), "balancer: closed block comment submits");
		b.reset();
		b.feed("\"unterminated");
		expect(!b.balanced(), "balancer: unterminated string continues");
	}

	// temp workspace WITH SPACES — the drag-drop path shape
	fs::path dir = jai::foundry::test_temp_directory() / "jaiscript selftest dir";
	std::error_code ec;
	fs::create_directories(dir, ec);
	options opt;

	{ // file mode: result int -> exit code, script dir import resolution
		fs::path lib = dir / "answer lib.jai";
		spill_file(lib.string(), "int selftest_answer() { return 6 * 7; }\n");
		fs::path script = dir / "main script.jai";
		spill_file(script.string(), "import \"answer lib.jai\";\nprint(\"selftest file mode\");\nselftest_answer()\n");
		auto eng = make_runner_engine(opt, nullptr);
		std::ostringstream captured;
		eng->set_output_stream(std::shared_ptr<std::ostream>(&captured, [](std::ostream*) {}));
		run_result r = run_file_in_engine(*eng, script.string());
		expect(r.ok, "file: script with spaces-in-path runs");
		expect(r.exit_code == 42, "file: int result becomes the exit code");
		expect(captured.str().find("selftest file mode") != std::string::npos, "file: print output captured");
	}

	{ // file mode: runtime error -> exit 1 + message
		fs::path bad = dir / "boom.jai";
		spill_file(bad.string(), "throw \"boom\";\n");
		auto eng = make_runner_engine(opt, nullptr);
		run_result r = run_file_in_engine(*eng, bad.string());
		expect(!r.ok && r.exit_code == 1, "file: runtime error exits 1");
		expect(r.error.find("boom") != std::string::npos, "file: error message surfaced");
	}

	{ // -e: several sources in order, one engine, combined with the other flags; the last result is the exit code
		const char* argv[] = {"jaiscript", "-e", "int base = 40;", "--backend=interp", "-e", "print(\"e ran\"); base + 2", "--workers=2"};
		options parsed;
		int parse_exit = parse_args(static_cast<int>(std::size(argv)), const_cast<char**>(argv), parsed, con);
		expect(parse_exit == -1 && parsed.sources.size() == 2 && parsed.script.empty(), "-e: each flag adds a source, no script file");
		expect(parsed.backend == "interp" && parsed.workers == 2, "-e: combines with --backend and --workers");
		expect(!parsed.debug, "debug: nothing listens unless asked");
		auto eng = make_runner_engine(parsed, nullptr);
		std::ostringstream captured;
		eng->set_output_stream(std::shared_ptr<std::ostream>(&captured, [](std::ostream*) {}));
		run_result r = run_sources_in_engine(*eng, parsed.sources);
		expect(r.ok && r.exit_code == 42, "-e: sources share one engine and the last int result is the exit code");
		expect(captured.str() == "e ran\n", "-e: only what the source prints is printed");
		run_result failed = run_sources_in_engine(*eng, {"throw \"e boom\";", "print(\"never\");"});
		expect(!failed.ok && failed.exit_code == 1 && failed.error.find("e boom") != std::string::npos, "-e: a script error exits 1");
		expect(captured.str().find("never") == std::string::npos, "-e: the first error stops the run");
	}

	{ // a value flag is the flag alone or flag=value, a malformed number is a usage error, and an int result keeps its low byte
		options parsed;
		auto parse = [&](std::vector<const char*> args) {
			args.insert(args.begin(), "jaiscript");
			parsed = options{};
			return parse_args(static_cast<int>(args.size()), const_cast<char**>(args.data()), parsed, con);
		};
		expect(parse({"--debug", "a.jai"}) == -1 && parsed.debug && parsed.debug_port == 0, "debug: --debug stands up the listener");
		expect(parse({"--debug-port=52490", "a.jai"}) == -1 && parsed.debug && parsed.debug_port == 52490, "debug: --debug-port=N implies --debug");
		expect(parse({"--disasmfoo", "a.jai"}) == 2 && parse({"--backendvm"}) == 2, "flags: a misspelled value flag is unknown, never a prefix");
		expect(parse({"--workers=abc"}) == 2 && parse({"--workers=-1"}) == 2 && parse({"--debug-port=70000"}) == 2 &&
			parse({"--budget=nan"}) == 2 && parse({"--benchmark-main=1x"}) == 2 && parse({"--workers"}) == 2, "flags: a malformed or missing number exits 2");
		expect(parse({"--workers", "3", "--budget=1.5", "--debug-port=52490"}) == -1, "flags: a value follows its flag or its '='");
		auto eng = make_runner_engine(opt, nullptr);
		expect(run_sources_in_engine(*eng, {"300"}).exit_code == 44 && run_sources_in_engine(*eng, {"-1"}).exit_code == 255 &&
			run_sources_in_engine(*eng, {"4294967296"}).exit_code == 0, "exit code: an int result outside 0..255 keeps its low byte");
	}

	{ // REPL scripted session: multi-line, auto-print, typo recovery, meta-commands, path paste
		fs::path pasted = dir / "pasted.jai";
		spill_file(pasted.string(), "int pasted_marker = 777;\nprint(\"pasted ran\");\n");
		fs::path not_script = dir / "CMakeLists.txt";
		spill_file(not_script.string(), "project(x)\n");
		std::ostringstream out;
		std::istringstream in(
			"int a = 2;\n"
			"a + 3\n"
			"int add(int x, int y) {\n"
			"return x + y;\n"
			"}\n"
			"add(4, 5)\n"
			"\"quoted string\"\n"
			"this is a deliberate typo(\n"
			")\n"
			"a + 40\n"
			":time on\n"
			"1 + 1\n"
			":backend interp\n"
			"2 + 2\n"
			"\"" + pasted.string() + "\"\n"
			"pasted_marker\n"
			+ not_script.string() + "\n"
			":reset\n"
			":help\n"
			":quit\n");
		repl_session session;
		session.opt = opt;
		session.con = &captured;
		session.engine_out = &out;
		expect(session.run(in, out, false) == 1, "repl: a piped session with an error exits 1");
		std::string text = out.str();
		expect(text.find("5") != std::string::npos, "repl: expression auto-prints");
		expect(text.find("9") != std::string::npos, "repl: multi-line function defined and called");
		expect(text.find("\"quoted string\"") != std::string::npos, "repl: strings auto-print quoted");
		expect(text.find("error:") != std::string::npos, "repl: typo reports an error");
		expect(text.find("42") != std::string::npos, "repl: engine usable after an error");
		expect(text.find("[0.") != std::string::npos || text.find(" ms]") != std::string::npos, "repl: :time prints a duration");
		expect(text.find("backend: interp") != std::string::npos, "repl: :backend switches");
		expect(text.find("4") != std::string::npos, "repl: post-switch execute works");
		expect(text.find("pasted ran") != std::string::npos, "repl: pasted .jai path runs into the session");
		expect(text.find("777") != std::string::npos, "repl: pasted file's globals visible");
		expect(text.find("is a file, not a JaiScript script") != std::string::npos, "repl: a pasted path to another kind of file is named, not parsed");
		expect(text.find(":backend vm|interp") != std::string::npos, "repl: :help prints");
	}

	{ // a line's last statement needs no ';' (declarations print nothing); inside braces the language's rule stands
		fs::path lib = dir / "semi lib.jai";
		spill_file(lib.string(), "int semi_lib = 31;\n");
		std::ostringstream out;
		std::istringstream in(
			"int y = 5\n"
			"auto s = \"hi\" // a trailing comment\n"
			"y += 1\n"
			"y * 7\n"
			"s + \"!\"\n"
			"import \"" + lib.generic_string() + "\"\n"
			"semi_lib + 11\n"
			"if (y > 0) { int q = 1 }\n"
			"int bad = 1 int worse = 2\n"
			":quit\n");
		repl_session session;
		session.opt = opt;
		session.con = &captured;
		session.engine_out = &out;
		session.run(in, out, false);
		const std::string text = out.str();
		expect(text.rfind("6\n42\n\"hi!\"\n42\nerror:", 0) == 0, "repl: declarations, compound assignment and import need no ';'");
		expect(text.find("1:24: error: expected ';'") != std::string::npos && text.find("1:13: error: expected ';'") != std::string::npos,
			"repl: inside braces and mid-line ';' is still required");
	}

	{ // interactive dressing: banner + prompts (piped/captured, so colors are off)
		std::ostringstream out;
		std::istringstream in("1 + 1\n:quit\n");
		repl_session session;
		session.opt = opt;
		session.con = &captured;
		session.engine_out = &out;
		expect(session.run(in, out, true) == 0, "repl: an interactive session exits 0");
		std::string text = out.str();
		expect(text.find("JaiScript " + jai::version()) != std::string::npos, "repl: banner shows version");
		expect(text.find("jai> ") != std::string::npos, "repl: prompt prints when interactive");
	}

	for (const char* backend : {"interp", "vm"}) {   // read_file's contract and binary data through the real file functions
		options file_opt = opt;
		file_opt.backend = backend;
		auto eng = make_runner_engine(file_opt, nullptr);
		const fs::path bin = dir / (std::string("binary ") + backend + ".bin");
		const fs::path empty = dir / (std::string("empty ") + backend + ".txt");
		const fs::path missing = dir / "no such file.txt";
		spill_file(empty.string(), "");
		eng->add_global("bin_path", jai::script_value(bin.generic_string(), eng.get()));
		eng->add_global("empty_path", jai::script_value(empty.generic_string(), eng.get()));
		eng->add_global("missing_path", jai::script_value(missing.generic_string(), eng.get()));
		eng->add_global("dir_path", jai::script_value(dir.generic_string(), eng.get()));
		auto run = [&](const std::string& src) {
			try { return eng->execute(src).to_string(); } catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
		};
		auto name = [&](const char* what) { return std::string("files [") + backend + "]: " + what; };
		const std::string bytes = "array<int> b = []; for (int i = 0; i < 256; ++i) { b.push(i); } b.push(13); b.push(10); b.push(0); b.push(10); ";
		expect(run(bytes + "write_file(bin_path, string::from_bytes(b)) && read_file(bin_path).to_bytes() == b;") == "true",
			name("every byte value, NUL and \\r\\n round trip").c_str());
		std::string disk = slurp_file(bin.string());
		bool exact = disk.size() == 260 && disk[13] == '\x0D' && disk[256] == '\x0D' && disk[257] == '\x0A' && disk[258] == '\0';
		for (int i = 0; i < 256 && exact; ++i) { exact = static_cast<unsigned char>(disk[i]) == i; }
		expect(exact, name("the file on disk holds exactly those bytes").c_str());
		expect(run("binary::writer w; w.uint32(0xCAFEBABE).text(\"hdr\", 8).float64(-0.5); write_file(bin_path, w.to_string()); "
			"var r = read_file(bin_path).reader(); to_string(r.uint32()) + \" \" + r.text(8) + \" \" + to_string(r.float64()) + \" \" + to_string(r.at_end());") ==
			"3405691582 hdr -0.5 true", name("a binary::writer file reads back through binary::reader").c_str());
		expect(run("read_file(empty_path).size();") == "0", name("an empty file reads as \"\"").c_str());
		expect(run("var m = \"\"; try { read_file(missing_path); } catch (e) { m = e; } m;") == "Cannot read file '" + missing.generic_string() + "': not found",
			name("a missing file raises").c_str());
		expect(run("var m = \"\"; try { read_file(dir_path); } catch (e) { m = e; } m;") == "Cannot read file '" + dir.generic_string() + "': not a file",
			name("a folder raises").c_str());
		expect(run("file_exists(missing_path) ? read_file(missing_path) : \"fallback\";") == "fallback", name("check first, then read").c_str());
	}

	{   // sound: format by header, then by extension; the play state a script reads (played only where an output device exists)
		auto le = [](uint32_t v, int bytes) { std::string out; for (int i = 0; i < bytes; ++i) { out += static_cast<char>((v >> (8 * i)) & 0xFF); } return out; };
		auto silence = [&](uint32_t samples) {   // 8-bit mono at 8 kHz
			return "RIFF" + le(36 + samples, 4) + "WAVEfmt " + le(16, 4) + le(1, 2) + le(1, 2) + le(8000, 4) + le(8000, 4) + le(1, 2) + le(8, 2) +
				"data" + le(samples, 4) + std::string(samples, static_cast<char>(128));
		};
		const std::string wav = silence(80);   // 10 ms
		const std::string track = std::string("\x00\x90\x3C\x64\x60\x80\x3C\x00\x00\xFF\x2F\x00", 12);
		const std::string midi = std::string("MThd\x00\x00\x00\x06\x00\x00\x00\x01\x01\xE0", 14) + "MTrk" + std::string("\x00\x00\x00", 3) + static_cast<char>(track.size()) + track;
		const fs::path sounds = dir / "sounds";
		std::error_code ec;
		fs::create_directories(sounds, ec);
		spill_file((sounds / "tone.wav").string(), wav);
		spill_file((sounds / "long.wav").string(), silence(8000 * 600));   // ten minutes: still playing whatever the machine's load
		spill_file((sounds / "tone.bin").string(), wav);
		spill_file((sounds / "song.mid").string(), midi);
		spill_file((sounds / "tagged.dat").string(), std::string("ID3\x04\x00", 5));
		spill_file((sounds / "noise.mp3").string(), "not a header");
		spill_file((sounds / "notes.txt").string(), "not a sound");
#ifdef _WIN32
		const bool output_device = ::waveOutGetNumDevs() > 0;
#else
		const bool output_device = false;
#endif
		for (const char* backend : {"interp", "vm"}) {
			options sound_opt = opt;
			sound_opt.backend = backend;
			auto eng = make_runner_engine(sound_opt, nullptr);
			eng->add_global("snd", jai::script_value(sounds.generic_string(), eng.get()));
			auto run = [&](const std::string& src) {
				try { return eng->execute(src).to_string(); } catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
			};
			auto name = [&](const char* what) { return std::string("sound [") + backend + "]: " + what; };
#ifdef _WIN32
			expect(run("var f = [\"tone.wav\", \"tone.bin\", \"song.mid\", \"tagged.dat\", \"noise.mp3\"]; var out = \"\"; "
				"for (auto n : f) { out += load_sound(snd + \"/\" + n).format() + \" \"; } out;") == "wav wav midi mp3 mp3 ",
				name("the header names the format, else the extension").c_str());
#endif
			expect(run("load_sound(snd + \"/missing.wav\") == null && load_sound(snd + \"/notes.txt\") == null && play_sound(snd + \"/notes.txt\") == null;") == "true",
				name("a missing file or an unknown format is no sound").c_str());
			if (output_device) {
				expect(run("var s = load_sound(snd + \"/long.wav\"); var seen = to_string(s.playing()) + \" \" + to_string(s.play()) + \" \" + to_string(s.playing()); "
					"s.stop(); seen += \" \" + to_string(s.playing()); s.play(true); seen += \" \" + to_string(s.playing()); stop_sounds(); seen + \" \" + to_string(s.playing());") ==
					"false true true false true false", name("play, stop, a loop and stop_sounds set the play state").c_str());
				expect(run("var s = play_sound(snd + \"/tone.wav\"); int waited = 0; while (s.playing() && waited < 60000) { sleep(0.01); waited += 10; } !s.playing();") == "true",
					name("a sound played once stops playing when it ends").c_str());
			}
		}
	}

	for (const char* backend : {"interp", "vm"}) {
		// Process-owned fixtures only. The invalid URL is rejected before Download
		// starts a worker, and missing audio files cannot produce audible output.
		const fs::path fixture = dir / (std::string("filesystem permission ") + backend);
		auto expect_permission = [&](bool ok, const std::string& name) {
			expect(ok, (std::string("filesystem [") + backend + "]: " + name).c_str());
		};
		try {
			const fs::path content = fixture / "content.txt";
			const fs::path removable = fixture / "removable.txt";
			if (!spill_file(content.string(), "unchanged") || !spill_file(removable.string(), "keep")) {
				throw std::runtime_error("cannot create filesystem permission fixtures");
			}
			options restricted_opt = opt;
			restricted_opt.backend = backend;
			std::ostringstream output;
			auto eng = make_runner_engine(restricted_opt, &output);
			eng->add_global("fixture", jai::script_value(fixture.generic_string(), eng.get()));
			int revoke_calls = 0;
			int guarded_calls = 0;
			eng->add_function("revoke_filesystem", [engine_ptr = eng.get(), &revoke_calls]() {
				++revoke_calls;
				engine_ptr->disable(jai::permissions::filesystem);
			});
			eng->add_function("guarded_probe", [&guarded_calls]() {
				++guarded_calls;
				return true;
			}, jai::permissions::filesystem);
			expect_permission(eng->execute("read_file(fixture + \"/content.txt\");").as_string() == "unchanged",
				"default file access remains enabled");
			eng->execute("var saved_read = read_file; var pending = Download(); var saved_start = reflect::bind(pending, \"start\");");
			eng->execute("revoke_filesystem();");
			expect_permission(revoke_calls == 1 && !eng->is_enabled(jai::permissions::filesystem),
				"host callback revokes subsequent access");
			const std::string denial = "Insufficient permission: filesystem.";
			auto expect_denied = [&](const std::string& expression, const char* name) {
				try {
					const auto result = eng->execute("try { " + expression +
						"; } catch (error) { return error; } return \"not denied\";").as_string();
					expect_permission(result == denial, std::string("denied ") + name);
					if (result != denial) { std::printf("        actual: %s\n", result.c_str()); }
				} catch (const std::exception& error) {
					expect_permission(false, std::string("catchable denial ") + name);
					std::printf("        unexpected: %s\n", error.what());
				}
			};
			const std::pair<const char*, const char*> denied_calls[] = {
				{"read_file(fixture + \"/content.txt\")", "read_file"},
				{"write_file(fixture + \"/content.txt\", \"changed\")", "write_file"},
				{"file_exists(fixture + \"/content.txt\")", "file_exists"},
				{"delete_file(fixture + \"/removable.txt\")", "delete_file"},
				{"move_file(fixture + \"/removable.txt\", fixture + \"/moved.txt\")", "move_file"},
				{"dir_exists(fixture)", "dir_exists"},
				{"make_dirs(fixture + \"/created\")", "make_dirs"},
				{"writable_dir(fixture + \"/probe-created\")", "writable_dir"},
				{"list_dir(fixture)", "list_dir"},
				{"load_sound(fixture + \"/missing.wav\")", "load_sound"},
				{"play_sound(fixture + \"/missing.mid\")", "play_sound"},
				{"crypto::sha256_file(fixture + \"/content.txt\")", "crypto::sha256_file"},
				{"zip_entries(fixture + \"/missing.zip\")", "zip_entries"},
				{"zip_extract(fixture + \"/missing.zip\", \"entry\", fixture + \"/extracted.txt\")", "zip_extract"},
				{"pending.start(\"jaiscript-selftest://never-network\", fixture + \"/download.bin\")", "Download.start"},
				{"saved_read(fixture + \"/content.txt\")", "saved read_file alias"},
				{"saved_start(\"jaiscript-selftest://never-network\", fixture + \"/alias.bin\")", "saved Download.start alias"},
				{"reflect::call(\"read_file\", fixture + \"/content.txt\")", "reflected read_file"},
				{"reflect::invoke(pending, \"start\", \"jaiscript-selftest://never-network\", fixture + \"/reflected.bin\")", "reflected Download.start"}
			};
			for (const auto& [expression, name] : denied_calls) { expect_denied(expression, name); }
			expect_denied("guarded_probe()", "host callback");
			expect_permission(guarded_calls == 0, "denied host callback never entered");
			bool uncaught_denial = false;
			try {
				eng->execute("saved_read(fixture + \"/content.txt\");");
			} catch (const std::exception& error) {
				uncaught_denial = error.what() == denial;
			}
			expect_permission(uncaught_denial, "uncaught denial reads as the caught text (no repeated category)");
			expect_permission(slurp_file(content.string()) == "unchanged", "denied write leaves bytes unchanged");
			expect_permission(fs::exists(removable), "denied delete preserves file");
			expect_permission(!fs::exists(fixture / "created") && !fs::exists(fixture / "probe-created"),
				"denied directory operations create nothing");
			expect_permission(!fs::exists(fixture / "extracted.txt") && !fs::exists(fixture / "download.bin")
				&& !fs::exists(fixture / "alias.bin") && !fs::exists(fixture / "reflected.bin"),
				"denied archive/download operations create nothing");
			expect_permission(eng->execute("!pending.pump() && !pending.done() && !pending.success() && "
				"pending.error() == \"\" && pending.total() == -1 && pending.fraction() == 0;").as_bool(),
				"denied start does not alter Download state; memory queries remain available");
#if !defined(_WIN32) && !defined(JAISCRIPT_RUNNER_HAS_CURL)
			// Only the curl-executable fallback polls the output file for progress.
			expect_denied("pending.received()", "Download.received filesystem fallback");
#else
			expect_permission(eng->execute("pending.received();").as_int() == 0,
				"Download.received remains available when progress is memory-only");
#endif
			expect_permission(eng->execute("pending.cancel(); stop_sounds(); true;").as_bool(),
				"explicit cancel, stop and status remain available");
			expect_permission(output.str().empty(), "denials produce no unexpected script output");
			eng->enable(jai::permissions::filesystem);
			expect_permission(eng->execute("saved_read(fixture + \"/content.txt\");").as_string() == "unchanged",
				"saved callable works after re-enable");
			expect_permission(eng->execute("write_file(fixture + \"/reenabled.txt\", \"enabled\") && "
				"file_exists(fixture + \"/reenabled.txt\");").as_bool()
				&& slurp_file((fixture / "reenabled.txt").string()) == "enabled",
				"reenabled file operations retain normal behavior");
			expect_permission(eng->execute("write_file(fixture + \"/save.tmp\", \"new\") && write_file(fixture + \"/save.txt\", \"old\") && "
				"move_file(fixture + \"/save.tmp\", fixture + \"/save.txt\") && !file_exists(fixture + \"/save.tmp\") && "
				"read_file(fixture + \"/save.txt\") == \"new\" && !move_file(fixture + \"/save.tmp\", fixture + \"/save.txt\");").as_bool(),
				"move_file replaces the destination and answers false for a missing source");
			expect_permission(eng->execute("load_sound(fixture + \"/missing.wav\") == null && play_sound(fixture + \"/missing.mid\") == null;").as_bool(),
				"load_sound and play_sound answer null for a missing file");
			expect_permission(!eng->execute("saved_start(\"jaiscript-selftest://never-network\", "
				"fixture + \"/reenabled-download.bin\");").as_bool()
				&& eng->execute("pending.error();").as_string() == "download: only http and https URLs are supported",
				"reenabled Download alias reaches its original validation without network access");
			expect_permission(eng->execute("guarded_probe();").as_bool() && guarded_calls == 1,
				"reenabled callback enters exactly once");
			eng->disable(jai::permissions::network);
			expect_permission(eng->execute("var m = \"\"; try { auto d = Download(); d.start(\"http://127.0.0.1:1/\", fixture + \"/net.bin\"); } "
				"catch (e) { m = e; } m;").as_string().find("network") != std::string::npos && !fs::exists(fixture / "net.bin"),
				"Download.start needs the network permission too");
			eng->enable(jai::permissions::network);
		} catch (const std::exception& error) {
			expect_permission(false, "fixture/setup or recovery");
			std::printf("        unexpected: %s\n", error.what());
		}
	}

	std::printf("selftest: %d passed, %d failed\n", passed, failed);
	return failed == 0 ? 0 : 1;
}

} // namespace

#ifdef JAISCRIPT_RUNNER_HOST_ONLY
// The language server (tools/lsp) checks scripts against the runner's exact host surface.
std::shared_ptr<jai::engine> jaiscript_runner_host_engine() {
	return make_runner_engine(options{}, nullptr);
}
#else
int main(int argc, char** argv) {
	jai::foundry::route_faults_to_stderr();
	console_state con;
	con.init();

	options opt;
	int parse_exit = parse_args(argc, argv, opt, con);
	if (parse_exit >= 0) { return parse_exit; }

	if (opt.selftest) { return run_selftest(con); }
	install_exit_handlers();
	if (opt.watch) { return runner_exit(run_watch_mode(opt, con)); }
	if (!opt.script.empty() || !opt.sources.empty()) { return runner_exit(run_file_mode(opt, con)); }
	return runner_exit(run_repl_mode(opt, con));
}
#endif
