#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <jaiscript/properties/property.hpp>
#include <jaiscript/properties/property_schema.hpp>
#include <jaiscript/core/script_extensible.hpp>
#include <jaiscript/signals/receiver_owner.hpp>
#include <jaiscript/detail/first_type.hpp>
#include <jaiscript/detail/static_type_name.hpp>
#include <jaiscript/core/binding_context.hpp>
#include <jaiscript/core/declared_methods.hpp>
#include <typeindex>

namespace jai {
	// Forward declarations
	class engine;

	namespace detail {
		// A property_owner class with a portable name queues its binding at program start; every bind_registrar runs the
		// queue beside its context's registrars. bind_declared_class is defined with dynamic_binder: the file that
		// instantiates the class's constructor includes core/dynamic_binder.hpp, or the link names the missing binding.
		// The context pointer is the bind_registrar call's, as the class's context type; null for an untagged class
		using declared_binding_fn = void (*)(engine&, const void* a_context);
		void queue_declared_binding(std::type_index a_type, std::string_view a_name, std::type_index a_context, declared_binding_fn a_bind);
		template<typename T> void bind_declared_class(engine& a_engine, const void* a_context);

		template<typename T>
		bool queue_declared_binding_of() {
			constexpr auto name = declared_or_derived_type_name<T>();
			if constexpr (!name.empty()) {
				queue_declared_binding(std::type_index(typeid(T)), name, std::type_index(typeid(typename T::_jai_context_type)), &bind_declared_class<T>);
			}
			return true;
		}

		// A variable template, not a static member: MSVC instantiates a class template's inline static members wherever the
		// class is complete, and this one instantiates the class's whole binding
		template<typename T>
		inline const bool declared_binding_queued = queue_declared_binding_of<T>();
	}

	namespace serialization {
		class any_archive_writer;
		class any_archive_reader;
		// Defined in serialization/polymorphic.hpp, included at the bottom of this header
		// (same pattern as registrar.hpp): implicit polymorphic registration for
		// property_owner types, and the base-pointer adjuster registration for MI.
		template<typename T> void try_auto_register_implicit();
		template<typename Derived, typename... Bases> struct register_relation;
	}

	class property_manager {
	public:
		property_manager();
		~property_manager();

		property_manager(property_manager&&) = delete;
		property_manager& operator=(property_manager&&) = delete;
		property_manager(const property_manager&) = delete;
		property_manager& operator=(const property_manager&) = delete;

		// The manager of an object copied or moved from a_source's owner: each property a_source registered inside the
		// a_ownerSize bytes at a_sourceOwner is registered at the same offset from this manager, where the owner's copy
		// or move constructs it afterwards. The script receivers stay with a_source.
		property_manager(const property_manager& a_source, const void* a_sourceOwner, size_t a_ownerSize) noexcept;

		void add(property_base* prop);
		// in name order; an entry holds no copy of its property's name
		std::span<property_base* const> all() const { return { m_properties, m_count }; }

		// Track a script-created receiver - keeps it alive for the lifetime of the property_manager
		template<typename T>
		std::shared_ptr<receiver<T>> track_receiver(std::shared_ptr<receiver<T>> recv) {
			return script_receivers_.track(std::move(recv));
		}

		receiver_owner& script_receivers() { return script_receivers_; }

		template<typename Fn>
		void visit(Fn&& func) const {
			for (property_base* prop : all()) {
				func(prop->name(), prop);
			}
		}

		property_base* get(const std::string& key) const;

		template<typename T>
		inline property<T>* get(const std::string& key) const;

		template<typename T>
		inline T* get_value(const std::string& key) const;

		// Internally wraps in any_archive_writer/reader for polymorphic property dispatch
		template<typename Archive>
		void save(Archive& ar) const;

		template<typename Archive>
		void load(Archive& ar);

		void clone_to_target(property_manager& target) const;

	private:
		size_t lower_bound(std::string_view a_name) const;

		property_base** m_properties = nullptr;
		std::uint32_t m_count = 0;
		std::uint32_t m_capacity : 31 = 0;
		// made by the relocating constructor: an entry may name a property its owner has yet to construct
		std::uint32_t m_relocated : 1 = 0;
		receiver_owner script_receivers_;  // Holds receivers created by script callbacks
	};

	inline property_base::property_base(property_manager& property_register, std::string name)
		: property_base(std::move(name)) {
		property_register.add(this);
	}

	template<typename T>
	inline property<T>* property_manager::get(const std::string& key) const {
		auto base = get(key);
		return base ? dynamic_cast<property<T>*>(base) : nullptr;
	}

