#pragma once

#ifndef __JAISCRIPT_SIGNALS_SIGNAL_SERIALIZATION_HPP__
#define __JAISCRIPT_SIGNALS_SIGNAL_SERIALIZATION_HPP__

#include <jaiscript/signals/signal.hpp>
// signal.hpp no longer pulls in the engine-coupled script glue; serialization needs a
// complete engine type (get_user_context<engine>) and the script-receiver execution path.
#include <jaiscript/signals/signal_impl.hpp>
#include <jaiscript/serialization/archive_impl.hpp>

#include <vector>
#include <map>
#include <string>

namespace jai {

// A serializable snapshot of a script receiver. Only script-based receivers persist (C++
// callbacks are runtime-only). Everything routes through the archive's format-agnostic
// ar(make_nvp(...)) path so signals round-trip in BOTH json and binary — raw begin_array /
// write_property_name framing is json-only and corrupts the binary stream.
struct serialized_receiver {
	std::vector<std::string> parameter_names;
	std::string script;
};

template<typename Archive>
void save(Archive& ar, const serialized_receiver& r) {
	ar(serialization::make_nvp("parameter_names", r.parameter_names));
	ar(serialization::make_nvp("script", r.script));
}

template<typename Archive>
void load(Archive& ar, serialized_receiver& r) {
	ar(serialization::make_nvp("parameter_names", r.parameter_names));
	ar(serialization::make_nvp("script", r.script));
}

// ADL hooks (named save/load so the archive's write_element/read_element find them). The format
// stores parameter_names (signal-level), observers (array), and owned_observers (id -> receiver
// map). A named connection lives in both observers_ and owned_connections_, so owned receivers
// are excluded from the observers list to avoid serializing them twice.
template<typename Archive, typename T, typename Sync>
void save(Archive& ar, const signal_emitter<T, Sync>& sig) {
	auto lock = sig.lock_state();
	std::vector<std::string> param_names = sig.parameter_names();
	ar(serialization::make_nvp("parameter_names", param_names));

	const auto snapshot = [](const std::shared_ptr<receiver<T>>& a_receiver) {
		serialized_receiver result;
		if (a_receiver->parameter_names()) { result.parameter_names = *a_receiver->parameter_names(); }
		result.script = a_receiver->script();
		return result;
	};

	std::vector<serialized_receiver> observers;
	for (const auto& entry : sig.observers_) {
		auto obs = sig.live_target(entry);
		if (!obs || !obs->has_script()) { continue; }
		bool owned = false;
		for (const auto& [id, owned_obs] : sig.owned_connections_) {
			if (owned_obs == obs) { owned = true; break; }
		}
		if (owned) { continue; }
		observers.push_back(snapshot(obs));
	}
	ar(serialization::make_nvp("observers", observers));

	std::map<std::string, serialized_receiver> owned_observers;
	for (const auto& [id, obs] : sig.owned_connections_) {
		if (!obs->has_script() || !sig.owns(*obs)) { continue; }
		owned_observers[id] = snapshot(obs);
	}
	ar(serialization::make_nvp("owned_observers", owned_observers));
}

// A load replaces the script-text receivers the signal holds (the ones a save writes) with the saved ones, so loading
// into a live object never doubles them; C++ callbacks and script function receivers stay connected.
template<typename Archive, typename T, typename Sync>
void load(Archive& ar, signal_emitter<T, Sync>& sig) {
	engine* eng = ar.template get_user_context<engine>();

	std::vector<std::string> param_names;
	ar(serialization::make_nvp("parameter_names", param_names));
	std::vector<serialized_receiver> observers;
	ar(serialization::make_nvp("observers", observers));
	std::map<std::string, serialized_receiver> owned_observers;
	ar(serialization::make_nvp("owned_observers", owned_observers));

	auto lock = sig.lock_state();
	sig.parameter_names(param_names);

	std::vector<std::shared_ptr<receiver<T>>> replaced;
	for (const auto& entry : sig.observers_) {
		if (auto obs = sig.live_target(entry); obs && obs->has_script()) {
			replaced.push_back(std::move(obs));
		}
	}
	for (const auto& obs : replaced) {
		sig.detach(obs);
	}

	auto rebuild = [&](const serialized_receiver& r) {
		std::shared_ptr<std::vector<std::string>> names;
		if (!r.parameter_names.empty()) {
			names = std::make_shared<std::vector<std::string>>(r.parameter_names);
		}
		return receiver<T>::make(r.script, eng, names);
	};

	for (const auto& r : observers) {
		auto recv = rebuild(r);
		sig.loaded_observers_.push_back(recv);
		sig.attach(std::move(recv), false);
	}

	for (const auto& [id, r] : owned_observers) {
		replaced.push_back(std::exchange(sig.owned_connections_[id], rebuild(r)));
		sig.attach(sig.owned_connections_[id], false);
	}

	if (eng) {
		sig.script_engine(eng);
	}
}

} // namespace jai

#endif // __JAISCRIPT_SIGNALS_SIGNAL_SERIALIZATION_HPP__
