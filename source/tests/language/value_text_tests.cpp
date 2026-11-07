// A value's display text (guide 09 "Printing values"): the one rule print, to_string, string +, template splices, format's {},
// join, string slots and a throw's message share. Every test runs on whichever backend the runner selected.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/detail/value_text.hpp>
#include <jaiscript/properties/macros.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/properties/observable_property.hpp>

#include <memory>
#include <sstream>
#include <string>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

namespace value_text_fixtures {

// unbudgeted: the large-value test's loops outlast the 5 s default in a loaded Debug build
std::string run_output(const std::string& source, bool unbudgeted = false) {
	auto e = make_engine();
	stdlib::register_all(*e);
	if (unbudgeted) { e->execution_budget(0.0); }
	auto out = std::make_shared<std::ostringstream>();
	e->set_output_stream(out);
	try {
		e->execute(source);
	} catch (const std::exception& ex) {
		*out << "threw: " << ex.what();
	}
	return out->str();
}

struct Lamp {
	int64_t watts = 60;
	std::string to_string() const { return "lamp(" + std::to_string(watts) + ")"; }
};

struct Crate {
	int64_t items = 3;
};

class Gauge : public jai::property_owner<Gauge> {
public:
	JAI_OBSERVABLE_PROPERTY((int64_t), level, 7);
};

}   // namespace value_text_fixtures

class value_text_tests : public suite {
public:
	value_text_tests() : suite("Value Text") {}

