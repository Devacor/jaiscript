#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"
#include <filesystem>
#include <fstream>
#include <tuple>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

namespace {

void check_stdlib_ints(const std::vector<std::pair<std::string, int64_t>>& a_cases) {
    check_each(a_cases, [](const auto& c) {
        auto eng = make_engine();
        jai::stdlib::register_all(*eng);
        check_eq(c.second, eng->execute(c.first).as_int());
    });
}

}

class namespace_tests : public suite {
public:
    namespace_tests() : suite("Namespace Tests") {}

    void forge_tests() override {
        // using ns::name; (a comma list): the rest of the unit's text reads the name as its qualified spelling (parse
        // time, file-local, not transitive, any namespace; the name cannot be declared again)
        test("using_declarations", [&]() {
            const std::string utils = "namespace utils { auto add(a, b) { return a + b; } int LIMIT = 7; }\n";
            for (bool use_vm : {false, true}) {
                auto run = [&](const std::string& src) -> std::string {
                    auto eng = make_engine();
                    eng->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
                    stdlib::register_all(eng);
                    try { return eng->execute(src).to_string(); } catch (const std::exception& e) { return std::string("ERROR: ") + e.what(); }
                };
                const char* be = use_vm ? " (vm)" : " (interpreter)";
                check_eq(std::string("8"), run(utils + "using utils::add, utils::LIMIT; add(LIMIT, 1)"), std::string("a function and a variable") + be);
                check_eq(std::string("14.0math::vec3"), run("using math::vec3, math::dot; vec3 a = vec3(1, 2, 3); to_string(dot(a, a)) + type_of(a)"), std::string("math types") + be);
                check_eq(std::string("4.0"), run("using math::vec2; function f(vec2 p) -> vec2 { return p * 2; } to_string(f(vec2(1, 2)).y)"), std::string("types in signatures") + be);
                check_eq(std::string("7"), run(utils + "class C { int add = 5; } using utils::add; var c = C(); c.add + add(1, 1)"), std::string("a member name after '.' stays") + be);
                check_eq(std::string("x7"), run(utils + "using utils::LIMIT; `x${LIMIT}`"), std::string("a template string's splice") + be);
                check_eq(std::string("3"), run(utils + "function g() -> int { using utils::add; return add(1, 2); } g()"), std::string("inside a function") + be);
                check(run(utils + "using utils::add; int add = 3;").find("by a using declaration") != std::string::npos, std::string("the name cannot be declared again") + be);
                check(run(utils + "using utils::add; function add() {}").find("by a using declaration") != std::string::npos, std::string("nor as a function") + be);
                check(run("using vec3;").find("names no namespace") != std::string::npos, std::string("a using declaration takes a qualified name") + be);
                check(run(utils + "using utils::add; using utils::add;").find("already named by a using declaration") != std::string::npos, std::string("one name, one using") + be);
                // file-local: a later string of the same engine does not see it
                auto eng = make_engine();
                eng->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
                eng->execute(utils + "using utils::add; var first = add(1, 1);");
                bool leaked = true;
                try { eng->execute("add(1, 1)"); } catch (const std::exception&) { leaked = false; }
                check(!leaked, std::string("not transitive across units") + be);
                // an included file neither inherits the includer's using nor passes its own on
                const auto dir = jai::foundry::test_temp_directory();
                { std::ofstream(dir / "using_inc_a.jai") << "var from_a = add(2, 3);\n"; }
                { std::ofstream(dir / "using_inc_b.jai") << "using utils::LIMIT;\nvar from_b = LIMIT;\n"; }
                auto inc = make_engine();
                inc->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
                inc->add_include_path(dir.string());
                bool inherited = true;
                try { inc->execute(utils + "using utils::add; include \"using_inc_a.jai\";"); } catch (const std::exception&) { inherited = false; }
                check(!inherited, std::string("an included file does not inherit the includer's using") + be);
                auto inc2 = make_engine();
                inc2->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
                inc2->add_include_path(dir.string());
                check_eq((int64_t)7, inc2->execute(utils + "include \"using_inc_b.jai\"; from_b").as_int(), std::string("the included file's own using applies to its text") + be);
                auto inc3 = make_engine();
                inc3->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
                inc3->add_include_path(dir.string());
                bool passed_on = true;
                try { inc3->execute(utils + "include \"using_inc_b.jai\"; LIMIT"); } catch (const std::exception&) { passed_on = false; }
                check(!passed_on, std::string("and does not reach the includer") + be);
                std::filesystem::remove(dir / "using_inc_a.jai");
                std::filesystem::remove(dir / "using_inc_b.jai");
            }
        });

        test("namespace_members_resolve_by_their_qualified_names", [this]() {
            check_results<int>({
                {R"(
                namespace utils {
                    auto add(a, b) {
                        return a + b;
                    }
                }
                utils::add(10, 20)
            )", 30},
                {R"(
                namespace math {
                    auto square(x) {
                        return x * x;
                    }
                    auto square(x, y) {
                        return x * x + y * y;
                    }
                }
                auto r1 = math::square(5);
                auto r2 = math::square(3, 4);
                r1 + r2
            )", 50},
                {R"(
                namespace config {
                    auto MAX_SIZE = 100;
                    auto MIN_SIZE = 10;
                }
                config::MAX_SIZE + config::MIN_SIZE
            )", 110},
                {R"(
                namespace shapes {
                    class Rectangle {
                    public:
                        int width;
                        int height;

                        Rectangle(w, h) {
                            width = w;
                            height = h;
                        }

                        auto area() {
                            return width * height;
                        }
                    };
                }
                auto rect = shapes::Rectangle(5, 10);
                rect.area()
            )", 50},
                {R"(
                namespace outer {
                    namespace inner {
                        auto getValue() {
                            return 42;
                        }
                    }
                }
                outer::inner::getValue()
            )", 42},
                {R"(
                namespace my::nested::deep {
                    auto getValue() {
                        return 99;
                    }
                }
                my::nested::deep::getValue()
            )", 99},
                {R"(
                namespace mylib {
                    auto VERSION = 1;

                    auto getVersion() {
                        return VERSION;
                    }

                    class Info {
                    public:
                        auto describe() {
                            return 1;
                        }
                    };
                }

                auto v1 = mylib::VERSION;
                auto v2 = mylib::getVersion();
                auto info = mylib::Info();
                auto v3 = info.describe();

                v1 + v2 + v3
            )", 3},
                {R"(
                namespace test {
                    auto func(x) { return x; }
                    auto func(x) override { return x * 2; }
                }
                test::func(5)
            )", 10},
                {R"(
                namespace string {
                    auto length(s) {
                        return 42;
                    }
                }
                string::length("test")
            )", 42},
            });
        });

        test("namespace_redeclarations_unknown_members_and_wrong_arity_throw", [&]() {
            const std::tuple<std::string> sources[] = {
                {R"(
                    namespace test {
                        auto func(x) { return x; }
                        auto func(x) { return x * 2; }
                    }
                )"},
                {R"(
                    namespace test {
                        auto func() { return 1; }
                    }
                    test::nonexistent()
                )"},
                {R"(
                    namespace math {
                        auto add(a, b) { return a + b; }
                    }
                    math::add(1, 2, 3)
                )"},
            };
            check_each(sources, [](const auto& source) {
                auto eng = make_engine();
                check_throws([&]() { eng->execute(std::get<0>(source)); });
            });
        });

        test("namespace_complex_members", [&]() {
            auto eng = make_engine();

            // Test class with static method, instance method, and global variable
            eng->execute(R"(
                namespace my::nested {
                    class cat {
                    public:
                        static bool meow() {
                            return true;
                        }

                        bool climb() {
                            return true;
                        }
                    };

                    cat GLOBAL_CAT = cat();
                }
            )");

            // Test static method access
            auto result1 = eng->execute("my::nested::cat::meow()");
            check_eq(result1.as<bool>(), true);

            // Test global variable instance method access
            auto result2 = eng->execute("my::nested::GLOBAL_CAT.climb()");
            check_eq(result2.as<bool>(), true);
        });

        test("namespace_overrides_class_static", [&]() {
            auto eng = make_engine();
            eng->execute(R"(
                class cat {
                public:
                    static auto meow() { return 1; }
                    static auto climb() { return 1; }
                    static auto meow(a) { return 5; }
                };

                namespace cat {
                    auto meow() override { return 2; }
                    auto dance() { return 2; }
                    auto meow(a, b) { return 10; }
                }
            )");

            // Namespace override of static method (0 params)
            auto r1 = eng->execute("cat::meow()");
            check_eq(r1.as<int>(), 2);

            // Class static method (1 param - no namespace override)
            auto r2 = eng->execute("cat::meow(1)");
            check_eq(r2.as<int>(), 5);

            // Namespace function (2 params)
            auto r3 = eng->execute("cat::meow(1, 2)");
            check_eq(r3.as<int>(), 10);

            // Class static method (no namespace override)
            auto r4 = eng->execute("cat::climb()");
            check_eq(r4.as<int>(), 1);

            // Namespace-only function
            auto r5 = eng->execute("cat::dance()");
            check_eq(r5.as<int>(), 2);
        });

        test("namespace_override_before_class", [&]() {
            auto eng = make_engine();
            // Define namespace with override BEFORE the class exists
            // The override keyword should still work when class is defined later
            eng->execute(R"(
                namespace dog {
                    auto bark() override { return 100; }
                    auto wag() { return 200; }
                }

                class dog {
                public:
                    static auto bark() { return 1; }
                    static auto sit() { return 2; }
                };
            )");

            // Namespace override should take precedence (defined before class)
            auto r1 = eng->execute("dog::bark()");
            check_eq(r1.as<int>(), 100);

            // Class static method (no namespace override)
            auto r2 = eng->execute("dog::sit()");
            check_eq(r2.as<int>(), 2);

            // Namespace-only function
            auto r3 = eng->execute("dog::wag()");
            check_eq(r3.as<int>(), 200);
        });
        // A namespace is a scope; its variables are cells (C++). Stores through ns::x, writes from the
        // namespace's own functions, initializers over earlier members, reopening in place, host entries read-only.
        test("namespace_variables_store_through_every_path_and_reopen_in_place", [this]() {
            check_stdlib_results<std::string>({
                {R"(
                namespace ns { var x = 3; int y = 4; }
                ns::x = 10; ns::x += 5; ++ns::x;
                ns::y = 7; ns::y++;
                auto r = (ns::x = 20);
                to_string(ns::x) + " " + to_string(ns::y) + " " + to_string(r)
            )", "20 8 20"},
                {R"(
                namespace ns {
                    var x = 3;
                    function set(v) { x = v; }
                    function get() { return x; }
                    function bump() { x += 1; return x; }
                }
                ns::set(42);
                var a = to_string(ns::x) + " " + to_string(ns::get());
                ns::x = 5;
                a + " " + to_string(ns::get()) + " " + to_string(ns::bump()) + " " + to_string(ns::x)
            )", "42 42 5 6 6"},
                {R"(
                namespace ns { var x = 3; function get() { return x; } }
                namespace ns { var x = 30; }
                to_string(ns::x) + " " + to_string(ns::get())
            )", "30 30"},
                {R"(
                namespace ns { int y = 4; }
                int y = 4;
                function s(var flag) { if (flag) { return "s"; } return 1; }
                var out = ""; var out2 = "";
                try { ns::y = s(true); out = "stored"; } catch (e) { out = e; }
                try { y = s(true); out2 = "stored"; } catch (e) { out2 = e; }
                var fv = 7.9; var out3 = ""; var out4 = "";
                try { ns::y = fv; out3 = "stored"; } catch (e) { out3 = e; }
                try { y = fv; out4 = "stored"; } catch (e) { out4 = e; }
                var bv = true;
                ns::y = bv; y = bv;
                (out == out2 && out != "stored" && out3 == out4 && out3 != "stored" && to_string(ns::y) == "1" && to_string(y) == "1")
                    ? "same"
                    : (out + " | " + out2 + " | " + out3 + " | " + out4 + " | " + to_string(ns::y) + " | " + to_string(y))
            )", "same"},
            });
        });

        test("namespace_initializers_nested_stores_and_member_calls_read_the_cells", [this]() {
            check_stdlib_ints({
                {R"(
                namespace ns { var x = 3; var y = x + 1; int z = y * 2; }
                ns::y * 100 + ns::z
            )", 408},
                {R"(
                namespace outer::inner { var z = 5; function get() { return z; } }
                outer::inner::z = 6;
                outer::inner::z += 1;
                outer::inner::z * 10 + outer::inner::get()
            )", 77},
                {R"(
                namespace ns { var twice = function(a) { return a * 2; }; }
                ns::twice(4)
            )", 8},
                {R"(
                namespace ns { function b() { return 11; } function a() { return ns::b(); } }
                ns::a()
            )", 11},
            });
        });

        test("host_registered_namespace_entry_is_read_only", [&]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            auto& ns = detail::engine_access::script_namespaces(*eng)[detail::engine_access::symbolize(*eng, "cfg")];
            if (!ns) { ns = std::make_shared<script_namespace_data>(); }
            ns->variables[detail::engine_access::symbolize(*eng, "host")] = script_value(static_cast<script_int>(5), eng.get());
            auto result = eng->execute(R"(
                namespace cfg { var own = 1; function peek() { return cfg::host * 10 + own; } }
                var out = to_string(cfg::host) + " " + to_string(cfg::peek());
                try { cfg::host = 1; out = out + " stored"; } catch (e) { out = out + " " + e; }
                out
            )");
            check_eq(std::string("5 51 Cannot assign to 'host': it is read-only"), result.as<std::string>());
        });

