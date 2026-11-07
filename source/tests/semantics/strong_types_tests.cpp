#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/core/bound_array.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <array>
#include <functional>
#include <string>
#include <variant>
#include <vector>
#include "../case_table.hpp"

namespace jai::foundry::tests {

using namespace std::string_literals;

namespace {

using row_value = std::variant<int, double, bool, std::string>;

struct value_row {
    const char* code;
    row_value expected;
    const char* prelude = nullptr;
};

struct rejected_row {
    const char* code;
    const char* text = nullptr;   // a substring the error must hold; null accepts any error
};

// Each row runs its prelude and then its code on a fresh engine; the result is read with as<T>() of the expected type.
void check_values(const std::vector<value_row>& a_rows, bool a_with_stdlib = false) {
    check_each(a_rows, [&](const value_row& a_row) {
        auto eng = make_engine();
        if (a_with_stdlib) { jai::stdlib::register_all(*eng); }
        if (a_row.prelude) { eng->execute(a_row.prelude); }
        const auto result = eng->execute(a_row.code);
        std::visit([&](const auto& a_value) {
            check_eq(a_value, result.as<std::decay_t<decltype(a_value)>>());
        }, a_row.expected);
    });
}

void check_rejected(const std::vector<rejected_row>& a_rows) {
    check_each(a_rows, [](const rejected_row& a_row) {
        auto eng = make_engine();
        bool caught = false;
        std::string message;
        try {
            eng->execute(a_row.code);
        } catch (const std::exception& error) {
            caught = true;
            message = error.what();
        }
        check_true(caught, "expected an error");
        if (a_row.text) { check_true(message.find(a_row.text) != std::string::npos, "error was: " + message); }
    });
}

} // namespace

class strong_types_tests : public suite {
public:
    strong_types_tests() : suite("Strong Types") {}

    void forge_tests() override {
        test("callable_slot_resets_to_null_and_takes_a_callable_again", [this]() {
            // The typed-slot table's function row (the checker's callable-or-null rule holds at
            // runtime too): a slot locked to a callable takes null, then a callable again; a
            // call through the null raises a catchable error; a non-callable still rejects
            for (bool vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                const std::string label = vm ? "vm" : "interp";
                auto run = [&](const char* src) {
                    try { return std::to_string(e->execute(src).as_int()); } catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
                };
                check_eq(std::string("7"), run("function f = []() { return 1; }; f = null; f = []() { return 7; }; f();"), label + " local reset");
                check_eq(std::string("3"), run("class C { function cb = []() { return 1; }; } C c = C(); c.cb = null; c.cb = []() { return 3; }; c.cb();"), label + " field reset");
                check_eq(std::string("1"), run("function g = []() { return 1; }; g = null; int caught = 0; try { g(); } catch (err) { caught = 1; } caught;"), label + " null call is catchable");
                check_true(run("function h = []() { return 1; }; h = null; h = 42; 0;").rfind("throw:", 0) == 0, label + " a non-callable still rejects after the reset");
            }
        });

        test("var_held_values_cross_the_element_boundary_as_their_payload", [this]() {
            // var is the runtime hatch: the element boundary (a declaration literal, push, a
            // subscript store, a map value store, the reference route) admits a var-held instance
            // of the element class or a subclass, a new element carries its class, a store over an
            // existing element assigns into it, the element keeping its object and taking the source's class, and a rejection names
            // the payload's type, never the var tag; byte-identical on
            // both backends
            const char* src = R"(
                class Animal { int id = 1; }
                class Dog : Animal { int extra = 2; }
                class Box { int b = 0; }
                function put(var& x, var val) { x = val; }
                var out = [];
                function cap(string label, function body) { try { out.push(label + ":" + to_string(body())); } catch (err) { out.push(label + "!" + err); } }
                var va = Animal(); var vd = Dog(); var vb = Box(); var vs = "s";
                cap("decl", function() -> string { array<Animal> a = [va, vd]; return type_of(a[0]) + "," + type_of(a[1]) + "," + to_string(a[1].id); });
                cap("push", function() -> string { array<Animal> a = []; a.push(va); a.push(vd); return type_of(a[0]) + "," + type_of(a[1]) + "," + to_string(a.size()); });
                cap("store", function() -> string { array<Animal> a = [Animal()]; a[0] = vd; return type_of(a[0]) + "," + to_string(a[0].id); });
                cap("ref", function() -> string { array<Animal> a = [Animal()]; put(a[0], vd); return type_of(a[0]) + "," + to_string(a[0].id); });
                cap("map", function() -> string { map<string, Animal> m = {}; m["k"] = va; return type_of(m["k"]); });
                cap("copy", function() -> int { array<Animal> a = []; a.push(va); a[0].id = 9; return va.id; });
                cap("box-store", function() -> int { array<Animal> a = [Animal()]; a[0] = vb; return 0; });
                cap("box-push", function() -> int { array<Animal> a = []; a.push(vb); return 0; });
                cap("box-ref", function() -> int { array<Animal> a = [Animal()]; put(a[0], vb); return 0; });
                cap("box-decl", function() -> int { array<Animal> a = [vb]; return 0; });
                cap("box-int", function() -> int { array<int> a = [1]; a[0] = vb; return 0; });
                cap("str-push", function() -> int { array<int> a = []; a.push(vs); return 0; });
                cap("str-store", function() -> int { array<int> a = [1]; a[0] = vs; return 0; });
                cap("str-decl", function() -> int { array<int> a = [vs]; return 0; });
                cap("str-map", function() -> int { map<string, int> m = {}; m["k"] = vs; return 0; });
                out;
            )";
            std::vector<std::string> seen[2];
            for (bool vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                stdlib::register_all(*e);
                const auto rows = e->execute(src).as_array();
                for (const auto& r : rows) { seen[vm ? 1 : 0].push_back(r.as_string()); }
            }
            check_eq(seen[0], seen[1], "both backends land and reject identically");
            const std::vector<std::string> expected = {
                "decl:Animal,Dog,1", "push:Animal,Dog,2", "store:Dog,1", "ref:Dog,1", "map:Animal", "copy:1",
                "box-store!Cannot assign 'Box' to element of type 'Animal'",
                "box-push!Cannot push 'Box' to array<Animal>",
                "box-ref!Cannot assign 'Box' to element of type 'Animal'",
                "box-decl!Cannot store Box in array<Animal>",
                "box-int!Cannot assign 'Box' to element of type 'int'",
                "str-push!Cannot push 'string' to array<int>",
                "str-store!Cannot assign 'string' to element of type 'int'",
                "str-decl!Cannot store string in array<int>",
                "str-map!Cannot assign 'string' to element of type 'int'",
            };
            check_eq(expected.size(), seen[0].size(), "row count");
            for (size_t i = 0; i < expected.size() && i < seen[0].size(); ++i) { check_eq(expected[i], seen[0][i], "row " + std::to_string(i)); }
        });

