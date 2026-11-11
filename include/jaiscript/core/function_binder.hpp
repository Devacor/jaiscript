// Part of the engine's closure, which includes it in its order: a translation unit that names it first is sent there
#if !defined(__JAISCRIPT_CORE_ENGINE_HPP__)
#include "engine.hpp"
#elif !defined(__JAISCRIPT_CORE_FUNCTION_BINDER_HPP__)
#define __JAISCRIPT_CORE_FUNCTION_BINDER_HPP__

#include "types.hpp"
#include "value.hpp"
#include "conversion_registry.hpp"
#include "conversion_registry_impl.hpp"
#include "bound_array.hpp"
#include "bound_map.hpp"
#include "parameter_storage.hpp"
#include "runtime_errors.hpp"
#include <jaiscript/detail/numeric_conversion.hpp>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <tuple>

// Forward declarations to avoid circular dependencies
namespace jai {
    class engine;

    // Helper function to convert custom types using engine's conversion registry
    // Implementation in engine_impl.hpp
    template<typename T>
    script_value convert_custom_type_with_registry(const T& t, engine* eng);

    // Helper function to convert C++ reference to script_value
    // For registered types, creates non-owning reference via make_cpp_bound
    // For unregistered types, falls back to copy semantics
    // Implementation in engine_impl.hpp
    template<typename T>
    script_value convert_reference_with_registry(T& t, engine* eng);

    // For registered classes, wraps a shared_ptr as an engine object (shared ownership -
    // identity and mutation preserved); returns nullopt for unregistered types.
    // Implementation in engine_impl.hpp
    template<typename T>
    std::optional<script_value> try_convert_shared_ptr_with_registry(const std::shared_ptr<T>& ptr, engine* eng);

    // A host callable's C++ exception as the script's catchable error: its what() parked, never interned (a host call that
    // fails in a loop with a varying text leaves the symbol table as it was); a frozen region's worker gets the slot-less text
    inline constexpr std::string_view host_exception_slotless_text = "A host call failed during a parallel region";
    // (engine is incomplete here; the dependent E defers the lookup to the instantiation point)
    template<typename E>
    error_propagator host_exception_error(E* eng, const char* text) {
        const uint64_t id = eng ? detail::engine_access_of<E>::park_error_text(*eng, text) : 0;
        return error_propagator{make_error_code(runtime_error_code::cpp_exception),
                                id || !eng ? std::string_view("{0}") : host_exception_slotless_text, id, 0};
    }

    // Wraps a script FUNCTION value as a std::function<Sig> so script lambdas can be
    // assigned into C++ callback members/parameters. Implementation in engine_impl.hpp
    // (argument conversion needs the complete engine).
    template<typename Sig>
    std::function<Sig> convert_script_function(const script_value& fn);
}
#include <type_traits>
#include <typeinfo>

namespace jai {

// Forward declarations
class class_instance;
class engine;

    // Type traits to help with function binding
    namespace detail {
        
        // Helper to convert C++ types to/from value
        // Note: This generic template only works for copyable types.
        // Non-copyable types should be handled via shared_ptr<T> specialization or T& references.
        template<typename T>
        struct value_converter {
            static T from(const script_value& v, engine* eng)
                requires std::is_copy_constructible_v<T> || is_specialization_v<T, std::vector> || is_script_map_like_v<T>
            {
                // Handle containers explicitly to avoid infinite recursion
                if constexpr (std::is_same_v<T, script_map>) {
                    // Native map type (custom transparent comparator): copy out directly —
                    // the generic std::map branch would rebuild with std::less
                    return v.as_map();
                } else if constexpr (is_specialization_v<T, std::vector>) {
                    using element_type = typename T::value_type;
                    return conversions::convert_script_array_to_vector<element_type>(v, eng);
                } else if constexpr (is_script_map_like_v<T>) {
                    using key_type = typename T::key_type;
                    using value_type = typename T::mapped_type;
                    return conversions::convert_script_map_to_stdmap<key_type, value_type>(v, eng);
                } else if constexpr (std::is_enum_v<T>) {
                    return static_cast<T>(v.as<script_int>());
                } else if constexpr (std::is_class_v<T> && !std::is_same_v<T, std::string>) {
                    if (v.is_object()) {
                        // a borrowed object has no owner to share: copy it without the shared attempt's throw
                        if (const auto* holder = v.peek_object_holder(); holder && holder->bound_object() && !holder->holds_object()) {
                            return v.as<T>();
                        }
                        try {
                            auto ptr = v.as<std::shared_ptr<T>>();
                            return *ptr;
                        } catch (const std::exception&) {
                            return v.as<T>();
                        }
                    } else {
                        return v.as<T>();
                    }
                } else {
                    // For basic types, use standard conversion
                    return v.as<T>();
                }
            }
            
            static script_value to(const T& t, engine* eng) {
                // Handle standard containers explicitly since script_value doesn't have constructors for them
                if constexpr (std::is_same_v<T, script_map>) {
                    // Native map type (custom transparent comparator): wrap directly
                    auto map_value = script_value::make_map(nullptr, nullptr, eng);
                    const_cast<script_map&>(map_value.as_map()) = t;
                    return map_value;
                } else if constexpr (is_specialization_v<T, std::vector>) {
                    // For vectors, use the conversion utility with engine for registry access
                    using element_type = typename T::value_type;
                    return conversions::convert_vector_to_script_array<element_type>(t, eng);
                } else if constexpr (is_script_map_like_v<T>) {
                    // For maps, use the conversion utility
                    using key_type = typename T::key_type;
                    using value_type = typename T::mapped_type;
                    return conversions::convert_stdmap_to_script_map<key_type, value_type>(t, eng);
                } else if constexpr (std::is_class_v<T> &&
                             !std::is_same_v<T, std::string>) {
                    // For user classes, use the conversion registry if available
                    // Implementation moved to engine_impl.hpp to avoid circular dependencies
                    if (!eng) {
                        throw runtime_error("Engine reference required for custom type conversion");
                    }
                    return convert_custom_type_with_registry<T>(t, eng);
                } else if constexpr (std::is_enum_v<T>) {
                    if (!eng) {
                        throw runtime_error("Engine reference required for script_value creation");
                    }
                    return script_value(static_cast<script_int>(t), eng);
                } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T> ||
                                     std::is_same_v<T, bool> || std::is_same_v<T, std::string>) {
                    // For basic types, use the script_value constructor with engine
                    if (!eng) {
                        throw runtime_error("Engine reference required for script_value creation");
                    }
                    return script_value(t, eng);
                } else {
                    static_assert(std::is_integral_v<T> || std::is_floating_point_v<T> ||
                                  std::is_same_v<T, bool> || std::is_same_v<T, std::string> ||
                                  std::is_class_v<T> || std::is_enum_v<T>,
                        "value_converter<T>::to() does not support this type. "
                        "Register a custom converter or use a supported type.");
                    throw runtime_error("Unsupported type for script_value conversion");
                }
            }
        };
        
