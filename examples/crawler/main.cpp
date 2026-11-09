// jai_crawler: GRUBWELL's native host, a thin console shell around a JaiScript engine.
// Everything that makes the game (the raycast view, dungeon generation, combat, AI, items, the
// screens) lives in scripts/*.jai. The host gives the script a terminal (raw keys, the console
// size, its colour depth, an alternate screen it restores on every exit), a seeded Rng, file IO
// for saves, and the command-line flags as globals. scripts/crawler.jai is the same contract
// written in script for the standalone runner.

#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#ifdef JAISCRIPT_ENABLE_DEBUGGER
#include <jaiscript/debug/connector.hpp>
#endif

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <jaiscript/testing/unattended.hpp>

namespace {

struct host_options {
	int64_t seed = 1337;
	std::string backend = "vm";
	uint32_t miss_every = 0;     // engine::cells_miss_every (--miss-every N; a build with JAISCRIPT_MISS_INJECTION)
	bool smoke = false;
	bool god = false;
	bool load = false;
	bool quiet = false;          // no frames (smoke implies it)
	bool time_turns = false;     // key->frame latency summary on exit
	int64_t turns = 250;         // smoke turn budget
	int64_t reload_every = 0;    // the smoke pilot hot-reloads the scripts every N turns
	bool anim = false;           // a headless run still builds every animation frame
	std::string colors;          // "24" or "256"; empty = detect
	int64_t screen_cols = 0;     // --screen CxR: lay out as if the console were C x R (headless runs)
	int64_t screen_rows = 0;
	bool ascii = false;          // plain ASCII glyphs, for terminals without Unicode block characters
	std::string scripted_input;  // keys fed to read_key(); empty = interactive
	std::string scripts_dir;
	std::string save_path = "jai_crawler_save.json";
	bool debug = false;          // stand up the DAP debug_connector
	bool debug_wait = false;     // also hold boot until a debugger attaches (--debug-wait)
	int64_t debug_port = 52472;
};

// --time: read_key return -> frame_mark (turn logic) -> draw entry (frame build) -> fwrite+fflush (blit)
struct turn_perf {
	using clock = std::chrono::steady_clock;
	bool enabled = false;
	bool key_pending = false;
	bool frame_pending = false;
	clock::time_point key_stamp{};
	clock::time_point frame_stamp{};
	int turns = 0;
	double logic_sum = 0, logic_max = 0, logic_min = 1e9;
	double build_sum = 0, build_max = 0, build_min = 1e9;
	double blit_sum = 0, blit_max = 0, blit_min = 1e9;

	static double ms_between(clock::time_point a, clock::time_point b) {
		return std::chrono::duration<double, std::milli>(b - a).count();
	}

	static void track(double ms, double& sum, double& lo, double& hi) {
		sum += ms;
		if (ms > hi) { hi = ms; }
		if (ms < lo) { lo = ms; }
	}

	void on_key() {
		if (!enabled) { return; }
		key_stamp = clock::now();
		key_pending = true;
		frame_pending = false;
	}

	void on_frame_mark() {
		if (!enabled || !key_pending) { return; }
		frame_stamp = clock::now();
		frame_pending = true;
	}

	// true when this draw closes a measured turn (the caller then times the blit)
	bool on_draw_begin() {
		if (!enabled || !key_pending) { return false; }
		auto now = clock::now();
		track(frame_pending ? ms_between(key_stamp, frame_stamp) : ms_between(key_stamp, now), logic_sum, logic_min, logic_max);
		track(frame_pending ? ms_between(frame_stamp, now) : 0.0, build_sum, build_min, build_max);
		++turns;
		key_pending = false;
		frame_pending = false;
		return true;
	}

	void on_blit(double ms) { track(ms, blit_sum, blit_min, blit_max); }

