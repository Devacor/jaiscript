#pragma once

#ifndef __JAISCRIPT_DETAIL_RETURN_CONV_HPP__
#define __JAISCRIPT_DETAIL_RETURN_CONV_HPP__

// Typed-return classification shared by both backends: which route a `return` takes
// through the one typed-slot table (typed_slot.hpp). Derived from the declared return
// type alone (engine-independent), memoized per function through return_conv_of.
// none = untyped/any/void/auto (deref only); ref = the pass-through kernel
// (ref_lvalue.hpp ref_return_pass_through); prim_* = a storage-matching plain result is
// the identity (exactly what the table's identity row yields); check = the full table.

#include <jaiscript/core/type_info.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/uniquely_held.hpp>

namespace jai::detail {

    enum class return_conv : uint8_t {
        unclassified = 0,
        none,
        ref,
        prim_int,
        prim_float,
        prim_bool,
        prim_char,
        prim_string,
        prim_shared,
        prim_weak,
        check,
    };

    inline return_conv classify_return_conv(const type_info* t) {
        if (!t || t->base_type == script_value_type::jai_any_type) {
            return return_conv::none;
        }
        if (t->base_type == script_value_type::jai_reference_type) {
            return return_conv::ref;
        }
        if (t->type_name.empty() || t->type_name == "void" || t->type_name == "auto") {
            return return_conv::none;
        }
        switch (t->base_type) {
            case script_value_type::jai_int_type: return return_conv::prim_int;
            case script_value_type::jai_float_type: return return_conv::prim_float;
            case script_value_type::jai_bool_type: return return_conv::prim_bool;
            case script_value_type::jai_char_type: return return_conv::prim_char;
            case script_value_type::jai_string_type: return return_conv::prim_string;
            case script_value_type::jai_shared_ptr_type: return return_conv::prim_shared;   // a handle binds a handle slot as-is, whatever its pointee
            case script_value_type::jai_weak_ptr_type: return return_conv::prim_weak;
            default: return return_conv::check;
        }
    }

    // Memo on script_defined_function / function_decl::backend_return_conv (mutable; the
    // idempotent write is benign - parallel workers hold private copies)
    template <typename F>
    inline return_conv return_conv_of(const F& fn) {
        if (fn.backend_return_conv == 0) {
            fn.backend_return_conv = static_cast<uint8_t>(classify_return_conv(fn.return_type.get()));
        }
        return static_cast<return_conv>(fn.backend_return_conv);
    }

    // prim_* lanes: a storage-matching plain result needs no conversion (the raw index
    // excludes bound values and references, which take the full route)
    inline bool return_conv_identity(return_conv conv, const script_value& v) noexcept {
        switch (conv) {
            case return_conv::prim_int: return v.raw_storage_index() == script_value::TYPEID_INT;
            case return_conv::prim_float: return v.raw_storage_index() == script_value::TYPEID_FLOAT;
            case return_conv::prim_bool: return v.raw_storage_index() == script_value::TYPEID_BOOL;
            case return_conv::prim_char: return v.raw_storage_index() == script_value::TYPEID_CHAR;
            case return_conv::prim_string: return v.raw_storage_index() == script_value::TYPEID_STRING;
            case return_conv::prim_weak: return v.raw_storage_index() == script_value::TYPEID_WEAK_PTR;
            default: return false;
        }
    }


    // ---- By-value return materialization (C++ [stmt.return]) ----
    // The result of a by-value `return e;` aliases nothing the callee could name. Operand
    // classes (return_stmt::return_operand_class memo on the interpreter, the compiler's
    // operand on the vm): a temporary (call, literal, operator, `this`, construction) moves;
    // an lvalue read (member, element, ternary over lvalues, assignment) copies through the
    // ONE store kernel (clone_for_assignment: values deep-copy, shared_ptr handles share,
    // scalars/strings are copies already); an identifier copies too unless it names the
    // callee's OWN slot, whose payload moves out of the dying frame instead (NRVO /
    // implicit move). A member read off a call's result (`make().m`, C++'s xvalue) moves when
    // nothing else holds the value it read (the temporary died with the read) and copies when
    // something does (a handle's object other handles keep). A reference result (element read,
    // ref-returning call) copies its target the same way. `-> T&` returns and the top-level
    // program's `return` never enter here. Both backends run exactly these functions.
    inline constexpr uint8_t return_operand_unclassified = 0;
    inline constexpr uint8_t return_operand_temporary = 1;
    inline constexpr uint8_t return_operand_ident = 2;
    inline constexpr uint8_t return_operand_lvalue = 3;
    inline constexpr uint8_t return_operand_temporary_member = 4;   // a member chain over a call that may yield a handle
    inline constexpr uint8_t return_operand_value_member = 5;       // a member chain over a call that yields a class by value: an xvalue
    inline constexpr uint8_t return_operand_parameter = 6;          // the callee's own by-value parameter

