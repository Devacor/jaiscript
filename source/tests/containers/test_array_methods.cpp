#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include "../case_table.hpp"

namespace jai::foundry::tests {

class array_methods_tests : public suite {
public:
    array_methods_tests() : suite("Array Methods") {}

    void forge_tests() override {
        test("index_of_first_last_and_length_read_an_array", [this]() {
            check_results<int>({
                {"var arr = [1, 2, 3, 4, 5]; arr.index_of(3);", 2},
                {"var arr = [1, 2, 3]; arr.index_of(10);", -1},
                {"var arr = [10, 20, 30]; arr.first();", 10},
                {"var arr = [10, 20, 30]; arr.last();", 30},
                {"var arr = [1, 2, 3, 4, 5]; arr.length();", 5},
            });
        });

        test("has_and_contains_test_membership", [this]() {
            check_results<bool>({
                {"var arr = [1, 2, 3]; arr.has(2);", true},
                {"var arr = [1, 2, 3]; arr.has(10);", false},
                {"var arr = [1, 2, 3]; arr.contains(2);", true},
            });
        });

        test("slice_positive", [this]() {
            auto eng = make_engine();
            auto result = eng->execute("var arr = [1, 2, 3, 4, 5]; array::slice(arr, 1, 3);");
            const auto& arr = result.as_array();
            check_eq(arr.size(), 2);
            check_eq(arr[0].as<int>(), 2);
            check_eq(arr[1].as<int>(), 3);
        });

        test("slice_negative", [this]() {
            auto eng = make_engine();
            auto result = eng->execute("var arr = [1, 2, 3, 4, 5]; array::slice(arr, -3, -1);");
            const auto& arr = result.as_array();
            check_eq(arr.size(), 2);
            check_eq(arr[0].as<int>(), 3);
            check_eq(arr[1].as<int>(), 4);
        });

        test("filter", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var arr = [1, 2, 3, 4, 5, 6];
                array::filter(arr, [](auto x) -> auto { return x % 2 == 0; });
            )");
            const auto& arr = result.as_array();
            check_eq(arr.size(), 3);
            check_eq(arr[0].as<int>(), 2);
            check_eq(arr[1].as<int>(), 4);
            check_eq(arr[2].as<int>(), 6);
        });

