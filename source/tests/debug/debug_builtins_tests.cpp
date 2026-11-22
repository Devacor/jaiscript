// debug::stack() and debug::locals() from a script, the controller's dump of an uncaught error from C++, and where a rethrown
// error says it was raised: identical on both backends.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/debug/controller.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <string>

namespace jai::foundry::tests {

class debug_builtins_tests : public suite {
public:
    debug_builtins_tests() : suite("Debug Builtins") {}

    static std::shared_ptr<jai::engine> engine_on(jai::backend_type type) {
        auto eng = make_default_engine();
        eng->set_backend(type);
        jai::stdlib::register_all(eng);
        return eng;
    }

    // What `source` leaves in the global `seen` (text), on each backend
    void check_both(const std::string& source, const std::string& expected) {
        for (const auto type : {jai::backend_type::interpreter, jai::backend_type::vm}) {
            auto eng = engine_on(type);
            std::string got;
            try {
                eng->execute_file_source("probe.jai", source);
                got = eng->get_variable("seen").as_string();
            } catch (const std::exception& e) {
                got = std::string("error: ") + e.what();
            }
            check_eq(expected, got, type == jai::backend_type::vm ? "vm" : "interpreter");
        }
    }

    void forge_tests() override {
        test("stack_names_every_frame_innermost_first_and_locals_reads_a_frame", [this]() {
            check_both(
                "string seen;\n"                                                   // 1
                "class Cell { int v = 3; array<int> marks = [1, 2]; }\n"           // 2
                "function inner(int a, string s) -> int {\n"                       // 3
                "    Cell c;\n"                                                     // 4
                "    map<string, int> m = {\"k\": 4};\n"                           // 5
                "    seen += to_string(debug::stack()) + \"\\n\";\n"               // 6
                "    seen += to_string(debug::locals()) + \"\\n\";\n"              // 7
                "    seen += to_string(debug::locals(1)) + \"\\n\";\n"             // 8
                "    return a;\n"                                                   // 9
                "}\n"                                                              // 10
                "function outer() -> int {\n"                                      // 11
                "    int depth = 2;\n"                                             // 12
                "    auto apply = [&](int k) -> int { return inner(k, \"hi\"); };\n"   // 13: [&] captures only what it names
                "    return apply(depth * 3);\n"                                   // 14
                "}\n"                                                              // 15
                "for (int i = 0; i < 1; ++i) {\n"                                  // 16
                "    int block = 9;\n"                                             // 17
                "    outer();\n"                                                   // 18
                "    seen += to_string(debug::locals());\n"                        // 19
                "}\n",                                                             // 20
                "[\"inner (probe.jai:6)\", \"<lambda> (probe.jai:13)\", \"outer (probe.jai:14)\", \"<script> (probe.jai:18)\"]\n"
                "{\"a\": 6, \"c\": Cell{v: 3, marks: [1, 2]}, \"m\": {\"k\": 4}, \"s\": \"hi\"}\n"
                "{\"k\": 6}\n"
                "{\"block\": 9, \"i\": 0}");
        });

        test("locals_are_copies_and_a_frame_past_the_stack_raises", [this]() {
            check_both(
                "string seen;\n"
                "function f() -> int {\n"
                "    array<int> xs = [1];\n"
                "    auto copy = debug::locals();\n"
                "    copy[\"xs\"].push(2);\n"
                "    seen = to_string(xs.size()) + \" \";\n"
                "    try { debug::locals(9); } catch (e) { seen += e.message; }\n"
                "    return 0;\n"
                "}\n"
                "f();\n",
                "1 'debug::locals' has no frame 9: debug::stack() lists the frames");
        });

        test("locals_inside_a_hot_typed_loop_are_current", [this]() {
            check_both(
                "string seen;\n"
                "function spin() -> int {\n"
                "    int sum = 0;\n"
                "    for (int i = 0; i < 5000; ++i) {\n"
                "        sum += i;\n"
                "        if (i == 4000) { seen = to_string(debug::locals()); }\n"
                "    }\n"
                "    return sum;\n"
                "}\n"
                "spin();\n",
                "{\"i\": 4000, \"sum\": 8002000}");
        });

        test("a_rethrow_keeps_where_the_error_was_raised", [this]() {
            check_both(
                "string seen;\n"                                                   // 1
                "function risky(int n) -> int {\n"                                 // 2
                "    array<int> xs = [1, 2];\n"                                    // 3
                "    return xs[n];\n"                                              // 4
                "}\n"                                                              // 5
                "function relay() {\n"                                             // 6
                "    try { risky(5); } catch (e) { throw; }\n"                     // 7
                "}\n"                                                              // 8
                "try { relay(); } catch (e) {\n"                                   // 9
                "    seen = to_string(e.line) + \" \" + to_string(e.stack.size()) + \" \" + e.message;\n"   // 10
                "}\n"                                                              // 11
                "function thrower() { throw \"bad\"; }\n"                          // 12
                "function relay_thrown() { try { thrower(); } catch (e) { throw; } }\n"   // 13
                "try { relay_thrown(); } catch (e) { seen += \" | \" + to_string(e.line) + \" \" + e.message; }\n",   // 14
                "4 3 Array index 5 out of bounds for array of size 2 | 12 bad");
        });

        test("an_uncaught_error_reaches_the_host_with_every_frame_and_its_locals", [this]() {
            for (const auto type : {jai::backend_type::interpreter, jai::backend_type::vm}) {
                auto eng = engine_on(type);
                std::string dumped;
                int reports = 0;
                auto& dbg = eng->debugger();
                dbg.set_on_uncaught_error([&](const std::string& message) {
                    ++reports;
                    dumped = message + "\n" + dbg.dump();
                });
                std::string error;
                try {
                    eng->execute_file_source("dump.jai",
                        "function risky(int n) -> int {\n"
                        "    array<int> xs = [1, 2];\n"
                        "    return xs[n];\n"
                        "}\n"
                        "function caller(string why) -> int {\n"
                        "    int at = 5;\n"
                        "    try { risky(9); } catch (e) { }\n"
                        "    return risky(at);\n"
                        "}\n"
                        "caller(\"test\");\n");
                } catch (const std::exception& e) {
                    error = e.what();
                }
                const char* name = type == jai::backend_type::vm ? "vm" : "interpreter";
                check_true(error.find("Array index 5") != std::string::npos, error);
                check_eq(1, reports, "only the error no catch takes reaches the host");
                check_eq(std::string("Array index 5 out of bounds for array of size 2\n"
                                     "  at risky (dump.jai:3)\n"
                                     "      n = 5\n"
                                     "      xs = [1, 2]\n"
                                     "  at caller (dump.jai:8)\n"
                                     "      at = 5\n"
                                     "      why = \"test\"\n"
                                     "  at <script> (dump.jai:10)\n"), dumped, name);
            }
        });
    }
};

} // namespace jai::foundry::tests

using debug_builtins_tests = jai::foundry::tests::debug_builtins_tests;
FOUNDRY_REGISTER(debug_builtins_tests)
