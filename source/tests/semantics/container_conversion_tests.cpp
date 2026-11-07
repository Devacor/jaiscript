#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/timing.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/bound_array.hpp>
#include <jaiscript/core/bound_map.hpp>
#include <jaiscript/core/jaibite.hpp>
#include "value_rows.hpp"
#include <algorithm>
#include <vector>
#include <map>
#include <numeric>
#include <cmath>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests::container_conversion_fixtures {

struct Point {
    double x = 0.0;
    double y = 0.0;
    Point() = default;
    Point(double a_x, double a_y) : x(a_x), y(a_y) {}
    double distance_from_origin() const { return std::sqrt(x * x + y * y); }
};

struct Person {
    std::string name;
    int age = 0;
    Person() = default;
    Person(const std::string& a_name, int a_age) : name(a_name), age(a_age) {}
};

struct Widget {
    std::string name {"default"};
    int value = 0;
    Widget() = default;
    Widget(const std::string& a_name, int a_value) : name(a_name), value(a_value) {}
};

struct NonDefaultWidget {
    std::string name;
    int value;
    NonDefaultWidget(const std::string& a_name, int a_value) : name(a_name), value(a_value) {}
};

struct Calculator {
    double accumulator = 0.0;
    Calculator() = default;
    Calculator(double a_initial) : accumulator(a_initial) {}
    void add(double a_value) { accumulator += a_value; }
    void multiply(double a_value) { accumulator *= a_value; }
    double result() const { return accumulator; }
};

struct Animal {
    std::string name;
    virtual ~Animal() = default;
    virtual std::string speak() const = 0;
};

struct Dog : Animal {
    Dog() { name = "Dog"; }
    Dog(const std::string& a_name) { name = a_name; }
    std::string speak() const override { return "Woof!"; }
};

template<typename Container>
int sum_of(const Container& a_values) {
    int result = 0;
    for (int value : a_values) { result += value; }
    return result;
}

template<typename Map>
auto sum_of_values(const Map& a_map) {
    std::decay_t<decltype(a_map.begin()->second)> result {};
    for (const auto& [key, value] : a_map) { result += value; }
    return result;
}

