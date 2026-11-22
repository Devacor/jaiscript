// const (docs/site/guide/03-value-semantics.html "const: set once, then only read"): declarations, parameters, class constants, C++'s top-level rule (deep through values, stopping at
// handles), the load-time diagnostics and the run-time texts. Every test runs on whichever backend the runner selected
// (--backend=vm for the vm); the interpreter is the oracle.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <memory>
#include <sstream>
#include <string>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

namespace {

struct run_result {
    std::string output;
    std::string error;
};

run_result run(const std::string& source) {
    auto e = make_engine();
    stdlib::register_all(*e);
    auto out = std::make_shared<std::ostringstream>();
    e->set_output_stream(out);
    run_result r;
    try {
        e->execute(source);
    } catch (const std::exception& ex) {
        r.error = ex.what();
    }
    r.output = out->str();
    return r;
}

bool contains(const std::string& haystack, std::string_view needle) { return haystack.find(needle) != std::string::npos; }

#define CHECK_ERROR(source, needle) do { const auto r_ = run(source); check_true(contains(r_.error, needle), std::string(source) + " -> " + r_.error); } while (0)

}   // namespace

class const_tests : public suite {
public:
    const_tests() : suite("Const") {}

    void forge_tests() override {
        // An enum's value read with a dot and called, or a const map's key read so, writes nothing: the text is the call's, not the
        // const rule's, at load and at run time
        test("a_call_of_an_enum_value_or_a_map_key_is_no_write", [this]() {
            CHECK_ERROR("enum Lvl { low, high } print(Lvl.high(1));", "error: cannot call 'high': it is an int, not a function");
            CHECK_ERROR("namespace ns { enum Lvl { low, high } } print(ns::Lvl.high(1));", "error: cannot call 'high': it is an int, not a function");
            CHECK_ERROR("const map<string, int> cm = {\"high\": 2}; print(cm.high(1));", "error: map has no member 'high'; read the key as [\"high\"]");
            const auto lvl = run("enum Lvl { low, high } print(Lvl.high(1));");
            check_false(contains(lvl.error, "is const"), lvl.error);
            const auto at_run = run(R"JS(
                [[warn_mode]];
                enum Lvl { low, high }
                const var cm = {"high": 2};
                try { print(Lvl.high(1)); } catch (e) { print(e); }
                try { print(Lvl::high(1)); } catch (e) { print(e); }
                try { print(cm.high(1)); } catch (e) { print(e); }
                print(Lvl.high, " ", Lvl.size(), " ", cm.size());
            )JS");
            check_eq(std::string(""), at_run.error);
            check_eq(std::string("Cannot call 'high': it is an int, not a function\nCannot call 'high': it is an int, not a function\n"
                                 "map has no member 'high'; read the key as [\"high\"]\n1 2 1\n"), at_run.output);
        });

        // ------------------------------------------------------------------ declarations
        test("const_reads_like_a_variable", [this]() {
            const auto r = run(R"JS(
                const int a = 4;
                const b = a * 2;
                const string s = "hi";
                function f() { const int x = 1; return x + a; }
                print(a, " ", b, " ", s, " ", f());
            )JS");
            check_eq(std::string(""), r.error);
            check_eq(std::string("4 8 hi 5\n"), r.output);
        });

        test("store_to_a_const_is_a_load_error", [this]() {
            CHECK_ERROR("const int a = 1; a = 2;", "cannot assign to const 'a'");
            CHECK_ERROR("function f() { const int a = 1; a += 2; return a; } f();", "cannot assign to const 'a'");
            CHECK_ERROR("function f() { const int a = 1; a++; return a; } f();", "cannot assign to const 'a'");
            CHECK_ERROR("function f() { const int a = 1; --a; return a; } f();", "cannot assign to const 'a'");
        });

        test("store_to_a_const_raises_at_run_time", [this]() {
            const auto r = run(R"JS([[warn_mode]];
                function f() { const int a = 1; try { a = 2; } catch (e) { print("caught: ", e); } return a; }
                print(f());
            )JS");
            check_eq(std::string(""), r.error);
            check_eq(std::string("caught: Cannot assign to 'a': it is read-only\n1\n"), r.output);
        });

        test("const_needs_an_initializer", [this]() {
            CHECK_ERROR("const int a;", "const 'a' needs an initializer");
            CHECK_ERROR("function f() { const x; }", "const 'x' needs an initializer");
            check_eq(std::string(""), run("class P { int x = 3; } const P p; print(p.x);").error);
        });

        test("javascript_spelling_is_const_auto", [this]() {
            const auto r = run("const xs = [1, 2]; xs.push(3);");
            check_true(contains(r.error, "cannot call 'push' on 'xs': 'xs' is const and 'push' changes it"), r.error);
        });

        test("builtin_methods_write_a_const_as_the_reference_states", [this]() {
            // the reference's effect column decides: a reading method loads on a const, a mutator is an error, and a method that
            // hands the elements to a callback reads unless the callback writes through the reference its element binds
            const auto r = run(R"JS(
                const array<int> xs = [1, 2, 3, 4];
                bool big(int x) { return x > 2; }
                int total = 0;
                xs.for_each([&](int x) { total += x; });
                xs.for_each([](int& x) { total += x; });
                print(xs.reduce((a, b) => a + b, 0), " ", xs.some(x => x > 3), " ", xs.every(big), " ", xs.find_index(x => x == 3), " ", xs.find(x => x > 1));
                print(total, " ", array::map(xs, x => x * 2), " ", xs);
                const string s = "abc";
                print(s.is_valid_utf8(), " ", s.to_bytes().size(), " ", s.find("b"));
                class C { }
                const shared_ptr<C> h = new C();
                print(same_object(h, h), " ", h.cpp_ref_count() > 0);
                int sum(array<int>& v) { return v.reduce((a, b) => a + b, 0); }
                print(sum([5, 6]));
            )JS");
            check_eq(std::string(""), r.error);
            check_eq(std::string("10 true false 2 2\n20 [2, 4, 6, 8] [1, 2, 3, 4]\ntrue 3 1\ntrue true\n11\n"), r.output);
            CHECK_ERROR("const array<int> xs = [1, 2]; xs.slice(1);", "cannot call 'slice' on 'xs': 'xs' is const and 'slice' changes it");
            CHECK_ERROR("const array<int> xs = [1, 2]; xs.find([](int& x) { x = 0; return true; });", "'xs' is const and 'find' changes it");
            CHECK_ERROR("const array<int> xs = [1, 2]; xs.for_each([](int& x) { x += 1; });", "'xs' is const and 'for_each' changes it");
            CHECK_ERROR("void zero(int& x) { x = 0; } const array<int> xs = [1, 2]; xs.for_each(zero);", "'xs' is const and 'for_each' changes it");
            CHECK_ERROR("void clear_all(array<int>& v) { v.for_each([](int& x) { x = 0; }); } clear_all([1]);", "'clear_all' writes through parameter 'v'");
            const auto held = run(R"JS(
                const array<int> xs = [1, 2];
                function bump = [](int& x) { x += 1; };
                try { xs.for_each(bump); } catch (e) { print(e); }
                print(xs);
            )JS");
            check_eq(std::string(""), held.error);
            check_eq(std::string("Cannot call 'for_each' on 'xs': 'xs' is const and 'for_each' changes it\n[1, 2]\n"), held.output);
        });

        // ------------------------------------------------------------------ deep through values
        test("a_const_value_is_deeply_read_only", [this]() {
            CHECK_ERROR("const array<int> a = [1, 2]; a[0] = 5;", "cannot assign to 'a[0]': 'a' is const");
            CHECK_ERROR("const map<string, int> m = {\"k\": 1}; m[\"k\"] = 2;", "'m' is const");
            CHECK_ERROR("class P { int x = 0; } const P p; p.x = 1;", "cannot assign to 'p.x': 'p' is const");
            CHECK_ERROR("class P { array<int> xs = []; } const P p; p.xs.push(1);", "'p' is const and 'push' changes it");
            CHECK_ERROR("const string s = \"ab\"; s += \"c\";", "cannot assign to const 's'");
        });

        test("methods_that_write_their_receiver", [this]() {
            const char* cls = R"JS(
                class Counter {
                    int n = 0;
                    void bump() { n += 1; }
                    void bump_twice() { bump(); bump(); }
                    int read() { return n; }
                }
            )JS";
            check_eq(std::string("0\n"), run(std::string(cls) + "const Counter c; print(c.read());").output);
            CHECK_ERROR(std::string(cls) + "const Counter c; c.bump();", "'c' is const and 'bump' writes its receiver");
            CHECK_ERROR(std::string(cls) + "const Counter c; c.bump_twice();", "'bump_twice' writes its receiver");
        });

