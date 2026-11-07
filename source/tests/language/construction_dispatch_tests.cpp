#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <string>

using namespace jai::foundry;

namespace jai::foundry::tests {

// C++ [class.cdtor]: while a constructor or destructor runs, a virtual call on the object dispatches to the class whose
// constructor or destructor is running; once that level's constructor returns, the next level's overrides apply
class construction_dispatch_tests : public suite {
public:
    construction_dispatch_tests() : suite("Construction Dispatch") {}

    static constexpr const char* k_levels = R"(
        string LOG = "";
        class A {
            A() { LOG = LOG + "A:" + describe() + " "; }
            ~A() { LOG = LOG + "~A:" + describe() + " "; }
            string describe() { return "a"; }
        }
        class B : A {
            string s = describe();
            B() { LOG = LOG + "B:" + describe() + "/" + this.describe() + "/" + s + " "; }
            ~B() { LOG = LOG + "~B:" + describe() + " "; }
            string describe() override { return "b"; }
        }
        class C : B {
            string t = describe();
            C() { LOG = LOG + "C:" + describe() + "/" + t + " "; }
            ~C() { LOG = LOG + "~C:" + describe() + " "; }
            string describe() override { return "c"; }
        }
    )";

    void expect_log(const std::string& program, const std::string& expected, const char* what) {
        for (int variant = 0; variant < 2; ++variant) {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            if (variant == 1) {
                eng->type_proofs(false);
                eng->ir_passes(false);
            }
            std::string got;
            try {
                got = eng->execute(program).as_string();
            } catch (const std::exception& e) {
                got = std::string("threw: ") + e.what();
            }
            check_eq(expected, got, std::string(what) + (variant == 0 ? " (default engine)" : " (proofs and passes off)"));
        }
    }

    void forge_tests() override {
        test("each_level_constructs_and_destroys_with_its_own_overrides", [this]() {
            const std::string levels = k_levels;
            expect_log(levels + "function f() -> void { A a; LOG = LOG + \"| \"; } f(); LOG;", "A:a | ~A:a ", "one level");
            expect_log(levels + "function f() -> void { B b; LOG = LOG + \"| \"; } f(); LOG;", "A:a B:b/b/b | ~B:b ~A:a ", "two levels");
            expect_log(levels + "function f() -> void { C c; LOG = LOG + \"| \"; } f(); LOG;", "A:a B:b/b/b C:c/c | ~C:c ~B:b ~A:a ",
                       "three levels: the middle level's constructor sees its own override");
        });

        test("a_constructed_object_dispatches_to_its_most_derived_override", [this]() {
            const std::string levels = k_levels;
            expect_log(levels + "function f() -> string { C c; LOG = \"\"; return c.describe() + c.describe(); } f();", "cc",
                       "after construction");
        });

        test("a_call_through_another_handle_uses_the_class_under_construction", [this]() {
            expect_log(R"(
                string LOG = "";
                void peek(const A& a) { LOG = LOG + a.describe(); }
                class A { A() { peek(this); } ~A() { peek(this); } string describe() { return "a"; } }
                class B : A { B() { peek(this); } ~B() { peek(this); } string describe() override { return "b"; } }
                function f() -> void { B b; LOG = LOG + "|"; }
                f(); LOG;
            )", "ab|ba", "a reference to the object under construction");
        });

        test("a_warm_call_site_in_a_base_constructor_follows_each_object", [this]() {
            expect_log(R"(
                string LOG = "";
                class A { A() { LOG = LOG + describe(); } string describe() { return "a"; } }
                class B : A { string describe() override { return "b"; } }
                function f() -> void {
                    for (int i = 0; i < 40; ++i) { A a; B b; LOG = LOG + b.describe(); }
                }
                f(); LOG;
            )", std::string([] { std::string s; for (int i = 0; i < 40; ++i) { s += "aab"; } return s; }()),
               "the base constructor's call site stays on A while the loop warms it");
        });

        test("a_load_constructs_with_each_levels_overrides", [this]() {
            expect_log(R"(
                string LOG = "";
                class A { int x = 0; A() { LOG = LOG + describe(); } string describe() { return "a"; } }
                class B : A { int y = 0; B() { LOG = LOG + describe(); } string describe() override { return "b"; } }
                B src; src.x = 3; src.y = 4;
                LOG = "";
                B back = from_json(to_json(src));
                LOG + "|" + to_string(back.x) + to_string(back.y);
            )", "ab|34", "from_json runs the zero-argument constructors");
        });

