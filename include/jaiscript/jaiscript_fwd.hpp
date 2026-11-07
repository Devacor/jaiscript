#pragma once
#include <type_traits>

#ifndef __JAISCRIPT_JAISCRIPT_FWD_HPP__
#define __JAISCRIPT_JAISCRIPT_FWD_HPP__

// Forced inlining for hot-path leaf functions the compiler's inline budget abandons in
// the giant interpreter/vm TUs (each use is WPR-justified — cite the row). GCC/Clang's
// always_inline is the same hard demand as MSVC's __forceinline; plain `inline` is the
// portable floor for anything else.
#if defined(_MSC_VER)
#define JAI_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define JAI_FORCEINLINE inline __attribute__((always_inline))
#else
#define JAI_FORCEINLINE inline
#endif
#if defined(_MSC_VER)
#define JAI_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define JAI_NOINLINE __attribute__((noinline))
#else
#define JAI_NOINLINE
#endif

// Inline header code holds locks and guards across throws; without unwinding their destructors never run
#if defined(_MSC_VER) && !defined(_CPPUNWIND)
#error "JaiScript requires C++ exception unwinding (/EHsc)"
#endif

namespace jai {
    
    class engine;
    class script_value;
    class held_value;
    namespace detail {
        // a binding treats a held value (an execute result the host passes back in) as the script_value it is
        template <typename T> inline constexpr bool is_script_value_v = std::is_same_v<std::remove_cvref_t<T>, script_value> || std::is_same_v<std::remove_cvref_t<T>, held_value>;
        struct engine_access;
        // engine_access from a template written before the engine is complete: naming the engine type E defers the lookup to
        // the instantiation
        template <typename E, typename Access = engine_access> struct engine_access_of : Access {};
    }
    class type_info;
    class wrapped_function;
    class class_definition;
    class class_instance;
    template<typename T> class dynamic_binder;
    class variable_scope;
    class parse_error;
    class runtime_error;
    class serialization_error;
    
    struct source_location;
    
    enum class script_value_type {
        jai_null_type,
        jai_any_type,            // Dynamic type (var keyword) - any value allowed
        jai_int_type,
        jai_float_type,
        jai_string_type,
        jai_char_type,
        jai_bool_type,
        jai_array_type,          // Array<T>
        jai_map_type,            // Map<K,V>
        jai_object_type,         // Object<T> - typed object
        jai_function_type,       // Function<ReturnType(Args...)>
        jai_reference_type,      // T& - Reference to another value
        jai_weak_ptr_type,       // weak_ptr<T>
        jai_shared_ptr_type,     // shared_ptr<T>
        jai_cpp_bound_type,      // C++ bound value
        jai_invalid_type,        // Invalid/non-existent value (used as sentinel)
        jai_utf8_string_type,    // utf8_string: the same bytes as a string, character semantics
        jai_vec2_type,           // math::vec2 (2 x f32)
        jai_vec3_type,           // math::vec3 (3 x f32)
        jai_ivec2_type,          // math::ivec2 (2 x i32)
        jai_ivec3_type,          // math::ivec3 (3 x i32)
        jai_vec4_type,           // math::vec4 (4 x f32)
        jai_color_type           // math::color (r, g, b, a f32, straight alpha)
    };
    inline constexpr script_value_type k_last_script_value_type = script_value_type::jai_color_type;   // the bound a .jaibite's type numbers are read against: move it with a new last member
    
    // The unit whose code is running and the execution that compiled it:
    // id numbers units by first execution (0 = the host), seq numbers executions engine-wide
    struct unit_stamp {
        unsigned int id = 0;
        unsigned int seq = 0;
    };

    // The vm's cell block counters (engine::cells_counters): block entries, boundary operations run, parked blocks resumed, misses.
    // Counted only on those paths, never per instruction; the interpreter reports zeros
    struct cell_counters {
        unsigned long long entries = 0, boundaries = 0, resumes = 0, misses = 0, row_entries = 0;   // row_entries: entries whose sites and bindings a loop's block-entry row answered
    };

} // namespace jai

#endif // __JAISCRIPT_JAISCRIPT_FWD_HPP__