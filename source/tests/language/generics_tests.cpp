// Generics (docs/site/guide/21-generics.html): a class, function or method with type parameters is instantiated at load for
// each list of type arguments the program uses, and each instance is checked and run as if it had been written by hand
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/debug/controller.hpp>
#include <jaiscript/detail/engine_access.hpp>
#include <jaiscript/detail/type_name_key.hpp>
#include <jaiscript/core/class_registry.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include "../case_table.hpp"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iterator>
#include <mutex>
#include <thread>
#include <fstream>
#include <string>

namespace jai::foundry::tests {

struct generic_bound_host_base {
	virtual ~generic_bound_host_base() = default;
	int64_t n = 0;
};
struct generic_bound_host : generic_bound_host_base {};

class generics_tests : public suite {
public:
	generics_tests() : suite("Generics") {}

	static std::string failure(engine& eng, const std::string& code) {
		try {
			eng.execute(code);
		} catch (const std::exception& e) {
			return e.what();
		}
		return "(no error)";
	}

	static bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
	// the warnings of the engine's last check, as their texts
	static std::vector<std::string> warnings(engine& eng) { return diagnostics(eng, check_diagnostic::level::warning); }
	// and its errors
	static std::vector<std::string> errors(engine& eng) { return diagnostics(eng, check_diagnostic::level::error); }
	static std::vector<std::string> diagnostics(engine& eng, check_diagnostic::level severity) {
		std::vector<std::string> texts;
		for (const auto& d : eng.last_check_diagnostics().diagnostics) {
			if (d.severity == severity) { texts.push_back(d.message); }
		}
		return texts;
	}

	// a directory of this suite's own, emptied: the units a test writes
	static std::filesystem::path unit_directory(const std::string& test) {
		const std::filesystem::path directory = test_temp_directory() / ("generics_" + test);
		std::error_code ignored;
		std::filesystem::remove_all(directory, ignored);
		std::filesystem::create_directories(directory);
		return directory;
	}
	static std::string write_unit(const std::filesystem::path& directory, const std::string& name, const std::string& text) {
		const std::filesystem::path path = directory / name;
		std::ofstream(path, std::ios::binary) << text;
		return path.string();
	}
	static std::shared_ptr<engine> stdlib_engine() {
		auto e = make_engine();
		stdlib::register_all(*e);
		return e;
	}

	// the surface the design's samples use: the generics of docs/site/guide/21-generics.html
	static const char* declarations() {
		return R"(
			class Stack<T> {
				array<T> items;
				void push(T item) { items.push(item); }
				T pop() { T last = items[items.size() - 1]; items.pop(); return last; }
				int size() { return items.size(); }
			}
			class Pair<K, V> {
				K first;
				V second;
				Pair(K k, V v) { first = k; second = v; }
			}
			class Entity { int id = 0; }
			class Goblin : Entity { int gold = 3; }
			class Orc : Entity { int rage = 9; }
			class Pool<T : Entity> {
				array<shared_ptr<T>> live;
				static int made;
			}
			T largest<T>(array<T> xs) { T best = xs[0]; for (auto x : xs) { if (x > best) { best = x; } } return best; }
			function smallest<T>(array<T> xs) -> T { T best = xs[0]; for (auto x : xs) { if (x < best) { best = x; } } return best; }
			coroutine T each<T>(array<T> xs) { for (auto x : xs) { yield x; } }
			namespace wire { T decode<T>(string text) { return from_json<T>(text); } }
			class CartForm { string sku; int count = 0; }
			class Request {
				string body;
				T json<T>() { return from_json<T>(body); }
				static T make<T>() { return T(); }
			}
		)";
	}

	void forge_tests() override {
		test("a_generic_declaration_parses_and_runs_nothing", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			check_eq((int64_t)7, e->execute(std::string(declarations()) + " 7;").as_int());
		});

		test("a_generic_never_instantiated_still_fails_the_load_on_a_syntax_error", [this]() {
			auto e = make_engine();
			check_true(contains(failure(*e, "class Broken<T> { int x = ; } 1;"), "expected expression"));
			check_true(contains(failure(*e, "T broken<T>(T x) { return x +; } 1;"), "expected expression"));
			check_true(contains(failure(*e, "class Host { T broken<T>() { return ; ; ] } } 1;"), "error"));
			check_eq((int64_t)2, e->execute("1 + 1").as_int());
		});

		test("the_type_parameter_list_reports_what_this_version_leaves_out", [this]() {
			auto e = make_engine();
			check_true(contains(failure(*e, "class A<T, T> {}"), "duplicate type parameter 'T' in 'A'"));
			check_true(contains(failure(*e, "class A<> {}"), "generic 'A' lists no type parameter: name at least one (A<T>)"));
			check_true(contains(failure(*e, "class A<int N> {}"), "'A' declares a non-type parameter ('int N'): a generic's parameters are types (A<T>)"));
			check_true(contains(failure(*e, "class A<Size N> {}"), "'A' declares a non-type parameter ('Size N'): a generic's parameters are types (A<T>)"));
			check_true(contains(failure(*e, "class A<T = int> {}"), "default type arguments are not supported: declare 'T' alone and name the argument at each use"));
			check_true(contains(failure(*e, "class A<T...> {}"), "variadic type parameters are not supported: list each parameter of 'A'"));
			check_true(contains(failure(*e, "class A<int> {}"), "'A' names a type where a type parameter belongs: specialization is not supported (declare A<T> once, for every T)"));
			check_true(contains(failure(*e, "class A<typename T> {}"), "a type parameter is its name alone: A<T>, not <typename T>"));
			check_true(contains(failure(*e, "class A<class T> {}"), "a type parameter is its name alone: A<T>, not <class T>"));
			check_true(contains(failure(*e, "class A<A> {}"), "'A' is the generic's own name: its type parameter needs another"));
			check_true(contains(failure(*e, "class A<T : > {}"), "expected a class after ':': 'T : Base' bounds T by a class"));
			check_true(contains(failure(*e, "void f<T>(T x) {} void g() { T h<T>(T x) { return x; } }"),
				"generic 'h' is declared inside a function body: a generic is declared at program scope, in a namespace or in a class"));
			check_true(contains(failure(*e, "auto f = []<T>(T x) { return x; };"),
				"a lambda takes no type parameters: give it auto parameters ([](auto x) { ... }), which take any type"));
			check_true(contains(failure(*e, "T f<T>(T x) { return x; } function g = f<int>;"),
				"'f<int>' is an instance of the generic function 'f', which is not a value: call it, or pass a lambda that does ((x) => f<int>(x))"));
			check_true(contains(failure(*e, "class B { override T m<T>() { return T(); } }"),
				"generic method 'm' cannot be marked override: a generic method is not virtual"));
		});

		test("a_generic_method_cannot_declare_its_classs_type_parameter_again", [this]() {
			auto e = make_engine();
			const std::string text = "'T' is a type parameter of 'Box': the generic method 'get' cannot declare it again; give its parameter another name (get<U>)";
			const std::string unused = failure(*e, "class Box<T> { T v; T get<T>(T x) { return x; } }\n1;");
			check_true(contains(unused, "<script>:1:27: error: " + text), unused);
			check_true(contains(failure(*e, "class Box<T> { T v; T get<T>(T x) { return x; } }\nBox<int> b; b.get(1);"), text));
			check_true(contains(failure(*e, "class Item { int n = 1; } class Box<T> { T v; T get<T>(T x) { return x; } }\nBox<Item> b;"), text));
			check_true(contains(failure(*e, "class Box<T> { static T make<T>() { return T(); } }"),
				"'T' is a type parameter of 'Box': the generic method 'make' cannot declare it again; give its parameter another name (make<U>)"));
			check_true(contains(failure(*e, "namespace n { class Box<K, T> { T v; U get<U, T>(U x) { return x; } } }"), text));
			// its own name for a parameter, and a lambda that types a parameter by the class's, keep loading
			check_eq((int64_t)5, e->execute("class Box<T> { T v; U get<U>(U x) { return x; } int f() { auto l = (T x) => 1; return l(v); } }\nBox<int> b; b.get(4) + b.f();").as_int());
		});

