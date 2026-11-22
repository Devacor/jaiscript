#pragma once

#ifndef __JAISCRIPT_SIGNALS_SIGNAL_IMPL_HPP__
#define __JAISCRIPT_SIGNALS_SIGNAL_IMPL_HPP__

#include <jaiscript/signals/signal.hpp>
#include <jaiscript/signals/receiver_save.hpp>
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
typename detail::signal_state<T, Sync>::shared_receiver_type detail::signal_state<T, Sync>::connect(const std::string& a_id, const script_value& a_function, bool a_joinsRunningEmits) {
	// bound before it is published: a synchronized emitter's other threads may emit to it once connect_named returns
	auto recv = receiver<T>::make(convert_script_function<T>(a_function));
	recv->bind_engine(a_function.payload_engine(), engine_lifetime_token(a_function.payload_engine()));
	recv->save_form_ = detail::function_receiver_save(a_function);
	return connect_named(a_id, std::move(recv), a_joinsRunningEmits);
}

template<typename T, typename Sync>
typename detail::signal_state<T, Sync>::shared_receiver_type detail::signal_state<T, Sync>::connect_source(const std::string& a_id, const script_value& a_lambda, bool a_joinsRunningEmits) {
	auto save = detail::source_receiver_save(a_lambda);
	auto recv = receiver<T>::make(convert_script_function<T>(a_lambda));
	recv->bind_engine(a_lambda.payload_engine(), engine_lifetime_token(a_lambda.payload_engine()));
	recv->save_form_ = std::move(save);
	return connect_named(a_id, std::move(recv), a_joinsRunningEmits);
}

// A text receiver's failure throws out of the emit as a direct execute() of its text would (a function receiver's throws as
// its converted std::function does); the receivers after it do not run
template<typename T>
template<typename... Args>
void receiver<T>::call_script(Args&&... args) {
	if (script_engine_ && !script_callback_.empty()) {
		auto locals = detail::build_script_locals(script_engine_, ordered_parameter_names_, std::forward<Args>(args)...);
		add_saved_captures(locals);
		script_engine_->execute(script_callback_, locals);
	}
}

template<typename T>
template<typename... Args>
bool receiver<T>::call_script_predicate(Args&&... args) {
	if (script_engine_ && !script_callback_.empty()) {
		auto locals = detail::build_script_locals(script_engine_, ordered_parameter_names_, std::forward<Args>(args)...);
		add_saved_captures(locals);
		return script_engine_->execute(script_callback_, locals).template as<bool>();
	}
	return false;
}

template<typename T>
void receiver<T>::call_script() {
	if (script_engine_ && !script_callback_.empty()) {
		instance_variables locals;
		add_saved_captures(locals);
		if (locals.empty()) {
			script_engine_->execute(script_callback_);
		} else {
			script_engine_->execute(script_callback_, locals);
		}
	}
}

template<typename T>
bool receiver<T>::call_script_predicate() {
	if (script_engine_ && !script_callback_.empty()) {
		instance_variables locals;
		add_saved_captures(locals);
		return (locals.empty() ? script_engine_->execute(script_callback_) : script_engine_->execute(script_callback_, locals)).template as<bool>();
	}
	return false;
}

// A loaded source receiver's captures start from their saved values on every emit
template<typename T>
void receiver<T>::add_saved_captures(instance_variables& a_locals) const {
	if (save_form_) {
		for (const auto& [name, value] : save_form_->capture_values) {
			a_locals.insert_or_assign(name, value.clone());
		}
	}
}

} // namespace jai

#endif // __JAISCRIPT_SIGNALS_SIGNAL_IMPL_HPP__
