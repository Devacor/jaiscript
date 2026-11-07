#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/ast_serializer.hpp>
#include "../case_table.hpp"

using namespace jai::foundry;

namespace jai::foundry::tests {

class parser_tests : public suite {
public:
    parser_tests() : suite("Parser Tests") {}
    
    void forge_tests() override {
        test("function_spelling_is_kept_for_the_checker", [this]() {
            // `function f = expr;` parses as a callable-or-null slot that remembers its spelling at top
            // level, in a body and as a field; the flag rides the jaibite so a re-check of a loaded bite
            // sees it; `auto` and `var` declarations do not carry it
            auto eng = make_engine();
            auto* symbolizer = detail::engine_access::get_symbolizer(*eng);
            const std::string code =
                "function f = 42; auto g = 42; var h = 42; function later;"
                " void body() { function k = null; int n = 0; }"
                " class C { function cb = null; int n = 0; static function sf = null; static int sn = 0; }"
                " namespace N { function nf = 42; int nn = 0; }";
            lexer lex(code, symbolizer, "test.jai");
            auto tokens = lex.tokenize();
            std::unordered_set<std::string> empty_types;
            parser p(tokens, symbolizer, eng.get(), empty_types, "test.jai");
            auto parsed = p.parse();
            check(!p.has_errors() && parsed.has_value(), "parses");
            auto decl_flag = [](const declaration_ptr& d) {
                return d && d->get_type() == node_type::variable_decl && static_cast<const variable_decl*>(d.get())->function_spelling;
            };
            auto probe = [&](const std::vector<declaration_ptr>& decls, const char* label) {
                check_eq((size_t)7, decls.size(), std::string(label) + " declaration count");
                check_true(decl_flag(decls[0]), std::string(label) + " function f");
                check_false(decl_flag(decls[1]), std::string(label) + " auto g");
                check_false(decl_flag(decls[2]), std::string(label) + " var h");
                check_true(decl_flag(decls[3]), std::string(label) + " function later;");
                const auto& slot_type = static_cast<const variable_decl*>(decls[0].get())->type;
                check_true(slot_type && slot_type->base_type == script_value_type::jai_function_type && slot_type->type_params.empty(),
                           std::string(label) + " the slot is the callable type");
                auto* fn = static_cast<const function_decl*>(decls[4].get());
                check_true(fn->body && fn->body->declarations.size() == 2 && decl_flag(fn->body->declarations[0]) &&
                           !decl_flag(fn->body->declarations[1]), std::string(label) + " body local");
                auto* cls = static_cast<const class_decl*>(decls[5].get());
                check_true(cls->members.size() == 4 && decl_flag(cls->members[0].declaration) &&
                           !decl_flag(cls->members[1].declaration), std::string(label) + " field");
                check_true(cls->members.size() == 4 && decl_flag(cls->members[2].declaration) &&
                           !decl_flag(cls->members[3].declaration), std::string(label) + " static field after the modifier");
                auto* ns = static_cast<const namespace_decl*>(decls[6].get());
                check_true(ns->declarations.size() == 2 && decl_flag(ns->declarations[0]) &&
                           !decl_flag(ns->declarations[1]), std::string(label) + " namespace variable");
            };
            probe(parsed.value(), "parsed");
            const auto bytes = detail::serialize_jaibite(parsed.value(), *symbolizer, eng->registration_fingerprint());
            uint64_t fingerprint = 0;
            auto loaded = detail::deserialize_jaibite(bytes.data(), bytes.size(), eng.get(), fingerprint);
            probe(loaded, "loaded");
        });

        test("class_typed_reference_declaration_parses_as_a_declaration", [this]() {
            // `Base& b = derived;` and `Base& b;` at statement level are reference
            // declarations (they read as bitwise-and plus assignment before), and one without its
            // initializer is an error; `Box& get(` stays the reference-return function; a bitwise-and
            // statement keeps its meaning
            check_parse_succeeds("class A { int x = 1; } class B : A {} B b = B(); A& a = b; a.x = 5;");
            check_parse_fails("class A {} A& r;");
            check_parse_succeeds("class Box { int v = 0; } Box b = Box(); Box& get() { return b; }");
            auto eng = make_engine();
            auto* symbolizer = detail::engine_access::get_symbolizer(*eng);
            lexer lex("class A { int x = 1; } class B : A {} B b = B(); A& a = b;", symbolizer, "test.jai");
            auto tokens = lex.tokenize();
            std::unordered_set<std::string> empty_types;
            parser p(tokens, symbolizer, eng.get(), empty_types, "test.jai");
            auto parsed = p.parse();
            check(!p.has_errors() && parsed.has_value(), "parses");
            const auto& decls = parsed.value();
            check_eq((size_t)4, decls.size());
            check_true(decls[3]->get_type() == node_type::variable_decl, "a declaration, not an expression");
            auto* ref = static_cast<const variable_decl*>(decls[3].get());
            check_true(ref->type && ref->type->base_type == script_value_type::jai_reference_type, "a reference type");
            check_eq(std::string("A&"), ref->type->type_name);
        });

        test("every_expression_and_statement_form_parses_and_malformed_ones_fail", [this]() {
            const std::pair<const char*, bool> rows[] = {   // {code, parses}
                {"42;", true},
                {"3.14;", true},
                {"\"hello\";", true},
                {"'x';", true},
                {"true;", true},
                {"false;", true},
                {"null;", true},
                {"x;", true},
                {"foo_bar;", true},
                {"_private;", true},
                {"1 + 2;", true},
                {"x * y;", true},
                {"a && b || c;", true},
                {"x == y;", true},
                {"a < b;", true},
                {"-x;", true},
                {"!flag;", true},
                {"++i;", true},
                {"--count;", true},
                {"&value;", false},   // no address-of: the parser names auto& and T& parameters instead
                {"x = 42;", true},
                {"y += 10;", true},
                {"z *= 2;", true},
                {"x ? y : z;", true},
                {"a > b ? a : b;", true},
                {"flag ? 1 : 0;", true},
                {"(42);", true},
                {"(x + y) * z;", true},
                {"((a + b) * c) / d;", true},
                {"foo();", true},
                {"bar(x);", true},
                {"baz(a, b, c);", true},
                {"nested(func(x));", true},
                {"obj.property;", true},
                {"ptr->member;", true},
                {"nested.obj.prop;", true},
                {"arr[0];", true},
                {"matrix[i][j];", true},
                {"[1, 2, 3];", true},
                {"[];", true},
                {"[x, y + z, func()];", true},
                {"var x = 42;", true},
                {"let y = \"hello\";", true},
                {"auto w = getValue();", true},
                {"function add(auto a, auto b) -> auto { return a + b; }", true},
                {"auto multiply(auto x, auto y) -> auto { return x * y; }", true},
                {"function getValue() -> auto { return 42; }", true},
                {"if (x > 0) { print(x); }", true},
                {"if (flag) doSomething(); else doOther();", true},
                {"while (condition) { update(); }", true},
                {"for (var i = 0; i < 10; ++i) { work(i); }", true},
                {"return;", true},
                {"return 42;", true},
                {"return x + y;", true},
                {"auto lambda = [](){ return 42; };", true},
                {"auto typed = [](auto x) -> auto { return x * 2; };", true},
                {"result = (a + b) * c.getValue() - arr[i]->member;", true},
                {"bool check = x > 0 && func(y) != null;", true},
                {"auto final = condition ? getValue() : getDefault();", true},
                {"42 +;", false},
                {"if (x > 0;", false},
                {"return return;", false},
            };
            check_each(rows, [&](const auto& row) {
                if (row.second) {
                    check_parse_succeeds(row.first);
                } else {
                    check_parse_fails(row.first);
                }
            });
        });

        test("map_literals", [this]() {
            check_parse_succeeds("{\"key\": value};");
            check_parse_succeeds("{\"a\": 1, \"b\": 2};");
            check_parse_succeeds("var m = {};");
            // a '{' that starts a statement opens a block, as in C++: nested, empty, and inside a body
            check_parse_succeeds("{}");
            check_parse_succeeds("{ {} }");
            check_parse_succeeds("{ { int a = 1; } }");
            check_parse_succeeds("void f() { { { int a = 1; } {} } }");
            check_parse_succeeds("void f() { ({\"k\": 1}); }");
            const auto errors = collect_parse_errors("void f() { {\"k\": 1}; }");
            check_true(!errors.empty() && errors[0].find("a statement that starts with '{' is a block") != std::string::npos,
                       errors.empty() ? std::string("no error") : errors[0]);
        });

        test("break_continue_statements", [this]() {
            check_parse_succeeds("while (true) { if (done) break; }");
            check_parse_succeeds("for (var i = 0; i < 10; ++i) { if (skip) continue; work(); }");
            check_parse_succeeds("switch (1) { case 1: break; }");
            check_parse_succeeds("while (true) { switch (1) { case 1: continue; } }");
            check_parse_succeeds("do { break; } while (true);");
            check_parse_succeeds("for (auto x : [1]) { [](){ while (true) { break; } }; continue; }");
            // C++: a jump with nothing to leave is ill-formed, a function or lambda body leaving nothing it sits in
            for (const char* source : {"break;", "continue;", "if (true) { break; }", "switch (1) { case 1: continue; }",
                                       "while (true) { auto f = []() { break; }; }", "for (;;) { void g() { continue; } }"}) {
                const auto errors = collect_parse_errors(source);
                check_true(!errors.empty() && errors[0].find("outside a loop") != std::string::npos, std::string(source) + ": " + (errors.empty() ? std::string("no error") : errors[0]));
            }
        });

        // 'map'/'array' are hard keywords by grammar contract outside member names; using one as a name
        // used to report a generic "Expected X name" (or "Expected '<'") with no
        // cause. The diagnostic now names the keyword at the true token.
        test("type_keyword_as_name_diagnostics", [this]() {
            auto contains = [](const std::vector<std::string>& errs, const std::string& needle) {
                for (const auto& e : errs) {
                    if (e.find(needle) != std::string::npos) return true;
                }
                return false;
            };
            // a member declaration makes a keyword a name: fields reached qualified (c.map)
            check(collect_parse_errors("class C { var map = {}; var array = []; }").empty(), "keyword field names");

            auto globalErrs = collect_parse_errors("int map = 5;");
            check(contains(globalErrs, "'map' is a type keyword"), "global: " + (globalErrs.empty() ? "<none>" : globalErrs[0]));
            check(contains(globalErrs, "variable name"), "global what");

            auto paramErrs = collect_parse_errors("function f(var map) { return 0; }");
            check(contains(paramErrs, "'map' is a type keyword"), "param");

            auto destructureErrs = collect_parse_errors("auto [x, map] = p;");
            check(contains(destructureErrs, "'map' is a type keyword"), "destructuring");

            // after a dot, and as a method's declared name, a keyword is a member name
            check(collect_parse_errors("obj.map; obj.string(); p->array;").empty(), "member access");
            check(collect_parse_errors("class C { int map(int x) { return x; } static bool default() { return true; } }").empty(), "method names");

            // Statement starting with the keyword commits to a type parse - the '<'
            // message now explains the collision instead of a bare "Expected '<'"
            auto stmtErrs = collect_parse_errors("function g() { map = m; }");
            check(contains(stmtErrs, "built-in map type keyword"), "statement: " + (stmtErrs.empty() ? "<none>" : stmtErrs[0]));
        });

        // A failed member/ctor body used to recover at the first ';' INSIDE the body:
        // the member loop resumed mid-body, took the method's '}' for the class's, and
        // every later member leaked to top level with a spurious "Expected expression"
        // at the class's real closing brace. Brace-aware recovery keeps errors at the
        // true sites and the class intact.
        test("class_recovery_stays_in_class", [this]() {
            const std::string code =
                "class Level {\n"              // 1
                "\tstring map = \"\";\n"       // 2  a keyword field name (reached as this.map)
                "\tint kills = 0;\n"           // 3
                "\tLevel(string m) {\n"        // 4
                "\t\tmap = m;\n"               // 5  <- the true site (bare, map is the keyword)
                "\t}\n"                        // 6
                "\tvoid report() {\n"          // 7
                "\t\tkills = kills + 0;\n"     // 8
                "\t}\n"                        // 9
                "\tint total() {\n"            // 10
                "\t\treturn kills;\n"          // 11
                "\t}\n"                        // 12
                "}\n";                         // 13 <- no error may land here
            auto errs = collect_parse_errors(code);
            check_eq((size_t)1, errs.size());
            check(errs.size() == 1 && errs[0].find("5:") != std::string::npos &&
                  errs[0].find("built-in map type keyword") != std::string::npos,
                  "the error at the ctor-body use: " + (errs.empty() ? "<none>" : errs[0]));
            for (const auto& e : errs) {
                check(e.find("Expected expression") == std::string::npos, "no downstream cascade: " + e);
                check(e.find("13:") == std::string::npos, "no error at the class close: " + e);
            }
        });
    }

private:
    std::vector<std::string> collect_parse_errors(const std::string& code) {
        auto eng = make_engine();
        auto* symbolizer = detail::engine_access::get_symbolizer(*eng);
        lexer lex(code, symbolizer, "test.jai");
        auto tokens = lex.tokenize();
        std::unordered_set<std::string> empty_types;
        parser p(tokens, symbolizer, eng.get(), empty_types, "test.jai");
        try {
            auto ast = p.parse();
        } catch (const parse_error&) {
            // errors_ still carries the reports
        }
        return p.get_errors();
    }

    void check_parse_succeeds(const std::string& code) {
        auto eng = make_engine();
        auto* symbolizer = detail::engine_access::get_symbolizer(*eng);
        lexer lex(code, symbolizer, "test.jai");
        auto tokens = lex.tokenize();
        std::unordered_set<std::string> empty_types;
        parser p(tokens, symbolizer, eng.get(), empty_types, "test.jai");
        auto ast = p.parse();
        check(!p.has_errors(), "Expected parsing to succeed for: " + code);
    }

    void check_parse_fails(const std::string& code) {
        auto eng = make_engine();
        auto* symbolizer = detail::engine_access::get_symbolizer(*eng);
        lexer lex(code, symbolizer, "test.jai");
        auto tokens = lex.tokenize();
        std::unordered_set<std::string> empty_types;
        parser p(tokens, symbolizer, eng.get(), empty_types, "test.jai");
        try {
            auto ast = p.parse();
            check(p.has_errors(), "Expected parsing to fail for: " + code);
        } catch (const parse_error&) {
            // Expected - parsing failed as intended
            check(true);
        }
    }
};

} // namespace jai::foundry::tests

// Auto-register with the test framework
FOUNDRY_REGISTER(jai::foundry::tests::parser_tests)