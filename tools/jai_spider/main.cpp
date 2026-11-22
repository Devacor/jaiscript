// jai_spider - a web application host for JaiScript. `jai_spider app.jai` serves the app over HTTP/1.1: one network
// thread moves the bytes and hands each request to a pool of workers, and each request runs in an engine of its own,
// built from the app before the request arrived and destroyed after it, so no request sees what another left behind.
// A saved edit applies to the next request, SQLite is a database away, spider::every runs background tasks, and files
// under --root are served as they are. The host is a consumer of the library: sockets and HTTP (http_server.hpp), the
// text forms of HTTP (http_text.hpp), SQLite (database.hpp), the console (console.hpp) and the app's sources
// (sources.hpp) live here, request and router are script (lib/spider.jai), a response is the library's spider::response.
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/core/load_report.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include "../jaiscript/http_transport.hpp"
#include "../jaiscript/file_io.hpp"
#include "console.hpp"
#include "database.hpp"
#include "http_server.hpp"
#include "http_text.hpp"
#include "sources.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <jaiscript/testing/unattended.hpp>

namespace {

namespace fs = std::filesystem;
using steady = std::chrono::steady_clock;

std::atomic<bool> g_stop{false};   // Ctrl+C (host-exe state, not engine state)

#ifdef _WIN32
BOOL WINAPI ctrl_handler(DWORD type) {
	if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
		g_stop.store(true);
		return TRUE;
	}
	return FALSE;
}
#else
void stop_signal(int) { g_stop.store(true); }
#endif

struct options {
	std::string app;
	std::string host = "127.0.0.1";
	int port = 8080;
	std::string root;
	std::string backend = "vm";
	double budget = 30.0;
	int workers = 0;              // --workers N: 0 is one per hardware thread
	size_t queue = 1024;          // --queue N: requests that may wait for a free worker; past it 503
	spider_host::http_limits limits;
	std::string shutdown_token;   // --allow-shutdown TOKEN: a loopback POST /__spider/shutdown carrying it stops the server
	bool dev = false;             // --dev: error pages carry the error and its trace; lib/ is imported from the checkout
	bool quiet = false;
	std::string load_report;      // --load-report[=json]: where the first engine build spent its time, on stderr; empty = off
};

void print_usage() {
	std::puts(
		"jai_spider - a web application host for JaiScript\n"
		"\n"
		"usage:\n"
		"  jai_spider app.jai [options]\n"
		"\n"
		"The app hands its handler to spider::spin(handler) (spider::serve is the same call): a function of a\n"
		"spider::request, or a spider::router (lib/spider.jai has request, router and the html/json/redirect/\n"
		"file helpers; spider::response is the library's). Each request runs in a fresh engine of its own, so\n"
		"state lives in the database or the session, never in script globals. A saved edit applies to the next\n"
		"request; an edit that does not check is reported here and the app keeps serving the code it had.\n"
		"spider::every(seconds, fn) runs fn in the background; spider::catch / sling / get / post reach other servers.\n"
		"\n"
		"options:\n"
		"  --port N            port to listen on (default 8080; 0 picks a free one)\n"
		"  --host ADDR         IPv4 address to listen on (default 127.0.0.1; 0.0.0.0 for every interface)\n"
		"  --root DIR          serve the files under DIR for GET and HEAD before the app sees the request\n"
		"                      (default: the app's public/ folder when it exists)\n"
		"  --workers N         threads running requests, each with its next engine built ahead (default: one per\n"
		"                      hardware thread)\n"
		"  --queue N           requests that may wait for a free worker (default 1024); past it 503 with Retry-After\n"
		"  --max-body N        largest request body in bytes, K, M or G suffix allowed (default 2M); larger is 413\n"
		"  --timeout S         seconds a request may take to arrive and a response to leave (default 30)\n"
		"  --budget S          execution budget per request in seconds (default 30; 0 = unlimited)\n"
		"  --backend vm|interp execution backend (default vm)\n"
		"  --dev               development: an error page shows the error and its trace, and lib/ is imported\n"
		"                      from the source checkout so an edit to spider.jai applies on the next request\n"
		"  --load-report       print where the first engine build spent its time, per file and phase, on stderr\n"
		"                      (--load-report=json: the same as one line of JSON)\n"
		"  --allow-shutdown T  POST /__spider/shutdown from this machine with the header X-Spider-Shutdown: T\n"
		"                      stops the server\n"
		"  --quiet             no request log\n"
		"  --version / --help\n"
		"\n"
		"One network thread serves static files and keeps every connection moving; the workers run the app.\n"
		"Ctrl+C stops the server.\n");
}

bool parse_size(const std::string& text, size_t& out) {
	if (text.empty()) { return false; }
	size_t multiplier = 1;
	std::string digits = text;
	const char last = static_cast<char>(std::toupper(static_cast<unsigned char>(text.back())));
	if (last == 'K' || last == 'M' || last == 'G') {
		multiplier = last == 'K' ? 1024 : last == 'M' ? 1024 * 1024 : size_t{1024} * 1024 * 1024;
		digits.pop_back();
	}
	size_t count = 0;
	auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), count);
	if (digits.empty() || ec != std::errc{} || end != digits.data() + digits.size() || count > SIZE_MAX / multiplier) { return false; }
	out = count * multiplier;
	return out > 0;
}