	void report() const {
		if (!enabled || turns == 0) { return; }
		std::fprintf(stderr,
			"\n--time over %d key->frame turns (ms):\n"
			"  turn logic   min %7.3f  avg %7.3f  max %8.3f\n"
			"  frame build  min %7.3f  avg %7.3f  max %8.3f\n"
			"  console blit min %7.3f  avg %7.3f  max %8.3f\n"
			"  total        avg %7.3f\n",
			turns,
			logic_min, logic_sum / turns, logic_max,
			build_min, build_sum / turns, build_max,
			blit_min, blit_sum / turns, blit_max,
			(logic_sum + build_sum + blit_sum) / turns);
	}
};

// The terminal the game owns between enter() and restore_terminal(): the alternate screen, a
// hidden cursor and (on POSIX) raw keys. It is given back on a normal exit, a script error,
// Ctrl+C, a closed console and SIGTERM alike; host_log lines written meanwhile print after it.
const char* const k_enter_screen = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H";
const char* const k_leave_screen = "\x1b[0m\x1b[2J\x1b[H\x1b[?25h\x1b[?1049l";
volatile std::sig_atomic_t g_screen_entered = 0;
std::vector<std::string> g_held_log;
#ifndef _WIN32
termios g_saved_termios{};
volatile std::sig_atomic_t g_raw_keys = 0;
#endif

void restore_terminal() {
#ifndef _WIN32
	if (g_raw_keys) {
		tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
		g_raw_keys = 0;
	}
#endif
	if (g_screen_entered) {
		g_screen_entered = 0;
		std::fputs(k_leave_screen, stdout);
	}
	for (const std::string& line : g_held_log) {
		std::fputs(line.c_str(), stdout);
		std::fputc('\n', stdout);
	}
	g_held_log.clear();
	std::fflush(stdout);
}

#ifdef _WIN32
BOOL WINAPI on_console_break(DWORD type) {
	if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
		restore_terminal();
		ExitProcess(130);
	}
	return FALSE;
}
#else
void on_signal(int sig) {
	if (g_raw_keys) { tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios); }
	if (g_screen_entered) {
		ssize_t written = write(STDOUT_FILENO, k_leave_screen, std::strlen(k_leave_screen));
		(void)written;
	}
	std::signal(sig, SIG_DFL);
	std::raise(sig);
}
#endif

// COLORTERM=truecolor|24bit says 24-bit, and so does a Windows console that took VT processing.
// Anything else gets the xterm 256-colour cube.
bool detect_truecolor(bool vt_console) {
#ifdef _WIN32
	char* colorterm = nullptr;
	size_t size = 0;
	std::string value;
	if (_dupenv_s(&colorterm, &size, "COLORTERM") == 0 && colorterm) {
		value = colorterm;
		std::free(colorterm);
	}
	return value == "truecolor" || value == "24bit" || vt_console;
#else
	(void)vt_console;
	const char* colorterm = std::getenv("COLORTERM");
	std::string value = colorterm ? colorterm : "";
	return value == "truecolor" || value == "24bit";
#endif
}

struct console_host {
	bool vt_enabled = false;
	bool quiet = false;
	std::deque<std::string> scripted;
	bool input_was_scripted = false;
	turn_perf perf;

	void init() {
#ifdef _WIN32
		HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD mode = 0;
		if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
			vt_enabled = SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
		}
		SetConsoleOutputCP(CP_UTF8);
		SetConsoleCtrlHandler(on_console_break, TRUE);
#endif
	}

	void enter() {
		std::atexit(restore_terminal);
		if (quiet) { return; }
#ifndef _WIN32
		if (!input_was_scripted && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_saved_termios) == 0) {
			termios raw = g_saved_termios;
			raw.c_lflag &= ~(ICANON | ECHO);   // ISIG stays: Ctrl+C raises SIGINT, which restores and exits
			raw.c_cc[VMIN] = 1;
			raw.c_cc[VTIME] = 0;
			tcsetattr(STDIN_FILENO, TCSANOW, &raw);
			g_raw_keys = 1;
		}
		std::signal(SIGINT, on_signal);
		std::signal(SIGTERM, on_signal);
