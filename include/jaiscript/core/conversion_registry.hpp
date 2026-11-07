#pragma once

#ifndef __JAISCRIPT_CORE_CONVERSION_REGISTRY_HPP__
#define __JAISCRIPT_CORE_CONVERSION_REGISTRY_HPP__

#include "conversion_registry_fwd.hpp"
#include "types.hpp"
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>
#include <memory>
#include <typeinfo>
#include <typeindex>
#include <type_traits>

namespace jai::detail { class object_ref; }
#include <algorithm>

namespace jai {

// Forward declaration
class script_value;

// Forward declarations
class engine;

namespace conversions {

// std::runtime_error(prefix + type_name), out of line so the registry templates carry one call
[[noreturn]] JAI_NOINLINE void throw_unregistered_conversion(const char* prefix, const char* type_name);

// type_id is already defined in conversion_registry_fwd.hpp

// A registration is two function pointers over shared state, so registering a type instantiates no std::function; null
// pointers mark a standard std::vector/std::map shape, converted by the code of the caller that names the type
struct conversion_entry {
    void (*from_script)(const conversion_entry& entry, const script_value& v, void* out) = nullptr;   // emplaces into the caller's std::optional<T>
    script_value (*to_script)(const conversion_entry& entry, const void* value) = nullptr;
    engine* eng = nullptr;
    std::shared_ptr<void> state;
};

// Registry for custom conversions
class conversion_registry {
private:
    // Raw pointer to the owning engine
    engine* engine_ref_ = nullptr;

public:
    conversion_registry() = default;
    explicit conversion_registry(engine* eng) : engine_ref_(eng) {}
    ~conversion_registry() = default;

    // Non-copyable but moveable
    conversion_registry(const conversion_registry&) = delete;
    conversion_registry& operator=(const conversion_registry&) = delete;
    conversion_registry(conversion_registry&&) = default;
    conversion_registry& operator=(conversion_registry&&) = default;

    // Set engine reference (for registries created before engine)
    void set_engine(engine* eng) { engine_ref_ = eng; }

    // Get engine pointer for conversions
    engine* get_engine() const {
        return engine_ref_;
    }
    
    // The overload-scoring cost of an implicit conversion between built-in kinds (0 for the same kind, 1000 for none)
    int get_builtin_conversion_cost(script_value_type from, script_value_type to) const;
    
    // === Custom Type Conversions ===
    
    // Register a custom conversion for type T
    template<typename T>
    void register_conversion(
        std::function<T(const script_value&)> from_func,
        std::function<script_value(const T&)> to_func
    );
    
    // Replaces any earlier registration for the type
    void register_entry(type_id type, conversion_entry entry);
    const conversion_entry* find_entry(type_id type) const;

    // Check if conversion exists for type T
    template<typename T>
    bool has_conversion() const { return find_entry(type_id::of<T>()) != nullptr; }
    
    // Convert from script_value to T using registered conversion
    template<typename T>
    T convert_from_script(const script_value& v) const;
    
    // Convert from T to script_value using registered conversion
    template<typename T>
    script_value convert_to_script(const T& value) const;
    
    // === Custom Object Extractor (replaces static extractor in script_value) ===
    
    // Type for custom extractor function
    using extractor_func = std::function<std::shared_ptr<void>(std::string_view, const detail::object_ref&)>;
    
    // Set custom extractor for unwrapping objects (e.g., extracting C++ objects from class_instance)
    void set_custom_extractor(extractor_func extractor) {
        custom_extractor_ = std::move(extractor);
    }
    
    // Get custom extractor
    const extractor_func& get_custom_extractor() const {
        return custom_extractor_;
    }
    
    // Extract custom object
    std::shared_ptr<void> extract_custom_object(std::string_view type_name, const detail::object_ref& data) const {
        if (custom_extractor_) {
            return custom_extractor_(type_name, data);
        }
        return nullptr;
    }
    
private:
    // Hashed on std::type_index: libstdc++'s type_info::before is not a strict weak order between internal-linkage and other types
    std::unordered_map<std::type_index, conversion_entry> conversions_;
    
    // Custom object extractor
    extractor_func custom_extractor_;
};

// Forward declarations for helper functions - implementations in conversion_registry_impl.hpp
template<typename T>
std::vector<T> convert_script_array_to_vector(const script_value& v, engine* eng);

template<typename K, typename V>
std::map<K, V> convert_script_map_to_stdmap(const script_value& v, engine* eng);

} // namespace conversions

// Convenience functions for engine
namespace conversions {

// Add conversion functions to engine
class conversion_manager {
public:
    explicit conversion_manager(std::shared_ptr<conversion_registry> registry, engine* eng) 
        : registry_(std::move(registry)), engine_(eng) {}
    
    // Register standard conversions for common vector types
    template<typename T>
    void add_vector_conversion();
    
    // Register standard conversions for common map types
    template<typename K, typename V>
    void add_map_conversion();
    
    // Register conversions for bound_array types
    template<typename T>
    void add_bound_array_conversion();
    
    // Register conversions for bound_map types
    template<typename K, typename V>
    void add_bound_map_conversion();
    
    // Register custom conversion
    template<typename T>
    void add_custom_conversion(
        std::function<T(const script_value&)> from_func,
        std::function<script_value(const T&)> to_func
    );
    
private:
    std::shared_ptr<conversion_registry> registry_;
    engine* engine_;
};

// Function to register all standard conversions into a registry
void register_all_standard_conversions(std::shared_ptr<conversion_registry> registry);

} // namespace conversions

} // namespace jai

#endif // __JAISCRIPT_CORE_CONVERSION_REGISTRY_HPP__