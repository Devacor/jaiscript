// demoreel shared host implementation: a deliberately thin console shell around a
// script_adapter. Every visual effect lives in the scene scripts; the host only
// provides a VT console, a monotonic clock, non-blocking keys, the seeded rng, the
// frame pump and the harnesses:
//
//   r    hot-reloads the scene scripts when the session supports it
//   --smoke runs N deterministic frames on every backend and compares frame hashes
//   --perf runs N frames of every scene mid-flight and reports ms/frame + a hash per scene

#include "demoreel_host.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <conio.h>
#pragma comment(lib, "winmm.lib")
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace demoreel {

double now_seconds() {
	using clock = std::chrono::steady_clock;
	static const clock::time_point start = clock::now();
	return std::chrono::duration<double>(clock::now() - start).count();
}

uint64_t fnv1a(uint64_t hash, const std::string& bytes) {
	for (unsigned char c : bytes) {
		hash ^= c;
		hash *= 1099511628211ull;
	}
	return hash;
}

std::string slurp_file(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) { return ""; }
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

std::string locate_scenes_dir(const std::string& override_dir, const char* argv0,
                              const char* source_scenes_dir, const char* local_dirname,
                              const char* entry_file) {
	namespace fs = std::filesystem;
	if (!override_dir.empty()) { return override_dir; }
	if (source_scenes_dir && *source_scenes_dir &&
	    fs::exists(fs::path(source_scenes_dir) / entry_file)) {
		return source_scenes_dir;
	}
	fs::path exe_dir = fs::absolute(fs::path(argv0)).parent_path();
	fs::path local = exe_dir / local_dirname;
	if (fs::exists(local / entry_file)) { return local.string(); }
	return local_dirname;
}

namespace {

// ---- VT output filter: trims escape bytes host-side so every scene benefits.
// The scripts already emit a color escape only when the palette INDEX changes;
// this pass merges cells whose COLORS are indistinguishable. A sequence is only
// dropped when reapplying it would leave terminal state unchanged (SGR is
// idempotent), so mode 1 is exact within `tol` per channel and mode 2 is exact
// within the xterm-256 cube quanta.

struct sgr_color {
	int kind = 0;   // 0 unknown, 1 default, 2 rgb, 3 indexed
	int r = 0, g = 0, b = 0, idx = 0;

	bool matches(const sgr_color& o, int tol) const {
		if (kind != o.kind) { return false; }
		switch (kind) {
			case 1: return true;
			case 2: return std::abs(r - o.r) <= tol && std::abs(g - o.g) <= tol && std::abs(b - o.b) <= tol;
			case 3: return idx == o.idx;
			default: return false;
		}
	}
};

struct sgr_state {
	sgr_color fg, bg;
};

int cube216_axis(int v) {
	if (v < 48) { return 0; }
	if (v < 115) { return 1; }
	return (v - 35) / 40;
}

int cube216_index(int r, int g, int b) {
	return 16 + 36 * cube216_axis(r) + 6 * cube216_axis(g) + cube216_axis(b);
}

// One pass over `in`. Consecutive SGR sequences form a pending group applied to
// `target`; the next glyph settles it: dropped if the live state already matches,
// emitted otherwise (rewritten to 38;5/48;5 in mode 2). Non-SGR escapes pass
// through untouched (they don't affect colors). mode 0 is byte-exact passthrough.
std::string filter_vt(const std::string& in, sgr_state& st, int mode, int tol) {
	if (mode == 0) { return in; }
	std::string out;
	out.reserve(in.size());
	std::string pending;
	sgr_state target = st;
	const int effective_tol = (mode == 2) ? 0 : tol;

	auto flush = [&]() {
		if (pending.empty()) { return; }
		if (target.fg.matches(st.fg, effective_tol) && target.bg.matches(st.bg, effective_tol)) {
			target = st;    // dropped: rewind to the live state
		} else {
			out += pending;
			st = target;
		}
		pending.clear();
	};

	const size_t n = in.size();
	size_t i = 0;
	while (i < n) {
		if (in[i] == '\x1b' && i + 1 < n && in[i + 1] == '[') {
			size_t j = i + 2;
			while (j < n && !(in[j] >= '@' && in[j] <= '~')) { ++j; }
			if (j >= n) { flush(); out.append(in, i, n - i); break; }
			if (in[j] != 'm') {
				flush();
				out.append(in, i, j - i + 1);
				i = j + 1;
				continue;
			}
			int params[16];
			int np = 0;
			bool too_many = false;
			int v = 0;
			for (size_t k = i + 2; k <= j; ++k) {
				char c = in[k];
				if (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); }
				else { if (np < 16) { params[np++] = v; } else { too_many = true; } v = 0; }
			}
			std::string rewritten = "\x1b[";
			bool first = true;
			auto put = [&](int pv) {
				if (!first) { rewritten += ';'; }
				rewritten += std::to_string(pv);
				first = false;
			};
			bool opaque = too_many;
			int p = 0;
			while (p < np) {
				int c = params[p];
				if (c == 0) { target.fg = {1}; target.bg = {1}; put(0); ++p; }
				else if ((c == 38 || c == 48) && p + 4 < np && params[p + 1] == 2) {
					sgr_color col;
					if (mode == 2) {
						col.kind = 3;
						col.idx = cube216_index(params[p + 2], params[p + 3], params[p + 4]);
						put(c); put(5); put(col.idx);
					} else {
						col.kind = 2;
						col.r = params[p + 2]; col.g = params[p + 3]; col.b = params[p + 4];
						put(c); put(2); put(col.r); put(col.g); put(col.b);
					}
					(c == 38 ? target.fg : target.bg) = col;
					p += 5;
				}
				else if ((c == 38 || c == 48) && p + 2 < np && params[p + 1] == 5) {
					(c == 38 ? target.fg : target.bg) = {3, 0, 0, 0, params[p + 2]};
					put(c); put(5); put(params[p + 2]);
					p += 3;
				}
				else if ((c >= 30 && c <= 37) || (c >= 90 && c <= 97)) { target.fg = {3, 0, 0, 0, 1000 + c}; put(c); ++p; }
				else if (c == 39) { target.fg = {1}; put(c); ++p; }
				else if ((c >= 40 && c <= 47) || (c >= 100 && c <= 107)) { target.bg = {3, 0, 0, 0, 1000 + c}; put(c); ++p; }
				else if (c == 49) { target.bg = {1}; put(c); ++p; }
				else { opaque = true; put(c); ++p; }
			}
			rewritten += 'm';
			if (opaque) { target.fg.kind = 0; target.bg.kind = 0; }   // never dedupe what we don't model
			pending += rewritten;
			i = j + 1;
			continue;
		}
		flush();
		out += in[i++];
	}
	flush();
	return out;
}

// --ascii: terminals without the block glyphs (the Linux text console) get one
// character per cell, its density from the cell's two pixels' mean brightness and
// its color their mean. Rows carrying text (title handles, captions, the HUD)
// pass through as they are.
struct rgb_color {
	int r = 0, g = 0, b = 0;
};

std::string ascii_row(const std::string& row) {
	bool text_row = false;
	for (size_t i = 0; i < row.size(); ++i) {
		if (row[i] == '\x1b') {
			while (i < row.size() && !(row[i] >= '@' && row[i] <= '~' && row[i] != '[')) { ++i; }
			continue;
		}
		unsigned char c = static_cast<unsigned char>(row[i]);
		if (c > ' ' && c < 127) { text_row = true; break; }
	}
	if (text_row) { return row; }

	static const char ramp[] = " .:-=+*#%@";
	std::string out;
	out.reserve(row.size());
	rgb_color fg{229, 229, 229};
	rgb_color bg;
	int last_key = -1;
	auto cell = [&](const rgb_color& top, const rgb_color& bottom) {
		int r = (top.r + bottom.r) / 2, g = (top.g + bottom.g) / 2, b = (top.b + bottom.b) / 2;
		int luma = (r * 299 + g * 587 + b * 114) / 1000;
		// density carries the brightness (square-root ramp, so dark scenes keep their shapes)
		// and the color only the hue, at full value
		int glyph = static_cast<int>(std::sqrt(luma / 255.0) * 9.99);
		int peak = std::max(r, std::max(g, b));
		int key = glyph == 0 ? 0 : (r << 16 | g << 8 | b) + 1;
		if (key != last_key && glyph != 0) {
			out += "\x1b[38;2;" + std::to_string(r * 255 / peak) + ';' + std::to_string(g * 255 / peak) + ';'
				+ std::to_string(b * 255 / peak) + 'm';
			last_key = key;
		}
		out += ramp[glyph];
	};
	out += "\x1b[48;2;0;0;0m";
	size_t i = 0;
	while (i < row.size()) {
		if (row[i] == '\x1b' && i + 1 < row.size() && row[i + 1] == '[') {
			size_t j = i + 2;
			int params[16];
			int np = 0;
			int v = 0;
			while (j < row.size() && !(row[j] >= '@' && row[j] <= '~')) {
				if (row[j] >= '0' && row[j] <= '9') { v = v * 10 + (row[j] - '0'); }
				else if (np < 16) { params[np++] = v; v = 0; }
				++j;
			}
			if (j < row.size() && row[j] == 'm') {
				if (np < 16) { params[np++] = v; }
				for (int p = 0; p < np; ++p) {
					if (params[p] == 0) { fg = {229, 229, 229}; bg = {}; }
					else if ((params[p] == 38 || params[p] == 48) && p + 4 < np && params[p + 1] == 2) {
						(params[p] == 38 ? fg : bg) = {params[p + 2], params[p + 3], params[p + 4]};
						p += 4;
					}
				}
			}
			i = j + 1;
			continue;
		}
		if (row.compare(i, 3, "\xE2\x96\x80") == 0) { cell(fg, bg); i += 3; }   // upper half block
		else if (row.compare(i, 3, "\xE2\x96\x88") == 0) { cell(fg, fg); i += 3; }   // full block
		else if (row[i] == ' ') { cell(bg, bg); ++i; }
		else { ++i; }
	}
	return out + "\x1b[0m";
}

std::string ascii_frame(const std::string& frame) {
	std::string out;
	out.reserve(frame.size());
	size_t start = 0;
	for (;;) {
		size_t nl = frame.find('\n', start);
		out += ascii_row(frame.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
		if (nl == std::string::npos) { break; }
		out += '\n';
		start = nl + 1;
	}
	return out;
}

// Ctrl+C, Ctrl+Break, SIGTERM and SIGHUP end the interactive loop through the
// normal exit, so the console comes back with its cursor, colors and main screen.
std::atomic<bool> g_quit_requested{false};

#ifdef _WIN32
BOOL WINAPI console_ctrl_handler(DWORD type) {
	if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
		g_quit_requested.store(true);
		return TRUE;
	}
	return FALSE;
}
#else
void posix_quit_signal(int) { g_quit_requested.store(true); }
#endif

// COLORTERM=truecolor|24bit and Windows Terminal get 24-bit color; everything
// else (macOS Terminal.app, the Linux console, legacy conhost) the xterm-256 cube.
bool terminal_has_truecolor() {
	const char* colorterm = std::getenv("COLORTERM");
	if (colorterm) {
		std::string value = colorterm;
		if (value == "truecolor" || value == "24bit") { return true; }
	}
	return std::getenv("WT_SESSION") != nullptr;
}

struct console_host {
	bool active = false;
	bool is_console = false;   // stdout is a real console (not redirected)
	bool is_wt = false;        // Windows Terminal (fast VT) vs legacy conhost
	int mode = 1;              // filter mode (see filter_vt)
	int tol = 8;
	bool diff = false;         // row-diff redraw
	bool ascii = false;        // one density character per cell instead of half blocks
	std::vector<std::string> prev_rows;
	uint64_t frames_drawn = 0;
	uint64_t bytes_raw = 0;
	uint64_t bytes_emitted = 0;
	double last_draw_ms = 0.0;
#ifdef _WIN32
	HANDLE hout = INVALID_HANDLE_VALUE;
	UINT saved_output_cp = 0;
#else
	bool raw_input = false;
	termios saved_input{};
#endif

	void init() {
#ifdef _WIN32
		hout = GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD mode_flags = 0;
		if (hout != INVALID_HANDLE_VALUE && GetConsoleMode(hout, &mode_flags)) {
			is_console = true;
			SetConsoleMode(hout, mode_flags | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
		}
		saved_output_cp = GetConsoleOutputCP();
		SetConsoleOutputCP(CP_UTF8);
		SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
		timeBeginPeriod(1);
#else
		is_console = isatty(STDOUT_FILENO) != 0;
		if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved_input) == 0) {
			termios raw = saved_input;
			raw.c_lflag &= ~(ICANON | ECHO);
			raw.c_cc[VMIN] = 0;
			raw.c_cc[VTIME] = 0;
			raw_input = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
		}
		struct sigaction quit{};
		quit.sa_handler = posix_quit_signal;
		sigemptyset(&quit.sa_mask);
		for (int sig : {SIGINT, SIGTERM, SIGHUP}) { sigaction(sig, &quit, nullptr); }
#endif
		is_wt = std::getenv("WT_SESSION") != nullptr;
		std::fputs("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H", stdout);
		std::fflush(stdout);
		active = true;
	}

	void shutdown() {
		if (!active) { return; }
		std::fputs("\x1b[0m\x1b[?1049l\x1b[?25h", stdout);
		std::fflush(stdout);
#ifdef _WIN32
		if (saved_output_cp != 0) { SetConsoleOutputCP(saved_output_cp); }
		SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
		timeEndPeriod(1);
#else
		if (raw_input) { tcsetattr(STDIN_FILENO, TCSANOW, &saved_input); }
		for (int sig : {SIGINT, SIGTERM, SIGHUP}) { std::signal(sig, SIG_DFL); }
#endif
		active = false;
	}

	// single buffered write per frame; on a real console go straight to
	// WriteConsoleA (skips CRT stream locking/chunking)
	void write_out(const std::string& s) {
		bytes_emitted += s.size();
#ifdef _WIN32
		if (is_console) {
			DWORD written = 0;
			WriteConsoleA(hout, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
			return;
		}
#endif
		std::fwrite(s.data(), 1, s.size(), stdout);
		std::fflush(stdout);
	}

	void draw(const std::string& script_frame) {
		double t0 = now_seconds();
		++frames_drawn;
		bytes_raw += script_frame.size();
		const std::string& frame = ascii ? ascii_frame(script_frame) : script_frame;
		std::string out;
		out.reserve(frame.size() + 16);
		if (!diff) {
			sgr_state st;
			out += "\x1b[H";
			out += filter_vt(frame, st, mode, tol);
			out += "\x1b[0m";
		} else {
			// rows are self-contained (first cell always a full escape, RESET at the
			// end), so unchanged rows can be skipped. Filtering runs per row with
			// fresh state to keep the cached rows byte-comparable.
			std::vector<std::string> rows;
			size_t start = 0;
			for (;;) {
				size_t nl = frame.find('\n', start);
				sgr_state st;
				rows.push_back(filter_vt(
					frame.substr(start, nl == std::string::npos ? std::string::npos : nl - start),
					st, mode, tol));
				if (nl == std::string::npos) { break; }
				start = nl + 1;
			}
			bool full = rows.size() != prev_rows.size();
			for (size_t y = 0; y < rows.size(); ++y) {
				if (!full && rows[y] == prev_rows[y]) { continue; }
				out += "\x1b[" + std::to_string(y + 1) + ";1H";
				out += rows[y];
			}
			out += "\x1b[0m";
			prev_rows = std::move(rows);
		}
		write_out(out);
		last_draw_ms = (now_seconds() - t0) * 1000.0;
	}

	void size(int& w, int& h) {
		w = 100; h = 40;
#ifdef _WIN32
		CONSOLE_SCREEN_BUFFER_INFO info;
		if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
			w = info.srWindow.Right - info.srWindow.Left;      // one column spare: no wrap
			h = info.srWindow.Bottom - info.srWindow.Top;      // one row spare: no scroll
		}
#else
		winsize ws{};
		if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
			w = ws.ws_col - 1;
			h = ws.ws_row - 1;
		}
#endif
		if (w > 110) { w = 110; }
		if (h > 44) { h = 44; }
		if (w < 60) { w = 60; }
		if (h < 24) { h = 24; }
	}

	// "" when no key pending; letters lowercased; specials by name.
	std::string poll_key() {
#ifdef _WIN32
		if (!_kbhit()) { return ""; }
		int c = _getch();
		if (c == 0 || c == 0xE0) {
			int c2 = _kbhit() ? _getch() : 0;
			switch (c2) {
				case 72: return "up";
				case 80: return "down";
				case 75: return "left";
				case 77: return "right";
				default: return "";
			}
		}
		switch (c) {
			case 9: return "tab";
			case 13: return "enter";
			case 27: return "esc";
			default: break;
		}
		if (c >= 'A' && c <= 'Z') { c += 32; }
		if (c >= 32 && c < 127) { return std::string(1, static_cast<char>(c)); }
#else
		if (!raw_input) { return ""; }
		unsigned char c = 0;
		if (read(STDIN_FILENO, &c, 1) != 1) { return ""; }
		if (c == 27) {
			unsigned char seq[2] = {0, 0};
			if (read(STDIN_FILENO, &seq[0], 1) != 1) { return "esc"; }
			if (seq[0] != '[' && seq[0] != 'O') { return "esc"; }
			unsigned char fin = 0;
			while (read(STDIN_FILENO, &fin, 1) == 1 && fin >= 0x20 && fin <= 0x3F) {}
			switch (fin) {
				case 'A': return "up";
				case 'B': return "down";
				case 'C': return "right";
				case 'D': return "left";
				default: return "";
			}
		}
		if (c == 9) { return "tab"; }
		if (c == 10 || c == 13) { return "enter"; }
		if (c >= 'A' && c <= 'Z') { c = static_cast<unsigned char>(c + 32); }
		if (c >= 32 && c < 127) { return std::string(1, static_cast<char>(c)); }
#endif
		return "";
	}
};

