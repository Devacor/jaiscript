// How a named signal receiver saves and what a load connects (signals/receiver_save.hpp): a function by its name in the program,
// a method by its object's archive id, a lambda connect_source accepted by its body as source
#include <jaiscript/signals/receiver_save.hpp>
#include <jaiscript/core/engine_impl.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/engine_access.hpp>
#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/stdlib/json.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace jai::detail {

namespace {

constexpr const char* k_source_form = "connect_source takes a lambda written [captures](parameters) { body } that captures values";

std::string symbol_text(engine& a_engine, uint64_t a_id) {
	return std::string(engine_access::get_symbolizer(a_engine)->get_string(a_id));
}

bool is_program_name(std::string_view a_name) {
	if (a_name.empty()) { return false; }
	bool word_start = true;
	for (size_t i = 0; i < a_name.size(); ++i) {
		const char c = a_name[i];
		if (c == ':' && i + 1 < a_name.size() && a_name[i + 1] == ':' && !word_start) {
			++i;
			word_start = true;
			continue;
		}
		const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
		if (!letter && (word_start || c < '0' || c > '9')) { return false; }
		word_start = false;
	}
	return !word_start;
}

// A function the program declared at program scope: a declaration, an overload set, a script class's static method
bool is_program_function(const script_value& a_value) {
	if (!a_value.is_function()) { return false; }
	const script_function& function = a_value.deref().as_function();
	if (const auto* thunk = function.target<script_callable_thunk>()) {
		const script_callable& payload = thunk->payload;
		return (payload.kind == script_callable::kind_type::function && payload.fn && payload.fn->declared) ||
			payload.kind == script_callable::kind_type::overload_set || payload.kind == script_callable::kind_type::static_method;
	}
	return function.target<script_static_method_dispatch>() != nullptr;
}

// The name a load finds the function by, or empty for one the program gives none (a block's function, a host callable)
std::string program_function_name(engine& a_engine, const script_value& a_function) {
	const script_function& function = a_function.deref().as_function();
	if (const auto* dispatch = function.target<script_static_method_dispatch>()) {
		return dispatch->cls && dispatch->cls->is_script_class() ? dispatch->cls->get_name() + "::" + symbol_text(a_engine, dispatch->name_id) : std::string();
	}
	if (const auto* thunk = function.target<script_callable_thunk>()) {
		const script_callable& payload = thunk->payload;
		std::string result;
		if (payload.kind == script_callable::kind_type::static_method) {
			if (payload.cls() && payload.cls()->is_script_class() && payload.ast()) { result = payload.cls()->get_name() + "::" + std::string(payload.ast()->name); }
		} else if (payload.kind == script_callable::kind_type::overload_set && payload.overloads()) {
			result = symbol_text(a_engine, payload.overloads()->name_id);
		} else if (payload.kind == script_callable::kind_type::function && payload.fn && payload.fn->declared && !payload.fn->block_defined) {
			result = std::string(payload.fn->name);
		}
		return !result.empty() && (result.find("::") != std::string::npos || a_engine.has_function(result)) ? result : std::string();
	}
	for (const auto& [space_id, space] : engine_access::script_namespaces(a_engine)) {
		if (!space || !space->scope) { continue; }
		for (const auto& [member_id, declarations] : space->functions) {
			const script_value* cell = space->scope->get_own_value_ptr(member_id);
			if (cell && cell->is_function() && &cell->deref().as_function() == &function) {
				return symbol_text(a_engine, space_id) + "::" + symbol_text(a_engine, member_id);
			}
		}
	}
	return {};
}

// The connected function a load finds by the saved name, or null for a name the program does not declare
std::optional<script_value> find_program_function(engine& a_engine, const std::string& a_name) {
	if (!is_program_name(a_name)) { return std::nullopt; }
	const size_t split = a_name.rfind("::");
	if (split == std::string::npos) {
		if (!a_engine.has_function(a_name)) { return std::nullopt; }
		script_value value = a_engine.get_variable(a_name).value();
		return is_program_function(value) ? std::optional<script_value>(std::move(value)) : std::nullopt;
	}
	const std::string scope = a_name.substr(0, split);
	const uint64_t member_id = engine_access::symbolize(a_engine, std::string_view(a_name).substr(split + 2));
	if (auto cls = engine_access::get_class_definition(a_engine, scope); cls && cls->is_script_class()) {
		script_value method = cls->get_static_method(member_id, false);
		return method.is_function() ? std::optional<script_value>(std::move(method)) : std::nullopt;
	}
	auto& spaces = engine_access::script_namespaces(a_engine);
	auto space = spaces.find(engine_access::symbolize(a_engine, scope));
	if (space == spaces.end() || !space->second || !space->second->scope || !space->second->functions.count(member_id)) { return std::nullopt; }
	const script_value* cell = space->second->scope->get_own_value_ptr(member_id);
	return cell && cell->is_function() ? std::optional<script_value>(cell->deref()) : std::nullopt;
}

// A bound method's object and the method it calls
bool bound_parts(const script_value& a_function, script_value& a_object, script_value& a_method) {
	const script_function& function = a_function.deref().as_function();
	if (const auto* bound = function.target<bound_method_fn>()) {
		a_object = bound->this_obj;
		a_method = bound->method;
		return true;
	}
	const auto* thunk = function.target<script_callable_thunk>();
	if (thunk && thunk->payload.kind == script_callable::kind_type::bound_method && thunk->payload.this_obj() && thunk->payload.bound_dispatch()) {
		a_object = *thunk->payload.this_obj();
		a_method = *thunk->payload.bound_dispatch();
		return true;
	}
	return false;
}

std::string bound_method_name(engine& a_engine, const script_value& a_method) {
	if (!a_method.is_function()) { return {}; }
	const script_function& function = a_method.deref().as_function();
	if (const auto* dispatch = function.target<script_method_dispatch>()) { return symbol_text(a_engine, dispatch->name_id); }
	if (const auto* dispatch = function.target<class_definition::cpp_method_dispatch>()) { return symbol_text(a_engine, dispatch->name_id); }
	return {};
}

// What keeps a capture from saving, or null for a value the archive writes back as it was: data, never code
const char* unsaveable_capture(const script_value& a_value) {
	std::vector<script_value> pending { a_value.deref() };
	while (!pending.empty()) {
		const script_value value = std::move(pending.back());
		pending.pop_back();
		if (value.is_function()) {
			return "a function";
		}
		if (value.is_array()) {
			const script_array* node = value.unchecked_array_node();
			if (node && node->kind() == script_array::kind_t::hetero) {
				for (const auto& element : node->values()) { pending.push_back(element.deref()); }
			}
		} else if (value.is_map()) {
			for (const auto& [key, element] : value.as_map()) { pending.push_back(element.deref()); }
		}
	}
	return nullptr;
}

// A lambda's value captures by name: a converted closure's cells, or the capture environment the lambda closes over
std::map<std::string, script_value> source_lambda_captures(engine& a_engine, const script_defined_function& a_lambda) {
	std::map<std::string, script_value> result;
	if (a_lambda.captures) {
		const auto& ids = *a_lambda.captures->ids;
		for (size_t i = 0; i < ids.size() && i < a_lambda.captures->cells.size(); ++i) {
			result.emplace(symbol_text(a_engine, ids[i]), a_lambda.captures->cells[i].deref());
		}
	} else if (a_lambda.closure_env) {
		for (const auto& [name, value] : a_lambda.closure_env->get_local_variables()) {
			result.emplace(std::string(name), value.deref());
		}
	}
	return result;
}

script_value captures_map(engine& a_engine, const std::map<std::string, script_value>& a_captures) {
	script_value result = script_value::make_map(engine_access::get_type_info_string(a_engine), nullptr, &a_engine);
	auto& entries = const_cast<script_map&>(result.as_map());
	for (const auto& [name, value] : a_captures) {
		entries.emplace(script_value(name, &a_engine), value);
	}
	return result;
}

} // namespace

