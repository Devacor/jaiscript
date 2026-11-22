#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// Every source file the engine loads (a root, a module, an include, a template, a generic's file) is loaded by one loader:
// its bytes with the stamp of exactly those bytes. No test here sleeps or races a thread: the engine's stamp source stands in
// for the saver and lands a save at a chosen stamp query, before or after the query answers.
class source_loading_tests : public suite {
public:
	source_loading_tests() : suite("Source Loading") {}

	static std::optional<engine::file_stamp> disk_stamp(const std::string& a_path) {
		std::error_code time_error, size_error;
		const auto mtime = std::filesystem::last_write_time(a_path, time_error);
		const auto size = std::filesystem::file_size(a_path, size_error);
		if (time_error || size_error) { return std::nullopt; }
		return engine::file_stamp{uint64_t(mtime.time_since_epoch().count()), uint64_t(size)};
	}

	// a folder of its own, whose files each get a later stamp than any written before
	struct folder {
		std::filesystem::path dir;
		std::filesystem::file_time_type base = std::filesystem::file_time_type::clock::now();
		int tick = 0;
		std::map<std::string, engine::file_stamp> written;   // file name and text -> the stamp that text was saved under

		explicit folder(const std::string& a_name) : dir(jai::foundry::test_temp_directory() / ("source_loading_" + a_name)) {
			std::error_code ec;
			std::filesystem::remove_all(dir, ec);
			std::filesystem::create_directories(dir);
		}
		~folder() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
		std::string path(const std::string& a_file) const { return (dir / a_file).string(); }
		void write(const std::string& a_file, const std::string& a_text) {
			{
				std::ofstream out(dir / a_file, std::ios::binary | std::ios::trunc);
				out << a_text;
			}
			std::filesystem::last_write_time(dir / a_file, base + std::chrono::seconds(2 * ++tick));
			written[a_file + '\n' + a_text] = *disk_stamp((dir / a_file).string());
		}
		std::string bytes(const std::string& a_file) const {
			std::ifstream in(dir / a_file, std::ios::binary);
			std::ostringstream text;
			text << in.rdbuf();
			return text.str();
		}
	};

	// what the engine's observer saw: every whole read, in order
	struct observed {
		struct load {
			std::string file;
			std::optional<engine::file_stamp> stamp;
			std::string bytes;
		};
		std::vector<load> loads;

		const load* last(const std::string& a_file) const {
			for (auto it = loads.rbegin(); it != loads.rend(); ++it) { if (it->file == a_file) { return &*it; } }
			return nullptr;
		}
		size_t count(const std::string& a_file) const {
			size_t result = 0;
			for (const auto& l : loads) { result += l.file == a_file; }
			return result;
		}
		bool saw(const std::string& a_file, const std::string& a_bytes) const {
			for (const auto& l : loads) { if (l.file == a_file && l.bytes == a_bytes) { return true; } }
			return false;
		}
		engine::source_file_observing observer() {
			return [this](const std::string& a_path, const std::optional<engine::file_stamp>& a_stamp, std::string_view a_bytes) {
				loads.push_back({std::filesystem::path(a_path).filename().string(), a_stamp, std::string(a_bytes)});
			};
		}
	};
	// every read the observer saw carries the stamp its bytes were saved under
	static bool stamped_as_saved(const observed& a_seen, const folder& a_folder, const std::string& a_file) {
		for (const auto& l : a_seen.loads) {
			if (l.file != a_file) { continue; }
			const auto saved = a_folder.written.find(a_file + '\n' + l.bytes);
			if (saved == a_folder.written.end() || !l.stamp || l.stamp->mtime != saved->second.mtime || l.stamp->size != saved->second.size) { return false; }
		}
		return true;
	}

	// the saver: a save lands at the at-th stamp query of `file` counted from arming, before that query answers or after
	// it, or after every query
	struct landing {
		std::string file;
		int queries = 0;
		int at = 0;
		bool before = true;
		bool every = false;
		bool fired = false;
		bool saving = false;
		std::function<void()> save;