        // C++ [basic.lookup.unqual]: a bare name in a namespace's code finds the namespace's members, then the enclosing
        // namespaces' (innermost first), then the globals; ::name reaches the global
        test("namespace_code_finds_bare_names_in_its_namespaces_innermost_first", [this]() {
            check_stdlib_results<std::string>({
                {R"(
                int a() { return 100; }
                int k = 1000;
                namespace ns {
                    int k = 5;
                    int b() { return a() + k; }
                    int a() { return 1; }
                    int c() { return ::a() + ::k; }
                    auto mk() { return [](int x) { return a() + x; }; }
                    int sq(int v) { return v * v; }
                    int sq(int v, int w) { return v * w; }
                    int pick() { return sq(3) + sq(2, 5); }
                    coroutine int twice() { yield a(); yield a() + 1; }
                    int drive() { int total = 0; for (var v : twice()) { total += v; } return total; }
                }
                `${ns::b()} ${ns::c()} ${ns::mk()(10)} ${ns::pick()} ${ns::drive()} ${a()} ${k}`
            )", "6 1100 11 19 3 100 1000"},
                {R"(
                int depth = 0;
                namespace outer {
                    int depth = 1;
                    int f() { return 10; }
                    namespace inner {
                        int depth = 2;
                        int g() { return f() + depth; }
                    }
                }
                namespace outer::inner::deep { int h() { return f() * depth; } }
                namespace outer { int f2() { return depth; } }
                `${outer::inner::g()} ${outer::inner::deep::h()} ${outer::f2()} ${depth}`
            )", "12 20 1 0"},
                {R"(
                namespace shop {
                    int tax = 3;
                    int price(int v) { return v + tax; }
                    class Item {
                        int base = price(10);
                        int cost() { return price(base); }
                        static int listed() { return price(1); }
                    }
                    class Crate {
                        int n = 0;
                        Crate(int k) { n = price(k); }
                    }
                }
                auto item = shop::Item();
                `${item.base} ${item.cost()} ${shop::Item::listed()} ${shop::Crate(2).n}`
            )", "13 16 4 5"},
            });
        });

