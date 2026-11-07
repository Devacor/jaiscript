#pragma once

#ifndef JAISCRIPT_SERIALIZATION_ARCHIVE_IMPL_HPP
#define JAISCRIPT_SERIALIZATION_ARCHIVE_IMPL_HPP

#include <jaiscript/serialization/archive.hpp>

// Records host types nest at most where the host thread's stack cannot be asked: a host type's own save or load (serialize,
// save, load, load_and_construct) recurses natively once per record it opens, so the writer and the reader raise at the
// stack's margin (detail::native_stack_low) or at this many levels, whichever comes first. Script values never count:
// their walks run on work lists
#ifndef JAI_MAX_HOST_RECORD_DEPTH
#define JAI_MAX_HOST_RECORD_DEPTH 1024
#endif

namespace jai::detail { bool native_stack_low() noexcept; }

#include <jaiscript/serialization/serialization_metadata.hpp>
#include <jaiscript/serialization/traits.hpp>
#include <jaiscript/serialization/polymorphic.hpp>
#include <jaiscript/core/value.hpp>
#include <jaiscript/core/type_info.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/script_extensible.hpp>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <memory>
#include <map>
#include <unordered_map>
#include <variant>
#include <optional>
#include <tuple>
#include <utility>
#include <concepts>
#include <stack>

namespace jai {

class serialization_error : public runtime_error {
public:
    explicit serialization_error(const std::string& message) : runtime_error(message) {}
    explicit serialization_error(const char* message) : runtime_error(message) {}

    serialization_error(const std::string& message, const std::string& property_name, const std::string& class_name = "")
        : runtime_error(build_message(message, property_name, class_name)) {}

private:
    static std::string build_message(const std::string& message, const std::string& property_name, const std::string& class_name) {
        std::string result = "Serialization error: " + message;
        if (!property_name.empty()) {
            result += " (property: " + property_name;
            if (!class_name.empty()) {
                result += " in class: " + class_name;
            }
            result += ")";
        }
        return result;
    }
};

}

namespace jai {
namespace serialization {

[[noreturn]] JAI_NOINLINE inline void throw_host_record_depth() {
    throw serialization_error("Maximum record nesting depth (" + std::to_string(JAI_MAX_HOST_RECORD_DEPTH) + ") exceeded");
}
[[noreturn]] JAI_NOINLINE inline void throw_host_record_stack() {
    throw serialization_error("Record nesting depth exceeds the host thread's stack");
}
// A reader met another kind of data where it expects an object, an array, a map or an integer
[[noreturn]] void throw_archive_expected(const char* expected, std::string_view found);
[[noreturn]] void throw_archive_value_range(std::string_view value_text, const char* field);
[[noreturn]] void throw_archive_value_range(int64_t value, const char* field);
[[noreturn]] void throw_archive_value_range(uint64_t value, const char* field);
[[noreturn]] void throw_archive_value_range(double value, const char* field);

template<typename T>
constexpr const char* archive_integer_field() {
    constexpr bool is_signed = std::is_signed_v<T>;
    if constexpr (sizeof(T) == 1) { return is_signed ? "int8" : "uint8"; }
    else if constexpr (sizeof(T) == 2) { return is_signed ? "int16" : "uint16"; }
    else if constexpr (sizeof(T) == 4) { return is_signed ? "int32" : "uint32"; }
    else { return is_signed ? "int64" : "uint64"; }
}

// Every integer a reader loads lands here: a value outside the field's range, or a number with a fraction, is refused
// rather than wrapped or truncated
template<typename T>
T archive_integer(int64_t value) {
    if constexpr (std::is_signed_v<T>) {
        if (value < static_cast<int64_t>(std::numeric_limits<T>::min()) || value > static_cast<int64_t>(std::numeric_limits<T>::max())) [[unlikely]] {
            throw_archive_value_range(value, archive_integer_field<T>());
        }
    } else if (value < 0 || static_cast<uint64_t>(value) > static_cast<uint64_t>(std::numeric_limits<T>::max())) [[unlikely]] {
        throw_archive_value_range(value, archive_integer_field<T>());
    }
    return static_cast<T>(value);
}

template<typename T>
T archive_integer(uint64_t value) {
    if (value > static_cast<uint64_t>(std::numeric_limits<T>::max())) [[unlikely]] {
        throw_archive_value_range(value, archive_integer_field<T>());
    }
    return static_cast<T>(value);
}

template<typename T>
T archive_integer(double value) {
    if (!(value >= -9223372036854775808.0 && value < 18446744073709551616.0) || value != std::trunc(value)) [[unlikely]] {
        throw_archive_value_range(value, archive_integer_field<T>());
    }
    return value < 0 ? archive_integer<T>(static_cast<int64_t>(value)) : archive_integer<T>(static_cast<uint64_t>(value));
}

// A count read from the data reserves at most 1 MiB ahead; elements past it grow the container as they actually load
template<typename Container>
void reserve_for_load(Container& container, size_t count) {
    constexpr size_t most = (size_t{1} << 20) / sizeof(typename Container::value_type) + 1;
    container.reserve(count < most ? count : most);
}

// Before an archive opens a record: open_records are the ones already open
inline void enter_host_record(size_t open_records) {
    if (open_records >= JAI_MAX_HOST_RECORD_DEPTH) [[unlikely]] { throw_host_record_depth(); }
    if (jai::detail::native_stack_low()) [[unlikely]] { throw_host_record_stack(); }
}

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
}

// All serialization functions call I/O methods through self() for static dispatch.
// Derived classes implement I/O methods as regular (non-virtual) methods.

// Serialization Context - tracks whether we're writing property values or array elements.
// This enables automatic wrapping decisions:
// - In PropertyValue context: complex types are already inside an object, no wrapping needed
// - In ArrayElement context: complex types need to be wrapped in begin_object/end_object
enum class SerializationContext {
    Root,           // Top-level, before any container
    PropertyValue,  // After write_property_name, expecting a single value
    ArrayElement,   // Inside an array, expecting sequence of values
    MapValue,       // After write_map_key, expecting a single value
    ObjectBody      // Inside an object body - don't wrap the next complex value
};

// The writer state no archive type changes, defined in the library (archive_core.cpp) so a host translation unit
// never instantiates its containers
class archive_writer_core : public archive_base {
protected:
    explicit archive_writer_core(engine* eng);
    ~archive_writer_core();

    // The id of raw_ptr, and whether this call assigned it (its first sighting); 0 for null
    std::pair<uint32_t, bool> track_shared_ptr(const void* raw_ptr);

    engine* engine_ref_;
    // Hashed on std::type_index: libstdc++'s type_info::before is not a strict weak order between internal-linkage and other types.
    // Made at first use (most archives never set a context or meet a handle)
    std::unique_ptr<std::unordered_map<std::type_index, void*>> user_contexts_;

    std::unique_ptr<std::unordered_map<const void*, uint32_t>> shared_ptr_ids_;
    uint32_t next_shared_id_ = 1;  // 0 reserved for null

    std::stack<SerializationContext, std::vector<SerializationContext>> context_stack_;

    uint32_t version_ = 0;

public:
    SerializationContext current_context() const;
    void push_context(SerializationContext ctx);
    void pop_context();

    // Complex types with save/serialize/property_mgr need object wrapping UNLESS
    // we're already inside an object body that was created for this value.
    // ObjectBody context means the object wrapper was already written externally.
    bool needs_object_wrapper() const;

    // Transition from PropertyValue/MapValue context after writing a value
    void consume_value_context();