		void arm(int a_at, bool a_before, std::function<void()> a_save) { queries = 0; at = a_at; before = a_before; every = false; fired = false; save = std::move(a_save); }
		void arm_every(std::function<void()> a_save) { queries = 0; at = 0; every = true; fired = false; save = std::move(a_save); }
		void disarm() { at = 0; every = false; save = nullptr; }
		void land() {
			if (saving || !save) { return; }
			saving = true;
			fired = true;
			save();
			saving = false;
		}
	};
	static engine::file_stamp_reading stamps(landing& a_landing) {
		return [&a_landing](const std::string& a_path) -> std::optional<engine::file_stamp> {
			bool now = false;
			if (!a_landing.file.empty() && std::filesystem::path(a_path).filename() == a_landing.file) {
				++a_landing.queries;
				now = a_landing.queries == a_landing.at;
			}
			if (now && a_landing.before) { a_landing.land(); }
			const std::optional<engine::file_stamp> result = disk_stamp(a_path);
			if ((now && !a_landing.before) || (a_landing.every && !a_landing.file.empty() && std::filesystem::path(a_path).filename() == a_landing.file)) { a_landing.land(); }
			return result;
		};
	}

	// One way a program reads a file: the watched file, its text for a version (good, and broken: rejected at load), the
	// files around it, and the version the program runs now (a load error throws)
	struct site {
		std::string name;
		std::string file;
		std::function<std::string(int)> good;
		std::function<std::string(int)> broken;
		std::function<void(folder&)> setup;
		std::function<std::string(engine&, folder&)> run;
	};

	static std::string pad(int a_version) { return std::string(static_cast<size_t>(a_version), ' '); }   // each version its own size
	static std::string trimmed(std::string a_text) {
		while (!a_text.empty() && a_text.back() == ' ') { a_text.pop_back(); }
		return a_text;
	}

	static std::vector<site> sites() {
		const auto number = [](int v) { return std::to_string(v); };
		const auto run_root = [](engine& e, folder& f) { return trimmed(e.execute_file(f.path("root.jai")).to_string()); };
		const auto call_after_import = [](const char* a_module, const char* a_call) {
			return [a_module, a_call](engine& e, folder&) {
				e.execute(std::string("import(\"") + a_module + "\");");
				return trimmed(e.execute(a_call).to_string());
			};
		};
		const auto text_page = [number](int v) { return number(v) + pad(v); };
		const auto broken_page = [](int v) { return "${1 +}" + pad(v); };
		std::vector<site> result;
		result.push_back({"execute_file", "root.jai",
			[number](int v) { return "return " + number(v) + ";" + pad(v); },
			[](int v) { return "return (;" + pad(v); },
			[](folder&) {}, run_root});
		result.push_back({"import", "mod.jai",
			[number](int v) { return "int sl_value() { return " + number(v) + "; }\n" + pad(v); },
			[](int v) { return v % 2 ? "int sl_value() { return (; }\n" + pad(v) : "int sl_value() { int q = \"s\"; return 0; }\n" + pad(v); },
			[](folder&) {}, call_after_import("mod.jai", "sl_value();")});
		result.push_back({"run_time_include", "inc.jai",
			[number](int v) { return "return " + number(v) + ";" + pad(v); },
			[](int v) { return "return (;" + pad(v); },
			[](folder& f) { f.write("root.jai", "string p = \"inc.jai\";\nreturn include(p);\n"); }, run_root});
		result.push_back({"literal_include", "lit.jai",
			[number](int v) { return "int sl_lit() { return " + number(v) + "; }\n" + pad(v); },
			[](int v) { return "int sl_lit() { return (; }\n" + pad(v); },
			[](folder& f) { f.write("root.jai", "include \"lit.jai\";\nreturn sl_lit();\n"); }, run_root});
		result.push_back({"spliced_include", "dep.jai",
			[number](int v) { return "return " + number(v) + ";" + pad(v); },
			[](int v) { return "return (;" + pad(v); },
			[](folder& f) { f.write("root.jai", "int probe() { include \"dep.jai\"; return 0; }\nreturn probe();\n"); }, run_root});
		result.push_back({"literal_template", "page.spider", text_page, broken_page,
			[](folder& f) { f.write("root.jai", "return template(\"page.spider\").to_string();\n"); }, run_root});
		result.push_back({"dynamic_template", "page.spider", text_page, broken_page,
			[](folder& f) { f.write("root.jai", "string p = \"page.spider\";\nreturn template(p).to_string();\n"); }, run_root});
		result.push_back({"untrusted_template", "card.spider", text_page, broken_page,
			[](folder& f) { f.write("root.jai", "map<string, var> m = {\"k\": 1};\nstring p = \"card.spider\";\nreturn untrusted_template(p, m).to_string();\n"); }, run_root});
		result.push_back({"literal_untrusted_template", "card.spider", text_page, broken_page,
			[](folder& f) { f.write("root.jai", "map<string, var> m = {\"k\": 1};\nreturn untrusted_template(\"card.spider\", m).to_string();\n"); }, run_root});
		result.push_back({"module_splicing", "dep.jai",
			[number](int v) { return "return " + number(v) + ";" + pad(v); },
			[](int v) { return "return (;" + pad(v); },
			[](folder& f) { f.write("holder.jai", "int sl_spliced() { include \"dep.jai\"; return 0; }\n"); }, call_after_import("holder.jai", "sl_spliced();")});
		result.push_back({"module_template", "page.spider", text_page, broken_page,
			[](folder& f) { f.write("holder.jai", "string sl_page() { return template(\"page.spider\").to_string(); }\n"); }, call_after_import("holder.jai", "sl_page();")});
		result.push_back({"generic_file", "box.jai",
			[number](int v) { return "class SlBox<T> { T held; int tag() { return " + number(v) + "; } }\n" + pad(v); },
			[](int v) { return "class SlBox<T> { T held; int tag() { return (; } }\n" + pad(v); },
			[](folder& f) { f.write("holder.jai", "import \"box.jai\";\nint sl_tag() { SlBox<int> b; return b.tag(); }\n"); }, call_after_import("holder.jai", "sl_tag();")});
		// the generic's file names bare a namespace class a later import declares, so one load parses it twice
		result.push_back({"generic_file_parsed_again", "crate.jai",
			[number](int v) { return "namespace slp { class SlCrate<T> { T held; int tag() { return " + number(v) + "; } } int weigh() { SlItem i; return i.weight; } }\n" + pad(v); },
			[](int v) { return "namespace slp { class SlCrate<T> { T held; int tag() { return (; } } int weigh() { SlItem i; return i.weight; } }\n" + pad(v); },
			[](folder& f) {
				f.write("item.jai", "namespace slp { class SlItem { int weight = 0; } }\n");
				f.write("holder.jai", "import \"crate.jai\";\nimport \"item.jai\";\nint sl_crate() { slp::SlCrate<int> c; return c.tag() + slp::weigh(); }\n");
			}, call_after_import("holder.jai", "sl_crate();")});
		return result;
	}