// a whole number in [low, high], or a finite one for a double
template<typename Number>
bool parse_number(const std::string& text, Number low, Number high, Number& out) {
	Number value{};
	if constexpr (std::is_floating_point_v<Number>) {
		char* end = nullptr;
		value = static_cast<Number>(std::strtod(text.c_str(), &end));
		if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value)) { return false; }
	} else {
		auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (ec != std::errc{} || end != text.data() + text.size()) { return false; }
	}
	if (value < low || value > high) { return false; }
	out = value;
	return true;
}

int parse_args(int argc, char** argv, options& opt) {
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto is_flag = [&](const char* flag) {
			const size_t n = std::strlen(flag);
			return a.compare(0, n, flag) == 0 && (a.size() == n || a[n] == '=');
		};
		auto value_of = [&](const char* flag, std::string& out) {
			const size_t n = std::strlen(flag);
			if (a.size() > n) {
				out = a.substr(n + 1);
				return true;
			}
			if (i + 1 < argc) {
				out = argv[++i];
				return true;
			}
			std::fprintf(stderr, "jai_spider: missing value for %s\n", flag);
			return false;
		};
		auto refuse = [](const char* text) {
			std::fprintf(stderr, "jai_spider: %s\n", text);
			return 2;
		};
		std::string value;
		if (is_flag("--port")) {
			if (!value_of("--port", value)) { return 2; }
			if (!parse_number(value, 0, 65535, opt.port)) { return refuse("--port takes 0 to 65535"); }
		}
		else if (is_flag("--host")) { if (!value_of("--host", opt.host)) { return 2; } }
		else if (is_flag("--root")) { if (!value_of("--root", opt.root)) { return 2; } }
		else if (is_flag("--backend")) { if (!value_of("--backend", opt.backend)) { return 2; } }
		else if (is_flag("--budget")) {
			if (!value_of("--budget", value)) { return 2; }
			if (!parse_number(value, 0.0, 1e9, opt.budget)) { return refuse("--budget takes seconds from 0 (0 is unlimited)"); }
		}
		else if (is_flag("--timeout")) {
			if (!value_of("--timeout", value)) { return 2; }
			if (!parse_number(value, 0.001, 1e6, opt.limits.timeout_seconds)) { return refuse("--timeout takes seconds above 0"); }
		}
		else if (is_flag("--max-body")) {
			if (!value_of("--max-body", value)) { return 2; }
			if (!parse_size(value, opt.limits.max_body_bytes)) { return refuse("--max-body takes a byte count above 0 (2097152, 512K, 2M, 1G)"); }
		}
		else if (is_flag("--workers")) {
			if (!value_of("--workers", value)) { return 2; }
			if (!parse_number(value, 1, 1024, opt.workers)) { return refuse("--workers takes a count from 1 to 1024"); }
		}
		else if (is_flag("--queue")) {
			if (!value_of("--queue", value)) { return 2; }
			if (!parse_number(value, size_t{0}, size_t{1000000}, opt.queue)) { return refuse("--queue takes a count from 0 to 1000000"); }
		}
		else if (is_flag("--allow-shutdown")) {
			if (!value_of("--allow-shutdown", opt.shutdown_token)) { return 2; }
			if (opt.shutdown_token.empty()) { return refuse("--allow-shutdown takes a token the shutdown request carries"); }
		}
		else if (a == "--dev") { opt.dev = true; }
		else if (a == "--load-report") { opt.load_report = "table"; }
		else if (a == "--load-report=json") { opt.load_report = "json"; }
		else if (a == "--quiet") { opt.quiet = true; }
		else if (a == "--version" || a == "-v") { std::printf("jai_spider (JaiScript %s, SQLite %s)\n", jai::version().c_str(), sqlite3_libversion()); std::exit(0); }
		else if (a == "--help" || a == "-h" || a == "/?") { print_usage(); std::exit(0); }
		else if (!a.empty() && a[0] == '-') {
			std::fprintf(stderr, "jai_spider: unknown flag: %s\n", a.c_str());
			return 2;
		}
		else if (opt.app.empty()) { opt.app = a; }
		else {
			std::fprintf(stderr, "jai_spider: unexpected extra argument: %s\n", a.c_str());
			return 2;
		}
	}
	if (opt.backend == "interpreter") { opt.backend = "interp"; }
	if (opt.app.empty()) {
		print_usage();
		return 2;
	}
	if (opt.backend != "vm" && opt.backend != "interp") {
		std::fprintf(stderr, "jai_spider: unknown backend: %s (use vm or interp)\n", opt.backend.c_str());
		return 2;
	}
	if (opt.workers == 0) { opt.workers = static_cast<int>(std::max(1u, std::thread::hardware_concurrency())); }
	return -1;
}

struct console_colors {
	bool enabled = false;

	void init() {
#ifdef _WIN32
		HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD mode = 0;
		if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
			enabled = SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
		}
		SetConsoleOutputCP(CP_UTF8);
#else
		enabled = isatty(STDOUT_FILENO) != 0;
#endif
	}
	std::string paint(const char* code, const std::string& text) const { return enabled ? std::string("\x1b[") + code + "m" + text + "\x1b[0m" : text; }
	std::string red(const std::string& text) const { return paint("31", text); }
	std::string green(const std::string& text) const { return paint("32", text); }
	std::string yellow(const std::string& text) const { return paint("33", text); }
	std::string dim(const std::string& text) const { return paint("90", text); }
};

std::string html_text(std::string_view text) {
	std::string result;
	result.reserve(text.size());
	for (char c : text) {
		switch (c) {
			case '&': result += "&amp;"; break;
			case '<': result += "&lt;"; break;
			case '>': result += "&gt;"; break;
			case '"': result += "&quot;"; break;
			case '\'': result += "&#39;"; break;
			default: result += c;
		}
	}
	return result;
}

