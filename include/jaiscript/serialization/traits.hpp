#pragma once

#ifndef JAISCRIPT_SERIALIZATION_TRAITS_HPP
#define JAISCRIPT_SERIALIZATION_TRAITS_HPP

// Serialization type traits - pure type detection with minimal dependencies
// This header has NO dependencies on archive implementations

#include <type_traits>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <list>
#include <deque>
#include <forward_list>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <optional>
#include <variant>
#include <tuple>
#include <iterator>
#include <cstdint>

namespace jai {

namespace serialization {
	template<typename T> class construct;
	template<typename T> class construct_unique;
}

// THE friend class for serialization access — the single class every trait probe and
// dispatch routes through. Defined here (not construct.hpp) so it is complete before
// archive_impl.hpp's trait specializations name it. Private serialize/save/load and
// load_and_construct members need exactly one declaration: `friend jai::access;`
class access {
public:
	template<typename T, typename... Args>
	static std::shared_ptr<T> make_shared(Args&&... args) {
		return std::shared_ptr<T>(new T(std::forward<Args>(args)...));
	}
	template<typename T, typename... Args>
	static std::unique_ptr<T> make_unique(Args&&... args) {
		return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
	}
	// A load's default construction whatever the constructor's access (a private one through friend jai::access)
	template<typename T>
	static auto new_default() -> decltype(new T()) {
		return new T();
	}

	template<typename Archive, typename T>
	static auto load_and_construct(Archive& ar, serialization::construct<T>& c)
		-> decltype(T::load_and_construct(ar, c)) {
		return T::load_and_construct(ar, c);
	}

	template<typename Archive, typename T>
	static auto load_and_construct(Archive& ar, serialization::construct_unique<T>& c)
		-> decltype(T::load_and_construct(ar, c)) {
		return T::load_and_construct(ar, c);
	}

	// Explicit-T forms for callers with custom construct wrappers
	template<typename T, typename Archive, typename Construct>
	static auto load_and_construct(Archive& ar, Construct& c)
		-> decltype(T::load_and_construct(ar, c)) {
		return T::load_and_construct(ar, c);
	}
	template<typename T, typename Archive, typename Construct>
	static auto load_and_construct(Archive& ar, Construct& c, std::uint32_t version)
		-> decltype(T::load_and_construct(ar, c, version)) {
		return T::load_and_construct(ar, c, version);
	}

