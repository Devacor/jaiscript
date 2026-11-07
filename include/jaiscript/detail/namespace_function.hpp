#pragma once

#ifndef __JAISCRIPT_DETAIL_NAMESPACE_FUNCTION_HPP__
#define __JAISCRIPT_DETAIL_NAMESPACE_FUNCTION_HPP__

// A namespace function's name is a cell of its namespace's scope (C++ [basic.lookup.unqual]: a body finds its namespace's
// members by their bare names, the enclosing namespaces' next, the globals last), holding the value `ns::f` reads: the
// overload set's dispatcher. Shared VERBATIM by the vm and the interpreter.

#include <jaiscript/core/coroutine.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/core/overload_resolution.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/namespace_scope.hpp>
#include <memory>
#include <vector>

namespace jai::detail {

	// The host entries a namespace's code reads by bare name, defined into `layer` (made over the scope on the first one): the
	// namespace's and its enclosing ones', each unless a nearer namespace declares the name (in its scope or as a host entry)
	inline void define_namespace_host_entries(engine& eng, script_namespace_data& own, const std::shared_ptr<environment>& scope, std::shared_ptr<environment>& layer) {
		auto& spaces = engine_access::script_namespaces(eng);
		std::vector<const script_namespace_data*> nearer;
		for (const script_namespace_data* space = &own; space;) {
			for (const auto& [var_id, var_value] : space->variables) {
				bool hidden = space->scope && space->scope->get_own_value_ptr(var_id) != nullptr;
				for (const script_namespace_data* inner : nearer) {
					hidden = hidden || inner->variables.count(var_id) != 0 || (inner->scope && inner->scope->get_own_value_ptr(var_id) != nullptr);
				}
				if (hidden) { continue; }
				if (!layer) {
					layer = std::make_shared<environment>(scope, engine_access::get_symbolizer(eng));
					layer->mark_namespace_scope();
				}
				layer->define(var_id, var_value);
			}
			nearer.push_back(space);
			auto enclosing = space->enclosing ? spaces.find(space->enclosing) : spaces.end();
			space = enclosing != spaces.end() ? enclosing->second.get() : nullptr;
		}
	}

	// A call's environment: the namespace's scope (its cells, the enclosing namespaces' scopes, then the globals) under a layer of
	// the host entries as they stand, made for the call
	inline std::shared_ptr<environment> namespace_body_environment(engine& eng, uint64_t namespace_id) {
		auto& spaces = engine_access::script_namespaces(eng);
		auto ns_it = spaces.find(namespace_id);
		if (ns_it == spaces.end() || !ns_it->second) {
			return std::make_shared<environment>(engine_access::get_global_environment(eng), engine_access::get_symbolizer(eng));
		}
		script_namespace_data& own = *ns_it->second;
		const std::shared_ptr<environment>& scope = namespace_scope(eng, own, namespace_id);
		std::shared_ptr<environment> layer;
		define_namespace_host_entries(eng, own, scope, layer);
		return layer ? layer : scope;
	}

	// The environment a namespace's class declaration hands its methods, constructors and initializers: the same, with the layer
	// kept by the namespace and refilled at each declaration and host registration (calls on any thread only read it)
	inline std::shared_ptr<environment> namespace_class_environment(engine& eng, uint64_t namespace_id) {
		auto& own = engine_access::script_namespaces(eng)[namespace_id];
		if (!own) { own = std::make_shared<script_namespace_data>(); }
		const std::shared_ptr<environment>& scope = namespace_scope(eng, *own, namespace_id);
		define_namespace_host_entries(eng, *own, scope, own->host_layer);
		return own->host_layer ? own->host_layer : scope;
	}

	// `ns::f` and a bare `f` in the namespace's code: the overloads these arguments select through the one scorer (a tie raises),
	// else a static method of a class sharing the namespace's name, else the overload miss
	inline script_value namespace_function_value(engine& eng, uint64_t namespace_id, uint64_t member_id, std::vector<std::shared_ptr<function_decl>> overloads) {
		const uint64_t class_var_id = engine_access::get_symbolizer(eng)->get_class_var_id_with_view(namespace_id).first;
		engine* owner = &eng;
		script_function dispatcher = [owner, overloads = std::move(overloads), namespace_id, member_id, class_var_id](const std::vector<script_value>& args) -> checked_result<script_value> {
			execution_backend* backend = engine_access::get_execution_backend(*owner);
			if (!backend) {
				return checked_result<script_value>(make_error_code(runtime_error_code::engine_destroyed), "Engine backend unavailable");
			}
			auto selected = select_declaration(overloads, args, member_id, owner);
			if (!selected) { return selected.error_value(); }
			if (const std::shared_ptr<function_decl>& decl = selected.value()) {
				std::shared_ptr<environment> body_env = namespace_body_environment(*owner, namespace_id);
				if (decl->is_coroutine) {   // a coroutine member mints a handle over the namespace's scope
					auto handle = std::make_shared<coroutine_handle>(owner);
					handle->set_function(decl, args, body_env);
					return script_value::make_coroutine_handle(engine_access::symbolize(*owner, "coroutine_handle"), std::static_pointer_cast<void>(handle), owner);
				}
				script_callable payload;
				payload.kind = script_callable::kind_type::function;
				payload.fn = std::make_shared<script_defined_function>(decl->name, shared_parameters_for(decl.get()), decl->return_type, decl->body,
				                                                       std::move(body_env), decl->local_count);
				payload.fn->unit = decl->unit;
				return backend->execute_callable(payload, args);
			}
			if (auto class_var = engine_access::get_global_environment(*owner)->get(class_var_id); class_var && class_var.value().is_object()) {
				auto holder = class_var.value().get_object_holder();
				if (holder && holder->type_name() == "class_definition") {
					script_value static_method = std::static_pointer_cast<class_definition>(holder->host_object())->static_method_in_chain(member_id);
					if (static_method.is_function()) {
						const script_function& func = static_method.as_function();
						return engine_access::active_run_pins(*owner) ? call_function_value(func, args) : func(args);
					}
				}
			}
			return overload_miss(make_error_code(runtime_error_code::not_a_function), member_id, args.data(), args.size(), owner);
		};
		return script_value::make_function(std::move(dispatcher), &eng);
	}

	// A function the namespace admitted: its name's cell takes the overload set as it stands
	inline void publish_namespace_function(engine& eng, script_namespace_data& ns, uint64_t namespace_id, uint64_t member_id) {
		namespace_scope(eng, ns, namespace_id)->define(member_id, namespace_function_value(eng, namespace_id, member_id, ns.functions[member_id]));
		engine_access::bump_definition_epoch(eng);
	}

} // namespace jai::detail

#endif