jai::script_value script_map_of(jai::engine* eng, std::vector<std::pair<std::string, jai::script_value>> entries) {
	jai::script_value result = jai::script_value::make_map(nullptr, nullptr, eng);
	auto& fields = result.as_map();
	for (auto& [key, value] : entries) { fields[jai::script_value(key, eng)] = std::move(value); }
	return result;
}

// The HTTP text forms the standard library leaves to a web host (lib/spider.jai builds spider::request on them)
void register_http_text(jai::engine& eng) {
	eng.add_function("parse_cookies", [](const std::string& header) { return spider_host::parse_cookies(header); });
	jai::engine* engine_pointer = &eng;
	eng.add_function("parse_multipart", [engine_pointer](const std::string& body, const std::string& content_type) {
		std::vector<spider_host::multipart_part> parts;
		if (!spider_host::parse_multipart(body, content_type, parts)) { throw std::runtime_error("parse_multipart: the body is not multipart/form-data with this boundary"); }
		jai::script_value result = jai::script_value::make_array(nullptr, engine_pointer);
		for (auto& part : parts) {
			result.as_array().push_back(script_map_of(engine_pointer, {
				{"name", jai::script_value(part.name, engine_pointer)},
				{"filename", jai::script_value(part.filename, engine_pointer)},
				{"content_type", jai::script_value(part.content_type, engine_pointer)},
				{"data", jai::script_value(std::move(part.data), engine_pointer)},
				{"is_file", jai::script_value(part.has_filename, engine_pointer)},
			}));
		}
		return result;
	});
	eng.add_function("http_date", [](jai::script_int unix_seconds) { return spider_host::http_date(static_cast<std::time_t>(unix_seconds)); });
	eng.add_function("content_type_for", [](const std::string& path) { return spider_host::content_type_for(fs::path(path).extension().string()); });
}

bool is_loopback(const std::string& address) { return address.rfind("127.", 0) == 0; }

// A Host header naming this machine: localhost, a 127.x address or [::1], with any port. A server listening on loopback
// answers only these, so a page whose own name resolves to 127.0.0.1 (DNS rebinding) cannot reach it.
bool host_names_loopback(const std::string& host) {
	std::string name = host;
	if (!name.empty() && name.front() == '[') {
		name = name.substr(0, name.find(']') + 1);
	} else {
		name = name.substr(0, name.find(':'));
	}
	return spider_host::iequals(name, "localhost") || name == "[::1]" || is_loopback(name);
}

// text for the console: control bytes and bytes past ASCII as \xNN, so a request cannot write terminal escapes
std::string printable(std::string_view text) {
	std::string result;
	for (unsigned char c : text) {
		if (c < 0x20 || c >= 0x7F) {
			char escaped[8];
			std::snprintf(escaped, sizeof(escaped), "\\x%02X", c);
			result += escaped;
		} else {
			result += static_cast<char>(c);
		}
	}
	return result;
}

bool is_header_name(std::string_view name) {
	return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
		return std::isalnum(static_cast<unsigned char>(c)) || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
	});
}

std::string trimmed(std::string text) {
	while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) { text.pop_back(); }
	return text;
}

// "0.25" and "60": a task's period as the log names it
std::string seconds_text(double seconds) {
	char text[32];
	std::snprintf(text, sizeof(text), "%g", seconds);
	return text;
}

// One engine built from the app for one request or task, or the failure that kept the app from loading. It is stale
// once a file it was built from moves.
struct app_build {
	std::shared_ptr<jai::engine> eng;
	std::string failure;
	spider_host::stamp_set watched;
	std::vector<double> task_periods;   // the app's spider::every declarations, in order

	bool stale() const { return !spider_host::stamps_hold(watched); }
};

// What waits for a worker: a request to answer, or a run of a background task
struct job {
	enum class kind { request, task };
	kind what = kind::request;
	spider_host::http_request request;
	spider_host::http_ticket ticket;
	steady::time_point arrived;
	size_t task = 0;
};

// First in, first out. Requests wait up to the bound; a task never waits behind it, and each has one run out at a time.
class job_queue {
public:
	explicit job_queue(size_t a_bound) : bound_(a_bound) {}

	bool push_request(job&& a_job) {
		{
			std::lock_guard<std::mutex> hold(lock_);
			if (closed_ || waiting_requests_ >= bound_) { return false; }
			++waiting_requests_;
			jobs_.push_back(std::move(a_job));
		}
		ready_.notify_one();
		return true;
	}

	void push_task(job&& a_job) {
		{
			std::lock_guard<std::mutex> hold(lock_);
			if (closed_) { return; }
			jobs_.push_back(std::move(a_job));
		}
		ready_.notify_one();
	}

	// the next job, or nullopt once the queue is closed
	std::optional<job> pop() {
		std::unique_lock<std::mutex> hold(lock_);
		ready_.wait(hold, [this] { return closed_ || !jobs_.empty(); });
		if (closed_) { return std::nullopt; }
		job next = std::move(jobs_.front());
		jobs_.pop_front();
		if (next.what == job::kind::request) { --waiting_requests_; }
		return next;
	}

	void close() {
		{
			std::lock_guard<std::mutex> hold(lock_);
			closed_ = true;
		}
		ready_.notify_all();
	}

private:
	std::mutex lock_;
	std::condition_variable ready_;
	std::deque<job> jobs_;
	size_t waiting_requests_ = 0;
	size_t bound_;
	bool closed_ = false;
};