	template<typename T>
	inline T* property_manager::get_value(const std::string& key) const {
		if (auto prop = get<T>(key)) {
			return &prop->get();
		}
		return nullptr;
	}

	// ============================================================================
	// Property Owner (CRTP with optional inheritance tracking)
	// ============================================================================
	//
	// Unified property owner class. Use CRTP pattern by passing your class
	// as the first template parameter. Additional bases are optional.
	//
	// Example:
	//   // Simple class (no inheritance)
	//   class Entity : public property_owner<Entity> {
	//       JAI_PROPERTY((int), health);
	//   };
	//
	//   // With inheritance tracking
	//   class Player : public property_owner<Player, Entity> {
	//       JAI_PROPERTY((std::string), name);
	//   };
	//
	// The type registry will track inheritance and include base properties
	// when serializing derived classes.
	//
	// Signal receiver tracking:
	//   property_owner includes a receiver_owner for automatic signal cleanup.
	//   Use track() to tie receiver lifetime to the object:
	//
	//   class Player : public property_owner<Player> {
	//   public:
	//       Player(signal<void(int)>& damage_signal) {
	//           track(damage_signal.connect([this](int dmg) {
	//               // handle damage - auto-disconnects when Player dies
	//           }));
	//       }
	//   };

	namespace detail {
		// Whether a class can copy or move its base subobject of type Base. No trait constructs an abstract class, so
		// for one its assignment stands in: a class gives up both or neither.
		template<typename Base>
		inline constexpr bool base_copy_constructible_v = std::is_abstract_v<Base> ? std::is_copy_assignable_v<Base> : std::is_copy_constructible_v<Base>;
		template<typename Base>
		inline constexpr bool base_move_constructible_v = std::is_abstract_v<Base> ? std::is_move_assignable_v<Base> : std::is_move_constructible_v<Base>;
	}

