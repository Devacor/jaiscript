// Host-call cost cases: each row's typed loop (k_iterations) timed against the bare loop's on both backends (the ratio and
// the medians' ns per call over the loop).
// The rows split a host call into its parts: a plain script_function global (the call mechanics), the binder's
// conversions without an overload set, a typed add_function (one overload), a real overload set, a statement-position
// call, a host method on a bound class, and the math adapters' vector argument and result. A before/after of a host-call
// change is one run of this suite: compare the printed HOSTCALL rows.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/timing.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/math/adapter.hpp>
#include <cstdio>
#include <string>

using namespace jai;
using namespace jai::foundry;

namespace host_call_bench_hosts {
	struct counter {
		double total = 0;
		double add(double k) { total += k; return total; }
	};
}

namespace jai::foundry::tests {

class host_call_bench : public suite {
public:
	host_call_bench() : suite("Host Call Bench") {}

	static std::shared_ptr<jai::engine> host(bool use_vm) {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
		e->execution_budget(0);
		jai::stdlib::register_all(e);
		engine* eng = e.get();
		e->add_function_with_arity("fraw", [eng](const std::vector<script_value>& a) -> checked_result<script_value> {
			return script_value(a[0].unchecked_as_float() + 1.0, eng);
		}, 1);
		e->add_function_with_arity("fbound", make_function_binder([](double k) { return k + 1.0; }, eng).bind(), 1);
		e->add_function("fs", [](double k) { return k + 1.0; });
		e->add_function("fs2", [](double a, double b) { return a + b; });
		e->add_function("fset", [](double k) { return k + 1.0; });
		e->add_function("fset", [](const std::string& s) { return static_cast<double>(s.size()); });
		e->add_function("fv", [](jai::vec3 p) { return static_cast<double>(p.x + p.y + p.z); });
		e->add_function("vf", [](double k) { return jai::vec3{static_cast<float>(k), 2.0f, 3.0f}; });
		dynamic_binder<host_call_bench_hosts::counter>(*e, "Counter").constructor<>().method("add", &host_call_bench_hosts::counter::add).build();
		e->execute("function sfs(float k) -> float { return k + 1.0; }");
		return e;
	}

	// The iterations of each loop: a Release reading needs no more, and a row's test stays seconds in Debug
	static constexpr int k_iterations = 20000;

	// A row is a test of its own, timed on the interpreter and then the VM, so no one test runs every row
	void row_test(const char* name, std::string reference, std::string src) {
		test(name, [name, reference, src]() {
			for (bool use_vm : {false, true}) {
				auto e = host(use_vm);
				auto timed = [&e](const std::string& script) { return [&e, script] { return time_seconds([&] { e->execute(script); }); }; };
				const char* backend = use_vm ? "vm    " : "interp";
				const speed_ratio speed = compare_speed(std::string("HOSTCALL ") + backend + " " + name + " against the bare loop", timed(reference), timed(src));
				std::printf("HOSTCALL %s %-20s %7.1f ns/iter %7.1f over the loop\n", backend, name, speed.measured_seconds * 1e9 / k_iterations,
				            (speed.measured_seconds - speed.reference_seconds) * 1e9 / k_iterations);
			}
		});
	}

	void forge_tests() override {
		const std::pair<const char*, const char*> rows[] = {
			{"loop", "s += k;"},
			{"plain_fn", "s += fraw(k);"},
			{"binder_only", "s += fbound(k);"},
			{"typed_one_overload", "s += fs(k);"},
			{"typed_two_args", "s += fs2(k, k);"},
			{"typed_overload_set", "s += fset(k);"},
			{"typed_statement", "fs(k);"},
			{"bound_method", "s += c.add(k);"},
			{"vec_arg", "s += fv(p);"},
			{"vec_result", "s += vf(k).x;"},
			{"script_fn", "s += sfs(k);"},
		};
		const std::string n = std::to_string(k_iterations);
		auto source_of = [&n](const char* body) {
			return std::string("function f(int n) -> float { math::vec3 p = math::vec3(1, 2, 3); var c = Counter(); float s = 0.0; float k = 1.5; "
			                   "for (int i = 0; i < n; i++) { ") + body + " } return s; } f(" + n + ");";
		};
		for (const auto& [name, body] : rows) { row_test(name, source_of(rows[0].second), source_of(body)); }
	}
};

} // namespace jai::foundry::tests

using host_call_bench = jai::foundry::tests::host_call_bench;
FOUNDRY_REGISTER(host_call_bench)