        test("handle_typed_elements_take_null_and_live_instances", [this]() {
			// array<shared_ptr<T>> / array<weak_ptr<T>> elements and
			// map<K, shared_ptr<T>> values take null at every site (a declaration literal, push,
			// a subscript store, a map store, the reference route, a var-held null); a shared_ptr<T>
			// element takes a live instance of T or a subclass the way a shared_ptr<T> variable does
			// (the slot owns the handle) and checks a handle's class; the texts spell
			// shared_ptr<T> in full (array<shared_ptr<Foo>> and array<Foo> were one id); by-value
			// class and primitive elements stay non-nullable with their texts; both backends agree
			const char* src = R"(
				[[warn_mode]] // preserve runtime boundary diagnostics for intentional mismatches
				class Foo { int n = 1; }
				class Dog : Foo { int extra = 2; }
				class Bar { int b = 0; }
				function put(var& x, var val) { x = val; }
				var out = [];
				function cap(string label, function body) { try { out.push(label + ":" + to_string(body())); } catch (err) { out.push(label + "!" + err); } }
				cap("decl", function() -> string { shared_ptr<Foo> h = Foo(); array<shared_ptr<Foo>> a = [null, h]; return to_string(a[0] == null) + "," + to_string(a[1] == null) + "," + to_string(a[1].n); });
				cap("push", function() -> string { array<shared_ptr<Foo>> a = []; shared_ptr<Foo> h = Foo(); a.push(null); a.push(h); a.push(null); return to_string(a.size()) + "," + to_string(a[0] == null) + "," + to_string(a[1].n); });
				cap("store", function() -> string { array<shared_ptr<Foo>> a = [null]; shared_ptr<Foo> h = Foo(); a[0] = h; int n = a[0].n; a[0] = null; return to_string(n) + "," + to_string(a[0] == null); });
				cap("ref", function() -> string { array<shared_ptr<Foo>> a = [null]; shared_ptr<Foo> h = Foo(); put(a[0], h); int n = a[0].n; put(a[0], null); return to_string(n) + "," + to_string(a[0] == null); });
				cap("var-null", function() -> string { array<shared_ptr<Foo>> a = []; var v = null; a.push(v); a[0] = v; return to_string(a.size()) + "," + to_string(a[0] == null); });
				cap("map-decl", function() -> string { map<string, shared_ptr<Foo>> m = {"k": null}; return to_string(m["k"] == null); });
				cap("map-store", function() -> string { map<string, shared_ptr<Foo>> m = {}; m["k"] = null; shared_ptr<Foo> h = Foo(); m["k"] = h; int n = m["k"].n; m["k"] = null; return to_string(n) + "," + to_string(m["k"] == null); });
				cap("weak", function() -> string { array<weak_ptr<Foo>> a = [null]; a.push(null); a[0] = null; return to_string(a.size()) + "," + to_string(a[0] == null); });
				cap("weak-push-shared", function() -> string { array<weak_ptr<Foo>> a = []; shared_ptr<Foo> h = Foo(); shared_ptr<Dog> d = Dog(); a.push(h); a.push(d); return to_string(a.size()) + "," + to_string(a[0].expired()) + "," + to_string(a[1].expired()); });
				cap("weak-store-shared", function() -> string { array<weak_ptr<Foo>> a = [null]; shared_ptr<Foo> h = Foo(); a[0] = h; bool live = !a[0].expired(); h = null; return to_string(live) + "," + to_string(a[0].expired()); });
				cap("weak-map-shared", function() -> string { map<string, weak_ptr<Foo>> m = {}; shared_ptr<Foo> h = Foo(); m["k"] = h; bool live = !m["k"].expired(); h = null; return to_string(live) + "," + to_string(m["k"].expired()); });
				cap("weak-literal-shared", function() -> string { shared_ptr<Foo> h = Foo(); array<weak_ptr<Foo>> a = [h, null]; return to_string(a[0].expired()) + "," + to_string(a[1] == null); });
				cap("weak-null-handle", function() -> string { shared_ptr<Foo> n = null; array<weak_ptr<Foo>> a = []; a.push(n); a.push(null); return to_string(a.size()) + "," + to_string(a[0] == null); });
				cap("weak-wrong-class", function() -> int { array<weak_ptr<Foo>> a = []; shared_ptr<Bar> b = Bar(); a.push(b); return 0; });
				cap("iterate", function() -> int { array<shared_ptr<Foo>> a = [null]; shared_ptr<Foo> h = Foo(); a.push(h); a.push(null); int c = 0; for (auto x : a) { if (x != null) { c += x.n; } } return c; });
				cap("raw-push", function() -> string { array<shared_ptr<Foo>> a = []; a.push(Foo()); return type_of(a[0]) + "," + to_string(a[0].n) + "," + to_string(a[0] == null); });
				cap("raw-decl", function() -> string { array<shared_ptr<Foo>> a = [Foo(), null]; return to_string(a.size()) + "," + type_of(a[0]) + "," + to_string(a[1] == null); });
				cap("raw-store", function() -> string { array<shared_ptr<Foo>> a = [null]; a[0] = Foo(); a[0].n = 4; return to_string(a[0].n); });
				cap("raw-map", function() -> string { map<string, shared_ptr<Foo>> m = {}; m["k"] = Foo(); return to_string(m["k"].n); });
				cap("raw-shares-like-a-handle-slot", function() -> string { Foo f = Foo(); shared_ptr<Foo> h = f; h.n = 5; Foo g = Foo(); array<shared_ptr<Foo>> a = []; a.push(g); a[0].n = 5; return to_string((f.n == 5) == (g.n == 5)); });
				cap("raw-subclass", function() -> string { array<shared_ptr<Foo>> a = []; a.push(Dog()); shared_ptr<Dog> d = Dog(); a.push(d); return to_string(a.size()) + "," + type_of(a[0]) + "," + type_of(a[1]); });
				cap("handle-array-widens", function() -> string { array<shared_ptr<Dog>> ds = [Dog()]; array<shared_ptr<Foo>> fs = ds; return to_string(fs.size()) + "," + type_of(fs[0]); });
				cap("wrong-class-raw", function() -> int { array<shared_ptr<Foo>> a = []; a.push(Bar()); return 0; });
				cap("wrong-class-handle", function() -> int { array<shared_ptr<Foo>> a = []; shared_ptr<Bar> b = Bar(); a.push(b); return 0; });
				cap("wrong-class-store", function() -> int { array<shared_ptr<Foo>> a = [null]; a[0] = Bar(); return 0; });
				cap("int-into-handle-array", function() -> int { array<shared_ptr<Foo>> a = [5]; return 0; });
				cap("value-decl", function() -> int { array<Foo> a = [null]; return 0; });
				cap("value-push", function() -> int { array<Foo> a = []; a.push(null); return 0; });
				cap("value-store", function() -> int { array<Foo> a = [Foo()]; a[0] = null; return 0; });
				cap("value-map", function() -> int { map<string, Foo> m = {}; m["k"] = null; return 0; });
				cap("int-push", function() -> int { array<int> a = []; a.push(null); return 0; });
				cap("string-decl", function() -> int { array<string> a = [null]; return 0; });
				out;
			)";
			std::vector<std::string> seen[2];
			for (bool vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e);
				const auto rows = e->execute(src).as_array();
				for (const auto& r : rows) { seen[vm ? 1 : 0].push_back(r.as_string()); }
			}
			check_eq(seen[0], seen[1], "both backends land and reject identically");
			const std::vector<std::string> expected = {
				"decl:true,false,1", "push:3,true,1", "store:1,true", "ref:1,true", "var-null:1,true",
				"map-decl:true", "map-store:1,true", "weak:2,true", "weak-push-shared:2,false,false", "weak-store-shared:true,true",
				"weak-map-shared:true,true", "weak-literal-shared:false,true", "weak-null-handle:2,true",
				"weak-wrong-class!Cannot push 'shared_ptr<Bar>' to array<weak_ptr<Foo>>", "iterate:1",
				"raw-push:Foo,1,false", "raw-decl:2,Foo,true", "raw-store:4", "raw-map:1", "raw-shares-like-a-handle-slot:true",
				"raw-subclass:2,Dog,Dog", "handle-array-widens:1,Dog",
				"wrong-class-raw!Cannot push 'Bar' to array<shared_ptr<Foo>>",
				"wrong-class-handle!Cannot push 'shared_ptr<Bar>' to array<shared_ptr<Foo>>",
				"wrong-class-store!Cannot assign 'Bar' to element of type 'shared_ptr<Foo>'",
				"int-into-handle-array!Cannot store int in array<shared_ptr<Foo>>",
				"value-decl!Cannot store null in array<Foo>",
				"value-push!Cannot push 'null' to array<Foo>",
				"value-store!Cannot assign 'null' to element of type 'Foo'",
				"value-map!Cannot assign 'null' to element of type 'Foo'",
				"int-push!Cannot push 'null' to array<int>",
				"string-decl!Cannot store null in array<string>",
			};
			check_eq(expected.size(), seen[0].size(), "row count");
			for (size_t i = 0; i < expected.size() && i < seen[0].size(); ++i) { check_eq(expected[i], seen[0][i], "row " + std::to_string(i)); }
		});

        test("assignment_expressions_yield_the_stored_value", [this]() {
            // The value of an assignment expression is the destination's
            // stored value on every spelling (`auto r = (c.n = 3.9)` is int 3): typed local, bare and
            // explicit field, static, accessor (read back through _get_), array element, map value,
            // nested element, compound forms; postfix keeps the old stored value; a var element keeps
            // what it stored; a heavy value comes back as a copy. Both backends agree row for row
            const char* src = R"(
                class C { int n = 0; int _get_x() { return n; } void _set_x(int v) { n = v; } static int s = 0;
                          string test_plain() { auto r = (n = 3.9); return type_of(r) + ":" + to_string(r) + "|" + type_of(n) + ":" + to_string(n); }
                          string test_compound() { n = 1; auto r = (n += 2.9); return type_of(r) + ":" + to_string(r) + "|" + type_of(n) + ":" + to_string(n); } }
                class D { int n = 0; int _get_x() { return n; } void _set_x(int v) { n = v * 2; } }
                class S { string t = ""; }
                class H { array<int> a = []; }
                class T { static array<int> a = []; }
                class G { string s = "a"; }
                class W { int seen = 0; void _set_virt(int v) { seen = seen + v; } }
                class T2 { static int s = 0; static string m() { float v = 3.9; auto r = (s = v); return type_of(r) + ":" + to_string(r) + "|" + type_of(s) + ":" + to_string(s); } }
                var out = [];
                function cap(string label, function body) { try { out.push(label + ":" + to_string(body())); } catch (err) { out.push(label + "!" + err); } }
                function show(var r, var dst) -> string { return type_of(r) + ":" + to_string(r) + "|" + type_of(dst) + ":" + to_string(dst); }
                cap("local_plain", function() -> string { int n = 0; auto r = (n = 3.9); return show(r, n); });
                cap("local_compound", function() -> string { int n = 1; auto r = (n += 2.9); return show(r, n); });
                cap("bare_field_plain", function() -> string { C c = C(); return c.test_plain(); });
                cap("bare_field_compound", function() -> string { C c = C(); return c.test_compound(); });
                cap("field_plain", function() -> string { C c = C(); auto r = (c.n = 3.9); return show(r, c.n); });
                cap("field_compound", function() -> string { C c = C(); c.n = 1; auto r = (c.n += 2.9); return show(r, c.n); });
                cap("static_plain", function() -> string { auto r = (C::s = 3.9); return show(r, C::s); });
                cap("static_compound", function() -> string { C::s = 1; auto r = (C::s += 2.9); return show(r, C::s); });
                cap("accessor_plain", function() -> string { C c = C(); auto r = (c.x = 3.9); return show(r, c.x); });
                cap("accessor_compound", function() -> string { C c = C(); c.n = 1; auto r = (c.x += 2.9); return show(r, c.x); });
                cap("array_plain", function() -> string { array<int> a = [0]; auto r = (a[0] = 3.9); return show(r, a[0]); });
                cap("array_compound", function() -> string { array<int> a = [1]; auto r = (a[0] += 2.9); return show(r, a[0]); });
                cap("map_plain", function() -> string { map<string, int> m = {"k": 0}; auto r = (m["k"] = 3.9); return show(r, m["k"]); });
                cap("map_compound", function() -> string { map<string, int> m = {"k": 1}; auto r = (m["k"] += 2.9); return show(r, m["k"]); });
                cap("nested_plain", function() -> string { array<array<int>> g = [[0]]; auto r = (g[0][0] = 3.9); return show(r, g[0][0]); });
                cap("var_array_plain", function() -> string { array<var> a = [0]; auto r = (a[0] = 3.9); return show(r, a[0]); });
                cap("field_bool", function() -> string { C c = C(); auto r = (c.n = true); return show(r, c.n); });
                cap("string_field", function() -> string { S s = S(); utf8_string u = "42"; auto r = (s.t = u); return show(r, s.t); });
                cap("postfix_field", function() -> string { C c = C(); c.n = 7; auto r = (c.n++); return show(r, c.n); });
                cap("postfix_element", function() -> string { array<int> a = [7]; auto r = (a[0]++); return show(r, a[0]); });
                cap("prefix_element", function() -> string { array<int> a = [7]; auto r = (++a[0]); return show(r, a[0]); });
                cap("chain", function() -> string { array<int> a = [0]; var k = (a[0] = 3.9); return show(k, a[0]); });
                cap("accessor_setter_transforms", function() -> string { D d = D(); auto r = (d.x = 3); return show(r, d.x); });
                cap("heavy_copy", function() -> string { array<array<int>> g = [[1]]; auto r = (g[0] = [5, 6]); r.push(7); return to_string(r.size()) + "," + to_string(g[0].size()); });
                cap("member_heavy", function() -> string { H h = H(); auto r = (h.a = [5, 6]); r.push(7); return to_string(r.size()) + "," + to_string(h.a.size()); });
                cap("map_heavy", function() -> string { map<string, array<int>> m = {}; auto r = (m["k"] = [5, 6]); r.push(7); return to_string(r.size()) + "," + to_string(m["k"].size()); });
                cap("static_heavy", function() -> string { auto r = (T::a = [5, 6]); r.push(7); return to_string(r.size()) + "," + to_string(T::a.size()); });
                cap("compound_member_heavy", function() -> string { G g = G(); auto r = (g.s += "b"); r += "c"; return r + "," + g.s; });
                cap("bare_static_from_lvalue", function() -> string { return T2::m(); });
                cap("setter_only_member", function() -> string { W w = W(); auto r = (w.virt = 2.5); return type_of(r) + ":" + to_string(r) + "|" + to_string(w.seen); });
                cap("statement_position", function() -> string { C c = C(); c.n = 3.9; array<int> a = [0]; a[0] = 2.5; map<string, int> m = {}; m["k"] = 1.5; c.x = 4.9; return to_string(c.n) + to_string(a[0]) + to_string(m["k"]); });
                out;
            )";
            std::vector<std::string> seen[2];
            for (bool vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
                stdlib::register_all(*e);
                const auto rows = e->execute(src).as_array();
                for (const auto& r : rows) { seen[vm ? 1 : 0].push_back(r.as_string()); }
            }
            check_eq(seen[0], seen[1], "both backends yield identically");
            const std::string stored3 = "int:3|int:3";
            const std::vector<std::string> expected = {
                "local_plain:" + stored3, "local_compound:" + stored3, "bare_field_plain:" + stored3, "bare_field_compound:" + stored3,
                "field_plain:" + stored3, "field_compound:" + stored3, "static_plain:" + stored3, "static_compound:" + stored3,
                "accessor_plain:" + stored3, "accessor_compound:" + stored3, "array_plain:" + stored3, "array_compound:" + stored3,
                "map_plain:" + stored3, "map_compound:" + stored3, "nested_plain:" + stored3,
                "var_array_plain:float:3.9|float:3.9", "field_bool:int:1|int:1", "string_field:string:42|string:42",
                "postfix_field:int:7|int:8", "postfix_element:int:7|int:8", "prefix_element:int:8|int:8", "chain:" + stored3,
                "accessor_setter_transforms:int:6|int:6", "heavy_copy:3,2",
                "member_heavy:3,2", "map_heavy:3,2", "static_heavy:3,2", "compound_member_heavy:abc,ab", "bare_static_from_lvalue:int:3|int:3", "setter_only_member:float:2.5|2", "statement_position:421",
            };
            check_eq(expected.size(), seen[0].size(), "row count");
            for (size_t i = 0; i < expected.size() && i < seen[0].size(); ++i) { check_eq(expected[i], seen[0][i], "row " + std::to_string(i)); }
        });

        test("bool_and_int_elements_convert", [this]() {
			// Invalid conversion statements use warn_mode so their runtime texts are still exercised.
			// bool and int convert both ways at the element boundary (a
			// declaration literal, push, a subscript store, a map value, a whole typed array), like
			// the scalar slots; int -> string stays explicit (to_string / cast<string>), float and
			// string rows keep their texts, map keys stay strict; both backends agree
			const char* src = R"(
				var out = [];
				function cap(string label, function body) { try { out.push(label + ":" + to_string(body())); } catch (err) { out.push(label + "!" + err); } }
				cap("int-decl", function() -> string { array<int> a = [true, false, 7]; return to_string(a[0]) + to_string(a[1]) + to_string(a[2]) + type_of(a[0]); });
				cap("int-push", function() -> string { array<int> a = []; a.push(true); a.push(false); return to_string(a[0]) + to_string(a[1]); });
				cap("int-store", function() -> string { array<int> a = [5]; a[0] = true; string r = to_string(a[0]); a[0] = false; return r + to_string(a[0]); });
				cap("bool-decl", function() -> string { array<bool> b = [1, 0, 2, -3]; return to_string(b[0]) + "," + to_string(b[1]) + "," + to_string(b[2]) + "," + to_string(b[3]) + type_of(b[0]); });
				cap("bool-push", function() -> string { array<bool> b = []; b.push(0); b.push(9); return to_string(b[0]) + "," + to_string(b[1]); });
				cap("bool-store", function() -> string { array<bool> b = [false]; b[0] = 1; string r = to_string(b[0]); b[0] = 0; return r + "," + to_string(b[0]); });
				cap("map-int", function() -> string { map<string, int> m = {"k": true}; m["j"] = false; return to_string(m["k"]) + to_string(m["j"]); });
				cap("map-bool", function() -> string { map<string, bool> m = {"k": 1}; m["j"] = 0; return to_string(m["k"]) + "," + to_string(m["j"]); });
				cap("whole-bool-to-int", function() -> string { array<bool> b = [true, false]; array<int> a = b; return to_string(a[0]) + to_string(a[1]) + type_of(a[0]); });
				cap("whole-int-to-bool", function() -> string { array<int> a = [0, 3]; array<bool> b = a; return to_string(b[0]) + "," + to_string(b[1]); });
				cap("var-held", function() -> string { var t = true; array<int> a = [t]; a.push(t); a[0] = t; return to_string(a.size()) + to_string(a[0]); });
				cap("int-to-string", function() -> int { [[warn_mode]] array<string> s = [1]; return 0; });
				cap("push-int-to-string", function() -> int { array<string> s = []; [[warn_mode]] s.push(1); return 0; });
				cap("string-to-int", function() -> int { [[warn_mode]] array<int> a = ["1"]; return 0; });
				cap("explicit", function() -> string { array<string> s = [to_string(1), cast<string>(15)]; return s[0] + s[1]; });
				cap("float-to-bool", function() -> int { [[warn_mode]] array<bool> b = [1.5]; return 0; });
				cap("bool-to-float", function() -> int { [[warn_mode]] array<float> f = [true]; return 0; });
				cap("bool-key", function() -> int { [[warn_mode]] map<int, int> m = {true: 1}; return 0; });
				out;
			)";
			std::vector<std::string> seen[2];
			for (bool vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				stdlib::register_all(*e);
				const auto rows = e->execute(src).as_array();
				for (const auto& r : rows) { seen[vm ? 1 : 0].push_back(r.as_string()); }
			}
			check_eq(seen[0], seen[1], "both backends land and reject identically");
			const std::vector<std::string> expected = {
				"int-decl:107int", "int-push:10", "int-store:10",
				"bool-decl:true,false,true,truebool", "bool-push:false,true", "bool-store:true,false",
				"map-int:10", "map-bool:true,false", "whole-bool-to-int:10int", "whole-int-to-bool:false,true", "var-held:21",
				"int-to-string!Cannot store int in array<string>",
				"push-int-to-string!Cannot push 'int' to array<string>",
				"string-to-int!Cannot store string in array<int>",
				"explicit:115",
				"float-to-bool!Cannot store float in array<bool>",
				"bool-to-float!Cannot store bool in array<float>",
				"bool-key!Cannot store bool in map<int,int>",
			};
			check_eq(expected.size(), seen[0].size(), "row count");
			for (size_t i = 0; i < expected.size() && i < seen[0].size(); ++i) { check_eq(expected[i], seen[0][i], "row " + std::to_string(i)); }
		});

        test("auto_locks_to_its_first_values_type", [this]() {
            check_values({
                {"auto x = 5; x = 10; x", 10},
                {"auto x; x = 5; x = 10; x", 10},
                {"function get_number() -> int { return 42; } auto x = get_number(); x = 100; x", 100},
                {"auto x = 5 + 3; auto y = 5.0 + 3; auto z = x + 2; z", 10},
                {"auto b = 5 > 3; b", true},
                {"auto x = true ? 42 : 0; x = x + 8; x", 50},
                {"auto x = 5; { auto x = \"hello\"; x = \"world\"; } x = 10; x", 10},
                {"auto x = 10; x += 5; x -= 3; x *= 2; x", 24},
                {"auto x = 10.0; x += 2.5; x", 12.5},
                {"auto s = \"hello\"; s += \" \"; s += \"world\"; s", "hello world"s},
                {"auto x = 10; auto fn = [=]() { return x * 2; }; fn()", 20},
            });
            check_values({
                {"auto a = 5; auto b = \"hello\"; auto c = 3.14; auto d = true; a = 10; b = \"world\"; c = 2.0; d = false; "
                 "to_string(a) + \" \" + b + \" \" + to_string(c) + \" \" + to_string(d)", "10 world 2.0 false"s},
            }, true);
            check_rejected({{"auto x; x = 5; x = \"hello\";"}});
        });

        test("auto_rejects_incompatible_type", [this]() {
            const char* src = R"(
                auto x = 5;
                x = "hello";  // ERROR: string to int
            )";
            // strict (default): the locked auto is a typed local, the store is a load error
            auto eng = make_engine();
            std::string text;
            try { eng->execute(src); } catch (const static_check_error& e) { text = e.what(); }
            check_true(text.find("cannot assign 'string' to 'x' declared 'int'") != std::string::npos,
                "strict rejects the store before anything runs (got: " + text + ")");
            // a warn region: the runtime's own rejection
            auto warn = make_engine();
            bool caught = false;
            try {
                warn->execute(std::string("[[warn_mode]]; ") + src);
            } catch (const std::exception& e) {
                caught = true;
                std::string msg = e.what();
                check_true(msg.find("Cannot assign") != std::string::npos, msg);
            }
            check_eq(caught, true, "Should reject string assignment to int variable");
            // strict admits a var-sourced store; the runtime still rejects it
            auto dynamic = make_engine();
            caught = false;
            try {
                dynamic->execute("auto x = 5; var s = \"hello\"; x = s;");
            } catch (const static_check_error&) {
            } catch (const std::exception& e) {
                caught = true;
                check_true(std::string(e.what()).find("Cannot assign string to 'x' (declared int)") != std::string::npos, e.what());
            }
            check_eq(caught, true, "var-sourced string into a locked int raises at runtime");
        });

        test("extern_raises_until_defined_and_inline_creates_on_both_backends", [this]() {
            auto eng = make_engine();
            // externs declare nothing: a use before the host's add_global or a declaration raises
            auto r = eng->execute(R"(
                extern int hp;
                extern loose;
                extern function host_fn;
                class Game { int v = 3; }
                extern shared_ptr<Game> G;
                function total() -> int { return hp + G.v; }
                var caught = "";
                try { total(); } catch (e) { caught = e; }
                caught
            )");
            check_eq(std::string("'hp' is declared extern but never defined (an 'inline' declaration creates it)"), r.as<std::string>());
            eng->add_global("hp", script_value((int64_t)4, eng.get()));
            eng->execute("var G = Game();");
            check_eq((int64_t)7, eng->execute("total()").as_int());
            // an inline handle starts null; a store fills the shared cell
            auto inl = make_engine();
            check_eq(std::string("Cannot access 'v' on null ('G' is null)"), inl->execute(R"(
                class Game { int v = 3; }
                inline shared_ptr<Game> G;
                function read_v() -> int { return G.v; }
                var caught = "";
                try { read_v(); } catch (e) { caught = e; }
                caught
            )").as<std::string>());
            inl->execute("inline shared_ptr<Game> G; G = cast<shared_ptr<Game>>(Game());");
            check_eq((int64_t)3, inl->execute("read_v()").as_int());
            // a typed extern is a checked receiver: the store below is rejected before anything runs
            auto strict = make_engine();
            std::string text;
            try {
                strict->execute("class Game { int v = 3; }\nextern Game H;\nfunction poke() { H.v = \"s\"; }");
            } catch (const static_check_error& e) {
                text = e.what();
            }
            check_true(text.find("cannot assign 'string' to field 'v'") != std::string::npos,
                "typed extern flows into member checks (got: " + text + ")");
        });

        test("warn_mode_regions_keep_the_runtime_conversion_errors", [this]() {
            // unit-level, statement-level and block-first: strict loads each, the runtime's own
            // rejection is byte-identical to the warn engine's (both backends)
            for (const char* src : {"[[warn_mode]];\nint x = \"s\";\nx",
                                    "int a = 1;\n[[warn_mode]] int x = \"s\";\nx",
                                    "int a = 1;\n{ [[warn_mode]] a = 2; int x = \"s\"; }\na"}) {
                auto eng = make_engine();
                std::string text;
                try {
                    eng->execute(src);
                } catch (const static_check_error& e) {
                    text = std::string("STATIC ") + e.what();
                } catch (const std::exception& e) {
                    text = e.what();
                }
                check_true(text.find("Cannot initialize") == 0, "runtime rejects the store (got: " + text + ")");
                // the reference: the same program with one region over the whole unit
                std::string whole_unit = src;
                for (const char* spelling : {"[[warn_mode]];", "[[warn_mode]]"}) {
                    for (size_t at = whole_unit.find(spelling); at != std::string::npos; at = whole_unit.find(spelling)) {
                        whole_unit.erase(at, std::string(spelling).size());
                    }
                }
                auto warn = make_engine();
                std::string warn_text;
                try { warn->execute("[[warn_mode]]; " + whole_unit); } catch (const std::exception& e) { warn_text = e.what(); }
                check_eq(text, warn_text);
            }
        });

        test("var_takes_any_value_and_keeps_its_methods", [this]() {
            check_values({
                {"var x = 5; x = \"hello\"; x = 3.14; x = true; x", true},
                {"var x; x = \"first\"; x = 42; x = [1, 2, 3]; x.size()", 3},
                {"var arr = [1, 2, 3, 4, 5]; arr.index_of(3)", 2},
                {"var m = {\"a\": 1, \"b\": 2}; m.has(\"a\")", true},
                {"var x = [1, 2, 3]; x = {\"key\": \"value\"}; x.has(\"key\")", true},
                {"var x = [[1, 2], [3, 4]]; x = {\"key\": [5, 6, 7]}; x[\"key\"][2]", 7},
                {"var arr = []; arr.push(1); arr.push(\"two\"); arr.push(3.0); arr.push(true); arr.size()", 4},
            });
            check_values({
                {"function stringify(var x) -> string { return to_string(x); } stringify(42) + \" \" + stringify(\"hello\") + \" \" + stringify(true)",
                 "42 hello true"s},
                {"class Container { var data = null; function set(var val) { data = val; } function get() { return data; } } "
                 "auto c = Container(); c.set(42); auto v1 = c.get(); c.set(\"hello\"); auto v2 = c.get(); to_string(v1) + \" \" + v2", "42 hello"s},
            }, true);
        });

        test("scalar_slots_convert_between_numbers_and_bools", [this]() {
            check_values({
                {"float x = 3.14; x = 10; x", 10.0},
                {"bool b = true; b = 0; b", false},
                {"int x = 0; x = true; auto y = x; x = false; y + x", 1},
            });
        });

        test("auto_containers_nest_and_keep_their_methods", [this]() {
            check_values({
                {"auto arr = [[1, 2], [3, 4], [5, 6]]; arr[1][0]", 3},
                {"auto arr = [{\"a\": 1}, {\"b\": 2}]; arr[0][\"a\"]", 1},
                {"auto m = {\"nums\": [10, 20, 30]}; m[\"nums\"][1]", 20},
                {"auto arr = [1, 2, 3, 4, 5, 6]; auto evens = array::filter(arr, [](x) { return x % 2 == 0; }); evens.size()", 3},
                {"auto arr = [5, 2, 8, 1, 9]; arr.sort(); arr[0]", 1},
                {"auto m = {\"a\": 1, \"b\": 2, \"c\": 3}; m.keys().size()", 3},
                {"auto numbers = [1, 2, 3, 4, 5]; auto doubled = []; for (auto n : numbers) { doubled.push(n * 2); } doubled[2]", 6},
            });
        });

        test("object_and_handle_slots_take_null_and_keep_their_class", [this]() {
            const char* test_class = "class TestClass { int value = 42; }";
            check_values({
                {"auto obj = shared_ptr<TestClass>(); obj = null; obj == null", true, test_class},
                {"auto shared = shared_ptr<TestClass>(); auto weak = weak_ptr<TestClass>(shared); shared = null; weak.expired()", true, test_class},
                {"auto obj = TestObj(); obj = null; obj = TestObj(); obj.x", 1, "class TestObj { int x = 1; }"},
            });
        });

        test("function_with_typed_params", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                function add(int a, int b) -> int {
                    return a + b;
                }
                add(3, 4)
            )");
            check_eq(result.as<int>(), 7);
        });

        test("function_param_type_conversion", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                function double_it(float x) -> float {
                    return x * 2;
                }
                double_it(5)  // int converts to float
            )");
            check_eq(result.as<double>(), 10.0);
        });

        test("loop_counters_and_range_elements_take_their_declared_kind", [this]() {
            check_values({
                {"auto sum = 0; for (int i = 0; i < 5; i = i + 1) { sum = sum + i; } sum", 10},
                {"auto sum = 0; for (auto i = 0; i < 5; i = i + 1) { sum = sum + i; } sum", 10},
                {"auto arr = [1, 2, 3, 4, 5]; auto sum = 0; for (auto x : arr) { sum = sum + x; } sum", 15},
                {"var arr = [1, 2, 3]; var sum = 0; for (var x : arr) { sum = sum + x; } sum", 6},
                {"var total = 0; auto arr = [1, 2, 3, 4, 5]; for (auto x : arr) { total = total + x; } total", 15},
            });
            check_values({
                {"var result = \"\"; for (var i = 0; i < 3; i = i + 1) { result = result + to_string(i); } result", "012"s},
            }, true);
        });

        // Test that fast path gracefully falls back to slow path when type changes
        test("for_loop_fast_path_fallthrough", [this]() {
            auto eng = make_engine();
            // Fast path -> slow path fallthrough on mid-loop type change must not skip the
            // pending update: iteration count matches the general path (and the vm backend)
            auto result = eng->execute(R"(
                var iterations = 0;
                for (var i = 0; i < 5; ++i) {
                    iterations = iterations + 1;
                    if (i == 2) {
                        i = 2.5;  // Change to float - triggers fallthrough, but still < 5
                    }
                }
                iterations
            )");
            // i=0,1,2 (iters 1-3; body sets i=2.5), ++ -> 3.5 (iter 4), 4.5 (iter 5), 5.5 exits
            check_eq(result.as<int>(), 5);
        });

        test("cpp_values_flow_into_auto_slots_and_var_values_into_cpp_calls", [this]() {
            struct crossing {
                const char* code;
                std::function<void()> run;
            };
            const crossing rows[] = {
                {"auto x = cpp_int; x = x + 10; x", [this]() {
                    auto eng = make_engine();
                    int cpp_value = 42;
                    eng->add_global_ref("cpp_int", cpp_value);
                    check_eq(eng->execute("auto x = cpp_int; x = x + 10; x").as<int>(), 52);
                }},
                {"auto s = cpp_str; s = s + \" world\"; s", [this]() {
                    auto eng = make_engine();
                    std::string cpp_str = "hello";
                    eng->add_global_ref("cpp_str", cpp_str);
                    check_eq(eng->execute("auto s = cpp_str; s = s + \" world\"; s").as<std::string>(), std::string("hello world"));
                }},
                {"var x = 10; var y = 20; sum_ints(x, y)", [this]() {
                    auto eng = make_engine();
                    eng->add_function("sum_ints", [](int a, int b) { return a + b; });
                    check_eq(eng->execute("var x = 10; var y = 20; sum_ints(x, y)").as<int>(), 30);
                }},
            };
            check_each(rows, [](const crossing& a_row) { a_row.run(); });
        });

        test("type_error_message_is_descriptive", [this]() {
            auto eng = make_engine();
            bool caught = false;
            std::string error_msg;
            try {
                eng->execute(R"(
                    int x = 5;
                    x = "hello";
                )");
            } catch (const std::exception& e) {
                caught = true;
                error_msg = e.what();
            }
            check_eq(caught, true);
            // Error message should indicate a type mismatch
            check(error_msg.find("x") != std::string::npos ||
                  error_msg.find("int") != std::string::npos ||
                  error_msg.find("Cannot assign") != std::string::npos ||
                  error_msg.find("ype") != std::string::npos,  // "Type mismatch" or "type_mismatch"
                  "Error should indicate type issue: " + error_msg);
        });

        test("a_class_slot_takes_its_class_or_a_subclass_and_rejects_the_rest", [this]() {
            check_values({
                {"class Dog { var name = \"Fido\"; } auto pet = Dog(); pet = Dog(); pet.name", "Fido"s},
                {"class Dog { var name = \"Fido\"; } class Cat { var name = \"Whiskers\"; } var pet = Dog(); pet = Cat(); pet.name", "Whiskers"s},
                {"class Animal { var species = \"unknown\"; } class Bird : Animal { var canFly = true; } auto pet = Animal(); pet = Bird(); pet.species", "unknown"s},
                {"class Dog { var name = \"Fido\"; } auto pet = Dog(); pet = null; pet = Dog(); pet.name", "Fido"s},
                {"class Entity { var id = 0; } class LivingEntity : Entity { var health = 100; } class Player : LivingEntity { var score = 0; } "
                 "auto entity = Entity(); entity = Player(); entity.id", 0},
                {"auto c = Cat(); c.name = \"Whiskers\"; c.name", "Whiskers"s, "class Cat { string name = \"\"; } class Dog { string name = \"\"; }"},
                {"class Counter { int count = 0; function increment() { count = count + 1; } function get() -> int { return count; } } "
                 "auto c = Counter(); c.increment(); c.increment(); c.increment(); c.get()", 3},
            });
            check_rejected({
                {"class Animal { var species = \"unknown\"; } class Bird : Animal { var canFly = true; } auto bird = Bird(); bird = Animal();"},
                {"class Dog { var name = \"Fido\"; } class Cat { var name = \"Whiskers\"; } auto pet = Dog(); pet = null; pet = Cat();"},
                {"class Point2D { var x = 0; var y = 0; } class Vector2D { var x = 0; var y = 0; } auto p = Point2D(); p = Vector2D();"},
                {"class Animal { var name = \"\"; } class Dog : Animal { var breed = \"mutt\"; } class Cat : Animal { var indoor = true; } auto pet = Dog(); pet = Cat();"},
                {"class TypeA { var a = 1; } class TypeB { var b = 2; } auto x = TypeA(); auto y = TypeB(); x = y;"},
            });
        });

        test("class_type_different_class_rejected", [this]() {
            auto eng = make_engine();
            // Different class types should NOT be assignable with 'auto'
            bool threw = false;
            try {
                eng->execute(R"(
                    class Dog { var name = "Fido"; }
                    class Cat { var name = "Whiskers"; }
                    auto pet = Dog();
                    pet = Cat();  // Different type - SHOULD FAIL
                )");
            } catch (const std::exception& e) {
                threw = true;
                std::string msg = e.what();
                // Should indicate type mismatch (class names or generic type error)
                check(msg.find("Cat") != std::string::npos ||
                      msg.find("Dog") != std::string::npos ||
                      msg.find("ype") != std::string::npos,  // "Type mismatch" or "type_mismatch"
                      "Error should indicate type issue: " + msg);
            }
            check(threw, "Expected type error when assigning Cat to Dog variable");
        });

        // These tests verify that custom assignment operators enable type conversions

        // TODO: Parser doesn't support "function operator=(Type arg)" syntax yet
        // This test is commented out until parser support is added
        // test("script_class_assignment_operator_conversion") - needs parser update for operator=(Type)

        test("script_class_constructor_conversion", [this]() {
            auto eng = make_engine();
            // Script class with constructor that accepts a different type
            auto result = eng->execute(R"(
                class Celsius {
                    var temp = 0.0;

                    // Constructor from Fahrenheit
                    function Celsius(Fahrenheit f) {
                        temp = (f.temp - 32.0) * 5.0 / 9.0;
                    }
                }

                class Fahrenheit {
                    var temp = 0.0;
                }

                auto f = Fahrenheit();
                f.temp = 212.0;  // Boiling point

                auto c = Celsius(f);  // Should convert via constructor
                c.temp
            )");
            // 212°F = 100°C
            check(result.as<double>() > 99.9 && result.as<double>() < 100.1,
                  "Expected ~100.0, got " + std::to_string(result.as<double>()));
        });

        test("script_class_multiple_assignment_operators", [this]() {
            auto eng = make_engine();
            // Test operator= with var parameter accepting different types
            auto result = eng->execute(R"(
                class Distance {
                    var meters = 0.0;
                    var last_assigned_type = "";

                    function operator=(var value) {
                        // Multiply by 1.0 to convert int to float if needed
                        meters = value * 1.0;
                        last_assigned_type = "assigned";
                    }
                }

                auto d1 = Distance();
                auto d2 = Distance();

                d1 = 100;      // int -> calls operator=
                d2 = 3.14;     // float -> calls operator=

                d1.meters + d2.meters  // 100.0 + 3.14 = 103.14
            )");
            check(result.as<double>() > 103.0 && result.as<double>() < 104.0,
                  "Expected ~103.14, got " + std::to_string(result.as<double>()));
        });

        // TODO: These C++ dynamic_binder tests require assignment_from<T> API to be implemented
        // test("cpp_class_assignment_operator_conversion") - needs dynamic_binder::assignment_from<T>
        // test("cpp_class_assignment_from_script_class") - needs dynamic_binder::assignment_from_script

        test("script_class_implicit_conversion_via_constructor", [this]() {
            auto eng = make_engine();
            // Test that a constructor accepting another type enables implicit conversion
            auto result = eng->execute(R"(
                class Wrapper {
                    var data = 0;

                    // Constructor from int enables Wrapper(int) conversion
                    function Wrapper(int value) {
                        data = value * 10;
                    }
                }

                function acceptWrapper(Wrapper w) -> int {
                    return w.data;
                }

                // This should work if int->Wrapper conversion is implicit via constructor
                // For now, test explicit conversion
                acceptWrapper(Wrapper(5))
            )");
            check_eq(result.as<int>(), 50);
        });

        // ===== COMPOUND ASSIGNMENT: C++ RULES ON TYPED TARGETS =====
        // x op= rhs ≡ x = T(x op rhs): compute in the promoted type, convert the result
        // back to the declared type exactly like plain '=' (int truncates toward zero).
        // var (any-typed) targets keep the old dynamic behavior (int += 2.5 -> float 3.5).
        // Every test runs on BOTH backends and pins byte-identical output.

        // warn_region: the run time's verdict, the unit under [[warn_mode]]
        auto both_in = [](bool warn_region, const char* src) {
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
        auto both = [both_in](const char* src) { return both_in(false, src); };
        auto check_both = [this, both](const char* src, const std::string& expected, const char* what) {
            auto [i_out, v_out] = both(src);
            check_eq(expected, i_out, std::string("interp: ") + what);
            check_eq(expected, v_out, std::string("vm: ") + what);
        };

        test("compound_int_target_converts_back", [this, check_both]() {
            check_both("int x = 10; x += 2.5; type_of(x) + \":\" + to_string(x)", "int:12", "int +=");
            check_both("int x = 10; x -= 2.5; to_string(x)", "7", "int -=");
            check_both("int x = 10; x *= 2.5; to_string(x)", "25", "int *=");
            check_both("int x = 10; x /= 4.0; to_string(x)", "2", "int /= truncates 2.5 -> 2");
            check_both("int x = 10; x %= 4.5; to_string(x)", "1", "int %= fmod then truncate");
        });

        test("compound_truncates_toward_zero", [this, check_both]() {
            check_both("int x = 0; x += 2.5; to_string(x)", "2", "2.5 -> 2");
            check_both("int x = 0; x -= 2.5; to_string(x)", "-2", "-2.5 -> -2");
            check_both("int x = 5; x -= 7.5; to_string(x)", "-2", "5 - 7.5 = -2.5 -> -2");
        });

        test("compound_float_target_stays_float", [this, check_both]() {
            check_both("float f = 1.5; f += 2; type_of(f) + \":\" + to_string(f)", "float:3.5", "float +=int");
            check_both("float f = 7.5; f %= 2; to_string(f)", "1.5", "float %=");
            check_both("float f = 5.0; f /= 2; to_string(f)", "2.5", "float /=");
        });

        test("compound_auto_locked_behaves_typed", [this, check_both]() {
            check_both("auto x = 10; x += 2.5; type_of(x) + \":\" + to_string(x)", "int:12", "auto-locked int");
            check_both("auto f = 1.0; f += 2.5; type_of(f) + \":\" + to_string(f)", "float:3.5", "auto-locked float");
            // An auto parameter is inferred-then-enforced, so it converts back too
            check_both("function f(auto x) { x += 2.5; return type_of(x) + \":\" + to_string(x); } f(10)", "int:12", "inferred param");
            check_both("function f(int x) { x += 2.5; return to_string(x); } f(10)", "12", "typed param");
            check_both("function g() { int y = 10; y += 2.5; return to_string(y); } g()", "12", "slot local");
        });

        test("compound_var_target_stays_dynamic", [this, check_both]() {
            check_both("var x = 10; x += 2.5; type_of(x) + \":\" + to_string(x)", "float:12.5", "var += float promotes");
            check_both("var x = 10; x %= 3; type_of(x) + \":\" + to_string(x)", "int:1", "var %= int stays int");
            check_both("var x = 10; x %= 4.5; type_of(x) + \":\" + to_string(x)", "float:1.0", "var %= float promotes");
            // The dynamic tag survives a mixed-type compound: the variable is still a var
            // afterwards (the old in-place path used to clobber the 'any' tag, wrongly
            // locking the variable to the promoted type)
            check_both("var x = 10; x += 2.5; x = \"s\"; x", "s", "var stays var after mixed compound");
        });

        test("compound_modulo_scalar_targets", [this, check_both, both]() {
            check_both("int x = 10; x %= 3; to_string(x)", "1", "int %= int");
            check_both("auto x = 10; x %= 3; to_string(x)", "1", "auto %= int");
            // Divide/modulo by zero errors stay identical across backends
            auto [i1, v1] = both("int x = 10; x %= 0; x");
            check_eq(i1, v1, "modulo-by-zero parity");
            check(i1.find("ERROR") == 0, "int %= 0 raises");
            auto [i2, v2] = both("float f = 1.5; f %= 0.0; f");
            check_eq(i2, v2, "float modulo-by-zero parity");
            check(i2.find("ERROR") == 0, "float %= 0.0 raises");
        });

        test("compound_string_targets_append", [this, check_both, both]() {
            check_both("string s = \"a\"; s += \"b\"; s", "ab", "string += string");
            check_both("var s = \"a\"; s += \"b\"; s", "ab", "var-string += string");
            // Non-string rhs on a string target keeps today's type_mismatch (parity-pinned)
            auto [i_out, v_out] = both("string s = \"a\"; s += 1; s");
            check_eq(i_out, v_out, "string += int parity");
            check(i_out.find("ERROR") == 0, "string += int raises");
        });

        test("compound_expression_result_is_stored_value", [this, check_both]() {
            // The compound expression evaluates to the converted (stored) value, exactly
            // like '=' on a typed local does
            check_both("int x = 10; var r = (x += 2.5); type_of(r) + \":\" + to_string(r)", "int:12", "result converted");
            check_both("var x = 10; var r = (x += 2.5); type_of(r) + \":\" + to_string(r)", "float:12.5", "var result promoted");
        });

        test("compound_typed_element_and_ref_targets", [this, check_both]() {
            // Constrained element: the store converts (12.5 -> 12) and a consumed expression's
            // value is the stored element
            check_both("array<int> a = [10]; a[0] += 2.5; type_of(a[0]) + \":\" + to_string(a[0])", "int:12", "typed element converts");
            check_both("array<int> a = [10]; var r = (a[0] += 2.5); to_string(r)", "12", "element result is the stored element");
            check_both("array<int> a = [10]; a[0] %= 4.5; to_string(a[0])", "1", "typed element %=");
            // Constrained ref bound to a typed element behaves route-independently
            check_both(R"(
                array<int> a = [10];
                function f(int& r) { r += 2.5; }
                f(a[0]);
                type_of(a[0]) + ":" + to_string(a[0])
            )", "int:12", "constrained ref compound converts");
        });

        test("compound_mismatch_errors_parity", [this, both]() {
            auto [i1, v1] = both("int x = 10; x += \"s\"; x");
            check_eq(i1, v1, "int += string parity");
            check(i1.find("ERROR") == 0, "int += string raises");
            auto [i2, v2] = both("bool b = true; b += 1; b");
            check_eq(i2, v2, "bool += int parity");
            check(i2.find("ERROR") == 0, "bool += int raises");
        });

        test("compound_overflow_policy_covers_intermediate", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                if (!e->throw_on_overflow()) { return; }  // wrap build: policy off by design
                bool threw = false;
                try { e->execute("int x = 9223372036854775807; x += 1; x"); }
                catch (const std::exception&) { threw = true; }
                check(threw, use_vm ? "vm: += overflow raises" : "interp: += overflow raises");
            }
        });

        // ===== TYPED CLASS FIELDS ENFORCE LIKE LOCALS =====
        // Declared field types are stored on the class definition and every write path
        // converts through the field kernel (same table '=' uses on locals): direct member
        // assign, implicit-this assign, compound, ctor field-init, set_field C++ API,
        // from_json. var fields stay dynamic; auto fields infer from the initialized value
        // then enforce.

        test("typed_field_direct_assign_converts", [this, check_both, both, both_in]() {
            check_both(R"(
                class C { int v = 0; }
                auto c = C();
                c.v = 4.7;
                type_of(c.v) + ":" + to_string(c.v)
            )", "int:4", "int field truncates float assign");
            check_both(R"(
                class C { float f = 0.0; }
                auto c = C();
                c.f = 3;
                type_of(c.f) + ":" + to_string(c.f)
            )", "float:3.0", "float field widens int assign");
            check_both(R"(
                class C { string s = ""; }
                auto c = C();
                utf8_string u = "42";
                c.s = u;
                type_of(c.s) + ":" + c.s
            )", "string:42", "a string field takes a utf8_string's bytes");
            // a string literal into an int field: a strict load error; the locals-identical
            // runtime text under warn and, under strict, for a var-sourced value
            const char* typed_mismatch = R"(
                class C { int v = 0; }
                auto c = C();
                c.v = "nope";
                c.v
            )";
            auto [i_out, v_out] = both(typed_mismatch);
            check_eq(i_out, v_out, "string-into-int field strict listing parity");
            check_true(i_out.find("ERROR: static check failed") == 0 &&
                i_out.find("cannot assign 'string' to field 'v' declared 'int'") != std::string::npos,
                std::string("strict rejects the field store at load, got: ") + i_out);
            auto [w_out, wv_out] = both_in(true, typed_mismatch);
            check_eq(w_out, wv_out, "string-into-int field error parity");
            check_true(w_out.find("ERROR") == 0 && w_out.find("Cannot assign string to field 'v' of C (declared int)") != std::string::npos,
                std::string("locals-identical error text, got: ") + w_out);
            auto [d_out, dv_out] = both(R"(
                class C { int v = 0; }
                auto c = C();
                var s = "nope";
                c.v = s;
                c.v
            )");
            check_eq(d_out, dv_out, "var-sourced string-into-int field error parity");
            check_true(d_out.find("ERROR") == 0 && d_out.find("Cannot assign string to field 'v' of C (declared int)") != std::string::npos,
                std::string("var-sourced store keeps the runtime text under strict, got: ") + d_out);
        });

        test("typed_field_implicit_this_and_ctor_convert", [this, check_both, both]() {
            check_both(R"(
                class C {
                    int v = 0;
                    void set(var x) { v = x; }
                }
                auto c = C();
                c.set(9.9);
                type_of(c.v) + ":" + to_string(c.v)
            )", "int:9", "implicit-this assign converts");
            check_both(R"(
                class C { int v = 4.7; }
                auto c = C();
                type_of(c.v) + ":" + to_string(c.v)
            )", "int:4", "ctor field-init converts the initializer");
            check_both(R"(
                class C {
                    int v = 0;
                    C(var x) { v = x; }
                }
                auto c = C(6.9);
                to_string(c.v)
            )", "6", "ctor body assign converts");
            auto [i_out, v_out] = both(R"(
                class C {
                    int v = 0;
                    void set(var x) { v = x; }
                }
                auto c = C();
                c.set("bad");
                c.v
            )");
            check_eq(i_out, v_out, "implicit-this mismatch parity");
            check_true(i_out.find("ERROR") == 0, "implicit-this mismatch raises");
        });

        test("typed_field_compound_converts", [this, check_both]() {
            check_both(R"(
                class C { int v = 10; }
                auto c = C();
                c.v += 2.5;
                type_of(c.v) + ":" + to_string(c.v)
            )", "int:12", "member compound converts back");
            check_both(R"(
                class C {
                    int v = 10;
                    void bump() { v += 2.5; }
                }
                auto c = C();
                c.bump();
                type_of(c.v) + ":" + to_string(c.v)
            )", "int:12", "implicit-this compound converts back");
            check_both(R"(
                class C { int v = 10; }
                auto c = C();
                c.v %= 4.5;
                to_string(c.v)
            )", "1", "member %= math::fmod then truncate");
        });

        test("var_and_auto_fields", [this, check_both, both]() {
            // var fields stay fully dynamic
            check_both(R"(
                class C { var v = 1; }
                auto c = C();
                c.v = 2.5;
                var t1 = type_of(c.v);
                c.v = "s";
                t1 + "|" + c.v
            )", "float|s", "var field stays dynamic");
            check_both(R"(
                class C { var v = 10; }
                auto c = C();
                c.v += 2.5;
                type_of(c.v) + ":" + to_string(c.v)
            )", "float:12.5", "var field compound stays dynamic");
            // auto fields infer from the initializer then enforce
            check_both(R"(
                class C { auto v = 1; }
                auto c = C();
                c.v = 2.5;
                type_of(c.v) + ":" + to_string(c.v)
            )", "int:2", "auto field locks to inferred int");
            auto [i_out, v_out] = both(R"(
                class C { auto v = 1; }
                auto c = C();
                c.v = "s";
                c.v
            )");
            check_eq(i_out, v_out, "auto field mismatch parity");
            check_true(i_out.find("ERROR") == 0, "auto field rejects a string");
        });

        test("typed_field_inheritance_enforced", [this, check_both, both]() {
            check_both(R"(
                class Base { int bv = 0; }
                class Derived : Base { var dv = 0; }
                auto d = Derived();
                d.bv = 7.7;
                type_of(d.bv) + ":" + to_string(d.bv)
            )", "int:7", "base typed field enforced on derived instance");
            auto [i_out, v_out] = both(R"(
                class Base { int bv = 0; }
                class Derived : Base { var dv = 0; }
                auto d = Derived();
                d.bv = "bad";
                d.bv
            )");
            check_eq(i_out, v_out, "inherited field mismatch parity");
            check_true(i_out.find("ERROR") == 0, "inherited typed field rejects a string");
        });

        test("typed_field_object_fields_keep_object_model", [this, check_both, both, both_in]() {
            // Declared class-typed fields accept same class, subtypes, null, and shared_ptr
            // wrappers (JaiScript objects travel as object OR shared_ptr over one holder).
            // A subtype member is reached through cast<T&> or a var receiver: the field's
            // static class is S, and S has no 't' (exactly like C++). A field holding an S keeps
            // its object and takes the subtype's class (no slicing).
            check_both(R"(
                class S { int x = 1; }
                class T : S { int t = 2; }
                class H { S p = null; }
                auto h = H();
                h.p = S();
                var a = h.p.x;
                h.p = T();
                var b = cast<T&>(h.p).t;
                var hv = h.p;
                var b2 = hv.t;
                h.p = null;
                var c2 = "n";
                if (h.p == null) { c2 = "null"; }
                to_string(a) + "|" + to_string(b) + "|" + to_string(b2) + "|" + c2
            )", "1|2|2|null", "same class, subtype (cast<T&> and var receiver), and null accepted");
            const char* base_receiver = R"(
                class S { int x = 1; }
                class T : S { int t = 2; }
                class H { S p = null; }
                auto h = H();
                h.p = T();
                h.p.t
            )";
            auto [s_out, sv_out] = both(base_receiver);
            check_eq(s_out, sv_out, "subtype member on the base-typed field: strict listing parity");
            check_true(s_out.find("ERROR: static check failed") == 0 &&
                s_out.find("no member 't' in class 'S'") != std::string::npos,
                std::string("strict rejects the subtype member on the static base class, got: ") + s_out);
            auto [w_out, wv_out] = both_in(true, base_receiver);
            check_eq(std::string("2"), w_out, "warn: the dynamic object still answers");
            check_eq(w_out, wv_out, "warn subtype-member parity");
            const char* unrelated = R"(
                class S { int x = 1; }
                class B { int x = 1; }
                class H { S p = null; }
                auto h = H();
                h.p = B();
                h.p
            )";
            auto [i_out, v_out] = both(unrelated);
            check_eq(i_out, v_out, "unrelated class rejection parity");
            check_true(i_out.find("ERROR: static check failed") == 0 &&
                i_out.find("cannot assign 'B' to field 'p' declared 'S'") != std::string::npos,
                std::string("unrelated class rejected at load, got: ") + i_out);
            auto [wu_out, wuv_out] = both_in(true, unrelated);
            check_eq(wu_out, wuv_out, "warn unrelated class rejection parity");
            check_true(wu_out.find("ERROR") == 0 && wu_out.find("static check failed") == std::string::npos,
                std::string("warn: the runtime rejects the unrelated class, got: ") + wu_out);
            auto [du_out, duv_out] = both(R"(
                class S { int x = 1; }
                class B { int x = 1; }
                class H { S p = null; }
                auto h = H();
                var b = B();
                h.p = b;
                h.p
            )");
            check_eq(wu_out, du_out, "var-sourced unrelated class: the same runtime text under strict");
            check_eq(du_out, duv_out, "var-sourced unrelated class rejection parity");
        });

        // strict vs warn at the typed boundary: a strongly-typed -> strongly-typed mismatch is
        // a load error under strict and the runtime's conversion error under warn; a var-sourced
        // value reaches the runtime in both modes; clean typed programs are mode-blind. Every
        // row is byte-identical across backends.
        test("strict_vs_warn_boundary", [this, both_in]() {
            struct row { const char* src; const char* strict_needle; const char* warn_needle; };
            const row typed_mismatches[] = {
                {"int x = \"s\"; x",
                 "cannot initialize 'x' declared 'int' with 'string'", "Cannot initialize 'x' (declared int) with string"},
                {"function bad() -> int { return \"nope\"; } bad()",
                 "cannot return 'string' from 'bad' declared to return 'int'", "Cannot return string from 'bad' (declared -> int)"},
                {"function takes(int x) -> int { return x; } takes([1, 2])",
                 "argument 1 of 'takes': cannot pass 'array' to 'x' declared 'int'", "Cannot pass array to parameter 'x' of 'takes' (declared int)"},
                {"class A { int a = 0; } class B { int b = 0; } function f(A v) -> int { return v.a; } f(B())",
                 "argument 1 of 'f': cannot pass 'B' to 'v' declared 'A'", "Cannot convert B to A"},
                {"class C { int v = 0; } auto c = C(); c.v = \"nope\"; c.v",
                 "cannot assign 'string' to field 'v' declared 'int'", "Cannot assign string to field 'v' of C (declared int)"},
                {"auto x = 5; x = \"hello\"; x",
                 "cannot assign 'string' to 'x' declared 'int'", "Cannot assign string to 'x' (declared int)"},
                {"int x = 1; x += \"s\"; x",
                 "cannot assign 'string' to 'x' declared 'int'", "Cannot apply '+=' to int and string"},
                {"string s = 42; s",
                 "cannot initialize 's' declared 'string' with 'int': stringify it with to_string(x) or `${x}`", "Cannot initialize 's' (declared string) with int"},
            };
            for (const row& r : typed_mismatches) {
                auto [si, sv] = both_in(false, r.src);
                check_eq(si, sv, std::string("strict listing parity: ") + r.src);
                check_true(si.find("ERROR: static check failed") == 0 && si.find(r.strict_needle) != std::string::npos,
                    std::string("strict load error: ") + r.src + " (got: " + si + ")");
                auto [wi, wv] = both_in(true, r.src);
                check_eq(wi, wv, std::string("warn runtime text parity: ") + r.src);
                check_true(wi.find("ERROR: ") == 0 && wi.find("static check failed") == std::string::npos &&
                    wi.find(r.warn_needle) != std::string::npos,
                    std::string("warn runtime error: ") + r.src + " (got: " + wi + ")");
            }
            struct var_row { const char* src; const char* needle; };
            const var_row var_sourced[] = {
                {"var s = \"s\"; int x = s; x", "Cannot initialize 'x' (declared int) with string"},
                {"var v = \"nope\"; function bad() -> int { return v; } bad()", "Cannot return string from 'bad' (declared -> int)"},
                {"var arr = [1, 2]; function takes(int x) -> int { return x; } takes(arr)", "Cannot pass array to parameter 'x' of 'takes' (declared int)"},
                {"class A { int a = 0; } class B { int b = 0; } function f(A v) -> int { return v.a; } var b = B(); f(b)", "Cannot convert B to A"},
                {"class C { int v = 0; } auto c = C(); var s = \"nope\"; c.v = s; c.v", "Cannot assign string to field 'v' of C (declared int)"},
                {"auto x = 5; var s = \"hello\"; x = s; x", "Cannot assign string to 'x' (declared int)"},
                {"var caught = \"\"; var s = \"s\"; try { int x = s; } catch (e) { caught = e; } caught", "Cannot initialize 'x' (declared int) with string"},
            };
            for (const var_row& r : var_sourced) {
                auto [si, sv] = both_in(false, r.src);
                auto [wi, wv] = both_in(true, r.src);
                check_eq(si, sv, std::string("var-sourced strict parity: ") + r.src);
                check_eq(si, wi, std::string("var-sourced: strict and warn agree: ") + r.src);
                check_eq(wi, wv, std::string("var-sourced warn parity: ") + r.src);
                check_true(si.find("static check failed") == std::string::npos && si.find(r.needle) != std::string::npos,
                    std::string("var-sourced runtime text: ") + r.src + " (got: " + si + ")");
            }
            const char* last_var = var_sourced[6].src;
            check_eq(std::string("Cannot initialize 'x' (declared int) with string"), both_in(false, last_var).first,
                "the var-sourced runtime error is catchable in script under strict");
            struct clean_row { const char* src; const char* expected; };
            const clean_row clean[] = {
                {"int x = 4.7; x", "4"},
                {"float f = 2; type_of(f) + \":\" + to_string(f)", "float:2.0"},
                {"class S { int x = 1; } class T : S { int t = 2; } function f(S s) -> int { return s.x; } f(T())", "1"},
                {"function twice(int v) -> int { return v * 2; } twice(21)", "42"},
                {"class C { int v = 0; } auto c = C(); c.v = 4.7; type_of(c.v) + \":\" + to_string(c.v)", "int:4"},
                {"class S { int x = 1; } class T : S { int t = 2; } class H { S p = null; } auto h = H(); h.p = T(); cast<T&>(h.p).t", "2"},
            };
            for (const clean_row& r : clean) {
                auto [si, sv] = both_in(false, r.src);
                auto [wi, wv] = both_in(true, r.src);
                check_eq(std::string(r.expected), si, std::string("clean under strict: ") + r.src);
                check_eq(si, sv, std::string("clean strict parity: ") + r.src);
                check_eq(si, wi, std::string("clean: strict and warn agree: ") + r.src);
                check_eq(wi, wv, std::string("clean warn parity: ") + r.src);
            }
        });

        // cast<T>(e) / cast<T&>(e) / cast<shared_ptr<T>>(e) (docs/site/guide/02-type-ladder.html, detail/cast_kernel.hpp):
        // every row is byte-identical on both backends under the strict default
        const std::string cast_prelude =
            "class S { int x = 1; } class T : S { int t = 2; } class H { S p = null; } "
            "auto h = H(); h.p = T(); ";
        auto with_prelude = [cast_prelude](const char* tail) { return cast_prelude + tail; };

        test("cast_value_reference_and_shared_forms", [this, check_both, with_prelude]() {
            check_both(with_prelude("cast<T&>(h.p).t").c_str(), "2", "reference form reads the subtype member");
            check_both(with_prelude("cast<T&>(h.p).t = 5; var d = h.p; to_string(d.t)").c_str(), "5", "reference form writes through");
            check_both(with_prelude("T tv = cast<T>(h.p); tv.t = 9; var d = h.p; to_string(d.t) + \",\" + to_string(tv.t)").c_str(), "2,9", "value form is an independent copy");
            check_both(with_prelude("shared_ptr<T> th = cast<shared_ptr<T>>(h.p); th.t = 11; var d = h.p; to_string(d.t)").c_str(), "11", "shared_ptr form shares the instance");
            check_both(with_prelude("auto& r = cast<T&>(h.p); r.t = 21; cast<T&>(h.p).t").c_str(), "21", "auto& aliases the cast reference");
            check_both(with_prelude("T tmp = T(); auto& r = cast<T&>(tmp); r.t = 33; to_string(tmp.t)").c_str(), "33", "identifier operand binds the variable");
            check_both(with_prelude("cast<T&>(T()).t").c_str(), "2", "temporary operand is pinned");
            check_both(with_prelude("array<S> arr = [T(), T()]; cast<T&>(arr[1]).t = 44; var e = arr[1]; to_string(e.t)").c_str(), "44", "subscript operand aliases the element");
            check_both(with_prelude("shared_ptr<T> sp = new T(); T v = cast<T>(sp); v.t = 8; to_string(sp.t)").c_str(), "2", "value form deep-copies a handle");
            check_both(with_prelude("T t0 = T(); auto& r = t0; T v = cast<T>(r); v.t = 8; to_string(t0.t) + \",\" + to_string(v.t)").c_str(), "2,8", "value form copies a reference operand");
        });

        test("cast_upcast_and_null", [this, check_both, with_prelude]() {
            check_both(with_prelude("S up = cast<S>(T()); to_string(up.x)").c_str(), "1", "upcast by value");
            check_both(with_prelude("cast<S&>(h.p).x").c_str(), "1", "upcast by reference");
            check_both(with_prelude("shared_ptr<S> sh = cast<shared_ptr<S>>(h.p); to_string(sh.x)").c_str(), "1", "upcast handle");
            check_both("class T { int t = 2; } var n = null; to_string(cast<T>(n) == null) + \",\" + to_string(cast<shared_ptr<T>>(n) == null)", "true,true", "null passes the value and shared_ptr forms");
            check_both("class T { int t = 2; } var n = null; var c = \"\"; try { cast<T&>(n).t; } catch (e) { c = e; } c", "Cannot cast 'null' to 'T&'", "null fails the reference form");
        });

        test("cast_failures_are_catchable_with_exact_text", [this, check_both, with_prelude]() {
            check_both(with_prelude("var c = \"\"; try { var bad = cast<H>(h.p); } catch (e) { c = e; } c").c_str(), "Cannot cast 'T' to 'H'", "unrelated class, value form");
            check_both(with_prelude("var c = \"\"; try { cast<H&>(h.p).p; } catch (e) { c = e; } c").c_str(), "Cannot cast 'T' to 'H&'", "unrelated class, reference form");
            check_both(with_prelude("var c = \"\"; try { var bad = cast<shared_ptr<H>>(h.p); } catch (e) { c = e; } c").c_str(), "Cannot cast 'T' to 'shared_ptr<H>'", "unrelated class, shared_ptr form");
            check_both("class T { int t = 2; } var v = 5; var c = \"\"; try { var bad = cast<T>(v); } catch (e) { c = e; } c", "Cannot cast 'int' to 'T'", "non-object into a class");
            check_both("var v = \"abc\"; var c = \"\"; try { var bad = cast<int>(v); } catch (e) { c = e; } c", "Cannot cast 'string' to 'int'", "unconvertible primitive");
            check_both(with_prelude("var c = \"\"; try { var ok = cast<T>(h.p); c = \"ran\"; } catch (e) { c = e; } c").c_str(), "ran", "admitted casts do not raise");
        });

        test("cast_primitive_conversions", [this, check_both]() {
            check_both("var f = 2.7; to_string(cast<int>(f)) + \":\" + type_of(cast<int>(f))", "2:int", "float -> int truncates");
            check_both("var i = 5; cast<string>(i) + \":\" + type_of(cast<string>(i))", "5:string", "int -> string");
            check_both("var i = 1; to_string(cast<float>(i)) + \":\" + type_of(cast<float>(i))", "1.0:float", "int -> float widens");
            check_both("var z = 0; to_string(cast<bool>(z))", "false", "-> bool is truthiness");
            check_both("int total = 0; var xs = [1.5, 2.5]; for (auto x : xs) { total += cast<int>(x); } to_string(total)", "3", "cast inside a typed compound store");
            check_both("int n = 0; var v = 7.9; n = cast<int>(v); to_string(n)", "7", "cast<int> is a parse-proven typed store");
        });

        test("cast_char_rules", [this, check_both, both]() {
            // char <-> int/float/string convert through the cast alone (docs/site/guide/02-type-ladder.html
            // "cast<T>(e): the explicit boundary"); plain assignment keeps refusing the pair
            check_both("var ch = 'a'; to_string(cast<int>(ch)) + \":\" + type_of(cast<int>(ch))", "97:int", "char -> int is the code point");
            check_both("to_string(cast<int>('\\xC3'))", "195", "char -> int is unsigned (0..255)");
            check_both("var ch = 'a'; to_string(cast<float>(ch)) + \":\" + type_of(cast<float>(ch))", "97.0:float", "char -> float is the code point widened");
            check_both("var ch = 'a'; cast<string>(ch) + \":\" + type_of(cast<string>(ch))", "a:string", "char -> string is one text unit");
            check_both("var ch = 'a'; to_string(cast<char>(ch)) + \":\" + type_of(cast<char>(ch))", "a:char", "char -> char is identity");
            check_both("var i = 97; to_string(cast<char>(i)) + \":\" + type_of(cast<char>(i))", "a:char", "int -> char in range");
            check_both("to_string(cast<char>(0) == '\\x00') + \",\" + to_string(cast<char>(255) == '\\xFF')", "true,true", "int -> char range ends");
            check_both("var s = \"a\"; to_string(cast<char>(s)) + \":\" + type_of(cast<char>(s))", "a:char", "one-character string -> char");
            check_both("to_string(cast<char>(cast<int>('z')) == 'z')", "true", "char -> int -> char round trip");
            check_both("to_string(cast<char>(cast<string>('q')) == 'q')", "true", "char -> string -> char round trip");
            check_both("to_string(cast<int>(cast<char>(200)))", "200", "int -> char -> int round trip");
            check_both("char c = cast<char>(65); to_string(c) + \":\" + type_of(c)", "A:char", "cast<char> is a parse-proven typed store");
            check_both("int code = cast<int>('a'); to_string(code)", "97", "cast<int> of a char into a typed local");
            check_both("var i = 256; var c = \"\"; try { var bad = cast<char>(i); } catch (e) { c = e; } c", "Cannot cast 'int' to 'char'", "int above 255 fails");
            check_both("var i = -1; var c = \"\"; try { var bad = cast<char>(i); } catch (e) { c = e; } c", "Cannot cast 'int' to 'char'", "negative int fails");
            check_both("var s = \"ab\"; var c = \"\"; try { var bad = cast<char>(s); } catch (e) { c = e; } c", "Cannot cast 'string' to 'char'", "multi-character string fails");
            check_both("var s = \"\"; var c = \"\"; try { var bad = cast<char>(s); } catch (e) { c = e; } c", "Cannot cast 'string' to 'char'", "empty string fails");
            check_both("var b = true; var c = \"\"; try { var bad = cast<char>(b); } catch (e) { c = e; } c", "Cannot cast 'bool' to 'char'", "bool -> char stays refused");
            check_both("var f = 66.5; var c = \"\"; try { var bad = cast<char>(f); } catch (e) { c = e; } c", "Cannot cast 'float' to 'char'", "float -> char stays refused");
            check_both("var i = 97; var c = \"\"; try { cast<char&>(i); } catch (e) { c = e; } c", "Cannot cast 'int' to 'char&'", "reference form never converts");
            check_both("var v = 65; var c = \"\"; try { char bad = v; } catch (e) { c = e; } c", "Cannot initialize 'bad' (declared char) with int", "plain assignment still has no int -> char rule");
            auto [si, sv] = both("char c = 65;");
            check_eq(si, sv, "static rejection parity: char c = 65");
            check(si.find("ERROR: ") == 0 && si.find("cannot initialize 'c' declared 'char' with 'int'") != std::string::npos,
                  "char c = 65 is still rejected before anything runs: " + si);
        });

        test("cast_inside_method_on_field", [this, check_both]() {
            check_both(R"(
                class S { int x = 1; }
                class T : S { int t = 2; }
                class M {
                    S field = null;
                    M() { field = T(); }
                    int peek() { return cast<T&>(field).t; }
                    int poke() { cast<T&>(field).t = 77; return cast<T&>(this.field).t; }
                    T copy() { return cast<T>(field); }
                }
                auto m = M();
                to_string(m.peek()) + "," + to_string(m.poke()) + "," + to_string(m.copy().t)
            )", "2,77,77", "field operand through the implicit and explicit receiver");
        });

        test("cast_reference_is_an_assignment_target", [this, check_both, both, both_in, with_prelude]() {
            // cast<T&>(e) is an lvalue (C++): =, op= and ++/-- store through it under the rules of the storage it names
            check_both("int x = 1; cast<int&>(x) = 5; to_string(x)", "5", "a variable");
            check_both("int x = 1; cast<int&>(x) = 3.9; type_of(x) + \":\" + to_string(x)", "int:3", "the variable's table converts");
            check_both("int x = 10; cast<int&>(x) += 2.5; type_of(x) + \":\" + to_string(x)", "int:12", "a compound converts back to the variable's type");
            check_both("int x = 5; cast<int&>(x)++; ++cast<int&>(x); cast<int&>(x)--; to_string(x)", "6", "increment and decrement");
            check_both("int x = 5; int old = cast<int&>(x)++; int now = ++cast<int&>(x); to_string(old) + \",\" + to_string(now) + \",\" + to_string(x)", "5,7,7", "postfix yields the old value, prefix the new");
            check_both("int x = 0; auto r = (cast<int&>(x) = 3.9); type_of(r) + \":\" + to_string(r)", "int:3", "the expression's value is the stored value");
            check_both("int x = 1; auto r = (cast<int&>(x) += 0.5); type_of(r) + \":\" + to_string(r)", "int:1", "a consumed compound yields the stored value");
            check_both("function f() -> int { int x = 1; cast<int&>(x) += 2.9; cast<int&>(x) *= 2; return x; } to_string(f())", "6", "a function's slot local");
            check_both("class C { int n = 0; } C c = C(); cast<int&>(c.n) = 7; cast<int&>(c.n) *= 3; to_string(c.n)", "21", "a field");
            check_both("class C { int n = 0; void go() { cast<int&>(n) = 4; cast<int&>(this.n) += 1; } } C c = C(); c.go(); to_string(c.n)", "5", "a bare field and this.field");
            check_both("class W { static int s = 1; } cast<int&>(W::s) = 6; cast<int&>(W::s) += 1; to_string(W::s)", "7", "a static");
            check_both("array<float> a = [1.0, 2.0]; int i = 1; cast<float&>(a[i]) += 1; cast<float&>(a[0]) = 4; to_string(a[0]) + \",\" + to_string(a[1])", "4.0,3.0", "a typed element");
            check_both("var a = [1, 2]; cast<int&>(a[1]) = 9; to_string(a[1])", "9", "an untyped element");
            check_both("map<string, int> m = {\"a\": 1}; cast<int&>(m[\"a\"]) += 4; to_string(m[\"a\"])", "5", "a map entry");
            check_both("string s = \"ab\"; cast<string&>(s) += \"cd\"; s", "abcd", "a string appends");
            check_both("class P { int v = 1; } P p = P(); P q = P(); q.v = 9; cast<P&>(p) = q; q.v = 2; to_string(p.v)", "9", "a class value copies");
            check_both(with_prelude("T n = T(); n.t = 5; cast<T&>(h.p) = n; var d = h.p; to_string(d.t)").c_str(), "5", "a subtype through a base-typed field");
            check_both("math::vec2 v = math::vec2(1, 2); cast<float&>(v.x) = 8; cast<float&>(v.y) += 1; cast<float&>(v.y)++; to_string(v)", "(8.0, 4.0)", "a math component");
            check_both("var log = \"\"; function k() -> int { log += \"k\"; return 0; } function v() -> int { log += \"v\"; return 1; } array<int> a = [0]; cast<int&>(a[k()]) = v(); cast<int&>(a[k()]) += v(); log + to_string(a[0])", "vkvk2", "the right side runs before the target");
            check_both("int x = 1; var c = \"\"; try { cast<float&>(x) = 2; } catch (e) { c = e; } c + to_string(x)", "Cannot cast 'int' to 'float&'1", "the cast's own verdict raises first");
            auto [si, sv] = both("int x = 1; cast<int&>(x) = \"s\";");
            check_eq(si, sv, "static rejection parity: a string into cast<int&>");
            check(si.find("ERROR: ") == 0 && si.find("cannot assign 'string'") != std::string::npos, "a string into cast<int&> is rejected at load: " + si);
            auto [ii, iv] = both("string s = \"a\"; cast<string&>(s)++;");
            check_eq(ii, iv, "static rejection parity: ++ on cast<string&>");
            check(ii.find("ERROR: ") == 0 && ii.find("cannot apply '++' to 'string'") != std::string::npos, "++ on cast<string&> is rejected at load: " + ii);
            auto [oi, ov] = both_in(true, "int x = 1; var c = \"\"; try { cast<int&>(x) = \"s\"; } catch (e) { c = e; } c");
            check_eq(oi, ov, "run-time rejection parity: a string into cast<int&>");
            check_eq(std::string("Cannot assign string to a variable declared int"), oi, "a string into cast<int&> raises at run time");
            auto [pi, pv] = both_in(true, "string s = \"a\"; var c = \"\"; try { cast<string&>(s)++; } catch (e) { c = e; } c");
            check_eq(pi, pv, "run-time rejection parity: ++ on cast<string&>");
            check_eq(std::string("Cannot apply '++' to string"), pi, "++ on cast<string&> raises at run time");
            check_both("var log = \"\"; function v() -> int { log += \"v\"; return 5; } "
                       "coroutine h() { array<int> a = [0, 0]; cast<int&>(a[(yield 1) == null ? 1 : 0]) += v(); int k = 1; cast<int&>(k) += v() + ((yield 2) == null ? 1 : 0); yield to_string(a[1]) + to_string(k); } "
                       "var co = h(); co.next(); co.next(); co.next() + log", "57vv", "a yield inside the target or the right side resumes without re-running either");
            check_both("class W { static int s = 1; } cast<int&>(W::s) += 0.75; type_of(W::s) + \":\" + to_string(W::s)", "int:1", "a static's own table");
        });

        test("reference_stores_run_the_storage_rules", [this, check_both]() {
            // a compound through a reference to a typed variable runs the variable's table, as the direct compound does
            check_both("int x = 10; auto& r = x; r += 2.5; type_of(x) + \":\" + to_string(x)", "int:12", "auto& to an int");
            check_both("function f(int& r) { r += 2.5; } int x = 10; f(x); type_of(x) + \":\" + to_string(x)", "int:12", "an int& parameter");
            check_both("int x = 10; var& r = x; r -= 0.5; type_of(x) + \":\" + to_string(x)", "int:9", "var& to an int");
            const std::string cnt = "class Cnt { int n = 0; function operator=(int v) { n = v * 10; } } ";
            check_both((cnt + "Cnt c = Cnt(); auto& r = c; r = 3; to_string(c.n)").c_str(), "30", "a reference to a class variable runs its operator=");
            check_both((cnt + "class H { Cnt k = Cnt(); } H h = H(); auto& r = h.k; r = 4; to_string(h.k.n)").c_str(), "40", "a reference to a class field runs its operator=");
            check_both((cnt + "Cnt c = Cnt(); cast<Cnt&>(c) = 2; to_string(c.n)").c_str(), "20", "a store through cast<Cnt&> runs its operator=");
        });

        test("a_static_member_is_a_reference_return", [this, check_both]() {
            check_both("class W { static int s = 3; } function g() -> int& { return W::s; } auto& r = g(); r = 9; to_string(W::s)", "9", "a static through -> int&");
            check_both("class W { static int s = 3; static int& get() { return W::s; } } auto& r = W::get(); r += 2; to_string(W::s)", "5", "a static method returning its class's static");
            check_both("namespace ns { int v = 1; } function g() -> int& { return ns::v; } auto& r = g(); r = 4; to_string(ns::v)", "4", "a namespace variable");
        });

        test("a_reference_result_is_an_assignment_target", [this, check_both, both]() {
            // f() returning T&, a conditional of lvalues, an assignment and a prefix ++/-- are lvalues (C++)
            check_both("int g = 1; function f() -> int& { return g; } f() = 9; f() += 1; ++f(); f()++; to_string(g)", "12", "a function returning int&");
            check_both("int g = 1; function f() -> int& { return g; } f() = 3.9; type_of(g) + \":\" + to_string(g)", "int:3", "the referent's table converts");
            check_both("int g = 1; function f() -> int& { return g; } auto r = (f() = 3.9); type_of(r) + \":\" + to_string(r)", "int:3", "the expression's value is the stored value");
            check_both("class W { array<int> d = [1, 2, 3]; int& first() { return d[0]; } } W w = W(); w.first() = 11; w.first() *= 2; to_string(w.d[0])", "22", "a method returning int&");
            check_both("class S { static int s = 1; static int& get() { return S::s; } } S::get() = 4; S::get()++; to_string(S::s)", "5", "a static method returning a static");
            check_both("class W { array<int> d = [1, 2, 3]; int& operator[](int i) { return d[i]; } } W w = W(); w[2] = 30; ++w[1]; to_string(w.d[1]) + \",\" + to_string(w.d[2])", "3,30", "a user operator[] returning int&");
            check_both("var log = \"\"; int g = 0; function f() -> int& { log += \"f\"; return g; } function v() -> int { log += \"v\"; return 1; } f() = v(); f() += v(); log + to_string(g)", "vfvf2", "the right side runs before the call");
            check_both("int a = 1; int b = 2; bool t = true; (t ? a : b) = 5; (!t ? a : b) += 10; to_string(a) + \",\" + to_string(b)", "5,12", "a conditional of variables");
            check_both("int a = 1; float b = 2; bool t = true; (t ? a : b) = 2.5; (!t ? a : b) = 2.5; to_string(a) + \",\" + to_string(b)", "2,2.5", "each arm stores by its own table");
            check_both("class P { int x = 0; } P p = P(); array<int> arr = [0, 0]; int i = 1; bool t = false; (t ? p.x : arr[i]) = 7; ++(t ? p.x : arr[i]); (!t ? p.x : arr[i])--; to_string(p.x) + \",\" + to_string(arr[1])", "-1,8", "a conditional of a field and an element");
            check_both("int a = 0; int b = 0; int c = 0; bool t = false; bool u = true; (t ? a : (u ? b : c)) = 3; to_string(a) + to_string(b) + to_string(c)", "030", "a nested conditional");
            check_both("var log = \"\"; function cond() -> bool { log += \"c\"; return true; } function v() -> int { log += \"v\"; return 1; } int a = 0; int b = 0; (cond() ? a : b) = v(); log + to_string(a)", "vc1", "the right side, then the condition once");
            check_both("string s = \"a\"; string q = \"b\"; (true ? s : q) += \"c\"; s", "ac", "a conditional of strings");
            check_both("int a = 0; int b = 5; (a = b) = 7; to_string(a) + \",\" + to_string(b)", "7,5", "an assignment yields its left side");
            check_both("var log = \"\"; function v(int n) -> int { log += to_string(n); return n; } int a = 0; (a = v(1)) = v(2); log + \",\" + to_string(a)", "21,2", "the outer right side runs first");
            check_both("int a = 1; (a += 1) *= 3; to_string(a)", "6", "a compound yields its left side");
            check_both("int x = 1; ++x = 10; int y = 1; (++y) += 5; int z = 1; --z = 4; to_string(x) + \",\" + to_string(y) + \",\" + to_string(z)", "10,7,4", "a prefix increment yields its operand");
            check_both("array<int> a = [1]; ++a[0] = 9; int x = 0; auto r = (++x = 10); to_string(a[0]) + \",\" + to_string(r)", "9,10", "a prefix increment of an element");
            check_both("coroutine h() { int a = 0; int b = 0; ((yield 1) == null ? a : b) = 5; yield to_string(a) + to_string(b); } var co = h(); co.next(); co.next()", "50", "a yield inside the condition resumes once");
            auto [si, sv] = both("int g = 1; function f() -> int& { return g; } f() = \"s\";");
            check_eq(si, sv, "static rejection parity: a string into an int& result");
            check(si.find("ERROR: ") == 0 && si.find("cannot assign 'string'") != std::string::npos, "a string into an int& result is rejected at load: " + si);
            auto [ti, tv] = both("int a = 1; int b = 2; (true ? a : b) = \"s\";");
            check_eq(ti, tv, "static rejection parity: a string into a conditional of ints");
            check(ti.find("ERROR: ") == 0 && ti.find("cannot assign 'string'") != std::string::npos, "a string into a conditional of ints is rejected at load: " + ti);
        });

        test("a_non_lvalue_target_names_what_was_written", [this, both, both_in]() {
            // the text names the expression and why it holds no storage; strict reports it at load (lowercase), a program the
            // checker does not see raises it before anything runs, except a call's result, which only the call can tell
            const std::pair<const char*, const char*> rows[] = {
                {"5 = 1;", "Cannot assign to a literal"},
                {"int x = 1; cast<int>(x) = 2;", "Cannot assign to the result of cast<int>(x): it is a temporary value (cast<int&>(x) names the variable)"},
                {"int x = 1; x + 1 = 2;", "Cannot assign to the result of '+': it is a temporary value"},
                {"int x = 1; -x = 2;", "Cannot assign to the result of '-': it is a temporary value"},
                {"int x = 1; x++ = 5;", "Cannot assign to x++: it yields the old value"},
                {"array<int> a = [1]; a[0]-- = 5;", "Cannot assign to a[0]--: it yields the old value"},
                {"int a = 1; (true ? a : 5) = 2;", "Cannot assign to a conditional: its arm '5' names no storage"},
                {"function h() -> int { return 1; } h() = 2;", "Cannot assign to a call result: 'h' returns by value"},
                {"class P { int v = 0; } P() = P();", "Cannot assign to a call result: 'P' returns by value"},
                {"[1, 2] = 3;", "Cannot assign to an array literal"},
            };
            for (const auto& [src, text] : rows) {
                std::string lower = text;
                lower[0] = 'c';
                auto [si, sv] = both(src);
                check_eq(si, sv, std::string("static parity: ") + src);
                check(si.find("ERROR: ") == 0 && si.find(lower) != std::string::npos, std::string("load error for ") + src + ": " + si);
                const std::string caught = std::string("var c = \"\"; try { ") + src + " } catch (e) { c = e; } c";
                auto [oi, ov] = both_in(true, caught.c_str());
                check_eq(std::string(text), oi, std::string("interp run-time text: ") + src);
                check_eq(std::string(text), ov, std::string("vm run-time text: ") + src);
            }
            auto [ri, rv] = both_in(true, "var cnt = 0; function bump() { cnt++; return 5; } int x = 1; var out = \"\"; "
                                    "try { x++ = bump(); } catch (e) { out += e; } try { bump() = 1; } catch (e) { out += \"|\" + e; } out + \"|\" + to_string(cnt) + to_string(x)");
            check_eq(std::string("Cannot assign to x++: it yields the old value|Cannot assign to a call result: 'bump' returns by value|11"), ri, "interp: a non-lvalue raises before it runs, a call's result after");
            check_eq(ri, rv, "vm agrees");
        });

        test("a_reference_stores_by_its_declared_type", [this, check_both]() {
            // The reference's declared type governs the store (cast<int&>(v), int& r = v); var& and auto&
            // keep the referent's own rule
            check_both("var v = 1; cast<int&>(v) = 2.5; type_of(v) + \":\" + to_string(v)", "int:2", "cast<int&> over a var");
            check_both("var v = 1; int& r = v; r = 2.5; type_of(v) + \":\" + to_string(v)", "int:2", "int& over a var");
            check_both("var v = 1; int& r = v; r += 0.5; type_of(v) + \":\" + to_string(v)", "int:1", "a compound through int& over a var");
            check_both("var v = 1; int& r = v; v = 1.5; r++; type_of(v) + \":\" + to_string(v)", "int:2", "++ through int& over a var that now holds a float");
            check_both("var v = 1; cast<int&>(v) += 0.5; cast<int&>(v)++; type_of(v) + \":\" + to_string(v)", "int:2", "compound and ++ through cast<int&>");
            check_both("var a = [1, 2]; int& r = a[0]; r = 7.9; type_of(a[0]) + \":\" + to_string(a[0])", "int:7", "int& over an untyped element");
            check_both("var v = 1; auto& r = v; r = 2.5; var& q = v; q = \"s\"; type_of(v)", "string", "auto& and var& keep the var");
            check_both("class Cnt { int n = 0; function operator=(int x) { n = x * 10; } } var c = Cnt(); cast<Cnt&>(c) = 2; to_string(c.n)", "20", "a class's operator= through cast<Cnt&> over a var");
            check_both("function f() -> int { var v = 1; int& r = v; r = 3.5; return v; } to_string(f())", "3", "a function's slot local");
            check_both("var v = 1.5; float& r = v; r = 2; r += 1; var w = 1.5; float& s = w; s = 1; s++; type_of(v) + \":\" + to_string(v) + \",\" + type_of(w) + \":\" + to_string(w)", "float:3.0,float:2.0", "an int stored through float& takes the declared kind");
            check_both("var v = 1; int& r = v; r += 2; r *= 3; r++; type_of(v) + \":\" + to_string(v)", "int:10", "matching kinds take the storage's own path");
            // a function's `T& name` is decided at its stores from the name; one whose reference reaches another binding carries
            // the declared type with it, so every route agrees
            check_both("function f() -> string { var v = 1; int& q = v; q = 2.5; return type_of(v) + to_string(v); } f()", "int2", "a slot-resident int& over a var");
            check_both("function f() -> string { var v = 1; int& q = v; q += 0.5; v = 1.5; q++; return type_of(v) + to_string(v); } f()", "int2", "compound and ++ through a slot-resident int&");
            check_both("function g(var& p) { p = 2.5; } function f() -> string { var v = 1; int& q = v; g(q); return type_of(v) + to_string(v); } f()", "int2", "passed on to a var& parameter");
            check_both("function f() -> string { var v = 1; int& q = v; auto& r = q; r = 2.5; return type_of(v) + to_string(v); } f()", "int2", "aliased by auto&");
            check_both("function f() -> string { var v = 1; int& q = v; var h = [&]() { q = 2.5; }; h(); return type_of(v) + to_string(v); } f()", "int2", "captured by reference");
            check_both("function f() -> string { var v = 1; var w = 1; int& q = v; (true ? q : w) = 2.5; return type_of(v) + to_string(v); } f()", "int2", "an arm of a conditional target");
            check_both("function f() -> string { var v = 1.5; float& q = v; q = 2; q += 1; return type_of(v) + to_string(v); } f()", "float3.0", "a slot-resident float& over a var holding a float");
        });

        test("a_function_slot_takes_only_a_callable_or_null", [this, check_both]() {
            // a std::function's rule at every boundary, a null slot included: a callable or null, anything else raises naming `function`
            check_both("function cb; var v = 5; var c = \"\"; try { cb = v; c = \"stored\"; } catch (e) { c = e; } c", "Cannot assign int to 'cb' (declared function)", "a null slot");
            check_both("function cb = []() { return 1; }; var v = 5; var c = \"\"; try { cb = v; c = \"stored\"; } catch (e) { c = e; } c", "Cannot assign int to 'cb' (declared function)", "a slot holding a lambda");
            check_both("class K { function f; } K k = K(); var v = 5; var c = \"\"; try { k.f = v; c = \"stored\"; } catch (e) { c = e; } c", "Cannot assign int to field 'f' of K (declared function)", "a field");
            check_both("function g(function f) { return 1; } var v = 5; var c = \"\"; try { g(v); c = \"passed\"; } catch (e) { c = e; } c", "Cannot pass int to parameter 'f' of 'g' (declared function)", "a parameter");
            check_both("function h() -> string { function local; var w = 7; string r = \"\"; try { local = w; r = \"stored\"; } catch (e) { r = e; } return r; } h()", "Cannot assign int to 'local' (declared function)", "a function's local");
            check_both("function cb; cb = []() { return 3; }; cb = null; cb = []() { return 4; }; to_string(cb())", "4", "null resets and a callable stores again");
            check_both("function t = to_string; t(5)", "5", "a host function");
        });

        test("a_compound_takes_the_value_of_a_subscript_right_side", [this, check_both]() {
            // every compound target takes the value an element or map read yields (the read arrives as a reference)
            check_both("class C { float f = 1.0; } C o = C(); array<float> e = [0.5, 1.5]; o.f += e[1]; to_string(o.f)", "2.5", "a field");
            check_both("class S { static float s = 1.0; } array<float> e = [0.5, 1.5]; S::s += e[1]; to_string(S::s)", "2.5", "a static");
            check_both("float x = 1.0; array<float> e = [0.5, 1.5]; x += e[1]; to_string(x)", "2.5", "a variable");
            check_both("array<float> a = [1.0]; array<float> e = [0.5, 1.5]; a[0] += e[1]; to_string(a[0])", "2.5", "an element");
            check_both("array<float> a = [1.0]; var e = [0.5, 1.5]; auto& r = a[0]; r += e[1]; to_string(a[0])", "2.5", "a reference");
            check_both("float z = 1.0; array<float> e = [0.5, 1.5]; cast<float&>(z) += e[1]; to_string(z)", "2.5", "a reference cast");
            check_both("string s = \"a\"; array<string> n = [\"b\"]; s += n[0]; class T { string t = \"x\"; } T k = T(); k.t += n[0]; s + k.t", "abxb", "text appends");
            check_both("class C { int f = 1; } C o = C(); map<string, int> m = {\"k\": 4}; o.f -= m[\"k\"]; o.f *= m[\"k\"]; to_string(o.f)", "-12", "a map value into a field");
        });

        test("a_reference_binding_must_reference", [this, check_both, both]() {
            // A T& binds a var only while it holds T's kind (a class: T or a subclass); otherwise the binding
            // raises, in every binding form, naming the value's type and T&
            check_both("var v = 1; var c = \"\"; try { float& r = v; c = \"bound\"; } catch (e) { c = e; } c", "Cannot initialize 'r' (declared float&) with int", "float& over a var holding an int");
            check_both("var v = 2.5; var c = \"\"; try { int& r = v; c = \"bound\"; } catch (e) { c = e; } c", "Cannot initialize 'r' (declared int&) with float", "int& over a var holding a float");
            check_both("class Enemy { int hp = 1; } var v = 5; var c = \"\"; try { Enemy& e = v; c = \"bound\"; } catch (x) { c = x; } c", "Cannot initialize 'e' (declared Enemy&) with int", "a class reference over a var holding an int");
            check_both("function f() -> string { var v = 1; var c = \"\"; try { float& r = v; c = \"bound\"; } catch (e) { c = e; } return c; } f()", "Cannot initialize 'r' (declared float&) with int", "a function's local");
            check_both("var a = [1, 2]; var c = \"\"; try { float& r = a[0]; c = \"bound\"; } catch (e) { c = e; } c", "Cannot initialize 'r' (declared float&) with int", "an untyped element");
            check_both("function f(float& p) { p = 1; } var v = 1; var c = \"\"; try { f(v); c = \"bound\"; } catch (e) { c = e; } c", "Cannot pass int to parameter 'p' of 'f' (declared float&)", "a parameter");
            check_both("var v = 1; var c = \"\"; try { cast<float&>(v) = 2; c = \"bound\"; } catch (e) { c = e; } c", "Cannot cast 'int' to 'float&'", "a reference cast");
            check_both("var v = 1; int& r = v; r = 2.5; type_of(v) + to_string(v)", "int2", "a matching binding stores by T");
            check_both("class A { int a = 1; } class B : A { int b = 2; } var v = B(); A& r = v; type_of(r)", "B", "a subclass binds a base reference");
            check_both("var v = 1; auto& r = v; var& q = v; r = \"s\"; type_of(v)", "string", "auto& and var& bind any value");
            auto [si, sv] = both("int x = 1; float& r = x;");
            check_eq(si, sv, "static rejection parity: float& over an int");
            check(si.find("ERROR: ") == 0 && si.find("cannot initialize 'r' declared 'float&' with 'int'") != std::string::npos, "float& over an int is rejected at load: " + si);
        });

        test("a_reference_parameter_stores_by_its_declared_type", [this, check_both]() {
            // `void f(int& p)` over a var stores ints, decided at the stores from the parameter's declared type
            check_both("function f(int& p) { p = 2.5; } var v = 1; f(v); type_of(v) + \":\" + to_string(v)", "int:2", "a converting store");
            check_both("function f(int& p) { p += 0.5; p++; } var v = 1; f(v); type_of(v) + \":\" + to_string(v)", "int:2", "compound and ++");
            check_both("function f(int& p) { p = p + 1; p += 2; p++; } var v = 1; f(v); type_of(v) + \":\" + to_string(v)", "int:5", "matching kinds");
            check_both("function f(float& p) { p = 2; p += 1; } var v = 1.5; f(v); type_of(v) + \":\" + to_string(v)", "float:3.0", "an int stored through float& into a var");
            check_both("class Cnt { int n = 0; function operator=(int x) { n = x * 10; } } function f(Cnt& c) { c = 3; } var c = Cnt(); f(c); to_string(c.n)", "30", "a class's operator=");
            check_both("function f(var& p) { p = 2.5; } function g(auto& p) { p = \"s\"; } var v = 1; f(v); var w = 1; g(w); type_of(v) + \",\" + type_of(w)", "float,string", "var& and auto& keep the var");
            check_both("function g(var& q) { q = 2.5; } function f(int& p) { g(p); } var v = 1; f(v); type_of(v) + \":\" + to_string(v)", "int:2", "passed on to a var& parameter");
            check_both("class K { void set(int& p) { p = 7.9; } } var v = 1; K().set(v); type_of(v) + \":\" + to_string(v)", "int:7", "a method's parameter");
            check_both("function f(int& p) { p = p + 1; } function h() -> int { int n = 1; f(n); f(n); return n; } to_string(h())", "3", "a typed local argument");
            check_both("function f(int& p) { p = 2.5; } function h() -> string { var w = 1; f(w); return type_of(w) + to_string(w); } h()", "int2", "a var local argument");
            check_both("function f(int& p) { var g = [&]() { p = 4.5; }; g(); } var v = 1; f(v); type_of(v) + \":\" + to_string(v)", "int:4", "captured by reference");
        });

        test("typed_field_set_field_cpp_api", [this]() {
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute("class C { int v = 0; var w = 0; }  auto c = C();");
                auto obj = e->execute("c");
                auto instance = obj.as<jai::strong_ptr<jai::class_instance>>();
                check_not_null(instance.get());
                uint64_t v_id = detail::engine_access::symbolize(*e, "v");
                uint64_t w_id = detail::engine_access::symbolize(*e, "w");
                // Converts like '='
                instance->set_field(v_id, jai::script_value(4.7, e.get()));
                check_eq((int64_t)4, e->execute("c.v").as_int());
                // var fields stay dynamic through the C++ API
                instance->set_field(w_id, jai::script_value(std::string("s"), e.get()));
                check_eq(std::string("s"), e->execute("c.w").as<std::string>());
                // No conversion -> throws with the locals-identical text
                bool threw = false;
                try { instance->set_field(v_id, jai::script_value(std::string("bad"), e.get())); }
                catch (const std::exception& ex) {
                    threw = std::string(ex.what()).find("Cannot assign string to field 'v' of C (declared int)") != std::string::npos;
                }
                check(threw, "set_field rejects an inconvertible value");
            }
        });

        test("typed_field_json_roundtrip", [this, check_both]() {
            check_both(R"(
                class P { int hp = 0; float speed = 0.0; string name = ""; }
                auto p = P();
                p.hp = 42; p.speed = 1.5; p.name = "hero";
                var j = to_json(p);
                var q = from_json(j);
                type_of(q.hp) + ":" + to_string(q.hp) + "|" + type_of(q.speed) + ":" + to_string(q.speed) + "|" + q.name
            )", "int:42|float:1.5|hero", "JSON math::round-trip preserves typed fields");
            // A float JSON payload into an int field converts on the way in
            check_both(R"(
                class P { int hp = 0; }
                var q = from_json("{\"$type\": \"P\", \"$val\": {\"hp\": 7.9}}");
                type_of(q.hp) + ":" + to_string(q.hp)
            )", "int:7", "JSON float into int field truncates");
        });

        test("compound_fused_mixed_type_bails_to_conversion", [this, check_both]() {
            // VM: x += f * 2.0 compiles to op_compound_fused; the int-only fast path must
            // bail (float operand) and the general path converts each iteration (2.5 -> 2)
            check_both(R"(
                function h() {
                    int x = 0;
                    float f = 1.25;
                    for (int i = 0; i < 4; ++i) { x += f * 2.0; }
                    return type_of(x) + ":" + to_string(x);
                }
                h()
            )", "int:8", "fused mixed bails and converts");
            // Pure int fused fast path is untouched
            check_both(R"(
                function h() {
                    int x = 0;
                    for (int i = 0; i < 5; ++i) { x += i * 2; }
                    return to_string(x);
                }
                h()
            )", "20", "fused int fast path");
        });

        // ===== TYPED CONTAINER BOUNDARIES ARE ELEMENT-CHECKED =====
        // An array/map value crossing into a concretely-typed container binding (decl,
        // assign, field write) validates every element with push's rules: exact match
        // passes, int<->float converts, anything else raises. The stored value is
        // retagged to the declared type so later pushes stay enforced. Pre-fix, the
        // same-base fast path admitted ANY array into array<T> — a string could sit
        // behind an array<int> tag (silently breaking the parallel all-primitive proof).

        test("typed_array_decl_validates_elements", [this, both, check_both]() {
            auto [i1, v1] = both("var a = [\"s\"]; array<int> b = a; b[0]");
            check_eq(i1, v1, "string-into-array<int> parity");
            check(i1.find("ERROR") == 0, "laundered string array into array<int> raises");
            auto [i2, v2] = both("var a = [1, \"s\", 3]; array<int> b = a; b.size()");
            check_eq(i2, v2, "mixed-into-array<int> parity");
            check(i2.find("ERROR") == 0, "mixed elements into array<int> raises");
            check_both("var a = [1.5, 2.5]; array<int> b = a; type_of(b[0]) + \":\" + to_string(b[0]) + \",\" + to_string(b[1])",
                       "int:1,2", "float elements truncate into array<int>");
            check_both("var a = [1, 2]; array<float> b = a; type_of(b[0]) + \":\" + to_string(b[0])",
                       "float:1.0", "int elements widen into array<float>");
            check_both("var a = [1.5]; array<int> b = a; type_of(a[0]) + \":\" + to_string(a[0])",
                       "float:1.5", "source unchanged after converting decl");
        });

        test("typed_array_literal_init_converts_elements", [this, check_both]() {
            // decl contract (see visit_variable_decl): typed decls convert their
            // initializer exactly like assignment — element payloads included
            check_both("array<float> a = [1, 2, 3]; type_of(a[0]) + \":\" + to_string(a[0])",
                       "float:1.0", "literal ints become float payloads");
            check_both("array<int> a = [1.5]; type_of(a[0]) + \":\" + to_string(a[0])",
                       "int:1", "literal float truncates into array<int>");
        });

        test("typed_array_assign_validates_elements", [this, both, check_both]() {
            auto [i1, v1] = both("array<int> b = [1]; var a = [\"s\"]; b = a; b[0]");
            check_eq(i1, v1, "assign parity");
            check(i1.find("ERROR") == 0, "assigning string array into array<int> raises");
            check_both("array<int> b = [1]; var a = [2.5]; b = a; to_string(b[0])", "2", "assign converts");
        });

        test("typed_array_field_validates_elements", [this, both, check_both]() {
            auto [i1, v1] = both("class C { array<int> f = []; } auto c = C(); var a = [\"s\"]; c.f = a; c.f[0]");
            check_eq(i1, v1, "field parity");
            check(i1.find("ERROR") == 0, "string array into array<int> field raises");
            // after a valid store the field's declared tag governs pushes (retag at the boundary)
            auto [i2, v2] = both("class C { array<int> f = []; } auto c = C(); var a = [1, 2]; c.f = a; c.f.push(\"s\"); c.f.size()");
            check_eq(i2, v2, "field push-after-store parity");
            check(i2.find("ERROR") == 0, "push string into stored array<int> field raises");
            check_both("class C { array<int> f = []; } auto c = C(); var a = [2.5]; c.f = a; to_string(c.f[0])", "2", "field store converts");
        });

        test("typed_map_decl_validates_values", [this, both, check_both]() {
            auto [i1, v1] = both("var m = {\"k\": \"s\"}; map<string, int> t = m; t.size()");
            check_eq(i1, v1, "map parity");
            check(i1.find("ERROR") == 0, "string values into map<string,int> raises");
            check_both("var m = {\"k\": 2.5}; map<string, int> t = m; to_string(t[\"k\"])", "2", "map value converts");
        });

        test("typed_array_var_ref_keeps_enforcement", [this, both]() {
            // pin: var& shares and PRESERVES the array's tag — stores through the alias
            // stay typed (the demote-on-stamp design leans on this)
            auto [i1, v1] = both("array<int> b = [1]; var& r = b; r.push(\"s\"); b.size()");
            check_eq(i1, v1, "var& push parity");
            check(i1.find("ERROR") == 0, "push through var& alias stays enforced");
        });

        // ===== TYPED FLAT ARRAY STORAGE (docs/site/guide/09-containers-strings.html "Representation follows the declared type") =====
        // array<int>/array<float> hold raw primitive buffers by their declared type (the
        // representation, not a mode). The contract: typed
        // storage is observationally invisible - every snippet runs on both backends and
        // the outputs pin the plain-semantics answer.

        auto quad = [](const char* src) {
            std::string out[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execution_budget(0);
                jai::stdlib::register_all(e);
                try { out[idx] = e->execute(src).to_string(); }
                catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                ++idx;
            }
            return std::array<std::string, 2>{out[0], out[1]};
        };
        auto check_quad = [this, quad](const char* src, const std::string& expected, const char* what) {
            auto r = quad(src);
            check_eq(expected, r[0], std::string("interp/typed: ") + what);
            check_eq(expected, r[1], std::string("vm/typed: ") + what);
        };
        auto check_quad_error = [this, quad](const char* src, const char* what) {
            auto r = quad(src);
            check_eq(r[0], r[1], std::string("backend parity: ") + what);
            check(r[0].find("ERROR") == 0, std::string("raises: ") + what);
        };

        test("object_buffer_behaviour", [check_quad, check_quad_error]() {
            // array<UserType> is an object slot buffer (kind obj); every row below is the
            // exact pre-change output of both backends (representation change only)
            const std::string K = "class K { int a = 1; K() {} K(int v) { a = v; } string f() { return \"k\" + to_string(a); } } ";
            auto q = [&](const char* tail) { return K + tail; };
            check_quad(q("array<K> ks = [K(1), K(2)]; to_string(ks[0].a) + \",\" + to_string(ks[1].a)").c_str(), "1,2", "reads");
            check_quad(q("array<K> ks = [K(1), K(2)]; ks[1].a = 9; to_string(ks[1].a)").c_str(), "9", "field write through the element");
            check_quad(q("array<K> ks = [K(1), K(2)]; ks[0] = K(5); to_string(ks[0].a)").c_str(), "5", "element store");
            check_quad(q("array<K> ks = [K(1), K(2)]; ks.push(K(3)); to_string(ks.size()) + \":\" + to_string(ks[2].a)").c_str(), "3:3", "push");
            check_quad(q("array<K> ks = [K(1), K(2)]; auto c = ks; c[0].a = 42; to_string(ks[0].a) + \",\" + to_string(c[0].a)").c_str(), "1,42", "copy is a deep copy");
            check_quad(q("array<K> ks = [K(1), K(2)]; int s = 0; for (auto k : ks) { s += k.a; } to_string(s)").c_str(), "3", "range-for by value reads");
            check_quad(q("array<K> ks = [K(1), K(2)]; for (auto k : ks) { [[warn_mode]] k.a = 77; } to_string(ks[0].a + ks[1].a)").c_str(), "3", "range-for by value mutates copies");
            check_quad(q("array<K> ks = [K(1), K(2)]; for (auto& k : ks) { k.a = k.a * 10; } to_string(ks[0].a + ks[1].a)").c_str(), "30", "range-for by reference writes through");
            check_quad(q("array<K> ks = [K(1), K(2)]; ks.reverse(); to_string(ks[0].a)").c_str(), "2", "reverse");
            check_quad(q("array<K> ks = [K(1), K(2)]; ks[1].f()").c_str(), "k2", "method call on an element");
            check_quad(q("array<K> ks = [K(1)]; type_of(ks) + \"|\" + type_of(ks[0])").c_str(), "array|K", "type_of");
            check_quad(q("array<K> ks = [K(1), K(2)]; to_json(ks)").c_str(), "[{\"$type\":\"K\",\"$val\":{\"a\":1}},{\"$type\":\"K\",\"$val\":{\"a\":2}}]", "json");
            check_quad(q("array<K> a = [K(1)]; array<K> b = a; array<K> c = [K(2)]; to_string(a == b) + \",\" + to_string(a == c) + \",\" + to_string(same_object(a, b))").c_str(), "true,false,false", "equality is memberwise (a copy equals its source, identity via same_object)");
            check_quad(q("shared_ptr<K> sp = K(7); array<K> ks = [sp]; sp.a = 8; to_string(ks[0].a) + \"|\" + type_of(ks[0])").c_str(), "7|K", "a handle stored into a class element copies its instance");
            check_quad(q("shared_ptr<K> sp = K(7); array<shared_ptr<K>> ks = [sp]; auto c = ks; c[0].a = 9; to_string(ks[0].a) + \",\" + to_string(sp.a)").c_str(), "9,9", "copying an array keeps a shared element shared (C++ copy semantics)");
            check_quad(q("array<K> ks = [K(1), K(2), K(3)]; to_string(array::filter(ks, [](K k) { return k.a > 1; }).size())").c_str(), "2", "filter");
            check_quad(q("array<K> ks = [K(1)]; ks.clear(); ks.push(K(4)); to_string(ks.size()) + \":\" + to_string(ks[0].a)").c_str(), "1:4", "clear then push");
            check_quad(q("array<K> ks = [K(1)]; var v = ks; string r = \"\"; try { v.push(5); } catch (e) { r = e; } v.push(K(2)); v = 5; r + \"|\" + type_of(v)").c_str(), "Cannot push 'int' to array<K>|int", "a var copy keeps array<K>, the var stays a var");
            check_quad(q("var v = [K(1), K(2)]; array<K> ks = v; v[0].a = 50; to_string(ks[0].a) + \",\" + to_string(ks.size())").c_str(), "1,2", "array<K> from a dynamic array copies");
            check_quad(q("function total(array<K> xs) -> int { int s = 0; for (auto x : xs) { s += x.a; } xs[0].a = 100; return s; } array<K> ks = [K(1), K(2)]; to_string(total(ks)) + \",\" + to_string(ks[0].a)").c_str(), "3,1", "by-value parameter");
            check_quad(q("function bump(array<K>& xs) { xs[0].a = 100; xs.push(K(9)); } array<K> ks = [K(1)]; bump(ks); to_string(ks[0].a) + \",\" + to_string(ks.size())").c_str(), "100,2", "by-reference parameter");
            check_quad(q("function mk() -> array<K> { array<K> r = [K(3), K(4)]; return r; } auto got = mk(); to_string(got[1].a) + \"|\" + type_of(got)").c_str(), "4|array", "returned array");
            check_quad(q("array<array<K>> grid = [[K(1)], [K(2), K(3)]]; to_string(grid[1][1].a)").c_str(), "3", "nested arrays stay dynamic outside");
            check_quad(q("array<K> ks = []; ks.push(K(2)); to_string(ks.size()) + \":\" + to_string(ks[0].a)").c_str(), "1:2", "empty then push");
            check_quad(q("coroutine int walk(array<K> xs) { for (auto x : xs) { yield x.a; } return 0; } array<K> ks = [K(5), K(6)]; auto h = walk(ks); to_string(h.next()) + \",\" + to_string(h.next())").c_str(), "5,6", "coroutine over an object buffer");
            check_quad(q("array<K> ks = [K(1), K(2)]; int acc = 0; for (int i = 0; i < 1000; i++) { acc += ks[i & 1].a; } to_string(acc)").c_str(), "1500", "hot element field reads");
            check_quad_error(q("class J { int b = 0; } array<K> ks = [K()]; ks.push(J())").c_str(), "another class does not push");
            check_quad_error(q("array<K> ks = [K()]; ks.push(5)").c_str(), "an int does not push");
            check_quad_error(q("class J { int b = 0; } array<K> x = [J()]").c_str(), "another class does not declare");
            check_quad_error(q("class J { int b = 0; } array<K> ks = [K()]; ks[0] = J()").c_str(), "another class does not store");
            check_quad(q("class D : K { } array<D> ds = [D()]; array<K> ks = ds; to_string(ks.size()) + \",\" + to_string(ks[0].a)").c_str(), "1,1", "array<Derived> converts to array<Base> (subclasses admitted)");
        });

        test("type_of_names_the_held_class_through_casts", [check_quad]() {
            // type_of answers what reflect::type_name answers - the INSTANCE's class, never
            // the value tag. cast<Base>(d) copies rather than slices, so all four spellings
            // still hold a Derived.
            check_quad("class Base { int a = 1; } class Derived : Base { int b = 2; } Derived d = Derived(); auto v = cast<Base>(d); "
                       "type_of(v) + \",\" + type_of(cast<Base&>(d)) + \",\" + type_of(cast<shared_ptr<Base>>(d)) + \",\" + type_of(d)",
                       "Derived,Derived,Derived,Derived", "value / reference / handle casts and the variable");
        });

        test("map_behaviour_battery", [check_quad, check_quad_error]() {
            // Every row is the pre-change output of BOTH backends: typed map storage
            // must keep them
            // byte-identical, and the two EDGE rows are undecided, not behaviour to copy
            check_quad("var m = {\"a\": 1, \"b\": 2}; to_string(m[\"a\"] + m[\"b\"]) + \",\" + to_string(m.size())", "3,2", "literal read + size");
            check_quad("map<string,int> m = {\"a\": 1, \"b\": 2}; to_string(m[\"b\"]) + \"|\" + type_of(m)", "2|map", "typed declaration");
            check_quad("map<int,string> m = {3: \"c\", 1: \"a\", 2: \"b\"}; string s = \"\"; for (auto kv : m) { s += kv.second; } s", "abc", "int keys iterate in key order");
            check_quad("var m = {\"zeta\": 1, \"alpha\": 2, \"mid\": 3}; string s = \"\"; for (auto kv : m) { s += kv.first + \",\"; } s", "alpha,mid,zeta,", "string keys iterate in byte order");
            check_quad("var m = {2: \"i\", \"x\": \"s\", 1.5: \"f\", true: \"b\"}; string s = \"\"; for (auto kv : m) { s += type_of(kv.first) + \",\"; } s", "int,float,string,bool,", "mixed keys order by type rank");
            check_quad("map<string,int> m = {}; m[\"k\"] = 5; m[\"k\"] = m[\"k\"] + 1; to_string(m[\"k\"]) + \",\" + to_string(m.size())", "6,1", "store and re-store");
            check_quad("map<string,int> m = {\"a\": 1}; string r = \"\"; try { int x = m[\"nope\"]; } catch (e) { r = e; } r + \",\" + to_string(m.size())", "Key \"nope\" not found in map,1", "a missing key raises, no auto-insert");
            check_quad("map<string,int> m = {\"a\": 1}; to_string(m.contains(\"a\")) + \",\" + to_string(m.contains(\"b\"))", "true,false", "contains");
            check_quad("map<string,int> m = {\"a\": 1, \"b\": 2}; m.erase(\"a\"); to_string(m.size()) + \",\" + to_string(m.contains(\"a\"))", "1,false", "erase");
            check_quad("map<string,int> m = {\"b\": 2, \"a\": 1}; to_json(m.keys()) + to_json(m.values())", "[\"a\",\"b\"][1,2]", "keys and values in order");
            check_quad("map<string,int> m = {\"a\": 1}; auto c = m; c[\"a\"] = 9; to_string(m[\"a\"]) + \",\" + to_string(c[\"a\"])", "1,9", "copy is a deep copy");
            check_quad("class K { int a = 1; K() {} K(int v) { a = v; } } map<string,K> m = {\"x\": K(4)}; m[\"x\"].a = 7; to_string(m[\"x\"].a)", "7", "field write through an entry");
            check_quad("class K { int a = 1; K() {} K(int v) { a = v; } } map<string,K> m = {\"x\": K(4)}; auto c = m; c[\"x\"].a = 9; to_string(m[\"x\"].a) + \",\" + to_string(c[\"x\"].a)", "4,9", "object values clone on copy");
            check_quad("map<string,int> m = {\"n\": 1}; m[\"n\"] += 10; to_string(m[\"n\"])", "11", "compound store on an entry");
            check_quad("map<string,int> m = {\"n\": 1}; auto& r = m[\"n\"]; r = 42; to_string(m[\"n\"])", "42", "reference to an entry writes through");
            check_quad("var m = {\"inner\": {\"x\": 1}}; m[\"inner\"][\"x\"] = 2; to_string(m[\"inner\"][\"x\"])", "2", "nested map write");
            check_quad("map<string,int> m = {\"b\": 2, \"a\": 1}; to_json(m)", "{\"a\":1,\"b\":2}", "json in key order");
            check_quad("map<float,int> m = {1.5: 1, 0.5: 2}; string s = \"\"; for (auto kv : m) { s += to_string(kv.second); } s", "21", "float keys order by value");
            check_quad("map<string,int> m = {\"a\": 1, \"b\": 2}; for (auto& kv : m) { kv.second = kv.second * 10; } to_string(m[\"a\"] + m[\"b\"])", "30", "reference range-for writes values");
            check_quad("map<string,int> m = {}; to_string(m.size()) + \",\" + to_string(m.empty())", "0,true", "empty");
            check_quad("function total(map<string,int> mm) -> int { int s = 0; for (auto kv : mm) { s += kv.second; } [[warn_mode]] mm[\"z\"] = 100; return s; } map<string,int> m = {\"a\": 1, \"b\": 2}; to_string(total(m)) + \",\" + to_string(m.size())", "3,2", "by-value parameter");
            check_quad("function grow(map<string,int>& mm) { mm[\"z\"] = 100; } map<string,int> m = {\"a\": 1}; grow(m); to_string(m.size()) + \",\" + to_string(m[\"z\"])", "2,100", "by-reference parameter");
			check_quad("map<string,int> m = {\"a\": 1}; var v = m; bool rejected = false; try { v[5] = 2; } catch (e) { rejected = true; } string before = to_string(rejected) + \",\" + to_string(v.size()) + \",\" + type_of(v); v = 7; before + \"|\" + to_string(v)", "true,1,map|7", "var map keeps content constraints but permits rebinding");
            check_quad("var v = {\"a\": 1, \"b\": 2}; map<string,int> m = v; v[\"a\"] = 50; to_string(m[\"a\"]) + \",\" + to_string(m.size())", "1,2", "typed map from a dynamic map copies");
            check_quad("map<string,int> a = {\"x\": 1}; map<string,int> b = a; to_string(a == b) + \",\" + to_string(same_object(a, b))", "true,false", "equality is memberwise (a copy equals its source, identity via same_object)");
            check_quad("map<string,int> m = {\"px\": 3}; int acc = 0; for (int i = 0; i < 1000; i++) { acc += m[\"px\"]; } to_string(acc)", "3000", "constant-key reads");
            check_quad("map<string,int> m = {}; for (int i = 0; i < 50; i++) { m[\"k\" + to_string(i)] = i; } int acc = 0; for (int i = 0; i < 50; i++) { acc += m[\"k\" + to_string(i)]; } to_string(acc) + \",\" + to_string(m.size())", "1225,50", "dynamic-key stores and reads");
            check_quad("map<string,int> m = {\"a\": 1}; m.clear(); m[\"b\"] = 2; to_string(m.size()) + \",\" + to_string(m.contains(\"a\"))", "1,false", "clear then store");
            check_quad("map<string,array<int>> m = {\"a\": [1, 2]}; m[\"a\"].push(3); to_string(m[\"a\"].size()) + \",\" + to_string(m[\"a\"][2])", "3,3", "array value mutates through the entry");
            check_quad("coroutine int walk(map<string,int> mm) { for (auto kv : mm) { yield kv.second; } return 0; } map<string,int> m = {\"a\": 5, \"b\": 6}; auto h = walk(m); to_string(h.next()) + \",\" + to_string(h.next())", "5,6", "coroutine over a map");
            check_quad_error("map<string,int> m = {}; m[5] = 1", "key type enforced on a direct store");
            check_quad_error("map<string,int> m = {}; m[\"k\"] = \"str\"", "value type enforced on store");
            check_quad_error("map<string,int> m = {\"a\": \"s\"}", "value type enforced on declaration");
            check_quad("map<string,int> m = {\"n\": 1}; m[\"n\"]++; to_string(m[\"n\"])", "2", "increment of a map entry (works like += 1)");
        });

        test("instance_plan_battery", [check_quad, check_quad_error]() {
            // create_instance seeds every default through a per-class plan (slot order, post-
            // conversion) and a literal-only chain skips the initializer re-run; every row is
            // the exact pre-plan output of both backends.
            check_quad("class A { int a = 1; float f = 2; string s = \"3\"; bool b = 1; } A x = A(); to_string(x.a) + \",\" + to_string(x.f) + \",\" + x.s + \",\" + to_string(x.b)", "1,2.0,3,true", "literal defaults convert to the declared types");
            check_quad("class P { int a = 1; int b = 2; } class C : P { int b = 20; int c = 3; } C x = C(); to_string(x.a) + \",\" + to_string(x.b) + \",\" + to_string(x.c)", "1,20,3", "child default overrides the parent default");
            check_quad("class P { int a = 0; P(int v) { a = v; } } class C : P { int a2 = 7; C() : super(5) {} } C x = C(); to_string(x.a) + \",\" + to_string(x.a2)", "5,7", "parent ctor body writes a field the plan seeded");
            check_quad("class P { int a = 1; P() { a = 9; } } class C : P { int a = 4; C() : super() {} } C x = C(); to_string(x.a)", "4", "child re-declares a field the parent ctor wrote: child initializer wins after the parent body");
            check_quad("class D { int a = 1; int b = 2; D() : this(10) {} D(int v) { a = v; } } D x = D(); to_string(x.a) + \",\" + to_string(x.b)", "10,2", "this(...) delegation keeps the target ctor as the field authority");
            check_quad("int g = 5; class N { int a = g + 1; int b = 2; } N x = N(); g = 100; N y = N(); to_string(x.a) + \",\" + to_string(y.a) + \",\" + to_string(y.b)", "6,101,2", "a non-literal initializer re-evaluates per construction");
            check_quad("class R { int a = 1; } R x = R(); class R { int a = 2; int z = 3; } R y = R(); to_string(x.a) + \",\" + to_string(y.a) + \",\" + to_string(y.z)", "1,2,3", "a reload rebuilds the plan (new default, new slot)");
            check_quad("class P { int a = 1; } class C : P { int c = 2; } C x = C(); class P { int a = 50; } C y = C(); to_string(x.a) + \",\" + to_string(y.a)", "1,50", "a PARENT reload invalidates the child's plan");
            check_quad("class V { float x = 0; float y = 0; V(float px, float py) { x = px; y = py; } } float s = 0.0; for (int i = 0; i < 100; ++i) { auto v = V(1.0, 2.0); s += v.x + v.y; } to_string(s)", "300.0", "the hot ctor shape across many constructions");
            check_quad("class K { int a = 1; shared_ptr<K> next; } K x = K(); to_string(x.a) + \",\" + to_string(x.next == null)", "1,true", "handle-typed field defaults to a typed null");
            check_quad("class E { int a = 1; int b; } E x = E(); to_string(x.a) + \",\" + to_string(x.b)", "1,0", "an uninitialized typed field is its type's zero");
            check_quad("class M { var m = [1, 2]; } M x = M(); M y = M(); x.m.push(3); to_string(x.m.size()) + \",\" + to_string(y.m.size())", "3,2", "container defaults are per-instance copies");
            check_quad("class S { string name = \"n\"; int hp = 3; } S a = S(); S b = S(); a.name = \"q\"; a.name + b.name + to_string(a.hp + b.hp)", "qn6", "string defaults are independent");
        });

        test("bare_field_store_then_read_in_loops_battery", [check_quad]() {
            // The Foundry field-write shape: a bare field store followed by a bare read of the
            // same field as an operand in the same method frame, across loop iterations. Any
            // inline cache on the operand path must observe the store (the vm once read a stale
            // or wrong cell here while every other test stayed green).
            const std::string BW = "class BW { int a = 0; int b = 0; ";
            check_quad((BW + "int run() { for (auto k = 0; k < 20; ++k) { for (auto i = 0; i < 1000; ++i) { a = i; b = a + i; } } return a + b; } } BW w = BW(); to_string(w.run())").c_str(), "2997", "store then binary read");
            check_quad((BW + "string run() { string log = \"\"; for (auto i = 0; i < 3; ++i) { a = i; b = a + i; log += to_string(a) + \",\" + to_string(b) + \";\"; } return log; } } BW w = BW(); w.run()").c_str(), "0,0;1,2;2,4;", "per-iteration values");
            check_quad((BW + "int run() { int hits = 0; for (auto i = 0; i < 10; ++i) { a = i; if (a < 5) { hits += 1; } if (i >= a) { hits += 10; } } return hits; } } BW w = BW(); to_string(w.run())").c_str(), "105", "store then fused compare reads");
            check_quad((BW + "int run() { for (auto i = 0; i < 4; ++i) { a = i; b = a * 2; a = b + 1; } return a * 100 + b; } } BW w = BW(); to_string(w.run())").c_str(), "706", "two fields written and read alternately");
            check_quad((BW + "int run() { int s = 0; for (auto i = 0; i < 5; ++i) { a += i; s += a; } return s; } } BW w = BW(); to_string(w.run())").c_str(), "20", "compound store then read");
            check_quad((BW + "int run() { for (auto i = 0; i < 3; ++i) { a = i; } return a; } } BW w = BW(); BW v = BW(); to_string(w.run()) + \",\" + to_string(v.run()) + \",\" + to_string(w.a)").c_str(), "2,2,2", "two instances through one site");
        });

        test("method_ic_exact_type_memo_battery", [check_quad, check_quad_error]() {
            // A single-overload typed method armed in the call-site IC: exact-typed arguments take
            // the memo (no resolver run), convertible ones re-resolve, rejected ones raise the
            // parameter's text, and a retyping reload invalidates the memo through the epoch.
            const std::string M = "class M { int acc = 0; int add(int v) { acc += v; return acc; } float scale(float f, int k) { return f * k; } } M m = M(); ";
            auto q = [&](const char* tail) { return M + tail; };
            check_quad(q("int t = 0; for (int i = 0; i < 5; ++i) { t = m.add(i); } to_string(t)").c_str(), "10", "exact int args hit the memo across the loop");
            check_quad(q("m.add(1); m.add(2.9); to_string(m.acc)").c_str(), "3", "a float into int truncates on the resolver path after a memo hit (one table)");
            check_quad(q("m.add(1); var x = \"x\"; var caught = \"\"; try { m.add(x); } catch (e) { caught = e; } caught").c_str(), "Cannot pass string to parameter 'v' of 'add' (declared int)", "a var-sourced rejected argument raises the parameter's text after memo hits");
            check_quad(q("to_string(m.scale(2.0, 3)) + \",\" + to_string(m.scale(2, 3))").c_str(), "6.0,6.0", "two-parameter memo: exact then int-to-float conversion");
            check_quad(q("m.add(1); m.add(2); class M { int acc = 0; int add(float v) { acc += cast<int>(v * 10.0); return acc; } } m.add(3); to_string(m.acc)").c_str(), "33", "a retyping reload invalidates the memo (the new overload runs)");
            check_quad(q("m.add(1); class M { int acc = 0; int add(int v) { acc += v; return acc; } int add(string s) { return -1; } } to_string(m.add(2)) + \",\" + to_string(m.add(\"s\"))").c_str(), "3,-1", "a reload adding an overload leaves the single-overload memo behind");
            check_quad_error(q("var n = null; m.add(n)").c_str(), "var-sourced null into a typed int parameter raises identically");
        });

        test("null_compare_battery", [this, check_quad]() {
            // Null compares (`x == null` / `x != null`) on both backends: every row is the
            // exact pre-lane output of both backends. The vm branch lane answers a null
            // operand from storage (handle_equal's verdict); weak_ptr, cpp-bound and
            // operator-overloaded engines keep the general path
            const std::string P = "class N { int a = 1; shared_ptr<N> next; N other = null; } class E { int v = 0; bool operator==(E o) { return true; } bool operator!=(E o) { return true; } } class L { shared_ptr<L> left; int f() { if (left == null) { return 1; } return 2; } int g() { if (left != null) { return 1; } return 2; } } auto n = N(); auto e = E(); auto l = L(); var v; int i = 5; float fl = 1.5; string s = \"s\"; char c = 'c'; bool b = true; array<var> arr = [1]; map<string,int> m = {\"a\": 1}; function fn() { return 1; } auto sp = shared_ptr<N>(); weak_ptr<N> w = sp; ";
            auto q = [&](const char* tail) { return P + tail; };
            check_quad(q("n == null").c_str(), "false", "object vs null");
            check_quad(q("n != null").c_str(), "true", "object != null");
            check_quad(q("n.next == null").c_str(), "true", "unset shared_ptr field");
            check_quad(q("n.other == null").c_str(), "true", "typed-null object field");
            check_quad(q("null == null").c_str(), "true", "null == null");
            check_quad(q("null != null").c_str(), "false", "null != null");
            check_quad(q("v == null").c_str(), "true", "unset var");
            // a typed slot that never holds null: strict rejects the compare (C++ `5 == nullptr`); in a warn region the
            // run time's verdict stands on both backends
            const auto check_runtime = [this, &q](const char* tail, const std::string& expected, const char* what) {
                const std::string src = q(tail);
                check_true(jai::foundry::make_default_engine()->check(src).has_errors(), std::string("strict rejects: ") + what);
                for (bool use_vm : {false, true}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    e->execution_budget(0);
                    jai::stdlib::register_all(e);
                    check_eq(expected, e->execute("[[warn_mode]]; " + src).to_string(), std::string(use_vm ? "vm/warn region: " : "interp/warn region: ") + what);
                }
            };
            check_runtime("i == null", "false", "int vs null");
            check_runtime("fl != null", "true", "float != null");
            check_runtime("s == null", "false", "string vs null");
            check_runtime("c == null", "false", "char vs null (no promotion)");
            check_runtime("b != null", "true", "bool != null");
            check_runtime("arr == null", "false", "array vs null");
            check_runtime("m != null", "true", "map != null");
            check_quad(q("fn == null").c_str(), "false", "function vs null");
            check_quad(q("e == null").c_str(), "false", "class operator== is NOT consulted for a null operand");
            check_quad(q("e != null").c_str(), "true", "class operator!= is NOT consulted for a null operand");
            check_quad(q("null == e").c_str(), "false", "null on the left");
            check_quad(q("l.f()").c_str(), "1", "method body: if (field == null) - the branch lane");
            check_quad(q("l.g()").c_str(), "2", "method body: if (field != null)");
            check_quad(q("l.left = shared_ptr<L>(); l.f()").c_str(), "2", "set shared_ptr field: == null false");
            check_quad(q("l.left = shared_ptr<L>(); l.g()").c_str(), "1", "set shared_ptr field: != null true");
            check_quad(q("sp == null").c_str(), "false", "live shared_ptr");
            check_quad(q("w == null").c_str(), "false", "weak_ptr to a live object (general path)");
            check_quad(q("w != null").c_str(), "true", "live weak_ptr != null");
            check_quad(q("sp = null; w == null").c_str(), "true", "expired weak_ptr reads as null");
            check_quad(q("sp = null; w != null").c_str(), "false", "expired weak_ptr != null");
            check_quad(q("n.next = shared_ptr<N>(); n.next == null").c_str(), "false", "assigned shared_ptr field");
            check_quad(q("n.next = shared_ptr<N>(); n.next != null").c_str(), "true", "assigned shared_ptr field != null");
            check_quad(q("array<var> xs = [N(), null, N()]; int cnt = 0; for (auto x : xs) { if (x != null) { cnt += 1; } } cnt").c_str(), "2", "loop guard != null over mixed elements");
            check_quad(q("array<var> xs = [N(), null, N()]; int cnt = 0; for (auto x : xs) { if (x == null) { cnt += 1; } } cnt").c_str(), "1", "loop guard == null over mixed elements");
            check_quad(q("bool bb = (n == null); bb").c_str(), "false", "value position (binary, not the branch lane)");
            check_quad(q("bool bc = (n.next != null); bc").c_str(), "false", "value position on an unset field");
            check_quad(q("string r = \"\"; try { throw \"boom\"; } catch (err) { r = to_string(err == null) + \",\" + to_string(n == null); } r").c_str(), "false,false", "inside a catch (catch-var gate keeps the general path)");
            check_quad(q("(null == null) == true").c_str(), "true", "nested compare");
        });

        test("null_compare_defers_to_global_operator_overload", [this]() {
            // A host-registered global `==` latches the custom-binary-op gate: the vm lane
            // must step aside so the overload sees `obj == null` exactly like the interpreter
            std::string out[2];
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(e);
                e->add_function("==", [](const jai::script_value&, const jai::script_value&) { return true; });
                out[use_vm ? 1 : 0] = e->execute("class N { int a = 1; } auto n = N(); int hits = 0; if (n == null) { hits += 1; } if (null == null) { hits += 10; } to_string(hits) + \",\" + to_string(n == null)").to_string();
            }
            check_eq(out[0], out[1], "backends agree under a global == overload");
        });

        test("truthiness_battery", [check_quad]() {
            // Object truthiness (`if (x)` / `if (!x)` / ternary) on both backends: rows are
            // the exact pre-change outputs. A class with no to_bool() anywhere in its chain
            // answers from a class-level memo; `!e` in branch position compiles to an
            // inverted jump on the vm (same is_truthy, one op fewer)
            const std::string P = "class T { int a = 1; } class B { int a = 1; bool to_bool() { return a > 5; } } class D : B { int b = 2; } class Q { bool to_bool() { throw \"qbang\"; } } auto t = T(); auto b = B(); auto d = D(); auto sp = shared_ptr<T>(); shared_ptr<T> e; var v; int i0 = 0; float f0 = 0.0; string s0 = \"\"; array<var> a0 = []; auto qq = Q(); ";
            auto q = [&](const char* tail) { return P + tail; };
            check_quad(q("t ? 1 : 2").c_str(), "1", "plain object is truthy");
            check_quad(q("!t").c_str(), "false", "!object");
            check_quad(q("b ? 1 : 2").c_str(), "2", "to_bool() decides (false)");
            check_quad(q("!b").c_str(), "true", "!object with to_bool() false");
            check_quad(q("b.a = 9; b ? 1 : 2").c_str(), "1", "to_bool() decides (true)");
            check_quad(q("b.a = 9; !b").c_str(), "false", "!object with to_bool() true");
            check_quad(q("d ? 1 : 2").c_str(), "2", "inherited to_bool() (parent chain)");
            check_quad(q("!d").c_str(), "true", "!derived with inherited to_bool()");
            check_quad(q("sp ? 1 : 2").c_str(), "1", "live shared_ptr is truthy");
            check_quad(q("!sp").c_str(), "false", "!live shared_ptr");
            check_quad(q("e ? 1 : 2").c_str(), "2", "unset shared_ptr is falsy");
            check_quad(q("!e").c_str(), "true", "!unset shared_ptr");
            check_quad(q("v ? 1 : 2").c_str(), "2", "unset var is falsy");
            check_quad(q("!v").c_str(), "true", "!unset var");
            check_quad(q("int r = 0; if (t) { r += 1; } if (!t) { r += 10; } if (!e) { r += 100; } if (e) { r += 1000; } if (!b) { r += 10000; } r").c_str(), "10101", "if guards: object, !object, !unset shared_ptr, unset shared_ptr, !to_bool object");
            check_quad(q("int w = 0; while (!e) { w += 1; e = shared_ptr<T>(); } w").c_str(), "1", "while (!ptr) exits once assigned");
            check_quad(q("to_string(!i0) + \",\" + to_string(!f0) + \",\" + to_string(!s0) + \",\" + to_string(!a0) + \",\" + to_string(!'c') + \",\" + to_string(!true) + \",\" + to_string(!null)").c_str(), "true,true,true,true,false,false,true", "!primitive value forms");
            check_quad(q("int r2 = 0; if (!i0) { r2 += 1; } if (!f0) { r2 += 2; } if (!s0) { r2 += 4; } if (!a0) { r2 += 8; } if (!'c') { r2 += 16; } if (!null) { r2 += 32; } if (!(t && e)) { r2 += 64; } if (!t || !e) { r2 += 128; } r2").c_str(), "239", "if (!x) over every primitive kind, && and || operands");
            check_quad(q("string msg = \"none\"; try { if (!qq) { msg = \"neg\"; } else { msg = \"pos\"; } } catch (err) { msg = err; } msg").c_str(), "qbang", "a throwing to_bool() under if (!x) propagates");
            check_quad(q("string msg = \"none\"; try { if (qq) { msg = \"pos2\"; } else { msg = \"neg2\"; } } catch (err) { msg = err; } msg").c_str(), "qbang", "a throwing to_bool() under if (x) propagates");
            check_quad(q("class H { bool to_bool() { return false; } } auto h = H(); int r3 = 0; if (!h) { r3 += 1; } if (h) { r3 += 2; } r3").c_str(), "1", "class declared after the battery prefix: memo per class");
        });

        test("field_store_battery", [check_quad]() {
            // Bare field stores inside methods (`i = x`, the ctor-body and creature-state
            // shape) on both backends: rows are the exact pre-lane outputs. The vm's store-side
            // this-field cache lands typed enforcement + the store on the cell directly;
            // texts, conversions and value semantics must not move
            const std::string P = "class F { int i = 1; float f = 1.5; string s = \"a\"; bool b = false; auto a = 10; var v = 0; array<int> arr = [1, 2]; F other = null; shared_ptr<F> sp; static int st = 5; string setInt(var x) { string r = \"ok\"; try { i = x; } catch (err) { r = err; } return r; } string setFloat(var x) { string r = \"ok\"; try { f = x; } catch (err) { r = err; } return r; } string setStr(var x) { string r = \"ok\"; try { s = x; } catch (err) { r = err; } return r; } string setBool(var x) { string r = \"ok\"; try { b = x; } catch (err) { r = err; } return r; } string setAuto(var x) { string r = \"ok\"; try { a = x; } catch (err) { r = err; } return r; } string setVar(var x) { string r = \"ok\"; try { v = x; } catch (err) { r = err; } return r; } string setArr(var x) { string r = \"ok\"; try { arr = x; } catch (err) { r = err; } return r; } string setOther(var x) { string r = \"ok\"; try { other = x; } catch (err) { r = err; } return r; } string setSp(var x) { string r = \"ok\"; try { sp = x; } catch (err) { r = err; } return r; } int loopStore(int n) { for (int k = 0; k < n; ++k) { i = k; f = k; } return i; } int chainStore() { i = 7; int t = i; i = t + 1; return i; } int storeResult() { int r = (i = 42); return r; } float storeResultConv() { float r = (i = 3.9); return r; } string setStatic(var x) { string r = \"ok\"; try { st = x; } catch (err) { r = err; } return r; } string dump() { return to_string(i) + \"|\" + to_string(f) + \"|\" + s + \"|\" + to_string(b) + \"|\" + to_string(a) + \"|\" + to_string(v) + \"|\" + to_string(arr.size()) + \"|\" + to_string(other == null) + \"|\" + to_string(sp == null); } } class G : F { int g = 2; string setInherited(var x) { string r = \"ok\"; try { i = x; } catch (err) { r = err; } return r; } string setG(var x) { string r = \"ok\"; try { g = x; } catch (err) { r = err; } return r; } } auto o = F(); ";
            auto q = [&](const char* tail) { return P + tail; };
            check_quad(q("o.setInt(5) + \" \" + o.dump()").c_str(), "ok 5|1.5|a|false|10|0|2|true|true", "typed int field, int");
            check_quad(q("o.setInt(2.7) + \" \" + o.dump()").c_str(), "ok 2|1.5|a|false|10|0|2|true|true", "typed int field, float truncates");
            check_quad(q("o.setInt(\"x\") + \" \" + o.dump()").c_str(), "Cannot assign string to field 'i' of F (declared int) 1|1.5|a|false|10|0|2|true|true", "typed int field, string raises, field untouched");
            check_quad(q("o.setInt(true) + \" \" + o.dump()").c_str(), "ok 1|1.5|a|false|10|0|2|true|true", "typed int field, bool converts");
            check_quad(q("o.setInt(null) + \" \" + o.dump()").c_str(), "Cannot assign null to field 'i' of F (declared int) 1|1.5|a|false|10|0|2|true|true", "typed int field, null raises");
            check_quad(q("o.setFloat(2) + \" \" + o.dump()").c_str(), "ok 1|2.0|a|false|10|0|2|true|true", "typed float field, int promotes");
            check_quad(q("o.setFloat(\"q\") + \" \" + o.dump()").c_str(), "Cannot assign string to field 'f' of F (declared float) 1|1.5|a|false|10|0|2|true|true", "typed float field, string raises");
            check_quad(q("o.setStr(12) + \" \" + o.dump()").c_str(), "Cannot assign int to field 's' of F (declared string) 1|1.5|a|false|10|0|2|true|true", "typed string field, int raises, field untouched");
            check_quad(q("o.setBool(1) + \" \" + o.dump()").c_str(), "ok 1|1.5|a|true|10|0|2|true|true", "typed bool field, int converts");
            check_quad(q("o.setAuto(2.5) + \" \" + o.dump()").c_str(), "ok 1|1.5|a|false|2|0|2|true|true", "auto field infers int from its initializer, float truncates");
            check_quad(q("o.setAuto(\"str\") + \" \" + o.dump()").c_str(), "Cannot assign string to field 'a' of F (declared int) 1|1.5|a|false|10|0|2|true|true", "auto field rejects a string");
            check_quad(q("o.setVar(\"anything\") + \" \" + o.setVar([1,2,3]) + \" \" + o.dump()").c_str(), "ok ok 1|1.5|a|false|10|[1, 2, 3]|2|true|true", "var field takes anything");
            check_quad(q("o.setArr([4,5,6]) + \" \" + o.setArr([\"a\"]) + \" \" + o.setArr(7) + \" \" + o.dump()").c_str(), "ok Cannot assign array to field 'arr' of F (declared array<int>) Cannot assign int to field 'arr' of F (declared array<int>) 1|1.5|a|false|10|0|3|true|true", "typed array field: typed array ok, wrong element type and scalar raise");
            check_quad(q("auto o2 = F(); string r1 = o.setOther(o2); o2.i = 99; r1 + \" \" + to_string(o.other.i) + \" \" + o.setOther(5)").c_str(), "ok 1 Cannot assign int to field 'other' of F (declared F)", "object field: by-value copy, scalar raises");
            check_quad(q("o.setSp(shared_ptr<F>()) + \" \" + o.setSp(F()) + \" \" + o.setSp(3) + \" \" + o.dump()").c_str(), "ok ok Cannot assign int to field 'sp' of F (declared shared_ptr<F>) 1|1.5|a|false|10|0|2|true|false", "shared_ptr field: handle, value, scalar");
            check_quad(q("to_string(o.loopStore(1000)) + \" \" + o.dump()").c_str(), "999 999|999.0|a|false|10|0|2|true|true", "1000 stores at one site (the cached lane)");
            check_quad(q("to_string(o.chainStore()) + \",\" + to_string(o.storeResult()) + \",\" + to_string(o.storeResultConv()) + \",\" + to_string(o.i)").c_str(), "8,42,3.0,3", "store as expression: value is the stored (converted) value");
            check_quad(q("o.setStatic(9) + \" \" + o.setStatic(\"bad\") + \" \" + to_string(F::st)").c_str(), "ok Cannot assign string to static 'st' of F (declared int) 9", "static field stores enforce like instance fields (not the this-field lane)");
            check_quad(q("auto g = G(); g.setInherited(11) + \" \" + g.setInherited(\"no\") + \" \" + g.dump() + \" \" + g.setG(3.2) + \" \" + to_string(g.g)").c_str(), "ok Cannot assign string to field 'i' of G (declared int) 11|1.5|a|false|10|0|2|true|true ok 3", "inherited field through a derived instance");
            check_quad(q("array<F> fs = [F(), G()]; string acc = \"\"; for (auto& e : fs) { acc = acc + e.setInt(1) + \",\"; } acc = acc + to_string(fs[0].i) + to_string(fs[1].i); acc").c_str(), "ok,ok,11", "one site, two classes (polymorphic site re-arms)");
            check_quad(q("class H { int i = 0; int bump() { for (int k = 0; k < 5; ++k) { i += 1; } return i; } } auto h = H(); to_string(h.bump()) + \" \" + to_string(h.bump())").c_str(), "5 10", "compound field stores");
            check_quad(q("class R { int x = 1; int y = 2; void swap() { int t = x; x = y; y = t; } string dump() { return to_string(x) + \",\" + to_string(y); } } auto r = R(); r.swap(); r.swap(); r.swap(); r.dump()").c_str(), "2,1", "two fields swapped through a local");
            check_quad(q("class S { array<int> xs = [1]; void put(array<int> src) { xs = src; } int total() { int t = 0; for (auto q : xs) { t += q; } return t; } } auto sobj = S(); array<int> mine = [1, 2, 3]; sobj.put(mine); mine.push(100); to_string(sobj.total()) + \" \" + to_string(mine.size())").c_str(), "6 4", "lvalue rhs clones (value semantics)");
            check_quad(q("class Q { int i = 1; string setThis(var x) { string r = \"ok\"; try { this.i = x; } catch (err) { r = err; } return r; } } auto q = Q(); q.setThis(4) + \" \" + to_string(q.i) + \" \" + q.setThis(\"z\") + \" \" + to_string(q.i)").c_str(), "ok 4 Cannot assign string to field 'i' of Q (declared int) 4", "explicit this.field (member path, not the bare lane)");
            check_quad(q("class W { weak_ptr<F> w; string setW(var x) { string r = \"ok\"; try { w = x; } catch (err) { r = err; } return r; } } auto w = W(); auto keep = shared_ptr<F>(); w.setW(keep) + \" \" + w.setW(F()) + \" \" + to_string(w.w == null)").c_str(), "ok Cannot assign F to field 'w' of W (declared weak_ptr<F>) false", "weak_ptr field: a shared handle downgrades (C++ weak = shared), a value raises");
        });

        auto quad_in_warn_region = [](const char* src) {
            std::string out[2];
            int idx = 0;
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execution_budget(0);
                jai::stdlib::register_all(e);
                try { out[idx] = e->execute(std::string("[[warn_mode]]; ") + src).to_string(); }
                catch (const std::exception& ex) { out[idx] = std::string("ERROR: ") + ex.what(); }
                ++idx;
            }
            return std::array<std::string, 2>{out[0], out[1]};
        };
        auto check_quad_runtime = [this, quad_in_warn_region](const char* src, const std::string& expected, const char* what) {
            auto r = quad_in_warn_region(src);
            check_eq(expected, r[0], std::string("interp/warn region: ") + what);
            check_eq(expected, r[1], std::string("vm/warn region: ") + what);
        };
        auto check_quad_error_text = [this, quad_in_warn_region](const char* src, const std::string& needle, const char* what) {
            auto r = quad_in_warn_region(src);
            check_eq(r[0], r[1], std::string("backend parity: ") + what);
            check(r[0].find("ERROR") == 0, std::string("raises: ") + what);
            check(r[0].find(needle) != std::string::npos, std::string("text: ") + what + " (got: " + r[0] + ")");
        };

        test("static_store_battery", [check_quad]() {
            // Every supported static-store spelling (C::s anywhere; bare s in instance methods,
            // constructors and static methods) runs the instance-field store table on both
            // backends: same conversions, same text, the static untouched on a miss
            const std::string P = "class C { static int i = 1; static float f = 1.5; static string s = \"a\"; static bool b = false; static auto a = 10; static var v = 0; static array<int> arr = [1, 2]; static C other = null; static shared_ptr<C> sp; static weak_ptr<C> w; int x = 0; C() {} C(var val) { i = val; } string setQ(var val) { string r = \"ok\"; try { i = val; } catch (err) { r = err; } return r; } static string setS(var val) { string r = \"ok\"; try { i = val; } catch (err) { r = err; } return r; } string dump() { return to_string(i) + \"|\" + to_string(f) + \"|\" + s + \"|\" + to_string(b) + \"|\" + to_string(a) + \"|\" + to_string(v) + \"|\" + to_string(arr.size()) + \"|\" + to_string(other == null) + \"|\" + to_string(sp == null) + \"|\" + to_string(w == null); } } function setI(var val) { string r = \"ok\"; try { C::i = val; } catch (err) { r = err; } return r; } function setF(var val) { string r = \"ok\"; try { C::f = val; } catch (err) { r = err; } return r; } function setStr(var val) { string r = \"ok\"; try { C::s = val; } catch (err) { r = err; } return r; } function setB(var val) { string r = \"ok\"; try { C::b = val; } catch (err) { r = err; } return r; } function setA(var val) { string r = \"ok\"; try { C::a = val; } catch (err) { r = err; } return r; } function setV(var val) { string r = \"ok\"; try { C::v = val; } catch (err) { r = err; } return r; } function setArr(var val) { string r = \"ok\"; try { C::arr = val; } catch (err) { r = err; } return r; } function setOther(var val) { string r = \"ok\"; try { C::other = val; } catch (err) { r = err; } return r; } function setSp(var val) { string r = \"ok\"; try { C::sp = val; } catch (err) { r = err; } return r; } function setW(var val) { string r = \"ok\"; try { C::w = val; } catch (err) { r = err; } return r; } auto o = C(); ";
            auto q = [&](const char* tail) { return P + tail; };
            const std::string base = "1|1.5|a|false|10|0|2|true|true|true";
            check_quad(q("setI(5) + \" \" + o.dump()").c_str(), "ok 5|1.5|a|false|10|0|2|true|true|true", "C::i, int");
            check_quad(q("setI(2.7) + \" \" + o.dump()").c_str(), "ok 2|1.5|a|false|10|0|2|true|true|true", "C::i, float truncates");
            check_quad(q("setI(\"x\") + \" \" + o.dump()").c_str(), "Cannot assign string to static 'i' of C (declared int) " + base, "C::i, string raises, static untouched");
            check_quad(q("setI(true) + \" \" + o.dump()").c_str(), "ok 1|1.5|a|false|10|0|2|true|true|true", "C::i, bool converts");
            check_quad(q("setI(null) + \" \" + o.dump()").c_str(), "Cannot assign null to static 'i' of C (declared int) " + base, "C::i, null raises");
            check_quad(q("setI([1]) + \" \" + o.dump()").c_str(), "Cannot assign array to static 'i' of C (declared int) " + base, "C::i, array raises");
            check_quad(q("setI(C()) + \" \" + o.dump()").c_str(), "Cannot assign C to static 'i' of C (declared int) " + base, "C::i, object raises");
            check_quad(q("o.setQ(5) + \" \" + o.setQ(2.7) + \" \" + o.setQ(\"x\") + \" \" + to_string(C::i)").c_str(), "ok ok Cannot assign string to static 'i' of C (declared int) 2", "bare static in an instance method");
            check_quad(q("C::setS(6) + \" \" + C::setS(3.9) + \" \" + C::setS(\"x\") + \" \" + to_string(C::i)").c_str(), "ok ok Cannot assign string to static 'i' of C (declared int) 3", "bare static in a static method");
            check_quad(q("C(7); string r = \"ok\"; try { C(\"x\"); } catch (err) { r = err; } r + \" \" + to_string(C::i)").c_str(), "Cannot assign string to static 'i' of C (declared int) 7", "bare static in a constructor");
            check_quad(q("auto lam = [](var val) { string r = \"ok\"; try { C::i = val; } catch (err) { r = err; } return r; }; lam(\"x\") + \" \" + lam(3) + \" \" + to_string(C::i)").c_str(), "Cannot assign string to static 'i' of C (declared int) ok 3", "C::i inside a lambda");
            check_quad(q("var vv = 2.9; C::i = vv; to_string(C::i) + \"|\" + type_of(C::i)").c_str(), "2|int", "C::i at top level keeps the declared type");
            check_quad(q("setF(2) + \" \" + setF(\"q\") + \" \" + o.dump()").c_str(), "ok Cannot assign string to static 'f' of C (declared float) 1|2.0|a|false|10|0|2|true|true|true", "float static: int promotes, string raises");
            check_quad(q("setStr(12) + \" \" + setStr(null) + \" \" + setStr(C()) + \" \" + o.dump()").c_str(), "Cannot assign int to static 's' of C (declared string) Cannot assign null to static 's' of C (declared string) Cannot assign C to static 's' of C (declared string) 1|1.5|a|false|10|0|2|true|true|true", "a string static takes text only (the instance-field row)");
            check_quad(q("setB(1) + \" \" + o.dump()").c_str(), "ok 1|1.5|a|true|10|0|2|true|true|true", "bool static: int converts");
            check_quad(q("setA(2.5) + \" \" + setA(\"str\") + \" \" + o.dump()").c_str(), "ok Cannot assign string to static 'a' of C (declared int) 1|1.5|a|false|2|0|2|true|true|true", "auto static infers int from its initializer");
            check_quad(q("setV(\"anything\") + \" \" + setV([1, 2, 3]) + \" \" + o.dump()").c_str(), "ok ok 1|1.5|a|false|10|[1, 2, 3]|2|true|true|true", "var static takes anything");
            check_quad(q("setArr([4, 5, 6]) + \" \" + setArr([\"a\"]) + \" \" + setArr(7) + \" \" + o.dump()").c_str(), "ok Cannot assign array to static 'arr' of C (declared array<int>) Cannot assign int to static 'arr' of C (declared array<int>) 1|1.5|a|false|10|0|3|true|true|true", "array<int> static: typed array ok, wrong element type and scalar raise");
            check_quad(q("auto o2 = C(); string r1 = setOther(o2); o2.x = 99; r1 + \" \" + to_string(C::other.x) + \" \" + setOther(5)").c_str(), "ok 0 ok", "object static: by-value copy, a scalar converts through C(var)");
            check_quad(q("setSp(shared_ptr<C>()) + \" \" + setSp(C()) + \" \" + setSp(3) + \" \" + o.dump()").c_str(), "ok ok Cannot assign int to static 'sp' of C (declared shared_ptr<C>) 1|1.5|a|false|10|0|2|true|false|true", "shared_ptr static: handle, value, scalar");
            check_quad(q("auto keep = shared_ptr<C>(); setW(keep) + \" \" + setW(C()) + \" \" + o.dump()").c_str(), "ok Cannot assign C to static 'w' of C (declared weak_ptr<C>) 1|1.5|a|false|10|0|2|true|true|false", "weak_ptr static: the instance-field rows (a shared handle downgrades, a value raises)");
            check_quad("class P { int v = 1; } class S { static shared_ptr<P> p; } auto h = shared_ptr<P>(); S::p = h; h.v = 5; to_string(S::p.v)", "5", "shared_ptr static shares the handle on both backends");
            check_quad(q("int r = (C::i = 42); to_string(r)").c_str(), "42", "store as expression");
            check_quad(q("float r = (C::i = 3.9); to_string(r) + \"|\" + to_string(C::i)").c_str(), "3.0|3", "store as expression yields the stored (converted) value");
        });

        test("static_compound_battery", [check_quad, check_quad_error]() {
            // `C::s op= v` and ++/-- on a class-qualified static are lvalues on both backends:
            // C::s = T(C::s op v) with the compound texts; the bare forms keep working
            const std::string P = "class C { static int i = 1; static float f = 1.5; static string s = \"a\"; static var v = 1; string bumpQ() { i += 2; i++; ++i; return to_string(i); } static string bumpS() { i += 2; i++; ++i; return to_string(i); } } function L(var x) { return x; } ";
            auto q = [&](const char* tail) { return P + tail; };
            check_quad(q("C::i += 1; to_string(C::i)").c_str(), "2", "+=");
            check_quad(q("C::i -= 1; to_string(C::i)").c_str(), "0", "-=");
            check_quad(q("C::i *= 2; to_string(C::i)").c_str(), "2", "*=");
            check_quad(q("C::i = 8; C::i /= 2; to_string(C::i) + \"|\" + type_of(C::i)").c_str(), "4|int", "/= converts back to the declared int");
            check_quad(q("C::i = 7; C::i %= 3; to_string(C::i)").c_str(), "1", "%=");
            check_quad(q("C::i += 1.5; to_string(C::i) + \"|\" + type_of(C::i)").c_str(), "2|int", "int static += float truncates back to int");
            check_quad(q("C::v += 1.5; to_string(C::v) + \"|\" + type_of(C::v)").c_str(), "2.5|float", "var static += float becomes float");
            check_quad(q("C::f *= 2; to_string(C::f)").c_str(), "3.0", "float static *=");
            check_quad(q("C::s += \"b\"; C::s").c_str(), "ab", "string static concat");
            check_quad(q("auto old = C::i++; to_string(old) + \",\" + to_string(C::i)").c_str(), "1,2", "postfix ++ yields the old value");
            check_quad(q("auto nw = ++C::i; to_string(nw) + \",\" + to_string(C::i)").c_str(), "2,2", "prefix ++ yields the new value");
            check_quad(q("C::i--; --C::i; to_string(C::i)").c_str(), "-1", "-- both forms");
            check_quad(q("auto o = C(); o.bumpQ() + \" \" + C::bumpS() + \" \" + to_string(C::i)").c_str(), "5 9 9", "bare compound and ++ in instance and static methods");
            check_quad(q("for (int k = 0; k < 1000; k++) { C::i += 1; } to_string(C::i)").c_str(), "1001", "1000 compound stores at one site");
            check_quad("class K { int x = 1; K(int v) { x = v; } function operator+(int n) -> K { return K(x + n); } } class H { static K k = K(1); } H::k += 5; to_string(H::k.x)", "6", "object static compound dispatches operator+");
            check_quad(q("string r = \"ok\"; try { C::i += L(\"x\"); } catch (err) { r = err; } r + \" \" + to_string(C::i)").c_str(), "Cannot apply '+=' to int and string 1", "+= string into an int static raises the compound text, static untouched");
            check_quad(q("string r = \"ok\"; try { C::i /= L(0); } catch (err) { r = err; } r").c_str(), "Division by zero", "/= 0");
            check_quad(q("string r = \"ok\"; try { C::i %= L(0); } catch (err) { r = err; } r").c_str(), "Modulo by zero", "%= 0");
            check_quad(q("C::i = 9223372036854775807; string r = \"ok\"; try { C::i++; } catch (err) { r = err; } r + \" \" + to_string(C::i)").c_str(), "Integer overflow in '++' 9223372036854775807", "++ overflow raises, static untouched");
            check_quad_error(q("C::s++").c_str(), "++ on a string static");
            check_quad_error(q("C::nosuch = 1").c_str(), "store to a missing static");
            check_quad_error(q("C::nosuch += 1").c_str(), "compound to a missing static");
            check_quad_error(q("Nope::x = 1").c_str(), "store to a missing class");
        });

        test("static_definition_typing", [check_quad, check_quad_error, check_quad_error_text]() {
            // A static's declared type is recorded at definition and its initializer converts
            // through the field table; a missing initializer is a readable typed null
            check_quad("class C { static float f = 3; } type_of(C::f) + \" \" + to_string(C::f)", "float 3.0", "static float from an int initializer");
            check_quad("class C { static int n = true; } to_string(C::n) + \"|\" + type_of(C::n)", "1|int", "static int from a bool initializer");
            check_quad_error("function L(var x) { return x; } class C { static int s = L(\"x\"); }", "an unconvertible static initializer raises at definition");
            check_quad_error_text("function L(var x) { return x; } class C { static int s = L(\"x\"); }", "Cannot assign string to static 's' of C (declared int)", "the definition raises the store text");
            check_quad("class C { static int s; } to_string(C::s)", "0", "a typed static without an initializer is its type's zero");
            check_quad("class C { static var v = null; } to_string(C::v == null)", "true", "null var static reads");
            check_quad("class P { int a = 0; } class C { static shared_ptr<P> p; } to_string(C::p == null)", "true", "null shared_ptr static reads");
            check_quad("class P { int a = 0; } class C { static P obj; static P none = null; } to_string(C::obj.a) + \",\" + to_string(C::none == null)", "0,true", "a class-typed static default-constructs; = null spells a null one");
            check_quad("function L(var x) { return x; } class C { static int s; } C::s = L(4); string r = \"ok\"; try { C::s = L(\"x\"); } catch (err) { r = err; } to_string(C::s) + \" \" + r", "4 Cannot assign string to static 's' of C (declared int)", "a null-initialized static still enforces its declared type");
            check_quad("function L(var x) { return x; } class C { static auto a = 2; } C::a = L(2.5); string r = \"ok\"; try { C::a = L(\"str\"); } catch (err) { r = err; } to_string(C::a) + \" \" + r", "2 Cannot assign string to static 'a' of C (declared int)", "auto static enforces the inferred int");
        });

        test("static_spellings_rejected", [check_quad_error, check_quad_runtime, check_quad_error_text]() {
            // `C.s` and `this.s` are not static spellings: their read and write errors stay,
            // byte-identical across backends (the write raises a STRING, never a bool)
            const std::string P = "class C { static int s = 0; int q = 1; string viaThis() { string r = \"ok\"; try { this.s = 1; } catch (err) { r = type_of(err) + \":\" + to_string(err); } return r; } string readThis() { string r = \"ok\"; try { int t = this.s; } catch (err) { r = to_string(err); } return r; } } ";
            check_quad_error("class C { static int s = 0; } C.s", "dot read on a class name");
            check_quad_runtime("class C { static int s = 0; } string r = \"\"; try { C.s = 8; } catch (err) { r = type_of(err) + \":\" + to_string(err); } r + \" \" + to_string(C::s)", "string:'C' is a class; use C::s 0", "dot store on a class name");
            check_quad_error("class C { static int s = 0; } C.s += 1", "dot compound on a class name");
            check_quad_error("class C { static int s = 0; } C.s++", "dot ++ on a class name");
            check_quad_runtime((P + "auto o = C(); o.viaThis() + \" \" + to_string(C::s)").c_str(), "string:Cannot assign to 's': C has no such member 0", "this.s store");
            check_quad_runtime((P + "auto o = C(); o.readThis()").c_str(), "C has no member 's'", "this.s read");
            check_quad_error_text("class S { private: static int counter = 0; public: static int peek() { return counter; } } S::counter = 5", "Cannot access private member 'counter' of class 'S'", "private static store is refused before any conversion");
        });

        test("increment_targets_battery", [check_quad, check_quad_error]() {
            // ++/-- work on every lvalue, not only identifiers. Non-
            // identifier targets desugar at parse time to `+= 1` / `-= 1` (the compound
            // machinery both backends share); postfix yields the exact old value
            check_quad("array<int> a = [1, 2]; a[0]++; to_string(a[0])", "2", "postfix ++ on a typed element");
            check_quad("array<int> a = [1, 2]; ++a[1]; to_string(a[1])", "3", "prefix ++ on a typed element");
            check_quad("array<int> a = [5]; a[0]--; to_string(a[0])", "4", "postfix -- on a typed element");
            check_quad("var a = [1.5]; a[0]++; to_string(a[0])", "2.5", "++ on a float element");
            check_quad("var a = [7]; a[0]++; to_string(a[0])", "8", "++ on a dynamic element");
            check_quad("array<int> a = [5]; auto x = a[0]++; to_string(x) + \",\" + to_string(a[0])", "5,6", "postfix in expression position yields the old value");
            check_quad("array<int> a = [5]; auto y = ++a[0]; to_string(y) + \",\" + to_string(a[0])", "6,6", "prefix in expression position yields the new value");
            check_quad("array<float> f = [9007199254740994.0]; auto o = f[0]++; to_string(o == 9007199254740994.0) + \",\" + to_string(f[0] == 9007199254740995.0 || f[0] == 9007199254740996.0)", "true,true", "postfix old value is exact for large floats");
            check_quad("class K { int a = 1; } auto k = K(); k.a++; to_string(k.a)", "2", "postfix ++ on a member");
            check_quad("class K { int a = 1; } auto k = K(); ++k.a; to_string(k.a)", "2", "prefix ++ on a member");
            check_quad("class K { int a = 1; } auto k = K(); auto o = k.a++; to_string(o) + \",\" + to_string(k.a)", "1,2", "postfix member yields the old value");
            check_quad("class K { int a = 1; void bump() { this.a++; } } auto k = K(); k.bump(); to_string(k.a)", "2", "explicit this member");
            check_quad("class K { int a = 1; void bump() { a++; } } auto k = K(); k.bump(); to_string(k.a)", "2", "implicit this member (unchanged path)");
            check_quad("var m = {\"n\": 1}; m[\"n\"]++; to_string(m[\"n\"])", "2", "map entry");
            check_quad("map<string,int> m = {\"n\": 1}; auto o = m[\"n\"]++; to_string(o) + \",\" + to_string(m[\"n\"])", "1,2", "map entry postfix yields the old value");
            check_quad("var a = [[1]]; a[0][0]++; to_string(a[0][0])", "2", "nested element");
            check_quad("class K { array<int> v = [1]; } auto k = K(); k.v[0]++; to_string(k.v[0])", "2", "member-index element");
            check_quad("int n = 5; auto x = n++; auto y = ++n; to_string(x) + \",\" + to_string(y) + \",\" + to_string(n)", "5,7,7", "identifiers keep their path");
            check_quad("array<int> a = [1, 2, 3]; for (int i = 0; i < 3; i++) { a[i]++; } to_string(a[0] + a[1] + a[2])", "9", "loop over elements");
            check_quad_error("var a = [\"s\"]; a[0]++", "++ on a string element raises the same text on both backends");
            check_quad_error("var m = {}; m[\"missing\"]++", "++ on a missing map entry raises the same text on both backends");
            check_quad_error("5++", "++ on a literal raises the same text on both backends");
            check_quad_error("array<int> a = [9223372036854775807]; a[0]++", "++ overflow on a typed element follows the overflow policy (checked build raises)");
        });

        test("map_key_types_battery", [check_quad, check_quad_error]() {
            // The declared key type is enforced on direct stores; map<var,V>
            // takes any key; an `auto` key or value slot infers from a uniform literal (mixed = error)
            // and stays dynamic for an empty literal - the array rule applied to maps
            check_quad("map<var,int> m = {}; m[5] = 1; m[\"x\"] = 2; to_string(m.size())", "2", "map<var,V> takes any key");
            check_quad("map<auto,int> m = {}; m[\"x\"] = 1; m[2] = 3; to_string(m.size())", "2", "map<auto,V> with an empty literal stays dynamic");
            check_quad("map<auto,int> m = {5: 1}; m[6] = 2; to_string(m.size()) + \",\" + to_string(m[6])", "2,2", "map<auto,V> infers an int key from a uniform literal");
            check_quad("map<string,auto> m = {\"a\": 1}; m[\"b\"] = 2; to_string(m[\"b\"])", "2", "map<K,auto> infers an int value");
            check_quad("auto m = {\"a\": 1, \"b\": 2}; m[\"c\"] = 3; to_string(m.size())", "3", "auto with a uniform map literal infers map<string,int>");
            check_quad("map<auto,auto> m = {1.5: \"x\"}; m[2.5] = \"y\"; to_string(m.size())", "2", "both slots infer");
            check_quad("map<auto,int> m = {5: 1}; type_of(m)", "map", "the inferred map is still a map");
            check_quad_error("map<auto,int> m = {5: 1}; m[\"x\"] = 1", "an inferred int key rejects a string key");
            check_quad_error("map<auto,int> m = {5: 1, \"hi\": 1}", "mixed keys under an auto key slot raise");
            check_quad_error("map<string,auto> m = {\"a\": 1}; m[\"b\"] = \"s\"", "an inferred int value rejects a string value");
            check_quad_error("auto m = {\"a\": 1}; m[5] = 1", "auto-inferred map<string,int> rejects an int key");
            check_quad_error("map<int,int> m = {}; m[1.5] = 1", "keys never coerce: a float key into an int map raises");
        });

        test("subclass_admission_battery", [check_quad, check_quad_error]() {
			// Runtime subclass tags and value-dependent downcasts are tested explicitly in warn mode.
			// Class-typed arrays admit the class and its subclasses (exact
			// ids compare first; the parent-chain walk runs only on a name mismatch); each
			// slot keeps its own class tag; whole-array conversions prove the source class once
			check_quad("[[warn_mode]] class B { int a = 1; } class D : B { int b = 2; } array<B> arr = [B(), D()]; to_string(arr.size()) + \",\" + to_string(arr[1].b)", "2,2", "a subclass element keeps its own class (per-slot tag): its fields stay reachable");
			check_quad("[[warn_mode]] class B { int a = 1; } class D : B { int b = 2; } array<B> arr = [B()]; arr.push(D()); to_string(arr.size()) + \",\" + to_string(arr[1].a)", "2,1", "push of a subclass into array<Base>");
			check_quad("[[warn_mode]] class B { int a = 1; } class D : B { int b = 2; } array<B> arr = [B()]; D d = D(); d.a = 5; arr[0] = d; to_string(arr[0].a) + type_of(arr[0]) + to_string(arr[0].b)", "5D2", "store of a subclass over an element takes the subclass in place (no slicing)");
			check_quad("[[warn_mode]] class B { int a = 1; } class D : B { int b = 2; } class E : D { int c = 3; } array<B> deep = [E()]; to_string(deep[0].c)", "3", "two inheritance levels");
			check_quad("[[warn_mode]] class B { int a = 1; } class D : B { int b = 2; } array<D> ds = [D()]; array<B> bs = ds; bs[0].a = 9; to_string(ds[0].a) + \",\" + to_string(bs[0].a)", "1,9", "array<Derived> into array<Base> is a copy (value semantics)");
			check_quad("[[warn_mode]] class B { int a = 1; } class D : B { int b = 2; } array<B> bs = [D(), D()]; array<D> back = bs; to_string(back.size()) + \",\" + to_string(back[1].b)", "2,2", "array<Base> whose elements are all Derived converts to array<Derived>");
			check_quad("[[warn_mode]] class B { int a = 1; } class D : B { int b = 2; } function total(array<B> xs) -> int { int t = 0; for (auto x : xs) { t += x.a; } return t; } array<D> ds = [D(), D()]; to_string(total(ds))", "2", "array<Derived> binds a by-value array<Base> parameter");
			check_quad("[[warn_mode]] class B { int a = 1; } class D : B { int b = 2; } function grow(array<B>& xs) { xs.push(B()); } array<B> bs = [D()]; grow(bs); to_string(bs.size())", "2", "reference parameter over array<Base> holding a subclass");
			check_quad("[[warn_mode]] class B { int a = 1; } class D : B { int b = 2; } array<B> arr = [D()]; auto c = arr; c[0].b = 7; to_string(arr[0].b) + \",\" + to_string(c[0].b)", "2,7", "copy of an array<Base> clones the subclass element");
			check_quad_error("[[warn_mode]] class B { int a = 1; } class D : B { int b = 2; } array<B> bs = [B()]; array<D> x = bs", "array<Base> holding a Base does not convert to array<Derived>");
			check_quad_error("[[warn_mode]] class B { int a = 1; } class D : B { int b = 2; } function grow(array<B>& xs) { xs.push(B()); } array<D> ds = [D()]; grow(ds)", "a reference parameter aliases the caller's array under ITS tag: pushing a Base into array<Derived> still raises");
			check_quad_error("[[warn_mode]] class B { int a = 1; } class J { int j = 0; } array<B> bs = [B()]; bs.push(J())", "an unrelated class is still rejected");
		});

        test("typed_decl_initializer_exception_wins", [check_quad_error]() {
            // fuzz seed 22684: the interpreter's typed declaration enforced the
            // declared type on the null a raising member read leaves behind, reporting
            // 'Type mismatch in assignment' where the vm reports the member exception
            check_quad_error("class K { int a = 1; } auto k = K(); int y = k.missing", "int decl from a missing member raises the member text");
            check_quad_error("class K { int a = 1; } auto k = K(); double d = (k.missing == 1) ? 1.0 : 2.0", "double decl from a missing-member comparison raises the member text");
            check_quad_error("class K { int a = 1; } auto k = K(); string s = k.missing", "string decl from a missing member raises the member text");
            check_quad_error("class K { int a = 1; } auto k = K(); shared_ptr<K> p = k.missing", "shared_ptr decl from a missing member raises the member text");
            check_quad_error("class K { int a = 1; } auto k = K(); weak_ptr<K> w = k.missing", "weak_ptr decl from a missing member raises the member text");
        });

        test("typed_storage_reads_and_stores", [check_quad, check_quad_error]() {
            check_quad("array<int> a = [1,2,3]; `${to_string(a[0])},${to_string(a[2])}`", "1,3", "reads");
            check_quad("array<int> a = [1,2,3]; a[1] = 9; to_string(a[1])", "9", "store");
            check_quad("array<int> a = [1]; a[0] = 2.7; `${type_of(a[0])}:${to_string(a[0])}`", "int:2", "store converts");
            check_quad("array<float> a = [1.5]; a[0] = 2; `${type_of(a[0])}:${to_string(a[0])}`", "float:2.0", "float store widens");
            check_quad("array<int> a = [10]; a[0] += 2.5; `${type_of(a[0])}:${to_string(a[0])}`", "int:12", "compound converts");
            check_quad("array<int> a = [10]; a[0] %= 4.5; to_string(a[0])", "1", "compound modulo");
            // byte kinds (bool/char buffers): reads, stores, push, sort, reverse, copies, json
            check_quad("array<bool> f = [true, false, true]; f[1] = true; to_string(f[0]) + to_string(f[1]) + to_string(f.size())", "truetrue3", "bool buffer store/read");
            check_quad("array<bool> f = []; f.push(false); f.push(true); f.sort((a, b) => !a && b); to_string(f[0]) + to_string(f[1])", "falsetrue", "bool buffer push/sort");
            check_quad("array<char> c = ['q', 'a', 'z']; c.sort(); c.reverse(); to_string(c[0]) + to_string(c[2])", "za", "char buffer sort/reverse");
            check_quad("array<char> c = ['a', 'b']; array<char> d = c; d[0] = 'x'; to_string(c[0]) + to_string(d[0])", "ax", "char buffer copy is a copy");
            check_quad("array<bool> f = [true, false]; to_json(f)", "[true,false]", "bool buffer serializes");
            check_quad("array<char> c = ['h', 'i']; var s = \"\"; for (auto ch : c) { s = s + to_string(ch); } s", "hi", "char buffer range-for");
            check_quad("array<bool> f = [true]; f.push(1); f.push(0); to_json(f)", "[true,true,false]", "bool buffer converts an int");
            check_quad("array<char> c = ['a']; c[0] = \"s\"; to_string(c[0])", "s", "char buffer takes a one-character string");
            check_quad_error("array<char> c = ['a']; c[0] = \"st\"", "char buffer rejects a longer string");
            // fuzz seed 201467 (element band): an int written into a bool element through a reference reached the
            // buffer unconverted and crashed both backends; a reference store takes the direct store's rows
            check_quad("array<bool> f = [false, true]; for (auto& x : f) { x = 5; } to_json(f)", "[true,true]", "bool buffer store through a range-for reference converts an int");
            check_quad("function z(bool& p) { p = 0; } array<bool> f = [true]; z(f[0]); to_json(f)", "[false]", "an int stored through a bool& parameter into a bool buffer converts");
            check_quad("array<bool> f = [false]; auto& r = f[0]; r = 3; to_json(f)", "[true]", "bool buffer store through a reference local");
            check_quad("array<int> a = [1]; for (auto& x : a) { x = true; } to_json(a)", "[1]", "int buffer store through a reference converts a bool");
            // string handles: reads, stores, push, sort, join, copy, in-place mutator through an element, json
            check_quad("array<string> s = [\"b\", \"a\"]; s.push(\"c\"); s.sort(); s.join(\"-\")", "a-b-c", "string buffer push/sort/join");
            check_quad("array<string> s = [\"x\"]; s[0] = \"y\"; s[0] + to_string(s.size())", "y1", "string buffer store/read");
            check_quad("array<string> s = [\"ab\"]; array<string> t = s; t[0] = \"zz\"; s[0] + t[0]", "abzz", "string buffer copy is a copy");
            check_quad("array<string> s = [\"q\", \"r\"]; var o = \"\"; for (auto w : s) { o = o + w; } o", "qr", "string buffer range-for");
            check_quad("array<string> s = [\"hi\"]; to_json(s)", "[\"hi\"]", "string buffer serializes");
            check_quad("array<string> s = [\"a\", \"b\"]; s.reverse(); s[0]", "b", "string buffer reverse");
            check_quad_error("array<string> s = [\"a\"]; s.push(1)", "string buffer rejects int");
            // fuzz seed 129: the vm's unfused element-store lane took ANY typed
            // node with a numeric rhs - an int landed unchecked in a string buffer
            check_quad_error("array<string> s = [\"a\"]; int g = 5; s[0] = g", "string buffer element store rejects int");
            check_quad("array<string> s = [\"a\"]; s[0] += \"1\"; s[0]", "a1", "string buffer compound store concatenates through the enforcing path");
            check_quad("array<bool> f = [true, true]; f[0] = 0; f[1] = 7; to_json(f)", "[false,true]", "bool buffer element store converts an int");
            check_quad_error("array<char> c = ['a']; c[0] += 1", "char buffer compound store rejects int");
            check_quad("array<string> s = [\"a\"]; string t = \"b\"; s[0] = t; s[0]", "b", "string buffer element store of a string");
            // fuzz seed 6977: the vm's inline push named an ELEMENT-REFERENCE
            // argument 'reference' in the mismatch text where the interpreter names the
            // referent (pre-existing for dynamic tables too; ref locals and fields were fine)
            check_quad_error("array<string> t = [\"a\"]; array<int> v = [1]; v.push(t[0])", "push of a string-buffer element into array<int> names the referent");
            check_quad_error("array<var> t = [\"a\"]; array<int> v = [1]; v.push(t[0])", "push of a dynamic-table element into array<int> names the referent");
            check_quad("array<string> t = [\"a\"]; array<string> v = []; v.push(t[0]); v[0]", "a", "push of a string-buffer element into array<string>");
        });

        test("byte_buffers_uint8_int8", [this]() {
            // array<uint8> / array<int8>: one raw byte per element, element reads are ints, every store runs the range kernel;
            // both backends, proofs on and off, byte-identical outputs and error texts
            auto run = [](const std::string& src, bool use_vm, bool proofs) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->type_proofs(proofs);
                e->execution_budget(0);
                jai::stdlib::register_all(e);
                try { return e->execute(src).to_string(); }
                catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
            };
            auto expect = [this, run](const std::string& src, const std::string& expected, const char* what) {
                for (bool proofs : {true, false}) {
                    for (bool use_vm : {false, true}) {
                        check_eq(expected, run(src, use_vm, proofs), std::string(what) + (use_vm ? " (vm" : " (interpreter") + (proofs ? ")" : ", proofs off)"));
                    }
                }
            };
            auto caught = [](const std::string& body) { return "var out = \"none\"; try { " + body + "; } catch (e) { out = e; } out"; };

            expect("array<uint8> b = [0, 1, 255]; to_string(b[0]) + \",\" + to_string(b[2]) + \",\" + type_of(b[2]) + \",\" + to_string(b.size())", "0,255,int,3", "uint8 reads are ints");
            expect("array<int8> b = [-128, 127]; to_string(b[0]) + \",\" + to_string(b[1]) + \",\" + to_string(b[0] + b[1])", "-128,127,-1", "int8 reads are signed ints");
            expect("array<uint8> b = [0]; b[0] = 200; int i = 0; b[i] = 201; to_json(b)", "[201]", "a store inside the range");
            expect(caught("array<uint8> b = [0]; int v = 256; b[0] = v"), "Cannot store 256 in array<uint8> (0..255)", "a store above the range");
            expect(caught("array<uint8> b = [0]; int v = -1; b[0] = v"), "Cannot store -1 in array<uint8> (0..255)", "a negative store");
            expect(caught("array<int8> b = [0]; int v = 128; b[0] = v"), "Cannot store 128 in array<int8> (-128..127)", "int8 above");
            expect(caught("array<int8> b = [0]; int v = -129; b[0] = v"), "Cannot store -129 in array<int8> (-128..127)", "int8 below");
            expect(caught("array<int8> b = []; int v = 128; b.push(v)"), "Cannot store 128 in array<int8> (-128..127)", "push above the range");
            expect("array<uint8> b = []; for (int i = 0; i < 10; i++) { b.push(i * 25); } int s = 0; for (int i = 0; i < b.size(); i++) { s += b[i]; } to_string(s) + \" \" + to_string(b[9])", "1125 225", "push and read in counted loops");
            expect("array<uint8> b = [0, 0, 0]; b[0] = 7.9; b[1] = true; b[2] = false; to_json(b)", "[7,1,0]", "float and bool convert");
            expect(caught("array<uint8> b = [0]; float f = 300.5; b[0] = f"), "Cannot store 300 in array<uint8> (0..255)", "a float converts then meets the range");
            expect(caught("array<uint8> b = [0]; float f = 1e300; b[0] = f"), "Cannot convert float to integer: non-finite or out of range", "a float outside int");
            expect("array<uint8> b = [250]; b[0] += 5; to_string(b[0])", "255", "a compound inside the range");
            expect(caught("array<uint8> b = [255]; b[0] += 1"), "Cannot store 256 in array<uint8> (0..255)", "a compound past the range");
            expect(caught("array<uint8> b = [255]; b[0]++"), "Cannot store 256 in array<uint8> (0..255)", "++ past the range");
            expect(caught("array<int8> b = [-128]; b[0]--"), "Cannot store -129 in array<int8> (-128..127)", "-- past the range");
            expect("array<uint8> b = [1, 2]; for (auto& x : b) { x = x * 100; } to_json(b)", "[100,200]", "range-for by reference");
            expect(caught("array<uint8> b = [1, 3]; for (auto& x : b) { x = x * 100; } b.size()"), "Cannot store 300 in array<uint8> (0..255)", "range-for by reference past the range");
            expect("array<uint8> b = [9]; auto& r = b[0]; r = 10; to_string(b[0])", "10", "an element reference");
            expect(caught("array<uint8> b = [9]; auto& r = b[0]; int v = 999; r = v"), "Cannot store 999 in array<uint8> (0..255)", "an element reference past the range");
            // element conversions: int <-> uint8/int8 by value both ways, uint8 <-> int8 through the range, array<var> explicitly
            expect("array<uint8> b = [1, 2]; array<int> x = b; x[0] = 1000; to_json(x) + \" \" + to_json(b)", "[1000,2] [1,2]", "array<int> from array<uint8> copies");
            expect("array<int8> b = [-1, 2]; array<float> f = b; to_json(f)", "[-1.0,2.0]", "array<float> from array<int8>");
            expect("array<int> x = [1, 255]; array<uint8> b = x; b[0] = 9; to_json(b) + \" \" + to_json(x)", "[9,255] [1,255]", "array<uint8> from array<int> copies");
            expect(caught("array<int> x = [1, 300]; array<uint8> b = x"), "Cannot store 300 in array<uint8> (0..255)", "array<uint8> from an int past the range");
            expect(caught("array<int8> s = [-1]; array<uint8> b = s"), "Cannot store -1 in array<uint8> (0..255)", "array<uint8> from array<int8>");
            expect("array<uint8> u = [127]; array<int8> s = u; to_json(s)", "[127]", "array<int8> from array<uint8> in range");
            expect("array<float> f = [1.9, 2.1]; array<uint8> b = f; to_json(b)", "[1,2]", "array<uint8> from array<float>");
            expect("array<uint8> b = [5]; array<var> v = b; v.push(\"s\"); to_json(v) + \" \" + to_json(b)", "[5,\"s\"] [5]", "array<var> from array<uint8>");
            expect("var v = [1, 2, 3]; array<uint8> b = v; to_json(b)", "[1,2,3]", "array<uint8> from an untyped array");
            expect(caught("array<char> c = ['a']; var cv = c; array<uint8> b = cv"), "Cannot store char in array<uint8>", "a char does not convert");
            check(run("array<char> c = ['a']; array<uint8> b = c;", false, true).find("expected 'uint8', got 'char'") != std::string::npos, "the checker refuses a char buffer");
            expect(caught("array<uint8> b = [0]; var s = \"x\"; b[0] = s"), "Cannot assign 'string' to element of type 'uint8'", "a string element store");
            expect(caught("array<uint8> b = []; var s = \"x\"; b.push(s)"), "Cannot push 'string' to array<uint8>", "a string push");
            // the builtins
            expect("array<int8> s = [5, -3, 0]; s.sort(); array<uint8> u = [200, 3, 100]; u.sort(); to_json(s) + to_json(u)", "[-3,0,5][3,100,200]", "sort by value");
            expect("array<uint8> u = [1, 2, 3]; u.reverse(); var p = u.pop(); u.push(9); to_json(u) + to_string(p) + to_string(u.index_of(9)) + to_string(u.contains(3))", "[3,2,9]12true", "reverse, pop, push, index_of, contains");
            expect("array<uint8> u = [1, 2, 3, 4]; u.remove(0); to_json(u) + \" \" + to_json(array::slice(u, 1, 3)) + \" \" + u.join(\"-\")", "[2,3,4] [3,4] 2-3-4", "remove, slice, join");
            expect("array<uint8> u = [4, 5, 6]; u.remove_if([](int x) -> bool { return x == 5; }); to_json(u) + to_json(array::filter(u, [](int x) -> bool { return x > 4; }))", "[4,6][6]", "remove_if, filter");
            expect("array<uint8> u = [3, 1]; u.sort([](int a, int b) -> bool { return a > b; }); to_json(u)", "[3,1]", "comparator sort");
            expect("array<uint8> a = [1, 2]; array<uint8> b = a; b[0] = 9; array<uint8> c = [1, 2]; array<int> d = [1, 2]; to_string(a[0]) + to_string(a == c) + to_string(a == b) + to_string(a == d)", "1truefalsetrue", "copies copy, == is memberwise");
            expect("array<array<uint8>> g = [[1, 2], [3]]; g[1].push(255); to_json(g)", "[[1,2],[3,255]]", "nested byte buffers");
            expect(caught("array<array<uint8>> g = [[1]]; int v = 256; g[0].push(v)"), "Cannot store 256 in array<uint8> (0..255)", "a nested byte buffer keeps its range");
            expect("class P { array<uint8> data; } P p; p.data.push(7); p.data.push(200); P q = from_json(to_json(p)); int v = 256; string r = \"none\"; try { q.data.push(v); } catch (e) { r = e; } to_json(q.data) + \" \" + r",
                   "[7,200] Cannot store 256 in array<uint8> (0..255)", "json round trip keeps the declared kind");
            expect("class P { array<int8> data; } P p; p.data.push(-5); P q = from_binary(to_binary(p)); to_json(q.data)", "[-5]", "binary archive round trip");
            expect("array<uint8> b = [1, 2, 3, 4]; parallel_for (auto& x : b) { x = x * 2; } to_json(b)", "[2,4,6,8]", "parallel_for writes a byte buffer in place");
            // a scalar byte is an int; the checker reads literals against the range
            check(run("uint8 x = 5; x", false, true).find("is an array element type only") != std::string::npos, "a scalar uint8 is a parse error");
            check(run("function f(int8 v) { return v; } f(1)", true, true).find("is an array element type only") != std::string::npos, "an int8 parameter is a parse error");
            check(run("map<string, uint8> m = {}; m.size()", false, true).find("is an array element type only") != std::string::npos, "a uint8 map value is a parse error");
            check(run("array<uint8> b = [300];", false, true).find("300 is outside uint8 (0..255)") != std::string::npos, "the checker reads a literal element");
            check(run("array<int8> b = [0]; b[0] = -200;", true, true).find("-200 is outside int8 (-128..127)") != std::string::npos, "the checker reads a literal store");
            // one byte per element in the node
            auto e = jai::foundry::make_engine();
            auto v = e->execute("array<uint8> b = []; for (int i = 0; i < 1000; i++) { b.push(i % 256); } b");
            const jai::script_array* node = v.unchecked_array_node();
            check(node->kind() == jai::script_array::kind_t::u8, "array<uint8> holds the u8 byte buffer");
            check_eq((size_t)1000, node->bytes().size(), "one byte per element");
            check(e->execute("array<int8> b = [1]; b").unchecked_array_node()->kind() == jai::script_array::kind_t::i8, "array<int8> holds the i8 byte buffer");
            auto bytes = v.as<std::vector<uint8_t>>();
            check_eq((size_t)1000, bytes.size(), "std::vector<uint8_t> extraction");
            check_eq((int)255, (int)bytes[255], "extraction keeps each byte");
            // the host boundary: a std::vector<uint8_t> parameter takes the buffer, a bound_array writes it in place under
            // the range, a std::vector<uint8_t> result is an array<uint8>
            for (bool use_vm : {false, true}) {
                auto h = jai::foundry::make_default_engine();
                h->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                jai::stdlib::register_all(h);
                h->add_function("host_sum", [](std::vector<uint8_t> b) -> script_int { script_int s = 0; for (uint8_t x : b) { s += x; } return s; });
                h->add_function("host_fill", [](jai::bound_array<uint8_t>& b) -> script_int { b[0] = 9; b.push_back(200); return static_cast<script_int>(b.size()); });
                h->add_function("host_push", [](jai::bound_array<script_int>& b, script_int v) -> script_int { b.push_back(v); return static_cast<script_int>(b.size()); });
                h->add_function("host_bytes", []() -> std::vector<uint8_t> { return {1, 2, 255}; });
                const char* tag = use_vm ? " (vm)" : " (interpreter)";
                check_eq(std::string("195"), h->execute("to_string(host_sum(\"ab\".to_bytes()))").to_string(), std::string("a std::vector<uint8_t> parameter") + tag);
                check_eq(std::string("3 [9,2,200]"), h->execute("array<uint8> b = [1, 2]; var n = host_fill(b); to_string(n) + \" \" + to_json(b)").to_string(), std::string("a bound_array<uint8_t>& parameter") + tag);
                check_eq(std::string("Cannot store 300 in array<uint8> (0..255) [1]"),
                         h->execute("array<uint8> b = [1]; var out = \"none\"; try { host_push(b, 300); } catch (e) { out = e; } out + \" \" + to_json(b)").to_string(),
                         std::string("a bound_array write keeps the byte range") + tag);
                check_eq(std::string("[1,2,255] int"), h->execute("var b = host_bytes(); to_json(b) + \" \" + type_of(b[2])").to_string(), std::string("a std::vector<uint8_t> result") + tag);
                check(h->execute("host_bytes()").unchecked_array_node()->kind() == jai::script_array::kind_t::u8, std::string("a std::vector<uint8_t> result is an array<uint8>") + tag);
            }
        });

        test("typed_storage_errors_match_plain", [check_quad_error]() {
            check_quad_error("array<int> a = [1]; a[0] = \"s\"; a[0]", "store mismatch");
            check_quad_error("array<int> a = []; a.push(\"s\"); a.size()", "push mismatch");
            check_quad_error("array<int> a = [1]; a[5]", "read OOB");
            check_quad_error("array<int> a = [1]; a[3] = 2; a[0]", "store OOB");
            check_quad_error("array<int> a = [1]; for (auto& x : a) { x = \"s\"; } a[0]", "range-for ref mismatch");
        });

        test("typed_storage_builtins", [check_quad]() {
            check_quad("array<float> f = []; f.push(1); f.push(2.5); to_string(f.size()) + type_of(f[0]) + to_string(f.back())",
                       "2float2.5", "push/size/back");
            check_quad("array<int> a = [3,1,2]; a.sort(); to_string(a[0]) + to_string(a[1]) + to_string(a[2])", "123", "default sort");
            check_quad("array<int> a = [3,1,2]; a.sort([](auto x, auto y) -> auto { return x > y; }); to_string(a[0]) + to_string(a[2])", "31", "comparator sort");
            check_quad("array<int> a = [1,2,3]; a.reverse(); to_string(a[0])", "3", "reverse");
            check_quad("array<int> a = [1,2,3]; a.remove(1); to_string(a.size()) + to_string(a[1])", "23", "remove");
            check_quad("array<int> a = [1,2,3,4]; var n = a.remove_if([](auto x) -> auto { return x % 2 == 0; }); to_string(n) + \":\" + to_string(a.size()) + to_string(a[1])", "2:23", "remove_if");
            check_quad("array<int> a = [5,6,7]; to_string(a.index_of(6)) + to_string(a.has(7)) + to_string(a.has(9))", "1truefalse", "index_of/has");
            check_quad("array<int> a = [1,2,3,4]; var s = array::slice(a, 1,3); to_string(s.size()) + to_string(s[0])", "22", "slice");
            check_quad("array<int> a = [1,2]; to_string(a.pop()) + \":\" + to_string(a.size())", "2:1", "pop");
            check_quad("array<int> a = [1,2,3]; a.join(\"-\")", "1-2-3", "join");
            check_quad("array<int> a = [9]; to_string(a.front()) + to_string(a.first()) + to_string(a.back()) + to_string(a.last())", "9999", "front/back");
            check_quad("array<float> f = [1.5,2.5]; var t = 0.0; var r = array::filter(f, [](auto x) -> auto { return x > 2.0; }); to_string(r.size()) + to_string(r[0])", "12.5", "filter");
        });

        test("typed_storage_iteration_and_refs", [check_quad]() {
            check_quad("array<int> a = [1,2,3]; var s = 0; for (auto x : a) { s += x; } to_string(s)", "6", "range-for copy");
            check_quad("array<int> a = [1,2,3]; for (auto& x : a) { x *= 2; } to_string(a[0]) + to_string(a[2])", "26", "range-for ref mutates");
            check_quad("array<int> a = [5]; var& r = a[0]; r = 9; to_string(a[0])", "9", "var& element write");
            check_quad("array<int> a = [5]; var& r = a[0]; r += 2.5; type_of(a[0]) + \":\" + to_string(a[0])", "int:7", "ref compound converts");
            check_quad("array<int> a = [5]; var& r = a[0]; r++; to_string(a[0]) + to_string(r)", "66", "ref increment commits");
            check_quad("array<float> a = [1.5]; var& r = a[0]; r--; to_string(a[0])", "0.5", "float ref decrement");
            check_quad(R"(
                array<int> a = [10];
                function f(int& r) { r += 2.5; }
                f(a[0]);
                type_of(a[0]) + ":" + to_string(a[0])
            )", "int:12", "int& param through element");
            check_quad("array<int> a = [1,2]; var [x, y] = a; to_string(x) + to_string(y)", "12", "destructure");
        });

        test("typed_storage_value_semantics_and_laundering", [check_quad]() {
            // decl copy is a deep copy (kind-preserving buffer copy)
            check_quad("array<int> a = [1,2]; array<int> b = a; b[0] = 9; to_string(a[0]) + to_string(b[0])", "19", "typed->typed copy");
            // a var copy keeps the typed array: the element type holds, the variable stays a var
            check_quad("array<int> a = [1,2]; var v = a; string r = \"\"; try { v.push(\"s\"); } catch (e) { r = \"raised\"; } v.push(3); r + to_string(v.size()) + to_string(a.size())", "raised32", "a var copy keeps array<int>");
            // var& preserves the tag: still enforced
            check_quad("array<int> a = [1]; var& r = a; r.push(2); to_string(a.size())", "2", "var& push int");
            // shrink-under-ref errors identically
            check_quad("array<int> a = [1,2]; var v = a.pop(); to_string(v) + to_string(a.size())", "21", "pop shrink");
            // nested in maps/untyped containers: element keeps its tag + storage
            check_quad("var m = []; array<int> a = [7]; m.push(a); to_string(m[0][0])", "7", "typed inside untyped");
        });

        test("typed_to_array_var_conversion_rebuilds", [this, quad, check_quad, check_quad_error]() {
			// `array<var> x = typed;` is the explicit representation step (docs/site/guide/09-containers-strings.html
			// "Representation follows the declared type"):
			// every typed kind reboxes into a fresh script_value vector under the array<var> tag
			// (pushes of anything then land), the source buffer is untouched, and the two are
			// independent. Pre-fix the raw buffer survived under the array<var> tag and the
			// first heterogeneous push wrote a script_value into it (segfault, both backends).
			check_quad("array<int> ints = [1, 2, 3]; array<var> m = ints; m.push(\"four\"); ints.push(9); m[0] = \"zero\"; "
					   "to_json(m) + \"|\" + to_json(ints) + \"|\" + to_string(m.size()) + \"|\" + type_of(m) + \"|\" + type_of(m[1]) + \"|\" + type_of(m[3])",
					   "[\"zero\",2,3,\"four\"]|[1,2,3,9]|4|array|int|string", "int buffer -> array<var>");
			check_quad("array<float> fs = [1.5, 2.5]; array<var> m = fs; m.push(\"x\"); to_json(m) + \"|\" + to_json(fs) + \"|\" + type_of(m[0])",
					   "[1.5,2.5,\"x\"]|[1.5,2.5]|float", "float buffer -> array<var>");
			check_quad("array<bool> bs = [true, false]; array<var> m = bs; m.push(\"x\"); to_json(m) + \"|\" + to_json(bs) + \"|\" + type_of(m[0])",
					   "[true,false,\"x\"]|[true,false]|bool", "bool buffer -> array<var>");
			check_quad("array<char> cs = ['a', 'b']; array<var> m = cs; m.push(\"x\"); to_json(m) + \"|\" + to_json(cs) + \"|\" + type_of(m[0])",
					   "[{\"$char\":97},{\"$char\":98},\"x\"]|[{\"$char\":97},{\"$char\":98}]|char", "char buffer -> array<var>");
			check_quad("array<string> ss = [\"a\", \"b\"]; array<var> m = ss; m.push(5); m[0] = \"z\"; to_json(m) + \"|\" + to_json(ss) + \"|\" + type_of(m[2])",
					   "[\"z\",\"b\",5]|[\"a\",\"b\"]|int", "string buffer -> array<var>");
			check_quad("class K { int a = 1; K(int v) { a = v; } } array<K> ks = [K(1), K(2)]; array<var> m = ks; m.push(\"x\"); m[0].a = 9; "
					   "to_string(m.size()) + \"|\" + type_of(m[0]) + \"|\" + to_string(m[0].a) + \"|\" + to_string(ks[0].a) + \"|\" + type_of(m[2])",
					   "3|K|9|1|string", "object buffer -> array<var> (decl copies the instances)");
			check_quad("array<int> ints = []; array<var> m = ints; m.push(\"x\"); to_json(m) + \"|\" + to_json(ints)", "[\"x\"]|[]", "empty buffer -> array<var>");
			// the same table at every boundary: assignment, parameter, return, field store,
			// field initializer, cast
			check_quad("array<int> ints = [1, 2, 3]; array<var> m = []; m = ints; m.push(\"x\"); to_json(m) + \"|\" + to_json(ints)",
					   "[1,2,3,\"x\"]|[1,2,3]", "assignment");
			check_quad("array<int> ints = [1, 2, 3]; function f(array<var> a) { a.push(\"x\"); return to_json(a); } f(ints) + \"|\" + to_json(ints)",
					   "[1,2,3,\"x\"]|[1,2,3]", "parameter");
			check_quad("array<int> ints = [1, 2, 3]; array<var> f() { return ints; } array<var> r = f(); r.push(\"x\"); to_json(r) + \"|\" + to_json(ints)",
					   "[1,2,3,\"x\"]|[1,2,3]", "return into array<var>");
			check_quad("array<int> ints = [1, 2, 3]; array<var> f() { return ints; } var r = f(); r.push(\"x\"); to_json(r) + \"|\" + to_json(ints)",
					   "[1,2,3,\"x\"]|[1,2,3]", "return into var (the rebuild never aliases the source)");
			check_quad("class C { array<var> f = []; } array<int> ints = [1, 2, 3]; auto c = C(); c.f = ints; c.f.push(\"x\"); to_json(c.f) + \"|\" + to_json(ints)",
					   "[1,2,3,\"x\"]|[1,2,3]", "field store");
			check_quad("array<int> ints = [1, 2, 3]; class C { array<var> f = ints; } auto c = C(); c.f.push(\"x\"); to_json(c.f) + \"|\" + to_json(ints)",
					   "[1,2,3,\"x\"]|[1,2,3]", "field initializer");
			check_quad("array<int> ints = [1, 2, 3]; array<var> m = cast<array<var>>(ints); m.push(\"x\"); to_json(m) + \"|\" + to_json(ints)",
					   "[1,2,3,\"x\"]|[1,2,3]", "cast<array<var>>");
			// auto infers the buffer (no rebuild): still enforced
			check_quad("array<int> ints = [1, 2, 3]; auto a = ints; a.push(4); ints.push(9); to_json(a) + \"|\" + to_json(ints)", "[1,2,3,4]|[1,2,3,9]", "auto keeps array<int>");
			check_quad_error("array<int> ints = [1, 2, 3]; auto a = ints; a.push(\"x\"); a.size()", "auto keeps array<int> enforcement");
			// the opposite direction rebuilds the raw buffer from a vector holding only ints
			check_quad("array<int> ints = [1, 2, 3]; array<var> m = ints; m.push(4); array<int> back = m; back.push(5); m.push(\"s\"); to_json(back) + \"|\" + to_json(m)",
					   "[1,2,3,4,5]|[1,2,3,4,\"s\"]", "array<var> -> array<int> rebuilds the buffer");
			// element coercion between primitive buffers, independent both ways
			check_quad("array<int> ints = [1, 2]; array<float> fs = ints; fs.push(3.5); ints.push(7); to_json(fs) + \"|\" + type_of(fs[0]) + \"|\" + to_json(ints)",
					   "[1.0,2.0,3.5]|float|[1,2,7]", "array<int> -> array<float>");
			check_quad("array<float> fs = [1.5, 2.5]; array<int> ints = fs; ints.push(3); to_json(ints) + \"|\" + type_of(ints[0]) + \"|\" + to_json(fs)",
					   "[1,2,3]|int|[1.5,2.5]", "array<float> -> array<int>");
			// mismatches keep the documented boundary text on both backends
			auto mixed = quad("array<var> m = [1, \"two\"]; array<int> ints = m; ints.size()");
			check_eq(mixed[0], mixed[1], "mixed vector -> array<int> parity");
			check(mixed[0].find("Cannot store string in array<int>") != std::string::npos, "mixed vector -> array<int> raises the boundary text: " + mixed[0]);
			auto strs = quad("[[warn_mode]] array<string> ss = [\"a\"]; array<int> ints = ss; ints.size()");
			check_eq(strs[0], strs[1], "string buffer -> array<int> parity");
			check(strs[0].find("Cannot store string in array<int>") != std::string::npos, "string buffer -> array<int> raises the boundary text: " + strs[0]);
		});

        test("field_initializers_copy_like_declarations", [this, quad, check_quad]() {
            // `T f = expr;` in a class body is an initialization: an lvalue-shaped or
            // reference-wrapped initializer copies through clone_for_assignment exactly
            // like `T local = expr;` (containers and value objects deep-copy, shared_ptr
            // handles share, temporaries move). Pre-fix every instance aliased the
            // global's node on both backends.
            check_quad("array<int> ints = [1, 2, 3]; class C { array<int> f = ints; } C c = C(); c.f.push(4); to_json(ints) + \"|\" + to_json(c.f)",
                       "[1,2,3]|[1,2,3,4]", "typed array field from a global");
            check_quad("array<int> ints = [1, 2, 3]; class C { var f = ints; } C c = C(); c.f.push(4); to_json(ints) + \"|\" + to_json(c.f)",
                       "[1,2,3]|[1,2,3,4]", "var field from a global");
            check_quad("array<int> ints = [1, 2, 3]; class C { auto f = ints; } C c = C(); c.f.push(4); to_json(ints) + \"|\" + to_json(c.f)",
                       "[1,2,3]|[1,2,3,4]", "auto field from a global");
            check_quad("array<int> ints = [1, 2, 3]; class C { array<int> f = ints; } C c = C(); ints.push(9); to_json(c.f)",
                       "[1,2,3]", "the global's later growth never reaches the field");
            check_quad("array<int> ints = [1, 2, 3]; class C { array<int> f = ints; } C a = C(); C b = C(); a.f.push(4); to_string(b.f.size()) + \"|\" + to_string(ints.size())",
                       "3|3", "two instances are independent of each other and the global");
            check_quad("map<string, int> gm = {\"a\": 1}; class C { map<string, int> f = gm; } C c = C(); c.f[\"b\"] = 2; to_string(gm.size()) + \"|\" + to_string(c.f.size())",
                       "1|2", "map field from a global");
            check_quad("class P { int x = 1; } P gp = P(); class C { P f = gp; } C c = C(); c.f.x = 9; to_string(gp.x) + \"|\" + to_string(c.f.x)",
                       "1|9", "value-object field from a global");
            check_quad("class P { int x = 1; } class C { P a = P(); P b = a; } C c = C(); c.b.x = 7; to_string(c.a.x) + \"|\" + to_string(c.b.x)",
                       "1|7", "value-object field from an earlier field");
            check_quad("string gs = \"hi\"; class C { string f = gs; } C c = C(); c.f += \"!\"; gs + \"|\" + c.f",
                       "hi|hi!", "string field from a global");
            check_quad("class H { array<int> inner = [5]; } H gh = H(); class C { array<int> f = gh.inner; } C c = C(); c.f.push(6); to_json(gh.inner) + \"|\" + to_json(c.f)",
                       "[5]|[5,6]", "member-expression initializer");
            // subscript initializers arrive as element reference wrappers: the field holds
            // the VALUE (pre-fix a var field kept the wrapper and died with the element)
            check_quad("array<array<int>> grid = [[1, 2], [3, 4]]; class C { var f = grid[0]; } C c = C(); c.f.push(9); grid.clear(); to_string(c.f.size())",
                       "3", "var field from a subscript survives the source element");
            check_quad("array<int> ints = [1, 2, 3]; class C { var f = ints[0]; } C c = C(); c.f = 50; to_string(ints[0]) + \"|\" + to_string(c.f)",
                       "1|50", "var field from a scalar element: the write lands on the field");
            check_quad("array<int> ints = [1, 2, 3]; class C { int f = ints[1]; } C c = C(); c.f = 60; to_string(ints[1]) + \"|\" + to_string(c.f)",
                       "2|60", "typed field from a scalar element");
            check_quad("class C { int a = 1; int b = a; array<int> xs = [1, 2]; array<int> ys = xs; } C c = C(); c.a = 5; c.ys.push(3); "
                       "to_string(c.b) + \"|\" + to_json(c.xs) + \"|\" + to_json(c.ys)",
                       "1|[1,2]|[1,2,3]", "field from an earlier field");
            check_quad("class P { int x = 1; } shared_ptr<P> sp = new P(); class C { shared_ptr<P> f = sp; var g = sp; } C c = C(); c.f.x = 42; c.g.x = 43; to_string(sp.x)",
                       "43", "shared_ptr initializers share the handle");
            check_quad("class C { array<int> f; C(array<int> a) { f = a; } } array<int> src = [1]; C c = C(src); c.f.push(2); to_json(src) + \"|\" + to_json(c.f)",
                       "[1]|[1,2]", "ctor-body store from a parameter (pin)");
            check_quad("array<int> ints = [1, 2, 3]; class A { array<int> f = ints; A(int v) {} } class B : A { B() : super(1) {} } B b = B(); b.f.push(4); to_json(ints) + \"|\" + to_json(b.f)",
                       "[1,2,3]|[1,2,3,4]", "super-chain per-level initializer pass");
            check_quad("array<int> ints = [1, 2, 3]; class B1 { array<int> bf = ints; } class D1 : B1 { int d = 0; } D1 d = D1(); d.bf.push(4); to_json(ints) + \"|\" + to_json(d.bf)",
                       "[1,2,3]|[1,2,3,4]", "inherited initializer through the recursive pass");
            check_quad("array<int> ints = [1, 2, 3]; class S { static array<int> s = ints; } S::s.push(7); to_json(ints) + \"|\" + to_json(S::s)",
                       "[1,2,3]|[1,2,3,7]", "static initializer");
            check_quad("array<int> ints = [1]; class C { int a = 1; } C c1 = C(); C c2 = C(); class C { int a = 1; array<int> g = ints; } "
                       "c1.g.push(2); C c3 = C(); c3.g.push(3); to_json(ints) + \"|\" + to_json(c1.g) + \"|\" + to_json(c2.g) + \"|\" + to_json(c3.g)",
                       "[1]|[1,2]|[1]|[1,3]", "hot reload seeds per instance; later constructions stay independent");
            check_quad("function mk() { return [1, 2]; } class C { array<int> f = mk(); } C c = C(); c.f.push(3); to_json(c.f)",
                       "[1,2,3]", "temporaries still initialize");
            check_quad("array<int> ints = [1, 2, 3]; class B { int x = 0; } class C : B { array<int> f = ints; } B b = B(); extend(b, C); b.f.push(4); to_json(ints) + \"|\" + to_json(b.f)",
                       "[1,2,3]|[1,2,3,4]", "extend() runs the same pass");
            // the field table still converts and rejects with its own text
            check_quad("float gf = 2.5; class C { int f = gf; } C c = C(); type_of(c.f) + \":\" + to_string(c.f)", "int:2", "conversion through the field table");
            auto rej = quad("var gs = \"s\"; class CD { int f = gs; } CD cd = CD(); cd.f");
            check_eq(rej[0], rej[1], "rejection parity");
            check(rej[0].find("Cannot assign string to field 'f' of CD (declared int)") != std::string::npos, "rejection keeps the field-store text: " + rej[0]);
        });

        test("typed_storage_parallel_bridge", [check_quad]() {
            // a typed parallel source demotes in place at the barrier: results identical
            check_quad(R"(
                array<int> a = [1, 2, 3, 4];
                parallel_for (auto& x : a) { x = x * 2; }
                to_string(a[0]) + to_string(a[3])
            )", "28", "parallel_for over typed");
            check_quad(R"(
                array<float> f = [1.5, 2.5];
                function dbl(var x) { return x * 2.0; }
                var r = parallel_transform(f, dbl);
                to_string(r[1])
            )", "5.0", "parallel_transform over typed");
            check_quad(R"(
                array<int> pal = [10, 20, 30];
                var src = [0, 1, 2];
                function lookup(var i) { return pal[i]; }
                var r = parallel_transform(src, lookup);
                to_string(r[2])
            )", "30", "typed array as parallel capture");
        });

        test("typed_storage_engages_raw_buffers", [this]() {
            // Not just parity: declared element types must actually produce typed nodes
            // (and the laundering path must actually demote)
            auto e = jai::foundry::make_engine();
            jai::stdlib::register_all(e);
            auto v = e->execute("array<int> a = [1,2,3]; a");
            check(v.is_array(), "returns array");
            check(v.unchecked_array_node()->kind() == jai::script_array::kind_t::i64, "array<int> holds an i64 buffer");
            auto f = e->execute("array<float> b = [1.5]; b");
            check(f.unchecked_array_node()->kind() == jai::script_array::kind_t::f64, "array<float> holds an f64 buffer");
            auto b = e->execute("array<bool> flags = [true, false, true]; flags");
            check(b.unchecked_array_node()->kind() == jai::script_array::kind_t::b8, "array<bool> holds a byte buffer");
            auto c = e->execute("array<char> cs = ['a', 'z']; cs");
            check(c.unchecked_array_node()->kind() == jai::script_array::kind_t::c8, "array<char> holds a byte buffer");
            auto ab = e->execute("auto inferred = [true, false]; inferred");
            check(ab.unchecked_array_node()->kind() == jai::script_array::kind_t::b8, "auto bool literal infers the b8 buffer");
            auto st = e->execute("array<string> names = [\"a\", \"b\"]; names");
            check(st.unchecked_array_node()->kind() == jai::script_array::kind_t::str, "array<string> holds a handle buffer");
            auto as = e->execute("auto words = [\"x\", \"y\"]; words");
            check(as.unchecked_array_node()->kind() == jai::script_array::kind_t::str, "auto string literal infers the handle buffer");
            auto d = e->execute("array<int> c = [1]; var laundered = c; laundered");
            check(d.unchecked_array_node()->kind() == jai::script_array::kind_t::i64, "a var copy keeps the raw buffer");
            auto o = e->execute("class K { int a = 1; } array<K> ks = [K(), K()]; ks");
            check(o.unchecked_array_node()->kind() == jai::script_array::kind_t::obj, "array<K> holds an object slot buffer");
            auto oe = e->execute("class K2 { int a = 1; } array<K2> none = []; none");
            check(oe.unchecked_array_node()->kind() == jai::script_array::kind_t::obj, "an empty array<K> is an object slot buffer");
            auto ol = e->execute("class K3 { int a = 1; } array<K3> k3 = [K3()]; var laundered3 = k3; laundered3");
            check(ol.unchecked_array_node()->kind() == jai::script_array::kind_t::obj, "a var copy keeps the object slot buffer");
            auto oa = e->execute("class K4 { int a = 1; } auto inferred4 = [K4(), K4()]; inferred4");
            check(oa.unchecked_array_node()->kind() == jai::script_array::kind_t::hetero, "an auto object literal stays dynamic");
            auto os = e->execute("class K5 { int a = 1; } shared_ptr<K5> sp = K5(); array<K5> ks5 = [sp]; ks5");
            check(os.unchecked_array_node()->kind() == jai::script_array::kind_t::obj, "a shared_ptr-tagged element lives in the object buffer");
        });

        test("open_element_boundary_rebuilds_typed_source", [this]() {
            // The boundary kernel itself: a typed node crossing into array<var> comes back as a
            // FRESH hetero node under the target tag; the source buffer is untouched. The stamp
            // net (set_type_info) demotes any typed node that still meets an open-element tag.
            auto e = jai::foundry::make_engine();
            jai::stdlib::register_all(e);
            auto ints = e->execute("array<int> ints = [1, 2, 3]; ints");
            check(ints.unchecked_array_node()->kind() == jai::script_array::kind_t::i64, "source is an i64 buffer");
            jai::type_info_ptr open{detail::engine_access::get_type_info_array(*e, detail::engine_access::get_type_info_any(*e))};
            auto out = jai::detail::enforce_container_boundary(ints, open, e.get());
            check(out.value.has_value(), "open boundary admits a typed source");
            check(out.value->unchecked_array_node() != ints.unchecked_array_node(), "rebuilt into a fresh node");
            check(out.value->unchecked_array_node()->kind() == jai::script_array::kind_t::hetero, "array<var> is the script_value-vector form");
            check(out.value->get_type_info().get() == open.get(), "rebuilt value carries the target tag");
            check_eq((size_t)3, out.value->unchecked_array_node()->size(), "elements carried over");
            check_eq((int64_t)3, out.value->unchecked_array_node()->values()[2].as_int(), "elements boxed in order");
            check(ints.unchecked_array_node()->kind() == jai::script_array::kind_t::i64, "source buffer untouched");
            auto m = e->execute("array<var> m = ints; m.push(\"four\"); m");
            check(m.unchecked_array_node()->kind() == jai::script_array::kind_t::hetero, "the decl lands the vector form");
            check_eq((size_t)4, m.unchecked_array_node()->size(), "push after the conversion");
            auto ks = e->execute("class KO { int a = 1; } array<KO> ks = [KO()]; array<var> mo = ks; mo");
            check(ks.unchecked_array_node()->kind() == jai::script_array::kind_t::hetero, "an object buffer reboxes too");
            auto stamped = e->execute("array<float> fs = [1.5]; fs").clone();
            check(stamped.unchecked_array_node()->kind() == jai::script_array::kind_t::f64, "clone keeps the buffer");
            stamped.set_type_info(open);
            check(stamped.unchecked_array_node()->kind() == jai::script_array::kind_t::hetero, "stamping an open-element tag demotes the node");
        });

        // type_of on a shared-pointer-semantic instance must match a value instance: both name the class.
        test("type_of_shared_and_value_instance_both_name_the_class", [this, check_both]() {
            const char* decl = "class P { int v = 0; } ";
            check_both((std::string(decl) + "auto p = P(); type_of(p)").c_str(), "P", "value instance");
            check_both((std::string(decl) + "var p = new P(); type_of(p)").c_str(), "P", "new P()");
            check_both((std::string(decl) + "shared_ptr<P> p = P(); type_of(p)").c_str(), "P", "shared_ptr<P> p = P()");
            check_both((std::string(decl) + "auto p = shared_ptr<P>(); type_of(p)").c_str(), "P", "auto p = shared_ptr<P>()");
            // value == shared, on the same class, in one program
            check_both((std::string(decl) +
                "auto a = P(); auto b = new P(); type_of(a) + \"==\" + type_of(b)").c_str(),
                "P==P", "value == shared");
        });

        // ===== PARSE-TIME TYPE PROOFS (docs/site/embedding/06-static-checking.html "Proofs") =====
        // A proof selects a lane and a total runtime guard makes it sound, so every program
        // must read the same on {interp, vm} x {type_proofs on, off}. Segments run in order on
        // one engine per lane; a class unit is executed three times so its field proofs arm
        // (first execute -> stamped -> identical-reload hit) before the rows that use them.
        auto proof_run = [](const std::vector<std::string>& segments) {
            std::array<std::string, 4> out;   // interp/proofs, interp/plain, vm/proofs, vm/plain
            int idx = 0;
            for (bool use_vm : {false, true}) {
                for (bool proofs : {true, false}) {
                    auto e = jai::foundry::make_default_engine();
                    e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                    e->type_proofs(proofs);
                    e->execution_budget(0);
                    jai::stdlib::register_all(e);
                    std::string acc;
                    for (const auto& seg : segments) {
                        try { acc += e->execute(seg).to_string(); }
                        catch (const std::exception& ex) { acc += std::string("ERROR: ") + ex.what(); }
                        acc += '\x1f';
                    }
                    out[idx++] = acc;
                }
            }
            return out;
        };
        auto last_outcome = [](const std::string& joined) {
            const std::string body = joined.substr(0, joined.empty() ? 0 : joined.size() - 1);
            const size_t cut = body.rfind('\x1f');
            return cut == std::string::npos ? body : body.substr(cut + 1);
        };
        auto check_proof_lanes = [this](const std::array<std::string, 4>& r, const std::string& what) {
            const char* lane[4] = {"interp/proofs", "interp/plain", "vm/proofs", "vm/plain"};
            for (int i = 0; i < 4; ++i) { check_eq(r[1], r[i], std::string(lane[i]) + " vs interp/plain: " + what); }
        };
        // every lane equal; the final segment's outcome pinned when `expected` is non-empty
        auto check_proof_run = [this, proof_run, last_outcome, check_proof_lanes](const std::vector<std::string>& segments, const std::string& expected, const std::string& what) {
            auto r = proof_run(segments);
            check_proof_lanes(r, what);
            if (!expected.empty()) { check_eq(expected, last_outcome(r[0]), "value: " + what); }
        };
        auto check_proof_error = [this, proof_run, last_outcome, check_proof_lanes](const std::vector<std::string>& segments, const std::string& what) {
            auto r = proof_run(segments);
            check_proof_lanes(r, what);
            check(last_outcome(r[0]).rfind("ERROR", 0) == 0, "raises: " + what + " -> " + last_outcome(r[0]));
        };
        auto thrice = [](const std::string& unit) { return std::vector<std::string>{unit, unit, unit}; };
        auto thrice_then = [](const std::string& unit, const std::string& tail) { return std::vector<std::string>{unit, unit, unit, tail}; };

        test("proof_battery_same_type_stores", [check_proof_run, check_proof_error, thrice, thrice_then]() {
            // per certain kind: env-global `=`, decl init, bare `f =`, `this.f =`, `o.f =`,
            // typed and var parameters, typed return, type_of after the stores, then a
            // mismatching store that must take the ladder behind the (now stale) proof
            struct kind_row { const char* type; const char* a; const char* b; const char* mismatch; const char* expect; const char* expect_mismatch; };
            const kind_row rows[] = {
                {"int",    "1",     "2",     "2.5", "int:2:int:2:2:1:2",                      "int:2:int:2"},
                {"float",  "1.5",   "2.5",   "3",   "float:2.5:float:2.5:2.5:1.5:2.5", "float:3.0:float:3.0"},
                {"bool",   "true",  "false", "1",   "bool:false:bool:false:false:true:false", "bool:true:bool:true"},
                {"char",   "'a'",   "'b'",   "65",  "char:b:char:b:b:a:b",                    ""},
                {"string", "\"a\"", "\"b\"", "3",   "string:b:string:b:b:a:b",                ""},
            };
            for (const kind_row& k : rows) {
                const std::string T = k.type;
                const std::string unit =
                    "class C { " + T + " f = " + k.a + ";\n"
                    "  " + T + " set(" + T + " v) { f = v; this.f = v; return f; }\n"
                    "  " + T + " pass(var v) { " + T + " local = v; local = v; return local; }\n"
                    "  " + T + " viaArg(" + T + " v) { return set(v); } }\n"
                    + T + " g = " + k.a + "; g = " + k.b + ";\n"
                    + T + " fn(" + T + " v) { " + T + " local = v; local = " + k.b + "; return local; }\n"
                    "C c = C(); c.f = " + k.b + "; " + T + " r = c.set(" + k.a + "); r = c.viaArg(" + k.b + "); " + T + " q = c.pass(" + k.a + ");\n"
                    "type_of(g) + \":\" + to_string(g) + \":\" + type_of(c.f) + \":\" + to_string(c.f) + \":\" + to_string(r) + \":\" + to_string(q) + \":\" + to_string(fn(" + k.a + "))";
                const std::string mismatch =
                    "c.f = " + std::string(k.mismatch) + "; g = " + k.mismatch + "; type_of(c.f) + \":\" + to_string(c.f) + \":\" + type_of(g) + \":\" + to_string(g)";
                check_proof_run(thrice(unit), k.expect, T + " same-type lanes");
                if (T == "char" || T == "string") { check_proof_error(thrice_then(unit, mismatch), "int into a proven " + T + " field/global raises"); }
                else { check_proof_run(thrice_then(unit, mismatch), k.expect_mismatch, T + " mismatching store takes the ladder"); }
            }
            const std::string obj_unit =
                "class K { int n = 0; K(int v) { n = v; } }\n"
                "class C { K f = K(0);\n"
                "  K set(K v) { f = v; this.f = v; return f; }\n"
                "  K pass(var v) { K local = v; local = v; return local; }\n"
                "  K viaArg(K v) { return set(v); } }\n"
                "K g = K(1); g = K(2);\n"
                "K fn(K v) { K local = v; local = K(2); return local; }\n"
                "C c = C(); c.f = K(2); K r = c.set(K(1)); r = c.viaArg(K(2)); K q = c.pass(K(1));\n"
                "type_of(g) + \":\" + to_string(g.n) + \":\" + type_of(c.f) + \":\" + to_string(c.f.n) + \":\" + to_string(r.n) + \":\" + to_string(q.n) + \":\" + to_string(fn(K(1)).n)";
            check_proof_run(thrice(obj_unit), "K:2:K:2:2:1:2", "object same-type lanes");
            check_proof_run(thrice_then(obj_unit, "c.f = 5; g = 6; to_string(c.f.n) + \":\" + to_string(g.n)"), "", "int into an object slot takes the ctor-conversion ladder");
            check_proof_error(thrice_then(obj_unit, "class J { int m = 0; } g = J(); g.n"), "unrelated class into an object slot raises");
            check_proof_run(thrice_then(obj_unit, "c.f = null; type_of(c.f)"), "", "null into an object field takes the ladder");
        });

        test("proof_battery_not_certain", [check_proof_run, check_proof_error, thrice_then]() {
            // nothing here is certain (var, auto without initializer, handles, containers,
            // references, by-ref params, callable locals, coroutines, null, placeholders,
            // derived-into-base, handle-into-value): the ladder runs and all lanes agree
            const std::string cls = "class C { int v = 1; } class D : C { int w = 2; }\n";
            check_proof_run({"var v = 1; v = \"s\"; v = 2.5; type_of(v) + \":\" + to_string(v)"}, "float:2.5", "var");
            check_proof_run({"auto n; n = 3; n = 4; type_of(n) + \":\" + to_string(n)"}, "int:4", "auto without initializer");
            check_proof_error({"auto n; n = 3; n = \"s\"; n"}, "auto locks at its first assignment");
            check_proof_run({cls + "shared_ptr<C> sp = C(); shared_ptr<C> sq = sp; sq.v = 5; to_string(sp.v)"}, "5", "shared_ptr handle");
            check_proof_run({cls + "shared_ptr<C> sp = C(); weak_ptr<C> wp = sp; to_string(wp.expired()) + \":\" + to_string(wp.lock().v)"}, "false:1", "weak_ptr handle");
            check_proof_run({"array<int> a = [1, 2]; array<int> b = a; b[0] = 9; to_string(a[0]) + to_string(b[0])"}, "19", "typed array copies");
            check_proof_run({"map<string,int> m = {\"a\": 1}; map<string,int> n = m; n[\"a\"] = 2; to_string(m[\"a\"]) + to_string(n[\"a\"])"}, "12", "typed map copies");
            check_proof_run({"int x = 1; int& r = x; r = 5; to_string(x)"}, "5", "reference declaration");
            check_proof_run({"void bump(int& r) { r = r + 1; } int x = 1; bump(x); to_string(x)"}, "2", "by-reference parameter");
            check_proof_run({"int twice(int v) { return v * 2; } var f = twice; to_string(f(4))"}, "8", "callable local");
            check_proof_error({"int twice(int v) { return v * 2; } var f = twice; f(\"s\")"}, "callable local still enforces its parameter");
            check_proof_run({"coroutine int gen(int n) { yield n; return n + 1; } auto h = gen(3); to_string(h.next()) + \":\" + to_string(h.next())"}, "3:4", "coroutine callee");
            check_proof_run({cls + "C a = C(); a = null; type_of(a)"}, "", "null into an object slot");
            check_proof_run({cls + "C a; C b = a; type_of(b)"}, "", "typed-null placeholder");
            check_proof_run({cls + "C b = D(); to_string(b.v) + \":\" + type_of(b)"}, "1:D", "derived into base");
            check_proof_run({cls + "shared_ptr<C> sp = C(); C c = sp; c.v = 9; to_string(sp.v) + \":\" + to_string(c.v)"}, "", "handle into a value slot");
            check_proof_error({"int x = 1; x = \"s\"; x"}, "string into int raises");
            check_proof_error({"char c = 'a'; c = 65; c"}, "int into char raises");
            check_proof_error({"int f(int v) { return v; } f(\"s\")"}, "string argument into an int parameter raises");
            check_proof_error({"int g() { return \"s\"; } g()"}, "string returned from an int function raises");
            check_proof_error({cls + "class J { int m = 1; } C c = C(); c = J(); c"}, "unrelated class into an object slot raises");
            const std::string armed = "class F { int v = 1; int set(int x) { v = x; return v; } } F f = F(); f.set(2); f.v";
            check_proof_error(thrice_then(armed, "f.set(\"z\")"), "string into a proven int parameter raises after arming");
            check_proof_error(thrice_then(armed, "f.v = \"z\"; f.v"), "string into a proven member store raises after arming");
        });

        test("proof_call_arg_identity_guard", [check_proof_run, check_proof_error, thrice_then]() {
            // a stamped call site names its callee body: another function bound to the same
            // name, a later redefinition, overload sets, defaults and by-ref params all run
            // the unproven path and every lane agrees
            check_proof_run({"int f(int v) { return v + 1; } int other(int v) { return v * 10; } int a = f(1); f = other; int b = f(1); to_string(a) + \":\" + to_string(b)"}, "", "global function rebound between calls");
            check_proof_run({"int f(int v) { return v + 1; } var use(var a) { return f(a); } to_string(use(2))",
                             "string f(string s) { return s + \"!\"; } to_string(use(\"2\"))",
                             "to_string(use(\"3\"))"}, "3!", "later execute redefines the callee with another signature");
            check_proof_run({"int f(int v) { return v + 1; } var use() { return f(2); } to_string(use())",
                             "string f(string s) { return s + \"!\"; } to_string(use())"}, "3", "a later execute adds an overload of another signature: the stamped int argument keeps the int overload");
            check_proof_run({"class O { int m(int v) { return v; } int m(int a, int b) { return a + b; } int m(string s) { return 100; } }\n"
                             "O o = O(); to_string(o.m(1)) + \":\" + to_string(o.m(1, 2)) + \":\" + to_string(o.m(\"s\"))"}, "1:3:100", "overloaded methods");
            check_proof_run({"int d(int a, int b = 5) { return a + b; } to_string(d(1)) + \":\" + to_string(d(1, 2))"}, "6:3", "defaults");
            check_proof_run({"void inc(int& r, int by) { r = r + by; } int x = 1; inc(x, 2); to_string(x)"}, "3", "by-reference parameter");
            const std::string unit = "class O { int m(int v) { return v * 2; } } O o = O(); int use2() { return o.m(3); } to_string(use2())";
            check_proof_run(thrice_then(unit, "class O { int m(int v) { return v * 2; } int m(string s) { return 7; } } to_string(use2()) + \":\" + to_string(o.m(\"s\"))"), "6:7", "reload grows the overload set behind a stamped site");
            check_proof_run(thrice_then(unit, "class O { string m(int v) { return \"m\"; } } to_string(use2())"), "", "reload retypes the callee's return behind a stamped site");
            check_proof_error(thrice_then("int twice(int v) { return v * 2; } int r = twice(4); to_string(twice(r))", "twice(\"s\")"), "string into a proven int parameter raises");
        });

        test("proof_member_second_execute", [check_proof_run, thrice]() {
            // the same source three times on one engine: first execute (no field proofs),
            // second (stamped, then the first redefinition's full-path reload), third
            // (re-stamped; identical reloads keep hitting) - equal across passes and lanes
            const std::string unit =
                "class P { int hp = 10; float x = 1.5; string tag = \"p\"; bool alive = true; char c = 'a';\n"
                "  int tick(int d) { hp = hp - d; this.hp = this.hp + 1; x = x * 2.0; tag = tag + \"!\"; alive = !alive; c = 'b'; return hp; } }\n"
                "P p = P(); int r = p.tick(3); r = r + p.tick(1); p.hp = r;\n"
                "to_string(r) + \":\" + to_string(p.hp) + \":\" + type_of(p.hp) + \":\" + type_of(p.x) + \":\" + p.tag + \":\" + to_string(p.alive) + \":\" + to_string(p.c)";
            check_proof_run(thrice(unit), "16:16:int:float:p!!:true:b", "self-defining unit executed three times");
            // module pattern: the class unit runs three times before its users' first check
            const std::string cls = "class Q { int f = 10; string tag = \"q\"; int tick(int d) { f = f - d; this.f = this.f + 1; tag = tag + \"!\"; return f; } Q dup() { Q q = Q(); q.f = f; q.tag = tag; return q; } }";
            const std::string users = "Q q = Q(); int r2 = q.tick(3); Q d = q.dup(); d.f = r2; d.tag = q.tag; to_string(r2) + \":\" + to_string(q.f) + \":\" + to_string(d.f) + \":\" + d.tag + \":\" + to_string(q.dup().f)";
            check_proof_run({cls, cls, cls, users, users, users}, "8:8:8:q!:8", "module pattern");
        });

        test("proof_stats_fire_and_kill_switch", [this]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            // the runner's backend: the proof pass stamps every kind on an armed unit (the
            // counters prove the lanes are reachable), a type_proofs(false) twin stamps nothing,
            // and both read identically at every step - including a switch flipped mid-run
            auto on = make_engine();
            auto off = make_engine();
            off->type_proofs(false);
            jai::stdlib::register_all(on);
            jai::stdlib::register_all(off);
            auto outcome = [](jai::engine& e, const std::string& src) -> std::string {
                try { return "VALUE " + e.execute(src).to_string(); }
                catch (const std::exception& ex) { return std::string("ERROR ") + ex.what(); }
            };
            const std::string unit =
                "class P { int hp = 10; float x = 1.5; string tag = \"p\"; bool alive = true; char c = 'a';\n"
                "  int tick(int d) { hp = hp - d; this.hp = this.hp + 1; x = x * 2.0; tag = tag + \"!\"; alive = !alive; c = 'b'; return hp; } }\n"
                "P p = P(); int r = p.tick(3); r = r + p.tick(1); p.hp = r;\n"
                "to_string(r) + \":\" + to_string(p.hp) + \":\" + type_of(p.hp) + \":\" + p.tag";
            const std::string use = "{ int k = 0; k = 2; r = p.tick(k); } p.hp = r; p.tag = \"t\"; to_string(p.hp) + \":\" + p.tag";
            const std::string bad = "p.hp = \"x\"; p.hp";
            for (int pass = 0; pass < 3; ++pass) {
                check_eq(outcome(*off, unit), outcome(*on, unit), "unit pass " + std::to_string(pass));
            }
            const proof_stats st = on->last_proof_stats();
            check_ge(st.stores, (uint32_t)1, "same-type local stores");
            check_ge(st.args, (uint32_t)1, "int argument into an int parameter");
            check_ge(st.returns, (uint32_t)1, "int returned from an int method");
            const proof_stats none = off->last_proof_stats();
            check_eq((uint32_t)0, none.stores + none.args + none.returns, "the twin stamps nothing");
            for (int pass = 0; pass < 3; ++pass) {
                const std::string got = outcome(*on, use);
                check_eq(outcome(*off, use), got, "use pass " + std::to_string(pass));
                if (pass == 0) { check_eq(std::string("VALUE 15:t"), got); }
                const std::string rejected = outcome(*on, bad);
                check_eq(outcome(*off, bad), rejected, "bad pass " + std::to_string(pass));
                check(rejected.rfind("ERROR", 0) == 0, "string into a proven int field raises: " + rejected);
            }
            on->type_proofs(false);
            check_eq(outcome(*off, use), outcome(*on, use), "switched off mid-run");
            check_eq((uint32_t)0, on->last_proof_stats().stores, "stripped at the next execute");
            on->type_proofs(true);
            check_eq(outcome(*off, use), outcome(*on, use), "switched back on");
            check_ge(on->last_proof_stats().stores, (uint32_t)1, "re-stamped at the next execute");
        });

        // ===== USER ACCESSORS ON MEMBER ACCESS =====
        // expr.x (any receiver) reads through the nearest USER _get_x in the chain and stores
        // through the nearest USER _set_x; the synthesized field accessors never shadow one.
        // Bare x inside the chain's methods is the backing field. Both backends, strict mode.
        test("accessor_member_reads_and_stores", [check_both]() {
            check_both(
                "class Q { int b = 1; int _get_b() { return 42; } int _set_b(int v) { b = v * 10; return b; } int raw() { return b; } }\n"
                "var q = Q();\n"
                "string r = to_string(q.b);\n"
                "q.b = 2;\n"
                "r = r + \":\" + to_string(q.b) + \":\" + to_string(q.raw());\n"
                "r = r + \":\" + to_string(q._get_b()) + \":\" + to_string(q._set_b(3)) + \":\" + to_string(q.raw());\n"
                "var y = (q.b = 5);\n"
                "r + \":\" + to_string(y) + \":\" + to_string(q.raw())",
                "42:42:20:42:30:30:42:50", "same-class _get_b/_set_b own the member; a consumed store reads back through _get_b");
        });

        test("accessor_bare_access_in_body", [check_both]() {
            check_both(
                "class C { int b = 1; int _get_b() { return b + 100; } void _set_b(int v) { b = v * 10; }\n"
                "  int bare() { return b; } int viaThis() { return this.b; } void bareStore(int v) { b = v; } }\n"
                "var c = C();\n"
                "string r = to_string(c.b) + \":\" + to_string(c.bare()) + \":\" + to_string(c.viaThis());\n"
                "c.bareStore(5);\n"
                "r = r + \":\" + to_string(c.b) + \":\" + to_string(c.bare());\n"
                "c.b = 7;\n"
                "r + \":\" + to_string(c.bare()) + \":\" + to_string(c.viaThis())",
                "101:1:101:105:5:70:170", "bare b is the backing field (no recursion), this.b is the accessor");
        });

        test("accessor_compound_and_incdec", [check_both]() {
            check_both(
                "class C { int b = 1; int _get_b() { return b + 100; } void _set_b(int v) { b = v * 10; } int raw() { return b; } }\n"
                "var c = C();\n"
                "c.b += 1;\n"
                "string r = to_string(c.b) + \":\" + to_string(c.raw());\n"
                "var old = c.b++;\n"
                "r = r + \":\" + to_string(old) + \":\" + to_string(c.b) + \":\" + to_string(c.raw());\n"
                "c.b -= 10;\n"
                "r = r + \":\" + to_string(c.b) + \":\" + to_string(c.raw());\n"
                "var pre = --c.b;\n"
                "r + \":\" + to_string(pre) + \":\" + to_string(c.b) + \":\" + to_string(c.raw())",
                "1120:1020:1120:11310:11210:113100:113000:1131090:1131090:1130990",
                "compound reads through _get_b and writes through _set_b; postfix yields the old getter value, prefix the getter's value after the store");
        });

        test("accessor_child_over_parent_field", [check_both]() {
            // no `override` needed over a base's SYNTHESIZED accessor; `override` still accepted
            check_both(
                "class P { int b = 1; }\n"
                "class C : P { int _get_b() { return b + 100; } void _set_b(int v) { b = v * 10; } int raw() { return b; } }\n"
                "var c = C();\n"
                "string r = \"\";\n"
                "for (int i = 0; i < 3; ++i) { r = r + to_string(c.b) + \",\"; }\n"
                "for (int i = 0; i < 3; ++i) { c.b = i + 2; r = r + to_string(c.b) + \",\"; }\n"
                "int hot() { int s = 0; for (int i = 0; i < 5; ++i) { s += c.b; } return s; }\n"
                "r = r + to_string(hot()) + \",\" + to_string(hot()) + \",\";\n"
                "void hots(int v) { for (int i = 0; i < 3; ++i) { c.b = v; } }\n"
                "hots(7); r = r + to_string(c.raw()) + \",\"; hots(9); r + to_string(c.raw())",
                "101,101,101,120,130,140,700,700,70,90", "child accessor over a parent field: every read/store dispatches (no armed slot)");
            check_both(
                "class P { int b = 1; } class C : P { override int _get_b() { return b + 100; } } var c = C(); to_string(c.b)",
                "101", "override spelling accepted");
        });

        test("accessor_parent_over_child_field", [check_both]() {
            check_both(
                "class P { int log = 0; int _get_b() { return 42; } void _set_b(int v) { log = log + v; } }\n"
                "class C : P { int b = 1; int raw() { return b; } }\n"
                "var c = C();\n"
                "string r = to_string(c.b);\n"
                "c.b = 2;\n"
                "r + \":\" + to_string(c.b) + \":\" + to_string(c.raw()) + \":\" + to_string(c.log)",
                "42:42:1:2", "the parent's USER accessor wins over the child's synthesized one");
        });

        test("accessor_proofs_on_off_identical", [this, check_proof_run, thrice_then]() {
            if (vm_reference_run()) { return; }   // asserts the optimized VM's proofs or elisions
            const std::string unit =
                "class Q { int b = 1; int plain = 2; int _get_b() { return b + 100; } void _set_b(int v) { b = v * 10; } int raw() { return b; } }\n"
                "class P { int b = 1; } class C : P { int _get_b() { return b + 100; } void _set_b(int v) { b = v * 10; } int raw() { return b; } }\n"
                "class PP { int log = 0; int _get_b() { return 42; } void _set_b(int v) { log = log + v; } } class CC : PP { int b = 1; }\n"
                "string probe(Q q, C c, CC cc) { q.b = 2; c.b = 3; cc.b = 4; q.plain = q.plain + 1; q.b += 1;\n"
                "  return to_string(q.b) + \":\" + to_string(q.raw()) + \":\" + to_string(c.b) + \":\" + to_string(c.raw()) + \":\" + to_string(cc.b) + \":\" + to_string(cc.log) + \":\" + to_string(q.plain); }\n"
                "Q q = Q(); C c = C(); CC cc = CC();\n"
                "probe(q, c, cc)";
            // by-value class parameters copy the instance, so every probe sees fresh receivers
            check_proof_run(thrice_then(unit, "probe(q, c, cc)"), "1310:1210:130:30:42:4:3", "typed receivers with user accessors: all four lanes agree");
        });

        test("accessor_hot_reload_adds_and_removes", [this]() {
            for (bool use_vm : {false, true}) {
                const std::string tag = use_vm ? "vm: " : "interp: ";
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute(
                    "class R { int f = 1; int log = 0; int raw() { return f; } }\n"
                    "var r = R();\n"
                    "int hot() { int s = 0; for (int i = 0; i < 5; ++i) { s += r.f; } return s; }\n"
                    "void hots(int v) { for (int i = 0; i < 5; ++i) { r.f = v; } }");
                check_eq((int64_t)5, e->execute("hot()").as_int(), tag + "plain field");
                e->execute("hots(2);");
                check_eq((int64_t)10, e->execute("hot()").as_int(), tag + "armed read site");
                e->execute("class R { int f = 1; int log = 0; int raw() { return f; } int _get_f() { return f * 10; } void _set_f(int v) { log = log + 1; } }");
                check_eq((int64_t)100, e->execute("hot()").as_int(), tag + "the reload's getter serves the hot read site");
                e->execute("hots(3);");
                check_eq((int64_t)5, e->execute("r.log").as_int(), tag + "the reload's setter fires at the hot store site");
                check_eq((int64_t)2, e->execute("r.raw()").as_int(), tag + "the setter only logged");
                check_eq((int64_t)20, e->execute("r.f").as_int(), tag + "plain member read through the getter");
                e->execute("class R { int f = 1; int log = 0; int raw() { return f; } }");
                check_eq((int64_t)10, e->execute("hot()").as_int(), tag + "accessor removed: the raw field again");
                e->execute("hots(4);");
                check_eq((int64_t)20, e->execute("hot()").as_int(), tag + "raw store again");
                check_eq((int64_t)5, e->execute("r.log").as_int(), tag + "log untouched by raw stores");
                // a parent gaining an accessor reaches existing child instances
                e->execute(
                    "class P { int b = 1; } class C : P { int raw() { return b; } } var c = C();\n"
                    "int hotc() { int s = 0; for (int i = 0; i < 5; ++i) { s += c.b; } return s; }");
                check_eq((int64_t)5, e->execute("hotc()").as_int(), tag + "child reads the parent field");
                check_eq((int64_t)5, e->execute("hotc()").as_int(), tag + "armed");
                e->execute("class P { int b = 1; int _get_b() { return b + 100; } }");
                check_eq((int64_t)505, e->execute("hotc()").as_int(), tag + "the parent's new getter serves the child's hot site");
                check_eq((int64_t)1, e->execute("c.raw()").as_int(), tag + "bare read in the child is the field");
            }
        });

        test("accessor_field_keeps_ic_off", [check_both]() {
            // a getter with a side effect: an armed member slot IC would sum the raw field (0)
            check_both(
                "class C { int b = 0; int n = 0; int _get_b() { n = n + 1; return n; } }\n"
                "var c = C();\n"
                "int sum() { int s = 0; for (int i = 0; i < 5; ++i) { s += c.b; } return s; }\n"
                "string r = to_string(sum()) + \":\" + to_string(sum());\n"
                "class D { int b = 0; int cnt = 0; void _set_b(int v) { cnt = cnt + 1; } int raw() { return b; } }\n"
                "var d = D();\n"
                "void fill() { for (int i = 0; i < 5; ++i) { d.b = i; } }\n"
                "fill(); fill();\n"
                "r + \":\" + to_string(d.cnt) + \":\" + to_string(d.b) + \":\" + to_string(d.raw())",
                "15:40:10:0:0", "every hot read/store dispatches to the accessor");
        });

        test("accessor_setter_error_text_parity", [this, both]() {
            auto [i_out, v_out] = both(
                "class C { int b = 1; void _set_b(int v) { if (v < 0) { throw \"negative\"; } b = v * 10; } int raw() { return b; } }\n"
                "var c = C();\n"
                "var m1 = \"\"; try { c.b = -1; m1 = \"stored\"; } catch (err1) { m1 = err1; }\n"
                "var m2 = \"\"; try { c.b = \"str\"; m2 = \"stored\"; } catch (err2) { m2 = err2; }\n"
                "var m3 = \"\"; try { c.b = 1.5; m3 = \"stored\"; } catch (err3) { m3 = err3; }\n"
                "m1 + \"|\" + m2 + \"|\" + m3 + \"|\" + to_string(c.raw()) + \"|\" + to_string(c.b)");
            check_eq(i_out, v_out, "setter error texts agree across backends");
            check_eq(std::string("negative|Cannot pass string to parameter 'v' of '_set_b' (declared int)|stored|10|10"), i_out, "script throw, parameter rejection, float->int conversion");
        });

        test("accessor_ref_binding_is_non_lvalue", [check_both]() {
            check_both(
                "class C { int b = 1; int plain = 2; int _get_b() { return b; } }\n"
                "var c = C();\n"
                "void f(int& r) { r = 9; }\n"
                "var m = \"\"; try { f(c.b); m = \"bound\"; } catch (err) { m = err; }\n"
                "f(c.plain);\n"
                "m + \":\" + to_string(c.plain) + \":\" + to_string(c.b)",
                "Cannot pass non-lvalue to reference parameter:9:1", "an accessor-bearing field is a computed property to a reference");
        });
        // ===== ONE CONVERSION TABLE FOR EVERY TYPED SLOT (detail::convert_for_typed_slot) =====
        // declaration/initializer, `=`, by-value parameter binding, typed return and field store
        // all run the assignment table (docs/site/guide/02-type-ladder.html "The one conversion table"); each site owns its
        // rejection text. Every row is byte-identical across backends.

        // A string slot takes text (a string, a utf8_string's bytes) or an object's own to_string(), as an int slot takes an object's
        // to_int(); cast<string> is the explicit stringify
        test("typed_slot_string_takes_text_everywhere", [this, check_both, both]() {
            check_both(R"JAI(
                class X { string to_string() { return "X!"; } }
                string f(string t) { return t; }
                string g() { return X(); }
                class H { string s = ""; }
                auto h = H(); h.s = X();
                string a = X(); string b; b = X();
                utf8_string u = "uu"; string c = u;
                string k = 'k'; utf8_string uk = 'k';
                f("p") + "|" + f(X()) + "|" + g() + "|" + h.s + "|" + a + "|" + b + "|" + c + "|" + k + uk
            )JAI", "p|X!|X!|X!|X!|X!|uu|kk", "text, a char and an object's to_string() reach a string slot on every path");
            const char* rejected[] = {
                "string s = 5;",
                "string s = \"a\"; s = 2.5;",
                "string f(string t) { return t; } f(true);",
                "string g() { return 5; }",
                "class H { string s = 3; }",
                "class H { string s = \"\"; } auto h = H(); h.s = false;",
                "string s = null;",
                "string s = [1];",
                "class D {} string s = D();",
                "utf8_string u = 7;",
            };
            for (const char* src : rejected) {
                auto [i_out, v_out] = both(src);
                check_eq(i_out, v_out, std::string("parity: ") + src);
                check(v_out.find("static check failed") != std::string::npos && v_out.find("stringify it with to_string(x) or `${x}`") != std::string::npos,
                      std::string("a load error with the hint: ") + src + " (got: " + v_out + ")");
            }
            check_both(R"JAI(
                class D {}
                var v = 5;
                var d = D();
                string m(var f) { try { f(); return "none"; } catch (e) { return e; } }
                string p(string t) { return t; }
                string r() { return v; }
                class H { string s = ""; }
                m([]() { string s = v; }) + "|" + m([]() { string s = ""; s = v; }) + "|" + m([]() { p(v); }) + "|" + m([]() { r(); }) + "|" +
                    m([]() { auto h = H(); h.s = v; }) + "|" + m([]() { string s = d; })
            )JAI", "Cannot initialize 's' (declared string) with int|Cannot assign int to 's' (declared string)|"
                   "Cannot pass int to parameter 't' of 'p' (declared string)|Cannot return int from 'r' (declared -> string)|"
                   "Cannot assign int to field 's' of H (declared string)|Cannot initialize 's' (declared string) with D",
                   "a var-sourced non-text value raises where it lands");
            check_both(R"JAI(
                class D {}
                cast<string>(5) + cast<string>(2.5) + cast<string>(true) + cast<string>('c') + cast<string>([1]) + cast<string>(D()) + to_string(7)
            )JAI", "52.5truec[1]D{}7", "cast<string> and to_string stringify anything");
        });

        test("typed_slot_bool_is_truthiness_everywhere", [check_both]() {
            check_both(R"JAI(
                class D {} class B { bool to_bool() { return false; } } class C { int a = 0; }
                bool b(bool x) { return x; }
                string j(bool v) { return to_string(v) + ","; }
                j(b(5)) + j(b(0)) + j(b(2.5)) + j(b("")) + j(b("a")) + j(b(null)) + j(b([])) + j(b([1])) + j(b(D())) + j(b(B())) + j(b(shared_ptr<C>()))
            )JAI", "true,false,true,false,true,false,false,true,true,false,true,", "bool parameter: is_truthy rows incl. to_bool()");
            check_both(R"JAI(
                class D {} class B { bool to_bool() { return false; } }
                bool r1() { return 5; } bool r2() { return ""; } bool r3() { return D(); } bool r4() { return B(); } bool r5() { return [1]; }
                to_string(r1()) + "," + to_string(r2()) + "," + to_string(r3()) + "," + to_string(r4()) + "," + to_string(r5())
            )JAI", "true,false,true,false,true", "bool return: is_truthy rows");
            check_both(R"JAI(
                class D {} class B { bool to_bool() { return false; } }
                class H { bool f = false; }
                auto h = H();
                h.f = [1]; string acc = to_string(h.f);
                h.f = D(); acc = acc + "," + to_string(h.f);
                h.f = B(); acc = acc + "," + to_string(h.f);
                h.f = {"k": 1}; acc = acc + "," + to_string(h.f);
                h.f = []; acc = acc + "," + to_string(h.f);
                acc
            )JAI", "true,true,false,true,false", "bool field: containers and objects convert like a bool local");
            check_both(R"JAI(
                class B { bool to_bool() { return false; } }
                bool p(bool x) { return x; }
                var v = B();
                string r = "";
                if (v) { r = "T"; } else { r = "F"; }
                bool d = v;
                r + to_string(d) + to_string(p(v))
            )JAI", "Ffalsefalse", "bool slots agree with if (x)");
        });

        test("typed_slot_numeric_from_bool_everywhere", [check_both]() {
            check_both(R"JAI(
                int i(int x) { return x; } float fl(float x) { return x; } int r() { return true; } float rf() { return false; }
                to_string(i(true)) + "," + to_string(fl(true)) + "," + to_string(r()) + "," + to_string(rf()) + "," + to_string(i(false))
            )JAI", "1,1.0,1,0.0,0", "bool into int/float parameters and returns");
        });

        test("typed_slot_object_to_x_methods_everywhere", [check_both, check_quad]() {
            const char* var_sourced = R"JAI(
                class X { int to_int() { return 7; } float to_float() { return 1.5; } char to_char() { return 'x'; } }
                class H { int i = 0; float f = 0.0; char c = 'a'; }
                var xs = X();
                int a = xs; float b = xs; char c = xs; int d; d = xs;
                auto h = H(); h.i = xs; h.f = xs; h.c = xs;
                int r() { return xs; } int p(int v) { return v; }
                to_string(a) + "," + to_string(b) + "," + to_string(c) + "," + to_string(d) + "," + to_string(h.i) + "," + to_string(h.f) + "," + to_string(h.c) + "," + to_string(r()) + "," + to_string(p(xs))
            )JAI";
            check_quad(var_sourced, "7,1.5,x,7,7,1.5,x,7,7", "to_int/to_float/to_char on decl, assign, field, return, param");
            check_both(R"JAI(
                class X { int to_int() { return 7; } float to_float() { return 1.5; } char to_char() { return 'x'; } }
                int a = X(); float b = X(); char c = X();
                to_string(a) + "," + to_string(b) + "," + to_string(c)
            )JAI", "7,1.5,x", "the checker admits to_X() sources under strict");
            check_both(R"JAI(
                class X { int to_int() { return 7; } }
                class Y { auto to_int() { return X(); } }
                var y = Y();
                string r = ""; try { int v = y; r = "ok"; } catch (e) { r = e; }
                int p(int v) { return v; }
                string r2 = ""; try { p(y); r2 = "ok"; } catch (e) { r2 = e; }
                r + "|" + r2
            )JAI", "Cannot initialize 'v' (declared int) with Y|Cannot pass Y to parameter 'v' of 'p' (declared int)", "an object result of to_X() never converts again");
            check_both(R"JAI(
                class Q { int to_int() { return 1; } }
                var q = Q();
                string r = ""; try { char c = q; r = "ok"; } catch (e) { r = e; }
                r
            )JAI", "Cannot initialize 'c' (declared char) with Q", "no to_char(): char slot rejects");
        });

        test("typed_slot_handle_into_value_slot", [this, check_both, both]() {
            check_both(R"JAI(
                class C { int a = 0; C() {} C(int v) { a = v; } }
                C v = new C(2); C w; w = new C(3); C e = shared_ptr<C>();
                var h = new C(4); C shared = h; shared.a = 9;
                to_string(v.a) + "," + to_string(w.a) + "," + to_string(e.a) + "," + to_string(h.a)
            )JAI", "2,3,0,4", "a C declaration from a shared_ptr<C> handle copies the pointee, as C x = *h does");
            auto [i_out, v_out] = both(R"JAI(
                class C { int a = 0; C() {} C(int v) { a = v; } }
                string pt(C c) { return type_of(c) + ":" + to_string(c == null); }
                C d1 = new C(2); C d2 = shared_ptr<C>();
                string decl = type_of(d1) + ":" + to_string(d1 == null) + "|" + type_of(d2) + ":" + to_string(d2 == null);
                string param = pt(new C(2)) + "|" + pt(shared_ptr<C>());
                to_string(decl == param) + " " + decl
            )JAI");
            check_eq(i_out, v_out, "handle-into-value-slot shape parity");
            check_true(i_out.rfind("true ", 0) == 0, "decl and param agree on the bound shape (got: " + i_out + ")");
        });

        test("typed_slot_null_into_handle_params_and_returns", [check_both]() {
            check_both(R"JAI(
                class C { int a = 0; }
                string p(shared_ptr<C> x) { return to_string(x == null) + ":" + type_of(x); }
                shared_ptr<C> g() { return null; }
                string pw(weak_ptr<C> x) { return to_string(x == null) + ":" + type_of(x); }
                weak_ptr<C> gw() { return null; }
                shared_ptr<C> q = null; weak_ptr<C> qw; qw = null;
                p(null) + "|" + to_string(g() == null) + ":" + type_of(g()) + "|" + pw(null) + "|" + to_string(gw() == null) + ":" + type_of(gw()) + "|" + type_of(q) + "|" + type_of(qw)
            )JAI", "true:object|true:object|true:weak_ptr|true:weak_ptr|object|weak_ptr", "null binds to shared_ptr/weak_ptr parameters and returns as the typed null `=` stores");
        });

        test("typed_slot_no_chained_conversion_anywhere", [check_both]() {
            check_both(R"JAI(
                class TA { int v = 1; } class TB { int v = 0; TB(TA a) { v = a.v * 2; } } class TC { int v = 0; TC(TB b) { v = b.v * 3; } }
                class H { TC t = null; } class X { int to_int() { return 7; } } class C { int a = 0; C() {} C(int v) { a = v; } }
                var ta = TA(); var xs = X();
                string r1 = ""; try { TC c = ta; r1 = "ok"; } catch (e) { r1 = e; }
                string r2 = ""; try { TC c2 = null; c2 = ta; r2 = "ok"; } catch (e) { r2 = e; }
                string r3 = ""; try { auto h = H(); h.t = ta; r3 = "ok"; } catch (e) { r3 = e; }
                int pc(TC c) { return c.v; }
                string r4 = ""; try { pc(ta); r4 = "ok"; } catch (e) { r4 = e; }
                TC rc() { return ta; }
                string r5 = ""; try { rc(); r5 = "ok"; } catch (e) { r5 = e; }
                string r6 = ""; try { C c = xs; r6 = "ok"; } catch (e) { r6 = e; }
                r1 + "|" + r2 + "|" + r3 + "|" + r4 + "|" + r5 + "|" + r6
            )JAI", "Cannot initialize 'c' (declared TC) with TA|Cannot assign TA to 'c2' (declared TC)|Cannot assign TA to field 't' of H (declared TC)|Cannot convert TA to TC (no suitable single-argument constructor)|Cannot convert TA to TC (no suitable single-argument constructor)|Cannot initialize 'c' (declared C) with X",
                "TA->TB->TC and X->to_int->C are never implicit");
            check_both(R"JAI(
                class TA { int v = 1; } class TB { int v = 0; TB(TA a) { v = a.v * 2; } }
                class HB { TB t = null; }
                var ta = TA();
                TB b = ta; TB b2 = null; b2 = ta; auto hb = HB(); hb.t = ta;
                int pb(TB b) { return b.v; } TB rb() { return ta; }
                to_string(b.v) + to_string(b2.v) + to_string(hb.t.v) + to_string(pb(ta)) + to_string(rb().v)
            )JAI", "22222", "exactly one user conversion works on every path");
        });

        test("typed_slot_field_ctor_conversion", [this, check_both]() {
            check_both(R"JAI(
                class C { int a = 0; C() {} C(int v) { a = v; } } class D {} class H { C c; } class G { D d; }
                auto h = H(); h.c = 5; string r = to_string(h.c.a); h.c = 2.5; r = r + "," + to_string(h.c.a);
                var t = true;
                try { h.c = t; r = r + ",ok"; } catch (e) { r = r + "," + e; }
                auto g = G(); var c1 = C(1); try { g.d = c1; r = r + ",ok"; } catch (e) { r = r + "," + e; }
                r
            )JAI", "5,2,Cannot assign bool to field 'c' of H (declared C),Cannot assign C to field 'd' of G (declared D)", "field stores construct through a true 1-param ctor; bool needs int<->float or same base");
            // host set_field runs the table without its script rows; hot reload likewise falls back
            // to the initializer default when the live value converts only through script
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->execute("class C { int a = 0; C() {} C(int v) { a = v; } } class H { C c; string s = \"\"; } auto h = H();");
                auto instance = e->execute("h").as<jai::strong_ptr<jai::class_instance>>();
                check_not_null(instance.get());
                bool threw = false;
                try { instance->set_field(detail::engine_access::symbolize(*e, "c"), jai::script_value((int64_t)5, e.get())); }
                catch (const std::exception& ex) { threw = std::string(ex.what()).find("Cannot assign int to field 'c' of H (declared C)") != std::string::npos; }
                check(threw, "set_field: ctor conversion is a script row, refused from C++");
                threw = false;
                try { instance->set_field(detail::engine_access::symbolize(*e, "s"), jai::script_value((int64_t)5, e.get())); }
                catch (const std::exception& ex) { threw = std::string(ex.what()).find("Cannot assign int to field 's' of H (declared string)") != std::string::npos; }
                check(threw, "set_field: a string field takes text only");
                e->execute("class R { int f = 0; } R r = R(); r.f = 5; class X { int to_int() { return 7; } } class R2 { X f = X(); } R2 r2 = R2();");
                e->execute("class R { C f; }");
                check_eq(std::string("0"), e->execute("r.f.a").to_string(), "reload int->C: ctor-only conversion adopts the default (a default-constructed C)");
                e->execute("class R2 { int f = 3; }");
                check_eq(std::string("3"), e->execute("r2.f").to_string(), "reload X->int: to_int-only conversion adopts the default");
            }
        });

        test("typed_slot_container_boundary_on_params_and_returns", [this, check_both, both]() {
			check_both(R"JAI(
				[[warn_mode]] // retain runtime boundary-text coverage
				void pa(array<int> a) {}
				string r1 = ""; try { pa(["x"]); r1 = "ok"; } catch (e) { r1 = e; }
				array<int> ga() { return ["x"]; }
				string r2 = ""; try { ga(); r2 = "ok"; } catch (e) { r2 = e; }
				string pb(array<int> a) { return type_of(a[1]) + to_string(a[1]); }
				string r3 = pb([1, 2.5]);
				array<int> g = [1, 2];
				string pt(array<int> a) { return to_string(a.size()) + type_of(a[0]); }
				r1 + "|" + r2 + "|" + r3 + "|" + pt(g)
			)JAI", "Cannot store string in array<int>|Cannot store string in array<int>|int2|2int", "array<int> parameters and returns run the element boundary");
			auto [i_out, v_out] = both(R"JAI(
				[[warn_mode]] // retain runtime boundary-text coverage
				void pc(array<int> a) { a.push("zz"); }
				string r4 = ""; try { pc([1]); r4 = "ok"; } catch (e) { r4 = e; }
				string r5 = ""; try { array<int> x = [1]; x.push("zz"); r5 = "ok"; } catch (e) { r5 = e; }
				void pm(map<string,int> m) {}
				string r6 = ""; try { pm({"k": "v"}); r6 = "ok"; } catch (e) { r6 = e; }
				string r7 = ""; try { map<string,int> m = {"k": "v"}; r7 = "ok"; } catch (e) { r7 = e; }
				to_string(r4 == r5) + ":" + r4 + "|" + to_string(r6 == r7) + ":" + r6
			)JAI");
			check_eq(i_out, v_out, "container parameter texts agree across backends");
			check_true(i_out.rfind("true:Cannot push", 0) == 0 && i_out.find("|true:Cannot store") != std::string::npos,
				"a bound array<int>/map<string,int> parameter is the declared representation (got: " + i_out + ")");
		});

        test("typed_slot_local_container_assign_checks_elements", [check_both]() {
            check_both(R"JAI(
                string f() { array<int> v = [1]; var s = ["x"]; try { v = s; } catch (e) { return e; } return type_of(v[0]); }
                f()
            )JAI", "Cannot store string in array<int>", "a slot-local `=` into array<int> runs the boundary like the global path");
            check_both(R"JAI(
                string f() { map<string,int> m = {"a": 1}; var s = {"k": "v"}; string r = ""; try { m = s; r = "ok"; } catch (e) { r = e; } return r + ":" + to_string(m.size()); }
                f()
            )JAI", "Cannot store string in map<string,int>:1", "a slot-local `=` into map<string,int> runs the boundary");
        });

        test("typed_slot_rejection_texts", [check_both]() {
            check_both(R"JAI(
                void i(int x) {} var n = null; string r1 = ""; try { i(n); } catch (e) { r1 = e; }
                class A {} class B {} void fa(A a) {} var b = B(); string r2 = ""; try { fa(b); } catch (e) { r2 = e; }
                class C { int a = 0; } void sp(shared_ptr<C> p) {} var five = 5; string r3 = ""; try { sp(five); } catch (e) { r3 = e; }
                var s = "s"; int r() { return s; } string r4 = ""; try { r(); } catch (e) { r4 = e; }
                void wp(weak_ptr<C> p) {} string r5 = ""; try { wp(five); } catch (e) { r5 = e; }
                void pa(array<int> a) {} string r6 = ""; try { pa(five); } catch (e) { r6 = e; }
                var arr = [1]; string r7 = ""; try { i(arr); } catch (e) { r7 = e; }
                r1 + "|" + r2 + "|" + r3 + "|" + r4 + "|" + r5 + "|" + r6 + "|" + r7
            )JAI", "Cannot pass null to parameter 'x' of 'i' (declared int)|Cannot convert B to A (no suitable single-argument constructor)|Cannot pass int to parameter 'p' of 'sp' (declared shared_ptr<C>)|Cannot return string from 'r' (declared -> int)|Cannot pass int to parameter 'p' of 'wp' (declared weak_ptr<C>)|Cannot pass int to parameter 'a' of 'pa' (declared array<int>)|Cannot pass array to parameter 'x' of 'i' (declared int)",
                "source spellings are the payload's type, targets their canonical name");
        });

        test("typed_slot_handle_variable_texts", [check_both]() {
            // weak_ptr<T> / shared_ptr<T> VARIABLES reject through family D's declaration and
            // assignment shapes like every other slot: the payload's type,
            // the slot, the declared handle type - never 'any', never a bare placeholder
            check_both(R"JAI(
                class Foo { int a = 1; } class Bar { int b = 2; }
                var obj = Foo(); var five = 5; var sb = shared_ptr<Bar>(); var str = "s"; var arr = [1]; var bar = Bar();
                string r1 = ""; try { weak_ptr<Foo> w = obj; } catch (e) { r1 = e; }
                string r2 = ""; try { weak_ptr<Foo> w = five; } catch (e) { r2 = e; }
                string r3 = ""; try { weak_ptr<Foo> w = sb; } catch (e) { r3 = e; }
                string r4 = ""; try { weak_ptr<Foo> w; w = obj; } catch (e) { r4 = e; }
                string r5 = ""; try { weak_ptr<Foo> w; w = five; } catch (e) { r5 = e; }
                string r6 = ""; try { weak_ptr<Foo> w; w = sb; } catch (e) { r6 = e; }
                string r7 = ""; try { auto p = shared_ptr<Foo>(); p = str; } catch (e) { r7 = e; }
                string r8 = ""; try { auto p = shared_ptr<Foo>(); p = arr; } catch (e) { r8 = e; }
                string r9 = ""; try { shared_ptr<Foo> p = shared_ptr<Foo>(); p = bar; } catch (e) { r9 = e; }
                string local1() { string r = ""; try { weak_ptr<Foo> w; var o = Foo(); w = o; } catch (e) { r = e; } return r; }
                string local2() { string r = ""; try { var o = Foo(); weak_ptr<Foo> w = o; } catch (e) { r = e; } return r; }
                r1 + "|" + r2 + "|" + r3 + "|" + r4 + "|" + r5 + "|" + r6 + "|" + r7 + "|" + r8 + "|" + r9 + "|" + local1() + "|" + local2()
            )JAI", "Cannot initialize 'w' (declared weak_ptr<Foo>) with Foo|Cannot initialize 'w' (declared weak_ptr<Foo>) with int|Cannot initialize 'w' (declared weak_ptr<Foo>) with Bar|Cannot assign Foo to 'w' (declared weak_ptr<Foo>)|Cannot assign int to 'w' (declared weak_ptr<Foo>)|Cannot assign Bar to 'w' (declared weak_ptr<Foo>)|Cannot assign string to 'p' (declared shared_ptr<Foo>)|Cannot assign array to 'p' (declared shared_ptr<Foo>)|Cannot assign Bar to 'p' (declared shared_ptr<Foo>)|Cannot assign Foo to 'w' (declared weak_ptr<Foo>)|Cannot initialize 'w' (declared weak_ptr<Foo>) with Foo",
                "handle variables name the slot and spell the payload's type (globals and slotted locals alike)");
            check_both(R"JAI(
                class Foo { int a = 1; }
                class Foo2 : Foo { }
                var keep = shared_ptr<Foo>(); var sub = shared_ptr<Foo2>(); var nothing = null; var five = 5;
                string r1 = "ok"; try { weak_ptr<Foo> w = keep; weak_ptr<Foo> w2 = sub; weak_ptr<Foo> w3 = nothing; w = sub; w2 = nothing; w3 = keep; } catch (e) { r1 = e; }
                string r2 = "ok"; try { shared_ptr<Foo> p = keep; p = five; } catch (e) { r2 = e; }
                weak_ptr<Foo> gw = sub; gw = keep; gw = sub; gw = keep;
                string r3 = "ok"; try { gw = five; } catch (e) { r3 = e; }
                string local() { weak_ptr<Foo> lw = sub; lw = keep; lw = sub; string r = "ok"; try { lw = keep; lw = five; } catch (e) { r = e; } return r; }
                string r4 = ""; try { shared_ptr<Foo> gp = keep; var vgw = gw; gp = vgw; } catch (e) { r4 = e; }
                r1 + "|" + r2 + "|" + to_string(keep.a) + "|" + r3 + "|" + local() + "|" + r4
            )JAI", "ok|Cannot assign int to 'p' (declared shared_ptr<Foo>)|1|Cannot assign int to 'gw' (declared weak_ptr<Foo>)|"
                   "Cannot assign int to 'lw' (declared weak_ptr<Foo>)|Cannot assign weak_ptr to 'gp' (declared shared_ptr<Foo>)",
                "handles and subclass handles still bind; a rejected handle store leaves the pointee untouched; a handle variable keeps its "
                "declared type through subclass stores, a global as a local");
        });

        test("auto_container_homogeneity_index", [check_both]() {
            // the offending index rides the pre-interned count table (it rendered as a symbol
            // NAME before); every 'auto' homogeneity text, both containers, nested too
            check_both(R"JAI(
                class A { int a = 0; } class B { int b = 0; }
                var one = 1; var s = "s";
                string r1 = ""; try { auto x = [1, 2, s]; } catch (e) { r1 = e; }
                string r2 = ""; try { auto x = ["a", "b", "c", "d", one]; } catch (e) { r2 = e; }
                string r3 = ""; try { array<auto> x = [1, 2.5]; } catch (e) { r3 = e; }
                string r4 = ""; try { auto x = [A(), B()]; } catch (e) { r4 = e; }
                string r5 = ""; try { auto m = {"a": 1, "b": s}; } catch (e) { r5 = e; }
                string r6 = ""; try { auto m = {"a": A(), "b": B()}; } catch (e) { r6 = e; }
                string r7 = ""; try { auto x = [[1], [2, s]]; } catch (e) { r7 = e; }
                r1 + "|" + r2 + "|" + r3 + "|" + r4 + "|" + r5 + "|" + r6 + "|" + r7
            )JAI", "Array 'auto' requires homogeneous elements - type mismatch at index 2|Array 'auto' requires homogeneous elements - type mismatch at index 4|Array 'auto' requires homogeneous elements - type mismatch at index 1|Array 'auto' requires homogeneous class types - mismatch at index 1|Map 'auto' requires homogeneous values - type mismatch at index 1|Map 'auto' requires homogeneous class types - mismatch at index 1|Array 'auto' requires homogeneous elements - type mismatch at index 1",
                "homogeneity texts render the index as a number");
        });

        test("typed_slot_throwing_conversion_propagates", [check_both]() {
            check_both(R"JAI(
                class T { int v = 0; T(int x) { throw "boom"; } }
                var five = 5;
                string r1 = ""; try { T t = five; } catch (e) { r1 = e; }
                string r2 = ""; try { T t2 = null; t2 = five; } catch (e) { r2 = e; }
                class H { T t = null; } string r3 = ""; try { auto h = H(); h.t = five; } catch (e) { r3 = e; }
                void p(T t) {} string r4 = ""; try { p(five); } catch (e) { r4 = e; }
                T g() { return five; } string r5 = ""; try { g(); } catch (e) { r5 = e; }
                r1 + "|" + r2 + "|" + r3 + "|" + r4 + "|" + r5
            )JAI", "boom|boom|boom|boom|boom", "a converting constructor's own error surfaces on every path");
            check_both(R"JAI(
                class X { int to_int() { throw "bang"; } }
                var xs = X();
                string r1 = ""; try { int a = xs; } catch (e) { r1 = e; }
                int p(int v) { return v; } string r2 = ""; try { p(xs); } catch (e) { r2 = e; }
                r1 + "|" + r2
            )JAI", "bang|bang", "a throwing to_X() surfaces on every path");
        });

        // Typed by-value returns land on the copy (the conversion runs on the return's own
        // value, never on the callee's storage), identically under every proof lane.
        test("typed_return_lands_on_copy", [check_both, check_proof_run]() {
            check_both(R"JAI(
                array<var> src = [1, 2];
                array<int> f() { return src; }
                var r = f(); r.push(3);
                class Base { int b = 1; }
                class Derived : Base { int d = 2; }
                Derived dg = Derived();
                Base g() { return dg; }
                var x = g(); x.b = 9;
                class H { array<int> items = [1]; array<int> get() { return items; } }
                H hd = H();
                var hi = hd.get(); hi.push(2);
                to_string(src.size()) + ":" + type_of(r) + ":" + to_string(r.size()) + "|" + to_string(dg.b) + ":" + to_string(x.b) + "|" + to_string(hd.items.size()) + ":" + to_string(hi.size())
            )JAI", "2:array:3|1:9|1:2", "typed returns of a global, a derived global and a field are copies");
            check_proof_run({"array<int> src = [1, 2]; array<int> f() { return src; } var r = f(); r.push(3); to_string(src.size()) + \":\" + to_string(r.size())"},
                            "2:3", "same-type proven return of a global still copies under every lane");
            check_proof_run({"class T { int v = 1; } T obj = T(); T f() { return obj; } T mk() { T t = T(); t.v = 5; return t; } var a = f(); a.v = 9; var b = mk(); to_string(obj.v) + \":\" + to_string(a.v) + \":\" + to_string(b.v)"},
                            "1:9:5", "proven object return copies the global and moves the local under every lane");
        });

        test("typed_slot_proof_lanes", [check_proof_run]() {
            check_proof_run({"string f(string t) { return t + \"!\"; } utf8_string u = \"5\"; string r = f(u); r = r + f(u); r"}, "5!5!", "string parameter bound from a utf8_string under every lane");
            check_proof_run({"int sum(array<int> a) { int s = 0; for (auto x : a) { s += x; } return s; } to_string(sum([1, 2.5, 3]))"}, "6", "array<int> parameter bound from a hetero literal under every lane");
            check_proof_run({"bool b(bool x) { return x; } to_string(b(5)) + to_string(b(0))"}, "truefalse", "bool parameter bound from an int under every lane");
        });

        // ===== STRINGS ARE VALUES (copy-on-write at the in-place write gate) =====
        // A by-value string copy (declaration, =, parameter, capture, element, map value,
        // field, return) is observationally a deep copy: "strings never alias at a distance
        // ... the mutation is strictly local to the variable you called the method on"
        // (guide ch.9 copy-first idiom). Storage stays one shared node per
        // value; the copy materializes at the first in-place write - the 17 mutators, +=,
        // a host std::string& (value.hpp unchecked_as_string_ref).

        test("string_value_semantics_battery", [check_quad]() {
            check_quad("string a = \"hello\"; string b = a; b.to_upper(); a + \"|\" + b", "hello|HELLO", "typed decl copy");
            check_quad("var a = \"hello\"; var b = a; b.to_upper(); a + \"|\" + b", "hello|HELLO", "var decl copy");
            check_quad("auto a = \"hello\"; auto b = a; b.to_upper(); a + \"|\" + b", "hello|HELLO", "auto decl copy (the documented copy-first idiom)");
            check_quad("string a = \"hello\"; string b = \"\"; b = a; b.to_upper(); a + \"|\" + b", "hello|HELLO", "assignment copy");
            check_quad("function f(string s) { s.to_upper(); return s; } string a = \"hello\"; f(a) + a", "HELLOhello", "by-value string parameter");
            check_quad("function f(var s) { s.to_upper(); return s; } string a = \"hello\"; f(a) + a", "HELLOhello", "by-value var parameter");
            check_quad("function f(string& s) { s.to_upper(); } string a = \"hello\"; string b = a; f(b); a + \"|\" + b", "hello|HELLO", "string& parameter mutates its argument alone");
            check_quad("function run() { string a = \"c\"; var lam = [=]() { a.to_upper(); return a; }; return lam() + a; } run()", "Cc", "[=] capture of a local is a copy");
            check_quad("string a = \"c\"; var lam = [&]() { a.to_upper(); return a; }; lam() + a", "CC", "[&] capture aliases");
            check_quad("string a = \"e\"; var arr = [a]; arr[0].to_upper(); arr.push(a); arr[1].to_lower(); a + to_json(arr)", "e[\"E\",\"e\"]", "array literal element and push are copies; the elements mutate");
            check_quad("string a = \"e\"; array<string> arr = [a]; arr[0].to_upper(); a + to_json(arr)", "e[\"E\"]", "array<string> element is a copy");
            check_quad("string a = \"e\"; var m = {\"k\": a}; m[\"k\"].to_upper(); a + to_json(m)", "e{\"k\":\"E\"}", "map value is a copy");
            check_quad("class D { string s = \"x\"; void m() { s += \"!\"; s.to_upper(); } } string h = \"q\"; var d = D(); d.s = h; d.m(); h + d.s", "qQ!", "field store is a copy; bare-field += and mutator land in the field");
            check_quad("string a = \"x\"; string b = a; b += \"y\"; a + b", "xxy", "+= on a copy");
            check_quad("string a = \"x\"; string b = a; for (int i = 0; i < 3; ++i) { b += \"y\"; } a + b", "xxyyy", "loop += on a copy");
            check_quad("string a = \"z\"; var b = a; auto c = b; c.to_upper(); a + b + c", "zzZ", "chained copies mutate the last only");
            check_quad("class R { string n = \"n\"; string get() { return n; } } var r = R(); string g = r.get(); g.to_upper(); r.n + g", "nN", "returned field copy");
            check_quad("string a = \"hello\"; string b = a; to_string(b.length()) + b.substr(1) + to_string(b.find(\"l\")) + a", "5ello2hello", "observers on a copy");
            check_quad("string a = \"ab\"; string b = a; b.repeat(2); a + b", "ababab", "repeat on a copy");
            check_quad("string s = \"x\"; s.insert(0, s); s.replace(\"x\", s); s", "xxxx", "an argument aliasing the receiver reads the pre-write content");
        });

        test("string_mutator_receiver_lvalue_battery", [check_quad]() {
            // C++ `string& to_upper()`: the receiver binds like a reference parameter -
            // every lvalue spelling mutates its target, sources stay intact
            check_quad("var arr = [\"ab\", \"cd\"]; arr[1].to_upper(); to_json(arr)", "[\"ab\",\"CD\"]", "array<var> element receiver");
            check_quad("array<string> ts = [\"ab\", \"cd\"]; ts[0].to_upper(); to_json(ts)", "[\"AB\",\"cd\"]", "array<string> element receiver (scratch commit)");
            check_quad("array<string> ts = [\"ab\", \"cd\"]; for (auto& s : ts) { s.to_upper(); } to_json(ts)", "[\"AB\",\"CD\"]", "array<string> range-for by reference");
            check_quad("var strs = [\"ab\", \"cd\"]; for (auto& s : strs) { s.to_upper(); } strs[0] + strs[1]", "ABCD", "array<var> range-for by reference");
            check_quad("array<string> ts = [\"ab\"]; auto& r = ts[0]; r.to_upper(); to_json(ts)", "[\"AB\"]", "ref alias of a typed element");
            check_quad("var arr = [\"ab\"]; auto& r = arr[0]; r.to_upper(); to_json(arr)", "[\"AB\"]", "ref alias of an element");
            check_quad("var i = 0; var arr = [\"ab\", \"cd\"]; arr[i + 1].to_upper(); to_json(arr)", "[\"ab\",\"CD\"]", "computed-index element receiver");
            check_quad("var m = {\"k\": \"v\"}; m[\"k\"].to_upper(); string k = \"k\"; m[k].pad_left(3, '-'); to_json(m)", "{\"k\":\"--V\"}", "map entry receiver, literal and variable key");
            check_quad("class D { string s = \"x\"; string t = \"y\"; void m() { s.to_upper(); this.t.to_upper(); } } var d = D(); d.m(); d.s.pad_left(3, '-'); d.s + d.t", "--XY", "bare field, this.field and obj.field receivers");
            check_quad("class D { string s = \"x\"; } class O { D d = D(); } var o = O(); o.d.s.to_upper(); o.d.s", "X", "nested field chain receiver");
            check_quad("class D { string s = \"x\"; } var ds = [D()]; ds[0].s.to_upper(); ds[0].s", "X", "field of an element receiver");
            check_quad("class P { string _n = \"a\"; string _get_n() { return _n; } } var p = P(); p.n.to_upper(); p.n", "a", "a computed property is a temporary");
            check_quad("string s = \"abc\"; s.substr(1).to_upper(); s", "abc", "a call result is a temporary");
            check_quad("function mk() { return \"t\"; } mk().to_upper(); \"dropped\"", "dropped", "a temporary takes the write and is dropped");
            check_quad("string q = \"hello\"; var f = []() { q.to_upper(); }; f(); q", "HELLO", "a string method taken as a value is a lambda over the receiver");
            check_quad("string a = \"hello\"; string b = a; b.to_lower(); b.trim(); b.trim_left(); b.trim_right(); b.pad_left(6, ' '); b.pad_right(7, ' '); b.pad_center(9, '*'); b.replace_first(\"h\", \"j\"); b.replace_last(\"o\", \"y\"); b.replace(\"l\", \"L\"); b.insert(0, \">\"); b.erase(0, 1); b.remove_prefix(1); b.remove_suffix(1); b.reverse(); b.repeat(1); b.to_upper(); a + \"|\" + b",
                       "hello| YLLEJ ", "all seventeen mutators on a copy leave the source");
            check_quad("var arr = [\"hello\"]; string a = arr[0]; arr[0].to_lower(); arr[0].trim(); arr[0].trim_left(); arr[0].trim_right(); arr[0].pad_left(6, ' '); arr[0].pad_right(7, ' '); arr[0].pad_center(9, '*'); arr[0].replace_first(\"h\", \"j\"); arr[0].replace_last(\"o\", \"y\"); arr[0].replace(\"l\", \"L\"); arr[0].insert(0, \">\"); arr[0].erase(0, 1); arr[0].remove_prefix(1); arr[0].remove_suffix(1); arr[0].reverse(); arr[0].repeat(1); arr[0].to_upper(); a + \"|\" + arr[0]",
                       "hello| YLLEJ ", "all seventeen mutators through an element receiver land in the element");
        });

        test("string_mutator_chain_battery", [check_quad, check_quad_error]() {
            // A mutator returns nothing (C++ `void to_upper()`), so a chain uses its result: the inner call raises before it writes
            // (guide ch.9 "In place or a copy")
            check_quad("var t = \"  hello world  \"; t.trim(); t.to_upper(); t.replace(\"WORLD\", \"JAI\"); t", "HELLO JAI", "guide ch.9 mutators as statements");
            check_quad("var t = \" ab \"; string r = \"\"; try { t.trim().to_upper(); } catch (e) { r = e.message; } r + \"|\" + t",
                       "trim changes t in place and returns nothing; string::trim(t) returns a trimmed copy| ab ", "a chain on a variable raises before the first write");
            check_quad("var arr = [\" ab \"]; string r = \"\"; try { arr[0].trim().to_upper(); } catch (e) { r = e.message; } r + \"|\" + arr[0]",
                       "trim changes obj in place and returns nothing; string::trim(obj) returns a trimmed copy| ab ", "a chain rooted at an element raises before the first write");
            check_quad_error("string t = \"ab\"; t.to_upper().to_lower(); t", "a chain on a typed variable is a load error");
            check_quad("auto name = \"jai\"; auto upper = name; upper.to_upper(); name + \"|\" + upper", "jai|JAI", "guide ch.9: copy first keeps the original");
            check_quad_error("string s = \"abc\"; s.substr(1).to_upper().reverse(); s", "a chain rooted at a temporary is a load error too");
        });

    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::strong_types_tests)
