#pragma once

#ifndef __JAISCRIPT_CORE_DECLARED_METHODS_HPP__
#define __JAISCRIPT_CORE_DECLARED_METHODS_HPP__

#include <jaiscript/core/permission.hpp>
#include <type_traits>
#include <utility>

// JAI_METHOD(ReturnType, name, (parameters) qualifiers, options...) declares the member function
// `ReturnType name(parameters) qualifiers` and binds it to script under the same name; a body or a `;` follows it.
//   JAI_METHOD(void, spawn_wave, (int64_t count, bool elite));
//   JAI_METHOD(int64_t, health, () const) { return hp; }
//   JAI_METHOD((std::map<std::string, int64_t>), tallies, () const);
//   JAI_METHOD(void, save, (), jai::serial, jai::permissions::filesystem);
//   JAI_METHOD(Vec, operator+, (const Vec& a_other) const) { return {x + a_other.x}; }
// docs/site/embedding/02-dynamic-binder.html#jai-method is the full contract.

namespace jai {

template<typename U> class dynamic_binder;

namespace detail {
	// A JAI_METHOD's slot is the number of slots its class declared before it: overload resolution picks the
	// jai_method_counter taking the highest rank. The first finds property_owner's, or this one by argument-dependent lookup.
	// A class-scope lookup a later member changes is ill-formed, no diagnostic required ([class.member.lookup]); MSVC, GCC
	// and Clang all resolve it where it stands, which is what the slots rely on.
	template<int N>
	struct method_rank : method_rank<N - 1> {};
	template<>
	struct method_rank<0> {};
	template<int N>
	struct method_index {
		static constexpr int value = N;
	};
	inline constexpr int method_slot_limit = 128;
	method_index<0> jai_method_counter(method_rank<0>);

	// operator+ is the script operator +; every other name is itself
	constexpr const char* script_method_name(const char* a_name) {
		constexpr char prefix[] = "operator";
		for (int i = 0; i < 8; ++i) {
			if (a_name[i] != prefix[i]) {
				return a_name;
			}
		}
		const char next = a_name[8];
		if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') || (next >= '0' && next <= '9') || next == '_') {
			return a_name;
		}
		const char* result = a_name + 8;
		while (*result == ' ') {
			++result;
		}
		return result;
	}

	template<typename... Options>
	std::bool_constant<(function_option<Options> && ...)> method_options_check(int, const Options&...);

	template<auto... Options>
	struct method_options {};

	// A slot's binding as constants: the binder reads them in constant expressions, so a slot emits no function or data
	template<typename Pointer, typename Options>
	struct method_entry {
		using options = Options;
		const char* name;
		Pointer pointer;
	};

	template<auto... Options, typename Pointer>
	constexpr method_entry<Pointer, method_options<Options...>> make_method_entry(const char* a_name, Pointer a_pointer) {
		return { script_method_name(a_name), a_pointer };
	}

	// Binds a class's JAI_METHOD slots from its binder's automatic surface; a JAI_METHOD in any access section names it a friend
	struct method_access {
		template<typename T>
		static constexpr int slot_count() {
			if constexpr (requires { T::jai_method_counter(method_rank<method_slot_limit>{}); }) {
				return decltype(T::jai_method_counter(method_rank<method_slot_limit>{}))::value;
			} else {
				return 0;
			}
		}

		// a plain class counts on from its base's slots, which come first and bind with the base
		template<typename T, int I>
		static constexpr bool owns_slot() {
			if constexpr (requires(T& a_object) { a_object.jai_method_owner(method_index<I>{}); }) {
				return std::is_same_v<decltype(std::declval<T&>().jai_method_owner(method_index<I>{})), T*>;
			} else {
				return false;
			}
		}

		template<typename T, int... Is>
		static constexpr int inherited_slots(std::integer_sequence<int, Is...>) {
			return ((owns_slot<T, Is>() ? 0 : 1) + ... + 0);
		}