        // Specialization for std::function: script lambdas convert into C++ callbacks
        // (the spawn/update hook pattern). Reading a callback back yields null —
        // C++ callables are opaque to scripts.
        template<typename Sig>
        struct value_converter<std::function<Sig>> {
            static checked_result<std::function<Sig>> checked_from(const script_value& v, engine* eng) {
                return from(v, eng);
            }

            static std::function<Sig> from(const script_value& v, engine* /*eng*/) {
                if (!v.is_function()) {
                    return {};
                }
                return convert_script_function<Sig>(v);
            }

            static script_value to(const std::function<Sig>& /*v*/, engine* eng) {
                return script_value(std::monostate{}, eng);
            }
        };

        // Specialization for int (convert to script_int/int64_t)
        template<>
        struct value_converter<int> {
            // Non-throwing version - returns checked_result for error propagation
            static checked_result<int> checked_from(const script_value& v, engine*) {
                return v.checked_as<int>();
            }

            // Throwing version - for backward compatibility with existing bindings
            static int from(const script_value& v, engine* eng) {
                auto result = checked_from(v, eng);
                if (!result) [[unlikely]] {
                    throw_numeric_error(result.message(), result.symbol_id(), result.symbol_id2());
                }
                return result.value();
            }

            static script_value to(const int& t, engine* eng) {
                if (!eng) {
                    throw runtime_error("Engine reference required for script_value creation");
                }
                return script_value(static_cast<script_int>(t), eng);
            }
        };

        // Specialization for float (convert to script_float/double)
        template<>
        struct value_converter<float> {
            // Non-throwing version
            static checked_result<float> checked_from(const script_value& v, engine*) {
                return v.checked_as<float>();
            }

            static float from(const script_value& v, engine* eng) {
                auto result = checked_from(v, eng);
                if (!result) [[unlikely]] {
                    throw_numeric_error(result.message(), result.symbol_id(), result.symbol_id2());
                }
                return result.value();
            }

            static script_value to(const float& t, engine* eng) {
                if (!eng) {
                    throw runtime_error("Engine reference required for script_value creation");
                }
                return script_value(static_cast<script_float>(t), eng);
            }
        };

        // Specialization for double - handle int-to-double conversion
        template<>
        struct value_converter<double> {
            // Non-throwing version
            static checked_result<double> checked_from(const script_value& v, engine*) {
                return v.checked_as<double>();
            }

            static double from(const script_value& v, engine* eng) {
                auto result = checked_from(v, eng);
                if (!result) [[unlikely]] {
                    throw_numeric_error(result.message(), result.symbol_id(), result.symbol_id2());
                }
                return result.value();
            }

            static script_value to(const double& t, engine* eng) {
                if (!eng) {
                    throw runtime_error("Engine reference required for script_value creation");
                }
                return script_value(t, eng);  // script_float is double
            }
        };

        // A char parameter takes a char or a string of exactly one character, as a script char slot does
        template<>
        struct value_converter<char> {
            static checked_result<char> checked_from(const script_value& v, engine*) {
                const script_value& actual = v.deref();
                if (actual.type() == script_value_type::jai_string_type) {
                    return actual.raw_storage_index() == script_value::TYPEID_STRING ? char_of_text(actual.unchecked_as_string())
                                                                                     : char_of_text(actual.to_string());
                }
                return v.checked_as<char>();
            }

            static char from(const script_value& v, engine* eng) {
                auto result = checked_from(v, eng);
                if (!result) [[unlikely]] {
                    throw_numeric_error(result.message(), result.symbol_id(), result.symbol_id2());
                }
                return result.value();
            }

            static script_value to(const char& t, engine* eng) {
                if (!eng) {
                    throw runtime_error("Engine reference required for script_value creation");
                }
                return script_value(static_cast<script_char>(t), eng);
            }
        };
        template<>
        struct value_converter<const char&> : value_converter<char> {};   // a one-character string holds no char to bind: the value

        // Specialization for script_value itself (no conversion needed!)
        template<>
        struct value_converter<script_value> {
            static const script_value& from(const script_value& v, engine*) {
                return v;
            }

            static script_value to(const script_value& v, engine*) {
                return v;  // Return as-is, no wrapping!
            }
        };

        // A held value (an execute result a bound function returns) goes back in as the plain value it holds
        template<>
        struct value_converter<held_value> {
            static script_value to(const script_value& v, engine*) { return v; }
        };

        // Specialization for const script_value& - pass through directly
        template<>
        struct value_converter<const script_value&> {
            static const script_value& from(const script_value& v, engine*) {
                return v;
            }
        };

        // Specialization for std::shared_ptr<T> - auto-unwrap to T
        template<typename T>
        struct value_converter<std::shared_ptr<T>> {
            static std::shared_ptr<T> from(const script_value& v, engine*) {
                return v.as<std::shared_ptr<T>>();
            }
            
            static script_value to(const std::shared_ptr<T>& ptr, engine* eng) {
                if (!ptr) {
                    if (!eng) {
                        throw runtime_error("Engine reference required for null script_value creation");
                    }
                    return script_value(std::monostate{}, eng); // Return null for nullptr
                }

                // Registered classes share ownership (identity + mutation preserved; also the
                // only lossless path for non-copyables like Node/Interface).
                if constexpr (std::is_class_v<T> && !std::is_same_v<T, std::string>) {
                    if (auto made = try_convert_shared_ptr_with_registry(ptr, eng)) {
                        return std::move(*made);
                    }
                }

                // Auto-unwrap: dereference the shared_ptr and convert the underlying object
                // This delegates to the base value_converter<T> which may have custom handling
                return value_converter<T>::to(*ptr, eng);
            }
        };
        