        test("a_hybrid_constructs_its_script_levels_with_their_own_overrides", [this]() {
            struct host_base { int64_t v = 0; };
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            dynamic_binder<host_base>(*eng, "HostBase").constructor<>().property("v", &host_base::v).build();
            std::string got;
            try {
                got = eng->execute(R"(
                    string LOG = "";
                    class S1 : HostBase { S1() : super() { LOG = LOG + describe(); } ~S1() { LOG = LOG + describe(); } string describe() { return "1"; } }
                    class S2 : S1 { S2() { LOG = LOG + describe(); } ~S2() { LOG = LOG + describe(); } string describe() override { return "2"; } }
                    function f() -> void { S2 s; LOG = LOG + "|"; }
                    f(); LOG;
                )").as_string();
            } catch (const std::exception& e) {
                got = std::string("threw: ") + e.what();
            }
            check_eq(std::string("12|21"), got);
        });

        test("super_in_a_base_constructor_names_that_level_s_base", [this]() {
            expect_log(R"(
                string LOG = "";
                class A { string describe() { return "a"; } }
                class B : A { B() { LOG = LOG + super::describe(); } string describe() override { return "b"; } }
                class C : B { string describe() override { return "c"; } }
                function f() -> void { C c; }
                f(); LOG;
            )", "a", "B's constructor on a C object");
        });

        test("extend_runs_each_level_s_field_initializers_with_its_overrides", [this]() {
            expect_log(R"(
                class A { string describe() { return "a"; } }
                class B : A { string s = describe(); string describe() override { return "b"; } }
                class C : B { string t = describe(); string describe() override { return "c"; } }
                A obj;
                extend(obj, C);
                C& c = obj;
                c.s + c.t + c.describe();
            )", "bcc", "extend re-runs B's initializers as B and C's as C");
        });

        test("a_copy_taken_during_a_base_constructor_is_that_base_s_object", [this]() {
            expect_log(R"(
                var KEEP = 0;
                class A { int x = 1; A() { A copy = this; KEEP = copy; } string describe() { return "a"; } }
                class B : A { int y = 2; string describe() override { return "b"; } }
                B b;
                b.x = 5;
                KEEP.describe() + to_string(KEEP.x) + "|" + b.describe();
            )", "a1|b", "the copy is an A");
        });

        test("a_global_function_answers_a_call_no_level_declares", [this]() {
            expect_log(R"(
                string LOG = "";
                string helper() { return "g"; }
                class A { A() { LOG = LOG + helper(); } ~A() { LOG = LOG + helper(); } string go() { return helper(); } }
                class B : A { string helper() { return "h"; } }
                function f() -> void { B b; LOG = LOG + "|" + b.go() + "|"; }
                f(); LOG;
            )", "g|h|g", "construction and destruction reach the global; a constructed object's method reaches its member first");
        });

