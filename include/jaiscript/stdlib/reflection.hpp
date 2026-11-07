#pragma once

#ifndef JAISCRIPT_STDLIB_REFLECTION_HPP
#define JAISCRIPT_STDLIB_REFLECTION_HPP

// The reflect:: namespace (docs/site/guide/12-reflection.html). Every builtin is a LIVE read of the engine's
// registered classes: hot
// reload a class and the next call sees the new shape (recapture = re-ask). Results are
// ordinary value-semantic containers minted at call time. The namespace itself is RESERVED
// (is_reserved_namespace_name): these answers are engine truth and cannot be impersonated
// from script. Registered once per engine; both backends invoke the same std::function, so
// output and error text are byte-identical by construction.

#include <jaiscript/core/engine.hpp>
#include <memory>

namespace jai {
namespace stdlib {

void register_reflection_functions(engine& eng_ref);
inline void register_reflection_functions(const std::shared_ptr<engine>& eng) { register_reflection_functions(*eng); }
inline void register_reflection_functions(engine* eng) { register_reflection_functions(*eng); }

} // namespace stdlib
} // namespace jai

#endif // JAISCRIPT_STDLIB_REFLECTION_HPP