std::shared_ptr<const receiver_save> function_receiver_save(const script_value& a_function) {
	const script_value& function = a_function.deref();
	if (!function.is_function()) { return nullptr; }
	receiver_save::kind_type kind = receiver_save::kind_type::function;
	const script_function& callable = function.as_function();
	if (const auto* thunk = callable.target<script_callable_thunk>()) {
		switch (thunk->payload.kind) {
			case script_callable::kind_type::function:
				if (!thunk->payload.fn || !thunk->payload.fn->declared) { return nullptr; }
				break;
			case script_callable::kind_type::overload_set:
			case script_callable::kind_type::static_method:
				break;
			case script_callable::kind_type::bound_method:
				kind = receiver_save::kind_type::method;
				break;
			default:
				return nullptr;
		}
	} else if (callable.target<bound_method_fn>()) {
		kind = receiver_save::kind_type::method;
	}
	auto result = std::make_shared<receiver_save>();
	result->kind = kind;
	if (engine* eng = function.payload_engine()) { result->function.emplace(engine_access::hold_callback(*eng, function)); }
	return result;
}

std::shared_ptr<const receiver_save> source_receiver_save(const script_value& a_lambda) {
	const script_value& function = a_lambda.deref();
	engine* eng = function.payload_engine();
	require_source_receivers(eng);
	const auto* thunk = function.is_function() ? function.as_function().target<script_callable_thunk>() : nullptr;
	const script_defined_function* lambda = thunk && thunk->payload.kind == script_callable::kind_type::function ? thunk->payload.fn.get() : nullptr;
	if (!eng || !lambda || lambda->declared || lambda->saved_source.empty()) {
		throw runtime_error(k_source_form);
	}
	auto result = std::make_shared<receiver_save>();
	result->kind = receiver_save::kind_type::source;
	result->source = std::string(lambda->saved_source);
	for (const auto& parameter : lambda->parameters()) {
		result->parameter_names.push_back(parameter.name);
	}
	const auto captures = source_lambda_captures(*eng, *lambda);
	for (const auto& [name, value] : captures) {
		if (const char* reason = unsaveable_capture(value)) {
			throw runtime_error("connect_source cannot save the capture '" + name + "': " + reason);
		}
	}
	if (!captures.empty()) {
		serialization::json_archive_writer writer(-1, eng);
		writer.write_value(captures_map(*eng, captures));
		result->captures = writer.take();
	}
	return result;
}

