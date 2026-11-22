// What a reload decides (docs/site/embedding/07-hot-reload.html "The contract", "Callers in other files"): a rejected reload
// leaves nothing and waits for a file it involved to change, and a generic's instance gets the verdict the plain function or
// class in its place gets (docs/site/guide/21-generics.html "Declaring it again"), in every arrangement of the files
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jai::foundry::tests {

class reload_verdict_tests : public suite {
public:
	reload_verdict_tests() : suite("Reload Verdicts") {}

	// one program in a folder of its own: files written with a stamp that always moves, an engine that checks every reach
	struct program {
		std::filesystem::path dir;
		std::shared_ptr<engine> eng;
		int tick = 0;

		explicit program(const std::string& name) : dir(test_temp_directory() / ("rv_" + name)) {
			std::error_code ec;
			std::filesystem::remove_all(dir, ec);
			std::filesystem::create_directories(dir, ec);
			eng = make_engine();
			stdlib::register_all(*eng);
			eng->add_include_path(dir.string());
			eng->import_recheck_interval(0);
			eng->jaibite_cache(false);
		}
		~program() {
			eng.reset();
			std::error_code ec;
			std::filesystem::remove_all(dir, ec);
		}
		std::string path(const std::string& file) const { return (dir / file).generic_string(); }
		void write(const std::string& file, const std::string& text) {
			const auto p = dir / file;
			{ std::ofstream(p, std::ios::binary | std::ios::trunc) << text; }
			std::error_code ec;
			std::filesystem::last_write_time(p, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick + 4000), ec);
		}
		std::string run(const std::string& code) {
			try {
				return eng->execute(code).to_string();
			} catch (const std::exception& e) {
				return std::string("ERR: ") + e.what();
			}
		}
		std::string run_file(const std::string& file) {
			try {
				return eng->execute_file(path(file)).to_string();
			} catch (const std::exception& e) {
				return std::string("ERR: ") + e.what();
			}
		}
	};

	static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
	static bool failed(const std::string& text) { return text.rfind("ERR: ", 0) == 0; }

	// one generic or plain declaration of the name, in the forms the same program is written in
	struct family {
		std::string name;
		std::string declare;   // %s: the letter its body answers
		std::string call;
	};
	static std::string declared(const family& f, const std::string& letter) {
		std::string text = f.declare;
		for (size_t at = text.find("%s"); at != std::string::npos; at = text.find("%s")) { text.replace(at, 2, letter); }
		return text;
	}
	static std::vector<family> families() {
		return {
			{"a plain function", "string f(int x) { return \"%s\"; }\n", "f(1)"},
			{"a generic function", "string f<T>(T x) { return \"%s\"; }\n", "f(1)"},
			{"a generic function called with its type argument", "string f<T>(T x) { return \"%s\"; }\n", "f<int>(1)"},
			{"a generic class", "class F<T> { string f(T x) { return \"%s\"; } }\n", "F<int>().f(1)"},
			{"a class in a namespace", "namespace n { class F { string f(int x) { return \"%s\"; } } }\n", "n::F().f(1)"},
			{"a generic class in a namespace", "namespace n { class F<T> { string f(T x) { return \"%s\"; } } }\n", "n::F<int>().f(1)"},
			{"a class with a plain method", "class K { string f(int x) { return \"%s\"; } }\n", "K().f(1)"},
			{"a class with a generic method", "class K { string f<T>(T x) { return \"%s\"; } }\n", "K().f(1)"},
		};
	}

	void forge_tests() override {
		test("a_rejected_reload_leaves_none_of_its_declarations", [this]() {
			// the reload is reached from a file's function, or from an executed string whose own text names the import
			for (const bool through_string : {false, true}) {
				const std::string how = through_string ? " (reached from an executed string)" : " (reached from a file's function)";
				program p(through_string ? "a_string" : "a_file");
				p.write("lib.jai", "int size_of(int x) { return x; }\n");
				p.write("root.jai", "import \"lib.jai\";\nint use() { return size_of(1); }\nint tick() { import(\"lib.jai\"); return use(); }\n");
				check_eq(std::string("null"), p.run_file("root.jai"));
				p.write("lib.jai", "int size_of(string x) { return 9; }\nint fresh() { return 7; }\nclass Fresh { int n = 4; }\nint FRESH = 5;\n");
				const std::string rejected = p.run(through_string ? "import(\"lib.jai\");" : "tick();");
				check_true(has(rejected, "root.jai:2:29: error: argument 1 of 'size_of': cannot pass 'int' to 'x' declared 'string'"), "the caller rejects the reload" + how + ": " + rejected);
				check_true(has(p.run("fresh();"), "<script>:1:1: error: use of undeclared identifier 'fresh'"), "a function of the rejected text is not declared" + how + ": " + p.run("fresh();"));
				check_true(has(p.run("Fresh().n;"), "<script>:1:1: error: use of undeclared identifier 'Fresh'"), "nor a class" + how);
				check_true(has(p.run("FRESH;"), "<script>:1:1: error: use of undeclared identifier 'FRESH'"), "nor a variable" + how);
				check_true(has(p.run("int q() { return fresh(); }\nq();"), "error: use of undeclared identifier 'fresh'"), "nor inside a function a later text declares" + how);
				check_eq(std::string("1"), p.run("size_of(1);"), "the function keeps the signature it runs with" + how);
				check_true(has(p.run("size_of(\"a\");"), "<script>:1:12: error: argument 1 of 'size_of': cannot pass 'string' to 'x' declared 'int'"), "and a call the rejected text would take is refused" + how + ": " + p.run("size_of(\"a\");"));
				check_eq(std::string("1"), p.run("use();"), "the old code runs" + how);
				// the fix applies, and what it declares is declared
				p.write("lib.jai", "int size_of(int x) { return x + 1; }\nint fresh() { return 7; }\n");
				check_eq(std::string("2"), p.run(through_string ? "import(\"lib.jai\"); use();" : "tick();"), "the fix reloads" + how);
				check_eq(std::string("7"), p.run("fresh();"), "and brings its declarations" + how);
			}
			// a root file run again whose closure holds a rejected edit: the same nothing
			{
				program p("a_root");
				p.write("lib.jai", "int size_of(int x) { return x; }\n");
				p.write("user.jai", "int use() { return size_of(1); }\n");
				p.write("root.jai", "import \"lib.jai\";\nimport \"user.jai\";\n");
				check_eq(std::string("null"), p.run_file("root.jai"));
				p.write("lib.jai", "int size_of(string x) { return 9; }\nint fresh() { return 7; }\n");
				const std::string rejected = p.run_file("root.jai");
				check_true(has(rejected, "user.jai:1:29: error: argument 1 of 'size_of'"), "the root's own run again is rejected for the caller: " + rejected);
				check_true(has(p.run("fresh();"), "error: use of undeclared identifier 'fresh'"), "and leaves no declaration of the rejected text: " + p.run("fresh();"));
				check_eq(std::string("1"), p.run("size_of(1);"));
				check_eq(std::string("1"), p.run("use();"));
			}
			// the string's own declarations ran before its import was reached: they stay, as any code that ran does
			{
				program p("a_beside");
				p.write("lib.jai", "int size_of(int x) { return x; }\n");
				p.write("root.jai", "import \"lib.jai\";\nint use() { return size_of(1); }\n");
				check_eq(std::string("null"), p.run_file("root.jai"));
				p.write("lib.jai", "int size_of(string x) { return 9; }\nint fresh() { return 7; }\n");
				check_true(failed(p.run("int beside() { return 3; }\nimport \"lib.jai\";\nbeside();")), "the import's rejection is the string's error");
				check_eq(std::string("3"), p.run("beside();"), "what the string declared before the import stays declared");
				check_true(has(p.run("fresh();"), "error: use of undeclared identifier 'fresh'"), "the module's rejected text does not");
			}
		});

		test("a_module_rejected_for_a_file_in_its_closure_raises_once_and_waits", [this]() {
			struct closure {
				std::string name;
				std::string part;          // the file in the module's closure; empty: the module's own text is what breaks
				std::string good, broken, fixed;
				std::string outer;         // the module the import reaches
				std::string error;         // what the rejection names
			};
			const std::vector<closure> closures = {
				{"an imported module", "part.jai", "int part() { return 1; }\n", "int part() { return \"broken\"; }\n", "int part() { return 2; }\n",
				 "import \"part.jai\";\nint outer() { return part() + 10; }\n", "part.jai:1:14: error: cannot return 'string' from 'part'"},
				{"a file included at the top level", "part.jai", "int part() { return 1; }\n", "int part() { return \"broken\"; }\n", "int part() { return 2; }\n",
				 "include \"part.jai\";\nint outer() { return part() + 10; }\n", "part.jai:1:14: error: cannot return 'string' from 'part'"},
				{"a file spliced into a function", "part.jai", "int n = 1;\n", "int n = \"broken\";\n", "int n = 2;\n",
				 "int outer() { include \"part.jai\"; return n + 10; }\n", "error: cannot initialize 'n' declared 'int' with 'string'"},
				{"a generic's file whose edit breaks an instance", "part.jai", "class Box<T> { T item; int size() { return 1; } }\n",
				 "class Box<T> { T item; int size() { return item.size(); } }\n", "class Box<T> { T item; int size() { return 2; } }\n",
				 "import \"part.jai\";\nint outer() { Box<int> b; return b.size() + 10; }\n", "part.jai:1:49: error: int has no member 'size'"},
				{"a generic's file that no longer parses", "part.jai", "class Box<T> { T item; int size() { return 1; } }\n",
				 "class Box<T> { T item; int size() { return 5 +; } }\n", "class Box<T> { T item; int size() { return 2; } }\n",
				 "import \"part.jai\";\nint outer() { Box<int> b; return b.size() + 10; }\n", "part.jai:1:47: error: expected expression"},
				{"the module's own text", "", "int outer() { return 11; }\n", "int outer() { return \"broken\"; }\n", "int outer() { return 12; }\n",
				 "", "outer.jai:1:15: error: cannot return 'string' from 'outer'"},
			};
			for (const closure& c : closures) {
				program p("d_" + std::to_string(&c - closures.data()));
				const std::string edited = c.part.empty() ? "outer.jai" : c.part;
				p.write("outer.jai", c.outer);
				p.write(edited, c.good);
				p.write("root.jai", "import \"outer.jai\";\nint tick() { import(\"outer.jai\"); return outer(); }\n");
				check_eq(std::string("null"), p.run_file("root.jai"), c.name);
				check_eq(std::string("11"), p.run("tick();"), c.name + ": before the edit");
				p.write(edited, c.broken);
				const std::string first = p.run("tick();");
				check_true(failed(first) && has(first, c.error), c.name + ": the first reach raises the rejection: " + first);
				for (int reach = 1; reach <= 3; ++reach) {
					check_eq(std::string("11"), p.run("tick();"), c.name + ": reach " + std::to_string(reach) + " after it runs the code the program had");
				}
				check_eq(std::string("false"), p.run("string path = \"outer\" + \".jai\"; to_string(import(path));"), c.name + ": an import of it answers false while it waits");
				p.write(edited, c.fixed);
				check_eq(std::string("12"), p.run("tick();"), c.name + ": the fix reloads");
				// another broken edit is another rejection, raised once
				p.write(edited, c.broken);
				check_true(failed(p.run("tick();")), c.name + ": a second broken edit raises again");
				check_eq(std::string("12"), p.run("tick();"), c.name + ": and waits again");
			}
		});

		test("a_generic_a_file_drops_stays_as_a_function_or_a_class_it_drops_does", [this]() {
			struct dropped {
				std::string name;
				std::string before;
				std::string use;
				std::string fresh;   // a use of it no unit made before the drop (another instance, for a generic)
			};
			const std::vector<dropped> kinds = {
				{"a plain function", "int twice(int x) { return x + x; }\n", "twice(4)", "twice(5)"},
				{"a generic function", "T twice<T>(T x) { return x + x; }\n", "twice<int>(4)", "twice<float>(2.5)"},
				{"a generic function called by deduction", "T twice<T>(T x) { return x + x; }\n", "twice(4)", "twice(\"ab\").size()"},
				{"a plain class", "class Box { int item = 3; int size() { return 8; } }\n", "Box().size()", "Box().item"},
				{"a generic class", "class Box<T> { T item; int size() { return 8; } }\n", "Box<int>().size()", "Box<string>().size()"},
				{"a generic function in a namespace", "namespace n { T twice<T>(T x) { return x + x; } }\n", "n::twice<int>(4)", "n::twice<float>(2.5)"},
			};
			for (const bool module_user : {false, true}) {
				for (const dropped& k : kinds) {
					const std::string who = k.name + (module_user ? ", used by a module" : ", used by the root file");
					program p("e_" + std::to_string(&k - kinds.data()) + (module_user ? "m" : "r"));
					p.write("lib.jai", k.before);
					if (module_user) {
						p.write("user.jai", "import \"lib.jai\";\nvar use() { return " + k.use + "; }\n");
						p.write("root.jai", "import \"user.jai\";\nvar tick() { import(\"lib.jai\", \"user.jai\"); return use(); }\n");
					} else {
						p.write("root.jai", "import \"lib.jai\";\nvar use() { return " + k.use + "; }\nvar tick() { import(\"lib.jai\"); return use(); }\n");
					}
					check_eq(std::string("null"), p.run_file("root.jai"), who);
					const std::string value = p.run("tick();");
					check_true(!failed(value), who + ": before the drop: " + value);
					p.write("lib.jai", "int other() { return 1; }\n");
					check_eq(value, p.run("tick();"), who + ": the reload that drops it is accepted and its users run as they did");
					check_eq(value, p.run("tick();"), who + ": at every reach");
					check_eq(std::string("1"), p.run("other();"), who + ": the file's new text is applied");
					check_eq(value, p.run("var later() { return " + k.use + "; }\nlater();"), who + ": a later text uses it");
					const std::string newer = p.run("var newer() { return " + k.fresh + "; }\nnewer();");
					check_true(!failed(newer), who + ": and makes of it what no unit made before: " + newer);
					if (module_user) {
						p.write("user.jai", "import \"lib.jai\";\nvar use() { return " + k.use + "; }\nint edited() { return 1; }\n");
						check_eq(value, p.run("tick();"), who + ": its user's own edit reloads");
						check_eq(std::string("1"), p.run("edited();"), who);
					}
					// the name declared again is the declaration from there on
					p.write("lib.jai", k.before + "int other() { return 2; }\n");
					check_eq(value, p.run("tick();"), who + ": declared again");
					check_eq(std::string("2"), p.run("other();"), who);
				}
			}
			// a name the file gives to something else is declared again, not dropped: what cannot be made from that rejects the reload
			struct renamed {
				std::string name;
				std::string before, after;
				std::string use;
			};
			const std::vector<renamed> renames = {
				{"a generic function declared with other parameters", "T twice<T>(T x) { return x + x; }\n", "T twice<T>(T x, T y) { return x + y; }\n", "twice<int>(4)"},
				{"a generic function's name given to a plain function of another type", "T twice<T>(T x) { return x + x; }\n", "string twice(string x) { return x; }\n", "twice<int>(4)"},
				{"a generic class's name given to a plain class", "class Box<T> { T item; int size() { return 8; } }\n", "class Box { int size() { return 9; } }\n", "Box<int>().size()"},
				{"a generic class declared with another count of parameters", "class Box<T> { T item; int size() { return 8; } }\n", "class Box<K, V> { K item; int size() { return 9; } }\n", "Box<int>().size()"},
			};
			for (const renamed& r : renames) {
				program p("e_renamed_" + std::to_string(&r - renames.data()));
				p.write("lib.jai", r.before);
				p.write("root.jai", "import \"lib.jai\";\nvar use() { return " + r.use + "; }\nvar tick() { import(\"lib.jai\"); return use(); }\n");
				check_eq(std::string("null"), p.run_file("root.jai"), r.name);
				const std::string value = p.run("tick();");
				p.write("lib.jai", r.after);
				const std::string rejected = p.run("tick();");
				check_true(failed(rejected) && has(rejected, "root.jai:2:"), r.name + ": the reload is rejected at the caller: " + rejected);
				check_eq(value, p.run("tick();"), r.name + ": and the old code runs");
			}
		});

		test("the_same_declaration_in_two_files_gets_one_verdict_whatever_it_declares", [this]() {
			// every arrangement of two files that declare the name (a answers "a", b answers "b"), a third that only calls it and a
			// root that may declare its own: the later declaration to run is the one every caller reaches
			struct arrangement {
				std::string name;
				std::string root;     // ${own}: the root's own declaration; ${call}: the call
				std::string expected;
			};
			const std::vector<arrangement> arrangements = {
				{"a then b", "import \"fa.jai\";\nimport \"fb.jai\";\n${call} + use_a() + use_b();", "bbb"},
				{"b then a", "import \"fb.jai\";\nimport \"fa.jai\";\n${call} + use_a() + use_b();", "aaa"},
				{"a, b, then a file that only calls", "import \"fa.jai\";\nimport \"fb.jai\";\nimport \"fc.jai\";\n${call} + use_a() + use_b() + use_c();", "bbbb"},
				{"the calling file first", "import \"fc.jai\";\nimport \"fa.jai\";\nimport \"fb.jai\";\n${call} + use_a() + use_b() + use_c();", "bbbb"},
				{"b, a, then the calling file", "import \"fb.jai\";\nimport \"fa.jai\";\nimport \"fc.jai\";\n${call} + use_a() + use_b() + use_c();", "aaaa"},
				{"a call between the imports", "string out = \"\";\nimport \"fa.jai\";\nout += ${call} + use_a();\nimport \"fb.jai\";\nout += \"|\" + ${call} + use_a() + use_b();\nout;", "aa|bbb"},
				{"the root's own declaration, then b", "${own}string out = ${call};\nimport \"fb.jai\";\nout += \"|\" + ${call} + use_b();\nout;", "r|bb"},
			};
			const auto fill = [](std::string text, const std::string& own, const std::string& call) {
				for (size_t at = text.find("${own}"); at != std::string::npos; at = text.find("${own}")) { text.replace(at, 6, own); }
				for (size_t at = text.find("${call}"); at != std::string::npos; at = text.find("${call}")) { text.replace(at, 7, call); }
				return text;
			};
			const std::vector<family> all = families();
			for (const family& f : all) {
				for (const arrangement& a : arrangements) {
					program p("g1_" + std::to_string(&f - all.data()) + "_" + std::to_string(&a - arrangements.data()));
					p.write("fa.jai", declared(f, "a") + "string use_a() { return " + f.call + "; }\n");
					p.write("fb.jai", declared(f, "b") + "string use_b() { return " + f.call + "; }\n");
					p.write("fc.jai", "string use_c() { return " + f.call + "; }\n");
					p.write("root.jai", fill(a.root, declared(f, "r"), f.call));
					check_eq(a.expected, p.run_file("root.jai"), f.name + ", " + a.name);
				}
			}
			// a reload that declares it again: the file that ran last is the one every caller reaches, at once
			for (const family& f : all) {
				program p("g1_reload_" + std::to_string(&f - all.data()));
				const auto file = [&](const char* letter, const char* user) { return declared(f, letter) + "string " + user + "() { return " + f.call + "; }\n"; };
				p.write("fa.jai", file("a", "use_a"));
				p.write("fb.jai", file("b", "use_b"));
				p.write("fc.jai", "string use_c() { return " + f.call + "; }\n");
				p.write("root.jai", "import \"fa.jai\";\nimport \"fb.jai\";\nimport \"fc.jai\";\nstring all() { import(\"fa.jai\", \"fb.jai\", \"fc.jai\"); return " + f.call + " + use_a() + use_b() + use_c(); }\n");
				std::string trace = p.run_file("root.jai");
				const auto step = [&](const std::string& code) { trace += " "; trace += p.run(code); };
				step("all();");
				p.write("fa.jai", file("A", "use_a"));
				step("all();");
				p.write("fb.jai", file("B", "use_b"));
				step("all();");
				p.write("fc.jai", "string use_c() { return " + f.call + " + \"!\"; }\n");
				step("all();");
				p.write("fa.jai", file("Z", "use_a"));
				step("all();");
				step(f.call + ";");
				check_eq(std::string("null bbbb AAAA BBBB BBBB! ZZZZ! Z"), trace, f.name + ": across reloads");
			}
		});

		test("generic_functions_that_differ_in_shape_stay_overloads_across_files", [this]() {
			program p("g1_shapes");
			p.write("fa.jai", "string f<T>(T x) { return \"one\"; }\nstring g<T>(array<T> xs) { return \"array\"; }\n");
			p.write("fb.jai", "string f<T>(T x, T y) { return \"two\"; }\nstring g<K, V>(map<K, V> m) { return \"map\"; }\n");
			p.write("root.jai", "import \"fa.jai\";\nimport \"fb.jai\";\nf(1) + \" \" + f(1, 2) + \" \" + g([1]) + \" \" + g({\"k\": 1});");
			check_eq(std::string("one two array map"), p.run_file("root.jai"));
			// an instance only a calling file names follows the declaration that ran last, as every other does
			program q("g1_third");
			q.write("fa.jai", "string f<T>(T x) { return \"a\"; }\n");
			q.write("fb.jai", "string f<T>(T x) { return \"b\"; }\n");
			q.write("fc.jai", "string use_c() { return f(\"text\") + f(2.5); }\n");
			q.write("root.jai", "import \"fa.jai\";\nimport \"fb.jai\";\nimport \"fc.jai\";\nstring all() { import(\"fa.jai\", \"fb.jai\", \"fc.jai\"); return use_c(); }\n");
			check_eq(std::string("null"), q.run_file("root.jai"));
			check_eq(std::string("bb"), q.run("all();"));
			q.write("fa.jai", "string f<T>(T x) { return \"A\"; }\n");
			check_eq(std::string("AA"), q.run("all();"), "the file declared again makes every instance the engine holds again");
		});

		test("a_namespaces_generic_function_collides_across_files_as_its_plain_function_does", [this]() {
			// a namespace's function another unit declares with the same parameters raises where the second declaration runs,
			// unless that declaration is written `override`: the generic gets the plain function's verdict, text included
			struct form {
				std::string name;
				std::string declare;   // %s: the letter its body answers; %o: where `override` stands
				std::string call;
			};
			const std::vector<form> forms = {
				{"a plain function", "namespace n { string f(int x)%o { return \"%s\"; } }\n", "n::f(1)"},
				{"a generic function", "namespace n { string f<T>(T x)%o { return \"%s\"; } }\n", "n::f(1)"},
				{"a generic function called with its type argument", "namespace n { string f<T>(T x)%o { return \"%s\"; } }\n", "n::f<int>(1)"},
			};
			const auto text = [](const form& f, const std::string& letter, bool overrides) {
				std::string out = f.declare;
				out.replace(out.find("%o"), 2, overrides ? " override" : "");
				out.replace(out.find("%s"), 2, letter);
				return out;
			};
			const std::string collision = "Function 'f' already exists in namespace 'n'. Use 'override' keyword to replace it.";
			for (const form& f : forms) {
				const auto files = [&](program& p, bool override_a, bool override_b) {
					p.write("fa.jai", text(f, "a", override_a) + "string use_a() { return " + f.call + "; }\n");
					p.write("fb.jai", text(f, "b", override_b) + "string use_b() { return " + f.call + "; }\n");
					p.write("fc.jai", "string use_c() { return " + f.call + "; }\n");
				};
				const std::string second = "string out = \"\";\nimport \"fa.jai\";\nout += " + f.call + " + use_a();\n"
				                           "try { import(\"fb.jai\"); out += \"|loaded\"; } catch (e) { out += \"|\" + e; }\nout += \"|\" + " + f.call + " + use_a();\nout;";
				{
					program p("n1_plain_" + std::to_string(&f - forms.data()));
					files(p, false, false);
					p.write("root.jai", second);
					check_eq("aa|" + collision + "|aa", p.run_file("root.jai"), f.name + ": the second file's declaration collides and the first stands");
					check_eq(std::string("a"), p.run(f.call + ";"), f.name + ": later text reaches the declaration that stands");
				}
				{
					program p("n1_first_" + std::to_string(&f - forms.data()));
					files(p, true, false);
					p.write("root.jai", second);
					check_eq("aa|" + collision + "|aa", p.run_file("root.jai"), f.name + ": override on the first only does not let the second in");
				}
				{
					program p("n1_second_" + std::to_string(&f - forms.data()));
					files(p, false, true);
					p.write("root.jai", "import \"fa.jai\";\nimport \"fb.jai\";\nimport \"fc.jai\";\n"
					                    "string all() { import(\"fa.jai\", \"fb.jai\", \"fc.jai\"); return " + f.call + " + use_a() + use_b() + use_c(); }\n");
					check_eq(std::string("null"), p.run_file("root.jai"), f.name);
					check_eq(std::string("bbbb"), p.run("all();"), f.name + ": override on the second replaces, for every caller");
					// a reload of either file: the overriding file replaces again, the other collides with what stands and waits
					p.write("fb.jai", text(f, "B", true) + "string use_b() { return " + f.call + "; }\n");
					check_eq(std::string("BBBB"), p.run("all();"), f.name + ": the overriding file declared again");
					p.write("fa.jai", text(f, "A", false) + "string use_a() { return " + f.call + "; }\n");
					check_eq("ERR: " + collision, p.run("all();").substr(0, 5 + collision.size()), f.name + ": the first file declared again collides with the declaration that stands");
					check_eq(std::string("BBBB"), p.run("all();"), f.name + ": and the program runs on");
				}
				{
					program p("n1_same_" + std::to_string(&f - forms.data()));
					files(p, false, false);
					p.write("root.jai", "import \"fc.jai\";\nimport \"fa.jai\";\nstring all() { import(\"fa.jai\", \"fc.jai\"); return " + f.call + " + use_a() + use_c(); }\n");
					check_eq(std::string("null"), p.run_file("root.jai"), f.name + ": a file that only calls it, imported first");
					check_eq(std::string("aaa"), p.run("all();"), f.name);
					p.write("fa.jai", text(f, "A", false) + "string use_a() { return " + f.call + "; }\n");
					check_eq(std::string("AAA"), p.run("all();"), f.name + ": the same file run again replaces its own");
				}
			}
			// a class in a namespace is replaced by the later file, a generic method with it
			for (const char* method : {"string f(int x)", "string f<T>(T x)"}) {
				program p(std::string("n1_method_") + (method[8] == '<' ? "g" : "p"));
				p.write("fa.jai", std::string("namespace n { class K { ") + method + " { return \"a\"; } } }\nstring use_a() { return n::K().f(1); }\n");
				p.write("fb.jai", std::string("namespace n { class K { ") + method + " { return \"b\"; } } }\nstring use_b() { return n::K().f(1); }\n");
				p.write("root.jai", "import \"fa.jai\";\nimport \"fb.jai\";\nn::K().f(1) + use_a() + use_b();");
				check_eq(std::string("bbb"), p.run_file("root.jai"), method);
			}
		});

		test("a_file_the_host_runs_again_is_checked_as_a_reload", [this]() {
			for (const bool hand_text : {false, true}) {
				const std::string how = hand_text ? " (execute_file_source)" : " (execute_file)";
				program p(hand_text ? "c_source" : "c_file");
				const auto run_root = [&](const std::string& text) {
					p.write("root.jai", text);
					if (!hand_text) { return p.run_file("root.jai"); }
					try {
						return p.eng->execute_file_source(p.path("root.jai"), text).to_string();
					} catch (const std::exception& e) {
						return std::string("ERR: ") + e.what();
					}
				};
				p.write("user.jai", "int use() { return f(1) + 1; }\nint size() { C c; return c.a; }\n");
				const std::string v1 = "int f(int x) { return x; }\nclass C { int a = 3; }\nimport \"user.jai\";\n";
				check_eq(std::string("null"), run_root(v1), "the first run" + how);
				check_eq(std::string("2|3"), p.run("to_string(use()) + \"|\" + to_string(size());"));
				// the same text again is no reload
				check_eq(std::string("null"), run_root(v1), "the same text again" + how);
				// a function another file calls, declared with other parameters
				const std::string rejected = run_root("int f(string x) { return 9; }\nclass C { int a = 3; }\nint fresh() { return 1; }\nimport \"user.jai\";\n");
				check_true(has(rejected, "static check failed:") && has(rejected, "user.jai:1:23: error: argument 1 of 'f': cannot pass 'int' to 'x' declared 'string'"),
				           "the caller in another file rejects it" + how + ": " + rejected);
				check_eq(std::string("2|3"), p.run("to_string(use()) + \"|\" + to_string(size());"), "nothing applied" + how);
				// and nothing of it stays in what later text is checked against
				check_eq(std::string("1"), p.run("f(1);"), "a later text is checked against the function that runs" + how);
				check_true(has(p.run("fresh();"), "use of undeclared identifier 'fresh'"), "what the rejected text declared is not declared" + how + ": " + p.run("fresh();"));
				// rejected for a mistake of its own, the same
				const std::string own_mistake = run_root("int f(int x) { return x; }\nclass C { int a = 3; }\nint fresh2() { return nope; }\nimport \"user.jai\";\n");
				check_true(has(own_mistake, "use of undeclared identifier 'nope'"), "its own mistake" + how + ": " + own_mistake);
				check_true(has(p.run("fresh2();"), "use of undeclared identifier 'fresh2'"), "nothing of that text either" + how + ": " + p.run("fresh2();"));
				check_eq(std::string("2|3"), p.run("to_string(use()) + \"|\" + to_string(size());"), "nothing applied" + how);
				// a class another file reads a field of, declared without it
				const std::string reshaped = run_root("int f(int x) { return x; }\nclass C { int b = 3; }\nimport \"user.jai\";\n");
				check_true(has(reshaped, "user.jai:2:") && has(reshaped, "error: no member 'a' in class 'C'"), "the field's reader rejects it" + how + ": " + reshaped);
				check_eq(std::string("2|3"), p.run("to_string(use()) + \"|\" + to_string(size());"), "nothing applied" + how);
				// an edit every loaded unit still fits applies
				check_eq(std::string("null"), run_root("int f(int x) { return x + 5; }\nclass C { int a = 3; int b = 4; }\nimport \"user.jai\";\n"), "a fitting edit" + how);
				check_eq(std::string("7|3"), p.run("to_string(use()) + \"|\" + to_string(size());"), "and runs" + how);
				// text the host executes as a string checks no unit but its own: a function of other parameters is an overload beside the first
				check_eq(std::string("9"), p.run("int f(string x) { return 9; }\nf(\"s\");"), "an executed string" + how);
				check_eq(std::string("7"), p.run("use();"), how);
				// two files the host runs itself, neither importing the other: the one run again is checked against the other
				const auto run_lib = [&](const std::string& text) {
					p.write("lib.jai", text);
					if (!hand_text) { return p.run_file("lib.jai"); }
					try {
						return p.eng->execute_file_source(p.path("lib.jai"), text).to_string();
					} catch (const std::exception& e) {
						return std::string("ERR: ") + e.what();
					}
				};
				p.write("caller.jai", "int call() { D d; return g(1) + d.a; }\n");
				check_eq(std::string("null"), run_lib("int g(int x) { return x; }\nclass D { int a = 3; }\n"), how);
				check_eq(std::string("null"), p.run_file("caller.jai"), how);
				check_eq(std::string("4"), p.run("call();"), how);
				const std::string lone = run_lib("int g(string x) { return 9; }\nclass D { int a = 3; }\n");
				check_true(has(lone, "caller.jai:1:") && has(lone, "error: argument 1 of 'g': cannot pass 'int' to 'x' declared 'string'"), "a file run beside it rejects it" + how + ": " + lone);
				check_eq(std::string("4"), p.run("call();"), "nothing applied" + how);
				const std::string lone_class = run_lib("int g(int x) { return x; }\nclass D { int b = 3; }\n");
				check_true(has(lone_class, "caller.jai:1:") && has(lone_class, "error: no member 'a' in class 'D'"), "and for its class" + how + ": " + lone_class);
				check_eq(std::string("4"), p.run("call();"), "nothing applied" + how);
				check_eq(std::string("null"), run_lib("int g(int x) { return x + 10; }\nclass D { int a = 3; }\n"), "a fitting edit" + how);
				check_eq(std::string("14"), p.run("call();"), how);
			}
		});

		test("text_the_host_executes_warns_of_each_use_in_another_unit_it_no_longer_fits", [this]() {
			// A string checks no unit but its own. Where it declares again a class or a function the engine holds, what a reload of
			// that text would be rejected for is a warning of its check, at each use's file and line: nothing is refused
			program p("c_text");
			const auto warnings = [&]() {   // each place by its file's name
				std::vector<std::string> texts;
				for (const auto& d : p.eng->last_check_diagnostics().diagnostics) {
					if (d.severity != check_diagnostic::level::warning) { continue; }
					const size_t open = d.message.rfind('('), cut = d.message.find_last_of("/\\");
					texts.push_back(open != std::string::npos && cut != std::string::npos && cut > open ? d.message.substr(0, open + 1) + d.message.substr(cut + 1) : d.message);
				}
				return texts;
			};
			const auto listed = [](const std::vector<std::string>& texts) {
				std::string all;
				for (const auto& t : texts) { all += t + "\n"; }
				return all;
			};
			const std::string lead = "a use in another unit no longer fits what this text declares: ";
			p.write("user.jai", "int size() { C c; return c.a; }\n");
			p.write("other.jai", "int twice() { C c; return c.a + c.a; }\nint count() { int n = tally(2); return n; }\n");
			check_eq(std::string("null"), p.run("class C { int a = 3; }\nint tally(int n) { return n; }"));
			check_eq(std::string("null"), p.run_file("user.jai"));
			check_eq(std::string("null"), p.run_file("other.jai"));
			check_eq(std::string("3|6|2"), p.run("to_string(size()) + \"|\" + to_string(twice()) + \"|\" + to_string(count());"));
			check_true(warnings().empty(), listed(warnings()));
			// a class two files read a field of, declared without it: the string runs, and each reader is named
			check_eq(std::string("null"), p.run("class C { int b = 3; }"));
			check_eq(lead + "no member 'a' in class 'C' (user.jai:1:28)\n" + lead + "no member 'a' in class 'C' (other.jai:1:29)\n" + lead + "no member 'a' in class 'C' (other.jai:1:35)\n",
			         listed(warnings()));
			check_true(failed(p.run("size();")), "the use fails where it runs");
			// the same text again declares nothing new
			check_eq(std::string("null"), p.run("class C { int b = 3; }"));
			check_true(warnings().empty(), listed(warnings()));
			// a declaration every use fits
			check_eq(std::string("null"), p.run("class C { int a = 4; int b = 3; }"));
			check_true(warnings().empty(), listed(warnings()));
			check_eq(std::string("4|8"), p.run("to_string(size()) + \"|\" + to_string(twice());"));
			// a function declared again with its parameters and another result
			check_eq(std::string("null"), p.run("string tally(int n) { return \"s\"; }"));
			check_eq(lead + "cannot initialize 'n' declared 'int' with 'string' (other.jai:2:19)\n", listed(warnings()));
			check_true(failed(p.run("count();")), "the use fails where it runs: " + p.run("count();"));
			// other parameters are an overload beside the first: every caller still fits
			check_eq(std::string("null"), p.run("int tally(int n) { return n + 1; }"));
			check_true(warnings().empty(), listed(warnings()));
			check_eq(std::string("null"), p.run("int tally(string s) { return 9; }"));
			check_true(warnings().empty(), listed(warnings()));
			check_eq(std::string("3"), p.run("count();"));
			// a string that declares nothing the engine holds
			check_eq(std::string("null"), p.run("class Fresh { int a = 1; }\nint fresh() { return 1; }"));
			check_true(warnings().empty(), listed(warnings()));
		});

		test("a_bite_never_replaces_what_live_code_holds_with_the_text_it_was_saved_from", [this]() {
			// A bite carries the instances its text names as declarations. Saved where the generic had one text and run where it
			// has another, it makes them again from the declaration the engine holds: the engine's own objects and callers keep
			// the code they run, and the bite's code is checked against what it will meet.
			const std::string lib_v1 = "class Cell<T> { T v; int tag() { return 1; } }\nT same<T>(T x) { return x; }\nnamespace box { class Lid<T> { T v; int tag() { return 10; } } T pick<T>(T x) { return x; } }\n";
			const std::string user = "Cell<float> f; f.v = 1.5; Cell<Cell<int>> nested; nested.v.v = same(4); box::Lid<int> lid; lid.v = box::pick(3);\n"
			                         "to_string(f.tag()) + \"|\" + to_string(nested.v.v) + \"|\" + to_string(nested.tag()) + \"|\" + to_string(lid.tag() + lid.v) + \"|\" + reflect::type_of(nested);";
			const auto engine_with = [](const std::string& text) {
				auto e = make_engine();
				stdlib::register_all(*e);
				if (!text.empty()) { e->execute(text); }
				return e;
			};
			const auto outcome = [](engine& e, const std::string& code) {
				try {
					return e.execute(code).to_string();
				} catch (const std::exception& error) {
					return std::string("ERR: ") + error.what();
				}
			};
			auto maker = engine_with(lib_v1);
			jaibite made = maker->jaibite(user);
			check_eq(std::string("1|4|1|13|Cell<Cell<int>>"), made.execute().to_string(), "where it was made");
			const std::vector<uint8_t> bytes = made.save_bytes();
			const auto loaded_in = [&](engine& e) {
				try {
					return e.jaibite_load_bytes(bytes).execute().to_string();
				} catch (const std::exception& error) {
					return std::string("ERR: ") + error.what();
				}
			};
			{
				auto same_text = engine_with(lib_v1);
				check_eq(std::string("1|4|1|13|Cell<Cell<int>>"), loaded_in(*same_text), "an engine that holds the text it was saved from");
			}
			// another body for every generic the bite names (a namespace's function declared again by another text is written
			// override), and live objects of the instances it names
			const std::string lib_v2 = "class Cell<T> { T v; int tag() { return 2; } }\nT same<T>(T x) { return x + x; }\nnamespace box { class Lid<T> { T v; int tag() { return 20; } } T pick<T>(T x) override { return x + 1; } }\n";
			const std::string live = "Cell<float> early; Cell<Cell<int>> early_nested; Cell<int> early_int; box::Lid<int> early_lid; int warm = same(1) + box::pick(1);\n"
			                         "string held() { return to_string(early.tag()) + to_string(early_nested.tag()) + to_string(early_int.tag()) + \"|\" + to_string(early_lid.tag()) + \"|\" + to_string(same(1)) + to_string(box::pick(1)); }\n";
			{
				auto other = engine_with(lib_v2 + live);
				check_eq(std::string("222|20|22"), outcome(*other, "held();"), "before the bite");
				check_eq(std::string("2|8|2|24|Cell<Cell<int>>"), loaded_in(*other), "the bite runs the text the engine holds");
				check_eq(std::string("222|20|22"), outcome(*other, "held();"), "and what live code holds answers as it did");
				check_eq(std::string("2|8|2|24|Cell<Cell<int>>"), loaded_in(*other), "loaded and run again");
				check_eq(std::string("222|20|22"), outcome(*other, "held();"));
			}
			{
				auto unmade = engine_with(lib_v2);   // the generics declared, no instance made yet
				check_eq(std::string("2|8|2|24|Cell<Cell<int>>"), loaded_in(*unmade), "no instance made before the bite");
				check_eq(std::string("2"), outcome(*unmade, "Cell<float> later; later.tag();"), "a later unit's instance is the engine's text");
			}
			// a bite made in this engine, then the generic declared again: its next run follows the declaration
			{
				auto e = engine_with(lib_v1);
				jaibite own = e->jaibite(user);
				check_eq(std::string("1|4|1|13|Cell<Cell<int>>"), own.execute().to_string());
				e->execute(lib_v2 + live);
				check_eq(std::string("222|20|22"), outcome(*e, "held();"));
				check_eq(std::string("2|8|2|24|Cell<Cell<int>>"), own.execute().to_string(), "the bite's own next run");
				check_eq(std::string("222|20|22"), outcome(*e, "held();"), "left live code as it was");
			}
			// a declaration the bite's text no longer fits is the bite's load error, as the same text's would be
			const std::vector<std::pair<std::string, std::string>> unfit = {
				{"class Cell<K, V> { K v; int tag() { return 3; } }\nT same<T>(T x) { return x; }\nnamespace box { class Lid<T> { T v; int tag() { return 10; } } T pick<T>(T x) { return x; } }\n", "'Cell' takes 2 type arguments, not 1"},
				{"class Cell<T> { T held; int tag() { return 4; } }\nT same<T>(T x) { return x; }\nnamespace box { class Lid<T> { T v; int tag() { return 10; } } T pick<T>(T x) { return x; } }\n", "no member 'v' in class 'Cell<float>'"},
				{"class Cell<T> { T v; int tag() { return 1; } }\nT same<T>(T x, T y) { return x; }\nnamespace box { class Lid<T> { T v; int tag() { return 10; } } T pick<T>(T x) { return x; } }\n", "'same<int>' expects 2 argument(s), got 1"},
				{"class Cell { int v = 1; int tag() { return 77; } }\nT same<T>(T x) { return x; }\nnamespace box { class Lid<T> { T v; int tag() { return 10; } } T pick<T>(T x) { return x; } }\n", "'Cell<float>' cannot be made: 'Cell' is a plain class here, not a generic"},
			};
			for (const auto& [text, says] : unfit) {
				auto e = engine_with(text);
				const std::string refused = loaded_in(*e);
				check_true(failed(refused) && has(refused, says), says + ": " + refused);
			}
			// where nothing declares the generic the bite's instances wait for a declaration, as they always did
			{
				auto bare = engine_with("");
				const std::string waits = loaded_in(*bare);
				check_true(failed(waits), "no generic declared: " + waits);
			}
		});

		test("a_saved_bite_runs_as_its_source_would_in_every_engine_it_meets", [this]() {
			// Bites saved from one text of a library of generics, loaded where the library has another text: each answers what its
			// own source answers there, or fails where that source fails, and whatever the engine's live code holds answers as it
			// did before the bite ran
			const auto library = [](const std::string& cell, const std::string& same, const std::string& tool, const std::string& box) {
				return "class Limit { int n = 0; }\n" + cell + "\n" + same + "\n" + tool + "\n" + box + "\n";
			};
			const std::string cell = "class Cell<T> { T v; int tag() { return 1; } }", same = "T same<T>(T x) { return x; }";
			const std::string tool = "class Tool { int level = 1; T scaled<T>(T x) { return x; } }";
			const std::string box = "namespace box { class Lid<T> { T v; int tag() { return 10; } } T pick<T>(T x) { return x; } }";
			const std::vector<std::pair<std::string, std::string>> engines = {
				{"the text the bites were saved from", library(cell, same, tool, box)},
				{"other bodies", library("class Cell<T> { T v; int tag() { return 2; } }", "T same<T>(T x) { return x + x; }", "class Tool { int level = 1; T scaled<T>(T x) { return x + x; } }",
				                          "namespace box { class Lid<T> { T v; int tag() { return 20; } } T pick<T>(T x) { return x + 1; } }")},
				{"a field added", library("class Cell<T> { T v; int extra = 7; int tag() { return 3 + extra; } }", same, tool, box)},
				{"another count of type parameters", library("class Cell<K, V> { K v; int tag() { return 4; } }", same, tool, box)},
				{"another count of value parameters", library(cell, "T same<T>(T x, T y) { return x; }", tool, box)},
				{"a bound the arguments do not meet", library("class Cell<T : Limit> { T v; int tag() { return 5; } }", same, tool, box)},
				{"a plain class of the name", library("class Cell { int v = 1; int tag() { return 6; } }", same, tool, box)},
				{"a plain function of the name", library(cell, "int same(int x) { return x + 100; }", tool, box)},
				{"the method no longer generic", library(cell, same, "class Tool { int level = 1; int scaled(int x) { return x + 50; } }", box)},
				{"another namespace text", library(cell, same, tool, "namespace box { class Lid<T> { T v; int tag() { return 30; } } T pick<T>(T x, int n = 2) { return x + n; } }")},
				{"no class generic", library("", same, tool, box)},
				{"nothing declared", ""},
			};
			const std::vector<std::pair<std::string, std::string>> sources = {
				{"classes and functions", "Cell<float> f; f.v = 1.5; Cell<Cell<int>> nested; nested.v.v = same(4); to_string(f.tag()) + \"|\" + to_string(nested.v.v) + \"|\" + reflect::type_of(nested);"},
				{"functions only", "to_string(same(4)) + \"|\" + same(\"s\") + \"|\" + to_string(same<float>(1.5));"},
				{"classes only", "Cell<int> a; Cell<string> b; b.v = \"t\"; to_string(a.tag() + b.tag()) + \"|\" + b.v;"},
				{"a generic method", "Tool t; to_string(t.scaled<int>(4)) + \"|\" + t.scaled(\"m\");"},
				{"a namespace's", "box::Lid<int> lid; lid.v = box::pick(3); to_string(lid.tag() + lid.v) + \"|\" + to_string(box::pick<float>(0.5));"},
				{"its own generic beside the library's", "class Pair<A, B> { A a; B b; } U twice<U>(U x) { return x + x; } Pair<int, Cell<int>> p; p.a = twice(same(2)); to_string(p.a) + \"|\" + to_string(p.b.tag());"},
			};
			const std::string live = "Cell<float> early; Cell<Cell<int>> early_nested; Cell<int> early_int; Cell<string> early_text; box::Lid<int> early_lid; Tool early_tool;\n"
			                         "string held() { return to_string(early.tag()) + to_string(early_nested.tag()) + to_string(early_int.tag()) + to_string(early_text.tag()) + \"|\" + to_string(early_lid.tag()) + \"|\" + "
			                         "to_string(same(1)) + same(\"h\") + to_string(same(0.5)) + \"|\" + to_string(box::pick(1)) + to_string(box::pick(0.5)) + \"|\" + to_string(early_tool.scaled(2)) + early_tool.scaled(\"k\"); }\n";
			const auto engine_with = [](const std::string& text) {
				auto e = make_engine();
				stdlib::register_all(*e);
				if (!text.empty()) { e->execute(text); }
				return e;
			};
			const auto outcome = [](engine& e, const std::string& code) {
				try {
					return e.execute(code).to_string();
				} catch (const std::exception& error) {
					return std::string("ERR: ") + error.what();
				}
			};
			size_t cases = 0, refused = 0;
			std::string wrong;
			std::set<std::string> waiting;
			for (const auto& [source_name, source] : sources) {
				auto maker = engine_with(engines.front().second);
				jaibite made = maker->jaibite(source);
				const std::string where_made = made.execute().to_string();
				const std::vector<uint8_t> bytes = made.save_bytes();
				for (const auto& [engine_name, text] : engines) {
					for (const bool with_live : {false, true}) {
						const std::string what = source_name + ", loaded where the library has " + engine_name + (with_live ? ", live instances made" : "");
						auto e = engine_with(text);
						auto twin = engine_with(text);
						std::string before;
						if (with_live) {
							before = outcome(*e, live + "held();");
							if (failed(before)) { continue; }   // this library does not make those instances
							(void)outcome(*twin, live + "held();");
						}
						const std::string expected = outcome(*twin, source);
						std::string got;
						try {
							jaibite loaded = e->jaibite_load_bytes(bytes);
							got = loaded.execute().to_string();
						} catch (const std::exception& error) {
							got = std::string("ERR: ") + error.what();
						}
						++cases;
						if (failed(expected)) {
							++refused;
							if (!failed(got)) { wrong += what + ": the source fails (" + expected.substr(0, 110) + "), the bite answered " + got + "\n"; }
						} else if (got != expected) {
							// a call the saved text bound to a generic's instance stays that instance's: where the engine declares no
							// generic of the name the bite waits for one and says so, though its source would reach the plain function
							if (has(got, "is not declared yet: it waits for") || has(got, "cannot be made: ")) { waiting.insert(source_name + " | " + engine_name); }
							else { wrong += what + ": the source answers " + expected + ", the bite " + got.substr(0, 260) + "\n"; }
						}
						if (with_live && before != outcome(*e, "held();")) { wrong += what + ": what live code holds changed\n"; }
						if (engine_name == engines.front().first && got != where_made) { wrong += what + ": not as where it was made: " + got.substr(0, 200) + "\n"; }
					}
				}
			}
			check_eq(std::string(), wrong);
			std::string waited;
			for (const auto& w : waiting) { waited += w + "\n"; }
			check_eq(std::string("classes and functions | a plain function of the name\n"), waited);
			check_true(cases >= 90 && refused >= 20, "the table covers loads that run and loads that are refused: " + std::to_string(cases) + " cases, " + std::to_string(refused) + " refused");
		});

		test("a_diagnostic_about_another_files_text_names_that_file", [this]() {
			// A unit's listing names the unit's file. A place in another file's text (a file spliced into a function's body, a
			// generic another file declares) is that file's: its path leads the line
			program p("g1_names");
			p.write("part.jai", "int n = \"text\";\n");
			p.write("outer.jai", "int f() {\n\tinclude \"part.jai\";\n\treturn 1;\n}\n");
			const std::string spliced = p.run_file("outer.jai");
			check_true(failed(spliced) && has(spliced, "part.jai:1:") && !has(spliced, "outer.jai:1:"), spliced);
			p.write("lib.jai", "class Box<T> {\n\tT item;\n\tint size() { return item.size(); }\n}\nT twice<T>(T x) { return x.none(); }\n");
			p.write("game.jai", "import \"lib.jai\";\nBox<int> b;\nint n = b.size();\n");
			const std::string instance = p.run_file("game.jai");
			check_true(failed(instance) && has(instance, "lib.jai:3:") && has(instance, "in Box<int>, instantiated at ") && has(instance, "game.jai:2:"), instance);
			p.write("calls.jai", "import \"lib.jai\";\nint m = twice(3);\n");
			const std::string function_instance = p.run_file("calls.jai");
			check_true(failed(function_instance) && has(function_instance, "lib.jai:5:") && has(function_instance, "calls.jai:2:"), function_instance);
			// a declaration a diagnostic cites is cited with its file when that is another: an instance of a generic lib.jai declares
			p.write("uses.jai", "import \"lib.jai\";\nBox<string> s;\ns.nothing = 1;\ns.item = 3;\nclass Own { int hp = 1; }\nOwn o;\no.nothing = 2;\n");
			const std::string cited = p.run_file("uses.jai");
			check_true(has(cited, "no member 'nothing' in class 'Box<string>' (declared at ") && has(cited, "lib.jai:1:7)"), cited);
			check_true(has(cited, "cannot assign 'int' to field 'item' declared 'string'") && has(cited, "lib.jai:2:4)"), cited);
			check_true(has(cited, "no member 'nothing' in class 'Own' (declared at 5:7)"), "one in the unit's own file is cited by line: " + cited);
			// a warning in text spliced into a method cites the field it shadows in the class's own file
			p.write("part4.jai", "int hp = 3;\n");
			p.write("keeper.jai", "class Keeper {\n\tint hp = 1;\n\tint hit() {\n\t\tinclude \"part4.jai\";\n\t\treturn hp;\n\t}\n}\n");
			const std::string keeper = p.run_file("keeper.jai");
			std::string shadowed;
			for (const auto& d : p.eng->last_check_diagnostics().diagnostics) { shadowed += d.file + " " + d.message + "\n"; }
			check_true(has(shadowed, "part4.jai local 'hp' shadows field 'hp' of class 'Keeper' (field declared at ") && has(shadowed, "keeper.jai:2:"), keeper + " | " + shadowed);
			// spliced text that names the unit's first instance, with a mistake of its own and one after it in the unit's text
			p.write("part2.jai", "Box<string> held;\nint n = \"text\";\n");
			p.write("outer2.jai", "import \"lib.jai\";\nint g() {\n\tinclude \"part2.jai\";\n\tint m = \"more\";\n\treturn 1;\n}\n");
			const std::string both = p.run_file("outer2.jai");
			check_true(failed(both) && has(both, "part2.jai:2:") && has(both, "outer2.jai:4:") && !has(both, "outer2.jai:2:"), both);
			// a mistake the parser finds in spliced text
			p.write("part3.jai", "int n = ;\n");
			p.write("outer3.jai", "int h() {\n\tinclude \"part3.jai\";\n\treturn 1;\n}\n");
			const std::string unparsed = p.run_file("outer3.jai");
			check_true(failed(unparsed) && has(unparsed, "part3.jai"), unparsed);
			// and an executed string that names them
			check_eq(std::string("null"), p.run_file("lib.jai"));
			const std::string from_text = p.run("Box<float> late; late.size();");
			check_true(failed(from_text) && has(from_text, "lib.jai:3:") && has(from_text, "in Box<float>, instantiated at <script>:1:"), from_text);
		});

		test("an_unapplied_edit_of_a_generics_file_reaches_nothing_until_an_import_does", [this]() {
			// A generic's file changed on disk and no import has looked at it yet. Text the host runs meanwhile names instances
			// of the declarations the engine runs: held ones, new ones, a string run before the edit and again after it, a file
			// run again and a file loaded for the first time. The edit arrives with the import that reaches it, or is rejected there.
			for (const bool fits : {true, false}) {
				const std::string which = fits ? " (another body)" : " (a body Box<int> cannot take)";
				program p(fits ? "pend_fits" : "pend_breaks");
				const auto lib = [](const std::string& tag, const std::string& twice) {
					return "class Box<T> {\n\tT item;\n\tint tag() { return " + tag + "; }\n}\nT twice<T>(T x) { return " + twice + "; }\nint plain() { return " + tag + "; }\n";
				};
				p.write("lib.jai", lib("1", "x"));
				p.write("root.jai", "import \"lib.jai\";\nBox<int> b;\nint use() { return b.tag() + twice(1) + plain(); }\n");
				p.write("other.jai", "Box<int> o;\nint other() { Box<float> g; return o.tag() + g.tag() + twice(2); }\n");
				check_eq(std::string("null"), p.run_file("root.jai"), which);
				check_eq(std::string("null"), p.run_file("other.jai"), which);
				check_eq(std::string("3|4"), p.run("to_string(use()) + \"|\" + to_string(other());"), which);
				check_eq(std::string("4"), p.run("Box<int> c2; c2.tag() + twice(3);"), "before the edit" + which);
				check_eq(std::string("1"), p.run("Box<float> f; f.tag();"), "before the edit" + which);
				p.write("lib.jai", fits ? lib("2", "x + x") : "class Box<T> {\n\tT item;\n\tint tag() { return item.size(); }\n}\nT twice<T>(T x) { return x.none(); }\nint plain() { return 2; }\n");
				const std::vector<std::pair<std::string, std::string>> meanwhile = {
					{"use();", "3"},
					{"Box<int> c; c.tag();", "1"},                   // an instance the engine holds
					{"Box<string> s; s.tag();", "1"},                // one it makes now
					{"twice(5);", "5"},
					{"twice(\"ab\");", "ab"},
					{"plain();", "1"},
					{"Box<int> c2; c2.tag() + twice(3);", "4"},      // a string that ran before the edit
					{"Box<float> f; f.tag();", "1"},
					{"other();", "4"},
				};
				for (const auto& [code, answer] : meanwhile) { check_eq(answer, p.run(code), "the edit on disk, " + code + which); }
				check_eq(std::string("null"), p.run_file("other.jai"), "a file run again with its text" + which);
				p.write("third.jai", "int third() { Box<int> t; Box<char> ch; return t.tag() + ch.tag() + twice(4); }\n");
				check_eq(std::string("null"), p.run_file("third.jai"), "a file's first load" + which);
				check_eq(std::string("4|6|3"), p.run("to_string(other()) + \"|\" + to_string(third()) + \"|\" + to_string(use());"), which);
				const std::string reached = p.run("import \"lib.jai\"; use();");
				if (fits) {
					check_eq(std::string("6"), reached, "the import applies it");
					check_eq(std::string("6|2"), p.run("Box<string> late; to_string(use()) + \"|\" + to_string(late.tag());"));
				} else {
					check_true(failed(reached) && has(reached, "lib.jai:3:") && has(reached, "int has no member 'size'"), "the import rejects it: " + reached);
					check_eq(std::string("3|1"), p.run("Box<string> late; to_string(use()) + \"|\" + to_string(late.tag());"), "and the code that ran runs on");
				}
			}
		});

		test("a_bites_instance_of_a_generic_the_engine_lacks_waits_and_says_what_holds_the_name", [this]() {
			// A bite's instance of a generic the engine does not declare waits for it, as any unit's does: the bite loads, a use
			// reached before the generic is declared raises there (code before it has run), and once the generic is declared the
			// same bite runs. Where a plain class or function holds the name, the text says so. A method's instance is never added
			// to a class that stands without the generic, in an engine with no generic at all too: the class keeps the methods it
			// declared and the bite's call raises.
			const std::string lib = "class Cell<T> { T v; int tag() { return 1; } }\nT same<T>(T x) { return x; }\nclass Tool { int level = 1; T scaled<T>(T x) { return x; } }\n";
			struct loaded_bite {
				std::shared_ptr<engine> eng;
				std::string ran;
				std::string at_load = "loaded";
				std::string at_run;
			};
			const auto meet = [&](const std::string& source, const std::string& engine_text, const std::string& declared_later) {
				std::vector<uint8_t> bytes;
				{
					auto maker = make_engine();
					stdlib::register_all(*maker);
					maker->add_function("mark", [](const std::string&) {});
					maker->execute(lib);
					bytes = maker->jaibite(source).save_bytes();
				}
				auto result = std::make_shared<loaded_bite>();
				result->eng = make_engine();
				stdlib::register_all(*result->eng);
				result->eng->add_function("mark", [raw = result.get()](const std::string& s) { raw->ran += s + ";"; });
				if (!engine_text.empty()) { result->eng->execute(engine_text); }
				try {
					jaibite bite = result->eng->jaibite_load_bytes(bytes);
					const auto run = [&]() {
						try {
							return "= " + bite.execute().to_string();
						} catch (const std::exception& error) {
							const std::string text = error.what();
							return text.substr(0, text.find('\n'));
						}
					};
					result->at_run = run();
					if (!declared_later.empty()) {
						result->eng->execute(declared_later);
						result->at_run += " | then " + run();
					}
				} catch (const std::exception& error) {
					const std::string text = error.what();
					result->at_load = text.substr(0, text.find('\n'));
				}
				return result;
			};
			const std::string calls = "mark(\"before\"); int n = same(4); mark(\"after\"); n;";
			const std::string makes = "mark(\"before\"); Cell<float> c; mark(\"after\"); c.tag();";
			const std::string method = "mark(\"before\"); Tool t; int n = t.scaled<int>(4); mark(\"after\"); n;";
			// the generic is declared after the bite loads: the first run raises at the use, the next one runs
			auto early = meet(calls, "", lib);
			check_eq(std::string("loaded"), early->at_load);
			check_eq(std::string("'same<int>' is not declared yet: it waits for 'same' | then = 4"), early->at_run);
			check_eq(std::string("before;before;after;"), early->ran, "the first run reached the use");
			auto early_class = meet(makes, "class Unrelated { int n = 1; }", lib);
			check_eq(std::string("'Cell<float>' is not declared yet: it waits for 'Cell' | then = 1"), early_class->at_run);
			check_eq(std::string("before;before;after;"), early_class->ran);
			// a plain function or class holds the name: the use says so
			auto plain_function = meet(calls, "int same(int x) { return x + 100; }", "");
			check_eq(std::string("loaded"), plain_function->at_load);
			check_eq(std::string("'same<int>' cannot be made: 'same' is a plain function here, not a generic"), plain_function->at_run);
			check_eq(std::string("104"), plain_function->eng->execute("same(4);").to_string(), "the engine's function is untouched");
			auto plain_class = meet(makes, "class Cell { int v = 1; int tag() { return 6; } }", "");
			check_eq(std::string("'Cell<float>' cannot be made: 'Cell' is a plain class here, not a generic"), plain_class->at_run);
			check_eq(std::string("6"), plain_class->eng->execute("Cell c; c.tag();").to_string(), "the engine's class is untouched");
			// a method's instance where the class stands without the generic, whether or not the engine declares any generic
			for (const std::string others : {"", "T unrelated<T>(T x) { return x; }\n"}) {
				const std::string where = others.empty() ? "no generic in the engine" : "another generic in the engine";
				auto kept_out = meet(method, others + "class Tool { int level = 1; int scaled(int x) { return x + 50; } }", "");
				check_eq(std::string("loaded"), kept_out->at_load, where);
				check_eq(std::string("Tool has no member 'scaled<int>'"), kept_out->at_run, where + ": the bite's call finds no such method");
				check_eq(std::string("before;"), kept_out->ran, where);
				check_eq(std::string("54"), kept_out->eng->execute("Tool t; t.scaled(4);").to_string(), where + ": the engine's method is the one it declared");
			}
			// where its class is not declared at all, the bite's check says so
			auto no_class = meet(method, "", "");
			check_true(no_class->at_load.rfind("static check failed", 0) == 0 && no_class->ran.empty(), no_class->at_load);
		});
	}
};

}

using reload_verdict_tests = jai::foundry::tests::reload_verdict_tests;
FOUNDRY_REGISTER(reload_verdict_tests)
