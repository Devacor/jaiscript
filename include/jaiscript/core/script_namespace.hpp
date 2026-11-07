#pragma once

#ifndef __JAISCRIPT_CORE_SCRIPT_NAMESPACE_HPP__
#define __JAISCRIPT_CORE_SCRIPT_NAMESPACE_HPP__

#include <jaiscript/core/value.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

namespace jai {

class function_decl;
class class_definition;
class environment;

// A host entry's static signature for the checker: the arity range and
// the kind each argument must have (jai_string_type = text, a string or a utf8_string; jai_null_type = anything); the
// entry still checks its arguments at run time. result_follows_first: the result is text of the first argument's kind.
// jai_float_type is a number (an int or a float); a jai_object_type result names its class in result_class. nodiscard: an
// expression statement that drops the result fails the load; in_place_twin: the entry copies what the same-named method
// changes in place (string::to_upper and s.to_upper()), which the load error names.
struct host_entry_signature {
    uint8_t min_args = 0;
    uint8_t max_args = 0;
    std::vector<script_value_type> params;
    script_value_type result = script_value_type::jai_null_type;
    bool result_follows_first = false;
    std::string result_class;
    bool nodiscard = false;
    bool in_place_twin = false;
};

// Flat script namespace registry entry ("a::b::c" is one namespace name, no nesting).
// Engine-owned (see engine::script_namespaces()) so namespaces survive across backends.
struct script_namespace_data {
    std::unordered_map<uint64_t, std::vector<std::shared_ptr<function_decl>>> functions;  // function_name_id -> overloads
    std::unordered_map<uint64_t, script_value> variables;                                 // variable_name_id -> value
    std::unordered_map<uint64_t, std::shared_ptr<class_definition>> classes;              // class_name_id -> definition
    std::shared_ptr<environment> scope;   // the cells of the script-declared variables and functions, parented on the enclosing namespace's scope, else the global environment (detail/namespace_scope.hpp)
    uint64_t enclosing = 0;   // the namespace "a" for "a::b" once the scope exists; 0 for one at program scope
    std::shared_ptr<environment> host_layer;   // over the scope: the host entries its classes' members read by bare name (detail/namespace_function.hpp)
    std::unordered_map<uint64_t, host_entry_signature> signatures;   // variable_name_id -> a host entry's signature
};

// Engine-owned namespaces: reflect:: answers and jai:: capabilities cannot be
// supplied by script declarations. math:: stays deliberately OPEN. Flat namespace
// names make "jai::sub" its own name, so reserve both root-qualified prefixes too.
inline bool is_reserved_namespace_name(std::string_view name) {
    return name == "reflect" || name.starts_with("reflect::") ||
        name == "jai" || name.starts_with("jai::");
}

inline error_propagator reserved_namespace_error(uint64_t name_id) {
    return error_propagator{make_error_code(runtime_error_code::type_mismatch),
        "Namespace '{0}' is reserved for the engine (C++-registered only)", name_id};
}

} // namespace jai

#endif // __JAISCRIPT_CORE_SCRIPT_NAMESPACE_HPP__
