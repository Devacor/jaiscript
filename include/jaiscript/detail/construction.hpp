#pragma once

#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/core/script_class.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/execution_limits.hpp>
#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

namespace jai::detail {
	struct aggregate_plan;

	// What a constructor's prologue and an aggregate's initializers run in. Its initializer-list arguments: the constructor's
	// parameters by name, `this`, then the definition environment; its field initializers (members): `this`, then the definition
	// environment, as a C++ default member initializer never sees the parameters (null: env)
	struct construction_scope {
		std::shared_ptr<environment> env;
		std::shared_ptr<environment> members;
		explicit operator bool() const noexcept { return env != nullptr; }
	};

	// What a backend lends the construction kernel: its field initializers, its depth and unwinding state, and how it constructs a
	// class through the implicit default constructor (the interpreter's lender also runs a constructor's activation for the
	// kernel's construct_level; the vm's prologues are compiled)
	class construction_host {
	public:
		construction_host(engine* a_eng, string_symbolizer* a_symbols, string_symbolizer* a_envSymbols, int& a_callDepth, int a_depthGate, const bool& a_unwinding) :
			eng(a_eng),
			symbols(a_symbols),
			env_symbols(a_envSymbols),
			call_depth(a_callDepth),
			depth_gate(a_depthGate),
			unwinding(a_unwinding) {
		}

		engine* const eng;
		string_symbolizer* const symbols;
		string_symbolizer* const env_symbols;   // the environments' epoch sink (a parallel worker's own); names go through symbols
		int& call_depth;
		const int depth_gate;   // a call depth at or past it asks pass_depth_gate
		const bool& unwinding;

		virtual checked_result<void> evaluate_field_initializers(const strong_ptr<class_instance>& instance, const std::shared_ptr<script_class_definition>& cls,
		                                                         construction_scope& scope, bool skip_parent_recursion, const aggregate_plan* aggregate) = 0;
		// A scope: the level environment, its access context `level`
		void open_scope(construction_scope& scope, const std::shared_ptr<environment>& definition_env, const script_value& this_value, script_class_definition* level);
		void close_scope(construction_scope&) {}
		// The picked constructor's T& parameters bind the call's places now, ahead of any of the construction's code (C++: the
		// parameters bind at the call): each such argument becomes its reference. A host without the call's site leaves them
		virtual checked_result<void> bind_reference_arguments(const function_decl& ctor, std::vector<script_value>& args) = 0;
		// The class's implicit default constructor on the instance under construction (`this_value`): its bases, then its fields
		virtual checked_result<void> construct_implicitly(const std::shared_ptr<script_class_definition>& cls, const std::shared_ptr<environment>& definition_env,
		                                                  const script_value& this_value) = 0;
		virtual std::optional<error_propagator> pass_depth_gate() = 0;
		virtual std::shared_ptr<environment> current_environment() = 0;
		virtual std::shared_ptr<environment> global_environment() = 0;
		virtual const class_definition* caller_access_context() = 0;   // asked only for a private constructor
		// a parallel worker's method sandbox, where a construction without a constructor of its own resolves names (null
		// off the workers: the program's scope)
		virtual std::shared_ptr<environment> parallel_sandbox() = 0;

	protected:
		~construction_host() = default;
	};

	// One field a constructor's prologue initializes: `listed` is its member initializer, which reads the constructor's parameters;
	// otherwise the value is the field's own initializer, which never sees them
	struct field_initialization {
		uint64_t field_id = 0;
		expression_ptr value;
		bool listed = false;
	};

	inline bool initializes_fields(const function_decl& ctor) {
		return std::any_of(ctor.initializers.begin(), ctor.initializers.end(),
		                   [](const constructor_initializer& init) { return init.kind == constructor_initializer::kind_t::field; });
	}

	// C++ [class.base.init]: what the prologue of a constructor that names fields initializes, in the fields' declaration order
	// whatever the list's order: a field the list names takes that initializer and skips its own. own_initializers false leaves the
	// fields the list does not name to the literal stamp (a literal-only chain)
	std::vector<field_initialization> constructor_field_plan(const script_class_definition& level, const function_decl& ctor,
	                                                         const string_symbolizer& symbols, bool own_initializers);

	inline bool constructor_takes_references(const function_decl& ctor) {
		return std::any_of(ctor.parameters.begin(), ctor.parameters.end(), [](const parameter& param) { return param.is_reference; });
	}