    // the call a member chain reads through (null: the chain starts elsewhere)
    inline const call_expr* member_chain_call(const expression* e) noexcept {
        while (e && e->get_type() == node_type::member_expr) {
            const auto* m = static_cast<const member_expr*>(e);
            if (m->is_static) return nullptr;
            e = m->object.get();
        }
        return e && e->get_type() == node_type::call_expr ? static_cast<const call_expr*>(e) : nullptr;
    }
    inline bool member_of_call_result(const expression* e) noexcept { return member_chain_call(e) != nullptr; }

    inline uint8_t classify_return_operand(const expression* e) noexcept {
        if (!e) return return_operand_temporary;
        switch (e->get_type()) {
            case node_type::identifier_expr: return return_operand_ident;
            case node_type::member_expr: return member_of_call_result(e) ? return_operand_temporary_member : return_operand_lvalue;
            case node_type::assignment_expr: return return_operand_lvalue;
            case node_type::binary_expr: {
                const auto* b = static_cast<const binary_expr*>(e);
                if (b->op.type == token_type::question_question) {   // either operand's value, as a conditional's arms
                    return classify_return_operand(b->left.get()) != return_operand_temporary ||
                           classify_return_operand(b->right.get()) != return_operand_temporary
                        ? return_operand_lvalue : return_operand_temporary;
                }
                return b->op.type == token_type::left_bracket ? return_operand_lvalue : return_operand_temporary;
            }
            case node_type::ternary_expr: {
                const auto* t = static_cast<const ternary_expr*>(e);
                return classify_return_operand(t->then_expression.get()) != return_operand_temporary ||
                       classify_return_operand(t->else_expression.get()) != return_operand_temporary
                    ? return_operand_lvalue : return_operand_temporary;
            }
            default: return return_operand_temporary;
        }
    }

    // Raw indexes a by-value return hands out as they are: scalars, strings, functions and
    // handles are copies already (the prim_* identity lanes see exactly the value they saw).
    // The ONE-test fast head of every return door: a plain result runs the pre-materialization
    // door byte for byte.
    inline constexpr uint32_t return_plain_mask =
        (1u << script_value::TYPEID_NULL) | (1u << script_value::TYPEID_INT) | (1u << script_value::TYPEID_FLOAT) |
        (1u << script_value::TYPEID_STRING) | (1u << script_value::TYPEID_CHAR) | (1u << script_value::TYPEID_BOOL) |
        (1u << script_value::TYPEID_FUNCTION) | (1u << script_value::TYPEID_WEAK_PTR);
    inline bool return_value_plain(const script_value& v) noexcept {
        return ((1u << v.raw_storage_index()) & return_plain_mask) != 0;
    }

    // In place: a reference copies its target; a heavy lvalue read and a host object a T& result lends deep-copy (bound
    // values decode); a heavy temporary and every plain value stay as they are. An object a scalar return type converts is
    // read in place (slot_converts_object).
    inline void materialize_return_value(script_value& v, bool lvalue_read, const type_info* declared = nullptr) {
        if (return_value_plain(v)) return;
        if (slot_converts_object(v.deref(), declared)) [[unlikely]] {
            if (v.is_reference()) { v = script_value(v.deref()); }
            return;
        }
        if (!v.is_reference()) {
            const type_info* tag = v.type_info_raw();
            if (tag && tag->base_type == script_value_type::jai_shared_ptr_type) return;   // a shared handle's copy is itself (clone_for_assignment shares it)
        }
        if (lvalue_read || v.is_reference() || borrows_host_object(v)) { v = clone_for_assignment(v); }
    }

