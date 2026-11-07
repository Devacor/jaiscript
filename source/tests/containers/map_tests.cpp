#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <map>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class map_tests : public suite {
public:
    map_tests() : suite("Map Tests") {}

    void forge_tests() override {
        test("map_literal_basic", [&]() {
            auto eng = make_engine();

            // Use 'var' for maps with heterogeneous values (string and int)
            eng->execute(R"(
                var m = {"name": "John", "age": 30, "city": "NYC"};
            )");

            auto result = eng->execute("m");
            check(result.is_map());

            check_eq(eng->execute("m[\"name\"]").as<std::string>(), "John");
            check_eq(eng->execute("m[\"age\"]").as<int>(), 30);
            check_eq(eng->execute("m[\"city\"]").as<std::string>(), "NYC");
        });

        test("map_literal_mixed_types", [&]() {
            auto eng = make_engine();

            // Use 'var' for heterogeneous map values
            // 'auto' requires homogeneous values
            eng->execute(R"(
                var mixed = {
                    "int_val": 42,
                    "str_val": "hello",
                    "float_val": 3.14,
                    "bool_val": true
                };
            )");

            check_eq(eng->execute("mixed[\"int_val\"]").as<int>(), 42);
            check_eq(eng->execute("mixed[\"str_val\"]").as<std::string>(), "hello");
            check_eq(eng->execute("mixed[\"float_val\"]").as<double>(), 3.14);
            check_eq(eng->execute("mixed[\"bool_val\"]").as<bool>(), true);
        });

        test("map_literal_empty", [&]() {
            auto eng = make_engine();

            eng->execute("auto empty = {};");

            auto result = eng->execute("empty");
            auto map = result.as<std::map<std::string, script_value>>();
            check_eq(map.size(), 0);
        });

        test("map_access_read", [&]() {
            auto eng = make_engine();

            // Use 'var' for maps with heterogeneous values
            eng->execute(R"(
                var person = {"name": "Alice", "age": 25};
            )");

            check_eq(eng->execute("person[\"name\"]").as<std::string>(), "Alice");
            check_eq(eng->execute("person[\"age\"]").as<int>(), 25);
        });

        test("map_access_write", [&]() {
            auto eng = make_engine();

            // Use 'var' for maps with heterogeneous values
            eng->execute(R"(
                var person = {"name": "Alice"};
                person["name"] = "Bob";
                person["age"] = 30;
            )");

            check_eq(eng->execute("person[\"name\"]").as<std::string>(), "Bob");
            check_eq(eng->execute("person[\"age\"]").as<int>(), 30);
        });

        // TODO: Implement map.has() method
        // test("map_has_key_method", [&]() {
        //     auto eng = make_engine();

        //     eng->execute(R"(
        //         auto m = {"key1": "value1", "key2": "value2"};
        //     )");

        //     check_eq(eng->execute("m.has(\"key1\")").as<bool>(), true);
        //     check_eq(eng->execute("m.has(\"key2\")").as<bool>(), true);
        //     check_eq(eng->execute("m.has(\"key3\")").as<bool>(), false);
        //     check_eq(eng->execute("m.has(\"nonexistent\")").as<bool>(), false);
        // });

        test("map_size_method", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto m = {"a": 1, "b": 2, "c": 3};
            )");

            check_eq(eng->execute("m.size()").as<int>(), 3);

            eng->execute("m[\"d\"] = 4;");
            check_eq(eng->execute("m.size()").as<int>(), 4);
        });

        test("map_empty_method", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto m = {};
            )");

            check_eq(eng->execute("m.empty()").as<bool>(), true);

            eng->execute("m[\"key\"] = \"value\";");
            check_eq(eng->execute("m.empty()").as<bool>(), false);
        });

        test("map_clear_method", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto m = {"a": 1, "b": 2, "c": 3};
                m.clear();
            )");

            check_eq(eng->execute("m.size()").as<int>(), 0);
            check_eq(eng->execute("m.empty()").as<bool>(), true);
        });

        // TODO: Implement map.erase() method
        // test("map_erase_key", [&]() {
        //     auto eng = make_engine();

        //     eng->execute(R"(
        //         auto m = {"a": 1, "b": 2, "c": 3};
        //         m.erase("b");
        //     )");

        //     check_eq(eng->execute("m.size()").as<int>(), 2);
        //     check_eq(eng->execute("m.has(\"a\")").as<bool>(), true);
        //     check_eq(eng->execute("m.has(\"b\")").as<bool>(), false);
        //     check_eq(eng->execute("m.has(\"c\")").as<bool>(), true);
        // });

        test("map_keys_method", [&]() {
            auto eng = make_engine();

            // Use 'var' for maps with heterogeneous values
            eng->execute(R"(
                var m = {"name": "Alice", "age": 30, "city": "NYC"};
                auto keys = m.keys();
            )");

            auto result = eng->execute("keys");
            auto keys_vec = result.as<std::vector<script_value>>();
            check_eq(keys_vec.size(), 3);

            // Keys should be present (order may vary)
            bool has_name = false, has_age = false, has_city = false;
            for (const auto& key : keys_vec) {
                auto k = key.as<std::string>();
                if (k == "name") has_name = true;
                if (k == "age") has_age = true;
                if (k == "city") has_city = true;
            }
            check(has_name && has_age && has_city);
        });

        test("map_values_method", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto m = {"a": 1, "b": 2, "c": 3};
                auto vals = m.values();
            )");

            auto result = eng->execute("vals");
            auto vals_vec = result.as<std::vector<script_value>>();
            check_eq(vals_vec.size(), 3);
        });

        test("map_nested_in_map", [&]() {
            auto eng = make_engine();

            // Use 'var' for maps with heterogeneous values
            eng->execute(R"(
                var person = {
                    "name": "Alice",
                    "address": {
                        "street": "123 Main St",
                        "city": "NYC",
                        "zip": 10001
                    }
                };
            )");

            check_eq(eng->execute("person[\"name\"]").as<std::string>(), "Alice");
            check_eq(eng->execute("person[\"address\"][\"street\"]").as<std::string>(), "123 Main St");
            check_eq(eng->execute("person[\"address\"][\"city\"]").as<std::string>(), "NYC");
            check_eq(eng->execute("person[\"address\"][\"zip\"]").as<int>(), 10001);
        });

        test("map_nested_modification", [&]() {
            auto eng = make_engine();

            // Use 'var' for maps with heterogeneous values
            eng->execute(R"(
                var data = {
                    "user": {
                        "name": "Alice",
                        "age": 30
                    }
                };
                data["user"]["name"] = "Bob";
                data["user"]["age"] = 35;
            )");

            check_eq(eng->execute("data[\"user\"][\"name\"]").as<std::string>(), "Bob");
            check_eq(eng->execute("data[\"user\"][\"age\"]").as<int>(), 35);
        });

        test("map_deeply_nested", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto deep = {
                    "level1": {
                        "level2": {
                            "level3": {
                                "value": 42
                            }
                        }
                    }
                };
            )");

            check_eq(eng->execute("deep[\"level1\"][\"level2\"][\"level3\"][\"value\"]").as<int>(), 42);
        });

        test("map_with_array_values", [&]() {
            auto eng = make_engine();

            // Arrays have homogeneous elements, but the map values are heterogeneous (array types differ)
            eng->execute(R"(
                var data = {
                    "numbers": [1, 2, 3],
                    "strings": ["a", "b", "c"]
                };
            )");

            check_eq(eng->execute("data[\"numbers\"][0]").as<int>(), 1);
            check_eq(eng->execute("data[\"numbers\"][2]").as<int>(), 3);
            check_eq(eng->execute("data[\"strings\"][0]").as<std::string>(), "a");
            check_eq(eng->execute("data[\"strings\"][2]").as<std::string>(), "c");
        });

        test("array_with_map_elements", [&]() {
            auto eng = make_engine();

            // Use 'var' for arrays containing heterogeneous maps
            eng->execute(R"(
                var users = [
                    {"name": "Alice", "age": 30},
                    {"name": "Bob", "age": 25},
                    {"name": "Charlie", "age": 35}
                ];
            )");

            check_eq(eng->execute("users[0][\"name\"]").as<std::string>(), "Alice");
            check_eq(eng->execute("users[0][\"age\"]").as<int>(), 30);
            check_eq(eng->execute("users[1][\"name\"]").as<std::string>(), "Bob");
            check_eq(eng->execute("users[2][\"age\"]").as<int>(), 35);
        });

        test("map_complex_nested_structure", [&]() {
            auto eng = make_engine();

            // Use 'var' for deeply nested heterogeneous structures
            eng->execute(R"(
                var company = {
                    "name": "TechCorp",
                    "employees": [
                        {
                            "name": "Alice",
                            "skills": ["Python", "C++", "JavaScript"]
                        },
                        {
                            "name": "Bob",
                            "skills": ["Java", "Go"]
                        }
                    ],
                    "offices": {
                        "NYC": {"address": "123 Main St", "employees": 50},
                        "SF": {"address": "456 Tech Ave", "employees": 30}
                    }
                };
            )");

            check_eq(eng->execute("company[\"name\"]").as<std::string>(), "TechCorp");
            check_eq(eng->execute("company[\"employees\"][0][\"name\"]").as<std::string>(), "Alice");
            check_eq(eng->execute("company[\"employees\"][0][\"skills\"][1]").as<std::string>(), "C++");
            check_eq(eng->execute("company[\"offices\"][\"NYC\"][\"employees\"]").as<int>(), 50);
            check_eq(eng->execute("company[\"offices\"][\"SF\"][\"address\"]").as<std::string>(), "456 Tech Ave");
        });

        test("map_iteration_keys", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto m = {"a": 1, "b": 2, "c": 3};
                auto count = 0;
                auto keys = m.keys();
                for (auto i = 0; i < keys.size(); i = i + 1) {
                    count = count + 1;
                }
            )");

            check_eq(eng->execute("count").as<int>(), 3);
        });

        test("map_missing_key_behavior", [&]() {
            // every read of a missing key raises, a compound's and a reference's too, and none grows the map; m[k] = v inserts
            auto eng = make_engine();
            auto result = eng->execute(R"(
                map<string, int> hp = {"health": 3};
                var loose = {"a": {"b": 1}};
                var typed_as_var = hp;
                map<string, int> make() { return {"t": 1}; }
                string out = "";
                try { int x = hp["helth"]; } catch (e) { out += e + "|"; }
                try { hp["helth"] += 1; } catch (e) { out += e + "|"; }
                try { hp["helth"]++; } catch (e) { out += e + "|"; }
                try { --hp["helth"]; } catch (e) { out += e + "|"; }
                try { var& r = hp["helth"]; } catch (e) { out += e + "|"; }
                try { var x = loose["a"]["c"]; } catch (e) { out += e + "|"; }
                try { loose["q"]["c"] = 1; } catch (e) { out += e + "|"; }
                try { loose['z'] -= 1; } catch (e) { out += e + "|"; }
                try { var x = loose[2.5] ?? 0; } catch (e) { out += e + "|"; }
                var array_key = [1, "two"];
                try { loose[array_key] ??= 1; } catch (e) { out += e + "|"; }
                try { var x = make()["u"]; } catch (e) { out += e + "|"; }
                try { var x = typed_as_var[5]; } catch (e) { out += e + "|"; }
                try { var [k, v] = loose["pair"]; } catch (e) { out += e + "|"; }
                out += `${hp.size()} ${loose.size()} ${loose["a"].size()}`;
                hp["fresh"] = 1;
                hp["fresh"] += 1;
                hp["count"] = hp.get("count", 0) + 1;
                out + " " + hp
            )");
            check_eq(std::string("Key \"helth\" not found in map|Key \"helth\" not found in map|Key \"helth\" not found in map|"
                                 "Key \"helth\" not found in map|Key \"helth\" not found in map|Key \"c\" not found in map|"
                                 "Key \"q\" not found in map|Key 'z' not found in map|Key 2.5 not found in map|"
                                 "Key [1, \"two\"] not found in map|Key \"u\" not found in map|"
                                 "Cannot use 'int' as a key of type 'string'|Key \"pair\" not found in map|"
                                 "1 1 1 {\"count\": 1, \"fresh\": 2, \"health\": 3}"),
                     result.as<std::string>());
        });

        test("map_in_function_parameter", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                function getProperty(var obj, string key) -> var {
                    return obj[key];
                }
            )");

            // Use 'var' for maps with heterogeneous values
            eng->execute("var person = {\"name\": \"Alice\", \"age\": 30};");

            check_eq(eng->execute("getProperty(person, \"name\")").as<std::string>(), "Alice");
            check_eq(eng->execute("getProperty(person, \"age\")").as<int>(), 30);
        });

        // TODO: Fix - crashes when indexing directly into function return value
        // test("map_as_return_value", [&]() {
        //     auto eng = make_engine();

        //     eng->execute(R"(
        //         function makeConfig() -> auto {
        //             return {"debug": true, "port": 8080};
        //         }
        //     )");

        //     check_eq(eng->execute("makeConfig()[\"debug\"]").as<bool>(), true);
        //     check_eq(eng->execute("makeConfig()[\"port\"]").as<int>(), 8080);
        // });

        // TODO: Implement map merge operator
        // test("map_merge_operation", [&]() {
        //     auto eng = make_engine();

        //     eng->execute(R"(
        //         auto map1 = {"a": 1, "b": 2};
        //         auto map2 = {"c": 3, "d": 4};
        //         auto merged = map1 + map2;
        //     )");

        //     check_eq(eng->execute("merged[\"a\"]").as<int>(), 1);
        //     check_eq(eng->execute("merged[\"b\"]").as<int>(), 2);
        //     check_eq(eng->execute("merged[\"c\"]").as<int>(), 3);
        //     check_eq(eng->execute("merged[\"d\"]").as<int>(), 4);
        //     check_eq(eng->execute("merged.size()").as<int>(), 4);
        // });

        // TODO: Implement map + operator for merging
        // test("map_override_on_merge", [&]() {
        //     auto eng = make_engine();

        //     eng->execute(R"(
        //         auto map1 = {"key": "value1", "other": "data"};
        //         auto map2 = {"key": "value2"};
        //         auto merged = map1 + map2;
        //     )");

        //     // Second map should override first
        //     check_eq(eng->execute("merged[\"key\"]").as<std::string>(), "value2");
        //     check_eq(eng->execute("merged[\"other\"]").as<std::string>(), "data");
        // });

        test("map_merge_function", [&]() {
            auto eng = make_engine();
            stdlib::register_all(eng);

            eng->execute(R"(
                auto map1 = {"key": "value1", "other": "data"};
                auto map2 = {"key": "value2"};
                auto merged = merge(map1, map2);
            )");

            // Second map should override first
            check_eq(eng->execute("merged[\"key\"]").as<std::string>(), "value2");
            check_eq(eng->execute("merged[\"other\"]").as<std::string>(), "data");
        });

        test("merge_arity_text_spells_the_count", [&]() {
            auto eng = make_engine();
            stdlib::register_all(eng);
            auto caught = eng->execute(R"(
                var text = "";
                try { merge({"a": 1}); } catch (e) { text = e; }
                text
            )");
            check_eq(std::string("merge() expects exactly 2 arguments, got 1"), caught.as<std::string>());
        });

        test("map_with_numeric_string_keys", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto m = {"0": "zero", "1": "one", "2": "two"};
            )");

            check_eq(eng->execute("m[\"0\"]").as<std::string>(), "zero");
            check_eq(eng->execute("m[\"1\"]").as<std::string>(), "one");
            check_eq(eng->execute("m[\"2\"]").as<std::string>(), "two");
        });

        // A map key is read-only (C++ `const Key`, the ordering invariant), by value AND by
        // reference; `auto& kv` aliases the value only. The pair's `first` stays a SHARED
        // handle of the key (no per-iteration copy); the checker
        // rejects the mutation spellings it can see under strict, and the runtime backstop
        // is the string copy-on-write gate: whatever the checker
        // cannot see mutates a copy and the map is never touched. Every row runs on both
        // backends and pins byte-identical output.
        auto both_mode = [](const char* src, bool warn_region) {
            std::string out[2];
            int idx = 0;
            const std::string program = (warn_region ? std::string("[[warn_mode]]; ") : std::string()) + src;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execution_budget(0);
                jai::stdlib::register_all(e);
                try { out[idx] = e->execute(program).to_string(); }
                catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                ++idx;
            }
            return std::make_pair(out[0], out[1]);
        };

        test("map_key_read_only_static_battery", [this, both_mode]() {
            const std::string text = "map keys are read-only";   // the operand spelling follows the pair variable (kv.first, p.first)
            auto rejected = [&](const char* src, const char* what) {
                auto [i_out, v_out] = both_mode(src, false);
                check_eq(i_out, v_out, std::string("backend parity: ") + what);
                check(i_out.find("ERROR: static check failed") == 0, std::string("load error: ") + what + " (got: " + i_out + ")");
                check(i_out.find("cannot mutate ") != std::string::npos && i_out.find(text) != std::string::npos, std::string("text: ") + what + " (got: " + i_out + ")");
            };
            auto accepted = [&](const char* src, const std::string& expected, const char* what) {
                auto [i_out, v_out] = both_mode(src, false);
                check_eq(expected, i_out, std::string("interp: ") + what);
                check_eq(expected, v_out, std::string("vm: ") + what);
            };
            rejected("map<string,int> m = {\"a\": 1}; for (auto kv : m) { kv.first.to_upper(); } 0", "by-value pair, string mutator");
            rejected("map<string,int> m = {\"a\": 1}; for (auto& kv : m) { kv.first.to_upper(); } 0", "by-reference pair, string mutator");
            rejected("map<string,int> m = {\"a\": 1}; for (auto kv : m) { kv.first.insert(0, \"x\"); } 0", "insert");
            rejected("map<string,int> m = {\"a\": 1}; for (auto kv : m) { kv.first.erase(0, 1); } 0", "erase");
            rejected("map<string,int> m = {\"a\": 1}; for (auto kv : m) { kv.first.replace(\"a\", \"b\"); } 0", "replace");
            rejected("map<string,int> m = {\"a\": 1}; for (auto kv : m) { kv.first = \"z\"; } 0", "store");
            rejected("map<string,int> m = {\"a\": 1}; for (auto kv : m) { kv.first += \"!\"; } 0", "compound store");
            rejected("map<int,int> m = {1: 1}; for (auto kv : m) { kv.first++; } 0", "postfix ++ (desugars to += 1)");
            rejected("map<int,int> m = {1: 1}; for (auto& kv : m) { --kv.first; } 0", "prefix -- by reference");
            rejected("auto m = {\"a\": 1}; for (auto kv : m) { kv.first.to_upper(); } 0", "auto map literal is statically a map");
            rejected("map<string,int> m = {\"a\": 1}; for (auto& kv : m) { kv.first += \"hey\"; } 0", "kv.first += \"hey\" by reference");
            rejected("map<string,int> m = {\"a\": 1}; for (auto kv : m) { var p = kv; p.first.to_upper(); } 0", "a copied pair still holds the key (C++ pair<const K, V>)");
            rejected("map<string,int> m = {\"a\": 1}; for (auto& kv : m) { auto& p = kv; p.first.insert(0, \"x\"); } 0", "an alias of the pair still holds the key");
            rejected("map<string,int> m = {\"a\": 1}; for (auto kv : m) { auto f = [&]() { kv.first.to_upper(); }; f(); } 0", "the pair reached through a [&] lambda");
            // Alias spellings: a mutable alias of a key has no C++ equivalent (const Key) - the
            // fix is always the copy, which is copy-on-write at runtime
            auto rejected_alias = [&](const char* src, const char* what) {
                auto [i_out, v_out] = both_mode(src, false);
                check_eq(i_out, v_out, std::string("backend parity: ") + what);
                check(i_out.find("ERROR: static check failed") == 0, std::string("load error: ") + what + " (got: " + i_out + ")");
                check(i_out.find("cannot alias ") != std::string::npos &&
                      i_out.find("map keys are read-only (copy it instead: var k = ") != std::string::npos,
                      std::string("alias text: ") + what + " (got: " + i_out + ")");
            };
            rejected_alias("map<string,int> m = {\"a\": 1}; for (auto& kv : m) { var& cheeky = kv.first; } 0", "var& cheeky = kv.first");
            rejected_alias("map<string,int> m = {\"a\": 1}; for (auto kv : m) { auto& k = kv.first; } 0", "auto& alias of the key");
            rejected_alias("map<string,int> m = {\"a\": 1}; for (auto kv : m) { var& k = kv.first; } 0", "var& alias of the key");
            rejected_alias("function up(string& s) { s.to_upper(); } map<string,int> m = {\"a\": 1}; for (auto kv : m) { up(kv.first); } 0", "passed to a string& parameter");
            rejected_alias("map<string,int> m = {\"a\": 1}; for (auto& kv : m) { shared_ptr<auto> cheeky = kv.first; } 0", "shared_ptr<var> is not a spelling; shared_ptr<auto> is: shared_ptr<auto> cheeky = kv.first");
            rejected_alias("map<string,int> m = {\"a\": 1}; for (auto kv : m) { shared_ptr<string> c = kv.first; } 0", "shared_ptr<string> from the key");
            rejected_alias("map<string,int> m = {\"a\": 1}; for (auto kv : m) { shared_ptr<auto> c = kv.first; } 0", "shared_ptr<auto> from the key");
            rejected_alias("map<string,int> m = {\"a\": 1}; shared_ptr<string> h = null; for (auto kv : m) { h = kv.first; } 0", "store into a shared_ptr-declared variable");
            rejected_alias("map<string,int> m = {\"a\": 1}; for (auto kv : m) { var& c = cast<string&>(kv.first); } 0", "cast<string&> of the key");
            rejected_alias("function keep(shared_ptr<string> s) { return 0; } map<string,int> m = {\"a\": 1}; for (auto kv : m) { keep(kv.first); } 0", "passed to a shared_ptr<string> parameter");
            rejected_alias("map<string,int> m = {\"a\": 1}; function first_key() -> string& { for (auto& kv : m) { return kv.first; } } first_key(); 0", "returned by reference (a map<K,V>& parameter is dynamic to the checker: a global map here)");
            rejected_alias("map<string,int> m = {\"a\": 1}; function first_key() -> shared_ptr<string> { for (auto kv : m) { return kv.first; } return null; } first_key(); 0", "returned as a shared_ptr");
            rejected_alias("map<string,int> m = {\"a\": 1}; for (auto kv : m) { var p = kv; var& c = p.first; } 0", "alias through a copied pair");
            rejected_alias("map<string,int> m = {\"a\": 1}; string other = \"o\"; bool flag = true; for (auto kv : m) { var& c = flag ? kv.first : other; } 0", "a ternary over the key keeps the marker");
            accepted("map<string,int> m = {\"a\": 1}; string r = \"\"; for (auto& kv : m) { var okay = kv.first; okay.to_upper(); okay += \"!\"; r += okay; } r + to_json(m)", "A!{\"a\":1}", "var okay = kv.first is the copy-on-write path");
            accepted("map<string,int> m = {\"a\": 1}; string r = \"\"; for (auto kv : m) { string s2 = kv.first; s2.to_upper(); string u = cast<string>(kv.first); u += \"?\"; r += s2 + u; } r + to_json(m)", "Aa?{\"a\":1}", "typed copies and a by-value cast");
            accepted("map<string,int> m = {\"a\": 1}; function first_key() -> string { for (auto kv : m) { return kv.first; } return \"\"; } string k = first_key(); k.to_upper(); k + to_json(m)", "A{\"a\":1}", "returned by value: a copy");
            accepted("map<string,int> m = {\"a\": 1}; string r = \"\"; for (auto kv : m) { auto f = [=]() { return kv.first + \"x\"; }; r += f(); } r", "ax", "reading the key inside a lambda");
            accepted("map<string,int> m = {\"a\": 1, \"b\": 2}; string s = \"\"; for (auto kv : m) { s += kv.first; s += to_string(kv.first.length()); } s", "a1b1", "reads of the key are fine");
            accepted("map<string,int> m = {\"a\": 1}; for (auto& kv : m) { kv.second = kv.second * 10; kv.second += 1; } to_string(m[\"a\"])", "11", "the value stays writable by reference");
            accepted("map<string,string> m = {\"a\": \"v\"}; for (auto& kv : m) { kv.second.to_upper(); } m[\"a\"]", "V", "a string value mutator by reference");
            accepted("function show(string s) { return s; } map<string,int> m = {\"a\": 1}; string r = \"\"; for (auto kv : m) { r += show(kv.first); } r", "a", "passed by value");
            accepted("var m = {\"a\": 1}; for (auto kv : m) { kv.first.to_upper(); } to_json(m)", "{\"a\":1}", "a var map is dynamic: the runtime backstop keeps the key");
            accepted("map<string,int> m = {\"a\": 1}; for (auto kv : m) { var k = kv.first; k.to_upper(); } to_json(m)", "{\"a\":1}", "a var copy of the key is the checker-invisible path: runtime backstop");
            accepted("var arr = [[\"a\", 1]]; for (auto kv : arr) { kv[0].to_upper(); } string s = to_json(arr); for (auto& kv : arr) { kv[0].to_upper(); } s + to_json(arr)", "[[\"a\",1]][[\"A\",1]]", "an array of pairs is not a map: by value copies, by reference writes through");
        });

        test("map_key_immutable_battery", [this, both_mode]() {
            auto row = [&](const char* src, const std::string& expected, const char* what) {
                auto [i_out, v_out] = both_mode(src, true);
                check_eq(expected, i_out, std::string("interp: ") + what);
                check_eq(expected, v_out, std::string("vm: ") + what);
            };
            row("map<string,int> m = {\"a\": 1, \"b\": 2}; string seen = \"\"; for (auto kv : m) { kv.first.to_upper(); seen += kv.first; } to_json(m) + seen + to_string(m.contains(\"a\"))",
                "{\"a\":1,\"b\":2}ABtrue", "by-value pair: kv.first mutates the pair's copy, never the key");
            row("map<string,int> m = {\"a\": 1, \"b\": 2}; for (auto& kv : m) { kv.first.to_upper(); kv.second = kv.second * 10; } to_json(m)",
                "{\"a\":10,\"b\":20}", "by-reference pair: second aliases the value, first is a copy");
            row("map<string,int> m = {\"a\": 1, \"b\": 2}; for (auto& kv : m) { kv.first.to_upper(); kv.first += \"!\"; } to_json(m)",
                "{\"a\":1,\"b\":2}", "by-reference pair: mutator and += on first leave the key");
            row("map<string,int> m = {\"a\": 1, \"b\": 2}; for (auto kv : m) { var k = kv.first; k.to_upper(); k += \"!\"; } to_json(m)",
                "{\"a\":1,\"b\":2}", "var-sourced key copy mutated");
            row("map<string,int> m = {\"a\": 1, \"b\": 2}; string r = \"\"; for (auto kv : m) { var p = kv; p.first.to_upper(); r += p.first + kv.first; } to_json(m) + r",
                "{\"a\":1,\"b\":2}AaBb", "the pair escaping whole: the copy's first mutates, the loop pair and the key do not");
            row("map<string,int> m = {\"a\": 1, \"b\": 2}; var ks = m.keys(); ks[0].to_upper(); to_json(m) + to_json(ks)",
                "{\"a\":1,\"b\":2}[\"A\",\"b\"]", "keys() hands out shared handles; the element detaches on write");
            row("map<string,int> m = {\"b\": 2, \"a\": 1, \"c\": 3}; for (auto& kv : m) { kv.first.to_upper(); } m[\"a\"] = 100; string o = \"\"; for (auto kv : m) { o += kv.first; } o + to_string(m.size()) + to_string(m[\"a\"])",
                "abc3100", "iteration order and lookups hold after the attempt");
            row("map<var,int> m = {\"a\": 1, \"b\": 2}; for (auto kv : m) { kv.first.to_upper(); } for (auto& kv : m) { kv.first.to_upper(); } to_json(m) + to_string(m.contains(\"a\"))",
                "{\"a\":1,\"b\":2}true", "string keys in map<var,V>, both shapes");
            row("string k = \"a\"; map<string,int> m = {}; m[k] = 1; k.to_upper(); k += \"!\"; to_json(m) + to_string(m.contains(\"a\")) + k",
                "{\"a\":1}trueA!", "a key inserted through a variable shares its node until the variable is written");
            row("map<string,int> m = {\"y\": 1}; m.filter([](k, v) { k.to_upper(); return true; }); var f = m; to_json(m) + to_json(f)",
                "{\"y\":1}{\"y\":1}", "filter predicate argument detaches on write");
            row("map<string,int> m = {\"z\": 1}; m.remove_if([](k, v) { k.to_upper(); return false; }); to_json(m)",
                "{\"z\":1}", "remove_if predicate argument detaches on write");
            row("map<string,int> m = {\"v\": 1}; var e = m.to_array(); e[0][0].to_upper(); to_json(m) + to_json(e)",
                "{\"v\":1}[[\"V\",1]]", "to_array() entries detach on write");
            row("function tk(string s) { s.to_upper(); return s; } function tkr(string& s) { s.to_upper(); return s; } map<string,int> m = {\"u\": 1}; string r = \"\"; for (auto kv : m) { r += tk(kv.first) + kv.first; tkr(kv.first); r += kv.first; } to_json(m) + r",
                "{\"u\":1}UuU", "kv.first through a by-value and a string& parameter: the pair's own copy is the lvalue");
            row("map<string,int> m = {\"s\": 1}; string r = \"\"; for (auto kv : m) { auto& kr = kv.first; kr.to_upper(); r += kr + kv.first; } to_json(m) + r",
                "{\"s\":1}SS", "auto& kr = kv.first aliases the pair's copy");
            row("map<string,int> m = {\"w\": 1}; for (auto kv : m) { kv.first += \"!\"; kv.first = \"zzz\"; } to_json(m)",
                "{\"w\":1}", "stores to kv.first replace the pair field");
            row("map<int,string> m = {1: \"a\", 2: \"b\"}; for (auto kv : m) { kv.second.to_upper(); } string s1 = to_json(m); for (auto& kv : m) { kv.second.to_upper(); } s1 + to_json(m)",
                "{\"$map\":[[1,\"a\"],[2,\"b\"]]}{\"$map\":[[1,\"A\"],[2,\"B\"]]}", "int keys: by-value second is a copy, by-reference second writes through");
            row("map<string,string> m = {\"k\": \"v\"}; m[\"k\"].to_upper(); string a = to_json(m); string key = \"k\"; m[key].to_lower(); a + to_json(m)",
                "{\"k\":\"V\"}{\"k\":\"v\"}", "map value through a subscript receiver mutates in place");
            row("string src = \"v\"; map<string,string> m = {\"k\": src}; m[\"k\"].to_upper(); to_json(m) + src",
                "{\"k\":\"V\"}v", "a map value copied from a variable detaches on write");
        });

        // The map's contract: iteration and serialization see key order, never insertion order; an
        // iteration continues after its key when the map changes under it (a callee's stores and erases included); a reference
        // to a value survives the table's growth; keys of different kinds stay distinct; == and JSON follow key order
        test("map_contract_battery", [this, both_mode]() {
            const char* src = R"JAI(
string log = "";
map<var, int> m = {};
for (int i = 0; i < 2000; ++i) { m[(i * 7919) % 2000] = i; }
for (int i = 0; i < 2000; i += 2) { m.erase(i); }
for (int i = 3000; i > 2000; i -= 3) { m[i] = 1; }
int prev = -1; bool sorted = true; int n = 0;
for (auto [k, v] : m) { if (k <= prev) { sorted = false; } prev = k; n += 1; }
log += to_string(sorted) + " " + to_string(n) + " " + to_string(m.size());
var mixed = {};
mixed["b"] = 1; mixed[3] = 2; mixed["a"] = 3; mixed[1] = 4; mixed[2.5] = 5; mixed[true] = 6; mixed['c'] = 7;
string order = "";
for (auto [k, v] : mixed) { order += to_string(k) + ":" + to_string(v) + ","; }
log += " | " + order;
var e = {};
for (int i = 0; i < 10; ++i) { e[i] = i; }
string seen = "";
for (auto [k, v] : e) { seen += to_string(k); if (k == 3) { e.erase(4); e.erase(3); e[11] = 0; } }
log += " | " + seen;
map<string, int> refs = {};
refs["k"] = 1;
auto& r = refs["k"];
for (int i = 0; i < 3000; ++i) { refs["x" + to_string(i)] = i; }
r = 42;
log += " | " + to_string(refs["k"]) + " " + to_string(refs.size());
var u = {};
u["ab"] = 1;
u[utf8_string("ab")] = 2;
log += " | " + to_string(u.size());
var f = {};
f[0.0] = 1; f[-0.0] = 2; f[math::NAN_VALUE] = 3; f[-math::NAN_VALUE] = 4; f[1] = 5; f[1.0] = 6; f[math::vec2(0.0, 1.0)] = 7; f[math::vec2(-0.0, 1.0)] = 8;
string fs = "";
for (auto [k, v] : f) { fs += to_string(v) + ","; }
log += " | " + to_string(f.size()) + " " + fs;
var a = {"z": 1, "y": 2, "x": [1, 2]};
var b = {};
b["x"] = [1, 2]; b["z"] = 1; b["y"] = 2;
log += " | " + to_string(a == b) + " " + to_json(a) + " " + to_json(from_json(to_json(b)));
var c = {};
for (int i = 0; i < 50; ++i) { c[i] = i; }
int keys_seen = 0;
void grow(var& target, int at) { for (int j = 100; j < 400; ++j) { target[j] = j; } target.erase(at + 1); }
for (auto [k, v] : c) { keys_seen += 1; if (k == 10) { grow(c, 10); } }
log += " | " + to_string(keys_seen) + " " + to_string(c.size());
var d = {};
for (int i = 0; i < 20; ++i) { d[i] = i; }
int calls = 0;
d.filter([&](var k, var v) -> bool { calls += 1; if (k == 5) { d[500] = 1; d.erase(6); } return k % 2 == 0; });
string ds = "";
for (auto [k, v] : d) { ds += to_string(k) + ","; }
log += " | " + to_string(calls) + " " + ds;
log;
)JAI";
            auto [i_out, v_out] = both_mode(src, false);
            check_eq(std::string("true 1334 1334 | 1:4,3:2,2.5:5,a:3,b:1,c:7,true:6, | 01235678911 | 42 3001 | 2 | 5 5,2,6,4,8, | "
                                 "true {\"x\":[1,2],\"y\":2,\"z\":1} {\"x\":[1,2],\"y\":2,\"z\":1} | 349 349 | 19 0,2,4,8,10,12,14,16,18,500,"), i_out, "interpreter");
            check_eq(i_out, v_out, "backend parity");
        });

        // string keys around the eight-byte short form: lengths 0..9, embedded NULs, a string and a utf8_string of the same
        // bytes, a key read as a member, a built key equal to a stored one, an erase
        // the host's script_value == agrees with <=> and the key hash: a heavy value equals itself (its identity), a string
        // and a utf8_string are two kinds
        test("cpp_equality_agrees_with_the_key_order", [this]() {
            auto eng = make_engine();
            const script_value arr = eng->execute("[1, 2]");
            const script_value other = eng->execute("[1, 2]");
            const script_value text(std::string("ab"), eng.get());
            const script_value utf = eng->execute("utf8_string u = \"ab\"; u;");
            check_true(arr == arr, "an array is == itself");
            check_false(arr == other, "two arrays are two identities");
            check_eq((arr <=> other) == 0, arr == other);
            check_eq(arr.map_key_hash(), script_value(arr).map_key_hash());
            check_false(text == utf, "a string is not a utf8_string in C++");
            check_eq((text <=> utf) == 0, text == utf);
        });

        test("map_short_string_keys", [this, both_mode]() {
            const char* src = R"JAI(
var m = {};
array<string> ks = ["", "a", "ab", "abcdefg", "abcdefgh", "abcdefghi", "abcdefgh!", "a\x00", "a\x00b", "hp"];
for (int i = 0; i < ks.size(); ++i) { m[ks[i]] = i; }
m[utf8_string("ab")] = 100;
m[utf8_string("abcdefgh")] = 101;
string out = to_string(m.size());
for (int i = 0; i < ks.size(); ++i) { out += " " + to_string(m[ks[i]]); }
out += " " + to_string(m[utf8_string("ab")]) + " " + to_string(m[utf8_string("abcdefgh")]) + " " + to_string(m["hp"]);
string copy = "abcdefg";
copy += "h";
out += " " + to_string(m[copy]) + " " + to_string(m.has("abcdefgh\x00")) + " " + to_string(m.has("a"));
m.erase("abcdefgh");
out += " " + to_string(m.has("abcdefgh")) + " " + to_string(m[utf8_string("abcdefgh")]);
out;
)JAI";
            auto [i_out, v_out] = both_mode(src, false);
            check_eq(std::string("12 0 1 2 3 4 5 6 7 8 9 100 101 9 4 false true false 101"), i_out, "interpreter");
            check_eq(i_out, v_out, "backend parity");
        });

        // script_map::recall / recall_or_insert against find through every mutator: an entry memo holds only while its map is
        // unchanged and the remembered key equals the asked key; a copy's memo names its source; a new map at a dead map's address
        // starts with no memo
        test("map_entry_memo_through_every_mutator", [this]() {
            auto e = jai::foundry::make_default_engine();
            auto key = [&](const std::string& t) { return jai::script_value(t, e.get()); };
            auto num = [&](int64_t v) { return jai::script_value(static_cast<jai::script_int>(v), e.get()); };
            jai::script_map m;
            jai::script_map::entry_memo memo;
            auto agrees = [&](jai::script_map& map, const std::string& k, const char* step) {
                const jai::script_map::value_type* r = map.recall(memo, key(k));
                auto it = map.find(key(k));
                const jai::script_map::value_type* f = it == map.end() ? nullptr : &*it;
                check_true(r == f, std::string("recall agrees with find after ") + step + " (" + k + ")");
            };
            m[key("a")] = num(1);
            agrees(m, "a", "an insert");
            m.recall_or_insert(memo, key("a"))->second = num(2);
            check_eq(int64_t(2), m.find(key("a"))->second.as_int(), "a store through the memo lands in the entry");
            m[key("b")] = num(3);                         agrees(m, "a", "another insert");
            m[key("a")] = num(4);                         agrees(m, "a", "a value store");
            m.erase(key("a"));                            agrees(m, "a", "an erase of the key");
            check_true(m.recall(memo, key("a")) == nullptr, "an erased key is absent");
            m.recall_or_insert(memo, key("a"))->second = num(5);
            agrees(m, "a", "an insert through the memo");
            m.erase(key("b"));                            agrees(m, "a", "an erase of another key");
            for (int i = 0; i < 2000; ++i) { m[key("g" + std::to_string(i))] = num(i); }
            agrees(m, "a", "growth");
            agrees(m, "g1999", "growth");
            m.clear();                                    agrees(m, "a", "clear");
            m[key("a")] = num(6); agrees(m, "a", "a reinsert");
            jai::script_map other; other[key("a")] = num(7);
            m = other;                                    agrees(m, "a", "a copy assignment");
            check_eq(int64_t(7), m.recall(memo, key("a"))->second.as_int(), "the assigned map's value");
            jai::script_map moved; moved[key("a")] = num(8);
            m = std::move(moved);                         agrees(m, "a", "a move assignment");
            jai::script_map copy(m);
            m.recall(memo, key("a"));
            const jai::script_map::value_type* in_copy = copy.recall(memo, key("a"));
            check_true(in_copy == &*copy.find(key("a")), "a memo taken on the source misses on the copy");
            alignas(jai::script_map) unsigned char buf[sizeof(jai::script_map)];
            auto* first = new (buf) jai::script_map();
            (*first)[key("a")] = num(9);
            check_true(first->recall(first->lane_memo(), key("a")) != nullptr, "the first map's lane memo holds");
            first->~script_map();
            auto* second = new (buf) jai::script_map();
            (*second)[key("a")] = num(10);
            const jai::script_map::value_type* r2 = second->recall(second->lane_memo(), key("a"));
            check_true(r2 == &*second->find(key("a")) && r2->second.as_int() == 10, "a new map at the dead map's address starts without its memo");
            second->~script_map();
        });

        // the vm's map lanes remember their last entry: a get then a store of one key, a key changed in place between them, a copy,
        // a replaced map, an erase, a clear, a map kept across a re-run of its unit, and a parallel body reading the map (workers
        // leave the shared memo alone) all read what the interpreter reads
        // an int key's lookup beside keys of other kinds (float, char, bool, string), present and absent, read through
        // a subscript, get and has on both backends
        test("map_int_keys_beside_other_kinds", [this, both_mode]() {
            const char* src = R"JAI(
map<var, var> m = {};
m[1] = "int"; m[1.0] = "float"; m['1'] = "char"; m[true] = "bool"; m["1"] = "string"; m[-7] = "neg";
for (int i = 100; i < 160; ++i) { m[i] = i * 2; }
string out = to_string(m[1]) + to_string(m[1.0]) + to_string(m['1']) + to_string(m[true]) + to_string(m["1"]) + to_string(m[-7]);
int s = 0; for (int i = 90; i < 170; ++i) { if (m.has(i)) { s += m.get(i, 0); } }
string miss = ""; try { miss = to_string(m[2]); } catch (e) { miss = e; }
out += " " + to_string(s) + " " + miss + " " + to_string(m.size());
out;
)JAI";
            auto [i_out, v_out] = both_mode(src, true);
            // '1' and "1" are one key: the string store replaces the char key's value
            check_eq(std::string("intfloatstringboolstringneg 15540 Key 2 not found in map 65"), i_out, "interpreter");
            check_eq(i_out, v_out, "backend parity");
        });

        test("map_lane_memo_parity", [this, both_mode]() {
            const char* src = R"JAI(
map<string, int> m = {};
array<string> src = ["a", "b", "a", "c", "b", "a"];
for (auto& s : src) { m[s] = m.get(s, 0) + 1; }
string s = "a";
m[s] = m.get(s, 0) + 10;
s.append("x");
m[s] = m.get(s, 0) + 100;
var m2 = m;
m2["a"] = -1;
string out = to_string(m["a"]) + " " + to_string(m2["a"]) + " " + to_string(m["ax"]) + " " + to_string(m["b"]);
m = {"a": 5};
m["a"] = m.get("a", 0) + 1;
out += " " + to_string(m["a"]) + " " + to_string(m.size());
m.erase("a");
out += " " + to_string(m.get("a", 7));
m.clear();
m["z"] = m.get("z", 1) + 1;
out += " " + to_string(m["z"]);
map<string, int> table = {"k0": 10, "k1": 20, "k2": 30, "k3": 40};
array<int> xs = [];
for (int i = 0; i < 64; ++i) { xs.push(i); }
table["k1"] = table.get("k1", 0) + 1;
parallel_for (auto& x : xs) { x = table.get("k" + to_string(x % 4), 0); }
table["k1"] = table.get("k1", 0) + 1;
int total = 0;
for (int x : xs) { total += x; }
out += " " + to_string(total) + " " + to_string(table["k1"]);
out;
)JAI";
            auto [i_out, v_out] = both_mode(src, false);
            check_eq(std::string("13 -1 100 2 6 1 7 2 1616 22"), i_out, "interpreter");
            check_eq(i_out, v_out, "backend parity");
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(e);
                const char* unit = "map<string, int> kept;\nint bump(string k) { kept[k] = kept.get(k, 0) + 1; return kept[k]; }\n";
                e->execute(unit);
                e->execute("bump(\"a\"); bump(\"a\"); bump(\"b\");");
                e->execute(unit);
                const std::string got = e->execute("to_string(bump(\"a\")) + \" \" + to_string(bump(\"b\")) + \" \" + to_string(kept.size());").to_string();
                check_eq(std::string("3 2 2"), got, use_vm ? "re-run unit (vm)" : "re-run unit (interpreter)");
            }
        });

        test("map_field_survives_hot_reload", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(e);
                e->execute("class Inv { map<string, int> items = {}; }\nvar inv = Inv();\n"
                           "for (int i = 0; i < 300; ++i) { inv.items[\"k\" + to_string(i)] = i; }");
                e->execute("class Inv { map<string, int> items = {}; int extra = 7; }");
                const std::string got = e->execute(
                    "string first = \"\"; for (auto [k, v] : inv.items) { if (first == \"\") { first = k; } }\n"
                    "inv.items[\"k5\"] = 50; inv.items.erase(\"k6\");\n"
                    "to_string(inv.items.size()) + \" \" + to_string(inv.items[\"k42\"]) + \" \" + to_string(inv.items[\"k5\"]) + \" \" + first + \" \" + to_string(inv.extra);").to_string();
                check_eq(std::string("299 42 50 k0 7"), got, use_vm ? "vm" : "interpreter");
            }
        });

        // script_map against an ordered model under random operations on clustered ints, sparse ints, negatives, floats
        // and strings: every lookup, the size, the key order of iteration, copies, and the address of every live value
        // (an entry stays where it was made until its key is erased)
        test("map_storage_against_a_model", [this]() {
            auto e = jai::foundry::make_default_engine();
            jai::engine* eng = e.get();
            std::map<jai::script_value, int64_t, jai::script_value_map_less> model;
            jai::script_map m;
            std::map<jai::script_value, const jai::script_map::value_type*, jai::script_value_map_less> where;
            uint64_t rng = 0x9E3779B97F4A7C15ull;
            auto next = [&]() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
            auto make_key = [&]() -> jai::script_value {
                const uint64_t r = next() % 100;
                if (r < 45) return jai::script_value(static_cast<jai::script_int>(1028 + next() % 300), eng);
                if (r < 60) return jai::script_value(static_cast<jai::script_int>(static_cast<int64_t>(next() % 2000000) - 1000000), eng);
                if (r < 70) return jai::script_value(static_cast<jai::script_int>(-(static_cast<int64_t>(next() % 64))), eng);
                if (r < 78) return jai::script_value(static_cast<jai::script_float>(static_cast<double>(1028 + next() % 300)), eng);
                if (r < 82) return jai::script_value(static_cast<jai::script_int>(INT64_MAX - static_cast<int64_t>(next() % 4)), eng);
                return jai::script_value(std::string("k") + std::to_string(next() % 400), eng);
            };
            bool ok = true;
            for (int step = 0; step < 20000 && ok; ++step) {
                const uint64_t op = next() % 100;
                const jai::script_value k = make_key();
                if (op < 55) {
                    const int64_t v = static_cast<int64_t>(next() % 1000);
                    m[k] = jai::script_value(static_cast<jai::script_int>(v), eng);
                    model[k] = v;
                    if (!where.count(k)) { where[k] = &*m.find(k); }
                } else if (op < 75) {
                    const bool had = model.erase(k) != 0;
                    where.erase(k);
                    ok = ok && (m.erase(k) == (had ? 1u : 0u));
                } else if (op < 98) {
                    const jai::script_map::value_type* hit = m.lookup(k);
                    auto it = model.find(k);
                    ok = ok && ((hit != nullptr) == (it != model.end())) && (!hit || hit->second.as_int() == it->second);
                } else if (op == 98) {
                    jai::script_map copy(m);
                    m = copy;
                    where.clear();
                    for (const auto& kv : model) { where[kv.first] = &*m.find(kv.first); }
                } else {
                    m.clear(); model.clear(); where.clear();
                }
                if (step % 997 == 0 || step == 19999) {
                    ok = ok && m.size() == model.size();
                    auto mi = model.begin();
                    for (const auto& kv : m) {
                        if (mi == model.end() || !(kv.first == mi->first) || kv.second.as_int() != mi->second) { ok = false; break; }
                        ++mi;
                    }
                    ok = ok && mi == model.end();
                    for (const auto& [key, addr] : where) { const auto* hit = m.lookup(key); ok = ok && hit == addr; }
                }
                if (!ok) { check_true(false, "script_map disagrees with the model at step " + std::to_string(step)); }
            }
            check_true(ok, "script_map agrees with the ordered model through every operation");
        });

        // int keys filled out of order, below the first key and far away keep their addresses, their order and their lookups; a
        // bound int finds and replaces the int key it equals
        test("map_int_keys_grow_rebase_erase_and_bound_keys", [this]() {
            auto e = jai::foundry::make_default_engine();
            jai::engine* eng = e.get();
            auto key = [eng](int64_t x) { return jai::script_value(static_cast<jai::script_int>(x), eng); };
            jai::script_map m;
            std::vector<int64_t> order;
            for (int64_t i = 0; i < 1000; ++i) { order.push_back(i); }
            uint64_t rng = 12345;
            for (size_t i = order.size(); i > 1; --i) { rng = rng * 6364136223846793005ull + 1442695040888963407ull; std::swap(order[i - 1], order[(rng >> 33) % i]); }
            std::map<int64_t, const jai::script_map::value_type*> where;
            for (int64_t x : order) { m[key(x)] = key(x * 2); where[x] = m.lookup(key(x)); }
            for (int64_t x = -1; x >= -500; --x) { m[key(x)] = key(x * 2); where[x] = m.lookup(key(x)); }
            for (int64_t x : {int64_t(1) << 40, int64_t(-1) << 40, INT64_MAX, INT64_MIN, int64_t(5000)}) { m[key(x)] = key(7); where[x] = m.lookup(key(x)); }
            bool ok = m.size() == where.size();
            for (const auto& [x, addr] : where) { ok = ok && m.lookup(key(x)) == addr && m.count(key(x)) == 1; }
            check_true(ok, "every key keeps its address through growth and re-basing");
            check_true(m.lookup(key(1000)) == nullptr && m.lookup(key(-501)) == nullptr && m.lookup(key(4999)) == nullptr, "absent keys stay absent");
            for (int64_t x = -500; x < 1000; x += 2) { ok = ok && m.erase(key(x)) == 1; where.erase(x); }
            for (int64_t x = -500; x < 1000; x += 4) { m[key(x)] = key(-x); where[x] = m.lookup(key(x)); }
            auto wi = where.begin();
            for (const auto& kv : m) { ok = ok && wi != where.end() && kv.first.as_int() == wi->first && &kv == wi->second; ++wi; }
            check_true(ok && wi == where.end(), "erase and re-insert keep the order and the remaining addresses");
            check_eq(int64_t(-8), m.at(key(8)).as_int(), "a re-inserted key holds its new value");
            check_eq(int64_t(2 * 7), m.at(key(7)).as_int(), "an odd key kept its value");

            int64_t host = 7;
            jai::script_value bound = jai::script_value::make_cpp_bound(&host, eng);
            check_eq(int64_t(14), m.at(bound).as_int(), "a bound int finds the int key it equals");
            m[bound] = key(99);
            check_eq(int64_t(99), m.at(key(7)).as_int(), "a store through a bound int replaces that key's value");
            const size_t before = m.size();
            host = 123456789;
            m[jai::script_value::make_cpp_bound(&host, eng)] = key(5);
            check_eq(before + 1, m.size(), "a bound int with a new value adds one key");
            check_eq(int64_t(5), m.at(key(123456789)).as_int(), "an int key finds the bound key it equals");
            check_eq(size_t(1), m.erase(key(123456789)), "and erases it");
            check_eq(before, m.size(), "size after the erase");

            jai::script_map copy(m);
            ok = copy.size() == m.size();
            auto ci = copy.begin();
            for (const auto& kv : m) { ok = ok && ci != copy.end() && kv.first == ci->first && kv.second == ci->second && &*ci != &kv; ++ci; }
            check_true(ok, "a copy holds the same entries in the same order at its own addresses");
            jai::script_map moved(std::move(copy));
            check_eq(m.size(), moved.size(), "a move keeps every entry");
            check_true(copy.empty() && copy.lookup(key(8)) == nullptr, "the moved-from map is empty");
            copy[key(8)] = key(1);
            check_eq(size_t(1), copy.size(), "and usable");
        });

        // maps mixing clustered int keys with sparse ints, floats and strings read, store, iterate and serialize as one
        // ordered map on both backends; a reference into a value survives inserts around it
        test("map_mixed_keys_read_store_iterate_and_serialize", [this, both_mode]() {
            const char* src = R"JAI(
map<var, var> m = {};
for (int i = 0; i < 124; ++i) { m[1028 + i * 2] = i; }
m[-5] = "neg"; m[1000000] = "far"; m[1030.0] = "float"; m["1030"] = "text"; m[true] = "bool";
var& r = m[1100];
for (int i = 0; i < 300; ++i) { m[2000 + i * 7] = i; }
r = "kept";
m.erase(1032); m[1032] = "again";
string out = `${to_string(m[1028])} ${to_string(m[1030])} ${to_string(m[1030.0])} ${to_string(m["1030"])} ${to_string(m[1100])} ${to_string(m[1032])} ${to_string(m.has(1029))} ${to_string(m.get(1031, -1))}`;
string keys = ""; int n = 0;
for (auto [k, v] : m) { if (n < 6 || n > m.size() - 4) { keys += `${to_string(k)},`; } ++n; }
map<int, int> lump = {};
for (int i = 0; i < 124; ++i) { lump[1028 + i] = i * 3; }
int s = 0; for (int i = 1000; i < 1200; ++i) { s += lump.get(i, 0); if (lump.has(i)) { lump[i] = lump[i] + 1; } }
var copy = lump; copy[1028] = -1;
out += ` ${keys} ${to_string(n)} ${to_string(s)} ${to_string(lump[1028])} ${to_string(copy[1028])} ${to_json(lump).size()}`;
out;
)JAI";
            auto [i_out, v_out] = both_mode(src, true);
            check_eq(i_out, v_out, "backend parity");
            check_true(i_out.find("ERROR") == std::string::npos, "runs: " + i_out);
            check_eq(std::string("0 1 float text kept again false -1 -5,1028,1030,1032,1034,1036,1030.0,1030,true, 429 22878 1 -1 1338"), i_out, "pinned output");
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::map_tests)
