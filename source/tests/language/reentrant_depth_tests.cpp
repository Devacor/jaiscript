#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>

namespace jai::foundry::tests {

class reentrant_depth_tests : public suite {
public:
	reentrant_depth_tests() : suite("Reentrant Depth") {}

	template<class Engine>
	static void configure(Engine& eng, size_t depth) {
		if constexpr (requires { eng.max_call_depth(eng.get_backend_type(), depth); }) {
			eng.max_call_depth(eng.get_backend_type(), depth);
		} else {
			check_true(false, "engine::max_call_depth configuration is missing");
		}
	}

	static void expect_depth_error(engine& eng, const std::string& source) {
		std::string message;
		try { eng.execute(source); }
		catch (const std::exception& error) { message = error.what(); }
		check_true(message.find("Maximum recursion depth") != std::string::npos,
			"expected a catchable depth error, got: " + message);
		check_eq(script_int(42), eng.execute("40 + 2;").as_int());
	}

	static void install_reentry(engine& eng, const std::shared_ptr<size_t>& remaining) {
		engine* raw = &eng;
		// The callback owns only its bounded counter, never the owning engine.
		eng.add_function("host_reenter", [raw, remaining]() -> script_int {
			if (*remaining == 0) { return 7; }
			--*remaining;
			return raw->execute("host_reenter();").as_int();
		});
	}

	void forge_tests() override {
		test("fresh_coroutine_resume_chain_obeys_cap", [this]() {
			auto eng = make_engine();
			configure(*eng, 8);
			// Finite even without the new guard: old binaries stop after 21 resumes.
			expect_depth_error(*eng, R"(
				coroutine int descend(int n) {
					if (n == 0) { return 0; }
					auto child = descend(n - 1);
					return child.next() + 1;
				}
				auto root = descend(20);
				root.next();
			)");
			check_eq(script_int(2), eng->execute("auto small = descend(2); small.next();").as_int());
		});

		test("already_suspended_coroutine_chain_obeys_cap", [this]() {
			auto eng = make_engine();
			configure(*eng, 8);
			check_eq(script_int(210), eng->execute(R"(
				var handles = [];
				coroutine int parked(int index) {
					yield index;
					if (index == 0) { return 0; }
					return handles[index - 1].next() + 1;
				}
				for (int i = 0; i <= 20; ++i) { handles.push(parked(i)); }
				int total = 0;
				for (int i = 0; i <= 20; ++i) { total += handles[i].next(); }
				total;
			)").as_int());
			// Every handle has yielded already, so this exercises continuation replay
			// rather than the interpreter's guarded first-resume call_function path.
			expect_depth_error(*eng, "handles[20].next();");
			check_eq(script_int(42), eng->execute(
				"coroutine int fresh() { return 42; } auto next = fresh(); next.next();").as_int());
		});

		test("yield_and_completion_release_resume_depth", [this]() {
			auto eng = make_engine();
			configure(*eng, 1);
			check_eq(script_int(528), eng->execute(R"(
				coroutine int sequence() {
					for (int i = 0; i < 32; ++i) { yield i; }
					return 32;
				}
				auto values = sequence();
				int total = 0;
				for (int i = 0; i <= 32; ++i) { total += values.next(); }
				total;
			)").as_int());
			check_eq(script_int(42), eng->execute("int single() { return 42; } single();").as_int());
		});

		test("host_execute_reentry_without_script_frame_obeys_cap", [this]() {
			auto eng = make_engine();
			configure(*eng, 8);
			auto remaining = std::make_shared<size_t>(20);
			install_reentry(*eng, remaining);
			// Every source entry is just a native call, never a script function frame.
			expect_depth_error(*eng, "host_reenter();");
			check_true(*remaining > 0, "the cap must stop before the finite fallback");
			*remaining = 2;
			check_eq(script_int(7), eng->execute("host_reenter();").as_int());
			check_eq(size_t(0), *remaining);
			check_eq(script_int(42), eng->execute("40 + 2;").as_int());
		});

		test("script_catches_reentry_depth_error_and_callback_recovers", [this]() {
			auto eng = make_engine();
			configure(*eng, 8);
			auto remaining = std::make_shared<size_t>(20);
			install_reentry(*eng, remaining);
			auto message = eng->execute(R"(
				string message = "";
				try { host_reenter(); } catch (error) { message = error; }
				message;
			)").as<std::string>();
			check_true(message.find("Maximum recursion depth") != std::string::npos,
				"the outer script catch must receive the reentrant depth error: " + message);
			*remaining = 2;
			check_eq(script_int(7), eng->execute("host_reenter();").as_int());
			check_eq(script_int(42), eng->execute("40 + 2;").as_int());
		});
	}
};

} // namespace jai::foundry::tests

using reentrant_depth_tests = jai::foundry::tests::reentrant_depth_tests;
FOUNDRY_REGISTER(reentrant_depth_tests)