        test("a_namespace_reload_rebinds_its_bare_calls", [&]() {
            const auto dir = jai::foundry::test_temp_directory();
            const auto module = dir / "ns_reload_module.jai";
            { std::ofstream(module) << "namespace mod { int v() { return 1; } int get() { return v(); } }\n"; }
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->add_include_path(dir.string());
            check_eq((int64_t)1, eng->execute("import \"ns_reload_module.jai\"; function reload() -> bool { return import(\"ns_reload_module.jai\"); } mod::get()").as_int());
            { std::ofstream(module) << "namespace mod { int v() { return 22; } int get() { return v(); } int more() { return v() + 1; } }\n"; }
            check_true(eng->execute("reload();").as_bool(), "the touched module re-ran");
            check_eq(std::string("22 23"), eng->execute("`${mod::get()} ${mod::more()}`;").as<std::string>());
            eng->execute("namespace mod { int v() override { return 5; } int v(int x) { return x; } int both() { return v() + v(30); } }");
            check_eq(std::string("5 35"), eng->execute("`${mod::get()} ${mod::both()}`;").as<std::string>());
            std::filesystem::remove(module);
        });

        test("a_qualified_host_registration_is_a_namespace_member", [&]() {
            // add_global("game::tick") and add_function("game::spawn") register into namespace game, as the library's
            // namespaces do: game::tick reads them, the namespace's code reads them bare, and script cannot assign them
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            int64_t host_tick = 41;
            eng->add_global_ref("game::tick", host_tick);
            eng->add_global("game::title", std::string("demo"));
            eng->add_function("game::spawn", [](int64_t n) -> int64_t { return n * 2; });
            eng->add_function("game::spawn", [](int64_t n, int64_t m) -> int64_t { return n * m; });
            host_tick = 42;
            check_true(eng->has_variable("game::tick") && eng->has_function("game::spawn"), "the host API still names the qualified global");
            auto result = eng->execute(R"(
                extern game::tick;
                namespace game {
                    int next() { return tick + spawn(1) + spawn(2, 3); }
                    class Clock { int read() { return tick + 1; } }
                }
                var out = `${game::tick} ${game::title} ${game::spawn(5)} ${game::next()} ${game::Clock().read()}`;
                try { game::tick = 1; } catch (e) { out = out + " | " + e; }
                out
            )");
            check_eq(std::string("42 demo 10 50 43 | Cannot assign to 'tick': it is read-only"), result.as<std::string>());
            bool refused = false;
            try { eng->execute("print(game::tock);"); } catch (const static_check_error& err) { refused = std::string(err.what()).find("no member 'tock' in namespace 'game'") != std::string::npos; }
            check_true(refused, "a member the host did not register");
            auto bare = eng->check("namespace game { void poke() { tick = 9; } }");
            check_true(bare.error_count() == 1 && bare.format().find("cannot assign to const 'tick'") != std::string::npos, "a bare store at load: " + bare.format());
            auto lenient = eng->execute("[[warn_mode]]; namespace game { void poke() { tick = 9; } } var o = \"\"; try { game::poke(); } catch (e) { o = e; } o");
            check_eq(std::string("Cannot assign to 'tick': it is read-only"), lenient.as<std::string>());
            check_eq((int64_t)42, host_tick);
        });