	// Member save/load/serialize accessors — allows SFINAE detection of protected methods.
	// Types only need `friend jai::access;` for all serialization support.
	template<typename Archive, typename T>
	static auto member_save(Archive& ar, const T& obj, uint32_t v) -> decltype(obj.save(ar, v)) { return obj.save(ar, v); }
	template<typename Archive, typename T>
	static auto member_save(Archive& ar, const T& obj) -> decltype(obj.save(ar)) { return obj.save(ar); }
	template<typename Archive, typename T>
	static auto member_load(Archive& ar, T& obj, uint32_t v) -> decltype(obj.load(ar, v)) { return obj.load(ar, v); }
	template<typename Archive, typename T>
	static auto member_load(Archive& ar, T& obj) -> decltype(obj.load(ar)) { return obj.load(ar); }
	template<typename Archive, typename T>
	static auto member_serialize(Archive& ar, T& obj, uint32_t v) -> decltype(obj.serialize(ar, v)) { return obj.serialize(ar, v); }
	template<typename Archive, typename T>
	static auto member_serialize(Archive& ar, T& obj) -> decltype(obj.serialize(ar)) { return obj.serialize(ar); }
};

namespace serialization {

template<typename T, typename = void>
struct has_access_default_constructor : std::false_type {};
template<typename T>
struct has_access_default_constructor<T, std::void_t<decltype(::jai::access::new_default<T>())>> : std::true_type {};

// What a load default-constructs: a public default constructor, or a private one the type opens to jai::access
template<typename T>
inline constexpr bool load_default_constructible = std::is_default_constructible_v<T> || has_access_default_constructor<T>::value;

template<typename T>
std::shared_ptr<T> make_default_for_load() {
	if constexpr (std::is_default_constructible_v<T>) {
		return std::make_shared<T>();
	} else {
		return std::shared_ptr<T>(::jai::access::new_default<T>());
	}
}

template<typename T>
std::unique_ptr<T> make_unique_default_for_load() {
	if constexpr (std::is_default_constructible_v<T>) {
		return std::make_unique<T>();
	} else {
		return std::unique_ptr<T>(::jai::access::new_default<T>());
	}
}

} // namespace serialization

// Whether a class declares or inherits a member of a serialization name at any access level, and whether that member is
// a plain public one. The archives ask where a type has no route they can call, and of a property_owner about to take its
// properties' route, to tell a forgotten `friend jai::access;` from a type that declares no such member.
namespace serialization_traits {

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable: 4250 4623 4624 4625 4626 5026 5027)
#endif

#define JAI_DETAIL_MEMBER_NAME_PROBE(member) \
	struct member##_name_fallback { void member(); }; \
	template<typename T> struct member##_name_probe : T, member##_name_fallback {}; \
	/* naming the member through the probe is ambiguous exactly when T has one */ \
	template<typename T, typename = void> struct member##_name_found : std::true_type {}; \
	template<typename T> struct member##_name_found<T, std::void_t<decltype(&member##_name_probe<T>::member)>> : std::false_type {}; \
	template<typename T, bool = std::is_class_v<T> && !std::is_final_v<T>> struct declares_##member : std::false_type {}; \
	template<typename T> struct declares_##member<T, true> : member##_name_found<T> {}; \
	/* one public member that is neither a template nor overloaded: its address needs no friend */ \
	template<typename T, typename = void> struct has_plain_public_##member : std::false_type {}; \
	template<typename T> struct has_plain_public_##member<T, std::void_t<decltype(&T::member)>> : std::true_type {}

JAI_DETAIL_MEMBER_NAME_PROBE(serialize);
JAI_DETAIL_MEMBER_NAME_PROBE(save);
JAI_DETAIL_MEMBER_NAME_PROBE(load);
JAI_DETAIL_MEMBER_NAME_PROBE(load_and_construct);

#undef JAI_DETAIL_MEMBER_NAME_PROBE

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

// What a type with no callable route says about one serialization name
enum class member_name_verdict { absent, unreachable, not_an_archive_function };

template<typename Declares, typename PlainPublic>
inline constexpr member_name_verdict member_name_verdict_of =
	!Declares::value ? member_name_verdict::absent :
	PlainPublic::value ? member_name_verdict::not_an_archive_function : member_name_verdict::unreachable;

template<typename T> inline constexpr member_name_verdict serialize_member_verdict_v = member_name_verdict_of<declares_serialize<T>, has_plain_public_serialize<T>>;
template<typename T> inline constexpr member_name_verdict save_member_verdict_v = member_name_verdict_of<declares_save<T>, has_plain_public_save<T>>;
template<typename T> inline constexpr member_name_verdict load_member_verdict_v = member_name_verdict_of<declares_load<T>, has_plain_public_load<T>>;
template<typename T> inline constexpr bool declares_load_and_construct_v = declares_load_and_construct<T>::value;

// Stands for any archive and any construct<U>, to ask whether jai::access can call the load_and_construct a class
// declares or inherits, whichever class that one constructs
struct any_load_and_construct_argument { template<typename U> operator U&() const; };
template<typename T, typename = void>
struct reaches_load_and_construct : std::false_type {};
template<typename T>
struct reaches_load_and_construct<T, std::void_t<decltype(::jai::access::load_and_construct<T>(
	std::declval<any_load_and_construct_argument&>(), std::declval<any_load_and_construct_argument&>()))>> : std::true_type {};

// A class whose own load_and_construct, or the nearest base's, is closed to jai::access
template<typename T>
inline constexpr bool load_and_construct_unreachable_v = declares_load_and_construct_v<T> && !reaches_load_and_construct<T>::value;

} // namespace serialization_traits

