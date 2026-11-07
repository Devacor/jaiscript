#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"
#include <functional>
#include <memory>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

struct capture_probe { int v = 7; int value() const { return v; } };

// A host object that calls a stored script callback from its methods and its destructor
struct reentry_host {
	std::function<void()> cb;
	std::function<void()> on_death;
	std::string name = "alive";
	int* deaths = nullptr;
	~reentry_host() {
		name = "dead";
		if (deaths) { ++*deaths; }
		if (on_death) { on_death(); }
	}
	void call() { if (cb) { auto local = cb; local(); } }   // a local copy: the callback may end this object
	void set(script_value f) { cb = convert_script_function<void()>(f); }
	void set_death(script_value f) { on_death = convert_script_function<void()>(f); }
	script_value hold(const script_value& v) { call(); return v; }
	std::string run() { call(); return name; }
};

// A frame window of ~50 slots, recursed deep enough to move the vm's value stack several times (and within the
// interpreter's call-depth cap)
inline std::string stack_moving_source() {
	std::string s = "function stack_deep(int n) -> int { ";
	for (int i = 0; i < 48; ++i) { s += "int l" + std::to_string(i) + " = n + " + std::to_string(i) + "; "; }
	return s + "if (n <= 0) { return l47; } return stack_deep(n - 1) + l0 - n; }";
}

// A host eval shape: a converted callback runs a nested execute() while the outer execute() is live.
class reentrant_callback_tests : public suite {
public:
	reentrant_callback_tests() : suite("Reentrant Callbacks") {}

	std::shared_ptr<engine> reentry_engine(int* deaths = nullptr) {
		auto eng = make_engine();
		stdlib::register_all(*eng);
		dynamic_binder<reentry_host>(*eng, "ReentryHost")
			.method("set", &reentry_host::set)
			.method("set_death", &reentry_host::set_death)
			.method("hold", &reentry_host::hold)
			.method("run", &reentry_host::run)
			.method("poke", [](reentry_host& h) -> reentry_host& { h.call(); return h; })
			.build();
		engine* raw = eng.get();
		eng->add_function("make_host", [raw, deaths]() {
			auto h = std::make_shared<reentry_host>();
			h->deaths = deaths;
			return raw->make_object(h);
		});
		eng->execute(stack_moving_source());
		return eng;
	}