std::string strip_ansi(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size();) {
		if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
			i += 2;
			while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) { ++i; }
			if (i < s.size()) { ++i; }
			continue;
		}
		out += s[i++];
	}
	return out;
}

struct slot {
	std::unique_ptr<script_session> session;
	std::string name;                 // backend name
	double last_ms = 0.0;             // last measured demo_frame cost
	double load_ms = 0.0;             // load (parse / precompiled load) time
};

host_services make_services() {
	host_services s;
	s.log = [](const std::string& line) {
		std::fputs(line.c_str(), stdout);
		std::fputc('\n', stdout);
		std::fflush(stdout);
	};
	return s;
}

void open_slot(slot& sl, script_adapter& adapter, const std::string& backend, const host_options& opt) {
	sl.name = backend;
	sl.session = adapter.make_session(backend, make_services(), opt);
	double t0 = now_seconds();
	try {
		sl.session->load();
	} catch (const std::exception& ex) {
		throw std::runtime_error("load[" + backend + "]: " + ex.what());
	}
	sl.load_ms = (now_seconds() - t0) * 1000.0;
}

std::string run_frame(slot& sl, const frame_args& a) {
	double t0 = now_seconds();
	std::string text = sl.session->frame(a);
	sl.last_ms = (now_seconds() - t0) * 1000.0;
	return text;
}

