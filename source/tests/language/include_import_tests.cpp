#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/core/engine_time.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/detail/ast_serializer.hpp>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <thread>
#include <chrono>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class include_import_tests : public suite {
public:
    include_import_tests() : suite("Include/Import Tests") {}
    
    void forge_tests() override {
        // Helper to create a temporary file
        auto create_temp_file = [](const std::string& filename, const std::string& content) {
            std::filesystem::path temp_dir = jai::foundry::test_temp_directory();
            std::filesystem::path file_path = temp_dir / filename;
            std::ofstream file(file_path);
            file << content;
            file.close();
            return file_path.string();
        };
        
        // Helper to clean up temporary file
        auto cleanup_temp_file = [](const std::string& path) {
            std::filesystem::remove(path);
        };
        
        test("basic_include", [=, this]() {
            auto eng = make_engine();
            
            // Create a temporary file to include
            auto math_file = create_temp_file("math_lib.jai", R"(
                function add(int a, int b) -> int {
                    return a + b;
                }
                auto PI = 3.14159;
            )");
            
            // Add temp directory to include paths
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            
            // Include the file
            eng->execute("include \"math_lib.jai\"");
            
            // Verify functions and variables are available
            check_eq(eng->execute("add(2, 3)").as<int>(), 5);
            check_eq(eng->execute("PI").as<double>(), 3.14159);
            
            cleanup_temp_file(math_file);
        });
        
        test("include_with_angle_brackets", [=, this]() {
            auto eng = make_engine();
            
            // Create a temporary file
            auto lib_file = create_temp_file("stdlib.jai", R"(
                function multiply(int a, int b) -> int {
                    return a * b;
                }
            )");
            
            // Add temp directory to include paths
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            
            // Include with angle brackets
            eng->execute("include <stdlib.jai>");
            
            // Verify function is available
            check_eq(eng->execute("multiply(4, 5)").as<int>(), 20);
            
            cleanup_temp_file(lib_file);
        });
        
        test("multiple_includes_execute_multiple_times", [=, this]() {
            auto eng = make_engine();
            
            // Create a file with side effects
            auto counter_file = create_temp_file("counter.jai", R"(
                auto counter = 0;
            )");
            
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            
            // Initialize counter
            eng->execute("include \"counter.jai\"");
            
            // Now create increment file
            cleanup_temp_file(counter_file);
            counter_file = create_temp_file("counter.jai", R"(
                // counter already exists from first include, so we can increment it
                counter = counter + 1;
            )");
            
            // Include multiple times - each should execute
            eng->execute("include \"counter.jai\"");
            check_eq(eng->execute("counter").as<int>(), 1);
            
            eng->execute("include \"counter.jai\"");
            check_eq(eng->execute("counter").as<int>(), 2);
            
            eng->execute("include \"counter.jai\"");
            check_eq(eng->execute("counter").as<int>(), 3);
            
            cleanup_temp_file(counter_file);
        });
        
        test("basic_import_once_behavior", [=, this]() {
            auto eng = make_engine();
            
            // Create a file with side effects
            auto module_file = create_temp_file("module.jai", R"(
                auto module_loaded = 1;
                
                function get_module_count() -> int {
                    return module_loaded;
                }
            )");
            
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            eng->set_import_behavior(engine::import_behavior::once);
            
            // Import multiple times - should only execute once
            eng->execute("import \"module.jai\"");
            check_eq(eng->execute("get_module_count()").as<int>(), 1);
            
            eng->execute("import \"module.jai\"");
            check_eq(eng->execute("get_module_count()").as<int>(), 1);
            
            eng->execute("import \"module.jai\"");
            check_eq(eng->execute("get_module_count()").as<int>(), 1);
            
            cleanup_temp_file(module_file);
        });
        
        test("import_file_timestamp_behavior", [=, this]() {
            auto eng = make_engine();
            
            // Create initial file
            auto test_file = create_temp_file("timestamp_test.jai", R"(
                auto version = 1;
            )");
            
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            eng->set_import_behavior(engine::import_behavior::file_timestamp);
            
            // First import
            eng->execute("import \"timestamp_test.jai\"");
            check_eq(eng->execute("version").as<int>(), 1);
            
            // Import again without modification - should not re-import
            eng->execute("import \"timestamp_test.jai\"");
            check_eq(eng->execute("version").as<int>(), 1);
            
            // Modify the file (with small delay to ensure timestamp changes)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::ofstream file(test_file);
            file << "auto version = 2;";
            file.close();
            
            // Import again - should re-import due to changed timestamp
            eng->execute("import \"timestamp_test.jai\"");
            check_eq(eng->execute("version").as<int>(), 2);
            
            cleanup_temp_file(test_file);
        });
        
        test("import_always_behavior", [=, this]() {
            auto eng = make_engine();
            
            // Create a file with counter
            auto always_file = create_temp_file("always_import.jai", R"(
                auto import_count = 0;
            )");
            
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            eng->set_import_behavior(engine::import_behavior::always);
            
            // Initialize counter
            eng->execute("import \"always_import.jai\"");
            
            // Now create increment file
            cleanup_temp_file(always_file);
            always_file = create_temp_file("always_import.jai", R"(
                import_count = import_count + 1;
            )");
            
            // Import multiple times - should execute every time
            eng->execute("import \"always_import.jai\"");
            check_eq(eng->execute("import_count").as<int>(), 1);
            
            eng->execute("import \"always_import.jai\"");
            check_eq(eng->execute("import_count").as<int>(), 2);
            
            eng->execute("import \"always_import.jai\"");
            check_eq(eng->execute("import_count").as<int>(), 3);
            
            cleanup_temp_file(always_file);
        });
        
        test("include_path_resolution", [=, this]() {
            auto eng = make_engine();
            
            // Create files in different directories
            auto temp_dir = jai::foundry::test_temp_directory();
            auto lib_dir = temp_dir / "lib";
            auto scripts_dir = temp_dir / "scripts";
            
            std::filesystem::create_directories(lib_dir);
            std::filesystem::create_directories(scripts_dir);
            
            // Create same filename in different directories
            std::ofstream lib_file(lib_dir / "utils.jai");
            lib_file << "auto from_lib = true;";
            lib_file.close();
            
            std::ofstream scripts_file(scripts_dir / "utils.jai");
            scripts_file << "auto from_scripts = true;";
            scripts_file.close();
            
            // Test path priority - first added path takes precedence
            eng->add_include_path(lib_dir.string());
            eng->add_include_path(scripts_dir.string());
            
            eng->execute("include \"utils.jai\"");
            check(eng->execute("from_lib").as<bool>());
            check_throws([&]() { eng->execute("from_scripts"); });
            
            // Clean up
            std::filesystem::remove_all(lib_dir);
            std::filesystem::remove_all(scripts_dir);
        });
        
        test("import_tracking_api", [=, this]() {
            auto eng = make_engine();
            
            // Create test files
            auto file1 = create_temp_file("tracked1.jai", "auto file1_loaded = true;");
            auto file2 = create_temp_file("tracked2.jai", "auto file2_loaded = true;");
            
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            eng->set_import_behavior(engine::import_behavior::once);
            
            // Initially no imports
            check(eng->get_imported_files().empty());
            
            // Import first file
            eng->execute("import \"tracked1.jai\"");
            check(eng->is_imported(std::filesystem::canonical(file1).string()));
            check(!eng->is_imported(std::filesystem::canonical(file2).string()));
            check_eq(eng->get_imported_files().size(), 1);
            
            // Import second file
            eng->execute("import \"tracked2.jai\"");
            check(eng->is_imported(std::filesystem::canonical(file1).string()));
            check(eng->is_imported(std::filesystem::canonical(file2).string()));
            check_eq(eng->get_imported_files().size(), 2);
            
            // Reset specific import
            eng->reset_import(std::filesystem::canonical(file1).string());
            check(!eng->is_imported(std::filesystem::canonical(file1).string()));
            check(eng->is_imported(std::filesystem::canonical(file2).string()));
            
            // Reset all imports
            eng->reset_imports();
            check(eng->get_imported_files().empty());
            
            cleanup_temp_file(file1);
            cleanup_temp_file(file2);
        });
        
        test("hash_prefix_ignored", [=, this]() {
            auto eng = make_engine();
            
            // Create a test file
            auto hash_file = create_temp_file("hash_test.jai", R"(
                auto with_hash = 42;
            )");
            
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            
            // Test that # prefix is ignored
            eng->execute("#include \"hash_test.jai\"");
            check_eq(eng->execute("with_hash").as<int>(), 42);
            
            // Also works with import
            eng->execute("#import <hash_test.jai>");
            
            cleanup_temp_file(hash_file);
        });
        
        test("file_not_found_error", [=, this]() {
            auto eng = make_engine();
            
            // Try to include non-existent file
            check_throws([&]() {
                eng->execute("include \"non_existent_file.jai\"");
            });
            
            check_throws([&]() {
                eng->execute("import <another_missing_file.jai>");
            });
        });
        
        test("nested_includes", [=, this]() {
            auto eng = make_engine();
            
            // Create nested include files
            auto inner_file = create_temp_file("inner.jai", R"(
                auto inner_value = 100;
            )");
            
            auto outer_file = create_temp_file("outer.jai", R"(
                include "inner.jai"
                auto outer_value = inner_value + 50;
            )");
            
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            
            // Include outer file which includes inner
            eng->execute("include \"outer.jai\"");
            
            check_eq(eng->execute("inner_value").as<int>(), 100);
            check_eq(eng->execute("outer_value").as<int>(), 150);
            
            cleanup_temp_file(inner_file);
            cleanup_temp_file(outer_file);
        });
        
        test("dynamic_include_with_variable", [=, this]() {
            auto eng = make_engine();
            
            // Create a test file
            auto dynamic_file = create_temp_file("dynamic_content.jai", R"(
                auto dynamic_value = 42;
                function get_dynamic() -> int {
                    return dynamic_value * 2;
                }
            )");
            
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            
            // Set up a variable with the filename
            eng->execute("auto filename = \"dynamic_content.jai\";");
            
            // Include using the variable
            eng->execute("include(filename);");
            
            // Verify the content was included
            check_eq(eng->execute("dynamic_value").as<int>(), 42);
            check_eq(eng->execute("get_dynamic()").as<int>(), 84);
            
            cleanup_temp_file(dynamic_file);
        });
        
        test("include_is_textual_the_file_runs_at_the_include_site", [=, this]() {
            // Includes are the includer's scope. The included declarations belong to the including unit (a
            // later `var x` in the includer writes the included x in place, and the included function sees it), and an include
            // inside a block or a function body scopes with it (docs/site/guide/10-modules.html)
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            auto lib = create_temp_file("textual_lib.jai", R"(
                var x = 3;
                function readx() { return x; }
            )");
            auto blk = create_temp_file("textual_blk.jai", R"(
                var y = 9;
            )");
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            check_eq(std::string("3 5 5"), eng->execute(R"(var x = 1; include "textual_lib.jai"; var after = x; var x = 5; to_string(after) + " " + to_string(x) + " " + to_string(readx());)").as<std::string>());
            check_eq(std::string("9 gone 9 gone"), eng->execute(R"(string r = ""; { include "textual_blk.jai"; r = to_string(y); } try { r = r + " " + to_string(y); } catch (e) { r = r + " gone"; } function f() { include "textual_blk.jai"; return y; } r = r + " " + to_string(f()); try { r = r + " " + to_string(y); } catch (e) { r = r + " gone"; } r;)").as<std::string>());
            cleanup_temp_file(lib);
            cleanup_temp_file(blk);
        });
        test("a_literal_include_inside_a_function_is_spliced_into_the_function", [=, this]() {
            // An include inside a function sees its parameters and locals (pay for what you use): a
            // literal include in a function body is parsed into the body (C++), so the checker and both backends see one
            // function; the included text reads and writes the parameter, declares a local the function then uses, defines a
            // function; a nested literal include splices too
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            auto local = create_temp_file("splice_local.jai", "x = x + 1; x;");
            auto lib = create_temp_file("splice_lib.jai", "var y = x * 2; function twice(int v) { return v * 2; } include \"splice_inner.jai\";");
            auto inner = create_temp_file("splice_inner.jai", "var z = x + 100;");
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            check_eq((int64_t)6, eng->execute("int run(int x) { include(\"splice_local.jai\"); return x; } run(5);").as_int());
            check_eq((int64_t)6, eng->execute("int run2(int x) { include \"splice_local.jai\"; return x; } run2(5);").as_int());
            check_eq((int64_t)125, eng->execute("int both(int x) { include \"splice_lib.jai\"; return y + twice(x) + z; } both(5);").as_int());   // 10 + 10 + 105
            check_eq((int64_t)7, eng->execute("int blk(int x) { { include \"splice_local.jai\"; } { include \"splice_local.jai\"; } return x; } blk(5);").as_int());   // each block's include writes the parameter
            auto self = create_temp_file("splice_self.jai", "int g() {\n    include \"splice_self.jai\";\n    return 1;\n}\n");
            std::string cycle;
            try { eng->execute_file(self); } catch (const std::exception& ex) { cycle = ex.what(); }
            check_true(cycle.find("splice_self.jai:2:13: error: include 'splice_self.jai' includes itself or nests too deep") != std::string::npos, "the error names the include itself: " + cycle);
            cleanup_temp_file(self);
            cleanup_temp_file(local);
            cleanup_temp_file(lib);
            cleanup_temp_file(inner);
        });
        test("a_function_body_include_declares_nested_functions_and_no_class", [=, this]() {
            // The spliced text is the function's own, as pasted C++ text is: a function it declares is a
            // nested function, local to the body and gone after it, the program-scope function of that name untouched (the
            // strict checker warns that it hides one); a class, always a program-scope type, cannot be declared there
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            auto fns = create_temp_file("splice_fns.jai", "function helper() -> string { return \"included\"; }\nfunction only_here() -> string { return \"only\"; }\n");
            auto cls = create_temp_file("splice_cls.jai", "var before = 1;\nclass SplicedK { int v = 2; }\n");
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            check_eq(std::string("included only|program|gone"), eng->execute(R"(
                function helper() -> string { return "program"; }
                function reload() -> string { include "splice_fns.jai"; return helper() + " " + only_here(); }
                string r = reload() + "|" + helper() + "|";
                [[warn_mode]] try { only_here(); r += "still"; } catch (e) { r += "gone"; }
                r;
            )").as<std::string>());
            size_t hides = 0;
            for (const auto& d : eng->last_check_diagnostics().diagnostics) {
                if (d.severity == check_diagnostic::level::warning &&
                    d.message.find("splice_fns.jai:1:10: function 'helper' from the include in function 'reload' is local to it and hides the program-scope 'helper' there without replacing it; import the file to replace program-scope functions") != std::string::npos) { ++hides; }
                check_true(d.message.find("function 'only_here' from the include") == std::string::npos, "a nested function hiding nothing is silent");
            }
            check_eq((size_t)1, hides, "the checker warns once for the hidden program-scope function");
            std::string err;
            try { eng->execute("function reload2() { include \"splice_cls.jai\"; } reload2();"); } catch (const std::exception& ex) { err = ex.what(); }
            check_true(err.find("an include inside a function body cannot declare class 'SplicedK' (a class is a program-scope type and the included text is the function's own); import 'splice_cls.jai' instead") != std::string::npos, err);
            check_throws([&]() { eng->execute("SplicedK k; k.v;"); });
            cleanup_temp_file(fns);
            cleanup_temp_file(cls);
        });
        test("an_import_inside_a_function_reloads_functions_and_classes", [=, this]() {
            // the reload a function-body include cannot do: an import is program scope wherever it stands, so a re-run replaces
            // functions and classes in place and live instances migrate
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->import_recheck_interval(0);
            auto mod = create_temp_file("reload_by_import.jai", "function greet() -> string { return \"v1\"; }\nclass ReloadP { int n = 1; string who() { return \"P1\"; } }\n");
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            check_eq(std::string("v1 P1"), eng->execute(R"(
                import "reload_by_import.jai";
                function reload_scripts() -> bool { return import("reload_by_import.jai"); }
                ReloadP keep = ReloadP();
                `${greet()} ${keep.who()}`;
            )").as<std::string>());
            mod = create_temp_file("reload_by_import.jai", "function greet() -> string { return \"v2!\"; }\nclass ReloadP { int n = 1; int extra = 7; string who() { return \"P2\"; } }\n");
            check_true(eng->execute("reload_scripts();").as_bool(), "the import re-ran the touched module");
            check_eq(std::string("v2! P2 7"), eng->execute("`${greet()} ${keep.who()} ${keep.extra}`;").as<std::string>());
            check_false(eng->execute("reload_scripts();").as_bool(), "an untouched module does not re-run");
            cleanup_temp_file(mod);
        });
        test("a_computed_include_path_inside_a_function_stays_a_runtime_include", [=, this]() {
            // pay for what you use: a computed path cannot be spliced, so it runs in the function's environment and does not
            // see the frame (the checker names the included x undeclared at the include's load)
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            auto local = create_temp_file("splice_dyn.jai", "x = x + 1; x;");
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            check_throws([&]() { eng->execute("int run(int x) { var p = \"splice_dyn.jai\"; include(p); return x; } run(5);"); });
            cleanup_temp_file(local);
        });
        test("a_changed_spliced_file_reparses_its_includer", [=, this]() {
            // the parse cache stamps the spliced files: the includer's text is unchanged, the included file is not
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            auto local = create_temp_file("splice_reload.jai", "x = x + 1; x;");
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            const std::string dir = jai::foundry::test_temp_directory().string();
            const std::string includer = dir + "/splice_includer.jai";
            const char* text = "int run(int x) { include \"splice_reload.jai\"; return x; } return run(5);";
            check_eq((int64_t)6, eng->execute_file_source(includer, text).as_int());
            local = create_temp_file("splice_reload.jai", "x = x + 1000; x;");   // a different size: the stamp moves even within one clock tick
            check_eq((int64_t)1005, eng->execute_file_source(includer, text).as_int());
            cleanup_temp_file(local);
        });
        test("a_changed_spliced_file_reparses_its_includer_on_every_cache_tier", [=, this]() {
            // Regression on the spliced-include cache path: the same engine's execute_file (the file cache's fast path) and a literal
            // execute (the literal cache's fast path) kept the old parse when only the spliced file changed (3, 3, 3 for 3, 11, 119); every tier
            // revalidates the spliced stamps before a hit runs. Different sizes move the stamp within one clock tick
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            auto dep = create_temp_file("splice_live_dep.jai", "return 3;");
            auto parent = create_temp_file("splice_live_parent.jai", "int probe() { include \"splice_live_dep.jai\"; return 7; } return probe();");
            check_eq((int64_t)3, eng->execute_file(parent).as_int());
            dep = create_temp_file("splice_live_dep.jai", "return 11;");
            check_eq((int64_t)11, eng->execute_file(parent).as_int());
            dep = create_temp_file("splice_live_dep.jai", "return 119;");
            check_eq((int64_t)119, eng->execute_file(parent).as_int());
            dep = create_temp_file("splice_live_dep.jai", "return 5;");
            check_eq((int64_t)5, eng->execute("int lit() { include \"splice_live_dep.jai\"; return 7; } return lit();").as_int());
            dep = create_temp_file("splice_live_dep.jai", "return 55;");
            check_eq((int64_t)55, eng->execute("int lit() { include \"splice_live_dep.jai\"; return 7; } return lit();").as_int());
            cleanup_temp_file(dep);
            cleanup_temp_file(parent);
        });
        test("a_textual_include_shared_by_two_units_resolves_each_units_cells", [=, this]() {
            // An included file's compiled program and the functions it defines belong to
            // the including unit; two units including the same file each get their own, so the VM's lookup caches never serve
            // another unit's binding
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            auto reader = create_temp_file("unit_reader.jai", "x;");
            auto writer = create_temp_file("unit_writer.jai", "x = x + 1; x;");
            auto fnlib = create_temp_file("unit_fnlib.jai", "function getx() { return x; }");
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            const std::string dir = jai::foundry::test_temp_directory().string();
            const std::string unitA = dir + "/unit_A.jai", unitB = dir + "/unit_B.jai";   // a file unit is its path: the content is a reload
            eng->execute_file_source(unitA, "var x = 11;");
            eng->execute_file_source(unitB, "var x = 22;");
            std::string out;
            for (int k = 0; k < 2; ++k) {
                out += eng->execute_file_source(unitA, "return include(\"unit_reader.jai\");").to_string() + " ";
                out += eng->execute_file_source(unitB, "return include(\"unit_reader.jai\");").to_string() + " ";
            }
            check_eq(std::string("11 22 11 22 "), out);
            out.clear();
            for (int k = 0; k < 2; ++k) {
                out += eng->execute_file_source(unitA, "return include(\"unit_writer.jai\");").to_string() + " ";
                out += eng->execute_file_source(unitB, "return include(\"unit_writer.jai\");").to_string() + " ";
            }
            check_eq(std::string("12 23 13 24 "), out);
            out.clear();
            for (int k = 0; k < 2; ++k) {
                out += eng->execute_file_source(unitA, "include \"unit_fnlib.jai\"; return getx();").to_string() + " ";
                out += eng->execute_file_source(unitB, "include \"unit_fnlib.jai\"; return getx();").to_string() + " ";
            }
            check_eq(std::string("13 24 13 24 "), out);
            cleanup_temp_file(reader);
            cleanup_temp_file(writer);
            cleanup_temp_file(fnlib);
        });
        test("dynamic_import_with_expression", [=, this]() {
            auto eng = make_engine();
            
            // Create test files
            auto moduleA = create_temp_file("moduleA.jai", R"(
                auto module_letter = "A";
            )");
            
            auto moduleB = create_temp_file("moduleB.jai", R"(
                auto module_letter = "B";
            )");
            
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            eng->set_import_behavior(engine::import_behavior::once);
            
            // Import using string concatenation
            eng->execute("auto module_name = \"module\" + \"A\" + \".jai\";");
            eng->execute("import(module_name);");
            check_eq(eng->execute("module_letter").as<std::string>(), "A");
            
            // Try to import module B
            eng->execute("module_name = \"module\" + \"B\" + \".jai\";");
            eng->execute("import(module_name);");
            
            // Now we should have module B imported too
            eng->execute("auto letterB = \"B\";");
            eng->execute("import(\"module\" + letterB + \".jai\");");
            
            // Since we're using 'once' behavior, importing again should not change anything. The same reader text
            // executed again resolves as of this run, so it sees module B's declaration
            check_eq(eng->execute("module_letter").as<std::string>(), "B");

            cleanup_temp_file(moduleA);
            cleanup_temp_file(moduleB);
        });

        // === include in expression position (include returns its value) ===
        // `var cfg = include "file.jai";` evaluates to the included file's result — exactly
        // what engine::execute returns for that source. Statement-position include is
        // unchanged (value discarded). include always RUNS the file (no cache), so its
        // value is always fresh — no interaction with import's once-cache.
        test("include_expression_returns_value", [=, this]() {
            auto config_file = create_temp_file("expr_config.jai", R"(
                { "width": 800, "height": 600 };
            )");
            auto expr_file = create_temp_file("expr_calc.jai", R"(
                var acc = 0;
                for (int i = 1; i <= 4; i++) { acc += i; }
                acc * 10;
            )");
            for (bool use_vm : {false, true}) {
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                eng->add_include_path(jai::foundry::test_temp_directory().string());
                auto r = eng->execute(R"(
                    var cfg = include "expr_config.jai";
                    auto calc = include "expr_calc.jai";
                    cfg["width"] + cfg["height"] + calc;
                )");
                check_eq((int64_t)1500, r.as_int(), use_vm ? "vm include value" : "interp include value");
            }
            cleanup_temp_file(config_file);
            cleanup_temp_file(expr_file);
        });

        test("include_expression_dynamic_path_and_nesting", [=, this]() {
            auto leaf = create_temp_file("expr_leaf.jai", "21;");
            for (bool use_vm : {false, true}) {
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                eng->add_include_path(jai::foundry::test_temp_directory().string());
                // computed path form in expression position + in-call use
                auto r = eng->execute(R"(
                    function twice(v) { return v * 2; }
                    var part = "leaf";
                    twice(include("expr_" + part + ".jai"));
                )");
                check_eq((int64_t)42, r.as_int(), use_vm ? "vm dynamic include" : "interp dynamic include");
            }
            cleanup_temp_file(leaf);
        });

        test("include_statement_position_unchanged", [=, this]() {
            // Statement include still runs for side effects and DISCARDS its value; it
            // also always re-runs (fresh evaluation each time, unlike import)
            auto counter_file = create_temp_file("expr_counter.jai", R"(
                bump_count();
                777;
            )");
            for (bool use_vm : {false, true}) {
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                int bumps = 0;
                eng->add_function("bump_count", [&bumps]() { bumps++; });
                eng->add_include_path(jai::foundry::test_temp_directory().string());
                auto r = eng->execute(R"(
                    include "expr_counter.jai";
                    include "expr_counter.jai";
                    var v = include "expr_counter.jai";
                    v;
                )");
                check_eq((int64_t)777, r.as_int(), "expression include still returns");
                check_eq(3, bumps, use_vm ? "vm include always runs" : "interp include always runs");
            }
            cleanup_temp_file(counter_file);
        });

        // === jaibite disk cache: every file-based load maintains <folder>/.cache/<file>.jaibite; a
        // cache whose header records the exact source (size + hash) and the registration
        // fingerprint loads instead of parsing; anything else reparses and rewrites.

        auto sibling_of = [](const std::string& src) {
            return jai::foundry::jaibite_cache_beside(src).string();
        };
        auto planted_sibling_of = [](const std::string& src) {   // a test writing its own cache first makes the folder
            const auto result = jai::foundry::jaibite_cache_beside(src);
            std::filesystem::create_directories(result.parent_path());
            return result.string();
        };
        auto starts_with_jbit = [](const std::string& path) {
            std::ifstream f(path, std::ios::binary);
            char magic[4] = {};
            f.read(magic, 4);
            return f.gcount() == 4 && magic[0] == 'J' && magic[1] == 'B' && magic[2] == 'I' && magic[3] == 'T';
        };
        // the sibling's recorded source identity rewritten to the file's current text: a load then proves the parse was
        // skipped, since that text need not parse
        auto relabel_cache = [](const std::string& src, const std::string& cache) {
            std::ifstream in(src);
            std::stringstream text;
            text << in.rdbuf();
            const auto id = jai::detail::jaibite_identity_of(text.str());
            std::fstream f(cache, std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(20);
            for (uint64_t v : {id.size, id.hash}) {
                for (int i = 0; i < 8; ++i) { f.put(char(uint8_t(v >> (8 * i)))); }
            }
        };
        auto relabel_sibling = [sibling_of](const std::string& src) {
            std::ifstream in(src);
            std::stringstream text;
            text << in.rdbuf();
            const auto id = jai::detail::jaibite_identity_of(text.str());
            std::fstream f(sibling_of(src), std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(20);
            for (uint64_t v : {id.size, id.hash}) {
                for (int i = 0; i < 8; ++i) { f.put(char(uint8_t(v >> (8 * i)))); }
            }
        };

        test("jaibite_cache_creates_sibling_on_file_loads", [=, this]() {
            for (bool use_vm : {false, true}) {
                auto src = create_temp_file("cache_make.jai", "auto cache_make_v = 11;");
                std::filesystem::remove(sibling_of(src));
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                eng->add_include_path(jai::foundry::test_temp_directory().string());
                eng->execute("include \"cache_make.jai\"");
                check(std::filesystem::exists(sibling_of(src)), "include writes the sibling");
                check(starts_with_jbit(sibling_of(src)), "sibling is a jaibite (JBIT magic)");

                std::filesystem::remove(sibling_of(src));
                eng->execute_file(src);
                check(std::filesystem::exists(sibling_of(src)), "execute_file writes the sibling");

                std::filesystem::remove(sibling_of(src));
                eng->execute("import \"cache_make.jai\"");
                check(std::filesystem::exists(sibling_of(src)), "import writes the sibling");

                cleanup_temp_file(src);
                std::filesystem::remove(sibling_of(src));
            }
        });

        test("jaibite_cache_fresh_sibling_skips_parse", [=, this]() {
            // Proof the parse is skipped: a sibling recording the source's identity (relabeled
            // here) loads even though the .jai itself does not parse.
            for (bool use_vm : {false, true}) {
                auto src = create_temp_file("cache_skip.jai", "42;");
                std::filesystem::remove(sibling_of(src));
                {
                    auto writer = jai::foundry::make_default_engine();
                    writer->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    writer->add_include_path(jai::foundry::test_temp_directory().string());
                    check_eq((int64_t)42, writer->execute("var r = include \"cache_skip.jai\"; r;").as_int());
                }
                {
                    std::ofstream f(src, std::ios::trunc);
                    f << "%%% not a script %%%";
                }
                relabel_sibling(src);
                auto reader = jai::foundry::make_default_engine();
                reader->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                reader->add_include_path(jai::foundry::test_temp_directory().string());
                check_eq((int64_t)42, reader->execute("var r = include \"cache_skip.jai\"; r;").as_int(),
                         use_vm ? "vm: cache hit skipped the parse" : "interp: cache hit skipped the parse");
                cleanup_temp_file(src);
                std::filesystem::remove(sibling_of(src));
            }
        });

        test("jaibite_cache_loaded_ast_keeps_file_attribution", [=, this]() {
            // Composition pin with the debugger's path stamping: an AST loaded from the
            // sibling (parse provably skipped — the source is unparseable) still carries
            // the .jai filename in its source_locations, so stack traces name the file.
            for (bool use_vm : {false, true}) {
                auto src = create_temp_file("cache_attr.jai", "throw \"attr\";");
                std::filesystem::remove(sibling_of(src));
                {
                    auto writer = jai::foundry::make_default_engine();
                    writer->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    try { writer->execute_file(src); } catch (...) {}
                }
                {
                    std::ofstream f(src, std::ios::trunc);
                    f << "%%%";
                }
                relabel_sibling(src);
                auto reader = jai::foundry::make_default_engine();
                reader->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                try { reader->execute_file(src); } catch (...) {}
                auto trace = reader->last_stack_trace();
                check(!trace.empty(), "trace captured from cache-loaded AST");
                check_eq(src, trace.back().file,
                         use_vm ? "vm: cache-loaded AST names the .jai" : "interp: cache-loaded AST names the .jai");
                cleanup_temp_file(src);
                std::filesystem::remove(sibling_of(src));
            }
        });

        test("jaibite_cache_edit_goes_stale_equal_mtime_included", [=, this]() {
            for (bool use_vm : {false, true}) {
                auto src = create_temp_file("cache_edit.jai", "1;");
                std::filesystem::remove(sibling_of(src));
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                eng->add_include_path(jai::foundry::test_temp_directory().string());
                check_eq((int64_t)1, eng->execute("var a = include \"cache_edit.jai\"; a;").as_int());
                // edit lands on the very next include, same engine run, with the sibling's
                // mtime: the sibling records the old text, so the new one parses
                {
                    std::ofstream f(src, std::ios::trunc);
                    f << "2;";
                }
                std::filesystem::last_write_time(src, std::filesystem::last_write_time(sibling_of(src)));
                check_eq((int64_t)2, eng->execute("var b = include \"cache_edit.jai\"; b;").as_int(),
                         "equal mtimes = stale -> reparse");
                cleanup_temp_file(src);
                std::filesystem::remove(sibling_of(src));
            }
        });

        test("jaibite_cache_corrupt_sibling_falls_back_and_rewrites", [=, this]() {
            for (bool use_vm : {false, true}) {
                auto src = create_temp_file("cache_corrupt.jai", "7;");
                {
                    std::ofstream f(planted_sibling_of(src), std::ios::binary | std::ios::trunc);
                    f << "JBITgarbage-not-a-real-bite";
                }
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                eng->add_include_path(jai::foundry::test_temp_directory().string());
                check_eq((int64_t)7, eng->execute("var c = include \"cache_corrupt.jai\"; c;").as_int(),
                         "corrupt sibling -> silent reparse");
                check(starts_with_jbit(sibling_of(src)), "sibling rewritten valid");
                // the rewritten sibling really loads: clobber the source again
                {
                    std::ofstream f(src, std::ios::trunc);
                    f << "%%%";
                }
                relabel_sibling(src);
                auto eng2 = jai::foundry::make_default_engine();
                eng2->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                eng2->add_include_path(jai::foundry::test_temp_directory().string());
                check_eq((int64_t)7, eng2->execute("var d = include \"cache_corrupt.jai\"; d;").as_int());
                cleanup_temp_file(src);
                std::filesystem::remove(sibling_of(src));
            }
        });

        test("jaibite_cache_fingerprint_mismatch_reparses", [=, this]() {
            for (bool use_vm : {false, true}) {
                auto src = create_temp_file("cache_fp.jai", "100;");
                std::filesystem::remove(sibling_of(src));
                {   // engine A stamps its (extra-function) fingerprint into the sibling
                    auto a = jai::foundry::make_default_engine();
                    a->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    a->add_function("fp_extra", [](script_int v) -> script_int { return v; });
                    a->add_include_path(jai::foundry::test_temp_directory().string());
                    check_eq((int64_t)100, a->execute("var r = include \"cache_fp.jai\"; r;").as_int());
                }
                // source now says 200 and the sibling is relabeled to it: only the fingerprint differs
                {
                    std::ofstream f(src, std::ios::trunc);
                    f << "200;";
                }
                relabel_sibling(src);
                {   // engine B: different registration surface -> stale -> parses the source
                    auto b = jai::foundry::make_default_engine();
                    b->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    b->add_include_path(jai::foundry::test_temp_directory().string());
                    check_eq((int64_t)200, b->execute("var r = include \"cache_fp.jai\"; r;").as_int(),
                             "fingerprint mismatch -> reparse");
                }
                // B's reparse rewrote the sibling under B's (bare) surface; an engine
                // with the same bare surface now loads it (fingerprint matches)
                {
                    std::ofstream f(src, std::ios::trunc);
                    f << "%%%";
                }
                relabel_sibling(src);
                {
                    auto c = jai::foundry::make_default_engine();
                    c->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    c->add_include_path(jai::foundry::test_temp_directory().string());
                    check_eq((int64_t)200, c->execute("var r = include \"cache_fp.jai\"; r;").as_int(),
                             "matching fingerprint -> sibling loads");
                }
                cleanup_temp_file(src);
                std::filesystem::remove(sibling_of(src));
            }
        });

        test("jaibite_cache_version_mismatch_reparses_and_rewrites", [=, this]() {
            // Format-version stamp: a sibling from an older
            // parser/format silently reparses + rewrites — stale parse-time semantics
            // (e.g. slot assignment changes) must never load
            for (bool use_vm : {false, true}) {
                auto src = create_temp_file("cache_ver.jai", "42;");
                {   // valid magic, WRONG version, then padding past the header minimum
                    std::ofstream f(planted_sibling_of(src), std::ios::binary | std::ios::trunc);
                    const uint8_t magic[4] = { 'J', 'B', 'I', 'T' };
                    const uint32_t bad_version = 0x7FFFFFFF;
                    const uint32_t flags = 0;
                    const uint64_t fingerprint = 0;
                    f.write(reinterpret_cast<const char*>(magic), 4);
                    f.write(reinterpret_cast<const char*>(&bad_version), 4);
                    f.write(reinterpret_cast<const char*>(&flags), 4);
                    f.write(reinterpret_cast<const char*>(&fingerprint), 8);
                }
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                eng->add_include_path(jai::foundry::test_temp_directory().string());
                check_eq((int64_t)42, eng->execute("var v = include \"cache_ver.jai\"; v;").as_int(),
                         "version mismatch -> silent reparse");
                check(starts_with_jbit(sibling_of(src)), "sibling rewritten");
                // the rewrite carries the CURRENT version: clobber the source and reload
                {
                    std::ofstream f(src, std::ios::trunc);
                    f << "%%%";
                }
                relabel_sibling(src);
                auto eng2 = jai::foundry::make_default_engine();
                eng2->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                eng2->add_include_path(jai::foundry::test_temp_directory().string());
                check_eq((int64_t)42, eng2->execute("var w = include \"cache_ver.jai\"; w;").as_int(),
                         "rewritten sibling loads under the current version");
                cleanup_temp_file(src);
                std::filesystem::remove(sibling_of(src));
            }
        });

        test("jaibite_cache_disable_switch", [=, this]() {
            for (bool use_vm : {false, true}) {
                auto src = create_temp_file("cache_off.jai", "5;");
                std::filesystem::remove(sibling_of(src));
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                check(eng->jaibite_cache(), "cache defaults ON");
                eng->jaibite_cache(false);
                check(!eng->jaibite_cache(), "setter/getter");
                eng->add_include_path(jai::foundry::test_temp_directory().string());
                check_eq((int64_t)5, eng->execute("var r = include \"cache_off.jai\"; r;").as_int());
                check(!std::filesystem::exists(sibling_of(src)), "disabled -> no sibling written");
                cleanup_temp_file(src);
            }
        });

        test("jaibite_cache_write_failure_is_silent_and_counted", [=, this]() {
            for (bool use_vm : {false, true}) {
                auto src = create_temp_file("cache_ro.jai", "9;");
                std::filesystem::remove(sibling_of(src));
                std::filesystem::create_directory(planted_sibling_of(src));   // sibling path unwritable
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                eng->add_include_path(jai::foundry::test_temp_directory().string());
                check_eq((int64_t)9, eng->execute("var r = include \"cache_ro.jai\"; r;").as_int(),
                         "unwritable sibling never surfaces as an error");
                check(eng->jaibite_cache_write_failures() >= 1, "failure counted for diagnostics");
                std::filesystem::remove(sibling_of(src));
                cleanup_temp_file(src);
            }
        });

        auto put_file = [](const std::filesystem::path& path, const std::string& text) {
            std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        };
        auto temporaries_beside = [](const std::filesystem::path& sibling) {
            int count = 0;
            const std::string prefix = sibling.filename().string() + ".";
            for (const auto& e : std::filesystem::directory_iterator(sibling.parent_path())) {
                const std::string name = e.path().filename().string();
                count += name.rfind(prefix, 0) == 0 && e.path().extension() == ".tmp";
            }
            return count;
        };

        test("the_cache_lives_in_a_cache_folder_beside_the_source", [=, this]() {
            // <folder>/.cache/<file>.jaibite, the folder made when missing; sources that differ only in extension keep two caches
            const auto dir = jai::foundry::test_temp_directory() / "cache_folder";
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir);
            put_file(dir / "unit.jai", "int unit_v() { return 3; }\nunit_v();\n");
            put_file(dir / "unit.script", "4;");
            auto eng = make_engine();
            check_eq(script_int(3), eng->execute_file((dir / "unit.jai").string()).as_int());
            check_eq(script_int(4), eng->execute_file((dir / "unit.script").string()).as_int());
            check_true(std::filesystem::is_directory(dir / ".cache"), "the .cache folder is made");
            check_true(starts_with_jbit((dir / ".cache" / "unit.jai.jaibite").string()), "unit.jai's cache");
            check_true(starts_with_jbit((dir / ".cache" / "unit.script.jaibite").string()), "unit.script's own cache");
            check_false(std::filesystem::exists(dir / "unit.jaibite"), "nothing is written beside the source");
            check_eq((dir / ".cache" / "unit.jai.jaibite").string(), eng->jaibite_cache_path((dir / "unit.jai").string()));
            check_eq(std::string(), eng->jaibite_cache_path((dir / "unit.jaibite").string()), "a .jaibite source keeps no cache");
            check_eq(std::string(), eng->jaibite_cache_directory(), "no cache directory by default");
            std::filesystem::remove_all(dir);
        });

        test("a_cache_directory_takes_every_cache_at_the_source_path_mirrored_below_it", [=, this]() {
            const auto base = jai::foundry::test_temp_directory() / "cache_mirror";
            std::filesystem::remove_all(base);
            const auto project = base / "project";
            const auto caches = base / "caches";
            std::filesystem::create_directories(project / "scripts");
            std::filesystem::create_directories(project / "lib");
            put_file(project / "lib" / "helper.jai", "int helper_v() { return 20; }\n");
            const auto mainSource = project / "scripts" / "main.jai";
            put_file(mainSource, "import \"../lib/helper.jai\";\nhelper_v() + 1;\n");
            auto eng = make_engine();
            eng->jaibite_cache_directory(caches.string(), project.string());
            check_eq(caches.string(), eng->jaibite_cache_directory());
            check_eq(script_int(21), eng->execute_file(mainSource.string()).as_int());
            const auto mainCache = caches / "scripts" / "main.jai.jaibite";
            check_true(starts_with_jbit(mainCache.string()), "the root file's cache mirrors its folder");
            check_true(starts_with_jbit((caches / "lib" / "helper.jai.jaibite").string()), "an import's cache mirrors its folder");
            check_false(std::filesystem::exists(project / "scripts" / ".cache"), "nothing beside the source");
            check_false(std::filesystem::exists(project / "lib" / ".cache"), "nothing beside the import");

            put_file(mainSource, "%%% not a script %%%");
            relabel_cache(mainSource.string(), mainCache.string());
            auto reader = make_engine();
            reader->jaibite_cache_directory(caches.string(), project.string());
            check_eq(script_int(21), reader->execute_file(mainSource.string()).as_int(), "a valid cache in the directory loads: the parse is skipped");

            const std::string outside = (base / "elsewhere" / "x.jai").string();
            check_true(reader->jaibite_cache_path(outside).rfind((caches / "").string(), 0) == 0, "a source outside the root mirrors its whole path below the directory");
            check_eq((caches / "assets" / "y.jai.jaibite").string(), reader->jaibite_cache_path("assets/y.jai"), "a relative source mirrors as given");
            check_eq((caches / "^" / "z.jai.jaibite").string(), reader->jaibite_cache_path("../z.jai"), "a .. step stays inside the directory");
            reader->jaibite_cache_directory("");
            check_eq(jai::foundry::jaibite_cache_beside(outside).string(), reader->jaibite_cache_path(outside), "empty restores the folder beside the source");
            std::filesystem::remove_all(base);
        });

        test("an_unwritable_cache_directory_runs_from_memory_and_tries_once_per_text", [=, this]() {
            const auto base = jai::foundry::test_temp_directory() / "cache_blocked";
            std::filesystem::remove_all(base);
            std::filesystem::create_directories(base);
            put_file(base / "not_a_folder", "a file where the cache directory's parent would be");
            const std::string src = (base / "blocked.jai").string();
            const std::string text = "int blocked_v() { return 8; }\nblocked_v();\n";
            put_file(src, text);
            auto eng = make_engine();
            eng->jaibite_cache_directory((base / "not_a_folder" / "caches").string());
            for (int i = 0; i < 4; ++i) {
                check_eq(script_int(8), eng->execute_file_source(src, text).as_int(), "the source runs, no error");
            }
            check_eq(size_t(1), eng->jaibite_cache_write_failures(), "one attempt for one text, never one per run");
            check_eq(script_int(9), eng->execute_file_source(src, "int blocked_v() { return 9; }\nblocked_v();\n").as_int());
            check_eq(size_t(2), eng->jaibite_cache_write_failures(), "a new text tries once more");
            check_false(std::filesystem::exists(base / ".cache"), "nothing is written beside the source instead");
            std::filesystem::remove_all(base);
        });

        test("a_valid_cache_beside_the_source_loads_under_a_cache_directory", [=, this]() {
            // a bundle ships its .cache folders read-only: read when the directory holds no cache for the source, never copied
            const auto base = jai::foundry::test_temp_directory() / "cache_shipped";
            std::filesystem::remove_all(base);
            std::filesystem::create_directories(base);
            const std::string src = (base / "shipped.jai").string();
            put_file(src, "int shipped_v() { return 30; }\nshipped_v();\n");
            check_eq(script_int(30), make_engine()->execute_file(src).as_int());
            const auto shipped = jai::foundry::jaibite_cache_beside(src);
            check_true(starts_with_jbit(shipped.string()), "the build step leaves a cache beside the source");
            const std::string unparseable = "%%% not a script %%%";
            put_file(src, unparseable);
            relabel_cache(src, shipped.string());
            auto eng = make_engine();
            eng->jaibite_cache_directory((base / "caches").string(), base.string());
            check_eq(script_int(30), eng->execute_file(src).as_int(), "the shipped cache loads: the parse is skipped");
            check_eq(script_int(30), eng->execute_file_source(src, unparseable).as_int(), "and holds on an in-memory hit");
            check_eq(size_t(0), eng->jaibite_cache_write_failures());
            check_false(std::filesystem::exists(base / "caches"), "a cache that loaded is not written again");
            std::filesystem::remove_all(base);
        });

        // A cache holds a parse and a checked-clean stamp for the context the file met: the namespace classes the parse asked
        // about and the outside declarations the check resolved against. A warm load answers exactly as a cold one would.
        auto context_dir = [](const std::string& name) {
            const auto result = jai::foundry::test_temp_directory() / ("cache_context_" + name);
            std::filesystem::remove_all(result);
            std::filesystem::create_directories(result);
            return result;
        };
        auto outcome_of = [](engine& eng, const std::filesystem::path& file) {
            try {
                return "VALUE " + eng.execute_file(file.string()).to_string();
            } catch (const std::exception& e) {
                return std::string("ERROR ") + e.what();
            }
        };

        test("a_cached_stamp_holds_only_where_the_names_it_resolved_mean_the_same", [=, this]() {
            const auto dir = context_dir("verdict");
            put_file(dir / "ctx_state.jai", "int CTX_G = 7;\nclass CtxStats { int power = 3; }\n");
            put_file(dir / "ctx_rules.jai", "int ctx_damage(int hits) { return hits * CTX_G; }\nint ctx_power(CtxStats s) { return s.power; }\n");
            put_file(dir / "ctx_game.jai", "import \"ctx_rules.jai\";\nimport \"ctx_state.jai\";\nctx_damage(3);\n");
            put_file(dir / "ctx_alone.jai", "import \"ctx_rules.jai\";\n0;\n");
            check_eq(std::string("VALUE 21"), outcome_of(*make_engine(), dir / "ctx_game.jai"), "the game checks the rules clean and caches them");
            const std::string warm = outcome_of(*make_engine(), dir / "ctx_alone.jai");
            std::filesystem::remove_all(dir / ".cache");
            const std::string cold = outcome_of(*make_engine(), dir / "ctx_alone.jai");
            check_true(cold.find("undeclared identifier 'CTX_G'") != std::string::npos, cold);
            check_eq(cold, warm, "a program that never declares the name refuses the cached rules as it refuses parsed ones");

            check_eq(std::string("VALUE 21"), outcome_of(*make_engine(), dir / "ctx_game.jai"));
            put_file(dir / "ctx_state.jai", "int CTX_G = 7;\nclass CtxStats { string power = \"three\"; }\n");
            const std::string retyped_warm = outcome_of(*make_engine(), dir / "ctx_game.jai");
            std::filesystem::remove_all(dir / ".cache");
            const std::string retyped_cold = outcome_of(*make_engine(), dir / "ctx_game.jai");
            check_true(retyped_cold.find("static check failed") != std::string::npos, retyped_cold);
            check_eq(retyped_cold, retyped_warm, "a sibling's changed declaration re-checks the unit that resolved against it");
            std::filesystem::remove_all(dir);
        });

        // geo::Point exists only where ctx_points.jai ran; ctx_probe.jai's bare Point inside its namespace geo names it there,
        // and its own global Point everywhere else
        const std::string points = "namespace geo { class Point { int x = 1; } }\n";
        const std::string probe = "class Point { var x = 2; }\n"
                                  "namespace geo {\n"
                                  "    int probe() { Point p = Point(); return p.x; }\n"
                                  "    void reseat() { Point p = Point(); Point q = Point(); p = q; }\n"
                                  "}\n"
                                  "geo::probe();\n";

        test("a_cached_parse_of_a_reopened_namespace_holds_only_under_the_namespace_classes_it_asked_about", [=, this]() {
            const auto dir = context_dir("namespace");
            put_file(dir / "ctx_points.jai", points);
            put_file(dir / "ctx_probe.jai", probe);
            {   // the cache is written where geo::Point exists
                auto eng = make_engine();
                eng->execute_file((dir / "ctx_points.jai").string());
                check_eq(std::string("VALUE 1"), outcome_of(*eng, dir / "ctx_probe.jai"));
            }
            const std::string warm = outcome_of(*make_engine(), dir / "ctx_probe.jai");
            std::filesystem::remove_all(dir / ".cache");
            const std::string cold = outcome_of(*make_engine(), dir / "ctx_probe.jai");
            check_eq(std::string("VALUE 2"), cold);
            check_eq(cold, warm, "loaded where geo::Point does not exist, the bare Point names the global class");
            {   // in memory: the parse taken before geo::Point existed is not reused once it does
                auto eng = make_engine();
                check_eq(std::string("VALUE 2"), outcome_of(*eng, dir / "ctx_probe.jai"));
                eng->execute_file((dir / "ctx_points.jai").string());
                check_eq(std::string("VALUE 1"), outcome_of(*eng, dir / "ctx_probe.jai"), "the in-memory parse asks again");
            }
            std::filesystem::remove_all(dir);
        });

        test("a_cache_loaded_under_other_namespace_classes_stamps_the_proofs_a_parse_would", [=, this]() {
            const auto dir = context_dir("proofs");
            put_file(dir / "ctx_points.jai", points);
            put_file(dir / "ctx_probe.jai", probe);
            check_eq(std::string("VALUE 2"), outcome_of(*make_engine(), dir / "ctx_probe.jai"), "the cache is written where geo::Point does not exist");
            bool proving = true;
            auto run_after_points = [&](proof_stats& stats) {
                auto eng = make_engine();
                proving = eng->type_proofs();   // the VM reference runs unproven
                eng->execute_file((dir / "ctx_points.jai").string());
                const std::string result = outcome_of(*eng, dir / "ctx_probe.jai");
                stats = eng->last_proof_stats();
                return result;
            };
            proof_stats warm_stats, cold_stats;
            const std::string warm = run_after_points(warm_stats);
            std::filesystem::remove_all(dir / ".cache");
            const std::string cold = run_after_points(cold_stats);
            check_eq(proving, cold_stats.stores > 0, "reseat's bare Point is geo::Point: the store of a geo::Point is proven");
            check_eq(cold_stats.stores, warm_stats.stores, "the loaded program carries the store proof the parsed one does");
            check_eq(std::string("VALUE 1"), cold);
            check_eq(cold, warm, "the probe reads geo::Point's field");
            std::filesystem::remove_all(dir);
        });

        test("a_cached_stamp_holds_only_under_the_host_signatures_it_was_checked_against", [=, this]() {
            // two builds of one host register the same names (one registration fingerprint) with other signatures
            struct gauge { int64_t level = 4; };
            const auto first_build = [](engine& eng) {
                eng.add_function("ctx_scale", [](script_int v) -> script_int { return v * 2; });
                dynamic_binder<gauge>(eng, "CtxGauge").constructor<>().method("read", [](gauge& g) -> script_int { return g.level; }).strict_signatures().build();
            };
            const auto second_build = [](engine& eng) {
                eng.add_function("ctx_scale", [](script_int v, script_int by) -> script_int { return v * by; });
                dynamic_binder<gauge>(eng, "CtxGauge").constructor<>().method("read", [](gauge& g) -> std::string { return std::to_string(g.level); }).strict_signatures().build();
            };
            const auto registration_fingerprint_of = [this](const auto& build) {
                auto eng = make_engine();
                build(*eng);
                return eng->registration_fingerprint();
            };
            const auto dir = context_dir("host");
            put_file(dir / "ctx_scaled.jai", "int ctx_scaled() { return ctx_scale(3); }\n0;\n");
            put_file(dir / "ctx_reading.jai", "int ctx_reading(CtxGauge g) { return g.read(); }\n0;\n");
            for (const char* file : {"ctx_scaled.jai", "ctx_reading.jai"}) {
                const auto run = [&](const auto& build) {
                    auto eng = make_engine();
                    build(*eng);
                    return outcome_of(*eng, dir / file);
                };
                check_eq(std::string("VALUE 0"), run(first_build), std::string(file) + " checks clean under the first build and caches");
                check_eq(registration_fingerprint_of(first_build), registration_fingerprint_of(second_build), "the builds register the same names");
                const std::string warm = run(second_build);
                std::filesystem::remove_all(dir / ".cache");
                const std::string cold = run(second_build);
                check_true(cold.find("static check failed") != std::string::npos, cold);
                check_eq(cold, warm, std::string(file) + ": the second build checks the cached file as it checks a parsed one");
            }
            std::filesystem::remove_all(dir);
        });

        test("a_reimport_runs_the_saved_source_whatever_its_mtime", [=, this]() {
            // a revision put in place with an older mtime (a copy keeping its source file's time), with the same size, or
            // with the same mtime runs its own definitions on the re-run: the sibling written for the version before it
            // records another source, so it never stands in for this one
            const auto dir = jai::foundry::test_temp_directory();
            const auto path = dir / "hr_flight.jai";
            const auto sibling = jai::foundry::jaibite_cache_beside(path);
            struct revision { const char* name; const char* text; int value; int shift; };   // shift: new mtime - old, seconds
            const revision cases[] = {
                {"older mtime, other size", "flight_ran();\nint flight() { return 22; }\n", 22, -3600},
                {"older mtime, same size", "flight_ran();\nint flight() { return 2; }\n", 2, -3600},
                {"same mtime, other size", "flight_ran();\nint flight() { return 222; }\n", 222, 0},
            };
            for (const auto& c : cases) {
                for (bool keep_sibling : {true, false}) {
                    const std::string label = std::string(c.name) + (keep_sibling ? ", sibling present" : ", sibling absent");
                    std::filesystem::remove(sibling);
                    put_file(path, "flight_ran();\nint flight() { return 1; }\n");
                    auto eng = make_engine();
                    eng->add_include_path(dir.string());
                    int runs = 0;
                    eng->add_function("flight_ran", [&runs]() { ++runs; });
                    eng->execute("import \"hr_flight.jai\";");
                    check_eq(script_int(1), eng->execute("flight();").as_int(), label);
                    check_true(std::filesystem::exists(sibling), "the first load writes the sibling: " + label);
                    const auto before = std::filesystem::last_write_time(path);
                    put_file(path, c.text);
                    std::filesystem::last_write_time(path, before + std::chrono::seconds(c.shift));
                    if (!keep_sibling) { std::filesystem::remove(sibling); }
                    check_true(eng->execute("import(\"hr_flight.jai\");").as_bool(), "the stamp moved, the module re-runs: " + label);
                    check_eq(2, runs, label);
                    check_eq(script_int(c.value), eng->execute("flight();").as_int(), "the re-run runs the saved source: " + label);
                    auto fresh = make_engine();
                    fresh->add_include_path(dir.string());
                    fresh->add_function("flight_ran", []() {});
                    fresh->execute("import \"hr_flight.jai\";");
                    check_eq(script_int(c.value), fresh->execute("flight();").as_int(), "a new engine reads the saved source too: " + label);
                }
            }
            std::filesystem::remove(path);
            std::filesystem::remove(sibling);
        });

        test("a_jaibite_sibling_loads_only_for_the_exact_source_it_records", [=, this]() {
            // a fresh engine finds a sibling written for the previous text: older mtime, or same mtime and same size (a tool
            // restoring a file with its old time), the sibling never loads for the new text and is rewritten for it; a bite
            // saved without a source never loads as a sibling
            const auto path = jai::foundry::test_temp_directory() / "hr_restored.jai";
            const auto sibling = jai::foundry::jaibite_cache_beside(path);
            auto bytes_of = [](const std::filesystem::path& p) {
                std::ifstream f(p, std::ios::binary);
                return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            };
            for (int shift : {-3600, 0}) {
                const std::string label = shift ? "older mtime, same size" : "same mtime, same size";
                std::filesystem::remove(sibling);
                put_file(path, "int restored() { return 1; }\nrestored();\n");
                check_eq(script_int(1), make_engine()->execute_file(path.string()).as_int(), label);
                const std::string first = bytes_of(sibling);
                check_false(first.empty(), "the sibling is written: " + label);
                const auto before = std::filesystem::last_write_time(path);
                put_file(path, "int restored() { return 2; }\nrestored();\n");
                std::filesystem::last_write_time(path, before + std::chrono::seconds(shift));
                check_eq(script_int(2), make_engine()->execute_file(path.string()).as_int(), "the new text runs: " + label);
                const std::string second = bytes_of(sibling);
                check_true(!second.empty() && second != first, "the sibling is rewritten for the new text: " + label);
                const auto rewritten = std::filesystem::last_write_time(sibling);
                check_eq(script_int(2), make_engine()->execute_file(path.string()).as_int(), label);
                check_true(std::filesystem::last_write_time(sibling) == rewritten, "a matching sibling loads, unrewritten: " + label);
                check_eq(0, temporaries_beside(sibling), "no temporary file is left: " + label);
            }
            auto saver = make_engine();
            saver->jaibite("7;").save(sibling.string());
            put_file(path, "40 + 2;");
            check_eq(script_int(42), make_engine()->execute_file(path.string()).as_int(), "a bite saved without a source never loads as a sibling");
            std::filesystem::remove(path);
            std::filesystem::remove(sibling);
        });

        test("execute_file_reruns_a_same_size_file_put_back_with_an_older_mtime", [=, this]() {
            // the file fast lane trusts a stamp only while it is equal: an older mtime is a change like a newer one
            const auto path = jai::foundry::test_temp_directory() / "hr_lane.jai";
            put_file(path, "11;");
            auto eng = make_engine();
            check_eq(script_int(11), eng->execute_file(path.string()).as_int());
            check_eq(script_int(11), eng->execute_file(path.string()).as_int(), "the second run verifies the stamp");
            const auto before = std::filesystem::last_write_time(path);
            put_file(path, "22;");
            std::filesystem::last_write_time(path, before - std::chrono::hours(1));
            check_eq(script_int(22), eng->execute_file(path.string()).as_int(), "an older stamp re-reads the file");
            std::filesystem::remove(path);
            std::filesystem::remove(jai::foundry::jaibite_cache_beside(path));
        });

        test("a_sibling_that_cannot_be_replaced_is_kept", [=, this]() {
            // a sibling the rename cannot replace (read-only here; another engine holding it open in a race) stays: the write gives
            // up and leaves it, and its identity keeps it from standing in for the new text
            const auto path = jai::foundry::test_temp_directory() / "hr_keep.jai";
            const auto sibling = jai::foundry::jaibite_cache_beside(path);
            std::filesystem::remove(sibling);
            put_file(path, "int keep_v() { return 5; }\nkeep_v();\n");
            check_eq(script_int(5), make_engine()->execute_file(path.string()).as_int());
            check_true(std::filesystem::exists(sibling), "the first run writes the sibling");
            { std::ofstream(path, std::ios::binary | std::ios::trunc) << "int keep_v() { return 6; }\nkeep_v();\n"; }
            const auto read_only = std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write;
            std::filesystem::permissions(sibling, read_only, std::filesystem::perm_options::remove);
            check_eq(script_int(6), make_engine()->execute_file(path.string()).as_int(), "the new text runs whatever the sibling holds");
            check_true(std::filesystem::exists(sibling), "a sibling the write could not replace is kept");
            check_eq(0, temporaries_beside(sibling), "no temporary file after the attempt");
            std::filesystem::permissions(sibling, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
            check_eq(script_int(6), make_engine()->execute_file(path.string()).as_int(), "the next run parses the new text and rewrites the sibling");
            std::filesystem::remove(sibling);
            std::filesystem::remove(path);
        });

        test("a_jaibite_sibling_is_replaced_whole_or_not_at_all", [=, this]() {
            // the writer renames a finished temporary file over the sibling: a truncated sibling (a crash leftover) parses
            // and is replaced whole, a sibling that cannot be replaced never stands in for the new text, and no temporary
            // file outlives a write, failed or not
            const auto path = jai::foundry::test_temp_directory() / "hr_atomic.jai";
            const auto sibling = jai::foundry::jaibite_cache_beside(path);
            std::filesystem::remove(sibling);
            put_file(path, "int atomic_v() { return 5; }\natomic_v();\n");
            check_eq(script_int(5), make_engine()->execute_file(path.string()).as_int());
            const auto full = std::filesystem::file_size(sibling);
            std::filesystem::resize_file(sibling, jai::detail::k_jaibite_header_size + 3);
            check_eq(script_int(5), make_engine()->execute_file(path.string()).as_int(), "a truncated sibling with the right identity parses");
            check_eq(full, std::filesystem::file_size(sibling), "and is replaced whole");
            check_eq(0, temporaries_beside(sibling), "no temporary file after a write");

            std::filesystem::permissions(sibling, std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write, std::filesystem::perm_options::remove);
            put_file(path, "int atomic_v() { return 6; }\natomic_v();\n");
            auto eng = make_engine();
            check_eq(script_int(6), eng->execute_file(path.string()).as_int(), "a read-only stale sibling never loads for the new text");
            check_eq(0, temporaries_beside(sibling), "no temporary file after a write to a read-only sibling");
            if (std::filesystem::exists(sibling)) {
                std::filesystem::permissions(sibling, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
                std::filesystem::remove(sibling);
            }

            std::filesystem::create_directory(sibling);
            auto blocked = make_engine();
            check_eq(script_int(6), blocked->execute_file(path.string()).as_int());
            check_true(blocked->jaibite_cache_write_failures() >= 1, "a sibling path that is a folder fails the write");
            check_true(std::filesystem::is_directory(sibling), "the folder is left alone");
            check_eq(0, temporaries_beside(sibling), "no temporary file after a failed write");
            std::filesystem::remove(sibling);
            std::filesystem::remove(path);
        });

        test("an_import_re_runs_its_module_when_only_an_included_file_changes", [=, this]() {
            // a module's closure is the module and every file its run included (top level, nested, spliced into a function
            // body): a pass queries each once and a change to any of them, an older mtime included, re-runs the module;
            // between passes nothing is queried
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 0;
            auto write = [&](const char* name, const std::string& body, int shift = 0) {
                const auto path = dir / name;
                put_file(path, body);
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(shift ? shift : 2 * ++tick), ec);
            };
            write("cl_main.jai", "closure_ran();\ninclude \"cl_a.jai\";\nint cl_spliced() { include \"cl_s.jai\"; return S_VALUE; }\n");
            write("cl_a.jai", "include \"cl_b.jai\";\nint cl_a() { return 1; }\n");
            write("cl_b.jai", "int cl_b() { return 2; }\n");
            write("cl_s.jai", "int S_VALUE = 3;\n");
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->add_include_path(dir.string());
            int runs = 0;
            eng->add_function("closure_ran", [&runs]() { ++runs; });
            eng->import_recheck_interval(1.0e6);   // every pass below is opened on purpose
            eng->execute("void frame() { import(\"cl_main.jai\"); }\nframe();");
            check_eq(1, runs);
            check_eq(std::string("1 2 3"), eng->execute("to_string(cl_a()) + \" \" + to_string(cl_b()) + \" \" + to_string(cl_spliced());").to_string());
            const uint64_t warm = detail::engine_access::import_stat_count(*eng);
            eng->execute("for (int i = 0; i < 50; i++) { frame(); }");
            check_eq(warm, detail::engine_access::import_stat_count(*eng), "between passes the closure is not queried");
            eng->import_recheck_interval(1.0e6);
            const uint64_t before = detail::engine_access::import_stat_count(*eng);
            eng->execute("frame();");
            check_eq(1, runs, "an unchanged closure does not re-run");
            check_eq(before + 4, detail::engine_access::import_stat_count(*eng), "one query per file of the closure in a pass");
            struct edit { const char* file; const char* body; int shift; const char* probe; const char* expect; const char* why; };
            const edit edits[] = {
                {"cl_b.jai", "int cl_b() { return 5; }\n", 0, "cl_b();", "5", "a nested include changed"},
                {"cl_a.jai", "include \"cl_b.jai\";\nint cl_a() { return 4; }\n", 0, "cl_a();", "4", "a top-level include changed"},
                {"cl_s.jai", "int S_VALUE = 6;\n", 0, "cl_spliced();", "6", "a spliced include changed"},
                {"cl_b.jai", "int cl_b() { return 8; }\n", -3600, "cl_b();", "8", "an include put back with an older mtime"},
            };
            int expected_runs = 1;
            for (const auto& e : edits) {
                write(e.file, e.body, e.shift);
                eng->import_recheck_interval(1.0e6);
                eng->execute("frame();");
                check_eq(++expected_runs, runs, std::string("the module re-runs: ") + e.why);
                check_eq(std::string(e.expect), eng->execute(e.probe).to_string(), e.why);
                eng->execute("frame();");
                check_eq(expected_runs, runs, std::string("and the next frame does not: ") + e.why);
            }
            for (const char* f : {"cl_main.jai", "cl_a.jai", "cl_b.jai", "cl_s.jai"}) {
                std::filesystem::remove(dir / f);
                std::filesystem::remove(jai::foundry::jaibite_cache_beside(dir / f));
            }
        });

        test("an_import_checks_the_modules_its_module_imported_and_re_runs_a_changed_one_alone", [=, this]() {
            // while (running) { import("big.jai"); }: a pass checks big, then every module big's run imported (transitively,
            // each once however many paths reach it) with its own includes; a changed nested module re-runs on its own, checked
            // before any of it runs, and big does not; a broken edit raises a catchable error and changes nothing
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 0;
            auto write = [&](const char* name, const std::string& body) {
                const auto path = dir / name;
                put_file(path, body);
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            write("nm_big.jai", "big_ran();\nimport \"nm_mod.jai\";\nimport \"nm_side.jai\";\nint big_value() { return mod_value() + 100; }\n");
            write("nm_side.jai", "import \"nm_mod.jai\";\nint side_value() { return 7; }\n");
            write("nm_mod.jai", "mod_ran();\ninclude \"nm_inc.jai\";\nint mod_value() { return INC_BASE + 1; }\n");
            write("nm_inc.jai", "int INC_BASE = 10;\n");
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->add_include_path(dir.string());
            int big_runs = 0, mod_runs = 0;
            eng->add_function("big_ran", [&big_runs]() { ++big_runs; });
            eng->add_function("mod_ran", [&mod_runs]() { ++mod_runs; });
            eng->import_recheck_interval(1.0e6);   // every pass below is opened on purpose
            eng->execute("void frame() { import(\"nm_big.jai\"); }\nframe();");
            check_true(big_runs == 1 && mod_runs == 1, "the first frame loads the tree, the module reached twice once");
            check_eq(script_int(111), eng->execute("big_value();").as_int());
            const uint64_t warm = detail::engine_access::import_stat_count(*eng);
            eng->execute("for (int i = 0; i < 50; i++) { frame(); }");
            check_eq(warm, detail::engine_access::import_stat_count(*eng), "between passes the tree is not queried");
            auto pass = [&](const char* what) {
                eng->import_recheck_interval(1.0e6);
                const uint64_t before = detail::engine_access::import_stat_count(*eng);
                eng->execute("frame();");
                check_eq(before + 4, detail::engine_access::import_stat_count(*eng), std::string("one query per module and include of the tree: ") + what);
            };
            pass("unchanged");
            check_true(big_runs == 1 && mod_runs == 1, "an unchanged tree does not re-run");

            write("nm_mod.jai", "mod_ran();\ninclude \"nm_inc.jai\";\nint mod_value() { return INC_BASE + 2; }\n");
            eng->import_recheck_interval(1.0e6);
            eng->execute("frame();");
            check_eq(2, mod_runs, "an edited nested module re-runs");
            check_eq(1, big_runs, "its importer does not");
            check_eq(script_int(112), eng->execute("big_value();").as_int(), "the importer calls the new code");
            pass("after a nested re-run");
            check_true(big_runs == 1 && mod_runs == 2, "and the next pass runs nothing");

            write("nm_inc.jai", "int INC_BASE = 30;\n");
            eng->import_recheck_interval(1.0e6);
            eng->execute("frame();");
            check_true(big_runs == 1 && mod_runs == 3, "an edited include of a nested module re-runs that module alone");
            check_eq(script_int(132), eng->execute("big_value();").as_int());

            write("nm_mod.jai", "mod_ran();\nint mod_value() { return INC_BASE + ; }\n");
            eng->import_recheck_interval(1.0e6);
            const std::string caught = eng->execute("var m = \"\";\ntry { frame(); } catch (e) { m = e; }\nm;").to_string();
            check_false(caught.empty(), "a broken nested module raises a catchable error");
            check_true(big_runs == 1 && mod_runs == 3, "nothing of it ran");
            check_eq(script_int(132), eng->execute("big_value();").as_int(), "the program keeps the code it had");
            eng->import_recheck_interval(1.0e6);
            check_eq(std::string(""), eng->execute("var m = \"\";\ntry { frame(); } catch (e) { m = e; }\nm;").to_string(), "the failed stamp waits quietly");
            check_eq(3, mod_runs);

            write("nm_mod.jai", "mod_ran();\ninclude \"nm_inc.jai\";\nint mod_value() { return INC_BASE + 3; }\n");
            eng->import_recheck_interval(1.0e6);
            eng->execute("frame();");
            check_true(big_runs == 1 && mod_runs == 4, "the fix re-runs the nested module alone");
            check_eq(script_int(133), eng->execute("big_value();").as_int());
            for (const char* f : {"nm_big.jai", "nm_side.jai", "nm_mod.jai", "nm_inc.jai"}) {
                std::filesystem::remove(dir / f);
                std::filesystem::remove(jai::foundry::jaibite_cache_beside(dir / f));
            }
        });

        test("an_import_tree_is_what_the_module_last_run_imported", [=, this]() {
            // the tree holds the imports a module's last run executed, by the path each resolved to: a conditional import only
            // when its branch ran, a computed one as it resolved; a re-run of the module rebuilds it (an import no longer reached
            // drops out, its module stays loaded); a module also reached on its own each frame is queried once per pass
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 0;
            auto write = [&](const char* name, const std::string& body) {
                const auto path = dir / name;
                put_file(path, body);
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            const std::string a_text = "a_ran();\nif (use_y()) { import(\"tr_y.jai\"); }\nimport(\"tr_w.jai\");\nimport(z_path());\n";
            write("tr_a.jai", a_text);
            write("tr_y.jai", "y_ran();\nint y_value() { return 1; }\n");
            write("tr_w.jai", "int w_value() { return 3; }\n");
            write("tr_z.jai", "z_ran();\nint z_value() { return 2; }\n");
            write("tr_q.jai", "int q_value() { return 4; }\n");
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->add_include_path(dir.string());
            bool use_y = false;
            std::string z = "tr_z.jai";
            int a_runs = 0, y_runs = 0, z_runs = 0;
            eng->add_function("use_y", [&use_y]() { return use_y; });
            eng->add_function("z_path", [&z]() { return z; });
            eng->add_function("a_ran", [&a_runs]() { ++a_runs; });
            eng->add_function("y_ran", [&y_runs]() { ++y_runs; });
            eng->add_function("z_ran", [&z_runs]() { ++z_runs; });
            eng->import_recheck_interval(1.0e6);
            eng->execute("void tick() { import(\"tr_w.jai\"); }\nvoid frame() { import(\"tr_a.jai\"); tick(); }\nframe();");
            check_true(a_runs == 1 && z_runs == 1 && y_runs == 0, "the untaken branch imports nothing");
            auto pass = [&](uint64_t queries, const std::string& what) {
                eng->import_recheck_interval(1.0e6);
                const uint64_t before = detail::engine_access::import_stat_count(*eng);
                eng->execute("frame();");
                if (queries) { check_eq(before + queries, detail::engine_access::import_stat_count(*eng), what); }
            };
            pass(3, "a, w (in the tree and reached by tick, queried once) and z");
            write("tr_y.jai", "y_ran();\nint y_value() { return 10; }\n");
            pass(3, "an import whose branch did not run is not in the tree");
            check_eq(0, y_runs);

            z = "tr_q.jai";
            write("tr_a.jai", a_text);
            pass(0, "a re-runs and imports what the path resolves to now");
            check_eq(2, a_runs);
            write("tr_z.jai", "z_ran();\nint z_value() { return 20; }\n");
            pass(3, "the tree follows the computed path: a, w, q");
            check_eq(1, z_runs, "a module a no longer imports is not checked");
            check_eq(script_int(2), eng->execute("z_value();").as_int(), "and stays loaded as it was");

            use_y = true;
            write("tr_a.jai", a_text);
            pass(0, "a re-runs and takes the branch");
            check_true(a_runs == 3 && y_runs == 1, "the taken branch loads y");
            pass(4, "a, y, w, q");
            write("tr_y.jai", "y_ran();\nint y_value() { return 11; }\n");
            pass(0, "y changed");
            check_true(y_runs == 2 && a_runs == 3, "a module imported in a taken branch re-runs alone");
            check_eq(script_int(11), eng->execute("y_value();").as_int());

            use_y = false;
            write("tr_a.jai", a_text);
            pass(0, "a re-runs without the branch");
            check_eq(4, a_runs);
            write("tr_y.jai", "y_ran();\nint y_value() { return 12; }\n");
            pass(3, "the import its last run did not reach dropped out: a, w, q");
            check_eq(2, y_runs, "y is no longer checked");
            check_eq(script_int(11), eng->execute("y_value();").as_int(), "y stays loaded");
            for (const char* f : {"tr_a.jai", "tr_y.jai", "tr_w.jai", "tr_z.jai", "tr_q.jai"}) {
                std::filesystem::remove(dir / f);
                std::filesystem::remove(jai::foundry::jaibite_cache_beside(dir / f));
            }
        });

        test("a_missing_import_file_raises_once_then_waits_for_it_to_return", [=, this]() {
            // a module that ran before and whose file disappears raises once, then waits quietly like a failed stamp until a pass
            // finds the file again, which re-runs it through the usual checks; direct or nested alike. A file that never loaded
            // raises on every reach.
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 0;
            auto write = [&](const char* name, const std::string& body) {
                const auto path = dir / name;
                put_file(path, body);
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            for (bool nested : {false, true}) {
                const std::string label = nested ? " (nested)" : " (direct)";
                write("ms_outer.jai", "outer_ran();\nimport \"ms_mod.jai\";\n");
                write("ms_mod.jai", "mod_ran();\nint ms_value() { return 1; }\n");
                auto eng = make_engine();
                jai::stdlib::register_all(*eng);
                eng->add_include_path(dir.string());
                int outer_runs = 0, mod_runs = 0;
                eng->add_function("outer_ran", [&outer_runs]() { ++outer_runs; });
                eng->add_function("mod_ran", [&mod_runs]() { ++mod_runs; });
                eng->import_recheck_interval(1.0e6);
                eng->execute(nested ? "void frame() { import(\"ms_outer.jai\"); }" : "void frame() { import(\"ms_mod.jai\"); }");
                auto pass = [&]() {
                    eng->import_recheck_interval(1.0e6);
                    return eng->execute("var m = \"\";\ntry { frame(); } catch (e) { m = e; }\nm;").to_string();
                };
                check_eq(std::string(), pass(), "the first frame loads" + label);
                check_eq(1, mod_runs, label);
                std::filesystem::remove(dir / "ms_mod.jai");
                std::filesystem::remove(jai::foundry::jaibite_cache_beside(dir / "ms_mod.jai"));
                const std::string first = pass();
                check_true(first.find("Failed to open import file") != std::string::npos, "the pass that finds it gone raises: " + first + label);
                for (int i = 0; i < 3; ++i) { check_eq(std::string(), pass(), "then it waits quietly" + label); }
                check_eq(1, mod_runs, label);
                check_eq(script_int(1), eng->execute("ms_value();").as_int(), "the program keeps the module it loaded" + label);
                write("ms_mod.jai", "mod_ran();\nint ms_value() { return 2; }\n");
                check_eq(std::string(), pass(), "the file is back" + label);
                check_eq(2, mod_runs, "it re-runs once" + label);
                check_eq(script_int(2), eng->execute("ms_value();").as_int(), label);
                check_eq(std::string(), pass(), label);
                check_eq(2, mod_runs, "and not again" + label);
                check_eq(nested ? 1 : 0, outer_runs, "its importer does not re-run" + label);
            }
            auto eng = make_engine();
            eng->add_include_path(dir.string());
            for (int i = 0; i < 2; ++i) {
                std::string text;
                try { eng->execute("import(\"ms_never.jai\");"); } catch (const std::exception& e) { text = e.what(); }
                check_false(text.empty(), "a file that never loaded raises on every reach");
            }
            for (const char* f : {"ms_outer.jai", "ms_mod.jai"}) {
                std::filesystem::remove(dir / f);
                std::filesystem::remove(jai::foundry::jaibite_cache_beside(dir / f));
            }
        });

        test("an_edit_is_seen_inside_one_long_execute", [=, this]() {
            // interval 0: every reach checks, whichever site reaches the module, inside one execute; the default window: a loop
            // that keeps reaching the same import inside one execute sees the edit once the window has passed (the clock
            // sample moves with a sleep and with the budget check's samples in a busy loop)
            const auto dir = jai::foundry::test_temp_directory();
            const auto path = dir / "le_mod.jai";
            int tick = 0;
            auto write = [&](int v) {
                put_file(path, "le_ran();\nint le_value() { return " + std::to_string(v) + "; }\n");
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            auto setup = [&](int& runs, int& value) {
                auto eng = make_engine();
                jai::stdlib::register_all(*eng);
                eng->add_include_path(dir.string());
                eng->add_function("le_ran", [&runs]() { ++runs; });
                eng->add_function("rewrite", [&write, &value]() { write(++value); });
                eng->add_function("runs", [&runs]() { return static_cast<script_int>(runs); });
                return eng;
            };
            {
                int runs = 0, value = 1;
                write(value);
                auto eng = setup(runs, value);
                check_eq(std::string("1 2 3"), eng->execute(
                    "import_recheck_interval(0);\nimport(\"le_mod.jai\");\nstring a = to_string(le_value());\nrewrite();\n"
                    "import(\"le_mod.jai\");\nstring b = to_string(le_value());\nrewrite();\n"
                    "for (int i = 0; i < 2; i++) { import(\"le_mod.jai\"); }\na + \" \" + b + \" \" + to_string(le_value());").to_string(),
                    "interval 0: another site, and a loop's site, check on their next reach");
                check_eq(3, runs);
            }
            for (bool sleeping : {true, false}) {
                const std::string label = sleeping ? " (a sleep in the loop)" : " (a busy loop)";
                int runs = 0, value = 1;
                write(value);
                auto eng = setup(runs, value);
                eng->import_recheck_interval(0.2);
                const std::string body = sleeping ? "sleep(0.005);" : "for (int k = 0; k < 2000; k++) { spin += k; }";
                const script_int seen = eng->execute(
                    "int spin = 0;\nint frames = 0;\nimport(\"le_mod.jai\");\nrewrite();\n"
                    "while (runs() < 2 && frames < 100000) { import(\"le_mod.jai\"); " + body + " frames++; }\nle_value();").as_int();
                check_eq(script_int(2), seen, "the loop sees the edit within one execute" + label);
                check_eq(2, runs, label);
            }
            std::filesystem::remove(path);
            std::filesystem::remove(jai::foundry::jaibite_cache_beside(path));
        });

        test("a_unit_reload_hook_receives_its_previous_values_after_a_re_run", [=, this]() {
            // void hot_reload_migrate(map<string, var> previous) at a unit's top level runs once after a RE-run of that unit (never
            // the first load), after its top-level code, with every program-scope variable the unit declared as it was before the
            // re-run - a retyped one, a removed one, a re-initialized one; it is no global function: nothing can call it
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 0;
            auto write = [&](const char* name, const std::string& body) {
                const auto path = dir / name;
                put_file(path, body);
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            write("rh_mod.jai",
                "array<int> scores = [1, 2, 3];\nint legacy = 5;\nint phase = 1;\n"
                "void hot_reload_migrate(map<string, var> previous) { hook_ran(\"v1\"); }\n");
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->add_include_path(dir.string());
            std::vector<std::string> hooks;
            eng->add_function("hook_ran", [&hooks](std::string which) { hooks.push_back(which); });
            eng->import_recheck_interval(1.0e6);
            eng->execute("void frame() { import(\"rh_mod.jai\"); }\nframe();");
            check_true(hooks.empty(), "no hook on the first load");
            std::string called;
            try { eng->execute("hot_reload_migrate({});"); } catch (const std::exception& e) { called = e.what(); }
            check_true(called.find("hot_reload_migrate") != std::string::npos, "the hook is no global function: " + called);

            write("rh_mod.jai",
                "map<string, int> scores = {};\nint legacy_seen = 0;\nint phase = 2;\nint phase_seen = 0;\n"
                "void hot_reload_migrate(map<string, var> previous) {\n"
                "\thook_ran(\"v2\");\n"
                "\tarray<int> old = previous[\"scores\"];\n"
                "\tfor (int i = 0; i < old.size(); i++) { scores[to_string(i)] = old[i]; }\n"
                "\tlegacy_seen = previous[\"legacy\"];\n"
                "\tphase_seen = previous[\"phase\"] * 10 + phase;\n"
                "}\n");
            eng->import_recheck_interval(1.0e6);
            eng->execute("frame();");
            check_eq(size_t(1), hooks.size(), "a re-run runs the new source's hook once");
            check_eq(std::string("v2"), hooks.empty() ? std::string() : hooks.back());
            check_eq(script_int(3), eng->execute("scores[\"2\"];").as_int(), "a retyped global converted from its previous value");
            check_eq(script_int(5), eng->execute("legacy_seen;").as_int(), "a removed global is readable through previous");
            check_eq(script_int(12), eng->execute("phase_seen;").as_int(), "previous holds the value before the re-run; the hook runs after its top-level code");
            called.clear();
            try { eng->execute("hot_reload_migrate({});"); } catch (const std::exception& e) { called = e.what(); }
            check_false(called.empty(), "still not callable after a reload");
            called = eng->execute("[[warn_mode]]; var m = \"\";\ntry { hot_reload_migrate({}); } catch (e) { m = e; }\nm;").to_string();
            check_true(called.find("hot_reload_migrate") != std::string::npos, "at run time too: " + called);

            write("rh_mod.jai",
                "int rh_version() { return 3; }\n"
                "void hot_reload_migrate(map<string, var> previous) { hook_ran(\"v3\"); throw \"migrate failed\"; }\n");
            eng->import_recheck_interval(1.0e6);
            check_eq(std::string("migrate failed"), eng->execute("var m = \"\";\ntry { frame(); } catch (e) { m = e; }\nm;").to_string(),
                     "an error in the hook is catchable");
            check_eq(script_int(3), eng->execute("rh_version();").as_int(), "the program stays as the re-run left it");

            write("rh_mod.jai", "int rh_version() { return 4; }\n");
            eng->import_recheck_interval(1.0e6);
            eng->execute("frame();");
            check_eq(size_t(2), hooks.size(), "a run without the declaration has no hook");

            write("rh_bad.jai", "int bad_value = 1;\nvoid hot_reload_migrate(int previous) {}\n");
            std::string bad;
            try { eng->execute("import(\"rh_bad.jai\");"); } catch (const std::exception& e) { bad = e.what(); }
            check_true(bad.find("void hot_reload_migrate(map<string, var> previous)") != std::string::npos, "a wrong signature is a load error naming the shape: " + bad);
            for (const char* f : {"rh_mod.jai", "rh_bad.jai"}) {
                std::filesystem::remove(dir / f);
                std::filesystem::remove(jai::foundry::jaibite_cache_beside(dir / f));
            }
        });

        test("two_units_each_keep_their_own_reload_hook", [=, this]() {
            // each file's hook is its own: it sees only its unit's variables, and a re-run of the file by execute_file runs it too
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 0;
            auto write = [&](const char* name, const std::string& body) {
                const auto path = dir / name;
                put_file(path, body);
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            auto unit = [](const char* tag, int v) {
                return std::string("int ") + tag + "_var = " + std::to_string(v) + ";\n"
                    "void hot_reload_migrate(map<string, var> previous) { hook_ran(\"" + tag + ":\" + to_string(previous.size()) + \":\" + to_string(previous[\"" + tag + "_var\"])); }\n";
            };
            write("rh_a.jai", unit("a", 1));
            write("rh_b.jai", unit("b", 2));
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->add_include_path(dir.string());
            std::vector<std::string> hooks;
            eng->add_function("hook_ran", [&hooks](std::string which) { hooks.push_back(which); });
            eng->import_recheck_interval(1.0e6);
            eng->execute("void frame() { import(\"rh_a.jai\"); import(\"rh_b.jai\"); }\nframe();");
            check_true(hooks.empty());
            write("rh_a.jai", unit("a", 10));
            write("rh_b.jai", unit("b", 20));
            eng->import_recheck_interval(1.0e6);
            eng->execute("frame();");
            check_eq(size_t(2), hooks.size(), "each re-run module runs its own hook");
            check_true(std::find(hooks.begin(), hooks.end(), "a:1:1") != hooks.end() && std::find(hooks.begin(), hooks.end(), "b:1:2") != hooks.end(),
                       "each sees only its own unit's variables, as they were");
            {
                auto fresh = make_engine();
                jai::stdlib::register_all(*fresh);
                fresh->add_include_path(dir.string());
                fresh->add_function("hook_ran", [](std::string) {});
                fresh->execute("import \"rh_a.jai\";");
                std::string text;
                try { fresh->execute("hot_reload_migrate({});"); } catch (const std::exception& e) { text = e.what(); }
                check_false(text.empty(), "a hook loaded from the jaibite sibling is no global function either");
                auto bite = fresh->jaibite("int q_var = 1;\nvoid hot_reload_migrate(map<string, var> previous) {}\nq_var + 1;");
                check_eq(script_int(2), fresh->execute(bite).as_int());
                text.clear();
                try { fresh->execute("hot_reload_migrate({});"); } catch (const std::exception& e) { text = e.what(); }
                check_false(text.empty(), "nor one from a jaibite");
                check_eq(script_int(2), fresh->jaibite_load_bytes(bite.save_bytes()).execute().as_int(), "a saved bite round-trips with its hook kept off the program");
            }
            const auto file = dir / "rh_file.jai";
            write("rh_file.jai", unit("f", 7));
            eng->execute_file(file.string());
            check_eq(size_t(2), hooks.size(), "the first execute_file of a file is its first load");
            write("rh_file.jai", unit("f", 8));
            eng->execute_file(file.string());
            check_eq(std::string("f:1:7"), hooks.back(), "execute_file of the same file again is a re-run");
            for (const char* f : {"rh_a.jai", "rh_b.jai", "rh_file.jai"}) {
                std::filesystem::remove(dir / f);
                std::filesystem::remove(jai::foundry::jaibite_cache_beside(dir / f));
            }
        });

        test("an_import_runs_a_module_only_from_text_read_under_its_stamp", [=, this]() {
            // a save landing while a module is read (its stamp queried after the read disagrees with the one before) holds a
            // module that ran before until a pass reads it whole; a first load reads again at once. The engine's stamp source
            // stands in for the saver: while a save lands it moves the file's stamp on every query, so every read is torn
            const auto dir = jai::foundry::test_temp_directory();
            const auto path = dir / "hr_torn.jai";
            auto body = [](int v) { return "torn_ran();\nint torn_value() { return " + std::to_string(v) + "; }\n"; };
            struct landing_save {
                bool landing = false;
                int moves_left = -1;   // -1 = every query while landing
                uint64_t moved = 0;
            };
            auto stamps = [](landing_save& a_save) {
                return [&a_save](const std::string& a_path) -> std::optional<engine::file_stamp> {
                    std::error_code timeEc, sizeEc;
                    const auto mtime = std::filesystem::last_write_time(a_path, timeEc);
                    const auto size = std::filesystem::file_size(a_path, sizeEc);
                    if (timeEc || sizeEc) { return std::nullopt; }
                    engine::file_stamp result{uint64_t(mtime.time_since_epoch().count()), uint64_t(size)};
                    if (a_save.landing && a_save.moves_left != 0 && std::filesystem::path(a_path).filename() == "hr_torn.jai") {
                        result.mtime += ++a_save.moved;   // every query sees a later save than the one before
                        if (a_save.moves_left > 0) { --a_save.moves_left; }
                    }
                    return result;
                };
            };
            auto eng = make_engine();
            eng->jaibite_cache(false);
            eng->add_include_path(dir.string());
            landing_save save;
            eng->file_stamp_source(stamps(save));
            int runs = 0;
            eng->add_function("torn_ran", [&runs]() { ++runs; });
            put_file(path, body(1));
            eng->execute("import \"hr_torn.jai\";");
            check_eq(1, runs);
            put_file(path, body(2));
            std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(2));
            save.landing = true;
            bool ran = false;
            for (int i = 0; i < 3; ++i) { ran = eng->execute("import(\"hr_torn.jai\");").as_bool() || ran; }
            save.landing = false;
            check_false(ran, "a module saved during each read does not re-run");
            check_eq(1, runs);
            check_eq(script_int(1), eng->execute("torn_value();").as_int(), "the program keeps the version it ran");
            check_true(eng->execute("import(\"hr_torn.jai\");").as_bool(), "the next pass reads it whole and runs it");
            check_eq(2, runs);
            check_eq(script_int(2), eng->execute("torn_value();").as_int());

            auto first = make_engine();
            first->jaibite_cache(false);
            first->add_include_path(dir.string());
            landing_save firstSave;
            first->file_stamp_source(stamps(firstSave));
            int first_runs = 0;
            first->add_function("torn_ran", [&first_runs]() { ++first_runs; });
            firstSave.landing = true;
            firstSave.moves_left = 2;
            first->execute("import \"hr_torn.jai\";");
            check_eq(0, firstSave.moves_left, "the save landed during the first read");
            check_eq(1, first_runs, "a first load reads again at once and runs once");
            check_eq(script_int(2), first->execute("torn_value();").as_int());
            check_false(first->execute("import(\"hr_torn.jai\");").as_bool(), "the whole read it ran from is remembered");
            check_eq(1, first_runs);
            std::filesystem::remove(path);
        });

        test("import_is_a_statement_that_takes_no_scope", [=, this]() {
            // An import may stand wherever a statement may (a function, a block, a loop, a lambda) and
            // always runs at program scope; directly in a class or a namespace body it is a parse error naming the rule
            auto lib = create_temp_file("lib_anywhere.jai", "int lib_fn() { return 11; }\n");
            const char* nested[] = {
                "int f() { import \"lib_anywhere.jai\"; return lib_fn(); } f();",
                "if (true) { import \"lib_anywhere.jai\"; } lib_fn();",
                "if (true) import \"lib_anywhere.jai\"; lib_fn();",
                "var h = function() { import(\"lib_anywhere.jai\"); return lib_fn(); }; h();",
            };
            const char* members[] = {
                "class C { import \"lib_anywhere.jai\"; int v = 0; } 4;",
                "namespace ns { import \"lib_anywhere.jai\"; int g() { return 2; } } ns::g();",
            };
            for (bool use_vm : {false, true}) {
                for (const char* src : nested) {
                    auto eng = jai::foundry::make_default_engine();
                    eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    eng->add_include_path(jai::foundry::test_temp_directory().string());
                    check_eq((int64_t)11, eng->execute(src).as_int(), src);
                }
                for (const char* src : members) {
                    auto eng = jai::foundry::make_default_engine();
                    eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    std::string text;
                    try { (void)eng->execute(src); } catch (const std::exception& ex) { text = ex.what(); }
                    check_true(text.find("import is a statement, not a member: it is not allowed directly in a class or a namespace body") != std::string::npos, std::string("rejected with the rule: ") + src + " -> " + text);
                }
            }
            cleanup_temp_file(lib);
        });

        test("an_import_anywhere_runs_its_module_at_program_scope_once_per_change", [=, this]() {
            // wherever it is reached: the first reach runs the module as its own unit, a reach with the file's (mtime, size)
            // stamp unchanged costs the stamp check only, a touched file re-runs in place (functions and classes replace, live
            // instances migrate, initializers re-run); a re-run checks the module before any of it runs, and a failure is
            // catchable and leaves the program as it was
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 0;
            auto write_module = [&](const std::string& name, const std::string& body) {
                const auto path = dir / name;
                { std::ofstream(path, std::ios::binary | std::ios::trunc) << body; }
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            for (bool proofs : {true, false}) {
                auto eng = make_engine();
                jai::stdlib::register_all(*eng);
                eng->type_proofs(proofs);
                eng->add_include_path(dir.string());
                int runs = 0;
                eng->add_function("module_ran", [&runs]() { ++runs; });
                write_module("ia_mod.jai", "module_ran();\nint ia_value() { return 7; }\nint IA_LIMIT = 3;\n");
                eng->execute("void load() { import \"ia_mod.jai\"; }\nload();\nload();");
                check_eq(1, runs, "the first reach runs the module; an unchanged reach does not");
                check_eq((int64_t)7, eng->execute("ia_value();").as_int(), "its declarations are at program scope");
                check_eq((int64_t)3, eng->execute("IA_LIMIT;").as_int());
                eng->execute("for (int i = 0; i < 5; i++) { if (i % 2 == 0) { import \"ia_mod.jai\"; } }");
                check_eq(1, runs, "an if in a loop reaches it three times, the stamp holds");
                write_module("ia_mod.jai", "module_ran();\nint ia_value() { return 8; }\nint IA_LIMIT = 9;\n");
                eng->execute("for (int i = 0; i < 3; i++) { load(); }");
                check_eq(2, runs, "a touched module re-runs once");
                check_eq((int64_t)8, eng->execute("ia_value();").as_int(), "its function replaced in place");
                check_eq((int64_t)9, eng->execute("IA_LIMIT;").as_int(), "its initializer re-ran");
                write_module("ia_co.jai", "int ia_co_value() { return 21; }\n");
                check_eq((int64_t)21, eng->execute("coroutine int ia_gen() { import \"ia_co.jai\"; yield ia_co_value(); }\nint got = 0;\nfor (auto v : ia_gen()) { got = v; }\ngot;").as_int(), "from a coroutine");
                write_module("ia_lam.jai", "int ia_lam_value() { return 5; }\n");
                check_eq((int64_t)5, eng->execute("var ia_lam = []() { import \"ia_lam.jai\"; return ia_lam_value(); };\nia_lam();").as_int(), "from a lambda");
                write_module("ia_cls.jai", "class IaBox { int v = 1; int get() { return v; } }\n");
                eng->execute("void load_cls() { import \"ia_cls.jai\"; }\nload_cls();\nIaBox box = IaBox();\nbox.v = 4;");
                write_module("ia_cls.jai", "class IaBox { int v = 1; int extra = 5; int get() { return v * 100 + extra; } }\n");
                check_eq((int64_t)405, eng->execute("load_cls();\nbox.get();").as_int(), "the live instance migrated: its field kept, the new field and method arrived");
                write_module("ia_mod.jai", "module_ran();\nint ia_value() { return \"text\"; }\n");
                const std::string checked = eng->execute("var m = \"\";\ntry { load(); } catch (e) { m = e; }\nm;").to_string();
                check_true(checked.rfind("static check failed:\n", 0) == 0 && checked.find("ia_mod.jai:2:") != std::string::npos, checked);
                check_eq(2, runs, "none of the failing module ran");
                check_eq((int64_t)8, eng->execute("ia_value();").as_int(), "the program is unchanged");
                write_module("ia_mod.jai", "module_ran();\nint ia_value() { return 10; }\nthrow \"module failed\";\n");
                check_eq(std::string("module failed"), eng->execute("var m2 = \"\";\ntry { load(); } catch (e) { m2 = e; }\nm2;").to_string(), "a runtime error in the run is catchable");
                check_eq(3, runs);
                write_module("ia_mod.jai", "module_ran();\nint ia_value() { return 11; }\n");
                eng->disable(jai::filesystem);
                check_eq(std::string("Insufficient privilege: filesystem."), eng->execute("var m3 = \"\";\ntry { load(); } catch (e) { m3 = e; }\nm3;").to_string());
                eng->enable(jai::filesystem);
                eng->execute("load();");
                check_eq(4, runs);
                const std::string walled = eng->execute("array<int> xs = [1, 2, 3];\nvar m4 = \"\";\ntry { parallel_for (auto& x : xs) { import \"ia_mod.jai\"; x = x + 1; } } catch (e) { m4 = e; }\nm4;").to_string();
                check_true(walled.find("include/import is not allowed in a parallel body") != std::string::npos, walled);
                // the dev-reload idiom: a module list included from the loop's function; only a touched module re-runs, and a
                // touched module that fails its check leaves everything as it was
                int ran_a = 0, ran_b = 0;
                eng->add_function("ran_a", [&ran_a]() { ++ran_a; });
                eng->add_function("ran_b", [&ran_b]() { ++ran_b; });
                write_module("ia_list_a.jai", "ran_a();\nint ia_a() { return 1; }\n");
                write_module("ia_list_b.jai", "ran_b();\nint ia_b() { return 2; }\n");
                write_module("ia_list.jai", "import \"ia_list_a.jai\";\nimport \"ia_list_b.jai\";\n");
                eng->execute("void tick() { include \"ia_list.jai\"; }\nfor (int i = 0; i < 4; i++) { tick(); }");
                check_eq(std::string("1 1"), std::to_string(ran_a) + " " + std::to_string(ran_b));
                write_module("ia_list_a.jai", "ran_a();\nint ia_a() { return 10; }\n");
                eng->execute("for (int i = 0; i < 4; i++) { tick(); }");
                check_eq(std::string("2 1"), std::to_string(ran_a) + " " + std::to_string(ran_b), "only the touched module re-ran");
                check_eq((int64_t)12, eng->execute("ia_a() + ia_b();").as_int());
                write_module("ia_list_a.jai", "ran_a();\nint ia_a() { return 1.5 + \"x\"; }\nint ia_a2() { return 3; }\n");
                const std::string list_fail = eng->execute("var lf = \"\";\nfor (int i = 0; i < 2; i++) { try { tick(); } catch (e) { lf = e; } }\nlf;").to_string();
                check_true(list_fail.find("ia_list_a.jai:2:") != std::string::npos, list_fail);
                check_eq(std::string("2 1"), std::to_string(ran_a) + " " + std::to_string(ran_b), "the failing module ran nothing");
                check_eq((int64_t)12, eng->execute("ia_a() + ia_b();").as_int(), "the program kept the code it had");
                // the checker gathers a nested literal import as a unit of the program
                write_module("ia_seven.jai", "int ia_seven() { return 7; }\n");
                auto strict = make_engine();
                strict->type_proofs(proofs);
                strict->add_include_path(dir.string());
                check_eq((int64_t)7, strict->execute("void load_seven() { import \"ia_seven.jai\"; }\nload_seven();\nia_seven();").as_int());
            }
            for (const char* f : {"ia_mod.jai", "ia_co.jai", "ia_lam.jai", "ia_cls.jai", "ia_seven.jai", "ia_list.jai", "ia_list_a.jai", "ia_list_b.jai"}) { cleanup_temp_file((dir / f).string()); }
        });

        test("the_ladder_answers_exactly_while_it_remembers_its_answers", [=, this]() {
            // resolution is remembered per asking folder, and a file appearing on an earlier rung or the answer disappearing
            // is seen at once
            const auto base = jai::foundry::test_temp_directory() / "ladder_memo";
            const auto asking = base / "asking";
            const auto lib = base / "lib";
            std::filesystem::create_directories(asking);
            std::filesystem::create_directories(lib);
            { std::ofstream(lib / "lm.jai") << "1;"; }
            std::filesystem::remove(asking / "lm.jai");
            for (bool use_vm : {false, true}) {
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                eng->add_include_path(lib.string());
                const std::string root = (asking / "root.jai").string();
                auto ask = [&]() { return eng->execute_file_source(root, "resolve_path(\"lm.jai\");").to_string(); };
                const std::string from_lib = ask();
                check_true(from_lib.find("ladder_memo/lib/lm.jai") != std::string::npos, from_lib);
                check_eq(from_lib, ask(), "remembered");
                { std::ofstream(asking / "lm.jai") << "2;"; }
                const std::string sibling = ask();
                check_true(sibling.find("ladder_memo/asking/lm.jai") != std::string::npos, "an earlier rung's new file wins: " + sibling);
                std::filesystem::remove(asking / "lm.jai");
                check_eq(from_lib, ask(), "the vanished answer falls back down the ladder");
                std::filesystem::remove(lib / "lm.jai");
                check_eq(std::string(), ask(), "no rung holds it");
                { std::ofstream(lib / "lm.jai") << "1;"; }
            }
            std::filesystem::remove_all(base);
        });

        test("an_import_says_whether_its_module_ran", [=, this]() {
            // import(...) is an expression: true when a module (re)ran, false when it had nothing to do; the statement form
            // yields nothing; a group is true when any of its modules ran
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 300;
            auto write_module = [&](const std::string& name, const std::string& body) {
                const auto path = dir / name;
                { std::ofstream(path, std::ios::binary | std::ios::trunc) << body; }
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            for (bool use_vm : {false, true}) {
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(*eng);
                eng->import_recheck_interval(0);
                eng->add_include_path(dir.string());
                write_module("ir_a.jai", "int IR_A = 1;\n");
                write_module("ir_b.jai", "int IR_B = 1;\n");
                check_true(eng->execute("import(\"ir_a.jai\");").as_bool(), "the first reach runs the module");
                check_false(eng->execute("import(\"ir_a.jai\");").as_bool(), "an unchanged module does not run");
                write_module("ir_a.jai", "int IR_A = 2;\n");
                check_eq(std::string("true|2"), eng->execute("bool ran = import(\"ir_a.jai\");\nto_string(ran) + \"|\" + to_string(IR_A);").to_string());
                check_eq(std::string("true|false"), eng->execute("bool g1 = import(\"ir_a.jai\", \"ir_b.jai\");\nbool g2 = import([\"ir_a.jai\", \"ir_b.jai\"]);\nto_string(g1) + \"|\" + to_string(g2);").to_string(),
                         "a group is true when any module ran (ir_b's first run), then false");
                check_eq(std::string("skip"), eng->execute("string r = \"skip\";\nif (import(\"ir_b.jai\")) { r = \"ran\"; }\nr;").to_string());
                std::string bad;
                try { eng->execute("var p = 5;\nimport(p);"); } catch (const std::exception& ex) { bad = ex.what(); }
                check_true(bad.find("import(...) takes paths: strings or arrays of strings") != std::string::npos, bad);
                std::string unknown;
                try { eng->execute("import(\"ir_a.jai\", jai::forse);"); } catch (const std::exception& ex) { unknown = ex.what(); }
                check_true(unknown.find("unknown import directive 'jai::forse' (the directive is jai::force)") != std::string::npos, unknown);
            }
            for (const char* f : {"ir_a.jai", "ir_b.jai"}) { cleanup_temp_file((dir / f).string()); }
        });

        test("the_running_root_file_counts_as_imported", [=, this]() {
            // a module that imports the root file meets a cycle's back edge: the root's top level runs once, and the module
            // defines what the root calls after its import
            auto root = create_temp_file("rc_root.jai", "trace.push(\"root\");\nimport \"rc_mod.jai\";\nf();\n");
            auto mod = create_temp_file("rc_mod.jai", "trace.push(\"mod\");\nimport \"rc_root.jai\";\nint f() { return 2; }\n");
            auto eng = make_engine();
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            eng->execute("array<string> trace;");
            check_eq((int64_t)2, eng->execute_file(root).as_int());
            check_eq(std::string("root,mod"), eng->execute("trace.join(\",\");").to_string());
            check_eq((int64_t)2, eng->execute("import \"rc_root.jai\";\nf();").as_int(), "once the root has finished, an import of its file runs it as a module");
            check_eq(std::string("root,mod,root"), eng->execute("trace.join(\",\");").to_string());
            cleanup_temp_file(root);
            cleanup_temp_file(mod);
        });

        test("one_check_pass_covers_every_import_a_frame_reaches", [=, this]() {
            // the engine-wide pass: the first import reached after the window opens a pass, every import reached in it stats its
            // modules once, the pass closes when a site reaches a path it already checked (the frame loop came around) or at the
            // next top-level execute, and between passes an import does no file work - a frame checks every import or none
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 500;
            auto write_module = [&](const std::string& name, const std::string& body) {
                const auto path = dir / name;
                { std::ofstream(path, std::ios::binary | std::ios::trunc) << body; }
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            for (bool use_vm : {false, true}) {
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(*eng);
                check_near(0.2, eng->import_recheck_interval(), 1e-12);
                eng->add_include_path(dir.string());
                std::map<std::string, int> runs;
                eng->add_function("pass_ran", [&runs](std::string m) { ++runs[m]; });
                eng->import_recheck_interval(1.0e6);   // nothing expires by itself: every pass below is opened on purpose
                write_module("pp_one.jai", "pass_ran(\"one\");\n");
                write_module("pp_two.jai", "pass_ran(\"two\");\n");
                write_module("pp_nest.jai", "import \"pp_one.jai\";\npass_ran(\"nest\");\n");
                eng->execute("void frame() { import \"pp_one.jai\"; import \"pp_nest.jai\"; import \"pp_two.jai\"; }\nframe();");
                check_true(runs["one"] == 1 && runs["two"] == 1 && runs["nest"] == 1, "the first frame loads every module");
                const uint64_t warm = detail::engine_access::import_stat_count(*eng);
                eng->execute("for (int i = 0; i < 100; i++) { frame(); }");
                check_eq(warm, detail::engine_access::import_stat_count(*eng), "between passes an import does no file work (0 queries)");
                write_module("pp_two.jai", "pass_ran(\"two\");\n");
                write_module("pp_nest.jai", "import \"pp_one.jai\";\npass_ran(\"nest\");\n");
                eng->execute("frame();");
                check_eq(1, runs["two"], "a touch waits for the next pass");
                eng->import_recheck_interval(1.0e6);   // the window starts over: the next import reached opens a pass
                const uint64_t before = detail::engine_access::import_stat_count(*eng);
                eng->execute("for (int i = 0; i < 5; i++) { frame(); }");
                check_true(runs["nest"] == 2 && runs["two"] == 2 && runs["one"] == 1, "one frame took both changes (the nested import did not end the pass)");
                check_eq(before + 3, detail::engine_access::import_stat_count(*eng), "one query per module in the pass, none in the frames after it");
                write_module("pp_one.jai", "pass_ran(\"one\");\n");
                eng->execute("frame();");
                check_eq(1, runs["one"], "still inside the window");
                eng->execute("import(\"pp_one.jai\", jai::force);");
                check_eq(2, runs["one"], "jai::force opens a pass now");
                eng->execute("import(\"pp_one.jai\", jai::force);");
                check_eq(2, runs["one"], "an unchanged module does not run on a forced check");
                // a computed site remembers the paths it ran: a module it never ran loads at once, pass or not
                write_module("pp_late.jai", "pass_ran(\"late\");\n");
                eng->execute("array<string> MODS = [\"pp_one.jai\", \"pp_two.jai\"];\nvoid scan() { for (auto m : MODS) { import(m); } }\nscan();\nMODS.push(\"pp_late.jai\");\nscan();");
                check_eq(1, runs["late"], "a new path in the list loads on its first reach");
                const uint64_t steady = detail::engine_access::import_stat_count(*eng);
                eng->execute("for (int i = 0; i < 50; i++) { scan(); }");
                check_eq(steady, detail::engine_access::import_stat_count(*eng), "the computed site skips between passes too");
                // 0: every reach checks
                eng->import_recheck_interval(0);
                const uint64_t zero = detail::engine_access::import_stat_count(*eng);
                eng->execute("for (int i = 0; i < 4; i++) { import \"pp_one.jai\"; }");
                check_eq(zero + 4, detail::engine_access::import_stat_count(*eng), "0 checks on every reach");
                check_near(0.0, eng->execute("import_recheck_interval();").as_float(), 1e-12);
                eng->disable(jai::filesystem);
                check_eq(std::string("Insufficient privilege: filesystem."), eng->execute("var m = \"\";\ntry { frame(); } catch (e) { m = e; }\nm;").to_string(), "a disabled privilege is never skipped");
                eng->enable(jai::filesystem);
            }
            for (const char* f : {"pp_one.jai", "pp_two.jai", "pp_nest.jai", "pp_late.jai"}) { cleanup_temp_file((dir / f).string()); }
        });

        test("the_import_window_runs_on_the_engines_clock_sample", [=, this]() {
            // the window compares the engine's clock sample (the host entry, the budget check's samples, a time:: read of the
            // system clock), never the clock: an idle import reads no clock and queries no file, real time passing inside a run
            // opens no pass until something samples, and a host time source moves neither the window nor the budget
            const auto dir = jai::foundry::test_temp_directory();
            { std::ofstream(dir / "cs_a.jai", std::ios::binary | std::ios::trunc) << "int CS_A = 1;\n"; }
            std::string lines;
            for (int i = 0; i < 40; ++i) { lines += "import \"cs_a.jai\";\n"; }
            for (bool use_vm : {false, true}) {
                for (bool host_clock : {false, true}) {
                    auto eng = jai::foundry::make_default_engine();
                    eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    jai::stdlib::register_all(*eng);
                    eng->add_include_path(dir.string());
                    auto fake = std::make_shared<std::chrono::nanoseconds>(std::chrono::seconds(100));
                    if (host_clock) { eng->time().source([fake] { return *fake += std::chrono::seconds(1000); }, {}, {}); }
                    engine* raw = eng.get();
                    eng->add_function("stats", [raw]() { return static_cast<script_int>(detail::engine_access::import_stat_count(*raw)); });
                    eng->add_function("wait_ms", [](script_int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
                    const std::string be = std::string(use_vm ? " (vm" : " (interpreter") + (host_clock ? ", host clock)" : ")");
                    eng->import_recheck_interval(1.0e6);
                    eng->execute("void poll() { import \"cs_a.jai\"; }\npoll();");
                    eng->execute(lines);
                    const uint64_t stats = detail::engine_access::import_stat_count(*eng);
                    const uint64_t before_empty = detail::engine_access::clock_sample_count(*eng);
                    eng->execute("0;");
                    const uint64_t entry = detail::engine_access::clock_sample_count(*eng) - before_empty;
                    const uint64_t before_idle = detail::engine_access::clock_sample_count(*eng);
                    eng->execute(lines);
                    check_eq(entry, detail::engine_access::clock_sample_count(*eng) - before_idle, "40 idle imports read no clock beyond the host entry's" + be);
                    check_eq(stats, detail::engine_access::import_stat_count(*eng), "and query no file" + be);
                    eng->import_recheck_interval(0.001);
                    check_eq(std::string(host_clock ? "1 0 0" : "1 0 1"), eng->execute(
                        "int s0 = stats();\npoll();\nint s1 = stats();\nwait_ms(5);\npoll();\nint s2 = stats();\ntime::now();\npoll();\nint s3 = stats();\n"
                        "to_string(s1 - s0) + \" \" + to_string(s2 - s1) + \" \" + to_string(s3 - s2);").to_string(),
                        "the entry opens a pass; the frame coming around after the window opens none until a system-clock read samples" + be);
                    eng->execution_budget(jai::foundry::k_test_budget_seconds);
                    check_eq(std::string("5000"), eng->execute("int n = 0;\nfor (int i = 0; i < 5000; i++) { time::now(); n++; }\nn;").to_string(),
                             "a host clock racing ahead never trips the budget" + be);
                }
            }
            cleanup_temp_file((dir / "cs_a.jai").string());
        });

        test("a_group_import_checks_every_changed_module_before_any_runs", [=, this]() {
            // import("a", "b", ...) / import(array): one throttle decision, every changed module parsed and checked (knowing each
            // other's names) before any runs, a failure anywhere is one error that applies nothing, the changes then wait until
            // a failing file changes again, and the modules run in the listed order
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 700;
            auto write_module = [&](const std::string& name, const std::string& body) {
                const auto path = dir / name;
                { std::ofstream(path, std::ios::binary | std::ios::trunc) << body; }
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            for (bool use_vm : {false, true}) {
                for (bool proofs : {true, false}) {
                    auto eng = jai::foundry::make_default_engine();
                    eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(*eng);
                    if (!proofs) { eng->type_proofs(false); }
                    eng->add_include_path(dir.string());
                    eng->import_recheck_interval(0);
                    write_module("gi_a.jai", "int GA = 1;\nint ga_one() { return 1; }\n");
                    write_module("gi_b.jai", "int GB = GA * 10 + ga_one();\n");
                    check_eq(std::string("true|11"), eng->execute("array<string> MODS = [\"gi_a.jai\", \"gi_b.jai\"];\nbool r = import(MODS);\nto_string(r) + \"|\" + to_string(GB);").to_string());
                    write_module("gi_a.jai", "int GA = 2;\nint ga_one() { return 1; }\nint ga_two() { return 2; }\n");
                    write_module("gi_b.jai", "int GB = \"broken\";\n");
                    std::string failed = eng->execute("string m = \"\";\ntry { import(MODS); } catch (e) { m = e; }\nm;").to_string();
                    check_true(failed.find("gi_b.jai") != std::string::npos, "one catchable error names the failing module: " + failed);
                    check_eq(std::string("1|11"), eng->execute("to_string(GA) + \"|\" + to_string(GB);").to_string(), "nothing applied: gi_a's good change waits");
                    check_eq(std::string("false|1"), eng->execute("bool again = import(MODS);\nto_string(again) + \"|\" + to_string(GA);").to_string(), "the group waits quietly until the failing file changes");
                    write_module("gi_b.jai", "int GB = GA * 10 + ga_two();\n");
                    check_eq(std::string("true|2|22"), eng->execute("bool fixed = import(\"gi_a.jai\", \"gi_b.jai\");\nto_string(fixed) + \"|\" + to_string(GA) + \"|\" + to_string(GB);").to_string(),
                             "the fix applies both in order, gi_b checked against gi_a's new function");
                    write_module("gi_a.jai", "int GA = \"x\";\n");
                    write_module("gi_b.jai", "int GB = \"y\";\n");
                    std::string both = eng->execute("string m = \"\";\ntry { import(MODS); } catch (e) { m = e; }\nm;").to_string();
                    check_true(both.find("gi_a.jai") != std::string::npos && both.find("gi_b.jai") != std::string::npos, "two failures, one error: " + both);
                    check_eq(std::string("2|22"), eng->execute("to_string(GA) + \"|\" + to_string(GB);").to_string());
                }
            }
            for (const char* f : {"gi_a.jai", "gi_b.jai"}) { cleanup_temp_file((dir / f).string()); }
        });

        test("a_reload_that_breaks_code_that_is_not_changing_is_rejected", [=, this]() {
            // a reload re-checks every loaded unit against the declarations it brings (the root file, a string that declared
            // functions): one that would break is the reload's error, nothing applies and the edit waits; a compatible edit
            // applies and migrates the class's globals and live locals
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 1100;
            auto write_module = [&](const std::string& name, const std::string& body) {
                const auto path = dir / name;
                { std::ofstream(path, std::ios::binary | std::ios::trunc) << body; }
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            const std::string root = (dir / "dr_root.jai").generic_string();
            for (bool use_vm : {false, true}) {
                const std::string be = use_vm ? " (vm)" : " (interpreter)";
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(*eng);
                eng->add_include_path(dir.string());
                eng->import_recheck_interval(0);
                write_module("dr_c.jai", "class C { int a = 1; int get() { return a + 100; } }\n");
                eng->execute_file_source(root,
                    "import(\"dr_c.jai\");\nC g;\nint across() { C loc; loc.a = 5; import(\"dr_c.jai\"); return loc.get(); }\n"
                    "int ga() { C loc; return loc.a + g.a; }\n");
                eng->execute("int s_read() { C sc; return sc.a; }");
                write_module("dr_c.jai", "class C { int a = 1; int b = 7; int get() { return a + b + 100; } }\n");
                check_eq(std::string("112|108"), eng->execute("to_string(across()) + \"|\" + to_string(g.get());").to_string(),
                         "a compatible edit applies; the live local and the global migrate" + be);
                write_module("dr_c.jai", "class C { int b = 7; int get() { return b; } }\n");
                const std::string failed = eng->execute("string m = \"\";\ntry { import(\"dr_c.jai\"); } catch (e) { m = e; }\nm;").to_string();
                check_true(failed.rfind("static check failed:\n", 0) == 0, "one check error" + be + ": " + failed);
                check_true(failed.find(root + ":3:27: error: no member 'a' in class 'C'") != std::string::npos, "the root file's uses are named" + be + ": " + failed);
                check_true(failed.find(root + ":4:30: error: no member 'a' in class 'C'") != std::string::npos, "4:30" + be + ": " + failed);
                check_true(failed.find(root + ":4:36: error: no member 'a' in class 'C'") != std::string::npos, "4:36" + be + ": " + failed);
                check_true(failed.find("<script>:1:32: error: no member 'a' in class 'C'") != std::string::npos, "and the string's" + be + ": " + failed);
                check_eq(std::string("108|2|1"), eng->execute("to_string(g.get()) + \"|\" + to_string(ga()) + \"|\" + to_string(s_read());").to_string(),
                         "nothing applied: the old class stands" + be);
                check_eq(std::string("false"), eng->execute("to_string(import(\"dr_c.jai\"));").to_string(), "the edit waits until a file it involved changes" + be);
                write_module("dr_c.jai", "class C { int a = 1; int b = 7; int c = 3; int get() { return a + c; } }\n");
                check_eq(std::string("true|4|2"), eng->execute("bool r = import(\"dr_c.jai\");\nto_string(r) + \"|\" + to_string(g.get()) + \"|\" + to_string(ga());").to_string(),
                         "the fixed edit applies" + be);
            }
            for (const char* f : {"dr_c.jai", "dr_c.jaibite"}) { cleanup_temp_file((dir / f).string()); }
        });

        test("a_reload_checks_references_to_the_class_it_reshapes", [=, this]() {
            // a dependent's code runs only after the reload has migrated every live instance, so a reference to the reshaped class
            // reads the new shape: removing a field it uses is the reload's error, whether the reload is
            // reached inside the execute that loaded the modules (a game loop's frame) or in a later one
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 1200;
            auto write_module = [&](const std::string& name, const std::string& body) {
                const auto path = dir / name;
                { std::ofstream(path, std::ios::binary | std::ios::trunc) << body; }
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            const std::string uses = (dir / "rs_use.jai").string();
            for (bool use_vm : {false, true}) {
                for (bool same_execute : {true, false}) {
                    const std::string be = std::string(use_vm ? " (vm" : " (interpreter") + (same_execute ? ", same execute)" : ", later execute)");
                    auto eng = jai::foundry::make_default_engine();
                    eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    jai::stdlib::register_all(*eng);
                    eng->add_include_path(dir.string());
                    eng->import_recheck_interval(0);
                    eng->add_function("reshape", [&]() { write_module("rs_p.jai", "class P { int a = 1; }\n"); });
                    write_module("rs_p.jai", "class P { int a = 1; int b = 2; }\n");
                    write_module("rs_use.jai", "int by_ref(P& p) { return p.b; }\nint by_const_ref(const P& p) { return p.b; }\n");
                    const std::string reload = "string m = \"accepted\";\ntry { import(\"rs_p.jai\"); } catch (e) { m = e; }\n";
                    const std::string load = "import(\"rs_p.jai\");\nimport(\"rs_use.jai\");\nP SHARED;\n";
                    std::string failed;
                    if (same_execute) {
                        failed = eng->execute(load + "reshape();\n" + reload + "m;").to_string();
                    } else {
                        eng->execute(load);
                        eng->execute("reshape();");
                        failed = eng->execute(reload + "m;").to_string();
                    }
                    check_true(failed.rfind("static check failed:\n", 0) == 0, "the reload is rejected" + be + ": " + failed);
                    check_true(failed.find(uses + ":1:29: error: no member 'b' in class 'P'") != std::string::npos, "the reference parameter's use" + be + ": " + failed);
                    check_true(failed.find(uses + ":2:41: error: no member 'b' in class 'P'") != std::string::npos, "the const reference's use" + be + ": " + failed);
                    check_eq((int64_t)4, eng->execute("by_ref(SHARED) + by_const_ref(SHARED);").as_int(), "nothing applied: the old class stands" + be);
                }
            }
            for (const char* f : {"rs_p.jai", "rs_use.jai", "rs_p.jaibite", "rs_use.jaibite"}) { cleanup_temp_file((dir / f).string()); }
        });

        test("a_reload_is_checked_against_the_pending_edits_of_the_modules_it_would_break", [=, this]() {
            // a dependent in a sibling module: unchanged, it rejects the reload until it changes (the edit then tries again); with
            // its file already edited, the reload is checked against its new text and passes, whichever import is reached first.
            // Every module still re-runs at its own import, as before; a group import runs its modules in the listed order
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 1300;
            auto write_module = [&](const std::string& name, const std::string& body) {
                const auto path = dir / name;
                { std::ofstream(path, std::ios::binary | std::ios::trunc) << body; }
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            const auto module_x = [](const std::string& fields) { return "class X { " + fields + " }\nLOG = LOG + \"x\";\n"; };
            const auto module_y = [](const std::string& field) { return "X yx;\nint y_read() { return yx." + field + "; }\nLOG = LOG + \"y\";\n"; };
            for (bool use_vm : {false, true}) {
                const std::string be = use_vm ? " (vm)" : " (interpreter)";
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(*eng);
                eng->add_include_path(dir.string());
                eng->import_recheck_interval(0);
                write_module("dp_x.jai", module_x("int a = 1; int b = 2;"));
                write_module("dp_y.jai", module_y("a"));
                eng->execute(
                    "string LOG = \"\";\nimport(\"dp_x.jai\");\nimport(\"dp_y.jai\");\n"
                    "void frame() { import(\"dp_x.jai\"); LOG = LOG + \"|\"; import(\"dp_y.jai\"); }\n"
                    "void frame_yx() { import(\"dp_y.jai\"); LOG = LOG + \"|\"; import(\"dp_x.jai\"); }\n"
                    "string attempt(bool yx) { LOG = \"\"; string m = \"\"; try { if (yx) { frame_yx(); } else { frame(); } } catch (e) { m = e; } return m + \"#\" + LOG; }\n");
                const auto attempt = [&](bool yx) { return eng->execute(yx ? "attempt(true);" : "attempt(false);").to_string(); };
                const auto y_read = [&]() { return eng->execute("y_read();").as_int(); };
                check_eq(std::string("xy"), eng->execute("LOG;").to_string());

                write_module("dp_x.jai", module_x("int b = 2; int c = 3;"));
                write_module("dp_y.jai", module_y("b"));
                check_eq(std::string("#x|y"), attempt(false), "the dependent saved before the reload: each runs at its own import" + be);
                check_eq((int64_t)2, y_read());

                write_module("dp_x.jai", module_x("int c = 3;"));
                const std::string rejected = attempt(false);
                check_true(rejected.rfind("static check failed:\n", 0) == 0 && rejected.find("dp_y.jai:2:26: error: no member 'b' in class 'X'") != std::string::npos,
                           "an unchanged dependent rejects the reload" + be + ": " + rejected);
                check_true(rejected.size() > 1 && rejected.substr(rejected.size() - 1) == "#", "nothing ran" + be + ": " + rejected);
                check_eq((int64_t)2, y_read(), "the old code stands" + be);
                check_eq(std::string("#|"), attempt(false), "the reload waits" + be);
                write_module("dp_y.jai", module_y("c"));
                check_eq(std::string("#x|y"), attempt(false), "editing the dependent lets the waiting reload try again" + be);
                check_eq((int64_t)3, y_read());

                write_module("dp_x.jai", module_x("int d = 4;"));
                write_module("dp_y.jai", module_y("d"));
                check_eq(std::string("#y|x"), attempt(true), "the dependent's import reached first: each still runs at its own import" + be);
                check_eq((int64_t)4, y_read());

                write_module("dp_x.jai", module_x("int e = 5;"));
                write_module("dp_y.jai", module_y("e"));
                check_eq(std::string("xy"), eng->execute("LOG = \"\";\nimport(\"dp_x.jai\", \"dp_y.jai\");\nLOG;").to_string(), "a group import runs in the listed order" + be);
                write_module("dp_x.jai", module_x("int f = 6;"));
                const std::string group = eng->execute("LOG = \"\";\nstring m = \"\";\ntry { import(\"dp_y.jai\", \"dp_x.jai\"); } catch (e) { m = e; }\nm + \"#\" + LOG;").to_string();
                check_true(group.find("dp_y.jai:2:26: error: no member 'e' in class 'X'") != std::string::npos && group.substr(group.size() - 1) == "#",
                           "a group whose change breaks an unchanged dependent applies nothing" + be + ": " + group);
                write_module("dp_y.jai", module_y("f"));
                check_eq(std::string("yx"), eng->execute("LOG = \"\";\nimport(\"dp_y.jai\", \"dp_x.jai\");\nLOG;").to_string(), "and applies in the listed order once fixed" + be);
                check_eq((int64_t)6, y_read());
            }
            for (const char* f : {"dp_x.jai", "dp_y.jai", "dp_x.jaibite", "dp_y.jaibite"}) { cleanup_temp_file((dir / f).string()); }
        });

        test("a_reload_of_a_sibling_leaves_its_dependents_checked_and_proven_as_a_fresh_program_would", [=, this]() {
            // dr_lvl.jai's function is typed by its declaration in dr_use.jai's proofs: after a reload changes that declaration the
            // dependent runs, and is proven, exactly as in an engine that met the new text first
            const auto dir = context_dir("reload");
            int tick = 1400;
            auto write_module = [&](const std::string& name, const std::string& body) {
                const auto path = dir / name;
                { std::ofstream(path, std::ios::binary | std::ios::trunc) << body; }
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            const std::string use = "int dr_twice() { int n = 0; n = dr_level(); return n * 2; }\nvar dr_read() { var v = dr_level(); return v; }\n";
            const std::string root = "import(\"dr_use.jai\");\nimport(\"dr_lvl.jai\");\n";
            const auto probe = [&](engine& eng) {
                std::string manifests;
                eng.disassembly_sink([&](const std::string&, const std::string& manifest) { manifests += manifest; });
                const std::string value = eng.execute("to_string(dr_twice()) + \"|\" + to_string(dr_read());").to_string();
                eng.disassembly_sink({});
                return value + "\n" + manifests;
            };
            const auto engine_in = [&]() {
                auto eng = make_engine();
                jai::stdlib::register_all(*eng);
                eng->add_include_path(dir.string());
                eng->jaibite_cache(false);
                return eng;
            };
            write_module("dr_use.jai", use);
            write_module("dr_lvl.jai", "int dr_level() { return 3; }\n");
            auto reloaded = engine_in();
            reloaded->execute(root);
            write_module("dr_lvl.jai", "float dr_level() { return 3.5; }\n");
            reloaded->execute(root);
            auto fresh = engine_in();
            fresh->execute(root);
            check_eq(probe(*fresh), probe(*reloaded), "the dependent runs and compiles as in a fresh engine");
            std::filesystem::remove_all(dir);
        });

        test("a_reload_whose_class_definition_would_fail_applies_nothing", [=, this]() {
            // the errors a class or namespace definition raises (a base nothing knows, override with nothing to override, a method
            // redefining a base method without override, a namespace reopening that redefines a function) are the check's: a group
            // with one applies none of its changes, instead of running the modules before the bad declaration and the declarations
            // above it
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 900;
            auto write_module = [&](const std::string& name, const std::string& body) {
                const auto path = dir / name;
                { std::ofstream(path, std::ios::binary | std::ios::trunc) << body; }
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            const std::vector<std::pair<std::string, std::string>> broken = {
                {"class Base { void m() { } }\nclass Kid : Base { void m() { } }\n", "method 'm' redefines 'Base::m': mark it 'override'"},
                {"class Kid { void m() override { } }\n", "method 'm' marked as override but no base class method found"},
                {"class Kid : NoSuchBase { int k = 1; }\n", "Base class 'NoSuchBase' not found"},
                {"namespace dn { int f() { return 1; } }\nnamespace dn { int f() { return 2; } }\n", "'f' is already defined with parameters () (declared at "},
            };
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->add_include_path(dir.string());
            eng->import_recheck_interval(0);
            write_module("cd_a.jai", "int CA = 1;\n");
            write_module("cd_b.jai", "int cd_b_fn() { return 1; }\n");
            check_eq(std::string("1|1"), eng->execute("array<string> CD = [\"cd_a.jai\", \"cd_b.jai\"];\nimport(CD);\nto_string(CA) + \"|\" + to_string(cd_b_fn());").to_string());
            int version = 1;
            for (const auto& [body, text] : broken) {
                ++version;
                write_module("cd_a.jai", "int CA = " + std::to_string(version) + ";\n");
                write_module("cd_b.jai", "int cd_b_fn() { return " + std::to_string(version) + "; }\n" + body + "int cd_b_after() { return 0; }\n");
                const std::string failed = eng->execute("string m = \"\";\ntry { import(CD); } catch (e) { m = e; }\nm;").to_string();
                check_true(failed.rfind("static check failed:\n", 0) == 0 && failed.find("cd_b.jai") != std::string::npos && failed.find(text) != std::string::npos,
                           "the check names the definition error: " + failed);
                check_eq(std::string("1|1"), eng->execute("to_string(CA) + \"|\" + to_string(cd_b_fn());").to_string(), "nothing applied for: " + text);
            }
            write_module("cd_b.jai", "int cd_b_fn() { return 9; }\nclass Base { void m() { } }\nclass Kid : Base { void m() override { } }\n");
            check_eq(std::string("5|9"), eng->execute("import(CD);\nto_string(CA) + \"|\" + to_string(cd_b_fn());").to_string(), "the fixed group applies");
            for (const char* f : {"cd_a.jai", "cd_b.jai"}) { cleanup_temp_file((dir / f).string()); }
        });

        test("a_root_file_lists_its_check_errors_under_its_path", [=, this]() {
            // the root file's listing names the file like an included or imported one does; source text stays "<script>"
            const std::string path = (jai::foundry::test_temp_directory() / "root_listing.jai").generic_string();
            for (bool use_vm : {false, true}) {
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                std::string from_file, from_text;
                try { eng->execute_file_source(path, "int x = \"s\";\n"); } catch (const std::exception& ex) { from_file = ex.what(); }
                try { eng->execute("int y = \"s\";\n"); } catch (const std::exception& ex) { from_text = ex.what(); }
                check_true(from_file.find(path + ":1:5: error: cannot initialize 'x'") != std::string::npos, from_file);
                check_true(from_file.find("<script>:1:5") == std::string::npos, from_file);
                check_true(from_text.find("<script>:1:5: error: cannot initialize 'y'") != std::string::npos, from_text);
            }
        });

        test("a_module_rerun_keeps_a_variable_declared_without_an_initializer", [=, this]() {
            // A same-unit re-run resets a variable only when its declaration assigns; `T x;` keeps the
            // value (the first run defaults it), unless its declared type changed, which resets it to the new type's default
            const auto dir = jai::foundry::test_temp_directory();
            int tick = 100;
            auto write_module = [&](const std::string& name, const std::string& body) {
                const auto path = dir / name;
                { std::ofstream(path, std::ios::binary | std::ios::trunc) << body; }
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            for (bool proofs : {true, false}) {
                auto eng = make_engine();
                jai::stdlib::register_all(*eng);
                eng->type_proofs(proofs);
                eng->add_include_path(dir.string());
                const std::string body = "int kills;\nint resets = 0;\narray<int> pack;\nclass KeepBox { int n = 0; }\nKeepBox held;\nvar loose;\n"
                                         "void kill() { kills += 1; resets += 1; pack.push(kills); held.n = kills; loose = kills; }\n";
                write_module("ia_state.jai", body);
                eng->execute("void load_state() { import \"ia_state.jai\"; }\nload_state();");
                check_eq(std::string("0 0 0 0 null"), eng->execute("to_string(kills) + \" \" + to_string(resets) + \" \" + to_string(pack.size()) + \" \" + to_string(held.n) + \" \" + to_string(loose);").to_string(), "the first run defaults");
                eng->execute("kill(); kill(); kill();");
                write_module("ia_state.jai", body + "// touched\n");
                eng->execute("load_state();");
                check_eq(std::string("3 0 3 3 3"), eng->execute("to_string(kills) + \" \" + to_string(resets) + \" \" + to_string(pack.size()) + \" \" + to_string(held.n) + \" \" + to_string(loose);").to_string(),
                         "declarations without an initializer keep their values; the one that assigns resets");
                write_module("ia_state.jai", "float kills;\nint resets = 0;\narray<int> pack;\nclass KeepBox { int n = 0; }\nKeepBox held;\nvar loose;\n");
                eng->execute("load_state();");
                check_eq(std::string("float 0.0 3"), eng->execute("type_of(kills) + \" \" + to_string(kills) + \" \" + to_string(pack.size());").to_string(), "a declared type change resets to the new default");
                // the host's re-run of the same file is the same unit and keeps too
                const std::string host_path = (dir / "ia_host_state.jai").string();
                check_eq((int64_t)0, eng->execute_file_source(host_path, "int hp;\nhp;").as_int());
                eng->execute("hp = 42;");
                check_eq((int64_t)42, eng->execute_file_source(host_path, "int hp;\nhp;").as_int());
                check_eq((int64_t)5, eng->execute_file_source(host_path, "int hp = 5;\nhp;").as_int());
            }
            cleanup_temp_file((dir / "ia_state.jai").string());
        });

        test("an_imported_namespace_class_is_named_by_its_qualified_spelling", [=, this]() {
            // the importer never saw `namespace nq { class Box }`: the class is nq::Box (C++), the name that types declarations,
            // parameters, returns, fields, containers, handles and references, that type_of and the checker report, and that
            // reaches the redefined class after a re-import. Inside its namespace the bare name reads as nq::Box, outside it only
            // through using, and another namespace's Box is another class
            const auto dir = jai::foundry::test_temp_directory();
            const auto path = dir / "nq_mod.jai";
            int tick = 400;
            auto write_module = [&](const std::string& body) {
                { std::ofstream(path, std::ios::binary | std::ios::trunc) << body; }
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            const std::string module_v1 =
                "namespace nq {\nclass Box {\n\tint n = 3;\n\tBox() {}\n\tBox(int v) { n = v; }\n}\n"
                "class Holder {\n\tBox box;\n\tHolder() {}\n\tHolder(int v) { box = Box(v); }\n}\n}\n"
                "namespace nq::deep { class Leaf { int d = 7; } }\n"
                "namespace other { class Box { string s = \"o\"; } Box make() { return Box(); } }\n";
            const std::string uses =
                "import \"nq_mod.jai\";\n"
                "nq::Box a = nq::Box(5);\n"
                "nq::Holder h = nq::Holder(6);\n"
                "nq::Box d;\n"
                "nq::Box twice(nq::Box b) { return nq::Box(b.n * 2); }\n"
                "class Bag { nq::Box b; }\n"
                "Bag bag;\n"
                "bag.b = nq::Box(9);\n"
                "array<nq::Box> boxes = [nq::Box(1), nq::Box(2)];\n"
                "boxes.push(nq::Box(3));\n"
                "int sum = 0;\n"
                "for (auto& e : boxes) { sum += e.n; }\n"
                "map<string, nq::Box> named;\n"
                "named[\"k\"] = nq::Box(12);\n"
                "shared_ptr<nq::Box> handle = new nq::Box(11);\n"
                "weak_ptr<nq::Box> weak = handle;\n"
                "nq::Box& ref = a;\n"
                "ref.n += 1;\n"
                "nq::Box converted = 8;\n"
                "using nq::Box;\n"
                "Box bare = nq::Box(4);\n"
                "nq::deep::Leaf leaf;\n"
                "array<int> got = [a.n, h.box.n, d.n, twice(a).n, bag.b.n, sum, named[\"k\"].n, weak.lock()->n, converted.n, cast<nq::Box>(bare).n, leaf.d];\n"
                "string out = \"\";\n"
                "for (int v : got) { out += to_string(v) + \" \"; }\n"
                "out + type_of(a) + \" \" + type_of(handle) + \" \" + type_of(other::make()) + \" \" + other::make().s;\n";
            for (bool proofs : {true, false}) {
                write_module(module_v1);
                auto eng = make_engine();
                jai::stdlib::register_all(*eng);
                eng->type_proofs(proofs);
                eng->add_include_path(dir.string());
                check_eq(std::string("6 6 3 12 9 6 12 11 8 4 7 nq::Box nq::Box other::Box o"), eng->execute(uses).to_string(), "every qualified spelling of an imported class");
                check_eq(std::string("Cannot initialize 'bad' (declared nq::Box) with string|Cannot convert string to nq::Box (no suitable single-argument constructor)"),
                         eng->execute("var s = \"s\";\nstring r = \"\";\ntry { nq::Box bad = s; } catch (e) { r = e; }\n"
                                      "void take(nq::Box b) { }\ntry { take(s); } catch (e) { r += \"|\" + e; }\nr;").to_string(),
                         "a value the class does not take is still refused");
                for (const auto& [source, text] : std::vector<std::pair<std::string, std::string>>{
                         {"nq::Box bad2 = \"s\";", "cannot initialize 'bad2' declared 'nq::Box' with 'string'"},
                         {"nq::Box b2 = nq::Box(1);\nb2.nope;", "no member 'nope' in class 'nq::Box'"},
                         {"Box b4;", "Box"},
                         {"nq::Box b3 = nq::Box(1);\nb3.n = \"x\";", "cannot assign 'string' to field 'n' declared 'int'"}}) {
                    auto fresh = make_engine();
                    jai::stdlib::register_all(*fresh);
                    fresh->type_proofs(proofs);
                    fresh->add_include_path(dir.string());
                    std::string listing;
                    try { fresh->execute("import \"nq_mod.jai\";\n" + source); } catch (const std::exception& ex) { listing = ex.what(); }
                    check_true(listing.find(text) != std::string::npos, "the checker reads nq::Box as the module's Box before it runs: " + listing);
                }
                std::string module_v2 = module_v1;
                module_v2.replace(module_v2.find("int n = 3;"), 10, "int n = 30;");
                module_v2.replace(module_v2.find("n = v;"), 6, "n = v + 100;");
                write_module(module_v2);
                check_eq(std::string("30 101 102 6"), eng->execute("import \"nq_mod.jai\";\nnq::Box fresh_box;\nnq::Box made = nq::Box(1);\nnq::Box conv = 2;\n"
                                                                  "to_string(fresh_box.n) + \" \" + to_string(made.n) + \" \" + to_string(conv.n) + \" \" + to_string(a.n);").to_string(),
                         "the re-imported class is what nq::Box reaches; a live instance keeps its value");
            }
            std::filesystem::remove(path);
        });

        test("a_leaf_the_vm_inlines_is_replaced_by_its_modules_rerun_inside_the_hot_loop", [=, this]() {
            // the loop's own import re-runs the touched module mid-loop: the call that was grafted sees the new body from the
            // next iteration on, on both backends
            const auto dir = jai::foundry::test_temp_directory();
            const auto path = dir / "ia_leaf.jai";
            int tick = 200;
            auto write_leaf = [&](const std::string& body) {
                { std::ofstream(path, std::ios::binary | std::ios::trunc) << body; }
                std::error_code ec;
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
            };
            for (bool proofs : {true, false}) {
                write_leaf("int ia_leaf(int x) { return x + 1; }\n");
                auto eng = make_engine();
                eng->type_proofs(proofs);
                eng->add_include_path(dir.string());
                eng->add_function("touch_leaf", [&]() { write_leaf("int ia_leaf(int x) { return x + 1000; }\n"); });
                const auto total = eng->execute("import \"ia_leaf.jai\";\n"
                                                "int run() { int total = 0; for (int i = 0; i < 2000; i++) { import \"ia_leaf.jai\"; total += ia_leaf(i); if (i == 999) { touch_leaf(); } } return total; }\n"
                                                "run();").as_int();
                check_eq((int64_t)3000000, total);
            }
            std::filesystem::remove(path);
        });

        test("import_mtime_gate_sits_above_disk_cache", [=, this]() {
            // import's once-per-change gate decides IF the file runs at all; the disk
            // cache only decides parse-vs-load when it does (pin: hot-reload semantics).
            for (bool use_vm : {false, true}) {
                auto src = create_temp_file("cache_imp.jai", "cache_imp_bump();");
                std::filesystem::remove(sibling_of(src));
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                int bumps = 0;
                eng->add_function("cache_imp_bump", [&bumps]() { bumps++; });
                eng->add_include_path(jai::foundry::test_temp_directory().string());
                eng->set_import_behavior(engine::import_behavior::file_timestamp);
                eng->import_recheck_interval(0);   // the touch and the re-import happen within the default window
                eng->execute("import \"cache_imp.jai\"");
                eng->execute("import \"cache_imp.jai\"");
                check_eq(1, bumps, "unchanged file: import runs once");
                {
                    std::ofstream f(src, std::ios::trunc);
                    f << "cache_imp_bump(); cache_imp_bump();";
                }
                auto bumped = std::filesystem::last_write_time(sibling_of(src)) + std::chrono::seconds(2);
                std::filesystem::last_write_time(src, bumped);   // deterministic "edited later"
                eng->execute("import \"cache_imp.jai\"");
                check_eq(3, bumps, "mtime change: import re-runs the edited body");
                eng->execute("import \"cache_imp.jai\"");
                check_eq(3, bumps, "no further change: gated again");
                cleanup_temp_file(src);
                std::filesystem::remove(sibling_of(src));
            }
        });

        // A trace with each frame's directory dropped: "  at f (c.jai:1)\n..."
        auto trace_files = [](const std::string& trace) {
            std::string out;
            size_t start = 0;
            while (start < trace.size()) {
                size_t end = trace.find('\n', start);
                if (end == std::string::npos) { end = trace.size(); }
                std::string line = trace.substr(start, end - start);
                size_t open = line.find('(');
                size_t colon = line.rfind(':');
                if (open != std::string::npos && colon != std::string::npos && colon > open) {
                    size_t sep = line.find_last_of("/\\", colon);
                    if (sep != std::string::npos && sep > open) { line.erase(open + 1, sep - open); }
                }
                out += line + "\n";
                start = end + 1;
            }
            return out;
        };

        test("an_error_in_an_imported_top_level_names_the_imported_frames_and_the_import_chain", [=, this]() {
            // the imported unit's frames lead, the importer's frame at the import follows, byte-identical on both backends
            create_temp_file("imp_err_c.jai", "int boom() { array<int> a = [1]; return a[5]; }\nint X = boom();\n");
            create_temp_file("imp_err_d.jai", "import \"imp_err_c.jai\";\n");
            create_temp_file("imp_err_t.jai", "var pad = 1;\nthrow \"from the top\";\n");
            create_temp_file("imp_err_u.jai", "function run(string p) {\n\tinclude(p);\n}\nrun(\"imp_err_t.jai\");\n");
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->add_include_path(jai::foundry::test_temp_directory().string());
            std::string what;
            try { eng->execute("var before = 1;\nimport \"imp_err_d.jai\";\n"); } catch (const std::exception& ex) { what = ex.what(); }
            check_eq(std::string("Array index 5 out of bounds for array of size 1"), what);
            check_eq(std::string("  at boom (imp_err_c.jai:1)\n  at <script> (imp_err_c.jai:2)\n  at <script> (imp_err_d.jai:1)\n  at <script> (<script>:2)\n"),
                     trace_files(eng->format_stack_trace()));
            what.clear();
            try { eng->execute("include \"imp_err_u.jai\";\n"); } catch (const std::exception& ex) { what = ex.what(); }
            check_eq(std::string("from the top"), what);
            check_eq(std::string("  at <script> (imp_err_t.jai:2)\n  at run (imp_err_u.jai:2)\n  at <script> (imp_err_u.jai:4)\n  at <script> (<script>:1)\n"),
                     trace_files(eng->format_stack_trace()));
            for (const char* f : {"imp_err_c.jai", "imp_err_d.jai", "imp_err_t.jai", "imp_err_u.jai"}) { cleanup_temp_file((jai::foundry::test_temp_directory() / f).string()); }
        });

        test("file_data_answers_a_files_metadata_by_the_one_ladder", [=, this]() {
            // engine-core: path, filename, extension (with its dot), created/modified (ms since the Unix epoch), size, is_directory;
            // a relative name resolves like resolve_path; nothing there answers null (a watcher polls it without try/catch)
            const auto dir = jai::foundry::test_temp_directory() / "file_data_probe";
            std::filesystem::create_directories(dir);
            const auto file = dir / "save.dat";
            for (bool use_vm : {false, true}) {
                { std::ofstream(file, std::ios::binary | std::ios::trunc) << "12345"; }
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(*eng);
                eng->add_include_path(dir.string());
                auto d = eng->execute("var d = file_data(\"save.dat\"); d;");
                check_true(d.is_map(), "a map of the file's metadata");
                const std::string path = eng->execute("file_data(\"save.dat\")[\"path\"];").to_string();
                check_true(path.find("file_data_probe/save.dat") != std::string::npos && path.find('\\') == std::string::npos, path);
                check_eq(path, eng->execute("resolve_path(\"save.dat\");").to_string(), "the ladder's canonical spelling");
                check_eq(std::string("save.dat|.dat|5|false"), eng->execute("var d = file_data(\"save.dat\"); d[\"filename\"] + \"|\" + d[\"extension\"] + \"|\" + to_string(d[\"size\"]) + \"|\" + to_string(d[\"is_directory\"]);").to_string());
                const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                const int64_t modified = eng->execute("file_data(\"save.dat\")[\"modified\"];").as_int();
                check_true(modified > now_ms - 3600000 && modified <= now_ms + 60000, "modified is ms since the Unix epoch");
                auto created = eng->execute("file_data(\"save.dat\")[\"created\"];");
                check_true(created.is_null() || (created.is_int() && created.as_int() <= modified + 1000), "created: ms, or null where unavailable");
                // a rewrite moves modified (stamped forward so the clock's granularity cannot hide it)
                { std::ofstream(file, std::ios::binary | std::ios::trunc) << "1234567"; }
                std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(5));
                check_true(eng->execute("file_data(\"save.dat\")[\"modified\"];").as_int() > modified, "a rewrite moves modified");
                check_eq(script_int(7), eng->execute("file_data(\"save.dat\")[\"size\"];").as_int());
                check_true(eng->execute("file_data(\"no_such_file.dat\") == null;").as_bool(), "missing -> null");
                check_eq(std::string("true|0|"), eng->execute("var f = file_data(\"" + dir.generic_string() + "\"); to_string(f[\"is_directory\"]) + \"|\" + to_string(f[\"size\"]) + \"|\" + f[\"extension\"];").to_string());
                eng->disable(jai::filesystem);
                std::string denied;
                try { denied = eng->execute("var m = \"\"; try { file_data(\"save.dat\"); } catch (e) { m = e; } m;").to_string(); } catch (const std::exception& ex) { denied = ex.what(); }
                check_eq(std::string("Insufficient privilege: filesystem."), denied);
            }
            std::filesystem::remove_all(dir);
        });

        test("an_enum_in_a_file_included_inside_a_function_or_block_is_local_and_warns", [=, this]() {
            // include is textual: the file's enum is declared where the include runs; inside a function or a block that is a
            // local enum, so the program-scope enum keeps its values, and the include's report says so (both backends)
            create_temp_file("enum_inc.jai", "enum Color { cyan, magenta, yellow, key }\nvar seen = Color.yellow;\n");
            for (bool proofs : {true, false}) {
                auto eng = make_engine();
                jai::stdlib::register_all(*eng);
                eng->type_proofs(proofs);
                eng->add_include_path(jai::foundry::test_temp_directory().string());
                std::vector<std::string> channel;
                eng->set_script_warning_handler([&channel](const std::string& w) { channel.push_back(w); });
                check_eq((int64_t)2, eng->execute("enum Color { red, green, blue }\nvoid reload(string p) { include(p); }\nreload(\"enum_inc.jai\");\nColor.blue;").as_int());
                check_true(channel.size() == 1 && channel[0].find("enum 'Color' declared in function 'reload' is local to it") != std::string::npos, "the host's warning channel carries it");
                const check_report& r = eng->last_check_diagnostics();
                bool warned = false;
                for (const auto& d : r.diagnostics) {
                    if (d.severity == check_diagnostic::level::warning && d.message.find("enum_inc.jai:1:6: enum 'Color' declared in function 'reload' is local to it: the program-scope 'Color' keeps its values") != std::string::npos) { warned = true; }
                }
                check_true(warned, "the include's report names the enum and the function");
                check_eq((int64_t)2, eng->execute("string q = \"enum_inc.jai\";\n{ include(q); }\nColor.blue;").as_int());
                warned = false;
                for (const auto& d : eng->last_check_diagnostics().diagnostics) {
                    if (d.message.find("enum 'Color' declared in a block is local to it") != std::string::npos) { warned = true; }
                }
                check_true(warned, "a block include warns too");
                eng->execute("string top = \"enum_inc.jai\";\ninclude(top);");
                for (const auto& d : eng->last_check_diagnostics().diagnostics) {
                    check_true(d.message.find("is local to it") == std::string::npos, "a program-scope include replaces nothing silently: it is the program scope");
                }
            }
            cleanup_temp_file((jai::foundry::test_temp_directory() / "enum_inc.jai").string());
        });

        test("an_error_in_an_included_file_is_caught_like_inline_code", [=, this]() {
            // include is textual: a throw keeps its value and type, a runtime error is caught with its text alone, a load failure
            // (static check, parse) is caught with its listing, which names the included file; both backends, proofs on and off
            const std::string dir = jai::foundry::test_temp_directory().string();
            const std::string path = (jai::foundry::test_temp_directory() / "inc_err.jai").string();
            auto run = [&](jai::engine& e, const std::string& body) {
                create_temp_file("inc_err.jai", body);
                return e.execute("var r = null;\ntry { include(\"inc_err.jai\"); r = \"no error\"; } catch (e) { r = e; }\nr;");
            };
            for (bool proofs : {true, false}) {
                auto eng = make_engine();
                jai::stdlib::register_all(*eng);
                eng->type_proofs(proofs);
                eng->add_include_path(dir);
                check_eq(std::string("boom"), run(*eng, "throw \"boom\";\n").to_string());
                check_eq(std::string("42"), run(*eng, "throw 42;\n").to_string(), "the caught error reads as its message");
                auto typed = eng->execute("var r = null;\ntry { include(\"inc_err.jai\"); } catch (e) { r = e.value; }\nr;");
                check_true(typed.is_int(), "a thrown int stays an int across the include (e.value)");
                check_eq((int64_t)42, typed.as_int());
                check_eq(std::string("Cannot initialize 'x' (declared int) with string"), run(*eng, "var s = \"s\";\nint x = s;\n").to_string());
                check_eq(std::string("Array index 5 out of bounds for array of size 1"), run(*eng, "int boom() { array<int> a = [1]; return a[5]; }\nboom();\n").to_string());
                const std::string checked = run(*eng, "int x = \"s\";\n").to_string();
                check_true(checked.rfind("static check failed:\n", 0) == 0, checked);
                check_true(checked.find("inc_err.jai:1:5: error: cannot initialize 'x' declared 'int' with 'string'\n    int x = \"s\";\n        ^\n") != std::string::npos, checked);
                check_true(checked.find("<script>:1:5") == std::string::npos, checked);
                const std::string parsed = run(*eng, "void f(int var) {}\n").to_string();
                check_true(parsed.rfind("parse failed:", 0) == 0 && parsed.find("inc_err.jai:1:12: error:") != std::string::npos, parsed);
                // from a function body, a computed path; the program goes on after every catch
                create_temp_file("inc_err.jai", "throw \"deep\";\n");
                check_eq(std::string("deep after"), eng->execute("string go(string p) { try { include(p); } catch (e) { return e + \" after\"; } return \"none\"; }\ngo(\"inc_err.jai\");").to_string());
                // a nested include: the inner failure crosses both boundaries
                create_temp_file("inc_err_outer.jai", "include(\"inc_err.jai\");\n");
                check_eq(std::string("deep"), eng->execute("var q = null;\ntry { include(\"inc_err_outer.jai\"); } catch (e) { q = e; }\nq;").to_string());
                // after a failed include the includer's own location queries name the includer again
                const std::string root = (jai::foundry::test_temp_directory() / "inc_err_root.jai").string();
                const std::string here = eng->execute_file_source(root, "var p = \"\";\ntry { include(\"inc_err.jai\"); } catch (e) { }\np = current_script_path();\np;").to_string();
                check_true(here.size() >= 16 && here.substr(here.size() - 16) == "inc_err_root.jai", here);
            }
            cleanup_temp_file(path);
            cleanup_temp_file((jai::foundry::test_temp_directory() / "inc_err_outer.jai").string());
        });
    }
};

}

FOUNDRY_REGISTER(jai::foundry::tests::include_import_tests)