// spider::every's tasks, as the newest build that checked declared them: due times, whether a run is out, and whether
// the task stopped (a run returned false)
class task_schedule {
public:
	// the periods a build declared: a task keeps its place, schedule and stopped state while its period stays, and a new
	// or changed one first runs a period from now
	void declare(const std::vector<double>& a_periods) {
		{
			std::lock_guard<std::mutex> hold(lock_);
			const auto now = steady::now();
			tasks_.resize(a_periods.size());
			for (size_t i = 0; i < a_periods.size(); ++i) {
				if (tasks_[i].seconds != a_periods[i]) {
					tasks_[i].seconds = a_periods[i];
					tasks_[i].due = now + period(a_periods[i]);
					tasks_[i].stopped = false;
				}
			}
		}
		wake_.notify_all();
	}

	// runs on its own thread until stop: hands each due task, not already out, to dispatch
	void run(const std::function<void(size_t)>& a_dispatch) {
		std::unique_lock<std::mutex> hold(lock_);
		while (!stopping_) {
			const auto now = steady::now();
			auto next = now + std::chrono::hours(1);
			for (size_t i = 0; i < tasks_.size(); ++i) {
				scheduled& task = tasks_[i];
				if (task.out || task.stopped) { continue; }
				if (task.due <= now) {
					task.out = true;
					task.due = now + period(task.seconds);
					a_dispatch(i);
					continue;
				}
				next = std::min(next, task.due);
			}
			wake_.wait_until(hold, next);
		}
	}

	// a run ended: the next one starts a period after this one started, and never before this one ended
	void finished(size_t a_task, bool a_stop) {
		{
			std::lock_guard<std::mutex> hold(lock_);
			if (a_task >= tasks_.size()) { return; }
			tasks_[a_task].out = false;
			tasks_[a_task].stopped = tasks_[a_task].stopped || a_stop;
			tasks_[a_task].due = std::max(tasks_[a_task].due, steady::now());
		}
		wake_.notify_all();
	}

	void stop() {
		{
			std::lock_guard<std::mutex> hold(lock_);
			stopping_ = true;
		}
		wake_.notify_all();
	}

private:
	struct scheduled {
		double seconds = 0;
		steady::time_point due;
		bool out = false;
		bool stopped = false;
	};

	static steady::duration period(double seconds) {
		return std::chrono::duration_cast<steady::duration>(std::chrono::duration<double>(std::min(seconds, 1e9)));
	}

	std::mutex lock_;
	std::condition_variable wake_;
	std::vector<scheduled> tasks_;
	bool stopping_ = false;
};

class app_host {
public:
	app_host(const options& chosen, const console_colors& palette) :
		opt_(chosen),
		colors_(palette),
		jobs_(chosen.queue) {
	}

	~app_host() { stop(); }

	// resolves the paths and builds the app once on this thread, to report how it loads before the server listens
	bool start() {
		std::error_code ec;
		app_path_ = fs::weakly_canonical(fs::absolute(opt_.app, ec), ec);
		if (!fs::is_regular_file(app_path_, ec)) {
			console_.err(colors_.red("jai_spider: no such file: " + opt_.app) + "\n");
			return false;
		}
		app_dir_ = app_path_.parent_path();
		sources_.keep_copies_in(app_dir_ / ".cache");
		if (!opt_.root.empty()) {
			static_root_ = fs::canonical(fs::absolute(opt_.root, ec), ec);
			if (ec || !fs::is_directory(static_root_, ec)) {
				console_.err(colors_.red("jai_spider: --root is not a folder: " + opt_.root) + "\n");
				return false;
			}
		} else if (fs::is_directory(app_dir_ / "public", ec)) {
			static_root_ = fs::canonical(app_dir_ / "public", ec);
		}
		if (sqlite3_threadsafe() == 0 && opt_.workers > 1) {
			console_.err(colors_.yellow("jai_spider: this SQLite was built without threads (SQLITE_THREADSAFE=0); serving with one worker") + "\n");
			workers_wanted_ = 1;
		} else {
			workers_wanted_ = opt_.workers;
		}
		const auto began = steady::now();
		std::shared_ptr<app_build> first = build_app(true);
		if (!first) { return false; }
		build_ms_ = std::chrono::duration<double, std::milli>(steady::now() - began).count();
		return true;
	}

	// the workers and the task schedule; each worker builds its first engine before it takes a job
	void start_workers(spider_host::http_server& a_server) {
		server_ = &a_server;
		for (int i = 0; i < workers_wanted_; ++i) { workers_.emplace_back([this] { work(); }); }
		scheduler_ = std::thread([this] {
			schedule_.run([this](size_t task) {
				job run;
				run.what = job::kind::task;
				run.task = task;
				run.arrived = steady::now();
				jobs_.push_task(std::move(run));
			});
		});
	}

	// the workers finish the job in hand and stop; what still waits is dropped
	void stop() {
		stopping_.store(true);
		jobs_.close();
		schedule_.stop();
		for (auto& worker : workers_) {
			if (worker.joinable()) { worker.join(); }
		}
		workers_.clear();
		if (scheduler_.joinable()) { scheduler_.join(); }
	}

