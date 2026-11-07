#pragma once

// The construction kernel both backends run, a template over the backend's host (its final lender), so every hook call binds
// statically (construction.hpp declares the host and the scope)
#include <jaiscript/detail/construction.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/class_registry.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/detail/aggregate_init.hpp>
#include <jaiscript/detail/arg_capture.hpp>

namespace jai::detail {

namespace construction_kernel {
	// a level's scope, opened where its code starts and closed when the construction leaves the level
	template <class Host>
	struct level_scope {
		Host& host;
		construction_scope scope;
		explicit level_scope(Host& h) : host(h) {}
		level_scope(const level_scope&) = delete;
		level_scope& operator=(const level_scope&) = delete;
		~level_scope() { if (scope) { host.close_scope(scope); } }
	};

	// an initializer list's arguments, each taken as it evaluates, as a call's are (one a T& may bind carries the place it names); a
	// failure propagates as raised
	template <class Host>
	checked_result<void> evaluate_initializer_arguments(Host& host, const constructor_initializer& initializer, construction_scope& scope,
	                                                    const class_definition* binding_class, std::vector<script_value>& result) {
		result.reserve(initializer.arguments.size());
		const bool may_bind_places = constructors_take_references(binding_class);
		for (const auto& arg_expr : initializer.arguments) {
			auto evaluated = host.evaluate_in(scope, arg_expr);
			if (!evaluated) { return evaluated.error_value(); }
			result.push_back(std::move(evaluated.value()));
			capture_initializer_argument(host.eng, initializer, result.size() - 1, result.back(),
			                             [&] { return constructor_binding(binding_class, result.size() - 1); });
			if (host.unwinding) { return {}; }
			if (may_bind_places && !early_bound_reference(result.back())) {
				const arg_binding binding = constructor_binding(binding_class, result.size() - 1);
				if (!binds_reference(binding)) { continue; }
				script_value minted(std::monostate{}, host.eng);
				auto placed = host.initializer_place(scope, arg_expr.get(), minted);
				if (!placed) { return placed.error_value(); }
				if (placed.value()) { hold_minted_reference(host.eng, result.back(), binding, std::move(minted)); }
			}
		}
		return {};
	}

	// The constructor script base `index` of `level` runs with the arguments its initializer (`named`) passes: the one its overloads
	// pick, the implicit default one when it declares none but a base of its own runs code, else null (default-constructed: its
	// fields and its bases'), else the miss's error
	inline checked_result<std::shared_ptr<function_decl>> choose_base_constructor(const class_definition& level, script_class_definition& base, size_t index,
	                                                                               bool named, const script_value* args, size_t argc, engine* eng) {
		const auto& ctors = base.get_constructor_asts();
		if (ctors.size() == 1 && arity_accepts(ctors[0]->parameters, argc)) { return ctors[0]; }   // the scorer's pick, else the sole arity match
		auto picked = select_declaration(ctors, args, argc, base.get_type_id(), eng);
		if (!picked || picked.value()) { return picked; }
		const bool declares_constructors = !ctors.empty();
		if (!declares_constructors && argc == 0 && base.base_needs_construction()) { return base.implicit_default_constructor(); }
		using result = checked_result<std::shared_ptr<function_decl>>;
		if (index == 0) {
			if (declares_constructors) {
				if (argc == 0) { return no_default_constructor(base.get_type_id()); }
				return overload_miss(make_error_code(runtime_error_code::no_constructor_found), base.get_type_id(), args, argc, eng, true);
			}
			if (argc != 0) { return result(make_error_code(runtime_error_code::no_constructor_found), "No matching parent constructor found for super()"); }
		} else if (named && (declares_constructors || argc != 0)) {
			return overload_miss(make_error_code(runtime_error_code::no_constructor_found), base.get_type_id(), args, argc, eng, true);
		} else if (declares_constructors) {
			return result(make_error_code(runtime_error_code::no_constructor_found), k_base_needs_naming, level.get_type_id(), base.get_type_id());
		}
		return std::shared_ptr<function_decl>();
	}

