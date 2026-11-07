#pragma once

#include "jaiscript/core/dynamic_binder.hpp"
#include "jaiscript/core/script_class.hpp"
#include <memory>
#include <map>
#include <unordered_map>
#include <string>

namespace jai {

class class_registry {
public:
    // a_declaration: the class_decl the definition ran (a first definition the program pools modeled moves no checker surface)
    checked_result<void> register_script_class(std::shared_ptr<script_class_definition> class_def, const class_decl* a_declaration = nullptr);
    checked_result<void> register_cpp_class(std::shared_ptr<class_definition> class_def);
    std::shared_ptr<script_class_definition> find_script_class(const std::string& name);
    std::shared_ptr<script_class_definition> find_script_class(uint64_t type_id) const;

    std::map<std::string, std::shared_ptr<script_class_definition>> script_classes_;
    std::unordered_map<uint64_t, std::shared_ptr<script_class_definition>> script_classes_by_type_id_;
    std::map<std::string, std::shared_ptr<class_definition>> cpp_classes_;

private:
    void validate_class_hierarchy(const std::string& class_name);
};

namespace detail {
    // What a backend lends the one class-declaration builder both backends run
    struct class_declaration_host {
        engine* eng;
        string_symbolizer* symbols;
        const std::shared_ptr<environment>& env;   // the definition environment: base lookups and method definition envs
        uint64_t class_definition_type_id;
        const std::function<std::shared_ptr<class_definition>(const std::string&)>& class_lookup;
        unit_stamp unit;
        // the backend's evaluation of an initializer, in `env` when it is given (null: where the declaration runs)
        std::function<checked_result<script_value>(const expression_ptr&, const std::shared_ptr<environment>& env)> eval;
        std::function<void()> discard_unwinding;   // a migrate hook's error belongs to the migration, not the declaration
    };

    // A class declaration: a new script class, or a redefinition that migrates the live one in place
    checked_result<void> declare_script_class(class_decl* decl, const class_declaration_host& host);
}

} // namespace jai