    void set_engine(engine* eng) { engine_ref_ = eng; }
    engine* get_engine() const { return engine_ref_; }

    template<typename ContextType>
    ContextType* get_user_context() const {
        return static_cast<ContextType*>(get_user_context_raw(std::type_index(typeid(ContextType))));
    }

    template<typename ContextType>
    void set_user_context(ContextType* context) {
        set_user_context_raw(std::type_index(typeid(ContextType)), const_cast<std::remove_cv_t<ContextType>*>(context));
    }

    // The context set for ti, else the archive's engine when ti names engine, else null
    void* get_user_context_raw(std::type_index ti) const;
    void set_user_context_raw(std::type_index ti, void* context);

    std::pair<uint32_t, bool> get_or_assign_shared_id(const void* raw_ptr) { return track_shared_ptr(raw_ptr); }
    uint32_t lookup_shared_id(const void* raw_ptr) const;

    void set_version(uint32_t version) { version_ = version; }
    uint32_t get_version() const { return version_; }
};

template<class Derived>
class archive_writer_impl : public archive_writer_core {
public:
    struct is_writer_tag {};

protected:
    Derived* self() { return static_cast<Derived*>(this); }
    const Derived* self() const { return static_cast<const Derived*>(this); }

    // The object a locked weak handle serializes as, the first time the archive sees it
    template <class Holder>
    script_value weak_target_value(const Holder& shared) const {
        return script_value::make_object(std::string(shared->type_name()), shared->type_id(), shared->held(), engine_ref_);
    }

    // Every property in the class's order: C++ bound properties through their getters, script fields as stored (a
    // load restores storage, so an accessor the script declared never stands between a save and its load), hybrids
    // (inherited C++ properties chain through the C++ base); an accessor with no field saves what its getter answers.
    // A script-declared accessor's error and a host exception leave the archive; a host property that cannot read
    // stays out. False when a getter threw into the backend: the in-flight exception leaves the archive.
    template <class ClassDef, class Emit>
    bool for_each_serialized_property(const script_value& value, const ClassDef& class_def, engine* eng, Emit&& emit) {
        const strong_ptr<class_instance> instance = serialized_instance_of(value);
        const std::vector<std::string> names = class_def->get_property_names();
        size_t index = 0;
        bool aborted = false;
        script_value property(std::monostate{}, eng);
        while (next_serialized_property(value, instance.get(), class_def, eng, names, index, property, aborted)) {
            emit(names[index - 1], std::move(property));
            property = script_value(std::monostate{}, eng);
        }
        return !aborted;
    }

    static strong_ptr<class_instance> serialized_instance_of(const script_value& value) {
        strong_ptr<class_instance> instance;
        if (value.is_object()) {
            try { instance = value.as<strong_ptr<class_instance>>(); } catch (...) {}
        }
        return instance;
    }

    // The next of names (from index on) that holds state, read into out (a script field as stored, a property through its
    // getter); false when none is left, or when a getter threw into the backend (aborted)
    template <class ClassDef>
    bool next_serialized_property(const script_value& value, const class_instance* instance, const ClassDef& class_def, engine* eng,
                                  const std::vector<std::string>& names, size_t& index, script_value& out, bool& aborted) {
        while (index < names.size()) {
            const std::string& prop_name = names[index++];
            if (instance) {
                if (const script_value* stored = instance->find_script_field_value(detail::engine_access::symbolize(*eng, prop_name))) {
                    out = stored->deref();
                    return true;
                }
            }
            uint64_t getter_id = detail::engine_access::symbolize(*eng, "_get_" + prop_name);
            auto getter = class_def->get_method(getter_id);
            if (getter.type() != script_value_type::jai_function_type) { continue; }
            std::vector<script_value> args = {value};
            auto result = getter.as_function()(args);
            if (!result) {
                if (class_def->script_declares_user_accessor(getter_id) ||
                    result.error() == make_error_code(runtime_error_code::cpp_exception)) {
                    throw runtime_error(eng->format_error(result));
                }
                continue;
            }
            if (detail::engine_access::backend_unwinding(*eng)) {
                aborted = true;
                return false;
            }
            // a property handed out through a transparent wrapper (an observable property's reference) saves its value
            out = result.value().try_unwrap_transparent_wrapper().value_or(result.value());
            return true;
        }
        return false;
    }

public:
    explicit archive_writer_impl(engine* eng = nullptr) : archive_writer_core(eng) {}

    template<typename T>
    std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, std::string>>
    serialize(const char* name, const T& value) {
        self()->write_property_name(name);
        push_context(SerializationContext::PropertyValue);
        write_primitive(value);
        consume_value_context();
    }

    template<typename T>
    std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_enum_v<T> && !std::is_same_v<T, std::string>>
    serialize(const char* name, const T& value) {
        self()->write_property_name(name);
        push_context(SerializationContext::PropertyValue);
        write_value(value);  // Use write_value which is context-aware
        consume_value_context();
    }

    template<typename T>
    void operator()(const char* name, const T& value) {
        serialize(name, value);
    }

    template<typename T>
    void operator()(const nvp<T>& nv) {
        serialize(nv.name, nv.value);
    }

    template<typename T, typename... Rest>
    void operator()(const nvp<T>& first, const Rest&... rest) {
        serialize(first.name, first.value);
        if constexpr (sizeof...(rest) > 0) {
            operator()(rest...);
        }
    }

private:
    // Helper traits for SFINAE — uses Derived (the concrete archive) instead of a stub type.
    // These traits are only evaluated inside member function template enable_if/if-constexpr,
    // which are instantiated at the call site where Derived is complete.
    // Uses ::jai::access::member_* to detect protected methods via friendship.

    template<typename T, typename = void>
    struct has_versioned_save_method : std::false_type {};
    template<typename T>
    struct has_versioned_save_method<T, std::void_t<decltype(::jai::access::member_save(std::declval<Derived&>(), std::declval<const T&>(), std::uint32_t(0)))>> : std::true_type {};

    template<typename T, typename = void>
    struct has_simple_save_method : std::false_type {};
    template<typename T>
    struct has_simple_save_method<T, std::void_t<decltype(::jai::access::member_save(std::declval<Derived&>(), std::declval<const T&>()))>> : std::true_type {};

    template<typename T>
    struct has_save_method : std::bool_constant<has_versioned_save_method<T>::value || has_simple_save_method<T>::value> {};

    template<typename T, typename = void>
    struct has_versioned_serialize_method : std::false_type {};
    template<typename T>
    struct has_versioned_serialize_method<T, std::void_t<decltype(::jai::access::member_serialize(std::declval<Derived&>(), std::declval<T&>(), std::uint32_t(0)))>> : std::true_type {};

    template<typename T, typename = void>
    struct has_simple_serialize_method : std::false_type {};
    template<typename T>
    struct has_simple_serialize_method<T, std::void_t<decltype(::jai::access::member_serialize(std::declval<Derived&>(), std::declval<T&>()))>> : std::true_type {};

    template<typename T>
    struct has_serialize_method : std::bool_constant<has_versioned_serialize_method<T>::value || has_simple_serialize_method<T>::value> {};

    template<typename T, typename = void>
    struct has_property_mgr : std::false_type {};
    template<typename T>
    struct has_property_mgr<T, std::void_t<decltype(std::declval<T&>().property_mgr)>> : std::true_type {};

