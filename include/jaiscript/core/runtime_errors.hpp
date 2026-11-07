#pragma once

#ifndef __JAISCRIPT_CORE_RUNTIME_ERRORS_HPP__
#define __JAISCRIPT_CORE_RUNTIME_ERRORS_HPP__

#include <new>
#include <stdexcept>
#include <system_error>
#include <string>

namespace jai {

    // Using std::error_code pattern for zero-overhead error handling in hot paths
    enum class runtime_error_code {
        success = 0,  // Must be 0 for default-constructed error_code

        // Variable errors (1-10)
        undefined_variable = 1,
        invalid_reference = 3,
        max_recursion_depth = 4,

        // Type errors (11-30)
        type_mismatch = 11,
        not_a_function = 12,
        not_an_object = 13,
        not_a_class = 14,
        not_an_array = 15,
        not_a_map = 16,
        invalid_numeric_operand = 17,
        invalid_weak_ptr_conversion = 18,
        invalid_shared_ptr_conversion = 19,

        // Arithmetic errors (31-40)
        division_by_zero = 31,
        modulo_by_zero = 32,

        // Array/Map errors (41-50)
        index_out_of_bounds = 41,
        invalid_index_type = 42,
        array_empty = 43,
        map_key_not_found = 44,
        array_element_type_mismatch = 45,
        map_value_type_mismatch = 46,
        map_key_type_mismatch = 47,

        // Function/Method errors (51-70)
        argument_count_mismatch = 51,
        no_constructor_found = 52,
        invalid_super_call = 53,
        this_outside_method = 54,
        super_outside_method = 55,
        no_parent_class = 56,
        constructor_failed = 57,
        evaluation_failed = 58,
        member_access_on_null = 59,
        ambiguous_call = 60,   // overloads tie on the arguments

        // Class errors (71-80)
        class_not_found = 71,
        member_not_found = 72,
        static_member_not_found = 73,
        multiple_inheritance = 74,
        access_violation = 75,
        script_layer_retired = 76,   // a detached/moved script layer reached for its C++ object
        extend_rejected = 77,        // extend(object, Class): class/object relationship refused

        // File I/O errors (81-90)
        file_not_found = 81,
        include_path_invalid = 82,
        insufficient_privilege = 83,

        // Operator errors (101-110)
        unknown_operator = 101,
        unsupported_operation = 102,
        invalid_assignment_target = 103,
        invalid_operation = 104,

        // Capture errors (111-120)
        capture_undefined_variable = 111,
        capture_reference_failed = 112,

        // Engine errors (121-130)
        engine_destroyed = 121,
        stdlib_not_loaded = 122,
        cpp_exception = 123,
        internal_error = 124,
        execution_budget_exceeded = 125,
        memory_cap_exceeded = 126,
        // Interpreter-internal: a yield unwinding the expression it sits in; consumed by the
        // statement that owns the expression, never surfaced by a correct replay
        coroutine_suspended = 127,
        run_cancelled = 128,   // script_run::cancel: the suspended run's sleep raises it, terminal
    };

    class runtime_error_category_impl : public std::error_category {
    public:
        const char* name() const noexcept override {
            return "jaiscript.runtime";
        }

        std::string message(int ev) const override;
    };

    const std::error_category& runtime_error_category() noexcept;

    inline std::error_code make_error_code(runtime_error_code e) noexcept {
        return {static_cast<int>(e), runtime_error_category()};
    }

    class runtime_exception : public std::system_error {
    public:
        runtime_exception(std::error_code ec, const std::string& context)
            : std::system_error(ec, context) {}

        runtime_exception(runtime_error_code ec, const std::string& context)
            : std::system_error(make_error_code(ec), context) {}
    };

    namespace detail {
        inline constexpr const char* bare_throw_text = "Bare throw with no active exception thrown.";

        // The text a native exception raises in script: an allocation the platform refuses reads alike everywhere
        inline const char* native_exception_text(const std::exception& e) noexcept {
            return dynamic_cast<const std::bad_alloc*>(&e) || dynamic_cast<const std::length_error*>(&e) ? "Out of memory" : e.what();
        }
    }

} // namespace jai

// Register for implicit conversion to error_code
namespace std {
    template <>
    struct is_error_code_enum<jai::runtime_error_code> : true_type {};
}

#endif // __JAISCRIPT_CORE_RUNTIME_ERRORS_HPP__