        test("a_const_handle_keeps_its_pointee_writable", [this]() {
            const auto r = run(R"JS(
                class P { int hp = 1; void hit() { hp -= 1; } }
                const shared_ptr<P> h = new P();
                h.hp = 10;
                h.hit();
                print(h.hp);
            )JS");
            check_eq(std::string(""), r.error);
            check_eq(std::string("9\n"), r.output);
            CHECK_ERROR("class P { } const shared_ptr<P> h = new P(); h = new P();", "cannot assign to const 'h'");
        });

        test("const_var_decides_by_the_held_kind", [this]() {
            const auto handle = run(R"JS(
                class P { int hp = 1; }
                const var h = new P();
                h.hp = 5;
                print(h.hp);
            )JS");
            check_eq(std::string(""), handle.error);
            check_eq(std::string("5\n"), handle.output);
            const auto value = run(R"JS(
                const var a = [1, 2];
                try { a[0] = 5; } catch (e) { print("caught: ", e); }
                print(a[0]);
            )JS");
            check_eq(std::string(""), value.error);
            check_eq(std::string("caught: Cannot assign to 'a[0]': it is read-only ('a' is const)\n1\n"), value.output);
        });

        // ------------------------------------------------------------------ references, casts, returns
        test("references_to_a_const", [this]() {
            check_eq(std::string("3\n"), run("const int a = 3; const int& r = a; print(r);").output);
            CHECK_ERROR("function f() { const int a = 3; int& r = a; }", "cannot bind 'r' (declared int&) to const 'a'");
            CHECK_ERROR("function f() { const int a = 3; auto& r = a; r = 4; }", "cannot assign to const 'r'");
            CHECK_ERROR("function f() { const int a = 3; var x = cast<int&>(a); }", "cannot cast const 'a' to int&");
            CHECK_ERROR("const int a = 3; int& f() { return a; }", "cannot return const 'a' from 'f' by reference");
        });

