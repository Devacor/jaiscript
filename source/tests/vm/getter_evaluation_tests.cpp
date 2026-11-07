#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/testing/foundry.hpp>
#include <exception>
#include <string>
#include <vector>

namespace jai::foundry::tests {

struct growing_getter_source {
	script_int reads = 0;
};

class getter_evaluation_tests : public suite {
public:
	getter_evaluation_tests() : suite("Getter Evaluation") {}

	void forge_tests() override {
		test("index_decl_keeps_first_getter_result_on_bounds_failure", [this]() {
			for (bool proofs : {true, false}) {
				for (bool local_reference : {false, true}) {
					growing_getter_source source;
					auto eng = make_engine();
					eng->type_proofs(proofs);
					jai::stdlib::register_all(*eng);
					dynamic_binder<growing_getter_source>(*eng, "GrowingGetterSource")
						.property("values", [](growing_getter_source& a_source) {
							++a_source.reads;
							std::vector<script_int> result {10, 20};
							if (a_source.reads >= 3) { result.push_back(30); }
							return result;
						}, nullptr)
						.build();
					eng->add_global_ref("box", source);
					eng->add_function("clear_reads", [&source]() { source.reads = 0; });
					// Isolate reads inside the function from reference/argument binding.
					// Replaying the getter on a failed fast path used to turn this bounds
					// error into VALUE 30 on the third read.
					const std::string script = local_reference
						? "int read(int i) { auto& xs = box.values; clear_reads(); "
						  "int v = xs[i]; return v; } read(2);"
						: "int read(array<int>& xs, int i) { clear_reads(); "
						  "int v = xs[i]; return v; } read(box.values, 2);";
					std::string outcome;
					try {
						outcome = "VALUE " + std::to_string(eng->execute(script).as_int());
					} catch (const std::exception& error) {
						outcome = error.what();
					}
					check_eq(std::string("Array index 2 out of bounds for array of size 2"),
						outcome, script);
					check_eq(script_int{1}, source.reads, "one getter evaluation inside the function");
				}
			}
		});
	}
};

} // namespace jai::foundry::tests

using getter_evaluation_tests_alias = jai::foundry::tests::getter_evaluation_tests;
FOUNDRY_REGISTER(getter_evaluation_tests_alias)
