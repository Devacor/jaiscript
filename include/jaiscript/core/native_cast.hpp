// Part of the engine's closure, which includes it in its order: a translation unit that names it first is sent there
#if !defined(__JAISCRIPT_CORE_ENGINE_HPP__)
#include "engine.hpp"
#elif !defined(__JAISCRIPT_CORE_NATIVE_CAST_HPP__)
#define __JAISCRIPT_CORE_NATIVE_CAST_HPP__

#include "value.hpp"

namespace jai::detail {

// A holder's class tag describes the address before type erasure. Never cast that
// void* directly to a different C++ base: the base may have a nonzero offset.
// Callers with a cached target id keep exact-type conversions lookup-free.
checked_result<void*> checked_native_pointer(engine* eng, uint64_t source_id, const std::type_info& target,
	uint64_t target_id, void* pointer);

checked_result<std::shared_ptr<void>> checked_native_object(engine* eng, const script_value::object_holder& holder,
	const std::type_info& target, uint64_t target_id = UINT64_MAX);

// Only public C++ binding adapters use this; runtime helpers propagate error_value().
[[noreturn]] void throw_native_cast_error(engine* eng, error_propagator error);

template<typename T>
T native_cast_or_throw(checked_result<T> result, engine* eng) {
	if (!result) {
		throw_native_cast_error(eng, result.error_value());
	}
	return std::move(result.value());
}

}

#endif // __JAISCRIPT_CORE_NATIVE_CAST_HPP__
