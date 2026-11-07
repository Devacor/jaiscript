#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/detail/type_checker.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

// A class value keys a map by its class: through < or <=>, else hash() with ==, else by identity with a warning
namespace class_map_key_hosts {
	struct cell {
		int64_t x = 0;
		cell() = default;
		explicit cell(int64_t a_x) : x(a_x) {}
	};
}

namespace jai::foundry::tests {

class class_map_key_tests : public suite {
public:
	class_map_key_tests() : suite("Class Map Keys") {}

	static constexpr const char* classes = R"(
		class P { int v = 0; P(int a) { v = a; } int operator<=>(P o) { return v - o.v; } }
		class H { int v = 0; H(int a) { v = a; } int hash() { return v % 3; } bool operator==(H o) { return v == o.v; } }
		class I { int v = 0; I(int a) { v = a; } }
	)";

	void forge_tests() override {
		test("an_ordered_class_keys_by_its_order", [this]() {
			auto e = make_engine();
			stdlib::register_all(e);
			e->execute(classes);
			e->execute(R"(
				var m = {};
				m[P(3)] = "c"; m[P(1)] = "a"; m[P(2)] = "b"; m[P(1)] = "A";
				string walk = "";
				for (auto& [k, v] : m) { walk += to_string(k.v) + v; }
			)");
			check_eq((int64_t)3, e->execute("m.size()").as_int());
			check_eq(std::string("A"), e->execute("m[P(1)]").as<std::string>());
			check_eq(true, e->execute("m.contains(P(2)) && !m.contains(P(4))").as_bool());
			check_eq(std::string("1A2b3c"), e->execute("walk").as<std::string>());
			check_eq(true, e->execute("m.remove(P(2)) && m.size() == 2").as_bool());
			check_eq(true, e->execute("var x = {}; x[P(1)] = 1; var y = {}; y[P(1)] = 1; var z = {}; z[P(2)] = 1; x == y && x != z").as_bool());
			check_eq(true, e->execute("var c = m; c[P(9)] = \"z\"; c.size() == 3 && m.size() == 2 && c[P(1)] == \"A\"").as_bool());
		});

		test("a_reload_that_changes_a_key_class_operators_rekeys_live_maps", [this]() {
			// a reload re-derives each map's run of the class's keys: re-sorted, re-hashed or back to identity; keys the new order
			// calls one key become the first of them in that order, as an insert finds it
			auto e = make_engine();
			stdlib::register_all(e);
			e->execute("class K { int v = 0; K(int a) { v = a; } bool operator<(K o) { return v < o.v; } }\n"
			           "var m = {}; for (int i = 0; i < 6; ++i) { m[K(i)] = i; }\n"
			           "string walk() { string s = \"\"; for (auto& [k, v] : m) { s += to_string(k.v) + \":\" + to_string(v) + \" \"; } return s; }");
			e->execute("class K { int v = 0; K(int a) { v = a; } bool operator<(K o) { return v > o.v; } }");
			check_eq(std::string("true true 6"), e->execute("to_string(m.has(K(3))) + \" \" + to_string(m.has(K(0))) + \" \" + to_string(m.size())").to_string());
			e->execute("m[K(1)] = 100;");
			check_eq(std::string("5:5 4:4 3:3 2:2 1:100 0:0 "), e->execute("walk()").to_string(), "the reversed order, one key per value");
			e->execute("class K { int v = 0; K(int a) { v = a; } bool operator<(K o) { return v / 2 < o.v / 2; } }");
			check_eq(std::string("1:100 3:3 5:5 "), e->execute("walk()").to_string(), "keys the new order merges keep the first of them");
			check_eq(true, e->execute("m.has(K(0)) && m[K(2)] == 3 && m.size() == 3").as_bool());
			e->execute("class K { int v = 0; K(int a) { v = a; } int hash() { return v % 2; } bool operator==(K o) { return v % 4 == o.v % 4; } }");
			check_eq(std::string("1:100 3:3 "), e->execute("walk()").to_string(), "a class that hashes now: 5 and 1 are one key");
			check_eq(true, e->execute("m.has(K(7)) && !m.has(K(2)) && m.remove(K(3)) && m.size() == 1").as_bool());
			e->execute("class K { int v = 0; K(int a) { v = a; } }");
			check_eq(true, e->execute("!m.has(K(1)) && m.size() == 1").as_bool(), "a class without key operators keys by identity again");
		});

		test("a_hashed_class_keys_by_hash_and_equality", [this]() {
			auto e = make_engine();
			stdlib::register_all(e);
			e->execute(classes);
			e->execute(R"(
				var m = {};
				m[H(1)] = "one"; m[H(4)] = "four"; m[H(2)] = "two"; m[H(1)] = "uno";
				string walk = "";
				for (auto& [k, v] : m) { walk += to_string(k.v); }
			)");
			check_eq((int64_t)3, e->execute("m.size()").as_int());
			check_eq(std::string("uno"), e->execute("m[H(1)]").as<std::string>());
			check_eq(std::string("four"), e->execute("m[H(4)]").as<std::string>());
			check_eq(true, e->execute("walk == \"142\" || walk == \"412\"").as_bool());
		});

		test("a_bound_class_with_less_keys_by_it", [this]() {
			using class_map_key_hosts::cell;
			auto e = make_engine();
			stdlib::register_all(e);
			dynamic_binder<cell>(*e, "Cell")
				.constructor<int64_t>()
				.property("x", &cell::x)
				.method("<", [](const cell& a_self, const cell& a_other) { return a_self.x < a_other.x; })
				.build();
			e->execute("var m = {}; m[Cell(2)] = 1; m[Cell(1)] = 2; m[Cell(2)] = 3;");
			check_eq((int64_t)2, e->execute("m.size()").as_int());
			check_eq((int64_t)3, e->execute("m[Cell(2)]").as_int());
			check_eq((int64_t)1, e->execute("int first = 0; for (auto& [k, v] : m) { first = k.x; break; } first").as_int());
		});

		test("a_class_with_neither_keys_by_identity_and_warns_once", [this]() {
			auto e = make_engine();
			stdlib::register_all(e);
			std::vector<std::string> warnings;
			e->set_script_warning_handler([&](const std::string& a_message) { warnings.push_back(a_message); });
			e->execute(classes);
			e->execute("var m = {}; I a = I(1); m[a] = 1; m[I(1)] = 2; m[I(1)] = 3; bool found = m.contains(a);");
			check_eq((int64_t)3, e->execute("m.size()").as_int());
			check_eq(true, e->execute("found").as_bool());
			check_eq((size_t)1, warnings.size());
			check_true(!warnings.empty() && warnings[0].find("map keys of class 'I' compare by identity") != std::string::npos, warnings.empty() ? std::string("none") : warnings[0]);
			auto report = e->check(std::string(classes) + "map<I, int> n = {}; n[I(1)] = 1;");
			bool warned = false;
			for (const auto& d : report.diagnostics) { warned = warned || d.message.find("map keys of class 'I' compare by identity") != std::string::npos; }
			std::string all;
			for (const auto& d : report.diagnostics) { all += d.message + "|"; }
			check_true(warned, all);
		});

		test("a_key_operator_that_throws_leaves_the_map_unchanged", [this]() {
			auto e = make_engine();
			stdlib::register_all(e);
			e->execute(R"(
				class T { int v = 0; T(int a) { v = a; } bool operator<(T o) { if (o.v == 7 || v == 7) { throw "boom"; } return v < o.v; } }
				var m = {};
				m[T(1)] = 1;
				string caught = "";
				try { m[T(7)] = 2; } catch (err) { caught = err; }
			)");
			check_eq(std::string("boom"), e->execute("caught").as<std::string>());
			check_eq((int64_t)1, e->execute("m.size()").as_int());
			// a store runs the comparisons of its one search, as std::map does: a largest key is never a comparison's receiver
			e->execute(R"(
				class O { int a = 0; O(int x) { a = x; } bool operator<(O o) { if (a == 99) { throw "receiver"; } return a < o.a; } }
				var n = {};
				n[O(2)] = 2; n[O(1)] = 1;
				string late = "none";
				try { n[O(99)] = 9; } catch (err) { late = err; }
			)");
			check_eq(std::string("none"), e->execute("late").as<std::string>());
			check_eq((int64_t)3, e->execute("n.size()").as_int());
		});

		// a key operator that throws once propagates its first throw: the subscript never retries into a copy that loses the write
		test("a_key_operator_that_throws_once_raises_and_a_retried_store_lands", [this]() {
			auto e = make_engine();
			stdlib::register_all(e);
			e->execute(R"(
				int calls = 0;
				bool armed = false;
				class T { int v = 0; T(int a) { v = a; } bool operator<(T o) { calls += 1; if (armed) { armed = false; throw "boom"; } return v < o.v; } }
				var m = {};
				m[T(1)] = 1;
				armed = true;
				calls = 0;
				string caught = "";
				try { m[T(2)] = 2; } catch (err) { caught = err; }
				int failing_calls = calls;
				m[T(2)] = 2;
			)");
			check_eq(std::string("boom"), e->execute("caught").as<std::string>());
			check_eq((int64_t)1, e->execute("failing_calls").as_int(), "the comparator ran once for the failing store");
			check_eq((int64_t)2, e->execute("m.size()").as_int(), "the store after the throw lands");
			check_eq((int64_t)2, e->execute("m[T(2)]").as_int());
		});

		test("erasing_the_current_class_key_inside_a_range_for_continues_after_it", [this]() {
			auto e = make_engine();
			stdlib::register_all(e);
			e->execute(classes);
			e->execute(R"(
				var m = {};
				for (int i = 0; i < 5; i++) { m[P(i)] = i; }
				string seen = "";
				for (auto& [k, v] : m) { seen += to_string(k.v); if (k.v == 2) { m.remove(P(2)); } }
			)");
			check_eq(std::string("01234"), e->execute("seen").as<std::string>());
			check_eq((int64_t)4, e->execute("m.size()").as_int());
		});
	}
};

}

using class_map_key_tests = jai::foundry::tests::class_map_key_tests;
FOUNDRY_REGISTER(class_map_key_tests)
