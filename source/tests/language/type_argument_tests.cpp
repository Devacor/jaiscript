// Type arguments (docs/site/guide/06-functions.html "Type arguments") and records (docs/site/embedding/17-host-api.html
// "Records"): callee<T>(args) passes T as a reflect::type value only a host takes, a host registration types the result in
// terms of T, and jai::make_record / record_fields build and read script class instances from named values
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>

namespace jai::foundry::tests {

namespace type_argument_probe {
	struct store {
		std::string last;
	};
}

class type_argument_tests : public suite {
public:
	type_argument_tests() : suite("Type Arguments") {}

	static std::string failure(engine& eng, const std::string& code) {
		try {
			eng.execute(code);
		} catch (const std::exception& e) {
			return e.what();
		}
		return "(no error)";
	}

	static bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

	// the host surface the tests call: a free function, a typed-result family and a bound class
	static void bind_probe(engine& eng) {
		stdlib::register_all(eng);
		engine* e = &eng;
		eng.add_function("type_name_of", [](const script_type& t) { return t.name(); });
		eng.add_function("type_and_value", [](const script_type& t, script_int n) { return t.name() + ":" + std::to_string(n); });
		eng.add_function("two_types", [](const script_type& a, const script_type& b) { return a.name() + "," + b.name(); });
		eng.add_function("class_of", [](const script_type& t) { return t.class_name(); });
		eng.add_function("one_record", [e](const script_type& t, const std::string& handle) {
			auto made = make_record(*e, t, record_field_list{{"handle", script_value(handle, e)}}, "one_record");
			if (!made) { throw std::runtime_error(e->format_error(made)); }
			return std::move(made.value());
		}, returns<type_arg>);
		eng.add_function("some_records", [e](const script_type& t) {
			script_value rows = script_value::make_array(t.info(), e);
			for (const char* handle : {"rex", "kit"}) {
				auto made = make_record(*e, t, record_field_list{{"handle", script_value(std::string(handle), e)}}, "some_records");
				if (!made) { throw std::runtime_error(e->format_error(made)); }
				rows.unchecked_get_array_storage()->push(std::move(made.value()));
			}
			return rows;
		}, returns<array_of<type_arg>>);
		eng.add_function("first_record", [e](const script_type& t) {
			auto made = make_shared_record(*e, t, record_field_list{{"handle", script_value(std::string("rex"), e)}}, "first_record");
			if (!made) { throw std::runtime_error(e->format_error(made)); }
			return std::move(made.value());
		}, returns<shared_ptr_of<type_arg>>);
		dynamic_binder<type_argument_probe::store>(eng, "ProbeStore")
			.constructor<>()
			.method("fetch", [e](type_argument_probe::store& self, const script_type& t, const std::string& sql) {
				self.last = t.name() + " " + sql;
				return e->constant_argument_admits("ProbeStore.fetch", 0, sql) ? self.last : std::string("refused");
			})
			.method("last", [](const type_argument_probe::store& self) { return self.last; })
			.method("rows", [e](type_argument_probe::store&, const script_type& t) {
				script_value rows = script_value::make_array(t.info(), e);
				auto made = make_record(*e, t, record_field_list{{"handle", script_value(std::string("kit"), e)}}, "ProbeStore.rows");
				if (!made) { throw std::runtime_error(e->format_error(made)); }
				rows.unchecked_get_array_storage()->push(std::move(made.value()));
				return rows;
			}, returns<array_of<type_arg>>)
			.build();
		eng.constant_argument("ProbeStore.fetch", 0, "a constant, please");
	}