        test("a_call_only_a_derived_class_declares_is_refused_during_construction_and_destruction", [this]() {
            std::string reported;
            auto run = [this, &reported](const std::string& program) {
                std::string got;
                auto eng = make_engine();
                jai::stdlib::register_all(eng);
                eng->set_script_error_handler([&reported](const std::string& message) { reported += message; });
                try {
                    got = eng->execute("[[warn_mode]]; " + program).as_string();
                } catch (const std::exception& e) {
                    got = std::string("threw: ") + e.what();
                }
                return got;
            };
            const std::string under = "Cannot call 'helper' while 'A' is under construction: only a class derived from it declares it";
            const std::string classes = R"(
                string LOG = "";
                void peek(const A& a) { LOG = LOG + a.helper(); }
                class B : A { string helper() { return "h"; } }
            )";
            check_eq(under, run("class A { A() { LOG = LOG + helper(); } }" + classes + "string r = \"\"; try { B b; } catch (e) { r = e; } r;"), "bare");
            check_eq(under, run("class A { A() { LOG = LOG + this.helper(); } }" + classes + "string r = \"\"; try { B b; } catch (e) { r = e; } r;"), "this.");
            check_eq(under, run("class A { A() { peek(this); } }" + classes + "string r = \"\"; try { B b; } catch (e) { r = e; } r;"), "another handle");
            check_eq(under, run("class A { string s = helper(); }" + classes + "string r = \"\"; try { B b; } catch (e) { r = e; } r;"), "field initializer");
            check_eq(std::string("Cannot call 'helper' while 'B' is under construction: only a class derived from it declares it"),
                     run("class A { } class B : A { B() { helper(); } } class C : B { string helper() { return \"h\"; } }"
                         " string r = \"\"; try { B b; } catch (e) { r = e; } r;"), "the most-derived level's own body");
            run("class A { ~A() { LOG = LOG + helper(); } }" + classes + "function f() -> void { B b; } f(); LOG;");
            check_true(reported.find("Cannot call 'helper' while 'A' is under destruction: only a class derived from it declares it") != std::string::npos,
                       "a destructor's error is reported: " + reported);
            check_eq(std::string("h"), run("class A { string go() { return helper(); } }" + classes + "B b; b.go();"), "a constructed object reaches it");
        });

        test("the_strict_checker_reports_a_derived_only_call_at_load", [this]() {
            auto load = [this](const std::string& program) {
                std::string got;
                auto eng = make_engine();
                jai::stdlib::register_all(eng);
                try {
                    eng->execute(program);
                } catch (const std::exception& e) {
                    got = e.what();
                }
                return got;
            };
            const std::string derived = " class B : A { string helper() { return \"h\"; } }";
            const std::string construction = "Cannot call 'helper' while 'A' is under construction: only a class derived from it declares it";
            const std::string destruction = "Cannot call 'helper' while 'A' is under destruction: only a class derived from it declares it";
            for (const char* body : {"class A { A() { helper(); } }", "class A { A() { this.helper(); } }", "class A { string s = helper(); }"}) {
                const std::string what = load(body + derived);
                check_true(what.find(construction) != std::string::npos, what);
            }
            const std::string dtor = load("class A { ~A() { helper(); } }" + derived);
            check_true(dtor.find(destruction) != std::string::npos, dtor);
            check_eq(std::string(), load("class A { A() { auto later = [this]() { return helper(); }; } }" + derived), "a lambda may run after the constructor");
            check_eq(std::string(), load("class A { string go() { return helper(); } }" + derived), "an ordinary method");
            check_eq(std::string(), load("string helper() { return \"g\"; } class A { A() { helper(); } }" + derived), "a global answers it");
            const std::string nobody = load("class A { A() { helper(); } }");
            check_true(nobody.find("use of undeclared identifier 'helper'") != std::string::npos, nobody);
        });

        test("a_base_that_gained_a_field_on_reload_constructs_and_destroys_with_its_own_overrides", [this]() {
            for (int variant = 0; variant < 2; ++variant) {
                auto eng = make_engine();
                jai::stdlib::register_all(eng);
                if (variant == 1) {
                    eng->type_proofs(false);
                    eng->ir_passes(false);
                }
                std::string got;
                try {
                    eng->execute(R"(
                        string LOG = "";
                        class A { int a = 1; A() { } ~A() { } string describe() { return "a"; } }
                        class B : A { int b = 2; B() { } string describe() override { return "b"; } }
                        function warm() -> void { B first; }
                        warm();
                    )");
                    eng->execute(R"(class A {
                        int a = 1; string tag = "t"; int a2 = 7;
                        A() { LOG = LOG + describe() + tag + to_string(a2) + to_string(a); }
                        ~A() { LOG = LOG + describe() + to_string(a2); }
                        string describe() { return "a"; }
                    })");
                    got = eng->execute(R"(
                        function f() -> void {
                            B second;
                            second.b = 5;
                            LOG = LOG + "|" + to_string(second.a) + to_string(second.a2) + to_string(second.b) + second.describe() + second.tag + "|";
                        }
                        f(); LOG;
                    )").as_string();
                } catch (const std::exception& e) {
                    got = std::string("threw: ") + e.what();
                }
                check_eq(std::string("at71|175bt|a7"), got, variant == 0 ? "default engine" : "proofs and passes off");
            }
        });

        test("a_second_base_initializes_and_destroys_with_its_own_overrides", [this]() {
            expect_log(R"(
                string LOG = "";
                class A { int a = 1; }
                class C { int c = 2; string s = describe(); ~C() { LOG = LOG + describe() + to_string(c) + s; } string describe() { return "c"; } }
                class D : A, C { int d = 3; string describe() override { return "d"; } }
                function f() -> void { D o; LOG = LOG + o.describe() + to_string(o.a) + to_string(o.c) + to_string(o.d) + o.s + "|"; }
                f(); LOG;
            )", "d123c|c2c", "C's layout is not a prefix of D's");
        });

        test("a_delegating_constructor_a_derived_class_reaches_runs_its_target", [this]() {
            // C++ delegation holds at every level: Part(int) reached through super(k) first runs Part(int, string), its bases and
            // members, then its own body (the target once never ran, leaving the members their initializers)
            expect_log(R"(
                string LOG = "";
                class Root { Root() { LOG = LOG + "r"; } }
                class Part : Root {
                    string s = "init";
                    Part(int k) : this(k, "two") { s = s + "+one"; LOG = LOG + "p1"; }
                    Part(int k, string n) : super() { s = n + to_string(k); LOG = LOG + "p2"; }
                }
                class Unit : Part { string u = "u"; Unit(int k) : super(k) { u = u + s; LOG = LOG + "u"; } }
                Unit one(1);
                Part direct(2);
                LOG + "|" + one.u + "|" + direct.s;
            )", "rp2p1urp2p1|utwo1+one|two2+one", "the delegated-to constructor runs ahead of the delegating body");
        });
    }
};

}

using construction_dispatch_tests = jai::foundry::tests::construction_dispatch_tests;
FOUNDRY_REGISTER(construction_dispatch_tests)
