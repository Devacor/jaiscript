#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/detail/float_text.hpp>
#include "../case_table.hpp"
#include <charconv>
#include <cstdio>
#include <cstring>
#include <random>
#include <filesystem>
#include <fstream>

using namespace jai::foundry;

namespace jai::foundry::tests {

class string_stdlib_tests : public suite {
public:
    string_stdlib_tests() : suite("String Stdlib Tests") {}

    void forge_tests() override {
        test("float_text_parses_as_strtod_parses", [this]() {
            // the from_chars lane against strtod over a corpus: shortest, round-trip and fixed spellings of random doubles,
            // subnormals, overflow and underflow, hex, inf and nan, signs, leading whitespace and partial spellings; the same
            // bits, the same end and the same range verdict every time
            size_t mismatches = 0, checked = 0;
            std::string first_bad;
            auto compare = [&](const std::string& text) {
                const auto fast = jai::detail::parse_float_text(text.c_str(), text.size());
                const auto slow = jai::detail::parse_float_text_strtod(text.c_str());
                const bool same_value = (std::isnan(fast.value) && std::isnan(slow.value)) || std::memcmp(&fast.value, &slow.value, sizeof(double)) == 0;
                ++checked;
                if (!same_value || fast.end != slow.end || fast.out_of_range != slow.out_of_range) {
                    if (mismatches++ == 0) { first_bad = text; }
                }
            };
            std::mt19937_64 rng(20261009);
            char buf[64];
            for (int i = 0; i < 200000; ++i) {
                uint64_t bits = rng();
                double v;
                std::memcpy(&v, &bits, sizeof v);
                if (!std::isfinite(v)) { continue; }
                auto r = std::to_chars(buf, buf + sizeof buf, v);
                compare(std::string(buf, r.ptr));
                std::snprintf(buf, sizeof buf, "%.17g", v); compare(buf);
                std::snprintf(buf, sizeof buf, "%.3e", v); compare(buf);
                const double small = std::ldexp(static_cast<double>(rng() % 1000000), -static_cast<int>(rng() % 40));
                std::snprintf(buf, sizeof buf, "%.6f", small); compare(buf);
                std::snprintf(buf, sizeof buf, "%.9f", -small); compare(buf);
            }
            for (const char* t : {"0", "-0", "0.0", "00012.50", "1.", "-.5", ".5", "5.", "1e", "1e+", "1e-", "1e5", "1E5", "1e-5", "-1e-5",
                                  "4.9e-324", "5e-324", "2.4703282292062327e-324", "2.4703282292062328e-324", "2.2250738585072011e-308",
                                  "2.2250738585072014e-308", "1.7976931348623157e308", "1.7976931348623159e308", "1e309", "-1e309",
                                  "1e-400", "-1e-400", "0x1p3", "0X1.8p1", "-0x10", "inf", "-inf", "INF", "infinity", "nan", "NaN",
                                  "nan(123)", " 1.5", "\t2", "+1.5", "+.5", "", "-", ".", "-.", "e5", "1.5abc", "12,5", "1_000",
                                  "123456789012345678901234567890", "0.000000000000000000000000000000000001", "9007199254740993",
                                  "1.0000000000000002", "0.1", "0.2", "0.30000000000000004"}) {
                compare(t);
            }
            check_eq(size_t(0), mismatches, "mismatches over " + std::to_string(checked) + " texts (first: '" + first_bad + "')");
        });
        // ============================================================
        // Observer methods (non-mutating)
        // ============================================================

        test("length_and_size_count_bytes", [this]() {
            check_stdlib_results<script_int>({
                {"var s = \"\"; s.length();", 0},
                {"var s = \"hello\"; s.length();", 5},
                {"var s = \"test\"; s.size();", 4},
            });
        });

        test("empty_is_true_only_for_an_empty_string", [this]() {
            check_stdlib_results<bool>({
                {"var s = \"\"; s.empty();", true},
                {"var s = \"a\"; s.empty();", false},
            });
        });

        test("at_front_and_back_read_one_character", [this]() {
            check_stdlib_results<script_char>({
                {"var s = \"hello\"; s.at(1);", 'e'},
                {"var s = \"hello\"; s.at(-1);", 'o'},
                {"var s = \"hello\"; s.at(-2);", 'l'},
                {"var s = \"hello\"; s.front();", 'h'},
                {"var s = \"hello\"; s.back();", 'o'},
            });
        });

        test("substr_takes_a_start_and_an_optional_length", [this]() {
            check_stdlib_results<std::string>({
                {"var s = \"hello world\"; s.substr(0, 5);", "hello"},
                {"var s = \"hello world\"; s.substr(6);", "world"},
                {"var s = \"hello\"; s.substr(-3);", "llo"},
            });
        });

        // ============================================================
        // Search methods
        // ============================================================

        test("find_rfind_count_and_find_of_locate_text", [this]() {
            check_stdlib_results<script_int>({
                {"var s = \"hello world\"; s.find(\"wor\");", 6},
                {"var s = \"hello world\"; s.find(\"xyz\");", -1},
                {"var s = \"hello hello\"; s.find(\"ell\", 3);", 7},
                {"var s = \"hello hello\"; s.rfind(\"ell\");", 7},
                {"var s = \"abcabcabc\"; s.count(\"abc\");", 3},
                {"var s = \"hello world\"; s.find_first_of(\"aeiou\");", 1},
                {"var s = \"hello world\"; s.find_last_of(\"aeiou\");", 7},
            });
        });

        test("contains_starts_with_and_ends_with_test_for_a_substring", [this]() {
            check_stdlib_results<bool>({
                {"var s = \"hello world\"; s.contains(\"wor\");", true},
                {"var s = \"hello world\"; s.contains(\"xyz\");", false},
                {"var s = \"hello world\"; s.starts_with(\"hello\");", true},
                {"var s = \"hello world\"; s.starts_with(\"world\");", false},
                {"var s = \"hello world\"; s.ends_with(\"world\");", true},
                {"var s = \"hello world\"; s.ends_with(\"hello\");", false},
            });
        });

        // ============================================================
        // Parsing methods
        // ============================================================

        test("to_int_parses_with_a_default_and_a_base", [this]() {
            check_stdlib_results<script_int>({
                {"var s = \"42\"; s.to_int();", 42},
                {"var s = \"not_a_number\"; s.to_int(-1);", -1},
                {"var s = \"FF\"; s.to_int(0, 16);", 255},
            });
        });

        test("to_float_valid", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            auto result = engine->execute("var s = \"3.14\"; s.to_float();");
            check(std::abs(result.as<double>() - 3.14) < 0.001);
        });

        test("to_float_with_default", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            auto result = engine->execute("var s = \"not_a_number\"; s.to_float(-1.0);");
            check_eq(result.as<double>(), -1.0);
        });

        // ============================================================
        // Mutating methods (modify in place, return nothing)
        // ============================================================