	void forge_tests() override {
		test("a_type_argument_reaches_a_host_function_as_a_script_type", [this]() {
			auto e = make_engine();
			bind_probe(*e);
			e->execute("class User { int id; } namespace shop { class Product { string title; } }");
			check_eq(std::string("User"), e->execute("type_name_of<User>()").as_string());
			check_eq(std::string("array<User>"), e->execute("type_name_of<array<User>>()").as_string());
			check_eq(std::string("shared_ptr<shop::Product>"), e->execute("type_name_of<shared_ptr<shop::Product>>()").as_string());
			check_eq(std::string("map<string,array<int>>"), e->execute("type_name_of<map<string, array<int>>>()").as_string());
			check_eq(std::string("int:4"), e->execute("type_and_value<int>(4)").as_string());
			check_eq(std::string("User,float"), e->execute("two_types<User, float>()").as_string());
			check_eq(std::string("shop::Product"), e->execute("class_of<shared_ptr<shop::Product>>()").as_string());
			check_eq(std::string(""), e->execute("class_of<array<User>>()").as_string());
		});

		test("the_type_value_is_reflect_type_with_name_and_to_string", [this]() {
			auto e = make_engine();
			e->add_function("pass_type", [](const script_value& v) { return v; });
			check_eq(std::string("array<int> array<int>"), e->execute("var t = pass_type<array<int>>(); t.name() + \" \" + t.to_string();").as_string());
		});

		test("only_a_less_than_that_closes_before_a_paren_opens_type_arguments", [this]() {
			auto e = make_engine();
			bind_probe(*e);
			check_true(e->execute("int a = 1; int b = 2; a < b;").as_bool());
			check_true(e->execute("int a = 1; int b = 2; int c = 0; a < b && b > (c);").as_bool());
			check_false(e->execute("int a = 1; int b = 2; int c = 3; a < b >> (c);").as_bool());
			check_eq(std::string("array<array<int>>"), e->execute("type_name_of<array<array<int>>>()").as_string());
		});

		test("a_locals_less_than_always_compares", [this]() {
			// a local variable or a parameter is never generic and never a host function: '<' after one compares, whatever follows
			auto e = make_engine();
			e->execute("int two(bool x, bool y) { return (x ? 1 : 0) + (y ? 10 : 0); }");
			check_eq((int64_t)11, e->execute("int f() { int a = 1; int b = 2; int c = 3; int d = 2; return two(a < b, c > (d)); } f();").as_int());
			check_eq((int64_t)11, e->execute("int f(int a, int b, int c, int d) { return two(a < b, c > (d)); } f(1, 2, 3, 2);").as_int());
			check_eq((int64_t)11, e->execute("int f() { int a = 1; int b = 2; int c = 3; int d = 2; auto h = () => { auto k = () => two(a < b, c > (d)); return k(); }; return h(); } f();").as_int());
			check_true(e->execute("bool f(int a, int b) { return (a < b) == (b > (a)); } f(1, 2);").as_bool());
		});

		test("less_than_after_a_variable_the_unit_declared_above_compares", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			check_eq(std::string("2"), e->execute("int two(bool a, bool b) { return 2; }\nint a = 1; int b = 2; int c = 3; int d = 0;\nto_string(two(a < b, c > (d)));").as_string());
			e->execute("int pair(bool x, bool y) { return (x ? 1 : 0) + (y ? 10 : 0); }");
			check_eq((int64_t)11, e->execute("int p = 1; int q = 2; int r = 3; int s = 2; pair(p < q, r > (s));").as_int());
			check_eq((int64_t)11, e->execute("auto u = 1; var v = 2; float w = 3.0; int x = 2; pair(u < v, w > (x));").as_int());
			check_true(e->execute("int m = 1; int n = 2; bool both = (m < n) == (n > (m)); both;").as_bool());
			// inside a function of the unit, the variable declared above it is known too
			check_eq((int64_t)11, e->execute("int g1 = 1; int g2 = 2; int g3 = 3; int g4 = 2; int f() { return pair(g1 < g2, g3 > (g4)); } f();").as_int());
			// a run holding a type's keyword is still read as type arguments, as after a local
			check_true(contains(failure(*e, "int l = 1; l<int>(2);"), "'l' is an int (declared at 1:5), not a function"));
			// a variable declared below the use is a variable all the same
			check_eq((int64_t)11, e->execute("int h() { return pair(z1 < z2, z3 > (z4)); } int z1 = 1; int z2 = 2; int z3 = 3; int z4 = 2; h();").as_int());
		});

