#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/core/load_report.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// engine::load_report: which files a load reached, in what order, what the cache did with each and which phases ran. Structure
// and counts only: no test reads a duration's value, except that the durations add up.
class load_report_tests : public suite {
public:
	load_report_tests() : suite("Load Report") {}

	struct folder {
		std::filesystem::path dir;
		explicit folder(const std::string& a_name) : dir(jai::foundry::test_temp_directory() / ("load_report_" + a_name)) {
			std::error_code ec;
			std::filesystem::remove_all(dir, ec);
			std::filesystem::create_directories(dir);
		}
		~folder() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
		std::string path(const std::string& a_name) const { return (dir / a_name).string(); }
		std::string write(const std::string& a_name, const std::string& a_text) const {
			std::ofstream out(dir / a_name, std::ios::binary | std::ios::trunc);
			out << a_text;
			return path(a_name);
		}
	};

	static std::shared_ptr<engine> reporting_engine(const folder& a_folder, backend_type a_backend) {
		auto e = engine::make();
		e->set_backend(a_backend);
		e->import_recheck_interval(0);
		e->set_output_stream(std::make_shared<std::ostringstream>());
		e->add_include_path(a_folder.dir.string());
		e->load_report(true);
		return e;
	}
	static std::shared_ptr<engine> reporting_engine(const folder& a_folder) {
		auto e = make_engine();
		const backend_type backend = e->get_backend_type();
		return reporting_engine(a_folder, backend);
	}

	static std::string file_name(const load_report_row& a_row) { return std::filesystem::path(a_row.file).filename().string(); }
	static std::vector<std::string> file_names(const load_report_summary& a_report) {
		std::vector<std::string> names;
		for (const auto& row : a_report.rows) { names.push_back(file_name(row)); }
		return names;
	}
	static const load_report_row* row_named(const load_report_summary& a_report, const std::string& a_name) {
		for (const auto& row : a_report.rows) { if (file_name(row) == a_name) { return &row; } }
		return nullptr;
	}
	const load_report_row& row_of(const load_report_summary& a_report, const std::string& a_name) {
		const load_report_row* row = row_named(a_report, a_name);
		check_not_null(row, "the report has a row for " + a_name);
		static const load_report_row none;
		return row ? *row : none;
	}
	static uint64_t count_of(const load_report_row& a_row, load_phase a_phase) { return a_row.phase(a_phase).count; }