	// on the network thread: what the host answers itself (421, the shutdown request, static files) answers now; the
	// app's requests go to the workers
	std::optional<spider_host::http_response> dispatch(spider_host::http_request& request, spider_host::http_ticket ticket) {
		const auto start = steady::now();
		spider_host::http_response response;
		bool from_disk = false;
		const std::string* host = request.header("host");
		if (is_loopback(opt_.host) && host && !host_names_loopback(*host)) {
			response = spider_host::plain_response(421, "421 Misdirected Request: this server answers localhost, 127.x and [::1] only");
		} else if (!opt_.shutdown_token.empty() && request.path == "/__spider/shutdown" && request.method == "POST" && is_loopback(request.remote_address)) {
			const std::string* token = request.header("x-spider-shutdown");
			if (token && *token == opt_.shutdown_token) {
				response = spider_host::plain_response(200, "jai_spider: stopping");
				response.stop_server = true;
				response.close = true;
			} else {
				response = spider_host::plain_response(403, "403 Forbidden: the shutdown request needs X-Spider-Shutdown with the --allow-shutdown token");
			}
		} else if (auto file = static_file(request)) {
			response = std::move(*file);
			from_disk = true;
		} else {
			const std::string method = request.method;
			const std::string target = request.target;
			job answer;
			answer.request = std::move(request);
			answer.ticket = ticket;
			answer.arrived = start;
			if (jobs_.push_request(std::move(answer))) { return std::nullopt; }
			response = spider_host::plain_response(503, "503 Service Unavailable: every worker is busy and " + std::to_string(opt_.queue) + " requests wait already");
			response.set_header("Retry-After", "1");
			log_request(method, target, response.status, start, false);
			return response;
		}
		log_request(request.method, request.target, response.status, start, from_disk);
		return response;
	}

	const fs::path& root() const { return static_root_; }
	int workers() const { return workers_wanted_; }
	double build_ms() const { return build_ms_; }
	spider_host::console& console() { return console_; }

	// the server whose body limit spider::spin's max_body moves
	void attach(spider_host::http_server& a_server) { server_ = &a_server; }

private:

	// the copy the build put beside the executable; under --dev the checkout's lib/ while it is there, so an edit to
	// spider.jai applies like an edit to the app
	fs::path library_dir() const {
		std::error_code ec;
		const fs::path source = fs::path(JAI_SPIDER_SOURCE_LIBRARY_DIR);
		if (opt_.dev && fs::is_regular_file(source / "spider.jai", ec)) { return source; }
		return fs::path(jai::engine::executable_path()).parent_path() / "jai_spider_lib";
	}

	// One worker: an engine built ahead waits for the next job; the engine that ran a job is destroyed and the next one
	// is built after the answer has gone, so neither the build nor the teardown is on a request's path
	void work() {
		std::shared_ptr<app_build> ready = build_app(false);
		while (!stopping_.load()) {
			std::optional<job> next = jobs_.pop();
			if (!next) { break; }
			if (!ready || ready->stale()) { ready = build_app(false); }
			if (next->what == job::kind::request) {
				answer(*ready, *next);
			} else {
				run_task(*ready, next->task);
			}
			ready.reset();
			if (stopping_.load()) { break; }
			ready = build_app(false);
		}
	}

	// A fresh engine for the app: from the files on disk, or from the last good texts while an edit there does not
	// check. A failure is reported once; at start, a lib/spider.jai that does not load is fatal (null). Only a build
	// that is not stale when it ends is judged (app_sources asks under the lock that stores the verdict), as the last
	// good one or as a failure to report: one of an older
	// version that finishes late, one that caught a file between two writes of a save, and one that loaded a file
	// twice across a save are stale, so the next build judges the edit.
	std::shared_ptr<app_build> build_app(bool at_start) {
		if (auto serving = sources_.serving_last_good()) { return build_last_good(*serving->second, std::move(serving->first)); }
		auto record = std::make_shared<spider_host::stamp_record>();
		auto built = std::make_shared<app_build>();
		bool library_failed = false;
		built->eng = build_engine(app_path_, {app_dir_, library_dir()}, record, built->failure, library_failed, at_start && !opt_.load_report.empty());
		built->watched = record->files();
		if (library_failed && at_start) {
			console_.err(colors_.red("jai_spider: lib/spider.jai did not load:") + "\n" + built->failure + "\n");
			return nullptr;
		}
		if (built->eng) {
			const spider_host::source_news news = sources_.note_good(built->watched, record->texts());
			report(news);
			finish_build(*built, news.current);
			return built;
		}
		report(sources_.note_disk_failure(built->watched, built->failure));
		if (auto good = sources_.last_good()) { return build_last_good(*good, std::move(built->watched)); }
		return built;
	}

	// an engine from the snapshot's texts, watching the edit that failed (and the snapshot's own files), so a fix lands
	std::shared_ptr<app_build> build_last_good(const spider_host::source_snapshot& snapshot, spider_host::stamp_set failed) {
		auto built = std::make_shared<app_build>();
		built->watched = std::move(failed);
		for (const auto& [path, stamp] : snapshot.stamps) { built->watched.emplace(path, stamp); }
		std::string error;
		const spider_host::shadow_folder shadow = sources_.shadow_of(snapshot, app_dir_, library_dir(), error);
		if (shadow.folder.empty()) {
			built->failure = "the last good code could not be written out: " + error;
			return built;
		}
		const auto mirror = [&](const fs::path& path) { return shadow.of(path).value_or(path); };
		bool library_failed = false;
		built->eng = build_engine(mirror(app_path_), {mirror(app_dir_), mirror(library_dir())}, nullptr, built->failure, library_failed);
		if (built->eng) {
			finish_build(*built, false);
		} else {
			report(sources_.note_failure(built->watched, built->failure));
		}
		return built;
	}

