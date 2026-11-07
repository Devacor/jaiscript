#pragma once

#ifndef __JAISCRIPT_CORE_REGISTRAR_HPP__
#define __JAISCRIPT_CORE_REGISTRAR_HPP__

#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <typeindex>
#include <string>
#include <string_view>
#include <optional>
#include <algorithm>
#include <type_traits>

#include <jaiscript/detail/static_type_name.hpp>

#include <concepts>

namespace jai {

// Forward declarations
class engine;
template<typename T> class dynamic_binder;

// polymorphic.hpp is included at the bottom of this header (it depends on archive types
// defined elsewhere, and archive_impl.hpp closes the loop by including it). Forward-declare
// the auto-register entry point here so the registrar bodies below can name it as a template;
// its definition rides in with that bottom include.
namespace serialization {
	template<typename T> void try_auto_register(const std::string& name);
	template<typename Derived, typename... Bases> struct register_relation;
}

// Optional polymorphic base marker: registrar<T, Ctx, bases<Base...>> declares the serialization
// base edge(s) inline so a shared_ptr<Base> carrying this $type passes the load type check.
template<typename... Bases>
struct bases {};

template<typename T, typename... Bs>
inline void register_polymorphic_bases(bases<Bs...>) {
	if constexpr (sizeof...(Bs) > 0) { serialization::register_relation<T, Bs...>{}; }
}

// ============================================================================
// Type Name Registry - Global mapping from type_index to script name
// ============================================================================

class type_name_registry {
public:
    [[nodiscard]] static type_name_registry& instance();

    void register_type(std::type_index ti, std::string_view name);

    template<typename T>
    void register_type(std::string_view name) {
        register_type(std::type_index(typeid(T)), name);
    }

    [[nodiscard]] std::optional<std::string_view> get_name(std::type_index ti) const;

    template<typename T>
    [[nodiscard]] std::optional<std::string_view> get_name() const {
        return get_name(std::type_index(typeid(T)));
    }

    [[nodiscard]] std::optional<std::type_index> get_type(const std::string& name) const;

    [[nodiscard]] bool has_name(std::type_index ti) const;

    template<typename T>
    [[nodiscard]] bool has_name() const {
        return has_name(std::type_index(typeid(T)));
    }

private:
    type_name_registry() = default;
    // Hashed on std::type_index: libstdc++'s type_info::before is not a strict weak order between internal-linkage and other types
    std::unordered_map<std::type_index, std::string> type_names_;
    std::unordered_map<std::string, std::optional<std::type_index>> name_to_type_;
};

// ============================================================================
// Registrar Registry - Stores registration functions per context type
// ============================================================================

namespace detail {
	// Registrations apply in script-name order, the same on every compiler; a type registered again replaces its entry.
	// One type for every context (the context travels as const void*), defined in the library (class_registry.cpp).
	class registration_order {
	public:
		using erased_fn = std::function<void(engine&, const void* context)>;

		registration_order();
		~registration_order();

		void add(std::type_index ti, const std::string& name, erased_fn fn);
		void run_all(engine& eng, const void* context);
		[[nodiscard]] bool has(std::type_index ti) const;

	private:
		std::multimap<std::string, std::pair<std::type_index, erased_fn>> entries_;
		std::unordered_map<std::type_index, std::string> names_;
	};
}

template<typename Context>
class registrar_registry {
public:
    using registration_fn = std::function<void(engine&, const Context&)>;

    [[nodiscard]] static registrar_registry& instance() {
        static registrar_registry registry;
        return registry;
    }

    void add(std::type_index ti, const std::string& name, registration_fn fn) {
        ordered_.add(ti, name, [fn = std::move(fn)](engine& eng, const void* context) { fn(eng, *static_cast<const Context*>(context)); });
    }

    void register_all(engine& eng, const Context& ctx) {
        ordered_.run_all(eng, &ctx);
    }

    [[nodiscard]] bool has(std::type_index ti) const {
        return ordered_.has(ti);
    }

private:
    registrar_registry() = default;
    detail::registration_order ordered_;
};

template<>
class registrar_registry<void> {
public:
    using registration_fn = std::function<void(engine&)>;

    [[nodiscard]] static registrar_registry& instance() {
        static registrar_registry registry;
        return registry;
    }

    void add(std::type_index ti, const std::string& name, registration_fn fn) {
        ordered_.add(ti, name, [fn = std::move(fn)](engine& eng, const void*) { fn(eng); });
    }

    void register_all(engine& eng) {
        ordered_.run_all(eng, nullptr);
    }

