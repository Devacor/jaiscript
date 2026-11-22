// '<' after a name (docs/site/guide/06-functions.html "Type arguments"): type arguments need
// something declared at load that takes them; every other name compares, as it did before a call could name types
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/testing/timing.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/detail/engine_access.hpp>
#include <jaiscript/detail/less_than.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace jai::foundry::tests {

class less_than_tests : public suite {
public:
	less_than_tests() : suite("Less Than By Name") {}

	static bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
	static std::shared_ptr<engine> stdlib_engine() {
		auto e = make_engine();
		stdlib::register_all(*e);
		return e;
	}
	static std::string failure(engine& eng, const std::string& code) {
		try {
			eng.execute(code);
		} catch (const std::exception& e) {
			return e.what();
		}
		return "(no error)";
	}
	static std::filesystem::path unit_directory(const std::string& test) {
		const std::filesystem::path directory = test_temp_directory() / ("less_than_" + test);
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

	// The forms a comparison pair takes, over the spellings of a low value, a high one and a false: each is true
	struct form {
		const char* name;
		std::string (*spell)(const std::string& lo, const std::string& hi, const std::string& no);
	};
	static std::vector<form> forms() {
		return {
			{"call arguments", [](const std::string& lo, const std::string& hi, const std::string&) { return "both(" + lo + " < " + hi + ", " + hi + " > (" + lo + "))"; }},
			{"array literal", [](const std::string& lo, const std::string& hi, const std::string&) { return "all([" + lo + " < " + hi + ", " + hi + " > (" + lo + ")])"; }},
			{"braced init", [](const std::string& lo, const std::string& hi, const std::string&) { return "Two{" + lo + " < " + hi + ", " + hi + " > (" + lo + ")}.ok()"; }},
			{"single chain", [](const std::string& lo, const std::string& hi, const std::string& no) { return lo + " < " + hi + " > (" + no + ")"; }},
			{"scope follower", [](const std::string& lo, const std::string& hi, const std::string&) { return lo + " < " + hi + " > ::program_no"; }},
		};
	}
	// what every program of the table declares around its case
	static std::string prelude() {
		return "bool both(bool a, bool b) { return a && b; }\n"
		       "bool all(array<bool> xs) { return xs[0] && xs[1]; }\n"
		       "class Two { bool a; bool b; bool ok() { return a && b; } }\n"
		       "bool program_no = false;\n";
	}
	// Where the pair's names are declared: the text around an expression `E`, and how the names are spelled there
	struct place {
		const char* name;
		std::string before, after;   // before + "return " + E + ";" + after declares bool t()
		std::string lo, hi, no;
	};
	static std::vector<place> places() {
		return {
			{"local", "bool t() { int lo = 1; int hi = 5; bool no = false; ", " }\n", "lo", "hi", "no"},
			{"parameter", "bool t2(int lo, int hi, bool no) { ", " }\nbool t() { return t2(1, 5, false); }\n", "lo", "hi", "no"},
			{"field above", "class S { int lo = 1; int hi = 5; bool no = false; bool t() { ", " } }\nbool t() { return S().t(); }\n", "lo", "hi", "no"},
			{"field below", "class S { bool t() { ", " } int lo = 1; int hi = 5; bool no = false; }\nbool t() { return S().t(); }\n", "lo", "hi", "no"},
			{"inherited field", "class B { int lo = 1; int hi = 5; bool no = false; }\nclass S : B { bool t() { ", " } }\nbool t() { return S().t(); }\n", "lo", "hi", "no"},
			{"field in a lambda", "class S { int lo = 1; int hi = 5; bool no = false; bool t() { auto g = [this]() { ", " }; return g(); } }\nbool t() { return S().t(); }\n", "lo", "hi", "no"},
			{"local in a lambda", "bool t() { int lo = 1; int hi = 5; bool no = false; auto g = [&]() { ", " }; return g(); }\n", "lo", "hi", "no"},
			{"static field", "class S { static int lo = 1; static int hi = 5; static bool no = false; static bool t() { ", " } }\nbool t() { return S::t(); }\n", "lo", "hi", "no"},
			{"static field below", "class S { static bool t() { ", " } static int lo = 1; static int hi = 5; static bool no = false; }\nbool t() { return S::t(); }\n", "lo", "hi", "no"},
			{"qualified static", "class Cfg { static int lo = 1; static int hi = 5; static bool no = false; }\nbool t() { ", " }\n", "Cfg::lo", "Cfg::hi", "Cfg::no"},
			{"qualified static below", "bool t() { ", " }\nclass Cfg { static int lo = 1; static int hi = 5; static bool no = false; }\n", "Cfg::lo", "Cfg::hi", "Cfg::no"},
			{"namespace variable", "namespace cfg { int lo = 1; int hi = 5; bool no = false; bool t() { ", " } }\nbool t() { return cfg::t(); }\n", "lo", "hi", "no"},
			{"namespace variable below", "namespace cfg { bool t() { ", " } int lo = 1; int hi = 5; bool no = false; }\nbool t() { return cfg::t(); }\n", "lo", "hi", "no"},
			{"namespace variable, another block", "namespace cfg { bool t() { ", " } }\nnamespace cfg { int lo = 1; int hi = 5; bool no = false; }\nbool t() { return cfg::t(); }\n", "lo", "hi", "no"},
			{"enclosing namespace's variable", "namespace cfg { int lo = 1; int hi = 5; bool no = false; namespace deep { bool t() { ", " } } }\nbool t() { return cfg::deep::t(); }\n", "lo", "hi", "no"},
			{"qualified namespace variable", "namespace cfg { int lo = 1; int hi = 5; bool no = false; }\nbool t() { ", " }\n", "cfg::lo", "cfg::hi", "cfg::no"},
			{"program-scope variable", "int lo = 1; int hi = 5; bool no = false;\nbool t() { ", " }\n", "lo", "hi", "no"},
			{"program-scope variable below", "bool t() { ", " }\nint lo = 1; int hi = 5; bool no = false;\n", "lo", "hi", "no"},
			{"member head", "class P { int lo = 1; int hi = 5; bool no = false; }\nbool t() { P p; ", " }\n", "p.lo", "p.hi", "p.no"},
			{"this member head", "class S { int lo = 1; int hi = 5; bool no = false; bool t() { ", " } }\nbool t() { return S().t(); }\n", "this.lo", "this.hi", "this.no"},
			{"coroutine", "coroutine bool c() { int lo = 1; int hi = 5; bool no = false; yield ", "; }\nbool t() { bool r = false; for (auto v : c()) { r = v; } return r; }\n", "lo", "hi", "no"},
			{"field in a coroutine", "class S { int lo = 1; int hi = 5; bool no = false; coroutine bool c() { yield ", "; } }\nbool t() { S s; bool r = false; for (auto v : s.c()) { r = v; } return r; }\n", "lo", "hi", "no"},
			{"generic class's field", "class S<T> { T lo; T hi; bool no = false; bool t() { lo = 1; hi = 5; ", " } }\nbool t() { S<int> s; return s.t(); }\n", "lo", "hi", "no"},
			{"generic class's field below", "class S<T> { bool t() { lo = 1; hi = 5; ", " } T lo; T hi; bool no = false; }\nbool t() { S<int> s; return s.t(); }\n", "lo", "hi", "no"},
			{"field through a generic base", "class B<T> { T lo; T hi; bool no = false; }\nclass S : B<int> { bool t() { lo = 1; hi = 5; ", " } }\nbool t() { return S().t(); }\n", "lo", "hi", "no"},
			{"program-scope variable in a generic function", "int lo = 1; int hi = 5; bool no = false;\nbool g<T>(T x) { ", " }\nbool t() { return g<int>(0); }\n", "lo", "hi", "no"},
			{"program-scope variable below a generic class", "class S<T> { T x; bool t() { ", " } }\nint lo = 1; int hi = 5; bool no = false;\nbool t() { S<int> s; return s.t(); }\n", "lo", "hi", "no"},
		};
	}
	static std::string program(const place& where, const form& shape, bool statement_return = true) {
		const std::string expression = shape.spell(where.lo, where.hi, where.no);
		const bool yields = std::string(where.before).find("yield ") != std::string::npos && std::string(where.before).rfind("yield ") + 6 == where.before.size();
		return prelude() + where.before + (yields || !statement_return ? "" : "return ") + expression + (yields ? "" : ";") + where.after;
	}

	void forge_tests() override {
		// every head a variable can be, in every form, compares
		test("a_variable_compares_wherever_it_is_declared", [this]() {
			for (const place& where : places()) {
				for (const form& shape : forms()) {
					auto e = stdlib_engine();
					const std::string code = program(where, shape) + "t();";
					bool value = false;
					std::string error;
					try { value = e->execute(code).as_bool(); } catch (const std::exception& ex) { error = ex.what(); }
					check_true(value, std::string(where.name) + " / " + shape.name + ": " + error + "\n" + code);
				}
			}
		});
		// a generic of the head's name declared at program scope does not take a nearer variable's '<'
		test("a_nearer_variable_wins_over_a_generic_of_its_name", [this]() {
			for (const place& where : places()) {
				const std::string name(where.name);
				if (name.find("program-scope") != std::string::npos || name.find("member head") != std::string::npos || name.find("qualified") != std::string::npos) { continue; }
				for (const form& shape : forms()) {
					auto e = stdlib_engine();
					const std::string code = "T lo<T>(T x) { return x; }\n" + program(where, shape) + "t() && lo<int>(3) == 3;";
					bool value = false;
					std::string error;
					try { value = e->execute(code).as_bool(); } catch (const std::exception& ex) { error = ex.what(); }
					check_true(value, name + " / " + shape.name + ": " + error + "\n" + code);
				}
			}
		});
		// the kernel answers by the checker's own lookup: each kind of name, asked where it stands
		test("the_lookup_answers_what_the_checker_resolves", [this]() {
			auto e = stdlib_engine();
			e->add_global("host_value", script_value(script_int(3), e.get()));
			e->execute("int ran_value = 1; int ran_function() { return 1; } class RanClass { int field = 1; static int fixed = 2; int method() { return 1; } }\n"
			           "T ran_generic<T>(T x) { return x; } namespace ran { int inner = 1; int call() { return 1; } } enum RanLevel { ran_low, ran_high }");
			lexer lex("class Own : RanClass { int mine = 1; int act() { return 1; } T conv<T>(T x) { return x; } }\n"
			          "class Gen<T> { T held; T get() { return held; } }\nclass Kid : Gen<int> { }\n"
			          "namespace here { int v = 1; int f() { return 1; } namespace deep { int w = 1; } }\n"
			          "int own_value = 1; int own_function() { return 1; } enum Level { low, high }\n",
			          detail::engine_access::get_symbolizer(*e));
			const auto tokens = lex.tokenize();
			parser parse(tokens, detail::engine_access::get_symbolizer(*e), e.get(), detail::engine_access::get_registered_template_types(*e));
			auto parsed = parse.parse();
			check_true(static_cast<bool>(parsed));
			const auto ask = [&](detail::less_head form, const std::string& cls, const std::string& space, const std::string& head) {
				detail::less_query query;
				query.form = form;
				query.cls = cls;
				query.space = space;
				query.head = head;
				return detail::engine_access::less_than_answers(*e, parsed.value(), {query.text()})[0];
			};
			using detail::less_head;
			struct row { less_head form; const char* cls; const char* space; const char* head; char expected; };
			const std::vector<row> rows = {
				{less_head::bare, "", "", "own_value", 'v'}, {less_head::bare, "", "", "own_function", 'd'}, {less_head::bare, "", "", "Own", 'd'},
				{less_head::bare, "", "", "Gen", 'd'}, {less_head::bare, "", "", "Level", 'd'}, {less_head::bare, "", "", "low", 'n'},
				{less_head::bare, "", "", "here", 'd'}, {less_head::bare, "", "", "nothing_at_all", 'n'},
				{less_head::bare, "", "", "ran_value", 'v'}, {less_head::bare, "", "", "ran_function", 'd'}, {less_head::bare, "", "", "RanClass", 'd'},
				{less_head::bare, "", "", "ran_generic", 'd'}, {less_head::bare, "", "", "ran", 'd'}, {less_head::bare, "", "", "ran_low", 'n'},
				{less_head::bare, "", "", "host_value", 'v'}, {less_head::bare, "", "", "print", 'd'}, {less_head::bare, "", "", "from_json", 'd'},
				{less_head::bare, "Own", "", "mine", 'v'}, {less_head::bare, "Own", "", "act", 'd'}, {less_head::bare, "Own", "", "conv", 'd'},
				{less_head::bare, "Own", "", "field", 'v'}, {less_head::bare, "Own", "", "fixed", 'v'}, {less_head::bare, "Own", "", "method", 'd'},
				{less_head::bare, "Own", "", "own_value", 'v'}, {less_head::bare, "Own", "", "nothing_at_all", 'n'},
				{less_head::bare, "Gen", "", "held", 'v'}, {less_head::bare, "Gen", "", "get", 'd'},
				{less_head::bare, "Kid", "", "held", 'v'}, {less_head::bare, "Kid", "", "get", 'd'},
				{less_head::bare, "", "here", "v", 'v'}, {less_head::bare, "", "here", "f", 'd'}, {less_head::bare, "", "here", "deep", 'd'},
				{less_head::bare, "", "here::deep", "w", 'v'}, {less_head::bare, "", "here::deep", "v", 'v'}, {less_head::bare, "", "here::deep", "own_value", 'v'},
				{less_head::global, "Own", "here", "mine", 'n'}, {less_head::global, "Own", "here", "own_value", 'v'},
				{less_head::qualified, "", "", "here::v", 'v'}, {less_head::qualified, "", "", "here::f", 'd'}, {less_head::qualified, "", "", "here::deep::w", 'v'},
				{less_head::qualified, "", "here", "deep::w", 'v'}, {less_head::qualified, "", "", "Level::low", 'v'}, {less_head::qualified, "", "", "RanLevel::ran_low", 'v'},
				{less_head::qualified, "", "", "RanClass::fixed", 'v'}, {less_head::qualified, "", "", "RanClass::method", 'd'}, {less_head::qualified, "", "", "Own::conv", 'd'},
				{less_head::qualified, "", "", "ran::inner", 'v'}, {less_head::qualified, "", "", "ran::call", 'd'}, {less_head::qualified, "", "", "nowhere::x", 'n'},
				{less_head::member, "", "", "conv", 'd'}, {less_head::member, "", "", "mine", 'n'}, {less_head::member, "", "", "act", 'n'},
			};
			for (const row& r : rows) {
				const char answer = ask(r.form, r.cls, r.space, r.head);
				check_true(answer == r.expected, std::string(r.cls) + "|" + r.space + "|" + r.head + ": " + std::string(1, answer) + ", expected " + std::string(1, r.expected));
			}
		});
		// type arguments on everything declared that takes them, wherever it is declared
		test("a_generic_takes_type_arguments_wherever_it_is_declared", [this]() {
			const std::string pet = "class Pet { int v = 7; }\n";
			struct use { const char* name; const char* code; int64_t expected; };
			const std::vector<use> uses = {
				{"function above", "T pick<T>(T x) { return x; }\nint t() { return pick<Pet>(Pet()).v; }\nt();", 7},
				{"function below", "int t() { return pick<Pet>(Pet()).v; }\nT pick<T>(T x) { return x; }\nt();", 7},
				{"empty argument list", "int t() { Pet p = make<Pet>(); return p.v; }\nT make<T>() { return T(); }\nt();", 7},
				{"two type arguments in a statement", "int t() { Pet p; keep<Pet, Pet>(p, p); return keep<Pet, Pet>(p, p).v; }\nT keep<T, U>(T x, U y) { return x; }\nt();", 7},
				{"class below, braces", "int t() { Box<Pet> b = Box<Pet>{}; return b.held.v; }\nclass Box<T> { T held; }\nt();", 7},
				{"class below, static member", "int t() { return Box<Pet>::count(3); }\nclass Box<T> { T held; static int count(int n) { return n; } }\nt();", 3},
				{"class below, construction", "int t() { return Box<Pet>(Pet()).held.v; }\nclass Box<T> { T held; Box(T x) { held = x; } }\nt();", 7},
				{"namespace's function", "int t() { return shop::stock<Pet>(Pet()).v; }\nnamespace shop { T stock<T>(T x) { return x; } }\nt();", 7},
				{"namespace's function, bare inside", "namespace shop { int t() { return stock<Pet>(Pet()).v; } T stock<T>(T x) { return x; } }\nshop::t();", 7},
				{"method on a typed receiver", "class U { T2 same<T, T2>(T x, T2 y) { return y; } }\nint t() { U u; return u.same<Pet, int>(Pet(), 9); }\nt();", 9},
				{"method through this", "class U { int n = 4; int get() { return this.same<Pet, int>(Pet(), n); } T2 same<T, T2>(T x, T2 y) { return y; } }\nU().get();", 4},
				{"method by its bare name", "class U { int n = 4; int get() { return same<Pet, int>(Pet(), n); } T2 same<T, T2>(T x, T2 y) { return y; } }\nU().get();", 4},
				{"inside a generic body, by the parameter", "T make<T>() { return T(); }\nclass Box<T> { T g() { return make<T>(); } }\nBox<Pet> b; b.g().v;", 7},
				{"inside a generic body, by a class", "T make<T>() { return T(); }\nclass Box<T> { Pet g() { return make<Pet>(); } }\nBox<int> b; b.g().v;", 7},
				{"a head that begins with a type parameter", "class Tool { static T same<T>(T x) { return x; } }\nclass Box<T> { int g() { return T::same<Pet>(Pet()).v; } }\nBox<Tool> b; b.g();", 7},
				{"host function", "Pet p = from_json<Pet>(\"{\\\"v\\\": 5}\");\np.v;", 5},
				{"host function in an argument list", "int two(int a, int b) { return a + b; }\ntwo(from_json<Pet>(\"{\\\"v\\\": 5}\").v, from_json<Pet>(\"{\\\"v\\\": 2}\").v);", 7},
			};
			for (const use& u : uses) {
				auto e = stdlib_engine();
				int64_t value = -1;
				std::string error;
				try { value = e->execute(pet + u.code).as_int(); } catch (const std::exception& ex) { error = ex.what(); }
				check_eq(u.expected, value, std::string(u.name) + ": " + error);
			}
		});
		test("another_file_s_names_decide_once_the_program_is_gathered", [this]() {
			const auto directory = unit_directory("imports");
			write_unit(directory, "lib.jai", "int lib_lo = 1; int lib_hi = 5;\nclass LibBase { int base_lo = 1; int base_hi = 5; }\nclass Limits { static int floor = 2; }\n"
			                                 "enum Level { low, mid, high }\nT make<T>() { return T(); }\nT keep<T, U>(T x, U y) { return x; }\n"
			                                 "class Shelf<T> { T item; static int size() { return 4; } }\nnamespace shop { int floor = 2; T stock<T>(T x) { return x; } }\n");
			const std::string root = write_unit(directory, "root.jai",
				"import \"lib.jai\";\nclass Pet { int v = 7; }\nbool both(bool a, bool b) { return a && b; }\n"
				"class Kid : LibBase { bool f() { return both(base_lo < base_hi, base_hi > (base_lo)); } }\n"
				"bool a() { return both(lib_lo < lib_hi, lib_hi > (lib_lo)); }\n"
				"bool b() { return both(shop::floor < lib_hi, Limits::floor > (lib_lo)) && both(Level::low < Level::high, Level::high > (Level::low)); }\n"
				"int c() { Pet p = make<Pet>(); keep<Pet, Pet>(p, p); return p.v + Shelf<Pet>::size() + shop::stock<Pet>(Pet()).v; }\n"
				"a() && b() && Kid().f() && c() == 18;\n");
			auto e = stdlib_engine();
			e->jaibite_cache(false);
			check_true(e->execute_file(root).as_bool());
			check_true(e->execute_file(root).as_bool(), "a second load of the settled unit");
			// an import cycle: each file compares the other's variables and calls the other's generic
			const auto cycle = unit_directory("cycle");
			const std::string first = write_unit(cycle, "first.jai",
				"import \"second.jai\";\nclass Pet { int v = 3; }\nint first_lo = 1; int first_hi = 2;\nbool both(bool a, bool b) { return a && b; }\n"
				"T first_of<T>(T x) { return x; }\nbool first_cmp() { return both(second_lo < second_hi, second_hi > (second_lo)) && second_of<Pet>(Pet()).v == 3; }\n"
				"first_cmp() && second_cmp();\n");
			write_unit(cycle, "second.jai",
				"import \"first.jai\";\nint second_lo = 1; int second_hi = 2;\nT second_of<T>(T x) { return x; }\n"
				"bool second_cmp() { return both(first_lo < first_hi, first_hi > (first_lo)) && first_of<Pet>(Pet()).v == 3; }\n");
			auto cyclic = stdlib_engine();
			cyclic->jaibite_cache(false);
			check_true(cyclic->execute_file(first).as_bool());
		});
		// names that arrive after the parse are variables to a program that compares them
		test("a_name_nothing_declares_at_load_compares", [this]() {
			const auto directory = unit_directory("late");
			write_unit(directory, "late_lib.jai", "int lo = 1; int hi = 5;\n");
			const std::string root = write_unit(directory, "root.jai",
				"bool both(bool a, bool b) { return a && b; }\nstring path = \"late\" + \"_lib.jai\";\ninclude(path);\n"
				"bool f() { return both(lo < hi, hi > (lo)); }\nf();\n");
			auto e = stdlib_engine();
			e->jaibite_cache(false);
			check_true(e->execute_file(root).as_bool(), "a computed-path include's variables");
			auto vars = stdlib_engine();
			instance_variables given;
			given.emplace("lo", script_value(script_int(1), vars.get()));
			given.emplace("hi", script_value(script_int(5), vars.get()));
			check_true(vars->execute("bool both(bool a, bool b) { return a && b; }\nboth(lo < hi, hi > (lo));", given).as_bool(), "an execute's instance variables");
			auto host = stdlib_engine();
			host->add_global("lo", script_value(script_int(1), host.get()));
			host->add_global("hi", script_value(script_int(5), host.get()));
			check_true(host->execute("bool both(bool a, bool b) { return a && b; }\nbool f() { return both(lo < hi, hi > (lo)); }\nf();").as_bool(), "a host's variables");
			auto earlier = stdlib_engine();
			earlier->execute("int lo = 1; int hi = 5; bool both(bool a, bool b) { return a && b; }");
			check_true(earlier->execute("bool f() { return both(lo < hi, hi > (lo)); }\nf();").as_bool(), "an earlier execute's variables");
			auto warned = stdlib_engine();
			warned->execute("[[warn_mode]]\nbool both(bool a, bool b) { return a && b; }\nbool f() { return both(lo < hi, hi > (lo)); }\n");
			warned->execute("int lo = 1; int hi = 5;");
			check_true(warned->execute("f();").as_bool(), "[[warn_mode]] code whose names a later execute declares");
		});
		test("shapes_no_comparison_spells_and_a_lambda_s_parameters", [this]() {
			auto e = stdlib_engine();
			// a '(...)' that '=>' follows is a lambda: the '<' before it compares, on a name nothing declares a variable too
			const std::string lambda = failure(*e, "class S { var a; var b; var c; bool f() { return a < b > (x) => x; } }\nprint(1);");
			check_true(!contains(lambda, "parse failed") && !contains(lambda, "type arguments"), lambda);
			// '{' after the '>': on fields it is the comparison it always was, with that comparison's own error
			const std::string braces = failure(*e, "class S { int a = 1; int b = 2; bool f() { return a < b > {}; } }\nprint(1);");
			check_true(contains(braces, "cannot compare 'bool' with 'map' using '>'"), braces);
			// '>' before '()' is spelled by no comparison: type arguments, and the callee that takes none says so
			check_true(contains(failure(*e, "class Pet { }\nvoid f() { auto l = (int x) => x; print(l<Pet>()); }"), "'l' takes no type arguments: it is a lambda, and a lambda takes no type parameters"));
			check_true(contains(failure(*e, "void f() { auto l = (int x) => x; print(l<int>(2)); }"), "'l' takes no type arguments: it is a lambda, and a lambda takes no type parameters"));
			// a plain function and a plain class keep the reading that names them
			check_true(contains(failure(*e, "class Pet { }\nint plain(Pet p) { return 1; }\nprint(plain<Pet>(Pet()));"), "'plain' takes no type arguments: it is not a generic function"));
			check_true(contains(failure(*e, "class Pet { }\nclass Hero { }\nprint(Hero<Pet>());"), "'Hero' takes no type arguments: it is not a generic class"));
		});
		test("a_missing_generic_is_named_where_a_site_compares", [this]() {
			auto e = stdlib_engine();
			const std::string undeclared = failure(*e, "class Pet { }\nT make<T>(T x) { return x; }\nprint(mke<Pet>(Pet()));");
			check_true(contains(undeclared, "use of undeclared identifier 'mke' (no generic 'mke' is declared: '<' after a name that takes no type arguments compares)"), undeclared);
			const std::string method = failure(*e, "class Pet { }\nclass Tool { int plain(Pet p) { return 1; } }\nvoid f() { Tool t; print(t.plain<Pet>(Pet())); }");
			check_true(contains(method, "no generic 'plain' is declared: '<' after a name that takes no type arguments compares"), method);
		});
		test("a_member_head_asks_by_the_member_s_name", [this]() {
			auto e = stdlib_engine();
			// a field compares
			check_true(e->execute("class P { int x = 1; int y = 3; }\nbool both(bool a, bool b) { return a && b; }\nint w = 5; int h = 9;\nP p;\nboth(p.x < w, h > (p.y));").as_bool());
			// a field of the name of another class's generic method keeps the type-argument reading: parenthesize
			const std::string code = "class Other { T x<T>(T v) { return v; } }\nclass P { int x = 1; }\nbool both(bool a, bool b) { return a && b; }\nint w = 5; int h = 9;\nP p;\n";
			check_true(contains(failure(*e, code + "both(p.x < w, h > (w));"), "if this is a comparison, write (p.x < w)"));
			check_true(e->execute(code + "both((p.x < w), h > (w));").as_bool());
		});
		// a cached parse stands only while each head names what it named
		test("a_head_that_changes_kind_parses_its_unit_again", [this]() {
			auto e = stdlib_engine();
			const std::string user = "class Pet { int v = 7; }\nint two(int a, int b = 0) { return a + b; }\nint r = 0;\n[[warn_mode]]\nvoid use() { r = two(late<Pet>(Pet()).v); }\n";
			check_true(contains(failure(*e, user + "use();"), "late"), "nothing declares 'late': the site compares, and the run finds no 'late'");
			e->execute("T late<T>(T x) { return x; }");
			e->execute(user + "use();");
			check_eq((int64_t)7, e->execute("r;").as_int(), "the same text, parsed again once a generic of the name is declared");
			// a bite parsed where the head named a variable does not load where it names a function
			auto saved = stdlib_engine();
			saved->execute("int limit = 3; int cap = 5; bool both(bool a, bool b) { return a && b; }");
			auto bite = saved->jaibite("bool f() { return both(limit < cap, cap > (limit)); }\nf();\n");
			check_true(bite.execute().as_bool());
			auto same = stdlib_engine();
			same->execute("int limit = 3; int cap = 5; bool both(bool a, bool b) { return a && b; }");
			check_true(same->jaibite_load_bytes(bite.save_bytes()).execute().as_bool(), "the bite loads where its heads name what they named");
			auto other = stdlib_engine();
			other->execute("T limit<T>(T x) { return x; } int cap = 5; bool both(bool a, bool b) { return a && b; }");
			std::string refused = "(loaded)";
			try { other->jaibite_load_bytes(bite.save_bytes()); } catch (const std::exception& error) { refused = error.what(); }
			check_true(contains(refused, "the bite was parsed where 'limit' named a variable; here it names a function or a type, so '<' after it reads otherwise: save it again from source"), refused);
		});
		// A reload parses a loaded unit it does not run again from its source and checks it against the reload's declarations
		// (implementation::check_unit_text): that parse reads each site as the unit's settled parse does
		test("a_reload_reads_a_loaded_caller_as_its_settled_parse_does", [this]() {
			const auto directory = unit_directory("reload");
			const std::string lib = write_unit(directory, "lib.jai", "int lib_lo = 1; int lib_hi = 5;\nint scale() { return 1; }\nT pick<T>(T x) { return x; }\n");
			const std::string game = write_unit(directory, "game.jai",
				"import \"lib.jai\";\nclass Pet { int v = 100; }\nbool both(bool a, bool b) { return a && b; }\n"
				"int tick(int frame) { import(\"lib.jai\"); return (both(lib_lo < lib_hi, lib_hi > (lib_lo)) ? 10 : 0) + scale() + pick<Pet>(Pet()).v; }\n");
			auto e = stdlib_engine();
			e->add_include_path(directory.string());
			e->import_recheck_interval(0);
			e->execute_file(game);
			held_value tick = e->get_variable("tick");
			const auto frame = [&] {
				auto result = tick.as_function()({e->make_value(script_int(1))});
				return result ? result.value().to_string() : "ERR: " + e->format_error(result);
			};
			auto stamp = std::filesystem::last_write_time(lib);
			const auto edit = [&](const std::string& text) {
				write_unit(directory, "lib.jai", text);
				std::filesystem::last_write_time(lib, stamp += std::chrono::seconds(3));
			};
			check_eq(std::string("111"), frame());
			// the variables stay variables and the generic a generic: the caller's sites read as they did, and the reload applies
			edit("int lib_lo = 2; int lib_hi = 6;\nint scale() { return 2; }\nT pick<T>(T x) { return x; }\n");
			check_eq(std::string("112"), frame());
			// a variable the caller compares becomes a generic: '<' after it opens type arguments now, the caller no longer fits,
			// and the reload is refused with the error that says how the comparison is written
			edit("T lib_lo<T>(T x) { return x; }\nint lib_hi = 6;\nint scale() { return 3; }\nT pick<T>(T x) { return x; }\n");
			const std::string rejected = frame();
			check_true(rejected.rfind("ERR: ", 0) == 0 && contains(rejected, "game.jai:4:"), rejected);
			check_eq(std::string("112"), frame(), "the next frame runs the code it had");
		});
		// a measurement, never a gate: what a unit with sites pays on a cache hit (each site's head is asked again), beside the
		// same unit written with parentheses, which has no site and is never asked
		test("a_cache_hit_asks_each_site_again", [this]() {
			auto e = stdlib_engine();
			e->execution_budget(0.0);
			std::string fields = "bool both(bool a, bool b) { return a && b; }\nclass S { int lo = 1; int hi = 5;\n";
			std::string with_sites = fields, without = fields;
			for (int i = 0; i < 40; ++i) {
				with_sites += "\tbool f" + std::to_string(i) + "() { return both(lo < hi, hi > (lo)); }\n";
				without += "\tbool f" + std::to_string(i) + "() { return both((lo < hi), hi > (lo)); }\n";
			}
			with_sites += "}\nS().f0();";
			without += "}\nS().f0();";
			check_true(e->execute(with_sites).as_bool());
			check_true(e->execute(without).as_bool());
			const auto timed = [&e](const std::string& code) {
				return std::function<double()>([&e, &code] { return time_seconds([&e, &code] { for (int i = 0; i < 200; ++i) { e->execute(code); } }); });
			};
			compare_speed("200 cache hits of a 40-method unit with a '<' site in each method against the unit with none", timed(without), timed(with_sites), 9);
		});
	}
};

}
using less_than_tests = jai::foundry::tests::less_than_tests;
FOUNDRY_REGISTER(less_than_tests)