bool receiver_record_of(const receiver_save& a_save, serialization::archive_writer_core& a_writer, receiver_record& a_out) {
	engine* eng = a_writer.get_engine();
	switch (a_save.kind) {
		case receiver_save::kind_type::source:
			a_out.source = a_save.source;
			a_out.parameter_names = a_save.parameter_names;
			a_out.captures = a_save.captures;
			return true;
		case receiver_save::kind_type::function:
			if (eng && a_save.function && a_save.function->valid()) { a_out.function = program_function_name(*eng, *a_save.function); }
			return !a_out.function.empty();
		case receiver_save::kind_type::method: {
			script_value object = script_value::make_null(eng);
			script_value method = script_value::make_null(eng);
			if (!eng || !a_save.function || !a_save.function->valid() || !bound_parts(*a_save.function, object, method)) { return false; }
			a_out.method = bound_method_name(*eng, method);
			if (a_out.method.empty()) { return false; }
			a_out.object = a_writer.reference_object(object);
			return a_out.object != 0;
		}
	}
	return false;
}

void require_source_receivers(engine* a_engine) {
	if (a_engine) {
		if (auto allowed = a_engine->require_permission(permissions::source_receivers); !allowed) {
			throw runtime_error(a_engine->format_error(allowed));
		}
	}
}

