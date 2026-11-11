#pragma once

#ifndef __JAISCRIPT_SIGNAL_PROPERTY_HPP__
#define __JAISCRIPT_SIGNAL_PROPERTY_HPP__

#include <jaiscript/signals/signal_serialization.hpp>
#include <jaiscript/signals/signal_view.hpp>
#include <jaiscript/properties/property.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/serialization/archive_impl.hpp>

namespace jai {

	// A property that serializes an externally-owned signal_emitter under a given name, routing
	// through the ADL save/load hooks. The emitter stays a plain signal_emitter on the owning
	// class (so emission via operator() is unchanged); this just lets it ride property_mgr's
	// save/load like any other property. Only script receivers serialize (C++ callbacks don't).
	template<typename T, typename Sync>
	class signal_property : public property_base {
		// The emitter and the facade are members of the same object as this property, found by their offsets from it:
		// a copied or moved owner's property serves its own emitter and points its own facade at it.
		std::int32_t m_emitter_offset;
		std::int32_t m_facade_offset;

		template<typename Member>
		static std::int32_t offset_to(const signal_property* a_self, const Member& a_member) {
			return static_cast<std::int32_t>(reinterpret_cast<const char*>(std::addressof(a_member)) - reinterpret_cast<const char*>(a_self));
		}
		signal_emitter<T, Sync>& emitter() const {
			return *reinterpret_cast<signal_emitter<T, Sync>*>(const_cast<char*>(reinterpret_cast<const char*>(this)) + m_emitter_offset);
		}
		signal<T, Sync>& facade() const {
			return *reinterpret_cast<signal<T, Sync>*>(const_cast<char*>(reinterpret_cast<const char*>(this)) + m_facade_offset);
		}
	public:
		// a_facade is declared before this property, so the copy or move of an owner has made it by the time this runs
		signal_property(property_manager& reg, std::string name, signal_emitter<T, Sync>& a_emitter, signal<T, Sync>& a_facade)
			: property_base(reg, std::move(name)), m_emitter_offset(offset_to(this, a_emitter)), m_facade_offset(offset_to(this, a_facade)) {}

		signal_property(const signal_property& a_other) noexcept
			: property_base(a_other), m_emitter_offset(a_other.m_emitter_offset), m_facade_offset(a_other.m_facade_offset) {
			facade().emitter_ = &emitter();
		}
		signal_property& operator=(const signal_property&) noexcept { return *this; }

		void serialize(serialization::any_archive_writer& ar) const override {
			ar.dispatch([this](auto& concrete_ar) {
				concrete_ar.serialize(name().c_str(), emitter());
			});
		}
		void serialize(serialization::any_archive_reader& ar) override {
			ar.dispatch([this](auto& concrete_ar) {
				concrete_ar.read_custom(emitter());
			});
		}
		void clone_to_target(property_base&) override {}

		// The generic "Signal" script view connects through this erased table,
		// so auto_bind exposes the property with no per-signature registration.
		bool signal_ops(signal_script_ops& ops) override {
			ops = make_signal_script_ops(emitter());
			return true;
		}
	};
}

// Declares a serializable, script-connectable signal as a private-emitter / public-view pair,
// plus a hidden property that serializes the emitter under "name":
//
//   private: member_signal_emitter<Sig, Policy> emitter;   // emit source + serialized state
//   public:  signal<Sig, Policy>                name;      // connect target (scripts / external code)
//
// Emit through the emitter: emitter(args). Requires the enclosing class to be a property_owner
// (have a property_mgr). Leaves member access at public:. A moved owner takes the receivers, a copied
// one starts with none, and either way name is its own emitter's facade. The schema entry (is_signal) lets
// dynamic_binder's auto_bind expose the generic Signal view — declaring the signal is the whole
// registration. JAI_SIGNAL_PROPERTY(Sig, name) names the emitter nameSignal. Both are unsynchronized;
// JAI_SYNCHRONIZED_SIGNAL_PROPERTY declares the jai::signal_synchronized pair for a signal used from several threads.
#define JAI_SIGNAL_PROPERTY_WITH_POLICY(signature, name, emitter, policy) \
	private: \
		inline static const int _jai_schema_reg_##name = []{ \
			using OwnerT = _jai_owner_type; \
			jai::type_registry::instance().for_type<OwnerT>().add( \
				jai::property_meta{ #name, std::type_index(typeid(jai::signal_emitter<signature, policy>)), true, false, true }); \
			return 0; \
		}(); \
		jai::member_signal_emitter<signature, policy> emitter; \
	public: \
		jai::signal<signature, policy> name{ emitter }; \
	private: \
		jai::signal_property<signature, policy> _jai_signalprop_##name{ property_mgr, #name, emitter, name }; \
	public: \
		using _jai_signal_declared_##name = void

#define JAI_SIGNAL_PROPERTY_AS(signature, name, emitter) JAI_SIGNAL_PROPERTY_WITH_POLICY(signature, name, emitter, jai::signal_unsynchronized)
#define JAI_SIGNAL_PROPERTY(signature, name) JAI_SIGNAL_PROPERTY_AS(signature, name, name##Signal)
#define JAI_SYNCHRONIZED_SIGNAL_PROPERTY_AS(signature, name, emitter) JAI_SIGNAL_PROPERTY_WITH_POLICY(signature, name, emitter, jai::signal_synchronized)
#define JAI_SYNCHRONIZED_SIGNAL_PROPERTY(signature, name) JAI_SYNCHRONIZED_SIGNAL_PROPERTY_AS(signature, name, name##Signal)

#endif