        test("to_lower_and_to_upper_change_the_string_in_place", [this]() {
            check_stdlib_results<std::string>({
                {"var s = \"HELLO\"; s.to_lower(); s;", "hello"},
                {"var s = \"hello\"; s.to_upper(); s;", "HELLO"},
            });
        });

        // ============================================================
        // string:: namespace (NON-mutating: returns a new string, argument untouched)
        // ============================================================

        test("string_namespace_case_transforms_return_a_copy", [this]() {
            check_stdlib_results<std::string>({
                {"var s = \"AbC 123!\"; var t = string::to_lower(s); t + \"|\" + s;", "abc 123!|AbC 123!"},
                {"var s = \"AbC 123!\"; var t = string::to_upper(s); t + \"|\" + s;", "ABC 123!|AbC 123!"},
                {"string::to_lower(\"HeLLo\" + \" WoRLD\");", "hello world"},
            });
        });

        test("ns_case_transform_type_errors", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            // Host-function errors surface at the engine boundary (like other stdlib
            // host bindings), not as script-catchable exceptions - assert the error
            // surfaces and the engine stays usable afterwards
            auto expect_error = [&](const char* src) {
                try {
                    auto r = engine->execute(src);
                    check(!r.is_string(), std::string("expected an error result for: ") + src);
                } catch (const std::exception&) {
                    // engine-boundary throw is the other legal surface
                }
            };
            expect_error("string::to_lower(42);");
            expect_error("string::to_upper(\"a\", \"b\");");
            check_eq(engine->execute("string::to_upper(\"ok\");").as<std::string>(), "OK");
        });

        test("trim_strips_whitespace_or_given_characters_in_place", [this]() {
            check_stdlib_results<std::string>({
                {"var s = \"  hello  \"; s.trim(); s;", "hello"},
                {"var s = \"***hello***\"; s.trim(\"*\"); s;", "hello"},
                {"var s = \"  hello  \"; s.trim_left(); s;", "hello  "},
                {"var s = \"  hello  \"; s.trim_right(); s;", "  hello"},
            });
        });

        test("pad_left_and_pad_right_fill_to_a_width", [this]() {
            check_stdlib_results<std::string>({
                {"var s = \"hi\"; s.pad_left(5, '.'); s;", "...hi"},
                {"var s = \"hi\"; s.pad_right(5, '.'); s;", "hi..."},
            });
        });

        test("pad_center", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            auto result = engine->execute("var s = \"hi\"; s.pad_center(6, '.'); s;");
            check_eq(result.as<std::string>(), "..hi..");
            // a fill of several characters tiles from its start on each side (JavaScript's padStart / padEnd)
            check_eq(std::string("xyzxyab|abxyzxy|xyzabxyz|\xC3\xA9\xE6\x97\xA5\xC3\xA9\xE6\x97\xA5" "b"),
                     engine->execute("var l = \"ab\"; l.pad_left(7, \"xyz\"); var r = \"ab\"; r.pad_right(7, \"xyz\"); var c = \"ab\"; c.pad_center(8, \"xyz\");"
                                     " utf8_string u = \"b\"; u.pad_left(5, \"\xC3\xA9\xE6\x97\xA5\"); l + \"|\" + r + \"|\" + c + \"|\" + u;").to_string());
        });

        test("replace_rewrites_the_first_last_or_every_match_in_place", [this]() {
            check_stdlib_results<std::string>({
                {"var s = \"hello hello\"; s.replace_first(\"hello\", \"hi\"); s;", "hi hello"},
                {"var s = \"hello hello\"; s.replace_last(\"hello\", \"hi\"); s;", "hello hi"},
                {"var s = \"hello hello hello\"; s.replace(\"hello\", \"hi\"); s;", "hi hi hi"},
                {"var s = \"aaaa\"; s.replace(\"aa\", \"b\"); s;", "bb"},
                {"var s = \"aaa\"; s.replace(\"a\", \"b\"); s;", "bbb"},
            });
        });

        test("insert_erase_remove_reverse_and_repeat_edit_in_place", [this]() {
            check_stdlib_results<std::string>({
                {"var s = \"helloworld\"; s.insert(5, \" \"); s;", "hello world"},
                {"var s = \"hello world\"; s.erase(5, 1); s;", "helloworld"},
                {"var s = \"hello world\"; s.remove_prefix(6); s;", "world"},
                {"var s = \"hello world\"; s.remove_suffix(6); s;", "hello"},
                {"var s = \"hello\"; s.reverse(); s;", "olleh"},
                {"var s = \"ab\"; s.repeat(3); s;", "ababab"},
            });
        });

        // ============================================================
        // Chaining tests
        // ============================================================

        // a mutator returns nothing, so a chain uses its result: it raises before the first write
        test("chaining_trim_to_lower", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            auto result = engine->execute("var s = \"  HELLO  \"; var r = \"\"; try { s.trim().to_lower(); } catch (e) { r = e.message; } r + \"|\" + s;");
            check_eq(result.as<std::string>(), "trim changes s in place and returns nothing; string::trim(s) returns a trimmed copy|  HELLO  ");
        });

        test("chaining_multiple_operations", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            auto result = engine->execute("var s = \"  Hello World  \"; s.trim(); s.to_lower(); s.replace(\" \", \"_\"); s + \"|\" + string::replace(string::to_upper(s), \"_\", \"-\");");
            check_eq(result.as<std::string>(), "hello_world|HELLO-WORLD");
        });

        // ============================================================
        // Split and Join
        // ============================================================

        test("split_cuts_at_a_delimiter_or_between_characters", [this]() {
            check_stdlib_results<script_int>({
                {"var s = \"a,b,c\"; var arr = s.split(\",\"); arr.length();", 3},
                {"var s = \"abc\"; var arr = s.split(\"\"); arr.length();", 3},
            });
        });

        test("split_without_a_separator_splits_on_whitespace_and_skips_empties", [this]() {
            // Python/Ruby/Go/JS/Java's no-separator split: runs of the string library's whitespace separate pieces, no piece is
            // empty; string::whitespace spells the same; an exact separator keeps empties; "" splits into characters; explode is
            // every form's synonym; a utf8_string keeps its kind and "" splits it into characters
            auto engine = make_engine();
            stdlib::register_all(*engine);
            auto js = [&](const std::string& expr) { return engine->execute("to_json(" + expr + ");").as<std::string>(); };
            check_eq(std::string("[\"stats\",\"35\",\"25\",\"25\"]"), js("\"  stats 35\\t25\\n\\r25  \".split()"));
            check_eq(std::string("[]"), js("\" \\t\\n \".split()"));
            check_eq(std::string("[]"), js("\"\".split()"));
            check_eq(std::string("[\"a\",\"b\",\"c\"]"), js("\"a\\x0Bb\\x0Cc\".split(string::whitespace)"));
            check_eq(std::string("[\"a\",\"b  c \"]"), js("\"  a b  c \".split(string::whitespace, 1)"));
            check_eq(std::string("[\"a\",\"\",\"b\",\"\"]"), js("\"a,,b,\".split(\",\")"));
            check_eq(std::string("[\"a\",\" \",\"b\"]"), js("\"a b\".split(\"\")"));
            check_eq(std::string("[\"x\",\"y\"]"), js("\" x  y \".explode()"));
            check_eq(std::string("[\"a\",\"b\"]"), js("\"a|b\".explode(\"|\")"));
            check_eq(std::string("[\"a\",\"b\"]"), js("\"ab\".explode(\"\")"));
            check_eq(std::string(" \t\n\v\f\r"), engine->execute("string::whitespace;").as<std::string>());
            check_eq(std::string("utf8_string|2|\xC3\xA9t\xC3\xA9"), engine->execute("var p = utf8_string(\"  \xC3\xA9t\xC3\xA9 \\n l\xC3\xA0 \").split(); type_of(p[0]) + \"|\" + to_string(p.size()) + \"|\" + p[0];").as<std::string>());
            check_eq(std::string("3|\xC3\xA9"), engine->execute("var c = utf8_string(\"\xC3\xA9t\xC3\xA9\").explode(\"\"); to_string(c.size()) + \"|\" + c[0];").as<std::string>());
        });

        test("subscript_store_replaces_a_character_with_its_text", [this]() {
            // s[i] = c replaces byte i, s[i] = "text" replaces byte i with the whole text; a utf8_string counts characters;
            // compound stores read, concatenate and write back; the string stays a value (copies never see the write)
            for (bool proofs : {true, false}) {
                auto engine = make_engine();
                stdlib::register_all(*engine);
                if (!proofs) { engine->type_proofs(false); }
                auto es = [&](const std::string& src) { return engine->execute(src).as<std::string>(); };
                auto caught = [&](const std::string& src) { return es("string m = \"\"; try { " + src + " } catch (e) { m = e; } m;"); };
                check_eq(std::string("hallo"), es("string s = \"hello\"; s[1] = 'a'; s;"));
                check_eq(std::string("helllo"), es("string s = \"hello\"; s[2] = \"ll\"; s;"));
                check_eq(std::string("hellllo"), es("string s = \"hello\"; s[2] = `${s[2]}ll`; s;"));
                check_eq(std::string("hellllo"), es("string s = \"hello\"; s[2] += \"ll\"; s;"));
                check_eq(std::string("heo"), es("string s = \"hello\"; s[2] = \"\"; s[2] = \"\"; s;"));
                check_eq(std::string("hello|jello"), es("string s = \"hello\"; string t = s; s[0] = 'j'; `${t}|${s}`;"));
                check_eq(std::string("abc|aXc"), es("var s = \"abc\"; var t = s; t[1] = 'X'; `${s}|${t}`;"));
                check_eq(std::string("abXd|aYY"), es("array<string> a = [\"ab\", \"cd\"]; a[1][0] = 'X'; array<var> v = [\"ab\"]; v[0][1] = \"YY\"; a[0] + a[1] + \"|\" + v[0];"));
                check_eq(std::string("abZ"), es("map<string, string> m = {\"k\": \"abc\"}; m[\"k\"][2] = 'Z'; m[\"k\"];"));
                check_eq(std::string("Bob|Xbc"), es("class P { string name = \"bob\"; } class Q { string n = \"abc\"; void f() { n[0] = 'X'; } } P p = P(); p.name[0] = 'B'; Q q = Q(); q.f(); `${p.name}|${q.n}`;"));
                check_eq(std::string("Word|word|Zbc"), es("string up(string w) { w[0] = 'W'; return w; } void set0(string& w) { w[0] = 'Z'; } string o = \"word\"; string x = \"abc\"; set0(x); `${up(o)}|${o}|${x}`;"));
                check_eq(std::string("xxx|zzz"), es("string f() { string s = \"abc\"; for (int i = 0; i < 3; ++i) { s[i] = 'z'; } return s; } string g = \"abc\"; for (int i = 0; i < g.size(); ++i) { g[i] = 'x'; } `${g}|${f()}`;"));
                check_eq(std::string("char|Q|Qb"), es("string s = \"ab\"; auto r = (s[0] = 'Q'); `${type_of(r)}|${r}|${s}`;"));
                check_eq(std::string("hello"), es("utf8_string u = \"h\xC3\xA9llo\"; u[1] = \"e\"; u;"));
                check_eq(std::string("a\xE6\x97\xA5" "c|3|5"), es("utf8_string u = \"abc\"; u[1] = \"\xE6\x97\xA5\"; u + \"|\" + to_string(u.size()) + \"|\" + to_string(u.byte_size());"));
                check_eq(std::string("a\xC3\xA9xc|zb"), es("utf8_string u = \"a\xC3\xA9" "c\"; u[1] += \"x\"; utf8_string w = \"ab\"; w[0] = 'z'; u + \"|\" + w;"));
                check_eq(std::string("3|\xE8\xAA\x9E|\xE6\x97\xA5" "b\xE8\xAA\x9E|4|\xE6\x9C\xAC"), es("utf8_string u = \"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\"; utf8_string keep = u; int n = u.size(); u[1] = \"b\"; string first = to_string(u.size()) + \"|\" + u[2] + \"|\" + u; keep[0] = \"ab\"; first + \"|\" + to_string(keep.size()) + \"|\" + keep[2];"));
                check_eq(std::string("String index 9 out of bounds for string of size 5"), caught("var s9 = \"hello\"; s9[9] = 'x';"));
                check_eq(std::string("String index 3 out of bounds for string of size 3"), caught("var u3 = utf8_string(\"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\"); u3[3] = \"x\";"));
                check_eq(std::string("Cannot assign int to a string character (it takes a char or a string)"), caught("var si = \"hello\"; si[0] = 5;"));
                check_eq(std::string("Cannot assign int to a string character (it takes a char or a string)"), caught("var sc = \"hi\"; sc[0] += 1;"));
                check_eq(std::string("Cannot assign to rvalue expression"), caught("var gl = [](){ return \"ab\"; }; gl()[0] = 'x';"));
                try {
                    engine->execute("string s = \"hi\"; s[0] = 5;");
                    check_true(false, "a non-text store into a string character is a load error");
                } catch (const std::exception& ex) {
                    check_true(std::string(ex.what()).find("cannot store into 'string': a string character takes a char or a string, got 'int'") != std::string::npos, ex.what());
                }
            }
        });

        test("join_and_split_produce_the_pieces_and_the_text", [this]() {
            check_stdlib_results<std::string>({
                {"var s = \"a,b,c\"; var arr = s.split(\",\"); arr[0];", "a"},
                {"var arr = [\"a\", \"b\", \"c\"]; arr.join(\",\");", "a,b,c"},
                {"var arr = [\"a\", \"b\", \"c\"]; arr.join();", "abc"},
                {"var s = \"hello world\"; s.split(\" \").join(\"-\");", "hello-world"},
            });
        });

        test("template_strings_splice_expressions_and_honor_escapes", [this]() {
            check_stdlib_results<std::string>({
                {R"(auto name = "world"; `hello ${name}`;)", "hello world"},
                {R"(auto x = 10; auto y = 20; `${x} + ${y} = ${x + y}`;)", "10 + 20 = 30"},
                {R"(`just a plain string`;)", "just a plain string"},
                {R"(auto val = 42; `${val}`;)", "42"},
                {R"(`hello\`world`;)", "hello`world"},
                {R"(`price: \$${99}`;)", "price: $99"},
                {"`line1\nline2`;", "line1\nline2"},
                {R"(`\"\'\t\r\\${1}`;)", "\"'\t\r\\1"},
            });
        });

        test("template_string_nested_expressions", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            auto result = engine->execute(R"(auto hp = 80; auto max_hp = 100; `Health: ${hp}/${max_hp} (${hp * 100 / max_hp}%)`;)");
            check_eq(result.as<std::string>(), "Health: 80/100 (80%)");
            // :: in a splice is scope resolution, a format spec starts at a lone ':' (a parenthesized ::name is a group)
            auto scoped = engine->execute(R"(int hp = 7; namespace ns { int hp = 3; } `${::hp} ${ns::hp} ${ (::hp) } ${::hp:3} ${ns::hp:.1f} ${[::hp][0]}`;)");
            check_eq(std::string("7 3 7   7 3.0 7"), scoped.as<std::string>());
        });

        // Splice depth is tracked on LEXED brace tokens: whitespace before a
        // brace used to skip the depth bookkeeping - `${ x }` was a parse error and a
        // space-prefixed '{' swallowed the splice terminator (runaway template scan).
        test("template_string_splice_whitespace", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(engine->execute(R"(var x = 5; `${ x }`;)").as<std::string>(), "5");
            check_eq(engine->execute(R"(var y = 6; `${y }`;)").as<std::string>(), "6");
            check_eq(engine->execute(R"(var z = 7; `${ z}`;)").as<std::string>(), "7");
        });

        test("template_string_splice_map_literal", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(engine->execute(R"(`${ {"k": 1}["k"] }`;)").as<std::string>(), "1");
            check_eq(engine->execute(R"(`${ {"a": {"b": 2}}["a"]["b"] }`;)").as<std::string>(), "2");
        });

        // a template string of two or more splices is an append chain: the vm's index-tree landing took it for a fused
        // binary inside a function and raised an internal error at a map key under a compound op, ++ or --
        test("template_string_keys_under_compound_ops_in_functions", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(std::string(R"({"012":0,"1-2":2,"12":226,"121":-6,"1212":2,"34":4,"a1b2c":-2,"v113":113,"v226":226}|{"12":"ss"}|226|3443)"),
                engine->execute(R"(
                    map<string, int> heat = {"012": 0, "1-2": 0, "12": 0, "121": 0, "1212": 0, "34": 0, "a1b2c": 0};
                    map<string, string> names = {"12": ""};
                    void f(int x, int y) {
                        heat[`${x}${y}`]++;
                        heat[`${x}${y}`] += 2;
                        ++heat[`${x}-${y}`];
                        heat[`a${x}b${y}c`]--;
                        heat[`${x}${y}${x}`] -= 3;
                        heat[`${x:02d}${y}`] *= 1;
                        heat[`${x}${y}`] += 10;
                        heat[`${x}${y}`] = heat[`${x}${y}`] + 100;
                        names[`${x}${y}`] += "s";
                        int v = heat[`${x}${y}`];
                        heat[`v${v}`] = v;
                        string k = `${x}${y}`;
                        heat[k + `${x}${y}`] += 1;
                        heat[`${x + y}${y * 2}`] |= 4;
                    }
                    f(1, 2);
                    f(1, 2);
                    int g(int x, int y) { return heat[`${x}${y}`]; }
                    string h(int x, int y) { return `${x}${y}` + `${y}${x}`; }
                    to_json(heat) + "|" + to_json(names) + "|" + to_string(g(1, 2)) + "|" + h(3, 4);
                )").as<std::string>());
        });

        test("template_string_splice_no_runaway", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            // A following backtick string must stay a SEPARATE template (the broken
            // depth count used to swallow the terminator and chain into it)
            auto result = engine->execute(R"(var a = `${ {"k": 9}["k"] }`; var b = `ok`; a + b;)");
            check_eq(result.as<std::string>(), "9ok");
        });

        test("map_literal_postfix_index", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            // Map literals are primaries now: postfix applies directly
            check_eq((int64_t)1, engine->execute(R"({"k": 1}["k"];)").as_int());
            check_eq((int64_t)2, engine->execute(R"({"a": 1, "b": 2}.size();)").as_int());
        });

        // ============================================================
        // Template-string format specs: ${expr:spec}
        // spec = [[fill]align][width][.precision][f|x|X|b] (docs/site/guide/09-containers-strings.html "Template strings & format specs")
        // ============================================================

        test("template_spec_float_precision", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(engine->execute(R"(var pi = 3.14159; `${pi:.2f}`;)").as<std::string>(), "3.14");
            check_eq(engine->execute(R"(var pi = 3.14159; `${pi:.0f}`;)").as<std::string>(), "3");
            check_eq(engine->execute(R"(var pi = 3.14159; `${pi:.3}`;)").as<std::string>(), "3.142");  // bare .N is fixed
            check_eq(engine->execute(R"(var h = 80.0; `${h:f}`;)").as<std::string>(), "80.000000");    // :f default precision 6
        });

        test("template_spec_width_align_int", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(engine->execute(R"(var gold = 1234; `${gold:6}`;)").as<std::string>(), "  1234");  // numbers right-align
            check_eq(engine->execute(R"(var gold = 1234; `${gold:<6}`;)").as<std::string>(), "1234  ");
            check_eq(engine->execute(R"(var gold = 1234; `${gold:^6}`;)").as<std::string>(), " 1234 ");
        });

        test("template_spec_width_align_string", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(engine->execute(R"(var name = "Bob"; `${name:6}`;)").as<std::string>(), "Bob   ");  // strings left-align
            check_eq(engine->execute(R"(var name = "Bob"; `${name:>6}`;)").as<std::string>(), "   Bob");
            check_eq(engine->execute(R"(var name = "Bob"; `${name:^7}`;)").as<std::string>(), "  Bob  ");
        });

        test("template_spec_fill_char", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(engine->execute(R"(var gold = 1234; `${gold:*>8}`;)").as<std::string>(), "****1234");
            check_eq(engine->execute(R"(var name = "Bob"; `${name:-<6}`;)").as<std::string>(), "Bob---");
            check_eq(engine->execute(R"(var n = 7; `${n:0>4}`;)").as<std::string>(), "0007");  // '0' fill with explicit align
        });

        test("template_spec_hex_binary", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(engine->execute(R"(var n = 255; `${n:x}`;)").as<std::string>(), "ff");
            check_eq(engine->execute(R"(var n = 255; `${n:X}`;)").as<std::string>(), "FF");
            check_eq(engine->execute(R"(var n = 5; `${n:b}`;)").as<std::string>(), "101");
            check_eq(engine->execute(R"(var n = -255; `${n:x}`;)").as<std::string>(), "-ff");
        });

        test("template_spec_int_as_fixed", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(engine->execute(R"(var gold = 1234; `${gold:.1f}`;)").as<std::string>(), "1234.0");
        });

        test("template_spec_width_precision_fill_combo", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(engine->execute(R"(var pi = 3.14159; `${pi:>8.2f}`;)").as<std::string>(), "    3.14");
            check_eq(engine->execute(R"(var pi = 3.14159; `${pi:*^9.1f}`;)").as<std::string>(), "***3.1***");
        });

        test("template_spec_char_bool", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(engine->execute(R"(var c = 'a'; `${c:3}`;)").as<std::string>(), "a  ");
            check_eq(engine->execute(R"(var f = true; `${f:6}`;)").as<std::string>(), "true  ");
        });

        // The ambiguity rule: a ternary's top-level ':' closes its '?' and stays part of
        // the expression; only a top-level ':' that closes NO ternary starts a spec.
        test("template_spec_ternary_still_expression", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(engine->execute(R"(var a = true; `${a ? 1 : 2}`;)").as<std::string>(), "1");
            check_eq(engine->execute(R"(var a = false; var b = true; `${a ? 1 : b ? 2 : 3}`;)").as<std::string>(), "2");
            check_eq(engine->execute(R"(var a = true; `${(a ? 1 : 2):3}`;)").as<std::string>(), "  1");  // spec after parenthesized ternary
        });

        test("template_spec_after_subscript", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(engine->execute(R"(var m = {"k": 42}; `${m["k"]:6}`;)").as<std::string>(), "    42");
            check_eq(engine->execute(R"(`${ {"k": 3}["k"]:4}`;)").as<std::string>(), "   3");  // map literal's ':' is nested
        });

        test("template_spec_nested_template", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            check_eq(engine->execute(R"(var pi = 3.14159; `${`${pi:.1f}`:>8}`;)").as<std::string>(), "     3.1");
        });

        test("template_spec_core_no_stdlib", [this]() {
            // Engine-core feature: works without stdlib::register_all
            auto engine = make_engine();
            check_eq(engine->execute(R"(var x = 3.5; `${x:.1f}`;)").as<std::string>(), "3.5");
            check_eq(engine->execute(R"(format_value(255, "X");)").as<std::string>(), "FF");  // desugar target, directly callable
        });

        test("template_spec_bound_global", [this]() {
            auto engine = make_engine();
            float hp = 87.5f;
            engine->add_global_ref("hp", hp);
            check_eq(engine->execute(R"(`${hp:>7.1f}`;)").as<std::string>(), "   87.5");
        });

        test("template_spec_malformed_is_lex_error", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            auto expect_lex_error = [&](const std::string& source, const std::string& fragment) {
                try {
                    engine->execute(source);
                    check(false, "expected a parse error for: " + source);
                } catch (const std::exception& e) {
                    check(std::string(e.what()).find(fragment) != std::string::npos,
                          "error for '" + source + "' should mention \"" + fragment + "\", got: " + e.what());
                }
            };
            expect_lex_error(R"(var x = 1; `${x:q}`;)", "unsupported format spec ':q' in template string");
            expect_lex_error(R"(var x = 1; `${x:}`;)", "unsupported format spec ':' in template string");
            expect_lex_error(R"(var x = 1; `${x:#x}`;)", "unsupported format spec ':#x' in template string");  // no alternate form
            expect_lex_error(R"(var x = 1; `${x:.f}`;)", "unsupported format spec ':.f' in template string");
            expect_lex_error(R"(var x = 1; `${x:5.}`;)", "unsupported format spec ':5.' in template string");
            expect_lex_error(R"(var x = 1; `${x:-5}`;)", "unsupported format spec ':-5' in template string");  // '+' and ' ' are the signs
        });

        test("template_spec_type_mismatch_is_runtime_error", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            // Catchable in-script, with identical text on both backends (one shared kernel)
            check_eq(engine->execute(R"(var name = "Bob"; var msg = ""; try { `${name:.2f}`; } catch (e) { msg = e; } msg;)").as<std::string>(),
                     "format spec ':.2f' does not apply to string value");
            check_eq(engine->execute(R"(var f = true; var msg = ""; try { `${f:x}`; } catch (e) { msg = e; } msg;)").as<std::string>(),
                     "format spec ':x' does not apply to bool value");
            check_eq(engine->execute(R"(var pi = 3.5; var msg = ""; try { `${pi:b}`; } catch (e) { msg = e; } msg;)").as<std::string>(),
                     "format spec ':b' does not apply to float value");
        });

        test("template_spec_hud_line", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            auto result = engine->execute(R"(
                var hp = 73.5; var maxhp = 100.0; var gold = 1234; var name = "Grubwell";
                `HP ${hp:>6.1f}/${maxhp:<6.1f} G${gold:>7} ${name:<12}|`;
            )");
            check_eq(result.as<std::string>(), "HP   73.5/100.0  G   1234 Grubwell    |");
        });

        // Crash regression: a subscript read in argument position mints an element
        // reference; string builtins gate on is_string() (derefs) then read
        // unchecked_as_string() (doesn't) - s.find(parts[0]) was an access violation
        // on BOTH backends before deref_builtin_args (builtin_methods.hpp). Covers the
        // identifier-receiver direct path AND the minted bound-method path.
        test("subscript_arg_to_string_builtins", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            auto result = engine->execute(R"(
                var s = "the quick brown fox";
                var parts = ["quick", "fox", "wolf"];
                s.find(parts[0]) == 4 &&
                s.count(parts[1]) == 1 &&
                s.find(parts[2]) == -1 &&
                ("prefix " + s).contains(parts[0]) &&
                s.starts_with(["the", "a"][0]);
            )");
            check_eq(result.as<bool>(), true);
        });

        // ============================================================
        // UTF-8 characters, everyday operations, the string:: namespace, format pieces
        // ============================================================

        // each helper reads the result while its engine is alive (a value holds its engine)
        auto eval = [this](const char* src) { auto engine = make_engine(); stdlib::register_all(*engine); engine->execute(src); };
        auto es = [this](const char* src) { auto engine = make_engine(); stdlib::register_all(*engine); return engine->execute(src).as<std::string>(); };
        auto ei = [this](const char* src) { auto engine = make_engine(); stdlib::register_all(*engine); return engine->execute(src).as<script_int>(); };
        auto eb = [this](const char* src) { auto engine = make_engine(); stdlib::register_all(*engine); return engine->execute(src).as<bool>(); };

        test("utf8_chars_walk_characters_and_keep_bad_bytes", [this, es, ei]() {
            check_eq(std::string("[h][\xC3\xA9][\xF0\x9F\x99\x82]"), es("string acc = \"\"; for (string ch : \"h\xC3\xA9\xF0\x9F\x99\x82\".utf8_chars()) { acc += \"[\" + ch + \"]\"; } acc;"));
            check_eq(script_int(3), ei("(\"a\" + \"\\xff\" + \"b\").utf8_chars().size();"));   // an invalid byte is its own element
            check_eq(script_int(3), ei("\"h\xC3\xA9\xF0\x9F\x99\x82\".utf8_length();"));
            check_eq(script_int(7), ei("\"h\xC3\xA9\xF0\x9F\x99\x82\".size();"));   // size stays bytes
            check_eq(script_int(7), ei("\"h\xC3\xA9\xF0\x9F\x99\x82\".byte_size();"));
        });

        test("utf8_validity_code_and_from_code", [this, eval, es, ei, eb]() {
            check_eq(true, eb("\"h\xC3\xA9\".is_valid_utf8();"));
            check_eq(false, eb("(\"a\" + \"\\xff\").is_valid_utf8();"));
            check_eq(false, eb("string::is_valid_utf8(\"\\xc3\");"));   // a truncated sequence
            check_eq(script_int(233), ei("\"\xC3\xA9\".code();"));
            check_eq(std::string("\xC3\xA9\xF0\x9F\x99\x82"), es("string::from_code(233) + string::from_code(128578);"));
            check_throws([&]() { eval("\"ab\".code();"); });
            check_throws([&]() { eval("string::from_code(55296);"); });   // a surrogate
        });

        test("lines_partition_compare_and_classes", [this, es, ei, eb]() {
            check_eq(std::string("a|b|c"), es("\"a\\nb\\r\\nc\\n\".lines().join(\"|\");"));
            check_eq(std::string("key|=|val=x"), es("\"key=val=x\".partition(\"=\").join(\"|\");"));
            check_eq(std::string("nope||"), es("\"nope\".partition(\"=\").join(\"|\");"));
            check_eq(script_int(-1), ei("\"abc\".compare(\"abd\");"));
            check_eq(script_int(0), ei("\"x\".compare(\"x\");"));
            check_eq(true, eb("\"123\".is_digit() && \"Ab\".is_alpha() && \"a1\".is_alnum() && \" \\t\".is_space() && \"AB\".is_upper() && \"ab\".is_lower();"));
            check_eq(false, eb("\"\".is_digit() || \"1a\".is_digit();"));
            check_eq(std::string("a|b|c,d"), es("\"a,b,c,d\".split(\",\", 2).join(\"|\");"));
        });

        test("string_namespace_free_functions", [this, eval, es]() {
            check_eq(std::string("a-b-c"), es("string::join([\"a\", \"b\", \"c\"], \"-\");"));
            check_eq(std::string("[hi][  hi  ]"), es("string t = \"  hi  \"; \"[\" + string::trim(t) + \"][\" + t + \"]\";"));   // the argument is untouched
            check_eq(std::string("a+b+c"), es("string::replace(\"a-b-c\", \"-\", \"+\");"));
            check_eq(std::string("ababab"), es("string::repeat(\"ab\", 3);"));
            check_eq(std::string("007x.."), es("string::pad_left(\"7\", 3, \"0\") + string::pad_right(\"x\", 3, '.');"));
            check_eq(std::string("HI!"), es("namespace string { function shout(string s) -> string { return string::to_upper(s) + \"!\"; } } string::shout(\"hi\");"));   // the namespace stays script-extensible
            check_throws([&]() { eval("string::nope(\"x\");"); });
        });

        test("format_sign_zero_and_float_types", [this, eval, es]() {
            check_eq(std::string("[00042] [-0042] [+7] [+3.5] [1.234500e+03] [0.00012] [255] [ 42] [0003.142]"),
                es("`[${42:05}] [${-42:05}] [${7:+}] [${3.5:+.1f}] [${1234.5:e}] [${0.00012:g}] [${255:d}] [${42: }] [${3.14159:08.3f}]`;"));
            check_eq(std::string("    ab|003.14|+5"), es("format(\"{0:>6}|{1:06.2f}|{2:+d}\", \"ab\", 3.14159, 5);"));
            check_eq(std::string("  x00"), es("`${\"x00\":>5}`;"));   // an explicit align on a string keeps its fill
            check_throws([&]() { eval("`${\"s\":05}`;"); });   // zero padding is numeric only
        });

        test("utf8_string_counts_characters", [this, es, ei, eb]() {
            check_eq(script_int(4), ei("utf8_string u = \"a\xC3\xB1\xE6\x97\xA5" "b\"; u.size();"));
            check_eq(script_int(4), ei("utf8_string u = \"a\xC3\xB1\xE6\x97\xA5" "b\"; u.length();"));
            check_eq(script_int(7), ei("utf8_string u = \"a\xC3\xB1\xE6\x97\xA5" "b\"; u.byte_size();"));
            check_eq(std::string("\xE6\x97\xA5"), es("utf8_string u = \"a\xC3\xB1\xE6\x97\xA5" "b\"; u[2];"));
            check_eq(std::string("utf8_string"), es("utf8_string u = \"a\xC3\xB1\xE6\x97\xA5" "b\"; type_of(u[2]);"));
            check_eq(std::string("b|a|\xC3\xB1\xE6\x97\xA5" "b|\xC3\xB1\xE6\x97\xA5"), es("utf8_string u = \"a\xC3\xB1\xE6\x97\xA5" "b\"; u.at(-1) + \"|\" + u.front() + \"|\" + u.substr(1) + \"|\" + u.substr(1, 2);"));
            check_eq(std::string("2|3|-1|true|true"), es("utf8_string u = \"a\xC3\xB1\xE6\x97\xA5" "b\"; to_string(u.find(\"\xE6\x97\xA5\")) + \"|\" + to_string(u.rfind(\"b\")) + \"|\" + to_string(u.find(\"z\")) + \"|\" + to_string(u.contains(\"\xC3\xB1\")) + \"|\" + to_string(u.starts_with(\"a\xC3\xB1\"));"));
            check_eq(std::string("b\xE6\x97\xA5\xC3\xB1" "a|4|aZ\xC3\xB1\xE6\x97\xA5" "b"), es("utf8_string u = \"a\xC3\xB1\xE6\x97\xA5" "b\"; utf8_string r = u; r.reverse(); utf8_string z = u; z.insert(1, \"Z\"); r + \"|\" + to_string(u.split(\"\").size()) + \"|\" + z;"));
            check_eq(std::string("\xC3\xA9\xC3\xA9\xE6\x97\xA5|\xE6\x97\xA5\xC3\xA9\xC3\xA9"), es("utf8_string a = \"\xE6\x97\xA5\"; a.pad_left(3, \"\xC3\xA9\"); utf8_string b = \"\xE6\x97\xA5\"; b.pad_right(3, \"\xC3\xA9\"); a + \"|\" + b;"));
            check_eq(std::string("utf8_string|utf8_string|utf8_string"), es("utf8_string u = \"a b\"; type_of(u.split(\" \")[0]) + \"|\" + type_of(string::to_upper(u)) + \"|\" + type_of(u.substr(0, 1));"));
            check_eq(std::string("H\xC3\xA9LLO"), es("utf8_string u = \"h\xC3\xA9llo\"; u.to_upper(); u;"));
        });

        test("utf8_string_operators_and_slots", [this, es, ei, eb]() {
            check_eq(std::string("utf8_string|utf8_string|utf8_string|string"), es("utf8_string u = \"\xC3\xA9\"; type_of(u + \"x\") + \"|\" + type_of(\"x\" + u) + \"|\" + type_of(u + \"5\") + \"|\" + type_of(\"x\" + \"y\");"));
            check_eq(true, eb("utf8_string u = \"a\xC3\xB1\"; u == \"a\xC3\xB1\" && \"a\xC3\xB1\" == u && !(u != \"a\xC3\xB1\") && u < \"b\" && \"\xC3\xA9\" > u;"));
            check_eq(true, eb("utf8_string u = \"\"; utf8_string v = \"x\"; !u && v;"));
            check_eq(std::string("string|3|utf8_string|2"), es("utf8_string u = \"a\xC3\xB1\"; string s = u; utf8_string back = s; type_of(s) + \"|\" + to_string(s.size()) + \"|\" + type_of(back) + \"|\" + to_string(back.size());"));
            check_eq(std::string("utf8_string|12"), es("utf8_string n = to_string(12); `${type_of(n)}|${n}`;"));
            check_eq(script_int(2), ei("function f(utf8_string t) -> utf8_string { return t + \"x\"; } f(\"\xC3\xBC\").size();"));
            check_eq(std::string("utf8_string|utf8_string|2|3"), es("var w = utf8_string(\"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\"); class H { utf8_string t = \"\xC3\xBC\"; } H h = H(); h.t += \"x\"; array<var> a = [w]; a[0] += \"!\"; type_of(h.t) + \"|\" + type_of(a[0]) + \"|\" + to_string(h.t.size()) + \"|\" + to_string(w.size());"));
            check_eq(std::string("utf8_string|string|utf8_string"), es("type_of(string::join([\"a\".to_utf8(), \"b\"], \"-\")) + \"|\" + type_of(string::join([\"a\", \"b\"], \"-\")) + \"|\" + type_of(string::trim(\"  \xC3\xBC \".to_utf8()));"));
            check_eq(script_int(2), ei("map<var, int> m = {}; m[\"a\xC3\xB1\".to_utf8()] = 1; m[\"a\xC3\xB1\"] = 2; m[utf8_string(\"a\xC3\xB1\")] = 3; m.size();"));
            check_eq(std::string("[\"\xC3\xA9\",\"x\"]"), es("to_json([\"\xC3\xA9\".to_utf8(), \"x\"]);"));
            check_eq(std::string("utf8_string|3"), es("var r = from_binary(to_binary(\"a\xC3\xB1\".to_utf8())); type_of(r) + \"|\" + to_string(r.byte_size());"));
        });

        test("utf8_string_iteration_and_errors", [this, eval, es, ei]() {
            check_eq(std::string("[a][\xC3\xB1][\xE6\x97\xA5][b]"), es("utf8_string u = \"a\xC3\xB1\xE6\x97\xA5" "b\"; string out = \"\"; for (auto c : u) { out += \"[\" + c + \"]\"; } out;"));
            check_eq(script_int(7), ei("utf8_string u = \"a\xC3\xB1\xE6\x97\xA5" "b\"; int n = 0; for (int i = 0; i < u.size(); ++i) { n += u[i].byte_size(); } n;"));
            check_eq(std::string("a\xC3\xB1\xE6\x97\xA5" "b|aZ\xC3\xB1\xE6\x97\xA5" "b"), es("utf8_string u = \"a\xC3\xB1\xE6\x97\xA5" "b\"; var c = u; c.insert(1, \"Z\"); u + \"|\" + c;"));
            check_eq(std::string("String index 4 out of bounds for string of size 4"), es("string m = \"\"; try { utf8_string u = \"a\xC3\xB1\xE6\x97\xA5" "b\"; u[4]; } catch (e) { m = e; } m;"));
            check_eq(std::string("Cannot iterate a utf8_string by reference: its characters are not lvalues (use for (auto c : s))"), es("string m = \"\"; try { utf8_string u = \"ab\"; for (auto& c : u) {} } catch (e) { m = e; } m;"));
            check_eq(std::string("xb"), es("utf8_string u = \"ab\"; u[0] = \"x\"; u;"));
            check_eq(std::string("utf8_string has no member 'to_utf8'"), es("string m = \"\"; try { var u = utf8_string(\"ab\"); u.to_utf8(); } catch (e) { m = e; } m;"));
        });

        test("utf8_string_positions_over_a_long_text", [this, ei]() {
            check_eq(script_int(0), ei("string s = \"\"; for (int i = 0; i < 1000; ++i) { s += \"a\xC3\xA9\xE6\x97\xA5\"; } utf8_string u = s; array<var> ch = s.utf8_chars(); int bad = 0; for (int k = 0; k < 3000; k += 7) { int i = (k * 1237) % 3000; if (u[i] != ch[i]) { ++bad; } } for (int i = 2999; i >= 0; i -= 3) { if (u[i] != ch[i]) { ++bad; } } u.insert(0, \"\xE6\x97\xA5\"); if (u[1] != \"a\" || u.size() != 3001 || u[3000] != \"\xE6\x97\xA5\") { ++bad; } bad;"));
        });

        test("utf8_string_declarations_survive_the_jaibite_cache", [this]() {
            namespace fs = std::filesystem;
            fs::path dir = jai::foundry::test_temp_directory() / "jai_utf8_bite";
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::create_directories(dir);
            std::ofstream(dir / "main.jai") << "utf8_string u = \"a\xC3\xB1\"; var n = u.size(); var k = type_of(u);\n";
            for (int run = 0; run < 2; ++run) {   // the first run writes the sibling, the second loads it
                auto engine = make_engine(); stdlib::register_all(*engine);
                engine->execute_file((dir / "main.jai").string());
                check_eq(script_int(2), engine->execute("n;").as<script_int>());
                check_eq(std::string("utf8_string"), engine->execute("k;").as<std::string>());
                check_true(fs::exists(dir / ".cache" / "main.jai.jaibite"));
            }
            fs::remove_all(dir, ec);
        });

        test("proven_builtin_calls_keep_the_general_semantics", [this, es]() {
            // the builtin-call instruction's inline paths and its fallbacks: results, error texts, argument kinds, the receiver
            // re-read after a reassigning argument
            check_eq(std::string("4|-1|17|true|false|true|true|uick|ick brown fox|q|f|3|1|true|7"), es(R"(
                string s = "the quick brown fox";
                string out = `${s.find("quick")}|${s.find("zz")}|${s.find("o", 13)}|${s.contains("fox")}|${s.contains("cat")}|${s.starts_with("the")}|${s.ends_with("fox")}|${s.substr(5, 4)}|${s.substr(-13)}|${s.at(4)}|${s.at(-3)}`;
                map<string, int> m = {"a": 3};
                array<int> xs = [];
                out = `${out}|${m.get("a", 9)}|${m.size()}|${m.has("a")}|${(s.size() - 12)}`;
                out;)"));
            check_eq(std::string("find() first argument must be a string|at() index out of bounds|substr() start argument must be an integer|05"), es(R"(
                string s = "hello"; var n = 3; var t = "x"; string out = "";
                try { s.find(n); } catch (e) { out = out + e; }
                try { s.at(9); } catch (e) { out = `${out}|${e}`; }
                try { s.substr(t); } catch (e) { out = `${out}|${e}`; }
                out = `${out}|${s.substr(99).size()}${s.size()}`;
                out;)"));
            check_eq(std::string("3"), es(R"(
                string s = "abc";
                function swap() -> string { s = "xyzw"; return "w"; }
                `${s.find(swap())}`;)"));
        });

        test("template_chains_append_in_order", [this, es]() {
            check_eq(std::string("a1b2.5ctruedx|AB:A,B|utf8_string|3|string"), es(R"(
                string trail = "";
                class P { string tag; P(string t) { tag = t; } function to_string() -> string { trail = trail + tag; return tag; } }
                int i = 1; float f = 2.5; bool b = true; char c = 'x';
                string first = `a${i}b${f}c${b}d${c}`;
                string order = `${P("A")},${P("B")}`;
                var u = `${"é".to_utf8()}!`;
                string typed = "q"; string joined = `${typed}${typed}${typed}`;
                `${first}|${trail}:${order}|${type_of(u)}|${joined.size()}|${type_of(joined)}`;)"));
        });

        test("floats_print_shortest_and_round_trip", [this, es, eb]() {
            check_eq(std::string("0.30000000000000004|2.0|0.3333333333333333|1e+20|-0.0|1e-07|123456789.0"),
                es("`${to_string(0.1 + 0.2)}|${to_string(2.0)}|${to_string(1.0 / 3.0)}|${to_string(1e20)}|${to_string(-0.0)}|${to_string(1e-7)}|${to_string(123456789.0)}`;"));
            check_eq(std::string("x2.5 [1.5] 1.0,2.5"), es("array<float> a = [1.0, 2.5]; \"x\" + to_string(2.5) + ` [${1.5}] ` + a.join(\",\");"));
            check_eq(true, eb("array<float> v = [0.1, 0.1 + 0.2, 1.0 / 3.0, 2.0, 1e20, 1e-300, 5e-324, 1.7976931348623157e308]; bool ok = true; "
                              "for (int i = 0; i < v.size(); ++i) { float x = v[i]; float t = to_string(x).to_float(); var j = from_json(to_json([x])); float y = j[0]; if (t != x || y != x) { ok = false; } } ok;"));
        });

        test("subnormals_parse_and_only_zero_or_infinite_results_are_out_of_range", [this, es, eb]() {
            // glibc and MinGW's CRT flag every subnormal strtod result ERANGE (MSVC's does not): the verdict is the value's
            check_eq(true, eb("1e-310 > 0.0 && 5e-324 > 0.0 && 5e-324 < 1e-323;"));
            check_eq(std::string("5e-324|1e-310|-1.0|-1.0|-1.0"), es(
                "to_string(\"4.9e-324\".to_float(-1.0)) + \"|\" + to_string(\" 1e-310 \".to_float(-1.0)) + \"|\" + "
                "to_string(\"1e-400\".to_float(-1.0)) + \"|\" + to_string(\"1e999\".to_float(-1.0)) + \"|\" + to_string(\"x\".to_float(-1.0));"));
            check_throws([this]() { auto engine = make_engine(); engine->execute("float f = 1e999;"); });
            check_throws([this]() { auto engine = make_engine(); engine->execute("float f = 1e-400;"); });
        });

        test("raw_strings_hold_their_text_as_written", [this, es, ei]() {
            check_eq(std::string("C:\\games\\save.dat"), es(R"JAI(R"(C:\games\save.dat)";)JAI"));
            check_eq(std::string("\\d+\\.\\d*"), es(R"JAI(R"(\d+\.\d*)";)JAI"));
            check_eq(std::string("say(\"hi\")"), es(R"JAI(R"js(say("hi"))js";)JAI"));
            check_eq(std::string("a)\"b"), es(R"JAI(R"x(a)"b)x";)JAI"));
            check_eq(std::string(""), es(R"JAI(R"()";)JAI"));
            check_eq(std::string("one\ntwo\nthree"), es("R\"(one\r\ntwo\rthree)\";"));   // a line ending inside is one \n
            check_eq(std::string("x\\ny"), es(R"JAI("x" + R"(\n)" + "y";)JAI"));
            check_eq(script_int(6), ei("var R = 5; R + 1;"));   // R before anything but a quote is a name
        });

        test("raw_string_errors_name_the_problem", [this]() {
            auto parse_text = [this](const std::string& src) {
                auto engine = make_engine();
                try { engine->execute(src); } catch (const std::exception& e) { return std::string(e.what()); }
                return std::string("no error");
            };
            auto check_contains = [this](const std::string& text, const std::string& part) { check_true(text.find(part) != std::string::npos, text); };
            check_contains(parse_text("string s = R\"(a\nb\nc)\";\nint x = \"no\";"), ":4:");   // the lines inside count
            check_contains(parse_text("R\"(abc"), "unterminated raw string literal: no )\" closes it");
            check_contains(parse_text("R\"ab(abc)a\""), "unterminated raw string literal: no )ab\" closes it");
            check_contains(parse_text("R\"a b(x)a b\""), "invalid raw string delimiter: up to 16 characters before '(', none a space, parenthesis, backslash or quote");
            check_contains(parse_text("R\"12345678901234567(x)12345678901234567\""), "invalid raw string delimiter");
            check_contains(parse_text("R\"abc"), "unterminated raw string literal");
        });
    }
};

} // namespace jai::foundry::tests

// Auto-register with the test framework
FOUNDRY_REGISTER(jai::foundry::tests::string_stdlib_tests)
