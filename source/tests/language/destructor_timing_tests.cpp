#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"

#include <string>

using namespace jai::foundry;

namespace jai::foundry::tests {

struct teardown_mark {
    std::string* log;
    std::string name;
    teardown_mark(std::string* a_log, std::string a_name) : log(a_log), name(std::move(a_name)) {}
    ~teardown_mark() { *log += name; }
};

// C++ object lifetimes (the contract): a block's locals die at its closing brace in reverse declaration order, a loop
// body's at the end of each iteration, parameters at the end of the callee after its locals, a prvalue argument is the
// parameter itself, a member dies after its own class's destructor body and before its base's, and program-scope
// variables die at engine teardown in reverse construction order. Every expected log below is the C++ program's.
class destructor_timing_tests : public suite {
public:
    destructor_timing_tests() : suite("Destructor Timing") {}

    static constexpr const char* k_prelude = R"(
        string LOG = "";
        class Token { string name = ""; Token() {} Token(string n) { name = n; } ~Token() { LOG = LOG + name; } }
    )";

    // The program's last expression is the log; both the default engine and one with proofs and passes off run it
    void expect_log(const std::string& program, const std::string& expected) {
        for (int variant = 0; variant < 2; ++variant) {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            if (variant == 1) {
                eng->type_proofs(false);
                eng->ir_passes(false);
            }
            std::string got;
            try {
                got = eng->execute(std::string(k_prelude) + program).as_string();
            } catch (const std::exception& e) {
                got = std::string("threw: ") + e.what();
            }
            check_eq(expected, got, variant == 0 ? "default engine" : "proofs and passes off");
        }
    }

    // No prelude: lifetimes hold whether or not a class declares a destructor. The programs run in order on one engine (a later one
    // declares what the functions an earlier one compiled never saw); the last one's result is the outcome
    void expect_outcome(const std::vector<std::string>& programs, const std::string& expected) {
        for (int variant = 0; variant < 2; ++variant) {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            if (variant == 1) {
                eng->type_proofs(false);
                eng->ir_passes(false);
            }
            std::string got;
            try {
                for (const std::string& program : programs) { got = eng->execute(program).as_string(); }
            } catch (const std::exception& e) {
                got = std::string("threw: ") + e.what();
            }
            check_eq(expected, got, variant == 0 ? "default engine" : "proofs and passes off");
        }
    }