#endif
		g_screen_entered = 1;
		std::fputs(k_enter_screen, stdout);
		std::fflush(stdout);
	}

	void draw(const std::string& frame) {
		if (quiet) { return; }
		bool measured = perf.on_draw_begin();
		auto blit_start = turn_perf::clock::now();
		std::string out;
		out.reserve(frame.size() + 8);
		out += "\x1b[H";
		out += frame;
		out += "\x1b[0m";
		std::fwrite(out.data(), 1, out.size(), stdout);
		std::fflush(stdout);
		if (measured) { perf.on_blit(turn_perf::ms_between(blit_start, turn_perf::clock::now())); }
	}

	void log(const std::string& line) {
		if (g_screen_entered) {
			g_held_log.push_back(line);
			return;
		}
		std::fputs(line.c_str(), stdout);
		std::fputc('\n', stdout);
		std::fflush(stdout);
	}

	std::string read_key() {
		std::string key = next_key(-1.0);
		perf.on_key();
		return key;
	}

	// a key, or "" when none arrives within the timeout; scripted input never waits
	std::string poll_key(double seconds) {
		std::string key = next_key(seconds < 0.0 ? 0.0 : seconds);
		if (!key.empty()) { perf.on_key(); }
		return key;
	}

	// timeout < 0 blocks until a key
	std::string next_key(double timeout) {
		if (input_was_scripted) {
			if (scripted.empty()) { return "Q"; }   // the script ran out: hard quit
			std::string key = scripted.front();
			scripted.pop_front();
			if (key == ";") { return "enter"; }
			if (key == "^") { return "esc"; }
			return key;
		}
		auto deadline = turn_perf::clock::now()
			+ std::chrono::duration_cast<turn_perf::clock::duration>(std::chrono::duration<double>(timeout < 0.0 ? 0.0 : timeout));
		while (timeout < 0.0 || key_waiting(deadline)) {
			std::string key = decode_key();
			if (!key.empty()) { return key; }
		}
		return "";
	}

#ifdef _WIN32
	bool key_waiting(turn_perf::clock::time_point deadline) {
		while (!_kbhit()) {
			if (turn_perf::clock::now() >= deadline) { return false; }
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
		return true;
	}

	std::string decode_key() {
		int c = _getch();
		if (c == 0 || c == 0xE0) {
			switch (_getch()) {
				case 72: return "up";
				case 80: return "down";
				case 75: return "left";
				case 77: return "right";
				default: return "";
			}
		}
		switch (c) {
			case 3: restore_terminal(); std::exit(130);
			case 13: return "enter";
			case 27: return "esc";
			case 8: return "backspace";
			case 9: return "tab";
			default: break;
		}
		if (c >= 32 && c < 127) { return std::string(1, static_cast<char>(c)); }
		return "";
	}
#else
	static bool byte_within(unsigned char& b, int ms) {
		pollfd fd{STDIN_FILENO, POLLIN, 0};
		if (poll(&fd, 1, ms) <= 0) { return false; }
		return read(STDIN_FILENO, &b, 1) == 1;
	}

	bool key_waiting(turn_perf::clock::time_point deadline) {
		auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - turn_perf::clock::now()).count();
		pollfd fd{STDIN_FILENO, POLLIN, 0};
		return poll(&fd, 1, left < 0 ? 0 : static_cast<int>(left)) > 0;
	}

	// an arrow is ESC [ A..D (or ESC O A..D in application cursor mode); a lone ESC is the escape key
	std::string decode_key() {
		unsigned char b = 0;
		if (read(STDIN_FILENO, &b, 1) != 1) { return "Q"; }   // stdin closed
		if (b == 0x1B) {
			unsigned char b2 = 0;
			if (!byte_within(b2, 25) || (b2 != '[' && b2 != 'O')) { return "esc"; }
			unsigned char fin = 0;
			bool have = byte_within(fin, 25);
			while (have && b2 == '[' && fin >= 0x20 && fin <= 0x3F) { have = byte_within(fin, 25); }
			if (!have) { return ""; }
			switch (fin) {
				case 'A': return "up";
				case 'B': return "down";
				case 'C': return "right";
				case 'D': return "left";
				default: return "";
			}
		}
		if (b == '\n' || b == '\r') { return "enter"; }
		if (b == '\t') { return "tab"; }
		if (b == 127 || b == 8) { return "backspace"; }
		if (b >= 32 && b < 127) { return std::string(1, static_cast<char>(b)); }
		return "";
	}
#endif

	static jai::script_int columns() {
#ifdef _WIN32
		CONSOLE_SCREEN_BUFFER_INFO info;
		if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) { return info.srWindow.Right - info.srWindow.Left + 1; }
#else
		winsize ws{};
		if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) { return ws.ws_col; }
#endif
		return 80;
	}

	static jai::script_int rows() {
#ifdef _WIN32
		CONSOLE_SCREEN_BUFFER_INFO info;
		if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) { return info.srWindow.Bottom - info.srWindow.Top + 1; }
#else
		winsize ws{};
		if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) { return ws.ws_row; }
#endif
		return 25;
	}
};

// Deterministic xorshift64*, the only randomness the game rules touch. Bound as script class Rng
// so a seed replays a run exactly.
struct crawler_rng {
	uint64_t s;

