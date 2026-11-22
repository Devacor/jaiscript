#pragma once

#ifndef __JAISCRIPT_CORE_SCRIPT_TYPE_HPP__
#define __JAISCRIPT_CORE_SCRIPT_TYPE_HPP__

#include <jaiscript/core/type_info.hpp>
#include <string>

namespace jai {

// A type a script passes as a type argument: db.query<User>(sql) calls db.query(User, sql), and User arrives as the value
// reflect::type, which a host function or bound method takes as a `const jai::script_type&` parameter
// (docs/site/embedding/17-host-api.html). It holds the type as the script spelled it; a class is named, not captured, so the
// class a host builds from it is the one loaded when it builds (hot reload replaces it like any class).
class script_type {
public:
    // only a type argument makes one: a script cannot construct a reflect::type
    explicit script_type(type_info_ptr a_type) : type_(a_type) {}

    // The canonical spelling: "User", "array<User>", "shared_ptr<shop::Product>", "int"
    std::string name() const;
    std::string to_string() const { return name(); }
    // The class the type names: T for a class T or a shared_ptr<T>; empty for any other type
    std::string class_name() const;
    // The engine's interned type (engine-owned: it lives as long as the engine)
    type_info_ptr info() const { return type_; }
    explicit operator bool() const { return type_.get() != nullptr; }

private:
    type_info_ptr type_;
};

} // namespace jai

#endif // __JAISCRIPT_CORE_SCRIPT_TYPE_HPP__
