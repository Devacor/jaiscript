#pragma once

#include <jaiscript/core/type_info.hpp>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <cstdint>
#include <typeindex>
#include <typeinfo>
#include <type_traits>

// Forward declarations to avoid circular dependencies
namespace jai {
    class script_value;
}

namespace jai {
namespace serialization {

class json_archive_writer;
class json_archive_reader;
class binary_archive_writer;
class binary_archive_reader;

enum class writer_archive_id : uint8_t {
    json = 0,
    binary = 1,
    unknown = 255
};

enum class reader_archive_id : uint8_t {
    json = 0,
    binary = 1,
    unknown = 255
};

template<typename Archive> struct writer_archive_id_trait {
    static constexpr writer_archive_id value = writer_archive_id::unknown;
};
template<typename Archive> struct reader_archive_id_trait {
    static constexpr reader_archive_id value = reader_archive_id::unknown;
};

// The type-erased archives forward through one table of named functions per concrete archive (a manual vtable). The
// entries are never lambdas: MSVC numbers the lambdas in a constructor template's member initializers per translation
// unit, so two units can give one lambda name different bodies and the linker keeps either.
struct writer_operations {
    void (*write_property_name)(void*, const std::string&);
    void (*write_int8)(void*, int8_t);
    void (*write_int16)(void*, int16_t);
    void (*write_int32)(void*, int32_t);
    void (*write_int64)(void*, int64_t);
    void (*write_uint8)(void*, uint8_t);
    void (*write_uint16)(void*, uint16_t);
    void (*write_uint32)(void*, uint32_t);
    void (*write_uint64)(void*, uint64_t);
    void (*write_float32)(void*, float);
    void (*write_float64)(void*, double);
    void (*write_bool)(void*, bool);
    void (*write_string)(void*, const std::string&);
    void (*begin_object)(void*, const std::string&, uint32_t);
    void (*end_object)(void*);
    void (*begin_array)(void*, size_t);
    void (*end_array)(void*);
    void (*begin_map)(void*, size_t);
    void (*end_map)(void*);
    void (*write_map_key)(void*, const std::string&);
    std::pair<uint32_t, bool> (*get_or_assign_shared_id)(void*, const void*);
};

template<typename Archive>
struct archive_writer_thunks {
    static void write_property_name(void* p, const std::string& n) { static_cast<Archive*>(p)->write_property_name(n); }
    static void write_int8(void* p, int8_t v) { static_cast<Archive*>(p)->write_int8(v); }
    static void write_int16(void* p, int16_t v) { static_cast<Archive*>(p)->write_int16(v); }
    static void write_int32(void* p, int32_t v) { static_cast<Archive*>(p)->write_int32(v); }
    static void write_int64(void* p, int64_t v) { static_cast<Archive*>(p)->write_int64(v); }
    static void write_uint8(void* p, uint8_t v) { static_cast<Archive*>(p)->write_uint8(v); }
    static void write_uint16(void* p, uint16_t v) { static_cast<Archive*>(p)->write_uint16(v); }
    static void write_uint32(void* p, uint32_t v) { static_cast<Archive*>(p)->write_uint32(v); }
    static void write_uint64(void* p, uint64_t v) { static_cast<Archive*>(p)->write_uint64(v); }
    static void write_float32(void* p, float v) { static_cast<Archive*>(p)->write_float32(v); }
    static void write_float64(void* p, double v) { static_cast<Archive*>(p)->write_float64(v); }
    static void write_bool(void* p, bool v) { static_cast<Archive*>(p)->write_bool(v); }
    static void write_string(void* p, const std::string& v) { static_cast<Archive*>(p)->write_string(v); }
    static void begin_object(void* p, const std::string& t, uint32_t v) { static_cast<Archive*>(p)->begin_object(t, v); }
    static void end_object(void* p) { static_cast<Archive*>(p)->end_object(); }
    static void begin_array(void* p, size_t s) { static_cast<Archive*>(p)->begin_array(s); }
    static void end_array(void* p) { static_cast<Archive*>(p)->end_array(); }
    static void begin_map(void* p, size_t s) { static_cast<Archive*>(p)->begin_map(s); }
    static void end_map(void* p) { static_cast<Archive*>(p)->end_map(); }
    static void write_map_key(void* p, const std::string& k) { static_cast<Archive*>(p)->write_map_key(k); }
    static std::pair<uint32_t, bool> get_or_assign_shared_id(void* p, const void* raw) { return static_cast<Archive*>(p)->get_or_assign_shared_id(raw); }
};

template<typename Archive>
inline constexpr writer_operations writer_operations_of {
    &archive_writer_thunks<Archive>::write_property_name,
    &archive_writer_thunks<Archive>::write_int8,
    &archive_writer_thunks<Archive>::write_int16,
    &archive_writer_thunks<Archive>::write_int32,
    &archive_writer_thunks<Archive>::write_int64,
    &archive_writer_thunks<Archive>::write_uint8,
    &archive_writer_thunks<Archive>::write_uint16,
    &archive_writer_thunks<Archive>::write_uint32,
    &archive_writer_thunks<Archive>::write_uint64,
    &archive_writer_thunks<Archive>::write_float32,
    &archive_writer_thunks<Archive>::write_float64,
    &archive_writer_thunks<Archive>::write_bool,
    &archive_writer_thunks<Archive>::write_string,
    &archive_writer_thunks<Archive>::begin_object,
    &archive_writer_thunks<Archive>::end_object,
    &archive_writer_thunks<Archive>::begin_array,
    &archive_writer_thunks<Archive>::end_array,
    &archive_writer_thunks<Archive>::begin_map,
    &archive_writer_thunks<Archive>::end_map,
    &archive_writer_thunks<Archive>::write_map_key,
    &archive_writer_thunks<Archive>::get_or_assign_shared_id,
};

// Only used for dynamic/runtime polymorphism - static code uses CRTP directly.
class any_archive_writer {
    void* ptr_;
    writer_archive_id id_;
    const writer_operations* operations_;

public:
    static constexpr bool is_text_format = true; // Conservative default for type-erased