        test("copies_of_a_const_are_writable", [this]() {
            const auto r = run(R"JS(
                const array<int> a = [1, 2];
                auto b = a;
                b.push(3);
                var c = a;
                c[0] = 9;
                print(to_json(a), " ", to_json(b), " ", to_json(c));
            )JS");
            check_eq(std::string(""), r.error);
            check_eq(std::string("[1,2] [1,2,3] [9,2]\n"), r.output);
        });

        // ------------------------------------------------------------------ parameters
        test("const_reference_parameter_takes_literals", [this]() {
            const auto r = run(R"JS(
                int twice(const int& x) { return x * 2; }
                auto plus = (const int& x) => x + 1;
                print(twice(21), " ", plus(1));
            )JS");
            check_eq(std::string(""), r.error);
            check_eq(std::string("42 2\n"), r.output);
            CHECK_ERROR("void f(const int& x) { x = 2; }", "cannot assign to const 'x'");
            CHECK_ERROR("void f(const int x) { x = 2; }", "cannot assign to const 'x'");
            // a const T& binds a T made from what a T parameter takes (C++); a T& binds only an lvalue of its type
            const auto c = run(R"JS(
                int twice(const int& x) { return x * 2; }
                float half(const float& x) { return x / 2; }
                float f = 2.5; int i = 3; var v = 7; var fv = 7.9;
                print(twice(true), " ", half(3), " ", half(i), " ", f, " ", twice(v));
                try { twice(fv); } catch (e) { print(" ", e); }
            )JS");
            check_eq(std::string(""), c.error);
            check_eq(std::string("2 1.5 1.5 2.5 14\n Cannot pass float to parameter 'x' of 'twice' (declared int&)\n"), c.output);
            CHECK_ERROR("int twice(const int& x) { return x * 2; } float f = 2.5; twice(f);", "argument 1 of 'twice': cannot bind 'float' to 'x' declared 'const int&'");
            CHECK_ERROR("int twice(const int& x) { return x * 2; } twice(\"s\");", "argument 1 of 'twice': cannot bind 'string' to 'x' declared 'const int&'");
            CHECK_ERROR("void bump(int& x) { x += 1; } float f = 1.5; bump(f);", "argument 1 of 'bump': cannot bind 'float' to 'x' declared 'int&'");
        });