namespace serialization {

#define JAI_DETAIL_UNREACHABLE_LOAD_AND_CONSTRUCT \
	"the type declares load_and_construct() but jai::access cannot reach it: add 'friend jai::access;' to the class (or make the function public)"

// The compile error for a type T an archive must construct on a load and cannot
template<typename T>
constexpr void no_route_to_construct() {
	static_assert(!serialization_traits::load_and_construct_unreachable_v<T>, JAI_DETAIL_UNREACHABLE_LOAD_AND_CONSTRUCT);
	static_assert(!serialization_traits::declares_load_and_construct_v<T> || serialization_traits::load_and_construct_unreachable_v<T>,
		"the type's load_and_construct() does not construct it: the archive calls load_and_construct(Archive&, jai::serialization::construct<T>&) "
		"with T the type it loads, and an inherited one constructs its own class");
	static_assert(serialization_traits::declares_load_and_construct_v<T>,
		"the archive cannot construct this type on a load: give it a default constructor (a non-public one with 'friend jai::access;') "
		"or a static load_and_construct(Archive&, jai::serialization::construct<T>&)");
}

// A polymorphic type may load through its subclasses alone, so only a load_and_construct jai::access cannot reach is an error
template<typename T>
constexpr void no_route_to_construct_polymorphic() {
	static_assert(!serialization_traits::load_and_construct_unreachable_v<T>, JAI_DETAIL_UNREACHABLE_LOAD_AND_CONSTRUCT);
}

#undef JAI_DETAIL_UNREACHABLE_LOAD_AND_CONSTRUCT

} // namespace serialization