// std::vector / std::map parameters and returns, and every bound class the rows build containers of; no standard conversions
void bind_std_containers(engine& a_engine) {
    dynamic_binder<Point>(a_engine, "Point")
        .constructor<>()
        .constructor<double, double>()
        .property("x", &Point::x)
        .property("y", &Point::y)
        .build();
    dynamic_binder<Person>(a_engine, "Person")
        .constructor<>()
        .constructor<const std::string&, int>()
        .property("name", &Person::name)
        .property("age", &Person::age)
        .build();
    dynamic_binder<Widget>(a_engine, "Widget")
        .constructor<>()
        .constructor<const std::string&, int>()
        .property("name", &Widget::name)
        .property("value", &Widget::value)
        .build();
    dynamic_binder<NonDefaultWidget>(a_engine, "NonDefaultWidget")
        .constructor<const std::string&, int>()
        .property("name", &NonDefaultWidget::name)
        .property("value", &NonDefaultWidget::value)
        .build();
    dynamic_binder<Calculator>(a_engine, "Calculator")
        .constructor<>()
        .constructor<double>()
        .method("add", &Calculator::add)
        .method("multiply", &Calculator::multiply)
        .method("result", &Calculator::result)
        .property("accumulator", &Calculator::accumulator)
        .build();
    dynamic_binder<Animal>(a_engine, "Animal")
        .property("name", &Animal::name)
        .method("speak", &Animal::speak)
        .build();
    dynamic_binder<Dog>(a_engine, "Dog")
        .constructor<>()
        .constructor<const std::string&>()
        .base_class<Animal>()
        .build();

    a_engine.add_function("sum_integers", [](std::vector<int> a_nums) { return sum_of(a_nums); });
    a_engine.add_function("get_vector_size", [](std::vector<int> a_nums) { return a_nums.size(); });
    a_engine.add_function("double_integers", [](std::vector<int> a_nums) {
        for (int& n : a_nums) { n *= 2; }
        return a_nums;
    });
    a_engine.add_function("range", [](int a_start, int a_end) {
        std::vector<int> result;
        for (int i = a_start; i < a_end; ++i) { result.push_back(i); }
        return result;
    });
    a_engine.add_function("sum_doubles", [](std::vector<double> a_nums) { return std::accumulate(a_nums.begin(), a_nums.end(), 0.0); });
    a_engine.add_function("concatenate_strings", [](std::vector<std::string> a_strs) {
        std::string result;
        for (const auto& s : a_strs) { result += s; }
        return result;
    });
    a_engine.add_function("join_strings", [](std::vector<std::string> a_strs, const std::string& a_sep) {
        std::string result;
        for (size_t i = 0; i < a_strs.size(); ++i) { result += (i > 0 ? a_sep : "") + a_strs[i]; }
        return result;
    });
    a_engine.add_function("sum_matrix", [](std::vector<std::vector<int>> a_matrix) {
        int result = 0;
        for (const auto& row : a_matrix) { result += sum_of(row); }
        return result;
    });
    a_engine.add_function("sum_bound_matrix", [](const bound_array<bound_array<int>>& a_matrix) {
        int result = 0;
        for (const auto& row : a_matrix) { result += sum_of(row); }
        return result;
    });
    a_engine.add_function("create_matrix", [](int a_rows, int a_cols) {
        std::vector<std::vector<int>> result(a_rows, std::vector<int>(a_cols));
        for (int i = 0; i < a_rows; ++i) {
            for (int j = 0; j < a_cols; ++j) { result[i][j] = i * a_cols + j; }
        }
        return result;
    });
    a_engine.add_function("is_empty_vec", [](std::vector<int> a_v) { return a_v.empty(); });
    a_engine.add_function("is_empty_map", [](std::map<std::string, int> a_m) { return a_m.empty(); });
    a_engine.add_function("count_total_items", [](std::vector<std::map<std::string, int>> a_data) {
        int result = 0;
        for (const auto& m : a_data) { result += static_cast<int>(m.size()); }
        return result;
    });
    a_engine.add_function("sum_all_groups", [](std::map<std::string, std::vector<int>> a_groups) {
        int result = 0;
        for (const auto& [name, values] : a_groups) { result += sum_of(values); }
        return result;
    });
    a_engine.add_function("total_distance", [](std::vector<Point> a_points) {
        double result = 0.0;
        for (const auto& p : a_points) { result += p.distance_from_origin(); }
        return result;
    });
    a_engine.add_function("create_points", [](std::vector<double> a_coords) {
        std::vector<Point> result;
        for (size_t i = 0; i + 1 < a_coords.size(); i += 2) { result.emplace_back(a_coords[i], a_coords[i + 1]); }
        return result;
    });

    a_engine.add_function("sum_string_to_int_map", [](std::map<std::string, int> a_map) { return sum_of_values(a_map); });
    a_engine.add_function("sum_double_values", [](std::map<std::string, double> a_map) { return sum_of_values(a_map); });
    a_engine.add_function("concatenate_string_values", [](std::map<std::string, std::string> a_map) { return sum_of_values(a_map); });
    a_engine.add_function("get_map_size", [](std::map<std::string, int> a_map) { return a_map.size(); });
    a_engine.add_function("get_string_keys", [](std::map<std::string, int> a_map) {
        std::vector<std::string> result;
        for (const auto& [key, value] : a_map) { result.push_back(key); }
        return result;
    });
    a_engine.add_function("get_int_values", [](std::map<std::string, int> a_map) {
        std::vector<int> result;
        for (const auto& [key, value] : a_map) { result.push_back(value); }
        return result;
    });
    a_engine.add_function("map_contains_key", [](std::map<std::string, int> a_map, const std::string& a_key) { return a_map.contains(a_key); });
    a_engine.add_function("get_map_value", [](std::map<std::string, int> a_map, const std::string& a_key) {
        auto it = a_map.find(a_key);
        return it != a_map.end() ? it->second : -1;
    });
    a_engine.add_function("number_names", [](std::map<int, std::string> a_nums) {
        std::string result;
        for (const auto& [num, name] : a_nums) { result += (result.empty() ? "" : ", ") + std::to_string(num) + "=" + name; }
        return result;
    });
    a_engine.add_function("create_scores", []() -> std::map<std::string, int> { return {{"Alice", 95}, {"Bob", 87}, {"Charlie", 92}}; });
    a_engine.add_function("create_string_int_map", []() -> std::map<std::string, int> { return {{"first", 1}, {"second", 2}, {"third", 3}}; });
    a_engine.add_function("reverse_map", [](std::map<std::string, int> a_input) {
        std::map<int, std::string> result;
        for (const auto& [key, value] : a_input) { result[value] = key; }
        return result;
    });
    a_engine.add_function("create_nested_map", []() -> std::map<std::string, std::map<std::string, int>> {
        return {{"group1", {{"a", 1}, {"b", 2}}}, {"group2", {{"x", 10}, {"y", 20}}}};
    });
    a_engine.add_function("sum_nested_map", [](std::map<std::string, std::map<std::string, int>> a_nested) {
        int result = 0;
        for (const auto& [group, inner] : a_nested) { result += sum_of_values(inner); }
        return result;
    });
    a_engine.add_function("create_person_map", []() -> std::map<std::string, Person> {
        return {{"john", Person("John Doe", 30)}, {"jane", Person("Jane Smith", 25)}, {"bob", Person("Bob Johnson", 35)}};
    });
    a_engine.add_function("sum_ages", [](std::map<std::string, Person> a_people) {
        int result = 0;
        for (const auto& [key, person] : a_people) { result += person.age; }
        return result;
    });
    a_engine.add_function("get_names", [](std::map<std::string, Person> a_people) {
        std::vector<std::string> result;
        for (const auto& [key, person] : a_people) { result.push_back(person.name); }
        return result;
    });

    a_engine.add_function("process_mixed_vector", [](std::vector<script_value> a_values) {
        return static_cast<int>(std::count_if(a_values.begin(), a_values.end(), [](const script_value& v) { return v.is_int(); }));
    });
    a_engine.add_function("count_map_types", [](script_map a_map) {
        int result = 0;
        for (const auto& [key, value] : a_map) { result += value.is_int() ? 1 : 0; }
        return result;
    });
    engine* host = &a_engine;
    a_engine.add_function("create_mixed_vector", [host]() -> std::vector<script_value> {
        return {host->make_value(42), host->make_value("hello"), host->make_value(3.14), host->make_value(true)};
    });
    a_engine.add_function("create_mixed_map", [host]() {
        script_map result;
        result.insert_or_assign(host->make_value("int"), host->make_value(42));
        result.insert_or_assign(host->make_value("string"), host->make_value("hello"));
        result.insert_or_assign(host->make_value("float"), host->make_value(3.14));
        result.insert_or_assign(host->make_value("bool"), host->make_value(true));
        return result;
    });

    a_engine.add_function("count_widgets", [](std::vector<Widget> a_widgets) { return static_cast<int>(a_widgets.size()); });
    a_engine.add_function("get_widget_value", [](std::map<std::string, Widget> a_widgets, const std::string& a_key) {
        auto it = a_widgets.find(a_key);
        return it != a_widgets.end() ? it->second.value : -1;
    });
    a_engine.add_function("get_widget_by_id", [](std::map<int, Widget> a_widgets, int a_id) {
        auto it = a_widgets.find(a_id);
        return it != a_widgets.end() ? it->second.name : std::string("not found");
    });
    a_engine.add_function("create_widgets", []() -> std::vector<Widget> { return {Widget("a", 1), Widget("b", 2), Widget("c", 3)}; });
    a_engine.add_function("sum_widget_values", [](const bound_array<Widget>& a_widgets) {
        int result = 0;
        for (const auto& w : a_widgets) { result += w.value; }
        return result;
    });
    a_engine.add_function("count_ndwidgets", [](std::vector<NonDefaultWidget> a_widgets) { return static_cast<int>(a_widgets.size()); });
    a_engine.add_function("count_all_widgets", [](std::vector<std::vector<Widget>> a_groups) {
        int result = 0;
        for (const auto& group : a_groups) { result += static_cast<int>(group.size()); }
        return result;
    });
    a_engine.add_function("sum_all_results", [](const bound_array<Calculator>& a_calcs) {
        double result = 0.0;
        for (const auto& calc : a_calcs) { result += calc.result(); }
        return result;
    });
    a_engine.add_function("count_dogs", [](std::vector<Dog> a_dogs) { return static_cast<int>(a_dogs.size()); });
    a_engine.add_function("count_script_values", [](const std::vector<script_value>& a_values) { return static_cast<int>(a_values.size()); });
    if (!detail::engine_access::get_conversion_registry(a_engine)->has_conversion<std::vector<script_value>>()) {
        a_engine.add_vector_conversion<script_value>();
    }
    a_engine.add_function("make_speak", [](std::shared_ptr<Animal> a_animal) { return a_animal ? a_animal->speak() : std::string("null"); });
}

