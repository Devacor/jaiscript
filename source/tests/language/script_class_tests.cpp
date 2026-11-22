#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/detail/foreign_keyword.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"
#include <array>
#include <cmath>

namespace jai::foundry::tests {

class script_class_tests : public suite {
public:
    script_class_tests() : suite("Script Class Tests") {}
    
    void forge_tests() override {
        // A reference names storage it does not hold, so it is never a field or an element: the parse says which handle to hold
        test("a_reference_is_never_a_field_or_an_element", [this]() {
            auto e = make_engine();
            auto diagnosed = [&](const std::string& src) {
                std::string all;
                try {
                    for (const auto& d : e->check(src).diagnostics) { all += d.message + "\n"; }
                } catch (const std::exception& ex) { all = ex.what(); }
                return all;
            };
            const std::string h = "class H { int hp = 0; }\nH g;\n";
            const std::string handles = "A reference cannot be a field: hold a shared_ptr<H> (owning) or a weak_ptr<H> (observing)";
            check_true(diagnosed(h + "class C { H& r; }").find(handles) != std::string::npos, diagnosed(h + "class C { H& r; }"));
            check_true(diagnosed(h + "class C { const H& r = g; }").find(handles) != std::string::npos, "a const reference field");
            check_true(diagnosed("class C { var& r; }").find("A reference cannot be a field: hold the value itself, or a shared_ptr<T> to the object that owns it") !=
                       std::string::npos, diagnosed("class C { var& r; }"));
            check_true(diagnosed("array<int&> xs;").find("A reference cannot be an element: hold the int itself, or a shared_ptr<T> to the object that owns it") !=
                       std::string::npos, diagnosed("array<int&> xs;"));
            check_true(diagnosed(h + "map<string, H&> m;").find("A reference cannot be an element: hold a shared_ptr<H> (owning) or a weak_ptr<H> (observing)") !=
                       std::string::npos, diagnosed(h + "map<string, H&> m;"));
            check_true(diagnosed(h + "class C { H& get() { return g; } }").empty(), "a reference-returning method stays a method");
        });

        test("class_typed_reference_declaration_aliases_the_object", [this]() {
            // `Base& b = derived;` at statement level is a reference declaration on both
            // backends (it parsed as bitwise-and plus an assignment to an undeclared name):
            // stores through it land on the object, a base-typed reference reads a derived
            // instance, and `Box& get()` keeps its reference-return meaning beside it
            auto e = make_engine();
            auto r = e->execute(R"(
                class A { int x = 1; }
                class B : A { int y = 2; }
                B b = B();
                A& a = b;
                a.x = 5;
                A& again = a;
                again.x += 1;
                class Box { int v = 0; }
                Box box = Box();
                Box& get() { return box; }
                get().v = 9;
                int through(A& r) { return r.x; }
                return [b.x, a.x, through(b), box.v, b.y];
            )");
            const auto& v = r.as_array();
            check_eq((int64_t)6, v[0].as_int(), "the store landed on the object");
            check_eq((int64_t)6, v[1].as_int(), "the reference reads the object");
            check_eq((int64_t)6, v[2].as_int());
            check_eq((int64_t)9, v[3].as_int(), "reference-return function unchanged");
            check_eq((int64_t)2, v[4].as_int());
        });

        test("classes_hold_fields_construct_and_call_methods", [this]() {
            const std::pair<const char*, int64_t> cases[] = {
                {"class Point { int x = 0; int y = 0; } auto p = Point(); p.x = 10; p.y = 20; p.x + p.y", 30},
                {"class Point { int x = 0; int y = 0; Point(int x, int y) { this.x = x; this.y = y; } } auto p = Point(5, 7); p.x * p.y", 35},
                {"class Rectangle { int width = 0; int height = 0; Rectangle(int w, int h) { this.width = w; this.height = h; } "
                 "int area() { return this.width * this.height; } int perimeter() { return 2 * (this.width + this.height); } } "
                 "auto rect = Rectangle(4, 3); rect.area() + rect.perimeter()", 12 + 14},
                // a constructor assigns a field by its bare name; a field initializer holds without one
                {"class Cat { int a = 0; Cat(int val) { a = val; } } auto cat = Cat(42); cat.a", 42},
                {"class Cat { int a = 42; } auto cat = Cat(); cat.a", 42},
                {"class Cat { int a = 0; Cat(int val) { a = val; } } auto cat = Cat(99); cat.a", 99},
            };
            check_each(cases, [&](const auto& c) {
                auto js_engine = make_engine();
                auto result = js_engine->execute(c.first);
                check(result.is_int());
                check_eq(result.as_int(), c.second);
            });
        });

        test("script_cpp_interop", [this]() {
            auto js_engine = make_engine();
            
            js_engine->add_function("host_sqrt", [](double x) { return std::sqrt(x); });

            struct Vector2D {
                double x, y;
                Vector2D(double x = 0, double y = 0) : x(x), y(y) {}
                double magnitude() const { return std::sqrt(x*x + y*y); }
            };
            
            dynamic_binder<Vector2D>(*js_engine, "Vector2D")
                .constructor<double, double>()
                .property("x", &Vector2D::x)
                .property("y", &Vector2D::y)
                .method("magnitude", &Vector2D::magnitude)
                .build();
            
            const char* script = R"(
                class Point3D {
                    Vector2D position = Vector2D(0, 0);
                    int z = 0;
                    
                    Point3D(double x, double y, int z) {
                        this.position = Vector2D(x, y);
                        this.z = z;
                    }
                    
                    double magnitude3D() {
                        auto mag2D = this.position.magnitude();
                        return host_sqrt(mag2D * mag2D + this.z * this.z);
                    }
                }
                
                auto p = Point3D(3, 4, 12);
                p.magnitude3D()
            )";
            
            auto result = js_engine->execute(script);
            check(result.is_float());
            check_eq(result.as_float(), 13.0); // sqrt(3²+4²+12²) = sqrt(169) = 13
        });
        
        test("script_inherits_from_cpp", [this]() {
            auto js_engine = make_engine();
            
            class Creature {
            public:
                std::string name;
                int health;
                
                Creature(const std::string& n, int h = 100) : name(n), health(h) {}
                
                virtual std::string attack() {
                    return name + " attacks!";
                }
                
                int getHealth() const { return health; }
            };
            
            dynamic_binder<Creature>(*js_engine, "Creature")
                .constructor<std::string>()
                .constructor<std::string, int>()
                .property("name", &Creature::name)
                .property("health", &Creature::health)
                .method("attack", &Creature::attack)
                .method("getHealth", &Creature::getHealth)
                .build();
            
            const char* script = R"(
                // Script class inheriting from C++ class
                class Dragon : Creature {
                    int firepower = 50;

                    Dragon(string name) : super(name, 200) {
                        // Dragon constructor
                    }

                    string attack() override {
                        return super::attack() + " Dragon breathes fire!";
                    }

                    int getDamage() {
                        return this.firepower + 10;
                    }
                }

                auto dragon = Dragon("Smaug");
                dragon.getDamage()
            )";
            
            auto result = js_engine->execute(script);
            check(result.is_int());
            check_eq(result.as_int(), 60); // firepower(50) + 10
        });
        
        test("a_bare_name_in_a_method_resolves_locals_then_members_then_globals", [this]() {
            // the order holds through every lane the field caches take: a member beats a global defined after the method ran, a
            // local beats the member and ::name reaches the global
            auto e = make_engine();
            stdlib::register_all(*e);
            e->execute(R"(
                int x = 100;
                class A {
                    int x = 1;
                    int y = 2;
                    int read() { return x; }
                    int read_global() { return ::x; }
                    int read_local() { int x = 7; return x; }
                    int read_y() { return y; }
                    void bump() { x += 1; y = y * 3; }
                }
                A a = A();
                int warm = 0;
                for (int i = 0; i < 50; i++) { warm += a.read() + a.read_y(); }
            )");
            e->execute("int y = 99;");
            check_eq(std::string("[1, 100, 7, 2]"), e->execute("[a.read(), a.read_global(), a.read_local(), a.read_y()]").to_string());
            e->execute("for (int i = 0; i < 3; i++) { a.bump(); }");
            check_eq(std::string("[4, 54, 100, 99]"), e->execute("[a.read(), a.read_y(), x, y]").to_string(), "stores land on the fields");
        });

        test("a_base_method_reads_and_writes_its_fields_on_every_subclass_layout", [this]() {
            // single and multiple inheritance put the base's fields at other slots in the derived class; the field caches follow
            // the receiver's class, alternating classes at one site included
            auto e = make_engine();
            stdlib::register_all(*e);
            auto r = e->execute(R"(
                class P { int p = 10; }
                class Q {
                    int q = 2;
                    int getq() { return q; }
                    void setq(int v) { q = v; }
                    int twice() { return q + q; }
                }
                class R : P, Q { int r = 3; }
                class S : Q { int s0 = 0; int s1 = 0; }
                array<Q> all = [Q(), R(), S(), R(), Q()];
                int sum = 0;
                for (int round = 0; round < 40; round++) {
                    for (int i = 0; i < all.size(); i++) {
                        all[i].setq(i + round);
                        sum += all[i].getq() + all[i].twice();
                    }
                }
                return [sum, all[1].getq(), all[2].getq()];
            )");
            check_eq(std::string("[12900, 40, 41]"), r.to_string());
        });

        test("a_hybrid_methods_bare_names_reach_script_fields_and_cpp_properties", [this]() {
            struct Body { int hp = 5; std::string tag = "c"; };
            auto e = make_engine();
            stdlib::register_all(*e);
            dynamic_binder<Body>(*e, "Body")
                .property("hp", &Body::hp)
                .property("tag", &Body::tag)
                .build();
            auto r = e->execute(R"(
                class Hero : Body {
                    int power = 3;
                    int score() { return hp * 10 + power; }
                    void hit() { hp = hp - 1; power += 2; }
                    string label() { return tag + to_string(power); }
                }
                Hero h = Hero();
                int total = 0;
                for (int i = 0; i < 3; i++) { h.hit(); total += h.score(); }
                return [total, h.hp, h.label()];
            )");
            check_eq(std::string("[111, 2, \"c9\"]"), r.to_string());
        });

        test("a_class_reshaped_while_its_method_runs_reads_the_new_fields", [this]() {
            // the host redefines the class mid-method (a hot reload adds a field in front and drops one): the running body's
            // next bare reads follow the migrated instance, and a name whose field went away resolves to the global
            auto e = make_engine();
            stdlib::register_all(*e);
            jai::engine* raw = e.get();
            e->add_function("reshape", [raw]() { raw->execute("class M { int added = 40; int a = 1; int run() { return a; } }"); });
            auto r = e->execute(R"(
                int b = 500;
                class M {
                    int a = 1;
                    int b = 2;
                    int run() {
                        int before = a + b;
                        reshape();
                        int after = a + b;
                        return before * 1000 + after;
                    }
                }
                M m = M();
                int warm = 0;
                for (int i = 0; i < 20; i++) { warm += m.a + m.b; }
                return m.run();
            )");
            check_eq((int64_t)3501, r.as_int());
        });

        test("per_class_guard_facts_follow_every_receiver_class_and_redefinition", [this]() {
            // a base method's bare names on alternating subclasses (the member-miss memo's second class) and a grafted call on several
            // receiver classes (the graft guard's per-class rows); a subclass redefined to declare a method of the bare name takes it
            // from then on
            auto e = make_engine();
            stdlib::register_all(*e);
            e->execute(R"(
                int helper(int k) { return k * 10; }
                class Shape { int side = 1; int area() { return side * side; } int report(int k) { return helper(k) + area(); } }
                class Square : Shape { int extra = 0; }
                class Tile : Shape { int pad0 = 0; int pad1 = 0; }
                class Cube : Shape { int depth = 2; }
                array<Shape> all = [Shape(), Square(), Tile(), Cube(), Square()];
                int run_all(int rounds) { int s = 0; for (int r = 0; r < rounds; r++) { for (int i = 0; i < all.size(); i++) { all[i].side = i + 1; s += all[i].report(r); } } return s; }
            )");
            check_eq((int64_t)775, e->execute("run_all(5)").as_int(), "every class at the sites");
            e->execute("class Tile : Shape { int pad0 = 0; int pad1 = 0; int helper(int k) { return 1000; } }");
            check_eq((int64_t)5675, e->execute("run_all(5)").as_int(), "the redefined class's own helper wins over the global");
        });

        test("remembered_comparison_and_handle_facts_follow_redefinition", [this]() {
            // object == / != without operators, then a redefinition that declares them; a subclass handle into a base parameter, then a
            // redefinition that drops the base from the chain
            auto e = make_engine();
            stdlib::register_all(*e);
            e->execute(R"(
                class Base { int v = 1; }
                class Leaf : Base { int w = 2; }
                class Other { int v = 3; }
                int take(shared_ptr<Base> b) { return b.v; }
                shared_ptr<Leaf> L = make_shared<Leaf>();
                shared_ptr<Base> B = make_shared<Base>();
                int count_ne(int rounds) { int s = 0; for (int i = 0; i < rounds; i++) { if (L != B) { s += 1; } if (L == L) { s += 10; } } return s; }
                int count_take(int rounds) { int s = 0; for (int i = 0; i < rounds; i++) { s += take(L); } return s; }
            )");
            check_eq((int64_t)110, e->execute("count_ne(10)").as_int(), "identity comparisons");
            check_eq((int64_t)10, e->execute("count_take(10)").as_int(), "a subclass handle into a base parameter");
            e->execute("class Leaf : Base { int w = 2; bool operator!=(const shared_ptr<Base>& o) { return false; } bool operator==(const shared_ptr<Leaf>& o) { return false; } }");
            check_eq((int64_t)100, e->execute("count_ne(10)").as_int(), "the redefined class's operator!= answers");
            std::string message;
            e->execute("class Leaf : Other { int w = 2; }");
            try { e->execute("count_take(1)"); } catch (const std::exception& ex) { message = ex.what(); }
            check(!message.empty(), "a class no longer derived from Base is refused: " + message);
        });

        test("a_method_body_reads_its_receivers_fields_by_slot_across_reshapes_and_reclassing", [this]() {
            // a method body's bare fields read and written by slot: a receiver re-classed mid-call (extend), a class that gains a method
            // named like a field mid-call, and the same method on subclasses with fields of their own
            auto e = make_engine();
            stdlib::register_all(*e);
            jai::engine* raw = e.get();
            auto r = e->execute(R"(
                class K {
                    int a = 1;
                    int b = 2;
                    int sum() { return a + b; }
                    void bump() { a = a + 10; b += 1; }
                    int reclass_mid() { int before = a + b; extend(this, Mix); return before * 1000 + a + b; }
                }
                class K2 : K { int c = 5; int sum2() { return sum() + c; } }
                class Mix : K { int extra = 7; int sum() override { return extra; } }
                K k = K();
                K2 k2 = K2();
                int s = 0;
                for (int i = 0; i < 30; i++) { k.bump(); k2.bump(); s += k.sum() + k2.sum2(); }
                K m = K();
                int mid = m.reclass_mid();
                return [s, k.a, k2.b, mid];
            )");
            check_eq(std::string("[10560, 301, 32, 3003]"), r.to_string(), "slot reads across subclasses and a mid-call re-class");
        });

        test("a_method_body_stores_and_declares_handles_and_reads_through_its_fields", [this]() {
            // a bare handle field stored from its own member (`cur = cur.next`), handle locals declared from null and from the field, an int
            // field stored and converted into a float field, a member read through a bare field reaching a null (the raise's text), a
            // subclass handle with its own getter stored through a typed parameter, and the destructors' order as each handle lets go; a
            // method called on a local receiver and on a program-scope one
            auto e = make_engine();
            stdlib::register_all(*e);
            auto r = e->execute(R"(
                string log = "";
                class Node { shared_ptr<Node> next = null; int id = 0; Node(int a) { id = a; } ~Node() { log = `${log}~${id}`; } }
                class Special : Node { int extra = 9; Special(int a) : super(a) {} function _get_id() -> int { return 100; } }
                class Walker {
                    shared_ptr<Node> cur = null; int steps = 0; int state = 1; float f = 0.5;
                    int walk(int n) {
                        int sum = 0;
                        for (int i = 0; i < n; ++i) {
                            if (cur == null) { break; }
                            sum = sum + cur.id;
                            shared_ptr<Node> here = cur;
                            shared_ptr<Node> none = null;
                            cur = cur.next;
                            state = state * 2 + 1;
                            f = state;
                            steps = steps + 1;
                        }
                        return sum;
                    }
                    string missing() { string err = ""; try { int x = cur.id; } catch (e) { err = e; } return err; }
                    void drop() { cur = null; }
                    void retarget(shared_ptr<Special> s) { cur = s; }
                }
                string run() {
                    shared_ptr<Node> a = new Node(1); shared_ptr<Node> b = new Special(2); shared_ptr<Node> c = new Node(3);
                    a.next = b; b.next = c;
                    Walker w = Walker(); w.cur = a; a = null;
                    int s1 = w.walk(2); log = `${log}|`;
                    int s2 = w.walk(5);
                    string err = w.missing();
                    shared_ptr<Special> sp = new Special(7); w.retarget(sp); sp = null;
                    int s3 = w.walk(1);
                    b = null; c = null; w.drop();
                    return `${s1},${s2},${s3},${w.steps},${w.state},${w.f}|${err}|${log}`;
                }
                string first = run();
                Walker g = Walker(); g.cur = new Node(5);
                int t = g.walk(3); g.drop();
                return `${first}|${t},${g.state}|${log}`;
            )");
            check_eq(std::string("101,3,100,4,31,31.0|Cannot access 'id' on null ('cur' is null)|~1|~7~2~3|5,3|~1|~7~2~3~5"), r.to_string(),
                     "handle stores, declarations and member reads through bare fields");
        });

