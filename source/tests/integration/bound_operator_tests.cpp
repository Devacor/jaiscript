#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/detail/type_checker.hpp>

#include <memory>
#include <string>

using namespace jai;
using namespace jai::foundry;

// A bound class's operators answer wherever the language compares or combines its values, and an operator counts for an
// operand only when one of its overloads takes it, as a script class's does
namespace bound_operator_hosts {
	struct money {
		int64_t cents = 0;
		money() = default;
		explicit money(int64_t a_cents) : cents(a_cents) {}
	};
	struct tag {
		int64_t id = 0;
		tag() = default;
		explicit tag(int64_t a_id) : id(a_id) {}
	};
	struct meter {
		int64_t n = 0;
		meter() = default;
		explicit meter(int64_t a_n) : n(a_n) {}
	};
	// std::atomic's shape: not copyable, operators that return a value or the object, a conversion to its value
	struct tally {
		int64_t n = 0;
		int64_t calls = 0;
		tally() = default;
		tally(const tally&) = delete;
		tally& operator=(const tally&) = delete;
	};
	// Operators the class declares, as members and as free functions, which its binder binds with no line of its own
	struct cents {
		int64_t n = 0;
		cents() = default;
		explicit cents(int64_t a_n) : n(a_n) {}
		cents operator+(const cents& a_other) const { return cents(n + a_other.n); }
		cents operator-() const { return cents(-n); }
		cents& operator+=(const cents& a_other) { n += a_other.n; return *this; }
		cents& operator++() { ++n; return *this; }
		bool operator==(const cents& a_other) const = default;
		auto operator<=>(const cents& a_other) const { return n <=> a_other.n; }
		int64_t operator[](int64_t a_digit) const { return (n / (a_digit == 0 ? 1 : 10 * a_digit)) % 10; }
		int64_t operator()(int64_t a_times) const { return n * a_times; }
		std::string to_string() const { return std::to_string(n) + "c"; }
	};
	inline cents operator*(const cents& a_left, const cents& a_right) { return cents(a_left.n * a_right.n); }
	// converts to an int, which gives no class operator of its own
	struct handle {
		int64_t id = 0;
		operator int64_t() const { return id; }
	};
}

namespace jai::foundry::tests {

class bound_operator_tests : public suite {
public:
	bound_operator_tests() : suite("Bound Operators") {}

	static std::shared_ptr<engine> make_bound_engine() {
		using bound_operator_hosts::money;
		using bound_operator_hosts::tag;
		auto eng = make_engine();
		stdlib::register_all(eng);
		dynamic_binder<money>(*eng, "Money")
			.constructor<int64_t>()
			.property("cents", &money::cents)
			.method("==", [](const money& a_self, const money& a_other) { return a_self.cents == a_other.cents; })
			.method("<", [](const money& a_self, const money& a_other) { return a_self.cents < a_other.cents; })
			.method("<=>", [](const money& a_self, int64_t a_other) -> int64_t { return a_self.cents < a_other ? -1 : a_self.cents > a_other ? 1 : 0; })
			.method("+", [](const money& a_self, const money& a_other) { return money(a_self.cents + a_other.cents); })
			.method("to_string", [](const money& a_self) { return std::to_string(a_self.cents) + "c"; })
			.build();
		dynamic_binder<tag>(*eng, "Tag")
			.constructor<int64_t>()
			.property("id", &tag::id)
			.method("==", [](const tag& a_self, const tag& a_other) { return a_self.id == a_other.id; })
			.method("<=>", [](const tag& a_self, const tag& a_other) -> int64_t { return a_self.id < a_other.id ? -1 : a_self.id > a_other.id ? 1 : 0; })
			.build();
		return eng;
	}

	static std::shared_ptr<engine> make_meter_engine() {
		using bound_operator_hosts::meter;
		auto eng = make_engine();
		stdlib::register_all(eng);
		dynamic_binder<meter>(*eng, "Meter")
			.constructor<int64_t>()
			.property("n", &meter::n)
			.method("-", [](const meter& a_self, const meter& a_other) { return meter(a_self.n - a_other.n); })
			.method("-", [](int64_t a_left, const meter& a_right) { return meter(a_left - a_right.n); })
			.method("==", [](const meter& a_self, int64_t a_other) { return a_self.n == a_other; })
			.method("add", [](meter& a_self, const meter& a_other) { a_self.n += a_other.n; })
			.method("+=", [](meter& a_self, const meter& a_other) -> meter& { a_self.n += 100 * a_other.n; return a_self; })
			.build();
		return eng;
	}

