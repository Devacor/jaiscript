#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class switch_statement_tests : public suite {
public:
    switch_statement_tests() : suite("Switch Statement Tests") {}
    
    void forge_tests() override {
        test("basic_integer_switch", [this]() {
            auto engine = make_engine();
            jai::stdlib::register_all(engine);
            auto result = engine->execute(R"(
                auto x = 2;
                auto result = "";
                switch (x) {
                    case 1:
                        result = "one";
                    case 2:
                        result = "two";
                    case 3:
                        result = "three";
                    default:
                        result = "other";
                }
                result
            )");
            check_eq(result.template as<std::string>(), std::string("two"));
        });
        
        test("switch_runs_the_matching_case_and_falls_through_only_when_told", [this]() {
            const std::pair<const char*, const char*> cases[] = {
                {"auto x = 2; auto result = \"\"; switch (x) { case 1: result = result + \"one\"; fallthrough; "
                 "case 2: result = result + \"two\"; fallthrough; case 3: result = result + \"three\"; fallthrough; "
                 "default: result = result + \"default\"; } result", "twothreedefault"},
                {"auto x = 2; auto result = \"\"; switch (x) { case 1: result = \"one\"; break; case 2: result = \"two\"; "
                 "break; case 3: result = \"three\"; break; default: result = \"other\"; } result", "two"},
                {"auto name = \"Bob\"; auto greeting = \"\"; switch (name) { case \"Alice\": greeting = \"Hello Alice!\"; "
                 "case \"Bob\": greeting = \"Hey Bob!\"; case \"Charlie\": greeting = \"Hi Charlie!\"; "
                 "default: greeting = \"Hello stranger!\"; } greeting", "Hey Bob!"},
                {"auto x = 99; auto result = \"\"; switch (x) { case 1: result = \"one\"; case 2: result = \"two\"; default: result = \"default case\"; } result", "default case"},
                {"auto x = 99; auto result = \"unchanged\"; switch (x) { case 1: result = \"one\"; case 2: result = \"two\"; } result", "unchanged"},
                {"auto x = 1; auto y = 2; auto result = \"\"; switch (x) { case 1: switch (y) { case 1: result = \"x=1,y=1\"; "
                 "case 2: result = \"x=1,y=2\"; default: result = \"x=1,y=other\"; } case 2: result = \"x=2\"; "
                 "default: result = \"x=other\"; } result", "x=1,y=2"},
                {"auto get_name = [](int x) { switch (x) { case 1: return \"one\"; case 2: return \"two\"; case 3: return \"three\"; default: return \"other\"; } }; get_name(2)", "two"},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_eq(engine->execute(c.first).template as<std::string>(), std::string(c.second));
            });
        });

        test("an_empty_case_shares_the_body_of_the_label_below_it", [this]() {
            const std::vector<std::pair<std::string, std::string>> cases = {
                {"string r = \"\"; for (int i = 0; i < 6; ++i) { switch (i) { case 0: r += \"z\"; case 1: case 2: r += \"a\"; case 3: default: r += \"d\"; } } print(r);", "zaaddd"},
                {"string f(int i) { switch (i) { case 1: case 2: case 3: return \"low\"; case 4: break; case 5: case 6: return \"hi\"; case 7: } return \"none\"; }\n"
                 "string r = \"\"; for (int i = 0; i < 9; ++i) { r += f(i) + \" \"; } print(r);", "none low low low none hi hi none none "},
                {"int f(string s) { switch (s) { case \"a\": case \"b\": return 1; default: return 0; } } print(f(\"a\"), f(\"b\"), f(\"c\"));", "110"},
                {"int n = 0; int bump() { n += 1; return 9; } string r = \"u\"; switch (1) { case 1: case bump(): r = \"body\"; } print(r, n);", "body1"},
                {"string r = \"u\"; switch (1) { case 1: } switch (1) { case 1: default: } switch (2) { case 1: break; case 2: r += \"2\"; } switch (1) { case 1: break; case 2: r += \"x\"; } print(r);", "u2"},
                {"switch (1) { default: case 2: print(1); }", "ERROR a label that shares a body with 'default' needs 'default' as the last label of the switch"},
                {"switch (1) { case 1: default: print(1); case 2: print(2); }", "ERROR a label that shares a body with 'default' needs 'default' as the last label of the switch"},
                {"switch (3) { default: break; case 2: print(2); } switch (3) { default: print(\"d\"); case 1: case 2: print(2); }", "d"},
            };
            check_outputs(cases);
        });

        test("switch_with_expressions", [this]() {
            auto engine = make_engine();
            auto result = engine->execute(R"(
                auto x = 5;
                auto result = 0;
                switch (x + 5) {
                    case 8:
                        result = 8;
                    case 10:
                        result = 10;
                    case 12:
                        result = 12;
                    default:
                        result = -1;
                }
                result
            )");
            check_eq(result.template as<int>(), 10);
        });
        
        test("case_values_run_in_order_until_a_match_and_once_on_a_fallthrough", [this]() {
            auto engine = make_engine();
            auto result = engine->execute(R"(
                int trace = 0;
                int mark(int v) { trace = trace * 10 + v; return v; }
                array<int> run(int x) {
                    trace = 0;
                    int hits = 0;
                    switch (x) {
                        case mark(1): hits += 1; fallthrough;
                        case mark(2): hits += 10;
                        case mark(3): hits += 100; fallthrough;
                        case mark(4): hits += 1000; fallthrough;
                        default: hits += 10000;
                    }
                    return [trace, hits];
                }
                [run(1), run(2), run(3), run(4), run(9)]
            )");
            check_eq(std::string("[[12, 11], [12, 10], [1234, 11100], [1234, 11000], [1234, 10000]]"), result.to_string());
        });

        test("switch_on_a_call_evaluates_the_condition_once", [this]() {
            auto engine = make_engine();
            auto result = engine->execute(R"(
                int calls = 0;
                int next() { calls += 1; return calls; }
                int run() {
                    int out = 0;
                    for (int i = 0; i < 6; ++i) {
                        switch (next() % 3) {
                            case 0: out += 1;
                            case 1: out += 10;
                            default: out += 100;
                        }
                    }
                    return out * 100 + calls;
                }
                run()
            )");
            check_eq((int64_t)22206, result.as_int());
        });

        test("switch_on_dynamic_values_uses_the_languages_equality", [this]() {
            auto engine = make_engine();
            auto result = engine->execute(R"(
                class Key {
                    int k;
                    Key(int v) { k = v; }
                    bool operator==(const Key& other) { return k % 10 == other.k % 10; }
                }
                string number(var v) {
                    switch (v) {
                        case 1: return "one";
                        case 3.5: return "three and a half";
                        default: return "other";
                    }
                }
                string key(Key v) {
                    switch (v) {
                        case Key(1): return "one";
                        case Key(2): return "two";
                        default: return "other";
                    }
                }
                number(1.0) + "," + number(3.5) + "," + number(2) + "," + key(Key(12)) + "," + key(Key(21)) + "," + key(Key(5))
            )");
            check_eq(std::string("one,three and a half,other,two,one,other"), result.template as<std::string>());
        });

        test("break_and_continue_inside_a_switch_inside_a_loop", [this]() {
            auto engine = make_engine();
            auto result = engine->execute(R"(
                int run() {
                    int out = 0;
                    for (int i = 0; i < 10; ++i) {
                        switch (i & 3) {
                            case 0: continue;
                            case 1:
                                if (i > 4) { break; }
                                out += 1;
                            case 2: {
                                int local = i * 2;
                                out += local;
                            }
                            default:
                                switch (i) {
                                    case 3: out += 1000; break;
                                    default: out += 100;
                                }
                        }
                        out += 10000;
                    }
                    return out;
                }
                run()
            )");
            check_eq((int64_t)71117, result.as_int());
        });

        test("int_tables_keep_first_match_negative_sparse_and_non_int_subjects", [this]() {
            auto engine = make_engine();
            auto result = engine->execute(R"(
                int dense(var v) {
                    switch (v) {
                        case -2: return 1;
                        case 0: return 2;
                        case 3: return 3;
                        case 0: return 4;
                        case 7: return 5;
                        default: return 9;
                    }
                }
                int sparse(int v) {
                    switch (v) {
                        case 1: return 1;
                        case 1000000: return 2;
                        case -1000000: return 3;
                        case 5: return 4;
                    }
                    return 0;
                }
                int total = 0;
                for (int i = -3; i < 9; ++i) { total = total * 10 + dense(i); }
                array<int> out = [total, dense(3.0), dense(-2.0), dense(2.5), sparse(1000000), sparse(-1000000), sparse(5), sparse(6)];
                out
            )");
            check_eq(std::string("[919299399959, 3, 1, 9, 2, 3, 4, 0]"), result.to_string());
        });

        test("const_keys_follow_a_redeclaration", [this]() {
            // the same unit re-run (a reload of its file) re-initializes its consts in place: the table built from the old values goes stale
            auto engine = make_engine();
            engine->disable(jai::permissions::filesystem);
            const std::string path = "D:/virtual/switch/const_keys.jai";
            const std::string pick = R"(
                int pick(int v) {
                    switch (v) {
                        case A: return 10;
                        case B: return 20;
                        case C: return 30;
                        case D: return 40;
                        default: return 0;
                    }
                }
                int sum = pick(1) + pick(2) + pick(5);
            )";
            engine->execute_file_source(path, "const int A = 1; const int B = 2; const int C = 3; const int D = 4;" + pick);
            check_eq((int64_t)30, engine->execute("sum").as_int());
            engine->execute_file_source(path, "const int A = 5; const int B = 2; const int C = 3; const int D = 4;");
            check_eq((int64_t)10, engine->execute("pick(5)").as_int());
            check_eq((int64_t)0, engine->execute("pick(1)").as_int());
        });

        test("fallthrough_only_in_switch", [this]() {
            auto engine = make_engine();
            // This should fail to parse - fallthrough outside switch
            check_throws<parse_error>([&]() {
                engine->execute(R"(
                    fallthrough;
                )");
            });
        });

        test("a_declaration_in_an_unbraced_case_names_the_braces_once", [this]() {
            // a case body holds statements, as in C++: a declaration there is one error naming the braces, the rest of the switch
            // parses without a cascade, and the braced body declares
            const char* unbraced[] = {
                "string a = \"x\";\nswitch (a) {\n    case \"x\":\n        int n = 3;\n        print(n);\n    default: print(\"other\");\n}",
                "string a = \"x\";\nswitch (a) {\n    case \"y\": print(1);\n    default:\n        array<string> parts = \"a:b\".split(\":\");\n        print(parts);\n}",
            };
            for (const char* src : unbraced) {
                auto engine = make_engine();
                std::string got = "no error";
                try { engine->execute(src); } catch (const std::exception& ex) { got = ex.what(); }
                const std::string text = "error: a declaration in a case body needs the body in braces: case value: { ... }";
                const size_t at = got.find(text);
                check_true(at != std::string::npos && got.find("error:", at + 1) == std::string::npos, got);
            }
            auto engine = make_engine();
            check_eq((int64_t)3, engine->execute("string a = \"x\"; int r = 0; switch (a) { case \"x\": { int n = 3; r = n; } default: r = -1; } r;").as_int());
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::switch_statement_tests)