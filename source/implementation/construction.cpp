#include <jaiscript/detail/construction.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/class_registry.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/detail/aggregate_init.hpp>
#include <jaiscript/detail/arg_capture.hpp>
#include <jaiscript/detail/parallel_transform.hpp>
#include <jaiscript/detail/ref_lvalue.hpp>

namespace jai::detail {

void stamp_literal_field_initializers(class_instance& instance, const script_class_definition& cls, bool skip_parent_recursion, engine* eng) {
	if (!skip_parent_recursion) {
		for (const auto& parent : cls.get_parent_classes()) {
			if (parent && parent->is_script_class()) {
				stamp_literal_field_initializers(instance, static_cast<const script_class_definition&>(*parent), false, eng);
			}
		}
	}
	for (const auto& [field_id, initializer_ast] : cls.get_field_initializer_asts()) {
		if (!initializer_ast) { continue; }
		const auto& storage = static_cast<const literal_expr*>(initializer_ast.get())->value.get_storage();
		switch (storage.index()) {
			case script_value::TYPEID_INT: instance.set_field(field_id, script_value(storage.get<script_int>(), eng)); break;
			case script_value::TYPEID_FLOAT: instance.set_field(field_id, script_value(storage.get<script_float>(), eng)); break;
			case script_value::TYPEID_CHAR: instance.set_field(field_id, script_value(storage.get<script_char>(), eng)); break;
			case script_value::TYPEID_BOOL: instance.set_field(field_id, script_value(storage.get<script_bool>(), eng)); break;
			default: instance.set_field(field_id, script_value(std::monostate{}, eng)); break;
		}
	}
}

std::vector<field_initialization> constructor_field_plan(const script_class_definition& level, const function_decl& ctor,
                                                         const string_symbolizer& symbols, bool own_initializers) {
	std::vector<std::pair<uint64_t, const constructor_initializer*>> listed;
	for (const auto& init : ctor.initializers) {
		if (init.kind != constructor_initializer::kind_t::field) { continue; }
		if (const auto id = symbols.find(init.target)) { listed.emplace_back(*id, &init); }
	}
	std::vector<field_initialization> result;
	for (uint64_t id : level.field_order()) {
		if (level.has_static_field(id)) { continue; }
		const auto named = std::find_if(listed.begin(), listed.end(), [id](const auto& entry) { return entry.first == id; });
		if (named != listed.end()) {
			result.push_back({id, named->second->value, true});
		} else if (own_initializers) {
			if (expression_ptr own = level.get_field_initializer_ast(id)) { result.push_back({id, std::move(own), false}); }
		}
	}
	return result;
}

void construction_host::open_scope(construction_scope& scope, const std::shared_ptr<environment>& definition_env, const script_value& this_value,
                                   script_class_definition* level) {
	scope.env = std::make_shared<environment>(definition_env, env_symbols);
	scope.env->define(symbols->get_this_id(), this_value);
	scope.env->set_access_context(level);
}

checked_result<void> bind_placeless_reference_argument(engine* eng, const function_decl& ctor, size_t i, script_value& arg, bool may_prove) {
	const parameter& param = ctor.parameters[i];
	if (arg.is_reference() || borrows_host_object(arg)) { return {}; }
	if (is_string_reference_temporary(param, arg) || param.is_const ||
	    (eng && ref_parameter_takes_temporary(*eng, ctor.parameters, ctor.body.get(), i, may_prove))) {
		arg = make_reference_argument_temporary(param, arg, eng);
		return {};
	}
	return checked_result<void>(make_error_code(runtime_error_code::invalid_reference), "Cannot pass non-lvalue to reference parameter");
}

JAI_NOINLINE std::optional<checked_result<script_value>> implicit_copy_construction(const script_class_definition& cls, const std::vector<script_value>& args,
                                                                                    engine* eng) {
	if (prvalue_of_class(args[0], cls)) { return args[0]; }
	if (!is_instance_of_class(args[0], cls, true)) { return std::nullopt; }
	const auto& ctors = cls.get_constructor_asts();
	const std::shared_ptr<function_decl>& implicit = cls.implicit_copy_constructor();
	for (const auto& ctor : ctors) {
		if (ctor->parameters.empty() || !arity_accepts(ctor->parameters, 1)) { continue; }
		const type_info* first = ctor->parameters[0].type.get();
		if (first && first->base_type == script_value_type::jai_reference_type) { first = first->type_params.empty() ? nullptr : first->type_params[0].get(); }
		if (first && first->base_type == script_value_type::jai_object_type && first->type_name == cls.get_name()) { return std::nullopt; }
	}
	std::vector<std::shared_ptr<function_decl>> candidates;
	candidates.reserve(ctors.size() + 1);
	candidates.insert(candidates.end(), ctors.begin(), ctors.end());
	candidates.push_back(implicit);
	const auto verdict = score_declarations(candidates, args.data(), args.size(), eng);
	if (!verdict.best || verdict.rival || *verdict.best != implicit) { return std::nullopt; }
	return copy_of_instance(args[0]);
}

JAI_NOINLINE std::optional<error_propagator> refuse_cdtor_call(class_instance* instance, uint64_t name_id) {
	if (!instance) { return std::nullopt; }
	auto phase = instance->dispatch_phase();
	if (phase == class_instance::cdtor_phase::none && instance->construction_pending()) { phase = class_instance::cdtor_phase::construction; }
	const class_definition* level = instance->get_class_definition();
	engine* eng = instance->get_engine();
	if (phase == class_instance::cdtor_phase::none || !level || !eng || level->method_ptr(name_id) != nullptr) { return std::nullopt; }
	for (const auto& [name, cls] : engine_access::get_class_registry(*eng).script_classes_) {
		if (cls.get() == level || !cls->derives_from(level) || cls->method_ptr(name_id) == nullptr) { continue; }
		return error_propagator{make_error_code(runtime_error_code::member_not_found),
			phase == class_instance::cdtor_phase::construction ? k_call_under_construction : k_call_under_destruction,
			name_id, level->get_type_id()};
	}
	return std::nullopt;
}

}