	// some constructor of `cls` takes a T&: an initializer list's arguments to it may bind places
	inline bool constructors_take_references(const class_definition* cls) {
		if (!cls || !cls->is_script_class()) { return false; }
		const auto& ctors = static_cast<const script_class_definition*>(cls)->get_constructor_asts();
		return std::any_of(ctors.begin(), ctors.end(), [](const std::shared_ptr<function_decl>& ctor) { return constructor_takes_references(*ctor); });
	}

	// A constructor's prologue evaluates code (an initializer-list argument, or a field initializer the class does not stamp): it needs
	// a scope of its own
	inline bool prologue_evaluates(const script_class_definition& level, const function_decl& ctor) {
		bool delegates = false;
		for (const auto& init : ctor.initializers) {
			if (!init.arguments.empty() || init.kind == constructor_initializer::kind_t::field) { return true; }
			delegates = delegates || init.target == "this";
		}
		if (delegates || level.chain_initializers_all_literal()) { return false; }
		const auto& fields = level.get_field_initializer_asts();
		return std::any_of(fields.begin(), fields.end(), [](const auto& field) { return field.second != nullptr; });
	}

	// A one-argument construction from an object of the class, ahead of the declared constructors: a prvalue of exactly the class is
	// the object (C++17 [dcl.init]); else, C++ [class.copy.ctor], a class that declares constructors, none of them from an object of
	// itself, still has the implicit copy constructor T(const T&), a candidate when the argument is an object of the class (or of a
	// class derived from it) held by value or through a shared_ptr: the answer is the copy when the scorer ranks it ahead of every
	// declared constructor (a declared T(shared_ptr<T>) takes a handle as it is), nullopt when a declared one takes the call
	std::optional<checked_result<script_value>> implicit_copy_construction(const script_class_definition& cls, const std::vector<script_value>& args, engine* eng);

	// A T& argument that names no place: a reference or a host object a T& result named binds as it is, a temporary the parameter
	// takes (a string&, a const T&, one the body never writes through) binds as one, anything else raises
	checked_result<void> bind_placeless_reference_argument(engine* eng, const function_decl& ctor, size_t i, script_value& arg, bool may_prove);

	// A construction whose result is a handle (new T(...), make_shared<T>(...)): its constructor's `this` is that handle's object, so
	// a handle made from `this` there shares it. The next construction to start takes the stamp; one nested in its arguments runs first
	struct handle_construction_scope {
		engine* eng;
		handle_construction_scope(engine* a_engine, type_info* a_handle) : eng(a_engine) {
			if (eng && !engine_access::parallel_region_active(*eng)) { engine_access::handle_construction(*eng) = a_handle; } else { eng = nullptr; }
		}
		~handle_construction_scope() { if (eng) { engine_access::handle_construction(*eng) = nullptr; } }
	};
	inline type_info* take_handle_construction(engine* a_engine) noexcept {
		if (!a_engine) { return nullptr; }
		type_info*& slot = engine_access::handle_construction(*a_engine);
		type_info* result = slot;
		slot = nullptr;
		return result;
	}
	inline script_value constructed_this(const script_class_definition& a_cls, strong_ptr<class_instance> a_instance, type_info* a_handle, engine* a_engine) {
		script_value result = script_value::make_class_object(a_cls, std::move(a_instance), a_engine);
		if (a_handle) { result.set_type_info(type_info_ptr{a_handle}); }
		return result;
	}