		test("a_comparison_read_as_type_arguments_says_what_to_write", [this]() {
			auto e = make_engine();
			bind_probe(*e);
			e->execute("int two(bool x, bool y) { return (x ? 1 : 0) + (y ? 10 : 0); }");
			// another unit's variables compare, as the unit's own do
			e->execute("int a = 1; int b = 2; int c = 3; int d = 2;");
			check_eq((int64_t)11, e->execute("two(a < b, c > (d));").as_int());
			check_eq((int64_t)11, e->execute("two((a < b), c > (d));").as_int());
			// after a member the member's name decides: a field compares, unless some class declares a generic method of its name.
			// There '<' opens type arguments; the field takes none and a type argument names no type, so a comparison was most
			// likely meant, and the call's one error says how to write it
			check_eq((int64_t)11, e->execute("class P { int n = 1; } P p; two(p.n < b, c > (d));").as_int());
			const std::string member = failure(*e, "class Q { T n<T>(T x) { return x; } } class P2 { int n = 1; } P2 p2; two(p2.n < b, c > (d));");
			check_true(contains(member, "unknown type 'b': 'p2.n<b, c>(...)' reads as a call with type arguments, which 'p2.n' does not take; if this is a comparison, write (p2.n < b)"), member);
			check_false(contains(member, "unknown type 'c'"), "the call's one error");
			// a callee that takes type arguments keeps the declaration's error
			const std::string host = failure(*e, "type_name_of<Nope>();");
			check_true(contains(host, "unknown type 'Nope'") && !contains(host, "comparison"), host);
			const std::string generic = failure(*e, "T same<T>(T x) { return x; } same<Nope>(1);");
			check_true(contains(generic, "unknown type 'Nope'") && !contains(generic, "comparison"), generic);
		});

		test("a_script_callee_takes_no_type_arguments", [this]() {
			auto e = make_engine();
			const std::pair<const char*, const char*> cases[] = {
				{"int f(int x) { return x; } f<int>(1);", "'f' takes no type arguments: it is not a generic function"},
				{"namespace ns { int f(int x) { return x; } } ns::f<int>(1);", "'ns::f' takes no type arguments: it is not a generic function"},
				{"class C { int m() { return 1; } } C c; c.m<int>();", "'m' takes no type arguments: it is not a generic method"},
				{"class C { int m() { return 1; } int u() { return m<int>(); } }", "'m' takes no type arguments: it is not a generic method"},
				{"class C { static int s() { return 1; } } C::s<int>();", "'s' takes no type arguments: it is not a generic method"},
				{"class Hero { int n = 1; } Hero<int>();", "'Hero' takes no type arguments: it is not a generic class"},
				{"void g() { auto l = [](int x) { return x; }; l<int>(2); }", "'l' takes no type arguments: it is a lambda, and a lambda takes no type parameters"}};
			for (const auto& [code, text] : cases) {
				const std::string got = failure(*e, code);
				check_true(contains(got, text), std::string(code) + " -> " + got);
			}
		});

		test("type_arguments_on_a_type_that_takes_none_are_one_error_wherever_a_type_stands", [this]() {
			struct kind {
				const char* declares;   // what the unit declares first
				const char* name;
				const char* text;
			};
			const kind kinds[] = {
				{"class Hero { } ", "Hero", "'Hero' takes no type arguments: it is not a generic class"},
				{"namespace n { class Hero { } } ", "n::Hero", "'n::Hero' takes no type arguments: it is not a generic class"},
				{"", "time::date", "'time::date' takes no type arguments: it is a host class, not a host template"},
				{"", "regex", "'regex' takes no type arguments: it is a host class, not a host template"},
				{"enum Color { red, green } ", "Color", "'Color' takes no type arguments: it is an enum"},
				{"", "int", "'int' takes no type arguments: it is a builtin type"},
				{"", "string", "'string' takes no type arguments: it is a builtin type"},
				{"", "math::vec2", "'math::vec2' takes no type arguments: it is a builtin type"},
				{"", "utf8_string", "'utf8_string' takes no type arguments: it is a builtin type"},
			};
			const char* positions[] = {
				"NAME<int> bad;",                                  // a declaration
				"class Holder { NAME<int> field; }",               // a field
				"void take(NAME<int> p) { }",                      // a parameter
				"NAME<int> make() { NAME<int> x; return x; }",     // a return type
				"array<NAME<int>> many;",
				"map<string, NAME<int>> keyed;",
				"shared_ptr<NAME<int>> held;",
				"auto made = NAME<int>();",                        // Name<Args>(...) in an expression
			};
			for (const kind& k : kinds) {
				for (const char* position : positions) {
					auto e = make_engine();
					bind_probe(*e);
					std::string code = position;
					for (size_t at = code.find("NAME"); at != std::string::npos; at = code.find("NAME", at)) { code.replace(at, 4, k.name); }
					code = k.declares + code;
					const std::string text = failure(*e, code);
					const size_t first = text.find(k.text);
					check_true(first != std::string::npos, code + " => " + text);
					check_true(first == std::string::npos || text.find("takes no type arguments", first + std::string(k.text).size()) == std::string::npos, "once: " + code + " => " + text);
					check_false(contains(text, "expected"), "no parse error: " + code + " => " + text);
				}
			}
			// a host template the host did not bind with that spelling is its own case, and a generic's instance is none of this
			auto e = make_engine();
			bind_probe(*e);
			check_eq((int64_t)3, e->execute("class Box<T> { T v; } Box<int> b; b.v = 3; b.v;").as_int());
			check_true(contains(failure(*e, "int f(int x) { return x; } f<int>(1);"), "'f' takes no type arguments: it is not a generic function"));
		});