namespace serialization_traits {

template<typename T> struct is_std_vector : std::false_type {};
template<typename T, typename A> struct is_std_vector<std::vector<T, A>> : std::true_type {};
template<typename T> inline constexpr bool is_std_vector_v = is_std_vector<T>::value;

template<typename T> struct is_std_map : std::false_type {};
template<typename K, typename V, typename C, typename A> struct is_std_map<std::map<K, V, C, A>> : std::true_type {};
template<typename T> inline constexpr bool is_std_map_v = is_std_map<T>::value;

template<typename T> struct is_std_unordered_map : std::false_type {};
template<typename K, typename V, typename H, typename E, typename A>
struct is_std_unordered_map<std::unordered_map<K, V, H, E, A>> : std::true_type {};
template<typename T> inline constexpr bool is_std_unordered_map_v = is_std_unordered_map<T>::value;

template<typename T> struct is_std_array : std::false_type {};
template<typename T, std::size_t N> struct is_std_array<std::array<T, N>> : std::true_type {};
template<typename T> inline constexpr bool is_std_array_v = is_std_array<T>::value;

template<typename T> struct is_std_pair : std::false_type {};
template<typename T1, typename T2> struct is_std_pair<std::pair<T1, T2>> : std::true_type {};
template<typename T> inline constexpr bool is_std_pair_v = is_std_pair<T>::value;

template<typename T> struct is_std_optional : std::false_type {};
template<typename T> struct is_std_optional<std::optional<T>> : std::true_type {};
template<typename T> inline constexpr bool is_std_optional_v = is_std_optional<T>::value;

template<typename T> struct is_std_variant : std::false_type {};
template<typename... Ts> struct is_std_variant<std::variant<Ts...>> : std::true_type {};
template<typename T> inline constexpr bool is_std_variant_v = is_std_variant<T>::value;

template<typename T> struct is_std_tuple : std::false_type {};
template<typename... Ts> struct is_std_tuple<std::tuple<Ts...>> : std::true_type {};
template<typename T> inline constexpr bool is_std_tuple_v = is_std_tuple<T>::value;

template<typename T> struct is_std_set : std::false_type {};
template<typename K, typename C, typename A> struct is_std_set<std::set<K, C, A>> : std::true_type {};
template<typename T> inline constexpr bool is_std_set_v = is_std_set<T>::value;

template<typename T> struct is_std_unordered_set : std::false_type {};
template<typename K, typename H, typename E, typename A>
struct is_std_unordered_set<std::unordered_set<K, H, E, A>> : std::true_type {};
template<typename T> inline constexpr bool is_std_unordered_set_v = is_std_unordered_set<T>::value;

template<typename T> struct is_std_list : std::false_type {};
template<typename T, typename A> struct is_std_list<std::list<T, A>> : std::true_type {};
template<typename T> inline constexpr bool is_std_list_v = is_std_list<T>::value;

template<typename T> struct is_std_deque : std::false_type {};
template<typename T, typename A> struct is_std_deque<std::deque<T, A>> : std::true_type {};
template<typename T> inline constexpr bool is_std_deque_v = is_std_deque<T>::value;

template<typename T> struct is_std_forward_list : std::false_type {};
template<typename T, typename A> struct is_std_forward_list<std::forward_list<T, A>> : std::true_type {};
template<typename T> inline constexpr bool is_std_forward_list_v = is_std_forward_list<T>::value;

template<typename T, typename = void>
struct is_iterable : std::false_type {};
template<typename T>
struct is_iterable<T, std::void_t<
    decltype(std::begin(std::declval<T&>())),
    decltype(std::end(std::declval<T&>()))
>> : std::true_type {};
template<typename T> inline constexpr bool is_iterable_v = is_iterable<T>::value;

template<typename T, typename = void>
struct has_key_type : std::false_type {};
template<typename T>
struct has_key_type<T, std::void_t<typename T::key_type>> : std::true_type {};
template<typename T> inline constexpr bool has_key_type_v = has_key_type<T>::value;

template<typename T, typename = void>
struct has_mapped_type : std::false_type {};
template<typename T>
struct has_mapped_type<T, std::void_t<typename T::mapped_type>> : std::true_type {};
template<typename T> inline constexpr bool has_mapped_type_v = has_mapped_type<T>::value;

template<typename T>
inline constexpr bool is_map_like_v = has_key_type_v<T> && has_mapped_type_v<T>;

template<typename T>
inline constexpr bool is_set_like_v = has_key_type_v<T> && !has_mapped_type_v<T> && is_iterable_v<T>;

template<typename T, typename = void>
struct has_value_type : std::false_type {};
template<typename T>
struct has_value_type<T, std::void_t<typename T::value_type>> : std::true_type {};
template<typename T> inline constexpr bool has_value_type_v = has_value_type<T>::value;

template<typename T>
inline constexpr bool is_sequence_container_v =
    is_iterable_v<T> &&
    has_value_type_v<T> &&
    !has_key_type_v<T> &&
    !std::is_same_v<T, std::string> &&
    !is_std_array_v<T>;  // std::array handled separately due to fixed size

template<typename T> struct vector_element { using type = void; };
template<typename T, typename A> struct vector_element<std::vector<T, A>> { using type = T; };
template<typename T> using vector_element_t = typename vector_element<T>::type;

template<typename T> struct array_element { using type = void; };
template<typename T, std::size_t N> struct array_element<std::array<T, N>> { using type = T; };
template<typename T> using array_element_t = typename array_element<T>::type;

template<typename T> struct array_size { static constexpr std::size_t value = 0; };
template<typename T, std::size_t N> struct array_size<std::array<T, N>> { static constexpr std::size_t value = N; };
template<typename T> inline constexpr std::size_t array_size_v = array_size<T>::value;

template<typename T> struct pair_first { using type = void; };
template<typename T1, typename T2> struct pair_first<std::pair<T1, T2>> { using type = T1; };
template<typename T> using pair_first_t = typename pair_first<T>::type;

template<typename T> struct pair_second { using type = void; };
template<typename T1, typename T2> struct pair_second<std::pair<T1, T2>> { using type = T2; };
template<typename T> using pair_second_t = typename pair_second<T>::type;

template<typename T> struct optional_element { using type = void; };
template<typename T> struct optional_element<std::optional<T>> { using type = T; };
template<typename T> using optional_element_t = typename optional_element<T>::type;

template<typename T> struct is_std_weak_ptr : std::false_type {};
template<typename T> struct is_std_weak_ptr<std::weak_ptr<T>> : std::true_type {};
template<typename T> inline constexpr bool is_std_weak_ptr_v = is_std_weak_ptr<T>::value;

template<typename T> struct is_std_shared_ptr : std::false_type {};
template<typename T> struct is_std_shared_ptr<std::shared_ptr<T>> : std::true_type {};
template<typename T> inline constexpr bool is_std_shared_ptr_v = is_std_shared_ptr<T>::value;

template<typename T> struct is_std_unique_ptr : std::false_type {};
template<typename T, typename D> struct is_std_unique_ptr<std::unique_ptr<T, D>> : std::true_type {};
template<typename T> inline constexpr bool is_std_unique_ptr_v = is_std_unique_ptr<T>::value;

template<typename T>
inline constexpr bool is_smart_ptr_v = is_std_weak_ptr_v<T> || is_std_shared_ptr_v<T> || is_std_unique_ptr_v<T>;

template<typename T> struct weak_ptr_element { using type = void; };
template<typename T> struct weak_ptr_element<std::weak_ptr<T>> { using type = T; };
template<typename T> using weak_ptr_element_t = typename weak_ptr_element<T>::type;

template<typename T> struct shared_ptr_element { using type = void; };
template<typename T> struct shared_ptr_element<std::shared_ptr<T>> { using type = T; };
template<typename T> using shared_ptr_element_t = typename shared_ptr_element<T>::type;

template<typename T> struct unique_ptr_element { using type = void; };
template<typename T, typename D> struct unique_ptr_element<std::unique_ptr<T, D>> { using type = T; };
template<typename T> using unique_ptr_element_t = typename unique_ptr_element<T>::type;

} // namespace serialization_traits
} // namespace jai

