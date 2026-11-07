#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <iostream>
#include <cmath>

namespace jai::foundry::tests {

class TestPoint {
public:
    double x, y;
    
    TestPoint() : x(0), y(0) {
        std::cout << "TestPoint default constructor called\n";
    }
    TestPoint(double x, double y) : x(x), y(y) {
        std::cout << "TestPoint constructor called with x=" << x << ", y=" << y << "\n";
    }
    
    double length() const { return std::sqrt(x * x + y * y); }
};

class constructor_debug_tests : public suite {
public:
    constructor_debug_tests() : suite("Constructor Debug") {}
    
    void forge_tests() override {
        test("constructor_with_ints", [this]() {
            auto eng = make_engine();
            
            std::cout << "Registering TestPoint class...\n";
            dynamic_binder<TestPoint>(*eng, "TestPoint")
                .constructor<>()
                .constructor<double, double>()
                .method("length", &TestPoint::length)
                .property("x", &TestPoint::x)
                .property("y", &TestPoint::y)
                .build();
            
            std::cout << "Executing TestPoint(3, 4)...\n";
            try {
                auto result = eng->execute("auto p = TestPoint(3, 4); p.length()");
                std::cout << "Success! Result: " << result.as<double>() << "\n";
                check_eq(result.as<double>(), 5.0);
            } catch (const std::exception& e) {
                std::cout << "FAILED: " << e.what() << "\n";
                throw;
            }
        });
        
        test("direct_value_conversion", [this]() {
            auto eng = make_engine();
            // Test the script_value conversion directly
            script_value intVal(script_int(3), eng.get());
            std::cout << "intVal type: " << static_cast<int>(intVal.type()) << " (should be 1 for int)\n";
            
            try {
                double d = intVal.as<double>();
                std::cout << "Converted int 3 to double: " << d << "\n";
                check_eq(d, 3.0);
            } catch (const std::exception& e) {
                std::cout << "FAILED to convert int to double: " << e.what() << "\n";
                throw;
            }
        });
    }
};

} // namespace jai::foundry::tests

// Auto-register with the test framework
FOUNDRY_REGISTER(jai::foundry::tests::constructor_debug_tests)