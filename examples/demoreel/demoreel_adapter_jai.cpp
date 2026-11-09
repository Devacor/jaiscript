// jai_demoreel: the JaiScript adapter for the demoreel shared host. The bytecode vm runs scenes/*.jai;
// --smoke hashes its frame stream, 'r' hot-reloads every scene file.

#include "demoreel_host.hpp"

#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#ifdef JAISCRIPT_ENABLE_DEBUGGER
#include <jaiscript/debug/connector.hpp>
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

#include <jaiscript/testing/unattended.hpp>

namespace {

// Load order matters only in that base classes execute before derived ones.
// main.jai is last and is NEVER hot-reloaded (it owns the persistent globals).
const char* kSceneFiles[] = {
	"util.jai", "music.jai", "reel.jai", "donut.jai", "julia.jai", "lava.jai", "pipes.jai", "fire.jai", "boids.jai",
	"globhunter.jai", "voxelspace.jai", "chrome.jai", "skull.jai", "fluid.jai", "show.jai", "finale.jai", "intro.jai", "main.jai",
};

struct jai_flags {
	bool precompiled = false;            // jaibite save/load boot path
	std::string jaibite_path = "demoreel.jaibite";
	bool debug = false;                  // debug session (VS Code attach)
	bool debug_wait = false;             // also hold boot until a debugger attaches
	int64_t debug_port = 52472;
	std::string coverage;                // the vm's execution-coverage census, written at exit (a build with JAISCRIPT_VM_COVERAGE)
	std::string pix;                     // the cell shape: half, sext or auto (the script's default)
};

// The first diagnostic of a failed load as the reel shows it: "file.jai:line:col: error: ...", then the source line and
// its caret when the listing has them, tabs as spaces so the caret keeps its column.
std::string first_diagnostic(const std::string& what, const std::string& file) {
	std::vector<std::string> lines;
	size_t start = 0;
	while (start <= what.size()) {
		size_t nl = what.find('\n', start);
		std::string line = what.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
		if (!line.empty() && line.back() == '\r') { line.pop_back(); }
		for (char& c : line) { if (c == '\t') { c = ' '; } }
		lines.push_back(line);
		if (nl == std::string::npos) { break; }
		start = nl + 1;
	}
	size_t at = 0;
	while (at < lines.size() && lines[at].find(": error") == std::string::npos) { ++at; }
	if (at == lines.size()) {
		at = 0;
		while (at < lines.size() && lines[at].find_first_not_of(' ') == std::string::npos) { ++at; }
		return at < lines.size() ? file + ": " + lines[at] : file + ": reload failed";
	}
	std::string result = lines[at];
	if (size_t named = result.find(file + ":"); named != std::string::npos) { result = result.substr(named); }
	for (size_t k = at + 1; k <= at + 2 && k < lines.size() && lines[k].rfind("    ", 0) == 0; ++k) {
		result += "\n" + lines[k].substr(4);
	}
	return result;
}

jai::script_value call_script(jai::engine& eng, const char* fn_name,
                              const std::vector<jai::script_value>& args) {
	jai::script_value fn = eng.get_variable(fn_name);
	auto result = fn.as_function()(args);
	if (!result.has_value()) {
		throw std::runtime_error(std::string(fn_name) + ": " + std::string(result.message()));
	}
	return result.value();
}

class jai_session : public demoreel::script_session {
public:
	jai_session(const demoreel::host_services& services, const demoreel::host_options& opt, const jai_flags& flags)
		: opt_(opt), flags_(flags), eng_(jai::engine::make()) {
		if (!flags_.coverage.empty() && !jai::engine::vm_coverage_available()) { throw std::runtime_error("--coverage needs a build with JAISCRIPT_VM_COVERAGE"); }
		eng_->execution_budget(0.0);
		if (opt.workers > 0) { eng_->parallel_thread_count(static_cast<size_t>(opt.workers)); }
		jai::stdlib::register_all(eng_);

		jai::dynamic_binder<demoreel::demo_rng>(*eng_, "Rng")
			.constructor<jai::script_int>()
			.method("next", &demoreel::demo_rng::next)
			.method("roll", &demoreel::demo_rng::roll)
			.method("chance", &demoreel::demo_rng::chance)
			.method("nextf", &demoreel::demo_rng::nextf)
			.build();

		eng_->add_function("host_log", [log = services.log](const std::string& line) { log(line); });
		// script functions over math::, not host bindings: parallel bodies (skull.jai) call them
		eng_->execute("function itrunc(float v) -> int { return math::itrunc(v); }\n"
			"function ifloor(float v) -> int { return math::ifloor(v); }\n");
		eng_->add_global("ESC", eng_->make_value(std::string("\x1b")));
	}

