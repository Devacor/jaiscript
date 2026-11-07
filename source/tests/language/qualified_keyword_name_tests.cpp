#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <memory>
#include <string>

using namespace jai;
using namespace jai::foundry;

// A reserved word is a name where a qualifier or a member declaration makes it one; bare, it stays the keyword
namespace qualified_keyword_hosts {
	struct promise {
		int64_t settled = 0;
		std::string trail;
		promise() = default;
		int64_t catch_it(int64_t a_code) { trail += "catch;"; settled = a_code; return a_code * 2; }
		std::string remove(const std::string& a_key) { trail += "delete " + a_key + ";"; return a_key; }
		static int64_t make_new() { return 41; }
	};
}

namespace jai::foundry::tests {

class qualified_keyword_name_tests : public suite {
public:
	qualified_keyword_name_tests() : suite("Qualified Keyword Names") {}

	static std::string run(const std::shared_ptr<engine>& a_engine, const std::string& a_source) {
		try {
			return a_engine->execute(a_source).to_string();
		} catch (const std::exception& e) {
			return std::string("ERROR: ") + e.what();
		}
	}

	// a parse listing's first entry, without its source line and caret
	static std::string first_diagnostic(const std::string& a_text) {
		const size_t header = a_text.find('\n');
		return header == std::string::npos ? a_text : a_text.substr(0, a_text.find('\n', header + 1));
	}

	static std::shared_ptr<engine> fresh() {
		auto result = make_engine();
		stdlib::register_all(result);
		return result;
	}

