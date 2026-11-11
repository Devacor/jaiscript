#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/type_info.hpp>
#include "value_rows.hpp"
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

namespace jai::foundry::tests {

class engine_tests : public suite {
public:
    engine_tests() : suite("Core Engine") {}

    void forge_tests() override {
        test("engine_has_stable_address", [this]() {
            check_false(std::is_move_constructible_v<engine>);
            check_false(std::is_move_assignable_v<engine>);
        });

        test("engine_owner_can_move", [this]() {
            auto original = make_engine();
            original->execute("int stable_x = 41; auto stable_f = [&]() { return stable_x + 1; };");
            auto* address = original.get();
            auto owner = std::move(original);
            check(owner.get() == address);
            check_eq(script_int{42}, owner->execute("stable_f();").as_int());
        });

        test("an_execute_returns_its_last_expression", [this]() {
            const value_row rows[] = {
                {"2 + 2", 4},
                {"5 * 6", 30},
                {"(2 + 3) * (4 - 1) / 5", 3},
                {"\"Hello, \" + \"World!\"", text{"Hello, World!"}},
                {"true && true", true},
                {"true && false", false},
                {"false || true", true},
                {"5 > 3", true},
                {"5 < 3", false},
                {"5 == 5", true},
                {"5 != 3", true},
                {"var x = null; x == null", true},
                {"var x = null; x != null", false},
            };
            check_values(rows);
        });

        test("execute_as_a_type_converts_the_result_or_throws", [this]() {
            auto eng = make_engine();
            check_eq(std::int64_t{4}, eng->execute<std::int64_t>("2 + 2"));
            check_eq(std::int32_t{4}, eng->execute<std::int32_t>("2 + 2"));
            check_eq(4, eng->execute<int>(std::string("2 + 2")));
            check_eq(4, eng->execute<int>(script_source("2 + 2")));
            check_eq(std::string("ab"), eng->execute<std::string>("\"a\" + \"b\""));
            instance_variables vars{{"n", script_value(script_int(40), eng.get())}};
            check_eq(42, eng->execute<int>("n + 2", vars));
            std::string out_of_range;
            try {
                eng->execute<std::int32_t>("3000000000");
            } catch (const std::exception& ex) {
                out_of_range = ex.what();
            }
            check_eq(std::string("Integer value out of range for int"), out_of_range);
            check_throws([&]() { eng->execute<int>("\"four\""); });
            check_eq(script_int{4}, eng->execute("2 + 2").as_int());
        });

        test("top_level_declarations_persist_across_executes", [this]() {
            // each step runs on the session's engine; a step with a value checks the execute's result
            struct step { const char* code; std::optional<int> value; };
            struct session { const char* code; std::vector<step> steps; };
            const session sessions[] = {
                {"var x = 42;", {{"x", 42}}},
                {"auto x = 42;", {{"x", 42}, {"x = 100;", {}}, {"x", 100}}},
                {"var counter = 0;", {{"counter = counter + 1;", {}}, {"counter = counter + 1;", {}}, {"counter", 2}}},
                {"auto a = 10; auto b = 20; auto c = a + b;", {{"a", 10}, {"b", 20}, {"c", 30}, {"a = 5;", {}}, {"a + b + c", 55}}},
                {"auto obj = {\"x\": 10, \"y\": 20};", {{"obj[\"x\"]", 10}, {"obj[\"y\"]", 20}, {"obj[\"x\"] = 30;", {}}, {"obj[\"x\"]", 30}}},
                {"function add(auto a, auto b) -> auto { return a + b; }", {{"add(3, 4)", 7}}},
                {"function add(a, b) { return a + b; }", {{"add(5, 3)", 8}, {"auto sum = add(10, 20);", {}}, {"sum", 30}}},
                {"class Point { int x = 0; int y = 0; Point(x_, y_) { x = x_; y = y_; } int sum() { return x + y; } }",
                 {{"auto p = Point(3, 4);", {}}, {"p.sum()", 7}, {"p.x = 10;", {}}, {"p.sum()", 14}}},
            };
            check_each(sessions, [&](const session& s) {
                auto eng = make_engine();
                eng->execute(s.code);
                for (const auto& [code, value] : s.steps) {
                    auto result = eng->execute(code);
                    if (value) { check_eq(*value, result.as<int>(), code); }
                }
            });
        });

        test("host_globals_and_functions_reach_scripts", [this]() {
            auto eng = make_engine();
            eng->add_global("PI", script_value(3.14159, eng.get()));
            check_near(eng->execute("PI * 2").as<double>(), 6.28318, 0.00001);
            eng->add_function("square", [](int n) { return n * n; });
            check_eq(eng->execute("square(5)").as<int>(), 25);

            eng->add_global("explicit_var", 100);
            eng->execute("auto script_var = 200;");
            check_eq(eng->execute("explicit_var").as<int>(), 100);
            check_eq(eng->execute("script_var").as<int>(), 200);
            eng->execute("explicit_var = 150;");
            eng->execute("script_var = 250;");
            check_eq(eng->execute("explicit_var").as<int>(), 150);
            check_eq(eng->execute("script_var").as<int>(), 250);
        });

        test("script_source_literal_lane", [this]() {
            auto eng = make_engine();
            // First execute parses; the re-executes hit the pointer-keyed literal lane.
            check_eq((int64_t)4, eng->execute(script_source("2 + 2")).as_int());
            check_eq((int64_t)4, eng->execute(script_source("2 + 2")).as_int());
            eng->execute(script_source("var ss_counter = 0;"));
            eng->execute(script_source("ss_counter = ss_counter + 1;"));
            eng->execute(script_source("ss_counter = ss_counter + 1;"));
            check_eq((int64_t)2, eng->execute(script_source("ss_counter")).as_int());
        });

        test("script_source_dynamic_routes_to_content_lane", [this]() {
            auto eng = make_engine();
            // A string_view wrap tags as non-literal: content-keyed lane, same semantics.
            std::string dynamic = "3 * 3";
            check_eq((int64_t)9, eng->execute(script_source(std::string_view(dynamic))).as_int());
            dynamic = "4 * 4";   // mutated content must be noticed (hot-reload semantics)
            check_eq((int64_t)16, eng->execute(script_source(std::string_view(dynamic))).as_int());
            // std::string still routes to the execute(const std::string&) overload, and a
            // bare literal cannot implicitly become a script_source (ctor is explicit) —
            // execute("...") keeps its historical std::string path.
            static_assert(!std::is_convertible_v<const char(&)[5], jai::script_source>);
            static_assert(!std::is_convertible_v<std::string, jai::script_source>);
            static_assert(std::is_convertible_v<std::string_view, jai::script_source>);
            check_eq((int64_t)25, eng->execute(std::string("5 * 5")).as_int());
        });

        test("script_source_literal_survives_cache_churn", [this]() {
            auto eng = make_engine();
            check_eq((int64_t)11, eng->execute(script_source("6 + 5")).as_int());
            // Flood the content cache past its LRU bound with unique dynamics; the
            // literal entry holds its own reference and must still re-execute correctly.
            for (int i = 0; i < 80; ++i) {
                eng->execute("1 + " + std::to_string(i));
            }
            check_eq((int64_t)11, eng->execute(script_source("6 + 5")).as_int());
        });

        test("execute_file_stat_gate", [this]() {
            namespace fs = std::filesystem;
            auto eng = make_engine();
            fs::path dir = jai::foundry::test_temp_directory() / "jai_engine_tests_file_gate";
            fs::create_directories(dir);
            fs::path file = dir / "gate_probe.jai";
            {
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out << "var fg_value = 1; fg_value";
            }
            std::string pathStr = file.string();
            check_eq((int64_t)1, eng->execute_file(pathStr).as_int());
            check_eq((int64_t)1, eng->execute_file(pathStr).as_int());   // verify pass
            check_eq((int64_t)1, eng->execute_file(pathStr).as_int());   // stat-only reuse
            // An edited file must be noticed (mtime/size gate -> reparse).
            {
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out << "fg_value = 2; fg_value";
            }
            check_eq((int64_t)2, eng->execute_file(pathStr).as_int());
            check_eq((int64_t)2, eng->execute_file(pathStr).as_int());
            std::error_code cleanupEc;
            fs::remove_all(dir, cleanupEc);
        });

        test("error_handling", [this]() {
            auto eng = make_engine();
            check_throws([&]() {
                eng->execute("undefined_variable");
            }, "Should throw on undefined variable");
        });

        test("a_jaibite_reruns_its_program_on_every_execute", [this]() {
            // a program re-run redefines its functions, classes and coroutines each time (the hot-reload path) and sees globals it changed
            struct bite_row { const char* code; const char* setup; std::vector<int> runs; bool through_engine = false; };
            const bite_row rows[] = {
                {"2 + 2", "", {4, 4}},
                {"10 * 3", "", {30, 30}, true},
                {"counter = counter + 1; counter", "var counter = 0;", {1, 2, 3}},
                {R"(
                    function double_it(auto n) -> auto { return n * 2; }
                    auto sum = 0;
                    for (auto i = 0; i < 100; i += 1) {
                        sum += double_it(i);
                    }
                    sum
                )", "", {9900, 9900}},
                {R"(
                    class Point {
                        int x = 7;
                        int doubled() { return x * 2; }
                    }
                    auto p = Point();
                    p.doubled()
                )", "", {14, 14, 14}},
                {R"(
                    try {
                        throw "boom";
                    } catch (e) {
                        caught = caught + 1;
                    }
                    caught
                )", "var caught = 0;", {1, 2}},
                {R"(
                    coroutine int counter() {
                        yield 1;
                        yield 2;
                        yield 3;
                    }
                    auto c = counter();
                    auto total = 0;
                    for (auto v : c) {
                        total += v;
                    }
                    total
                )", "", {6, 6}},
                {R"(
                    coroutine int counter() {
                        yield 1;
                        yield 2;
                        return 3;
                    }
                    auto c = counter();
                    auto total = c.next() + c.next() + c.next();
                    total
                )", "", {6, 6}},
            };
            check_each(rows, [&](const bite_row& row) {
                auto eng = make_engine();
                eng->execute(row.setup);
                auto bite = eng->jaibite(row.code);
                for (int expected : row.runs) {
                    check_eq(expected, (row.through_engine ? eng->execute(bite) : bite.execute()).as<int>());
                }
                if (std::string(row.setup) == "var counter = 0;") {
                    check_eq(3, eng->execute("counter").as<int>(), "the global the bite changed");
                }
            });
        });

        test("jaibite_parse_error_throws_at_creation", [this]() {
            auto eng = make_engine();
            check_throws([&]() {
                auto bite = eng->jaibite("auto x = = 5;");
            }, "Parse errors should surface when the jaibite is created");
        });

        test("jaibite_runtime_error_throws_on_execute", [this]() {
            // strict (default): an undeclared name fails the check when the bite is created
            auto eng = make_engine();
            check_throws<jai::static_check_error>([&]() { eng->jaibite("undefined_variable_xyz"); },
                "Undeclared identifiers fail the static check at jaibite creation");
            // a var-sourced mismatch loads clean and throws at every execute
            auto bite = eng->jaibite("var s = \"s\"; int i = s;");
            check_true(bite.checked_clean());
            check_throws([&]() { bite.execute(); }, "Runtime errors should throw on execute");
            check_throws([&]() { bite.execute(); }, "And keep throwing on re-execute");
            // a [[warn_mode]] unit: created fine, the runtime error surfaces on execute
            auto undeclared = eng->jaibite("[[warn_mode]]; undefined_variable_xyz");
            check_throws([&]() { undeclared.execute(); }, "Runtime errors should throw on execute");
            check_throws([&]() { undeclared.execute(); }, "And keep throwing on re-execute");
        });

        test("jaibite_outlives_engine_safely", [this]() {
            jai::jaibite bite;
            {
                auto eng = make_engine();
                bite = eng->jaibite("1 + 1");
                check_eq(bite.execute().as<int>(), 2);
            }
            check_throws([&]() { bite.execute(); }, "Executing after engine destruction should throw");
        });

        test("engine_type_infos_intern_and_describe_their_shape", [this]() {
            auto eng = make_engine();
            auto int_type = detail::engine_access::get_type_info_int(*eng);
            check(int_type != nullptr);
            check_eq(int_type, detail::engine_access::get_type_info_int(*eng));
            check(int_type != detail::engine_access::get_type_info_float(*eng));

            auto ref_type = detail::engine_access::get_type_info_reference(*eng, int_type);
            check(ref_type != nullptr);
            check(ref_type->is_reference());
            auto ref_ref_type = detail::engine_access::get_type_info_reference(*eng, ref_type);
            check(ref_ref_type != nullptr);
            check(ref_ref_type->is_reference());

            auto array_type = detail::engine_access::get_type_info_array(*eng, int_type);
            check(array_type != nullptr);
            check(array_type->is_array());
            check_eq(array_type->element_type().get(), int_type);

            auto string_type = detail::engine_access::get_type_info_string(*eng);
            auto map_type = detail::engine_access::get_type_info_map(*eng, string_type, int_type);
            check(map_type != nullptr);
            check(map_type->is_map());
            check_eq(map_type->key_type().get(), string_type);
            check_eq(map_type->value_type().get(), int_type);
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::engine_tests)
