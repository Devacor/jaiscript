#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <cassert>
#include <utility>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// Unregistered class for opaque cpp_bound pins (make_value(T*) on a type no engine knows).
struct t1_opaque_probe { int v = 1; };

class cpp_bound_tests : public suite {
public:
    cpp_bound_tests() : suite("C++ Bound Values") {}
    
    void forge_tests() override {
        test("primitive_types_read_write", [this]() {
            auto eng = jai::foundry::make_engine();
            
            int counter = 100;
            double temperature = 20.5;
            std::string name = "Initial";
            bool flag = false;
            
            // Add global references
            eng->add_global_ref("counter", counter);
            eng->add_global_ref("temperature", temperature);
            eng->add_global_ref("name", name);
            eng->add_global_ref("flag", flag);
            
            // Test reading from script
            auto result = eng->execute("counter");
            check_eq(result.template as<int>(), 100, "Read C++ int from script");
            
            result = eng->execute("temperature");
            check_eq(result.template as<double>(), 20.5, "Read C++ double from script");
            
            result = eng->execute("name");
            check_eq(result.template as<std::string>(), "Initial", "Read C++ string from script");
            
            result = eng->execute("flag");
            check_eq(result.template as<bool>(), false, "Read C++ bool from script");
            
            // Test writing from script
            eng->execute("counter = 200;");
            check_eq(counter, 200, "Write to C++ int from script");
            
            eng->execute("temperature = 30.5;");
            check_eq(temperature, 30.5, "Write to C++ double from script");
            
            eng->execute("name = \"Updated\";");
            check_eq(name, "Updated", "Write to C++ string from script");
            
            eng->execute("flag = true;");
            check_eq(flag, true, "Write to C++ bool from script");
            
            // Test arithmetic operations
            eng->execute("counter = counter + 50;");
            check_eq(counter, 250, "Arithmetic on C++ bound int");
            
            // Test string concatenation
            eng->execute("name = name + \" Again\";");
            check_eq(name, "Updated Again", "String concatenation on C++ bound string");
        });
        
        test("cpp_changes_visible_in_script", [this]() {
            auto eng = jai::foundry::make_engine();
            
            int health = 100;
            eng->add_global_ref("health", health);
            
            // Change in C++
            health = 75;
            
            // Read from script
            auto result = eng->execute("health");
            check_eq(result.template as<int>(), 75, "C++ changes visible in script");
        });
        
        test("integration_with_functions", [this]() {
            auto eng = jai::foundry::make_engine();
            
            int score = 0;
            eng->add_global_ref("score", score);
            
            eng->add_function("add_score", [&score](int points) {
                score += points;
            });
            
            eng->execute("add_score(10);");
            check_eq(score, 10, "Function modifies C++ bound variable");
            
            eng->execute("score = score * 2;");
            check_eq(score, 20, "Script modifies same variable");
        });
        
        test("mixed_operations", [this]() {
            auto eng = jai::foundry::make_engine();
            
            int x = 10;
            int y = 20;
            eng->add_global_ref("x", x);
            eng->add_global_ref("y", y);
            eng->add_global("z", 30);  // Regular global for comparison
            
            // Mixed arithmetic
            auto result = eng->execute("x + y + z");
            check_eq(result.template as<int>(), 60, "Mixed arithmetic with bound and regular values");
            
            // Modify bound values
            eng->execute("x = x * 2; y = y + 5;");
            check_eq(x, 20, "x modified correctly");
            check_eq(y, 25, "y modified correctly");
            
            // Check they can be used in expressions
            result = eng->execute("x > y");
            check_eq(result.template as<bool>(), false, "Comparison with bound values");
        });

        // ---- cpp_bound semantics: reads, stores, operators, truthiness and identity of bound values

        // char is integral, so make_cpp_bound's integral branch wins: a bound char is a
        // width-1 bound INT today (reads as its code point) — pinned as-is.
        test("pin_bound_char_read_write", [this]() {
            auto eng = jai::foundry::make_engine();
            char ch = 'a';
            eng->add_global_ref("bch", ch);
            check_eq((int64_t)97, eng->execute("bch").as_int(), "bound char reads as int code point");
            check_true(eng->execute("bch == 97").as_bool(), "bound char == int");
            eng->execute("bch = 122;");
            check_eq('z', ch, "write to C++ char from script (low byte)");
            ch = 'q';
            check_eq((int64_t)113, eng->execute("bch").as_int(), "C++ char change visible in script");
        });

        test("pin_copy_alias_clone_detach", [this]() {
            auto eng = jai::foundry::make_engine();
            int cav = 100;
            eng->add_global_ref("cav", cav);
            auto v = eng->execute("cav");
            check_true(v.is_cpp_bound(), "execute returns a bound copy");
            cav = 42;
            check_eq(42, v.template as<int>(), "bound copy reads the live variable");
            script_value copyOfBound = v;
            cav = 7;
            check_true(copyOfBound.is_cpp_bound(), "copy of bound stays bound");
            check_eq(7, copyOfBound.template as<int>(), "copy of bound aliases the same variable");
            auto detached = v.clone();
            check_false(detached.is_cpp_bound(), "clone of bound primitive detaches");
            check_eq(7, detached.template as<int>(), "clone snapshots the live value");
            cav = 9;
            check_eq(7, detached.template as<int>(), "clone unaffected by later C++ writes");
            check_eq(9, v.template as<int>(), "original copy still live");
        });

        test("pin_moved_from_bound", [this]() {
            auto eng = jai::foundry::make_engine();
            int mvv = 5;
            eng->add_global_ref("mvv", mvv);
            auto v = eng->execute("mvv");
            check_true(v.is_cpp_bound(), "pre-move: bound");
            script_value w = std::move(v);
            check_true(w.is_cpp_bound(), "moved-to keeps the binding");
            check_false(v.is_cpp_bound(), "moved-from loses the binding");
            check_true(v.is_null(), "moved-from reads as null");
            check_null(v.value().payload_engine(), "moved-from names no engine");
            v = w;
            check_true(v.is_cpp_bound(), "moved-from is reassignable");
            mvv = 11;
            check_eq(11, v.template as<int>(), "reassigned value aliases again");
        });

        test("pin_bound_int_operator_reads", [this]() {
            auto eng = jai::foundry::make_engine();
            script_int ba = 7;
            script_int bb = 3;
            eng->add_global_ref("ba", ba);
            eng->add_global_ref("bb", bb);
            check_eq((int64_t)10, eng->execute("ba + 3").as_int(), "bound +");
            check_eq((int64_t)10, eng->execute("3 + ba").as_int(), "bound + (rhs)");
            check_eq((int64_t)5, eng->execute("ba - 2").as_int(), "bound -");
            check_eq((int64_t)3, eng->execute("10 - ba").as_int(), "bound - (rhs)");
            check_eq((int64_t)14, eng->execute("ba * 2").as_int(), "bound *");
            check_eq((int64_t)3, eng->execute("ba / 2").as_int(), "bound /");
            check_eq((int64_t)2, eng->execute("14 / ba").as_int(), "bound / (rhs)");
            check_eq((int64_t)3, eng->execute("ba % 4").as_int(), "bound %");
            check_eq((int64_t)2, eng->execute("16 % ba").as_int(), "bound % (rhs)");
            check_true(eng->execute("ba < 10").as_bool(), "bound <");
            check_true(eng->execute("ba <= 7").as_bool(), "bound <=");
            check_true(eng->execute("ba > 3").as_bool(), "bound >");
            check_false(eng->execute("ba >= 8").as_bool(), "bound >=");
            check_true(eng->execute("ba == 7").as_bool(), "bound ==");
            check_true(eng->execute("7 == ba").as_bool(), "bound == (rhs)");
            check_false(eng->execute("ba != 7").as_bool(), "bound !=");
            check_eq((int64_t)-1, eng->execute("ba <=> 9").as_int(), "bound <=> less");
            check_eq((int64_t)0, eng->execute("ba <=> 7").as_int(), "bound <=> equal");
            check_eq((int64_t)1, eng->execute("9 <=> ba").as_int(), "bound <=> (rhs)");
            check_eq((int64_t)5, eng->execute("ba & 5").as_int(), "bound &");
            check_eq((int64_t)15, eng->execute("ba | 8").as_int(), "bound |");
            check_eq((int64_t)6, eng->execute("ba ^ 1").as_int(), "bound ^");
            check_eq((int64_t)14, eng->execute("ba << 1").as_int(), "bound <<");
            check_eq((int64_t)3, eng->execute("ba >> 1").as_int(), "bound >>");
            check_eq((int64_t)-7, eng->execute("-ba").as_int(), "unary - on bound");
            check_eq((int64_t)-8, eng->execute("~ba").as_int(), "unary ~ on bound");
            check_false(eng->execute("!ba").as_bool(), "unary ! on bound");
            check_eq((int64_t)10, eng->execute("ba + bb").as_int(), "bound + bound");
            check_true(eng->execute("bb < ba").as_bool(), "bound < bound");
            check_false(eng->execute("ba == bb").as_bool(), "bound == bound");
            eng->execute("ba = ba + 50;");   // the exact pinned shape
            check_eq((int64_t)57, ba, "counter = counter + 50 writes through");
        });

        test("pin_bound_float_operator_reads", [this]() {
            auto eng = jai::foundry::make_engine();
            double bf = 2.5;
            script_int bi = 7;
            eng->add_global_ref("bf", bf);
            eng->add_global_ref("bi", bi);
            check_eq(3.0, eng->execute("bf + 0.5").as_float(), "bound float +");
            check_eq(3.0, eng->execute("0.5 + bf").as_float(), "bound float + (rhs)");
            check_eq(2.0, eng->execute("bf - 0.5").as_float(), "bound float -");
            check_eq(5.0, eng->execute("bf * 2.0").as_float(), "bound float *");
            check_eq(5.0, eng->execute("bf / 0.5").as_float(), "bound float /");
            check_eq(2.0, eng->execute("5.0 / bf").as_float(), "bound float / (rhs)");
            check_true(eng->execute("bf < 3.0").as_bool(), "bound float <");
            check_true(eng->execute("bf <= 2.5").as_bool(), "bound float <=");
            check_true(eng->execute("bf > 2.0").as_bool(), "bound float >");
            check_false(eng->execute("bf >= 2.6").as_bool(), "bound float >=");
            check_true(eng->execute("bf == 2.5").as_bool(), "bound float ==");
            check_false(eng->execute("bf != 2.5").as_bool(), "bound float !=");
            check_eq((int64_t)-1, eng->execute("bf <=> 3.0").as_int(), "bound float <=>");
            check_eq(-2.5, eng->execute("-bf").as_float(), "unary - on bound float");
            check_false(eng->execute("!bf").as_bool(), "unary ! on bound float");
            check_eq(5.0, eng->execute("bf * 2").as_float(), "bound float * int");
            check_eq(9.5, eng->execute("bi + bf").as_float(), "bound int + bound float");
        });

        test("pin_bound_in_if_eq_tostring_callarg_mapkey", [this]() {
            auto eng = jai::foundry::make_engine();
            jai::stdlib::register_all(*eng);
            int bc = 100;
            double bt = 20.5;
            eng->add_global_ref("bc", bc);
            eng->add_global_ref("bt", bt);
            check_eq((int64_t)1, eng->execute("var r6 = 0; if (bc) { r6 = 1; } r6").as_int(), "bound in if");
            check_true(eng->execute("bc == 100").as_bool(), "bound == int");
            check_true(eng->execute("bt == 20.5").as_bool(), "bound == float");
            check_eq(std::string("100"), eng->execute("to_string(bc)").as_string(), "to_string(bound)");
            eng->add_function("pin_twice", [](script_int n) { return n * 2; });
            check_eq((int64_t)200, eng->execute("pin_twice(bc)").as_int(), "bound as call argument");
            check_eq((int64_t)5, eng->execute("var m6 = {}; m6[bc] = 5; m6[100]").as_int(), "bound int as map key");
        });

        test("pin_same_as_bound_aliases", [this]() {
            auto eng = jai::foundry::make_engine();
            script_int sharedInt = 7;
            eng->add_global_ref("sa", sharedInt);
            eng->add_global_ref("sb", sharedInt);
            check_true(eng->execute("sa.same_as(sb)").as_bool(), "two aliases of one C++ int are same_as");
            check_true(eng->execute("sa.same_as(7)").as_bool(), "bound same_as equal plain literal");
            check_true(eng->execute("var pn7 = 7; sa.same_as(pn7)").as_bool(), "bound same_as equal plain var");
            check_false(eng->execute("sa.same_as(8)").as_bool(), "bound same_as unequal int");
        });

        // A compound on a bound value stored AS an element/member/field
        // decode-reads the LIVE C++ value, then detach-and-replaces the slot (C++ untouched).
        // All-detach: write-through
        // belongs to the registered NAME (and refs/captures/by-ref params bound to it),
        // never to stored copies. Literal construction snapshots like every
        // assignment-shaped store, so the slots hold 100 (construction-time value), not
        // a live alias.
        test("pin_element_compound_detach_and_replace", [this]() {
            auto eng = jai::foundry::make_engine();
            int c8 = 100;
            eng->add_global_ref("c8", c8);
            eng->execute("var arr8 = [c8]; var m8 = {\"k\": c8};");
            c8 = 40;
            eng->execute("arr8[0] += 5;");
            check_eq(40, c8, "array-element compound leaves C++ variable untouched");
            check_eq((int64_t)105, eng->execute("arr8[0]").as_int(), "array element snapshots at construction (all-detach)");
            eng->execute("m8[\"k\"] += 2;");
            check_eq(40, c8, "map-value compound leaves C++ variable untouched");
            check_eq((int64_t)102, eng->execute("m8[\"k\"]").as_int(), "map value snapshots at construction (all-detach)");
            c8 = 1000;
            check_eq((int64_t)105, eng->execute("arr8[0]").as_int(), "array element stays detached");
            check_eq((int64_t)102, eng->execute("m8[\"k\"]").as_int(), "map value stays detached");
        });

        // Ref compounds on stored bound values:
        // stored values are detached snapshots, so ref-mediated compounds through
        // container slots and the implicit-this move path can never reach C++.
        test("pin_ref_compound_on_stored_bound_never_reaches_cpp", [this]() {
            for (bool use_vm : {false, true}) {
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                int c14 = 10;
                eng->add_global_ref("c14", c14);
                eng->execute("var arr = [c14];");
                c14 = 20;
                eng->execute("for (auto& x : arr) { x += 5; }");
                check_eq(20, c14, use_vm ? "vm: range-for ref compound leaves C++ untouched"
                                         : "interp: range-for ref compound leaves C++ untouched");
                check_eq((int64_t)15, eng->execute("arr[0]").as_int(), "slot is the construction snapshot + 5");
                eng->execute("auto& r = arr[0]; r += 5;");
                check_eq(20, c14, "ref-decl compound leaves C++ untouched");
                check_eq((int64_t)20, eng->execute("arr[0]").as_int(), "ref-decl compound on the snapshot");
            }
        });

        test("pin_implicit_this_member_store_detaches", [this]() {
            for (bool use_vm : {false, true}) {
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                int c11 = 10;
                eng->add_global_ref("c11", c11);
                eng->add_function("mkBound11", [&c11, e = eng.get()]() {
                    return e->execute("c11");
                });
                eng->execute(R"(
                    class R11 { var f = 0; void grab() { f = mkBound11(); } }
                    var o = R11();
                    o.grab();
                )");
                c11 = 99;
                check_eq((int64_t)10, eng->execute("o.f").as_int(),
                         use_vm ? "vm: implicit-this move stores a detached snapshot"
                                : "interp: implicit-this move stores a detached snapshot");
            }
        });

        // Assigning a bound value INTO an object member detaches it at store time — and
        // array/map slots snapshot the same
        // way; the member snapshots at assignment, compound operates on the snapshot.
        test("pin_member_compound_detach_and_replace", [this]() {
            auto eng = jai::foundry::make_engine();
            int c9 = 100;
            eng->add_global_ref("c9", c9);
            eng->execute(R"(
                class Pin9 { var f = 0; void bump() { f += 5; } }
                var o9 = Pin9(); o9.f = c9;
                var p9 = Pin9(); p9.f = c9;
            )");
            c9 = 40;
            eng->execute("o9.f += 5;");
            check_eq(40, c9, "member compound leaves C++ variable untouched");
            check_eq((int64_t)105, eng->execute("o9.f").as_int(), "member snapshot taken at assign, compound on plain");
            eng->execute("p9.bump();");
            check_eq(40, c9, "implicit-this compound leaves C++ variable untouched");
            check_eq((int64_t)105, eng->execute("p9.f").as_int(), "implicit-this compound on plain snapshot");
            c9 = 7;
            check_eq((int64_t)105, eng->execute("o9.f").as_int(), "member stays detached");
            check_eq((int64_t)105, eng->execute("p9.f").as_int(), "field stays detached");
        });

        test("pin_ref_lvalue_bound_int_index", [this]() {
            auto eng = jai::foundry::make_engine();
            script_int bidx = 2;
            eng->add_global_ref("bidx", bidx);
            check_eq((int64_t)3, eng->execute("var arrB = [1, 2, 3]; auto& rb = arrB[bidx]; rb").as_int(),
                "ref-lvalue indexing reads via a live bound-int index");
            check_eq((int64_t)31, eng->execute("var arrA = [10, 20, 30, 40]; auto& ra = arrA[bidx]; ra = ra + 1; arrA[2]").as_int(),
                "ref-lvalue bound-int index write-through");
        });

        test("pin_truthiness_both_backends", [this]() {
            auto runMatrix = [this](bool useVm) {
                const std::string tag = useVm ? "vm: " : "interp: ";
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(useVm ? jai::backend_type::vm : jai::backend_type::interpreter);
                script_int bz = 0;
                script_int bn = 3;
                double bfz = 0.0;
                double bfn = 1.5;
                t1_opaque_probe probe;
                eng->add_global_ref("bz", bz);
                eng->add_global_ref("bn", bn);
                eng->add_global_ref("bfz", bfz);
                eng->add_global_ref("bfn", bfn);
                eng->add_global("bop", eng->make_value(&probe));
                check_eq((int64_t)1, eng->execute("var r1 = 1; if (bz) { r1 = 2; } r1").as_int(), tag + "zero bound int falsy in if");
                check_true(eng->execute("!bz").as_bool(), tag + "!zero-bound-int");
                check_eq((int64_t)2, eng->execute("var r2 = 1; if (bn) { r2 = 2; } r2").as_int(), tag + "nonzero bound int truthy in if");
                check_false(eng->execute("!bn").as_bool(), tag + "!nonzero-bound-int");
                check_eq((int64_t)1, eng->execute("var r3 = 1; if (bfz) { r3 = 2; } r3").as_int(), tag + "0.0 bound float falsy in if");
                check_true(eng->execute("!bfz").as_bool(), tag + "!zero-bound-float");
                check_eq((int64_t)2, eng->execute("var r4 = 1; if (bfn) { r4 = 2; } r4").as_int(), tag + "nonzero bound float truthy in if");
                // Opaque bound objects are TRUTHY while the host
                // pointer is live (they ARE objects to script).
                check_eq((int64_t)2, eng->execute("var r5 = 1; if (bop) { r5 = 2; } r5").as_int(), tag + "opaque bound truthy in if");
                check_false(eng->execute("!bop").as_bool(), tag + "!opaque-bound");
                check_eq((int64_t)3, eng->execute("var steps = 0; while (bn) { steps = steps + 1; bn = bn - 1; } steps").as_int(), tag + "while on bound int");
                check_eq((int64_t)0, bn, tag + "loop decrement wrote through");
                check_eq((int64_t)2, eng->execute("var r7 = 1; while (bop) { r7 = 2; break; } r7").as_int(), tag + "while on opaque bound enters (truthy)");
            };
            runMatrix(false);
            runMatrix(true);
        });

        test("pin_reads_through_refs", [this]() {
            auto eng = jai::foundry::make_engine();
            jai::stdlib::register_all(*eng);
            script_int bg = 100;
            eng->add_global_ref("bg", bg);
            // ref declaration over a bound global
            check_eq((int64_t)101, eng->execute("auto& r12a = bg; r12a + 1").as_int(), "arithmetic through ref decl");
            check_true(eng->execute("auto& r12b = bg; r12b == 100").as_bool(), "comparison through ref decl");
            check_eq((int64_t)1, eng->execute("auto& r12c = bg; var t12 = 0; if (r12c) { t12 = 1; } t12").as_int(), "if through ref decl");
            check_eq(std::string("100"), eng->execute("auto& r12d = bg; to_string(r12d)").as_string(), "to_string through ref decl");
            check_eq((int64_t)-100, eng->execute("auto& r12e = bg; -r12e").as_int(), "unary - through ref decl");
            // by-ref lambda capture of a bound global
            check_eq((int64_t)101, eng->execute("var f12 = [&bg]() -> auto { return bg + 1; }; f12()").as_int(), "arithmetic through by-ref capture");
            check_true(eng->execute("var g12 = [&bg]() -> auto { return bg == 100; }; g12()").as_bool(), "comparison through by-ref capture");
            // by-ref parameter bound to a bound global
            check_eq((int64_t)102, eng->execute("function rd12(int& t) { return t + 2; } rd12(bg)").as_int(), "arithmetic through by-ref param");
            check_true(eng->execute("function rc12(int& t) { return t >= 100; } rc12(bg)").as_bool(), "comparison through by-ref param");
        });

        // ---- Write-through and live-read semantics for bound values, both backends

        test("t2_compound_writethrough_matrix", [this]() {
            auto runMatrix = [this](bool useVm) {
                const std::string tag = useVm ? "vm: " : "interp: ";
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(useVm ? jai::backend_type::vm : jai::backend_type::interpreter);

                int8_t   mi8 = 0;  uint8_t  mu8 = 0;  int16_t mi16 = 0; int32_t mi32 = 0;
                uint32_t mu32 = 0; int64_t mi64 = 0;  uint64_t mu64 = 0;
                eng->add_global_ref("mi8", mi8);   eng->add_global_ref("mu8", mu8);
                eng->add_global_ref("mi16", mi16); eng->add_global_ref("mi32", mi32);
                eng->add_global_ref("mu32", mu32); eng->add_global_ref("mi64", mi64);
                eng->add_global_ref("mu64", mu64);

                auto intRow = [&](auto& var, const char* name) {
                    using T = std::decay_t<decltype(var)>;
                    const std::string n = name;
                    var = static_cast<T>(10);
                    eng->execute(n + " += 7;");
                    check_eq(static_cast<T>(17), var, tag + n + " += writes through");
                    eng->execute(n + " -= 2;");
                    check_eq(static_cast<T>(15), var, tag + n + " -= writes through");
                    eng->execute(n + " *= 2;");
                    check_eq(static_cast<T>(30), var, tag + n + " *= writes through");
                    eng->execute(n + " /= 3;");
                    check_eq(static_cast<T>(10), var, tag + n + " /= writes through");
                    check_eq((int64_t)11, eng->execute("++" + n).as_int(), tag + n + " ++pre returns new");
                    check_eq(static_cast<T>(11), var, tag + n + " ++pre writes through");
                    check_eq((int64_t)11, eng->execute(n + "++").as_int(), tag + n + " ++post returns old");
                    check_eq(static_cast<T>(12), var, tag + n + " ++post writes through");
                    check_eq((int64_t)12, eng->execute(n + "--").as_int(), tag + n + " --post returns old");
                    check_eq(static_cast<T>(11), var, tag + n + " --post writes through");
                    check_eq((int64_t)10, eng->execute("--" + n).as_int(), tag + n + " --pre returns new");
                    check_eq(static_cast<T>(10), var, tag + n + " --pre writes through");
                    check_eq((int64_t)10, eng->execute(n).as_int(), tag + n + " script view reads live");
                };
                intRow(mi8, "mi8"); intRow(mu8, "mu8"); intRow(mi16, "mi16"); intRow(mi32, "mi32");
                intRow(mu32, "mu32"); intRow(mi64, "mi64"); intRow(mu64, "mu64");

                float mf32 = 0.0f; double mf64 = 0.0;
                eng->add_global_ref("mf32", mf32); eng->add_global_ref("mf64", mf64);
                auto floatRow = [&](auto& var, const char* name) {
                    using T = std::decay_t<decltype(var)>;
                    const std::string n = name;
                    var = static_cast<T>(2.0);
                    eng->execute(n + " += 0.5;");
                    check_eq(static_cast<T>(2.5), var, tag + n + " += writes through");
                    eng->execute(n + " -= 1.0;");
                    check_eq(static_cast<T>(1.5), var, tag + n + " -= writes through");
                    eng->execute(n + " *= 4.0;");
                    check_eq(static_cast<T>(6.0), var, tag + n + " *= writes through");
                    eng->execute(n + " /= 3.0;");
                    check_eq(static_cast<T>(2.0), var, tag + n + " /= writes through");
                    check_eq(3.0, eng->execute("++" + n).as_float(), tag + n + " ++pre returns new");
                    check_eq(static_cast<T>(3.0), var, tag + n + " ++pre writes through");
                    check_eq(3.0, eng->execute(n + "++").as_float(), tag + n + " ++post returns old");
                    check_eq(static_cast<T>(4.0), var, tag + n + " ++post writes through");
                    check_eq(3.0, eng->execute("--" + n).as_float(), tag + n + " --pre returns new");
                    check_eq(static_cast<T>(3.0), var, tag + n + " --pre writes through");
                    check_eq(3.0, eng->execute(n).as_float(), tag + n + " script view reads live");
                };
                floatRow(mf32, "mf32"); floatRow(mf64, "mf64");

                std::string mstr = "ab";
                eng->add_global_ref("mstr", mstr);
                eng->execute("mstr += \"c\";");
                check_eq(std::string("abc"), mstr, tag + "string += writes through");
                check_eq(std::string("abc"), eng->execute("mstr").as_string(), tag + "string script view reads live");

                // section 12.3: int target += float rhs converts and writes through, binding kept
                script_int mconv = 10;
                eng->add_global_ref("mconv", mconv);
                eng->execute("mconv += 2.5;");
                check_eq((int64_t)12, mconv, tag + "int += float converts and writes through");
                mconv = 100;
                check_eq((int64_t)100, eng->execute("mconv").as_int(), tag + "binding kept after float-converting compound");
            };
            runMatrix(false);
            runMatrix(true);
        });

        test("t2_bound_string_live_semantics", [this]() {
            auto runMatrix = [this](bool useVm) {
                const std::string tag = useVm ? "vm: " : "interp: ";
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(useVm ? jai::backend_type::vm : jai::backend_type::interpreter);
                std::string bstr = "content";
                std::string bemp = "";
                std::string sharedStr = "shared";
                eng->add_global_ref("bstr", bstr);
                eng->add_global_ref("bemp", bemp);
                eng->add_global_ref("ssa", sharedStr);
                eng->add_global_ref("ssb", sharedStr);
                check_true(eng->execute("bstr == \"content\"").as_bool(), tag + "== compares the live string");
                check_false(eng->execute("bstr == \"\"").as_bool(), tag + "not the old empty shadow");
                check_true(eng->execute("bstr != \"other\"").as_bool(), tag + "!= live");
                check_true(eng->execute("bstr < \"x\"").as_bool(), tag + "< orders live");
                check_true(eng->execute("bstr <= \"content\"").as_bool(), tag + "<= live");
                check_true(eng->execute("bstr > \"aaa\"").as_bool(), tag + "> live");
                check_eq((int64_t)0, eng->execute("bstr <=> \"content\"").as_int(), tag + "<=> live");
                check_eq(std::string("contentx"), eng->execute("bstr + \"x\"").as_string(), tag + "concat reads live");
                check_false(eng->execute("!bstr").as_bool(), tag + "non-empty bound string truthy");
                check_true(eng->execute("!bemp").as_bool(), tag + "empty bound string falsy");
                check_eq((int64_t)1, eng->execute("var rs = 0; if (bstr) { rs = 1; } rs").as_int(), tag + "bound string in if");
                check_eq((int64_t)5, eng->execute("var msk = {}; msk[bstr] = 5; msk[\"content\"]").as_int(), tag + "bound string as map key");
                // same_as is not a string method (string receivers hit the builtin registry
                // first) - a bound string errors byte-identically to a plain string
                auto sameAsError = [&](const char* code) -> std::string {
                    try { eng->execute(code); } catch (const jai::runtime_error& e) { return e.what(); }
                    return std::string();
                };
                const std::string boundSame = sameAsError("ssa.same_as(ssb)");
                const std::string plainSame = sameAsError("var pss = \"shared\"; pss.same_as(pss)");
                check_true(boundSame.find("same_as") != std::string::npos, tag + "bound string same_as errors (got: " + boundSame + ")");
                check_eq(plainSame, boundSame, tag + "bound string same_as error matches plain string");
                bstr = "swapped";
                check_true(eng->execute("bstr == \"swapped\"").as_bool(), tag + "C++ change visible to ==");
            };
            runMatrix(false);
            runMatrix(true);
        });

        test("t2_writethrough_through_refs", [this]() {
            auto runMatrix = [this](bool useVm) {
                const std::string tag = useVm ? "vm: " : "interp: ";
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(useVm ? jai::backend_type::vm : jai::backend_type::interpreter);
                script_int bg2 = 100;
                eng->add_global_ref("bg2", bg2);
                eng->execute("auto& rr1 = bg2; rr1 += 5;");
                check_eq((int64_t)105, bg2, tag + "compound through ref decl writes through");
                eng->execute("auto& rr2 = bg2; ++rr2;");
                check_eq((int64_t)106, bg2, tag + "++ through ref decl writes through");
                eng->execute("var fcap = [&bg2]() { bg2 += 4; }; fcap();");
                check_eq((int64_t)110, bg2, tag + "compound through by-ref capture writes through");
                // Plain '=' through a ref stores into the C++ variable and the name stays bound
                eng->execute("auto& rr3 = bg2; rr3 = rr3 + 4;");
                check_eq((int64_t)114, bg2, tag + "plain assign through ref writes through");
                bg2 = 200;
                check_eq((int64_t)200, eng->execute("bg2").as_int(), tag + "the name still reads C++");
                check_eq((int64_t)200, eng->execute("rr3").as_int(), tag + "the reference still reads C++");
            };
            runMatrix(false);
            runMatrix(true);
        });

        test("t2_byref_cpp_extraction", [this]() {
            auto eng = jai::foundry::make_engine();
            script_int big = 10;
            std::string bs2 = "abc";
            int32_t b32 = 5;
            double bd = 1.5;
            float bf4 = 1.5f;
            bool bb1 = false;
            eng->add_global_ref("big", big);
            eng->add_global_ref("bs2", bs2);
            eng->add_global_ref("b32", b32);
            eng->add_global_ref("bd", bd);
            eng->add_global_ref("bf4", bf4);
            eng->add_global_ref("bb1", bb1);

            // S9 direct route: non-const as<T&> on a bound value aliases the LIVE C++ variable
            // when the binding's width/signedness exactly matches the script type.
            auto vbig = eng->execute("big");
            vbig.template as<script_int&>() += 5;
            check_eq((int64_t)15, big, "as<script_int&> aliases the live int64 (zero-copy write-through)");
            auto vs = eng->execute("bs2");
            vs.template as<std::string&>() += "!";
            check_eq(std::string("abc!"), bs2, "as<std::string&> aliases the live string");
            auto vbd = eng->execute("bd");
            vbd.template as<script_float&>() += 0.5;
            check_eq(2.0, bd, "as<script_float&> aliases the live double");
            auto vbb = eng->execute("bb1");
            vbb.template as<script_bool&>() = true;
            check_true(bb1, "as<script_bool&> aliases the live bool");

            // Width/sign-mismatched bindings throw a catchable jai::runtime_error - NEVER
            // std::bad_variant_access.
            auto v32 = eng->execute("b32");
            bool caught32 = false;
            try { v32.template as<script_int&>(); } catch (const jai::runtime_error&) { caught32 = true; }
            check_true(caught32, "int32 -> as<script_int&> throws jai::runtime_error");
            check_eq(5, b32, "mismatched-width target untouched");
            auto vf4 = eng->execute("bf4");
            bool caughtF = false;
            try { vf4.template as<script_float&>(); } catch (const jai::runtime_error&) { caughtF = true; }
            check_true(caughtF, "float -> as<script_float&> throws jai::runtime_error");
            check_eq(1.5f, bf4, "mismatched-width float target untouched");

            // engine.add_function with non-const T& params (the S9/12.4 zero-copy route):
            // create_argument preserves the S9 lvalue via std::ref, so exact-width bound
            // arguments alias the LIVE C++ variable - reads AND writes are real.
            script_int fnv = 10;
            eng->add_global_ref("fnv", fnv);
            engine* rawEng = eng.get();
            eng->add_variadic_function("probe_bound", [rawEng](const std::vector<script_value>& a) -> checked_result<script_value> {
                return script_value(!a.empty() && a[0].is_cpp_bound(), rawEng);
            });
            check_true(eng->execute("probe_bound(fnv)").as_bool(), "variadic native call arg stays a bound alias");
            script_int seen = 0;
            eng->add_function("bump_i", [&seen](script_int& n) { seen = n; n += 5; });
            eng->execute("bump_i(fnv);");
            check_eq((int64_t)10, seen, "typed add_function callee reads the live value");
            check_eq((int64_t)15, fnv, "typed add_function script_int& writes through to the C++ variable");
            std::string seenStr;
            eng->add_function("bump_s", [&seenStr](std::string& s) { seenStr = s; s += "!"; });
            eng->execute("bump_s(bs2);");
            check_eq(std::string("abc!"), seenStr, "typed add_function std::string& reads the live string");
            check_eq(std::string("abc!!"), bs2, "typed add_function std::string& writes through");
            double seenF = 0.0;
            eng->add_function("bump_f", [&seenF](script_float& f) { seenF = f; f += 0.25; });
            eng->execute("bump_f(bd);");
            check_eq(2.0, seenF, "typed add_function script_float& reads the live double");
            check_eq(2.25, bd, "typed add_function script_float& writes through");
            // Width-mismatched binding through the call route: catchable jai::runtime_error
            // (surfaced via the binder's cpp_exception wrapper) - never bad_variant_access.
            bool caughtCall = false;
            try { eng->execute("bump_i(b32);"); } catch (const jai::runtime_error&) { caughtCall = true; }
            check_true(caughtCall, "bound int32 -> script_int& param throws jai::runtime_error");
            check_eq(5, b32, "mismatched-width call target untouched");
            // A plain (unbound) argument on the non-const T& route: the callee works on a private
            // copy whose final value is stored back into the argument's variable after the call.
            eng->add_function("push_seven", [rawEng](std::vector<script_value>& a) { a.push_back(script_value((script_int)7, rawEng)); });
            eng->execute("var pa = [1]; push_seven(pa);");
            check_eq((int64_t)2, eng->execute("pa.size()").as_int(), "plain vector<script_value>& writes back into the variable");
            std::string seenPlain;
            eng->add_function("mut_ps", [&seenPlain](std::string& s) { seenPlain = s; s += "!"; });
            eng->execute("var ps = \"a\"; var pt = ps; mut_ps(ps);");
            check_eq(std::string("a"), seenPlain, "plain std::string& callee reads the value");
            check_eq(std::string("a!"), eng->execute("ps").template as<std::string>(), "plain std::string& writes back into the variable");
            check_eq(std::string("a"), eng->execute("pt").template as<std::string>(), "a shared-storage sibling stays untouched");
            script_int seenPlainInt = 0;
            eng->add_function("bump_pn", [&seenPlainInt](script_int& n) { seenPlainInt = n; n += 5; });
            eng->execute("var pn = 3; bump_pn(pn);");
            check_eq((int64_t)3, seenPlainInt, "plain script_int& callee reads the value");
            check_eq((int64_t)8, eng->execute("pn").as_int(), "plain script_int& writes back into the variable");
            // ...while the bound route above stays live-aliased (fnv/bs2/bd asserts).
        });

        test("t2_bound_zero_rhs_divmod_error_surface", [this]() {
            auto runMatrix = [this](bool useVm) {
                const std::string tag = useVm ? "vm: " : "interp: ";
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(useVm ? jai::backend_type::vm : jai::backend_type::interpreter);
                script_int bz2 = 0;
                eng->add_global_ref("bz2", bz2);
                eng->execute("var za = [10];");
                std::string divMsg, modMsg;
                try { eng->execute("za[0] /= bz2;"); } catch (const jai::runtime_error& e) { divMsg = e.what(); }
                check_true(divMsg.find("Division by zero") != std::string::npos, tag + "bound-zero /= keeps 'Division by zero' (got: " + divMsg + ")");
                try { eng->execute("za[0] %= bz2;"); } catch (const jai::runtime_error& e) { modMsg = e.what(); }
                check_true(modMsg.find("Modulo by zero") != std::string::npos, tag + "bound-zero %= keeps 'Modulo by zero' (got: " + modMsg + ")");
            };
            runMatrix(false);
            runMatrix(true);
        });

        test("t2_bound_zero_divisor_binary_twin_parity", [this]() {
            // Binary / and % with a bound-zero divisor must throw the SAME error (code message
            // + text via what()) as the plain twin in the same expression shape, per backend.
            auto runMatrix = [this](bool useVm) {
                const std::string tag = useVm ? "vm: " : "interp: ";
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(useVm ? jai::backend_type::vm : jai::backend_type::interpreter);
                script_int bi = 10, bz = 0;
                script_float bf = 1.5, bfz = 0.0;
                eng->add_global_ref("bi", bi);
                eng->add_global_ref("bz", bz);
                eng->add_global_ref("bf", bf);
                eng->add_global_ref("bfz", bfz);
                eng->execute("var qi = 10; var qz = 0; var qf = 1.5; var qfz = 0.0; var ba = [bz]; var qa = [0];");
                auto msgOf = [&](const std::string& expr) -> std::string {
                    try { eng->execute(expr); } catch (const jai::runtime_error& e) { return e.what(); }
                    return "<no throw>";
                };
                auto twin = [&](const std::string& boundExpr, const std::string& plainExpr, const std::string& label) {
                    check_eq(msgOf(plainExpr), msgOf(boundExpr), tag + label);
                };
                twin("bi / bz", "qi / qz", "ident/ident int /");
                twin("bi % bz", "qi % qz", "ident/ident int %");
                twin("10 / bz", "10 / qz", "literal/ident int /");
                twin("10 % bz", "10 % qz", "literal/ident int %");
                twin("bi / 0", "qi / 0", "ident/literal int /");
                twin("bi % 0", "qi % 0", "ident/literal int %");
                twin("bz / 0", "qz / 0", "bound-numerator ident/literal /");
                twin("bz % 0", "qz % 0", "bound-numerator ident/literal %");
                twin("bf / bz", "qf / qz", "ident/ident float /");
                twin("bf % bz", "qf % qz", "ident/ident float %");
                twin("bf / 0.0", "qf / 0.0", "ident/literal float /");
                twin("bf % 0.0", "qf % 0.0", "ident/literal float %");
                twin("10.0 / bfz", "10.0 / qfz", "literal/ident float /");
                twin("10.0 % bfz", "10.0 % qfz", "literal/ident float %");
                twin("10 / ba[0]", "10 / qa[0]", "complex-shape bound divisor /");
                twin("10 % ba[0]", "10 % qa[0]", "complex-shape bound divisor %");
                // Pin the restored fast-path surface itself (not just parity).
                check_true(msgOf("bi / bz").find("Division by zero") != std::string::npos, tag + "bound / keeps the integer fast-path text");
                check_true(msgOf("bi % bz").find("Modulo by zero") != std::string::npos, tag + "bound % keeps the modulo fast-path text and code");
                check_eq(std::string("<no throw>"), msgOf("bf / bz"), tag + "a bound float over a bound int zero is IEEE");
                // Nonzero bound divisors still compute through the normal path.
                check_eq((int64_t)5, eng->execute("bi / 2").as_int(), tag + "bound numerator / computes");
                check_eq((int64_t)1, eng->execute("bi % 3").as_int(), tag + "bound numerator % computes");
                check_eq((int64_t)2, eng->execute("20 / bi").as_int(), tag + "nonzero bound divisor computes");
            };
            runMatrix(false);
            runMatrix(true);
        });

        test("implicit_numeric_conversions_read_a_bound_value", [this]() {
            // operator int/int64_t/double/float are as<T>(): a bound value decodes its C++ memory (they read the payload union before)
            auto eng = jai::foundry::make_engine();
            int64_t wide = 7;
            int narrow = -3;
            double real = 2.5;
            float single = 1.25f;
            const script_value bound_wide = script_value::make_cpp_bound(&wide, eng.get());
            const script_value bound_narrow = script_value::make_cpp_bound(&narrow, eng.get());
            const script_value bound_real = script_value::make_cpp_bound(&real, eng.get());
            const script_value bound_single = script_value::make_cpp_bound(&single, eng.get());
            check_eq(7, static_cast<int>(bound_wide), "bound int64 as int");
            check_eq((int64_t)7, static_cast<int64_t>(bound_wide), "bound int64 as int64");
            check_eq(7.0, static_cast<double>(bound_wide), "bound int64 as double");
            check_eq(-3, static_cast<int>(bound_narrow), "bound int as int");
            check_eq(2.5, static_cast<double>(bound_real), "bound double as double");
            check_eq(1.25f, static_cast<float>(bound_single), "bound float as float");
            std::string text;
            try { (void)static_cast<int>(script_value(std::string("x"), eng.get())); } catch (const jai::runtime_error& e) { text = e.what(); }
            check_eq(std::string("Cannot convert string to int"), text);
        });

        test("a_host_store_through_a_typed_element_reference_converts_like_a_script_store", [this]() {
            auto eng = jai::foundry::make_engine();
            jai::stdlib::register_all(*eng);
            script_value names = eng->execute("array<string> names = [\"a\", \"b\"]; names;");
            script_value name_ref = script_value::make_element_reference(names.get_array_storage(), 1, eng.get(), names.get_type_info()->element_type());
            name_ref.assign_through(script_value(std::string("z"), eng.get()));
            check_eq(std::string("z"), names.get_array_storage()->get(1, eng.get()).as<std::string>(), "a string element takes a string");
            script_value counts = eng->execute("array<int> counts = [1, 2]; counts;");
            script_value count_ref = script_value::make_element_reference(counts.get_array_storage(), 0, eng.get(), counts.get_type_info()->element_type());
            count_ref.assign_through(script_value(script_int(9), eng.get()));
            check_eq((int64_t)9, counts.get_array_storage()->get(0, eng.get()).as_int(), "an int element takes an int");
            bool threw = false;
            try { count_ref.assign_through(script_value(script_float(2.5), eng.get())); } catch (const jai::runtime_error&) { threw = true; }
            check_true(threw, "a float into an int element raises, as the script store does");
            check_eq((int64_t)9, counts.get_array_storage()->get(0, eng.get()).as_int(), "a refused store leaves the element");
        });

        test("t2_static_gate_sizeof", [this]() {
            static_assert(sizeof(jai::script_value) == 16 && alignof(jai::script_value) == 8, "script_value layout gate");
            check_eq((size_t)16, sizeof(jai::script_value), "script_value is 16 bytes");
        });

        test("pin_unregistered_class_bound", [this]() {
            auto eng = jai::foundry::make_engine();
            t1_opaque_probe probe;
            probe.v = 5;
            auto val = eng->make_value(&probe);
            check_true(val.is_cpp_bound(), "make_value(T*) on unregistered type is bound");
            // Opaque bound objects are NON-null while the pointer
            // is live; a null host pointer stays null.
            check_false(val.is_null(), "opaque bound is non-null");
            auto& extracted = val.template as<t1_opaque_probe&>();
            extracted.v = 42;
            check_eq(42, probe.v, "as<T&> extraction aliases the live object");
            check(val.template get_cpp_bound_as<t1_opaque_probe>() == &probe, "get_cpp_bound_as returns the live pointer");
            eng->add_global("bop13", val);
            check_false(eng->execute("bop13 == null").as_bool(), "opaque bound != null (ruled)");
            check_true(eng->execute("bop13 == bop13").as_bool(), "opaque bound self-identity");
            jai::stdlib::register_all(*eng);
            check_false(eng->execute("is_registered_type(bop13)").as_bool(),
                        "is_registered_type false for opaque host tokens");
            check_true(eng->execute("class Marker {} is_registered_type(Marker())").as_bool(),
                       "is_registered_type true for script objects");
            check_true(eng->execute("is_registered_type(42)").as_bool(),
                       "is_registered_type true for ordinary values");
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::cpp_bound_tests)