	inline void adopt_host_base_object(class_instance& instance, const script_value& host_object) {
		if (!host_object.is_object()) { return; }
		auto host_instance = host_object.as<strong_ptr<class_instance>>();
		if (!host_instance) { return; }
		if (const script_value* native = host_instance->native()) {
			instance.set_native(*native);
		}
		instance.region_tag(host_instance->region_tag());
	}

	// A C++ base's constructor makes the object the instance adopts; a_ancestor marks a base above the direct one
	template <class Host>
	checked_result<void> construct_host_base(Host& host, class_instance& instance, const class_definition& base,
	                                         const std::vector<script_value>& args, bool a_ancestor) {
		const std::string& name = base.get_name();
		auto ctor = host.current_environment()->get(name, *host.symbols);
		if (args.empty() && (!ctor || !ctor.value().is_function() || host_lacks_default_constructor(host.eng, name))) {
			return no_default_constructor(base.get_type_id());
		}
		if (!ctor || !ctor.value().is_function()) { return {}; }
		script_value host_ctor = std::move(ctor.value());
		auto constructed = host_ctor.as_function()(args);
		if (!constructed) {
			if (args.empty() && is_constructor_miss(constructed.error())) {
				return no_default_constructor(base.get_type_id());
			}
			if (a_ancestor) {
				return checked_result<void>(make_error_code(runtime_error_code::constructor_failed), "Failed to call C++ ancestor constructor");
			}
			return constructed.error_value();
		}
		adopt_host_base_object(instance, constructed.value());
		return {};
	}

	// The initializer of `initializers` that constructs base `index` of `level`: super(...) or the base's name for the first, the name
	// for any other; null when none names it
	inline const constructor_initializer* base_initializer(const std::vector<constructor_initializer>& initializers, const class_definition& level, size_t index) {
		const auto& parents = level.get_parent_classes();
		const class_definition* base = index < parents.size() ? parents[index].get() : (index == 0 ? level.parent_ptr() : nullptr);
		for (const auto& init : initializers) {
			if ((index == 0 && init.target == "super") || (base && initializer_names(init.target, base->get_name()))) { return &init; }
		}
		return nullptr;
	}

	// what every level of one construction shares
	struct construction_context {
		const std::shared_ptr<script_class_definition>& cls;
		const std::shared_ptr<environment>& definition_env;
		class_instance& instance;
		const script_value& this_value;
		bool code_ran = false;   // a literal default create_instance seeded may have been overwritten since
	};

	template <class Host>
	checked_result<void> construct_level(Host& host, construction_context& cx, const std::shared_ptr<script_class_definition>& level,
	                                     const std::shared_ptr<function_decl>& ctor, const std::vector<script_value>& args);

	// A base its derived constructor leaves to its default and that has no constructor whose own bases run none: a literal-only chain
	// stamped, else its implicit default constructor (its bases, then its fields)
	template <class Host>
	checked_result<void> initialize_plain_base(Host& host, construction_context& cx, const std::shared_ptr<script_class_definition>& base) {
		if (base->chain_initializers_all_literal()) {
			if (cx.code_ran || !cx.instance.seeded_from_plan()) { stamp_literal_field_initializers(cx.instance, *base, false, host.eng); }
			return {};
		}
		cx.code_ran = true;
		return construct_level(host, cx, base, base->implicit_default_constructor(), {});
	}

