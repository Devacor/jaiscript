#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/engine_access.hpp>
#include <jaiscript/detail/lvalue_shape.hpp>
#include <jaiscript/detail/null_member_diagnostic.hpp>
#include <jaiscript/detail/operator_ids.hpp>

#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

// An error text names an operand as the program wrote it. The VM's graft compiles a call's result, an operand taken ahead of the
// call and a reference parameter's variable as slots of its own: none of their names reaches a text, on any backend.
namespace jai::foundry::tests {

class operand_name_text_tests : public suite {
public:
	operand_name_text_tests() : suite("Operand Name Texts") {}

	enum class backend { interpreter, vm, vm_reference };

	static std::string first_line(backend a_which, const std::string& a_source) {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(a_which == backend::interpreter ? backend_type::interpreter : backend_type::vm);
		if (a_which == backend::vm_reference) { e->vm_reference_mode(true); }
		stdlib::register_all(*e);
		try {
			return "= " + e->execute(a_source).to_string();
		} catch (const std::exception& error) {
			const std::string text = error.what();
			return text.substr(0, text.find('\n'));
		}
	}

	struct text_case {
		const char* body;       // statements of `int probe() { ... }`, which the program then calls
		const char* expected;   // the error's first line, or "= " and the value probe() returns
	};

	static void check_cases(const std::string& a_prelude, const std::vector<text_case>& a_cases) {
		std::string failures;
		for (const text_case& c : a_cases) {
			const std::string source = a_prelude + "int probe() { " + c.body + " }\nprobe();\n";
			for (const backend which : {backend::interpreter, backend::vm, backend::vm_reference}) {
				const std::string got = first_line(which, source);
				if (got != c.expected) {
					failures += std::string(c.body) + (which == backend::interpreter ? " (interpreter)" : which == backend::vm ? " (vm)" : " (vm-reference)") +
					            "\n  expected: " + c.expected + "\n  got:      " + got + "\n";
				}
			}
		}
		check_true(failures.empty(), failures);
	}

	static std::string prelude() {
		return
			"int add(int a, int b) { return a + b; }\n"
			"bool both(bool a, bool b) { return a && b; }\n"
			"float half(float a) { return a / 2.0; }\n"
			"var next(int a) { return a + 1; }\n"
			"var none(int a) { return null; }\n"
			"class Thing { int v = 1; var w = null; int get() { return v; } int twice(int a) { return a * v; } }\n"
			"var gnull = null;\n"
			"var gint = 5;\n";
	}

