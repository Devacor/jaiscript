// jaiscript_web: the engine and the runner's console builtins compiled to WebAssembly, for the site's Play page and landing page.
// Scripts see the runner's console builtins by the same names. Two builds share this file:
// - jaiscript_web_worker (no Asyncify) runs a script to its end inside a Web Worker: jaiweb_run_file / jaiweb_run_source block
//   the worker, sleep() waits in Atomics.wait, and the page's keys and terminal size arrive through shared memory that
//   Module.jaiwebPump drains before each console read.
// - jaiscript_web (Asyncify) runs in steps where the page cannot share memory with a worker: jaiweb_start_file /
//   jaiweb_start_source begin a suspendable run and the host calls jaiweb_tick when its sleep is due; a sleep hands control back. A call that switches fibers returns
//   the value it had when Asyncify unwound it, so these return nothing and the page asks jaiweb_running afterwards.
// Output goes to Module.jaiwebWrite as UTF-8 bytes either way.
// Either build also keeps a live engine across calls for the landing page's scene (jaiweb_live_file, jaiweb_live_eval):
// running a file again by its path hot-reloads it.
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/core/engine_time.hpp>
#include <jaiscript/core/script_run.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include "../jaiscript/file_io.hpp"
#include "../jaiscript/sound.hpp"

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <streambuf>
#include <string>
#include <vector>

namespace {

EM_JS(void, jaiweb_js_write, (const char* bytes, int size), {
	if (Module.jaiwebWrite) { Module.jaiwebWrite(HEAPU8.slice(bytes, bytes + size)); }
});

EM_JS(void, jaiweb_js_pump, (), {
	if (Module.jaiwebPump) { Module.jaiwebPump(); }
});

EM_JS(void, jaiweb_js_sleep, (double ms), {
	if (Module.jaiwebSleep) {
		Module.jaiwebSleep(ms);
	} else {
		const until = performance.now() + ms;
		while (performance.now() < until) {}
	}
});

class terminal_buffer : public std::streambuf {
public:
	terminal_buffer() { setp(storage, storage + sizeof(storage)); }
	~terminal_buffer() override { sync(); }

protected:
	int overflow(int c) override {
		sync();
		if (c != traits_type::eof()) {
			*pptr() = static_cast<char>(c);
			pbump(1);
		}
		return c == traits_type::eof() ? 0 : c;
	}
	int sync() override {
		const auto size = static_cast<int>(pptr() - pbase());
		if (size > 0) { jaiweb_js_write(pbase(), size); }
		setp(storage, storage + sizeof(storage));
		return 0;
	}

private:
	char storage[1 << 16];
};

// Sound commands to the page, which plays them with Web Audio: a load carries the file's bytes and its format
enum class sound_op { load = 0, play = 1, stop = 2, volume = 3, release = 4, stop_all = 5 };

EM_JS(void, jaiweb_js_sound, (int a_op, int a_id, const char* a_bytes, int a_size, double a_value), {
	if (Module.jaiwebSound) { Module.jaiwebSound(a_op, a_id, a_size > 0 ? HEAPU8.slice(a_bytes, a_bytes + a_size) : null, a_value); }
});

void send_sound(sound_op a_op, int a_id, double a_value = 0.0) { jaiweb_js_sound(static_cast<int>(a_op), a_id, nullptr, 0, a_value); }

// A WAV's length from its header (byte rate and data chunk), 0 when the header does not say
double wav_seconds(const std::string& a_bytes) {
	auto u32 = [&](size_t a_at) -> uint32_t {
		if (a_at + 4 > a_bytes.size()) { return 0; }
		const auto* p = reinterpret_cast<const unsigned char*>(a_bytes.data() + a_at);
		return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
	};
	uint32_t byte_rate = 0;
	for (size_t at = 12; at + 8 <= a_bytes.size();) {
		const uint32_t size = u32(at + 4);
		if (a_bytes.compare(at, 4, "fmt ") == 0) { byte_rate = u32(at + 16); }
		if (a_bytes.compare(at, 4, "data") == 0) { return byte_rate ? static_cast<double>(size) / byte_rate : 0.0; }
		at += 8 + size + (size & 1);
	}
	return 0.0;
}

// The page plays one voice a sound; the device goes when the last Sound naming it does (a copy names the same one)
struct page_sound_device {
	int id = 0;
	jai_runner::sound_format format = jai_runner::sound_format::none;
	double seconds = 0.0;          // 0: unknown (MIDI, MP3), so a one-shot counts as playing until stopped
	bool looping = false;
	bool stopped = true;
	bool finish_when_dropped = false;   // play_sound's: its last voice plays out after the Sound is gone
	std::chrono::steady_clock::time_point started;
	~page_sound_device() { send_sound(sound_op::release, id, finish_when_dropped ? 1.0 : 0.0); }
};

class page_sound {
public:
	explicit page_sound(std::shared_ptr<page_sound_device> a_device) : device(std::move(a_device)) {}

