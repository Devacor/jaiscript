#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <stdexcept>
#include <string>

namespace jai::foundry::tests {

class constant_argument_tests : public suite {
public:
	constant_argument_tests() : suite("Constant Arguments") {}

	struct store {
		std::string last;
	};

	static constexpr const char* k_refusal = "SQL text must be a constant; pass values as parameters (?)";

	std::shared_ptr<engine> make_host() {
		auto e = make_engine();
		stdlib::register_all(*e);
		engine* raw = e.get();
		e->add_function("run_sql", [raw](const std::string& sql, const script_value& value) -> std::string {
			if (!raw->constant_argument_admits("run_sql", 0, sql)) { throw std::runtime_error(k_refusal); }
			(void)value;
			return "ran " + sql;
		});
		e->add_function("choose", [raw](const std::string& wanted, std::vector<std::string> choices) -> std::string {
			for (const auto& c : choices) {
				if (c == wanted && raw->constant_argument_admits("choose", 1, c)) { return c; }
			}
			throw std::runtime_error("not a choice");
		});
		dynamic_binder<store>(*e, "Store")
			.constructor<>()
			.method("query", [raw](store& self, const std::string& sql) -> std::string {
				if (!raw->constant_argument_admits("Store.query", 0, sql)) { throw std::runtime_error(k_refusal); }
				self.last = sql;
				return "queried " + sql;
			})
			.build();
		e->constant_argument("run_sql", 0, k_refusal);
		e->constant_argument("choose", 1, "the choices must be constants");
		e->constant_argument("Store.query", 0, k_refusal);
		return e;
	}

	std::string load_error(engine& e, const std::string& source) {
		try {
			e.execute(source);
		} catch (const std::exception& ex) {
			return ex.what();
		}
		return "";
	}

	void forge_tests() override {
		test("constant_forms_pass_the_checker_and_run", [this]() {
			auto e = make_host();
			check_eq(std::string("ran SELECT 1|ran SELECT * FROM t WHERE id = ?|ran SELECT a|ran SELECT b|ran SELECT x FROM t"), e->execute(R"(
				const string FIND = "SELECT * FROM t WHERE id = ?";
				const string COLUMNS = "x";
				bool which = true;
				run_sql("SELECT 1", 0) + "|" + run_sql(FIND, 7) + "|" + run_sql(which ? "SELECT a" : "SELECT b", 0) + "|" +
					run_sql(!which ? "SELECT a" : "SELECT b", 0) + "|" +
					run_sql("SELECT " + COLUMNS + " FROM t", 0);)").as<std::string>());
		});

		test("the_checker_refuses_an_argument_that_is_not_a_constant", [this]() {
			auto e = make_host();
			const std::string text = load_error(*e, "string name = \"x\"; run_sql(\"SELECT * FROM t WHERE name = '\" + name + \"'\", 0);");
			check_true(text.find("argument 1 of 'run_sql' is not a constant: SQL text must be a constant; pass values as parameters (?)") != std::string::npos, text);
			const std::string templated = load_error(*e, "int id = 3; run_sql(`SELECT * FROM t WHERE id = ${id}`, 0);");
			check_true(templated.find("is not a constant") != std::string::npos, templated);
			const std::string method = load_error(*e, "Store s; string q = \"SELECT 1\"; s.query(q + \"\");");
			check_true(method.find("argument 1 of 'query' is not a constant") != std::string::npos, method);
			const std::string choices = load_error(*e, "array<string> mine = [\"a\"]; choose(\"a\", mine);");
			check_true(choices.find("argument 2 of 'choose' is not a constant: the choices must be constants") != std::string::npos, choices);
		});

		test("at_run_time_a_text_the_program_does_not_spell_is_refused_on_every_path", [this]() {
			auto e = make_host();
			check_eq(std::string(k_refusal), e->execute(R"([[warn_mode]];
				string m = "";
				var user = "1; DROP TABLE t";
				try { run_sql("SELECT * FROM t WHERE id = " + user, 0); } catch (e) { m = e; }
				m;)").as<std::string>());
			check_eq(std::string(k_refusal), e->execute(R"([[warn_mode]];
				string m = "";
				var f = run_sql;
				try { f("DELETE FROM t", 0); } catch (e) { m = e; }
				m;)").as<std::string>());
			check_eq(std::string(k_refusal), e->execute(R"([[warn_mode]];
				string m = "";
				var s = Store();
				try { reflect::invoke(s, "query", "DELETE FROM t"); } catch (e) { m = e; }
				m;)").as<std::string>());
			check_eq(std::string(k_refusal), e->execute(R"([[warn_mode]];
				string m = "";
				var s = Store();
				var text = "SELECT 2";
				try { s.query(text + ";DROP TABLE t"); } catch (e) { m = e; }
				m;)").as<std::string>());
		});

		test("a_built_text_equal_to_a_constant_is_that_constant", [this]() {
			auto e = make_host();
			check_eq(std::string("ran SELECT 1"), e->execute(R"([[warn_mode]];
				run_sql("SELECT 1", 0);
				var built = "SELECT" + " " + to_string(1);
				run_sql(built, 0);)").as<std::string>());
		});

		test("units_are_gathered_across_executes", [this]() {
			auto e = make_host();
			check_eq(std::string("ran SELECT 9"), e->execute("run_sql(\"SELECT 9\", 0);").as<std::string>());
			e->execute("const string LATER = \"SELECT later\";");
			check_eq(std::string("ran SELECT later"), e->execute("run_sql(LATER, 0);").as<std::string>());
			check_eq(std::string("not a choice|b"), e->execute(R"([[warn_mode]];
				string m = "";
				try { choose("c", ["a", "b"]); } catch (e) { m = e; }
				m + "|" + choose("b", ["a", "b"]);)").as<std::string>());
		});

		test("a_const_another_unit_declares_later_is_admitted", [this]() {
			auto e = make_host();
			e->execute("[[warn_mode]]; var call_later = function() { return run_sql(SHARED_QUERY, 0); };");
			e->execute("const string SHARED_QUERY = \"SELECT shared\";");
			check_eq(std::string("ran SELECT shared"), e->execute("call_later();").as<std::string>());
		});
	}
};

} // namespace jai::foundry::tests

using constant_argument_tests = jai::foundry::tests::constant_argument_tests;
FOUNDRY_REGISTER(constant_argument_tests)
