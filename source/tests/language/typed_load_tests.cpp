// A typed load (docs/site/guide/06-functions.html "Type arguments", docs/site/embedding/17-host-api.html "Records") over data
// that carries archive records (docs/site/embedding/08-serialization.html): from_json<T> and jai::make_record refuse a saved
// value its field cannot take whether or not the object that holds it is written with a $type
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/stdlib/json.hpp>
#include <jaiscript/detail/engine_access.hpp>
#include <jaiscript/detail/type_value.hpp>
#include <jaiscript/serialization/binary_archive.hpp>
#include <jaiscript/serialization/json_archive.hpp>
#include <string>
#include <vector>

namespace jai::foundry::tests {

class typed_load_tests : public suite {
public:
	typed_load_tests() : suite("Typed Loads") {}

	static std::string outcome(engine& eng, const std::string& code) {
		try {
			return eng.execute(code).to_string();
		} catch (const std::exception& e) {
			return std::string("ERR: ") + e.what();
		}
	}

	static std::string quoted(const std::string& text) {
		std::string out = "\"";
		for (const char c : text) {
			if (c == '"' || c == '\\') { out += '\\'; }
			out += c;
		}
		return out + "\"";
	}

	static const char* classes() {
		return "class Cell<T> { T held; } class Other { string held = \"o\"; } class Base { int b = 1; } class Derived : Base { int d = 2; }"
		       " class Outer { Cell<int> cell; Base base; array<Cell<int>> items; map<string, Cell<int>> keyed; shared_ptr<Cell<int>> h;"
		       " weak_ptr<Cell<int>> w; var any; function cb; int n = 3; bool flag = false; float f = 1.5; string s = \"d\"; }"
		       " Cell<int> ci; Cell<string> cs; Cell<Cell<int>> cc;";
	}

	static std::string tagged(const std::string& members) { return "{\"$type\":\"Outer\",\"$val\":" + members + "}"; }

	void forge_tests() override {
		test("a_tagged_record_refuses_the_member_its_untagged_form_refuses", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			e->execute(classes());
			const std::string wrong = "{\"$type\":\"Cell<string>\",\"$val\":{\"held\":\"x\"}}";
			const std::vector<std::pair<std::string, std::string>> cases = {
				{"{\"cell\":" + wrong + "}", "from_json: field 'cell' of 'Outer' is Cell<int>, not Cell<string>"},
				{"{\"cell\":{\"$type\":\"Other\",\"$val\":{\"held\":\"x\"}}}", "from_json: field 'cell' of 'Outer' is Cell<int>, not Other"},
				{"{\"items\":[" + wrong + "]}", "from_json: field 'items' of 'Outer' is array<Cell<int>>, not array"},
				{"{\"keyed\":{\"k\":" + wrong + "}}", "from_json: field 'keyed' of 'Outer' is map<string,Cell<int>>, not map"},
				{"{\"h\":{\"$id\":1,\"$type\":\"Cell<string>\",\"$val\":{\"held\":\"x\"}}}", "from_json: field 'h' of 'Outer' is shared_ptr<Cell<int>>, not Cell<string>"},
				{"{\"n\":\"x\"}", "from_json: field 'n' of 'Outer' is int, not string"},
				{"{\"s\":[1]}", "from_json: field 's' of 'Outer' is string, not array"},
				{"{\"n\":null}", "from_json: field 'n' of 'Outer' is int and cannot be null"},
				{"{\"f\":null}", "from_json: field 'f' of 'Outer' is float and cannot be null"},
				{"{\"cell\":{\"held\":\"x\"}}", "from_json: field 'held' of 'Cell<int>' is int, not string"},
			};
			for (const auto& [members, refusal] : cases) {
				check_eq("ERR: " + refusal, outcome(*e, "to_json(from_json<Outer>(" + quoted(members) + "));"), "untagged " + members);
				check_eq("ERR: " + refusal, outcome(*e, "to_json(from_json<Outer>(" + quoted(tagged(members)) + "));"), "tagged " + members);
			}
			// the record the test was written for: an instance of a generic under an instance of the same generic
			const std::string nested = "{\"held\":" + wrong + "}";
			const std::string refusal = "ERR: from_json: field 'held' of 'Cell<Cell<int>>' is Cell<int>, not Cell<string>";
			check_eq(refusal, outcome(*e, "to_json(from_json<Cell<Cell<int>>>(" + quoted(nested) + "));"), "untagged");
			check_eq(refusal, outcome(*e, "to_json(from_json<Cell<Cell<int>>>(" + quoted("{\"$type\":\"Cell<Cell<int>>\",\"$val\":" + nested + "}") + "));"), "tagged");
		});