	void forge_tests() override {
		test("keyword_methods_and_fields_after_a_dot", [this]() {
			check_eq(std::string("14|7|c|8|42|false|3|imp|9|11"), run(fresh(), R"(
				class Promise {
					int default = 7;
					string class = "c";
					int for = 3;
					int catch(function f) { return f(this.default); }
					int new() { return this.default + 1; }
					int return(int x) { return x * 2; }
					bool if(bool b) { return !b; }
					string import() { return "imp"; }
					int delete() { return 9; }
				}
				Promise p;
				string out = to_string(p.catch((x) => x * 2)) + "|" + to_string(p.default) + "|" + p.class + "|" + to_string(p.new()) + "|" +
					to_string(p.return(21)) + "|" + to_string(p.if(true)) + "|" + to_string(p.for) + "|" + p.import() + "|" + to_string(p.delete());
				p.default = 11;
				out + "|" + to_string(p.default)
			)"));
		});

		test("keyword_fields_store_through_every_assignment_form", [this]() {
			check_eq(std::string("6|5|1|x!|9"), run(fresh(), R"(
				class Box { int for = 3; int if = 0; int while = 0; string catch = "x"; array<int> import = [1, 2]; }
				Box b;
				b.for += 3;
				b.if = b.for - 1;
				b.while++;
				b.catch += "!";
				b.import[1] = 9;
				to_string(b.for) + "|" + to_string(b.if) + "|" + to_string(b.while) + "|" + b.catch + "|" + to_string(b.import[1])
			)"));
		});

		test("keyword_members_through_handles_and_null_safe_access", [this]() {
			check_eq(std::string("2|2|null|5"), run(fresh(), R"(
				class Node { int default = 2; int catch() { return this.default; } }
				shared_ptr<Node> sp = new Node();
				shared_ptr<Node> none = null;
				var loose = Node();
				loose.default = 5;
				to_string(sp->catch()) + "|" + to_string(sp?.default) + "|" + to_string(none?.catch()) + "|" + to_string(loose.default)
			)"));
		});

		test("keyword_members_after_double_colon", [this]() {
			check_eq(std::string("fetched a.com|4|6|5|12|6"), run(fresh(), R"(
				namespace spider {
					string catch(string url) { return "fetched " + url; }
					int default = 4;
					const new = 5;
					int bump() { return 1; }
					int return(int x) { return x + spider::default + spider::bump(); }
					function if = (x) => x * 3;
				}
				string out = spider::catch("a.com") + "|" + to_string(spider::default);
				spider::default = 6;
				out + "|" + to_string(spider::default) + "|" + to_string(spider::new) + "|" + to_string(spider::return(5)) + "|" + to_string(spider::if(2))
			)"));
			check_eq(std::string("0|1|3|c"), run(fresh(), R"(
				enum Mode { default, catch, for, if }
				string out = to_string(cast<int>(Mode::default)) + "|" + to_string(cast<int>(Mode::catch)) + "|" + to_string(cast<int>(Mode::if));
				switch (Mode::catch) {
					case Mode::default: out += "|d";
					case Mode::catch: out += "|c";
				}
				out
			)"));
			check_eq(std::string("10|12|3|20"), run(fresh(), R"(
				class Limits { static int class = 10; const static int import = 12; const default = 3; static int new(int x) { return x * 2; } }
				to_string(Limits::class) + "|" + to_string(Limits::import) + "|" + to_string(Limits::default) + "|" + to_string(Limits::new(10))
			)"));
		});

		test("keyword_methods_inherit_and_override", [this]() {
			check_eq(std::string("3|3|3|1|base"), run(fresh(), R"(
				class Base { int catch() { return 1; } string default() { return "base"; } }
				class Derived : Base { override int catch() { return 2 + super::catch(); } }
				Derived d;
				Base b = d;
				var loose = d;
				shared_ptr<Base> sp = new Derived();
				to_string(d.catch()) + "|" + to_string(b.catch()) + "|" + to_string(sp->catch()) + "|" + to_string(Base().catch()) + "|" + loose.default()
			)"));
		});

		test("keyword_members_through_reflection", [this]() {
			check_eq(std::string("true|true|true|8|1|5|7"), run(fresh(), R"(
				class Promise { int default = 7; function new = null; int catch(int x) { return x * 2; } }
				Promise p;
				string out = to_string(reflect::has_method(p, "catch")) + "|" + to_string(reflect::has_field(p, "default")) + "|" +
					to_string(reflect::get(p, "new") == null) + "|" + to_string(reflect::invoke(p, "catch", 4));
				reflect::set(p, "new", (x) => x + 1);
				reflect::set(p, "default", 5);
				var bound = reflect::bind(p, "catch");
				out + "|" + to_string(p.new(0)) + "|" + to_string(p.default) + "|" + to_string(bound(3) + 1)
			)"));
		});

		test("keyword_fields_serialize_under_their_names", [this]() {
			check_eq(std::string("{\"$type\":\"Box\",\"$val\":{\"default\":9,\"class\":\"k\",\"for\":[1,2]}}|11|k|9"), run(fresh(), R"(
				class Box { int default = 1; string class = "k"; array<int> for = [1, 2]; }
				Box b;
				b.default = 9;
				string j = to_json(b);
				Box back = from_json(j);
				Box bin = from_binary(to_binary(b));
				j + "|" + to_string(back.default + back.for[1]) + "|" + back.class + "|" + to_string(bin.default)
			)"));
		});

		test("host_bound_keyword_members", [this]() {
			using qualified_keyword_hosts::promise;
			auto eng = fresh();
			dynamic_binder<promise>(*eng, "HostPromise")
				.constructor<>()
				.method("catch", &promise::catch_it)
				.method("delete", &promise::remove)
				.property("default", &promise::settled)
				.static_method("new", &promise::make_new)
				.build();
			auto& ns = detail::engine_access::script_namespaces(*eng)[detail::engine_access::symbolize(*eng, "net")];
			if (!ns) { ns = std::make_shared<script_namespace_data>(); }
			ns->variables[detail::engine_access::symbolize(*eng, "catch")] = script_value(static_cast<script_int>(17), eng.get());
			check_eq(std::string("8|4|k|41|17|true|10"), run(eng, R"(
				auto p = HostPromise();
				string out = to_string(p.catch(4)) + "|" + to_string(p.default) + "|" + p.delete("k") + "|" + to_string(HostPromise::new()) + "|" + to_string(net::catch);
				p.default = 10;
				out + "|" + to_string(reflect::has_method(p, "catch")) + "|" + to_string(reflect::get(p, "default"))
			)"));
		});

		test("bare_keywords_stay_reserved", [this]() {
			auto eng = fresh();
			check_eq(std::string("ERROR: parse failed:\n<script>:1:1: error: expected expression"), first_diagnostic(run(eng, "catch(5);")));
			check_eq(std::string("ERROR: parse failed:\n<script>:1:6: error: expected variable name"), first_diagnostic(run(eng, "void catch() {}")));
			check_eq(std::string("ERROR: parse failed:\n<script>:1:5: error: expected variable name"), first_diagnostic(run(eng, "int default = 0;")));
			check_eq(std::string("ERROR: parse failed:\n<script>:1:43: error: expected expression"),
			         first_diagnostic(run(eng, "class C { void catch(int x) {} void g() { catch(1); } }")));
			check_eq(std::string("ERROR: parse failed:\n<script>:1:39: error: expected expression"),
			         first_diagnostic(run(eng, "class C { int x = 0; int f() { return default; } }")));
			const std::string parameter = first_diagnostic(run(eng, "int f(int default) { return 1; }"));
			check_true(parameter.rfind("ERROR: parse failed:\n<script>:1:11: error: expected parameter name", 0) == 0, parameter);
		});

		test("this_and_super_name_no_member", [this]() {
			auto eng = fresh();
			check_eq(std::string("ERROR: parse failed:\n<script>:1:15: error: 'this' is the receiver and cannot be used as a member name"), first_diagnostic(run(eng, "class C { int this = 0; }")));
			check_eq(std::string("ERROR: parse failed:\n<script>:1:16: error: 'super' is the base class and cannot be used as a member name"), first_diagnostic(run(eng, "class C { void super() {} }")));
			check_eq(std::string("ERROR: parse failed:\n<script>:1:19: error: 'this' is the receiver and cannot be used as a member name"), first_diagnostic(run(eng, "namespace n { int this = 1; }")));
			check_eq(std::string("ERROR: parse failed:\n<script>:1:10: error: expected enum value name"), first_diagnostic(run(eng, "enum E { this }")));
			check_eq(std::string("ERROR: parse failed:\n<script>:1:21: error: expected member name"), first_diagnostic(run(eng, "class C { const int = 1; }")));
		});
	}
};

} // namespace jai::foundry::tests

using qualified_keyword_name_tests = jai::foundry::tests::qualified_keyword_name_tests;
FOUNDRY_REGISTER(qualified_keyword_name_tests)