frame_args headless_args(double t, double dt) {
	frame_args a;
	a.t = t;
	a.dt = dt;
	return a;
}

std::vector<std::string> harness_backends(script_adapter& adapter, const host_options& opt) {
	if (opt.backend_selected) { return {opt.backend}; }
	return adapter.backends();
}

std::string hex64(uint64_t v) {
	char buf[24];
	std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
	return buf;
}

int run_smoke(script_adapter& adapter, const host_options& opt) {
	const int w = 100, h = 40;
	const double dt = 1.0 / 30.0;
	std::printf("%s --smoke: %lld frames @ %dx%d, seed %lld, fixed dt %.5f\n\n",
		adapter.program_name().c_str(), static_cast<long long>(opt.frames), w, h,
		static_cast<long long>(opt.seed), dt);
	std::printf("%-12s | %-14s | %10s | %9s | %12s | %s\n",
		"backend", "boot", "total ms", "ms/frame", "cells/sec", "frame hash");
	std::printf("-------------+----------------+------------+-----------+--------------+-------------------\n");

	std::vector<std::string> names = harness_backends(adapter, opt);
	std::vector<uint64_t> hashes;
	std::vector<double> per_frame;
	const bool trace = std::getenv("DEMOREEL_TRACE") != nullptr;
	const char* dump_env = std::getenv("DEMOREEL_DUMP_FRAME");
	const int64_t dump_frame = dump_env ? std::stoll(dump_env) : -1;
	const bool full_durations = std::getenv("DEMOREEL_SMOKE_FULLDUR") != nullptr;
	for (const std::string& name : names) {
		slot sl;
		open_slot(sl, adapter, name, opt);
		try {
			sl.session->boot(w, h, opt.seed, sl.name, !full_durations, false);
		} catch (const std::exception& ex) {
			throw std::runtime_error("boot[" + sl.name + "]: " + ex.what());
		}
		uint64_t hash = fnv1a_offset;
		double t0 = now_seconds();
		for (int64_t i = 0; i < opt.frames; ++i) {
			if (trace) { std::fprintf(stderr, "[%s f%lld]\n", sl.name.c_str(), static_cast<long long>(i)); std::fflush(stderr); }
			std::string frame;
			try {
				frame = run_frame(sl, headless_args(i * dt, dt));
			} catch (const std::exception& ex) {
				std::string msg = "frame " + std::to_string(i) + "[" + sl.name + "]: " + ex.what();
				std::string st = sl.session->stack_trace();
				if (!st.empty()) { msg += "\n" + st; }
				throw std::runtime_error(msg);
			}
			if (i == dump_frame) {
				std::ofstream out("demoreel_frame_" + sl.name + ".txt", std::ios::binary | std::ios::trunc);
				out.write(frame.data(), static_cast<std::streamsize>(frame.size()));
			}
			hash = fnv1a(hash, frame);
		}
		double total_ms = (now_seconds() - t0) * 1000.0;
		double pf = total_ms / opt.frames;
		per_frame.push_back(pf);
		hashes.push_back(hash);
		double cells_per_sec = (static_cast<double>(w) * h * opt.frames) / (total_ms / 1000.0);
		std::printf("%-12s | %7.1f ms %-4s | %10.1f | %9.3f | %12.0f | %s\n",
			sl.name.c_str(), sl.load_ms, sl.session->load_kind().c_str(),
			total_ms, pf, cells_per_sec, hex64(hash).c_str());
	}

	bool match = true;
	for (uint64_t hv : hashes) { match = match && hv == hashes[0]; }
	if (hashes.size() > 1) {
		std::printf("\nbackend parity: %s", match ? "OK (frame streams byte-identical)\n" : "MISMATCH\n");
		if (match && per_frame.size() == 2 && per_frame[0] > 0.0) {
			std::printf("%s speedup over %s: %.2fx\n", names[1].c_str(), names[0].c_str(), per_frame[0] / per_frame[1]);
		}
	}
	if (!opt.expect.empty()) {
		bool ok = hex64(hashes[0]) == opt.expect;
		std::printf("expected stream hash %s: %s\n", opt.expect.c_str(), ok ? "OK" : "MISMATCH");
		match = match && ok;
	}
	return match ? 0 : 1;
}

