#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <cstdint>
#include <memory>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {
namespace conversion_tests_ns {

class TestObject {
public:
    int value;
    std::string name;

    TestObject() : value(0), name("default") {}
    TestObject(int v, const std::string& n) : value(v), name(n) {}

    int get_value() const { return value; }
    void set_value(int v) { value = v; }
    const std::string& get_name() const { return name; }
    void set_name(const std::string& n) { name = n; }
};

class conversion_tests : public suite {
public:
    conversion_tests() : suite("Script Value Conversions") {}

    void forge_tests() override {
        test("a_host_reads_a_scalar_as_every_cpp_type_it_fits", [this]() {
            auto engine = make_engine();
            jai::stdlib::register_all(*engine);

            script_value int_val = engine->execute("42");
            check_eq(int_val.as<int>(), 42);
            check_eq(int_val.as<int64_t>(), 42L);
            check_eq(int_val.as<size_t>(), 42U);
            check_eq(int_val.as<script_int>(), 42);
            check_eq(int_val.as<int32_t>(), 42);
            check_eq(static_cast<int8_t>(engine->execute("127;").as<script_int>()), static_cast<int8_t>(127));
            check_eq(static_cast<int16_t>(engine->execute("32000;").as<script_int>()), static_cast<int16_t>(32000));
            check_eq(static_cast<int32_t>(engine->execute("123456;").as<script_int>()), static_cast<int32_t>(123456));
            check_eq(engine->execute("9876543210;").as<script_int>(), static_cast<int64_t>(9876543210));
            check_eq(static_cast<uint8_t>(engine->execute("255;").as<script_int>()), static_cast<uint8_t>(255));
            check_eq(static_cast<uint16_t>(engine->execute("65000;").as<script_int>()), static_cast<uint16_t>(65000));
            check_eq(static_cast<uint32_t>(engine->execute("4000000000;").as<script_int>()), static_cast<uint32_t>(4000000000));
            check_eq(static_cast<uint64_t>(engine->execute("9223372036854775807;").as<script_int>()), static_cast<uint64_t>(9223372036854775807LL));

            script_value float_val = engine->execute("3.14");
            check_eq(float_val.as<double>(), 3.14);
            check_eq(float_val.as<float>(), 3.14f);
            script_value pi = engine->execute("3.14159;");
            check_near(pi.as<script_float>(), 3.14159, 0.00001);
            check_near(static_cast<float>(pi.as<script_float>()), 3.14159f, 0.00001f);
            check_near(engine->execute("var x = 5; var y = 2.5; x + y;").as<double>(), 7.5, 0.001);
            check_eq(engine->execute("var x = 7.8; x;").as<int>(), 7);

            script_value string_val = engine->execute("\"hello world\"");
            check_eq(string_val.as<std::string>(), "hello world");
            check_eq(string_val.as_string(), "hello world");
            check_eq(engine->execute("\"line1\\nline2\\ttab\";").as_string(), "line1\nline2\ttab");
            check_eq(engine->execute("\"Number: \" + to_string(42);").as_string(), "Number: 42");

            check_eq(engine->execute("true").as<bool>(), true);
            check_eq(engine->execute("true;").as<script_bool>(), true);
            check_eq(engine->execute("false;").as<bool>(), false);
            check_eq(engine->execute("false;").as<script_bool>(), false);

            check_eq(engine->execute("'A'").as<char>(), 'A');

            script_value null_val = engine->execute("null;");
            check(null_val.is_null());
            check_eq(null_val.type(), script_value_type::jai_null_type);
        });

        test("a_host_reads_container_elements_by_their_type", [this]() {
            auto engine = make_engine();

            auto int_vec = engine->execute("[1, 2, 3, 4, 5]").as<std::vector<script_value>>();
            check_eq(int_vec.size(), 5U);
            check_eq(int_vec[0].as<int>(), 1);
            check_eq(int_vec[4].as<int>(), 5);

            auto string_vec = engine->execute("[\"hello\", \"world\", \"test\"]").as<std::vector<script_value>>();
            check_eq(string_vec.size(), 3U);
            check_eq(string_vec[0].as<std::string>(), "hello");
            check_eq(string_vec[2].as<std::string>(), "test");

            auto mixed_vec = engine->execute("[1, \"hello\", 3.14, true]").as<std::vector<script_value>>();
            check_eq(mixed_vec.size(), 4U);
            check_eq(mixed_vec[0].as<int>(), 1);
            check_eq(mixed_vec[1].as<std::string>(), "hello");
            check_eq(mixed_vec[2].as<double>(), 3.14);
            check_eq(mixed_vec[3].as<bool>(), true);

            auto array = engine->execute("[42, 3.14, \"hello\", true];").as_array();
            check_eq(array[0].as<int>(), 42);
            check_near(array[1].as<double>(), 3.14, 0.01);
            check_eq(array[2].as_string(), "hello");
            check_eq(array[3].as<bool>(), true);

            auto map_ref = engine->execute("{\"key1\": 1, \"key2\": 2, \"key3\": 3}").as<script_map>();
            check_eq(map_ref.size(), 3U);
            check_eq(map_ref.at(script_value("key1", engine.get())).as<int>(), 1);
            check_eq(map_ref.at(script_value("key2", engine.get())).as<int>(), 2);

            auto m = engine->execute(R"({"int": 42, "float": 3.14, "string": "hello", "bool": true})").as_map();
            check_eq(m[script_value("int", engine.get())].as<int>(), 42);
            check_near(m[script_value("float", engine.get())].as<double>(), 3.14, 0.01);
            check_eq(m[script_value("string", engine.get())].as_string(), "hello");
            check_eq(m[script_value("bool", engine.get())].as<bool>(), true);
        });

        test("bound_class_conversions", [this]() {
            auto engine = make_engine();

            dynamic_binder<TestObject>(*engine, "TestObject")
                .constructor<>()
                .constructor<int, const std::string&>()
                .method("getValue", &TestObject::get_value)
                .method("setValue", &TestObject::set_value)
                .method("getName", &TestObject::get_name)
                .method("setName", &TestObject::set_name)
                .property("value", &TestObject::value)
                .property("name", &TestObject::name)
                .build();

            auto obj_ptr = engine->execute("TestObject(42, \"test\")").as<std::shared_ptr<TestObject>>();
            check_eq(obj_ptr->get_value(), 42);
            check_eq(obj_ptr->get_name(), "test");

            script_value result = engine->execute(R"(
                var obj = TestObject(10, "hello");
                obj.setValue(20);
                obj.setName("world");
                obj.getValue();
            )");
            check_eq(result.as<int>(), 20);
        });
    }
};

} // namespace conversion_tests_ns
} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::conversion_tests_ns::conversion_tests)
