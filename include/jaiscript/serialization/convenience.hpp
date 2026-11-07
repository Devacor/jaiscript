#pragma once

#include "json_archive.hpp"
#include "binary_archive.hpp"
#include <cctype>

namespace jai {

namespace detail {

inline constexpr char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

inline constexpr unsigned char base64_decode_table[] = {
    // 0-63: invalid (255), then mapped values
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255, // 0-15
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255, // 16-31
    255,255,255,255,255,255,255,255,255,255,255, 62,255,255,255, 63, // 32-47 (+ at 43, / at 47)
     52, 53, 54, 55, 56, 57, 58, 59, 60, 61,255,255,255,255,255,255, // 48-63 (0-9)
    255,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, // 64-79 (A-O)
     15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,255,255,255,255,255, // 80-95 (P-Z)
    255, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, // 96-111 (a-o)
     41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,255,255,255,255,255  // 112-127 (p-z)
};

inline std::string base64_encode(const std::string& input) {
    std::string result;
    result.reserve(((input.size() + 2) / 3) * 4);

    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(input.data());
    size_t len = input.size();
    size_t i = 0;

    while (i + 2 < len) {
        result += base64_chars[(bytes[i] >> 2) & 0x3F];
        result += base64_chars[((bytes[i] & 0x03) << 4) | ((bytes[i+1] >> 4) & 0x0F)];
        result += base64_chars[((bytes[i+1] & 0x0F) << 2) | ((bytes[i+2] >> 6) & 0x03)];
        result += base64_chars[bytes[i+2] & 0x3F];
        i += 3;
    }

    if (i < len) {
        result += base64_chars[(bytes[i] >> 2) & 0x3F];

        if (i + 1 < len) {
            result += base64_chars[((bytes[i] & 0x03) << 4) | ((bytes[i+1] >> 4) & 0x0F)];
            result += base64_chars[(bytes[i+1] & 0x0F) << 2];
            result += '=';
        } else {
            result += base64_chars[(bytes[i] & 0x03) << 4];
            result += "==";
        }
    }

    return result;
}

inline std::string base64_decode(const std::string& input) {
    std::string result;
    if (input.empty()) return result;

    size_t valid_chars = 0;
    for (char c : input) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 128 && base64_decode_table[uc] != 255) {
            ++valid_chars;
        } else if (c == '=') {
            break; // Stop at padding
        }
    }

    result.reserve((valid_chars * 3) / 4);

    unsigned char buffer[4];
    size_t buffer_idx = 0;

    for (char c : input) {
        if (c == '=') break; // Padding marks end

        unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 128) continue; // Skip invalid

        unsigned char val = base64_decode_table[uc];
        if (val == 255) continue; // Skip invalid

        buffer[buffer_idx++] = val;

        if (buffer_idx == 4) {
            result += static_cast<char>((buffer[0] << 2) | (buffer[1] >> 4));
            result += static_cast<char>((buffer[1] << 4) | (buffer[2] >> 2));
            result += static_cast<char>((buffer[2] << 6) | buffer[3]);
            buffer_idx = 0;
        }
    }

    if (buffer_idx >= 2) {
        result += static_cast<char>((buffer[0] << 2) | (buffer[1] >> 4));
    }
    if (buffer_idx >= 3) {
        result += static_cast<char>((buffer[1] << 4) | (buffer[2] >> 2));
    }

    return result;
}

} // namespace detail