        test("a_const_into_a_writing_reference_parameter", [this]() {
            const auto r = run("void inc(int& x) { x += 1; } const int a = 1; inc(a);");
            check_true(contains(r.error, "'inc' writes through parameter 'x'"), r.error);
            check_true(contains(r.error, "'a' is const"), r.error);
            check_eq(std::string("1\n"), run("int get(int& x) { return x; } const int a = 1; print(get(a));").output);
        });

        test("const_is_not_part_of_a_signature", [this]() {
            const auto r = run("class A { void m(int& x) { } void m(const int& x) { } }");
            check_true(contains(r.error, "'m' is already defined with parameters (int&)"), r.error);
            CHECK_ERROR("function f() { } function f() { }", "'f' is already defined");
        });

        // ------------------------------------------------------------------ loops, bindings, captures
        test("range_for_over_a_const", [this]() {
            CHECK_ERROR("function f(array<int> xs) { for (const auto& x : xs) { x = 1; } }", "cannot assign to const 'x'");
            CHECK_ERROR("const array<int> xs = [1]; for (auto& x : xs) { x = 1; }", "cannot assign to const 'x'");
            CHECK_ERROR("const array<int> xs = [1]; for (int& x : xs) { }", "cannot bind 'x' (declared int&) to an element of const 'xs'");
            check_eq(std::string("3\n"), run("const array<int> xs = [1, 2]; int t = 0; for (auto x : xs) { x += 1; t += 0; } for (const auto& x : xs) { t += x; } print(t);").output);
            CHECK_ERROR("const map<string, int> m = {\"a\": 1}; for (auto& [k, v] : m) { v = 2; }", "cannot assign to const 'v'");
            // const alone is const auto, as in a declaration
            check_eq(std::string("3 a1\n"), run("array<int> xs = [1, 2]; int t = 0; for (const x : xs) { t += x; } map<string, int> m = {\"a\": 1}; for (const [k, v] : m) { print(t, \" \", k, v); }").output);
            CHECK_ERROR("array<int> xs = [1]; for (const x : xs) { x = 2; }", "cannot assign to const 'x'");
            CHECK_ERROR("map<string, int> m = {\"a\": 1}; for (const [k, v] : m) { v = 2; }", "cannot assign to const 'v'");
            CHECK_ERROR("map<string, int> m = {\"a\": 1}; for (const auto& [k, v] : m) { v = 2; }", "cannot assign to const 'v'");
            CHECK_ERROR("array<int> xs = [1]; for (const : xs) { }", "a range-for's const variable needs a name: for (const x : xs)");
            // over value elements of a known class a write through the const variable fails the load, whatever spells it
            const std::string monster = "class M { int hp = 10; void hit(int n) { hp -= n; } int left() { return hp; } }\n";
            for (const char* spelled : {"const m", "const auto m", "const auto& m", "const M m", "const M& m"}) {
                const std::string loop = monster + "array<M> pack = [M(), M()]; for (" + spelled + " : pack) ";
                CHECK_ERROR(loop + "{ m.hit(1); }", "static check failed");
                CHECK_ERROR(loop + "{ m.hit(1); }", "cannot call 'hit' on 'm': 'm' is const and 'hit' writes its receiver");
                CHECK_ERROR(loop + "{ m.hp = 1; }", "cannot assign to 'm.hp': 'm' is const");
                check_eq(std::string("1010"), run(loop + "{ print(m.left(), skip_newline); }").output);
            }
            CHECK_ERROR(monster + "map<string, M> named = {\"a\": M()}; for (const [k, v] : named) { v.hit(1); }", "error: cannot call 'hit' on 'v': 'v' is const and 'hit' writes its receiver");
            CHECK_ERROR(monster + "map<string, M> named = {\"a\": M()}; for (const auto& [k, v] : named) { v.hp = 1; }", "error: cannot assign to 'v.hp': 'v' is const");
            // elements the load cannot type are the run's to refuse; a const handle keeps its object writable
            const auto late = run(monster + "var pack = [M()]; for (const m : pack) { m.hit(1); }");
            check_true(contains(late.error, "Cannot call 'hit' on 'm': 'm' is const and 'hit' writes its receiver") && !contains(late.error, "static check failed"), late.error);
            check_eq(std::string("98"), run(monster + "array<shared_ptr<M>> pack = [new M()]; for (const m : pack) { m.hit(1); print(m.hp, skip_newline); }"
                                                      " map<string, shared_ptr<M>> named = {\"a\": pack[0]}; for (const [k, v] : named) { v.hp -= 1; print(v.left(), skip_newline); }").output);
            CHECK_ERROR(monster + "array<shared_ptr<M>> pack = [new M()]; for (const m : pack) { m = null; }", "cannot assign to const 'm'");
        });