    template<typename Archive>
    explicit any_archive_writer(Archive& ar)
        : ptr_(&ar)
        , id_(writer_archive_id_trait<std::decay_t<Archive>>::value)
        , operations_(&writer_operations_of<Archive>) {}

    void write_property_name(const std::string& name) { operations_->write_property_name(ptr_, name); }
    void write_int8(int8_t v) { operations_->write_int8(ptr_, v); }
    void write_int16(int16_t v) { operations_->write_int16(ptr_, v); }
    void write_int32(int32_t v) { operations_->write_int32(ptr_, v); }
    void write_int64(int64_t v) { operations_->write_int64(ptr_, v); }
    void write_uint8(uint8_t v) { operations_->write_uint8(ptr_, v); }
    void write_uint16(uint16_t v) { operations_->write_uint16(ptr_, v); }
    void write_uint32(uint32_t v) { operations_->write_uint32(ptr_, v); }
    void write_uint64(uint64_t v) { operations_->write_uint64(ptr_, v); }
    void write_float32(float v) { operations_->write_float32(ptr_, v); }
    void write_float64(double v) { operations_->write_float64(ptr_, v); }
    void write_bool(bool v) { operations_->write_bool(ptr_, v); }
    void write_string(const std::string& v) { operations_->write_string(ptr_, v); }
    void begin_object(const std::string& type, uint32_t version) { operations_->begin_object(ptr_, type, version); }
    void end_object() { operations_->end_object(ptr_); }
    void begin_array(size_t size) { operations_->begin_array(ptr_, size); }
    void end_array() { operations_->end_array(ptr_); }
    void begin_map(size_t size) { operations_->begin_map(ptr_, size); }
    void end_map() { operations_->end_map(ptr_); }
    void write_map_key(const std::string& key) { operations_->write_map_key(ptr_, key); }
    std::pair<uint32_t, bool> get_or_assign_shared_id(const void* raw) { return operations_->get_or_assign_shared_id(ptr_, raw); }

    writer_archive_id archive_id() const { return id_; }
    void* raw_ptr() const { return ptr_; }

