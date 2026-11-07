#pragma once

#include "archive_impl.hpp"
#include <vector>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace jai {
namespace serialization {

class binary_archive_writer : public archive_writer_impl<binary_archive_writer> {
public:
    binary_archive_writer();
    explicit binary_archive_writer(std::vector<uint8_t>& buffer);
    explicit binary_archive_writer(engine* eng);
    binary_archive_writer(std::vector<uint8_t>& buffer, engine* eng);
    ~binary_archive_writer();

    static constexpr bool needs_property_keys = true;
    static constexpr bool is_text_format = false;

    // Get the serialized data
    const std::vector<uint8_t>& data() const;
    std::vector<uint8_t>& data();

    void write_int8(int8_t value);

    void write_int16(int16_t value);

    void write_int32(int32_t value);

    void write_int64(int64_t value);

    void write_uint8(uint8_t value);

    void write_uint16(uint16_t value);

    void write_uint32(uint32_t value);

    void write_uint64(uint64_t value);

    void write_float32(float value);

    void write_float64(double value);

    void write_bool(bool value);

    void write_string(const std::string& value);

    void write_null();

    void write_binary(const void* data, size_t size);
    
    // Buffered mode: collect properties during begin_object..end_object, then flush with sizes
    void begin_object();

    void begin_object(const std::string& type_name, uint32_t version);

    void end_object();

    void write_property_name(const std::string& name);

    void begin_array(size_t size);

    void end_array();

    // Map serialization - Binary uses array of key-value pairs
    void begin_map(size_t size);

    void end_map();

    void write_map_key(const std::string& key);

    // One value, its containers, instances and handle payloads walked on an explicit frame stack: nesting costs memory, never
    // native depth
    void write_value(const script_value& root);

private:
    enum class frame_shape : uint8_t { values, array, map };
    struct value_frame {
        value_frame(frame_shape a_shape, script_value a_source) : shape(a_shape), source(std::move(a_source)) {}
        frame_shape shape;
        script_value source;              // the array or map, unused for values
        size_t index = 0;
        size_t count = 0;
        bool value_next = false;          // a map entry's key went out: its value is next
        script_map::const_iterator next{};
        std::vector<script_value> values; // a handle's payload, or an instance's property values in order
    };

    // The frame's next child, in its container (whose storage stays put while frames move) or in scratch when it had to be
    // made; null when the frame has none left
    const script_value* next_child(value_frame& f, script_value& scratch);

    // Writes a leaf whole, or a container's header and the frame that writes its children; false when a getter threw into
    // the backend
    bool begin_value(const script_value& given, std::vector<value_frame>& frames);

    std::vector<uint8_t>* buffer_ = nullptr;
    std::vector<uint8_t> owned_buffer_;

    struct buffered_property { std::string name; std::vector<uint8_t> data; };
    struct object_write_state {
        std::vector<buffered_property> properties;
        std::vector<uint8_t>* current_property_buffer = nullptr;
    };
    std::vector<object_write_state> object_write_stack_;

    std::vector<uint8_t>& get_buffer();

    void write_raw(const void* data, size_t size);

    template<typename T>
    void write_little_endian(T value) {
        static_assert(std::is_integral_v<T>, "Only for integral types");
        std::make_unsigned_t<T> uval;
        std::memcpy(&uval, &value, sizeof(T));
        uint8_t bytes[sizeof(T)];
        for (size_t i = 0; i < sizeof(T); ++i) {
            bytes[i] = static_cast<uint8_t>(uval >> (i * 8));
        }
        write_raw(bytes, sizeof(T));
    }
};

class binary_archive_reader : public archive_reader_impl<binary_archive_reader> {
public:
    // Engine is REQUIRED for binary reading since we need to create script_values
    explicit binary_archive_reader(const std::vector<uint8_t>& data, engine* eng);

    explicit binary_archive_reader(const void* data, size_t size, engine* eng);

