#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"
#include <sstream>
#include <tuple>

using namespace jai::foundry;

namespace jai::foundry::tests {

namespace {

std::string printed_text(jai::engine& a_engine, const std::string& a_code) {
    auto capture = std::make_shared<std::ostringstream>();
    a_engine.set_output_stream(capture);
    a_engine.execute(a_code);
    return capture->str();
}

}

class io_stdlib_tests : public suite {
public:
    io_stdlib_tests() : suite("IO Stdlib Tests") {}

    void forge_tests() override {
        // ============================================================
        // The bare-engine print contract (the docs' two-line hello):
        // engine::make() ships a basic print (concatenation, no {} formatting,
        // host-controlled stream); register_all upgrades it IN PLACE — same
        // name, one registration, never a stacked double-print.
        // ============================================================

        test("bare_engine_print_concatenates_without_formatting", [this]() {
            check_each(std::vector<std::pair<std::string, std::string>>{
                {"print(\"hello\");", "hello"},
                {"print(\"Value: {}\", 42);", "Value: {}42"},
            }, [](const auto& c) {
                auto engine = jai::foundry::make_engine();
                check(printed_text(*engine, c.first).find(c.second) != std::string::npos);
            });
        });

        test("stdlib_upgrade_replaces_print_exactly_once", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            auto capture = std::make_shared<std::ostringstream>();
            engine->set_output_stream(capture);
            engine->execute("print(\"once\");");
            std::string output = capture->str();
            check(output.find("once") != std::string::npos);
            check_eq(output.find("once"), output.rfind("once"),
                "one registration fires: the upgrade replaced the basic print, not stacked on it");
        });

        // ============================================================
        // Output stream redirection
        // ============================================================

        // single-arg print is verbatim; {{ }} escapes are read only on the format path, which runs with arguments
        test("stdlib_print_writes_to_the_output_stream_and_formats_with_arguments", [this]() {
            const std::tuple<std::string, std::string, bool> cases[] = {
                {"print(\"Hello, World!\");", "Hello, World!", false},
                {"print(\"Value: {}\", 42);", "Value: 42", false},
                {"print(\"a\", \"b\", \"c\");", "abc", false},
                {R"(print("{{x}}");)", "{{x}}\n", true},
                {R"(print("{{}} {0}", 7);)", "{} 7\n", true},
                {R"(print("[{:>5}]", 42);)", "[   42]\n", true},
            };
            check_each(cases, [](const auto& c) {
                auto engine = make_engine();
                stdlib::register_all(*engine);
                const std::string output = printed_text(*engine, std::get<0>(c));
                if (std::get<2>(c)) {
                    check_eq(std::get<1>(c), output);
                } else {
                    check(output.find(std::get<1>(c)) != std::string::npos);
                }
            });
        });

        // ============================================================
        // Spec placeholders: {:spec} / {n:spec} — same mini-language as
        // template-string ${x:spec}, through the one shared formatter
        // ============================================================

        test("format_spec_placeholders", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(std::string("3.14"), engine->execute(R"(format("{:.2f}", 3.14159);)").as<std::string>());
            check_eq(std::string("    42"), engine->execute(R"(format("{0:>6}", 42);)").as<std::string>());
            check_eq(std::string("ff FF"), engine->execute(R"(format("{:x} {0:X}", 255);)").as<std::string>());
            check_eq(std::string("x=1 y=2.5"), engine->execute(R"(format("x={} y={1:.1f}", 1, 2.5);)").as<std::string>());
        });

        test("format_spec_outside_the_subset_raises", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            // a {n:...} placeholder whose spec the subset does not have raises, naming it; a {...} that is no placeholder stays literal
            auto caught = [&](const char* call) {
                return engine->execute(std::string("var msg = \"none\"; try { ") + call + "; } catch (e) { msg = e; } msg;").as<std::string>();
            };
            check_eq(std::string("Unsupported format spec ':zz' in format string"), caught("format(\"{} {:zz}\", 1, 2)"));
            check_eq(std::string("Unsupported format spec ':,x' in format string"), caught("format(\"{:,x}\", 255)"));
            check_eq(std::string("Unsupported format spec ':#x' in format string"), caught("print(\"{0:#x}\", 255)"));
            check_eq(std::string("{\"a\": 1} 2"), engine->execute(R"(format("{\"a\": 1} {}", 2);)").as<std::string>());
        });