// Every scene mid-flight, no transitions: script cost of update + render +
// presentation per frame on a freshly booted reel, and a hash of each scene's frames.
int run_perf(script_adapter& adapter, const host_options& opt) {
	const int w = opt.width > 0 ? static_cast<int>(opt.width) : 100;
	const int h = opt.height > 0 ? static_cast<int>(opt.height) : 40;
	const double dt = 1.0 / 30.0;
	std::printf("%s --perf: %lld frames per scene @ %dx%d from scene t=%.2f, seed %lld, workers %lld\n",
		adapter.program_name().c_str(), static_cast<long long>(opt.perf), w, h, opt.perf_at,
		static_cast<long long>(opt.seed), static_cast<long long>(opt.workers));
	std::ofstream csv;
	if (!opt.csv.empty()) {
		csv.open(opt.csv, std::ios::trunc);
		csv << "program,backend,workers,scene,name,ms_per_frame,bytes_per_frame,hash\n";
	}
	for (const std::string& name : harness_backends(adapter, opt)) {
		slot sl;
		open_slot(sl, adapter, name, opt);
		sl.session->boot(w, h, opt.seed, sl.name, false, false);
		const int64_t n = sl.session->scene_count();
		std::printf("\n%-12s | %2s | %-12s | %9s | %8s | %s\n", "backend", "#", "scene", "ms/frame", "KB/frame", "scene hash");
		std::printf("-------------+----+--------------+-----------+----------+-----------------\n");
		double total = 0.0;
		if (opt.scene >= n) { throw std::runtime_error("perf scene index is out of range"); }
		const int64_t first = opt.scene < 0 ? 0 : opt.scene;
		const int64_t end = opt.scene < 0 ? n : opt.scene + 1;
		for (int64_t s = first; s < end; ++s) {
			if (opt.perf_warmup > 0) {
				sl.session->boot(w, h, opt.seed, sl.name, false, false);
				sl.session->jump(s, opt.perf_at);
				for (int64_t f = 0; f < opt.perf_warmup; ++f) {
					(void)sl.session->frame(headless_args(opt.perf_at + f * dt, dt));
				}
			}
			sl.session->boot(w, h, opt.seed, sl.name, false, false);
			sl.session->jump(s, opt.perf_at);
			std::string scene = sl.session->scene_name();
			uint64_t hash = fnv1a_offset;
			uint64_t bytes = 0;
			double t0 = now_seconds();
			for (int64_t f = 0; f < opt.perf; ++f) {
				std::string frame;
				try {
					frame = sl.session->frame(headless_args(opt.perf_at + f * dt, dt));
				} catch (const std::exception& ex) {
					std::string msg = "scene " + std::to_string(s) + " frame " + std::to_string(f) + "[" + sl.name + "]: " + ex.what();
					std::string st = sl.session->stack_trace();
					if (!st.empty()) { msg += "\n" + st; }
					throw std::runtime_error(msg);
				}
				bytes += frame.size();
				hash = fnv1a(hash, frame);
			}
			double ms = (now_seconds() - t0) * 1000.0 / static_cast<double>(opt.perf);
			total += ms;
			std::printf("%-12s | %2lld | %-12s | %9.3f | %8.1f | %s\n", sl.name.c_str(),
				static_cast<long long>(s + 1), scene.c_str(), ms,
				static_cast<double>(bytes) / static_cast<double>(opt.perf) / 1024.0, hex64(hash).c_str());
			if (csv) {
				csv << adapter.program_name() << ',' << sl.name << ',' << opt.workers << ',' << (s + 1) << ','
				    << scene << ',' << ms << ',' << (bytes / static_cast<uint64_t>(opt.perf)) << ',' << hex64(hash) << '\n';
			}
		}
		std::printf("%-12s | mean %.3f ms/frame over %lld scenes\n", sl.name.c_str(), total / static_cast<double>(end - first),
			static_cast<long long>(end - first));
	}
	return 0;
}