    // ADL-findable save(Archive&, const T&) / serialize(Archive&, T&) free functions
    template<typename T> using has_adl_save = adl_detect::has_adl_save<Derived, T>;
    template<typename T> using has_adl_serialize = adl_detect::has_adl_serialize<Derived, T>;

public:
    // operator() for types with member save method
    // Prefers versioned save(Archive&, uint32_t), falls back to simple save(Archive&)
    // Context-aware: wraps in begin_object/end_object when called from array/root context
    // Uses access:: to support protected methods that friend jai::access
    template<typename T, std::enable_if_t<has_save_method<T>::value, int> = 0>
    void operator()(const T& obj) {
        bool wrap = needs_object_wrapper();
        if (wrap) {
            self()->begin_object();
            push_context(SerializationContext::ObjectBody);  // Prevent nested wrapping
        }
        if constexpr (has_versioned_save_method<T>::value) {
            ::jai::access::member_save(*self(), obj, 0);
        } else {
            ::jai::access::member_save(*self(), obj);
        }
        if (wrap) {
            pop_context();
            self()->end_object();
        }
    }

    // operator() for types with serialize() method (but no save())
    // Prefers versioned serialize(Archive&, uint32_t), falls back to simple serialize(Archive&)
    // Context-aware: wraps in begin_object/end_object when called from array/root context
    // Uses access:: to support protected methods that friend jai::access
    template<typename T, std::enable_if_t<
        !has_save_method<T>::value &&
        has_serialize_method<T>::value, int> = 0>
    void operator()(const T& obj) {
        bool wrap = needs_object_wrapper();
        if (wrap) {
            self()->begin_object();
            push_context(SerializationContext::ObjectBody);  // Prevent nested wrapping
        }
        if constexpr (has_versioned_serialize_method<T>::value) {
            ::jai::access::member_serialize(*self(), const_cast<T&>(obj), 0);
        } else {
            ::jai::access::member_serialize(*self(), const_cast<T&>(obj));
        }
        if (wrap) {
            pop_context();
            self()->end_object();
        }
    }

    // operator() for property_owner types WITHOUT custom save() or serialize()
    // Context-aware: wraps in begin_object/end_object when called from array/root context
    template<typename T, std::enable_if_t<
        !has_save_method<T>::value &&
        !has_serialize_method<T>::value &&
        has_property_mgr<T>::value, int> = 0>
    void operator()(const T& obj) {
        bool wrap = needs_object_wrapper();
        if (wrap) {
            self()->begin_object();
            push_context(SerializationContext::ObjectBody);  // Prevent nested wrapping
        }
        obj.save_owned_properties(*self());  // walks the inheritance chain (not just the derived mgr)
        if (wrap) {
            pop_context();
            self()->end_object();
        }
    }

    template<typename T>
    std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, std::string>>
    operator()(const T& value) {
        write_primitive(value);
    }

    template<typename T, std::enable_if_t<
        !has_save_method<T>::value &&
        !has_serialize_method<T>::value &&
        !has_property_mgr<T>::value &&
        !std::is_arithmetic_v<T> &&
        !std::is_enum_v<T> &&
        !std::is_same_v<T, std::string>, int> = 0>
    void operator()(const T& value) {
        using namespace serialization_traits;

        // ====================================================================
        // Smart pointers (must check before containers since they're iterable)
        // ====================================================================
        // shared_ptr: serialize as {"$id": N, "$val": {...}} or {"$id": N} if reference
        // Polymorphic types get {"$id": N, "$type": "ConcreteType", "$val": {...}}
        if constexpr (is_std_shared_ptr_v<T>) {
            if (!value) {
                self()->write_null();
            } else {
                auto [id, is_new] = self()->get_or_assign_shared_id(value.get());
                self()->begin_object();
                push_context(SerializationContext::ObjectBody);
                self()->serialize("$id", id);
                if (is_new) {
                    write_shared_payload(value);
                }
                pop_context();
                self()->end_object();
            }
        }
        // weak_ptr: serialize as {"$ref": N, "$val"?: {...}} or null. The "$val" is present
        // only when the weak_ptr is the FIRST reference to the object in traversal order (a
        // forward reference): we then inline the data under the same id so the reader can
        // reconstruct it. Without this the old code wrote {"$ref": 0} (lookup_shared_id
        // returned 0 for the not-yet-written object) and the link was silently dropped on
        // load. Mirrors the shared_ptr path and the script weak handle's path;
        // because depth-first write/read order is identical, the data always precedes any
        // back-reference, so no forward reference survives to load time.
        else if constexpr (is_std_weak_ptr_v<T>) {
            if (auto shared = value.lock()) {
                auto [id, is_new] = self()->get_or_assign_shared_id(shared.get());
                self()->begin_object();
                push_context(SerializationContext::ObjectBody);
                self()->serialize("$ref", id);
                if (is_new) {
                    write_shared_payload(shared);
                }
                pop_context();
                self()->end_object();
            } else {
                self()->write_null();
            }
        }
        // unique_ptr: serialize as {"$val": {...}} or null
        else if constexpr (is_std_unique_ptr_v<T>) {
            if (!value) {
                self()->write_null();
            } else {
                self()->begin_object();
                push_context(SerializationContext::ObjectBody);
                self()->serialize("$val", *value);
                pop_context();
                self()->end_object();
            }
        }
        // std::pair - serialize as 2-element array
        else if constexpr (is_std_pair_v<T>) {
            self()->begin_array(2);
            push_context(SerializationContext::ArrayElement);
            write_element(value.first);
            write_element(value.second);
            pop_context();
            self()->end_array();
        }
        // std::optional - serialize as [has_value, value?]
        else if constexpr (is_std_optional_v<T>) {
            self()->begin_array(value.has_value() ? 2 : 1);
            push_context(SerializationContext::ArrayElement);
            self()->write_bool(value.has_value());
            if (value.has_value()) {
                write_element(*value);
            }
            pop_context();
            self()->end_array();
        }
        // std::variant - serialize as [index, value]
        else if constexpr (is_std_variant_v<T>) {
            self()->begin_array(2);
            push_context(SerializationContext::ArrayElement);
            write_element(static_cast<uint32_t>(value.index()));
            std::visit([this](const auto& v) { write_element(v); }, value);
            pop_context();
            self()->end_array();
        }
        else if constexpr (is_std_tuple_v<T>) {
            constexpr auto size = std::tuple_size_v<T>;
            self()->begin_array(size);
            push_context(SerializationContext::ArrayElement);
            std::apply([this](const auto&... args) { (write_element(args), ...); }, value);
            pop_context();
            self()->end_array();
        }
        else if constexpr (is_std_array_v<T>) {
            constexpr auto N = array_size_v<T>;
            self()->begin_array(N);
            push_context(SerializationContext::ArrayElement);
            for (const auto& elem : value) {
                write_element(elem);
            }
            pop_context();
            self()->end_array();
        }
        // Map-like containers (std::map, std::unordered_map, std::multimap, etc.)
        else if constexpr (is_map_like_v<T>) {
            using K = typename T::key_type;
            if constexpr (std::is_same_v<K, std::string>) {
                // String keys: serialize as JSON object
                self()->begin_map(value.size());
                for (const auto& [key, val] : value) {
                    self()->write_map_key(key);
                    push_context(SerializationContext::MapValue);
                    write_value(val);
                    consume_value_context();
                }
                self()->end_map();
            } else {
                // Non-string keys: serialize as array of [key, value] pairs
                self()->begin_array(value.size());
                push_context(SerializationContext::ArrayElement);
                for (const auto& [key, val] : value) {
                    self()->begin_array(2);
                    push_context(SerializationContext::ArrayElement);
                    write_element(key);
                    write_element(val);
                    pop_context();
                    self()->end_array();
                }
                pop_context();
                self()->end_array();
            }
        }
        // Set-like containers (std::set, std::unordered_set, std::multiset, etc.)
        else if constexpr (is_set_like_v<T>) {
            self()->begin_array(value.size());
            push_context(SerializationContext::ArrayElement);
            for (const auto& elem : value) {
                write_element(elem);
            }
            pop_context();
            self()->end_array();
        }
        // Sequence containers (std::vector, std::list, std::deque, std::forward_list)
        else if constexpr (is_sequence_container_v<T>) {
            // Use std::distance for forward_list which doesn't have size()
            size_t count = 0;
            if constexpr (requires { value.size(); }) {
                count = value.size();
            } else {
                count = static_cast<size_t>(std::distance(std::begin(value), std::end(value)));
            }
            self()->begin_array(count);
            push_context(SerializationContext::ArrayElement);
            for (const auto& elem : value) {
                write_element(elem);
            }
            pop_context();
            self()->end_array();
        }
        else if constexpr (is_jai_property_v<T>) {
            write_value(value.get());
        }
        else if constexpr (has_adl_serialize<T>::value) {
            // ADL serialize functions write named properties, so wrap in object
            bool wrap = needs_object_wrapper();
            if (wrap) {
                self()->begin_object();
                push_context(SerializationContext::ObjectBody);
            }
            serialize(*self(), const_cast<T&>(value));
            if (wrap) {
                pop_context();
                self()->end_object();
            }
        }
        else if constexpr (has_adl_save<T>::value) {
            // ADL save functions write named properties, so wrap in object
            bool wrap = needs_object_wrapper();
            if (wrap) {
                self()->begin_object();
                push_context(SerializationContext::ObjectBody);
            }
            save(*self(), value);
            if (wrap) {
                pop_context();
                self()->end_object();
            }
        } else {
            throw serialization_error(
                "Type '" + std::string(typeid(T).name()) + "' is not serializable. "
                "Add save/load, serialize, property_owner, or ADL functions — or mark the property transient.");
        }
    }

