#pragma once

#ifndef __JAISCRIPT_CORE_TYPE_INFO_HPP__
#define __JAISCRIPT_CORE_TYPE_INFO_HPP__

#include "types.hpp"
#include <jaiscript/jaiscript_fwd.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/math/adapter.hpp>
#include <vector>
#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <string_view>

namespace jai {

    // The script spelling of a math kind (the type name, type_of, diagnostics); null for any other type
    constexpr const char* math_type_name(script_value_type kind) noexcept {
        switch (kind) {
            case script_value_type::jai_vec2_type: return "math::vec2";
            case script_value_type::jai_vec3_type: return "math::vec3";
            case script_value_type::jai_vec4_type: return "math::vec4";
            case script_value_type::jai_color_type: return "math::color";
            case script_value_type::jai_ivec2_type: return "math::ivec2";
            case script_value_type::jai_ivec3_type: return "math::ivec3";
            default: return nullptr;
        }
    }
    constexpr bool is_math_type(script_value_type kind) noexcept { return math_type_name(kind) != nullptr; }

    // A base type's script spelling in diagnostics
    inline constexpr std::string_view base_type_spelling(script_value_type type) {
        switch (type) {
            case script_value_type::jai_null_type: return "null";
            case script_value_type::jai_int_type: return "int";
            case script_value_type::jai_float_type: return "float";
            case script_value_type::jai_string_type: return "string";
            case script_value_type::jai_utf8_string_type: return "utf8_string";
            case script_value_type::jai_char_type: return "char";
            case script_value_type::jai_bool_type: return "bool";
            case script_value_type::jai_array_type: return "array";
            case script_value_type::jai_map_type: return "map";
            case script_value_type::jai_object_type: return "object";
            case script_value_type::jai_function_type: return "function";
            case script_value_type::jai_reference_type: return "reference";
            case script_value_type::jai_shared_ptr_type: return "shared_ptr";
            case script_value_type::jai_weak_ptr_type: return "weak_ptr";
            case script_value_type::jai_any_type: return "any";
            case script_value_type::jai_invalid_type: return "invalid";
            default: return math_type_name(type) ? math_type_name(type) : "unknown";
        }
    }
    // The math kind a fused type token names ("math::vec3", lexer.cpp); jai_null_type for any other spelling
    constexpr script_value_type math_type_from_name(std::string_view name) noexcept {
        if (name == "math::vec2") return script_value_type::jai_vec2_type;
        if (name == "math::vec3") return script_value_type::jai_vec3_type;
        if (name == "math::vec4") return script_value_type::jai_vec4_type;
        if (name == "math::color") return script_value_type::jai_color_type;
        if (name == "math::ivec2") return script_value_type::jai_ivec2_type;
        if (name == "math::ivec3") return script_value_type::jai_ivec3_type;
        return script_value_type::jai_null_type;
    }

    // A template instance's registered name: the base, then each argument's canonical spelling (type_info::canonical_name
    // for a script type, type_name_helper for a C++ one), so the parser's Box<array<int>> and a host's
    // Box<std::vector<int64_t>> name one class
    inline std::string template_instance_name(std::string_view base, const std::vector<std::string>& arguments) {
        std::string name(base);
        name += '<';
        for (size_t i = 0; i < arguments.size(); ++i) {
            if (i) { name += ", "; }
            name += arguments[i];
        }
        name += '>';
        return name;
    }

    // Forward declarations
    class type_info;

    // Wrapper for type_info* to enable operator overloading and prevent pointer comparison bugs
    struct type_info_ptr {
        type_info* ptr = nullptr;

        type_info_ptr() = default;
        type_info_ptr(type_info* p) : ptr(p) {}
        type_info_ptr(std::nullptr_t) : ptr(nullptr) {}

        type_info* operator->() const { return ptr; }
        type_info& operator*() const { return *ptr; }
        operator type_info*() const { return ptr; }
        explicit operator bool() const { return ptr != nullptr; }

        // Get the raw pointer
        type_info* get() const { return ptr; }
    };

    // Represents complete type information including generic parameters
    class engine;

    // a value's tag word holds a type_info pointer above its index byte and engine-made flag (value.hpp)
    inline constexpr size_t k_value_tag_alignment = 512;

    class alignas(k_value_tag_alignment) type_info {
    public:
        script_value_type base_type;
        // An enum's types, set when its declaration runs (engine::note_enum): the interned object spelling of its name, which a
        // slot declares, and the int type its values are stamped with (a stamp of the first lands as the second: set_type_info)
        bool is_enum = false;
        std::string type_name;  // For Object<T>, this would be the class name
        uint64_t id = 0;        // Interned canonical name for O(1) type comparison

        // Generic type parameters
        std::vector<type_info_ptr> type_params;

        // Native type information for serialization
        size_t native_size = 0;     // Size in bytes of the native C++ type
        bool is_signed = true;      // For integral types

