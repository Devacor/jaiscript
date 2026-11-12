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
#include <vector>
#include <span>

#include <jaiscript/detail/static_type_name.hpp>
#include <jaiscript/core/binding_context.hpp>
#include <jaiscript/core/engine_ref.hpp>

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

// registrar<T, Ctx, bases<Base...>> (core/binding_context.hpp) declares the serialization base edge(s) inline so a
// shared_ptr<Base> carrying this $type passes the load type check.
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

// Which registrars one bind_registrar call runs: jai::only<A, B>() runs those, jai::except<A, B>() all but those
class registrar_filter {
public:
	enum class mode { only, except };

	registrar_filter(mode a_mode, std::vector<std::type_index> a_types) : filter_mode(a_mode), types(std::move(a_types)) {}

	[[nodiscard]] bool admits(std::type_index a_type) const {
		const bool listed = std::find(types.begin(), types.end(), a_type) != types.end();
		return filter_mode == mode::only ? listed : !listed;
	}
	[[nodiscard]] const std::vector<std::type_index>& listed() const { return types; }

private:
	mode filter_mode;
	std::vector<std::type_index> types;
};

template<typename... T>
[[nodiscard]] registrar_filter only() { return registrar_filter(registrar_filter::mode::only, {std::type_index(typeid(T))...}); }

template<typename... T>
[[nodiscard]] registrar_filter except() { return registrar_filter(registrar_filter::mode::except, {std::type_index(typeid(T))...}); }

namespace detail {
	// Registrations apply in script-name order, the same on every compiler; a type registered again replaces its entry.
	// run_all also runs every property_owner class that binds itself (property_manager.hpp) and that no registrar of
	// any context claims, merged into the same order; two types under one script name throw, naming both.
	// One type for every context (the context travels as const void*), defined in the library (class_registry.cpp).
	class registration_order {
	public:
		using erased_fn = std::function<void(engine&, const void* context)>;
		struct context_run {
			registration_order* order;
			const void* context;
			std::type_index context_type;
		};

		registration_order();
		~registration_order();

		void add(std::type_index ti, const std::string& name, erased_fn fn);
		// A filter that leaves out a class an admitted one derives from throws, naming both
		void run_all(engine& eng, const void* context, std::type_index context_type, const registrar_filter* filter = nullptr);
		// Several contexts' registries in order, the filter's types checked against all of them
		static void run_chain(engine& eng, std::span<const context_run> runs, const registrar_filter* filter);
		[[nodiscard]] bool has(std::type_index ti) const;

	private:
		static void check_filter(const registrar_filter* filter, std::span<const registration_order* const> orders);
		void run_checked(engine& eng, const context_run& run, std::span<const registration_order* const> chain, const registrar_filter* filter);

		std::multimap<std::string, std::pair<std::type_index, erased_fn>> entries_;
		std::unordered_map<std::type_index, std::string> names_;
	};

	// The spelling a host writes (engine.cpp): demangled under GCC and Clang, no class key under MSVC
	std::string readable_cpp_type_name(const std::type_index& type);
	[[noreturn]] void throw_registrar_renames_owner(std::type_index a_type, const std::string& a_name, std::string_view a_serialization_name);
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

    void register_all(engine& eng, const Context& ctx, const registrar_filter* filter = nullptr) {
        ordered_.run_all(eng, &ctx, std::type_index(typeid(Context)), filter);
    }

    [[nodiscard]] bool has(std::type_index ti) const {
        return ordered_.has(ti);
    }

    [[nodiscard]] detail::registration_order& order() { return ordered_; }

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