    ~binary_archive_reader();

private:
    // Object state for pre-read property access with offset-based seeking
    struct property_location { size_t offset; uint32_t size; };
    struct object_state {
        std::map<std::string, size_t> property_name_to_index;       // Name -> index (for out-of-order access)
        std::vector<std::string> property_order;                     // Property names in order
        std::vector<property_location> property_locations;           // Offsets/sizes in same order (O(1) index access)
        size_t next_sequential_index = 0;
        size_t end_object_pos = 0;
    };
    std::vector<object_state> object_stack_;
    uint32_t seek_size_ = 0;    // Size of value to read (for validation)

public:
    static constexpr bool needs_property_keys = true;
    static constexpr bool is_text_format = false;

    // Basic type deserialization - check pre-read property first, then fall back to stream
    int8_t read_int8();

    int16_t read_int16();

    int32_t read_int32();

    int64_t read_int64();

    uint8_t read_uint8();

    uint16_t read_uint16();

    uint32_t read_uint32();

    uint64_t read_uint64();

    float read_float32();

    double read_float64();

    bool read_bool();

    std::string read_string();

    bool peek_null();

    void read_null();

    std::vector<uint8_t> read_binary(size_t /*expected_size*/);

    // Wire format must agree with the writer's end_object: count, names[], sizes[], values[]
    void begin_object();

    void begin_object(std::string& type_name, uint32_t& version);

    void end_object();

    size_t begin_array();

    void end_array();

    bool read_property_name(std::string& name);

    // Map deserialization - Binary reads from array of key-value pairs
    size_t begin_map();

    void end_map();

    bool read_map_key(std::string& key);

    void clear_property_value() {}
    bool has_current_property_value() const { return false; }
    bool in_array() const { return false; }

    bool has_property(const std::string& name);

    const std::vector<std::string>& get_object_property_names() const;

    bool seek_property_by_index(size_t index);

    bool seek_property_by_index(size_t index, const std::string& expected_name);

    bool seek_property(const std::string& name);

    script_value read_value();

private:
    std::vector<uint8_t> data_;
    size_t pos_;
    std::unordered_set<uint32_t> handle_ids_read_;

    void read_raw(void* dest, size_t size);

    template<typename T>
    T read_little_endian() {
        static_assert(std::is_integral_v<T>, "Only for integral types");

        uint8_t bytes[sizeof(T)];
        read_raw(bytes, sizeof(T));

        std::make_unsigned_t<T> value = 0;
        for (size_t i = 0; i < sizeof(T); ++i) {
            value |= static_cast<std::make_unsigned_t<T>>(bytes[i]) << (i * 8);
        }
        T result;
        std::memcpy(&result, &value, sizeof(T));
        return result;
    }

    // Raw read methods that always read from stream (for pre-reading phase)
    uint8_t read_uint8_raw();
    void expect_marker(uint8_t marker, const char* expected);

    size_t remaining_bytes() const;

    std::string read_string_raw();

    // One value; its arrays, maps, objects and handle payloads open frames on an explicit stack, so nesting costs memory,
    // never native depth
    script_value read_value_internal();

    enum class frame_shape : uint8_t { array, map, handle, object };
    struct value_frame {
        value_frame(frame_shape a_shape, uint64_t a_remaining, script_value a_built, engine* a_eng) :
            shape(a_shape), remaining(a_remaining), built(std::move(a_built)), key(std::monostate{}, a_eng) {}
        frame_shape shape;
        uint64_t remaining;
        script_value built;
        script_value key;
        bool have_key = false;
        std::string type_name;
        uint32_t version = 0;
        std::vector<std::string> names;
        std::vector<script_value> values;
    };

    // A leaf value read whole (true), or a container that opened a frame for its children (false)
    bool read_or_open(engine* eng, std::vector<value_frame>& frames, script_value& out);

    void take_child(engine* eng, value_frame& f, script_value&& child);

    script_value finish_frame(engine* eng, value_frame& f);
};

template<> struct writer_archive_id_trait<binary_archive_writer> {
    static constexpr writer_archive_id value = writer_archive_id::binary;
};
template<> struct reader_archive_id_trait<binary_archive_reader> {
    static constexpr reader_archive_id value = reader_archive_id::binary;
};

} // namespace serialization
} // namespace jai