    // Usage: ar.dispatch([&](auto& concrete_ar) { concrete_ar(make_nvp("name", value)); });
    // Implementation is in archive.hpp after concrete archive definitions
    template<typename Fn>
    decltype(auto) dispatch(Fn&& fn);
};

struct reader_operations {
    bool (*seek_property)(void*, const std::string&);
    bool (*read_property_name)(void*, std::string&);
    int8_t (*read_int8)(void*);
    int16_t (*read_int16)(void*);
    int32_t (*read_int32)(void*);
    int64_t (*read_int64)(void*);
    uint8_t (*read_uint8)(void*);
    uint16_t (*read_uint16)(void*);
    uint32_t (*read_uint32)(void*);
    uint64_t (*read_uint64)(void*);
    float (*read_float32)(void*);
    double (*read_float64)(void*);
    bool (*read_bool)(void*);
    std::string (*read_string)(void*);
    void (*begin_object)(void*, std::string&, uint32_t&);
    void (*end_object)(void*);
    size_t (*begin_array)(void*);
    void (*end_array)(void*);
    size_t (*begin_map)(void*);
    void (*end_map)(void*);
    bool (*read_map_key)(void*, std::string&);
    bool (*has_property)(void*, const std::string&);
    bool needs_property_keys;
    void* (*get_engine)(void*);
    void* (*get_user_context)(void*, std::type_index);
    void (*clear_property_value)(void*);
    // Shared pointer ID tracking for type-erased smart pointer deserialization. The void
    // pointers carry a type_index so multiple-inheritance base-offset adjustment can happen
    // on the concrete side (this header can't see polymorphic_registry).
    bool (*has_deserialized_shared)(void*, uint32_t);
    std::shared_ptr<void> (*get_deserialized_shared_adjusted)(void*, uint32_t, std::type_index);
    void (*register_deserialized_shared_typed)(void*, uint32_t, std::shared_ptr<void>, std::type_index);
};

template<typename Archive>
struct archive_reader_thunks {
    static bool seek_property(void* p, const std::string& n) { return static_cast<Archive*>(p)->seek_property(n); }
    static bool read_property_name(void* p, std::string& n) { return static_cast<Archive*>(p)->read_property_name(n); }
    static int8_t read_int8(void* p) { return static_cast<Archive*>(p)->read_int8(); }
    static int16_t read_int16(void* p) { return static_cast<Archive*>(p)->read_int16(); }
    static int32_t read_int32(void* p) { return static_cast<Archive*>(p)->read_int32(); }
    static int64_t read_int64(void* p) { return static_cast<Archive*>(p)->read_int64(); }
    static uint8_t read_uint8(void* p) { return static_cast<Archive*>(p)->read_uint8(); }
    static uint16_t read_uint16(void* p) { return static_cast<Archive*>(p)->read_uint16(); }
    static uint32_t read_uint32(void* p) { return static_cast<Archive*>(p)->read_uint32(); }
    static uint64_t read_uint64(void* p) { return static_cast<Archive*>(p)->read_uint64(); }
    static float read_float32(void* p) { return static_cast<Archive*>(p)->read_float32(); }
    static double read_float64(void* p) { return static_cast<Archive*>(p)->read_float64(); }
    static bool read_bool(void* p) { return static_cast<Archive*>(p)->read_bool(); }
    static std::string read_string(void* p) { return static_cast<Archive*>(p)->read_string(); }
    static void begin_object(void* p, std::string& t, uint32_t& v) { static_cast<Archive*>(p)->begin_object(t, v); }
    static void end_object(void* p) { static_cast<Archive*>(p)->end_object(); }
    static size_t begin_array(void* p) { return static_cast<Archive*>(p)->begin_array(); }
    static void end_array(void* p) { static_cast<Archive*>(p)->end_array(); }
    static size_t begin_map(void* p) { return static_cast<Archive*>(p)->begin_map(); }
    static void end_map(void* p) { static_cast<Archive*>(p)->end_map(); }
    static bool read_map_key(void* p, std::string& k) { return static_cast<Archive*>(p)->read_map_key(k); }
    static bool has_property(void* p, const std::string& n) { return static_cast<Archive*>(p)->has_property(n); }
    static void* get_engine(void* p) { return static_cast<void*>(static_cast<Archive*>(p)->get_engine()); }
    static void* get_user_context(void* p, std::type_index ti) { return static_cast<Archive*>(p)->get_user_context_raw(ti); }
    static void clear_property_value(void* p) { static_cast<Archive*>(p)->clear_property_value(); }
    static bool has_deserialized_shared(void* p, uint32_t id) { return static_cast<Archive*>(p)->has_deserialized_shared(id); }
    // The concrete side adjusts the complete pointer to the target base subobject
    static std::shared_ptr<void> get_deserialized_shared_adjusted(void* p, uint32_t id, std::type_index target) {
        return static_cast<Archive*>(p)->get_deserialized_shared_adjusted(id, target);
    }
    static void register_deserialized_shared_typed(void* p, uint32_t id, std::shared_ptr<void> complete, std::type_index type) {
        static_cast<Archive*>(p)->register_deserialized_shared_typed(id, std::move(complete), type);
    }
};

template<typename Archive>
inline constexpr reader_operations reader_operations_of {
    &archive_reader_thunks<Archive>::seek_property,
    &archive_reader_thunks<Archive>::read_property_name,
    &archive_reader_thunks<Archive>::read_int8,
    &archive_reader_thunks<Archive>::read_int16,
    &archive_reader_thunks<Archive>::read_int32,
    &archive_reader_thunks<Archive>::read_int64,
    &archive_reader_thunks<Archive>::read_uint8,
    &archive_reader_thunks<Archive>::read_uint16,
    &archive_reader_thunks<Archive>::read_uint32,
    &archive_reader_thunks<Archive>::read_uint64,
    &archive_reader_thunks<Archive>::read_float32,
    &archive_reader_thunks<Archive>::read_float64,
    &archive_reader_thunks<Archive>::read_bool,
    &archive_reader_thunks<Archive>::read_string,
    &archive_reader_thunks<Archive>::begin_object,
    &archive_reader_thunks<Archive>::end_object,
    &archive_reader_thunks<Archive>::begin_array,
    &archive_reader_thunks<Archive>::end_array,
    &archive_reader_thunks<Archive>::begin_map,
    &archive_reader_thunks<Archive>::end_map,
    &archive_reader_thunks<Archive>::read_map_key,
    &archive_reader_thunks<Archive>::has_property,
    Archive::needs_property_keys,
    &archive_reader_thunks<Archive>::get_engine,
    &archive_reader_thunks<Archive>::get_user_context,
    &archive_reader_thunks<Archive>::clear_property_value,
    &archive_reader_thunks<Archive>::has_deserialized_shared,
    &archive_reader_thunks<Archive>::get_deserialized_shared_adjusted,
    &archive_reader_thunks<Archive>::register_deserialized_shared_typed,
};

class any_archive_reader {
    void* ptr_;
    reader_archive_id id_;
    const reader_operations* operations_;

public:
    template<typename Archive>
    explicit any_archive_reader(Archive& ar)
        : ptr_(&ar)
        , id_(reader_archive_id_trait<std::decay_t<Archive>>::value)
        , operations_(&reader_operations_of<Archive>) {}

