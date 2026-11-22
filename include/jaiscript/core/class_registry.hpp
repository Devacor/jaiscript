#pragma once

#include "jaiscript/core/dynamic_binder.hpp"
#include "jaiscript/core/script_class.hpp"
#include "jaiscript/core/script_namespace.hpp"
#include <memory>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <string>
#include <vector>

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
    // The linker of generic instances (docs/site/guide/21-generics.html "When an instance is declared"): an instance is
    // declared the moment everything it is built on is declared. A unit offers its instances as it starts; one whose generic
    // or classes (its type arguments', its bases, a method instance's own class) are not all declared waits here, with what
    // the offering backend lent its declaration, and is defined when the last of them is (detail::declare_script_class).
    // One entry per instance, whatever the count of offers; one whose class is never declared stays, and a use of it
    // raises as a use of any class not declared does.
    struct waiting_instance {
        std::string key;                    // the instance: a class's name, or a method's with its class's
        std::shared_ptr<void> owner;        // the unit's node that holds the declaration
        class_decl* decl = nullptr;
        engine* eng = nullptr;
        string_symbolizer* symbols = nullptr;
        std::shared_ptr<environment> env;
        uint64_t class_definition_type_id = 0;
        std::function<std::shared_ptr<class_definition>(const std::string&)> class_lookup;
        unit_stamp unit;
        std::function<checked_result<script_value>(const expression_ptr&, const std::shared_ptr<environment>& env)> eval;
        std::function<void()> discard_unwinding;
    };
    std::vector<waiting_instance> waiting_instances_;       // in the order offered
    // The generics whose declaration has run, by qualified name: the stamp of each definition declared under it (a class's
    // generic method is declared as its class is)
    std::unordered_map<std::string, std::unordered_set<uint64_t>> declared_generics_;
    // A namespace's generic functions, by qualified name: the declaration that stands for each set of value parameters and
    // the unit that ran it. Another unit's declaration of the same parameters collides as a plain function's does, unless it
    // is written `override`; the unit's own run again replaces its own.
    struct namespace_generic {
        std::shared_ptr<const detail::generic_definition> definition;
        uint32_t unit = 0;
    };
    std::unordered_map<std::string, std::vector<namespace_generic>> namespace_generics_;
    // A generic function's instance a unit offered before its generic's declaration ran, bound when that declaration runs:
    // the function as the offering backend made it, to its name in `env` (detail::declare_script_function), or a
    // namespace's function, its declaration joining `space` (detail::namespace_function_waits)
    struct waiting_function {
        std::string generic;
        uint64_t stamp = 0;   // of the definition the instance was made from
        bool remade = false;  // function_decl::instance_remade: it waits for that definition's own declaration
        std::shared_ptr<environment> env;
        uint64_t name_id = 0;
        std::shared_ptr<script_defined_function> fn;
        script_value value;
        bool is_override = false;
        std::shared_ptr<script_namespace_data> space;
        uint64_t space_id = 0;
        std::shared_ptr<function_decl> declaration;
    };
    std::vector<waiting_function> waiting_functions_;       // in the order offered

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

    // A class declaration: a new script class, or a redefinition that migrates the live one in place. A generic's instance
    // (class_decl::instance_stamp, method_instances) is defined here when all it is built on is declared, else it waits; a
    // generic class's marker (generic_marker) and every class defined define the instances that waited for them.
    checked_result<void> declare_script_class(class_decl* decl, const class_declaration_host& host);
    // A function declaration's last step, the one both backends run: the name is bound to the function in `env`, alone or
    // in the overload set of its name (detail::declare_function_overload). A generic's instance (fn->generic_stamp) is bound
    // the moment its generic is declared: offered before that, it waits and is bound as the generic's declaration runs.
    checked_result<void> declare_script_function(engine& eng, const function_decl& decl, const std::shared_ptr<environment>& env,
                                                 const std::shared_ptr<script_defined_function>& fn, script_value value);
    // A namespace's function that is a generic's instance, offered before its generic's declaration ran: true, and it joins
    // `space` as that declaration runs; false for every other function (the backend declares it now)
    bool namespace_function_waits(engine& eng, const std::shared_ptr<script_namespace_data>& space, uint64_t space_id, const function_decl& decl);
    // "'Box<Item>' is not declared yet: it waits for 'Item'" for an instance a unit offered that still waits; empty otherwise
    std::string waiting_instance_text(engine& eng, const std::string& name);
    // The whole text of the error for a name that is not defined because it is a generic's instance that still waits (a class
    // a unit offered, a function whose generic's declaration has not run), in the symbolizer's storage; empty for any other
    // name. The program scope asks it for every undefined name (environment::explain_undefined).
    std::string_view undeclared_instance_text(engine& eng, uint64_t id);
    // The generic an instance's name spells: "wire::decode" for "wire::decode<Cart>", "Box<int>::map" for "Box<int>::map<string>"
    std::string_view instance_generic_name(std::string_view instance);
    // What an instance declared under `name` was made from, as the engine holds it now: the stamp of a class's definition or
    // of a method's on its class, and one stamp per overload a function's name holds; empty when nothing is declared there
    std::vector<uint64_t> declared_instance_stamps(engine& eng, const std::string& name);
}

} // namespace jai