	// the engine with the host's registrations, the library and the app imported; null with the failure on error
	std::shared_ptr<jai::engine> build_engine(const fs::path& app, const std::vector<fs::path>& include_paths,
			const std::shared_ptr<spider_host::stamp_record>& record, std::string& failure, bool& library_failed, bool report_load = false) {
		const auto began = steady::now();
		auto eng = jai::engine::make();
		eng->set_backend(opt_.backend == "interp" ? jai::backend_type::interpreter : jai::backend_type::vm);
		eng->execution_budget(opt_.budget);
		eng->set_output_stream(std::make_shared<spider_host::console_stream>(console_));
		if (record) {
			eng->file_stamp_source([record](const std::string& path) { return record->stamp(path); });
			eng->source_file_observer([record](const std::string& path, const std::optional<jai::engine::file_stamp>& stamp, std::string_view bytes) {
				record->loaded(path, stamp, bytes);
			});
		}
		jai::stdlib::register_all(eng);
		jai::stdlib::register_web(eng);
#ifdef JAISCRIPT_ENABLE_IMAGE
		jai::stdlib::register_image(*eng);
#endif
		jai_runner::register_http_client(*eng);
		jai_runner::register_file_io(*eng);
		register_http_text(*eng);
		spider_host::register_database(*eng, app_dir_);
		for (const fs::path& path : include_paths) { eng->add_include_path(path.generic_string()); }
		if (report_load) {
			eng->load_report(true);
			eng->load_report_setup_time(std::chrono::duration<double>(steady::now() - began).count());
		}
		try {
			eng->execute("import \"spider.jai\";");
		} catch (const std::exception& e) {
			failure = trimmed(e.what());
			library_failed = true;
			return nullptr;
		}
		std::string escaped;
		for (char c : app.generic_string()) {
			if (c == '"' || c == '\\') { escaped += '\\'; }
			escaped += c;
		}
		try {
			eng->execute("import(\"" + escaped + "\")");
		} catch (const std::exception& e) {
			failure = trimmed(e.what());
			return nullptr;
		}
		if (report_load) {
			const jai::load_report_summary report = eng->load_report_snapshot();
			console_.err(opt_.load_report == "json" ? report.json() + "\n" : report.format());
			eng->load_report(false);
		}
		return eng;
	}

	// spider::spin's options and spider::every's declarations, read from the engine just built; the declarations of the
	// last good build are the schedule
	void finish_build(app_build& built, bool newest) {
		double budget = opt_.budget;
		size_t max_body = opt_.limits.max_body_bytes;
		try {
			auto spun = built.eng->get_variable("spider_host_options").as_function()({});
			if (spun && spun.value().is_map()) {
				for (const auto& [key, value] : spun.value().as_map()) {
					const std::string name = key.as_string();
					const jai::script_value& setting = value.deref();
					if (name == "budget" && (setting.is_int() || setting.is_float())) { budget = setting.is_int() ? static_cast<double>(setting.as_int()) : setting.as_float(); }
					if (name == "max_body" && setting.is_int() && setting.as_int() > 0) { max_body = static_cast<size_t>(setting.as_int()); }
				}
			}
			auto declared = built.eng->get_variable("spider_host_tasks").as_function()({});
			if (declared && declared.value().is_array()) {
				declared.value().unchecked_array_node()->for_each(built.eng.get(), [&](const jai::script_value& seconds) {
					built.task_periods.push_back(seconds.is_int() ? static_cast<double>(seconds.as_int()) : seconds.as_float());
				});
			}
		} catch (const std::exception&) {
		}
		built.eng->execution_budget(budget);
		if (server_) { server_->max_body(max_body); }
		if (newest) { schedule_.declare(built.task_periods); }
	}

	void report(const spider_host::source_news& news) {
		if (!news.reloaded.empty()) { console_.out(colors_.green("jai_spider: reloaded " + news.reloaded) + "\n"); }
		if (news.loads) { console_.out(colors_.green("jai_spider: the app loads") + "\n"); }
		if (!news.failure.empty()) {
			console_.err(colors_.red(news.failure_after_good ? "jai_spider: the edit was rejected; serving the code the app had:" : "jai_spider: the app did not load:")
				+ "\n" + news.failure + "\n");
		}
	}

	void log_request(const std::string& method, const std::string& target, int status, steady::time_point start, bool from_disk) {
		if (opt_.quiet) { return; }
		const double ms = std::chrono::duration<double, std::milli>(steady::now() - start).count();
		char timing[32];
		std::snprintf(timing, sizeof(timing), "%.2f ms", ms);
		const std::string code = std::to_string(status);
		const std::string painted = status >= 500 ? colors_.red(code) : status >= 400 ? colors_.yellow(code) : colors_.green(code);
		console_.out(method + " " + printable(target) + " " + painted + " " + colors_.dim(std::string(timing) + (from_disk ? " (file)" : "")) + "\n");
	}

	void answer(const app_build& built, job& request_job) {
		const spider_host::http_request& request = request_job.request;
		spider_host::http_response response = run_app(built, request);
		const int status = response.status;
		server_->complete(request_job.ticket, std::move(response));
		log_request(request.method, request.target, status, request_job.arrived, false);
	}