        test("format_spec_groups_thousands", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(std::string("1,234,567 1,234,567.89 1_234_567 dead_beef 00,001,234 -0,001,234    1,234,567| -1,234,567.5 12 -999"),
                     engine->execute(R"(format("{:,} {:,.2f} {:_} {:_x} {:010,} {:010,} {:>12,}| {:,} {:,} {:,}", 1234567, 1234567.891, 1234567, 3735928559, 1234, -1234, 1234567, -1234567.5, 12, -999);)").as<std::string>());
            check_eq(std::string("1,234,567 0.500 9_876_543_210 +1,000"), engine->execute(R"(`${1234567:,} ${0.5:,.3f} ${9876543210:_d} ${1000:+,}`;)").as<std::string>());
            auto result = engine->execute(R"(var msg = ""; try { format("{:,}", "abc"); } catch (e) { msg = e; } msg;)");
            check_eq(std::string("format spec ':,' does not apply to string value"), result.as<std::string>());
        });

        test("format_spec_type_mismatch_throws", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            auto result = engine->execute(R"(var msg = ""; try { format("{:.1f}", "abc"); } catch (e) { msg = e; } msg;)");
            check_eq(std::string("format spec ':.1f' does not apply to string value"), result.as<std::string>());
        });

        test("output_capture_multiple_prints", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);

            auto capture = std::make_shared<std::ostringstream>();
            engine->set_output_stream(capture);

            engine->execute("print(\"Line 1\");");
            engine->execute("print(\"Line 2\");");
            engine->execute("print(\"Line 3\");");

            std::string output = capture->str();
            check(output.find("Line 1") != std::string::npos);
            check(output.find("Line 2") != std::string::npos);
            check(output.find("Line 3") != std::string::npos);
        });

        test("output_capture_reset_to_stdout", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);

            auto capture = std::make_shared<std::ostringstream>();
            engine->set_output_stream(capture);

            engine->execute("print(\"Captured\");");

            // Reset to stdout (nullptr)
            engine->set_output_stream(nullptr);

            // Further prints should go to stdout, not capture buffer
            std::string captured = capture->str();
            check(captured.find("Captured") != std::string::npos);
        });

        test("output_capture_skip_newline", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);

            auto capture = std::make_shared<std::ostringstream>();
            engine->set_output_stream(capture);

            engine->execute("print(\"NoNewline\", skip_newline);");
            engine->execute("print(\"Next\");");

            std::string output = capture->str();
            // Should have "NoNewline" immediately followed by "Next" on same line
            check(output.find("NoNewlineNext") != std::string::npos);
        });

        // ============================================================
        // Format function tests
        // ============================================================

        test("format_substitutes_placeholders_and_concatenates_the_rest", [this]() {
            check_stdlib_results<std::string>({
                {"format(\"Hello, {}!\", \"World\");", "Hello, World!"},
                {"format(\"{} + {} = {}\", 2, 3, 5);", "2 + 3 = 5"},
                {"format(\"a\", \"b\", \"c\");", "abc"},
                {"format(42);", "42"},
            });
        });

        // C++20's constant-format rule: only a string literal written as the first argument of print or format is a format
        // string; any other text, user input included, prints as it is
        test("only_a_string_literal_is_a_format_string", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            auto capture = std::make_shared<std::ostringstream>();
            engine->set_output_stream(capture);
            engine->execute(R"(
                string user = "a{}b";
                print(user, 5);
                print("x{}y", 5);
                var held = "{} {}";
                print(held, 1, 2);
                array<string> texts = ["{0}"];
                print(texts[0], 3);
                print(`t${1}{}`, 4);
                auto p = print;
                p("{}", 6);
                print(vformat(user, 7));
            )");
            check_eq(std::string("a{}b5\nx5y\n{} {}12\n{0}3\nt1{}4\n{}6\na7b\n"), capture->str());
            check_eq(std::string("{}1"), engine->execute(R"(var s = "{}"; format(s, 1);)").as<std::string>());
            check_eq(std::string("1-{2}"), engine->execute(R"(string s = "{}-{{{}}}"; vformat(s, 1, 2);)").as<std::string>());
            check_eq(std::string("mine"), engine->execute(R"(function format(a, b) { return "mine"; } format("{}", 1);)").as<std::string>());
        });

        test("format_of_a_runtime_text_is_a_load_error", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            std::string message;
            try {
                engine->execute(R"(string s = "{}"; format(s, 1);)");
            } catch (const static_check_error& e) {
                message = e.what();
            }
            check(message.find("the format text of 'format' must be a string literal: vformat(fmt, ...) formats a run-time text, and + or a template string joins text") != std::string::npos, message);
            check_eq(std::string("5 units"), engine->execute(R"(int n = 5; format(n, " units");)").as<std::string>());
        });

        // docs/site/guide/01-hello.html: a format text's placeholders and the values after it pair up; a literal's mismatch
        // fails the load, vformat's run-time text raises the same counts when it runs
        test("format_placeholders_and_arguments_pair_up", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            const auto load_error = [&](const std::string& a_code) {
                try {
                    engine->execute(a_code);
                } catch (const static_check_error& e) {
                    const std::string what = e.what();
                    const size_t at = what.find("the format text of ");
                    return at == std::string::npos ? what : what.substr(at, what.find('\n', at) - at);
                }
                return std::string("loaded");
            };
            check_eq(std::string("the format text of 'print' has 2 placeholder(s) for 1 argument(s)"), load_error(R"(print("{} {}", 1);)"));
            check_eq(std::string("the format text of 'print' names {2}, past its 1 argument(s)"), load_error(R"(print("{2}", 1);)"));
            check_eq(std::string("the format text of 'format' has 1 placeholder(s) for 2 argument(s)"), load_error(R"(string s = format("{} items", 5, 6);)"));
            check_eq(std::string("the format text of 'format' never uses argument 1 of its 2 argument(s)"), load_error(R"(string s = format("{0} {0}", 5, 6);)"));
            check_eq(std::string("the format text of 'print' has 2 placeholder(s) for 1 argument(s)"), load_error(R"(print("{:>4} {}", 1, skip_newline);)"));
            check_eq(std::string("the format text of 'print' has 2 placeholder(s) for 1 argument(s)"), load_error(R"(function f(var a) { print("{} {}", a); })"));
            check_eq(std::string("the format text of 'print' has 3 placeholder(s) for 2 argument(s)"), load_error(R"(function f(var a) { print("{} {} {}", 1, a); })"));
            // a format text alone pairs its placeholders with no values
            check_eq(std::string("the format text of 'print' has 1 placeholder(s) for 0 argument(s)"), load_error(R"(print("{}");)"));
            check_eq(std::string("the format text of 'print' has 1 placeholder(s) for 0 argument(s)"), load_error(R"(print("{:>4}", skip_newline);)"));
            check_eq(std::string("the format text of 'format' names {0}, past its 0 argument(s)"), load_error(R"(string s = format("{0}");)"));
            check_eq(std::string("{}|{{}}|a {{ b }}|plain\n"), printed_text(*engine, R"(
                string braces = "{}";
                print(braces, "|", skip_newline);
                print("{{}}", skip_newline);
                print("|a {{ b }}|", skip_newline);
                print(format("plain"));
            )"));
            // a value's place a control may take is the run's to count; text without a placeholder is no format text
            check_eq(std::string("1 2|1|2 1 2|x = 5|[7]\n"), printed_text(*engine, R"(
                function show(var v) { print("{}|", v, skip_newline); }
                print("{} {}|", 1, 2, skip_newline);
                show(1);
                print("{1} {0} {1}|", 1, 2, skip_newline);
                print("x = ", 5, skip_newline);
                print("|[{:d}]", 7);
            )"));

            const auto raised = [&](const std::string& a_code) {
                return engine->execute("string r = \"none\"; try { " + a_code + " } catch (e) { r = e; } r;").as<std::string>();
            };
            check_eq(std::string("Format text has 2 placeholder(s) for 1 argument(s)"), raised(R"(string t = "{} {}"; vformat(t, 1);)"));
            check_eq(std::string("Format text has 1 placeholder(s) for 0 argument(s)"), raised(R"(string t = "{}"; vformat(t);)"));
            check_eq(std::string("Format text has 1 placeholder(s) for 0 argument(s)"), raised(R"([[warn_mode]] print("{}");)"));
            check_eq(std::string("Format text has 1 placeholder(s) for 0 argument(s)"), raised(R"([[warn_mode]] string s = format("{}");)"));
            check_eq(std::string("Format text names {2}, past its 1 argument(s)"), raised(R"(string t = "{2}"; vformat(t, 1);)"));
            check_eq(std::string("Format text has 1 placeholder(s) for 2 argument(s)"), raised(R"(string t = "{} items"; vformat(t, 5, 6);)"));
            check_eq(std::string("Format text never uses argument 0 of its 2 argument(s)"), raised(R"(string t = "{1}{1:d}"; vformat(t, 5, 6);)"));
            check_eq(std::string("Format text has 1 placeholder(s) for 2 argument(s)"), raised(R"(function line(var last) -> string { return format("{}", 1, last); } line(2);)"));
        });
    }
};

} // namespace jai::foundry::tests

// Auto-register with the test framework
FOUNDRY_REGISTER(jai::foundry::tests::io_stdlib_tests)