        template<typename T>
        struct value_converter<std::weak_ptr<T>> {
            static std::weak_ptr<T> from(const script_value& v, engine*) {
                return v.as<std::weak_ptr<T>>();
            }

            static script_value to(const std::weak_ptr<T>& ptr, engine* eng) {
                if (!eng) {
                    throw runtime_error("Engine reference required for custom type conversion");
                }
                return convert_custom_type_with_registry<std::weak_ptr<T>>(ptr, eng);
            }
        };

        // Specializations for reference types - ZERO-COPY for built-in types
        template<typename T>
        struct value_converter<T&> {
            static T& from(const script_value& v, engine* eng) {
                if constexpr (math_adapted<T>) {
                    auto* storage = detail::get_engine_parameter_storage(eng);
                    if (!storage) {
                        throw runtime_error("No parameter storage available for reference conversion");
                    }
                    std::remove_cv_t<T>& temp = storage->allocate<std::remove_cv_t<T>>();
                    temp = v.as<std::remove_cv_t<T>>();
                    return temp;
                }
                // Get the actual target value (dereferences if v is a reference)
                script_value& target = const_cast<script_value&>(v).deref();
                
                // ZERO-COPY: Direct mutable access to stored values using safe accessors
                if constexpr (std::is_same_v<T, script_int> || std::is_same_v<T, int64_t>) {
                    // Direct reference to stored int - ZERO COPY
                    return target.as_int_ref();
                }
                else if constexpr (std::is_same_v<T, std::string>) {
                    // Direct reference to stored string - ZERO COPY
                    return target.as_string_ref(eng);
                }
                else if constexpr (std::is_same_v<T, std::vector<script_value>>) {
                    // Direct reference to stored array - ZERO COPY
                    return target.as_array();
                }
                else if constexpr (std::is_same_v<T, script_map>) {
                    // Direct reference to stored map - ZERO COPY
                    return target.as_map();
                }
                else {
                    // Fall back to the as<T&>() method for other types (including int conversions)
                    return target.as<T&>();
                }
            }
            
            static script_value to(T& t, engine* eng) {
                // Smart pointers route to their own specialization — the registry path
                // would look up a class literally named "std::shared_ptr<T>".
                if constexpr (is_specialization_v<T, std::shared_ptr>) {
                    return value_converter<T>::to(t, eng);
                }
                else if constexpr (math_adapted<T>) {
                    return convert_custom_type_with_registry<std::remove_cv_t<T>>(t, eng);
                }
                // avoids copying non-copyable types and preserves reference semantics
                else if constexpr (std::is_class_v<T> &&
                             !std::is_same_v<T, std::string> &&
                             !is_specialization_v<T, std::vector> &&
                             !is_script_map_like_v<T>) {
                    return convert_reference_with_registry<T>(t, eng);
                }
                // For basic types, delegate to const T& version
                return value_converter<const T&>::to(t, eng);
            }
        };
        
        // Specialization for const references
        template<typename T>
        struct value_converter<const T&> {
            static const T& from(const script_value& v, engine* eng) {
                // For std::string, return reference directly
                if constexpr (std::is_same_v<T, std::string>) {
                    return v.as_string();
                }
                // For std::vector<script_value> arrays, return reference to avoid copy
                else if constexpr (std::is_same_v<T, std::vector<script_value>>) {
                    return v.as_array();
                }
                // For script_map maps, return reference to avoid copy
                else if constexpr (std::is_same_v<T, script_map>) {
                    return v.as_map();
                }
                // For std::vector<T> containers, use conversion utility
                else if constexpr (is_specialization_v<T, std::vector>) {
                    auto* storage = detail::get_engine_parameter_storage(eng);
                    if (!storage) {
                        throw runtime_error("No parameter storage available for const reference conversion");
                    }
                    T& temp = storage->allocate<T>();
                    using element_type = typename T::value_type;
                    temp = conversions::convert_script_array_to_vector<element_type>(v, eng);
                    return temp;
                }
                // For std::map<K,V> containers, use conversion utility
                else if constexpr (is_script_map_like_v<T>) {
                    auto* storage = detail::get_engine_parameter_storage(eng);
                    if (!storage) {
                        throw runtime_error("No parameter storage available for const reference conversion");
                    }
                    T& temp = storage->allocate<T>();
                    using key_type = typename T::key_type;
                    using value_type = typename T::mapped_type;
                    temp = conversions::convert_script_map_to_stdmap<key_type, value_type>(v, eng);
                    return temp;
                }
                // A math value has no host object to alias: the decoded host value in parameter storage
                else if constexpr (math_adapted<T>) {
                    auto* storage = detail::get_engine_parameter_storage(eng);
                    if (!storage) {
                        throw runtime_error("No parameter storage available for const reference conversion");
                    }
                    T& temp = storage->allocate<T>();
                    temp = v.as<T>();
                    return temp;
                }
                // Smart-pointer params can't alias script storage (the holder owns T, not
                // shared_ptr<T>), and a callback is built from the script callable: a temp in parameter storage.
                else if constexpr (is_specialization_v<T, std::shared_ptr> || is_specialization_v<T, std::weak_ptr> ||
                                   is_specialization_v<T, std::function>) {
                    auto* storage = detail::get_engine_parameter_storage(eng);
                    if (!storage) {
                        throw runtime_error("No parameter storage available for const reference conversion");
                    }
                    T& temp = storage->allocate<T>();
                    if constexpr (is_specialization_v<T, std::function>) {
                        temp = value_converter<T>::from(v, eng);
                    } else {
                        temp = v.as<T>();
                    }
                    return temp;
                }
                // For custom classes, alias the live object (handles class_instance
                // wrappers, cpp_bound references, and raw holders via as<T&>)
                else if constexpr (std::is_class_v<T> &&
                                  !std::is_same_v<T, std::string> &&
                                  !is_specialization_v<T, std::vector> &&
                                  !is_script_map_like_v<T>) {
                    return const_cast<script_value&>(v).deref().as<T&>();
                } else {
                    // For basic types (int, float, bool, etc.), need to make a copy
                    auto* storage = detail::get_engine_parameter_storage(eng);
                    if (!storage) {
                        throw runtime_error("No parameter storage available for const reference conversion");
                    }
                    T& temp = storage->allocate<T>();
                    temp = v.as<T>();
                    return temp;
                }
            }
            