	void forge_tests() override {
		using value_text_fixtures::run_output;

		test("scalars_print_raw_at_the_top_and_as_literals_inside", [this]() {
			check_eq(std::string(
				"a\"b c 2.0 true null\n"
				"[\"a\\\"b\", 'c', 2.0, true, null, 7]\n"
				"['\\'', '\\\\', '\\n', '\\x01', '\\xff', '\"']\n"
				"[\"tab\\there\", \"nul\\x00z\", \"\\x7f\", \"r\\r\", \"q'\", \"h\xC3\xA9\"]\n"
				"h\xC3\xA9 [\"\xC3\xBC\"] [(1.0, 2.0)] [(1, 2)]\n"), run_output(R"JAI(
				print("a\"b", " ", 'c', " ", 2.0, " ", true, " ", null);
				print(["a\"b", 'c', 2.0, true, null, 7]);
				print(['\'', '\\', '\n', '\x01', '\xff', '"']);
				print(["tab\there", "nul\x00z", "\x7f", "r\r", "q'", "h\xC3\xA9"]);
				print(utf8_string("h\xC3\xA9"), " ", [utf8_string("\xC3\xBC")], " ", [math::vec2(1, 2)], " ", [math::ivec2(1, 2)]);
			)JAI"));
		});

		test("arrays_and_maps_show_their_contents_in_order", [this]() {
			check_eq(std::string(
				"[1, 2, 3] [1.5, 2.0] [\"x\", \"y\"] [true, false] ['a'] [] {}\n"
				"{\"a\": [1, {\"b\": null}], \"z\": 0}\n"
				"{1: \"one\", 2: \"two\"} {[1, 2]: \"pair\"}\n"
				"[[1], [2, [3]]]\n"), run_output(R"JAI(
				array<int> ints = [1, 2, 3];
				array<float> floats = [1.5, 2.0];
				array<string> strs = ["x", "y"];
				array<bool> bools = [true, false];
				array<char> chars = ['a'];
				print(ints, " ", floats, " ", strs, " ", bools, " ", chars, " ", [], " ", {});
				print({"z": 0, "a": [1, {"b": null}]});
				var keyed = {};
				keyed[[1, 2]] = "pair";
				print({2: "two", 1: "one"}, " ", keyed);
				print([[1], [2, [3]]]);
			)JAI"));
		});

		test("instances_show_their_class_and_fields_base_first", [this]() {
			check_eq(std::string(
				"P{x: 1, n: \"hi\"}\n"
				"R{x: 1, n: \"hi\", f: 2.0, tags: [\"t\"], secret: 9}\n"
				"Empty{} [Empty{}]\n"
				"Outer{inner: P{x: 1, n: \"hi\"}, list: [P{x: 1, n: \"hi\"}]}\n"), run_output(R"JAI(
				class P { int x = 1; string n = "hi"; }
				class Q : P { float f = 2.0; }
				class R : Q { array<string> tags = ["t"]; private: int secret = 9; }
				class Empty {}
				class Outer { P inner; array<P> list = [P()]; }
				print(P());
				print(R());
				print(Empty(), " ", [Empty()]);
				print(Outer());
			)JAI"));
		});

		test("a_class_to_string_speaks_for_it_wherever_it_sits", [this]() {
			check_eq(std::string(
				"T3 [T3, T3] {\"k\": T3}\n"
				"D:T3\n"
				"Box{t: T3, n: N{v: 1}}\n"), run_output(R"JAI(
				class T { int v = 3; string to_string() { return `T${v}`; } }
				class D : T { }
				class N { int v = 1; int to_string() { return 5; } }
				class Box { T t; N n; }
				print(T(), " ", [T(), T()], " ", {"k": T()});
				print("D:", D());
				print(Box());
			)JAI"));
		});

		test("handles_show_their_pointee_and_cycles_terminate", [this]() {
			check_eq(std::string(
				"Node{v: 1, next: Node{v: 2, next: <cycle Node>, back: null}, back: null}\n"
				"Node{v: 1, next: Node{v: 2, next: null, back: <cycle Node>}, back: null}\n"
				"[Node{v: 0, next: null, back: null}, Node{v: 0, next: null, back: null}]\n"
				"Pair{l: Node{v: 5, next: null, back: null}, r: Node{v: 5, next: null, back: null}}\n"
				"null [null]\n"
				"Node{v: 1, next: <cycle Node>, back: <cycle Node>}\n"), run_output(R"JAI(
				class Node { int v = 0; shared_ptr<Node> next; weak_ptr<Node> back; }
				class Pair { shared_ptr<Node> l; shared_ptr<Node> r; }
				shared_ptr<Node> a = new Node();
				shared_ptr<Node> b = new Node();
				a.v = 1; b.v = 2;
				a.next = b; b.next = a;
				print(a);
				b.back = a;
				b.next = null;
				print(a);
				shared_ptr<Node> c = new Node();
				print([c, c]);
				shared_ptr<Node> five = new Node();
				five.v = 5;
				Pair p;
				p.l = five; p.r = five;
				print(p);
				weak_ptr<Node> w;
				{
					shared_ptr<Node> t = new Node();
					w = t;
				}
				print(w, " ", [w]);
				a.next = a; a.back = a;
				print(a);
				a.next = null; a.back = null; b.back = null;
			)JAI"));
		});

		test("functions_and_coroutines_name_their_kind", [this]() {
			check_eq(std::string("<function> <function> [<function>] <coroutine>\n"), run_output(R"JAI(
				int g(int a) { return a; }
				auto f = [](int a) { return a; };
				coroutine int counter() { yield 1; }
				var co = counter();
				print(g, " ", f, " ", [print], " ", co);
			)JAI"));
		});

		test("every_text_path_agrees", [this]() {
			const std::string want = "[T3, {\"k\": P{x: 1}}]";
			check_eq(want + "\n" + want + "\n" + want + "\n" + want + "\n" + want + "\n" + want + "\n" + want + "\n" + want + "\n" + want + "\n" + want + "\n",
				run_output(R"JAI(
				class T { int v = 3; string to_string() { return `T${v}`; } }
				class P { int x = 1; }
				var v = [T(), {"k": P()}];
				print(v);
				print(to_string(v));
				print(`${v}`);
				print(`${v}`);
				print(format("{}", v));
				print("{}", v);
				print([v].join(""));
				string s = cast<string>(v);
				print(s);
				string id(string t) { return t; }
				print(id(to_string(v)));
				try { throw v; } catch (e) { print(e); }
			)JAI"));
		});

		test("padding_specs_pad_the_display_text_and_numeric_specs_name_the_value", [this]() {
			check_eq(std::string(
				"[1]   |  [1]|P{x: 1}  |\n"
				"caught: format spec ':.2f' does not apply to array value\n"
				"caught: format spec ':d' does not apply to object value\n"), run_output(R"JAI(
				class P { int x = 1; }
				var a = [1];
				print(format("{:<6}|{:>5}|", a, a) + `${P():<9}|`);
				try { print(format("{:.2f}", a)); } catch (e) { print("caught: ", e); }
				try { print(`${P():d}`); } catch (e) { print("caught: ", e); }
			)JAI"));
		});

		test("a_container_first_argument_is_never_a_format_string", [this]() {
			check_eq(std::string("[\"{}\"]1\n{\"{}\": 2}x\n[{}]2\n"), run_output(R"JAI(
				print(["{}"], 1);
				print({"{}": 2}, "x");
				print(format([{}], 2));
			)JAI"));
		});

		test("a_failing_or_throwing_to_string_inside_a_container_is_the_callers_error", [this]() {
			check_eq(std::string(
				"caught: nope\n"
				"caught: nope\n"
				"caught: Division by zero\n"
				"caught: Division by zero\n"
				"caught: nope\n"), run_output(R"JAI(
				class Bad { string to_string() { throw "nope"; } }
				class Bad2 { string to_string() { int z = 0; return `x${(1 / z)}`; } }
				try { print([1, Bad()]); } catch (e) { print("caught: ", e); }
				try { string s = `p${{"k": [Bad()]}}`; print(s); } catch (e) { print("caught: ", e); }
				try { print(to_string([Bad2()])); } catch (e) { print("caught: ", e); }
				try { print(`${[Bad2()]}`); } catch (e) { print("caught: ", e); }
				try { print([Bad()].join(",")); } catch (e) { print("caught: ", e); }
			)JAI"));
		});

		test("a_to_string_that_changes_the_printed_container_reads_what_the_walk_holds", [this]() {
			check_eq(std::string("[m, 5, 6] [1, 2, 3]\n[m, 2, 3]\n"), run_output(R"JAI(
				var log = [];
				class Swap { string to_string() { log = [1, 2, 3]; return "m"; } }
				log = [Swap(), 5, 6];
				string s = to_string(log);
				print(s, " ", log);
				class Mutate { string to_string() { log.clear(); log.push(1); log.push(2); log.push(3); return "m"; } }
				log = [Mutate(), 5, 6];
				print(log);
			)JAI"));
		});

		test("large_and_deep_values_print_in_full", [this]() {
			// 0..19999 is 88890 digits; a Link level is "Link{next: " and "}"; a map entry adds ": \"v\"" and every separator ", "
			check_eq(std::string("128890 240016 228890 220011 4004\n"), run_output(R"JAI(
				array<int> big = [];
				for (int i = 0; i < 20000; ++i) { big.push(i); }
				class Link { shared_ptr<Link> next; }
				shared_ptr<Link> head = new Link();
				for (int i = 0; i < 20000; ++i) { shared_ptr<Link> n = new Link(); n.next = head; head = n; }
				string deep = to_string(head);
				var maps = {};
				for (int i = 0; i < 20000; ++i) { maps[i] = "v"; }
				var nested = [];
				for (int i = 0; i < 2000; ++i) { nested = [nested]; }
				print(to_string(big).size(), " ", deep.size(), " ", to_string(maps).size(), " ", deep.find("null"), " ", to_string([nested]).size());
			)JAI", true));
		});

		test("host_objects_use_their_bound_to_string_else_their_type_name", [this]() {
			using value_text_fixtures::Lamp;
			using value_text_fixtures::Crate;
			using value_text_fixtures::Gauge;
			auto e = make_engine();
			stdlib::register_all(*e);
			auto out = std::make_shared<std::ostringstream>();
			e->set_output_stream(out);
			dynamic_binder<Lamp>(*e, "Lamp").constructor<>().property("watts", &Lamp::watts).method("to_string", &Lamp::to_string).build();
			dynamic_binder<Crate>(*e, "Crate").constructor<>().property("items", &Crate::items).build();
			dynamic_binder<Gauge>(*e, "Gauge").auto_bind().build();
			e->add_global("gauge", e->make_object(std::make_shared<Gauge>()));
			e->execute(R"JAI(
				class Shelf : Crate { int label = 4; }
				print(Lamp(), " ", [Crate()], " ", "c: " + Crate());
				print(Shelf());
				print(gauge.level, " ", [gauge.level]);
			)JAI");
			check_eq(std::string("lamp(60) [<Crate>] c: <Crate>\nShelf{label: 4}\n7 [7]\n"), out->str());
		});

		test("to_string_is_byte_identical_to_print_for_every_kind", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			auto out = std::make_shared<std::ostringstream>();
			e->set_output_stream(out);
			e->execute(R"JAI(
				class T { int v = 3; string to_string() { return `T${v}`; } }
				class P { int x = 1; string s = "q\"t"; }
				class Node { int v = 0; shared_ptr<Node> next; weak_ptr<Node> back; }
				shared_ptr<Node> a = new Node();
				a.next = a;
				a.back = a;
				shared_ptr<Node> b = new Node();
				weak_ptr<Node> wb = b;
				P shared = P();
				var kinds = [
					[1, [2, [3, []]], {}], {"k": [1, {"n": null}], "s": "x\n"}, 'c', "top", 2.5, true, null,
					T(), P(), [T(), P()], {"t": T()}, a, wb, [shared, shared], utf8_string("u"), math::vec3(1, 2, 3), print
				];
				for (var& k : kinds) {
					print(k);
					print(to_string(k));
					print(`${k}`);
					print(`${k}`);
					print(format("{}", k));
				}
				a.next = null; a.back = null;
			)JAI");
			std::istringstream lines(out->str());
			std::vector<std::string> got;
			for (std::string line; std::getline(lines, line);) { got.push_back(line); }
			check_eq(static_cast<size_t>(17 * 5), got.size(), "five texts per kind");
			for (size_t i = 0; i + 4 < got.size(); i += 5) {
				for (size_t path = 1; path < 5; ++path) { check_eq(got[i], got[i + path], "print and every other path agree on kind " + std::to_string(i / 5)); }
			}
			check_eq(std::string("[1, [2, [3, []]], {}]"), got[0]);
			check_eq(std::string("{\"k\": [1, {\"n\": null}], \"s\": \"x\\n\"}"), got[5]);
			check_eq(std::string("Node{v: 0, next: <cycle Node>, back: <cycle Node>}"), got[55]);
			check_eq(std::string("[P{x: 1, s: \"q\\\"t\"}, P{x: 1, s: \"q\\\"t\"}]"), got[65]);
		});

		test("the_structural_text_runs_no_script_and_stops_at_a_limit", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			e->execute(R"JAI(
				class T { int v = 3; string to_string() { return `T${v}`; } }
				var held = [T(), "s", [1, 2, 3, 4, 5, 6, 7, 8]];
			)JAI");
			script_value held = e->get_variable("held");
			check_eq(std::string("[T{v: 3}, \"s\", [1, 2, 3, 4, 5, 6, 7, 8]]"), held.to_string());
			check_eq(std::string("[T3, \"s\", [1, 2, 3, 4, 5, 6, 7, 8]]"), e->text_of(held).value());
			check_eq(std::string("[T{v: 3}, \"s\", [1, 2, 3, 4..."), detail::value_text(held, {}, 26).value());
		});
	}
};

}   // namespace jai::foundry::tests

using value_text_tests = jai::foundry::tests::value_text_tests;
FOUNDRY_REGISTER(value_text_tests)