        // shared_ptr held by a var-DECLARED variable: '=' with a
        // handle rhs REBINDS unchecked across classes. Same name/id/element as the
        // plain spelling (every other read behaves identically); interned as a twin
        // via engine::get_type_info_shared_ptr_dynamic. Excluded from equals().
        bool dynamic_pointee = false;

        // A typed container held by a var slot: the container keeps its element/key/value types (every element
        // boundary reads it), the slot stays dynamic ('=' takes any value). Same name/id/element as the plain spelling;
        // interned as a twin via engine::get_type_info_array_dynamic/get_type_info_map_dynamic. Excluded from equals().
        bool dynamic_slot = false;

        // A reference whose DECLARED type is T (`int& r = v`, cast<int&>(v)): the T& twin a store through
        // it converts by T's table before the storage's own rules. Same name/id/element as the plain T&; interned via
        // engine::get_type_info_declared_reference. Excluded from equals().
        bool declared_reference = false;
        type_info* declared_referent = nullptr;   // on the declared twin: T, one load from the reference value's stamp
        // on the declared twin: the storage index a value of T's kind has (int, float, bool, string), or 0xFF when every store
        // runs T's table; 0 on every other type_info (a store through it decides with one compare)
        uint8_t declared_kind = 0;

        // Engine-interned "T&" twin cache (engine::get_type_info_reference fast path -
        // elides the intern-map lookup on every reference mint). Only meaningful on the
        // INTERNED instance; pre-intern copies carry a discarded value. Excluded from
        // equals(). Written only outside parallel regions (workers read prewarmed values).
        type_info* cached_reference_type = nullptr;
        type_info* cached_declared_reference_type = nullptr;   // the declared_reference twin, the same way

        // enum_value_type on both of an enum's types; enum_numbers in declaration order. Excluded from equals().
        type_info* enum_value_type = nullptr;
        std::vector<int64_t> enum_numbers;
        bool enum_holds(int64_t number) const {
            for (int64_t held : enum_numbers) { if (held == number) { return true; } }
            return false;
        }

        // Constructors
        type_info(script_value_type type) : base_type(type) {}
        type_info(script_value_type type, std::string_view name) : base_type(type), type_name(name) {}

        // Type checking
        bool is_array() const { return base_type == script_value_type::jai_array_type; }
        bool is_map() const { return base_type == script_value_type::jai_map_type; }
        bool is_object() const { return base_type == script_value_type::jai_object_type; }
        bool is_function() const { return base_type == script_value_type::jai_function_type; }
        bool is_reference() const { return base_type == script_value_type::jai_reference_type; }
        bool is_weak_ptr() const { return base_type == script_value_type::jai_weak_ptr_type; }
        // a value of this type can own an instance (its end observable through a destructor, a weak_ptr or a use count): anything
        // but a scalar, a string, a math vector, a weak_ptr or a reference
        bool may_own_instance() const {
            switch (base_type) {
            case script_value_type::jai_int_type: case script_value_type::jai_float_type: case script_value_type::jai_bool_type:
            case script_value_type::jai_char_type: case script_value_type::jai_string_type: case script_value_type::jai_utf8_string_type:
            case script_value_type::jai_reference_type: case script_value_type::jai_weak_ptr_type:
            case script_value_type::jai_vec2_type: case script_value_type::jai_vec3_type: case script_value_type::jai_ivec2_type:
            case script_value_type::jai_ivec3_type: case script_value_type::jai_vec4_type: case script_value_type::jai_color_type:
                return false;
            default:
                return true;
            }
        }
        // uint8 / int8: the one-byte element types of array<uint8> / array<int8> (a host's uint8_t stays a plain int)
        bool is_byte_element() const {
            return base_type == script_value_type::jai_int_type && native_size == 1 && (type_name == "uint8" || type_name == "int8");
        }

        // Get type parameters
        type_info_ptr element_type() const {  // For Array<T>, weak_ptr<T>, etc.
            return type_params.empty() ? nullptr : type_params[0];
        }

        type_info_ptr key_type() const {  // For Map<K,V>
            return type_params.empty() ? nullptr : type_params[0];
        }

        type_info_ptr value_type() const {  // For Map<K,V>
            return type_params.size() < 2 ? nullptr : type_params[1];
        }

        type_info_ptr return_type() const {  // For Function
            return type_params.empty() ? nullptr : type_params[0];
        }

        std::vector<type_info_ptr> arg_types() const {  // For Function
            if (type_params.size() <= 1) return {};

            return std::vector<type_info_ptr>(type_params.begin() + 1, type_params.end());
        }

        // script_string representation for debugging
        std::string to_string() const;

        // Canonical name for type interning (unique identifier for this type structure)
        // Examples: "int", "array<int>", "array<>", "map<string,int>", "map<,>"
        std::string canonical_name() const;

        // The script spelling a reader sees (reflection, reports): canonical_name with an untyped container spelled bare
        // ("array", "map") and a dynamic element as var ("map<string,var>"); "var" for any
        std::string spelling() const;