    void register_all(engine& eng, const registrar_filter* filter = nullptr) {
        ordered_.run_all(eng, nullptr, std::type_index(typeid(void)), filter);
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
// A property_owner class needs none: it binds itself under its serialization name in every bind_registrar call its
// jai::context<> tag admits, its static void jai_auto_bind(jai::dynamic_binder<T>&) (or (auto&), or with its context)
// binding the rest. A registrar for it adds to that binding in its context's engines, under the same name.
//
// At engine initialization:
//   jai::bind_registrar(engine);                     // No context
//   jai::bind_registrar(engine, ctx);                // With context, and every context ctx's jai_context_bases lists
//   jai::bind_registrar(engine, jai::except<EditorPanel>());   // every registrar but EditorPanel's (jai::only<...>: just those)
//   jai::bind_registrar(engine, ctx, jai::match_exact_context_type);   // ctx's own registrars alone

// Shared by the auto-named registrar constructors: the ONE naming ladder (self-owned
// jai_type_name pin, else the bare C++ type name — also the serialization $type, so the
// two stay in lockstep), or a compile error pointing at the explicit-name constructor
// when T has no portable name.
namespace detail {
	// A registrar of a property_owner class adds to its binding, under the name that is also its serialization $type
	template<typename T>
	void check_registrar_name(const std::string& a_name) {
		if constexpr (requires { typename T::_jai_owner_type; }) {
			if constexpr (std::is_same_v<typename T::_jai_owner_type, T>) {
				constexpr auto derived = declared_or_derived_type_name<T>();
				if constexpr (!derived.empty()) {
					if (a_name != derived) { throw_registrar_renames_owner(std::type_index(typeid(T)), a_name, derived); }
				}
			}
		}
	}

	// Runs T's jai_auto_bind(builder, const C&) where the registrar's context is or derives from C (dynamic_binder.hpp)
	template<typename T, typename Context>
	void run_registrar_context_hook(dynamic_binder<T>& a_builder, const Context& a_context);
}

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
template<typename T, typename ContextSpelling, typename BasesTag = bases<>>
class registrar {
    // registrar<T, const Ctx> and registrar<T, Ctx&> fill Ctx's registry
    using Context = std::remove_cvref_t<ContextSpelling>;

    static void register_simple(std::string name_str) {
        type_name_registry::instance().register_type<T>(name_str);
        serialization::try_auto_register<T>(name_str);
        register_polymorphic_bases<T>(BasesTag{});
        registrar_registry<Context>::instance().add(
            std::type_index(typeid(T)), name_str,
            [name_str](engine& eng, const Context& ctx) {
                detail::check_registrar_name<T>(name_str);
                dynamic_binder<T> builder(eng, name_str);
                detail::run_registrar_context_hook(builder, ctx);
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
                detail::check_registrar_name<T>(name_str);
                dynamic_binder<T> builder(eng, name_str);
                detail::run_registrar_context_hook(builder, ctx);
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
                detail::check_registrar_name<T>(name_str);
                dynamic_binder<T> builder(eng, name_str);
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
                detail::check_registrar_name<T>(name_str);
                dynamic_binder<T> builder(eng, name_str);
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

namespace detail {
	// A context declares the contexts it extends with using jai_context_bases = jai::bases<Base...>; each base's registry runs
	// first, with the context as that base, each registry once
	template<typename Context>
	void collect_context_runs(const Context& ctx, std::vector<std::type_index>& seen, std::vector<registration_order::context_run>& runs);

	template<typename Context, typename... Bases>
	void collect_base_context_runs(const Context& ctx, std::vector<std::type_index>& seen, std::vector<registration_order::context_run>& runs, bases<Bases...>) {
		(collect_context_runs<Bases>(static_cast<const Bases&>(ctx), seen, runs), ...);
	}

	template<typename Context>
	void collect_context_runs(const Context& ctx, std::vector<std::type_index>& seen, std::vector<registration_order::context_run>& runs) {
		if (std::find(seen.begin(), seen.end(), std::type_index(typeid(Context))) != seen.end()) { return; }
		seen.emplace_back(typeid(Context));
		if constexpr (requires { typename Context::jai_context_bases; }) {
			collect_base_context_runs(ctx, seen, runs, typename Context::jai_context_bases{});
		}
		runs.push_back({ &registrar_registry<Context>::instance().order(), &ctx, std::type_index(typeid(Context)) });
	}

	// bind_registrar's trailing options, any number in either order: a jai::only / jai::except filter, jai::match_exact_context_type
	struct bind_registrar_options {
		const registrar_filter* filter = nullptr;
		bool exact = false;
	};
	inline void add_bind_registrar_option(bind_registrar_options& a_options, const registrar_filter& a_filter) { a_options.filter = &a_filter; }
	inline void add_bind_registrar_option(bind_registrar_options& a_options, match_exact_context_type_t) { a_options.exact = true; }

	template<typename O>
	concept bind_registrar_option = std::is_same_v<std::remove_cvref_t<O>, registrar_filter> || std::is_same_v<std::remove_cvref_t<O>, match_exact_context_type_t>;

	template<bind_registrar_option... Options>
	bind_registrar_options fold_bind_registrar_options(const Options&... a_options) {
		bind_registrar_options result;
		(add_bind_registrar_option(result, a_options), ...);
		return result;
	}

	template<typename Context>
	void bind_context_registrars(engine& eng, const Context& ctx, const bind_registrar_options& options) {
		if constexpr (requires { typename Context::jai_context_bases; }) {
			if (!options.exact) {
				std::vector<std::type_index> seen;
				std::vector<registration_order::context_run> runs;
				collect_context_runs(ctx, seen, runs);
				registration_order::run_chain(eng, runs, options.filter);
				return;
			}
		}
		registrar_registry<Context>::instance().register_all(eng, ctx, options.filter);
	}
}

// The trailing options: a jai::only<...>() or jai::except<...>() filter and jai::match_exact_context_type, in either order
template<typename Context, detail::bind_registrar_option... Options>
    requires (!detail::bind_registrar_option<Context>)
void bind_registrar(engine_ref eng, const Context& ctx, const Options&... options) {
    detail::bind_context_registrars<std::remove_cvref_t<Context>>(*eng, ctx, detail::fold_bind_registrar_options(options...));
}

template<detail::bind_registrar_option... Options>
void bind_registrar(engine_ref eng, const Options&... options) {
    registrar_registry<void>::instance().register_all(*eng, detail::fold_bind_registrar_options(options...).filter);
}


} // namespace jai

#include <jaiscript/core/dynamic_binder.hpp>
// Include polymorphic registry for auto-registration of polymorphic types, plus the
// any_archive dispatch() definitions its save/load factories instantiate (see the note
// at the bottom of polymorphic.hpp).
#include <jaiscript/serialization/polymorphic.hpp>
#include <jaiscript/serialization/archive_dispatch.hpp>

#endif // __JAISCRIPT_CORE_REGISTRAR_HPP__
