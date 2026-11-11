#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include "../case_table.hpp"
#include <jaiscript/stdlib/stdlib.hpp>

namespace jai::foundry::tests {

class map_methods_tests : public suite {
public:
    map_methods_tests() : suite("Map Methods") {}

    void forge_tests() override {
        test("has_reports_whether_a_key_is_present", [this]() {
            check_results<bool>({
                {R"(var m = {"a": 1, "b": 2}; m.has("a");)", true},
                {R"(var m = {"a": 1, "b": 2}; m.has("z");)", false},
            });
        });

        test("get_falls_back_to_its_default_and_length_counts_entries", [this]() {
            check_results<int>({
                {R"(var m = {"a": 10, "b": 20}; m.get("a", 0);)", 10},
                {R"(var m = {"a": 10, "b": 20}; m.get("z", 99);)", 99},
                {R"(var m = {"a": 1, "b": 2, "c": 3}; m.length();)", 3},
            });
        });

        test("remove_existing", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var m = {"a": 1, "b": 2, "c": 3};
                var removed = m.remove("b");
                removed;
            )");
            check_eq(result.as<bool>(), true);

            result = eng->execute("m.length();");
            check_eq(result.as<int>(), 2);
        });

        test("remove_nonexistent", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var m = {"a": 1, "b": 2};
                m.remove("z");
            )");
            check_eq(result.as<bool>(), false);
        });

        test("remove_if", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var m = {"a": 1, "b": 2, "c": 3, "d": 4};
                m.remove_if([](auto k, auto v) -> auto { return v % 2 == 0; });
            )");
            check_eq(result.as<int>(), 2);

            result = eng->execute("m.length();");
            check_eq(result.as<int>(), 2);
        });

        test("filter", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var m = {"a": 1, "b": 2, "c": 3, "d": 4};
                m.filter([](auto k, auto v) -> auto { return v > 2; });
                m.length();
            )");
            check_eq(result.as<int>(), 2);
        });

        test("to_array", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var m = {"x": 10, "y": 20};
                var pairs = m.to_array();
                pairs.length();
            )");
            check_eq(result.as<int>(), 2);

            // Check that each element is a 2-element array
            result = eng->execute("pairs[0].length();");
            check_eq(result.as<int>(), 2);
        });

        // Builtin args arrive BY-VALUE: a subscript read in argument position mints an
        // element/map-entry reference, and map key lookups compare through operator<=>
        // (which never derefs) - so m.has(arr[i]) silently missed while the hoisted-temp
        // spelling hit (the jaidoom MINFO.has(MO_TYPE[i]) workaround). These pin
        // hoisted-temp equivalence through deref_builtin_args (builtin_methods.hpp).
        test("map_methods_take_subscript_reads_as_keys_by_value", [this]() {
            check_results<bool>({
                {R"(
                var minfo = {};
                minfo[3004] = "trooper";
                minfo[9] = "shotgun";
                var types = [3004, 9, 42];
                minfo.has(types[0]) && minfo.has(types[1]) && !minfo.has(types[2]);
            )", true},
                {R"(
                var m = {"alpha": 1, "beta": 2};
                var keys = ["alpha", "gamma"];
                m.has(keys[0]) && m.contains(keys[0]) && !m.has(keys[1]);
            )", true},
                {R"(
                var m = {"a": 10, "b": 20, "c": 30};
                var keys = ["a", "b", "c", "z"];
                int got = m.get(keys[0], -1);
                var removed = m.remove(keys[1]);
                m.erase(keys[2]);
                got == 10 && removed && m.length() == 1 && m.get(keys[3], 99) == 99;
            )", true},
                {R"(
                var mi = {};
                mi[7] = true;
                var grid = [[7, 8], [9, 10]];
                var look = {"x": 7};
                mi.has(grid[0][0]) && mi.has(look["x"]) && !mi.has(grid[1][0]);
            )", true},
            });
        });

        // Literal JSON-style map keys (jaibite v4): {1: "a"} was "Expected expression"
        // while {"s": 0, 1: "a"} parsed - detection only gated on the FIRST entry.
        // Keys keep their runtime type: has(1) hits, has("1") misses.
        test("map_literal_keys_keep_their_kinds_and_a_bare_identifier_stringifies", [this]() {
            check_results<bool>({
                {R"(
                var m = {3004: "trooper", 9: "shotgun"};
                m.size() == 2 && m[3004] == "trooper" && m.has(9) && !m.has("9");
            )", true},
                {R"(
                var a = {"s": 0, 1: "a", 2.5: "f", true: "b", 'c': "ch"};
                var b = {1: "a", "s": 0};
                a.size() == 5 && a[1] == "a" && a[2.5] == "f" && a[true] == "b" && a['c'] == "ch" &&
                b.size() == 2 && b[1] == "a" && b["s"] == 0;
            )", true},
                {R"(
                int x = 42;
                var m = {x: 1};
                m.has("x") && !m.has(42);
            )", true},
            });
        });

        test("statement_block_disambiguation_holds", [this]() {
            auto eng = make_engine();
            // A brace starting `<literal> ?` is still a block (ternary statement), not a map
            auto result = eng->execute(R"(
                int r = 0;
                { 1 ? (r = 7) : (r = 9); }
                r;
            )");
            check_eq(result.as<int>(), 7);
            // Negative and computed keys stay out of JSON style (C++ style covers them)
            check_throws([&]() { eng->execute("var m = {-1: \"neg\"}; m;"); });
            check_throws([&]() { eng->execute("var m = {1 + 2: \"sum\"}; m;"); });
            // ...and the C++ style really does cover the negative-key case
            auto neg = eng->execute(R"(var m = {{-1, "neg"}}; m.has(-1);)");
            check_eq(neg.as<bool>(), true);
        });

        test("hashed_maps_iterate_in_key_order", [this]() {
            // lookups are hashed; every observation of order (iteration, keys(), values(), to_json-free printing through a loop,
            // structural equality) sees key order, whatever the insertion order
            auto eng = make_engine(); stdlib::register_all(*eng);
            auto r = eng->execute(R"(
                function run() -> string {
                    map<var, int> m = {};
                    m["b"] = 1; m[3] = 2; m["a"] = 3; m[1] = 4; m[2.5] = 5; m[true] = 6; m[-7] = 7;
                    string out = "";
                    for (auto kv : m) { out = `${out}${kv.first}=${kv.second},`; }
                    array<var> ks = m.keys();
                    out = `${out}|${ks.size()}:${ks[0]}:${ks[ks.size() - 1]}`;
                    map<int, int> x = {}; map<int, int> y = {};
                    for (int i = 0; i < 50; ++i) { x[(i * 37) % 50] = i; }
                    for (int i = 49; i >= 0; --i) { y[(i * 37) % 50] = i; }
                    out = `${out}|${(x == y)}`;
                    int prev = -1; bool sorted = true;
                    for (auto kv : x) { if (kv.first <= prev) { sorted = false; } prev = kv.first; }
                    out = `${out}|${sorted}`;
                    return out;
                }
                run();
            )");
            check_eq(std::string("-7=7,1=4,3=2,2.5=5,a=3,b=1,true=6,|7:-7:true|true|true"), r.as<std::string>());
        });

        test("hashed_map_iteration_survives_mutation", [this]() {
            // a range-for whose body inserts or erases continues with the next greater key, as a std::map walk does
            auto eng = make_engine(); stdlib::register_all(*eng);
            auto r = eng->execute(R"(
                function run() -> string {
                    map<int, int> m = {1: 1, 3: 3, 5: 5};
                    string out = "";
                    for (auto kv : m) {
                        out = `${out}${kv.first},`;
                        if (kv.first == 1) { m[4] = 4; m[0] = 0; }   // 4 is ahead (visited), 0 is behind (not)
                        if (kv.first == 3) { m.remove(3); }           // erasing the current entry
                    }
                    out = `${out}|${m.size()}`;
                    map<int, int> e = {1: 1, 2: 2, 3: 3};
                    string seen = "";
                    for (auto kv : e) { seen = seen + to_string(kv.first); if (kv.first == 1) { e.remove(2); } }
                    return `${out}|${seen}`;
                }
                run();
            )");
            check_eq(std::string("1,3,4,5,|4|13"), r.as<std::string>());
        });

        test("hashed_map_keys_keep_their_kinds_and_float_identity", [this]() {
            auto eng = make_engine(); stdlib::register_all(*eng);
            auto r = eng->execute(R"(
                function run() -> string {
                    map<var, int> m = {};
                    m[0.0] = 1; m[-0.0] = 2; m[1] = 3; m[1.0] = 4; m["1"] = 5; m['1'] = 6;
                    return `${m.size()},${m[0.0]},${m[-0.0]},${m[1]},${m[1.0]},${m.has("1")},${m.has(2)}`;
                }
                run();
            )");
            check_eq(std::string("4,2,2,3,4,true,false"), r.as<std::string>());   // 0.0 and -0.0 are one key, and so are "1" and '1'
        });

        test("scalar_map_lanes_keep_the_general_semantics", [this]() {
            // stores and reads with raw scalar keys and values (fused and unfused sites), against the texts and post-states the
            // entry-reference path produces
            auto eng = make_engine();
            auto r = eng->execute(R"(
                function run() -> string {
                    map<string, int> ms = {};
                    array<string> keys = ["a", "b"];
                    string k = "b";
                    ms[k] = true;                // a bool converts into the int value slot
                    ms[keys[0]] = 7;
                    var v = (ms[keys[0]] = 9);   // the assignment's value
                    string out = `${ms["a"]},${ms[k]},${v},${ms.size()}`;
                    string miss = "";
                    try { var x = ms["zz"]; } catch (e) { miss = e; }
                    out = `${out},${miss},${ms.size()},${ms.has(keys[1])},${ms.has("q")}`;
                    var xs = "x"; try { ms[keys[0]] = xs; } catch (e) { out = `${out}|${e}`; }
                    out = `${out},${ms.size()}`;
                    map<int, float> mi = {};
                    for (int i = 0; i < 5; ++i) { mi[i & 1] = i; }
                    out = `${out}|${mi[0]},${mi[1]},${mi.size()}`;
                    try { map<int, int> bad = {}; var s = "s"; bad[s] = 1; } catch (e) { out = `${out}|${e}`; }
                    var& ref = keys[1];
                    ms[ref] = 5;
                    out = `${out}|${ms["b"]}`;
                    return out;
                }
                run();
            )");
            check_eq(std::string("9,1,9,2,Key \"zz\" not found in map,2,true,false|Cannot assign 'string' to element of type 'int',2|4.0,3.0,2|Cannot use 'string' as a key of type 'int'|5"),
                     r.as<std::string>());
        });
    }
};

} // namespace jai::foundry::tests

using map_methods_tests = jai::foundry::tests::map_methods_tests;
FOUNDRY_REGISTER(map_methods_tests)