    [[nodiscard]] bool has(std::type_index ti) const {
        return ordered_.has(ti);
    }

private:
    registrar_registry() = default;
    detail::registration_order ordered_;
};

// ============================================================================
// jai::registrar - Type registration for JaiScript engine
// ============================================================================
//
// Usage (a HostContext is whatever the host hands every configure function; void for none):
//   static jai::registrar<Player, HostContext> _player("Player", [](auto& builder, const HostContext& ctx) { ... });
//
// Auto-named (the script name and serialization $type are derived from the C++ type, so
// you don't repeat the class name as a string — works for non-template classes):
//   static jai::registrar<Player, HostContext> _player;                                   // name = "Player"
//   static jai::registrar<Player, HostContext> _player([](auto& builder, const HostContext& ctx) { ... });
// Template instantiations have no portable derived name and must pass one explicitly
// (e.g. Point<int> -> "Pointi"); omitting it is a compile error directing you here.
//
// At engine initialization:
//   jai::bind_registrar(engine);                     // No context
//   jai::bind_registrar<HostContext>(engine, ctx);   // With context

// Shared by the auto-named registrar constructors: the ONE naming ladder (self-owned
// jai_type_name pin, else the bare C++ type name — also the serialization $type, so the
// two stay in lockstep), or a compile error pointing at the explicit-name constructor
// when T has no portable name.
template<typename T>
inline std::string registrar_auto_name() {
    constexpr auto derived = ::jai::detail::declared_or_derived_type_name<T>();
    static_assert(!derived.empty(),
        "jai::registrar<T, Context>: T has no portable auto-name (template instantiation, "
        "lambda, or local type). Pass an explicit name: jai::registrar<T, Context>(\"Name\").");
    return std::string(derived);
}

// ============================================================================
// C++17-compatible registrar (runtime name)
// ============================================================================

// Primary template - with context. Optional bases<...> tag declares polymorphic base relations
// inline: registrar<T, Ctx, bases<Base>>.
template<typename T, typename Context, typename BasesTag = bases<>>
class registrar {
    static void register_simple(std::string name_str) {
        type_name_registry::instance().register_type<T>(name_str);
        serialization::try_auto_register<T>(name_str);
        register_polymorphic_bases<T>(BasesTag{});
        registrar_registry<Context>::instance().add(
            std::type_index(typeid(T)), name_str,
            [name_str](engine& eng, const Context&) {
                dynamic_binder<T> builder(eng, name_str);
                builder.auto_bind();
                builder.build();
            }
        );
    }

    template<typename F>
    static void register_configured(std::string name_str, F&& configure) {
        type_name_registry::instance().register_type<T>(name_str);
        serialization::try_auto_register<T>(name_str);
        register_polymorphic_bases<T>(BasesTag{});
        registrar_registry<Context>::instance().add(
            std::type_index(typeid(T)), name_str,
            [name_str, configure = std::forward<F>(configure)](engine& eng, const Context& ctx) {
                dynamic_binder<T> builder(eng, name_str);
                builder.auto_bind();
                configure(builder, ctx);
                builder.build();
            }
        );
    }

public:
    explicit registrar(const char* name) { register_simple(std::string(name)); }

    registrar() { register_simple(registrar_auto_name<T>()); }

    template<typename F>
    registrar(const char* name, F&& configure) { register_configured(std::string(name), std::forward<F>(configure)); }

    // Auto-named + configuration lambda. enable_if keeps a string literal from binding
    // here instead of the explicit-name constructor above.
    template<typename F, std::enable_if_t<std::is_invocable_v<F&, dynamic_binder<T>&, const Context&>, int> = 0>
    explicit registrar(F&& configure) { register_configured(registrar_auto_name<T>(), std::forward<F>(configure)); }
};

// Specialization for no context
template<typename T, typename BasesTag>
class registrar<T, void, BasesTag> {
    static void register_simple(std::string name_str) {
        type_name_registry::instance().register_type<T>(name_str);
        serialization::try_auto_register<T>(name_str);
        register_polymorphic_bases<T>(BasesTag{});
        registrar_registry<void>::instance().add(
            std::type_index(typeid(T)), name_str,
            [name_str](engine& eng) {
                dynamic_binder<T> builder(eng, name_str);
                builder.auto_bind();
                builder.build();
            }
        );
    }

    template<typename F>
    static void register_configured(std::string name_str, F&& configure) {
        type_name_registry::instance().register_type<T>(name_str);
        serialization::try_auto_register<T>(name_str);
        register_polymorphic_bases<T>(BasesTag{});
        registrar_registry<void>::instance().add(
            std::type_index(typeid(T)), name_str,
            [name_str, configure = std::forward<F>(configure)](engine& eng) {
                dynamic_binder<T> builder(eng, name_str);
                builder.auto_bind();
                configure(builder);
                builder.build();
            }
        );
    }

public:
    explicit registrar(const char* name) { register_simple(std::string(name)); }

    registrar() { register_simple(registrar_auto_name<T>()); }

    template<typename F>
    registrar(const char* name, F&& configure) { register_configured(std::string(name), std::forward<F>(configure)); }

    // Auto-named + configuration lambda. enable_if keeps a string literal from binding
    // here instead of the explicit-name constructor above.
    template<typename F, std::enable_if_t<std::is_invocable_v<F&, dynamic_binder<T>&>, int> = 0>
    explicit registrar(F&& configure) { register_configured(registrar_auto_name<T>(), std::forward<F>(configure)); }
};

// ============================================================================
// bind_registrar - Trigger all registered bindings
// ============================================================================

template<typename Context>
void bind_registrar(engine& eng, const Context& ctx) {
    registrar_registry<Context>::instance().register_all(eng, ctx);
}

inline void bind_registrar(engine& eng) {
    registrar_registry<void>::instance().register_all(eng);
}

// shared_ptr / raw-pointer convenience (engine::make() returns a shared_ptr)
template<typename Context>
void bind_registrar(const std::shared_ptr<engine>& eng, const Context& ctx) { bind_registrar(*eng, ctx); }
template<typename Context>
void bind_registrar(engine* eng, const Context& ctx) { bind_registrar(*eng, ctx); }
inline void bind_registrar(const std::shared_ptr<engine>& eng) { bind_registrar(*eng); }
inline void bind_registrar(engine* eng) { bind_registrar(*eng); }

} // namespace jai

#include <jaiscript/core/dynamic_binder.hpp>
// Include polymorphic registry for auto-registration of polymorphic types, plus the
// any_archive dispatch() definitions its save/load factories instantiate (see the note
// at the bottom of polymorphic.hpp).
#include <jaiscript/serialization/polymorphic.hpp>
#include <jaiscript/serialization/archive_dispatch.hpp>

#endif // __JAISCRIPT_CORE_REGISTRAR_HPP__
