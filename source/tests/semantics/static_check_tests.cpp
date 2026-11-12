// Static type checking (docs/site/embedding/06-static-checking.html; strict by default): mode API, the
// type-ladder rule table, accumulation/recovery/cap, strict gating (throws before anything
// runs), warn retrieval, hot-reload + registration-epoch re-checks, jaibite stamping, and a
// generator-corpus false-positive gate (false positives are the feature's death).

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/builtin_api_arities.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/detail/type_proofs.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>
#include <jaiscript/detail/ast_serializer.hpp>
#include <jaiscript/core/class_registry.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include "fuzz/fuzz_generator.hpp"
#include "fuzz/fuzz_harness.hpp"
#include "../case_table.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

namespace {

struct probe_object {
    int64_t value = 0;
    int64_t doubled() const { return value * 2; }
};

// A getter-only property beside writable ones (member pointer, lambda pair)
struct read_only_probe {
    int64_t value = 0;
    int64_t lam_backing = 0;
};
void bind_read_only_probe(jai::engine& e) {
    dynamic_binder<read_only_probe>(e, "RoProbe")
        .constructor<>()
        .property("value", &read_only_probe::value)
        .property("lam",
                  [](read_only_probe& s) -> int64_t { return s.lam_backing; },
                  [](read_only_probe& s, int64_t v) { s.lam_backing = v; })
        .property("ro", [](const read_only_probe& s) -> int64_t { return s.value + 100; }, nullptr)
        .build();
}

size_t error_count(const check_report& r) { return r.error_count(); }
size_t warning_count(const check_report& r) {
    return r.diagnostics.size() - r.error_count();
}
using dlevel = check_diagnostic::level;
bool has_diag(const check_report& r, dlevel severity, std::string_view text) {
    for (const auto& d : r.diagnostics) {
        if (d.severity == severity && d.message.find(text) != std::string::npos) { return true; }
    }
    return false;
}

// Proof differential helper: one line per outcome, error text included verbatim.
std::string outcome(jai::engine& e, const std::string& src) {
    try {
        return "VALUE " + e.execute(src).to_string();
    } catch (const std::exception& ex) {
        return std::string("ERROR ") + ex.what();
    }
}

uint64_t shape_epoch(jai::engine& e, const std::string& cls) {
    auto def = detail::engine_access::get_class_registry(e).find_script_class(cls);
    return def ? def->field_shape_epoch() : 0;
}

// Program-closure fixtures: one temp directory per test, added as the engine's include path.
std::filesystem::path closure_dir(const std::string& name) {
    auto dir = jai::foundry::test_temp_directory() / ("jai_closure_" + name);
    std::filesystem::create_directories(dir);
    return dir;
}

void write_unit(const std::filesystem::path& dir, const std::string& file, const std::string& content) {
    std::ofstream out(dir / file, std::ios::binary | std::ios::trunc);
    out << content;
}

std::shared_ptr<jai::engine> program_engine(const std::filesystem::path& dir) {
    auto e = make_engine();
    e->jaibite_cache(false);
    e->add_include_path(dir.string());
    return e;
}

// the bytes of a bite no checker has seen (parsed and serialized directly), for a loader to check
std::vector<uint8_t> unstamped_bite_bytes(jai::engine& a_engine, const std::string& a_source) {
    const auto templates = detail::engine_access::get_registered_template_types(a_engine);
    lexer lex(a_source, detail::engine_access::get_symbolizer(a_engine));
    auto tokens = lex.tokenize();
    parser syntax(tokens, detail::engine_access::get_symbolizer(a_engine), &a_engine, templates);
    auto declarations = syntax.parse();
    check_true(bool(declarations), "fixture must parse: " + a_source);
    return detail::serialize_jaibite(declarations.value(), *detail::engine_access::get_symbolizer(a_engine), a_engine.registration_fingerprint());
}

} // namespace

class static_check_tests : public suite {
public:
    static_check_tests() : suite("Static Type Checking") {}

    void forge_tests() override {
		test("known_builtin_call_arities_come_from_api_catalog", [this]() {
			auto e = make_engine();
			for (const auto& source : {
				"array<int> values = [1]; array::slice(values, 0, 1, 2);",
				"array<int> values = [1]; values.sort((a,b) => a < b, 7);",
				"array<int> values = [1]; values.reverse(0);",
				"map<string,int> values = {\"a\": 1}; values.get();",
				"string value = \"7\"; value.to_int(null, 10, 2);",
				"string value = \" x \"; value.trim(\" \", \"x\");",
				"utf8_string value = \"abc\"; value.substr(0, 1, 2);"
			}) {
				auto result = e->check(source);
				check_true(result.has_errors(), result.format(source));
				check_true(has_diag(result, dlevel::error, "expects"), result.format(source));
			}
			for (const auto& source : {
				"array<int> values = [1]; auto s1 = array::slice(values); auto s2 = array::slice(values, 0); auto s3 = array::slice(values, 0, 1); values.erase(0);",
				"string value = \"7\"; value.to_int(); value.to_int(0); value.to_int(0, 10); value.split(); value.split(\",\", 2);",
				"string value = \" x \"; value.trim(); value.trim(\" \");",
				"map<string,int> values = {\"a\": 1}; values.get(\"a\"); values.get(\"a\", 0);"
			}) {
				auto result = e->check(source);
				check_eq(std::size_t(0), result.error_count(), result.format(source));
			}
		});

        // ------------------------------------------------------------ mode API

        test("member_global_collision_warns", [this]() {
            // Same-unit member/global name collisions warn (bare name in methods means
            // the member; ::name reaches the global). Engine-registered names stay
            // silent - shadowing to_string is the established print-integration idiom.
            auto e = make_engine();
            auto r = e->check(R"(
                int hp = 100;
                string ping() { return "g"; }
                class Creature {
                    int hp = 5;
                    string ping() { return "m"; }
                    string to_string() { return "c"; }
                }
            )");
            check_eq((size_t)0, error_count(r));
            check_eq((size_t)2, warning_count(r));
            check_true(r.diagnostics[0].message.find("shadows the global") != std::string::npos);
            auto quiet = e->check(R"(
                class Plain { int solo = 1; string to_string() { return "p"; } }
            )");
            check_eq((size_t)0, quiet.diagnostics.size());
        });

        test("an_assignment_as_a_condition_warns_unless_it_has_its_own_parentheses", [this]() {
            auto e = make_engine();
            const std::string text = "assignment used as a condition: write '==' to compare, or '((a = b))' with parentheses of its own to assign";
            auto warned = e->check("int a = 0; int b = 3;\nif (a = b) { }\nwhile (a = 0) { }\ndo { } while (a = 0);\nfor (; a = 0; ) { }\nif ((a) = b) { }");
            check_eq((size_t)0, error_count(warned));
            check_eq((size_t)5, warning_count(warned));
            for (size_t i = 0; i < warned.diagnostics.size(); ++i) {
                check_eq(text, warned.diagnostics[i].message);
                check_eq(i + 2, warned.diagnostics[i].line);
            }
            auto quiet = e->check("int a = 0; int b = 3; bool c = false;\nif ((a = b)) { }\nwhile ((a = 0)) { }\ndo { } while ((a = 0));\nfor (; (a = 0); ) { }\n"
                                  "if (a == b) { }\nif ((a = b) > 0) { }\nif (a += 1) { }\nif (c || (a = b)) { }");
            check_true(quiet.diagnostics.empty(), quiet.format());
        });

        test("a_handle_builtin_shadow_warns_where_a_handle_holds_the_class", [this]() {
            auto e = make_engine();
            std::vector<std::string> run_time;
            e->set_script_warning_handler([&](const std::string& m) { run_time.push_back(m); });
            const std::string shadow = "shadows the builtin shared_ptr";
            auto by_value = e->check("class World { int n = 0; void reset() { n = 1; } }\nWorld w;\nw.reset();");
            check_false(has_diag(by_value, dlevel::warning, shadow), by_value.format());
            auto held = e->check("class Pool { int n = 0; void reset() { n = 1; } }\nvar p = new Pool();");
            check_true(has_diag(held, dlevel::warning, "class method 'reset' shadows the builtin shared_ptr reset() - the builtin is unreachable on Pool instances"), held.format());
            check_eq((size_t)1, held.diagnostics.size());
            check_eq((size_t)1, held.diagnostics[0].line);
            auto inherited = e->check("class Base { int use_count() { return 1; } }\nclass Sub : Base { }\nshared_ptr<Sub> s;");
            check_true(has_diag(inherited, dlevel::warning, "class method 'use_count' shadows the builtin shared_ptr use_count() - the builtin is unreachable on Base instances"), inherited.format());
            check_eq((size_t)1, inherited.diagnostics[0].line);
            e->execute("class Prior { int n = 0; void reset() { n = 1; } }\nPrior v;\nv.reset();");
            auto later = e->check("int x = 1;\nvar q = make_shared<Prior>();");
            check_true(has_diag(later, dlevel::warning, "unreachable on Prior instances"), later.format());
            check_eq((size_t)2, later.diagnostics[0].line);
            check_eq((size_t)0, run_time.size());
        });

        test("warn_region_leaves_the_runtime_alone_battery", [this]() {
            // In a [[warn_mode]] unit the runtime alone decides: conversions run, a typed mismatch
            // raises the runtime's own error at the store, and no diagnostic is an error.
            auto e = make_engine();
            check_eq((int64_t)1, e->execute("[[warn_mode]]; int x = true; x").as_int());
            check_eq(std::string("Cannot initialize 'x' (declared int) with float"),
                     e->execute("[[warn_mode]]; string m = \"\"; try { int x = 4.7; } catch (err) { m = err; } m").as<std::string>());
            check_eq(std::string("Cannot initialize 's' (declared string) with int"),
                     e->execute("[[warn_mode]]; string m = \"\"; try { string s = 42; } catch (err) { m = err; } m").as<std::string>());
            check_eq(true, e->execute("[[warn_mode]]; bool b = \"x\"; b").as<bool>());
            check_eq((int64_t)7, e->execute("[[warn_mode]]; var v = 5; v = \"s\"; 7").as_int());
            check_eq((int64_t)10, e->execute("[[warn_mode]]; function ten() { return 10; } ten()").as_int());
            std::string text;
            try { e->execute("[[warn_mode]]; function bad() -> int { return \"nope\"; } bad();"); }
            catch (const static_check_error&) { text = "static_check_error"; }
            catch (const std::exception& ex) { text = ex.what(); }
            check_true(text.find("Cannot return string from 'bad' (declared -> int)") != std::string::npos,
                "a warn region: the runtime's text, never the checker's (got: " + text + ")");
            check_false(e->last_check_diagnostics().has_errors());
        });

        test("check_reports_without_executing", [this]() {
            auto e = make_engine();
            auto report = e->check("int x = \"hello\";");
            check_eq((size_t)1, error_count(report));
            check_eq((size_t)1, report.diagnostics[0].line);
        });

        // ------------------------------------------------------------ runtime parity probes
        // The checker's error set must be a subset of runtime failures (plus the
        // documented declared-intent rules). These probes pin the runtime behaviors
        // the rule table mirrors; if one fails, the table drifted.

        test("runtime_parity_probes", [this]() {
            auto e = make_engine();
            // conversions the checker must ACCEPT
            check_eq((int64_t)4, e->execute("int a = cast<int>(4.7); a").as_int());
            check_eq((int64_t)1, e->execute("int b = true; b").as_int());
            check_eq(true, e->execute("bool d = 7; d").as<bool>());
            check_near(2.0, e->execute("float f = 2; f").as_float(), 0.0001);
            // violations the checker flags must FAIL at runtime too
            check_throws([&]() { e->execute("int p1 = \"s\";"); });
            check_throws([&]() { e->execute("int p2 = null;"); });
            check_throws([&]() { e->execute("int p5 = 4.7;"); });
            check_throws([&]() { e->execute("[[warn_mode]]; int p6 = 4.7;"); });
            // declared intent: a bool slot refuses text at load; the run time alone reads its truthiness
            check_throws([&]() { e->execute("bool p7 = \"x\";"); });
            check_eq(true, e->execute("[[warn_mode]]; bool p8 = \"x\"; p8").as<bool>());
            check_throws([&]() { e->execute("int p3 = 'c';"); });
            check_throws([&]() { e->execute("string p4 = 42;"); });
            check_throws([&]() { e->execute("char p4 = 65;"); });
            check_throws([&]() { e->execute("function tp(int a) { return a; } tp(\"s\");"); });
        });

        // ------------------------------------------------------------ the type ladder

        test("the_type_ladder_enforces_typed_slots_infers_auto_and_leaves_var_dynamic", [this]() {
            const std::pair<const char*, size_t> cases[] = {
                {"int x = 5;", 0},
                {"int x = 4.7;", 1},
                {"int x = cast<int>(4.7);", 0},
                {"int x = true;", 0},
                {"float f = 2;", 0},
                {"string s = 42;", 1},
                {"bool b = \"x\";", 1},
                {"bool b = 2;", 0},
                {"int x;", 0},
                {"int x = \"s\";", 1},
                {"int x = null;", 1},
                {"int x = 'c';", 1},
                {"char c = 65;", 1},
                {"int x = [1, 2];", 1},
                {"auto x = 5; x = 7;", 0},
                {"auto x = 5; x = \"s\";", 1},
                {"auto x; x = 5; x = \"s\";", 0},   // auto without an initializer locks at its first run-time store, which the checker cannot order
                {"var v = 5; v = \"s\";", 0},
                {"var v = \"s\"; int x = v;", 0},
                {"var v = 1; int x = v + 1;", 0},
                {"var v = 5; v += \"s\";", 0},
            };
            check_each(cases, [&](const auto& c) {
                auto e = make_engine();
                check_eq(c.second, error_count(e->check(c.first)));
            });
        });

        test("ladder_assignment_to_typed", [this]() {
            auto e = make_engine();
            check_eq((size_t)1, error_count(e->check("int x = 1; x = 2.5;")));
            check_eq((size_t)0, error_count(e->check("int x = 1; x = true; float f = 1.0; f = x;")));
            check_eq((size_t)1, error_count(e->check("int x = 1; x = \"s\";")));
            auto r = e->check("int declared_here = 1;\ndeclared_here = \"s\";");
            check_eq((size_t)1, error_count(r));
            check_eq((size_t)2, r.diagnostics[0].line);
            check_eq((size_t)1, r.diagnostics[0].related_line);   // points at the declaration
        });

        test("compound_assign_follows_cpp_rules", [this]() {
            // `x op= rhs` on a typed target is `x = T(x op rhs)`: an int result widens into a
            // float target, a float or string result into an int target is refused.
            auto e = make_engine();
            check_eq((size_t)1, error_count(e->check("int x = 1; x += 2.5;")));
            check_eq((size_t)0, error_count(e->check("int x = 4; x /= 2;")));
            check_eq((size_t)0, error_count(e->check("float f = 1.0; f *= 2;")));
            check_eq((size_t)1, error_count(e->check("string s = \"a\"; s += 5;")));   // the runtime raises: a string target appends text only
            check_eq((size_t)0, error_count(e->check("string s = \"a\"; s += 'b'; s += \"c\";")));
            check_eq((size_t)1, error_count(e->check("int x = 1; x += \"s\";")));
        });

        // ------------------------------------------------------------ identifiers

        test("undeclared_identifier_is_an_error", [this]() {
            auto e = make_engine();
            check_eq((size_t)1, error_count(e->check("zzz_nope + 1;")));
            check_eq((size_t)1, error_count(e->check("zzz_nope();")));
            check_eq((size_t)1, error_count(e->check("zzz_nope = 5;")));
            check_eq((size_t)0, error_count(e->check("print(\"hi\");")));   // registered surface visible
        });

        test("a_name_only_another_scope_declares_is_undeclared", [this]() {
            auto e = make_engine();
            const std::string decls =
                "class C { int hp = 1; int heal() { return 2; } }\n"
                "enum E { Alpha, Beta }\n"
                "namespace ns { int nv = 3; int nfn() { return 1; } }\n"
                "function f(int pn) { int loc = 1; for (var rv : [1]) {} return pn + loc; }\n"
                "var lam = [](int lp) { int ll = 2; return lp + ll; };\n"
                "if (true) { int block_local = 4; }\n";
            for (const char* use : {"hp", "heal()", "Alpha", "nv", "nfn()", "pn", "loc", "rv", "lp", "ll", "block_local"}) {
                auto r = e->check(decls + "print(" + use + ");");
                check_eq((size_t)1, error_count(r), std::string(use) + ": " + r.format());
                check_true(r.format().find("use of undeclared identifier") != std::string::npos, r.format());
            }
            auto n = e->check("class D { int x = n; D(int n) {} }");
            check_eq((size_t)1, error_count(n), n.format());
            auto other = e->check("function g(int n) { return n; }\nclass D { int x = n; }");
            check_eq((size_t)1, error_count(other), other.format());
            auto reader = e->check("if (true) { var g = 1; }\nfunction reader() { return g; }");
            check_eq((size_t)1, error_count(reader), reader.format());
            auto sibling = e->check(decls + "namespace other { int peek() { return nv + nfn(); } }");
            check_eq((size_t)2, error_count(sibling), "another namespace's members: " + sibling.format());
            auto arity = e->check("namespace ns4 { int caller() { return later(); } int later(int x) { return x; } }");
            check_eq((size_t)1, error_count(arity), "a bare call checks against the namespace's own declaration: " + arity.format());
            // the program scope in any order, a namespace's own names inside it, and what a derived class declares from a base's bodies
            check_eq((size_t)0, error_count(e->check("function reads() { return later_global; }\nint later_global = 2;\nreads();")));
            check_eq((size_t)0, error_count(e->check("namespace ns2 { function nf() { return nv2; } int nv2 = 3; }\nns2::nf();")));
            check_eq((size_t)0, error_count(e->check("int later = 1; namespace ns3 { int caller() { return later(2); } int later(int x) { return x; } }\nns3::caller();")));
            check_eq((size_t)0, error_count(e->check("namespace o { int f() { return 1; } namespace i { int g() { return f(); } } }\nnamespace o::i::d { int h() { return f(); } }\no::i::d::h();")));
            const std::string derived = "class B : A { int dfield = 10; int helper() { return 1; } }";
            check_eq((size_t)0, error_count(e->check("class A { int go() { return helper() + dfield; } }\n" + derived)));
            check_eq((size_t)0, error_count(e->check("class A { int go() { auto l = [this]() { return helper(); }; return l(); } }\n" + derived)));
            auto stranger = e->check("class A { int go() { return helper(); } }\nclass Z { int helper() { return 1; } }");
            check_eq((size_t)1, error_count(stranger), stranger.format());
        });

        test("dynamic_include_keeps_undeclared_lenient_literal_does_not", [this]() {
            auto e = make_engine();
            // a dynamic path can define anything: warning severity
            auto dyn = e->check("var p = \"somewhere.jai\";\nimport(p);\nzzz_maybe_included();");
            check_eq((size_t)0, error_count(dyn));
            check_ge(warning_count(dyn), (size_t)1);
            // a literal path is the program closure's: an unresolvable one contributes no names
            auto lit = e->check("include \"somewhere_missing.jai\"\nzzz_maybe_included();");
            check_eq((size_t)1, error_count(lit));
            check_true(lit.diagnostics[0].message.find("zzz_maybe_included") != std::string::npos);
        });

        test("forward_references_resolve", [this]() {
            auto e = make_engine();
            check_eq((size_t)0, error_count(e->check(
                "function a() { return b(); }\nfunction b() { return 41; }\na();")));
            check_eq((size_t)0, error_count(e->check(
                "function make_it() { return Later(); }\nclass Later { int v = 7; }\nmake_it();")));
            // a nested function sees no enclosing local, so a local declared after it is undeclared there
            check_eq((size_t)1, error_count(e->check(
                "function outer() { function inner() { return late_var; } int late_var = 5; return inner(); }")));
        });

        // ------------------------------------------------------------ script function calls

        test("script_function_arity_and_types", [this]() {
            auto e = make_engine();
            const char* fn = "function add(int a, int b) { return a + b; }\n";
            check_eq((size_t)0, error_count(e->check(std::string(fn) + "add(1, 2);")));
            check_eq((size_t)0, error_count(e->check(std::string(fn) + "add(true, 2);")));   // converts
            check_eq((size_t)1, error_count(e->check(std::string(fn) + "add(1.5, 2);")));
            check_eq((size_t)1, error_count(e->check(std::string(fn) + "add(1);")));
            check_eq((size_t)1, error_count(e->check(std::string(fn) + "add(1, 2, 3);")));
            check_eq((size_t)1, error_count(e->check(std::string(fn) + "add(\"x\", 2);")));
            // var/unknown args match anything
            check_eq((size_t)0, error_count(e->check(std::string(fn) + "var v = \"x\"; add(v, 2);")));
        });

        test("script_function_defaults_and_overloads", [this]() {
            auto e = make_engine();
            const char* fn = "function d(int a, int b = 2) { return a + b; }\n";
            check_eq((size_t)0, error_count(e->check(std::string(fn) + "d(1);")));
            check_eq((size_t)0, error_count(e->check(std::string(fn) + "d(1, 5);")));
            check_eq((size_t)1, error_count(e->check(std::string(fn) + "d();")));
            // overloads: error only when NO overload matches (pair types that both reject a string argument)
            const char* ov = "function pick(int a) { return 1; }\nfunction pick(char c) { return 2; }\n";
            check_eq((size_t)0, error_count(e->check(std::string(ov) + "pick(1);")));
            check_eq((size_t)0, error_count(e->check(std::string(ov) + "pick('c');")));
            check_eq((size_t)0, error_count(e->check(std::string(ov) + "pick(\"s\");")));   // a one-character string fills the char
            check_eq((size_t)1, error_count(e->check(std::string(ov) + "pick(\"st\");")));
            // a string overload takes text only: an array matches neither
            const char* ov2 = "function show(int a) { return 1; }\nfunction show(string s) { return 2; }\n";
            check_eq((size_t)1, error_count(e->check(std::string(ov2) + "show([1]);")));
        });

        test("return_type_checking", [this]() {
            auto e = make_engine();
            check_eq((size_t)0, error_count(e->check("int f() { return 5; } f();")));
            check_eq((size_t)0, error_count(e->check("float f() { return 2; } f();")));    // converts
            check_eq((size_t)1, error_count(e->check("int f() { return 2.5; } f();")));
            check_eq((size_t)1, error_count(e->check("string f() { return 5; } f();")));   // text only
            check_eq((size_t)1, error_count(e->check("int f() { return \"s\"; } f();")));
            check_eq((size_t)1, error_count(e->check("void f() { return 5; } f();")));
            // A structurally reachable fall-through violates a required result.
            auto falls = e->check("int f() { if (false) { return 1; } } f();");
            check_eq((size_t)1, error_count(falls));
            auto clean = e->check("int f() { if (false) { return 1; } return 2; } f();");
            check_eq((size_t)0, clean.diagnostics.size());
            // while(true) loops that can only exit via return don't warn
            auto spin = e->check("int f() { while (true) { return 1; } } f();");
            check_eq((size_t)0, spin.diagnostics.size());
        });

		test("required_results_are_checked_before_execution", [this]() {
			for (bool vm : {false, true}) {
				for (bool proofs : {false, true}) {
					auto e = jai::foundry::make_default_engine();
					e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
					e->type_proofs(proofs);
					check_true(e->check("int bad() {}").has_errors());
					check_true(e->check("bool bad() { return; }").has_errors());
					check_true(e->check("int& bad() {}").has_errors());
					check_false(e->check("int good() { if (true) { return 1; } }").has_errors());
					check_false(e->check("int good() { for (;;) {} }").has_errors());
					check_false(e->check("int good(int x) { switch (x) { case 0: fallthrough; default: return 1; } }").has_errors());
					check_true(e->check("int bad(int x) { switch (x) { case 0: if (x == 0) { break; } return 1; default: return 2; } }").has_errors());
					e->execute("int runs = 0;");
					check_throws([&]() { e->execute("runs += 1; int incomplete() {}"); });
					check_eq(int64_t{0}, e->get_variable("runs").as_int());
					check_true(e->execute("[[warn_mode]]; int unchecked() {} unchecked();").is_null(), "a warn region keeps the completion behavior");
				}
			}
		});

		test("known_reference_contracts_match_parameters_returns_and_grafts", [this]() {
			const std::string source = R"JAI(
				class Base { int value = 1; }
				class Derived : Base {}
				class Other { int value = 2; }
				Derived derived;
				Other other;
				Base& good() { return derived; }
				Base& bad() { return other; }
			)JAI";
			for (bool vm : {false, true}) {
				for (bool proofs : {false, true}) {
					auto e = jai::foundry::make_default_engine();
					e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
					e->type_proofs(proofs);
					check_true(e->check(source).has_errors());
					check_true(e->check("void take(array<int>& value) {} array<string> words = [\"a\"]; take(words);").has_errors());
					check_true(e->check("void take(math::vec2& value) {} math::vec3 v; take(v);").has_errors());
					e->execute("[[warn_mode]]; " + source);
					check_eq(int64_t{1}, e->execute("good().value;").as_int());
					check_true(e->execute("[[warn_mode]]; var m = \"\"; try { bad(); } catch (err) { m = err; } m;").as_string().starts_with("Cannot return reference"));
					check_true(e->execute("[[warn_mode]]; void take(array<int>& value) {} array<string> words = [\"a\"]; var m = \"\"; try { take(words); } catch (err) { m = err; } m;").as_string().starts_with("Cannot pass"));
					check_true(e->execute("[[warn_mode]]; int touch(int& value) { return value + 1; } string run() { float value = 1.5; try { return \"bad:\" + touch(value); } catch (err) { return err; } } run();").as_string().starts_with("Cannot pass"), "grafting cannot bypass a typed reference bind");
				}
			}
		});

        test("coroutines_stay_lenient", [this]() {
            auto e = make_engine();
            // a coroutine's returns/yields aren't held to the declared type in v1, and
            // calling one yields a handle, not the declared type
            check_eq((size_t)0, error_count(e->check(
                "coroutine int gen() { yield 1; yield 2; }\nauto g = gen();")));
        });

        // ------------------------------------------------------------ host surface

        test("host_function_signature_checking", [this]() {
            auto e = make_engine();
            e->add_function("square_probe", [](script_int n) -> script_int { return n * n; });
            check_eq((size_t)0, error_count(e->check("square_probe(4);")));
            check_eq((size_t)0, error_count(e->check("square_probe(2.5);")));   // numeric conversion
            check_eq((size_t)1, error_count(e->check("square_probe(\"s\");")));
            check_eq((size_t)1, error_count(e->check("square_probe(1, 2);")));
            check_eq((size_t)1, error_count(e->check("square_probe();")));
            // variadics accept anything
            check_eq((size_t)0, error_count(e->check("print(1, \"x\", [2]);")));
            // and the runtime agrees with the rejections
            check_throws([&]() { e->execute("square_probe(\"s\");"); });
        });

        test("host_class_member_checking", [this]() {
            auto e = make_engine();
            dynamic_binder<probe_object>(*e, "ProbeObject")
                .constructor<>()
                .method("doubled", &probe_object::doubled)
                .property("value", &probe_object::value)
                .build();
            const char* mk = "ProbeObject p = ProbeObject();\n";
            check_eq((size_t)0, error_count(e->check(std::string(mk) + "p.doubled();")));
            check_eq((size_t)0, error_count(e->check(std::string(mk) + "p.value;")));
            check_eq((size_t)0, error_count(e->check(std::string(mk) + "p.value = 5;")));
            check_eq((size_t)1, error_count(e->check(std::string(mk) + "p.zzz_nope();")));
            check_eq((size_t)1, error_count(e->check(std::string(mk) + "p.zzz_nope;")));
        });

        test("host_static_method_arity_is_a_check_error", [this]() {
            auto fresh = [](bool use_vm) {
                auto e = make_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                dynamic_binder<probe_object>(*e, "Gate")
                    .constructor<>()
                    .static_method("twice", [](script_int n) -> script_int { return n * 2; })
                    .static_method("pick", [](script_int a, script_int b) -> script_int { return a + b; })
                    .static_method("pick", [](script_int n) -> script_int { return n; })
                    .build();
                detail::engine_access::get_class_definition(*e, "Gate")->add_static_method("any", [raw = e.get()](const std::vector<script_value>& a) -> checked_result<script_value> {
                    return script_value(static_cast<script_int>(a.size()), raw);
                });
                return e;
            };
            auto e = fresh(false);
            auto r = e->check("Gate::twice();");
            check_eq((size_t)1, error_count(r));
            check_true(has_diag(r, dlevel::error, "'Gate::twice' expects 1 argument(s), got 0"), r.format());
            check_eq((size_t)1, error_count(e->check("Gate::twice(1, 2);")));
            check_eq((size_t)0, error_count(e->check("Gate::twice(4);")));
            // a second binding of a static name replaces the first, so the checker holds calls to the live one
            check_eq((size_t)0, error_count(e->check("Gate::pick(1);")));
            auto replaced = e->check("Gate::pick(1, 2);");
            check_eq((size_t)1, error_count(replaced));
            check_true(has_diag(replaced, dlevel::error, "'Gate::pick' expects 1 argument(s), got 2"), replaced.format());
            // a binding of unknown arity takes any count
            check_eq((size_t)0, error_count(e->check("Gate::any(); Gate::any(1, 2, 3);")));
            for (bool use_vm : {false, true}) {
                auto s = fresh(use_vm);
                bool static_err = false;
                try { s->execute("Gate::twice();"); } catch (const static_check_error&) { static_err = true; } catch (...) {}
                check_true(static_err, "strict rejects at load");
                check_eq((int64_t)8, s->execute("Gate::twice(4)").as_int());
                check_eq((int64_t)3, s->execute("Gate::any(1, 2, 3)").as_int());
                // warn: a warning, and the runtime raise stands
                auto w = s->check("[[warn_mode]];\nGate::twice();");
                check_eq((size_t)0, error_count(w));
                check_true(has_diag(w, dlevel::warning, "'Gate::twice' expects 1 argument(s), got 0"), w.format());
                check_eq(std::string("'Gate::twice' expects 1 argument, got 0"),
                         s->execute("[[warn_mode]]; var m = \"\"; try { Gate::twice(); } catch (err) { m = err; } m;").to_string());
                check_eq((int64_t)5, s->execute("Gate::pick(5)").as_int(), "the run time calls the later binding");
                check_eq(std::string("'Gate::pick' expects 1 argument, got 2"),
                         s->execute("[[warn_mode]]; var m = \"\"; try { Gate::pick(1, 2); } catch (err) { m = err; } m;").to_string());
            }
        });

        test("builtin_method_arity_counts_one_argument_in_the_singular", [this]() {
            auto e = make_engine();
            auto one = e->check("array<int> a = [1];\na.contains();");
            check_eq((size_t)1, error_count(one));
            check_true(has_diag(one, dlevel::error, "'contains' on 'array<int>' expects 1 argument, got 0"), one.format());
            auto two = e->check("string s = \"ab\";\ns.replace(\"a\");");
            check_eq((size_t)1, error_count(two));
            check_true(has_diag(two, dlevel::error, "'replace' on 'string' expects 2 arguments, got 1"), two.format());
            auto range = e->check("array<int> a = [1];\na.join(\",\", \"x\");");
            check_eq((size_t)1, error_count(range));
            check_true(has_diag(range, dlevel::error, "'join' on 'array<int>' expects 0 to 1 arguments, got 2"), range.format());
        });

        test("read_only_property_store_is_a_check_error", [this]() {
            // A store to a getter-only binder property is the mistake the runtime raises
            // (`Cannot assign to 'ro': it is read-only`); on a closed class the checker
            // reports it at load, in its own voice.
            const char* text = "cannot assign to read-only property 'ro'";
            auto fresh = [](bool use_vm) {
                auto e = make_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                bind_read_only_probe(*e);
                return e;
            };
            auto e = fresh(false);
            const std::string mk = "RoProbe h = RoProbe();\n";
            const std::string hybrid = "class W : RoProbe { W() : super() {} ";
            for (const char* bad : {"h.ro = 5;", "h.ro += 1;", "h.ro++;", "--h.ro;", "h.ro *= 2;"}) {
                auto r = e->check(mk + bad);
                check_eq((size_t)1, error_count(r), bad);
                check_true(has_diag(r, dlevel::error, text), bad);
            }
            for (const char* body : {"void go() { ro = 5; }", "void go() { ro += 1; }", "void go() { ro++; }",
                                     "void go() { --ro; }", "void go() { this.ro = 5; }", "void go() { this.ro += 1; }",
                                     "void go() { this.ro++; }"}) {
                auto r = e->check(hybrid + body + " }");
                check_eq((size_t)1, error_count(r), body);
                check_true(has_diag(r, dlevel::error, text), body);
            }
            // the getter reads; properties with a setter store; overrides and shadows are lenient
            check_eq((size_t)0, error_count(e->check(mk + "int x = h.ro;")));
            check_eq((size_t)0, error_count(e->check(mk + "h.value = 5; h.value += 1; h.value++; h.lam = 5; h.lam++;")));
            check_eq((size_t)0, error_count(e->check(hybrid + "void go() { value = 5; lam += 1; value++; this.value = 2; int x = ro; } }")));
            check_eq((size_t)0, error_count(e->check(hybrid + "int stored = 0; void _set_ro(int v) override { stored = v; } void go() { ro = 5; this.ro += 1; } }"
                                                     "W w = W(); w.ro = 5; w.ro++;")));
            check_eq((size_t)0, error_count(e->check(hybrid + "int ro = 1; void go() { ro = 5; this.ro++; } } W w = W(); w.ro = 2;")));
            check_eq((size_t)0, error_count(e->check(mk + "var v = h; v.ro = 5; v.ro++;")));
            // strict: the load fails before anything runs, on both backends
            for (bool use_vm : {false, true}) {
                auto s = fresh(use_vm);
                check_throws([&]() { s->execute(mk + "h.ro = 5;"); });
                check_throws([&]() { s->execute(hybrid + "void go() { ro++; } } W w = W(); w.go();"); });
                bool static_err = false;
                try { s->execute(mk + "h.ro = 5;"); } catch (const static_check_error&) { static_err = true; } catch (...) {}
                check_true(static_err, "strict rejects at load");
            }
            // warn: a warning, and the runtime raise stands
            for (bool use_vm : {false, true}) {
                auto w = fresh(use_vm);
                auto r = w->check("[[warn_mode]]\n" + mk + "h.ro = 5;");
                check_eq((size_t)0, error_count(r));
                check_eq((size_t)1, warning_count(r));
                check_true(has_diag(r, dlevel::warning, text));
                std::string got;
                try { w->execute("[[warn_mode]]\n" + mk + "h.ro = 5;"); } catch (const std::exception& ex) { got = ex.what(); }
                check_eq(std::string("Cannot assign to 'ro': it is read-only"), got);
                check_eq((int64_t)100, w->execute("h.ro").as_int(), "the getter still reads");
            }
        });

        // ------------------------------------------------------------ script classes