        test("captures_keep_constness_like_cpp", [this]() {
            CHECK_ERROR("function f() { const int a = 1; auto g = [a]() { a = 2; }; }", "cannot assign to const 'a'");
            CHECK_ERROR("function f() { const int a = 1; auto g = [&a]() { a = 2; }; }", "cannot assign to const 'a'");
            const auto r = run(R"JS(
                function f() { const int a = 1; auto g = [b = a]() { b += 1; return b; }; return g(); }
                print(f());
            )JS");
            check_eq(std::string(""), r.error);
            check_eq(std::string("2\n"), r.output);
        });

        test("destructuring_and_for_init", [this]() {
            CHECK_ERROR("function f() { const auto [a, b] = [1, 2]; a = 3; }", "cannot assign to const 'a'");
            CHECK_ERROR("function f() { for (const int i = 0; i < 3; i++) { } }", "cannot assign to const 'i'");
            CHECK_ERROR("function f() { const [a, b] = [1, 2]; b = 3; }", "cannot assign to const 'b'");
            CHECK_ERROR("function f() { for (const i = 0; i < 3; i++) { } }", "cannot assign to const 'i'");
            check_eq(std::string("3 0\n"), run("const [a, b] = [1, 2]; int n = 0; for (const start = 0; n < 2; n++) { } print(a + b, \" \", n - 2);").output);
            CHECK_ERROR("class M { int hp = 1; void hit() { hp -= 1; } } array<M> pack = [M(), M()]; const [a, b] = pack; a.hit();",
                        "error: cannot call 'hit' on 'a': 'a' is const and 'hit' writes its receiver");
        });

        // ------------------------------------------------------------------ class constants
        test("class_constants_are_static", [this]() {
            const auto r = run(R"JS(
                class Hero {
                    const int MAX_HP = 100;
                    static const float SPEED = 2.5;
                    int hp = 0;
                    void heal() { hp = MAX_HP; }
                }
                Hero h;
                h.heal();
                print(h.hp, " ", Hero::MAX_HP, " ", Hero::SPEED);
            )JS");
            check_eq(std::string(""), r.error);
            check_eq(std::string("100 100 2.5\n"), r.output);
            CHECK_ERROR("class C { const int K = 1; } C::K = 2;", "cannot assign to const 'C::K'");
            CHECK_ERROR("class C { const int K = 1; void f() { K = 2; } }", "cannot assign to const 'K'");
            CHECK_ERROR("class C { const int K; }", "const 'K' needs an initializer");
        });