namespace detail {
template<typename T> struct shared_ptr_element { using type = T; };
template<typename T> struct shared_ptr_element<std::shared_ptr<T>> { using type = T; };
template<typename T> using shared_ptr_element_t = typename shared_ptr_element<T>::type;

template<typename T> struct unique_ptr_element { using type = T; };
template<typename T, typename D> struct unique_ptr_element<std::unique_ptr<T, D>> { using type = T; };
template<typename T> using unique_ptr_element_t = typename unique_ptr_element<T>::type;

template<typename T> struct is_shared_ptr : std::false_type {};
template<typename T> struct is_shared_ptr<std::shared_ptr<T>> : std::true_type {};
template<typename T> inline constexpr bool is_shared_ptr_v = is_shared_ptr<T>::value;

template<typename T> struct is_unique_ptr : std::false_type {};
template<typename T, typename D> struct is_unique_ptr<std::unique_ptr<T, D>> : std::true_type {};
template<typename T> inline constexpr bool is_unique_ptr_v = is_unique_ptr<T>::value;

// Combined: is owning smart pointer (shared_ptr or unique_ptr, but NOT weak_ptr)
template<typename T>
inline constexpr bool is_owning_ptr_v = is_shared_ptr_v<T> || is_unique_ptr_v<T>;

template<typename T>
using owning_ptr_element_t = std::conditional_t<
    is_shared_ptr_v<T>,
    shared_ptr_element_t<T>,
    std::conditional_t<is_unique_ptr_v<T>, unique_ptr_element_t<T>, T>
>;

template<typename T, typename = void>
struct has_property_mgr : std::false_type {};

template<typename T>
struct has_property_mgr<T, std::void_t<decltype(std::declval<T>().property_mgr)>> : std::true_type {};

// Concept to detect types with templated save() method (using concrete archive for detection)
template<typename T, typename = void>
struct has_save_method : std::false_type {};

template<typename T>
struct has_save_method<T, std::void_t<decltype(std::declval<T>().save(std::declval<serialization::json_archive_writer&>()))>> : std::true_type {};

// Concept to detect types with templated load() method (using concrete archive for detection)
template<typename T, typename = void>
struct has_load_method : std::false_type {};

template<typename T>
struct has_load_method<T, std::void_t<decltype(std::declval<T>().load(std::declval<serialization::json_archive_reader&>()))>> : std::true_type {};

template<typename T, typename = void>
struct has_serialize_method : std::false_type {};

template<typename T>
struct has_serialize_method<T, std::void_t<decltype(std::declval<T>().serialize(std::declval<serialization::json_archive_writer&>()))>> : std::true_type {};

template<typename T>
constexpr bool can_use_direct_archive_v = []() {
    if constexpr (is_owning_ptr_v<T>) {
        using E = owning_ptr_element_t<T>;
        return has_property_mgr<E>::value || has_save_method<E>::value || has_serialize_method<E>::value;
    } else {
        return has_property_mgr<T>::value || has_save_method<T>::value || has_serialize_method<T>::value;
    }
}();

template<typename T>
constexpr bool can_use_direct_archive_read_v = []() {
    if constexpr (is_owning_ptr_v<T>) {
        using E = owning_ptr_element_t<T>;
        return has_property_mgr<E>::value || has_load_method<E>::value || has_serialize_method<E>::value;
    } else {
        return has_property_mgr<T>::value || has_load_method<T>::value || has_serialize_method<T>::value;
    }
}();

// The root object every archive's document is: an owning pointer goes through the archive's smart-ptr path ($id/$type/$val),
// so its dynamic (polymorphic) type and aliasing survive instead of being sliced to the static pointee; a type the
// archive serializes directly writes its content into the root; anything else is the root's value0
template<typename Archive, typename T>
void write_root(Archive& ar, const T& value) {
    ar.begin_object("", 1);
    if constexpr (!is_owning_ptr_v<T> && can_use_direct_archive_v<T>) {
        ar.serialize_object_content(value);
    } else {
        ar.serialize("value0", value);
    }
    ar.end_object();
}

// The root object back: an owning pointer's $type selects the concrete polymorphic factory
template<typename T, typename Archive>
T read_root(Archive& ar) {
    std::string type_name;
    uint32_t version;
    ar.begin_object(type_name, version);
    T result{};
    if constexpr (!is_owning_ptr_v<T> && can_use_direct_archive_read_v<T>) {
        ar(result);
    } else {
        ar.serialize("value0", result);
    }
    ar.end_object();
    return result;
}

} // namespace detail