        test("script_class_members_and_field_types", [this]() {
            auto e = make_engine();
            const char* cls =
                "class Cat {\n"
                "    int lives = 9;\n"
                "    void meow() {}\n"
                "    int fed(int amount) { return lives + amount; }\n"
                "}\n"
                "Cat c = Cat();\n";
            check_eq((size_t)0, error_count(e->check(std::string(cls) + "c.lives = 5; c.meow(); c.fed(2);")));
            check_eq((size_t)1, error_count(e->check(std::string(cls) + "c.zzz_nope();")));
            check_eq((size_t)1, error_count(e->check(std::string(cls) + "c.zzz_nope = 1;")));
            // typed fields enforce like locals (declared field types are real)
            check_eq((size_t)1, error_count(e->check(std::string(cls) + "c.lives = \"s\";")));
            check_eq((size_t)1, error_count(e->check(std::string(cls) + "c.fed(\"s\");")));
            check_eq((size_t)1, error_count(e->check(std::string(cls) + "c.meow(1);")));
            check_eq((size_t)1, error_count(e->check(
                "class Dog { int teeth = \"many\"; }")));   // field initializer violates declared type
            // methods see fields bare, and typed-field stores inside methods check too
            check_eq((size_t)0, error_count(e->check(
                "class A { int v = 0; void set(int nv) { v = nv; } }")));
            check_eq((size_t)1, error_count(e->check(
                "class A { int v = 0; void bad() { v = \"s\"; } }")));
        });

        test("script_class_constructors", [this]() {
            auto e = make_engine();
            const char* cls = "class Pt { int x = 0; Pt(int nx) { x = nx; } }\n";
            check_eq((size_t)0, error_count(e->check(std::string(cls) + "Pt p = Pt(3);")));
            check_eq((size_t)1, error_count(e->check(std::string(cls) + "Pt p = Pt();")));
            check_eq((size_t)1, error_count(e->check(std::string(cls) + "Pt p = Pt(\"s\");")));
            // no declared ctor: default, copy, and aggregate initialization (one initializer per field, each typed)
            const char* plain = "class Bare { int v = 1; }\n";
            check_eq((size_t)0, error_count(e->check(std::string(plain) + "Bare b = Bare();")));
            check_eq((size_t)0, error_count(e->check(std::string(plain) + "Bare b = Bare(Bare(4));")));
            check_eq((size_t)0, error_count(e->check(std::string(plain) + "Bare b = Bare(1); Bare c{2}; Bare d = Bare{3};")));
            check_eq((size_t)1, error_count(e->check(std::string(plain) + "Bare b = Bare(1, 2);")));
            check_eq((size_t)1, error_count(e->check(std::string(plain) + "Bare b{\"s\"};")));
            check_eq((size_t)1, error_count(e->check("class Hid { private: int v = 1; }\nHid h{1};")));
        });

        test("inheritance_members_and_assignability", [this]() {
            auto e = make_engine();
            const char* hier =
                "class Animal { int hp = 10; void heal() { hp = hp + 1; } }\n"
                "class Tiger : Animal { void roar() {} }\n";
            check_eq((size_t)0, error_count(e->check(std::string(hier) +
                "Tiger t = Tiger(); t.roar(); t.heal(); t.hp = 5;")));
            check_eq((size_t)1, error_count(e->check(std::string(hier) +
                "Tiger t = Tiger(); t.zzz_nope();")));
            check_eq((size_t)1, error_count(e->check(std::string(hier) +
                "Tiger t = Tiger(); t.hp = \"s\";")));   // inherited typed field
            // derived -> base ok; base -> derived is not
            check_eq((size_t)0, error_count(e->check(std::string(hier) + "Animal a = Tiger();")));
            check_eq((size_t)1, error_count(e->check(std::string(hier) + "Tiger t = Animal();")));
            // unknown base class opens the hierarchy: no member errors
            check_eq((size_t)0, error_count(e->check(
                "include \"base.jai\"\nclass Sub : MysteryBase {}\nSub s = Sub();\ns.whatever();")));
        });

        // ------------------------------------------------------------ accumulation / recovery / cap

        test("accumulates_all_errors_sorted", [this]() {
            auto e = make_engine();
            auto r = e->check(
                "var ok = 0;\n"          // 1: clean
                "int b = \"x\";\n"       // 2: error
                "int c = \"y\";\n"       // 3: error
                "zzz_undefined();\n");   // 4: error
            check_eq((size_t)3, error_count(r));
            check_eq((size_t)2, r.diagnostics[0].line);
            check_eq((size_t)3, r.diagnostics[1].line);
            check_eq((size_t)4, r.diagnostics[2].line);
            check_gt(r.diagnostics[0].column, (size_t)0);
            // formatted output is compiler-style with source line + caret
            std::string formatted = r.format("var ok = 0;\nint b = \"x\";\nint c = \"y\";\nzzz_undefined();\n");
            check_true(formatted.find("<script>:2:") != std::string::npos);
            check_true(formatted.find("error:") != std::string::npos);
            check_true(formatted.find("int b = \"x\";") != std::string::npos);
            check_true(formatted.find("^") != std::string::npos);
        });

        test("recovery_no_cascades_and_dedupe", [this]() {
            auto e = make_engine();
            // after the bad init, x keeps its declared type: downstream uses stay clean
            auto r = e->check("int x = \"s\";\nint y = x + 1;\nint z = x * 2;");
            check_eq((size_t)1, error_count(r));
        });

        test("diagnostic_cap_at_100", [this]() {
            auto e = make_engine();
            std::string src;
            for (int i = 0; i < 120; ++i) {
                src += "int v" + std::to_string(i) + " = \"s\";\n";
            }
            auto r = e->check(src);
            check_eq(detail::k_check_diagnostic_cap, r.diagnostics.size());
            check_true(r.capped);
        });

        // ------------------------------------------------------------ strict / warn gating

        test("strict_throws_before_anything_runs", [this]() {
            auto e = make_engine();
            e->execute("var sentinel = 0;");
            bool threw = false;
            std::string message;
            try {
                // the assignment line is checker-clean; the error is later — nothing
                // may run anyway (side-effect-free proof)
                e->execute("sentinel = 5;\nint oops = \"s\";");
            } catch (const static_check_error& err) {
                threw = true;
                message = err.what();
            }
            check_true(threw);
            check_true(message.find("error:") != std::string::npos);
            check_true(message.find("oops") != std::string::npos);
            check_eq((int64_t)0, e->execute("sentinel").as_int());
        });

        test("strict_allows_clean_scripts", [this]() {
            auto e = make_engine();
            check_eq((int64_t)9, e->execute("int f(int v) { return v * 3; } f(3)").as_int());
        });

        test("warn_region_executes_and_diagnostics_are_retrievable", [this]() {
            auto e = make_engine();
            // checker-error in dead code: runtime-clean, still diagnosed, as a warning
            auto result = e->execute("[[warn_mode]]; var flag = false;\nif (flag) { zzz_nope(); }\n7");
            check_eq((int64_t)7, result.as_int());
            check_false(e->last_check_diagnostics().has_errors());
            check_true(has_diag(e->last_check_diagnostics(), dlevel::warning, "use of undeclared identifier 'zzz_nope'"));
            check_eq((size_t)2, e->last_check_diagnostics().diagnostics[0].line);
        });

        // ------------------------------------------------------------ hot reload / epoch

        test("registration_epoch_recheck", [this]() {
            auto e = make_engine();
            bool threw = false;
            try { e->execute("mystery_fn_zz(1);"); } catch (const static_check_error&) { threw = true; }
            check_true(threw);
            // registering the function bumps the surface epoch: the SAME cached source
            // re-checks clean and runs
            e->add_function("mystery_fn_zz", [](script_int v) -> script_int { return v + 1; });
            check_eq((int64_t)2, e->execute("mystery_fn_zz(1)").as_int());
        });

        test("hot_reload_new_source_is_rechecked", [this]() {
            auto e = make_engine();
            e->execute("class Hot { int v = 1; }\nHot h = Hot();\nh.v;");
            // reload with a changed class body (new source = new cache entry = checked):
            // the stale member access is caught against the class defined IN that source
            bool threw = false;
            try {
                e->execute("class Hot { int w = 2; }\nHot h = Hot();\nh.v;");
            } catch (const static_check_error&) {
                threw = true;
            }
            check_true(threw);
        });

        // ------------------------------------------------------------ jaibite

        test("jaibite_strict_gates_creation", [this]() {
            auto e = make_engine();
            auto bite = e->jaibite("int a = 20; a * 2");
            check_true(bite.checked_clean());
            check_eq((int64_t)40, bite.execute().as_int());
            check_throws([&]() { e->jaibite("int bad = \"s\";"); });
        });

        test("jaibite_warn_region_carries_diagnostics", [this]() {
            auto e = make_engine();
            auto bite = e->jaibite("[[warn_mode]]; int bad = \"s\";");
            check_true(bite.checked_clean());   // no error-severity diagnostic: the region's errors are warnings
            check_true(has_diag(bite.check_diagnostics(), dlevel::warning, "cannot initialize 'bad' declared 'int' with 'string'"));
            check_true(has_diag(e->last_check_diagnostics(), dlevel::warning, "cannot initialize 'bad' declared 'int' with 'string'"));
        });

        test("jaibite_save_load_stamp", [this]() {
            auto e = make_engine();
            auto bite = e->jaibite("int a = 5; a + 1");
            check_true(bite.checked_clean());
            auto bytes = bite.save_bytes();

            // same engine: fingerprint matches, the checked-clean stamp is trusted
            auto loaded = e->jaibite_load_bytes(bytes);
            check_false(loaded.registration_mismatch());
            check_true(loaded.checked_clean());
            check_eq((int64_t)6, loaded.execute().as_int());

            // unstamped bytes (no checker saw them) load unchecked, then are re-checked on load
            auto unchecked_bytes = unstamped_bite_bytes(*e, "int b = 7; b");
            auto reloaded = e->jaibite_load_bytes(unchecked_bytes);
            check_true(reloaded.checked_clean());   // re-checked at load

            // mismatched registration surface: the stamp is NOT trusted, the bite is re-checked instead
            auto e3 = make_engine();
            e3->add_function("extra_reg_fn_zz", [](script_int v) -> script_int { return v; });
            auto loaded3 = e3->jaibite_load_bytes(bytes);
            check_true(loaded3.registration_mismatch());
            check_true(loaded3.checked_clean());
            check_eq((int64_t)6, loaded3.execute().as_int());

            // the load of a bad bite throws at load, before anything runs
            auto bad_bytes = unstamped_bite_bytes(*e3, "int bad = \"s\";");
            check_throws([&]() { e3->jaibite_load_bytes(bad_bytes); });

            // the stamp vouches for the names the bite resolved where it was checked: an engine where they mean something else
            // checks the saved bite again, with the verdict a fresh bite of the same source gets there
            const std::string reader = "int stamp_read() { return stamp_seen; }\nint stamp_count(StampBox b) { return b.n; }";
            auto host = make_engine();
            host->execute("int stamp_seen = 4; class StampBox { int n = 1; }");
            const auto resolved = host->jaibite(reader).save_bytes();
            const auto verdicts = [&](const std::string& setup) {
                auto eng = make_engine();
                eng->execute(setup);
                const auto verdict = [](const auto& load) {   // a loaded bite's listing has no source to quote: its diagnostic lines
                    std::string result = "clean";
                    try { load(); } catch (const static_check_error& ex) {
                        result.clear();
                        std::istringstream lines(ex.what());
                        for (std::string line; std::getline(lines, line);) { if (line.find("error:") != std::string::npos) { result += line + "\n"; } }
                    }
                    return result;
                };
                return std::make_pair(verdict([&] { eng->jaibite(reader); }), verdict([&] { eng->jaibite_load_bytes(resolved); }));
            };
            check_true(verdicts("0;").first != "clean", "a fresh bite is refused where stamp_seen is undeclared");
            for (const std::string setup : {"0;", "int stamp_seen = 1;", "class StampBox { string n = \"s\"; } int stamp_seen = 9;"}) {
                const auto [fresh, loaded] = verdicts(setup);
                check_eq(fresh, loaded, setup);
            }
            check_eq(std::string("clean"), verdicts("int stamp_seen = 9; class StampBox { int n = 2; }").second, "the same declarations keep the stamp");
        });

        test("jaibite_stamp_from_another_checker_revision_rechecks", [this]() {
            // a checked-clean stamp vouches for the checker that gave it: one from an older revision (rules that have since
            // grown) is checked again on load, one from this revision is trusted
            auto e = make_engine();
            auto bytes = unstamped_bite_bytes(*e, "int bad = \"s\";");
            auto stamp = [&bytes](uint32_t flags) {
                std::vector<uint8_t> stamped = bytes;
                for (int i = 0; i < 4; ++i) { stamped[8 + i] = static_cast<uint8_t>(flags >> (8 * i)); }
                return stamped;
            };
            const auto older = stamp(jai::detail::k_jaibite_flag_checked_clean | ((jai::detail::k_jaibite_checker_revision - 1) << 8));
            check_throws<static_check_error>([&]() { e->jaibite_load_bytes(older); }, "an older revision's stamp re-checks and finds the error");
            auto current = e->jaibite_load_bytes(stamp(jai::detail::jaibite_checked_clean_flags()));
            check_true(current.checked_clean(), "this revision's stamp is trusted");
        });

        test("jaibite_checks_at_creation_and_at_load", [this]() {
            const char* bad = "int bad = \"s\";";
            // strict (default): checked at creation, the listing names the typed mismatch
            auto strict = make_engine();
            std::string text;
            try { strict->jaibite(bad); } catch (const static_check_error& ex) { text = ex.what(); }
            check_true(text.find("cannot initialize 'bad' declared 'int' with 'string'") != std::string::npos,
                "strict creation rejects the typed mismatch (got: " + text + ")");
            // unstamped bytes load re-checked, so the load rejects them before any execute
            auto e = make_engine();
            const auto unchecked_bytes = unstamped_bite_bytes(*e, bad);
            check_throws<static_check_error>([&]() { e->jaibite_load_bytes(unchecked_bytes); });
            // a warn region: created with diagnostics; execute reaches the runtime's own error
            auto warn = make_engine();
            auto flagged = warn->jaibite(std::string("[[warn_mode]]; ") + bad);
            check_true(has_diag(flagged.check_diagnostics(), dlevel::warning, "cannot initialize 'bad' declared 'int' with 'string'"));
            text.clear();
            try { flagged.execute(); }
            catch (const static_check_error&) { text = "static_check_error"; }
            catch (const std::exception& ex) { text = ex.what(); }
            check_true(text.find("Cannot initialize 'bad' (declared int) with string") != std::string::npos,
                "a warn region executes into the runtime error (got: " + text + ")");
            // strict admits the var-sourced twin; the same runtime error fires at execute
            auto var_bite = strict->jaibite("var s = \"s\"; int bad = s;");
            check_true(var_bite.checked_clean());
            text.clear();
            try { var_bite.execute(); } catch (const std::exception& ex) { text = ex.what(); }
            check_true(text.find("Cannot initialize 'bad' (declared int) with string") != std::string::npos,
                "var-sourced store keeps the runtime text under strict (got: " + text + ")");
        });

        // ------------------------------------------------------------ misc surface

        test("lenient_surfaces_stay_silent", [this]() {
            auto e = make_engine();
            // builtin container/string methods, subscripts, maps, enums, destructuring
            check_eq((size_t)0, error_count(e->check(
                "array<int> a = [1, 2, 3];\na.push(4);\na[0] = 9;\nint n = a.size();")));
            check_eq((size_t)0, error_count(e->check(
                "map<string, int> m = {\"a\": 1};\nm[\"b\"] = 2;")));
            check_eq((size_t)0, error_count(e->check(
                "string s = \"abc\";\nint n = s.length();")));
            check_eq((size_t)0, error_count(e->check(
                "enum Direction { north, south }\nauto d = Direction.north;")));
            check_eq((size_t)1, error_count(e->check(
                "enum Direction { north, south }\nauto d = Direction.zzz_west;")));
            check_eq((size_t)0, error_count(e->check(
                "auto [x, y] = [1, 2];\nx + y;")));
            // lambdas: params + captures resolve, bodies get checked
            check_eq((size_t)0, error_count(e->check(
                "int base = 2;\nauto f = [=](int v) { return v + base; };\nf(3);")));
            check_eq((size_t)1, error_count(e->check(
                "auto f = [](int v) { int q = \"s\"; return v; };")));
        });

        test("ternary_and_operator_inference", [this]() {
            auto e = make_engine();
            check_eq((size_t)0, error_count(e->check("bool c = true; int x = c ? 1 : 2;")));
            check_eq((size_t)1, error_count(e->check("bool c = true; int x = c ? \"a\" : \"b\";")));
            check_eq((size_t)1, error_count(e->check("int x = 1 < 2;\nchar c = x;")));   // bool -> char? no: int decl from bool ok; char from int errors
            check_eq((size_t)1, error_count(e->check("char c = \"a\" + \"b\";")));       // string into char
            // a string's + joins text: a number, bool, char or null beside it is the run-time text at load, in either order; a
            // template string's own + makes its splice text
            for (const auto& [source, text] : std::vector<std::pair<std::string, std::string>>{
                     {"string s = 1 + \"b\";", "cannot apply '+' to 'int' and 'string': stringify it with to_string(x) or `${x}`"},
                     {"string s = \"b\" + 2.5;", "cannot apply '+' to 'string' and 'float': stringify it with to_string(x) or `${x}`"},
                     {"bool f = true; string s = \"b\" + f;", "cannot apply '+' to 'string' and 'bool': stringify it with to_string(x) or `${x}`"},
                     {"string s = \"b\" + null;", "cannot apply '+' to 'string' and 'null': stringify it with to_string(x) or `${x}`"}}) {
                auto r = e->check(source);
                check_eq((size_t)1, error_count(r), source);
                check_true(has_diag(r, dlevel::error, text), source + "\n" + r.format(source));
            }
            check_eq((size_t)0, error_count(e->check("int n = 1; char c = 'c'; string s = `${n}${c}${null}${true}` + \"b\";")), "template splices are text");
            check_eq((size_t)0, error_count(e->check("[[warn_mode]]; string s = 1 + \"b\";")));
        });

        // A mixed-branch ternary has no single static type, so 'auto x = c ? 0 : 1.5' is a var;
        // the warning says so, since the spelling reads like a typed
        // slot. Typed decls absorb the join and var stays dynamic.
        test("warn_auto_from_mixed_branch_ternary", [this]() {
            auto e = make_engine();
            auto r = e->check("bool c = true; auto x = c ? 0 : \"s\";");
            check_eq((size_t)0, error_count(r));
            check_eq((size_t)1, warning_count(r));
            check_true(r.diagnostics[0].message.find("'int'") != std::string::npos &&
                       r.diagnostics[0].message.find("'string'") != std::string::npos);
            check_true(r.diagnostics[0].message.find("so 'x' is a var") != std::string::npos);

            // container/class-shaped mixes warn too, naming both shapes
            auto rc = e->check("bool c = true; auto a = c ? [1] : {\"k\": 1};");
            check_eq((size_t)1, warning_count(rc));
            check_true(rc.diagnostics[0].message.find("'array'") != std::string::npos &&
                       rc.diagnostics[0].message.find("'map'") != std::string::npos);

            // silent: matched branches, typed decl, var decl, non-ternary initializer
            check_eq((size_t)0, e->check("bool c = true; auto x = c ? 1 : 2;").diagnostics.size());
            check_eq((size_t)0, e->check("bool c = true; auto x = c ? 0 : 1.5;").diagnostics.size());   // int with float: float
            check_eq((size_t)0, e->check("bool c = true; float x = c ? 0 : 1.5;").diagnostics.size());
            check_eq((size_t)0, e->check("bool c = true; var x = c ? 0 : 1.5;").diagnostics.size());
            check_eq((size_t)0, e->check("auto x = 1.5;").diagnostics.size());
        });

        // ------------------------------------------------------------ shadowing warnings
        // Always WARNING severity (shadowing is legal; strict must not throw on it).

        test("a_scope_declares_a_name_once", [this]() {
            auto e = make_engine();
            const std::string placeholder = "'_' is declared more than once in this scope, so it names none of them: give the variable you use its own name";
            for (const auto& [source, text] : std::vector<std::pair<std::string, std::string>>{
                {"int f(int a, int a) { return a; }", "redeclaration of 'a' in the same scope"},
                {"auto g = [](int a, string a) { return a; };", "redeclaration of 'a' in the same scope"},
                {"void f() { int y = 1; string y = \"s\"; }", "redeclaration of 'y' in the same scope"},
                {"{ int y = 1; int y = 2; }", "redeclaration of 'y' in the same scope"},
                {"void f() { auto [a, b] = [1, 2]; int a = 3; }", "redeclaration of 'a' in the same scope"},
                {"class C { int x; string x; }", "redeclaration of member 'x' in class 'C'"},
                {"class C { int x; void x() {} }", "redeclaration of member 'x' in class 'C'"},
                {"void f(int _, int _) { }", "redeclaration of '_' in the same scope"},   // a parameter is never a placeholder (C++26)
                {"void f() { auto _ = 1; auto _ = 2; print(_); }", placeholder},
                {"void f() { auto _ = 1; auto [_, b] = [1, 2]; _ = b; }", placeholder},
                {"void f(int _) { auto _ = 2; { print(_); } }", placeholder},
                {"auto _ = 1; auto _ = 2; print(_);", placeholder},
                {"int y = 1; string y = \"s\";", "redeclaration of 'y' in the same scope"},   // one unit's top level is a scope

                {"auto [a, b] = [1, 2]; int z = 2; string z = \"s\"; string w = z;", "redeclaration of 'z' in the same scope"},
                {"auto [a, b] = [1, 2]; int b = 3;", "redeclaration of 'b' in the same scope"}
            }) {
                auto report = e->check(source);
                check_true(has_diag(report, dlevel::error, text), source + "\n" + report.format(source));
            }
            for (const char* source : {
                "void f() { int y = 1; { string y = \"s\"; } }",       // a nested block's own name shadows (a warning)
                "extern int y; int y = 1; int n = y;",                   // an extern's definition
                "inline int y = 1; inline int y = 2; int y = 3;",        // an inline declaration adopts the name; the unit's plain one turns it
                "int y = 1; { int y = 2; } if (y == 1) { string y = \"s\"; }",
                "class C { void f() {} void f(int a) {} }",              // overloads
                "void f(int a) { for (int i = 0; i < 1; i++) {} for (int i = 0; i < 1; i++) {} }",
                "void f(int _) { auto _ = 1; auto _ = 2; auto [_, _] = [3, 4]; { auto _ = 5; print(_); } }",   // C++26's placeholder
                "void f() { auto _ = 1; print(_); }"                                                             // one '_' is a variable
            }) {
                auto report = e->check(source);
                check_eq(std::size_t(0), report.error_count(), std::string(source) + "\n" + report.format(source));
                if (std::string(source).find('_') != std::string::npos) { check_eq(std::size_t(0), report.diagnostics.size(), report.format(source)); }
            }
            const std::string twice = "int y = 1;\nvar y = [1];";
            check_true(e->check(twice).format(twice).find("redeclaration of 'y' in the same scope (declared at 1:5)") != std::string::npos, "the first declaration's position");
            // a later run of the program declares its names again: the later declaration stands
            auto rerun = make_engine();
            rerun->execute("int y = 1;");
            check_eq(std::string("s"), rerun->execute("string y = \"s\"; y;").as_string());
            // every placeholder lives to the end of its scope and dies in reverse order, as a C++ local does
            auto run = make_engine();
            check_eq(std::string("open a,open b,close b,close a,"), run->execute(
                "string log = \"\";\nclass G { string n = \"\"; G(string a) { n = a; log += \"open \" + n + \",\"; } ~G() { log += \"close \" + n + \",\"; } }\n"
                "void f() { auto _ = G(\"a\"); auto _ = G(\"b\"); }\nf();\nlog;").as_string());
        });

		test("lambda_capture_lists_are_exhaustive", [this]() {
			for (auto backend : {backend_type::interpreter, backend_type::vm}) {
				{
					for (const auto& source : {
						"auto f() { int x = 5; return []() { return x; }; } f()",
						"auto f() { int x = 1; int y = 2; return [x]() { return x + y; }; } f()",
						"auto f() { int x = 1; return []() { return [=]() { return x; }; }; } f()",
						"auto f() { int x = 1; return []() { return [x]() { return x; }; }; } f()",
						"class C { int x = 1; auto f() { return []() { return this.x; }; } } C c = C(); c.f()",
						"class C { int x = 1; auto f() { return []() { return x; }; } } C c = C(); c.f()"
					}) {
						auto e = make_engine();
						e->set_backend(backend);
						auto result = outcome(*e, source);
						check_true(result.rfind("ERROR", 0) == 0 && result.find("capture") != std::string::npos, result);
					}
				}
				for (const auto& source : {
					"auto f() { int x = 3; return [=]() { return x; }; } auto g = f(); g()",
					"auto f() { int x = 3; return [x]() { return x; }; } auto g = f(); g()",
					"auto f() { int x = 3; return [copy = x]() { return copy; }; } auto g = f(); g()",
					"auto f() { int x = 3; return [=]() { return [=]() { return x; }; }; } auto outer = f(); auto inner = outer(); inner()",
					"auto f() { int x = 3; return [x]() { return [x]() { return x; }; }; } auto outer = f(); auto inner = outer(); inner()",
					"int shared = 3; auto f() { return []() { return shared; }; } auto g = f(); g()",
					"auto f() { return [](int n) { int m = n; return m; }; } auto g = f(); g(3)",
					"namespace N { int x = 2; auto f() { return []() { return x; }; } } auto g = N::f(); N::x = 3; g()"
				}) {
					auto e = make_engine();
					e->set_backend(backend);
					check_eq(std::string("VALUE 3"), outcome(*e, source));
				}
				auto e = make_engine();
				e->set_backend(backend);
				check_eq(std::string("VALUE 4"), outcome(*e,
					"auto f() { int x = 3; auto g = [&]() { x += 1; return x; }; g(); return x; } f()"));
				auto warned = make_engine();
				warned->set_backend(backend);
				check_eq(std::string("VALUE 12|33"), outcome(*warned,
					"[[warn_mode]]; class C { int x = 1; auto f() { return []() { return x; }; } auto g(int k) { return [k]() { return k; }; } } C c = C(); string m = \"\"; "
					"try { c.f(); } catch (e) { m += \"1\"; } try { c.f(); } catch (e) { m += \"2\"; } `${m}|${c.g(3)()}${c.g(3)()}`"),
					"an unverified capture list raises at every creation; a verified one is checked once");
			}
		});

		test("documented_container_and_instance_capture_contracts", [this]() {
			for (auto backend : {backend_type::interpreter, backend_type::vm}) {
				auto e = make_engine();
				e->set_backend(backend);
				check_eq(std::string("VALUE 22"), outcome(*e,
					"array<int> a = [1]; var b = a; var c = array::slice(a, 0, 1); int caught = 0; "
					"try { b.push(\"x\"); } catch (err) { caught += 1; } "
					"try { c.push(\"x\"); } catch (err) { caught += 1; } "
					"array<var> d = a; d.push(\"x\"); caught * 10 + d.size();"));
				check_eq(std::string("VALUE 4"), outcome(*e,
					"class C { int hp = 3; auto callback() { return [this]() { hp += 1; return hp; }; } } "
					"C owner = C(); auto callback = owner.callback(); callback(); owner.hp;"));
				const std::string source = "auto incomplete = []() -> int { };";
				check_eq(std::size_t(1), error_count(e->check(source)));
			}
		});

        test("shadow_local_over_field", [this]() {
            auto e = make_engine();
            auto r = e->check("class Creature {\n  int hp = 10;\n  function hit() -> void { var hp = 3; }\n}");
            check_eq((size_t)0, error_count(r));
            check_eq((size_t)1, warning_count(r));
            check_true(r.diagnostics[0].message.find(
                "local 'hp' shadows field 'hp' of class 'Creature' (field declared at 2:") != std::string::npos);
        });

        test("shadow_param_over_field_but_not_ctor_params", [this]() {
            auto e = make_engine();
            auto r = e->check("class C {\n  int hp = 1;\n  function heal(int hp) -> void { this.hp += hp; }\n}");
            check_eq((size_t)1, warning_count(r));
            check_true(r.diagnostics[0].message.find("parameter 'hp' shadows field 'hp'") != std::string::npos);
            // C(int hp) { this.hp = hp; } is the established idiom - silent
            check_eq((size_t)0, e->check(
                "class C {\n  int hp = 1;\n  C(int hp) { this.hp = hp; }\n}").diagnostics.size());
        });

        test("shadow_rangefor_and_catch_over_field", [this]() {
            auto e = make_engine();
            auto r = e->check(
                "class C {\n  var items = [1];\n  function each() -> void {\n"
                "    for (var items : [2]) { var y = items; }\n"
                "    try { throw \"e\"; } catch (items) { }\n  }\n}");
            check_eq((size_t)2, warning_count(r));
            check_true(r.diagnostics[0].message.find("range-for variable 'items' shadows field") != std::string::npos);
            check_true(r.diagnostics[1].message.find("catch variable 'items' shadows field") != std::string::npos);
        });

        test("shadow_inherited_and_static_fields", [this]() {
            auto e = make_engine();
            auto r = e->check(
                "class Base { int bv = 5; }\n"
                "class D : Base { function m() -> void { var bv = 1; } }");
            check_eq((size_t)1, warning_count(r));
            check_true(r.diagnostics[0].message.find("shadows field 'bv' of class 'Base'") != std::string::npos);
            auto r2 = e->check(
                "class S {\n  static int count = 0;\n  function m() -> void { var count = 1; }\n}");
            check_eq((size_t)1, warning_count(r2));
            check_true(r2.diagnostics[0].message.find("shadows static field 'count'") != std::string::npos);
        });

        test("shadow_outer_local", [this]() {
            auto e = make_engine();
            auto r = e->check(
                "function f() -> void {\n  var x = 1;\n  if (x > 0) { var x = 2; }\n}");
            check_eq((size_t)1, warning_count(r));
            check_true(r.diagnostics[0].message.find(
                "local 'x' shadows a declaration in an enclosing scope (declared at 2:") != std::string::npos);
        });

        test("shadow_global_only_when_used_earlier", [this]() {
            auto e = make_engine();
            // read the global, then declare a local with the same spelling: warn
            auto r = e->check(
                "var total = 0;\nfunction f() -> void {\n  total += 1;\n  var total = 5;\n}");
            check_eq((size_t)1, warning_count(r));
            check_true(r.diagnostics[0].message.find(
                "local 'total' shadows global 'total' used earlier in this function") != std::string::npos);
            // plain local-over-global with no earlier use: silent (too common to flag)
            check_eq((size_t)0, e->check(
                "var total = 0;\nfunction f() -> void { var total = 5; }").diagnostics.size());
        });

        test("shadow_warnings_never_gate_strict", [this]() {
            auto e = make_engine();
            // strict throws on errors only; a shadow warning still executes
            check_eq((int64_t)3, e->execute(
                "class K { int hp = 10; function m() -> int { var hp = 3; return hp; } }\n"
                "K().m()").as_int());
            check_eq((size_t)1, e->last_check_diagnostics().diagnostics.size());
        });

        test("shadow_lambda_field_visibility", [this]() {
            auto e = make_engine();
            // [=] lambdas cannot reach fields (audited): a same-named lambda local is
            // NOT a field shadow. [this] lambdas can: warn.
            check_eq((size_t)0, e->check(
                "class C {\n  int v = 1;\n  function m() -> void { var f = [=]() { var v = 2; return v; }; }\n}").diagnostics.size());
            auto r = e->check(
                "class C {\n  int v = 1;\n  function m() -> void { var f = [this]() { var v = 2; return v; }; }\n}");
            check_eq((size_t)1, warning_count(r));
        });

        // ------------------------------------------------------- field/global ambiguity
        // Runtime bare-name resolution reaches GLOBALS before this-fields (audited,
        // docs/site/guide/07-classes.html): a name that is both is flow-order-ambiguous
        // to the static walk, so member hits on it degrade to unknown. Pinned here
        // because the old field-first inference produced a strict-mode false positive.

        test("field_global_collision_follows_the_runtime_order", [this]() {
            // docs/site/guide/07-classes.html: a bare name in a method resolves locals, then members, then globals, so the
            // checker types the field (the run time reads and writes it) and ::name reaches the global
            auto e = make_engine();
            check_eq((size_t)1, error_count(e->check(
                "class C { string count = \"s\"; function probe() -> void { int x = count; } }\n"
                "var count = 5;\nC().probe();")), "a read takes the field's type");
            check_eq((size_t)1, error_count(e->check(
                "int count = 5;\nclass C { int count = 3; function poke() -> void { count = \"s\"; } }\nC().poke();")),
                "a write takes the field's type, the global declared first");
            check_eq((size_t)0, error_count(e->check(
                "int count = 5;\nclass C { string count = \"s\"; function probe() -> void { int x = ::count; string s = count; } }\nC().probe();")),
                "::count is the global, count the field");
            check_eq((size_t)1, error_count(e->check(
                "int f() { return 1; }\nclass C { string f() { return \"s\"; } function probe() -> void { int x = f(); } }\nC().probe();")),
                "a bare call reaches the method");
            check_eq((size_t)1, error_count(e->check(
                "class C { string only_field = \"s\"; function probe() -> void { int x = only_field; } }")));
            auto warned = e->check("int count = 5;\nclass C { string count = \"s\"; }");
            bool collision = false;
            for (const auto& d : warned.diagnostics) { if (d.message.find("shadows the global 'count'") != std::string::npos) { collision = true; } }
            check_true(collision, "the collision warning stays");
        });

        test("shared_ptr_decl_pointee_checking", [this]() {
            // Runtime enforces typed shared_ptr decl pointees;
            // the checker models the same rule through its object-assignability
            // lattice: wrong class errors, derived->base stays clean.
            auto e = make_engine();
            check_eq((size_t)1, error_count(e->check(
                "class Foo { int x = 1; } class Bar { int y = 2; } shared_ptr<Bar> wrong = Foo();")));
            check_eq((size_t)0, error_count(e->check(
                "class Base { int b = 1; } class D : Base { int d = 2; } shared_ptr<Base> ok = D();")));
            check_eq((size_t)0, error_count(e->check(
                "class Foo2 { int x = 1; } shared_ptr<Foo2> n = null;")));
        });

        test("shared_ptr_auto_decl_infer_then_enforce", [this]() {
            // shared_ptr<auto> models runtime's infer-then-enforce:
            // the pointee comes from the initializer's class and behaves like the
            // explicit spelling thereafter. Nothing to infer (missing/null/non-class
            // initializer) errors, matching runtime.
            auto e = make_engine();
            check_eq((size_t)1, error_count(e->check(
                "class Foo { int x = 1; } shared_ptr<auto> p;")), "no init");
            check_eq((size_t)1, error_count(e->check(
                "class Foo { int x = 1; } shared_ptr<auto> p = null;")), "null init");
            check_eq((size_t)1, error_count(e->check(
                "class Foo { int x = 1; } shared_ptr<auto> p = 5;")), "int init");
            // infer-then-enforce: wrong-class reassign errors, upcast + null stay clean
            check_eq((size_t)1, error_count(e->check(
                "class Foo { int x = 1; } class Bar { int y = 2; } shared_ptr<auto> p = Foo(); p = Bar();")), "wrong-class reassign");
            check_eq((size_t)0, error_count(e->check(
                "class Base { int b = 1; } class D : Base { int d = 2; } shared_ptr<auto> p = Base(); p = new D();")), "upcast reassign");
            check_eq((size_t)0, error_count(e->check(
                "class Foo { int x = 1; } shared_ptr<auto> p = Foo(); p = null; int x = p.x;")), "null reassign + member");
            // inferred tag models member usage exactly like the explicit spelling
            // (parity-equality: whatever the explicit twin reports, inferred matches)
            for (const char* tail : { "shared_ptr<%s> p = new Foo(); string s = p.x;",
                                      "shared_ptr<%s> p = new Foo(); p.nope();" }) {
                char explicit_src[256], inferred_src[256];
                std::snprintf(explicit_src, sizeof(explicit_src),
                    (std::string("class Foo { int x = 1; } ") + tail).c_str(), "Foo");
                std::snprintf(inferred_src, sizeof(inferred_src),
                    (std::string("class Foo { int x = 1; } ") + tail).c_str(), "auto");
                check_eq(error_count(e->check(explicit_src)), error_count(e->check(inferred_src)),
                         std::string("explicit/inferred parity: ") + tail);
            }
        });