        test("sort", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var arr = [5, 2, 8, 1, 9];
                arr.sort();
                arr;
            )");
            const auto& arr = result.as_array();
            check_eq(arr.size(), 5);
            check_eq(arr[0].as<int>(), 1);
            check_eq(arr[1].as<int>(), 2);
            check_eq(arr[2].as<int>(), 5);
            check_eq(arr[3].as<int>(), 8);
            check_eq(arr[4].as<int>(), 9);
        });

        test("sort_with_custom_comparator", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var arr = [1, 2, 3, 4, 5];
                arr.sort([](auto a, auto b) -> auto { return a > b; });
                arr;
            )");
            const auto& arr = result.as_array();
            check_eq(arr.size(), 5);
            check_eq(arr[0].as<int>(), 5);
            check_eq(arr[1].as<int>(), 4);
            check_eq(arr[2].as<int>(), 3);
            check_eq(arr[3].as<int>(), 2);
            check_eq(arr[4].as<int>(), 1);
        });

        test("reverse", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var arr = [1, 2, 3, 4, 5];
                arr.reverse();
                arr;
            )");
            const auto& arr = result.as_array();
            check_eq(arr.size(), 5);
            check_eq(arr[0].as<int>(), 5);
            check_eq(arr[1].as<int>(), 4);
            check_eq(arr[2].as<int>(), 3);
            check_eq(arr[3].as<int>(), 2);
            check_eq(arr[4].as<int>(), 1);
        });

        test("remove_by_index", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var arr = [1, 2, 3, 4, 5];
                var removed = arr.remove(2);
                removed;
            )");
            check_eq(result.as<bool>(), true);

            result = eng->execute("arr.length();");
            check_eq(result.as<int>(), 4);
        });

        test("remove_if", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var arr = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
                arr.remove_if([](auto x) -> auto { return x % 2 == 0; });
            )");
            check_eq(result.as<int>(), 5);

            result = eng->execute("arr.length();");
            check_eq(result.as<int>(), 5);
        });

        // Pins that subscript args stay BY-VALUE at the builtin boundary
        // (deref_builtin_args, builtin_methods.hpp): array lookups already deref'd
        // via operator== and push copies via vm_convert_array_element - these must
        // not regress while the map/string boundaries normalize.
        test("subscript_args_stay_by_value", [this]() {
            auto eng = make_engine();
            auto result = eng->execute(R"(
                var src = [5, 6];
                var dst = [];
                dst.push(src[0]);
                src[0] = 50;
                var arr = [5, 6, 7];
                arr.has(src[1]) && arr.index_of(src[1]) == 1 && dst[0] == 5;
            )");
            check_eq(result.as<bool>(), true);
        });

        // std::vector's sizing: vector<T>(n) value-initializes n elements, vector<T>(n, v) copies v n times, vector<T>{a, b} is
        // the list; resize, assign and fill run the same constructors a C++ vector runs, and reserve changes nothing observable
        test("sized_construction_and_resize_follow_std_vector", [this]() {
            const std::string prelude =
                "class C { int n = 1; C() { print(\"ctor\"); } C(const C& o) { n = o.n + 10; print(\"copy\"); } }\n"
                "class D { int n = 1; void operator=(const D& o) { n = o.n + 100; print(\"assign\"); } }\n";
            const std::pair<const char*, const char*> cases[] = {
                {"print(array<int>(3));", "[0, 0, 0]"},
                {"print(array<int>(3, 7));", "[7, 7, 7]"},
                {"array<int> a(2, 5); print(a);", "[5, 5]"},
                {"array<string> a(2, \"x\"); print(a);", "[\"x\", \"x\"]"},
                {"auto a = array<bool>(2); print(a);", "[false, false]"},
                {"print(array<array<int>>(2));", "[[], []]"},
                {"print(array<int>{3}, \" \", array<int>{1, 2});", "[3] [1, 2]"},
                {"array<int> a{4, 5}; print(a);", "[4, 5]"},
                {"print(array<int>(), \" \", array<int>{});", "[] []"},
                {"array<int> xs = [1, 2, 3]; xs.resize(5); print(xs); xs.resize(1); print(xs); xs.resize(3, 9); print(xs);",
                 "[1, 2, 3, 0, 0]\n[1]\n[1, 9, 9]"},
                {"array<int> xs = [1, 2, 3]; xs.assign(2, 4); print(xs); xs.fill(6); print(xs);", "[4, 4]\n[6, 6]"},
                {"array<int> xs = [1, 2]; xs.reserve(1000); print(xs.size(), \" \", xs);", "2 [1, 2]"},
                {"array<int> xs = [1, 2]; auto ys = array::resize(xs, 4, 3); auto zs = array::fill(xs, 0); print(xs, ys, zs);",
                 "[1, 2][1, 2, 3, 3][0, 0]"},
                {"var v = [1, \"a\"]; v.resize(3); print(v);", "[1, \"a\", null]"},
                {"array<C> a(2); print(a[0].n, a[1].n);", "ctor\nctor\n11"},
                {"C c; c.n = 5; array<C> a(2, c); print(a[0].n, a[1].n);", "ctor\ncopy\ncopy\n1515"},
                {"array<C> a; a.resize(1); C c; a.resize(3, c); a.fill(c); print(a.size());", "ctor\nctor\ncopy\ncopy\n3"},
                {"D d; d.n = 4; array<D> a(2); a.fill(d); print(a[0].n, a[1].n);", "assign\nassign\n104104"},
                {"D d; array<D> a(1); a.assign(3, d); print(a.size(), a[0].n, a[2].n);", "assign\n31011"},
                {"array<int> xs; xs.resize(-1);", "ERROR array size cannot be negative"},
                {"auto a = array<int>(-2);", "ERROR array size cannot be negative"},
                {"var count = \"3\"; array<int> xs; xs.resize(count);", "ERROR resize() size must be an int"},
                {"var s = \"x\"; auto a = array<int>(2, s);", "ERROR Cannot push 'string' to array<int>"},
                {"array<uint8> b; var big = 300; b.resize(2, big);", "ERROR Cannot store 300 in array<uint8> (0..255)"},
                {"array<int> xs = [1]; var s = \"x\"; xs.fill(s);", "ERROR Cannot push 'string' to array<int>"},
                {"array<int> xs; xs.fill(\"s\");", "ERROR error: fill() cannot store into 'array<int>': expected 'int', got 'string'"},
                {"auto a = array<int>(2, \"s\");", "ERROR error: resize() cannot store into 'array<int>': expected 'int', got 'string'"},
                {"array<int> xs; xs.resize(\"2\");", "ERROR error: resize() size must be an int, got 'string'"},
                {"array<int> a(1, 2, 3);", "ERROR array<T>(...) takes a size and an optional value, not 3 arguments"},
            };
            check_outputs(cases, prelude);
        });
    }
};

} // namespace jai::foundry::tests

using array_methods_tests = jai::foundry::tests::array_methods_tests;
FOUNDRY_REGISTER(array_methods_tests)
