#pragma once

#ifndef __JAISCRIPT_CORE_BINDING_CONTEXT_HPP__
#define __JAISCRIPT_CORE_BINDING_CONTEXT_HPP__

#include <cstddef>
#include <tuple>
#include <type_traits>

namespace jai {

// A list of base types: registrar<T, Ctx, bases<Base...>> declares serialization base edges, and a context's
// using jai_context_bases = jai::bases<Base...> declares the contexts it extends
template<typename... Bases>
struct bases {};

// property_owner<Derived, Bases..., jai::context<Ctx>>: the class binds only in a bind_registrar call whose context is Ctx or
// lists Ctx in its jai_context_bases, recursively. A subclass inherits its base's context and may only narrow it.
template<typename Context>
struct context {
	using type = Context;
};

// bind_registrar(eng, ctx, jai::match_exact_context_type): only ctx's own registry, and of the tagged classes only those tagged
// with exactly its type; untagged classes still bind
struct match_exact_context_type_t {};
inline constexpr match_exact_context_type_t match_exact_context_type{};

namespace detail {
	template<typename T>
	struct is_context_tag : std::false_type {};
	template<typename C>
	struct is_context_tag<context<C>> : std::true_type {};

	template<typename T>
	struct context_bases_of {
		using type = bases<>;
	};
	template<typename T>
		requires requires { typename T::jai_context_bases; }
	struct context_bases_of<T> {
		using type = typename T::jai_context_bases;
	};

	template<typename Context, typename Required>
	struct context_extends;

	template<typename Required, typename BasesTag>
	struct any_context_extends;
	template<typename Required, typename... ContextBases>
	struct any_context_extends<Required, bases<ContextBases...>> : std::bool_constant<(context_extends<ContextBases, Required>::value || ...)> {};

	// Context is Required, or lists it in jai_context_bases, recursively; every context extends no context (void)
	template<typename Context, typename Required>
	struct context_extends : std::bool_constant<std::is_void_v<Required> || std::is_same_v<Context, Required> ||
		any_context_extends<Required, typename context_bases_of<Context>::type>::value> {};

	template<typename Context, typename Required>
	inline constexpr bool context_extends_v = context_extends<Context, Required>::value;

	// A property_owner level's bases with its context tag left out
	template<typename... Args>
	using real_bases_t = decltype(std::tuple_cat(std::declval<std::conditional_t<is_context_tag<Args>::value, std::tuple<>, std::tuple<Args>>>()...));

	template<typename T>
	struct context_of_tag {
		using type = void;
	};
	template<typename C>
	struct context_of_tag<context<C>> {
		using type = C;
	};

	template<typename Found, typename Next>
	using first_non_void_t = std::conditional_t<std::is_void_v<Found>, Next, Found>;

	template<typename... Args>
	struct declared_context {
		using type = void;
	};
	template<typename First, typename... Rest>
	struct declared_context<First, Rest...> {
		using type = first_non_void_t<typename context_of_tag<First>::type, typename declared_context<Rest...>::type>;
	};

	// The context a property_owner level carries: void for none
	template<typename T>
	struct owner_context {
		using type = void;
	};
	template<typename T>
		requires requires { typename T::_jai_context_type; }
	struct owner_context<T> {
		using type = typename T::_jai_context_type;
	};

	struct conflicting_contexts {};

	template<typename Found, typename Next>
	using merge_context_t = std::conditional_t<std::is_void_v<Next> || std::is_same_v<Found, Next>, Found,
		std::conditional_t<std::is_void_v<Found>, Next, conflicting_contexts>>;

	template<typename Found, typename... Rest>
	struct inherited_context {
		using type = Found;
	};
	template<typename Found, typename First, typename... Rest>
	struct inherited_context<Found, First, Rest...> {
		using type = typename inherited_context<merge_context_t<Found, typename owner_context<First>::type>, Rest...>::type;
	};

	// A level's tag replaces its bases' contexts only by narrowing each of them
	template<typename Declared, typename... Bases>
	inline constexpr bool narrows_every_base_context_v = (context_extends_v<Declared, typename owner_context<Bases>::type> && ...);

	template<typename Declared, typename Inherited>
	using effective_context_t = std::conditional_t<std::is_void_v<Declared>, Inherited, Declared>;

	// Stands in the base list for a level's context tag: unique to the level, so two levels never share a base subobject
	template<typename Derived>
	struct context_level {};

	template<typename Derived, typename Base>
	using owner_base_t = std::conditional_t<is_context_tag<Base>::value, context_level<Derived>, Base>;
}

} // namespace jai

#endif // __JAISCRIPT_CORE_BINDING_CONTEXT_HPP__