        // ------------------------------------------------------------ type proofs
        // docs/site/embedding/06-static-checking.html "Proofs": the proof pass rides the checker walk in every mode
        // and stamps AST nodes; consumers guard at runtime, so proofs on/off must be
        // observationally identical. These pin the stamping side (counters) and the
        // engine plumbing (switch, re-stamp on surface moves, jaibite paths).

        test("proofs_default_on_and_switch_roundtrip", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            auto e = make_engine();
            check_true(e->type_proofs());
            e->type_proofs(false);
            check_false(e->type_proofs());
            e->type_proofs(true);
            check_true(e->type_proofs());
        });

        test("proofs_stamp_with_counters", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            // a clean program: no diagnostics, and the proof pass stamps
            auto e = make_engine();
            const std::string src =
                "int a = 1; a = 2; string s = \"x\"; s = s + \"y\";\n"
                "int twice(int v) { return v * 2; }\n"
                "int r = twice(a);\n"
                "float f = 1.5; f = 2.5; bool b = true; b = false; char c = 'q'; c = 'r';\n"
                "r";
            check_eq((int64_t)4, e->execute(src).as_int());
            const proof_stats& st = e->last_proof_stats();
            check_ge(st.stores, (uint32_t)5, "same-type stores");
            check_eq((uint32_t)1, st.args, "int argument into int parameter");
            check_eq((uint32_t)1, st.returns, "int expression returned from int function");
            check_eq((size_t)0, e->last_check_diagnostics().diagnostics.size());

            // the twin with proofs off runs the same source with zero stamps and equal output
            auto off = make_engine();
            off->type_proofs(false);
            check_eq((int64_t)4, off->execute(src).as_int());
            const proof_stats& none = off->last_proof_stats();
            check_eq((uint32_t)0, none.stores + none.args + none.returns);
        });

        test("proofs_not_stamped_from_uncertain_types", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            auto e = make_engine();
            // var/auto-no-init/shared_ptr/weak_ptr/array/map/reference decls, engine globals,
            // defaulted and by-ref params, includes-free but dynamic: nothing is certain here
            e->add_global("g", script_value((int64_t)3, e.get()));
            const std::string src =
                "var v = 1; v = 2; auto n; n = 3; array<int> xs = [1]; map<string,int> m = {\"a\": 1};\n"
                "int gi = g; gi = g;\n"
                "int dflt(int a, int b = 1) { return a + b; } dflt(1);\n"
                "void byref(int& r) { r = 5; } int q = 0; byref(q);\n"
                "class Foo { int x = 1; } shared_ptr<Foo> sp = new Foo(); weak_ptr<Foo> wp = sp; Foo cp = Foo(); cp = Foo();\n"
                "q";
            e->execute(src);
            const proof_stats& st = e->last_proof_stats();
            check_eq((uint32_t)1, st.stores, "only `cp = Foo()` (object same_type) proves");
            check_eq((uint32_t)0, st.args, "defaulted/by-ref callees are never proven");
        });

        test("proofs_off_strips_cached_entry_and_on_restamps", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            auto e = make_engine();
            const std::string src = "int a = 1; a = 2; a";
            check_eq((int64_t)2, e->execute(src).as_int());
            check_ge(e->last_proof_stats().stores, (uint32_t)1);
            // switching off re-runs the cached entry in clear mode at its next execute
            e->type_proofs(false);
            check_eq((int64_t)2, e->execute(src).as_int());
            check_eq((uint32_t)0, e->last_proof_stats().stores);
            // and back on re-stamps
            e->type_proofs(true);
            check_eq((int64_t)2, e->execute(src).as_int());
            check_ge(e->last_proof_stats().stores, (uint32_t)1);
        });

        test("proofs_restamp_after_registration_and_hold_across_identical_reload", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            auto e = make_engine();
            const std::string src =
                "class Pt { int x = 1; float y = 2.0;\n"
                "  int bump(int d) { x = x + d; this.y = this.y * 2.0; return x; } }\n"
                "Pt p = Pt(); p.bump(3); p.x";
            // first execute: same_type arg/return proofs stamp
            check_eq((int64_t)4, e->execute(src).as_int());
            check_ge(e->last_proof_stats().args, (uint32_t)1);
            check_ge(e->last_proof_stats().returns, (uint32_t)1);
            const uint64_t epoch_after_first = shape_epoch(*e, "Pt");
            // registering the class moved the surface: the SAME cached entry re-checks.
            // Its class_decl then runs as the FIRST redefinition, which is a full-path
            // reload (the structural key stores lazily): shape epoch and surface move once more
            check_eq((int64_t)4, e->execute(src).as_int());
            check_ge(e->last_proof_stats().returns, (uint32_t)1, "re-checked");
            check_ne(epoch_after_first, shape_epoch(*e, "Pt"));
            const uint64_t epoch_after_second = shape_epoch(*e, "Pt");
            // third execute: re-checks (surface moved), its reload is identical -> epoch holds
            check_eq((int64_t)4, e->execute(src).as_int());
            check_ge(e->last_proof_stats().returns, (uint32_t)1, "re-checked");
            check_eq(epoch_after_second, shape_epoch(*e, "Pt"));
            const uint64_t epoch_after_third = shape_epoch(*e, "Pt");
            // from here on: identical reloads move nothing (no re-check, shape epoch holds)
            e->execute("1 + 1");
            check_eq((int64_t)4, e->execute(src).as_int());
            check_eq((uint32_t)0, e->last_proof_stats().returns, "identical reload must not re-check");
            check_eq(epoch_after_third, shape_epoch(*e, "Pt"));
            check_eq((int64_t)4, e->execute(src).as_int());
            check_eq((uint32_t)0, e->last_proof_stats().returns);
            check_eq(epoch_after_third, shape_epoch(*e, "Pt"));
            // non-identical reload (field retype) moves the shape epoch and the surface:
            // the original entry re-checks at its next execute
            e->execute("class Pt { string x = \"1\"; float y = 2.0;\n"
                       "  int bump(int d) { return d; } }");
            check_ne(epoch_after_third, shape_epoch(*e, "Pt"));
            e->execute("1 + 1");
            e->execute(src);
            check_ge(e->last_proof_stats().returns, (uint32_t)1, "re-checked after the shape change");
        });

        test("accessor_members_known_with_unknown_type", [this]() {
            // _get_x/_set_x declare a member x the checker knows (no `no member` error) whose
            // static type is the single declared accessor's ("Members"): a typed getter types
            // the read, a typed setter checks the store, bare x inside the class keeps the
            // field's declared type, and nothing is ever proven from the member.
            auto e = make_engine();
            check_eq((int64_t)5, e->execute(
                "class V { int seen = 0; int _get_virt() { return seen; } void _set_virt(int v) { seen = v; } }\n"
                "V v = V(); v.virt = 5; v.virt").as_int(), "virtual property on a typed receiver under strict");
            check_eq(std::string("s"), e->execute(
                "class P { int b = 1; } class C : P { override string _get_b() { return \"s\"; } }\n"
                "C c = C(); string s = c.b; s").as<std::string>(), "the getter's type, not the field's declared int");
            std::string text;
            try { e->execute("C c2 = C(); int n = c2.b; n"); text = "ran"; }
            catch (const static_check_error&) { text = "static"; }
            catch (const std::exception& ex) { text = ex.what(); }
            check_true(text.find("Cannot initialize 'n' (declared int) with string") != std::string::npos, "registered-only class: the getter's declaration is out of view, a run-time mismatch: " + text);
            std::string same_unit;
            try { e->execute("class P2 { int b = 1; } class C2 : P2 { override string _get_b() { return \"s\"; } }\nC2 c3 = C2(); int n = c3.b; n"); same_unit = "ran"; }
            catch (const static_check_error& err) { same_unit = err.what(); }
            catch (const std::exception& ex) { same_unit = std::string("runtime: ") + ex.what(); }
            check_true(same_unit.find("cannot initialize 'n' declared 'int' with 'string'") != std::string::npos,
                       "same unit: the one declared getter in the chain types the member, a load error: " + same_unit);
            bool threw = false;
            std::string message;
            try { e->execute("class Q { int b = 1; int _get_b() { return 7; } } Q q = Q(); q.zzz;"); }
            catch (const static_check_error& err) { threw = true; message = err.what(); }
            check_true(threw, "a genuinely unknown member still errors");
            check_true(message.find("no member 'zzz' in class 'Q'") != std::string::npos, message);
        });

        test("proof_jaibite_paths_restamp", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            auto e = make_engine();
            auto bite = e->jaibite("int a = 5; int b = 0; b = a; b");
            check_eq((uint32_t)1, e->last_proof_stats().stores, "proof pass at creation");
            check_true(bite.checked_clean());
            auto loaded = e->jaibite_load_bytes(bite.save_bytes());
            check_eq((uint32_t)1, e->last_proof_stats().stores, "re-derived at load, never serialized");
            check_true(loaded.checked_clean());
            check_eq((int64_t)5, loaded.execute().as_int());
            e->type_proofs(false);
            check_eq((int64_t)5, loaded.execute().as_int());
            check_eq((uint32_t)0, e->last_proof_stats().stores, "next execute clears the bite's stamps");
            e->type_proofs(true);
            // a trusted-clean load keeps its empty report and still stamps
            auto clean = e->jaibite("int q = 1; int w = 0; w = q; w");
            check_true(clean.checked_clean());
            auto trusted = e->jaibite_load_bytes(clean.save_bytes());
            check_true(trusted.checked_clean());
            check_eq((size_t)0, trusted.check_diagnostics().diagnostics.size());
            check_eq((uint32_t)1, e->last_proof_stats().stores);
            check_eq((int64_t)1, trusted.execute().as_int());
        });

        test("proof_kernel_same_type_payload", [this]() {
            auto e = make_engine();
            script_value i((int64_t)5, e.get());
            script_value f(2.5, e.get());
            script_value s(std::string("x"), e.get());
            check_true(detail::same_type_payload(i, detail::engine_access::get_type_info_int(*e)));
            check_false(detail::same_type_payload(i, detail::engine_access::get_type_info_float(*e)));
            check_true(detail::same_type_payload(f, detail::engine_access::get_type_info_float(*e)));
            check_true(detail::same_type_payload(s, detail::engine_access::get_type_info_string(*e)));
            check_false(detail::same_type_payload(s, detail::engine_access::get_type_info_int(*e)));
            check_false(detail::same_type_payload(i, nullptr));
            // a non-any tag naming another base defeats the identity claim
            script_value tagged((int64_t)5, e.get());
            tagged.set_type_info(type_info_ptr{detail::engine_access::get_type_info_float(*e)});
            check_false(detail::same_type_payload(tagged, detail::engine_access::get_type_info_int(*e)));
        });

        test("proof_battery_results_equal_with_proofs_on_and_off", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            // Same backend, two engines: proofs on vs off must agree on every value and
            // every error text (consumers are guard-gated; this pins the contract).
            auto on = make_engine();
            auto off = make_engine();
            off->type_proofs(false);
            const char* snippets[] = {
                "int x = 1; x = 2; x = 2.5; type_of(x) + \":\" + to_string(x)",
                "string s = \"a\"; s = \"b\"; s = 3; s",
                "float f = 1.0; f = 2.0; f = 3; f",
                "bool b = true; b = false; b = 1; b",
                "char c = 'a'; c = 'b'; c",
                "int x = 1; x = \"s\"; x",
                "class C { int v = 1; string n = \"a\"; int get() { v = v + 1; this.n = this.n + \"!\"; return v; } }\n"
                "C c = C(); c.get(); c.get(); c.v = 10; c.n + to_string(c.v)",
                "class C { int v = 1; } C a = C(); C b = a; b.v = 7; to_string(a.v) + to_string(b.v)",
                "class C { int v = 1; } C a = C(); a.v = \"s\"; a.v",
                "int twice(int v) { return v * 2; } int r = twice(4); twice(r)",
                "int twice(int v) { return v * 2; } twice(\"s\")",
                "string name(int v) { return v; } name(3)",
                "class C { int v = 1; } C make() { return C(); } C m = make(); m.v",
                "var d = 1; d = \"x\"; auto a = 2; a = 3; a",
                // a map's missing key reads null: its element proves nothing, so the grafted parameter and the parallel return convert
                "map<string, int> M = {\"a\": 1}; int dbl(int a) { return a * 2; } int run() { int s = 0; array<string> ks = [\"a\", \"b\"]; for (string k : ks) { s += dbl(M[k]); } return s; } run()",
                "map<string, int> M = {\"a\": 1}; int pick(int x) { return M[\"k\"]; } parallel_transform([0], pick)",
            };
            for (const char* src : snippets) {
                // twice each: the second run is the cache hit (re-checked after class registration)
                for (int pass = 0; pass < 2; ++pass) {
                    check_eq(outcome(*off, src), outcome(*on, src), std::string("proofs on/off diverge on: ") + src);
                }
            }
            check_true(on->type_proofs());
            check_false(off->type_proofs());
        });

        test("proof_consumers_retype_on_reload_assigns_never_errors", [this]() {
            // Field proofs armed before a reload that retypes the fields miss on
            // the shape epoch, and the ladder converts/rejects exactly as the unproven twin.
            auto on = make_engine();
            auto off = make_engine();
            off->type_proofs(false);
            for (auto* e : {on.get(), off.get()}) {
                e->add_variadic_function("to_string", [e](const std::vector<script_value>& args) -> script_value {
                    return script_value(args.empty() ? std::string() : args[0].to_string(), e);
                });
            }
            const char* v1 =
                "class R { int f = 1; string g = \"a\";\n"
                "  int set(int v) { f = v; this.f = v; g = \"s\"; return f; }\n"
                "  string show() { return to_string(f) + \":\" + g + \":\" + this.g; } }\n"
                "R r = R(); r.set(41); r.f = 42; r.show()";
            const char* v2 =
                "class R { string f = \"z\"; int g = 0;\n"
                "  int set(int v) { f = to_string(v); this.f = to_string(v); g = 5; return v; }\n"
                "  string show() { return f + \":\" + to_string(g) + \":\" + to_string(this.g); } }\n"
                "r.show()";
            const char* use = "r.set(7); r.f = 9; r.g = 3; r.show()";
            const char* bad = "r.g = \"x\"; r.show()";
            for (int pass = 0; pass < 3; ++pass) {   // first-execute -> stamped -> identical-reload hit
                check_eq(outcome(*off, v1), outcome(*on, v1));
                check_eq(outcome(*off, use), outcome(*on, use));
            }
            check_eq(outcome(*off, v2), outcome(*on, v2));
            for (int pass = 0; pass < 3; ++pass) {
                check_eq(outcome(*off, use), outcome(*on, use));
                check_eq(outcome(*off, bad), outcome(*on, bad));
            }
            const char* retyped_use = "r.set(7); r.f = \"9\"; r.g = 3; r.show()";
            check_eq(outcome(*off, retyped_use), outcome(*on, retyped_use));
            check_eq(std::string("VALUE 9:3:3"), outcome(*on, retyped_use));
        });

        // ------------------------------------------------------------ fuzz corpus FP gate

        test("fuzz_corpus_false_positive_gate", [this]() {
            // 200 generator seeds: any program that runs clean must not produce strict
            // errors unless it genuinely violates declared types. The generator emits
            // valid typing, so the expected FP count is zero. Bounded by budget checks, not the clock (the wall-clock
            // budget only backstops), so the programs that run clean do not depend on the machine's load.
            constexpr uint64_t k_seed_count = 200;
            constexpr uint64_t k_step_budget = 32;
            constexpr double k_backstop_seconds = 30.0;
            int false_positives = 0;
            int clean_programs = 0;
            std::string first_report;
            for (uint64_t seed = 0; seed < k_seed_count; ++seed) {
                fuzz::program p = fuzz::generate_program(seed, 3);
                auto eng = make_engine();
                eng->execution_budget(k_backstop_seconds);
                eng->execution_step_budget(k_step_budget);
                eng->memory_cap(64 * 1024 * 1024);
                jai::engine* raw = eng.get();
                // the corpus presumes the host surface every real host provides
                eng->add_variadic_function("print", [raw](const std::vector<script_value>& args) -> script_value {
                    (void)args;
                    return script_value(std::monostate{}, raw);
                });
                eng->add_variadic_function("to_string", [raw](const std::vector<script_value>& args) -> script_value {
                    return script_value(args.empty() ? std::string() : args[0].to_string(), raw);
                });
                bool ran_clean = true;
                std::vector<std::string> flagged;
                for (const auto& seg : p.segments) {
                    check_report report;
                    try {
                        report = eng->check(seg);
                    } catch (...) {
                        ran_clean = false;   // parse-level issue; not a checker finding
                        break;
                    }
                    if (report.has_errors()) {
                        for (const auto& d : report.diagnostics) {
                            // a lost write judges intent, not types: the generator writes copies freely (the harness runs a warn region)
                            if (d.severity == check_diagnostic::level::error && d.message.find("so the write is lost") == std::string::npos) {
                                flagged.push_back(std::to_string(d.line) + ":" + std::to_string(d.column) +
                                                  " " + d.message);
                            }
                        }
                    }
                    try {
                        eng->execute("[[warn_mode]]; " + seg);   // a flagged segment still runs, so it can prove it runs clean
                    } catch (...) {
                        ran_clean = false;   // runtime error: errors on it are legitimate
                        break;
                    }
                }
                if (!ran_clean) { continue; }
                ++clean_programs;
                if (!flagged.empty()) {
                    ++false_positives;
                    if (first_report.empty()) {
                        first_report = "seed " + std::to_string(seed) + " ran clean but was flagged:\n";
                        for (const auto& f : flagged) { first_report += "  " + f + "\n"; }
                        first_report += p.joined();
                    }
                }
            }
            check_gt(clean_programs, 0);
            check_eq(0, false_positives, first_report.empty() ? "no false positives" : first_report);
        });

        // ------------------------------------------------------------ proof differential gate

        test("parameter_alias_differential_gate", [this]() {
            // Read-only by-value parameter and local elision: per generated program (the read-only local band always on),
            // each backend must produce identical segment results, printed output and liveness with
            // engine::parameter_alias and engine::local_alias on and off (the alias is a proof-gated copy elision, never
            // a semantic). Same protocol as proof_differential_gate, bounded by budget checks instead of the clock, so the
            // programs compared and the binds counted do not depend on the machine's load (the wall-clock budget only backstops).
            constexpr uint64_t k_seed_count = 200;
            constexpr uint64_t k_step_budget = 32;
            constexpr double k_backstop_seconds = 30.0;
            int compared_programs = 0;
            int divergences = 0;
            uint64_t elided_binds = 0;
            uint64_t elided_locals = 0;
            std::string first_report;
            auto same = [](const fuzz::run_outcome& a, const fuzz::run_outcome& b) {
                return a.segment_results == b.segment_results && a.printed == b.printed && a.alive == b.alive;
            };
            for (uint64_t seed = 0; seed < k_seed_count; ++seed) {
                fuzz::program p = fuzz::generate_program(seed, 3, fuzz::band_mode::never, fuzz::band_mode::never, fuzz::band_mode::never,
                                                         fuzz::band_mode::never, fuzz::band_mode::always);
                fuzz::run_outcome runs[2][2];   // [backend][alias: 0 off, 1 on]
                bool budget = false;
                for (int be = 0; be < 2 && !budget; ++be) {
                    for (int al = 0; al < 2 && !budget; ++al) {
                        fuzz::run_config cfg;
                        cfg.use_vm = be == 1;
                        cfg.budget_seconds = k_backstop_seconds;
                        cfg.step_budget = k_step_budget;
                        cfg.param_alias = al == 1;
                        runs[be][al] = fuzz::run_program(p, cfg);
                        budget = runs[be][al].budget_hit;
                    }
                }
                if (budget) { continue; }
                ++compared_programs;
                elided_binds += runs[0][1].alias_binds + runs[1][1].alias_binds;
                elided_locals += runs[0][1].local_alias_binds + runs[1][1].local_alias_binds;
                for (int be = 0; be < 2; ++be) {
                    if (same(runs[be][0], runs[be][1])) { continue; }
                    ++divergences;
                    if (first_report.empty()) {
                        first_report = "seed " + std::to_string(seed) + " on the " + (be ? "vm" : "interpreter") +
                                       " differs between parameter_alias off and on:\n" + p.joined();
                    }
                }
            }
            check_gt(compared_programs, 100);
            check_gt((int64_t)elided_binds, (int64_t)0, "the generated programs exercise the elision");
            check_gt((int64_t)elided_locals, (int64_t)0, "the generated programs exercise the local elision");
            check_eq(0, divergences, first_report.empty() ? "no alias divergences" : first_report);
        });

        test("proof_differential_gate", [this]() {
            // The byte-parallel contract (docs/site/embedding/06-static-checking.html "Proofs"): per generated
            // program, each backend must produce identical segment results, printed output,
            // error text and liveness with type_proofs on and off - four fresh engines with
            // identical registrations per seed. Cross-backend agreement is the fuzz smoke
            // suite's contract and is re-checked here modulo its known suppressions. Bounded by budget checks, not the
            // clock (the wall-clock budget only backstops), so the programs compared do not depend on the machine's load.
            constexpr uint64_t k_seed_count = 200;
            constexpr uint64_t k_step_budget = 32;
            constexpr double k_backstop_seconds = 30.0;
            int compared_programs = 0;
            int proof_divergences = 0;
            int backend_divergences = 0;
            std::string first_report;
            auto same = [](const fuzz::run_outcome& a, const fuzz::run_outcome& b) {
                return a.segment_results == b.segment_results && a.printed == b.printed && a.alive == b.alive;
            };
            auto diff_text = [](const fuzz::run_outcome& a, const char* la, const fuzz::run_outcome& b, const char* lb) {
                std::string d;
                const size_t n = std::min(a.segment_results.size(), b.segment_results.size());
                for (size_t i = 0; i < n; ++i) {
                    if (a.segment_results[i] != b.segment_results[i]) {
                        d += "  segment " + std::to_string(i) + ":\n    " + la + ": " + a.segment_results[i] +
                             "\n    " + lb + ": " + b.segment_results[i] + "\n";
                    }
                }
                if (a.segment_results.size() != b.segment_results.size()) { d += "  segment count differs\n"; }
                if (a.printed != b.printed) { d += "  printed output differs " + fuzz::first_difference(a.printed, b.printed) + "\n"; }
                if (a.alive != b.alive) {
                    d += std::string("  engine-alive-after: ") + la + "=" + (a.alive ? "yes" : "NO") +
                         " " + lb + "=" + (b.alive ? "yes" : "NO") + "\n";
                }
                return d;
            };
            for (uint64_t seed = 0; seed < k_seed_count; ++seed) {
                fuzz::program p = fuzz::generate_program(seed, 3);
                fuzz::run_outcome runs[2][2];   // [backend: 0 interp, 1 vm][proofs: 0 off, 1 on]
                bool budget = false;
                for (int be = 0; be < 2 && !budget; ++be) {
                    for (int pr = 0; pr < 2 && !budget; ++pr) {
                        fuzz::run_config cfg;
                        cfg.use_vm = be == 1;
                        cfg.budget_seconds = k_backstop_seconds;
                        cfg.step_budget = k_step_budget;
                        cfg.proofs = pr == 1;
                        runs[be][pr] = fuzz::run_program(p, cfg);
                        budget = runs[be][pr].budget_hit;
                    }
                }
                if (budget) { continue; }   // a spent budget: never a finding
                ++compared_programs;
                for (int be = 0; be < 2; ++be) {
                    if (same(runs[be][0], runs[be][1])) { continue; }
                    ++proof_divergences;
                    if (first_report.empty()) {
                        first_report = "seed " + std::to_string(seed) + " on the " + (be ? "vm" : "interpreter") +
                                       " differs between type_proofs off and on:\n" +
                                       diff_text(runs[be][0], "proofs off", runs[be][1], "proofs on ") + p.joined();
                    }
                }
                if (!same(runs[0][0], runs[1][0])) {
                    const std::string d = diff_text(runs[0][0], "interp", runs[1][0], "vm    ");
                    if (fuzz::match_suppression(p, d).empty()) {
                        ++backend_divergences;
                        if (first_report.empty()) {
                            first_report = "seed " + std::to_string(seed) + " diverges between the backends (proofs off):\n" + d + p.joined();
                        }
                    }
                }
            }
            check_gt(compared_programs, 0);
            check_eq(0, proof_divergences, first_report.empty() ? "proofs on/off agree on both backends" : first_report);
            check_eq(0, backend_divergences, first_report.empty() ? "backends agree" : first_report);
        });

        // ------------------------------------------------------------ cast<T>(e)

        test("cast_types_certain_and_reference_receivers_resolve_members", [this]() {
            auto e = make_engine();
            const std::string prelude =
                "class S { int x = 1; } class T : S { int t = 2; } class H { S p; }\n"
                "auto h = H(); h.p = T();\n";
            // a subtype member on a certain base-typed receiver is the error; cast<T&> is the fix
            bool threw = false;
            try { e->execute(prelude + "h.p.t;"); } catch (const static_check_error&) { threw = true; }
            check_true(threw, "subtype member on a certain base receiver is a strict error");
            check_eq((int64_t)2, e->execute(prelude + "cast<T&>(h.p).t").as_int());
            check_eq((int64_t)2, e->execute(prelude + "T tv = cast<T>(h.p); tv.t").as_int());
            check_eq((int64_t)2, e->execute(prelude + "shared_ptr<T> th = cast<shared_ptr<T>>(h.p); th.t").as_int());
            // the cast's class is certain: a missing member on it is still an error
            threw = false;
            std::string message;
            try { e->execute(prelude + "cast<T&>(h.p).zzz;"); } catch (const static_check_error& err) { threw = true; message = err.what(); }
            check_true(threw, "unknown member on a cast reference");
            check_true(message.find("no member 'zzz' in class 'T'") != std::string::npos);
            threw = false;
            try { e->execute(prelude + "T tv = cast<T>(h.p); tv.zzz;"); } catch (const static_check_error&) { threw = true; }
            check_true(threw, "unknown member on a cast value");
        });

        test("cast_unknown_type_is_a_check_error_and_a_runtime_error_in_a_warn_region", [this]() {
            auto e = make_engine();
            bool threw = false;
            std::string message;
            try { e->execute("var v = 1; cast<Nope>(v);"); } catch (const static_check_error& err) { threw = true; message = err.what(); }
            check_true(threw);
            check_true(message.find("unknown type in cast: 'Nope'") != std::string::npos);
            // a warn region keeps the runtime verdict: the operand's class is simply not the target
            auto w = make_engine();
            check_eq(std::string("Cannot cast 'int' to 'Nope'"),
                     w->execute("[[warn_mode]]; var v = 1; var c = \"\"; try { cast<Nope>(v); } catch (e) { c = e; } c").to_string());
            check_true(has_diag(w->last_check_diagnostics(), dlevel::warning, "unknown type in cast: 'Nope'"));
        });

        test("a_declared_type_must_name_a_type", [this]() {
            // every spelling a declaration's type takes: a variable, a field, a parameter, a return, an element, a pointee, a
            // range-for variable, a lambda's parameter
            const auto message = [this](const std::string& source) {
                auto e = make_engine();
                jai::stdlib::register_all(*e);
                try { e->execute(source); } catch (const static_check_error& err) { return std::string(err.what()); }
                return std::string("loaded");
            };
            for (const char* source : {"Nope n;", "class C { Nope z; }", "void f(Nope n) { }", "Nope g() { return null; }",
                                       "array<Nope> xs;", "map<string, Nope> m;", "shared_ptr<Nope> h;", "weak_ptr<Nope> w;",
                                       "for (Nope x : [1]) { }", "auto l = [](Nope n) { return 1; };", "int f(const Nope& n) { return 1; }"}) {
                check_true(message(source).find("error: unknown type 'Nope'") != std::string::npos, std::string(source) + " -> " + message(source));
            }
            check_true(message("vec2 p;").find("error: unknown type 'vec2'; did you mean 'math::vec2' (using math::vec2;)?") != std::string::npos,
                       message("vec2 p;"));
            check_eq(std::string("loaded"), message("class A { B b; } class B { int x = 1; } namespace ns { class K { } } ns::K k; using ns::K; K k2; "
                                                    "time::date d; random r(1); regex re(\"a\"); math::vec3 v; shared_ptr<A> h; array<B> bs;"));
            auto w = make_engine();
            w->execute("[[warn_mode]]; void f(Nope n) { } 1;");
            check_true(has_diag(w->last_check_diagnostics(), dlevel::warning, "unknown type 'Nope'"));
        });

        test("a_qualified_name_must_name_a_namespace_member", [this]() {
            // a host namespace (math::, string::, reflect::), a script one, nested paths; a member another declaration of the
            // namespace adds (a script's math::, a later reopening) is declared wherever it stands
            const auto message = [this](const std::string& source) {
                auto e = make_engine();
                jai::stdlib::register_all(*e);
                try { e->execute(source); } catch (const static_check_error& err) { return std::string(err.what()); }
                return std::string("loaded");
            };
            const auto rejects = [&](const std::string& source, const std::string& text) {
                check_true(message(source).find("error: " + text) != std::string::npos, source + " -> " + message(source));
            };
            rejects("float x = 4.0; print(math::sqroot(x));", "no member 'sqroot' in namespace 'math'");
            rejects("print(math::pi);", "no member 'pi' in namespace 'math'");
            rejects("print(string::to_uper(\"a\"));", "no member 'to_uper' in namespace 'string'");
            rejects("print(reflect::clases());", "no member 'clases' in namespace 'reflect'");
            rejects("namespace a::b { int x = 1; } print(a::b::y);", "no member 'y' in namespace 'a::b'");
            rejects("namespace a { int x = 1; } print(a::c::y);", "no member 'c' in namespace 'a'");
            rejects("namespace a { int x = 1; } a::z = 2;", "no member 'z' in namespace 'a'");
            check_eq(std::string("loaded"), message(
                "float twice(float v) { return math::double_it(v); }\n"
                "namespace math { float double_it(float v) { return v * 2.0; } }\n"
                "namespace geo { namespace inner { int k = 3; } class Box { static int n = 1; } }\n"
                "print(math::sqrt(4.0), math::PI, string::to_lower(\"A\"), geo::inner::k, geo::Box::n, twice(1.0));"));
            auto earlier = make_engine();
            earlier->execute("namespace tools { int k = 7; }");
            check_eq((int64_t)7, earlier->execute("tools::k;").as_int(), "a member an earlier run declared");
            bool refused = false;
            try { earlier->execute("tools::kk;"); } catch (const static_check_error& err) { refused = std::string(err.what()).find("no member 'kk' in namespace 'tools'") != std::string::npos; }
            check_true(refused, "a member no run declared");
            auto w = make_engine();
            w->execute("[[warn_mode]]; var f = 1; try { f = math::sqroot(4.0); } catch (e) { } 1;");
            check_true(has_diag(w->last_check_diagnostics(), dlevel::warning, "no member 'sqroot' in namespace 'math'"));
        });

        test("a_namespace_is_not_a_value", [this]() {
            // C++ [basic.namespace]: a namespace's name alone is no expression; the run time finds no variable of the name
            const auto message = [this](const std::string& source) {
                auto e = make_engine();
                jai::stdlib::register_all(*e);
                try { e->execute(source); } catch (const static_check_error& err) { return std::string(err.what()); } catch (const std::exception& err) { return std::string("raised: ") + err.what(); }
                return std::string("loaded");
            };
            for (const char* ns : {"math", "time", "reflect", "crypto", "binary", "string", "geo"}) {
                const std::string text = std::string("'") + ns + "' is a namespace, not a value; name one of its members (" + ns + "::name)";
                const std::string field = "namespace geo { int k = 1; } class Gator { int spd = " + std::string(ns) + "; }";
                check_true(message(field).find("error: " + text) != std::string::npos, field + " -> " + message(field));
                const std::string warned = "namespace geo { int k = 1; } [[warn_mode]]; var v = " + std::string(ns) + ";";
                check_eq(std::string("raised: Undefined variable '") + ns + "'", message(warned), warned);
            }
            // a class's name is its constructor, a value an int slot refuses; a namespace's members and an enum's value table stay values
            check_true(message("int spd = random;").find("error: cannot initialize 'spd' declared 'int' with 'class random'") != std::string::npos, message("int spd = random;"));
            check_true(message("class C {} int c = C;").find("error: cannot initialize 'c' declared 'int' with 'class C'") != std::string::npos, message("class C {} int c = C;"));
            check_eq(std::string("loaded"), message("var r = random; var c = r(3); enum E { A } var e = E; namespace math { int twice(int x) { return 2 * x; } } math::twice(math::PI > 3.0 ? 1 : 0);"));
            // an enum's name is its value table, a map<string,int>: a slot that takes that map takes it, any other refuses it at load
            const std::pair<const char*, const char*> tables[] = {
                {"enum E { A, B } int x = E;", "cannot initialize 'x' declared 'int' with 'enum E': an enum's name is its value table, a map<string,int>; name one of its values (E::A)"},
                {"enum E { A, B } int x = 0; x = E;", "cannot assign 'enum E' to 'x' declared 'int': an enum's name is its value table"},
                {"enum E { A, B } void f(string s) {} f(E);", "argument 1 of 'f': cannot pass 'enum E' to 's' declared 'string': an enum's name is its value table"},
                {"enum E { A, B } int g() { return E; } g();", "cannot return 'enum E' from 'g' declared to return 'int': an enum's name is its value table"},
                {"enum E { A, B } bool b = E;", "cannot initialize 'b' declared 'bool' with 'enum E': an enum's name is its value table"},
                {"enum E { A, B } map<int,int> t = E;", "cannot initialize 't' declared 'map<int,int>' with 'enum E': an enum's name is its value table, a map<string,int> (map key type: expected 'int', got 'string')"},
            };
            for (const auto& [source, text] : tables) { check_true(message(source).find(std::string("error: ") + text) != std::string::npos, std::string(source) + " -> " + message(source)); }
            check_eq(std::string("raised: Cannot initialize 'x' (declared int) with map"), message("[[warn_mode]]; enum E { A, B } int x = E;"));
            check_eq(std::string("loaded"), message("enum E { A, B } map<string,int> t = E; map<string,float> f = E; auto a = E; bool b = !E.empty(); array<map<string,int>> all = [E]; "
                                                    "int n = 0; for (auto [k, v] : E) { n += v; } int one = E.B;"));
        });

        test("a_unary_operator_takes_only_the_operands_its_rows_or_a_class_operator_take", [this]() {
            const auto message = [this](const std::string& source) {
                auto e = make_engine();
                jai::stdlib::register_all(*e);
                try { return e->execute(source).to_string(); } catch (const static_check_error& err) { return std::string(err.what()); } catch (const std::exception& err) { return std::string("raised: ") + err.what(); }
            };
            const std::pair<const char*, const char*> refusals[] = {
                {"int spd = -true;", "Unary minus requires numeric operand"},
                {"string s = \"a\"; var z = -s;", "Unary minus requires numeric operand"},
                {"array<int> a = [1]; var z = -a;", "Unary minus requires numeric operand"},
                {"class A {} A a; var z = -a;", "Unary minus requires numeric operand"},
                {"var z = +\"s\";", "Unary plus requires numeric operand"},
                {"using math::vec2; vec2 v = vec2(1.0, 2.0); var z = ~v;", "Bitwise NOT requires integer operand"},
                {"float f = 1.5; var z = ~f;", "Bitwise NOT requires integer operand"},
                {"var z = ~false;", "Bitwise NOT requires integer operand"},
            };
            for (const auto& [source, text] : refusals) {
                check_true(message(source).find(std::string("error: ") + text) != std::string::npos, std::string(source) + " -> " + message(source));
                check_eq(std::string("raised: ") + text, message(std::string("[[warn_mode]]; ") + source), source);
            }
            // a char promotes to its int under each, a math value negates, a class operator answers, a var waits for the run
            check_eq(std::string("[-97, -98, 97, -1.0, 7]"), message(
                "char c = 'a'; using math::vec2; class A { int operator-() { return 7; } } A a; var t = true; try { var z = -t; } catch (e) { } "
                "[-c, ~c, +c, (-vec2(1.0, 2.0)).x, -a];"));
        });

        test("cast_value_is_a_proof_source_reference_is_not", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            const std::string prelude =
                "class S { int x = 1; } class T : S { int t = 2; } class H { S p; }\n"
                "auto h = H(); h.p = T();\n";
            auto e = make_engine();
            check_eq((int64_t)2, e->execute(prelude + "T tv = T(); tv = cast<T>(h.p); tv.t").as_int());
            check_ge(e->last_proof_stats().stores, (uint32_t)1, "tv = cast<T>(h.p) proves same_type");
            auto r = make_engine();
            check_eq((int64_t)2, r->execute(prelude + "T tv = T(); tv = cast<T&>(h.p); tv.t").as_int());
            check_eq((uint32_t)0, r->last_proof_stats().stores, "a reference never proves");
        });

        // cast<C>(h) on a handle or reference operand deep-copies (C++ value semantics - the
        // runtime is right); the checker names the aliasing spellings at warning severity.

        test("cast_value_form_on_handle_warns", [this]() {
            const std::string P = "class C { int v = 1; } class H { shared_ptr<C> p = null; }\n";
            const std::string text = "cast<C>(h) copies the instance - use cast<C&>(h) to alias it or cast<shared_ptr<C>>(h) to share it";
            auto e = make_engine();
            auto r = e->check(P + "shared_ptr<C> h = shared_ptr<C>();\nauto c = cast<C>(h);");
            check_eq((size_t)0, error_count(r));
            check_eq((size_t)1, warning_count(r));
            check_true(has_diag(r, dlevel::warning, text));
            check_eq((size_t)3, r.diagnostics[0].line, "anchored at the cast");
            check_eq((size_t)2, r.diagnostics[0].related_line, "related: the operand's declaration");
            r = e->check(P + "auto hh = H(); auto c = cast<C>(hh.p);");
            check_eq((size_t)1, warning_count(r));
            check_true(has_diag(r, dlevel::warning, "cast<C>(hh.p) copies the instance - use cast<C&>(hh.p) to alias it or cast<shared_ptr<C>>(hh.p) to share it"));
            check_eq((size_t)0, r.diagnostics[0].related_line, "member chain: no related declaration");
            r = e->check(P + "function mk() -> shared_ptr<C> { return shared_ptr<C>(); } auto c = cast<C>(mk());");
            check_eq((size_t)1, warning_count(r));
            check_true(has_diag(r, dlevel::warning, "cast<C>(...) copies the instance - use cast<C&>(...) to alias it or cast<shared_ptr<C>>(...) to share it"));
            r = e->check(P + "shared_ptr<auto> h = C(); auto c = cast<C>(h);");
            check_eq((size_t)1, warning_count(r));
            check_true(has_diag(r, dlevel::warning, text));
            r = e->check(P + "function f(shared_ptr<C> p) -> int { auto q = cast<C>(p); return q.v; }");
            check_eq((size_t)1, warning_count(r));
            check_true(has_diag(r, dlevel::warning, "cast<C>(p) copies the instance"));
            r = e->check(P + "class M { shared_ptr<C> f = null; int m() { auto q = cast<C>(f); return q.v; } int n() { auto q = cast<C>(this.f); return q.v; } }");
            check_eq((size_t)0, error_count(r));
            check_eq((size_t)2, warning_count(r));
            check_true(has_diag(r, dlevel::warning, "cast<C>(f) copies the instance"));
            check_true(has_diag(r, dlevel::warning, "cast<C>(this.f) copies the instance - use cast<C&>(this.f) to alias it"));
        });

        test("cast_value_form_on_reference_warns", [this]() {
            const std::string P = "class C { int v = 1; }\n";
            auto e = make_engine();
            auto r = e->check(P + "C c0 = C(); auto& r = c0; auto c = cast<C>(r);");
            check_eq((size_t)0, error_count(r));
            check_eq((size_t)1, warning_count(r));
            check_true(has_diag(r, dlevel::warning, "cast<C>(r) copies the instance - use cast<C&>(r) to alias it or cast<shared_ptr<C>>(r) to share it"));
            r = e->check(P + "C c0 = C(); var& vr = c0; auto c = cast<C>(vr);");
            check_eq((size_t)1, warning_count(r));
            check_true(has_diag(r, dlevel::warning, "cast<C>(vr) copies the instance"));
            r = e->check(P + "function f(C& p) -> int { auto q = cast<C>(p); return q.v; }");
            check_eq((size_t)1, warning_count(r));
            check_true(has_diag(r, dlevel::warning, "cast<C>(p) copies the instance"));
            r = e->check(P + "array<C> xs = [C()]; for (auto& x : xs) { auto q = cast<C>(x); }");
            check_eq((size_t)1, warning_count(r));
            check_true(has_diag(r, dlevel::warning, "cast<C>(x) copies the instance"));
        });

        test("cast_copy_warning_silent_forms", [this]() {
            const std::string P = "class C { int v = 1; } class H { shared_ptr<C> p = null; }\n";
            auto e = make_engine();
            const char* rows[] = {
                "shared_ptr<C> h = shared_ptr<C>(); auto& a = cast<C&>(h);",
                "shared_ptr<C> h = shared_ptr<C>(); shared_ptr<C> s = cast<shared_ptr<C>>(h);",
                "shared_ptr<C> h = shared_ptr<C>(); int i = cast<int>(h.v); string t = cast<string>(42);",
                "C c = C(); auto d = cast<C>(c);",
                "auto d = cast<C>(C());",
                "function f(C p) -> int { auto q = cast<C>(p); return q.v; }",
                "shared_ptr<C> h = shared_ptr<C>(); var g = h; auto d = cast<C>(g);",
                "shared_ptr<C> h = shared_ptr<C>(); weak_ptr<C> w = h; var m = \"\"; try { auto d = cast<C>(w); } catch (e) { m = e; }",
                "class M { C f = C(); int m() { auto q = cast<C>(f); return q.v + cast<C>(this.f).v; } }",
                "class D : C { int m() { auto q = cast<C>(this); return q.v; } }",
            };
            for (const char* row : rows) {
                auto r = e->check(P + row);
                check_eq((size_t)0, r.diagnostics.size(), row);
            }
        });

        test("cast_copy_warning_never_gates", [this]() {
            const std::string P = "class C { int v = 1; }\n";
            const std::string body = "shared_ptr<C> h = shared_ptr<C>(); auto c = cast<C>(h); c.v = 5; h.v * 10 + c.v";
            auto strict = make_engine();
            check_eq((int64_t)15, strict->execute(P + body).as_int());
            const auto& r = strict->last_check_diagnostics();
            check_eq((size_t)1, r.diagnostics.size());
            check_eq((size_t)0, error_count(r));
            check_true(r.diagnostics[0].severity == dlevel::warning);
            auto warn = make_engine();
            check_eq((int64_t)15, warn->execute("[[strict_mode]];\n" + P + body).as_int());
            check_false(warn->last_check_diagnostics().has_errors());
            check_eq((size_t)1, warning_count(warn->last_check_diagnostics()));
            // a weak_ptr operand keeps the runtime's verdict and draws nothing
            check_eq(std::string("Cannot cast 'weak_ptr' to 'C'"),
                     warn->execute(P + "shared_ptr<C> h = shared_ptr<C>(); weak_ptr<C> w = h; var m = \"\"; try { auto d = cast<C>(w); } catch (e) { m = e; } m").to_string());
            check_eq((size_t)0, warn->last_check_diagnostics().diagnostics.size());
        });

        test("cast_copy_warning_jaibite_unaffected", [this]() {
            const std::string src = "class C { int v = 1; }\nshared_ptr<C> h = shared_ptr<C>(); auto c = cast<C>(h); c.v = 5; h.v * 10 + c.v";
            for (const std::string& unit : {src, "[[warn_mode]]; " + src}) {
                auto e = make_engine();
                auto bite = e->jaibite(unit);
                check_true(bite.checked_clean());
                check_false(bite.check_diagnostics().has_errors());
                check_true(has_diag(bite.check_diagnostics(), dlevel::warning, "cast<C>(h) copies the instance"));
                auto loaded = e->jaibite_load_bytes(bite.save_bytes());
                check_true(loaded.checked_clean());
                check_eq((int64_t)15, loaded.execute().as_int());
            }
        });

        // ------------------------------------------------------------ programs (closure checking)

        test("program_closure_forward_reference_across_units", [this]() {
            auto dir = closure_dir("fwd");
            write_unit(dir, "fwd_a.jai", "function read_g() -> int { return G.v; }\n");
            write_unit(dir, "fwd_b.jai", "class Game { int v = 7; }\nvar G = Game();\n");
            auto e = program_engine(dir);
            check_eq((int64_t)7, e->execute("import \"fwd_a.jai\";\nimport \"fwd_b.jai\";\nread_g()").as_int());
            check_eq((size_t)0, error_count(e->check("import \"fwd_a.jai\";\nimport \"fwd_b.jai\";\nread_g()")));
            // the same leaf on its own, with nothing declaring G, is the genuine error
            auto alone = make_engine();
            check_eq((size_t)1, error_count(alone->check("function read_g() -> int { return G.v; }")));
            std::filesystem::remove_all(dir);
        });

        test("program_closure_leaf_error_fails_the_root_before_anything_runs", [this]() {
            auto dir = closure_dir("leaf_err");
            write_unit(dir, "leaf_err.jai", "function f() -> int { return zzz_missing + 1; }\n");
            auto e = program_engine(dir);
            std::string what;
            try {
                e->execute("import \"leaf_err.jai\";\n1");
            } catch (const static_check_error& ex) {
                what = ex.what();
            }
            check_true(what.find("static check failed") != std::string::npos, "root throws static_check_error");
            check_true(what.find("leaf_err.jai") != std::string::npos, "listing names the leaf's file");
            check_true(what.find("use of undeclared identifier 'zzz_missing'") != std::string::npos);
            check_true(what.find("return zzz_missing + 1") != std::string::npos, "snippet comes from the leaf's source");
            check_false(e->has_function("f"), "nothing ran");
            // the report engine::check returns folds the closure's diagnostics in, path-qualified
            auto r = e->check("import \"leaf_err.jai\";\n1");
            check_eq((size_t)1, error_count(r));
            check_eq((size_t)0, r.diagnostics[0].line);
            check_true(r.diagnostics[0].message.find("leaf_err.jai:1:") != std::string::npos);
            check_true(r.diagnostics[0].message.find("zzz_missing") != std::string::npos);
            std::filesystem::remove_all(dir);
        });

        test("global_qualified_store_checks_the_program_scope_declaration", [this]() {
            for (bool proofs : {true, false}) {
                auto e = make_engine();
                e->type_proofs(proofs);
                auto r = e->check("int PACK_MAX = 5;\n::PACK_MAX = \"lots\";");
                check_eq((size_t)1, error_count(r));
                check_true(has_diag(r, dlevel::error, "cannot assign 'string' to 'PACK_MAX' declared 'int'"));
                // ::name skips the locals and fields that shadow it, both ways
                check_eq((size_t)1, error_count(e->check("int PACK_MAX = 5;\nvoid f() { string PACK_MAX = \"a\"; ::PACK_MAX = \"lots\"; }")));
                check_eq((size_t)0, error_count(e->check("string G2 = \"\";\nvoid f() { int G2 = 0; ::G2 = \"x\"; G2 = 3; }")));
                check_eq((size_t)0, error_count(e->check("string G3 = \"\";\nclass K { int G3 = 0; void m() { ::G3 = \"x\"; G3 = 4; } }")));
                // a global another unit declared, typed off its engine cell (the reload-stub shape: a file included later)
                auto run = make_engine();
                run->type_proofs(proofs);
                run->execute("int LIMIT = 5;");
                check_eq((size_t)1, error_count(run->check("void reload() { ::LIMIT = \"lots\"; }")));
                check_eq((size_t)0, error_count(run->check("void reload() { ::LIMIT = 7; LIMIT = 8; }")));
            }
            // across the units of one program, before anything runs: the declaring unit's spelled type
            auto dir = closure_dir("gq_store");
            write_unit(dir, "gq_data.jai", "int PACK_MAX = 5;\n");
            write_unit(dir, "gq_logic.jai", "void bump() { ::PACK_MAX = \"lots\"; }\n");
            auto p = program_engine(dir);
            std::string what;
            try { p->execute("import \"gq_data.jai\";\nimport \"gq_logic.jai\";\n1"); } catch (const static_check_error& ex) { what = ex.what(); }
            check_true(what.find("gq_logic.jai:1:") != std::string::npos && what.find("cannot assign 'string' to 'PACK_MAX' declared 'int'") != std::string::npos, what);
            write_unit(dir, "gq_logic.jai", "void bump() { ::PACK_MAX = 9; }\n");
            check_eq((int64_t)1, p->execute("import \"gq_data.jai\";\nimport \"gq_logic.jai\";\n1").as_int());
            std::filesystem::remove_all(dir);
        });

        // a weak_ptr<T> element takes a shared handle of T or a subclass like a weak_ptr<T> declaration (C++: vector<weak_ptr>
        // push_back(shared_ptr)); an unrelated handle is named in full
        test("checker_weak_elements_take_shared_handles", [this]() {
            auto e = make_engine();
            const std::string classes = "class P { int id = 0; }\nclass D : P { }\nclass Q { }\nshared_ptr<P> a = new P();\nshared_ptr<D> d = new D();\nshared_ptr<P> n = null;\n";
            auto r = e->check(classes + "array<weak_ptr<P>> ws = [a];\nws.push(a);\nws.push(d);\nws.push(n);\nws.push(null);\nws[0] = a;\n"
                                        "map<string, weak_ptr<P>> m = {};\nm[\"k\"] = d;\nweak_ptr<P> w = a;\nws.push(w);");
            check_eq((size_t)0, error_count(r), r.format());
            r = e->check(classes + "shared_ptr<Q> q = new Q();\narray<weak_ptr<P>> ws = [];\nws.push(q);");
            check_eq((size_t)1, error_count(r), r.format());
            check_true(has_diag(r, dlevel::error, "expected 'weak_ptr<P>', got 'shared_ptr<Q>'"), r.format());
        });

        test("checker_sees_promoted_char_arithmetic_host_results_and_string_namespace_arguments", [this]() {
            for (bool proofs : {true, false}) {
                auto e = make_engine();
                jai::stdlib::register_all(*e);
                e->type_proofs(proofs);
                auto r = e->check("string z = \"\";\nint a = 1;\nz += to_char(a) + to_char(a);");
                check_eq((size_t)1, error_count(r));
                check_true(has_diag(r, dlevel::error, "cannot apply '+=' to 'string' and 'int'"), r.format());
                r = e->check("array<string> msg;\nint a = 1;\nmsg.push(to_char(a) + to_char(a) + to_char(a));");
                check_eq((size_t)1, error_count(r));
                check_true(has_diag(r, dlevel::error, "cannot push into 'array<string>'"), r.format());
                r = e->check("int a = 65;\nvar t = string::repeat(to_char(a), 3);");
                check_eq((size_t)1, error_count(r));
                check_true(has_diag(r, dlevel::error, "string::repeat"), r.format());
                check_true(has_diag(r, dlevel::error, "'char'"), r.format());
                check_eq((size_t)1, error_count(e->check("class C { string s = \"\"; }\nC c = C();\nc.s += 5;")));
                check_true(has_diag(e->check("class C { string s = \"\"; }\nC c = C();\nc.s++;"), dlevel::error, "cannot apply '++' to 'string'"));
                check_eq((size_t)1, error_count(e->check("string s = \"\";\ns += 2.5;")));
                // the spellings that work stay clean
                check_eq((size_t)0, error_count(e->check("string z = \"\";\nint a = 1;\nz += to_char(a);\nz += `${to_char(a)}${to_char(a)}`;\nz += \"x\";\nvar q = to_char(a) + 1;\nint n = to_char(a) + to_char(a);")));
                check_eq((size_t)0, error_count(e->check("var t = string::repeat(\"ab\", 2) + string::pad_left(\"7\", 3, '0') + string::pad_right(\"x\", 2, \"-\");\nutf8_string u = \"a\";\nvar w = string::repeat(u, 2);\narray<string> v;\nint a = 1;\nv.push(`${to_char(a)}`);\narray<string> elems = [\"x\"];\nelems[0] += 5;")));
                check_eq((size_t)0, error_count(e->check("var s = \"\";\ns += 5;")), "a var target stays dynamic");
            }
        });

        test("an_unknown_method_on_a_typed_container_string_or_scalar_is_a_load_error", [this]() {
            // the builtin registry both backends dispatch through is the member list of a declared array, string, utf8_string or
            // map (a map's keys are read with [], a stored callable called as m["f"](x)); it gives an int, float, bool or char none,
            // and same_as answers every value
            for (bool proofs : {true, false}) {
                auto e = make_engine();
                jai::stdlib::register_all(*e);
                e->type_proofs(proofs);
                auto r = e->check("array<string> a = [\"x\"];\na.splice(0, \"y\");");
                check_eq((size_t)1, error_count(r));
                check_true(has_diag(r, dlevel::error, "no member 'splice' on 'array<string>'"), r.format());
                check_true(has_diag(e->check("array<int> a = [1];\na.pusj(2);"), dlevel::error, "no member 'pusj' on 'array<int>' (did you mean 'push'?)"));
                check_true(has_diag(e->check("string s = \"abc\";\ns.frobnicate();"), dlevel::error, "no member 'frobnicate' on 'string'"));
                check_true(has_diag(e->check("map<string, int> m = {\"k\": 1};\nm.frobnicate();"), dlevel::error, "map has no member 'frobnicate'; read the key as [\"frobnicate\"]"));
                check_true(has_diag(e->check("map<string, int> m = {\"k\": 1};\nint v = m.k;"), dlevel::error, "map has no member 'k'; read the key as [\"k\"]"));
                check_true(has_diag(e->check("map<string, function> fm = {\"f\": [](int x) { return x; }};\nfm.f(1);"), dlevel::error, "map has no member 'f'; read the key as [\"f\"]"));
                check_eq((size_t)1, error_count(e->check("utf8_string u = \"a\";\nu.frobnicate();")));
                const std::pair<const char*, const char*> scalars[] = {
                    {"int i = 5;\ni.abs();", "int has no member 'abs'"},
                    {"print(5.abs());", "int has no member 'abs'"},
                    {"var x = 1.foo;", "int has no member 'foo'"},
                    {"int i = 5;\ni?.abs();", "int has no member 'abs'"},
                    {"float f = 1.5;\nint n = f.to_int();", "float has no member 'to_int'"},
                    {"bool b = true;\nvar v = b.size;", "bool has no member 'size'"},
                    {"char c = 'a';\nc.to_upper();", "char has no member 'to_upper'"},
                    {"int twice(int a) { return a * 2; }\ntwice(2).abs();", "int has no member 'abs'"},
                    {"var f = 5.same_as;", "'same_as' is a method of int; call it with obj.same_as() or wrap the call in a lambda"},
                };
                for (const auto& [source, text] : scalars) {
                    check_true(has_diag(e->check(source), dlevel::error, text), source);
                    auto run = make_engine();
                    run->type_proofs(proofs);
                    std::string raised;
                    try { run->execute(std::string("[[warn_mode]];\n") + source); } catch (const std::exception& error) { raised = error.what(); }
                    check_eq(std::string(text), raised, source);
                }
                check_eq((size_t)0, error_count(e->check("bool same = 5.same_as(5);\nchar c = 'a';\nbool also = c.same_as('a');\nvar v = 5;\nv.anything();\narray<int> xs = [1];\nxs[0].whatever();")));
                // the registry's names, maps that may hold callables, dynamic and element receivers stay clean
                check_eq((size_t)0, error_count(e->check("array<string> a = [\"x\"];\na.push(\"y\");\nint n = a.size();\nstring s = \"ab\";\ns.to_upper();\nvar k = s.find(\"b\");\nmap<string, int> m = {\"k\": 1};\nvar ks = m.keys();\nbool has = m.contains(\"k\");")));
                check_eq((size_t)0, error_count(e->check("map<string, function> fm = {\"f\": [](int x) { return x; }};\nfm[\"f\"](1);\nenum Dir { n, s }\nint d = Dir.s;\nvar vm = {\"g\": 1};\nvm.anything();\nvar dv = [1];\ndv.whatever();\narray<string> rows = [\"a\"];\nrows[0].whatever();")));
                auto w = make_engine();
                w->type_proofs(proofs);
                check_eq(std::string("array has no member 'splice'"), w->execute("[[warn_mode]]; array<string> a = [\"x\"];\nvar m = \"\";\ntry { a.splice(0, \"y\"); } catch (err) { m = err; }\nm;").to_string());
                check_true(has_diag(w->last_check_diagnostics(), dlevel::warning, "no member 'splice' on 'array<string>'"));
            }
        });

        test("a_subscript_key_a_typed_map_refuses_is_a_load_error", [this]() {
            // the key boundary checks the key's kind against the declared key type before any lookup (a char and its one-character
            // string are one key): a known key it refuses is the run time's text, read or write
            const std::pair<const char*, const char*> keys[] = {
                {"map<int,int> m;\nvar v = m[\"x\"];", "Cannot use 'string' as a key of type 'int'"},
                {"map<int,int> m;\nm[\"x\"] = 3;", "Cannot use 'string' as a key of type 'int'"},
                {"map<int,int> m;\nm[\"x\"] += 3;", "Cannot use 'string' as a key of type 'int'"},
                {"map<int,int> m = {1: 1};\nvar v = m[1.0];", "Cannot use 'float' as a key of type 'int'"},
                {"map<float,int> m = {1.0: 1};\nvar v = m[1];", "Cannot use 'int' as a key of type 'float'"},
                {"map<bool,int> m;\nm[1] = 2;", "Cannot use 'int' as a key of type 'bool'"},
                {"map<string,int> m;\nm[1] = 2;", "Cannot use 'int' as a key of type 'string'"},
                {"map<char,int> m;\nm[\"ab\"] = 2;", "Cannot convert 'string' to 'char': a char takes a string of exactly one character"},
                {"map<char,int> m;\nvar v = m[\"\"];", "Cannot convert 'string' to 'char': a char takes a string of exactly one character"},
            };
            for (bool proofs : {true, false}) {
                for (const auto& [source, text] : keys) {
                    auto e = make_engine();
                    e->type_proofs(proofs);
                    check_true(has_diag(e->check(source), dlevel::error, text), source);
                    std::string raised;
                    try { e->execute(std::string("[[warn_mode]];\n") + source); } catch (const std::exception& error) { raised = error.what(); }
                    check_eq(std::string(text), raised, source);
                }
                auto e = make_engine();
                e->type_proofs(proofs);
                check_eq((size_t)0, error_count(e->check("map<char,int> c;\nc[\"a\"] = 1;\nc['b'] = 2;\nmap<string,int> s;\ns['a'] = 1;\nvar k = \"x\";\nmap<int,int> m;\n"
                                                         "var d = m.has(\"x\");\nvar g = m.get(\"x\", 0);\nmap<var,int> any;\nany[\"x\"] = 1;\nany[2] = 2;\nmap<auto,int> open;\nopen[1] = 1;")));
            }
        });

        test("a_coroutine_handle_answers_next_and_done_only", [this]() {
            for (bool proofs : {true, false}) {
                auto e = make_engine();
                e->type_proofs(proofs);
                const std::string gen = "coroutine int g() { yield 1; yield 2; }\n";
                auto r = e->check(gen + "auto h = g();\nh.resume();");
                check_eq((size_t)1, error_count(r), r.format());
                check_true(has_diag(r, dlevel::error, "no member 'resume' on 'coroutine_handle' (did you mean 'next'?)"), r.format());
                check_true(has_diag(e->check(gen + "g().resume();"), dlevel::error, "no member 'resume' on 'coroutine_handle' (did you mean 'next'?)"));
                check_true(has_diag(e->check(gen + "class C { coroutine int m() { yield 1; } }\nauto c = C();\nauto h = c.m();\nh.resume();"), dlevel::error,
                                    "no member 'resume' on 'coroutine_handle' (did you mean 'next'?)"));
                check_true(has_diag(e->check(gen + "auto h = g();\nh.nxt();"), dlevel::error, "no member 'nxt' on 'coroutine_handle' (did you mean 'next'?)"));
                check_true(has_diag(e->check(gen + "auto h = g();\nvar f = h.resume;"), dlevel::error, "no member 'resume' on 'coroutine_handle'"));
                check_true(has_diag(e->check(gen + "auto h = g();\nh.next(1, 2);"), dlevel::error, "next() takes 0 or 1 arguments ([count])"));
                check_true(has_diag(e->check(gen + "auto h = g();\nh.next(\"2\");"), dlevel::error, "next() count must be an integer"));
                check_true(has_diag(e->check(gen + "auto h = g();\nh.done(1);"), dlevel::error, "done() takes no arguments"));
                check_eq((size_t)0, error_count(e->check(gen + "auto h = g();\nint a = h.next();\nvar b = h.next(2);\nbool d = h.done();\nfor (auto v : g()) { a += v; }\nh = g();")));
                // a user class's own resume is its method, and a var handle stays dynamic
                check_eq((size_t)0, error_count(e->check("class Timer { int resume() { return 1; } }\nauto t = Timer();\nt.resume();")));
                check_eq((size_t)0, error_count(e->check(gen + "var h = g();\nh.anything();")));
            }
        });

        test("reference_temporary_rules_keep_non_string_write_checks", [this]() {
            // string& admits writable temporaries; other reference types retain inferred read-only admission.
            const std::string decl = "int index_of_name(array<string>& names, string n, int def) { for (int i = 0; i < names.size(); ++i) { if (names[i] == n) { return i; } } return def; }\n";
            for (bool proofs : {true, false}) {
                auto e = make_engine();
                e->type_proofs(proofs);
                auto run = [&](const std::string& src) { return e->execute(src).to_string(); };
                check_eq((size_t)0, error_count(e->check(decl + "index_of_name([\"common\", \"rare\"], \"rare\", 0);")));
                check_eq(std::string("1"), run(decl + "index_of_name([\"common\", \"rare\"], \"rare\", 0);"));
                check_eq(std::string("hi bob|hi ab"), run("string greet(string& name) { return \"hi \" + name; }\ngreet(\"bob\") + \"|\" + greet(\"a\" + \"b\");"));
                check_eq(std::string("43"), run("int len(string& s) { return s.size(); }\nint outer(string& s) { return len(s); }\n"
                    "int count(array<int>& xs, int i) { if (i >= xs.size()) { return 0; } return 1 + count(xs, i + 1); }\n"
                    "outer(\"abcd\") * 10 + count([5, 6, 7], 0);"));
                check_eq(std::string("true"), run("bool same(array<int>& a, array<int>& b) { return same_object(a, b); }\nvoid grow(array<int>& xs) { xs.push(4); }\n"
                    "array<int> v = [1];\nbool s = same(v, v);\ngrow(v);\ns && v.size() == 2;"));
                check_eq(std::string("true"), run("class P { string name = \"p\"; bool is(string& n) { return name == n; } }\nP p = P();\np.is(\"p\");"));

                auto r = e->check("void inc(int& x) { x = x + 1; }\ninc(5);");
                check_eq((size_t)1, error_count(r));
                check_true(has_diag(r, dlevel::error, "'inc' writes through parameter 'x' (line 1); pass a variable"), r.format());
                check_eq((size_t)0, error_count(e->check("void shout(string& s) { s.to_upper(); }\nshout(\"hi\");")));
                check_eq((size_t)1, error_count(e->check("void shout(string& s) { s.to_upper(); }\nshout(5);")), "temporary admission adds no numeric-to-string reference conversion");
                check_eq((size_t)0, error_count(e->check("void set0(string& s) { s = \"x\"; }\nvoid outer2(string& s) {\n\tset0(s);\n}\nouter2(\"lit\");")));
                check_eq((size_t)1, error_count(e->check("void clear_all(array<int>& xs) { auto& r = xs; r.clear(); }\nclear_all([1]);")));
                check_eq((size_t)0, error_count(e->check("class K { void put(string& s) { s = \"\"; } }\nK k = K();\nk.put(\"lit\");")));
                const std::string log = "class Log { string last = \"\"; void say(string& m) { last = m; } }\n";
                check_eq((size_t)0, error_count(e->check(log + "Log l = Log();\nl.say(\"hi\");")));
                check_eq((size_t)0, error_count(e->check(log + "class Loud : Log { void say(string& m) override {\n\tm = m + \"!\";\n} }\nLog l = Log();\nl.say(\"hi\");")), "string literal admission is independent of overrides");
                check_eq((size_t)0, error_count(e->check(decl + "array<string> names = [\"a\"];\nindex_of_name(names, \"a\", 0);")));
                check_eq((size_t)0, error_count(e->check("void inc(int& x) { x = x + 1; }\nvoid inc(int x, int y) { }\nint v = 1;\ninc(v);")));

                check_eq(std::string("x"), run("string set_name(string& name) { name = \"x\"; return name; }\nstring g() { return \"a\"; }\nset_name(g());"));
                check_eq(std::string("3"), run("auto lf = [](string& s) { return s.size(); };\nlf(\"abc\");"));
                check_eq(std::string("2"), run("int peek(string& s) { return s.size(); }\nstring g2() { return \"ab\"; }\npeek(g2());"));
                run("int peek(string& s) { s = \"zzz\"; return 0; }");
                check_eq(std::string("0"), run("peek(g2());"), "a string parameter remains callable after reload adds a write");

                auto w = make_engine();
                w->type_proofs(proofs);
                check_eq(std::string("Cannot pass non-lvalue to reference parameter"),
                    w->execute("[[warn_mode]]; void inc(int& x) { x = x + 1; }\nvar m = \"\";\ntry { inc(5); } catch (err) { m = err; }\nm;").to_string());
                check_true(has_diag(w->last_check_diagnostics(), dlevel::warning, "'inc' writes through parameter 'x' (line 1); pass a variable"));
            }
        });

        // A reference parameter's default names storage as in C++: a literal or an operator's result is a load error, and at the bind
        // (in a [[warn_mode]] region) a default yielding no reference raises; a const T& default binds a temporary, an lvalue default binds it
        test("a_reference_parameters_default_must_name_storage", [this]() {
            auto e = make_engine();
            auto r = e->check("int peek(int& p = 5) { return p; }\nvoid push_one(array<int>& xs = [1]) { xs.push(2); }\nauto lf = [](int& q = 2 + 3) { return q; };");
            check_eq((size_t)3, error_count(r), r.format());
            check_true(has_diag(r, dlevel::error, "cannot bind non-lvalue default to reference parameter 'p'"), r.format());
            check_true(has_diag(r, dlevel::error, "cannot bind non-lvalue default to reference parameter 'xs'"), r.format());
            check_true(has_diag(r, dlevel::error, "cannot bind non-lvalue default to reference parameter 'q'"), r.format());
            check_eq((size_t)0, error_count(e->check("int g = 1;\narray<int> cs = [1];\nclass C { static int s = 2; int f = 3; }\nC c = C();\n"
                "int a(int& p = g) { return p; }\nint b(int& p = cs[0]) { return p; }\nint d(int& p = C::s) { return p; }\nint h(int& p = c.f) { return p; }\n"
                "int k(const int& p = 5) { return p; }\nint m(int& p = true ? g : g) { return p; }")));

            auto off = make_engine();
            check_eq(std::string("Cannot bind non-lvalue default to reference parameter 'p'"),
                off->execute("[[warn_mode]]; int peek(int& p = 5) { return p; }\nvar msg = \"\";\ntry { peek(); } catch (err) { msg = err; }\nmsg;").to_string());
            check_eq(std::string("Cannot bind non-lvalue default to reference parameter 'q'"),
                off->execute("[[warn_mode]]; auto lf = [](int& q = 2 + 3) { return q; };\nvar msg = \"\";\ntry { lf(); } catch (err) { msg = err; }\nmsg;").to_string());
            check_eq(std::string("7 5 11"),
                off->execute("[[warn_mode]]; int peek(int& p = 5) { return p; }\nint v = 7;\nint k(const int& p = 5) { return p; }\nint g = 1;\nvoid bump(int& p = g) { p += 10; }\nbump();\n"
                             "`${peek(v)} ${k()} ${g}`;").to_string());
        });

        // A conditional of two arm types is a converted temporary (C++), so a non-const reference cannot bind it in any position;
        // one arm type binds, and a const reference or a by-value parameter takes the mixed conditional
        test("a_reference_to_a_conditional_needs_one_arm_type", [this]() {
            auto e = make_engine();
            const std::string decls = "bool flag = true;\nint g = 1;\nfloat f = 2.5;\nint h = 2;\nvoid set9(int& p) { p = 9; }\n";
            const std::string text = "cannot bind a reference to a conditional of 'int' and 'float': its arms must have one type";
            for (const std::string body : {"int& r = flag ? g : f;", "auto& r = flag ? g : f;", "set9(flag ? g : f);",
                                           "function rr() -> int& { return flag ? g : f; }", "void d(int& p = flag ? g : f) { }",
                                           "cast<int&>(flag ? g : f) = 3;"}) {
                auto r = e->check(decls + body);
                check_eq((size_t)1, error_count(r), body + "\n" + r.format());
                check_true(has_diag(r, dlevel::error, text), r.format());
            }
            check_eq((size_t)0, error_count(e->check(decls + "auto& r = flag ? g : h;\nset9(flag ? g : h);\nconst auto& k = flag ? g : f;\n"
                "void byval(float p) { }\nbyval(flag ? g : f);")));
        });

        test("string_reference_temporaries_mutate_privately_and_variables_alias", [this]() {
            const std::string source = R"JAI(
                string normalize(string& value) {
                    value.trim();
                    value.to_upper();
                    value += "!";
                    return value;
                }
                string label = "  alice  ";
                string copy = label;
                string existing = normalize(label);
                string literal = normalize("  bob  ");
                string computed = normalize("  " + "cara  ");
                string cached() { return normalize("  cached  "); }
                label + "|" + copy + "|" + existing + "|" + literal + "|" + computed + "|" + cached() + "|" + cached();
            )JAI";
            for (bool vm : {false, true}) {
                for (bool proofs : {false, true}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                    e->type_proofs(proofs);
                    check_eq(std::string("ALICE!|  alice  |ALICE!|BOB!|CARA!|CACHED!|CACHED!"), e->execute(source).as_string());
                    check_eq((size_t)0, error_count(e->last_check_diagnostics()));
                }
            }
        });

        test("string_reference_temporaries_support_lambdas_methods_and_overloads", [this]() {
            const std::string source = R"JAI(
                auto append = [](string& value) { value += "!"; return value; };
                class Receiver {
                    string saved;
                    Receiver(string& value) { value += "?"; saved = value; }
                    string upper(string& value) { value.to_upper(); return value; }
                    static string decorate(string& value) { value += "."; return value; }
                    string pick(string& value) { value += "+"; return value; }
                    string pick(var& value) { value = 42; return "fallback"; }
                    int pick(int value) { return value + 1; }
                }
                string existing = "named";
                string lambda_named = append(existing);
                string lambda_literal = append("literal");
                Receiver receiver = Receiver("ctor");
                `${lambda_named}|${existing}|${lambda_literal}|${receiver.saved}|${receiver.upper("method")}|${Receiver::decorate("static")}|${receiver.pick("overload")}|${receiver.pick(4)}`;
            )JAI";
            for (bool vm : {false, true}) {
                for (bool proofs : {false, true}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                    e->type_proofs(proofs);
                    check_eq(std::string("named!|named!|literal!|ctor?|METHOD|static.|overload+|5"), e->execute(source).as_string());
                }
            }
        });

        test("string_reference_temporary_cells_survive_escaping_captures", [this]() {
            const std::string source = R"JAI(
                function retain(string& value) {
                    value += "!";
                    return [&]() { value += "?"; return value; };
                }
                string& keep(string& value) { return value; }
                auto first = retain("a");
                auto second = retain("b");
                auto& held = keep("held");
                held += "!";
                first() + "|" + second() + "|" + first() + "|" + held;
            )JAI";
            for (bool vm : {false, true}) {
                for (bool proofs : {false, true}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                    e->type_proofs(proofs);
                    check_eq(std::string("a!?|b!?|a!??|held!"), e->execute(source).as_string());
                }
            }
        });

        test("string_reference_temporaries_detach_bound_values_and_accept_host_calls", [this]() {
            for (bool vm : {false, true}) {
                std::string native_string{"native"};
                auto e = jai::foundry::make_default_engine();
                e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                e->add_global_ref("native_string", native_string);
                e->add_function("bound_text", [engine_ptr = e.get()]() { return engine_ptr->get_variable("native_string"); });
                check_eq(std::string("NATIVE"), e->execute("string upper(string& value) { value.to_upper(); return value; }\nupper(bound_text());").as_string());
                check_eq(std::string("native"), native_string, "a bound string used as a temporary detaches from native storage");
                check_eq(std::string("NATIVE"), e->execute("upper(native_string);").as_string());
                check_eq(std::string("NATIVE"), native_string, "a named native string remains an alias");
                auto callable = e->get_variable("upper");
                std::vector<script_value> args{e->make_value(std::string{"host"})};
                auto result = callable.as_function()(args);
                check_true(static_cast<bool>(result));
                if (result) {
                    check_eq(std::string("HOST"), result.value().as_string());
                }
                check_eq(std::string("host"), args[0].as_string(), "a host-supplied value argument has private mutation");
            }
        });

        test("string_reference_temporaries_are_private_in_parallel_workers", [this]() {
            const std::string source = R"JAI(
                int lengthen(string& value) { value += "!"; return value.size(); }
                array<int> values = [];
                for (int i = 0; i < 128; ++i) { values.push(0); }
                parallel_for (auto& value : values) { value = lengthen("a") + lengthen("b"); }
                int sum = 0;
                for (auto value : values) { sum += value; }
                sum;
            )JAI";
            for (bool vm : {false, true}) {
                for (size_t workers : {1u, 2u, 4u}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                    e->parallel_thread_count(workers);
                    check_eq(int64_t{512}, e->execute(source).as_int());
                }
            }
        });

        test("an_enum_declared_in_a_function_is_local_and_warns_when_it_hides_a_program_enum", [this]() {
            // the runtime scopes an enum written in a function or a block to that scope (both backends); the checker models it
            // (no false 'no value' against the program-scope enum) and warns when the declaration names a program-scope enum
            const std::string src = "enum Color { red, green, blue }\n"
                                    "int f() { enum Color { cyan, magenta, yellow, key } return Color.yellow; }\n"
                                    "int g() { enum Shade { dim, lit } return Shade.lit; }\n"
                                    "int b = 0;\n"
                                    "{ enum Color { one, two } b = Color.two; }\n"
                                    "f() * 100 + g() * 10 + b + Color.blue * 1000;";
            for (bool proofs : {true, false}) {
                auto e = make_engine();
                e->type_proofs(proofs);
                check_eq((int64_t)2211, e->execute(src).as_int());
                const check_report& r = e->last_check_diagnostics();
                check_eq((size_t)0, error_count(r));
                check_eq((size_t)2, warning_count(r));
                check_true(has_diag(r, dlevel::warning, "enum 'Color' declared in function 'f' is local to it: the program-scope 'Color' keeps its values"));
                check_true(has_diag(r, dlevel::warning, "enum 'Color' declared in a block is local to it"));
                check_false(has_diag(r, dlevel::warning, "enum 'Shade'"), "a local enum hiding nothing is silent");
                auto bad = make_engine();
                check_throws([&]() { bad->execute("enum Color { red }\nint f() { enum Color { cyan } return Color.red; }\nf();"); });
            }
        });

        test("program_closure_unit_parse_error_is_reported_first_and_alone", [this]() {
            auto dir = closure_dir("parse_err");
            write_unit(dir, "pe_b.jai", "void f(int var) {}\n");
            write_unit(dir, "pe_mid.jai", "import \"pe_b.jai\";\nfunction g() -> int { f(1); return 2; }\n");
            const std::string root = "import \"pe_mid.jai\";\nf(1);\ng();\n";
            auto e = program_engine(dir);
            std::string what;
            bool as_parse_error = false;
            try { e->execute(root); }
            catch (const parse_error& ex) { what = ex.what(); as_parse_error = true; }
            catch (const std::exception& ex) { what = ex.what(); }
            check_true(as_parse_error, "the unit's parse failure fails the root as a parse error");
            check_true(what.find("pe_b.jai:1:12: error: 'var' is a type keyword") != std::string::npos, what);
            check_true(what.find("undeclared identifier") == std::string::npos, "no cascade from the unit's missing names");
            check_false(e->has_function("g"), "nothing ran");
            auto r = e->check(root);
            check_eq((size_t)1, r.diagnostics.size());
            check_eq((size_t)1, error_count(r));
            check_true(r.diagnostics[0].message.find("pe_b.jai:1:12:") != std::string::npos, r.diagnostics[0].message);
            // the fix is seen on the next run
            write_unit(dir, "pe_b.jai", "void f(int v) {}\n");
            check_eq((int64_t)2, e->execute(root).as_int());
            std::filesystem::remove_all(dir);
        });

        test("program_closure_survives_leaf_hot_edit", [this]() {
            auto dir = closure_dir("hot_edit");
            write_unit(dir, "hot_a.jai", "function read_g() -> int { return G.v; }\n");
            write_unit(dir, "hot_b.jai", "class Game { int v = 7; }\nvar G = Game();\n");
            auto e = program_engine(dir);
            e->set_import_behavior(engine::import_behavior::always);
            const std::string root = "import \"hot_a.jai\";\nimport \"hot_b.jai\";\nread_g()";
            check_eq((int64_t)7, e->execute(root).as_int());
            write_unit(dir, "hot_a.jai", "function read_g() -> int { return G.v * 10; }\n");
            check_eq((int64_t)70, e->execute(root).as_int());
            write_unit(dir, "hot_a.jai", "function read_g() -> int { return G.v * 100; }\n");
            check_eq((int64_t)700, e->execute(root).as_int());
            std::filesystem::remove_all(dir);
        });

        test("program_name_memory_outlives_the_closure", [this]() {
            auto dir = closure_dir("memory");
            write_unit(dir, "mem_x.jai", "function x_fn() -> int { return y_fn(); }\nthrow \"stop before y\";\n");
            write_unit(dir, "mem_y.jai", "function y_fn() -> int { return 1; }\n");
            auto e = program_engine(dir);
            check_throws([&]() { e->execute("import \"mem_x.jai\";\nimport \"mem_y.jai\";\n"); });
            check_false(e->has_function("y_fn"), "the program stopped before y ran");
            // a unit checked on its own still knows the program's names...
            check_eq((size_t)0, error_count(e->check("function q() -> int { return y_fn(); }")));
            // ...where an engine that never saw the program does not
            check_eq((size_t)1, error_count(make_engine()->check("function q() -> int { return y_fn(); }")));
            std::filesystem::remove_all(dir);
        });

        test("program_closure_cycle_checks_and_executes", [this]() {
            auto dir = closure_dir("cycle");
            write_unit(dir, "cyc_a.jai", "import \"cyc_b.jai\";\nfunction from_a() -> int { return from_b() + 1; }\n");
            write_unit(dir, "cyc_b.jai", "import \"cyc_a.jai\";\nfunction from_b() -> int { return 41; }\n");
            auto e = program_engine(dir);
            check_eq((size_t)0, error_count(e->check("import \"cyc_a.jai\";\nfrom_a()")));
            check_eq((int64_t)42, e->execute("import \"cyc_a.jai\";\nfrom_a()").as_int());
            std::filesystem::remove_all(dir);
        });

        test("program_executed_again_unchanged_reads_and_checks_nothing", [this]() {
            // a load reads each file once, a unit an import runs keeps the verdict the root's gather gave it, and the program's
            // own classes becoming defined as it runs move nothing the checker read
            auto dir = closure_dir("again");
            write_unit(dir, "again_app.jai", "import \"again_store.jai\";\nimport \"again_views.jai\";\nfunction page() -> string { return render(Store()); }\n");
            write_unit(dir, "again_store.jai", "class Store { int rows = 3; }\n");
            write_unit(dir, "again_views.jai", "import \"again_store.jai\";\nfunction render(Store s) -> string { return \"rows \" + to_string(s.rows); }\n");
            auto e = program_engine(dir);
            jai::stdlib::register_all(*e);
            const std::string root = "import(\"again_app.jai\");";
            e->execute(root);
            check_eq((uint64_t)3, detail::engine_access::source_read_count(*e), "one read per file");
            check_eq((uint64_t)4, detail::engine_access::check_walk_count(*e), "the root and each unit checked once");
            e->execute(root);
            e->execute(root);
            check_eq((uint64_t)3, detail::engine_access::source_read_count(*e), "an unchanged program reads nothing again");
            check_eq((uint64_t)4, detail::engine_access::check_walk_count(*e), "an unchanged program checks nothing again");
            check_eq(std::string("rows 3"), e->execute("page()").to_string());
            write_unit(dir, "again_store.jai", "class Store { int rows = 3; int cols = 4; }\n");
            check_eq(std::string("rows 3"), e->execute(root + "\npage()").to_string());
            write_unit(dir, "again_store.jai", "class Store { int rows = \"three\"; }\n");
            check_throws([&]() { e->execute(root); }, "a broken edit is rejected");
            check_eq(std::string("rows 3"), e->execute("page()").to_string(), "the program keeps the code it had");
            std::filesystem::remove_all(dir);
        });

        test("program_closure_stamps_proofs_in_units", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            auto dir = closure_dir("proofs");
            write_unit(dir, "typed_unit.jai",
                "int a = 1; a = 2;\nint twice(int v) { return v * 2; }\nint r = twice(a);\n");
            auto e = program_engine(dir);
            e->execute("import \"typed_unit.jai\";\n");
            const proof_stats& st = e->last_proof_stats();
            check_ge(st.stores, (uint32_t)1, "the unit's same-type store proves");
            check_eq((uint32_t)1, st.args, "the unit's single-overload call proves its argument");
            check_eq((int64_t)4, e->execute("r").as_int());
            std::filesystem::remove_all(dir);
        });

        // ------------------------------------------------------------ programs (class tables across units)

        test("program_closure_subclass_resolves_parent_declared_in_another_unit", [this]() {
            auto dir = closure_dir("xunit_parent");
            write_unit(dir, "xp_a.jai", "class A { int hp = 3; int bonus() { return 2; } }\n");
            auto e = program_engine(dir);
            // bare inherited field + method inside the subclass's method
            const std::string root = "import \"xp_a.jai\";\nclass B : A { int tick() { return hp + bonus(); } }\nB().tick()";
            auto r = e->check(root);
            check_eq((size_t)0, error_count(r), r.format(root));
            check_eq((int64_t)5, e->execute(root).as_int());
            // this.x, a receiver typed as the subclass, and a receiver typed as the parent
            const std::string root2 = "import \"xp_a.jai\";\nclass B : A { int tick() { return this.hp + this.bonus(); } }\n"
                                      "B b = B();\nA a = b;\nb.hp + b.bonus() + b.tick() + a.hp";
            auto r2 = e->check(root2);
            check_eq((size_t)0, error_count(r2), r2.format(root2));
            check_eq((int64_t)13, e->execute(root2).as_int());
            // and a genuine miss on the same receivers is still the error the same-unit case reports
            auto miss = e->check("import \"xp_a.jai\";\nclass B : A { int tick() { return this.zzz_nope; } }\nB b = B();\nb.zzz_nope2");
            check_eq((size_t)2, error_count(miss));
            check_true(has_diag(miss, dlevel::error, "no member 'zzz_nope' in class 'B'"));
            check_true(has_diag(miss, dlevel::error, "no member 'zzz_nope2' in class 'B'"));
            std::filesystem::remove_all(dir);
        });

        test("program_closure_three_unit_class_chain", [this]() {
            auto dir = closure_dir("xunit_chain");
            write_unit(dir, "ch_a.jai", "class A { int a = 1; }\n");
            write_unit(dir, "ch_b.jai", "import \"ch_a.jai\";\nclass B : A { int b = 2; int ab() { return a + b; } }\n");
            write_unit(dir, "ch_c.jai", "import \"ch_b.jai\";\nclass C : B { int c = 4; int sum() { return a + b + c + ab(); } }\n");
            auto e = program_engine(dir);
            const std::string root = "import \"ch_c.jai\";\nclass D : C { int d = 8; int all() { return a + b + c + d + sum(); } }\nD().all()";
            auto r = e->check(root);
            check_eq((size_t)0, error_count(r), r.format(root));
            check_eq((int64_t)25, e->execute(root).as_int());
            std::filesystem::remove_all(dir);
        });

        test("program_closure_chain_ends_in_a_cpp_class", [this]() {
            // the creature shape: script leaf -> script kind (another unit) -> C++ base (dynamic_binder)
            auto dir = closure_dir("xunit_cpp");
            write_unit(dir, "cpp_kind.jai", "class Kind : ProbeObject { int t = 1; Kind() : super() {} int twice() { return doubled(); } }\n");
            auto e = program_engine(dir);
            dynamic_binder<probe_object>(*e, "ProbeObject")
                .constructor<>()
                .method("doubled", &probe_object::doubled)
                .property("value", &probe_object::value)
                .build();
            const std::string root = "import \"cpp_kind.jai\";\n"
                                     "class W : Kind { W() : super() {} int tick() { return doubled() + twice() + t; } }\n"
                                     "W w = W();\nw.value = 5;\nw.tick() + w.doubled() + w.t + w.value";
            auto r = e->check(root);
            check_eq((size_t)0, error_count(r), r.format(root));
            check_eq((int64_t)37, e->execute(root).as_int());
            // a bare C++ property resolves through the chain too - checked AND executed
            const std::string prop_src = "import \"cpp_kind.jai\";\nclass W : Kind { W() : super() {} int v() { return value; } }\nW w2 = W();\nw2.value = 5;\nw2.v()";
            auto prop = e->check(prop_src);
            check_eq((size_t)0, error_count(prop), prop.format(prop_src));
            check_eq((int64_t)5, e->execute(prop_src).as_int());
            // a miss below the C++ base is still certain (closed binder surface)
            auto miss = e->check("import \"cpp_kind.jai\";\nclass W : Kind { int tick() { return zzz_nope; } }");
            check_eq((size_t)1, error_count(miss));
            check_true(has_diag(miss, dlevel::error, "use of undeclared identifier 'zzz_nope'"));
            std::filesystem::remove_all(dir);
        });

        test("program_closure_typed_mismatch_against_cross_unit_parent_is_the_same_error", [this]() {
            auto dir = closure_dir("xunit_mismatch");
            const std::string a_src = "class A { int hp = 3; int bonus() { return 2; } }\n";
            write_unit(dir, "xm_a.jai", a_src);
            struct row { const char* body; const char* text; };
            const row rows[] = {
                {"class B : A { void f() { hp = \"s\"; } }", "cannot assign 'string' to field 'hp' declared 'int'"},
                {"class B : A { void f() { this.hp = \"s\"; } }", "cannot assign 'string' to field 'hp' declared 'int'"},
                {"class B : A { }\nB b = B();\nb.hp = \"s\";", "cannot assign 'string' to field 'hp' declared 'int'"},
                {"class B : A { int f() { return bonus(\"x\"); } }", "'bonus' expects 0 argument(s), got 1"},
                {"class B : A { }\nB b = B();\nb.bonus(1, 2);", "'bonus' expects 0 argument(s), got 2"},
            };
            for (const auto& rw : rows) {
                auto cross = program_engine(dir)->check("import \"xm_a.jai\";\n" + std::string(rw.body));
                auto same = make_engine()->check(a_src + rw.body);
                check_eq((size_t)1, error_count(cross), rw.body);
                check_eq((size_t)1, error_count(same), rw.body);
                check_eq(same.diagnostics[0].message, cross.diagnostics[0].message);
                check_true(cross.diagnostics[0].message.find(rw.text) != std::string::npos, rw.text);
                // the same-unit report points at the declaration; the cross-unit one cannot (it is in another file)
                check_true(same.diagnostics[0].related_line != 0, "same-unit related location");
                check_eq((size_t)0, cross.diagnostics[0].related_line, "cross-unit related location");
                // strict gates the load with that text
                std::string what;
                try { program_engine(dir)->execute("import \"xm_a.jai\";\n" + std::string(rw.body)); }
                catch (const static_check_error& ex) { what = ex.what(); }
                check_true(what.find(rw.text) != std::string::npos, what);
            }
            std::filesystem::remove_all(dir);
        });

        test("program_closure_unknown_parent_keeps_bare_names_lenient", [this]() {
            // a parent nowhere - not this unit, not the program, not registered - can define anything
            auto e = make_engine();
            auto ext = e->check("extern MysteryKind;\nclass W : MysteryKind { int tick() { hp = 1; return hp + bonus(); } }\n"
                                "class X : W { int t2() { return [this]() { return hp; }(); } }");
            check_eq((size_t)0, error_count(ext), ext.format());
            check_eq((size_t)0, warning_count(ext), ext.format());
            auto inc = e->check("include \"nowhere_base.jai\"\nclass Sub : MysteryBase { int tick() { return hp + bonus(); } }");
            check_eq((size_t)0, error_count(inc), inc.format());
            // a known chain is still checked
            auto known = e->check("class K { int hp = 1; }\nclass W : K { int tick() { return hp + zzz_nope; } }");
            check_eq((size_t)1, error_count(known));
            check_true(has_diag(known, dlevel::error, "use of undeclared identifier 'zzz_nope'"));
        });

        test("program_closure_cross_unit_parent_proofs_wait_for_registration", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            auto dir = closure_dir("xunit_proofs");
            write_unit(dir, "pf_a.jai", "class A { int hp = 3; }\n");
            write_unit(dir, "pf_b.jai", "class B : A { int tick() { int x = hp; return hp; } }\n");   // no includes: proof gates open
            const std::string root = "import \"pf_a.jai\";\nimport \"pf_b.jai\";\nB().tick()";
            for (bool proofs : {true, false}) {
                auto e = program_engine(dir);
                e->type_proofs(proofs);
                check_eq((int64_t)3, e->execute(root).as_int());
                check_eq((int64_t)3, e->execute(root).as_int());
            }
            // last_proof_stats() is the last unit checked: pf_b.jai. Its FIRST check knows A only
            // through the program table - nothing proves. The same two classes in one unit prove
            // the return on their first check.
            auto e = program_engine(dir);
            check_eq((size_t)0, error_count(e->check(root)));
            check_eq((uint32_t)0, e->last_proof_stats().returns, "cross-unit first check: no return proof");
            auto one = make_engine();
            check_eq((size_t)0, error_count(one->check("class A { int hp = 3; }\nclass B : A { int tick() { int x = hp; return hp; } }\nB().tick()")));
            check_ge(one->last_proof_stats().returns, (uint32_t)1, "same-unit first check proves the return");
            // once pf_a's import has defined A, pf_b's deferred proofs re-stamp with the registered class in view: the return
            // proves as in the same-unit check (no entry of this engine names B, so pf_b's re-stamp is the last pass)
            auto run = program_engine(dir);
            run->execute("import \"pf_a.jai\";\nimport \"pf_b.jai\";\n");
            check_ge(run->last_proof_stats().returns, (uint32_t)1, "registered: return proof");
            check_eq((int64_t)3, run->execute(root).as_int());
            std::filesystem::remove_all(dir);
        });

        // ------------------------------------------------------------ escape hatches

        test("extern_declares_a_name_the_host_registers_later", [this]() {
            auto e = make_engine();
            e->execute("extern foo;\nfunction use_foo() -> int { return foo + 1; }");
            e->add_global("foo", script_value((int64_t)41, e.get()));
            check_eq((int64_t)42, e->execute("use_foo()").as_int());
            // the same unit without the extern is the genuine error
            auto alone = make_engine();
            auto r = alone->check("function use_foo() -> int { return foo + 1; }");
            check_eq((size_t)1, error_count(r));
            check_true(r.diagnostics[0].message.find("use of undeclared identifier 'foo'") != std::string::npos);
        });

        test("extern_typed_checks_members_and_proves_stores", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            auto e = make_engine();
            auto r = e->check("class Game { int v = 1; }\nextern Game G;\nfunction bad() -> int { return G.missing; }");
            check_eq((size_t)1, error_count(r));
            check_true(r.diagnostics[0].message.find("no member 'missing'") != std::string::npos,
                "typed extern receiver resolves members (got: " + r.diagnostics[0].message + ")");
            check_eq((size_t)0, error_count(e->check("class Game { int v = 1; }\nextern shared_ptr<Game> G;\nfunction ok() -> int { return G.v; }")));
            // a same-type store through a typed extern proves; the untyped form stays dynamic
            auto p = make_engine();
            p->execute("extern int counter;\nfunction bump() { counter = 7; }");
            check_ge(p->last_proof_stats().stores, (uint32_t)1, "typed extern store proves");
            p->add_global("counter", script_value((int64_t)0, p.get()));
            p->execute("bump()");
            check_eq((int64_t)7, p->execute("counter").as_int());
            auto d = make_engine();
            d->execute("extern loose;\nfunction set_loose() { loose = 7; }");
            check_eq((uint32_t)0, d->last_proof_stats().stores, "untyped extern store is unproven");
            check_eq((size_t)0, error_count(d->check("extern loose;\nfunction f() { loose = \"any\"; loose = 1.5; }")));
        });

        test("extern_typed_proofs_stay_guarded_against_a_mismatching_host_value", [this]() {
            // the host binds a string where the unit declared int: proofs on and off must agree
            const char* src = "extern int hp;\nfunction read_hp() -> int { return hp; }\nfunction set_hp() { hp = 5; }\n"
                              "var a = 0; try { a = read_hp(); } catch (ex) { a = -1; }\n"
                              "set_hp();\nhp";
            std::string with_proofs, without_proofs;
            for (bool proofs : { true, false }) {
                auto e = make_engine();
                e->type_proofs(proofs);
                e->add_global("hp", script_value(std::string("full"), e.get()));
                (proofs ? with_proofs : without_proofs) = outcome(*e, src);
            }
            check_eq(without_proofs, with_proofs);
            check_true(with_proofs.find("Cannot assign int to 'hp' (declared string)") != std::string::npos, with_proofs);
        });

        test("extern_function_allows_unchecked_calls", [this]() {
            auto e = make_engine();
            check_eq((size_t)0, error_count(e->check("extern function f;\nfunction g() -> int { return f(1, 2); }")));
            e->execute("extern function f;\nfunction g() -> int { return f(1, 2); }");
            e->add_function("f", [](script_int a, script_int b) { return a + b; });
            check_eq((int64_t)3, e->execute("g()").as_int());
            // extern names ride the program closure like any declared global
            auto dir = closure_dir("extern_leaf");
            write_unit(dir, "extern_decl.jai", "extern function host_fn;\n");
            write_unit(dir, "extern_use.jai", "function call_host() -> int { return host_fn(20); }\n");
            auto p = program_engine(dir);
            check_eq((size_t)0, error_count(p->check("import \"extern_use.jai\";\nimport \"extern_decl.jai\";\n1")));
            p->add_function("host_fn", [](script_int a) { return a + 1; });
            check_eq((int64_t)21, p->execute("import \"extern_use.jai\";\nimport \"extern_decl.jai\";\ncall_host()").as_int());
            std::filesystem::remove_all(dir);
        });

        test("extern_without_a_definition_raises_at_its_first_use", [this]() {
            // An extern declares nothing; a use while nothing defines the name raises, and `inline` creates it
            auto e = make_engine();
            e->execute("extern ghost;\nfunction touch() -> int { return ghost; }");
            std::string read = outcome(*e, "touch()");
            check_true(read.rfind("ERROR 'ghost' is declared extern but never defined (an 'inline' declaration creates it)", 0) == 0, read);
            check_eq((size_t)0, error_count(e->check("extern int ghost;\nfunction f() -> int { return ghost; }")));
            check_true(e->execute("inline int ghost;\nghost == 0").as_bool(), "inline creates the variable with its type's default");
            check_eq((int64_t)0, e->execute("touch()").as_int());
            auto f = make_engine();
            std::string call = outcome(*f, "extern function host_fn;\nhost_fn(1)");
            check_true(call.rfind("ERROR 'host_fn' is declared extern but never defined", 0) == 0, call);
        });

        test("extern_declares_a_qualified_name_the_host_registers_later", [this]() {
            // a qualified name its namespace does not declare is a load error until an extern declares it, at program scope or in
            // a namespace (there it is qualified by the namespace); a use while nothing defines it raises the extern's text
            const auto message = [this](const std::string& source) {
                auto e = make_engine();
                try { e->execute(source); } catch (const static_check_error& err) { return std::string(err.what()); }
                return std::string("loaded");
            };
            check_true(message("bool t() { return time::now() >= 0.0; }").find("error: use of undeclared identifier 'time'") != std::string::npos,
                       message("bool t() { return time::now() >= 0.0; }"));
            check_true(message("namespace game { int k = 1; } int t() { return game::tick; }").find("error: no member 'tick' in namespace 'game'") != std::string::npos);
            auto e = make_engine();
            e->execute("extern function time::now;\nextern int game::tick;\nextern a::b::c;\n"
                       "namespace zone { extern function spawn; extern int depth; extern inner::depth; int level() { return depth + 1; } }\n"
                       "bool t() { return time::now() >= 0.0; }\nint tick() { return game::tick + 1; }\nvoid set_tick() { game::tick = 2; }\n"
                       "void bump_tick() { game::tick += 2; }\nvar read_c() { return a::b::c; }\nvoid set_c() { a::b::c = 1; }\n"
                       "int spawn_one() { return zone::spawn(1); }\nvar inner_depth() { return zone::inner::depth; }\n");
            const auto raises = [&](const std::string& source, const std::string& text) {
                const std::string got = outcome(*e, source);
                check_true(got.rfind("ERROR " + text, 0) == 0, source + " -> " + got);
            };
            raises("t();", "'time::now' is declared extern but never defined");
            raises("tick();", "'game::tick' is declared extern but never defined (an 'inline' declaration creates it)");
            raises("set_tick();", "'game::tick' is declared extern but never defined (an 'inline' declaration creates it)");
            raises("bump_tick();", "'game::tick' is declared extern but never defined (an 'inline' declaration creates it)");
            raises("read_c();", "'a::b::c' is declared extern but never defined (an 'inline' declaration creates it)");
            raises("set_c();", "'a::b::c' is declared extern but never defined (an 'inline' declaration creates it)");
            raises("spawn_one();", "'zone::spawn' is declared extern but never defined");
            raises("zone::level();", "'depth' is declared extern but never defined (an 'inline' declaration creates it)");
            raises("inner_depth();", "'zone::inner::depth' is declared extern but never defined (an 'inline' declaration creates it)");
            jai::stdlib::register_time_functions(*e);
            check_true(e->execute("t();").as_bool(), "the host registered time::now after the script loaded");
            e->execute("namespace game { int tick = 41; }\nnamespace a::b { var c = \"c\"; }\nnamespace zone { int spawn(int n) { return n + 6; } int depth = 6; }");
            check_eq((int64_t)42, e->execute("tick();").as_int(), "a later unit defines game::tick");
            check_eq(std::string("c"), e->execute("read_c();").as_string());
            check_eq((int64_t)7, e->execute("spawn_one();").as_int(), "a later unit defines zone::spawn");
            check_eq((int64_t)7, e->execute("zone::level();").as_int(), "a bare name in the namespace reads the extern");
            // the typed form types the qualified name as a plain typed extern types its name; the namespace opens for its other members
            const auto errors = [this](const std::string& source) {
                auto c = make_engine();
                std::string result;
                for (const auto& d : c->check(source).diagnostics) {
                    if (d.severity == dlevel::error) { result += d.message + "\n"; }
                }
                return result;
            };
            check_eq(std::string("cannot assign 'string' to 'game::tick' declared 'int'\n"),
                     errors("extern int game::tick;\nvoid f() { game::tick = \"fast\"; }"));
            check_eq(std::string("cannot initialize 's' declared 'string' with 'int': stringify it with to_string(x) or `${x}`\n"),
                     errors("extern int game::tick;\nstring s = game::tick;"));
            const std::string constant = errors("extern const int game::cap;\nvoid f() { game::cap = 2; }");
            check_true(constant.find("'game::cap'") != std::string::npos, constant);
            check_eq(std::string("no member 'd' in namespace 'a::b'\n"), errors("extern a::b::c;\nvar x = a::b::c;\nvar y = a::b::d;"));
            check_eq(std::string(), errors("namespace game { extern int tick; int next() { return tick + 1; } }\nint n = game::tick;\n"
                                           "extern function game::spawn;\nvoid f() { game::spawn(1, \"any\"); }"));
        });

        test("inline_declares_a_checked_global", [this]() {
            auto e = make_engine();
            check_eq((size_t)0, error_count(e->check("inline int hp;\nfunction f() -> int { return hp + 1; }")));
            auto r = e->check("inline int hp = 3;\nfunction g() { hp = \"full\"; }");
            check_eq((size_t)1, error_count(r));
            check_true(r.diagnostics[0].message.find("cannot assign 'string'") != std::string::npos, r.diagnostics[0].message);
            check_eq((size_t)0, error_count(e->check("inline cfg;\nfunction h() { cfg = \"any\"; cfg = 1.5; }")));
        });

        test("inline_and_plain_declarations_across_the_closure_are_load_errors", [this]() {
            // either order across units is one load error, at the plain declaration; one unit's own mix converts at run time
            auto e = make_engine();
            check_eq((size_t)0, error_count(e->check("inline int frags;\nint frags = 1;")));
            check_eq((size_t)0, error_count(e->check("int frags = 1;\ninline int frags;")));
            auto dir = closure_dir("inline_mix");
            write_unit(dir, "hud.jai", "inline int frags;\n");
            write_unit(dir, "stats.jai", "int kills;\n");
            auto plain = program_engine(dir)->check("import \"hud.jai\";\nint frags = 3;\n");
            check_eq((size_t)1, error_count(plain));
            check_true(plain.diagnostics[0].message == "'frags' is already defined inline; declare it 'inline int frags;' to share it", plain.diagnostics[0].message);
            auto inl = program_engine(dir)->check("import \"stats.jai\";\ninline int kills;\n");
            check_eq((size_t)1, error_count(inl));
            const std::string m = inl.diagnostics[0].message;
            check_true(m.find("stats.jai:1:5: 'kills' is already defined inline; declare it 'inline int kills;' to share it") != std::string::npos, m);
            check_eq((size_t)0, error_count(program_engine(dir)->check("import \"hud.jai\";\ninline int frags = 3;\n")));
            check_eq((size_t)0, error_count(program_engine(dir)->check("import \"stats.jai\";\nint kills = 2;\n")));
            std::filesystem::remove_all(dir);
        });

        test("extern_in_a_shared_leaf_declares_an_optional_siblings_names", [this]() {
            // A leaf every root imports calls into a module only SOME roots import, behind a
            // runtime flag: the single-player program never declares those names, so the leaf
            // says so; where the sibling really defines them the extern is a no-op.
            auto dir = closure_dir("optional_sibling");
            write_unit(dir, "game.jai", "var NET_GAME = false;\n");
            write_unit(dir, "net.jai", "import \"game.jai\";\nvar NET_FRAGS = 7;\nfunction np_hit(int d) -> int { return d + NET_FRAGS; }\nfunction np_start() { NET_GAME = true; }\n");
            const std::string leaf = "import \"game.jai\";\nfunction hit(int d) -> int { if (NET_GAME) { return np_hit(d); } return d; }\n";
            write_unit(dir, "things.jai", leaf);
            const std::string single = "import \"things.jai\";\nhit(1)";
            const std::string netplay = "import \"things.jai\";\nimport \"net.jai\";\nnp_start();\nhit(1)";
            auto bare = program_engine(dir);
            check_eq((size_t)1, error_count(bare->check(single)), "the single-player program never declares np_hit");
            check_throws([&]() { bare->execute(single); });
            check_eq((size_t)0, error_count(program_engine(dir)->check(netplay)));
            write_unit(dir, "things.jai", "extern function np_hit;\n" + leaf);
            auto sp = program_engine(dir);
            check_eq((size_t)0, error_count(sp->check(single)));
            check_eq((int64_t)1, sp->execute(single).as_int());
            auto np = program_engine(dir);
            check_eq((size_t)0, error_count(np->check(netplay)));
            check_eq((int64_t)8, np->execute(netplay).as_int(), "the extern is a no-op where net.jai defines np_hit");
            std::filesystem::remove_all(dir);
        });

        // ------------------------------------------ [[warn_mode]] / [[strict_mode]]

        test("warn_mode_unit_downgrades_errors_and_keeps_the_runtime", [this]() {
            auto e = make_engine();
            auto r = e->check("[[warn_mode]];\nint x = \"s\";\nfunction f() -> int { return zzz_missing; }");
            check_eq((size_t)0, error_count(r));
            check_eq((size_t)2, warning_count(r));
            check_true(r.format().find("warning: cannot initialize 'x' declared 'int' with 'string'") != std::string::npos);
            // strict loads it; the runtime keeps its conversion error (both backends)
            std::string what = outcome(*e, "[[warn_mode]];\nint x = \"s\";\nx");
            check_true(what.find("ERROR Cannot initialize") == 0, "runtime rejection survives (got: " + what + ")");
            check_false(what.find("static check failed") != std::string::npos);
            // without the attribute the same unit fails the static gate
            std::string strict = outcome(*make_engine(), "int x = \"s\";\nx");
            check_true(strict.find("ERROR static check failed") == 0, "got: " + strict);
            // no trailing ';' and a leading comment are both fine; a dangling attribute covers nothing
            check_eq((size_t)0, error_count(make_engine()->check("// legacy unit\n[[warn_mode]]\nint x = \"s\";")));
            check_eq((size_t)0, error_count(make_engine()->check("int y = 1;\n[[warn_mode]];")));
            // unit-scoped: a warn_mode leaf does not fail the program it belongs to
            auto dir = closure_dir("warn_leaf");
            write_unit(dir, "warn_leaf.jai", "[[warn_mode]];\nfunction leaf_f() -> int { return zzz_nope; }\n");
            auto p = program_engine(dir);
            check_eq((size_t)0, error_count(p->check("import \"warn_leaf.jai\";\n1")));
            check_eq((int64_t)1, p->execute("import \"warn_leaf.jai\";\n1").as_int());
            std::filesystem::remove_all(dir);
        });

        test("statement_attribute_covers_one_statement", [this]() {
            auto r = make_engine()->check("int x = 5;\n[[warn_mode]] y = 5;\nz = 5;");
            check_eq((size_t)1, error_count(r), r.format());
            check_eq((size_t)1, warning_count(r), r.format());
            check_true(has_diag(r, dlevel::warning, "undeclared identifier 'y'"), r.format());
            check_true(has_diag(r, dlevel::error, "undeclared identifier 'z'"), r.format());
            // two attributed statements in a row
            r = make_engine()->check("int x = 1;\n[[warn_mode]] y = 5;\n[[warn_mode]] z = 5;\nw = 5;");
            check_eq((size_t)1, error_count(r), r.format());
            check_eq((size_t)2, warning_count(r), r.format());
            check_true(has_diag(r, dlevel::error, "undeclared identifier 'w'"), r.format());
            // an if body without braces is a statement position too
            r = make_engine()->check("int x = 1;\nif (x > 0) [[warn_mode]] y = 5;\nz = 5;");
            check_eq((size_t)1, error_count(r), r.format());
            check_true(has_diag(r, dlevel::warning, "undeclared identifier 'y'"), r.format());
            check_true(has_diag(r, dlevel::error, "undeclared identifier 'z'"), r.format());
            // the statement's whole subtree, a nested block included
            r = make_engine()->check("int x = 1;\n[[warn_mode]] if (x > 0) { y = 5; }\nz = 5;");
            check_eq((size_t)1, error_count(r), r.format());
            check_true(has_diag(r, dlevel::warning, "undeclared identifier 'y'"), r.format());
        });

        test("block_first_attribute_covers_the_block", [this]() {
            auto r = make_engine()->check("int x = 1;\n{ [[warn_mode]] y = 5; z = 5; }");
            check_eq((size_t)0, error_count(r), r.format());
            check_eq((size_t)2, warning_count(r), r.format());
            // the mode reverts after the block
            r = make_engine()->check("int x = 1;\n{ [[warn_mode]] y = 5; z = 5; }\nw = 5;");
            check_eq((size_t)1, error_count(r), r.format());
            check_true(has_diag(r, dlevel::error, "undeclared identifier 'w'"), r.format());
            // function body and while body; innermost wins inside the warn block
            r = make_engine()->check("function f() { [[warn_mode]] y = 5; z = 5; [[strict_mode]] q = 5; }\nwhile (false) { [[warn_mode]] w = 1; }");
            check_eq((size_t)1, error_count(r), r.format());
            check_eq((size_t)3, warning_count(r), r.format());
            check_true(has_diag(r, dlevel::error, "undeclared identifier 'q'"), r.format());
            // a block-level region flows into nested lambda bodies
            r = make_engine()->check("function f() { [[warn_mode]] g([](){ x = 5; }); }");
            check_eq((size_t)0, error_count(r), r.format());
            check_eq((size_t)2, warning_count(r), r.format());
        });

        test("statement_attribute_stops_at_lambda_bodies", [this]() {
            // the call site is lenient; the lambda body keeps the enclosing (strict) mode
            auto r = make_engine()->check("function f() { int k = 1;\n[[warn_mode]] self.spawnOnNetwork([](){ x = 5; }); }");
            check_eq((size_t)1, error_count(r), r.format());
            check_true(has_diag(r, dlevel::warning, "undeclared identifier 'self'"), r.format());
            check_true(has_diag(r, dlevel::error, "undeclared identifier 'x'"), r.format());
            // block-first inside the lambda demotes its body only; the outer call is still checked
            r = make_engine()->check("function f() { int k = 1;\nself.spawnOnNetwork([](){ [[warn_mode]] x = 5; }); }");
            check_eq((size_t)1, error_count(r), r.format());
            check_true(has_diag(r, dlevel::error, "undeclared identifier 'self'"), r.format());
            check_true(has_diag(r, dlevel::warning, "undeclared identifier 'x'"), r.format());
            // a nested named function body keeps the enclosing mode the same way
            r = make_engine()->check("int k = 1;\n[[warn_mode]] function h() { x = 5; }\nh();");
            check_eq((size_t)1, error_count(r), r.format());
            check_true(has_diag(r, dlevel::error, "undeclared identifier 'x'"), r.format());
            // an undeclared callee in a warn region is callable with an unknown result: no proof
            r = make_engine()->check("int k = 1;\n[[warn_mode]] int v = mystery(1);\nint s = v;");
            check_eq((size_t)0, error_count(r), r.format());
        });

        test("innermost_wins_and_strict_mode_regions_gate", [this]() {
            // strict inside a warn unit re-arms
            std::string what = outcome(*make_engine(), "[[warn_mode]];\nint ok = \"s\";\n[[strict_mode]] int q = \"s\";\nq");
            check_true(what.find("ERROR static check failed") == 0, "got: " + what);
            check_true(what.find("error: cannot initialize 'q'") != std::string::npos, what);
            check_true(what.find("warning: cannot initialize 'ok'") != std::string::npos, what);
            // a statement-level warn region runs into the runtime error; the rest of the unit still gates
            auto e = make_engine();
            std::string free = outcome(*e, "int a = 1;\n[[warn_mode]] int b = \"s\";\nb");
            check_true(free.find("ERROR Cannot initialize") == 0, "got: " + free);
            check_false(e->last_check_diagnostics().has_errors());
            std::string partial = outcome(*e, "int a = \"s\";\n[[warn_mode]] int b = \"s\";\nb");
            check_true(partial.find("ERROR static check failed") == 0, "got: " + partial);
            // jaibite creation gates the same way: a strict region inside a warn unit gates, the warn unit alone loads
            check_throws<static_check_error>([&]() { e->jaibite("[[warn_mode]];\nint ok = 1;\n[[strict_mode]] int q = \"s\";"); });
            auto warned = e->jaibite("[[warn_mode]];\nint q = \"s\";");
            check_true(warned.checked_clean());
            check_true(has_diag(warned.check_diagnostics(), dlevel::warning, "cannot initialize 'q'"));
        });

        test("unknown_attribute_is_a_parse_error", [this]() {
            auto e = make_engine();
            std::string what;
            try {
                e->execute("[[bogus]];\nint x = 1;");
            } catch (const parse_error& ex) {
                what = ex.what();
            }
            check_true(what.find("unknown attribute 'bogus'") != std::string::npos, "got: " + what);
            what.clear();
            try {
                e->execute("int x = 1;\n[[bogus]] x = 2;");
            } catch (const parse_error& ex) {
                what = ex.what();
            }
            check_true(what.find("unknown attribute 'bogus'") != std::string::npos, "got: " + what);
            // [[ in expression position is still a nested array literal
            check_eq((int64_t)2, e->execute("var m = [[1, 2], [3]];\nm[0][1]").as_int());
            check_eq((int64_t)1, e->execute("[[warn_mode]];\nint x = 1;\nx").as_int());
            check_eq((int64_t)1, e->execute("[[strict_mode]]\nint z = 1;\nz").as_int());
        });
        test("typed_slot_table_sync", [this]() {
            // assignable IS the runtime table (detail::convert_for_typed_slot): rows the runtime
            // converts are accepted, rows it rejects are refused up front
            auto e = make_engine();
            check_eq((int64_t)3, e->execute("class K { int to_int() { return 3; } } int v = K(); v").as_int());
            check_eq((size_t)0, e->last_check_diagnostics().diagnostics.size(), "to_int() source is clean under strict");
            std::string refused;
            try { e->execute("string t(string s) { return s; } t(5)"); } catch (const std::exception& ex) { refused = ex.what(); }
            check_true(refused.find("cannot pass 'int' to 's' declared 'string': stringify it") != std::string::npos, "an int into a string parameter is refused up front (got: " + refused + ")");
            check_eq(std::string("yes"), e->execute("class C { int a = 0; } string p(shared_ptr<C> x) { if (x == null) { return \"yes\"; } return \"no\"; } p(null)").as<std::string>());
            check_eq((size_t)0, e->last_check_diagnostics().diagnostics.size(), "null into a shared_ptr parameter is clean");
            std::string text;
            try { e->execute("class D {} int w = D();"); } catch (const static_check_error& ex) { text = ex.what(); }
            check_true(text.find("cannot initialize 'w' declared 'int' with 'D'") != std::string::npos, "no to_int(): still a load error (got: " + text + ")");
            text.clear();
            try { e->execute("class TA { int v = 1; } class TB { TB(TA a) {} } class TC { TC(TB b) {} } TC c = TA();"); } catch (const static_check_error& ex) { text = ex.what(); }
            check_true(text.find("cannot initialize 'c' declared 'TC' with 'TA'") != std::string::npos, "a chained ctor conversion is refused up front (got: " + text + ")");
            text.clear();
            try { e->execute("class C2 { int a = 0; C2(int v) { a = v; } } C2 c2 = true;"); } catch (const static_check_error& ex) { text = ex.what(); }
            check_true(text.find("cannot initialize 'c2' declared 'C2' with 'bool'") != std::string::npos, "a converting ctor's parameter takes only its own base or int<->float (got: " + text + ")");
        });

        // Static field stores: every spelling (C::s, bare s in a method / static method,
        // compound forms, inside functions and lambdas) reports the field-store mismatch;
        // converting stores and initializers are clean like instance fields.
        test("static_store_mismatch_every_spelling", [this]() {
            auto e = make_engine();
            auto r = e->check("class C { static int s = 0;\n  void m() { s = \"x\"; }\n  static void sm() { s = \"y\"; } }");
            check_eq((size_t)2, error_count(r), "bare stores in an instance and a static method");
            check_true(has_diag(r, dlevel::error, "cannot assign 'string' to field 's' declared 'int'"));
            const std::string ok_cls = "class C { static int s = 0;\n  void m() { s = 2; s += 1; }\n  static void sm() { s = true; s++; } }\n";
            check_eq((size_t)0, error_count(e->check(ok_cls)), "well-typed bare stores are clean");
            check_eq((size_t)1, error_count(e->check(ok_cls + "C::s = \"x\";")), "C::s = string");
            check_eq((size_t)1, error_count(e->check(ok_cls + "C::s += \"x\";")), "C::s += string");
            check_eq((size_t)1, error_count(e->check(ok_cls + "function f() { C::s = \"x\"; }")), "inside a function");
            check_eq((size_t)1, error_count(e->check(ok_cls + "auto l = [](){ C::s = \"x\"; };")), "inside a lambda");
            check_true(has_diag(e->check(ok_cls + "C::s = \"x\";"), dlevel::error, "cannot assign 'string' to field 's' declared 'int'"));
            check_eq((size_t)0, error_count(e->check(ok_cls + "C::s = 5; C::s += 1; C::s++; --C::s; C::s = true;")), "well-typed C::s stores are clean");
            auto rf = e->check(ok_cls + "C::s = 2.7; C::s += 0.5;");
            check_eq((size_t)2, error_count(rf), rf.format());
            check_true(has_diag(rf, dlevel::error, "cannot assign 'float' to field 's' declared 'int': cast<int>(x) truncates toward zero, math::ifloor(x) rounds down"), rf.format());
            auto ri = e->check("class C { static int s = \"x\"; }");
            check_eq((size_t)1, error_count(ri));
            check_true(has_diag(ri, dlevel::error, "cannot initialize field 's' declared 'int' with 'string'"));
            check_eq((size_t)0, error_count(e->check("class C { static float f = 3; static int n = true; static int u; }")), "converting static initializers are clean");
        });

        // An instance has no static member (the run's "C has no member 'n'"): a dotted read, store or call of one names C::n
        test("a_static_through_an_instance_names_the_qualified_spelling", [this]() {
            auto e = make_engine();
            const std::string cls = "class C { static int n = 3; static int bump() { n += 1; return n; } int k = 1;\n"
                                    "  int bare() { return n + bump() + k; } }\nclass D : C { }\nC c;\nD d;\n";
            const std::pair<const char*, const char*> through[] = {
                {"int a = c.n;", "'n' is a static member of class 'C': name it C::n"},
                {"c.n = 5;", "'n' is a static member of class 'C': name it C::n"},
                {"int a = c.bump();", "'bump' is a static member of class 'C': name it C::bump(...)"},
                {"int a = d.n;", "'n' is a static member of class 'D': name it D::n"},
                {"int a = C().n;", "'n' is a static member of class 'C': name it C::n"},
                {"class F : C { int f() { return this.n; } }", "'n' is a static member of class 'F': name it F::n"},
            };
            for (const auto& [stmt, text] : through) {
                auto r = e->check(cls + stmt);
                check_eq((size_t)1, error_count(r), std::string(stmt) + "\n" + r.format());
                check_true(has_diag(r, dlevel::error, text), r.format());
            }
            check_eq((size_t)0, error_count(e->check(cls + "int a = C::n + C::bump() + D::n + c.k + c.bare();\n"
                "class G { static int n = 1; int _get_n() { return 2; } }\nG g;\nint b = g.n;")), "the qualified spellings, an instance member and an accessor");
            for (bool use_vm : {false, true}) {
                auto x = make_engine();
                x->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
                check_eq(std::string("C has no member 'n'|5"), x->execute("[[warn_mode]];\n" + cls + "string r = \"\";\ntry { int a = c.n; } catch (err) { r = err; }\n`${r}|${C::bump() + 1}`").to_string(),
                         use_vm ? "vm" : "interp");
            }
        });

        // ------------------------------------------------------------ precision map (docs "Precision map")

        // The creature shape: a script kind over a dynamic_binder base in an imported unit, a
        // subclass in the root. Typos and typed-field mismatches are errors (false-negative
        // guards); binder methods/properties reached through the chain carry no static
        // signature and stay lenient BY DESIGN - the runtime raises.
        test("creature_shape_cross_unit_cpp_base_precision", [this]() {
            auto dir = closure_dir("xunit_creature");
            write_unit(dir, "cr_kind.jai", "class Kind : ProbeObject { int hp = 1; Kind() : super() {} int twice() { return doubled(); } }\n");
            auto e = program_engine(dir);
            dynamic_binder<probe_object>(*e, "ProbeObject")
                .constructor<>()
                .method("doubled", &probe_object::doubled)
                .property("value", &probe_object::value)
                .build();
            const std::string imp = "import \"cr_kind.jai\";\n";
            const std::string sub = "class W : Kind { W() : super() {} ";
            auto typo = e->check(imp + sub + "int tick() { return hpp + this.hpp; } }");
            check_eq((size_t)2, error_count(typo), typo.format());
            check_true(has_diag(typo, dlevel::error, "use of undeclared identifier 'hpp'"), typo.format());
            check_true(has_diag(typo, dlevel::error, "no member 'hpp' in class 'W'"), typo.format());
            auto bare = e->check(imp + sub + "void f() { hp = \"s\"; } }");
            check_eq((size_t)1, error_count(bare), bare.format());
            check_true(has_diag(bare, dlevel::error, "cannot assign 'string' to field 'hp' declared 'int'"), bare.format());
            auto viathis = e->check(imp + sub + "void f() { this.hp = [1]; } }");
            check_eq((size_t)1, error_count(viathis), viathis.format());
            check_true(has_diag(viathis, dlevel::error, "cannot assign 'array' to field 'hp' declared 'int'"), viathis.format());
            auto recv = e->check(imp + sub + "}\nW w = W();\nw.hp = \"s\";\nchar c = w.hp;");
            check_eq((size_t)2, error_count(recv), recv.format());
            const std::string ok = imp + sub + "int tick() { return hp + twice() + doubled(); } }\nW w = W();\nw.value = 4;\nw.hp = 2;\nw.tick() + w.value";
            auto okr = e->check(ok);
            check_eq((size_t)0, error_count(okr), okr.format(ok));
            // lenient by design: binder arity and binder property types
            const std::string arity = imp + sub + "int tick() { return doubled(1) + this.doubled(1, 2); } }\nW w = W();\nw.doubled(1)";
            auto ar = e->check(arity);
            check_eq((size_t)0, error_count(ar), "binder arity through the chain is lenient: " + ar.format(arity));
            const std::string prop = imp + sub + "}\nW w = W();\nw.value = \"s\";\nw.value";
            check_eq((size_t)0, error_count(e->check(prop)), "binder property stores are lenient");
            check_eq((int64_t)22, e->execute(ok).as_int());
            std::string what = outcome(*e, arity);
            check_true(what.find("ERROR") == 0, "the runtime rejects the binder arity (got: " + what + ")");
            std::string pw = outcome(*e, prop);
            check_true(pw.find("ERROR") == 0, "the runtime rejects the binder property store (got: " + pw + ")");
            std::filesystem::remove_all(dir);
        });

        // Accessor-bearing members ("Members"): exactly one declared user getter types the read.
        test("accessor_member_reads_typed_by_single_getter", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            auto e = make_engine();
            const std::string cls =
                "class V { int hp_ = 5; string name_ = \"n\"; int _get_hp() { return hp_; } string _get_name() { return name_; } }\n"
                "V v = V();\n";
            auto r = e->check(cls + "char c = v.hp;");
            check_eq((size_t)1, error_count(r), r.format());
            check_true(has_diag(r, dlevel::error, "cannot initialize 'c' declared 'char' with 'int'"), r.format());
            auto r2 = e->check(cls + "int n = v.name;");
            check_eq((size_t)1, error_count(r2), r2.format());
            check_true(has_diag(r2, dlevel::error, "cannot initialize 'n' declared 'int' with 'string'"), r2.format());
            check_eq((size_t)1, error_count(e->check(cls + "array<int> a = v.hp;")), "array from an int getter");
            check_eq((size_t)1, error_count(e->check(cls + "int n = 0; n = v.name;")), "assignment from a string getter");
            check_eq((size_t)1, error_count(e->check(cls + "int take(int x) { return x; } take(v.name);")), "argument from a string getter");
            check_eq((size_t)1, error_count(e->check(cls + "int f(V q) { return q.name; }")), "return from a string getter");
            check_eq((size_t)0, error_count(e->check(cls + "int a = v.hp; float f = v.hp; string s = v.name; string t = `${v.hp}`; bool b = v.hp; var d = v.name; int l = v.name.size();")), "well-typed reads are clean");
            check_eq((size_t)0, error_count(e->check(cls + "v.name.zzz();")), "a builtin method on the typed result stays lenient");
            std::string what = outcome(*e, "[[warn_mode]]; " + cls + "int n = v.name; n");
            check_true(what.find("ERROR Cannot initialize") == 0, "the runtime keeps its rejection (got: " + what + ")");
            // never a proof source: the plain-field twin proves one more store than the getter-typed read
            auto plain = make_engine();
            check_eq((size_t)0, error_count(plain->check("class F { int hp = 5; } F f = F(); int x = 0; x = f.hp; int y = 0; y = 5; x + y")));
            auto acc = make_engine();
            check_eq((size_t)0, error_count(acc->check("class V { int hp_ = 5; int _get_hp() { return hp_; } } V v = V(); int x = 0; x = v.hp; int y = 0; y = 5; x + y")));
            check_eq(plain->last_proof_stats().stores, acc->last_proof_stats().stores + 1, "the getter-typed store is not proven");
        });

        // ...and exactly one declared user setter checks the store.
        test("accessor_member_stores_checked_by_single_setter", [this]() {
            auto e = make_engine();
            const std::string cls =
                "class S { int hp = 0; int _get_hp() { return hp; } void _set_hp(int v) { hp = v; } }\n"
                "S s = S();\n";
            auto r = e->check(cls + "s.hp = \"s\";");
            check_eq((size_t)1, error_count(r), r.format());
            check_true(has_diag(r, dlevel::error, "cannot assign 'string' to property 'hp' declared 'int'"), r.format());
            check_eq((size_t)1, error_count(e->check(cls + "s.hp = [1];")), "array into an int setter");
            check_eq((size_t)1, error_count(e->check(cls + "s.hp += \"x\";")), "compound: getter int + string is a string into the int setter");
            check_eq((size_t)1, error_count(e->check("class T { int m() { this.hp = \"s\"; return 1; } int hp_ = 0; void _set_hp(int v) { hp_ = v; } }")), "through this");
            check_eq((size_t)1, error_count(e->check(cls + "void f(S q) { q.hp = \"s\"; }")), "through a typed parameter");
            check_eq((size_t)0, error_count(e->check(cls + "s.hp = 7; s.hp = true; s.hp += 1; s.hp++; --s.hp; s.hp -= 2; var y = (s.hp = 3);")), "converting stores are clean");
            auto rf = e->check(cls + "s.hp = 2.5; s.hp -= 2.5;");
            check_eq((size_t)2, error_count(rf), rf.format());
            check_true(has_diag(rf, dlevel::error, "cannot assign 'float' to property 'hp' declared 'int': cast<int>(x) truncates toward zero, math::ifloor(x) rounds down"), rf.format());
            const std::string setter_only = "class T2 { int v_ = 0; void _set_hp(int v) { v_ = v; } }\nT2 t = T2();\n";
            check_eq((size_t)1, error_count(e->check(setter_only + "t.hp = \"s\";")), "setter-only plain store");
            check_eq((size_t)0, error_count(e->check(setter_only + "t.hp += \"s\";")), "setter-only compound store: no getter type, lenient");
            std::string what = outcome(*e, "[[warn_mode]]; " + cls + "s.hp = \"s\"; s.hp");
            check_true(what.find("ERROR Cannot pass string to parameter 'v' of '_set_hp' (declared int)") == 0, "the runtime keeps its rejection (got: " + what + ")");
            // never a proof source after registration either
            const std::string prog = cls + "s.hp = 4; int x = s.hp; x";
            check_eq((int64_t)4, e->execute(prog).as_int());
            check_eq((int64_t)4, e->execute(prog).as_int());
            check_eq((uint32_t)0, e->last_proof_stats().stores, "accessor store: no same_type proof");
        });

        // The rule's boundaries ("Members"): anything but ONE unambiguous declaration is lenient.
        test("accessor_member_ambiguity_stays_lenient", [this]() {
            auto e = make_engine();
            struct row { const char* cls; const char* why; };
            const row lenient_reads[] = {
                {"class A { int _get_hp() { return 1; } int _get_hp(int k) { return k; } }", "two getter overloads"},
                {"class A0 { int _get_hp() { return 1; } } class A : A0 { override string _get_hp() { return \"s\"; } }", "getters at two chain levels"},
                {"class A { var _get_hp() { return 1; } }", "var-returning getter"},
                {"class A { auto _get_hp() { return 1; } }", "auto-returning getter"},
                {"class A { int _get_hp(int k = 0) { return k; } }", "getter with a parameter"},
                {"class A { coroutine int _get_hp() { yield 1; } }", "coroutine getter"},
            };
            for (const auto& rw : lenient_reads) {
                auto r = e->check(std::string(rw.cls) + "\nA a = A();\nchar c = a.hp;\narray<int> xs = a.hp;");
                check_eq((size_t)0, error_count(r), std::string(rw.why) + ": " + r.format());
            }
            const row lenient_stores[] = {
                {"class A { void _set_hp(int v) { } void _set_hp(string v) { } }", "two setter overloads"},
                {"class A0 { void _set_hp(int v) { } } class A : A0 { override void _set_hp(string v) { } }", "setters at two chain levels"},
                {"class A { void _set_hp(var v) { } }", "var setter parameter"},
                {"class A { void _set_hp(v) { } }", "untyped setter parameter"},
                {"class A { void _set_hp(int& v) { } }", "reference setter parameter"},
                {"class A { void _set_hp(int a, int b = 0) { } }", "two-parameter setter"},
            };
            for (const auto& rw : lenient_stores) {
                auto r = e->check(std::string(rw.cls) + "\nA a = A();\na.hp = \"s\";\na.hp = [1];\na.hp += \"x\";");
                check_eq((size_t)0, error_count(r), std::string(rw.why) + ": " + r.format());
            }
            // a registered-only ancestor's accessor has no declaration in view: lenient, still known
            e->execute("class R0 { int hp_ = 1; int _get_hp() { return hp_; } void _set_hp(int v) { hp_ = v; } }");
            auto reg = e->check("class R1 : R0 { }\nR1 r = R1();\nchar c = r.hp;\nr.hp = \"s\";\nint n = r.hp;");
            check_eq((size_t)0, error_count(reg), "registered-only ancestor: " + reg.format());
        });

        // Cross-unit accessors type through the program table exactly like the same-unit case,
        // and stay off every proof lane before and after their unit has run.
        test("accessor_cross_unit_typed_for_checking_never_for_proofs", [this]() {
            auto dir = closure_dir("xunit_accessor");
            write_unit(dir, "ac_a.jai", "class A { int hp = 3; int _get_hp() { return hp; } void _set_hp(int v) { hp = v; } string _get_name() { return \"n\"; } }\n");
            write_unit(dir, "ac_b.jai", "import \"ac_a.jai\";\nint read_hp(A a) { int x = a.hp; a.hp = x + 1; return a.hp; }\n");
            auto bad = program_engine(dir)->check("import \"ac_a.jai\";\nA a = A();\na.hp = \"s\";\nchar c = a.hp;\nint n = a.name;");
            check_eq((size_t)3, error_count(bad), bad.format());
            check_true(has_diag(bad, dlevel::error, "cannot assign 'string' to property 'hp' declared 'int'"), bad.format());
            check_true(has_diag(bad, dlevel::error, "cannot initialize 'c' declared 'char' with 'int'"), bad.format());
            check_true(has_diag(bad, dlevel::error, "cannot initialize 'n' declared 'int' with 'string'"), bad.format());
            check_eq((size_t)0, bad.diagnostics[0].related_line, "cross-unit: no related location in this source");
            const std::string root = "import \"ac_a.jai\";\nimport \"ac_b.jai\";\nread_hp(A())";
            auto e = program_engine(dir);
            check_eq((size_t)0, error_count(e->check(root)));
            check_eq((int64_t)4, e->execute(root).as_int());
            check_eq((int64_t)4, e->execute(root).as_int());
            std::filesystem::remove_all(dir);
        });

        // A [[warn_mode]] unit reports the same accessor mistakes as warnings and still loads.
        test("accessor_mistakes_in_warn_mode_unit_are_warnings", [this]() {
            const std::string body =
                "class S { int hp = 0; int _get_hp() { return hp; } void _set_hp(int v) { hp = v; } string _get_name() { return \"n\"; } }\n"
                "S s = S();\n"
                "int n = s.name;\n"
                "char c = s.hp;\n"
                "s.hp = \"s\";\n"
                "s.hp += \"x\";\n"
                "s.zzz;\n";
            const std::string unit = "[[warn_mode]];\n" + body;
            auto e = make_engine();
            auto r = e->check(unit);
            check_eq((size_t)0, error_count(r), r.format(unit));
            check_eq((size_t)5, warning_count(r), r.format(unit));
            check_true(has_diag(r, dlevel::warning, "cannot initialize 'n' declared 'int' with 'string'"), r.format(unit));
            check_true(has_diag(r, dlevel::warning, "cannot initialize 'c' declared 'char' with 'int'"), r.format(unit));
            check_true(has_diag(r, dlevel::warning, "cannot assign 'string' to property 'hp' declared 'int'"), r.format(unit));
            check_true(has_diag(r, dlevel::warning, "no member 'zzz' in class 'S'"), r.format(unit));
            std::string what = outcome(*e, unit);
            check_true(what.find("ERROR Cannot initialize") == 0, "the strict engine loads the unit; the runtime raises (got: " + what + ")");
            auto strict = e->check(body);
            check_eq((size_t)5, error_count(strict), strict.format(body));
        });

        // A tool's view of the walk (tools/lsp): the observer sees every inferred expression type and
        // changes nothing about the diagnostics.
        test("observer_reports_expression_types", [this]() {
            auto e = jai::foundry::make_engine();
            const std::string src =
                "class P { int hp = 3; }\n"
                "P p = P();\n"
                "auto t = p.hp + 1.5;\n"
                "array<string> names = [\"a\"];\n"
                "auto n = names[0];\n"
                "int bad = \"s\";\n";
            auto types = detail::engine_access::get_registered_template_types(*e);
            lexer lex(src, detail::engine_access::get_symbolizer(*e));
            auto tokens = lex.tokenize();
            parser prs(tokens, detail::engine_access::get_symbolizer(*e), e.get(), types);
            auto parsed = prs.parse();
            check_true(static_cast<bool>(parsed), "fixture parses");
            struct recorder : check_observer {
                std::vector<std::string> names;
                void expression_type(const expression*, const check_expression_type& t) override { names.push_back(t.name); }
            } rec;
            detail::check_options quiet;
            quiet.proofs = detail::check_options::proofs_t::leave;
            detail::check_options observed = quiet;
            observed.observer = &rec;
            auto without = detail::run_static_check(*e, parsed.value(), nullptr, quiet);
            auto with = detail::run_static_check(*e, parsed.value(), nullptr, observed);
            check_eq(without.format(src), with.format(src));
            check_eq((size_t)1, error_count(with), with.format(src));
            auto saw = [&](const std::string& name) { return std::find(rec.names.begin(), rec.names.end(), name) != rec.names.end(); };
            check_true(saw("P"), "the constructor call types as P");
            check_true(saw("float"), "p.hp + 1.5 types as float");
            check_true(saw("array<string>"), "the typed global keeps its element type");
            check_true(saw("string"), "names[0] types as string");
        });

        test("parser_keeps_declarations_around_errors", [this]() {
            auto e = jai::foundry::make_engine();
            const std::string src = "int a = 1;\nint broken( {\nint b = 2;\n";
            auto types = detail::engine_access::get_registered_template_types(*e);
            lexer lex(src, detail::engine_access::get_symbolizer(*e));
            auto tokens = lex.tokenize();
            parser prs(tokens, detail::engine_access::get_symbolizer(*e), e.get(), types);
            auto parsed = prs.parse();
            check_false(static_cast<bool>(parsed), "the fixture does not parse");
            const auto& kept = prs.recovered_declarations();
            std::vector<std::string> names;
            for (const auto& d : kept) {
                if (d && d->get_type() == node_type::variable_decl) { names.emplace_back(static_cast<const variable_decl*>(d.get())->name); }
            }
            check_eq((size_t)2, names.size());
            check_true(names.size() == 2 && names[0] == "a" && names[1] == "b", "the declarations on both sides of the error are kept");
        });

        // ------------------------------------------------------------ members through references
        // A reference's referent type is as known as a value's: field names, field stores, method names and arity are checked
        // through T&, const T&, auto& of a known class, range-for variables and the fields reached through them, with the texts
        // a value receiver draws

        test("members_through_references_are_checked_like_values", [this]() {
            auto e = make_engine();
            const std::string cls = "class W { int n = 0; void put(int v) {} }\n";
            const std::vector<std::pair<std::string, std::string>> cases = {
                {"void f(W& w) { w.n = \"ring\"; }", "cannot assign 'string' to field 'n' declared 'int'"},
                {"void f(W& w) { w.putt(1); }", "no member 'putt' in class 'W'"},
                {"void f(W& w) { w.put(1, 2); }", "'put' expects 1 argument(s), got 2"},
                {"void f(W& w) { w.put(\"x\"); }", "cannot pass 'string' to 'v' declared 'int'"},
                {"void f(W& w) { var g = w.put; }", "'put' is a method of W"},
                {"void f(const W& w) { print(\"{}\", w.nme); }", "no member 'nme' in class 'W'"},
                {"void f(const W& w) { w.put(); }", "'put' expects 1 argument(s), got 0"},
                {"void f(const W& w) { w.n = 4; }", "cannot assign to 'w.n': 'w' is const"},
                {"W w = W();\nW& r = w;\nr.putt(1);", "no member 'putt' in class 'W'"},
                {"W w = W();\nauto& r = w;\nr.nme = 1;", "no member 'nme' in class 'W'"},
                {"W w = W();\nW& r = w;\nauto& q = r;\nq.n = \"s\";", "cannot assign 'string' to field 'n' declared 'int'"},
                {"array<W> ws = [W()];\nfor (auto& c : ws) { c.nme = 1; }", "no member 'nme' in class 'W'"},
                {"array<W> ws = [W()];\nfor (W& c : ws) { c.put(); }", "'put' expects 1 argument(s), got 0"},
                {"array<W> ws = [W()];\nfor (auto c : ws) { c.nme = 1; }", "no member 'nme' in class 'W'"},
                {"void f(array<W>& ws) { for (auto& c : ws) { c.n = \"s\"; } }", "cannot assign 'string' to field 'n' declared 'int'"},
                {"class H { W w = W(); array<W> ws = []; map<string, W> named = {}; }\nvoid f(H& h) { h.w.putt(); }", "no member 'putt' in class 'W'"},
                {"class H { W w = W(); array<W> ws = []; map<string, W> named = {}; }\nvoid f(H& h) { h.ws[0].n = \"s\"; }", "cannot assign 'string' to field 'n' declared 'int'"},
                {"class H { W w = W(); array<W> ws = []; map<string, W> named = {}; }\nvoid f(H& h) { h.named[\"a\"].nme = 1; }", "no member 'nme' in class 'W'"},
                {"void f(W& w) { auto g = [w]() { return w.nme; }; }", "no member 'nme' in class 'W'"},
            };
            for (const auto& [body, text] : cases) {
                const std::string src = cls + body;
                auto r = e->check(src);
                check_eq((size_t)1, error_count(r), r.format(src));
                check_true(has_diag(r, dlevel::error, text), r.format(src));
            }
            // the value receiver's text, byte for byte
            auto by_value = e->check(cls + "W w = W();\nw.n = \"ring\";\nw.putt(1);");
            auto by_ref = e->check(cls + "W w = W();\nW& r = w;\nr.n = \"ring\";\nr.putt(1);");
            check_eq((size_t)2, by_value.diagnostics.size(), by_value.format());
            check_eq((size_t)2, by_ref.diagnostics.size(), by_ref.format());
            for (size_t i = 0; i < 2 && i < by_ref.diagnostics.size() && i < by_value.diagnostics.size(); ++i) {
                check_eq(by_value.diagnostics[i].message, by_ref.diagnostics[i].message);
            }
        });

        test("members_through_references_stay_clean_where_values_do", [this]() {
            auto e = make_engine();
            const std::string src = R"(
                class Unit { int hp = 10; void hit(int d) { hp -= d; } }
                class Archer : Unit { int range = 6; }
                class Holder { Unit u = Unit(); array<Unit> squad = [Unit()]; map<string, Unit> named = {"a": Unit()}; }
                int run(Unit& u, const Unit& cu, var& anything, Holder& h) {
                    u.hp = 3; u.hit(1);
                    int total = cu.hp;
                    cast<Archer&>(u).range = 9;
                    h.u.hp = 2; h.u.hit(1);
                    for (auto& m : h.squad) { m.hit(1); }
                    Unit& first = h.squad[0]; first.hit(1);
                    h.named["a"].hit(2);
                    auto& alias = u; alias.hit(2);
                    auto& deeper = h.u; deeper.hit(1);
                    var loose = u;
                    auto f = [&u]() { u.hit(1); };
                    f();
                    return total + u.hp + h.u.hp + first.hp + h.named["a"].hp + cast<Archer&>(u).range + loose.range;
                }
                Archer a = Archer();
                Holder h = Holder();
                var any = 0;
                run(a, a, any, h)
            )";
            auto r = e->check(src);
            check_eq((size_t)0, r.diagnostics.size(), r.format(src));
            check_eq((int64_t)35, e->execute(src).as_int());
            // a var& and an auto& over a var reach anything: the run time decides
            auto open = e->check("class W { int n = 0; }\nvar v = W();\nvar& r = v;\nr.zz = 1;\nauto& q = v;\nq.yy();\nvoid f(var& x) { x.ww = 2; }");
            check_eq((size_t)0, error_count(open), open.format());
        });

        test("subclass_members_through_a_base_reference_are_errors_like_a_base_slot", [this]() {
            auto e = make_engine();
            const std::string cls = "class Unit { int hp = 10; }\nclass Archer : Unit { int range = 6; }\n";
            auto slot = e->check(cls + "Unit u = Archer();\nprint(\"{}\", u.range);");
            auto ref = e->check(cls + "Archer a = Archer();\nUnit& u = a;\nprint(\"{}\", u.range);");
            auto param = e->check(cls + "int f(Unit& u) { return u.range; }");
            for (const auto* r : {&slot, &ref, &param}) {
                check_eq((size_t)1, error_count(*r), r->format());
                check_true(has_diag(*r, dlevel::error, "no member 'range' in class 'Unit'"), r->format());
            }
            const std::string fixed = cls + "Archer a = Archer();\nUnit& u = a;\nprint(\"{}\", cast<Archer&>(u).range);";
            check_eq((size_t)0, error_count(e->check(fixed)));
        });

        test("reference_member_diagnostics_seed_no_proofs", [this]() {
            // the referent class serves diagnostics only: the proof pass stamps what it stamped before
            auto e = make_engine();
            const std::string src = "class W { int n = 0; void put(int v) { n = v; } }\n"
                                    "void f(W& w) { w.n = 3; w.put(4); }\nW w = W();\nf(w);\nW w2 = W();\nw.n + w2.n";
            e->execute(src);
            check_eq((int64_t)4, e->execute(src).as_int());
            auto plain = make_engine();
            const std::string open = "class W { int n = 0; void put(int v) { n = v; } }\n"
                                     "void f(var& w) { w.n = 3; w.put(4); }\nW w = W();\nf(w);\nW w2 = W();\nw.n + w2.n";
            plain->execute(open);
            check_eq((int64_t)4, plain->execute(open).as_int());
            check_eq(plain->last_proof_stats().stores, e->last_proof_stats().stores, "a T& receiver proves what a var& receiver does");
            check_eq(plain->last_proof_stats().args, e->last_proof_stats().args);
        });

        // ------------------------------------------------------------ comparisons with no comparison
        test("comparing_types_with_no_comparison_is_an_error", [this]() {
            auto e = make_engine();
            const std::string decls = "class C { int v = 1; }\nint i = 1; string s = \"s\"; bool b = true; char c = 'a'; array<int> a = [1]; "
                                      "map<string, int> m = {}; C oc = C();\n";
            const std::vector<std::pair<std::string, std::string>> cases = {
                {"i == s", "cannot compare 'int' with 'string' using '==': the types have no comparison (it is always false); convert one operand"},
                {"s != i", "cannot compare 'string' with 'int' using '!=': the types have no comparison (it is always true); convert one operand"},
                {"i < s", "cannot compare 'int' with 'string' using '<': the types have no comparison (it raises 'Invalid operands for < operator'); convert one operand"},
                {"s <=> 1", "using '<=>'"},
                {"b == 1", "cannot compare 'bool' with 'int' using '=='"},
                {"c == true", "cannot compare 'char' with 'bool' using '=='"},
                {"a == m", "cannot compare 'array<int>' with 'map<string,int>' using '=='"},
                {"a >= i", "using '>='"},
                {"oc == 1", "cannot compare 'C' with 'int' using '==': class 'C' declares no comparison operator (it is always false); declare the operator in the class or convert the operand"},
                {"2 < oc", "cannot compare 'int' with 'C' using '<': class 'C' declares no comparison operator"},
                {"oc == null", "cannot compare 'C' with null using '==': 'C' is never null (it is always false); declare the slot shared_ptr<C> (or var) to hold null"},
                {"i == null", "cannot compare 'int' with null using '==': 'int' is never null (it is always false); compare with a value of its type, or declare the slot var to hold null"},
                {"null != s", "cannot compare 'string' with null using '!=': 'string' is never null (it is always true)"},
                {"b == null", "cannot compare 'bool' with null using '=='"},
                {"a == null", "'array<int>' is never null"},
                {"m != null", "'map<string,int>' is never null"},
                {"c < null", "cannot compare 'char' with null using '<': 'char' is never null (it raises 'Invalid operands for < operator')"},
            };
            for (const auto& [expr, text] : cases) {
                const std::string src = decls + "bool r = " + expr + ";";
                auto rep = e->check(src);
                check_eq((size_t)1, error_count(rep), rep.format(src));
                check_true(has_diag(rep, dlevel::error, text), rep.format(src));
            }
            // a warn region reports it as a warning and runs, with the run-time result unchanged
            auto w = make_engine();
            check_eq(false, w->execute("[[warn_mode]]; int i = 1; string s = \"s\"; i == s").as<bool>());
            check_true(has_diag(w->last_check_diagnostics(), dlevel::warning, "cannot compare 'int' with 'string'"));
            // var operands keep the run-time verdict
            check_eq(false, e->execute("var v = 1; var t = \"s\"; v == t").as<bool>());
            // an auto takes its initializer's type, then holds to it like a declaration
            check_true(has_diag(e->check("auto k = 5;\nbool r = k == null;"), dlevel::error, "cannot compare 'int' with null using '=='"));
            // what can hold null compares with it: handles, functions, var, and a call whose container result falls off its body
            const std::string nullable = "class C { int v = 1; }\nC oc = C(); shared_ptr<C> h = new C(); weak_ptr<C> w = h; function fn = null; var v = 1;\n"
                                         "array<int> maybe(int k) { if (k > 0) { return [k]; } }\n"
                                         "[h != null, w == null, fn == null, v == null, maybe(0) == null, null == null].size()";
            auto quiet = e->check(nullable);
            check_eq((size_t)0, error_count(quiet), quiet.format(nullable));
            check_eq((int64_t)6, e->execute(nullable).as_int());
        });

        // ------------------------------------------------------------ arithmetic and bitwise with no row
        test("arithmetic_and_bitwise_no_row_takes_are_errors", [this]() {
            auto e = make_engine();
            const std::string decls = "class C { int v = 1; }\nclass M { int v = 2; M operator*(int x) { M r; r.v = v * x; return r; } }\nclass K : M { }\n"
                                      "int i = 1; float f = 1.5; char c = 'a'; string s = \"s\"; bool b = true; array<int> a = [1]; "
                                      "map<string, int> m = {}; C oc = C(); M om = M(); K ok = K();\n";
            const std::vector<std::pair<std::string, std::string>> binary = {
                {"oc * 2", "cannot apply '*' to 'C' and 'int': class 'C' declares no operator '*' (it raises 'Invalid operands for * operator'); declare the operator in the class or convert the operand"},
                {"s * 3", "cannot apply '*' to 'string' and 'int' (it raises 'Invalid operands for * operator'); convert one operand"},
                {"i + b", "cannot apply '+' to 'int' and 'bool' (it raises 'Invalid operands for + operator')"},
                {"a - a", "cannot apply '-' to 'array<int>' and 'array<int>'"},
                {"m / i", "(it raises 'Invalid operands for / operator')"},
                {"s - s", "cannot apply '-' to 'string' and 'string'"},
                {"2 * om", "cannot apply '*' to 'int' and 'M' (it raises 'Invalid operands for * operator'); a class's operator runs only with an instance of it on the left"},
                {"om + 1", "class 'M' declares no operator '+'"},
                {"2 - oc", "cannot apply '-' to 'int' and 'C' (it raises 'Invalid operands for - operator'); convert one operand"},
                {"ok - 1", "class 'K' declares no operator '-'"},
                {"i & f", "cannot apply '&' to 'int' and 'float' (it raises 'Bitwise & requires integer operands'); convert one operand"},
                {"b << 1", "(it raises 'Left shift requires integer operands')"},
                {"s >> 1", "(it raises 'Right shift requires integer operands')"},
                {"oc ^ 1", "class 'C' declares no operator '^' (it raises 'Bitwise ^ requires integer operands')"},
            };
            for (const auto& [expr, text] : binary) {
                const std::string src = decls + "var r = " + expr + ";";
                auto rep = e->check(src);
                check_eq((size_t)1, error_count(rep), rep.format(src));
                check_true(has_diag(rep, dlevel::error, text), rep.format(src));
            }
            const std::vector<std::pair<std::string, std::string>> stores = {
                {"oc *= 2;", "cannot apply '*=' to 'C' and 'int': class 'C' declares no operator '*'; declare the operator in the class or convert the operand"},
                {"i *= s;", "cannot apply '*=' to 'int' and 'string'"},
                {"s -= \"a\";", "cannot apply '-=' to 'string' and 'string'"},
                {"i += b;", "cannot apply '+=' to 'int' and 'bool'"},
                {"oc &= 1;", "cannot apply '&=' to 'C' and 'int': class 'C' declares no operator '&'"},
                {"b++;", "cannot apply '++' to 'bool'"},
                {"void g() { oc--; }", "cannot apply '--' to 'C'"},
                {"int k = om++;", "cannot apply '++' to 'M'"},
                {"c++;", "cannot assign 'int' to 'c' declared 'char'"},
                {"void h(char p) { --p; }", "cannot assign 'int' to 'p' declared 'char'"},
                {"class W { char ch = 'a'; void bump() { ch++; } }", "cannot assign 'int' to field 'ch' declared 'char'"},
            };
            for (const auto& [stmt, text] : stores) {
                const std::string src = decls + stmt;
                auto rep = e->check(src);
                check_eq((size_t)1, error_count(rep), rep.format(src));
                check_true(has_diag(rep, dlevel::error, text), rep.format(src));
            }
            const std::string fine = decls + "var r = [i + f, c * 2, i % f, s + s, s + c, oc + s, om * 2, ok * 2, i & c, c << 2, math::vec2(1, 2) * 2];\n"
                                             "i *= 2; f += i; f++; i--; om *= 2; var v = s; var w = b;\nr.size()";
            auto quiet = e->check(fine);
            check_eq((size_t)0, error_count(quiet), quiet.format(fine));
            check_eq((int64_t)11, e->execute(fine).as_int());
            // var operands keep the run-time verdict, and a warn region reports it as a warning with the run-time error unchanged
            check_eq((size_t)0, error_count(e->check("var v = \"s\"; var r = v * 2;")));
            auto w = make_engine();
            check_eq(std::string("Invalid operands for * operator"),
                     w->execute("[[warn_mode]]; string s = \"s\"; var c = \"\"; try { var r = s * 2; } catch (e) { c = e; } c").to_string());
            check_true(has_diag(w->last_check_diagnostics(), dlevel::warning, "cannot apply '*' to 'string' and 'int'"));
        });

        // [[nodiscard]] (guide/06-functions.html#nodiscard): an expression statement that drops the result of a call the checker
        // resolves to a [[nodiscard]] callee fails the load; the library's copying twins of the in-place methods are marked
        test("a_discarded_nodiscard_result_fails_the_load", [this]() {
            auto e = make_engine();
            const std::string decls = "[[nodiscard]] int add(int a, int b) { return a + b; }\n"
                                      "class W { int g = 1; [[nodiscard]] bool spend(int n) { g -= n; return true; } [[nodiscard]] static int make() { return 2; } }\n"
                                      "namespace ns { [[nodiscard]] int pick() { return 3; } }\n"
                                      "[[nodiscard]] int over(int a) { return a; }\nint over(int a, int b) { return 0; }\n"
                                      "[[nodiscard]] coroutine int gen() { yield 1; }\n"
                                      "W w = W();\nbool flag = true;\n";
            const std::pair<const char*, const char*> dropped[] = {
                {"add(1, 2);", "the result of 'add' is discarded, but 'add' is [[nodiscard]]: use it, or keep it in the placeholder (auto _ = add(...);)"},
                {"w.spend(1);", "the result of 'W.spend' is discarded, but 'W.spend' is [[nodiscard]]: use it, or keep it in the placeholder (auto _ = w.spend(...);)"},
                {"W::make();", "the result of 'W.make' is discarded"},
                {"ns::pick();", "the result of 'ns::pick' is discarded"},
                {"over(1);", "the result of 'over' is discarded"},
                {"gen();", "the result of 'gen' is discarded"},
                {"flag ? add(1, 2) : 0;", "the result of 'add' is discarded"},
                {"void f() { add(1, 2); }", "the result of 'add' is discarded"},
                {"auto l = [&]() { w.spend(1); };", "the result of 'W.spend' is discarded"},
            };
            for (const auto& [stmt, text] : dropped) {
                auto r = e->check(decls + stmt + "\nprint(1);");
                check_eq((size_t)1, error_count(r), r.format());
                check_true(has_diag(r, dlevel::error, text), r.format());
            }
            const char* kept[] = {"auto k = add(1, 2);", "auto _ = add(1, 2);\nauto _ = w.spend(1);", "void g() { auto _ = add(1, 2); auto _ = ns::pick(); }", "print(add(1, 2));", "if (w.spend(1)) { }", "over(1, 2);", "auto h = gen();",
                                  "int sum = add(1, 2) + ns::pick();", "function f = add; f(1, 2);", "var d = w; d.spend(1);"};
            for (const char* stmt : kept) {
                auto r = e->check(decls + stmt + "\nprint(1);");
                check_eq((size_t)0, error_count(r), std::string(stmt) + "\n" + r.format());
            }
            check_eq((size_t)0, error_count(e->check(decls + "add(1, 2)")), "the program's last expression is its result");
            auto w = e->check("[[warn_mode]]; " + decls + "add(1, 2);\nprint(1);");
            check_eq((size_t)0, error_count(w), w.format());
            check_true(has_diag(w, dlevel::warning, "the result of 'add' is discarded"), w.format());

            const std::pair<const char*, const char*> twins[] = {
                {"string name = \"ab\";\nstring::to_upper(name);", "the result of 'string::to_upper' is discarded: it returns a new value and leaves its argument unchanged, so the statement does nothing; keep the result, or call the in-place method (name.to_upper(...))"},
                {"string s = \" a \";\nstring::trim(s);", "(s.trim(...))"},
                {"string s = \"a\";\nstring::append(s, \"b\");", "(s.append(...))"},
                {"array<int> xs = [2, 1];\narray::sort(xs);", "(xs.sort(...))"},
                {"array<int> xs = [2, 1];\narray::filter(xs, [](int x) { return x > 1; });", "(xs.filter(...))"},
                {"array<int> xs = [2, 1];\narray::map(xs, [](int x) { return x; });", "(xs.map(...))"},
                {"map<string, int> m = {\"a\": 1};\nmap::filter(m, [](k, v) { return v > 0; });", "(m.filter(...))"},
            };
            jai::stdlib::register_all(*e);
            for (const auto& [src, text] : twins) {
                auto r = e->check(std::string(src) + "\nprint(1);");
                check_eq((size_t)1, error_count(r), std::string(src) + "\n" + r.format());
                check_true(has_diag(r, dlevel::error, text), r.format());
            }
            check_eq((size_t)0, error_count(e->check("string n = \"a\";\nprint(string::to_upper(n));\nstring::join([\"a\"], \",\");")));

            auto host = make_engine();
            host->add_function("try_open", [](script_int n) -> bool { return n > 0; }, jai::nodiscard);
            host->add_function("plain_open", [](script_int n) -> bool { return n > 0; });
            host->add_function("try_read", [](const std::string& path) -> bool { return !path.empty(); }, jai::nodiscard, jai::permissions::filesystem);
            host->add_variadic_function("try_any", [h = host.get()](const std::vector<script_value>& args) -> script_value { return h->make_value(!args.empty()); }, jai::nodiscard);
            dynamic_binder<probe_object>(*host, "ProbeObject").constructor<>()
                .method("doubled", &probe_object::doubled, jai::nodiscard)
                .method("tripled", [](probe_object& self) { return self.doubled() + 1; }, jai::nodiscard)
                .static_method("made", []() -> script_int { return 4; }, jai::nodiscard)
                .build();
            check_true(host->is_nodiscard_function("try_open") && !host->is_nodiscard_function("plain_open"));
            check_true(host->is_nodiscard_function("try_read") && host->is_nodiscard_function("try_any"));
            check_true(host->is_nodiscard_function("ProbeObject.doubled") && host->is_nodiscard_function("ProbeObject.tripled"));
            auto hr = host->check("try_open(1);\nplain_open(1);\ntry_read(\"a\");\ntry_any(1);\nProbeObject p = ProbeObject();\np.doubled();\np.tripled();\nProbeObject::made();\nprint(1);");
            check_eq((size_t)6, error_count(hr), hr.format());
            check_true(has_diag(hr, dlevel::error, "the result of 'ProbeObject.made' is discarded"), hr.format());
            check_true(has_diag(hr, dlevel::error, "the result of 'try_read' is discarded"), hr.format());
            check_true(has_diag(hr, dlevel::error, "the result of 'ProbeObject.tripled' is discarded"), hr.format());
            check_true(has_diag(hr, dlevel::error, "the result of 'try_open' is discarded, but 'try_open' is [[nodiscard]]"), hr.format());
            check_true(has_diag(hr, dlevel::error, "the result of 'ProbeObject.doubled' is discarded"), hr.format());

            const std::pair<const char*, const char*> misplaced[] = {
                {"[[nodiscard]] int x = 1;", "[[nodiscard]] applies to a function or a method"},
                {"class C { [[nodiscard]] int f = 1; }", "[[nodiscard]] applies to a function or a method"},
                {"class C { [[nodiscard]] C() { } }", "[[nodiscard]] applies to a function or a method"},
                {"[[nodiscard]] void v() { }", "[[nodiscard]] function 'v' returns void: there is no result to keep"},
                {"namespace ns { [[nodiscard]] int x = 1; }", "[[nodiscard]] applies to a function or a method"},
            };
            for (const auto& [src, text] : misplaced) {
                for (bool use_vm : {false, true}) {
                    auto p = make_engine();
                    p->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
                    std::string got;
                    try { p->execute(src); } catch (const std::exception& ex) { got = ex.what(); }
                    check_true(got.find(text) != std::string::npos, std::string(src) + " -> " + got);
                }
            }
        });

        // A lost write (guide/03-value-semantics.html#lost-writes): a store into a by-value copy that no path reads again, and that
        // nothing else can observe, fails the load; every way the write could be seen keeps the program legal
        test("a_lost_write_to_a_copy_fails_the_load", [this]() {
            auto e = make_engine();
            const std::string decls = "class E { int hp = 10; string name = \"e\"; array<int> marks = []; }\n"
                                      "array<E> es = [E(), E()];\narray<string> names = [\"a\"];\narray<int> xs = [1, 2];\nint total = 0;\n";
            const std::pair<const char*, const char*> lost[] = {
                {"for (auto e : es) e.hp -= 1;", "'e.hp' writes to 'e', a copy of the element that is never read afterwards, so the write is lost; iterate with 'auto& e' to change the element"},
                {"for (E e : es) { e.hp = 0; }", "iterate with 'E& e' to change the element"},
                {"for (auto s : names) { s.to_upper(); }", "'s.to_upper()' writes to 's'"},
                {"for (auto x : xs) { x *= 2; }", "'x' writes to 'x'"},
                {"for (auto e : es) { e.marks.push(1); }", "'e.marks.push(...)' writes to 'e'"},
                {"for (auto e : es) { e.hp -= 1; total += 1; }", "'e.hp' writes to 'e'"},
                {"void hurt(map<string, int> m) { m[\"a\"] = 5; }", "'m[...]' writes to parameter 'm', a copy of the argument that is never read afterwards, so the write is lost; take 'map<string,int>& m' to change the caller's value"},
                {"void shout(string s) { s.to_upper(); }", "take 'string& s' to change the caller's value"},
                {"auto l = [](int n) { n += 1; };", "writes to parameter 'n'"},
                {"void f() { E first = es[0]; first.hp = 1; }", "'first.hp' writes to 'first', a copy that is never read afterwards, so the write is lost; declare 'E& first' to change the original"},
                {"void f() { auto k = es[0].hp; k++; }", "declare 'auto& k' to change the original"},
                {"void f() { auto first = es[0]; first.hp = 1; }", "'first.hp' writes to 'first', a copy that is never read afterwards, so the write is lost; declare 'auto& first' to change the original"},
                {"map<string, E> named = {\"a\": E()};\nvoid f() { auto a = named[\"a\"]; a.hp -= 1; }", "declare 'auto& a' to change the original"},
                {"class W { E boss; array<E> crew = [E()]; }\nW w;\nvoid f() { auto b = w.boss; b.hp = 1; }", "declare 'auto& b' to change the original"},
                {"class W { E boss; array<E> crew = [E()]; }\nW w;\nvoid f() { auto c = w.crew[0]; c.name = \"x\"; }", "'c.name' writes to 'c'"},
                {"int f(int n) { int r = 0; if (n > 0) { r = n; } else { r = 1; return r; } return 0; }", "'r' writes to 'r'"},
                {"void f() { int r = 0; try { r = 5; } catch (err) { print(r); } }", "'r' writes to 'r'"},
                {"void f() { auto nums = [1, 2, 3]; for (auto x : nums) { x *= 2; } print(nums); }", "'x' writes to 'x'"},   // the literal types the auto array
                {"auto tags = [\"a\", \"b\"];\nfor (auto t : tags) { t.to_upper(); }", "'t.to_upper()' writes to 't'"},
            };
            for (const auto& [stmt, text] : lost) {
                auto r = e->check(decls + stmt + "\nprint(total);");
                check_eq((size_t)1, error_count(r), std::string(stmt) + "\n" + r.format());
                check_true(has_diag(r, dlevel::error, text), r.format());
            }
            const char* legal[] = {
                "for (auto& e : es) { e.hp -= 1; }",
                "for (auto e : es) { e.hp -= 1; print(e.hp); }",
                "for (auto e : es) { auto c = e; c.hp -= 3; print(c.hp); }",
                "void log_hit() {}\nfor (auto e : es) { e.hp -= 1; log_hit(); }",       // a call may observe it (reflect::instances)
                "void f(int n) { auto vals = [n, 2]; for (auto x : vals) { x *= 2; } }",   // only a literal's elements type an auto array here
                "auto mixed = [1, \"a\"];\nfor (auto x : mixed) { x = 2; }",
                "for (auto e : es) { e.hp -= 1; E other = E(); }",                    // a construction runs code
                "for (auto x : xs) { x *= 2; total += x; }",
                "int f(int n) { while (n > 0) { n--; } return n; }",
                "int f(int n) { for (int i = 0; i < 3; i++) { n += i; } return n; }",
                "int f(int n) { int r = 0; for (auto x : xs) { if (x > n) { r = x; break; } } return r; }",
                "int f() { int r = 0; try { r = 5; throw \"x\"; } catch (err) { return r; } return 0; }",
                "void f(int n) { auto& r = n; n = 3; print(r); }",                     // a reference reads it later
                "void f(array<int> a) { for (auto& x : a) { x = 1; } }",               // writes through a reference: not judged
                "void f(int n) { auto g = [&]() { return n; }; n = 4; print(g()); }",  // a lambda keeps it
                "void f(int n) { auto g = [n]() { return n; }; n = 4; print(g()); }",
                "void f(var v) { v = 5; }",                                            // var: not judged
                "void f(E e) { e.hp = 5; }",                                           // a by-value class parameter may share a handle's object
                "void f(shared_ptr<E> p) { p.hp = 5; }",
                "for (auto h : [new E()]) { h.hp = 1; }",                              // handles share their object
                "void f() { auto h = new E(); auto g = h; g.hp = 1; }",
                "array<shared_ptr<E>> hs = [new E()];\nfor (E e : hs) { e.hp = 0; }",   // a class-typed variable over handles shares
                "void bump(int& x) { x += 1; } void f(int n) { bump(n); n = 2; bump(n); }",
                "int f(int n) { switch (n) { case 1: n = 5; fallthrough; default: return n; } return 0; }",
                "for (auto e : es) { e.hp -= 1; if (e.hp > 0) { continue; } }",
                "void f(int n) { n = 1; include(\"no_such_file.jai\"); }",            // an include may name it
                "void f(array<int> a) { try { a[5] = 1; } catch (err) { print(err); } }",   // an element store may raise
                "void f(array<uint8> b, int v) { b.push(v); }",                       // a narrow element range-checks
                "void f(map<int, int> m, var k) { try { m[k] = 1; } catch (err) { print(err); } }",   // the key converts
                "void f(string s) { try { s.repeat(-1); } catch (err) { print(err); } }",
            };
            for (const char* stmt : legal) {
                auto r = e->check(decls + stmt + "\nprint(total);");
                bool lost_write = false;
                for (const auto& d : r.diagnostics) { if (d.message.find("so the write is lost") != std::string::npos) { lost_write = true; } }
                check_true(!lost_write, std::string(stmt) + "\n" + r.format());
            }
            // the engine's own print and format over plain values run no script, so they observe no copy; an instance's text may
            {
                auto s = make_engine();
                jai::stdlib::register_all(*s);
                auto hit = s->check(decls + "for (auto e : es) { e.hp -= 1; print(\"hit {}\", total); }");
                check_true(has_diag(hit, dlevel::error, "'e.hp' writes to 'e'"), hit.format());
                for (const char* stmt : {"for (auto e : es) { e.hp -= 1; print(e); }", "for (auto e : es) { e.hp -= 1; print(\"hp {}\", e.hp); }",
                                         "void print(string s) {}\nfor (auto e : es) { e.hp -= 1; print(\"hit\"); }"}) {
                    auto r = s->check(decls + stmt);
                    check_eq((size_t)0, error_count(r), std::string(stmt) + "\n" + r.format());
                }
            }
            // a change to a container or text a call returned by value dies with the copy; an element store into one raises
            const std::pair<const char*, const char*> through_results[] = {
                {"array<int> list() { return xs; }\nlist().push(4);", "push changes a call result in place and nothing reads it: 'list' returns by value"},
                {"string label() { return \"a\"; }\nlabel().to_upper();", "to_upper changes a call result in place and nothing reads it: 'label' returns by value"},
                {"class B { array<int> v = [1]; array<int> list() { return v; } }\nB b;\nb.list().sort();", "sort changes a call result in place and nothing reads it: 'b.list' returns by value"},
                {"class B { array<int> v = [1]; array<int> list() { return v; } }\nB b;\nb.list()[0] = 7;", "cannot assign to a call result: 'b.list' returns by value"},
                {"map<string, int> table() { return {\"a\": 1}; }\ntable()[\"a\"] += 1;", "cannot assign to a call result: 'table' returns by value"},
            };
            for (const auto& [stmt, text] : through_results) {
                auto r = e->check(decls + stmt);
                check_eq((size_t)1, error_count(r), std::string(stmt) + "\n" + r.format());
                check_true(has_diag(r, dlevel::error, text), r.format());
            }
            for (const char* stmt : {
                "array<int>& list() { return xs; }\nlist().push(4);\nlist()[0] = 7;",    // a reference result names storage
                "var list() { return xs; }\nlist().push(4);",                            // an undeclared result is the run's
                "array<int> list() { return xs; }\nint n = list().size();\nauto copy = list();\ncopy.push(4);\nprint(copy);",
                "array<int> list() { return xs; }\ntry { list().pop(); } catch (err) { print(err); }",   // a change that may raise
                "E first() { return es[0]; }\nfirst().hp = 5;",                          // a class result may share its object: the run decides
            }) {
                auto r = e->check(decls + stmt);
                check_eq((size_t)0, error_count(r), std::string(stmt) + "\n" + r.format());
            }
            // a class that runs code of its own when written or destroyed, or has a subclass that does, is not judged
            const char* observing[] = {
                "class D { int hp = 1; ~D() { print(hp); } }",
                "class D { int v = 1; int _get_hp() { return v; } void _set_hp(int x) { v = x; print(x); } }",
                "class D { int hp = 1; } class Boss : D { ~Boss() { print(hp); } }",
                "class D { int hp = 1; var held; }",
            };
            for (const char* cls : observing) {
                auto r = e->check(std::string(cls) + "\narray<D> ds = [D()];\nfor (auto d : ds) { d.hp = 5; }");
                bool lost_write = false;
                for (const auto& d : r.diagnostics) { if (d.message.find("so the write is lost") != std::string::npos) { lost_write = true; } }
                check_true(!lost_write, std::string(cls) + "\n" + r.format());
            }
            // the observable cases run identically on both backends: the destructor sees the copy's write
            for (bool use_vm : {false, true}) {
                auto x = make_engine();
                x->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
                check_eq(std::string("5|1"), x->execute("string log = \"\";\nclass D { int hp = 1; ~D() { if (log == \"\") { log = `${hp}`; } } }\n"
                    "array<D> ds = [D()];\nfor (auto d : ds) { d.hp = 5; }\n`${log}|${ds[0].hp}`").to_string(), use_vm ? "vm" : "interp");
            }
            auto w = e->check("[[warn_mode]];\n" + decls + "for (auto e : es) { e.hp -= 1; }");
            check_eq((size_t)0, error_count(w), w.format());
            check_true(has_diag(w, dlevel::warning, "so the write is lost"), w.format());
            auto region = e->check(decls + "for (auto e : es) {\n\t[[warn_mode]] e.hp -= 1;\n}");
            check_eq((size_t)0, error_count(region), region.format());
        });

        test("comparisons_that_exist_stay_clean", [this]() {
            auto e = make_engine();
            const std::string src = R"(
                class C { int v = 1; }
                class D { int v = 1; bool operator==(int o) { return v == o; } }
                class K { int v = 2; int operator<=>(var o) { return 0; } }
                enum E { A, B }
                int i = 1; float f = 1.0; char c = 'a'; string s = "s"; array<int> a = [1]; map<string, int> m = {};
                C oc = C(); D od = D(); K ok = K(); shared_ptr<C> h = new C(); var dyn = 3; function fn = (x) => x; var nothing = null;
                array<bool> r = [i == f, i < c, c == 97, s == "t", a == [2], m != {}, oc == C(), od == 1, 1 == od,
                                 ok < 3, 3 < ok, h == null, fn == null, dyn == "x", dyn < 2, E::A == 0, E::A == E::B, h == h,
                                 oc == h, a < [3], s < "t", dyn == null, nothing == 1];
                r.size()
            )";
            auto rep = e->check(src);
            check_eq((size_t)0, rep.diagnostics.size(), rep.format(src));
            check_eq((int64_t)23, e->execute(src).as_int());
        });

        // ------------------------------------------------------------ typed slots: what never converts at load
        test("a_float_into_an_int_slot_is_a_load_error_at_every_store_site", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            const std::string hint = ": cast<int>(x) truncates toward zero, math::ifloor(x) rounds down";
            const std::string vec_hint = ": cast<math::ivec2>(v) truncates each component toward zero";
            const std::vector<std::pair<std::string, std::string>> cases = {
                {"int x = 2.5;", "cannot initialize 'x' declared 'int' with 'float'" + hint},
                {"int x = 0; x = 2.5;", "cannot assign 'float' to 'x' declared 'int'" + hint},
                {"int x = 0; x += 0.5;", "cannot assign 'float' to 'x' declared 'int'" + hint},
                {"void f(int p) {} f(1.5);", "argument 1 of 'f': cannot pass 'float' to 'p' declared 'int'" + hint},
                {"int g() { return 1.5; }", "cannot return 'float' from 'g' declared to return 'int'" + hint},
                {"class C { int n = 1.5; }", "cannot initialize field 'n' declared 'int' with 'float'" + hint},
                {"class C { int n = 0; } C c; c.n = 1.5;", "cannot assign 'float' to field 'n' declared 'int'" + hint},
                {"class C { int v = 0; C(int x) { v = x; } } C c = C(1.5);", "argument 1 of 'C': cannot pass 'float' to 'x' declared 'int'" + hint},
                {"class C { int v = 0; C(int x) { v = x; } } C c = 1.5;", "cannot initialize 'c' declared 'C' with 'float'"},
                {"array<int> a = [1]; a[0] = 1.5;", "cannot store into 'array<int>': expected 'int', got 'float'" + hint},
                {"array<int> a = [1]; a.push(1.5);", "cannot push into 'array<int>': expected 'int', got 'float'" + hint},
                {"array<int> a = [1]; a.fill(1.5);", "fill() cannot store into 'array<int>': expected 'int', got 'float'" + hint},
                {"array<int> a = [1]; a.resize(3, 1.5);", "resize() cannot store into 'array<int>': expected 'int', got 'float'" + hint},
                {"map<string,int> m = {}; m[\"k\"] = 1.5;", "cannot store into 'map<string,int>': expected 'int', got 'float'" + hint},
                {"array<float> f = [1.5]; array<int> a = f;", "cannot initialize 'a' declared 'array<int>' with 'array<float>': array element type: expected 'int', got 'float'" + hint},
                {"void f(int q = 1.5) {}", "default value of 'q' declared 'int' has type 'float'"},
                // a float vector into an integer vector's slot is the same narrowing
                {"math::vec2 v; math::ivec2 a = v;", "cannot initialize 'a' declared 'math::ivec2' with 'math::vec2'" + vec_hint},
                {"math::vec2 v; math::ivec2 a; a = v;", "cannot assign 'math::vec2' to 'a' declared 'math::ivec2'" + vec_hint},
                {"void f(math::ivec2 p) {} f(math::vec2(1.5, 2.5));", "argument 1 of 'f': cannot pass 'math::vec2' to 'p' declared 'math::ivec2'" + vec_hint},
                {"math::ivec3 g() { return math::vec3(1.5, 2.5, 3.5); }", "cannot return 'math::vec3' from 'g' declared to return 'math::ivec3'"},
                {"class C { math::ivec2 at; } C c; c.at = math::vec2(1.5, 2.5);", "cannot assign 'math::vec2' to field 'at' declared 'math::ivec2'" + vec_hint},
                {"array<math::ivec2> a; a.push(math::vec2(1.5, 2.5));", "cannot push into 'array<math::ivec2>': expected 'math::ivec2', got 'math::vec2'" + vec_hint},
                {"array<math::ivec2> a = [math::ivec2(1, 2)]; a[0] = math::vec2(1.5, 2.5);", "cannot store into 'array<math::ivec2>': expected 'math::ivec2', got 'math::vec2'" + vec_hint},
                {"array<math::vec2> f; array<math::ivec2> a = f;", "cannot initialize 'a' declared 'array<math::ivec2>' with 'array<math::vec2>': array element type: expected 'math::ivec2', got 'math::vec2'" + vec_hint},
                {"math::ivec2 a; math::ivec2& r = a; r = math::vec2(1.5, 2.5);", "cannot assign 'math::vec2' to 'r' declared 'math::ivec2&'" + vec_hint},
                // a typed reference stores by its referent's table
                {"void s(int& a) { a = \"str\"; }", "cannot assign 'string' to 'a' declared 'int&'"},
                {"int u = 1; int& r = u; r = \"s\";", "cannot assign 'string' to 'r' declared 'int&'"},
                {"void s(string& a) { a = 5; }", "cannot assign 'int' to 'a' declared 'string&': stringify it with to_string(x) or `${x}`"},
                {"void s(array<int>& a) { a = \"x\"; }", "cannot assign 'string' to 'a' declared 'array<int>&': expected 'array<int>', got 'string'"},
                {"void s(bool& b) { b = \"x\"; }", "cannot assign 'string' to 'b' declared 'bool&': write !x.empty() or an explicit comparison"},
                {"void s(int& a) { a += 0.5; }", "cannot assign 'float' to 'a' declared 'int&'" + hint},
            };

            for (const auto& [src, text] : cases) {
                auto rep = e->check(src);
                check_eq((size_t)1, error_count(rep), rep.format(src));
                check_true(has_diag(rep, dlevel::error, text), rep.format(src));
                auto warned = e->check("[[warn_mode]]; " + src);
                check_eq((size_t)0, error_count(warned), warned.format(src));
                check_true(has_diag(warned, dlevel::warning, text), warned.format(src));
            }
            const std::string clean = "float f = 1; f = 2; f += 3; int a = cast<int>(2.5); int b = math::ifloor(2.5); int c = math::itrunc(-2.5); int d = true; "
                                      "var v = 2; int g = v; array<float> fs = [1, 2]; array<int> xs = [cast<int>(f)]; xs.push(math::ifloor(f)); `${a} ${b} ${c} ${d} ${g}`;";
            auto rep = e->check(clean);
            check_eq((size_t)0, rep.diagnostics.size(), rep.format(clean));
            check_eq(std::string("2 2 -2 1 2"), e->execute(clean).as_string());
            // cast<math::ivecN> is the narrowing's spelling and the constructor converts what it is handed; an ivec widens anywhere;
            // a reference stores what its referent takes
            const std::string vectors =
                "math::vec2 v = math::vec2(1.9, -2.9); math::ivec2 a = cast<math::ivec2>(v); math::ivec3 b = cast<math::ivec3>(math::vec3(1.5, -2.5, 3.5));\n"
                "math::vec2 w = a; array<math::vec2> ws; ws.push(a); math::vec3 up(math::ivec3 i) { return i; } math::ivec2 made = math::ivec2(v);\n"
                "void set(int& n, string& s, float& f, array<int>& xs) { n = 2; n += 1; s = \"t\"; s += \"u\"; f = 1; xs = [4]; } int n = 0; string s; float f = 0.5; array<int> xs;\n"
                "set(n, s, f, xs); `${a} ${b} ${w} ${ws} ${up(b)} ${made} ${n}${s}${f}${xs}`;";
            rep = e->check(vectors);
            check_eq((size_t)0, rep.diagnostics.size(), rep.format(vectors));
            check_eq(std::string("(1, -2) (1, -2, 3) (1.0, -2.0) [(1.0, -2.0)] (1.0, -2.0, 3.0) (1, -2) 3tu1.0[4]"), e->execute(vectors).as_string());
            // through var the slot refuses at run time, and a component the cast cannot hold raises as cast<int> does
            const std::vector<std::pair<std::string, std::string>> at_run = {
                {"math::ivec2 a = v;", "Cannot initialize 'a' (declared math::ivec2) with math::vec2"},
                {"math::ivec2 a; a = v;", "Cannot assign math::vec2 to 'a' (declared math::ivec2)"},
                {"void f(math::ivec2 p) {} f(v);", "Cannot pass math::vec2 to parameter 'p' of 'f' (declared math::ivec2)"},
                {"math::ivec2 g() { return v; } g();", "Cannot return math::vec2 from 'g' (declared -> math::ivec2)"},
                {"class C { math::ivec2 at; } C c; c.at = v;", "Cannot assign math::vec2 to field 'at' of C (declared math::ivec2)"},
                {"array<math::ivec2> xs; xs.push(v);", "Cannot push 'math::vec2' to array<math::ivec2>"},
                {"array<math::ivec2> xs = [math::ivec2(1, 2)]; xs[0] = v;", "Cannot assign 'math::vec2' to element of type 'math::ivec2'"},
                {"var many = [v]; array<math::ivec2> xs = many;", "Cannot store math::vec2 in array<math::ivec2>"},
                {"math::ivec2 a; math::ivec2& r = a; r = v;", "Cannot assign math::vec2 to 'r' (declared math::ivec2&)"},
                {"math::vec2 big = math::vec2(1e30, 0.0); cast<math::ivec2>(big);", "Cannot convert float to integer: non-finite or out of range"},
                {"math::vec3 nan = math::vec3(0.0, math::NAN_VALUE, 0.0); cast<math::ivec3>(nan);", "Cannot convert float to integer: non-finite or out of range"},
                {"void s(int& a) { var t = \"str\"; a = t; } int n = 0; s(n);", "Cannot assign string to 'a' (declared int&)"},
            };
            for (const auto& [src, text] : at_run) {
                const std::string got = outcome(*make_engine(), "var v = math::vec2(1.9, -2.9); " + src);
                check_true(got.rfind("ERROR " + text, 0) == 0, src + " -> " + got);
            }
        });

        test("a_class_value_slot_takes_no_null_and_a_class_never_holds_itself_by_value", [this]() {
            auto e = make_engine();
            const std::string tail = ": a 'V' is never null; declare 'shared_ptr<V>' (or var) for a slot that can be empty";
            const std::vector<std::pair<std::string, std::string>> cases = {
                {"V a = null;", "cannot initialize 'a' declared 'V' with 'null'" + tail},
                {"V b; b = null;", "cannot assign 'null' to 'b' declared 'V'" + tail},
                {"void f(V v) {} f(null);", "argument 1 of 'f': cannot pass 'null' to 'v' declared 'V'" + tail},
                {"V h() { return null; }", "cannot return 'null' from 'h' declared to return 'V'" + tail},
                {"class H { V v = null; }", "cannot initialize field 'v' declared 'V' with 'null'" + tail},
                {"class H { static V s = null; }", "cannot initialize field 's' declared 'V' with 'null'" + tail},
                {"array<V> vs = [V()]; for (V v : vs) { v = null; }", "cannot assign 'null' to 'v' declared 'V'" + tail},
                {"void f(V d = null) {}", "default value of 'd' declared 'V' has type 'null'"},
                {"array<V> vs = []; vs.push(null);", "cannot push into 'array<V>': expected 'V', got 'null'"},
                {"map<string,V> m = {}; m[\"k\"] = null;", "cannot store into 'map<string,V>': expected 'V', got 'null'"},
                {"V a; bool b = a == null;", "cannot compare 'V' with null using '==': 'V' is never null (it is always false); declare the slot shared_ptr<V> (or var) to hold null"},
                {"V g(int k) { if (k > 0) { return V(); } }", "'g' declared to return 'V' may reach the end of its body without returning"},
                {"V g() { return; }", "'g' declared to return 'V' returns no value here"},
            };
            for (const auto& [src, text] : cases) {
                const std::string unit = "class V {} " + src;
                auto rep = e->check(unit);
                check_eq((size_t)1, error_count(rep), rep.format(unit));
                check_true(has_diag(rep, dlevel::error, text), rep.format(unit));
            }
            const std::string nullable = "class V { int n = 1; } shared_ptr<V> p = null; p = new V(); p = null; weak_ptr<V> w = null; var v = V(); v = null; auto a = null; "
                                         "function fn = null; shared_ptr<V> give() { return null; } array<shared_ptr<V>> ps = [null]; p == null && give() == null;";
            auto quiet = e->check(nullable);
            check_eq((size_t)0, error_count(quiet), quiet.format(nullable));
            check_eq(true, e->execute(nullable).as<bool>());

            const std::string link = ": a class value is never null; make one link a 'shared_ptr<";
            const std::vector<std::pair<std::string, std::vector<std::string>>> cycles = {
                {"class Node { Node next; }", {"class 'Node' holds itself by value (Node.next)" + link + "Node>' (an array or a map of them starts empty)"}},
                {"class Node { Node next = null; }", {"class 'Node' holds itself by value (Node.next)", "cannot initialize field 'next' declared 'Node' with 'null': a 'Node' is never null"}},
                {"class A { B b; } class B { C c; } class C { A a; }", {"class 'A' holds itself by value (A.b -> B.c -> C.a)" + link + "A>'", "class 'B' holds itself by value (B.c -> C.a -> A.b)", "class 'C' holds itself by value (C.a -> A.b -> B.c)"}},
                {"class Base { Derived d; } class Derived : Base { }", {"class 'Base' holds itself by value (Base.d -> Derived : Base)", "class 'Derived' holds itself by value (Derived : Base -> Base.d)" + link + "Derived>'"}},
            };
            for (const auto& [src, texts] : cycles) {
                for (const std::string& unit : {src, "[[warn_mode]]; " + src}) {
                    auto rep = e->check(unit);
                    check_ge(error_count(rep), (size_t)1, rep.format(unit));
                    if (unit == src) { check_eq(texts.size(), error_count(rep), rep.format(unit)); }
                    for (const auto& text : texts) {
                        if (unit == src || text.find("holds itself") != std::string::npos) { check_true(has_diag(rep, dlevel::error, text), text + "\n" + rep.format(unit)); }
                    }
                }
            }
            const std::string linked = "class Node { int v = 0; shared_ptr<Node> next = null; array<Node> kids = []; map<string,Node> named = {}; }\n"
                                       "Node n; n.next = new Node(); n.next.v = 4; n.kids.push(Node()); n.named[\"k\"] = Node(); n.next.v + n.kids.size() + n.named.size() + (n.next.next == null ? 1 : 0);";
            auto ok = e->check(linked);
            check_eq((size_t)0, error_count(ok), ok.format(linked));
            check_eq((int64_t)7, e->execute(linked).as_int());
        });

        test("an_enum_name_is_a_type_whose_slots_take_its_values_only", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            const std::string decl = "enum Dir { north, south, east = 5, west } enum Fx { bones, blood }\n";
            const std::string hint = ": cast<Dir>(n) converts an int that is one of its values";
            const std::vector<std::pair<std::string, std::string>> cases = {
                {"Dir d = 1;", "cannot initialize 'd' declared 'Dir' with 'int'" + hint},
                {"Dir d = Fx::bones;", "cannot initialize 'd' declared 'Dir' with 'Fx'" + hint},
                {"Dir d = Dir::north; d = 1;", "cannot assign 'int' to 'd' declared 'Dir'" + hint},
                {"void go(Dir where) {} go(1);", "argument 1 of 'go': cannot pass 'int' to 'where' declared 'Dir'" + hint},
                {"Dir f() { return 1; }", "cannot return 'int' from 'f' declared to return 'Dir'" + hint},
                {"class C { Dir d = 1; }", "cannot initialize field 'd' declared 'Dir' with 'int'" + hint},
                {"class C { Dir d; } C c; c.d = 1;", "cannot assign 'int' to field 'd' declared 'Dir'" + hint},
                {"array<Dir> ds = []; ds.push(1);", "cannot push into 'array<Dir>': expected 'Dir', got 'int'"},
                {"map<Dir,int> m = {}; m[1] = 2;", "Cannot use 'int' as a key of type 'Dir'"},
                {"Dir d = Dir::north; d += 1;", "cannot assign 'int' to 'd' declared 'Dir'" + hint},
                {"Dir d = Dir::north; d++;", "cannot apply '++' to 'Dir'"},
                {"Dir d = Dir::north; bool b = d == null;", "cannot compare 'Dir' with null using '==': 'Dir' is never null"},
                {"Dir d = Dir::north; string s = d;", "cannot initialize 's' declared 'string' with 'Dir'"},
            };
            for (const auto& [src, text] : cases) {
                auto rep = e->check(decl + src);
                check_eq((size_t)1, error_count(rep), rep.format(decl + src));
                check_true(has_diag(rep, dlevel::error, text), rep.format(decl + src));
            }
            const std::string clean = decl +
                "Dir pick(Dir where) { return where; }\n"
                "class Walker { Dir facing; Dir back = Dir::south; }\n"
                "string which(Dir d) { return \"Dir\"; } string which(int n) { return \"int\"; }\n"
                "Dir d = Dir::south; Dir first; int n = d; auto a = Dir::west; a = pick(Dir.east); var v = Dir::west; Walker w;\n"
                "array<int> xs = [d]; map<int,string> names = {}; names[d] = \"s\"; array<Dir> ds = [Dir::north]; ds.push(d); map<Dir,int> m = {}; m[Dir::east] = 1;\n"
                "Dir c = cast<Dir>(5); string out = \"\"; switch (c) { case Dir::east: out = \"east\"; break; default: out = \"other\"; }\n"
                "`${type_of(d)} ${type_of(first)}:${first} ${type_of(n)}:${n} ${type_of(d + 1)}:${d + 1} ${type_of(a)}:${a} ${type_of(v)}:${v} ${type_of(w.facing)}:${w.facing}${w.back} "
                "${d == 1} ${out} ${which(d)},${which(Dir::north)},${which(3)},${which(v)} ${Dir[\"east\"]} ${Dir.size()} ${ds.size()} ${m[Dir::east]}`;";
            auto rep = e->check(clean);
            check_eq((size_t)0, error_count(rep), rep.format(clean));
            check_eq(std::string("Dir Dir:0 int:1 int:2 Dir:5 int:6 Dir:01 true east Dir,Dir,int,int 5 4 2 1"), e->execute(clean).as_string());
            // through a var the run time decides: one of the enum's values enters and takes the type, any other int is refused
            check_eq(std::string("Dir:5|Cannot initialize 'd' (declared Dir) with int|Cannot assign int to 'd' (declared Dir)|Cannot pass int to parameter 'where' of 'go' (declared Dir)|"
                                 "Cannot cast 3 to 'Dir': not one of its values|Cannot cast 'string' to 'Dir'"),
                     e->execute(decl +
                "string t(function f) { string r = \"\"; try { r = `${f()}`; } catch (err) { r = err; } return r; }\n"
                "void go(Dir where) {}\n"
                "t([]() { var five = 5; Dir d = five; return `${type_of(d)}:${d}`; }) + \"|\" + t([]() { var three = 3; Dir d = three; return 1; }) + \"|\" +\n"
                "t([]() { var three = 3; Dir d = Dir::north; d = three; return 1; }) + \"|\" + t([]() { var three = 3; go(three); return 1; }) + \"|\" +\n"
                "t([]() { return cast<Dir>(3); }) + \"|\" + t([]() { var s = \"east\"; return cast<Dir>(s); });").as_string());
        });

        test("typed_slots_refuse_floats_nulls_and_ints_on_every_remaining_path", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            const std::string fix = ": cast<int>(x) truncates toward zero, math::ifloor(x) rounds down";
            const std::vector<std::pair<std::string, std::string>> cases = {
                {"int x = 10; x %= 4.5;", "cannot assign 'float' to 'x' declared 'int'" + fix},
                {"array<int> xs = [1]; xs.insert(0, 1.5);", "cannot insert into 'array<int>': expected 'int', got 'float'" + fix},
                {"array<float> fs = [1.5]; for (int v : fs) { }", "cannot initialize 'v' declared 'int' with 'float'" + fix},
                {"coroutine int g() { yield 1.5; }", "cannot yield 'float' from 'g' declared to return 'int'" + fix},
                {"coroutine int g() { yield 1; return 2.5; }", "cannot return 'float' from 'g' declared to return 'int'" + fix},
                {"bool c = true; int t = c ? 1 : 2.5;", "cannot initialize 't' declared 'int' with 'float'" + fix},
                {"function f(a, b = a * 2) { return b; }", "default value of 'b' names parameter 'a': a default cannot read a parameter"},
                {"map<string,int> m = {}; m.hp = 5;", "cannot assign to member 'hp' of a map; store the key as [\"hp\"]"},
            };
            for (const auto& [src, text] : cases) {
                auto rep = e->check(src);
                check_eq((size_t)1, error_count(rep), rep.format(src));
                check_true(has_diag(rep, dlevel::error, text), rep.format(src));
            }
            const std::string clean =
                "enum Dir { north, south } class P { int a = 0; }\n"
                "bool c = true; float f = c ? 1 : 2.5; auto a = c ? 1 : 2.5; array<int> xs = [1]; float sum = 0.0; for (float w : xs) { sum += w; }\n"
                "map<Dir, string> names = {Dir::south: \"S\", Dir::north: \"N\"};\n"
                "string caught = \"\"; try { throw Dir::south; } catch (int e) { caught = \"int\"; } catch (Dir e) { caught = `${type_of(e)}:${e}`; }\n"
                "`${type_of(a)} ${sum} ${names[Dir::south]}${names.size()} ${caught}`;";
            auto rep = e->check(clean);
            check_eq((size_t)0, error_count(rep), rep.format(clean));
            check_eq(std::string("float 1.0 S2 Dir:1"), e->execute(clean).as_string());
            // a null a var hands a class value static or field, and a null literal under a warn region, is the store's run-time error
            const auto refused = [](const std::string& body) {
                auto fresh = make_engine();
                try { fresh->execute("[[warn_mode]]\nclass P { int a = 0; } function nothing(var x) { return x; }\n" + body); } catch (const std::exception& ex) { return std::string(ex.what()); }
                return std::string("stored");
            };
            check_true(refused("class C { static P none = nothing(null); }").find("Cannot assign null to static 'none' of C (declared P)") != std::string::npos);
            check_true(refused("class D { P f = nothing(null); } D d;").find("Cannot assign null to field 'f' of D (declared P)") != std::string::npos);
            check_true(refused("class E { P f = null; } E e;").find("Cannot assign null to field 'f' of E (declared P)") != std::string::npos);
        });

        test("a_declared_bool_slot_refuses_text_and_containers_at_load", [this]() {
            auto e = make_engine();
            const std::string hint = ": write !x.empty() or an explicit comparison";
            const std::vector<std::pair<std::string, std::string>> cases = {
                {"bool b = \"x\";", "cannot initialize 'b' declared 'bool' with 'string'" + hint},
                {"bool b = false; b = \"x\";", "cannot assign 'string' to 'b' declared 'bool'" + hint},
                {"void f(bool flag) {} f(\"x\");", "argument 1 of 'f': cannot pass 'string' to 'flag' declared 'bool'" + hint},
                {"bool g() { return \"x\"; }", "cannot return 'string' from 'g' declared to return 'bool'" + hint},
                {"class C { bool on = \"x\"; }", "cannot initialize field 'on' declared 'bool' with 'string'" + hint},
                {"class C { bool on = false; } C c; c.on = \"x\";", "cannot assign 'string' to field 'on' declared 'bool'" + hint},
                {"utf8_string u = \"x\"; bool b = u;", "cannot initialize 'b' declared 'bool' with 'utf8_string'" + hint},
                {"bool b = [1];", "cannot initialize 'b' declared 'bool' with 'array'" + hint},
                {"bool b = {\"k\": 1};", "cannot initialize 'b' declared 'bool' with 'map'" + hint},
            };
            for (const auto& [src, text] : cases) {
                auto rep = e->check(src);
                check_eq((size_t)1, error_count(rep), rep.format(src));
                check_true(has_diag(rep, dlevel::error, text), rep.format(src));
            }
            // conditions keep truthiness, numbers, null and handles still convert, and a var's string converts at run time
            const std::string clean = "class K {} string s = \"x\"; string none = \"\"; bool a = !s.empty(); bool b = s == \"x\"; bool c = 1; bool d = 0.5; bool e = !s; bool f = s && true;\n"
                                      "var v = \"x\"; bool g = v; var blank = \"\"; bool g2 = blank; shared_ptr<K> p = null; bool h = p; int taken = 0; if (s) { taken += 1; } if (none) { taken += 10; }\n"
                                      "`${a}${b}${c}${d}${e}${f}${g}${g2}${h}${taken}`;";
            auto rep = e->check(clean);
            check_eq((size_t)0, error_count(rep), rep.format(clean));
            check_eq(std::string("truetruetruetruefalsetruetruefalsefalse1"), e->execute(clean).as_string());
        });

        test("a_builtin_method_on_a_known_receiver_has_its_catalog_result_type", [this]() {
            auto e = make_engine();
            const std::string decl = "string text = \"abc\"; utf8_string u = \"abc\"; array<int> xs = [1]; array<string> names = [\"a\"]; map<string,int> m = {\"k\": 1};\n";
            const std::string stringify = ": stringify it with to_string(x) or `${x}`";
            const std::vector<std::pair<std::string, std::string>> cases = {
                {"int bad = names.join(\",\");", "cannot initialize 'bad' declared 'int' with 'string'"},
                {"string s = text.size();", "cannot initialize 's' declared 'string' with 'int'" + stringify},
                {"string s = xs.size();", "cannot initialize 's' declared 'string' with 'int'" + stringify},
                {"string s = m.size();", "cannot initialize 's' declared 'string' with 'int'" + stringify},
                {"string s = text.find(\"b\");", "cannot initialize 's' declared 'string' with 'int'" + stringify},
                {"string s = text.contains(\"a\");", "cannot initialize 's' declared 'string' with 'bool'" + stringify},
                {"string s = xs.empty();", "cannot initialize 's' declared 'string' with 'bool'" + stringify},
                {"string s = m.has(\"k\");", "cannot initialize 's' declared 'string' with 'bool'" + stringify},
                {"int n = text.substr(0, 1);", "cannot initialize 'n' declared 'int' with 'string'"},
                {"int n = u.substr(0, 1);", "cannot initialize 'n' declared 'int' with 'utf8_string'"},
                {"string s = xs.front();", "cannot initialize 's' declared 'string' with 'int'" + stringify},
                {"string s = xs.pop();", "cannot initialize 's' declared 'string' with 'int'" + stringify},
                {"int n = names.back();", "cannot initialize 'n' declared 'int' with 'string'"},
                {"int n = text.at(0);", "cannot initialize 'n' declared 'int' with 'char'"},
                {"string s = text.to_int(0);", "cannot initialize 's' declared 'string' with 'int'" + stringify},
                {"string s = text.to_float(0.0);", "cannot initialize 's' declared 'string' with 'float'" + stringify},
            };
            for (const auto& [src, text] : cases) {
                auto rep = e->check(decl + src);
                check_eq((size_t)1, error_count(rep), rep.format(decl + src));
                check_true(has_diag(rep, dlevel::error, text), rep.format(decl + src));
            }
            const std::string clean = decl + "int a = text.size(); int b = xs.size(); bool c = xs.empty(); bool d = m.has(\"k\"); string f = names.join(\",\"); string g = text.substr(0, 1);\n"
                                             "utf8_string w = u.substr(1, 1); int h = xs.front(); string i = names.back(); char j = text.at(1); int k = text.find(\"c\"); string seven = \"7\";\n"
                                             "int l = seven.to_int(0); float n = seven.to_float(0.0); int o = seven.to_int(); int p = xs.pop();\n"
                                             "`${a} ${b} ${c} ${d} ${f} ${g} ${w} ${h} ${i} ${j} ${k} ${l} ${n} ${o} ${p}`;";
            auto rep = e->check(clean);
            check_eq((size_t)0, error_count(rep), rep.format(clean));
            check_eq(std::string("3 1 false true a a b 1 a b 2 7 7.0 7 1"), e->execute(clean).as_string());
            // to_int() with no fallback returns null for text that does not parse, which an int slot refuses at run time
            check_eq(std::string("ERROR Cannot initialize 'n' (declared int) with null"), outcome(*e, "string word = \"abc\"; int n = word.to_int(); n"));
        });

        test("a_qualified_name_is_checked_as_the_bare_one", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            const std::string decl = "namespace game { int score = 0; string title = \"t\"; void add(int n) { score += n; } int twice(int n) { return n * 2; }\n"
                                     "  namespace deep { array<int> log; void note(string s) { } } }\n";
            const std::vector<std::pair<std::string, std::string>> cases = {
                {"game::score = \"ten\";", "cannot assign 'string' to 'game::score' declared 'int'"},
                {"game::score += 0.5;", "cannot assign 'float' to 'game::score' declared 'int': cast<int>(x) truncates toward zero, math::ifloor(x) rounds down"},
                {"game::deep::log = 5;", "cannot assign 'int' to 'game::deep::log' declared 'array<int>': expected 'array<int>', got 'int'"},
                {"string s = game::score;", "cannot initialize 's' declared 'string' with 'int': stringify it with to_string(x) or `${x}`"},
                {"int n = game::title;", "cannot initialize 'n' declared 'int' with 'string'"},
                {"game::add(\"x\");", "argument 1 of 'game::add': cannot pass 'string' to 'n' declared 'int'"},
                {"game::add(1, 2);", "'game::add' expects 1 argument(s), got 2"},
                {"game::deep::note(5);", "argument 1 of 'game::deep::note': cannot pass 'int' to 's' declared 'string': stringify it with to_string(x) or `${x}`"},
                {"string s = game::twice(2);", "cannot initialize 's' declared 'string' with 'int': stringify it with to_string(x) or `${x}`"},
                {"using game::add; add(\"x\");", "argument 1 of 'game::add': cannot pass 'string' to 'n' declared 'int'"},
                {"using game::score; score = \"ten\";", "cannot assign 'string' to 'game::score' declared 'int'"},
            };
            for (const auto& [src, text] : cases) {
                auto rep = e->check(decl + src);
                check_eq((size_t)1, error_count(rep), rep.format(decl + src));
                check_true(has_diag(rep, dlevel::error, text), rep.format(decl + src));
            }
            const std::string clean = decl + "using game::twice; game::score = 2; game::add(3); game::deep::note(\"n\"); game::deep::log = [1]; int n = game::twice(game::score); float f = game::score;\n"
                                             "var any = \"late\"; string caught = \"\"; try { game::score = any; } catch (e) { caught = e; }\n"
                                             "`${n} ${f} ${twice(1)} ${game::title} ${caught}`;";
            auto rep = e->check(clean);
            check_eq((size_t)0, error_count(rep), rep.format(clean));
            check_eq(std::string("10 5.0 2 t Cannot assign string to 'score' (declared int)"), e->execute(clean).as_string());
            // a namespace another run declared: a call none of its functions takes fails the load, any it does take loads
            check_eq((size_t)0, error_count(e->check("game::add(4); game::deep::note(\"x\");")));
            auto later = e->check("game::add(\"x\");");
            check_true(has_diag(later, dlevel::error, "argument 1 of 'game::add': cannot pass 'string' to 'n' declared 'int'"), later.format(""));
        });

        test("builtin_and_host_arguments_are_checked_against_their_signatures", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            struct Probe { int n = 0; void set(int v) { n = v; } std::string name(const std::string& prefix, int times) const { return prefix + std::to_string(times); } };
            dynamic_binder<Probe>(*e, "Probe").constructor<>().method("set", &Probe::set).method("name", &Probe::name).build();
            e->add_function("host_scale", [](double value, int by) { return value * by; });
            e->add_function("host_label", [](const std::string& text) { return text + "!"; });
            const std::string decl = "string text = \"abc\"; array<int> xs = [1, 2]; map<string,int> m = {\"k\": 1}; Probe probe;\n";
            const std::vector<std::pair<std::string, std::string>> cases = {
                {"float r = math::sqrt(\"nine\");", "argument 1 of 'math::sqrt': cannot pass 'string' where a number is expected"},
                {"float r = math::pow(2, xs);", "argument 2 of 'math::pow': cannot pass 'array<int>' where a number is expected"},
                {"float r = math::length(3);", "argument 1 of 'math::length': cannot pass 'int' where a vector or color is expected"},
                {"var r = math::floor(\"x\");", "argument 1 of 'math::floor': cannot pass 'string' where a number or a vector or color is expected"},
                {"string r = text.substr(\"a\");", "argument 1 of 'substr' on 'string': cannot pass 'string' where a number is expected"},
                {"int r = text.find(3);", "argument 1 of 'find' on 'string': cannot pass 'int' where text is expected"},
                {"xs.remove(\"x\");", "argument 1 of 'remove' on 'array<int>': cannot pass 'string' where a number is expected"},
                {"xs.filter(3);", "argument 1 of 'filter' on 'array<int>': cannot pass 'int' where a function is expected"},
                {"xs.concat(text);", "argument 1 of 'concat' on 'array<int>': cannot pass 'string' where an array is expected"},
                {"string r = xs.join(4);", "argument 1 of 'join' on 'array<int>': cannot pass 'int' where text is expected"},
                {"m.filter(\"k\");", "argument 1 of 'filter' on 'map<string,int>': cannot pass 'string' where a function is expected"},
                {"host_scale(\"wide\", 2);", "no overload of 'host_scale' accepts these argument types"},
                {"host_scale(1.5);", "'host_scale' expects 2 argument(s), got 1"},
                {"host_label(5);", "no overload of 'host_label' accepts these argument types"},
                {"probe.set(\"five\");", "no overload of 'Probe.set' accepts these argument types"},
                {"probe.set(1, 2);", "'Probe.set' expects 1 argument(s), got 2"},
                {"probe.name(3, 4);", "no overload of 'Probe.name' accepts these argument types"},
            };
            for (const auto& [src, text] : cases) {
                auto rep = e->check(decl + src);
                check_eq((size_t)1, error_count(rep), rep.format(decl + src));
                check_true(has_diag(rep, dlevel::error, text), rep.format(decl + src));
            }
            // what a signature leaves open stays the run's to judge: a var, a class's callable or text conversion, an extern function
            const std::string clean = decl + "extern function later;\nclass Even { bool operator()(int v) { return v % 2 == 0; } }\n"
                "var any = 9; float a = math::sqrt(any); float b = math::pow(2, 3.0); var c = math::floor(math::vec2(1.5, 2.5)); xs.filter([](int v) { return v % 2 == 0; }); Even even; bool k = even(2);\n"
                "string d = text.substr(1); int f = text.find(\"c\"); string g = xs.join(\"-\"); probe.set(4); string h = probe.name(\"p\", 2); float i = host_scale(1, 2);\n"
                "string j = host_label(\"ok\"); void never() { later(1, \"two\", [3]); }\n"
                "`${a} ${b} ${c} ${xs} ${d} ${f} ${g} ${h} ${i} ${j}`;";
            auto rep = e->check(clean);
            check_eq((size_t)0, error_count(rep), rep.format(clean));
            check_eq(std::string("3.0 8.0 (1.0, 2.0) [2] bc 2 2 p2 2.0 ok!"), e->execute(clean).as_string());
            // the generated shapes agree with the run: every argument shape the load refuses raises when the call runs
            static const std::pair<uint8_t, const char*> samples[] = {{jai::detail::shape_number, "1"}, {jai::detail::shape_text, "\"a\""}, {jai::detail::shape_array, "[1, 2]"},
                {jai::detail::shape_map, "{\"a\": 1}"}, {jai::detail::shape_function, "[](a) { return true; }"}, {jai::detail::shape_math, "math::vec2(1, 2)"}};
            static const char* const receivers[] = {"[1, 2, 3]", "{\"a\": 1}", "\"hello\"", "\"hello\".to_utf8()"};
            const auto sample_for = [](uint8_t takes) {
                for (const auto& [shape, text] : samples) { if (takes & shape) { return text; } }
                return "1";
            };
            size_t refused = 0;
            std::string program = "[[warn_mode]]\narray<string> passed;\n";
            const auto add_calls = [&](const std::string& head, const uint8_t* shapes, size_t minimum, size_t maximum, const std::string& label) {
                const size_t count = std::min<size_t>(maximum, jai::detail::BUILTIN_SHAPE_SLOTS);
                for (size_t i = 0; i < count; ++i) {
                    if (shapes[i] == jai::detail::shape_any) { continue; }
                    for (const auto& [shape, text] : samples) {
                        if (shapes[i] & shape) { continue; }
                        std::string args;
                        for (size_t k = 0; k <= std::max(i, minimum == 0 ? 0 : minimum - 1); ++k) { args += (k ? ", " : "") + std::string(k == i ? text : sample_for(shapes[k])); }
                        program += "try { " + head + "(" + args + "); passed.push(\"" + label + " " + std::to_string(i) + " " + std::to_string(shape) + "\"); } catch (e) { }\n";
                        ++refused;
                    }
                }
            };
            for (const auto& row : jai::detail::BUILTIN_ARITIES) {
                add_calls("var r = " + std::string(receivers[row.receiver]) + "; r." + std::string(row.name), row.shapes, row.minimum, row.maximum, std::to_string(row.receiver) + "." + std::string(row.name));
            }
            for (const auto& row : jai::detail::MATH_FUNCTION_SHAPES) {
                bool rival = false;   // a signature of another shape with the same count answers for the name too
                for (const auto& other : jai::detail::MATH_FUNCTION_SHAPES) { rival = rival || (&other != &row && other.name == row.name && other.minimum <= row.maximum && row.minimum <= other.maximum); }
                if (!rival) { add_calls("var q = math::" + std::string(row.name), row.shapes, row.minimum, row.maximum, "math::" + std::string(row.name)); }
            }
            check_true(refused > 500, "the table checks arguments: " + std::to_string(refused));
            check_eq(std::string("[]"), e->execute(program + "to_string(passed);").as_string());
        });

        test("a_program_checks_calls_and_reads_across_its_units", [this]() {
            auto dir = closure_dir("cross_unit_calls");
            write_unit(dir, "lib.jai", "int twice(int n) { return n * 2; }\nint LIMIT = 10;\nvar loose = 1;\nfunction either(a) { return a; }\n");
            const std::string root = "import \"lib.jai\";\n";
            const std::vector<std::pair<std::string, std::string>> cases = {
                {"string s = twice(\"x\");", "argument 1 of 'twice': cannot pass 'string' to 'n' declared 'int'"},
                {"int n = twice(1, 2);", "'twice' expects 1 argument(s), got 2"},
                {"string s = twice(2);", "cannot initialize 's' declared 'string' with 'int': stringify it with to_string(x) or `${x}`"},
                {"string s = LIMIT;", "cannot initialize 's' declared 'string' with 'int': stringify it with to_string(x) or `${x}`"},
                {"LIMIT = \"high\";", "cannot assign 'string' to 'LIMIT' declared 'int'"},
                {"void later() { array<int> xs = twice(3); }", "cannot initialize 'xs' declared 'array<int>' with 'int': expected 'array<int>', got 'int'"},
            };
            for (const auto& [src, text] : cases) {
                auto e = program_engine(dir);
                const std::string got = outcome(*e, root + src);
                check_true(got.find("error: " + text) != std::string::npos, src + " -> " + got);
            }
            {
                auto e = program_engine(dir);
                check_eq(std::string("VALUE 14 10 1 x"), outcome(*e, root + "int n = twice(7); int m = LIMIT; string s = `${loose}`; loose = \"x\"; `${n} ${m} ${s} ${either(loose)}`;"));
                // a later run of another text meets the functions the engine holds
                check_true(outcome(*e, "twice(\"again\");").find("error: argument 1 of 'twice': cannot pass 'string' to 'n' declared 'int'") != std::string::npos);
                check_eq(std::string("VALUE 8"), outcome(*e, "twice(4)"));
            }
            // a reload that changes a signature its callers no longer fit is refused, naming them; the old code keeps running; with the
            // callers edited to fit, the same reload applies
            write_unit(dir, "main.jai", "import \"lib.jai\";\nint doubled(int v) { return twice(v); }\n");
            auto e = program_engine(dir);
            e->set_import_behavior(engine::import_behavior::always);
            const std::string run = "import \"main.jai\";\nimport \"lib.jai\";\ndoubled(4)";
            check_eq(std::string("VALUE 8"), outcome(*e, run));
            write_unit(dir, "lib.jai", "string twice(string n) { return n + n; }\nint LIMIT = 10;\nvar loose = 1;\nfunction either(a) { return a; }\n");
            const std::string refused = outcome(*e, run);
            check_true(refused.find("main.jai") != std::string::npos && refused.find("argument 1 of 'twice': cannot pass 'int' to 'n' declared 'string'") != std::string::npos, refused);
            check_eq(std::string("VALUE 8"), outcome(*e, "doubled(4)"), "the refused reload left the old code running");
            write_unit(dir, "main.jai", "import \"lib.jai\";\nint doubled(int v) { return twice(`${v}`).size(); }\n");
            check_eq(std::string("VALUE 2"), outcome(*e, run));
            std::filesystem::remove_all(dir);
        });

    }
};

} // namespace jai::foundry::tests

using static_check_tests = jai::foundry::tests::static_check_tests;
FOUNDRY_REGISTER(static_check_tests)
