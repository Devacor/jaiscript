#pragma once

#ifndef __JAISCRIPT_CORE_ENGINE_REF_HPP__
#define __JAISCRIPT_CORE_ENGINE_REF_HPP__

#include <memory>

namespace jai {

class engine;

// The engine a host call works on, however the host holds it: engine&, engine*, std::shared_ptr<engine> (what
// engine::make() returns) or std::unique_ptr<engine>
class engine_ref {
public:
	engine_ref(engine& a_engine) noexcept : target_(&a_engine) {}
	engine_ref(engine* a_engine) noexcept : target_(a_engine) {}
	engine_ref(const std::shared_ptr<engine>& a_engine) noexcept : target_(a_engine.get()) {}
	engine_ref(const std::unique_ptr<engine>& a_engine) noexcept : target_(a_engine.get()) {}

	engine& operator*() const noexcept { return *target_; }
	engine* operator->() const noexcept { return target_; }
	engine* get() const noexcept { return target_; }
	operator engine&() const noexcept { return *target_; }

private:
	engine* target_;
};

} // namespace jai

#endif // __JAISCRIPT_CORE_ENGINE_REF_HPP__