	static std::shared_ptr<engine> make_tally_engine(bound_operator_hosts::tally& a_hits) {
		using bound_operator_hosts::tally;
		auto eng = make_engine();
		stdlib::register_all(eng);
		dynamic_binder<tally>(*eng, "Tally")
			.method("++", [](tally& a_self) -> tally& { ++a_self.calls; ++a_self.n; return a_self; })
			.method("++", [](tally& a_self, int64_t) { ++a_self.calls; return a_self.n++; })
			.method("--", [](tally& a_self) { ++a_self.calls; return --a_self.n; })
			.method("--", [](tally& a_self, int64_t) { ++a_self.calls; return a_self.n--; })
			.method("+=", [](tally& a_self, int64_t a_by) -> tally& { ++a_self.calls; a_self.n += a_by; return a_self; })
			.method("-=", [](tally& a_self, int64_t a_by) { ++a_self.calls; return a_self.n -= a_by; })
			.method("=", [](tally& a_self, int64_t a_to) -> tally& { ++a_self.calls; a_self.n = a_to; return a_self; })
			.method("to_int", [](const tally& a_self) { return a_self.n; })
			.method("count", [](const tally& a_self) { return a_self.n; })
			.build();
		eng->add_global_ref("hits", a_hits);
		return eng;
	}