    static constexpr const char* k_observed = R"(
        class P { int v = 3; }
        class Box { shared_ptr<P> t = null; var u = null; }
        string OUT = "";
        function note(string s) -> void { OUT = OUT + s + " "; }
    )";

    void forge_tests() override {
        test("locals_die_as_control_leaves_their_scope_innermost_first", [this]() {
            const std::pair<std::string, std::string> cases[] = {
                {R"(
                function f() -> void { Token a = Token("a"); { Token i = Token("i"); Token j = Token("j"); } LOG = LOG + "|"; Token b = Token("b"); }
                f(); LOG;
            )", "ji|ba"},
                // a const reference holds the temporary it binds and dies as a local, whatever ends its scope
                {R"(
                function mk(string n) -> Token { return Token(n); }
                function g() -> int {
                    Token a = Token("a"); const Token& r = mk("r");
                    { const Token& b = Token("b"); } LOG = LOG + "|";
                    for (int i = 0; i < 2; i++) { const Token& c = mk(to_string(i)); LOG = LOG + "."; }
                    Token z = Token("z"); return 1;
                }
                function h() -> void { const Token& t = mk("t"); throw "x"; }
                g(); try { h(); } catch (e) { LOG = LOG + "c"; } LOG;
            )", "b|.0.1zratc"},
                {R"(
                function f() -> void { for (int k = 0; k < 2; k++) { Token t = Token(to_string(k)); LOG = LOG + "."; } LOG = LOG + "|"; }
                f(); LOG;
            )", ".0.1|"},
                {R"(
                function f() -> void { int k = 0; while (k < 2) { Token t = Token(to_string(k)); k++; LOG = LOG + "."; } LOG = LOG + "|"; }
                f(); LOG;
            )", ".0.1|"},
                {R"(
                function f() -> void { for (int k = 0; k < 3; k++) { Token t = Token(to_string(k)); { Token u = Token("u"); if (k == 1) { break; } } LOG = LOG + "."; } LOG = LOG + "|"; }
                f(); LOG;
            )", "u.0u1|"},
                {R"(
                function f() -> void { for (int k = 0; k < 2; k++) { Token t = Token(to_string(k)); if (k == 0) { continue; } LOG = LOG + "."; } LOG = LOG + "|"; }
                f(); LOG;
            )", "0.1|"},
                {R"(
                function f() -> void { Token a = Token("a"); { Token b = Token("b"); if (true) { Token c = Token("c"); return; } } }
                f(); LOG;
            )", "cba"},
                // a range-for's range is the loop's outermost local: the body's locals and the loop variable die before it, the
                // locals declared ahead of the loop after it; nested loops end innermost first, and a range dies last to first
                {R"(
                function arr(string a, string b) -> array<Token> { array<Token> r = [Token(a), Token(b)]; return r; }
                function g() -> int { Token a = Token("a"); for (auto t : arr("x", "y")) { Token b = Token("b"); return 1; } return 0; }
                function h() -> int { for (auto o : arr("x", "y")) { for (auto i : arr("m", "n")) { return 1; } } return 0; }
                g(); LOG = LOG + "|"; h(); LOG = LOG + "|";
                for (auto t : arr("x", "y")) { LOG = LOG + "["; } LOG = LOG + "|";
                for (auto t : arr("x", "y")) { LOG = LOG + "["; break; } LOG;
            )", "bxyxa|mnmxyx|[x[yyx|[xyx"},
                {R"(
                function f() -> void { try { Token a = Token("a"); { Token b = Token("b"); throw 1; } } catch (e) { LOG = LOG + "!"; } LOG = LOG + "|"; }
                f(); LOG;
            )", "ba!|"},
                {R"(
                function g() -> void { Token a = Token("a"); { Token b = Token("b"); throw 1; } }
                function f() -> void { try { g(); } catch (e) { LOG = LOG + "!"; } }
                f(); LOG;
            )", "ba!"},
                // a range dies inside the scopes the throw leaves, after its loop's interior, in the same function or a callee
                {R"(
                function arr() -> array<Token> { array<Token> r = [Token("x"), Token("y")]; return r; }
                function boom() -> int { throw "boom"; return 0; }
                function f() -> void { try { Token a = Token("a"); for (auto t : arr()) { Token b = Token("b"); boom(); } } catch (e) { LOG = LOG + "c"; } LOG = LOG + "|"; }
                function g() -> void { Token a = Token("a"); for (auto t : arr()) { Token b = Token("b"); boom(); } }
                function k() -> void { try { g(); } catch (e) { LOG = LOG + "c"; } LOG = LOG + "|"; }
                f(); k(); LOG;
            )", "bxyxac|bxyxac|"},
                {R"(
                function f(Token p) -> void { Token l = Token("l"); LOG = LOG + "body"; }
                f(Token("p")); LOG;
            )", "bodylp"},
                {R"(
                function one(Token p) -> void { LOG = LOG + "body"; }
                function f() -> void { Token t = Token("t"); one(t); LOG = LOG + "|"; }
                f(); LOG;
            )", "bodyt|t"},
                {R"(
                function f(array<Token>& ts) -> void { for (Token t : ts) { LOG = LOG + "<" + t.name + ">"; } LOG = LOG + "|"; }
                array<Token> TS; TS.push(Token("p")); TS.push(Token("q"));
                LOG = ""; f(TS); LOG;
            )", "<p>p<q>q|"},
                {R"(
                function f(array<Token>& ts) -> void { for (auto& t : ts) { LOG = LOG + "<" + t.name + ">"; } LOG = LOG + "|"; }
                array<Token> TS; TS.push(Token("p")); TS.push(Token("q"));
                LOG = ""; f(TS); LOG;
            )", "<p><q>|"},
                {R"(
                function f() -> void { for (Token t = Token("i"); LOG.size() < 3; LOG = LOG + ".") { } LOG = LOG + "|"; }
                f(); LOG;
            )", "...i|"},
                {R"(
                function f() -> void { if (Token("c").name == "c") { LOG = LOG + "body"; } }
                f(); LOG;
            )", "cbody"},
                {R"(
                function f() -> void { int k = 0; while (Token(to_string(k)).name != "2") { LOG = LOG + "."; k++; } LOG = LOG + "|"; }
                f(); LOG;
            )", "0.1.2|"},
                {R"(
                function f(int k) -> void { switch (k) { case 1: { Token t = Token("c"); LOG = LOG + "."; break; } default: LOG = LOG + "d"; } LOG = LOG + "|"; }
                f(1); f(2); LOG;
            )", ".c|d|"},
                {R"(
                function f() -> void { { shared_ptr<Token> h = new Token("h"); LOG = LOG + "."; } LOG = LOG + "|"; }
                f(); LOG;
            )", ".h|"},
                {R"(
                function f() -> void { { var v = Token("v"); LOG = LOG + "."; } LOG = LOG + "|"; }
                f(); LOG;
            )", ".v|"},
                {R"(
                function f() -> void { shared_ptr<Token> keep = null; { shared_ptr<Token> h = new Token("h"); keep = h; } LOG = LOG + "|"; }
                f(); LOG;
            )", "|h"},
            };
            check_each(cases, [&](const auto& c) { expect_log(c.first, c.second); });
        });

        // C++ [except.ctor]: a throw out of a constructor destroys every fully constructed subobject in reverse order; the
        // throwing level's own destructor body never runs, its members (all constructed before its body) do die
        test("a_throw_out_of_construction_destroys_exactly_the_constructed_subobjects", [this]() {
            const std::pair<std::string, std::string> cases[] = {
                {R"(
                class B { B() { throw "boom"; } ~B() { LOG = LOG + "~B"; } }
                class D : B { int x = 1; ~D() { LOG = LOG + "~D"; } }
                class E : B { E() {} ~E() { LOG = LOG + "~E"; } }
                function f() -> void { try { D d; } catch (x) { LOG = LOG + "!"; } try { E e; } catch (x) { LOG = LOG + "?"; } }
                f(); LOG;
            )", "!?"},
                {R"(
                int note(string s) { LOG = LOG + s; return 1; }
                int thrower() { throw "t"; return 0; }
                class A { A(int v) { note("A"); } ~A() { note("~A"); } }
                class G : A { int k = note("k"); G() : super(thrower()) { note("G"); } ~G() { note("~G"); } }
                class B { B() { throw "b"; } }
                class E : B { int k = note("k"); E() { note("E"); } ~E() { note("~E"); } }
                function f() -> void { try { G g; } catch (x) { LOG = LOG + "|" + x; } try { E e; } catch (x) { LOG = LOG + "|" + x; } }
                f(); LOG;
            )", "|t|b"},
                {R"(
                class B { B() { LOG = LOG + "B"; } ~B() { LOG = LOG + "~B"; } }
                class D : B { D() { LOG = LOG + "D"; throw "x"; } ~D() { LOG = LOG + "~D"; } }
                function f() -> void { try { D d; } catch (e) { LOG = LOG + "!"; } }
                f(); LOG;
            )", "BD~B!"},
                {R"(
                class B { B() {} ~B() { LOG = LOG + "~B"; } }
                class D : B { Token t = Token("t"); Token u = Token("u"); D() { throw "x"; } ~D() { LOG = LOG + "~D"; } }
                function f() -> void { try { D d; } catch (e) { LOG = LOG + "!"; } }
                f(); LOG;
            )", "ut~B!"},
                {R"(
                class B { ~B() { LOG = LOG + "~B"; } }
                class D : B { D() { throw "x"; } ~D() { LOG = LOG + "~D"; } }
                function f() -> void { try { D d; } catch (e) { LOG = LOG + "!"; } }
                f(); LOG;
            )", "~B!"},
                {R"(
                class A { A() { LOG = LOG + "A"; } ~A() { LOG = LOG + "~A"; } }
                class B : A { B() { LOG = LOG + "B"; throw "x"; } ~B() { LOG = LOG + "~B"; } }
                class C : B { C() { LOG = LOG + "C"; } ~C() { LOG = LOG + "~C"; } }
                function f() -> void { try { C c; } catch (e) { LOG = LOG + "!"; } }
                f(); LOG;
            )", "AB~A!"},
                {R"(
                int boom() { throw "f"; return 0; }
                class B { B() { LOG = LOG + "B"; } ~B() { LOG = LOG + "~B"; } }
                class D : B { Token t = Token("t"); int k = boom(); D() { LOG = LOG + "D"; } ~D() { LOG = LOG + "~D"; } }
                class E : B { Token t = Token("e"); int k = boom(); ~E() { LOG = LOG + "~E"; } }
                function f() -> void { try { D d; } catch (e) { LOG = LOG + "!"; } try { E e; } catch (e) { LOG = LOG + "?"; } }
                f(); LOG;
            )", "Bt~B!Be~B?"},
                {R"(
                class B { B() { LOG = LOG + "B"; } ~B() { LOG = LOG + "~B"; } }
                class C : B { C(int v) { LOG = LOG + "T"; } C() : this(1) { LOG = LOG + "D"; throw "x"; } ~C() { LOG = LOG + "~C"; } }
                function f() -> void { try { C c; } catch (e) { LOG = LOG + "!"; } }
                f(); LOG;
            )", "BTD~C~B!"},
                {R"(
                class M { M() { LOG = LOG + "M"; } ~M() { LOG = LOG + "~M"; } }
                class B { B() { LOG = LOG + "B"; throw "b"; } ~B() { LOG = LOG + "~B"; } }
                class D : B { M m; D() { LOG = LOG + "D"; } ~D() { LOG = LOG + "~D"; } }
                function f() -> void { try { D d; } catch (e) { LOG = LOG + "!"; } }
                f(); LOG;
            )", "B!"},
                {R"(
                class M { string tag = ""; M(string t) { tag = t; LOG = LOG + t; } ~M() { LOG = LOG + "~" + tag; } }
                class Bad { Bad() { LOG = LOG + "X"; throw "m"; } ~Bad() { LOG = LOG + "~X"; } }
                class A { M ma = M("a"); A() { LOG = LOG + "A"; } ~A() { LOG = LOG + "~A"; } }
                class T : A { M mt = M("t"); Bad bad; M after = M("z"); T() { LOG = LOG + "T"; } ~T() { LOG = LOG + "~T"; } }
                function f() -> void { try { T t; } catch (e) { LOG = LOG + "!"; } }
                f(); LOG;
            )", "aAtX~t~A~a!"},
            };
            check_each(cases, [&](const auto& c) { expect_log(c.first, c.second); });
        });

        // C++ [class.base.init]: bases first, then members in declaration order (a member without an initializer is
        // default-constructed at its turn), then the body; destruction reverses it
        test("bases_then_members_then_the_body_at_every_level", [this]() {
            const std::string members = R"(
                class M { string tag = ""; M() { LOG = LOG + "m"; } M(string t) { tag = t; LOG = LOG + t; } ~M() { LOG = LOG + "~" + tag; } }
                class A { M ma = M("a"); A() { LOG = LOG + "A"; } ~A() { LOG = LOG + "~A"; } }
                class B : A { M mb = M("b"); M plain; B() { LOG = LOG + "B"; } ~B() { LOG = LOG + "~B"; } }
                class C : B { M mc = M("c"); C() { LOG = LOG + "C"; } ~C() { LOG = LOG + "~C"; } }
            )";
            expect_log(members + "function f() -> void { A a; LOG = LOG + \"|\"; } f(); LOG;", "aA|~A~a");
            expect_log(members + "function f() -> void { B b; LOG = LOG + \"|\"; } f(); LOG;", "aAbmB|~B~~b~A~a");
            expect_log(members + "function f() -> void { C c; LOG = LOG + \"|\"; } f(); LOG;", "aAbmBcC|~C~c~B~~b~A~a");
        });

        test("a_throwing_hybrid_body_destroys_its_cpp_base_once", [this]() {
            struct counted_base { std::shared_ptr<int> deaths; ~counted_base() { if (deaths) { ++*deaths; } } };
            auto deaths = std::make_shared<int>(0);
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            dynamic_binder<counted_base>(*eng, "CountedBase")
                .constructor<>()
                .method("arm", [deaths](counted_base& self) { self.deaths = deaths; })
                .build();
            std::string got;
            try {
                got = eng->execute(R"(
                    string LOG = "";
                    class D : CountedBase { D() : super() { arm(); LOG = LOG + "D"; throw "x"; } ~D() { LOG = LOG + "~D"; } }
                    function f() -> void { try { D d; } catch (e) { LOG = LOG + "!"; } }
                    f(); LOG;
                )").as_string();
            } catch (const std::exception& e) {
                got = std::string("threw: ") + e.what();
            }
            check_eq(std::string("D!"), got, "the script level's destructor body never runs");
            check_eq(1, *deaths, "the C++ base dies once");
        });

        test("a_prvalue_argument_is_the_parameter_of_whatever_it_calls", [this]() {
            const std::string h = R"(class H { int k = 0; H(Token p) { LOG = LOG + "body"; } } )";
            const std::string m = R"(class M { void take(Token p) { LOG = LOG + "body"; } } )";
            const std::string r = R"(class R { int k = 0; R(Token& r, Token p) { LOG = LOG + "body"; } } )";
            const std::string t = R"(class T { int k = 0; T(Token p) { LOG = LOG + "body"; throw 1; } } )";
            const std::pair<std::string, std::string> cases[] = {
                {R"(
                function args(Token p, Token q) -> void { LOG = LOG + "body"; }
                args(Token("x"), Token("y")); LOG;
            )", "bodyyx"},
                {h + R"(function f() -> void { H x = H(Token("p")); LOG = LOG + "|"; } f(); LOG;)", "bodyp|"},
                {h + R"({ H x = H(Token("p")); LOG = LOG + "|"; } LOG;)", "bodyp|"},
                {h + R"(H G = H(Token("p")); LOG = LOG + "|"; LOG;)", "bodyp|"},
                {R"(
                class H2 { int k = 0; H2(Token a, Token b) { LOG = LOG + "body"; } }
                function f() -> void { H2 x = H2(Token("a"), Token("b")); LOG = LOG + "|"; } f(); LOG;
            )", "bodyba|"},
                {h + R"({ shared_ptr<H> x = new H(Token("p")); LOG = LOG + "|"; } LOG;)", "bodyp|"},
                {h + R"(function f() -> void { shared_ptr<H> x = new H(Token("p")); LOG = LOG + "|"; } f(); LOG = LOG + "!"; LOG;)", "bodyp|!"},
                {h + R"(function f() -> shared_ptr<H> { return new H(Token("p")); } shared_ptr<H> x = f(); LOG = LOG + "|"; LOG;)", "bodyp|"},
                {h + R"(
                function f() -> void { for (int i = 0; i < 3; i++) { shared_ptr<H> x = new H(Token(to_string(i))); LOG = LOG + "."; } LOG = LOG + "|"; }
                f(); LOG;
            )", "body0.body1.body2.|"},
                {R"(
                class H2 { int k = 0; H2(Token a, Token b) { LOG = LOG + "body"; } }
                function f() -> void { shared_ptr<H2> x = new H2(Token("a"), Token("b")); LOG = LOG + "|"; } f(); LOG;
            )", "bodyba|"},
                {m + R"(M G; G.take(Token("p")); LOG = LOG + "|"; LOG;)", "bodyp|"},
                {m + R"(function f() -> void { M m; m.take(Token("p")); LOG = LOG + "|"; } f(); LOG;)", "bodyp|"},
                {m + R"(function f() -> void { shared_ptr<M> m = new M(); m.take(Token("p")); LOG = LOG + "|"; } f(); LOG;)", "bodyp|"},
                {m + R"(function f() -> void { M().take(Token("p")); LOG = LOG + "|"; } f(); LOG;)", "bodyp|"},
                {R"(class S { static void take(Token p) { LOG = LOG + "body"; } } S::take(Token("p")); LOG = LOG + "|"; LOG;)", "bodyp|"},
                {R"(auto lf = [](Token p) { LOG = LOG + "body"; }; lf(Token("p")); LOG = LOG + "|"; LOG;)", "bodyp|"},
                {h + R"(
                function g(H x) -> void { LOG = LOG + "g"; }
                function f() -> void { g(H(Token("p"))); LOG = LOG + "|"; } f(); LOG;
            )", "bodypg|"},
                {h + R"(
                function g(shared_ptr<H> x, Token q) -> void { LOG = LOG + "g"; }
                function f() -> void { g(new H(Token("p")), Token("q")); LOG = LOG + "|"; } f(); LOG;
            )", "bodypgq|"},
                {h + R"(
                class O { int k = 0; O(H a) { LOG = LOG + "o"; } }
                function f() -> void { O o = O(H(Token("p"))); LOG = LOG + "|"; shared_ptr<O> n = new O(H(Token("q"))); LOG = LOG + "|"; } f(); LOG;
            )", "bodypo|bodyqo|"},
                {r + R"(function f() -> void { Token t = Token("t"); R x = R(t, Token("p")); LOG = LOG + "|"; } f(); LOG;)", "bodyp|t"},
                {r + R"(function f() -> void { Token t = Token("t"); shared_ptr<R> x = new R(t, Token("p")); LOG = LOG + "|"; } f(); LOG;)", "bodyp|t"},
                {r + R"(function f() -> void { Token t = Token("t"); Token u = Token("u"); R x = R(t, u); LOG = LOG + "|"; } f(); LOG;)", "bodyu|ut"},
                {R"(
                class B { int b = 0; B(Token p) { LOG = LOG + "base"; } }
                class D : B { int d = 0; D(Token q) : super(Token("p")) { LOG = LOG + "body"; } }
                function f() -> void { D x = D(Token("q")); LOG = LOG + "|"; shared_ptr<D> y = new D(Token("r")); LOG = LOG + "|"; } f(); LOG;
            )", "basepbodyq|basepbodyr|"},
                {R"(
                class G { int k = 0; G(Token p) { LOG = LOG + "target"; } G(int n) : this(Token("p")) { LOG = LOG + "body"; } }
                function f() -> void { G x = G(1); LOG = LOG + "|"; shared_ptr<G> y = new G(2); LOG = LOG + "|"; } f(); LOG;
            )", "targetpbody|targetpbody|"},
                {t + R"(function f() -> void { try { shared_ptr<T> x = new T(Token("p")); } catch (e) { LOG = LOG + "c"; } LOG = LOG + "|"; } f(); LOG;)", "bodypc|"},
                {t + R"(function f() -> void { try { T x = T(Token("p")); } catch (e) { LOG = LOG + "c"; } LOG = LOG + "|"; } f(); LOG;)", "bodypc|"},
                {R"(
                class H { int k = 0; H(Token p) { LOG = LOG + "body"; } }
                coroutine gen() -> int { shared_ptr<H> h = new H(Token("p")); LOG = LOG + "y"; yield 1; H k = H(Token("q")); LOG = LOG + "z"; yield 2; }
                function f() -> void { var g = gen(); g.next(); LOG = LOG + "|"; g.next(); LOG = LOG + "|"; g.next(); LOG = LOG + "|"; }
                f(); LOG;
            )", "bodypy|bodyqz||"},
            };
            check_each(cases, [&](const auto& c) { expect_log(c.first, c.second); });
        });

        // A converting constructor makes the slot's object once, and a temporary it converts is that constructor's parameter
        // (C++17): a function's, a method's, a constructor's and new's parameter, a declaration, a return and an assignment
        test("a_conversion_makes_one_object_from_its_source", [this]() {
            const std::string l = R"(class L { string k = ""; L(Token t) { k = t.name; LOG = LOG + "L"; } ~L() { LOG = LOG + "~" + k; } } )";
            expect_log(l + R"(
                class M { void take(L l) { LOG = LOG + "m"; } }
                class H { int k = 0; H(L l) { LOG = LOG + "h"; } }
                function take(L l) -> void { LOG = LOG + "f"; }
                function word(Token t) -> void { LOG = LOG + t.name; }
                function f() -> void {
                    take(Token("a")); LOG = LOG + "|";
                    Token v = Token("v"); take(v); LOG = LOG + "|";
                    word("w"); LOG = LOG + "|";
                    M m; m.take(Token("b")); LOG = LOG + "|";
                    H h = H(Token("c")); LOG = LOG + "|";
                    shared_ptr<H> n = new H(Token("d")); LOG = LOG + "|";
                }
                f(); LOG;
            )", "Laf~a|Lvf~v|ww|Lbm~b|Lch~c|Ldh~d|v");
            expect_log(l + R"(
                class Hold { L f = L(Token("i")); void set() { f = Token("u"); } }
                function ret_temp() -> L { return Token("r"); }
                function ret_local() -> L { Token loc = Token("o"); return loc; }
                function f() -> void {
                    L a = Token("d"); LOG = LOG + "|";
                    L b = ret_temp(); LOG = LOG + "|";
                    L c = ret_local(); LOG = LOG + "|";
                    a = Token("q"); LOG = LOG + "|";
                    Hold h; LOG = LOG + "|";
                    h.f = Token("s"); LOG = LOG + "|";
                    h.set(); LOG = LOG + "|";
                }
                f(); LOG;
            )", "Ld|Lr|Lo|Lq~q|Li|Ls~s|Lu~u|~u~o~r~q");
        });

        // A user copy constructor builds every copy C++ copy-constructs; a prvalue initializing a slot and a returned own local are
        // elided; a base's copy constructor builds the base of a class that declares none; a throwing one leaves no object
        test("a_copy_constructor_builds_every_copy", [this]() {
            const std::string c = R"(class C { string n = ""; C(string s) { n = s; } C(const C& o) { if (o.n == "bad") { throw "x"; } n = o.n + "'"; LOG = LOG + "c" + n; } ~C() { LOG = LOG + "~" + n; } } )";
            expect_log(c + R"(
                C G = C("g");
                function take(C v) -> void { LOG = LOG + "t"; }
                function ret_glob() -> C { return G; }
                function ret_param(C p) -> C { return p; }
                function ret_local() -> C { C r = C("r"); return r; }
                function f() -> void {
                    C a = C("a"); C b = a; LOG = LOG + "|";
                    take(a); take(C("p")); LOG = LOG + "|";
                    C r1 = ret_glob(); C r2 = ret_param(a); C r3 = ret_local(); LOG = LOG + "|";
                    array<C> xs = [a]; for (C e : xs) { LOG = LOG + "."; } LOG = LOG + "|";
                    auto l = [a]() { return a.n; }; LOG = LOG + "|";
                    b = a; LOG = LOG + "|";
                }
                f(); LOG;
            )", "ca'|ca't~a't~p|cg'ca'ca''~a'|ca'ca''.~a''|ca'||~a'~a'~r~a''~g'~a~a");
            expect_log(c + R"(
                class B { string bn = ""; B(string s) { bn = s; } B(const B& o) { bn = o.bn + "'"; LOG = LOG + "cB"; } }
                class D : B { C m = C("m"); D(string s) : super(s) {} }
                function f() -> void {
                    D d = D("d"); D e = d; LOG = LOG + "|" + e.bn + e.m.n + "|";
                    C bad = C("bad");
                    try { C copy = bad; } catch (err) { LOG = LOG + "caught"; }
                    array<C> src = [C("p"), C("bad")];
                    try { array<C> cp = src; } catch (err) { LOG = LOG + "|caught"; }
                    LOG = LOG + "|";
                }
                f(); LOG;
            )", "cBcm'|d'm'|caughtcp'~p'|caught|~bad~p~bad~m'~m");
            // a returned parameter moves an array's storage (no element copies) and moves a class memberwise, copying a field
            // whose class declares a copy constructor
            expect_log(c + R"(
                class E { C c = C("e"); }
                function pass(array<C> p) -> array<C> { return p; }
                function passe(E e) -> E { return e; }
                function f() -> void {
                    C a = C("a"); array<C> xs = [a]; LOG = LOG + "|";
                    array<C> r = pass(xs); LOG = LOG + "|" + r[0].n + "|";
                    E e = E(); E e2 = passe(e); LOG = LOG + "|" + e2.c.n + "|";
                }
                f(); LOG;
            )", "ca'|ca''|a''|ce'ce''~e'|e''|~e''~e~a''~a'~a");
            // a class a namespace declares is ns::N: its T(const T&), spelled with the bare name inside, is its copy constructor
            expect_log(R"(
                namespace ns { class N { string n = ""; N(string s) { n = s; } N(const N& o) { n = o.n + "'"; LOG = LOG + "c" + n; } ~N() { LOG = LOG + "~" + n; } } }
                function f() -> void { ns::N a = ns::N("a"); ns::N b = a; LOG = LOG + "|"; }
                f(); LOG;
            )", "ca'|~a'~a");
        });

        test("a_returned_local_moves", [this]() {
            expect_log(R"(
                function make() -> Token { Token t = Token("m"); return t; }
                function f() -> void { Token x = make(); LOG = LOG + "|"; }
                f(); LOG;
            )", "|m");
            // a member read off a call's result moves when the read was its last hold (C++'s xvalue), and copies a member another
            // owner keeps
            expect_log(R"(
                class Box { Token m; Box(string s) { m.name = s; } }
                shared_ptr<Box> G = new Box("g");
                function off_temporary(string s) -> Token { Token loc = Token("l"); return Box(s).m; }
                function shared_box() -> shared_ptr<Box> { return G; }
                function off_kept() -> Token { return shared_box().m; }
                function f() -> void { Token y = off_temporary("x"); LOG = LOG + "|"; Token z = off_kept(); z.name = "c"; LOG = LOG + "|"; }
                f(); LOG = LOG + "|" + G.m.name; LOG;
            )", "l||cx|g");
            // read through the handle's last owner the member is still an lvalue: returned or passed, it is copied
            expect_log(R"(
                class Box { Token m; Box(string s) { m.name = s; } }
                function last(string s) -> shared_ptr<Box> { return new Box(s); }
                function off_last(string s) -> Token { return last(s).m; }
                function take(Token t) -> void { LOG = LOG + "t"; }
                function f() -> void { Token y = off_last("h"); LOG = LOG + "|"; take(last("a").m); LOG = LOG + "|"; }
                f(); LOG;
            )", "h|ata|h");
        });

        test("members_die_after_their_class_body_and_before_the_base", [this]() {
            const std::pair<std::string, std::string> cases[] = {
                {R"(
                class Base { Token bm = Token("bm"); Base() {} ~Base() { LOG = LOG + "~B"; } }
                class Derived : Base { Token d1 = Token("d1"); Token d2 = Token("d2"); Derived() {} ~Derived() { LOG = LOG + "~D"; } }
                function f() -> void { Derived d = Derived(); LOG = ""; }
                f(); LOG;
            )", "~Dd2d1~Bbm"},
                {R"(
                class Base { Token bm = Token("bm"); Base() {} ~Base() { LOG = LOG + "~B"; } }
                class Plain : Base { Token pm = Token("pm"); Plain() {} }
                function f() -> void { Plain p = Plain(); LOG = ""; }
                f(); LOG;
            )", "pm~Bbm"},
            };
            check_each(cases, [&](const auto& c) { expect_log(c.first, c.second); });
        });

        test("program_scope_variables_die_at_teardown_in_reverse", [this]() {
            for (int variant = 0; variant < 2; ++variant) {
                std::string log;
                {
                    auto eng = make_engine();
                    if (variant == 1) { eng->type_proofs(false); eng->ir_passes(false); }
                    eng->add_function("note", [&log](std::string s) { log += s; });
                    eng->execute(R"(
                        class T { string n = ""; T(string s) { n = s; } ~T() { note(n); } }
                        T G1 = T("1");
                        namespace ns { T inner = T("n"); }
                        class S { static T s = T("s"); }
                        T G2 = T("2");
                        auto keep = [c = T("c")]() { return c.n; };
                        array<T> GA;
                        GA.push(T("a"));
                    )");
                    log.clear();
                }
                check_eq(std::string("ac2sn1"), log, "the last constructed dies first: namespace members, statics and captures alike");
            }
            // host objects alone, in a program no script destructor names: their C++ destructors keep the order
            std::string log;
            {
                auto eng = make_engine();
                dynamic_binder<teardown_mark>(*eng, "Mark").build();
                eng->add_function("mark", [&log](std::string s) { return std::make_shared<teardown_mark>(&log, std::move(s)); });
                eng->execute(R"(
                    var G1 = mark("1");
                    namespace ns { var inner = mark("n"); }
                    class S { static var s = mark("s"); }
                    var G2 = mark("2");
                )");
            }
            check_eq(std::string("2sn1"), log, "a host object's C++ destructor runs in reverse construction order without any script destructor");
        });

        test("a_destructor_error_at_teardown_is_reported", [this]() {
            std::string reported;
            bool threw = false;
            try {
                auto eng = make_engine();
                eng->set_script_error_handler([&reported](const std::string& message) { reported += message; });
                eng->execute(R"(
                    class Bad { Bad() {} ~Bad() { throw "teardown"; } }
                    Bad B = Bad();
                )");
            } catch (...) {
                threw = true;
            }
            check_false(threw, "teardown does not propagate");
            check_true(reported.find("teardown") != std::string::npos, "reported: " + reported);
            // so is one a destructor raises while another error unwinds (C++ would terminate): the first error reaches its handler,
            // whether the dying local is the try body's, a callee's or a grafted callee's, and whether a throw or a run-time error unwinds
            for (const char* unwinds : {"boom();", "throw \"t\";", "int z = 1 / (LOG.size() - LOG.size());"}) {
                for (const bool in_callee : {false, true}) {
                    std::string during;
                    auto eng = make_engine();
                    eng->set_script_error_handler([&during](const std::string& message) { during += message; });
                    const std::string body = std::string("Bad b = Bad(\"b\"); ") + unwinds;
                    const std::string program = std::string(R"(
                        string LOG = "";
                        class Bad { string name = ""; Bad(string n) { name = n; } ~Bad() { LOG = LOG + name; throw "d" + name; } }
                        function boom() -> int { throw "boom"; return 0; }
                    )") + (in_callee ? "function g() -> void { " + body + " } function f() -> void { try { g(); }" : "function f() -> void { try { " + body + " }") +
                        R"( catch (e) { LOG = LOG + "|c"; } LOG = LOG + "|after"; }
                        try { f(); } catch (e) { LOG = LOG + "|outer:" + e.message; }
                        LOG;
                    )";
                    check_eq(std::string("b|c|after"), eng->execute(program).as_string(), program);
                    check_true(during.find("db") != std::string::npos, "reported while unwinding: " + during);
                }
            }
        });

        // A destructor that runs while an expression releases a temporary leaves nothing of its own body on the value stack
        test("a_destructor_inside_an_expression_keeps_the_expression_value", [this]() {
            expect_log(R"(
                function a2() -> bool { string s = Token("a").name; return s == "a"; }
                function a5() -> int { return Token("abc").name.size(); }
                bool ok = a2(); int n = a5();
                LOG = LOG + "|" + to_string(ok) + to_string(n); LOG;
            )", "aabc|true3");
        });

        test("an_abandoned_coroutine_destroys_its_locals", [this]() {
            expect_log(R"(
                coroutine gen() -> int { Token a = Token("a"); yield 1; yield 2; }
                function f() -> void { var g = gen(); g.next(); LOG = LOG + "|"; g = null; LOG = LOG + "|"; }
                f(); LOG;
            )", "|a|");
            // suspended inside a handler: the handler's locals, then the thrown object it holds, then the body's locals
            expect_log(R"(
                coroutine gen() -> int { Token a = Token("a"); try { throw Token("t"); } catch (e) { Token h = Token("h"); yield 1; } yield 2; }
                function f() -> void { var g = gen(); g.next(); LOG = LOG + "|"; g = null; LOG = LOG + "|"; }
                f(); LOG;
            )", "|hta|");
            // a suspended range-for ends as a return would end it: its interior, its range, then the locals ahead of it
            expect_log(R"(
                function arr() -> array<Token> { array<Token> r = [Token("x"), Token("y")]; return r; }
                coroutine gen() -> int { Token a = Token("a"); for (auto t : arr()) { Token b = Token("b"); yield 1; } }
                function f() -> void { var g = gen(); g.next(); LOG = LOG + "|"; g = null; LOG = LOG + "|"; }
                f(); LOG;
            )", "|bxyxa|");
            // a by-value parameter is one object: the argument the call made moves into it at the first resume
            expect_log(R"(
                coroutine gen(Token p) -> int { Token a = Token("a"); Token b = Token("b"); yield 1; yield 2; }
                function f() -> void { var g = gen(Token("p")); LOG = LOG + "<"; g.next(); LOG = LOG + "|"; g = null; LOG = LOG + "|"; }
                f(); LOG;
            )", "p<|bap|");
        });

        // C++ destroys the exception object as its handler exits; a rethrow carries it to the next handler
        test("a_thrown_object_dies_as_its_handler_exits", [this]() {
            expect_log(R"(
                function f() -> void {
                    try { throw Token("e"); } catch (e) { Token k = e.value; k.name = "k"; LOG = LOG + "h"; }
                    LOG = LOG + "|";
                    for (int i = 0; i < 2; i++) { try { throw Token(to_string(i)); } catch (e) { if (i == 0) { continue; } LOG = LOG + "h"; } }
                    LOG = LOG + "|";
                    try { try { throw Token("n"); } catch (e) { throw "new"; } } catch (e) { LOG = LOG + "c"; }
                    LOG = LOG + "|";
                    try { try { throw Token("r"); } catch (e) { LOG = LOG + "i"; throw; } } catch (e) { LOG = LOG + "o"; }
                    LOG = LOG + "|";
                }
                f(); LOG;
            )", "hke|0h1|nc|ior|");
            // a handler's locals die ahead of the thrown object it holds, when the handler returns or breaks too
            expect_log(R"(
                function r() -> int { try { throw Token("t"); } catch (e) { Token l = Token("l"); return 1; } return 0; }
                function f() -> void { r(); for (int i = 0; i < 2; i++) { try { throw Token("u"); } catch (e) { Token m = Token("m"); break; } } }
                f(); LOG;
            )", "ltmu");
            // at program scope too, where the handler's scopes are environments: a break, a continue from a nested block, a new
            // throw and a rethrow each close the handler's scopes, then the thrown object dies
            expect_log(R"(
                for (int i = 0; i < 1; i++) { try { throw Token("a"); } catch (e) { Token h = Token("h"); break; } }
                LOG = LOG + "|";
                for (int i = 0; i < 2; i++) { try { throw Token(to_string(i)); } catch (e) { Token h = Token("h"); { Token k = Token("k"); continue; } } }
                LOG = LOG + "|";
                try { try { throw Token("n"); } catch (e) { Token h = Token("h"); throw "new"; } } catch (e) { LOG = LOG + "c"; }
                LOG = LOG + "|";
                try { try { throw Token("r"); } catch (e) { throw; } } catch (e) { LOG = LOG + "o"; }
                LOG;
            )", "ha|kh0kh1|hnc|or");
        });

        test("a_coroutine_keeps_its_locals_across_yields", [this]() {
            expect_log(R"(
                coroutine gen() -> int { Token a = Token("a"); yield 1; LOG = LOG + "r"; yield 2; }
                function f() -> void { var g = gen(); g.next(); LOG = LOG + "|"; g.next(); LOG = LOG + "|"; g.next(); LOG = LOG + "|"; }
                f(); LOG;
            )", "|r|a|");
        });

        // Assignment into a slot holding a class instance by value is C++'s copy assignment: the
        // slot keeps its object, the declared class's members take the source's, then a temporary source dies
        test("assignment_copy_assigns_and_the_temporary_dies", [this]() {
            expect_log(R"(
                function f() -> void { Token x = Token("a"); x = Token("b"); LOG = LOG + "|"; }
                f(); LOG;
            )", "b|b");
            expect_log(R"(
                function f() -> void { Token x = Token("a"); Token y = Token("c"); x = y; LOG = LOG + "|"; }
                f(); LOG;
            )", "|cc");
            expect_log(R"(
                function f() -> void { Token x = Token("a"); Token y = (x = Token("b")); LOG = LOG + to_string(same_object(x, y)) + "|"; }
                f(); LOG;
            )", "bfalse|bb");
            // a field a method or constructor body names, on every run of the site
            expect_log(R"(
                class W { Token d = Token("i"); W(string s) { d = Token(s); } function set(string s) -> void { d = Token(s); } }
                function f() -> void {
                    for (int i = 0; i < 2; i++) { W k = W("k" + to_string(i)); LOG = LOG + "|"; }
                    W w = W("w"); w.set("a"); LOG = LOG + "|"; w.set("b"); LOG = LOG + "|"; w.set("c"); LOG = LOG + "|";
                }
                f(); LOG;
            )", "k0|k0k1|k1wa|b|c|c");
        });

        test("assignment_keeps_the_object_references_and_handles_name", [this]() {
            expect_log(R"(
                function f() -> string { Token x = Token("a"); Token& r = x; shared_ptr<Token> h = x; x = Token("b"); return r.name + h.name + to_string(same_object(h, x)); }
                string out = f(); out + ":" + LOG;
            )", "bbtrue:bb");
            expect_log(R"(
                function f() -> string { Token x = Token("a"); Token& r = x; r = Token("r"); LOG = LOG + "|"; return x.name; }
                string out = f(); out + ":" + LOG;
            )", "r:r|r");
        });

        // No slicing: a source of another class the slot admits re-classes the slot's object in place:
        // the levels it loses die (destructor body, then their members), it takes the source's class, every member assigns
        // from the source; references, handles and same_object keep seeing the one object
        test("assignment_from_another_admitted_class_reclasses_the_object_in_place", [this]() {
            const std::pair<std::string, std::string> cases[] = {
                {std::string(k_family) + R"(
                function f() -> string { Base x = Base(); shared_ptr<Base> h = x; D2 s = D2(); s.a = 5; s.d2 = 6; x = s; LOG = LOG + "|";
                    return to_string(x.a) + type_of(h) + to_string(same_object(h, x)) + to_string(cast<D2&>(x).d2); }
                string out = f(); out + ":" + LOG;
            )", "5D2true6:|2B2B"},
                {std::string(k_family) + R"(
                function f() -> string { Base x = D1(); shared_ptr<Base> h = x; D2 s = D2(); s.a = 7; LOG = LOG + "|"; x = s; LOG = LOG + "|";
                    return to_string(x.a) + to_string(cast<D2&>(h).d2) + to_string(same_object(h, x)); }
                string out = f(); out + ":" + LOG;
            )", "73true:|1t|2B2B"},
                {std::string(k_family) + R"(
                function f() -> string { Base x = D1(); shared_ptr<Base> h = x; Base b = Base(); b.a = 9; LOG = LOG + "|"; x = b; LOG = LOG + "|";
                    string r = to_string(x.a) + type_of(h) + to_string(same_object(h, x));
                    try { int k = cast<D1&>(x).d1; r = r + "no"; } catch (e) { r = r + "!"; }
                    return r; }
                string out = f(); out + ":" + LOG;
            )", "9Basetrue!:|1t|BB"},
                {R"(
                class P { int a = 0; P() {} }
                class Q : P { Q() {} ~Q() { LOG = LOG + "Q"; } }
                function f() -> void { P x = P(); x = Q(); LOG = LOG + "|"; }
                f(); LOG;
            )", "Q|Q"},
                {R"(
                class P { int a = 0; P() {} ~P() { LOG = LOG + "P"; } }
                class Q : P { Q() {} ~Q() { LOG = LOG + "Q"; } }
                function f() -> void { P x = Q(); x = P(); LOG = LOG + "|"; }
                f(); LOG;
            )", "QP|P"},
            };
            check_each(cases, [&](const auto& c) { expect_log(c.first, c.second); });
        });

        test("a_reference_to_a_dropped_member_raises", [this]() {
            expect_log(std::string(k_family) + R"(
                function f() -> string { Base x = D1(); D1& dx = cast<D1&>(x); int& r = dx.d1; Token& t = dx.t1; x = Base(); string out = "";
                    try { int k = r; out = out + "read"; } catch (e) { out = out + e; }
                    out = out + "/";
                    try { r = 4; out = out + "write"; } catch (e) { out = out + e; }
                    out = out + "/";
                    try { string n = t.name; out = out + "member"; } catch (e) { out = out + e; }
                    LOG = "";
                    return out; }
                f();
            )", "Reference to a removed field/Reference to a removed field/Reference to a removed field");
            expect_log(std::string(k_family) + R"(
                function f() -> string { Base x = D1(); D1& dx = cast<D1&>(x); int& r = dx.d1; string out = "";
                    for (int i = 0; i < 3; i++) { x = D2(); D1 back = D1(); back.d1 = 10 + i; x = back; out = out + to_string(r); }
                    LOG = "";
                    return out; }
                f();
            )", "101112");
        });

        test("an_unrelated_source_still_raises", [this]() {
            expect_log(std::string(k_family) + R"(
                [[warn_mode]]
                class Other { int a = 1; Other() {} }
                function f() -> string { Base x = Base(); shared_ptr<Base> h = x; var o = Other(); string r = "";
                    try { x = o; r = "no"; } catch (e) { r = e; }
                    return r + "|" + type_of(h) + to_string(same_object(h, x)); }
                string out = f(); LOG = ""; out;
            )", "Cannot assign Other to 'x' (declared Base)|Basetrue");
        });

        test("every_by_value_slot_reclasses_in_place", [this]() {
            const std::string family = std::string(k_family);
            expect_log(family + R"(
                function f() -> string { array<Base> a; a.push(Base()); shared_ptr<Base> h = a[0]; a[0] = D2(); return type_of(a[0]) + to_string(same_object(h, a[0])) + to_string(cast<D2&>(h).d2); }
                string out = f(); LOG = ""; out;
            )", "D2true3");
            expect_log(family + R"(
                class Holder { Base b; }
                function f() -> string { Holder o; shared_ptr<Base> h = o.b; o.b = D2(); return type_of(h) + to_string(same_object(h, o.b)) + to_string(cast<D2&>(o.b).d2); }
                string out = f(); LOG = ""; out;
            )", "D2true3");
            expect_log(family + R"(
                class S { static Base st; }
                function f() -> string { shared_ptr<Base> h = S::st; S::st = D2(); return type_of(h) + to_string(same_object(h, S::st)) + to_string(cast<D2&>(S::st).d2); }
                string out = f(); LOG = ""; out;
            )", "D2true3");
            expect_log(family + R"(
                function f() -> string { map<string, Base> m; m["k"] = Base(); shared_ptr<Base> h = m["k"]; m["k"] = D2(); return type_of(h) + to_string(same_object(h, m["k"])) + to_string(cast<D2&>(m["k"]).d2); }
                string out = f(); LOG = ""; out;
            )", "D2true3");
            expect_log(family + R"(
                function put(Base& p) -> void { p = D2(); }
                function f() -> string { Base x = Base(); shared_ptr<Base> h = x; Base& r = x; r = D1(); string first = type_of(h); put(x);
                    return first + type_of(h) + to_string(same_object(h, x)); }
                string out = f(); LOG = ""; out;
            )", "D1D2true");
            expect_log(family + R"(
                class Acc { int n = 0; Acc() {} Acc(int v) { n = v; } function operator+(Acc o) -> Acc { return Sum(n + o.n); } }
                class Sum : Acc { int parts = 2; Sum(int v) { n = v; } }
                function f() -> string { Acc x = Acc(1); shared_ptr<Acc> h = x; Acc two = Acc(2); x += two; return type_of(h) + to_string(h.n) + to_string(same_object(h, x)) + to_string(cast<Sum&>(x).parts); }
                string out = f(); LOG = ""; out;
            )", "Sum3true2");
        });

        test("a_reference_declared_narrower_than_its_storage_keeps_its_class", [this]() {
            expect_log(std::string(k_family) + R"(
                [[warn_mode]]
                function f() -> string { Base x = D1(); D1& r = cast<D1&>(x); string out = "";
                    try { var s = D2(); r = s; out = "no"; } catch (e) { out = e; }
                    return out + "|" + type_of(x); }
                string out = f(); LOG = ""; out;
            )", "Cannot assign D2 to 'r' (declared D1&)|D1");
        });

        test("field_element_map_and_static_stores_copy_assign", [this]() {
            expect_log(R"(
                class Holder { Token t; }
                function f() -> string { Holder o; shared_ptr<Token> h = o.t; o.t = Token("n"); LOG = LOG + "|"; return h.name + to_string(same_object(h, o.t)); }
                string out = f(); out + ":" + LOG;
            )", "ntrue:n|n");
            expect_log(R"(
                function f() -> string { array<Token> a; Token p = Token("p"); a.push(p); shared_ptr<Token> h = a[0]; a[0] = Token("q"); LOG = LOG + "|"; return h.name + to_string(same_object(h, a[0])); }
                string out = f(); out + ":" + LOG;
            )", "qtrue:q|pq");
            expect_log(R"(
                function f() -> string { map<string, Token> m; m["k"] = Token("m"); LOG = LOG + "/"; shared_ptr<Token> h = m["k"]; m["k"] = Token("n"); LOG = LOG + "|"; return h.name + to_string(same_object(h, m["k"])); }
                string out = f(); out + ":" + LOG;
            )", "ntrue:/n|n");
            expect_log(R"(
                class S { static Token st; }
                function f() -> string { shared_ptr<Token> h = S::st; S::st = Token("s"); LOG = LOG + "|"; return h.name + to_string(same_object(h, S::st)); }
                string out = f(); out + ":" + LOG;
            )", "strue:s|");
        });

        test("a_fresh_element_takes_the_temporary_and_a_pushed_one_copies", [this]() {
            expect_log(R"(
                function f() -> void { map<string, Token> m; m["k"] = Token("m"); LOG = LOG + "|"; }
                f(); LOG;
            )", "|m");
            expect_log(R"(
                function f() -> void { array<Token> a; a.push(Token("p")); LOG = LOG + "/"; a[0] = Token("q"); LOG = LOG + "|"; }
                f(); LOG;
            )", "p/q|q");
        });

        test("a_compound_assignment_copy_assigns_the_operator_result", [this]() {
            expect_log(R"(
                class Acc { int n = 0; Acc(int v) { n = v; } ~Acc() { LOG = LOG + to_string(n); } function operator+(Acc o) -> Acc { return Acc(n + o.n); } }
                function f() -> string { Acc x = Acc(1); shared_ptr<Acc> h = x; Acc two = Acc(2); x += two; LOG = LOG + "|"; return to_string(h.n) + to_string(same_object(h, x)); }
                string out = f(); out + ":" + LOG;
            )", "3true:23|23");
        });

        test("a_var_slot_replaces_its_value", [this]() {
            expect_log(R"(
                function f() -> string { var v = Token("a"); shared_ptr<Token> h = v; v = Token("b"); LOG = LOG + "|"; return h.name + v.name; }
                string out = f(); out + ":" + LOG;
            )", "ab:|ab");
        });

        test("a_hot_reloaded_instance_copy_assigns_with_the_new_layout", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(std::string(k_prelude) + R"(
                Token held = Token("x");
                shared_ptr<Token> alias = held;
            )");
            const std::string got = eng->execute(R"(
                class Token { string name = ""; int hp = 7; Token() {} Token(string n) { name = n; } ~Token() { LOG = LOG + name; } }
                LOG = "";
                Token next = Token("y");
                next.hp = 9;
                held = next;
                alias.name + to_string(alias.hp) + to_string(same_object(alias, held)) + ":" + LOG;
            )").as_string();
            check_eq(std::string("y9true:"), got);
        });

        test("a_reclassed_instance_migrates_with_its_new_class", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(std::string(k_prelude) + k_family + R"(
                Base held = Base();
                shared_ptr<Base> alias = held;
                held = D2();
            )");
            const std::string got = eng->execute(R"(
                class D2 : Base { int d2 = 3; int extra = 5; D2() {} ~D2() { LOG = LOG + "2"; } }
                LOG = "";
                type_of(alias) + to_string(cast<D2&>(held).extra) + to_string(same_object(alias, held)) + ":" + LOG;
            )").as_string();
            check_eq(std::string("D25true:"), got);
        });

        test("a_literal_element_copied_before_a_later_call_is_copied_once", [this]() {
            expect_log(R"(
                class Box { Token t; }
                function one() -> int { return 1; }
                function f() -> void {
                    var q = Box(); q.t = Token("q");
                    LOG = LOG + "1";
                    var arr = [q, Token("a")];
                    LOG = LOG + "2";
                    var m = {"k": q, "j": one()};
                    LOG = LOG + "3";
                    arr = [one(), q, one()];
                    LOG = LOG + "4";
                }
                f(); LOG;
            )", "q123aq4qqq");
        });

        test("push_takes_a_temporary_only_where_no_destructor_sees_it", [this]() {
            expect_log(R"(
                class Plain { int v = 0; array<int> xs; }
                class Holder { Token t; Holder(string n) { t = Token(n); } }
                function make(int v) -> Plain { Plain p; p.v = v; return p; }
                function f() -> string {
                    array<Plain> a; Plain p; p.v = 3; a.push(p); p.v = 4; a.push(make(7)); a[1].xs.push(5);
                    var b = []; b.push(make(8)); b[0].v = 9;
                    array<Holder> h; h.push(Holder("h"));
                    LOG = LOG + "|";
                    return to_string(a[0].v) + to_string(p.v) + to_string(a[1].v) + to_string(a[1].xs.size()) + to_string(b[0].v);
                }
                LOG = LOG + f() + LOG; LOG;
            )", "34719hh|h");
        });

        test("long_chains_release_without_native_recursion", [this]() {
            for (const char* drop : {"function f(int n) -> int { shared_ptr<Node> head = build(n); return head.x; } f(N);",
                                     "shared_ptr<Node> head = build(N); head = null;",
                                     "var v = build(N); v = 0;",
                                     "shared_ptr<Holder> t = null; for (int i = 0; i < N; i++) { shared_ptr<Holder> h = new Holder(); h.items = [t, {\"k\": i}]; t = h; } t = null;",
                                     "shared_ptr<Holder> t = null; for (int i = 0; i < N; i++) { shared_ptr<Holder> h = new Holder(); h.items = {\"n\": t}; t = h; } t = null;"}) {
                for (int n : {10000, 100000}) {
                    auto eng = make_engine();
                    jai::stdlib::register_all(eng);
                    eng->execution_budget(0);
                    std::string got;
                    try {
                        got = eng->execute("int N = " + std::to_string(n) + R"(;
                            class Node { int x = 0; shared_ptr<Node> next = null; }
                            class Holder { var items = null; }
                            function build(int n) -> shared_ptr<Node> {
                                shared_ptr<Node> head = null;
                                for (int i = 0; i < n; i++) { shared_ptr<Node> c = new Node(); c.x = i; c.next = head; head = c; }
                                return head;
                            }
                        )" + std::string(drop) + " \"ok\";").as_string();
                    } catch (const std::exception& e) {
                        got = e.what();
                    }
                    check_eq(std::string("ok"), got, std::string(drop) + " n=" + std::to_string(n));
                }
            }
        });

        test("nested_containers_release_without_native_recursion", [this]() {
            auto eng = make_engine();
            jai::script_value nested = jai::script_value::make_array(nullptr, eng.get());
            for (int i = 0; i < 1000000; ++i) {
                jai::script_value outer = jai::script_value::make_array(nullptr, eng.get());
                outer.get_array_storage()->values().push_back(jai::script_value(static_cast<jai::script_int>(i), eng.get()));
                outer.get_array_storage()->values().push_back(std::move(nested));
                nested = std::move(outer);
            }
            nested = jai::script_value(std::monostate{}, eng.get());
            jai::script_value maps = jai::script_value::make_map(nullptr, nullptr, eng.get());
            for (int i = 0; i < 1000000; ++i) {
                jai::script_value outer = jai::script_value::make_map(nullptr, nullptr, eng.get());
                outer.get_map_storage()->insert_or_assign(jai::script_value("next", eng.get()), std::move(maps));
                maps = std::move(outer);
            }
            maps = jai::script_value(std::monostate{}, eng.get());
            check(true);
        });

        test("a_long_chain_at_teardown_releases_without_native_recursion", [this]() {
            for (const char* program : {"shared_ptr<Node> head = build(100000);",
                                        "class Holder { static shared_ptr<Node> head = null; } Holder::head = build(100000);",
                                        "class Holder { static var bag = null; } Holder::bag = [build(50000), {\"k\": build(50000)}];",
                                        "class Tail { ~Tail() {} } shared_ptr<Node> head = build(100000); Tail t;"}) {
                auto eng = make_engine();
                eng->execution_budget(0);
                eng->execute(std::string(R"(
                    class Node { int x = 0; shared_ptr<Node> next = null; }
                    function build(int n) -> shared_ptr<Node> {
                        shared_ptr<Node> head = null;
                        for (int i = 0; i < n; i++) { shared_ptr<Node> c = new Node(); c.x = i; c.next = head; head = c; }
                        return head;
                    }
                )") + program);
                eng.reset();
                check(true);
            }
        });

        // A destructor class's instance shares one allocation with its control block: a weak handle held past its death
        // keeps that memory, reads expired, and a death the release stack defers still runs its body over live fields
        test("weak_handles_outlive_a_destructor_class_death_at_every_limit", [this]() {
            const std::string program = R"(
                class P { int id = 0; array<int> xs = [1, 2, 3]; shared_ptr<P> next = null; ~P() { LOG = LOG + "d" + to_string(id) + ":" + to_string(xs.size()); } }
                array<weak_ptr<P>> KEEP = [];
                function run() -> void {
                    shared_ptr<P> head = null;
                    for (int i = 0; i < 6; i++) { shared_ptr<P> n = new P(); n.id = i; n.next = head; head = n; weak_ptr<P> w = n; KEEP.push(w); }
                    head = null;
                }
                run();
                int alive = 0;
                for (auto& w : KEEP) { if (!w.expired()) { alive = alive + 1; } }
                LOG + "|" + to_string(alive);
            )";
            for (uint32_t limit : {1000000u, 0u, 1u, 3u}) {
                auto eng = make_engine();
                jai::stdlib::register_all(eng);
                detail::engine_access::release_recursion_limit(*eng, limit);
                check_eq(std::string("d5:3d4:3d3:3d2:3d1:3d0:3|0"), eng->execute("string LOG = \"\";" + program).as_string(),
                         "limit " + std::to_string(limit));
            }
        });

        // The release stack keeps C++ order exactly: every limit, the stack always (0) to never in practice, logs alike
        // call sites remember the callee they last validated (inline guards, the call-site cache, a worker's memo); none of those
        // keeps a closure alive: a capturing closure called at a site dies when its variable lets go, and a self-referencing
        // closure's cell cycle is released when its frame exits, at the same points as with no cache
        test("a_closure_a_call_site_validated_dies_on_time", [this]() {
            expect_log(R"(
                var f = [t = Token("c")](int x) -> int { return x + 1; };
                int s = 0;
                for (int i = 0; i < 3; ++i) { s += f(i); }
                LOG = LOG + "a";
                f = [](int x) -> int { return x; };
                LOG = LOG + "|";
                for (int i = 0; i < 3; ++i) { s += f(i); }
                function mk() -> int {
                    Token keep = Token("k");
                    var g;
                    g = [&](int n) -> int { if (n == 0) { return 0; } return g(n - 1) + 1; };
                    int r = 0;
                    for (int i = 0; i < 3; ++i) { r += g(2); }
                    return r;
                }
                int r = mk();
                LOG = LOG + "|" + to_string(r) + "|" + to_string(s);
                LOG
            )", "ac|k|6|9");
        });

        test("the_release_stack_keeps_destruction_order", [this]() {
            const std::string shapes = R"(
                class Tag { string n = ""; Tag() {} Tag(string s) { n = s; } ~Tag() { LOG = LOG + n; } }
                class Base { string id = ""; ~Base() { LOG = LOG + "b" + id; } }
                class Link : Base { Tag first; shared_ptr<Link> next = null; Tag last; ~Link() { LOG = LOG + "<" + id; } }
                class Plain { Tag t; shared_ptr<Link> link = null; var bag = null; }
                class Dropper { shared_ptr<Dropper> other = null; string id = ""; ~Dropper() { LOG = LOG + "d" + id; other = null; LOG = LOG + "/" + id; } }
                function chain(int n) -> shared_ptr<Link> {
                    shared_ptr<Link> head = null;
                    for (int i = 0; i < n; i++) {
                        shared_ptr<Link> c = new Link();
                        c.id = to_string(i); c.first = Tag("f" + to_string(i)); c.last = Tag("l" + to_string(i)); c.next = head; head = c;
                    }
                    return head;
                }
                function run() -> void {
                    { shared_ptr<Link> h = chain(12); }
                    LOG = LOG + "|";
                    { var p = Plain(); p.t = Tag("pt"); p.link = chain(3); p.bag = [Tag("x"), chain(2), {"k": chain(2), "j": Tag("y")}, [[Tag("z")]]]; }
                    LOG = LOG + "|";
                    { var arr = []; for (int i = 0; i < 6; i++) { var q = Plain(); q.t = Tag("q" + to_string(i)); q.bag = arr; arr = [q, Tag("a" + to_string(i))]; } }
                    LOG = LOG + "|";
                    { shared_ptr<Dropper> d = null; for (int i = 0; i < 5; i++) { shared_ptr<Dropper> e = new Dropper(); e.id = to_string(i); e.other = d; d = e; } }
                }
                run(); LOG;
            )";
            std::string oracle;
            for (uint32_t limit : {1000000u, 0u, 1u, 2u, 5u}) {
                auto eng = make_engine();
                jai::stdlib::register_all(eng);
                detail::engine_access::release_recursion_limit(*eng, limit);
                std::string got;
                try {
                    got = eng->execute("string LOG = \"\";" + shapes).as_string();
                } catch (const std::exception& e) {
                    got = std::string("threw: ") + e.what();
                }
                if (oracle.empty()) {
                    oracle = got;
                    check(oracle.find("threw") == std::string::npos && oracle.size() > 100, oracle);
                } else {
                    check_eq(oracle, got, "limit " + std::to_string(limit));
                }
            }
        });

        // C++ [class.base.init]: every base constructs in declaration order (each with its own bases first), then the members, then
        // the body; destruction runs the reverse
        test("every_base_constructs_in_declaration_order_and_dies_in_reverse", [this]() {
            const std::string family = R"(
                class A { int a = 1; A() { LOG = LOG + "A"; } ~A() { LOG = LOG + "~A"; } }
                class P { int p = 2; P() { LOG = LOG + "P"; } ~P() { LOG = LOG + "~P"; } }
                class B : A { int b = 3; B() { LOG = LOG + "B"; } ~B() { LOG = LOG + "~B"; } }
                class C { int c = 4; C() { LOG = LOG + "C"; } ~C() { LOG = LOG + "~C"; } }
                class Q : P, C { int q = 5; Q() { LOG = LOG + "Q"; } ~Q() { LOG = LOG + "~Q"; } }
                class D : B, Q { Token t = Token("t"); D() { LOG = LOG + "D"; } ~D() { LOG = LOG + "~D"; } }
                class E : B, Q { }
                class F : A, C { int f = a + c; }
            )";
            expect_log(family + "function f() -> void { D d; LOG = LOG + \"|\"; } f(); LOG;", "ABPCQD|~Dt~Q~C~P~B~A");
            expect_log(family + "function f() -> void { E e; LOG = LOG + \"|\"; } f(); LOG;", "ABPCQ|~Q~C~P~B~A");
            expect_log(family + "function f() -> void { F o; LOG = LOG + to_string(o.f) + \"|\"; } f(); LOG;", "AC5|~C~A");
        });

        // C++ [class.base.init]: an initializer list names any base, in any order; the bases still construct in declaration order, an
        // unnamed one default-constructed, and super(...) is the first
        test("an_initializer_list_names_each_base", [this]() {
            const std::string family = R"JAI(
                class A { int a = 0; A(int v) { a = v; LOG = LOG + "A" + to_string(v); } }
                class C { int c = 0; C() { c = -1; LOG = LOG + "C()"; } C(int v) { c = v; LOG = LOG + "C" + to_string(v); } }
                class D : A, C { D() : C(2), A(1) { LOG = LOG + "D"; } }
                class E : A, C { E() : super(3) { LOG = LOG + "E"; } }
                class F : A, C { F(int x) : A(x), C(x * 10) { LOG = LOG + to_string(a + c); } }
            )JAI";
            expect_log(family + "D d; LOG;", "A1C2D");
            expect_log(family + "E e; LOG;", "A3C()E");
            expect_log(family + "F f = F(4); LOG;", "A4C4044");
        });

        test("an_initializer_list_names_only_bases_once_and_every_base_without_a_default", [this]() {
            const std::string prelude = "class A { A() {} A(int v) {} } class C { C() {} } class N { N(int v) {} } ";
            const std::vector<std::pair<std::string, std::string>> cases = {
                {"class H : C, N { H() {} } H h;", "Class 'H' must construct base 'N': it has no default constructor"},
                {"class H : C, N { } H h;", "Class 'H' must construct base 'N': it has no default constructor"},
                {"class K : A { K() : C() {} }", "Class 'K' has no base 'C' to construct"},
                {"class L : A { L() : super(1), A(2) {} }", "Class 'L' constructs base 'A' twice"},
                {"class M : A, C { M() : C(), C() {} }", "Class 'M' constructs base 'C' twice"},
            };
            for (const auto& [program, text] : cases) {
                for (bool warn_region : {false, true}) {
                    auto eng = make_engine();
                    std::string got;
                    try {
                        eng->execute((warn_region ? "[[warn_mode]]; " : "") + prelude + program);
                    } catch (const std::exception& e) {
                        got = e.what();
                    }
                    check_true(got.find(text) != std::string::npos, (warn_region ? "a warn region: " : "strict: ") + program + " -> " + got);
                }
            }
        });

        test("a_throw_in_a_later_base_destroys_the_bases_before_it", [this]() {
            const std::string family = R"(
                class A { A() { LOG = LOG + "A"; } ~A() { LOG = LOG + "~A"; } }
                class Late { Late() { throw "boom"; } ~Late() { LOG = LOG + "~Late"; } }
                class C { C() { LOG = LOG + "C"; } ~C() { LOG = LOG + "~C"; } }
                class D : A, Late { D() { LOG = LOG + "D"; } ~D() { LOG = LOG + "~D"; } }
                class G : A, C { G() { throw "body"; } ~G() { LOG = LOG + "~G"; } }
            )";
            expect_log(family + "try { D d; } catch (e) { LOG = LOG + \"|\" + e; } LOG;", "A~A|boom");
            expect_log(family + "try { G g; } catch (e) { LOG = LOG + \"|\" + e; } LOG;", "AC~C~A|body");
        });

        test("an_override_of_a_later_base_s_method_is_an_override", [this]() {
            auto eng = make_engine();
            std::string got;
            try {
                got = eng->execute(R"(
                    class A { int a = 0; }
                    class C { string who() { return "c"; } string only() { return "o"; } }
                    class D : A, C { string who() override { return "d"; } }
                    D d; d.who() + d.only();
                )").as_string();
            } catch (const std::exception& e) {
                got = std::string("threw: ") + e.what();
            }
            check_eq(std::string("do"), got);
        });

        test("a_diamond_is_refused_at_load_and_at_declaration", [this]() {
            const std::string diamond = "class A { int a = 0; } class B : A { } class C : A { } class D : B, C { }";
            const std::string text = "Class 'D' inherits 'A' through both 'B' and 'C' (diamond inheritance is not supported)";
            for (bool warn_region : {false, true}) {
                auto eng = make_engine();
                std::string got;
                try {
                    eng->execute(warn_region ? "[[warn_mode]]; " + diamond : diamond);
                } catch (const std::exception& e) {
                    got = e.what();
                }
                check_true(got.find(text) != std::string::npos, (warn_region ? "a warn region: " : "strict: ") + got);
            }
            auto eng = make_engine();
            std::string direct;
            try {
                eng->execute("[[warn_mode]]; class A { int a = 0; } class B : A { } class D : A, B { }");
            } catch (const std::exception& e) {
                direct = e.what();
            }
            check_true(direct.find("Class 'D' inherits 'A' through both 'A' and 'B' (diamond inheritance is not supported)") != std::string::npos, direct);
        });

        test("a_reload_replaces_and_adds_destructors_and_copy_constructors", [this]() {
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
                        class A { int a = 1; ~A() { LOG = LOG + "old"; } }
                        class B : A { int b = 2; }
                        class C { int c = 3; }
                    )");
                    eng->execute(R"(class A { int a = 1; ~A() { LOG = LOG + "new"; } })");
                    eng->execute(R"(class C { int c = 3; ~C() { LOG = LOG + "c"; } })");
                    got = eng->execute("function f() -> void { B b; A a; C c; } f(); LOG;").as_string();
                    // a copy constructor a reload adds runs at the next copy, and one it removes no longer does
                    eng->execute(R"(LOG = ""; class K { string n = "k"; K() {} K(const K& o) { n = o.n + "'"; LOG = LOG + "copy"; } })");
                    got += "|" + eng->execute("function g() -> string { K a; K b = a; return b.n; } g() + LOG;").as_string();
                    eng->execute(R"(LOG = ""; class K { string n = "k"; K() {} })");
                    got += "|" + eng->execute("g() + LOG;").as_string();
                } catch (const std::exception& e) {
                    got = std::string("threw: ") + e.what();
                }
                check_eq(std::string("cnewnew|k'copy|k"), got, variant == 0 ? "default engine" : "proofs and passes off");
            }
            // a callee another unit redefines to return an instance: the compiled caller's condition temporary still dies before the body
            for (int variant = 0; variant < 2; ++variant) {
                auto eng = make_engine();
                if (variant == 1) { eng->type_proofs(false); eng->ir_passes(false); }
                std::string got;
                try {
                    eng->execute(R"(
                        string LOG = "";
                        class T { string n; T(string a) { n = a; } ~T() { LOG = LOG + "-" + n; } }
                        int mk() { return 1; }
                        void h() { if (mk()) { LOG = LOG + "in"; } LOG = LOG + "|"; }
                        h();
                    )");
                    eng->execute(R"(T mk() { return T("x"); })");
                    got = eng->execute("h(); LOG;").as_string();
                } catch (const std::exception& e) {
                    got = std::string("threw: ") + e.what();
                }
                check_eq(std::string("in|-xin|"), got, variant == 0 ? "a reloaded callee, default engine" : "a reloaded callee, proofs and passes off");
            }
        });

        // C++ destroys a local inside the function that declares it, so the frames of an error its destructor raises run through it
        test("a_destructor_error_at_a_scope_exit_traces_through_the_function", [this]() {
            const std::string program =
                "class Bad { ~Bad() { throw \"teardown\"; } }\n"
                "function g() -> void {\n"
                "    Bad b;\n"
                "}\n"
                "g();\n";
            std::string got;
            auto thrower = make_engine();
            try {
                thrower->execute(program);
            } catch (const std::exception& e) {
                got = e.what();
            }
            check_true(got.find("teardown") != std::string::npos, got);
            check_eq(std::string("at ~Bad (<script>:1);at g (<script>:3);at <script> (<script>:5);"), trace_frames(thrower->format_stack_trace()));

            std::string reported;
            auto eng = make_engine();
            eng->set_script_error_handler([&reported](const std::string& message) { if (reported.empty()) { reported = message; } });
            try {
                eng->execute("[[warn_mode]]; class Worse { ~Worse() { nowhere(); } }\nfunction h() -> void {\n    Worse w;\n}\nh();\n");
            } catch (const std::exception& e) {
                reported = e.what();
            }
            check_true(reported.find("nowhere") != std::string::npos, reported);
            check_eq(std::string("at ~Worse (<script>:1);at h (<script>:3);at <script> (<script>:5);"), trace_frames(reported));
        });

        // A member initializer's own error leaves the constructor as raised, at every level: what a catch receives is what was thrown
        test("an_initializer_error_propagates_as_raised_at_every_level", [this]() {
            auto run = [this](const std::string& program) {
                std::string got;
                auto eng = make_engine();
                jai::stdlib::register_all(eng);
                try {
                    got = eng->execute("[[warn_mode]]; " + program).as_string();
                } catch (const std::exception& e) {
                    got = std::string("threw: ") + e.what();
                }
                return got;
            };
            const std::string catcher = " string r = \"none\"; try { B b; } catch (e) { r = to_string(e); } r;";
            check_eq(std::string("Undefined variable 'zz'"), run("class B { int z = zz; }" + catcher), "the class's own field");
            check_eq(std::string("Undefined variable 'zz'"), run("class A { int s = zz; A() {} } class B : A { B() {} }" + catcher), "a base's field");
            check_eq(std::string("Undefined variable 'zz'"), run("class A { int s = zz; } class B : A { }" + catcher), "a constructor-less base's field");
            check_eq(std::string("Undefined variable 'zz'"), run("class A { A(int v) {} } class B : A { B() : super(zz) {} }" + catcher), "a super() argument");
            check_eq(std::string("42"), run("int boom() { throw 42; } class A { int s = boom(); A() {} } class B : A { B() {} }" + catcher),
                     "a thrown value arrives intact");
            check_eq(std::string("Undefined variable 'zz'|1"),
                     run("string LOG = \"\"; class A { int a = 1; A() {} ~A() { LOG = LOG + to_string(a); } } class B : A { int s = zz; B() {} }"
                         " string r = \"none\"; try { B b; } catch (e) { r = to_string(e); } r + \"|\" + LOG;"),
                     "the constructed base is destroyed");
        });

        // A value a statement reads or computes on the way to its consumer is held by nothing once the consumer has it: a member
        // read is the member itself (C++ copies it straight into the declaration), and a temporary dies with the operation that
        // consumes it, so the object dies when its last named owner lets go
        test("a_value_read_on_the_way_to_its_consumer_is_held_by_nothing_after_it", [this]() {
            const std::pair<std::string, std::string> cases[] = {
                {R"(
                class Box { shared_ptr<Token> t = null; var d = null; int v = 0; Token tv = Token("v"); }
                function f() -> void {
                    shared_ptr<Box> b = new Box();
                    b.t = new Token("a"); shared_ptr<Token> x = b.t; x = null; LOG = LOG + "<"; b.t = null; LOG = LOG + ">";
                    b.t = new Token("b"); var y = b.t; y = null; LOG = LOG + "<"; b.t = null; LOG = LOG + ">";
                    b.t = new Token("c"); auto z = b.t; z = null; LOG = LOG + "<"; b.t = null; LOG = LOG + ">";
                    b.d = new Token("d"); var w = b.d; w = null; LOG = LOG + "<"; b.d = null; LOG = LOG + ">";
                    { Token copy = b.tv; copy.name = "e"; }
                    LOG = LOG + "|";
                    int k = b.v + 1;
                }
                f(); LOG;
            )", "<a><b><c><d>e|v"},
                {R"(
                class Box { shared_ptr<Token> t = null; }
                function f() -> void {
                    shared_ptr<Box> b = new Box(); shared_ptr<Box> c = new Box();
                    b.t = new Token("a"); bool z = b.t == null; LOG = LOG + "<"; b.t = null; LOG = LOG + ">";
                    b.t = new Token("b"); if (b.t) { LOG = LOG + "?"; } LOG = LOG + "<"; b.t = null; LOG = LOG + ">";
                    b.t = new Token("c"); if (b.t != null) { LOG = LOG + "?"; } LOG = LOG + "<"; b.t = null; LOG = LOG + ">";
                    shared_ptr<Token> x = null;
                    b.t = new Token("d"); x = b.t; x = null; LOG = LOG + "<"; b.t = null; LOG = LOG + ">";
                    b.t = new Token("e"); c.t = b.t; c.t = null; LOG = LOG + "<"; b.t = null; LOG = LOG + ">";
                }
                f(); LOG;
            )", "<a>?<b>?<c><d><e>"},
                {R"(
                class Box { shared_ptr<Token> t = null; }
                function clear(shared_ptr<Box> b) -> bool { b.t = null; LOG = LOG + "!"; return true; }
                function f() -> void {
                    shared_ptr<Box> b = new Box();
                    b.t = new Token("a"); bool z = b.t == null || clear(b); LOG = LOG + "|";
                    b.t = new Token("b"); if (b.t != null && clear(b)) { LOG = LOG + "?"; } LOG = LOG + "|";
                    b.t = new Token("c"); bool y = b.t != null && clear(b) && b.t == null; LOG = LOG + "|";
                }
                f(); LOG;
            )", "a!|b!?|c!|"},
                {R"(
                class Box { shared_ptr<Token> t = null; }
                class Holder {
                    shared_ptr<Box> bx = null;
                    function run() -> void {
                        bx = new Box(); shared_ptr<Box> other = new Box();
                        bx.t = new Token("a"); shared_ptr<Token> x = bx.t; x = null; LOG = LOG + "<"; bx.t = null; LOG = LOG + ">";
                        bx.t = new Token("b"); other.t = bx.t; other.t = null; LOG = LOG + "<"; bx.t = null; LOG = LOG + ">";
                        bx.t = new Token("c"); bool z = bx.t == null; LOG = LOG + "<"; bx.t = null; LOG = LOG + ">";
                    }
                }
                function f() -> void { Holder h = Holder(); h.run(); }
                f(); LOG;
            )", "<a><b><c>"},
                {R"(
                class Box { shared_ptr<Token> t = null; int v = 0; }
                function f() -> void {
                    array<shared_ptr<Box>> arr = [new Box(), new Box()];
                    arr[0].t = new Token("p"); arr[1].t = new Token("q");
                    for (int i = 0; i < 2; i++) { bool z = arr[i].t == null; arr[i].t = null; LOG = LOG + "."; }
                    LOG = LOG + "|";
                    array<shared_ptr<Token>> toks = [new Token("r")];
                    shared_ptr<Token> x = toks[0]; x = null; LOG = LOG + "<"; toks[0] = null; LOG = LOG + ">";
                    for (int i = 0; i < 2; i++) { arr[i].t = new Token("s" + to_string(i)); shared_ptr<Token> y = arr[i].t; y = null; arr[i].t = null; LOG = LOG + "."; }
                    LOG = LOG + "|";
                }
                f(); LOG;
            )", "p.q.|<r>s0.s1.|"},
                {R"(
                class V { int x = 0; string tag = ""; V() {} V(int a, string t) { x = a; tag = t; } ~V() { LOG = LOG + tag; } V operator+(V o) { return V(x + o.x, tag + o.tag); } }
                class W { string tag = ""; W(string t) { tag = t; } ~W() { LOG = LOG + tag; } W operator+(const W& o) { return W(tag + o.tag); } }
                function mk(string n) -> shared_ptr<Token> { return new Token(n); }
                function f() -> void {
                    bool z = mk("a") == null; LOG = LOG + "|";
                    for (int i = 0; i < 2; i++) { if (mk("L" + to_string(i)) != null) { LOG = LOG + "."; } LOG = LOG + ","; }
                    LOG = LOG + "|";
                    V a = V(1, "x"); V b = V(2, "y"); V c = V(3, "z");
                    V d = a + b + c; LOG = LOG + "|";
                    W e = W("p") + W("q"); LOG = LOG + "|";
                }
                f(); LOG;
            )", "a|L0.,L1.,|yzxy|qp|pqxyzzyx"},
                {R"(
                class Box { shared_ptr<Token> t = null; }
                function f() -> void {
                    shared_ptr<Box> b = new Box();
                    for (int i = 0; i < 3; i++) {
                        b.t = new Token(to_string(i)); shared_ptr<Token> x = b.t; x = null; LOG = LOG + "<"; b.t = null; LOG = LOG + ">";
                    }
                    int n = 0;
                    while (n < 2) { b.t = new Token("w" + to_string(n)); if (b.t != null) { n++; } b.t = null; LOG = LOG + "."; }
                    LOG = LOG + "|";
                }
                f(); LOG;
            )", "<0><1><2>w0.w1.|"},
            };
            check_each(cases, [&](const auto& c) { expect_log(c.first, c.second); });
        });

        test("with_no_destructor_class_every_local_temporary_and_parameter_dies_on_time", [this]() {
            // weak_ptr and use_count observe what a destructor would: block locals (a lambda's too), the value a declaration, compare,
            // condition, argument, arm, case, call or element read consumes, a switch subject a continue leaves, a callee's parameters
            expect_outcome({std::string(k_observed) + R"(
                function clear_check(shared_ptr<Box> b, weak_ptr<P> w) -> bool { b.t = null; note("in:" + to_string(w.expired())); return true; }
                function mk(weak_ptr<P>& w) -> shared_ptr<P> { shared_ptr<P> p = new P(); w = p; return p; }
                function take(bool a, bool b) -> void { }
                function pick(shared_ptr<Box> b) -> shared_ptr<P> { return b.t; }
                function g(shared_ptr<P> p) -> int { return p.v + 1; }
                function h(shared_ptr<P> p) -> int { shared_ptr<P> q = p; return q.v; }
                function locals() -> void {
                    weak_ptr<P> w = null;
                    { shared_ptr<P> s = new P(); w = s; } note("block:" + to_string(w.expired()));
                    for (int i = 0; i < 2; i++) { shared_ptr<P> s = new P(); w = s; } note("loop:" + to_string(w.expired()));
                    { var s = new P(); w = s; } note("var:" + to_string(w.expired()));
                    { shared_ptr<P> s = new P(); w = s; auto f = [s]() { return s.v; }; s = null; note("held:" + to_string(w.expired())); }
                    note("lambda:" + to_string(w.expired()));
                }
                function reads() -> void {
                    shared_ptr<Box> b = new Box();
                    b.t = new P(); weak_ptr<P> w = b.t;
                    shared_ptr<P> x = b.t; x = null; b.t = null; note("decl:" + to_string(w.expired()));
                    b.t = new P(); w = b.t; bool z = b.t == null; b.t = null; note("cmp:" + to_string(w.expired()));
                    b.t = new P(); w = b.t; if (b.t != null) { } b.t = null; note("cond:" + to_string(w.expired()));
                    b.t = new P(); w = b.t; bool y = b.t == null || clear_check(b, w); note("full:" + to_string(w.expired()));
                    b.t = new P(); w = b.t; take(b.t == null, clear_check(b, w)); note("args:" + to_string(w.expired()));
                    b.t = new P(); w = b.t; shared_ptr<P> c = true ? b.t : null; c = null; b.t = null; note("arm:" + to_string(w.expired()));
                    b.t = new P(); w = b.t; switch (b.t.v) { case 3: b.t = null; note("case:" + to_string(w.expired())); break; default: break; }
                    b.t = new P(); [[warn_mode]] z = b.t == null; note("count:" + to_string(b.t.use_count()));
                    b.u = new P(); w = b.u; var q = b.u; q = null; b.u = null; note("untyped:" + to_string(w.expired()));
                    weak_ptr<P> made = null;
                    bool m = mk(made) == null; note("call:" + to_string(made.expired()));
                    if (mk(made) != null) { note("body:" + to_string(made.expired())); }
                    array<shared_ptr<P>> a = [new P()]; w = a[0];
                    x = a[0]; x = null; a[0] = null; note("elem:" + to_string(w.expired()));
                    a[0] = new P(); w = a[0];
                    for (int i = 0; i < 1; i++) { bool e = a[i] == null; a[i] = null; } note("eloop:" + to_string(w.expired()));
                    for (int i = 0; i < 2; i++) {
                        if (i == 1) { note("continue:" + to_string(w.expired())); }
                        b.t = new P(); w = b.t;
                        switch (pick(b)) { case null: break; default: b.t = null; continue; }
                    }
                    note("subject:" + to_string(w.expired()));
                    b.t = new P(); w = b.t; int k = g(b.t); b.t = null; note("param:" + to_string(w.expired()));
                    b.t = new P(); w = b.t; [[warn_mode]] k = h(b.t); b.t = null; note("local:" + to_string(w.expired()));
                }
                locals(); reads(); OUT;
            )"}, "block:true loop:true var:true held:false lambda:true decl:true cmp:true cond:true in:true full:true in:true args:true arm:true "
                 "case:true count:1 untyped:true call:true body:true elem:true eloop:true continue:true subject:true param:true local:true ");
        });

        test("a_destructor_class_declared_after_a_function_compiled_runs_on_time", [this]() {
            expect_outcome({R"(
                string LOG = "";
                class Box { var t = null; }
                function f(shared_ptr<Box> b) -> void { bool z = b.t == null; LOG = LOG + "<"; b.t = null; LOG = LOG + ">"; }
                function g(shared_ptr<Box> b) -> void { if (b.t != null) { LOG = LOG + "?"; } LOG = LOG + "<"; b.t = null; LOG = LOG + ">"; }
                function k(shared_ptr<Box> b) -> void { { var x = b.t; } LOG = LOG + "<"; b.t = null; LOG = LOG + ">"; }
                shared_ptr<Box> warm = new Box(); f(warm); g(warm); k(warm); LOG = "";
            )", R"(
                class D { ~D() { LOG = LOG + "d"; } }
                shared_ptr<Box> b = new Box(); b.t = new D(); f(b); b.t = new D(); g(b); b.t = new D(); k(b);
                LOG;
            )"}, "<d>?<d><d>");
        });
    }

    // the frames of the first error a program raises, as the engine's stack trace lists them
    static std::string trace_frames(const std::string& text) {
        std::string result;
        size_t at = 0;
        while ((at = text.find("at ", at)) != std::string::npos) {
            const size_t end = text.find('\n', at);
            result += text.substr(at, end == std::string::npos ? std::string::npos : end - at) + ";";
            if (end == std::string::npos) { break; }
            at = end;
        }
        return result;
    }

    static constexpr const char* k_family = R"(
        class Base { int a = 1; Base() {} ~Base() { LOG = LOG + "B"; } }
        class D1 : Base { int d1 = 2; Token t1 = Token("t"); D1() {} ~D1() { LOG = LOG + "1"; } }
        class D2 : Base { int d2 = 3; D2() {} ~D2() { LOG = LOG + "2"; } }
    )";
};

}

using destructor_timing_tests = jai::foundry::tests::destructor_timing_tests;
FOUNDRY_REGISTER(destructor_timing_tests)
