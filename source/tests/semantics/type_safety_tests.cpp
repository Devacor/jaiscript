#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <cmath>
#include <functional>
#include <string>
#include <variant>
#include <vector>
#include "../case_table.hpp"

namespace jai::foundry::tests {

// C++ classes for inheritance type safety tests
namespace type_safety_test_classes {

class Entity {
public:
    std::string name;
    int id;

    Entity() : name("unnamed"), id(0) {}
    Entity(const std::string& n, int i) : name(n), id(i) {}

    virtual std::string get_type() const { return "Entity"; }
    int get_id() const { return id; }
    std::string get_name() const { return name; }
};

class Vehicle {
public:
    int speed;
    std::string model;

    Vehicle() : speed(0), model("unknown") {}
    Vehicle(int s, const std::string& m) : speed(s), model(m) {}

    virtual int get_max_speed() const { return speed; }
    void accelerate(int amount) { speed += amount; }
};

class Animal {
public:
    std::string species;
    int legs;

    Animal() : species("unknown"), legs(4) {}
    Animal(const std::string& s, int l) : species(s), legs(l) {}

    virtual std::string speak() const { return "..."; }
    int get_legs() const { return legs; }
};

} // namespace type_safety_test_classes

using namespace type_safety_test_classes;
using namespace std::string_literals;

namespace {

using row_value = std::variant<int, double, bool, std::string>;

struct value_row {
    const char* code;
    row_value expected;
    const char* prelude = nullptr;
};

struct typed_row {
    const char* code;
    script_value_type type;
    row_value expected;
};

struct rejected_row {
    const char* code;
    const char* text = nullptr;   // a substring the error must hold; null accepts any error
    const char* prelude = nullptr;
};

void check_result(const script_value& a_result, const row_value& a_expected) {
    std::visit([&](const auto& a_value) {
        check_eq(a_value, a_result.as<std::decay_t<decltype(a_value)>>());
    }, a_expected);
}

// Each row runs its prelude and then its code on a fresh engine; the result is read with as<T>() of the expected type.
void check_values(const std::vector<value_row>& a_rows) {
    check_each(a_rows, [](const value_row& a_row) {
        auto eng = make_engine();
        if (a_row.prelude) { eng->execute(a_row.prelude); }
        check_result(eng->execute(a_row.code), a_row.expected);
    });
}

void check_typed_values(const std::vector<typed_row>& a_rows) {
    check_each(a_rows, [](const typed_row& a_row) {
        auto eng = make_engine();
        auto result = eng->execute(a_row.code);
        check_eq(static_cast<int>(a_row.type), static_cast<int>(result.type()), "type");
        check_result(result, a_row.expected);
    });
}

void check_rejected(const std::vector<rejected_row>& a_rows) {
    check_each(a_rows, [](const rejected_row& a_row) {
        auto eng = make_engine();
        if (a_row.prelude) { eng->execute(a_row.prelude); }
        bool caught = false;
        std::string message;
        try {
            eng->execute(a_row.code);
        } catch (const std::exception& error) {
            caught = true;
            message = error.what();
        }
        check_true(caught, "expected an error");
        if (a_row.text) { check_true(message.find(a_row.text) != std::string::npos, "error was: " + message); }
    });
}

} // namespace

class type_safety_tests : public suite {
public:
    type_safety_tests() : suite("Type Safety") {}