	void forge_tests() override {
		test("bound_operators_answer_wherever_values_compare", [this]() {
			auto eng = make_bound_engine();
			eng->execute(R"(
				Money a = Money(1); Money b = Money(2); Money c = Money(1);
				class Wallet { Money m = Money(0); Wallet(Money a_m) { m = a_m; } }
				array<Money> sorted = [Money(3), Money(1), Money(2)];
				sorted.sort();
			)");
			check_eq(true, eng->execute("a == c && a != b && a < b").as_bool());
			check_eq(true, eng->execute("[a, b] == [c, b] && [a, b].contains(c) && [b, a].index_of(c) == 1").as_bool());
			check_eq(true, eng->execute("{1: a} == {1: c} && Wallet(a) == Wallet(c) && Wallet(a) != Wallet(b)").as_bool());
			check_eq((int64_t)123, eng->execute("sorted[0].cents * 100 + sorted[1].cents * 10 + sorted[2].cents").as_int());
		});

		test("an_explicit_operator_that_cannot_take_the_operand_yields_to_the_spaceship", [this]() {
			auto eng = make_bound_engine();
			eng->execute("Money a = Money(1);");
			check_eq(true, eng->execute("a < 5").as_bool());
			check_eq(false, eng->execute("a > 5").as_bool());
			check_eq(true, eng->execute("5 > a").as_bool());
			check_eq(true, eng->execute("a < Money(2)").as_bool());
			auto script = make_engine();
			script->execute(R"(
				class V { int v = 0; V(int a_v) { v = a_v; } bool operator<(V o) { return v < o.v; } int operator<=>(int o) { return v - o; } }
				V w = V(1);
			)");
			check_eq(true, script->execute("w < 5").as_bool());
			check_eq(true, script->execute("w < V(2)").as_bool());
		});

		test("a_bound_operator_that_cannot_take_the_operand_leaves_the_builtin_meaning", [this]() {
			auto eng = make_bound_engine();
			eng->execute("Money a = Money(7);");
			check_eq(std::string("7c!"), eng->execute("a + \"!\"").as<std::string>());
			check_eq((int64_t)9, eng->execute("(a + Money(2)).cents").as_int());
		});

		test("equality_without_a_spaceship_answers_only_values_of_the_class", [this]() {
			auto eng = make_bound_engine();
			eng->execute("Money a = Money(1); Tag t = Tag(1);");
			check_eq(false, eng->execute("a == t").as_bool());
			check_eq(true, eng->execute("a != t").as_bool());
			auto script = make_engine();
			script->execute(R"(
				class P { int v = 0; P() {} bool operator==(P o) { return v == o.v; } }
				class Q { int v = 0; Q() {} }
				P p = P();
			)");
			script->execute("class N { int v = 3; N() {} bool operator==(int a_n) { return v == a_n; } } N n = N();");
			check_eq(true, script->execute("n == 3 && 3 == n && n != 4 && 4 != n").as_bool());
			check_eq(false, script->execute("p == Q()").as_bool());
			check_eq(true, script->execute("p != Q()").as_bool());
			check_eq(true, script->execute("p == P()").as_bool());
		});

		test("a_bound_reflected_operator_answers_a_left_operand_with_none", [this]() {
			auto eng = make_meter_engine();
			eng->execute("Meter m = Meter(3);");
			check_eq((int64_t)7, eng->execute("(10 - m).n").as_int());
			check_eq((int64_t)1, eng->execute("(m - Meter(2)).n").as_int());
			check_eq(true, eng->execute("m == 3 && 3 == m && m != 4 && 4 != m").as_bool());
		});

		test("a_missing_bound_operator_names_the_binding_to_add", [this]() {
			auto eng = make_meter_engine();
			eng->execute("Meter m = Meter(3);");
			auto miss = [&](const std::string& code) {
				try { eng->execute(code); } catch (const std::exception& e) { return std::string(e.what()); }
				return std::string("no error");
			};
			check_true(miss("m - 1").find("No bound 'Meter - int': bind operator \"-\" taking int on Meter, or convert the operand") != std::string::npos, miss("m - 1"));
			check_true(miss("2 * m").find("No bound 'int * Meter': bind operator \"*\" as (int, Meter) on Meter, or convert the operand") != std::string::npos, miss("2 * m"));
			check_true(miss("2 < m").find("No bound 'int < Meter': bind operator \"<=>\" taking int on Meter, or convert the operand") != std::string::npos, miss("2 < m"));
			check_true(miss("m.add(5)").find("No overload of 'add' takes (int)") != std::string::npos, miss("m.add(5)"));
			auto money = make_bound_engine();
			money->execute("Money a = Money(1);");
			std::string spaceship;
			try { money->execute("a <=> Money(2)"); } catch (const std::exception& e) { spaceship = e.what(); }
			check_true(spaceship.find("No bound 'Money <=> Money': bind operator \"<=>\" taking Money on Money, or convert the operand") != std::string::npos, spaceship);
		});

		test("a_bound_compound_operator_changes_its_target_in_place", [this]() {
			auto eng = make_meter_engine();
			eng->execute(R"(
				Meter m = Meter(1);
				m += Meter(2);
				class Box { Meter inner = Meter(1); Box() {} }
				Box box = Box();
				box.inner += Meter(1);
				array<Meter> ms = [Meter(1)];
				ms[0] += Meter(3);
				Meter& alias = m;
				alias += Meter(1);
			)");
			check_eq((int64_t)301, eng->execute("m.n").as_int());
			check_eq((int64_t)101, eng->execute("box.inner.n").as_int());
			check_eq((int64_t)301, eng->execute("ms[0].n").as_int());
			check_eq((int64_t)401, eng->execute("(m += Meter(1)).n").as_int());
			auto money = make_bound_engine();
			check_eq((int64_t)3, money->execute("Money a = Money(1); a += Money(2); a.cents").as_int());
		});

		test("a_bound_increment_and_compound_run_once_and_yield_what_they_return", [this]() {
			bound_operator_hosts::tally hits;
			auto eng = make_tally_engine(hits);
			check_eq((int64_t)1, eng->execute("(++hits).count()").as_int());
			check_eq((int64_t)1, eng->execute("hits++").as_int());
			check_eq((int64_t)1, eng->execute("--hits").as_int());
			check_eq((int64_t)1, eng->execute("hits--").as_int());
			check_eq((int64_t)0, hits.n);
			check_eq((int64_t)4, hits.calls);
			check_eq((int64_t)5, eng->execute("(hits += 5).count()").as_int());
			check_eq((int64_t)3, eng->execute("hits -= 2").as_int());
			check_eq((int64_t)6, hits.calls);
			eng->execute(R"(
				Tally& alias = hits;
				alias += 10; ++alias; alias++; alias -= 1; --alias; alias--;
				class Holder { void bump() { hits += 100; hits++; } }
				Holder().bump();
			)");
			check_eq((int64_t)113, hits.n);
			check_eq((int64_t)14, hits.calls);
		});

		test("a_reference_parameter_binds_a_bound_object_without_a_copy", [this]() {
			bound_operator_hosts::tally hits;
			auto eng = make_tally_engine(hits);
			eng->execute("void bump(Tally& t) { t += 5; ++t; } bump(hits); bump(hits);");
			check_eq((int64_t)12, hits.n);
			check_eq((int64_t)4, hits.calls);
		});

		test("a_typed_slot_converts_a_bound_object_through_its_conversion", [this]() {
			bound_operator_hosts::tally hits;
			hits.n = 7;
			auto eng = make_tally_engine(hits);
			check_eq((int64_t)7, eng->execute("int n = hits; n").as_int());
			check_eq((int64_t)9, eng->execute("int add(int a_k) { return a_k + 2; } add(hits)").as_int());
			check_eq((int64_t)7, eng->execute("int give() { return hits; } give()").as_int());
			check_eq((int64_t)14, eng->execute("class Keep { int k = 0; static int s = 0; } Keep kept; kept.k = hits; Keep::s = hits; kept.k + Keep::s").as_int());
			check_eq((int64_t)7, eng->execute("Tally& r = hits; int z = r; z").as_int());
			check_eq((int64_t)0, hits.calls);
			std::string copied;
			try { eng->execute("auto c = hits;"); } catch (const std::exception& e) { copied = e.what(); }
			check_true(copied.find("Cannot copy C++ object of type 'Tally'") != std::string::npos, copied);
		});

		test("the_checker_warns_where_a_reflected_binding_is_missing", [this]() {
			auto eng = make_meter_engine();
			auto report = eng->check("Meter m = Meter(3); auto x = 2 * m; auto y = 2 - m;");
			check_eq((size_t)0, report.error_count());
			size_t warned = 0;
			for (const auto& d : report.diagnostics) {
				if (d.message.find("No bound 'int * Meter': bind operator \"*\" as (int, Meter) on Meter") != std::string::npos) { ++warned; }
				check_true(d.message.find("int - Meter") == std::string::npos, d.message);
			}
			check_eq((size_t)1, warned);
		});

		test("a_binder_binds_the_constructors_and_operators_its_class_declares", [this]() {
			using bound_operator_hosts::cents;
			using bound_operator_hosts::handle;
			auto eng = make_engine();
			stdlib::register_all(eng);
			dynamic_binder<cents>(*eng, "Cents").constructor<int64_t>().property("n", &cents::n);
			dynamic_binder<handle>(*eng, "Handle").property("id", &handle::id);
			eng->execute(R"(
				Cents a = Cents(12); Cents b = Cents(3);
				Cents copy = a;
				Cents zero = Cents();
				Cents sum = a + b; Cents product = a * b; Cents negative = -a;
				Cents counted = Cents(1); counted += b; ++counted;
			)");
			check_eq((int64_t)12, eng->execute("copy.n").as_int());
			check_eq((int64_t)0, eng->execute("zero.n").as_int());
			check_eq((int64_t)15, eng->execute("sum.n").as_int());
			check_eq((int64_t)36, eng->execute("product.n").as_int());
			check_eq((int64_t)-12, eng->execute("negative.n").as_int());
			check_eq((int64_t)5, eng->execute("counted.n").as_int());
			check_eq(true, eng->execute("a == Cents(12) && a != b && b < a && a >= b && !(a < b)").as_bool());
			check_eq((int64_t)21, eng->execute("a[0] * 10 + a[1]").as_int());
			check_eq((int64_t)36, eng->execute("a(3)").as_int());
			check_eq(false, eng->execute("reflect::has_method(a, \"to_string\")").as_bool());
			check_eq(false, eng->execute("Handle h = Handle(); h.id = 2; reflect::has_method(h, \"+\")").as_bool());
			// a script class over it assigns its own fields too: the base's = is hidden, as C++'s implicit operator= hides it
			check_eq((int64_t)37, eng->execute(R"(
				class Purse : Cents { int extra = 0; }
				var from = Purse(); from.n = 7; from.extra = 3;
				var into = Purse(); into = from;
				into.extra * 10 + into.n
			)").as_int());
		});

		test("an_explicit_binding_replaces_the_automatic_one", [this]() {
			using bound_operator_hosts::cents;
			auto eng = make_engine();
			stdlib::register_all(eng);
			dynamic_binder<cents>(*eng, "Cents")
				.constructor<int64_t>(jai::nodiscard)
				.property("n", &cents::n)
				.method("+", [](const cents& a_self, const cents& a_other) { return cents(a_self.n + a_other.n + 100); })
				.method("to_string", &cents::to_string);
			check_eq((int64_t)105, eng->execute("(Cents(2) + Cents(3)).n").as_int());
			check_eq(std::string("2c"), eng->execute("to_string(Cents(2))").as<std::string>());
			check_true(eng->check("Cents(4); int after = 1;").has_errors());
			check_false(eng->check("Cents kept = Cents(4);").has_errors());
		});

		test("a_script_operator_that_cannot_take_the_operand_yields_to_the_bound_base", [this]() {
			auto eng = make_bound_engine();
			eng->execute(R"(
				class Badge : Tag { Badge(int a_id) : Tag(a_id) {} override bool operator==(Badge o) { return false; } }
				var badge = Badge(4);
				var plain = Tag(4);
			)");
			check_eq(false, eng->execute("badge == Badge(4)").as_bool());
			check_eq(true, eng->execute("badge == plain").as_bool());
			check_eq(true, eng->execute("plain == badge").as_bool());
			check_eq(true, eng->execute("badge < Tag(5)").as_bool());
		});
	}
};

}

using bound_operator_tests = jai::foundry::tests::bound_operator_tests;
FOUNDRY_REGISTER(bound_operator_tests)
