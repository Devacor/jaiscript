#pragma once

#include <jaiscript/testing/foundry.hpp>
#include <memory>

namespace jai::foundry::tests {
    std::unique_ptr<suite> create_function_tests();
}