#include <jaiscript/core/runtime_errors.hpp>

namespace jai {

std::string runtime_error_category_impl::message(int ev) const {
    switch (static_cast<runtime_error_code>(ev)) {
        case runtime_error_code::success:
            return "Success";

        // Variable errors
        case runtime_error_code::undefined_variable:
            return "Undefined variable";
        case runtime_error_code::invalid_reference:
            return "Invalid reference";
        case runtime_error_code::max_recursion_depth:
            return "Maximum recursion depth exceeded";
        case runtime_error_code::execution_budget_exceeded:
            return "Script execution budget exceeded";
        case runtime_error_code::memory_cap_exceeded:
            return "Script memory cap exceeded";
        case runtime_error_code::coroutine_suspended:
            return "Coroutine suspended inside an expression the replay does not cover";
        case runtime_error_code::run_cancelled:
            return "Run cancelled";

        // Type errors
        case runtime_error_code::type_mismatch:
            return "Type mismatch";
        case runtime_error_code::not_a_function:
            return "Cannot call non-function value";
        case runtime_error_code::not_an_object:
            return "Value is not an object";
        case runtime_error_code::not_a_class:
            return "Value is not a class";
        case runtime_error_code::not_an_array:
            return "Value is not an array";
        case runtime_error_code::not_a_map:
            return "Value is not a map";
        case runtime_error_code::invalid_numeric_operand:
            return "Invalid numeric operand";
        case runtime_error_code::invalid_weak_ptr_conversion:
            return "Invalid weak_ptr conversion";
        case runtime_error_code::invalid_shared_ptr_conversion:
            return "Invalid shared_ptr conversion";

        // Arithmetic errors
        case runtime_error_code::division_by_zero:
            return "Division by zero";
        case runtime_error_code::modulo_by_zero:
            return "Modulo by zero";

        // Array/Map errors
        case runtime_error_code::index_out_of_bounds:
            return "Array index out of bounds";
        case runtime_error_code::invalid_index_type:
            return "Array index must be an integer";
        case runtime_error_code::array_empty:
            return "Cannot access element of empty array";
        case runtime_error_code::map_key_not_found:
            return "Map key not found";
        case runtime_error_code::array_element_type_mismatch:
            return "Array element type mismatch";
        case runtime_error_code::map_value_type_mismatch:
            return "Map value type mismatch";
        case runtime_error_code::map_key_type_mismatch:
            return "Map key type mismatch";
        case runtime_error_code::member_access_on_null:
            return "Member access on null value";

        // Function/Method errors
        case runtime_error_code::argument_count_mismatch:
            return "Function argument count mismatch";
        case runtime_error_code::ambiguous_call:
            return "Ambiguous call";
        case runtime_error_code::no_constructor_found:
            return "No constructor found";
        case runtime_error_code::invalid_super_call:
            return "Invalid super() call";
        case runtime_error_code::this_outside_method:
            return "'this' can only be used inside methods";
        case runtime_error_code::super_outside_method:
            return "'super' can only be used inside methods";
        case runtime_error_code::no_parent_class:
            return "Class has no parent class";

        // Class errors
        case runtime_error_code::class_not_found:
            return "Class not found";
        case runtime_error_code::member_not_found:
            return "Member not found";
        case runtime_error_code::script_layer_retired:
            return "Script layer was detached: this handle no longer reaches its object";
        case runtime_error_code::extend_rejected:
            return "extend rejected";
        case runtime_error_code::static_member_not_found:
            return "Static member not found";
        case runtime_error_code::multiple_inheritance:
            return "Multiple inheritance not supported";
        case runtime_error_code::access_violation:
            return "Member access violation";

        // File I/O errors
        case runtime_error_code::file_not_found:
            return "File not found";
        case runtime_error_code::include_path_invalid:
            return "Include path must be a string";
        case runtime_error_code::insufficient_privilege:
            return "Insufficient privilege";

        // Control flow errors

        // Operator errors
        case runtime_error_code::unknown_operator:
            return "Unknown operator";
        case runtime_error_code::unsupported_operation:
            return "Unsupported operation";
        case runtime_error_code::invalid_assignment_target:
            return "Invalid assignment target";
        case runtime_error_code::invalid_operation:
            return "Invalid operation";

        // Capture errors
        case runtime_error_code::capture_undefined_variable:
            return "Cannot capture undefined variable";
        case runtime_error_code::capture_reference_failed:
            return "Cannot capture variable by reference";

        // Engine errors
        case runtime_error_code::engine_destroyed:
            return "Engine reference destroyed";
        case runtime_error_code::stdlib_not_loaded:
            return "Standard library not loaded";
        case runtime_error_code::cpp_exception:
            return "C++ exception";

        default:
            return "Unknown runtime error";
    }
}

const std::error_category& runtime_error_category() noexcept {
    static runtime_error_category_impl instance;
    return instance;
}

} // namespace jai