    // The operand class with C++'s category of a member chain over a call: an xvalue when the call yields a class by value
    // (value_call(call), the backend's engine-aware test), else the chain may read through a handle
    template <class ValueCall>
    inline uint8_t classify_source(const expression* e, ValueCall&& value_call) {
        const uint8_t operand_class = classify_return_operand(e);
        if (operand_class != return_operand_temporary_member) { return operand_class; }
        const call_expr* call = member_chain_call(e);
        return call && value_call(*call) ? return_operand_value_member : operand_class;
    }

    // A value taken in place of its copy (a by-value return, argument or capture) is the copy C++ would make or skip: a
    // prvalue is its destination, and when the copy can be observed (a destructor or a user copy constructor in its tree)
    // nothing but a prvalue, or an xvalue whose copy runs no user copy constructor, stands for it
    inline bool source_skips_copy(uint8_t operand_class, const script_value& v) {
        if (operand_class == return_operand_temporary) { return true; }
        if (!uniquely_held(v)) { return false; }
        if (operand_class == return_operand_value_member) { return !move_runs_copy_constructor(v); }
        return operand_class == return_operand_temporary_member && !value_copies_observably(v);
    }

    // A by-value return of this operand class copies the value it read: a temporary never, a member read off a call's
    // result unless it stands for its copy, any other lvalue always
    inline bool return_operand_copies(uint8_t operand_class, const script_value& v) {
        if (operand_class == return_operand_temporary) { return false; }
        if (operand_class == return_operand_temporary_member || operand_class == return_operand_value_member) { return !source_skips_copy(operand_class, v); }
        return true;
    }

    // Only an instance held by value can run a user copy constructor when it moves (a handle shares, a container moves its storage)
    inline bool may_run_copy_constructor_on_move(const script_value& v) noexcept {
        if (v.raw_storage_index() != script_value::TYPEID_OBJECT) { return false; }
        const type_info* tag = v.type_info_raw();
        return !tag || tag->base_type != script_value_type::jai_shared_ptr_type;
    }

    // A yield hands out what a return of the same operand would (materialize_return_value), except that no slot moves
    // out: the body resumes and still owns its locals, so a named local copies like any lvalue
    inline bool yield_operand_is_lvalue(const expression* e) noexcept {
        return classify_return_operand(e) != return_operand_temporary;
    }

    inline bool same_heavy_node(const script_value& a, const script_value& b) noexcept {
        const size_t idx = a.raw_storage_index();
        if (idx != b.raw_storage_index()) return false;
        switch (idx) {
            case script_value::TYPEID_ARRAY: return a.unchecked_array_node() == b.unchecked_array_node();
            case script_value::TYPEID_MAP: return &a.unchecked_as_map() == &b.unchecked_as_map();
            case script_value::TYPEID_OBJECT: return a.unchecked_object_handle().get() == b.unchecked_object_handle().get();
            default: return false;
        }
    }

    // A conversion's result that reads nothing of its source (a converting constructor's new object, held nowhere else)
    inline bool converted_temporary(const script_value& converted, const script_value& source) {
        return !converted.is_reference() && uniquely_held(converted) && !same_heavy_node(converted, source.deref());
    }

    // The dying frame slot behind `return <ident>;` when it carries the returned heavy node
    // itself (unboxed) or through a cell nobody else holds (a finished ref-param bind): the
    // payload to release so the returned copy becomes a move. Null = copy (a global, a
    // member, a cell a live closure or alias shares, a scalar).
    inline script_value* return_own_slot_payload(script_value* slot, const script_value& returned) noexcept {
        if (!slot) return nullptr;
        script_value* payload = slot->raw_storage_index() == script_value::TYPEID_REFERENCE
            ? slot->unique_cell_payload() : slot;
        return payload && same_heavy_node(*payload, returned) ? payload : nullptr;
    }

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_RETURN_CONV_HPP__