int run_capture(script_adapter& adapter, const host_options& opt) {
	const int w = 100, h = 40;
	const double dt = 1.0 / 30.0;
	slot sl;
	open_slot(sl, adapter, opt.backend, opt);
	sl.session->boot(w, h, opt.seed, sl.name, false, false);
	const char* cap_t_env = std::getenv("DEMOREEL_CAP_T");
	double cap_t = cap_t_env ? std::stod(cap_t_env) : 5.0;
	sl.session->jump(opt.capture, cap_t);
	std::string frame;
	for (int i = 0; i < 75; ++i) {
		frame = run_frame(sl, headless_args(cap_t + i * dt, dt));
	}
	std::string plain = strip_ansi(opt.ascii ? ascii_frame(frame) : frame);
	std::fwrite(plain.data(), 1, plain.size(), stdout);
	std::fputc('\n', stdout);
	return 0;
}

// Headless exercise of the live session's tricks: 'r' (hot reload of every scene
// file) and a jump back onto the scene and time it was showing.
int run_reload_test(script_adapter& adapter, const host_options& opt) {
	const int w = 100, h = 40;
	const double dt = 1.0 / 30.0;
	slot sl;
	open_slot(sl, adapter, opt.backend, opt);
	sl.session->boot(w, h, opt.seed, sl.name, opt.smoke, false);
	for (int i = 0; i < 40; ++i) { run_frame(sl, headless_args(i * dt, dt)); }
	if (!sl.session->reload()) {
		std::printf("reload-test: %s does not support hot reload\n", sl.name.c_str());
		return 1;
	}
	for (int i = 40; i < 80; ++i) { run_frame(sl, headless_args(i * dt, dt)); }
	std::puts("reload-test: hot reload OK");
	int64_t idx = sl.session->scene_index();
	double st = sl.session->scene_time();
	sl.session->jump(idx, st);
	for (int i = 80; i < 110; ++i) { run_frame(sl, headless_args(i * dt, dt)); }
	std::printf("reload-test: jump back onto the scene OK (scene %lld t=%.2f)\n", static_cast<long long>(idx), st);
	return 0;
}