	// Base `index` of `level` (C++ [class.base.init]): with the arguments the level's initializer list names it with, else
	// default-constructed
	template <class Host>
	checked_result<void> construct_base(Host& host, construction_context& cx, const std::shared_ptr<script_class_definition>& level,
	                                    const std::shared_ptr<class_definition>& base, size_t index, const constructor_initializer* init,
	                                    construction_scope& scope) {
		std::vector<script_value> base_args;
		if (init && !init->arguments.empty()) {
			cx.code_ran = true;
			auto evaluated = evaluate_initializer_arguments(host, *init, scope, base.get(), base_args);
			if (!evaluated || host.unwinding) { return evaluated; }
		}
		if (!base->is_script_class()) {
			if (index > 0) { return {}; }
			cx.code_ran = true;
			if (level.get() != cx.cls.get()) { return construct_host_base(host, cx.instance, *base, base_args, true); }
			try {
				return construct_host_base(host, cx.instance, *base, base_args, false);
			} catch (const runtime_error&) {
				return checked_result<void>(make_error_code(runtime_error_code::constructor_failed), "Failed to call C++ parent constructor");
			}
		}
		auto script_base = std::static_pointer_cast<script_class_definition>(base);
		auto base_ctor = choose_base_constructor(*level, *script_base, index, init != nullptr, base_args.data(), base_args.size(), host.eng);
		if (!base_ctor) { return base_ctor.error_value(); }
		if (!base_ctor.value()) { return initialize_plain_base(host, cx, script_base); }
		cx.code_ran = true;
		return construct_level(host, cx, script_base, base_ctor.value(), base_args);
	}

	// The prologue of one constructor's activation, its parameters bound in the scope: a delegation's whole target, else each base in
	// declaration order (each marked constructed once it is), then the level's own fields
	template <class Host>
	checked_result<void> construct_level_members(Host& host, construction_context& cx, const std::shared_ptr<script_class_definition>& level,
	                                             const std::shared_ptr<function_decl>& ctor, construction_scope& scope) {
		const auto& initializers = ctor->initializers;
		const auto delegation = std::find_if(initializers.begin(), initializers.end(), [](const constructor_initializer& init) { return init.target == "this"; });
		if (delegation != initializers.end()) {
			// C++ delegation: the target constructor constructs the level (its bases, its members and its body) ahead of this body
			cx.code_ran = true;
			std::vector<script_value> target_args;
			auto evaluated = evaluate_initializer_arguments(host, *delegation, scope, level.get(), target_args);
			if (!evaluated || host.unwinding) { return evaluated; }
			std::vector<std::shared_ptr<function_decl>> other_ctors;
			for (const auto& candidate : level->get_constructor_asts()) { if (candidate != ctor) { other_ctors.push_back(candidate); } }
			auto target_pick = select_declaration(other_ctors, target_args, level->get_type_id(), host.eng);
			if (!target_pick) { return target_pick.error_value(); }
			if (!target_pick.value()) {
				return overload_miss(make_error_code(runtime_error_code::no_constructor_found), level->get_type_id(), target_args.data(), target_args.size(), host.eng, true);
			}
			auto constructed = construct_level(host, cx, level, target_pick.value(), target_args);
			if (!constructed || host.unwinding) { return constructed; }
			cx.instance.constructed_levels(cx.cls->constructed_through(level.get()));   // C++: the level is constructed once its target finishes
			return {};
		}
		const auto& parents = level->get_parent_classes();
		if (parents.empty() && std::any_of(initializers.begin(), initializers.end(), [](const constructor_initializer& init) { return init.target == "super"; })) {
			return checked_result<void>(make_error_code(runtime_error_code::no_constructor_found), "Cannot call super() - class has no base class");
		}
		for (size_t i = 0; i < parents.size(); ++i) {
			if (!parents[i]) { continue; }
			const constructor_initializer* init = base_initializer(initializers, *level, i);
			if (!init || init->arguments.empty()) { host.at(ctor.get()); }   // the constructor's own line, as its frame stands while the base constructs
			auto constructed = construct_base(host, cx, level, parents[i], i, init, scope);
			if (!constructed || host.unwinding) { return constructed; }
			cx.instance.constructed_levels(cx.cls->constructed_through(parents[i].get()));
		}
		if (level->chain_initializers_all_literal()) {
			if (cx.code_ran || !cx.instance.seeded_from_plan()) { stamp_literal_field_initializers(cx.instance, *level, true, host.eng); }
		} else if (!level->get_field_initializer_asts().empty()) {
			auto initialized = host.evaluate_field_initializers(cx.instance.strong_from_this(), level, scope, true, nullptr);
			if (host.unwinding) { return {}; }
			if (!initialized) { return initialized; }
		}
		return {};
	}