// const bound_array<T>& and const bound_map<K, V>& parameters after add_standard_conversions
void bind_bound_containers(engine& a_engine) {
    detail::engine_access::add_standard_conversions(a_engine);
    dynamic_binder<Point>(a_engine, "Point")
        .constructor<double, double>()
        .property("x", &Point::x)
        .property("y", &Point::y)
        .method("distance_from_origin", &Point::distance_from_origin)
        .build();
    a_engine.add_function("sum_ints", [](const bound_array<int>& a_nums) { return sum_of(a_nums); });
    a_engine.add_function("sum_doubles", [](const bound_array<double>& a_nums) {
        double result = 0.0;
        for (double n : a_nums) { result += n; }
        return result;
    });
    a_engine.add_function("avg_doubles", [](const bound_array<double>& a_nums) {
        double result = 0.0;
        for (double n : a_nums) { result += n; }
        return a_nums.empty() ? 0.0 : result / a_nums.size();
    });
    a_engine.add_function("count_true", [](const bound_array<bool>& a_flags) {
        int result = 0;
        for (bool flag : a_flags) { result += flag ? 1 : 0; }
        return result;
    });
    a_engine.add_function("join_strings", [](const bound_array<std::string>& a_strs) {
        std::string result;
        for (size_t i = 0; i < a_strs.size(); ++i) {
            if (i > 0) { result += " "; }
            result += a_strs[i];
        }
        return result;
    });
    a_engine.add_function("sum_map_values", [](const bound_map<std::string, int>& a_map) {
        int result = 0;
        for (const auto& [key, value] : a_map) { result += value; }
        return result;
    });
    a_engine.add_function("sum_double_values", [](const bound_map<std::string, double>& a_map) {
        double result = 0.0;
        for (const auto& [key, value] : a_map) { result += value; }
        return result;
    });
    a_engine.add_function("point_distance", [](const Point& a_point) { return a_point.distance_from_origin(); });
    a_engine.add_function("total_distance", [](const bound_array<Point>& a_points) {
        double result = 0.0;
        for (const auto& p : a_points) { result += p.distance_from_origin(); }
        return result;
    });
    a_engine.add_function("create_point", [](double a_x, double a_y) { return Point(a_x, a_y); });
}

} // namespace jai::foundry::tests::container_conversion_fixtures