	void forge_tests() override {
		test("a_call_result_called_is_a_computed_callee", [this]() {
			check_cases(prelude(), {
				{"return both(true, false)();", "Cannot call a value of type bool: it is not a function"},
				{"return add(1, 2)();", "Cannot call a value of type int: it is not a function"},
				{"return add(1, 2)(add(3, 4));", "Cannot call a value of type int: it is not a function"},
				{"return add(add(1, 2), 3)();", "Cannot call a value of type int: it is not a function"},
				{"return half(3.0)();", "Cannot call a value of type float: it is not a function"},
				{"return next(1)();", "Cannot call a value of type int: it is not a function"},
				{"int q = add(1, 2)(); return q;", "Cannot call a value of type int: it is not a function"},
				{"var q = add(1, 2)(); return 0;", "Cannot call a value of type int: it is not a function"},
				{"add(1, 2)(); return 0;", "Cannot call a value of type int: it is not a function"},
				{"gint = add(1, 2)(); return 0;", "Cannot call a value of type int: it is not a function"},
				{"if (add(1, 2)()) { return 1; } return 0;", "Cannot call a value of type int: it is not a function"},
				{"while (both(true, true)()) { return 1; } return 0;", "Cannot call a value of type bool: it is not a function"},
				{"return add(1, 2)() ? 1 : 0;", "Cannot call a value of type int: it is not a function"},
				{"return add(1, 2)() + 1;", "Cannot call a value of type int: it is not a function"},
				{"return 1 + add(1, 2)();", "Cannot call a value of type int: it is not a function"},
				{"return -add(1, 2)();", "Cannot call a value of type int: it is not a function"},
				{"switch (add(1, 2)()) { case 1: return 1; } return 0;", "Cannot call a value of type int: it is not a function"},
				{"Thing t; return t.twice(2)();", "Cannot call a value of type int: it is not a function"},
				{"Thing t; return t.get()(add(1, 2));", "Cannot call a value of type int: it is not a function"},
				{"var l = (int x) => x; return l(1)(add(1, 2));", "Cannot call a value of type int: it is not a function"},
				{"return none(1)();", "Cannot call null: it is not a function"},
				{"return none(add(1, 2))();", "Cannot call null: it is not a function"},
			});
		});

		test("an_element_called_is_a_computed_callee", [this]() {
			check_cases(prelude(), {
				{"array<var> fs = [1]; return fs[0](add(1, 2));", "Cannot call a value of type int: it is not a function"},
				{"array<var> fs = [1]; return fs[add(0, 0)]();", "Cannot call a value of type int: it is not a function"},
				{"array<var> fs = [null]; return fs[0](add(add(1, 2), 3));", "Cannot call null: it is not a function"},
				{"map<string, var> fs = {\"k\": \"text\"}; return fs[\"k\"](add(1, 2));", "Cannot call a value of type string: it is not a function"},
			});
		});

		test("a_named_callee_keeps_its_name", [this]() {
			check_cases(prelude(), {
				{"var h = 5; return h(add(1, 2));", "Cannot call 'h': it is an int, not a function"},
				{"var h = 5; return h(add(add(1, 2), 3));", "Cannot call 'h': it is an int, not a function"},
				{"var h = null; return h(1, add(add(1, 2), 4));", "Cannot call 'h': it is null, not a function"},
				{"var h = \"text\"; return h(add(add(1, 2), 3), add(4, 5));", "Cannot call 'h': it is a string, not a function"},
				{"var h = 2.5; return h(add(add(1, 2), 3)) + add(4, 5);", "Cannot call 'h': it is a float, not a function"},
				{"Thing t; var h = t; return h(add(add(1, 2), 3));", "Cannot call 'h': it is a Thing, not a function"},
				{"return gint(add(1, 2));", "Cannot call 'gint': it is an int, not a function"},
				{"return gint(add(add(1, 2), 3));", "Cannot call 'gint': it is an int, not a function"},
				{"return gnull(add(add(1, 2), 3));", "Cannot call 'gnull': it is null, not a function"},
				{"Thing t; return t.v(add(1, 2));", "Cannot call 'v': it is an int, not a function"},
				{"Thing t; return t.v(add(add(1, 2), 3));", "Cannot call 'v': it is an int, not a function"},
				{"Thing t; return t.w(add(add(1, 2), 3));", "Cannot call 'w': it is null, not a function"},
				{"auto apply = (var fn) => fn(add(1, 2)); return apply(3);", "Cannot call 'fn': it is an int, not a function"},
				{"auto apply = (var fn) => fn(add(add(1, 2), 3)); return apply(true);", "Cannot call 'fn': it is a bool, not a function"},
				{"auto apply = function(var fn) { return fn(add(add(1, 2), 3)); }; return apply(null);", "Cannot call 'fn': it is null, not a function"},
			});
			check_cases(prelude() + "int call(var fn) { return fn(add(add(1, 2), 3)); }\n", {
				{"return call(2.5);", "Cannot call 'fn': it is a float, not a function"},
				{"return call(\"text\");", "Cannot call 'fn': it is a string, not a function"},
			});
		});

		// A value no call takes is named by its type, whichever it is: the texts read "invalid" for a utf8_string and every math
		// kind, and the VM, which keeps a typed math local as its components, named no callee at all there
		test("a_called_value_is_named_by_its_type_whatever_holds_it", [this]() {
			check_cases("[[warn_mode]];\nmath::vec2 gvec = math::vec2(1.0, 2.0);\nvar gany = math::vec3(1.0, 2.0, 3.0);\nutf8_string gtext = \"t\";\n", {
				{"gvec(); return 0;", "Cannot call 'gvec': it is a math::vec2, not a function"},
				{"gany(); return 0;", "Cannot call 'gany': it is a math::vec3, not a function"},
				{"gtext(); return 0;", "Cannot call 'gtext': it is a utf8_string, not a function"},
				{"math::vec2 local = math::vec2(1.0, 2.0); local(); return 0;", "Cannot call 'local': it is a math::vec2, not a function"},
				{"math::vec2 local = math::vec2(1.0, 2.0); local(1, 2); return 0;", "Cannot call 'local': it is a math::vec2, not a function"},
				{"math::vec2 local = math::vec2(1.0, 2.0); float sum = local.x + local.y; local(); return 0;", "Cannot call 'local': it is a math::vec2, not a function"},
				{"math::vec3 local = math::vec3(1.0, 2.0, 3.0); local(); return 0;", "Cannot call 'local': it is a math::vec3, not a function"},
				{"math::vec4 local = math::vec4(1.0, 2.0, 3.0, 4.0); local(); return 0;", "Cannot call 'local': it is a math::vec4, not a function"},
				{"math::ivec2 local = math::ivec2(1, 2); local(); return 0;", "Cannot call 'local': it is a math::ivec2, not a function"},
				{"math::ivec3 local = math::ivec3(1, 2, 3); local(); return 0;", "Cannot call 'local': it is a math::ivec3, not a function"},
				{"math::color local = math::color(1.0, 0.5, 0.25, 1.0); local(); return 0;", "Cannot call 'local': it is a math::color, not a function"},
				{"var loose = math::vec2(1.0, 2.0); loose(); return 0;", "Cannot call 'loose': it is a math::vec2, not a function"},
				{"auto inferred = math::ivec2(1, 2); inferred(); return 0;", "Cannot call 'inferred': it is a math::ivec2, not a function"},
				{"utf8_string local = \"t\"; local(); return 0;", "Cannot call 'local': it is a utf8_string, not a function"},
				{"var loose = gtext; loose(); return 0;", "Cannot call 'loose': it is a utf8_string, not a function"},
				{"(math::vec2(1.0, 2.0))(); return 0;", "Cannot call a value of type math::vec2: it is not a function"},
				{"array<math::color> shades = [math::color(1.0, 1.0, 1.0, 1.0)]; shades[0](); return 0;", "Cannot call a value of type math::color: it is not a function"},
				{"array<utf8_string> words = [gtext]; words[0](); return 0;", "Cannot call a value of type utf8_string: it is not a function"},
				// the kinds the texts already named stay as they were
				{"int count = 3; count(); return 0;", "Cannot call 'count': it is an int, not a function"},
				{"float ratio = 1.5; ratio(); return 0;", "Cannot call 'ratio': it is a float, not a function"},
				{"array<int> xs = [1]; xs(); return 0;", "Cannot call 'xs': it is an array, not a function"},
			});
			check_cases("[[warn_mode]];\nint through(math::vec2 p) { p(); return 0; }\n", {
				{"return through(math::vec2(1.0, 2.0));", "Cannot call 'p': it is a math::vec2, not a function"},
			});
			// a typed math local the VM keeps as components answers its other uses as before
			check_cases("", {
				{"math::vec2 local = math::vec2(1.0, 2.0); local = local + local; return math::itrunc(local.x + local.y);", "= 6"},
			});
		});

		// E::v, C::s and ns::x name their callee whatever the arguments are: the VM's check ahead of an argument that runs code named none.
		// The load refuses a call of a name declared with a type no call takes; [[warn_mode]] leaves the verdict to the run, whose text this pins
		test("a_qualified_callee_keeps_its_name", [this]() {
			const std::string scoped = prelude() +
				"enum Lvl { low, mid, high }\n"
				"class Cfg { static int max = 3; static var hook = null; static string label = \"l\"; }\n"
				"namespace cfg { int lo = 1; var hooks = [1]; namespace inner { float deep = 2.5; } }\n"
				"using cfg::lo, Lvl::low;\n";
			check_cases(scoped, {
				{"[[warn_mode]]; return Lvl::high(1);", "Cannot call 'high': it is an int, not a function"},
				{"[[warn_mode]]; return Lvl::high(add(1, 2));", "Cannot call 'high': it is an int, not a function"},
				{"[[warn_mode]]; return Lvl::high(gint);", "Cannot call 'high': it is an int, not a function"},
				{"[[warn_mode]]; return Lvl::low(add(1, 2)) + 1;", "Cannot call 'low': it is an int, not a function"},
				{"[[warn_mode]]; int n = 0; for (int i = 0; i < 2; ++i) { n += Lvl::mid(add(i, 1)); } return n;", "Cannot call 'mid': it is an int, not a function"},
				{"[[warn_mode]]; return Cfg::max(3);", "Cannot call 'max': it is an int, not a function"},
				{"[[warn_mode]]; return Cfg::max(add(1, 2));", "Cannot call 'max': it is an int, not a function"},
				{"return Cfg::hook(add(1, 2));", "Cannot call 'hook': it is null, not a function"},
				{"[[warn_mode]]; return Cfg::label(add(1, 2));", "Cannot call 'label': it is a string, not a function"},
				{"[[warn_mode]]; return cfg::lo(1);", "Cannot call 'lo': it is an int, not a function"},
				{"[[warn_mode]]; return cfg::lo(add(1, 2));", "Cannot call 'lo': it is an int, not a function"},
				{"return cfg::hooks(add(1, 2));", "Cannot call 'hooks': it is an array, not a function"},
				{"[[warn_mode]]; return cfg::inner::deep(gint);", "Cannot call 'deep': it is a float, not a function"},
				{"[[warn_mode]]; return lo(add(1, 2));", "Cannot call 'lo': it is an int, not a function"},
				{"[[warn_mode]]; return low(add(1, 2));", "Cannot call 'low': it is an int, not a function"},
				{"return ::gint(add(1, 2));", "Cannot call 'gint': it is an int, not a function"},
				{"[[warn_mode]]; auto l = (int x) => Lvl::high(add(x, 1)); return l(1);", "Cannot call 'high': it is an int, not a function"},
				{"[[warn_mode]]; auto l = function(int x) { return Cfg::max(add(x, 1)); }; return l(1);", "Cannot call 'max': it is an int, not a function"},
				{"[[warn_mode]]; auto l = (int x) => cfg::lo(add(x, 1)); return l(1);", "Cannot call 'lo': it is an int, not a function"},
			});
			const std::string in_class = scoped +
				"class S {\n"
				"    Lvl cur = Lvl::mid;\n"
				"    int by_enum() { [[warn_mode]] return Lvl::high(cur); }\n"
				"    int by_static() { [[warn_mode]] return Cfg::max(cur); }\n"
				"    int by_namespace() { [[warn_mode]] return cfg::lo(cur); }\n"
				"    int by_using() { [[warn_mode]] return lo(cur); }\n"
				"    static int from_static() { [[warn_mode]] return Lvl::low(add(1, 2)); }\n"
				"}\n"
				"class D : Cfg { int inherited() { return max(add(1, 2)); } int qualified() { [[warn_mode]] return Cfg::max(add(1, 2)); } }\n";
			check_cases(in_class, {
				{"return S().by_enum();", "Cannot call 'high': it is an int, not a function"},
				{"return S().by_static();", "Cannot call 'max': it is an int, not a function"},
				{"return S().by_namespace();", "Cannot call 'lo': it is an int, not a function"},
				{"return S().by_using();", "Cannot call 'lo': it is an int, not a function"},
				{"return S::from_static();", "Cannot call 'low': it is an int, not a function"},
				{"return D().inherited();", "Cannot call 'max': it is an int, not a function"},
				{"return D().qualified();", "Cannot call 'max': it is an int, not a function"},
			});
			const std::pair<const char*, const char*> at_program_scope[] = {
				{"[[warn_mode]] Lvl::high(add(1, 2));", "Cannot call 'high': it is an int, not a function"},
				{"[[warn_mode]] Cfg::max(add(1, 2));", "Cannot call 'max': it is an int, not a function"},
				{"[[warn_mode]] cfg::lo(add(1, 2));", "Cannot call 'lo': it is an int, not a function"},
				{"[[warn_mode]] lo(add(1, 2));", "Cannot call 'lo': it is an int, not a function"},
				{"[[warn_mode]] for (int i = 0; i < 2; ++i) { Lvl::high(i); }", "Cannot call 'high': it is an int, not a function"},
				{"[[warn_mode]] var r = cfg::inner::deep(gint) + 1;", "Cannot call 'deep': it is a float, not a function"},
			};
			std::string failures;
			for (const auto& c : at_program_scope) {
				for (const backend which : {backend::interpreter, backend::vm, backend::vm_reference}) {
					const std::string got = first_line(which, scoped + c.first + "\n");
					if (got != c.second) {
						failures += std::string(c.first) + (which == backend::interpreter ? " (interpreter)" : which == backend::vm ? " (vm)" : " (vm-reference)") +
						            "\n  expected: " + c.second + "\n  got:      " + got + "\n";
					}
				}
			}
			check_true(failures.empty(), failures);
		});

		// a block holds a scalar local and a compiler value in cells: the callee probe reads the slot, which takes the cell's value first
		test("a_callee_a_loop_holds_reads_as_its_value", [this]() {
			check_cases(prelude(), {
				{"int n = 0; for (int i = 0; i < 2; ++i) { n += add(i, 1)(); } return n;", "Cannot call a value of type int: it is not a function"},
				{"int n = 0; for (int i = 0; i < 3; ++i) { n += next(i)(); } return n;", "Cannot call a value of type int: it is not a function"},
				{"int n = 0; int i = 0; while (i < 3) { n += add(i, 1)(); i += 1; } return n;", "Cannot call a value of type int: it is not a function"},
				{"int n = 0; for (int i = 0; i < 3; ++i) { n += add(i, 1)(add(i, 2)); } return n;", "Cannot call a value of type int: it is not a function"},
				{"int n = 0; array<int> xs = [1, 2, 3]; for (int x : xs) { n += add(x, 1)(); } return n;", "Cannot call a value of type int: it is not a function"},
				{"float t = 0.0; for (int i = 0; i < 3; ++i) { t += half(t)(); } return 0;", "Cannot call a value of type float: it is not a function"},
				{"var h = 5; int n = 0; for (int i = 0; i < 3; ++i) { n += h(add(add(i, 1), 2)); } return n;", "Cannot call 'h': it is an int, not a function"},
				{"var h = 5; int n = 0; for (int i = 0; i < 3; ++i) { h = h + 1; n += h(add(i, 1)); } return n;", "Cannot call 'h': it is an int, not a function"},
				{"int n = 0; for (int i = 0; i < 3; ++i) { var h = i + 1; n += h(i); } return n;", "Cannot call 'h': it is an int, not a function"},
				{"[[warn_mode]]; int n = 0; for (int i = 0; i < 3; ++i) { n += i(5); } return n;", "Cannot call 'i': it is an int, not a function"},
				{"[[warn_mode]]; int n = 0; for (int i = 0; i < 3; ++i) { n += n(5); } return n;", "Cannot call 'n': it is an int, not a function"},
				{"[[warn_mode]]; array<int> xs = [1, 2, 3]; int n = 0; for (int p : xs) { n += p(5); } return n;", "Cannot call 'p': it is an int, not a function"},
				{"array<int> xs = [1, 2, 3]; int n = 0; for (auto& p : xs) { n += p(5); } return n;", "Cannot call 'p': it is an int, not a function"},
				{"[[warn_mode]]; int n = 0; float g = 1.5; for (int i = 0; i < 3; ++i) { g = g * 2.0; n += g(i); } return n;", "Cannot call 'g': it is a float, not a function"},
				{"[[warn_mode]]; int n = 0; bool b = true; for (int i = 0; i < 3; ++i) { b = !b; n += b(i); } return n;", "Cannot call 'b': it is a bool, not a function"},
				{"[[warn_mode]]; int n = 0; int k = 0; while (k < 3) { k = k + 1; n += k(k); } return n;", "Cannot call 'k': it is an int, not a function"},
				{"var h = (int x) => x * 2; int n = 0; for (int i = 0; i < 3; ++i) { n += h(add(add(i, 1), 2)); } return n;", "= 24"},
				{"array<var> fs = [next, next]; int n = 0; for (auto& p : fs) { n += p(5); } return n;", "= 12"},
				{"int n = 0; for (int i = 0; i < 3; ++i) { var g = next; n += g(i); } return n;", "= 6"},
				{"int n = 0; for (int i = 0; i < 3; ++i) { n += add(i, next(i)); } return n;", "= 9"},
			});
			// an element alias the block holds as an index: a callable element is called through the reference its slot takes
			check_cases(prelude() + "class Scale { int k = 1; int operator()(int x) { return x * k; } }\n"
				"array<Scale> scales() { array<Scale> ss = [Scale(), Scale(), Scale()]; ss[0].k = 2; ss[1].k = 3; ss[2].k = 5; return ss; }\n", {
				{"array<Scale> ss = scales(); int n = 0; for (auto& p : ss) { n += p(5); } return n;", "= 50"},
				{"array<Scale> ss = scales(); int n = 0; for (auto& p : ss) { n += p.k; n += p(1); } return n;", "= 20"},
				{"array<Scale> ss = scales(); int n = 0; for (int i = 0; i < 3; ++i) { auto& p = ss[i]; n += p(i); } return n;", "= 13"},
				{"array<Thing> ts = [Thing(), Thing()]; int n = 0; for (auto& p : ts) { n += p.v; n += p(1); } return n;", "Cannot call 'p': it is a Thing, not a function"},
			});
		});

		test("a_reference_parameter_keeps_its_own_name", [this]() {
			check_cases(prelude() +
				"int field_of(var& p) { return p.v; }\n"
				"int store_into(var& p) { p.v = 1; return 1; }\n"
				"int deep_field_of(var& p) { return field_of(p); }\n", {
				{"var q = null; return field_of(q);", "Cannot access 'v' on null ('p' is null)"},
				{"var q = null; return store_into(q);", "Cannot assign to 'v' on null ('p' is null)"},
				{"var q = null; return deep_field_of(q);", "Cannot access 'v' on null ('p' is null)"},
				{"var q = null; return field_of(q) + add(1, 2);", "Cannot access 'v' on null ('p' is null)"},
			});
		});

		// operator++(int)'s parameter is C++'s unnamed marker: a text and a listing give it no name, and no listing shows a builtin only a desugaring calls
		test("an_unspellable_name_reaches_no_text", [this]() {
			const std::string tally = "class Tally { int n = 0; Tally operator++(int) { Tally old = this; n += 1; return old; } Tally operator--(int) { return this; } }\nTally t;\n";
			const std::vector<std::pair<std::string, std::string>> cases{
				{tally + "reflect::invoke(t, \"++\", \"text\");", "Cannot pass string to an int parameter"},
				{tally + "reflect::invoke(t, \"--\", null);", "Cannot pass null to an int parameter"},
				{tally + "var bound = reflect::bind(t, \"++\"); bound(2.5);", "Cannot pass float to an int parameter"},
				{tally + "t++; t++; to_string(t.n) + \"|\" + to_string(reflect::invoke(t, \"++\", 0).n) + \"|\" + to_string(t.n)", "= 2|2|3"},
				{tally + "var marker = reflect::method_arguments(t, \"++\")[0][0]; marker[\"name\"] + \"|\" + marker[\"type\"]", "= |int"},
				{tally + "var marker = reflect::methods(t)[0][\"params\"][0]; marker[\"name\"] + \"|\" + marker[\"type\"]", "= |int"},
				{"int hidden = 0; for (auto name : reflect::functions()) { if (name.contains(\"#\") || name.contains(\"__jai\")) { hidden += 1; } } hidden", "= 0"},
			};
			std::string failures;
			for (const auto& [source, expected] : cases) {
				for (const backend which : {backend::interpreter, backend::vm, backend::vm_reference}) {
					const std::string got = first_line(which, source);
					if (got != expected) { failures += source + "\n  expected: " + expected + "\n  got:      " + got + "\n"; }
				}
			}
			check_true(failures.empty(), failures);
		});

		// the kernels that name an operand: a compiler's own value (its name starts with \x01) has no name, and a stand-in reads what it replaces
		test("a_compiler_value_names_nothing", [this]() {
			auto e = jai::foundry::make_default_engine();
			identifier_expr temp(source_location{}, std::string_view("\x01inl"), detail::engine_access::symbolize(*e, "\x01inl"));
			check_eq(uint64_t{0}, callee_name_symbol(&temp));
			check_eq(uint64_t{0}, null_member_receiver_symbol(&temp));
			check_eq(std::string(), std::string(null_member_receiver_name(&temp)));
			check_eq(std::string("Cannot access '{0}' on null"), std::string(null_member_message(&temp)));
			check_eq(std::string("..."), detail::target_spelling(&temp));
			identifier_expr held(source_location{}, std::string_view("held"), detail::engine_access::symbolize(*e, "held"));
			temp.stands_for = &held;
			check_eq(held.symbol_id, callee_name_symbol(&temp));
			check_eq(held.symbol_id, null_member_receiver_symbol(&temp));
			check_eq(std::string("held"), std::string(null_member_receiver_name(&temp)));
			check_eq(std::string("Cannot call '{0}' on null ('{1}' is null)"), std::string(null_member_message(&temp, true)));
			check_eq(std::string("held"), detail::target_spelling(&temp));
			check_eq(std::string("Cannot assign to 'v' on null ('held' is null)"), null_member_store_text(&temp, "v"));
			check_true(detail::unspellable_name(detail::k_postfix_marker_name) && !detail::unspellable_name("held") && !detail::unspellable_name(""), "the marker's name is unspellable");
		});

		test("a_receiver_is_named_as_written", [this]() {
			check_cases(prelude(), {
				{"return none(1).size();", "Cannot call 'size' on null ('none(...)' returned null)"},
				{"return none(add(1, 2)).field;", "Cannot access 'field' on null ('none(...)' returned null)"},
				{"var q = null; return q.get() + add(1, 2);", "Cannot call 'get' on null ('q' is null)"},
				{"return gnull.v + add(1, 2);", "Cannot access 'v' on null ('gnull' is null)"},
				{"gnull.v = add(1, 2); return 1;", "Cannot assign to 'v' on null ('gnull' is null)"},
				{"Thing t; return t.w.v + add(add(1, 2), 3);", "Cannot access 'v' on null ('w' is null)"},
				{"return next(1).field;", "int has no member 'field'"},
				{"return next(add(1, 2)).size();", "int has no member 'size'"},
			});
		});
	}
};

} // namespace jai::foundry::tests

using operand_name_text_tests = jai::foundry::tests::operand_name_text_tests;
FOUNDRY_REGISTER(operand_name_text_tests)
