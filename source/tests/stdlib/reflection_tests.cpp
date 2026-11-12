// reflect:: — the script-side reflection capability (docs/site/guide/12-reflection.html front door,
// companion). These pins hold the three contracts: reserved
// namespace (engine truth is not impersonatable; math:: stays open by contract),
// live-query recapture (hot reload -> re-ask sees the new shape), and allowlist scoping
// (an engine reflects only what IT registered).

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/macros.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <map>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class reflect_host_thing : public property_owner<reflect_host_thing> {
public:
    JAI_PROPERTY((int), armor, 4);
    reflect_host_thing() = default;
};

class reflection_tests : public suite {
    std::string declaration_error(engine& eng, const std::string& source) {
        try {
            eng.execute(source);
        } catch (const std::exception& error) {
            return error.what();
        }
        return {};
    }

    static std::shared_ptr<engine> make_reflective_engine() {
        auto eng = jai::foundry::make_engine();
        stdlib::register_all(*eng);
        return eng;
    }

public:
    reflection_tests() : suite("Reflection") {}

    void forge_tests() override {
        test("namespace_call_resolves", [this]() {
            // The registration probe: reflect:: members are namespace VARIABLES holding
            // function values, called through ordinary namespace-member syntax.
            auto eng = make_reflective_engine();
            check_eq(eng->execute("reflect::type_name(42)").as<std::string>(), "int");
        });

        test("reserved_namespace_rejects_script_declarations", [this]() {
            auto eng = make_reflective_engine();
            check_throws([&]() { eng->execute("namespace reflect { function evil() { return 1; } }"); },
                "namespace reflect is engine-owned");
            check_throws([&]() { eng->execute("namespace reflect::sub { function evil() { return 1; } }"); },
                "flat sub-names are reserved with the prefix");
        });

        test("jai_namespace_rejects_script_declarations", [this]() {
            for (const std::string name : {"jai", "jai::sub", "jai::sub::deep"}) {
                for (const std::string body : {"function f() { return 1; }", "var value = 1;", "class Value { int value = 1; }"}) {
                    auto eng = jai::foundry::make_engine();
                    const auto error = declaration_error(*eng, "namespace " + name + " { " + body + " }");
                    check(!error.empty(), "script declarations into " + name + " must be rejected");
                    check_eq(eng->execute("6 * 7").as_int(), script_int{42});
                }
            }
        });

        test("reserved_namespace_error_text", [this]() {
            for (const std::string name : {"reflect", "reflect::sub", "jai", "jai::sub"}) {
                auto eng = jai::foundry::make_engine();
                check_eq("Namespace '" + name + "' is reserved for the engine (C++-registered only)",
                    declaration_error(*eng, "namespace " + name + " { var value = 1; }"));
            }
        });

        test("reserved_namespace_preserves_host_values", [this]() {
            for (const std::string name : {"jai", "jai::sub", "reflect", "reflect::sub"}) {
                auto eng = jai::foundry::make_engine();
                auto& ns = detail::engine_access::script_namespaces(*eng)[detail::engine_access::symbolize(*eng, name)];
                ns = std::make_shared<script_namespace_data>();
                ns->variables[detail::engine_access::symbolize(*eng, "host_value")] = script_value(script_int{42}, eng.get());
                int calls = 0;
                eng->add_function("touch", [&calls]() -> script_int { ++calls; return 7; });
                check_eq(eng->execute(name + "::host_value").as_int(), script_int{42});
                const auto error = declaration_error(*eng,
                    "namespace " + name + " { var host_value = touch(); }");
                check(!error.empty(), "a script cannot reopen " + name);
                check_eq(0, calls);
                check_eq(eng->execute(name + "::host_value").as_int(), script_int{42});
            }
        });

        test("reserved_namespace_saved_program_rejected", [this]() {
            auto writer = jai::foundry::make_engine();
            auto bytes = writer->jaibite("namespace jai::saved { var value = 1; }").save_bytes();
            auto reader = jai::foundry::make_engine();
            auto bite = reader->jaibite_load_bytes(bytes);
            std::string error;
            try {
                bite.execute();
            } catch (const std::exception& caught) {
                error = caught.what();
            }
            check_eq(std::string("Namespace 'jai::saved' is reserved for the engine (C++-registered only)"),
                error);
            check_eq(reader->execute("6 * 7").as_int(), script_int{42});
        });

        test("reserved_namespace_prefix_boundaries", [this]() {
            for (const std::string name : {"jailed", "jai_script", "reflector", "reflective", "outer::jai"}) {
                auto eng = jai::foundry::make_engine();
                check_eq(eng->execute("namespace " + name +
                    " { function answer() { return 42; } } " + name + "::answer()").as_int(), script_int{42});
            }
        });

        test("math_namespace_stays_open_by_contract", [this]() {
            // The deliberate contrast: script-contributed math is legitimate math.
            auto eng = make_reflective_engine();
            eng->execute("namespace math { function double_it(auto x) -> auto { return x * 2; } }");
            check_eq(eng->execute("math::double_it(21)").as_int(), (int64_t)21 * 2);
        });

        test("type_name_is_semantic", [this]() {
            auto eng = make_reflective_engine();
            check_eq(eng->execute("reflect::type_name(1.5)").as<std::string>(), "float");
            check_eq(eng->execute("reflect::type_name(\"hi\")").as<std::string>(), "string");
            check_eq(eng->execute("reflect::type_name(null)").as<std::string>(), "null");
            eng->execute("class Creature { int hp = 10; }");
            check_eq(eng->execute("reflect::type_name(Creature())").as<std::string>(), "Creature");
            // References answer what they hold, never the wrapper
            check_eq(eng->execute(
                "var c = Creature(); var& r = c; reflect::type_name(r)").as<std::string>(), "Creature");
        });

        test("type_names_spell_their_element_types", [this]() {
            auto eng = make_reflective_engine();
            check_eq(eng->execute("array<int> a = [1]; reflect::type_name(a)").as<std::string>(), "array<int>");
            check_eq(eng->execute("map<string, float> m = {\"k\": 1.0}; reflect::type_name(m)").as<std::string>(), "map<string,float>");
            check_eq(eng->execute("array<array<string>> n = [[\"x\"]]; reflect::type_name(n)").as<std::string>(), "array<array<string>>");
            check_eq(eng->execute("var u = [1, \"x\"]; reflect::type_name(u)").as<std::string>(), "array");
            eng->execute("class Bag { array<int> items = []; static array<string> names = []; static int count = 0; map<string, int> tally; var loose; auto inferred = 1; }");
            auto fields = eng->execute("reflect::fields(\"Bag\")");
            std::map<std::string, std::string> types;
            for (const auto& entry : fields.as_array()) {
                const auto& m = entry.as_map();
                types[m.at(script_value(std::string("name"), eng.get())).as<std::string>()] =
                    m.at(script_value(std::string("type"), eng.get())).as<std::string>();
            }
            check_eq(std::string("array<int>"), types["items"], "a typed field");
            check_eq(std::string("array<string>"), types["names"], "a typed static");
            check_eq(std::string("int"), types["count"], "a scalar static");
            check_eq(std::string("map<string,int>"), types["tally"], "a typed map field");
            check_eq(std::string("var"), types["loose"], "var");
            check_eq(std::string("auto"), types["inferred"], "auto");
            check_eq(eng->execute("function f(array<int> a, map<string, bool> b) {} reflect::function_arguments(\"f\")[0][0][\"type\"]").as<std::string>(),
                     "array<int>", "a parameter's declared type");
        });

        test("has_field_and_has_method_walk_the_chain", [this]() {
            auto eng = make_reflective_engine();
            eng->execute(R"(
                class Base { int hp = 5; void heal() { hp = hp + 1; } }
                class Derived : Base { int mana = 2; }
            )");
            check_eq(eng->execute("reflect::has_field(Derived(), \"hp\")").as<bool>(), true);
            check_eq(eng->execute("reflect::has_field(Derived(), \"mana\")").as<bool>(), true);
            check_eq(eng->execute("reflect::has_field(Derived(), \"nope\")").as<bool>(), false);
            check_eq(eng->execute("reflect::has_method(\"Derived\", \"heal\")").as<bool>(), true);
            check_eq(eng->execute("reflect::has_method(\"Derived\", \"nope\")").as<bool>(), false);
        });

        test("classes_bases_and_unknown_class_error", [this]() {
            auto eng = make_reflective_engine();
            eng->execute("class Alpha {} class Beta : Alpha {}");
            check_eq(eng->execute(
                "var found = 0; for (auto c : reflect::classes()) { if (c == \"Alpha\" || c == \"Beta\") { found = found + 1; } } found"
            ).as_int(), (int64_t)2);
            check_eq(eng->execute("reflect::bases(Beta())[0]").as<std::string>(), "Alpha");
            check_eq(eng->execute("reflect::bases(\"Alpha\").size()").as_int(), (int64_t)0);
            check_throws([&]() { eng->execute("reflect::bases(\"Gremlin\");"); },
                "unknown class name raises the family error");
        });

        test("is_cpp_bound_distinguishes_host_from_script", [this]() {
            auto eng = make_reflective_engine();
            dynamic_binder<reflect_host_thing>(*eng, "HostThing")
                .constructor<>()
                .build();
            eng->execute("class ScriptThing { int x = 1; }");
            check_eq(eng->execute("reflect::is_cpp_bound(\"HostThing\")").as<bool>(), true);
            check_eq(eng->execute("reflect::is_cpp_bound(\"ScriptThing\")").as<bool>(), false);
            check_eq(eng->execute("reflect::is_cpp_bound(ScriptThing())").as<bool>(), false);
            check_eq(eng->execute("reflect::is_cpp_bound(42)").as<bool>(), false);
        });

        test("generation_bumps_on_hot_reload_and_recapture_sees_new_shape", [this]() {
            // The live-query contract: reload -> the SAME queries answer the new shape,
            // and generation() is the cheap should-I-re-ask poll.
            auto eng = make_reflective_engine();
            eng->execute("class Evolver { int a = 1; }");
            auto gen0 = eng->execute("reflect::generation(\"Evolver\")").as_int();
            check_eq(eng->execute("reflect::has_field(\"Evolver\", \"b\")").as<bool>(), false);
            eng->execute("class Evolver { int a = 1; int b = 2; }");   // hot reload: new field
            auto gen1 = eng->execute("reflect::generation(\"Evolver\")").as_int();
            check(gen1 > gen0, "generation moved on redefinition");
            check_eq(eng->execute("reflect::has_field(\"Evolver\", \"b\")").as<bool>(), true);
        });

        test("fields_declaration_order_base_first", [this]() {
            // The determinism contract: base's fields first, then own, each in SOURCE
            // order — never hash order. Access labels and static flags ride the entry.
            auto eng = make_reflective_engine();
            eng->execute(R"(
                class FBase { int hp = 5; private: string secret = "s"; }
                class FDer : FBase { float speed = 1.5; static int count = 0; }
            )");
            eng->execute("var fs = reflect::fields(FDer());");
            check_eq(eng->execute("fs.size()").as_int(), (int64_t)4);
            check_eq(eng->execute("fs[0][\"name\"]").as<std::string>(), "hp");
            check_eq(eng->execute("fs[0][\"from\"]").as<std::string>(), "FBase");
            check_eq(eng->execute("fs[0][\"type\"]").as<std::string>(), "int");
            check_eq(eng->execute("fs[1][\"name\"]").as<std::string>(), "secret");
            check_eq(eng->execute("fs[1][\"access\"]").as<std::string>(), "private");
            check_eq(eng->execute("fs[2][\"name\"]").as<std::string>(), "speed");
            check_eq(eng->execute("fs[2][\"from\"]").as<std::string>(), "FDer");
            check_eq(eng->execute("fs[3][\"name\"]").as<std::string>(), "count");
            check_eq(eng->execute("fs[3][\"static\"]").as<bool>(), true);
            // Order is a cross-engine contract, not one engine's accident
            auto eng2 = make_reflective_engine();
            eng2->execute(R"(
                class FBase { int hp = 5; private: string secret = "s"; }
                class FDer : FBase { float speed = 1.5; static int count = 0; }
            )");
            check_eq(eng2->execute("reflect::fields(FDer())[0][\"name\"]").as<std::string>(), "hp");
            check_eq(eng2->execute("reflect::fields(FDer())[2][\"name\"]").as<std::string>(), "speed");
        });

        test("fields_reload_recaptures_new_shape_and_order", [this]() {
            auto eng = make_reflective_engine();
            eng->execute("class Shape { int a = 1; int b = 2; }");
            check_eq(eng->execute("reflect::fields(Shape()).size()").as_int(), (int64_t)2);
            eng->execute("class Shape { int b = 2; int c = 3; int a = 1; }");   // reload: reorder + add
            eng->execute("var fs2 = reflect::fields(\"Shape\");");
            check_eq(eng->execute("fs2.size()").as_int(), (int64_t)3);
            check_eq(eng->execute("fs2[0][\"name\"]").as<std::string>(), "b");
            check_eq(eng->execute("fs2[1][\"name\"]").as<std::string>(), "c");
            check_eq(eng->execute("fs2[2][\"name\"]").as<std::string>(), "a");
        });

        test("fields_of_host_class_reports_schema_kinds", [this]() {
            auto eng = make_reflective_engine();
            dynamic_binder<reflect_host_thing>(*eng, "HostThing2")
                .constructor<>()
                .build();
            eng->execute("var hf = reflect::fields(\"HostThing2\");");
            check_eq(eng->execute("hf.size()").as_int(), (int64_t)1);
            check_eq(eng->execute("hf[0][\"name\"]").as<std::string>(), "armor");
            check_eq(eng->execute("hf[0][\"type\"]").as<std::string>(), "int");
            check_eq(eng->execute("hf[0][\"kind\"]").as<std::string>(), "field");
            // Non-class values: empty array, never an error (property grids call this on anything)
            check_eq(eng->execute("reflect::fields(42).size()").as_int(), (int64_t)0);
        });

        test("instances_walks_the_live_registry", [this]() {
            auto eng = make_reflective_engine();
            eng->execute("class Tracked { int n = 0; }");
            eng->execute("var a = new Tracked(); var b = new Tracked(); a.n = 7;");
            check_eq(eng->execute("reflect::instances(\"Tracked\").size()").as_int(), (int64_t)2);
            check_eq(eng->execute(
                "var total = 0; for (auto t : reflect::instances(\"Tracked\")) { total = total + t.n; } total"
            ).as_int(), (int64_t)7);
        });

        test("instances_answer_in_creation_order", [this]() {
            auto eng = make_reflective_engine();
            eng->execute("class Ordered { int n = 0; Ordered() {} Ordered(int v) { n = v; } }");
            eng->execute("var keep = []; for (int i = 0; i < 8; ++i) { keep.push(new Ordered(i)); }");
            eng->execute("keep.erase(1); keep.erase(4); var late = new Ordered(9);");
            check_eq(std::string("0,2,3,4,6,7,9,"), eng->execute(
                "var s = \"\"; for (auto t : reflect::instances(\"Ordered\")) { s = s + to_string(t.n) + \",\"; } s"
            ).as<std::string>());
        });

        test("get_set_invoke_read_write_like_direct_syntax", [this]() {
            auto eng = make_reflective_engine();
            eng->execute(R"(
                class Soldier {
                    int hp = 10;
                    private: int secret = 1;
                    public: int wound(int amount) { hp = hp - amount; return hp; }
                    private: int covert() { return secret; }
                }
                var s = Soldier();
            )");
            check_eq(eng->execute("reflect::get(s, \"hp\")").as_int(), (int64_t)10);
            eng->execute("reflect::set(s, \"hp\", 42);");
            check_eq(eng->execute("s.hp").as_int(), (int64_t)42);
            check_eq(eng->execute("reflect::invoke(s, \"wound\", 5)").as_int(), (int64_t)37);
            check_eq(eng->execute("s.hp").as_int(), (int64_t)37);
            // Reads see everything (the to_json rule): private state is readable, with
            // access REPORTED by fields() so tools can choose to respect it
            check_eq(eng->execute("reflect::get(s, \"secret\")").as_int(), (int64_t)1);
        });

        test("set_invoke_bind_and_construct_reach_private_members", [this]() {
            // Reflection ignores script access control: reflecting is an intentional request for
            // the whole shape, while direct syntax keeps enforcing private
            auto eng = make_reflective_engine();
            eng->execute(R"(
                class Vault {
                    private: int gold = 100;
                    private: int peek() { return gold; }
                }
                class Sealed {
                    public: int n = 0;
                    private: Sealed(int v) { n = v; }
                }
                var v = Vault();
            )");
            check_eq(eng->execute(R"(
                var direct_err = "";
                try { v.gold = 5; } catch (e) { direct_err = e; }
                direct_err
            )").as<std::string>(), std::string("Cannot access private member 'gold' of class 'Vault'"));
            eng->execute("reflect::set(v, \"gold\", 5);");
            check_eq(eng->execute("reflect::get(v, \"gold\")").as_int(), (int64_t)5);
            check_eq(eng->execute("reflect::invoke(v, \"peek\")").as_int(), (int64_t)5);
            check_eq(eng->execute("var p = reflect::bind(v, \"peek\"); p()").as_int(), (int64_t)5);
            check_eq(eng->execute("var s = reflect::construct(\"Sealed\", 9); s.n").as_int(), (int64_t)9);
            check_eq(eng->execute("var sh = reflect::construct_shared(\"Sealed\", 4); sh.n").as_int(), (int64_t)4);
            // Builtin types expose no internals through reflection
            check_eq(eng->execute(R"(
                var err = "";
                try { array<int> xs = [1]; reflect::set(xs, "size", 3); } catch (e) { err = "refused"; }
                err
            )").as<std::string>(), std::string("refused"));
        });

        test("call_construct_and_construct_shared", [this]() {
            auto eng = make_reflective_engine();
            eng->execute(R"(
                function raise(auto x) -> auto { return x + 1; }
                namespace tools { function twice(auto x) -> auto { return x * 2; } }
                class P { int x = 0; P(int v) { x = v; } }
            )");
            check_eq(eng->execute("reflect::call(\"raise\", 41)").as_int(), (int64_t)42);
            check_eq(eng->execute("reflect::call(\"tools::twice\", 21)").as_int(), (int64_t)42);
            check_throws([&]() { eng->execute("reflect::call(\"nope\");"); },
                "unknown function raises the family error");
            check_eq(eng->execute("reflect::construct(\"P\", 7).x").as_int(), (int64_t)7);
            check_throws([&]() { eng->execute("reflect::construct(\"Gremlin\", 1);"); },
                "unknown class raises the family error");
            // Value vs reference semantics: == Class(...) vs == new Class(...)
            eng->execute("var a = reflect::construct(\"P\", 1); var b = a; b.x = 9;");
            check_eq(eng->execute("a.x").as_int(), (int64_t)1);
            eng->execute("var sp = reflect::construct_shared(\"P\", 1); var tp = sp; tp.x = 9;");
            check_eq(eng->execute("sp.x").as_int(), (int64_t)9);
        });

        test("statics_by_class_name", [this]() {
            auto eng = make_reflective_engine();
            eng->execute(R"(
                class Counter {
                    static int count = 3;
                    static int bump() { count = count + 1; return count; }
                }
            )");
            check_eq(eng->execute("reflect::get(\"Counter\", \"count\")").as_int(), (int64_t)3);
            eng->execute("reflect::set(\"Counter\", \"count\", 10);");
            check_eq(eng->execute("Counter::count").as_int(), (int64_t)10);
            check_eq(eng->execute("reflect::invoke(\"Counter\", \"bump\")").as_int(), (int64_t)11);
        });

        test("host_properties_through_the_same_doors", [this]() {
            // Host property_owner properties answer through the _get_/_set_ method door
            auto eng = make_reflective_engine();
            dynamic_binder<reflect_host_thing>(*eng, "HostThing3")
                .constructor<>()
                .build();
            eng->execute("var h = HostThing3();");
            check_eq(eng->execute("reflect::get(h, \"armor\")").as_int(), (int64_t)4);
            eng->execute("reflect::set(h, \"armor\", 11);");
            check_eq(eng->execute("h.armor").as_int(), (int64_t)11);
            check_eq(eng->execute("reflect::get(h, \"armor\")").as_int(), (int64_t)11);
        });

        test("methods_enumerate_sorted_with_overloads_and_chain", [this]() {
            // Determinism: (name, arity, static) order, never hash order; overloads are
            // separate entries; auto-accessors and constructors are OTHER surfaces
            // (fields() / construct()) and stay out.
            auto eng = make_reflective_engine();
            eng->execute(R"(
                class MBase { void alpha() {} }
                class MDer : MBase {
                    int zeta(int a, float b = 1.5) { return a; }
                    int zeta(int a, int b, int c) { return a + b + c; }
                    private: void hush() {}
                    static int total() { return 1; }
                }
                var ms = reflect::methods("MDer");
            )");
            check_eq(eng->execute("ms.size()").as_int(), (int64_t)5);
            check_eq(eng->execute("ms[0][\"name\"]").as<std::string>(), "alpha");
            check_eq(eng->execute("ms[0][\"from\"]").as<std::string>(), "MBase");
            check_eq(eng->execute("ms[1][\"name\"]").as<std::string>(), "hush");
            check_eq(eng->execute("ms[1][\"access\"]").as<std::string>(), "private");
            check_eq(eng->execute("ms[2][\"name\"]").as<std::string>(), "total");
            check_eq(eng->execute("ms[2][\"static\"]").as<bool>(), true);
            check_eq(eng->execute("ms[3][\"name\"]").as<std::string>(), "zeta");
            check_eq(eng->execute("ms[3][\"arity\"]").as_int(), (int64_t)2);
            check_eq(eng->execute("ms[4][\"arity\"]").as_int(), (int64_t)3);
            check_eq(eng->execute("ms[3][\"params\"][1][\"name\"]").as<std::string>(), "b");
            check_eq(eng->execute("ms[3][\"params\"][1][\"type\"]").as<std::string>(), "float");
            check_eq(eng->execute("ms[3][\"params\"][1][\"default\"]").as<bool>(), true);
        });

        test("method_arguments_one_param_array_per_overload", [this]() {
            auto eng = make_reflective_engine();
            eng->execute(R"(
                class W {
                    int f(int a) { return a; }
                    int f(int a, string b) { return a; }
                }
            )");
            eng->execute("var pa = reflect::method_arguments(\"W\", \"f\");");
            check_eq(eng->execute("pa.size()").as_int(), (int64_t)2);
            check_eq(eng->execute("pa[0].size()").as_int(), (int64_t)1);
            check_eq(eng->execute("pa[1].size()").as_int(), (int64_t)2);
            check_eq(eng->execute("pa[1][1][\"type\"]").as<std::string>(), "string");
            check_throws([&]() { eng->execute("reflect::method_arguments(\"W\", \"nope\");"); },
                "unknown method raises the family error");
        });

        test("functions_globals_and_function_arguments", [this]() {
            auto eng = make_reflective_engine();
            eng->execute(R"(
                function greet(string who, int times = 1) -> string { return who; }
                namespace tools { function twice(auto x) -> auto { return x * 2; } }
                var health = 12;
                class Q { int q = 1; }
            )");
            check_eq(eng->execute(
                "var found = 0; for (auto f : reflect::functions()) { if (f == \"greet\" || f == \"tools::twice\") { found = found + 1; } } found"
            ).as_int(), (int64_t)2);
            // Constructors are the classes() surface, not functions()
            check_eq(eng->execute(
                "var q = 0; for (auto f : reflect::functions()) { if (f == \"Q\") { q = q + 1; } } q"
            ).as_int(), (int64_t)0);
            check_eq(eng->execute(
                "var g = 0; for (auto n : reflect::globals()) { if (n == \"health\") { g = g + 1; } if (n == \"greet\") { g = g + 100; } } g"
            ).as_int(), (int64_t)1);
            eng->execute("var fa = reflect::function_arguments(\"greet\");");
            check_eq(eng->execute("fa.size()").as_int(), (int64_t)1);
            check_eq(eng->execute("fa[0][0][\"name\"]").as<std::string>(), "who");
            check_eq(eng->execute("fa[0][0][\"type\"]").as<std::string>(), "string");
            check_eq(eng->execute("fa[0][1][\"default\"]").as<bool>(), true);
            check_eq(eng->execute("reflect::function_arguments(\"tools::twice\")[0][0][\"name\"]").as<std::string>(), "x");
            // Host registrations are type-erased: empty overload list, never an error
            check_eq(eng->execute("reflect::function_arguments(\"print\").size()").as_int(), (int64_t)0);
            check_throws([&]() { eng->execute("reflect::function_arguments(\"nope\");"); },
                "unknown function raises the family error");
        });

        test("sandbox_engine_reflects_nothing_it_did_not_register", [this]() {
            // The allowlist contract: reflection is a capability over THIS engine's
            // registrations. A second engine in the same process knows nothing about
            // the first engine's classes.
            auto eng_a = make_reflective_engine();
            eng_a->execute("class OnlyInA { int x = 1; }");
            auto eng_b = make_reflective_engine();
            check_throws([&]() { eng_b->execute("reflect::bases(\"OnlyInA\");"); },
                "engine B cannot name engine A's class");
        });
    }
};

} // namespace jai::foundry::tests

using reflection_tests = jai::foundry::tests::reflection_tests;
FOUNDRY_REGISTER(reflection_tests)