	// A script callback run from inside a host call or a release moves the vm's value stack; nothing the host call or
	// the release still holds may point into it
	void stack_moving_reentry() {
		test("a_host_methods_reference_arguments_survive_a_callback_that_moves_the_stack", [this]() {
			auto eng = reentry_engine();
			auto r = eng->execute(R"(
				var h = make_host();
				h.set([]() -> void { stack_deep(150); });
				var got = h.hold([10, 20, 30]);
				var text = h.hold("text " + to_string(7));
				var chained = h.poke().hold(5);
				to_string(got[2]) + "|" + text + "|" + to_string(chained);
			)");
			check_eq(std::string("30|text 7|5"), r.to_string());
		});

		test("a_host_methods_receiver_outlives_a_callback_that_drops_it", [this]() {
			int deaths = 0;
			auto eng = reentry_engine(&deaths);
			auto r = eng->execute(R"(
				function local_receiver() -> string {
					var mine = make_host();
					mine.set([&]() -> void { mine = null; stack_deep(150); });
					return mine.run();
				}
				var h = make_host();
				h.set([]() -> void { h = null; stack_deep(150); });
				h.run() + "|" + local_receiver();
			)");
			check_eq(std::string("alive|alive"), r.to_string(), "the call keeps its receiver alive");
			check_eq(2, deaths, "each host object died once its call ended");
		});

		test("a_host_destructor_that_runs_script_leaves_the_frame_it_unwinds_intact", [this]() {
			int deaths = 0;
			auto eng = reentry_engine(&deaths);
			auto r = eng->execute(R"(
				function scoped() -> int {
					var d = make_host();
					d.set_death([]() -> void { stack_deep(150); });
					var a1 = [1]; var e = make_host(); var a2 = [2, 2]; var a3 = "three " + to_string(3);
					return a1.size() + a2.size() + a3.size();
				}
				var total = 0;
				for (int i = 0; i < 3; i++) { total += scoped(); }
				total;
			)");
			check_eq((int64_t)30, r.as_int());
			check_eq(6, deaths, "every host object in the frame died with it");
		});

		test("a_store_whose_old_value_runs_a_destructor_that_moves_the_stack_lands", [this]() {
			int deaths = 0;
			auto eng = reentry_engine(&deaths);
			auto r = eng->execute(R"(
				class Grower { ~Grower() { stack_deep(150); } }
				function over_script() -> string {
					var g = Grower();
					var keep = [1, 2, 3];
					g = 7;
					return to_string(g) + ":" + to_string(keep[2]);
				}
				function over_host() -> string {
					var d = make_host();
					d.set_death([]() -> void { stack_deep(150); });
					var keep = [4, 5, 6];
					d = null;
					return "gone:" + to_string(keep[2]);
				}
				over_script() + "|" + over_host();
			)");
			check_eq(std::string("7:3|gone:6"), r.to_string());
			check_eq(1, deaths, "the host object died at the store");
		});

		test("a_destructor_that_stores_to_the_variable_being_overwritten_keeps_its_store", [this]() {
			auto eng = make_engine();
			stdlib::register_all(*eng);
			auto r = eng->execute(R"(
				var g = null;
				class Rewriter { ~Rewriter() { g = 5; } }
				g = Rewriter();
				g = null;
				to_string(g);
			)");
			check_eq(std::string("5"), r.to_string(), "the store lands first; the old value's destructor runs after it (C++ shared_ptr order)");
		});

		test("nested_builtin_calls_inside_a_builtin_callback_keep_the_outer_arguments", [this]() {
			auto eng = make_engine();
			stdlib::register_all(*eng);
			auto r = eng->execute(R"(
				array<int> a = [3, 1, 2];
				array<int> seen = [];
				array<int> kept = array::filter(a, [&](int x) -> bool {
					array<int> b = [x];
					b.push(x);
					seen.push(b.size());
					return x > 1;
				});
				to_string(kept.size()) + ":" + to_string(seen.size()) + ":" + to_string(kept[0]);
			)");
			check_eq(std::string("2:3:3"), r.to_string());
		});
	}