	explicit crawler_rng(jai::script_int seed)
		: s(seed != 0 ? static_cast<uint64_t>(seed) : 0x9E3779B97F4A7C15ull) {
		for (int i = 0; i < 4; ++i) { next_raw(); }
	}

	uint64_t next_raw() {
		s ^= s >> 12;
		s ^= s << 25;
		s ^= s >> 27;
		return s * 0x2545F4914F6CDD1Dull;
	}

	jai::script_int next(jai::script_int n) {
		if (n <= 0) { return 0; }
		return static_cast<jai::script_int>(next_raw() % static_cast<uint64_t>(n));
	}

	jai::script_int roll(jai::script_int lo, jai::script_int hi) {
		if (hi <= lo) { return lo; }
		return lo + next(hi - lo + 1);
	}

	bool chance(jai::script_int percent) { return next(100) < percent; }

	// masked to 62 bits so the value survives a script-side int64 round trip through JSON
	jai::script_int state() const { return static_cast<jai::script_int>(s & 0x3FFFFFFFFFFFFFFFull); }
	void set_state(jai::script_int v) {
		s = static_cast<uint64_t>(v);
		if (s == 0) { s = 0x9E3779B97F4A7C15ull; }
	}
};

// the file's bytes; a missing or unreadable one raises the runner's catchable
// "Cannot read file '<path>': <reason>" (tools/jaiscript/README.md)
std::string read_file_or_raise(const std::string& path) {
	std::error_code ec;
	const std::filesystem::file_status st = std::filesystem::status(path, ec);
	const char* reason = nullptr;
	if (st.type() == std::filesystem::file_type::not_found) { reason = "not found"; }
	else if (ec) { reason = ec == std::errc::permission_denied ? "permission denied" : "cannot be opened"; }
	else if (!std::filesystem::is_regular_file(st)) { reason = "not a file"; }
	if (!reason) {
		errno = 0;
		std::ifstream in(path, std::ios::binary);
		if (!in) {
			reason = errno == EACCES ? "permission denied" : "cannot be opened";
		} else {
			std::ostringstream ss;
			ss << in.rdbuf();
			if (!in.bad()) { return ss.str(); }
			reason = "read failed";
		}
	}
	throw std::runtime_error("Cannot read file '" + path + "': " + reason);
}

bool spill_file(const std::string& path, const std::string& content) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) { return false; }
	out.write(content.data(), static_cast<std::streamsize>(content.size()));
	return out.good();
}

std::string locate_scripts_dir(const std::string& override_dir, const char* argv0) {
	namespace fs = std::filesystem;
	if (!override_dir.empty()) { return override_dir; }
#ifdef JAI_CRAWLER_SOURCE_SCRIPTS_DIR
	if (fs::exists(fs::path(JAI_CRAWLER_SOURCE_SCRIPTS_DIR) / "main.jai")) {
		return JAI_CRAWLER_SOURCE_SCRIPTS_DIR;
	}
#endif
	fs::path exe_dir = fs::absolute(fs::path(argv0)).parent_path();
	fs::path local = exe_dir / "crawler_scripts";
	if (fs::exists(local / "main.jai")) { return local.string(); }
	return "scripts";
}

#ifdef JAISCRIPT_ENABLE_DEBUGGER
// The connector listens for the engine's whole lifetime, so VS Code can attach at any time;
// wait_to_boot (--debug-wait) also holds boot until Enter, so boot-time breakpoints catch.
void start_debugger(jai::engine& eng, int port, bool wait_to_boot) {
	jai::debug::listen(eng, port);
	std::fprintf(stderr, "[jai debug] listening on 127.0.0.1:%d - attach from VS Code any time.\n", port);
	if (wait_to_boot) {
		std::fprintf(stderr, "[jai debug] --debug-wait: attach + set breakpoints, then press Enter to boot.\n> ");
		std::fflush(stderr);
		std::string line;
		std::getline(std::cin, line);
	}
	std::fflush(stderr);
}
#endif

