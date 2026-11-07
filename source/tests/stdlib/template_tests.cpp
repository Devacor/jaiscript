#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <filesystem>
#include <fstream>
#include <string>

namespace jai::foundry::tests {

namespace {
	struct template_host_thing {
		std::string secret = "host";
		std::string reveal() const { return secret; }
	};
}

class template_tests : public suite {
public:
	template_tests() : suite("Template Tests") {}

	void forge_tests() override {
		// A folder of its own per test: a template file and the script that renders it
		struct folder {
			std::filesystem::path dir;
			explicit folder(const std::string& a_name) : dir(jai::foundry::test_temp_directory() / ("templates_" + a_name)) {
				std::error_code ec;
				std::filesystem::remove_all(dir, ec);
				std::filesystem::create_directories(dir);
			}
			~folder() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
			std::string write(const std::string& a_file, const std::string& a_text) const {
				std::error_code ec;
				std::filesystem::create_directories((dir / a_file).parent_path(), ec);
				std::ofstream out(dir / a_file, std::ios::binary | std::ios::trunc);
				out << a_text;
				return (dir / a_file).string();
			}
		};
		auto engine_for = []() {
			auto eng = make_engine();
			stdlib::register_all(*eng);
			stdlib::register_web(*eng);
			return eng;
		};
		auto run = [](std::shared_ptr<engine>& a_engine, const folder& a_folder, const std::string& a_script) {
			const std::string main = a_folder.write("main.jai", a_script);
			return a_engine->execute_file(main).as_string();
		};
		auto failure = [](std::shared_ptr<engine>& a_engine, const folder& a_folder, const std::string& a_script) {
			const std::string main = a_folder.write("main.jai", a_script);
			try {
				a_engine->execute_file(main);
			} catch (const std::exception& e) {
				return std::string(e.what());
			}
			return std::string("<no error>");
		};

		test("splices_escape_and_a_nested_template_is_not_escaped_twice", [=, this]() {
			folder f("nesting");
			f.write("item.spider", "<li>${item}</li>");
			f.write("list.spider", "<ul>${template(\"item.spider\", item)}${raw(\"<hr>\")}</ul>${title:>8}");
			auto eng = engine_for();
			check_eq(std::string("<ul><li>a &lt;b&gt; &amp; &#39;c&#39;</li><hr></ul>     x&lt;y"),
				run(eng, f, "string item = \"a <b> & 'c'\"; string title = \"x<y\"; template(\"list.spider\", item, title).to_string();"));
			check_eq(std::string(""), eng->execute("string hidden = \"\"; for (var n : reflect::functions()) { if (to_string(n).contains(\"#\")) { hidden += to_string(n); } } hidden;").as_string(),
				"the builtins a template desugars to are not listed");
		});

		test("an_array_of_html_joins_and_an_array_of_text_joins_escaped", [=, this]() {
			folder f("join");
			f.write("row.spider", "<td>${cell}</td>");
			f.write("table.spider", "<tr>${array::map(cells, cell => template(\"row.spider\", cell))}</tr>${cells}");
			auto eng = engine_for();
			check_eq(std::string("<tr><td>1</td><td>&lt;2&gt;</td></tr>1&lt;2&gt;"),
				run(eng, f, "array<string> cells = [\"1\", \"<2>\"]; template(\"table.spider\", cells).to_string();"));
		});

		test("names_bind_by_reference_with_their_static_type", [=, this]() {
			folder f("reference");
			f.write("bump.spider", "${count += 2}|${names[0] = \"q\"}");
			auto eng = engine_for();
			check_eq(std::string("7|q 7 q"), run(eng, f,
				"int count = 5; array<string> names = [\"x\", \"y\"]; string page = template(\"bump.spider\", count, names).to_string(); `${page} ${count} ${names[0]}`;"));
			f.write("store.spider", "${count = \"text\"}");
			const std::string wrong = failure(eng, f, "int count = 1; template(\"store.spider\", count);");
			check(wrong.find("store.spider:1:") != std::string::npos && wrong.find("count") != std::string::npos, "a store the caller's type refuses fails the load at the template's line: " + wrong);
		});

		test("the_template_sees_its_names_and_the_program_and_nothing_else", [=, this]() {
			folder f("visibility");
			f.write("page.spider", "${shout(who)} ${limit}");
			auto eng = engine_for();
			check_eq(std::string("ADA! 3"), run(eng, f,
				"int limit = 3; string shout(string s) { return string::to_upper(s) + \"!\"; } string who = \"global\"; "
				"class Page { string who = \"field\"; string render() { string who = \"ada\"; return template(\"page.spider\", who).to_string(); } } Page().render();"));
			f.write("leak.spider", "<p>\n${secret}</p>");
			const std::string leak = failure(eng, f, "void f() { int secret = 1; string who = \"x\"; template(\"leak.spider\", who); } f();");
			check(leak.find("leak.spider:2:3: error: use of undeclared identifier 'secret'") != std::string::npos, "an enclosing local the call does not pass is unseen: " + leak);
			f.write("this.spider", "${this.x}");
			const std::string self = failure(eng, f, "class C { int x = 1; void f() { template(\"this.spider\"); } } C().f();");
			check(self.find("'this' is not one of them") != std::string::npos, "this is not a name the template sees: " + self);
		});

		test("load_time_checks_point_at_the_template_line", [=, this]() {
			folder f("checks");
			f.write("entry.spider", "<article>\n<p>${entry.mesage}</p>\n</article>\n");
			auto eng = engine_for();
			const std::string member = failure(eng, f, "class Entry { string message = \"hi\"; } Entry entry = Entry(); template(\"entry.spider\", entry);");
			check(member.find("entry.spider:2:12: error: no member 'mesage' in class 'Entry'") != std::string::npos, "a member typo names the template's line: " + member);
			f.write("call.spider", "x\ny\n${fromat(1)}");
			const std::string call = failure(eng, f, "template(\"call.spider\");");
			check(call.find("call.spider:3:3: error: use of undeclared identifier 'fromat'") != std::string::npos, "a misspelled function names the template's line: " + call);
			const std::string plain = failure(eng, f, "array<int> rows = [1]; template(\"call.spider\", rows[0]);");
			check(plain.find("template() argument 2 is not a plain variable name") != std::string::npos && plain.find("(auto entry = rows[0];)") != std::string::npos,
				"an argument that is not a name fails the load with the local to write: " + plain);
			check_eq(std::string("<no error>"), failure(eng, f, "var r = 1; string s = \"\";"));
		});

		test("an_edited_template_applies_on_the_next_import_and_a_broken_edit_keeps_the_last_version", [=, this]() {
			folder f("reload");
			const std::string page = f.write("page.spider", "one ${n}");
			f.write("views.jai", "string view(int n) { return template(\"page.spider\", n).to_string(); }\n");
			auto eng = engine_for();
			const std::string main = f.write("main.jai", "import \"views.jai\"; view(1);");
			check_eq(std::string("one 1"), eng->execute_file(main).as_string());
			auto render = [&]() { return eng->execute("import \"" + (f.dir / "views.jai").generic_string() + "\"; view(2);").as_string(); };
			f.write("page.spider", "two, ${n}!");
			check_eq(std::string("two, 2!"), render(), "the edit applies on the next import");
			f.write("page.spider", "three ${missing_name}");
			check_throws([&]() { render(); }, "the broken edit is reported by the import that reaches it");
			check_eq(std::string("two, 2!"), eng->execute("view(2);").as_string(), "the last version that checked keeps rendering");
			f.write("page.spider", "four ${n} fixed");
			check_eq(std::string("four 2 fixed"), render(), "the fix applies");
		});

		test("a_path_known_at_run_time_compiles_at_first_render_and_reloads", [=, this]() {
			folder f("dynamic");
			f.write("dyn.spider", "hello ${who}");
			auto eng = engine_for();
			const std::string main = f.write("main.jai", "string which = \"dyn.spider\"; string render(string who) { return template(which, who).to_string(); } render(\"ada\");");
			check_eq(std::string("hello ada"), eng->execute_file(main).as_string());
			f.write("dyn.spider", "hi there ${who}");
			check_eq(std::string("hi there bo"), eng->execute("render(\"bo\");").as_string(), "an edit applies at the next render");
			f.write("dyn.spider", "broken ${nobody}");
			check_throws([&]() { eng->execute("render(\"cy\");"); }, "a broken edit raises once");
			check_eq(std::string("hi there cy"), eng->execute("render(\"cy\");").as_string(), "then the last version that checked renders");
			f.write("dyn.spider", "fixed ${who}!");
			check_eq(std::string("fixed di!"), eng->execute("render(\"di\");").as_string());
			check_eq(std::string("template file 'nope.spider' not found"), eng->execute("string which2 = \"nope.spider\"; string s = \"\"; try { template(which2, s); } catch (e) { s = e; } s;").as_string());
		});

		test("a_template_rendering_itself_recurses_through_the_run_time_path", [=, this]() {
			folder f("recursion");
			f.write("tree.spider", "<li>${node[\"name\"]}${array::map(node[\"kids\"], node => template(\"tree.spider\", node))}</li>");
			auto eng = engine_for();
			check_eq(std::string("<li>root<li>a</li><li>b<li>c</li></li></li>"), run(eng, f,
				"var node = {\"name\": \"root\", \"kids\": [{\"name\": \"a\", \"kids\": []}, {\"name\": \"b\", \"kids\": [{\"name\": \"c\", \"kids\": []}]}]}; template(\"tree.spider\", node).to_string();"));
		});

		// --------------------------------------------------------- scoped_template: one test per rule, each with the escape it blocks

		auto scoped_error = [=](const std::string& a_setup, const std::string& a_text) {
			auto eng = engine_for();
			dynamic_binder<template_host_thing>(*eng, "TemplateHostThing").constructor<>().method("reveal", &template_host_thing::reveal).build();
			const std::string script = a_setup + " string text = R\"tpl(" + a_text + ")tpl\"; string m = \"<rendered>\"; try { m = scoped_template_text(text, data).to_string(); } catch (e) { m = e; } m;";
			return eng->execute(script).as_string();
		};
		const std::string data = "int secret = 42; int leak() { return secret; } class Pet { string name = \"Rex\"; string speak() { return \"secret \" + to_string(secret); } bool operator==(Pet o) { return secret == 42; } } "
			"map<string, var> data = {\"name\": \"<ann>\", \"pet\": Pet(), \"host\": TemplateHostThing(), \"call\": () => secret, \"items\": [\"b\", \"a\"]};";

		test("scoped_template_renders_built_in_values_escaped", [=, this]() {
			check_eq(std::string("&lt;ann&gt; Rex 002 B,A"),
				scoped_error(data, "${args[\"name\"]} ${args[\"pet\"].name} ${args[\"items\"].size():03} ${array::map(args[\"items\"], i => string::to_upper(i)).join(\",\")}"));
			auto eng = engine_for();
			check_eq(std::string("ab [\"b\",\"a\"]"), eng->execute("map<string, var> d = {\"items\": [\"b\", \"a\"]}; `${scoped_template_text(\"${array::sort(args[\\\"items\\\"])}\", d).to_string()} ${to_json(d[\"items\"])}`;").as_string(),
				"the string::, array:: and map:: functions work on copies");
			check_eq(std::string("sort changes obj in place and returns nothing; array::sort(obj) returns a sorted copy"),
				eng->execute("map<string, var> d = {\"items\": [\"b\", \"a\"]}; string m = \"\"; try { scoped_template_text(\"${args[\\\"items\\\"].sort()}\", d); } catch (e) { m = e; } m;").as_string(),
				"an in-place method's result is the language's error");
			check_eq(std::string(" \t\n\v\f\r|"), eng->execute("map<string, var> d; scoped_template_text(\"${string::whitespace}|\", d).to_string();").as_string(), "a namespace value reads");
		});

		test("scoped_template_refuses_a_global_read", [=, this]() {
			check_eq(std::string("parse failed:\n<scoped_template_text>:1:3: error: a scoped template sees only args: 'secret' is not available\n    ${secret}\n      ^"),
				scoped_error(data, "${secret}"));
		});

		test("scoped_template_refuses_a_free_function_call", [=, this]() {
			const std::string m = scoped_error(data, "${leak()}");
			check(m.find("a scoped template calls only built-in methods and the string::, array:: and map:: functions: 'leak(...)' is not allowed") != std::string::npos, m);
			const std::string other = scoped_error(data, "${web::html_escape(args[\"name\"])}");
			check(other.find("a scoped template calls only built-in methods and the string::, array:: and map:: functions: 'html_escape(...)' is not allowed") != std::string::npos, "another namespace's function: " + other);
			const std::string script_ns = scoped_error("namespace leaks { int f() { return 1; } } " + data, "${leaks::f()}");
			check(script_ns.find("'f(...)' is not allowed") != std::string::npos, "a script namespace's function: " + script_ns);
		});

		test("scoped_template_refuses_a_bound_cpp_method", [=, this]() {
			check_eq(std::string("a scoped template does not call a method of a class instance, a host object or a function: its code could run"),
				scoped_error(data, "${args[\"host\"].reveal()}"));
			check_eq(std::string("a scoped template reads the plain fields of a script class instance: 'secret' is not one"),
				scoped_error(data, "${args[\"host\"].secret}"), "a host property's getter is host code");
		});

		test("scoped_template_refuses_a_script_class_method", [=, this]() {
			check_eq(std::string("a scoped template does not call a method of a class instance, a host object or a function: its code could run"),
				scoped_error(data, "${args[\"pet\"].speak()}"));
			check_eq(std::string("a scoped template does not splice a class instance, a host object or a function: its code could run"),
				scoped_error(data, "${args[\"pet\"]}"), "splicing an instance would run its to_string()");
			check_eq(std::string("a scoped template does not call a built-in method over a class instance, a host object or a function: its code could run"),
				scoped_error(data, "${array::map(args[\"items\"], i => args[\"pet\"]).size()}"), "a built-in that would reach an instance's code");
		});

		test("scoped_template_refuses_an_operator_overload", [=, this]() {
			check_eq(std::string("a scoped template does not apply an operator to a class instance, a host object or a function: its code could run"),
				scoped_error(data, "${args[\"pet\"] == args[\"pet\"]}"));
		});

		test("scoped_template_refuses_a_function_value_call", [=, this]() {
			const std::string held = scoped_error(data, "${args[\"call\"]()}");
			check(held.find("a scoped template does not call function values") != std::string::npos, "a function held in the map: " + held);
			check_eq(std::string("map has no member 'call'; read the key as [\"call\"]"), scoped_error(data, "${({\"k\": 1}).call()}"), "a map has no methods past its built-ins");
			const std::string m = scoped_error(data, "${(x => x)(1)}");
			check(m.find("a scoped template does not call function values") != std::string::npos, m);
		});

		test("scoped_template_refuses_raw", [=, this]() {
			const std::string m = scoped_error(data, "${raw(args[\"name\"])}");
			check(m.find("raw() is not allowed in a scoped template: its output is always escaped") != std::string::npos, m);
		});

		test("scoped_template_caps_its_output", [=, this]() {
			auto eng = engine_for();
			check_eq(std::string("1048576 10485760"), eng->execute("`${to_string(reflect::scoped_template_output_limit())} ${reflect::template_output_limit()}`;").as_string(),
				"a script reads both limits, at their defaults");
			eng->scoped_template_output_limit(16);
			check_eq(std::string("a scoped template's output passed its 16-byte limit: the host raises it with engine::scoped_template_output_limit"), eng->execute(
				"map<string, var> d = {\"s\": \"0123456789\"}; string m = \"\"; try { scoped_template_text(\"${args[\\\"s\\\"]}${args[\\\"s\\\"]}\", d); } catch (e) { m = e; } m;").as_string());
			check_eq(std::string("0123456789"), eng->execute("map<string, var> d = {\"s\": \"0123456789\"}; scoped_template_text(\"${args[\\\"s\\\"]}\", d).to_string();").as_string());
			check_eq(std::string("a scoped template's output passed its 16-byte limit: the host raises it with engine::scoped_template_output_limit"), eng->execute(
				"map<string, var> d = {\"s\": \"0123456789\"}; string m = \"\"; try { scoped_template_text(\"${string::repeat(args[\\\"s\\\"], 2).size()}\", d); } catch (e) { m = e; } m;").as_string(),
				"a built-in that makes a longer string counts as writing it");
			eng->scoped_template_output_limit(0);
			check_eq(std::string("20"), eng->execute("map<string, var> d = {\"s\": \"0123456789\"}; to_string(scoped_template_text(\"${args[\\\"s\\\"]}${args[\\\"s\\\"]}\", d).to_string().size());").as_string(), "0 is no limit");
		});

		test("template_caps_its_output", [=, this]() {
			folder f("cap");
			f.write("part.spider", "<i>${text}</i>");
			f.write("page.spider", "<p>${template(\"part.spider\", text)}</p>");
			auto eng = engine_for();
			eng->template_output_limit(12);
			check_eq(std::string("template()'s output passed its 12-byte limit: the host raises it with engine::template_output_limit"), run(eng, f,
				"string text = \"ab\"; string m = \"\"; try { m = template(\"page.spider\", text).to_string(); } catch (e) { m = e; } m;"));
			check_eq(std::string("12 1048576"), eng->execute("`${to_string(reflect::template_output_limit())} ${reflect::scoped_template_output_limit()}`;").as_string());
			eng->template_output_limit(16);
			check_eq(std::string("<p><i>ab</i></p>"), run(eng, f, "string text = \"ab\"; template(\"page.spider\", text).to_string();"), "the host raises it");
			eng->template_output_limit(0);
			check_eq(std::string("<p><i>abcdefghijklmnop</i></p>"), run(eng, f, "string text = \"abcdefghijklmnop\"; template(\"page.spider\", text).to_string();"), "0 is no limit");
		});

		test("templates_read_files_under_the_filesystem_privilege", [=, this]() {
			folder f("privilege");
			f.write("page.spider", "<b>${who}</b>");
			auto eng = engine_for();
			const std::string page = (f.dir / "page.spider").generic_string();
			eng->disable(privilege::filesystem);
			auto caught = [&](const std::string& a_call) {
				return eng->execute("string who = \"x\"; map<string, var> m = {\"who\": \"y\"}; string p = \"" + page + "\"; string e = \"<rendered>\"; try { " + a_call + "; } catch (err) { e = err; } e;").as_string();
			};
			check_eq(std::string("Insufficient privilege: filesystem."), caught("template(\"" + page + "\", who)"), "a literal path");
			check_eq(std::string("Insufficient privilege: filesystem."), caught("template(p, who)"), "a path known at run time");
			check_eq(std::string("<b>y</b>"), eng->execute("map<string, var> m = {\"who\": \"y\"}; scoped_template_text(\"<b>${args[\\\"who\\\"]}</b>\", m).to_string();").as_string(), "template text needs no file");
			eng->enable(privilege::filesystem);
			check_eq(std::string("<rendered>"), caught("template(\"" + page + "\", who)"), "granted again, the same call renders");
		});

		test("scoped_template_reads_a_map_key_with_brackets_never_a_dot", [=, this]() {
			auto eng = engine_for();
			const std::string load = [&]() {
				try { eng->execute("map<string, var> m = {\"name\": \"a\"}; scoped_template_text(\"x\\n${args.name}\", m);"); } catch (const std::exception& e) { return std::string(e.what()); }
				return std::string("<no error>");
			}();
			check(load.find("<scoped_template_text>:2:8: error: map has no member 'name'; read the key as [\"name\"]") != std::string::npos, "args.k fails the load: " + load);
			check_eq(std::string("map has no member 'k'; read the key as [\"k\"]"), eng->execute(
				"map<string, var> m = {\"inner\": {\"k\": 1}}; string e = \"\"; try { scoped_template_text(\"${args[\\\"inner\\\"].k}\", m); } catch (err) { e = err; } e;").as_string(),
				"a map the template reaches at run time raises when it renders");
			check_eq(std::string("1 a"), eng->execute("map<string, var> m = {\"inner\": {\"k\": 1}, \"name\": \"a\"}; scoped_template_text(\"${args[\\\"inner\\\"][\\\"k\\\"]} ${array::map([1], args => args)[0] == 1 ? args[\\\"name\\\"] : \\\"\\\"}\", m).to_string();").as_string(),
				"brackets read, and a lambda parameter named args is its own");
		});

		test("scoped_template_runs_on_its_own_privilege", [=, this]() {
			folder f("scoped_privilege");
			f.write("template/card.spider", "<b>${args[\"who\"]}</b>");
			auto eng = engine_for();
			eng->set_scoped_template_dirs({(f.dir / "template").string()});
			auto render = [&](const std::string& a_call) {
				return eng->execute("map<string, var> m = {\"who\": \"y\"}; string p = \"card.spider\"; string e = \"\"; try { e = " + a_call + ".to_string(); } catch (err) { e = err; } e;").as_string();
			};
			check_eq(std::string("true true"), eng->execute("to_string(reflect::is_enabled(jai::scoped_template)) + \" \" + to_string(reflect::is_enabled(\"scoped_template\"));").as_string(), "on by default");
			eng->disable(privilege::filesystem);
			check_eq(std::string("<b>y</b>"), render("scoped_template(\"card.spider\", m)"), "a literal path with filesystem off");
			check_eq(std::string("<b>y</b>"), render("scoped_template(p, m)"), "a path known at run time with filesystem off");
			eng->disable(privilege::scoped_template);
			check_eq(std::string("Insufficient privilege: scoped_template."), render("scoped_template(\"card.spider\", m)"), "a literal path");
			check_eq(std::string("Insufficient privilege: scoped_template."), render("scoped_template(p, m)"), "a path known at run time");
			check_eq(std::string("Insufficient privilege: scoped_template."), render("scoped_template_text(\"<i>${args[\\\"who\\\"]}</i>\", m)"), "template text");
			check_eq(std::string("false"), eng->execute("to_string(reflect::is_enabled(\"scoped_template\"));").as_string());
			eng->enable(privilege::scoped_template);
			check_eq(std::string("<i>y</i>"), render("scoped_template_text(\"<i>${args[\\\"who\\\"]}</i>\", m)"), "granted again");
		});

		test("scoped_template_reads_its_template_dirs_in_order", [=, this]() {
			folder f("scoped_dirs");
			f.write("template/card.spider", "default ${args[\"who\"]}");
			f.write("a/card.spider", "from a");
			f.write("b/card.spider", "from b");
			f.write("b/only.spider", "only b");
			f.write("loose.spider", "beside the script");
			auto eng = engine_for();
			check_eq(std::string("[\"template\"]"), eng->execute("to_json(reflect::scoped_template_dirs());").as_string(), "the default list");
			const std::string call = "map<string, var> m = {\"who\": \"x\"}; string e = \"\"; try { e = scoped_template(p, m).to_string(); } catch (err) { e = err; } e;";
			auto render = [&](const std::string& a_path) { return run(eng, f, "string p = \"" + a_path + "\"; " + call); };
			check_eq(std::string("default x"), render("card.spider"), "./template beside the calling script");
			check_eq(std::string("template file 'loose.spider' not found in the scoped template directories"), render("loose.spider"), "the script's own folder is not a template folder");
			eng->set_scoped_template_dirs({"a", "b"});
			check_eq(std::string("from a"), render("card.spider"), "the first folder that holds the file wins");
			check_eq(std::string("only b"), render("only.spider"), "a later folder answers what an earlier one lacks");
			eng->remove_scoped_template_dir("a");
			check_eq(std::string("from b"), render("card.spider"));
			eng->add_scoped_template_dir("template");
			check_eq(std::string("[\"b\",\"template\"]"), eng->execute("to_json(reflect::scoped_template_dirs());").as_string());
			eng->set_scoped_template_dirs({"missing"});
			check_eq(std::string("template file 'card.spider' not found in the scoped template directories"), render("card.spider"), "a folder that does not exist holds nothing");
		});

		test("scoped_template_refuses_a_path_that_leaves_its_dirs", [=, this]() {
			folder f("scoped_escape");
			f.write("template/inner/x.spider", "inner");
			f.write("secret.spider", "secret");
			auto eng = engine_for();
			const std::string call = "map<string, var> m; string e = \"\"; try { e = scoped_template(p, m).to_string(); } catch (err) { e = err; } e;";
			auto render = [&](const std::string& a_path) { return run(eng, f, "string p = R\"q(" + a_path + ")q\"; " + call); };
			check_eq(std::string("inner"), render("inner/x.spider"));
			check_eq(std::string("scoped_template: '../secret.spider' names a parent directory (..)"), render("../secret.spider"));
			check_eq(std::string("scoped_template: 'inner/../inner/x.spider' names a parent directory (..)"), render("inner/../inner/x.spider"), "even one that stays inside");
			check_eq(std::string("scoped_template: 'inner\\..\\x.spider' names a parent directory (..)"), render("inner\\..\\x.spider"));
			check_eq(std::string("scoped_template: '/secret.spider' is not a path relative to its template directories"), render("/secret.spider"));
			check_eq(std::string("scoped_template: '\\secret.spider' is not a path relative to its template directories"), render("\\secret.spider"));
			check_eq(std::string("scoped_template: 'C:/secret.spider' is not a path relative to its template directories"), render("C:/secret.spider"));
			check_eq(std::string("scoped_template: '//server/share/secret.spider' is not a path relative to its template directories"), render("//server/share/secret.spider"));
			const std::string absolute = (f.dir / "secret.spider").string();
			check_eq("scoped_template: '" + absolute + "' is not a path relative to its template directories", render(absolute));
			const std::string load = failure(eng, f, "map<string, var> m; scoped_template(\"../secret.spider\", m);");
			check(load.find("scoped_template: '../secret.spider' names a parent directory (..)") != std::string::npos, "a literal path fails the load: " + load);
			std::error_code ec;
			std::filesystem::create_directories(f.dir / "outside");
			f.write("outside/leak.spider", "leaked");
			std::filesystem::create_directory_symlink(f.dir / "outside", f.dir / "template" / "link", ec);
#if defined(_WIN32)
			if (ec) {
				const std::string junction = "cmd /c mklink /J \"" + (f.dir / "template" / "link").string() + "\" \"" + (f.dir / "outside").string() + "\" > nul 2>&1";
				ec = std::error_code(std::system(junction.c_str()), std::generic_category());
			}
#endif
			if (!ec && std::filesystem::exists(f.dir / "template" / "link" / "leak.spider")) {
				check_eq(std::string("scoped_template: 'link/leak.spider' leaves its template directory through a link"), render("link/leak.spider"), "a link out of the folder");
			}
		});

		test("a_null_splice_is_an_error", [=, this]() {
			folder f("null_splice");
			f.write("page.spider", "<p>\n${title}</p>${title ?? \"\"}");
			f.write("literal.spider", "${null}");
			auto eng = engine_for();
			const std::string null_title = run(eng, f, "var title = null; string e = \"\"; try { e = template(\"page.spider\", title).to_string(); } catch (err) { e = err; } e;");
			check(null_title.rfind("a template splice is null: ${title} at ", 0) == 0 && null_title.size() > 13 && null_title.substr(null_title.size() - 13) == "page.spider:2", "a null splice names itself and its line: " + null_title);
			check_eq(std::string("<p>\nhi</p>hi"), run(eng, f, "var title = \"hi\"; template(\"page.spider\", title).to_string();"), "?? stays the way out");
			const std::string load = failure(eng, f, "template(\"literal.spider\");");
			check(load.find("literal.spider:1:3: error: a template splice is null: ${null} at ") != std::string::npos, "a splice the checker sees is null fails the load: " + load);
			check_eq(std::string("a template splice is null: ${args[\"x\"]} at <scoped_template_text>:1"), eng->execute(
				"map<string, var> m = {\"x\": null}; string e = \"\"; try { scoped_template_text(\"${args[\\\"x\\\"]}\", m); } catch (err) { e = err; } e;").as_string());
			check_eq(std::string("-"), eng->execute("map<string, var> m = {\"x\": null}; scoped_template_text(\"${args[\\\"x\\\"] ?? \\\"-\\\"}\", m).to_string();").as_string());
			check_eq(std::string("x null"), eng->execute("var n = null; `x ${n}`;").as_string(), "a backtick string outside a template is unchanged");
		});

		test("scoped_template_reads_a_file_checked_at_load", [=, this]() {
			folder f("scoped");
			f.write("template/card.spider", "<b>${args[\"name\"]}</b>");
			f.write("template/bad.spider", "\n${secret}");
			auto eng = engine_for();
			check_eq(std::string("<b>&lt;i&gt;</b>"), run(eng, f, "map<string, var> m = {\"name\": \"<i>\"}; scoped_template(\"card.spider\", m).to_string();"));
			const std::string bad = failure(eng, f, "int secret = 1; map<string, var> m; scoped_template(\"bad.spider\", m);");
			check(bad.find("bad.spider:2:3: error: a scoped template sees only args: 'secret' is not available") != std::string::npos, bad);
		});
	}
};

}

using template_tests = jai::foundry::tests::template_tests;
FOUNDRY_REGISTER(template_tests)