		test("an_unknown_type_argument_is_the_declarations_error", [this]() {
			auto e = make_engine();
			bind_probe(*e);
			check_true(contains(failure(*e, "type_name_of<Nope>();"), "unknown type 'Nope'"));
		});

		test("a_registration_types_the_result_in_terms_of_the_type_argument", [this]() {
			auto e = make_engine();
			bind_probe(*e);
			e->execute("class User { int id = 3; string handle; }");
			check_eq(std::string("rex3"), e->execute("auto u = one_record<User>(\"rex\"); u.handle + to_string(u.id);").as_string());
			check_eq(std::string("kit"), e->execute("auto all = some_records<User>(); all[1].handle;").as_string());
			check_eq(std::string("rex"), e->execute("auto one = first_record<User>(); one->handle;").as_string());
			check_eq(std::string("rex"), e->execute("shared_ptr<User> h = first_record<User>(); h->handle;").as_string());
			check_eq(2, static_cast<int>(e->type_argument_result("ProbeStore.rows")));
			check_eq(3, static_cast<int>(e->type_argument_result("first_record")));
			check_eq(0, static_cast<int>(e->type_argument_result("type_name_of")));
			check_eq(std::string("kit"), e->execute("ProbeStore s = ProbeStore(); auto rows = s.rows<User>(); rows[0].handle;").as_string());
			for (const char* code : {"auto u = one_record<Member>(\"rex\"); u.Handel;", "auto all = some_records<Member>(); all[0].Handel;",
			                         "auto one = first_record<Member>(); one->Handel;", "ProbeStore s = ProbeStore(); auto rows = s.rows<Member>(); rows[0].Handel;"}) {
				const std::string unit = std::string("class Member { string handle; } ") + code;
				check_true(contains(e->check(unit).format(), "no member 'Handel' in class 'Member'"), code);
			}
		});

		test("a_constant_argument_index_counts_the_value_arguments", [this]() {
			auto e = make_engine();
			bind_probe(*e);
			e->execute("class User { int id; }");
			check_eq(std::string("User SELECT 1"), e->execute("ProbeStore s = ProbeStore(); s.fetch<User>(\"SELECT 1\");").as_string());
			check_true(contains(e->check("ProbeStore s = ProbeStore(); string t = \"x\"; s.fetch<User>(t);").format(),
				"argument 1 of 'fetch' is not a constant: a constant, please"));
		});

		test("type_arguments_survive_a_jaibite_round_trip", [this]() {
			auto e = make_engine();
			bind_probe(*e);
			jai::jaibite bite = e->jaibite("class User { string handle; } auto all = some_records<User>(); type_name_of<array<User>>() + all[1].handle;");
			const std::vector<uint8_t> bytes = bite.save_bytes();
			auto loaded = make_engine();
			bind_probe(*loaded);
			jai::jaibite again = loaded->jaibite_load_bytes(bytes);
			check_eq(std::string("array<User>kit"), std::string(loaded->execute(again).as_string()));
		});

