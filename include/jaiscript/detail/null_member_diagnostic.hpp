#pragma once

#include <jaiscript/detail/ast.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace jai {

// The symbol naming a member-access receiver: `obj.frob()` -> obj, `game().creature(id).dot()` ->
// creature. Zero when the receiver has no name (a literal, an index, a ternary).
inline uint64_t null_member_receiver_symbol(const expression* receiver) {
    if (!receiver) { return 0; }
    switch (receiver->get_type()) {
        case node_type::identifier_expr: return static_cast<const identifier_expr*>(receiver)->symbol_id;
        case node_type::member_expr: return static_cast<const member_expr*>(receiver)->member_id;
        case node_type::call_expr: return null_member_receiver_symbol(static_cast<const call_expr*>(receiver)->callee.get());
        default: return 0;
    }
}

// {0} = the member, {1} = the receiver symbol (only used by the shapes that have one);
// as_callee = the member is being called
inline std::string_view null_member_message(const expression* receiver, bool as_callee = false) {
    if (null_member_receiver_symbol(receiver) == 0) {
        return as_callee ? "Cannot call '{0}' on null" : "Cannot access '{0}' on null";
    }
    if (receiver->get_type() == node_type::call_expr) {
        return as_callee ? "Cannot call '{0}' on null ('{1}(...)' returned null)"
                         : "Cannot access '{0}' on null ('{1}(...)' returned null)";
    }
    return as_callee ? "Cannot call '{0}' on null ('{1}' is null)"
                     : "Cannot access '{0}' on null ('{1}' is null)";
}

// A non-null receiver with no member table at all (int, float, bool, char, function, ...):
// {0} = the receiver's base type spelling, {1} = the member
inline constexpr std::string_view scalar_member_message = "{0} has no member '{1}'";

// `C.s` where C names a class (its value is the constructor): a dot reaches an instance's members, a class's own are C::s.
// The class name when the receiver is that shape, else empty.
template <class Engine>
inline std::string_view class_named_by_dot_receiver(const expression* receiver, const script_value& value, Engine* eng) {
    if (!eng || !receiver || receiver->get_type() != node_type::identifier_expr || !value.is_function()) { return {}; }
    const auto* id = static_cast<const identifier_expr*>(receiver);
    return detail::engine_access::get_class_definition(*eng, std::string(id->name)) ? id->name : std::string_view{};
}

inline std::string class_dot_member_text(std::string_view cls, std::string_view member) {
    return "'" + std::string(cls) + "' is a class; use " + std::string(cls) + "::" + std::string(member);
}

// `x.m` on a receiver without m: kind is the receiver's class or builtin kind (empty: object)
inline std::string no_member_text(std::string_view kind, std::string_view member) {
    return std::string(kind.empty() ? std::string_view("object") : kind) + " has no member '" + std::string(member) + "'";
}

// `x.m = v` on an object without m
inline std::string no_such_member_store_text(std::string_view member, std::string_view cls) {
    return "Cannot assign to '" + std::string(member) + "': " + std::string(cls.empty() ? std::string_view("object") : cls) + " has no such member";
}

// `x.m = v` on a non-object (cls: the class a dot receiver names, else empty)
inline std::string non_object_member_store_text(std::string_view cls, std::string_view member) {
    return cls.empty() ? std::string("Cannot assign to member of non-object type") : class_dot_member_text(cls, member);
}

// The spelling null_member_receiver_symbol names: `obj` for obj.m, `creature` for game().creature(id).m
inline std::string_view null_member_receiver_name(const expression* receiver) {
    if (!receiver) { return {}; }
    switch (receiver->get_type()) {
        case node_type::identifier_expr: return static_cast<const identifier_expr*>(receiver)->name;
        case node_type::member_expr: return static_cast<const member_expr*>(receiver)->member;
        case node_type::call_expr: return null_member_receiver_name(static_cast<const call_expr*>(receiver)->callee.get());
        default: return {};
    }
}

// `x.m = v` through a null receiver: the read's shapes (null_member_message) with the store's verb
inline std::string null_member_store_text(const expression* receiver, std::string_view member) {
    std::string out = "Cannot assign to '" + std::string(member) + "' on null";
    const std::string_view name = null_member_receiver_name(receiver);
    if (name.empty()) { return out; }
    if (receiver->get_type() == node_type::call_expr) { return out + " ('" + std::string(name) + "(...)' returned null)"; }
    return out + " ('" + std::string(name) + "' is null)";
}

// The raise's (text, symbol): a frozen symbolizer (a parallel worker) gets the name-free shape
template <class Symbolizer>
inline std::pair<std::string_view, uint64_t> class_dot_member_error(Symbolizer& symbols, bool frozen, std::string_view cls,
                                                                    std::string_view member) {
    if (frozen) { return {"A class's members are reached with '::'", 0}; }
    return {"{0}", symbols.intern(class_dot_member_text(cls, member))};
}

// The name a call site can attribute to its callee: an identifier's symbol, a member's
// member id, 0 for a computed callee (fs[0](x), pick()(x))
inline uint64_t callee_name_symbol(const expression* callee) {
    if (!callee) { return 0; }
    switch (callee->get_type()) {
        case node_type::identifier_expr: return static_cast<const identifier_expr*>(callee)->symbol_id;
        case node_type::member_expr: {
            const uint64_t id = static_cast<const member_expr*>(callee)->member_id;
            return id == UINT64_MAX ? 0 : id;
        }
        default: return 0;
    }
}

// Interned base-type spellings, filled once at backend init: error paths never intern
// (parallel workers run against a frozen symbolizer)
struct base_type_symbols {
    static constexpr size_t count = static_cast<size_t>(script_value_type::jai_invalid_type) + 1;
    uint64_t ids[count] = {};
    template <typename Symbolizer>
    void intern(const Symbolizer& symbolizer) {
        for (size_t i = 0; i < count; ++i) {
            ids[i] = symbolizer.intern(base_type_spelling(static_cast<script_value_type>(i)));
        }
    }
    uint64_t operator[](script_value_type type) const {
        const size_t i = static_cast<size_t>(type);
        return i < count ? ids[i] : ids[count - 1];
    }
};

// Calling a value that is not a function: named callee ->
// "Cannot call 'x': it is an int, not a function" ({0} = name, {1} = type), computed callee ->
// "Cannot call a value of type int: it is not a function" ({0} = type). The type spells the
// class for class instances, the base type otherwise; the article follows the spelling.
struct not_callable_text {
    std::string_view message;
    uint64_t sym0 = 0;
    uint64_t sym1 = 0;
};

inline not_callable_text not_callable_diagnostic(uint64_t name_id, script_value_type base, uint64_t base_id,
                                                 std::string_view class_name, uint64_t class_id) {
    const std::string_view spelling = class_name.empty() ? base_type_spelling(base) : class_name;
    const uint64_t type_id = class_name.empty() ? base_id : class_id;
    if (class_name.empty() && base == script_value_type::jai_null_type) {
        return name_id == 0 ? not_callable_text{"Cannot call null: it is not a function", 0, 0}
                            : not_callable_text{"Cannot call '{0}': it is null, not a function", name_id, 0};
    }
    if (name_id == 0) {
        return {"Cannot call a value of type {0}: it is not a function", type_id, 0};
    }
    const char c = spelling.empty() ? 'x' : static_cast<char>(spelling.front() | 0x20);
    const bool vowel = c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
    return {vowel ? "Cannot call '{0}': it is an {1}, not a function"
                  : "Cannot call '{0}': it is a {1}, not a function", name_id, type_id};
}

}
