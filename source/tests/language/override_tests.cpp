#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>

namespace jai::foundry::tests {

class override_tests : public suite {
public:
    override_tests() : suite("Override and Virtual Tests") {}
    
    void forge_tests() override {
        test("one_call_site_dispatches_many_classes_and_follows_a_redefinition", [this]() {
            // a method site meeting more classes than one keeps each class's method, and a redefined class answers anew
            auto engine = make_engine();
            engine->execute(R"(
                class Shape { int area(int k) { return 0; } }
                class Sq : Shape { int area(int k) override { return 1 * k; } }
                class Tri : Shape { int area(int k) override { return 10 * k; } }
                class Hex : Shape { int area(int k) override { return 100 * k; } }
                class Oct : Shape { int area(int k) override { return 1000 * k; } }
                class Dec : Shape { int area(int k) override { return 10000 * k; } }
                int sum(array<shared_ptr<Shape>>& shapes, int rounds) {
                    int total = 0;
                    for (int r = 0; r < rounds; ++r) {
                        for (int i = 0; i < shapes.size(); ++i) { total += shapes[i].area(1); }
                    }
                    return total;
                }
                array<shared_ptr<Shape>> shapes = [new Sq(), new Tri(), new Hex(), new Oct(), new Dec(), new Tri(), new Sq()];
            )");
            check_eq((int64_t)(3 * 11122), engine->execute("sum(shapes, 3)").as_int());
            engine->execute("class Hex : Shape { int area(int k) override { return 7 * k; } }");
            check_eq((int64_t)(2 * 11029), engine->execute("sum(shapes, 2)").as_int());
        });

        test("bare_sibling_calls_follow_the_receiver_class_through_inheritance", [this]() {
            // a base method's bare calls of a virtual and of an inherited sibling, run on receivers of several classes
            auto engine = make_engine();
            engine->execute(R"(
                class Base {
                    int pick() { return 1; }
                    int helper(int k) { return k * 1000; }
                    int run(int k) { return pick() + helper(k); }
                }
                class A : Base { int pick() override { return 2; } }
                class B : Base { int pick() override { return 3; } }
                class C : A { int pick() override { return 4; } }
                class E : Base {}
                int sum(array<shared_ptr<Base>>& xs, int rounds) {
                    int total = 0;
                    for (int r = 0; r < rounds; ++r) {
                        for (int i = 0; i < xs.size(); ++i) { total += xs[i].run(1); }
                    }
                    return total;
                }
                array<shared_ptr<Base>> xs = [new A(), new B(), new C(), new E(), new Base(), new B()];
            )");
            check_eq((int64_t)(3 * (6000 + 2 + 3 + 4 + 1 + 1 + 3)), engine->execute("sum(xs, 3)").as_int());
            engine->execute("class Base { int pick() { return 1; } int helper(int k) { return k * 7; } int run(int k) { return pick() + helper(k); } }");
            check_eq((int64_t)(2 * (42 + 2 + 3 + 4 + 1 + 1 + 3)), engine->execute("sum(xs, 2)").as_int());
        });

        test("override_promotes_to_virtual", [this]() {
            auto engine = make_engine();
            
            // Base class with non-virtual method
            engine->execute(R"(
                class Base {
                    auto greet() { return "Base"; }
                }
            )");
            
            // Derived class with override - should promote Base::greet to virtual
            engine->execute(R"(
                class Derived : Base {
                    auto greet() override { return "Derived"; }
                }
            )");
            
            // Test polymorphic behavior
            auto result = engine->execute(R"(
                Base b = Derived();
                b.greet();
            )");
            
            check_eq(result.as<std::string>(), "Derived");
        });
        
        test("override_keyword_required_for_virtual_methods", [this]() {
            auto engine = make_engine();
            
            engine->execute(R"(
                class Animal {
                    auto speak() { return "..."; }
                }
                
                class Dog : Animal {
                    auto speak() override { return "Woof"; }
                }
            )");
            
            // Now Animal::speak is virtual, so Cat MUST use override
            try {
                engine->execute(R"([[warn_mode]];   // the run time's verdict (the checker reports it at load)
                    class Cat : Animal {
                        auto speak() { return "Meow"; }  // Missing override!
                    }
                )");
                throw test_failure("Expected exception for missing override keyword");
            } catch (const runtime_error& e) {
                check_eq(std::string("Method 'speak' redefines 'Animal::speak': mark it 'override'"), std::string(e.what()));
            }
        });
        
        test("override_without_base_method_throws", [this]() {
            auto engine = make_engine();
            
            engine->execute(R"(
                class Base {
                    auto foo() { return 42; }
                }
            )");
            
            try {
                engine->execute(R"([[warn_mode]];
                    class Derived : Base {
                        auto bar() override { return 99; }  // No bar() in Base!
                    }
                )");
                throw test_failure("Expected exception for override without base method");
            } catch (const runtime_error& e) {
                check_true(std::string(e.what()).find("no base class method found") != std::string::npos,
                          "Exception message should contain 'no base class method found'");
            }
        });
        
        test("shadowing_without_override_throws", [this]() {
            auto engine = make_engine();
            
            engine->execute(R"(
                class Base {
                    auto compute() { return 1; }
                }
            )");
            
            try {
                engine->execute(R"([[warn_mode]];
                    class Derived : Base {
                        auto compute() { return 2; }  // Shadows Base::compute without override
                    }
                )");
                throw test_failure("Expected exception for shadowing without override");
            } catch (const runtime_error& e) {
                check_eq(std::string("Method 'compute' redefines 'Base::compute': mark it 'override'"), std::string(e.what()));
            }
        });
        
        test("constructor_not_affected_by_override", [this]() {
            auto engine = make_engine();
            
            // Constructors shouldn't be virtual/override
            engine->execute(R"(
                class Base {
                    int x;
                    Base(int val) { x = val; }
                }
                
                class Derived : Base {
                    int y;
                    Derived(int a, int b) : super(a) { y = b; }
                }
            )");
            
            auto result = engine->execute(R"(
                auto d = Derived(10, 20);
                d.x + d.y;
            )");
            
            check_eq(result.as<int>(), 30);
        });
    }
};

}

FOUNDRY_REGISTER(jai::foundry::tests::override_tests)