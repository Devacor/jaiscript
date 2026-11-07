#pragma once

#ifndef JAISCRIPT_SIGNALS_SIGNAL_BINDING_HPP
#define JAISCRIPT_SIGNALS_SIGNAL_BINDING_HPP

// Heavy on purpose (dynamic_binder + engine) — include from registrar .cpp files only.

#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/signals/signal.hpp>
#include <jaiscript/signals/signal_impl.hpp>

namespace jai {

	// Emission rights are granted by WHICH TYPE a member exposes: signal<Sig> is the
	// non-emittable consumer view (connect/disconnect/connected only), signal_emitter<Sig>
	// adds emit(args...) so script can fire receivers connected in C++ or script.
	template<typename Sig, typename Sync>
	struct signal_emit_binder;

	template<typename R, typename... Args, typename Sync>
	struct signal_emit_binder<R(Args...), Sync> {
		static void add_emit(dynamic_binder<signal_emitter<R(Args...), Sync>>& a_builder) {
			a_builder.method("emit", [](signal_emitter<R(Args...), Sync>& a_self, Args... a_args) {
				a_self.emit(std::forward<Args>(a_args)...);
			});
		}
	};

	// Named connections are owned by the signal, so they persist for its lifetime.
	// Registers both jai::signal<Sig> (a_name) and jai::signal_emitter<Sig>
	// (a_name + "Emitter") — members are exposed as either. Sync is the emitter policy the bound types carry.
	template<typename Sig, typename Sync = signal_unsynchronized>
	void bind_signal_type(engine& a_engine, const std::string& a_name) {
		if (!a_engine.is_type_registered(a_name)) {
			dynamic_binder<signal<Sig, Sync>> builder(a_engine, a_name);
			builder.method("connect", [](signal<Sig, Sync>& a_self, const std::string& a_id, script_value a_callback) {
				if (a_callback.is_string()) {
					a_self.connect(a_id, a_callback.as_string());
				} else {
					a_self.connect(a_id, a_callback);
				}
			});
			builder.method("disconnect", [](signal<Sig, Sync>& a_self, const std::string& a_id) {
				a_self.disconnect(a_id);
			});
			builder.method("connected", [](signal<Sig, Sync>& a_self, const std::string& a_id) {
				return a_self.connected(a_id);
			});
			builder.serial_method("connect").serial_method("disconnect");   // a receiver list other objects' code reads and fires
			builder.build();
		}
		const std::string emitter_name = a_name + "Emitter";
		if (!a_engine.is_type_registered(emitter_name)) {
			dynamic_binder<signal_emitter<Sig, Sync>> builder(a_engine, emitter_name);
			builder.method("connect", [](signal_emitter<Sig, Sync>& a_self, const std::string& a_id, script_value a_callback) {
				if (a_callback.is_string()) {
					a_self.connect(a_id, a_callback.as_string());
				} else {
					a_self.connect(a_id, a_callback);
				}
			});
			builder.method("disconnect", [](signal_emitter<Sig, Sync>& a_self, const std::string& a_id) {
				a_self.disconnect(a_id);
			});
			builder.method("connected", [](signal_emitter<Sig, Sync>& a_self, const std::string& a_id) {
				return a_self.connected(a_id);
			});
			signal_emit_binder<Sig, Sync>::add_emit(builder);
			builder.serial_method("connect").serial_method("disconnect").serial_method("emit");   // emit runs other objects' receivers
			builder.build();
		}
	}

} // namespace jai

#endif // JAISCRIPT_SIGNALS_SIGNAL_BINDING_HPP
