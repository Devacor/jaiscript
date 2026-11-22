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

// A program's files in a directory of their own, and engines that find them there
struct program_files {
    std::filesystem::path dir;
    explicit program_files(const std::string& a_name) : dir(jai::foundry::test_temp_directory() / ("namespace_program_" + a_name)) {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }
    ~program_files() {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }
    void write(const std::string& a_file, const std::string& a_text) const {
        std::ofstream(dir / a_file, std::ios::binary | std::ios::trunc) << a_text;
    }
    std::shared_ptr<jai::engine> engine(bool a_cache = false) const {
        auto result = make_engine();
        jai::stdlib::register_all(*result);
        result->jaibite_cache(a_cache);
        result->add_include_path(dir.string());
        return result;
    }
};

// the source's last value as text, or what its load or run raised
std::string outcome(jai::engine& a_engine, const std::string& a_source) {
    try {
        return a_engine.execute(a_source).to_string();
    } catch (const std::exception& e) {
        return std::string(e.what());
    }
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
                check_eq(std::string("14.0math::vec3"), run("using math::vec3, math::dot; vec3 a = vec3(1, 2, 3); to_string(dot(a, a)) + reflect::type_of(a)"), std::string("math types") + be);
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

        // Every namespace block of a program adds to the one namespace, so a bare name in a namespace's code is the member any
        // file of the program declares, as a type and as a value, whichever file is imported first
        test("a_namespaces_code_finds_bare_the_members_another_file_declares", [&]() {
            const std::string declaring = R"(
                int base = 1000;
                class Item { int price = 500; }
                namespace shop {
                    class Item { int price = 3; static int count = 2; Item() {} Item(int p) { price = p; } static int made() { return 7; } }
                    class Special : Item { int extra = 1; }
                    const int LIMIT = 4;
                    int tax(int p) { return p + 1; }
                    int tax(int p, int q) { return p + q; }
                    int base = 10;
                    namespace inner { int k() { return 5; } class Deep { int d = 9; } }
                }
            )";
            const std::string naming = R"(
                namespace shop {
                    int total() { Item i; return tax(i.price) + base; }
                    int consts() { return LIMIT + tax(1, 2); }
                    int statics() { return Item::count + Item::made(); }
                    int built() { return Item(8).price; }
                    int listed() { array<Item> a = [Item(1), Item(2)]; return a.size(); }
                    int handles() { auto h = make_shared<Item>(4); shared_ptr<Item> g = new Item(5); return h.price + g.price; }
                    int casts() { Special s; Item& i = cast<Item&>(s); return i.price; }
                    int param(Item it) { return it.price; }
                    class Crate { Item held; int both() { return held.price + tax(1); } }
                    void restock() { base = 20; }
                    namespace inner { int deep() { Deep d; Item i; return d.d + i.price; } }
                }
                namespace shop::inner { int up() { return tax(1) + base + k(); } }
                string report() {
                    string before = `${shop::total()} ${shop::consts()} ${shop::statics()} ${shop::built()} ${shop::listed()} ${shop::handles()} ${shop::casts()} ${shop::param(shop::Item(6))} ${shop::Crate().both()} ${shop::inner::deep()}`;
                    shop::restock();
                    return before + ` ${shop::base} ${shop::inner::up()} ${base} ${Item().price}`;
                }
            )";
            const std::string expected = "14 7 9 8 2 9 3 6 5 12 20 27 1000 500";
            program_files files("members");
            files.write("declaring.jai", declaring);
            files.write("naming.jai", naming);
            files.write("naming_imports.jai", "import \"declaring.jai\";\n" + naming);
            files.write("naming_subclass.jai", "namespace shop { class Crate : Item { int both() { return price + tax(1); } } int crate() { return Crate().both(); } }");
            files.write("naming_early.jai", "namespace shop { int early = tax(1); }");
            files.write("declaring_again.jai", "namespace shop { int tax(int p) { return 0; } }");
            files.write("base_again.jai", "namespace shop { int base = 77; int peek() { return base; } }");
            files.write("item_again.jai", "namespace shop { class Item { int price = 99; } int peek() { Item i; return i.price; } }");
            files.write("naming_hidden.jai", "string base = \"global\"; namespace shop { int hidden() { return tax(1) + base; } }");
            files.write("naming_using.jai", "using shop::Item, shop::tax, shop::base;\nint outside() { Item i; return tax(i.price) + base; }\nnamespace other { int inside() { Item i; return tax(i.price, 1); } }");
            check_each(std::vector<std::pair<std::string, std::string>>{
                {"include \"declaring.jai\"; include \"naming.jai\"; report()", expected},   // one unit: what every program below must match
                {"import \"declaring.jai\"; import \"naming.jai\"; report()", expected},
                {"import \"naming.jai\"; import \"declaring.jai\"; report()", expected},
                {"import \"naming_imports.jai\"; report()", expected},
                {"void load() { import \"declaring.jai\"; }\n" + naming + "\nload(); report()", expected},
                {"import \"declaring.jai\"; import \"naming_subclass.jai\"; shop::crate()", "5"},
                {"import \"naming_using.jai\"; import \"declaring.jai\"; `${outside()} ${other::inside()}`", "14 4"},
                {"import \"naming_hidden.jai\"; import \"declaring.jai\"; shop::hidden()", "12"},   // the namespace's int hides the unit's own global string
                // top-level code still runs in import order: the name resolves at load, its declaration has not run
                {"import \"naming_subclass.jai\"; import \"declaring.jai\"; shop::crate()", "Base class 'shop::Item' not found"},
                {"import \"naming_early.jai\"; import \"declaring.jai\"; shop::early", "Undefined variable 'tax'"},
                // one name declared by two files of the namespace is the redefinition it is in one file's two blocks
                {"import \"declaring.jai\"; import \"declaring_again.jai\"; shop::tax(1)", "Function 'tax' already exists in namespace 'shop'. Use 'override' keyword to replace it."},
                // a variable two files declare is the one variable, initialized again by the file that runs later; a class is replaced
                {"import \"declaring.jai\"; import \"base_again.jai\"; `${shop::base} ${shop::peek()}`", "77 77"},
                {"import \"base_again.jai\"; import \"declaring.jai\"; `${shop::base} ${shop::peek()}`", "10 10"},
                {"import \"declaring.jai\"; import \"item_again.jai\"; `${shop::peek()} ${shop::Item().price}`", "99 99"},
            }, [&](const auto& c) {
                auto eng = files.engine();
                const std::string got = outcome(*eng, c.first);
                check_eq(c.second, got.substr(0, got.find('\n')), c.first);
            });
        });

        // A bare name that resolves to a namespace's member is checked as its qualified spelling is, wherever the member is
        // declared: above the use, below it, in another block of the namespace or in another file of the program
        test("a_bare_namespace_member_checks_as_its_qualified_spelling", [&]() {
            const std::string members = "int tax(int p) { return p + 1; } int rate(int a, int b = 2) { return a + b; } int sq(int v) { return v; } "
                "int sq(int v, int w) { return v * w; } [[nodiscard]] int fee() { return 1; } coroutine int twice() { yield 1; yield 2; } int base = 10; ";
            // {the namespace's code, what the load says of it}: the same code names each member bare and as shop::member
            const std::vector<std::pair<std::string, std::string>> uses{
                {"int use() { return @tax(\"x\", 2, 3); }", "'shop::tax' expects 1 argument(s), got 3"},
                {"int use() { return @tax(\"x\"); }", "argument 1 of 'shop::tax': cannot pass 'string' to 'p' declared 'int'"},
                {"int use() { string s = @tax(1); return 0; }", "cannot initialize 's' declared 'string' with 'int'"},
                {"int use() { return @rate(); }", "'shop::rate' expects 1 to 2 argument(s), got 0"},
                {"int use() { return @sq(1, 2, 3); }", "no overload of 'shop::sq' matches this call (3 argument(s))"},
                {"void use() { @fee(); }", "the result of 'shop::fee' is discarded, but 'shop::fee' is [[nodiscard]]"},
                {"int use() { int k = @twice(); return k; }", "cannot initialize 'k' declared 'int' with 'coroutine<int>'"},
                {"int use() { string s = @base; return 0; }", "cannot initialize 's' declared 'string' with 'int'"},
                {"void use() { @base = \"x\"; }", "cannot assign 'string' to 'shop::base' declared 'int'"},
                {"int use() { int n = 0; for (var v : @twice()) { n += v; } auto _ = @fee(); @base = @sq(2) + @sq(2, 3) + @rate(1); return @tax(@base) + n; }", ""},
            };
            const auto spelled = [](std::string text, const std::string& prefix) {
                for (size_t at = text.find('@'); at != std::string::npos; at = text.find('@', at)) { text.replace(at, 1, prefix); }
                return text;
            };
            program_files files("bare");
            files.write("declaring.jai", "namespace shop { " + members + "}");
            check_each(uses, [&](const auto& use) {
                for (const std::string prefix : std::vector<std::string>{"", "shop::"}) {
                    const std::string code = "namespace shop { " + spelled(use.first, prefix) + " }";
                    files.write("naming.jai", code);
                    for (const std::string& program : std::vector<std::string>{
                        "namespace shop { " + members + "} " + code + " shop::use(); 1",
                        code + " namespace shop { " + members + "} shop::use(); 1",
                        "import \"declaring.jai\"; import \"naming.jai\"; shop::use(); 1",
                        "import \"naming.jai\"; import \"declaring.jai\"; shop::use(); 1",
                    }) {
                        const std::string got = outcome(*files.engine(), program);
                        check_true(use.second.empty() ? got == "1" : got.find(use.second) != std::string::npos, program + "\n" + code + "\n" + got);
                    }
                }
            });
            // a function read as a value carries the declarations a signature slot judges
            check_each(std::vector<std::string>{"tax", "shop::tax"}, [&](const std::string& name) {
                const std::string got = outcome(*files.engine(), "namespace shop { int tax(int p) { return p + 1; } int use() { function<int(int)> fits = " + name +
                    "; function<string(string)> f = " + name + "; return fits(1); } } shop::use()");
                check_true(got.find("cannot initialize 'f' declared 'function<string(string)>' with 'function': 'shop::tax' declares parameter 'p' as 'int'") != std::string::npos, got);
            });
            // the name a bare use reaches is the nearest one: a local or a parameter, then the namespace's member, then each enclosing
            // namespace's, then the program's; ::name is the program's
            const std::string nested = R"(
                int f(int a) { return 1; }
                string base = "program";
                namespace outer {
                    int f(int a, int b) { return 2; }
                    int base = 5;
                    int g(int a) { return 7; }
                    namespace inner {
                        int f(int a, int b, int c) { return 3; }
                        USE
                    }
                }
            )";
            check_each(std::vector<std::pair<std::string, std::string>>{
                {"int use() { return f(1, 2, 3) * 100 + ::f(1) * 10 + outer::f(1, 2) + g(1) + base; }", "324"},
                {"string use(function<string(string)> f, string base) { string s = base; return f(s) + to_string(::f(1)) + ::base; }", "program!1program"},
                {"int use() { auto g = [](string s) { return s.size(); }; return g(\"four\"); }", "4"},
                {"int use() { return f(1); }", "'outer::inner::f' expects 3 argument(s), got 1"},
                {"int use() { return f(1, 2); }", "'outer::inner::f' expects 3 argument(s), got 2"},
                {"int use() { return ::f(1, 2); }", "'f' expects 1 argument(s), got 2"},
                {"int use() { return g(1, 2); }", "'outer::g' expects 1 argument(s), got 2"},
                {"int use() { string s = base; return 0; }", "cannot initialize 's' declared 'string' with 'int'"},
                {"int use() { int n = ::base; return n; }", "cannot initialize 'n' declared 'int' with 'string'"},
            }, [&](const auto& c) {
                std::string program = nested;
                program.replace(program.find("USE"), 3, c.first);
                program += c.first.find("function<string(string)> f") != std::string::npos ? "outer::inner::use([](string s) { return s + \"!\"; }, base)" : "outer::inner::use()";
                const std::string got = outcome(*files.engine(), program);
                check_true(got == c.second || (got.size() > 30 && got.find(c.second) != std::string::npos), c.first + "\n" + got);
            });
            // another file's program-scope function of the name is judged only where the bare name reaches the program scope
            files.write("quiet.jai", "[[nodiscard]] int quiet(int x) { return x + 1; }");
            check_eq(std::string("6"), outcome(*files.engine(), "import \"quiet.jai\"; namespace shop { void quiet(string s) { } int use() { quiet(\"member\"); return 1; } } "
                "int local() { auto quiet = [](string s) { return 0; }; quiet(\"local\"); return 2; } class K { void quiet(string s) { } int run() { quiet(\"method\"); return 3; } } "
                "shop::use() + local() + K().run()"));
            const std::string dropped = outcome(*files.engine(), "import \"quiet.jai\"; void drop() { quiet(1); } 1");
            check_true(dropped.find("the result of 'quiet' is discarded") != std::string::npos, dropped);
            // code that runs before the member's declaration reaches what is outside the namespace under the name, so a name both
            // declare is judged as the member only where the member's declaration stands above the use
            check_eq(std::string("1"), outcome(*files.engine(), "namespace a { int v = 1; } namespace a::b { int g() { int k = v; return k; } int z = g(); string v = \"s\"; } a::b::z"));
            files.write("early.jai", "string base = \"program\"; int tax(string s) { return 5; } namespace shop { string early() { string s = base; return s + to_string(tax(s)); } } string first = shop::early();");
            check_eq(std::string("program5"), outcome(*files.engine(), "import \"early.jai\"; import \"declaring.jai\"; first"));
        });

        // A reload pools what its files declare now, each parsed knowing the others: a member the namespace gains is found
        // bare by the namespace's other files, and a program that still names a dropped member no longer loads
        test("a_reload_brings_a_namespaces_new_members_to_its_other_files", [&]() {
            program_files files("reload");
            files.write("declaring.jai", "namespace shop { class Item { int price = 3; } int tax(int p) { return p + 1; } int base = 10; }");
            files.write("naming.jai", "namespace shop { int total() { Item i; return tax(i.price) + base; } }");
            auto eng = files.engine();
            check_eq(std::string("14"), outcome(*eng, "import \"declaring.jai\"; import \"naming.jai\"; function reload() -> bool { return import(\"declaring.jai\", \"naming.jai\"); } shop::total()"));
            files.write("declaring.jai", "namespace shop { class Item { int price = 3; } class Box { int w = 6; } enum Size { small, big = 7 } int tax(int p) { return p + 1; } int fee() { return 100; } int base = 10; }");
            files.write("naming.jai", "namespace shop { int total() { Item i; Box b; Size s = Size::big; return tax(i.price) + base + fee() + b.w + s; } }");
            check_eq(std::string("true"), outcome(*eng, "reload();"), "both files re-ran");
            check_eq(std::string("127"), outcome(*eng, "shop::total()"));
            files.write("declaring.jai", "namespace shop { class Item { int price = 3; } class Box { int w = 6; } enum Size { small, big = 7 } int fee() { return 100; } int base = 10; }");
            const std::string refused = outcome(*files.engine(), "import \"declaring.jai\"; import \"naming.jai\"; shop::total()");
            check_true(refused.find("naming.jai") != std::string::npos && refused.find("use of undeclared identifier 'tax'") != std::string::npos, refused);
        });

        // A cached unit's clean check stands while the files that declare the names it read stand: a later engine walks the
        // root alone, and a member the declaring file drops fails the cached unit's load
        test("a_cached_check_of_namespace_code_follows_the_file_that_declares_its_names", [&]() {
            const std::string declaring = "namespace shop { class Item { int price = 3; } int tax(int p) { return p + 1; } int base = 10; }";
            check_each(std::vector<std::string>{
                "import \"declaring.jai\"; import \"naming.jai\"; shop::total()",
                "import \"naming.jai\"; import \"declaring.jai\"; shop::total()",
            }, [&](const std::string& root) {
                program_files files("cache");
                files.write("declaring.jai", declaring);
                files.write("naming.jai", "namespace shop { int total() { Item i; return tax(i.price) + base; } }");
                check_eq(std::string("14"), outcome(*files.engine(true), root), "the load that writes the caches");
                auto cached = files.engine(true);
                check_eq(std::string("14"), outcome(*cached, root), "the load that reads them");
                check_eq((uint64_t)1, detail::engine_access::check_walk_count(*cached), "the root's text alone is checked again");
                files.write("declaring.jai", "namespace shop { class Item { int price = 3; } int levy(int p) { return p + 1; } int base = 10; }");
                const std::string refused = outcome(*files.engine(true), root);
                check_true(refused.find("naming.jai") != std::string::npos && refused.find("use of undeclared identifier 'tax'") != std::string::npos, refused);
            });
        });

        // An enum a namespace declares is the type ns::Name: its code names it bare, every other file by the qualified name,
        // and every rule of an enum holds for it under that name
        test("a_namespace_declares_enums", [&]() {
            const std::string declaring = R"(
                enum Kind { plain = 40, other }
                namespace shop {
                    enum Kind { a, b, c = b + 5, d = Kind::c + 1 }
                    Kind next(Kind k) { return k == Kind::a ? Kind::b : Kind.a; }
                    array<Kind> all() { array<Kind> r = [Kind::a]; r.push(Kind::d); return r; }
                    map<string, Kind> named() { map<string, Kind> m = {"first": Kind::a}; m["last"] = cast<Kind>(7); return m; }
                    string word(Kind k) { switch (k) { case Kind::a: return "a"; case Kind::c: return "c"; default: return "other"; } }
                    int through_lambda() { auto f = [](Kind k) -> Kind { return k == Kind::c ? Kind::d : k; }; return f(Kind::c) + Kind::b; }
                    class Item {
                        Kind kind = Kind::b;
                        array<Kind> tags = [Kind::a, Kind::c];
                        Kind bump() { kind = cast<Kind>(kind + 5); return kind; }
                        static Kind first() { return Kind::a; }
                    }
                    int count() { return Kind.size(); }
                    int outer() { return ::Kind::other; }
                    string over(Kind k) { return "kind"; }
                    string over(int n) { return "int"; }
                }
                namespace shop { Kind later = Kind::d; }
                namespace a::b { enum Level { low = 1, high } Level top() { return Level::high; } }
                namespace a { namespace b { Level bottom = Level::low; enum Side { left, right } } }
            )";
            check_each(std::vector<std::pair<std::string, std::string>>{
                {"`${shop::Kind::b} ${shop::Kind.c} ${shop::Kind::d} ${Kind::other} ${shop::outer()} ${reflect::type_of(Kind::plain)}`", "1 6 7 41 41 Kind"},
                {"`${shop::next(shop::Kind::a)} ${reflect::type_of(shop::next(shop::Kind::b))} ${shop::all()} ${shop::named()} ${shop::word(shop::Kind::c)} ${shop::through_lambda()}`",
                 "1 shop::Kind [0, 7] {\"first\": 0, \"last\": 7} c 8"},
                {"shop::Item i; `${i.kind} ${reflect::type_of(i.kind)} ${i.bump()} ${reflect::type_of(shop::Item::first())} ${i.tags} ${reflect::fields(i)[0][\"type\"]} ${reflect::fields(i)[1][\"type\"]}`",
                 "1 shop::Kind 6 shop::Kind [0, 6] shop::Kind array<shop::Kind>"},
                {"shop::Kind k; array<shop::Kind> ks = [shop::Kind::b]; map<string, shop::Kind> m; m[\"x\"] = k; shop::Kind same(shop::Kind v) { return v; }\n"
                 "`${k} ${reflect::type_of(k)} ${reflect::type_of(ks[0])} ${reflect::type_of(same(cast<shop::Kind>(6)))} ${reflect::type_of(m[\"x\"])}`", "0 shop::Kind shop::Kind shop::Kind shop::Kind"},
                {"var out = \"\"; try { cast<shop::Kind>(9); } catch (e) { out = e; } out", "Cannot cast 9 to 'shop::Kind': not one of its values"},
                {"string s = \"\"; for (auto [name, n] : shop::Kind) { s += name + \"=\" + to_string(n) + \" \"; } s + to_string(shop::count())", "a=0 b=1 c=6 d=7 4"},
                {"var v = shop::Kind::b; shop::Kind k = v; var out = \"\"; try { throw shop::Kind::c; } catch (shop::Kind e) { out = reflect::type_of(e); }\n"
                 "`${reflect::type_of(v)} ${reflect::type_of(k)} ${shop::over(k)} ${shop::over(1)} ${out}`", "int shop::Kind kind int shop::Kind"},
                {"shop::Item i; i.bump(); string j = to_json(i); shop::Item back = from_json(j); shop::Kind k = from_json(to_json(shop::Kind::d));\n"
                 "shop::Item binary = from_binary(to_binary(i)); array<shop::Kind> ks = from_json(\"[0, 6]\");\n"
                 "`${j} ${reflect::type_of(back.kind)} ${back.kind} ${reflect::type_of(back.tags[1])} ${reflect::type_of(k)} ${k} ${reflect::type_of(binary.kind)} ${reflect::type_of(binary.tags[1])} ${reflect::type_of(ks[1])}`",
                 "{\"$type\":\"shop::Item\",\"$val\":{\"kind\":6,\"tags\":[0,6]}} shop::Kind 6 shop::Kind shop::Kind 7 shop::Kind shop::Kind shop::Kind"},
                {"`${shop::later} ${reflect::type_of(shop::later)} ${a::b::top()} ${reflect::type_of(a::b::bottom)} ${a::b::Level::high} ${reflect::type_of(a::b::Side::right)}`", "7 shop::Kind 2 a::b::Level 2 a::b::Side"},
                {"using a::b::Level, shop::Item; Level l = Level::high; array<Level> ls = [l, Level.low]; Item i; `${l} ${reflect::type_of(ls[1])} ${reflect::type_of(i.kind)}`", "2 a::b::Level shop::Kind"},
                {"namespace shop { int local() { enum Kind { x = 9, y } return Kind::y + Kind.size(); } int after() { return Kind::c; } } `${shop::local()} ${shop::after()}`", "12 6"},
                // the checker's rules, under the qualified name
                {"shop::Kind[\"a\"] = 5;", "error: cannot assign to 'shop::Kind[...]': 'shop::Kind' is const (declared at 4:26)"},
                {"namespace shop { void wipe() { Kind.clear(); } }", "error: cannot call 'clear' on 'shop::Kind': 'shop::Kind' is const and 'clear' changes it (declared at 4:26)"},
                {"shop::Kind k = 2;", "error: cannot initialize 'k' declared 'shop::Kind' with 'int': cast<shop::Kind>(n) converts an int that is one of its values"},
                {"shop::Kind k = Kind::plain;", "error: cannot initialize 'k' declared 'shop::Kind' with 'Kind': cast<shop::Kind>(n) converts an int that is one of its values"},
                {"Kind k = shop::Kind::a;", "error: cannot initialize 'k' declared 'Kind' with 'shop::Kind': cast<Kind>(n) converts an int that is one of its values"},
                {"namespace shop { void wrong() { Kind k = Kind::a; k = 1; } }", "error: cannot assign 'int' to 'k' declared 'shop::Kind': cast<shop::Kind>(n) converts an int that is one of its values (declared at 24:50)"},
                {"shop::Kind k = shop::Kind::a; k++;", "error: cannot apply '++' to 'shop::Kind'"},
                {"shop::Kind k = shop::Kind::zzz;", "error: no value 'zzz' in enum 'shop::Kind' (declared at 4:26)"},
                {"namespace shop { int wrong() { return Kind.zzz; } }", "error: no value 'zzz' in enum 'shop::Kind' (declared at 4:26)"},
                {"int x = shop::Kind;", "error: cannot initialize 'x' declared 'int' with 'enum shop::Kind': an enum's name is its value table, a map<string,int>; name one of its values (shop::Kind::a)"},
                {"array<shop::Kind> xs = [1];", "error: cannot initialize 'xs' declared 'array<shop::Kind>' with 'array': array element 0: expected 'shop::Kind', got 'int'"},
                {"var p = dynamic_cast<shared_ptr<shop::Kind>>(null);", "error: dynamic_cast<shop::Kind> names a class, and 'shop::Kind' is an enum: cast<shop::Kind>(n) converts an int that is one of its values"},
                {"a::Side s;", "error: unknown type 'a::Side'"},
                {"shop::Kind k = shop::Kinds::a;", "error: no member 'Kinds' in namespace 'shop'"},
            }, [&](const auto& c) {
                auto eng = make_engine();
                jai::stdlib::register_all(*eng);
                const std::string got = outcome(*eng, declaring + c.first);
                const size_t error = got.find("error: ");
                check_eq(c.second, error == std::string::npos ? got : got.substr(error, got.find('\n', error) - error), c.first);
            });
            // a later run names what an earlier one declared: the engine knows the enum, its values are the run's to find
            check_each(std::vector<std::pair<std::string, std::string>>{
                {"shop::Kind k = shop::Kind::b; Kind g = Kind::other; namespace shop { Kind again(Kind v) { return v == Kind::b ? Kind::c : v; } }\n"
                 "`${reflect::type_of(k)} ${reflect::type_of(g)} ${shop::again(k)} ${reflect::type_of(shop::again(k))}`", "shop::Kind Kind 6 shop::Kind"},
                {"shop::Kind k = 2;", "error: cannot initialize 'k' declared 'shop::Kind' with 'int': cast<shop::Kind>(n) converts an int that is one of its values"},
                {"shop::Kind::zzz", "No value 'zzz' in enum 'shop::Kind'"},
                {"shop::Kind[\"a\"] = 5;", "error: cannot assign to 'shop::Kind[...]': 'shop::Kind' is const"},
            }, [&](const auto& c) {
                auto eng = make_engine();
                jai::stdlib::register_all(*eng);
                eng->execute(declaring);
                const std::string got = outcome(*eng, c.first);
                const size_t error = got.find("error: ");
                check_eq(c.second, error == std::string::npos ? got.substr(0, got.find('\n')) : got.substr(error, got.find('\n', error) - error), c.first);
            });
            // a unit that declares the enum where it names it, in any order
            check_each(std::vector<std::pair<std::string, std::string>>{
                {"namespace shop { enum Kind { a, b } Kind k = Kind::b; } shop::Kind j = shop::k; `${j} ${reflect::type_of(j)}`", "1 shop::Kind"},
                {"namespace shop { enum Kind { a, b } class Kind2 {} enum Other { Kind2, Kind = 5 } } `${shop::Other::Kind2} ${shop::Other::Kind}`", "0 5"},
                {"namespace shop { Kind first() { return Kind::a; } enum Kind { a = 3, b } } `${shop::first()} ${reflect::type_of(shop::first())}`", "3 shop::Kind"},
                {"namespace shop { enum Kind { a, b } } namespace shop { enum Kind { c, d, e } } `${shop::Kind}`", "{\"c\": 0, \"d\": 1, \"e\": 2}"},
                {"namespace shop { enum Kind { a, b = Kind::zzz } }", "parse failed:\n<script>:1:37: error: enum value 'b' names 'zzz', which is not an earlier value of 'shop::Kind'"},
            }, [&](const auto& c) {
                auto eng = make_engine();
                jai::stdlib::register_all(*eng);
                const std::string got = outcome(*eng, c.first);
                check_eq(c.second, got.substr(0, got.find("\n    ")), c.first);
            });
        });

        // The files of a program share a namespace's enum as they share its classes: named bare where the namespace is open,
        // in either import order; the declaration that runs last holds the name and a value another file's enum lacks is the
        // run's to find, as at program scope
        test("a_namespaces_code_finds_bare_the_enum_another_file_declares", [&]() {
            const std::string naming = R"(
                namespace shop {
                    Kind pick(int n) { return n > 0 ? Kind::b : cast<Kind>(n); }
                    array<Kind> both() { return [Kind::a, Kind.b]; }
                    class Tagged { Kind kind = Kind::b; map<string, Kind> named = {"x": Kind::a}; }
                    int table() { return Kind.size(); }
                }
                string report() {
                    shop::Tagged t = from_json(to_json(shop::Tagged()));
                    return `${shop::pick(1)} ${reflect::type_of(shop::pick(0))} ${reflect::type_of(shop::both()[1])} ${reflect::type_of(t.kind)} ${reflect::type_of(t.named["x"])} ${shop::table()}`;
                }
            )";
            const std::string expected = "1 shop::Kind shop::Kind shop::Kind shop::Kind 2";
            program_files files("enums");
            files.write("declaring.jai", "enum Kind { global = 9 } namespace shop { enum Kind { a, b } }");
            files.write("naming.jai", naming);
            files.write("naming_imports.jai", "import \"declaring.jai\";\n" + naming);
            files.write("naming_using.jai", "using shop::Kind;\nKind outside() { Kind k = Kind::b; return k; }");
            files.write("naming_value.jai", "namespace shop { Kind wrong() { return Kind::zzz; } }");
            files.write("naming_slot.jai", "namespace shop { Kind slot() { Kind k = 1; return k; } }");
            files.write("declaring_again.jai", "namespace shop { enum Kind { c, d, e } }");
            check_each(std::vector<std::pair<std::string, std::string>>{
                {"include \"declaring.jai\"; include \"naming.jai\"; report()", expected},
                {"import \"declaring.jai\"; import \"naming.jai\"; report()", expected},
                {"import \"naming.jai\"; import \"declaring.jai\"; report()", expected},
                {"import \"naming_imports.jai\"; report()", expected},
                {"import \"naming_using.jai\"; import \"declaring.jai\"; `${outside()} ${reflect::type_of(outside())} ${Kind::global}`", "1 shop::Kind 9"},
                {"import \"declaring.jai\"; import \"declaring_again.jai\"; `${shop::Kind}`", "{\"c\": 0, \"d\": 1, \"e\": 2}"},
                {"import \"declaring_again.jai\"; import \"declaring.jai\"; `${shop::Kind}`", "{\"a\": 0, \"b\": 1}"},
                {"import \"naming_value.jai\"; import \"declaring.jai\"; shop::wrong()", "No value 'zzz' in enum 'shop::Kind'"},
                {"import \"naming_slot.jai\"; import \"declaring.jai\"; shop::slot()",
                 "error: cannot initialize 'k' declared 'shop::Kind' with 'int': cast<shop::Kind>(n) converts an int that is one of its values"},
            }, [&](const auto& c) {
                for (const char* pass : {"", " (the load that writes the caches)", " (the load that reads them)"}) {
                    auto eng = files.engine(*pass != 0);
                    const std::string got = outcome(*eng, c.first);
                    const size_t error = got.find("error: ");
                    check_eq(c.second, error == std::string::npos ? got.substr(0, got.find('\n')) : got.substr(error, got.find('\n', error) - error), c.first + pass);
                }
            });
        });

        // A namespace's generics meet its other members under their bare names as under shop::name: a generic function is
        // found and its type arguments deduced from a member's declared result, an instance's body is checked as hand-written
        // code is, a plain function of a generic's name keeps the calls it takes, and an enum is a type argument
        test("a_namespaces_generics_meet_its_bare_names_as_its_qualified_ones", [&]() {
            const std::string twice = "T twice<T>(T x) { return x + x; } ";
            const std::string tax = "int tax(int p) { return p + 1; } ";
            const std::string pick = "T pick<T>(T x) { return x; } int pick(int x) { return x + 100; } ";
            program_files files("generics");
            files.write("tax.jai", "namespace shop { " + tax + "}");
            files.write("twice.jai", "namespace shop { " + twice + "}");
            files.write("bump.jai", "namespace shop { T bump<T>(T x) { tax(x); return x; } }");
            files.write("kind.jai", "namespace shop { enum Kind { a, b } }");
            // {program, its value or what the load says, the note under an instance's error}: @ is "" and then "shop::"
            check_each(std::vector<std::tuple<std::string, std::string, std::string>>{
                {"namespace shop { " + twice + "int go() { return @twice(4) + @twice<int>(1); } } shop::go()", "10", ""},
                {"namespace shop { " + twice + tax + "int go() { return @twice(@tax(1)); } } shop::go()", "4", ""},
                {"namespace shop { " + twice + "int go() { return @twice(@tax(1)); } " + tax + "} shop::go()", "4", ""},
                {"import \"tax.jai\"; namespace shop { " + twice + "int go() { return @twice(@tax(1)); } } shop::go()", "4", ""},
                {"namespace shop { " + twice + "int go() { return @twice(@tax(1)); } } import \"tax.jai\"; shop::go()", "4", ""},
                {"import \"twice.jai\"; namespace shop { " + tax + "int go() { return @twice(@tax(1)); } } shop::go()", "4", ""},
                {"namespace shop { " + twice + "auto loose(p) { return p; } int go() { return @twice(@loose(1)); } } shop::go()",
                 "cannot deduce T for 'twice': argument 1's type is not known at load; write twice<T>(...)", ""},
                {"namespace shop { " + tax + "int base = 10; T bump<T>(T x) { @base = @base + @tax(1); return x; } int go() { return @bump(4) + @base; } } shop::go()", "16", ""},
                {"namespace shop { " + tax + "T bump<T>(T x) { @tax(x, 2); return x; } int go() { return @bump(4); } } shop::go()",
                 "'shop::tax' expects 1 argument(s), got 2", "in shop::bump<int>, instantiated at"},
                {"namespace shop { " + tax + "T bump<T>(T x) { @tax(x); return x; } int go() { return @bump(4); } string bad() { return @bump(\"s\"); } } shop::go()",
                 "argument 1 of 'shop::tax': cannot pass 'string' to 'p' declared 'int'", "in shop::bump<string>, instantiated at"},
                {"import \"bump.jai\"; namespace shop { " + tax + "int go() { return @bump(4); } } shop::go()", "4", ""},
                {"import \"bump.jai\"; import \"tax.jai\"; namespace shop { string go() { return @bump(\"s\"); } } shop::go()",
                 "argument 1 of 'shop::tax': cannot pass 'string' to 'p' declared 'int'", "in shop::bump<string>, instantiated at"},
                {"int tax(int p, int q) { return p + q; } T both<T>(T x) { return tax(x, x); } namespace shop { " + tax + "} both(4)", "8", ""},
                {"namespace shop { " + pick + "string go() { return to_string(@pick(1)) + @pick(\"s\") + @pick<string>(\"t\"); } } shop::go()", "101st", ""},
                {"namespace shop { string go() { return to_string(@pick(1)) + @pick(\"s\") + @pick<string>(\"t\"); } " + pick + "} shop::go()", "101st", ""},
                {"namespace shop { " + pick + "string go() { string s = @pick(1); return s; } } shop::go()", "cannot initialize 's' declared 'string' with 'int'", ""},
                {"namespace shop { enum Kind { a, b } class Stack<T> { array<T> items; } Kind kind<T>(T v) { return Kind::b; } "
                 "int go() { Stack<Kind> s; s.items.push(@kind(1)); return s.items.size(); } } "
                 "shop::Stack<shop::Kind> s; s.items.push(shop::kind(\"x\")); `${reflect::type_of(s)} ${s.items[0] == shop::Kind::b} ${reflect::type_of(s.items[0])} ${shop::go()}`",
                 "shop::Stack<shop::Kind> true shop::Kind 1", ""},
                {"import \"kind.jai\"; namespace shop { class Stack<T> { array<T> items; } Kind kind<T>(T v) { return Kind::b; } "
                 "string go() { Stack<Kind> s; s.items.push(@kind(1)); return reflect::type_of(s) + reflect::type_of(s.items[0]); } } shop::go()", "shop::Stack<shop::Kind>shop::Kind", ""},
            }, [&](const auto& c) {
                for (const std::string prefix : std::vector<std::string>{"", "shop::"}) {
                    std::string program = std::get<0>(c);
                    for (size_t at = program.find('@'); at != std::string::npos; at = program.find('@', at)) { program.replace(at, 1, prefix); }
                    const std::string got = outcome(*files.engine(), program);
                    const std::string& expected = std::get<1>(c);
                    check_true(got == expected || (got.size() > 30 && got.find(expected) != std::string::npos && got.find(std::get<2>(c)) != std::string::npos), program + "\n" + got);
                }
            });
        });

        // C++ [namespace.qual]: the first segment of a qualified name is looked up as a bare name is, so namespace code names a
        // nested namespace from where it stands, innermost first, whichever file opens it
        test("namespace_code_names_a_nested_namespace_relatively", [&]() {
            const std::string declaring = R"(
                namespace outer {
                    namespace inner {
                        class Item { int v = 2; static int made() { return 7; } }
                        enum Kind { a, b }
                        int base = 10;
                        const int LIMIT = 3;
                        int k() { return 4; }
                        namespace deep { int k() { return 5; } class Far { int v = 6; } }
                    }
                }
            )";
            const std::string naming = R"(
                namespace outer {
                    int call() { return inner::k() + inner::deep::k(); }
                    int read() { return inner::base + inner::LIMIT; }
                    void store() { inner::base = 20; }
                    int typed() { inner::Item i; inner::deep::Far f; array<inner::Item> all = [inner::Item()]; return i.v + f.v + all.size() + inner::Item::made(); }
                    inner::Kind kind(inner::Kind given) { inner::Kind k = inner::Kind::b; return given == k ? k : inner::Kind::a; }
                    auto later() { return [](int x) { return inner::k() + x; }; }
                    class User { int field = inner::k(); int method() { return inner::deep::k() + field; } }
                    namespace inner { int up() { return deep::k() + k(); } }
                    namespace other { int across() { return inner::k() * 2; } }
                }
                namespace outer::inner::deep { int sibling() { return deep::k(); } }
                string report() {
                    outer::store();
                    return `${outer::call()} ${outer::read()} ${outer::typed()} ${outer::kind(outer::inner::Kind::b) == outer::inner::Kind::b} ${outer::later()(1)} ` +
                           `${outer::User().method()} ${outer::inner::up()} ${outer::other::across()} ${outer::inner::deep::sibling()} ${outer::inner::k()}`;
                }
            )";
            const std::string expected = "9 23 16 true 5 9 9 8 5 4";
            const std::string hidden = "namespace inner { int k() { return 1; } int only() { return 2; } } namespace outer { namespace inner { int k() { return 40; } } ";
            program_files files("relative");
            files.write("declaring.jai", declaring);
            files.write("naming.jai", naming);
            files.write("nested_later.jai", "namespace outer { namespace inner { int late() { return 8; } } }");
            files.write("using_relative.jai", "namespace outer { namespace inner { int k() { return 4; } } using inner::k; int use() { return k(); } }");
            check_each(std::vector<std::pair<std::string, std::string>>{
                {declaring + naming + "report()", expected},
                {"import \"declaring.jai\"; import \"naming.jai\"; report()", expected},
                {"import \"naming.jai\"; import \"declaring.jai\"; report()", expected},
                {"import \"nested_later.jai\"; namespace outer { int use() { return inner::late(); } } outer::use()", "8"},
                {"namespace outer { int use() { return inner::late(); } } import \"nested_later.jai\"; outer::use()", "8"},
                {"import \"using_relative.jai\"; outer::use()", "4"},
                // the innermost namespace of the name wins, and the search stops there; ::inner reaches the program's
                {hidden + "int use() { return inner::k() + ::inner::k(); } } outer::use()", "41"},
                {hidden + "int use() { return outer::inner::k(); } } outer::use()", "40"},
                {hidden + "int use() { return inner::only(); } } outer::use()", "no member 'only' in namespace 'outer::inner'"},
                {"namespace inner { int k() { return 1; } } namespace outer { int use() { return inner::k(); } } outer::use()", "1"},
                {"namespace outer { int use() { return nowhere::k(); } } outer::use()", "use of undeclared identifier 'nowhere'"},
                {"namespace game { namespace ai { int level() { return depth; } } extern ai::depth; } 1", "1"},
            }, [&](const auto& c) {
                auto eng = files.engine();
                const std::string got = outcome(*eng, c.first);
                check_true(got == c.second || (c.second.size() > 8 && got.find(c.second) != std::string::npos), c.first + "\n" + got);
            });
            auto earlier_run = files.engine();
            earlier_run->execute(declaring);
            check_eq(std::string("9"), outcome(*earlier_run, "namespace outer { int again() { return inner::k() + inner::deep::k(); } } outer::again()"), "a namespace an earlier run opened");
        });

        // A namespace's object is as typed through its qualified name as a program-scope object is through its own
        test("a_member_taken_through_a_qualified_namespace_variable_is_checked", [&]() {
            struct database { int rows = 2; };
            const std::string declaring = R"(
                class Part { int v = 2; }
                class Store { int n = 1; array<int> items = [1]; Part part; static int count = 5; int get(int k) { return n + k; } Part make() { return Part(); } static int made(int k) { return k; } }
                class Sub : Store { int get(int k) override { return 100; } int only = 5; }
                class List<T> { array<T> xs; int size() { return xs.size(); } void add(T v) { xs.push(v); } T first() { return xs[0]; } }
                T twice<T>(T v) { return v + v; }
                namespace mv {
                    Store db;
                    shared_ptr<Store> held = make_shared<Store>();
                    array<Store> list = [Store()];
                    map<string, Store> table = {"a": Store()};
                    array<int> numbers = [1, 2];
                    string title = "x";
                    auto inferred = Store();
                    var loose = Store();
                    Database host;
                    List<int> ints;
                    class Item { int n = 1; Item() {} Item(int k) { n = k; } static int made() { return 7; } static int count = 3; }
                    class Box<T> { T v; T get() { return v; } }
                    List<int> fresh() { List<int> l; l.add(4); return l; }
                    namespace in { Store deep; }
                }
            )";
            program_files files("receivers");
            files.write("declaring.jai", declaring);
            const auto engine = [&]() {
                auto result = files.engine();
                dynamic_binder<database>(*result, "Database").constructor<>().method("rows", [](database& self) { return self.rows; }).method("add", [](database& self, int a_count) { self.rows += a_count; }).build();
                return result;
            };
            const std::vector<std::pair<std::string, std::string>> refused{
                {"mv::db.nope();", "no member 'nope' in class 'Store'"},
                {"mv::db.nope = 1;", "no member 'nope' in class 'Store'"},
                {"string s = mv::db.get(1);", "cannot initialize 's' declared 'string' with 'int'"},
                {"mv::db.get(\"x\", 2);", "'get' expects 1 argument(s), got 2"},
                {"mv::db.get(\"x\");", "argument 1 of 'get': cannot pass 'string' to 'k' declared 'int'"},
                {"mv::db.n = \"x\";", "cannot assign 'string' to field 'n' declared 'int'"},
                {"mv::db.items.nope();", "no member 'nope' on 'array<int>'"},
                {"string s = mv::db.items.size();", "cannot initialize 's' declared 'string' with 'int'"},
                {"mv::db.part.nope;", "no member 'nope' in class 'Part'"},
                {"mv::db.make().nope();", "no member 'nope' in class 'Part'"},
                {"mv::held.nope();", "no member 'nope' in class 'Store'"},
                {"string s = mv::held.get(1);", "cannot initialize 's' declared 'string' with 'int'"},
                {"mv::list[0].nope;", "no member 'nope' in class 'Store'"},
                {"mv::table[\"a\"].nope();", "no member 'nope' in class 'Store'"},
                {"mv::numbers.nope();", "no member 'nope' on 'array<int>'"},
                {"mv::numbers.push(\"x\");", "cannot push into 'array<int>'"},
                {"string s = mv::numbers.size();", "cannot initialize 's' declared 'string' with 'int'"},
                {"mv::title.nope();", "no member 'nope' on 'string'"},
                {"mv::in::deep.nope();", "no member 'nope' in class 'Store'"},
                {"mv::db = 3;", "cannot assign 'int' to 'mv::db' declared 'Store'"},
                {"mv::host.nope();", "no member 'nope' in class 'Database'"},
                {"mv::ints.nope();", "no member 'nope' in class 'List<int>'"},
                {"mv::ints.add(\"x\");", "argument 1 of 'add': cannot pass 'string' to 'v' declared 'int'"},
                {"string s = mv::fresh().size();", "cannot initialize 's' declared 'string' with 'int'"},
                {"mv::Item().nope();", "no member 'nope' in class 'mv::Item'"},
                {"mv::Item(1, 2);", "mv::Item"},
                {"string s = mv::Item().n;", "cannot initialize 's' declared 'string' with 'int'"},
                {"string s = mv::Box<int>().get();", "cannot initialize 's' declared 'string' with 'int'"},
                {"string s = mv::Item::made();", "cannot initialize 's' declared 'string' with 'int'"},
                {"mv::Item::nope();", "no static member 'nope' in class 'mv::Item'"},
                {"string s = mv::Item::count;", "cannot initialize 's' declared 'string' with 'int'"},
            };
            const std::string import_first = "import \"declaring.jai\"; ";
            auto same_file = engine();
            auto other_file = engine();
            check_each(refused, [&](const auto& c) {
                const std::string in_function = "void f() { " + c.first + " } 1";
                const check_report here = same_file->check(declaring + in_function);
                check_true(here.has_errors() && here.format().find(c.second) != std::string::npos, "the same file: " + here.format());
                other_file->check(import_first + in_function);
                const check_report there = other_file->check(import_first + in_function);
                check_true(there.has_errors() && there.format().find(c.second) != std::string::npos, "another file: " + there.format());
            });
            const std::string inferred = outcome(*engine(), declaring + "void f() { mv::inferred.nope(); } 1");   // auto: typed below its declaration, as at program scope
            check_true(inferred.find("no member 'nope' in class 'Store'") != std::string::npos, inferred);
            check_each(std::vector<std::pair<std::string, std::string>>{
                {"mv::db = Sub(); `${mv::db.get(1)} ${mv::db.n} ${cast<Sub&>(mv::db).only}`", "100 1 5"},   // the slot holds the subclass: the run dispatches on it
                {"Store& r = mv::db; r.n = 9; mv::db.items.push(2); `${mv::db.n} ${mv::db.items.size()}`", "9 2"},
                {"`${mv::held.get(1)} ${mv::list[0].n} ${mv::table[\"a\"].get(2)} ${mv::in::deep.n} ${mv::inferred.n} ${mv::numbers.size()} ${mv::title.size()}`", "2 1 3 1 1 2 1"},
                {"mv::loose.anything_at_all = 1; 1", "Cannot"},   // a var slot stays the run's to judge
                {"mv::host.add(3); mv::host.rows()", "5"},
                {"mv::ints.add(3); `${twice(mv::ints.size())} ${twice(mv::ints.first())} ${twice(mv::fresh().size())} ${twice(mv::db.get(1))}`", "2 6 2 4"},
                {"`${twice(mv::Item().n)} ${twice(mv::Item(4).n)} ${mv::Item::made()} ${mv::Item::count} ${twice(mv::Box<int>().get())}`", "2 8 7 3 0"},
            }, [&](const auto& c) {
                const std::string got = outcome(*engine(), declaring + c.first);
                check_true(got == c.second || (c.second == "Cannot" && got.find("static check failed") == std::string::npos), c.first + "\n" + got);
            });
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