    // write_value: unified entry point. Wraps complex types when needed:
    // - In PropertyValue context: no wrapping (already inside an object)
    // - In ArrayElement context: wrap complex types with begin_object/end_object
    // - In Root context: wrap complex types (standalone serialization)
    template<typename T>
    void write_value(const T& value) {
        if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, std::string>) {
            write_primitive(value);
        }
        else {
            (*self())(value);
        }
    }

    template<typename T>
    std::enable_if_t<std::is_arithmetic_v<T> || std::is_same_v<T, std::string>>
    write_element(const T& value) {
        write_primitive(value);
    }

    template<typename T>
    std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_same_v<T, std::string>>
    write_element(const T& value) {
        write_value(value);
    }

private:
    template<typename T>
    void write_primitive(const T& value) {
        if constexpr (std::is_same_v<T, int8_t>) self()->write_int8(value);
        else if constexpr (std::is_same_v<T, int16_t>) self()->write_int16(value);
        else if constexpr (std::is_same_v<T, int32_t>) self()->write_int32(value);
        else if constexpr (std::is_same_v<T, int64_t>) self()->write_int64(value);
        else if constexpr (std::is_same_v<T, uint8_t>) self()->write_uint8(value);
        else if constexpr (std::is_same_v<T, uint16_t>) self()->write_uint16(value);
        else if constexpr (std::is_same_v<T, uint32_t>) self()->write_uint32(value);
        else if constexpr (std::is_same_v<T, uint64_t>) self()->write_uint64(value);
        else if constexpr (std::is_same_v<T, float>) self()->write_float32(value);
        else if constexpr (std::is_same_v<T, double>) self()->write_float64(value);
        else if constexpr (std::is_same_v<T, bool>) self()->write_bool(value);
        else if constexpr (std::is_same_v<T, std::string>) self()->write_string(value);
        else if constexpr (std::is_enum_v<T>) write_primitive(static_cast<std::underlying_type_t<T>>(value));
        else if constexpr (std::is_integral_v<T>) self()->write_int64(static_cast<int64_t>(value));
        else if constexpr (std::is_floating_point_v<T>) self()->write_float64(static_cast<double>(value));
    }

    // A registered dynamic type's payload under "$val", written through its save function
    template<typename Entry>
    void write_registered_payload(const Entry& entry, const void* complete) {
        self()->write_property_name("$val");
        push_context(SerializationContext::PropertyValue);
        self()->begin_object();
        push_context(SerializationContext::ObjectBody);
        any_archive_writer wrapper(*self());
        entry.save_fn(wrapper, complete);
        pop_context();
        self()->end_object();
        pop_context();
    }

    // Writes the polymorphic-aware payload of a shared object (the caller is already
    // inside the wrapping object body): "$type"+"$val" for a registered polymorphic
    // dynamic type, else "$val". Shared by the shared_ptr and the forward-referencing
    // weak_ptr write paths so the two cannot drift apart.
    template<typename SharedPtr>
    void write_shared_payload(const SharedPtr& value) {
        using elem_type = typename SharedPtr::element_type;
        if constexpr (std::is_polymorphic_v<elem_type>) {
            const auto& actual_type = typeid(*value);
            // value.get() is a Base*; save_fn casts the void back to the concrete
            // (Derived) type, so hand it the complete object — dynamic_cast<void*>
            // recovers the most-derived address (correct under multiple inheritance).
            const void* complete = dynamic_cast<const void*>(value.get());
            std::string hybrid_type, hybrid_fields;
            strong_ptr<class_instance> layer;
            if (engine_ref_) {
                if constexpr (std::is_base_of_v<script_extensible, elem_type>) {
                    layer = value->jai_script_slot().script_layer();
                } else {
                    layer = detail::engine_access::script_layer_of(*engine_ref_, complete);
                }
            }
            if (layer && detail::engine_access::script_layer_save(*engine_ref_, layer, hybrid_type, hybrid_fields)) {
                // A script class deriving elem_type owns this object: $type carries the
                // script class name, $script its script-level fields, $val the C++ payload
                // through the dynamic C++ type's registered save (base bytes unchanged).
                auto* entry = polymorphic_registry::instance().find(std::type_index(actual_type));
                if (!entry || !entry->save_fn) {
                    throw serialization_error(std::string("Cannot serialize hybrid script class '") + hybrid_type +
                        "': its C++ base '" + actual_type.name() +
                        "' is not registered. Register it with jai::registrar<T, Context>(\"Name\") or derive from jai::property_owner<T, ...>.");
                }
                self()->serialize("$type", hybrid_type);
                self()->serialize("$script", hybrid_fields);
                write_registered_payload(*entry, complete);
                return;
            }
            if (actual_type != typeid(elem_type)) {
                auto* entry = polymorphic_registry::instance().find(std::type_index(actual_type));
                if (entry && entry->save_fn) {
                    self()->serialize("$type", entry->name);
                    write_registered_payload(*entry, complete);
                    return;
                }
                // Writing the static type's payload here would silently slice the object
                // and load back as the wrong type — fail loudly instead (cereal semantics).
                throw serialization_error(std::string("Cannot serialize unregistered polymorphic type '") + actual_type.name() +
                    "' through pointer-to-base '" + typeid(elem_type).name() +
                    "'. Register it with jai::registrar<T, Context, jai::bases<Base>>(\"Name\") or derive from jai::property_owner<T, Base>.");
            }
        }
        self()->serialize("$val", *value);
    }

public:
    // Pushes ObjectBody context so operator() doesn't wrap again
    template<typename T>
    void serialize_object_content(const T& value) {
        push_context(SerializationContext::ObjectBody);
        (*self())(value);
        pop_context();
    }
};