            static script_value to(const T& t, engine* eng) {
                // Smart pointers route to their own specialization — the registry path
                // would look up a class literally named "std::shared_ptr<T>" (the sim's
                // creature-signal args hit exactly this).
                if constexpr (is_specialization_v<T, std::shared_ptr>) {
                    return value_converter<T>::to(t, eng);
                }
                // Check if this is a custom class type (excluding standard containers and string)
                else if constexpr (std::is_class_v<T> &&
                             !std::is_same_v<T, std::string> &&
                             !is_specialization_v<T, std::vector> &&
                             !is_script_map_like_v<T>) {
                    // For user classes, use the conversion registry if available
                    // Implementation moved to engine_impl.hpp to avoid circular dependencies
                    if (!eng) {
                        throw runtime_error("Engine reference required for custom type conversion");
                    }
                    if constexpr (std::is_copy_constructible_v<T>) {
                        return convert_custom_type_with_registry<T>(t, eng);
                    } else {
                        throw runtime_error("Cannot convert non-copyable type by const reference. "
                                           "Use T& or shared_ptr<T> instead.");
                    }
                } else {
                    // For basic types and standard containers, use the script_value constructor with engine
                    if (!eng) {
                        throw runtime_error("Engine reference required for script_value creation");
                    }
                    return script_value(t, eng);
                }
            }
        };
        
        // Specialization for void
        template<>
        struct value_converter<void> {
            static script_value to(engine* eng) {
                if (!eng) {
                    throw runtime_error("Engine reference required for script_value creation");
                }
                // Create null/void value with engine reference
                return script_value(std::monostate{}, eng);
            }
        };
        
        // Specialization for bound_array<T> - zero-copy wrapper
        template<typename T>
        struct value_converter<bound_array<T>> {
            static bound_array<T> from(const script_value& v, engine*) {
                if (!v.is_array()) {
                    throw runtime_error("Cannot convert non-array to bound_array<T>");
                }
                // Create by value - will deep copy
                return bound_array<T>(v);
            }
            
            static script_value to(const bound_array<T>& arr, engine* eng) {
                // If the array owns its data, return the owned value
                if (arr.is_owned()) {
                    return arr.as_script_value();
                }
                // Otherwise, we need to create a new script_value with the array data
                // This will copy the array elements
                if (!eng) {
                    throw runtime_error("Engine reference required for script_value creation");
                }
                return script_value(arr.to_vector(), eng);
            }
        };
        
        // Note: bound_array<T>&, const bound_array<T>& are handled by create_argument() 
        // in the function call path, not through value_converter. They create stack-based
        // objects for zero-copy semantics without needing thread_local storage.
        
        // Specialization for bound_map<K,V> - zero-copy wrapper
        template<typename K, typename V>
        struct value_converter<bound_map<K, V>> {
            static bound_map<K, V> from(const script_value& v, engine*) {
                if (!v.is_map()) {
                    throw runtime_error("Cannot convert non-map to bound_map<K,V>");
                }
                // Create by value - will deep copy
                return bound_map<K, V>(v);
            }
            
            static script_value to(const bound_map<K, V>& map, engine*) {
                // If the map owns its data, return the owned value
                if (map.is_owned()) {
                    return map.as_script_value();
                }
                // Otherwise, we need to create a new script_value with the map data
                return script_value(map.to_map());
            }
        };
        
        // Note: bound_map<K,V>& and const bound_map<K,V>& are handled directly
        // in the function binder's call_with_bound_maps functions to ensure
        // proper stack-based lifetime management without thread_local storage.
        
        // Specialization for std::map<K,V> return values - converts to script map
        template<typename K, typename V>
        struct value_converter<std::map<K, V>> {
            static std::map<K, V> from(const script_value& v, engine* eng) {
                if (!v.is_map()) {
                    throw runtime_error("Cannot convert non-map to std::map<K,V>");
                }
                // Convert script map to std::map
                std::map<K, V> result;
                auto& local_map = v.as_map();
                for (const auto& [key, value] : local_map) {
                    K converted_key = value_converter<K>::from(key, eng);
                    V converted_value = value_converter<V>::from(value, eng);
                    result[converted_key] = converted_value;
                }
                return result;
            }
            
            static script_value to(const std::map<K, V>& stdmap, engine* eng) {
                // Use the engine-aware conversion function
                return conversions::convert_stdmap_to_script_map(stdmap, eng);
            }
        };
        
        // A non-const T& host parameter whose argument reaches the callee as a private copy: the callee's final value
        // is stored back through the script lvalue the argument names. A registered class aliases its live object,
        // a bound view aliases its container and a callback stays opaque, so none of those write back.
        template<typename P>
        consteval bool host_writes_back() {
            if constexpr (!std::is_lvalue_reference_v<P> || std::is_const_v<std::remove_reference_t<P>>) { return false; }
            else {
                using D = std::remove_cv_t<std::remove_reference_t<P>>;
                return std::is_arithmetic_v<D> || std::is_enum_v<D> || std::is_same_v<D, std::string> ||
                       detail::is_script_value_v<D> || is_specialization_v<D, std::vector> || is_script_map_like_v<D> ||
                       is_specialization_v<D, std::shared_ptr> || math_adapted<D>;
            }
        }

        template<typename... Ps>
        consteval uint64_t host_writeback_mask() {
            uint64_t mask = 0;
            [[maybe_unused]] uint64_t bit = 1;
            ((mask |= host_writes_back<Ps>() ? bit : 0, bit <<= 1), ...);
            return mask;
        }
        // Skip = the leading self parameter of a method lambda, which is no script argument
        template<typename Tuple, size_t Skip = 0> struct tuple_writeback_mask;
        template<typename... Ps> struct tuple_writeback_mask<std::tuple<Ps...>, 0> { static constexpr uint64_t value = host_writeback_mask<Ps...>(); };
        template<typename P0, typename... Ps> struct tuple_writeback_mask<std::tuple<P0, Ps...>, 1> { static constexpr uint64_t value = host_writeback_mask<Ps...>(); };
        template<> struct tuple_writeback_mask<std::tuple<>, 1> { static constexpr uint64_t value = 0; };