		test("a_tagged_record_anywhere_in_a_typed_load_refuses_a_member_that_does_not_fit", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			e->execute(classes());
			const std::string unfit = "{\"$type\":\"Cell<int>\",\"$val\":{\"held\":\"x\"}}";
			const std::string refusal = "ERR: from_json: field 'held' of 'Cell<int>' is int, not string";
			check_eq(refusal, outcome(*e, "to_json(from_json<Cell<int>>(" + quoted(unfit) + "));"), "the root");
			check_eq(refusal, outcome(*e, "to_json(from_json<array<Cell<int>>>(" + quoted("[" + unfit + "]") + "));"), "an element of the root");
			check_eq(refusal, outcome(*e, "to_json(from_json<map<string, Cell<int>>>(" + quoted("{\"k\":" + unfit + "}") + "));"), "a value of the root");
			check_eq(refusal, outcome(*e, "to_json(from_json<shared_ptr<Cell<int>>>(" + quoted("{\"$id\":1,\"$type\":\"Cell<int>\",\"$val\":{\"held\":\"x\"}}") + "));"), "a handle");
			check_eq(refusal, outcome(*e, "to_json(from_json<Outer>(" + quoted("{\"any\":" + unfit + "}") + "));"), "a var field of an untagged record");
			check_eq(refusal, outcome(*e, "to_json(from_json<Outer>(" + quoted(tagged("{\"any\":" + unfit + "}")) + "));"), "a var field of a tagged record");
			check_eq(refusal, outcome(*e, "to_json(from_json<var>(" + quoted("{\"deep\":[{\"k\":" + unfit + "}]}") + "));"), "under plain data");
		});

