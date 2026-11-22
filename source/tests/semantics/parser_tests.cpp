#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/ast_serializer.hpp>
#include <jaiscript/detail/generics.hpp>
#include "../case_table.hpp"
#include "../input_deadline.hpp"

using namespace jai::foundry;

namespace jai::foundry::tests {

class parser_tests : public suite {
public:
    parser_tests() : suite("Parser Tests") {}
    
    void forge_tests() override {
        test("a_generic_definition_owns_its_token_text", [this]() {
            // A generic keeps its declaration's tokens after the lexer is gone (instances, stamps and the jaibite writer read
            // them later), and a literal's lexeme pointed into the lexer's copy of the source: each kept lexeme must be
            // symbolizer storage
            auto eng = make_engine();
            auto* symbolizer = detail::engine_access::get_symbolizer(*eng);
            std::vector<declaration_ptr> decls;
            {
                const std::string code =
                    "T pick<T>(T a, T b) { float f = 2.5; string s = \"text\"; string r = R\"(raw)\"; char c = 'q'; return 7 > 3 ? a : b; }"
                    " class Box<T : Base> { T held; int n = 42; string label() { return \"box\"; } }";
                lexer lex(code, symbolizer, "test.jai");
                auto tokens = lex.tokenize();
                std::unordered_set<std::string> empty_types;
                parser p(tokens, symbolizer, eng.get(), empty_types, "test.jai");
                auto parsed = p.parse();
                check(!p.has_errors() && parsed.has_value(), "parses");
                if (parsed) { decls = std::move(parsed.value()); }
            }
            size_t generics = 0, literals = 0;
            const auto owned = [&](const token& t, const std::string& where) {
                if (t.lexeme.empty()) { return; }
                if (t.type == token_type::integer_literal || t.type == token_type::float_literal || t.type == token_type::string_literal ||
                    t.type == token_type::char_literal) { ++literals; }
                const auto id = symbolizer->find(t.lexeme);
                check_true(id.has_value() && symbolizer->get_string(*id).data() == t.lexeme.data(),
                    where + ": '" + std::string(t.lexeme) + "' is not symbolizer storage");
            };
            for (const auto& d : decls) {
                if (!d || d->get_type() != node_type::generic_decl) { continue; }
                const auto& definition = *static_cast<const generic_decl*>(d.get())->definition;
                ++generics;
                for (const token& t : definition.tokens) { owned(t, std::string(definition.name)); }
                for (const auto& parameter : definition.parameters) {
                    for (const token& t : parameter.bound) { owned(t, std::string(definition.name) + " bound"); }
                }
            }
            check_eq((size_t)2, generics, "both generics kept their tokens");
            check_eq((size_t)8, literals, "every literal spelling was looked at");
        });

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

        // '>>' after one open '<' closes it and leaves a '>' nothing opened: the parse reads it as it reads '> >'
        test("a_shift_that_closes_one_type_argument_list_leaves_a_stray_greater", [this]() {
            const std::vector<std::string> heads = {
                "string<int>>", "int<int>>", "bool<int>>", "float<T>>", "char<int>>", "array<int>>", "map<string, int>>",
                "weak_ptr<int>>", "shared_ptr<float>>", "function<void()>>", "coroutine<int>>", "array<array<int>>>",
            };
            // where each head stands, and whether the '>' left over is an error there (after a type named as a value it compares)
            const std::vector<std::pair<std::string, bool>> programs = {
                {"@", true}, {"@ y;", true}, {"@ y; print(1);", true}, {";@", true}, {";@ y;", true}, {"int x = 1;\n@ y;\nprint(2);", true},
                {"print(1);\n@", true}, {"print(1); @ y = 2; print(3);", true}, {"void f() { @ y; }", true}, {"void f() { int a; @ x; }", true},
                {"void f() { @ }", true}, {"class C { @ x; }", true}, {"class C { int a; @ x; int b; }", true}, {"class C { @ }", true},
                {"{ @ }", true}, {"{ @ y; }", true}, {"if (true) { @ x; }", true}, {"namespace n { @ x; }", true}, {"void f(@ x) { }", true},
                {"@ f() { return 1; }", true}, {"for (@ x : xs) { }", true}, {"for (@ x = 1; ; ) { }", true}, {"var v = @();", true},
                {"var v = @;", true}, {"cast<@>(1);", true}, {"auto g = [](@ x) { };", true}, {"try { } catch (@ e) { }", true},
                {"class C { @ m() { return 1; } }", true}, {"void f() { int a = 1; @ }\nprint(1);", true},
                {"print(@{});", false}, {"if (@ x = 1) { }", false},
            };
            input_deadline deadline("a parse");
            auto eng = make_engine();
            std::string failures;
            size_t parsed = 0;
            for (const auto& [program, stray] : programs) {
                for (const std::string& head : heads) {
                    for (const char* ending : {"", "\n"}) {
                        const std::string code = spelled(program, head) + ending;
                        std::string spaced_head = head;
                        spaced_head.insert(spaced_head.rfind(">>") + 1, " ");
                        deadline.taking(code);
                        const std::vector<std::string> reported = parse_messages(code, *eng);
                        const std::vector<std::string> expected = parse_messages(spelled(program, spaced_head) + ending, *eng);
                        ++parsed;
                        if (stray && reported.empty()) { failures += code + "\n  parsed without an error\n"; }
                        else if (reported != expected) {
                            failures += code + "\n  reported '" + (reported.empty() ? std::string("<none>") : reported.front()) + "' (" +
                                        std::to_string(reported.size()) + "), spelled '> >' it reports '" +
                                        (expected.empty() ? std::string("<none>") : expected.front()) + "' (" + std::to_string(expected.size()) + ")\n";
                        }
                    }
                }
            }
            check_eq(programs.size() * heads.size() * 2, parsed);
            check_true(failures.empty(), failures);
        });

        test("a_stray_greater_at_the_end_of_the_file_is_reported_once", [this]() {
            const std::vector<std::string> stray = {"expected variable name"};
            const std::vector<std::string> after_a_lone_semicolon = {"expected expression", "expected variable name"};
            const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
                {"string<int>>", stray}, {"array<int>>", stray}, {"print(1);\nweak_ptr<int>>", stray},
                {";array<int>>", after_a_lone_semicolon}, {";shared_ptr<float>>", after_a_lone_semicolon},
            };
            input_deadline deadline("a parse");
            for (const auto& [code, expected] : cases) {
                deadline.taking(code);
                check_true(parse_messages(code) == expected, code);
            }
            // the load error is the same text on both backends
            const auto load_error = [](backend_type backend, const std::string& code) {
                auto e = make_default_engine();
                e->set_backend(backend);
                try {
                    e->execute(code);
                } catch (const std::exception& error) {
                    return std::string(error.what());
                }
                return std::string("<no error>");
            };
            for (const auto& [code, expected] : cases) {
                deadline.taking(code);
                const std::string interpreted = load_error(backend_type::interpreter, code);
                check_eq(interpreted, load_error(backend_type::vm, code), code);
                check_true(interpreted.find("error: expected variable name") != std::string::npos, interpreted);
            }
            check_eq(std::string("parse failed:\n<script>:1:11: error: expected variable name\n    string<int>>\n              ^"),
                     load_error(backend_type::vm, "string<int>>"));
        });

