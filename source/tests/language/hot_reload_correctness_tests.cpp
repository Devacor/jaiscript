// Regression tests for hot-reload correctness.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <variant>
#include "../case_table.hpp"

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// C++ base for the script-extends-C++ reload test. Tracks live instances so the test can prove
// the _cpp_object survives a field-changing reload — neither dropped (which would run the
// destructor) nor silently reconstructed.
struct ReloadCppBase {
    int payload = 0;
    static int live_instances;
    ReloadCppBase() { ++live_instances; }
    ReloadCppBase(int v) : payload(v) { ++live_instances; }
    ~ReloadCppBase() { --live_instances; }
    int payload_value() const { return payload; }
};
int ReloadCppBase::live_instances = 0;

class hot_reload_correctness_tests : public suite {
public:
    hot_reload_correctness_tests() : suite("Hot Reload Correctness") {}

    void forge_tests() override {

        // Static fields are runtime state; a reload must preserve them, not reset to the
        // initializer.
        test("static_state_preserved_across_reload", [&]() {
            auto eng = make_engine();
            eng->execute("class Counter { static int count = 0; }");
            eng->execute("Counter::count = 5;");
            check_eq(eng->execute("Counter::count").as<int>(), 5);
            eng->execute("class Counter { static int count = 0; }"); // reload, same shape
            check_eq(eng->execute("Counter::count").as<int>(), 5);   // bug: resets to 0
        });

        // A static dropped from the new definition must no longer be accessible.
        test("removed_static_inaccessible_after_reload", [&]() {
            auto eng = make_engine();
            eng->execute("class S { static int a = 1; static int b = 2; }");
            eng->execute("class S { static int a = 1; }"); // b removed
            check_throws([&]() { (void)eng->execute("S::b"); }, "removed static must be inaccessible");
        });

        // Reloading a BASE class must not drop the derived instance's own field data.
        test("base_reload_preserves_derived_instance_data", [&]() {
            auto eng = make_engine();
            eng->execute(R"(
                class Base { int bv = 1; }
                class Derived : Base { int dv = 2; }
                auto d = Derived();
                d.bv = 88;
                d.dv = 99;
            )");
            eng->execute("class Base { int bv = 1; int nb = 7; }"); // base gains a field
            check_eq(eng->execute("d.dv").as<int>(), 99); // bug: derived data lost
            check_eq(eng->execute("d.bv").as<int>(), 88); // inherited data preserved
            check_eq(eng->execute("d.nb").as<int>(), 7);  // new inherited field added
        });

        // A field a reload adds starts on each live object as its initializer makes it there, with 'this' bound, as construction does
        test("a_reloaded_initializer_that_calls_a_method_runs_on_each_live_object", [&]() {
            for (int variant = 0; variant < 2; ++variant) {
                auto eng = make_engine();
                if (variant == 1) {
                    eng->type_proofs(false);
                    eng->ir_passes(false);
                }
                eng->execute(R"(
                    class W { int a = 1; int twice() { return a * 2; } }
                    class V : W { int v = 0; }
                    W w; w.a = 5;
                    V v; v.a = 7;
                )");
                eng->execute("class W { int a = 1; int b = twice(); int twice() { return a * 2; } }");
                check_eq((int64_t)10, eng->execute("w.b").as_int(), "the live W");
                check_eq((int64_t)14, eng->execute("v.b").as_int(), "the live V");
                check_eq((int64_t)2, eng->execute("W fresh; fresh.b").as_int(), "a new W");
            }
        });

        // RETYPE MIGRATION RULE: changing a field's declared type on reload CONVERTS the live value wherever the assignment
        // conversion table allows (int -> string, float -> int truncation, int -> float widening), on derived instances too;
        // a value with no conversion adopts the new initializer, and a same-type initializer change keeps the live value
        test("a_reload_converts_a_retyped_fields_live_value_or_adopts_the_new_default", [this]() {
            struct retype_case { const char* code; const char* before; const char* reload; std::variant<int, double, std::string> expected; };
            const char* const derived = R"(
                class Base { int bv = 0; }
                class Derived : Base { int dv = 0; }
                auto d = Derived();
                d.bv = 42;
                d.dv = 99;
            )";
            const retype_case cases[] = {
                {"b.val", R"( class Box { int val = 0; } auto b = Box(); b.val = 42; )", "class Box { string val = \"\"; }", std::string("42")},
                {"b.f", R"( class Box { float f = 0.0; int i = 0; } auto b = Box(); b.f = 6.9; b.i = 3; )", "class Box { int f = 0; float i = 0.0; }", 6},
                {"b.i", R"( class Box { float f = 0.0; int i = 0; } auto b = Box(); b.f = 6.9; b.i = 3; )", "class Box { int f = 0; float i = 0.0; }", 3.0},
                {"b.s", R"( class Box { string s = ""; } auto b = Box(); b.s = "hello"; )", "class Box { int s = 5; }", 5},
                {"b.val", R"( class Box { int val = 0; } auto b = Box(); b.val = 42; )", "class Box { int val = 7; }", 42},
                {"d.bv", derived, "class Base { string bv = \"\"; }", std::string("42")},
                {"d.dv", derived, "class Base { string bv = \"\"; }", 99},
            };
            check_each(cases, [](const retype_case& c) {
                auto eng = make_engine();
                eng->execute(c.before);
                eng->execute(c.reload);
                auto value = eng->execute(c.code);
                if (const auto* i = std::get_if<int>(&c.expected)) {
                    check_eq(value.as<int>(), *i);
                } else if (const auto* f = std::get_if<double>(&c.expected)) {
                    check_eq(value.as<double>(), *f);
                } else {
                    check_eq(std::get<std::string>(c.expected), value.as<std::string>());
                }
            });
        });

