#pragma once

#ifndef JAISCRIPT_DETAIL_STATIC_TYPE_NAME_HPP
#define JAISCRIPT_DETAIL_STATIC_TYPE_NAME_HPP

#include <string_view>
#include <type_traits>

// Compile-time C++ type names via the compiler's signature macro (the nameof/ctti
// technique). Used by property_owner's implicit polymorphic registration so a plain
// class gets a stable, human-readable serialization name with no explicit registrar.

namespace jai {
namespace detail {

template<typename T>
constexpr std::string_view raw_type_signature() {
#if defined(_MSC_VER) && !defined(__clang__)
	return __FUNCSIG__;
#elif defined(__clang__) || defined(__GNUC__)
	return __PRETTY_FUNCTION__;
#else
	return "";
#endif
}

constexpr std::string_view extract_type_name(std::string_view sig) {
#if defined(_MSC_VER) && !defined(__clang__)
	// "... raw_type_signature<class game::Foo>(void)"
	constexpr std::string_view marker = "raw_type_signature<";
	auto start = sig.find(marker);
	if (start == std::string_view::npos) { return {}; }
	start += marker.size();
	auto end = sig.rfind(">(");
	if (end == std::string_view::npos || end <= start) { return {}; }
	auto name = sig.substr(start, end - start);
	if (name.substr(0, 6) == "class ") { name.remove_prefix(6); }
	else if (name.substr(0, 7) == "struct ") { name.remove_prefix(7); }
	else if (name.substr(0, 5) == "enum ") { name.remove_prefix(5); }
	return name;
#else
	// gcc: "... [with T = game::Foo; std::string_view = ...]"   clang: "... [T = game::Foo]"
	constexpr std::string_view marker = "T = ";
	auto start = sig.find(marker);
	if (start == std::string_view::npos) { return {}; }
	start += marker.size();
	auto end = sig.find_first_of(";]", start);
	if (end == std::string_view::npos) { return {}; }
	return sig.substr(start, end - start);
#endif
}

// Fully qualified ("game::scene::Node"); empty if the compiler is unsupported.
// Best-effort spelling for diagnostics — for names that go into saved data, use
// static_unqualified_type_name, which validates portability.
template<typename T>
constexpr std::string_view static_type_name() {
	return extract_type_name(raw_type_signature<T>());
}

// The implicit registration name: the bare class identifier, all qualification
// stripped ("game::scene::Node" -> "Node"). Anonymous-namespace types fall out as their
// bare name too — the compiler-specific marker (`anonymous namespace', {anonymous}, ...)
// sits in a discarded middle segment, so taking everything after the last "::" yields a
// clean identifier without special-casing. Kept bare for consistency with explicit
// jai::registrar names (also bare), so a type can move between implicit and explicit
// registration without its serialized $type changing.
//
// Empty when there is no stable bare identifier: templates (their argument spelling
// varies by compiler), lambdas, and any spelling whose final segment isn't a plain
// identifier. Those must be named explicitly — a jai_type_name member, or externally via
// jai::registrar / polymorphic_registry::try_auto_register without touching the type.
template<typename T>
constexpr std::string_view static_unqualified_type_name() {
	auto name = static_type_name<T>();
	if (name.empty()) { return {}; }
	if (name.find('<') != std::string_view::npos) { return {}; } //template instantiation
	if (auto pos = name.rfind("::"); pos != std::string_view::npos) { name.remove_prefix(pos + 2); }
	for (char c : name) {
		const bool identifierChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		if (!identifierChar) { return {}; } //lambda / local / anything with a non-identifier final segment
	}
	return name;
}

// THE naming ladder, shared by script registration (dynamic_binder auto-registration,
// jai::registrar auto-name) and serialization identity (polymorphic implicit entries):
// a self-owned jai_type_name pin wins, then the bare unqualified type name, then empty
// (= no portable auto-name: template instantiations, lambdas, locals — name explicitly).
// One function so a type carries ONE name everywhere and can move between implicit and
// explicit registration without that name changing.
//
// The pin is inherited by C++ lookup, so an unpinned Derived would otherwise claim its
// Base's name: when the pinning class also declares its owner tag (JAI_PROPERTY_OWNER
// emits both), only the exact owner keeps the pin and everyone else falls to the derived
// name. Bare hand-written pins keep whole-chain visibility (the legacy contract).
template<typename T>
constexpr std::string_view declared_or_derived_type_name() {
	if constexpr (requires { std::string_view{T::jai_type_name}; }) {
		if constexpr (requires { typename T::jai_name_owner; }) {
			if constexpr (std::is_same_v<typename T::jai_name_owner, T>) {
				return std::string_view{T::jai_type_name};
			} else {
				return static_unqualified_type_name<T>();
			}
		} else {
			return std::string_view{T::jai_type_name};
		}
	} else {
		return static_unqualified_type_name<T>();
	}
}

// Compile-time self-test: locks the signature parse against compiler/format drift
// (new compiler versions, clang-cl, gcc -fno-pretty-templates, ...). If a toolchain
// renders the signature differently, the build fails HERE instead of silently
// deriving wrong serialization names. GCC prints diagnostics minimally-qualified
// relative to the function's own namespace, so this jai::detail probe loses its
// prefix there — the UNQUALIFIED name (the naming ladder's actual input) is
// identical on every supported compiler and is what the self-test locks hard.
struct static_type_name_probe {};
static_assert(static_type_name<static_type_name_probe>() == std::string_view("jai::detail::static_type_name_probe") ||
              static_type_name<static_type_name_probe>() == std::string_view("static_type_name_probe"),
	"static_type_name: signature parse failed for this compiler — implicit registration names would be wrong. Fix extract_type_name for this compiler's signature format.");
static_assert(static_unqualified_type_name<static_type_name_probe>() == std::string_view("static_type_name_probe"),
	"static_unqualified_type_name: namespace stripping failed for this compiler.");

} // namespace detail
} // namespace jai

#endif // JAISCRIPT_DETAIL_STATIC_TYPE_NAME_HPP
