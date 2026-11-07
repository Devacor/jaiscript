#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"
#include <filesystem>
#include <fstream>

using namespace jai::foundry;

namespace jai::foundry::tests {

class function_tests : public suite {
public:
    function_tests() : suite("Function Tests") {}
    
    void forge_tests() override {
        // A named function declared in a function body captures nothing: an enclosing frame local is
        // a parse error on both backends; globals stay visible, and function(...) / => keep their [&] lambda semantics
        test("nested_named_function_captures_nothing", [this]() {
            for (const char* source : {
                "var y = 100; int g() { int y = 5; int helper(int x) { return x + y; } return helper(1); } g();",
                "int g() { int y = 5; void helper() { y = 6; } helper(); return y; } g();",
                "int g(int y) { int helper() { return [&]() { return y; }(); } return helper(); } g(5);"}) {
                auto engine = make_engine();
                std::string message;
                try { engine->execute(source); } catch (const std::exception& e) { message = e.what(); }
                check_true(message.find("nested function 'helper' cannot use enclosing local 'y'") != std::string::npos, message);
            }
            auto engine = make_engine();
            check_eq(script_int(101), engine->execute("var y = 100; int g() { int helper(int x) { return x + y; } return helper(1); } g();").as_int());
            check_eq(script_int(10), engine->execute("int r(int n) { int rec(int k) { if (k == 0) { return 0; } return rec(k - 1) + 1; } return rec(n); } r(10);").as_int());
            check_eq(script_int(6), engine->execute("int g() { int y = 5; var helper = function(int x) { return x + y; }; return helper(1); } g();").as_int());
            check_eq(script_int(6), engine->execute("int g() { int y = 5; var helper = (int x) => x + y; return helper(1); } g();").as_int());
        });

        test("functions_take_parameters_recurse_and_return_their_value", [this]() {
            const std::pair<const char*, int> cases[] = {
                {"auto add(auto a, auto b) -> auto { return a + b; } auto result = add(5, 3); result;", 8},
                {"auto getValue() -> auto { return 42; } getValue();", 42},
                {"auto x = 10; auto setX(auto value) -> auto { x = value; } setX(25); x;", 25},
                {"function multiply(auto a, auto b, auto c) -> auto { return a * b * c; } multiply(2, 3, 4);", 24},
                {"function factorial(auto n) -> auto { if (n <= 1) { return 1; } return n * factorial(n - 1); } factorial(5);", 120},
                {"function double(auto x) -> auto { return x * 2; } function add(auto a, auto b) -> auto { return a + b; } double(add(3, 4));", 14},
                {"function compute(auto n) -> auto { auto temp = n * 2; auto result = temp + 5; return result; } compute(10);", 25},
                {"function max(auto a, auto b) -> auto { if (a > b) { return a; } else { return b; } } max(15, 10);", 15},
                {"function findFirst(auto n) -> auto { for (auto i = 0; i < 10; ++i) { if (i == n) { return i; } } return -1; } findFirst(5);", 5},
                {"auto global_var = 100; function test_scope() -> auto { auto local_var = 50; return local_var + global_var; } test_scope();", 150},
                {"auto x = 10; function shadow_test(auto x) -> auto { return x * 2; } shadow_test(5);", 10},
                {"auto cpp_style(auto x) -> auto { return x + 1; } function func_style(auto x) -> auto { return x + 2; } "
                 "function short_style(auto x) -> auto { return x + 3; } cpp_style(10) + func_style(10) + short_style(10);", 36},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                script_value result = engine->execute(c.first);
                check(result.is_int());
                check_eq(result.as<int>(), c.second);
            });
        });

        test("function_returning_string", [this]() {
            auto engine = make_engine();
            
            std::string script = R"(
                function greeting(auto name) -> auto {
                    return "Hello, " + name + "!";
                }
                
                greeting("World");
            )";
            
            script_value result = engine->execute(script);
            check(result.is_string());
            check_eq(result.as<std::string>(), "Hello, World!");
        });

        test("default_parameters_fill_the_missing_trailing_arguments", [this]() {
            const std::pair<const char*, int64_t> cases[] = {
                {"function add(int a, int b = 10) -> int { return a + b; } add(5);", 15},
                {"function add(int a, int b = 10) -> int { return a + b; } add(5, 20);", 25},
                {"function make_point(int x = 0, int y = 0, int z = 0) -> int { return x + y + z; } make_point() + make_point(1) + make_point(1, 2) + make_point(1, 2, 3);", 0 + 1 + 3 + 6},
            };
            check_each(cases, [&](const auto& c) {
                auto eng = make_engine();
                jai::stdlib::register_all(eng);
                check_eq(eng->execute(c.first).template as<int64_t>(), c.second);
            });
        });

