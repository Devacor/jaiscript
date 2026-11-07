#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests::deep_copy_fixtures {

struct Point {
    double x = 0.0;
    double y = 0.0;
    Point() = default;
    Point(double a_x, double a_y) : x(a_x), y(a_y) {}
};

class GeometricShape {
public:
    virtual ~GeometricShape() = default;
    virtual double area() const = 0;
    virtual std::string type() const = 0;
};

class GeometricCircle : public GeometricShape {
public:
    double radius;
    GeometricCircle(double r) : radius(r) {}
    double area() const override { return 3.14159 * radius * radius; }
    std::string type() const override { return "Circle"; }
};

class GeometricRectangle : public GeometricShape {
public:
    double width, height;
    GeometricRectangle(double w, double h) : width(w), height(h) {}
    double area() const override { return width * height; }
    std::string type() const override { return "Rectangle"; }
};

class test_object {
public:
    int value;
    std::string name;
    test_object() : value(0), name("default") {}
    test_object(int v, const std::string& n) : value(v), name(n) {}
    int get_value() const { return value; }
    void set_value(int v) { value = v; }
    std::string get_name() const { return name; }
    void set_name(const std::string& n) { name = n; }
};

class derived_object : public test_object {
public:
    double extra;
    derived_object() : test_object(), extra(0.0) {}
    derived_object(int v, const std::string& n, double e) : test_object(v, n), extra(e) {}
    double get_extra() const { return extra; }
    void set_extra(double e) { extra = e; }
};

void bind_copyable_classes(engine& a_engine) {
    dynamic_binder<Point>(a_engine, "Point")
        .constructor<>()
        .constructor<double, double>()
        .property("x", &Point::x)
        .property("y", &Point::y)
        .build();
    dynamic_binder<GeometricShape>(a_engine, "GeometricShape")
        .method("area", &GeometricShape::area)
        .method("type", &GeometricShape::type)
        .build();
    dynamic_binder<GeometricCircle>(a_engine, "GeometricCircle")
        .base_class<GeometricShape>()
        .constructor<double>()
        .property("radius", &GeometricCircle::radius)
        .build();
    dynamic_binder<GeometricRectangle>(a_engine, "GeometricRectangle")
        .base_class<GeometricShape>()
        .constructor<double, double>()
        .property("width", &GeometricRectangle::width)
        .property("height", &GeometricRectangle::height)
        .build();
    dynamic_binder<test_object>(a_engine, "test_object")
        .constructor<int, script_string>()
        .method("get_value", &test_object::get_value)
        .method("set_value", &test_object::set_value)
        .method("get_name", &test_object::get_name)
        .method("set_name", &test_object::set_name)
        .property("value", &test_object::value)
        .property("name", &test_object::name)
        .build();
    dynamic_binder<derived_object>(a_engine, "derived_object")
        .constructor<int, script_string, script_float>()
        .base_class<test_object>()
        .method("get_extra", &derived_object::get_extra)
        .method("set_extra", &derived_object::set_extra)
        .build();
}

} // namespace jai::foundry::tests::deep_copy_fixtures

namespace jai::foundry::tests {

class deep_copy_tests : public suite {
public:
    deep_copy_tests() : suite("Deep Copy Tests") {}