namespace jai { template<typename T> class property; }

namespace jai {
namespace serialization_traits {

template<typename T> struct is_jai_property : std::false_type {};
template<typename T> struct is_jai_property<::jai::property<T>> : std::true_type {};
template<typename T> inline constexpr bool is_jai_property_v = is_jai_property<T>::value;

template<typename T> struct property_element { using type = void; };
template<typename T> struct property_element<::jai::property<T>> { using type = T; };
template<typename T> using property_element_t = typename property_element<T>::type;

template<typename T>
inline constexpr bool is_direct_serializable_v =
	std::is_arithmetic_v<T> ||
	std::is_enum_v<T> ||
	std::is_same_v<T, std::string> ||
	std::is_same_v<T, char> ||
	std::is_same_v<T, unsigned char>;

template<typename T, typename = void>
struct is_property_owner : std::false_type {};

template<typename T>
struct is_property_owner<T, std::void_t<
	decltype(std::declval<T&>().property_mgr)
>> : std::true_type {};

template<typename T>
inline constexpr bool is_property_owner_v = is_property_owner<T>::value;

// load_and_construct detection (for types without default constructors)
// Note: These require jai::access to be complete, so they're declared here
// but the actual detection happens when construct.hpp is included
template<typename T, typename = void>
struct has_member_load_and_construct : std::false_type {};

template<typename T, typename = void>
struct has_free_load_and_construct : std::false_type {};

template<typename T>
inline constexpr bool has_load_and_construct_v =
	has_member_load_and_construct<T>::value || has_free_load_and_construct<T>::value;

template<typename T, typename = void>
struct has_member_load_and_construct_unique : std::false_type {};

template<typename T, typename = void>
struct has_free_load_and_construct_unique : std::false_type {};

template<typename T>
inline constexpr bool has_load_and_construct_unique_v =
	has_member_load_and_construct_unique<T>::value || has_free_load_and_construct_unique<T>::value;

} // namespace serialization_traits

using serialization_traits::is_std_vector_v;
using serialization_traits::is_std_map_v;
using serialization_traits::is_std_unordered_map_v;
using serialization_traits::is_std_array_v;
using serialization_traits::is_std_pair_v;
using serialization_traits::is_std_optional_v;
using serialization_traits::is_std_variant_v;
using serialization_traits::is_std_tuple_v;
using serialization_traits::is_std_weak_ptr_v;
using serialization_traits::is_std_shared_ptr_v;
using serialization_traits::is_std_unique_ptr_v;
using serialization_traits::is_smart_ptr_v;
using serialization_traits::is_direct_serializable_v;
using serialization_traits::is_property_owner_v;
using serialization_traits::is_jai_property_v;
using serialization_traits::property_element_t;

} // namespace jai