        // engine.cpp: the script lvalue a host T& argument names (target: the reference its store goes through, null when
        // the argument names none - a literal, a temporary, a call no script site made) and the value the callee starts from
        struct host_reference {
            script_value target;
            script_value value;
            host_slot slot;       // an unboxed frame-slot local: the store re-finds it (no target)
        };
        host_reference host_reference_argument(engine* eng, const std::vector<script_value>& args, size_t index, size_t offset);
        checked_result<void> host_reference_store(engine* eng, const host_slot& slot, script_value& target, script_value&& value);

        template<uint64_t Mask>
        using host_call_result_t = std::conditional_t<Mask != 0, checked_result<script_value>, script_value>;

        template<typename T>
        struct writeback_argument {
            T* live = nullptr;
            std::optional<T> owned;
            script_value target;
            host_slot slot;
            operator T&() { return live ? *live : *owned; }
        };

        template<typename T>
        T writeback_decode(const script_value& v, engine* eng) {
            if constexpr (detail::is_script_value_v<T>) { return v; }
            else { return value_converter<T>::from(v, eng); }
        }

        template<typename T>
        script_value writeback_encode(const T& t, engine* eng) {
            if constexpr (detail::is_script_value_v<T>) { return t; }
            else if constexpr (std::is_same_v<T, bool>) { return script_value(t, eng); }
            else if constexpr (std::is_same_v<T, char>) { return script_value(static_cast<script_char>(t), eng); }
            else if constexpr (std::is_enum_v<T> || std::is_integral_v<T>) { return script_value(static_cast<script_int>(t), eng); }
            else if constexpr (std::is_floating_point_v<T>) { return script_value(static_cast<script_float>(t), eng); }
            else { return value_converter<T>::to(t, eng); }
        }

