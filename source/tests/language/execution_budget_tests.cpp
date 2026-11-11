#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"
#include <chrono>
#include <thread>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class execution_budget_tests : public suite {
public:
	execution_budget_tests() : suite("Execution Budget") {}

	void forge_tests() override {
		test("default_budget_is_five_seconds", [this]() {
			check_near(5.0, jai::engine::make()->execution_budget(), 1e-9);
		});

		test("budget_stops_every_loop_lane_and_the_engine_recovers", [this]() {
			const char* const loops[] = {
				"while (true) { }",
				"for (int i = 0; i < 2; i += 0) { }",
				// fuzz seed 33294: the counting loop's fall-through, taken once a reference binding boxes the counter, had no back-edge check
				"function bump(int& x) { x += 1; } for (int i = 0; i < 4; i++) { if (i != 0) { i = i - 1; } else { bump(i); i = 0; } }",
				"for (int i = 0; i < 4; i++) { if (i == 0) { auto f = [&]() { i = 0; }; f(); } i = i - 1; }",
				// no catch clause takes the budget's error
				"try { while (true) { } } catch (error e) { } catch (e) { }",
			};
			check_each(loops, [&](const char* loop) {
				auto e = make_engine();
				e->execution_budget(0.05);
				check_throws([&]() { e->execute(loop); });
				check_eq((int64_t)4, e->execute("2 + 2").as_int());
			});
		});

		test("a_direct_host_call_runs_under_the_budget_set_before_it", [this]() {
			// a host calling a script function with no script running is a host entry: the deadline is armed from the
			// budget current at the call (not the last execute's), the entry's latch resets, and the clock sample is written
			for (bool use_vm : {false, true}) {
				const std::string be = use_vm ? " (vm)" : " (interpreter)";
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
				e->add_function("nap", []() { std::this_thread::sleep_for(std::chrono::milliseconds(3)); });
				e->execution_budget(1000.0);
				e->execute("function run(int n) -> int { nap(); int acc = 0; for (int i = 0; i < n; i++) { acc += 1; } return acc; }\n"
				           "function idle() -> int { return 1; }");
				const auto run = e->get_variable("run").as_function();
				const std::vector<script_value> n{ script_value(static_cast<script_int>(20000), e.get()) };
				const uint64_t samples = detail::engine_access::clock_sample_count(*e);
				check_true(e->get_variable("idle").as_function()({}).has_value(), "the idle call runs" + be);
				check_true(detail::engine_access::clock_sample_count(*e) > samples, "a direct call's entry writes the clock sample" + be);
				e->execution_budget(0.0005);
				auto tripped = run(n);
				check_true(!tripped.has_value() && std::string(tripped.message()).find("Script execution budget exceeded") != std::string::npos,
				           "the 3 ms nap outlives the 0.5 ms budget set before the call" + be);
				e->execution_budget(1000.0);
				auto fresh = run(n);
				check_true(fresh.has_value() && fresh.value().as_int() == 20000, "the next call arms its own deadline" + be);
			}
		});

		test("zero_budget_disables_the_limit", [this]() {
			auto e = make_engine();
			e->execution_budget(0.0);
			auto r = e->execute("var x = 0; while (x < 200000) { x += 1; } x;");
			check_eq((int64_t)200000, r.as_int());
		});

		// Budget/timeout errors are TERMINAL - no script catch may
		// swallow them (catchable semantics would enable the FZ-BUDGET-LIVELOCK
		// denial-of-service: catch-and-retry loops keep execute() from ever returning).
		test("budget_error_is_terminal_not_catchable", [this]() {
			auto e = make_engine();
			e->execution_budget(0.05);
			bool threw = false;
			std::string msg;
			try {
				e->execute("var caught = 0; try { while (true) { } } catch (e) { caught = 1; } caught;");
			} catch (const std::exception& ex) {
				threw = true;
				msg = ex.what();
			}
			check_true(threw);
			check_true(msg.find("execution budget") != std::string::npos);
			check_eq((int64_t)4, e->execute("2 + 2").as_int());
		});

		// fuzz seeds 436/3507/8285/1213/1462/1788/8356/8391: a catch-and-retry loop around infinite work must return from execute()
		test("a_terminal_budget_error_skips_every_catch", [this]() {
			const char* const spins[] = {
				"while (true) { try { while (true) { } } catch (e) { } }",
				"try { try { while (true) { } } catch (a) { } } catch (b) { }",
				"void spin() { try { while (true) { } } catch (e) { } } spin();",
				"auto f = []() -> auto { try { while (true) { } } catch (e) { return 1; } return 2; }; f();",
				"coroutine int spin() { try { while (true) { } } catch (e) { yield 1; } return 0; } auto c = spin(); c.next();",
			};
			check_each(spins, [&](const char* spin) {
				auto e = make_engine();
				e->execution_budget(0.05);
				check_throws([&]() { e->execute(spin); });
				check_eq((int64_t)4, e->execute("2 + 2").as_int());
			});
		});

		// A terminal error in a REENTRANT execute (host callback calling execute()
		// mid-script) fails the inner execute AND propagates through the boundary:
		// the outer script's catch is skipped too, all the way to the host.
		test("budget_terminal_crosses_reentrant_execute_boundary", [this]() {
			auto e = make_engine();
			e->execution_budget(0.05);
			engine* raw = e.get();
			e->add_function("inner_spin", [raw]() -> script_int {
				raw->execute("while (true) { }");
				return 1;
			});
			check_throws([&]() {
				e->execute("var got = 0; try { got = inner_spin(); } catch (err) { got = -1; } got;");
			});
			check_eq((int64_t)4, e->execute("2 + 2").as_int());
		});

		test("budget_rearms_per_execute", [this]() {
			auto e = make_engine();
			e->execution_budget(0.05);
			check_throws([&]() { e->execute("while (true) { }"); });
			// A fresh execute gets a fresh deadline (not the already-expired one)
			e->execution_budget(jai::foundry::k_test_budget_seconds);
			auto r = e->execute("var x = 0; while (x < 10000) { x += 1; } x;");
			check_eq((int64_t)10000, r.as_int());
		});

		// ===== engine::memory_cap (approximate high-water accounting) =====

		test("memory_cap_defaults_to_zero_and_zero_is_unlimited", [this]() {
			auto e = make_engine();
			check_eq((size_t)0, e->memory_cap());
			e->memory_cap(65536);
			check_eq((size_t)65536, e->memory_cap());
			e->memory_cap(0);
			check_eq((size_t)0, e->memory_cap());
			auto r = e->execute(R"(
				var a = [];
				var n = 0;
				while (n < 20000) { a.push("xxxxxxxxxx"); n += 1; }
				a.size();
			)");
			check_eq((int64_t)20000, r.as_int());
		});

		// The first raise per execute is an ordinary catchable error, whatever grew: array pushes, a map's subscript auto-insert (raised
		// at the loop back-edge) and environment storage for names defined in fresh scopes
		test("memory_cap_first_raise_is_catchable_for_every_growth_kind", [this]() {
			struct growth_case { const char* code; size_t cap; };
			const growth_case growths[] = {
				{R"(
					var a = [];
					var caught = 0;
					try { while (true) { a.push("xxxxxxxxxx"); } } catch (err) { caught = 1; a = []; }
					var n = 0;
					while (n < 1000) { n += 1; }
					caught;
				)", 256 * 1024},
				{R"(
					var m = {};
					var i = 0;
					var caught = 0;
					try { while (true) { m[i] = i; i += 1; } } catch (err) { caught = 1; }
					caught;
				)", 128 * 1024},
				{R"(
					var caught = 0;
					try {
						while (true) { var x = [1]; }
					} catch (err) { caught = 1; }
					caught;
				)", 128 * 1024},
			};
			check_each(growths, [&](const growth_case& growth) {
				auto e = make_engine();
				e->memory_cap(growth.cap);
				check_eq((int64_t)1, e->execute(growth.code).as_int());
			});
		});

		test("memory_cap_error_text_names_the_cap", [this]() {
			auto e = make_engine();
			e->memory_cap(65536);
			auto r = e->execute(R"(
				var a = [];
				var msg = "";
				try { while (true) { a.push("xxxxxxxxxx"); } } catch (err) { msg = err; }
				msg;
			)");
			check_eq(std::string("Script memory cap exceeded (65536 bytes) - raise engine::memory_cap or break up the work"),
				r.as<std::string>());
		});

		// ONE script catch per execute; the SECOND raise in the same
		// execute is TERMINAL (Part-1 machinery - every catch skipped, execute fails).
		test("memory_cap_second_raise_is_terminal", [this]() {
			auto e = make_engine();
			e->memory_cap(256 * 1024);
			bool threw = false;
			std::string msg;
			try {
				e->execute(R"(
					var a = [];
					var caught = 0;
					try { while (true) { a.push("xxxxxxxxxx"); } } catch (err) { caught = 1; a = []; }
					var b = [];
					try { while (true) { b.push("xxxxxxxxxx"); } } catch (err2) { caught = 9; }
					caught;
				)");
			} catch (const std::exception& ex) {
				threw = true;
				msg = ex.what();
			}
			check_true(threw);
			check_true(msg.find("memory cap exceeded") != std::string::npos);
			check_eq((int64_t)4, e->execute("2 + 2").as_int());
		});

		test("memory_cap_resets_per_execute", [this]() {
			auto e = make_engine();
			e->memory_cap(256 * 1024);
			check_throws([&]() {
				e->execute(R"(
					var a = [];
					try { while (true) { a.push("xxxxxxxxxx"); } } catch (e1) { }
					var b = [];
					try { while (true) { b.push("xxxxxxxxxx"); } } catch (e2) { }
				)");
			});
			// Fresh execute: fresh allowance and raise count
			auto r = e->execute(R"(
				var a = [];
				var caught = 0;
				try { while (true) { a.push("xxxxxxxxxx"); } } catch (err) { caught = 1; }
				caught;
			)");
			check_eq((int64_t)1, r.as_int());
		});

		// Every builtin that grows a value past its inputs is a chokepoint: the growth is denied before it exists
		test("memory_cap_charges_every_growing_builtin", [this]() {
			const std::string cap_text = "Script memory cap exceeded (1048576 bytes) - raise engine::memory_cap or break up the work";
			const char* growths[] = {
				"string s = \"x\"; s.pad_left(64000000);",
				"string s = \"x\"; s.pad_right(64000000, \"ab\");",
				"string s = \"x\"; s.pad_center(64000000);",
				"utf8_string u = utf8_string(\"x\"); u.pad_left(64000000);",
				"string s = string::pad_left(\"x\", 64000000);",
				"string s = string::repeat(\"x\", 64000000);",
				"string s = \"abcd\"; s.repeat(4611686018427387905);",
				"string s = \"x\"; s.repeat(500000); s.append(s, s, s);",
				"string s = \"x\"; s.repeat(500000); string t = string::append(s, s, s);",
				"var a = [1]; for (int i = 0; i < 30; i++) { a.concat(a); }",
				"var a = [1]; for (int i = 0; i < 30; i++) { a.append(a); }",
				"var a = [\"a\", \"b\", \"c\"]; string sep = \"x\"; sep.repeat(500000); a.join(sep);",
				"var a = [\"a\", \"b\", \"c\"]; string sep = \"x\"; sep.repeat(500000); string::join(a, sep);",
				"string s = \"x\"; s.repeat(500000); s.replace(\"x\", \"yyyy\");",
				"string s = \"x\"; s.repeat(500000); string::replace(s, \"x\", \"yyyy\");",
				"string s = \"x\"; s.repeat(500000); var parts = s.split(\"\");",
				"string s = \"x\"; s.repeat(500000); var parts = s.utf8_chars();",
				"string s = \"x\\n\"; s.repeat(300000); var parts = s.lines();",
				"var w = binary::writer(); w.text(\"x\", 64000000);",
				"var w = binary::writer(); for (int i = 0; i < 1000000; i++) { w.int64(i); }",
				"var deep = [[[[[[1]]]]]]; to_json(deep, 10000000);",
			};
			for (const char* growth : growths) {
				auto e = make_engine();
				stdlib::register_all(*e);
				e->memory_cap(1024 * 1024);
				std::string caught;
				try {
					auto r = e->execute(std::string("string msg = \"none\"; try { [[warn_mode]]; ") + growth + " } catch (err) { msg = err; } msg");
					caught = r.as<std::string>();
				} catch (const std::exception& ex) {
					caught = std::string("escaped: ") + ex.what();
				}
				check_eq(cap_text, caught, growth);
				check_eq((int64_t)4, e->execute("2 + 2").as_int(), std::string("engine usable after ") + growth);
			}
		});

		// A suspended coroutine keeps its state (the vm's fiber stack, the interpreter's frames): starting one counts against
		// the cap, so a script cannot park unbounded coroutines under it
		test("memory_cap_counts_suspended_coroutines", [this]() {
			auto e = make_engine();
			e->memory_cap(1024 * 1024);
			auto r = e->execute(R"(
				coroutine int ticker() { int i = 0; while (true) { yield i; i += 1; } return 0; }
				var parked = [];
				string msg = "none";
				try { for (int i = 0; i < 20000; i++) { var h = ticker(); h.next(); parked.push(h); } } catch (err) { msg = err; }
				msg
			)");
			check_eq(std::string("Script memory cap exceeded (1048576 bytes) - raise engine::memory_cap or break up the work"), r.as<std::string>());
			check_eq((int64_t)4, e->execute("2 + 2").as_int());
		});

		// A repeat whose byte count passes size_t reports like any oversized text instead of reserving the wrapped size and
		// appending forever
		test("an_overflowing_repeat_reports_at_once", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			for (const char* source : {"string s = \"abcd\"; s.repeat(4611686018427387905);", "string s = string::repeat(\"abcd\", 4611686018427387905);"}) {
				check_throws([&]() { e->execute(source); }, source);
			}
			check_eq((int64_t)4, e->execute("2 + 2").as_int());
		});

		// replace builds its result in one pass (replacing in place shifted the tail at every hit: four million hits would run
		// for hours, out of the budget's reach inside one builtin, which the watchdog reads as a hang)
		test("replace_is_linear_in_the_hits", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			check_eq((int64_t)8000000, e->execute("string s = \"x\"; s.repeat(4000000); s.replace(\"x\", \"yy\"); s.size()").as_int());
			check_eq((int64_t)8000000, e->execute("string s = \"x\"; s.repeat(4000000); string::replace(s, \"x\", \"yy\").size()").as_int());
			check_eq(std::string("a-b-c"), e->execute("string s = \"a, b, c\"; s.replace(\", \", \"-\"); s").as<std::string>());
			check_eq(std::string("xxyxxy"), e->execute("string s = \"xyxy\"; s.replace(\"x\", \"xx\"); s").as<std::string>());
			check_eq(std::string("q"), e->execute("string s = \"aaaa\"; s.replace(\"aa\", \"\"); s + \"q\"").as<std::string>());
		});
	}
};

} // namespace jai::foundry::tests

using execution_budget_tests = jai::foundry::tests::execution_budget_tests;
FOUNDRY_REGISTER(execution_budget_tests)
