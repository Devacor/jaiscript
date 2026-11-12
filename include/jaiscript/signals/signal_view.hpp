#pragma once

#ifndef __JAISCRIPT_SIGNALS_SIGNAL_VIEW_HPP__
#define __JAISCRIPT_SIGNALS_SIGNAL_VIEW_HPP__

#include <memory>
#include <string>
#include <jaiscript/core/value.hpp>
#include <jaiscript/signals/signal_decl.hpp>

namespace jai {

	class engine;

	// Type-erased script surface over any signal<Sig> / signal_emitter<Sig>.
	// The engine binds ONE generic "Signal" view class over this table, so
	// signal members reach script with no per-signature bind_signal_type
	// ceremony (that path still works and takes precedence when registered).
	// The thunks instantiate wherever make_signal_script_ops is called —
	// sites that already compile the emitter (signal_property.hpp,
	// dynamic_binder.hpp) — never in signal_decl-only translation units.
	//
	// Lifetime: `source` shares the signal's state, so the view follows its
	// emitter through moves; once the emitter is gone connect_fn connects
	// nothing and returns false.
	//
	// Text receivers: connect(id, "script text") routes the binding engine
	// into the emitter first, so C++ emits can fire them. Named parameters
	// are not settable through the erased view — text receivers see the
	// emitter's existing parameter_names (function receivers are preferred).
	struct signal_script_ops {
		std::shared_ptr<void> source;
		bool (*connect_fn)(void*, const std::string&, const script_value&, engine*) = nullptr;
		void (*disconnect_fn)(void*, const std::string&) = nullptr;
		bool (*connected_fn)(void*, const std::string&) = nullptr;

		bool valid() const { return source != nullptr && connect_fn != nullptr; }
	};

	// S = signal<Sig>, signal_emitter<Sig> or member_signal_emitter<Sig>.
	template<typename S>
	signal_script_ops make_signal_script_ops(S& sig) {
		const auto& state = detail::signal_state_access::state(sig);
		using state_type = typename std::remove_cvref_t<decltype(state)>::element_type;
		signal_script_ops ops;
		ops.source = state;
		ops.connect_fn = +[](void* src, const std::string& id, const script_value& cb, engine* eng) {
			state_type& s = *static_cast<state_type*>(src);
			if (s.retired()) {
				return false;
			}
			if (cb.is_string()) {
				if (eng) { s.script_engine(eng); }
				s.connect(id, cb.as_string(), false);
			} else {
				s.connect(id, cb, false);
			}
			return true;
		};
		ops.disconnect_fn = +[](void* src, const std::string& id) {
			static_cast<state_type*>(src)->disconnect(id);
		};
		ops.connected_fn = +[](void* src, const std::string& id) {
			return static_cast<state_type*>(src)->connected(id);
		};
		return ops;
	}

} // namespace jai

#endif