        test("namespace_variable_shared_by_every_unit", [&]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->execute("namespace ns { var x = 1; function get() { return x; } }");
            eng->execute("var x = 100; ns::x = 2;");
            auto result = eng->execute("to_string(x) + \" \" + to_string(ns::x) + \" \" + to_string(ns::get())");
            check_eq(std::string("100 2 2"), result.as<std::string>());
            // a unit that declares the global again versions the global's cell, never the namespace's own
            auto again = eng->execute("var x = 300; to_string(x) + \" \" + to_string(ns::x) + \" \" + to_string(ns::get())");
            check_eq(std::string("300 2 2"), again.as<std::string>());
        });

        test("a_namespace_functions_frame_holds_its_locals_and_its_temporaries_apart", [&]() {
            // the vm places a body's temporaries above its declared locals: a namespace function's frame is its declaration's
            // size, so a loop bound read from a parameter stays the parameter
            for (bool proofs : {true, false}) {
                auto eng = make_engine();
                jai::stdlib::register_all(*eng);
                eng->type_proofs(proofs);
                auto result = eng->execute(R"(
                    namespace qq {
                        int loop(array<string> pattern) {
                            int n = 0;
                            for (int i = 0; i < pattern.size(); i++) { n += 1; }
                            return n;
                        }
                        string joined(array<string> parts, string sep) {
                            string out = "";
                            for (int i = 0; i < parts.size(); i++) { out += (i > 0 ? sep : "") + parts[i] + to_string(i * 2 + parts.size()); }
                            return out;
                        }
                    }
                    to_string(qq::loop(["a"])) + " " + to_string(qq::loop(["a", "b", "c"])) + " " + qq::joined(["x", "y"], "-")
                )");
                check_eq(std::string("1 3 x2-y4"), result.to_string(), proofs ? "proofs on" : "proofs off");
            }
        });

        test("namespace_variable_binds_a_reference_by_its_qualified_name", [&]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            auto result = eng->execute(R"(
                namespace ns { var x = 1; }
                function take(var& a) { a = 77; }
                var out = "";
                try { var& r = ns::x; r = 5; out = "bound:" + to_string(ns::x); } catch (e) { out = e; }
                var out2 = "";
                try { take(ns::x); out2 = "passed"; } catch (e) { out2 = e; }
                out + "|" + out2 + "|" + to_string(ns::x)
            )");
            const std::string text = result.as<std::string>();
            check_eq(std::string("bound:5|passed|77"), text);
        });

    }
};

}

FOUNDRY_REGISTER(jai::foundry::tests::namespace_tests)