	void run_task(const app_build& built, size_t task) {
		std::string error;
		bool stop = false;
		if (built.eng) {
			try {
				auto ran = built.eng->get_variable("spider_host_run_task").as_function()({jai::script_value(static_cast<jai::script_int>(task), built.eng.get())});
				if (!ran) {
					error = built.eng->format_error(ran) + "\n" + built.eng->format_stack_trace();
				} else {
					stop = ran.value().is_bool() && !ran.value().as_bool();
				}
			} catch (const std::exception& e) {
				error = e.what();
			}
		}
		const std::string seconds = task < built.task_periods.size() ? seconds_text(built.task_periods[task]) : "?";
		if (!error.empty()) {
			console_.err(colors_.red("jai_spider: spider::every task " + std::to_string(task + 1) + " (every " + seconds + " s) failed:") + "\n" + trimmed(error) + "\n");
		} else if (stop) {
			console_.err("jai_spider: spider::every task " + std::to_string(task + 1) + " (every " + seconds + " s) stopped: it returned false\n");
		}
		schedule_.finished(task, stop);
	}

	// the error and its trace go to the log; the page shows them only under --dev
	spider_host::http_response error_page(const std::string& detail) {
		spider_host::http_response response;
		response.status = 500;
		response.set_header("Content-Type", "text/html; charset=utf-8");
		response.set_header("Cache-Control", "no-store");
		response.body = "<!doctype html><meta charset=\"utf-8\"><title>500 Internal Server Error</title>"
			"<style>body{font:15px/1.5 system-ui,sans-serif;margin:2rem;color:#222}pre{background:#f6f6f6;padding:1rem;overflow:auto;"
			"border-left:4px solid #c33}</style><h1>500 Internal Server Error</h1>";
		if (opt_.dev) { response.body += "<pre>" + html_text(detail) + "</pre>"; }
		response.body += "\n";
		return response;
	}

	spider_host::http_response run_app(const app_build& built, const spider_host::http_request& request) {
		if (!built.eng) { return error_page("The app did not load:\n" + built.failure); }
		jai::engine* eng = built.eng.get();
		jai::script_value raw = script_map_of(eng, {
			{"method", jai::script_value(request.method, eng)},
			{"target", jai::script_value(request.target, eng)},
			{"path", jai::script_value(request.path, eng)},
			{"query_string", jai::script_value(request.query_string, eng)},
			{"version", jai::script_value(request.version, eng)},
			{"headers", eng->make_value(request.headers)},
			{"body", jai::script_value(request.body, eng)},
			{"remote_addr", jai::script_value(request.remote_address, eng)},
		});
		std::string error;
		try {
			jai::script_value serve = eng->get_variable("spider_host_serve");
			auto result = serve.as_function()({raw});
			if (result) { return response_from(*eng, request, result.value()); }
			error = eng->format_error(result) + "\n" + eng->format_stack_trace();
		} catch (const std::exception& e) {
			error = e.what();
		}
		error = trimmed(error);
		console_.err(colors_.red("jai_spider: " + request.method + " " + printable(request.path) + " failed:") + "\n" + error + "\n");
		return error_page(error);
	}

	static jai::script_value field_of(jai::engine& eng, const jai::script_value& parts, const char* key) {
		const auto& fields = parts.as_map();
		auto it = fields.find(jai::script_value(std::string(key), &eng));
		if (it == fields.end()) { throw std::runtime_error(std::string("spider_host_serve's answer has no '") + key + "'"); }
		return it->second;
	}

	spider_host::http_response response_from(jai::engine& eng, const spider_host::http_request& request, const jai::script_value& parts) {
		spider_host::http_response response;
		const jai::script_int status = field_of(eng, parts, "status").as_int();
		if (status < 200 || status > 599) {
			throw std::runtime_error("Response status " + std::to_string(status) + " is not a final HTTP status (200 to 599)");
		}
		response.status = static_cast<int>(status);
		const std::string file = field_of(eng, parts, "file").as_string();
		for (const auto& [key, value] : field_of(eng, parts, "headers").as_map()) {
			const std::string name = key.as_string();
			const std::string text = value.as_string();
			if (!is_header_name(name) || text.find_first_of(std::string_view("\r\n\0", 3)) != std::string::npos) {
				throw std::runtime_error("Response header '" + printable(name) + "' is not a header name, or its value holds a line break or NUL");
			}
			if (spider_host::is_hop_header(name) || spider_host::iequals(name, "x-sendfile")) { continue; }
			response.set_header(name, text);
		}
		field_of(eng, parts, "cookies").unchecked_array_node()->for_each(&eng, [&](const jai::script_value& cookie) {
			const std::string line = cookie.as_string();
			if (line.find_first_of(std::string_view("\r\n\0", 3)) != std::string::npos) {
				throw std::runtime_error("Response cookie '" + printable(line) + "' holds a line break or NUL");
			}
			response.headers.emplace_back("Set-Cookie", line);
		});
		if (!file.empty()) {
			fs::path path = fs::path(file);
			if (path.is_relative()) { path = app_dir_ / path; }
			auto served = file_response(path, request, response.status);
			if (!served) {
				console_.err(colors_.red("jai_spider: file(\"" + printable(file) + "\"): no such file") + "\n");
				return spider_host::plain_response(404, "Not Found");
			}
			for (auto& [name, text] : response.headers) { served->set_header(name, text); }
			return std::move(*served);
		}
		response.body = field_of(eng, parts, "body").as_string();
		if (!response.body.empty() && !response.has_header("content-type")) { response.set_header("Content-Type", "text/html; charset=utf-8"); }
		return response;
	}

