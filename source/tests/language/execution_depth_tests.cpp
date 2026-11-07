#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/reflection.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <limits>
namespace jai::foundry::tests {

class execution_depth_tests : public suite {
public:
	execution_depth_tests() : suite("Execution Depth") {}

	template<class Engine>
	static void configure(Engine& eng, backend_type backend, size_t depth) {
		if constexpr (requires { eng.max_call_depth(backend, depth); }) {
			eng.max_call_depth(backend, depth);
		} else {
			check_true(false, "engine::max_call_depth configuration is missing");
		}
	}

	template<class Engine>
	static size_t configured(const Engine& eng, backend_type backend) {
		if constexpr (requires { eng.max_call_depth(backend); }) {
			return eng.max_call_depth(backend);
		} else {
			check_true(false, "engine::max_call_depth query is missing");
			return 0;
		}
	}

	static std::string depth_error(engine& eng, const std::string& source) {
		std::string result;
		try { eng.execute(source); }
		catch (const std::exception& error) { result = error.what(); }
		check_true(result.find("Maximum recursion depth") != std::string::npos,
			"expected a catchable depth error, got: " + result);
		check_eq(script_int(42), eng.execute("40 + 2;").as_int());
		return result;
	}

	void forge_tests() override {
		test("defaults_are_backend_specific", [this]() {
			auto eng = make_engine();
			check_eq(size_t(256), configured(*eng, backend_type::interpreter));
			check_eq(size_t(250000), configured(*eng, backend_type::vm));
		});

		test("interpreter_default_bounds_non_tail_recursion", [this]() {
			auto eng = jai::foundry::make_default_engine();
			eng->set_backend(backend_type::interpreter);
			eng->execution_budget(0);
			depth_error(*eng, "int descend(int n) { if (n == 0) { return 0; } return descend(n - 1) + 1; } descend(512);");
		});

		test("vm_default_preserves_deep_non_tail_recursion", [this]() {
			auto eng = jai::foundry::make_default_engine();
			eng->set_backend(backend_type::vm);
			eng->execution_budget(0);
			check_eq(script_int(5000), eng->execute(
				"int descend(int n) { if (n == 0) { return 0; } return descend(n - 1) + 1; } descend(5000);").as_int());
		});

		// Every script-to-script call shape runs as an in-loop frame: 50000 levels need no native stack, and a
		// recursion whose frames carry an environment (a try per level) stays linear
		test("vm_deep_recursion_of_every_call_shape", [this]() {
			const std::pair<const char*, const char*> shapes[] = {
				{"function", "int d(int n) { if (n == 0) { return 0; } return d(n - 1) + 1; } d(50000);"},
				{"method", "class C { int f(int n) { if (n == 0) { return 0; } return f(n - 1) + 1; } } C().f(50000);"},
				{"method through a handle", "class C { shared_ptr<C> self; int f(int n) { if (n == 0) { return 0; } return self.f(n - 1) + 1; } } shared_ptr<C> c = new C(); c.self = c; int r = c.f(50000); c.self = null; r;"},
				{"static method", "class C { static int f(int n) { if (n == 0) { return 0; } return C::f(n - 1) + 1; } } C::f(50000);"},
				{"constructor", "class C { int d = 0; C(int n) { if (n > 0) { auto child = C(n - 1); d = child.d + 1; } } } C(50000).d;"},
				{"constructor through new", "class C { int d = 0; C(int n) { if (n > 0) { shared_ptr<C> child = new C(n - 1); d = child.d + 1; } } } C(50000).d;"},
				{"mutual recursion", "int a(int n) { if (n == 0) { return 0; } return b(n - 1) + 1; } int b(int n) { if (n == 0) { return 0; } return a(n - 1) + 1; } a(50000);"},
				{"lambda in a global", "var f = function(int n) { if (n == 0) { return 0; } return f(n - 1) + 1; }; f(50000);"},
				{"lambda in a local", "int run(int n) { function f; f = [&](int k) -> int { if (k == 0) { return 0; } return f(k - 1) + 1; }; return f(n); } run(50000);"},
				{"function value", "int d(int n) { if (n == 0) { return 0; } var g = d; return g(n - 1) + 1; } d(50000);"},
				{"default argument", "int d(int n, int k = 1) { if (n == 0) { return 0; } return d(n - 1) + k; } d(50000);"},
				{"bound method", "class C { int f(int n) { if (n == 0) { return 0; } return reflect::bind(this, \"f\")(n - 1) + 1; } } C().f(50000);"},
				{"callable object", "class V { int operator()(int n) { if (n == 0) { return 0; } return this(n - 1) + 1; } } auto v = V(); v(50000);"},
				{"try per level", "int d(int n) { if (n == 0) { return 0; } try { return d(n - 1) + 1; } catch (e) { throw e; } } d(50000);"},
				{"range-for per level", "int d(int n) { if (n == 0) { return 0; } int s = 0; for (auto x : [1]) { s = d(n - 1) + x; } return s; } d(50000);"},
			};
			for (const auto& [shape, source] : shapes) {
				auto eng = jai::foundry::make_default_engine();
				jai::stdlib::register_reflection_functions(eng);
				eng->set_backend(backend_type::vm);
				std::string outcome;
				try { outcome = std::to_string(eng->execute(source).as_int()); }
				catch (const std::exception& error) { outcome = error.what(); }
				check_eq(std::string("50000"), outcome, shape);
				check_eq(script_int(42), eng->execute("40 + 2;").as_int());
			}
		});

		// The cap is a backstop: memory and time bound a runaway recursion first (frames are charged every 10000 levels)
		test("vm_runaway_recursion_trips_the_memory_cap", [this]() {
			auto eng = jai::foundry::make_default_engine();
			eng->set_backend(backend_type::vm);
			eng->execution_budget(0);
			configure(*eng, backend_type::vm, size_t(std::numeric_limits<int>::max()));
			eng->memory_cap(64u << 20);
			std::string message;
			try { eng->execute("int forever(int n) { return forever(n + 1) + 1; } forever(0);"); }
			catch (const std::exception& error) { message = error.what(); }
			check_true(message.find("Script memory cap exceeded") != std::string::npos, "got: " + message);
			check_eq(script_int(42), eng->execute("int r = 0; try { forever(0); } catch (e) { r = 42; } r;").as_int());
			check_eq(script_int(1000), eng->execute("int d(int n) { if (n == 0) { return 0; } return d(n - 1) + 1; } d(1000);").as_int());
		});

		test("vm_runaway_recursion_trips_the_budget", [this]() {
			auto eng = jai::foundry::make_default_engine();
			eng->set_backend(backend_type::vm);
			eng->execution_budget(0.25);
			configure(*eng, backend_type::vm, size_t(std::numeric_limits<int>::max()));
			std::string message;
			try { eng->execute("int forever(int n) { return forever(n + 1) + 1; } forever(0);"); }
			catch (const std::exception& error) { message = error.what(); }
			check_true(message.find("Script execution budget exceeded") != std::string::npos, "got: " + message);
			check_eq(script_int(42), eng->execute("40 + 2;").as_int());
		});

		test("vm_cap_bounds_deep_recursion_and_unwinds", [this]() {
			auto eng = jai::foundry::make_default_engine();
			eng->set_backend(backend_type::vm);
			eng->execution_budget(0);
			configure(*eng, backend_type::vm, 60000);
			eng->execute("int descend(int n) { if (n == 0) { return 0; } return descend(n - 1) + 1; }");
			check_eq(script_int(59999), eng->execute("descend(59999);").as_int());
			check_true(depth_error(*eng, "descend(60000);").starts_with("Maximum recursion depth exceeded - possible infinite recursion"));
			check_eq(script_int(42), eng->execute("int r = 0; try { descend(60000); } catch (e) { r = 42; } r;").as_int());
			check_eq(script_int(59999), eng->execute("descend(59999);").as_int());
		});

		test("vm_throw_unwinds_deep_frames", [this]() {
			auto eng = jai::foundry::make_default_engine();
			eng->set_backend(backend_type::vm);
			eng->execution_budget(0);
			check_eq(script_int(7), eng->execute(R"(
				int d(int n) { if (n == 0) { throw "bottom"; } return d(n - 1) + 1; }
				int r = 0;
				try { d(50000); } catch (e) { if (e == "bottom") { r = 7; } }
				r;
			)").as_int());
			check_eq(script_int(2000), eng->execute(R"(
				int hops = 0;
				int rethrow(int n) { if (n == 0) { throw "deep"; } try { return rethrow(n - 1); } catch (e) { hops += 1; throw e; } }
				try { rethrow(2000); } catch (e) { }
				hops;
			)").as_int());
			check_eq(script_int(49999), eng->execute("int up(int n) { if (n == 0) { return 0; } return up(n - 1) + 1; } up(49999);").as_int());
		});

		// A recursion through a host callback re-enters run() natively per level: the native-stack headroom check raises
		// its own catchable text at the re-entry (the cap is out of reach), and the engine stays usable
		test("vm_host_reentry_recursion_fails_cleanly", [this]() {
			auto eng = jai::foundry::make_default_engine();
			eng->set_backend(backend_type::vm);
			eng->execution_budget(0);
			configure(*eng, backend_type::vm, size_t(std::numeric_limits<int>::max()));
			eng->add_function("host_call", [](script_value f, script_int k) { return convert_script_function<script_int(script_int)>(f)(k); });
			eng->execute("int d(int n) { if (n == 0) { return 0; } return host_call(d, n - 1) + 1; }");
			std::string message;
			try { eng->execute("d(100000000);"); }
			catch (const std::exception& error) { message = error.what(); }
			check_true(message.find("Native stack exhausted before the call-depth cap") != std::string::npos, "got: " + message);
			check_eq(script_int(1), eng->execute(R"(
				int caught = 0;
				try { d(100000000); } catch (e) { if (e.find("Native stack exhausted") == 0) { caught = 1; } }
				caught;
			)").as_int());
			check_eq(script_int(100), eng->execute("d(100);").as_int());
		});

		test("vm_default_argument_recursion_fails_cleanly", [this]() {
			auto eng = jai::foundry::make_default_engine();
			eng->set_backend(backend_type::vm);
			eng->execution_budget(0);
			configure(*eng, backend_type::vm, size_t(std::numeric_limits<int>::max()));
			eng->execute("int left = 0; int f(int d = left > 0 ? g() : 0) { return d + 1; } int g() { left -= 1; return f(); }");
			check_eq(script_int(11), eng->execute("left = 10; f();").as_int());
			std::string message;
			try { eng->execute("left = 100000000; f();"); }
			catch (const std::exception& error) { message = error.what(); }
			check_true(message.find("Native stack exhausted before the call-depth cap") != std::string::npos, "got: " + message);
			check_eq(script_int(11), eng->execute("left = 10; f();").as_int());
		});

		test("configured_boundary_and_recovery", [this]() {
			auto eng = make_engine();
			configure(*eng, eng->get_backend_type(), 8);
			eng->execute("int descend(int n) { if (n == 0) { return 0; } return descend(n - 1) + 1; }");
			check_eq(script_int(7), eng->execute("descend(7);").as_int());
			depth_error(*eng, "descend(8);");
			check_eq(script_int(7), eng->execute("descend(7);").as_int());
			configure(*eng, eng->get_backend_type(), 32);
			check_eq(script_int(20), eng->execute("descend(20);").as_int());
			configure(*eng, eng->get_backend_type(), 4);
			depth_error(*eng, "descend(4);");
		});

		test("engine_configuration_is_independent_and_survives_backend_selection", [this]() {
			auto first = jai::foundry::make_default_engine();
			auto second = jai::foundry::make_default_engine();
			configure(*first, backend_type::interpreter, 12);
			configure(*first, backend_type::vm, 48);
			first->set_backend(backend_type::interpreter);
			check_eq(size_t(12), configured(*first, backend_type::interpreter));
			check_eq(size_t(48), configured(*first, backend_type::vm));
			check_eq(size_t(256), configured(*second, backend_type::interpreter));
			check_eq(size_t(250000), configured(*second, backend_type::vm));
		});

		test("invalid_caps_leave_configuration_unchanged", [this]() {
			auto eng = make_engine();
			configure(*eng, eng->get_backend_type(), 12);
			for (size_t invalid : {size_t(0), size_t(std::numeric_limits<int>::max()) + 1}) {
				check_throws([&]() { configure(*eng, eng->get_backend_type(), invalid); });
				check_eq(size_t(12), configured(*eng, eng->get_backend_type()));
			}
			check_throws([&]() { configure(*eng, backend_type::auto_select, 12); });
		});

		test("cap_error_text_is_identical_with_distinct_caps", [this]() {
			std::string messages[2];
			for (int i = 0; i < 2; ++i) {
				auto eng = jai::foundry::make_default_engine();
				eng->set_backend(i == 0 ? backend_type::interpreter : backend_type::vm);
				configure(*eng, eng->get_backend_type(), i == 0 ? 8 : 16);
				messages[i] = depth_error(*eng, "int forever() { return forever() + 1; } forever();");
			}
			check_eq(messages[0], messages[1]);
			check_true(messages[0].starts_with("Maximum recursion depth exceeded - possible infinite recursion"));
		});

		test("script_can_catch_depth_error_and_continue", [this]() {
			auto eng = make_engine();
			configure(*eng, eng->get_backend_type(), 8);
			check_eq(script_int(42), eng->execute(R"(
				int forever() { return forever() + 1; }
				int result = 0;
				try { result = forever(); } catch { result = 42; }
				result;
			)").as_int());
		});

		test("method_and_constructor_bodies_obey_configuration", [this]() {
			for (const std::string source : {
				"class C { int f(int n) { if (n == 0) { return 0; } return f(n-1)+1; } } C().f(20);",
				"class C { static int f(int n) { if (n == 0) { return 0; } return C::f(n-1)+1; } } C::f(20);",
				"class C { int f(int n) { if (n == 0) { return 0; } return f(n-1)+1; } } auto c = C(); auto bound = reflect::bind(c, \"f\"); bound(20);",
				"class C { C(int n) { if (n > 0) { auto child = C(n-1); } } } C(20);"}) {
				auto eng = make_engine();
				jai::stdlib::register_reflection_functions(eng);
				configure(*eng, eng->get_backend_type(), 8);
				depth_error(*eng, source);
			}
		});

		test("configuration_cannot_change_inside_execution", [this]() {
			auto eng = make_engine();
			const auto backend = eng->get_backend_type();
			configure(*eng, backend, 12);
			engine* raw = eng.get();
			eng->add_function("change_depth", [raw, backend]() { configure(*raw, backend, 24); });
			std::string message;
			try { eng->execute("change_depth();"); }
			catch (const std::exception& error) { message = error.what(); }
			check_true(message.find("Cannot change max_call_depth during execution") != std::string::npos);
			check_eq(size_t(12), configured(*eng, backend));
			check_eq(script_int(42), eng->execute("40 + 2;").as_int());
		});

		test("reused_parallel_workers_receive_updated_caps", [this]() {
			auto eng = make_engine();
			eng->parallel_thread_count(4);
			configure(*eng, eng->get_backend_type(), 64);
			eng->execute(R"(
				int descend(int n) { if (n == 0) { return 0; } return descend(n-1)+1; }
				int item(int n) { return descend(n); }
				var input = [];
				for (int i = 0; i < 256; ++i) { input.push(20); }
			)");
			const std::string source = "auto output = parallel_transform(input, item); output[0] + output[255];";
			check_eq(script_int(40), eng->execute(source).as_int());
			configure(*eng, eng->get_backend_type(), 8);
			depth_error(*eng, source);
			configure(*eng, eng->get_backend_type(), 64);
			check_eq(script_int(40), eng->execute(source).as_int());
		});


		test("vm_tail_calls_do_not_consume_additional_depth", [this]() {
			auto eng = jai::foundry::make_default_engine();
			eng->set_backend(backend_type::vm);
			eng->execution_budget(0);
			configure(*eng, backend_type::vm, 4);
			check_eq(script_int(0), eng->execute(
				"int descend(int n) { if (n == 0) { return 0; } return descend(n-1); } descend(100000);").as_int());
		});

		// A chain of closures each capturing the last nests one environment per link; its release ran one native frame per
		// link (a process kill at teardown). Its environments release on a work list now
		test("a_long_closure_chain_releases_without_native_depth", [this]() {
			for (const char* drop : {"f = null;", ""}) {
				auto eng = make_engine();
				eng->execution_budget(0);
				eng->execute(std::string("var f = null; for (int i = 0; i < 100000; i++) { var g = f; f = [g]() { return g; }; } int length = 0; var at = f; "
				                         "while (at != null && length < 3) { at = at(); length += 1; } ") + drop);
				check_eq(script_int(3), eng->execute("length").as_int());
			}
		});
	}
};

} // namespace jai::foundry::tests

using execution_depth_tests = jai::foundry::tests::execution_depth_tests;
FOUNDRY_REGISTER(execution_depth_tests)
