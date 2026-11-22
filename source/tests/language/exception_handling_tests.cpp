#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/engine_access.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>

using namespace jai::foundry;

namespace jai::foundry::tests {

namespace {
// Strict (the default) rejects a typed mistake at load: the listing is the probe, empty
// when the source loaded or failed for any other reason.
std::string static_error_text(jai::engine& e, const char* src) {
    try { e.execute(src); }
    catch (const static_check_error& ex) { return ex.what(); }
    catch (...) {}
    return {};
}

// Runs src to its uncaught error: what(), empty when it ran clean.
std::string failure_text(jai::engine& e, const char* src) {
    try { e.execute(src); }
    catch (const std::exception& ex) { return ex.what(); }
    return {};
}

// The kept trace as "fn:line|fn:line" (files omitted).
std::string trace_shape(jai::engine& e) {
    std::string out;
    for (const auto& fr : e.last_stack_trace()) {
        if (!out.empty()) out += "|";
        out += fr.function + ":" + std::to_string(fr.line);
    }
    return out;
}
}

class exception_handling_tests : public suite {
public:
    exception_handling_tests() : suite("Exception Handling") {}
    
    void forge_tests() override {
        // A trace lists the innermost 64 frames and a final marker for the rest, on both backends
        test("stack_trace_keeps_innermost_64_frames", [this]() {
            auto engine = make_engine();
            engine->execution_budget(0);
            auto lines = engine->execute(R"(
                var st = [];
                int d(int n) { if (n == 0) { try { throw "x"; } catch (e) { st = e.stack; } return 0; } return d(n - 1) + 1; }
                d(100);
                st;
            )");
            check_eq(size_t(65), lines.as<std::vector<std::string>>().size());
            check_eq(std::string("... 38 more frames"), lines.as<std::vector<std::string>>()[64]);
            check_eq(script_int(64), engine->execute("var short_st = []; int s(int n) { if (n == 0) { try { throw \"x\"; } catch (e) { short_st = e.stack; } return 0; } return s(n - 1) + 1; } s(62); short_st.size();").as_int());
            std::string trace;
            try { engine->execute("int u(int n) { if (n == 0) { throw \"bottom\"; } return u(n - 1) + 1; } u(100);"); }
            catch (const std::exception&) { trace = engine->format_stack_trace(); }
            check_true(trace.find("  ... 38 more frames") != std::string::npos, trace);
        });

        // A throw past the cap counts the frames it does not show without reading each one: a catch and rethrow at every level of a
        // deep stack reads frames in proportion to the depth (twice as deep, twice the reads), never its square
        test("vm_deep_catch_and_rethrow_is_linear", [this]() {
            auto reads_for = [this](int depth) {
                auto engine = jai::foundry::make_default_engine();
                engine->set_backend(backend_type::vm);
                engine->execution_budget(0);
                const uint64_t before = detail::engine_access::trace_graft_frame_reads(*engine);
                check_eq(script_int(depth), engine->execute(R"(
                    int hops = 0;
                    int rethrow(int n) { if (n == 0) { throw "deep"; } try { return rethrow(n - 1); } catch (e) { hops += 1; throw e; } }
                    try { rethrow()" + std::to_string(depth) + R"(); } catch (e) { }
                    hops;
                )").as_int());
                return detail::engine_access::trace_graft_frame_reads(*engine) - before;
            };
            const uint64_t reads = reads_for(1000), doubled = reads_for(2000);
            check_true(reads > 0 && doubled * 2 < reads * 5, "reads at depth 1000: " + std::to_string(reads) + ", at 2000: " + std::to_string(doubled));
            auto engine = jai::foundry::make_default_engine();
            engine->set_backend(backend_type::vm);
            engine->execution_budget(0);
            auto lines = engine->execute(R"(
                var st = [];
                int d(int n) { if (n == 0) { try { throw "x"; } catch (e) { st = e.stack; } return 0; } return d(n - 1) + 1; }
                d(2000);
                st;
            )");
            check_eq(size_t(65), lines.as<std::vector<std::string>>().size());
            check_eq(std::string("... 1938 more frames"), lines.as<std::vector<std::string>>()[64]);
        });

        test("throw_unwinding_out_of_condition_positions", [this]() {
            // Fuzz seed 285 (pre-existing both backends at 7/13): a throw unwinding out
            // of a COMPARISON condition left a null placeholder that the
            // expression_returns_bool fast path fed to unchecked_as_bool (nullptr deref;
            // truthiness conditions were safe). Every condition position must propagate
            // the throw - no branch/body runs, no crash, engine stays usable.
            const char* shapes[] = {
                "int f() { throw \"boom\"; } if (f() > 0) { mark = 1; } else { mark = 2; }",           // if, cmp-literal
                "int f() { throw \"boom\"; } if (f() != f()) { mark = 1; } else { mark = 2; }",        // if, cmp-call
                "int f() { throw \"boom\"; } if (f()) { mark = 1; } else { mark = 2; }",               // if, truthiness
                "int f() { throw \"boom\"; } void g() { if (f() > 0) { mark = 1; } } g();",            // nested frame
                "int f() { throw \"boom\"; } while (f() > 0) { mark = 1; }",                           // while
                "int f() { throw \"boom\"; } for (int i = 0; f() > i; i++) { mark = 1; }",             // for
                "int f() { throw \"boom\"; } mark = f() > 0 ? 1 : 2;",                                 // ternary
            };
            for (const char* shape : shapes) {
                auto e = make_engine();
                e->execute("var mark = 0;");
                check_throws([&]() { e->execute(shape); });
                check_eq((int64_t)0, e->execute("mark").as_int());       // neither branch ran
                check_eq((int64_t)3, e->execute("1 + 2").as_int());      // engine usable after
            }
        });

        test("try_catch_catches_nests_and_rethrows", [this]() {
            const std::pair<const char*, const char*> cases[] = {
                {"try { throw \"Test error message\"; } catch (e) { return e; }", "Test error message"},
                {"try { throw \"Error message\"; } catch { return \"Caught without variable\"; }", "Caught without variable"},
                {"try { try { throw \"Inner error\"; } catch (inner) { throw \"Outer error: \" + inner; } } catch (outer) { return outer; }", "Outer error: Inner error"},
                {"try { throw \"Original error\"; } catch (e) { try { throw; } catch (re) { return re; } }", "Original error"},
                {"auto result = \"Start\"; try { result = result + \" \" + throw \"Error in expression\"; } "
                 "catch (e) { result = result + \" caught: \" + e; } return result;", "Start caught: Error in expression"},
                {"auto outer_var = \"outer\"; try { auto inner_var = \"inner\"; throw \"test\"; } catch (e) { return outer_var + \" \" + e; }", "outer test"},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                script_value result = engine->execute(c.first);
                check_eq(result.is_string(), true);
                check_eq(result.as_string(), c.second);
            });
        });

        test("throw_without_catch_bubbles_to_cpp", [this]() {
            auto engine = make_engine();
            
            std::string script = R"(
                throw "Unhandled exception";
            )";
            
            bool caught_exception = false;
            try {
                engine->execute(script);
            } catch (const script_exception& e) {
                caught_exception = true;
                check_eq(std::string(e.what()), "Unhandled exception");
            }
            
            check_eq(caught_exception, true);
        });
        
        test("a_cpp_exception_is_caught_as_its_what_text", [this]() {
            const std::pair<const char*, const char*> cases[] = {
                {"try { risky_function(); } catch (e) { return e; }", "C++ function error"},
                {"try { generic_exception(); } catch (e) { return e; }", "Generic C++ error"},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                engine->add_function("risky_function", []() { throw std::runtime_error("C++ function error"); });
                engine->add_function("generic_exception", []() { throw std::logic_error("Generic C++ error"); });
                script_value result = engine->execute(c.first);
                check_eq(result.is_string(), true);
                check_eq(result.as_string(), c.second);
            });
        });

        test("exception_with_numeric_values", [this]() {
            auto engine = make_engine();
            jai::stdlib::register_all(*engine);

            // the catch variable is the thrown value
            script_value result = engine->execute("try { throw 42; } catch (e) { return e; }");
            check_eq(result.is_int(), true);
            check_eq(result.as<int>(), 42);
            check_eq(std::string("int true 43 42"), engine->execute("try { throw 42; } catch (e) { return reflect::type_of(e) + \" \" + to_string(e == 42) + \" \" + to_string(e + 1) + \" \" + to_string(e); }").as_string());
        });