        // An exact-width host variable (a cpp_bound argument) is the callee's object itself
        template<typename T>
        writeback_argument<T> make_writeback_argument(const std::vector<script_value>& args, size_t index, size_t offset, engine* eng) {
            writeback_argument<T> a{ nullptr, std::nullopt, script_value(std::monostate{}, eng), {} };
            script_value& actual = const_cast<script_value&>(args[index + offset].deref());
            if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, std::string>) {
                if (actual.is_cpp_bound()) {
                    a.live = &actual.template as<T&>();
                    return a;
                }
            }
            host_reference ref = host_reference_argument(eng, args, index, offset);
            a.target = std::move(ref.target);
            a.slot = ref.slot;
            a.owned.emplace(writeback_decode<T>(a.target.is_reference() || a.slot.frame ? ref.value : actual, eng));
            return a;
        }

        template<typename T>
        checked_result<void> commit_writeback(writeback_argument<T>& a, engine* eng) {
            if (a.live || (!a.target.is_reference() && !a.slot.frame)) { return {}; }
            return host_reference_store(eng, a.slot, a.target, writeback_encode<T>(*a.owned, eng));
        }
        template<typename T>
        checked_result<void> commit_writeback(T&, engine*) { return {}; }

        // In parameter order; the first failed store is the call's error
        template<typename Tuple>
        checked_result<void> commit_writebacks(Tuple& carriers, engine* eng) {
            checked_result<void> result;
            std::apply([&](auto&... carrier) {
                ((result ? void(result = commit_writeback(carrier, eng)) : void()), ...);
            }, carriers);
            return result;
        }

        // The callee's writes are stored back after it returns, and after it throws (a C++ callee's writes before a throw
        // stand); the result converts while the private copies it may name still live
        template<typename R, typename Tuple, typename Call>
        checked_result<script_value> invoke_with_writeback(Tuple& carriers, engine* eng, Call&& call) {
            script_value out(std::monostate{}, eng);
            try {
                if constexpr (std::is_void_v<R>) {
                    call();
                } else {
                    decltype(auto) result = call();
                    out = value_converter<R>::to(result, eng);
                }
            } catch (...) {
                (void)commit_writebacks(carriers, eng);
                throw;
            }
            if (auto stored = commit_writebacks(carriers, eng); !stored) { return stored.error_value(); }
            return out;
        }

        // Helper to extract function traits
        template<typename T>
        struct function_traits;
        
        // Function pointer
        template<typename R, typename... Args>
        struct function_traits<R(*)(Args...)> {
            using return_type = R;
            using argument_types = std::tuple<Args...>;
            static constexpr size_t arity = sizeof...(Args);
        };
        
        // member function pointer
        template<typename C, typename R, typename... Args>
        struct function_traits<R(C::*)(Args...)> {
            using return_type = R;
            using class_type = C;
            using argument_types = std::tuple<Args...>;
            static constexpr size_t arity = sizeof...(Args);
        };
        
        // Const member function pointer
        template<typename C, typename R, typename... Args>
        struct function_traits<R(C::*)(Args...) const> {
            using return_type = R;
            using class_type = C;
            using argument_types = std::tuple<Args...>;
            static constexpr size_t arity = sizeof...(Args);
        };
        
        // Function types (not pointers)
        template<typename R, typename... Args>
        struct function_traits<R(Args...)> {
            using return_type = R;
            using argument_types = std::tuple<Args...>;
            static constexpr size_t arity = sizeof...(Args);
        };
        
        // Functors and lambdas
        template<typename T>
        struct function_traits : function_traits<decltype(&T::operator())> {};
        
        // Reference types - strip reference and forward to base type
        template<typename T>
        struct function_traits<T&> : function_traits<T> {};
        
        template<typename T>
        struct function_traits<T&&> : function_traits<T> {};
        
        
        
    } // namespace detail
    
    namespace detail {
        // A bound object a parameter aliases: T&, const T&, T or shared_ptr<T> (by value or const&) of a class the boundary
        // hands by identity
        template<typename P>
        consteval bool host_object_param() {
            using D = std::remove_cvref_t<P>;
            if constexpr (is_specialization_v<D, std::shared_ptr>) {
                return std::is_class_v<typename D::element_type> && !(std::is_lvalue_reference_v<P> && !std::is_const_v<std::remove_reference_t<P>>);
            } else if constexpr (std::is_class_v<D>) {
                return !std::is_same_v<D, std::string> && !is_script_value_v<D> && !math_adapted<D> &&
                       !is_specialization_v<D, std::vector> && !is_script_map_like_v<D> && !is_specialization_v<D, std::unordered_map> &&
                       !is_specialization_v<D, std::set> &&
                       !is_specialization_v<D, std::unordered_set> && !is_specialization_v<D, std::function> &&
                       !is_specialization_v<D, std::weak_ptr> && !is_specialization_v<D, std::unique_ptr> &&
                       !is_specialization_v<D, bound_array> && !is_specialization_v<D, bound_map> &&
                       (std::is_reference_v<P> || std::is_copy_constructible_v<D>);
            } else {
                return false;
            }
        }

        // The one rule the direct lanes share (a free or static function's by-value call, a bound method's native lane): every
        // parameter but a non-const reference to anything other than a bound object, which the ordinary call writes back or views
        template<typename P>
        consteval bool host_lane_param() {
            return !(std::is_reference_v<P> && !std::is_const_v<std::remove_reference_t<P>>) || host_object_param<P>();
        }

        template<typename Tuple, size_t... Is>
        consteval bool host_lane_params(std::index_sequence<Is...>) {
            return (host_lane_param<std::tuple_element_t<Is, Tuple>>() && ...);
        }
        template<typename Tuple>
        consteval bool host_lane_params() {
            return host_lane_params<Tuple>(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
        }

        // A direct-lane argument behind a reference parameter takes the ordinary call's conversion: a bound object, a string, a
        // script_value, a script array or a script map is the script's own, borrowed in place, a bound_array or bound_map views
        // it; anything else is the converted value, a temporary the call's full expression keeps
        template<typename P>
        decltype(auto) lane_argument(const script_value& arg, engine* eng) {
            using D = std::remove_cvref_t<P>;
            if constexpr (!std::is_reference_v<P>) {
                return value_converter<P>::from(arg, eng);
            } else {
                script_value& actual = const_cast<script_value&>(arg.deref());
                if constexpr (is_specialization_v<D, std::shared_ptr> || is_specialization_v<D, std::weak_ptr> || math_adapted<D>) {
                    return actual.as<D>();
                } else if constexpr (is_specialization_v<D, std::function>) {
                    return value_converter<D>::from(actual, eng);
                } else if constexpr (host_object_param<P>()) {
                    return static_cast<std::remove_reference_t<P>&>(actual.as<D&>());
                } else if constexpr (std::is_same_v<D, script_value>) {
                    return static_cast<const script_value&>(actual);
                } else if constexpr (is_specialization_v<D, bound_array>) {
                    if (!arg.is_array()) { throw runtime_error("Cannot convert non-array to bound_array<T>&"); }
                    return D(const_cast<script_value&>(arg), eng);
                } else if constexpr (is_specialization_v<D, bound_map>) {
                    if (!arg.is_map()) { throw runtime_error("Cannot convert non-map to bound_map<K,V>&"); }
                    return D(const_cast<script_value&>(arg).as_map(), eng);
                } else if constexpr (std::is_same_v<D, std::string> || std::is_same_v<D, std::vector<script_value>> || std::is_same_v<D, script_map>) {
                    return static_cast<const D&>(actual.as<const D&>());
                } else if constexpr (std::is_same_v<D, char>) {
                    return value_converter<char>::from(actual, eng);
                } else {
                    return D(actual.as<P>());
                }
            }
        }
    }

    // Main function binder
    template<typename Func>
    class function_binder {
    public:
        explicit function_binder(Func f, engine* eng) : func_(std::move(f)), engine_(eng) {}
        
        script_function bind() {
            using traits = detail::function_traits<Func>;
            using args_tuple = typename traits::argument_types;
            
            // IMPORTANT: capture func_ by value and engine pointer, not this!
            // The function_binder is temporary and will be destroyed after add_function returns
            
            if constexpr (!detail::host_lane_params<args_tuple>()) {
                // Use reference-aware calling for functions with reference parameters
                return [func = func_, eng = engine_](const std::vector<script_value>& args) -> checked_result<script_value> {
                    using traits = detail::function_traits<Func>;
                    using return_type = typename traits::return_type;
                    using args_tuple = typename traits::argument_types;

                    // Check argument count
                    if (args.size() != traits::arity) {
                        return checked_result<script_value>(
                            make_error_code(runtime_error_code::argument_count_mismatch),
                            traits::arity == 1 ? "Function expects {0} argument, got {1}" : "Function expects {0} arguments, got {1}",
                            static_cast<uint64_t>(traits::arity), static_cast<uint64_t>(args.size())
                        );
                    }

                    // Call with reference support, catching any C++ exceptions
                    try {
                        return function_binder::call_with_reference_support<return_type, args_tuple>(func, args, std::make_index_sequence<traits::arity>{}, eng);
                    } catch (const std::exception& e) {
                        return host_exception_error(eng, detail::native_exception_text(e));
                    }
                };
            } else {
                return by_value_call{func_, engine_};
            }
        }

        // The binding of a function every parameter of which takes the direct lane; invoke is the same call without the std::function
        struct by_value_call {
            Func func;
            engine* eng;
            checked_result<script_value> operator()(const std::vector<script_value>& args) const { return call(args.data(), args.size()); }
            checked_result<script_value> call(const script_value* args, size_t argc) const {
                using traits = detail::function_traits<Func>;
                using return_type = typename traits::return_type;
                using args_tuple = typename traits::argument_types;

                if (argc != traits::arity) {
                    return checked_result<script_value>(
                        make_error_code(runtime_error_code::argument_count_mismatch),
                        traits::arity == 1 ? "Function expects {0} argument, got {1}" : "Function expects {0} arguments, got {1}",
                        static_cast<uint64_t>(traits::arity), static_cast<uint64_t>(argc)
                    );
                }

                try {
                    return function_binder::call_impl_static<return_type, args_tuple>(func, args, std::make_index_sequence<traits::arity>{}, eng);
                } catch (const std::exception& e) {
                    return host_exception_error(eng, detail::native_exception_text(e));
                }
            }
            static checked_result<script_value> invoke(const void* self, const script_value* args, size_t argc) {
                return static_cast<const by_value_call*>(self)->call(args, argc);
            }
            static const void* locate(const script_function& f) { return f.template target<by_value_call>(); }
        };

    private:
        Func func_;
        engine* engine_;
        
        template<typename R, typename ArgsTuple, typename F, size_t... Is>
        static script_value call_impl_static(F&& func, const script_value* args, std::index_sequence<Is...>, engine* eng) {
            if constexpr (std::is_void_v<R>) {
                // Void function
                call_void_impl_static<ArgsTuple>(std::forward<F>(func), args, std::index_sequence<Is...>{}, eng);
                return script_value(std::monostate{}, eng); // Return null for void
            } else {
                // Non-void function - use decltype(auto) to preserve reference types
                decltype(auto) result = call_non_void_impl_static<R, ArgsTuple>(std::forward<F>(func), args, std::index_sequence<Is...>{}, eng);
                return detail::value_converter<R>::to(result, eng);
            }
        }

        // No parameter_storage scope_guard: a lane argument is a by-value conversion or a borrow, never the engine's parameter
        // storage, so these bindings (the whole math stdlib) run inside parallel regions without touching shared engine state
        template<typename ArgsTuple, typename F, size_t... Is>
        static void call_void_impl_static(F&& func, const script_value* args, std::index_sequence<Is...>, [[maybe_unused]] engine* eng) {
            func(detail::lane_argument<std::tuple_element_t<Is, ArgsTuple>>(args[Is], eng)...);
        }

        template<typename R, typename ArgsTuple, typename F, size_t... Is>
        static R call_non_void_impl_static(F&& func, const script_value* args, std::index_sequence<Is...>, [[maybe_unused]] engine* eng) {
            return func(detail::lane_argument<std::tuple_element_t<Is, ArgsTuple>>(args[Is], eng)...);
        }
        
        // Reference-aware calling methods
        template<typename R, typename ArgsTuple, typename F, size_t... Is>
        static checked_result<script_value> call_with_reference_support(F&& func, const std::vector<script_value>& args, std::index_sequence<Is...>, engine* eng) {
            if constexpr (detail::tuple_writeback_mask<ArgsTuple>::value != 0) {
                auto arg_tuple = std::make_tuple(create_argument<std::tuple_element_t<Is, ArgsTuple>, Is>(args, eng)...);
                return detail::invoke_with_writeback<R>(arg_tuple, eng, [&]() -> decltype(auto) { return std::apply(func, arg_tuple); });
            } else if constexpr (std::is_void_v<R>) {
                // Void function with reference support
                call_with_reference_support_void<ArgsTuple>(std::forward<F>(func), args, std::index_sequence<Is...>{}, eng);
                return script_value(std::monostate{}, eng); // Return null for void
            } else {
                // Non-void function with reference support - use decltype(auto) to preserve reference types
                decltype(auto) result = call_with_reference_support_non_void<R, ArgsTuple>(std::forward<F>(func), args, std::index_sequence<Is...>{}, eng);
                return detail::value_converter<R>::to(result, eng);
            }
        }

        template<typename ArgsTuple, typename F, size_t... Is>
        static void call_with_reference_support_void(F&& func, const std::vector<script_value>& args, std::index_sequence<Is...>, engine* eng) {
            // Create bound_map objects on the stack for zero-copy semantics
            // This approach supports recursion naturally
            call_with_bound_maps_void<ArgsTuple, F, Is...>(std::forward<F>(func), args, eng);
        }
        
        // Helper to create bound_map objects on stack and call void function
        template<typename ArgsTuple, typename F, size_t... Is>
        static void call_with_bound_maps_void(F&& func, const std::vector<script_value>& args, engine* eng) {
            // Create tuple of arguments, handling bound_map types specially
            auto arg_tuple = std::make_tuple(create_argument<std::tuple_element_t<Is, ArgsTuple>, Is>(args, eng)...);
            
            // Call function with unpacked arguments
            std::apply(std::forward<F>(func), arg_tuple);
        }
        
        template<typename R, typename ArgsTuple, typename F, size_t... Is>
        static R call_with_reference_support_non_void(F&& func, const std::vector<script_value>& args, std::index_sequence<Is...>, engine* eng) {
            // Create bound_map objects on the stack for zero-copy semantics
            // This approach supports recursion naturally
            return call_with_bound_maps<R, ArgsTuple, F, Is...>(std::forward<F>(func), args, eng);
        }
        
        // Helper trait to detect bound_array types
        template<typename T>
        struct is_bound_array_ref : std::false_type {};
        
        template<typename T>
        struct is_bound_array_ref<bound_array<T>&> : std::true_type {
            using value_type = T;
        };
        
        template<typename T>
        struct is_bound_array_ref<const bound_array<T>&> : std::true_type {
            using value_type = T;
        };
        
        // Helper to check if a type is bound_array (not a reference)
        template<typename T>
        struct is_bound_array_type : std::false_type {};
        
        template<typename T>
        struct is_bound_array_type<bound_array<T>> : std::true_type {};
        
        // Helper to check if a type is bound_map (not a reference)
        template<typename T>
        struct is_bound_map_type : std::false_type {};
        
        template<typename K, typename V>
        struct is_bound_map_type<bound_map<K, V>> : std::true_type {};
        
        // Helper trait to detect bound_map types
        template<typename T>
        struct is_bound_map_ref : std::false_type {};
        
        template<typename K, typename V>
        struct is_bound_map_ref<bound_map<K, V>&> : std::true_type {
            using key_type = K;
            using value_type = V;
        };
        
        template<typename K, typename V>
        struct is_bound_map_ref<const bound_map<K, V>&> : std::true_type {
            using key_type = K;
            using value_type = V;
        };
        
        // Helper to create bound_map objects on stack and call function
        template<typename R, typename ArgsTuple, typename F, size_t... Is>
        static R call_with_bound_maps(F&& func, const std::vector<script_value>& args, engine* eng) {
            // Create tuple of arguments, handling bound_map types specially
            auto arg_tuple = std::make_tuple(create_argument<std::tuple_element_t<Is, ArgsTuple>, Is>(args, eng)...);
            
            // Call function with unpacked arguments
            return std::apply(std::forward<F>(func), arg_tuple);
        }
        
        // Create individual arguments, handling bound_array and bound_map types specially
        template<typename T, size_t I>
        static auto create_argument(const std::vector<script_value>& args, engine* eng) {
            const script_value& arg = args[I];
            if constexpr (detail::host_writes_back<T>()) {
                return detail::make_writeback_argument<std::remove_cvref_t<T>>(args, I, 0, eng);
            } else if constexpr (is_bound_array_ref<T>::value) {
                using V = typename is_bound_array_ref<T>::value_type;
                
                if (!arg.is_array()) {
                    throw runtime_error("Cannot convert non-array to bound_array<T>&");
                }
                // Create bound_array object on stack - zero-copy view of the script array's
                // node (typed buffers included; the element type must fit the node's kind)
                return bound_array<V>(const_cast<script_value&>(arg), eng);
            } else if constexpr (is_bound_map_ref<T>::value) {
                using K = typename is_bound_map_ref<T>::key_type;
                using V = typename is_bound_map_ref<T>::value_type;
                
                if (!arg.is_map()) {
                    throw runtime_error("Cannot convert non-map to bound_map<K,V>&");
                }
                // Create bound_map object on stack - zero-copy reference to script_map
                return bound_map<K, V>(const_cast<script_value&>(arg).as_map(), eng);
            } else if constexpr (std::is_reference_v<T>) {
                // For reference parameters, work with the dereferenced value
                using base_type = std::remove_cv_t<std::remove_reference_t<T>>;
                
                // Get the actual value (deref handles references automatically)
                const script_value& actual_arg = arg.deref();
                
                // Smart-pointer params materialize the handle by value (holders own T, not
                // shared_ptr<T> — wrapping again would reinterpret T's bytes as a handle);
                // the tuple element is an lvalue, so const shared_ptr<T>& binds to it.
                if constexpr (is_specialization_v<base_type, std::shared_ptr> || is_specialization_v<base_type, std::weak_ptr>) {
                    return actual_arg.as<base_type>();
                }
                // A callback is built from the script callable
                else if constexpr (is_specialization_v<base_type, std::function>) {
                    return detail::value_converter<base_type>::from(actual_arg, eng);
                }
                // A math value has no host object to alias: the decoded host value
                else if constexpr (math_adapted<base_type>) {
                    return actual_arg.as<base_type>();
                }
                // For custom classes, get shared_ptr and return reference to the object
                // Exclude string, script_value, bound_array, bound_map, and built-in container types which have special handling
                else if constexpr (std::is_class_v<base_type> &&
                            !std::is_same_v<base_type, std::string> &&
                            !detail::is_script_value_v<base_type> &&
                            !is_bound_array_type<base_type>::value &&
                            !is_bound_map_type<base_type>::value &&
                            !is_specialization_v<base_type, std::vector> &&
                            !is_script_map_like_v<base_type>) {
					// Reference parameters borrow the live object. Ownership is only required
					// for shared_ptr parameters; their conversion is handled above.
					return std::ref(const_cast<script_value&>(actual_arg).as<base_type&>());
                } else if constexpr (detail::is_script_value_v<base_type>) {
                    // For script_value references, return directly - no conversion needed
                    return actual_arg;
                } else if constexpr (std::is_same_v<base_type, std::string>) {
                    return std::cref(const_cast<script_value&>(actual_arg).as<const std::string&>());   // the script's own string
                } else if constexpr (std::is_same_v<base_type, char>) {
                    return detail::value_converter<char>::from(actual_arg, eng);
                } else {
                    // For built-in types and special types, use the non-const as<T&>() method
                    return const_cast<script_value&>(actual_arg).as<T>();
                }
            } else {
                // For value parameters, use normal conversion which creates a copy
                return detail::value_converter<T>::from(arg, eng);
            }
        }
        
    };
    
    namespace detail {
        template<typename Func>
        concept engine_first_variadic = std::is_invocable_v<const std::decay_t<Func>&, engine&, std::span<const script_value>>;

        // A variadic callable's arguments by value: an element reference an argument site minted (`f(a[i])`) reaches it as the
        // element's value
        checked_result<script_value> call_variadic_by_value(const std::vector<script_value>& args,
                                                            checked_result<script_value> (*invoke)(const void*, const script_value*, size_t), const void* self);

        // An engine-first variadic binding; invoke is the same call on the arguments where they are, the VM's direct entry
        template<typename Func>
        struct engine_variadic_call {
            using result_type = std::invoke_result_t<const Func&, engine&, std::span<const script_value>>;
            Func func;
            engine* eng;
            checked_result<script_value> operator()(const std::vector<script_value>& args) const { return call_variadic_by_value(args, &invoke, this); }
            checked_result<script_value> call(const script_value* args, size_t argc) const {
                using D = std::remove_cvref_t<result_type>;
                const std::span<const script_value> arguments{args, argc};
                if constexpr (std::is_void_v<result_type>) {
                    func(*eng, arguments);
                    return script_value(std::monostate{}, eng);
                } else if constexpr (std::is_same_v<D, script_value> || std::is_same_v<D, checked_result<script_value>>) {
                    return func(*eng, arguments);
                } else {
                    decltype(auto) result = func(*eng, arguments);
                    return value_converter<result_type>::to(result, eng);
                }
            }
            static checked_result<script_value> invoke(const void* self, const script_value* args, size_t argc) {
                return static_cast<const engine_variadic_call*>(self)->call(args, argc);
            }
            static const void* locate(const script_function& f) { return f.template target<engine_variadic_call>(); }
        };
    }

    // Helper function to create a binder
    // The binding owns a copy of the callable: an lvalue argument is copied, never referenced
    template<typename Func>
    function_binder<std::decay_t<Func>> make_function_binder(Func&& f, engine* eng) {
        return function_binder<std::decay_t<Func>>(std::forward<Func>(f), eng);
    }
    
} // namespace jai

#endif // __JAISCRIPT_CORE_FUNCTION_BINDER_HPP__
