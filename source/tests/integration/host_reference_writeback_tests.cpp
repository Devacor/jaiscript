#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/math/adapter.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

// A non-const T& host parameter writes the callee's final value back into the script lvalue its argument names
// in every value kind and every lvalue kind, on both backends
namespace writeback_hosts {
    struct flat2 { float x = 0, y = 0; };
    enum class mode : int { off = 0, on = 1, dim = 2 };
    struct gate {
        int64_t count = 0;
        int64_t level_ = 0;
        int64_t level() const { return level_; }
        void set_level(int64_t v) { level_ = v; }
        void add(int64_t& target, int64_t by) { target += by; count += by; }
        void peek(int64_t& target) const { target = count; }
    };
    struct made {
        int64_t n = 0;
        made() = default;
        explicit made(int64_t& seed) : n(seed) { seed *= 10; }
    };
    // a stateful functor: the direct lane and the ordinary call must reach one object
    struct counter {
        mutable int64_t n = 0;
        int64_t operator()(gate&) const { return ++n; }
    };
    inline int64_t halved(int64_t x) { return x / 2; }
}
JAI_BIND_VEC2((writeback_hosts::flat2), x, y)

namespace jai::foundry::tests {

class host_reference_writeback_tests : public suite {
public:
	host_reference_writeback_tests() : suite("Host Reference Writeback") {}

	static std::shared_ptr<engine> host(bool use_vm) {
		using namespace writeback_hosts;
		auto e = jai::foundry::make_default_engine();
		e->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
		e->execution_budget(0);
		stdlib::register_all(e);
		engine* raw = e.get();
		e->add_function("add_i64", [](int64_t& x, int64_t by) { x += by; });
		e->add_function("triple_int", [](int& x) { x *= 3; });
		e->add_function("halve", [](double& x) { x /= 2; });
		e->add_function("set_f32", [](float& x) { x = 1.25f; });
		e->add_function("flip", [](bool& b) { b = !b; });
		e->add_function("shout", [](std::string& s) { s += "!"; });
		e->add_function("push_size", [](std::vector<int64_t>& v) { v.push_back(static_cast<int64_t>(v.size())); });
		e->add_function("push_any", [raw](std::vector<script_value>& v) { v.push_back(script_value(std::string("tail"), raw)); });
		e->add_function("put_z", [](std::map<std::string, int64_t>& m) { m["z"] = 26; });
		e->add_function("replace", [raw](script_value& v) { v = script_value(std::string("replaced"), raw); });
		e->add_function("nudge", [](flat2& p) { p.x += 10; p.y = -1; });
		e->add_function("set_text", [](std::string& s) { s = "text"; });
		e->add_function("peek_i64", [](const int64_t& x) { return x * 2; });
		e->add_function("peek_str", [](const std::string& s) { return static_cast<int64_t>(s.size()); });
		e->add_function("boom", [](int64_t& x) { x = 7; throw std::runtime_error("boom"); });
		e->add_function("mut_less", [](int64_t& a, int64_t& b) { const bool r = a < b; a = -1; b = -1; return r; });
		e->add_function("swap_ab", [](int64_t& a, int64_t& b) { std::swap(a, b); });
		e->add_function("around", [raw](int64_t& x, script_value during) {
			x = 5;
			engine::external_call_guard guard(raw);
			(void)during.as_function()(std::vector<script_value>{});
			x += 1;
		});
		dynamic_binder<gate>(*e, "Gate")
			.constructor<>()
			.property("count", &gate::count)
			.property("level", &gate::level, &gate::set_level)
			.method("add", &gate::add)
			.method("peek", &gate::peek)
			.method("reset_into", [](gate& self, int64_t& target) { target = self.count; self.count = 0; })
			.method("tick", counter{})
			.method("cycle", [](gate&, mode& m) { m = m == mode::off ? mode::on : mode::dim; })
			.static_method("twice", [](int64_t& x) { x *= 2; })
			.static_method("halved", &halved)
			.build();
		dynamic_binder<made>(*e, "Made")
			.constructor<int64_t&>()
			.property("n", &made::n)
			.build();
		return e;
	}