        test("catch_clauses_take_a_thrown_value_by_type_and_a_run_time_error_by_kind", [this]() {
            const std::string prelude =
                "class Oops { int code = 7; string to_string() { return \"oops!\"; } }\n"
                "class Sub : Oops { }\n"
                "class Other { }\n"
                "int copies = 0;\n"
                "class Counted { int n = 1; Counted() {} Counted(const Counted& o) { n = o.n; copies += 1; } }\n"
                "string which(var thrower) {\n"
                "    try { thrower(); }\n"
                "    catch (Other o) { return \"other\"; }\n"
                "    catch (Oops o) { return \"oops \" + to_string(o.code) + \" \" + reflect::type_of(o); }\n"
                "    catch (int n) { return \"int \" + to_string(n + 1); }\n"
                "    catch (string s) { return \"string \" + s; }\n"
                "    catch (array<int> a) { return \"array \" + to_string(a.size()); }\n"
                "    catch (error e) { return \"error \" + e.kind + \": \" + e.message; }\n"
                "    catch (e) { return \"all \" + reflect::type_of(e); }\n"
                "    return \"none\";\n"
                "}\n";
            const std::vector<std::pair<std::string, std::string>> cases = {
                {"print(which(() => { throw Oops(); }), \"|\", which(() => { throw Sub(); }), \"|\", which(() => { throw Other(); }));", "oops 7 Oops|oops 7 Sub|other"},
                {"print(which(() => { throw 41; }), \"|\", which(() => { throw \"text\"; }), \"|\", which(() => { throw 1.5; }), \"|\", which(() => { throw [1, 2]; }), \"|\", which(() => { }));",
                 "int 42|string text|all float|array 2|none"},
                {"print(which(() => { array<int> a = [1]; return a[3]; }));", "error bounds: Array index 3 out of bounds for array of size 1"},
                {"print(which(() => { map<string, int> m = {}; return m[\"k\"]; }));", "error key_not_found: Key \"k\" not found in map"},
                {"print(which(() => { int z = 0; return 1 / z; }), \"|\", which(() => { int big = 9223372036854775807; return big + 1; }));",
                 "error arithmetic: Division by zero|error arithmetic: Integer overflow in '+'"},
                {"print(which(() => { var v = null; return v.x; }), \"|\", which(() => { var p = Oops(); return p.nope; }));",
                 "error null_receiver: Cannot access 'x' on null ('v' is null)|error no_member: Oops has no member 'nope'"},
                {"print(which(() => { var n = 5; return n(); }), \"|\", which(() => { var f = (a, b) => a; return f(1); }));",
                 "error not_callable: Cannot call 'n': it is an int, not a function|error arguments: lambda expects 2 arguments, got 1"},
                {"print(which(() => { var v = \"seven\"; int n = v; return n; }), \"|\", which(() => { array<int> a = []; return a.pop(); }));",
                 "error type: Cannot initialize 'n' (declared int) with string|error container: Cannot pop from empty array"},
                {"print(which(() => { return to_char(999); }));", "error host: to_char expects 0..255, got 999"},
                // the catch-all's variable is the thrown value; a value with a node also answers its place while the handler runs
                {"try { throw {\"k\": 5}; } catch (e) { print(reflect::type_of(e), \" \", e[\"k\"], \" \", e.line); }", "map 5 17"},
                {"try { throw [1, 2]; } catch (e) { print(reflect::type_of(e), \" \", e.size(), \" \", e[1], \" \", e.line); }", "array 2 2 17"},
                {"try { throw Oops(); } catch (e) { print(reflect::type_of(e), \" \", e.code, \" \", e.line, \" \", e.message, \" \", e, \" \", e.value.code, \" \", e.stack.size()); }", "Oops 7 17 oops! oops! 7 1"},
                {"try { throw \"s\"; } catch (e) { print(reflect::type_of(e), \" \", e, \" \", e.line, \" \", e.value); }", "string s 17 s"},
                {"try { var v = [1]; v[9]; } catch (e) { print(reflect::type_of(e), \" \", e, \"|\", e.kind, \" \", e.kind == error_kind::bounds, \" \", e.line, \" \", e.value); }",
                 "error Array index 9 out of bounds for array of size 1|bounds true 17 null"},
                {"try { var v = [1]; v[9]; } catch (e) { string m = e; print(reflect::type_of(m), \" \", m.size(), \" \", e + \"!\"); }", "string 47 Array index 9 out of bounds for array of size 1!"},
                // a thrown number answers .value and .message through its catch variable, a lambda's copy of it included
                {"int h() { try { throw 3; } catch (e) { var f = [=]() { return e.value * 2; }; return f() + e.value; } return 0; } print(h());\ntry { throw 2.5; } catch (e) { print(e.message + \"!\", \" \", e.value); }", "9\n2.5! 2.5"},
                // the variable is the thrown instance: no copy is made, and a write reaches e.value
                {"try { throw Counted(); } catch (Counted c) { c.n = 5; print(copies, \" \", c.n); }\ntry { throw Counted(); } catch (const Counted& c) { print(copies, \" \", c.n); }\n"
                 "try { throw Counted(); } catch (e) { e.n = 9; print(copies, \" \", e.value.n); }", "0 5\n0 1\n0 9"},
                {"try { throw new Oops(); } catch (Oops o) { print(o.code, \" \", reflect::type_of(o)); }", "7 Oops"},
                // no clause takes it: the error goes on to the next handler from where it was raised
                {"void typed_only() { try { throw \"nope\"; } catch (int n) { print(\"int\"); } }\ntry { typed_only(); } catch (e) { print(\"propagated \", e, \" \", e.line); }", "propagated nope 17"},
                // a try none of whose clauses takes the error is passed without entering a handler: the stack is the raise's
                {"int inner(array<int> a) { return a[5]; } int middle() { return inner([1]); } int outer() { try { return middle(); } catch (int k) { return k; } return -1; }\ntry { outer(); } catch (e) { print(e.line, \" \", e.stack.size()); }", "17 4"},
                {"try { try { var v = [1]; v[9]; } catch (string s) { print(\"wrong\"); } } catch (error e) { print(\"outer \", e.kind, \" \", e.line); }", "outer bounds 17"},
                {"void typed_only() { try { throw 3; } catch (string s) { } }\ntyped_only();", "ERROR 3"},
                {"try { throw \"e\"; } catch (string e) { try { throw 1; } catch (int n) { print(n); } print(e, \" \", e.line); }", "1\ne 17"},
                // a parallel body's handlers take an error by kind too
                {"string bounds = error_kind::bounds; array<int> xs = [1, 2, 3, 4];\nparallel_for (auto& x : xs) { try { var a = [1]; a[x + 5]; } catch (string s) { x = -1; } "
                 "catch (error e) { x = e.kind == bounds ? 100 + x : -2; } }\nprint(xs);", "[101, 102, 103, 104]"},
                {"try { throw 1; } catch (var e) { print(e); }\ntry { throw 2; } catch (auto e) { print(e); }\ntry { throw 3; } catch { print(\"bare\"); }", "1\n2\nbare"},
                {"try { throw 1; } catch (e) { } catch (int n) { }", "ERROR a catch with no type catches everything: it is the last handler of its try"},
                {"try { throw 1; } catch (int n) { } catch { } catch (string s) { }", "ERROR a catch with no type catches everything: it is the last handler of its try"},
                {"try { throw 1; } catch (Nope n) { }", "ERROR unknown type 'Nope'"},
                {"try { throw 1; } catch (int n) { n = \"s\"; }", "ERROR "},
            };
            check_outputs(cases, prelude, true);
        });