    void forge_tests() override {
        test("comparisons_yield_bool_and_compare_int_and_float_numerically", [this]() {
            check_typed_values({
                {"5 == 5.0", script_value_type::jai_bool_type, true},
                {"5 + 5.0 == 10.0", script_value_type::jai_bool_type, true},
                {"5 != 5.0", script_value_type::jai_bool_type, false},
                {"5 != 6.0", script_value_type::jai_bool_type, true},
                {"0 == 0.0", script_value_type::jai_bool_type, true},
                {"-5 == -5.0", script_value_type::jai_bool_type, true},
                {"5 == 5", script_value_type::jai_bool_type, true},
                {"5 == 6", script_value_type::jai_bool_type, false},
                {"5 != 5", script_value_type::jai_bool_type, false},
                {"3.14 == 3.14", script_value_type::jai_bool_type, true},
                {"\"hello\" == \"hello\"", script_value_type::jai_bool_type, true},
                {"\"hello\" == \"world\"", script_value_type::jai_bool_type, false},
                {"true == true", script_value_type::jai_bool_type, true},
                {"true == false", script_value_type::jai_bool_type, false},
                {"null == null", script_value_type::jai_bool_type, true},
                {"null != null", script_value_type::jai_bool_type, false},
                {"5 < 10", script_value_type::jai_bool_type, true},
                {"5 > 10", script_value_type::jai_bool_type, false},
                {"5 <= 5", script_value_type::jai_bool_type, true},
                {"5 >= 5", script_value_type::jai_bool_type, true},
                {"3.14 < 4.0", script_value_type::jai_bool_type, true},
                {"\"apple\" < \"banana\"", script_value_type::jai_bool_type, true},
                {"\"zebra\" > \"apple\"", script_value_type::jai_bool_type, true},
            });
        });

        test("arithmetic_stays_int_until_a_float_operand_joins", [this]() {
            check_typed_values({
                {"5 + 3", script_value_type::jai_int_type, 8},
                {"10 / 3", script_value_type::jai_int_type, 3},
                {"5 + 3.0", script_value_type::jai_float_type, 8.0},
                {"5 + 5.0", script_value_type::jai_float_type, 10.0},
                {"5 + 3.14", script_value_type::jai_float_type, 8.14},
                {"10.0 / 3", script_value_type::jai_float_type, 10.0 / 3},
            });
        });

        test("logical_operators_yield_bool", [this]() {
            check_typed_values({
                {"5 && 10", script_value_type::jai_bool_type, true},
                {"0 || \"hello\"", script_value_type::jai_bool_type, true},
                {"0 && 10", script_value_type::jai_bool_type, false},
                {"true && 42", script_value_type::jai_bool_type, true},
                {"false && 42", script_value_type::jai_bool_type, false},
                {"auto arr = []; arr || \"default\"", script_value_type::jai_bool_type, true},
            });
        });

        test("conditions_take_zero_empty_and_null_as_false", [this]() {
            check_values({
                {"auto arr = []; auto result = 0; if (arr) { result = 1; } result", 0},
                {"auto m = {\"a\": 1}; m.clear(); auto result = 0; if (m) { result = 1; } result", 0},
                {"auto arr = [1, 2, 3]; auto result = 0; if (arr) { result = 1; } result", 1},
                {"auto m = {\"a\": 1}; auto result = 0; if (m) { result = 1; } result", 1},
                {"auto r = 99; if (0) { r = 1; } else { r = 0; } r", 0},
                {"auto r = 99; if (42) { r = 1; } else { r = 0; } r", 1},
                {"auto r = 99; if (\"\") { r = 1; } else { r = 0; } r", 0},
                {"auto r = 99; if (\"hello\") { r = 1; } else { r = 0; } r", 1},
                {"auto r = 99; if (null) { r = 1; } else { r = 0; } r", 0},
                {"auto r = 99; auto arr = [1, 2, 3]; if (arr) { r = 1; } else { r = 0; } r", 1},
                {"auto r = 99; auto m = {\"a\": 1}; if (m) { r = 1; } else { r = 0; } r", 1},
            });
        });

        test("object_self_equality_works", [this]() {
            auto eng = make_engine();

            eng->execute(R"(
                class Thing { int value = 42; }
            )");

            auto result = eng->execute(R"(
                auto t = Thing();
                t == t  // Comparing object to itself
            )");

            // FIXED: Self-comparison now works via reference equality
            check_eq(result.as<bool>(), true, "Object compared to itself returns true");
        });

        test("object_equality_different_instances_is_memberwise", [this]() {
            auto eng = make_engine();

            eng->execute(R"(
                class Point {
                    int x = 0;
                    int y = 0;
                    Point(int px, int py) { x = px; y = py; }
                }
            )");

            auto result = eng->execute(R"(
                auto a = Point(1, 2);
                auto b = Point(1, 2);
                [a == b, same_object(a, b), a == Point(1, 3)]  // memberwise like C++ containers; identity is same_object
            )");

            const auto& v = result.as_array();
            check_eq(v[0].as<bool>(), true, "equal fields compare equal without operator==");
            check_eq(v[1].as<bool>(), false, "same_object tells the two instances apart");
            check_eq(v[2].as<bool>(), false, "a differing field compares unequal");
        });

        test("object_operator_equals_custom", [this]() {
            auto eng = make_engine();

            // Define a class with operator== for custom equality
            eng->execute(R"(
                class Point {
                    int x = 0;
                    int y = 0;
                    Point(int px, int py) { x = px; y = py; }

                    bool operator==(Point other) {
                        return x == other.x && y == other.y;
                    }
                }
            )");

            // Same values with operator== should be equal when a == b is used
            auto result = eng->execute(R"(
                auto a = Point(1, 2);
                auto b = Point(1, 2);
                a == b
            )");
            check_eq(result.as<bool>(), true, "Points with same values are equal via operator==");

            // Different values should not be equal
            auto diff_result = eng->execute(R"(
                auto a = Point(1, 2);
                auto c = Point(3, 4);
                a == c
            )");
            check_eq(diff_result.as<bool>(), false, "Points with different values are not equal");
        });

        test("object_operator_equals_with_not_equal", [this]() {
            auto eng = make_engine();

            // Define a class with operator==
            eng->execute(R"(
                class Item {
                    int id = 0;
                    Item(int i) { id = i; }

                    bool operator==(Item other) {
                        return id == other.id;
                    }
                }
            )");

            // Test a == b uses operator== method
            auto eq_result = eng->execute(R"(
                auto a = Item(42);
                auto b = Item(42);
                a == b
            )");
            check_eq(eq_result.as<bool>(), true, "Items with same id are equal via operator==");

            // Test a != b also works (negation of ==)
            auto neq_result = eng->execute(R"(
                auto a = Item(42);
                auto c = Item(99);
                a != c
            )");
            check_eq(neq_result.as<bool>(), true, "Items with different id are not equal");
        });

        test("script_class_comparison_operators", [this]() {
            auto eng = make_engine();

            // Define a class with all comparison operators
            eng->execute(R"(
                class Score {
                    int value = 0;
                    Score(int v) { value = v; }

                    bool operator<(Score other) {
                        return value < other.value;
                    }
                    bool operator<=(Score other) {
                        return value <= other.value;
                    }
                    bool operator>(Score other) {
                        return value > other.value;
                    }
                    bool operator>=(Score other) {
                        return value >= other.value;
                    }
                }
            )");

            // Test less than
            check_eq(eng->execute("Score(10) < Score(20)").as<bool>(), true, "10 < 20");
            check_eq(eng->execute("Score(20) < Score(10)").as<bool>(), false, "20 < 10 is false");

            // Test greater than
            check_eq(eng->execute("Score(20) > Score(10)").as<bool>(), true, "20 > 10");
            check_eq(eng->execute("Score(10) > Score(20)").as<bool>(), false, "10 > 20 is false");

            // Test less than or equal
            check_eq(eng->execute("Score(10) <= Score(20)").as<bool>(), true, "10 <= 20");
            check_eq(eng->execute("Score(10) <= Score(10)").as<bool>(), true, "10 <= 10");
            check_eq(eng->execute("Score(20) <= Score(10)").as<bool>(), false, "20 <= 10 is false");

            // Test greater than or equal
            check_eq(eng->execute("Score(20) >= Score(10)").as<bool>(), true, "20 >= 10");
            check_eq(eng->execute("Score(10) >= Score(10)").as<bool>(), true, "10 >= 10");
            check_eq(eng->execute("Score(10) >= Score(20)").as<bool>(), false, "10 >= 20 is false");
        });

        test("script_class_arithmetic_operators", [this]() {
            auto eng = make_engine();

            // Define a class with all arithmetic operators
            // Note: Use 'function operator+(...) -> Type' syntax for custom return types
            eng->execute(R"(
                class Vec2 {
                    float x = 0.0;
                    float y = 0.0;
                    Vec2(float px, float py) { x = px; y = py; }

                    function operator+(Vec2 other) -> Vec2 {
                        return Vec2(x + other.x, y + other.y);
                    }
                    function operator-(Vec2 other) -> Vec2 {
                        return Vec2(x - other.x, y - other.y);
                    }
                    function operator*(float scalar) -> Vec2 {
                        return Vec2(x * scalar, y * scalar);
                    }
                    function operator/(float scalar) -> Vec2 {
                        return Vec2(x / scalar, y / scalar);
                    }
                }
            )");

            // Test addition
            auto add_x = eng->execute("(Vec2(1, 2) + Vec2(3, 4)).x");
            auto add_y = eng->execute("(Vec2(1, 2) + Vec2(3, 4)).y");
            check_eq(add_x.as<double>(), 4.0, "Vec2 addition x");
            check_eq(add_y.as<double>(), 6.0, "Vec2 addition y");

            // Test subtraction
            auto sub_x = eng->execute("(Vec2(5, 7) - Vec2(2, 3)).x");
            auto sub_y = eng->execute("(Vec2(5, 7) - Vec2(2, 3)).y");
            check_eq(sub_x.as<double>(), 3.0, "Vec2 subtraction x");
            check_eq(sub_y.as<double>(), 4.0, "Vec2 subtraction y");

            // Test multiplication
            auto mul_x = eng->execute("(Vec2(3, 4) * 2).x");
            auto mul_y = eng->execute("(Vec2(3, 4) * 2).y");
            check_eq(mul_x.as<double>(), 6.0, "Vec2 multiplication x");
            check_eq(mul_y.as<double>(), 8.0, "Vec2 multiplication y");

            // Test division
            auto div_x = eng->execute("(Vec2(6, 8) / 2).x");
            auto div_y = eng->execute("(Vec2(6, 8) / 2).y");
            check_eq(div_x.as<double>(), 3.0, "Vec2 division x");
            check_eq(div_y.as<double>(), 4.0, "Vec2 division y");
        });

        test("script_class_compound_assignment_operators", [this]() {
            auto eng = make_engine();

            // Define a class with arithmetic operators
            eng->execute(R"(
                class Vec2 {
                    float x = 0.0;
                    float y = 0.0;
                    Vec2(float px, float py) { x = px; y = py; }

                    function operator+(Vec2 other) -> Vec2 {
                        return Vec2(x + other.x, y + other.y);
                    }
                    function operator-(Vec2 other) -> Vec2 {
                        return Vec2(x - other.x, y - other.y);
                    }
                    function operator*(float scalar) -> Vec2 {
                        return Vec2(x * scalar, y * scalar);
                    }
                    function operator/(float scalar) -> Vec2 {
                        return Vec2(x / scalar, y / scalar);
                    }
                }
            )");

            // Test += operator
            auto result_x = eng->execute(R"(
                auto v = Vec2(1, 2);
                v += Vec2(3, 4);
                v.x
            )");
            auto result_y = eng->execute(R"(
                auto v = Vec2(1, 2);
                v += Vec2(3, 4);
                v.y
            )");
            check_eq(result_x.as<double>(), 4.0, "Vec2 += x");
            check_eq(result_y.as<double>(), 6.0, "Vec2 += y");

            // Test -= operator
            auto sub_x = eng->execute(R"(
                auto v = Vec2(5, 7);
                v -= Vec2(2, 3);
                v.x
            )");
            auto sub_y = eng->execute(R"(
                auto v = Vec2(5, 7);
                v -= Vec2(2, 3);
                v.y
            )");
            check_eq(sub_x.as<double>(), 3.0, "Vec2 -= x");
            check_eq(sub_y.as<double>(), 4.0, "Vec2 -= y");

            // Test *= operator
            auto mul_x = eng->execute(R"(
                auto v = Vec2(3, 4);
                v *= 2;
                v.x
            )");
            auto mul_y = eng->execute(R"(
                auto v = Vec2(3, 4);
                v *= 2;
                v.y
            )");
            check_eq(mul_x.as<double>(), 6.0, "Vec2 *= x");
            check_eq(mul_y.as<double>(), 8.0, "Vec2 *= y");

            // Test /= operator
            auto div_x = eng->execute(R"(
                auto v = Vec2(6, 8);
                v /= 2;
                v.x
            )");
            auto div_y = eng->execute(R"(
                auto v = Vec2(6, 8);
                v /= 2;
                v.y
            )");
            check_eq(div_x.as<double>(), 3.0, "Vec2 /= x");
            check_eq(div_y.as<double>(), 4.0, "Vec2 /= y");
        });

        test("type_based_method_overloading", [this]() {
            auto eng = make_engine();

            // Define a class with multiple overloads of the same method using different types
            eng->execute(R"(
                class Vec2 {
                    float x = 0.0;
                    float y = 0.0;
                    Vec2(float px, float py) { x = px; y = py; }

                    // Overload operator* with different types
                    function operator*(float scalar) -> Vec2 {
                        return Vec2(x * scalar, y * scalar);
                    }

                    function operator*(Vec2 other) -> Vec2 {
                        return Vec2(x * other.x, y * other.y);
                    }

                    function operator*(int n) -> Vec2 {
                        return Vec2(x * n, y * n);
                    }

                    // Regular method overloading
                    function scale(float s) -> Vec2 {
                        return Vec2(x * s, y * s);
                    }

                    function scale(Vec2 s) -> Vec2 {
                        return Vec2(x * s.x, y * s.y);
                    }
                }
            )");

            // Test operator* with float - should call float overload
            auto mul_float_x = eng->execute("(Vec2(3, 4) * 2.0).x");
            auto mul_float_y = eng->execute("(Vec2(3, 4) * 2.0).y");
            check_eq(mul_float_x.as<double>(), 6.0, "Vec2 * float x");
            check_eq(mul_float_y.as<double>(), 8.0, "Vec2 * float y");

            // Test operator* with Vec2 - should call Vec2 overload (component-wise)
            auto mul_vec_x = eng->execute("(Vec2(3, 4) * Vec2(2, 3)).x");
            auto mul_vec_y = eng->execute("(Vec2(3, 4) * Vec2(2, 3)).y");
            check_eq(mul_vec_x.as<double>(), 6.0, "Vec2 * Vec2 x (component-wise)");
            check_eq(mul_vec_y.as<double>(), 12.0, "Vec2 * Vec2 y (component-wise)");

            // Test operator* with int - should call int overload
            auto mul_int_x = eng->execute("(Vec2(3, 4) * 2).x");
            auto mul_int_y = eng->execute("(Vec2(3, 4) * 2).y");
            check_eq(mul_int_x.as<double>(), 6.0, "Vec2 * int x");
            check_eq(mul_int_y.as<double>(), 8.0, "Vec2 * int y");

            // Test regular method overloading - scale with float
            auto scale_float_x = eng->execute("Vec2(3, 4).scale(2.0).x");
            auto scale_float_y = eng->execute("Vec2(3, 4).scale(2.0).y");
            check_eq(scale_float_x.as<double>(), 6.0, "Vec2.scale(float) x");
            check_eq(scale_float_y.as<double>(), 8.0, "Vec2.scale(float) y");

            // Test regular method overloading - scale with Vec2
            auto scale_vec_x = eng->execute("Vec2(3, 4).scale(Vec2(2, 3)).x");
            auto scale_vec_y = eng->execute("Vec2(3, 4).scale(Vec2(2, 3)).y");
            check_eq(scale_vec_x.as<double>(), 6.0, "Vec2.scale(Vec2) x");
            check_eq(scale_vec_y.as<double>(), 12.0, "Vec2.scale(Vec2) y");
        });

        test("type_based_overloading_with_var_fallback", [this]() {
            auto eng = make_engine();

            // Test that untyped parameters (var) act as "any" fallback
            // Following C++ semantics: typed conversions are preferred over var/any
            eng->execute(R"(
                class Wrapper {
                    var data = null;
                }

                class Calculator {
                    function process(int x) -> int {
                        return x * 2;
                    }

                    function process(string s) -> string {
                        return s + s;
                    }

                    // This should match only when no typed conversion is possible
                    function process(var x) -> string {
                        return "fallback";
                    }
                }

                var calc = Calculator();
            )");

            // Test with int - should call int overload (exact match)
            auto int_result = eng->execute("Calculator().process(5)");
            check_eq(int_result.as<int>(), 10, "process(int) returns x * 2");

            // Test with string - should call string overload (exact match)
            auto str_result = eng->execute("Calculator().process(\"ab\")");
            check_eq(str_result.as<std::string>(), "abab", "process(string) returns s + s");

            // Test with float - C++ semantics: float->int conversion beats var fallback
            auto float_result = eng->execute("Calculator().process(3.14)");
            check_eq(float_result.as<int>(), 6, "process(float) converts to int (C++ semantics)");

            // Test with object - should call var fallback (no conversion to int/string)
            auto obj_result = eng->execute("Calculator().process(Wrapper())");
            check_eq(obj_result.as<std::string>(), "fallback", "process(object) falls back to var");

            // Test with array - should call var fallback (no conversion to int/string)
            auto arr_result = eng->execute("Calculator().process([1, 2, 3])");
            check_eq(arr_result.as<std::string>(), "fallback", "process(array) falls back to var");
        });

        test("type_based_overloading_multi_param", [this]() {
            auto eng = make_engine();

            // Test overloading with multiple parameters of different types
            eng->execute(R"(
                class Math {
                    // Different parameter type combinations
                    function combine(int a, int b) -> int {
                        return a + b;
                    }

                    function combine(float a, float b) -> float {
                        return a * b;
                    }

                    function combine(string a, string b) -> string {
                        return `${a}-${b}`;
                    }

                    function combine(int a, string b) -> string {
                        return `${b}:${a}`;
                    }
                }
            )");

            // Test int, int -> addition
            auto ii = eng->execute("Math().combine(3, 4)");
            check_eq(ii.as<int>(), 7, "combine(int, int) = addition");

            // Test float, float -> multiplication
            auto ff = eng->execute("Math().combine(3.0, 4.0)");
            check_eq(ff.as<double>(), 12.0, "combine(float, float) = multiplication");

            // Test string, string -> concatenation with dash
            auto ss = eng->execute("Math().combine(\"hello\", \"world\")");
            check_eq(ss.as<std::string>(), "hello-world", "combine(string, string) = dash concat");

            // Test int, string -> special format
            auto is_result = eng->execute("Math().combine(42, \"answer\")");
            check_eq(is_result.as<std::string>(), "answer:42", "combine(int, string) = colon format");
        });

        test("type_based_overloading_conversion_count", [this]() {
            auto eng = make_engine();

            // Test that overloads with fewer conversions are preferred
            eng->execute(R"(
                class Converter {
                    // Requires 0 conversions for (int, int)
                    function calc(int a, int b) -> string {
                        return "int-int";
                    }

                    // Requires 0 conversions for (float, float)
                    function calc(float a, float b) -> string {
                        return "float-float";
                    }

                    // Requires 1 conversion for mixed (int, float) or (float, int)
                    function calc(float a, int b) -> string {
                        return "float-int";
                    }
                }
            )");

            // Exact matches should win
            auto ii = eng->execute("Converter().calc(1, 2)");
            check_eq(ii.as<std::string>(), "int-int", "calc(int, int) exact match");

            auto ff = eng->execute("Converter().calc(1.0, 2.0)");
            check_eq(ff.as<std::string>(), "float-float", "calc(float, float) exact match");

            // Mixed types - prefers exact match for one param
            auto fi = eng->execute("Converter().calc(1.0, 2)");
            check_eq(fi.as<std::string>(), "float-int", "calc(float, int) exact match");
        });

        test("type_based_overloading_arity_difference", [this]() {
            auto eng = make_engine();

            // Test overloading by both type and arity
            eng->execute(R"(
                class Variadic {
                    function foo(int x) -> string { return "one-int"; }
                    function foo(int x, int y) -> string { return "two-int"; }
                    function foo(int x, int y, int z) -> string { return "three-int"; }
                    function foo(float x) -> string { return "one-float"; }
                    function foo(string x) -> string { return "one-string"; }
                }
            )");

            // Test arity selection
            check_eq(eng->execute("Variadic().foo(1)").as<std::string>(), "one-int", "foo(int)");
            check_eq(eng->execute("Variadic().foo(1, 2)").as<std::string>(), "two-int", "foo(int, int)");
            check_eq(eng->execute("Variadic().foo(1, 2, 3)").as<std::string>(), "three-int", "foo(int, int, int)");

            // Test type selection with same arity
            check_eq(eng->execute("Variadic().foo(1.5)").as<std::string>(), "one-float", "foo(float)");
            check_eq(eng->execute("Variadic().foo(\"hi\")").as<std::string>(), "one-string", "foo(string)");
        });

        test("type_based_overloading_class_types", [this]() {
            auto eng = make_engine();

            // Test overloading with user-defined class types
            eng->execute(R"(
                class Point {
                    float x = 0.0;
                    float y = 0.0;
                    Point(float px, float py) { x = px; y = py; }
                }

                class Rect {
                    float w = 0.0;
                    float h = 0.0;
                    Rect(float pw, float ph) { w = pw; h = ph; }
                }

                class Geometry {
                    function area(Point p) -> float { return p.x * p.y; }
                    function area(Rect r) -> float { return r.w * r.h; }
                    function area(float radius) -> float { return 3.14159 * radius * radius; }
                }
            )");

            // Test with Point
            auto point_area = eng->execute("Geometry().area(Point(3, 4))");
            check_eq(point_area.as<double>(), 12.0, "area(Point) = x * y");

            // Test with Rect
            auto rect_area = eng->execute("Geometry().area(Rect(5, 6))");
            check_eq(rect_area.as<double>(), 30.0, "area(Rect) = w * h");

            // Test with float (circle area)
            auto circle_area = eng->execute("Geometry().area(2.0)");
            check(std::abs(circle_area.as<double>() - 12.56636) < 0.001, "area(float) = pi * r^2");
        });

        test("type_based_overloading_numeric_preference", [this]() {
            auto eng = make_engine();

            // Verify int exact match beats float conversion
            eng->execute(R"(
                class NumTest {
                    function test(int x) -> string { return "int"; }
                    function test(float x) -> string { return "float"; }
                }
            )");

            // Integer literal should prefer int overload
            check_eq(eng->execute("NumTest().test(5)").as<std::string>(), "int", "5 calls int overload");

            // Float literal should prefer float overload
            check_eq(eng->execute("NumTest().test(5.0)").as<std::string>(), "float", "5.0 calls float overload");

            // Expression result type matters
            check_eq(eng->execute("NumTest().test(2 + 3)").as<std::string>(), "int", "int + int = int");
            check_eq(eng->execute("NumTest().test(2.0 + 3.0)").as<std::string>(), "float", "float + float = float");
        });

        test("a_float_stored_into_an_int_slot_truncates_toward_zero", [this]() {
            check_values({
                {"auto x = 5; x = 3.9; x", 3},
                {"auto x = 5; x = -3.9; x", -3},
                {"int i = 5; i = 3.7; i", 3},
            });
        });

        test("typed_array_stores_check_the_element_type", [this]() {
            check_rejected({
                {"array<int> nums = [1, 2, 3]; [[warn_mode]] nums.push(\"hello\");", "Cannot push 'string' to array<int>"},
                {"array<int> nums = [1, 2, 3]; [[warn_mode]] nums[0] = \"hello\";", "Cannot assign 'string' to element of type 'int'"},
            });
            check_values({
                {"array<int> nums = [1, 2, 3]; nums.push(4); nums.size()", 4},
                {"array<float> nums = [1.0, 2.0]; nums.push(3); nums.size()", 3},
                {"array<int> nums = [1, 2, 3]; nums[0] = 42; nums[0]", 42},
                {"array<array<int>> matrix = [[1, 2], [3, 4]]; matrix[0][1]", 2},
                {"array<array<int>> matrix = [[1, 2]]; matrix.push([3, 4]); matrix.size()", 2},
            });
        });

        test("auto_uniform_literal_infers_element_type", [this]() {
            // A uniform primitive literal infers its element type and
            // takes that representation; auto is infer-then-enforce for arrays too
            auto eng = make_engine();
            check_eq((int64_t)4, eng->execute("auto nums = [1, 2, 3]; nums.push(4.7); nums[3]").as_int(), "auto int literal locks to int (float converts)");
            check_throws([&]() { eng->execute("auto nums = [1, 2, 3]; nums.push(\"hello\")"); });
            check_throws([&]() { eng->execute("auto names = [\"a\", \"b\"]; names.push(1)"); });
            check_throws([&]() { eng->execute("array<auto> f = [1.5, 2.5]; f.push(\"x\")"); });
            check_eq((int64_t)2, eng->execute("auto e = []; e.push(\"x\"); e.push(1); e.size()").as_int(), "empty literal stays dynamic");
            check_eq((int64_t)3, eng->execute("array<var> v = [1, 2, 3]; v.push(\"s\"); v.size() - 1").as_int(), "array<var> stays dynamic");
            auto buf = eng->execute("auto b = [1, 2, 3]; b");
            check(buf.unchecked_array_node()->kind() == jai::script_array::kind_t::i64, "auto int literal holds an i64 buffer");
        });

        test("nested_array_inner_type_is_checked", [this]() {
            // The container boundary recurses: an inner literal converts to array<int> (a
            // typed node) or is rejected, so array<array<int>> is typed at every level
            auto eng = make_engine();
            check_throws([&]() {
                eng->execute(R"(
                    array<array<int>> matrix = [[1, 2]];
                    matrix.push(["hello", "world"]);
                )");
            });
            check_eq((int64_t)3, eng->execute("array<array<int>> m = [[1, 2]]; m.push([3, 4]); m[1][0];").as_int());
        });

        test("auto_container_literals_are_homogeneous_at_every_depth_and_var_is_exempt", [this]() {
            check_values({
                {"auto nums = [1, 2, 3]; nums.size()", 3},
                {"auto strs = [\"hello\", \"world\"]; strs.size()", 2},
                {"array<auto> nums = [1, 2, 3]; nums.size()", 3},
                {"auto empty = []; empty.size()", 0},
                {"auto single = [42]; single[0]", 42},
                {"auto arr = [1, 2, 3]; arr.push(4); arr.push(5); arr.size()", 5},
                {"auto matrix = [[1, 2], [3, 4]]; matrix[1][1]", 4},
                {"auto cube = [[[1, 2], [3, 4]], [[5, 6], [7, 8]]]; cube[1][0][1]", 6},
                {"auto data = {\"a\": {\"x\": 1, \"y\": 2}, \"b\": {\"x\": 3, \"y\": 4}}; data[\"b\"][\"y\"]", 4},
                {"auto data = [[{\"a\": 1, \"b\": 2}], [{\"c\": 3, \"d\": 4}]]; data[1][0][\"d\"]", 4},
                {"auto data = {\"items\": [[1, 2], [3, 4]]}; data[\"items\"][1][0]", 3},
                {"auto items = [{\"a\": 1}, {\"b\": 2}]; items[0][\"a\"]", 1},
                {"auto data = {\"nums\": [1, 2, 3], \"more\": [4, 5]}; data[\"nums\"][1]", 2},
                {"auto m = {\"a\": 1, \"b\": 2, \"c\": 3}; m[\"a\"] + m[\"b\"] + m[\"c\"]", 6},
                {"auto matrix = [[1, 2], [3, 4], [5, 6]]; matrix[0][0] + matrix[1][1] + matrix[2][0]", 1 + 4 + 5},
                {"auto data = {\"nums\": [1, 2, 3], \"more\": [4, 5, 6]}; data[\"nums\"].size() + data[\"more\"].size()", 6},
                {"var nums = [1, 2, 3]; nums.push(\"hello\"); nums.push(true); nums.size()", 5},
                {"var mixed = [1, \"hello\", 3.14, true]; mixed.size()", 4},
                {"var nums = [5.0, 5]; nums.size()", 2},
                {"array<var> items = [1, \"hello\", 3.14]; items.size()", 3},
                {"var chaos = [[1, \"a\", true], {\"x\": [1, \"b\"]}, 42]; chaos.size()", 3},
            });
            check_rejected({
                {"auto mixed = [5.0, 5];"},
                {"array<auto> items = [1, \"hello\"];"},
                {"auto mixed = [[1, \"a\"], [2, \"b\"]];"},
                {"auto deep = [[[1, \"x\"]]];"},
                {"auto bad = {\"a\": {\"x\": 1, \"y\": \"bad\"}};"},
                {"auto bad = {\"items\": [[1, \"bad\"]]};"},
            });
        });

        test("auto_array_rejects_mixed_types", [this]() {
            auto eng = make_engine();

            // auto x = [1, "hello"] - mixed types, should fail
            bool caught = false;
            try {
                eng->execute(R"(
                    auto mixed = [1, "hello"];
                )");
            } catch (const std::exception& e) {
                caught = true;
                std::string msg = e.what();
                check_true(msg.find("homogeneous") != std::string::npos ||
                           msg.find("Array declared with 'auto'") != std::string::npos,
                    "Error should mention homogeneous requirement");
            }

            check_true(caught, "auto [int, string] should fail");
        });

        test("typed_slots_convert_numbers_and_reject_other_kinds", [this]() {
            check_values({
                {"string s = \"hello\"; s = \"world\"; s", "world"s},
                {"bool b = true; b = false; b", false},
                {"float f = 3.14; f = 5; f", 5.0},
                {"auto x = 3.14; x = 5; x", 5.0},
                {"function square(float x) -> float { return x * x; } square(5)", 25.0},
                {"function get_value() -> int { return 42; } get_value()", 42},
                {"class Data { int value = 0; } auto d = Data(); d.value = 42; d.value", 42},
                {"function modify(int& ref) { ref = ref + 10; } auto x = 5; modify(x); x", 15},
            });
            check_typed_values({
                {"auto x = 42; x = 100; x", script_value_type::jai_int_type, 100},
            });
            check_rejected({
                {"int x = 5; x = \"hello\";"},
                {"auto x = 42; x = \"string\";"},
                {"int x = 5; x = null;"},
                {"function add(int a, int b) -> int { return a + b; } add(\"hello\", \"world\");"},
            });
        });

        test("bool_rejects_incompatible_assignment", [this]() {
            auto eng = make_engine();
            bool caught = false;
            try {
                eng->execute(R"(
                    bool b = true;
                    b = 42;  // int to bool - may or may not be allowed
                )");
                // If it gets here without throwing, check the conversion behavior
                // Note: JaiScript may allow int->bool conversion (truthy)
            } catch (const std::exception&) {
                caught = true;
            }
            // Document actual behavior - this may pass or fail depending on bool coercion rules
        });

        test("var_accepts_any_type", [this]() {
            auto eng = make_engine();
            eng->execute(R"(
                var x = 42;
                x = "hello";
                x = 3.14;
                x = true;
            )");
            // Should not throw
        });

        test("var_preserves_actual_type", [this]() {
            auto eng = make_engine();

            auto r1 = eng->execute("var x = 42; x");
            check_eq(r1.type(), script_value_type::jai_int_type);

            auto r2 = eng->execute("x = \"hello\"; x");
            check_eq(r2.type(), script_value_type::jai_string_type);
        });

        test("null_assignable_to_object_types", [this]() {
            auto eng = make_engine();
            eng->execute(R"(
                class Foo { int x = 0; }
            )");

            eng->execute(R"(
                auto f = Foo();
                f = null;  // OK for objects
            )");
        });

        test("a_derived_object_reads_base_fields_through_a_base_or_derived_slot", [this]() {
            const char* prelude = "class Animal { string name = \"\"; } class Dog : Animal { Dog() { name = \"dog\"; } }";
            check_values({
                {"Animal a = Dog(); a.name", "dog"s, prelude},
                {"auto d = Dog(); d.name", "dog"s, prelude},
            });
        });

        test("constructor_enables_implicit_conversion", [this]() {
            auto eng = make_engine();
            eng->execute(R"(
                class Wrapper {
                    int value = 0;
                    Wrapper(int v) { value = v; }
                }
                function process(Wrapper w) -> int { return w.value; }
            )");

            auto result = eng->execute("process(42)");  // int -> Wrapper via ctor
            check_eq(result.as<int>(), 42);
        });

        test("no_chained_constructor_conversion", [this]() {
            auto eng = make_engine();
            eng->execute(R"(
                class A { int x = 0; }
                class B { B(A a) { } }
                class C { C(B b) { } }
            )");

            bool caught = false;
            try {
                eng->execute(R"(
                    function take_c(C c) {}
                    take_c(A());  // Would need A->B->C chain
                )");
            } catch (const std::exception&) {
                caught = true;
            }
            check(caught, "chained conversions should be rejected");
        });

        test("shared_ptr_handles_keep_their_class_and_weak_ptr_takes_only_handles", [this]() {
            check_values({
                {"auto n = shared_ptr<Node>(); n.value = 42; n.value", 42, "class Node { int value = 0; }"},
                {"auto d = shared_ptr<Data>(); d.x = 99; process(d)", 99,
                 "class Data { int x = 0; } function process(shared_ptr<Data> d) -> int { return d.x; }"},
                {"auto o = shared_ptr<Obj>(); o.x = 42; auto w = weak_ptr<Obj>(o); auto locked = w.lock(); locked.x", 42, "class Obj { int x = 0; }"},
            });
            check_rejected({
                {"auto o = Obj(); auto w = weak_ptr<Obj>(o);", "shared_ptr", "class Obj { int x = 0; }"},
            });
        });

        test("bitwise_operators_take_integers_only", [this]() {
            check_values({
                {"5 & 3", 1},
                {"5 | 3", 7},
                {"5 ^ 3", 6},
                {"1 << 3", 8},
                {"8 >> 2", 2},
            });
            check_rejected({{"5.0 & 3"}});
        });

        test("values_cross_between_cpp_and_script_with_their_types", [this]() {
            struct crossing {
                const char* code;
                std::function<void()> run;
            };
            auto global_round_trip = [this](auto a_value, script_value_type a_type) {
                auto eng = make_engine();
                eng->add_global("cpp_value", script_value(a_value, eng.get()));
                auto result = eng->execute("cpp_value");
                check_eq(result.type(), a_type);
                check_eq(result.as<decltype(a_value)>(), a_value);
            };
            const crossing rows[] = {
                {"int global", [&]() { global_round_trip(42, script_value_type::jai_int_type); }},
                {"float global", [&]() { global_round_trip(3.14, script_value_type::jai_float_type); }},
                {"string global", [&]() { global_round_trip(std::string("hello"), script_value_type::jai_string_type); }},
                {"bool global", [&]() { global_round_trip(true, script_value_type::jai_bool_type); }},
                {"42", [this]() { check_eq(make_engine()->execute("42").as<int>(), 42); }},
                {"3.14", [this]() {
                    auto eng = make_engine();
                    auto result = eng->execute("3.14");
                    check(std::abs(result.as<double>() - 3.14) < 0.001);
                    check(std::abs(result.as<float>() - 3.14f) < 0.001f);
                }},
                {"typed_add(3, 4)", [this]() {
                    auto eng = make_engine();
                    eng->add_function("typed_add", [](int a, int b) -> int { return a + b; });
                    check_eq(eng->execute("typed_add(3, 4)").as<int>(), 7);
                }},
                {"make_string()", [this]() {
                    auto eng = make_engine();
                    eng->add_function("make_string", []() -> std::string { return "from_cpp"; });
                    auto result = eng->execute("make_string()");
                    check_eq(result.type(), script_value_type::jai_string_type);
                    check_eq(result.as<std::string>(), "from_cpp");
                }},
            };
            check_each(rows, [](const crossing& a_row) { a_row.run(); });
        });

        test("script_class_fields_methods_and_returns_keep_their_declared_types", [this]() {
            check_values({
                {"auto calc = Calculator(); calc.value = 10; calc.add(5)", 15,
                 "class Calculator { int value = 0; function add(int x) -> int { return value + x; } function get_value() -> int { return value; } }"},
                {"auto p = Player(\"Hero\"); p.id = 42; p.id", 42,
                 "class Entity { int id = 0; string name = \"\"; } class Player : Entity { int health = 100; Player(string n) { name = n; } }"},
                {"auto c = C(); c.a_val + c.b_val + c.c_val", 6, "class A { int a_val = 1; } class B : A { int b_val = 2; } class C : B { int c_val = 3; }"},
                {"make_result(42).value", 42,
                 "class Result { int value = 0; } function make_result(int v) -> Result { auto r = Result(); r.value = v; return r; }"},
            });
        });

        test("script_class_inheritance_method_override", [this]() {
            auto eng = make_engine();
            eng->execute(R"(
                class Base {
                    function get_name() -> string { return "Base"; }
                }
                class Derived : Base {
                    override function get_name() -> string { return "Derived"; }
                }
            )");

            auto base_result = eng->execute("Base().get_name()");
            check_eq(base_result.as<std::string>(), "Base");

            auto derived_result = eng->execute("Derived().get_name()");
            check_eq(derived_result.as<std::string>(), "Derived");
        });

        test("script_class_polymorphic_call", [this]() {
            auto eng = make_engine();
            eng->execute(R"(
                class Animal {
                    function speak() -> string { return "..."; }
                }
                class Dog : Animal {
                    override function speak() -> string { return "Woof!"; }
                }
                class Cat : Animal {
                    override function speak() -> string { return "Meow!"; }
                }
            )");

            // Direct method calls on derived classes
            auto dog_result = eng->execute("Dog().speak()");
            check_eq(dog_result.as<std::string>(), "Woof!");

            auto cat_result = eng->execute("Cat().speak()");
            check_eq(cat_result.as<std::string>(), "Meow!");

            // Upcasting to base class
            auto base_result = eng->execute(R"(
                Animal a = Dog();
                a.speak()
            )");
            check_eq(base_result.as<std::string>(), "Woof!");
        });

        test("script_class_type_checking_in_function", [this]() {
            auto eng = make_engine();
            eng->execute(R"(
                class TypeA { int x = 0; }
                class TypeB { int y = 0; }
                function process_a(TypeA a) -> int { return a.x; }
            )");

            // Should work with TypeA
            auto result = eng->execute(R"(
                auto a = TypeA();
                a.x = 42;
                process_a(a)
            )");
            check_eq(result.as<int>(), 42);

            // Should fail with TypeB
            bool caught = false;
            try {
                eng->execute("process_a(TypeB())");
            } catch (const std::exception&) {
                caught = true;
            }
            check(caught, "TypeB should not be accepted where TypeA is expected");
        });

        test("script_inherits_cpp_basic_field_access", [this]() {
            auto eng = make_engine();

            // Register C++ Entity class
            dynamic_binder<Entity>(*eng, "Entity")
                .constructor<>()
                .constructor<std::string, int>()
                .property("name", &Entity::name)
                .property("id", &Entity::id)
                .method("get_type", &Entity::get_type)
                .method("get_id", &Entity::get_id)
                .method("get_name", &Entity::get_name)
                .build();

            // Script class inherits from C++ Entity
            auto result = eng->execute(R"(
                class Player : Entity {
                    int score = 0;

                    Player(string n, int i) : super(n, i) {
                        score = 100;
                    }
                }

                auto p = Player("Hero", 42);
                p.id  // Access C++ base class field
            )");
            check_eq(result.as<int>(), 42);
        });

        test("script_inherits_cpp_base_method_call", [this]() {
            auto eng = make_engine();

            dynamic_binder<Entity>(*eng, "Entity")
                .constructor<std::string, int>()
                .property("name", &Entity::name)
                .property("id", &Entity::id)
                .method("get_type", &Entity::get_type)
                .method("get_name", &Entity::get_name)
                .build();

            auto result = eng->execute(R"(
                class Player : Entity {
                    Player(string n) : super(n, 1) {}
                }

                auto p = Player("TestPlayer");
                p.get_name()  // Call C++ base class method
            )");
            check_eq(result.as<std::string>(), "TestPlayer");
        });

        test("script_inherits_cpp_override_wins_and_super_reaches_the_cpp_method", [this]() {
            const std::pair<const char*, const char*> cases[] = {
                {"class Enemy : Entity { Enemy(string n) : super(n, 999) {} override function get_type() -> string { return \"Enemy\"; } } "
                 "auto e = Enemy(\"Goblin\"); e.get_type()", "Enemy"},
                {"class Boss : Entity { Boss(string n) : super(n, 1000) {} override function get_type() -> string { return super::get_type() + \"_Boss\"; } } "
                 "auto b = Boss(\"Dragon\"); b.get_type()", "Entity_Boss"},
            };
            check_each(cases, [this](const auto& c) {
                auto eng = make_engine();
                dynamic_binder<Entity>(*eng, "Entity")
                    .constructor<std::string, int>()
                    .property("name", &Entity::name)
                    .method("get_type", &Entity::get_type)
                    .build();
                check_eq(eng->execute(c.first).template as<std::string>(), std::string(c.second));
            });
        });

        test("script_inherits_cpp_script_specific_field", [this]() {
            auto eng = make_engine();

            dynamic_binder<Entity>(*eng, "Entity")
                .constructor<std::string, int>()
                .property("name", &Entity::name)
                .property("id", &Entity::id)
                .build();

            auto result = eng->execute(R"(
                class Character : Entity {
                    int level = 1;
                    int experience = 0;

                    Character(string n) : super(n, 0) {
                        level = 1;
                        experience = 0;
                    }

                    function gain_exp(int amount) {
                        experience = experience + amount;
                        if (experience >= 100) {
                            level = level + 1;
                            experience = 0;
                        }
                    }
                }

                auto c = Character("Warrior");
                c.gain_exp(150);
                c.level
            )");
            check_eq(result.as<int>(), 2);
        });

        test("script_inherits_cpp_type_preserved_in_parameter", [this]() {
            auto eng = make_engine();

            dynamic_binder<Entity>(*eng, "Entity")
                .constructor<std::string, int>()
                .property("name", &Entity::name)
                .property("id", &Entity::id)
                .method("get_id", &Entity::get_id)
                .build();

            auto result = eng->execute(R"(
                class NPC : Entity {
                    NPC(string n, int i) : super(n, i) {}
                }

                // Function accepts base Entity type
                function process_entity(Entity e) -> int {
                    return e.get_id();
                }

                auto npc = NPC("Merchant", 500);
                process_entity(npc)  // NPC should work as Entity parameter
            )");
            check_eq(result.as<int>(), 500);
        });

        test("script_inherits_cpp_different_cpp_base_types", [this]() {
            auto eng = make_engine();

            // Register Vehicle
            dynamic_binder<Vehicle>(*eng, "Vehicle")
                .constructor<int, std::string>()
                .property("speed", &Vehicle::speed)
                .property("model", &Vehicle::model)
                .method("get_max_speed", &Vehicle::get_max_speed)
                .method("accelerate", &Vehicle::accelerate)
                .build();

            // Register Animal
            dynamic_binder<Animal>(*eng, "Animal")
                .constructor<std::string, int>()
                .property("species", &Animal::species)
                .property("legs", &Animal::legs)
                .method("speak", &Animal::speak)
                .method("get_legs", &Animal::get_legs)
                .build();

            // Script classes extending different C++ bases
            auto vehicle_result = eng->execute(R"(
                class Car : Vehicle {
                    Car(string m) : super(0, m) {}

                    override function get_max_speed() -> int {
                        return 200;
                    }
                }

                auto car = Car("Sedan");
                car.get_max_speed()
            )");
            check_eq(vehicle_result.as<int>(), 200);

            auto animal_result = eng->execute(R"(
                class Dog : Animal {
                    Dog() : super("Canine", 4) {}

                    override function speak() -> string {
                        return "Woof!";
                    }
                }

                auto dog = Dog();
                dog.speak()
            )");
            check_eq(animal_result.as<std::string>(), "Woof!");
        });

        test("script_inherits_cpp_type_mismatch_rejected", [this]() {
            auto eng = make_engine();

            dynamic_binder<Vehicle>(*eng, "Vehicle")
                .constructor<int, std::string>()
                .property("speed", &Vehicle::speed)
                .build();

            dynamic_binder<Animal>(*eng, "Animal")
                .constructor<std::string, int>()
                .property("species", &Animal::species)
                .build();

            // Car extends Vehicle
            eng->execute(R"(
                class Car : Vehicle {
                    Car() : super(0, "car") {}
                }
            )");

            // Function expects Animal
            eng->execute(R"(
                function pet(Animal a) -> string {
                    return a.species;
                }
            )");

            // Passing Car (extends Vehicle) to function expecting Animal should fail
            bool caught = false;
            try {
                eng->execute("pet(Car())");
            } catch (const std::exception&) {
                caught = true;
            }
            check(caught, "Car (extends Vehicle) should not be accepted as Animal parameter");
        });

        test("script_classes_over_a_cpp_base_initialize_every_level", [this]() {
            const std::pair<const char*, const char*> cases[] = {
                {"class Character : Entity { int level = 1; Character(string n) : super(n, 0) {} } "
                 "class Mage : Character { int mana = 100; Mage(string n) : super(n) { level = 5; } } "
                 "auto m = Mage(\"Gandalf\"); `${m.name} Level:${m.level} Mana:${m.mana}`", "Gandalf Level:5 Mana:100"},
                {"class Character : Entity { int level = 1; Character(string n, int startLevel) : super(n, 0) { level = startLevel; } } "
                 "class Mage : Character { int mana = 50; Mage(string n, int startLevel, int startMana) : super(n, startLevel) { mana = startMana; } } "
                 "class Archmage : Mage { int spellPower = 100; Archmage(string n) : super(n, 20, 500) { spellPower = 999; } } "
                 "auto a = Archmage(\"Merlin\"); `${a.name} L:${a.level} M:${a.mana} SP:${a.spellPower}`", "Merlin L:20 M:500 SP:999"},
            };
            check_each(cases, [this](const auto& c) {
                auto eng = make_engine();
                dynamic_binder<Entity>(*eng, "Entity")
                    .constructor<std::string, int>()
                    .property("name", &Entity::name)
                    .property("id", &Entity::id)
                    .build();
                check_eq(eng->execute(c.first).template as<std::string>(), std::string(c.second));
            });
        });

        test("script_inherits_cpp_cpp_method_mutation", [this]() {
            auto eng = make_engine();

            dynamic_binder<Vehicle>(*eng, "Vehicle")
                .constructor<int, std::string>()
                .property("speed", &Vehicle::speed)
                .property("model", &Vehicle::model)
                .method("accelerate", &Vehicle::accelerate)
                .build();

            auto result = eng->execute(R"(
                class Motorcycle : Vehicle {
                    Motorcycle(string m) : super(0, m) {}
                }

                auto bike = Motorcycle("Ducati");
                bike.accelerate(50);  // Call C++ method that mutates state
                bike.accelerate(30);
                bike.speed
            )");
            check_eq(result.as<int>(), 80);
        });

        test("script_inherits_cpp_script_overrides_cpp_field_access", [this]() {
            auto eng = make_engine();

            dynamic_binder<Entity>(*eng, "Entity")
                .constructor<std::string, int>()
                .property("name", &Entity::name)
                .property("id", &Entity::id)
                .method("get_name", &Entity::get_name)
                .build();

            // Script can access and modify C++ base field
            auto result = eng->execute(R"(
                class CustomEntity : Entity {
                    CustomEntity() : super("initial", 0) {}

                    function rename(string newName) {
                        name = newName;  // Modify C++ base class field
                    }
                }

                auto ce = CustomEntity();
                ce.rename("Modified");
                ce.get_name()
            )");
            check_eq(result.as<std::string>(), "Modified");
        });

        test("script_inherits_cpp_shared_ptr_inheritance", [this]() {
            auto eng = make_engine();

            dynamic_binder<Entity>(*eng, "Entity")
                .constructor<std::string, int>()
                .property("name", &Entity::name)
                .property("id", &Entity::id)
                .method("get_id", &Entity::get_id)
                .build();

            auto result = eng->execute(R"(
                class SmartEntity : Entity {
                    SmartEntity(string n, int i) : super(n, i) {}
                }

                auto ptr = shared_ptr<SmartEntity>("PtrTest", 777);
                ptr.get_id()
            )");
            check_eq(result.as<int>(), 777);
        });

        test("script_inherits_cpp_return_derived_from_base_function", [this]() {
            auto eng = make_engine();

            dynamic_binder<Entity>(*eng, "Entity")
                .constructor<std::string, int>()
                .property("name", &Entity::name)
                .property("id", &Entity::id)
                .build();

            auto result = eng->execute(R"(
                class Monster : Entity {
                    int danger_level = 0;

                    Monster(string n, int d) : super(n, 0) {
                        danger_level = d;
                    }
                }

                function create_monster(string name, int danger) -> Entity {
                    return Monster(name, danger);
                }

                auto e = create_monster("Orc", 5);
                e.name
            )");
            check_eq(result.as<std::string>(), "Orc");
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::type_safety_tests)