        // Equality comparison
        bool equals(const type_info& other) const;

    private:
        friend class engine;  // Engine is responsible for creating and interning type_info objects
        friend class parser;  // Parser needs to create type_info during parsing
        friend class class_definition;  // Class definitions need to create type_info for their types

        // Factory methods for common types - private, only accessible by engine/parser/class_definition
        // These create type_info objects with interned canonical names
        static type_info make_int(string_symbolizer& symbolizer);

        static type_info make_float(string_symbolizer& symbolizer);

        static type_info make_string(string_symbolizer& symbolizer);

        static type_info make_utf8_string(string_symbolizer& symbolizer);

        static type_info make_math(string_symbolizer& symbolizer, script_value_type kind);

        static type_info make_bool(string_symbolizer& symbolizer);

        static type_info make_char(string_symbolizer& symbolizer);

        static type_info make_void(string_symbolizer& symbolizer);

        // any type (var keyword) - dynamic typing, any value allowed
        static type_info make_any(string_symbolizer& symbolizer);

        static type_info make_invalid(string_symbolizer& symbolizer);

        // Factory methods for specific native types
        static type_info make_int8(string_symbolizer& symbolizer);

        static type_info make_uint8(string_symbolizer& symbolizer);

        // Array<T>
        static type_info make_array(string_symbolizer& symbolizer, type_info_ptr element_type);

        // Map<K,V>
        static type_info make_map(string_symbolizer& symbolizer, type_info_ptr keyType, type_info_ptr valueType);

        // Object<T>
        static type_info make_object(string_symbolizer& symbolizer, std::string_view className);

        // weak_ptr<T>
        static type_info make_weak_ptr(string_symbolizer& symbolizer, type_info_ptr pointee_type);

        // shared_ptr<T>
        static type_info make_shared_ptr(string_symbolizer& symbolizer, type_info_ptr pointee_type);

        // T&
        static type_info make_reference(string_symbolizer& symbolizer, type_info_ptr referenced_type);

        // C++ bound value
        static type_info make_cpp_bound(string_symbolizer& symbolizer, type_info_ptr bound_type);

        // Function<ReturnType(Args...)>
        // The `function` spelling (a slot taking any callable or null, a std::function's rule)
        static type_info make_callable(string_symbolizer& symbolizer);

        static type_info make_function(string_symbolizer& symbolizer, type_info_ptr return_type, std::vector<type_info_ptr> arg_types);

        // An integral or floating C++ type of that size, as make<T> reaches it
        static type_info make_native_int(string_symbolizer& symbolizer, size_t native_size, bool is_signed);
        static type_info make_native_float(string_symbolizer& symbolizer, size_t native_size);

        // Template factory method for C++ types
        template<typename T>
        static type_info make(string_symbolizer& symbolizer) {
            if constexpr (std::is_same_v<T, script_int>) {
                return make_int(symbolizer);
            } else if constexpr (std::is_same_v<T, script_float>) {
                return make_float(symbolizer);
            } else if constexpr (std::is_same_v<T, script_string>) {
                return make_string(symbolizer);
            } else if constexpr (std::is_same_v<T, script_bool>) {
                return make_bool(symbolizer);
            } else if constexpr (std::is_same_v<T, script_char>) {
                return make_char(symbolizer);
            } else if constexpr (std::is_same_v<T, void>) {
                return make_void(symbolizer);
            } else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
                // All integral types (int, long, etc.) map to script_int
                return make_native_int(symbolizer, sizeof(T), std::is_signed_v<T>);
            } else if constexpr (std::is_floating_point_v<T>) {
                // All floating point types (float, double) map to script_float
                return make_native_float(symbolizer, sizeof(T));
            } else if constexpr (math_adapted<T>) {
                return make_math(symbolizer, math_adapter<std::remove_cv_t<T>>::kind);
            } else {
                // For custom types, create an object type with the type name
                return make_object(symbolizer, typeid(T).name());
            }
        }
    };

    // Equality operators for type_info_ptr - compares content, not addresses
    inline bool operator==(type_info_ptr lhs, type_info_ptr rhs) {
        if (lhs.ptr == rhs.ptr) return true;
        if (!lhs.ptr || !rhs.ptr) return false;
        return lhs.ptr->equals(*rhs.ptr);
    }

    inline bool operator!=(type_info_ptr lhs, type_info_ptr rhs) {
        return !(lhs == rhs);
    }

    // Example usage in JaiScript syntax:
    // int x = 5;                           // type_info::make_int()
    // array<int> nums = {1, 2, 3};         // type_info::make_array(make_int())
    // map<string, int> scores;             // type_info::make_map(make_string(), make_int())
    // weak_ptr<Creature> creature;         // type_info::make_weak_ptr(make_object("Creature"))
    // Function<int(float, float)> add;     // type_info::make_function(make_int(), {make_float(), make_float()})
    
} // namespace jai

#endif // __JAISCRIPT_CORE_TYPE_INFO_HPP__