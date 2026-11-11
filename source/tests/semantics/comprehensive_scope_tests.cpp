#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"
#include <iostream>
#include <vector>
#include <string>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// Global log to track construction/destruction order
static std::vector<std::string> operation_log;

class CppBoundObject {
public:
    static int instance_count;  // Total created (for unique IDs)
    static int alive_count;     // Currently alive
    int id;
    std::string name;

    CppBoundObject(const std::string& n) : id(++instance_count), name(n) {
        alive_count++;
        operation_log.push_back("CppBoundObject(\"" + name + "\") ctor, id=" + std::to_string(id) + ", alive=" + std::to_string(alive_count));
    }

    CppBoundObject(const CppBoundObject& other) : id(other.id), name(other.name) {
        alive_count++;  // Copies also increase alive count
        operation_log.push_back("CppBoundObject COPY from id=" + std::to_string(other.id) + ", alive=" + std::to_string(alive_count));
    }

    ~CppBoundObject() {
        alive_count--;
        operation_log.push_back("~CppBoundObject(\"" + name + "\") dtor, id=" + std::to_string(id) + ", alive=" + std::to_string(alive_count));
    }

    void instance_method() {
        operation_log.push_back("CppBoundObject::instance_method() called on \"" + name + "\"");
    }

    static void static_method() {
        operation_log.push_back("CppBoundObject::static_method() called");
    }

    std::string get_name() const { return name; }
    int get_id() const { return id; }

    static void reset() {
        instance_count = 0;
        alive_count = 0;
        operation_log.clear();
    }
};

int CppBoundObject::instance_count = 0;
int CppBoundObject::alive_count = 0;

void register_cpp_bound_object(engine& eng) {
    dynamic_binder<CppBoundObject>(eng, "CppBoundObject")
        .constructor<std::string>()
        .method("instance_method", &CppBoundObject::instance_method)
        .method("get_name", &CppBoundObject::get_name)
        .method("get_id", &CppBoundObject::get_id)
        .property("name", &CppBoundObject::name)
        .property("id", &CppBoundObject::id)
        .static_method("static_method", &CppBoundObject::static_method)
        .static_property("instance_count", &CppBoundObject::instance_count)
        .static_property("alive_count", &CppBoundObject::alive_count)
        .build();
}

class comprehensive_scope_tests : public suite {
public:
    comprehensive_scope_tests() : suite("Comprehensive Scope Tests") {}

