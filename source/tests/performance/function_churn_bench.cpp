// Function-value churn microbenchmarks (copy-trio targets from the function-value copy
// audit): lambda create+call, named declare+call, pass+call. Each iteration re-executes
// a pre-parsed bite, so the measured work is the runtime mint + call machinery, not
// lexing/parsing. Compare min-of-3 whole-suite runs.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <map>
#include <optional>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class function_churn_bench : public suite {
public:
	function_churn_bench() : suite("Function Churn Bench") {}

	std::shared_ptr<jai::engine> test_engine;
	std::map<std::string, jai::jaibite> bites;

	void pre_test() override {
		test_engine = make_engine();
		bites.clear();
	}

	void run_bite(const char* key, const char* source) {
		auto it = bites.find(key);
		if (it == bites.end()) {
			it = bites.emplace(key, test_engine->jaibite(source)).first;
		}
		it->second.execute();
	}

	void forge_tests() override {
		benchmark("Lambda create+call (capture-free)", [this]() {
			run_bite("lcc", "auto g = [](int y) { return y * 2; }; return g(21);");
		}, 2000);

		benchmark("Lambda create+call (capturing)", [this]() {
			run_bite("lcap", "var k = 4; auto g = [=](int y) { return y * k; }; return g(21);");
		}, 2000);

		benchmark("Named declare+call", [this]() {
			run_bite("ncc", "int f(int x) { return x + 1; } return f(1);");
		}, 2000);

		benchmark("Pass function + call through", [this]() {
			run_bite("pcc",
				"int f(int x) { return x + 1; }"
				"int caller(var g, int x) { return g(x); }"
				"return caller(f, 5);");
		}, 2000);
	}
};

} // namespace jai::foundry::tests

using function_churn_bench = jai::foundry::tests::function_churn_bench;
FOUNDRY_REGISTER(function_churn_bench)