	void finish_when_dropped() { device->finish_when_dropped = true; }

	bool play() { return play_looped(false); }
	bool play_looped(bool a_loop) {
		device->looping = a_loop;
		device->stopped = false;
		device->started = std::chrono::steady_clock::now();
		send_sound(sound_op::play, device->id, a_loop ? 1.0 : 0.0);
		return true;
	}
	void stop() {
		device->stopped = true;
		send_sound(sound_op::stop, device->id);
	}
	bool playing() const {
		if (device->stopped) { return false; }
		if (device->looping || device->seconds <= 0.0) { return true; }
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - device->started).count() < device->seconds;
	}
	void volume(double a_level) { send_sound(sound_op::volume, device->id, std::clamp(a_level, 0.0, 1.0)); }
	std::string format() const { return jai_runner::sound_format_name(device->format); }

private:
	std::shared_ptr<page_sound_device> device;
};

int g_next_sound_id = 1;

std::shared_ptr<page_sound> load_page_sound(const std::string& a_path) {
	const jai_runner::sound_format format = jai_runner::sniff_sound_format(a_path);
	if (format == jai_runner::sound_format::none) { return nullptr; }
	const std::string bytes = jai_runner::read_file_or_raise(a_path);
	auto device = std::make_shared<page_sound_device>();
	device->id = g_next_sound_id++;
	device->format = format;
	device->seconds = format == jai_runner::sound_format::wav ? wav_seconds(bytes) : 0.0;
	jaiweb_js_sound(static_cast<int>(sound_op::load), device->id, bytes.data(), static_cast<int>(bytes.size()), static_cast<double>(format));
	return std::make_shared<page_sound>(std::move(device));
}

// The runner's sound builtins by the same names and shapes, played by the page
void register_page_sound(jai::engine& eng) {
	jai::dynamic_binder<page_sound>(eng, "Sound")
		.method("play", &page_sound::play)
		.method("play", &page_sound::play_looped)
		.method("stop", &page_sound::stop)
		.method("playing", &page_sound::playing)
		.method("volume", &page_sound::volume)
		.method("format", &page_sound::format)
		.build();
	eng.add_function("load_sound", [](const std::string& path) { return load_page_sound(path); }, jai::permissions::filesystem);
	eng.add_function("play_sound", [](const std::string& path) {
		auto result = load_page_sound(path);
		if (result) {
			result->finish_when_dropped();
			result->play();
		}
		return result;
	}, jai::permissions::filesystem);
	eng.add_function("play_sound", [](const std::string& path, bool loop) {
		auto result = load_page_sound(path);
		if (result) {
			result->finish_when_dropped();
			result->play_looped(loop);
		}
		return result;
	}, jai::permissions::filesystem);
	eng.add_function("stop_sounds", []() { send_sound(sound_op::stop_all, 0); });
}

// Key codes the page sends: a printable ASCII character is its own code (letters lowercased), named keys follow
const char* const NAMED_KEYS[] {"space", "left", "right", "up", "down", "shift", "ctrl", "alt", "esc", "enter", "tab", "backspace"};
constexpr int FIRST_NAMED_KEY = 128;

