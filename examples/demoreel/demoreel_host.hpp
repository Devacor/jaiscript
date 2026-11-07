// demoreel shared host: the language-agnostic console shell. The host owns the VT console and
// its output filter, key polling, frame timing/pacing, the xorshift64* Rng, every
// command-line flag, and the harnesses (--smoke parity table with the FNV-1a
// frame-stream hash, --perf per-scene timing, --bench, --capture, --reload-test).
// A script_adapter supplies everything language-specific: runtime creation,
// host-API binding, script loading, and the demo_* entry-point calls.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace demoreel {

struct host_options {
	int64_t seed = 20260705;
	std::string backend;                 // "" = adapter default; validated against adapter.backends()
	bool backend_selected = false;       // explicit --backend narrows --smoke/--perf to that backend
	bool smoke = false;
	int64_t frames = 1700;               // smoke frame budget (covers every scene of the reel)
	std::string expect;                  // smoke: required stream hash (hex), exit 1 on mismatch
	int64_t perf = 0;                    // headless per-scene timing: N frames per scene
	int64_t perf_warmup = 0;             // untimed frames on the same runtime, followed by a reboot of the measured scene
	double perf_at = 3.0;                // perf: scene time the measured frames start from
	std::string csv;                     // perf: also write rows to this file
	int64_t workers = -1;                // parallel width; -1 engine default, 1 serial (ports: always serial)
	int64_t capture = -1;                // scene index to text-capture (README material)
	bool reload_test = false;            // headless hot-reload + backend-swap exercise
	int64_t width = 0;                   // 0 = autodetect
	int64_t height = 0;
	double target_fps = 30.0;
	std::string scenes_dir;
	int64_t bench = 0;                   // run N frames of the real loop, report sim/write split
	int64_t scene = -1;                  // jump here after boot (bench targeting)
	std::string bench_out;               // also write the bench report to this file
	int64_t filter_mode = -1;            // -1 auto, 0 off, 1 truecolor+tolerance, 2 xterm-256
	int64_t tol = 8;                     // per-channel merge tolerance for mode 1
	bool diff = false;                   // row-diff redraw (skip unchanged rows)
	bool ascii = false;                  // one density character per cell, no block glyphs
	const char* argv0 = "";
};

// Host capabilities handed to the adapter for binding into the script runtime.
struct host_services {
	std::function<void(const std::string&)> log;   // host_log: one line to stdout, never hashed
};

double now_seconds();
uint64_t fnv1a(uint64_t hash, const std::string& bytes);
const uint64_t fnv1a_offset = 14695981039346656037ull;
std::string slurp_file(const std::string& path);
// override dir > compiled-in source dir (if entry_file exists there) >
// <exe dir>/<local_dirname> > bare local_dirname
std::string locate_scenes_dir(const std::string& override_dir, const char* argv0,
                              const char* source_scenes_dir, const char* local_dirname,
                              const char* entry_file);

// Deterministic xorshift64*: the only randomness the scenes may touch, so the reel
// replays identically from a seed in every language. Bind this exact struct as the
// script-visible "Rng" class (constructor (int seed); next roll chance nextf).
struct demo_rng {
	uint64_t s;

	explicit demo_rng(int64_t seed)
		: s(seed != 0 ? static_cast<uint64_t>(seed) : 0x9E3779B97F4A7C15ull) {
		for (int i = 0; i < 4; ++i) { next_raw(); }
	}

	uint64_t next_raw() {
		s ^= s >> 12;
		s ^= s << 25;
		s ^= s >> 27;
		return s * 0x2545F4914F6CDD1Dull;
	}

	int64_t next(int64_t n) {
		if (n <= 0) { return 0; }
		return static_cast<int64_t>(next_raw() % static_cast<uint64_t>(n));
	}

	int64_t roll(int64_t lo, int64_t hi) {
		if (hi <= lo) { return lo; }
		return lo + next(hi - lo + 1);
	}

	bool chance(int64_t percent) { return next(100) < percent; }

	double nextf() {
		return static_cast<double>(next_raw() >> 11) * (1.0 / 9007199254740992.0);
	}
};

// demo_frame's arguments. Smoke/perf pass key "" and zeros for the perf floats.
struct frame_args {
	double t = 0.0;
	double dt = 0.0;
	std::string key;
	double fps = 0.0;
	double ms_sim = 0.0;       // last measured script cost of this session
	double ms_draw = 0.0;      // last console write
	double ms_other = 0.0;     // the other backend's last cost (HUD), 0 when none
};

// One loaded script runtime. The host drives it: load, boot, then frame per frame.
// Script errors surface as std::exception throws; stack_trace() is queried after one.
class script_session {
public:
	virtual ~script_session() = default;
	virtual void load() = 0;                              // resolve the scenes dir, load every script
	virtual std::string load_kind() { return "prse"; }   // 4-char smoke-table tag ("prse", "bite", ...)
	// demo_boot(w, h, seed, backend, smoke, hud): w x h terminal cells; smoke = short scenes
	virtual void boot(int64_t w, int64_t h, int64_t seed, const std::string& backend,
	                  bool smoke, bool hud) = 0;
	virtual std::string frame(const frame_args& a) = 0;  // demo_frame(...): the FULL frame, rows joined by \n
	virtual int64_t scene_index() = 0;                    // demo_scene_index()
	virtual double scene_time() = 0;                      // demo_scene_time()
	virtual int64_t scene_count() = 0;                    // demo_scene_count()
	virtual std::string scene_name() = 0;                 // demo_scene_name()
	virtual void jump(int64_t idx, double at_t) = 0;      // demo_jump(idx, at_t)
	virtual bool reload() { return false; }               // hot reload scene files + demo_reload(); false = unsupported
	virtual std::string stack_trace() { return ""; }
};

class script_adapter {
public:
	virtual ~script_adapter() = default;
	virtual std::string program_name() const = 0;         // "jai_demoreel", "demoreel_lua", ...
	virtual std::string language() const = 0;             // "JaiScript", "Lua 5.4 / sol2", ...
	// Backends --smoke and --perf run (in order) and --backend accepts.
	virtual std::vector<std::string> backends() const = 0;
	virtual std::string default_backend() const { return backends().back(); }
	virtual std::string normalize_backend(const std::string& b) const { return b; }
	// Sessions the interactive loop keeps live (TAB cycles them); the first is
	// replaced by the --backend choice.
	virtual std::vector<std::string> interactive_backends(const host_options&) const { return backends(); }
	// Adapter-specific flag at argv[i]: returns how many args it consumed (0 = unknown).
	virtual int parse_flag(int /*argc*/, char** /*argv*/, int /*i*/) { return 0; }
	virtual std::string usage() const { return ""; }
	// A fresh runtime with every host-API binding in place, scripts not yet loaded.
	virtual std::unique_ptr<script_session> make_session(const std::string& backend,
		const host_services& services, const host_options& opt) = 0;
};

// Full program: parse flags, run the selected harness or the interactive loop.
int run_demoreel(int argc, char** argv, script_adapter& adapter);

} // namespace demoreel