        test("default_parameter_string", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto result = eng->execute(R"(
                function greet(string name, string greeting = "hello") -> string {
                    return greeting + " " + name;
                }
                greet("world");
            )");
            check_eq(result.as<std::string>(), std::string("hello world"));
        });

        test("enum_basic", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto result = eng->execute(R"(
                enum Direction { north, south, east, west }
                Direction.east;
            )");
            check_eq(result.as<int64_t>(), int64_t(2));
            // C++ enumerators: an initializer is a constant integer over literals and earlier values, the rest count on by one
            auto values = eng->execute(R"(
                enum Flag { none, read = 1 << 0, write = 1 << 1, exec = 1 << 2, all = read | write | exec, next }
                enum Level { low = -2, mid, high = Level::mid + 10, letter = 'A', twin = letter }
                to_string([Flag.none, Flag.read, Flag.write, Flag.exec, Flag.all, Flag.next]) + " " + to_string([Level.low, Level.mid, Level.high, Level.letter, Level.twin]);
            )");
            check_eq(std::string("[0, 1, 2, 4, 7, 8] [-2, -1, 9, 65, 65]"), values.as<std::string>());
        });

        test("enum_comparison", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto result = eng->execute(R"(
                enum Color { red, green, blue }
                auto c = Color.green;
                c == Color.green;
            )");
            check_eq(result.as<bool>(), true);
        });

        test("enum_in_switch", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto result = eng->execute(R"(
                enum Weapon { sword, bow, staff }
                auto w = Weapon.bow;
                auto damage = 0;
                switch (w) {
                    case Weapon.sword: damage = 10;
                    case Weapon.bow: damage = 8;
                    case Weapon.staff: damage = 12;
                }
                damage;
            )");
            check_eq(result.as<int64_t>(), int64_t(8));
        });

        test("null_safe_on_null", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto result = eng->execute(R"(
                auto obj = null;
                obj?.toString();
            )");
            check(result.is_null());
        });

        test("null_safe_on_value", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto result = eng->execute(R"(
                auto s = "hello";
                s?.length();
            )");
            check_eq(result.as<int64_t>(), int64_t(5));
        });

        test("null_safe_chain", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto result = eng->execute(R"(
                auto obj = null;
                auto x = obj?.toString();
                x == null;
            )");
            check_eq(result.as<bool>(), true);
        });

        test("null_member_call_names_receiver", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            std::string message;
            try { eng->execute("auto obj = null; obj.frob();"); } catch (const std::exception& e) { message = e.what(); }
            check(message.find("Cannot call 'frob' on null") != std::string::npos);
            check(message.find("'obj' is null") != std::string::npos);
            message.clear();
            try { eng->execute("auto maker = []() -> var { return null; }; maker().frob();"); } catch (const std::exception& e) { message = e.what(); }
            check(message.find("'maker(...)' returned null") != std::string::npos);
            message.clear();
            try { eng->execute("class Box { var inner = null; } auto box = Box(); box.inner.frob();"); } catch (const std::exception& e) { message = e.what(); }
            check(message.find("'inner' is null") != std::string::npos);
        });

        test("nocopy_class_refuses_by_value_copies", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto viaRef = eng->execute(R"(
                nocopy class Ledger { int total = 1; }
                auto ledger = Ledger();
                auto& same = ledger;
                same.total = 2;
                ledger.total == 2;
            )");
            check_eq(viaRef.as<bool>(), true);
            check_throws([&]() { eng->execute("auto copy = ledger;"); });
            check_throws([&]() { eng->execute("void take(Ledger l) {} take(ledger);"); });
            auto viaParam = eng->execute(R"(
                void poke(Ledger& l) { l.total = 3; }
                poke(ledger);
                ledger.total == 3;
            )");
            check_eq(viaParam.as<bool>(), true);
            std::string message;
            try { eng->execute("auto again = ledger;"); } catch (const std::exception& e) { message = e.what(); }
            check(message.find("nocopy class 'Ledger'") != std::string::npos);
        });

        test("nocopy_is_inherited", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(R"(
                nocopy class Ledger { int total = 1; }
                class Audit : Ledger { int checks = 0; }
                class Deep : Audit {}
                auto audit = Audit();
                auto deep = Deep();
            )");
            std::string message;
            try { eng->execute("auto copy = audit;"); } catch (const std::exception& e) { message = e.what(); }
            check(message.find("nocopy class 'Audit'") != std::string::npos, "a derived class is nocopy: " + message);
            message.clear();
            try { eng->execute("void take(Deep d) {} take(deep);"); } catch (const std::exception& e) { message = e.what(); }
            check(message.find("nocopy class 'Deep'") != std::string::npos, "two levels down still nocopy: " + message);
            check_eq(eng->execute("auto& same = deep; same.checks = 4; deep.checks == 4;").as<bool>(), true, "references still bind");
        });

        // Read-only by-value parameter elision: a by-value parameter the callee provably never
        // mutates, aliases, compares or uses whole binds the argument by handle instead of a
        // clone. engine::parameter_alias_binds() counts the binds; the semantics are the
        // clone's either way (every row below runs on both backends).
        test("readonly_parameter_alias_proven_shapes", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto r = eng->execute(R"(
                class Vec3 { float x = 0.0; float y = 0.0; float z = 0.0; string tag = "v"; array<int> ids = [1, 2, 3]; Vec3() {} Vec3(float a) { x = a; y = a * 2; z = a * 3; } }
                float xy(Vec3 v) { return v.x * v.y; }                       // member reads of primitives
                int idsum(Vec3 v) { int s = 0; for (int i = 0; i < v.ids.size(); i++) { s += v.ids[i]; } return s + v.tag.size(); }   // subscript + read-only builtins
                int total(array<int> xs) { int s = 0; for (int i = 0; i < xs.size(); i++) { s += xs[i]; } return s; }
                int lookup(map<string, int> m, string k) { return m.has(k) ? m[k] : -1; }
                float rebound(Vec3 v, Vec3 w) { float a = v.x; v = w; return a + v.x; }   // rebinding the slot, never the referent (w escapes: it is stored)
                Vec3 p = Vec3(2.0);
                array<int> nums = [4, 5, 6];
                map<string, int> table = {"a": 1, "b": 2};
                float acc = 0.0;
                for (int i = 0; i < 50; i++) { acc += xy(p) + idsum(p) + total(nums) + lookup(table, "b") + rebound(p, p); }
                return [acc, p.x, p.ids.size(), nums.size(), table.size()];
            )");
            const auto& v = r.as_array();
            check_near(50.0 * (8.0 + 7 + 15 + 2 + 4.0), v[0].as<double>(), 1e-9);
            check_near(2.0, v[1].as<double>(), 1e-12);
            check_eq((int64_t)3, v[2].as_int());
            check_eq((int64_t)250, (int64_t)detail::engine_access::parameter_alias_binds(*eng));   // five proven binds per iteration
        });

        test("readonly_parameter_alias_refused_shapes_keep_the_copy", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto r = eng->execute(R"(
                class C { int b = 0; C() {} }
                class P { int a = 1; array<int> xs = [1]; C child = C(); P() {} }
                P g = P();
                int mutates(P p) { p.a = p.a + 10; return p.a; }
                int pushes(P p) { p.xs.push(7); return p.xs.size(); }
                int compares(P p) { return p == g ? 1 : 0; }
                int child_compares(P p) { return p.child == g.child ? 1 : 0; }   // heavy terminal: identity-observable
                P returns(P p) { return p; }
                void by_ref(P& r) { r.a = 99; }
                int passes_by_ref(P p) { by_ref(p); return p.a; }
                int aliases(P p) { var& r = p; r.a = 55; return r.a; }
                int writes_global(P p) { g.a = 77; return p.a; }
                int subscripts_store(P p) { p.xs[0] = 42; return p.xs[0]; }
                int compound(P p) { p.a += 1; return p.a; }
                int whole_arg(P p) { return mutates(p); }
                P x = P();
                int r1 = mutates(x); int r2 = pushes(x); int r3 = compares(g); int r4 = child_compares(g);
                P y = returns(x); y.a = 1234;
                int r5 = passes_by_ref(x); int r6 = aliases(x); int r7 = writes_global(g); int r8 = subscripts_store(x);
                int r9 = compound(x); int r10 = whole_arg(x);
                return [r1, r2, r3, r4, x.a, x.xs.size(), r5, r6, g.a, r8, x.xs[0], r9, r10, x.a];
            )");
            const auto& v = r.as_array();
            check_eq((int64_t)11, v[0].as_int());
            check_eq((int64_t)2, v[1].as_int());
            check_eq((int64_t)1, v[2].as_int());     // memberwise ==: the copy equals the global it was copied from
            check_eq((int64_t)1, v[3].as_int());     // memberwise ==: equal child values compare equal
            check_eq((int64_t)1, v[4].as_int());     // x untouched by every callee
            check_eq((int64_t)1, v[5].as_int());
            check_eq((int64_t)99, v[6].as_int());
            check_eq((int64_t)55, v[7].as_int());
            check_eq((int64_t)77, v[8].as_int());
            check_eq((int64_t)42, v[9].as_int());
            check_eq((int64_t)1, v[10].as_int());
            check_eq((int64_t)2, v[11].as_int());
            check_eq((int64_t)11, v[12].as_int());
            check_eq((int64_t)1, v[13].as_int());
            check_eq((int64_t)0, (int64_t)detail::engine_access::parameter_alias_binds(*eng));
        });

        test("readonly_parameter_alias_revoked_by_redefinition", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            // The proof is transitive (a callee that writes enclosing state revokes it) and
            // cached against the definition epoch: redefining the helper re-proves on the next call
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(R"(
                class P { int a = 1; P() {} }
                P g = P();
                int helper() { return 1; }
                int reads(P p) { int h = helper(); return p.a + h; }
            )");
            check_eq((int64_t)2, eng->execute("reads(g);").as_int());
            check_eq((int64_t)1, (int64_t)detail::engine_access::parameter_alias_binds(*eng));
            eng->execute("int helper() { g.a = 50; return 1; }");
            // copy semantics: the helper's write lands on g, the parameter still holds the pre-call value
            check_eq((int64_t)2, eng->execute("g.a = 1; reads(g);").as_int());
            check_eq((int64_t)50, eng->execute("g.a;").as_int());
            check_eq((int64_t)1, (int64_t)detail::engine_access::parameter_alias_binds(*eng));
        });

        test("readonly_parameter_alias_skips_nocopy_and_host_objects", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(R"(
                nocopy class Ledger { int total = 1; }
                int peek(Ledger l) { return l.total; }
                auto ledger = Ledger();
            )");
            std::string message;
            try { eng->execute("peek(ledger);"); } catch (const std::exception& e) { message = e.what(); }
            check(message.find("nocopy class 'Ledger'") != std::string::npos, "a nocopy by-value pass still raises: " + message);
            check_eq((int64_t)0, (int64_t)detail::engine_access::parameter_alias_binds(*eng));
        });

        // Read-only by-value local elision: `T x = lvalue;` binds the source's object when nothing in x's block after it can
        // mutate, alias or compare x or write the source. engine::local_alias_binds() counts the binds; the results are the
        // copy's either way (every row runs on both backends).
        test("readonly_local_alias_proven_shapes", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto r = eng->execute(R"(
                class Def { string name = "d"; int atk = 3; array<int> codes = [1, 2, 3]; array<int> vals = [10, 20, 30]; Def() {} Def(int a) { atk = a; } }
                array<Def> DEFS = [Def(1), Def(2), Def(3)];
                Def G = Def(7);
                map<string, int> M = {"a": 1};
                int stat(int i, int code) {
                    int v = 0;
                    Def d = DEFS[i];
                    for (int k = 0; k < d.codes.size(); ++k) { if (d.codes[k] == code) { v = v + d.vals[k]; } }
                    return v + d.atk;
                }
                int from_global() { Def d = G; return d.atk + d.name.size(); }
                int pushes_other(int n) { array<int> out; for (int i = 0; i < 3; i++) { Def d = DEFS[i]; if (d.atk > n) { continue; } out.push(d.atk); } return out.size(); }
                int pure_helper(int a) { return a * 2; }
                int calls_pure(int i) { Def d = DEFS[i]; int h = pure_helper(d.atk); return h + d.vals[0]; }
                int container_local() { array<int> xs = G.codes; int s = 0; for (int i = 0; i < xs.size(); i++) { s += xs[i]; } return s; }
                int map_local() { map<string, int> m = M; return m.has("a") ? m["a"] : 0; }
                int from_local_source() { Def a = Def(5); Def b = a; return b.atk; }
                int reads_other_global() { Def d = DEFS[0]; return d.atk + DEFS[1].atk + G.atk; }
                int stat_loop() { int s = 0; for (int j = 0; j < 4; j++) { s += stat(j % 3, 2); } return s; }   // a callee the vm may graft
                int acc = 0;
                for (int it = 0; it < 10; it++) {
                    acc += stat(1, 2) + from_global() + pushes_other(2) + calls_pure(2) + container_local() + map_local() + from_local_source() + reads_other_global() + stat_loop();
                }
                return [acc, DEFS[1].atk, G.atk, G.codes.size(), DEFS.size()];
            )");
            const auto& v = r.as_array();
            check_eq((int64_t)10 * (22 + 8 + 2 + 16 + 6 + 1 + 5 + 10 + 87), v[0].as_int());
            check_eq((int64_t)2, v[1].as_int());
            check_eq((int64_t)7, v[2].as_int());
            check_eq((int64_t)3, v[3].as_int());
            check_eq((int64_t)140, (int64_t)detail::engine_access::local_alias_binds(*eng));   // fourteen proven declarations per iteration
        });

        test("readonly_local_alias_refused_shapes_keep_the_copy", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto r = eng->execute(R"(
                class Def { int atk = 3; array<int> vals = [1]; Def() {} }
                class Acc { int _hp = 1; int _get_hp() { G.atk = G.atk + 100; return _hp; } }
                Def G = Def();
                int take(Def x) { x.atk = 1; return x.atk; }
                void bump() { G.atk = G.atk + 1; }
                void write_ref(int& x) { x = 66; }
                int mutates() { Def d = G; d.atk = 9; return d.atk * 10 + G.atk; }
                int pushes() { Def d = G; d.vals.push(2); return d.vals.size() * 10 + G.vals.size(); }
                int rebinds() { Def d = G; d = Def(); [[warn_mode]] d.atk = 4; return G.atk; }
                int aliases() { Def d = G; var& a = d; a.atk = 55; return G.atk; }
                int captures() { Def d = G; auto f = [d]() { return d.atk; }; G.atk = 4; int c = f(); G.atk = 3; return c; }
                Def returns() { Def d = G; return d; }
                int passes() { Def d = G; return take(d) * 10 + G.atk; }
                int compares() { Def d = G; return d == G ? 1 : 0; }
                int identity() { Def d = G; return same_object(d, G) ? 1 : 0; }
                int callee_writes() { Def d = G; bump(); int c = d.atk; G.atk = 3; return c; }
                int local_source_written() { Def a = Def(); Def b = a; [[warn_mode]] a.atk = 8; return b.atk; }
                int handle_alias() { shared_ptr<Def> hp = G; Def d = G; hp.atk = 77; int c = d.atk; G.atk = 3; return c; }
                int ref_arg_write() { Def d = G; write_ref(G.atk); int c = d.atk; G.atk = 3; return c; }
                int ref_param_write(Def& q) { Def d = G; q.atk = 44; int c = d.atk; return c; }
                int getter() { Acc a = Acc(); Def d = G; int h = a.hp; int c = d.atk; G.atk = 3; return c + h; }
                int host_call() { Def d = G; to_json(d); return d.atk; }
                int global_write() { Def d = G; G = Def(); G.atk = 5; int c = d.atk; G.atk = 3; return c; }
                int r1 = mutates(); int r2 = pushes(); int r3 = rebinds(); int r4 = aliases(); int r5 = captures();
                Def y = returns(); y.atk = 1234;
                int r6 = passes(); int r7 = compares(); int r8 = identity(); int r9 = callee_writes(); int r10 = local_source_written();
                int r11 = handle_alias(); int r12 = ref_arg_write(); int r13 = ref_param_write(G); G.atk = 3; int r14 = getter(); int r15 = host_call();
                int r16 = global_write();
                return [r1, r2, r3, r4, r5, G.atk, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15, r16, G.vals.size()];
            )");
            const auto& v = r.as_array();
            check_eq((int64_t)93, v[0].as_int());
            check_eq((int64_t)21, v[1].as_int());
            check_eq((int64_t)3, v[2].as_int());
            check_eq((int64_t)3, v[3].as_int());
            check_eq((int64_t)3, v[4].as_int());
            check_eq((int64_t)3, v[5].as_int());
            check_eq((int64_t)13, v[6].as_int());
            check_eq((int64_t)1, v[7].as_int());     // memberwise ==: the copy equals its source
            check_eq((int64_t)0, v[8].as_int());     // a copy is never the same object
            check_eq((int64_t)3, v[9].as_int());
            check_eq((int64_t)3, v[10].as_int());
            check_eq((int64_t)3, v[11].as_int());
            check_eq((int64_t)3, v[12].as_int());
            check_eq((int64_t)3, v[13].as_int());
            check_eq((int64_t)4, v[14].as_int());
            check_eq((int64_t)3, v[15].as_int());
            check_eq((int64_t)3, v[16].as_int());
            check_eq((int64_t)1, v[17].as_int());
            check_eq((int64_t)0, (int64_t)detail::engine_access::local_alias_binds(*eng));
        });

        test("readonly_local_alias_revoked_by_redefinition_and_switch", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(R"(
                class P { int a = 1; P() {} }
                P g = P();
                int helper() { return 1; }
                int reads() { P p = g; int h = helper(); return p.a + h; }
            )");
            check_eq((int64_t)2, eng->execute("reads();").as_int());
            check_eq((int64_t)1, (int64_t)detail::engine_access::local_alias_binds(*eng));
            eng->execute("int helper() { g.a = 50; return 1; }");
            check_eq((int64_t)2, eng->execute("g.a = 1; reads();").as_int());   // the copy keeps the pre-call value
            check_eq((int64_t)50, eng->execute("g.a;").as_int());
            check_eq((int64_t)1, (int64_t)detail::engine_access::local_alias_binds(*eng));
            eng->local_alias(false);
            eng->execute("int helper() { return 1; }");
            check_eq((int64_t)2, eng->execute("g.a = 1; reads();").as_int());
            check_eq((int64_t)1, (int64_t)detail::engine_access::local_alias_binds(*eng));   // the kill switch keeps every copy
        });

        test("a_method_redefined_while_it_runs_finishes_its_old_body", [this]() {
            // the running call holds no pin on the method's dispatcher: its body, overload and environment stay live through the reload,
            // and the next call takes the new body
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            jai::engine* raw = eng.get();
            eng->add_function("reload_c", [raw]() { raw->execute("class C { int base = 7; int run(int k) { return 1000 + k; } }"); });
            eng->execute(R"(
                class C { int base = 7; int run(int k) { int a = base + k; if (k == 2) { reload_c(); } array<int> pad = [a, a]; return pad[0] + pad[1] + base; } }
                C c = C();
            )");
            check_eq(std::string("[21, 23, 25, 1003]"), eng->execute("[c.run(0), c.run(1), c.run(2), c.run(3)];").to_string(), "old body to its end, then the new one");
        });

        test("a_constructor_or_static_method_redefined_while_it_runs_finishes_its_old_body", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            jai::engine* raw = eng.get();
            eng->add_function("reload_d", [raw]() { raw->execute("class D { int v = 0; D(int k) { v = 1000 + k; } static int twice(int k) { return 2000 + k; } }"); });
            eng->execute(R"(
                class D { int v = 0; D(int k) { int a = k * 2; if (k == 2) { reload_d(); } array<int> pad = [a, a]; v = pad[0] + pad[1]; }
                          static int twice(int k) { int a = k * 3; if (k == 2) { reload_d(); } array<int> pad = [a, a]; return pad[0] + pad[1]; } }
            )");
            check_eq(std::string("[0, 4, 8, 1003]"), eng->execute("[D(0).v, D(1).v, D(2).v, D(3).v];").to_string(), "constructor: old body to its end, then the new one");
            eng->execute(R"(
                class D { int v = 0; D(int k) { v = k; }
                          static int twice(int k) { int a = k * 3; if (k == 2) { reload_d(); } array<int> pad = [a, a]; return pad[0] + pad[1]; } }
            )");
            check_eq(std::string("[0, 6, 12, 2003]"), eng->execute("[D::twice(0), D::twice(1), D::twice(2), D::twice(3)];").to_string(), "static method: old body to its end, then the new one");
        });

        test("an_indexed_store_of_a_ternary_evaluates_the_value_first_and_stores_each_arm", [this]() {
            auto e = make_engine();
            jai::stdlib::register_all(e);
            auto r = e->execute(R"(
                array<string> LOG = [];
                int pick(int k) { LOG.push("i" + to_string(k)); return k; }
                bool cond(bool b) { LOG.push("c"); return b; }
                int run() {
                    array<int> a = [0, 0, 0, 0];
                    array<var> v = [null, null];
                    for (int i = 0; i < 4; i++) {
                        a[pick(i)] = cond(i % 2 == 0) ? i * 10 : -i;
                        a[i] = a[i] < 5 ? a[i] + 100 : a[i];
                    }
                    v[0] = a[1] < 0 ? "neg" : 1.5;
                    v[1] = a[0] == 100 ? a : null;
                    return a[0] + a[1] + a[2] + a[3] + (v[0] == "neg" ? 1000 : 0) + (v[1] == null ? 0 : 10000);
                }
                return [run(), LOG];
            )");
            check_eq(std::string("[10316, [\"c\", \"i0\", \"c\", \"i1\", \"c\", \"i2\", \"c\", \"i3\"]]"), r.to_string());
        });

        test("readonly_local_alias_skips_nocopy_and_destructors", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(R"(
                nocopy class Ledger { int total = 1; }
                auto ledger = Ledger();
                int peek() { Ledger l = ledger; return l.total; }
            )");
            std::string message;
            try { eng->execute("peek();"); } catch (const std::exception& e) { message = e.what(); }
            check(message.find("nocopy class 'Ledger'") != std::string::npos, "a nocopy local copy still raises: " + message);
            auto d = make_engine();
            jai::stdlib::register_all(d);
            auto r = d->execute(R"(
                int DEAD = 0;
                class Tracked { int v = 2; ~Tracked() { DEAD = DEAD + 1; } }
                Tracked T = Tracked();
                int read_tracked() { Tracked t = T; return t.v; }
                int s = 0;
                for (int i = 0; i < 3; i++) { s += read_tracked(); }
                return [s, DEAD];
            )");
            check_eq((int64_t)6, r.as_array()[0].as_int());
            check_eq((int64_t)3, r.as_array()[1].as_int());   // every copy's destructor runs
            check_eq((int64_t)0, (int64_t)detail::engine_access::local_alias_binds(*d));
            check_eq((int64_t)0, (int64_t)detail::engine_access::local_alias_binds(*eng));
        });

        // The parameter proof's write channels: a callee's other reference parameter, a writing reference argument rooted at
        // the parameter, a handle copied from program state, and the copy's own destructor
        test("readonly_parameter_alias_write_channels_keep_the_copy", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto r = eng->execute(R"(
                int DEAD = 0;
                class Foo { int x = 1; Foo() {} }
                class Holder { Foo foo = Foo(); Holder() {} }
                class Tracked { int v = 2; ~Tracked() { DEAD = DEAD + 1; } }
                shared_ptr<Holder> H = Holder();
                Foo G = Foo();
                Tracked T = Tracked();
                int other_ref(Foo p, Foo& q) { q.x = 5; return p.x; }
                void w(int& h) { h = 6; }
                int ref_arg(Foo p) { w(p.x); return p.x; }
                void poke() { shared_ptr<Holder> h = H; h.foo.x = 7; }
                int through_handle(Foo p) { poke(); return p.x; }
                int read_tracked(Tracked t) { return t.v; }
                int r1 = other_ref(G, G); int g1 = G.x; G.x = 1;
                int r2 = ref_arg(G); int g2 = G.x; G.x = 1;
                int r3 = through_handle(H.foo);
                int r4 = read_tracked(T);
                return [r1, g1, r2, g2, r3, r4, DEAD];
            )");
            const auto& v = r.as_array();
            check_eq((int64_t)1, v[0].as_int(), "other_ref: the parameter is a copy");
            check_eq((int64_t)5, v[1].as_int());
            check_eq((int64_t)6, v[2].as_int(), "ref_arg: the callee writes the copy");
            check_eq((int64_t)1, v[3].as_int(), "ref_arg: the caller's object is untouched");
            check_eq((int64_t)1, v[4].as_int(), "through_handle: the parameter is a copy");
            check_eq((int64_t)2, v[5].as_int());
            check_eq((int64_t)1, v[6].as_int(), "the copy's destructor runs");
            check_eq((int64_t)0, (int64_t)detail::engine_access::parameter_alias_binds(*eng), "no write-channel parameter is elided");
        });

        // A class declaration in the callee takes its value from a chain of declared steps (a typed global's element, a local's
        // field, a reference parameter's element) without running code, so it no longer refuses the parameter's proof; a call the
        // vm grafts binds by the same verdict and counts the same binds
        test("readonly_parameter_alias_declared_chains_and_grafts", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto r = eng->execute(R"(
                class Def { int atk = 3; array<int> codes = [1, 2, 3]; Def() {} }
                class Item { int base = 0; int pmag = 1; Item() {} }
                class Bag { array<Item> items = []; Def def = Def(); Bag() {} }
                array<Def> DEFS = [Def(), Def()];
                Bag BAG = Bag();
                int from_global(Item it) { Def d = DEFS[it.base]; int s = 0; for (int k = 0; k < d.codes.size(); ++k) { s += d.codes[k]; } return s + d.atk + it.pmag; }
                int from_local(Item it) { Bag b = BAG; Item first = b.items[0]; Def d = b.def; return first.pmag + d.atk + it.pmag; }
                int from_reference(Item it, Bag& b) { Item first = b.items[1]; return first.pmag * 10 + it.pmag; }
                int grafts() {
                    int s = 0;
                    for (int i = 0; i < 4; i++) { s += from_global(BAG.items[i % 2]) + from_local(BAG.items[0]) + from_reference(BAG.items[1], BAG); }
                    return s;
                }
                BAG.items.push(Item()); BAG.items.push(Item());
                BAG.items[1].base = 1; BAG.items[1].pmag = 2;
                int direct = from_global(BAG.items[0]) + from_local(BAG.items[1]) + from_reference(BAG.items[0], BAG);
                int looped = grafts();
                return [direct, looped, BAG.items[0].pmag, BAG.items[1].pmag, DEFS[0].atk];
            )");
            const auto& v = r.as_array();
            check_eq((int64_t)(10 + 6 + 21), v[0].as_int());
            check_eq((int64_t)(2 * 10 + 2 * 11 + 4 * 5 + 4 * 22), v[1].as_int());
            check_eq((int64_t)1, v[2].as_int());
            check_eq((int64_t)2, v[3].as_int());
            check_eq((int64_t)3, v[4].as_int());
            check_eq((int64_t)15, (int64_t)detail::engine_access::parameter_alias_binds(*eng), "three proven parameters per call, called directly and from a loop");
        });

        test("readonly_parameter_alias_declared_chains_refuse_open_types", [this]() {
            // a var global can hold another class next (its typed twin is not its type), and a class with a subclass
            // gives a global no fixed class: a declaration from either may run a converting constructor, which here writes the
            // caller's argument; the parameter keeps its copy
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto r = eng->execute(R"(
                class Item { int pmag = 1; Item() {} }
                class Base { int v = 1; Base() {} }
                class Kid : Base { Kid() {} }
                Item ITEM = Item();
                class Conv { int v = 2; Conv() {} Conv(Base b) { ITEM.pmag = 99; v = b.v; } }
                array<Conv> CONVS = [Conv()];
                array<Base> BASES = [Base()];
                var PICK = CONVS;
                Base SOLO = Base();
                int reads_var(Item it) { Conv c = PICK[0]; return c.v * 100 + it.pmag; }
                int reads_solo(Item it) { Base b = SOLO; return b.v * 100 + it.pmag; }
                int a = reads_var(ITEM);
                PICK = BASES;
                int b = reads_var(ITEM);
                int pm = ITEM.pmag; ITEM.pmag = 1;
                int c = reads_solo(ITEM);
                return [a, b, pm, c];
            )");
            const auto& v = r.as_array();
            check_eq((int64_t)201, v[0].as_int());
            check_eq((int64_t)101, v[1].as_int(), "the converting constructor's write never reaches the copied parameter");
            check_eq((int64_t)99, v[2].as_int());
            check_eq((int64_t)101, v[3].as_int());
            check_eq((int64_t)0, (int64_t)detail::engine_access::parameter_alias_binds(*eng), "an open source type refuses the proof");
        });

        test("graft_parameter_binds_take_the_calls_elisions", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            // a prvalue argument of a grafted call moves into the parameter; a mutating callee keeps the copy semantics
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto r = eng->execute(R"(
                class Item { int pmag = 1; array<int> tags = [1]; Item() {} }
                Item G = Item();
                Item make(int p) { Item it; it.pmag = p; return it; }
                int bump(Item it) { it.pmag = it.pmag + 10; it.tags.push(2); return it.pmag * 10 + it.tags.size(); }
                int peek(Item it) { return it.pmag; }
                int run() { int s = 0; for (int i = 0; i < 3; i++) { s += bump(make(i)) + bump(G) + peek(make(i)) + peek(G); } return s; }
                int s = run();
                return [s, G.pmag, G.tags.size()];
            )");
            const auto& v = r.as_array();
            check_eq((int64_t)((102 + 112 + 122) + 3 * 112 + (0 + 1 + 2) + 3 * 1), v[0].as_int());
            check_eq((int64_t)1, v[1].as_int());
            check_eq((int64_t)1, v[2].as_int());
            check_eq((int64_t)6, (int64_t)detail::engine_access::parameter_alias_binds(*eng), "peek's two parameters per iteration");
        });

        // The destructor gate is per type: an elision is refused when the elided value's tree can hold an instance whose chain
        // declares a script destructor, or when such an instance can die while the elided value lives
        test("alias_destructor_gate_is_per_type", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            auto r = eng->execute(R"(
                int DEAD = 0;
                class Def { int atk = 3; Def() {} }
                class Tracked { int v = 2; Tracked() {} ~Tracked() { DEAD = DEAD + 1; } }
                class Holder { int n = 1; Tracked t = Tracked(); Holder() {} }
                class Writer { int v = 0; Writer() {} ~Writer() { G.atk = G.atk + 100; } }
                Def G = Def();
                Holder HG = Holder();
                Writer WG = Writer();
                int local_unrelated() { Def d = G; return d.atk; }
                int param_unrelated(Def p) { return p.atk; }
                int holder_local() { Holder h = HG; return h.n; }
                int take_writer(Writer w) { return w.v; }
                int dies_in_range(Writer w) { Def d = G; int k = take_writer(w); int c = d.atk; return c + k; }
                int r1 = local_unrelated();
                int r2 = param_unrelated(G);
                int d0 = DEAD;
                int r3 = holder_local();
                int d1 = DEAD;
                int r4 = dies_in_range(WG);
                G.atk = 3;
                return [r1, r2, r3, d1 - d0, r4];
            )");
            const auto& v = r.as_array();
            check_eq((int64_t)3, v[0].as_int());
            check_eq((int64_t)3, v[1].as_int());
            check_eq((int64_t)1, v[2].as_int());
            check_eq((int64_t)1, v[3].as_int(), "the copied Holder's Tracked field runs its destructor");
            check_eq((int64_t)3, v[4].as_int(), "the copy keeps its value when a destructor in the range writes the source");
            check_eq((int64_t)1, (int64_t)detail::engine_access::local_alias_binds(*eng), "an unrelated local still elides");
            check_eq((int64_t)1, (int64_t)detail::engine_access::parameter_alias_binds(*eng), "an unrelated parameter still elides");
        });

        test("destructuring_basic", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(R"(
                auto [x, y, z] = [1, 2, 3];
            )");
            check_eq(eng->get_variable("x").as<int64_t>(), int64_t(1));
            check_eq(eng->get_variable("y").as<int64_t>(), int64_t(2));
            check_eq(eng->get_variable("z").as<int64_t>(), int64_t(3));
        });

        test("destructuring_fewer_vars", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(R"(
                auto [a, b] = [10, 20, 30];
            )");
            check_eq(eng->get_variable("a").as<int64_t>(), int64_t(10));
            check_eq(eng->get_variable("b").as<int64_t>(), int64_t(20));
        });

        test("destructuring_more_vars", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(R"(
                auto [a, b, c] = [10, 20];
            )");
            check_eq(eng->get_variable("a").as<int64_t>(), int64_t(10));
            check_eq(eng->get_variable("b").as<int64_t>(), int64_t(20));
            check(eng->get_variable("c").is_null());
        });


        // ---- C++ return-type-first free functions with class types (top level) ----

        test("class_typed_return_first_declares_a_function_and_a_class_typed_variable_stays_a_variable", [this]() {
            const std::pair<const char*, int64_t> cases[] = {
                {"class Point { int x = 5; } Point mk() { return Point(); } mk().x;", 5},
                {"class Point { int x = 6; } Point mk() { return Point{}; } function call0(g) { return g(); } var f = mk; f().x + call0(mk).x;", 12},
                {"class Point { int x = 8; } Point p = Point(); p.x;", 8},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_eq(c.second, engine->execute(c.first).as_int());
            });
        });

        test("class_typed_return_type_enforced", [this]() {
            auto engine = make_engine();
            check_throws([&]() {
                engine->execute(R"(
                    class P { int x = 1; }
                    class Q { int y = 2; }
                    P mk() { return Q(); }
                    mk();
                )");
            });
        });

        test("class_typed_return_first_redefinition_last_wins", [this]() {
            auto engine = make_engine();
            // one signature defined twice in one unit is an error (docs/site/guide/06-functions.html "Overloads and redefinition"); another unit's definition replaces it
            engine->execute(R"(
                class P { int x = 1; }
                P mk() { return P(); }
            )");
            auto r = engine->execute(R"(
                P mk() { var p = P(); p.x = 2; return p; }
                mk().x;
            )");
            check_eq(int64_t(2), r.as_int());
        });

        // ---- untyped parameter defaults ----

        test("untyped_parameter_defaults", [this]() {
            const std::pair<const char*, int64_t> cases[] = {
                {"function f(x = 3) { return x; } f();", 3},
                {"function g(x = 3) { return x; } g(10);", 10},
                {"auto f = [](x = 8) { return x; }; f() + f(1);", 9},
                {"function f(x, y = 4) { return x + y; } f(1) + f(1, 1);", 7},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_eq(c.second, engine->execute(c.first).as_int());
            });
        });

        test("untyped_param_default_ordering_still_enforced", [this]() {
            auto engine = make_engine();
            // parse error surfaces as a throw (probed both backends); engine stays usable
            check_throws([&]() {
                engine->execute("function f(x = 1, y) { return y; } f(2);");
            });
            check_eq(int64_t(4), engine->execute("2 + 2").as_int());
        });

        // ---- function-typed variables and fields ----

        test("function_types_variables_and_fields_and_still_declares_functions_and_methods", [this]() {
            const std::pair<const char*, int64_t> cases[] = {
                {"function f = [](x) { return x * 2; }; f(21);", 42},
                {"function f; f = [](){ return 7; }; f();", 7},
                {"function g() { return 3; } function f = g; f();", 3},
                {"function outer() { function f = [](x) { return x + 1; }; return f(41); } outer();", 42},
                {"class K { function cb = [](){ return 8; }; } K().cb();", 8},
                {"class K { function cb; } auto k = K(); k.cb = [](){ return 9; }; k.cb();", 9},
                // `function operator=` parses as a method, never as a function-typed field: a var holding a V runs it
                {"class V { int x = 0; function operator=(o) { x = o.x + 100; return this; } } var a = V(); var b = V(); b.x = 1; a = b; a.x;", 101},
                {"function g(x) { return x * 2; } g(21);", 42},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_eq(c.second, engine->execute(c.first).as_int());
            });
        });

        // ---- return-type conflicts (contradictory leading +
        // trailing types = parse error; a matching pair is redundant-legal) ----

        test("return_type_conflict_is_parse_error", [this]() {
            auto engine = make_engine();
            check_throws([&]() { engine->execute("int f() -> string { return \"s\"; } f();"); });
            check_throws([&]() { engine->execute("class K { int m() -> string { return \"s\"; } } K().m();"); });
            check_throws([&]() { engine->execute("namespace n { int f() -> string { return \"s\"; } } n::f();"); });
        });

        test("return_type_redundant_match_ok", [this]() {
            auto engine = make_engine();
            check_eq(int64_t(4), engine->execute("int f() -> int { return 4; } f();").as_int());
            check_eq(int64_t(5), engine->execute("function g() -> int { return 5; } g();").as_int());
            check_eq(int64_t(6), engine->execute("auto h() -> int { return 6; } h();").as_int());
            check_eq(int64_t(7), engine->execute("class K { int m() -> int { return 7; } } K().m();").as_int());
        });

        test("arrow_auto_keeps_leading_type", [this]() {
            auto engine = make_engine();
            // -> { adds no information; the declared int still converts/enforces
            check_eq(int64_t(2), engine->execute("int f() -> { return 2.9; } f();").as_int());
            check_throws([&]() { engine->execute("int g() -> { return \"x\"; } g();"); });
        });

        // ---- initializer lists are constructor-only ----

        test("initializer_list_only_on_constructors", [this]() {
            auto engine = make_engine();
            check_throws([&]() { engine->execute("function f() : super(1) { return 3; } f();"); });
            check_throws([&]() { engine->execute("int f() : this(1) { return 3; } f();"); });
            check_throws([&]() { engine->execute("class K { function m() : super(1) { return 1; } } 1;"); });
            check_throws([&]() { engine->execute("class A { int v = 0; } class B : A { int m() : super() { return 1; } } 1;"); });
            check_throws([&]() { engine->execute("class K { ~K() : super() {} } 1;"); });
        });

        test("constructor_initializers_still_work", [this]() {
            auto engine = make_engine();
            check_eq(int64_t(15), engine->execute(
                "class K { int a = 0; K(int v) { a = v; } K() : this(15) {} } K().a;").as_int());
            check_eq(int64_t(9), engine->execute(
                "class A { int v = 0; A(int x) { v = x; } } class B : A { B() : super(9) {} } B().v;").as_int());
        });

        // ---- top-level functions of other signatures overload; a coroutine does not, and a function and
        // a coroutine of one name warn; include composition is silent (warn severity, scoped to one textual parse unit) ----

        auto duplicate_warnings = [](jai::engine& eng) {
            size_t n = 0;
            for (const auto& d : eng.last_check_diagnostics().diagnostics) {
                if (d.severity == jai::check_diagnostic::level::warning &&
                    d.message.find("duplicate definition") != std::string::npos) { n++; }
            }
            return n;
        };

        test("top_level_overloads_do_not_warn", [&, this]() {
            auto engine = make_engine();
            check_eq(int64_t(21), engine->execute(
                "function f(a) { return 1; } function f(a, b) { return 2; } f(0, 0) * 10 + f(0);").as_int());
            check_eq(size_t(0), duplicate_warnings(*engine));
        });

        test("duplicate_warning_spans_definition_forms", [&, this]() {
            auto engine = make_engine();
            engine->execute("int f(int a) { return 1; } coroutine function f() { yield 1; } 1;");
            check_eq(size_t(1), duplicate_warnings(*engine));
        });

        test("duplicate_warning_silent_for_legit_overloads", [&, this]() {
            auto engine = make_engine();
            engine->execute(
                "namespace n { int f() { return 1; } int f(int a) { return 2; } }"
                "class K { int m(a) { return 1; } int m(a, b) { return 2; } } n::f();");
            check_eq(size_t(0), duplicate_warnings(*engine));
        });

        test("duplicate_warning_silent_across_executes", [&, this]() {
            auto engine = make_engine();
            engine->execute("function f(a) { return 1; } f(0);");
            engine->execute("function f(a) { return 2; } f(0);");   // hot-reload shape
            check_eq(size_t(0), duplicate_warnings(*engine));
        });

        test("duplicate_warning_silent_for_include_composition", [&, this]() {
            namespace fs = std::filesystem;
            auto dir = jai::foundry::test_temp_directory();
            auto file = dir / "fn_dupwarn_lib.jai";
            { std::ofstream out(file); out << "function f(a) { return 1; }\n"; }

            auto engine = make_engine();
            engine->add_include_path(dir.string());
            // file defines f once; this unit includes it TWICE and defines its own f
            // once - all composition, zero textual duplicates, zero warnings
            auto r = engine->execute(
                "include \"fn_dupwarn_lib.jai\"; include \"fn_dupwarn_lib.jai\";"
                "function f(a, b) { return a + b; } f(1, 2);");
            check_eq(int64_t(3), r.as_int());
            check_eq(size_t(0), duplicate_warnings(*engine));
            fs::remove(file);
        });

        test("duplicate_warning_never_blocks_strict_mode", [&, this]() {
            auto engine = make_engine();
            check_eq(int64_t(1), engine->execute(
                "coroutine function f() { yield 1; } function f(a) { return 1; } f(0);").as_int());
            check_eq(size_t(1), duplicate_warnings(*engine));
        });

        test("same_signature_twice_in_one_unit_is_an_error", [&, this]() {
            // the general redefinition rule (docs/site/guide/06-functions.html "Overloads and redefinition")
            auto engine = make_engine();
            std::string err;
            try { engine->execute("function f(a) { return 1; } function f(a) { return 2; } f(0);"); } catch (const std::exception& ex) { err = ex.what(); }
            check_true(err.find("'f' is already defined (declared at 1:10)") != std::string::npos, err);
            engine->execute("function g(a) { return 1; }");
            check_eq(int64_t(2), engine->execute("function g(a) { return 2; } g(0);").as_int());   // another unit: a redefinition
        });

        // ---- anonymous function expressions: `function (params) {...}` ----
        // Pure desugar to a no-capture lambda ([]-equivalent auto-capture:
        // enclosing-function locals snapshot by value at creation, globals live)

        test("anonymous_function_expressions_assign_pass_invoke_and_leave_declarations_alone", [this]() {
            const std::pair<const char*, int64_t> cases[] = {
                {"var f = function(x) { return x * 2; }; f(21);", 42},
                {"auto g = function(a, b) { return a + b; }; g(3, 4);", 7},
                {"function h = function() { return 9; }; h();", 9},
                {"function apply(g, v) { return g(v); }apply(function(x) { return x + 1; }, 41);", 42},
                {"function(x) { return x + 1; }(41);", 42},
                {"var f = function(x = 8) { return x; }; f();", 8},
                {"function(x) { return x; }; 1;", 1},
                {"function f(x) { return x * 2; } f(21);", 42},
                {"function h = [](){ return 3; }; h();", 3},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_eq(c.second, engine->execute(c.first).as_int());
            });
        });

        test("anonymous_function_trailing_return", [this]() {
            auto engine = make_engine();
            check_eq(int64_t(5), engine->execute(
                "var f = function(a) -> int { return a; }; f(5);").as_int());
            check_eq(int64_t(6), engine->execute(
                "var g = function(a) -> { return a; }; g(6);").as_int());
            check_throws([&]() {
                engine->execute("var h = function() -> int { return \"x\"; }; h();");
            });
        });

        test("anonymous_function_closes_over_its_scope_by_reference", [this]() {
            // function(...) is the JavaScript form: [&] over the enclosing scope, shared live cells that outlive it
            auto engine = make_engine();
            check_eq(int64_t(5), engine->execute("var adder = function(x) { return function(y) { return x + y; }; }; adder(2)(3);").as_int(), "curried");
            check_eq(int64_t(2), engine->execute("function live() { int a = 1; var f = function() { return a; }; a = 2; return f(); } live();").as_int(), "a later write is seen");
            check_eq(int64_t(3), engine->execute("function writes() { int a = 1; var inc = function() { a += 1; }; inc(); inc(); return a; } writes();").as_int(), "writes land in the local");
            check_eq(int64_t(40), engine->execute("function nested() { int a = 1; var f = function() { return function() { return a * 10; }; }; a = 4; return f()(); } nested();").as_int(), "nested");
            check_eq(int64_t(2), engine->execute("function counter() { int n = 0; return function() { n += 1; return n; }; } var c = counter(); c(); c();").as_int(), "the cell outlives the frame");
            check_eq(int64_t(13), engine->execute("class C { int hp = 5; int m() { int k = 2; var f = function() { return k + hp + this.hp; }; k = 3; return f(); } } C().m();").as_int(), "a method's locals and this");
            check_throws([&]() { engine->execute("function bare() { int a = 1; var g = [](){ return a; }; return g(); } bare();"); });
        });

        test("instance_capture_keeps_the_other_captures", [this]() {
            // [this] with locals, explicit or by default: the locals stay in front of the members
            auto engine = make_engine();
            engine->execute(R"(
                class C {
                    int hp = 5;
                    int k = 100;
                    int named() { int k = 2; var f = [this, k]() { return k + this.hp; }; k = 3; return f(); }
                    int shared() { int k = 2; var f = [this, &k]() { return k + hp; }; k = 3; return f(); }
                    int copied() { int k = 2; var f = [=]() { return k + this.hp; }; k = 3; return f(); }
                    int live() { int k = 2; var f = [&]() { return k + hp + this.hp; }; k = 3; return f(); }
                }
            )");
            check_eq(int64_t(7), engine->execute("C().named();").as_int());
            check_eq(int64_t(8), engine->execute("C().shared();").as_int());
            check_eq(int64_t(7), engine->execute("C().copied();").as_int());
            check_eq(int64_t(13), engine->execute("C().live();").as_int());
        });

        test("anonymous_function_takes_no_name_and_no_coroutine_form", [this]() {
            const char* const cases[] = {
                {"var f = function g(x) { return x; }; f(1);"},
                {"var f = coroutine function() { yield 1; }; 1;"},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_throws([&]() { engine->execute(c); });
            });
        });

        test("destructuring_from_variable", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            eng->execute(R"(
                auto arr = [100, 200];
                auto [x, y] = arr;
            )");
            check_eq(eng->get_variable("x").as<int64_t>(), int64_t(100));
            check_eq(eng->get_variable("y").as<int64_t>(), int64_t(200));
        });

        // T& parameters bound to the caller's locals (the vm's plain frame with references): a plain local boxes, a recursive
        // pass shares, a reference local shares its target, a closure keeps the reference, a declared T& still admits only T
        test("reference_parameters_from_caller_locals", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(eng);
            check_eq(std::string("16 441 7.0float 10/10 [Cannot pass int to parameter 'f' of 'setf' (declared float&)] 3"), eng->execute(R"(
                void bump(int& r, int by) { r = r + by; }
                void fill(array<int>& a, int n) { if (n == 0) { return; } a.push(n); fill(a, n - 1); }
                void setf(float& f, int v) { f = v; }
                int keep(int& r) { auto g = [&]() { r = r * 2; }; g(); return r; }
                function run() -> string {
                    int x = 1; bump(x, 2); bump(x, 3);
                    array<int> a = []; fill(a, 4);
                    var v = 1.5; setf(v, 7);
                    int k = 5; int got = keep(k);
                    var iv = 3; string err = "";
                    try { setf(iv, 2); } catch (e) { err = e; }
                    int& rx = x; bump(rx, 10);
                    return to_string(x) + " " + to_string(a.size()) + to_string(a[0]) + to_string(a[3]) + " " + to_string(v) + type_of(v) + " " +
                           to_string(k) + "/" + to_string(got) + " [" + err + "] " + to_string(iv);
                }
                run();
            )").to_string());
        });

        // ---- free-function overloads: overloads coexist as in C++, only the same signature replaces, a
        // reloaded unit replaces or drops only its own declarations; a tie raises, the nearest base breaks a class tie ----

        auto error_of = [](auto&& run) -> std::string {
            try { run(); } catch (const std::exception& ex) { return ex.what(); }
            return "";
        };

        test("free_function_overloads_coexist", [this]() {
            auto engine = make_engine();
            check_eq(std::string("AB33|1,2,3,4"), engine->execute(R"(
                class O { var x = 0; }
                string log = "";
                void mix(var& r, string n) { log = `${log}A`; r = 33; }
                void mix(var r, int n) { log = `${log}B`; r = 44; }
                var o = new O();
                mix(o.x, "s");
                mix(o.x, 5);
                int f(int a) { return 1; }
                int f(string a) { return 2; }
                int f(int a, int b) { return 3; }
                int f(float a) { return 4; }
                `${log}${o.x}|${f(7)},${f("x")},${f(1, 2)},${f(2.5)}`;
            )").to_string());
        });

        test("free_function_overloads_coexist_across_units", [this]() {
            auto engine = make_engine();
            engine->execute("int f(int a) { return 1; }");
            engine->execute("int f(string a) { return 2; }");
            check_eq(int64_t(12), engine->execute("f(5) * 10 + f(\"x\");").as_int(), "both units' overloads");
            engine->execute("int f(string a) { return 3; }");   // the same signature replaces, in any unit
            check_eq(int64_t(13), engine->execute("f(5) * 10 + f(\"x\");").as_int(), "the same signature replaces");
            // the checker resolves a unit's call against the program's whole set: the earlier units' overloads and its own
            auto split = make_engine();
            split->execute("int g() { return 1; }");
            check_eq(int64_t(15), split->execute("int g(int a) { return a; } g() * 10 + g(5);").as_int(), "another unit's overload takes the call");
            check_eq(int64_t(2), split->execute("int g() { return 2; } g();").as_int(), "the same signature replaces the earlier unit's");
            bool refused = false;
            try { split->execute("int g(int a) { return a; } g(1, 2);"); }
            catch (const static_check_error& err) { refused = std::string(err.what()).find("no overload of 'g' matches this call (2 argument(s))") != std::string::npos; }
            check_true(refused, "a call no overload of the program takes is still a load error");
        });

        test("free_function_overload_reload_drops_only_its_own", [this, error_of]() {
            namespace fs = std::filesystem;
            const auto dir = jai::foundry::test_temp_directory() / "jai_overload_reload";
            fs::create_directories(dir);
            const auto mod = dir / "ov_mod.jai";
            auto write = [&](const char* text) { std::ofstream out(mod, std::ios::trunc); out << text; };
            write("int f(int a) { return 1; }\nint f(string a) { return 2; }\n");
            auto engine = make_engine();
            const std::string path = mod.generic_string();
            engine->execute("int f(float a) { return 4; }");
            engine->execute("import \"" + path + "\";");
            check_eq(int64_t(124), engine->execute("f(5) * 100 + f(\"x\") * 10 + f(2.5);").as_int(), "three overloads from two units");
            fs::last_write_time(mod, fs::last_write_time(mod) + std::chrono::seconds(2));
            write("int f(int a) { return 5; }\n");
            fs::last_write_time(mod, fs::last_write_time(mod) + std::chrono::seconds(4));
            engine->execute("import \"" + path + "\";");
            check_eq(int64_t(54), engine->execute("f(5) * 10 + f(2.5);").as_int(), "the reload replaced its own and kept the other unit's");
            check_true(!error_of([&] { engine->execute("f(\"x\");"); }).empty(), "the reload dropped its string overload");
            fs::remove_all(dir);
        });

        test("namespace_function_overloads_by_signature", [this]() {
            auto engine = make_engine();
            check_eq(int64_t(123), engine->execute(R"(
                namespace n {
                    int f(int a) { return 1; }
                    int f(string a) { return 2; }
                    int f(float a) { return 3; }
                }
                n::f(1) * 100 + n::f("x") * 10 + n::f(2.5);
            )").as_int());
        });

        test("an_ambiguous_overload_call_raises", [this, error_of]() {
            auto engine = make_engine();
            engine->execute("void g(int a, float b) { } void g(float a, int b) { }");
            const std::string free_err = error_of([&] { engine->execute("[[warn_mode]]; g(1, 1);"); });   // the run-time verdict
            check_true(free_err.find("mbiguous") != std::string::npos, "free: " + free_err);
            engine->execute("namespace n { void g(int a, float b) { } void g(float a, int b) { } }");
            const std::string ns_err = error_of([&] { engine->execute("[[warn_mode]]; n::g(1, 1);"); });
            check_true(ns_err.find("mbiguous") != std::string::npos, "namespace: " + ns_err);
            engine->execute("class K { K() { } K(int a, float b) { } K(float a, int b) { } void g(int a, float b) { } void g(float a, int b) { } }");
            const std::string method_err = error_of([&] { engine->execute("[[warn_mode]]; K().g(1, 1);"); });
            check_true(method_err.find("mbiguous") != std::string::npos, "method: " + method_err);
            const std::string ctor_err = error_of([&] { engine->execute("[[warn_mode]]; K(1, 1);"); });
            check_true(ctor_err.find("mbiguous") != std::string::npos, "constructor: " + ctor_err);
            check_eq(int64_t(1), engine->execute("int h(int a, float b) { return 1; } int h(float a, int b) { return 2; } h(1, 2.5);").as_int(), "a better match is no tie");
        });

        test("an_ambiguous_overload_call_is_a_load_error_under_strict", [this, error_of]() {
            auto engine = make_engine();
            const std::string err = error_of([&] { engine->execute("int ran = 0; void g(int a, float b) { } void g(float a, int b) { } ran = 1; g(1, 1);"); });
            check_true(err.find("mbiguous") != std::string::npos, err);
            check_true(!detail::engine_access::get_global_environment(*engine)->get_value_ptr(detail::engine_access::get_symbolizer(*engine)->intern("ran")), "nothing ran");
        });

        // ---- overloads that differ only by reference: the second definition is an error naming both ----

        test("an_overload_differing_only_by_reference_is_an_error", [this, error_of]() {
            const std::string tail = " only by reference; overloads must differ in parameter types or count";
            for (bool warn_region : {true, false}) {
                const std::string label = warn_region ? "run time: " : "load: ";
                auto run = [&](const std::string& src) {
                    auto engine = make_engine();
                    return error_of([&] { engine->execute(warn_region ? "[[warn_mode]]; " + src : src); });
                };
                const std::string free_err = run("void f(var& x) { } void f(var x) { } 1;");
                check_true(free_err.find("'f(var x)' differs from 'f(var& x)' (line 1)" + tail) != std::string::npos, label + "free: " + free_err);
                const std::string method_err = run("class K { void m(int& a, string b) { } void m(int a, string b) { } } 1;");
                check_true(method_err.find("'m(int a, string b)' differs from 'm(int& a, string b)' (line 1)" + tail) != std::string::npos, label + "method: " + method_err);
                const std::string static_err = run("class S { static void m(int a) { } static void m(int& a) { } } 1;");
                check_true(static_err.find("'m(int& a)' differs from 'm(int a)' (line 1)" + tail) != std::string::npos, label + "static: " + static_err);
                const std::string ctor_err = run("class C { int v = 0; C(int& a) { } C(int a) { } } 1;");
                check_true(ctor_err.find("'C(int a)' differs from 'C(int& a)' (line 1)" + tail) != std::string::npos, label + "constructor: " + ctor_err);
                const std::string ns_err = run("namespace n { void f(var& x) { } void f(var x) { } } 1;");
                check_true(ns_err.find("'f(var x)' differs from 'f(var& x)' (line 1)" + tail) != std::string::npos, label + "namespace: " + ns_err);
            }
            namespace fs = std::filesystem;
            const auto dir = jai::foundry::test_temp_directory() / "jai_refonly_context";
            fs::create_directories(dir);
            { std::ofstream out(dir / "refonly_a.jai", std::ios::trunc); out << "void f(var& x) { }\n"; }
            { std::ofstream out(dir / "refonly_b.jai", std::ios::trunc); out << "void f(var x) { }\n"; }
            auto engine = make_engine();
            const std::string a_path = (dir / "refonly_a.jai").generic_string(), b_path = (dir / "refonly_b.jai").generic_string();
            const std::string files_err = error_of([&] { engine->execute("import \"" + a_path + "\"; import \"" + b_path + "\";"); });
            check_true(files_err.find("'f(var x)' differs from 'f(var& x)' (line 1)" + tail) != std::string::npos, "two files in one execute: " + files_err);
            fs::remove_all(dir);
        });

        test("a_reference_only_overload_in_another_execute_context_redefines", [this, error_of]() {
            // A separate host execute, or a module's re-run, redefines instead of conflicting
            namespace fs = std::filesystem;
            auto strings = make_engine();
            strings->execute("int f(var& x) { return 1; }");
            check_eq(std::string(), error_of([&] { strings->execute("int f(var x) { return 2; }"); }), "a second execute of a string");
            check_eq(int64_t(2), strings->execute("var v = 0; f(v);").as_int(), "the later string's definition runs");
            strings->execute("namespace n { int g(int& a) { return 1; } }");
            check_eq(int64_t(2), strings->execute("namespace n { int g(int a) { return 2; } } int k = 0; n::g(k);").as_int(), "a namespace reopened by another execute");

            const auto dir = jai::foundry::test_temp_directory() / "jai_refonly_redefine";
            fs::create_directories(dir);
            const auto a = dir / "redef_a.jai", b = dir / "redef_b.jai", m = dir / "redef_m.jai";
            { std::ofstream out(a, std::ios::trunc); out << "int f(var& x) { return 1; }\n"; }
            { std::ofstream out(b, std::ios::trunc); out << "int f(var x) { return 2; }\n"; }
            auto files = make_engine();
            files->execute_file(a.generic_string());
            check_eq(std::string(), error_of([&] { files->execute_file(b.generic_string()); }), "a second execute_file");
            check_eq(int64_t(2), files->execute("var v = 0; f(v);").as_int(), "the later file's definition runs");

            { std::ofstream out(m, std::ios::trunc); out << "int other() { return 0; }\n"; }
            auto reloads = make_engine();
            const std::string m_path = m.generic_string();
            reloads->add_function("rewrite_module", [m]() -> jai::script_int {
                { std::ofstream out(m, std::ios::trunc); out << "int g(var& x) { return 2; }\n"; }
                fs::last_write_time(m, fs::last_write_time(m) + std::chrono::seconds(4));
                return 0;
            });
            check_eq(int64_t(2), reloads->execute("int g(var x) { return 1; } import \"" + m_path + "\"; rewrite_module(); import \"" + m_path +
                                                   "\"; var v = 0; g(v);").as_int(), "a module's re-run in the same execute redefines");
            fs::remove_all(dir);
        });

        test("overloads_that_differ_in_a_type_too_coexist_and_a_reload_is_no_second_definition", [this, error_of]() {
            namespace fs = std::filesystem;
            auto engine = make_engine();
            check_eq(int64_t(12), engine->execute(R"(
                class O { var x = 0; }
                int mix(var& r, string n) { return 1; }
                int mix(var r, int n) { return 2; }
                var o = new O();
                mix(o.x, "s") * 10 + mix(o.x, 5);
            )").as_int(), "a reference and a type difference coexist");
            engine->execute("[[warn_mode]]; int h(var& a, int b) { return 1; } int h(var a, float b) { return 2; }");
            const std::string tie = error_of([&] { engine->execute("[[warn_mode]]; var v = 1; h(v, true);"); });
            check_true(tie.find("Ambiguous call to 'h'") != std::string::npos, "a call that ties another way stays ambiguous: " + tie);

            const auto dir = jai::foundry::test_temp_directory() / "jai_refonly_reload";
            fs::create_directories(dir);
            const auto mod = dir / "refonly_mod.jai";
            { std::ofstream out(mod, std::ios::trunc); out << "int g(var& x) { return 1; }\n"; }
            auto reloading = make_engine();
            const std::string path = mod.generic_string();
            reloading->execute("import \"" + path + "\";");
            fs::last_write_time(mod, fs::last_write_time(mod) + std::chrono::seconds(2));
            { std::ofstream out(mod, std::ios::trunc); out << "int g(var x) { return 2; }\n"; }
            fs::last_write_time(mod, fs::last_write_time(mod) + std::chrono::seconds(4));
            check_eq(std::string(), error_of([&] { reloading->execute("import \"" + path + "\";"); }), "the reload replaces the unit's own definition");
            check_eq(int64_t(2), reloading->execute("g(5);").as_int(), "the reloaded definition runs");
            fs::remove_all(dir);
        });

        test("a_single_declaration_stays_a_plain_function_value", [this]() {
            // only an overloaded name leaves the call sites' direct path (the VM's inline cache keys on a plain function value)
            auto engine = make_engine();
            auto kind_of = [&](const char* name) {
                const script_value* v = detail::engine_access::get_global_environment(*engine)->get_value_ptr(detail::engine_access::get_symbolizer(*engine)->intern(name));
                const auto* thunk = v && v->is_function() ? v->as_function().target<script_callable_thunk>() : nullptr;
                return thunk ? static_cast<int>(thunk->payload.kind) : -1;
            };
            engine->execute("int f(int a) { return 1; } int f2(int a) { return f(a) + 1; } f2(1);");
            check_eq(static_cast<int>(script_callable::kind_type::function), kind_of("f"), "one declaration");
            engine->execute("int f(string a) { return 2; }");
            check_eq(static_cast<int>(script_callable::kind_type::overload_set), kind_of("f"), "two signatures");
            check_eq(static_cast<int>(script_callable::kind_type::function), kind_of("f2"), "the other name");
        });

        test("the_nearest_base_breaks_a_class_tie", [this]() {
            auto engine = make_engine();
            check_eq(int64_t(23), engine->execute(R"(
                class A { } class B : A { } class C : B { }
                int h(A a) { return 1; }
                int h(B b) { return 2; }
                class M { int h(A a) { return 1; } int h(B b) { return 3; } }
                h(C()) * 10 + M().h(C());
            )").as_int());
        });
    }
};

} // namespace jai::foundry::tests

// Auto-register with the test framework
FOUNDRY_REGISTER(jai::foundry::tests::function_tests)