std::string key_name_of(int a_code) {
	if (a_code > 32 && a_code < 127) { return std::string(1, static_cast<char>(a_code)); }
	const int named = a_code - FIRST_NAMED_KEY;
	if (named >= 0 && named < static_cast<int>(std::size(NAMED_KEYS))) { return NAMED_KEYS[named]; }
	return std::string();
}

struct page_input {
	std::map<std::string, bool> held;
	std::vector<std::string> edges;
	int cols = 100;
	int rows = 40;
	std::map<std::string, std::string> environment {{"COLORTERM", "truecolor"}, {"TERM", "xterm-256color"}};
};

struct web_session {
	terminal_buffer buffer;
	std::ostream out {&buffer};
	std::shared_ptr<jai::engine> eng;
	std::optional<jai::script_run> run;
	int exit_code = 0;
};

page_input g_input;
std::unique_ptr<web_session> g_session;

// The landing page's scene: one engine that outlives its calls, so running a file again by its path hot-reloads it
struct live_engine {
	std::shared_ptr<jai::engine> eng;
	std::string reply;
	bool failed = false;
};
live_engine g_live;

jai::engine& live_engine_instance() {
	if (!g_live.eng) {
		g_live.eng = jai::engine::make();
		jai::stdlib::register_all(g_live.eng);
	}
	return *g_live.eng;
}

const char* live_reply(const std::function<std::string(jai::engine&)>& a_body) {
	try {
		g_live.reply = a_body(live_engine_instance());
		g_live.failed = false;
	} catch (const std::exception& ex) {
		g_live.reply = ex.what();
		g_live.failed = true;
	}
	return g_live.reply.c_str();
}

void register_page_console(jai::engine& eng) {
	eng.add_function("key_down", [](const std::string& name) -> bool {
		jaiweb_js_pump();
		auto it = g_input.held.find(name);
		return it != g_input.held.end() && it->second;
	});
	eng.add_function("read_key", []() -> std::string {
		jaiweb_js_pump();
		if (g_input.edges.empty()) { return ""; }
		std::string key = g_input.edges.front();
		g_input.edges.erase(g_input.edges.begin());
		return key;
	});
	eng.add_function("term_cols", []() -> jai::script_int {
		jaiweb_js_pump();
		return g_input.cols;
	});
	eng.add_function("term_rows", []() -> jai::script_int {
		jaiweb_js_pump();
		return g_input.rows;
	});
	eng.add_function("env_var", [](const std::string& name) -> std::string {
		auto it = g_input.environment.find(name);
		return it == g_input.environment.end() ? std::string() : it->second;
	});
}

void print_failure(web_session& session, const std::string& error) {
	const std::string trace = session.eng ? session.eng->format_stack_trace() : std::string();
	session.out << "\x1b[0m\x1b[31m" << error << "\x1b[0m\r\n";
	if (!trace.empty()) { session.out << trace << "\r\n"; }
	session.out.flush();
}

std::vector<std::string> split_lines(const char* a_text) {
	std::vector<std::string> result;
	std::string current;
	for (const char* c = a_text; c && *c; ++c) {
		if (*c == '\n') {
			result.push_back(current);
			current.clear();
		} else {
			current += *c;
		}
	}
	if (!current.empty()) { result.push_back(current); }
	return result;
}

