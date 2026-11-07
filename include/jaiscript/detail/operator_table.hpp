#pragma once

#ifndef __JAISCRIPT_DETAIL_OPERATOR_TABLE_HPP__
#define __JAISCRIPT_DETAIL_OPERATOR_TABLE_HPP__

#include <jaiscript/core/value.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <array>
#include <optional>
#include <string_view>

namespace jai::detail {

    // ---- THE operator enumeration (single source of truth) --------------------------------
    // Every operator symbol user code can register globally (C++ add_function/add_global) and
    // both backends can dispatch to. Token->slot and name->slot below must stay the ONLY
    // mappings - do not grow new operator-symbol lists elsewhere (engine gate, backend
    // probes, and the transient-read gate all key off this enum).
    enum class op_slot : uint8_t {
        plus, minus, star, slash, percent,
        less, less_equal, greater, greater_equal, equal_equal, bang_equal, spaceship,
        ampersand, pipe, caret, left_shift, right_shift,
        subscript,   // "[]" - the custom subscript operator
        count,
        none = count
    };

    // the compound index of a binary operator (+ - * / % & | ^ << >> -> 0..9, the order of detail::compound_mismatch)
    inline int compound_index(token_type op) noexcept {
        switch (op) {
            case token_type::minus: return 1;
            case token_type::star: return 2;
            case token_type::slash: return 3;
            case token_type::percent: return 4;
            case token_type::ampersand: return 5;
            case token_type::pipe: return 6;
            case token_type::caret: return 7;
            case token_type::left_shift: return 8;
            case token_type::right_shift: return 9;
            default: return 0;
        }
    }

    // a compound assignment's binary operator (`&=` -> `&`); token_type::equal for any other token
    inline token_type compound_base(token_type op) noexcept {
        switch (op) {
            case token_type::plus_equal: return token_type::plus;
            case token_type::minus_equal: return token_type::minus;
            case token_type::star_equal: return token_type::star;
            case token_type::slash_equal: return token_type::slash;
            case token_type::percent_equal: return token_type::percent;
            case token_type::ampersand_equal: return token_type::ampersand;
            case token_type::pipe_equal: return token_type::pipe;
            case token_type::caret_equal: return token_type::caret;
            case token_type::left_shift_equal: return token_type::left_shift;
            case token_type::right_shift_equal: return token_type::right_shift;
            default: return token_type::equal;
        }
    }

    inline bool bitwise_token(token_type op) noexcept {
        return op == token_type::ampersand || op == token_type::pipe || op == token_type::caret ||
               op == token_type::left_shift || op == token_type::right_shift;
    }

    inline bool compound_is_bitwise(token_type op) noexcept { return bitwise_token(compound_base(op)); }

    inline const char* binary_op_name(token_type op) noexcept {
        switch (op) {
            case token_type::minus: return "-";
            case token_type::star: return "*";
            case token_type::slash: return "/";
            case token_type::percent: return "%";
            case token_type::ampersand: return "&";
            case token_type::pipe: return "|";
            case token_type::caret: return "^";
            case token_type::left_shift: return "<<";
            case token_type::right_shift: return ">>";
            default: return "+";
        }
    }

    inline op_slot binary_op_slot(token_type t) noexcept {
        switch (t) {
            case token_type::plus: return op_slot::plus;
            case token_type::minus: return op_slot::minus;
            case token_type::star: return op_slot::star;
            case token_type::slash: return op_slot::slash;
            case token_type::percent: return op_slot::percent;
            case token_type::less: return op_slot::less;
            case token_type::less_equal: return op_slot::less_equal;
            case token_type::greater: return op_slot::greater;
            case token_type::greater_equal: return op_slot::greater_equal;
            case token_type::equal_equal: return op_slot::equal_equal;
            case token_type::bang_equal: return op_slot::bang_equal;
            case token_type::spaceship: return op_slot::spaceship;
            case token_type::ampersand: return op_slot::ampersand;
            case token_type::pipe: return op_slot::pipe;
            case token_type::caret: return op_slot::caret;
            case token_type::left_shift: return op_slot::left_shift;
            case token_type::right_shift: return op_slot::right_shift;
            default: return op_slot::none;
        }
    }

    inline op_slot op_slot_for_name(std::string_view name) noexcept {
        if (name.size() == 1) {
            switch (name[0]) {
                case '+': return op_slot::plus;
                case '-': return op_slot::minus;
                case '*': return op_slot::star;
                case '/': return op_slot::slash;
                case '%': return op_slot::percent;
                case '<': return op_slot::less;
                case '>': return op_slot::greater;
                case '&': return op_slot::ampersand;
                case '|': return op_slot::pipe;
                case '^': return op_slot::caret;
                default: return op_slot::none;
            }
        }
        if (name == "==") return op_slot::equal_equal;
        if (name == "!=") return op_slot::bang_equal;
        if (name == "<=") return op_slot::less_equal;
        if (name == ">=") return op_slot::greater_equal;
        if (name == "<=>") return op_slot::spaceship;
        if (name == "<<") return op_slot::left_shift;
        if (name == ">>") return op_slot::right_shift;
        if (name == "[]") return op_slot::subscript;
        return op_slot::none;
    }

    // ---- Per-engine flat dispatch table ----------------------------------------------------
    // Holds the CURRENT global dispatcher value for each registered operator (the OverloadSet
    // dispatcher or the lone registered function - overload/arity/type matching stays inside
    // that value). Resolution is one mask test + one array read: no environment probe, no
    // hashing, no string keys anywhere on an operator path. Owned by engine::implementation;
    // refreshed at the registration chokepoints; backends hold a const pointer (wired by
    // wire_backend). Registration is main-thread / outside parallel regions, so reads need
    // no synchronization.
    class engine_operator_table {
    public:
        [[nodiscard]] bool any() const noexcept { return mask_ != 0; }

        [[nodiscard]] const script_value* entry(op_slot s) const noexcept {
            if (s == op_slot::none || ((mask_ >> static_cast<unsigned>(s)) & 1u) == 0) {
                return nullptr;
            }
            return &*entries_[static_cast<size_t>(s)];
        }

        void set(op_slot s, script_value dispatcher) {
            entries_[static_cast<size_t>(s)] = std::move(dispatcher);
            mask_ |= 1u << static_cast<unsigned>(s);
        }

        void clear(op_slot s) noexcept {
            entries_[static_cast<size_t>(s)].reset();
            mask_ &= ~(1u << static_cast<unsigned>(s));
        }

    private:
        std::array<std::optional<script_value>, static_cast<size_t>(op_slot::count)> entries_{};
        uint32_t mask_ = 0;
    };

    static_assert(static_cast<size_t>(op_slot::count) <= 32, "mask_ is 32 bits");

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_OPERATOR_TABLE_HPP__
