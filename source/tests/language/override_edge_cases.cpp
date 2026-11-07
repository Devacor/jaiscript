#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <optional>
#include <variant>
#include "../case_table.hpp"

namespace jai::foundry::tests {

class override_edge_cases : public suite {
public:
    override_edge_cases() : suite("Override Edge Cases") {}
    
    void forge_tests() override {
        test("a_base_typed_slot_calls_the_most_derived_override", [this]() {
            struct dispatch_case { const char* code; const char* classes; std::variant<int, std::string> expected; };
            const char* const chain = R"(
                class A { auto value() { return "A"; } }
                class B : A { auto value() override { return "B"; } }
                class C : B { auto value() override { return "C"; } }
                class D : C { auto value() override { return "D"; } }
            )";
            const char* const animals = R"(
                class Animal {
                    auto speak() { return "..."; }
                    auto move() { return "moves"; }
                    auto eat() { return "eats"; }
                }
                class Dog : Animal {
                    auto speak() override { return "Woof!"; }
                    auto move() override { return "runs"; }
                }
            )";
            const char* const devices = R"(
                class Device { auto power() { return "Device power"; } }
                class Phone : Device { auto power() override { return "Phone power"; } }
                class Computer : Device { auto power() override { return "Computer power"; } }
            )";
            const char* const skip = R"(
                class A { auto method() { return 1; } }
                class B : A { auto other() { return 2; } }
                class C : B { auto method() override { return 3; } }
            )";
            const char* const numbers = R"(
                class A { auto value() { return 1; } }
                class B : A { auto value() override { return 2; } }
                class C : B { auto value() override { return 3; } }
            )";
            const char* const math = R"(
                class Math { auto calculate(int x) { return x * 2; } }
                class AdvancedMath : Math { auto calculate(int x) override { return x * x; } }
            )";
            const dispatch_case cases[] = {
                {"A a = D(); a.value();", chain, std::string("D")},
                {"B b = D(); b.value();", chain, std::string("D")},
                {"C c = D(); c.value();", chain, std::string("D")},
                {"Animal a = Dog(); a.speak();", animals, std::string("Woof!")},
                {"Animal a = Dog(); a.move();", animals, std::string("runs")},
                {"Animal a = Dog(); a.eat();", animals, std::string("eats")},
                {"Device d = Phone(); d.power();", devices, std::string("Phone power")},
                {"Device d = Computer(); d.power();", devices, std::string("Computer power")},
                {"A a = C(); a.method();", skip, 3},
                {"B b = C(); b.method();", skip, 3},
                {"A a = B(); a.value();", numbers, 2},
                {"A a = C(); a.value();", numbers, 3},
                {"B b = C(); b.value();", numbers, 3},
                {"Math m = AdvancedMath(); m.calculate(5);", math, 25},
            };
            check_each(cases, [](const dispatch_case& c) {
                auto engine = make_engine();
                engine->execute(c.classes);
                auto result = engine->execute(c.code);
                if (const auto* i = std::get_if<int>(&c.expected)) {
                    check_eq(result.as<int>(), *i);
                } else {
                    check_eq(result.as<std::string>(), std::get<std::string>(c.expected));
                }
            });
        });

        test("override_with_different_return_types", [this]() {
            auto engine = make_engine();
            
            // Override should work with compatible return types
            engine->execute(R"(
                class Shape {
                    auto area() { return 0.0; }
                }
                
                class Circle : Shape {
                    float radius = 5.0;
                    auto area() override { return 3.14159 * radius * radius; }
                }
            )");
            
            auto result = engine->execute("Shape s = Circle(); s.area();");
            check_near(result.as<double>(), 78.53975, 0.001);
        });
        
        test("override_method_calling_super", [this]() {
            auto engine = make_engine();
            
            // Override that calls parent implementation
            engine->execute(R"(
                class Logger {
                    auto log(string msg) { return "[LOG] " + msg; }
                }
                
                class TimestampLogger : Logger {
                    auto log(string msg) override { 
                        return "[TIME] " + super::log(msg); 
                    }
                }
            )");
            
            auto result = engine->execute(R"(
                Logger l = TimestampLogger();
                l.log("Hello");
            )");
            
            check_eq(result.as<std::string>(), "[TIME] [LOG] Hello");
        });
        
        test("override_with_private_base_method", [this]() {
            auto engine = make_engine();
            
            // Private methods shouldn't be overrideable (when we add private support)
            // For now, test that methods work as expected
            engine->execute(R"(
                class Base {
                    auto internal() { return 42; }  // Would be private
                    auto public_method() { return internal() * 2; }
                }
                
                class Derived : Base {
                    // When private is supported, this should NOT override the private method
                    // For now, we use 'override' keyword to explicitly shadow the base method
                    auto internal() override { return 99; }
                    auto public_method() override { return internal() * 3; }
                }
            )");
            
            auto result = engine->execute("Base b = Derived(); b.public_method();");
            check_eq(result.as<int>(), 297);  // 99 * 3
        });

        test("shadowing_without_override_fails", [this]() {
            auto engine = make_engine();

            // Attempting to shadow a base class method without 'override' keyword should fail
            check_throws([&]() {
                engine->execute(R"(
                    class Base {
                        auto method() { return 42; }
                    }

                    class Derived : Base {
                        // Missing 'override' keyword - should fail
                        auto method() { return 99; }
                    }
                )");
            });

            // Also test that using 'override' makes it work
            engine->execute(R"(
                class BaseOk {
                    auto method() { return 42; }
                }

                class DerivedOk : BaseOk {
                    auto method() override { return 99; }
                }
            )");

            auto result = engine->execute("BaseOk b = DerivedOk(); b.method();");
            check_eq(result.as<int>(), 99);
        });

        test("constructor_with_override_in_same_class", [this]() {
            auto engine = make_engine();
            jai::stdlib::register_all(*engine);  // Register stdlib including to_string

            // Class with both constructor and override methods
            engine->execute(R"(
                class Vehicle {
                    string type = "generic";
                    auto describe() { return "Vehicle: " + type; }
                }
                
                class Car : Vehicle {
                    int wheels;
                    Car(int w) : super() { 
                        wheels = w; 
                        type = "car";
                    }
                    auto describe() override { 
                        return "Car with " + to_string(wheels) + " wheels"; 
                    }
                }
            )");
            
            auto result = engine->execute(R"(
                Vehicle v = Car(4);
                v.describe();
            )");
            
            check_eq(result.as<std::string>(), "Car with 4 wheels");
        });
        
        test("override_with_field_access", [this]() {
            auto engine = make_engine();
            
            // Override methods that access fields
            engine->execute(R"(
                class Counter {
                    int count = 0;
                    auto increment() { count = count + 1; return count; }
                    auto get_count() { return count; }
                }
                
                class DoubleCounter : Counter {
                    auto increment() override { 
                        count = count + 2; 
                        return count; 
                    }
                }
            )");
            
            auto result = engine->execute(R"(
                Counter c = DoubleCounter();
                c.increment();
                c.increment();
                c.get_count();
            )");
            
            check_eq(result.as<int>(), 4);  // Two increments of 2 each
        });
        
        test("multiple_override_errors_in_one_class", [this]() {
            auto engine = make_engine();
            
            engine->execute(R"(
                class Base {
                    auto foo() { return 1; }
                    auto bar() { return 2; }
                }
            )");
            
            // Try to define a class with multiple override errors
            try {
                engine->execute(R"([[warn_mode]];   // the run time reports the first (the checker lists both at load)
                    class Bad : Base {
                        auto foo() { return 10; }  // Missing override
                        auto baz() override { return 20; }  // No such method in base
                    }
                )");
                throw test_failure("Expected exception for override errors");
            } catch (const runtime_error& e) {
                // Should catch the first error encountered
                check_true(
                    std::string(e.what()).find("redefines 'Base::foo': mark it 'override'") != std::string::npos ||
                    std::string(e.what()).find("no base class method found") != std::string::npos,
                    "Should report override error"
                );
            }
        });
        
        test("override_never_crosses_static_and_instance", [this]() {
            const char* const cases[] = {
                {R"(
                    class Base { auto method() { return 42; } }
                    class Derived : Base { static auto method() override { return 99; } }
                )"},
                {R"(
                    class Base { static auto method() { return 42; } }
                    class Derived : Base { auto method() override { return 99; } }
                )"},
            };
            check_each(cases, [](const char* c) {
                auto engine = make_engine();
                check_throws([&]() { engine->execute(c); });
            });
        });

        // static methods answer only Class::method() and instance methods only obj.method(); an instance method hides a base's
        // static of the same name (C++ name hiding)
        test("same_named_static_and_instance_methods_across_a_base_and_derived_class", [this]() {
            struct same_name_case { const char* code; const char* classes; std::optional<int> expected; };
            const char* const static_static = R"(
                class Base { static auto method() { return 1; } }
                class Derived : Base { static auto method() { return 2; } }
            )";
            const char* const instance_static = R"(
                class Base { auto method() { return 10; } }
                class Derived : Base { static auto method() { return 20; } }
            )";
            const char* const static_instance = R"(
                class Base {
                    Base() {}
                    static auto method() { return 100; }
                }
                class Derived : Base {
                    Derived() : super() {}
                    auto method() { return 200; }
                }
            )";
            const char* const instance_instance = R"(
                class Base { auto method() { return 5; } }
                class Derived : Base { auto method() override { return 50; } }
            )";
            const same_name_case cases[] = {
                {"Base::method();", static_static, 1},
                {"Derived::method();", static_static, 2},
                {"Base b = Base(); b.method();", static_static, std::nullopt},
                {"Derived d = Derived(); d.method();", static_static, std::nullopt},
                {"Base::method();", instance_static, std::nullopt},
                {"Base b = Base(); b.method();", instance_static, 10},
                {"Derived::method();", instance_static, 20},
                {"Derived d = Derived(); d.method();", instance_static, 10},
                {"Base::method();", static_instance, 100},
                {"Base b = Base(); b.method();", static_instance, std::nullopt},
                {"Derived::method();", static_instance, std::nullopt},
                {"Derived d = Derived(); d.method();", static_instance, 200},
                {"Base::method();", instance_instance, std::nullopt},
                {"Base b = Base(); b.method();", instance_instance, 5},
                {"Derived::method();", instance_instance, std::nullopt},
                {"Derived d = Derived(); d.method();", instance_instance, 50},
                {"Base b2 = Derived(); b2.method();", instance_instance, 50},
            };
            check_each(cases, [](const same_name_case& c) {
                auto engine = make_engine();
                engine->execute(c.classes);
                if (c.expected) {
                    check_eq(engine->execute(c.code).as<int>(), *c.expected);
                } else {
                    check_throws([&]() { engine->execute(c.code); });
                }
            });
        });

        test("static_methods_no_virtual_dispatch", [this]() {
            auto engine = make_engine();

            // Static methods are resolved at compile-time via Class::
            engine->execute(R"(
                class Base {
                    static auto getValue() { return 1; }
                    auto getInstance() { return Base::getValue(); }
                }

                class Derived : Base {
                    static auto getValue() { return 2; }
                }
            )");

            // When called through base reference, uses base's static method
            auto result1 = engine->execute("Base b = Derived(); b.getInstance();");
            check_eq(result1.as<int>(), 1);  // Uses Base::getValue(), not Derived::getValue()

            // Direct static calls use the class specified
            auto result2 = engine->execute("Base::getValue();");
            check_eq(result2.as<int>(), 1);

            auto result3 = engine->execute("Derived::getValue();");
            check_eq(result3.as<int>(), 2);
        });

        test("mixed_static_instance_same_name", [this]() {
            auto engine = make_engine();

            // A class can have both static and instance methods with same name
            // They're in different namespaces (Class::method vs obj.method)
            engine->execute(R"(
                class Counter {
                    int value = 0;
                    static int total = 0;

                    Counter(int v) {
                        value = v;
                        total = total + v;
                    }

                    auto getValue() { return value; }
                    static auto getValue() { return total; }
                }
            )");

            // Instance method
            auto result1 = engine->execute("Counter c = Counter(10); c.getValue();");
            check_eq(result1.as<int>(), 10);

            // Static method
            auto result2 = engine->execute("Counter::getValue();");
            check_eq(result2.as<int>(), 10);  // total accumulated

            // Create another instance
            engine->execute("Counter c2 = Counter(5);");

            // Static method sees accumulated total
            auto result3 = engine->execute("Counter::getValue();");
            check_eq(result3.as<int>(), 15);
        });
    }
};

}

FOUNDRY_REGISTER(jai::foundry::tests::override_edge_cases)