web_session& new_session(const char* a_args, double a_budget, bool a_blocking_sleep) {
	g_session.reset();
	g_input.held.clear();
	g_input.edges.clear();
	g_session = std::make_unique<web_session>();
	web_session& session = *g_session;
	auto eng = jai::engine::make();
	eng->execution_budget(a_budget);
#ifdef JAISCRIPT_WEB_THREAD_POOL
	// a blocked worker cannot start a thread, so the engine stays within the threads its pool started with
	const int cores = emscripten_num_logical_cores();
	eng->parallel_thread_count(static_cast<size_t>(std::clamp(cores - 1, 1, JAISCRIPT_WEB_THREAD_POOL - 1)));
#endif
	eng->set_output_stream(std::shared_ptr<std::ostream>(&session.out, [](std::ostream*) {}));
	if (a_blocking_sleep) {
		// the budget stops a program that runs that long without a sleep, as it does where every sleep ends a host entry
		eng->time().source({}, {}, [sleeper = eng.get(), awake_since = std::chrono::steady_clock::now()](std::chrono::nanoseconds a_duration) mutable {
			jaiweb_js_sleep(std::chrono::duration<double, std::milli>(a_duration).count());
			const auto woke = std::chrono::steady_clock::now();
			sleeper->time().extend_execution_deadline(woke - awake_since);
			awake_since = woke;
		});
	}
	jai::stdlib::register_all(eng);
	jai::stdlib::register_web(eng);
	jai_runner::register_file_io(*eng);
	register_page_sound(*eng);
	register_page_console(*eng);
	jai::script_value args = jai::script_value::make_array(nullptr, eng.get());
	for (const auto& arg : split_lines(a_args)) { args.as_array().emplace_back(arg, eng.get()); }
	eng->add_global("ARGS", std::move(args));
	session.eng = std::move(eng);
	return session;
}

std::string read_script(jai::engine& a_eng, const char* a_path, std::filesystem::path& a_resolved) {
	a_resolved = std::filesystem::absolute(a_path);
	a_eng.add_include_path(a_resolved.parent_path().string());
	std::string result = jai_runner::read_file_or_raise(a_resolved.string());
	if (result.rfind("\xEF\xBB\xBF", 0) == 0) { result.erase(0, 3); }
	return result;
}

void print_check_warnings(jai::engine& a_eng, const std::string& a_content, const std::string& a_name) {
	const jai::check_report report = a_eng.check(a_content);
	if (report.diagnostics.empty() || report.has_errors()) { return; }
	g_session->out << "static check warnings:\n" << report.format(a_content, a_name);
	g_session->out.flush();
}

int exit_code_of(const jai::script_value& a_value) {
	return a_value.is_int() ? static_cast<int>(static_cast<uint64_t>(a_value.as_int()) & 0xFF) : 0;
}

int run_to_end(const char* a_args, double a_budget, const std::function<jai::script_value(jai::engine&)>& a_body) {
	web_session& session = new_session(a_args, a_budget, true);
	try {
		session.exit_code = exit_code_of(a_body(*session.eng));
	} catch (const std::exception& ex) {
		print_failure(session, ex.what());
		session.exit_code = 1;
	}
	session.out.flush();
	return session.exit_code;
}

void begin_run(const char* a_args, double a_budget, const std::function<jai::script_run(jai::engine&)>& a_start) {
	web_session& session = new_session(a_args, a_budget, false);
	try {
		session.run.emplace(a_start(*session.eng));
	} catch (const std::exception& ex) {
		print_failure(session, ex.what());
		session.run.reset();
		session.exit_code = 1;
	}
}

} // namespace