        test("a_method_bodys_program_scope_names_yield_to_a_receivers_members", [this]() {
            // a bare program-scope name in a method reads the program scope until the receiver's class takes the name: a subclass field, a
            // subclass static, then a reload of the class itself adding a field of the name
            auto e = make_engine();
            stdlib::register_all(*e);
            auto r = e->execute(R"(
                int G = 10;
                const int K = 3;
                class A { int n = 1; int f() { return G + K * n; } int g() { return G; } }
                class B : A { int G = 100; }
                class C : A { static int K = 7; }
                A a = A(); B b = B(); C c = C();
                int s = 0;
                for (int i = 0; i < 3; ++i) { s = s + a.f() + b.f() + c.f(); G = G + 1; }
                return `${s} ${a.f()} ${b.f()} ${c.f()} ${b.g()}`;
            )");
            check_eq(std::string("405 16 103 20 100"), r.to_string(), "the program scope's names and the members that take them");
            e->execute("class A { int n = 1; int K = 50; int f() { return G + K * n; } int g() { return G; } }");
            auto r2 = e->execute("return `${a.f()} ${b.f()} ${c.f()}`;");
            check_eq(std::string("63 150 63"), r2.to_string(), "a reloaded class's own field of the name");
        });

        test("handle_field_stores_follow_a_reload_that_moves_the_fields", [this]() {
            // the walker's class is redefined with its fields in another order and a field of another handle type in between: the next
            // calls store and read by the new layout
            auto e = make_engine();
            stdlib::register_all(*e);
            e->execute(R"(
                class Node { shared_ptr<Node> next = null; int id = 0; Node(int a) { id = a; } }
                class Walker { shared_ptr<Node> cur = null; int steps = 0;
                    int walk(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + cur.id; cur = cur.next; steps = steps + 1; } return s; } }
                shared_ptr<Node> a = new Node(1); shared_ptr<Node> b = new Node(2); a.next = b; b.next = a;
                Walker w = Walker(); w.cur = a;
                int first = w.walk(3);
            )");
            e->execute(R"(
                class Walker { int steps = 0; shared_ptr<Node> spare = null; shared_ptr<Node> cur = null;
                    int walk(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + cur.id * 10; spare = cur; cur = cur.next; steps = steps + 1; } return s; } }
            )");
            auto r = e->execute("int second = w.walk(3); a.next = null; return [first, second, w.steps, w.spare.id, w.cur.id];");
            check_eq(std::string("[4, 50, 6, 2, 1]"), r.to_string(), "the reloaded layout's stores and reads");
        });

        test("a_method_bodys_returns_and_scalar_stores_follow_its_receivers_layout", [this]() {
            // bare int, float and var fields stored from arithmetic in a loop (a var field holding an int taking a float, one holding a float
            // taking an int), an int field taking a float's cast, bare fields and a program-scope name returned, on the class and on a subclass that takes the name as a field;
            // then a reload that moves the fields and makes the name a field of the class itself
            auto e = make_engine();
            stdlib::register_all(*e);
            auto r = e->execute(R"(
                int G = 7;
                class Acc { int n = 0; float f = 0.5; var v = 1; var u = 1.5;
                    void step(int k) { for (int i = 0; i < k; ++i) { n = n + 2; f = f + 1.5; v = v + 1; } }
                    void blend() { for (int i = 0; i < 2; ++i) { v = v + 0.25; n = cast<int>(f * 2.0); u = i + 3; } }
                    int get() { return n; } float getf() { return f; } var getv() { return v; } int glob() { return G; } }
                class Sub : Acc { int G = 70; }
                Acc a = Acc(); Sub b = Sub();
                a.step(3); b.step(2);
                string before = `${a.get()} ${a.getf()} ${a.getv()} ${b.get()} ${b.glob()} ${a.glob()}`;
                a.blend();
                return `${before} | ${a.get()} ${a.getv()} ${a.u}`;
            )");
            check_eq(std::string("6 5.0 4 4 70 7 | 10 4.5 4"), r.to_string(), "returns and stores through the fields");
            e->execute(R"(
                class Acc { var v = 1; int G = 30; float f = 0.5; int n = 0;
                    void step(int k) { for (int i = 0; i < k; ++i) { n = n + 3; f = f + 1.0; v = v + 2; } }
                    int get() { return n; } float getf() { return f; } var getv() { return v; } int glob() { return G; } }
            )");
            auto r2 = e->execute(R"(Acc c = Acc(); c.step(2); return `${c.get()} ${c.getf()} ${c.getv()} ${c.glob()} ${b.glob()}`;)");
            check_eq(std::string("6 2.5 5 30 70"), r2.to_string(), "the reloaded layout's returns and stores");
        });

        test("a_bare_sibling_call_follows_its_receivers_class", [this]() {
            // a bare call of a method of the body's class, in a loop and in an expression: a subclass's override answers on its instances,
            // a program-scope function of the name never does, and a reload of the method is the next call's
            auto e = make_engine();
            stdlib::register_all(*e);
            auto r = e->execute(R"(
                int sib() { return 1000; }
                class A { int k = 1; int sib() { return k; }
                    int run(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + sib(); k = k + 1; } return s; }
                    int twice() { return sib() * 10 + sib(); } }
                class B : A { int sib() override { return 5; } }
                A a = A(); B b = B();
                return [a.run(3), b.run(3), a.twice(), b.twice(), sib()];
            )");
            check_eq(std::string("[6, 15, 44, 55, 1000]"), r.to_string(), "the receiver's class answers the bare call");
            e->execute("class A { int k = 1; int sib() { return k * 100; } int run(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + sib(); k = k + 1; } return s; } int twice() { return sib() * 10 + sib(); } }");
            auto r2 = e->execute("A c = A(); return [c.run(2), c.twice()];");
            check_eq(std::string("[300, 3300]"), r2.to_string(), "the reloaded method answers");
        });

        test("method_frames_share_records_with_every_other_frame_kind", [this]() {
            // plain method calls in turn with a static method, a constructor, a lambda calling a method, a free function taking the receiver
            // by reference, a method calling siblings, and recursion that throws through the method frames: the results, the caught text and
            // the escaping trace's frames
            auto e = make_engine();
            stdlib::register_all(*e);
            auto r = e->execute(R"(
                class Counter { int n = 0; static int made = 0;
                    Counter() { made = made + 1; }
                    int bump() { n = n + 1; return n; }
                    static int twice(int x) { return x * 2; }
                    int deep(int k) { if (k == 0) { throw `bottom ${n}`; } return deep(k - 1) + 1; }
                    int chain() { return bump() + twice(n) + helper(this); } }
                int helper(Counter& c) { return c.n; }
                string run() {
                    Counter c = Counter();
                    var f = [&](int x) { return x + c.bump(); };
                    int s = 0;
                    string caught = "";
                    for (int i = 0; i < 3; ++i) {
                        s = s + c.bump() + Counter::twice(i) + f(i) + c.chain();
                        Counter d = Counter();
                        s = s + d.bump();
                        try { c.deep(2); } catch (e) { caught = `${e}`; }
                    }
                    return `${s} ${c.n} ${Counter::made} ${caught}`;
                }
                return run();
            )");
            check_eq(std::string("111 9 4 bottom 9"), r.to_string(), "the results through the shared records");
            std::string shape;
            try { e->execute("Counter k = Counter(); k.bump(); k.deep(3);"); }
            catch (const std::exception&) {
                for (const auto& fr : e->last_stack_trace()) { shape += (shape.empty() ? "" : "|") + fr.function + ":" + std::to_string(fr.line); }
            }
            check_eq(std::string("deep:6|deep:6|deep:6|deep:6|<script>:1"), shape, "the frames of the escaping throw");
        });

        test("the_call_instructions_enter_lean_frames_on_every_shape", [this]() {
            // a polymorphic site over three classes, a reference receiver, bare sibling calls with and without arguments, a free
            // function called with none, a throw through those frames, a method redefined between calls and while one runs, the
            // depth limit through method and sibling frames, and the budget over a loop of calls
            auto e = make_engine();
            stdlib::register_all(*e);
            jai::engine* raw = e.get();
            e->add_function("redefine", [raw]() { raw->execute("class Sq : Shape { override int area() { return side * side * 100; } }"); });
            auto r = e->execute(R"(
                int seven() { return 7; }
                class Shape { int side = 2; int area() { return 0; } int twice() { return area() * 2; } int scaled(int k) { return area() * k; }
                    int probe() { return seven() + twice(); } int sink(int k) { if (k == 0) { throw `sink ${side}`; } return sink(k - 1); } }
                class Sq : Shape { override int area() { return side * side; } }
                class Tri : Shape { override int area() { return side * side / 2; } }
                class Hex : Shape { override int area() { return side * side * 3; } }
                int sweep(array<Shape>& all) {
                    int s = 0;
                    for (int i = 0; i < 12; ++i) {
                        Shape& r = all[i % 3];
                        s += all[i % 3].area() + r.twice() + r.scaled(i) + r.probe();
                    }
                    return s;
                }
                array<Shape> all = [Sq(), Tri(), Hex()];
                int before = sweep(all);
                string caught = "";
                try { all[1].sink(3); } catch (x) { caught = `${x}`; }
                class Run { int go(Shape& s) { int a = s.area(); redefine(); return a * 1000 + s.area(); } }
                int mid = Run().go(all[0]);
                return `${before} ${caught} ${mid} ${sweep(all)} ${seven()}`;
            )");
            check_eq(std::string("872 sink 2 4400 15920 7"), r.to_string(), "the results through every lane");
            std::string shape;
            try { e->execute("Shape t = Tri(); t.sink(2);"); }
            catch (const std::exception&) {
                for (const auto& fr : e->last_stack_trace()) { shape += (shape.empty() ? "" : "|") + fr.function + ":" + std::to_string(fr.line); }
            }
            check_eq(std::string("sink:4|sink:4|sink:4|<script>:1"), shape, "the frames of the escaping throw");
            e->max_call_depth(jai::backend_type::vm, 200);
            e->max_call_depth(jai::backend_type::interpreter, 200);
            auto deep = e->execute(R"(
                class Deep { int down(int k) { return k == 0 ? 0 : down(k - 1) + 1; } int bare() { return again(); } int again() { return bare(); } }
                string a = "";
                string b = "";
                try { Deep().down(1000); } catch (x) { a = `${x}`; }
                try { Deep().bare(); } catch (x) { b = `${x}`; }
                return `${a} | ${b} | ${Deep().down(50)}`;
            )");
            check_eq(std::string("Maximum recursion depth exceeded - possible infinite recursion | Maximum recursion depth exceeded - possible infinite recursion | 50"),
                deep.to_string(),
                "the depth limit through method and sibling frames");
            e->execution_budget(0.05);
            check_throws([&]() { e->execute("class Spin { int n = 0; void bump() { n = n + 1; } int get() { return n; } } Spin s = Spin(); while (true) { s.bump(); s.get(); }"); });
            check_eq((int64_t)4, e->execute("2 + 2").as_int());
        });

        test("a_method_calls_result_lands_in_its_consumers_operand", [this]() {
            // method results as arithmetic operands: int and float returns into typed locals, a var return of another kind through a
            // subclass, a mutating method between two reads of the same receiver (left to right), builtin calls on both sides of one binary,
            // and a method that throws from inside the binary
            auto e = make_engine();
            stdlib::register_all(*e);
            auto r = e->execute(R"(
                class P { int v = 3; float w = 0.25; int get() { return v; } float getw() { return w; } var any() { return v; }
                    int bump() { v = v + 1; return v; } int fail() { throw `bad ${v}`; return 0; } }
                class Q : P { var any() override { return 1.5; } }
                string run() {
                    P p = P(); shared_ptr<P> q = new Q();
                    int s = 0; float t = 0.0;
                    for (int i = 0; i < 4; ++i) { s = s + p.get(); t = t + p.getw(); s = s * 1 + p.bump(); }
                    var x = 1 + p.any();
                    var y = 1 + q.any();
                    int z = p.get() + p.bump() + p.get();
                    array<int> arr = [1, 2, 3];
                    int sz = arr.size() + arr.pop();
                    string err = "";
                    try { int u = s + p.fail(); } catch (e) { err = e; }
                    return `${s} ${t} ${x} ${y} ${z} ${sz} ${arr.size()} ${err}`;
                }
                return run();
            )");
            check_eq(std::string("40 1.0 8 2.5 23 6 2 bad 8"), r.to_string(), "the results the consumers read");
        });

        test("class_destructor_basic", [this]() {
            auto js_engine = make_engine();
            stdlib::register_all(*js_engine);
            
            int destructor_count = 0;

            // Use getter/setter functions instead of direct variable binding
            js_engine->add_function("get_destructor_count", [&destructor_count]() { return destructor_count; });
            js_engine->add_function("inc_destructor_count", [&destructor_count]() { destructor_count++; });
            
            const char* script = R"(
                class Resource {
                    string name = "";
                    
                    Resource(string n) {
                        name = n;
                        print("Resource " + name + " created");
                    }
                    
                    ~Resource() {
                        print("Resource " + name + " destroyed");
                        inc_destructor_count();
                    }
                }

                // Test 1: Simple destruction when variable goes out of scope
                {
                    auto r1 = Resource("file1");
                    auto r2 = Resource("file2");
                }

                // Check that both destructors were called
                auto count_after_scope = get_destructor_count();

                // Test 2: Destruction on reassignment
                var r = Resource("temp");
                r = Resource("replacement"); // Should destroy "temp"
                auto count_after_reassign = get_destructor_count();

                // Test 3: Destruction on null assignment
                r = null; // Should destroy "replacement"
                auto count_after_null = get_destructor_count();
                
                [count_after_scope, count_after_reassign, count_after_null]
            )";
            
            auto result = js_engine->execute(script);
            check(result.is_array());
            auto arr = result.as_array();
            check_eq(arr.size(), 3);

            std::cout << "Destructor counts: [" << arr[0].as_int() << ", " << arr[1].as_int() << ", " << arr[2].as_int() << "]" << std::endl;
            std::cout << "Expected: [2, 3, 4]" << std::endl;

            check_eq(arr[0].as_int(), 2); // Two destructors after scope
            check_eq(arr[1].as_int(), 3); // One more after reassignment
            check_eq(arr[2].as_int(), 4); // One more after null assignment
        });
        
        test("class_destructor_polymorphic", [this]() {
            auto js_engine = make_engine();
            stdlib::register_all(*js_engine);

            const char* script = R"JAI(
                auto base_destructor_count = 0;
                auto derived_destructor_count = 0;

                class BaseResource {
                    string type = "base";

                    BaseResource() {
                        print("BaseResource created");
                    }

                    ~BaseResource() {
                        print("BaseResource destroyed");
                        base_destructor_count = base_destructor_count + 1;
                    }
                }

                class FileResource : BaseResource {
                    string filename = "";

                    FileResource(string name) : super() {
                        type = "file";
                        filename = name;
                        print("FileResource " + filename + " created");
                    }

                    ~FileResource() {
                        print("FileResource " + filename + " destroyed");
                        derived_destructor_count = derived_destructor_count + 1;
                        // Note: Base destructor should be called automatically after this
                    }
                }

                // Test polymorphic destruction through base pointer
                auto mid_base = 0;
                auto mid_derived = 0;

                {
                    print("=== Test REVERSED order ===");
                    print("Creating file_ref FIRST");
                    auto file_ref = FileResource("data.txt");
                    print("Creating base_ref SECOND");
                    auto base_ref = BaseResource();
                    print("About to exit scope - should destroy base_ref then file_ref");
                }

                print("After scope exit - base: " + to_string(base_destructor_count) + ", derived: " + to_string(derived_destructor_count));
                auto final_base = base_destructor_count;
                auto final_derived = derived_destructor_count;

                [final_base, final_derived]
            )JAI";
            
            auto result = js_engine->execute(script);
            check(result.is_array());
            auto arr = result.as_array();
            check_eq(arr.size(), 2);

            // After scope exit: both objects should be destroyed
            // base_ref: 1 BaseResource destructor
            // file_ref: 1 FileResource destructor + 1 BaseResource (base part) destructor
            // Total: 2 base destructors, 1 derived destructor
            check_eq(arr[0].as_int(), 2); // Base destructor called twice
            check_eq(arr[1].as_int(), 1); // Derived destructor called once
        });

        test("class_destructor_nested_scopes", [this]() {
            auto js_engine = make_engine();
            stdlib::register_all(*js_engine);

            std::string order_log;
            js_engine->add_function("log_event", [&order_log](const std::string& event) {
                if (!order_log.empty()) order_log += ",";
                order_log += event;
            });

            const char* script = R"(
                class Tracker {
                    string name = "";

                    Tracker(string n) {
                        name = n;
                        log_event(name + "_ctor");
                    }

                    ~Tracker() {
                        log_event(name + "_dtor");
                    }
                }

                // Test nested scopes with proper LIFO destruction order
                {
                    auto outer1 = Tracker("outer1");
                    {
                        auto inner1 = Tracker("inner1");
                        auto inner2 = Tracker("inner2");
                        {
                            auto deep1 = Tracker("deep1");
                        } // deep1 destroyed here
                        auto inner3 = Tracker("inner3");
                    } // inner3, inner2, inner1 destroyed here (LIFO)
                    auto outer2 = Tracker("outer2");
                } // outer2, outer1 destroyed here (LIFO)

                // Test multiple objects in same scope
                {
                    auto a = Tracker("a");
                    auto b = Tracker("b");
                    auto c = Tracker("c");
                } // c, b, a destroyed in reverse order (LIFO)

                "done"
            )";

            auto result = js_engine->execute(script);
            check(result.is_string());
            check_eq(result.as_string(), "done");

            // Verify exact destruction order
            std::string expected =
                "outer1_ctor,"
                "inner1_ctor,inner2_ctor,"
                "deep1_ctor,deep1_dtor,"
                "inner3_ctor,"
                "inner3_dtor,inner2_dtor,inner1_dtor,"
                "outer2_ctor,"
                "outer2_dtor,outer1_dtor,"
                "a_ctor,b_ctor,c_ctor,"
                "c_dtor,b_dtor,a_dtor";

            std::cout << "Destruction order log:\n" << order_log << std::endl;
            std::cout << "Expected:\n" << expected << std::endl;

            check_eq(order_log, expected);
        });

        test("class_destructor_in_containers", [this]() {
            auto js_engine = make_engine();
            stdlib::register_all(*js_engine);

            const char* script = R"JAI(
                auto global_count = 0;

                class CountedObject {
                    int id = 0;

                    CountedObject(int i) {
                        id = i;
                        global_count = global_count + 1;
                        print("Object " + to_string(id) + " created (count=" + to_string(global_count) + ")");
                    }

                    ~CountedObject() {
                        global_count = global_count - 1;
                        print("Object " + to_string(id) + " destroyed (count=" + to_string(global_count) + ")");
                    }
                }

                // Test destruction in arrays
                auto initial_count = global_count; // Should be 0

                auto count_with_array = 0;
                auto count_after_clear = 0;

                {
                    auto objects = [
                        CountedObject(1),
                        CountedObject(2),
                        CountedObject(3)
                    ];
                    count_with_array = global_count; // Should be 3

                    // Clear array
                    objects = [];
                    count_after_clear = global_count; // Should be 0
                }

                // Test destruction in maps
                auto count_with_map = 0;
                {
                    auto object_map = {
                        "first": CountedObject(10),
                        "second": CountedObject(20)
                    };
                    count_with_map = global_count; // Should be 2
                }

                auto final_count = global_count; // Should be 0

                [initial_count, count_with_array, count_after_clear, count_with_map, final_count]
            )JAI";

            auto result = js_engine->execute(script);
            check(result.is_array());
            auto arr = result.as_array();
            check_eq(arr.size(), 5);

            std::cout << "Container destructor counts: [" << arr[0].as_int() << ", " << arr[1].as_int() << ", "
                      << arr[2].as_int() << ", " << arr[3].as_int() << ", " << arr[4].as_int() << "]" << std::endl;
            std::cout << "Expected: [0, 3, 0, 2, 0]" << std::endl;

            check_eq(arr[0].as_int(), 0); // Initial count
            check_eq(arr[1].as_int(), 3); // 3 objects in array
            check_eq(arr[2].as_int(), 0); // All destroyed after clear
            check_eq(arr[3].as_int(), 2); // 2 objects in map
            check_eq(arr[4].as_int(), 0); // All destroyed after scope
        });

        // C++ semantics: every base contributes its fields and constructor; static members stay with the class that declares them
        test("multiple_inheritance_builds_every_base", [this]() {
            const std::pair<const char*, std::vector<int64_t>> cases[] = {
                {"class A { int x = 1; } class B { int y = 2; } class C : A, B { int z = 3; } auto c = C(); [c.x, c.y, c.z]", {1, 2, 3}},
                {"class A { int x = 0; A(int val) { this.x = val; } } class B { int y = 0; B(int val) { this.y = val; } } "
                 "class C : A, B { int z = 0; C(int a, int b, int c) : super(a), B(b) { this.z = c; } } auto c = C(1, 2, 3); "
                 "[c.x, c.y, c.z]", {1, 2, 3}},
                {"class A { int a = 1; } class B : A { int b = 2; } class C { int c = 3; } class D : B, C { int d = 4; } auto obj = D(); [obj.a, obj.b, obj.c, obj.d]", {1, 2, 3, 4}},
                {"class A { int a = 100; } class B { int b = 200; } class C : A, B { int c = 300; } auto obj = C(); [obj.a, obj.b, obj.c]", {100, 200, 300}},
                {"class A { static int static_a = 100; } class B { static int static_b = 200; } "
                 "class C : A, B { static int static_c = 300; } [A::static_a, B::static_b, C::static_c]", {100, 200, 300}},
            };
            check_each(cases, [&](const auto& c) {
                auto js_engine = make_engine();
                auto result = js_engine->execute(c.first);
                check(result.is_array());
                auto arr = result.as_array();
                check_eq(arr.size(), c.second.size());
                for (size_t i = 0; i < c.second.size(); ++i) {
                    check_eq(arr[i].as_int(), c.second[i]);
                }
            });
        });

        test("multiple_inheritance_method_lookup", [this]() {
            auto js_engine = make_engine();

            const char* script = R"(
                class A {
                    auto get_a() { return "from A"; }
                }

                class B {
                    auto get_b() { return "from B"; }
                }

                class C : A, B {
                    auto get_c() { return "from C"; }
                }

                auto c = C();
                [c.get_a(), c.get_b(), c.get_c()]
            )";

            auto result = js_engine->execute(script);
            check(result.is_array());
            auto arr = result.as_array();
            check_eq(arr.size(), 3);
            check_eq(arr[0].as_string(), "from A");
            check_eq(arr[1].as_string(), "from B");
            check_eq(arr[2].as_string(), "from C");
        });

        test("multiple_inheritance_field_conflict_detected", [this]() {
            auto js_engine = make_engine();

            // C++ semantics: ambiguous field access should error
            // If both parent classes have a field with the same name,
            // and the derived class doesn't redefine it, that's an error
            const char* script = R"(
                class A {
                    int value = 10;
                    auto get_name() { return "A"; }
                }

                class B {
                    int value = 20;
                    auto get_name() { return "B"; }
                }

                class C : A, B {
                    // ERROR: 'value' and 'get_name' are ambiguous from A and B
                }

                auto c = C();
                c.value
            )";

            // Should throw an error about ambiguous field 'value'
            check_throws([&]() {
                js_engine->execute(script);
            }, "Expected error for ambiguous field in multiple inheritance");
        });

        test("multiple_inheritance_override_in_derived", [this]() {
            auto js_engine = make_engine();

            const char* script = R"(
                class A {
                    int value = 10;
                }

                class B {
                    int value = 20;
                }

                class C : A, B {
                    int value = 30;  // Derived class overrides
                }

                auto c = C();
                c.value
            )";

            auto result = js_engine->execute(script);
            check(result.is_int());
            check_eq(result.as_int(), 30);  // Derived class wins
        });

        test("multiple_inheritance_destructors", [this]() {
            auto js_engine = make_engine();

            const char* script = R"(
                auto log = "";

                class A {
                    ~A() { log = log + "A"; }
                }

                class B {
                    ~B() { log = log + "B"; }
                }

                class C : A, B {
                    ~C() { log = log + "C"; }
                }

                {
                    auto c = C();
                }

                log
            )";

            auto result = js_engine->execute(script);
            check(result.is_string());
            // C++: the derived destructor, then the bases in reverse declaration order
            check_eq(std::string("CBA"), result.as_string());
        });

        test("dynamic_cast_answers_by_the_run_time_class", [this]() {
            struct HostBase { int id = 1; };
            struct HostDerived : HostBase { int extra = 2; };
            const auto engine_with_host = [] {
                auto e = make_engine();
                jai::stdlib::register_all(*e);
                dynamic_binder<HostBase>(*e, "HostBase").constructor<>().property("id", &HostBase::id).build();
                dynamic_binder<HostDerived>(*e, "HostDerived").constructor<>().base_class<HostBase>().property("extra", &HostDerived::extra).build();
                return e;
            };
            const std::string classes = R"(
                class Unit { int hp = 1; }
                class Hero : Unit { }
                class Paladin : Hero { string smite() { return "smite"; } }
                class Healer { }
                class Cleric : Unit, Healer { }
                class Squire : HostDerived { }
                namespace game { class Boss : Unit { } }
                enum Dir { north, south }
            )";
            auto e = engine_with_host();
            check_eq(std::string(
                "Unit:TFFF Hero:TTFF Paladin:TTTF smite:Paladin Cleric:TFFT |TTTFTFT|TTFF|FFFFFFFFF|smite1 4|Cannot cast 'Hero' to 'Paladin&'|Cannot cast 'null' to 'Hero&'|"
                "Cannot cast 'int' to 'Hero&'|TTTF1|TF|reflect::is_instance_of: unknown class 'Nope'|7TTFF"),
                e->execute(classes + R"(
                    string flag(bool b) { return b ? "T" : "F"; }
                    string out;
                    array<shared_ptr<Unit>> party = [new Unit(), new Hero(), new Paladin(), new Cleric()];
                    for (auto u : party) {
                        out += reflect::type_of(u) + ":" + flag(dynamic_cast<Unit>(u) != null) + flag(dynamic_cast<shared_ptr<Hero>>(u) != null) +
                               flag(dynamic_cast<Paladin>(u) != null) + flag(dynamic_cast<Healer>(u) != null) + " ";
                        if (auto knight = dynamic_cast<Paladin>(u)) { out += knight.smite() + ":" + reflect::type_of(knight) + " "; }
                    }
                    // a value, a reference, a var, a null handle, a live and an expired weak handle
                    Paladin p;
                    Unit& r = p;
                    var v = p;
                    shared_ptr<Hero> none = null;
                    weak_ptr<Unit> live = party[2];
                    weak_ptr<Unit> dead;
                    { shared_ptr<Unit> gone = new Hero(); dead = gone; }
                    out += "|" + flag(dynamic_cast<Hero>(p) != null) + flag(dynamic_cast<Paladin>(r) != null) + flag(dynamic_cast<Unit>(v) != null) +
                           flag(dynamic_cast<Hero>(none) != null) + flag(dynamic_cast<Hero>(live) != null) + flag(dynamic_cast<Unit>(dead) != null) +
                           flag(same_object(dynamic_cast<Paladin>(party[2]), party[2]));
                    // a namespace's class, and the test by a name given at run time
                    game::Boss boss;
                    out += "|" + flag(dynamic_cast<game::Boss>(boss) != null) + flag(reflect::is_instance_of(boss, "Unit")) + flag(reflect::is_instance_of(boss, "Hero")) +
                           flag(reflect::is_instance_of(p, "Healer"));
                    // a var that holds no object names none, and never an error
                    var things = [null, 5, 2.5, "s", [1], {"a": 1}, [](){ return 1; }, math::vec2(1, 2), Dir::north];
                    out += "|";
                    for (var t : things) { out += flag(dynamic_cast<Unit>(t) != null || reflect::is_instance_of(t, "Unit")); }
                    // the reference form is cast<T&>: the object itself, or the cast's error
                    dynamic_cast<Unit&>(p).hp = 4;
                    out += "|" + dynamic_cast<Paladin&>(r).smite() + to_string(dynamic_cast<Hero&>(party[1]).hp) + " " + to_string(p.hp);
                    try { dynamic_cast<Paladin&>(party[1]); } catch (err) { out += "|" + err; }
                    try { dynamic_cast<Hero&>(none); } catch (err) { out += "|" + err; }
                    try { var five = 5; dynamic_cast<Hero&>(five); } catch (err) { out += "|" + err; }
                    // host classes by the bases their binders name, a script class over one, and a host object in a var
                    HostDerived hd; var held = hd; Squire squire;
                    out += "|" + flag(dynamic_cast<HostBase>(hd) != null) + flag(dynamic_cast<HostDerived>(held) != null) +
                           flag(dynamic_cast<HostBase>(squire) != null) + flag(dynamic_cast<Unit>(held) != null) + to_string(dynamic_cast<HostBase&>(squire).id);
                    out += "|" + flag(reflect::is_instance_of(5, "Unit") == false) + flag(reflect::is_instance_of(none, "Hero"));
                    try { reflect::is_instance_of(p, "Nope"); } catch (err) { out += "|" + err; }
                    // each word alone is an ordinary name
                    class Holder { int instanceof = 3; int dynamic_cast = 4; }
                    Holder holder;
                    int dynamic_pointer_cast = 0;
                    out += "|" + to_string(holder.instanceof + holder.dynamic_cast + dynamic_pointer_cast) + flag(reflect::is_instance_of(squire, "HostDerived")) +
                           flag(reflect::is_instance_of(squire, "HostBase")) + flag(reflect::is_instance_of("Hero", "Unit")) + flag(reflect::is_instance_of(dead, "Unit"));
                    out;
                )").as_string());
            const auto load_error = [&](const std::string& code) {
                auto fresh = engine_with_host();
                try { fresh->execute(classes + "Hero h; int n = 5;\n" + code); } catch (const std::exception& ex) { return std::string(ex.what()); }
                return std::string("loaded");
            };
            const auto refuses = [&](const std::string& code, const std::string& text) {
                const std::string error = load_error(code);
                check_true(error.find(text) != std::string::npos, code + " -> " + error);
            };
            const std::string forms = "error: dynamic_cast takes a class: dynamic_cast<T>(e) is e as a shared_ptr<T>, null when it is no T, and dynamic_cast<T&>(e) is the reference";
            refuses("auto b = dynamic_cast<Nope>(h);", "error: unknown type in cast: 'Nope'");
            refuses("auto b = dynamic_cast<int>(n);", forms);
            refuses("auto b = dynamic_cast<var>(h);", forms);
            refuses("auto b = dynamic_cast<array<Hero>>(h);", forms);
            refuses("auto b = dynamic_cast<shared_ptr<Hero>&>(h);", forms);
            refuses("auto b = dynamic_cast<Dir>(h);", "error: dynamic_cast<Dir> names a class, and 'Dir' is an enum: cast<Dir>(n) converts an int that is one of its values");
            refuses("auto b = dynamic_cast<Unit>(n);", "error: dynamic_cast<Unit> is always null: the operand is 'int', never an object of a class");
            refuses("dynamic_cast<Unit&>(n).hp = 2;", "error: dynamic_cast<Unit&> always raises: the operand is 'int', never an object of a class");
            refuses("auto b = dynamic_pointer_cast<Hero>(h);", std::string("error: ") + std::string(detail::foreign_word_text(detail::foreign_word::dynamic_pointer_cast_call)));
            check_eq(std::string("loaded"), load_error("bool up = dynamic_cast<Unit>(h) != null; if (dynamic_cast<Paladin>(h)) { } dynamic_cast<Unit&>(h); bool same = dynamic_cast<shared_ptr<Hero>>(h) != null;"));
            auto warned = engine_with_host()->check(classes + "Hero h; if (dynamic_cast<Healer>(h)) { }");
            check_eq((size_t)1, warned.diagnostics.size());
            check_true(!warned.has_errors() && warned.diagnostics[0].message == "dynamic_cast<Healer> is null for every 'Hero' but one whose class derives from 'Healer' too", warned.format(""));
        });

        // The VM answers reflect::is_instance_of(x, "T") with a literal name in place: each answer and error is the call's
        test("an_instance_test_by_a_literal_name_answers_as_the_call_does", [this]() {
            struct HostBase { int id = 1; };
            struct HostDerived : HostBase { int extra = 2; };
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            dynamic_binder<HostBase>(*e, "HostBase").constructor<>().property("id", &HostBase::id).build();
            dynamic_binder<HostDerived>(*e, "HostDerived").constructor<>().base_class<HostBase>().property("extra", &HostDerived::extra).build();
            e->execute(R"JAI(
                class Unit { int hp = 1; }
                class Hero : Unit { }
                class Squire : HostDerived { }
                namespace game { class Boss : Unit { } }
                string flag(bool b) { return b ? "T" : "F"; }
                string probe(var x) {
                    string out = flag(reflect::is_instance_of(x, "Unit")) + flag(reflect::is_instance_of(x, "Hero")) + flag(reflect::is_instance_of(x, "game::Boss")) +
                                 flag(reflect::is_instance_of(x, "HostBase")) + flag(reflect::is_instance_of(x, " Hero "));
                    try { out += flag(reflect::is_instance_of(x, "Late")); } catch (err) { out += "(" + err + ")"; }
                    return out;
                }
                string sweep() {
                    shared_ptr<Unit> none = null;
                    weak_ptr<Unit> gone;
                    { shared_ptr<Unit> held = new Hero(); gone = held; }
                    var things = [Unit(), Hero(), game::Boss(), HostDerived(), Squire(), none, gone, 5, "Hero"];
                    string out;
                    for (var x : things) { out += probe(x) + " "; }
                    return out;
                }
                var asked(var x) { return reflect::is_instance_of(x, "Hero"); }
                string stand_in(var x, string name) { return "script:" + name; }
                Hero first;
            )JAI");
            const std::string unknown = "(reflect::is_instance_of: unknown class 'Late')";
            check_eq("TFFFF" + unknown + " TTFFT" + unknown + " TFTFF" + unknown + " FFFTF" + unknown + " FFFTF" + unknown + " FFFFF" + unknown + " FFFFF" + unknown +
                     " FFFFF" + unknown + " FFFFF" + unknown + " ", e->execute("sweep()").as_string());
            // the sites compiled before the class existed, and before it was redefined without its base (its objects follow the definition)
            e->execute("class Late : Hero { }");
            check_eq(std::string("TTFFTT TTFFTF"), e->execute("probe(Late()) + \" \" + probe(first)").as_string());
            e->execute("class Hero { int mana = 2; }");
            check_eq(std::string("FTFFTF FTFFTF FTFFTT"), e->execute("probe(Hero()) + \" \" + probe(first) + \" \" + probe(Late())").as_string());
            // the name bound again by the host: its function is the one called, with the operand and the text
            check_true(e->execute("asked(first)").as_bool());
            e->add_global("reflect::is_instance_of", e->get_variable("stand_in"));
            check_eq(std::string("script:Hero"), e->execute("asked(first)").as_string());
            e->add_function("reflect::is_instance_of", [](int a_operand, std::string a_name) { return a_name + ":" + std::to_string(a_operand); });
            check_eq(std::string("Hero:7"), e->execute("asked(7)").as_string());
        });

        // A handle never aliases a value slot: every way a class value becomes a handle copies it, a temporary is the handle's object, a
        // handle stays shared, and a handle-form cast over a declared class value is a test or a load error
        test("a_handle_never_aliases_a_value_slot", [this]() {
            const std::string classes = R"(
                class Dog { string name = "d"; int copies = 0; Dog() {} Dog(const Dog& o) { name = o.name; copies = o.copies + 1; }
                    shared_ptr<Dog> me() { return shared_from_this(); }
                    var weak_me() { return weak_from_this(); }
                    shared_ptr<Dog> own() { shared_ptr<Dog> s = this; return s; } }
                class Pup : Dog { int bones = 1; }
                class Kennel { shared_ptr<Dog> star = null; Dog resident; array<shared_ptr<Dog>> pack; }
                string OUT = "";
            )";
            const auto fresh = [this] {
                auto e = make_engine();
                jai::stdlib::register_all(*e);
                return e;
            };
            // each body runs at the top level and inside a function, where the VM reads locals from their slots
            const std::pair<const char*, const char*> rows[] = {
                {"Dog d; shared_ptr<Dog> h = make_shared<Dog>(d); h.name = \"x\"; OUT = `${d.name} ${h.name} ${h.copies}`;", "d x 1"},
                {"Dog d; shared_ptr<auto> h = make_shared<Dog>(d); h.name = \"x\"; OUT = `${d.name} ${h.name}`;", "d x"},
                {"Pup p; shared_ptr<Dog> h = make_shared<Pup>(p); h.name = \"x\"; OUT = `${p.name} ${reflect::type_of(h)}`;", "d Pup"},
                {"shared_ptr<Dog> h = make_shared<Dog>(); OUT = to_string(h.copies);", "0"},
                {"shared_ptr<Dog> a = new Dog(); shared_ptr<Dog> b = a; b.name = \"x\"; OUT = `${a.name} ${same_object(a, b)}`;", "x true"},
                {"Dog d; var v = d; shared_ptr<Dog> h = make_shared<Dog>(v); h.name = \"x\"; OUT = `${v.name} ${h.name}`;", "d x"},
                {"shared_ptr<Dog> a = new Dog(); var v = a; shared_ptr<Dog> h = v; h.name = \"x\"; OUT = a.name;", "x"},
                {"Dog d; Dog& r = d; shared_ptr<Dog> h = make_shared<Dog>(r); h.name = \"x\"; OUT = d.name;", "d"},
                {"const Dog c; shared_ptr<Dog> h = make_shared<Dog>(c); h.name = \"x\"; OUT = c.name;", "d"},
                {"array<Dog> ds; ds.push(Dog()); shared_ptr<Dog> h = make_shared<Dog>(ds[0]); h.name = \"x\"; OUT = ds[0].name;", "d"},
                {"Kennel k; shared_ptr<Dog> h = make_shared<Dog>(k.resident); h.name = \"x\"; OUT = k.resident.name;", "d"},
                {"Dog d; shared_ptr<Dog> h = make_shared<Dog>(true ? d : d); h.name = \"x\"; OUT = d.name;", "d"},
                {"shared_ptr<Dog> a = new Dog(); auto h = a.own(); h.name = \"x\"; OUT = a.name;", "x"},
                {"Dog d; array<shared_ptr<Dog>> all; all.push(make_shared<Dog>(d)); all[0].name = \"x\"; OUT = `${d.name} ${all[0].copies}`;", "d 1"},
                {"array<shared_ptr<Dog>> all; all.push(make_shared<Dog>()); OUT = to_string(all[0].copies);", "0"},
                {"array<Dog> ds; ds.push(Dog()); array<shared_ptr<Dog>> all; all.push(make_shared<Dog>(ds[0])); all[0].name = \"x\"; OUT = ds[0].name;", "d"},
                {"Dog d; array<shared_ptr<Dog>> all = [new Dog()]; all[0] = make_shared<Dog>(d); all[0].name = \"x\"; OUT = d.name;", "d"},
                {"Dog d; array<shared_ptr<Dog>> all; all.insert(0, make_shared<Dog>(d)); all[0].name = \"x\"; OUT = d.name;", "d"},
                {"Dog d; array<shared_ptr<Dog>> all = [make_shared<Dog>(d)]; all[0].name = \"x\"; OUT = `${d.name} ${all[0].copies}`;", "d 1"},
                {"Dog d; map<string, shared_ptr<Dog>> all; all[\"k\"] = make_shared<Dog>(d); all[\"k\"].name = \"x\"; OUT = d.name;", "d"},
                {"Dog d; map<string, shared_ptr<Dog>> all = {\"k\": make_shared<Dog>(d)}; all[\"k\"].name = \"x\"; OUT = `${d.name} ${all[\"k\"].copies}`;", "d 1"},
                {"Dog d; Kennel k; k.star = make_shared<Dog>(d); k.star.name = \"x\"; OUT = d.name;", "d"},
                {"Dog d; Kennel k; k.pack.push(make_shared<Dog>(d)); k.pack[0].name = \"x\"; OUT = d.name;", "d"},
                {"array<Dog> ds; ds.push(Dog()); for (Dog h : ds) { h.name = \"x\"; OUT = h.name; } OUT = OUT + ds[0].name;", "xd"},
                // what the checker cannot call a value is judged when the line runs
                {"Dog d; var v = d; int was = v.copies; auto h = dynamic_cast<Dog>(v); h.name = \"x\"; OUT = `${v.name} ${h.name} ${h.copies - was}`;", "d x 1"},
                {"Dog d; var v = d; int was = v.copies; auto h = cast<shared_ptr<Dog>>(v); h.name = \"x\"; OUT = `${v.name} ${h.copies - was}`;", "d 1"},
                {"Dog d; Dog& r = d; auto h = dynamic_cast<Dog>(r); h.name = \"x\"; OUT = d.name;", "d"},
                {"array<Dog> ds; ds.push(Dog()); auto h = dynamic_cast<Dog>(ds[0]); h.name = \"x\"; OUT = ds[0].name;", "d"},
                {"shared_ptr<Dog> a = new Dog(); var v = a; auto h = dynamic_cast<Dog>(v); h.name = \"x\"; OUT = a.name;", "x"},
                {"shared_ptr<Dog> a = new Pup(); auto h = cast<shared_ptr<Pup>>(a); h.name = \"x\"; OUT = a.name;", "x"},
                {"shared_ptr<Dog> a = new Dog(); weak_ptr<Dog> w = a; auto h = dynamic_cast<Dog>(w); h.name = \"x\"; OUT = a.name;", "x"},
                {"auto h = dynamic_cast<Dog>(Dog()); OUT = `${reflect::type_of(h)} ${h.copies}`;", "Dog 0"},
                // read for its truth the cast is the test: it copies nothing
                {"Dog d; if (dynamic_cast<Dog>(d)) { d.name = \"x\"; } OUT = `${d.name} ${d.copies}`;", "x 0"},
                {"Dog s = Pup(); OUT = !dynamic_cast<Pup>(s) ? \"no\" : \"yes\";", "yes"},
                {"Dog d; bool b = dynamic_cast<Dog>(d) && d.copies == 0; OUT = to_string(b);", "true"},
                {"Dog d; OUT = `${dynamic_cast<Dog>(d) != null} ${null == dynamic_cast<Pup>(d)}`;", "true true"},
                {"Dog d; OUT = to_string(dynamic_cast<Dog>(d) ? 1 : 2);", "1"},
                {"Dog d; int n = 0; while (dynamic_cast<Dog>(d) && n < 2) { n += 1; } OUT = `${n} ${d.copies}`;", "2 0"},
                {"Dog d; var v = d; int was = v.copies; if (dynamic_cast<Dog>(v)) { v.name = \"x\"; } OUT = `${v.name} ${v.copies - was}`;", "x 0"},
                {"shared_ptr<Dog> a = new Dog(); auto s = a.me(); s.name = \"x\"; OUT = `${a.name} ${a.weak_me().expired()}`;", "x false"},
            };
            for (const auto& row : rows) {
                check_eq(std::string(row.second), fresh()->execute(classes + row.first + "\nOUT;").as_string(), row.first);
                check_eq(std::string(row.second), fresh()->execute(classes + "void run() { " + row.first + " }\nrun(); OUT;").as_string(), row.first);
            }
            // a class value never becomes a handle on its own, as in C++: the load refuses what it sees, the run time a var path
            const std::string into_handle_refused = "a Dog value does not become a handle: make_shared<Dog>(value) copies it into a new one";
            for (const char* form : {"Dog d; shared_ptr<Dog> h = d;", "shared_ptr<Dog> h = Dog();", "shared_ptr<Dog> h = new Dog(); Dog d; h = d;",
                                     "Dog d; array<shared_ptr<Dog>> all; all.push(d);", "Dog d; map<string, shared_ptr<Dog>> all; all[\"k\"] = d;",
                                     "Dog d; Kennel k; k.star = d;", "void take(shared_ptr<Dog> h) {} Dog d; take(d);",
                                     "shared_ptr<Dog> give() { Dog d; return d; }"}) {
                std::string all;
                try { for (const auto& d : fresh()->check(classes + form).diagnostics) { all += d.message + "\n"; } } catch (const std::exception& ex) { all = ex.what(); }
                check_true(all.find(into_handle_refused) != std::string::npos, std::string(form) + " -> " + all);
            }
            check_eq(std::string("A Dog value does not become a handle: make_shared<Dog>(value) copies it into a new one"),
                     fresh()->execute("[[warn_mode]];\n" + classes + "Dog d; var v = d; try { shared_ptr<Dog> h = v; } catch (e) { OUT = e; }\nOUT;").as_string(),
                     "the run time refuses a var path");
            check_eq((int64_t)2, fresh()->execute(classes + "int bump() { Dog s = Pup(); if (dynamic_cast<Pup>(s)) { s.bones += 1; return s.bones; } return 0; } bump();").as_int(),
                     "the test narrows the slot, and a write reaches it");
            // a constructor new or make_shared runs builds the handle's object: a handle made from its `this` shares it
            const std::string registry = R"(
                array<shared_ptr<Ent>> REG;
                shared_ptr<Ent> LAST = null;
                void keep(shared_ptr<Ent> e) { LAST = e; }
                class Ent { int n = 0; shared_ptr<Ent> self = null; Ent() { REG.push(this); self = this; keep(this); auto same = shared_from_this(); same.n = 1; } }
            )";
            check_eq(std::string("1|5|6|7|true|8"), fresh()->execute(registry + R"(
                auto e = new Ent(); string out = to_string(e.n);
                REG[0].n = 5; out += "|" + to_string(e.n);
                e.self.n = 6; out += "|" + to_string(e.n);
                LAST.n = 7; out += "|" + to_string(e.n) + "|" + to_string(same_object(e, REG[0]) && same_object(e, LAST));
                auto m = make_shared<Ent>(); REG[1].n = 8; out += "|" + to_string(m.n);
                e.self = null; m.self = null; out;
            )").as_string(), "this in a constructor new runs is the handle's object");
            check_eq(std::string("3|4"), fresh()->execute(R"(
                class Late { int n = 0; Late() { LATE.push(this); LATE_MAP["k"] = this; } }
                array<shared_ptr<Late>> LATE;
                map<string, shared_ptr<Late>> LATE_MAP;
                auto x = new Late(); LATE[0].n = 3; string out = to_string(x.n); LATE_MAP["k"].n = 4; out + "|" + to_string(x.n);
            )").as_string(), "a program-scope container declared below the constructor shares the handle's object");
            const auto refusal = [&](const std::string& body) {
                try { fresh()->execute(classes + body); } catch (const std::exception& ex) { return std::string(ex.what()); }
                return std::string("loaded");
            };
            const auto names_both = [](const std::string& cast, const std::string& operand, const std::string& cls) {
                return cast + ": '" + operand + "' is a class value, and a handle never aliases one; test it with if (dynamic_cast<" + cls + ">(" + operand +
                       ")), or take a reference with dynamic_cast<" + cls + "&>(" + operand + ")";
            };
            const std::pair<const char*, std::string> refused[] = {
                {"Dog d; auto h = dynamic_cast<shared_ptr<Dog>>(d);", names_both("dynamic_cast<Dog>(d)", "d", "Dog")},
                {"Dog d; auto h = dynamic_cast<Dog>(d);", names_both("dynamic_cast<Dog>(d)", "d", "Dog")},
                {"Dog a; Dog d = dynamic_cast<Dog>(a);", names_both("dynamic_cast<Dog>(a)", "a", "Dog")},
                {"Dog d; auto h = cast<shared_ptr<Dog>>(d);", names_both("cast<shared_ptr<Dog>>(d)", "d", "Dog")},
                {"Dog d; if (auto h = dynamic_cast<Dog>(d)) { }", names_both("dynamic_cast<Dog>(d)", "d", "Dog")},
                {"void take(var x) { } Dog d; take(dynamic_cast<Dog>(d));", names_both("dynamic_cast<Dog>(d)", "d", "Dog")},
                {"var give(Dog d) { return dynamic_cast<Dog>(d); }", names_both("dynamic_cast<Dog>(d)", "d", "Dog")},
                {"Dog d; string n = dynamic_cast<Dog>(d).name;", names_both("dynamic_cast<Dog>(d)", "d", "Dog")},
                {"Kennel k; auto h = dynamic_cast<Dog>(k.resident);", names_both("dynamic_cast<Dog>(k.resident)", "k.resident", "Dog")},
                {"Dog d; d.me();", "shared_from_this() needs an object reached through a shared_ptr, and this one is a class value"},
                {"Dog d; d.weak_me();", "weak_from_this() needs an object reached through a shared_ptr, and this one is a class value"},
                {"class Early { Early() { auto s = shared_from_this(); } } Early e;", "shared_from_this() needs an object reached through a shared_ptr, and this one is a class value"},
                // `this` into a handle on a class value raises; a copy would be another object
                {"Dog d; d.own();", "'this' becomes a handle only on an object reached through a shared_ptr, and this one is a class value"},
                {"array<shared_ptr<Ent>> REG; class Ent { Ent() { REG.push(this); } } Ent e;", "'this' becomes a handle only on an object reached through a shared_ptr, and this one is a class value"},
                {"class Ent { shared_ptr<Ent> self = null; Ent() { self = this; } } Ent e;", "'this' becomes a handle only on an object reached through a shared_ptr, and this one is a class value"},
                // the slot's declared type decides, wherever the program declares it (here below the class that stores into it)
                {"class Late { Late() { LATE.push(this); } } array<shared_ptr<Late>> LATE; Late l;", "'this' becomes a handle only on an object reached through a shared_ptr, and this one is a class value"},
                {"class LateMap { LateMap() { LATE_MAP[\"k\"] = this; } } map<string, shared_ptr<LateMap>> LATE_MAP; LateMap m;", "'this' becomes a handle only on an object reached through a shared_ptr, and this one is a class value"},
                {"class LateSlot { LateSlot() { LATE_SLOT[0] = this; } } array<shared_ptr<LateSlot>> LATE_SLOT = [null]; LateSlot s;", "'this' becomes a handle only on an object reached through a shared_ptr, and this one is a class value"},
                {"class LateInsert { LateInsert() { LATE_INSERT.insert(0, this); } } array<shared_ptr<LateInsert>> LATE_INSERT; LateInsert s;", "'this' becomes a handle only on an object reached through a shared_ptr, and this one is a class value"},
            };
            for (const auto& row : refused) { check_true(refusal(row.first).find(row.second) != std::string::npos, row.first); }
        });

        test("a_passed_test_narrows_the_variable_until_something_can_change_it", [this]() {
            const std::string classes = R"(
                class Unit { int hp = 1; string tag() { return "unit"; } }
                class Hero : Unit { int mana = 5; string tag() override { return "hero"; } }
                class Paladin : Hero { int faith = 7; }
                class Rogue : Unit { int stealth = 2; }
                string who(Hero h) { return h.tag() + to_string(h.mana); }
                void touch() { }
            )";
            const std::string narrowed = classes + R"(
                int direct(Unit u) { if (reflect::is_instance_of(u, "Hero")) { Hero h = u; return u.mana + h.mana; } return 0; }
                int handle(shared_ptr<Unit> u) { if (!dynamic_cast<Paladin>(u)) { return -1; } return u.faith; }
                int chained(var v) { return dynamic_cast<shared_ptr<Hero>>(v) && v.mana > 1 ? v.mana : 0; }
                int either(Unit u) { if (!dynamic_cast<Hero>(u) || u.mana < 0) { return 0; } return u.mana; }
                int otherwise(Unit u) { if (!(reflect::is_instance_of(u, "Hero"))) { return 0; } else { return u.mana; } }
                int looped(array<shared_ptr<Unit>> xs) { int t = 0; for (auto x : xs) { if (!dynamic_cast<Hero>(x)) { continue; } t += x.mana; } return t; }
                int walked(shared_ptr<Unit> u) { int n = 0; while (dynamic_cast<shared_ptr<Paladin>>(u)) { n += u.faith; u = new Unit(); } return n; }
                int stepped(shared_ptr<Unit> u) { int n = 0; for (; reflect::is_instance_of(u, "Hero"); u = new Unit()) { n += u.mana; } return n; }
                int kinds(var x) { if (reflect::type_of(x) == "int") { return x + 1; } if (reflect::type_of(x) != "string") { return 0; } return x.size(); }
                int typed(var v) { try { throw v; } catch (Hero e) { return e.mana; } return 0; }
                int caught(var v) { try { return typed(v); } catch (err) { return 0; } }
                int reflected(Unit u) { if (reflect::is_instance_of(u, "Paladin")) { return u.faith; } return 0; }
                int referred(Unit& u) { if (reflect::is_instance_of(u, "Hero")) { return u.mana; } return 0; }
                int snapshot(shared_ptr<Unit> u) { if (reflect::is_instance_of(u, "Hero")) { auto f = [u]() { return u.mana; }; u = new Unit(); return f(); } return 0; }
                string passed(Unit u) { if (reflect::is_instance_of(u, "Hero")) { return who(u); } return "-"; }
                int both(var v) { if (reflect::is_instance_of(v, "Hero") && reflect::is_instance_of(v, "Rogue")) { return v.mana + v.stealth; } return 3; }
                int copied(shared_ptr<Unit> u) { if (dynamic_cast<Hero>(u)) { auto c = u; return c.mana; } return 0; }
                int declared(shared_ptr<Unit> u) { if (auto h = dynamic_cast<Hero>(u)) { return h.mana; } return 0; }   // the declared name has the cast's type
                void keep(shared_ptr<Unit>& r, Unit& v) { }
                int bound(shared_ptr<Unit> u, Unit w) { if (reflect::is_instance_of(u, "Hero") && reflect::is_instance_of(w, "Hero")) { int m = u.mana + w.mana; keep(u, w); shared_ptr<Unit>& a = u; return m; } return 0; }   // a reference binds the variable as declared
                int compared(var x) { if (reflect::type_of(x) == "int") { return x == "5" || x == null ? 1 : 0; } return 2; }   // a var keeps its run-time comparisons
                Paladin p;
                `${direct(Hero())} ${direct(Unit())} ${handle(new Paladin())} ${handle(new Hero())} ${chained(Hero())} ${chained(5)} ${either(Hero())} ${either(Rogue())} ` +
                `${otherwise(Paladin())} ${looped([new Hero(), new Unit(), new Paladin()])} ${walked(new Paladin())} ${stepped(new Hero())} ${kinds(41)} ${kinds("four")} ${kinds(2.5)} ` +
                `${caught(Paladin())} ${caught(3)} ${reflected(Paladin())} ${referred(p)} ${snapshot(new Hero())} ${passed(Paladin())} ${passed(Unit())} ${both(Hero())} ${copied(new Paladin())} ${compared(5)} ${bound(new Hero(), Paladin())} ${declared(new Paladin())}`;
            )";
            const std::string expected = "10 0 7 -1 5 0 5 0 5 10 7 5 42 4 0 5 0 7 5 5 hero5 - 3 5 0 10 5";
            auto e = make_engine();
            jai::stdlib::register_all(*e);
            auto rep = e->check(narrowed);
            check_eq((size_t)0, rep.diagnostics.size(), rep.format(narrowed));
            check_eq(expected, e->execute(narrowed).as_string());
            auto unproven = make_engine();
            jai::stdlib::register_all(*unproven);
            unproven->type_proofs(false);
            check_eq(expected, unproven->execute(narrowed).as_string());
            // every way the variable may change ends the narrowing: the member is the declared class's again, a load error
            const std::vector<std::string> ended = {
                "int f(Unit u) { if (reflect::is_instance_of(u, \"Hero\")) { u = Unit(); return u.mana; } return 0; }",
                "void poke(shared_ptr<Unit>& r) { r = new Unit(); } int f(shared_ptr<Unit> u) { if (reflect::is_instance_of(u, \"Hero\")) { poke(u); return u.mana; } return 0; }",
                "int f(shared_ptr<Unit> u) { if (reflect::is_instance_of(u, \"Hero\")) { auto g = [&]() { u = new Unit(); }; g(); return u.mana; } return 0; }",
                "int f(shared_ptr<Unit> u) { auto g = [&]() { u = new Unit(); }; if (reflect::is_instance_of(u, \"Hero\")) { g(); return u.mana; } return 0; }",
                "int f(shared_ptr<Unit> u) { if (reflect::is_instance_of(u, \"Hero\")) { auto g = [&]() { return u.mana; }; return g(); } return 0; }",
                "int f(shared_ptr<Unit> u) { if (reflect::is_instance_of(u, \"Hero\")) { auto g = [&]() { auto h = [=]() { return u.mana; }; return h(); }; return g(); } return 0; }",
                "int f(shared_ptr<Unit> u) { int t = 0; if (reflect::is_instance_of(u, \"Hero\")) { for (int i = 0; i < 2; i++) { t += u.mana; u = new Unit(); } } return t; }",
                "int f(shared_ptr<Unit> u) { int t = 0; while (t < 2) { if (reflect::is_instance_of(u, \"Hero\")) { t += 1; } t += u.mana; } return t; }",
                "int f(shared_ptr<Unit> u) { if (reflect::is_instance_of(u, \"Hero\")) { shared_ptr<Unit>& a = u; a = new Unit(); return u.mana; } return 0; }",
                "int f(shared_ptr<Unit> u) { if (reflect::is_instance_of(u, \"Hero\")) { u.reset(); return u.mana; } return 0; }",
                "int f(Unit& u) { if (reflect::is_instance_of(u, \"Hero\")) { touch(); return u.mana; } return 0; }",
                "int f(Unit u) { if (reflect::is_instance_of(u, \"Hero\")) { } return u.mana; }",
                "int f(Unit u) { return reflect::is_instance_of(u, \"Hero\") ? 0 : u.mana; }",
                "int f(Unit u, bool ok) { if (reflect::is_instance_of(u, \"Hero\") || ok) { return u.mana; } return 0; }",
                "int f(Unit u) { if (!(reflect::is_instance_of(u, \"Hero\"))) { return u.mana; } return 0; }",
                "class Holder { Unit slot = Hero(); int f() { if (reflect::is_instance_of(slot, \"Hero\")) { return slot.mana; } return 0; } }",
                "shared_ptr<Unit> G = new Hero(); int f() { if (reflect::is_instance_of(G, \"Hero\")) { return G.mana; } return 0; }",
                "int f(array<Unit> xs) { if (reflect::is_instance_of(xs[0], \"Hero\")) { return xs[0].mana; } return 0; }",
            };
            for (const auto& body : ended) {
                auto fresh = make_engine();
                jai::stdlib::register_all(*fresh);
                const std::string program = classes + body;
                auto found = fresh->check(program);
                check_true(found.has_errors() && found.diagnostics.size() == 1 && found.diagnostics[0].message.find("no member '") == 0, found.format(program));
            }
            // under [[warn_mode]] the same programs run: the run time's own member read decides, on both backends alike
            auto warned = make_engine();
            jai::stdlib::register_all(*warned);
            check_eq(std::string("5|Unit has no member 'mana'|5"), warned->execute("[[warn_mode]];\n" + classes + R"(
                string t(function f) { string r = ""; try { r = `${f()}`; } catch (err) { r = err; } return r; }
                int f(shared_ptr<Unit> u) { auto g = [&]() { u = new Unit(); }; if (reflect::is_instance_of(u, "Hero")) { int before = u.mana; g(); return u.mana; } return 0; }
                int k(var v) { if (reflect::is_instance_of(v, "Hero")) { return v.mana; } return 0; }
                t([]() { return k(Hero()); }) + "|" + t([]() { return f(new Hero()); }) + "|" + t([]() { return k(Paladin()); });
            )").as_string());
        });

        test("diamond_inheritance_should_fail", [this]() {
            auto js_engine = make_engine();

            const char* script = R"(
                class Base {
                    int x = 1;
                }

                class A : Base {
                }

                class B : Base {
                }

                class Diamond : A, B {
                }

                auto d = Diamond();
                d.x
            )";

            // This should fail at class definition time due to diamond detection
            bool exception_thrown = false;
            try {
                auto result = js_engine->execute(script);
                // If we get here, diamond was not rejected - test should fail
            } catch (const std::exception&) {
                // Expected to throw/error
                exception_thrown = true;
            }

            check(exception_thrown);  // Verify diamond inheritance was rejected
        });

        // `new T(args)` is pure sugar for shared_ptr<T>(args) construction:
        // same new_expr, same tagging, reference semantics opt-in. A 'var'
        // decl keeps the shared_ptr marker (var p = new P() IS shared_ptr<P> p = make_shared<P>()).
        test("new_keyword_shared_ptr_sugar_basics", [this]() {
            const char* src = R"(
                class P { int x = 3; P() { x = 3; } P(int v) { x = v; } }
                var p = new P(7);
                var q = p;
                q.x = 99;
                shared_ptr<P> typed = new P(5);
                auto viaExpr = [](:o) -> { return o.x; };
                to_string(p.x) + "|" + to_string(q.x) + "|" + to_string(typed.x)
                    + "|" + to_string(viaExpr(new P(41)));
            )";
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(e);
                check_eq(std::string("99|99|5|41"), e->execute(src).as<std::string>(),
                         use_vm ? "vm new sugar" : "interp new sugar");
            }
        });

        test("new_keyword_matches_shared_ptr_spelling", [this]() {
            // new P() and shared_ptr<P>(...) are the same expression: same reflect::type_of,
            // same sharing behavior, brace form included
            const char* src = R"(
                class P { int x = 1; P() { x = 1; } }
                var a = new P();
                var b = shared_ptr<P>();
                var braced = new P{};
                var aliasA = a;
                aliasA.x = 10;
                var aliasB = b;
                aliasB.x = 20;
                (reflect::type_of(a) == reflect::type_of(b) ? "same" : "diff") + "|" + to_string(a.x)
                    + "|" + to_string(b.x) + "|" + to_string(braced.x);
            )";
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(e);
                check_eq(std::string("same|10|20|1"), e->execute(src).as<std::string>(),
                         use_vm ? "vm spelling parity" : "interp spelling parity");
            }
        });

        test("new_keyword_inheritance_upcast", [this]() {
            // new Derived into shared_ptr<Base> follows today's decl upcast rules
            const char* src = R"(
                class Base { int v = 1; Base() { v = 1; } }
                class Derived : Base { Derived() : super() { v = 2; } }
                shared_ptr<Base> b = new Derived();
                var alias = b;
                alias.v = 42;
                b.v;
            )";
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                check_eq((int64_t)42, e->execute(src).as_int(),
                         use_vm ? "vm upcast" : "interp upcast");
            }
        });

        test("new_keyword_registered_cpp_class", [this]() {
            // new works for registered C++ classes exactly like shared_ptr<T>(...)
            struct counter_probe { int n = 0; };
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                dynamic_binder<counter_probe>(*e, "CounterProbe")
                    .constructor<>()
                    .property("n", &counter_probe::n)
                    .build();
                const char* src = R"(
                    var c = new CounterProbe();
                    var alias = c;
                    alias.n = 13;
                    c.n;
                )";
                check_eq((int64_t)13, e->execute(src).as_int(),
                         use_vm ? "vm cpp class new" : "interp cpp class new");
            }
        });

        test("new_keyword_statement_and_error_shapes", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                // statement position: constructs and discards without error
                const char* stmt_src = R"(
                    class P { int x = 0; P() { x = 0; } }
                    new P();
                    7;
                )";
                check_eq((int64_t)7, e->execute(stmt_src).as_int(),
                         use_vm ? "vm stmt new" : "interp stmt new");
                // `new P;` without an argument list is a parse error (rejected either
                // as a throw or a null result depending on recovery)
                auto e2 = jai::foundry::make_default_engine();
                e2->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                bool rejected = false;
                try {
                    auto r = e2->execute("class P { int x = 0; } var p = new P;");
                    rejected = r.is_null();
                } catch (const std::exception&) {
                    rejected = true;
                }
                check_true(rejected, use_vm ? "vm new-without-parens rejected"
                                            : "interp new-without-parens rejected");
            }
        });

        // === Member access enforcement (private:/protected:/public: labels) ===
        // Two levels: private = the declaring class and every class derived
        // from it; protected is its synonym; lambdas defined in a method inherit its class's
        // access; free functions/top level are public-only. Host C++ APIs stay unrestricted.
        test("member_access_enforcement_matrix", [this]() {
            const char* src = R"(
                class C {
                private:
                    int secret = 7;
                    int hidden_helper() { return secret * 2; }
                protected:
                    int prot = 8;
                public:
                    int pub = 9;
                    int peek() { return this.secret + this.prot + hidden_helper(); }
                    function lam() { var f = [this]() { return this.secret; }; return f(); }
                }
                class D : C {
                    int useProt() { return this.prot + 1; }
                }
                function freeProbe(o) { return o.secret; }
                class Other { int poke(o) { return o.secret; } }
                var c = C();
                var d = D();
                var out = `${c.peek()}|${c.pub}|${c.lam()}|${d.useProt()}`;
                try { var a = c.secret; out = out + "|LEAK"; } catch (e) { out = out + "|" + e; }
                try { var b = c.prot; out = out + "|LEAK"; } catch (e) { out = out + "|" + e; }
                try { var h = c.hidden_helper(); out = out + "|LEAK"; } catch (e) { out = out + "|" + e; }
                try { var f = freeProbe(c); out = out + "|LEAK"; } catch (e) { out = out + "|free:" + e; }
                try { var o = Other().poke(c); out = out + "|LEAK"; } catch (e) { out = out + "|other:" + e; }
                try { c.secret = 1; out = out + "|WROTE"; } catch (e) { out = out + "|w:" + e; }
                try { c.prot += 1; out = out + "|COMPOUND"; } catch (e) { out = out + "|cw:" + e; }
                out;
            )";
            const std::string expected =
                "29|9|7|9"
                "|Cannot access private member 'secret' of class 'C'"
                "|Cannot access private member 'prot' of class 'C'"
                "|Cannot access private member 'hidden_helper' of class 'C'"
                "|free:Cannot access private member 'secret' of class 'C'"
                "|other:Cannot access private member 'secret' of class 'C'"
                "|w:Cannot access private member 'secret' of class 'C'"
                "|cw:Cannot access private member 'prot' of class 'C'";
            std::string outputs[2];
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(e);
                outputs[use_vm ? 1 : 0] = e->execute(src).as<std::string>();
            }
            check_eq(expected, outputs[0], "interp access matrix");
            check_eq(outputs[0], outputs[1], "access matrix parity (byte-identical incl. error text)");
        });

        test("member_access_private_reaches_derived_classes", [this]() {
            // Derived methods reach Base-private and Base-protected alike, bare, through this.
            // and through super:: (two levels)
            const char* src = R"(
                class Base {
                private:
                    int core = 1;
                    int corem() { return 11; }
                protected:
                    int sharedv = 2;
                    int sharedm() { return 22; }
                }
                class Derived : Base {
                    int okProt() { return this.sharedv + super::sharedm(); }
                    int badPriv() { return this.core; }
                    int badPrivM() { return super::corem(); }
                }
                var d = Derived();
                var out = `${d.okProt()}`;
                try { var a = d.badPriv(); out = `${out}|${a}`; } catch (e) { out = `${out}|${e}`; }
                try { var b = d.badPrivM(); out = `${out}|${b}`; } catch (e) { out = `${out}|${e}`; }
                out;
            )";
            const std::string expected = "24|1|11";
            std::string outputs[2];
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                outputs[use_vm ? 1 : 0] = e->execute(src).as<std::string>();
            }
            check_eq(expected, outputs[0], "interp base/derived access");
            check_eq(outputs[0], outputs[1], "base/derived access parity");
        });

        test("member_access_statics_and_ref_binding", [this]() {
            const char* src = R"(
                class S {
                private:
                    static int counter = 5;
                    int f = 3;
                public:
                    static function bump() { counter += 1; return counter; }
                }
                function wantsRef(int& r) { r = 99; }
                var out = `${S::bump()}`;
                try { var a = S::counter; out = out + "|LEAK"; } catch (e) { out = out + "|" + e; }
                try { S::counter = 0; out = out + "|WROTE"; } catch (e) { out = out + "|w:" + e; }
                var s = S();
                try { wantsRef(s.f); out = out + "|BOUND"; } catch (e) { out = out + "|ref:" + e; }
                out;
            )";
            const std::string expected =
                "6"
                "|Cannot access private member 'counter' of class 'S'"
                "|w:Cannot access private member 'counter' of class 'S'"
                "|ref:Cannot access private member 'f' of class 'S'";
            std::string outputs[2];
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                outputs[use_vm ? 1 : 0] = e->execute(std::string("[[warn_mode]]; ") + src).as<std::string>();   // the runtime raise (strict rejects S::counter at load)
            }
            check_eq(expected, outputs[0], "interp statics/ref access");
            check_eq(outputs[0], outputs[1], "statics/ref access parity");
        });

        test("member_access_private_field_by_reference_beside_lambda", [this]() {
            // A T& argument binds in the calling method's class, whatever else the method holds (a lambda gives it a frame env)
            const char* src = R"(
                class Cave {
                    Cave() {}
                    Cave(int& a_seed) { a_seed += 100; }
                    void dig(int& a_dice) { a_dice += 1; }
                    static void digStatic(int& a_dice) { a_dice += 10; }
                }
                class Game {
                    int arrow() { cave.dig(dice); var f = x => x + 1; return f(dice); }
                    int bracket() { openCave.dig(dice); var f = []() { return 1; }; return dice + f(); }
                    int viaThis() { cave.dig(this.dice); var f = x => x; return f(dice); }
                    int staticCallee() { Cave::digStatic(dice); var f = x => x; return f(dice); }
                    int constructed() { Cave made(dice); var f = x => x; return f(dice); }
                    int inLambda() { var f = [this]() { cave.dig(dice); return dice; }; return f(); }
                    int noLambda() { cave.dig(dice); return dice; }
                    Cave openCave;
                private:
                    Cave cave;
                    int dice = 0;
                }
                Game game;
                string out = "";
                try { out += `${game.arrow()}`; } catch (e) { out += e; }
                try { out += `|${game.bracket()}`; } catch (e) { out += "|" + e; }
                try { out += `|${game.viaThis()}`; } catch (e) { out += "|" + e; }
                try { out += `|${game.staticCallee()}`; } catch (e) { out += "|" + e; }
                try { out += `|${game.constructed()}`; } catch (e) { out += "|" + e; }
                try { out += `|${game.inLambda()}`; } catch (e) { out += "|" + e; }
                try { out += `|${game.noLambda()}`; } catch (e) { out += "|" + e; }
                out;
            )";
            std::string outputs[2];
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                outputs[use_vm ? 1 : 0] = e->execute(src).as<std::string>();
            }
            check_eq(std::string("2|3|3|13|113|114|115"), outputs[0], "interp private field by reference beside a lambda");
            check_eq(outputs[0], outputs[1], "private field by reference beside a lambda parity");
        });

        // ------------------------------------------------- bare-name resolution order
        // Audit pins (docs/site/guide/07-classes.html "Name resolution in
        // methods"): catch var > locals/params > enclosing scopes > GLOBALS > fields >
        // methods > statics, identical on both backends for reads AND writes. The
        // surprising row - globals shadow fields - is DELIBERATELY pinned; this.name
        // always reaches the field.

        test("name_resolution_locals_and_params_shadow_fields", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = make_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                auto r = e->execute(R"(
                    class C {
                        string hp = "field";
                        function local_wins() -> string { var hp = "local"; return hp; }
                        function param_wins(var hp) -> string { return hp; }
                        function catch_wins() -> string { try { throw "caught"; } catch (hp) { return hp; } }
                        function field_when_alone() -> string { return hp; }
                    }
                    auto c = C();
                    c.local_wins() + "|" + c.param_wins("param") + "|" + c.catch_wins() + "|" + c.field_when_alone()
                )");
                check_eq(std::string("local|param|caught|field"), r.as<std::string>());
            }
        });

        test("name_resolution_fields_shadow_globals", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = make_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                // C++ unqualified lookup: inside a method, the FIELD shadows a
                // same-named global for reads AND writes; the global is untouched and
                // stays reachable at top level; a global defined AFTER the class never
                // steals bare member reads
                auto r = e->execute(R"(
                    var g = "global";
                    class C {
                        string g = "field";
                        function read_bare() -> string { return g; }
                        function write_bare() -> void { g = "written"; }
                        function read_this() -> string { return this.g; }
                    }
                    var c = C();
                    c.write_bare();
                    var before_late = "";
                    class Late { string v = "field"; function probe() -> string { return v; } }
                    var lc = Late();
                    before_late = lc.probe();
                    var v = "late-global";
                    c.read_bare() + "|" + c.read_this() + "|" + g + "|" + before_late + "|" + lc.probe()
                )");
                check_eq(std::string("written|written|global|field|field"), r.as<std::string>());
            }
        });

        // ------------------------------------- shared_ptr<T> type-intern identity
        // GLOOM bug: engine::get_type_info's composite key had no case for
        // jai_shared_ptr_type, so every parsed shared_ptr<T> collapsed onto the FIRST
        // interned T engine-wide - `new Bar()` after `new Foo()` SILENTLY CONSTRUCTED
        // a Foo (both backends; new desugars to the same shared_ptr<T>(args) node).
        // The key now includes the pointee. Matrix pinned below; the sibling caches
        // were audited clean: class_definition::overload_resolution_cache_ is a
        // per-class member (class identity inherent) and the call-site
        // callee IC pins script_function identity per call_site.

        test("shared_ptr_construction_distinguishes_classes", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class Foo { int x = 1; }
                    class Bar { int y = 2; }
                    var a = new Foo();
                    var b = new Bar();
                    auto sf = shared_ptr<Foo>();
                    auto sb = shared_ptr<Bar>();
                    auto vf = Foo();
                    auto vb = Bar();
                )");
                check_eq((int64_t)1, e->execute("a.x").as_int());
                check_eq((int64_t)2, e->execute("b.y").as_int());   // was: b built as a Foo
                check_eq((int64_t)1, e->execute("sf.x").as_int());
                check_eq((int64_t)2, e->execute("sb.y").as_int());
                check_eq((int64_t)1, e->execute("vf.x").as_int());
                check_eq((int64_t)2, e->execute("vb.y").as_int());
            }
        });

        test("shared_ptr_construction_same_arity_args_and_interleaved", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class Foo { int x = 0; Foo(int v) { x = v; } }
                    class Bar { int y = 0; Bar(int v) { y = v; } }
                    var b1 = new Bar(20);
                    var a1 = new Foo(10);
                    var b2 = new Bar{30};
                    auto a2 = shared_ptr<Foo>(40);
                )");
                check_eq((int64_t)20, e->execute("b1.y").as_int());
                check_eq((int64_t)10, e->execute("a1.x").as_int());  // was: a1 built as a Bar
                check_eq((int64_t)30, e->execute("b2.y").as_int());
                check_eq((int64_t)40, e->execute("a2.x").as_int());
            }
        });

        test("shared_ptr_typed_decl_enforces_right_pointee", [this]() {
            // The stale-pointee variant: shared_ptr<Bar> declarations must enforce
            // against BAR even when shared_ptr<Foo> was interned first.
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class Foo { int x = 1; }
                    class Bar { int y = 2; }
                    shared_ptr<Foo> a = make_shared<Foo>();
                    shared_ptr<Bar> b = make_shared<Bar>();
                )");
                check_eq((int64_t)1, e->execute("a.x").as_int());
                check_eq((int64_t)2, e->execute("b.y").as_int());
                // Typed shared_ptr decls ENFORCE the pointee
                // class - wrong-class value init and wrong-class sp-aliasing both
                // error; upcasts (script chains) stay legal; null stays assignable.
                check_throws([&]() { e->execute("shared_ptr<Bar> wrong = make_shared<Foo>();"); });
                check_throws([&]() { e->execute("shared_ptr<Bar> wrong2 = a;"); });   // aliasing a shared_ptr<Foo>
                e->execute(R"(
                    class Base { int bv = 5; }
                    class Derived : Base { int dv = 6; }
                    shared_ptr<Base> up1 = make_shared<Derived>();
                    var d = new Derived();
                    shared_ptr<Base> up2 = d;
                    shared_ptr<Base> nul = null;
                )");
                check_eq((int64_t)5, e->execute("up1.bv").as_int());
                check_eq((int64_t)5, e->execute("up2.bv").as_int());
                check_eq(true, e->execute("nul == null").as<bool>());
            }
        });

        test("shared_ptr_auto_decl_infers_then_enforces", [this]() {
            // shared_ptr<auto>: the pointee is INFERRED from the
            // initializer's exact class, then enforced exactly like the explicit
            // spelling - a temporary as the handle's object, handle sharing, upcast-on-reassign,
            // wrong-class errors, and null-after-establishment all mirror shared_ptr<T>.
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class Foo { int x = 1; }
                    class Bar { int y = 2; }
                    shared_ptr<auto> a = new Foo();      // infer from new
                    shared_ptr<auto> b = make_shared<Foo>();          // the temporary is the object (mirrors shared_ptr<Foo> b = make_shared<Foo>())
                    shared_ptr<auto> c = a;              // infer from a shared_ptr variable
                    var arr = [new Foo()];
                    shared_ptr<auto> d = arr[0];         // infer from an element read
                )");
                e->execute("c.x = 7;");
                check_eq((int64_t)7, e->execute("a.x").as_int());      // c shares a's handle
                e->execute("auto b2 = b; b2.x = 42;");
                check_eq((int64_t)42, e->execute("b.x").as_int());     // sp copies share
                e->execute("d.x = 9;");
                check_eq((int64_t)9, e->execute("arr[0].x").as_int()); // d shares the element
                // enforce-after: the inferred tag drives the explicit spelling's
                // reassignment checks and its init enforcement when aliased onward
                check_throws([&]() { e->execute("a = Bar();"); });
                check_throws([&]() { e->execute("a = new Bar();"); });
                check_throws([&]() { e->execute("shared_ptr<Bar> wrong = a;"); });
                e->execute("shared_ptr<Foo> fine = a;");               // tag says Foo
                e->execute("a = null;");                               // null legal AFTER establishment
                check_eq(true, e->execute("a == null").as<bool>());
                // upcast-on-reassign mirrors the explicit spelling; downcast errors
                e->execute(R"(
                    class Base { int bv = 5; }
                    class Derived : Base { int dv = 6; }
                    shared_ptr<auto> up = make_shared<Base>();
                    up = new Derived();
                    shared_ptr<auto> down = make_shared<Derived>();
                )");
                check_eq((int64_t)5, e->execute("up.bv").as_int());
                check_throws([&]() { e->execute("down = Base();"); });
                // nothing to infer: missing/null/non-class initializers error
                check_throws([&]() { e->execute("shared_ptr<auto> n0;"); });
                check_throws([&]() { e->execute("shared_ptr<auto> n1 = null;"); });
                check_throws([&]() { e->execute("shared_ptr<auto> n2 = 5;"); });
            }
            // nothing-to-infer error text: byte-identical across backends, and points
            // at the escape hatches (var, or an explicit shared_ptr<T>)
            auto run_catch = [](bool use_vm, const char* src) -> std::string {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                try { e->execute(src); } catch (const std::exception& ex) { return ex.what(); }
                return "<no throw>";
            };
            for (const char* src : { "shared_ptr<auto> n = null;", "class F {} shared_ptr<auto> n;" }) {
                std::string interp_msg = run_catch(false, src);
                std::string vm_msg = run_catch(true, src);
                check_eq(interp_msg, vm_msg, std::string("parity: ") + src);
                check(interp_msg.find("use var, or an explicit shared_ptr<T>") != std::string::npos, interp_msg);
            }
        });

        test("shared_ptr_var_is_a_parse_error", [this]() {
            // shared_ptr<var> is REJECTED at parse -
            // plain var already holds and rebinds any shared_ptr across unrelated
            // classes, so a constrained-but-dynamic pointee has no use case, and var
            // meaning something different inside angle brackets would muddy the ladder.
            // The error teaches the two real spellings.
            for (bool use_vm : {false, true}) {
                for (const char* src : { "class Foo { int x = 1; } shared_ptr<var> p = make_shared<Foo>();",
                                         "shared_ptr<var> n = null;",
                                         "function f(shared_ptr<var> a) { return a; }" }) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    bool rejected = false;
                    std::string msg;
                    try {
                        auto r = e->execute(src);
                        rejected = r.is_null();   // recoverable parse errors synchronize to null
                    } catch (const std::exception& ex) {
                        rejected = true;
                        msg = ex.what();
                    }
                    check_true(rejected, std::string(use_vm ? "vm: " : "interp: ") + src);
                    if (!msg.empty()) {
                        check_true(msg.find("shared_ptr<var> is not supported") != std::string::npos, msg);
                        check_true(msg.find("shared_ptr<auto>") != std::string::npos, msg);
                    }
                    // the engine stays usable after the rejection
                    check_eq((int64_t)7, e->execute("3 + 4").as_int());
                }
                // and plain var really does hold + rebind any class's handle (the why)
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class Foo { int x = 1; }
                    class Bar { int y = 2; }
                    var p = new Foo();
                    var alias = p;
                )");
                e->execute("alias.x = 3;");
                check_eq((int64_t)3, e->execute("p.x").as_int());   // var copies share the handle
                e->execute("p = new Bar();");                        // and rebind across classes
                check_eq((int64_t)2, e->execute("p.y").as_int());
                check_eq((int64_t)3, e->execute("alias.x").as_int()); // rebind, not a copy: alias untouched
            }
        });

        test("var_held_handles_rebind_unchecked", [this]() {
            // A var-DECLARED holder behaves
            // IDENTICALLY to a typed handle holder wherever typed is legal (the monotonic
            // ladder). A handle rhs REBINDS; a COMPATIBLE value rhs assigns INTO the
            // pointee (just like shared_ptr<T>/auto); var's ONLY extra power is REBINDING
            // an INCOMPATIBLE value or a primitive where typed would refuse. Typed
            // spellings (shared_ptr<T>, shared_ptr<auto>, plain auto copies) keep enforcing.
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class Foo { int x = 1; }
                    class Bar { int y = 2; }
                )");
                // decl-marker acquisition: var p = new Foo()
                e->execute("var p = new Foo(); var keep = p;");
                e->execute("p = new Bar();");
                check_eq((int64_t)2, e->execute("p.y").as_int());
                check_eq((int64_t)1, e->execute("keep.x").as_int());   // alias untouched by rebind
                // assign-marker acquisition: var q; q = new Foo()
                e->execute("var q; q = new Foo(); q = new Bar();");
                check_eq((int64_t)2, e->execute("q.y").as_int());
                // var-to-var rebind shares the handle
                e->execute("var r = new Foo(); var s = new Bar(); r = s; r.y = 9;");
                check_eq((int64_t)9, e->execute("s.y").as_int());
                // rebindability survives copies into var decls and repeated rebinds
                e->execute("var t = p; t = new Foo(); t = new Bar();");
                check_eq((int64_t)2, e->execute("t.y").as_int());
                // weak_ptr from a var-held handle still validates its pointee
                e->execute("var wsrc = new Foo(); weak_ptr<Foo> w = wsrc;");
                check_eq(false, e->execute("w.expired()").as<bool>());
                check_throws([&]() { e->execute("weak_ptr<Bar> wbad = wsrc;"); });
                // MONOTONIC LADDER: a var-held
                // handle behaves IDENTICALLY to a typed holder wherever typed is legal.
                // A COMPATIBLE value rhs (same class or subclass of the held pointee)
                // assigns INTO the shared pointee, so aliases SEE the change - exactly
                // like shared_ptr<Foo>/auto below. var's ONLY extra power is rebinding an
                // INCOMPATIBLE value or a primitive.
                e->execute("var u = new Foo(); var ualias = u; auto src = Foo(); src.x = 7; u = src;");
                check_eq((int64_t)7, e->execute("ualias.x").as_int());   // alias SEES the change (assign-into)
                check_eq((int64_t)7, e->execute("u.x").as_int());
                // it is the SAME shared object: mutating u touches the alias too
                e->execute("u.x = 42;");
                check_eq((int64_t)42, e->execute("ualias.x").as_int());
                // cross-class (INCOMPATIBLE) value rhs REBINDS - var NEVER refuses (no throw),
                // and the old alias is untouched (the handle was re-pointed, not written)
                e->execute("u = Bar();");
                check_eq((int64_t)2, e->execute("u.y").as_int());
                check_eq((int64_t)42, e->execute("ualias.x").as_int());  // old object untouched by the rebind
                // primitive rhs rebinds too
                e->execute("var pv = new Foo(); pv = 5;");
                check_eq((int64_t)5, e->execute("pv").as_int());
                // ...and the var stays dynamic after a value rebind - it accepts a handle again
                e->execute("pv = new Bar();");
                check_eq((int64_t)2, e->execute("pv.y").as_int());
                // TYPED tier: a class value never becomes a handle, as in C++ (make_shared<Foo>(value) is the copy; Foo& through
                // the handle assigns into the shared object)
                check_throws([&]() { e->execute("shared_ptr<Foo> tu = new Foo(); auto tsrc = Foo(); tu = tsrc;"); });
                e->execute("shared_ptr<Foo> tu2 = new Foo(); shared_ptr<Foo> tualias = tu2; auto tsrc2 = Foo(); tsrc2.x = 7; Foo& into = tu2; into = tsrc2;");
                check_eq((int64_t)7, e->execute("tualias.x").as_int());   // the reference reaches THROUGH the handle
                check_throws([&]() { e->execute("shared_ptr<Foo> tf2 = new Foo(); tf2 = Bar();"); });
                // typed handle-rhs still enforces; auto copies of a var handle
                // re-lock to the plain spelling (infer-then-enforce)
                check_throws([&]() { e->execute("shared_ptr<Foo> tf = new Foo(); tf = new Bar();"); });
                check_throws([&]() { e->execute("auto af = p; af = new Foo();"); });   // p holds Bar; af locked to Bar
                e->execute("auto ab = p; ab = new Bar();");                            // same class stays legal
                check_eq((int64_t)2, e->execute("ab.y").as_int());
            }
        });

        test("user_method_wins_over_builtin_handle_method", [this]() {
            // A user class method takes PRIORITY over a same-named
            // builtin shared_ptr handle method (reset/use_count/unique/cpp_ref_count)
            // (otherwise gloom's fx.reset() would silently no-op the builtin reset()): the
            // class method wins on shared_ptr<T> instances and the builtin stays reachable where NO class method shadows it
            // (the checker's warning: Static Type Checking's a_handle_builtin_shadow_warns_where_a_handle_holds_the_class).
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);

                // class reset() defined -> user method WINS (builtin would have no-op'd)
                e->execute("class Pool { int n = 0; void reset() { n = 99; } }");
                e->execute("var p = new Pool(); p.reset();");                 // identifier receiver (fast path)
                check_eq((int64_t)99, e->execute("p.n").as_int(), use_vm ? "vm fast-path" : "interp fast-path");
                // generic (non-identifier receiver) path also resolves the user method
                e->execute("var arr = [new Pool()]; arr[0].reset();");
                check_eq((int64_t)99, e->execute("arr[0].n").as_int());
                // a class-defined use_count() wins over the builtin ref-count reader
                e->execute("class Counter { int use_count() { return 7; } }");
                check_eq((int64_t)7, e->execute("var c = new Counter(); c.use_count()").as_int());

                // NO collision -> builtin stays reachable
                e->execute("class Bare { int x = 5; }");
                check_ge(e->execute("shared_ptr<Bare> b = new Bare(); b.use_count()").as_int(), (int64_t)1,
                         "builtin use_count still reachable when unshadowed");
            }
        });

        // C++ `p.reset()`: the handle's reset() empties the storage its receiver names (a variable, a field, an element, a map
        // entry, a weak_ptr) and leaves the object to its other owners; a call result is a temporary it leaves alone
        test("handle_reset_empties_the_receiver", [this]() {
            const std::string prelude =
                "class C { int v = 1; } class H { shared_ptr<C> p; } "
                "var keep = new C(); function make() -> shared_ptr<C> { return keep; } ";
            const std::pair<const char*, const char*> cases[] = {
                {"var c = new C(); var d = c; c.reset(); to_string(c == null) + \" \" + to_string(d.v) + \" \" + to_string(d == null)", "true 1 false"},
                {"shared_ptr<C> e = new C(); e.reset(); bool was = e == null; e = new C(); to_string(was) + \" \" + to_string(e.v)", "true 1"},
                {"var h = H(); h.p = new C(); h.p.reset(); to_string(h.p == null)", "true"},
                {"var arr = [new C(), new C()]; arr[0].reset(); to_string(arr[0] == null) + \" \" + to_string(arr[1] == null)", "true false"},
                {"var m = {\"k\": new C()}; m[\"k\"].reset(); to_string(m[\"k\"] == null) + \" \" + to_string(m.size())", "true 1"},
                {"var d = new C(); weak_ptr<C> w = weak_ptr<C>(d); w.reset(); to_string(w.lock() == null) + \" \" + to_string(d.v)", "true 1"},
                {"make().reset(); to_string(keep == null) + \" \" + to_string(keep.v)", "false 1"},
                {"function local() -> bool { var x = new C(); x.reset(); return x == null; } to_string(local())", "true"},
                {"var c = new C(); string m = \"\"; try { c.reset(1); } catch (err) { m = err; } m + \" \" + to_string(c == null)", "reset(h) takes a handle (new T(), make_shared<T>() or a shared_ptr), not int false"},
                {"var d = new C(); weak_ptr<C> w = weak_ptr<C>(d); string m = \"\"; try { w.reset(d); } catch (err) { m = err; } m", "reset() takes no arguments"},
            };
            for (const auto& [source, expected] : cases) {
                auto e = make_engine();
                stdlib::register_all(*e);
                check_eq(std::string(expected), e->execute(prelude + source).as<std::string>(), source);
            }
        });

        // C++ `p.reset(new T())`: p.reset(h) points the receiver at h's object, leaves the object it held to its other owners and
        // never writes through it or runs operator=; h is a handle of a class the receiver admits, as a shared_ptr<T> assignment
        test("handle_reset_with_a_handle_repoints_the_receiver", [this]() {
            const std::string prelude =
                "class C { int v = 1; void operator=(C o) { v = 1000; } } class D : C { int w = 2; } class U { int u = 3; } "
                "class H { shared_ptr<C> p; } function fresh(int v) -> shared_ptr<C> { var c = new C(); c.v = v; return c; } ";
            const std::pair<const char*, const char*> cases[] = {
                {"shared_ptr<C> p = new C(); shared_ptr<C> a = p; p.reset(fresh(5)); to_string(p.v) + \" \" + to_string(a.v) + \" \" + to_string(same_object(p, a))", "5 1 false"},
                {"var p = new C(); var a = p; p.reset(new C()); to_string(p.v) + \" \" + to_string(a.v) + \" \" + to_string(same_object(p, a))", "1 1 false"},
                {"shared_ptr<C> p = new C(); shared_ptr<C> q = fresh(7); p.reset(q); to_string(same_object(p, q)) + \" \" + to_string(p.v)", "true 7"},
                {"shared_ptr<C> p = new C(); p.reset(make_shared<C>()); to_string(p.v)", "1"},
                {"shared_ptr<C> p = new C(); p.reset(new D()); reflect::type_of(p)", "D"},
                {"shared_ptr<C> p = new C(); shared_ptr<C> n = null; p.reset(n); to_string(p == null)", "true"},
                {"shared_ptr<C> p = new C(); p.reset(null); to_string(p == null)", "true"},
                {"shared_ptr<C> p = null; p.reset(fresh(4)); to_string(p.v)", "4"},
                {"var h = H(); h.p = new C(); var old = h.p; h.p.reset(fresh(6)); to_string(h.p.v) + \" \" + to_string(old.v)", "6 1"},
                {"var arr = [new C()]; var old = arr[0]; arr[0].reset(fresh(8)); to_string(arr[0].v) + \" \" + to_string(old.v)", "8 1"},
                {"function local() -> int { shared_ptr<C> x = new C(); x.reset(fresh(9)); return x.v; } to_string(local())", "9"},
                {"var p = new C(); string m = \"\"; try { p.reset(C()); } catch (err) { m = err; } m + \" \" + to_string(p.v)", "reset(h) takes a handle (new T(), make_shared<T>() or a shared_ptr), not C 1"},
                {"var p = new D(); var c = new C(); string m = \"\"; try { p.reset(c); } catch (err) { m = err; } m", "Cannot reset shared_ptr<D> to a shared_ptr<C>"},
                {"var p = new C(); var u = new U(); string m = \"\"; try { p.reset(u); } catch (err) { m = err; } m", "Cannot reset shared_ptr<C> to a shared_ptr<U>"},
                {"shared_ptr<C> p = new C(); var u = new U(); string m = \"\"; try { p.reset(u); } catch (err) { m = err; } m", "Cannot reset shared_ptr<C> to a shared_ptr<U>"},
                {"var p = new C(); string m = \"\"; try { p.reset(new C(), new C()); } catch (err) { m = err; } m", "reset() takes at most one argument"},
            };
            for (const auto& [source, expected] : cases) {
                auto e = make_engine();
                stdlib::register_all(*e);
                std::string got;
                try { got = e->execute(prelude + source).as<std::string>(); } catch (const std::exception& error) { got = std::string("THREW ") + error.what(); }
                check_eq(std::string(expected), got, source);
            }
            // the checker reports a misuse it can see at load
            const std::pair<const char*, const char*> loads[] = {
                {"shared_ptr<C> p = new C(); p.reset(5);", "reset(h) takes a handle (new T(), make_shared<T>() or a shared_ptr), not int"},
                {"shared_ptr<C> p = new C(); p.reset(C());", "reset(h) takes a handle (new T(), make_shared<T>() or a shared_ptr), not C"},
                {"shared_ptr<C> p = new C(); p.reset(new U());", "Cannot reset shared_ptr<C> to a shared_ptr<U>"},
                {"shared_ptr<C> p = new C(); p.reset(new C(), new C());", "reset() takes at most one argument"},
            };
            for (const auto& [source, expected] : loads) {
                auto e = make_engine();
                stdlib::register_all(*e);
                std::string got;
                try { e->execute(prelude + source); got = "loaded"; } catch (const std::exception& error) { got = error.what(); }
                check_true(got.find(expected) != std::string::npos, std::string(source) + " -> " + got);
            }
        });

        test("shared_ptr_construction_across_hot_reload", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class Foo { int x = 1; }
                    class Bar { int y = 2; }
                    var a = new Foo();
                )");
                check_eq((int64_t)1, e->execute("a.x").as_int());
                // redefine ONE class, then construct both spellings again
                e->execute("class Foo { int x = 1; int z = 7; }");
                e->execute("var a2 = new Foo(); var b2 = new Bar();");
                check_eq((int64_t)7, e->execute("a2.z").as_int());
                check_eq((int64_t)2, e->execute("b2.y").as_int());
            }
        });

        test("method_ic_reload_at_hot_call_site", [this]() {
            // The vm's monomorphic method IC (call_site::mic_*) revalidates on
            // method_epoch: a hot call site crossing a class redefinition must
            // dispatch the NEW body immediately, on both backends.
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class C { int f() { return 1; } }
                    var c = C();
                    int hot() { int s = 0; for (int i = 0; i < 5; ++i) { s += c.f(); } return s; }
                )");
                check_eq((int64_t)5, e->execute("hot()").as_int(), use_vm ? "vm pre-reload" : "interp pre-reload");
                e->execute("class C { int f() { return 2; } }");
                check_eq((int64_t)10, e->execute("hot()").as_int(),
                         use_vm ? "vm hot site sees reloaded body" : "interp hot site sees reloaded body");
            }
        });

        test("method_ic_base_reload_invalidates_derived_site", [this]() {
            // Epoch propagation through derived_classes_: redefining the BASE must
            // invalidate a hot site whose cached receiver class is the DERIVED.
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class B { int f() { return 1; } }
                    class D : B { int pad = 0; }
                    var d = D();
                    int hot() { int s = 0; for (int i = 0; i < 5; ++i) { s += d.f(); } return s; }
                )");
                check_eq((int64_t)5, e->execute("hot()").as_int());
                e->execute("class B { int f() { return 2; } }");
                check_eq((int64_t)10, e->execute("hot()").as_int(),
                         use_vm ? "vm derived site sees base reload" : "interp derived site sees base reload");
            }
        });

        test("method_ic_typed_param_site_stays_type_sensitive", [this]() {
            // A cached single-overload method with TYPED params must keep rejecting
            // by argument type at the same hot site, with backend-identical outcomes.
            std::array<std::string, 2> outcomes;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class T { int f(int x) { return x + 1; } }
                    var t = T();
                    var hot(var v) { var msg = "ok"; try { msg = t.f(v); } catch (err) { msg = err; } return msg; }
                )");
                check_eq((int64_t)2, e->execute("hot(1)").as_int(), "int arg dispatches");
                check_eq((int64_t)3, e->execute("hot(2)").as_int(), "site stays hot");
                outcomes[use_vm ? 1 : 0] = e->execute("hot(\"s\")").as<std::string>();
            }
            check_eq(outcomes[0], outcomes[1], "backends agree on typed-param mismatch outcome");
        });

        test("method_ic_untyped_param_site_accepts_any_type", [this]() {
            // Untyped (var) params make resolution arg-independent (mic_static): the
            // hot site must accept a different arg type on every call, no lock-in.
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class U { int n = 0; void f(var v) { n += 1; } }
                    var u = U();
                    int hot(var v) { u.f(v); return u.n; }
                )");
                check_eq((int64_t)1, e->execute("hot(1)").as_int());
                check_eq((int64_t)2, e->execute("hot(2)").as_int());
                check_eq((int64_t)3, e->execute("hot(\"s\")").as_int(), "string arg through the hot site");
                check_eq((int64_t)4, e->execute("hot(2.5)").as_int(), "float arg through the hot site");
                check_eq((int64_t)5, e->execute("hot([1,2])").as_int(), "array arg through the hot site");
            }
        });

        test("method_ic_polymorphic_site", [this]() {
            // One call site alternating receiver classes: the monomorphic cache must
            // miss-and-refill without ever dispatching the wrong class's method.
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class A { int f() { return 1; } }
                    class Z { int f() { return 10; } }
                    var items = [new A(), new Z(), new A(), new Z()];
                    int hot() { int s = 0; for (int i = 0; i < 4; ++i) { s += items[i].f(); } return s; }
                )");
                check_eq((int64_t)22, e->execute("hot()").as_int(), use_vm ? "vm alternating classes" : "interp alternating classes");
                check_eq((int64_t)22, e->execute("hot()").as_int(), "second pass identical");
            }
        });

        test("method_ic_host_added_field_shadows_method_at_hot_site", [this]() {
            // Host C++ set_field can add an instance field named like a method
            // (insert_or_assign contract); the hit path re-probes has_field, so the
            // shadow must take effect at an already-hot site, backend-identically.
            std::array<std::string, 2> outcomes;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class S { int f() { return 1; } }
                    var s = S();
                    var hot() { var msg = "ok"; try { msg = s.f(); } catch (err) { msg = err; } return msg; }
                )");
                check_eq((int64_t)1, e->execute("hot()").as_int());
                check_eq((int64_t)1, e->execute("hot()").as_int(), "site hot before shadow");
                auto sv = e->execute("s");
                auto holder = sv.get_object_holder();
                check_not_null(holder.get(), "instance holder");
                auto instance = holder->instance_handle();
                instance->set_field(detail::engine_access::symbolize(*e, "f"), jai::script_value((int64_t)42, e.get()));
                auto after = e->execute("hot()");
                outcomes[use_vm ? 1 : 0] = after.is_string() ? after.as<std::string>()
                                                             : std::to_string(after.as_int());
            }
            check_eq(outcomes[0], outcomes[1], "backends agree on field-shadow outcome");
        });

        test("field_read_ic_reload_at_hot_site", [this]() {
            // vm GET_MEMBER site IC (chunk::member_ic) revalidates on method_epoch: a user
            // _get_x added by reload serves an already-hot read site, and a removed field
            // errors — identically on both backends.
            std::array<std::string, 2> removed;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class P { int x = 3; }
                    var p = P();
                    int hot() { int s = 0; for (int i = 0; i < 5; ++i) { s += p.x; } return s; }
                )");
                check_eq((int64_t)15, e->execute("hot()").as_int());
                e->execute("p.x = 4;");
                check_eq((int64_t)20, e->execute("hot()").as_int(), "hot site reads the live cell");
                // A user _get_x owns the member: the reload bumps the epoch, the hot site
                // revalidates and dispatches to the getter (both backends)
                e->execute("class P { int x = 3; int _get_x() { return 100; } }");
                check_eq((int64_t)500, e->execute("hot()").as_int(),
                         use_vm ? "vm getter serves the hot site across reload" : "interp getter serves the hot site across reload");
                e->execute("class P { int y = 1; }");
                removed[use_vm ? 1 : 0] = e->execute(
                    "var msg = \"\"; try { var t = hot(); msg = \"ran\"; } catch (err) { msg = err; } msg").as<std::string>();
                check_eq(std::string("V:P has no member 'x'"), e->execute(
                    "var m3 = \"\"; try { var t3 = p.x; m3 = \"ran\"; } catch (err3) { m3 = \"V:\" + err3; } m3").as<std::string>(),
                    use_vm ? "vm plain removed-field read errors" : "interp plain removed-field read errors");
            }
            check_eq(std::string("P has no member 'x'"), removed[1],
                     "vm removed-field text at the hot site");
            check_eq(removed[0], removed[1],
                     "backends agree at the hot site (compound-RHS error fix)");
        });

        test("compound_rhs_error_surfaces_original_exception", [this]() {
            // A compound assign whose RHS raises must surface the ORIGINAL exception,
            // not a follow-on arithmetic error against the placeholder. The interpreter
            // clobbered the catch value on three compound paths (slot target, member
            // target, subscript target — the constrained-ref branch had the unwinding
            // check, its siblings didn't); the vm was correct throughout. Every shape
            // asserts the exact text on BOTH backends.
            const char* expected = "V:R2 has no member 'x'";
            const std::pair<const char*, const char*> shapes[] = {
                {"fn-only",
                 "int f1() { return r2.x; } var m = \"\"; try { var t = f1(); m = \"ran\"; } catch (er) { m = \"V:\" + er; } m"},
                {"counted-loop-in-fn compound slot target",
                 "int f2() { int s = 0; for (int i = 0; i < 5; ++i) { s += r2.x; } return s; } var m = \"\"; try { var t = f2(); m = \"ran\"; } catch (er) { m = \"V:\" + er; } m"},
                {"compound env target",
                 "int gs = 0; var m = \"\"; try { gs += r2.x; m = \"ran\"; } catch (er) { m = \"V:\" + er; } m"},
                {"compound member target",
                 "int f5() { r2.y += r2.x; return 0; } var m = \"\"; try { var t = f5(); m = \"ran\"; } catch (er) { m = \"V:\" + er; } m"},
                {"compound subscript target",
                 "int f6() { var a = [1]; a[0] += r2.x; return 0; } var m = \"\"; try { var t = f6(); m = \"ran\"; } catch (er) { m = \"V:\" + er; } m"},
            };
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute("class R2 { int y = 1; } var r2 = R2();");
                for (const auto& [label, src] : shapes) {
                    check_eq(std::string(expected), e->execute(src).as<std::string>(),
                             std::string(use_vm ? "vm: " : "interp: ") + label);
                }
            }
        });

        test("field_store_ic_reload_and_setter_shadow", [this]() {
            // Write-side twin of field_read_ic: the vm SET_MEMBER site IC revalidates on
            // method_epoch — a user _set_ added by reload fires at an already-hot store
            // site (declared field or not), typed-store conversion errors stay
            // backend-identical, and the synthesized setter never blocks the slot path.
            std::array<std::string, 2> badstore;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class W { int x = 0; int log = 0; }
                    var w = W();
                    void hot(int v) { for (int i = 0; i < 5; ++i) { w.x = v + i; } }
                )");
                e->execute("hot(10);");
                check_eq((int64_t)14, e->execute("w.x").as_int(), "hot store site writes the cell");
                // A user _set_x for a DECLARED field owns the store: the reload bumps the
                // epoch and the hot site dispatches to the setter (both backends)
                e->execute("class W { int x = 0; int log = 0; void _set_x(int v) { x = v; log = log + 1; } }");
                e->execute("hot(20);");
                check_eq((int64_t)24, e->execute("w.x").as_int(), "the setter's bare store lands");
                check_eq((int64_t)5, e->execute("w.log").as_int(),
                         use_vm ? "vm user _set_x fires at the hot site" : "interp user _set_x fires at the hot site");
                // a custom setter for a NON-field name DOES fire (no auto competition,
                // and the store site stays negative in the IC — no slot exists)
                e->execute(R"(
                    class V { int seen = 0; void _set_virt(int v) { seen = seen + v; } }
                    var vv = V();
                    void hotv(int v) { for (int i = 0; i < 3; ++i) { vv.virt = v; } }
                )");
                e->execute("hotv(7);");
                check_eq((int64_t)21, e->execute("vv.seen").as_int(),
                         use_vm ? "vm non-field custom setter at hot site" : "interp non-field custom setter at hot site");
                // typed-store conversion error at a hot site: identical outcome both backends
                badstore[use_vm ? 1 : 0] = e->execute(
                    "var m = \"\"; try { w.x = [1,2]; m = \"stored\"; } catch (er) { m = er; } m").as<std::string>();
            }
            check_eq(badstore[0], badstore[1], "backends agree on typed-store mismatch outcome");
        });

        test("bare_sibling_method_calls_in_loop", [this]() {
            // Bare sibling calls (helper() instead of this.helper()) mint the typed
            // bound_method thunk and enter the vm in-loop; behavior must stay identical
            // to the old opaque lambda on both backends: dispatch, sibling calls inside
            // a coroutine body, coroutine-method minting through a bare call, the thunk
            // as a stored first-class value, and typed-arg decline error text.
            std::array<std::string, 2> err;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(R"(
                    class S {
                        int v = 3; var brain = null; int ticks = 0;
                        int helper(int k) { return v * k; }
                        int outer() { return helper(4) + helper(1); }
                        int grab() { var fv = [this](int k) { return helper(k); }; return fv(3); }
                        coroutine void steps() { while (true) { ticks = ticks + helper(1); yield; } }
                        void tick() { if (brain == null || brain.done()) { brain = steps(); } brain.next(); }
                    }
                    var s = S();
                )");
                check_eq((int64_t)15, e->execute("s.outer()").as_int(),
                         use_vm ? "vm bare sibling dispatch" : "interp bare sibling dispatch");
                e->execute("for (int i = 0; i < 5; ++i) { s.tick(); }");
                check_eq((int64_t)15, e->execute("s.ticks").as_int(),
                         use_vm ? "vm siblings inside coroutine body" : "interp siblings inside coroutine body");
                check_eq((int64_t)9, e->execute("s.grab()").as_int(), "thunk as stored first-class value");
                err[use_vm ? 1 : 0] = e->execute(
                    "var m = \"\"; try { var t = s.v; class X {} var q = X(); m = \"pre\"; } catch (er0) { m = er0; } "
                    "try { var t2 = s.outer(); s.helper([1]); m = \"ran\"; } catch (er) { m = er; } m").as<std::string>();
            }
            check_eq(err[0], err[1], "backends agree on typed-arg decline outcome");
        });

        // C++ spells a reference-returning method with the reference on the leading type, in a class body too
        test("leading_reference_return_type_on_methods", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(e);
                const std::string tag = use_vm ? " (vm)" : " (interpreter)";
                auto run = [&](const char* src) {
                    try { return e->execute(src).to_string(); } catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
                };
                check_eq(std::string("5"), run("class Counter { int n = 0; int& slot() { return n; } } var c = Counter(); auto& r = c.slot(); r = 5; to_string(c.n)"), "int& method" + tag);
                check_eq(std::string("9"), run("class Box { int v = 1; Box& pick(Box& other) { return other; } } var a = Box(); var b = Box(); auto& r = a.pick(b); r.v = 9; to_string(b.v)"), "a method returning its own class by reference" + tag);
                check_eq(std::string("3"), run("class V { int x = 3; int& operator[](int i) { return x; } } var v = V(); to_string(v[0])"), "an operator returning a reference" + tag);
                check_eq(std::string("7"), run("class W { static int& first(array<int>& a) { return a[0]; } } var xs = [1, 2]; auto& r = W::first(xs); r = 7; to_string(xs[0])"), "a static method" + tag);
            }
        });

        test("member_access_hot_reload_and_host_api", [this]() {
            // Hot reload is permissive: enforcement consults the CURRENT class_definition
            // at access time — instances keep working, new accesses follow new labels.
            // Host C++ get_field stays unrestricted by design.
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(e);
                e->execute("class R { public: int v = 4; } var r = R();");
                check_eq((int64_t)4, e->execute("r.v").as_int(), "public before reload");
                // reload flips v to private
                e->execute("class R { private: int v = 4; public: int look() { return v; } }");
                auto blocked = e->execute("var msg = \"\"; try { var x = r.v; msg = \"leak\"; } catch (err) { msg = err; } msg");
                check_eq(std::string("Cannot access private member 'v' of class 'R'"),
                         blocked.as<std::string>(), use_vm ? "vm reload enforces" : "interp reload enforces");
                check_eq((int64_t)4, e->execute("r.look()").as_int(), "instance keeps working via method");
                // host C++ API is unrestricted
                auto rv = e->execute("r");
                auto holder = rv.get_object_holder();
                check_not_null(holder.get(), "instance holder");
                auto instance = holder->instance_handle();
                uint64_t v_id = detail::engine_access::symbolize(*e, "v");
                check_eq((int64_t)4, instance->get_field(v_id).as_int(), "host get_field bypasses enforcement");
            }
        });

        // C++ aggregate initialization ([dcl.init.aggr]; detail/aggregate_init.hpp): a class declaring no constructor
        // takes one initializer per instance field in declaration order, bases first
        auto ints = [this](const script_value& r, const std::vector<int64_t>& expected, const std::string& what) {
            const auto& v = r.as_array();
            check_eq(expected.size(), v.size(), what + " (count)");
            for (size_t i = 0; i < expected.size() && i < v.size(); ++i) { check_eq(expected[i], v[i].as_int(), what); }
        };

        test("aggregate_initialization_every_spelling", [this, ints]() {
            auto e = make_engine();
            ints(e->execute(R"(
                class P { int x; int y; }
                P a{1, 2};
                P b(3, 4);
                auto c = P{5, 6};
                auto d = P(7, 8);
                var h = new P{9, 10};
                return [a.x, a.y, b.x, b.y, c.x, c.y, d.x, d.y, h.x, h.y];
            )"), {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, "P a{..}, P b(..), P{..}, P(..), new P{..}");
        });

        test("aggregate_initialization_fewer_initializers_keep_the_rest", [this, ints]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                int calls = 0;
                function bump() -> int { calls += 1; return 9; }
                class Q { int x = bump(); int y = bump(); int z; string s = "d"; }
                Q q{1};
                class R { int a; int b = this.a + 1; }
                R r{5};
                return [q.x, q.y, q.z, calls, r.b, q.s];
            )");
            const auto& v = r.as_array();
            const std::vector<int64_t> expected{1, 9, 0, 1, 6};
            for (size_t i = 0; i < expected.size(); ++i) {
                check_eq(expected[i], v[i].as_int(), "an initialized field skips its initializer, the rest run theirs in order and read the earlier fields");
            }
            check_eq(std::string("d"), v[5].as<std::string>(), "a string field keeps its initializer");
        });

        test("aggregate_initialization_bases_first_and_nested", [this, ints]() {
            auto e = make_engine();
            ints(e->execute(R"(
                class B { int a = 0; }
                class D : B { int b = 0; int sum() { return a + b; } }
                D d{1, 2};
                class In { int v; }
                class Out { In i; int n; }
                Out o{In{4}, 5};
                In copy = In(o.i);
                copy.v = 7;
                return [d.a, d.b, D{3, 4}.sum(), o.i.v, o.n, copy.v];
            )"), {1, 2, 7, 4, 5, 7}, "bases first, methods allowed, a nested aggregate, one same-class argument copies");
            ints(e->execute("[o.i.v]"), {4}, "the copy is its own object");
        });

        test("aggregate_initialization_converts_through_each_field", [this]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                class F { float f; int i; var any; }
                F x{1, true, "t"};
                return [x.f / 2, x.i, x.any];
            )");
            const auto& v = r.as_array();
            check_near(0.5, v[0].as_float(), 1e-12);
            check_eq((int64_t)1, v[1].as_int(), "bool converts into an int field");
            check_eq(std::string("t"), v[2].as<std::string>(), "a var field takes anything");
        });

        // C++ implicit base construction: a constructor naming no `: super(...)`, and a class declaring no constructor,
        // default-construct the base (its constructor runs) ahead of the derived class's fields
        test("a_constructor_without_super_default_constructs_its_base", [this, ints]() {
            auto e = make_engine();
            ints(e->execute(R"(
                class A { int a = 1; int log = 0; A() { log = 10; } }
                class B : A { int b = 2; int seen = this.log; B() { b = log + 5; } }
                B x = B();
                class Opt { int v = 0; Opt(int given = 4) { v = given; } }
                class UsesOpt : Opt { UsesOpt() {} }
                UsesOpt o = UsesOpt();
                return [x.log, x.b, x.seen, o.v];
            )"), {10, 15, 10, 4}, "the base constructor ran before the derived fields and body; a defaulted parameter is a default constructor");
        });

        test("a_constructorless_class_runs_its_base_default_constructor", [this, ints]() {
            auto e = make_engine();
            ints(e->execute(R"(
                class A { int log = 0; A() { log = 10; } }
                class C : A { int c = 3; int seen = this.log; }
                C y = C();
                class D : C { D() {} }
                D z = D();
                class E : C { }
                E w = E();
                return [y.log, y.c, y.seen, z.log, z.seen, w.log];
            )"), {10, 3, 10, 10, 10, 10}, "through constructor-less levels, base first");
        });

        test("a_base_without_a_default_constructor_raises", [this]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                [[warn_mode]]
                function t(function f) -> string { string r = "none"; try { f(); } catch (e) { r = e; } return r; }
                class P { int v = 0; P(int given) { v = given; } }
                class Q : P { Q() {} }
                class R : P { }
                class S : R { S() {} }
                class Fine : P { Fine() : super(7) {} }
                return [t([]() { var q = Q(); }), t([]() { var r = R(); }), t([]() { var s = S(); }), t([]() { var f = Fine(); })];
            )");
            const auto& v = r.as_array();
            const std::string text = "Base 'P' has no default constructor; call super(...)";
            check_eq(text, v[0].as<std::string>(), "a constructor without super()");
            check_eq(text, v[1].as<std::string>(), "a constructor-less class");
            check_eq(text, v[2].as<std::string>(), "through a constructor-less level");
            check_eq(std::string("none"), v[3].as<std::string>(), "an explicit super(...) is untouched");
            auto checked = e->check("class P { P(int given) {} } class Q : P { Q() {} } class R : P { } var r = R();");
            size_t found = 0;
            for (const auto& d : checked.diagnostics) { if (d.message == text) { ++found; } }
            check_eq((size_t)2, found, "the checker reports the constructor and the construction");
        });

        test("aggregate_initialization_errors", [this]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                [[warn_mode]]
                function t(function f) -> string { string r = "none"; try { f(); } catch (e) { r = e; } return r; }
                class P { int x; int y; }
                class E { }
                class H { private: int v; }
                class Base { int a; Base(int v) { a = v; } }
                class Derived : Base { int b; }
                var one = 1; var word = "w";
                return [t([]() { var p = P(one, one, one); }), t([]() { var p = E{one}; }), t([]() { var p = P{word}; }),
                        t([]() { var h = H(one); }), t([]() { var d = Derived(one, one); })];
            )");
            const auto& v = r.as_array();
            check_eq(std::string("Too many initializers for 'P': it has 2 fields, got 3"), v[0].as<std::string>(), "too many");
            check_eq(std::string("Too many initializers for 'E': it has 0 fields, got 1"), v[1].as<std::string>(), "an empty class");
            check_true(v[2].as<std::string>().find("field 'x'") != std::string::npos, "the field's own conversion text: " + v[2].as<std::string>());
            check_eq(std::string("No constructor of 'H' takes (int)"), v[3].as<std::string>(), "a private field: not an aggregate");
            check_eq(std::string("No constructor of 'Derived' takes (int, int)"), v[4].as<std::string>(), "a base with a constructor: not an aggregate");
        });

        // a call that ends without a value yields null whatever it was called on: only a constructor's call yields its object
        auto nulls = [this](const script_value& r, size_t count, const std::string& what) {
            const auto& v = r.as_array();
            check_eq(count, v.size(), what + " (count)");
            for (size_t i = 0; i < v.size(); ++i) { check_true(v[i].is_null(), what + ": result " + std::to_string(i) + " is " + v[i].to_string()); }
        };

        test("a_method_that_ends_without_a_value_yields_null", [this, nulls]() {
            auto e = make_engine();
            stdlib::register_all(*e);
            nulls(e->execute(R"(
                class C {
                    int n = 1;
                    void m() {}
                    void early() { return; }
                    var v() {}
                    auto a() {}
                    function f() {}
                    void g<T>(T x) {}
                    void operator()(int k) { n = k; }
                }
                class D : C { void m() override {} }
                class Box<T> { T held; void put(T x) { held = x; } }
                var through(C& c) { return c.m(); }
                C c; var any = c; D d; Box<int> box;
                shared_ptr<C> p = make_shared<C>(); shared_ptr<C> pd = make_shared<D>(); weak_ptr<C> w = p; var made = new C();
                var kept = c.m();
                return [kept, c.m(), any.m(), c.early(), c.v(), c.a(), c.f(), c.g<int>(1), c.g("s"), box.put(3), p.m(), pd.m(), w.lock().m(),
                        made.m(), through(d), c(4), true ? c.m() : c.m(), false ? c.m() : c.m()];
            )"), 18, "a typed, var, generic, handle and base receiver");
            check_true(e->execute("c.m()").is_null(), "the host reads null from a program that ends in the call");
            check_eq((int64_t)4, e->execute("c.n").as_int(), "operator() ran on its object");
        });

        test("a_method_that_returns_nothing_yields_null_inside_a_class", [this, nulls]() {
            auto e = make_engine();
            nulls(e->execute(R"(
                class Other { int o = 1; void m() {} }
                class C {
                    int n = 1;
                    void m() {}
                    array<var> inside() { return [m(), this.m()]; }
                    array<var> beside_a_lambda(Other x) { var bump = [&]() { n += 1; }; bump(); return [m(), x.m()]; }
                    array<var> in_a_try(Other x) { try { return [m(), x.m()]; } catch (e) { return [e]; } }
                    array<var> in_a_loop(Other x) { array<var> out; for (int i = 0; i < 2; ++i) { out.push(x.m()); } return out; }
                }
                class D : C { void m() override {} array<var> up() { return [super::m(), m()]; } }
                C c; D d; Other o;
                array<var> all = c.inside();
                all.concat(c.beside_a_lambda(o)); all.concat(c.in_a_try(o)); all.concat(c.in_a_loop(o)); all.concat(d.up());
                return all;
            )"), 10, "bare, through this and super, and on another object");
        });

        test("a_path_of_a_method_that_reaches_its_end_yields_null", [this, nulls]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                [[warn_mode]]
                class Part { int p = 2; }
                class C {
                    int n = 1;
                    var pick(bool p) { if (p) { return 1; } }
                    auto deduced(bool p) { if (p) { return 1; } }
                    var down(int d) { if (d > 0) { return down(d - 1); } }
                    var guarded() { try { n += 1; } catch (e) { } }
                    Part part() { }
                    shared_ptr<Part> handle() { }
                    array<int> list() { }
                }
                var pick(bool p) { if (p) { return 1; } }
                C c;
                return [[pick(true), c.pick(true), c.deduced(true)],
                        [pick(false), c.pick(false), c.deduced(false), c.down(3), c.guarded(), c.part(), c.handle(), c.list()]];
            )");
            const auto& v = r.as_array();
            for (const script_value& returned : v[0].as_array()) { check_eq((int64_t)1, returned.as_int(), "the returning path"); }
            nulls(v[1], 8, "as a function's does");
        });

        test("a_function_or_lambda_called_in_a_method_yields_null_when_it_returns_nothing", [this, nulls]() {
            auto e = make_engine();
            nulls(e->execute(R"(
                void nothing() {}
                var call(function f) { return f(); }
                class C {
                    int n = 1;
                    auto later() { return nothing(); }
                    array<var> run() {
                        var free = nothing;
                        return [later(), free(), call(nothing), call([]() { }), call([&]() { n = 2; }), call([this]() { n = 3; }), call(() => nothing())];
                    }
                    function plain() { return []() { }; }
                    function capturing() { return [this]() { n = 4; }; }
                }
                C c;
                array<var> all = c.run();
                all.concat([c.later(), call(c.plain()), call(c.capturing())]);
                return all;
            )"), 10, "a deduced method returning the call, a function value and a lambda");
            check_eq((int64_t)4, e->execute("c.n").as_int(), "each lambda ran");
        });

        test("a_method_that_returns_nothing_yields_null_through_a_bound_value_and_a_callback", [this, nulls]() {
            auto e = make_engine();
            stdlib::register_all(*e);
            nulls(e->execute(R"(
                class C { int n = 1; void m() {} void take(int x) {} var fold(var sum, int x) {} }
                C c; array<int> xs = [1, 2];
                var bound = reflect::bind(c, "m");
                var take = reflect::bind(c, "take");
                array<var> all = [bound(), reflect::invoke(c, "m"), reflect::invoke(c, "take", 1), xs.reduce(reflect::bind(c, "fold"), 0)];
                all.concat(array::map(xs, (int x) => c.take(x))); all.concat(array::map(xs, take));
                all.concat(parallel_transform(xs, (int x) => c.take(x)));
                return all;
            )"), 10, "reflect::bind, reflect::invoke and a builtin that reads its callback's result");
            auto called = e->get_variable("bound").as_function()({});
            check_true(called && called.value().is_null(), "the host calls the bound method");
        });

        test("a_method_that_returns_nothing_reads_as_null_in_an_error", [this]() {
            auto e = make_engine();
            auto r = e->execute(R"(
                namespace shop { class Till { int n = 1; void ring() {} int total() { return rung; } } }
                shop::Till till;
                var rung = till.ring();
                var any = till;
                array<var> out;
                try { out.push(`${till.total()}`); } catch (e) { out.push(e); }
                try { out.push(`${any.ring().n}`); } catch (e) { out.push(e); }
                try { rung.n = 5; out.push(`${till.n}`); } catch (e) { out.push(e); }
                return out;
            )");
            const auto& v = r.as_array();
            check_eq(std::string("Cannot return null from 'total' (declared -> int)"), v[0].as<std::string>(), "a typed return of the result");
            check_eq(std::string("Cannot access 'n' on null ('ring(...)' returned null)"), v[1].as<std::string>(), "a member read on the result");
            check_eq(std::string("Cannot assign to 'n' on null ('rung' is null)"), v[2].as<std::string>(), "a member store through the result");
        });

        test("a_constructor_call_yields_its_object_and_an_operator_what_it_returns", [this, ints, nulls]() {
            struct Host { int id = 5; };
            auto e = make_engine();
            stdlib::register_all(*e);
            dynamic_binder<Host>(*e, "Host").constructor<>().property("id", &Host::id).build();
            ints(e->execute(R"(
                class P { int a = 1; P() { a = 2; } P(int v) { a = v; if (v > 5) { return; } a = v + 1; } }
                class Q : P { int b = 0; Q() : super(7) { b = 1; } Q(int x) : this() { b = x; } }
                class R : P { int r = 3; }
                class Plain { int x = 4; }
                class Over : Host { int extra = 6; Over() : super() {} }
                P p; P copy = p; var make = P; shared_ptr<P> h = make_shared<P>(4); var n = new Q(6); Q q; array<P> ps = [P(), P(1)];
                return [p.a, P(3).a, P(9).a, q.a, q.b, Q(5).a, Q(5).b, R().a, R().r, h.a, n.a, n.b, make(2).a, Plain().x, Plain{9}.x, ps[1].a,
                        copy.a, reflect::construct("P", 8).a, reflect::construct_shared("Q").b, Over().id, Over().extra];
            )"), {2, 4, 9, 7, 1, 7, 5, 2, 3, 5, 7, 6, 3, 4, 9, 2, 2, 8, 1, 5, 6}, "every construction, one ended by a bare return included");
            ints(e->execute(R"(
                class Tally {
                    int n = 0;
                    Tally& operator+=(int k) { n += k; return this; }
                    Tally& operator++() { n += 1; return this; }
                    Tally operator++(int) { Tally old = this; n += 1; return old; }
                    Tally& operator=(int k) { n = k; return this; }
                    Tally& self() { return this; }
                }
                Tally t; t += 5; ++t; Tally before = t++; array<Tally> all = [t]; all[0] += 10;
                return [t.n, before.n, all[0].n, (t += 1).n, (++t).n, (t = 3).n, t.self().n];
            )"), {7, 6, 17, 8, 9, 3, 3}, "an operator or a method that returns this");
            auto r = e->execute(R"(
                class Quiet {
                    int n = 0;
                    void operator+=(int k) { n += k; }
                    void operator++() { n += 1; }
                    void operator=(int k) { n = k; }
                }
                Quiet s; s += 5; ++s; array<Quiet> some = [s]; some[0] += 10;
                var added = (s += 1); var stepped = ++s; var assigned = (s = 2);
                return [[added, stepped], [s.n, some[0].n, assigned.n]];
            )");
            const auto& v = r.as_array();
            nulls(v[0], 2, "an in-place operator that returns nothing");
            ints(v[1], {2, 16, 2}, "it changed its object, and an assignment yields its target");
        });
    }
};

} // namespace jai::foundry::tests

using script_class_tests = jai::foundry::tests::script_class_tests;
FOUNDRY_REGISTER(script_class_tests)