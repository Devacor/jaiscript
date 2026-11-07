// Closed typed loops run from their back edge as one cell block. These are the contract controls
// for that shape: every shape lands the same on the interpreter and the vm.
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class pilot_cluster_tests : public suite {
public:
	pilot_cluster_tests() : suite("Closed Typed Loops") {}

	// The render column loop's shape: a typed compare head, a typed element load through a var&
	// container, two no-result typed-lane element stores through a var& container, typed adds,
	// the back edge. Returns "<gi*1000+y>:<weighted pixel sum>" or "throw:<text>".
	static constexpr const char* k_prelude = R"(
		array<int> PIX = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
		var STRIPS = [[7, 8, 9, 10]];
		function paint(var& pix, var& strip, int cy0, int cy1, int vw, int px0, int tstep, int tacc0, int k) -> int {
			int gi = cy0 * vw + px0;
			int tacc = tacc0;
			int y = cy0;
			int acc = 0;
			while (y < cy1) {
				int c = strip[tacc >> 11];
				pix[gi] = c;
				pix[gi + 1] = c;
				tacc += tstep;
				gi += vw;
				acc += k;
				y = y + 1;
			}
			return gi * 1000 + y;
		}
		function weighted() -> int { int s = 0; for (int i = 0; i < PIX.size(); ++i) { s += PIX[i] * (i + 1); } return s; }
		function run(int cy0, int cy1, int tstep, int k) -> string {
			var& s = STRIPS[0];
			string out = "";
			try { out = to_string(paint(PIX, s, cy0, cy1, 2, 0, tstep, 0, k)); }
			catch (err) { out = "throw:" + err; }
			return out + ":" + to_string(weighted());
		}
	)";

	std::string run_on(bool vm, const std::string& script, bool proofs = true, double budget = 0.0) {
		auto e = jai::foundry::make_default_engine();
		e->set_backend(vm ? backend_type::vm : backend_type::interpreter);
		stdlib::register_all(*e);
		if (!proofs) { e->type_proofs(false); }
		if (budget > 0.0) { e->execution_budget(budget); }
		try { return e->execute(script).as_string(); }
		catch (const std::exception& ex) { return std::string("throw:") + ex.what(); }
	}

	void both(const std::string& script, const std::string& expected, const char* label, bool proofs = true) {
		const std::string interp = run_on(false, script, proofs);
		const std::string vm = run_on(true, script, proofs);
		check_eq(interp, vm, std::string(label) + ": both backends");
		if (!expected.empty()) { check_eq(expected, vm, std::string(label) + ": expected"); }
	}

	void forge_tests() override {
		// four iterations over strip entries 0..3: PIX = [7,7,8,8,9,9,10,10,0,0,0,0], gi 8, y 4
		test("column_loop_lands_the_same_on_both_backends", [this]() {
			both(std::string(k_prelude) + "run(0, 4, 2048, 1);", "8004:326", "four iterations");
		});
		test("zero_and_one_iteration", [this]() {
			both(std::string(k_prelude) + "run(4, 4, 2048, 1);", "8004:0", "zero iterations");
			both(std::string(k_prelude) + "run(0, 1, 2048, 1);", "2001:21", "one iteration");
		});
		test("overflow_after_stores_keeps_the_stores_and_the_text", [this]() {
			// acc += k with k = 2^62: the first addition gives 2^62, the second overflows, so the
			// second iteration's stores are complete and the third never starts
			both(std::string(k_prelude) + "run(0, 4, 2048, 4611686018427387904);", "", "late overflow");
			const std::string vm = run_on(true, std::string(k_prelude) + "run(0, 4, 2048, 4611686018427387904);");
			check_true(vm.rfind("throw:", 0) == 0, "the overflow raises: " + vm);
			check_true(vm.find(":77") != std::string::npos, "two iterations of stores retained (7*1+7*2+8*3+8*4 = 77): " + vm);
		});
		test("non_int_element_mid_loop_falls_back_with_the_same_text", [this]() {
			const std::string script = std::string(k_prelude) + "STRIPS[0][2] = \"x\"; run(0, 4, 2048, 1);";
			both(script, "", "string element on the third iteration");
			const std::string vm = run_on(true, script);
			check_true(vm.rfind("throw:", 0) == 0, "the declaration rejects the string: " + vm);
			check_true(vm.find(":77") != std::string::npos, "two iterations of stores retained: " + vm);
		});
		test("proofs_off_lands_the_same", [this]() {
			both(std::string(k_prelude) + "run(0, 4, 2048, 1);", "8004:326", "proofs off", /*proofs*/ false);
		});
	}
};

}

using pilot_cluster_tests = jai::foundry::tests::pilot_cluster_tests;
FOUNDRY_REGISTER(pilot_cluster_tests)