		test("make_record_builds_by_the_store_rules_and_widens_what_a_row_holds", [this]() {
			auto e = make_engine();
			bind_records(*e);
			e->execute("class Item { string name; int qty = 1; } "
			           "class Base { int id; } "
			           "class Cart : Base { bool open; float total; array<Item> items; map<string, int> notes; Item best; shared_ptr<Item> spare; "
			           "string kept = \"init\"; }");
			e->execute("Cart c = record_from<Cart>({\"id\": 7, \"open\": 1, \"total\": 3, \"items\": \"[{\\\"name\\\": \\\"pin\\\"}, {\\\"name\\\": \\\"hat\\\", \\\"qty\\\": 2}]\", "
			           "\"notes\": \"{\\\"a\\\": 1}\", \"best\": {\"name\": \"mask\"}, \"spare\": {\"name\": \"tail\"}, \"unknown\": 5});");
			check_eq(std::string("7 true 3.0 2 hat 2 1 1 mask tail init"), e->execute(
				"to_string(c.id) + \" \" + to_string(c.open) + \" \" + to_string(c.total) + \" \" + to_string(c.items.size()) + \" \" + c.items[1].name + "
				"\" \" + to_string(c.items[1].qty) + \" \" + to_string(c.items[0].qty) + \" \" + to_string(c.notes[\"a\"]) + \" \" + c.best.name + \" \" + "
				"c.spare->name + \" \" + c.kept;").as_string());
			check_eq(std::string("rex"), e->execute("auto h = shared_record_from<Item>({\"name\": \"rex\"}); h->name;").as_string());
		});

		test("make_record_refuses_with_the_class_the_field_and_both_types", [this]() {
			auto e = make_engine();
			bind_records(*e);
			e->execute("class User { int id; string handle; } class Needs { int id; Needs(int i) { id = i; } }");
			check_eq(std::string("record_from: field 'id' of 'User' is int, not string"), failure(*e, "record_from<User>({\"id\": \"one\"});"));
			check_eq(std::string("record_from: field 'id' of 'User' is int and cannot be null"), failure(*e, "record_from<User>({\"id\": null});"));
			check_eq(std::string("record_from: field 'id' of 'User' is int, not float"), failure(*e, "record_from<User>({\"id\": 1.5});"));
			check_eq(std::string("record_from: 'Needs' has no constructor taking no arguments"), failure(*e, "record_from<Needs>({\"id\": 1});"));
			check_eq(std::string("record_from: a record is an object of a script class, not array<User>"), failure(*e, "record_from<array<User>>({});"));
		});

		test("record_fields_reads_bases_first_and_stores_containers_as_json", [this]() {
			auto e = make_engine();
			bind_records(*e);
			e->execute("class Base { int id = 4; } class Tagged : Base { string name = \"rex\"; array<string> tags = [\"a\"]; bool on = true; }");
			check_eq(std::string("id=4;name=rex;tags=[\"a\"];on=true;"), e->execute("fields_text(Tagged());").as_string());
			check_eq(std::string("a=1;b=[1,2];"), e->execute("fields_text({\"b\": [1, 2], \"a\": 1});").as_string());
			check_eq(std::string("fields_text: a record is an object of a script class or a map, not int"), failure(*e, "fields_text(3);"));
		});

		test("from_json_with_a_type_argument_builds_that_type", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			e->execute("class Item { string name; int qty = 1; } class Order { int id; array<Item> items; shared_ptr<Item> first; }");
			check_eq(std::string("2 hat 1 pin"), e->execute("Order o = from_json<Order>(\"{\\\"id\\\": 2, \\\"items\\\": [{\\\"name\\\": \\\"hat\\\"}], "
				"\\\"first\\\": {\\\"name\\\": \\\"pin\\\"}}\"); to_string(o.id) + \" \" + o.items[0].name + \" \" + to_string(o.items[0].qty) + \" \" + o.first->name;").as_string());
			check_eq(std::string("b"), e->execute("auto all = from_json<array<Item>>(\"[{\\\"name\\\": \\\"a\\\"}, {\\\"name\\\": \\\"b\\\"}]\"); all[1].name;").as_string());
			check_eq(script_int(6), e->execute("from_json<int>(\"5\") + 1;").as_int());
			check_eq(std::string("from_json: the JSON is map<string,var>, not array<int>"), failure(*e, "from_json<array<int>>(\"{}\");"));
			check_true(contains(e->check("class Receipt { int id; } auto r = from_json<Receipt>(\"{}\"); r.Idd;").format(), "no member 'Idd' in class 'Receipt'"));
			check_eq(std::string("x"), e->execute("var plain = from_json(\"{\\\"a\\\": \\\"x\\\"}\"); plain[\"a\"];").as_string());
		});