int run_interactive(script_adapter& adapter, const host_options& opt) {
	console_host console;
	console.init();
	// legacy conhost parses VT slowly, and a terminal without 24-bit color quantizes it badly:
	// both get the xterm-256 cube
#ifdef _WIN32
	const bool legacy_conhost = console.is_console && !console.is_wt && std::getenv("COLORTERM") == nullptr;
#else
	const bool legacy_conhost = false;
#endif
	console.mode = (opt.filter_mode >= 0) ? static_cast<int>(opt.filter_mode) : (terminal_has_truecolor() ? 1 : 2);
	console.tol = static_cast<int>(opt.tol);
	console.diff = opt.diff;
	console.ascii = opt.ascii;
	int w = 100, h = 40;
	console.size(w, h);
	if (opt.width > 0) { w = static_cast<int>(opt.width); }
	if (opt.height > 0) { h = static_cast<int>(opt.height); }

	slot sl;
	double sim_ms_sum = 0.0, draw_ms_sum = 0.0;
	int64_t bench_done = 0;
	try {
		open_slot(sl, adapter, opt.backend, opt);
		sl.session->boot(w, h, opt.seed, sl.name, false, true);
		if (opt.scene >= 0) { sl.session->jump(opt.scene, 5.0); }

		double t_prev = now_seconds();
		double fps_ema = opt.target_fps;
		std::string host_notice;
		double notice_until = 0.0;
		if (legacy_conhost) {
			host_notice = " tip: Windows Terminal renders this reel much faster (conhost: 256-color mode on, --truecolor to override) ";
			notice_until = now_seconds() + 6.0;
		}

		for (;;) {
			double frame_start = now_seconds();
			double dt = frame_start - t_prev;
			t_prev = frame_start;
			if (dt > 0.1) { dt = 0.1; }
			if (dt > 0.0001) { fps_ema = fps_ema * 0.92 + (1.0 / dt) * 0.08; }

			std::string key = console.poll_key();
			if (key == "q" || key == "esc" || g_quit_requested.load()) { break; }
			if (key == "r") {
				try {
					host_notice = sl.session->reload() ? sl.session->reload_notice() : " hot reload unsupported here ";
				} catch (const std::exception& ex) {
					host_notice = std::string(" reload error: ") + ex.what() + " ";
				}
				notice_until = now_seconds() + 2.5;
				key = "";
			}

			frame_args a;
			a.t = frame_start;
			a.dt = dt;
			a.key = key;
			a.fps = fps_ema;
			a.ms_sim = sl.last_ms;
			a.ms_draw = console.last_draw_ms;
			std::string frame = run_frame(sl, a);
			console.draw(frame);
			if (opt.bench > 0) {
				sim_ms_sum += sl.last_ms;
				draw_ms_sum += console.last_draw_ms;
				if (++bench_done >= opt.bench) { break; }
			}
			if (now_seconds() < notice_until && !host_notice.empty()) {
				std::string banner = "\x1b[" + std::to_string(h) + ";1H\x1b[30;46m" + host_notice + "\x1b[0m";
				std::fwrite(banner.data(), 1, banner.size(), stdout);
				std::fflush(stdout);
			}

			double budget = 1.0 / opt.target_fps;
			double used = now_seconds() - frame_start;
			if (used < budget) { std::this_thread::sleep_for(std::chrono::duration<double>(budget - used)); }
		}
	} catch (const std::exception& ex) {
		console.shutdown();
		std::fprintf(stderr, "%s: script error: %s\n", adapter.program_name().c_str(), ex.what());
		std::string trace = sl.session ? sl.session->stack_trace() : "";
		if (!trace.empty()) { std::fprintf(stderr, "[%s]\n%s\n", sl.name.c_str(), trace.c_str()); }
		return 1;
	}
	console.shutdown();
	if (opt.bench > 0 && console.frames_drawn > 0) {
		double fr = static_cast<double>(console.frames_drawn);
		double sim = sim_ms_sum / fr, drw = draw_ms_sum / fr;
		char report[512];
		std::snprintf(report, sizeof(report),
			"bench: %llu frames, scene %lld | terminal=%s | mode=%d tol=%d diff=%s\n"
			"  sim   %8.2f ms/frame  (script update + frame string)\n"
			"  write %8.2f ms/frame  (filter + console write)\n"
			"  total %8.2f ms/frame  (%.1f fps uncapped)\n"
			"  bytes/frame %.0f raw -> %.0f written (%.2fx reduction)\n",
			static_cast<unsigned long long>(console.frames_drawn),
			static_cast<long long>(opt.scene),
			console.is_wt ? "windows-terminal" : (console.is_console ? "conhost" : "redirected"),
			console.mode, console.tol, console.diff ? "on" : "off",
			sim, drw, sim + drw, 1000.0 / (sim + drw),
			static_cast<double>(console.bytes_raw) / fr,
			static_cast<double>(console.bytes_emitted) / fr,
			static_cast<double>(console.bytes_raw) / static_cast<double>(console.bytes_emitted ? console.bytes_emitted : 1));
		std::fputs(report, stderr);
		if (!opt.bench_out.empty()) {
			std::ofstream f(opt.bench_out, std::ios::trunc);
			f << report;
		}
	}
	std::printf("%s: %s load %.1fms (%s), last frame %.2fms\n", adapter.program_name().c_str(), sl.name.c_str(), sl.load_ms,
		sl.session->load_kind().c_str(), sl.last_ms);
	return 0;
}