namespace {

// The host object an archive id loaded, as script reaches it: its script layer when one owns it, else a fresh handle
script_value loaded_object_value(engine& a_engine, const std::shared_ptr<void>& a_object, std::type_index a_type) {
	if (auto layer = engine_access::script_layer_of(a_engine, a_object.get())) {
		return script_value::make_object(layer->get_class_definition()->get_name(), layer, &a_engine);
	}
	auto definition = engine_access::get_class_definition_by_type(a_engine, a_type);
	if (!definition) {
		return script_value::make_invalid(&a_engine);
	}
	auto instance = detail::make_native_instance(*definition, definition->get_name(), a_object, &a_engine);
	engine_access::note_script_layer(a_engine, a_object.get(), instance);
	return script_value::make_object(definition->get_name(), instance, &a_engine);
}

} // namespace

} // namespace jai::detail

namespace jai::serialization {

void archive_reader_core::defer_receivers(detail::deferred_receivers&& a_receivers) {
	if (!deferred_receivers_) { deferred_receivers_ = std::make_unique<std::vector<detail::deferred_receivers>>(); }
	deferred_receivers_->push_back(std::move(a_receivers));
	if (read_depth_ == 0) { arm_deferred_receivers(); }
}

void archive_reader_core::drop_deferred_receivers() noexcept {
	deferred_receivers_.reset();
}

// Each record connects, in id order, what the loading program and this archive hold under its names; one they do not hold
// fails the load naming it, unless the engine skips unknown receivers
void archive_reader_core::arm_deferred_receivers() {
	auto jobs = std::move(deferred_receivers_);
	for (auto& job : *jobs) {
		engine* eng = job.eng;
		const bool skip = eng && eng->skip_unknown_receivers();
		for (const auto& [id, record] : job.records) {
			std::string failure;
			detail::loaded_receiver loaded;
			loaded.eng = eng;
			if (!record.function.empty()) {
				if (auto function = eng ? detail::find_program_function(*eng, record.function) : std::nullopt) {
					loaded.function = std::move(*function);
				} else {
					failure = "Signal receiver '" + id + "' names no function '" + record.function + "' in the program";
				}
			} else if (!record.method.empty()) {
				const deserialized_entry* entry = nullptr;
				if (cpp_shared_ptrs_) {
					if (auto it = cpp_shared_ptrs_->find(record.object); it != cpp_shared_ptrs_->end()) { entry = &it->second; }
				}
				script_value object = entry && eng ? detail::loaded_object_value(*eng, entry->object, entry->type) : script_value::make_null(eng);
				auto instance = object.is_object() ? object.get_class_instance() : nullptr;
				if (!instance) {
					failure = "Signal receiver '" + id + "' names an object the archive does not hold ({\"$ref\": " + std::to_string(record.object) + "})";
				} else {
					script_value method = instance->get_class_definition()->get_method(detail::engine_access::symbolize(*eng, record.method), false);
					if (method.is_function()) {
						loaded.function = make_bound_method(object, method);
					} else {
						failure = "Signal receiver '" + id + "' names no method '" + record.method + "' on '" + instance->get_class_definition()->get_name() + "'";
					}
				}
			} else if (eng && !eng->is_enabled(permissions::source_receivers)) {
				failure = "Signal receiver '" + id + "' is source, and the engine's source receivers are disabled";
			} else {
				auto source = std::make_shared<detail::receiver_save>();
				source->source = record.source;
				source->parameter_names = record.parameter_names;
				source->captures = record.captures;
				if (!record.captures.empty() && eng) {
					json_archive_reader reader(record.captures, eng);
					const script_value captures = stdlib::load_archive_value(eng, reader.read_value(), true);
					if (captures.is_map()) {
						for (const auto& [name, value] : captures.as_map()) {
							if (name.is_string()) { source->capture_values.insert_or_assign(name.as_string(), detail::engine_access::hold_callback(*eng, value.deref())); }
						}
					}
				}
				loaded.source = std::move(source);
			}
			if (!failure.empty()) {
				if (skip) { continue; }
				throw serialization_error(failure);
			}
			job.arm(job.state.get(), id, loaded);
		}
	}
}

} // namespace jai::serialization
