#pragma once

#ifndef __JAISCRIPT_DETAIL_METHOD_VALUE_HPP__
#define __JAISCRIPT_DETAIL_METHOD_VALUE_HPP__

// A method is called or bound, never read bare: obj.method without a call raises this text on both
// backends and in the strict checker. A class instance, handle or host object binds with reflect::bind; a value type's
// (string, array, map, coroutine) methods run on the value, so a lambda wraps the call.

#include <jaiscript/detail/ast.hpp>
#include <string>
#include <string_view>

namespace jai::detail {

	// The receiver as written when it is a plain chain of names (player, this, team.lead), else "obj"
	inline std::string method_value_receiver(const member_expr* e) {
		std::string chain;
		const expression* cur = e ? e->object.get() : nullptr;
		while (cur) {
			if (cur->get_type() == node_type::identifier_expr) {
				const std::string head(static_cast<const identifier_expr*>(cur)->name);
				return chain.empty() ? head : head + "." + chain;
			}
			if (cur->get_type() == node_type::this_expr) { return chain.empty() ? std::string("this") : "this." + chain; }
			if (cur->get_type() != node_type::member_expr) { break; }
			const auto* m = static_cast<const member_expr*>(cur);
			if (m->is_static || m->null_safe) { break; }
			chain = chain.empty() ? std::string(m->member) : std::string(m->member) + "." + chain;
			cur = m->object.get();
		}
		return "obj";
	}

	inline std::string method_value_text(std::string_view member, std::string_view owner, const std::string& receiver, bool value_type) {
		std::string text = "'" + std::string(member) + "' is a method of " + std::string(owner) + "; call it with " + receiver + "." +
		                   std::string(member) + "()";
		if (value_type) { return text + " or wrap the call in a lambda"; }
		return text + " or bind it with reflect::bind(" + receiver + ", \"" + std::string(member) + "\")";
	}

	// A generic function or method, or one of its instances, named where a value stands (largest, largest<int>, Cfg::make<int>):
	// none is a value. `generic` is the generic's own name when `spelled` names an instance of it; `call` is what a lambda calls.
	inline std::string generic_value_text(std::string_view spelled, std::string_view generic, bool method, std::string_view call) {
		std::string text = "'" + std::string(spelled) + "' is " + (generic.empty() ? std::string("a generic ") : std::string("an instance of the generic "));
		text += method ? "method" : "function";
		if (!generic.empty()) { text += " '" + std::string(generic) + "'"; }
		return text + ", which is not a value: call it, or pass a lambda that does ((x) => " + std::string(call) + "(x))";
	}
	// The same through a receiver (p.conv, p.conv<int>): `bound` is the name reflect::bind takes
	inline std::string generic_method_value_text(std::string_view spelled, std::string_view generic, const std::string& receiver, std::string_view bound) {
		std::string text = "'" + std::string(spelled) + "' is " + (generic.empty() ? std::string("a generic method") : "an instance of the generic method '" + std::string(generic) + "'");
		return text + ", which is not a value: call it, or bind " + (generic.empty() ? "an instance" : "it") + " with reflect::bind(" + receiver + ", \"" + std::string(bound) + "\")";
	}

	// The same violation spelled bare inside a method (implicit this)
	inline std::string bare_method_value_text(std::string_view member, std::string_view owner) {
		return "'" + std::string(member) + "' is a method of " + std::string(owner) + "; call it with " + std::string(member) +
		       "() or bind it with reflect::bind(this, \"" + std::string(member) + "\")";
	}

}

#endif // __JAISCRIPT_DETAIL_METHOD_VALUE_HPP__