		test("a_tagged_record_takes_the_member_its_untagged_form_takes", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			e->execute(classes());
			const std::vector<std::string> cases = {
				"{\"cell\":{\"held\":5}}",
				"{\"items\":[{\"held\":5},{\"$type\":\"Cell<int>\",\"$val\":{\"held\":6}}]}",
				"{\"keyed\":{\"k\":{\"held\":5}}}",
				"{\"h\":{\"held\":5}}",
				"{\"cell\":\"{\\\"held\\\":9}\"}",
				"{\"flag\":1,\"f\":2}",
				"{\"base\":{\"$type\":\"Derived\",\"$val\":{\"b\":7,\"d\":8}}}",
				"{\"cb\":null,\"h\":null,\"any\":null}",
				"{\"nobody\":1,\"n\":4}",
			};
			for (const std::string& members : cases) {
				const std::string plain = outcome(*e, "to_json(from_json<Outer>(" + quoted(members) + "));");
				check_true(plain.rfind("ERR: ", 0) != 0, "the untagged form loads: " + members + " -> " + plain);
				check_eq(plain, outcome(*e, "to_json(from_json<Outer>(" + quoted(tagged(members)) + "));"), "tagged " + members);
			}
			// what an archive converts stays converted under a tag: a float truncates into an int field as cast<int> does
			check_eq(std::string("7"), outcome(*e, "from_json<Outer>(" + quoted(tagged("{\"n\":7.9}")) + ").n;"));
		});

		test("an_archive_record_that_names_no_class_is_refused_where_a_class_belongs", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			e->execute(classes());
			const std::string stray = "{\"$type\":\"Cel<int>\",\"$val\":{\"held\":5}}";
			check_eq(std::string("ERR: from_json: the JSON is Cel<int>, not Cell<int>"), outcome(*e, "to_json(from_json<Cell<int>>(" + quoted(stray) + "));"), "the root");
			check_eq(std::string("ERR: from_json: the JSON is Nope, not Cell<int>"),
			         outcome(*e, "to_json(from_json<Cell<int>>(" + quoted("{\"$type\":\"Nope\",\"$val\":{\"held\":5}}") + "));"), "a name nothing declares");
			check_eq(std::string("ERR: from_json: field 'cell' of 'Outer' is Cell<int>, not Cel<int>"),
			         outcome(*e, "to_json(from_json<Outer>(" + quoted("{\"cell\":" + stray + "}") + "));"), "a field of an untagged record");
			check_eq(std::string("ERR: from_json: field 'cell' of 'Outer' is Cell<int>, not Cel<int>"),
			         outcome(*e, "to_json(from_json<Outer>(" + quoted(tagged("{\"cell\":" + stray + "}")) + "));"), "a field of a tagged record");
			check_eq(std::string("ERR: from_json: field 'h' of 'Outer' is shared_ptr<Cell<int>>, not Cel<int>"),
			         outcome(*e, "to_json(from_json<Outer>(" + quoted(tagged("{\"h\":" + stray + "}")) + "));"), "a handle field");
			check_eq(std::string("ERR: from_json: field 'items' of 'Outer' is array<Cell<int>>, not array"),
			         outcome(*e, "to_json(from_json<Outer>(" + quoted(tagged("{\"items\":[" + stray + "]}")) + "));"), "an element");
			// where no class belongs the record stays the data it is
			check_eq(std::string("Cel<int>"), outcome(*e, "from_json<Outer>(" + quoted(tagged("{\"any\":" + stray + "}")) + ").any[\"$type\"];"), "a var field");
			check_eq(std::string("Cel<int>"), outcome(*e, "from_json<map<string, var>>(" + quoted(stray) + ")[\"$type\"];"), "a map");
		});

		test("an_untyped_load_converts_or_keeps_the_constructed_value_and_records_it", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			e->execute(classes());
			e->hot_reload_reporting(true);
			const std::string members = "{\"cell\":{\"$type\":\"Cell<string>\",\"$val\":{\"held\":\"x\"}},\"n\":\"x\",\"f\":2}";
			check_eq(std::string("0|3|2.0"), outcome(*e, "var o = from_json(" + quoted(tagged(members)) + "); to_string(o.cell.held) + \"|\" + to_string(o.n) + \"|\" + to_string(o.f);"));
			std::string rows;
			for (const auto& row : e->hot_reload_reports()) {
				if (row.outcome == field_migration_outcome::reset) { rows += row.class_name + "." + row.field_name + ";"; }
			}
			check_eq(std::string("Outer.cell;Outer.n;"), rows, "the values the load did not keep");
		});

		test("a_load_inside_a_typed_load_follows_its_own_rule", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			e->execute("class Inner { int n = 3; } class Config { int level = 1; }"
			           " class Holder { int seen = 0; Inner inner; Holder() { var c = from_json(\"{\\\"$type\\\":\\\"Config\\\",\\\"$val\\\":{\\\"level\\\":\\\"high\\\"}}\"); seen = c.level; } }"
			           " class Strict { int seen = 0; Strict() { try { from_json<Config>(\"{\\\"$type\\\":\\\"Config\\\",\\\"$val\\\":{\\\"level\\\":\\\"high\\\"}}\"); } catch (e) { seen = 1; } } }");
			check_eq(std::string("1|8"), outcome(*e, "auto h = from_json<Holder>(\"{\\\"$type\\\":\\\"Holder\\\",\\\"$val\\\":{\\\"inner\\\":{\\\"n\\\":8}}}\"); to_string(h.seen) + \"|\" + to_string(h.inner.n);"),
			         "an untyped load a constructor runs during a typed load keeps the archive's rule");
			check_eq(std::string("ERR: from_json: field 'n' of 'Inner' is int, not string"),
			         outcome(*e, "to_json(from_json<Holder>(\"{\\\"$type\\\":\\\"Holder\\\",\\\"$val\\\":{\\\"inner\\\":{\\\"$type\\\":\\\"Inner\\\",\\\"$val\\\":{\\\"n\\\":\\\"x\\\"}}}}\"));"),
			         "and the typed load's rule holds again after it");
			check_eq(std::string("1|3"), outcome(*e, "var s = from_json(\"{\\\"$type\\\":\\\"Strict\\\",\\\"$val\\\":{\\\"seen\\\":\\\"x\\\"}}\"); var i = from_json(\"{\\\"$type\\\":\\\"Inner\\\",\\\"$val\\\":{\\\"n\\\":\\\"x\\\"}}\"); to_string(s.seen) + \"|\" + to_string(i.n);"),
			         "a typed load a constructor runs during an untyped load does not make the untyped one strict");
		});

		test("a_record_a_host_builds_refuses_the_same_members", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			engine* raw = e.get();
			e->add_function("row", [raw](const script_type& type, const std::string& column) {
				auto made = make_record(*raw, type, record_field_list{{"cell", script_value(column, raw)}}, "row");
				if (!made) { throw std::runtime_error(raw->format_error(made)); }
				return std::move(made.value());
			}, returns<type_arg>);
			e->execute("class Cell<T> { T held; } class Outer { Cell<Cell<int>> cell; } Cell<int> ci; Cell<string> cs; Cell<Cell<int>> cc;");
			const std::string wrong = "{\"$type\":\"Cell<Cell<int>>\",\"$val\":{\"held\":{\"$type\":\"Cell<string>\",\"$val\":{\"held\":\"x\"}}}}";
			check_eq(std::string("ERR: row: field 'held' of 'Cell<Cell<int>>' is Cell<int>, not Cell<string>"), outcome(*e, "to_json(row<Outer>(" + quoted(wrong) + "));"));
			check_eq(std::string("4"), outcome(*e, "row<Outer>(" + quoted("{\"$type\":\"Cell<Cell<int>>\",\"$val\":{\"held\":{\"held\":4}}}") + ").cell.held.held;"));
			check_eq(std::string("ERR: row: text for 'Cell<Cell<int>>' is not JSON: Expected '\"' in JSON"), outcome(*e, "to_json(row<Outer>(\"{\"));"), "text that is no JSON keeps its own error");
		});

		test("a_binary_archive_under_a_typed_load_refuses_the_same_members", [this]() {
			// what another version of the program saved: each class held a Cell<string> there
			auto writer = make_engine();
			stdlib::register_all(*writer);
			writer->execute("class Cell<T> { T held; } class Field { Cell<string> cell; } class Items { array<Cell<string>> items; } class Keyed { map<string, Cell<string>> keyed; }"
			                " class Handle { shared_ptr<Cell<string>> h; } class Fits { Cell<int> cell; }"
			                " Field field; field.cell.held = \"x\"; Items items; items.items.push(Cell<string>()); Keyed keyed; keyed.keyed[\"k\"] = Cell<string>();"
			                " Handle handle; handle.h = new Cell<string>(); Fits fits; fits.cell.held = 6;");
			auto e = make_engine();
			stdlib::register_all(*e);
			std::string payload;
			e->add_function("payload", [&payload]() { return payload; });
			e->execute("class Cell<T> { T held; } class Field { Cell<int> cell; } class Items { array<Cell<int>> items; } class Keyed { map<string, Cell<int>> keyed; }"
			           " class Handle { shared_ptr<Cell<int>> h; } class Fits { Cell<int> cell; } Cell<string> cs;");
			// from_binary<T> through the script for the typed load, from_binary for the archive's own rule
			const auto load = [&](const char* name, const char* cls, std::string_view who) -> std::string {
				payload = writer->execute(std::string("to_binary(") + name + ");").as_string();
				return outcome(*e, who.empty() ? std::string("from_binary(payload()); \"loaded\";") : "from_binary<" + std::string(cls) + ">(payload()); \"loaded\";");
			};
			check_eq(std::string("ERR: from_binary: field 'cell' of 'Field' is Cell<int>, not Cell<string>"), load("field", "Field", "from_binary"));
			check_eq(std::string("ERR: from_binary: field 'items' of 'Items' is array<Cell<int>>, not array"), load("items", "Items", "from_binary"));
			check_eq(std::string("ERR: from_binary: field 'keyed' of 'Keyed' is map<string,Cell<int>>, not map"), load("keyed", "Keyed", "from_binary"));
			check_eq(std::string("ERR: from_binary: field 'h' of 'Handle' is shared_ptr<Cell<int>>, not Cell<string>"), load("handle", "Handle", "from_binary"));
			check_eq(std::string("loaded"), load("fits", "Fits", "from_binary"), "a record whose members fit");
			check_eq(std::string("loaded"), load("field", "Field", {}), "the archive's own rule keeps the constructed value");
		});

		test("every_route_and_pair_of_classes_loads_whole_or_not_at_all", [this]() {
			// What one version of a program saved, loaded where the field is declared with each class in turn: a field, an array's
			// element, a map's value and a handle, as JSON and as binary, by a typed load and by an untyped one. A typed load
			// answers the saved data whole or refuses it; an untyped load answers it whole or keeps the constructed value. No load
			// answers an object of a class the field does not hold, and none drops a value it could have kept.
			const std::string classes = "class Cell<T> { T held; } class Other { string o = \"o\"; } class Base { int b = 1; } class Derived : Base { int d = 1; }"
			                            " Cell<int> made_i; Cell<string> made_s; Cell<Cell<int>> made_c;";
			const std::vector<std::pair<std::string, std::string>> types = {   // a class and how the saved object differs from a constructed one
				{"Cell<int>", "x.held = 5;"}, {"Cell<string>", "x.held = \"s\";"}, {"Cell<Cell<int>>", "x.held.held = 6;"},
				{"Other", "x.o = \"p\";"}, {"Base", "x.b = 2;"}, {"Derived", "x.b = 2; x.d = 3;"},
			};
			struct route {
				std::string name;
				std::string declared;   // %T: the class
				std::string fill;       // with `x` made
				bool handle;
			};
			const std::vector<route> routes = {
				{"a field", "%T v;", "h.v = x;", false}, {"an element", "array<%T> v;", "h.v.push(x);", false},
				{"a map's value", "map<string, %T> v;", "h.v[\"k\"] = x;", false}, {"a handle", "shared_ptr<%T> v;", "h.v = x;", true},
			};
			const auto with = [](std::string text, const std::string& type) {
				text.replace(text.find("%T"), 2, type);
				return text;
			};
			const auto fits = [](const std::string& saved, const std::string& declared) { return saved == declared || (saved == "Derived" && declared == "Base"); };
			const auto outcome = [](engine& e, const std::string& code) {
				try {
					return e.execute(code).to_string();
				} catch (const std::exception& error) {
					return std::string("ERR: ") + error.what();
				}
			};
			size_t loads = 0;
			for (const route& r : routes) {
				std::vector<std::string> json(types.size()), binary(types.size());
				for (size_t saved = 0; saved < types.size(); ++saved) {
					auto writer = make_engine();
					stdlib::register_all(*writer);
					writer->execute(classes + " class Holder { " + with(r.declared, types[saved].first) + " int mark = 0; } Holder h; h.mark = 7; " +
					                (r.handle ? "auto x = new " + types[saved].first + "(); " : types[saved].first + " x; ") + types[saved].second + " " + r.fill);
					json[saved] = writer->execute("to_json(h);").as_string();
					binary[saved] = writer->execute("to_binary(h);").as_string();
				}
				for (size_t declared = 0; declared < types.size(); ++declared) {
					auto e = make_engine();
					stdlib::register_all(*e);
					std::string payload;
					e->add_function("payload", [&payload]() { return payload; });
					e->execute(classes + " class Holder { " + with(r.declared, types[declared].first) + " int mark = 0; }");
					const std::string kept = e->execute("Holder k; k.mark = 7; to_json(k);").as_string();   // the saved mark over a constructed v
					for (size_t saved = 0; saved < types.size(); ++saved) {
						const std::string what = r.name + " declared " + types[declared].first + ", saved " + types[saved].first;
						const bool whole = fits(types[saved].first, types[declared].first);
						// JSON, typed and untyped
						payload = json[saved];
						const std::string typed_json = outcome(*e, "to_json(from_json<Holder>(payload()));");
						const std::string untyped_json = outcome(*e, "to_json(from_json(payload()));");
						// binary, untyped and typed, through the script
						payload = binary[saved];
						const std::string untyped_binary = outcome(*e, "to_json(from_binary(payload()));");
						const std::string typed_binary = outcome(*e, "to_json(from_binary<Holder>(payload()));");
						loads += 4;
						if (whole) {
							check_eq(json[saved], typed_json, what + ": a typed JSON load answers the save whole");
							check_eq(json[saved], untyped_json, what + ": an untyped JSON load answers the save whole");
							check_eq(json[saved], typed_binary, what + ": a typed binary load answers the save whole");
							check_eq(json[saved], untyped_binary, what + ": an untyped binary load answers the save whole");
						} else {
							check_true(typed_json.rfind("ERR: from_json: field 'v' of 'Holder' is ", 0) == 0, what + ": a typed JSON load refuses it: " + typed_json);
							check_true(typed_binary.rfind("ERR: from_binary: field 'v' of 'Holder' is ", 0) == 0, what + ": a typed binary load refuses it: " + typed_binary);
							check_eq(kept, untyped_json, what + ": an untyped JSON load keeps the constructed value");
							check_eq(kept, untyped_binary, what + ": an untyped binary load keeps the constructed value");
						}
					}
				}
			}
			check_eq(size_t(576), loads, "every pair on every route in both formats by both loads");
		});

		test("a_weak_fields_ref_to_another_class_is_refused_by_a_typed_load_and_kept_out_by_an_untyped_one", [this]() {
			// Where the weak_ptr<T> slot rule meets the load rule: a $ref that names an object of another class. A typed load
			// refuses it whether or not the holder is written with a $type, with the text every member its field cannot take
			// gets; an untyped load keeps the field's constructed value (a weak field is constructed null: lock() on it raises,
			// where a $ref to no object loads an empty pointer), as for any saved value the field cannot take.
			auto e = make_engine();
			stdlib::register_all(*e);
			e->execute("class Kin { int id = 1; } class Heir : Kin { int extra = 5; } class Stranger { int n = 2; }"
			           " class Keeper { shared_ptr<Kin> own; shared_ptr<Stranger> other; shared_ptr<Heir> kid; weak_ptr<Kin> w; }"
			           " string seen(Keeper k) { try { auto reached = k.w.lock();"
			           " return reached == null ? \"empty\" : reflect::type_of(reached) + \" \" + to_string(reached.id); } catch (e) { return \"null\"; } }");
			const auto members = [](int target) {
				return std::string("{\"own\":{\"$id\":1,\"$type\":\"Kin\",\"$val\":{\"id\":1}},\"other\":{\"$id\":2,\"$type\":\"Stranger\",\"$val\":{\"n\":2}},"
				                   "\"kid\":{\"$id\":3,\"$type\":\"Heir\",\"$val\":{\"id\":7,\"extra\":5}},\"w\":{\"$ref\":") + std::to_string(target) + "}}";
			};
			const std::string refusal = "from_json: field 'w' of 'Keeper' is weak_ptr<Kin>, not weak_ptr<Stranger>";
			struct row {
				const char* what;
				int target;
				std::string typed;     // what a typed load answers, tagged or not
				std::string untyped;   // what an untyped load of the tagged form answers
			};
			const std::vector<row> rows = {
				{"its own class", 1, "Kin 1", "Kin 1"},
				{"a derived class", 3, "Heir 7", "Heir 7"},
				{"another class", 2, "ERR: " + refusal, "null"},
				{"no object", 9, "empty", "empty"},
			};
			for (const auto& r : rows) {
				const std::string plain = members(r.target);
				const std::string tagged = "{\"$type\":\"Keeper\",\"$val\":" + plain + "}";
				check_eq(r.typed, outcome(*e, "seen(from_json<Keeper>(" + quoted(tagged) + "));"), std::string("typed, tagged, a $ref to ") + r.what);
				check_eq(r.typed, outcome(*e, "seen(from_json<Keeper>(" + quoted(plain) + "));"), std::string("typed, untagged, a $ref to ") + r.what);
				check_eq(r.untyped, outcome(*e, "Keeper k = from_json(" + quoted(tagged) + "); seen(k);"), std::string("untyped, a $ref to ") + r.what);
			}
			// What the refusal calls its source: a weak reference is the weak_ptr of the class it reaches, wherever it stands,
			// and a handle is its class, tagged or not
			const std::string stranger = "\"other\":{\"$id\":1,\"$type\":\"Stranger\",\"$val\":{\"n\":2}}";
			const std::vector<std::pair<std::string, std::string>> named = {
				{"{" + stranger + ",\"own\":{\"$ref\":1}}", "from_json: field 'own' of 'Keeper' is shared_ptr<Kin>, not weak_ptr<Stranger>"},
				{"{" + stranger + ",\"own\":{\"$id\":1}}", "from_json: field 'own' of 'Keeper' is shared_ptr<Kin>, not Stranger"},
				{"{\"own\":{\"$id\":1,\"$type\":\"Stranger\",\"$val\":{\"n\":2}}}", "from_json: field 'own' of 'Keeper' is shared_ptr<Kin>, not Stranger"},
				{"{\"w\":{\"$id\":2,\"$type\":\"Stranger\",\"$val\":{\"n\":2}}}", "from_json: field 'w' of 'Keeper' is weak_ptr<Kin>, not Stranger"},
			};
			for (const auto& [plain, says] : named) {
				check_eq("ERR: " + says, outcome(*e, "from_json<Keeper>(" + quoted(plain) + ");"), "untagged: " + plain);
				check_eq("ERR: " + says, outcome(*e, "from_json<Keeper>(" + quoted("{\"$type\":\"Keeper\",\"$val\":" + plain + "}") + ");"), "tagged: " + plain);
			}
			// the rest of a refused document is not kept either: the typed load raises, it does not answer a holder without w
			check_true(outcome(*e, "from_json<Keeper>(" + quoted(members(2)) + ").own.id;").rfind("ERR: ", 0) == 0);
		});

		test("a_load_that_code_run_by_a_typed_load_starts_follows_its_own_rule", [this]() {
			// The typed rule is the typed load's own. Code that load runs (a constructor, a post_load hook) is not its data: an
			// archive a host function reads for that code starts from the archive's rule, where it inherited the typed one and
			// refused, in the typed load's name, a value its own load keeps the constructed value for.
			auto e = make_engine();
			stdlib::register_all(*e);
			const std::string saved = "{\"$type\":\"Config\",\"$val\":{\"level\":\"high\",\"name\":\"kept\"}}";   // level no longer fits its field
			e->add_function("host_config", [raw = e.get(), saved]() -> script_value {
				serialization::json_archive_reader reader(saved, raw);
				return stdlib::load_archive_value(raw, reader.read_value(), true);
			});
			e->execute("class Config { int level = 1; string name = \"made\"; } class Inner { int n = 3; }"
			           " class Built { string seen = \"\"; Inner inner; Built() { var c = host_config(); seen = to_string(c.level) + c.name; } }"
			           " class Hooked { string seen = \"\"; int n = 0; void post_load(int version) { var c = host_config(); seen = to_string(c.level) + c.name; } }");
			check_eq(std::string("1kept"), outcome(*e, "var c = host_config(); to_string(c.level) + c.name;"), "the host's read alone: the archive's rule");
			const std::string built = "{\"inner\":{\"n\":8}}";
			check_eq(std::string("1kept|8"), outcome(*e, "auto b = from_json<Built>(" + quoted(built) + "); b.seen + \"|\" + to_string(b.inner.n);"),
			         "a constructor a typed load of an untagged record runs");
			check_eq(std::string("1kept|8"), outcome(*e, "auto b = from_json<Built>(" + quoted("{\"$type\":\"Built\",\"$val\":" + built + "}") + "); b.seen + \"|\" + to_string(b.inner.n);"),
			         "a constructor a typed load of a tagged record runs");
			check_eq(std::string("1kept|4"), outcome(*e, "auto h = from_json<Hooked>(" + quoted("{\"$type\":\"Hooked\",\"$val\":{\"n\":4}}") + "); h.seen + \"|\" + to_string(h.n);"),
			         "a post_load hook a typed load runs");
			check_eq(std::string("1kept|8"), outcome(*e, "Built made; made.inner.n = 8; auto b = from_binary<Built>(to_binary(made)); b.seen + \"|\" + to_string(b.inner.n);"),
			         "and through a typed binary load");
			// the typed load's rule holds for its own data, before and after the code it runs
			check_eq(std::string("ERR: from_json: field 'n' of 'Inner' is int, not string"),
			         outcome(*e, "from_json<Built>(" + quoted("{\"$type\":\"Built\",\"$val\":{\"inner\":{\"$type\":\"Inner\",\"$val\":{\"n\":\"x\"}}}}") + ");"));
			check_eq(std::string("ERR: from_json: field 'n' of 'Hooked' is int, not string"),
			         outcome(*e, "from_json<Hooked>(" + quoted("{\"$type\":\"Hooked\",\"$val\":{\"n\":\"x\"}}") + ");"));
		});
	}
};

}

using typed_load_tests = jai::foundry::tests::typed_load_tests;
FOUNDRY_REGISTER(typed_load_tests)
