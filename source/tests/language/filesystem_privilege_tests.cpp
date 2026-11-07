#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/reflection.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace jai::foundry::tests {

struct filesystem_binding_probe {
    script_int read() { return 17; }
    script_int query() const { return 23; }
};

class filesystem_privilege_tests : public suite {
public:
    filesystem_privilege_tests() : suite("Filesystem Privilege") {}

    static void filesystem_enabled(engine& eng, bool enabled) {
        if (enabled) { eng.enable(privilege::filesystem); }
        else { eng.disable(privilege::filesystem); }
    }

    template<class F>
    static std::string failure(F&& operation) {
        try { operation(); }
        catch (const std::exception& error) { return error.what(); }
        return {};
    }

    static std::string denied() { return "Insufficient privilege: filesystem."; }
    // The host boundary drops a category the text already begins with, so an uncaught
    // denial reads exactly as the caught one.
    static std::string runtime_denied() { return denied(); }

    template<class Function>
    static void add_protected(engine& eng, const std::string& name, Function function) {
        eng.add_function(name, std::move(function), privilege::filesystem);
    }

    static void bind_probe(engine& eng) {
        dynamic_binder<filesystem_binding_probe> binder(eng, "FilesystemProbe");
        binder.constructor<>();
        binder.method("read", &filesystem_binding_probe::read, privilege::filesystem)
            .method("query", &filesystem_binding_probe::query, privilege::filesystem)
            .method("lambda", [](filesystem_binding_probe& self) { return self.read(); }, privilege::filesystem);
        binder.build();
    }

    static std::filesystem::path fixture(const char* name, const std::string& content) {
        const auto result = test_temp_directory() / name;
        std::ofstream(result, std::ios::binary) << content;
        return result;
    }