		test("from_binary_with_a_type_argument_builds_that_type_as_from_json_does", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			e->execute("class Item { string name; int qty = 1; } class Order { int id; array<Item> items; shared_ptr<Item> first; }"
			           " Order made; made.id = 2; Item hat; hat.name = \"hat\"; made.items.push(hat); made.first = new Item(); made.first->name = \"pin\";");
			check_eq(std::string("2 hat 1 pin"), e->execute("Order o = from_binary<Order>(to_binary(made)); to_string(o.id) + \" \" + o.items[0].name + \" \" + to_string(o.items[0].qty) + \" \" + o.first->name;").as_string());
			// untagged data builds a class by the record rules, as from_json<T> of an object does
			check_eq(std::string("b 1"), e->execute("auto all = from_binary<array<Item>>(to_binary([{\"name\": \"a\"}, {\"name\": \"b\"}])); all[1].name + \" \" + to_string(all[1].qty);").as_string());
			check_eq(std::string("pin"), e->execute("shared_ptr<Item> h = from_binary<shared_ptr<Item>>(to_binary(made.first)); h->name;").as_string());
			check_eq(script_int(6), e->execute("from_binary<int>(to_binary(5)) + 1;").as_int());
			check_eq(std::string("b"), e->execute("map<int, string> m; m[3] = \"b\"; from_binary<map<int, string>>(to_binary(m))[3];").as_string());
			check_eq(std::string("hat"), e->execute("from_binary<Item>(base64_decode(base64_encode(to_binary(hat)))).name;").as_string());
			check_eq(std::string("from_binary: the data is map, not array<int>"), failure(*e, "from_binary<array<int>>(to_binary({}));"));
			check_eq(std::string("from_binary: field 'qty' of 'Item' is int, not string"), failure(*e, "from_binary<Item>(to_binary({\"qty\": \"x\"}));"));
			check_eq(std::string("from_json: field 'qty' of 'Item' is int, not string"), failure(*e, "from_json<Item>(to_json({\"qty\": \"x\"}));"));
			check_true(contains(e->check("class Receipt { int id; } auto r = from_binary<Receipt>(to_binary(1)); r.Idd;").format(), "no member 'Idd' in class 'Receipt'"));
			check_eq(std::string("x"), e->execute("var plain = from_binary(to_binary({\"a\": \"x\"})); plain[\"a\"];").as_string());
		});
	}

	static void bind_records(engine& eng) {
		stdlib::register_all(eng);
		engine* e = &eng;
		eng.add_function("record_from", [e](const script_type& t, const script_value& fields) {
			auto made = make_record(*e, t, fields, "record_from");
			if (!made) { throw std::runtime_error(e->format_error(made)); }
			return std::move(made.value());
		}, returns<type_arg>);
		eng.add_function("shared_record_from", [e](const script_type& t, const script_value& fields) {
			record_field_list list;
			for (const auto& [key, value] : fields.deref().as_map()) { list.emplace_back(key.as_string(), value); }
			auto made = make_shared_record(*e, t, list, "shared_record_from");
			if (!made) { throw std::runtime_error(e->format_error(made)); }
			return std::move(made.value());
		}, returns<shared_ptr_of<type_arg>>);
		eng.add_function("fields_text", [e](const script_value& record) {
			auto fields = record_fields(*e, record, "fields_text");
			if (!fields) { throw std::runtime_error(e->format_error(fields)); }
			std::string out;
			for (const auto& [name, value] : fields.value()) { out += name + "=" + value.to_string() + ";"; }
			return out;
		});
	}
};

} // namespace jai::foundry::tests

using type_argument_tests = jai::foundry::tests::type_argument_tests;
FOUNDRY_REGISTER(type_argument_tests)