void print_usage(script_adapter& adapter) {
#ifdef DEMOREEL_LAYOUT_PADDING
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
	std::string backends;
	for (const std::string& b : adapter.backends()) { backends += (backends.empty() ? "" : " | ") + b; }
	std::printf("%s - the demoscene tech reel (%s)\n", adapter.program_name().c_str(), adapter.language().c_str());
	std::printf("  --backend B      backend: %s (default %s); narrows --smoke/--perf\n", backends.c_str(),
		adapter.default_backend().c_str());
	std::puts(
		"  --seed N         reel seed (default 20260705)\n"
		"  --fps N          frame pacing target (default 30)\n"
		"  --w N / --h N    force console dimensions (also --perf's)\n"
		"  --smoke          headless determinism + perf run on every backend\n"
		"  --frames N       smoke frame budget (default 1700)\n"
		"  --expect HEX     smoke: required frame-stream hash (exit 1 on mismatch)\n"
		"  --perf N         headless: N frames of every scene mid-flight, ms/frame + scene hash\n"
		"  --perf-at T      perf: scene time the measured frames start from (default 3.0)\n"
		"  --perf-warmup N  untimed frames on the same runtime before each measured scene\n"
		"  --csv P          perf: also write rows to file P\n"
		"  --workers N      parallel width (-1 engine default, 1 serial; ports always serial)\n"
		"  --capture I      print scene I as a plain-text frame and exit (--ascii: a density map)\n"
		"  --reload-test    headless hot-reload + scene-jump self-test\n"
		"  --scenes DIR     override the scenes directory\n"
		"  --bench N        run N frames of the real loop, report sim/write split, exit\n"
		"  --scene I        select one zero-based scene for --perf or --bench\n"
		"  --bench-out P    also write the bench report to file P\n"
		"  --truecolor      24-bit output with tolerance run-merging (the default under\n"
		"                   COLORTERM=truecolor|24bit and in Windows Terminal)\n"
		"  --256color       xterm-256 output (short escapes, long runs; every other terminal)\n"
		"  --ascii          one density character per cell, for terminals without block glyphs\n"
		"  --no-filter      byte-exact legacy output (no escape trimming)\n"
		"  --tol N          per-channel merge tolerance for --truecolor (default 8)\n"
		"  --diff           row-diff redraw: skip rows unchanged since last frame");
	std::string extra = adapter.usage();
	if (!extra.empty()) { std::fputs(extra.c_str(), stdout); }
	std::puts(
		"\nkeys: r hot reload | h HUD | f freeze | n/p/space scene skip | 1-9,0 jump to scene |\n"
		"      q/ESC quit");
}

} // namespace