        // ------------------------------------------------------------------ program scope
        test("a_rerun_initializes_again", [this]() {
            auto e = make_engine();
            stdlib::register_all(*e);
            auto out = std::make_shared<std::ostringstream>();
            e->set_output_stream(out);
            const std::string src = "const int LIMIT = 7; print(LIMIT);";
            e->execute(src);
            e->execute(src);
            check_eq(std::string("7\n7\n"), out->str());
        });

        test("another_unit_cannot_store_to_a_const", [this]() {
            auto e = make_engine();
            stdlib::register_all(*e);
            e->execute("const int LIMIT = 7;");
            std::string err;
            try { e->execute("LIMIT = 8;"); } catch (const std::exception& ex) { err = ex.what(); }
            check_true(contains(err, "cannot assign to const 'LIMIT'"), err);
            err.clear();
            try { e->add_global("LIMIT", script_value(script_int(9), e.get())); } catch (const std::exception& ex) { err = ex.what(); }
            check_true(contains(err, "Cannot assign to 'LIMIT': it is read-only"), err);
        });

        // ------------------------------------------------------------------ folding and hot reload
        test("a_redefined_class_constant_reaches_every_use", [this]() {
            auto e = make_engine();
            stdlib::register_all(*e);
            e->execute("class C { const int K = 1; } int readk() { return C::K; }");
            check_eq(script_int(1), e->execute("readk();").as_int());
            check_eq(script_int(1), e->execute("readk();").as_int());
            e->execute("class C { const int K = 2; }");
            check_eq(script_int(2), e->execute("readk();").as_int());
            e->execute("class C { static int K = 3; }");   // no longer a const: a mutable static keeps its value, the read is general again
            check_eq(script_int(2), e->execute("readk();").as_int());
            e->execute("C::K = 5;");
            check_eq(script_int(5), e->execute("readk();").as_int());
        });

        test("a_running_frame_sees_the_new_value", [this]() {
            auto e = make_engine();
            stdlib::register_all(*e);
            engine* raw = e.get();
            e->add_function("redefine", [raw](script_int k) {
                raw->execute("class C { const int K = " + std::to_string(k) + "; }");
                return k;
            });
            e->execute(R"JS(
                class C { const int K = 1; }
                int run() { int t = 0; for (int i = 0; i < 4; i++) { t += C::K; if (i == 1) { redefine(10); } } return t; }
            )JS");
            check_eq(script_int(22), e->execute("run();").as_int());
        });

        test("a_reopened_namespace_constant", [this]() {
            auto e = make_engine();
            stdlib::register_all(*e);
            e->execute("namespace cfg { const int W = 3; } int w() { return cfg::W; }");
            check_eq(script_int(3), e->execute("w();").as_int());
            check_eq(script_int(3), e->execute("w();").as_int());
            e->execute("namespace cfg { const int W = 4; }");
            check_eq(script_int(4), e->execute("w();").as_int());
        });

        // ------------------------------------------------------------------ enums
        test("an_enum_name_is_a_const_map", [this]() {
            check_true(contains(run("enum Dir { n, e } Dir = 5;").error, "cannot assign to const 'Dir'"));
            check_true(contains(run("enum Dir { n, e } Dir[\"s\"] = 2;").error, "'Dir' is const"));
            const auto r = run("enum Dir { n, e } int d = Dir::e; d = Dir::n; print(d, \" \", Dir.e, \" \", Dir.size());");
            check_eq(std::string(""), r.error);
            check_eq(std::string("0 1 2\n"), r.output);
        });

        test("a_redefined_enum_reaches_every_use", [this]() {
            auto e = make_engine();
            stdlib::register_all(*e);
            e->execute("enum Fx { fire, poison } int p() { return Fx::poison; } int q() { int t = 0; for (int i = 0; i < 3; i++) { t += Fx.poison; } return t; }");
            check_eq(script_int(1), e->execute("p();").as_int());
            check_eq(script_int(1), e->execute("p();").as_int());
            check_eq(script_int(3), e->execute("q();").as_int());
            e->execute("enum Fx { poison, fire }");
            check_eq(script_int(0), e->execute("p();").as_int());
            check_eq(script_int(0), e->execute("q();").as_int());
        });

