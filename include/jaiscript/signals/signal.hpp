#pragma once

// Convenience umbrella: class declarations + script-callback execution glue.
//
// Include this in .cpp files and in headers that are NOT on hot compile paths.
//
// For headers included by many translation units (e.g. component.h, render.h)
// where the 1.8s parse cost of pulling in core/engine.hpp matters, include
// <jaiscript/signals/signal_decl.hpp> instead and let the .cpp files include
// this umbrella (or signal_impl.hpp directly) to get the call_script definitions.

#include <jaiscript/signals/signal_decl.hpp>
#include <jaiscript/signals/signal_impl.hpp>