// All deserialization functions call I/O methods through self() for static dispatch.
// Derived classes implement I/O methods as regular (non-virtual) methods.

// The reader state no archive type changes, defined in the library (archive_core.cpp) so a host translation unit
// never instantiates its containers
class archive_reader_core : public archive_base {
protected:
    explicit archive_reader_core(engine* eng);
    ~archive_reader_core();

    // Records the complete object of an id's first load and its dynamic type
    void store_deserialized(uint32_t id, std::shared_ptr<void> complete, std::type_index type);

    engine* engine_ref_;

    // The outermost read is one load: a load session opens there and completes when it returns, so the script
    // post_load hooks of everything it loaded run once the whole archive is loaded (a read that throws drops them)
    int read_depth_ = 0;

    // C++ shared_ptr tracking for property serialization (type-erased). The stored pointer
    // is the COMPLETE object (most-derived) and `type` its dynamic type, so a back-reference
    // taken through any base view adjusts to the right subobject under multiple inheritance.
    struct deserialized_entry {
        std::shared_ptr<void> object;
        std::type_index type = std::type_index(typeid(void));
    };
    std::unique_ptr<std::unordered_map<uint32_t, deserialized_entry>> cpp_shared_ptrs_;   // made at first use

    // Hashed on std::type_index: libstdc++'s type_info::before is not a strict weak order between internal-linkage and other types
    std::unique_ptr<std::unordered_map<std::type_index, void*>> user_contexts_;

    uint32_t version_ = 0;

public:
    void set_engine(engine* eng) { engine_ref_ = eng; }
    engine* get_engine() const { return engine_ref_; }

    template<typename T>
    void register_deserialized_shared(uint32_t id, const std::shared_ptr<T>& ptr) {
        if (id == 0 || !ptr) { return; }
        if constexpr (std::is_polymorphic_v<T>) {
            // Store the complete object + its dynamic type so back-refs through any base
            // view adjust correctly (the aliasing shared_ptr keeps the original owner).
            void* complete = dynamic_cast<void*>(ptr.get());
            store_deserialized(id, std::shared_ptr<void>(ptr, complete), std::type_index(typeid(*ptr)));
        } else {
            store_deserialized(id, std::static_pointer_cast<void>(ptr), std::type_index(typeid(T)));
        }
    }

    template<typename T>
    std::shared_ptr<T> get_deserialized_shared(uint32_t id) const {
        auto adjusted = get_deserialized_shared_adjusted(id, std::type_index(typeid(T)));
        return std::static_pointer_cast<T>(adjusted);
    }

    bool has_deserialized_shared(uint32_t id) const;

    // Type-erased back-ref retrieval: returns the stored object as a void shared_ptr whose
    // pointer is already adjusted from the complete object to the `target` base subobject,
    // so the caller's static_pointer_cast<target> is a plain reinterpret of a correct
    // address. Used by both the templated get above and any_archive_reader.
    std::shared_ptr<void> get_deserialized_shared_adjusted(uint32_t id, std::type_index target) const;

    // Type-erased registration: `complete` is the most-derived pointer, `type` its dynamic
    // type (computed by the caller, which still has the static type).
    void register_deserialized_shared_typed(uint32_t id, std::shared_ptr<void> complete, std::type_index type) {
        if (id != 0 && complete) {
            store_deserialized(id, std::move(complete), type);
        }
    }

    template<typename ContextType>
    ContextType* get_user_context() const {
        return static_cast<ContextType*>(get_user_context_raw(std::type_index(typeid(ContextType))));
    }

    template<typename ContextType>
    void set_user_context(ContextType* context) {
        set_user_context_raw(std::type_index(typeid(ContextType)), const_cast<std::remove_cv_t<ContextType>*>(context));
    }

    // The context set for ti, else the archive's engine when ti names engine, else null
    void* get_user_context_raw(std::type_index ti) const;
    void set_user_context_raw(std::type_index ti, void* context);

    uint32_t get_version() const { return version_; }
};

template<class Derived>
class archive_reader_impl : public archive_reader_core {
public:
    struct is_reader_tag {};

protected:
    Derived* self() { return static_cast<Derived*>(this); }
    const Derived* self() const { return static_cast<const Derived*>(this); }

    struct read_scope {
        archive_reader_impl* reader;
        std::optional<detail::engine_access::load_session> session;
        explicit read_scope(archive_reader_impl* a_reader) : reader(a_reader) {
            if (reader->read_depth_++ == 0 && reader->engine_ref_) { session.emplace(reader->engine_ref_); }
        }
        ~read_scope() { --reader->read_depth_; }
        read_scope(const read_scope&) = delete;
        read_scope& operator=(const read_scope&) = delete;
        void done() {
            if (session) { session->complete(); }
        }
    };

public:
    explicit archive_reader_impl(engine* eng) : archive_reader_core(eng) {}

    template<typename T>
    std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, std::string>>
    serialize(const char* name, T& value) {
        if (!self()->seek_property(name)) {
            throw serialization_error("Property '" + std::string(name) + "' not found during deserialization");
        }
        read_primitive(value);
    }

    template<typename T>
    std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_enum_v<T> && !std::is_same_v<T, std::string>>
    serialize(const char* name, T& value) {
        if (!self()->seek_property(name)) {
            throw serialization_error("Property '" + std::string(name) + "' not found during deserialization");
        }
        read_custom(value);
    }

    template<typename T>
    void operator()(const char* name, T& value) {
        read_scope scope(this);
        serialize(name, value);
        scope.done();
    }

    template<typename T>
    void operator()(const nvp<T>& nv) {
        read_scope scope(this);
        serialize(nv.name, nv.value);
        scope.done();
    }

    template<typename T, typename... Rest>
    void operator()(const nvp<T>& first, const Rest&... rest) {
        read_scope scope(this);
        serialize(first.name, first.value);
        if constexpr (sizeof...(rest) > 0) {
            operator()(rest...);
        }
        scope.done();
    }

private:
    // Helper traits for SFINAE — uses Derived (the concrete archive) instead of a stub type.
    // These traits are only evaluated inside member function template enable_if/if-constexpr,
    // which are instantiated at the call site where Derived is complete.
    // Uses ::jai::access::member_* to detect protected methods via friendship.

    template<typename T, typename = void>
    struct has_versioned_load_method : std::false_type {};
    template<typename T>
    struct has_versioned_load_method<T, std::void_t<decltype(::jai::access::member_load(std::declval<Derived&>(), std::declval<T&>(), std::uint32_t(0)))>> : std::true_type {};

    template<typename T, typename = void>
    struct has_simple_load_method : std::false_type {};
    template<typename T>
    struct has_simple_load_method<T, std::void_t<decltype(::jai::access::member_load(std::declval<Derived&>(), std::declval<T&>()))>> : std::true_type {};

    template<typename T>
    struct has_load_method : std::bool_constant<has_versioned_load_method<T>::value || has_simple_load_method<T>::value> {};

    template<typename T, typename = void>
    struct has_versioned_serialize_method : std::false_type {};
    template<typename T>
    struct has_versioned_serialize_method<T, std::void_t<decltype(::jai::access::member_serialize(std::declval<Derived&>(), std::declval<T&>(), std::uint32_t(0)))>> : std::true_type {};