        test("namespace_constants", [this]() {
            check_eq(std::string("3\n"), run("namespace cfg { const int W = 3; } print(cfg::W);").output);
            CHECK_ERROR("namespace cfg { const int W = 3; } cfg::W = 4;", "cannot assign to const 'cfg::W'");
        });

        // A write the run time decides (a path through a var, a method on a const var) raises where it stands when the function
        // holding it is called from another function's body: the VM grafts such a callee, and the graft's copy of the body carried
        // no guard, so the const was written. Each case's callee is on line 6; the error names that line and the whole stack.
        test("a_guarded_write_raises_in_a_function_another_function_calls", [this]() {
            const std::string prelude =
                "class Box { var items = [1, 2]; int n = 0; void set(int x) { n = x; } int get() { return n; } }\n"
                "const Box cb;\n"
                "const var cv = Box();\n"
                "const var carr = [1, 2, 3];\n"
                "void report(function body) { try { body(); print(\"ran\"); } catch (e) { print(e, \" @\", e.line, \" #\", e.stack.size()); } }\n";
            const std::string state = " print(cb.items, \" \", cb.n, \" \", cv.items, \" \", cv.n, \" \", carr);\n";
            const std::string untouched = "[1, 2] 0 [1, 2] 0 [1, 2, 3]\n";
            const std::string store = "Cannot assign to 'cv.n': it is read-only ('cv' is const)";
            struct guarded_case { const char* callee; const char* caller; std::string expected; bool warn_region = false; };
            const guarded_case cases[] = {
                {"void w() { cb.items.push(3); }", "void d() { w(); } report([]() { d(); });", "Cannot call 'push' on 'cb.items': 'cb' is const and 'push' writes its receiver @6 #5\n"},
                {"void w() { cv.items.push(9); }", "void d() { w(); } report([]() { d(); });", "Cannot call 'push' on 'cv.items': 'cv' is const and 'push' writes its receiver @6 #5\n"},
                {"void w() { cv.n = 5; }", "void d() { w(); } report([]() { d(); });", store + " @6 #5\n"},
                {"void w() { ++cv.n; }", "void d() { w(); } report([]() { d(); });", store + " @6 #5\n"},
                {"void w() { cv.n++; }", "void d() { w(); } report([]() { d(); });", store + " @6 #5\n"},
                {"void w() { cv.n += 4; }", "void d() { w(); } report([]() { d(); });", store + " @6 #5\n"},
                {"void w() { carr[0] = 9; }", "void d() { w(); } report([]() { d(); });", "Cannot assign to 'carr[0]': it is read-only ('carr' is const) @6 #5\n"},
                {"void w() { cv.set(6); }", "void d() { w(); } report([]() { d(); });", "Cannot call 'set' on 'cv': 'cv' is const and 'set' writes its receiver @6 #5\n"},
                {"void w() { cv.n = 5; } void w2() { w(); }", "void d() { w2(); } report([]() { d(); });", store + " @6 #6\n"},
                {"void w() { cv.n = 5; } void w2() { w(); } void w3() { w2(); }", "void d() { w3(); } report([]() { d(); });", store + " @6 #7\n"},
                {"void w() { for (int i = 0; i < 2; ++i) { cv.n = i + 1; } }", "void d() { w(); } report([]() { d(); });", store + " @6 #5\n"},
                {"void w(bool c) { if (c) { cv.n = 3; } }", "void d() { w(true); } report([]() { d(); });", store + " @6 #5\n"},
                {"void w() { cv.n = 5; }", "void d() { for (int i = 0; i < 3; ++i) { w(); } } report([]() { d(); });", store + " @6 #5\n"},
                {"int w(int a) { cv.n = a; return a + 1; }", "void d() { int r = w(4); print(r); } report([]() { d(); });", store + " @6 #5\n"},
                {"int w(int a) { cv.n = a; return a + 1; }", "void d() { int r = 1 + w(4) * 2; print(r); } report([]() { d(); });", store + " @6 #5\n"},
                {"int w(int a) { cv.n = a; return a + 1; }", "int d() { return w(4); } report([]() { print(d()); });", store + " @6 #5\n"},
                {"int w(int a) { cv.n = a; return a + 1; }", "void d() { if (w(4) > 0) { print(\"then\"); } } report([]() { d(); });", store + " @6 #5\n"},
                {"int w(int a) { cv.n = a; return a + 1; }", "void d() { auto l = (int a) => w(a); print(l(2)); } report([]() { d(); });", store + " @6 #6\n"},
                {"int w() { return cv.get() + 1; }", "void d() { print(w()); } report([]() { d(); });", "1\nran\n"},
                {"class H { void touch() { cv.n = 11; } void via() { touch(); } static void st() { cv.n = 12; } int calc(int a) { cv.n = a; return a * 2; } }",
                 "void d() { H h; h.touch(); } report([]() { d(); });", store + " @6 #5\n"},
                {"class H { void touch() { cv.n = 11; } void via() { touch(); } static void st() { cv.n = 12; } int calc(int a) { cv.n = a; return a * 2; } }",
                 "void d() { H h; h.via(); } report([]() { d(); });", store + " @6 #6\n"},
                {"class H { void touch() { cv.n = 11; } void via() { touch(); } static void st() { cv.n = 12; } int calc(int a) { cv.n = a; return a * 2; } }",
                 "void d() { H::st(); } report([]() { d(); });", store + " @6 #5\n"},
                {"class H { void touch() { cv.n = 11; } void via() { touch(); } static void st() { cv.n = 12; } int calc(int a) { cv.n = a; return a * 2; } }",
                 "void d() { H h; int r = h.calc(3); print(r); } report([]() { d(); });", store + " @6 #5\n"},
                // a warn region (the unit's first statement leads it) lets the load pass and the node raise its text when it is reached
                {"void w() { cb.n = 6; }", "void d() { w(); } report([]() { d(); });", "Cannot assign to 'cb.n': it is read-only ('cb' is const) @6 #5\n", true},
                {"int w(int a) { return a + (cb.n = 7); }", "void d() { print(w(1)); } report([]() { d(); });", "Cannot assign to 'cb.n': it is read-only ('cb' is const) @6 #5\n", true},
                {"void w(bool c) { if (c) { { cb.items.push(4); } } }", "void d() { w(true); } report([]() { d(); });",
                 "Cannot call 'push' on 'cb.items': 'cb' is const and 'push' writes its receiver @6 #5\n", true},
                {"void w() { cast<Box&>(cb).n = 4; }", "void d() { w(); } report([]() { d(); });", "Cannot cast const 'cb' to Box&: cast<Box>(cb) copies it @6 #5\n", true},
                {"void w() { cb.n = 6; } void w2() { w(); } void w3() { w2(); }", "void d() { w3(); } report([]() { d(); });",
                 "Cannot assign to 'cb.n': it is read-only ('cb' is const) @6 #7\n", true},
                {"enum Lvl { low, mid, high } void w() { Lvl.clear(); }", "void d() { w(); } report([]() { d(); });",
                 "Cannot call 'clear' on 'Lvl': 'Lvl' is const and 'clear' changes it @6 #5\n", true},
            };
            std::string failures;
            for (const guarded_case& c : cases) {
                const std::string source = (c.warn_region ? "[[warn_mode]] " : "") + prelude + c.callee + "\n" + c.caller + state;
                const auto r = run(source);
                const std::string want = c.expected + untouched;
                if (!r.error.empty() || r.output != want) {
                    failures += std::string(c.callee) + " / " + c.caller + "\n  expected: " + want + "  got:      " + r.output + (r.error.empty() ? "" : "  error: " + r.error + "\n");
                }
            }
            check_true(failures.empty(), failures);
        });
    }
};

}   // namespace jai::foundry::tests

using const_tests = jai::foundry::tests::const_tests;
FOUNDRY_REGISTER(const_tests)