	void forge_tests() override {
		test("off_by_default_collects_nothing", [this]() {
			folder f("off");
			const std::string root = f.write("root.jai", "int a = 1;\n");
			auto e = make_engine();
			check_false(e->load_report());
			e->add_include_path(f.dir.string());
			e->execute_file(root);
			const load_report_summary report = e->load_report_snapshot();
			check_true(report.rows.empty());
			check_eq(uint64_t(0), report.wall_nanoseconds);
		});

		test("turning_it_off_drops_the_report", [this]() {
			folder f("drop");
			const std::string root = f.write("root.jai", "int a = 1;\n");
			auto e = reporting_engine(f);
			check_true(e->load_report());
			e->execute_file(root);
			check_eq(size_t(1), e->load_report_snapshot().rows.size());
			e->load_report(false);
			check_false(e->load_report());
			check_true(e->load_report_snapshot().rows.empty());
		});

		test("rows_stand_in_load_order_with_the_phases_that_ran", [this]() {
			folder f("order");
			f.write("leaf.jai", "int leaf_value = 3;\n");
			f.write("first.jai", "import \"leaf.jai\";\nint first_value = leaf_value + 1;\n");
			f.write("second.jai", "int second_value = 5;\n");
			const std::string root = f.write("root.jai", "import \"first.jai\";\nimport \"second.jai\";\nint total = first_value + second_value;\n");
			auto e = reporting_engine(f);
			e->execute_file(root);
			const load_report_summary report = e->load_report_snapshot();
			check_true(file_names(report) == std::vector<std::string>({"root.jai", "first.jai", "leaf.jai", "second.jai"}), "depth-first from the root");
			for (const auto& row : report.rows) {
				check_gt(row.source_bytes, uint64_t(0));
				check_eq(uint64_t(1), row.source_reads, "each file is opened once: " + row.file);
				check_gt(count_of(row, load_phase::stamp_query), uint64_t(0));
				check_gt(count_of(row, load_phase::lex), uint64_t(0));
				check_gt(count_of(row, load_phase::parse), uint64_t(0));
				check_eq(uint64_t(1), row.checks_run);
				check_eq(uint64_t(1), count_of(row, load_phase::run));
				check_eq(uint64_t(1), count_of(row, load_phase::cache_write));
			}
			check_gt(count_of(report.engine, load_phase::import_bookkeeping), uint64_t(0), "imports leave work no file owns");
		});

		test("a_cold_load_misses_and_writes_and_a_warm_one_hits_and_trusts_the_stamp", [this]() {
			folder f("warm");
			f.write("mod.jai", "int mod_value = 7;\nint twice(int x) { return x * 2; }\n");
			const std::string root = f.write("root.jai", "import \"mod.jai\";\nint r = twice(mod_value);\n");
			{
				auto cold = reporting_engine(f);
				cold->execute_file(root);
				const load_report_summary report = cold->load_report_snapshot();
				for (const std::string name : {"root.jai", "mod.jai"}) {
					const load_report_row& row = row_of(report, name);
					check_true(row.cache == load_cache::miss, name + " misses");
					check_eq(std::string("no file"), row.cache_reason);
					check_eq(std::string("written"), row.cache_write);
					check_gt(row.cache_bytes, uint64_t(0));
					check_eq(uint64_t(0), count_of(row, load_phase::decode));
					check_eq(uint64_t(1), row.checks_run);
					check_eq(uint64_t(0), row.checks_trusted);
				}
			}
			auto warm = reporting_engine(f);
			warm->execute_file(root);
			const load_report_summary report = warm->load_report_snapshot();
			for (const std::string name : {"root.jai", "mod.jai"}) {
				const load_report_row& row = row_of(report, name);
				check_true(row.cache == load_cache::hit, name + " hits");
				check_eq(std::string(), row.cache_reason);
				check_eq(std::string("up to date"), row.cache_write);
				check_gt(row.cache_bytes, uint64_t(0));
				check_eq(uint64_t(1), count_of(row, load_phase::cache_read));
				check_eq(uint64_t(1), count_of(row, load_phase::decode));
				check_eq(uint64_t(0), count_of(row, load_phase::lex));
				check_eq(uint64_t(0), count_of(row, load_phase::parse));
				check_eq(uint64_t(0), count_of(row, load_phase::cache_write));
				check_eq(uint64_t(0), row.checks_run, "the stamp stands in for the check");
				check_eq(uint64_t(1), row.checks_trusted);
			}
		});

		test("a_miss_says_why", [this]() {
			folder f("reasons");
			const std::string root = f.write("root.jai", "int a = 1;\n");
			const auto reason = [&](const std::function<void(engine&)>& a_setup = {}) {
				auto e = reporting_engine(f);
				if (a_setup) { a_setup(*e); }
				e->execute_file(root);
				const load_report_summary report = e->load_report_snapshot();
				const load_report_row& row = row_of(report, "root.jai");
				check_true(row.cache == load_cache::miss);
				return row.cache_reason;
			};
			check_eq(std::string("no file"), reason());
			f.write("root.jai", "int a = 2;\n");
			check_eq(std::string("different source"), reason());
			check_eq(std::string("fingerprint"), reason([](engine& e) { e.add_function("load_report_extra", std::function<int()>([] { return 1; })); }));
			reason();   // the plain engine's cache again

			const std::filesystem::path cache = f.dir / ".cache" / "root.jai.jaibite";
			std::string bytes;
			{
				std::ifstream in(cache, std::ios::binary);
				std::ostringstream text;
				text << in.rdbuf();
				bytes = text.str();
			}
			check_gt(bytes.size(), size_t(36));
			std::string other_version = bytes;
			other_version[4] = static_cast<char>(other_version[4] + 1);
			std::ofstream(cache, std::ios::binary | std::ios::trunc) << other_version;
			check_eq(std::string("format version"), reason());
			std::ofstream(cache, std::ios::binary | std::ios::trunc) << bytes.substr(0, 10);
			check_eq(std::string("corrupt"), reason());
		});

		test("a_cache_another_registration_wrote_is_replaced_so_the_next_load_hits", [this]() {
			folder f("fingerprints");
			f.write("leaf.jai", "int leaf_value = 3;\n");
			f.write("mod.jai", "import \"leaf.jai\";\nint twice(int x) { return x * 2; }\n");
			f.write("part.jai", "int part_value = 4;\n");
			const std::string root = f.write("root.jai", "import \"mod.jai\";\ninclude \"part.jai\";\nint r = twice(leaf_value) + part_value;\n");
			const std::vector<std::string> files {"root.jai", "mod.jai", "leaf.jai", "part.jai"};
			const auto other_host = [](engine& e) { e.add_function("load_report_other_host", std::function<int()>([] { return 1; })); };
			const auto load = [&](const std::function<void(engine&)>& a_setup) {
				auto e = reporting_engine(f);
				if (a_setup) { a_setup(*e); }
				e->execute_file(root);
				check_eq(script_int(10), e->get_variable("r").value().as_int());
				return e->load_report_snapshot();
			};
			load(other_host);
			// a host of another registration finds every cache written under the first one's and replaces it
			const load_report_summary replacing = load({});
			for (const std::string& name : files) {
				const load_report_row& row = row_of(replacing, name);
				check_true(row.cache == load_cache::miss, name + " misses");
				check_eq(std::string("fingerprint"), row.cache_reason, name);
				check_eq(std::string("written"), row.cache_write, name + " is written again");
			}
			const load_report_summary again = load({});
			for (const std::string& name : files) {
				check_true(row_of(again, name).cache == load_cache::hit, name + " hits on the next load");
			}
			// the first host's next load replaces them in turn
			const load_report_summary back = load(other_host);
			for (const std::string& name : files) {
				check_eq(std::string("written"), row_of(back, name).cache_write, name + " is written for the first host again");
			}
			const load_report_summary settled = load(other_host);
			for (const std::string& name : files) {
				check_true(row_of(settled, name).cache == load_cache::hit, name + " hits for the first host");
			}
			// two live engines of the two registrations each write a file once, however often they run it in turn
			auto first = reporting_engine(f);
			other_host(*first);
			auto second = reporting_engine(f);
			for (int round = 0; round < 3; ++round) {
				first->execute_file(root);
				second->execute_file(root);
			}
			for (const auto& e : {first, second}) {
				const load_report_summary report = e->load_report_snapshot();
				for (const std::string& name : files) {
					check_true(count_of(row_of(report, name), load_phase::cache_write) <= 1, name + " is written at most once by each engine");
				}
			}
		});

		test("a_unit_with_a_spliced_file_keeps_no_cache_and_says_so", [this]() {
			folder f("spliced");
			f.write("part.inc", "int spliced_local = 4;\n");
			const std::string root = f.write("root.jai", "int use() {\n\tinclude \"part.inc\";\n\treturn spliced_local;\n}\nint r = use();\n");
			auto e = reporting_engine(f);
			e->execute_file(root);
			const load_report_summary report = e->load_report_snapshot();
			const load_report_row& row = row_of(report, "root.jai");
			check_true(row.cache == load_cache::not_cacheable);
			check_eq(std::string("spliced files"), row.cache_reason);
			check_eq(std::string("spliced files"), row.cache_write);
			check_eq(uint64_t(0), count_of(row, load_phase::cache_write));
			check_false(std::filesystem::exists(f.dir / ".cache" / "root.jai.jaibite"), "no cache file");
			const load_report_row& part = row_of(report, "part.inc");
			check_eq(uint64_t(1), part.source_reads, "the spliced file has a row of its own");
			check_true(part.cache == load_cache::none);
		});

		test("a_unit_using_another_files_generic_keeps_no_cache_and_says_so", [this]() {
			folder f("generics");
			f.write("gen.jai", "T same<T>(T value) { return value; }\n");
			const std::string root = f.write("root.jai", "import \"gen.jai\";\nint r = same<int>(3);\n");
			auto e = reporting_engine(f);
			e->execute_file(root);
			const load_report_summary report = e->load_report_snapshot();
			const load_report_row& row = row_of(report, "root.jai");
			check_true(row.cache == load_cache::not_cacheable);
			check_eq(std::string("another file's generics"), row.cache_reason);
			check_gt(count_of(row, load_phase::generics), uint64_t(0));
			check_true(row_of(report, "gen.jai").cache == load_cache::miss, "the generic's own file caches");
		});

		test("source_text_and_a_disabled_cache_are_not_cacheable", [this]() {
			folder f("nocache");
			const std::string root = f.write("root.jai", "int a = 1;\n");
			auto text = reporting_engine(f);
			text->execute("int b = 2;");
			const load_report_summary text_report = text->load_report_snapshot();
			check_eq(std::string("<script>"), text_report.rows.at(0).file);
			check_true(text_report.rows.at(0).cache == load_cache::not_cacheable);
			check_eq(std::string("source text"), text_report.rows.at(0).cache_reason);

			auto off = reporting_engine(f);
			off->jaibite_cache(false);
			off->execute_file(root);
			const load_report_summary off_report = off->load_report_snapshot();
			check_true(row_of(off_report, "root.jai").cache == load_cache::not_cacheable);
			check_eq(std::string("cache off"), row_of(off_report, "root.jai").cache_reason);
			check_eq(std::string(), row_of(off_report, "root.jai").cache_write);
		});

		test("running_again_is_a_memory_hit_that_keeps_what_the_first_load_found", [this]() {
			folder f("memory");
			const std::string root = f.write("root.jai", "int a = 1;\n");
			auto e = reporting_engine(f);
			e->execute_file(root);
			e->execute_file(root);
			const load_report_summary report = e->load_report_snapshot();
			check_eq(size_t(1), report.rows.size());
			check_true(report.rows[0].cache == load_cache::miss);
			check_eq(uint64_t(2), count_of(report.rows[0], load_phase::run));
		});

		test("the_phases_add_up_to_the_wall_time", [this]() {
			folder f("reconcile");
			f.write("mod.jai", "int twice(int x) { return x * 2; }\n");
			const std::string root = f.write("root.jai", "import \"mod.jai\";\nint r = twice(4);\n");
			auto e = reporting_engine(f);
			e->execute_file(root);
			const load_report_summary report = e->load_report_snapshot();
			uint64_t sum = report.engine.nanoseconds();
			for (const auto& row : report.rows) { sum += row.nanoseconds(); }
			check_gt(report.wall_nanoseconds, uint64_t(0));
			check_eq(report.wall_nanoseconds, sum);
			check_eq(sum, report.totals.nanoseconds());
			check_eq(report.wall_nanoseconds - report.rows[0].phase(load_phase::run).nanoseconds, report.load_nanoseconds());
		});

		test("functions_are_counted_and_only_called_ones_compile", [this]() {
			folder f("bodies");
			const std::string root = f.write("root.jai",
				"int called() { return 1; }\nint never() { return 2; }\n"
				"class Box { int get() { return 3; } int unused() { return 4; } }\n"
				"namespace tools { int spare() { return 5; } }\n"
				"Box box; int r = called() + box.get();\n");
			auto e = reporting_engine(f);
			e->execute_file(root);
			const load_report_summary report = e->load_report_snapshot();
			const load_report_row& row = row_of(report, "root.jai");
			check_eq(uint64_t(5), row.functions);
			check_gt(row.function_bytes, uint64_t(0));
			if (e->get_backend_type() == backend_type::vm) {
				check_eq(uint64_t(2), row.functions_compiled);
				check_gt(row.function_bytes_compiled, uint64_t(0));
				check_lt(row.function_bytes_compiled, row.function_bytes);
				check_eq(uint64_t(1), count_of(row, load_phase::vm_compile));
				check_gt(count_of(row, load_phase::body_compile), uint64_t(0));
			} else {
				check_eq(uint64_t(0), row.functions_compiled);
				check_eq(uint64_t(0), count_of(row, load_phase::vm_compile));
				check_eq(uint64_t(0), count_of(row, load_phase::body_compile));
			}
		});

		test("a_literal_template_is_charged_to_its_file", [this]() {
			folder f("template");
			f.write("page.tpl", "<p>{{ name }}</p>\n");
			const std::string root = f.write("root.jai", "string name = \"x\";\nstring page = template(\"page.tpl\", name);\n");
			auto e = reporting_engine(f);
			stdlib::register_all(e);
			e->execute_file(root);
			const load_report_summary report = e->load_report_snapshot();
			const load_report_row& row = row_of(report, "page.tpl");
			check_gt(count_of(row, load_phase::template_compile), uint64_t(0));
			check_eq(uint64_t(1), row.source_reads);
		});

		test("both_backends_report_the_same_rows_but_for_the_vms_compiles", [this]() {
			const auto load = [&](backend_type a_backend, const std::string& a_name, int a_runs) {
				folder f("backend_" + a_name);
				f.write("part.inc", "int from_part = 1;\n");
				f.write("mod.jai", "int mod_value = 2;\nint helper() { int kept = 1; include \"part.inc\"; return kept + from_part; }\n");
				const std::string root = f.write("root.jai", "import \"mod.jai\";\nint r = helper() + mod_value;\n");
				load_report_summary report;
				for (int i = 0; i < a_runs; ++i) {
					auto e = reporting_engine(f, a_backend);
					e->execute_file(root);
					report = e->load_report_snapshot();
				}
				return report;
			};
			for (int runs = 1; runs <= 2; ++runs) {
				const load_report_summary vm = load(backend_type::vm, "vm" + std::to_string(runs), runs);
				const load_report_summary interp = load(backend_type::interpreter, "interp" + std::to_string(runs), runs);
				check_true(vm.vm);
				check_false(interp.vm);
				check_true(file_names(vm) == file_names(interp));
				check_eq(vm.rows.size(), interp.rows.size());
				for (size_t i = 0; i < vm.rows.size() && i < interp.rows.size(); ++i) {
					const load_report_row& a = vm.rows[i];
					const load_report_row& b = interp.rows[i];
					check_true(a.cache == b.cache, a.file);
					check_eq(a.cache_reason, b.cache_reason);
					check_eq(a.cache_write, b.cache_write);
					check_eq(a.source_bytes, b.source_bytes);
					check_eq(a.source_reads, b.source_reads);
					check_eq(a.checks_run, b.checks_run);
					check_eq(a.checks_trusted, b.checks_trusted);
					check_eq(a.functions, b.functions);
					for (size_t p = 0; p < k_load_phase_count; ++p) {
						const load_phase phase = static_cast<load_phase>(p);
						if (phase == load_phase::vm_compile || phase == load_phase::body_compile) { continue; }
						if (phase == load_phase::other || phase == load_phase::stamp_query) { continue; }   // bookkeeping counts, not phases of the program
						check_eq(a.phases[p].count, b.phases[p].count, a.file + " " + load_phase_name(phase));
					}
					check_eq(uint64_t(0), b.phase(load_phase::vm_compile).count);
					check_eq(uint64_t(0), b.phase(load_phase::body_compile).count);
				}
				check_gt(vm.rows[0].phase(load_phase::vm_compile).count, uint64_t(0));
			}
		});

		test("the_report_changes_nothing_the_program_does", [this]() {
			folder f("same");
			f.write("mod.jai", "print(\"mod ran\");\nint value = 6;\n");
			const std::string root = f.write("root.jai", "import \"mod.jai\";\nprint(value * 7);\nvalue;\n");
			const auto run = [&](bool a_report) {
				auto out = std::make_shared<std::ostringstream>();
				auto e = make_engine();
				e->set_output_stream(out);
				e->add_include_path(f.dir.string());
				e->load_report(a_report);
				const held_value result = e->execute_file(root);
				return out->str() + "=" + std::to_string(result.as_int());
			};
			const std::string plain = run(false);
			check_eq(plain, run(true));
			check_eq(plain, run(true), "and again over the caches");
		});

		test("the_table_and_the_json_name_every_file", [this]() {
			folder f("text");
			f.write("mod.jai", "int value = 6;\n");
			const std::string root = f.write("root.jai", "import \"mod.jai\";\nint r = value;\n");
			auto e = reporting_engine(f);
			e->load_report_setup_time(0.001);
			e->execute_file(root);
			const load_report_summary report = e->load_report_snapshot();
			check_eq(uint64_t(1000000), report.setup_nanoseconds);
			const std::string table = report.format();
			check_true(table.find("root.jai") != std::string::npos && table.find("mod.jai") != std::string::npos, table);
			check_true(table.find("miss: no file") != std::string::npos, table);
			check_true(table.find("engine setup") != std::string::npos, table);

			auto reader = make_engine();
			stdlib::register_all(reader);
			reader->add_global("report_text", script_value(report.json(), reader.get()));
			check_eq(int64_t(2), reader->execute("from_json(report_text)[\"rows\"].size()").as_int());
		});
	}
};

}

using load_report_tests = jai::foundry::tests::load_report_tests;
FOUNDRY_REGISTER(load_report_tests)