    bool seek_property(const std::string& name) { return operations_->seek_property(ptr_, name); }
    bool read_property_name(std::string& name) { return operations_->read_property_name(ptr_, name); }
    int8_t read_int8() { return operations_->read_int8(ptr_); }
    int16_t read_int16() { return operations_->read_int16(ptr_); }
    int32_t read_int32() { return operations_->read_int32(ptr_); }
    int64_t read_int64() { return operations_->read_int64(ptr_); }
    uint8_t read_uint8() { return operations_->read_uint8(ptr_); }
    uint16_t read_uint16() { return operations_->read_uint16(ptr_); }
    uint32_t read_uint32() { return operations_->read_uint32(ptr_); }
    uint64_t read_uint64() { return operations_->read_uint64(ptr_); }
    float read_float32() { return operations_->read_float32(ptr_); }
    double read_float64() { return operations_->read_float64(ptr_); }
    bool read_bool() { return operations_->read_bool(ptr_); }
    std::string read_string() { return operations_->read_string(ptr_); }
    void begin_object(std::string& type, uint32_t& version) { operations_->begin_object(ptr_, type, version); }
    void end_object() { operations_->end_object(ptr_); }
    size_t begin_array() { return operations_->begin_array(ptr_); }
    void end_array() { operations_->end_array(ptr_); }
    size_t begin_map() { return operations_->begin_map(ptr_); }
    void end_map() { operations_->end_map(ptr_); }
    bool read_map_key(std::string& key) { return operations_->read_map_key(ptr_, key); }
    bool has_property(const std::string& name) { return operations_->has_property(ptr_, name); }
    bool needs_property_keys() { return operations_->needs_property_keys; }

