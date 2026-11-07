#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class static_method_tests : public suite {
public:
    static_method_tests() : suite("Static Method Tests") {}
    
    void forge_tests() override {
        test("basic_static_method", [&]() {
            auto eng = make_engine();
            
            eng->execute(R"(
                class MathUtils {
                    static function add(int a, int b) -> int {
                        return a + b;
                    }
                    
                    static function multiply(int x, int y) -> int {
                        return x * y;
                    }
                }
            )");
            
            // Call static methods directly via class name
            check_eq(eng->execute("MathUtils::add(5, 3)").as<int>(), 8);
            check_eq(eng->execute("MathUtils::multiply(4, 7)").as<int>(), 28);
        });
        
        test("static_method_with_string", [&]() {
            auto eng = make_engine();
            
            eng->execute(R"(
                class StringUtils {
                    static function concat(string a, string b) -> string {
                        return a + b;
                    }
                    
                    static function repeat(string str, int count) -> string {
                        auto result = "";
                        for (auto i = 0; i < count; i = i + 1) {
                            result = result + str;
                        }
                        return result;
                    }
                }
            )");
            
            check_eq(eng->execute("StringUtils::concat(\"Hello\", \" World\")").as<std::string>(), "Hello World");
            check_eq(eng->execute("StringUtils::repeat(\"A\", 3)").as<std::string>(), "AAA");
        });
        
        test("static_method_accessing_static_fields", [&]() {
            auto eng = make_engine();
            
            eng->execute(R"(
                class Counter {
                    static int count = 0;
                    
                    static function increment() {
                        count = count + 1;
                    }
                    
                    static function getCount() -> int {
                        return count;
                    }
                    
                    static function reset() {
                        count = 0;
                    }
                }
            )");

            // Initial count should be 0
            check_eq(eng->execute("Counter::getCount()").as<int>(), 0);
            
            // Increment and check
            eng->execute("Counter::increment()");
            check_eq(eng->execute("Counter::getCount()").as<int>(), 1);
            
            // Increment multiple times
            eng->execute("Counter::increment()");
            eng->execute("Counter::increment()");
            check_eq(eng->execute("Counter::getCount()").as<int>(), 3);
            
            // Reset and check
            eng->execute("Counter::reset()");
            check_eq(eng->execute("Counter::getCount()").as<int>(), 0);
        });
        
        test("static_method_with_inheritance", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                class Base {
                    static int base_value = 10;

                    static function getBaseValue() -> int {
                        return base_value;
                    }
                }

                class Derived : Base {
                    static int derived_value = 20;

                    static function getDerivedValue() -> int {
                        return derived_value;
                    }

                    static function getTotalValue() -> int {
                        // Static members follow C++ semantics: not inherited, must use explicit qualification
                        return Base::base_value + derived_value;
                    }
                }
            )");

            // Test base class static method
            check_eq(eng->execute("Base::getBaseValue()").as<int>(), 10);

            // Test derived class can access parent static fields via explicit qualification
            check_eq(eng->execute("Derived::getTotalValue()").as<int>(), 30);

            // Test derived class own static method
            check_eq(eng->execute("Derived::getDerivedValue()").as<int>(), 20);
        });
        
        test("static_method_void_return", [&]() {
            auto eng = make_engine();
            
            eng->execute(R"(
                class Logger {
                    static array<string> logs = [];
                    
                    static function log(string message) {
                        logs.push(message);
                    }
                    
                    static function clear() {
                        logs = [];
                    }
                    
                    static function getLogCount() -> int {
                        return logs.size();
                    }
                }
            )");
            
            // Initially no logs
            check_eq(eng->execute("Logger::getLogCount()").as<int>(), 0);
            
            // Add some logs
            eng->execute("Logger::log(\"First message\")");
            eng->execute("Logger::log(\"Second message\")");
            check_eq(eng->execute("Logger::getLogCount()").as<int>(), 2);
            
            // Clear logs
            eng->execute("Logger::clear()");
            check_eq(eng->execute("Logger::getLogCount()").as<int>(), 0);
        });
        
        test("static_method_errors", [&]() {
            auto eng = make_engine();
            
            eng->execute(R"(
                class TestClass {
                    static function staticMethod() -> string {
                        return "static";
                    }
                    
                    function instanceMethod() -> string {
                        return "instance";
                    }
                }
            )");
            
            // Try to access non-existent static method
            check_throws([&]() {
                eng->execute("TestClass::nonExistent()");
            });
            
            // Try to access instance method as static
            check_throws([&]() {
                eng->execute("TestClass::instanceMethod()");
            });
            
            // Try to use :: on non-class
            check_throws([&]() {
                eng->execute("auto x = 5; x::something()");
            });
        });
        
        test("static_method_with_complex_types", [&]() {
            auto eng = make_engine();
            
            eng->execute(R"(
                class ArrayUtils {
                    static function sum(array<int> arr) -> int {
                        auto total = 0;
                        for (auto i = 0; i < arr.size(); i = i + 1) {
                            total = total + arr[i];
                        }
                        return total;
                    }
                    
                    static function createRange(int start, int end) -> array<int> {
                        auto result = [];
                        for (auto i = start; i <= end; i = i + 1) {
                            result.push(i);
                        }
                        return result;
                    }
                }
            )");
            
            // Test array parameter
            check_eq(eng->execute("ArrayUtils::sum([1, 2, 3, 4, 5])").as<int>(), 15);
            
            // Test array return value: the declared array<int> return converts the dynamic
            // literal into the typed representation (a raw int buffer)
            auto result = eng->execute("ArrayUtils::createRange(1, 5)");
            check(result.is_array());
            auto arr = result.as<std::vector<jai::script_int>>();
            check_eq(arr.size(), (size_t)5);
            check_eq(arr[0], (jai::script_int)1);
            check_eq(arr[4], (jai::script_int)5);
        });
        
        test("static_method_mixed_with_instance", [&]() {
            auto eng = make_engine();
            
            eng->execute(R"(
                class Calculator {
                    static string mode = "standard";
                    int value = 0;
                    
                    Calculator(int initial) {
                        value = initial;
                    }
                    
                    static function setMode(string newMode) {
                        mode = newMode;
                    }
                    
                    static function getMode() -> string {
                        return mode;
                    }
                    
                    function add(int x) -> int {
                        value = value + x;
                        return value;
                    }
                    
                    function getValue() -> int {
                        return value;
                    }
                }
            )");
            
            // Test static methods
            check_eq(eng->execute("Calculator::getMode()").as<std::string>(), "standard");
            eng->execute("Calculator::setMode(\"scientific\")");
            check_eq(eng->execute("Calculator::getMode()").as<std::string>(), "scientific");
            
            // Test instance methods
            eng->execute("auto calc = Calculator(10); calc");
            check_eq(eng->execute("calc.getValue()").as<int>(), 10);
            check_eq(eng->execute("calc.add(5)").as<int>(), 15);

            // Static methods don't affect instances and vice versa
            check_eq(eng->execute("Calculator::getMode()").as<std::string>(), "scientific");
        });

        test("static_method_overloads_dispatch_by_arity", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                class Fmt {
                    static function tag(string s) -> string { return "[" + s + "]"; }
                    static function tag(string s, string wrap) -> string { return wrap + s + wrap; }
                }
            )");

            check_eq(eng->execute("Fmt::tag(\"x\")").as<std::string>(), "[x]");
            check_eq(eng->execute("Fmt::tag(\"x\", \"*\")").as<std::string>(), "*x*");
        });

        // a static method runs in the caller's loop: overloads, recursion, a bare static and a sibling static, a reference
        // parameter, a throw caught by the caller, a call through a saved function value, from a loop and from a method
        test("static_methods_called_from_loops", [&]() {
            auto eng = make_engine();
            eng->execute(R"(
                class K {
                    static int count = 0;
                    static int pick(int a) { return 1; }
                    static int pick(string a) { return 2; }
                    static int fact(int n) { if (n <= 1) { return 1; } return n * K::fact(n - 1); }
                    static int bump() { count += 1; return twice(count); }
                    static int twice(int x) { return x * 2; }
                    static void inc(int& r) { r += 10; }
                    static int fail(int x) { if (x > 2) { throw "too big"; } return x; }
                    int via_method(int n) { int s = 0; for (int i = 0; i < n; i++) { s += K::twice(i); } return s; }
                }
            )");
            check_eq((int64_t)1212, eng->execute("int s = 0; for (int i = 0; i < 2; i++) { s = s * 100 + K::pick(i) * 10 + K::pick(\"x\"); } s").as_int(), "overloads by argument type");
            check_eq((int64_t)120, eng->execute("int f = 0; for (int i = 0; i < 3; i++) { f = K::fact(5); } f").as_int(), "recursion");
            check_eq((int64_t)6, eng->execute("int b = 0; for (int i = 0; i < 3; i++) { b = K::bump(); } b").as_int(), "a bare static and a sibling static");
            check_eq((int64_t)30, eng->execute("int v = 0; for (int i = 0; i < 3; i++) { K::inc(v); } v").as_int(), "a reference parameter");
            check_eq(std::string("too big"), eng->execute("string m = \"\"; for (int i = 0; i < 4; i++) { try { K::fail(i); } catch (e) { m = e; } } m").as<std::string>(), "a throw");
            check_eq((int64_t)3, eng->execute("int ok = 0; for (int i = 0; i < 4; i++) { try { ok += K::fail(i); } catch (e) { ok += 100; } } ok - 100").as_int(), "the calls before the throw");
            check_eq((int64_t)8, eng->execute("var g = K::twice; int r = 0; for (int i = 0; i < 2; i++) { r = g(4); } r").as_int(), "a saved function value");
            check_eq((int64_t)12, eng->execute("K k = K(); k.via_method(4)").as_int(), "from a method");
        });
    }
};

}

FOUNDRY_REGISTER(jai::foundry::tests::static_method_tests)