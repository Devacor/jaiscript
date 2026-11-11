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
// A record writes named values: through its properties, a member save or serialize jai::access reaches, a free serialize or
// save, or its type's binding
template<typename Archive, typename T>
constexpr bool writes_record_v = Archive::template writes_member_record_v<T> || Archive::template writes_binding_record_v<T> ||
	serialization::writes_free_record_v<Archive, T>;

template<typename Archive, typename T>
constexpr bool reads_record_v = Archive::template reads_member_record_v<T> || Archive::template reads_binding_record_v<T> ||
	serialization::reads_free_record_v<Archive, T>;

// The root object every archive's document is: a record writes its named values into the root; anything else is the
// root's value0, so an owning pointer goes through the archive's smart-ptr path ($id/$type/$val) and keeps its dynamic
// type and aliasing
template<typename Archive, typename T>
void write_root(Archive& ar, const T& value) {
    ar.begin_object("", 1);
    if constexpr (writes_record_v<Archive, T>) {
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
    if constexpr (reads_record_v<Archive, T>) {
        ar.read_object_content(result);
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
