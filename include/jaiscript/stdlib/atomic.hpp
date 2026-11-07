#pragma once

#include <jaiscript/core/engine.hpp>

namespace jai {
namespace stdlib {

	// atomic<int>, atomic<float> and atomic<bool>: std::atomic<int64_t>, std::atomic<double> and std::atomic<bool> bound as C++
	// spells them (source/implementation/stdlib/atomic.cpp), registered only by this call (never by register_all). A host binds an
	// atomic it owns into each engine by reference (engine::add_global_ref), so engines on any threads share it; every operation
	// is one atomic step, and the types are not copyable.
	void register_atomics(engine& eng_ref);
	inline void register_atomics(const std::shared_ptr<engine>& e) { register_atomics(*e); }
	inline void register_atomics(engine* e) { register_atomics(*e); }

} // namespace stdlib
} // namespace jai