    template<typename T, typename = void>
    struct has_simple_serialize_method : std::false_type {};
    template<typename T>
    struct has_simple_serialize_method<T, std::void_t<decltype(::jai::access::member_serialize(std::declval<Derived&>(), std::declval<T&>()))>> : std::true_type {};

    template<typename T>
    struct has_serialize_method : std::bool_constant<has_versioned_serialize_method<T>::value || has_simple_serialize_method<T>::value> {};

    template<typename T, typename = void>
    struct has_property_mgr : std::false_type {};
    template<typename T>
    struct has_property_mgr<T, std::void_t<decltype(std::declval<T&>().property_mgr)>> : std::true_type {};

    template<typename T, typename = void>
    struct has_load_and_construct : std::false_type {};
    template<typename T>
    struct has_load_and_construct<T, std::void_t<
        decltype(::jai::access::load_and_construct(
            std::declval<Derived&>(),
            std::declval<construct<T>&>()
        ))
    >> : std::true_type {};

    // ADL-findable load(Archive&, T&) / serialize(Archive&, T&) free functions
    template<typename T> using has_adl_load = adl_detect::has_adl_load<Derived, T>;
    template<typename T> using has_adl_serialize = adl_detect::has_adl_serialize<Derived, T>;

    // Helper to load variant alternatives by index
    template<typename Variant, std::size_t I = 0>
    void load_variant_by_index(Variant& v, std::size_t index) {
        if constexpr (I < std::variant_size_v<Variant>) {
            if (I == index) {
                using T = std::variant_alternative_t<I, Variant>;
                T val{};
                read_element(val);
                v = std::move(val);
            } else {
                load_variant_by_index<Variant, I + 1>(v, index);
            }
        } else {
            throw serialization_error("Invalid variant index: " + std::to_string(index));
        }
    }

public:
    // operator() for types with member load method
    // Prefers versioned load(Archive&, uint32_t), falls back to simple load(Archive&)
    // Note: Does NOT wrap - naming implies nesting. ar(obj) = inline, ar("name", obj) = nested.
    // Uses access:: to support protected methods that friend jai::access
    template<typename T, std::enable_if_t<has_load_method<T>::value, int> = 0>
    void operator()(T& obj) {
        read_scope scope(this);
        if constexpr (has_versioned_load_method<T>::value) {
            ::jai::access::member_load(*self(), obj, 0);
        } else {
            ::jai::access::member_load(*self(), obj);
        }
        scope.done();
    }

    // operator() for types with serialize() method (but no load())
    // Prefers versioned serialize(Archive&, uint32_t), falls back to simple serialize(Archive&)
    // Note: Does NOT wrap - consistent with load() types. Wrapping via ar("name", obj).
    // Uses access:: to support protected methods that friend jai::access
    template<typename T, std::enable_if_t<
        !has_load_method<T>::value &&
        has_serialize_method<T>::value, int> = 0>
    void operator()(T& obj) {
        read_scope scope(this);
        if constexpr (has_versioned_serialize_method<T>::value) {
            ::jai::access::member_serialize(*self(), obj, 0);
        } else {
            ::jai::access::member_serialize(*self(), obj);
        }
        scope.done();
    }

    // operator() for property_owner types WITHOUT custom load() or serialize()
    template<typename T, std::enable_if_t<
        !has_load_method<T>::value &&
        !has_serialize_method<T>::value &&
        has_property_mgr<T>::value, int> = 0>
    void operator()(T& obj) {
        read_scope scope(this);
        obj.load_owned_properties(*self());  // walks the inheritance chain (not just the derived mgr)
        scope.done();
    }

    template<typename T>
    std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, std::string>>
    operator()(T& value) {
        read_primitive(value);
    }

    template<typename T, std::enable_if_t<
        !has_load_method<T>::value &&
        !has_serialize_method<T>::value &&
        !has_property_mgr<T>::value &&
        !std::is_arithmetic_v<T> &&
        !std::is_enum_v<T> &&
        !std::is_same_v<T, std::string>, int> = 0>
    void operator()(T& value) {
        read_scope scope(this);
        read_unscoped(value);
        scope.done();
    }

    template<typename T>
    void read_unscoped(T& value) {
        using namespace serialization_traits;

        if constexpr (is_std_vector_v<T>) {
            using elem_type = vector_element_t<T>;
            size_t size = self()->begin_array();
            value.clear();
            reserve_for_load(value, size);
            for (size_t i = 0; i < size; ++i) {
                elem_type elem{};
                read_element(elem);
                value.push_back(std::move(elem));
            }
            self()->end_array();
        }
        else if constexpr (is_std_array_v<T>) {
            constexpr auto N = array_size_v<T>;
            size_t size = self()->begin_array();
            if (size != N) {
                throw serialization_error("Array size mismatch: expected " + std::to_string(N) +
                                          ", got " + std::to_string(size));
            }
            for (size_t i = 0; i < N; ++i) {
                read_element(value[i]);
            }
            self()->end_array();
        }
        else if constexpr (is_std_map_v<T> || is_std_unordered_map_v<T>) {
            using K = typename T::key_type;
            using V = typename T::mapped_type;
            value.clear();
            if constexpr (std::is_same_v<K, std::string>) {
                size_t size = self()->begin_map();
                for (size_t i = 0; i < size; ++i) {
                    std::string key;
                    if (!self()->read_map_key(key)) break;
                    V val{};
                    read_element(val);
                    value[key] = std::move(val);
                }
                self()->end_map();
            } else {
                size_t size = self()->begin_array();
                for (size_t i = 0; i < size; ++i) {
                    self()->begin_array();
                    K key{};
                    V val{};
                    read_element(key);
                    read_element(val);
                    self()->end_array();
                    value[std::move(key)] = std::move(val);
                }
                self()->end_array();
            }
        }
        else if constexpr (is_set_like_v<T>) {
            using elem_type = typename T::key_type;
            size_t size = self()->begin_array();
            value.clear();
            for (size_t i = 0; i < size; ++i) {
                elem_type elem{};
                read_element(elem);
                value.insert(std::move(elem));
            }
            self()->end_array();
        }
        else if constexpr (is_std_pair_v<T>) {
            self()->begin_array();
            read_element(value.first);
            read_element(value.second);
            self()->end_array();
        }
        // std::shared_ptr - format: {"$id": N, "$val": {...}} or {"$id": N} or null
        // Polymorphic: {"$id": N, "$type": "ConcreteType", "$val": {...}}
        else if constexpr (is_std_shared_ptr_v<T>) {
            using elem_type = shared_ptr_element_t<T>;
            if (self()->peek_null()) {
                self()->read_null();
                value.reset();
                return;
            }
            self()->begin_object();
            uint32_t id{};
            self()->serialize("$id", id);
            if (self()->has_deserialized_shared(id)) {
                value = self()->template get_deserialized_shared<elem_type>(id);
            } else {
                // A payload-less {"$id": N} is a back-reference by construction (the writer
                // inlines every forward reference), so one that names nothing loaded so far is
                // a dangling reference: a corrupt or hand-edited file. Loading it as null used
                // to surface later as a null dereference.
                if (id != 0 && !self()->has_property("$val") && !self()->has_property("$type")) {
                    throw serialization_error("Dangling shared_ptr reference: {\"$id\": " + std::to_string(id) + "} names an object this archive never defined (" + std::string(typeid(elem_type).name()) + ")");
                }
                // First (data-bearing) encounter of this id. read_shared_payload handles
                // polymorphic / load_and_construct / default construction and registers the
                // object eagerly under id.
                value = self()->template read_shared_payload<elem_type>(id);
            }
            self()->end_object();
        }
        // std::weak_ptr - format: {"$ref": N, "$val"?: {...}} or null. "$val" appears when
        // the weak_ptr is the first (forward) reference to the object; we then reconstruct
        // and register it so later shared/weak refs to this id resolve.
        else if constexpr (is_std_weak_ptr_v<T>) {
            using elem_type = weak_ptr_element_t<T>;
            if (self()->peek_null()) {
                self()->read_null();
                value.reset();
                return;
            }
            self()->begin_object();
            uint32_t id{};
            self()->serialize("$ref", id);
            if (self()->has_deserialized_shared(id)) {
                value = self()->template get_deserialized_shared<elem_type>(id);
            } else if (self()->has_property("$val") || self()->has_property("$type")) {
                value = self()->template read_shared_payload<elem_type>(id);
            } else {
                value.reset();
            }
            self()->end_object();
        }
        // std::unique_ptr - format: {"$val": {...}} or null
        else if constexpr (is_std_unique_ptr_v<T>) {
            using elem_type = unique_ptr_element_t<T>;
            if (self()->peek_null()) {
                self()->read_null();
                value.reset();
                return;
            }
            self()->begin_object();
            if constexpr (::jai::serialization::load_default_constructible<elem_type>) {
                value = ::jai::serialization::make_unique_default_for_load<elem_type>();
                self()->serialize("$val", *value);
            } else {
                throw serialization_error("Cannot deserialize unique_ptr<T>: type is not default constructible");
            }
            self()->end_object();
        }
        else if constexpr (is_std_optional_v<T>) {
            using elem_type = optional_element_t<T>;
            self()->begin_array();
            bool has_value = self()->read_bool();
            if (has_value) {
                elem_type val{};
                read_element(val);
                value = std::move(val);
            } else {
                value = std::nullopt;
            }
            self()->end_array();
        }
        else if constexpr (is_std_variant_v<T>) {
            self()->begin_array();
            uint32_t index;
            read_element(index);
            load_variant_by_index(value, static_cast<std::size_t>(index));
            self()->end_array();
        }
        else if constexpr (is_std_tuple_v<T>) {
            self()->begin_array();
            std::apply([this](auto&... args) { (read_element(args), ...); }, value);
            self()->end_array();
        }
        else if constexpr (is_jai_property_v<T>) {
            using element_type = property_element_t<T>;
            element_type temp{};
            (*self())(temp);
            value = std::move(temp);
        }
        // Fallback: try ADL serialize(), then ADL load(), or provide compile-time error
        else if constexpr (has_adl_serialize<T>::value) {
            // ADL serialize functions read named properties, so expect object wrapper
            self()->begin_object();
            serialize(*self(), value);
            self()->end_object();
        }
        else if constexpr (has_adl_load<T>::value) {
            // ADL load functions read named properties, so expect object wrapper
            self()->begin_object();
            load(*self(), value);
            self()->end_object();
        } else {
            throw serialization_error(
                "Type '" + std::string(typeid(T).name()) + "' is not deserializable. "
                "Add save/load, serialize, property_owner, or ADL functions — or mark the property transient.");
        }
    }