	// Every property_owner is script_extensible (docs/site/embedding/13-script-extensible.html): the slot comes
	// from the first extensible base when one exists (so multiple property_owner bases never
	// leave two ambiguous slots), else from deriving the mixin here.
	// One jai::context<Ctx> among Bases tags the level (core/binding_context.hpp); it is no base, and every use of the bases below
	// leaves it out.
	template<typename Derived, typename... Bases>
	class property_owner : public detail::owner_base_t<Derived, Bases>...,
	                       public std::conditional_t<detail::any_script_extensible_v<Bases...>, detail::no_script_slot<Derived>, script_extensible> {
		static_assert((detail::is_context_tag<Bases>::value + ... + 0) <= 1, "jai::property_owner: a level takes one jai::context<>");
		using _jai_declared_context = typename detail::declared_context<Bases...>::type;
		using _jai_inherited_context = typename detail::inherited_context<void, Bases...>::type;
		static_assert(!std::is_void_v<_jai_declared_context> || !std::is_same_v<_jai_inherited_context, detail::conflicting_contexts>,
			"jai::property_owner: its bases carry different contexts; tag the class with a jai::context<C> whose jai_context_bases lists each");
		static_assert(std::is_void_v<_jai_declared_context> || detail::narrows_every_base_context_v<_jai_declared_context, Bases...>,
			"jai::property_owner: a jai::context<C> may only narrow its bases' context: C must be that context or list it in jai_context_bases");
		static constexpr bool _jai_tagged_level = !std::is_void_v<_jai_declared_context>;

		template<typename... Reals>
		static void add_base_edges(std::tuple<Reals...>*) {
			(type_registry::instance().add_base(std::type_index(typeid(Derived)), std::type_index(typeid(Reals))), ...);
		}
		template<typename... Reals>
		static void register_base_relations(std::tuple<Reals...>*) {
			if constexpr (sizeof...(Reals) > 0) { serialization::register_relation<Derived, Reals...>{}; }
		}

	public:
		using _jai_owner_type = Derived;
		using _jai_base_types = detail::real_bases_t<Bases...>;
		using _jai_context_type = detail::effective_context_t<_jai_declared_context, _jai_inherited_context>;
		static constexpr size_t _jai_base_count = std::tuple_size_v<_jai_base_types>;
		// each level's JAI_METHOD slots count from 0: this hides its bases' counters (core/declared_methods.hpp)
		static detail::method_index<0> jai_method_counter(detail::method_rank<0>);

		script_extensible& jai_script_slot() {
			if constexpr (detail::any_script_extensible_v<Bases...>) {
				return static_cast<detail::first_script_extensible_t<Bases...>&>(*this);
			} else {
				return static_cast<script_extensible&>(*this);
			}
		}
		const script_extensible& jai_script_slot() const {
			if constexpr (detail::any_script_extensible_v<Bases...>) {
				return static_cast<const detail::first_script_extensible_t<Bases...>&>(*this);
			} else {
				return static_cast<const script_extensible&>(*this);
			}
		}
		strong_ptr<class_instance> script_layer() const { return jai_script_slot().script_layer(); }
		bool has_script_layer() const { return jai_script_slot().has_script_layer(); }

		property_manager property_mgr;

		property_manager& reflection() { return property_mgr; }
		const property_manager& reflection() const { return property_mgr; }

		static const type_property_schema& schema() {
			return type_registry::instance().for_type<Derived>();
		}

		static std::vector<std::string> all_property_names() {
			return type_registry::instance().all_property_names<Derived>();
		}

		property_owner() {
			register_inheritance();
		}

		// one argument that is this owner itself is a copy or a move, never the base's constructor argument
		template<typename... Args>
		static constexpr bool _jai_is_relocation_v = sizeof...(Args) == 1 && (std::is_convertible_v<std::remove_reference_t<Args>*, const property_owner*> && ...);

		template<typename... Args>
		explicit property_owner(Args&&... args) requires (_jai_base_count == 1 && !_jai_is_relocation_v<Args...>)
			: std::tuple_element_t<0, detail::real_bases_t<Bases...>>(std::forward<Args>(args)...) {
			register_inheritance();
		}

		template<typename... Args>
		explicit property_owner(Args&&... args) requires (!_jai_tagged_level && sizeof...(Bases) > 1 && sizeof...(Bases) == sizeof...(Args))
			: Bases(std::forward<Args>(args))... {
			register_inheritance();
		}

	private:
		// Serialization identity, implicitly: every property_owner type registers its
		// class name with the polymorphic registry at program start (odr-used below, so
		// the entry exists before main — load factories must be ready before the first
		// construction). An explicit jai::registrar<T, Ctx>("Name") always takes
		// precedence; use it for custom names, template types, or a binary that loads a
		// type it never constructs (only construction odr-uses this member).
		static inline const bool _jai_poly_registered = [] {
			serialization::try_auto_register_implicit<Derived>();
			// Record Derived->Base pointer adjusters so a shared_ptr<Base> whose dynamic
			// type is Derived reconstructs the correct Base subobject under multiple
			// inheritance (each base is also a property_owner and records its own bases,
			// so adjust_to_base composes them transitively).
			register_base_relations(static_cast<detail::real_bases_t<Bases...>*>(nullptr));
			return true;
		}();

	public:
		// The schema's base chain is compile-time knowledge (the CRTP Bases pack), so it
		// must not wait for first construction: every dynamic_binder of the class calls this at
		// REGISTRATION time so schema-driven binding sees inherited properties before any
		// instance exists; constructors keep calling it too (idempotent magic static).
		// The class's own binding queues from here, at the same points.
		static void register_inheritance() {
			(void)_jai_poly_registered;
			(void)detail::declared_binding_queued<Derived>;
			static const auto _inheritance_registered = []() {
				add_base_edges(static_cast<detail::real_bases_t<Bases...>*>(nullptr));
				return true;
			}();
			(void)_inheritance_registered;
		}

		virtual ~property_owner() = default;

		// Post-deserialization hook (called after all objects are deserialized)
		// Override in derived classes to resolve cross-references, update caches, etc.
		// Uses type-erased archive for polymorphic dispatch
		virtual void post_load(serialization::any_archive_reader& ar) {
			if constexpr (sizeof...(Bases) > 0) {
				(try_post_load_base<Bases>(ar, 0), ...);
			}
		}

		template<typename Archive>
		void load_with_hook(Archive& ar) {
			load_owned_properties(ar);
			detail::first_type_t<serialization::any_archive_reader, Archive> type_erased(ar);
			post_load(type_erased);
		}

		// --- Inheritance-aware property serialization -----------------------------------------
		// property_owner<Derived, Bases...> holds ONE property_mgr per inheritance level, and the
		// derived level name-hides the bases'. These walk the whole chain (each base is itself a
		// property_owner, so it recurses, as post_load does) so a derived object's
		// INHERITED properties are serialized too; without them only the most-derived level survives.

		// Visit every level's properties (bases first, then own) — the single save walk.
		// Same-name properties across levels follow C++ name-hiding: the DERIVED level wins
		// and the shadowed base entry is skipped. The flat save format has one slot per name;
		// without this, duplicate keys corrupted opposite levels in JSON vs binary loads.
		template<typename Fn>
		void visit_owned_properties(Fn&& fn) const {
			std::vector<std::pair<const std::string*, property_base*>> entries;
			collect_owned_properties(entries);
			for (size_t i = 0; i < entries.size(); ++i) {
				bool shadowed = false;
				for (size_t j = i + 1; j < entries.size(); ++j) {
					if (*entries[j].first == *entries[i].first) {
						shadowed = true;
						break;
					}
				}
				if (!shadowed) {
					fn(*entries[i].first, entries[i].second);
				}
			}
		}

		void collect_owned_properties(std::vector<std::pair<const std::string*, property_base*>>& out) const {
			if constexpr (sizeof...(Bases) > 0) {
				(try_collect_base<Bases>(out, 0), ...);
			}
			for (property_base* prop : property_mgr.all()) {
				out.emplace_back(&prop->name(), prop);
			}
		}

		// Save: one type-erased wrap, then write every level's savable properties into the current
		// (already-open, flat) object.
		template<typename Archive>
		void save_owned_properties(Archive& ar) const {
			detail::first_type_t<serialization::any_archive_writer, Archive> type_erased(ar);
			visit_owned_properties([&type_erased](const std::string&, property_base* prop) {
				if (prop->allow_save()) {
					prop->serialize(type_erased);
				}
			});
			if constexpr (requires(serialization::any_archive_writer& a_writer) { ar.write_bound_properties(a_writer, nullptr); }) {
				ar.write_bound_properties(type_erased, nullptr);
			}
		}

		// Load (concrete archive): each level seeks its own properties by name from the same flat
		// object — property_manager::load is re-entrant by design (see its comment), so call it per level.
		template<typename Archive>
		void load_owned_properties(Archive& ar) {
			if constexpr (sizeof...(Bases) > 0) {
				(try_load_base<Bases>(ar, 0), ...);
			}
			property_mgr.load(ar);
		}

		// Find a property by name anywhere in the chain (the type-erased load reads names
		// sequentially and must resolve base-level properties too).
		property_base* find_owned_property(const std::string& name) {
			if (auto* p = property_mgr.get(name)) {
				return p;
			}
			property_base* found = nullptr;
			if constexpr (sizeof...(Bases) > 0) {
				((found = found ? found : try_find_base<Bases>(name, 0)), ...);
			}
			return found;
		}

		const property_base* find_owned_property(const std::string& name) const {
			return const_cast<property_owner*>(this)->find_owned_property(name);
		}

		// --- Runtime reflection helpers (tooling: inspectors, debuggers) ------------------------
		// Typed access by name over the whole inheritance chain, built on property_base's erased
		// bridge. Writes route through erased_assign, so observable_property change signals fire.

		// Virtual twins of visit_owned_properties/find_owned_property: every property_owner level
		// re-overrides these, so a caller holding a BASE pointer still reflects over the most
		// derived type's full chain (the non-virtual walkers are bound to the static type).
		virtual void visit_reflected_properties(const std::function<void(const std::string&, property_base*)>& fn) const {
			visit_owned_properties(fn);
		}
		virtual property_base* find_reflected_property(const std::string& name) {
			return find_owned_property(name);
		}

		// Whole-chain clone through the virtual walkers: a base-typed caller still clones the
		// most-derived object, and each property applies its own clone (custom lambdas honoured).
		void clone_reflected_to(property_owner& target) const {
			visit_reflected_properties([&target](const std::string& name, property_base* prop) {
				auto* dest = target.find_reflected_property(name);
				if (dest && dest->value_type_id() == prop->value_type_id()) {
					prop->clone_to_target(*dest);
				}
			});
		}

		std::type_index property_type_id(const std::string& name) const {
			auto* p = find_owned_property(name);
			return p ? p->value_type_id() : std::type_index(typeid(void));
		}

		template<typename T>
		T* property_value(const std::string& name) {
			auto* p = find_owned_property(name);
			return p ? p->template value_as<T>() : nullptr;
		}

		template<typename T>
		const T* property_value(const std::string& name) const {
			auto* p = find_owned_property(name);
			return p ? p->template value_as<T>() : nullptr;
		}

		template<typename T>
		bool set_property_value(const std::string& name, const T& value) {
			auto* p = find_owned_property(name);
			return p && p->assign_value(value);
		}

	protected:
		receiver_owner receivers_;

		template<typename T>
		std::shared_ptr<receiver<T>> track(std::shared_ptr<receiver<T>> recv) {
			return receivers_.track(std::move(recv));
		}

		receiver_owner& receivers() { return receivers_; }
		const receiver_owner& receivers() const { return receivers_; }

		// Copies and moves, level by level. The new owner's property_mgr registers the properties Derived declares where
		// they sit in the new object, and Derived's own copy or move constructs them there with their values. A copy has
		// no receivers; a move takes the tracked ones, script receivers included, and leaves the source registered,
		// destructible and assignable. Assignment leaves every registration alone: Derived's members assign the values,
		// and copy assignment first clones each property into its counterpart, which is all a hand-written Derived
		// assignment that calls only this one gets.
		using _jai_script_slot_base = std::conditional_t<detail::any_script_extensible_v<Bases...>, detail::no_script_slot<Derived>, script_extensible>;

		property_owner(const property_owner& a_other) requires (detail::base_copy_constructible_v<Bases> && ...)
			: detail::owner_base_t<Derived, Bases>(static_cast<const detail::owner_base_t<Derived, Bases>&>(a_other))...
			, _jai_script_slot_base()
			, property_mgr(a_other.property_mgr, static_cast<const Derived*>(&a_other), sizeof(Derived)) {
		}

		property_owner(property_owner&& a_other) noexcept requires (detail::base_move_constructible_v<Bases> && ...)
			: detail::owner_base_t<Derived, Bases>(std::move(static_cast<detail::owner_base_t<Derived, Bases>&>(a_other)))...
			, _jai_script_slot_base()
			, property_mgr(a_other.property_mgr, static_cast<const Derived*>(&a_other), sizeof(Derived))
			, receivers_(std::move(a_other.receivers_)) {
			property_mgr.script_receivers() = std::move(a_other.property_mgr.script_receivers());
		}

		property_owner& operator=(const property_owner& a_other) requires (std::is_copy_assignable_v<Bases> && ...) {
			if (this != &a_other) {
				(static_cast<void>(static_cast<detail::owner_base_t<Derived, Bases>&>(*this) = static_cast<const detail::owner_base_t<Derived, Bases>&>(a_other)), ...);
				a_other.property_mgr.clone_to_target(property_mgr);
			}
			return *this;
		}

		property_owner& operator=(property_owner&& a_other) noexcept requires (std::is_move_assignable_v<Bases> && ...) {
			if (this != &a_other) {
				(static_cast<void>(static_cast<detail::owner_base_t<Derived, Bases>&>(*this) = std::move(static_cast<detail::owner_base_t<Derived, Bases>&>(a_other))), ...);
				property_mgr.script_receivers() = std::move(a_other.property_mgr.script_receivers());
				receivers_ = std::move(a_other.receivers_);
			}
			return *this;
		}

	private:
		// Each base level's walk; the long overloads skip a base that is no level (and the context tag)
		template<typename Base>
		auto try_post_load_base(serialization::any_archive_reader& ar, int)
			-> decltype(static_cast<Base*>(this)->post_load(ar), void()) {
			static_cast<Base*>(this)->post_load(ar);
		}
		template<typename Base>
		void try_post_load_base(serialization::any_archive_reader&, long) {}

		template<typename Base>
		auto try_collect_base(std::vector<std::pair<const std::string*, property_base*>>& out, int) const
			-> decltype(static_cast<const Base*>(this)->collect_owned_properties(out), void()) {
			static_cast<const Base*>(this)->collect_owned_properties(out);
		}
		template<typename Base>
		void try_collect_base(std::vector<std::pair<const std::string*, property_base*>>&, long) const {}

		template<typename Base, typename Archive>
		auto try_load_base(Archive& ar, int)
			-> decltype(static_cast<Base*>(this)->load_owned_properties(ar), void()) {
			static_cast<Base*>(this)->load_owned_properties(ar);
		}
		template<typename Base, typename Archive>
		void try_load_base(Archive&, long) {}

		template<typename Base>
		auto try_find_base(const std::string& name, int)
			-> decltype(static_cast<Base*>(this)->find_owned_property(name)) {
			return static_cast<Base*>(this)->find_owned_property(name);
		}
		template<typename Base>
		property_base* try_find_base(const std::string&, long) { return nullptr; }
	};

} // namespace jai

// Implicit polymorphic registration rides in with the registry definition (same pattern
// as registrar.hpp): forward-declared above, defined here. archive_dispatch.hpp supplies
// the any_archive dispatch() bodies the registration factories instantiate (see the note
// at the bottom of polymorphic.hpp).
#include <jaiscript/serialization/polymorphic.hpp>
#include <jaiscript/serialization/archive_dispatch.hpp>

// property<T>'s serialize overrides, after both classes they use are complete, whichever header came first
#include <jaiscript/properties/property_serialization.hpp>