template<typename T>
std::string to_json(engine& eng, const T& value, int indent = 2) {
    serialization::json_archive_writer ar(indent, &eng);
    detail::write_root(ar, value);
    return ar.str();
}

template<typename T, typename Context>
std::string to_json(engine& eng, const T& value, Context& ctx, int indent = 2) {
    serialization::json_archive_writer ar(indent, &eng);
    ar.template set_user_context<Context>(&ctx);
    detail::write_root(ar, value);
    return ar.str();
}

template<typename T>
T from_json(engine& eng, const std::string& json) {
    serialization::json_archive_reader ar(json, &eng);
    return detail::read_root<T>(ar);
}

template<typename T, typename Context>
T from_json(engine& eng, const std::string& json, Context& ctx) {
    serialization::json_archive_reader ar(json, &eng);
    ar.template set_user_context<Context>(&ctx);
    return detail::read_root<T>(ar);
}

template<typename T>
std::vector<uint8_t> to_binary(engine& eng, const T& value) {
    serialization::binary_archive_writer ar(&eng);
    detail::write_root(ar, value);
    return ar.data();
}

template<typename T, typename Context>
std::vector<uint8_t> to_binary(engine& eng, const T& value, Context& ctx) {
    serialization::binary_archive_writer ar(&eng);
    ar.template set_user_context<Context>(&ctx);
    detail::write_root(ar, value);
    return ar.data();
}

template<typename T>
std::string to_binary_string(engine& eng, const T& value) {
    serialization::binary_archive_writer ar(&eng);
    detail::write_root(ar, value);
    auto& data = ar.data();
    return std::string(data.begin(), data.end());
}

template<typename T, typename Context>
std::string to_binary_string(engine& eng, const T& value, Context& ctx) {
    serialization::binary_archive_writer ar(&eng);
    ar.template set_user_context<Context>(&ctx);
    detail::write_root(ar, value);
    auto& data = ar.data();
    return std::string(data.begin(), data.end());
}

template<typename T>
T from_binary(engine& eng, const std::vector<uint8_t>& data) {
    serialization::binary_archive_reader ar(data, &eng);
    return detail::read_root<T>(ar);
}

template<typename T, typename Context>
T from_binary(engine& eng, const std::vector<uint8_t>& data, Context& ctx) {
    serialization::binary_archive_reader ar(data, &eng);
    ar.template set_user_context<Context>(&ctx);
    return detail::read_root<T>(ar);
}

template<typename T>
T from_binary_string(engine& eng, const std::string& data) {
    serialization::binary_archive_reader ar(reinterpret_cast<const uint8_t*>(data.data()), data.size(), &eng);
    return detail::read_root<T>(ar);
}

template<typename T, typename Context>
T from_binary_string(engine& eng, const std::string& data, Context& ctx) {
    serialization::binary_archive_reader ar(reinterpret_cast<const uint8_t*>(data.data()), data.size(), &eng);
    ar.template set_user_context<Context>(&ctx);
    return detail::read_root<T>(ar);
}

template<typename T>
std::string to_base64(engine& eng, const T& value) {
    return detail::base64_encode(to_binary_string(eng, value));
}

template<typename T, typename Context>
std::string to_base64(engine& eng, const T& value, Context& ctx) {
    return detail::base64_encode(to_binary_string(eng, value, ctx));
}

template<typename T>
T from_base64(engine& eng, const std::string& base64) {
    return from_binary_string<T>(eng, detail::base64_decode(base64));
}

template<typename T, typename Context>
T from_base64(engine& eng, const std::string& base64, Context& ctx) {
    return from_binary_string<T>(eng, detail::base64_decode(base64), ctx);
}

inline std::string base64_encode(const std::string& input) {
    return detail::base64_encode(input);
}

inline std::string base64_decode(const std::string& input) {
    return detail::base64_decode(input);
}

} // namespace jai