    void forge_tests() override {
        test("a_bound_object_dies_when_its_scope_ends", [this]() {
            struct scope_row { const char* code; int alive; int ctors; int dtors; };   // -1: the count is not pinned
            const scope_row rows[] = {
                {R"(
                    {
                        auto obj1 = CppBoundObject("block_scope");
                    }
                    {
                        auto outer = CppBoundObject("outer_block");
                        {
                            auto inner = CppBoundObject("inner_block");
                        }
                    }
                    true
                )", 0, 3, 3},
                {R"(
                    auto condition = true;
                    if (condition) {
                        auto if_obj = CppBoundObject("if_true_branch");
                    }
                    if (!condition) {
                        auto else_obj = CppBoundObject("if_false_branch");
                    } else {
                        auto else_true = CppBoundObject("else_branch");
                    }
                    true
                )", 0, -1, -1},
                {R"(
                    for (auto i = 0; i < 3; i = i + 1) {
                        auto loop_obj = CppBoundObject("loop_iteration");
                    }
                    true
                )", 0, 3, 3},
                {R"(
                    void test_function() {
                        auto func_local = CppBoundObject("function_local");
                    }
                    test_function();
                    test_function();
                    true
                )", 0, 2, 2},
                {R"(
                    auto make_lambda = []() {
                        auto lambda_local = CppBoundObject("lambda_local");
                        return lambda_local.get_id();
                    };
                    auto id1 = make_lambda();
                    auto id2 = make_lambda();
                    true
                )", 0, -1, -1},
                {R"(
                    class ScriptClass {
                        auto cpp_member;
                        ScriptClass(auto name) {
                            cpp_member = CppBoundObject(name);
                        }
                        ~ScriptClass() {
                        }
                        void instance_method() {
                            cpp_member.instance_method();
                        }
                        static void static_method() {
                            CppBoundObject::static_method();
                        }
                    }
                    {
                        auto script_obj = ScriptClass("script_class_member");
                        script_obj.instance_method();
                    }
                    ScriptClass::static_method();
                    true
                )", 0, -1, -1},
                // the global object outlives the run; every other construction, copies included, is destroyed
                {R"(
                    auto global_obj = CppBoundObject("global");
                    class ScriptClassWithDestructor {
                        auto member;
                        ScriptClassWithDestructor(auto name) {
                            member = CppBoundObject(name + "_member");
                        }
                        ~ScriptClassWithDestructor() {
                        }
                        void call_lambda() {
                            auto lambda = [this]() {
                                auto lambda_obj = CppBoundObject("lambda_capture");
                                member.instance_method();
                            };
                            lambda();
                        }
                    }
                    void test_function() {
                        auto func_obj = CppBoundObject("function");
                        for (auto i = 0; i < 2; i = i + 1) {
                            auto loop_obj = CppBoundObject("loop");
                            if (i == 0) {
                                auto if_obj = CppBoundObject("if_branch");
                            }
                        }
                    }
                    {
                        auto block_obj = CppBoundObject("block");
                        auto script_class = ScriptClassWithDestructor("script_class");
                        script_class.call_lambda();
                        test_function();
                    }
                    true
                )", 1, -1, -1},
                {R"(
                    int test_early_return(auto should_return) {
                        auto before_if = CppBoundObject("before_if");
                        if (should_return) {
                            auto in_if = CppBoundObject("in_if");
                            return 42;
                        }
                        return 0;
                    }
                    auto result1 = test_early_return(true);
                    auto result2 = test_early_return(false);
                    result1 == 42 && result2 == 0
                )", 0, -1, -1},
                {R"(
                    void throwing_function() {
                        auto before_throw = CppBoundObject("before_throw");
                    }
                    throwing_function();
                    true
                )", 0, -1, -1},
            };
            check_each(rows, [&](const scope_row& row) {
                auto eng = make_engine();
                stdlib::register_all(*eng);
                register_cpp_bound_object(*eng);
                CppBoundObject::reset();
                check_eq(true, eng->execute(row.code).as<bool>());
                check_eq(row.alive, CppBoundObject::alive_count, "alive");
                int ctor_count = 0;
                int dtor_count = 0;
                for (const auto& op : operation_log) {
                    ctor_count += op.find("ctor") != std::string::npos ? 1 : 0;
                    dtor_count += op.find("dtor") != std::string::npos ? 1 : 0;
                }
                if (row.ctors >= 0) { check_eq(row.ctors, ctor_count, "constructions"); }
                if (row.dtors >= 0) { check_eq(row.dtors, dtor_count, "destructions"); }
            });
        });
    }
};

// C++ unqualified-lookup order inside methods: locals -> class members -> globals.
// Pins the member-shadows-global contract (a same-named global must never hide a
// field or method inside the class's own methods) across every bare-identifier
// path: calls, reads, stores, compound stores, ++/--, index bases, ref args,
// closures, statics, inheritance, and hot-reload flips.
class member_shadowing_tests : public suite {
public:
    member_shadowing_tests() : suite("Member Shadowing Tests") {}

    static std::shared_ptr<jai::engine> make_stdlib_engine() {
        auto e = make_engine();
        stdlib::register_all(*e);
        return e;
    }

    void forge_tests() override {
        test("method_shadows_global_same_arity", [this]() {
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                string who = "";
                void greet(int n) { who = "global"; }
                class A {
                    void greet(int n) { who = "method"; }
                    void go() { greet(1); }
                }
                auto a = A();
                a.go();
                who
            )");
            check_eq(std::string("method"), r.as<std::string>());
        });

