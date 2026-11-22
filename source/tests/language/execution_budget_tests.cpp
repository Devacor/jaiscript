#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/coroutine.hpp>
#include <jaiscript/core/script_run.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/detail/host_time.hpp>
#include "../case_table.hpp"
#include <chrono>
#include <thread>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

namespace {
	// Time passing on the calling thread without a wait: the running entry's deadline nears by a_seconds, and the coarse
	// clock that times host calls runs ahead by the same
	void pass_time(engine& a_engine, double a_seconds) {
		const auto span = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(a_seconds));
		if (execution_backend* backend = detail::engine_access::calling_backend(a_engine)) { backend->extend_execution_deadline(-span); }
		detail::engine_access::execution_limits(a_engine).host_tick_skew += static_cast<uint64_t>(span.count() / detail::k_host_tick_nanoseconds);
	}

	struct slow_loader {
		engine* eng = nullptr;
		script_int loaded = 0;
		slow_loader() = default;
		explicit slow_loader(engine* a_engine) : eng(a_engine) {}
		slow_loader(double a_seconds, const slow_loader& a_like) : eng(a_like.eng) { pass_time(*eng, a_seconds); }
		slow_loader(const slow_loader& a_like, double a_seconds) : eng(a_like.eng) { pass_time(*eng, a_seconds); }
		void load(double a_seconds) { pass_time(*eng, a_seconds); ++loaded; }
		script_int size() const { pass_time(*eng, 5000.0); return loaded; }
		void resize(script_int a_size) { pass_time(*eng, 5000.0); loaded = a_size; }
	};

	// Host code in every bound shape that takes options, each call passing the seconds it is given (5000 where it takes
	// none): the host_ and marked names say jai::exclude_execution_time, the counted_ and plain ones are the same bodies
	// bound without it. Each two-overload name (mixed, Loader.pick, the Loader constructor) marks one overload only.
	void bind_slow_host(engine& a_engine) {
		engine* eng = &a_engine;
		const auto work = [eng](double a_seconds) { pass_time(*eng, a_seconds); };
		const auto work_long = [eng]() { pass_time(*eng, 5000.0); };
		const auto work_any = [eng](const std::vector<script_value>& a_args) -> checked_result<script_value> {
			pass_time(*eng, a_args.empty() ? 5000.0 : a_args[0].deref().as_float());
			return script_value(std::monostate{}, eng);
		};
		const auto work_any_engine = [](engine& a_owner, std::span<const script_value> a_args) -> script_value {
			pass_time(a_owner, a_args.empty() ? 5000.0 : a_args[0].deref().as_float());
			return script_value(std::monostate{}, &a_owner);
		};
		a_engine.add_function("host_work", work, jai::exclude_execution_time);
		a_engine.add_function("counted_work", work);
		a_engine.add_function("host_work_long", work_long, jai::exclude_execution_time);
		a_engine.add_function("counted_work_long", work_long);
		a_engine.add_variadic_function("host_work_any", script_function(work_any), jai::exclude_execution_time);
		a_engine.add_variadic_function("counted_work_any", script_function(work_any));
		a_engine.add_variadic_function("host_work_spanned", work_any_engine, jai::exclude_execution_time);
		a_engine.add_variadic_function("counted_work_spanned", work_any_engine);
		a_engine.add_function("mixed", [eng](script_int) { pass_time(*eng, 5000.0); }, jai::exclude_execution_time);
		a_engine.add_function("mixed", [eng](const std::string&) { pass_time(*eng, 5000.0); });
		a_engine.add_function("host_calls", [eng](double a_before, const std::string& a_callback, double a_after) {
			pass_time(*eng, a_before);
			auto called = eng->get_variable(a_callback).as_function()({});
			pass_time(*eng, a_after);
			return called.has_value();
		}, jai::exclude_execution_time);
		a_engine.add_function("host_runs", [eng](double a_before, const script_value& a_callback, double a_after) {
			pass_time(*eng, a_before);
			auto ran = a_callback.as_function()({});
			pass_time(*eng, a_after);
			return ran.has_value();
		}, jai::exclude_execution_time);
		a_engine.add_function("host_sorts", [eng](std::vector<script_int> a_values, const script_value& a_before) {
			pass_time(*eng, 5000.0);
			for (size_t pass = 0; pass + 1 < a_values.size(); ++pass) {
				for (size_t i = 0; i + 1 < a_values.size() - pass; ++i) {
					auto before = a_before.as_function()({ script_value(a_values[i + 1], eng), script_value(a_values[i], eng) });
					if (!before) { return script_int(-1); }
					if (before.value().as_bool()) { std::swap(a_values[i], a_values[i + 1]); }
				}
			}
			pass_time(*eng, 5000.0);
			return a_values.empty() ? script_int(0) : a_values.front();
		}, jai::exclude_execution_time);
		dynamic_binder<slow_loader>(a_engine, "Loader")
			.constructor<double, const slow_loader&>(jai::exclude_execution_time)
			.constructor<const slow_loader&, double>()
			.method("load", &slow_loader::load, jai::exclude_execution_time)
			.method("counted_load", &slow_loader::load)
			.method("load_through", [](slow_loader& a_self, double a_seconds) { a_self.load(a_seconds); }, jai::exclude_execution_time)
			.method("counted_load_through", [](slow_loader& a_self, double a_seconds) { a_self.load(a_seconds); })
			.method("pick", [](slow_loader& a_self, script_int) { pass_time(*a_self.eng, 5000.0); }, jai::exclude_execution_time)
			.method("pick", [](slow_loader& a_self, const std::string&) { pass_time(*a_self.eng, 5000.0); })
			.method("+", [](slow_loader& a_self, const slow_loader& a_other) { pass_time(*a_self.eng, 5000.0); return a_self.loaded + a_other.loaded; }, jai::exclude_execution_time)
			.method("-", [](slow_loader& a_self, const slow_loader& a_other) { pass_time(*a_self.eng, 5000.0); return a_self.loaded - a_other.loaded; })
			.property("size", &slow_loader::size, &slow_loader::resize)
			.static_method("warm", work, jai::exclude_execution_time)
			.static_method("counted_warm", work)
			.build();
		a_engine.add_global("loader", std::make_shared<slow_loader>(eng));
	}
}

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

		// ===== the budget inside the engine's own builtins (detail/work_budget.hpp) =====

		// One builtin call is no back-edge and no call entry: its own work count ends it. Each row is one statement alone in an
		// execute under a step budget its work passes several times over (`stopped` reads what the stop left); the same
		// statement then runs to its end with the budget off (`done`).
		test("a_long_builtin_ends_at_the_budget_inside_its_own_run", [this]() {
			struct long_builtin { const char* setup; const char* code; const char* probe; const char* stopped; const char* done; };
			const char* const ints = "array<int> a = array<int>(400000, 7); int n = 0;";
			const char* const text = "string s = \"ab,\"; s.repeat(300000); int n = 0;";
			const char* const parts = "string s = \"ab,\"; s.repeat(300000); var parts = s.split(\",\"); int n = 0;";
			const char* const entries = "var m = {}; for (int i = 0; i < 150000; i++) { m[i] = i; } int n = 0;";
			const char* const mixed = "var v = []; for (int i = 0; i < 60000; i++) { v.push(60000 - i); } int n = 0;";
			const char* const numbers = "var v = []; for (int i = 0; i < 100000; i++) { v.push(i); } string packed = to_binary(v); int n = 0;";
			const long_builtin rows[] = {
				{"int n = 0;", "{ array<int> a = array<int>(400000, 7); n = a.size(); }", "n", "0", "400000"},
				{"int n = 0;", "try { array<int> a = array<int>(400000, 7); n = a.size(); } catch (error e) { n = -1; } catch (e) { n = -2; }", "n", "0", "400000"},
				{"array<int> a = [1];", "a.resize(400000, 7);", "a.size()", "1", "400000"},
				{ints, "a.fill(9);", "to_string(a[0]) + \",\" + to_string(a[399999])", "9,7", "9,9"},
				{ints, "a.assign(400000, 3);", "to_string(a[0]) + \",\" + to_string(a[399999])", "3,7", "3,3"},
				{ints, "{ var b = array::fill(a, 9); n = b.size(); }", "to_string(n) + \",\" + to_string(a[0])", "0,7", "400000,7"},
				{ints, "{ var b = array::slice(a, 0, 400000); n = b.size(); }", "n", "0", "400000"},
				{ints, "n = a.index_of(8);", "n", "0", "-1"},
				{"array<int> a = array<int>(1200000, 7); int n = 0;", "{ var b = a; b.push(1); n = b.size(); }", "n", "0", "1200001"},
				{"var h = []; for (int i = 0; i < 100000; i++) { h.push([i, i]); } int n = 0;", "{ var c = h; c.push(1); n = c.size(); }", "n", "0", "100001"},
				{"array<int> a = array<int>(400000, 7); a[0] = 9;", "a.sort();", "to_string(a.size()) + \",\" + to_string(a.index_of(9) >= 0)", "400000,true", "400000,true"},
				{"array<string> a = array<string>(60000, \"b\"); a[0] = \"c\";", "a.sort();", "to_string(a.size()) + \",\" + to_string(a.index_of(\"c\") >= 0)", "60000,true", "60000,true"},
				{mixed, "v.sort();", "v[0]", "60000", "1"},
				{mixed, "{ var w = array::sort(v); n = w[0]; }", "to_string(n) + \",\" + to_string(v[0])", "0,60000", "1,60000"},
				{"string s = \"0123456789abcdef\";", "s.repeat(300000);", "s.size()", "16", "4800000"},
				{"string s = \"0123456789abcdef\"; int n = 0;", "n = string::repeat(s, 300000).size();", "n", "0", "4800000"},
				{parts, "n = parts.join(\"-\").size();", "n", "0", "900000"},
				{parts, "n = string::join(parts, \"-\").size();", "n", "0", "900000"},
				{text, "{ var pieces = s.split(\",\"); n = pieces.size(); }", "n", "0", "300001"},
				{text, "s.replace(\",\", \";\");", "s.count(\";\")", "0", "300000"},
				{text, "n = string::replace(s, \",\", \";;\").size();", "n", "0", "1200000"},
				{text, "n = s.count(\",\");", "n", "0", "300000"},
				{text, "{ var characters = s.utf8_chars(); n = characters.size(); }", "n", "0", "900000"},
				{"string s = \"ab\\n\"; s.repeat(300000); int n = 0;", "{ var each = s.lines(); n = each.size(); }", "n", "0", "300000"},
				{"string s = \"ab,\"; s.repeat(2000000); var re = regex(\"z\"); int n = 0;", "{ var found = re.search(s); n = 1; }", "n", "0", "1"},
				{"string s = \"ab,\"; s.repeat(300000); var re = regex(\",\"); int n = 0;", "n = re.replace(s, \"\").size();", "n", "0", "600000"},
				{ints, "n = to_json(a).size();", "n", "0", "800001"},
				{"string j = \"7,\"; j.repeat(300000); j = \"[\" + j + \"7]\"; int n = 0;", "{ var v = from_json(j); n = v.size(); }", "n", "0", "300001"},
				{numbers, "n = to_binary(v).size() > 100000 ? 1 : 2;", "n", "0", "1"},
				{numbers, "{ var back = from_binary(packed); n = back.size(); }", "n", "0", "100000"},
				{entries, "{ var k = m.keys(); n = k.size(); }", "n", "0", "150000"},
				{entries, "{ var pairs = m.to_array(); n = pairs.size(); }", "n", "0", "150000"},
				{entries, "{ var c = m; c[-1] = 0; n = c.size(); }", "n", "0", "150001"},
				{entries, "{ var both = map::merge(m, m); n = both.size(); }", "n", "0", "150000"},
			};
			const std::string spent = "Script execution budget exceeded - raise engine::execution_step_budget or break up the work";
			check_each(rows, [&](const long_builtin& row) {
				auto e = make_engine();
				stdlib::register_all(*e);
				e->execute(row.setup);
				e->execution_step_budget(1);
				std::string raised = "returned normally";
				try { e->execute(row.code); } catch (const std::exception& error) { raised = error.what(); }
				check_true(raised.find(spent) != std::string::npos, "the budget ends it: " + raised);
				e->execution_step_budget(0);
				check_eq(std::string(row.stopped), e->execute(row.probe).to_string(), "what the stop left");
				e->execute(row.code);
				check_eq(std::string(row.done), e->execute(row.probe).to_string(), "the same statement with the budget off");
			});
		});

		// A host callable is no call entry either: a walk that calls one counts its own elements, so the calls stop short
		test("a_walk_over_a_host_callback_stops_at_the_budget", [this]() {
			struct walk { const char* code; size_t most; };
			const walk walks[] = {
				{"a.for_each(tally);", 400000},
				{"array::map(a, tally);", 400000},
				{"a.sort(before);", 400000 * 18},
			};
			check_each(walks, [&](const walk& row) {
				auto e = make_engine();
				size_t calls = 0;
				e->add_function("tally", [&calls](script_int a_value) -> script_int { ++calls; return a_value; });
				e->add_function("before", [&calls](script_int a_left, script_int a_right) -> bool { ++calls; return a_left < a_right; });
				e->execute("var a = array<int>(400000, 7);");
				e->execution_step_budget(1);
				check_throws([&]() { e->execute(row.code); });
				check_true(calls > 0 && calls < row.most, std::to_string(calls) + " calls");
				check_eq((int64_t)4, e->execute("2 + 2").as_int());
			});
		});

		test("a_short_builtin_is_no_budget_check", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			e->execution_step_budget(1);
			check_eq((int64_t)1018, e->execute(R"(
				array<int> a = array<int>(1000, 7);
				a.sort(); a.fill(1); a.reverse();
				var b = a; b.push(2);
				string s = "x"; s.repeat(100);
				var parts = s.split("");
				a.size() + to_json(a).size() / 1000 + parts.join(",").size() / 14 + from_json("[1, 2]").size() - b.size() + 1001
			)").as_int());
		});

		// An entry that outran its budget in work no check sampled (a host function, with too few back-edges after it to reach a
		// sample) hands the host the budget's error, never a success: an execute, a jaibite, a function the host calls, a
		// coroutine the host steps and a suspendable run's slice alike
		test("an_entry_that_outran_its_budget_never_returns_success", [this]() {
			const std::string spent = "Script execution budget exceeded - raise engine::execution_budget or break up the work";
			for (bool use_vm : {false, true}) {
				const std::string be = use_vm ? " (vm)" : " (interpreter)";
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
				e->add_function("nap", []() { std::this_thread::sleep_for(std::chrono::milliseconds(3)); });
				e->execution_budget(1000.0);
				e->execute("function quick() -> int { nap(); return 1; }\n"
				           "coroutine int steps() { nap(); yield 1; return 2; }\n"
				           "int after = 0; var h = steps();");
				auto bite = e->jaibite("nap(); 1;");
				const auto quick = e->get_variable("quick").as_function();
				auto handle = std::static_pointer_cast<coroutine_handle>(e->execute("h").get_object_holder()->host_object());
				const auto thrown = [&](auto&& a_run) {
					try { a_run(); } catch (const std::exception& error) { return std::string(error.what()); }
					return std::string("returned normally");
				};
				e->execution_budget(0.0005);
				check_eq(spent, thrown([&]() { e->execute("nap();"); }), "an execute that is one host call" + be);
				check_eq(spent, thrown([&]() { e->execute("nap(); after = after + 1; after = after + 1;"); }), "a few statements after it" + be);
				check_eq(spent, thrown([&]() { e->execute("try { nap(); } catch (error e) { } catch (e) { }"); }), "no catch stands between it and the host" + be);
				check_eq(spent, thrown([&]() { e->execute(bite); }), "a jaibite" + be);
				auto called = quick({});
				check_true(!called.has_value() && std::string(called.message()) == spent, "a function the host calls" + be);
				auto stepped = handle->next(e.get());
				check_true(!stepped.has_value() && std::string(stepped.message()) == spent, "a coroutine the host steps" + be);
				check_eq(spent, thrown([&]() { script_run run = e->start("nap(); sleep(0.001); 1;"); }), "a suspendable run's slice" + be);
				e->execution_budget(1000.0);
				check_eq((int64_t)2, e->execute("after").as_int(), "the statements ran" + be);
				check_eq((int64_t)1, e->execute("nap(); quick()").as_int(), "a budget the entry keeps to" + be);
				check_true(quick({}).has_value(), "and the next call" + be);
			}
		});

		// A parallel region's chunks count their builtins' work against the budget the region's entry has left
		test("a_parallel_body_of_long_builtins_ends_at_the_budget", [this]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			e->parallel_thread_count(4);
			e->execute("array<int> big = array<int>(4000000, 7); int probe(int x) { return big.index_of(8) + x; } var a = array<int>(64, 1);");
			e->execution_budget(0.002);
			std::string raised = "returned normally";
			try { e->execute("var out = parallel_transform(a, probe); out.size()"); } catch (const std::exception& error) { raised = error.what(); }
			check_true(raised.find("Script execution budget exceeded - raise engine::execution_budget or break up the work") != std::string::npos, raised);
			e->execution_budget(jai::foundry::k_test_budget_seconds);
			check_eq((int64_t)4, e->execute("2 + 2").as_int());
		});

		// ===== jai::exclude_execution_time (detail/host_time.hpp) =====

		// The clock counts a host's bound code like the script's own, except a binding marked jai::exclude_execution_time. Each
		// row runs alone under a 1000 s budget, its calls passing 5000 s: a marked binding leaves the budget whole in every
		// shape that takes options, the same body bound without the option spends it, and an overload is timed by its own
		// marking
		test("a_binding_marked_exclude_execution_time_is_not_counted", [this]() {
			struct host_time_case { const char* code; bool within; };
			const host_time_case rows[] = {
				{"host_work(5000.0);", true},
				{"counted_work(5000.0);", false},
				{"host_work_long();", true},
				{"counted_work_long();", false},
				{"host_work_any(5000.0);", true},
				{"counted_work_any(5000.0);", false},
				{"host_work_spanned(5000.0);", true},
				{"counted_work_spanned(5000.0);", false},
				{"var made = Loader(5000.0, loader);", true},
				{"var made = Loader(loader, 5000.0);", false},
				{"loader.load(5000.0);", true},
				{"loader.counted_load(5000.0);", false},
				{"loader.load_through(5000.0);", true},
				{"loader.counted_load_through(5000.0);", false},
				{"Loader::warm(5000.0);", true},
				{"Loader::counted_warm(5000.0);", false},
				{"var sum = loader + loader;", true},
				{"var difference = loader - loader;", false},
				{"var size = loader.size;", false},
				{"loader.size = 3;", false},
				{"mixed(1);", true},
				{"mixed(\"one\");", false},
				{"loader.pick(1);", true},
				{"loader.pick(\"one\");", false},
				{"try { host_work(5000.0); } catch (e) { }", true},
				{"host_work(5000.0); counted_work(900.0);", true},
				{"host_work(5000.0); counted_work(1100.0);", false},
				{"for (int i = 0; i < 300; i++) { host_work(0.0); counted_work(5.0); }", false},
				{"for (int i = 0; i < 300; i++) { host_work(50.0); loader.load(50.0); counted_work(2.0); }", true},
				// script a marked call runs is counted; the call's own time on either side of it is credited once
				{"host_calls(5000.0, \"idle\", 5000.0);", true},
				{"host_calls(5000.0, \"idle\", 5000.0); counted_work(900.0);", true},
				{"host_calls(5000.0, \"idle\", 5000.0); counted_work(1100.0);", false},
				{"host_calls(5000.0, \"hosted\", 5000.0); counted_work(900.0);", true},
				{"host_calls(5000.0, \"working\", 5000.0); counted_work(500.0);", true},
				{"host_calls(5000.0, \"working\", 5000.0); counted_work(700.0);", false},
				{"host_calls(5000.0, \"runaway\", 5000.0);", false},
				// no script buys time through a marked call: a lambda handed to it, a function value it calls and an operator it
				// triggers are counted, each ending at the budget when it runs away
				{"host_runs(5000.0, []() { idle(); }, 5000.0); counted_work(900.0);", true},
				{"host_runs(5000.0, idle, 5000.0); counted_work(1100.0);", false},
				{"host_runs(5000.0, []() { counted_work(2000.0); idle(); }, 5000.0);", false},
				{"host_runs(5000.0, []() { counted_work(400.0); idle(); }, 5000.0); counted_work(700.0);", false},
				{"host_runs(5000.0, runaway, 5000.0);", false},
				{"host_calls(5000.0, \"Still\", 5000.0); counted_work(800.0);", true},
				{"host_calls(5000.0, \"Spinner\", 5000.0);", false},
				{"host_sorts([3, 1, 2], [](int a, int b) { counted_work(100.0); idle(); return a < b; }); counted_work(600.0);", true},
				{"host_sorts([3, 1, 2], [](int a, int b) { counted_work(100.0); idle(); return a < b; }); counted_work(800.0);", false},
				{"host_sorts([3, 1, 2], [](int a, int b) { counted_work(400.0); idle(); return a < b; });", false},
			};
			const std::string spent = "Script execution budget exceeded - raise engine::execution_budget or break up the work";
			for (bool use_vm : {false, true}) {
				const std::string be = use_vm ? " (vm)" : " (interpreter)";
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
				bind_slow_host(*e);
				e->execute("function idle() { for (int i = 0; i < 3000; i++) { } }\n"
				           "function hosted() { host_work(5000.0); loader.load(5000.0); idle(); }\n"
				           "function working() { counted_work(400.0); idle(); }\n"
				           "function runaway() { counted_work(2000.0); idle(); }\n"
				           "class Still { Still() { counted_work(100.0); } }\n"
				           "class Spinner { Spinner() { counted_work(2000.0); } }");
				e->execution_budget(1000.0);
				check_each(rows, [&](const host_time_case& row) {
					std::string raised = "returned normally";
					try { e->execute(row.code); } catch (const std::exception& error) { raised = error.what(); }
					check_eq(row.within ? std::string("returned normally") : spent, raised, row.code + be);
				});
			}
		});

		// What a marked call is credited is what it took; an unmarked one is credited nothing, and neither is without a
		// wall-clock budget to move
		test("excluded_time_is_credited_only_against_an_armed_budget", [this]() {
			for (bool use_vm : {false, true}) {
				const std::string be = use_vm ? " (vm)" : " (interpreter)";
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
				bind_slow_host(*e);
				const auto credited_seconds = [&](const char* a_code) {
					try { e->execute(a_code); } catch (const std::exception&) {}
					return std::chrono::duration<double>(detail::host_ticks_span(detail::engine_access::execution_limits(*e).host_ticks_credited)).count();
				};
				e->execute("function host_named() { counted_work(2000.0); for (int i = 0; i < 3000; i++) { } }");
				e->execution_budget(1000.0);
				// a script function is counted however it is reached: the host calling its value is an entry like any other
				auto held = e->get_variable("host_named").as_function()({});
				check_true(!held.has_value() && std::string(held.message()).find("Script execution budget exceeded") != std::string::npos, "a script function the host calls" + be);
				check_eq(uint64_t(0), detail::engine_access::execution_limits(*e).host_ticks_credited, "and nothing is credited for it" + be);
				check_true(std::abs(credited_seconds("host_work(5000.0);") - 5000.0) < 1.0, "a function" + be);
				check_true(std::abs(credited_seconds("loader.load(5000.0); Loader::warm(5000.0);") - 10000.0) < 1.0, "a method and a static" + be);
				check_eq(0.0, credited_seconds("counted_work(100.0); loader.counted_load(100.0);"), "unmarked bindings" + be);
				check_eq(0.0, credited_seconds("int n = 0; for (int i = 0; i < 3000; i++) { n = i; }"), "script alone" + be);
				e->execution_budget(0.0);
				check_eq(0.0, credited_seconds("host_work(5000.0); loader.load(5000.0);"), "no budget armed" + be);
				e->execution_step_budget(100000);
				check_eq(0.0, credited_seconds("host_work(5000.0); loader.load(5000.0);"), "a step budget alone" + be);
			}
		});

		// A parallel region's workers each credit their own deadline, and the region's caller is credited the least any of them was
		test("a_parallel_body_in_a_marked_binding_is_not_counted", [this]() {
			auto e = make_engine();
			bind_slow_host(*e);
			e->parallel_thread_count(4);
			e->execute("int slow(int x) { host_work(5000.0); for (int i = 0; i < 3000; i++) { } return x + 1; }\n"
			           "int spin(int x) { counted_work(5000.0); for (int i = 0; i < 3000; i++) { } return x; } var a = array<int>(64, 1);");
			e->execution_budget(1000.0);
			check_eq((int64_t)64, e->execute("var out = parallel_transform(a, slow); out.size()").as_int());
			check_true(detail::engine_access::execution_limits(*e).host_ticks_credited > 0, "the caller's entry is credited");
			check_throws([&]() { e->execute("var out = parallel_transform(a, spin); out.size()"); });
			check_eq((int64_t)4, e->execute("2 + 2").as_int());
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
