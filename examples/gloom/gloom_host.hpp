// GLOOM shared host: the language-agnostic console shell. reference/README.md is the
// game/host spec. The host owns: VT console, held-key state + key events, a monotonic
// clock, the xorshift64* Rng, command-line flags, and the --smoke/--bench
// harnesses with their hashes. A script_adapter supplies everything
// language-specific: engine creation, host-API binding, script loading, and the
// six gloom_* entry-point calls.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gloom {

struct host_options {
	int64_t seed = 666;
	std::string backend;             // "" = adapter default; validated against adapter.backends()
	bool smoke = false;
	bool backend_selected = false;  // explicit --backend narrows smoke to that backend
	int64_t ticks = 2000;            // smoke tick budget
	int64_t warmup_ticks = 0;        // smoke: untimed ticks, then reboot the same runtime
	int64_t workers = -1;            // parallel width; -1 = engine default, 0 = serial (ports: always serial)
	uint32_t miss_every = 0;         // jai: engine::cells_miss_every (--miss-every N; a build with JAISCRIPT_MISS_INJECTION)
	std::string coverage;            // jai: the vm's execution-coverage census written here at exit (--coverage FILE; a build with JAISCRIPT_VM_COVERAGE)
	bool param_alias = true;         // jai: engine::parameter_alias (--no-param-alias for an A/B on one binary)
	bool local_alias = true;         // jai: engine::local_alias (--no-local-alias for an A/B on one binary)
	int64_t width = 0;               // 0 = autodetect
	int64_t height = 0;
	double target_fps = 30.0;
	std::string scripts_dir;
	int64_t bench = 0;               // run N frames of the real loop headless-input, report sim/draw split
	int64_t dump_frame = -1;         // smoke: write frame N to gloom_frame_<backend>.txt
	int64_t coverage_reset_tick = -1; // smoke: restart the VM's coverage counters at tick N (a coverage build; hashes unchanged)
	bool god = false;
	std::string pix = "quad";        // sub-cell pixel mode: half (1x2) | quad (2x2) | sext (2x3)
	// Presentation only: the ports draw the reference look and may ignore all three.
	std::string look;                // reference | showcase; resolved by run_gloom (reference under --smoke)
	int truecolor = -1;              // 1 24-bit, 0 xterm-256; resolved by run_gloom from the terminal
	bool ascii = false;              // plain ASCII cells for terminals without block glyphs
	const char* argv0 = "";
};

// Host capabilities handed to the adapter for binding into the script runtime.
// key_down is already gated: it returns false permanently in smoke/bench runs.
// A POSIX terminal sends no key releases: a key reads as held while its auto-repeat keeps arriving.
struct host_services {
	std::function<bool(const std::string&)> key_down;
	std::function<void(const std::string&)> log;       // host_log: one line to stdout, never hashed
};

double now_seconds();
uint64_t fnv1a(uint64_t hash, const std::string& bytes);
std::string utf8_encode(int64_t codepoint);
// 0 half (1x2), 1 quad (2x2), 2 sext (2x3) pixels per cell — bind as HOST_PIX
inline int64_t pix_mode_index(const std::string& pix) {
	return pix == "half" ? 0 : (pix == "sext" ? 2 : 1);
}
// override dir > compiled-in source dir (if entry_file exists there) >
// <exe dir>/<local_dirname> > bare local_dirname
std::string locate_scripts_dir(const std::string& override_dir, const char* argv0,
                               const char* source_scripts_dir, const char* local_dirname,
                               const char* entry_file);

// Deterministic xorshift64* — the ONLY randomness the game may touch, so runs
// replay identically from a seed in every port (that is the parity contract).
// Bind this exact struct as the script-visible "Rng" class in every language.
struct gloom_rng {
	uint64_t s;

	explicit gloom_rng(int64_t seed)
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

	int64_t state() const { return static_cast<int64_t>(s & 0x3FFFFFFFFFFFFFFFull); }
};

// One booted script runtime. The host drives it: load_scripts, [force_autopilot,]
// boot, then frame per frame; state_hash/summary at the end. Script errors
// surface as std::exception throws; stack_trace() is queried after a throw.
class script_session {
public:
	virtual ~script_session() = default;
	virtual void load_scripts() = 0;                    // resolve scripts dir + load/parse everything
	virtual void force_autopilot() = 0;                 // gloom_force_autopilot() — before boot (--bench)
	virtual void boot(int64_t w, int64_t h) = 0;        // gloom_boot(w, h) — once after load
	virtual std::string frame(double dt, const std::string& key,
	                          double fps, double ms_sim, double ms_draw) = 0;   // gloom_frame(...)
	virtual int64_t state_hash() = 0;                   // gloom_state_hash()
	virtual bool wants_quit() = 0;                      // gloom_wants_quit()
	virtual void summary() = 0;                         // gloom_summary()
	virtual std::string stack_trace() { return ""; }    // script backtrace of the last error, "" if none
	virtual void coverage_reset() {}                    // a coverage build: restart the VM's counters
};

class script_adapter {
public:
	virtual ~script_adapter() = default;
	virtual std::string program_name() const = 0;       // "jai_gloom", "gloom_lua", ...
	virtual std::string language() const = 0;           // "JaiScript", "Lua/sol2", ...
	// Backends --smoke runs (in order) and --backend accepts. Single-runtime
	// ports return one name, as JaiScript does ({"vm"}).
	virtual std::vector<std::string> backends() const = 0;
	virtual std::string default_backend() const { return backends().back(); }
	virtual std::string normalize_backend(const std::string& b) const { return b; }
	virtual std::string title() const {
		return program_name() + " - GLOOM, a terminal raycast shooter (" + language() + " port)";
	}
	// Fresh engine + host-API bindings (reference/README.md section 2), scripts not yet
	// loaded. live_input=false in smoke/bench (services.key_down is gated too).
	virtual std::unique_ptr<script_session> make_session(const std::string& backend,
		const host_services& services, const host_options& opt, bool live_input) = 0;
};

// Full program: parse flags, run smoke or the interactive/bench loop. Call from main().
int run_gloom(int argc, char** argv, script_adapter& adapter);

} // namespace gloom