	void forge_tests() override {
		auto run = [](const std::string& src, bool use_vm) {
			auto e = host(use_vm);
			try { return e->execute(src).to_string(); }
			catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
		};
		auto expect = [this, run](const std::string& src, const std::string& expected, const char* what) {
			for (bool use_vm : {false, true}) {
				check_eq(expected, run(src, use_vm), std::string(what) + (use_vm ? " (vm)" : " (interpreter)"));
			}
		};

		test("every_value_kind_writes_back", [expect]() {
			expect("int a = 2; add_i64(a, 40); to_string(a)", "42", "int64_t&");
			expect("int b = 5; triple_int(b); to_string(b)", "15", "int& (a private copy converted both ways)");
			expect("float c = 5.0; halve(c); to_string(c)", "2.5", "double&");
			expect("float f = 0.0; set_f32(f); to_string(f)", "1.25", "float&");
			expect("bool t = false; flip(t); to_string(t)", "true", "bool&");
			expect("string s = \"hey\"; shout(s); s", "hey!", "std::string&");
			expect("array<int> v = [7]; push_size(v); v.join(\",\")", "7,1", "std::vector<int64_t>& into a typed array");
			expect("var w = [1]; push_any(w); w.join(\",\")", "1,tail", "std::vector<script_value>&");
			expect("map<string, int> m = {\"a\": 1}; put_z(m); to_string(m[\"z\"]) + to_string(m.size())", "262", "std::map&");
			expect("var r = 1; replace(r); r", "replaced", "script_value&");
			expect("var gt = Gate(); int md = 0; gt.cycle(md); to_string(md)", "1", "an enum& (a method: a free function's overload set takes no enum)");
			expect("math::vec2 p = math::vec2(1, 2); nudge(p); to_string(p) + type_of(p)", "(11.0, -1.0)math::vec2", "a math adapter's host type");
		});

		test("every_lvalue_kind_writes_back", [expect]() {
			expect("function f() -> int { int local = 1; add_i64(local, 1); return local; } to_string(f())", "2", "a function's local");
			expect("int g = 1; function f() { add_i64(g, 5); } f(); to_string(g)", "6", "a global from a function");
			expect("class Box { int v = 1; } var b = Box(); add_i64(b.v, 9); to_string(b.v)", "10", "a field");
			expect("class Box { int v = 1; void bump() { add_i64(v, 2); add_i64(this.v, 3); } } var b = Box(); b.bump(); to_string(b.v)", "6", "a bare field and this.field in a method");
			expect("var a = [1, 2, 3]; add_i64(a[0], 10); a.join(\",\")", "11,2,3", "an element by literal index");
			expect("array<int> a = [1, 2, 3]; int i = 1; add_i64(a[i], 10); add_i64(a[i + 1], 20); a.join(\",\")", "1,12,23", "typed elements by identifier and computed index");
			expect("var m = {\"k\": 1}; string key = \"k\"; add_i64(m[\"k\"], 1); add_i64(m[key], 1); to_string(m[\"k\"])", "3", "a map value by literal and identifier key");
			expect("var gt = Gate(); add_i64(gt.count, 4); to_string(gt.count)", "4", "a host member property (the live member)");
			expect("var gt = Gate(); add_i64(gt.level, 6); add_i64(gt.level, 1); to_string(gt.level)", "7", "a host getter/setter property (through the setter)");
			expect("math::vec2 v = math::vec2(1, 2); halve(v.x); set_f32(v.y); to_string(v)", "(0.5, 1.25)", "a math component");
			expect("class Holder { math::vec2 pos = math::vec2(4, 8); } var h = Holder(); halve(h.pos.y); to_string(h.pos)", "(4.0, 4.0)", "a math component of a field");
			expect("array<math::vec2> a = [math::vec2(1, 2), math::vec2(3, 4)]; int i = 0; halve(a[i + 1].y); to_string(a[1])", "(3.0, 2.0)", "a math component behind a computed subscript");
			expect("var a = [1, 2]; var& r = a[1]; add_i64(r, 5); a.join(\",\")", "1,7", "a reference");
			expect("class K { static int s = 1; } add_i64(K::s, 1); to_string(K::s)", "2", "a static");
			expect("int x = 1; int y = 2; swap_ab(x, y); to_string(x) + to_string(y)", "21", "two T& parameters");
		});

		test("the_store_follows_the_assignment_table", [expect, run, this]() {
			for (bool use_vm : {false, true}) {
				check_eq(run("int n = 3; n = 1.5; to_string(n)", use_vm), run("int n = 3; halve(n); to_string(n)", use_vm),
					std::string("a double written back into an int variable converts like n = 1.5") + (use_vm ? " (vm)" : " (interpreter)"));
				check_eq(run("array<int> a = [3]; a[0] = 1.5; a.join(\",\")", use_vm), run("array<int> a = [3]; halve(a[0]); a.join(\",\")", use_vm),
					std::string("an element takes the element boundary") + (use_vm ? " (vm)" : " (interpreter)"));
			}
			expect("int k = 1; string m = \"\"; try { [[warn_mode]] set_text(k); } catch (e) { m = \"raised\"; } m + to_string(k)", "raised1", "a store the variable's type rejects raises and leaves it");
		});

		test("non_lvalues_and_const_refs", [this, expect]() {
			expect("add_i64(5, 1); \"ok\"", "ok", "a literal passes as a temporary");
			expect("int x = 1; add_i64(x + 1, 1); to_string(x)", "1", "a temporary writes nothing back");
			expect("int x = 4; to_string(peek_i64(x)) + to_string(x)", "84", "const T& reads");
			expect("string s = \"abc\"; to_string(peek_str(s)) + s", "3abc", "const std::string& reads");
			// a const T& binding takes the direct lane (one rule with the bound methods' native lane), and a const std::string&
			// parameter is the script's own string, never a copy
			static_assert(detail::host_lane_params<std::tuple<const std::string&, const writeback_hosts::gate&, writeback_hosts::gate&, const int64_t&, int64_t>>());
			static_assert(!detail::host_lane_params<std::tuple<int64_t&>>() && !detail::host_lane_params<std::tuple<std::string&>>());
			for (bool use_vm : {false, true}) {
				auto e = host(use_vm);
				const std::string* seen = nullptr;
				e->add_function("seen_alone", [&seen](const std::string& s) { seen = &s; });
				e->add_function("seen_beside_a_write", [&seen](const std::string& s, int64_t& n) { seen = &s; n = 1; });
				e->execute("string text = \"a string longer than any small-string buffer\"; int n = 0;");
				const std::string* own = &e->get_variable("text").as<const std::string&>();
				e->execute("seen_alone(text);");
				check_true(seen == own, std::string("the direct lane borrows the string") + (use_vm ? " (vm)" : " (interpreter)"));
				seen = nullptr;
				e->execute("seen_beside_a_write(text, n);");
				check_true(seen == own, std::string("the writeback call borrows it too") + (use_vm ? " (vm)" : " (interpreter)"));
			}
		});

		test("methods_statics_and_constructors", [expect]() {
			expect("var gt = Gate(); int t = 1; gt.add(t, 5); to_string(t) + to_string(gt.count)", "65", "a member-pointer method");
			expect("var gt = Gate(); gt.count = 9; int t = 0; gt.peek(t); to_string(t)", "9", "a const method");
			expect("var gt = Gate(); gt.count = 3; int t = 0; gt.reset_into(t); to_string(t) + to_string(gt.count)", "30", "a lambda method with self");
			expect("int t = 4; Gate::twice(t); to_string(t)", "8", "a static method");
			expect("int seed = 3; var md = Made(seed); to_string(md.n) + to_string(seed)", "330", "a constructor");
			expect("int seed = 2; var md = new Made(seed); to_string(md.n) + to_string(seed)", "220", "a new expression");
		});

		test("arity_texts_count_one_argument_in_the_singular", [this, expect]() {
			auto caught = [](const std::string& call) {
				return "[[warn_mode]]; var m = \"\"; try { " + call + "; } catch (err) { m = err; } m";
			};
			expect(caught("Gate::twice()"), "'Gate::twice' expects 1 argument, got 0", "a static lambda");
			expect(caught("Gate::halved(1, 2)"), "'Gate::halved' expects 1 argument, got 2", "a static function pointer");
			// a script call meets the overload set first: the bound callable's own count check answers a host's direct call
			auto e = host(true);
			auto text = [](const checked_result<script_value>& r) { return format_error_message_numeric(r.message(), r.symbol_id(), r.symbol_id2()); };
			auto by_value = make_function_binder([](int64_t x) { return -x; }, e.get()).bind();
			check_eq(std::string("Function expects 1 argument, got 2"), text(by_value(std::vector<script_value>(2, script_value(int64_t{1}, e.get())))));
			auto by_reference = make_function_binder([](int64_t& x) { x = 1; }, e.get()).bind();
			check_eq(std::string("Function expects 1 argument, got 0"), text(by_reference(std::vector<script_value>{})));
			auto two = make_function_binder([](int64_t a, int64_t b) { return a + b; }, e.get()).bind();
			check_eq(std::string("Function expects 2 arguments, got 1"), text(two(std::vector<script_value>(1, script_value(int64_t{1}, e.get())))));
		});

		test("later_arguments_and_throws", [expect]() {
			expect("var m = {\"k\": 1}; function side() -> int { return 5; } add_i64(m[\"k\"], side()); to_string(m[\"k\"])", "6", "a map value ahead of an argument that runs code");
			expect("var a = [1, 2, 3]; int i = 0; function side() -> int { return 10; } add_i64(a[i + 1], side()); a.join(\",\")", "1,12,3", "a computed element ahead of an argument that runs code");
			expect("int x = 1; function bump_x() -> int { x = 100; return 1; } add_i64(x, bump_x()); to_string(x)", "101", "the callee starts from the variable as it is at the call");
			expect("int x = 1; string m = \"\"; try { boom(x); } catch (e) { m = e; } m + to_string(x)", "boom7", "a callee's write before it throws stands");
		});

		test("a_callback_during_the_call_sees_the_variable_before_the_store", [expect]() {
			// the callee works on its copy: script the host runs meanwhile sees the variable as it was, and the variable takes
			// the callee's final value when it returns (the same whether the local lives in a cell or in its frame slot)
			expect("function f() -> string { int v = 1; string seen = \"\"; around(v, [&]() { seen = to_string(v); }); return seen + \"|\" + to_string(v); } f()", "1|6", "a local a lambda captures by reference");
			expect("function f() -> string { int v = 1; around(v, [&]() { v = 100; }); return to_string(v); } f()", "6", "the store after the call wins over a write during it");
			expect("function f() -> string { int v = 1; int w = 0; around(v, [=]() { }); around(v, []() { }); return to_string(v) + to_string(w); } f()", "60", "an unboxed local, twice");
			expect("int g = 1; string seen = \"\"; function peek() { seen = to_string(g); } around(g, peek); seen + \"|\" + to_string(g)", "1|6", "a global a callback reads by name");
			expect("class Box { int v = 1; } var b = Box(); string seen = \"\"; around(b.v, [&]() { seen = to_string(b.v); }); seen + \"|\" + to_string(b.v)", "1|6", "a field");
			expect("function f() -> int { int v = 1; for (int i = 0; i < 3; i++) { add_i64(v, i); } return v; } to_string(f())", "4", "a loop local stays a plain slot");
			expect("function f() -> int { int v = 1; for (int i = 0; i < 3; i++) { v = v + 1; add_i64(v, i); } return v; } to_string(f())", "7", "a loop local the block writes takes the host's value after the call");
		});

		test("builtin_callbacks_take_the_elements_as_lvalues", [expect]() {
			expect("var arr = [3, 1, 2]; var cmp = mut_less; arr.sort(cmp); arr.join(\",\") + type_of(cmp)", "-1,-1,-1function", "a host T& comparator writes the elements it compares, never the sort's own argument");
			expect("bool cmp(var& a, var& b) { return a < b; } var arr = [3, 1, 2]; arr.sort(cmp); arr.join(\",\") + type_of(cmp)", "1,2,3function", "a script T& comparator binds the elements");
			expect("var arr = [3, 1, 2]; arr.sort([](int& p, int& q) { p = p * 1; return p < q; }); arr.join(\",\")", "1,2,3", "a lambda comparator that writes through its parameters");
			expect("var cmpf = [](var& p, var& q) { p = 0; return false; }; var arr = [3, 1, 2]; arr.sort(cmpf); type_of(cmpf)", "function", "the comparator's writes never reach the variable passed to sort");
			expect("var a = [1, 2, 3]; var r = array::filter(a, [](int& x) { x = x * 10; return x > 10; }); r.join(\",\") + \"|\" + a.join(\",\")", "20,30|10,20,30", "filter reference parameters target the live source");
			expect("var a = [1, 2, 3, 4]; a.remove_if([](int& x) { x = x + 1; return x > 3; }); a.join(\",\")", "2,3", "remove_if keeps the written survivors");
			expect("var m = {\"a\": 1, \"b\": 2}; var f = map::filter(m, [](var& k, var& v) { v = v * 10; return v > 10; }); to_string(m[\"a\"]) + to_string(m[\"b\"]) + to_string(f.size())", "10201", "a map callback's value aliases the entry");
			expect("var m = {\"a\": 1, \"b\": 2}; m.remove_if([](var& k, var& v) { k = \"z\"; v = v + 1; return v > 2; }); to_string(m[\"a\"]) + to_string(m.size())", "21", "a map key reaches the callback as a copy");
		});

		test("direct_lane_and_ordinary_call_share_one_functor", [expect]() {
			expect("var gt = Gate(); gt.tick(); gt.tick(); var t = reflect::bind(gt, \"tick\"); to_string(t())", "3", "one functor behind both calls");
		});
	}
};

}

using host_reference_writeback_tests = jai::foundry::tests::host_reference_writeback_tests;
FOUNDRY_REGISTER(host_reference_writeback_tests)