        // An error in one instance's hot_reload_migrate hook must not abort the reload for
        // the others, and reaches the script-error handler with the class's name.
        test("migrate_hook_error_is_isolated", [&]() {
            auto eng = make_engine();
            std::string reported;
            eng->set_script_error_handler([&](const std::string& message) { reported += message; });
            eng->execute(R"(
                class Item {
                    int v = 0;
                    void hot_reload_migrate() { if (v == 1) { throw "boom"; } }
                }
                auto a = Item(); a.v = 1;
                auto b = Item(); b.v = 2;
            )");
            eng->execute("class Item { int v = 0; int w = 5; void hot_reload_migrate() { if (v == 1) { throw \"boom\"; } w = v * 10; } }");
            check_eq(eng->execute("b.w").as<int>(), 20); // b migrated despite a's hook throwing
            check_eq(eng->execute("b.v").as<int>(), 2);
            check_true(reported.find("Item::hot_reload_migrate failed: boom") != std::string::npos, reported);
        });

        // The hook runs against the new declaration: a retyped field already holds its converted value, and a field the
        // reload removes is still readable (the drop comes after the hook)
        test("migrate_hook_sees_retyped_fields_converted", [&]() {
            auto eng = make_engine();
            eng->execute("class Player { int hp = 100; } var p = Player(); p.hp = 62;");
            eng->execute("class Player { string hp = \"0\"; int n = 0; void hot_reload_migrate() { n = hp.size(); hp = `${hp}!`; } }");
            check_eq(std::string("62! 2"), eng->execute("`${p.hp} ${p.n}`").to_string());
        });

        // A changed base list migrates the inherited fields: the new base's fields start from their initializers, the old
        // base's leave, and the new chain's destructor runs at the object's death
        test("a_changed_base_migrates_inherited_fields", [&]() {
            auto eng = make_engine();
            std::string log;
            eng->add_function("note", [&log](std::string s) { log += s; });
            eng->execute(R"(
                class B1 { int x = 1; int who() { return 1; } }
                class B2 { int y = 2; int who() { return 2; } ~B2() { note(`~${y}`); } }
                class D : B1 { int d = 0; D(int v) { d = v; } }
                var a = D(1);
                class E : D { int e = 5; E() : D(3) {} }
                var b = E();
            )");
            eng->execute("class D : B2 { int d = 0; D(int v) { d = v; } }");
            check_eq(std::string("2 2 1 2 3 5"), eng->execute("`${a.who()} ${a.y} ${a.d} ${b.y} ${b.d} ${b.e}`").to_string());
            check_eq(std::string("D has no member 'x'"), eng->execute("var r = \"\"; try { var gone = a.x; } catch (e) { r = e; } r;").to_string());
            eng->execute("a = null; b = null;");
            check_eq(std::string("~2~2"), log);
            eng->execute("var c = D(4);");
            eng->execute("class D { int d = 0; D(int v) { d = v; } }");
            check_eq(std::string("D has no member 'y' 4"), eng->execute("var r = \"\"; try { var gone = c.y; } catch (e) { r = e; } `${r} ${c.d}`;").to_string(),
                     "a base the reload removes takes its fields along");
            eng->execute("class B3 { int x = 1; } class D3 : B3 { int d = 0; } var k = D3(); k.x = 5;");
            eng->execute("class D3 : B3 { int d = 0; int e = 1; }");
            check_eq(std::string("5 1"), eng->execute("`${k.x} ${k.e}`").to_string(), "a derived class's own field change keeps its inherited values");
        });

        // A hot_reload_migrate hook that constructs a new same-class instance must not hang
        // the engine (instances_ mutated while iterated). The budget caps the runaway.
        test("migrate_hook_constructing_instance_terminates", [&]() {
            auto eng = make_engine();
            eng->execute(R"(
                class Node {
                    int v = 0;
                    void hot_reload_migrate() { auto n = Node(); }
                }
                auto root = Node();
            )");
            eng->execute("class Node { int v = 0; int extra = 1; void hot_reload_migrate() { auto n = Node(); } }");
            check_eq(eng->execute("root.extra").as<int>(), 1);
        });

        // A call site made warm (a bound-method or dispatch cache filled and hit) resolves anew after a reload: a new static or
        // instance body, a derived override that arrives later, a method the base gains
        test("a_warm_call_site_resolves_anew_after_a_reload", [this]() {
            struct warm_case { const char* code; const char* setup; int64_t warm; const char* reload; const char* after_call; int64_t after; };
            const warm_case cases[] = {
                {"Util::ver()", "class Util { static int ver() { return 1; } }", 1, "class Util { static int ver() { return 2; } }", "Util::ver()", 2},
                {"g.value()", "class Greeter { int value() { return 1; } } auto g = Greeter();", 1, "class Greeter { int value() { return 2; } }", "g.value()", 2},
                {"mc.f();", "class MC { int v = 0; int f() { return 1; } } auto mc = MC();", 1, "class MC { int v = 0; int f() { return 2; } }", "mc.f();", 2},
                {"od.f();", "class OvBase { int f() { return 1; } } class OvDer : OvBase { } auto od = OvDer();", 1,
                 "class OvDer : OvBase { int f() override { return 5; } }", "od.f();", 5},
                {"gd.f();", "class GBase { int f() { return 1; } } class GDer : GBase { } auto gd = GDer();", 1,
                 "class GBase { int f() { return 1; } int g() { return 9; } }", "gd.g();", 9},
            };
            check_each(cases, [](const warm_case& c) {
                auto eng = make_engine();
                eng->execute(c.setup);
                check_eq(c.warm, eng->execute(c.code).as_int());
                check_eq(c.warm, eng->execute(c.code).as_int());
                eng->execute(c.reload);
                check_eq(c.after, eng->execute(c.after_call).as_int());
            });
        });