        test("method_shadows_global_wrong_arity_global", [this]() {
            // The jaidoom shape: 0-arg global + 1-arg method, bare 1-arg call.
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                string who = "";
                void sync_exit() { who = "global"; }
                class Level {
                    int exits = 0;
                    void sync_exit(int player) { who = "method " + to_string(player); exits += 1; }
                    void trigger(int player) { sync_exit(player); }
                }
                auto l = Level();
                l.trigger(3);
                who + "/" + to_string(l.exits)
            )");
            check_eq(std::string("method 3/1"), r.as<std::string>());
        });

        test("field_shadows_global_read_write_compound", [this]() {
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                int hp = 999;
                class Creature {
                    int hp = 5;
                    int read_hp() { return hp; }
                    void write_hp() { hp = 42; }
                    void bump_hp() { hp += 1; hp++; }
                }
                auto c = Creature();
                int before = c.read_hp();
                c.write_hp();
                c.bump_hp();
                to_string(before) + "/" + to_string(c.hp) + "/" + to_string(hp)
            )");
            check_eq(std::string("5/44/999"), r.as<std::string>());
        });

        test("local_shadows_field_shadows_global", [this]() {
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                int v = 1;
                class B {
                    int v = 2;
                    int local_wins() { int v = 3; return v; }
                    int field_wins() { return v; }
                    int param_wins(int v) { return v; }
                }
                auto b = B();
                to_string(b.local_wins()) + "/" + to_string(b.field_wins()) + "/" + to_string(b.param_wins(4))
            )");
            check_eq(std::string("3/2/4"), r.as<std::string>());
        });

        test("inherited_members_shadow_globals", [this]() {
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                int score = 100;
                string ping() { return "global"; }
                class Base {
                    int score = 7;
                    string ping() { return "base"; }
                }
                class Derived : Base {
                    string probe() { return ping() + "/" + to_string(score); }
                    void raise() { score += 3; }
                }
                auto d = Derived();
                d.raise();
                d.probe() + "/" + to_string(score)
            )");
            check_eq(std::string("base/10/100"), r.as<std::string>());
        });

        test("ctor_name_bare_call_still_constructs", [this]() {
            // The class-name global stays the constructor: a bare Fact(...) inside a
            // method of Fact must build a NEW instance, never self-bind as a member.
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                class Fact {
                    int n = 0;
                    Fact(int v) { n = v; }
                    Fact successor() { return Fact(n + 1); }
                }
                auto f = Fact(5);
                f.successor().n
            )");
            check_eq((int64_t)6, r.as_int());
        });

        test("closure_in_method_member_over_global", [this]() {
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                int fuel = 500;
                class Rocket {
                    int fuel = 3;
                    int probe() {
                        int local = 9;
                        auto f = [=]() { return fuel * 100 + local; };
                        return f();
                    }
                }
                auto rk = Rocket();
                rk.probe()
            )");
            check_eq((int64_t)309, r.as_int());
        });

        test("statics_shadow_globals_in_static_methods", [this]() {
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                int count = 1000;
                string tick() { return "global"; }
                class S {
                    static int count = 0;
                    static string tick() { count += 1; return "static"; }
                    static string go() { return tick(); }
                }
                S::go() + "/" + to_string(S::count) + "/" + to_string(count)
            )");
            check_eq(std::string("static/1/1000"), r.as<std::string>());
        });

        test("statics_shadow_globals_in_instance_methods", [this]() {
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                int total = 1000;
                class T {
                    static int total = 0;
                    void add(int n) { total += n; }
                }
                auto t = T();
                t.add(5);
                to_string(T::total) + "/" + to_string(total)
            )");
            check_eq(std::string("5/1000"), r.as<std::string>());
        });

        test("index_base_field_shadows_global", [this]() {
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                var grid = [100, 200, 300];
                class Board {
                    var grid = [1, 2, 3];
                    int read(int i) { return grid[i]; }
                    void write(int i, int v) { grid[i] = v; }
                    void bump(int i) { grid[i] += 10; }
                }
                auto b = Board();
                b.write(0, 7);
                b.bump(1);
                to_string(b.read(0)) + "/" + to_string(b.read(1)) + "/" + to_string(grid[0]) + "/" + to_string(grid[1])
            )");
            check_eq(std::string("7/12/100/200"), r.as<std::string>());
        });

        test("ref_arg_field_shadows_global", [this]() {
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                int ammo = 999;
                void reload(int& x) { x = 30; }
                class Gun {
                    int ammo = 0;
                    void refill() { reload(ammo); }
                }
                auto g = Gun();
                g.refill();
                to_string(g.ammo) + "/" + to_string(ammo)
            )");
            check_eq(std::string("30/999"), r.as<std::string>());
        });

        test("no_member_no_shadow_globals_unchanged", [this]() {
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                int power = 11;
                string shout() { return "global"; }
                class Plain {
                    int other = 0;
                    string go() { power += 1; return shout() + "/" + to_string(power); }
                }
                auto p = Plain();
                p.go()
            )");
            check_eq(std::string("global/12"), r.as<std::string>());
        });

        test("member_shadow_hot_ladder_warm", [this]() {
            // IC-warmth: bare shadowed field reads/writes in a hot loop must stay on
            // the member after the per-site caches arm.
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                int acc = 100000;
                class W {
                    int acc = 0;
                    int spin() {
                        for (int i = 0; i < 1000; i++) { acc += 1; }
                        return acc;
                    }
                }
                auto w = W();
                to_string(w.spin()) + "/" + to_string(acc)
            )");
            check_eq(std::string("1000/100000"), r.as<std::string>());
        });

        test("bare_method_call_hot_loop_stays_member", [this]() {
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                int step() { return 1000; }
                class Walker {
                    int steps = 0;
                    int step() { return 1; }
                    int walk() {
                        int sum = 0;
                        for (int i = 0; i < 100; i++) { sum += step(); }
                        return sum;
                    }
                }
                auto wk = Walker();
                wk.walk()
            )");
            check_eq((int64_t)100, r.as_int());
        });

        test("hot_reload_flips_bare_resolution", [this]() {
            auto e = make_stdlib_engine();
            auto r1 = e->execute(R"(
                int power = 100;
                class R { int get() { int s = 0; for (int i = 0; i < 50; i++) { s = get_once(); } return s; } int get_once() { return power; } }
                auto r = R();
                r.get()
            )");
            check_eq((int64_t)100, r1.as_int());
            auto r2 = e->execute(R"(
                class R { int power = 7; int get() { int s = 0; for (int i = 0; i < 50; i++) { s = get_once(); } return s; } int get_once() { return power; } }
                r.get()
            )");
            check_eq((int64_t)7, r2.as_int());
            auto r3 = e->execute(R"(
                class R { int get() { int s = 0; for (int i = 0; i < 50; i++) { s = get_once(); } return s; } int get_once() { return power; } }
                r.get()
            )");
            check_eq((int64_t)100, r3.as_int());
        });

        test("a_refused_method_call_names_the_function_the_method_hides", [this]() {
            // a bare call in a method finds the member first (C++ lookup); when the member refuses the arguments and the
            // function it hides takes them, the refusal names the qualified call
            auto e = make_stdlib_engine();
            const auto refusal = [&](const std::string& a_source) {
                const auto report = e->check(a_source);
                for (const auto& d : report.diagnostics) {
                    if (d.severity == jai::check_diagnostic::level::error) { return d.message; }
                }
                return std::string("clean");
            };
            check_eq(std::string("'to_string' expects 0 argument(s), got 1: the method hides the function 'to_string' here; call that one as ::to_string(...)"),
                     refusal("class Money { int cents = 5; string to_string() { return \"$\" + to_string(cents); } }"));
            check_eq(std::string("'flavor' expects 0 argument(s), got 1: the method hides the function 'flavor' here; call that one as ::flavor(...)"),
                     refusal("string flavor(int n) { return \"g\"; }\nclass Tea { string flavor() { return \"m\"; } string sip() { return flavor(7); } }"));
            check_eq(std::string("argument 1 of 'show': cannot pass 'string' to 'n' declared 'int': the method hides the function 'show' here; call that one as ::show(...)"),
                     refusal("string show(string s) { return s; }\nclass A { string show(int n) { return \"m\"; } string go() { return show(\"x\"); } }"));
            check_eq(std::string("'twice' expects 0 argument(s), got 1: the method hides the function 'twice' here; call that one as ns::twice(...)"),
                     refusal("namespace ns { int twice(int n) { return n * 2; }\nclass A { int twice() { return 2; } int go() { return twice(2); } } }"));
            check_eq(std::string("'f' expects 0 argument(s), got 1"), refusal("void f() {}\nclass A { void f() {} void g() { f(1); } }"));
            check_eq(std::string("'f' expects 0 argument(s), got 1"), refusal("void f(string s) {}\nclass A { void f() {} void g() { f(1); } }"));
            check_eq(std::string("$5"), e->execute("class Money { int cents = 5; string to_string() { return \"$\" + ::to_string(cents); } }\nMoney m; m.to_string();").as_string());
        });

        test("global_qualified_escape_hatch", [this]() {
            // ::name reaches a deliberately shadowed global for every access form.
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                int hp = 100;
                string flavor(int n) { return "global " + to_string(n); }
                class Tea {
                    int hp = 5;
                    string flavor() { return "member"; }
                    string sip() { return ::flavor(7) + "/" + flavor(); }
                    int read_both() { return hp * 1000 + ::hp; }
                    void write_global() { ::hp = 42; }
                    void bump_global() { ::hp += 8; ::hp++; }
                }
                auto t = Tea();
                var a = t.sip();
                var b = t.read_both();
                t.write_global();
                t.bump_global();
                a + "|" + to_string(b) + "|" + to_string(hp) + "|" + to_string(t.hp)
            )");
            check_eq(std::string("global 7/member|5100|51|5"), r.as<std::string>());
        });

        test("global_qualified_misses_locals_and_errors", [this]() {
            auto e = make_stdlib_engine();
            e->execute("int g = 1;");
            check_eq((int64_t)1, e->execute("int f() { int g = 9; return ::g; } f()").as_int());
            check_throws([&]() { e->execute("void h() { int only_local = 3; ::only_local; } h();"); });
            check_eq((int64_t)1, e->execute("::g").as_int());
        });

        test("global_qualified_ref_arg_and_index", [this]() {
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                int ammo = 10;
                var grid = [1, 2, 3];
                void refill(int& x) { x = 77; }
                class Gun {
                    int ammo = 0;
                    var grid = [9, 9, 9];
                    int use() { refill(::ammo); return ::grid[1] + grid[1]; }
                }
                auto g = Gun();
                var v = g.use();
                to_string(v) + "|" + to_string(ammo)
            )");
            check_eq(std::string("11|77"), r.as<std::string>());
        });

        test("member_shadow_is_by_name_not_signature", [this]() {
            // C++ contract: shadowing is by NAME. A member with no viable overload
            // hides the global rather than falling back to it -- the call errors.
            const char* src = R"(
                string flavor(int n) { return "global"; }
                class Tea {
                    string flavor() { return "member"; }
                    string sip() { return flavor(42); }
                }
                auto cup = Tea();
            )";
            std::string load;
            try { make_stdlib_engine()->execute(src); } catch (const std::exception& ex) { load = ex.what(); }
            check_true(load.find("'flavor' expects 0 argument(s), got 1") != std::string::npos, "the checker sees the member hide the global: " + load);
            auto e = make_stdlib_engine();
            e->execute(std::string("[[warn_mode]]; ") + src);
            check_throws([&]() { e->execute("cup.sip();"); });
            check_eq(std::string("member"), e->execute("cup.flavor()").as<std::string>());
            check_eq(std::string("global"), e->execute("flavor(1)").as<std::string>());
        });

        test("global_read_in_method_follows_receiver_class_and_live_value", [this]() {
            // a site that resolved a global past the member probe keeps resolving it per receiver: a subclass field shadows it there
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                int LIMIT = 10;
                class A {
                    int count() {
                        int n = 0;
                        for (int i = 0; i < 20; ++i) { if (i < LIMIT) { n += 1; } }
                        return n;
                    }
                    int read() { return LIMIT + 0; }
                }
                class B : A { int LIMIT = 3; }
                A a = A();
                B b = B();
                string r = "";
                for (int k = 0; k < 3; ++k) {
                    r += to_string(a.count()) + "/" + to_string(b.count()) + "/" + to_string(a.read()) + "/" + to_string(b.read()) + " ";
                    LIMIT = LIMIT + 1;
                }
                r
            )");
            check_eq(std::string("10/3/10/3 11/3/11/3 12/3/12/3 "), r.as<std::string>());
        });

        test("class_reference_to_an_object_array_element_follows_the_slot", [this]() {
            // `P& e = ps[i]` binds the element: a store into the slot is seen through it, a subclass element binds, a removed one raises
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                class P { int x = 0; int y = 0; P(int a, int b) { x = a; y = b; } }
                class Q : P { Q(int a) : super(a, a) {} }
                string run() {
                    array<P> ps = [];
                    for (int i = 0; i < 6; ++i) { ps.push(P(i, i * 10)); }
                    ps.push(Q(7));
                    int total = 0;
                    for (int i = 0; i < 7; ++i) {
                        P& el = ps[i];
                        total += el.x + el.y;
                        ps[i] = P(100, 200);
                        total += el.x;
                    }
                    string out = to_string(total);
                    P& last = ps[6];
                    ps.pop();
                    try { int v = last.x; out += " read " + to_string(v); } catch (err) { out += " " + err; }
                    return out;
                }
                run()
            )");
            check_eq(std::string("879 Reference to a removed array element"), r.as<std::string>());
        });

        test("map_contains_map_entry_references_and_object_array_member_paths", [this]() {
            auto e = make_stdlib_engine();
            auto r = e->execute(R"(
                class E { int x = 0; E(int a) { x = a; } }
                class W { array<E> es = []; }
                string run() {
                    string out = "";
                    map<int, array<int>> m = {};
                    m[1] = [10];
                    var open = m;
                    out += to_string(m.contains(1)) + to_string(m.contains(2)) + to_string(open.contains(1)) + to_string(open.contains(3));
                    string s = "abc";
                    var os = s;
                    out += to_string(os.contains("b")) + "|";
                    array<int>& b = m[1];
                    b.push(11);
                    m[5] = [50];
                    out += to_string(b.size()) + to_string(b[1]);
                    b = [7];
                    out += to_string(m[1][0]);
                    m.erase(1);
                    try { out += to_string(b.size()); } catch (err) { out += " " + err; }
                    out += "|";
                    W w = W();
                    for (int i = 0; i < 4; ++i) { w.es.push(E(i)); }
                    int t = 0;
                    for (int k = 0; k < 4; ++k) { t += w.es[k].x; }
                    w.es[2] = E(40);
                    t += w.es[2].x;
                    out += to_string(t);
                    try { int v = w.es[9].x; out += to_string(v); } catch (err) { out += " " + err; }
                    return out;
                }
                run()
            )");
            check_eq(std::string("truefalsetruefalsetrue|2117 Reference to a removed map entry|46 Array index 9 out of bounds for array of size 4"), r.as<std::string>());
        });

        test("global_read_in_method_yields_to_a_static_added_on_reload", [this]() {
            auto e = make_stdlib_engine();
            e->execute(R"(
                int LIMIT = 10;
                class A { int read() { return LIMIT + 0; } }
                A a = A();
                int first = 0;
                for (int k = 0; k < 4; ++k) { first = a.read(); }
            )");
            check_eq((int64_t)10, e->execute("first").as_int());
            e->execute(R"(
                class A {
                    static int LIMIT = 4;
                    int read() { return LIMIT + 0; }
                }
            )");
            check_eq((int64_t)4, e->execute("a.read()").as_int());
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::comprehensive_scope_tests)
FOUNDRY_REGISTER(jai::foundry::tests::member_shadowing_tests)
