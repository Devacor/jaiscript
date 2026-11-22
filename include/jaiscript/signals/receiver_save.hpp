#pragma once

#ifndef __JAISCRIPT_SIGNALS_RECEIVER_SAVE_HPP__
#define __JAISCRIPT_SIGNALS_RECEIVER_SAVE_HPP__

#include <jaiscript/core/value.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jai {

class engine;
namespace serialization {
class archive_writer_core;
class archive_reader_core;
}

namespace detail {

// How a named receiver saves (signals/signal_serialization.hpp): its function by the name the program gives it, a method by
// its object and name, or source text an emit runs under its parameter names beside its saved captures
struct receiver_save {
	enum class kind_type : uint8_t { function, method, source };
	kind_type kind = kind_type::source;
	// What the receiver keeps of its engine is a callback's hold: the engine's teardown releases it with the receiver's function
	std::optional<held_value> function;     // function and method: the connected value, named when a save writes it
	std::string source;
	std::vector<std::string> parameter_names;
	std::string captures;                   // the value captures as one JSON object, empty for none
	std::unordered_map<std::string, held_value> capture_values;   // a loaded source receiver's captures: each emit runs over a copy of them
};

// One saved receiver as an archive writes and reads it, whatever the signal's signature
struct receiver_record {
	std::string function;
	uint32_t object = 0;
	std::string method;
	std::string source;
	std::vector<std::string> parameter_names;
	std::string captures;
};

// What a load connects for one record: a function value (a function or a bound method), or a source receiver's save
struct loaded_receiver {
	engine* eng = nullptr;
	std::optional<script_value> function;
	std::shared_ptr<const receiver_save> source;
};

// A signal's saved receivers, connected once the outermost read is whole so a method's object may follow the signal
struct deferred_receivers {
	std::shared_ptr<void> state;
	void (*arm)(void* a_state, const std::string& a_id, const loaded_receiver& a_loaded) = nullptr;
	engine* eng = nullptr;
	std::vector<std::pair<std::string, receiver_record>> records;
};

// A connected function's save, or null for a lambda, which stays runtime-only (signal_receivers.cpp)
std::shared_ptr<const receiver_save> function_receiver_save(const script_value& a_function);
// connect_source's save of a [value captures](parameters) { body } lambda; anything else throws the catalogued refusal
std::shared_ptr<const receiver_save> source_receiver_save(const script_value& a_lambda);
// The record a save writes; false when the program gives the function no name a load could find
bool receiver_record_of(const receiver_save& a_save, serialization::archive_writer_core& a_writer, receiver_record& a_out);
// A script's text connect and connect_source, under jai::permissions::source_receivers
void require_source_receivers(engine* a_engine);

} // namespace detail
} // namespace jai

#endif // __JAISCRIPT_SIGNALS_RECEIVER_SAVE_HPP__
