// Method-call cost cases: each row's typed loop (k_iterations) timed against the bare loop's on both backends (the ratio
// and the medians' ns per call over the loop).
// The rows split a method call by receiver and callee: a script class's method through a var, a typed local, a handle, an
// inherited and an overridden method, a method that touches no field, a sibling call and this.m() inside a method, a static
// method, and a host class's bound method (var and typed receivers, no arguments). A before/after of a method-call change is
// one run of this suite: compare the printed METHODCALL rows.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/timing.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <cstdio>
#include <string>

using namespace jai;
using namespace jai::foundry;

namespace method_call_bench_hosts {
	struct tally {
		double total = 0;
		double add(double k) { total += k; return total; }
		double get() const { return total; }
	};
}

namespace jai::foundry::tests {

class method_call_bench : public suite {
public:
	method_call_bench() : suite("Method Call Bench") {}

	static std::shared_ptr<jai::engine> host(bool use_vm) {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
		e->execution_budget(0);
		jai::stdlib::register_all(e);
		dynamic_binder<method_call_bench_hosts::tally>(*e, "Tally").constructor<>()
			.method("add", &method_call_bench_hosts::tally::add).method("get", &method_call_bench_hosts::tally::get).build();
		e->execute(
			"class S { float v = 0.0; "
			"  float add(float k) { v += k; return v; } "
			"  float idf(float k) { return k + 1.0; } "
			"  float twice(float k) { return k * 2.0; } "
			"  static float sadd(float k) { return k + 1.0; } "
			"  float run_sibling(int n, float k) { float s = 0.0; for (int i = 0; i < n; i++) { s += idf(k); } return s; } "
			"  float run_this(int n, float k) { float s = 0.0; for (int i = 0; i < n; i++) { s += this.idf(k); } return s; } "
			"  float run_loop(int n, float k) { float s = 0.0; for (int i = 0; i < n; i++) { s += k; } return s; } "
			"} "
			"class D : S { override float twice(float k) { return k * 3.0; } } "
			"function sfs(float k) -> float { return k + 1.0; }");
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
				const speed_ratio speed = compare_speed(std::string("METHODCALL ") + backend + " " + name + " against its loop", timed(reference), timed(src));
				std::printf("METHODCALL %s %-22s %7.1f ns/iter %7.1f over the loop\n", backend, name, speed.measured_seconds * 1e9 / k_iterations,
				            (speed.measured_seconds - speed.reference_seconds) * 1e9 / k_iterations);
			}
		});
	}

	void forge_tests() override {
		struct row { const char* name; const char* decl; const char* body; };
		const row rows[] = {
			{"loop", "", "s += k;"},
			{"script_fn", "", "s += sfs(k);"},
			{"script_method_var", "var o = S();", "s += o.idf(k);"},
			{"script_method_typed", "S o = S();", "s += o.idf(k);"},
			{"script_method_field", "S o = S();", "s += o.add(k);"},
			{"script_method_handle", "shared_ptr<S> o = new S();", "s += o.idf(k);"},
			{"script_inherited", "D o = D();", "s += o.idf(k);"},
			{"script_overridden", "S o = D();", "s += o.twice(k);"},
			{"script_static", "", "s += S::sadd(k);"},
			{"host_method_var", "var o = Tally();", "s += o.add(k);"},
			{"host_method_typed", "Tally o = Tally();", "s += o.add(k);"},
			{"host_method_noargs", "Tally o = Tally();", "s += o.get();"},
		};
		// rows whose loop runs inside a method (the call is a sibling call or this.m())
		const row in_method[] = {
			{"in_method_loop", "", "run_loop"},
			{"sibling_call", "", "run_sibling"},
			{"this_call", "", "run_this"},
		};
		const std::string n = std::to_string(k_iterations);
		auto free_loop = [&n](const row& r) { return std::string("function f(int n) -> float { ") + r.decl + " float s = 0.0; float k = 1.5; for (int i = 0; i < n; i++) { " + r.body + " } return s; } f(" + n + ");"; };
		auto method_loop = [&n](const row& r) { return std::string("S o = S(); o.") + r.body + "(" + n + ", 1.5);"; };
		for (const auto& r : rows) { row_test(r.name, free_loop(rows[0]), free_loop(r)); }
		for (const auto& r : in_method) { row_test(r.name, method_loop(in_method[0]), method_loop(r)); }
	}
};

} // namespace jai::foundry::tests

using method_call_bench = jai::foundry::tests::method_call_bench;
FOUNDRY_REGISTER(method_call_bench)