	void load() override {
		namespace fs = std::filesystem;
		dir_ = demoreel::locate_scenes_dir(opt_.scenes_dir, opt_.argv0,
#ifdef JAI_DEMOREEL_SOURCE_SCENES_DIR
			JAI_DEMOREEL_SOURCE_SCENES_DIR,
#else
			"",
#endif
			"demoreel_scenes", "main.jai");
		// the FINALE imports scenes/live/ while it runs
		eng_->add_global("SCENES_DIR", eng_->make_value(fs::canonical(fs::path(dir_)).generic_string()));
#ifdef JAISCRIPT_ENABLE_DEBUGGER
		if (flags_.debug) {
			// execute_file per scene so each node carries its real .jai path and VS Code
			// breakpoints bind; the listener stays up for the whole engine lifetime
			jai::debug::listen(*eng_, static_cast<int>(flags_.debug_port));
			std::fprintf(stderr, "[jai debug] listening on 127.0.0.1:%d - attach from VS Code any time.\n",
				static_cast<int>(flags_.debug_port));
			if (flags_.debug_wait) {
				std::fprintf(stderr, "[jai debug] --debug-wait: open a scenes/*.jai, set breakpoints, then press Enter.\n> ");
				std::fflush(stderr);
				std::string line;
				std::getline(std::cin, line);
			}
			for (const char* f : kSceneFiles) {
				eng_->execute_file(fs::canonical(fs::path(dir_) / f).string());
				loaded_[f] = demoreel::slurp_file(dir_ + "/" + f);
			}
			kind_ = "file";
			return;
		}
#endif
		std::string source;
		for (const char* f : kSceneFiles) {
			std::string body = demoreel::slurp_file(dir_ + "/" + f);
			if (body.empty()) { throw std::runtime_error("missing scene script: " + dir_ + "/" + f); }
			source += body;
			source += "\n";
			loaded_[f] = std::move(body);
		}
		// parse once: the concatenated source goes through engine->jaibite() (or a
		// saved .jaibite in --precompiled mode), then executes to define everything
		jai::jaibite bite;
		if (flags_.precompiled && fs::exists(flags_.jaibite_path)) {
			bite = eng_->jaibite_load(flags_.jaibite_path);
			kind_ = "bite";
		} else {
			bite = eng_->jaibite(source);
			kind_ = "prse";
			if (flags_.precompiled) { bite.save(flags_.jaibite_path); }
		}
		if (std::getenv("JAI_DEMOREEL_NO_JAIBITE")) { eng_->execute(source); return; }
		bite.execute();
	}

	std::string load_kind() override { return kind_; }

	void boot(int64_t w, int64_t h, int64_t seed, const std::string& backend, bool smoke, bool hud) override {
		if (!flags_.pix.empty()) { eng_->execute("demo_pixels(\"" + flags_.pix + "\");"); }
		eng_->execute("demo_boot(" + std::to_string(w) + ", " + std::to_string(h) + ", " +
			std::to_string(seed) + ", \"" + backend + "\", " + (smoke ? "true" : "false") + ", " +
			(hud ? "true" : "false") + ");");
	}

	std::string frame(const demoreel::frame_args& a) override {
		auto* e = eng_.get();
		return call_script(*eng_, "demo_frame", {
			jai::script_value(a.t, e),
			jai::script_value(a.dt, e),
			e->make_value(a.key),
			jai::script_value(a.fps, e),
			jai::script_value(a.ms_sim, e),
			jai::script_value(a.ms_draw, e),
			jai::script_value(a.ms_other, e),
		}).to_string();
	}

	int64_t scene_index() override { return call_script(*eng_, "demo_scene_index", {}).as_int(); }
	double scene_time() override { return call_script(*eng_, "demo_scene_time", {}).as<double>(); }
	int64_t scene_count() override { return call_script(*eng_, "demo_scene_count", {}).as_int(); }
	std::string scene_name() override { return call_script(*eng_, "demo_scene_name", {}).to_string(); }

	void jump(int64_t idx, double at_t) override {
		auto* e = eng_.get();
		call_script(*eng_, "demo_jump", { jai::script_value(idx, e), jai::script_value(at_t, e) });
	}

