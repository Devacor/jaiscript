#include <jaiscript/jaiscript.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/testing/foundry.hpp>
#include "../case_table.hpp"

namespace jai::foundry::tests {

struct MakeSharedVec {
	int x = 0;
	MakeSharedVec() = default;
	explicit MakeSharedVec(int a_x) : x(a_x) {}
};

class make_shared_tests : public suite {
public:
	make_shared_tests() : suite("Make Shared") {}

	std::string outcome(const std::string& a_source) {
		auto eng = make_engine();
		dynamic_binder<MakeSharedVec>(*eng, "Vec")
			.constructor<>()
			.constructor<int>()
			.property("x", &MakeSharedVec::x)
			.build();
		std::string result;
		try {
			auto value = eng->execute(a_source);
			result = value.is_null() ? std::string("<null>") : value.to_string();
		} catch (const std::exception& e) {
			result = std::string("error: ") + e.what();
		}
		return result;
	}

	void same_as_new(const std::string& a_make_shared, const std::string& a_new, const std::string& a_expected) {
		auto spelled = outcome(a_make_shared);
		check_eq(a_expected, spelled);
		check_eq(outcome(a_new), spelled);
	}

	void forge_tests() override {
		test("make_shared_answers_as_new_spells_it", [this]() {
			struct spelling_case { const char* code; const char* spelled_new; const char* expected; };
			const spelling_case cases[] = {
				{"shared_ptr<Vec> m = make_shared<Vec>(1); var n = m; n.x = 7; m.x", "shared_ptr<Vec> m = new Vec(1); var n = m; n.x = 7; m.x", "7"},
				{"auto m = make_shared<Vec>(); m.x", "auto m = new Vec(); m.x", "0"},
				{"class P { int a = 0; P(int v) { a = v; } } auto p = make_shared<P>(4); auto q = p; q.a = 9; p.a",
				 "class P { int a = 0; P(int v) { a = v; } } auto p = new P(4); auto q = p; q.a = 9; p.a", "9"},
				{"namespace ns { class A { int x = 3; } } auto a = make_shared<ns::A>(); a.x", "namespace ns { class A { int x = 3; } } auto a = new ns::A(); a.x", "3"},
				{"auto a = make_shared<array<int>>(); 1", "auto a = new array<int>(); 1", nullptr},
				{"auto a = make_shared<Vec>(1, 2, 3); 1", "auto a = new Vec(1, 2, 3); 1", nullptr},
			};
			check_each(cases, [this](const spelling_case& c) {
				const auto spelled = outcome(c.code);
				if (c.expected) { check_eq(std::string(c.expected), spelled); }
				check_eq(outcome(c.spelled_new), spelled);
			});
		});
		test("checker_types_it_as_shared_ptr", [this]() {
			same_as_new(
				"class P { int a = 2; } auto m = make_shared<P>(); shared_ptr<P> n = m; n.a",
				"class P { int a = 2; } auto m = new P(); shared_ptr<P> n = m; n.a", "2");
			check(outcome("class P { } auto m = make_shared<P>(); m = 5;").find("cannot assign 'int' to 'm' declared 'shared_ptr<P>'") != std::string::npos);
		});
		test("unknown_type_errors_like_new", [this]() {
			auto spelled = outcome("auto a = make_shared<Nope>(); 1");
			check(spelled != "1");
			check_eq(outcome("auto a = new Nope(); 1"), spelled);
		});
		test("missing_arguments_is_a_parse_error", [this]() {
			check_true(outcome("auto a = make_shared<Vec>; 1").rfind("error: parse failed:\n<script>:1:26: error: expected '(' after make_shared<T>", 0) == 0);
		});
		test("user_function_named_make_shared_stays_callable", [this]() {
			check_eq(std::string("6"), outcome("int make_shared(int v) { return v * 2; } make_shared(3)"));
			check_eq(std::string("5"), outcome("int make_shared(int v) { return v; } auto a = make_shared<Vec>(5); a.x"));
		});
		test("handle_results_and_stores_keep_the_declared_handle_type", [this]() {
			check_eq(std::string("111110"), outcome(R"(
				class Base { int v() { return 1; } }
				class Derived : Base { int v() override { return 2; } }
				class Holder {
					shared_ptr<Derived> d = new Derived();
					shared_ptr<Holder> link = null;
					shared_ptr<Base> as_base() { return d; }
					shared_ptr<Holder> me() { return shared_from_this(); }
				}
				int run() {
					shared_ptr<Holder> h = new Holder();
					h.link = new Holder();
					int total = 0;
					shared_ptr<Base> b = null;
					for (int i = 0; i < 5; ++i) { b = h.as_base(); total += b.v(); }
					b = new Base();
					total += b.v() * 100;
					shared_ptr<Holder> again = null;
					for (int i = 0; i < 3; ++i) { again = h.me(); }
					if (again == h) { total += 1000; }
					shared_ptr<Holder> x = null;
					for (int i = 0; i < 4; ++i) { x = h.link; }
					if (x == h.link && x != h) { total += 10000; }
					var dyn = h.as_base();
					dyn = new Holder();
					if (dyn != null) { total += 100000; }
					return total;
				}
				run()
			)"));
		});
		test("handle_result_and_store_of_another_class_raise", [this]() {
			auto eng = make_engine();
			auto r = eng->execute(R"([[warn_mode]];
				class Base { int v() { return 1; } }
				class Other { int w = 0; }
				class Holder {
					shared_ptr<Base> b = new Base();
					shared_ptr<Base> get() { return b; }
					shared_ptr<Other> wrong() { return b; }
				}
				string run() {
					string out = "";
					shared_ptr<Holder> h = new Holder();
					shared_ptr<Other> o = null;
					try { o = h.get(); out += "stored"; } catch (e) { out += e; }
					out += "|";
					try { o = h.wrong(); out += "returned"; } catch (e) { out += e; }
					return out;
				}
				run()
			)");
			check_eq(std::string("Cannot assign Base to 'o' (declared shared_ptr<Other>)|Cannot return Base from 'wrong' (declared -> shared_ptr<Other>)"), r.as<std::string>());
		});
	}
};

}

using make_shared_tests = jai::foundry::tests::make_shared_tests;
FOUNDRY_REGISTER(make_shared_tests)