	// One constructor's activation on its level (C++: one per constructor): the host binds its parameters, runs its prologue
	// (construct_level_members) in that activation, then its body; a base level answers as itself while it runs ([class.cdtor])
	template <class Host>
	checked_result<void> construct_level(Host& host, construction_context& cx, const std::shared_ptr<script_class_definition>& level,
	                                     const std::shared_ptr<function_decl>& ctor, const std::vector<script_value>& args) {
		if (native_stack_low()) [[unlikely]] {
			return raise_native_stack();
		}
		for (const auto& param : ctor->parameters) {
			if (param.symbol_id == UINT64_MAX) { param.symbol_id = host.symbols->intern(param.name); }
		}
		std::optional<dispatch_level_scope> level_dispatch;
		if (level.get() != cx.cls.get()) { level_dispatch.emplace(cx.instance, level.get(), class_instance::cdtor_phase::construction); }
		return host.run_constructor(level, ctor, args, cx.definition_env, cx.this_value,
		                            [&](construction_scope& scope) { return construct_level_members(host, cx, level, ctor, scope); });
	}
}


// A construction prepare_construction began, under its constructor's activation: its prologue, then its body
template <class Host>
checked_result<void> construct_instance_members(Host& host, const std::shared_ptr<script_class_definition>& cls, const std::shared_ptr<environment>& definition_env,
                                                const script_value& this_value, const std::shared_ptr<function_decl>& ctor, const std::vector<script_value>& args) {
	construction_kernel::construction_context cx{cls, definition_env, *this_value.peek_class_instance(), this_value};
	return construction_kernel::construct_level(host, cx, cls, ctor, args);
}

template <class Host>
checked_result<script_value> construct_default_instance(Host& host, const std::shared_ptr<script_class_definition>& cls,
                                                        const std::vector<script_value>& args) {
	// arguments to a class without a constructor: a copy, or aggregate initialization
	aggregate_plan aggregate;
	if (!args.empty()) {
		aggregate = plan_aggregate(*cls, args);
		if (!aggregate.fills()) { return aggregate_answer(aggregate, *cls, args, host.eng); }
	}
	if (host.call_depth >= host.depth_gate) {
		if (auto blocked = host.pass_depth_gate()) { return *blocked; }
	}
	if (native_stack_low()) [[unlikely]] {
		return raise_native_stack();
	}
	auto sandbox = host.parallel_sandbox();
	if (!aggregate.fills() && (cls->base_needs_construction() || !cls->chain_initializers_all_literal())) {
		// C++: the implicit default constructor, its bases ahead of its fields
		auto instance = cls->create_instance();
		instance->construction_pending(true);
		auto this_value = script_value::make_class_object(*cls, instance, host.eng);
		const auto definition_env = sandbox ? sandbox : cls->namespace_scope() ? cls->namespace_scope()
			: cls->base_needs_construction() ? host.global_environment() : environment_chain_root(host.current_environment());
		auto constructed = host.construct_implicitly(cls, definition_env, this_value);
		if (!constructed) { return constructed.error_value(); }
		if (!host.unwinding) { instance->construction_pending(false); }
		return this_value;
	}
	call_depth_guard initializer_depth(host.call_depth);
	auto instance = cls->create_instance();
	auto this_value = script_value::make_class_object(*cls, instance, host.eng);
	if (!aggregate.fills()) {
		if (!instance->seeded_from_plan()) { stamp_literal_field_initializers(*instance, *cls, false, host.eng); }
		return this_value;
	}
	const auto scope_parent = sandbox ? sandbox : cls->namespace_scope() ? cls->namespace_scope() : environment_chain_root(host.current_environment());
	construction_kernel::level_scope<Host> init_env(host);
	host.open_scope(init_env.scope, scope_parent, this_value, cls.get());   // aggregate filling reads the environment
	auto initialized = host.evaluate_field_initializers(instance, cls, init_env.scope, false, &aggregate);
	if (!initialized) { return initialized.error_value(); }
	return this_value;
}


}
