// Element checker/runtime agreement corpus (129 programs).
// Rejecting a program that runs with checking off fails this suite.
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>

namespace jai::foundry::tests {

class element_checker_runtime_tests : public suite {
public:
	element_checker_runtime_tests() : suite("Element Checker Runtime Agreement") {}

	void forge_tests() override {
		struct row { const char* name; const char* source; };
		const row cases[] = {
			{"E01_int_takes_bool_lit", R"element(array<int> a = [true];
print("RAN");)element"},
			{"E02_int_takes_float_lit", R"element(array<int> a = [1.5];
print("RAN");)element"},
			{"E03_int_rejects_1e100", R"element(array<int> a = [1e100];
print("RAN");)element"},
			{"E04_string_takes_int_lit", R"element(array<string> s = [1];
print("RAN");)element"},
			{"E05_string_takes_bool_lit", R"element(array<string> s = [true];
print("RAN");)element"},
			{"E06_bool_takes_int_lit", R"element(array<bool> b = [1];
print("RAN");)element"},
			{"E07_char_takes_int_lit", R"element(array<char> c = [65];
print("RAN");)element"},
			{"E08_char_takes_char_lit", R"element(array<char> c = ['a'];
print("RAN");)element"},
			{"E09_float_takes_int_lit", R"element(array<float> f = [1];
print("RAN");)element"},
			{"E10_float_takes_bool_lit", R"element(array<float> f = [true];
print("RAN");)element"},
			{"E11_class_takes_null_lit", R"element(class Foo { int n = 0; } array<Foo> a = [null];
print("RAN");)element"},
			{"E12_base_takes_derived_lit", R"element(class Foo { int n = 0; } class Bar : Foo { int m = 0; } array<Foo> a = [Bar()];
print("RAN");)element"},
			{"E13_derived_rejects_base_lit", R"element(class Foo { int n = 0; } class Bar : Foo { int m = 0; } array<Bar> a = [Foo()];
print("RAN");)element"},
			{"E14_class_rejects_unrelated_lit", R"element(class Foo { int n = 0; } class Bar { int m = 0; } array<Foo> a = [Bar()];
print("RAN");)element"},
			{"E15_class_ctor_conversion_lit", R"element(class Foo { int n = 0; Foo(int v) { n = v; } } array<Foo> a = [5];
print("RAN");)element"},
			{"E16_int_takes_to_int_object_lit", R"element(class Foo { int to_int() { return 1; } } array<int> a = [Foo()];
print("RAN");)element"},
			{"E17_int_array_to_string_array", R"element(array<int> a = [1]; array<string> s = a;
print("RAN");)element"},
			{"E18_empty_int_array_to_string_array", R"element(array<int> e = []; array<string> s = e;
print("RAN");)element"},
			{"E19_int_array_to_float_array", R"element(array<int> a = [1]; array<float> f = a;
print("RAN");)element"},
			{"E20_float_array_to_int_array", R"element(array<float> f = [1.5]; array<int> a = f;
print("RAN");)element"},
			{"E21_var_array_mixed_to_int_array", R"element(array<var> v = [1, "s"]; array<int> a = v;
print("RAN");)element"},
			{"E22_int_array_to_var_array_then_push_string", R"element(array<int> a = [1]; array<var> v = a; v.push("s");
print("RAN");)element"},
			{"E23_int_array_into_var_then_push_string", R"element(array<int> a = [1]; var v = a; v.push("s");
print("RAN");)element"},
			{"E24_int_array_into_auto_then_push_string", R"element(array<int> a = [1]; auto v = a; v.push("s");
print("RAN");)element"},
			{"E25_map_int_value_takes_bool_lit", R"element(map<string, int> m = {"k": true};
print("RAN");)element"},
			{"E26_map_int_value_rejects_string_lit", R"element(map<string, int> m = {"k": "v"};
print("RAN");)element"},
			{"E27_map_int_key_rejects_string_key", R"element(map<int, int> m = {"k": 1};
print("RAN");)element"},
			{"E28_map_float_value_takes_int_lit", R"element(map<string, float> m = {"k": 1};
print("RAN");)element"},
			{"E29_map_var_key_mixed", R"element(map<var, int> m = {1: 1, "s": 2};
print("RAN");)element"},
			{"E30_map_nested_int_array_takes_float", R"element(map<string, array<int>> m = {"k": [1.5]};
print("RAN");)element"},
			{"E31_map_nested_int_array_rejects_string", R"element(map<string, array<int>> m = {"k": ["s"]};
print("RAN");)element"},
			{"E32_nested_int_arrays_ok", R"element(array<array<int>> a = [[1], []];
print("RAN");)element"},
			{"E33_nested_array_takes_null", R"element(array<array<int>> a = [null];
print("RAN");)element"},
			{"E34_push_bool_into_int", R"element(array<int> a = [1]; a.push(true);
print("RAN");)element"},
			{"E35_push_float_into_int", R"element(array<int> a = [1]; a.push(2.5);
print("RAN");)element"},
			{"E36_push_string_into_int", R"element(array<int> a = [1]; a.push("s");
print("RAN");)element"},
			{"E37_push_int_into_string", R"element(array<string> s = []; s.push(1);
print("RAN");)element"},
			{"E38_store_bool_into_int", R"element(array<int> a = [1]; a[0] = true;
print("RAN");)element"},
			{"E39_store_float_into_int", R"element(array<int> a = [1]; a[0] = 2.5;
print("RAN");)element"},
			{"E40_store_string_into_int", R"element(array<int> a = [1]; a[0] = "s";
print("RAN");)element"},
			{"E41_store_int_into_string", R"element(array<string> s = ["a"]; s[0] = 1;
print("RAN");)element"},
			{"E42_read_int_element_into_string", R"element(array<int> a = [1]; int x = a[0]; string y = a[0];
print("RAN");)element"},
			{"E43_read_int_element_into_float", R"element(array<int> a = [1]; float f = a[0];
print("RAN");)element"},
			{"E44_read_string_element_into_int", R"element(array<string> s = ["a"]; int x = s[0];
print("RAN");)element"},
			{"E45_read_map_int_into_string", R"element(map<string, int> m = {"k": 1}; string s = m["k"];
print("RAN");)element"},
			{"E46_read_map_int_into_char", R"element(map<string, int> m = {"k": 1}; char c = m["k"];
print("RAN");)element"},
			{"E47_read_float_element_into_int", R"element(array<float> f = [1.5]; int i = f[0];
print("RAN");)element"},
			{"E48_push_int_expr", R"element(array<int> a = [1]; a.push(a[0] + 1);
print("RAN");)element"},
			{"E49_push_null_into_int", R"element(array<int> a = [1]; a.push(null);
print("RAN");)element"},
			{"E50_push_derived_into_base", R"element(class Foo { int n = 0; } class Bar : Foo { int m = 0; } array<Foo> a = []; a.push(Bar());
print("RAN");)element"},
			{"E51_push_int_into_class", R"element(class Foo { int n = 0; } array<Foo> a = []; a.push(5);
print("RAN");)element"},
			{"E52_store_var_held_instance", R"element(class Foo { int n = 0; } array<Foo> a = [Foo()]; var v = Foo(); a[0] = v; print(type_of(a[0]) + ":" + to_string(a[0].n));
print("RAN");)element"},
			{"E53_push_var_held_instance", R"element(class Foo { int n = 0; } array<Foo> a = []; var v = Foo(); a.push(v); print(type_of(a[0]) + ":" + to_string(a[0].n));
print("RAN");)element"},
			{"E54_var_held_int_store_and_push", R"element(array<int> a = [1]; var v = 5; a[0] = v; a.push(v);
print("RAN");)element"},
			{"E55_var_held_string_push_into_int", R"element(array<int> a = [1]; var v = "s"; a.push(v);
print("RAN");)element"},
			{"E56_var_held_string_store_into_int", R"element(array<int> a = [1]; var v = "s"; a[0] = v;
print("RAN");)element"},
			{"E57_param_literal_converts", R"element(function f(array<int> xs) {} f([1.5]);
print("RAN");)element"},
			{"E58_param_literal_rejects", R"element(function f(array<int> xs) {} f(["s"]);
print("RAN");)element"},
			{"E59_return_literal_converts", R"element(function g() -> array<int> { return [1.5]; } g();
print("RAN");)element"},
			{"E60_return_literal_rejects", R"element(function g() -> array<int> { return ["s"]; } g();
print("RAN");)element"},
			{"E61_compound_float_into_int_element", R"element(array<int> a = [1]; a[0] += 1.5;
print("RAN");)element"},
			{"E62_compound_string_into_int_element", R"element(array<int> a = [1]; a[0] += "s";
print("RAN");)element"},
			{"E63_store_int_plus_float", R"element(array<int> a = [1]; a[0] = a[0] + 0.5;
print("RAN");)element"},
			{"E64_increment_element", R"element(array<int> a = [1]; a[0]++;
print("RAN");)element"},
			{"E65_string_compound_int", R"element(array<string> s = ["a"]; s[0] += 1;
print("RAN");)element"},
			{"E66_member_store_through_element_rejects", R"element(class Foo { int n = 0; } array<Foo> a = [Foo()]; a[0].n = "s";
print("RAN");)element"},
			{"E67_member_store_through_element_converts", R"element(class Foo { int n = 0; } array<Foo> a = [Foo()]; a[0].n = 2.5;
print("RAN");)element"},
			{"E68_member_read_through_element", R"element(class Foo { int n = 0; } array<Foo> a = [Foo()]; int k = a[0].n; string t = a[0].n;
print("RAN");)element"},
			{"E69_missing_member_through_element", R"element(class Foo { int n = 0; } array<Foo> a = [Foo()]; a[0].missing = 1;
print("RAN");)element"},
			{"E70_member_on_int_element", R"element(array<int> a = [1]; a[0].n = 1;
print("RAN");)element"},
			{"E71_map_class_value_takes_null", R"element(class Foo { int n = 0; } map<string, Foo> m = {"k": null};
print("RAN");)element"},
			{"E73_push_through_typed_reference", R"element(array<int> a = [1]; array<int>& r = a; r.push("s");
print("RAN");)element"},
			{"E74_store_through_auto_ref_loop", R"element(array<int> a = [1]; for (auto& x : a) { x = "s"; }
print("RAN");)element"},
			{"E75_push_array_into_int", R"element(array<int> a = [1]; a.push(a);
print("RAN");)element"},
			{"E76_var_element_into_int_array", R"element(array<var> v = []; v.push([1]); array<int> b = v[0];
print("RAN");)element"},
			{"E77_push_comparison_into_int", R"element(array<int> a = [1]; a.push(1 == 1);
print("RAN");)element"},
			{"E78_push_char_into_int", R"element(array<int> a = [1]; a.push('a');
print("RAN");)element"},
			{"E79_push_int_into_float", R"element(array<float> f = [1.5]; f.push(1);
print("RAN");)element"},
			{"E80_push_int_and_string_into_bool", R"element(array<bool> b = []; b.push(0); b.push("s");
print("RAN");)element"},
			{"E81_push_string_into_char", R"element(array<char> c = []; c.push("a");
print("RAN");)element"},
			{"E82_push_size_result", R"element(array<int> a = [1]; a.push(a.size());
print("RAN");)element"},
			{"E83_push_string_concat", R"element(array<string> s = ["a"]; s.push(s[0] + 1);
print("RAN");)element"},
			{"E84_insert_string_into_int", R"element(array<int> a = [1]; a.insert(0, "s");
print("RAN");)element"},
			{"E85_store_null_into_int_element", R"element(array<int> a = [1]; a[0] = null;
print("RAN");)element"},
			{"E86_store_null_into_class_element", R"element(class Foo { int n = 0; } array<Foo> a = [Foo()]; a[0] = null;
print("RAN");)element"},
			{"E87_map_store_bool_into_int", R"element(map<string, int> m = {}; m["k"] = true;
print("RAN");)element"},
			{"E88_map_store_string_into_int", R"element(map<string, int> m = {}; m["k"] = "s";
print("RAN");)element"},
			{"E89_map_store_int_key_into_string_key", R"element(map<string, int> m = {}; m[5] = 1;
print("RAN");)element"},
			{"E90_map_compound_float", R"element(map<string, int> m = {"k": 1}; m["k"] += 1.5;
print("RAN");)element"},
			{"E91_push_ternary", R"element(array<int> a = [1]; a.push(1 == 1 ? 2 : 3);
print("RAN");)element"},
			{"E92_push_string_size", R"element(array<int> a = [1]; string s = "x"; a.push(s.size());
print("RAN");)element"},
			{"E93_push_typed_float_local_into_int", R"element(array<int> a = [1]; float f = 1.5; a.push(f);
print("RAN");)element"},
			{"E94_push_typed_string_local_into_int", R"element(array<int> a = [1]; string s = "x"; a.push(s);
print("RAN");)element"},
			{"E95_push_typed_bool_local_into_int", R"element(array<int> a = [1]; bool b = true; a.push(b);
print("RAN");)element"},
			{"E96_push_typed_char_local_into_int", R"element(array<int> a = [1]; char c = 'a'; a.push(c);
print("RAN");)element"},
			{"E97_push_typed_int_local_into_string", R"element(array<string> s = []; int n = 1; s.push(n);
print("RAN");)element"},
			{"E98_push_null_into_string", R"element(array<string> s = []; s.push(null);
print("RAN");)element"},
			{"E99_push_and_store_typed_instance", R"element(class Foo { int n = 0; } array<Foo> a = []; Foo f = Foo(); a.push(f); a[0] = f;
print("RAN");)element"},
			{"F00_push_and_store_typed_derived", R"element(class Foo { int n = 0; } class Bar : Foo { int m = 0; } array<Foo> a = []; Bar b = Bar(); a.push(b); a[0] = b;
print("RAN");)element"},
			{"F01_push_shared_handle_into_class", R"element(class Foo { int n = 0; } array<Foo> a = []; shared_ptr<Foo> h = Foo(); a.push(h);
print("RAN");)element"},
			{"F02_push_instance_into_shared_array", R"element(class Foo { int n = 0; } array<shared_ptr<Foo>> a = []; a.push(Foo());
print("RAN");)element"},
			{"F03_push_int_plus_float", R"element(array<int> a = [1]; a.push(1 + 1.5);
print("RAN");)element"},
			{"F04_push_string_plus_int", R"element(array<int> a = [1]; a.push("1" + 1);
print("RAN");)element"},
			{"F05_assign_string_literal_array", R"element(array<int> a = [1]; a = ["s"];
print("RAN");)element"},
			{"F06_assign_float_literal_array", R"element(array<int> a = [1]; a = [1.5];
print("RAN");)element"},
			{"F07_assign_empty_literal", R"element(array<int> a = [1]; a = [];
print("RAN");)element"},
			{"F08_assign_null_literal_array", R"element(array<int> a = [1]; a = [null];
print("RAN");)element"},
			{"F09_assign_nested_literal_array", R"element(array<int> a = [1]; a = [[1]];
print("RAN");)element"},
			{"F10_var_array_to_int_array_decl", R"element(array<var> v = [1]; array<int> a = v;
print("RAN");)element"},
			{"F11_var_to_int_array_then_push_string", R"element(var v = [1, 2]; array<int> a = v; a.push("s");
print("RAN");)element"},
			{"F12_store_string_at_var_index", R"element(array<int> a = [1]; var i = 0; a[i] = "s";
print("RAN");)element"},
			{"F13_store_char_into_int_element", R"element(array<int> a = [1]; a[0] = 'a';
print("RAN");)element"},
			{"F14_scalar_int_takes_char", R"element(int x = 'a';
print("RAN");)element"},
			{"F15_scalar_string_takes_int", R"element(string s = 1;
print("RAN");)element"},
			{"F16_scalar_class_takes_var_held", R"element(class Foo { int n = 0; } var v = Foo(); Foo f = v; print(type_of(f));
print("RAN");)element"},
			{"F17_scalar_int_takes_bool", R"element(int x = true;
print("RAN");)element"},
			{"F18_scalar_int_takes_float", R"element(int x = 1.5;
print("RAN");)element"},
			{"F19_scalar_char_takes_int", R"element(char c = 65;
print("RAN");)element"},
			{"F20_typed_read_then_int_decl_from_string_array", R"element(array<string> s = ["1"]; auto x = s[0]; int y = x;
print("RAN");)element"},
			{"F21_element_into_auto_then_typed", R"element(array<int> a = [1]; auto x = a[0]; string y = x;
print("RAN");)element"},
			{"F22_map_read_missing_key", R"element(map<string, int> m = {}; int x = m["missing"];
print("RAN");)element"},
			{"F23_map_read_into_class", R"element(class Foo { int n = 0; } map<string, Foo> m = {}; Foo f = m["k"];
print("RAN");)element"},
			{"F24_push_instance_into_int_names_class", R"element(class Foo { int n = 0; } array<int> a = [1]; a.push(Foo());
print("RAN");)element"},
			{"F25_store_var_held_instance_into_int_names_class", R"element(class Foo { int n = 0; } array<int> a = [1]; var v = Foo(); a[0] = v;
print("RAN");)element"},
			{"F26_two_dim_store_string", R"element(array<array<int>> g = [[1]]; g[0][0] = "s";
print("RAN");)element"},
			{"F27_two_dim_store_float", R"element(array<array<int>> g = [[1]]; g[0][0] = 1.5;
print("RAN");)element"},
			{"F28_two_dim_push_string", R"element(array<array<int>> g = [[1]]; g[0].push("s");
print("RAN");)element"},
			{"F29_inner_read_into_string", R"element(array<array<int>> g = [[1]]; string s = g[0][0];
print("RAN");)element"},
			{"F30_map_of_arrays_push_string", R"element(map<string, array<int>> m = {"k": [1]}; m["k"].push("s");
print("RAN");)element"},
		};
		for (const auto& item : cases) {
			test(item.name, [this, source = std::string(item.source)]() {
				auto eng = make_engine();
				jai::stdlib::register_all(*eng);
				const auto report = eng->check(source);
				bool runtime_succeeded = false;
				try {
					(void)eng->execute("[[warn_mode]]; " + source);   // run past the checker's verdict: the runtime is the oracle
					runtime_succeeded = true;
				} catch (const std::exception&) {
					// Dynamic or value-dependent failures may remain runtime checks.
				}
				check_false(report.has_errors() && runtime_succeeded,
					"checker rejected a program that ran:\n" + source + "\n" + report.format(source));
			});
		}
	}
};

} // namespace jai::foundry::tests

using element_checker_runtime_tests = jai::foundry::tests::element_checker_runtime_tests;
FOUNDRY_REGISTER(element_checker_runtime_tests)
