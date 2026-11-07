#pragma once

#ifndef __JAISCRIPT_SIGNALS_SIGNAL_IMPL_HPP__
#define __JAISCRIPT_SIGNALS_SIGNAL_IMPL_HPP__

#include <jaiscript/signals/signal.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/value.hpp>

namespace jai {

// ============================================================================
// receiver<T> script execution implementations
// ============================================================================

namespace detail {

template<typename... Args>
inline instance_variables build_script_locals(
	engine* eng,
	const std::shared_ptr<std::vector<std::string>>& param_names,
	Args&&... args
) {
	instance_variables locals;

	std::vector<script_value> values;
	values.reserve(sizeof...(Args));
	(values.push_back(eng->make_value(std::forward<Args>(args))), ...);

	for (size_t i = 0; i < values.size(); ++i) {
		std::string name = (param_names && i < param_names->size())
			? (*param_names)[i]
			: "arg_" + std::to_string(i);
		locals[name] = std::move(values[i]);
	}

	return locals;
}

} // namespace detail

template<typename T, typename Sync>
typename signal_emitter<T, Sync>::shared_receiver_type signal_emitter<T, Sync>::connect(const std::string& id, const script_value& fn_callback) {
	// bound before it is published: a synchronized emitter's other threads may emit to it once connect_named returns
	auto recv = receiver<T>::make(convert_script_function<T>(fn_callback));
	recv->bind_engine(fn_callback.payload_engine(), engine_lifetime_token(fn_callback.payload_engine()));
	return connect_named(id, std::move(recv), false);
}

template<typename T, typename Sync>
typename signal_emitter<T, Sync>::shared_receiver_type signal_emitter<T, Sync>::connect_immediate(const std::string& id, const script_value& fn_callback) {
	auto recv = receiver<T>::make(convert_script_function<T>(fn_callback));
	recv->bind_engine(fn_callback.payload_engine(), engine_lifetime_token(fn_callback.payload_engine()));
	return connect_named(id, std::move(recv), true);
}

// A text receiver's failure throws out of the emit as a direct execute() of its text would (a function receiver's throws as
// its converted std::function does); the receivers after it do not run
template<typename T>
template<typename... Args>
void receiver<T>::call_script(Args&&... args) {
	if (script_engine_ && !script_callback_.empty()) {
		auto locals = detail::build_script_locals(script_engine_, ordered_parameter_names_, std::forward<Args>(args)...);
		script_engine_->execute(script_callback_, locals);
	}
}

template<typename T>
template<typename... Args>
bool receiver<T>::call_script_predicate(Args&&... args) {
	if (script_engine_ && !script_callback_.empty()) {
		auto locals = detail::build_script_locals(script_engine_, ordered_parameter_names_, std::forward<Args>(args)...);
		return script_engine_->execute(script_callback_, locals).template as<bool>();
	}
	return false;
}

template<typename T>
void receiver<T>::call_script() {
	if (script_engine_ && !script_callback_.empty()) {
		script_engine_->execute(script_callback_);
	}
}

template<typename T>
bool receiver<T>::call_script_predicate() {
	if (script_engine_ && !script_callback_.empty()) {
		return script_engine_->execute(script_callback_).template as<bool>();
	}
	return false;
}

} // namespace jai

#endif // __JAISCRIPT_SIGNALS_SIGNAL_IMPL_HPP__