extern "C" {

// Worker build: runs a script file to its end under an execution budget (seconds; 0 is unlimited); args holds the script's
// ARGS, one per line. With warnings set, a clean load first lists the checker's warnings, as the runner's --warnings does.
// Returns the exit code.
EMSCRIPTEN_KEEPALIVE int jaiweb_run_file(const char* a_path, const char* a_args, double a_budget, int a_warnings) {
	return run_to_end(a_args, a_budget, [a_path, a_warnings](jai::engine& eng) {
		std::filesystem::path resolved;
		const std::string content = read_script(eng, a_path, resolved);
		if (a_warnings) { print_check_warnings(eng, content, resolved.filename().string()); }
		return eng.execute_file_source(resolved.string(), content);
	});
}

// Worker build: runs source text to its end under an execution budget (seconds; 0 is unlimited)
EMSCRIPTEN_KEEPALIVE int jaiweb_run_source(const char* a_source, double a_budget) {
	const std::string text = a_source ? a_source : "";
	return run_to_end("", a_budget, [&text](jai::engine& eng) { return eng.execute(text); });
}

// Asyncify build: begins a suspendable run of a script file; jaiweb_tick continues it
EMSCRIPTEN_KEEPALIVE void jaiweb_start_file(const char* a_path, const char* a_args) {
	begin_run(a_args, 0.0, [a_path](jai::engine& eng) {
		std::filesystem::path resolved;
		const std::string content = read_script(eng, a_path, resolved);
		return eng.start_file_source(resolved.string(), content);
	});
}

// Asyncify build: begins a suspendable run of source text under an execution budget
EMSCRIPTEN_KEEPALIVE void jaiweb_start_source(const char* a_source, double a_budget) {
	const std::string text = a_source ? a_source : "";
	begin_run("", a_budget, [text](jai::engine& eng) { return eng.start(text); });
}

// Asyncify build: continues the run when its sleep is over
EMSCRIPTEN_KEEPALIVE void jaiweb_tick() {
	if (!g_session || !g_session->run) { return; }
	web_session& session = *g_session;
	jai::script_run& run = *session.run;
	try {
		if (!run.done() && run.wake_at() <= session.eng->time().monotonic_now()) { run.resume(); }
		if (run.done()) {
			const auto& result = run.result();
			if (result) {
				session.exit_code = exit_code_of(result.value());
			} else {
				print_failure(session, session.eng->format_error(result));
				session.exit_code = 1;
			}
			session.run.reset();
		}
	} catch (const std::exception& ex) {
		print_failure(session, ex.what());
		session.run.reset();
		session.exit_code = 1;
	}
	session.out.flush();
}

// Asyncify build: 1 while a run is under way, 0 once it has finished (jaiweb_exit_code says how)
EMSCRIPTEN_KEEPALIVE int jaiweb_running() { return g_session && g_session->run ? 1 : 0; }

// Asyncify build: milliseconds until the run's sleep ends (0 when it is due or there is no run)
EMSCRIPTEN_KEEPALIVE double jaiweb_wait_ms() {
	if (!g_session || !g_session->run || g_session->run->done()) { return 0.0; }
	const auto wait = g_session->run->wake_at() - g_session->eng->time().monotonic_now();
	return std::max(0.0, std::chrono::duration<double, std::milli>(wait).count());
}

EMSCRIPTEN_KEEPALIVE int jaiweb_exit_code() { return g_session ? g_session->exit_code : 0; }

// Asyncify build: ends the run (its destructors run) and the engine with it
EMSCRIPTEN_KEEPALIVE void jaiweb_stop() {
	if (g_session) {
		if (g_session->run && !g_session->run->done()) { g_session->run->cancel(); }
		g_session->out.flush();
	}
	g_session.reset();
}

EMSCRIPTEN_KEEPALIVE void jaiweb_key(int a_code, int a_down) {
	const std::string key = key_name_of(a_code);
	if (key.empty()) { return; }
	if (a_down) {
		g_input.edges.push_back(key);
		if (g_input.edges.size() > 128) { g_input.edges.erase(g_input.edges.begin()); }
	}
	g_input.held[key] = a_down != 0;
}

EMSCRIPTEN_KEEPALIVE void jaiweb_release_keys() { g_input.held.clear(); }

EMSCRIPTEN_KEEPALIVE void jaiweb_resize(int a_cols, int a_rows) {
	g_input.cols = std::max(1, a_cols);
	g_input.rows = std::max(1, a_rows);
}

// Any build: runs a file in the live engine, where running the same path again reloads it in place. Returns "" or the error.
EMSCRIPTEN_KEEPALIVE const char* jaiweb_live_file(const char* a_path) {
	const std::string path = a_path ? a_path : "";
	return live_reply([&path](jai::engine& eng) {
		eng.execute_file_source(path, jai_runner::read_file_or_raise(path));
		return std::string();
	});
}

// Any build: evaluates source text in the live engine and returns its value as text (the error's text when it fails)
EMSCRIPTEN_KEEPALIVE const char* jaiweb_live_eval(const char* a_source) {
	const std::string source = a_source ? a_source : "";
	return live_reply([&source](jai::engine& eng) { return eng.execute(source).to_string(); });
}

EMSCRIPTEN_KEEPALIVE int jaiweb_live_failed() { return g_live.failed ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE const char* jaiweb_version() {
	static const std::string version = jai::version();
	return version.c_str();
}

}
