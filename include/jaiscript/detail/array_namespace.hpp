#pragma once
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/core/script_namespace.hpp>

namespace jai::detail {
	inline void register_array_namespace(engine& a_engine) {
		auto& ns = engine_access::script_namespaces(a_engine)[engine_access::symbolize(a_engine, "array")];
		if (!ns) { ns = std::make_shared<script_namespace_data>(); }
		struct Transform { const char* name; uint8_t minimum; uint8_t maximum; };
		for (const auto transform : {Transform{"append", 2, 2}, Transform{"concat", 2, 2}, Transform{"slice", 1, 3},
			Transform{"filter", 2, 2}, Transform{"map", 2, 2}, Transform{"sort", 1, 2}, Transform{"reverse", 1, 1},
			Transform{"resize", 2, 3}, Transform{"fill", 2, 2}}) {
			const uint64_t member = engine_access::symbolize(a_engine, transform.name);
			ns->signatures[member] = {transform.minimum, transform.maximum, {script_value_type::jai_array_type}, script_value_type::jai_array_type, std::string_view(transform.name) != "map", {}, true, true};
			ns->variables[member] = script_value::make_function([engine = &a_engine, member, transform](const std::vector<script_value>& a_args) -> checked_result<script_value> {
				if (a_args.size() < transform.minimum || a_args.size() > transform.maximum) {
					return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "Incorrect argument count for array namespace transform");
				}
				if (!a_args[0].is_array()) { return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "array namespace transforms require an array"); }
				script_value input = a_args[0].deref();
				std::vector<script_value> arguments;
				arguments.reserve(a_args.size() - 1);
				for (size_t i = 1; i < a_args.size(); ++i) { arguments.push_back(a_args[i].deref()); }
				return engine_access::get_execution_backend(*engine)->array_transform(member, input, arguments);
			}, &a_engine);
		}
	}

	// map::filter(m, predicate) returns the kept entries as a new map; m.filter(predicate) keeps them in m
	inline void register_map_namespace(engine& a_engine) {
		auto& ns = engine_access::script_namespaces(a_engine)[engine_access::symbolize(a_engine, "map")];
		if (!ns) { ns = std::make_shared<script_namespace_data>(); }
		const uint64_t member = engine_access::symbolize(a_engine, "filter");
		ns->signatures[member] = {2, 2, {script_value_type::jai_map_type}, script_value_type::jai_map_type, true, {}, true, true};
		ns->variables[member] = script_value::make_function([engine = &a_engine, member](const std::vector<script_value>& a_args) -> checked_result<script_value> {
			if (a_args.size() != 2) {
				return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "Incorrect argument count for map namespace transform");
			}
			if (!a_args[0].is_map()) { return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "map namespace transforms require a map"); }
			script_value input = a_args[0].deref();
			std::vector<script_value> arguments{a_args[1].deref()};
			return engine_access::get_execution_backend(*engine)->map_transform(member, input, arguments);
		}, &a_engine);
	}
}