	// a file under --root for GET and HEAD; a dot-file or dot-folder (judged on the file's real name, so an 8.3 short name
	// such as ENV~1 is no way around it) and anything outside the root are never served
	std::optional<spider_host::http_response> static_file(const spider_host::http_request& request) {
		if (static_root_.empty() || (request.method != "GET" && request.method != "HEAD")) { return std::nullopt; }
		fs::path candidate = static_root_;
		size_t start = 1;
		while (start <= request.path.size()) {
			size_t end = request.path.find('/', start);
			if (end == std::string::npos) { end = request.path.size(); }
			const std::string segment = request.path.substr(start, end - start);
			if (!segment.empty()) {
				if (segment[0] == '.' || segment.find_first_of("\\:") != std::string::npos) { return std::nullopt; }
				candidate /= fs::path(reinterpret_cast<const char8_t*>(segment.c_str()));
			}
			start = end + 1;
		}
		std::error_code ec;
		if (fs::is_directory(candidate, ec)) { candidate /= "index.html"; }
		if (!fs::is_regular_file(candidate, ec)) { return std::nullopt; }
		const fs::path resolved = fs::canonical(candidate, ec);
		const std::string root_text = static_root_.generic_string() + "/";
		if (ec || resolved.generic_string().rfind(root_text, 0) != 0) { return std::nullopt; }
		for (const fs::path& part : resolved.lexically_relative(static_root_)) {
			if (part.native().empty() || part.native().front() == '.') { return std::nullopt; }
		}
		return file_response(resolved, request, 200);
	}

	std::optional<spider_host::http_response> file_response(const fs::path& path, const spider_host::http_request& request, int status) const {
		std::error_code ec;
		if (!fs::is_regular_file(path, ec)) { return std::nullopt; }
		const uint64_t size = fs::file_size(path, ec);
		const auto modified = fs::last_write_time(path, ec);
#ifdef _MSC_VER
		const auto modified_utc = std::chrono::clock_cast<std::chrono::system_clock>(modified);
#else
		const auto modified_utc = fs::file_time_type::clock::to_sys(modified);
#endif
		const auto since_epoch = std::chrono::duration_cast<std::chrono::seconds>(modified_utc.time_since_epoch()).count();
		char tag[64];
		std::snprintf(tag, sizeof(tag), "W/\"%llx-%llx\"", static_cast<unsigned long long>(size),
			static_cast<unsigned long long>(modified.time_since_epoch().count()));
		const std::string last_modified = spider_host::http_date(static_cast<std::time_t>(since_epoch));
		spider_host::http_response response;
		response.status = status;
		response.set_header("Content-Type", spider_host::content_type_for(path.extension().string()));
		response.set_header("Last-Modified", last_modified);
		response.set_header("ETag", tag);
		response.set_header("Cache-Control", "no-cache");
		if (status == 200) {
			const std::string* match = request.header("if-none-match");
			const std::string* since = request.header("if-modified-since");
			if ((match && *match == tag) || (!match && since && *since == last_modified)) {
				response.status = 304;
				return response;
			}
		}
		response.file_path = path.string();
		response.file_size = size;
		return response;
	}

	// Shared by every thread, each guarded where it is defined: opt_, colors_ and the paths are written once in start()
	// and only read after; console_, sources_, jobs_ and schedule_ lock inside; server_->complete and max_body are the
	// server's thread-safe entries.
	const options& opt_;
	const console_colors& colors_;
	spider_host::console console_;
	spider_host::app_sources sources_;
	job_queue jobs_;
	task_schedule schedule_;
	std::vector<std::thread> workers_;
	std::thread scheduler_;
	std::atomic<bool> stopping_{false};
	fs::path app_path_;
	fs::path app_dir_;
	fs::path static_root_;
	spider_host::http_server* server_ = nullptr;
	int workers_wanted_ = 1;
	double build_ms_ = 0;
};

} // namespace

int main(int argc, char** argv) {
	jai::foundry::route_faults_to_stderr();
	options opt;
	if (int code = parse_args(argc, argv, opt); code >= 0) { return code; }
	console_colors colors;
	colors.init();
#ifdef _WIN32
	SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
	std::signal(SIGINT, stop_signal);
	std::signal(SIGTERM, stop_signal);
	std::signal(SIGPIPE, SIG_IGN);
#endif
	try {
		spider_host::http_server server(opt.limits);
		app_host host(opt, colors);   // after the server: its workers stop before the server goes
		host.attach(server);
		if (!host.start()) { return 1; }
		std::string error;
		if (!server.listen(opt.host, opt.port, error)) {
			host.console().err(colors.red("jai_spider: " + error) + "\n");
			return 1;
		}
		spider_host::console& out = host.console();
		if (!opt.quiet) { out.out(colors.dim("/\\(o.o)/\\  jai_spider " + jai::version()) + "\n"); }
		std::string spinning = "jai_spider: spinning " + opt.app + " on http://" + opt.host + ":" + std::to_string(server.port()) + "/";
		if (!host.root().empty()) { spinning += " (files from " + host.root().string() + ")"; }
		out.out(spinning + "\n");
		char built[192];
		std::snprintf(built, sizeof(built), "jai_spider: %d worker%s, a fresh engine for each request (the first build took %.1f ms); edits apply on the next request; Ctrl+C stops",
			host.workers(), host.workers() == 1 ? "" : "s", host.build_ms());
		out.out(colors.dim(built) + "\n");
		server.on_rejected([&](const std::string& remote, int status, const std::string& why) {
			if (!opt.quiet) { out.out(colors.yellow("jai_spider: " + std::to_string(status) + " to " + remote + ": " + why) + "\n"); }
		});
		host.start_workers(server);
		server.run(g_stop, [&](spider_host::http_request& request, spider_host::http_ticket ticket) { return host.dispatch(request, ticket); });
		host.stop();
		out.out("jai_spider: stopped\n");
	} catch (const std::exception& e) {
		std::cerr << "jai_spider: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
