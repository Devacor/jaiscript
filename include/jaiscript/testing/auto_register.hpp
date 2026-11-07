#pragma once

#include "foundry.hpp"
#include <vector>
#include <memory>
#include <functional>
#include <string>

namespace jai::foundry {

// Singleton registry for test suites
class test_registry {
public:
    using suite_factory = std::function<std::unique_ptr<suite>()>;
    
    static test_registry& instance() {
        static test_registry registry;
        return registry;
    }
    
    void register_suite(const std::string& name, suite_factory factory) {
        suites_.emplace_back(name, factory);
    }
    
    std::vector<std::unique_ptr<suite>> create_all_suites() {
        std::vector<std::unique_ptr<suite>> suites;
        for (const auto& [name, factory] : suites_) {
            suites.push_back(factory());
        }
        return suites;
    }
    
private:
    test_registry() = default;
    std::vector<std::pair<std::string, suite_factory>> suites_;
};

// RAII helper for auto-registration
template<typename TestSuite>
class suite_registrar {
public:
    explicit suite_registrar(const std::string& name) {
        test_registry::instance().register_suite(name, []() {
            return std::make_unique<TestSuite>();
        });
    }
};

} // namespace jai::foundry

// Helper macros for concatenation
#define FOUNDRY_CONCAT_IMPL(a, b) a##b
#define FOUNDRY_CONCAT(a, b) FOUNDRY_CONCAT_IMPL(a, b)

// Registers the suite with the main runner
#define FOUNDRY_REGISTER(TestClass)                                       \
namespace {                                                                \
    static ::jai::foundry::suite_registrar<TestClass> FOUNDRY_CONCAT(_registrar_, __LINE__)(#TestClass); \
}