	// A construction ahead of any of its code: the overload, its T& parameters bound to the call's places (captured_out then holds
	// the arguments) and the instance. Success yields the instance and the picked constructor rides ctor_out; a null ctor_out is the
	// implicit copy constructor's pick, whose value is the finished copy. Inline, so the hot pick and allocation compile in each
	// backend's own translation unit.
	template <class Host>
	inline checked_result<script_value> prepare_construction(Host& host, const std::shared_ptr<script_class_definition>& cls, const std::vector<script_value>& args,
	                                                         std::optional<call_depth_guard>& initializer_depth, std::shared_ptr<function_decl>& ctor_out,
	                                                         std::vector<script_value>& captured_out, bool check_access) {
		type_info* const handle = take_handle_construction(host.eng);
		if (host.call_depth >= host.depth_gate) {
			if (auto blocked = host.pass_depth_gate()) { return *blocked; }
		}
		if (native_stack_low()) [[unlikely]] {
			return raise_native_stack();
		}
		initializer_depth.emplace(host.call_depth);
		if (args.size() == 1 && args[0].deref().raw_storage_index() == script_value::TYPEID_OBJECT) {
			if (auto copied = implicit_copy_construction(*cls, args, host.eng)) {
				ctor_out = nullptr;
				return std::move(*copied);
			}
		}
		// the constructor overloads through the one script-overload scorer (typed exact > conversion > var); with several and none
		// viable the sole arity match binds (its parameter raises its own text), else the overload miss; a tie raises
		const auto& ctors = cls->get_constructor_asts();
		std::shared_ptr<function_decl> ctor;
		if (ctors.size() == 1) {
			if (arity_accepts(ctors[0]->parameters, args.size())) { ctor = ctors[0]; }
		} else {
			auto picked = select_declaration(ctors, args, cls->get_type_id(), host.eng);
			if (!picked) { return picked.error_value(); }
			ctor = std::move(picked.value());
		}
		if (!ctor) {
			return overload_miss(make_error_code(runtime_error_code::no_constructor_found), cls->get_type_id(), args.data(), args.size(), host.eng, true);
		}
		if (check_access) {
			if (auto access = enforce_constructor_access(*cls, ctor.get(), [&]() { return host.caller_access_context(); }); !access) {
				return access.error_value();
			}
		}
		if (constructor_takes_references(*ctor)) {
			if (host.owns_arguments(args)) {   // the call's own list moves: a prvalue in it stays its parameter
				captured_out.clear();
				captured_out.reserve(args.size());
				for (const script_value& arg : args) { captured_out.push_back(std::move(const_cast<script_value&>(arg))); }
			} else {
				captured_out = args;
			}
			if (auto bound = host.bind_reference_arguments(*ctor, captured_out); !bound) { return bound.error_value(); }
		}
		auto instance = cls->create_instance();
		instance->construction_pending(true);
		ctor_out = std::move(ctor);
		return constructed_this(*cls, std::move(instance), handle, host.eng);
	}

	// A construction prepare_construction began, under its constructor's activation: its prologue, then its body (the interpreter's;
	// the vm compiles the prologue into the constructor's chunk)
	template <class Host>
	checked_result<void> construct_instance_members(Host& host, const std::shared_ptr<script_class_definition>& cls, const std::shared_ptr<environment>& definition_env,
	                                                const script_value& this_value, const std::shared_ptr<function_decl>& ctor, const std::vector<script_value>& args);

	// A class without a constructor: a copy, aggregate initialization, or the implicit default constructor
	template <class Host>
	checked_result<script_value> construct_default_instance(Host& host, const std::shared_ptr<script_class_definition>& cls,
	                                                        const std::vector<script_value>& args);

	inline std::shared_ptr<environment> environment_chain_root(std::shared_ptr<environment> result) {
		for (int hops = 0; result && result->get_parent() && hops < 65536; ++hops) {
			result = result->get_parent();
		}
		return result;
	}

	// A literal-only chain's field initializers stamped without an environment: parents first, then the class's own
	void stamp_literal_field_initializers(class_instance& instance, const script_class_definition& cls, bool skip_parent_recursion, engine* eng);

	// The common default construction (no arguments, no base to construct, a literal-only chain) inline at the call; nullopt
	// leaves the call to construct_default_instance
	inline std::optional<script_value> construct_literal_default(const std::shared_ptr<script_class_definition>& cls, const std::vector<script_value>& args,
	                                                             engine* eng, int& call_depth, int depth_gate) {
		if (!args.empty() || call_depth >= depth_gate || cls->base_needs_construction() || !cls->chain_initializers_all_literal() || native_stack_low()) {
			return std::nullopt;
		}
		call_depth_guard depth(call_depth);
		auto instance = cls->create_instance();
		auto result = script_value::make_class_object(*cls, instance, eng);
		if (!instance->seeded_from_plan()) { stamp_literal_field_initializers(*instance, *cls, false, eng); }
		return result;
	}

	// A call that resolved nowhere on an object under construction or destruction, to a method only a class derived from the level
	// answering for it declares (C++ [class.cdtor]): the refusal both backends raise in place of the miss they would raise, and the
	// strict checker's text for the same call when it sees one
	inline constexpr std::string_view k_call_under_construction = "Cannot call '{0}' while '{1}' is under construction: only a class derived from it declares it";
	inline constexpr std::string_view k_call_under_destruction = "Cannot call '{0}' while '{1}' is under destruction: only a class derived from it declares it";
	std::optional<error_propagator> refuse_cdtor_call(class_instance* instance, uint64_t name_id);

	// A constructor that completed arms its instance's destructor
	inline void construction_completed(const script_value& instance_value) {
		const auto holder = instance_value.get_object_holder();
		if (holder && holder->is_instance()) { holder->unchecked_instance()->construction_pending(false); }
	}
}
