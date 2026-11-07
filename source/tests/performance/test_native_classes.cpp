#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <chrono>

using namespace jai;
using namespace jai::foundry;
using namespace std::chrono;

namespace jai::foundry::tests {

class native_class_performance : public suite {
public:
    native_class_performance() : suite("Native Class Performance") {}
    
    void forge_tests() override {
        test("basic_class_performance", [this]() {
            auto engine = make_engine();
            jai::stdlib::register_all(engine);
            
            auto start = high_resolution_clock::now();
            
            engine->execute(R"(
                class Point {
                    float x = 0.0;
                    float y = 0.0;
                    
                    Point(float px, float py) {
                        x = px;
                        y = py;
                    }
                    
                    float distanceTo(Point other) {
                        auto dx = x - other.x;
                        auto dy = y - other.y;
                        return math::sqrt(dx * dx + dy * dy);
                    }
                }
                
                auto p1 = Point(3.0, 4.0);
                auto p2 = Point(0.0, 0.0);
                p1.distanceTo(p2);
            )");
            
            auto end = high_resolution_clock::now();
            auto duration = duration_cast<microseconds>(end - start).count();
            
            std::cout << "Basic class test completed in " << duration << " uS\n";
            check(true); // Just ensure it runs
        });
        
        test("inheritance_performance", [this]() {
            auto engine = make_engine();
            jai::stdlib::register_all(engine);
            
            auto start = high_resolution_clock::now();
            
            auto result = engine->execute(R"(
                class Animal {
                    string name = "";
                    int energy = 100;
                    
                    Animal(string n) {
                        name = n;
                    }
                    
                    void move() {
                        energy -= 10;
                    }
                }
                
                class Dog : Animal {
                    string breed = "";
                    
                    Dog(string n, string b) : super(n) {
                        breed = b;
                    }
                    
                    void bark() {
                        energy -= 5;
                    }
                }
                
                auto dog = Dog("Buddy", "Golden Retriever");
                dog.move();
                dog.bark();
                dog.energy;
            )");
            
            auto end = high_resolution_clock::now();
            auto duration = duration_cast<microseconds>(end - start).count();
            
            std::cout << "Inheritance test completed in " << duration << " uS\n";
            check_eq(result.as_int(), 85); // Started at 100, -10 for move, -5 for bark
        });
        
        test("cat_tiger_example", [this]() {
            auto engine = make_engine();
            jai::stdlib::register_all(engine);
            
            auto result = engine->execute(R"(
                class Cat {
                    int a = 0;
                    Cat(int val) { this.a = val; }
                }

                class Tiger : Cat {
                    int b = 5;
                    Tiger() : super(5) {}

                    void roar() {
                        print("ROAR! Tiger with a=" + to_string(this.a) + ", b=" + to_string(this.b));
                    }

                    int getTotal() {
                        return this.a + this.b;
                    }
                }

                auto tiger = Tiger();
                tiger.roar();
                tiger.getTotal();
            )");
            
            check_eq(result.as_int(), 10); // a=5, b=5
        });
    }
};

} // namespace

FOUNDRY_REGISTER(jai::foundry::tests::native_class_performance)