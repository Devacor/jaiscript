#pragma once

#ifndef JAISCRIPT_SERIALIZATION_ARCHIVE_HPP
#define JAISCRIPT_SERIALIZATION_ARCHIVE_HPP

#include <concepts>
#include <type_traits>
#include <string>
#include <memory>
#include <cstdint>

#include "traits.hpp"

namespace jai {

class runtime_error;

namespace serialization {

struct archive_base {
    static constexpr bool is_jai_archive = true;
};

// The serialization access class is jai::access (traits.hpp, included above).
// Private serialize/save/load support needs exactly one declaration: `friend jai::access;`

template<typename T>
struct nvp {
    const char* name;
    T& value;
    nvp(const char* n, T& v) : name(n), value(v) {}
};

template<typename T>
nvp<T> make_nvp(const char* name, T& value) {
    return nvp<T>(name, value);
}

template<typename T>
nvp<const T> make_nvp(const char* name, const T& value) {
    return nvp<const T>(name, const_cast<const T&>(value));
}

#define JAI_NVP(x) ::jai::serialization::make_nvp(#x, x)

template<typename T>
concept jai_archive = requires { { T::is_jai_archive } -> std::convertible_to<bool>; } && T::is_jai_archive;

template<typename T>
concept not_jai_archive = !jai_archive<T>;

template<typename T>
inline constexpr bool is_save = std::is_base_of_v<archive_base, std::decay_t<T>> &&
    requires { typename std::decay_t<T>::is_writer_tag; };

template<typename T>
inline constexpr bool is_load = std::is_base_of_v<archive_base, std::decay_t<T>> &&
    requires { typename std::decay_t<T>::is_reader_tag; };


class json_archive_writer;
class json_archive_reader;
class binary_archive_writer;
class binary_archive_reader;
class polymorphic_registry;

template<class Derived> class archive_writer_impl;
template<class Derived> class archive_reader_impl;

} // namespace serialization
} // namespace jai

#endif // JAISCRIPT_SERIALIZATION_ARCHIVE_HPP