namespace jai::foundry::tests {

class container_conversion_tests : public suite {
public:
    container_conversion_tests() : suite("Container Conversion Tests") {}

    // one timed round: `calls` calls of the precompiled bite
    static std::function<double()> timed_calls(jai::jaibite& call, int calls = 20) {
        return [&call, calls] { return time_seconds([&call, calls] { for (int i = 0; i < calls; ++i) { call.execute(); } }); };
    }

    void forge_tests() override {
        using namespace container_conversion_fixtures;

        test("script_containers_convert_to_std_container_parameters", [this]() {
            const value_row rows[] = {
                {"sum_integers([1, 2, 3, 4, 5])", 15},
                {"sum_integers([])", 0},
                {"sum_integers([42])", 42},
                {"get_vector_size([10, 20, 30, 40])", 4},
                {"var large_array = []; for (int i = 0; i < 1000; ++i) { large_array.push(i); } get_vector_size(large_array);", 1000},
                {"sum_doubles([1, 2, 3])", 6.0},
                {"sum_doubles([1.5, 2.5, 3.0])", 7.0},
                {"sum_doubles([1, 2.5, 3])", 6.5},
                {"concatenate_strings([\"Hello\", \" \", \"World\", \"!\"])", text{"Hello World!"}},
                {"concatenate_strings([])", text{""}},
                {"concatenate_strings([\"single\"])", text{"single"}},
                {"join_strings([\"hello\", \"world\", \"test\"], \" \")", text{"hello world test"}},
                {"sum_matrix([[1, 2], [3, 4], [5, 6]])", 21},
                {"sum_bound_matrix([[1, 2], [3, 4]])", 10},
                {"var matrix = create_matrix(2, 2); sum_bound_matrix(matrix)", 6},
                {"is_empty_vec([])", true},
                {"is_empty_vec([1])", false},
                {"is_empty_map({})", true},
                {"is_empty_map({\"a\": 1})", false},
                {"count_total_items([{\"a\": 1, \"b\": 2}, {\"c\": 3}, {}])", 3},
                {"sum_all_groups({\"A\": [1, 2, 3], \"B\": [4, 5], \"C\": []})", 15},
                {"sum_string_to_int_map({\"a\": 1, \"b\": 2, \"c\": 3})", 6},
                {"sum_string_to_int_map({})", 0},
                {"sum_string_to_int_map({\"single\": 42})", 42},
                {"sum_string_to_int_map({\"key with spaces\": 1, \"key-with-dashes\": 2, \"key_with_underscores\": 3})", 6},
                {"sum_string_to_int_map({\"\": 42})", 42},
                {"get_map_size({\"one\": 1, \"two\": 2, \"three\": 3, \"four\": 4})", 4},
                {"get_string_keys({\"x\": 10, \"y\": 20, \"z\": 30})", listing{"[\"x\", \"y\", \"z\"]"}},
                {"get_int_values({\"alpha\": 100, \"beta\": 200})", listing{"[100, 200]"}},
                {"map_contains_key({\"exists\": 42, \"also\": 24}, \"exists\")", true},
                {"map_contains_key({\"exists\": 42, \"also\": 24}, \"missing\")", false},
                {"get_map_value({\"key1\": 100, \"key2\": 200}, \"key1\")", 100},
                {"get_map_value({\"key1\": 100, \"key2\": 200}, \"missing\")", -1},
                {"concatenate_string_values({\"a\": \"Hello\", \"b\": \" \", \"c\": \"World\"})", text{"Hello World"}},
                {"concatenate_string_values({})", text{""}},
                {"concatenate_string_values({\"only\": \"entry\"})", text{"entry"}},
                {"sum_double_values({\"x\": 1.5, \"y\": 2.5, \"z\": 3.0})", 7.0},
                {"sum_double_values({\"a\": 1, \"b\": 2, \"c\": 3})", 6.0},
                {"sum_double_values({\"mixed1\": 1, \"mixed2\": 2.5, \"mixed3\": 3})", 6.5},
                {"number_names({1: \"one\", 2: \"two\", 3: \"three\"})", text{"1=one, 2=two, 3=three"}},
                {"var nested = create_nested_map(); sum_nested_map(nested)", 33},
                {"sum_nested_map({\"g1\": {\"a\": 5, \"b\": 10}, \"g2\": {\"x\": 15, \"y\": 20}})", 50},
                {"process_mixed_vector([1, \"hello\", 3.14, true, 42])", 2},
                {"count_map_types({\"a\": 1, \"b\": \"hello\", \"c\": 2, \"d\": 3.14, \"e\": 3})", 3},
            };
            check_values(rows, bind_std_containers);
        });

        test("std_container_returns_become_script_containers", [this]() {
            const value_row rows[] = {
                {"range(1, 6)", listing{"[1, 2, 3, 4, 5]"}},
                {"double_integers([1, 2, 3])", listing{"[2, 4, 6]"}},
                {"create_matrix(3, 3)", listing{"[[0, 1, 2], [3, 4, 5], [6, 7, 8]]"}},
                {"create_scores()", listing{"{\"Alice\": 95, \"Bob\": 87, \"Charlie\": 92}"}},
                {"create_string_int_map()", listing{"{\"first\": 1, \"second\": 2, \"third\": 3}"}},
                {"var original = create_string_int_map(); reverse_map(original)", listing{"{1: \"first\", 2: \"second\", 3: \"third\"}"}},
                {"create_nested_map()", listing{"{\"group1\": {\"a\": 1, \"b\": 2}, \"group2\": {\"x\": 10, \"y\": 20}}"}},
                {"create_mixed_vector()", listing{"[42, \"hello\", 3.14, true]"}},
                {"create_mixed_map()", listing{"{\"bool\": true, \"float\": 3.14, \"int\": 42, \"string\": \"hello\"}"}},
                {"create_points([0.0, 0.0, 3.0, 4.0, 1.0, 1.0])", listing{"[?, ?, ?]"}},
                {"create_person_map()", listing{"{\"bob\": ?, \"jane\": ?, \"john\": ?}"}},
            };
            check_values(rows, bind_std_containers);
        });

        test("bound_class_values_convert_inside_std_containers_on_every_engine", [this]() {
            // each row binds the classes on a fresh engine: a conversion one engine registered serves the next as well
            const value_row rows[] = {
                {"auto points = []; auto p1 = Point(); p1.x = 3.0; p1.y = 4.0; points.push(p1); total_distance(points)", 5.0},
                {"var points = create_points([0.0, 0.0, 3.0, 4.0]); total_distance(points)", 5.0},
                {"var people = create_person_map(); sum_ages(people)", 90},
                {"var people = create_person_map(); get_names(people)", listing{"[\"Bob Johnson\", \"Jane Smith\", \"John Doe\"]"}},
                {"auto widgets = []; widgets.push(Widget(\"first\", 10)); widgets.push(Widget(\"second\", 20)); count_widgets(widgets)", 2},
                {"auto widgets = {}; widgets[\"first\"] = Widget(\"first\", 100); widgets[\"second\"] = Widget(\"second\", 200); get_widget_value(widgets, \"second\")", 200},
                {"auto widgets = {}; widgets[1] = Widget(\"first\", 100); widgets[2] = Widget(\"second\", 200); get_widget_by_id(widgets, 2)", text{"second"}},
                {"auto widgets = create_widgets(); sum_widget_values(widgets)", 6},
                {"auto widgets = []; widgets.push(NonDefaultWidget(\"test\", 42)); count_ndwidgets(widgets)", 1},
                {"auto groups = []; auto group1 = []; group1.push(Widget(\"a\", 1)); group1.push(Widget(\"b\", 2)); groups.push(group1); "
                 "auto group2 = []; group2.push(Widget(\"c\", 3)); groups.push(group2); count_all_widgets(groups)", 3},
                {"auto calcs = []; auto c1 = Calculator(10.0); c1.add(5.0); calcs.push(c1); auto c2 = Calculator(2.0); c2.multiply(3.0); calcs.push(c2); sum_all_results(calcs)", 21.0},
                {"auto dogs = []; dogs.push(Dog(\"Rex\")); dogs.push(Dog(\"Buddy\")); array<var> values = [1, 2, 3, 4, 5]; count_dogs(dogs) + count_script_values(values)", 7},
                {"auto dog = Dog(\"Rex\"); make_speak(dog)", text{"Woof!"}},
            };
            check_values(rows, bind_std_containers);
        });

        test("bound_array_and_bound_map_parameters_read_script_containers", [this]() {
            const value_row rows[] = {
                {"sum_ints([1, 2, 3, 4, 5])", 15},
                {"sum_doubles([1, 2.5, 3, 4.5])", 11.0},
                {"avg_doubles([1, 2.5, 3, 4.5])", 2.75},
                {"count_true([true, false, true])", 2},
                {"join_strings([\"Hello\", \"Enhanced\", \"Conversions\"])", text{"Hello Enhanced Conversions"}},
                {"sum_map_values({\"a\": 10, \"b\": 20, \"c\": 30})", 60},
                {"sum_double_values({\"x\": 1.5, \"y\": 2, \"z\": 3.5})", 7.0},
                {"point_distance(Point(3.0, 4.0))", 5.0},
                {"total_distance([Point(0.0, 0.0), Point(3.0, 4.0), Point(5.0, 12.0)])", 18.0},
                {"create_point(1.5, 2.5).x", 1.5},
                {"create_point(1.5, 2.5).y", 2.5},
                {"sum_ints([2147483647, -2147483648])", -1},
            };
            check_values(rows, bind_bound_containers);
        });

        test("specific_bound_container_registration", [this]() {
            auto engine = make_engine();
            detail::engine_access::add_bound_array_conversion<int>(*engine);
            detail::engine_access::add_bound_array_conversion<std::string>(*engine);
            detail::engine_access::add_bound_map_conversion<std::string, int>(*engine);
            engine->add_function("process_data", [](const bound_array<int>& a_numbers, const bound_array<std::string>& a_words, const bound_map<std::string, int>& a_scores) {
                int result = sum_of(a_numbers) + static_cast<int>(a_words.size()) * 10;
                for (const auto& [key, value] : a_scores) { result += value; }
                return result;
            });
            check_eq(56, engine->execute(R"(process_data([1, 2, 3], ["hello", "world"], {"alice": 10, "bob": 20}))").as<int>());
        });

        test("an_element_a_parameter_cannot_take_raises", [this]() {
            struct error_row { const char* code; bool bound; const char* error; };
            const error_row rows[] = {
                {"sum_integers([\"not\", \"ints\"])", false, ""},
                {"sum_integers([1, \"hello\", 3])", false, ""},
                {"sum_ints([9223372036854775807])", true, ""},
                {"count_true([1, 0, 2, false, true])", true, "Cannot convert int to bool"},
            };
            check_each(rows, [&](const error_row& row) {
                auto e = make_engine();
                row.bound ? bind_bound_containers(*e) : bind_std_containers(*e);
                std::string what;
                check_throws<runtime_error>([&]() {
                    try {
                        e->execute(row.code);
                    } catch (const runtime_error& error) {
                        what = error.what();
                        throw;
                    }
                });
                check_true(what.find(row.error) != std::string::npos, what);
            });
        });

        test("large_vector_performance", [this]() {
            // ten 10k-element conversions cost about what a hundred 1k ones do; a quadratic conversion would cost ten times as much
            auto engine = make_engine();
            engine->execution_budget(0.0);   // a measurement, not a wall-clock gate

            engine->add_function("sum_large", [](std::vector<int> nums) -> int64_t {
                int64_t sum = 0;
                for (int n : nums) sum += n;
                return sum;
            });
            engine->execute("auto small_array = []; for (auto i = 0; i < 1000; i += 1) { small_array.push(i); }");
            engine->execute("auto large_array = []; for (auto i = 0; i < 10000; i += 1) { large_array.push(i); }");
            check_eq(engine->execute("sum_large(large_array)").as<int64_t>(), 49995000LL);

            auto small_call = engine->jaibite("sum_large(small_array);");
            auto large_call = engine->jaibite("sum_large(large_array);");
            check_speed_ratio(compare_speed("10 10k-element vector<int> conversions against 100 1k ones", timed_calls(small_call, 100), timed_calls(large_call, 10), 15), 4.0);
        });

        test("performance_comparison", [this]() {
            // a typed std::vector<int> parameter against a variadic call reading the same array: reported, not bounded
            auto engine = make_engine();

            engine->add_variadic_function("sum_variadic", [engine](std::vector<script_value> args) -> script_value {
                if (args.size() != 1 || !args[0].is_array()) {
                    throw runtime_error("Expected single array argument");
                }
                const auto& arr = args[0].as_array();
                int sum = 0;
                for (const auto& val : arr) {
                    sum += val.as<int>();
                }
                return engine->make_value(sum);
            });

            engine->add_function("sum_typed", [](std::vector<int> nums) -> int {
                return std::accumulate(nums.begin(), nums.end(), 0);
            });

            engine->execute("auto test_array = [];");
            engine->execute("for (auto i = 0; i < 1000; i += 1) { test_array.push(i); }");

            auto variadic = engine->jaibite("sum_variadic(test_array);");
            auto typed = engine->jaibite("sum_typed(test_array);");
            check_eq((int64_t)499500, variadic.execute().as_int(), "the variadic sum");
            check_eq((int64_t)499500, typed.execute().as_int(), "the typed sum");
            compare_speed("a typed vector<int> parameter against a variadic one (1000 elements)", timed_calls(variadic), timed_calls(typed));
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::container_conversion_tests)