    template<typename T>
    std::enable_if_t<std::is_arithmetic_v<T> || std::is_same_v<T, std::string>>
    read_element(T& value) {
        read_primitive(value);
    }

    template<typename T>
    std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_same_v<T, std::string>>
    read_element(T& value) {
        // For custom types with serialize/load methods, expect object wrapper
        // since their serialize() will read named properties.
        // DON'T wrap smart pointers - they have their own array format,
        // and their pointed-to object will be wrapped when read_element(*ptr) is called.
        if constexpr (has_load_method<T>::value || has_serialize_method<T>::value ||
                      has_property_mgr<T>::value) {
            self()->begin_object();
            (*self())(value);
            self()->end_object();
        } else {
            (*self())(value);
        }
    }

private:
    template<typename T>
    void read_primitive(T& value) {
        if constexpr (std::is_same_v<T, int8_t>) value = self()->read_int8();
        else if constexpr (std::is_same_v<T, int16_t>) value = self()->read_int16();
        else if constexpr (std::is_same_v<T, int32_t>) value = self()->read_int32();
        else if constexpr (std::is_same_v<T, int64_t>) value = self()->read_int64();
        else if constexpr (std::is_same_v<T, uint8_t>) value = self()->read_uint8();
        else if constexpr (std::is_same_v<T, uint16_t>) value = self()->read_uint16();
        else if constexpr (std::is_same_v<T, uint32_t>) value = self()->read_uint32();
        else if constexpr (std::is_same_v<T, uint64_t>) value = self()->read_uint64();
        else if constexpr (std::is_same_v<T, float>) value = self()->read_float32();
        else if constexpr (std::is_same_v<T, double>) value = self()->read_float64();
        else if constexpr (std::is_same_v<T, bool>) value = self()->read_bool();
        else if constexpr (std::is_same_v<T, std::string>) value = self()->read_string();
        else if constexpr (std::is_enum_v<T>) { std::underlying_type_t<T> underlying; read_primitive(underlying); value = static_cast<T>(underlying); }
        else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T> && sizeof(T) == 8) value = static_cast<T>(self()->read_int64());   // the writer's bit pattern
        else if constexpr (std::is_integral_v<T>) value = archive_integer<T>(self()->read_int64());
        else if constexpr (std::is_floating_point_v<T>) value = static_cast<T>(self()->read_float64());
    }

    // Reconstructs the inlined payload ($type/$val) of a shared object at the current
    // object position, registers it under `id` (eagerly, so cyclic back-references inside
    // its own subtree resolve), and returns it. Shared by the shared_ptr read path and the
    // forward-referenced weak_ptr read path so they cannot drift apart. Returns null when no
    // payload is present (a pure back-reference whose target is registered elsewhere).
    template<typename elem_type>
    std::shared_ptr<elem_type> read_shared_payload(uint32_t id) {
        std::shared_ptr<elem_type> result;
        if constexpr (std::is_polymorphic_v<elem_type>) {
            std::string poly_type;
            if (self()->has_property("$type")) {
                self()->serialize("$type", poly_type);
            }
            if (!poly_type.empty()) {
                auto* entry = polymorphic_registry::instance().find(poly_type);
                std::string hybrid_class;
                std::string hybrid_base;
                if (!entry && engine_ref_) {
                    // A $type naming a script class deriving a bound C++ base: load through
                    // the base's registry entry, then re-attach the script layer below.
                    std::string base_name = detail::engine_access::script_layer_base_name(*engine_ref_, poly_type);
                    if (!base_name.empty()) {
                        entry = polymorphic_registry::instance().find(base_name);
                        if (entry) {
                            hybrid_class = poly_type;
                            hybrid_base = base_name;
                        }
                    }
                }
                // Reject a tampered $type that names a registered type unrelated to the
                // requested base BEFORE constructing it — otherwise the void->elem_type cast
                // below would reinterpret an unrelated object as elem_type (type confusion
                // reachable from untrusted payloads). B5.
                const auto refuse_unrelated = [&]() {
                    if (!polymorphic_registry::instance().is_assignable(entry->type, std::type_index(typeid(elem_type)))) {
                        throw serialization_error("Polymorphic type '" + poly_type + "' is not a registered subtype of requested base '" +
                            std::type_index(typeid(elem_type)).name() + "' (declare it: static jai::serialization::register_relation<" +
                            poly_type + ", Base>, or .base<Base>() on its registrar)");
                    }
                };
                if (entry && !hybrid_class.empty() && entry->load_into_fn) {
                    // A script class with a zero-argument constructor is constructed (its initializer list builds the C++
                    // base), then the saved values load over it: the base's properties into that object, then the script
                    // fields, then post_load. The layer owns the object it constructed, so the pointer aliases the layer.
                    refuse_unrelated();
                    std::shared_ptr<void> constructed_object;
                    auto constructed = detail::engine_access::construct_script_layer_for_load(*engine_ref_, hybrid_class, constructed_object);
                    if (constructed) {
                        std::string hybrid_fields;
                        if (self()->has_property("$script")) {
                            self()->serialize("$script", hybrid_fields);
                        }
                        if (!self()->seek_property("$val")) {
                            throw serialization_error("Expected '$val' for polymorphic shared_ptr");
                        }
                        void* adjusted = polymorphic_registry::instance().adjust_to_base(entry->type, std::type_index(typeid(elem_type)), constructed_object.get());
                        result = std::shared_ptr<elem_type>(detail::shared_owner_of(constructed), static_cast<elem_type*>(adjusted));
                        self()->register_deserialized_shared(id, result);
                        self()->begin_object();
                        any_archive_reader wrapper(*self());
                        entry->load_into_fn(wrapper, constructed_object.get());
                        self()->end_object();
                        auto hydrated = detail::engine_access::hydrate_script_layer(*engine_ref_, constructed, hybrid_fields);
                        if (!hydrated) {
                            throw serialization_error(engine_ref_->format_error(hydrated));
                        }
                        return result;
                    }
                }
                if (entry && entry->load_fn) {
                    refuse_unrelated();
                    std::string hybrid_fields;
                    if (!hybrid_class.empty() && self()->has_property("$script")) {
                        self()->serialize("$script", hybrid_fields);
                    }
                    if (!self()->seek_property("$val")) {
                        throw serialization_error("Expected '$val' for polymorphic shared_ptr");
                    }
                    self()->begin_object();
                    any_archive_reader wrapper(*self());
                    // Pass id so load_fn registers the object eagerly (before its
                    // properties load), enabling subtree back-references to resolve.
                    auto void_ptr = entry->load_fn(wrapper, id);
                    // void_ptr points at the complete (Derived) object; adjust to the Base
                    // subobject for the caller's shared_ptr<elem_type> (offset != 0 under MI).
                    void* adjusted = polymorphic_registry::instance().adjust_to_base(entry->type, std::type_index(typeid(elem_type)), void_ptr.get());
                    result = std::shared_ptr<elem_type>(void_ptr, static_cast<elem_type*>(adjusted));
                    self()->end_object();
                    if (!hybrid_class.empty()) {
                        // Re-attach the script layer. An object with a script_extensible slot owns
                        // it; otherwise the layer owns the object and the returned pointer aliases
                        // the layer, so the layer lives exactly as long as the loaded object.
                        auto instance = detail::engine_access::extend_loaded(*engine_ref_, hybrid_class, void_ptr, hybrid_fields);
                        if (!instance) {
                            throw serialization_error("Failed to reconstruct hybrid script class '" + hybrid_class + "'");
                        }
                        if (!detail::engine_access::script_layer_object_owned(*engine_ref_, instance)) {
                            result = std::shared_ptr<elem_type>(detail::shared_owner_of(instance), static_cast<elem_type*>(adjusted));
                        }
                    }
                    self()->register_deserialized_shared(id, result);
                    return result;
                } else if (entry) {
                    // registered, but neither load_and_construct nor a default constructor (public, or private to
                    // jai::access) can build it
                    throw serialization_error(hybrid_class.empty()
                        ? "Cannot deserialize '" + poly_type + "': type is not default constructible and has no load_and_construct"
                        : "Cannot load hybrid script class '" + hybrid_class + "': its C++ base '" + hybrid_base +
                          "' is not default constructible and has no load_and_construct");
                } else {
                    throw serialization_error("Unknown polymorphic type: " + poly_type);
                }
            }
            // No $type discriminator: fall through to concrete reconstruction below.
        }
        if (self()->has_property("$val")) {
            if constexpr (has_load_and_construct<elem_type>::value) {
                if (!self()->seek_property("$val")) {
                    throw serialization_error("Expected '$val' for shared_ptr with load_and_construct");
                }
                std::string type_name;
                uint32_t version = 0;
                self()->begin_object(type_name, version);
                construct<elem_type> c(result);
                c.set_on_construct([&]() { self()->register_deserialized_shared(id, result); });
                ::jai::access::load_and_construct(*self(), c);
                self()->end_object();
            } else if constexpr (::jai::serialization::load_default_constructible<elem_type>) {
                result = ::jai::serialization::make_default_for_load<elem_type>();
                self()->register_deserialized_shared(id, result);
                self()->serialize("$val", *result);
            } else {
                throw serialization_error("Cannot deserialize shared_ptr<" + std::string(typeid(elem_type).name()) + ">: type is not default constructible and has no load_and_construct");
            }
        }
        return result;
    }