        // a generic method whose parameter list runs past the class's '}' ends its text after it: the member loop is then outside
        // the class, where no member boundary is left to find
        test("a_member_that_reads_past_its_class_does_not_stop_the_recovery", [this]() {
            input_deadline deadline("a parse");
            for (const char* code : {"class C { T f<T>(T x } ) { } ) }", "class C { T f<T>(T x } ) { } ) ) int y; }\nprint(1);",
                                     "class C { T f<T>(T x } ) { } > }", "class C { class D { T f<T>(T x } } ) { } ) } }"}) {
                deadline.taking(code);
                check_true(!parse_messages(code).empty(), code);
            }
        });

        // every program one token away from these (a token dropped, doubled, replaced by a bracket or a separator, or one of those
        // put before it) parses to an end: a recovery reads on from wherever an error leaves it
        test("every_one_token_edit_of_a_generic_program_parses_to_an_end", [this]() {
            const std::vector<std::string> programs = {
                "class Box < T > { T item ; array < T > rest ; map < string , array < T >> index ; Box ( T first ) { item = first ; } "
                "T get ( ) { return item ; } U as < U > ( U other ) { return other ; } }",

                "T largest < T > ( array < T > xs ) { T best = xs [ 0 ] ; for ( auto x : xs ) { if ( best < x ) { best = x ; } } return best ; } "
                "Box < int > b = Box < int > ( 1 ) ; array < array < int >> grid = [ [ 1 ] ] ; "
                "print ( largest < int > ( grid [ 0 ] ) , b . as < string > ( \"s\" ) ) ;",

                "namespace shop { class Item { int price = 1 ; } T twice < T > ( T v ) { return v ; } } "
                "shared_ptr < shop :: Item > held = make_shared < shop :: Item > ( ) ; weak_ptr < shop :: Item > seen = held ; "
                "function < int ( int , array < int > ) > apply = [ ] ( int a , array < int > xs ) -> int { return a + cast < int > ( xs . size ( ) ) ; } ;",

                "map < string , shared_ptr < shop :: Item >> stock = { \"a\" : held } ; "
                "for ( auto [ k , v ] : stock ) { if ( auto n = v . price ; n < 2 && 2 > ( n ) ) { print ( k ) ; } } "
                "try { throw shop :: twice < int > ( 3 ) ; } catch ( int e ) { print ( e < 4 , 4 > ( e ) ) ; }",

                "class Base { int id = 0 ; } class Node < T : Base > : Base { T value ; shared_ptr < Node < T >> next ; static int count = 0 ; "
                "[ [ nodiscard ] ] int depth ( ) { int d = 0 ; do { d += 1 ; } while ( d < 3 ) ; return d ; } }",

                "coroutine int steps ( int n ) { for ( int i = 0 ; i < n ; i += 1 ) { yield i ; } } Node < Base > root ; "
                "switch ( root . depth ( ) ) { case 3 : { print ( dynamic_cast < Base & > ( root ) . id ) ; break ; } default : { } } "
                "const array < map < string , int >> rows = [ { \"n\" : 1 } ] ; int total = 0 ; "
                "for ( const auto & row : rows ) { total += row [ \"n\" ] ; } print ( total >> 1 , total > 0 ) ;",

                "class Pair < A , B > { A first ; B second ; Pair < B , A > swapped ( ) { Pair < B , A > result ; result . first = second ; "
                "result . second = first ; return result ; } bool operator < ( Pair < A , B > other ) { return first < other . first ; } }",

                "enum Color { red , green } Pair < int , string > p = Pair < int , string > { } ; auto q = p . swapped ( ) ; bool less = p < p ; "
                "int n = less ? 1 : 2 ; auto pick = [ & ] ( Pair < string , int > it ) { return it . second < n ; } ; "
                "auto twice = ( int x ) => x < n ? x : n ; while ( n > 0 ) { n -= 1 ; if ( pick ( q ) ) { continue ; } } "
                "array < function < bool ( Pair < string , int > ) >> tests = [ pick ] ; "
                "print ( tests . size ( ) , Color :: red == Color :: green , twice ( 1 ) ) ;",
            };
            const std::vector<std::string> placed = {"<", ">", ">>", "(", ")", "{", "}", ";", ","};
            input_deadline deadline("a parse");
            size_t parsed = 0;
            for (const std::string& program : programs) {
                std::vector<std::string> words;
                for (size_t at = 0; at < program.size();) {
                    const size_t end = std::min(program.find(' ', at), program.size());
                    words.push_back(program.substr(at, end - at));
                    at = end + 1;
                }
                auto eng = make_engine();
                const auto parse_edit = [&](size_t at, size_t dropped, const std::vector<std::string>& put) {
                    std::string code;
                    for (size_t i = 0; i <= words.size(); ++i) {
                        if (i == at) { for (const std::string& word : put) { code += word + ' '; } }
                        if (i < words.size() && (i < at || i >= at + dropped)) { code += words[i] + ' '; }
                    }
                    deadline.taking(code);
                    ++parsed;
                    return parse_messages(code, *eng);
                };
                check_true(parse_edit(0, 0, {}).empty(), "the program itself parses: " + program);
                for (size_t at = 0; at < words.size(); ++at) {
                    parse_edit(at, 1, {});
                    parse_edit(at, 0, {words[at]});
                    for (const std::string& word : placed) {
                        parse_edit(at, 1, {word});
                        parse_edit(at, 0, {word});
                    }
                }
            }
            check_true(parsed > 15000, std::to_string(parsed));
        });
    }

private:
    static std::string spelled(std::string program, const std::string& head) {
        for (size_t at = program.find('@'); at != std::string::npos; at = program.find('@', at + head.size())) { program.replace(at, 1, head); }
        return program;
    }

    std::vector<std::string> parse_messages(const std::string& code) {
        auto eng = make_engine();
        return parse_messages(code, *eng);
    }

    std::vector<std::string> parse_messages(const std::string& code, engine& eng) {
        auto* symbolizer = detail::engine_access::get_symbolizer(eng);
        lexer lex(code, symbolizer, "test.jai");
        auto tokens = lex.tokenize();
        std::unordered_set<std::string> empty_types;
        parser p(tokens, symbolizer, &eng, empty_types, "test.jai");
        auto ast = p.parse();
        std::vector<std::string> result;
        for (const auto& issue : p.issues()) { result.push_back(issue.message); }
        return result;
    }

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