    template<typename Engine>
    Engine* get_engine() { return static_cast<Engine*>(operations_->get_engine(ptr_)); }

    template<typename ContextType>
    ContextType* get_user_context() {
        void* raw = operations_->get_user_context(ptr_, std::type_index(typeid(ContextType)));
        return static_cast<ContextType*>(raw);
    }

    bool has_deserialized_shared(uint32_t id) { return operations_->has_deserialized_shared(ptr_, id); }

    // Get a previously deserialized shared_ptr by ID, cast to the target type. The void is
    // already adjusted to T's subobject, so the static_pointer_cast reinterprets a correct
    // address (multiple-inheritance safe).
    template<typename T>
    std::shared_ptr<T> get_deserialized_shared(uint32_t id) {
        auto void_ptr = operations_->get_deserialized_shared_adjusted(ptr_, id, std::type_index(typeid(T)));
        return std::static_pointer_cast<T>(void_ptr);
    }

    // Register a newly deserialized shared_ptr, storing the complete object + its dynamic
    // type so later back-refs through any base view adjust correctly.
    template<typename T>
    void register_deserialized_shared(uint32_t id, std::shared_ptr<T> ptr) {
        if (!ptr) { return; }
        if constexpr (std::is_polymorphic_v<T>) {
            void* complete = dynamic_cast<void*>(ptr.get());
            operations_->register_deserialized_shared_typed(ptr_, id, std::shared_ptr<void>(ptr, complete), std::type_index(typeid(*ptr)));
        } else {
            operations_->register_deserialized_shared_typed(ptr_, id, std::static_pointer_cast<void>(ptr), std::type_index(typeid(T)));
        }
    }

    reader_archive_id archive_id() const { return id_; }
    void* raw_ptr() const { return ptr_; }

    void clear_property_value() { operations_->clear_property_value(ptr_); }

    // Usage: ar.dispatch([&](auto& concrete_ar) { concrete_ar(make_nvp("name", value)); });
    // Implementation is in archive.hpp after concrete archive definitions
    template<typename Fn>
    decltype(auto) dispatch(Fn&& fn);
};

// How a bound property of a class with no route of its own reaches the archives (the class's binding record): one static
// function per bound member type, over the binding's own state. Null for a property the archives skip.
struct property_archive_route {
    void (*save)(const void* state, const char* name, const void* object, any_archive_writer& ar) = nullptr;
    void (*load)(const void* state, void* object, any_archive_reader& ar) = nullptr;
    std::shared_ptr<const void> state;
};

// A base level of a binding record (dynamic_binder::base_class): the base's binding in the archive's engine, written into
// the same object
struct base_archive_route {
    std::type_index type;
    void* (*upcast)(void* object);
};

// Serialization metadata for properties
struct property_metadata {
    std::string name;
    type_info_ptr type;
    bool read_only = false;  // Skip during serialization if true

    // Network flags
    enum network_flags : uint32_t {
        none = 0,
        reliable = 1 << 0,
        unreliable = 1 << 1,
        immediate = 1 << 2,
        compress_position = 1 << 3,
        throttle_100ms = 1 << 4,
        throttle_500ms = 1 << 5
    };
    uint32_t network_flags = network_flags::none;

    property_archive_route archive;
};

// Class serialization metadata
struct class_metadata {
    std::string class_name;
    uint32_t current_version = 1;
    std::vector<property_metadata> properties;
    std::vector<base_archive_route> bases;

    std::function<void(any_archive_writer&, const script_value&, uint32_t)> custom_save;
    std::function<script_value(any_archive_reader&, uint32_t)> custom_construct;

    std::vector<std::string> get_property_names() const {
        std::vector<std::string> result;
        result.reserve(properties.size());
        for (const auto& prop : properties) {
            result.push_back(prop.name);
        }
        return result;
    }
};

// Helper for network flags
struct network_flags {
    uint32_t flags;
    explicit network_flags(uint32_t f) : flags(f) {}
};

} // namespace serialization
} // namespace jai
