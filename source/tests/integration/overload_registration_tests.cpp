#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// Exercises the overloaded-function registration/dispatch surface across entry points and
// registration orders. These pin the behavior the registration consolidation must preserve, and
// guard the drift bugs the parallel paths have historically hidden (e.g. the dispatcher-into-its-
// own-set recursion that crashed a 0-arg call after a typed overload).
class overload_registration_tests : public suite {
public:
    overload_registration_tests() : suite("Overload Registration") {}

    void forge_tests() override {

        test("free_fn_same_arity_resolves_by_type", [this]() {
            auto eng = make_engine();
            eng->add_function("pick", [](int a, int b) -> std::string { (void)a; (void)b; return "ints"; });
            eng->add_function("pick", [](double a, double b) -> std::string { (void)a; (void)b; return "floats"; });
            check_eq(std::string("ints"), eng->execute("pick(1, 2)").as<std::string>());
            check_eq(std::string("floats"), eng->execute("pick(1.5, 2.5)").as<std::string>());
        });

        // A set remembers the overload an argument shape selected; an overload added later forgets it
        test("resolution_follows_argument_types_and_added_overloads", [this]() {
            auto eng = make_engine();
            eng->add_function("rc", [](double a) -> std::string { (void)a; return "f"; });
            eng->add_function("rc", [](const std::string& a) -> std::string { (void)a; return "s"; });
            const char* loop = "string out = \"\"; for (int i = 0; i < 3; i++) { out += rc(1.5) + rc(\"x\") + rc(2) + \"|\"; } out";
            check_eq(std::string("fsf|fsf|fsf|"), eng->execute(loop).as<std::string>());
            eng->add_function("rc", [](script_int a) -> std::string { (void)a; return "i"; });
            check_eq(std::string("fsi|fsi|fsi|"), eng->execute(loop).as<std::string>(), "the added int overload wins for an int");
        });

        // A std::string parameter reads a utf8_string's bytes, as the checker already admits; an exact string overload still wins
        test("a_utf8_string_reaches_a_string_parameter", [this]() {
            auto eng = make_engine();
            eng->add_function("bytes_of", [](std::string a) -> script_int { return static_cast<script_int>(a.size()); });
            check_eq((int64_t)3, eng->execute("utf8_string u = \"a\xC3\xA9\"; bytes_of(u);").as_int());
            check_eq((int64_t)3, eng->execute("var u = \"a\xC3\xA9\".to_utf8(); bytes_of(u);").as_int());
            eng->add_function("kind", [](const std::string& a) -> std::string { (void)a; return "string"; });
            eng->add_function("kind", [](script_int a) -> std::string { (void)a; return "int"; });
            check_eq(std::string("string int"), eng->execute("var u = \"x\".to_utf8(); kind(u) + \" \" + kind(1);").as<std::string>());
        });

        test("one_overload_rejects_with_the_set_text", [this]() {
            auto eng = make_engine();
            eng->add_function("one", [](double a) -> double { return a; });
            check_eq(std::string("No overload of 'one' takes (string)"),
                     eng->execute("string m = \"\"; var s = \"x\"; try { one(s); } catch (e) { m = e; } m").as<std::string>());
            check_near(2.5, eng->execute("one(2.5)").as_float(), 1e-12);
        });

        // A call site's cached host callee follows the name: a re-registration or a script definition takes over
        test("host_callee_cache_follows_rebinding", [this]() {
            auto eng = make_engine();
            eng->add_function("hx", [](script_int a) -> script_int { return a + 1; });
            eng->execute("function run() -> int { int s = 0; for (int i = 0; i < 3; i++) { s += hx(i); } return s; }");
            check_eq((int64_t)6, eng->execute("run()").as_int());
            check_eq((int64_t)6, eng->execute("run()").as_int(), "a second run hits the cache");
            eng->add_function("hx", [](script_int a) -> script_int { return a * 10; });
            check_eq((int64_t)30, eng->execute("run()").as_int(), "the re-registered host function");
            eng->execute("function hx(int a) -> int { return 100; }");
            check_eq((int64_t)300, eng->execute("run()").as_int(), "a script function of the name");
        });

        test("free_fn_distinct_arity", [this]() {
            auto eng = make_engine();
            eng->add_function("g", [](int a) -> int { return a; });
            eng->add_function("g", [](int a, int b) -> int { return a + b; });
            check_eq((int64_t)5, eng->execute("g(5)").as_int());
            check_eq((int64_t)7, eng->execute("g(3, 4)").as_int());
        });

        // Registration ORDER must not change resolution — order independence is exactly where the
        // parallel registration paths tend to drift.
        test("free_fn_registration_order_invariant", [this]() {
            auto a = make_engine();
            a->add_function("h", [](int x) -> std::string { (void)x; return "i"; });
            a->add_function("h", [](const std::string& x) -> std::string { (void)x; return "s"; });
            check_eq(std::string("i"), a->execute("h(1)").as<std::string>());
            check_eq(std::string("s"), a->execute("h(\"x\")").as<std::string>());

            auto b = make_engine();
            b->add_function("h", [](const std::string& x) -> std::string { (void)x; return "s"; });
            b->add_function("h", [](int x) -> std::string { (void)x; return "i"; });
            check_eq(std::string("i"), b->execute("h(1)").as<std::string>());
            check_eq(std::string("s"), b->execute("h(\"x\")").as<std::string>());
        });

        // Mixed string/int overloads coexist and the engine stays usable across calls (no set
        // corruption from registering the second overload).
        test("free_fn_mixed_types_engine_stays_usable", [this]() {
            auto eng = make_engine();
            eng->add_function("m", [](int x, int y) -> int { return x + y; });
            eng->add_function("m", [](const std::string& x, const std::string& y) -> std::string { return x + y; });
            check_eq((int64_t)3, eng->execute("m(1, 2)").as_int());
            check_eq(std::string("ab"), eng->execute("m(\"a\", \"b\")").as<std::string>());
            check_eq((int64_t)5, eng->execute("m(2, 3)").as_int());
        });

        // A typed function and then a variadic of the same name: the fixed-arity overload wins at
        // its own arity, the variadic catches every other arity.
        test("typed_then_variadic_same_name", [this]() {
            auto eng = make_engine();
            engine* e = eng.get();
            eng->add_function("v", [](int x, int y) -> std::string { (void)x; (void)y; return "two"; });
            eng->add_variadic_function("v", [e](const std::vector<script_value>& args) -> script_value {
                return script_value(std::string("var:") + std::to_string(args.size()), e);
            });
            check_eq(std::string("two"), eng->execute("v(1, 2)").as<std::string>());
            check_eq(std::string("var:1"), eng->execute("v(9)").as<std::string>());
            check_eq(std::string("var:3"), eng->execute("v(1, 2, 3)").as<std::string>());
        });

        test("variadic_then_typed_same_name", [this]() {
            auto eng = make_engine();
            engine* e = eng.get();
            eng->add_variadic_function("w", [e](const std::vector<script_value>& args) -> script_value {
                return script_value(std::string("var:") + std::to_string(args.size()), e);
            });
            eng->add_function("w", [](int x, int y) -> std::string { (void)x; (void)y; return "two"; });
            check_eq(std::string("two"), eng->execute("w(1, 2)").as<std::string>());
            check_eq(std::string("var:1"), eng->execute("w(9)").as<std::string>());
        });

        // The dispatcher-recursion regression, from the registration angle: a 0-arg constructor
        // registered (which immediately installs a dispatcher) followed by a typed N-arg ctor must
        // not migrate the dispatcher into its own set — a 0-arg call must not self-recurse.
        test("ctor_zeroarg_after_typed_no_recursion", [this]() {
            auto eng = make_engine();
            class C { public: int v = 7; C() {} C(int a, int b) { v = a + b; } };
            dynamic_binder<C>(*eng, "C").constructor<>().constructor<int, int>().property("v", &C::v).build();
            check_eq((int64_t)7, eng->execute("auto z = C(); z.v").as_int());
            check_eq((int64_t)5, eng->execute("auto t = C(2, 3); t.v").as_int());
        });

        // A bound method whose C++ signature takes only values that convert without failing gets a direct lane (the vm reaches the
        // method it bound without minting a bound method); its results, its declines (an argument of another kind, a count mismatch)
        // and its raises are the ordinary call's, on both backends. An overload, a guarded method or a class parameter has no lane.
        test("binder_method_lanes_are_the_ordinary_call", [this]() {
            struct Acc {
                int64_t total = 0; double scale = 1.0; std::string tag; int32_t small = 0;
                int64_t add(int64_t v) { total += v; return total; }
                Acc& bump() { total += 1; return *this; }
                double scaled(double v) const { return v * scale; }
                int narrow(int32_t v) { small = v; return small * 2; }
                void reset() { total = 0; }
                int64_t get_total() const { return total; }
                int boom(int v) { if (v > 2) { throw std::runtime_error("boom " + std::to_string(v)); } return v; }
                std::string pick_i(int) { return "i"; }
                std::string pick_s(const std::string&) { return "s"; }
            };
            auto run = [&](bool vm, const std::string& src, std::string* lanes = nullptr) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                stdlib::register_all(*e);
                dynamic_binder<Acc>(*e, "Acc").constructor<>()
                    .method("add", &Acc::add)
                    .method("bump", &Acc::bump)
                    .method("scaled", &Acc::scaled)
                    .method("narrow", &Acc::narrow)
                    .method("reset", &Acc::reset)
                    .method("get_total", &Acc::get_total)
                    .method("boom", &Acc::boom)
                    .method("label", [](Acc& self, const std::string& t) -> Acc& { self.tag = t; return self; })
                    .method("tagged", [](const Acc& self, bool upper) -> std::string { return upper ? self.tag + "!" : self.tag; })
                    .method("pick", &Acc::pick_i)
                    .method("pick", &Acc::pick_s)
                    .method("guarded", &Acc::get_total, jai::filesystem)
                    .build();
                if (lanes) {
                    auto def = detail::engine_access::get_class_definition(*e, "Acc");
                    for (const char* n : { "add", "bump", "scaled", "narrow", "reset", "get_total", "boom", "label", "tagged", "pick", "guarded" }) {
                        if (def && def->find_native_method(detail::engine_access::symbolize(*e, n))) { *lanes += std::string(n) + " "; }
                    }
                }
                std::string got;
                try { got = e->execute(src).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
                return got;
            };
            const std::string src = R"(
                auto a = Acc();
                int s = 0;
                for (int i = 0; i < 50; ++i) { s = s + a.add(i) % 7; s = s + a.narrow(i); }
                float f = a.scaled(3) + a.scaled(1.5);
                a.label("x").label("tag");
                string t = a.tagged(true) + a.tagged(false) + a.pick(1) + a.pick("q") + to_string(a.guarded());
                string errs = "";
                [[warn_mode]] try { a.add("seven"); } catch (e) { errs = errs + "|" + e; }
                try { a.narrow(1 << 40); } catch (e) { errs = errs + "|" + e; }
                [[warn_mode]] try { a.add(1, 2); } catch (e) { errs = errs + "|" + e; }
                try { a.boom(5); } catch (e) { errs = errs + "|" + e; }
                a.reset();
                int chained = Acc().bump().bump().add(3) + a.bump().bump().get_total();
                to_string(chained) + " " + to_string(s) + " " + to_string(f) + " " + t + " " + to_string(a.get_total()) + " " + to_string(a.boom(2)) + errs;
            )";
            const std::string oracle = run(false, src);
            check_true(oracle.rfind("throw:", 0) != 0 && oracle.find("boom 5") != std::string::npos, "the interpreter's answer: " + oracle);
            std::string lanes;
            check_eq(oracle, run(true, src, &lanes), "the vm's answer is the interpreter's");
            check_eq(std::string("add bump scaled narrow reset get_total boom label tagged pick "), lanes, "the lanes: every unguarded binding, a reference result and an overload set included");
        });

