#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class array_tests : public suite {
public:
    array_tests() : suite("Array Tests") {}

    void forge_tests() override {
        test("array_literal_basic", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto arr = [1, 2, 3, 4, 5];
            )");

            auto result = eng->execute("arr");
            check(result.is_array());

            auto vec = result.as<std::vector<script_value>>();
            check_eq(vec.size(), 5);
            check_eq(vec[0].as<int>(), 1);
            check_eq(vec[4].as<int>(), 5);
        });

        test("array_literal_mixed_types_with_var", [&]() {
            // Mixed-type arrays require 'var' declaration (heterogeneous)
            auto eng = make_engine();

            eng->execute(R"(
                var mixed = [1, "hello", 3.14, true];
            )");

            auto result = eng->execute("mixed");
            auto vec = result.as<std::vector<script_value>>();

            check_eq(vec.size(), 4);
            check_eq(vec[0].as<int>(), 1);
            check_eq(vec[1].as<std::string>(), "hello");
            check_eq(vec[2].as<double>(), 3.14);
            check_eq(vec[3].as<bool>(), true);
        });

        test("array_literal_auto_requires_homogeneous", [&]() {
            // auto x = [...] requires all elements to be the same type
            auto eng = make_engine();

            // This should fail - mixed types with auto
            bool threw = false;
            try {
                eng->execute(R"(
                    auto mixed = [1, "hello"];
                )");
            } catch (const std::exception&) {
                threw = true;
            }
            check(threw);  // Should fail with heterogeneous array

            // This should succeed - homogeneous types with auto
            eng->execute(R"(
                auto nums = [1, 2, 3];
            )");
            auto arr = eng->execute("nums");
            check(arr.is_array());
            check_eq(arr.as<std::vector<script_value>>().size(), 3);
        });

        test("array_literal_empty", [&]() {
            auto eng = make_engine();

            eng->execute("auto empty = [];");

            auto result = eng->execute("empty");
            auto vec = result.as<std::vector<script_value>>();
            check_eq(vec.size(), 0);
        });

        test("array_indexing_read", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto arr = [10, 20, 30, 40, 50];
            )");

            check_eq(eng->execute("arr[0]").as<int>(), 10);
            check_eq(eng->execute("arr[2]").as<int>(), 30);
            check_eq(eng->execute("arr[4]").as<int>(), 50);
        });

        test("array_indexing_write", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto arr = [1, 2, 3];
                arr[0] = 100;
                arr[1] = 200;
                arr[2] = 300;
            )");

            check_eq(eng->execute("arr[0]").as<int>(), 100);
            check_eq(eng->execute("arr[1]").as<int>(), 200);
            check_eq(eng->execute("arr[2]").as<int>(), 300);
        });

        test("array_push_operation", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto arr = [1, 2, 3];
                arr.push(4);
                arr.push(5);
            )");

            auto result = eng->execute("arr");
            auto vec = result.as<std::vector<script_value>>();
            check_eq(vec.size(), 5);
            check_eq(vec[3].as<int>(), 4);
            check_eq(vec[4].as<int>(), 5);
        });

        test("array_pop_operation", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto arr = [1, 2, 3, 4, 5];
                auto last = arr.pop();
            )");

            check_eq(eng->execute("last").as<int>(), 5);

            auto arr_result = eng->execute("arr");
            auto vec = arr_result.as<std::vector<script_value>>();
            check_eq(vec.size(), 4);
        });

        test("array_size_method", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto arr = [1, 2, 3];
            )");

            check_eq(eng->execute("arr.size()").as<int>(), 3);

            eng->execute("arr.push(4);");
            check_eq(eng->execute("arr.size()").as<int>(), 4);

            eng->execute("arr.pop();");
            check_eq(eng->execute("arr.size()").as<int>(), 3);
        });

        test("array_empty_method", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto arr = [];
            )");

            check_eq(eng->execute("arr.empty()").as<bool>(), true);

            eng->execute("arr.push(1);");
            check_eq(eng->execute("arr.empty()").as<bool>(), false);
        });

        test("array_clear_method", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto arr = [1, 2, 3, 4, 5];
                arr.clear();
            )");

            check_eq(eng->execute("arr.size()").as<int>(), 0);
            check_eq(eng->execute("arr.empty()").as<bool>(), true);
        });

        test("array_nested_arrays", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto matrix = [[1, 2, 3], [4, 5, 6], [7, 8, 9]];
            )");

            // Check outer array
            auto result = eng->execute("matrix");
            auto outer = result.as<std::vector<script_value>>();
            check_eq(outer.size(), 3);

            // Check first row
            check_eq(eng->execute("matrix[0][0]").as<int>(), 1);
            check_eq(eng->execute("matrix[0][1]").as<int>(), 2);
            check_eq(eng->execute("matrix[0][2]").as<int>(), 3);

            // Check second row
            check_eq(eng->execute("matrix[1][0]").as<int>(), 4);
            check_eq(eng->execute("matrix[1][1]").as<int>(), 5);
            check_eq(eng->execute("matrix[1][2]").as<int>(), 6);

            // Check third row
            check_eq(eng->execute("matrix[2][0]").as<int>(), 7);
            check_eq(eng->execute("matrix[2][1]").as<int>(), 8);
            check_eq(eng->execute("matrix[2][2]").as<int>(), 9);
        });

        test("array_nested_modification", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto matrix = [[1, 2], [3, 4]];
                matrix[0][0] = 100;
                matrix[1][1] = 200;
            )");

            check_eq(eng->execute("matrix[0][0]").as<int>(), 100);
            check_eq(eng->execute("matrix[0][1]").as<int>(), 2);
            check_eq(eng->execute("matrix[1][0]").as<int>(), 3);
            check_eq(eng->execute("matrix[1][1]").as<int>(), 200);
        });

        test("array_deeply_nested", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto deep = [[[1, 2], [3, 4]], [[5, 6], [7, 8]]];
            )");

            check_eq(eng->execute("deep[0][0][0]").as<int>(), 1);
            check_eq(eng->execute("deep[0][1][1]").as<int>(), 4);
            check_eq(eng->execute("deep[1][0][0]").as<int>(), 5);
            check_eq(eng->execute("deep[1][1][1]").as<int>(), 8);
        });

        test("array_with_nested_mixed_types", [&]() {
            auto eng = make_engine();

            // Use 'var' for heterogeneous nested arrays
            // 'auto' requires homogeneous elements at all levels
            eng->execute(R"(
                var complex = [
                    [1, "hello"],
                    [3.14, true],
                    [[1, 2], "nested"]
                ];
            )");

            check_eq(eng->execute("complex[0][0]").as<int>(), 1);
            check_eq(eng->execute("complex[0][1]").as<std::string>(), "hello");
            check_eq(eng->execute("complex[1][0]").as<double>(), 3.14);
            check_eq(eng->execute("complex[1][1]").as<bool>(), true);
            check_eq(eng->execute("complex[2][1]").as<std::string>(), "nested");
        });

        test("array_iteration_for_loop", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto arr = [1, 2, 3, 4, 5];
                auto sum = 0;
                for (auto i = 0; i < arr.size(); i = i + 1) {
                    sum = sum + arr[i];
                }
            )");

            check_eq(eng->execute("sum").as<int>(), 15);
        });

        test("array_bounds_checking", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                auto arr = [1, 2, 3];
            )");

            // Test valid access
            check_eq(eng->execute("arr[0]").as<int>(), 1);
            check_eq(eng->execute("arr[2]").as<int>(), 3);

            // Out of bounds access should throw
            check_throws([&]() {
                eng->execute("arr[10]");
            });

            check_throws([&]() {
                eng->execute("arr[-1]");
            });
        });

        test("array_in_function_parameter", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                function processArray(auto arr) -> int {
                    auto sum = 0;
                    for (auto i = 0; i < arr.size(); i = i + 1) {
                        sum = sum + arr[i];
                    }
                    return sum;
                }
            )");

            check_eq(eng->execute("processArray([1, 2, 3, 4, 5])").as<int>(), 15);
            check_eq(eng->execute("processArray([10, 20, 30])").as<int>(), 60);
        });

        test("array_as_return_value", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                function makeArray() -> auto {
                    return [1, 2, 3];
                }
            )");

            auto result = eng->execute("makeArray()");
            auto vec = result.as<std::vector<script_value>>();
            check_eq(vec.size(), 3);
            check_eq(vec[0].as<int>(), 1);
            check_eq(vec[2].as<int>(), 3);
        });

        // TODO: Implement array concatenation operator
        // test("array_concatenation", [&]() {
        //     auto eng = make_engine();

        //     eng->execute(R"(
        //         auto arr1 = [1, 2, 3];
        //         auto arr2 = [4, 5, 6];
        //         auto combined = arr1 + arr2;
        //     )");

        //     auto result = eng->execute("combined");
        //     auto vec = result.as<std::vector<script_value>>();
        //     check_eq(vec.size(), 6);
        //     check_eq(vec[0].as<int>(), 1);
        //     check_eq(vec[5].as<int>(), 6);
        // });

        // TODO: Implement array slice method
        // test("array_slice_operation", [&]() {
        //     auto eng = make_engine();

        //     eng->execute(R"(
        //         auto arr = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
        //         auto slice = array::slice(arr, 2, 5);
        //     )");

        //     auto result = eng->execute("slice");
        //     auto vec = result.as<std::vector<script_value>>();
        //     check_eq(vec.size(), 3);
        //     check_eq(vec[0].as<int>(), 2);
        //     check_eq(vec[1].as<int>(), 3);
        //     check_eq(vec[2].as<int>(), 4);
        // });

        test("concatenate_arrays", [&]() {
            auto eng = make_engine();
            stdlib::register_all(eng);

            eng->execute(R"(
                auto arr1 = [1, 2, 3];
                auto arr2 = [4, 5, 6];
                auto result = array::append(arr1, arr2);
            )");

            auto result = eng->execute("result");
            auto vec = result.as<std::vector<script_value>>();
            check_eq(vec.size(), 6);
            check_eq(vec[0].as<int>(), 1);
            check_eq(vec[2].as<int>(), 3);
            check_eq(vec[3].as<int>(), 4);
            check_eq(vec[5].as<int>(), 6);
        });

        test("append_arrays", [&]() {
            auto eng = make_engine();
            stdlib::register_all(eng);

            eng->execute(R"(
                auto arr1 = ["a", "b"];
                auto arr2 = ["c", "d", "e"];
                auto result = array::append(arr1, arr2);
            )");

            auto result = eng->execute("result");
            auto vec = result.as<std::vector<script_value>>();
            check_eq(vec.size(), 5);
            check_eq(vec[0].as<std::string>(), "a");
            check_eq(vec[1].as<std::string>(), "b");
            check_eq(vec[2].as<std::string>(), "c");
            check_eq(vec[3].as<std::string>(), "d");
            check_eq(vec[4].as<std::string>(), "e");
        });

        test("concatenate_strings", [&]() {
            auto eng = make_engine();
            stdlib::register_all(eng);

            eng->execute(R"(
                auto str1 = "Hello ";
                auto str2 = "World";
                auto result = string::append(str1, str2);
            )");

            check_eq(eng->execute("result").as<std::string>(), "Hello World");
        });

        test("append_strings", [&]() {
            auto eng = make_engine();
            stdlib::register_all(eng);

            eng->execute(R"(
                auto greeting = string::append("Good ", "morning");
            )");

            check_eq(eng->execute("greeting").as<std::string>(), "Good morning");
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::array_tests)