    void forge_tests() override {
        test("default_enabled_and_engine_local", [this]() {
            auto restricted = make_engine();
            auto trusted = make_engine();
            check_true(trusted->execute("reflect::is_enabled(jai::filesystem)").as_bool());
            filesystem_enabled(*restricted, false);
            check_false(restricted->execute("reflect::is_enabled(jai::filesystem)").as_bool());
            check_true(trusted->execute("reflect::is_enabled(jai::filesystem, jai::filesystem)").as_bool());
            filesystem_enabled(*restricted, true);
            check_true(restricted->execute("reflect::is_enabled(jai::filesystem | jai::filesystem)").as_bool());
        });

        test("all_location_queries_are_gated", [this]() {
            const char* queries[] = {
                "root_script_path()", "root_script_dir()", "current_script_path()", "current_script_dir()",
                "resolve_path(\"missing.jai\")", "executable_path()", "executable_dir()", "bin_dir()",
                "working_dir()", "parent_dir(\"a/b.jai\")", "file_name(\"a/b.jai\")",
                "app_data_dir(\"JaiTest\")", "app_cache_dir(\"JaiTest\")", "file_data(\"missing.jai\")"
            };
            auto eng = make_engine();
            filesystem_enabled(*eng, false);
            for (const char* query : queries) {
                check_eq(runtime_denied(), failure([&]() { eng->execute(query); }), query);
                check_eq(script_int(42), eng->execute("40 + 2").as_int());
            }
        });

        test("denial_precedes_host_directory_callback", [this]() {
            auto eng = make_engine();
            int calls = 0;
            eng->app_directory_resolvers([&](const std::string&) { ++calls; return "/data"; },
                                         [&](const std::string&) { ++calls; return "/cache"; });
            filesystem_enabled(*eng, false);
            check_eq(runtime_denied(), failure([&]() { eng->execute("app_data_dir(\"test\")"); }));
            check_eq(runtime_denied(), failure([&]() { eng->execute("app_cache_dir(\"test\")"); }));
            check_eq(0, calls);
        });

        test("saved_alias_checks_current_permission", [this]() {
            auto eng = make_engine();
            eng->execute("var saved_query = working_dir;");
            const auto original = eng->execute("saved_query()").as_string();
            filesystem_enabled(*eng, false);
            check_eq(runtime_denied(), failure([&]() { eng->execute("saved_query()"); }));
            filesystem_enabled(*eng, true);
            check_eq(original, eng->execute("saved_query()").as_string());
        });

        test("reentrant_disable_affects_next_call_not_started_call", [this]() {
            auto eng = make_engine();
            eng->app_directory_resolvers([&](const std::string&) {
                filesystem_enabled(*eng, false);
                return std::string("already-started");
            }, {});
            check_eq(std::string("already-started"), eng->execute("app_data_dir(\"test\")").as_string());
            check_eq(runtime_denied(), failure([&]() { eng->execute("working_dir()"); }));
            filesystem_enabled(*eng, true);
            eng->add_function("revoke_filesystem", [&]() { filesystem_enabled(*eng, false); });
            check_eq(runtime_denied(), failure([&]() { eng->execute("revoke_filesystem(); working_dir();"); }));
            check_eq(script_int(42), eng->execute("40 + 2").as_int());
        });

        test("denial_is_catchable_with_identical_output", [this]() {
            auto eng = make_engine();
            auto output = std::make_shared<std::ostringstream>();
            eng->set_output_stream(output);
            filesystem_enabled(*eng, false);
            const auto result = eng->execute(
                "var caught = \"\"; try { working_dir(); } catch (e) { caught = e; print(e); } caught;");
            check_eq(denied(), result.as_string());
            check_eq(denied() + "\n", output->str());
        });

        test("include_import_and_resolver_share_denial", [this]() {
            const auto path = fixture("privilege_include.jai", "40 + 2;");
            const auto spelling = path.generic_string();
            const std::string operations[] = {
                "include \"" + spelling + "\";", "include(\"" + spelling + "\");",
                "var included = include(\"" + spelling + "\");",
                "import \"" + spelling + "\";", "resolve_path(\"" + spelling + "\")"
            };
            for (const auto& source : operations) {
                auto eng = make_engine();
                filesystem_enabled(*eng, false);
                check_eq(runtime_denied(), failure([&]() { eng->execute(source); }), source);
            }
            auto eng = make_engine();
            filesystem_enabled(*eng, false);
            auto result = resolve_include_path(spelling, eng.get());
            check_false(bool(result));
            if (!result) { check_eq(denied(), std::string(result.message())); }
        });

        test("warm_file_import_and_bytecode_apis_cannot_bypass", [this]() {
            const auto path = fixture("privilege_host.jai", "40 + 2;");
            const auto bite_path = fixture("privilege_host.jaibite", "sentinel");
            auto eng = make_engine();
            eng->jaibite_cache(false);
            check_eq(script_int(42), eng->execute_file(path.string()).as_int());
            detail::engine_access::execute_import(*eng, path.string());
            auto bite = eng->jaibite("40 + 2;");
            bite.save(bite_path.string());
            auto bytes = bite.save_bytes();
            filesystem_enabled(*eng, false);
            check_eq(denied(), failure([&]() { eng->execute_file(path.string()); }));
            check_eq(denied(), failure([&]() { detail::engine_access::execute_import(*eng, path.string()); }));
            check_eq(denied(), failure([&]() { eng->jaibite_load(bite_path.string()); }));
            check_eq(denied(), failure([&]() { bite.save(bite_path.string()); }));
            check_eq(bytes, bite.save_bytes());
            check_eq(script_int(42), eng->jaibite_load_bytes(bytes).execute().as_int());
        });

        test("supplied_source_labels_do_not_create_disk_cache", [this]() {
            auto eng = make_engine();
            filesystem_enabled(*eng, false);
            const auto path = test_temp_directory() / "privilege_virtual.jai";
            const auto sibling = jaibite_cache_beside(path);
            check_eq(script_int(42), eng->execute_file_source(path.string(), "40 + 2;").as_int());
            check_false(std::filesystem::exists(sibling));
            check_eq(size_t(0), eng->jaibite_cache_write_failures());
        });

        test("restricted_source_ignores_fresh_disk_sibling", [this]() {
            const auto path = fixture("privilege_stale.jai", "40 + 2;");
            const auto sibling = jaibite_cache_beside(path);
            std::filesystem::create_directories(sibling.parent_path());
            auto eng = make_engine();
            eng->jaibite("7;").save(sibling.string());
            std::filesystem::last_write_time(sibling,
                std::filesystem::last_write_time(path) + std::chrono::seconds(5));
            filesystem_enabled(*eng, false);
            check_eq(script_int(42), eng->execute_file_source(path.string(), "40 + 2;").as_int());
        });

        test("checker_does_not_discover_disk_dependencies_when_restricted", [this]() {
            const auto path = fixture("privilege_check.jai", "function disk_only_name() -> int { return 42; }");
            const auto source = "import \"" + path.generic_string() + "\"; disk_only_name();";
            auto restricted = make_engine();
            filesystem_enabled(*restricted, false);
            check_true(restricted->check(source).has_errors());
            filesystem_enabled(*restricted, true);
            check_false(restricted->check(source).has_errors());
        });

        test("reflection_query_validates_arguments", [this]() {
            auto eng = make_engine();
            check_eq(std::string("reflect::is_enabled expects at least 1 argument"),
                failure([&]() { eng->execute("reflect::is_enabled()"); }));
            for (const char* args : {"true", "-1", "4", "jai::filesystem, 4"}) {
                check_eq(std::string("reflect::is_enabled expects valid privilege flags"),
                    failure([&]() { eng->execute(std::string("reflect::is_enabled(") + args + ")"); }));
            }
        });

        test("native_overloads_and_reflected_aliases_keep_the_gate", [this]() {
            auto eng = make_engine();
            stdlib::register_reflection_functions(eng);
            int calls = 0;
            add_protected(*eng, "protected_read", [&](script_int) { ++calls; return script_int(7); });
            add_protected(*eng, "protected_read", [&](const std::string&) { ++calls; return script_int(9); });
            eng->execute("var alias_read = protected_read;");
            check_eq(script_int(7), eng->execute("alias_read(1)").as_int());
            check_eq(script_int(9), eng->execute("protected_read(\"x\")").as_int());
            filesystem_enabled(*eng, false);
            for (const char* source : {"protected_read(1)", "alias_read(\"x\")", "reflect::call(\"protected_read\", 1)"}) {
                check_eq(runtime_denied(), failure([&]() { eng->execute(source); }));
            }
            check_eq(2, calls);
        });

        test("native_methods_and_saved_receiver_check_live_grants", [this]() {
            auto eng = make_engine();
            stdlib::register_reflection_functions(eng);
            bind_probe(*eng);
            eng->execute("var probe = FilesystemProbe(); var saved_read = reflect::bind(probe, \"read\");");
            check_eq(script_int(17), eng->execute("saved_read()").as_int());
            filesystem_enabled(*eng, false);
            for (const char* source : {"probe.read()", "probe.query()", "probe.lambda()", "saved_read()", "reflect::invoke(probe, \"read\")"}) {
                check_eq(runtime_denied(), failure([&]() { eng->execute(source); }));
            }
            filesystem_enabled(*eng, true);
            check_eq(script_int(23), eng->execute("probe.query()").as_int());
        });

        test("confine_paths_limits_the_ladder_to_its_roots", [this]() {
            // include, import, resolve_path and file_data open only files under the roots; a file found outside is skipped,
            // its text never reaches an error, and no .cache is written beside a source outside them
            namespace fs = std::filesystem;
            const fs::path inside = test_temp_directory() / "confine_in";
            const fs::path outside = test_temp_directory() / "confine_out";
            fs::create_directories(inside);
            fs::create_directories(outside);
            std::ofstream(inside / "lib.jai", std::ios::binary) << "int confined_lib() { return 5; }\n";
            std::ofstream(inside / "both.jai", std::ios::binary) << "int which() { return 1; }\n";
            std::ofstream(outside / "both.jai", std::ios::binary) << "int which() { return 2; }\n";
            std::ofstream(outside / "secret.txt", std::ios::binary) << "API_KEY=sk-secret\n";
            std::ofstream(outside / "root.jai", std::ios::binary) << "40 + 2;\n";
            auto eng = make_engine();
            eng->add_include_path(outside.string());
            eng->add_include_path(inside.string());
            eng->confine_paths({inside.string()});
            check_eq(size_t(1), eng->confine_paths().size());
            check_eq(script_int(6), eng->execute("include \"lib.jai\"; include \"both.jai\"; confined_lib() + which();").as_int(),
                     "the outside both.jai is skipped for the inside one");
            const std::string secret = (outside / "secret.txt").generic_string();
            const std::string escapes[] = {
                "include \"" + secret + "\";", "import \"" + secret + "\";", "include \"../confine_out/secret.txt\";",
                "resolve_path(\"" + secret + "\");", "file_data(\"" + secret + "\");"
            };
            for (const auto& source : escapes) {
                const std::string text = failure([&]() { eng->execute(source); });
                check_true(text.find("Path outside the confined roots: ") != std::string::npos, source + " -> " + text);
                check_true(text.find("API_KEY") == std::string::npos, "the file's text never shows: " + text);
            }
            check_eq(std::string(), eng->jaibite_cache_path((outside / "root.jai").string()));
            check_eq(script_int(42), eng->execute_file((outside / "root.jai").string()).as_int(), "the host still runs any file it names");
            check_false(fs::exists(jaibite_cache_beside(outside / "root.jai")), "no .cache beside a source outside the roots");
            check_true(fs::exists(jaibite_cache_beside(inside / "lib.jai")));
            eng->confine_paths({});
            check_false(eng->execute("resolve_path(\"" + secret + "\");").to_string().empty(), "empty roots open any readable path");
            fs::remove_all(inside);
            fs::remove_all(outside);
        });

        test("host_path_queries_obey_the_same_gate", [this]() {
            auto eng = make_engine();
            filesystem_enabled(*eng, false);
            check_eq(denied(), failure([&]() { eng->app_data_dir("JaiTest"); }));
            check_eq(denied(), failure([&]() { eng->app_cache_dir("JaiTest"); }));
        });
    }
};

} // namespace jai::foundry::tests

using filesystem_privilege_tests = jai::foundry::tests::filesystem_privilege_tests;
FOUNDRY_REGISTER(filesystem_privilege_tests)