        // A bare call of a sibling method inside a method (`step(n - 1)`) parks the class's method on the frame's receiver without minting a
        // bound method: a subclass overriding the sibling, a redefinition of the class (hot reload), a local that shadows the name and
        // alternating receivers of two classes all call what the ordinary resolution calls, on both backends
        test("sibling_method_calls_resolve_as_the_member_ladder", [this]() {
            const std::string src = R"(
                class Walker {
                    int base = 1;
                    int step(int n) { return n + base; }
                    int walk(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + step(i); } return s; }
                    int shadowed(int n) { var step = function(int k) -> int { return 1000 + k; }; return step(n); }
                    int down(int n) { if (n == 0) { return base; } return down(n - 1) + 1; }
                }
                class Runner : Walker {
                    int step(int n) override { return n * 10; }
                }
                var w = Walker(); var r = Runner();
                function probe() -> string { return to_string(w.walk(4)); }
                string out = "";
                for (int k = 0; k < 3; ++k) { out = out + to_string(w.walk(4)) + "," + to_string(r.walk(4)) + ";"; }
                out = out + to_string(w.shadowed(2)) + "," + to_string(r.down(5)) + ";";
                out;
            )";
            const std::string reload = R"(
                class Walker {
                    int base = 1;
                    int step(int n) { return n - base; }
                    int walk(int n) { int s = 0; for (int i = 0; i < n; ++i) { s = s + step(i); } return s; }
                    int shadowed(int n) { var step = function(int k) -> int { return 1000 + k; }; return step(n); }
                    int down(int n) { if (n == 0) { return base; } return down(n - 1) + 1; }
                }
                extern function probe;
                probe();
            )";
            auto run = [&](bool vm, bool proofs) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                stdlib::register_all(*e);
                if (!proofs) { e->type_proofs(false); }
                std::string got;
                try { got = e->execute(src).to_string(); got += "|" + e->execute(reload).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
                return got;
            };
            const std::string oracle = run(false, true);
            check_true(oracle.rfind("throw:", 0) != 0, "the interpreter's answer: " + oracle);
            check_eq(std::string("10,60;10,60;10,60;1002,6;|2"), oracle, "the expected values");
            check_eq(oracle, run(true, true), "vm");
            check_eq(oracle, run(true, false), "vm, proofs off");
            check_eq(oracle, run(false, false), "interpreter, proofs off");
        });

        // The sibling memo for the other method shapes: overloads chosen by the arguments, a reference parameter, a default argument, a
        // class parameter, a private sibling, a throw, a coroutine minted by a bare call, a lambda in a method and a sibling that a
        // derived class overrides on alternating receivers, each called from a loop
        test("sibling_method_calls_of_every_shape", [this]() {
            const std::string src = R"(
                class P { int x = 0; P(int v) { x = v; } }
                class Shapes {
                    int n = 2;
                    int pick(int k) { return k; }
                    int pick(string s) { return 100; }
                    void bump(int& r) { r = r + n; }
                    int dflt(int a, int b = 5) { return a * b; }
                    int px(P p) { return p.x; }
                    int vf(int k) { return k + 1; }
                    private: int secret(int k) { return k * 7; }
                    public:
                    int raise_at(int k) { if (k == 2) { throw "at " + to_string(k); } return k; }
                    coroutine int gen(int m) { for (int i = 0; i < m; ++i) { yield i * n; } }
                    string run() {
                        int s = 0; int r = 0; string errs = "";
                        for (int i = 0; i < 4; ++i) {
                            s = s + pick(i) + pick("q") + dflt(i) + dflt(i, 2) + px(P(i)) + secret(i) + vf(i);
                            bump(r);
                            try { s = s + raise_at(i); } catch (e) { errs = errs + e; }
                        }
                        var co = gen(3); int g = 0;
                        while (true) { var v = co.next(); if (co.done()) { break; } g = g + v; }
                        var f = [this](int k) { return pick(k) + vf(k); };
                        int l = 0; for (int i = 0; i < 3; ++i) { l = l + f(i); }
                        return to_string(s) + " " + to_string(r) + " " + errs + " " + to_string(g) + " " + to_string(l);
                    }
                }
                class Over : Shapes { int vf(int k) override { return k * 1000; } }
                var a = Shapes(); var b = Over();
                string out = "";
                for (int k = 0; k < 2; ++k) { out = out + a.run() + ";" + b.run() + ";"; }
                out;
            )";
            auto run = [&](bool vm, bool proofs) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                stdlib::register_all(*e);
                if (!proofs) { e->type_proofs(false); }
                std::string got;
                try { got = e->execute(src).to_string(); } catch (const std::exception& ex) { got = std::string("throw:") + ex.what(); }
                return got;
            };
            const std::string oracle = run(false, true);
            check_true(oracle.rfind("throw:", 0) != 0, "the interpreter's answer: " + oracle);
            check_eq(std::string("510 8 at 2 6 9;6500 8 at 2 6 3003;510 8 at 2 6 9;6500 8 at 2 6 3003;"), oracle, "the expected values");
            check_eq(oracle, run(true, true), "vm");
            check_eq(oracle, run(true, false), "vm, proofs off");
            check_eq(oracle, run(false, false), "interpreter, proofs off");
        });

        // The vm's memo of a host class's native lane follows the class: a lane replaced or withdrawn after a site cached it is what the
        // next call through that site takes (the replacement's answer, or the ordinary call's)
        test("native_lane_site_memo_follows_the_class", [this]() {
            struct Acc {
                int64_t total = 0;
                int64_t add(int64_t v) { total += v; return total; }
            };
            for (bool vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                stdlib::register_all(*e);
                dynamic_binder<Acc>(*e, "Acc").constructor<>().method("add", &Acc::add).build();
                e->execute("auto a = Acc(); function go(var acc, int n) -> int { int s = 0; for (int i = 0; i < n; ++i) { s = s + acc.add(i); } return s; }");
                const std::string tag = vm ? " (vm)" : " (interpreter)";
                check_eq((int64_t)20, e->execute("go(a, 5)").as_int(), "the lane" + tag);
                auto def = detail::engine_access::get_class_definition(*e, "Acc");
                def->withdraw_native_lane(detail::engine_access::symbolize(*e, "add"));
                check_eq((int64_t)20, e->execute("go(a, 5)").as_int(), "the withdrawn lane's ordinary call" + tag);
                def->add_native_method(detail::engine_access::symbolize(*e, "add"), [](void*, const script_value& receiver, const script_value*, size_t argc, script_value& out) {
                    if (argc != 1) { return false; }
                    out = script_value(script_int(1000), receiver.payload_engine());
                    return true;
                });
                check_eq(vm ? (int64_t)5000 : (int64_t)20, e->execute("go(a, 5)").as_int(), "a lane added later" + tag);
            }
        });
    }
};

} // namespace jai::foundry::tests

using overload_registration_tests = jai::foundry::tests::overload_registration_tests;
FOUNDRY_REGISTER(overload_registration_tests)