void print_usage() {
	std::puts(
		"jai_crawler - GRUBWELL, a first-person dungeon crawler written in JaiScript\n"
		"  --seed N        dungeon seed (default 1337)\n"
		"  --load          continue the saved run\n"
		"  --god           tourist mode: no damage, monstrous stats\n"
		"  --colors 24|256 colour depth (default: 24-bit when COLORTERM says so or on a Windows console)\n"
		"  --ascii         plain ASCII glyphs, for terminals without Unicode block characters\n"
		"  --smoke         headless deterministic autoplay; prints a state hash\n"
		"  --turns N       smoke turn budget (default 250)\n"
		"  --reload-every N  the smoke pilot hot-reloads the scripts every N turns\n"
		"  --anim          a headless run still builds every animation frame\n"
		"  --screen CxR    lay out as if the console were C x R (with --smoke)\n"
		"  --input KEYS    scripted keys (e.g. \"n11;wwdwg>\"; ';'=enter '^'=esc)\n"
		"  --quiet         no frames (useful with --input)\n"
		"  --time          key->frame latency summary on exit\n"
		"  --save PATH     save file (default jai_crawler_save.json)\n"
		"  --scripts DIR   scripts directory\n"
		"  --backend B     vm (the only backend this host links)\n"
		"  --debug         listen for a VS Code debugger for the whole run\n"
		"  --debug-wait    like --debug, and hold boot until you attach + press Enter\n"
		"  --debug-port N  --debug on port N (default 52472)\n");
}

} // namespace