		template<typename T, int I>
		static constexpr auto slot_entry = T::template jai_method_entry<T>(method_index<I>{});

		// one per bound signature and options, shared by every slot that has them
		template<typename T, typename Pointer, auto... Options>
		static void bind_entry(dynamic_binder<T>& a_builder, const char* a_name, Pointer a_pointer, method_options<Options...>) {
			a_builder.method(a_name, a_pointer, Options...);
		}

		template<typename T, int First, int... Is>
		static void bind_slots(dynamic_binder<T>& a_builder, std::integer_sequence<int, Is...>) {
			(bind_entry(a_builder, slot_entry<T, First + Is>.name, slot_entry<T, First + Is>.pointer,
				typename std::remove_const_t<decltype(slot_entry<T, First + Is>)>::options{}), ...);
		}

		template<typename T>
		static void bind(dynamic_binder<T>& a_builder) {
			constexpr int count = slot_count<T>();
			if constexpr (count > 0) {
				constexpr int first = inherited_slots<T>(std::make_integer_sequence<int, count>{});
				if constexpr (first < count) {
					bind_slots<T, first>(a_builder, std::make_integer_sequence<int, count - first>{});
				}
			}
		}
	};
}

} // namespace jai

#define JAI_DETAIL_METHOD_CAT_IMPL(a, b) a##b
#define JAI_DETAIL_METHOD_CAT(a, b) JAI_DETAIL_METHOD_CAT_IMPL(a, b)
// named by the line: a static member's initializer reads the counter where it stands, where MSVC reads a member template's
// signature again in the complete class
#define JAI_DETAIL_METHOD_SLOT JAI_DETAIL_METHOD_CAT(jai_method_slot_, __LINE__)

// (T) is T, and T is T
#define JAI_DETAIL_STRIP_PARENS(x) JAI_DETAIL_STRIP_ESCAPE(JAI_DETAIL_STRIP_ISH x)
#define JAI_DETAIL_STRIP_ISH(...) JAI_DETAIL_STRIP_ISH __VA_ARGS__
#define JAI_DETAIL_STRIP_ESCAPE(...) JAI_DETAIL_STRIP_ESCAPE_IMPL(__VA_ARGS__)
#define JAI_DETAIL_STRIP_ESCAPE_IMPL(...) JAI_DETAIL_STRIP_VAN##__VA_ARGS__
#define JAI_DETAIL_STRIP_VANJAI_DETAIL_STRIP_ISH

#define JAI_METHOD(return_type, name, signature, ...) \
	static constexpr int JAI_DETAIL_METHOD_SLOT = decltype(jai_method_counter(::jai::detail::method_rank<::jai::detail::method_slot_limit>{}))::value; \
	static_assert(JAI_DETAIL_METHOD_SLOT < ::jai::detail::method_slot_limit, "JAI_METHOD: a class level declares at most 128"); \
	static_assert(decltype(::jai::detail::method_options_check(0, ##__VA_ARGS__))::value, \
		"JAI_METHOD: an option is jai::nodiscard, jai::serial or a permission enum"); \
	friend struct ::jai::detail::method_access; \
	template<typename jai_self_type> \
	static constexpr auto jai_method_entry(::jai::detail::method_index<JAI_DETAIL_METHOD_SLOT>) { \
		return ::jai::detail::make_method_entry<__VA_ARGS__>(#name, \
			static_cast<JAI_DETAIL_STRIP_PARENS(return_type) (jai_self_type::*) signature>(&jai_self_type::name)); \
	} \
	auto jai_method_owner(::jai::detail::method_index<JAI_DETAIL_METHOD_SLOT>) -> decltype(this); \
	static ::jai::detail::method_index<JAI_DETAIL_METHOD_SLOT + 1> jai_method_counter(::jai::detail::method_rank<JAI_DETAIL_METHOD_SLOT + 1>); \
	JAI_DETAIL_STRIP_PARENS(return_type) name signature

#endif // __JAISCRIPT_CORE_DECLARED_METHODS_HPP__