		test("a_generic_classs_instance_is_a_value_wherever_a_class_name_is", [this]() {
			auto e = stdlib_engine();
			e->execute(R"(
				class Base<T> { T v; string who() { return "base"; } }
				class More<T> : Base<T> { int extra = 3; string who() override { return "more"; } }
				namespace zoo { class Wild<T> : Base<T> { string who() override { return "wild"; } } }
			)");
			check_eq(std::string("more 4 3 More<int>"), e->execute(R"(
				auto raw = new Base<int>();
				raw.v = 4;
				extend(raw, More<int>);
				raw.who() + " " + to_string(raw.v) + " " + to_string(raw.extra) + " " + reflect::type_of(raw);
			)").as_string());
			check_eq(std::string("wild zoo::Wild<string>"), e->execute("auto w = new Base<string>(); extend(w, zoo::Wild<string>); w.who() + \" \" + reflect::type_of(w);").as_string());
			check_eq(std::string("Base<float> 0.0"), e->execute("auto make = Base<float>; auto made = make(); reflect::type_of(made) + \" \" + to_string(made.v);").as_string());
			check_true(contains(failure(*e, "auto raw = new Base<int>(); extend(raw, More);"), "'More' is a generic class: name its type arguments (More<T>)"));
		});

		test("a_generic_function_or_method_where_a_value_stands_says_it_is_none", [this]() {
			auto e = make_engine();
			e->execute(R"(
				class Plain { T conv<T>(T x) { return x; } static T mk<T>() { return T(); } }
				namespace ns { T id<T>(T x) { return x; } }
				T id<T>(T x) { return x; }
				Plain p;
			)");
			const auto said = [&](const std::string& code, const std::string& text) {
				const std::string got = failure(*e, code);
				check_true(contains(got, text), code + " -> " + got);
			};
			const std::string lambda = ", which is not a value: call it, or pass a lambda that does ((x) => ";
			said("auto f = id;", "'id' is a generic function" + lambda + "id<T>(x))");
			said("function<int(int)> f = id;", "'id' is a generic function" + lambda + "id<T>(x))");
			said("print(id);", "'id' is a generic function" + lambda + "id<T>(x))");
			said("auto f = id<int>;", "'id<int>' is an instance of the generic function 'id'" + lambda + "id<int>(x))");
			said("var v = [id<int>];", "'id<int>' is an instance of the generic function 'id'" + lambda + "id<int>(x))");
			said("auto f = ns::id;", "'ns::id' is a generic function" + lambda + "ns::id<T>(x))");
			said("auto f = ns::id<int>;", "'ns::id<int>' is an instance of the generic function 'ns::id'" + lambda + "ns::id<int>(x))");
			said("namespace ns { auto f = id; }", "'id' is a generic function" + lambda + "id<T>(x))");
			said("auto f = Plain::mk;", "'Plain::mk' is a generic method" + lambda + "Plain::mk<T>(x))");
			said("auto f = Plain::mk<int>;", "'Plain::mk<int>' is an instance of the generic method 'Plain::mk'" + lambda + "Plain::mk<int>(x))");
			said("auto f = p.conv;", "'conv' is a generic method, which is not a value: call it, or bind an instance with reflect::bind(p, \"conv<T>\")");
			said("auto f = p.conv<int>;", "'conv<int>' is an instance of the generic method 'conv', which is not a value: call it, or bind it with reflect::bind(p, \"conv<int>\")");
			said("class In { T twice<T>(T x) { return x + x; } int use() { auto f = twice; return 1; } }",
			     "'twice' is a generic method, which is not a value: call it, or bind an instance with reflect::bind(this, \"twice<T>\")");
			// the lambdas the texts advise load and run
			check_eq((int64_t)12, e->execute("auto f = (x) => id<int>(x); auto g = (x) => ns::id<int>(x); f(5) + g(7);").as_int());
		});

		test("a_type_argument_that_is_no_type_says_what_it_is", [this]() {
			auto e = make_engine();
			e->execute("class Box<T> { T v; } namespace ns { class Bag<T> { T v; } } 1;");
			const std::string reference = "a type argument names a type: 'int&' is a reference, not one an instance can be made with; a generic writes T& in its own parameter list";
			const std::string constant = "a type argument names a type: 'const int' is not one an instance can be made with; const goes on the variable, never in a type argument";
			const std::string value = "a type argument names a type: a value is not one an instance can be made with; 'Box' takes types (Box<T>)";
			const std::pair<std::string, std::string> cases[] = {
				{"Box<int&> a;", reference}, {"void f(Box<int&> b) {}", reference}, {"array<Box<int&>> xs;", reference},
				{"Box<const int> a;", constant}, {"shared_ptr<Box<const int>> p;", constant},
				{"Box<3> a;", value}, {"Box<\"s\"> a;", value},
				{"ns::Bag<3> a;", "a type argument names a type: a value is not one an instance can be made with; 'ns::Bag' takes types (ns::Bag<T>)"}};
			for (const auto& [code, text] : cases) {
				const std::string got = failure(*e, code);
				check_true(contains(got, text) && !contains(got, "undeclared"), code + " -> " + got);
			}
			check_true(contains(failure(*e, "Box<int&> a;"), "<script>:1:8: error: " + reference));   // at the '&', and const at its own word
			check_true(contains(failure(*e, "Box<const int> a;"), "<script>:1:5: error: " + constant));
			check_true(contains(failure(*e, "Box<3> a;"), "<script>:1:5: error: " + value));
			// '<' and '&' that no type argument list closes still compare
			check_true(e->execute("int a = 1; int b = 3; bool c = a < (b & 2); c && a < b;").as_bool());
		});

		test("a_constructor_and_an_operator_take_no_type_parameters", [this]() {
			auto e = make_engine();
			const std::pair<std::string, std::string> cases[] = {
				{"class Box { int v; Box<T>(T x) { v = 1; } }", "<script>:1:23: error: a constructor takes no type parameters: give the class them (class Box<T>), or declare a constructor for each type it takes"},
				{"class Pair<K, V> { K k; Pair<K, V>(K a) { k = a; } }", "<script>:1:29: error: a constructor is named by its class's bare name: write Pair(...); inside the class, Pair is Pair<K, V>"},
				{"class Box { int v; Box operator+<T>(T x) { return this; } }", "<script>:1:33: error: an operator takes no type parameters: give the class them (class Box<T>), or declare operator+ for each type it takes"},
				{"class Box { int v; static Box operator*<T>(Box a, T x) { return a; } }", "an operator takes no type parameters: give the class them (class Box<T>), or declare operator* for each type it takes"},
				{"class Box { int v; T operator()<T>(T x) { return x; } }", "an operator takes no type parameters: give the class them (class Box<T>), or declare operator() for each type it takes"},
				{"class Box { int v; int operator[]<T>(T x) { return 1; } }", "an operator takes no type parameters: give the class them (class Box<T>), or declare operator[] for each type it takes"}};
			for (const auto& [code, text] : cases) {
				const std::string got = failure(*e, code);
				check_true(contains(got, text), code + " -> " + got);
			}
			// a constructor and an operator of a generic class are written as a plain class's, and an operator< is no type parameter list
			check_eq((int64_t)7, e->execute("class Pair<K, V> { K k; Pair(K a) { k = a; } bool operator<(Pair<K, V> o) { return k < o.k; } } Pair<int, int> p = Pair<int, int>(7); Pair<int, int> q = Pair<int, int>(9); p < q ? p.k : 0;").as_int());
		});

		test("a_generic_class_is_not_overloaded_by_its_parameter_count", [this]() {
			auto e = make_engine();
			const std::string text = "generic class 'Dup' is declared with 1 type parameter on line 1 and with 2 type parameters here: a generic class cannot be overloaded by its parameter count";
			const std::string unused = failure(*e, "class Dup<T> { T a; }\nclass Dup<K, V> { K a; V b; }\n1;");
			check_true(contains(unused, text), unused);
			check_true(contains(unused, ":2:"), "reported at the second declaration");
			check_true(contains(failure(*e, "class Dup<T> { T a; }\nclass Dup<K, V> { K a; V b; }\nDup<int> x; Dup<int, int> y;"), text));
			check_true(contains(failure(*e, "namespace n { class Dup<A, B> { A a; }\nclass Dup<T> { T a; } }"),
				"generic class 'n::Dup' is declared with 2 type parameters on line 1 and with 1 type parameter here: a generic class cannot be overloaded by its parameter count"));
			// the same count is the declared-again error it was, and functions still overload by their parameters
			check_true(contains(failure(*e, "class Dup<T> { T a; }\nclass Dup<U> { U a; }"), "generic 'Dup<U>' is declared again: line 1 declares it with the same parameters"));
			check_eq(std::string("1|2"), e->execute("string count<T>(T a) { return \"1\"; } string count<A, B>(A a, B b) { return \"2\"; } count(1) + \"|\" + count(1, 2);").as_string());
		});

		test("a_generic_declared_again_is_one_error_and_its_instances_none", [this]() {
			const auto errors_of = [this](const std::string& code) {
				auto e = stdlib_engine();
				(void)failure(*e, code);
				return errors(*e);
			};
			const std::string twice = "string show<T>(T x) { return \"a\"; }\nstring show<U>(U x) { return \"b\"; }\n";
			const std::string again = "generic 'show<U>' is declared again: line 1 declares it with the same parameters";
			// the instance the second declaration makes is that declaration's mistake, not a definition of 'show<int>' of its own
			check_eq(std::vector<std::string>{again}, errors_of(twice + "show(1);\n"));
			check_eq(std::vector<std::string>{again}, errors_of(twice + "show<int>(1) + show(\"s\");\n"));
			check_eq(std::vector<std::string>{"generic 'n::show<U>' is declared again: line 2 declares it with the same parameters"},
				errors_of("namespace n {\n" + twice + "}\nn::show(1);\n"));
			auto lax = stdlib_engine();
			(void)failure(*lax, "[[warn_mode]];\n" + twice + "show(1);\n");
			check_eq(std::vector<std::string>{"generic 'show<U>' is declared again: line 2 declares it with the same parameters"}, errors(*lax));
			check_true(warnings(*lax).empty(), warnings(*lax).empty() ? std::string() : warnings(*lax).front());
			// a class's, a namespace class's and a method's were one error already
			check_eq(std::vector<std::string>{"generic 'Box<U>' is declared again: line 1 declares it with the same parameters"},
				errors_of("class Box<T> { T v; }\nclass Box<U> { U v; }\nBox<int> b;\nb.v;\n"));
			check_eq(std::vector<std::string>{"generic 'n::Box<U>' is declared again: line 1 declares it with the same parameters"},
				errors_of("namespace n { class Box<T> { T v; }\nclass Box<U> { U v; } }\nn::Box<int> b;\nb.v;\n"));
			check_eq(std::vector<std::string>{"generic 'A::show<U>' is declared again: line 1 declares it with the same parameters"},
				errors_of("class A { " + twice + "}\nA a;\na.show(1);\n"));
			check_eq(std::vector<std::string>{"generic 'A::show<U>' is declared again: line 1 declares it with the same parameters"},
				errors_of("class A { static string show<T>(T x) { return \"a\"; }\nstatic string show<U>(U x) { return \"b\"; }\n}\nA::show(1);\n"));
			// two generics that make one instance of one signature are the redefinition they are, beside the one declared again
			const std::string by_int = "string show<V>(int x) { return \"c\"; }\n";
			check_eq(std::vector<std::string>{"'show<int>' is already defined"}, errors_of("string show<T>(T x) { return \"a\"; }\n" + by_int + "show<int>(1);\n"));
			check_eq(std::vector<std::string>{"'show<int>' is already defined with parameters (int)"},
				errors_of("namespace n {\nstring show<T>(T x) { return \"a\"; }\n" + by_int + "}\nn::show<int>(1);\n"));
			check_eq(std::vector<std::string>{again, "'show<int>' is already defined"}, errors_of(twice + by_int + "show<int>(1);\n"));
			// and two generic methods, as two methods written by hand are
			check_eq(std::vector<std::string>{"'A::show<int>' is already defined with parameters (int)"},
				errors_of("class A {\nstring show<T>(T x) { return \"a\"; }\n" + by_int + "}\nA a;\na.show<int>(1);\n"));
			check_eq(std::vector<std::string>{"'A::show<int>' is already defined with parameters (int)"},
				errors_of("class A {\nstatic string show<T>(T x) { return \"a\"; }\nstatic " + by_int + "}\nA::show<int>(1);\n"));
			check_eq(std::vector<std::string>{"'K<int>::show<int>' is already defined with parameters (int)"},
				errors_of("class K<W> {\nstring show<T>(T x) { return \"a\"; }\n" + by_int + "}\nK<int> k;\nk.show<int>(1);\n"));
			check_eq(std::vector<std::string>{"generic 'A::show<U>' is declared again: line 1 declares it with the same parameters", "'A::show<int>' is already defined with parameters (int)"},
				errors_of("class A { " + twice + by_int + "}\nA a;\na.show<int>(1);\n"));
			check_eq(std::string("a|c"), stdlib_engine()->execute("class A {\nstring show<T>(T x) { return \"a\"; }\n" + by_int + "}\nA a;\na.show<string>(\"s\") + \"|\" + a.show<bool>(1);\n").as_string(),
				"other type arguments make two instances of other signatures");
		});

		test("template_typename_is_a_foreign_spelling", [this]() {
			auto e = make_engine();
			const std::string text = "JaiScript has no 'template<...>' prefix: a generic lists its type parameters after its name, "
				"class Stack<T> { ... } or T largest<T>(array<T> xs) { ... }";
			check_true(contains(failure(*e, "template<typename T> class A {}"), text));
			check_true(contains(failure(*e, "template<typename T> T id(T x) { return x; }"), text));
			check_true(contains(failure(*e, "class A { template<typename T> T id(T x) { return x; } }"), text));
		});

		test("a_type_parameters_name_is_reserved_in_its_generic", [this]() {
			auto e = make_engine();
			const std::string text = "'T' is a type parameter of 'Stack': a declaration cannot reuse its name";
			check_true(contains(failure(*e, "class Stack<T> { int T; }"), text));
			check_true(contains(failure(*e, "class Stack<T> { void m() { int T = 1; } }"), text));
			check_true(contains(failure(*e, "class Stack<T> { void m(int T) {} }"), text));
			check_true(contains(failure(*e, "class Stack<T> { void T() {} }"), text));
			check_true(contains(failure(*e, "class Stack<T> { void m() { for (auto T : [1, 2]) {} } }"), text));
			check_true(contains(failure(*e, "void f<T>(array<int> T) {}"), "'T' is a type parameter of 'f': a declaration cannot reuse its name"));
			// a member read, a comparison against a construction and an instance test are no declaration
			check_eq((int64_t)1, e->execute("class Holder<T> { var held; bool m(T x) { held.T = 1; return x > T(0) || dynamic_cast<shared_ptr<T>>(x) != null; } } 1;").as_int());
		});

		// ---- classes and explicit calls

		test("a_generic_class_is_a_class_per_argument_list", [this]() {
			auto e = stdlib_engine();
			e->execute(declarations());
			check_eq((int64_t)10, e->execute("Stack<int> s; s.push(3); s.push(9); s.pop() + s.size();").as_int());
			check_eq(std::string("hp10"), e->execute("auto p = Pair<string, int>(\"hp\", 10); p.first + to_string(p.second);").as_string());
			check_eq(std::string("Stack<int>|Pair<string, int>|Stack<array<string>>"),
				e->execute("Stack<int> s; Pair<string, int> p(\"a\", 1); Stack<array<string>> n; reflect::type_of(s) + \"|\" + reflect::type_of(p) + \"|\" + reflect::type_of(n);").as_string());
			check_eq(std::string("b"), e->execute("Stack<string> names; names.push(\"a\"); names.push(\"b\"); names.pop();").as_string());
		});

		test("an_instance_constructs_by_every_spelling_a_class_does", [this]() {
			auto e = stdlib_engine();
			e->execute("class W<T> { T v; W(T x) { v = x; } T get() { return v; } }");
			check_eq((int64_t)1, e->execute("auto w = W<int>(1); w.get();").as_int());
			check_eq(std::string("q"), e->execute("auto w = W<string>{\"q\"}; w.get();").as_string());
			check_eq((int64_t)5, e->execute("W<int> w(5); w.get();").as_int());
			check_eq((int64_t)6, e->execute("W<int> w{6}; w.get();").as_int());
			check_eq((int64_t)7, e->execute("auto w = new W<int>(7); w.get();").as_int());
			check_eq((int64_t)8, e->execute("auto w = make_shared<W<int>>(8); w.get();").as_int());
			check_eq(std::string("W<int>"), e->execute("shared_ptr<W<int>> w = new W<int>(7); reflect::type_of(w);").as_string());
		});

		test("inside_an_instance_the_parameter_reads_as_its_argument", [this]() {
			auto e = stdlib_engine();
			e->execute(R"(
				class P { int n = 7; }
				class Maker<T> {
					static T make() { return T(); }
					T fresh() { T x; return x; }
					shared_ptr<T> boxed() { return new T(); }
					T from(var v) { return T(v); }
					T as(var v) { return cast<T>(v); }
					array<T> some() { array<T> xs; xs.push(T()); return xs; }
				}
				class Test<T> { bool is(var v) { return dynamic_cast<T>(v) != null; } T& same(T& v) { return dynamic_cast<T&>(v); } }
			)");
			check_eq((int64_t)0, e->execute("Maker<int>::make();").as_int());
			check_eq(std::string("!"), e->execute("Maker<string>::make() + \"!\";").as_string());
			check_eq((int64_t)7, e->execute("Maker<P>::make().n;").as_int());
			check_eq((int64_t)14, e->execute("Maker<P> m; m.fresh().n + m.boxed().n;").as_int());
			check_eq(std::string("P"), e->execute("Maker<P> m; reflect::type_of(m.boxed());").as_string());
			// T(v) is a class's construction and a primitive's conversion; T x; is each one's default
			check_eq((int64_t)2, e->execute("Maker<int> m; m.from(2.9) + m.fresh();").as_int());
			check_true(e->execute("Test<P> t; t.is(P()) && t.is(new P()) && !t.is(4);").as_bool());
			// what the argument makes of the text is the argument's own rule: a dynamic cast names a class
			const std::string builtin = failure(*e, "Test<int> t;");
			check_true(contains(builtin, "dynamic_cast takes a class: dynamic_cast<T>(e) is e as a shared_ptr<T>, null when it is no T, and dynamic_cast<T&>(e) is the reference") && contains(builtin, "in Test<int>, instantiated at <script>:1:5"));
			check_false(contains(builtin, "undeclared"));
			check_eq((int64_t)5, e->execute("Maker<int> m; var v = 4; m.as(v) + 1;").as_int());
			check_eq(std::string("1:float"), e->execute("Maker<float> m; auto xs = m.some(); to_string(xs.size()) + \":\" + reflect::type_of(xs[0]);").as_string());
		});

		test("a_primitive_constructs_by_its_own_name", [this]() {
			auto e = stdlib_engine();
			check_eq((int64_t)2, e->execute("int(2.9);").as_int());
			check_eq((int64_t)0, e->execute("int() + int{};").as_int());
			check_eq(std::string(""), e->execute("string();").as_string());
			check_false(e->execute("bool();").as_bool());
			check_eq(std::string("float"), e->execute("reflect::type_of(float(3));").as_string());
			check_true(contains(failure(*e, "int(1, 2);"), "a 'int' is initialized by one value, not 2"));
		});

		test("statics_are_per_instance", [this]() {
			auto e = stdlib_engine();
			e->execute(declarations());
			check_eq(std::string("2,1"), e->execute(R"(
				class Counter<T : Entity> { static int made = 0; void add() { made += 1; } }
				Counter<Goblin> a; Counter<Orc> b; a.add(); a.add(); b.add();
				to_string(Counter<Goblin>::made) + "," + to_string(Counter<Orc>::made);
			)").as_string());
			check_eq((int64_t)4, e->execute("Pool<Goblin>::made = 4; int made = Pool<Goblin>::made; made;").as_int());
			check_eq((int64_t)0, e->execute("Pool<Orc>::made;").as_int());
		});

		test("value_semantics_follow_the_argument", [this]() {
			auto e = stdlib_engine();
			e->execute("class Item { int n = 0; } class Cell<T> { T held; }");
			// a class argument copies with its cell, a shared_ptr argument shares
			check_eq(std::string("1,2"), e->execute("Cell<Item> a; a.held.n = 1; Cell<Item> b = a; b.held.n = 2; to_string(a.held.n) + \",\" + to_string(b.held.n);").as_string());
			check_eq(std::string("2,2"), e->execute(
				"Cell<shared_ptr<Item>> a; a.held = new Item(); a.held.n = 1; Cell<shared_ptr<Item>> b = a; b.held.n = 2; to_string(a.held.n) + \",\" + to_string(b.held.n);").as_string());
		});

		test("every_kind_of_type_is_an_argument", [this]() {
			auto e = stdlib_engine();
			e->execute("class Cell<T> { T v; }");
			check_eq(std::string("Cell<Color>:1"), e->execute("enum Color { red, green } Cell<Color> c; c.v = Color::green; reflect::type_of(c) + \":\" + to_string(c.v);").as_string());
			check_near(3.0, e->execute("Cell<math::vec2> c; c.v = math::vec2(1, 2); c.v.x + c.v.y;").as_float(), 1e-9);
			check_eq((int64_t)4, e->execute("Cell<map<string, int>> m; m.v[\"a\"] = 4; m.v[\"a\"];").as_int());
			check_eq((int64_t)8, e->execute("Cell<function> f; f.v = (x) => x * 2; f.v(4);").as_int());
			check_eq(std::string("Cell<var>"), e->execute("Cell<var> a; a.v = \"s\"; reflect::type_of(a);").as_string());
			check_eq(std::string("Cell<int>"), e->execute("Cell<Cell<int>> o; o.v.v = 3; reflect::type_of(o.v);").as_string());
			check_eq(std::string("Cell<weak_ptr<Cell<int>>>"), e->execute("Cell<weak_ptr<Cell<int>>> w; reflect::type_of(w);").as_string());
			check_true(contains(failure(*e, "Cell<auto> c;"), "a type argument names a type: 'auto' is not one an instance can be made with"));
			check_true(contains(failure(*e, "Cell<void> c;"), "a type argument names a type: 'void' is not one an instance can be made with"));
		});

		test("a_generic_names_itself_and_other_instances", [this]() {
			auto e = stdlib_engine();
			check_eq(std::string("6Box<int>"), e->execute(R"(
				class Box<T> { T item; Box(T x) { item = x; } Box<T> same() { return Box(item); } Box other() { return Box<T>(item); } }
				auto b = Box<int>(3);
				to_string(b.same().item + b.other().item) + reflect::type_of(b.same());
			)").as_string());
			check_eq(std::string("4Node<int>"), e->execute(R"(
				class Node<T> { T value; shared_ptr<Node<T>> next; }
				auto n = new Node<int>(); n.value = 4; n.next = new Node<int>();
				to_string(n.value) + reflect::type_of(n.next);
			)").as_string());
			check_eq(std::string("8true9"), e->execute(R"(
				class Base<T> { T held; Base(T x) { held = x; } }
				class Derived<T> : Base<T> { Derived(T x) : Base<T>(x) {} T twice() { return held + held; } }
				class Ints : Base<int> { Ints() : Base<int>(9) {} }
				Derived<int> d(4); Ints i;
				to_string(d.twice()) + to_string(reflect::is_instance_of(d, "Base<int>")) + to_string(i.held);
			)").as_string());
		});

		test("an_instances_name_is_its_canonical_spelling_across_namespaces", [this]() {
			auto e = stdlib_engine();
			check_eq(std::string("shop::Stack<shop::Product>|1|shop::Stack<shop::Product>|true"), e->execute(R"(
				namespace shop {
					class Product { string title = "tee"; }
					class Stack<T> { array<T> items; void push(T x) { items.push(x); } int size() { return items.size(); } }
					Stack<Product> inside;
				}
				shop::Stack<shop::Product> outside;
				outside.push(shop::Product());
				shop::inside = outside;
				reflect::type_of(outside) + "|" + to_string(shop::inside.size()) + "|" + reflect::type_of(shop::inside) + "|" + to_string(reflect::type_name(outside) == reflect::type_of(shop::inside));
			)").as_string());
		});

		test("a_generic_classs_bare_name_reaches_it_from_its_namespace_reopened_as_a_classs_does", [this]() {
			// a later unit reopens the namespace: the class and the generic class the earlier unit declared answer by their bare names
			auto e = stdlib_engine();
			e->execute("namespace shop {\n\tclass Item { int n = 1; }\n\tclass Shelf<T> { T item; int size() { return 2; } }\n}\n");
			check_eq((int64_t)1, e->execute("namespace shop { int plain() { Item i; return i.n; } }\nshop::plain();").as_int());
			check_eq(std::string("2|2|2|shop::Shelf<shop::Item>"), e->execute(
				"namespace shop {\n\tclass Rack : Shelf<int> { }\n"
				"\tstring probe() { Shelf<int> typed; auto made = Shelf<string>(); Rack rack; Shelf<Item> items;\n"
				"\t\treturn to_string(typed.size()) + \"|\" + to_string(made.size()) + \"|\" + to_string(rack.size()) + \"|\" + reflect::type_of(items); }\n}\nshop::probe();").as_string());
			// nested namespaces: the inner one's own and the enclosing one's
			auto nested = stdlib_engine();
			nested->execute("namespace a {\n\tclass Outer { int n = 1; }\n\tclass Gen<T> { int size() { return 2; } }\n"
				"\tnamespace b {\n\t\tclass Deep { int n = 3; }\n\t\tclass Wrap<T> { int size() { return 4; } }\n\t}\n}\n");
			check_eq((int64_t)4, nested->execute("namespace a { namespace b { int plain() { Outer o; Deep d; return o.n + d.n; } } }\na::b::plain();").as_int());
			check_eq((int64_t)6, nested->execute("namespace a { namespace b { int probe() { Gen<int> g; Wrap<int> w; return g.size() + w.size(); } } }\na::b::probe();").as_int());
			check_eq(std::string("a::Gen<a::b::Deep>"), nested->execute("namespace a::b { string spelled() { Gen<Deep> g; return reflect::type_of(g); } }\na::b::spelled();").as_string());
			// a using declaration names either
			check_eq((int64_t)3, e->execute("using shop::Item;\nusing shop::Shelf;\nItem i;\nShelf<int> s;\ni.n + s.size();").as_int());
			// from another file, once the file that declares them ran
			const auto directory = unit_directory("reopened_namespace");
			const std::string lib = write_unit(directory, "lib.jai", "namespace shop {\n\tclass Item { int n = 1; }\n\tclass Shelf<T> { T item; int size() { return 2; } }\n}\n");
			const std::string user = write_unit(directory, "user.jai", "namespace shop {\n\tint both() { Item i; Shelf<Item> s; return i.n + s.size(); }\n}\nshop::both();\n");
			auto files = stdlib_engine();
			files->execute_file(lib);
			check_eq((int64_t)3, files->execute_file(user).as_int());
			// a bite parsed where the namespace declared the generic class does not load where it does not, as with a class
			auto bite = e->jaibite("namespace shop { int held() { Shelf<int> s; return s.size(); } }\nshop::held();\n");
			check_eq((int64_t)2, bite.execute().as_int());
			auto bare = stdlib_engine();
			std::string refused = "(loaded)";
			try { bare->jaibite_load_bytes(bite.save_bytes()); } catch (const std::exception& error) { refused = error.what(); }
			check_true(contains(refused, "the bite was parsed where 'shop::Shelf' was a class or an enum (a bare 'Shelf' inside namespace 'shop' names it); here it is not: save it again from source"), refused);
		});

		test("a_generic_function_is_a_function_per_argument_list", [this]() {
			auto e = stdlib_engine();
			e->execute(declarations());
			check_eq(std::string("8,2"), e->execute("array<int> scores = [4, 8, 2]; int top = largest<int>(scores); to_string(top) + \",\" + to_string(smallest<int>(scores));").as_string());
			check_eq(std::string("pear"), e->execute("largest<string>([\"apple\", \"pear\", \"fig\"]);").as_string());
			check_eq((int64_t)6, e->execute("int sum = 0; for (auto v : each<int>([1, 2, 3])) { sum += v; } sum;").as_int());
			check_eq(std::string("abab8"), e->execute(R"(
				namespace text { T twice<T>(T x) { return x + x; } int go() { return twice<int>(4); } }
				text::twice<string>("ab") + to_string(text::go());
			)").as_string());
			check_eq(std::string("2,1"), e->execute("void swap<T>(T& a, T& b) { T t = a; a = b; b = t; } int x = 1; int y = 2; swap<int>(x, y); to_string(x) + \",\" + to_string(y);").as_string());
			check_eq((int64_t)24, e->execute("int fact<T>(T n) { return n <= 1 ? 1 : n * fact<T>(n - 1); } fact<int>(4);").as_int());
		});

		test("a_type_parameter_forwards_to_a_hosts_type_argument", [this]() {
			auto e = stdlib_engine();
			e->execute(declarations());
			check_eq(std::string("tee:2"), e->execute(R"(CartForm f = wire::decode<CartForm>("{\"sku\": \"tee\", \"count\": 2}"); f.sku + ":" + to_string(f.count);)").as_string());
		});

		test("an_instance_is_declared_the_moment_all_it_is_built_on_is", [this]() {
			// no place is computed from uses, so no shape of use can be missed: each program names the instance in an expression
			// above its first type (a construction, a call, a cast, a dynamic cast, a type argument, a lambda's or a function's body,
			// another instance's text)
			const std::string box = "class Box<T> { T v; Box() { } Box(T x) { v = x; } }\n";
			const std::pair<const char*, const char*> programs[] = {
				{"auto a = Box<int>(1);\nBox<int> b;\nto_string(a.v) + to_string(b.v);", "10"},
				{"string got = to_string(Box<int>(7).v);\narray<Box<int>> xs;\ngot + to_string(xs.size());", "70"},
				{"auto a = Box<int>(2);\nauto q = cast<Box<int>>(a);\nto_string(q.v);", "2"},
				{"auto a = Box<int>(2);\nto_string(dynamic_cast<shared_ptr<Box<int>>>(a) != null);", "true"},
				{"auto a = Box<int>(4);\nauto back = from_json<Box<int>>(to_json(a));\nto_string(back.v);", "4"},
				{"auto mk = () => Box<string>(\"s\");\nstring got = mk().v;\nmap<string, Box<string>> m;\ngot + to_string(m.size());", "s0"},
				{"int f() { auto a = Box<int>(5); return a.v; }\nstring got = to_string(f());\nBox<int> later;\ngot + to_string(later.v);", "50"},
				{"int f() { auto a = Box<int>(5); return a.v; }\nto_string(f());", "5"},
				{"class Maker<T> { int made(T x) { auto b = Box<T>(x); return b.v; } }\nMaker<int> m;\nstring got = to_string(m.made(3));\nBox<int> later;\ngot;", "3"},
			};
			for (const auto& [program, expected] : programs) {
				auto e = stdlib_engine();
				std::string got;
				try { got = e->execute(box + program).as_string(); } catch (const std::exception& error) { got = error.what(); }
				check_eq(std::string(expected), got, program);
			}
			auto e = stdlib_engine();
			// a class a type argument names, declared between the generic and the use: the instance exists once it is declared
			check_eq(std::string("falsetrue3"), e->execute(R"(
				class Shelf<T> { T v; }
				bool before = reflect::classes().contains("Shelf<Item>");
				class Item { int n = 3; }
				bool after = reflect::classes().contains("Shelf<Item>");
				Shelf<Item> b;
				to_string(before) + to_string(after) + to_string(b.v.n);
			)").as_string());
			// a function's instance a later round of the check finds an earlier call of; a method's instance likewise
			check_eq(std::string("46"), e->execute(R"(
				T first<T>(array<T> xs) { return xs[0]; }
				T wrap<T>(T x) { return x; }
				auto x = wrap(first([4, 5]));
				auto y = wrap(6);
				to_string(x) + to_string(y);
			)").as_string());
			check_eq(std::string("57"), e->execute(R"(
				class Oracle { int base = 4; T answer<T>(T x) { return x + base; } }
				Oracle o;
				auto x = wrap(o.answer(1));
				auto y = o.answer<int>(3);
				to_string(x) + to_string(y);
			)").as_string());
		});

		test("an_instances_declaration_runs_where_its_generic_and_classes_are_declared", [this]() {
			auto e = stdlib_engine();
			// its static initializers run at that moment and see what is declared above it, as a class written there would
			check_eq(std::string("70"), e->execute(R"(
				int seed() { return 5; }
				int base = 2;
				class Counter<T> { static T zero; static int made = seed() + base; T v; }
				to_string(Counter<int>::made) + to_string(Counter<int>::zero);
			)").as_string());
			check_true(failure(*e, "class Early<T> { static int made = later_value; T v; }\nint later_value = 9;\nEarly<int>::made;") != "(no error)",
				"an initializer reads nothing declared below the moment the instance is declared");
			// a class deriving from an instance; an instance deriving from a class declared further down
			check_eq(std::string("1"), e->execute("class Base<T> { T v; int tag() { return 1; } }\nclass Derived : Base<int> { }\nDerived d;\nto_string(d.tag());").as_string());
			check_eq(std::string("4"), e->execute("class Wrap<T> : Later { T v; }\nclass Later { int n = 4; }\nWrap<int> w;\nto_string(w.n);").as_string());
		});

		test("a_use_above_what_an_instance_is_built_on_is_the_order_error_of_a_plain_name", [this]() {
			auto e = stdlib_engine();
			const std::string order = ": top-level code runs in order, so declare it above the use or make the use after it";
			check_true(contains(failure(*e, "auto a = Box<int>(1);\nclass Box<T> { T v; Box(T x) { v = x; } }"), "'Box' is used before its declaration (line 2)" + order));
			check_true(contains(failure(*e, "class Crate<T> { T v; }\nauto a = Crate<Thing>();\nclass Thing { int n = 0; }"), "'Thing' is used before its declaration (line 3)" + order));
			check_true(contains(failure(*e, "int n = twice<int>(1);\nT twice<T>(T x) { return x + x; }"),
				"'twice' is called before its declaration (line 2): top-level code runs in order, so declare it above the call or make the call after it"));
			// a type above the generic is a run-time error, as a plain class's is, and says what the instance waits for
			check_true(contains(failure(*e, "Holder<int> h;\nclass Holder<T> { T held; }"), "'Holder<int>' is not declared yet: it waits for 'Holder'"));
			check_true(contains(failure(*e, "Plain p;\nclass Plain { int n = 0; }"), "No constructor found for class 'Plain'"));
		});

		test("a_functions_instance_is_declared_the_moment_its_generic_is", [this]() {
			// a call that runs above the generic's declaration fails where a plain function's does, and says what the instance waits for
			check_true(contains(failure(*stdlib_engine(), "int f() { return twice(2); }\nint r = f();\nint twice(int x) { return x + x; }"), "Undefined variable 'twice'"));
			const std::string waits = "'twice<int>' is not declared yet: it waits for 'twice'";
			check_true(contains(failure(*stdlib_engine(), "int f() { return twice<int>(2); }\nint r = f();\nT twice<T>(T x) { return x + x; }"), waits));
			check_true(contains(failure(*stdlib_engine(), "int f() { return twice(2); }\nint r = f();\nT twice<T>(T x) { return x + x; }"), waits));
			check_true(contains(failure(*stdlib_engine(), "namespace m { int f() { return twice(2); } }\nint r = m::f();\nnamespace m { T twice<T>(T x) { return x + x; } }"),
				"'m::twice<int>' is not declared yet: it waits for 'm::twice'"));
			// from its generic's declaration on it is there: a function above calls it once the declaration has run
			auto e = stdlib_engine();
			check_eq((int64_t)4, e->execute("int f() { return twice<int>(2); }\nT twice<T>(T x) { return x + x; }\nf();").as_int());
			check_eq(std::string("abab"), e->execute("namespace m { string f() { return twice(\"ab\"); } }\nnamespace m { T twice<T>(T x) { return x + x; } }\nm::f();").as_string());
			// a generic an earlier unit declared: its instance is declared as the unit that names it starts
			check_eq((int64_t)6, e->execute("int g() { return twice(3); }\ng();").as_int());
			// a generic the import in the middle of the unit brings: the instance is declared as the import runs
			const std::filesystem::path directory = unit_directory("function_link");
			write_unit(directory, "lib.jai", "T thrice<T>(T x) { return x + x + x; }\n");
			const std::string before = write_unit(directory, "before.jai", "int f() { return thrice(2); }\nint r = f();\nimport \"lib.jai\";\n");
			const std::string below = write_unit(directory, "below.jai", "int f() { return thrice(2); }\nimport \"lib.jai\";\nf();\n");
			std::string error = "(no error)";
			try { stdlib_engine()->execute_file(before); } catch (const std::exception& ex) { error = ex.what(); }
			check_true(contains(error, "'thrice<int>' is not declared yet: it waits for 'thrice'"), error);
			check_eq((int64_t)6, stdlib_engine()->execute_file(below).as_int());
			// the top-level call above the declaration is still the load error
			check_true(contains(failure(*stdlib_engine(), "int r = twice<int>(2);\nT twice<T>(T x) { return x + x; }"), "'twice' is called before its declaration (line 2)"));
		});

		test("an_instance_that_still_waits_says_what_for_when_data_or_reflection_reaches_it", [this]() {
			auto e = stdlib_engine();
			check_eq(std::string("reflect::construct: 'Rack<Part>' is not declared yet: it waits for 'Part'|'Rack<Part>' is not declared yet: it waits for 'Part'|1"), e->execute(R"(
				class Rack<T> { T v; }
				string got = "";
				try { reflect::construct("Rack<Part>"); } catch (e) { got += e; }
				got += "|";
				try { from_json("{\"$type\":\"Rack<Part>\",\"$val\":{}}"); } catch (e) { got += e; }
				class Part { int n = 1; }
				Rack<Part> rack;
				got + "|" + to_string(reflect::construct("Rack<Part>").v.n);
			)").as_string());
		});

		test("a_use_that_reaches_a_waiting_instance_says_what_it_waits_for", [this]() {
			auto e = stdlib_engine();
			const std::string waits = "'Box<Later>' is not declared yet: it waits for 'Later'";
			const std::string got = e->execute(R"(
				class Box<T> { T v; }
				void typed() { Box<Later> b; }
				void made() { auto b = Box<Later>(); }
				void shared() { auto b = new Box<Later>(); }
				void elements() { array<Box<Later>> many; many.resize(1); }
				string got = "";
				try { typed(); } catch (e) { got += e; }
				got += "|";
				try { made(); } catch (e) { got += e; }
				got += "|";
				try { shared(); } catch (e) { got += e; }
				got += "|";
				try { elements(); } catch (e) { got += e; }
				class Later { int n = 1; }
				typed(); made(); shared(); elements();
				got;
			)").as_string();
			check_eq(waits + "|" + waits + "|" + waits + "|" + waits, got);
			// a plain class not declared yet keeps the plain texts
			check_eq(std::string("No constructor found for class 'Plain'"), e->execute("string got = \"\";\nvoid typed() { Plain p; }\ntry { typed(); } catch (e) { got += e; }\nclass Plain { int n = 0; }\ngot;\n").as_string());
		});

		test("a_generic_an_import_in_the_middle_of_the_unit_brings", [this]() {
			const auto directory = unit_directory("import_in_the_middle");
			write_unit(directory, "lib.jai", "int lib_base = 3;\nclass Stack<T> {\n\tarray<T> items;\n\tstatic int made = lib_base;\n\tStack() { made += 1; }\n}\n");
			const std::string root = write_unit(directory, "root.jai", "int before = 1;\nimport \"lib.jai\";\nStack<int> s;\nto_string(before) + to_string(Stack<int>::made);\n");
			auto e = stdlib_engine();
			check_eq(std::string("14"), e->execute_file(root).as_string(), "the instance waited for the import, and its static saw the module's global");
			check_true(detail::engine_access::get_class_registry(*e).waiting_instances_.empty());
		});

		test("an_edit_to_an_argument_classs_file_reaches_the_instance", [this]() {
			const auto directory = unit_directory("argument_reload");
			const std::string item = write_unit(directory, "item.jai", "class Item {\n\tint n = 1;\n}\n");
			write_unit(directory, "user.jai", "import \"item.jai\";\nclass Box<T> {\n\tT v;\n}\nBox<Item> box;\nint user_value() { Box<Item> fresh; return fresh.v.n; }\n");
			auto e = stdlib_engine();
			e->add_include_path(directory.string());
			e->execute("import \"user.jai\";");
			check_eq((int64_t)1, e->execute("user_value();").as_int());
			auto stamp = std::filesystem::last_write_time(item);
			write_unit(directory, "item.jai", "class Item {\n\tint n = 55;\n}\n");
			std::filesystem::last_write_time(item, stamp + std::chrono::seconds(2));
			check_true(e->execute("import(\"user.jai\");").as_bool());
			check_eq((int64_t)55, e->execute("user_value();").as_int());
			check_eq(std::string("Box<Item>"), e->execute("Box<Item> again; reflect::type_of(again);").as_string());
		});

		test("errors_in_an_instance_name_the_generic_and_where_it_was_asked_for", [this]() {
			auto e = stdlib_engine();
			const std::string text = failure(*e, "class Box<T> {\n\tT item;\n\tvoid show() { print(item.nope); }\n}\nBox<int> b;\n");
			check_true(contains(text, "<script>:3:27: error: int has no member 'nope'"));
			check_true(contains(text, "    in Box<int>, instantiated at <script>:5:4"));
			// one note per level when instances nest
			const std::string nested = failure(*e, "class Inner<T> { T v; void f() { v.nope(); } }\nclass Outer<T> { Inner<T> in; }\nOuter<int> o;\n");
			check_true(contains(nested, "in Inner<int>, instantiated at <script>:2:23"));
			check_true(contains(nested, "in Outer<int>, instantiated at <script>:3:6"));
			// each instance reports what its own arguments break, and a sound instance reports nothing
			const std::string two = failure(*e, "class Named<T> { T v; string name() { return v.name; } }\nclass Dog { string name = \"rex\"; }\nNamed<Dog> d; Named<int> i; Named<float> f;\n");
			check_true(contains(two, "in Named<int>, instantiated at") && contains(two, "in Named<float>, instantiated at") && !contains(two, "in Named<Dog>"));
		});

		test("an_instance_that_cannot_be_made_says_why_at_the_use", [this]() {
			auto e = stdlib_engine();
			check_true(contains(failure(*e, "Foo<int> x;"), "<script>:1:4: error: unknown type 'Foo<int>': no generic class 'Foo' is declared"));
			check_true(contains(failure(*e, "class Foo {} Foo<int> x;"), "'Foo' takes no type arguments: it is not a generic class"));
			check_true(contains(failure(*e, "class Two<K, V> { K k; V v; } Two<int> p;"), "'Two' takes 2 type arguments, not 1"));
			check_true(contains(failure(*e, "T id<T>(T x) { return x; } id<int, string>(1);"), "'id' takes 1 type argument, not 2"));
			check_true(contains(failure(*e, "T id<T>(T x) { return x; } id<int>(1, 2);"), "'id<T>' takes 1 argument, not 2"));
			check_true(contains(failure(*e, "class Two<K, V> { K k; V v; Two(K a, V b) { k = a; v = b; } } auto p = Two(\"hp\", 10);"),
				"'Two' is a generic class: its type arguments are written, never deduced from a constructor's (Two<K, V>(...))"));
			check_true(contains(failure(*e, "int f(int x) { return x; } f<int>(1);"), "'f' takes no type arguments: it is not a generic function"));
			// a parse error only an argument brings: reported in the generic, with the instance's note
			const std::string parse = failure(*e, "class Init<T> { T made() { return T(1, 2); } }\nclass Vec { Vec(int a, int b) {} }\nInit<Vec> ok; Init<int> bad;\n");
			check_true(contains(parse, "a 'int' is initialized by one value, not 2") && contains(parse, "in Init<int>, instantiated at <script>:3:19") && !contains(parse, "Init<Vec>,"));
		});

		test("instantiation_nests_sixty_four_deep_and_no_deeper", [this]() {
			auto e = stdlib_engine();
			const std::string text = failure(*e, "class A<T> { A<array<T>> next; } A<int> a;");
			check_true(contains(text, "generic instantiation nests more than 64 deep: 'A<int>' needs 'A<array<int>>' needs 'A<array<array<int>>>', and so on without end"));
			check_true(contains(text, "more instantiations around these"));
			check_eq((int64_t)2, e->execute("1 + 1").as_int());
			check_true(contains(failure(*e, "void down<T>(T x) { array<T> xs; down<array<T>>(xs); } down<int>(1);"), "generic instantiation nests more than 64 deep"));
		});

		test("a_member_that_names_an_instance_built_from_its_own_is_refused_at_the_first_step", [this]() {
			auto e = stdlib_engine();
			const std::string text = failure(*e, "class List<T> {\n\tarray<T> items;\n\tList<List<T>> chunks() { List<List<T>> out; return out; }\n}\nList<int> l;\n");
			check_true(contains(text, ":3:6: error: 'List<int>' would need 'List<List<int>>' and so on without end: the member 'chunks' names an instance of 'List' built from the "
				"instance itself; a class's members are made with it and a generic function only where it is called, so write it as one outside the class: "
				"chunks<T>(List<T> self)"), text);
			check_false(contains(text, "nests more than 64 deep"));
			check_true(contains(text, "    in List<int>, instantiated at <script>:5:5"), text);
			// a field, and an instance reached through another generic's text
			const std::string field = failure(*e, "class Tree<T> {\n\tshared_ptr<Tree<array<Tree<T>>>> up;\n}\nTree<int> t;\n");
			check_true(contains(field, "'Tree<int>' would need 'Tree<array<Tree<int>>>' and so on without end: the member 'up' names an instance of 'Tree' built from the instance "
				"itself; a class's members are made with it and a generic function only where it is called, so let a generic function outside the class make that instance"), field);
			const std::string through = failure(*e, "class A<T> { B<T> b; }\nclass B<T> {\n\tint n = 0;\n\tA<A<T>> twice() { A<A<T>> x; return x; }\n}\nA<int> a;\n");
			check_true(contains(through, ":4:3: error: 'A<int>' would need 'A<A<int>>' and so on without end: the member 'twice' names an instance of 'A' built from the instance itself"), through);
			check_true(contains(through, "so let a generic function outside the class make that instance"), through);
			// the way out the text names: a generic function is made only where it is called
			check_eq((int64_t)0, e->execute("class List<T> { array<T> items; }\nList<List<T>> chunks<T>(List<T> self) { List<List<T>> out; return out; }\nList<int> l;\nchunks(l).items.size();\n").as_int());
			// an instance that names itself, or a smaller one, is no such chain
			check_eq((int64_t)1, e->execute("class Node<T> { shared_ptr<Node<T>> next; array<Node<T>> kids; T v; }\nNode<Node<int>> n; n.v.v = 1; n.v.v;").as_int());
		});

		test("a_long_instance_name_keeps_its_two_ends_in_a_note", [this]() {
			auto e = stdlib_engine();
			const std::string text = failure(*e, "class A<T> { A<array<T>> next; } A<int> a;");
			check_true(contains(text, "generic instantiation nests more than 64 deep: 'A<int>' needs 'A<array<int>>' needs 'A<array<array<int>>>', and so on without end"));
			check_true(contains(text, "    in A<array<array<array<array<array<arra ... >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>, instantiated at <script>:1:15"), text);
			size_t longest = 0;
			for (size_t at = 0; at < text.size();) {
				const size_t end = std::min(text.find('\n', at), text.size());
				longest = std::max(longest, end - at);
				at = end + 1;
			}
			check_true(longest < 200, "no line of the listing spells a 64-deep name: " + std::to_string(longest));
		});

		test("two_units_that_use_an_instance_use_one_class", [this]() {
			const auto directory = unit_directory("two_units");
			write_unit(directory, "stack.jai", "class Stack<T> {\n\tarray<T> items;\n\tstatic int made = 0;\n\tStack() { made += 1; }\n\tvoid push(T x) { items.push(x); }\n}\n");
			write_unit(directory, "first.jai", "import \"stack.jai\";\nStack<int> first_stack;\nfirst_stack.push(1);\nStack<int> take_first() { return first_stack; }\n");
			write_unit(directory, "second.jai", "import \"stack.jai\";\nStack<int> second_stack;\nint made_in_second() { return Stack<int>::made; }\n");
			const std::string root = write_unit(directory, "root.jai",
				"import \"first.jai\";\nimport \"second.jai\";\nsecond_stack = take_first();\nsecond_stack.push(2);\n"
				"to_string(second_stack.items.size()) + \"|\" + to_string(Stack<int>::made) + \"|\" + to_string(made_in_second()) + \"|\" + reflect::type_of(second_stack);\n");
			auto e = stdlib_engine();
			// each unit declares the instance it names; the second unit's declaration finds the class there and leaves its statics alone
			check_eq(std::string("2|2|2|Stack<int>"), e->execute_file(root).as_string());
		});

		test("an_error_in_another_files_generic_names_that_file", [this]() {
			const auto directory = unit_directory("other_file");
			const std::string lib = write_unit(directory, "lib.jai", "class Box<T> {\n\tT item;\n\tint size() { return item.count; }\n}\n");
			const std::string root = write_unit(directory, "root.jai", "import \"lib.jai\";\nBox<int> b;\n");
			auto e = stdlib_engine();
			std::string text;
			try { e->execute_file(root); } catch (const std::exception& error) { text = error.what(); }
			check_true(contains(text, lib + ":3:27: error: int has no member 'count'"));
			check_true(contains(text, "    in Box<int>, instantiated at " + root + ":2:4"));
		});

		test("a_later_execute_instantiates_what_an_earlier_one_declared", [this]() {
			auto e = stdlib_engine();
			e->execute("class Cell<T> { T v; } T same<T>(T x) { return x; }");
			check_eq((int64_t)3, e->execute("Cell<int> c; c.v = same<int>(3); c.v;").as_int());
			// the generic declared again: instances made after it are made from the new text
			e->execute("T same<T>(T x) { return x + x; }");
			check_eq((int64_t)6, e->execute("Cell<int> c; c.v = same<int>(3); c.v;").as_int());
		});

		// ---- declaring it again (docs/site/guide/21-generics.html "Declaring it again")

		test("a_generic_declared_again_makes_its_live_instances_again", [this]() {
			auto e = stdlib_engine();
			e->execute("class Box<T> { T v; static int made = 0; Box() { made += 1; } int get() { return 1; } }");
			e->execute("Box<int> a; a.v = 5; int use() { Box<int> x; return x.get(); }");
			check_eq((int64_t)1, e->execute("use();").as_int());
			// the unit that declares Box again names no instance: Box<int> is made again all the same, as a class declared again is
			e->execute("class Box<T> { T v; int extra = 7; static int made = 0; Box() { made += 1; } int get() { return 3 + extra; } }");
			check_true(warnings(*e).empty());
			check_eq((int64_t)10, e->execute("a.get();").as_int(), "the object that lived takes the new body and the new field");
			check_eq((int64_t)10, e->execute("use();").as_int(), "a function compiled before the change calls the new body");
			check_eq(std::string("5|7|3"), e->execute("to_string(a.v) + \"|\" + to_string(a.extra) + \"|\" + to_string(Box<int>::made);").as_string(),
				"fields and statics keep their values, as a class declared again keeps them");
			// the plain class it mirrors
			e->execute("class Plain { int v; static int made = 0; Plain() { made += 1; } int get() { return 1; } }");
			e->execute("Plain p; p.v = 5; int use_plain() { Plain x; return x.get(); }");
			e->execute("use_plain();");
			e->execute("class Plain { int v; int extra = 7; static int made = 0; Plain() { made += 1; } int get() { return 3 + extra; } }");
			check_eq(std::string("10|10|5|7|3"), e->execute("to_string(p.get()) + \"|\" + to_string(use_plain()) + \"|\" + to_string(p.v) + \"|\" + to_string(p.extra) + \"|\" + to_string(Plain::made);").as_string());
		});

		test("the_instances_are_made_again_where_the_generic_is_declared", [this]() {
			auto e = stdlib_engine();
			e->execute("class Box<T> { int get() { return 1; } } T twice<T>(T x) { return x; }");
			e->execute("Box<int> a; int call() { return twice<int>(4); }");
			// top-level code runs in order: above the new declaration the old instance answers, below it the new one
			check_eq(std::string("1,4|2,8"), e->execute(
				"string before = to_string(a.get()) + \",\" + to_string(call());\n"
				"class Box<T> { int get() { return 2; } }\n"
				"T twice<T>(T x) { return x + x; }\n"
				"before + \"|\" + to_string(a.get()) + \",\" + to_string(call());\n").as_string());
			check_eq(std::string("1|2"), e->execute(
				"class Plain { int get() { return 1; } }\nPlain p;\nstring was = to_string(p.get());\nclass Plain { int get() { return 2; } }\nwas + \"|\" + to_string(p.get());\n").as_string(),
				"a class declared twice in a unit: each declaration runs where it stands");
		});

		test("a_generic_function_and_method_declared_again_reach_their_callers", [this]() {
			auto e = stdlib_engine();
			e->execute("T same<T>(T x) { return x; } namespace wire { T twice<T>(T x) { return x; } }");
			e->execute("int use() { return same<int>(3) + wire::twice<int>(10); }");
			check_eq((int64_t)13, e->execute("use();").as_int());
			e->execute("T same<T>(T x) { return x + x; }");
			check_eq((int64_t)16, e->execute("use();").as_int());
			// a namespace's function declared again by another text collides, as its plain function does, unless it is written override
			check_true(contains(failure(*e, "namespace wire { T twice<T>(T x) { return x + x; } }"), "Function 'twice' already exists in namespace 'wire'. Use 'override' keyword to replace it."));
			check_eq((int64_t)16, e->execute("use();").as_int());
			e->execute("namespace wire { T twice<T>(T x) override { return x + x; } }");
			check_eq((int64_t)26, e->execute("use();").as_int());
			// another parameter list is another overload beside the first, as for a plain function: the instance stays
			e->execute("T same<T>(T x, T y) { return x * y; }");
			check_eq((int64_t)26, e->execute("use();").as_int());
			check_eq((int64_t)12, e->execute("same<int>(3, 4);").as_int());
			// a generic method of a plain class, and of a generic class
			e->execute("class Tool { int level = 1; T scaled<T>(T x) { return x * level; } }");
			e->execute("Tool tool; int scale() { return tool.scaled<int>(3); }");
			check_eq((int64_t)3, e->execute("scale();").as_int());
			e->execute("class Tool { int level = 2; T scaled<T>(T x) { return x * level + 100; } }");
			check_eq((int64_t)103, e->execute("scale();").as_int(), "the object keeps its field, the method's instance is the new text");
			e->execute("class Kit<T> { T base; U plus<U>(U x) { return x; } }");
			e->execute("Kit<int> kit; int kit_use() { return kit.plus<int>(1); }");
			check_eq((int64_t)1, e->execute("kit_use();").as_int());
			e->execute("class Kit<T> { T base; U plus<U>(U x) { return x + 41; } }");
			check_true(warnings(*e).empty());
			check_eq((int64_t)42, e->execute("kit_use();").as_int());
		});

		test("an_instance_the_new_declaration_cannot_make_keeps_its_earlier_one_with_a_warning", [this]() {
			auto e = stdlib_engine();
			e->execute("class Entity { int id = 0; } class Rock { int weight = 9; }");
			e->execute("class Pool<T> { T item; int n() { return 1; } } class Box<T> { T v; int n() { return 1; } }");
			e->execute("Pool<Rock> rocks; Pool<Entity> all; Box<int> count; Box<string> words;");
			// a bound the argument no longer meets
			e->execute("class Pool<T : Entity> { T item; int n() { return 2; } }");
			check_eq(std::vector<std::string>{"'Pool<Rock>' keeps its earlier declaration: this declaration of 'Pool' cannot make it: Pool<Rock>: 'Rock' does not derive from 'Entity', which T requires"},
				warnings(*e));
			check_eq(std::string("1|2"), e->execute("to_string(rocks.n()) + \"|\" + to_string(all.n());").as_string());
			check_true(contains(failure(*e, "Pool<Rock> more;"), "Pool<Rock>: 'Rock' does not derive from 'Entity', which T requires"), "named in new code: the ordinary error");
			check_eq((int64_t)1, e->execute("rocks.n();").as_int());
			// a body that no longer checks for those arguments
			e->execute("class Box<T> {\n\tT v;\n\tint n() { return v.size() + 5; }\n}\n");
			check_eq(std::vector<std::string>{"'Box<int>' keeps its earlier declaration: this declaration of 'Box' cannot make it: int has no member 'size' (<script>:3:21)"},
				warnings(*e));
			check_eq(std::string("1|5"), e->execute("to_string(count.n()) + \"|\" + to_string(words.n());").as_string());
			check_true(contains(failure(*e, "Box<int> more;"), "int has no member 'size'"));
			// the next declaration that can make it does
			e->execute("class Box<T> { T v; int n() { return 3; } }");
			check_true(warnings(*e).empty());
			check_eq(std::string("3|3"), e->execute("to_string(count.n()) + \"|\" + to_string(words.n());").as_string());
			// a function's instance
			e->execute("T dbl<T>(T x) { return x; } int use_dbl() { return dbl<int>(4); }");
			e->execute("T dbl<T>(T x) { return x.twice(); }");
			check_eq(std::vector<std::string>{"'dbl<int>' keeps its earlier declaration: this declaration of 'dbl' cannot make it: int has no member 'twice' (<script>:1:26)"}, warnings(*e));
			check_eq((int64_t)4, e->execute("use_dbl();").as_int());
			// the unit that declares the generic again and names the instance itself: its error, not a warning
			check_true(contains(failure(*e, "class Box<T> { T v; int n() { return v.size(); } }\nBox<int> named;\n"), "int has no member 'size'"));
			check_eq((int64_t)3, e->execute("count.n();").as_int());
		});

		test("warn_mode_makes_the_units_own_instances_and_never_replaces_one_it_only_makes_again", [this]() {
			auto e = stdlib_engine();
			// an instance the unit's text names is made, its errors warnings like the unit's others
			e->execute("[[warn_mode]];\nclass Lax<T> { T v; int n() { return v.nope(); } int ok() { return 1; } }\nLax<int> lax;\n");
			check_eq(std::vector<std::string>{"int has no member 'nope'"}, warnings(*e));
			check_eq((int64_t)1, e->execute("lax.ok();").as_int());
			// an instance the unit only makes again keeps its earlier declaration when the new text does not check for it
			e->execute("class Box<T> { T v; int n() { return 1; } }");
			e->execute("Box<int> b; int use() { return b.n(); }");
			e->execute("[[warn_mode]];\nclass Box<T> { T v; int n() { return v.nope(); } }\n");
			check_eq(std::vector<std::string>{"'Box<int>' keeps its earlier declaration: this declaration of 'Box' cannot make it: int has no member 'nope' (<script>:2:40)"}, warnings(*e));
			check_eq((int64_t)1, e->execute("use();").as_int());
		});

		test("a_generic_class_declared_again_with_another_parameter_list_replaces_the_first", [this]() {
			auto e = stdlib_engine();
			e->execute("class Box<T> { T v; int n() { return 1; } }");
			e->execute("Box<int> a; a.v = 4;");
			e->execute("class Box<K, V> { K k; V v; int n() { return 2; } }");
			check_eq(std::vector<std::string>{"'Box<int>' keeps its earlier declaration: this declaration of 'Box' cannot make it: 'Box' takes 2 type arguments, not 1"}, warnings(*e));
			check_eq((int64_t)2, e->execute("Box<int, string> b; b.n();").as_int());
			check_true(contains(failure(*e, "Box<int> c;"), "'Box' takes 2 type arguments, not 1"));
			check_eq(std::string("1|4"), e->execute("to_string(a.n()) + \"|\" + to_string(a.v);").as_string(), "the instance that lived keeps working");
			// and back
			e->execute("class Box<T> { T v; int n() { return 3; } }");
			check_eq(std::vector<std::string>{"'Box<int, string>' keeps its earlier declaration: this declaration of 'Box' cannot make it: 'Box' takes 1 type argument, not 2"}, warnings(*e));
			check_eq((int64_t)3, e->execute("a.n();").as_int());
			check_eq((int64_t)2, e->execute("b.n();").as_int());
		});

		test("two_files_that_declare_one_generic_class_the_later_replaces_as_for_a_class", [this]() {
			const auto directory = unit_directory("two_files");
			// only a.jai names Box<string>: b.jai's declaration makes it again all the same, as it runs
			write_unit(directory, "a.jai", "class Box<T> { int get() { return 1; } }\nBox<int> from_a;\nBox<string> only_a;\nint a_get() { return from_a.get() * 10 + only_a.get(); }\n");
			write_unit(directory, "b.jai", "class Box<T> { int get() { return 2; } }\nint b_get() { Box<int> b; return b.get(); }\n");
			const std::string root = write_unit(directory, "root.jai", "import \"a.jai\";\nimport \"b.jai\";\nBox<int> mine;\nto_string(mine.get()) + to_string(a_get()) + to_string(b_get());\n");
			write_unit(directory, "pa.jai", "class Plain { int get() { return 1; } }\nPlain from_pa;\nint pa_get() { return from_pa.get(); }\n");
			write_unit(directory, "pb.jai", "class Plain { int get() { return 2; } }\nint pb_get() { Plain b; return b.get(); }\n");
			const std::string plain = write_unit(directory, "plain.jai", "import \"pa.jai\";\nimport \"pb.jai\";\nPlain mine;\nto_string(mine.get()) + to_string(pa_get()) + to_string(pb_get());\n");
			check_eq(std::string("222"), stdlib_engine()->execute_file(plain).as_string());
			check_eq(std::string("2222"), stdlib_engine()->execute_file(root).as_string());
			// between the two imports the first file's declaration answers
			const std::string between = write_unit(directory, "between.jai",
				"import \"a.jai\";\nstring first = to_string(a_get());\nimport \"b.jai\";\nfirst + \"|\" + to_string(a_get());\n");
			check_eq(std::string("11|22"), stdlib_engine()->execute_file(between).as_string());
		});

		test("a_class_and_a_generic_class_do_not_share_a_name", [this]() {
			auto e = stdlib_engine();
			const std::string generic_second = failure(*e, "class Box { int plain = 1; }\nclass Box<T> { T v; }\n1;");
			check_true(contains(generic_second, ":2:7: error: class 'Box' is declared without type parameters on line 1 and with 1 type parameter here: a class and a generic class cannot share a name"), generic_second);
			const std::string class_second = failure(*e, "namespace n { class Box<K, V> { K k; }\nclass Box { int plain = 1; } }\n1;");
			check_true(contains(class_second, ":2:7: error: class 'n::Box' is declared with 2 type parameters on line 1 and without type parameters here: a class and a generic class cannot share a name"), class_second);
			// from a later unit the name is taken over: the generic by the class
			e->execute("class Box<T> { T v; int n() { return 1; } }");
			e->execute("Box<int> q;");
			e->execute("class Box { int plain = 1; }");
			check_eq(std::vector<std::string>{"'Box<int>' keeps its earlier declaration: this declaration of 'Box' cannot make it: 'Box' takes no type arguments: it is not a generic class"}, warnings(*e));
			check_eq((int64_t)1, e->execute("Box p; p.plain;").as_int());
			check_true(contains(failure(*e, "Box<int> again;"), "'Box' takes no type arguments: it is not a generic class"));
			check_eq((int64_t)1, e->execute("q.n();").as_int(), "the instance that lived stays");
			// and the class by the generic: the class stays for what uses it already, the bare name is the generic's
			e->execute("int old_plain() { Box b; return b.plain; }");
			e->execute("class Box<T> { T v; int n() { return 2; } }");
			check_eq((int64_t)2, e->execute("Box<int> r; r.n();").as_int());
			const std::string bare = "'Box' is a generic class: name its type arguments (Box<T>)";
			check_true(contains(failure(*e, "Box again;"), bare));
			check_true(contains(failure(*e, "array<Box> boxes;"), bare));
			check_true(contains(failure(*e, "void take(Box b) { }"), bare));
			check_true(contains(failure(*e, "auto made = new Box();"), bare));
			check_true(contains(failure(*e, "auto held = dynamic_cast<shared_ptr<Box>>(p);"), bare));
			check_true(contains(failure(*e, "auto& same = dynamic_cast<Box&>(p);"), bare));
			check_true(contains(failure(*e, "auto same = cast<Box>(p);"), bare));
			check_true(contains(failure(*e, "class Sub : Box { }"), bare));
			check_true(contains(failure(*e, "auto made = Box();"), "'Box' is a generic class: its type arguments are written, never deduced from a constructor's (Box<T>(...))"));
			// every other place a class's bare name stands says the same: a braced construction, a static member read, stored
			// and called, the name as a value, a handle's and a container's argument, a result, a field, a lambda's parameter, a
			// range-for's and a catch's variable, a type argument of a host function, a generic class and a generic function
			e->execute("class Tally { static int made = 7; static int count() { return 3; } }\nclass Cell<T> { T v; }\nT same<T>(T x) { return x; }");
			e->execute("class Tally<T> { T v; }");
			const std::string tally = "'Tally' is a generic class: name its type arguments (Tally<T>)";
			for (const char* use : {"auto made = Box{};", "Box made{};", "int n = Tally::made;", "Tally::made = 3;", "int n = Tally::count();", "auto f = Tally::count;",
			                        "auto c = Tally;", "string s = reflect::type_of(Tally);", "int n = Tally.made;",
			                        "shared_ptr<Tally> h;", "weak_ptr<Tally> w;", "map<string, Tally> m;", "Tally give() { return null; }", "function give() -> Tally { return null; }",
			                        "class Holder { Tally held; }", "auto f = (Tally t) => 1;", "array<int> xs; for (Tally t : xs) { }", "try { throw 1; } catch (Tally t) { }",
			                        "auto h = make_shared<Tally>();", "auto t = from_json<Tally>(\"{}\");", "Cell<Tally> c;", "same<Tally>(p);", "Tally& r = p;"}) {
				const std::string text = failure(*e, use);
				check_true(contains(text, std::string(use).find("Tally") != std::string::npos ? tally : bare), std::string(use) + ": " + text);
			}
			// a namespace's generic class, by its qualified name and by its bare name inside the namespace, and a generic class
			// no class stands behind
			e->execute("namespace shop { class Tally { static int made = 7; } }\nclass Lone<T> { static int made = 1; }\nnamespace shop { class Solo<T> { static int made = 2; } }");
			e->execute("namespace shop { class Tally<T> { T v; } }");
			for (const auto& [use, name] : std::vector<std::pair<const char*, const char*>>{{"int n = shop::Tally::made;", "shop::Tally"}, {"auto c = shop::Tally;", "shop::Tally"},
					{"namespace shop { int f() { return Tally::made; } }", "shop::Tally"}, {"using shop::Tally;\nint n = Tally::made;", "shop::Tally"},
					{"int n = Lone::made;", "Lone"}, {"auto c = Lone;", "Lone"}, {"int n = shop::Solo::made;", "shop::Solo"}}) {
				const std::string text = failure(*e, use);
				check_true(contains(text, std::string("'") + name + "' is a generic class: name its type arguments (" + name + "<T>)"), std::string(use) + ": " + text);
			}
			check_eq((int64_t)3, e->execute("Lone<int>::made + shop::Solo<int>::made;").as_int());
			check_eq(std::string("2|1|1"), e->execute("to_string(q.n()) + \"|\" + to_string(p.plain) + \"|\" + to_string(old_plain());").as_string());
			// a bare generic name with no class behind it says the same
			check_true(contains(failure(*e, "class Lone<T> { T v; }\nLone l;"), "'Lone' is a generic class: name its type arguments (Lone<T>)"));
		});

		test("an_edit_to_a_generic_makes_its_instances_again_once_and_keeps_their_statics", [this]() {
			const auto directory = unit_directory("reload_statics");
			const std::string body = " {\n\tstatic int made = 0;\n\tBox() { made += 1; }\n\tint size() { return SIZE; }\n}\n";
			const auto lib_text = [&](const std::string& size) {
				std::string text = "class Box<T>" + body + "class Plain" + body;
				text.replace(text.rfind("Box()"), 3, "Plain");
				for (size_t at = text.find("SIZE"); at != std::string::npos; at = text.find("SIZE")) { text.replace(at, 4, size); }
				return text;
			};
			const std::string lib = write_unit(directory, "lib.jai", lib_text("1"));
			write_unit(directory, "user.jai", "import \"lib.jai\";\nBox<int> box;\nPlain plain;\n"
				"int user_value() { return box.size() * 100 + Box<int>::made; }\nint plain_value() { return plain.size() * 100 + Plain::made; }\n");
			write_unit(directory, "other.jai", "import \"lib.jai\";\nint other_value() { Box<string> s; return s.size() * 100 + Box<string>::made; }\n");
			auto e = stdlib_engine();
			e->add_include_path(directory.string());
			e->execute("import \"user.jai\"; import \"other.jai\";");
			check_eq(std::string("101|101|101"), e->execute("to_string(user_value()) + \"|\" + to_string(plain_value()) + \"|\" + to_string(other_value());").as_string());
			auto stamp = std::filesystem::last_write_time(lib);
			write_unit(directory, "lib.jai", lib_text("5"));
			std::filesystem::last_write_time(lib, stamp += std::chrono::seconds(3));
			// only user.jai is imported again: the edit reaches the instance other.jai made too, and an instance's static is what a
			// class's is after the same reload (the unit that runs again declares the instance it finds made, and resets nothing)
			check_true(e->execute("import(\"user.jai\");").as_bool());
			check_true(warnings(*e).empty());
			const std::string plain_now = e->execute("to_string(plain_value());").as_string();
			check_eq(plain_now, e->execute("to_string(user_value());").as_string(), "Box<int> beside the class written by hand");
			check_eq((int64_t)5, e->execute("plain_value() / 100;").as_int());
			check_eq((int64_t)502, e->execute("other_value();").as_int(), "Box<string>, which no unit that ran again names, was made again with its static kept");
		});

		// ---- deduction

		const std::string deducible = R"(
			T largest<T>(array<T> xs) { T best = xs[0]; for (auto x : xs) { if (x > best) { best = x; } } return best; }
			T id<T>(T x) { return x; }
			K first<K, V>(map<K, V> m) { for (auto [k, v] : m) { return k; } return K(); }
			class Box<T> { T item; Box(T x) { item = x; } }
			class Crate : Box<string> { Crate() : Box<string>("wood") {} }
			T unbox<T>(Box<T> b) { return b.item; }
			class Goblin { int gold = 3; }
			T deref<T>(shared_ptr<T> p) { return p; }
			int gold_of<T>(weak_ptr<T> w) { return w.lock().gold; }
			void swap<T>(T& a, T& b) { T t = a; a = b; b = t; }
			T either<T>(T a, T b) { return a; }
			T make<T>() { return T(); }
			U conv<U, T>(T x) { return cast<U>(x); }
			T opt<T>(T a, T b = T()) { return a; }
		)";

		test("a_call_deduces_its_type_arguments_from_its_value_arguments", [this, deducible]() {
			auto e = stdlib_engine();
			e->execute(deducible);
			check_eq((int64_t)8, e->execute("array<int> scores = [4, 8, 2]; int top = largest(scores); top;").as_int());
			check_eq(std::string("1.5b"), e->execute("to_string(largest([1.5, 0.5])) + largest([\"a\", \"b\"]);").as_string());
			check_eq(std::string("4s2.5trueGoblinGoblin"), e->execute(
				"to_string(id(4)) + id(\"s\") + to_string(id(2.5)) + to_string(id(true)) + reflect::type_of(id(Goblin())) + reflect::type_of(id(new Goblin()));").as_string());
			check_eq(std::string("k"), e->execute("map<string, int> m = {\"k\": 1}; first(m);").as_string());
			check_eq(std::string("j"), e->execute("first({\"j\": 1});").as_string());
			check_eq((int64_t)7, e->execute("auto b = Box<int>(7); unbox(b);").as_int());
			check_eq(std::string("wood"), e->execute("Crate c; unbox(c);").as_string());   // a class that derives from an instance
			check_eq((int64_t)3, e->execute("shared_ptr<Goblin> g = new Goblin(); deref(g).gold;").as_int());
			check_eq((int64_t)3, e->execute("shared_ptr<Goblin> g = new Goblin(); weak_ptr<Goblin> w = g; gold_of(w);").as_int());
			check_eq(std::string("2,1"), e->execute("int x = 1; int y = 2; swap(x, y); to_string(x) + \",\" + to_string(y);").as_string());
			check_eq(std::string("3,4"), e->execute("to_string(opt(3)) + \",\" + to_string(opt(4, 5));").as_string());
			// the arguments written fill the leading parameters, the rest are deduced
			check_eq(std::string("41!"), e->execute("conv<string>(41) + \"!\";").as_string());
			check_near(1.0, e->execute("either<float>(1, 2.5);").as_float(), 1e-9);
			check_eq(std::string("abab8"), e->execute("namespace m { T twice<T>(T x) { return x + x; } int go() { return twice(4); } } m::twice(\"ab\") + to_string(m::go());").as_string());
		});

		test("a_deduction_that_fails_says_what_to_write", [this, deducible]() {
			auto e = stdlib_engine();
			e->execute(deducible);
			check_true(contains(failure(*e, "make();"), "cannot deduce T for 'make': no parameter names it; write make<T>()"));
			check_true(contains(failure(*e, "var v = 4; largest(v);"), "cannot deduce T for 'largest': argument 1's type is not known at load; write largest<T>(...)"));
			check_true(contains(failure(*e, "largest([1, \"a\"]);"), "cannot deduce T for 'largest': argument 1's type is not known at load; write largest<T>(...)"));
			check_true(contains(failure(*e, "either(1, \"a\");"), "cannot deduce T for 'either': it is int from argument 1 and string from argument 2"));
			check_true(contains(failure(*e, "either(1, 2.5);"), "cannot deduce T for 'either': it is int from argument 1 and float from argument 2"));   // no promotion
			check_true(contains(failure(*e, "id(null);"), "cannot deduce T for 'id': argument 1 is null, which says nothing of a type; write id<T>(...)"));
			check_true(contains(failure(*e, "auto f = (x) => x; id(f);"), "cannot deduce T for 'id': argument 1 is a function, which says nothing of a type; write id<T>(...)"));
			check_true(contains(failure(*e, "largest(5);"), "cannot deduce T for 'largest': argument 1 is int, which 'array<T>' does not match; write largest<T>(...)"));
		});

		test("the_hint_spells_the_generics_own_parameters_with_the_deduced_ones_filled_in", [this, deducible]() {
			auto e = stdlib_engine();
			e->execute(deducible);
			e->execute("V key_of<K, V>(K key) { return V(); } V third<A, V, C>(A a, C c) { return V(); } V of_map<K, V>(map<K, V> m) { return V(); }");
			check_true(contains(failure(*e, "conv(1);"), "cannot deduce U for 'conv': no parameter names it; write conv<U, int>(...)"));
			check_true(contains(failure(*e, "key_of(\"hp\");"), "cannot deduce V for 'key_of': no parameter names it; write key_of<string, V>(...)"));
			check_true(contains(failure(*e, "third(1, [2.5]);"), "cannot deduce V for 'third': no parameter names it; write third<int, V, array<float>>(...)"));
			check_true(contains(failure(*e, "var v = 1; of_map(v);"), "cannot deduce K for 'of_map': argument 1's type is not known at load; write of_map<K, V>(...)"));
			check_true(contains(failure(*e, "namespace m { T make<T>() { return T(); } } m::make();"), "cannot deduce T for 'make': no parameter names it; write make<T>()"));
		});

		test("a_construction_expression_has_a_type_deduction_reads", [this, deducible]() {
			auto e = stdlib_engine();
			e->execute(deducible);
			// of<T>(x) names the T the call deduced: the argument list of the instance Of<T>
			e->execute("class Of<T> {} string of<T>(T x) { string name = reflect::type_of(Of<T>()); return name.substr(3, name.size() - 4); }");
			// a primitive's construction, a class's and an instance's, by every spelling
			check_eq(std::string("int float string bool char int"), e->execute(
				"of(int(2.5)) + \" \" + of(float(2)) + \" \" + of(string(2)) + \" \" + of(bool(1)) + \" \" + of(char(65)) + \" \" + of(int());").as_string());
			check_eq(std::string("Goblin Goblin Box<int> Box<string>"), e->execute(
				"of(Goblin()) + \" \" + of(Goblin{}) + \" \" + of(Box<int>(3)) + \" \" + of(Box<string>(\"s\"));").as_string());
			check_eq((int64_t)7, e->execute("unbox(Box<int>(7));").as_int());
			// new, make_shared and cast name what they make
			check_eq(std::string("shared_ptr<Goblin> shared_ptr<Goblin> shared_ptr<Box<int>>"), e->execute(
				"of(new Goblin()) + \" \" + of(make_shared<Goblin>()) + \" \" + of(new Box<int>(3));").as_string());
			check_eq((int64_t)3, e->execute("deref(new Goblin()).gold + deref(make_shared<Goblin>()).gold - unbox(deref(new Box<int>(3)));").as_int());
			check_eq(std::string("int Goblin float"), e->execute("of(cast<int>(2.5)) + \" \" + of(cast<Goblin>(Goblin())) + \" \" + of(cast<float>(2));").as_string());
			// a math type's constructor, a typed container's, an enum's value
			check_eq(std::string("math::vec2 math::vec3 math::color math::ivec2"), e->execute(
				"of(math::vec2(1.0, 2.0)) + \" \" + of(math::vec3(1.0, 2.0, 3.0)) + \" \" + of(math::color(1.0, 0.5, 0.0, 1.0)) + \" \" + of(math::ivec2(1, 2));").as_string());
			check_eq(std::string("array<int> map<string,int> Tint"), e->execute(
				"enum Tint { red, green } of(array<int>(3)) + \" \" + of(map<string, int>()) + \" \" + of(Tint::green);").as_string());
		});

		test("a_declared_result_has_a_type_deduction_reads", [this, deducible]() {
			// each program is one unit: the receivers are declared where the calls stand, so the load is certain of them
			const std::string declared = deducible +
				"class Of<T> {} string of<T>(T x) { string name = reflect::type_of(Of<T>()); return name.substr(3, name.size() - 4); }\n"
				"U apply<T, U>(T x, function<U(T)> f) { return f(x); } namespace tools { T keep<T>(T x) { return x; } } class Bag { T hold<T>(T x) { return x; } }\n"
				"string s = \"hello\"; utf8_string u = \"hello\"; array<int> xs = [3, 1, 2]; array<string> names = [\"b\", \"a\"]; map<string, int> ages = {\"a\": 1}; Bag bag;\n";
			const auto text = [&](const std::string& code) { auto e = stdlib_engine(); return e->execute(declared + code).as_string(); };
			const auto refusal = [&](const std::string& code) { auto e = stdlib_engine(); return failure(*e, declared + code); };
			// a builtin method's result is the one the registry's catalog states, on each receiver
			check_eq(std::string("string bool int int char"), text(
				"of(s.substr(1)) + \" \" + of(s.contains(\"l\")) + \" \" + of(s.find(\"l\")) + \" \" + of(s.compare(\"a\")) + \" \" + of(s.at(0));"));
			check_eq(std::string("utf8_string utf8_string bool int"), text(
				"of(u.substr(1)) + \" \" + of(u.at(0)) + \" \" + of(u.contains(\"l\")) + \" \" + of(u.find(\"l\"));"));
			check_eq(std::string("bool int string bool bool"), text(
				"of(xs.contains(1)) + \" \" + of(xs.index_of(1)) + \" \" + of(names.join(\",\")) + \" \" + of(ages.contains(\"a\")) + \" \" + of(ages.has(\"a\"));"));
			check_eq(std::string("int float"), text("of(s.to_int(0)) + \" \" + of(s.to_float(0.5));"));   // the fallback's own type settles it
			// a host function's declared result, a host namespace entry's and a math value's component
			check_eq(std::string("int string string float int"), text(
				"of(thread_count()) + \" \" + of(current_script_dir()) + \" \" + of(string::repeat(\"a\", 2)) + \" \" + of(math::vec2(1.0, 2.0).x) + \" \" + of(math::ivec2(1, 2).x);"));
			// an operator over them, a literal of them, an auto variable declared with one
			check_eq(std::string("int string bool float array<string> array<bool> string"), text(
				"auto part = s.substr(1); of(s.find(\"l\") + 1) + \" \" + of(s.substr(1) + s.substr(2)) + \" \" + of(s.contains(\"l\") && true) + \" \" + of(s.find(\"l\") * 1.5) + \" \" + "
				"of([s.substr(1), \"x\"]) + \" \" + of([xs.contains(1)]) + \" \" + of(part);"));
			// a lambda that returns one names its signature's result, declared or an arrow; a generic method and a namespace's
			// function deduce as a function does
			check_eq(std::string("string string string int int bool int"), text(
				"of(apply(1, (int k) => names.join(\",\"))) + \" \" + of(apply(1, k => names.join(\",\"))) + \" \" + of(apply(1, (int k) => { if (k > 0) { return s.substr(k); } return \"none\"; })) + \" \" + "
				"of(apply(s.find(\"l\"), k => k + 1)) + \" \" + of(apply(1, (int k) => thread_count())) + \" \" + of(tools::keep(xs.contains(1))) + \" \" + of(bag.hold(s.find(\"l\")));"));
			check_eq(std::string("2,ello"), text("to_string(either(s.find(\"l\"), s.find(\"h\"))) + \",\" + id(s.substr(1));"));
			// declared results that disagree are the usual error, and a lambda whose returns disagree still says nothing
			check_true(contains(refusal("either(s.find(\"l\"), s.substr(1));"), "cannot deduce T for 'either': it is int from argument 1 and string from argument 2"));
			check_true(contains(refusal("apply(1, (int k) => { if (k > 0) { return s.find(\"l\"); } return \"none\"; });"),
				"cannot deduce U for 'apply': argument 2 is a function, which says nothing of a type; write apply<int, U>(...)"));
			// a declared result is a type like any other: beside another type it is the two-types error, and it picks among generics
			check_true(contains(refusal("either(s.find(\"l\"), 2.5);"), "cannot deduce T for 'either': it is int from argument 1 and float from argument 2"));
			check_true(contains(refusal("either(xs.contains(1), 1);"), "cannot deduce T for 'either': it is bool from argument 1 and int from argument 2"));
			check_true(contains(refusal("T first<T>(T a, int n) { return a; } U first<U>(string a, U n) { return n; } first(s.substr(1), 5);"),
				"call to 'first' is ambiguous: the generics 'first<T>'"));
			check_eq(std::string("string 2.0"), text("of(either(s.substr(1), \"x\")) + \" \" + to_string(either<float>(s.find(\"l\"), 2.5));"));
			// the text functions, a string's element, a host namespace's value and a builtin on a declared result
			check_eq(std::string("string string string char utf8_string float float int int int"), text(
				"int n = 5; of(to_string(n)) + \" \" + of(format(\"{}\", n)) + \" \" + of(vformat(\"{}\", n)) + \" \" + of(s[1]) + \" \" + of(u[1]) + \" \" + of(math::pi) + \" \" + "
				"of(math::tau * 2.0) + \" \" + of(s.substr(1).size()) + \" \" + of(to_string(n).size()) + \" \" + of(names.back().size());"));
			// so a line that can never succeed is the load's error, in the slot's own text
			check_true(contains(refusal("if (false) { array<int> q = to_string(5); }"), "cannot initialize 'q' declared 'array<int>' with 'string': expected 'array<int>', got 'string'"));
			// what no declaration states is still not known: a var's method and element, a builtin's open result
			for (const char* unknown : {"var v = s; id(v.substr(1));", "var v = s; id(v[1]);", "id(s.split(\",\"));"}) {
				check_true(contains(refusal(unknown), "cannot deduce T for 'id': argument 1's type is not known at load; write id<T>(...)"), unknown);
			}
		});

		test("an_ordinary_function_that_fits_wins_over_a_generic", [this]() {
			auto e = stdlib_engine();
			check_eq(std::string("101s"), e->execute("T id<T>(T x) { return x; } int id(int x) { return x + 100; } to_string(id(1)) + id(\"s\");").as_string());
			check_eq(std::string("1,2"), e->execute("T one<T>(T a) { return a; } T one<T, U>(T a, U b) { return a; } to_string(one(1)) + \",\" + to_string(one(2, \"x\"));").as_string());
			const std::string both = failure(*e, "void show<T>(array<T> xs) {}\nvoid show<T>(T x) {}\nshow(5);\nshow([1]);\n");
			check_true(contains(both, "<script>:4:9: error: call to 'show' is ambiguous: the generics 'show<T>' (<script>:1:6) and 'show<T>' (<script>:2:6) both take it"));
			check_false(contains(both, "<script>:3:"));   // an int is no array<T>: one generic takes it
			check_true(contains(failure(*e, "void two<T>(T a) {}\nvoid two<U>(U b) {}\n"), "generic 'two<U>' is declared again: line 1 declares it with the same parameters"));
			check_true(contains(failure(*e, "class Dup<T> {}\nclass Dup<U> {}\n"), "generic 'Dup<U>' is declared again: line 1 declares it with the same parameters"));
		});

		test("an_ordinary_function_another_unit_declares_wins_over_a_generic_too", [this]() {
			const std::string plain = "int twice(int x) { return x + x + 1; }\n";
			const std::string generic = "T twice<T>(T x) { return x + x; }\n";
			const std::string calls = "to_string(twice(2)) + \"|\" + twice(\"ab\");";
			check_eq(std::string("5|abab"), stdlib_engine()->execute(plain + generic + calls).as_string(), "one unit: the rule the others mirror");
			// from earlier executes, declared in either order
			auto e = stdlib_engine();
			e->execute(plain);
			e->execute(generic);
			check_eq(std::string("5|abab"), e->execute(calls).as_string());
			auto later = stdlib_engine();
			later->execute(generic);
			later->execute(plain);
			check_eq(std::string("5|abab"), later->execute(calls).as_string());
			// from imports, in either order
			const auto directory = unit_directory("ordinary_units");
			write_unit(directory, "plain.jai", plain);
			write_unit(directory, "generic.jai", generic);
			const std::string root = write_unit(directory, "root.jai", "import \"plain.jai\";\nimport \"generic.jai\";\n" + calls + "\n");
			const std::string other = write_unit(directory, "other.jai", "import \"generic.jai\";\nimport \"plain.jai\";\n" + calls + "\n");
			check_eq(std::string("5|abab"), stdlib_engine()->execute_file(root).as_string());
			check_eq(std::string("5|abab"), stdlib_engine()->execute_file(other).as_string());
			// a namespace's: one unit, from inside it (above the function and below it) and by its qualified name
			check_eq(std::string("5|abab|5|abab|5|abab"), stdlib_engine()->execute(
				"namespace wire {\nstring above() { return to_string(twice(2)) + \"|\" + twice(\"ab\"); }\n" + plain + generic +
				"string below() { return to_string(twice(2)) + \"|\" + twice(\"ab\"); }\n}\n"
				"wire::above() + \"|\" + wire::below() + \"|\" + to_string(wire::twice(2)) + \"|\" + wire::twice(\"ab\");").as_string());
			// and from earlier executes
			auto spaced = stdlib_engine();
			spaced->execute("namespace wire {\n" + plain + "}\n");
			spaced->execute("namespace wire {\n" + generic + "}\n");
			check_eq(std::string("5|abab"), spaced->execute("to_string(wire::twice(2)) + \"|\" + wire::twice(\"ab\");").as_string());
			check_eq(std::string("5|abab"), spaced->execute("namespace wire { string probe() { return to_string(twice(2)) + \"|\" + twice(\"ab\"); } }\nwire::probe();").as_string());
			// and from another file of the import closure, in either order: by its qualified name, by its bare name in a third
			// file's block of the namespace, and in the generic's own file
			write_unit(directory, "wire_plain.jai", "namespace wire {\n" + plain + "}\n");
			write_unit(directory, "wire_generic.jai", "namespace wire {\n" + generic + "string inside() { return to_string(twice(2)) + \"|\" + twice(\"ab\"); }\n}\n");
			const std::string wire_calls = "namespace wire { string probe() { return to_string(twice(2)) + \"|\" + twice(\"ab\"); } }\n"
				"to_string(wire::twice(2)) + \"|\" + wire::twice(\"ab\") + \"|\" + wire::probe() + \"|\" + wire::inside();\n";
			const std::string wire_root = write_unit(directory, "wire_root.jai", "import \"wire_plain.jai\";\nimport \"wire_generic.jai\";\n" + wire_calls);
			const std::string wire_other = write_unit(directory, "wire_other.jai", "import \"wire_generic.jai\";\nimport \"wire_plain.jai\";\n" + wire_calls);
			check_eq(std::string("5|abab|5|abab|5|abab"), stdlib_engine()->execute_file(wire_root).as_string());
			check_eq(std::string("5|abab|5|abab|5|abab"), stdlib_engine()->execute_file(wire_other).as_string());
			// a method, static or not, of a class this unit declares or an earlier one did
			const std::string tool = "class Tool {\n\tint twice(int x) { return x + x + 1; }\n\tT twice<T>(T x) { return x + x; }\n"
				"\tstatic int make(int x) { return x + 1; }\n\tstatic T make<T>(T x) { return x; }\n}\n";
			const std::string uses = "Tool t;\nto_string(t.twice(2)) + \"|\" + t.twice(\"ab\") + \"|\" + to_string(Tool::make(2)) + \"|\" + Tool::make(\"s\");";
			check_eq(std::string("5|abab|3|s"), stdlib_engine()->execute(tool + uses).as_string());
			auto held = stdlib_engine();
			held->execute(tool);
			check_eq(std::string("5|abab|3|s"), held->execute(uses).as_string());
		});

		// ---- generics of one name that take the same type arguments: two instances of that name, one overload set

		const std::string shows =
			"string show<T>(array<T> xs) { return \"array \" + to_string(xs.size()); }\n"
			"string show<T>(T x, int n) { return \"pair \" + to_string(n); }\n";

		test("generics_that_take_the_same_type_arguments_are_overloads_of_one_instance_name", [this, shows]() {
			auto e = stdlib_engine();
			check_eq(std::string("array 1|pair 1"), e->execute(shows + "show([1]) + \"|\" + show(5, 1);").as_string());
			// the functions written by hand that the two instances are
			check_eq(std::string("array 1|pair 1"), stdlib_engine()->execute(
				"string shown(array<int> xs) { return \"array \" + to_string(xs.size()); }\nstring shown(int x, int n) { return \"pair \" + to_string(n); }\n"
				"shown([1]) + \"|\" + shown(5, 1);").as_string());
			// the type arguments written: the value arguments pick among the instances, as among functions of one name
			check_eq(std::string("array 2|pair 7"), stdlib_engine()->execute(shows + "show<int>([1, 2]) + \"|\" + show<int>(5, 7);").as_string());
			const std::string kinds = "string kind<T>(array<T> xs) { return \"array\"; }\nstring kind<T>(T x) { return \"one\"; }\n";
			check_eq(std::string("array|one|one"), stdlib_engine()->execute(kinds + "kind<int>([1]) + \"|\" + kind<int>(5) + \"|\" + kind<string>(\"s\");").as_string());
			// deduced to the same arguments by both: the same overload set
			check_eq(std::string("same|float"), stdlib_engine()->execute(
				"string near<T>(T a, T b) { return \"same\"; }\nstring near<T>(T a, float b) { return \"float\"; }\nnear(1, 2) + \"|\" + near(1, 2.5);").as_string());
			// no instance of the name takes the value arguments: the text two functions written by hand give
			check_true(contains(failure(*e, kinds + "kind<int>(\"s\");\n"), "<script>:3:14: error: no overload of 'kind<int>' matches this call (1 argument(s)) (declared at 1:8)"));
			check_true(contains(failure(*e, "string kinds(array<int> xs) { return \"\"; }\nstring kinds(int x) { return \"\"; }\nkinds(\"s\");\n"),
				"<script>:3:10: error: no overload of 'kinds' matches this call (1 argument(s)) (declared at 1:8)"));
			// generics that find different type arguments name different instances: the call is ambiguous
			check_true(contains(failure(*e, kinds + "kind([1]);\n"), "call to 'kind' is ambiguous: the generics 'kind<T>' (<script>:1:8) and 'kind<T>' (<script>:2:8) both take it"));
			// in a namespace, from inside and outside
			check_eq(std::string("array 1|pair 1|array 0|pair 3"), stdlib_engine()->execute(
				"namespace wire {\n" + shows + "string inside() { array<int> none; return show(none) + \"|\" + show<string>(\"x\", 3); }\n}\n"
				"wire::show([1]) + \"|\" + wire::show(5, 1) + \"|\" + wire::inside();").as_string());
		});

		test("generic_methods_that_take_the_same_type_arguments_are_overloads_of_one_instance_name", [this, shows]() {
			auto e = stdlib_engine();
			check_eq(std::string("array 1|pair 1|array 2|pair 9"), e->execute(
				"class Desk {\n" + shows + "}\nDesk d;\nd.show([1]) + \"|\" + d.show(5, 1) + \"|\" + d.show<int>([1, 2]) + \"|\" + d.show<int>(5, 9);").as_string());
			check_eq(std::string("array 1|pair 1"), stdlib_engine()->execute(
				"class Desk {\nstatic string show<T>(array<T> xs) { return \"array \" + to_string(xs.size()); }\nstatic string show<T>(T x, int n) { return \"pair \" + to_string(n); }\n}\n"
				"Desk::show([1]) + \"|\" + Desk::show(5, 1);").as_string());
			// a generic class's generic methods
			check_eq(std::string("1|5|ab"), stdlib_engine()->execute(
				"class Kit<T> {\n\tT base;\n\tU plus<U>(U x) { return x; }\n\tU plus<U>(U x, U y) { return x + y; }\n}\n"
				"Kit<int> kit;\nto_string(kit.plus(1)) + \"|\" + to_string(kit.plus(2, 3)) + \"|\" + kit.plus<string>(\"a\", \"b\");").as_string());
			// a function above the class calls both: each instance waits for the class and joins it
			auto above = stdlib_engine();
			check_eq(std::string("array 1|pair 1"), above->execute(
				"string ask(Desk d) { return d.show([1]) + \"|\" + d.show(5, 1); }\nclass Desk {\n" + shows + "}\nask(Desk());").as_string());
			check_true(detail::engine_access::get_class_registry(*above).waiting_instances_.empty());
			auto waits = stdlib_engine();
			check_true(contains(failure(*waits, "string ask(Desk d) { return d.show([1]) + \"|\" + d.show(5, 1); }\nthrow \"stopped\";\nclass Desk {\n" + shows + "}\n"), "stopped"));
			check_eq((size_t)2, detail::engine_access::get_class_registry(*waits).waiting_instances_.size(), "one entry per instance: the two overloads are two");
			waits->execute("class Desk {\n" + shows + "}\n");
			check_eq(std::string("array 1|pair 1"), waits->execute("ask(Desk());").as_string());
		});

		test("units_share_each_overloads_instance_and_a_function_above_the_generics_calls_both", [this, shows]() {
			auto e = stdlib_engine();
			e->execute(shows);
			e->execute("string first() { return show([1]); }");
			e->execute("string second() { return show(5, 2); }");   // the name holds the other overload's instance: this one joins it
			check_eq(std::string("array 1|pair 2|array 3|pair 4"), e->execute("first() + \"|\" + second() + \"|\" + show([1, 2, 3]) + \"|\" + show(0, 4);").as_string());
			auto above = stdlib_engine();
			check_eq(std::string("array 1|pair 1"), above->execute("string both() { return show([1]) + \"|\" + show(5, 1); }\n" + shows + "both();").as_string());
			check_true(detail::engine_access::get_class_registry(*above).waiting_functions_.empty());
			const auto directory = unit_directory("overload_units");
			write_unit(directory, "shows.jai", shows);
			write_unit(directory, "first.jai", "import \"shows.jai\";\nstring first() { return show([1]); }\n");
			write_unit(directory, "second.jai", "import \"shows.jai\";\nstring second() { return show(5, 2); }\n");
			const std::string root = write_unit(directory, "root.jai", "import \"first.jai\";\nimport \"second.jai\";\nfirst() + \"|\" + second() + \"|\" + show(7, 8);\n");
			check_eq(std::string("array 1|pair 2|pair 8"), stdlib_engine()->execute_file(root).as_string());
			check_eq(std::string("array 1|pair 2|pair 8"), stdlib_engine()->execute_file(root).as_string(), "and from the units' jaibite caches");
		});

		test("an_overload_declared_again_makes_again_its_own_instances_only", [this, shows]() {
			auto e = stdlib_engine();
			e->execute(shows);
			e->execute("string first() { return show([1]); } string second() { return show(5, 2); }");
			e->execute("string show<T>(T x, int n) { return \"PAIR \" + to_string(n); }");
			check_true(warnings(*e).empty());
			check_eq(std::string("array 1|PAIR 2"), e->execute("first() + \"|\" + second();").as_string());
			e->execute("string show<T>(array<T> xs) { return \"ARRAY\"; }");
			check_true(warnings(*e).empty());
			check_eq(std::string("ARRAY|PAIR 2"), e->execute("first() + \"|\" + second();").as_string());
			// one that cannot make its instance keeps it, with the warning, and the other overload's is made again all the same
			e->execute("string show<T>(T x, int n) { return x.nope(); }\nstring show<T>(array<T> xs) { return \"third\"; }\n");
			check_eq(std::vector<std::string>{"'show<int>' keeps its earlier declaration: this declaration of 'show' cannot make it: int has no member 'nope' (<script>:1:39)"}, warnings(*e));
			check_eq(std::string("third|PAIR 2"), e->execute("first() + \"|\" + second();").as_string());
			// a class declared again makes each of its generic methods' instances again
			e->execute("class Desk {\n" + shows + "}\n");
			e->execute("Desk desk; string desk_first() { return desk.show([1]); } string desk_second() { return desk.show(5, 2); }");
			check_eq(std::string("array 1|pair 2"), e->execute("desk_first() + \"|\" + desk_second();").as_string());
			e->execute("class Desk {\n\tstring show<T>(array<T> xs) { return \"ARRAY\"; }\n\tstring show<T>(T x, int n) { return \"PAIR\"; }\n}\n");
			check_true(warnings(*e).empty());
			check_eq(std::string("ARRAY|PAIR"), e->execute("desk_first() + \"|\" + desk_second();").as_string());
			e->execute("class Desk {\n\tstring show<T>(array<T> xs) { return \"array again\"; }\n\tstring show<T>(T x, int n) { return x.nope(); }\n}\n");
			check_eq(std::vector<std::string>{"'Desk::show<int>' keeps its earlier declaration: this declaration of 'Desk::show' cannot make it: int has no member 'nope' (<script>:3:40)"}, warnings(*e));
			check_eq(std::string("array again|PAIR"), e->execute("desk_first() + \"|\" + desk_second();").as_string());
		});

		test("an_edit_to_one_overload_reaches_its_instance_and_leaves_the_others", [this, shows]() {
			const auto directory = unit_directory("overload_reload");
			const std::string lib = write_unit(directory, "shows.jai", shows);
			write_unit(directory, "user.jai", "import \"shows.jai\";\nstring first() { return show([1]); }\nstring second() { return show(5, 2); }\n");
			auto e = stdlib_engine();
			e->add_include_path(directory.string());
			e->execute("import \"user.jai\";");
			check_eq(std::string("array 1|pair 2"), e->execute("first() + \"|\" + second();").as_string());
			auto stamp = std::filesystem::last_write_time(lib);
			std::string edited = shows;
			edited.replace(edited.find("\"pair \""), 7, "\"PAIR \"");
			write_unit(directory, "shows.jai", edited);
			std::filesystem::last_write_time(lib, stamp + std::chrono::seconds(3));
			check_true(e->execute("import(\"user.jai\");").as_bool());
			check_true(warnings(*e).empty());
			check_eq(std::string("array 1|PAIR 2"), e->execute("first() + \"|\" + second();").as_string());
		});

		test("a_loaded_units_instance_belongs_to_the_overload_it_was_made_from", [this, shows]() {
			auto saver = stdlib_engine();
			auto bite = saver->jaibite(shows + "string first() { return show([1]); }\n");
			auto e = stdlib_engine();
			e->jaibite_load_bytes(bite.save_bytes()).execute();
			check_eq(std::string("array 1"), e->execute("first();").as_string());
			// the other overload declared again, with a body int does not check for: it made no show<int>, and makes none now
			e->execute("string show<T>(T x, int n) { return x.nope(); }");
			check_true(warnings(*e).empty(), warnings(*e).empty() ? std::string() : warnings(*e).front());
			check_eq(std::string("array 1"), e->execute("first();").as_string());
			// its own overload declared again: the instance the loaded unit declared is made again
			e->execute("string show<T>(array<T> xs) { return \"ARRAY\"; }");
			check_true(warnings(*e).empty());
			check_eq(std::string("ARRAY"), e->execute("first();").as_string());
		});

		test("a_jaibite_keeps_each_overloads_instance", [this, shows]() {
			auto e = stdlib_engine();
			auto bite = e->jaibite(shows + "class Desk {\n" + shows + "}\nDesk d;\nshow([1]) + \"|\" + show(5, 1) + \"|\" + d.show([1, 2]) + \"|\" + d.show(5, 3);\n");
			check_eq(std::string("array 1|pair 1|array 2|pair 3"), bite.execute().as_string());
			auto loader = stdlib_engine();
			auto loaded = loader->jaibite_load_bytes(bite.save_bytes());
			check_eq(std::string("array 1|pair 1|array 2|pair 3"), loaded.execute().as_string());
			check_eq(std::string("array 0|pair 4"), loader->execute("array<string> none; show(none) + \"|\" + show(\"s\", 4);").as_string());
		});

		// ---- bounds

		test("a_bound_is_checked_where_the_instance_is_asked_for", [this]() {
			auto e = stdlib_engine();
			e->execute(declarations());
			e->execute(R"(
				class Comparable<T> { bool before(T other) { return true; } }
				class Num : Comparable<Num> { int n = 0; }
				int ident<T : Entity>(T e) { return e.id; }
				bool sorted<T : Comparable<T>>(T a, T b) { return a.before(b); }
			)");
			check_eq((int64_t)0, e->execute("Pool<Goblin> goblins; Pool<Entity> all; goblins.live.size() + all.live.size();").as_int());
			check_eq((int64_t)0, e->execute("ident(Goblin()) + ident<Entity>(Entity());").as_int());
			check_true(e->execute("sorted(Num(), Num());").as_bool());   // the bound names the parameter: Comparable<Num>
			check_true(contains(failure(*e, "Pool<int> p;"), "<script>:1:5: error: Pool<int>: 'int' does not derive from 'Entity', which T requires"));
			check_true(contains(failure(*e, "ident(5);"), "<script>:1:8: error: ident<int>: 'int' does not derive from 'Entity', which T requires"));
			check_true(contains(failure(*e, "sorted(Goblin(), Goblin());"), "sorted<Goblin>: 'Goblin' does not derive from 'Comparable<Goblin>', which T requires"));
			// the error is the use's: the body the argument would break is not reported beside it
			const std::string only = failure(*e, "class Named<T : Entity> { T v; string name() { return v.nope; } }\nNamed<int> n;\n");
			check_true(contains(only, "Named<int>: 'int' does not derive from 'Entity', which T requires") && !contains(only, "nope"));
			check_true(contains(failure(*e, "class Bad<T : int> {}"), "expected a class after ':': 'T : Base' bounds T by a class"));
		});

		test("a_handle_meets_a_bound_its_class_meets", [this]() {
			auto e = stdlib_engine();
			dynamic_binder<generic_bound_host_base>(*e, "BoundHostBase").constructor<>().property("n", &generic_bound_host_base::n).build();
			dynamic_binder<generic_bound_host>(*e, "BoundHost").constructor<>().base_class<generic_bound_host_base>().build();
			e->execute(R"(
				class Entity { int id = 4; }
				class Goblin : Entity { int gold = 3; }
				class Rock { int id = 1; }
				int ident<T : Entity>(T e) { return e.id; }
				class Hold<T : Entity> { array<T> items; int first() { return items[0].id; } }
				namespace zoo { class Pen<T : Entity> { T held; } }
				class Mine : BoundHostBase { int own = 1; }
				int host_n<T : BoundHostBase>(T h) { return h.n; }
				shared_ptr<Goblin> g = new Goblin();
			)");
			check_eq((int64_t)16, e->execute("ident(g) + ident(new Goblin()) + ident<shared_ptr<Goblin>>(g) + ident(make_shared<Goblin>());").as_int());
			check_eq((int64_t)4, e->execute("Goblin v; Goblin& r = v; ident(r);").as_int());   // a reference deduces its class
			check_eq((int64_t)4, e->execute("Hold<shared_ptr<Goblin>> h; h.items.push(g); h.first();").as_int());
			check_eq((int64_t)3, e->execute("zoo::Pen<shared_ptr<Goblin>> p; p.held = g; p.held.gold;").as_int());
			check_eq((int64_t)0, e->execute("shared_ptr<BoundHost> bh = new BoundHost(); host_n(bh) + host_n(new Mine());").as_int());
			check_eq(std::string("reflected"), e->execute("reflect::type_of(Hold<shared_ptr<Goblin>>()) == \"Hold<shared_ptr<Goblin>>\" ? \"reflected\" : \"no\";").as_string());
			// a handle to a class the bound refuses is refused as the class is; a weak_ptr names no object until it is locked
			check_true(contains(failure(*e, "shared_ptr<Rock> k = new Rock(); ident(k);"), "ident<shared_ptr<Rock>>: 'shared_ptr<Rock>' does not derive from 'Entity', which T requires"));
			check_true(contains(failure(*e, "Hold<shared_ptr<Rock>> h;"), "Hold<shared_ptr<Rock>>: 'shared_ptr<Rock>' does not derive from 'Entity', which T requires"));
			const std::string weak = "'weak_ptr<Goblin>' does not derive from 'Entity', which T requires: a weak_ptr names no object until it is locked; pass w.lock()";
			check_true(contains(failure(*e, "weak_ptr<Goblin> w = g; ident(w);"), "ident<weak_ptr<Goblin>>: " + weak));
			check_true(contains(failure(*e, "Hold<weak_ptr<Goblin>> h;"), "Hold<weak_ptr<Goblin>>: " + weak));
			check_eq((int64_t)4, e->execute("weak_ptr<Goblin> w = g; ident<shared_ptr<Goblin>>(w.lock());").as_int());
			// in a bound that names the parameter, a handle reads as its class: shared_ptr<Num> meets T : Comparable<T> as Num does
			e->execute("class Comparable<T> { bool before(T other) { return true; } } class Num : Comparable<Num> { int n = 0; } bool sorted<T : Comparable<T>>(T a, T b) { return a.before(b); } 1;");
			check_true(e->execute("shared_ptr<Num> x = new Num(); shared_ptr<Num> y = new Num(); sorted(x, y) && sorted(Num(), Num());").as_bool());
			check_true(contains(failure(*e, "shared_ptr<Rock> a = new Rock(); sorted(a, a);"), "sorted<shared_ptr<Rock>>: 'shared_ptr<Rock>' does not derive from 'Comparable<Rock>', which T requires"));
		});

		test("a_bound_that_names_no_class_says_so_where_the_instance_is_asked_for", [this]() {
			auto e = stdlib_engine();
			e->execute("class Real { int n = 0; } class A<T : Nope> { T v; } int f<T : Nope>(T x) { return 1; } namespace ns { class B<U : Gone> { U v; } } 1;");
			const std::string a = failure(*e, "A<int> a;");
			check_true(contains(a, "<script>:1:2: error: A<int>: the bound of T, 'Nope', is no declared class: declare it, or bound T by a class that is (T : Base)"), a);
			check_true(contains(failure(*e, "A<Real> a;"), "A<Real>: the bound of T, 'Nope', is no declared class"));
			check_true(contains(failure(*e, "f(Real());"), "f<Real>: the bound of T, 'Nope', is no declared class: declare it, or bound T by a class that is (T : Base)"));
			check_true(contains(failure(*e, "ns::B<Real> b;"), "ns::B<Real>: the bound of U, 'Gone', is no declared class: declare it, or bound U by a class that is (U : Base)"));
			check_eq((int64_t)2, e->execute("class Nope { int n = 2; } class Kin : Nope {} A<Kin> k; k.v.n;").as_int());   // declared later, it bounds
		});

		test("a_parameter_bounded_by_itself_fails_where_the_generic_is_declared", [this]() {
			auto e = make_engine();
			check_true(contains(failure(*e, "class A<T : T> { T v; }\n1;"), "<script>:1:13: error: 'T' is bounded by itself in 'A': a bound names the class T must be or derive from (T : Base)"));
			check_true(contains(failure(*e, "int f<T : T>(T x) { return 1; }"), "'T' is bounded by itself in 'f': a bound names the class T must be or derive from (T : Base)"));
			check_true(contains(failure(*e, "class C { static void m<U : U>() {} }"), "'U' is bounded by itself in 'm': a bound names the class U must be or derive from (U : Base)"));
			check_true(contains(failure(*e, "namespace n { class P<K, V : V> { K k; } }"), "'V' is bounded by itself in 'P'"));
			// a bound that names another parameter is no bound by itself
			check_eq((int64_t)3, e->execute("class Base { int n = 3; } class Sub : Base {} class Two<T, U : T> { U u; } Two<Base, Sub> t; t.u.n;").as_int());
		});

		// ---- generic methods

		test("a_generic_method_is_a_method_per_argument_list", [this]() {
			auto e = stdlib_engine();
			e->execute(declarations());
			e->execute(R"(
				class Vault {
					string body = "{\"sku\": \"tee\", \"count\": 2}";
					T json<T>() { return from_json<T>(body); }
					static T make<T>() { return T(); }
					T twice<T>(T x) { return x + x; }
					T again<T>() { return json<T>(); }                 // a bare call reaches the class's own generic
					T reveal<T>() { return cast<T>(secret); }          // a method of the class: its private members are its own
				private:
					int secret = 5;
					T hidden<T>(T x) { return x; }
				}
			)");
			check_eq(std::string("tee:2"), e->execute("Vault v; CartForm f = v.json<CartForm>(); f.sku + \":\" + to_string(f.count);").as_string());
			check_eq(std::string("tee"), e->execute("Request req; req.body = \"{\\\"sku\\\": \\\"tee\\\"}\"; CartForm f = req.json<CartForm>(); f.sku;").as_string());
			check_eq((int64_t)0, e->execute("Vault::make<CartForm>().count + Vault::make<int>();").as_int());
			check_eq(std::string("8abab"), e->execute("Vault v; to_string(v.twice(4)) + v.twice<string>(\"ab\");").as_string());   // deduced, then written
			check_eq(std::string("tee"), e->execute("Vault v; v.again<CartForm>().sku;").as_string());
			check_eq((int64_t)5, e->execute("Vault v; v.reveal<int>();").as_int());
			check_eq((int64_t)2, e->execute("shared_ptr<Vault> v = new Vault(); v.json<CartForm>().count;").as_int());
			check_true(contains(failure(*e, "Vault v; v.hidden<int>(1);"), "private"));
			check_true(contains(failure(*e, "Vault v; v.json();"), "cannot deduce T for 'json': no parameter names it; write json<T>()"));
		});

		test("a_generic_methods_instance_is_named_bare_with_its_type_arguments_wherever_methods_are_listed", [this]() {
			auto e = stdlib_engine();
			e->execute(R"(
				class Plain {
					int n = 1;
					T conv<T>(T x) { return x; }
					int other() { return 2; }
					static T mk<T>() { return T(); }
					int open() { return hid<int>(4); }
				private:
					T hid<T>(T x) { return x; }
				}
				class Pair<K, V> { K first; T conv<T>(T x) { return x; } }
				class Kid : Plain { T conv<T>(T x) { return x + x; } }
				namespace zoo { class Pen<T> { T held; U take<U>(U x) { return x; } } }
				Plain p; p.conv(1); p.conv("s"); Plain::mk<int>(); p.open();
				Pair<string, int> q; q.conv(1);
				Kid k; k.conv(1);
				zoo::Pen<int> pen; pen.take(2);
				string names(var of) { string out; for (auto m : reflect::methods(of)) { out += m["name"] + ":" + m["from"] + ":" + m["access"] + " "; } return out; }
			)");
			check_eq(std::string("conv<int>:Plain:public conv<string>:Plain:public hid<int>:Plain:private mk<int>:Plain:public open:Plain:public other:Plain:public "),
			         e->execute("names(p);").as_string());
			check_eq(e->execute("names(p);").as_string(), e->execute("names(\"Plain\");").as_string());
			check_eq(std::string("conv<int>:Pair<string, int>:public "), e->execute("names(q);").as_string());
			const std::string kid = e->execute("names(k);").as_string();   // the derived class's instance hides the base's
			check_true(contains(kid, "conv<int>:Kid:public ") && !contains(kid, "conv<int>:Plain"), kid);
			check_eq(std::string("take<int>:zoo::Pen<int>:public "), e->execute("names(pen);").as_string());
			check_eq(std::string("[[{\"default\": false, \"name\": \"x\", \"type\": \"int\"}]]"), e->execute("to_string(reflect::method_arguments(\"Plain\", \"conv<int>\"));").as_string());
			check_eq(std::string("[[{\"default\": false, \"name\": \"x\", \"type\": \"int\"}]]"), e->execute("to_string(reflect::method_arguments(q, \"conv<int>\"));").as_string());
			check_eq(std::string("true true false"), e->execute("to_string(reflect::has_method(p, \"conv<int>\")) + \" \" + to_string(reflect::has_method(k, \"conv<int>\")) + \" \" + to_string(reflect::has_method(p, \"conv<float>\"));").as_string());
			check_eq((int64_t)19, e->execute("reflect::bind(p, \"conv<int>\")(5) + reflect::invoke(p, \"conv<int>\", 6) + reflect::invoke(\"Plain\", \"mk<int>\") + reflect::bind(k, \"conv<int>\")(4);").as_int());
			check_true(contains(failure(*e, "reflect::method_arguments(\"Plain\", \"conv<float>\");"), "reflect::method_arguments: 'Plain' has no method 'conv<float>'"));
			check_eq(std::string("false"), e->execute("to_string(reflect::has_method(p, \"Plain::conv<int>\"));").as_string());   // the name it is declared under is no name it lists
			// the private-access error names the method as the class lists it
			const std::string denied = failure(*e, "p.hid<int>(1);");
			check_true(contains(denied, "Cannot access private member 'hid<int>' of class 'Plain'"), denied);
		});

		test("a_text_about_a_call_names_a_generic_methods_instance_as_its_class_lists_it", [this]() {
			// An instance is declared under its class's name (Tool::scaled<int>) and named as a plain method is, bare: by the
			// errors a call raises, by the frames of a stack trace and by the load's texts about a call
			const std::string prelude = R"(
				class Tool {
					int level = 1;
					T scaled<T>(T x) { return x * level; }
					T loose<T>(var x) { return x; }
					T bump<T>(T x) { level += 1; return x; }
					T boom<T>(T x) { throw "bang"; return x; }
					T half<T>(T x) { return 10 / x; }
					static T made<T>(T x) { return x; }
				}
				class Sub : Tool {}
				class Pair<A, B> { A a; T conv<T>(T x) { throw "deep"; return x; } }
				namespace kit { class Box { T keep<T>(T x) { throw "inside"; return x; } } }
			)";
			struct row { std::string statement, says, trace; };
			const std::vector<row> rows = {
				{"shared_ptr<Tool> p; p.scaled<int>(4);", "Cannot call 'scaled<int>' on null ('p' is null)", ""},
				{"shared_ptr<Sub> p; p.scaled<int>(4);", "Cannot call 'scaled<int>' on null ('p' is null)", ""},
				{"shared_ptr<Pair<string, int>> p; p.conv<int>(1);", "Cannot call 'conv<int>' on null ('p' is null)", ""},
				{"Tool t; var a = \"x\"; t.scaled<int>(a);", "Cannot pass string to parameter 'x' of 'scaled<int>' (declared int)", "at scaled<int> ("},
				{"var a = \"x\"; Tool::made<int>(a);", "Cannot pass string to parameter 'x' of 'made<int>' (declared int)", "at made<int> ("},
				{"Tool t; t.loose<int>(\"s\");", "Cannot return string from 'loose<int>' (declared -> int)", "at loose<int> ("},
				{"Tool t; t.scaled<int>(1); var f = reflect::bind(t, \"scaled<int>\"); f(1, 2);", "'scaled<int>' expects 1 argument, got 2", ""},
				{"Tool t; t.scaled<int>(1); reflect::invoke(t, \"scaled<int>\", \"x\");", "Cannot pass string to parameter 'x' of 'scaled<int>' (declared int)", "at scaled<int> ("},
				{"Tool t; array<int> out = [0, 0, 0, 0]; parallel_for (auto& x : out) { x = t.bump<int>(x); }", "method 'bump<int>' of 'Tool' writes its object", ""},
				{"Tool t; t.boom<int>(1);", "bang", "at boom<int> ("},
				{"int go(int z) { Tool t; return t.half<int>(z); } go(0);", "ivision by zero", "at half<int> ("},
				{"Pair<string, int> p; p.conv<int>(1);", "deep", "at conv<int> ("},
				{"kit::Box b; b.keep<int>(1);", "inside", "at keep<int> ("},
				// the load's texts about a call
				{"Tool t; t.scaled<int>(\"x\");", "argument 1 of 'scaled<int>': cannot pass 'string' to 'x' declared 'int'", ""},
				{"Tool::made<int>(\"x\");", "argument 1 of 'made<int>': cannot pass 'string' to 'x' declared 'int'", ""},
				{"const Tool t; t.bump<int>(1);", "cannot call 'bump<int>' on 't': 't' is const and 'bump<int>' writes its receiver", ""},
				{"class R { T back<T>(T x) { return \"s\"; } } R r; r.back<int>(1);", "cannot return 'string' from 'back<int>' declared to return 'int'", ""},
			};
			for (const row& r : rows) {
				auto e = stdlib_engine();
				e->execute(prelude);
				const std::string got = failure(*e, r.statement);
				check_true(contains(got, r.says), r.statement + " | " + got);
				const std::string trace = r.trace.empty() ? std::string() : e->format_stack_trace();
				check_true(contains(trace, r.trace), r.statement + " | " + trace);
				for (const char* declared : {"Tool::", "Pair<string, int>::", "kit::", "Box::", "R::"}) {   // no text quotes the declared name, no frame leads with it
					check_true(!contains(got, std::string("'") + declared), r.statement + " | " + got);
					check_true(!contains(trace, std::string("at ") + declared), r.statement + " | " + trace);
				}
			}
		});

		test("the_receivers_class_picks_a_generic_methods_instance", [this]() {
			auto e = stdlib_engine();
			e->execute(R"(
				class Base { string tag<T>() { return "base"; } string kept<T>() { return "kept"; } }
				class Derived : Base { string tag<T>() { return "derived"; } }
				class Box<T> { T item; U apply<U>(function f) { return f(item); } static string kind<U>() { return "box"; } }
			)");
			// a derived class's generic of the name hides the base's; one it does not declare is the base's
			check_eq(std::string("base|derived|kept"), e->execute("Base b; Derived d; b.tag<int>() + \"|\" + d.tag<int>() + \"|\" + d.kept<int>();").as_string());
			// not virtual: the class the load knows picks the instance
			check_eq(std::string("base"), e->execute("Derived d; Base& b = d; b.tag<int>();").as_string());
			check_eq(std::string("4!box"), e->execute("Box<int> b; b.item = 4; b.apply<string>((x) => to_string(x) + \"!\") + Box<int>::kind<float>();").as_string());
			check_true(contains(failure(*e, "var b = Base(); b.tag<int>();"), "cannot instantiate 'tag<int>': the receiver's class is not known at load"));
			check_true(contains(failure(*e, "class A { T m<T>() { return T(); } T m<U>() { return U(); } }"), "generic 'A::m<U>' is declared again: line 1 declares it with the same parameters"));
		});

		test("a_generic_method_called_through_a_receiver_of_unknown_class_says_what_to_write", [this]() {
			auto e = stdlib_engine();
			e->execute("class Base { T twice<T>(T x) { return x + x; } int plain() { return 1; } } class Derived : Base {}");
			const std::string text = "Base::twice is a generic method: its instance is picked where the call is written, from the receiver's class, which is not known there; "
				"give the receiver a typed name (Base x = ...) or cast it (cast<Base&>(x).twice(...))";
			check_eq(text, failure(*e, "var b = Base(); b.twice(4);").substr(0, text.size()));
			// the instance exists by then (another call made it): the receiver's class is still what picks it
			check_eq(text, failure(*e, "Base typed; typed.twice(4); var b = Base(); b.twice(4);").substr(0, text.size()));
			check_true(contains(failure(*e, "var d = Derived(); d.twice(4);"),
				"Base::twice is a generic method: its instance is picked where the call is written, from the receiver's class, which is not known there; "
				"give the receiver a typed name (Derived x = ...) or cast it (cast<Derived&>(x).twice(...))"));
			// what the text says to write
			check_eq((int64_t)8, e->execute("var b = Base(); Base typed = b; typed.twice(4);").as_int());
			check_eq((int64_t)8, e->execute("var b = Base(); cast<Base&>(b).twice(4);").as_int());
			// a member the class does not have is what it was
			check_true(contains(failure(*e, "var b = Base(); b.nope(4);"), "Base has no member 'nope'"));
			check_eq((int64_t)1, e->execute("var b = Base(); b.plain();").as_int());
		});

		test("units_that_call_a_generic_methods_instance_share_it", [this]() {
			const auto directory = unit_directory("method_units");
			write_unit(directory, "counter.jai", "class Counter {\n\tint n = 0;\n\tT bump<T>(T by) { n += 1; return by; }\n}\nCounter shared_counter;\n");
			write_unit(directory, "first.jai", "import \"counter.jai\";\nint first_bump() { return shared_counter.bump<int>(1); }\n");
			write_unit(directory, "second.jai", "import \"counter.jai\";\nint second_bump() { return shared_counter.bump<int>(2); }\n");
			const std::string root = write_unit(directory, "root.jai",
				"import \"first.jai\";\nimport \"second.jai\";\nto_string(first_bump() + second_bump()) + \":\" + to_string(shared_counter.n);\n");
			auto e = stdlib_engine();
			check_eq(std::string("3:2"), e->execute_file(root).as_string());
		});

		test("a_generic_method_is_called_from_a_function_above_its_class", [this]() {
			auto e = stdlib_engine();
			// the instance is declared ahead of the function that first calls it; the class takes it when its own declaration runs
			check_eq((int64_t)7, e->execute(R"(
				int ask(Oracle o) { return o.answer<int>(3); }
				class Oracle { int base = 4; T answer<T>(T x) { return x + base; } }
				ask(Oracle());
			)").as_int());
			check_true(detail::engine_access::get_class_registry(*e).waiting_instances_.empty(), "the class took what waited for it");
		});

		test("a_generic_method_is_called_from_a_function_above_its_classs_import", [this]() {
			const auto directory = unit_directory("method_above_import");
			const std::string oracle = write_unit(directory, "oracle.jai", "class Oracle {\n\tint base = 4;\n\tT answer<T>(T x) { return x + base; }\n\tstatic T zero<T>() { return T(); }\n}\n");
			write_unit(directory, "user.jai", "int ask(Oracle o) { return o.answer<int>(3) + Oracle::zero<int>(); }\nimport \"oracle.jai\";\n");
			auto e = stdlib_engine();
			e->add_include_path(directory.string());
			e->execute("import \"user.jai\";");
			check_eq((int64_t)7, e->execute("ask(Oracle());").as_int());
			check_true(detail::engine_access::get_class_registry(*e).waiting_instances_.empty());
			// the class's file again: the class holds the instances its caller declared
			auto stamp = std::filesystem::last_write_time(oracle);
			write_unit(directory, "oracle.jai", "class Oracle {\n\tint base = 10;\n\tT answer<T>(T x) { return x + base; }\n\tstatic T zero<T>() { return T(); }\n}\n");
			std::filesystem::last_write_time(oracle, stamp + std::chrono::seconds(2));
			check_true(e->execute("import(\"user.jai\");").as_bool());
			check_eq((int64_t)13, e->execute("ask(Oracle());").as_int());
			check_true(detail::engine_access::get_class_registry(*e).waiting_instances_.empty());
		});

		test("a_call_ahead_of_the_class_fails_as_any_use_of_a_class_not_yet_declared", [this]() {
			auto e = stdlib_engine();
			// the instances wait for the class; a call that runs first finds no class, as a call of a plain method does
			check_eq(std::string("Class 'Oracle' not found|No constructor found for class 'Oracle'|0|7"), e->execute(R"(
				int zero() { return Oracle::zero<int>(); }
				int ask() { Oracle o; return o.answer<int>(3); }
				string early = "";
				try { zero(); } catch (e) { early += e; }
				early += "|";
				try { ask(); } catch (e) { early += e; }
				class Oracle { int base = 4; T answer<T>(T x) { return x + base; } static T zero<T>() { return T(); } }
				early + "|" + to_string(zero()) + "|" + to_string(ask());
			)").as_string());
		});

		test("an_instance_whose_class_is_never_declared_waits_as_one_entry", [this]() {
			auto e = stdlib_engine();
			auto& awaited = detail::engine_access::get_class_registry(*e).waiting_instances_;
			const std::string stopped = R"(
				int ask(Oracle o) { return o.answer<int>(3); }
				throw "stopped";
				class Oracle { int base = 4; T answer<T>(T x) { return x + base; } }
			)";
			for (int run = 0; run < 3; ++run) {   // each run declares the instance again: one entry, whatever the count of runs
				check_true(contains(failure(*e, stopped), "stopped"));
				check_eq((size_t)1, awaited.size());
			}
			check_true(contains(failure(*e, "Oracle o;"), "unknown type 'Oracle'"), "the instance that waits makes no class");
			// the class, declared at last, takes it: the function the stopped unit left calls the method
			e->execute("class Oracle { int base = 4; T answer<T>(T x) { return x + base; } }");
			check_true(awaited.empty());
			check_eq((int64_t)7, e->execute("ask(Oracle());").as_int());
		});

		test("a_class_declared_again_keeps_the_instances_that_waited_for_it", [this]() {
			auto e = stdlib_engine();
			e->execute(R"(
				int ask(Oracle o) { return o.answer<int>(3); }
				class Oracle { int base = 4; T answer<T>(T x) { return x + base; } }
			)");
			e->execute("class Oracle { int base = 5; T answer<T>(T x) { return x + base; } }");
			check_eq((int64_t)8, e->execute("ask(Oracle());").as_int());
		});

		test("a_class_declared_again_keeps_its_generic_methods_instances", [this]() {
			auto e = stdlib_engine();
			e->execute("class Tool { int level = 1; T scaled<T>(T x) { return x * level; } }");
			e->execute("Tool tool; int use() { return tool.scaled<int>(3); }");
			check_eq((int64_t)3, e->execute("use();").as_int());
			e->execute("class Tool { int level = 2; T scaled<T>(T x) { return x * level; } }");   // the class again: the instance its caller declared stays
			check_eq((int64_t)6, e->execute("Tool fresh; fresh.scaled<int>(3);").as_int());
		});

		// ---- the rest of the engine sees ordinary declarations

		test("an_edit_to_a_generic_reruns_the_modules_that_instantiate_it", [this]() {
			const auto directory = unit_directory("reload");
			const std::string lib = write_unit(directory, "lib.jai", "T tagged<T>(T x) { return x; }\nclass Box<T> {\n\tT item;\n\tint size() { return 1; }\n}\n");
			write_unit(directory, "user.jai", "import \"lib.jai\";\nuser_ran();\nBox<int> box;\nint user_value() { return tagged<int>(1) + box.size(); }\n");
			auto e = stdlib_engine();
			e->add_include_path(directory.string());
			int runs = 0;
			e->add_function("user_ran", [&runs]() { ++runs; });
			e->execute("import \"user.jai\";");
			check_eq((int64_t)2, e->execute("user_value();").as_int());
			check_eq(1, runs);
			check_false(e->execute("import(\"user.jai\");").as_bool(), "nothing moved: nothing re-runs");
			// the generic's file is part of the module that instantiates it: its edit re-runs that module
			auto stamp = std::filesystem::last_write_time(lib);
			write_unit(directory, "lib.jai", "T tagged<T>(T x) { return x + 10; }\nclass Box<T> {\n\tT item;\n\tint size() { return 5; }\n}\n");
			std::filesystem::last_write_time(lib, stamp += std::chrono::seconds(3));
			check_true(e->execute("import(\"user.jai\");").as_bool());
			check_eq(2, runs);
			check_eq((int64_t)16, e->execute("user_value();").as_int());
			// a broken edit is reported and the old code keeps running
			write_unit(directory, "lib.jai", "T tagged<T>(T x) { return x.nope; }\nclass Box<T> {\n\tT item;\n\tint size() { return 5; }\n}\n");
			std::filesystem::last_write_time(lib, stamp += std::chrono::seconds(3));
			const std::string broken = failure(*e, "import(\"user.jai\");");
			check_true(contains(broken, "int has no member 'nope'") && contains(broken, "in tagged<int>, instantiated at"));
			check_eq(2, runs);
			check_eq((int64_t)16, e->execute("user_value();").as_int());
		});

		// ---- a reload and the instances the loaded units use (docs/site/embedding/07-hot-reload.html "Callers in other files")

		test("the_host_reloads_a_generics_file_while_it_holds_instances", [this]() {
			const auto directory = unit_directory("host_reload");
			const auto lib_text = [](const std::string& size, const std::string& extra_field, const std::string& twice_body) {
				return "class Box<T> {\n\tT item;\n" + extra_field + "\tint size() { return " + size + "; }\n\tfunction sizer() { return [this]() { return size(); }; }\n}\n"
				       "T twice<T>(T x) { return " + twice_body + "; }\n";
			};
			const std::string lib = write_unit(directory, "lib.jai", lib_text("1", "", "x + x"));
			write_unit(directory, "game.jai",
				"import \"lib.jai\";\nBox<int> box;\nBox<string> words;\n"
				"int tick(int frame) { import(\"lib.jai\"); return twice<int>(frame) * 100 + box.size() * 10 + words.size(); }\n");
			auto e = stdlib_engine();
			e->add_include_path(directory.string());
			e->import_recheck_interval(0);
			e->hot_reload_reporting(true);
			e->execute_file((directory / "game.jai").string());
			held_value tick = e->get_variable("tick");
			e->execute("auto sizer = box.sizer(); auto bound = reflect::bind(box, \"size\");");
			held_value sizer = e->get_variable("sizer");
			held_value bound = e->get_variable("bound");
			// the host's call of a function value: its result, or the failure its checked_result carries
			const auto call = [&](const held_value& fn, const std::vector<script_value>& args = {}) {
				auto result = fn.as_function()(args);
				return result ? result.value().to_string() : "ERR: " + e->format_error(result);
			};
			const auto frame = [&] { return call(tick, {e->make_value(script_int(1))}); };
			const auto held = [&] { return call(sizer) + "|" + call(bound) + "|" + e->execute("to_string(words.size());").as_string(); };
			const auto report_rows = [&] {
				std::string rows;
				for (const auto& row : e->hot_reload_reports()) { rows += row.class_name + "." + row.field_name + " x" + std::to_string(row.count) + ";"; }
				return rows;
			};
			auto stamp = std::filesystem::last_write_time(lib);
			const auto edit = [&](const std::string& text) {
				write_unit(directory, "lib.jai", text);
				std::filesystem::last_write_time(lib, stamp += std::chrono::seconds(3));
			};
			const auto bytes_of = [](const std::filesystem::path& path) {
				std::ifstream in(path, std::ios::binary);
				return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			};
			check_eq(std::string("211"), frame());
			check_eq(std::string("1|1|1"), held());
			// an edit every instance can be made from: new bodies and a new field
			const std::string added = "\tint added = 9;\n";
			edit(lib_text("5", added, "x + x + x"));
			check_eq(std::string("355"), frame());
			check_eq(std::string("5|5|5"), held(), "the closure and the bound method the host holds call the new body");
			check_eq((int64_t)9, e->execute("box.added;").as_int());
			const std::string rows = report_rows();
			check_true(contains(rows, "Box<int>.added x1;") && contains(rows, "Box<string>.added x1;"), "the reload report names each instance: " + rows);
			const std::string cache = bytes_of(jaibite_cache_beside(lib));
			// An edit one of the root unit's instances cannot be made from rejects the reload, as a function its caller no longer
			// fits does: the error is the instance's, nothing is applied, and every instance and function keeps the code it had
			struct broken_edit {
				std::string what, text;
				std::vector<std::string> says;
			};
			const std::string sizer_text = "\tfunction sizer() { return [this]() { return size(); }; }\n";
			const std::vector<broken_edit> broken = {
				{"a syntax error in the generic", "class Box<T> {\n\tT item;\n\tint size() { return 5 +; }\n}\nT twice<T>(T x) { return x; }\n", {"lib.jai:3:"}},
				{"a body that does not check for one instance", "class Box<T> {\n\tT item;\n" + added + "\tint size() { return item.size(); }\n" + sizer_text + "}\nT twice<T>(T x) { return x; }\n",
					{"lib.jai:4:27: error: int has no member 'size'", "    in Box<int>, instantiated at ", "game.jai:2:4"}},
				{"another parameter count", "class Box<K, V> {\n\tK item;\n" + added + "\tint size() { return 7; }\n" + sizer_text + "}\nT twice<T>(T x) { return x; }\n",
					{"game.jai:2:4: error: 'Box' takes 2 type arguments, not 1", "game.jai:3:4: error: 'Box' takes 2 type arguments, not 1"}},
				{"a bound the instances do not meet", "class Limit { int n = 0; }\nclass Box<T : Limit> {\n\tT item;\n" + added + "\tint size() { return 7; }\n" + sizer_text + "}\nT twice<T>(T x) { return x; }\n",
					{"game.jai:2:4: error: Box<int>: 'int' does not derive from 'Limit', which T requires", "game.jai:3:4: error: Box<string>: 'string' does not derive from 'Limit', which T requires"}},
				{"a class of the generic's name", "class Box {\n\tint item;\n" + added + "\tint size() { return 7; }\n" + sizer_text + "}\nT twice<T>(T x) { return x; }\n",
					{"game.jai:2:4: error: 'Box' takes no type arguments: it is not a generic class"}},
				{"a function body that does not check for its instance", lib_text("7", added, "x.size()"),
					{"lib.jai:7:28: error: int has no member 'size'", "    in twice<int>, instantiated at ", "game.jai:4:65"}},
				{"a function of the generic function's name", "class Box<T> {\n\tT item;\n" + added + "\tint size() { return 7; }\n" + sizer_text + "}\nstring twice(string x) { return x; }\n",
					{"game.jai:4:65: error: 'twice' takes no type arguments: it is not a generic function"}},
				{"another type parameter count for the function", "class Box<T> {\n\tT item;\n" + added + "\tint size() { return 7; }\n" + sizer_text + "}\nT twice<T, U>(T x) { return x; }\n",
					{"game.jai:4:65: error: cannot deduce U for 'twice': no parameter names it; write twice<int, U>(...)"}},
			};
			for (const auto& [what, text, says] : broken) {
				edit(text);
				const std::string rejected = frame();
				check_true(rejected.rfind("ERR: ", 0) == 0, what + ": the frame reports the rejected reload: " + rejected);
				for (const auto& part : says) { check_true(contains(rejected, part), what + ": " + part + " in: " + rejected); }
				check_eq(std::string("355"), frame(), what + ": the next frame runs the code it had");
				check_eq(std::string("5|5|5"), held(), what + ": what the host holds, and the other instance, are as they were");
				check_eq((int64_t)9, e->execute("box.added;").as_int(), what);
				check_eq(rows, report_rows(), what + ": a rejected reload migrates nothing");
				check_eq(cache, bytes_of(jaibite_cache_beside(lib)), what + ": no cache holds the rejected text");
			}
			// the same shape with a plain function: the root's caller no longer fits, and the reload is refused
			{
				const auto plain_directory = unit_directory("host_reload_plain");
				const std::string plain_lib = write_unit(plain_directory, "plain_lib.jai", "int size_of(int x) { return x; }\n");
				const std::string plain_game = write_unit(plain_directory, "plain_game.jai", "import \"plain_lib.jai\";\nint plain_tick(int frame) { import(\"plain_lib.jai\"); return size_of(frame); }\n");
				auto p = stdlib_engine();
				p->add_include_path(plain_directory.string());
				p->import_recheck_interval(0);
				p->execute_file(plain_game);
				held_value plain_tick = p->get_variable("plain_tick");
				const auto plain_frame = [&] {
					auto result = plain_tick.as_function()({p->make_value(script_int(3))});
					return result ? result.value().to_string() : "ERR: " + p->format_error(result);
				};
				check_eq(std::string("3"), plain_frame());
				auto plain_stamp = std::filesystem::last_write_time(plain_lib);
				write_unit(plain_directory, "plain_lib.jai", "int size_of(string x) { return 9; }\n");
				std::filesystem::last_write_time(plain_lib, plain_stamp += std::chrono::seconds(3));
				const std::string plain_rejected = plain_frame();
				check_true(plain_rejected.rfind("ERR: ", 0) == 0 && contains(plain_rejected, "plain_game.jai:2:74: error: argument 1 of 'size_of': cannot pass 'int' to 'x' declared 'string'"), plain_rejected);
				check_eq(std::string("3"), plain_frame());
			}
			// the fix applies
			edit(lib_text("2", added, "x"));
			check_eq(std::string("122"), frame());
			check_eq(std::string("2|2|2"), held());
			// The host runs the generic's file itself: a file that already ran is a reload of that unit, so the instance a loaded
			// unit names and the text cannot make rejects it, and nothing is applied
			edit("class Box<T> {\n\tT item;\n" + added + "\tint size() { return item.size(); }\n" + sizer_text + "}\nT twice<T>(T x) { return x + x; }\n");
			std::string refused;
			try {
				e->execute_file(lib);
			} catch (const std::exception& error) {
				refused = error.what();
			}
			check_true(contains(refused, "lib.jai:4:27: error: int has no member 'size'\n    in Box<int>, instantiated at ") && contains(refused, "game.jai:2:4"), refused);
			check_eq(std::string("2|2|2"), held(), "nothing applied");
			// the module's import meets the same text: rejected once on that route too, then it waits
			const std::string import_refused = frame();
			check_true(contains(import_refused, "lib.jai:4:27: error: int has no member 'size'"), import_refused);
			check_eq(std::string("122"), frame());
			check_eq(std::string("122"), frame());
		});

		test("a_reload_gives_an_instances_users_the_verdict_it_gives_a_functions_callers", [this]() {
			// reload: 'r' the units that use it no longer fit the new text; 'a' they fit; 'd' the new text drops the name
			struct shape {
				std::string name, v1, user, v2;
				char reload;
				bool keeps;       // declared again by any other route, the instance keeps its earlier declaration with a warning
				bool plain;
				std::string says;
			};
			const std::string limit = "class Limit { int n = 0; }\n";
			const std::string box_v1 = limit + "class Box<T> {\n\tT item;\n\tint size() { return 1; }\n}\n";
			const std::string box_user = "Box<int> box;\nBox<string> words;\nint use() { return box.size() * 10 + words.size(); }\n";
			const std::string fn_v1 = limit + "T twice<T>(T x) { return x; }\n";
			const std::string fn_user = "int use() { return twice<int>(4); }\n";
			const std::string method_v1 = limit + "class Tool {\n\tint level = 1;\n\tT scaled<T>(T x) { return x; }\n}\n";
			const std::string method_user = "Tool tool;\nint use() { return tool.scaled<int>(4); }\n";
			const std::string plain_v1 = "int size_of(int x) { return 1; }\nclass Plain {\n\tint item;\n\tint size() { return 1; }\n}\n";
			const std::string plain_user = "Plain p;\nint use() { return size_of(3) + p.size(); }\n";
			const auto tool = [&](const std::string& member) { return limit + "class Tool {\n\tint level = 1;\n" + member + "}\n"; };
			const std::vector<shape> shapes = {
				{"a function, edited", plain_v1, plain_user, "int size_of(int x) { return 2; }\nclass Plain {\n\tint item;\n\tint size() { return 2; }\n}\n", 'a', false, true, ""},
				{"a function, another parameter type", plain_v1, plain_user, "int size_of(string x) { return 2; }\nclass Plain {\n\tint item;\n\tint size() { return 1; }\n}\n", 'r', false, true,
					"error: argument 1 of 'size_of': cannot pass 'int' to 'x' declared 'string'"},
				{"a function, another parameter count", plain_v1, plain_user, "int size_of(int x, int y) { return 2; }\nclass Plain {\n\tint item;\n\tint size() { return 1; }\n}\n", 'r', false, true,
					"error: 'size_of' expects 2 argument(s), got 1"},
				{"a class, its method gone", plain_v1, plain_user, "int size_of(int x) { return 1; }\nclass Plain {\n\tint item;\n}\n", 'r', false, true, "error: no member 'size' in class 'Plain'"},
				{"a function and a class, dropped", plain_v1, plain_user, "int unrelated() { return 2; }\n", 'd', false, true, ""},
				{"a generic class, edited", box_v1, box_user, limit + "class Box<T> {\n\tT item;\n\tint size() { return 2; }\n}\n", 'a', false, false, ""},
				{"a generic class, a body that does not check for int", box_v1, box_user, limit + "class Box<T> {\n\tT item;\n\tint size() { return item.size() + 2; }\n}\n", 'r', true, false,
					"lib.jai:4:27: error: int has no member 'size'\n    in Box<int>, instantiated at "},
				{"a generic class, a bound its arguments do not meet", box_v1, box_user, limit + "class Box<T : Limit> {\n\tT item;\n\tint size() { return 2; }\n}\n", 'r', true, false,
					"error: Box<int>: 'int' does not derive from 'Limit', which T requires"},
				{"a generic class, another parameter count", box_v1, box_user, limit + "class Box<K, V> {\n\tK item;\n\tint size() { return 2; }\n}\n", 'r', true, false,
					"error: 'Box' takes 2 type arguments, not 1"},
				{"a generic class, a class of its name", box_v1, box_user, limit + "class Box {\n\tint item;\n\tint size() { return 2; }\n}\n", 'r', true, false,
					"error: 'Box' takes no type arguments: it is not a generic class"},
				{"a generic class, dropped", box_v1, box_user, limit, 'd', false, false, ""},
				{"a generic function, edited", fn_v1, fn_user, limit + "T twice<T>(T x) { return x + x; }\n", 'a', false, false, ""},
				{"a generic function, a body that does not check for int", fn_v1, fn_user, limit + "T twice<T>(T x) { return x.size(); }\n", 'r', true, false,
					"lib.jai:2:28: error: int has no member 'size'\n    in twice<int>, instantiated at "},
				{"a generic function, a bound its argument does not meet", fn_v1, fn_user, limit + "T twice<T : Limit>(T x) { return x; }\n", 'r', true, false,
					"error: twice<int>: 'int' does not derive from 'Limit', which T requires"},
				{"a generic function, another type parameter count", fn_v1, fn_user, limit + "T twice<T, U>(T x) { return x + x; }\n", 'r', false, false,
					"error: cannot deduce U for 'twice': no parameter names it; write twice<int, U>(...)"},
				{"a generic function, another value parameter count", fn_v1, fn_user, limit + "T twice<T>(T x, T y) { return x + y; }\n", 'r', false, false, "error: 'twice<T>' takes 2 arguments, not 1"},
				{"a generic function, a function of its name", fn_v1, fn_user, limit + "string twice(string x) { return x; }\n", 'r', false, false,
					"error: 'twice' takes no type arguments: it is not a generic function"},
				{"a generic function, dropped", fn_v1, fn_user, limit, 'd', false, false, ""},
				{"a generic method, edited", method_v1, method_user, tool("\tT scaled<T>(T x) { return x + x; }\n"), 'a', false, false, ""},
				{"a generic method, a body that does not check for int", method_v1, method_user, tool("\tT scaled<T>(T x) { return x.size(); }\n"), 'r', true, false,
					"lib.jai:4:30: error: int has no member 'size'\n    in Tool::scaled<int>, instantiated at "},
				{"a generic method, a bound its argument does not meet", method_v1, method_user, tool("\tT scaled<T : Limit>(T x) { return x; }\n"), 'r', true, false,
					"error: Tool::scaled<int>: 'int' does not derive from 'Limit', which T requires"},
				{"a generic method, another type parameter count", method_v1, method_user, tool("\tT scaled<T, U>(T x) { return x + x; }\n"), 'r', false, false,
					"error: cannot deduce U for 'scaled': no parameter names it; write scaled<int, U>(...)"},
				{"a generic method, another value parameter count", method_v1, method_user, tool("\tT scaled<T>(T x, T y) { return x + y; }\n"), 'r', false, false,
					"error: 'Tool::scaled<T>' takes 2 arguments, not 1"},
				{"a generic method, a method of its name", method_v1, method_user, tool("\tstring scaled(string x) { return x; }\n"), 'r', false, false,
					"error: 'scaled' takes no type arguments: it is not a generic method"},
				{"a generic method, gone from its class", method_v1, method_user, tool(""), 'r', false, false, "error: no member 'scaled' in class 'Tool'"},
				{"a generic method, dropped with its class", method_v1, method_user, limit, 'd', false, false, ""},
			};
			// How the new text arrives. A reload (an import, or a file the host runs again) checks the units it does not run again; a
			// later execute of the text, another file and strings do not.
			enum class route { root_file, module, executed_string, module_run_again, execute_file, execute_file_source, later_execute, another_file, strings_only };
			const std::vector<std::pair<route, std::string>> routes = {
				{route::root_file, "a reload, the root file uses it"}, {route::module, "a reload, another module uses it"}, {route::executed_string, "a reload, an executed string uses it"},
				{route::module_run_again, "a reload that runs the module that uses it again"}, {route::execute_file, "execute_file of the file again"},
				{route::execute_file_source, "execute_file_source of the file"}, {route::later_execute, "a later execute of the text"},
				{route::another_file, "another file with the text, imported"}, {route::strings_only, "strings only"},
			};
			const auto outcome = [](engine& eng, const std::string& code) {
				try {
					return eng.execute(code).to_string();
				} catch (const std::exception& error) {
					return "ERR: " + std::string(error.what());
				}
			};
			int made = 0;
			for (const auto& s : shapes) {
				for (const auto& [how, route_name] : routes) {
					const std::string what = s.name + ", " + route_name;
					const auto directory = unit_directory("verdict_" + std::to_string(made++));
					auto e = make_engine();
					e->add_include_path(directory.string());
					e->import_recheck_interval(0);
					const std::string lib = write_unit(directory, "lib.jai", s.v1);
					auto stamp = std::filesystem::last_write_time(lib);
					const auto edit = [&] {
						write_unit(directory, "lib.jai", s.v2);
						std::filesystem::last_write_time(lib, stamp += std::chrono::seconds(3));
					};
					std::string verdict;
					const auto run = [&](const std::function<void()>& host_call) {
						try {
							host_call();
						} catch (const std::exception& error) {
							verdict = "ERR: " + std::string(error.what());
						}
					};
					switch (how) {
						case route::root_file:
							e->execute_file(write_unit(directory, "root.jai", "import \"lib.jai\";\n" + s.user + "int tick() { import(\"lib.jai\"); return use(); }\n"));
							break;
						case route::module: case route::module_run_again:
							write_unit(directory, "user.jai", "import \"lib.jai\";\n" + s.user);
							e->execute("import \"user.jai\";");
							break;
						case route::executed_string:
							e->execute("import \"lib.jai\";");
							e->execute(s.user);
							break;
						case route::strings_only:
							e->execute(s.v1);
							e->execute(s.user);
							break;
						default:
							e->execute_file(write_unit(directory, "root.jai", "import \"lib.jai\";\n" + s.user));
							break;
					}
					const std::string before = outcome(*e, "use();");
					check_true(before.rfind("ERR", 0) != 0, what + ": " + before);
					switch (how) {
						case route::root_file: edit(); verdict = outcome(*e, "tick();"); break;
						case route::module: case route::executed_string: edit(); verdict = outcome(*e, "import(\"lib.jai\");"); break;
						case route::module_run_again: edit(); verdict = outcome(*e, "import(\"user.jai\");"); break;
						case route::execute_file: edit(); run([&] { e->execute_file(lib); }); break;
						case route::execute_file_source: run([&] { e->execute_file_source(lib, s.v2); }); break;
						case route::another_file: write_unit(directory, "other.jai", s.v2); verdict = outcome(*e, "import \"other.jai\";"); break;
						default: verdict = outcome(*e, s.v2); break;
					}
					std::string warned;
					for (const auto& d : e->last_check_diagnostics().diagnostics) { if (d.severity == check_diagnostic::level::warning) { warned += d.message + "\n"; } }
					const std::string after = outcome(*e, "use();");
					const bool rejected = verdict.rfind("ERR: static check failed:", 0) == 0;
					// a file the host runs again (execute_file, execute_file_source of a path that already ran) is a reload too
				const bool reloads = how == route::root_file || how == route::module || how == route::executed_string || how == route::module_run_again ||
				                     how == route::execute_file || how == route::execute_file_source;
					// what the text drops stays for every unit, one that runs again too, as a function or a class it drops does
					const bool rejects = reloads && s.reload == 'r';
					check_eq(rejects, rejected, what + ": " + verdict);
					if (rejects) {
						check_true(contains(verdict, s.says), what + ": " + s.says + " in: " + verdict);
						check_eq(before, after, what + ": the code it had keeps running");
					} else {
						check_true(verdict.rfind("ERR", 0) != 0, what + ": " + verdict);
						check_eq(s.keeps, contains(warned, "keeps its earlier declaration: this declaration of"), what + ": " + warned);
					}
					if (reloads && s.reload == 'a') { check_true(before != after, what + ": the edit applies: " + after); }
					if (reloads && s.reload == 'd' && !rejects) { check_eq(before, after, what + ": what the text drops stays for the unit that uses it"); }
				}
			}
		});

		test("a_reload_checks_the_instances_of_every_file_it_reads_before_any_runs", [this]() {
			// root.jai names Box<int>; user.jai, a module, names Box<string> and imports the generic's file. An edit Box<int>
			// cannot be made from reaches the reload at user.jai's import: the module would run again and bring lib.jai with it
			const auto directory = unit_directory("reload_nested");
			const std::string lib = write_unit(directory, "lib.jai", "class Box<T> {\n\tT item;\n\tint size() { return 1; }\n}\n");
			write_unit(directory, "user.jai", "import \"lib.jai\";\nuser_ran();\nBox<string> words;\nint user_value() { return words.size(); }\n");
			const std::string root = write_unit(directory, "root.jai", "import \"user.jai\";\nBox<int> box;\nint root_value() { return box.size() * 10 + user_value(); }\n");
			auto e = stdlib_engine();
			e->add_include_path(directory.string());
			e->import_recheck_interval(0);
			int runs = 0;
			e->add_function("user_ran", [&runs]() { ++runs; });
			e->execute_file(root);
			check_eq((int64_t)11, e->execute("root_value();").as_int());
			auto stamp = std::filesystem::last_write_time(lib);
			write_unit(directory, "lib.jai", "class Box<T> {\n\tT item;\n\tint size() { return item.size() + 5; }\n}\n");
			std::filesystem::last_write_time(lib, stamp += std::chrono::seconds(3));
			const std::string rejected = failure(*e, "import(\"user.jai\");");
			check_true(contains(rejected, "lib.jai:3:27: error: int has no member 'size'\n    in Box<int>, instantiated at ") && contains(rejected, "root.jai:2:4"), rejected);
			check_eq(1, runs, "the module did not run again");
			check_eq((int64_t)11, e->execute("root_value();").as_int());
			// the fix: every instance is made again
			write_unit(directory, "lib.jai", "class Box<T> {\n\tT item;\n\tint size() { return 7; }\n}\n");
			std::filesystem::last_write_time(lib, stamp += std::chrono::seconds(3));
			check_true(e->execute("import(\"user.jai\");").as_bool());
			check_eq(2, runs);
			check_eq((int64_t)77, e->execute("root_value();").as_int());
		});

		test("a_reload_whose_file_takes_a_generic_classs_name_checks_the_instances_too", [this]() {
			const auto directory = unit_directory("reload_takes_name");
			write_unit(directory, "lib.jai", "class Box<T> {\n\tT item;\n\tint size() { return 1; }\n}\nclass Plain {\n\tint size() { return 0; }\n}\n");
			const std::string other = write_unit(directory, "other.jai", "int other_value() { return 1; }\n");
			const std::string root = write_unit(directory, "root.jai",
				"import \"lib.jai\";\nimport \"other.jai\";\nBox<int> box;\nPlain plain;\nint root_value() { return box.size() * 10 + other_value() + plain.size(); }\n"
				"int tick() { import(\"other.jai\"); return root_value(); }\n");
			auto e = stdlib_engine();
			e->add_include_path(directory.string());
			e->import_recheck_interval(0);
			e->execute_file(root);
			check_eq((int64_t)11, e->execute("root_value();").as_int());
			auto stamp = std::filesystem::last_write_time(other);
			const auto edit = [&](const std::string& text) {
				write_unit(directory, "other.jai", text);
				std::filesystem::last_write_time(other, stamp += std::chrono::seconds(3));
			};
			// another module's edit declares a class of the generic's name, then a generic class of it: the later declaration is
			// the one the root's instance has to be made from
			edit("class Box {\n\tint plain = 1;\n}\nclass Plain {\n\tint other = 1;\n}\nint other_value() { return 5; }\n");
			const std::string taken = failure(*e, "tick();");
			check_true(contains(taken, "root.jai:3:4: error: 'Box' takes no type arguments: it is not a generic class"), taken);
			check_true(contains(taken, "error: no member 'size' in class 'Plain'"), "the class written by hand beside it: " + taken);
			check_eq((int64_t)11, e->execute("root_value();").as_int());
			edit("class Box<T> {\n\tT item;\n\tint size() { return item.size(); }\n}\nint other_value() { return 5; }\n");
			const std::string replaced = failure(*e, "tick();");
			check_true(contains(replaced, "other.jai:3:27: error: int has no member 'size'\n    in Box<int>, instantiated at ") && contains(replaced, "root.jai:3:4"), replaced);
			check_eq((int64_t)11, e->execute("root_value();").as_int());
			edit("class Box<T> {\n\tT item;\n\tint size() { return 7; }\n}\nint other_value() { return 5; }\n");
			check_eq((int64_t)75, e->execute("tick();").as_int());
			check_eq((int64_t)75, e->execute("root_value();").as_int());
		});

		test("a_reload_keeps_with_a_warning_only_an_instance_no_loaded_unit_names", [this]() {
			const auto directory = unit_directory("reload_unnamed");
			const std::string lib = write_unit(directory, "lib.jai", "class Box<T> {\n\tT item;\n\tint size() { return 1; }\n}\nclass Crate<T> {\n\tT item;\n\tint weight() { return 3; }\n}\n"
				"namespace shop { class Shelf<T> {\n\tT item;\n\tint rows() { return 4; }\n} }\nT same<T>(T x) { return x; }\n");
			auto e = stdlib_engine();
			e->add_include_path(directory.string());
			e->import_recheck_interval(0);
			e->execute("import \"lib.jai\";");
			// scratch.jai made Box<float> and no longer names it: no loaded unit does, as no loaded unit calls a function once the
			// text that called it is gone
			const std::string scratch = write_unit(directory, "scratch.jai", "Box<float> ratio;\nratio.size();\n");
			check_eq((int64_t)1, e->execute_file(scratch).as_int());
			write_unit(directory, "scratch.jai", "int nothing = 0;\n");
			e->execute_file(scratch);
			e->execute("Box<string> words; Crate<int> crate; shop::Shelf<int> shelf; int use() { return words.size() * 100 + crate.weight() * 10 + shelf.rows() + same<int>(0); }");
			check_eq((int64_t)134, e->execute("use();").as_int());
			auto stamp = std::filesystem::last_write_time(lib);
			const auto edit = [&](const std::string& text) {
				write_unit(directory, "lib.jai", text);
				std::filesystem::last_write_time(lib, stamp += std::chrono::seconds(3));
			};
			// Box's new body checks for string and not for float; Crate, shop::Shelf and same are dropped, and stay for the unit
			// that uses them as a class or a function the text drops does
			edit("class Box<T> {\n\tT item;\n\tint size() { return item.size() + 2; }\n}\n");
			check_true(e->execute("import(\"lib.jai\");").as_bool());
			check_eq(std::vector<std::string>{"'Box<float>' keeps its earlier declaration: this declaration of 'Box' cannot make it: float has no member 'size' (" + lib + ":3:27)"}, warnings(*e));
			check_eq((int64_t)234, e->execute("use();").as_int());
			check_eq((int64_t)1, e->execute("ratio.size();").as_int(), "the instance no unit names keeps its earlier declaration");
			// a later reload: what the file dropped still stands for the unit that uses it
			edit("class Box<T> {\n\tT item;\n\tint size() { return 9; }\n}\n");
			check_true(e->execute("import(\"lib.jai\");").as_bool());
			check_true(warnings(*e).empty());
			check_eq(std::string("934|9"), e->execute("to_string(use()) + \"|\" + to_string(ratio.size());").as_string());
			// the first edit again, while a loaded unit names Box<float>: rejected, the dropped generics' instances no part of it
			e->execute("Box<float> named; int named_size() { return named.size(); }");
			edit("class Box<T> {\n\tT item;\n\tint size() { return item.size() + 4; }\n}\n");
			const std::string rejected = failure(*e, "import(\"lib.jai\");");
			check_true(contains(rejected, "lib.jai:3:27: error: float has no member 'size'\n    in Box<float>, instantiated at <script>:1:4"), rejected);
			check_false(contains(rejected, "Crate") || contains(rejected, "Shelf") || contains(rejected, "same"), rejected);
			check_eq(std::string("934|9"), e->execute("to_string(use()) + \"|\" + to_string(named_size());").as_string());
		});

		test("a_unit_that_instantiates_another_files_generic_keeps_no_jaibite_cache", [this]() {
			const auto directory = unit_directory("cache");
			const std::string lib = write_unit(directory, "lib.jai", "class Box<T> {\n\tT item;\n}\nBox<int> own;\n");
			const std::string user = write_unit(directory, "user.jai", "import \"lib.jai\";\nBox<string> b;\nb.item = \"x\";\nb.item;\n");
			auto e = stdlib_engine();
			check_eq(std::string("x"), e->execute_file(user).as_string());
			// lib.jai instantiates its own generic: cached with its instance. user.jai rests on lib.jai's text: no cache holds it
			check_true(std::filesystem::exists(jaibite_cache_beside(lib)));
			check_false(std::filesystem::exists(jaibite_cache_beside(user)));
			auto again = stdlib_engine();
			check_eq(std::string("x"), again->execute_file(user).as_string());   // the cached lib.jai still declares its generic
		});

		test("a_jaibite_keeps_a_generic_and_its_instances", [this]() {
			auto e = stdlib_engine();
			auto bite = e->jaibite("class Cell<T> { T v; T twice() { return v + v; } }\nT same<T>(T x) { return x; }\nCell<int> c;\nc.v = same(4);\nc.twice();\n");
			check_eq((int64_t)8, bite.execute().as_int());
			const std::vector<uint8_t> bytes = bite.save_bytes();
			auto loader = stdlib_engine();
			auto loaded = loader->jaibite_load_bytes(bytes);
			check_eq((int64_t)8, loaded.execute().as_int());
			// the generic a bite declares instantiates for the units that run after it
			check_eq(std::string("abab|Cell<string>"), loader->execute("Cell<string> s; s.v = same(\"ab\"); s.twice() + \"|\" + reflect::type_of(s);").as_string());
			// a bite that only uses a generic carries its instances
			auto user = loader->jaibite("Cell<float> f; f.v = 1.5; f.twice();");
			auto second = stdlib_engine();
			second->execute("class Cell<T> { T v; T twice() { return v + v; } }");
			auto reloaded = second->jaibite_load_bytes(user.save_bytes());
			check_near(3.0, reloaded.execute().as_float(), 1e-9);
		});

		test("a_saved_bite_carries_no_instance_it_only_made_again", [this]() {
			const std::string first = "class Box<T> { T v; int get() { return 1; } }\nT twice<T>(T x) { return x; }\n";
			auto e = stdlib_engine();
			e->execute(first);
			e->execute("Box<int> a; int use() { return twice<int>(4) + a.get(); }");
			// the bite declares the generics again and names no instance: Box<int> and twice<int> are made again in this engine
			auto bite = e->jaibite("class Box<T> { T v; int get() { return 2; } }\nT twice<T>(T x) { return x + x; }\n");
			bite.execute();
			check_eq((int64_t)10, e->execute("use();").as_int());
			const std::vector<uint8_t> bytes = bite.save_bytes();
			// a fresh engine holds no instance: the loaded bite declares its generics and nothing else
			auto fresh = stdlib_engine();
			auto loaded = fresh->jaibite_load_bytes(bytes);
			loaded.execute();
			check_false(fresh->execute("reflect::classes().contains(\"Box<int>\");").as_bool(), "an instance the saving engine held is no part of the unit");
			check_false(fresh->has_function("twice<int>") || fresh->has_variable("twice<int>"));
			check_eq((int64_t)10, fresh->execute("Box<int> b; twice<int>(4) + b.get();").as_int());
			// an engine that holds instances: the loaded bite makes those again, as the unit freshly checked there does
			auto holder = stdlib_engine();
			holder->execute(first);
			holder->execute("Box<string> s; int use() { return twice<int>(4) + s.get(); } float half() { return twice<float>(1.5); }");
			auto there = holder->jaibite_load_bytes(bytes);
			there.execute();
			check_true(warnings(*holder).empty());
			check_eq((int64_t)10, holder->execute("use();").as_int());
			check_near(3.0, holder->execute("half();").as_float(), 1e-9);
			check_false(holder->execute("reflect::classes().contains(\"Box<int>\");").as_bool(), "what this engine does not hold is not declared");
			// an instance the bite's own text names is the bite's, and is carried
			auto named = e->jaibite("class Box<T> { T v; int get() { return 3; } }\nBox<int> mine;\nmine.get();\n");
			auto elsewhere = stdlib_engine();
			check_eq((int64_t)3, elsewhere->jaibite_load_bytes(named.save_bytes()).execute().as_int());
		});

		test("a_cached_unit_keeps_its_instances_where_the_first_use_put_them", [this]() {
			const auto directory = unit_directory("cached_placement");
			const std::string root = write_unit(directory, "root.jai",
				"class Box<T> { T v; Box() { } Box(T x) { v = x; } }\nauto a = Box<int>(1);\nBox<int> b;\nto_string(a.v) + to_string(b.v);\n");
			check_eq(std::string("10"), stdlib_engine()->execute_file(root).as_string());
			check_true(std::filesystem::exists(jaibite_cache_beside(root)));
			check_eq(std::string("10"), stdlib_engine()->execute_file(root).as_string(), "the cache's declarations stand in the order the check left them");
		});

		test("reflection_and_serialization_name_an_instance_as_a_class", [this]() {
			auto e = stdlib_engine();
			e->execute("class Cell<T> { T v; }\nclass User { string name; }\nclass Page<T> { array<T> rows; int total = 0; }");
			check_eq(std::string("Cell<int>|true"), e->execute("Cell<int> c; reflect::type_name(c) + \"|\" + to_string(reflect::classes().contains(\"Cell<int>\"));").as_string());
			check_eq(std::string("{\"$type\":\"Cell<int>\",\"$val\":{\"v\":4}}"), e->execute("Cell<int> c; c.v = 4; to_json(c);").as_string());
			check_eq(std::string("4Cell<int>"), e->execute("Cell<int> c; c.v = 4; var back = from_json(to_json(c)); to_string(back.v) + reflect::type_of(back);").as_string());
			check_eq(std::string("int"), e->execute("Cell<int> c; reflect::fields(c)[0][\"type\"];").as_string());
			// a type argument to a host names an instance like any class
			check_eq(std::string("ann1Page<User>"), e->execute(
				R"(Page<User> p = from_json<Page<User>>("{\"rows\": [{\"name\": \"ann\"}], \"total\": 1}"); p.rows[0].name + to_string(p.total) + reflect::type_of(p);)").as_string());
		});

		test("a_type_naming_an_instance_the_program_never_made_raises", [this]() {
			auto e = stdlib_engine();
			e->execute("class Cell<T> { T v; }\nCell<int> made;");
			const std::string text = "'Cell<float>' names an instance of the generic 'Cell' that the program never made: name the type where it is loaded, as from_json<Cell<float>>(text) does";
			check_true(contains(failure(*e, R"(from_json("{\"$type\":\"Cell<float>\",\"$val\":{\"v\":1.5}}");)"), text));
			check_eq(text, e->execute(R"(string got = ""; try { from_json("[{\"$type\":\"Cell<float>\",\"$val\":{\"v\":1.5}}]"); } catch (e) { got = e; } got;)").as_string());
			// named where it is loaded, the instance is made; a name no generic declares is data, as any unknown class's is
			check_eq(std::string("1.5Cell<float>"), e->execute(R"(auto c = from_json<Cell<float>>("{\"$type\":\"Cell<float>\",\"$val\":{\"v\":1.5}}"); to_string(c.v) + reflect::type_of(c);)").as_string());
			check_eq(std::string("map"), e->execute(R"(reflect::type_of(from_json("{\"$type\":\"Shelf<float>\",\"$val\":{\"v\":1.5}}"));)").as_string());
			check_eq(std::string("map"), e->execute(R"(reflect::type_of(from_json("{\"$type\":\"Nowhere\",\"$val\":{\"v\":1.5}}"));)").as_string());
		});

		test("the_error_for_a_type_naming_no_instance_fits_the_mistake", [this]() {
			auto e = stdlib_engine();
			e->execute("class Cell<T> { T v; }\nclass Duo<A, B> { A a; B b; }\nenum Color { red }\nclass Plain { int n = 1; }\nnamespace shop { class Item { int n = 2; } }\nCell<int> made;");
			std::string payload;
			e->add_function("payload", [&payload]() { return payload; });
			const std::string blob = e->execute("to_binary(made);").as_string();
			const auto renamed = [&](const std::string& type) {
				const size_t at = blob.find("Cell<int>");
				const uint32_t size = static_cast<uint32_t>(type.size());
				return blob.substr(0, at - 4) + std::string(reinterpret_cast<const char*>(&size), 4) + type + blob.substr(at + 9);
			};
			const auto by_json = [&](const std::string& type) {
				payload = "{\"$type\":\"" + type + "\",\"$val\":{}}";
				return e->execute("string got = \"(no error)\"; try { from_json(payload()); } catch (e) { got = e; } got;").as_string();
			};
			const auto by_binary = [&](const std::string& type) {
				payload = renamed(type);
				return e->execute("string got = \"(no error)\"; try { from_binary(payload()); } catch (e) { got = e; } got;").as_string();
			};
			std::vector<std::pair<std::string, std::string>> mistakes = {
				{"Cell<float>", "'Cell<float>' names an instance of the generic 'Cell' that the program never made: name the type where it is loaded, as from_json<Cell<float>>(text) does"},
				{"Cell<Duo<float, Color>>", "'Cell<Duo<float, Color>>' names an instance of the generic 'Cell' that the program never made: name the type where it is loaded, as from_json<Cell<Duo<float, Color>>>(text) does"},
				{"Cell<map<string,array<shop::Item>>>", "'Cell<map<string,array<shop::Item>>>' names an instance of the generic 'Cell' that the program never made: name the type where it is loaded, as from_json<Cell<map<string,array<shop::Item>>>>(text) does"},
				{"Cell<Nope>", "'Cell<Nope>' names no instance of the generic 'Cell': unknown type 'Nope'"},
				{"Cell<array<Nope>>", "'Cell<array<Nope>>' names no instance of the generic 'Cell': unknown type 'Nope'"},
				{"Cell<int,int>", "'Cell<int,int>' names no instance of the generic 'Cell': 'Cell' takes 1 type argument, not 2"},
				{"Duo<int>", "'Duo<int>' names no instance of the generic 'Duo': 'Duo' takes 2 type arguments, not 1"},
				{"Cell<>", "'Cell<>' names no instance of the generic 'Cell': 'Cell' takes 1 type argument, not 0"},
				{"Cell<Duo<int>>", "'Cell<Duo<int>>' names no instance of the generic 'Cell': 'Duo' takes 2 type arguments, not 1"},
				{"Cell<Cell>", "'Cell<Cell>' names no instance of the generic 'Cell': 'Cell' takes 1 type argument, not 0"},
				{"Cell<map<int>>", "'Cell<map<int>>' names no instance of the generic 'Cell': 'map' takes 2 type arguments, not 1"},
				{"Cell<int<int>>", "'Cell<int<int>>' names no instance of the generic 'Cell': 'int' takes no type arguments: it is a builtin type"},
				{"Cell<Plain<int>>", "'Cell<Plain<int>>' names no instance of the generic 'Cell': 'Plain' takes no type arguments: it is not a generic class"},
				{"Cell<Color<int>>", "'Cell<Color<int>>' names no instance of the generic 'Cell': 'Color' takes no type arguments: it is an enum"},
				{"Cell<void>", "'Cell<void>' names no instance of the generic 'Cell': a type argument names a type: 'void' is not one an instance can be made with"},
				{"Cell<int>>", "'Cell<int>>' names no instance of the generic 'Cell': it is not the spelling of a type"},
				{"Cell<int><int>", "'Cell<int><int>' names no instance of the generic 'Cell': it is not the spelling of a type"},
				{"Cell<,>", "'Cell<,>' names no instance of the generic 'Cell': 'Cell' takes 1 type argument, not 2"},
				{"Duo<int,>", "'Duo<int,>' names no instance of the generic 'Duo': it is not the spelling of a type"},
			};
			std::string deep = "int";
			for (int i = 0; i < 700; ++i) { deep = "Cell<" + deep + ">"; }
			mistakes.emplace_back(deep, "'" + deep + "' names no instance of the generic 'Cell': no instance's name is longer than 4096 characters");
			for (const auto& [type, text] : mistakes) {
				check_eq(text, by_json(type), "from_json of " + type);
				const size_t advice = text.find(", as from_json<");
				check_eq(advice == std::string::npos ? text : text.substr(0, advice) + ", as from_binary<" + type + ">(bytes) does", by_binary(type), "from_binary of " + type);
			}
			// the advice loads, from either archive
			check_eq(std::string("Cell<float>"), e->execute("reflect::type_of(from_json<Cell<float>>(\"{\\\"$type\\\":\\\"Cell<float>\\\",\\\"$val\\\":{}}\"));").as_string());
			payload = renamed("Cell<bool>");
			check_eq(std::string("Cell<bool>"), e->execute("reflect::type_of(from_binary<Cell<bool>>(payload()));").as_string());
		});

		test("printed_and_saved_type_names_keep_their_spellings", [this]() {
			auto e = stdlib_engine();
			e->execute("class Duo<A, B> { A a; B b; } class Box<T> { T v; } namespace shop { class Item { int n = 2; } class Shelf<T> { T item; } }"
			           " Duo<int, string> d; Box<map<string, int>> bm; Box<Duo<int, array<float>>> nested; shop::Shelf<shop::Item> shelf; Box<var> any; Box<Box<var>> inner; Box<array<var>> list;");
			// an instance joins its arguments with ", ", a builtin container with ","
			check_eq(std::string("Duo<int, string>|Box<map<string,int>>|Box<Duo<int, array<float>>>|shop::Shelf<shop::Item>"),
				e->execute("reflect::type_of(d) + \"|\" + reflect::type_of(bm) + \"|\" + reflect::type_name(nested) + \"|\" + reflect::type_of(shelf);").as_string());
			check_eq(std::string("{\"$type\":\"Duo<int, string>\",\"$val\":{\"a\":0,\"b\":\"\"}}"), e->execute("to_json(d);").as_string());
			check_eq(std::string("{\"$type\":\"Box<map<string,int>>\",\"$val\":{\"v\":{}}}"), e->execute("to_json(bm);").as_string());
			check_true(e->execute("reflect::classes().contains(\"Duo<int, string>\") && !reflect::classes().contains(\"Duo<int,string>\");").as_bool());
			check_true(contains(e->execute("to_binary(d);").as_string(), "Duo<int, string>"));
			check_true(contains(e->execute("to_binary(bm);").as_string(), "Box<map<string,int>>"));
			// var is spelled var in an instance's name, at any depth; a builtin container's own name is what it was
			check_eq(std::string("Box<var>|Box<Box<var>>|Box<array<var>>"), e->execute("reflect::type_of(any) + \"|\" + reflect::type_of(inner) + \"|\" + reflect::type_of(list);").as_string());
			check_eq(std::string("{\"$type\":\"Box<var>\",\"$val\":{\"v\":null}}"), e->execute("to_json(any);").as_string());
			check_eq(std::string("Box<var>|7"), e->execute("any.v = 7; auto back = from_json(to_json(any)); reflect::type_of(back) + \"|\" + to_string(back.v);").as_string());
			check_eq(std::string("array<any>"), detail::engine_access::get_type_info_array(*e, detail::engine_access::get_type_info_any(*e))->canonical_name());
			check_eq(std::string("array<var>"), detail::engine_access::get_type_info_array(*e, detail::engine_access::get_type_info_any(*e))->spelling());
			check_eq(std::string("array|map"), e->execute("array<var> xs; map<string, var> m; reflect::type_of(xs) + \"|\" + reflect::type_of(m);").as_string());
		});

		test("a_type_name_that_arrives_as_text_is_read_without_its_spacing", [this]() {
			auto e = stdlib_engine();
			e->execute("class Duo<A, B> { A a; B b; } class Box<T> { T v; } namespace shop { class Item { int n = 2; } class Shelf<T> { T item; } } class Plain { int n = 1; }"
			           " Duo<int, string> d; d.a = 4; Box<map<string, int>> bm; shop::Shelf<shop::Item> shelf;");
			const auto reaches = [&](const std::string& spelled, const std::string& name) {
				const std::string quoted = "\"" + spelled + "\"";
				check_eq(name, e->execute("reflect::type_of(reflect::construct(" + quoted + "));").as_string(), "reflect::construct " + spelled);
				check_eq(name, e->execute("reflect::type_of(reflect::construct_shared(" + quoted + "));").as_string(), "reflect::construct_shared " + spelled);
				check_true(e->execute("reflect::fields(" + quoted + ").size() > 0 && reflect::is_instance_of(reflect::construct(" + quoted + "), " + quoted + ");").as_bool(), "reflect:: " + spelled);
				check_eq(name, e->execute("reflect::type_of(from_json(\"{\\\"$type\\\":\\\"" + spelled + "\\\",\\\"$val\\\":{}}\"));").as_string(), "from_json " + spelled);
				check_eq(name, detail::registered_type_spelling(e.get(), spelled), "the registered spelling of " + spelled);
			};
			for (const char* spelled : {"Duo<int, string>", "Duo<int,string>", "Duo< int , string >", "  Duo <int ,  string> "}) { reaches(spelled, "Duo<int, string>"); }
			for (const char* spelled : {"Box<map<string,int>>", "Box<map<string, int>>", "Box< map < string , int > >"}) { reaches(spelled, "Box<map<string,int>>"); }
			for (const char* spelled : {"shop::Shelf<shop::Item>", "shop :: Shelf < shop :: Item >"}) { reaches(spelled, "shop::Shelf<shop::Item>"); }
			reaches(" Plain ", "Plain");
			check_eq((int64_t)9, e->execute("from_json(\"{\\\"$type\\\":\\\"Duo< int,string >\\\",\\\"$val\\\":{\\\"a\\\":9,\\\"b\\\":\\\"x\\\"}}\").a;").as_int());
			// case stays significant
			check_true(contains(failure(*e, "reflect::construct(\"duo<int, string>\");"), "reflect::construct: unknown class 'duo<int, string>'"));
			// the binary and the base64 archives: the name in the data, spaced otherwise
			std::string blob = e->execute("to_binary(d);").as_string();
			const size_t at = blob.find("Duo<int, string>");
			check_true(at != std::string::npos);
			blob.replace(at, 16, "Duo< int,string>");
			e->add_function("spaced_blob", [blob]() { return blob; });
			e->add_function("spaced_base64", [blob]() { return jai::detail::base64_encode(blob); });
			check_eq(std::string("Duo<int, string>4"), e->execute("auto back = from_binary(spaced_blob()); reflect::type_of(back) + to_string(back.a);").as_string());
			check_eq(std::string("Duo<int, string>4"), e->execute("auto again = from_binary(base64_decode(spaced_base64())); reflect::type_of(again) + to_string(again.a);").as_string());
			// an instance that waits and one the program never made are told from their keys too
			check_eq(std::string("reflect::construct: 'Rack<Part>' is not declared yet: it waits for 'Part'|'Rack<Part>' is not declared yet: it waits for 'Part'"), e->execute(R"(
				class Rack<T> { T v; }
				string got = "";
				try { reflect::construct("Rack< Part >"); } catch (e) { got += e; }
				got += "|";
				try { from_json("{\"$type\":\" Rack <Part>\",\"$val\":{}}"); } catch (e) { got += e; }
				class Part { int n = 1; }
				Rack<Part> rack;
				got;
			)").as_string());
			check_true(contains(failure(*e, "from_json(\"{\\\"$type\\\":\\\"Duo< float,float >\\\",\\\"$val\\\":{}}\");"),
				"'Duo< float,float >' names an instance of the generic 'Duo' that the program never made: name the type where it is loaded, as from_json<Duo< float,float >>(text) does"));
		});

		// An instance is ordinary code: flow narrowing, initializer lists, declared compound operators, copy rebinding, the
		// dynamic casts and condition declarations hold inside one, and beside one, as written by hand
		test("an_instance_narrows_through_a_cast_of_its_type_parameter", [this]() {
			auto e = stdlib_engine();
			check_eq(std::string("3|0|8|int|other"), e->execute(R"(
				class Entity { int id = 0; }
				class Goblin : Entity { int gold = 3; }
				int gold_of<T : Entity>(Entity e) { if (dynamic_cast<shared_ptr<T>>(e)) { return e.gold; } return 0; }
				class Purse<T : Entity> {
					int take(Entity e) { if (reflect::is_instance_of(e, "Goblin")) { return e.gold + 5; } return -1; }
				}
				string kind<T>(var v) { if (reflect::type_of(v) == "int") { int n = v; return "int"; } return "other"; }
				Goblin g; Entity plain; Purse<Goblin> purse;
				`${gold_of<Goblin>(g)}|${gold_of<Goblin>(plain)}|${purse.take(g)}|${kind<bool>(4)}|${kind<bool>("s")}`;
			)").as_string());
			// without the test the member is the base's, in the instance as in a function written by hand
			const std::string unnarrowed = failure(*e, "class Entity { int id = 0; } class Goblin : Entity { int gold = 3; }"
			                                           " int gold_of<T : Entity>(Entity e) { return e.gold; } Goblin g; gold_of<Goblin>(g);");
			check_true(contains(unnarrowed, "no member 'gold' in class 'Entity'") && contains(unnarrowed, "in gold_of<Goblin>, instantiated at"), unnarrowed);
		});

		test("the_dynamic_casts_take_a_type_parameter_and_an_instance", [this]() {
			auto e = stdlib_engine();
			check_eq(std::string("true|false|4|9|true|false|true|false"), e->execute(R"(
				class Shape { int sides = 0; }
				class Square : Shape { Square() { sides = 4; } }
				class Probe<T> {
					bool holds(shared_ptr<Shape> s) { return dynamic_cast<T>(s) != null; }
					int sides_of(Shape& s) { return dynamic_cast<T&>(s).sides; }
				}
				bool is<T>(shared_ptr<Shape> s) { return dynamic_cast<T>(s) != null; }
				class Box<T> { T v; }
				class Wide<T> : Box<T> { }
				shared_ptr<Shape> sq = new Square(); shared_ptr<Shape> plain = new Shape();
				Probe<Square> probe; Square real;
				shared_ptr<Box<int>> held = new Wide<int>(); held.v = 9;
				`${probe.holds(sq)}|${probe.holds(plain)}|${probe.sides_of(real)}|${dynamic_cast<shared_ptr<Wide<int>>>(held).v}|${is<Square>(sq)}|${is<Square>(plain)}|${is<shared_ptr<Square>>(sq)}|${is<shared_ptr<Square>>(plain)}`;
			)").as_string());
			// a handle as the type argument is that handle (above); inside a spelled shared_ptr<T> it is a handle to a handle, and that instance alone fails
			const std::string handle = failure(*e, "class Shape { } bool is<T>(shared_ptr<Shape> s) { return dynamic_cast<shared_ptr<T>>(s) != null; } shared_ptr<Shape> s = new Shape(); is<Shape>(s); is<shared_ptr<Shape>>(s);");
			check_true(contains(handle, "dynamic_cast<shared_ptr<shared_ptr<Shape>>> names a handle to a handle: dynamic_cast takes a class or one shared_ptr to it, dynamic_cast<Shape>(e)") &&
			           contains(handle, "in is<shared_ptr<Shape>>, instantiated at") && !contains(handle, "in is<Shape>"), handle);
			const std::string plain = failure(*e, "class Shape { } bool is<T>(shared_ptr<Shape> s) { return dynamic_cast<T>(s) != null; } shared_ptr<Shape> s = new Shape(); is<Shape>(s); is<int>(s);");
			check_true(contains(plain, "dynamic_cast takes a class: dynamic_cast<T>(e) is e as a shared_ptr<T>, null when it is no T, and dynamic_cast<T&>(e) is the reference") &&
			           contains(plain, "in is<int>, instantiated at") && !contains(plain, "in is<Shape>"), plain);
			// a bound's class in a condition declaration, an instance's name and a namespace's as the cast's class
			check_eq(std::string("3|0|5|true|true|true"), e->execute(R"(
				class Animal { int legs = 4; }
				class Dog : Animal { int tricks = 3; }
				int tricks_of<T : Animal>(shared_ptr<Animal> a) { if (auto found = dynamic_cast<T>(a)) { return found.tricks; } return 0; }
				class Stack<T> { array<T> items; }
				namespace shop { class Item { } class Stack<T> { int n = 5; } }
				shared_ptr<Animal> dog = new Dog(); shared_ptr<Animal> other = new Animal();
				var stack = Stack<int>(); shop::Stack<shop::Item> crate;
				`${tricks_of<Dog>(dog)}|${tricks_of<Dog>(other)}|${dynamic_cast<shop::Stack<shop::Item>&>(crate).n}|${dynamic_cast<Stack<int>>(stack) != null}|` +
				`${dynamic_cast<shared_ptr<Stack<int>>>(stack) != null}|${dynamic_cast<Stack<int>&>(stack).items.empty()}`;
			)").as_string());
			const std::string builtin = failure(*e, "class Shape { } class Probe<T> { int f(Shape& s) { dynamic_cast<T&>(s); return 1; } } Probe<int> p;");
			check_true(contains(builtin, "dynamic_cast takes a class: dynamic_cast<T>(e) is e as a shared_ptr<T>, null when it is no T, and dynamic_cast<T&>(e) is the reference") &&
			           contains(builtin, "in Probe<int>, instantiated at"), builtin);
		});

		test("a_condition_declares_an_instance_and_a_comparison_stays_one", [this]() {
			auto e = stdlib_engine();
			check_eq(std::string("7|7|8|less|3"), e->execute(R"(
				class Box<T> { T v; Box() { } Box(T x) { v = x; } }
				shared_ptr<Box<int>> make(int n) { return new Box<int>(n); }
				string got = "";
				if (shared_ptr<Box<int>> b = make(7)) { got += to_string(b.v); }
				if (auto b = dynamic_cast<shared_ptr<Box<int>>>(make(7))) { got += "|" + to_string(b.v); }
				if (Box<int> c = Box<int>(8); c.v > 3) { got += "|" + to_string(c.v); }
				int a = 1; int b = 4;
				if (a < b) { got += "|less"; }
				int turns = 0;
				while (a < b) { a += 1; turns += 1; }
				got + "|" + to_string(turns);
			)").as_string());
		});

		test("an_instance_takes_an_initializer_list_and_a_declared_compound_operator", [this]() {
			auto e = stdlib_engine();
			check_eq(std::string("10|2|ab|1|11|1"), e->execute(R"(
				class Base<T> { T seed; Base(T s) : seed(s) { } }
				class Acc<T> : Base<T> {
					T total; int adds;
					Acc(T start) : Base<T>(start), total(start), adds(0) { }
					Acc& operator+=(T x) { total += x; adds += 1; return this; }
					Acc& operator++() { adds += 10; return this; }
				}
				Acc<int> a(5); a += 2; a += 3;
				Acc<string> s("a"); s += "b";
				Acc<int> n(0); ++n; n += 1;
				`${a.total}|${a.adds}|${s.total}|${s.adds}|${n.adds}|${n.total}`;
			)").as_string());
		});

		test("a_copy_of_an_instance_rebinds_the_lambdas_bound_to_it", [this]() {
			auto e = stdlib_engine();
			check_eq(std::string("1|6|x|xyz"), e->execute(R"(
				class Counter<T> {
					T n; var bump;
					void arm() { bump = [this](T by) { n += by; return n; }; }
				}
				Counter<int> c; c.arm(); c.bump(1);
				Counter<int> d = c; d.bump(5);
				Counter<string> s; s.arm(); s.bump("x");
				array<Counter<string>> all; all.push(s); all[0].bump("yz");
				`${c.n}|${d.n}|${s.n}|${all[0].n}`;
			)").as_string());
		});

		test("a_signature_is_a_type_inside_a_generic", [this]() {
			auto e = stdlib_engine();
			// function<R(A...)> in a generic's parameters and fields is ordinary code once the instance is made
			check_eq(std::string("4|8|3|7"), e->execute(R"(
				T apply<T>(function<T(T)> f, T x) { return f(x); }
				class Slot<T> { function<T(T)> f; T run(T x) { return f(x); } }
				int total = 0;
				void each<T>(array<T> xs, function<void(T)> f) { for (auto x : xs) { f(x); } }
				class Box<T> { T v; }
				Slot<int> s; s.f = (int a) => a * 2;
				each([1, 2], (int x) => { total += x; });
				Box<function> any; any.v = () => 7;
				`${apply<int>((int a) => a + 1, 3)}|${s.run(4)}|${total}|${any.v()}`;
			)").as_string());
			check_eq((int64_t)1, e->execute("T run<T>(function<T()> f) { return f(); } function<int()> one = () => 1; run<int>(one);").as_int());
			// a typed callable's own '<' is no type-argument list of a call, and a comparison beside one stays a comparison
			check_eq(std::string("3|true|false"), e->execute("function<int(int)> f = (int a) => a + 1; int g = 1; int h = 2; `${f(2)}|${g < h}|${f(1) < h}`;").as_string());
		});

		// function<R(A...)> and coroutine<Y> are type arguments like any other type: the instance's text spells them where the
		// parameter stood, and its name spells them as their own names do
		test("a_signature_and_a_coroutine_type_are_type_arguments", [this]() {
			auto e = stdlib_engine();
			const std::string box = "class Box<T> { T v; } T pick<T>(T a) { return a; } ";
			check_eq(std::string("3|Box<function<int(int)>>"), e->execute(box + "Box<function<int(int)>> b; b.v = (int a) => a + 1; `${b.v(2)}|${reflect::type_of(b)}`;").as_string());
			check_eq(std::string("1|Box<function<void()>>"), e->execute(box + "int hits = 0; array<Box<function<void()>>> bs; Box<function<void()>> one; one.v = () => { hits += 1; };"
				" bs.push(one); bs[0].v(); `${hits}|${reflect::type_of(bs[0])}`;").as_string());
			check_eq(std::string("42|Box<coroutine<int>>"), e->execute(box + "coroutine int gen() { yield 20; yield 22; } Box<coroutine<int>> c; c.v = gen();"
				" `${c.v.next() + c.v.next()}|${reflect::type_of(c)}`;").as_string());
			// in a call, written: a signature that returns void is one too
			check_eq(std::string("5|1"), e->execute(box + "function<int(int)> inc = (int a) => a + 1; auto g = pick<function<int(int)>>(inc);"
				" int n = 0; function<void()> bump = () => { n += 1; }; pick<function<void()>>(bump)(); `${g(4)}|${n}`;").as_string());
			// a reference parameter, a class and a second parameter are spelled as the signature's own name spells them
			check_eq(std::string("5|Box<function<void(Item&,int)>>"), e->execute(box + "class Item { int n = 1; } void grow(Item& it, int by) { it.n += by; }"
				" Box<function<void(Item&, int)>> g; g.v = grow; Item it; g.v(it, 4); `${it.n}|${reflect::type_of(g)}`;").as_string());
			// a parameter inside a signature or a coroutine type takes a callable argument the same way
			check_eq(std::string("7|Pipe<function<int()>>|coroutine<int>"), e->execute(
				"class Pipe<T> { function<T(T)> step; }\nclass Wrap<Y> { coroutine<Y> held; }\ncoroutine int gen() { yield 1; }\n"
				"Pipe<function<int()>> p; p.step = (function<int()> f) => f;\nWrap<int> w; w.held = gen();\n"
				"function<int()> seven = () => 7;\n`${p.step(seven)()}|${reflect::type_of(p)}|${reflect::type_of(w.held)}`;").as_string());
			// a field that is the parameter can hold a callable: a copy rebinds a lambda bound to the instance it was copied from
			check_eq(std::string("1|6"), e->execute(
				"class Counter<F> { int n; F bump; void arm() { bump = [this](int by) { n += by; return n; }; } }\n"
				"Counter<function<int(int)>> c; c.arm(); c.bump(1);\nCounter<function<int(int)>> d = c; d.bump(5);\n`${c.n}|${d.n}`;").as_string());
			e->execute(box + "class Stepper { coroutine<int> v; } Box<function<int(int)>> held; Box<coroutine<int>> stepping; Box<int> plain; Stepper by_hand;");
			check_true(detail::engine_access::get_class_definition(*e, "Box<function<int(int)>>")->value_tree_holds_callable());
			check_true((detail::engine_access::get_class_definition(*e, "Counter<function<int(int)>>")->copy_care() & class_definition::copy_care_watch) != 0);
			check_eq(detail::engine_access::get_class_definition(*e, "Stepper")->value_tree_holds_callable(),
			         detail::engine_access::get_class_definition(*e, "Box<coroutine<int>>")->value_tree_holds_callable(), "an instance answers as the class written by hand");
			check_false(detail::engine_access::get_class_definition(*e, "Box<int>")->value_tree_holds_callable());
		});

		// function<U(T)> against a function value is a walk over the signature: the result, then each parameter
		test("a_type_argument_is_deduced_through_a_signature", [this]() {
			auto e = stdlib_engine();
			const std::string run = "T run<T>(function<T()> f) { return f(); }\nU apply<T, U>(function<U(T)> f, T x) { return f(x); }\nT pick<T>(T a) { return a; }\n";
			// from a function's name, a lambda that declares its types, and a variable declared with a signature
			check_eq(std::string("1|2|ab|3"), e->execute(run + "int one() { return 1; }\nfunction<int()> three = () => 3;\n"
				"`${run(one)}|${run(() => 2)}|${run([]() -> string { return \"ab\"; })}|${run(three)}`;").as_string());
			check_eq(std::string("5.0|n4|true"), e->execute(run + "string label(int n) { return \"n\" + to_string(n); }\nfunction<bool(string)> filled = (string s) => s.size() > 0;\n"
				"`${apply((int a) => a * 2.5, 2)}|${apply(label, 4)}|${apply(filled, \"x\")}`;").as_string());
			// a parameter that is the type parameter takes a slot's own signature
			check_eq((int64_t)5, e->execute(run + "function<int(int)> inc = (int a) => a + 1;\nauto same = pick(inc);\nsame(4);").as_int());
			// through a coroutine type too
			check_eq((int64_t)9, e->execute("T first<T>(coroutine<T> c) { return c.next(); }\ncoroutine int gen() { yield 9; }\nfirst(gen());").as_int());
			// a signature's parameter and another argument must agree, and a signature of another count matches nothing
			check_true(contains(failure(*e, run + "apply((string s) => 1, 2);"), "cannot deduce T for 'apply': it is string from argument 1 and int from argument 2"));
			check_true(contains(failure(*e, run + "run((int a) => a);"), "cannot deduce T for 'run': argument 1 is function<int(int)>, which 'function<T()>' does not match; write run<T>(...)"));
			// what declares no one signature still says nothing: an overloaded name, a bare function slot, a lambda where the
			// parameter is the type parameter itself
			const std::string nothing = "cannot deduce T for 'run': argument 1 is a function, which says nothing of a type; write run<T>(...)";
			check_true(contains(failure(*e, run + "int two() { return 2; }\nint two(int x) { return x; }\nrun(two);"), nothing));
			check_true(contains(failure(*e, run + "function any = () => 1;\nrun(any);"), nothing));
			check_true(contains(failure(*e, run + "pick((int a) => a);"), "cannot deduce T for 'pick': argument 1 is a function, which says nothing of a type; write pick<T>(...)"));
			check_true(contains(failure(*e, run + "run(() => { });"), "cannot deduce T for 'run': argument 1 is function<void()>, which 'function<T()>' does not match; write run<T>(...)"));
		});

		// The other arguments are read first; an arrow whose parameters declare no type takes them from what those found, and its
		// result then gives what is left
		test("an_untyped_arrow_is_typed_from_the_arguments_beside_it_and_its_result_deduces", [this]() {
			auto e = stdlib_engine();
			const std::string map_all = "array<U> map_all<T, U>(array<T> xs, function<U(T)> f) { array<U> out; for (auto x : xs) { out.push(f(x)); } return out; }\n";
			check_eq(std::string("[2, 4, 6]|array<int>"), e->execute(map_all + "auto doubled = map_all([1, 2, 3], x => x * 2);\n`${doubled}|${reflect::type_name(doubled)}`;").as_string());
			check_eq(std::string("[\"1!\", \"2!\"]|[1, 2]|[0.5]"), e->execute(map_all + "class Item { int n; Item(int a) : n(a) {} }\narray<Item> items = [Item(1), Item(2)];\n"
				"`${map_all([1, 2], x => to_string(x) + \"!\")}|${map_all(items, (it) => it.n)}|${map_all([1], x => { return x / 2.0; })}`;").as_string());
			// whichever side of the call the arrow stands on, and beside a parameter it declares
			check_eq(std::string("7|ab3"), e->execute("U feed<T, U>(function<U(T)> f, T x) { return f(x); }\nV join<T, U, V>(function<V(T, U)> f, T a, U b) { return f(a, b); }\n"
				"`${feed(x => x + 1, 6)}|${join((string s, n) => s + to_string(n), \"ab\", 3)}`;").as_string());
			// the arrow's body is checked with the type its parameter took
			check_true(contains(failure(*e, map_all + "map_all([1, 2], x => x.nope);"), "int has no member 'nope'"));
			// with nothing beside it to read its parameter from, the arrow says nothing
			check_true(contains(failure(*e, "U conv<T, U>(function<U(T)> f) { return f(); }\nconv(x => x);"),
				"cannot deduce T for 'conv': argument 1 is a function, which says nothing of a type; write conv<T, U>(...)"));
			// a result the load cannot name leaves its parameter to be written
			check_true(contains(failure(*e, map_all + "var loose = 1;\nmap_all([1, 2], x => loose);"),
				"cannot deduce U for 'map_all': argument 2 is a function, which says nothing of a type; write map_all<int, U>(...)"));
		});

		test("an_instance_made_with_a_callable_type_keeps_its_name_where_names_travel", [this]() {
			auto e = stdlib_engine();
			e->execute("class Tagged<T> { int n = 0; }\nTagged<function<int(int)>> t; t.n = 5;\nTagged<function<int(int, string)>> two;\nTagged<coroutine<int>> stepping;");
			check_eq(std::string("Tagged<function<int(int)>>|Tagged<function<int(int,string)>>|Tagged<coroutine<int>>"),
				e->execute("reflect::type_of(t) + \"|\" + reflect::type_name(two) + \"|\" + reflect::type_of(stepping);").as_string());
			check_eq(std::string("{\"$type\":\"Tagged<function<int(int)>>\",\"$val\":{\"n\":5}}"), e->execute("to_json(t);").as_string());
			check_eq(std::string("Tagged<function<int(int)>>|5"), e->execute("auto back = from_json(to_json(t)); reflect::type_of(back) + \"|\" + to_string(back.n);").as_string());
			check_eq(std::string("Tagged<function<int(int)>>|5"), e->execute("auto again = from_binary(to_binary(t)); reflect::type_of(again) + \"|\" + to_string(again.n);").as_string());
			check_true(contains(e->execute("to_binary(t);").as_string(), "Tagged<function<int(int)>>"));
			check_true(e->execute("reflect::classes().contains(\"Tagged<function<int(int)>>\") && reflect::classes().contains(\"Tagged<coroutine<int>>\");").as_bool());
			// a name that arrives as text is read without its spacing
			for (const auto& [spelled, name] : std::vector<std::pair<const char*, const char*>>{{"Tagged< function<int( int )> >", "Tagged<function<int(int)>>"},
					{"Tagged<function<int(int, string)>>", "Tagged<function<int(int,string)>>"}, {"Tagged < coroutine < int > >", "Tagged<coroutine<int>>"}}) {
				const std::string quoted = "\"" + std::string(spelled) + "\"";
				check_eq(std::string(name), e->execute("reflect::type_of(reflect::construct(" + quoted + "));").as_string(), "reflect::construct " + std::string(spelled));
				check_true(e->execute("reflect::is_instance_of(reflect::construct(" + quoted + "), " + quoted + ");").as_bool(), "reflect::is_instance_of " + std::string(spelled));
				check_eq(std::string(name), e->execute("reflect::type_of(from_json(\"{\\\"$type\\\":\\\"" + std::string(spelled) + "\\\",\\\"$val\\\":{}}\"));").as_string(), "from_json " + std::string(spelled));
				check_eq(std::string(name), detail::registered_type_spelling(e.get(), spelled), "the registered spelling of " + std::string(spelled));
			}
			// a saved bite carries the instance and a fresh engine declares it
			const std::string unit = "class Box<T> { T v; }\nBox<function<int(int)>> b; b.v = (int a) => a + 1;\ncoroutine int gen() { yield 4; }\nBox<coroutine<int>> c; c.v = gen();\n"
				"`${b.v(2)}|${c.v.next()}|${reflect::type_of(b)}|${reflect::type_of(c)}`;";
			auto bite = e->jaibite(unit);
			auto fresh = stdlib_engine();
			check_eq(std::string("3|4|Box<function<int(int)>>|Box<coroutine<int>>"), fresh->jaibite_load_bytes(bite.save_bytes()).execute().as_string());
		});

		// A generic class is a class of its namespace to another file's block of that namespace, as a plain class is: its bare
		// name resolves there whichever file is imported first (engine::namespace_class_known answers for both)
		test("a_namespaces_generic_class_is_named_bare_from_another_files_block", [this]() {
			const std::filesystem::path directory = unit_directory("namespace_files");
			write_unit(directory, "declaring.jai", R"(
				namespace shop {
					class Item { int price = 3; }
					class Plain { int n = 1; }
					class Shelf<T> { T held; int n = 1; }
					namespace inner { class Deep<T> { T d; } class Flat { int f = 2; } }
				}
			)");
			const std::string naming = R"(
				namespace shop {
					int generic() { Shelf<Item> s; return s.held.price + s.n; }
					int plain() { Plain p; return p.n; }
					int ints() { Shelf<int> s; s.held = 5; return s.held; }
					int held() { Shelf<Plain> s; return s.held.n + s.n; }
					namespace inner { int deep() { Deep<Item> d; Flat f; Shelf<int> s; return d.d.price + f.f + s.n; } }
				}
				string report() { shop::Shelf<shop::Item> last; return `${shop::generic()} ${shop::plain()} ${shop::ints()} ${shop::held()} ${shop::inner::deep()} ${reflect::type_of(last)}`; }
			)";
			write_unit(directory, "naming.jai", naming);
			write_unit(directory, "naming_imports.jai", "import \"declaring.jai\";\n" + naming);
			write_unit(directory, "naming_using.jai", "using shop::Shelf, shop::Item, shop::Plain;\nint outside() { Shelf<Item> s; Plain p; return s.held.price + p.n; }");
			const std::string expected = "4 1 5 2 6 shop::Shelf<shop::Item>";
			const std::pair<std::string, std::string> programs[] = {
				{"include \"declaring.jai\"; include \"naming.jai\"; report();", expected},   // one unit: what every program below must match
				{"import \"declaring.jai\"; import \"naming.jai\"; report();", expected},
				{"import \"naming.jai\"; import \"declaring.jai\"; report();", expected},
				{"import \"naming_imports.jai\"; report();", expected},
				{"import \"declaring.jai\"; import \"naming_using.jai\"; to_string(outside());", "4"},
				{"import \"naming_using.jai\"; import \"declaring.jai\"; to_string(outside());", "4"},
			};
			for (const auto& [program, wanted] : programs) {
				auto e = stdlib_engine();
				e->jaibite_cache(false);
				e->add_include_path(directory.string());
				std::string got;
				try { got = e->execute(program).to_string(); } catch (const std::exception& error) { got = error.what(); }
				check_eq(wanted, got.substr(0, got.find('\n')), program);
			}
		});

		test("a_parallel_body_uses_an_instance", [this]() {
			auto e = stdlib_engine();
			check_eq(std::string("[2, 4, 6, 8]"), e->execute(R"(
				class Acc<T> { T total; void add(T x) { total += x; } }
				T doubled<T>(T x) { Acc<T> a; a.add(x); a.add(x); return a.total; }
				array<int> xs = [1, 2, 3, 4];
				parallel_for (auto& x : xs) { x = doubled<int>(x); }
				to_string(xs);
			)").as_string());
		});

		test("a_breakpoint_in_a_generic_stops_in_its_instance", [this]() {
			for (const backend_type type : {backend_type::interpreter, backend_type::vm}) {
				auto e = make_default_engine();
				e->set_backend(type);
				auto& dbg = e->debugger();
				std::mutex m;
				std::condition_variable cv;
				bool stopped = false;
				debug::stop_info last;
				dbg.set_on_stopped([&](const debug::stop_info& si) {
					std::lock_guard<std::mutex> lock(m);
					last = si;
					stopped = true;
					cv.notify_all();
				});
				dbg.set_breakpoints("<script>", {3});
				dbg.set_enabled(true);
				const std::string source =
					"class Box<T> {\n"
					"\tT item;\n"
					"\tT twice() { return item + item; }\n"   // line 3: the generic's own line
					"}\n"
					"Box<int> b;\n"
					"b.item = 4;\n"
					"int result = b.twice();\n";
				std::thread script([&] { e->execute(source); });
				{
					std::unique_lock<std::mutex> lock(m);
					check_true(cv.wait_for(lock, std::chrono::minutes(10), [&] { return stopped; }));
				}
				check_eq(std::string("breakpoint"), last.reason);
				check_eq(3, last.line);
				dbg.resume();
				script.join();
				check_eq((int64_t)8, e->get_variable("result").as_int());
			}
		});

		test("less_than_keeps_comparing_where_no_type_argument_list_closes", [this]() {
			auto e = make_engine();
			check_true(e->execute("int a = 1; int b = 2; a < b;").as_bool());
			check_true(e->execute("int a = 1; int b = 2; int c = 0; a < b && b > (c);").as_bool());
			check_true(e->execute("int a = 1; int b = 2; int c = 3; int d = 4; bool both(bool x, bool y) { return x && y; } both(a < b, c < d);").as_bool());
			check_true(e->execute("int a = 1; int b = 2; int c = 3; int d = 1; bool both(bool x, bool y) { return x && y; } both(a < b, c > d);").as_bool());
			check_eq((int64_t)3, e->execute("int n = 3; int i = 0; for (i = 0; i < n; i += 1) {} i;").as_int());
		});

		// A namespace lists a class under its bare name, which for an instance holds its arguments: Shelf<shop::Item> of shop,
		// cut at the last '::' outside them
		test("a_namespace_generics_instance_made_with_a_namespace_class_is_constructed_by_name", [this]() {
			auto e = stdlib_engine();
			check_eq(std::string("shop::Shelf<shop::Item>|shop::Shelf<array<shop::Item>>|shop::Shelf<shop::Shelf<int>>|shop::Shelf<shop::Item>|3|4|1|2"), e->execute(
				"namespace shop {\n\tclass Item { int n = 1; }\n\tclass Shelf<T> { T item; static int made = 3; static int count() { return 4; } int size() { return 2; } }\n"
				"\tstring inside() { auto s = Shelf<Item>(); return reflect::type_of(s); }\n}\n"
				"auto a = shop::Shelf<shop::Item>();\nauto b = shop::Shelf<array<shop::Item>>();\nauto c = shop::Shelf<shop::Shelf<int>>();\n"
				"`${reflect::type_of(a)}|${reflect::type_of(b)}|${reflect::type_of(c)}|${shop::inside()}|${shop::Shelf<shop::Item>::made}|${shop::Shelf<shop::Item>::count()}|${a.item.n}|${c.item.size()}`;").as_string());
			// in a later unit too, and an instance of a program-scope generic keeps its one name
			check_eq(std::string("2|Box<shop::Item>"), e->execute("class Box<T> { T v; }\nauto later = shop::Shelf<shop::Item>();\nauto boxed = Box<shop::Item>();\n"
				"`${later.size()}|${reflect::type_of(boxed)}`;").as_string());
		});

		// Type{...} after a qualified name is the construction it is after a bare one: a namespace's class, a namespace generic's
		// instance, and a type parameter whose argument is either
		test("a_braced_construction_takes_a_qualified_name", [this]() {
			auto e = stdlib_engine();
			check_eq(std::string("1|2|shop::Shelf<int>|7|shop::Shelf<shop::Item>|1|2"), e->execute(
				"namespace shop {\n\tclass Item { int n = 1; }\n\tclass Pt { int x; int y; }\n\tclass Shelf<T> { T item; int size() { return 2; } }\n}\n"
				"class Box<T> { T fresh() { return T{}; } }\n"
				"auto i = shop::Item{};\nauto s = shop::Shelf<int>{};\nauto p = shop::Pt{3, 4};\nauto both = shop::Shelf<shop::Item>{};\n"
				"Box<shop::Item> items;\nBox<shop::Shelf<int>> shelves;\n"
				"`${i.n}|${s.size()}|${reflect::type_of(s)}|${p.x + p.y}|${reflect::type_of(both)}|${items.fresh().n}|${shelves.fresh().size()}`;").as_string());
			check_eq((int64_t)5, e->execute("namespace a { namespace b { class Deep { int n = 5; } } }\nauto d = a::b::Deep{};\nd.n;").as_int());
			// each place a bare name's braces stand takes the qualified name's beside it: an initializer, an argument, a declaration,
			// an element, a result, a receiver, a condition, a generic's instance, and the namespace's own code
			check_eq(std::string("M0001_Initial M0001_Initial|42 42|3 3|4 4|5 5|8 8|2 2|yes yes|10 10"), e->execute(
				"namespace mv {\n\tclass Migration { int version; string name; function apply; }\n\tclass Pt { int x; int y; }\n\tclass Shelf<T> { T item; int size() { return 2; } }\n"
				"\tint inside() { return Pt{9, 1}.x + mv::Pt{0, 1}.y; }\n}\n"
				"class Migration { int version; string name; function apply; }\nclass Pt { int x; int y; }\nclass Shelf<T> { T item; int size() { return 2; } }\n"
				"int run(mv::Migration m) { return m.apply() + m.version; }\nint run_plain(Migration m) { return m.apply() + m.version; }\n"
				"mv::Pt make() { return mv::Pt{5, 6}; }\nPt make_plain() { return Pt{5, 6}; }\n"
				"function fn = () => 40;\nauto q = mv::Migration{1, \"M0001_Initial\", fn};\nauto u = Migration{1, \"M0001_Initial\", fn};\n"
				"mv::Pt a{1, 2};\nPt b{1, 2};\narray<mv::Pt> qs = [mv::Pt{1, 2}, mv::Pt{3, 4}];\narray<Pt> us = [Pt{1, 2}, Pt{3, 4}];\n"
				"string met = mv::Pt{1, 2}.x == 1 ? \"yes\" : \"no\";\nstring met_plain = Pt{1, 2}.x == 1 ? \"yes\" : \"no\";\n"
				"`${q.name} ${u.name}|${run(mv::Migration{2, \"x\", fn})} ${run_plain(Migration{2, \"x\", fn})}|${a.x + a.y} ${b.x + b.y}|${qs[1].y} ${us[1].y}|${make().x} ${make_plain().x}`"
				" + `|${mv::Pt{7, 8}.y} ${Pt{7, 8}.y}|${mv::Shelf<mv::Pt>{}.size()} ${Shelf<Pt>{}.size()}|${met} ${met_plain}|${mv::inside()} ${Pt{9, 1}.x + Pt{0, 1}.y}`;").as_string());
			// what is no type's name takes no brace initializer, as before
			check_true(contains(failure(*e, "class Holder { int n = 1; }\nHolder h;\nauto x = h.n{};"), "expected ';' after expression"));
			check_true(contains(failure(*e, "namespace shop { class Solo<T> { T v; } }\nauto x = shop::Solo{};"),
				"'shop::Solo' is a generic class: name its type arguments (shop::Solo<T>)"));
		});

		// v < n, n > (v) is two comparisons after every variable a declaration introduces, at program scope as in a function
		test("less_than_compares_after_every_declared_variable", [this]() {
			const std::string prelude =
				"bool both(bool a, bool b) { return a && b; }\n"
				"int n = 2;\n"
				"int get() { return 1; }\n"
				"array<int> xs = [1, 5];\n"
				"map<string, int> m = {\"a\": 1};\n";
			const std::vector<std::pair<std::string, std::string>> statements = {
				{"for (int i = 0; i < 3; i += 1) { if (both(i < n, n > (i))) { print(i); } }", "0\n1"},
				{"for (auto v : xs) { print(both(v < n, n > (v))); }", "true\nfalse"},
				{"for (int v : xs) { print(both(v < n, n > (v))); }", "true\nfalse"},
				{"for (const v : xs) { print(both(v < n, n > (v))); }", "true\nfalse"},
				{"for (auto& v : xs) { print(both(v < n, n > (v))); }", "true\nfalse"},
				{"for (auto [k, w] : m) { print(both(w < n, n > (w))); }", "true"},
				{"for (auto [k, w] : m) { print(both(k < \"b\", \"b\" > (k))); }", "true"},
				{"if (auto d = get(); both(d < n, n > (d))) { print(d); }", "1"},
				{"if (auto d = get(); d > 0) { print(both(d < n, n > (d))); } else { print(d); }", "true"},
				{"if (int d = get()) { print(both(d < n, n > (d))); }", "true"},
				{"if (int d = 0) { print(d); } else { print(both(d < n, n > (d))); }", "true"},
				{"while (auto d = get()) { print(both(d < n, n > (d))); break; }", "true"},
				{"try { throw 3; } catch (e) { print(both(e < n, n > (e))); }", "false"},
				{"try { throw 1; } catch (var e) { print(both(e < n, n > (e))); }", "true"},
				{"try { throw 1; } catch (auto e) { print(both(e < n, n > (e))); }", "true"},
				{"try { throw 1; } catch (int e) { print(both(e < n, n > (e))); }", "true"},
				{"{ int v = 1; print(both(v < n, n > (v))); }", "true"},
				{"{ int v = 1; { print(both(v < n, n > (v))); } }", "true"},
				{"{ const int v = 1; print(both(v < n, n > (v))); }", "true"},
				{"{ auto [p, q] = [1, 5]; print(both(p < n, n > (p))); print(both(q < n, n > (q))); }", "true\nfalse"},
				{"auto [p, q] = [1, 5]; print(both(p < n, n > (p)));", "true"},
				{"if (true) { int v = 1; print(both(v < n, n > (v))); }", "true"},
				{"switch (1) { case 1: { int v = 1; print(both(v < n, n > (v))); } }", "true"},
				{"int k = 0; do { int v = 1; print(both(v < n, n > (v))); k += 1; } while (k < 1);", "true"},
				{"{ int v = 1; auto f = [&]() { return both(v < n, n > (v)); }; print(f()); }", "true"},
			};
			std::vector<std::pair<std::string, std::string>> cases;
			for (const auto& [statement, printed] : statements) {
				cases.push_back({statement, printed});
				cases.push_back({"void go() { " + statement + " }\ngo();", printed});
			}
			check_outputs(cases, prelude, true);
		});

		// the variable counts in its scope: after it, the name is the generic the program declared
		test("a_declared_variable_stops_comparing_where_its_scope_ends", [this]() {
			const std::string prelude =
				"class Box { int held = 7; }\n"
				"T pick<T>(T a) { return a; }\n"
				"Box b;\n"
				"array<int> xs = [1];\n";
			const std::vector<std::pair<std::string, std::string>> statements = {
				{"for (int pick = 0; pick < 1; pick += 1) { } print(pick<Box>(b).held);", "7"},
				{"for (auto pick : xs) { } print(pick<Box>(b).held);", "7"},
				{"{ int pick = 1; } print(pick<Box>(b).held);", "7"},
				{"if (int pick = 1) { } print(pick<Box>(b).held);", "7"},
				{"try { throw 1; } catch (pick) { } print(pick<Box>(b).held);", "7"},
				{"{ auto [pick, other] = [1, 2]; } print(pick<Box>(b).held);", "7"},
			};
			std::vector<std::pair<std::string, std::string>> cases;
			for (const auto& [statement, printed] : statements) {
				cases.push_back({statement, printed});
				cases.push_back({"void go() { " + statement + " }\ngo();", printed});
			}
			check_outputs(cases, prelude, true);
		});

		test("the_typed_result_options_are_spelled_returns_of_a_shape", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			engine* raw = e.get();
			e->add_function("echo_as", [raw](const script_type& t, script_int n) {
				(void)t;
				return script_value(n, raw);
			}, returns<type_arg>);
			check_eq(static_cast<uint8_t>(1), e->type_argument_result("echo_as"));
			check_eq(static_cast<uint8_t>(1), returns<type_arg>.shape);
			check_eq(static_cast<uint8_t>(2), returns<array_of<type_arg>>.shape);
			check_eq(static_cast<uint8_t>(3), returns<shared_ptr_of<type_arg>>.shape);
			check_eq((int64_t)5, e->execute("int n = echo_as<int>(5); n;").as_int());
		});
	}
};

}

using generics_tests = jai::foundry::tests::generics_tests;
FOUNDRY_REGISTER(generics_tests)