int main(int argc, char** argv) {
	jai::foundry::route_faults_to_stderr();
	host_options opt;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto next_arg = [&](const char* flag) -> std::string {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "missing value for %s\n", flag);
				std::exit(2);
			}
			return argv[++i];
		};
		if (a == "--seed") { opt.seed = std::stoll(next_arg("--seed")); }
		else if (a == "--backend") { opt.backend = next_arg("--backend"); }
		else if (a == "--smoke") { opt.smoke = true; }
		else if (a == "--turns") { opt.turns = std::stoll(next_arg("--turns")); }
		else if (a == "--miss-every") { opt.miss_every = static_cast<uint32_t>(std::stoll(next_arg("--miss-every"))); }
		else if (a == "--input") { opt.scripted_input = next_arg("--input"); }
		else if (a == "--quiet") { opt.quiet = true; }
		else if (a == "--time") { opt.time_turns = true; }
		else if (a == "--reload-every") { opt.reload_every = std::stoll(next_arg("--reload-every")); }
		else if (a == "--anim") { opt.anim = true; }
		else if (a == "--screen") {
			const std::string size = next_arg("--screen");
			const auto x = size.find('x');
			if (x == std::string::npos) {
				std::fprintf(stderr, "--screen takes CxR, e.g. 135x37\n");
				return 2;
			}
			opt.screen_cols = std::stoll(size.substr(0, x));
			opt.screen_rows = std::stoll(size.substr(x + 1));
		}
		else if (a == "--dev") { /* hot reload is always on; accepted for muscle memory */ }
		else if (a == "--god") { opt.god = true; }
		else if (a == "--load") { opt.load = true; }
		else if (a == "--colors") { opt.colors = next_arg("--colors"); }
		else if (a == "--ascii") { opt.ascii = true; }
		else if (a == "--save") { opt.save_path = next_arg("--save"); }
		else if (a == "--scripts") { opt.scripts_dir = next_arg("--scripts"); }
		else if (a == "--debug") { opt.debug = true; }
		else if (a == "--debug-wait") { opt.debug = true; opt.debug_wait = true; }
		else if (a == "--debug-port") { opt.debug = true; opt.debug_port = std::stoll(next_arg("--debug-port")); }
		else if (a == "--help" || a == "-h") { print_usage(); return 0; }
		else { std::fprintf(stderr, "unknown flag: %s\n", a.c_str()); print_usage(); return 2; }
	}
	if (!opt.colors.empty() && opt.colors != "24" && opt.colors != "256") {
		std::fprintf(stderr, "--colors takes 24 or 256\n");
		return 2;
	}
	if (opt.backend != "vm") {
		std::fprintf(stderr, "unknown backend: %s\n", opt.backend.c_str());
		return 2;
	}

	console_host console;
	console.quiet = opt.smoke || opt.quiet;
	console.perf.enabled = opt.time_turns;
	if (!opt.scripted_input.empty()) {
		console.input_was_scripted = true;
		for (char c : opt.scripted_input) { console.scripted.emplace_back(1, c); }
	}
	console.init();
	bool truecolor = opt.colors.empty() ? detect_truecolor(console.vt_enabled) : opt.colors == "24";

	auto eng = jai::engine::make();
	if (opt.miss_every) { eng->cells_miss_every(opt.miss_every); }
	eng->execution_budget(0.0);  // the game blocks on read_key(); wall-clock budgets don't apply
	jai::stdlib::register_all(eng);

	std::string scripts_dir = locate_scripts_dir(opt.scripts_dir, argv[0]);
	eng->add_include_path(scripts_dir);
	eng->set_import_behavior(jai::engine::import_behavior::always);

	eng->add_function("draw", [&console](const std::string& frame) { console.draw(frame); });
	eng->add_function("read_key", [&console]() -> std::string { return console.read_key(); });
	eng->add_function("poll_key", [&console](double seconds) -> std::string { return console.poll_key(seconds); });
	eng->add_function("frame_mark", [&console]() { console.perf.on_frame_mark(); });
	eng->add_function("term_cols", []() -> jai::script_int { return console_host::columns(); });
	eng->add_function("term_rows", []() -> jai::script_int { return console_host::rows(); });
	eng->add_function("host_log", [&console](const std::string& line) { console.log(line); });

	eng->add_function("read_file", [](const std::string& path) -> std::string { return read_file_or_raise(path); });
	eng->add_function("write_file", [](const std::string& path, const std::string& content) -> bool {
		return spill_file(path, content);
	});
	eng->add_function("file_exists", [](const std::string& path) -> bool {
		return std::filesystem::exists(path);
	});
	eng->add_function("delete_file", [](const std::string& path) -> bool {
		std::error_code ec;
		return std::filesystem::remove(path, ec);
	});
	// a script file's size and write time, "" when it is missing: the hot-reload watcher's view of it
	eng->add_function("file_stamp", [scripts_dir](const std::string& name) -> std::string {
		std::error_code ec;
		const std::filesystem::path path = std::filesystem::path(scripts_dir) / name;
		const auto size = std::filesystem::file_size(path, ec);
		if (ec) { return ""; }
		const auto written = std::filesystem::last_write_time(path, ec);
		if (ec) { return ""; }
		return std::to_string(size) + ":" + std::to_string(static_cast<long long>(written.time_since_epoch().count()));
	});

	jai::dynamic_binder<crawler_rng>(*eng, "Rng")
		.constructor<jai::script_int>()
		.method("next", &crawler_rng::next)
		.method("roll", &crawler_rng::roll)
		.method("chance", &crawler_rng::chance)
		.method("state", &crawler_rng::state)
		.method("set_state", &crawler_rng::set_state)
		.build();

	eng->add_global("HOST_SEED", eng->make_value(opt.seed));
	eng->add_global("HOST_SMOKE", eng->make_value(opt.smoke));
	eng->add_global("HOST_TURNS", eng->make_value(opt.turns));
	eng->add_global("HOST_RELOAD_EVERY", eng->make_value(opt.reload_every));
	eng->add_global("HOST_ANIM", eng->make_value(opt.anim));
	eng->add_global("HOST_GOD", eng->make_value(opt.god));
	eng->add_global("HOST_LOAD", eng->make_value(opt.load));
	eng->add_global("HOST_SCRIPTED", eng->make_value(console.input_was_scripted));
	eng->add_global("HOST_INTERACTIVE", eng->make_value(!console.quiet && !console.input_was_scripted));
	eng->add_global("HOST_TRUECOLOR", eng->make_value(truecolor));
	eng->add_global("HOST_ASCII", eng->make_value(opt.ascii));
	eng->add_global("SAVE_PATH", eng->make_value(opt.save_path));
	eng->add_global("HOST_BACKEND", eng->make_value(opt.backend));

	if (opt.debug) {
#ifdef JAISCRIPT_ENABLE_DEBUGGER
		start_debugger(*eng, static_cast<int>(opt.debug_port), opt.debug_wait);
#else
		std::fprintf(stderr, "--debug needs a build with -DJAISCRIPT_ENABLE_DEBUGGER=ON\n");
#endif
	}
	console.enter();

	try {
		eng->execute("import \"main.jai\";");
		if (opt.screen_cols > 0) {
			eng->execute("SCREEN_COLS = " + std::to_string(opt.screen_cols) + "; SCREEN_ROWS = " + std::to_string(opt.screen_rows) + ";");
		}
		eng->execute("crawler_main();");
	} catch (const std::exception& ex) {
		restore_terminal();
		std::fprintf(stderr, "\njai_crawler: script error: %s\n", ex.what());
		std::string trace = eng->format_stack_trace();
		if (!trace.empty()) { std::fprintf(stderr, "%s\n", trace.c_str()); }
		return 1;
	}
	restore_terminal();
	console.perf.report();
	return 0;
}
