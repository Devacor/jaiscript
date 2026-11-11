// Fixes found by a documentation audit: backend parity of serialized field order, the interpreter's
// call-depth contract on a small native stack, and the texts of several runtime and checker diagnostics.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class doc_audit_fix_tests : public suite {
public:
    doc_audit_fix_tests() : suite("Doc Audit Fixes") {}

    static std::shared_ptr<engine> fresh(bool use_vm) {
        auto e = jai::foundry::make_default_engine();
        e->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
        e->execution_budget(0);
        stdlib::register_all(e);
        return e;
    }

    static std::string outcome(engine& e, const std::string& src) {
        try { return e.execute(src).to_string(); }
        catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
    }

    // the run time's verdict (a [[warn_mode]] region over the whole unit), or the program as the strict checker loads it
    enum class load { warn_region, strict };

    // Both backends, byte-identical, then the expected text
    void expect(const std::string& src, const std::string& expected, const std::string& what, load how = load::warn_region) {
        std::string out[2];
        for (int i = 0; i < 2; ++i) {
            auto e = fresh(i == 1);
            out[i] = outcome(*e, how == load::warn_region ? "[[warn_mode]]; " + src : src);
        }
        check_eq(out[0], out[1], what + " (backends agree)");
        check_eq(expected, out[0], what);
    }

    // A strict load error carrying the text, on both backends
    void expect_load_error(const std::string& src, const std::string& text, const std::string& what) {
        for (bool use_vm : {false, true}) {
            auto e = fresh(use_vm);
            std::string message;
            try { e->execute(src); message = "(loaded)"; }
            catch (const static_check_error& ex) { message = ex.what(); }
            catch (const std::exception& ex) { message = std::string("(runtime) ") + ex.what(); }
            check_true(message.starts_with("static check failed") && message.find(text) != std::string::npos,
                       what + (use_vm ? " (vm)" : " (interpreter)") + ": expected a load error with \"" + text + "\", got: " + message);
        }
    }

    void forge_tests() override {
        test("serialized_fields_follow_declaration_order_bases_first", [this]() {
            const std::string defs =
                "var zeta = 1; var yak = 2; var xeno = 3; var wolf = 4; var vole = 5;"
                " class K { int vole = 1; int wolf = 2; int xeno = 3; int yak = 4; int zeta = 5; string apple = \"a\"; array<int> bag = [1]; float cat = 1.5; int dog = 7; }"
                " class B { int z = 1; int a = 2; } class D : B { int y = 3; int b = 4; }"
                " class H { string name = \"Vex\"; int hp = 30; array<string> bag = [\"rope\"]; }";
            expect(defs + " K k; to_json(k);",
                   "{\"$type\":\"K\",\"$val\":{\"vole\":1,\"wolf\":2,\"xeno\":3,\"yak\":4,\"zeta\":5,\"apple\":\"a\",\"bag\":[1],\"cat\":1.5,\"dog\":7}}",
                   "a class whose field names were interned out of order");
            expect(defs + " D d; to_json(d);", "{\"$type\":\"D\",\"$val\":{\"z\":1,\"a\":2,\"y\":3,\"b\":4}}", "a base's fields come first");
            expect(defs + " H h; to_json(h);", "{\"$type\":\"H\",\"$val\":{\"name\":\"Vex\",\"hp\":30,\"bag\":[\"rope\"]}}", "the audit's example");
            expect(defs + " K k; auto back = from_binary(to_binary(k)); to_json(back);",
                   "{\"$type\":\"K\",\"$val\":{\"vole\":1,\"wolf\":2,\"xeno\":3,\"yak\":4,\"zeta\":5,\"apple\":\"a\",\"bag\":[1],\"cat\":1.5,\"dog\":7}}",
                   "the binary archive keeps the order");
        });

#if defined(_WIN32)
        // The vm's script calls are in-loop frames: its depth is its cap on any host stack. The interpreter recurses natively,
        // so a small host stack ends its recursion first, with its own catchable text (a known divergence: docs/site/reference/errors.html "Run-time errors")
        test("a_small_host_stack_bounds_the_interpreter_with_its_own_text", [this]() {
            struct probe_state {
                std::string results[2];
                std::string failure;
            } state;
            auto probe = [](void* raw) -> DWORD {
                auto& state = *static_cast<probe_state*>(raw);
                try {
                    {
                        auto e = fresh(true);
                        std::string& out = state.results[1];
                        out += outcome(*e, "int f(int n) { if (n == 0) { return 0; } return 1 + f(n - 1); } f(200);") + "|";
                        out += outcome(*e, "f(255);") + "|";
                        out += outcome(*e, "class T { int down(int n) { if (n == 0) { return 0; } return 1 + down(n - 1); } } T().down(200);") + "|";
                        out += outcome(*e, "int g(int d) { return d == 0 ? 0 : 1 + g(d - 1); } int r = -1; try { r = g(220); } catch (e) { r = -2; } r;") + "|";
                        out += outcome(*e, "class N { int depth = 0; N(int n) { if (n > 0) { N child = N(n - 1); depth = child.depth + 1; } } } N(100).depth;") + "|";
                        out += outcome(*e, "40 + 2;");
                    }
                    {
                        auto e = fresh(false);
                        std::string& out = state.results[0];
                        const std::string uncaught = outcome(*e, "int f(int n) { if (n == 0) { return 0; } return 1 + f(n - 1); } f(255);");
                        out += uncaught.substr(0, uncaught.find('\n')) + "|";
                        out += outcome(*e, "string t = \"\"; try { f(255); } catch (e) { t = e; } t;") + "|";
                        out += outcome(*e, "40 + 2;");
                    }
                } catch (const std::exception& error) {
                    state.failure = error.what();
                } catch (...) {
                    state.failure = "non-standard exception in the small-stack probe";
                }
                return 0;
            };
            HANDLE thread = CreateThread(nullptr, 1024 * 1024, probe, &state, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            check_true(thread != nullptr, "could not create the small-stack probe thread");
            const DWORD wait = WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
            check_eq(DWORD(WAIT_OBJECT_0), wait);
            check_eq(std::string(), state.failure);
            const std::string native = "Native stack exhausted before the call-depth cap: the host thread's stack bounds this recursion";
            check_eq("ERROR: " + native + "|" + native + "|42", state.results[0], "interpreter");
            check_eq(std::string("200|255|200|220|100|42"), state.results[1], "vm");
        });
#endif

        test("the_interpreter_cap_raises_the_depth_text_without_a_category_suffix", [this]() {
            auto e = fresh(false);
            const std::string uncaught = outcome(*e, "int f(int n) { if (n == 0) { return 0; } return 1 + f(n - 1); } f(256);");
            check_eq(std::string("ERROR: Maximum recursion depth exceeded - possible infinite recursion"), uncaught.substr(0, uncaught.find('\n')));
            check_eq(std::string("255"), outcome(*e, "f(255);"));
        });

        test("a_store_through_a_by_value_operator_subscript_names_the_call", [this]() {
            const std::string defs = "class G { array<int> v = [1, 2, 3]; int operator[](int i) { return v[i]; } }"
                                     " class R { array<int> v = [1, 2, 3]; int& operator[](int i) { return v[i]; } }";
            const std::string text = "Cannot assign to a call result: 'operator[]' returns by value";
            expect(defs + " G g; var out = \"\";"
                   " try { g[1] = 5; } catch (e) { out += e + \"|\"; }"
                   " try { g[1] += 5; } catch (e) { out += e + \"|\"; }"
                   " try { g[1]++; } catch (e) { out += e + \"|\"; }"
                   " int i = 2; try { g[i] = 5; } catch (e) { out += e + \"|\"; }"
                   " void put(G& x) { x[0] = 9; } try { put(g); } catch (e) { out += e + \"|\"; }"
                   " out + to_string(g.v[0]) + to_string(g.v[1]) + to_string(g.v[2]);",
                   text + "|" + text + "|" + text + "|" + text + "|" + text + "|123", "every store shape on a by-value operator[]");
            expect(defs + " R r; r[1] = 5; r[2] += 10; to_string(r.v[0]) + to_string(r.v[1]) + to_string(r.v[2]);", "1513", "a reference-returning operator[] stores");
            expect_load_error(defs + " G g; g[1] = 5;", "cannot assign to a call result: 'operator[]' returns by value", "strict, a plain store");
            expect_load_error(defs + " G g; g[1] += 5;", "cannot assign to a call result: 'operator[]' returns by value", "strict, a compound store");
            expect(defs + " R r; r[1] = 5; to_string(r[1]);", "5", "strict admits a reference-returning operator[]", load::strict);
        });

        test("a_class_static_read_with_a_dot_says_to_use_scope_resolution", [this]() {
            const std::string defs = "class C { static int s = 4; static int f() { return 1; } }";
            expect(defs + " var out = \"\";"
                   " try { var x = C.s; } catch (e) { out += e + \"|\"; }"
                   " try { C.s = 5; } catch (e) { out += e + \"|\"; }"
                   " try { C.f(); } catch (e) { out += e + \"|\"; }"
                   " void g() { var y = C.s; } try { g(); } catch (e) { out += e + \"|\"; }"
                   " out + to_string(C::s);",
                   "'C' is a class; use C::s|'C' is a class; use C::s|'C' is a class; use C::f|'C' is a class; use C::s|4",
                   "reads, stores and calls through a dot");
            expect_load_error(defs + " var x = C.s;", "'C' is a class; use C::s", "strict, a read");
            expect_load_error(defs + " C.s = 5;", "'C' is a class; use C::s", "strict, a store");
            expect_load_error(defs + " C.f();", "'C' is a class; use C::f", "strict, a call");
        });

        test("an_enum_value_reads_with_scope_resolution", [this]() {
            expect("enum Dir { n, s, e } to_string(Dir::s) + to_string(Dir::e) + to_string(Dir.n);", "120", "a program-scope enum");
            expect("int pick() { enum Mode { off, on } return Mode::on; } pick();", "1", "an enum declared in a function");
            expect("enum Dir { n, s } int read() { return Dir::s; } read();", "1", "a function reads a program-scope enum");
            expect("enum Dir { n, s } var out = \"\"; try { var x = Dir::zz; } catch (e) { out += e; } out;",
                   "No value 'zz' in enum 'Dir'", "a missing value");
            expect("enum Dir { n, s } to_string(Dir::s);", "1", "strict admits a scoped enum value", load::strict);
            expect_load_error("enum Dir { n, s } var x = Dir::zz;", "no value 'zz' in enum 'Dir'", "strict, a missing value");
        });

        test("constructor_overloads_none_viable_raise_the_overload_miss", [this]() {
            expect("class V { int a; V(int x) { a = x; } V(bool b) { a = 1; } } var out = \"\"; try { V v = V(\"s\"); } catch (e) { out += e; } out;",
                   "No constructor of 'V' takes (string)", "two one-argument constructors");
            expect("class V { int a; V(int x) { a = x; } } var out = \"\"; try { V v = V(\"s\"); } catch (e) { out += e; } out;",
                   "Cannot pass string to parameter 'x' of 'V' (declared int)", "a single constructor names its parameter");
            expect("class V { int a; V(bool b) { a = 1; } V(int x, int y) { a = 2; } } V v = V(\"s\"); v.a;",
                   "1", "the one constructor the arity admits converts its argument");
            expect("class B { int a; B(int x) { a = x; } B(bool y) { a = 1; } } class D : B { D() : super(\"s\") {} }"
                   " var out = \"\"; try { D d = D(); } catch (e) { out += e; } out;",
                   "No constructor of 'B' takes (string)", "an ancestor initializer");
            expect("class V { int a; V(int x) { a = x; } V(bool b) { a = 1; } V() : this(\"s\") {} }"
                   " var out = \"\"; try { V v = V(); } catch (e) { out += e; } out;",
                   "No constructor of 'V' takes (string)", "a delegation");
        });

        test("a_string_never_makes_a_bool_overload_viable", [this]() {
            expect("class M { int m(int a) { return 1; } int m(bool b) { return 2; } } var out = \"\"; try { M().m(\"s\"); } catch (e) { out += e; } out;",
                   "No overload of 'm' takes (string)", "methods at run time");
            expect_load_error("class M { int m(int a) { return 1; } int m(bool b) { return 2; } } M().m(\"s\");",
                              "no overload of 'm' matches this call", "methods at load");
            expect_load_error("class V { int a; V(int x) { a = x; } V(bool b) { a = 1; } } V v = V(\"s\");",
                              "error: no", "constructors at load");
            // a program's free functions of one name are an overload set too
            expect("int f(int a) { return 1; } int f(bool b) { return 2; } var out = \"\"; try { f(\"s\"); } catch (e) { out += e; } out;",
                   "No overload of 'f' takes (string)", "free functions at run time", load::warn_region);
            expect_load_error("void h(bool b) { } h(\"s\");", "argument 1 of 'h': cannot pass 'string' to 'b' declared 'bool': write !x.empty() or an explicit comparison",
                              "a lone bool parameter at load");
            expect("bool h(bool b) { return b; } var s = \"s\"; bool k = s; to_string(k) + to_string(h(s));", "truetrue", "a var's string keeps its truthiness in a lone bool slot", load::strict);
            expect("class M { int m(int a) { return 1; } int m(bool b) { return 2; } } to_string(M().m(true)) + to_string(M().m(3));", "21",
                   "exact overloads still pick", load::strict);
        });

        test("a_builtin_method_read_without_a_call_is_a_load_error", [this]() {
            expect_load_error("array<int> a = [1]; var f = a.push;",
                              "'push' is a method of array; call it with a.push() or wrap the call in a lambda", "an array");
            expect_load_error("string s = \"x\"; var f = s.size;",
                              "'size' is a method of string; call it with s.size() or wrap the call in a lambda", "a string");
            expect_load_error("map<string, int> m = {\"a\": 1}; var f = m.clear;",
                              "'clear' is a method of map; call it with m.clear() or wrap the call in a lambda", "a map");
            expect("array<int> a = [1]; a.push(2); string s = \"ab\"; to_string(a.size()) + to_string(s.size());", "22",
                   "calls stay admitted", load::strict);
            expect_load_error("map<string, int> m = {\"key\": 4}; m.key;", "map has no member 'key'; read the key as [\"key\"]", "a map key read with a dot");
        });

        test("a_using_name_redeclared_spells_its_qualified_name", [this]() {
            for (const auto& [source, text] : std::vector<std::pair<std::string, std::string>>{
                {"namespace g { int a = 1; } using g::a; int a = 2;", "<script>:1:44: error: 'a' names g::a by a using declaration; a variable cannot be declared with that name"},
                {"namespace g { namespace h { int b = 1; } } using g::h::b; int b = 2;", "<script>:1:63: error: 'b' names g::h::b by a using declaration; a variable cannot be declared with that name"}}) {
                std::string failure;
                try { make_engine()->execute(source); } catch (const std::exception& e) { failure = e.what(); }
                check_true(failure.rfind("parse failed:\n" + text, 0) == 0, failure);
            }
        });

        test("checker_and_reference_texts", [this]() {
            expect_load_error("class P { int x; int y; } P p{1, 2, 3};", "error: too many initializers for 'P': it has 2 fields, got 3",
                              "the aggregate checker text is lowercase like the checker's others");
            expect("class P { int x; int y; } var out = \"\"; try { P p = P(1, 2, 3); } catch (e) { out += e; } out;",
                   "Too many initializers for 'P': it has 2 fields, got 3", "the runtime text keeps its capital");
            expect("void inc(int& x) { x += 1; } var y = 2.5; var out = \"\"; try { inc(y); } catch (e) { out += e; } out;",
                   "Cannot pass float to parameter 'x' of 'inc' (declared int&)", "a reference parameter spells its reference");
            expect("int y = 1; int& ry = y; var s = \"s\"; var out = \"\"; try { ry = s; } catch (e) { out += e; } out + to_string(y);",
                   "Cannot assign string to 'ry' (declared int&)1", "a store through a reference names it");
            expect("class P { int x = 1; } var sv = 5; var out = \"\"; try { shared_ptr<P> sp = sv; } catch (e) { out += e; } out;",
                   "Cannot initialize 'sp' (declared shared_ptr<P>) with int", "a shared_ptr declaration names the types");
        });

        test("missing_include_and_import_files_name_the_path", [this]() {
            expect("var out = \"\"; try { import \"nope_missing.jai\"; } catch (e) { out += e + \"|\"; }"
                   " try { include \"nope_missing2.jai\"; } catch (e) { out += e; } out;",
                   "Failed to open import file: nope_missing.jai|Failed to open include file: nope_missing2.jai", "both forms");
        });

        test("the_step_budget_ends_a_host_entry_without_the_clock", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = fresh(use_vm);
                e->execution_step_budget(4);
                check_eq(uint64_t(4), e->execution_step_budget());
                const std::string looped = outcome(*e, "int n = 0; try { while (true) { n++; } } catch (e) { n = -1; } n;");
                check_true(looped.find("Script execution budget exceeded - raise engine::execution_step_budget or break up the work") != std::string::npos,
                           std::string(use_vm ? "vm" : "interpreter") + ": the spent budget is terminal and names its knob: " + looped);
                check_eq(std::string("3"), outcome(*e, "int k = 0; for (int i = 0; i < 3; i++) { k++; } k;"), "the next entry starts afresh");
                e->execution_step_budget(0);
                check_eq(std::string("100000"), outcome(*e, "int m = 0; for (int i = 0; i < 100000; i++) { m++; } m;"), "zero is unlimited");
            }
        });

        test("a_qualified_base_class_name_parses", [this]() {
            expect("namespace ns { class A { int x = 3; int twice() { return x * 2; } } } class B : ns::A { int y = 4; }"
                   " B b; to_string(b.twice()) + to_string(b.y);", "64", "a script namespace class as a base");
            expect("class W : binary::writer { int marks = 2; } W w; w.marks + 1;", "3",
                   "a host class registered under a qualified name");
        });
    }
};

} // namespace jai::foundry::tests

using doc_audit_fix_tests = jai::foundry::tests::doc_audit_fix_tests;
FOUNDRY_REGISTER(doc_audit_fix_tests)