    void forge_tests() override {
        test("containers_copy_deeply_and_a_reference_parameter_shares", [this]() {
            const std::pair<const char*, script_int> int_rows[] = {
                {R"(
                    var map1 = {"a": 1, "b": 2};
                    var map2 = map1;
                    map2["c"] = 3;
                    map1["a"] + map1.size() * 10
                )", 21},
                {R"(
                    var arr1 = [1, 2, 3];
                    var arr2 = arr1;
                    arr2.push(4);
                    arr1.size()
                )", 3},
                {R"(
                    var data1 = {"array": [1, 2, 3], "map": {"x": 10, "y": 20}};
                    var data2 = data1;
                    data2["array"].push(4);
                    data2["map"]["z"] = 30;
                    data1["array"].size() + data1["map"].size() * 10
                )", 23},
                {R"(
                    var map1 = {"a": 1};
                    var map2 = {"b": 2};
                    map2 = map1;
                    map2["c"] = 3;
                    map1.size()
                )", 1},
                {R"(
                    auto modify_map(auto m) -> auto {
                        m["modified"] = true;
                        return m.size();
                    }
                    var original = {"a": 1, "b": 2};
                    var size = modify_map(original);
                    original.size()
                )", 2},
                {R"(
                    var data = {"value": 42};
                    auto modify_ref(auto& m) -> auto {
                        m["value"] = 100;
                    }
                    modify_ref(data);
                    data["value"]
                )", 100},
            };
            check_each(int_rows, [&](const auto& row) {
                auto engine = make_engine();
                stdlib::register_all(*engine);
                check_eq(row.second, engine->execute(row.first).template as<script_int>());
            });
            const std::tuple<const char*> true_rows[] = {
                R"(
                    var map1 = {"a": 1, "b": [1, 2, 3]};
                    var map2 = map1;
                    map2["c"] = 3;
                    map2["b"].push(4);
                    map1.size() == 2 && map1["b"].size() == 3
                )",
                R"(
                    var data = {"array": [{"x": 1, "y": 2}, {"x": 3, "y": 4}], "nested": {"deep": {"value": 42}}};
                    var copy = data;
                    copy["array"][0]["x"] = 99;
                    copy["nested"]["deep"]["value"] = 100;
                    data["array"][0]["x"] == 1 && data["nested"]["deep"]["value"] == 42
                )",
                R"(
                    var arr1 = [1, 2, 3];
                    var arr2 = [4, 5];
                    arr2 = arr1;
                    arr2.push(4);
                    arr1.size() == 3 && arr2.size() == 4
                )",
                R"(
                    function modify(auto arr) {
                        arr.push(99);
                        return arr.size();
                    }
                    var original = [1, 2, 3];
                    var size = modify(original);
                    original.size() == 3 && size == 4
                )",
            };
            check_each(true_rows, [&](const auto& row) {
                auto engine = make_engine();
                stdlib::register_all(*engine);
                check(engine->execute(std::get<0>(row)).template as<script_bool>(), std::get<0>(row));
            });
        });

        test("bound_objects_copy_deeply_with_their_class_and_inside_containers", [this]() {
            auto engine = make_engine();
            stdlib::register_all(*engine);
            deep_copy_fixtures::bind_copyable_classes(*engine);

            const char* true_rows[] = {
                "var p1 = Point(3.0, 4.0); var p2 = p1; p2.x = 5.0; p2.y = 6.0; p1.x == 3.0 && p1.y == 4.0",
                "var shape1 = GeometricCircle(5.0); var shape2 = shape1; shape2.radius = 10.0; shape1.area() < 80 && shape2.area() > 300",
                "var points = [Point(1.0, 2.0), Point(3.0, 4.0), Point(5.0, 6.0)]; var copy = points; copy[0].x = 99.0; "
                "copy.push(Point(7.0, 8.0)); points.size() == 3 && points[0].x == 1.0",
            };
            for (const char* code : true_rows) {
                check(engine->execute(code).as<script_bool>(), code);
            }

            engine->execute(R"(
                var obj1 = test_object(42, "original");
                var obj2 = obj1;
                obj2.set_value(100);
                obj2.set_name("modified");
                var der1 = derived_object(42, "original", 3.14);
                var der2 = der1;
                der2.set_value(100);
                der2.set_name("modified");
                der2.set_extra(2.71);
                var obj = test_object(42, "original");
                var arr1 = [obj];
                var arr2 = arr1;
                arr2[0].set_value(100);
                var map1 = {"key": obj};
                var map2 = map1;
                map2["key"].set_name("modified");
            )");
            check_eq(engine->execute("obj1.get_value()").as<int>(), 42);
            check_eq(engine->execute("obj1.get_name()").as<std::string>(), "original");
            check_eq(engine->execute("obj2.get_value()").as<int>(), 100);
            check_eq(engine->execute("obj2.get_name()").as<std::string>(), "modified");
            check_eq(engine->execute("der1.get_value()").as<int>(), 42);
            check_eq(engine->execute("der1.get_name()").as<std::string>(), "original");
            check_eq(engine->execute("der1.get_extra()").as<double>(), 3.14);
            check_eq(engine->execute("der2.get_value()").as<int>(), 100);
            check_eq(engine->execute("der2.get_name()").as<std::string>(), "modified");
            check_eq(engine->execute("der2.get_extra()").as<double>(), 2.71);
            check_eq(engine->execute("obj.get_value()").as<int>(), 42);
            check_eq(engine->execute("obj.get_name()").as<std::string>(), "original");
            check_eq(engine->execute("arr1[0].get_value()").as<int>(), 42);
            check_eq(engine->execute("arr2[0].get_value()").as<int>(), 100);
            check_eq(engine->execute("map1[\"key\"].get_name()").as<std::string>(), "original");
            check_eq(engine->execute("map2[\"key\"].get_name()").as<std::string>(), "modified");
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::deep_copy_tests)