        test("a_caught_error_is_its_message_with_its_place_and_stack", [this]() {
            // catch (e) binds a run-time error as its message (every string operation reads it unchanged), of type "error", that
            // answers e.kind, e.message, e.file, e.line, e.column, e.stack (file:line per frame, innermost first) and e.value
            // (null); a copy keeps the answers, a new string made from it does not. A thrown value is the variable itself
            const std::string path = (jai::foundry::test_temp_directory() / "caught_probe.jai").generic_string();
            const std::string runtime_src =
                "int inner(array<int> a) {\n"                                             // 1
                "    return a[5];\n"                                                      // 2
                "}\n"                                                                     // 3
                "int outer() { return inner([1]); }\n"                                    // 4
                "string info = \"\";\n"                                                 // 5
                "try {\n"                                                                 // 6
                "    outer();\n"                                                          // 7
                "} catch (err) {\n"                                                       // 8
                "    var copy = err;\n"                                                   // 9
                "    info = err + \"|\" + err.message + \"|\" + to_string(err.line) + \"|\" + err.stack.join(\",\") + \"|\" + reflect::type_of(err) + \":\" + err.kind +\n"
                "           \"|\" + to_string(err == \"Array index 5 out of bounds for array of size 1\") + \"|\" + to_string(err.value == null) +\n"
                "           \"|\" + to_string(copy.line) + \"|\" + to_string(err.file == \"" + path + "\") + \"|\" + to_string(err.column > 0);\n"
                "    string made = err + \"\";\n"
                "    try { var f = made.file; } catch (inner_err) { info = info + \"|\" + inner_err; }\n"
                "}\n"
                "info;\n";
            const std::string throw_src =
                "class Oops { int code = 7; }\n"                                          // 1
                "void boom() {\n"                                                         // 2
                "    throw Oops();\n"                                                     // 3
                "}\n"                                                                     // 4
                "string r = \"\";\n"                                                    // 5
                "try { boom(); } catch (e) {\n"                                           // 6
                "    r = to_string(e.code) + \"|\" + to_string(e.line) + \"|\" + to_string(e.value.code) + \"|\" + to_string(e.stack.size()) + \"|\" + e.stack[0];\n"
                "}\n"
                "try { throw 42; } catch (e2) { r = r + \"|\" + to_string(e2) + \"|\" + to_string(e2 + 1); }\n"   // 9
                "try { try { throw \"deep\"; } catch (a) { throw; } } catch (b) { r = r + \"|\" + b.value + \"|\" + to_string(b.line); }\n"   // 10
                "r;\n";
            std::string first_runtime, first_throw;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(*e);
                const std::string runtime = e->execute_file_source(path, runtime_src).to_string();
                check_eq(std::string("Array index 5 out of bounds for array of size 1|Array index 5 out of bounds for array of size 1|2|") +
                         path + ":2," + path + ":4," + path + ":7|error:bounds|true|true|2|true|true|string has no member 'file'", runtime);
                const std::string thrown = e->execute_file_source(path, throw_src).to_string();
                check_eq(std::string("7|3|7|2|") + path + ":3|42|43|deep|10", thrown);
                if (!use_vm) { first_runtime = runtime; first_throw = thrown; }
                else { check_eq(first_runtime, runtime, "backends agree"); check_eq(first_throw, thrown, "backends agree"); }
            }
        });
        
        test("throw_captures_nested_stack_trace", [this]() {
            auto engine = make_engine();
            const char* src =
                "auto inner() { throw \"boom\"; }\n"   // line 1
                "auto middle() { return inner(); }\n"  // line 2
                "auto outer() { return middle(); }\n"  // line 3
                "outer();\n";                          // line 4
            try { engine->execute(src); } catch (...) {}

            auto trace = engine->last_stack_trace();
            check_ge(trace.size(), (size_t)4);
            check_eq(std::string("inner"), trace[0].function);
            check_eq(std::string("middle"), trace[1].function);
            check_eq(std::string("outer"), trace[2].function);
            check_eq(std::string("<script>"), trace[3].function);
            check_eq((size_t)1, trace[0].line);
            check_eq((size_t)2, trace[1].line);
            check_eq((size_t)3, trace[2].line);
            check_eq((size_t)4, trace[3].line);
            // calls the vm grafts into the top level keep their frames
            auto looped = make_engine();
            failure_text(*looped,
                "int half(int n) { return 10 / n; }\n"                       // line 1
                "int outer(int k) { return half(k) + 1; }\n"                 // line 2
                "int s = 0;\n"                                               // line 3
                "for (int i = 3; i >= 0; i--) { s = s + outer(i); }\n");     // line 4
            check_eq(std::string("half:1|outer:2|<script>:4"), trace_shape(*looped));
        });

        test("a_throw_in_construction_code_traces_the_level_running_it", [this]() {
            // each constructor's activation is a frame, as a C++ debugger shows member initialization: a field initializer and an
            // initializer-list argument stand at their line, a level constructing its base at the base's argument or its own line
            const std::string chain =
                "int boom(int v) {\n"
                "    if (at == v) { throw `boom ${v}`; }\n"
                "    return v;\n"
                "}\n"
                "class A {\n"
                "    int a = boom(1);\n"
                "    A(int x) {\n"
                "        boom(2);\n"
                "    }\n"
                "}\n"
                "class B : A {\n"
                "    int b = boom(3);\n"
                "    B() : super(boom(4)) {\n"
                "        boom(5);\n"
                "    }\n"
                "}\n"
                "class C : B {\n"
                "    int c = boom(6);\n"
                "    C() : super() {\n"
                "        boom(7);\n"
                "    }\n"
                "}\n"
                "C();\n";
            const char* chain_shapes[] = {"boom:3|A:7|B:14|C:20|<script>:24", "boom:3|A:9|B:14|C:20|<script>:24", "boom:3|B:13|C:20|<script>:24", "boom:3|B:14|C:20|<script>:24", "boom:3|B:15|C:20|<script>:24", "boom:3|C:19|<script>:24", "boom:3|C:21|<script>:24"};
            for (int at = 1; at <= 7; ++at) {
                auto engine = make_engine();
                failure_text(*engine, ("int at = " + std::to_string(at) + ";\n" + chain).c_str());
                check_eq(std::string(chain_shapes[at - 1]), trace_shape(*engine), "a three-level chain, throw point " + std::to_string(at));
            }
            const std::string shapes =
                "int boom(int v) {\n"
                "    if (at == v) { throw `boom ${v}`; }\n"
                "    return v;\n"
                "}\n"
                "class K {\n"
                "    int k = boom(1);\n"
                "}\n"
                "class P {\n"
                "    int p = boom(2);\n"
                "}\n"
                "class B : P {\n"
                "    B(int x) {}\n"
                "}\n"
                "class D : B {\n"
                "    D() : super(1) {}\n"
                "}\n"
                "class E {\n"
                "    int e = 0;\n"
                "    E(int v) {}\n"
                "    E() : this(boom(3)) {}\n"
                "}\n"
                "class L {\n"
                "    L(int v) {}\n"
                "}\n"
                "class R {\n"
                "    R(int v) {}\n"
                "}\n"
                "class M : L, R {\n"
                "    M() : L(1), R(boom(4)) {}\n"
                "}\n"
                "class Inner {\n"
                "    int i = boom(5);\n"
                "}\n"
                "class Outer {\n"
                "    Inner held = Inner();\n"
                "    Outer() {}\n"
                "}\n"
                "int f() {\n"
                "    if (at == 1) { K(); }\n"
                "    if (at == 2) { D(); }\n"
                "    if (at == 3) { E(); }\n"
                "    if (at == 4) { M(); }\n"
                "    if (at == 5) { Outer(); }\n"
                "    return 0;\n"
                "}\n"
                "f();\n";
            const char* other_shapes[] = {"boom:3|K:7|f:40|<script>:47", "boom:3|P:10|B:13|D:16|f:41|<script>:47", "boom:3|E:21|f:42|<script>:47", "boom:3|M:30|f:43|<script>:47", "boom:3|Inner:33|Outer:36|f:44|<script>:47"};
            const char* what[] = {"a class without a constructor", "a base without a constructor under a base level", "a delegation's argument",
                                  "a later base's argument", "a field initializer that constructs"};
            for (int at = 1; at <= 5; ++at) {
                auto engine = make_engine();
                failure_text(*engine, ("int at = " + std::to_string(at) + ";\n" + shapes).c_str());
                check_eq(std::string(other_shapes[at - 1]), trace_shape(*engine), what[at - 1]);
            }
        });

        test("an_aggregates_field_initializer_traces_its_class", [this]() {
            // the fields an aggregate leaves to their initializers run in a frame of the class, as a class without a constructor does
            const std::string aggregate =
                "int boom(int v) {\n"
                "    throw `boom ${v}`;\n"
                "}\n"
                "class K {\n"
                "    int v = 1;\n"
                "    int w = at == 1 ? boom(v) : [1, 2][v + 5];\n"
                "}\n"
                "K{4};\n";
            const char* shapes[] = {"boom:3|K:7|<script>:9", "K:7|<script>:9"};
            for (int at = 1; at <= 2; ++at) {
                auto engine = make_engine();
                failure_text(*engine, ("int at = " + std::to_string(at) + ";\n" + aggregate).c_str());
                check_eq(std::string(shapes[at - 1]), trace_shape(*engine), at == 1 ? "a throw from a call" : "a run-time error of its own");
            }
        });

        test("a_declaration_that_fails_at_load_traces_its_own_statement", [this]() {
            // each member of a namespace block is a statement of its own: a failing class, variable or function stands at its line,
            // whatever ran before it in the block, and a static's initializer at its own line, as a field's does
            const std::string prelude = "int boom() { throw \"boom\"; } var text() { return \"s\"; }\n";   // line 1
            const std::filesystem::path dir = jai::foundry::test_temp_directory();
            { std::ofstream(dir / "trace_declaring.jai") << "namespace shop { int tax(int p) { return p + 1; } }\n"; }
            { std::ofstream(dir / "trace_again.jai") << "int q = 1;\nnamespace shop {\n    class K { int k = 1; }\n    int a = 1;\n    int tax(int p) { return 0; }\n}\n"; }
            { std::ofstream(dir / "trace_nested.jai") << "namespace shop {\n    namespace inner { int a = 1; }\n    int tax(int p) { return 0; }\n}\n"; }
            check_each(std::vector<std::pair<std::string, std::string>>{
                {"namespace shop {\n    int a = 1;\n    class Crate : Item { int both() { return 1; } }\n}\n", "<script>:4"},
                {"namespace shop {\n    int a = 1;\n    int b = boom();\n}\n", "boom:1|<script>:4"},
                {"namespace shop {\n    class K { int k = 1; }\n    int b =\n        boom();\n}\n", "boom:1|<script>:4"},
                {"namespace shop {\n    int a = 1;\n    namespace inner {\n        int c = 2;\n        int b = boom();\n    }\n}\n", "boom:1|<script>:6"},
                {"namespace shop {\n    int a = 1;\n    namespace inner {\n        int c = 2;\n        class Crate : Item { }\n    }\n}\n", "<script>:6"},
                {"namespace shop {\n    int a = 1;\n    class K {\n        static int s =\n            boom();\n    }\n}\n", "boom:1|<script>:6"},
                {"int a = 1;\nclass K {\n    static int s =\n        boom();\n}\n", "boom:1|<script>:5"},
                // what the class's declaration raises after an initializer ran stands at the class
                {"namespace shop {\n    int a = 1;\n    class K {\n        static int ok = a + 1;\n        static int s =\n            text();\n    }\n}\n", "<script>:4"},
                {"int z = 0; namespace shop { int a = 1; class Crate : Item { } }\n", "<script>:2"},
                {"int z = 0; namespace shop { int a = 1; int b = boom(); }\n", "boom:1|<script>:2"},
                // a function another file's block declared: the redefinition stands at the function, in the importing frame's file
                {"import \"trace_declaring.jai\";\nimport \"trace_again.jai\";\n", "<script>:5|<script>:3"},
                {"import \"trace_declaring.jai\";\nvoid load() {\n    import \"trace_nested.jai\";\n}\nload();\n", "<script>:3|load:4|<script>:6"},
            }, [&](const auto& c) {
                auto engine = make_engine();
                engine->add_include_path(dir.string());
                check_true(!failure_text(*engine, (prelude + c.first).c_str()).empty(), "the load fails");
                check_eq(c.second, trace_shape(*engine));
            });
            for (const char* file : {"trace_declaring.jai", "trace_again.jai", "trace_nested.jai"}) { std::filesystem::remove(dir / file); }
        });

        // An error raised when no run is in flight (a function the host calls, a destructor at teardown) stands in no top-level
        // statement: its script frame names none, and reads nothing of a run whose jaibite the host has since dropped
        test("an_error_outside_a_run_names_no_statement_of_an_earlier_one", [this]() {
            for (const bool dropped : {false, true}) {
                auto engine = make_engine();
                std::optional<jai::jaibite> bite;
                bite.emplace(engine->jaibite("int risky(int x) { return 10 / x; }" + std::string(1, char(10)) + "int last = 1;" + std::string(1, char(10)) + "last;"));
                check_eq(int64_t(1), bite->execute().as_int());
                if (dropped) { bite.reset(); }
                auto result = engine->get_variable("risky").as_function()({engine->make_value(int64_t{0})});
                check_false(bool(result), "the call fails");
                const auto trace = engine->last_stack_trace();
                check_false(trace.empty());
                check_eq(std::string("<script>"), trace.back().function);
                check_eq(size_t(0), trace.back().line, "the script frame stands at no statement");
            }
            auto engine = make_engine();
            std::optional<jai::jaibite> bite;
            bite.emplace(engine->jaibite("class Late { int n = 0; ~Late() { int z = 0; n = 1 / z; } } Late keep; 1;"));
            check_eq(int64_t(1), bite->execute().as_int());
            bite.reset();
            engine.reset();   // the destructor raises at teardown, after the bite's statements are gone
        });

        test("top_level_throw_captures_script_frame", [this]() {
            auto engine = make_engine();
            try { engine->execute("throw \"x\";"); } catch (...) {}
            auto trace = engine->last_stack_trace();
            check_false(trace.empty());
            check_eq(std::string("<script>"), trace.back().function);
        });

        test("execute_file_stamps_stack_frame_file", [this]() {
            // execute_file must attribute the script to its path: every AST node's
            // location carries the file, observable via the captured stack trace.
            auto write_tmp = [](const std::string& name, const std::string& body) {
                std::filesystem::path p = jai::foundry::test_temp_directory() / name;
                std::ofstream(p) << body;
                return p.string();
            };
            auto engine = make_engine();

            std::string fileA = write_tmp("jaidbg_creature.jai", "throw \"x\";");
            try { engine->execute_file(fileA); } catch (...) {}
            auto trace = engine->last_stack_trace();
            check_false(trace.empty());
            check_eq(fileA, trace.back().file);

            // Identical content under a DIFFERENT path must not alias the cached AST:
            // the parse cache keys on (sourcePath, content), not content alone.
            std::string fileB = write_tmp("jaidbg_missile.jai", "throw \"x\";");
            try { engine->execute_file(fileB); } catch (...) {}
            auto trace2 = engine->last_stack_trace();
            check_false(trace2.empty());
            check_eq(fileB, trace2.back().file);

            std::filesystem::remove(fileA);
            std::filesystem::remove(fileB);
            check(std::filesystem::equivalent(std::filesystem::path(fileA).parent_path(), jai::foundry::test_temp_directory()),
                  "File fixtures live in the run's own directory");
        });

        test("current_and_root_source_file_follow_the_executing_frame", [this]() {
            // root = the file the outermost execute was given, kept afterwards; current = the
            // file stamped on the innermost executing statement, so an imported module's
            // function answers with the module while its caller answers with the root.
            namespace fs = std::filesystem;
            fs::path dir = jai::foundry::test_temp_directory() / "jai_srcfile_a";
            fs::create_directories(dir / "lib");
            std::ofstream(dir / "lib" / "mod.jai") << "function where() -> string { return probe(); }\n";
            std::ofstream(dir / "main.jai") << "import \"lib/mod.jai\";\nvar seen = [probe(), where()];\nfunction later() -> string { return probe(); }\n";
            auto engine = make_engine();
            jai::engine* raw = engine.get();
            engine->add_function("probe", [raw]() -> std::string { return detail::engine_access::current_source_file(*raw); });
            check_eq(std::string(), detail::engine_access::root_source_file(*engine));
            check_eq(std::string(), detail::engine_access::current_source_file(*engine));
            engine->add_include_path(dir.string());   // as the runner does for the entry script's folder
            std::string mainPath = (dir / "main.jai").string();
            engine->execute_file(mainPath);
            check_eq(mainPath, detail::engine_access::root_source_file(*engine));
            auto seen = engine->execute("seen");
            check_eq(fs::path(mainPath).lexically_normal(), fs::path(seen.as_array()[0].as_string()).lexically_normal());
            check_eq((dir / "lib" / "mod.jai").lexically_normal(), fs::path(seen.as_array()[1].as_string()).lexically_normal());
            check_eq(mainPath, detail::engine_access::root_source_file(*engine));   // a raw-content execute does not replace the root
            check_eq(fs::path(mainPath).lexically_normal(), fs::path(engine->execute("later()").as_string()).lexically_normal());
            std::error_code ec;
            fs::remove_all(dir, ec);
        });

        test("jaibite_siblings_name_their_files_after_the_folder_moves", [this]() {
            // The jaibite cache marks the unit's own file instead of storing its path, so a
            // copied or moved folder loads its bites and every frame still names the file
            // where it now lives (stack traces, breakpoints, current_source_file).
            namespace fs = std::filesystem;
            fs::path a = jai::foundry::test_temp_directory() / "jai_srcfile_move_a";
            fs::path b = jai::foundry::test_temp_directory() / "jai_srcfile_move_b";
            std::error_code ec;
            fs::remove_all(a, ec);
            fs::remove_all(b, ec);
            fs::create_directories(a / "lib");
            std::ofstream(a / "lib" / "mod.jai") << "function where() -> string { return probe(); }\n";
            std::ofstream(a / "main.jai") << "import \"lib/mod.jai\";\nvar seen = [probe(), where()];\n";
            auto first = make_engine();
            jai::engine* rawFirst = first.get();
            first->add_function("probe", [rawFirst]() -> std::string { return detail::engine_access::current_source_file(*rawFirst); });
            first->add_include_path(a.string());
            first->execute_file((a / "main.jai").string());
            check_true(fs::exists(a / ".cache" / "main.jai.jaibite"));
            check_true(fs::exists(a / "lib" / ".cache" / "mod.jai.jaibite"));

            fs::copy(a, b, fs::copy_options::recursive, ec);
            check_false(bool(ec));
            const auto mainBiteBefore = fs::last_write_time(b / ".cache" / "main.jai.jaibite", ec);
            auto second = make_engine();
            jai::engine* rawSecond = second.get();
            second->add_function("probe", [rawSecond]() -> std::string { return detail::engine_access::current_source_file(*rawSecond); });
            second->add_include_path(b.string());
            second->execute_file((b / "main.jai").string());
            auto seen = second->execute("seen");
            check_eq((b / "main.jai").lexically_normal(), fs::path(seen.as_array()[0].as_string()).lexically_normal());
            check_eq((b / "lib" / "mod.jai").lexically_normal(), fs::path(seen.as_array()[1].as_string()).lexically_normal());
            check_true(fs::last_write_time(b / ".cache" / "main.jai.jaibite", ec) == mainBiteBefore);   // loaded, not rewritten
            fs::remove_all(a, ec);
            fs::remove_all(b, ec);
        });

        test("import_resolves_from_the_importing_file_then_include_paths_then_cwd", [this]() {
            // One ladder for import/include, the checker's closure gather and resolve_path: the
            // importing file's folder first (a module imports its sibling by bare name), the
            // include paths next (a host's `Assets` rung), the working directory last.
            namespace fs = std::filesystem;
            fs::path dir = jai::foundry::test_temp_directory() / "jai_ladder";
            fs::path inc = jai::foundry::test_temp_directory() / "jai_ladder_inc";
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::remove_all(inc, ec);
            fs::create_directories(dir / "lib");
            fs::create_directories(inc);
            std::ofstream(dir / "lib" / "helper.jai") << "var helper_home = current_script_dir();\nfunction helper_value() -> int { return 41; }\n";
            std::ofstream(dir / "lib" / "shadow.jai") << "var shadow_lib = true;\n";
            std::ofstream(dir / "shadow.jai") << "var shadow_root = true;\n";
            std::ofstream(inc / "only.jai") << "function only_value() -> int { return 100; }\n";
            std::ofstream(dir / "lib" / "a.jai") << "import \"helper.jai\";\nimport \"only.jai\";\nvar from_lib = resolve_path(\"shadow.jai\");\nfunction a_value() -> int { return helper_value() + only_value(); }\n";
            std::ofstream(dir / "main.jai") << "import \"lib/a.jai\";\nvar v = a_value();\nvar from_root = resolve_path(\"shadow.jai\");\nvar found = resolve_path(\"lib/helper.jai\");\nvar missing = resolve_path(\"nope.jai\");\n";
            auto engine = make_engine();
            engine->add_include_path(dir.string());
            engine->add_include_path(inc.string());
            engine->execute_file((dir / "main.jai").string());
            check_eq((int64_t)141, engine->execute("v").as_int());
            auto norm = [](const std::string& p) { return fs::path(p).lexically_normal().generic_string(); };
            check_eq(norm((dir / "lib" / "shadow.jai").string()), norm(engine->execute("from_lib").as_string()));
            check_eq(norm((dir / "shadow.jai").string()), norm(engine->execute("from_root").as_string()));
            check_eq(norm((dir / "lib" / "helper.jai").string()), norm(engine->execute("found").as_string()));
            check_eq(std::string(), engine->execute("missing").as_string());
            check_eq(norm((dir / "lib").string()), norm(engine->execute("helper_home").as_string()));
            check_eq(norm((dir / "main.jai").string()), norm(engine->execute("root_script_path()").as_string()));
            check_eq(norm(dir.string()), norm(engine->execute("root_script_dir()").as_string()));
            check_true(!engine->execute("executable_dir()").as_string().empty());
            check_true(engine->execute("bin_dir() == executable_dir()").as_bool());
            check_eq(std::string("main.jai"), engine->execute("file_name(root_script_path())").as_string());
            check_eq(norm(dir.string()), norm(engine->execute("parent_dir(root_script_path())").as_string()));
            fs::remove_all(dir, ec);
            fs::remove_all(inc, ec);
        });

        test("app_directories_answer_through_the_host_resolvers", [this]() {
            auto engine = make_engine();
            check_true(!engine->execute("app_data_dir(\"JaiTestApp\")").as_string().empty());
            check_true(engine->execute("app_data_dir(\"JaiTestApp\").ends_with(\"JaiTestApp\")").as_bool());
            check_true(!engine->execute("app_cache_dir(\"JaiTestApp\")").as_string().empty());
            engine->app_directory_resolvers([](const std::string& app) { return "/sandbox/data/" + app; },
                                            [](const std::string& app) { return "/sandbox/cache/" + app; });
            check_eq(std::string("/sandbox/data/JaiTestApp"), engine->execute("app_data_dir(\"JaiTestApp\")").as_string());
            check_eq(std::string("/sandbox/cache/JaiTestApp"), engine->execute("app_cache_dir(\"JaiTestApp\")").as_string());
        });

        test("format_stack_trace_lists_frames", [this]() {
            auto engine = make_engine();
            try { engine->execute("auto f() { throw \"x\"; }\nf();\n"); } catch (...) {}
            auto s = engine->format_stack_trace();
            check_true(s.find("at f") != std::string::npos);
        });

        test("runtime_error_captures_nested_stack_trace", [this]() {
            const char* src =
                "auto inner() { return missingVar; }\n"  // line 1: undefined variable
                "auto middle() { return inner(); }\n"     // line 2
                "auto outer() { return middle(); }\n"     // line 3
                "outer();\n";                             // line 4
            // strict (default): the undeclared name is a load error - nothing ran, no trace
            auto strict = make_engine();
            check_true(static_error_text(*strict, src).find("use of undeclared identifier 'missingVar'") != std::string::npos);
            check_true(strict->last_stack_trace().empty());
            // a warn region: the runtime error unwinds through all three frames
            auto engine = make_engine();
            try { engine->execute(std::string("[[warn_mode]]; ") + src); } catch (...) {}
            auto trace = engine->last_stack_trace();
            check_ge(trace.size(), (size_t)4);
            check_eq(std::string("inner"), trace[0].function);
            check_eq(std::string("middle"), trace[1].function);
            check_eq(std::string("outer"), trace[2].function);
            check_eq((size_t)1, trace[0].line);
            // strict admits a var-sourced runtime failure and captures the same trace shape
            auto dynamic = make_engine();
            const char* var_src =
                "var v = \"s\";\n"                        // line 1
                "auto inner() { int i = v; return i; }\n" // line 2: runtime conversion error
                "auto middle() { return inner(); }\n"     // line 3
                "auto outer() { return middle(); }\n"     // line 4
                "outer();\n";                             // line 5
            try { dynamic->execute(var_src); } catch (...) {}
            auto trace2 = dynamic->last_stack_trace();
            check_ge(trace2.size(), (size_t)4);
            check_eq(std::string("inner"), trace2[0].function);
            check_eq(std::string("middle"), trace2[1].function);
            check_eq(std::string("outer"), trace2[2].function);
            check_eq((size_t)2, trace2[0].line);
        });

        test("method_throw_captures_stack_trace", [this]() {
            auto engine = make_engine();
            const char* src =
                "class Foo {\n"
                "    auto boom() { throw \"x\"; }\n"   // line 2
                "}\n"
                "auto f = Foo();\n"
                "f.boom();\n";                          // line 5
            try { engine->execute(src); } catch (...) {}
            auto trace = engine->last_stack_trace();
            check_false(trace.empty());
            check_eq((size_t)2, trace[0].line);
            check_eq(std::string("boom"), trace[0].function);
        });

        test("lambda_throw_captures_stack_trace", [this]() {
            auto engine = make_engine();
            const char* src =
                "auto fn = []() -> auto { throw \"x\"; };\n"  // line 1
                "fn();\n";                                     // line 2
            try { engine->execute(src); } catch (...) {}
            auto trace = engine->last_stack_trace();
            check_false(trace.empty());
            check_eq((size_t)1, trace[0].line);
        });

        // ---- A typed return whose conversion fails raises AT the return statement: the
        // returning function is the top frame and the call site sits below it, and the
        // text names the value's own type, never its var tag. ----
        test("return_conversion_error_attributed_to_return_site", [this]() {
            auto engine = make_engine();
            const char* src =
                "int f() {\n"                 // 1
                "  var x = \"s\";\n"          // 2
                "  return x;\n"               // 3
                "}\n"                         // 4
                "int g() { return 1; }\n"     // 5
                "int a = g();\n"              // 6
                "int b = f();\n";             // 7
            std::string what = failure_text(*engine, src);
            check_true(what.find("Cannot return string from 'f' (declared -> int)") == 0, "text names the value type (got: " + what + ")");
            check_true(what.find("got any") == std::string::npos, "no var tag in the text");
            check_eq(std::string("f:3|<script>:7"), trace_shape(*engine));
            check_eq(std::string("  at f (<script>:3)\n  at <script> (<script>:7)\n"), engine->format_stack_trace());
        });

        test("a_return_error_is_raised_at_the_return_statement_of_every_callee_kind", [this]() {
            const std::tuple<const char*, const char*, const char*> cases[] = {
                {"int f() {\n"                 // 1
                 "  var x = \"s\";\n"          // 2
                 "  return x;\n"               // 3
                 "}\n"                         // 4
                 "int g() {\n"                 // 5
                 "  int r = f();\n"            // 6
                 "  return r;\n"               // 7
                 "}\n"                         // 8
                 "int b = g();\n",
                 "Cannot return string from 'f' (declared -> int)", "f:3|g:6|<script>:9"},
                {"var x = \"s\";\n"            // 1
                 "auto l = []() -> int {\n"    // 2
                 "  return x;\n"               // 3
                 "};\n"                        // 4
                 "int b = l();\n",
                 "Cannot return string from lambda (declared -> int)", "<lambda>:3|<script>:5"},
                {"class C {\n"                 // 1
                 "  int m() {\n"               // 2
                 "    var x = \"s\";\n"        // 3
                 "    return x;\n"             // 4
                 "  }\n"                       // 5
                 "}\n"                         // 6
                 "auto c = C();\n"             // 7
                 "int b = c.m();\n",
                 "Cannot return string from 'm' (declared -> int)", "m:4|<script>:8"},
                {"int& f() {\n"                // 1
                 "  return 5;\n"               // 2
                 "}\n"                         // 3
                 "int b = f();\n",
                 "Function with reference return type must return an lvalue reference", "f:2|<script>:4"},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                std::string what = failure_text(*engine, std::get<0>(c));
                check_true(what.find(std::get<1>(c)) == 0, "text (got: " + what + ")");
                check_eq(std::string(std::get<2>(c)), trace_shape(*engine));
            });
        });

        test("coroutine_typed_return_conversion_attributed", [this]() {
            // Second resume at top level: the coroutine frame is on top of the resume site
            {
                auto engine = make_engine();
                const char* src =
                    "coroutine int co() {\n"  // 1
                    "  yield 1;\n"            // 2
                    "  var x = \"s\";\n"      // 3
                    "  return x;\n"           // 4
                    "}\n"                     // 5
                    "auto h = co();\n"        // 6
                    "var a = h.next();\n"   // 7
                    "var b = h.next();\n";  // 8
                std::string what = failure_text(*engine, src);
                check_true(what.find("Cannot return string from 'co' (declared -> int)") == 0, "top-level resume text (got: " + what + ")");
                check_eq(std::string("co:4|<script>:8"), trace_shape(*engine));
            }
            // Resumed inside a function: the resumer's frames stay under the coroutine's
            {
                auto engine = make_engine();
                const char* src =
                    "coroutine int co() {\n"  // 1
                    "  yield 1;\n"            // 2
                    "  var x = \"s\";\n"      // 3
                    "  return x;\n"           // 4
                    "}\n"                     // 5
                    "auto h = co();\n"        // 6
                    "int g() {\n"             // 7
                    "  h.next();\n"         // 8
                    "  return h.next();\n"  // 9
                    "}\n"                     // 10
                    "int b = g();\n";         // 11
                std::string what = failure_text(*engine, src);
                check_true(what.find("Cannot return string from 'co' (declared -> int)") == 0, "nested resume text (got: " + what + ")");
                check_eq(std::string("co:4|g:9|<script>:11"), trace_shape(*engine));
            }
            // First resume runs to completion
            {
                auto engine = make_engine();
                const char* src =
                    "coroutine int co() {\n"  // 1
                    "  var x = \"s\";\n"      // 2
                    "  return x;\n"           // 3
                    "}\n"                     // 4
                    "auto h = co();\n"        // 5
                    "int g() {\n"             // 6
                    "  return h.next();\n"  // 7
                    "}\n"                     // 8
                    "int b = g();\n";         // 9
                std::string what = failure_text(*engine, src);
                check_true(what.find("Cannot return string from 'co' (declared -> int)") == 0, "first-resume text (got: " + what + ")");
                check_eq(std::string("co:3|g:7|<script>:9"), trace_shape(*engine));
            }
        });

        // Same capture site: any error raised by a resumed segment keeps the coroutine frame
        test("coroutine_body_error_keeps_coroutine_frame", [this]() {
            {
                auto engine = make_engine();
                const char* src =
                    "coroutine int co() {\n"  // 1
                    "  yield 1;\n"            // 2
                    "  var a = [];\n"         // 3
                    "  int y = a[0];\n"       // 4
                    "  return y;\n"           // 5
                    "}\n"                     // 6
                    "auto h = co();\n"        // 7
                    "var a = h.next();\n"   // 8
                    "var b = h.next();\n";  // 9
                std::string what = failure_text(*engine, src);
                check_true(what.find("Array index 0 out of bounds") == 0, "index error text (got: " + what + ")");
                check_eq(std::string("co:4|<script>:9"), trace_shape(*engine));
            }
            {
                auto engine = make_engine();
                const char* src =
                    "coroutine int co() {\n"  // 1
                    "  yield 1;\n"            // 2
                    "  throw \"x\";\n"        // 3
                    "  return 1;\n"           // 4
                    "}\n"                     // 5
                    "auto h = co();\n"        // 6
                    "var a = h.next();\n"   // 7
                    "var b = h.next();\n";  // 8
                std::string what = failure_text(*engine, src);
                check_eq(std::string("x"), what);
                check_eq(std::string("co:3|<script>:8"), trace_shape(*engine));
            }
        });

        // ---- returncatch (C++ semantics): the typed-return conversion is part of the
        // `return` statement, so a try/catch around the failing return INSIDE the callee
        // catches it; uncaught, the attribution and text above are unchanged. ----
        test("return_conversion_error_caught_at_return_site", [this]() {
            auto engine = make_engine();
            script_value result = engine->execute(
                "int f() {\n"
                "  try {\n"
                "    var x = \"s\";\n"
                "    return x;\n"
                "  } catch (e) {\n"
                "    return -1;\n"
                "  }\n"
                "}\n"
                "int r = 0;\n"
                "try { r = f(); } catch (e) { r = -99; }\n"
                "r\n");
            check_eq((int64_t)-1, result.as_int());
            // A caught return-conversion error leaves no stale trace behind for a later error
            auto later = make_engine();
            const char* src =
                "int f() {\n"                                   // 1
                "  var x = \"s\";\n"                            // 2
                "  return x;\n"                                 // 3
                "}\n"                                           // 4
                "try { int v = f(); } catch (e) {}\n"           // 5
                "throw \"later\";\n";                           // 6
            check_eq(std::string("later"), failure_text(*later, src));
            check_eq(std::string("<script>:6"), trace_shape(*later));
            // ...nor does one caught inside the callee
            auto inner = make_engine();
            const char* src2 =
                "int f() {\n"                                                        // 1
                "  try { var x = \"s\"; return x; } catch (e) { return -1; }\n"      // 2
                "}\n"                                                                // 3
                "int v = f();\n"                                                     // 4
                "throw \"later\";\n";                                                // 5
            check_eq(std::string("later"), failure_text(*inner, src2));
            check_eq(std::string("<script>:5"), trace_shape(*inner));
        });

        test("return_conversion_caught_value_is_the_error_text", [this]() {
            auto engine = make_engine();
            script_value result = engine->execute(
                "int f() {\n"
                "  try { var x = \"s\"; return x; } catch (e) { throw \"caught:\" + e; }\n"
                "}\n"
                "string r = \"\";\n"
                "try { f(); } catch (e) { r = e; }\n"
                "r\n");
            check_eq(std::string("caught:Cannot return string from 'f' (declared -> int)"), result.as_string());
        });

        test("a_return_conversion_error_is_caught_inside_its_callee", [this]() {
            const std::pair<const char*, int64_t> cases[] = {
                {"int f() {\n"
                "  try {\n"
                "    try {\n"
                "      var x = \"s\";\n"
                "      return x;\n"
                "    } catch (e) {\n"
                "      if (e == \"other\") { return -2; }\n"
                "      throw e;\n"
                "    }\n"
                "  } catch (e2) {\n"
                "    return -3;\n"
                "  }\n"
                "}\n"
                "f()\n", (int64_t)-3},
                {"class C {\n"
                "  int m() {\n"
                "    try { var x = \"s\"; return x; } catch (e) { return -1; }\n"
                "  }\n"
                "}\n"
                "auto c = C();\n"
                "c.m()\n", (int64_t)-1},
                {"var x = \"s\";\n"
                "auto l = []() -> int {\n"
                "  try { return x; } catch (e) { return -1; }\n"
                "};\n"
                "l()\n", (int64_t)-1},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_eq(c.second, engine->execute(c.first).as_int());
            });
        });

        test("return_conversion_catch_rethrow_attributed_to_throw", [this]() {
            auto engine = make_engine();
            const char* src =
                "int f() {\n"                 // 1
                "  try {\n"                   // 2
                "    var x = \"s\";\n"        // 3
                "    return x;\n"             // 4
                "  } catch (e) {\n"           // 5
                "    throw e;\n"              // 6
                "  }\n"                       // 7
                "}\n"                         // 8
                "int b = f();\n";             // 9
            check_eq(std::string("Cannot return string from 'f' (declared -> int)"), failure_text(*engine, src));
            check_eq(std::string("f:6|<script>:9"), trace_shape(*engine));
        });

        test("coroutine_return_conversion_caught_at_return_site", [this]() {
            {
                auto engine = make_engine();
                check_eq((int64_t)1, engine->execute(
                    "coroutine int co() {\n"
                    "  yield 1;\n"
                    "  try { var x = \"s\"; return x; } catch (e) { return -1; }\n"
                    "}\n"
                    "auto h = co();\n"
                    "h.next()\n").as_int());
                check_eq((int64_t)-1, engine->execute("h.next()").as_int());
                check_true(engine->execute("h.done()").as_bool(), "completed after the caught return");
            }
            {
                auto engine = make_engine();
                check_eq((int64_t)-1, engine->execute(
                    "coroutine int co() {\n"
                    "  try { var x = \"s\"; return x; } catch (e) { return -1; }\n"
                    "}\n"
                    "auto h = co();\n"
                    "h.next()\n").as_int());
                check_true(engine->execute("h.done()").as_bool(), "first resume completed");
            }
            // Uncaught: the row-29 text/trace hold and the handle reads done (failed) afterwards
            {
                auto engine = make_engine();
                const char* src =
                    "coroutine int co() {\n"  // 1
                    "  yield 1;\n"            // 2
                    "  var x = \"s\";\n"      // 3
                    "  return x;\n"           // 4
                    "}\n"                     // 5
                    "auto h = co();\n"        // 6
                    "var a = h.next();\n"   // 7
                    "var b = h.next();\n";  // 8
                std::string what = failure_text(*engine, src);
                check_true(what.find("Cannot return string from 'co' (declared -> int)") == 0, "uncaught text (got: " + what + ")");
                check_eq(std::string("co:4|<script>:8"), trace_shape(*engine));
                check_true(engine->execute("h.done()").as_bool(), "a failed handle reads as done");
            }
        });

        test("ref_return_error_caught_at_return_site", [this]() {
            auto engine = make_engine();
            script_value result = engine->execute(
                "int g = 5;\n"
                "int& f() {\n"
                "  try { return 5; } catch (e) { return g; }\n"
                "}\n"
                "f()\n");
            check_eq((int64_t)5, result.as_int());
            auto texts = make_engine();
            script_value caught = texts->execute(
                "int& f() {\n"
                "  try { return 5; } catch (e) { throw \"c:\" + e; }\n"
                "}\n"
                "string r = \"\";\n"
                "try { f(); } catch (e) { r = e; }\n"
                "r\n");
            check_eq(std::string("c:Function with reference return type must return an lvalue reference"), caught.as_string());
        });

        test("throwing_conversion_caught_at_return_site", [this]() {
            auto engine = make_engine();
            script_value result = engine->execute(
                "class T { int to_int() { throw \"boom\"; } }\n"
                "int f() {\n"
                "  try { var t = T(); return t; } catch (e) { return e == \"boom\" ? -7 : -8; }\n"
                "}\n"
                "f()\n");
            check_eq((int64_t)-7, result.as_int());
            auto ctor = make_engine();
            script_value made = ctor->execute(
                "class Cat { int v = 0; Cat(int n) { if (n == 1) { throw \"ctorboom\"; } v = n; } }\n"
                "Cat f() {\n"
                "  try { var v = 1; return v; } catch (e) { return Cat(2); }\n"
                "}\n"
                "auto c = f();\n"
                "c.v\n");
            check_eq((int64_t)2, made.as_int());
        });

        test("catch_return_converts_in_turn", [this]() {
            auto engine = make_engine();
            const char* src =
                "int f() {\n"                 // 1
                "  try {\n"                   // 2
                "    var x = \"s\";\n"        // 3
                "    return x;\n"             // 4
                "  } catch (e) {\n"           // 5
                "    var y = \"t\";\n"        // 6
                "    return y;\n"             // 7
                "  }\n"                       // 8
                "}\n"                         // 9
                "int b = f();\n";             // 10
            std::string what = failure_text(*engine, src);
            check_true(what.find("Cannot return string from 'f' (declared -> int)") == 0, "text (got: " + what + ")");
            check_eq(std::string("f:7|<script>:10"), trace_shape(*engine));
            // ...and an outer try in the same function catches the catch's own return
            auto outer = make_engine();
            script_value result = outer->execute(
                "int f() {\n"
                "  try {\n"
                "    try { var x = \"s\"; return x; } catch (e) { var y = \"t\"; return y; }\n"
                "  } catch (e2) { return -4; }\n"
                "}\n"
                "f()\n");
            check_eq((int64_t)-4, result.as_int());
        });

        test("bare_return_in_typed_fn_inside_try_caught", [this]() {
            auto engine = make_engine();
            script_value result = engine->execute(
                "[[warn_mode]]; int f() {\n"   // strict rejects the bare return at load
                "  try { return; } catch (e) { return -1; }\n"
                "}\n"
                "f()\n");
            check_eq((int64_t)-1, result.as_int());
            auto plain = make_engine();
            const char* src =
                "[[warn_mode]]; int f() {\n"  // 1
                "  return;\n"                 // 2
                "}\n"                         // 3
                "int b = f();\n";             // 4
            std::string what = failure_text(*plain, src);
            check_true(what.find("Cannot return null from 'f' (declared -> int)") == 0, "bare return text (got: " + what + ")");
            check_eq(std::string("f:2|<script>:4"), trace_shape(*plain));
        });

        test("native_entry_return_conversion_caught_at_return_site", [this]() {
            auto engine = make_engine();
            script_value fv = engine->execute(
                "int f() {\n"
                "  try { var x = \"s\"; return x; } catch (e) { return -1; }\n"
                "}\n"
                "f\n");
            check_true(fv.is_function());
            std::vector<script_value> no_args;
            auto call = fv.as_function()(no_args);
            check_true(call.has_value(), "the callee's catch handled the conversion");
            check_eq((int64_t)-1, call.value().as_int());
            script_value gv = engine->execute(
                "int g() {\n"
                "  var x = \"s\";\n"
                "  return x;\n"
                "}\n"
                "g\n");
            auto bad = gv.as_function()(no_args);
            check_false(bad.has_value(), "an uncaught conversion error reaches the native caller");
            std::string text = engine->format_error(bad);
            check_true(text.find("Cannot return string from 'g' (declared -> int)") == 0, "native caller text (got: " + text + ")");
        });

        test("proven_return_inside_try_stays_identity", [this]() {
            for (bool proofs : {true, false}) {
                auto engine = make_engine();
                engine->type_proofs(proofs);
                check_eq((int64_t)1, engine->execute(
                    "int f() {\n"
                    "  int x = 1;\n"
                    "  try { return x; } catch (e) { return -1; }\n"
                    "}\n"
                    "float g() {\n"
                    "  int x = 2;\n"
                    "  try { return x; } catch (e) { return -1.0; }\n"
                    "}\n"
                    "f()\n").as_int());
                script_value widened = engine->execute("g()");
                check_true(widened.is_float(), "int -> float return still converts inside a try");
                check_near(2.0, widened.as_float(), 1e-9);
            }
        });

        test("throw_in_return_operand_inside_try_resumes_after_catch", [this]() {
            // The return never completes when its operand throws: the catch runs and control
            // continues past the try (the interpreter used to keep the stale return slot)
            auto engine = make_engine();
            check_eq((int64_t)2, engine->execute(
                "auto g() { throw \"x\"; }\n"
                "int f() { try { return g(); } catch (e) { } return 2; }\n"
                "f()\n").as_int());
            check_eq(std::string("x"), engine->execute(
                "string r = \"\";\n"
                "try { return g(); } catch (e) { r = e; }\n"
                "r\n").as_string());
        });

        // A throw during argument or callee evaluation propagates the thrown value, never a parameter-binding error in its place
        test("a_throw_while_evaluating_a_call_propagates_its_value", [this]() {
            const std::pair<const char*, const char*> cases[] = {
                {"auto h() { throw \"argfail\"; } auto g(int a, int b) { return \"g\"; } "
                 "auto caller() { try { return g(1, h()); } catch (e) { return \"c:\" + e; } } "
                 "return caller() + \"|\" + caller();", "c:argfail|c:argfail"},
                {"auto h() { throw \"argfail\"; } try { return \"r:\" + gn(1, h()); } catch (e) { return \"c:\" + e; }", "c:argfail"},
                {"auto h() { throw \"argfail\"; } class M { auto m(int a, int b) { return \"m\"; } } var o = M(); try { return \"r:\" + o.m(1, h()); } catch (e) { return \"c:\" + e; }", "c:argfail"},
                {"auto h() { throw \"argfail\"; } auto g(int a, int b) { return \"g\"; } var hv = h; try { return \"r:\" + g(1, hv()); } catch (e) { return \"c:\" + e; }", "c:argfail"},
                {"auto h() { throw \"calleefail\"; } try { h()(); } catch (e) { return \"c:\" + e; }", "c:calleefail"},
                {"auto h() { throw \"strfail\"; } var s = \"abc\"; try { var n = s.find(h()); return \"r:found\"; } catch (e) { return \"c:\" + e; }", "c:strfail"},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                engine->add_function("gn", [](int a, int b) { return std::string("gn"); });
                check_eq(std::string(c.second), engine->execute(c.first).as_string());
            });
        });

        test("throw_during_argument_evaluation_uncaught_text", [this]() {
            auto engine = make_engine();
            std::string got;
            try {
                engine->execute(
                    "auto h() { throw \"argfail\"; }\n"
                    "auto g(int a, int b) { return \"g\"; }\n"
                    "g(1, h());\n");
            } catch (const std::exception& e) {
                got = e.what();
            }
            check_eq(std::string("argfail"), got);
        });

        // An error a throw's operand raises is the error that propagates: its value, its type for a typed clause, and nothing
        // reported as lost on the way (the interpreter threw null in its place)
        test("an_error_raised_by_a_throw_operand_propagates_unchanged", [this]() {
            const char* prelude = R"jai(
                int f() { throw "in f"; }
                bool fb() { throw "in fb"; }
                var fv() { throw "in fv"; }
                int id(int a) { return a; }
                class Err { string m; Err(string a) { m = a; } }
                int typed() { throw Err("typed"); }
                class Thrower {
                    int m() { throw "in m"; }
                    Thrower operator+(Thrower o) { throw "in +"; }
                    int operator[](int i) { throw "in []"; }
                    int operator()(int i) { throw "in ()"; }
                    string to_string() { throw "in to_string"; }
                }
                class CtorThrows { CtorThrows() { throw "in ctor"; } }
                class Noisy { string n; Noisy(string a) { n = a; } ~Noisy() { print("~", n); } }
                var xs = [1, 2, 3];
                int zero = 0;
                Thrower t;
            )jai";
            const std::pair<const char*, const char*> cases[] = {
                {"try { throw f(); } catch (e) { print(e); }", "in f"},
                {"try { throw throw \"a\"; } catch (e) { print(e); }", "a"},
                {"try { throw throw throw \"deep\"; } catch (e) { print(e); }", "deep"},
                {"try { throw f() + 1; } catch (e) { print(e); }", "in f"},
                {"try { throw -f(); } catch (e) { print(e); }", "in f"},
                {"try { throw !fb(); } catch (e) { print(e); }", "in fb"},
                {"try { throw fb() && true; } catch (e) { print(e); }", "in fb"},
                {"try { throw false || fb(); } catch (e) { print(e); }", "in fb"},
                {"try { throw fv() ?? 3; } catch (e) { print(e); }", "in fv"},
                {"try { throw (zero == 0 ? f() : 2); } catch (e) { print(e); }", "in f"},
                {"try { throw id(f()); } catch (e) { print(e); }", "in f"},
                {"try { throw [f()]; } catch (e) { print(e); }", "in f"},
                {"try { throw {\"k\": f()}; } catch (e) { print(e); }", "in f"},
                {"try { throw `x${f()}`; } catch (e) { print(e); }", "in f"},
                {"try { throw cast<float>(f()); } catch (e) { print(e); }", "in f"},
                {"var l = []() { throw \"in lambda\"; }; try { throw l(); } catch (e) { print(e); }", "in lambda"},
                {"try { throw t.m(); } catch (e) { print(e); }", "in m"},
                {"try { throw t + t; } catch (e) { print(e); }", "in +"},
                {"try { throw t[1]; } catch (e) { print(e); }", "in []"},
                {"try { throw t(1); } catch (e) { print(e); }", "in ()"},
                {"try { throw t; } catch (e) { print(e); }", "in to_string"},
                {"try { throw CtorThrows(); } catch (e) { print(e); }", "in ctor"},
                {"try { throw typed(); } catch (Err e) { print(\"Err \", e.m); } catch (e) { print(\"untyped \", e); }", "Err typed"},
                {"try { throw typed(); } catch (int e) { print(\"int\"); } catch (string e) { print(\"string\"); } catch (e) { print(\"any \", e.m); }", "any typed"},
                {"try { throw xs[99]; } catch (e) { print(e); }", "Array index 99 out of bounds for array of size 3"},
                {"try { throw 1 / zero; } catch (e) { print(e); }", "Division by zero"},
                {"[[warn_mode]] try { throw undefined_name; } catch (e) { print(e); }", "Undefined variable 'undefined_name'"},
                {"void g() { Noisy a(\"a\"); throw f(); } try { g(); } catch (e) { print(e); }", "~a\nin f"},
                {"void g() { try { f(); } catch (e) { Noisy b(\"b\"); throw fb(); } } try { g(); } catch (e) { print(e); }", "~b\nin fb"},
                {"void g() { try { throw f(); } catch (int e) { print(\"int\"); } } try { g(); } catch (e) { print(e); }", "in f"},
                {"void g() { try { f(); } catch (e) { throw e + fv(); } } try { g(); } catch (e) { print(e); }", "in fv"},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                std::string reported;
                engine->set_script_error_handler([&](const std::string& text) { reported += text; });
                check_eq(std::string(c.second), printed_output(*engine, std::string(prelude) + c.first));
                check_eq(std::string(), reported);
            });
        });

        // Uncaught, the operand's error leaves execute as raised: its text, its raise site, its stack
        test("an_uncaught_error_of_a_throw_operand_keeps_its_raise_site", [this]() {
            const std::tuple<const char*, const char*, const char*> cases[] = {
                {"int f() { throw \"in f\"; }\nthrow f();\n", "in f", "f:1|<script>:2"},
                {"int f() { throw \"in f\"; }\nvoid g() {\n    throw f() + 1;\n}\ng();\n", "in f", "f:1|g:3|<script>:5"},
                {"\nthrow throw \"a\";\n", "a", "<script>:2"},
                {"var xs = [1];\nvoid g() {\n    throw xs[9];\n}\ng();\n", "Array index 9 out of bounds for array of size 1", "g:3|<script>:5"},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_eq(std::string(std::get<1>(c)), failure_text(*engine, std::get<0>(c)));
                check_eq(std::string(std::get<2>(c)), trace_shape(*engine));
            });
            auto engine = make_engine();
            check_eq(std::string("1|3"), engine->execute(
                "int f() { throw \"in f\"; }\n"
                "void g() { throw f(); }\n"
                "var shape = \"\"; try { g(); } catch (e) { shape = `${e.line}|${e.stack.size()}`; }\n"
                "shape\n").as_string());
        });

        // What reads an operand stops when the operand raises: no constructor runs on a null argument, no loop reads its
        // condition after its initializer or update failed, no case is compared, and the operand's error is the one caught
        test("a_raising_operand_stops_the_statement_that_reads_it", [this]() {
            const char* prelude = R"jai(
                int f() { throw "T"; }
                var fv() { throw "TV"; }
                bool side(string s) { print("side ", s); return true; }
                class Loose { var a; var b; Loose(var x, var y) { a = x; b = y; print("Loose ran"); } }
                class Typed { string n; Typed(string a) { n = a; print("Typed ran"); } }
                class Agg { int a = 0; int b = 0; }
                var xs = [1, 2];
                int zero = 0;
            )jai";
            const std::pair<const char*, const char*> cases[] = {
                {"try { var l = Loose(f(), 1); } catch (e) { print(e); }", "T"},
                {"try { Loose l(f(), 1); } catch (e) { print(e); }", "T"},
                {"try { var l = make_shared<Loose>(f(), 1); } catch (e) { print(e); }", "T"},
                {"try { Typed t(fv()); } catch (e) { print(e); }", "TV"},
                {"try { var t = make_shared<Typed>(fv()); } catch (e) { print(e); }", "TV"},
                {"try { var w = weak_ptr<Typed>(fv()); } catch (e) { print(e); }", "TV"},
                {"try { Agg a{f(), 1}; } catch (e) { print(e); }", "T"},
                {"try { var a = Agg{1, f()}; } catch (e) { print(e); }", "T"},
                {"try { auto [a, b] = fv(); } catch (e) { print(e); }", "TV"},
                {"try { for (int i = f(); i < 3; ++i) { print(\"body\"); } } catch (e) { print(e); }", "T"},
                {"try { for (var i = fv(); side(\"cond\"); ) { print(\"body\"); } } catch (e) { print(e); }", "TV"},
                {"try { for (int i = 0; side(\"cond\"); i += f()) { print(\"body\"); } } catch (e) { print(e); }", "side cond\nbody\nT"},
                {"try { for (int i = 0; side(\"cond\"); i += 1 / zero) { print(\"body\"); } } catch (e) { print(e); }", "side cond\nbody\nDivision by zero"},
                {"try { for (int i = 0; side(\"cond\"); i = xs[9]) { print(\"body\"); } } catch (e) { print(e); }",
                 "side cond\nbody\nArray index 9 out of bounds for array of size 2"},
                {"[[warn_mode]] try { switch (f()) { default: nosuch; } } catch (e) { print(e); }", "T"},
                {"[[warn_mode]] try { switch (f()) { case nosuch: break; } } catch (e) { print(e); }", "T"},
                {"try { switch (zero) { case f(): print(\"matched\"); break; default: print(\"default\"); } } catch (e) { print(e); }", "T"},
                {"try { parallel_for (auto& x : fv()) { x = 1; } } catch (e) { print(e); }", "TV"},
                {"try { var v = include(fv()); } catch (e) { print(e); }", "TV"},
                {"coroutine int g() { int x = yield f(); print(\"resumed\"); yield x; } try { auto c = g(); print(c.next()); } catch (e) { print(e); }", "T"},
                {"coroutine int g() { yield f(); print(\"resumed\"); yield 2; } try { auto c = g(); print(c.next()); } catch (e) { print(e); }", "T"},
                {"coroutine int g() { try { int x = yield f(); yield x; } catch (e) { print(\"in g \", e); yield 5; } yield 6; } auto c = g(); print(c.next(), c.next(), c.done());",
                 "in g T\n56false"},
                {"class Money { Money(int a) { throw \"conv\"; } } coroutine Money g() { var v = 5; try { var x = yield v; print(\"resumed\"); } catch (e) { print(\"in g \", e); } } "
                 "auto c = g(); print(c.next()); print(c.done());", "in g conv\nnull\ntrue"},
                {"bool lt(int i) { print(\"cond \", i); return i < 2; } coroutine int g() { for (int i = 0; lt(i); i = (yield i) ?? i + 1) { print(\"body \", i); } } "
                 "for (int v : g()) { print(\"got \", v); }", "cond 0\nbody 0\ngot 0\ncond 1\nbody 1\ngot 1\ncond 2"},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                std::string reported;
                engine->set_script_error_handler([&](const std::string& text) { reported += text; });
                check_eq(std::string(c.second), printed_output(*engine, std::string(prelude) + c.first));
                check_eq(std::string(), reported);
            });
        });

        // A default argument or an argument's conversion that raises is the call's error: nothing more binds, no instruction of the
        // callee runs (the VM ran its first one on a null parameter), and nothing is reported as lost
        test("an_error_raised_while_parameters_bind_runs_nothing_of_the_callee", [this]() {
            const char* prelude = R"jai(
                int counter = 0;
                int f() { throw "T"; }
                class Err { string m; Err(string a) { m = a; } }
                int ft() { throw Err("typed"); }
                class Tag { string n; Tag(string a) { n = a; print("+", n); } ~Tag() { print("~", n); } }
                Tag mk(string n) { return Tag(n); }
                int bump() { ++counter; return counter; }
                var xs = [1, 2];
                int zero = 0;
                class Money { int v = 0; Money(int a) { if (a < 0) { throw "conv"; } v = a; ++counter; } }
                class Copied { int v = 0; Copied() { } Copied(const Copied& o) { throw "copy"; } }
                int d_ret(int a = f()) { return a; }
                int d_decl(int a = f()) { int b = a; return b; }
                int d_add(int a = f()) { return a + 1; }
                void d_inc(int a = f()) { ++counter; }
                void d_compound(int a = f()) { counter += 1; }
                void d_idx(int a = xs[99]) { ++counter; }
                void d_div(int a = 1 / zero) { ++counter; }
                void d_host(int a = host_raise()) { ++counter; }
                void d_two(Tag t = mk("first"), int a = f()) { ++counter; }
                void d_later(int a = f(), int b = bump()) { ++counter; }
                void d_typed(int a = ft()) { ++counter; }
                class C {
                    void m(int a = f()) { ++counter; }
                    int ret(int a = f()) { return a; }
                    static void s(int a = f()) { ++counter; }
                }
                class K { int v = bump(); K(int a = f()) { ++counter; } ~K() { print("~K"); } }
                T gen<T>(T a = f()) { ++counter; return a; }
                coroutine int co(int a = f()) { ++counter; yield a; }
                int tail(int n, int a = f()) { ++counter; return n; }
                int tail_caller() { return tail(1); }
                int leaf(int a = f()) { return a + 1; }
                int leaf_caller() { return leaf() + 1; }
                void c_inc(Money m) { ++counter; }
                int c_read(Money m) { return m.v; }
                void c_two(Money a, Money b) { ++counter; }
                void c_copy(Copied c) { ++counter; }
                void c_cref(const Money& m) { ++counter; }
                int c_tail(int n, Money m) { if (n == 0) { return 0; } ++counter; return c_tail(n - 1, -1); }
                coroutine int c_co(Money m) { ++counter; yield 1; }
                void run(function body) {
                    counter = 0;
                    try { body(); print("no throw ", counter); }
                    catch (Err e) { print("Err ", e.m, " ", counter); }
                    catch (e) { print(e, " ", counter); }
                }
            )jai";
            const std::pair<const char*, const char*> cases[] = {
                {"run([]() { d_ret(); });", "T 0"},
                {"run([]() { d_decl(); });", "T 0"},
                {"run([]() { d_add(); });", "T 0"},
                {"run([]() { d_inc(); });", "T 0"},
                {"run([]() { d_compound(); });", "T 0"},
                {"run([]() { d_idx(); });", "Array index 99 out of bounds for array of size 2 0"},
                {"run([]() { d_div(); });", "Division by zero 0"},
                {"run([]() { d_host(); });", "host boom 0"},
                {"run([]() { d_two(); });", "+first\n~first\nT 0"},
                {"run([]() { d_later(); });", "T 0"},
                {"run([]() { d_typed(); });", "Err typed 0"},
                {"run([]() { C c; c.m(); });", "T 0"},
                {"run([]() { C c; print(c.ret()); });", "T 0"},
                {"run([]() { C::s(); });", "T 0"},
                {"run([]() { K k; });", "T 0"},
                {"run([]() { var k = K(); });", "T 0"},
                {"run([]() { var k = make_shared<K>(); });", "T 0"},
                {"run([]() { var l = [](int a = f()) { ++counter; }; l(); });", "T 0"},
                {"run([]() { int z = 3; var l = [z](int a = f()) { ++counter; return z; }; l(); });", "T 0"},
                {"run([]() { gen<int>(); });", "T 0"},
                {"run([]() { auto c = co(); print(\"made\"); c.next(); });", "made\nT 0"},
                {"run([]() { for (int v : co()) { print(v); } });", "T 0"},
                {"run([]() { function g = d_inc; g(); });", "T 0"},
                {"run([]() { var g = d_inc; g(); });", "T 0"},
                {"run([]() { tail_caller(); });", "T 0"},
                {"run([]() { leaf_caller(); });", "T 0"},
                {"run([]() { for (int i = 0; i < 3; ++i) { d_inc(); } });", "T 0"},
                {"run([]() { var g = [](int a = leaf()) { ++counter; }; g(); });", "T 0"},
                {"run([]() { c_inc(-1); });", "conv 0"},
                {"run([]() { print(c_read(-1)); });", "conv 0"},
                {"run([]() { c_two(-1, 5); });", "conv 0"},
                {"run([]() { c_two(5, -1); });", "conv 1"},
                {"run([]() { Copied c; c_copy(c); });", "copy 0"},
                {"run([]() { c_cref(-1); });", "conv 0"},
                {"run([]() { c_tail(3, 1); });", "conv 2"},
                {"run([]() { auto c = c_co(-1); c.next(); });", "conv 0"},
                {"try { d_inc(); } catch (e) { print(e, \" \", counter); }", "T 0"},
                {"try { C c; c.m(); } catch (e) { print(e, \" \", counter); }", "T 0"},
                {"try { c_inc(-1); } catch (e) { print(e, \" \", counter); }", "conv 0"},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                engine->add_function("host_raise", []() -> int { throw std::runtime_error("host boom"); });
                std::string reported;
                engine->set_script_error_handler([&](const std::string& text) { reported += text; });
                check_eq(std::string(c.second), printed_output(*engine, std::string(prelude) + c.first));
                check_eq(std::string(), reported);
            });
        });

        // Uncaught, such an error keeps the callee's frame, standing at its first statement (a constructor's at the constructor), on every backend
        test("an_error_raised_while_parameters_bind_keeps_the_callee_frame", [this]() {
            const char* prelude =
                "int f() { throw \"T\"; }\n"
                "int rt() { var xs = [1]; return xs[9]; }\n"
                "class Money { Money(int a) { throw \"conv\"; } }\n";
            const std::tuple<const char*, const char*, const char*> cases[] = {
                {"int g(\n  int a = f()\n) {\n  int b = a;\n  return b;\n}\ng();\n", "T", "f:1|g:7|<script>:10"},
                {"int g(\n  int a = throw \"D\"\n) {\n  return a;\n}\ng();\n", "D", "g:7|<script>:9"},
                {"int g(\n  int a = rt()\n) {\n  return a;\n}\ng();\n", "Array index 9 out of bounds for array of size 1", "rt:2|g:7|<script>:9"},
                {"int g(\n  int a = 1,\n  int b = f()\n) {\n  return a;\n}\nvoid h() {\n  g();\n}\nh();\n", "T", "f:1|g:8|h:11|<script>:13"},
                {"class C {\n  int m(\n    int a = f()\n  ) {\n    return a;\n  }\n}\nC c;\nc.m();\n", "T", "f:1|m:8|<script>:12"},
                {"class C {\n  static int s(\n    int a = f()\n  ) {\n    return a;\n  }\n}\nC::s();\n", "T", "f:1|s:8|<script>:11"},
                {"class K {\n  K(\n    int a = f()\n  ) {\n    print(a);\n  }\n}\nK k;\n", "T", "f:1|K:5|<script>:11"},
                {"var l = [](\n  int a = f()\n) {\n  return a;\n};\nl();\n", "T", "f:1|<lambda>:7|<script>:9"},
                {"coroutine int co(\n  int a = f()\n) {\n  yield a;\n}\nauto c = co();\nc.next();\n", "T", "f:1|co:7|<script>:10"},
                {"coroutine int co(\n  int a = throw \"D\"\n) {\n  yield a;\n}\nauto c = co();\nc.next();\n", "D", "co:7|<script>:10"},
                {"void g(\n  Money m\n) {\n  print(1);\n}\ng(5);\n", "conv", "Money:3|g:7|<script>:9"},
                {"int t(int n,\n  int a = f()\n) {\n  return n;\n}\nint tc() {\n  return t(1);\n}\ntc();\n", "T", "f:1|t:7|tc:10|<script>:12"},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_eq(std::string(std::get<1>(c)), failure_text(*engine, (std::string(prelude) + std::get<0>(c)).c_str()));
                check_eq(std::string(std::get<2>(c)), trace_shape(*engine));
            });
        });

        // A run-time error raised by the binding itself (the default's own expression, the argument's conversion) keeps the callee's
        // frame as a throw there does, uncaught and in the caught value's stack
        test("an_error_the_binding_raises_keeps_the_callee_frame", [this]() {
            const char* prelude =
                "var xs = [1];\n"
                "var text = \"s\";\n"
                "class Money { int v = 0; Money(int a) { v = a; } }\n";
            const char* index_text = "Array index 9 out of bounds for array of size 1";
            const std::tuple<const char*, const char*, const char*> cases[] = {
                {"int g(\n  int a = xs[9]\n) {\n  return a;\n}\ng();\n", index_text, "g:7|<script>:9"},
                {"int g(\n  int a\n) {\n  return a;\n}\ng(text);\n", "Cannot pass string to parameter 'a' of 'g' (declared int)", "g:7|<script>:9"},
                {"int g(\n  int a = 1,\n  int b = xs[9]\n) {\n  return a;\n}\nvoid h() {\n  g();\n}\nh();\n", index_text, "g:8|h:11|<script>:13"},
                {"class C {\n  int m(\n    int a = xs[9]\n  ) {\n    return a;\n  }\n}\nC c;\nc.m();\n", index_text, "m:8|<script>:12"},
                {"class C {\n  static int s(\n    int a = xs[9]\n  ) {\n    return a;\n  }\n}\nC::s();\n", index_text, "s:8|<script>:11"},
                {"var l = [](\n  int a = xs[9]\n) {\n  return a;\n};\nl();\n", index_text, "<lambda>:7|<script>:9"},
                {"coroutine int co(\n  int a = xs[9]\n) {\n  yield a;\n}\nauto c = co();\nc.next();\n", index_text, "co:7|<script>:10"},
                {"Money m(text);\n", "Cannot pass string to parameter 'a' of 'Money' (declared int)", "Money:3|<script>:4"},
                {"class Q {\n  Q() {\n  }\n}\nclass R : Q {\n  R(int a) {\n    print(a);\n  }\n}\nR r(text);\n",
                 "Cannot pass string to parameter 'a' of 'R' (declared int)", "R:9|<script>:13"},
                {"class Q {\n  Q(int a) {\n  }\n}\nclass R : Q {\n  R(int a)\n    : Q(\n      a) {\n    print(a);\n  }\n}\nR r(text);\n",
                 "Cannot pass string to parameter 'a' of 'R' (declared int)", "R:9|<script>:15"},
                {"int t(int n,\n  int a = xs[9]\n) {\n  return n;\n}\nint tc() {\n  return t(1);\n}\ntc();\n", index_text, "t:7|tc:10|<script>:12"},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_eq(std::string(std::get<1>(c)), failure_text(*engine, (std::string(prelude) + std::get<0>(c)).c_str()));
                check_eq(std::string(std::get<2>(c)), trace_shape(*engine));
            });
            auto engine = make_engine();
            check_eq(std::string("2"), printed_output(*engine, std::string(prelude) +
                "int g(int a = xs[9]) { return a; }\nvoid h() { g(); }\ntry { h(); } catch (e) { print(e.stack.size() - 1); }\n"));
        });

        // Regression: caught typed-return conversion error left the callee frame/env and
        // hasReturnValue_ live - catch body truncated, post-try statements skipped. The
        // literal typed mismatch is a strict load error (a strongly-typed value never reaches
        // a mismatched typed slot at runtime); the recovery path is pinned under warn and,
        // under strict, through a var-sourced value.
        test("caught_return_conversion_error_full_recovery", [this]() {
            const char* typed = R"(
                function bad() -> int { return "nope"; }
                var log = "";
                try { bad(); log += "no"; } catch (e) { log += "a"; log += "b"; }
                log += "c";
                log;
            )";
            auto strict = make_engine();
            check_true(static_error_text(*strict, typed).find("cannot return 'string' from 'bad' declared to return 'int'") != std::string::npos);
            auto region = make_engine();
            check_eq(std::string("abc"), region->execute(std::string("[[warn_mode]]; ") + typed).as_string());
            auto engine = make_engine();
            script_value result = engine->execute(R"(
                var v = "nope";
                function bad() -> int { return v; }
                var log = "";
                try { bad(); log += "no"; } catch (e) { log += "a"; log += "b"; }
                log += "c";
                log;
            )");
            check_eq(std::string("abc"), result.as_string());
        });

        test("caught_return_conversion_error_enclosing_typed_return", [this]() {
            const char* typed = R"(
                function bad() -> int { return "nope"; }
                function outer() -> int {
                    var seen = 0;
                    try { bad(); } catch (e) { seen = 1; }
                    return 7 + seen;
                }
                outer();
            )";
            auto strict = make_engine();
            check_true(static_error_text(*strict, typed).find("cannot return 'string' from 'bad' declared to return 'int'") != std::string::npos);
            auto region = make_engine();
            check_eq((int64_t)8, region->execute(std::string("[[warn_mode]]; ") + typed).as_int());
            auto engine = make_engine();
            script_value result = engine->execute(R"(
                var v = "nope";
                function bad() -> int { return v; }
                function outer() -> int {
                    var seen = 0;
                    try { bad(); } catch (e) { seen = 1; }
                    return 7 + seen;
                }
                outer();
            )");
            check_eq((int64_t)8, result.as_int());
        });

        test("caught_arg_conversion_error_full_recovery", [this]() {
            const char* typed = R"(
                function takes(int x) -> int { return x; }
                var log = "";
                try { takes([1, 2]); log += "no"; } catch (e) { log += "a"; }
                log += "b";
                log;
            )";
            auto strict = make_engine();
            check_true(static_error_text(*strict, typed).find("argument 1 of 'takes': cannot pass 'array' to 'x' declared 'int'") != std::string::npos);
            auto region = make_engine();
            check_eq(std::string("ab"), region->execute(std::string("[[warn_mode]]; ") + typed).as_string());
            check_eq((int64_t)3, region->execute("takes(3);").as_int());
            auto engine = make_engine();
            script_value result = engine->execute(R"(
                function takes(int x) -> int { return x; }
                var arr = [1, 2];
                var log = "";
                try { takes(arr); log += "no"; } catch (e) { log += "a"; }
                log += "b";
                log;
            )");
            check_eq(std::string("ab"), result.as_string());
            check_eq((int64_t)3, engine->execute("takes(3);").as_int());
        });

        // Regression: a script throw inside operator< / <= / > / >= was replaced by "Invalid operands for < operator"
        test("a_throw_in_an_ordering_operator_propagates", [this]() {
            const std::pair<const char*, const char*> cases[] = {
                {"class S { int v = 0; S(int n) { v = n; } function operator<(S o) { throw \"boom\"; } } var a = S(1); "
                 "var lim = S(5); var out = \"\"; try { if (a < lim) { out = \"T\"; } } catch (e) { out = \"C[\" + e + \"]\"; } "
                 "out;", "C[boom]"},
                {"class S { int v = 0; S(int n) { v = n; } function operator>=(S o) { throw \"ge-boom\"; } } var a = S(1); "
                 "var lim = S(5); var out = \"\"; try { var b = (a >= lim); out = \"got:\" + b; } "
                 "catch (e) { out = \"C[\" + e + \"]\"; } out;", "C[ge-boom]"},
                {"class S { int v = 0; S(int n) { v = n; } function operator<(S o) { throw \"deep\"; } } "
                 "function spin(var t, var lim) -> string { var c = 0; while (t < lim) { c += 1; } return \"exited:\" + c; } "
                 "var out = \"\"; try { out = spin(S(1), S(9)); } catch (e) { out = \"C[\" + e + \"]\"; } out + \":alive\";", "C[deep]:alive"},
            };
            check_each(cases, [&](const auto& c) {
                auto engine = make_engine();
                check_eq(std::string(c.second), engine->execute(c.first).as_string());
            });
        });

        // Error texts that embed a value (a host exception, a binary reader past its end, a name nothing declares) never
        // grow the symbol table: a failing call repeated in a loop leaves it as it was, and every text reads unchanged
        test("failing_calls_in_a_loop_keep_the_symbol_table_flat", [this]() {
            auto engine = make_engine();
            jai::stdlib::register_all(*engine);
            engine->add_function("fail", [](script_int n) -> script_int { throw std::runtime_error("fail " + std::to_string(n)); });
            engine->execute(R"(
                class Probe { int a = 0; }
                int base = 0;
                string last_host = "";
                string last_reflect = "";
                string last_binary = "";
                void run(int n) {
                    Probe p;
                    string bytes = "abc";
                    for (int i = 0; i < n; ++i) {
                        try { fail(base + i); } catch (e) { last_host = e; }
                        try { reflect::get(p, "nosuch" + to_string(base + i)); } catch (e) { last_reflect = e; }
                        auto r = bytes.reader();
                        try { r.skip(base + i + 10); } catch (e) { last_binary = e; }
                    }
                    base += n;
                }
            )");
            engine->execute("run(50);");
            engine->execute("run(50);");
            const size_t before = detail::engine_access::get_symbolizer(*engine)->size();
            engine->execute("run(50);");
            for (int i = 0; i < 50; ++i) {
                auto missing = detail::engine_access::get_global_environment(*engine)->get("missing_" + std::to_string(i));
                check_false(static_cast<bool>(missing));
                if (i == 49) { check_eq(std::string("Undefined variable 'missing_49'"), engine->format_error(missing)); }
                check_false(detail::engine_access::get_global_environment(*engine)->contains("missing_" + std::to_string(i)));
            }
            check_eq(before, detail::engine_access::get_symbolizer(*engine)->size());
            check_eq(std::string("fail 149"), engine->execute("last_host").as_string());
            check_eq(std::string("reflect::get: 'Probe' has no member 'nosuch149'"), engine->execute("last_reflect").as_string());
            check_eq(std::string("Cannot skip 159 bytes at position 0: the data is 3 bytes"), engine->execute("last_binary").as_string());
        });

        // A native exception a top-level statement raises (an allocation the library refuses) leaves execute as its own
        // text on both backends; the interpreter let it escape the statement loop and the engine prefixed it
        test("an_uncaught_native_exception_is_its_text_on_both_backends", [this]() {
            const char* sources[] = {
                "string s = \"abc\"; s = s.repeat(9223372036854775807);",
                "string s = \"abc\".repeat(9223372036854775807);",
                "function f() -> string { return \"abc\".repeat(9223372036854775807); } f();",
            };
            for (const char* source : sources) {
                std::string texts[2];
                for (int b = 0; b < 2; ++b) {
                    auto eng = jai::foundry::make_default_engine();
                    eng->set_backend(b == 0 ? backend_type::interpreter : backend_type::vm);
                    try { eng->execute(source); texts[b] = "<no throw>"; } catch (const std::exception& e) { texts[b] = e.what(); }
                    check_eq((int64_t)3, eng->execute("1 + 2").as_int(), source);
                }
                check_eq(texts[1], texts[0], source);
                check_true(texts[0].find("C++ exception") == std::string::npos, texts[0]);
                check_true(texts[0] != "<no throw>", source);
            }
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::exception_handling_tests)