public:
    // Made public for use by smart pointer serialization in property_serialization.hpp
    template<typename T>
    void read_custom(T& value) {
        if constexpr (has_versioned_load_method<T>::value) {
            std::string type_name;
            uint32_t version;
            self()->begin_object(type_name, version);
            ::jai::access::member_load(*self(), value, version);
            self()->end_object();
        }
        else if constexpr (has_simple_load_method<T>::value) {
            std::string type_name;
            uint32_t version;
            self()->begin_object(type_name, version);
            ::jai::access::member_load(*self(), value);
            self()->end_object();
        }
        else if constexpr (has_versioned_serialize_method<T>::value) {
            std::string type_name;
            uint32_t version;
            self()->begin_object(type_name, version);
            ::jai::access::member_serialize(*self(), value, version);
            self()->end_object();
        }
        else if constexpr (has_simple_serialize_method<T>::value) {
            std::string type_name;
            uint32_t version;
            self()->begin_object(type_name, version);
            ::jai::access::member_serialize(*self(), value);
            self()->end_object();
        }
        else if constexpr (has_property_mgr<T>::value) {
            std::string type_name;
            uint32_t version;
            self()->begin_object(type_name, version);
            value.load_owned_properties(*self());  // walks the inheritance chain
            self()->end_object();
        }
        else {
            (*self())(value);
        }
    }
};

class serialization_registry {
public:
    serialization_registry() = default;

    void register_class(const std::string& class_name, const class_metadata& metadata) {
        classes_[class_name] = metadata;
    }

    const class_metadata* get_class_metadata(const std::string& class_name) const {
        auto it = classes_.find(class_name);
        return it != classes_.end() ? &it->second : nullptr;
    }

private:
    std::map<std::string, class_metadata> classes_;
};

// NOTE: STL container serialization (vector, array, map, unordered_map, pair,
// optional, variant, tuple) and jai::property<T> are now handled internally by
// archive_writer_impl::operator() and archive_reader_impl::operator().
// No ADL free functions are needed. This avoids conflicts with Cereal's ADL-based
// serialization.

} // namespace serialization
} // namespace jai

// Include polymorphic registry after archive classes are defined
// (polymorphic.hpp needs archive types, archive templates need polymorphic_registry)
#include <jaiscript/serialization/polymorphic.hpp>

#endif // JAISCRIPT_SERIALIZATION_ARCHIVE_IMPL_HPP