	void forge_tests() override {
		stack_moving_reentry();
		// Methods entered from native code (bound methods a builtin calls, destructors, a bound method called directly):
		// fields, sibling calls, private members, a lambda capturing this, and the caller's own variables afterwards
		test("methods_entered_from_native_code_see_their_object_and_leave_the_caller_intact", [this]() {
			auto eng = make_engine();
			stdlib::register_all(*eng);
			auto r = eng->execute(R"(
				array<int> log = [];
				class Acc {
					int total = 0;
					int calls = 0;
					void add(int v) { total = total + v; bump(); }
					void bump() { calls += 1; }
					var reader() { return [this]() -> int { return total; }; }
					~Acc() { log.push(total); }
				}
				class Secret {
				private:
					int hidden = 1;
				public:
					void add(int v) { hidden += v; }
					int get() { return hidden; }
				}
				int scoped() { Acc tmp; tmp.add(5); return tmp.calls; }
				int outer = 40;
				Acc a;
				Secret s;
				array<int> xs = [1, 2, 3];
				xs.for_each(reflect::bind(a, "add"));
				xs.for_each(reflect::bind(s, "add"));
				var f = reflect::bind(a, "add");
				f(10);
				var read = a.reader();
				int c = scoped();
				to_string(a.total) + "|" + to_string(a.calls) + "|" + to_string(s.get()) + "|" + to_string(read()) + "|" +
					to_string(c) + "|" + to_string(log.size()) + ":" + to_string(log[0]) + "|" + to_string(outer);
			)");
			check_eq(std::string("16|4|7|16|1|1:5|40"), r.to_string());
		});

		// A bound method a builtin calls: an overload the argument count picks, overloads only the argument types tell
		// apart, a two-argument comparator and a class with private members
		test("bound_methods_as_builtin_callbacks_pick_the_overload_each_call_would", [this]() {
			auto eng = make_engine();
			stdlib::register_all(*eng);
			auto r = eng->execute(R"(
				class Acc { int total = 0; int add(int v) { total += v; return total; } }
				class Typed {
					int seen = 0;
					string show(int v) { seen += 1; return "i" + to_string(v); }
					string show(string v) { seen += 1; return "s" + v; }
				}
				class Order { bool before(int a, int b) { return a > b; } }
				class Hidden { private: int k = 10; public: int add(int v) { k += v; return k; } }
				Acc acc;
				Typed typed;
				Order order;
				Hidden hidden;
				array<int> xs = [1, 2, 3];
				array<var> sums = array::map(xs, reflect::bind(acc, "add"));
				array<var> shown = array::map([1, "b"], reflect::bind(typed, "show"));
				array<int> sorted = [2, 3, 1];
				sorted.sort(reflect::bind(order, "before"));
				array<var> hid = array::map(xs, reflect::bind(hidden, "add"));
				to_string(sums[2]) + "|" + to_string(acc.total) + "|" + shown[0] + shown[1] + to_string(typed.seen) + "|" +
					to_string(sorted[0]) + to_string(sorted[2]) + "|" + to_string(hid[2]);
			)");
			check_eq(std::string("6|6|i1sb2|31|16"), r.to_string());
		});

		// A method declared in a class outside a private member's family is refused that member on an instance of a
		// subclass inside it, however the method is entered (an in-loop call, a bound method, a builtin's callback); a method
		// whose body builds a closure over this runs as a builtin's callback too
		test("method_entries_check_private_access_against_the_declaring_class", [this]() {
			auto eng = make_engine();
			stdlib::register_all(*eng);
			auto r = eng->execute(R"(
				class Outer { int peek(var o) { return o.secret; } }
				class Holder : Outer { private: int secret = 7; public: int own() { return secret; } }
				class Leaf : Holder {}
				class Maker {
					int base = 2;
					int scaled(int v) { var f = [this](int k) -> int { return base * k; }; return f(v); }
				}
				Leaf leaf;
				string got = "";
				try { leaf.peek(leaf); got += "allowed"; } catch (e) { got += "refused"; }
				got += "|";
				try { var p = reflect::bind(leaf, "peek"); p(leaf); got += "allowed"; } catch (e) { got += "refused"; }
				got += "|";
				try { auto mapped = array::map([1], [&](int v) -> int { return leaf.peek(leaf); }); got += "allowed"; } catch (e) { got += "refused"; }
				got += "|" + to_string(leaf.own()) + "|";
				Maker m;
				array<var> scaled = array::map([1, 2, 3], reflect::bind(m, "scaled"));
				got + to_string(scaled[2]);
			)");
			check_eq(std::string("refused|refused|refused|7|6"), r.to_string());
		});

		test("a_bound_method_callback_runs_the_redefined_body_after_a_reload", [this]() {
			auto eng = make_engine();
			stdlib::register_all(*eng);
			eng->execute("class Acc { int total = 0; int add(int v) { total += v; return total; } }");
			eng->execute("Acc acc; var b = reflect::bind(acc, \"add\"); array<int> xs = [1, 2];");
			check_eq((int64_t)3, eng->execute("array::map(xs, b)[1]").as_int());
			eng->execute("class Acc { int total = 0; int add(int v) { total += v * 10; return total; } }");
			check_eq((int64_t)33, eng->execute("array::map(xs, b)[1]").as_int(), "the reloaded add ran on the same receiver");
		});
		test("nested_execute_in_converted_callback_keeps_outer_run", [this]() {
			auto eng = make_engine();
			engine* raw = eng.get();
			std::vector<std::string> host_trace;
			eng->add_function("host_eval", [raw](std::string src) -> bool {
				try { (void)raw->execute(src); return true; }
				catch (const std::exception&) { return false; }
			});
			eng->add_function("invoke", [&host_trace](script_value cb) {
				auto fn = convert_script_function<void()>(cb);
				host_trace.push_back("before");
				fn();
				host_trace.push_back("after");
			});
			eng->execute("var g_nested = 0;");
			auto r = eng->execute(R"(
				var outer = 1;
				invoke([&]() -> void {
					if (host_eval("g_nested = g_nested + 41;")) { outer = outer + 1; }
				});
				outer * 100 + g_nested;
			)");
			check_eq((int64_t)241, r.as_int(), "outer local and nested global both landed");
			check_eq((size_t)2, host_trace.size(), "C++ caller continued past the callback");
			check_eq((int64_t)7, eng->execute("3 + 4").as_int(), "engine usable afterwards");
		});

		test("nested_execute_failure_in_converted_callback_propagates_to_the_script", [this]() {
			// A script callback behind a host std::function throws its error through to the host, which
			// hands it back to the calling script as the host call's error
			auto eng = make_engine();
			engine* raw = eng.get();
			std::vector<std::string> reported;
			eng->set_script_error_handler([&reported](const std::string& msg) { reported.push_back(msg); });
			bool host_continued = false;
			eng->add_function("host_eval_unguarded", [raw](std::string src) { (void)raw->execute(src); });
			eng->add_function("invoke", [&host_continued](script_value cb) {
				convert_script_function<void()>(cb)();
				host_continued = true;
			});
			eng->execute("var g_boom = \"nested boom\"; var g_after = 0;");
			auto r = eng->execute(R"(
				var got = "none";
				try {
					invoke([]() -> void {
						host_eval_unguarded("throw g_boom;");
						g_after = 1;
					});
				} catch (e) { got = e; }
				g_after = g_after + 10;
				`${g_after}:${got}`;
			)");
			check_true(r.as_string().rfind("10:", 0) == 0 && r.as_string().find("nested boom") != std::string::npos, r.as_string());
			check_false(host_continued, "the host call ended at the callback's error");
			check_true(reported.empty(), "nothing is reported: the error travelled");
			check_eq((int64_t)7, eng->execute("3 + 4").as_int(), "engine usable afterwards");
		});

		test("callback_error_after_swallowed_nested_failure_propagates_its_own_text", [this]() {
			auto eng = make_engine();
			engine* raw = eng.get();
			std::vector<std::string> reported;
			eng->set_script_error_handler([&reported](const std::string& msg) { reported.push_back(msg); });
			bool host_saw_failure = false;
			eng->add_function("host_eval", [raw, &host_saw_failure](std::string src) -> bool {
				try { (void)raw->execute(src); return true; }
				catch (const std::exception& e) {
					host_saw_failure = std::string(e.what()).find("nested boom") != std::string::npos;
					return false;
				}
			});
			eng->add_function("host_throw", []() { throw std::runtime_error("callback boom"); });
			eng->add_function("invoke", [](script_value cb) { convert_script_function<void()>(cb)(); });
			eng->execute("var g_boom = \"nested boom\"; var g_after = 0;");
			auto r = eng->execute(R"(
				var got = "none";
				try {
					invoke([]() -> void {
						if (!host_eval("throw g_boom;")) { host_throw(); }
						g_after = 1;
					});
				} catch (e) { got = e; }
				g_after = g_after + 10;
				`${g_after}:${got}`;
			)");
			check_true(host_saw_failure, "host caught the nested failure with its text");
			check_eq(std::string("10:callback boom"), r.as_string(), "the callback's own error reached the script after a scrubbed nested failure");
			check_true(reported.empty(), "nothing is reported: the error travelled");
			check_eq((int64_t)7, eng->execute("3 + 4").as_int(), "engine usable afterwards");
		});

		test("swallowed_nested_failure_leaves_callback_and_outer_run_intact", [this]() {
			auto eng = make_engine();
			engine* raw = eng.get();
			std::vector<std::string> reported;
			eng->set_script_error_handler([&reported](const std::string& msg) { reported.push_back(msg); });
			eng->add_function("host_eval", [raw](std::string src) -> bool {
				try { (void)raw->execute(src); return true; }
				catch (const std::exception&) { return false; }
			});
			eng->add_function("invoke", [](script_value cb) { convert_script_function<void()>(cb)(); });
			eng->execute("var g_boom = \"nested boom\";");
			auto r = eng->execute(R"(
				var outer = 1;
				invoke([&]() -> void {
					if (!host_eval("throw g_boom;")) { outer = outer + 5; }
					outer = outer + 1;
				});
				outer;
			)");
			check_eq((int64_t)7, r.as_int(), "callback ran to completion on the outer frame's local");
			check_true(reported.empty(), "host handled the nested failure; nothing to report");
			check_eq(std::string("none"),
				eng->execute("var got = \"none\"; try { invoke([]() -> void { host_eval(\"throw g_boom;\"); }); } catch (e) { got = `${e}`; } got;").as<std::string>(),
				"an unrelated outer try/catch does not see the swallowed nested throw");
		});

		// Login/initialize.script shape: a [self] capture of a C++-bound object, fired later from a C++ signal.
		test("captured_cpp_bound_object_survives_into_converted_callback", [this]() {
			auto eng = make_engine();
			dynamic_binder<capture_probe> binder(*eng, "CaptureProbe");
			binder.method("value", &capture_probe::value);
			binder.build();
			auto obj = std::make_shared<capture_probe>();
			std::vector<std::function<void()>> stored;
			int64_t marked = -1;
			std::string err;
			eng->set_script_error_handler([&err](const std::string& m) { err = m; });
			eng->add_function("stash", [&stored](script_value cb) { stored.push_back(convert_script_function<void()>(cb)); });
			eng->add_function("mark", [&marked](script_int v) { marked = v; });
			eng->add_global("probeObj", eng->make_object(obj));
			eng->execute(R"(
				var init = [&](var& self) -> void { stash([self]() -> void { mark(self.value()); }); };
				init(probeObj);
			)");
			check_eq(size_t(1), stored.size(), "callback stashed");
			stored.front()();
			check_true(err.empty(), "callback error: " + err);
			check_eq((int64_t)7, marked, "captured C++-bound object is callable from the converted callback");
		});

		// Interface::initialize shape: C++ invokes a hook with T& and the script captures that parameter for later.
		test("captured_by_reference_callback_parameter_outlives_the_call", [this]() {
			auto eng = make_engine();
			dynamic_binder<capture_probe> binder(*eng, "CaptureProbe");
			binder.method("value", &capture_probe::value);
			binder.build();
			capture_probe live;
			std::vector<std::function<void()>> stored;
			int64_t marked = -1;
			std::string err;
			eng->set_script_error_handler([&err](const std::string& m) { err = m; });
			eng->add_function("stash", [&stored](script_value cb) { stored.push_back(convert_script_function<void()>(cb)); });
			eng->add_function("mark", [&marked](script_int v) { marked = v; });
			auto hook = eng->execute("[=](var& self) -> void { stash([self]() -> void { mark(self.value()); }); };");
			auto initialize = convert_script_function<void(capture_probe&)>(hook);
			initialize(live);
			check_true(err.empty(), "hook error: " + err);
			check_eq(size_t(1), stored.size(), "callback stashed");
			stored.front()();
			check_true(err.empty(), "callback error: " + err);
			check_eq((int64_t)7, marked, "captured by-reference parameter is still the live object");
		});

		// A host eval shape: a global 'self' exists when the hook is created and is nulled afterwards; the hook's own
		// var& parameter shadows it and the inner [self] capture must bind the parameter, not the (later null) global.
		test("captured_parameter_shadowing_a_nulled_global_binds_the_parameter", [this]() {
			auto eng = make_engine();
			dynamic_binder<capture_probe> binder(*eng, "CaptureProbe");
			binder.method("value", &capture_probe::value);
			binder.build();
			capture_probe live;
			auto other = std::make_shared<capture_probe>();
			other->v = 99;
			std::vector<std::function<void()>> stored;
			int64_t marked = -1;
			std::string err;
			eng->set_script_error_handler([&err](const std::string& m) { err = m; });
			eng->add_function("stash", [&stored](script_value cb) { stored.push_back(convert_script_function<void()>(cb)); });
			eng->add_function("mark", [&marked](script_int v) { marked = v; });
			eng->add_global("self", eng->make_object(other));
			auto hook = eng->execute("[=](var& self) -> void { stash([self]() -> void { mark(self.value()); }); };");
			eng->add_global("self", script_value(std::monostate{}, eng.get()));
			auto initialize = convert_script_function<void(capture_probe&)>(hook);
			initialize(live);
			check_true(err.empty(), "hook error: " + err);
			check_eq(size_t(1), stored.size(), "callback stashed");
			stored.front()();
			check_true(err.empty(), "callback error: " + err);
			check_eq((int64_t)7, marked, "inner capture bound the parameter (7), not the global (99/null)");
		});

		// Creature AI shape: a running script calls C++ which SYNCHRONOUSLY invokes a script callback; inside it, a
		// nested lambda explicitly captures both the callback's own capture and the callback's parameter.
		test("nested_external_callback_captures_its_parameter_and_own_capture", [this]() {
			auto eng = make_engine();
			dynamic_binder<capture_probe> binder(*eng, "CaptureProbe");
			binder.method("value", &capture_probe::value);
			binder.build();
			capture_probe live;
			std::vector<std::function<void()>> stored;
			int64_t marked = -1;
			std::string err;
			eng->set_script_error_handler([&err](const std::string& m) { err += m + " | "; });
			eng->add_function("stash", [&stored](script_value cb) { stored.push_back(convert_script_function<void()>(cb)); });
			eng->add_function("mark", [&marked](script_int v) { marked = v; });
			eng->add_function("with_probe", [&live](script_value cb) { convert_script_function<void(capture_probe&)>(cb)(live); });
			eng->execute(R"(
				var run = [=](int outer) -> void {
					with_probe([outer](var& probe) -> void { mark(outer); });
				};
				run(3);
			)");
			check_true(err.empty(), "stage A (callback's own capture read inside the external call): " + err);
			check_eq((int64_t)3, marked, "stage A value");
			err.clear(); marked = -1;
			eng->execute(R"(
				var run2 = [=](int outer) -> void {
					with_probe([outer](var& probe) -> void {
						stash([probe]() -> void { mark(probe.value()); });
					});
				};
				run2(3);
			)");
			check_true(err.empty(), "stage B (nested captures only the parameter): " + err);
			err.clear(); stored.clear(); marked = -1;
			eng->execute(R"(
				var run3 = [=](int outer) -> void {
					with_probe([outer](var& probe) -> void {
						stash([outer, probe]() -> void { mark(probe.value() * 10 + outer); });
					});
				};
				run3(3);
			)");
			check_true(err.empty(), "stage C (nested captures the callback's own capture): " + err);
			check_true(err.empty(), "errors: " + err);
			check_eq(size_t(1), stored.size(), "callback stashed");
			stored.front()();
			check_true(err.empty(), "errors: " + err);
			check_eq((int64_t)73, marked, "nested lambda saw the callback parameter (7) and the callback's capture (3)");
		});

		test("a_callback_capturing_the_outer_parameter_sees_it_in_every_spelling", [this]() {
			const std::tuple<std::string> scripts[] = {
				{R"(
				var run = [=](int outer) -> void { var cb = [outer](var& probe) -> void { mark(outer); }; with_probe(cb); };
				run(3);
			)"},
				{R"(
				var run = [=](int outer) -> void { with_probe([=](var& probe) -> void { mark(outer); }); };
				run(3);
			)"},
				{R"(
				function run(int outer) -> void { with_probe([outer](var& probe) -> void { mark(outer); }); }
				run(3);
			)"},
			};
			check_each(scripts, [](const auto& script) {
				auto eng = make_engine();
				dynamic_binder<capture_probe> binder(*eng, "CaptureProbe");
				binder.method("value", &capture_probe::value);
				binder.build();
				capture_probe live;
				int64_t marked = -1;
				std::string err;
				eng->set_script_error_handler([&err](const std::string& m) { err += m + " | "; });
				eng->add_function("mark", [&marked](script_int v) { marked = v; });
				eng->add_function("with_probe", [&live](script_value cb) { convert_script_function<void(capture_probe&)>(cb)(live); });
				try { eng->execute(std::get<0>(script)); } catch (const std::exception& e) { err += std::string("execute threw: ") + e.what(); }
				check_true(err.empty(), "errors: " + err);
				check_eq((int64_t)3, marked, "value");
			});
		});

		// Text-receiver shape fired from inside a script (Clickable::onAccept via press()).
		test("a_native_getters_exception_in_a_fused_push_argument_is_the_scripts_in_that_frame", [this]() {
			// a property reference stored in an array<var> element through the native array view; its getter throws on its
			// third (or first) call: dst.push(SRC[0]) is a call whose argument the fused push reads itself, so the exception
			// converts in the calling frame and the function's own try catches it, on both backends, proofs on and off
			// (an acceptance case; the unfused call's argument zone does the same). The engine's teardown never reads the program
			// scope's r: a property reference owns nothing, so the getter runs no more once the engine dies.
			struct source_t { engine* eng = nullptr; int throw_at = 3; bool armed = false; int calls = 0; };
			auto run = [&](bool vm, bool proofs, int throw_at) -> std::string {
				source_t source{ nullptr, throw_at };   // declared first: the engine refers to it until it dies
				auto e = jai::foundry::make_default_engine();
				source.eng = e.get();
				e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
				if (!proofs) { e->type_proofs(false); }
				stdlib::register_all(*e);
				dynamic_binder<source_t>(*e, "Source").property("number", [](source_t& s) -> script_int {
					if (s.armed && ++s.calls == s.throw_at) { throw std::runtime_error("boom"); }
					return 1;
				}, nullptr).build();
				e->add_global_ref("box", source);
				(void)e->execute("var& r = box.number; var SRC = [0];");
				std::string result;
				{
					auto reference = detail::engine_access::get_global_environment(*e)->get_ref("r");
					if (!reference) { return "no reference"; }
					auto src = e->get_variable("SRC");
					src.as_array()[0] = reference.value().get();
					source.armed = true; source.calls = 0;
					std::string out;
					try { out = e->execute("string probe() { array<int> dst = []; int i = 0; try { while (i < 4) { dst.push(SRC[0]); i++; } } catch (err) { return \"caught:\" + err + \"|\" + to_string(i) + \"|\" + to_string(dst.size()); } return \"ok:\" + to_string(dst.size()); } probe();").to_string(); }
					catch (const std::exception& ex) { out = std::string("throw:") + ex.what(); }
					result = out + "|calls=" + std::to_string(source.calls);
				}
				const int calls_before_teardown = source.calls;
				e.reset();
				if (source.calls != calls_before_teardown) { result += "|teardown ran the getter"; }
				return result;
			};
			for (bool proofs : { true, false }) {
				for (int at : { 3, 1 }) {
					const std::string i = run(false, proofs, at), v = run(true, proofs, at);
					check_eq(i, v, "the fused push's argument: both backends");
					const std::string want = at == 3 ? "caught:boom|2|2|calls=3" : "caught:boom|0|0|calls=1";
					check_eq(want, v, "caught by the pushing function's own try");
				}
			}
		});
		test("nested_execute_with_instance_vars_in_converted_callback", [this]() {
			auto eng = make_engine();
			engine* raw = eng.get();
			std::vector<std::string> nested_failures;
			eng->add_function("host_eval_with", [raw, &nested_failures](std::string src, script_int n) -> bool {
				instance_variables locals;
				locals["n"] = script_value(n, raw);
				try { (void)raw->execute(src, locals); return true; }
				catch (const std::exception& e) { nested_failures.push_back(e.what()); return false; }
			});
			eng->add_function("invoke", [](script_value cb) { convert_script_function<void()>(cb)(); });
			eng->execute("var g_sum = 0;");
			auto r = eng->execute(R"(
				var outer = 0;
				invoke([&]() -> void {
					host_eval_with("g_sum = g_sum + n;", 5);
					host_eval_with("g_sum = g_sum + n;", 6);
					outer = outer + 1;
				});
				outer * 100 + g_sum;
			)");
			check_true(nested_failures.empty(), nested_failures.empty() ? std::string() : "nested execute could not see its instance vars: " + nested_failures.front());
			check_eq((int64_t)111, r.as_int(), "instance-scoped nested runs landed and left the outer frame intact");
		});
	}
};

FOUNDRY_REGISTER(jai::foundry::tests::reentrant_callback_tests);

} // namespace jai::foundry::tests
