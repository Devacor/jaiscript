#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class static_field_tests : public suite {
public:
    static_field_tests() : suite("Static Field Tests") {}
    
    void forge_tests() override {
        test("basic_static_field", [&]() {
            auto eng = make_engine();
            
            eng->execute(R"(
                class Math {
                    static float PI = 3.14159;
                    static int MAX_VALUE = 100;
                }
            )");
            
            // Access static fields via class name
            check_eq(eng->execute("Math::PI").as<float>(), 3.14159f);
            check_eq(eng->execute("Math::MAX_VALUE").as<int>(), 100);
        });
        
        test("static_field_modification", [&]() {
            auto eng = make_engine();
            
            eng->execute(R"(
                class Counter {
                    static int count = 0;
                    
                    Counter() {
                        count = count + 1;
                    }
                }
            )");
            
            check_eq(eng->execute("Counter::count").as<int>(), 0);
            
            // Create instances and verify static field is shared
            eng->execute("auto c1 = Counter();");
            check_eq(eng->execute("Counter::count").as<int>(), 1);
            
            eng->execute("auto c2 = Counter();");
            check_eq(eng->execute("Counter::count").as<int>(), 2);
            
            // Modify via class name
            eng->execute("Counter::count = 10;");
            check_eq(eng->execute("Counter::count").as<int>(), 10);
        });
        
        test("implicit_static_access_in_methods", [&]() {
            auto eng = make_engine();
            
            eng->execute(R"(
                class Settings {
                    static string DEFAULT_NAME = "Unnamed";
                    static int MAX_ITEMS = 50;
                    
                    string name;
                    
                    Settings() {
                        // Access static field without ClassName::
                        name = DEFAULT_NAME;
                    }
                    
                    function getMax() -> int {
                        // Access static field without ClassName::
                        return MAX_ITEMS;
                    }
                    
                    function setDefaults() {
                        // Modify static fields without ClassName::
                        DEFAULT_NAME = "Default";
                        MAX_ITEMS = 100;
                    }
                }
            )");
            
            auto obj = eng->execute("Settings()");
            check_eq(eng->execute("auto s = Settings(); s.name").as<std::string>(), "Unnamed");
            check_eq(eng->execute("s.getMax()").as<int>(), 50);
            
            // Modify static fields through method
            eng->execute("s.setDefaults();");
            check_eq(eng->execute("Settings::DEFAULT_NAME").as<std::string>(), "Default");
            check_eq(eng->execute("Settings::MAX_ITEMS").as<int>(), 100);
            
            // New instance should see updated static values
            check_eq(eng->execute("auto s2 = Settings(); s2.name").as<std::string>(), "Default");
        });
        
        test("static_fields_with_inheritance", [&]() {
            auto eng = make_engine();

            eng->execute(R"(
                class Base {
                    static int base_static = 10;
                }

                class Derived : Base {
                    static int derived_static = 20;

                    function getBaseStatic() -> int {
                        // The declaring class's qualification names an inherited static
                        return Base::base_static;
                    }
                }
            )");

            check_eq(eng->execute("Base::base_static").as<int>(), 10);
            check_eq(eng->execute("Derived::derived_static").as<int>(), 20);

            // Access parent's static through derived class method using explicit qualification
            check_eq(eng->execute("auto d = Derived(); d.getBaseStatic()").as<int>(), 10);

            // Modify parent's static
            eng->execute("Base::base_static = 30;");
            check_eq(eng->execute("d.getBaseStatic()").as<int>(), 30);
        });
        
        test("static_field_errors", [&]() {
            auto eng = make_engine();
            
            eng->execute(R"(
                class TestClass {
                    static int static_field = 42;
                    int instance_field = 10;
                }
            )");
            
            // Try to access non-existent static field
            check_throws([&]() {
                eng->execute("TestClass::non_existent");
            });
            
            // Try to access instance field as static
            check_throws([&]() {
                eng->execute("TestClass::instance_field");
            });
            
            // Try to use :: on non-class
            check_throws([&]() {
                eng->execute("auto x = 5; x::something");
            });
        });
        
        test("static_fields_with_complex_types", [&]() {
            auto eng = make_engine();
            
            eng->execute(R"(
                class Config {
                    static array<string> VALID_OPTIONS = ["option1", "option2", "option3"];
                    static map<string, int> DEFAULTS = {"width": 800, "height": 600};
                    
                    function isValidOption(string opt) -> bool {
                        // Access static array without ClassName::
                        for (auto i = 0; i < VALID_OPTIONS.size(); i = i + 1) {
                            if (VALID_OPTIONS[i] == opt) {
                                return true;
                            }
                        }
                        return false;
                    }
                    
                    function getDefault(string key) -> int {
                        // Access static map without ClassName::
                        return DEFAULTS[key];
                    }
                }
            )");
            
            // Access static array
            check_eq(eng->execute("Config::VALID_OPTIONS.size()").as<int>(), 3);
            check_eq(eng->execute("Config::VALID_OPTIONS[0]").as<std::string>(), "option1");
            
            // Access static map
            check_eq(eng->execute("Config::DEFAULTS[\"width\"]").as<int>(), 800);
            
            // Use methods that access static fields
            check(eng->execute("auto cfg = Config(); cfg.isValidOption(\"option2\")").as<bool>());
            check(!eng->execute("cfg.isValidOption(\"invalid\")").as<bool>());
            check_eq(eng->execute("cfg.getDefault(\"height\")").as<int>(), 600);
            
            // Modify static collections
            eng->execute("Config::VALID_OPTIONS.push(\"option4\");");
            check_eq(eng->execute("Config::VALID_OPTIONS.size()").as<int>(), 4);
            
            eng->execute("Config::DEFAULTS[\"depth\"] = 32;");
            check_eq(eng->execute("Config::DEFAULTS[\"depth\"]").as<int>(), 32);
        });
        
        
        test("static_fields_shared_across_instances", [&]() {
            auto eng = make_engine();
            
            eng->execute(R"(
                class SharedStatic {
                    static array<string> log = [];
                    string name;
                    
                    SharedStatic(string n) {
                        name = n;
                        // All instances share the same static log
                        log.push("Created: " + name);
                    }
                    
                    function getLogSize() -> int {
                        return log.size();
                    }
                }
            )");
            
            eng->execute("auto s1 = SharedStatic(\"First\");");
            check_eq(eng->execute("s1.getLogSize()").as<int>(), 1);
            
            eng->execute("auto s2 = SharedStatic(\"Second\");");
            check_eq(eng->execute("s1.getLogSize()").as<int>(), 2);
            check_eq(eng->execute("s2.getLogSize()").as<int>(), 2);
            
            // Both instances see the same log
            check_eq(eng->execute("SharedStatic::log[0]").as<std::string>(), "Created: First");
            check_eq(eng->execute("SharedStatic::log[1]").as<std::string>(), "Created: Second");
        });
    }
};

}

FOUNDRY_REGISTER(jai::foundry::tests::static_field_tests)