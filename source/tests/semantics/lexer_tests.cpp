#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/jaiscript_fwd.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"
#include <sstream>
#include <fstream>
#include <filesystem>
#include <vector>

using namespace jai::foundry;

namespace jai::foundry::tests {

class lexer_tests : public suite {
public:
    lexer_tests() : suite("Lexer Tests") {}

    void forge_tests() override {
        test("each_lexeme_lexes_to_its_token_and_input_ends_in_eof", [this]() {
            struct token_row { const char* code; std::vector<token_type> types; };
            const token_row rows[] = {
                {"", {}},
                {"foo", {token_type::identifier}},
                {"int float bool class", {token_type::int_keyword, token_type::float_keyword, token_type::bool_keyword, token_type::class_keyword}},
                {"true false", {token_type::true_keyword, token_type::false_keyword}},
                {"42 0 999", {token_type::integer_literal, token_type::integer_literal, token_type::integer_literal}},
                {R"("hello" "world" "hello\nworld")", {token_type::string_literal, token_type::string_literal, token_type::string_literal}},
                {"'a' '\\n' '0'", {token_type::char_literal, token_type::char_literal, token_type::char_literal}},
                {"+ - * / % == != < > <= >= && || !", {token_type::plus, token_type::minus, token_type::star, token_type::slash, token_type::percent,
                    token_type::equal_equal, token_type::bang_equal, token_type::less, token_type::greater, token_type::less_equal,
                    token_type::greater_equal, token_type::ampersand_ampersand, token_type::pipe_pipe, token_type::bang}},
                {"+= -= *= /= %=", {token_type::plus_equal, token_type::minus_equal, token_type::star_equal, token_type::slash_equal, token_type::percent_equal}},
                {"( ) { } [ ] ; ,", {token_type::left_paren, token_type::right_paren, token_type::left_brace, token_type::right_brace,
                    token_type::left_bracket, token_type::right_bracket, token_type::semicolon, token_type::comma}},
            };
            check_each(rows, [&](const token_row& row) {
                string_symbolizer symbolizer;
                lexer lex(row.code, &symbolizer, "test.jai");
                auto tokens = lex.tokenize();
                check_eq(row.types.size() + 1, tokens.size(), "token count");
                for (size_t i = 0; i < row.types.size(); ++i) {
                    check_eq(row.types[i], tokens[i].type, std::string(tokens[i].lexeme));
                }
                check_eq(token_type::eof, tokens.back().type);
            });
        });

        test("literal_tokens_carry_their_values", [this]() {
            string_symbolizer symbolizer;
            lexer name("foo", &symbolizer, "test.jai");
            check_eq(std::string("foo"), std::string(name.tokenize()[0].lexeme));

            lexer ints("42 0 999", &symbolizer, "test.jai");
            const auto int_tokens = ints.tokenize();
            check_eq(42, int_tokens[0].int_value);
            check_eq(0, int_tokens[1].int_value);
            check_eq(999, int_tokens[2].int_value);

            lexer strings(R"("hello" "world" "hello\nworld")", &symbolizer, "test.jai");
            const auto string_tokens = strings.tokenize();
            check_eq(std::string("hello"), string_tokens[0].string_value);
            check_eq(std::string("world"), string_tokens[1].string_value);
            check_eq(std::string("hello\nworld"), string_tokens[2].string_value);

            lexer chars("'a' '\\n' '0'", &symbolizer, "test.jai");
            const auto char_tokens = chars.tokenize();
            check_eq('a', char_tokens[0].char_value);
            check_eq('\n', char_tokens[1].char_value);
            check_eq('0', char_tokens[2].char_value);
        });

        test("float_literals", [this]() {
            string_symbolizer symbolizer;
            lexer lex("3.14 0.0 42.0", &symbolizer, "test.jai");
            auto tokens = lex.tokenize();
            check_eq(std::size_t(4), tokens.size());
            check_eq(token_type::float_literal, tokens[0].type);
            check_near(3.14, tokens[0].float_value, 0.001);
            check_eq(token_type::float_literal, tokens[1].type);
            check_near(0.0, tokens[1].float_value, 0.001);
            check_eq(token_type::float_literal, tokens[2].type);
            check_near(42.0, tokens[2].float_value, 0.001);
            // C++'s floating literal: the fraction's digits are optional around the '.', and so is the exponent's sign
            lexer cpp("1.e5 1.E+5 2.e-1 1. .5 10.;", &symbolizer, "test.jai");
            const auto forms = cpp.tokenize();
            check_eq(std::size_t(8), forms.size());
            const double values[] = {1e5, 1e5, 0.2, 1.0, 0.5, 10.0};
            for (size_t i = 0; i < 6; ++i) {
                check_eq(token_type::float_literal, forms[i].type, std::string(forms[i].lexeme));
                check_near(values[i], forms[i].float_value, 1e-9);
            }
            // a '.' before a name stays a member access, and ?.5 is a ternary over .5
            lexer member("1.size c?.5:x", &symbolizer, "test.jai");
            const auto rest = member.tokenize();
            check_eq(token_type::integer_literal, rest[0].type);
            check_eq(token_type::dot, rest[1].type);
            check_eq(token_type::identifier, rest[2].type);
            check_eq(token_type::question, rest[4].type);
            check_eq(token_type::float_literal, rest[5].type);
        });

        test("utf8_bom_stripped_at_position_zero", [this]() {
            // A leading UTF-8 BOM is invisible: the same tokens as the un-BOM'd source,
            // and the first token still reports line 1, column 1.
            string_symbolizer symbolizer;
            lexer lex("\xEF\xBB\xBF" "foo + 1", &symbolizer, "test.jai");
            auto tokens = lex.tokenize();
            check_eq(std::size_t(4), tokens.size());
            check_eq(token_type::identifier, tokens[0].type);
            check_eq(std::string("foo"), std::string(tokens[0].lexeme));
            check_eq(std::size_t(1), tokens[0].location.line);
            check_eq(std::size_t(1), tokens[0].location.column);
            check_eq(token_type::plus, tokens[1].type);
            check_eq(token_type::integer_literal, tokens[2].type);
        });

        test("utf8_bom_only_stripped_mid_file_is_not", [this]() {
            // The strip is position-0 only: a BOM byte sequence later in the source is a
            // real (exotic) character and must NOT be silently swallowed.
            string_symbolizer symbolizer;
            lexer lex("a\xEF\xBB\xBF", &symbolizer, "test.jai");
            auto tokens = lex.tokenize();
            check_eq(token_type::identifier, tokens[0].type);
            check_eq(std::string("a"), std::string(tokens[0].lexeme));
            check_eq(token_type::error, tokens[1].type);   // the mid-file 0xEF is unexpected
        });

        test("lexer_errors_name_the_token_and_point_at_its_start", [this]() {
            string_symbolizer symbolizer;
            auto first_error = [&](const std::string& source) {
                lexer lex(source, &symbolizer, "test.jai");
                for (const auto& t : lex.tokenize()) {
                    if (t.type == token_type::error) { return t; }
                }
                return token(token_type::eof, "", source_location{});
            };
            const token comment = first_error("int a = 1;\n  /* never closed\nint b = 2;");
            check_eq(std::string("unterminated block comment: no */ closes it"), std::string(comment.lexeme));
            check_eq(std::size_t(2), comment.location.line);
            check_eq(std::size_t(3), comment.location.column);
            const token number = first_error("var x = 99999999999999999999;");
            check_eq(std::string("number literal out of range: '99999999999999999999'"), std::string(number.lexeme));
            check_eq(std::size_t(9), number.location.column);
            check_eq(std::string("invalid number literal: exponent has no digits in '1.e'"), std::string(first_error("var f = 1.e;").lexeme));
            check_eq(std::string("a character literal holds one character: write text in double quotes"), std::string(first_error("var c = 'ab';").lexeme));
            check_eq(std::string("unterminated character literal"), std::string(first_error("var c = 'a").lexeme));
        });

        // Lua's #xs is no length operator here: '#' begins only #include, #import and a first-line #! shebang
        test("hash_is_a_lex_error_outside_include_import_and_shebang", [this]() {
            string_symbolizer symbolizer;
            auto first_error = [&](const std::string& source) {
                lexer lex(source, &symbolizer, "test.jai");
                for (const auto& t : lex.tokenize()) {
                    if (t.type == token_type::error) { return t; }
                }
                return token(token_type::eof, "", source_location{});
            };
            const token length = first_error("var xs = [1];\nvar n = #xs;");
            check_eq(std::string("unexpected '#' before 'xs': JaiScript has no length operator; write xs.size()"), std::string(length.lexeme));
            check_eq(std::size_t(2), length.location.line);
            check_eq(std::size_t(9), length.location.column);
            check_eq(std::string("unexpected '#': it begins only #include, #import and a first-line #! shebang"), std::string(first_error("var n = 1 # 2;").lexeme));
            check_eq(std::string("unexpected '#': it begins only #include, #import and a first-line #! shebang"), std::string(first_error("var n = 1;\n#!not first").lexeme));
            check_eq(token_type::eof, first_error("#!/usr/bin/env jaiscript\nvar n = 1;").type);
            check_eq(token_type::eof, first_error("#include \"x.jai\"\n# import \"y.jai\";").type);
            auto engine = make_engine();
            check_eq((int64_t)3, engine->execute("#!/usr/bin/env jaiscript\n1 + 2").as_int());
        });

        test("a_utf16_bom_is_a_clear_encoding_error", [this]() {
            const char* rows[][1] = {{"\xFF\xFE" "foo"}, {"\xFE\xFF" "foo"}};   // little- and big-endian
            for (const auto& row : rows) {
                string_symbolizer symbolizer;
                lexer lex(row[0], &symbolizer, "test.jai");
                auto tokens = lex.tokenize();
                check_eq(token_type::error, tokens[0].type);
                check_eq(std::string("UTF-16 encoding is not supported - save the file as UTF-8"), std::string(tokens[0].lexeme));
                check_eq(std::size_t(1), tokens[0].location.line);
                check_eq(std::size_t(1), tokens[0].location.column);
                // Rest of the file is consumed: the error is followed by eof, not more errors.
                check_eq(token_type::eof, tokens[1].type);
            }
        });

        test("cr_only_and_crlf_line_endings_count_one_line_each", [this]() {
            // the bad char sits on line 3 in both: CR-only counts each CR (it once reported line 1), CRLF counts once (not 5)
            for (const char* source : {"a\rb\r@", "a\r\nb\r\n@"}) {
                string_symbolizer symbolizer;
                lexer lex(source, &symbolizer, "test.jai");
                auto tokens = lex.tokenize();
                const token* err = nullptr;
                for (auto& t : tokens) {
                    if (t.type == token_type::error) { err = &t; break; }
                }
                check(err != nullptr);
                check_eq(std::size_t(3), err->location.line);
            }
        });

        test("utf8_bom_executes_identically", [this]() {
            // A BOM'd script must produce byte-for-byte identical output and result to the
            // un-BOM'd source. Runs on whichever backend the runner selected (--backend=).
            const std::string script = "print(\"hi\"); var s = 1 + 2; print(s); 40 + s;";
            auto run = [&](const std::string& src) {
                auto engine = make_engine();
                stdlib::register_all(*engine);
                auto capture = std::make_shared<std::ostringstream>();
                engine->set_output_stream(capture);
                jai::script_int result = engine->execute(src).as_int();
                return std::make_pair(capture->str(), result);
            };
            auto plain = run(script);
            auto bommed = run("\xEF\xBB\xBF" + script);
            check_eq(plain.first, bommed.first);     // byte-for-byte identical output
            check_eq(plain.second, bommed.second);   // identical result
        });

        test("utf8_bom_execute_file_matches", [this]() {
            // File-based path: a BOM'd file on disk lexes cleanly through execute_file and
            // matches the same script run without a BOM.
            namespace fs = std::filesystem;
            const std::string script = "print(\"file\"); 7 + 5;";
            fs::path path = jai::foundry::test_temp_directory() / "jai_bom_lexer_test.jai";
            {
                std::ofstream out(path, std::ios::binary);
                out << "\xEF\xBB\xBF" << script;
            }
            auto engine = make_engine();
            stdlib::register_all(*engine);
            auto capture = std::make_shared<std::ostringstream>();
            engine->set_output_stream(capture);
            jai::script_int result = engine->execute_file(path.string()).as_int();
            std::error_code ec; fs::remove(path, ec);
            check_eq(std::string("file\n"), capture->str());
            check_eq((jai::script_int)12, result);
        });

        test("template_string_crlf_output_matches_lf", [this]() {
            // A template string spanning source lines: CRLF- and LF-saved forms must yield
            // byte-identical string values (line endings normalized to '\n').
            const std::string lf   = "var n = \"X\"; print(`line1\nline2 ${n}`);";
            const std::string crlf = "var n = \"X\"; print(`line1\r\nline2 ${n}`);";
            auto run = [&](const std::string& src) {
                auto engine = make_engine();
                stdlib::register_all(*engine);
                auto capture = std::make_shared<std::ostringstream>();
                engine->set_output_stream(capture);
                engine->execute(src);
                return capture->str();
            };
            std::string lf_out = run(lf);
            std::string crlf_out = run(crlf);
            check_eq(lf_out, crlf_out);
            check(crlf_out.find('\r') == std::string::npos);   // no stray CR survived
            check(crlf_out.find("line1\nline2 X") != std::string::npos);
        });
    }
};

} // namespace jai::foundry::tests

// Auto-register with the test framework
FOUNDRY_REGISTER(jai::foundry::tests::lexer_tests)
