#pragma once

#ifndef __JAISCRIPT_DETAIL_NAMESPACE_SCOPE_HPP__
#define __JAISCRIPT_DETAIL_NAMESPACE_SCOPE_HPP__

// A namespace is a scope (C++): its script-declared variables and functions are the cells of one environment
// per namespace, parented on the enclosing namespace's (else the global environment), so a namespace function
// reads and writes them by their bare names and `ns::x = v` writes the same cell. Shared VERBATIM by the vm and the interpreter:
// the scope, the qualified path of a static member chain and the store target of `ns::x = v`.
// Host-registered entries (script_namespace_data::variables) stay values the host owns: readable
// through `ns::name`, never assigned from script.

#include <jaiscript/core/class_definition.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <memory>
#include <optional>
#include <string>

namespace jai::detail {

	// A class a namespace declares is named ns::Name (C++); the namespace lists it under its bare name, the one its blocks read
	inline void register_namespace_class(engine& eng, script_namespace_data& ns, const class_decl& decl) {
		auto def = engine_access::get_class_definition(eng, std::string(decl.name));
		if (!def) { return; }
		const size_t cut = decl.name.rfind("::");
		ns.classes[engine_access::get_symbolizer(eng)->intern(cut == std::string_view::npos ? decl.name : decl.name.substr(cut + 2))] = std::move(def);
	}

	// "a::b" nests in "a" (C++17's nested namespace definition), so its scope is parented on a's and a bare name its code reads
	// finds b's members, then a's, then the globals
	inline const std::shared_ptr<environment>& namespace_scope(engine& eng, script_namespace_data& ns, uint64_t namespace_id) {
		if (!ns.scope) {
			string_symbolizer& symbols = *engine_access::get_symbolizer(eng);
			const std::string name(symbols.get_string(namespace_id));
			std::shared_ptr<environment> parent = engine_access::get_global_environment(eng);
			if (const size_t cut = name.rfind("::"); cut != std::string::npos) {
				ns.enclosing = symbols.intern(std::string_view(name).substr(0, cut));
				auto& enclosing = engine_access::script_namespaces(eng)[ns.enclosing];
				if (!enclosing) { enclosing = std::make_shared<script_namespace_data>(); }
				parent = namespace_scope(eng, *enclosing, ns.enclosing);
			}
			ns.scope = std::make_shared<environment>(std::move(parent), &symbols);
			ns.scope->mark_namespace_scope();
		}
		return ns.scope;
	}

	// "a::b" for the object of `a::b::x`; empty when the chain is not identifiers joined by ::
	inline std::string static_path_of(const expression* object) {
		if (object->get_type() == node_type::identifier_expr) {
			return std::string(static_cast<const identifier_expr*>(object)->name);
		}
		if (object->get_type() == node_type::member_expr) {
			const auto* m = static_cast<const member_expr*>(object);
			if (m->is_static) {
				std::string head = static_path_of(m->object.get());
				return head.empty() ? head : head + "::" + std::string(m->member);
			}
		}
		return {};
	}

	struct namespace_store_target {
		script_value* cell = nullptr;   // the scope's own cell; null names a host-registered entry
		bool missing = false;           // the path names a namespace that has no such member
	};

	// `path::member` as a namespace member for a store: the scope's cell, a host entry (cell null), a namespace without the
	// member (missing: the read's undefined-name text), or nothing (the path names no namespace: the class ladder decides).
	// Reads only: no name is interned on this path.
	inline std::optional<namespace_store_target> resolve_namespace_store(engine& eng, const member_expr* member, const string_symbolizer& symbols) {
		const std::string path = static_path_of(member->object.get());
		if (path.empty()) {
			return std::nullopt;
		}
		const auto path_id = symbols.find(path);
		if (!path_id) {
			return std::nullopt;
		}
		auto& spaces = engine_access::script_namespaces(eng);
		auto ns_it = spaces.find(*path_id);
		if (ns_it == spaces.end() || !ns_it->second) {
			return std::nullopt;
		}
		script_namespace_data& ns = *ns_it->second;
		if (ns.scope) {
			if (script_value* cell = ns.scope->get_own_value_ptr(member->member_id)) {
				return namespace_store_target{cell};
			}
		}
		if (ns.variables.count(member->member_id) != 0) {
			return namespace_store_target{};
		}
		return namespace_store_target{nullptr, true};
	}

	// `path::member` naming nothing that an extern declares: the extern's own text (environment::undefined_text), else nullopt
	JAI_NOINLINE inline std::optional<error_propagator> qualified_extern_missing(const environment& env, std::string_view path, std::string_view member,
	                                                                           const string_symbolizer& symbols) {
		const auto id = symbols.find(std::string(path) + "::" + std::string(member));
		if (!id) { return std::nullopt; }
		const std::string_view text = env.undefined_text(*id, {});
		if (text.empty()) { return std::nullopt; }
		return error_propagator{make_error_code(runtime_error_code::undefined_variable), text, *id, 0};
	}

	JAI_NOINLINE inline std::optional<error_propagator> qualified_extern_missing(const environment& env, const member_expr* member, const string_symbolizer& symbols) {
		const std::string path = static_path_of(member->object.get());
		if (path.empty()) { return std::nullopt; }
		return qualified_extern_missing(env, path, member->member, symbols);
	}

	// `ns::x = v` where ns declares no x: the text a read of ns::x raises
	JAI_NOINLINE inline error_propagator namespace_member_missing(const environment& env, const member_expr* member, string_symbolizer& symbols) {
		if (auto declared = qualified_extern_missing(env, member, symbols)) { return *declared; }
		if (symbols.frozen()) {
			return error_propagator{make_error_code(runtime_error_code::undefined_variable), "Undefined variable", 0, 0};
		}
		return error_propagator{make_error_code(runtime_error_code::undefined_variable), "Undefined variable '{0}'",
			symbols.intern(static_path_of(member->object.get()) + "::" + std::string(member->member)), 0};
	}

} // namespace jai::detail

#endif