	// live coding: re-execute every scene file (not main.jai), then rebuild the current scene so build()-time changes
	// show immediately. A file that fails to parse or check changes nothing, so its previous code keeps running; the reel
	// shows which files changed, or the first failure.
	bool reload() override {
		namespace fs = std::filesystem;
		std::string changed;
		std::string failed;
		for (const char* f : kSceneFiles) {
			if (std::string(f) == "main.jai") { continue; }
			std::string body = demoreel::slurp_file(dir_ + "/" + f);
			if (body.empty()) { continue; }
			try {
				eng_->execute_file_source(fs::canonical(fs::path(dir_) / f).string(), body);
			} catch (const std::exception& ex) {
				if (failed.empty()) { failed = first_diagnostic(ex.what(), f); }
				loaded_[f].clear();   // its fix reports as a reload
				continue;
			}
			if (body != loaded_[f]) {
				changed += (changed.empty() ? "" : " ") + std::string(f);
				loaded_[f] = std::move(body);
			}
		}
		call_script(*eng_, "demo_reload", { eng_->make_value(changed), eng_->make_value(failed) });
		return true;
	}

	std::string reload_notice() override { return ""; }

	std::string stack_trace() override { return eng_->format_stack_trace(); }

	~jai_session() override {
		if (!flags_.coverage.empty()) { std::ofstream out(flags_.coverage, std::ios::binary | std::ios::trunc); out << "jai_demoreel vm\n" << eng_->vm_coverage_report(); }
	}

private:
	demoreel::host_options opt_;
	jai_flags flags_;
	std::shared_ptr<jai::engine> eng_;
	std::string dir_;
	std::string kind_ = "prse";
	std::map<std::string, std::string> loaded_;   // each scene file's source as last loaded
};

class jai_adapter : public demoreel::script_adapter {
public:
	std::string program_name() const override { return "jai_demoreel"; }
	std::string language() const override { return "JaiScript"; }
	std::vector<std::string> backends() const override { return {"vm"}; }

	int parse_flag(int argc, char** argv, int i) override {
		std::string a = argv[i];
		auto has_value = [&]() {
			if (i + 1 < argc) { return true; }
			std::fprintf(stderr, "missing value for %s\n", a.c_str());
			std::exit(2);
		};
		if (a == "--precompiled") { flags_.precompiled = true; return 1; }
		if (a == "--jaibite" && has_value()) { flags_.jaibite_path = argv[i + 1]; return 2; }
		if (a == "--coverage" && has_value()) { flags_.coverage = argv[i + 1]; return 2; }
		if (a == "--pix" && has_value()) { flags_.pix = argv[i + 1]; return 2; }
#ifdef JAISCRIPT_ENABLE_DEBUGGER
		if (a == "--debug") { flags_.debug = true; return 1; }
		if (a == "--debug-wait") { flags_.debug = true; flags_.debug_wait = true; return 1; }
		if (a == "--debug-port" && has_value()) { flags_.debug = true; flags_.debug_port = std::stoll(argv[i + 1]); return 2; }
#endif
		return 0;
	}

	std::string usage() const override {
		return
			"  --precompiled    boot from demoreel.jaibite (saved on first run)\n"
			"  --jaibite PATH   .jaibite path for --precompiled\n"
			"  --coverage PATH  write the vm's execution-coverage census to PATH at exit (a build with JAISCRIPT_VM_COVERAGE)\n"
			"  --pix MODE       cells: sext (2x3 sextants), half (1x2 half blocks) or auto (sextants within the pixel budget)\n"
#ifdef JAISCRIPT_ENABLE_DEBUGGER
			"  --debug          listen for a VS Code attach the whole run\n"
			"  --debug-wait     like --debug, but also hold boot until you attach + press Enter\n"
			"  --debug-port N   --debug on port N (default 52472)\n"
#endif
			;
	}

	std::unique_ptr<demoreel::script_session> make_session(const std::string&,
			const demoreel::host_services& services, const demoreel::host_options& opt) override {
		return std::make_unique<jai_session>(services, opt, flags_);
	}

private:
	jai_flags flags_;
};

} // namespace

int main(int argc, char** argv) {
	jai::foundry::route_faults_to_stderr();
	jai_adapter adapter;
	return demoreel::run_demoreel(argc, argv, adapter);
}
