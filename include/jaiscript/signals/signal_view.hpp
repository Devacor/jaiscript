#pragma once

#ifndef __JAISCRIPT_SIGNALS_SIGNAL_VIEW_HPP__
#define __JAISCRIPT_SIGNALS_SIGNAL_VIEW_HPP__

#include <string>
#include <jaiscript/core/value.hpp>

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
	// Lifetime: `source` is unowned. The view is only handed out as a member
	// read of a live bound object (same hazard class as any cpp_bound member
	// reference); a view outliving its owner dangles.
	//
	// Text receivers: connect(id, "script text") routes the binding engine
	// into the emitter first, so C++ emits can fire them. Named parameters
	// are not settable through the erased view — text receivers see the
	// emitter's existing parameter_names (function receivers are preferred).
	struct signal_script_ops {
		void* source = nullptr;
		void (*connect_fn)(void*, const std::string&, const script_value&, engine*) = nullptr;
		void (*disconnect_fn)(void*, const std::string&) = nullptr;
		bool (*connected_fn)(void*, const std::string&) = nullptr;

		bool valid() const { return source != nullptr && connect_fn != nullptr; }
	};

	// S = signal<Sig> or signal_emitter<Sig> (identical named surfaces).
	template<typename S>
	signal_script_ops make_signal_script_ops(S& sig) {
		signal_script_ops ops;
		ops.source = &sig;
		ops.connect_fn = +[](void* src, const std::string& id, const script_value& cb, engine* eng) {
			S& s = *static_cast<S*>(src);
			if (cb.is_string()) {
				if (eng) { s.script_engine(eng); }
				s.connect(id, cb.as_string());
			} else {
				s.connect(id, cb);
			}
		};
		ops.disconnect_fn = +[](void* src, const std::string& id) {
			static_cast<S*>(src)->disconnect(id);
		};
		ops.connected_fn = +[](void* src, const std::string& id) {
			return static_cast<S*>(src)->connected(id);
		};
		return ops;
	}

} // namespace jai

#endif