	static std::shared_ptr<engine> engine_for(folder& a_folder, landing& a_landing, observed* a_seen = nullptr) {
		auto eng = make_engine();
		stdlib::register_all(*eng);
		stdlib::register_web(*eng);
		eng->jaibite_cache(false);
		eng->add_include_path(a_folder.dir.string());
		eng->set_untrusted_template_dirs({a_folder.dir.string()});
		eng->file_stamp_source(stamps(a_landing));
		if (a_seen) { eng->source_file_observer(a_seen->observer()); }
		return eng;
	}

	// what a run answers: the version it ran, or "error: ..." for a load that was refused
	static std::string attempt(const site& a_site, engine& a_engine, folder& a_folder) {
		try {
			return a_site.run(a_engine, a_folder);
		} catch (const std::exception& e) {
			return std::string("error: ") + e.what();
		}
	}

	static constexpr int k_landings = 12;
	static constexpr const char* k_unsettled = "kept changing while it was read";

	void forge_tests() override {
		for (const site& s : sites()) {
			test(s.name + "_runs_the_saved_text_wherever_a_save_lands", [s, this]() {
				// version 2 is on disk when the reload starts and version 3 lands at one stamp query of that reload: whichever
				// the reload ran, the next one runs version 3
				for (int at = 1; at <= k_landings; ++at) {
					for (const bool before : {true, false}) {
						const std::string where = " (" + s.name + ", save " + (before ? "before" : "after") + " query " + std::to_string(at) + ")";
						folder f(s.name);
						landing save;
						save.file = s.file;
						observed seen;
						auto eng = engine_for(f, save, &seen);
						s.setup(f);
						f.write(s.file, s.good(1));
						check_eq(std::string("1"), attempt(s, *eng, f), "the first load" + where);
						f.write(s.file, s.good(2));
						save.arm(at, before, [&] { f.write(s.file, s.good(3)); });
						const std::string reloaded = attempt(s, *eng, f);
						save.disarm();
						check_true(reloaded == "2" || reloaded == "3", "the reload runs a version that was saved whole" + where + ": " + reloaded);
						if (reloaded == "2" || reloaded == "3") {   // a later version may be loaded already, for the next pass to run
							check_true(seen.saw(s.file, s.good(reloaded == "2" ? 2 : 3)), "the observer saw the text that runs" + where);
						}
						const std::string settled = attempt(s, *eng, f);
						check_eq(std::string(save.fired ? "3" : "2"), settled, "the next reload runs what is on disk" + where);
						check_eq(settled, attempt(s, *eng, f), "and keeps running it" + where);
						check_true(seen.last(s.file) && seen.last(s.file)->bytes == f.bytes(s.file), "the observer's last bytes are the file's" + where);
						check_true(stamped_as_saved(seen, f, s.file), "every read the observer saw carries the stamp its bytes were saved under" + where);
					}
				}
			});

			test(s.name + "_never_holds_a_failure_against_text_it_did_not_check", [s, this]() {
				// a broken version 2 is on disk when the reload starts and a good version 3 lands at one stamp query of it: the
				// failure of version 2 is never held against version 3's stamp
				for (int at = 1; at <= k_landings; ++at) {
					for (const bool before : {true, false}) {
						const std::string where = " (" + s.name + ", save " + (before ? "before" : "after") + " query " + std::to_string(at) + ")";
						folder f(s.name + "_hold");
						landing save;
						save.file = s.file;
						auto eng = engine_for(f, save);
						s.setup(f);
						f.write(s.file, s.good(1));
						check_eq(std::string("1"), attempt(s, *eng, f), "the first load" + where);
						f.write(s.file, s.broken(2));
						save.arm(at, before, [&] { f.write(s.file, s.good(3)); });
						const std::string reloaded = attempt(s, *eng, f);
						save.disarm();
						check_true(reloaded == "1" || reloaded == "3" || reloaded.rfind("error: ", 0) == 0, "the reload is rejected, or runs the fix" + where + ": " + reloaded);
						if (!save.fired) { f.write(s.file, s.good(3)); }
						(void)attempt(s, *eng, f);
						check_eq(std::string("3"), attempt(s, *eng, f), "the fix that was saved runs" + where);
					}
				}
			});

			test(s.name + "_never_keeps_a_broken_save_as_the_text_it_runs", [s, this]() {
				// version 2 is on disk when the reload starts and a broken version 3 lands at one stamp query of it: a reload
				// that answers ran a good version, and the good version 4 saved afterwards runs
				for (int at = 1; at <= k_landings; ++at) {
					for (const bool before : {true, false}) {
						const std::string where = " (" + s.name + ", save " + (before ? "before" : "after") + " query " + std::to_string(at) + ")";
						folder f(s.name + "_broken");
						landing save;
						save.file = s.file;
						auto eng = engine_for(f, save);
						s.setup(f);
						f.write(s.file, s.good(1));
						check_eq(std::string("1"), attempt(s, *eng, f), "the first load" + where);
						f.write(s.file, s.good(2));
						save.arm(at, before, [&] { f.write(s.file, s.broken(3)); });
						for (int pass = 0; pass < 3; ++pass) {
							const std::string reloaded = attempt(s, *eng, f);
							check_true(reloaded == "1" || reloaded == "2" || reloaded.rfind("error: ", 0) == 0, "a reload answers with a good version or its error" + where + ": " + reloaded);
						}
						save.disarm();
						f.write(s.file, s.good(4));
						(void)attempt(s, *eng, f);
						check_eq(std::string("4"), attempt(s, *eng, f), "the next good save runs" + where);
					}
				}
			});

			test(s.name + "_refuses_a_file_that_never_settles", [s, this]() {
				// a save lands after every stamp query: no read is whole, so the load is an error and nothing is remembered
				const std::string where = " (" + s.name + ")";
				{
					folder f(s.name + "_unsettled_first");
					landing save;
					save.file = s.file;
					auto eng = engine_for(f, save);
					s.setup(f);
					int version = 1;
					f.write(s.file, s.good(version));
					save.arm_every([&] { f.write(s.file, s.good(++version)); });
					const std::string first = attempt(s, *eng, f);
					save.disarm();
					check_true(save.fired, "a save landed" + where);
					check_true(first.rfind("error: ", 0) == 0 && first.find(k_unsettled) != std::string::npos && first.find(s.file) != std::string::npos,
						"a first load of a file that never settles is a load error naming it" + where + ": " + first);
					check_eq(std::to_string(version), attempt(s, *eng, f), "nothing was remembered: the settled file loads" + where);
				}
				{
					folder f(s.name + "_unsettled_reload");
					landing save;
					save.file = s.file;
					auto eng = engine_for(f, save);
					s.setup(f);
					int version = 1;
					f.write(s.file, s.good(version));
					check_eq(std::string("1"), attempt(s, *eng, f), "the first load" + where);
					f.write(s.file, s.good(++version));
					save.arm_every([&] { f.write(s.file, s.good(++version)); });
					const std::string reloaded = attempt(s, *eng, f);
					save.disarm();
					check_true(save.fired, "a save landed during the reload" + where);
					check_true(reloaded == "1" || (reloaded.rfind("error: ", 0) == 0 && reloaded.find(k_unsettled) != std::string::npos),
						"a reload of a file that never settles keeps the code it had, or says why" + where + ": " + reloaded);
					check_eq(std::to_string(version), attempt(s, *eng, f), "the settled file is picked up by the next reload" + where);
				}
			});
		}

		test("a_host_that_keeps_the_observed_bytes_holds_the_program_that_ran", [this]() {
			// A fresh engine builds a program from several files while a save lands in one of them (a good version, or a
			// broken one). When the build answers, the bytes the observer last saw for each file, written out again, build
			// the same answer: the host never needs to read a file itself to know what the engine ran.
			const std::string root = "import \"mod.jai\";\ninclude \"lit.jai\";\nint probe() { include \"dep.jai\"; return 0; }\n"
				"return to_string(sl_value()) + \"|\" + to_string(sl_lit()) + \"|\" + to_string(probe()) + \"|\" + template(\"page.spider\").to_string();\n";
			struct part {
				const char* file;
				std::function<std::string(int)> good;
				std::string broken;
			};
			const std::vector<part> parts = {
				{"mod.jai", [](int v) { return "int sl_value() { return " + std::to_string(v) + "; }\n" + pad(v); }, "int sl_value() { return (; }\n"},
				{"lit.jai", [](int v) { return "int sl_lit() { return " + std::to_string(v) + "; }\n" + pad(v); }, "int sl_lit() { return (; }\n"},
				{"dep.jai", [](int v) { return "return " + std::to_string(v) + ";" + pad(v); }, "return (;"},
				{"page.spider", [](int v) { return std::to_string(v) + pad(v); }, "${1 +}"},
			};
			const auto build = [](engine& e, folder& f) -> std::string {
				try {
					std::string answer = e.execute_file(f.path("root.jai")).to_string();
					answer.erase(std::remove(answer.begin(), answer.end(), ' '), answer.end());
					return answer;
				} catch (const std::exception& ex) {
					return std::string("error: ") + ex.what();
				}
			};
			for (const part& moving : parts) {
				for (const bool breaks : {false, true}) {
					for (int at = 1; at <= 8; ++at) {
						for (const bool before : {true, false}) {
							const std::string where = std::string(" (") + moving.file + (breaks ? ", a broken save " : ", a save ") + (before ? "before" : "after") + " query " + std::to_string(at) + ")";
							folder f("kept");
							landing save;
							save.file = moving.file;
							observed seen;
							auto eng = engine_for(f, save, &seen);
							f.write("root.jai", root);
							for (const part& p : parts) { f.write(p.file, p.good(1)); }
							save.arm(at, before, [&] { f.write(moving.file, breaks ? moving.broken : moving.good(2)); });
							const std::string answer = build(*eng, f);
							save.disarm();
							if (answer.rfind("error: ", 0) == 0) {
								check_true(breaks && save.fired, "only the broken save fails the build" + where + ": " + answer);
								continue;
							}
							folder kept("kept_copy");
							for (const char* file : {"root.jai", "mod.jai", "lit.jai", "dep.jai", "page.spider"}) {
								const observed::load* last = seen.last(file);
								check_true(last != nullptr, std::string("the observer saw ") + file + where);
								if (last) { kept.write(file, last->bytes); }
							}
							landing still;
							auto again = engine_for(kept, still);
							check_eq(answer, build(*again, kept), "the kept bytes build the program that ran" + where);
						}
					}
				}
			}
		});

		test("a_rejected_reload_tries_again_once_the_code_it_would_break_is_loaded_again", [this]() {
			// a module's edit drops a field the root file reads, and the root's fix is saved a moment later: at one stamp query
			// of the frame that rejects the module. The rejection was judged against the root as it was loaded, so the root
			// loaded again lets the module try again, wherever the save landed and whoever read the root's file
			const std::string old_module = "class SlShape { int a = 1; }\n";
			const std::string new_module = "class SlShape { int b = 7; }\n ";
			const std::string old_root = "import(\"shape.jai\");\nSlShape held;\nreturn held.a;\n";
			const std::string new_root = "import(\"shape.jai\");\nSlShape held;\nreturn held.b;\n ";
			for (const bool host_reads : {false, true}) {
				for (int at = 1; at <= k_landings; ++at) {
					for (const bool before : {true, false}) {
						const std::string where = std::string(host_reads ? " (the host reads the root" : " (execute_file reads the root") + ", save " + (before ? "before" : "after") + " query " + std::to_string(at) + ")";
						folder f("dependent");
						landing save;
						save.file = "root.jai";
						auto eng = engine_for(f, save);
						// a frame of the host's loop: the reload point, then the root file run again
						const auto frame = [&]() -> std::string {
							try { eng->execute("import(\"shape.jai\");"); } catch (const std::exception&) {}
							try {
								if (!host_reads) { return eng->execute_file(f.path("root.jai")).to_string(); }
								return eng->execute_file_source(f.path("root.jai"), f.bytes("root.jai")).to_string();
							} catch (const std::exception& e) {
								return std::string("error: ") + e.what();
							}
						};
						f.write("shape.jai", old_module);
						f.write("root.jai", old_root);
						check_eq(std::string("1"), frame(), "the first load" + where);
						f.write("shape.jai", new_module);
						bool saved = false;
						const auto fix = [&] { if (!saved) { saved = true; f.write("root.jai", new_root); } };
						save.arm(at, before, fix);
						const std::string rejected = frame();
						save.disarm();
						check_true(rejected == "1" || rejected == "7" || rejected.rfind("error: ", 0) == 0, "the frame the edits land in" + where + ": " + rejected);
						fix();
						(void)frame();
						(void)frame();
						check_eq(std::string("7"), frame(), "both edits apply once the root is loaded again" + where);
					}
				}
			}
		});

		test("a_load_that_spans_a_save_shows_the_observer_both_versions", [this]() {
			// two functions splice one file and a save lands between the two splices: the program holds two versions of the
			// file until its next check, and the observer saw the file twice, under two stamps, so a host can tell
			bool spanned = false;
			for (int at = 1; at <= 8; ++at) {
				for (const bool before : {true, false}) {
					const std::string where = std::string(" (save ") + (before ? "before" : "after") + " query " + std::to_string(at) + ")";
					folder f("spanned");
					landing save;
					save.file = "dep.jai";
					observed seen;
					auto eng = engine_for(f, save, &seen);
					f.write("root.jai", "int first() { include \"dep.jai\"; return 0; }\nint second() { include \"dep.jai\"; return 0; }\nreturn first() * 100 + second();\n");
					f.write("dep.jai", "return 1;");
					save.arm(at, before, [&] { f.write("dep.jai", "return 2; "); });
					const script_int loaded = eng->execute_file(f.path("root.jai")).as_int();
					save.disarm();
					check_true(loaded == 101 || loaded == 102 || loaded == 202, "each splice is a whole version" + where + ": " + std::to_string(loaded));
					if (loaded == 102) {
						spanned = true;
						check_eq(size_t(2), seen.count("dep.jai"), "the observer saw both reads" + where);
						check_true(seen.loads.size() >= 2 && seen.saw("dep.jai", "return 1;") && seen.saw("dep.jai", "return 2; "), "one of each version" + where);
					}
					check_true(stamped_as_saved(seen, f, "dep.jai"), "each under the stamp its bytes were saved with" + where);
					check_eq(script_int(save.fired ? 202 : 101), eng->execute_file(f.path("root.jai")).as_int(), "the next check runs one version" + where);
				}
			}
			check_true(spanned, "some landing fell between the two splices");
		});

		test("the_observer_sees_each_whole_read_once_with_the_stamp_of_its_bytes", [this]() {
			folder f("observer");
			landing none;
			observed seen;
			auto eng = engine_for(f, none, &seen);
			f.write("root.jai", "import \"mod.jai\";\nint probe() { include \"dep.jai\"; return 0; }\nreturn sl_value() + probe() + template(\"page.spider\").to_string().to_int();\n");
			f.write("mod.jai", "int sl_value() { return 1; }\n");
			f.write("dep.jai", "return 20;");
			f.write("page.spider", "300");
			check_eq(script_int(321), eng->execute_file(f.path("root.jai")).as_int());
			for (const char* file : {"root.jai", "mod.jai", "dep.jai", "page.spider"}) {
				check_eq(size_t(1), seen.count(file), std::string("one load reads each file once: ") + file);
				const observed::load* last = seen.last(file);
				check_true(last && last->bytes == f.bytes(file), std::string("the bytes are the file's: ") + file);
				check_true(last && stamped_as_saved(seen, f, file), std::string("under the stamp they were saved with: ") + file);
			}
			check_eq(script_int(321), eng->execute_file(f.path("root.jai")).as_int());
			const size_t settled = seen.loads.size();
			check_eq(script_int(321), eng->execute_file(f.path("root.jai")).as_int());
			check_eq(settled, seen.loads.size(), "an unchanged program is not read again");
			f.write("dep.jai", "return 40;");
			check_eq(script_int(341), eng->execute_file(f.path("root.jai")).as_int());
			check_true(seen.last("dep.jai") && seen.last("dep.jai")->bytes == "return 40;", "a changed file is seen again, with its new bytes");
			eng->source_file_observer(nullptr);
			const size_t removed = seen.loads.size();
			f.write("dep.jai", "return 60;");
			check_eq(script_int(361), eng->execute_file(f.path("root.jai")).as_int());
			check_eq(removed, seen.loads.size(), "an empty observer removes it");
		});

		// A unit that names bare a namespace class another file declares parses again once its program is pooled. That parse
		// is of the text the load already holds, and a file it splices is the one the load read: nothing is read twice.
		test("a_unit_parsed_again_once_its_program_is_pooled_reads_no_file_again", [this]() {
			folder f("pooled");
			landing none;
			observed seen;
			auto eng = engine_for(f, none, &seen);
			const std::vector<std::string> files = {"root.jai", "naming.jai", "declaring.jai", "part.jai"};
			const auto reads_since = [&](const std::map<std::string, size_t>& a_before, const std::string& a_file) {
				const auto before = a_before.find(a_file);
				return seen.count(a_file) - (before == a_before.end() ? 0 : before->second);
			};
			const auto counted = [&] {
				std::map<std::string, size_t> result;
				for (const auto& file : files) { result[file] = seen.count(file); }
				return result;
			};
			f.write("root.jai", "import \"naming.jai\";\nimport \"declaring.jai\";\nfunction reload() -> bool { return import(\"declaring.jai\", \"naming.jai\"); }\nreturn shop::total();\n");
			f.write("naming.jai", "namespace shop { int total() { include \"part.jai\"; Item i; return i.price + part; } }\n");
			f.write("declaring.jai", "namespace shop { class Item { int price = 3; } }\n");
			f.write("part.jai", "int part = 4;");
			check_eq(script_int(7), eng->execute_file(f.path("root.jai")).as_int(), "the class the later import declares is the bare name's");
			for (const auto& file : files) { check_eq(size_t(1), seen.count(file), "the first load reads each file once: " + file); }
			check_eq(uint64_t(4), detail::engine_access::source_read_count(*eng), "and opens none it does not hand the observer");
			check_true(stamped_as_saved(seen, f, "naming.jai") && stamped_as_saved(seen, f, "part.jai"), "each under the stamp its bytes were saved with");

			const auto before = counted();
			f.write("declaring.jai", "namespace shop { class Item { int price = 3; } class Box { int w = 6; } }\n");
			f.write("naming.jai", "namespace shop { int total() { include \"part.jai\"; Item i; Box b; return i.price + part + b.w; } }\n");
			check_true(eng->execute("reload();").as_bool(), "the group reloads");
			check_eq(script_int(13), eng->execute("shop::total();").as_int(), "the class the reload adds is the bare name's");
			check_eq(size_t(1), reads_since(before, "naming.jai"), "a reload reads the unit it parses twice once");
			check_eq(size_t(1), reads_since(before, "declaring.jai"), "and the unit that declares the class once");
			check_eq(size_t(1), reads_since(before, "part.jai"), "and the file both parses splice once");
			check_eq(size_t(0), reads_since(before, "root.jai"), "and no file that stood still");
			check_eq(uint64_t(seen.loads.size()), detail::engine_access::source_read_count(*eng), "every read is one the observer saw");

			// a fresh engine with the files as they stand: the root's gather parses naming.jai before declaring.jai is pooled
			observed again;
			auto second = engine_for(f, none, &again);
			check_eq(script_int(13), second->execute_file(f.path("root.jai")).as_int());
			for (const auto& file : files) { check_eq(size_t(1), again.count(file), "a fresh engine reads each file once: " + file); }
			check_eq(uint64_t(4), detail::engine_access::source_read_count(*second));
		});

		test("a_file_the_stamp_source_cannot_stamp_loads_and_is_never_remembered", [this]() {
			folder f("unstamped");
			observed seen;
			auto eng = make_engine();
			eng->jaibite_cache(false);
			eng->add_include_path(f.dir.string());
			eng->file_stamp_source([](const std::string& a_path) -> std::optional<engine::file_stamp> {
				return std::filesystem::path(a_path).filename() == "mod.jai" ? std::nullopt : disk_stamp(a_path);
			});
			eng->source_file_observer(seen.observer());
			f.write("mod.jai", "int sl_value() { return 1; }\n");
			eng->execute("import(\"mod.jai\");");
			check_eq(script_int(1), eng->execute("sl_value();").as_int());
			check_true(seen.last("mod.jai") && !seen.last("mod.jai")->stamp, "the observer sees it without a stamp");
			{ std::ofstream out(f.dir / "mod.jai", std::ios::binary | std::ios::trunc); out << "int sl_value() { return 2; }\n"; }
			eng->execute("import(\"mod.jai\");");
			check_eq(script_int(2), eng->execute("sl_value();").as_int(), "every reach loads it again: no stamp says it stood still");
		});

		test("a_script_reads_as_the_platforms_text_mode_reads_it_and_a_template_as_its_bytes", [this]() {
			// what a program reads did not change with the loader: a script file (execute_file, import, a run-time include) reads
			// as the platform's text mode reads it, a template as its bytes. The oracle for a script is the same text read
			// through a text-mode stream and handed to execute_file_source.
			const auto outcome = [](const std::function<std::string()>& a_run) {
				try { return a_run(); } catch (const std::exception& e) { return std::string("error: ") + e.what(); }
			};
			std::vector<std::string> payloads = {"a\r\nb", "a\rb", "a\r\r\nb", "a\r", "\r\n", "a\nb\r\n", "ab\x1A" "cd", "a\r\n\x1A" "b"};
			for (size_t length = 4080; length <= 4100; length += 2) { payloads.push_back(std::string(length, 'x') + "\r\ny\rz\r\r\n"); }
			for (size_t length = 8180; length <= 8194; length += 2) { payloads.push_back(std::string(length, 'x') + "\r\n"); }
			int n = 0;
			for (const std::string& payload : payloads) {
				const std::string which = " (payload " + std::to_string(n++) + ", " + std::to_string(payload.size()) + " bytes)";
				folder f("modes");
				f.write("text.jai", "return `" + payload + "`;");
				f.write("mod.jai", "string sl_text() { return `" + payload + "`; }\n");
				f.write("card.spider", payload);
				f.write("includer.jai", "string p = \"text.jai\";\nreturn include(p);\n");
				const auto text_mode = [&](const char* a_file) {
					std::ifstream in(f.dir / a_file);
					std::ostringstream text;
					text << in.rdbuf();
					return text.str();
				};
				const auto fresh = [&] {
					auto eng = make_engine();
					stdlib::register_all(*eng);
					stdlib::register_web(*eng);
					eng->jaibite_cache(false);
					eng->add_include_path(f.dir.string());
					eng->set_untrusted_template_dirs({f.dir.string()});
					eng->add_global("payload", script_value(payload, eng.get()));
					return eng;
				};
				const std::string expected = outcome([&] { return fresh()->execute_file_source(f.path("text.jai"), text_mode("text.jai")).to_string(); });
				check_eq(expected, outcome([&] { return fresh()->execute_file(f.path("text.jai")).to_string(); }), "execute_file reads the script as text mode does" + which);
				check_eq(expected, outcome([&] { return fresh()->execute_file(f.path("includer.jai")).to_string(); }), "a run-time include too" + which);
				const std::string expected_module = outcome([&] { auto eng = fresh(); eng->execute_file_source(f.path("mod.jai"), text_mode("mod.jai")); return eng->execute("sl_text();").to_string(); });
				check_eq(expected_module, outcome([&] { auto eng = fresh(); eng->execute("import(\"mod.jai\");"); return eng->execute("sl_text();").to_string(); }), "an import too" + which);
				const std::string as_bytes = outcome([&] { return fresh()->execute("map<string, var> m = {}; untrusted_template_text(payload, m).to_string();").to_string(); });
				check_eq(as_bytes, outcome([&] { return fresh()->execute("map<string, var> m = {}; string p = \"card.spider\"; untrusted_template(p, m).to_string();").to_string(); }),
					"a template file renders as its bytes do" + which);
			}
		});
	}
};

}

using source_loading_tests = jai::foundry::tests::source_loading_tests;
FOUNDRY_REGISTER(source_loading_tests)