namespace jai {

class script_value;

namespace serialization {

// ADL-findable free-function detection. Must live at namespace scope: inside the
// archive classes, unqualified save/load/serialize would bind to the archives' own
// member functions at template-definition time on conforming compilers.
namespace adl_detect {
	template<typename Archive, typename T, typename = void>
	struct has_adl_save : std::false_type {};
	template<typename Archive, typename T>
	struct has_adl_save<Archive, T, std::void_t<decltype(save(std::declval<Archive&>(), std::declval<const T&>()))>> : std::true_type {};

	template<typename Archive, typename T, typename = void>
	struct has_adl_load : std::false_type {};
	template<typename Archive, typename T>
	struct has_adl_load<Archive, T, std::void_t<decltype(load(std::declval<Archive&>(), std::declval<T&>()))>> : std::true_type {};

	template<typename Archive, typename T, typename = void>
	struct has_adl_serialize : std::false_type {};
	template<typename Archive, typename T>
	struct has_adl_serialize<Archive, T, std::void_t<decltype(serialize(std::declval<Archive&>(), std::declval<T&>()))>> : std::true_type {};

	// inside an archive class the member serialize(name, value) hides the free function from ADL
	template<typename Archive, typename T>
	void call_adl_serialize(Archive& archive, T& value) { serialize(archive, value); }

	// the shapes an archive writes itself, ahead of any free function
	template<typename T>
	inline constexpr bool is_archive_shape_v =
		std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, std::string> ||
		serialization_traits::is_std_shared_ptr_v<T> || serialization_traits::is_std_weak_ptr_v<T> || serialization_traits::is_std_unique_ptr_v<T> ||
		serialization_traits::is_std_pair_v<T> || serialization_traits::is_std_optional_v<T> || serialization_traits::is_std_variant_v<T> ||
		serialization_traits::is_std_tuple_v<T> || serialization_traits::is_std_array_v<T> || serialization_traits::is_map_like_v<T> ||
		serialization_traits::is_set_like_v<T> || serialization_traits::is_sequence_container_v<T> || serialization_traits::is_jai_property_v<T>;
}


// Stands for every archive where a trait asks whether a class has a route of its own
struct route_probe_archive {
	static constexpr bool is_jai_archive = true;
	static constexpr bool is_text_format = false;
	template<typename... Args> void operator()(Args&&...) {}
	template<typename... Args> void serialize(Args&&...) {}
};

template<typename T>
inline constexpr bool has_member_route_v =
	requires(route_probe_archive& a, const T& t) { ::jai::access::member_save(a, t); } ||
	requires(route_probe_archive& a, const T& t) { ::jai::access::member_save(a, t, std::uint32_t(0)); } ||
	requires(route_probe_archive& a, T& t) { ::jai::access::member_load(a, t); } ||
	requires(route_probe_archive& a, T& t) { ::jai::access::member_load(a, t, std::uint32_t(0)); } ||
	requires(route_probe_archive& a, T& t) { ::jai::access::member_serialize(a, t); } ||
	requires(route_probe_archive& a, T& t) { ::jai::access::member_serialize(a, t, std::uint32_t(0)); };

// A class the archives write and read as the properties its engine binding declares (dynamic_binder::property): it has no
// route of its own, and no serialization member jai::access cannot reach (a forgotten friend stays a compile error)
template<typename T>
inline constexpr bool binding_record_v = std::is_class_v<T> && !adl_detect::is_archive_shape_v<T> && !std::is_same_v<T, script_value> &&
	!serialization_traits::is_property_owner_v<T> && !has_member_route_v<T> &&
	!adl_detect::has_adl_serialize<route_probe_archive, T>::value && !adl_detect::has_adl_save<route_probe_archive, T>::value &&
	!adl_detect::has_adl_load<route_probe_archive, T>::value &&
	serialization_traits::serialize_member_verdict_v<T> != serialization_traits::member_name_verdict::unreachable &&
	serialization_traits::save_member_verdict_v<T> != serialization_traits::member_name_verdict::unreachable &&
	serialization_traits::load_member_verdict_v<T> != serialization_traits::member_name_verdict::unreachable;

} // namespace serialization
} // namespace jai

#endif // JAISCRIPT_SERIALIZATION_TRAITS_HPP