        // Constructor overloads (arity-dispatched) must survive a reload. (Script *methods*
        // don't overload by arity — that's default-parameter territory — so the review's
        // "overload collapse" only applies to constructors here.)
        test("constructor_overload_survives_reload", [&]() {
            auto eng = make_engine();
            const char* def = R"(
                class Pt {
                    int x = 0;
                    int y = 0;
                    Pt(int a) { x = a; }
                    Pt(int a, int b) { x = a; y = b; }
                }
            )";
            eng->execute(def);
            check_eq(eng->execute("Pt(5).x").as<int>(), 5);
            check_eq(eng->execute("Pt(3, 4).y").as<int>(), 4);
            eng->execute(def); // reload, identical
            check_eq(eng->execute("Pt(7).x").as<int>(), 7);     // bug would collapse to 2-arg ctor
            check_eq(eng->execute("Pt(1, 2).y").as<int>(), 2);
        });

        // A script class extending a C++ (dynamic_binder) base holds the live C++ object in a
        // runtime _cpp_object field that never appears in the declared field defaults. A
        // field-changing reload must carry it across, so the inherited C++ method stays callable
        // and the C++ object is neither destructed nor rebuilt.
        test("cpp_object_survives_field_changing_reload", [&]() {
            auto eng = make_engine();
            const int base_live = ReloadCppBase::live_instances;

            dynamic_binder<ReloadCppBase>(*eng, "ReloadCppBase")
                .constructor<int>()
                .method("payload_value", &ReloadCppBase::payload_value)
                .build();

            eng->execute(R"(
                class Wrapper : ReloadCppBase {
                    int extra = 0;
                    Wrapper(int v) : super(v) {}
                }
                auto w = Wrapper(7);
                w.extra = 3;
            )");
            check_eq(eng->execute("w.payload_value()").as<int>(), 7);   // inherited C++ method works
            check_eq(ReloadCppBase::live_instances - base_live, 1);     // exactly one C++ base alive

            eng->execute(R"(
                class Wrapper : ReloadCppBase {
                    int extra = 0;
                    int added = 9;
                    Wrapper(int v) : super(v) {}
                }
            )"); // field-changing reload (gains `added`)

            check_eq(eng->execute("w.payload_value()").as<int>(), 7);   // _cpp_object preserved
            check_eq(eng->execute("w.added").as<int>(), 9);             // new field present
            check_eq(eng->execute("w.extra").as<int>(), 3);             // existing data kept
            check_eq(ReloadCppBase::live_instances - base_live, 1);     // not destructed, not rebuilt
        });

        // A reentrant execute (host callback firing mid-script) must define at TOP level:
        // redefining a function that is currently on the call stack replaces the global
        // binding (old frames finish with their old bodies) and the binding persists.
        test("function_redefined_mid_call_persists", [&]() {
            auto eng = make_engine();
            jai::engine* raw = eng.get();
            eng->add_function("swap_fn", [raw]() {
                raw->execute("function f(n) { return 1000; }");
            });
            eng->add_function("swap_other", [raw]() {
                raw->execute("function g(n) { return 2000; }");
            });
            // A different, not-on-stack function defined mid-call persists
            eng->execute("function fo(n) { if (n == 2) { swap_other(); } if (n <= 0) { return 0; } return fo(n - 1) + 1; }");
            check_eq((int64_t)4, eng->execute("fo(4)").as_int());
            check_eq((int64_t)2000, eng->execute("g(1)").as_int());
            // Redefining the RUNNING function replaces the global binding: old frames
            // finish with their old bodies, recursion resolves the new one, and the
            // new definition persists
            eng->execute("function f(n) { if (n == 2) { swap_fn(); } if (n <= 0) { return 0; } return f(n - 1) + 1; }");
            check_eq((int64_t)1003, eng->execute("f(4)").as_int());
            check_eq((int64_t)1000, eng->execute("f(1)").as_int());
            check_eq((int64_t)1000, eng->execute("f(4)").as_int());
        });

        test("self_redefinition_mid_call_persists_after_return", [&]() {
            auto eng = make_engine();
            jai::engine* raw = eng.get();
            eng->add_function("swap_min", [raw]() {
                raw->execute("function m(n) { return 77; }");
            });
            eng->execute("function m(n) { if (n == 1) { swap_min(); return m(0); } return 5; }");
            check_eq((int64_t)77, eng->execute("m(1)").as_int());
            // The interpreter used to lose the global binding entirely here
            check_eq((int64_t)77, eng->execute("m(0)").as_int());
        });

        // Class reloaded while one of its methods is mid-recursion: the continuing old
        // frame consistently fails to resolve the sibling method (pinned quirk on both
        // backends), but afterwards the instance, the class and new instances must all
        // stay alive and resolve the NEW body.
        test("class_reload_mid_recursion_keeps_globals_coherent", [&]() {
            auto eng = make_engine();
            jai::engine* raw = eng.get();
            eng->add_function("swap_rec", [raw]() {
                raw->execute("class R { int rec(int k) { if (k <= 0) { return 0; } return rec(k - 1) + 100; } }");
            });
            eng->execute(R"(
                class R { int rec(int k) { if (k == 2) { swap_rec(); } if (k <= 0) { return 0; } return rec(k - 1) + 1; } }
                auto r = R();
            )");
            try { eng->execute("r.rec(4)"); } catch (const std::exception&) {}
            check_eq((int64_t)400, eng->execute("r.rec(4)").as_int());
            check_eq((int64_t)100, eng->execute("auto r2 = R(); r2.rec(1);").as_int());
        });

        // A reentrant execute must not clobber the suspended outer run's environment:
        // the VM used to lose the method-scope env (and with it 'this') after a host
        // callback executed script mid-ctor/mid-method.
        test("reentrant_execute_preserves_this_in_ctor_and_method", [&]() {
            auto eng = make_engine();
            jai::engine* raw = eng.get();
            eng->add_function("reenter", [raw]() { raw->execute("g_r = g_r + 1;"); });
            eng->execute("var g_r = 0;");
            auto ctor_result = eng->execute(R"(
                class W2 {
                    int x = 0;
                    W2(int n) { reenter(); x = n; }
                    int bump(int n) { reenter(); x = x + n; return x; }
                }
                var w = W2(3);
                `${w.x}:${g_r}`;
            )");
            check_eq(std::string("3:1"), ctor_result.as<std::string>());
            check_eq((int64_t)10, eng->execute("w.bump(7)").as_int());
            check_eq((int64_t)2, eng->execute("g_r").as_int());
        });

        // Same failure surfaced silently when the ctor ran as a bind-time conversion:
        // the VM swallowed the broken ctor and reported "no suitable constructor".
        test("reentrant_execute_inside_conversion_ctor", [&]() {
            auto eng = make_engine();
            jai::engine* raw = eng.get();
            eng->add_function("reenter2", [raw]() { raw->execute("g_c = g_c + 1;"); });
            eng->execute("var g_c = 0;");
            eng->execute(R"(
                class W3 { int x = 0; W3(int n) { reenter2(); x = n; } }
                function wants3(W3 w) -> int { return w.x; }
            )");
            check_eq((int64_t)5, eng->execute("wants3(5)").as_int());
            check_eq((int64_t)1, eng->execute("g_c").as_int());
        });

        // A nested execute's uncaught throw belongs to the C++ host: once the host
        // swallows it, the suspended outer run must resume clean. The interpreter used
        // to skip the exception-state scrub on reentrant prepare_for_execution, so the
        // nested exception leaked into (and killed, or teleported into a catch of) the
        // outer run.
        test("reentrant_execute_swallowed_throw_does_not_leak_into_outer_run", [&]() {
            auto eng = make_engine();
            jai::engine* raw = eng.get();
            bool host_caught = false;
            eng->add_function("swallow", [raw, &host_caught]() {
                try { raw->execute("throw \"nested boom\";"); }
                catch (const std::exception&) { host_caught = true; }
            });
            check_eq((int64_t)42, eng->execute("var ok = 0; swallow(); ok = 42; ok;").as_int());
            check_true(host_caught);
            // Not catchable by an unrelated outer try/catch either
            check_eq(std::string("none"),
                eng->execute("var got = \"none\"; try { swallow(); } catch (e) { got = `${e}`; } got;").as<std::string>());
            check_eq((int64_t)7, eng->execute("3 + 4").as_int());
        });

        // ===== Method-dispatch cache invalidation pins (per-site inline cache era) =====
        // The engine's source cache reuses the SAME AST across identical execute()
        // strings, so a repeated call string exercises one call site's cache across
        // reloads - every invalidation edge below must resolve fresh.

        // Redefinition INSIDE a loop: the loop-body call site flips mid-loop.
        test("method_cache_reload_mid_loop", [&]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                class ML { int f() { return 1; } }
                auto m = ML();
                auto acc = 0;
                for (auto i = 0; i < 4; i += 1) {
                    acc = acc * 10 + m.f();
                    if (i == 1) {
                        class ML { int f() { return 2; } }
                    }
                }
                acc;
            )");
            check_eq((int64_t)1122, result.as_int());
        });

        // Method removed on reload: the hot call site must error, not run the old body.
        test("method_cache_method_removed", [&]() {
            auto eng = make_engine();
            eng->execute("class RM { int f() { return 1; } int g() { return 2; } } auto rm = RM();");
            check_eq((int64_t)2, eng->execute("rm.g();").as_int());
            check_eq((int64_t)2, eng->execute("rm.g();").as_int());
            eng->execute("class RM { int f() { return 1; } }");
            check_throws([&]() { auto r = eng->execute("rm.g();"); if (r.is_null()) throw std::runtime_error("member gone"); },
                         "removed method must not dispatch from a stale cache");
        });

        // A field with the method's name arriving on reload shadows the method
        // (field precedence) - a cached method dispatch must not bypass it.
        test("method_cache_field_shadows_method_after_reload", [&]() {
            auto eng = make_engine();
            eng->execute("class FS { int f() { return 1; } } auto fs = FS();");
            check_eq((int64_t)1, eng->execute("fs.f();").as_int());
            check_eq((int64_t)1, eng->execute("fs.f();").as_int());
            eng->execute("class FS { int f = 3; }");   // reload: f becomes a FIELD
            check_throws([&]() { auto r = eng->execute("fs.f();"); if (r.is_null()) throw std::runtime_error("not callable"); },
                         "field must shadow the stale cached method");
            check_eq((int64_t)3, eng->execute("fs.f;").as_int());
        });

        // Private methods stay denied even after the class's public methods have made
        // the class hot in dispatch caches.
        test("method_cache_private_stays_denied", [&]() {
            auto eng = make_engine();
            eng->execute(R"(
                class PV {
                    int pub() { return secret() + 1; }
                private:
                    int secret() { return 10; }
                }
                auto pv = PV();
            )");
            check_eq((int64_t)11, eng->execute("auto s = 0; for (auto i = 0; i < 50; i += 1) { s = pv.pub(); } s;").as_int());
            check_throws([&]() { auto r = eng->execute("pv.secret();"); if (r.is_null()) throw std::runtime_error("denied"); },
                         "private method must stay denied through hot dispatch caches");
        });

        // Same call site serving two receiver classes (polymorphic site): each receiver
        // resolves its own method.
        test("method_cache_polymorphic_site", [&]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                class PA { int f() { return 1; } }
                class PB { int f() { return 2; } }
                function callf(var o) -> int { return o.f(); }
                auto pa = PA();
                auto pb = PB();
                auto acc = 0;
                for (auto i = 0; i < 3; i += 1) {
                    acc = acc * 10 + callf(pa);
                    acc = acc * 10 + callf(pb);
                }
                acc;
            )");
            check_eq((int64_t)121212, result.as_int());
        });

        // Typed single-overload method called with a convertible (not exact) argument
        // after the site is hot on exact arguments: conversion semantics unchanged.
        test("method_cache_arg_type_switch", [&]() {
            auto eng = make_engine();
            eng->execute("class AT { int g(int x) { return x * 2; } } auto at = AT();");
            check_eq((int64_t)4, eng->execute("at.g(2);").as_int());
            check_eq((int64_t)4, eng->execute("at.g(2);").as_int());     // hot on int
            check_eq((int64_t)2, eng->execute("at.g(true);").as_int());  // bool converts, same site
            check_eq(std::string("Cannot pass float to parameter 'x' of 'g' (declared int)"),
                     eng->execute("var fl = 2.4; var refused = \"\"; try { at.g(fl); } catch (e) { refused = e; } refused;").to_string());
        });

        // ---- parse-time type proofs across reloads ----
        // Field proofs armed by three executes (first execute -> stamped -> identical-reload
        // hit) must miss safely once a reload changes the field's facts: every step compares a
        // type_proofs(false) twin on the same backend, never a literal alone.
        struct proof_twins { std::shared_ptr<engine> on, off; };
        auto make_twins = []() {
            proof_twins t{make_engine(), make_engine()};
            t.off->type_proofs(false);
            jai::stdlib::register_all(t.on);
            jai::stdlib::register_all(t.off);
            return t;
        };
        auto outcome = [](engine& e, const std::string& src) -> std::string {
            try { return "VALUE " + e.execute(src).to_string(); }
            catch (const std::exception& ex) { return std::string("ERROR ") + ex.what(); }
        };
        auto step = [this, outcome](proof_twins& t, const std::string& src, const std::string& what) {
            const std::string on = outcome(*t.on, src);
            check_eq(outcome(*t.off, src), on, what);
            return on;
        };
        auto arm = [step](proof_twins& t, const std::string& unit, const std::string& use) {
            for (int pass = 0; pass < 3; ++pass) {
                step(t, unit, "arming unit pass " + std::to_string(pass));
                step(t, use, "arming use pass " + std::to_string(pass));
            }
        };
        const std::string show_int = "  string show() { return to_string(f) + \":\" + to_string(this.f) + \":\" + type_of(f); } }";
        const std::string v1_int =
            "class R { int f = 1;\n"
            "  int set(int v) { f = v; this.f = v; return f; }\n" + show_int + "\n"
            "R r = R(); r.set(41); r.f = 42; r.show()";
        const std::string use_int = "r.set(7); r.f = 9; r.show()";
        const std::string v2_string =
            "class R { string f = \"z\";\n"
            "  int set(int v) { f = to_string(v); this.f = to_string(v); return v; }\n"
            "  string show() { return f + \":\" + this.f + \":\" + type_of(f); } }";
        const std::string v2_var =
            "class R { var f = 0;\n"
            "  var set(var v) { f = v; this.f = v; return f; }\n" + show_int;
        const std::string v2_private =
            "class R {\n private: int f = 1;\n public:\n"
            "  int set(int v) { f = v; this.f = v; return f; }\n" + show_int;

        test("proof_field_retype_int_to_string_assigns", [this, step, arm, make_twins, v1_int, use_int, v2_string]() {
            auto t = make_twins();
            arm(t, v1_int, use_int);
            step(t, v2_string, "reload int -> string");
            check_eq(std::string("VALUE 9:9:string"), step(t, "r.show()", "live value converted"));
            check_eq(std::string("VALUE 7:7:string"), step(t, "r.set(7); r.show()", "the reloaded body stores its text"));
            const std::string stale = step(t, use_int, "the armed use source stores an int into the string field");
            check(stale.rfind("ERROR", 0) == 0, "a string field takes no int, identically on both twins: " + stale);
            check_eq(std::string("VALUE 5:5:string"), step(t, "r.f = \"5\"; r.show()", "member store of text"));
            step(t, v1_int, "the armed unit reloads the class back to int");
            check_eq(std::string("VALUE 9:9:int"), step(t, use_int, "use after reloading back"));
        });

        test("proof_field_retype_float_to_int_truncates", [this, step, arm, make_twins]() {
            auto t = make_twins();
            const std::string v1 =
                "class R { float f = 1.5;\n"
                "  float set(float v) { f = v; this.f = v; return f; }\n"
                "  string show() { return to_string(f) + \":\" + to_string(this.f) + \":\" + type_of(f); } }\n"
                "R r = R(); r.set(4.5); r.f = 6.75; r.show()";
            const std::string use = "r.set(7.5); r.f = 9.25; r.show()";
            arm(t, v1, use);
            step(t, "class R { int f = 0;\n"
                    "  int set(float v) { f = cast<int>(v); this.f = cast<int>(v); return f; }\n"
                    "  string show() { return to_string(f) + \":\" + to_string(this.f) + \":\" + type_of(f); } }", "reload float -> int");
            check_eq(std::string("VALUE 9:9:int"), step(t, "r.show()", "live value truncated"));
            check_eq(std::string("VALUE 7:7:int"), step(t, "r.set(7.5); r.show()", "cast stores land through the ladder"));
            const std::string armed = step(t, use, "the armed use pushes a float into the int field");
            check_eq(std::string("ERROR Cannot assign float to field 'f' of R (declared int)"), armed);
            check_eq(std::string("VALUE 7:7:int"), step(t, "r.show()", "the refused store left the field"));
        });

        test("proof_field_retype_string_to_int_resets_default", [this, step, arm, make_twins]() {
            auto t = make_twins();
            const std::string v1 =
                "class R { string f = \"a\";\n"
                "  string set(string v) { f = v; this.f = v; return f; }\n"
                "  string show() { return f + \":\" + this.f + \":\" + type_of(f); } }\n"
                "R r = R(); r.set(\"x\"); r.f = \"hello\"; r.show()";
            const std::string use = "r.set(\"k\"); r.f = \"hello\"; r.show()";
            arm(t, v1, use);
            step(t, "class R { int f = 5;\n"
                    "  int set(int v) { f = v; this.f = v; return f; }\n"
                    "  string show() { return to_string(f) + \":\" + to_string(this.f) + \":\" + type_of(f); } }", "reload string -> int");
            check_eq(std::string("VALUE 5:5:int"), step(t, "r.show()", "unconvertible value adopts the new default"));
            const std::string armed = step(t, use, "the armed use pushes strings into the int field");
            check(armed.rfind("ERROR", 0) == 0, "raises identically on both twins: " + armed);
            check_eq(std::string("VALUE 9:9:int"), step(t, "r.set(9); r.show()", "int stores land"));
            const std::string bad = step(t, "r.f = \"nope\"; r.show()", "string into the retyped int field");
            check(bad.rfind("ERROR", 0) == 0, "raises identically on both twins: " + bad);
        });

        test("proof_field_retype_int_to_var_accepts_anything", [this, step, arm, make_twins, v1_int, use_int, v2_var]() {
            auto t = make_twins();
            arm(t, v1_int, use_int);
            step(t, v2_var, "reload int -> var");
            check_eq(std::string("VALUE 9:9:int"), step(t, "r.show()", "value kept"));
            check_eq(std::string("VALUE s:s:string"), step(t, "r.set(\"s\"); r.show()", "var field takes a string"));
            check_eq(std::string("VALUE 2.5:2.5:float"), step(t, "r.f = 2.5; r.show()", "var field takes a float"));
            check_eq(std::string("VALUE 9:9:int"), step(t, use_int, "the armed use source"));
        });

        test("proof_field_removed_then_read_errors_like_unproven", [this, step, arm, make_twins, v1_int, use_int]() {
            auto t = make_twins();
            arm(t, v1_int, use_int);
            step(t, "class R { int g = 0; int set(int v) { g = v; return g; } string show() { return to_string(g); } }", "reload drops f");
            const std::string read = step(t, "r.f", "removed field read");
            check(read.rfind("ERROR", 0) == 0, "removed field read raises identically: " + read);
            step(t, "r.f = 3; r.show()", "removed field write");
            step(t, use_int, "the armed use source against the shrunken class");
            check_eq(std::string("VALUE 9"), step(t, "r.set(9); r.show()", "surviving method works"));
            step(t, "class R { int g = 0; int set(int v) { f = v; return g; } string show() { return to_string(g); } }", "reload keeps a body naming the dropped field");
            step(t, "r.set(4); r.show()", "bare write to the dropped field");
        });

        test("proof_field_gains_custom_getter_on_reload", [this, step, arm, make_twins, v1_int, use_int, show_int]() {
            auto t = make_twins();
            arm(t, v1_int, use_int);
            // a user _get_f added by the reload owns member reads (r.f, this.f); bare f stays the
            // field. The reload moves the shape epoch, so the armed proofs miss and the ladder
            // answers the same way on both twins
            step(t, "class R { int f = 1;\n  int _get_f() { return f * 10; }\n  int set(int v) { f = v; this.f = v; return f; }\n" + show_int, "reload adds _get_f");
            check_eq(std::string("VALUE 90"), step(t, "to_string(r.f)", "the reload's getter serves the member read"));
            check_eq(std::string("VALUE 9:90:int"), step(t, "r.show()", "bare f is the field, this.f the getter"));
            check_eq(std::string("VALUE 9:90:int"), step(t, use_int, "the armed use source"));
        });

        test("proof_field_gains_custom_setter_on_reload", [this, step, arm, make_twins, v1_int, use_int, show_int]() {
            auto t = make_twins();
            arm(t, v1_int, use_int);
            step(t, "class R { int f = 1; int log = 0;\n  void _set_f(int v) { log = log + 1; }\n  int set(int v) { f = v; this.f = v; return f; }\n" + show_int, "reload adds _set_f");
            check_eq(std::string("VALUE 9:1"), step(t, "r.f = 3; to_string(r.f) + \":\" + to_string(r.log)", "the reload's setter owns the member store (it only logs)"));
            check_eq(std::string("VALUE 7:7:int"), step(t, use_int, "bare f = v lands, this.f = v and r.f = 9 go to the setter"));
            check_eq(std::string("VALUE 3"), step(t, "to_string(r.log)", "three member stores logged"));
        });

        test("proof_field_access_label_change_on_reload", [this, step, arm, make_twins, v1_int, use_int, v2_private]() {
            auto t = make_twins();
            arm(t, v1_int, use_int);
            step(t, v2_private, "reload makes f private");
            const std::string denied = step(t, "r.f", "private field read from outside");
            check(denied.rfind("ERROR", 0) == 0, "access error identical on both twins: " + denied);
            const std::string denied_write = step(t, "r.f = 3", "private field write from outside");
            check(denied_write.rfind("ERROR", 0) == 0, "access error identical on both twins: " + denied_write);
            check_eq(std::string("VALUE 6:6:int"), step(t, "r.set(6); r.show()", "methods still reach the field"));
            const std::string armed = step(t, use_int, "the armed use source hits the access wall");
            check(armed.rfind("ERROR", 0) == 0, "armed member store denied identically: " + armed);
        });

        test("proof_base_reload_invalidates_derived_proofs", [this, step, arm, make_twins]() {
            auto t = make_twins();
            const std::string v1 =
                "class B { int bv = 1; }\n"
                "class D : B { int dv = 2;\n"
                "  int set(int v) { bv = v; this.bv = v; dv = v; return dv; }\n"
                "  string show() { return to_string(bv) + \":\" + to_string(this.bv) + \":\" + to_string(dv) + \":\" + type_of(bv); } }\n"
                "D d = D(); d.set(5); d.show()";
            const std::string use = "d.set(7); d.bv = 8; d.show()";
            arm(t, v1, use);
            step(t, "class B { string bv = \"s\"; int extra = 3; }", "base reload retypes bv and gains a field");
            check_eq(std::string("VALUE 8:8:7:string"), step(t, "d.show()", "inherited value converted on the derived instance"));
            const std::string stale = step(t, use, "armed derived stores of an int into the string field");
            check(stale.rfind("ERROR", 0) == 0, "a string field takes no int, identically on both twins: " + stale);
            check_eq(std::string("VALUE 3"), step(t, "to_string(d.extra)", "base gained a field"));
            const std::string as_int = step(t, "class E : B { int get() { return bv; } } E e = E(); e.get()", "a derived body returning the string field as int");
            check(as_int.rfind("ERROR", 0) == 0, "raises identically on both twins: " + as_int);
            step(t, "class B { int bv = 1; }", "base back to int");
            check_eq(std::string("VALUE 1"), step(t, "to_string(e.get())", "unconvertible string adopts the int default"));
            check_eq(std::string("VALUE 8:8:7:int"), step(t, use, "armed derived stores enforce int again"));
        });

        test("proof_class_gains_base_on_reload", [this, step, arm, make_twins, v1_int, use_int, show_int]() {
            auto t = make_twins();
            arm(t, v1_int, use_int);
            step(t, "class Base0 { int z = 9; }\nclass R : Base0 { int f = 1;\n  int set(int v) { f = v; this.f = v; return f; }\n" + show_int, "reload gives R a base");
            step(t, "r.show()", "existing instance under the new chain");
            step(t, "to_string(r.z)", "inherited field on the migrated instance");
            check_eq(std::string("VALUE 9:9:int"), step(t, use_int, "the armed use source"));
            check_eq(std::string("VALUE 3:3:int"), step(t, "R n = R(); n.set(2); n.f = 3; n.show()", "fresh instance"));
        });

        test("proof_class_becomes_open_on_reload", [this, step, arm, make_twins, v1_int, use_int, show_int]() {
            // a C++ base makes the chain open to the proof pass; instances armed under the
            // closed class keep answering like the twin
            auto t = make_twins();
            for (auto* e : {t.on.get(), t.off.get()}) {
                dynamic_binder<ReloadCppBase>(*e, "ReloadCppBase")
                    .constructor<int>()
                    .method("payload_value", &ReloadCppBase::payload_value)
                    .build();
            }
            arm(t, v1_int, use_int);
            step(t, "class R : ReloadCppBase { int f = 1; R() : super(0) {}\n  int set(int v) { f = v; this.f = v; return f; }\n" + show_int, "reload gives R a C++ base");
            step(t, "r.show()", "pre-reload instance under the hybrid class");
            step(t, use_int, "the armed use source under the hybrid class");
            check_eq(std::string("VALUE 3:3:int"), step(t, "R h = R(); h.set(2); h.f = 3; h.show()", "fresh hybrid instance"));
            check_eq(std::string("VALUE 5:5:int"), step(t, "h.set(4); h.f = 5; h.show()", "hybrid stores"));
        });

        test("proof_same_source_reexecute_vm_chunk_reuse", [this, step, arm, make_twins, v1_int, use_int, v2_string, v2_var, v2_private]() {
            // ONE use string executed again and again (the vm reuses its compiled chunk)
            // while the class reloads underneath it
            auto t = make_twins();
            arm(t, v1_int, use_int);
            step(t, v2_string, "reload -> string");
            for (int pass = 0; pass < 3; ++pass) {
                const std::string stale = step(t, use_int, "reused use after the string retype " + std::to_string(pass));
                check(stale.rfind("ERROR", 0) == 0, "an int store into the string field raises identically: " + stale);
            }
            step(t, v1_int, "reload -> int");
            for (int pass = 0; pass < 3; ++pass) { check_eq(std::string("VALUE 9:9:int"), step(t, use_int, "reused use after reloading back " + std::to_string(pass))); }
            step(t, v2_var, "reload -> var");
            for (int pass = 0; pass < 2; ++pass) { check_eq(std::string("VALUE 9:9:int"), step(t, use_int, "reused use after the var retype " + std::to_string(pass))); }
            step(t, v2_private, "reload -> private");
            for (int pass = 0; pass < 2; ++pass) {
                const std::string denied = step(t, use_int, "reused use against the access wall " + std::to_string(pass));
                check(denied.rfind("ERROR", 0) == 0, "denied identically: " + denied);
            }
        });

        test("proof_extend_reclass_misses", [this, step, arm, make_twins]() {
            // extend(obj, Class) re-classes the instance in place: the class pointer guard
            // misses and the ladder resolves against the new class on both twins
            auto t = make_twins();
            const std::string v1 =
                "class Pt { int a = 1;\n"
                "  int set(int v) { a = v; this.a = v; return a; }\n"
                "  string show() { return to_string(a) + \":\" + to_string(this.a); } }\n"
                "class Pt3 : Pt { int b = 2; Pt3() : super() {} }\n"
                "Pt p = Pt(); p.set(5); p.show()";
            const std::string use = "p.set(6); p.a = 7; p.show()";
            arm(t, v1, use);
            step(t, "extend(p, Pt3); p.show() + \":\" + to_string(p.b)", "re-classed in place");
            check_eq(std::string("VALUE 7:7"), step(t, use, "the armed use source on the re-classed receiver"));
            step(t, "p.b = 9; p.set(1); p.show() + \":\" + to_string(p.b)", "stores after the re-class");
        });

        // ---- Static state under a retype (existing rules, pinned): the value survives
        // verbatim (no initializer re-run, no conversion), the NEW declared type governs
        // every later store, a dropped static disappears, a static NEW in the reload runs
        // its initializer through the field table. ----
        test("static_retype_on_reload_keeps_value_new_type_governs_stores", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto run = [&](const char* src) -> std::string {
                try { return eng->execute(src).to_string(); }
                catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
            };
            run("function L(var x) { return x; } class R { static int s = 0; } R::s = 7;");
            run("class R { static string s = \"init\"; }");
            check_eq(std::string("7:int"), run("to_string(R::s) + \":\" + type_of(R::s)"), "value preserved verbatim, old type");
            check_true(run("R::s = 9;").find("ERROR") == 0, "the new declared type governs stores: a string takes no int");
            check_eq(std::string("9:string"), run("R::s = \"9\"; R::s + \":\" + type_of(R::s)"), "the new declared type governs stores");
            run("class R { static int s = 0; }");
            check_eq(std::string("9:string"), run("to_string(R::s) + \":\" + type_of(R::s)"), "reload back preserves the string verbatim");
            const std::string err = run("R::s = L(\"zz\");");
            check_true(err.find("Cannot assign string to static 's' of R (declared int)") != std::string::npos, "unconvertible store raises under the new type (got: " + err + ")");
            check_eq(std::string("9:string"), run("to_string(R::s) + \":\" + type_of(R::s)"), "static untouched by the failed store");
            check_eq(std::string("4:int"), run("R::s = 4; to_string(R::s) + \":\" + type_of(R::s)"), "int governs again");
            run("class R { static int s = 0; }");
            check_eq(std::string("4"), run("to_string(R::s)"), "identical redefinition keeps the value");
            run("class R { static int s = 0; static float g = 3; }");
            check_eq(std::string("float:3.0"), run("type_of(R::g) + \":\" + to_string(R::g)"), "a static new in the reload converts its initializer");
            check_eq(std::string("4"), run("to_string(R::s)"), "the existing static kept its value");
            run("class R { static float g = 3; }");
            check_true(run("R::s").find("ERROR") == 0, "a dropped static is gone");
        });

        // A by-reference argument site remembers that its member is a plain script field of the receiver's class; a reload that
        // gives the field an accessor or another declared type, and a receiver of another class, take the full ladder again
        test("reference_argument_site_follows_reload_and_receiver_class", [&]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto run = [&](const char* src) -> std::string {
                try { return eng->execute(src).to_string(); }
                catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
            };
            run(R"(
                class P { int x = 1; }
                class Q { float x = 0.5; int pad = 0; }
                void bump(var& r) { r = r + 1; }
                void nudge(var& r) { r = r + 1.75; }
                function go(var& o) -> string { for (int i = 0; i < 3; i++) { bump(o.x); } return to_string(o.x); }
                function go_float(var& o) -> string { for (int i = 0; i < 3; i++) { nudge(o.x); } return to_string(o.x); }
                P p = P(); Q q = Q();
            )");
            check_eq(std::string("4"), run("go(p)"), "int field: each store lands as an int");
            check_eq(std::string("3.5"), run("go(q)"), "another class at the same site: its float field");
            check_eq(std::string("7"), run("go(p)"), "back to the first class");
            check_eq(std::string("ERROR: Cannot assign float to field 'x' of P (declared int)"), run("go_float(p)"), "int field: a float store is refused");
            check_eq(std::string("8.75"), run("go_float(q)"), "the float field at that site takes it");
            run("class P { float x = 1; }");
            check_eq(std::string("12.25"), run("go_float(p)"), "a reload that declares the field float governs the next binds");
            check_eq(std::string("15.25"), run("go(p)"), "and an int store widens into it");
            run("class P { float x = 1; float _get_x() { return x; } }");
            const std::string err = run("go(p)");
            check_true(err.find("Cannot pass non-lvalue to reference parameter") != std::string::npos, "a user accessor makes the member a non-lvalue (got: " + err + ")");
            run("class P { float x = 1; }");
            check_eq(std::string("18.25"), run("go(p)"), "dropping the accessor binds the field again");
            run(R"(
                class Base { int a = 0; }
                class D1 : Base { int d1 = 1; }
                void poke(int& r, Base& x) { x = Base(); r = 5; }
                function f() -> string {
                    Base x = D1(); string out = "";
                    for (int i = 0; i < 3; i++) { x = D1(); D1& dx = cast<D1&>(x); try { poke(dx.d1, x); out = out + "no"; } catch (e) { out = out + e + "/"; } }
                    return out;
                }
            )");
            check_eq(std::string("Reference to a removed field/Reference to a removed field/Reference to a removed field/"), run("f()"),
                "a bound member the callee's re-class drops raises at every bind");
        });
    }
};

FOUNDRY_REGISTER(jai::foundry::tests::hot_reload_correctness_tests)

} // namespace jai::foundry::tests