int run_demoreel(int argc, char** argv, script_adapter& adapter) {
	host_options opt;
	opt.argv0 = argc > 0 ? argv[0] : "";
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
		else if (a == "--backend") { opt.backend = next_arg("--backend"); opt.backend_selected = true; }
		else if (a == "--smoke") { opt.smoke = true; }
		else if (a == "--frames") { opt.frames = std::stoll(next_arg("--frames")); }
		else if (a == "--expect") { opt.expect = next_arg("--expect"); }
		else if (a == "--perf") { opt.perf = std::stoll(next_arg("--perf")); }
		else if (a == "--perf-warmup") { opt.perf_warmup = std::stoll(next_arg("--perf-warmup")); }
		else if (a == "--perf-at") { opt.perf_at = std::stod(next_arg("--perf-at")); }
		else if (a == "--csv") { opt.csv = next_arg("--csv"); }
		else if (a == "--workers") { opt.workers = std::stoll(next_arg("--workers")); }
		else if (a == "--capture") { opt.capture = std::stoll(next_arg("--capture")); }
		else if (a == "--reload-test") { opt.reload_test = true; }
		else if (a == "--scenes") { opt.scenes_dir = next_arg("--scenes"); }
		else if (a == "--fps") { opt.target_fps = std::stod(next_arg("--fps")); }
		else if (a == "--w") { opt.width = std::stoll(next_arg("--w")); }
		else if (a == "--h") { opt.height = std::stoll(next_arg("--h")); }
		else if (a == "--bench") { opt.bench = std::stoll(next_arg("--bench")); }
		else if (a == "--scene") { opt.scene = std::stoll(next_arg("--scene")); }
		else if (a == "--bench-out") { opt.bench_out = next_arg("--bench-out"); }
		else if (a == "--truecolor") { opt.filter_mode = 1; }
		else if (a == "--256color") { opt.filter_mode = 2; }
		else if (a == "--no-filter") { opt.filter_mode = 0; }
		else if (a == "--tol") { opt.tol = std::stoll(next_arg("--tol")); }
		else if (a == "--diff") { opt.diff = true; }
		else if (a == "--ascii") { opt.ascii = true; }
		else if (a == "--dev") { /* hot reload is always on; accepted for muscle memory */ }
		else if (a == "--help" || a == "-h") { print_usage(adapter); return 0; }
		else {
			int used = adapter.parse_flag(argc, argv, i);
			if (used <= 0) {
				std::fprintf(stderr, "unknown flag: %s\n", a.c_str());
				print_usage(adapter);
				return 2;
			}
			i += used - 1;
		}
	}
	if (opt.perf_warmup < 0 || opt.scene < -1) { std::fprintf(stderr, "invalid perf warmup or scene index\n"); return 2; }
	if (opt.backend.empty()) { opt.backend = adapter.default_backend(); }
	opt.backend = adapter.normalize_backend(opt.backend);
	bool known = false;
	for (const std::string& b : adapter.backends()) { known = known || b == opt.backend; }
	if (!known) {
		std::fprintf(stderr, "unknown backend: %s\n", opt.backend.c_str());
		return 2;
	}

	try {
		if (opt.smoke) { return run_smoke(adapter, opt); }
		if (opt.perf > 0) { return run_perf(adapter, opt); }
		if (opt.capture >= 0) { return run_capture(adapter, opt); }
		if (opt.reload_test) { return run_reload_test(adapter, opt); }
	} catch (const std::exception& ex) {
		std::fprintf(stderr, "%s: script error: %s\n", adapter.program_name().c_str(), ex.what());
		return 1;
	}
	return run_interactive(adapter, opt);
}

} // namespace demoreel
