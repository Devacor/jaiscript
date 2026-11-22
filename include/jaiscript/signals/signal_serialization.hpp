#pragma once

#ifndef __JAISCRIPT_SIGNALS_SIGNAL_SERIALIZATION_HPP__
#define __JAISCRIPT_SIGNALS_SIGNAL_SERIALIZATION_HPP__

#include <jaiscript/signals/signal.hpp>
// signal.hpp no longer pulls in the engine-coupled script glue; serialization needs a
// complete engine type (get_user_context<engine>) and the script-receiver execution path.
#include <jaiscript/signals/signal_impl.hpp>
#include <jaiscript/signals/receiver_save.hpp>
#include <jaiscript/serialization/archive_impl.hpp>

#include <vector>
#include <map>
#include <string>

namespace jai {

namespace detail {

// A method receiver's object, as the archive's other references name one: {"$ref": N}
struct receiver_object_ref {
	uint32_t id = 0;
};

template<typename Archive>
void save(Archive& ar, const receiver_object_ref& r) {
	ar(serialization::make_nvp("$ref", r.id));
}

template<typename Archive>
void load(Archive& ar, receiver_object_ref& r) {
	ar(serialization::make_nvp("$ref", r.id));
}

// One kind's fields, each written only when it holds something; a load reads the ones present
template<typename Archive>
void save(Archive& ar, const receiver_record& r) {
	if (!r.function.empty()) {
		ar(serialization::make_nvp("function", r.function));
	} else if (!r.method.empty()) {
		ar(serialization::make_nvp("object", receiver_object_ref{ r.object }));
		ar(serialization::make_nvp("method", r.method));
	} else {
		ar(serialization::make_nvp("source", r.source));
		if (!r.parameter_names.empty()) { ar(serialization::make_nvp("parameter_names", r.parameter_names)); }
		if (!r.captures.empty()) { ar(serialization::make_nvp("captures", r.captures)); }
	}
}

template<typename Archive>
void load(Archive& ar, receiver_record& r) {
	if (ar.has_property("function")) { ar(serialization::make_nvp("function", r.function)); }
	if (ar.has_property("object")) {
		receiver_object_ref object;
		ar(serialization::make_nvp("object", object));
		r.object = object.id;
	}
	if (ar.has_property("method")) { ar(serialization::make_nvp("method", r.method)); }
	if (ar.has_property("source")) { ar(serialization::make_nvp("source", r.source)); }
	if (ar.has_property("parameter_names")) { ar(serialization::make_nvp("parameter_names", r.parameter_names)); }
	if (ar.has_property("captures")) { ar(serialization::make_nvp("captures", r.captures)); }
}

} // namespace detail

// A save writes the named receivers that save, by id, under one "receivers" field present only when one does
template<typename T, typename Sync>
template<typename Archive>
void detail::signal_state<T, Sync>::save_receivers(Archive& ar) const {
	auto lock = lock_state();
	std::map<std::string, receiver_record> records;
	for (const auto& [id, obs] : owned_connections_) {
		if (!owns(*obs)) { continue; }
		receiver_record record;
		if (obs->save_form_) {
			if (!receiver_record_of(*obs->save_form_, ar, record)) { continue; }
		} else if (obs->has_script()) {
			record.source = obs->script();
			if (obs->parameter_names()) { record.parameter_names = *obs->parameter_names(); }
		} else {
			continue;
		}
		records.emplace(id, std::move(record));
	}
	if (!records.empty()) {
		ar(serialization::make_nvp("receivers", records));
	}
}

// A load replaces the receivers a save writes with the saved ones, so loading into a live object never doubles them; the
// rest (C++ callbacks, lambdas, unnamed receivers) stay connected. The saved ones connect when the outermost read completes.
template<typename T, typename Sync>
template<typename Archive>
void detail::signal_state<T, Sync>::load_receivers(Archive& ar, const std::shared_ptr<signal_state>& a_self) {
	engine* eng = ar.template get_user_context<engine>();
	deferred_receivers job;
	if (ar.has_property("receivers")) {
		std::map<std::string, receiver_record> records;
		ar(serialization::make_nvp("receivers", records));
		job.records.assign(std::make_move_iterator(records.begin()), std::make_move_iterator(records.end()));
	}

	signal_state& self = *a_self;
	{
		auto lock = self.lock_state();
		std::vector<shared_receiver_type> replaced;
		for (const auto& [id, obs] : self.owned_connections_) {
			if (self.owns(*obs) && saves(*obs)) {
				replaced.push_back(obs);
			}
		}
		for (const auto& obs : replaced) {
			self.detach(obs);
		}
	}
	if (eng) {
		self.script_engine(eng);
	}
	if (!job.records.empty()) {
		job.state = a_self;
		job.arm = &signal_state::arm_loaded;
		job.eng = eng;
		ar.defer_receivers(std::move(job));
	}
}

template<typename T, typename Sync>
void detail::signal_state<T, Sync>::arm_loaded(void* a_state, const std::string& a_id, const detail::loaded_receiver& a_loaded) {
	signal_state& self = *static_cast<signal_state*>(a_state);
	if (a_loaded.function) {
		self.connect(a_id, *a_loaded.function, false);
		return;
	}
	auto names = a_loaded.source->parameter_names.empty() ? nullptr : std::make_shared<std::vector<std::string>>(a_loaded.source->parameter_names);
	auto recv = receiver<T>::make(a_loaded.source->source, a_loaded.eng, names);
	recv->save_form_ = a_loaded.source;
	self.connect_named(a_id, std::move(recv), false);
}

// ADL hooks (named save/load so the archive's write_element/read_element find them).
template<typename Archive, typename T, typename Sync>
void save(Archive& ar, const signal_emitter<T, Sync>& sig) {
	detail::signal_state_access::state(sig)->save_receivers(ar);
}

template<typename Archive, typename T, typename Sync>
void load(Archive& ar, signal_emitter<T, Sync>& sig) {
	detail::signal_state<T, Sync>::load_receivers(ar, detail::signal_state_access::state(sig));
}

} // namespace jai

#endif // __JAISCRIPT_SIGNALS_SIGNAL_SERIALIZATION_HPP__
