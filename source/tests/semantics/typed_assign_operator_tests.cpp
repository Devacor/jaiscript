// docs/site/guide/03-value-semantics.html "Assignment runs operator=": '=' into a slot declared a class by value runs the class's
// operator= when one of its overloads takes the right-hand value, as C++ does, wherever the slot lives; with none that takes
// it the implicit copy assignment (or the conversion table) answers as before. Copy-initialization is copy construction.
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <string>

namespace jai::foundry::tests {

class typed_assign_operator_tests : public suite {
public:
	typed_assign_operator_tests() : suite("Typed Assign Operator") {}

	static constexpr const char* k_classes = R"(
		class R { int x = 1; int calls = 0; void operator=(R o) { x = o.x + 100; calls = calls + 1; } }
		class S : R { int s = 5; }
		class Q { int x = 1; int y = 2; void operator=(int v) { x = v; } }
		class T { int x = 1; void operator=(T o) { if (o.x < 0) { throw "refused"; } x = o.x; } }
		class Self { int x = 1; int seen = 0; void operator=(const Self& o) { if (same_object(this, o)) { seen = 1; } else { x = o.x; } } }
		class W { R r; int k = 0; void put(R o) { r = o; } }
		class G { static R sr; }
		R gr;
		string run(function f) { string out = ""; try { out = f(); } catch (e) { out = "ERR " + e; } return out; }
	)";

	void expect(const std::string& a_what, const std::string& a_body, const std::string& a_expected) {
		auto eng = make_engine();
		jai::stdlib::register_all(eng);
		eng->execute(k_classes);
		auto value = eng->execute("run([]() { " + a_body + " })");
		check_eq(a_expected, value.is_null() ? std::string("<null>") : value.as_string(), a_what + ": " + a_body);
	}

	void forge_tests() override {
		test("a_typed_local_runs_the_class_operator", [this]() {
			expect("a local", R"(R a = R(); R b = R(); b.x = 2; a = b; return to_string(a.x) + " " + to_string(a.calls);)", "102 1");
			expect("a subclass value the overload takes", R"(R a = R(); S b = S(); b.x = 3; a = b; return to_string(a.x) + " " + type_of(a);)", "103 R");
			expect("in a loop", R"(R a = R(); R b = R(); for (int i = 0; i < 3; ++i) { b.x = i; a = b; } return to_string(a.x) + " " + to_string(a.calls);)", "102 3");
			expect("a temporary on the right", R"(R a = R(); a = R(); return to_string(a.x);)", "101");
			expect("the expression's value is the assigned object", R"(R a = R(); R b = R(); R c = R(); c = (a = b); return to_string(a.x) + " " + to_string(c.x);)", "101 201");
			expect("through an R&", R"(R a = R(); R& r = a; R b = R(); b.x = 4; r = b; return to_string(a.x);)", "104");
			expect("a [&] capture", R"(R a = R(); R b = R(); auto g = [&]() { a = b; }; g(); return to_string(a.x);)", "101");
			expect("a by-value parameter", R"(auto f = [](R p) { R b = R(); b.x = 7; p = b; return to_string(p.x); }; return f(R());)", "107");
		});

		test("fields_statics_globals_and_elements_run_the_operator", [this]() {
			expect("a field", R"(W w = W(); R b = R(); b.x = 2; w.r = b; return to_string(w.r.x);)", "102");
			expect("a bare field store in a method", R"(W w = W(); R b = R(); b.x = 3; w.put(b); return to_string(w.r.x);)", "103");
			expect("a field in a loop", R"(W w = W(); R b = R(); for (int i = 0; i < 3; ++i) { w.r = b; } return to_string(w.r.calls);)", "3");
			expect("a static", R"(R b = R(); b.x = 4; G::sr = b; return to_string(G::sr.x);)", "104");
			expect("a global", R"(R b = R(); b.x = 5; gr = b; return to_string(gr.x);)", "105");
			expect("an array<R> element", R"(array<R> xs = [R(), R()]; R b = R(); b.x = 6; xs[1] = b; return to_string(xs[0].x) + " " + to_string(xs[1].x);)", "1 106");
			expect("an array<R> element in a loop", R"(array<R> xs = [R(), R()]; R b = R(); for (int i = 0; i < 2; ++i) { xs[i] = b; } return to_string(xs[0].x) + " " + to_string(xs[1].calls);)", "101 1");
			expect("a map<K, R> value", R"(map<string, R> m = {"k": R()}; R b = R(); b.x = 7; m["k"] = b; return to_string(m["k"].x);)", "107");
			expect("an implicit copy assignment runs a member's operator=", R"(W a = W(); W b = W(); b.r.x = 8; b.k = 9; a = b; return to_string(a.r.x) + " " + to_string(a.k);)", "108 9");
		});

		test("copy_initialization_is_copy_construction", [this]() {
			expect("a declaration", R"(R b = R(); b.x = 2; R a = b; return to_string(a.x) + " " + to_string(a.calls);)", "2 0");
			expect("a parameter", R"(auto f = [](R p) { return to_string(p.x) + " " + to_string(p.calls); }; R b = R(); b.x = 3; return f(b);)", "3 0");
			expect("a return", R"(auto f = []() -> R { R b = R(); b.x = 4; return b; }; R a = f(); return to_string(a.x);)", "4");
			expect("an array literal's element", R"(R b = R(); b.x = 5; array<R> xs = [b]; return to_string(xs[0].x);)", "5");
		});

		test("no_overload_that_takes_the_value_keeps_the_typed_store", [this]() {
			expect("operator=(int) on a Q value copies memberwise", R"(Q a = Q(); Q b = Q(); b.x = 7; b.y = 8; a = b; return to_string(a.x) + " " + to_string(a.y);)", "7 8");
			expect("an array<Q> element takes an int through operator=(int)", R"(array<Q> xs = [Q()]; xs[0] = 5; return to_string(xs[0].x);)", "5");
			expect("a map<K, Q> value takes an int through operator=(int)", R"(map<int, Q> m = {0: Q()}; m[0] = 6; return to_string(m[0].x);)", "6");
			expect("operator=(int) takes an int",R"(Q a = Q(); a = 5; return to_string(a.x) + " " + to_string(a.y);)", "5 2");
			expect("an unrelated value stays an error", R"(R a = R(); a = 3; return "stored";)", "ERR Cannot convert int to R (no suitable single-argument constructor)");
		});

		test("self_assignment_and_a_throwing_operator", [this]() {
			expect("by-value self-assignment runs the operator on a copy", R"(R a = R(); a = a; return to_string(a.x) + " " + to_string(a.calls);)", "101 1");
			expect("self-assignment through a reference parameter sees itself",R"(Self a = Self(); a.x = 3; a = a; return to_string(a.x) + " " + to_string(a.seen);)", "3 1");
			expect("an operator that throws leaves the object as it was", R"(T a = T(); a.x = 4; T b = T(); b.x = -1; try { a = b; } catch (e) { return "caught " + e + " " + to_string(a.x); } return "none";)", "caught refused 4");
			expect("an element whose operator throws", R"(array<T> xs = [T()]; T b = T(); b.x = -1; try { xs[0] = b; } catch (e) { return "caught " + to_string(xs[0].x); } return "none";)", "caught 1");
		});
	}
};

}

using typed_assign_operator_tests = jai::foundry::tests::typed_assign_operator_tests;
FOUNDRY_REGISTER(typed_assign_operator_tests)
