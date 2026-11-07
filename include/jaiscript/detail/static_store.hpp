#pragma once

#ifndef __JAISCRIPT_DETAIL_STATIC_STORE_HPP__
#define __JAISCRIPT_DETAIL_STATIC_STORE_HPP__

// Class-qualified static stores (C::s = v, C::s op= v, ++C::s) shared VERBATIM by the vm
// and the interpreter (parity by construction): the class-resolution ladder with its
// texts, then the instance-field store table over the static's declared type
// (class_definition::enforce_static_write). Bare static stores inside methods reach
// store_static_member through their own scope resolution.

#include <jaiscript/core/value.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/detail/namespace_scope.hpp>
#include <memory>
#include <optional>
#include <utility>

namespace jai::detail {

	inline checked_result<std::shared_ptr<class_definition>> resolve_static_store_class(const member_expr* member, const environment& env,
	                                                                                    const string_symbolizer& symbols,
	                                                                                    const class_definition* access_context) {
		using result = checked_result<std::shared_ptr<class_definition>>;
		if (member->object->get_type() != node_type::identifier_expr) {
			if (auto declared = qualified_extern_missing(env, member, symbols)) { return *declared; }
			return result(make_error_code(runtime_error_code::type_mismatch),
				"Static member assignment requires a class name");
		}
		const uint64_t class_name_id = static_cast<const identifier_expr*>(member->object.get())->symbol_id;
		auto [class_var_id, class_var_name] = symbols.get_class_var_id_with_view(class_name_id);
		auto class_result = env.get(class_var_id);
		if (!class_result) {
			if (auto declared = qualified_extern_missing(env, member, symbols)) { return *declared; }
			return result(make_error_code(runtime_error_code::undefined_variable),
				"Class '{0}' not found", class_name_id);
		}
		const script_value& class_var = class_result.value();
		if (!class_var.is_object()) {
			return result(make_error_code(runtime_error_code::type_mismatch),
				"'{0}' is not a class", class_name_id);
		}
		auto holder = class_var.get_object_holder();
		if (!holder || holder->type_name() != "class_definition") {
			return result(make_error_code(runtime_error_code::type_mismatch),
				"'{0}' is not a valid class", class_name_id);
		}
		auto class_def = std::static_pointer_cast<class_definition>(holder->host_object());
		if (class_def->chain_has_nonpublic()) [[unlikely]] {
			auto access = enforce_member_access(class_def.get(), member->member_id, access_context);
			if (!access) {
				return access.error_value();
			}
		}
		class_definition* owner = nullptr;   // D::s names a base's static as C++ does: the store lands on the declaring class
		if (class_def->find_static_field_in_chain(member->member_id, &owner) && owner != class_def.get()) {
			return owner->shared_from_this();
		}
		return class_def;
	}

	// A ::-qualified static (C::s) or namespace variable (ns::x) as a reference argument: its storage, a C++ lvalue. nullopt
	// when the spelling names neither (the caller's other ladders decide); an error for a host entry (read-only) or a denied
	// private static
	inline std::optional<checked_result<script_value*>> static_member_lvalue(engine& eng, const member_expr* member, const environment& env,
	                                                                         const string_symbolizer& symbols,
	                                                                         const class_definition* access_context) {
		using result = checked_result<script_value*>;
		if (!member || !member->is_static) { return std::nullopt; }
		if (auto ns = resolve_namespace_store(eng, member, symbols); ns && !ns->missing) {
			if (!ns->cell) {
				return result(make_error_code(runtime_error_code::invalid_reference), "Cannot pass non-lvalue to reference parameter");
			}
			return result(ns->cell);
		}
		if (member->object->get_type() != node_type::identifier_expr) { return std::nullopt; }
		auto cls = resolve_static_store_class(member, env, symbols, access_context);
		if (!cls) {
			if (cls.error() == make_error_code(runtime_error_code::access_violation)) { return result(cls.error_value()); }
			return std::nullopt;
		}
		if (script_value* cell = cls.value()->get_static_field_ptr(member->member_id)) { return result(cell); }
		return std::nullopt;
	}

	// A host static property (dynamic_binder::static_property with a setter): the store runs _set_x and reads back through
	// _get_x when there is one; nullopt when the class has no static setter for the name
	inline std::optional<checked_result<script_value>> store_static_accessor(class_definition& cls, uint64_t member_id, const script_value& incoming) {
		engine* eng = cls.get_engine();
		if (!eng) { return std::nullopt; }
		string_symbolizer* symbols = engine_access::get_symbolizer(*eng);
		script_value setter = cls.static_method_in_chain(symbols->get_setter_id_with_view(member_id).first);
		if (!setter.is_function()) { return std::nullopt; }
		std::vector<script_value> argument{incoming};
		auto set = setter.as_function()(argument);
		if (!set) {
			checked_result<script_value> failed = set.error_value();
			return failed;
		}
		script_value getter = cls.static_method_in_chain(symbols->get_getter_id_with_view(member_id).first);
		if (!getter.is_function()) { return checked_result<script_value>(incoming); }
		std::vector<script_value> none;
		return checked_result<script_value>(getter.as_function()(none));
	}

	// Presence, the field ladder, the write; the stored value is the expression result.
	// source: the store's operand (a by-value class static holding an instance takes it in place); copy(): the stored value otherwise
	template <class Copy>
	inline checked_result<script_value> store_static_member(class_definition& cls, uint64_t member_id, const script_value& source, Copy&& copy) {
		if (!cls.has_static_field(member_id)) {
			script_value incoming = copy();
			if (auto accessed = store_static_accessor(cls, member_id, incoming)) { return std::move(*accessed); }
			return checked_result<script_value>(make_error_code(runtime_error_code::undefined_variable),
				"Cannot assign to static member: field '{0}' not found", member_id);
		}
		auto enforced = cls.static_store(member_id, source, copy);
		if (!enforced) {
			return enforced.error_value();
		}
		script_value stored = enforced.value();
		(void)cls.set_static_field(member_id, std::move(enforced.value()));
		return stored;
	}

} // namespace jai::detail

#endif
