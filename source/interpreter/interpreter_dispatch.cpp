#include "../../include/jaiscript/detail/interpreter.hpp"
#include <jaiscript/detail/binary_handlers.hpp>

namespace jai {

struct interpreter::binary_host {
    interpreter& in;
    bool joins_text = false;
    script_value make_int(script_int v) const noexcept { return in.make_value(v); }
    script_value make_float(script_float v) const noexcept { return in.make_value(v); }
    script_value make_bool(bool v) const noexcept { return in.make_value(v); }
    jai::engine* engine() const noexcept { return in.engine_; }
    checked_result<script_value> concat(const script_value& l, const script_value& r) { return in.concat_with_methods(l, r); }
    std::optional<checked_result<script_value>> object_arithmetic(const script_value& l, const script_value& r, uint64_t id) { return detail::object_arithmetic_via_method(*this, l, r, id); }
    bool operator_takes(const script_value& l, const script_value& r, uint64_t id) {
        if (in.parallel_worker_) [[unlikely]] { return detail::worker_operator_takes(l, r, id); }
        auto target = in.resolve_member_target(l);
        return target && binary_operator_accepts(target.class_def, id, r);
    }
    std::optional<checked_result<bool>> object_comparison(const script_value& l, const script_value& r, uint64_t id) { return detail::object_comparison_via_method(*this, l, r, id); }
    std::optional<checked_result<bool>> object_equality(const script_value& l, const script_value& r) { return detail::object_equality_via_method(*this, l, r); }
    std::optional<checked_result<bool>> object_inequality(const script_value& l, const script_value& r) { return detail::object_inequality_via_method(*this, l, r); }
    std::optional<checked_result<bool>> object_comparison_reversed(const script_value& l, const script_value& r, uint64_t id) { return detail::object_comparison_reversed_via_method(*this, l, r, id); }
    std::optional<checked_result<script_value>> object_spaceship_reversed(const script_value& l, const script_value& r) { return detail::object_spaceship_reversed_via_method(*this, l, r); }
    std::optional<checked_result<bool>> object_spaceship_equality(const script_value& l, const script_value& r) { return detail::object_spaceship_equality_via_method(*this, l, r); }
    std::optional<checked_result<script_value>> object_reflected(const script_value& l, const script_value& r, uint64_t id) { return detail::object_reflected_via_method(*this, l, r, id); }
    const class_definition* operand_class(const script_value& v) const {
        if (in.parallel_worker_) [[unlikely]] { return detail::operand_instance_class(v); }
        return v.deref().is_object() ? in.resolve_member_target(v).class_def : nullptr;
    }
    uint64_t equality_id() const noexcept { return in.eq_method_id_; }
    bool defines_custom_equality(const script_value& v) const { return in.object_defines_custom_equality(v); }
    bool worker_equality_refused(const script_value& v) const { return in.object_defines_custom_equality(v) && !in.worker_operators_pinned(v, { in.eq_method_id_, in.ops_.spaceship }); }
    bool unwinding() const noexcept { return in.is_unwinding_; }
    bool parallel_worker() const noexcept { return in.parallel_worker_; }
    size_t& equality_depth() noexcept { return in.structural_equality_depth_; }
    checked_result<script_value> spaceship_member(const script_value& a, const script_value& b, detail::order_walk* walk) {
        return detail::spaceship_operands(*this, in.ops_.spaceship, a, b, walk);
    }
    checked_result<script_value> equal(const script_value& l, const script_value& r) { return in.binary_operator(token_type::equal_equal, l, r); }
    member_target resolve_member_target(const script_value& v) const { return in.resolve_member_target(v); }
    script_value operator_value(const member_target& target, uint64_t id) const { return in.operator_value(target, id); }
    checked_result<script_value> operator_call(const script_value& self, const script_value& method, uint64_t id, std::vector<script_value> args) { return in.operator_call(self, method, id, std::move(args)); }
    bool worker_operators_pinned(const script_value& v, std::initializer_list<uint64_t> ids) const { return in.worker_operators_pinned(v, ids); }
    std::string_view worker_operator_refusal(const script_value& v, std::initializer_list<uint64_t> ids) const { return in.worker_operator_refusal(v, ids); }
    const detail::parallel_method_pin* worker_operator_pin(const script_value& v, uint64_t id) const { return in.worker_operator_pin(v, id); }
    bool comparison_free(const script_value&) const noexcept { return false; }
    const detail::operator_ids& ids() const noexcept { return in.ops_; }
    script_value make_null() const { return in.make_value(); }
    script_value bound_method(const script_value& self, const script_value& method) const { return create_bound_method(self, method); }
    execution_backend& backend() noexcept { return in; }
};

checked_result<script_value> interpreter::evaluate_arithmetic(const script_value& left, token_type op, const script_value& right, int compound) {
    binary_host h{*this};
    return detail::evaluate_arithmetic(h, left, op, right, compound);
}
checked_result<script_value> interpreter::compound_value(token_type a_op, const script_value& a_current, const script_value& a_right, bool a_incdec,
                                                         bool a_text_slot) {
    binary_host h{*this};
    return detail::compound_value(h, a_op, a_current, a_right, a_incdec, a_text_slot);
}
std::optional<checked_result<script_value>> interpreter::object_compound_operand(const script_value& left, token_type op, const script_value& right) {
    binary_host h{*this};
    return detail::object_compound_operand(h, left, op, right);
}
std::optional<checked_result<script_value>> interpreter::object_compound_through_binary(const script_value& target, const script_value& right, uint64_t op_symbol_id) {
    binary_host h{*this};
    return detail::object_arithmetic_via_method(h, target, right, op_symbol_id);
}
std::optional<checked_result<script_value>> interpreter::object_operator_in_place(const script_value& target, uint64_t base_id, const script_value& right, detail::incdec_form form) {
    binary_host h{*this};
    return detail::object_operator_in_place(h, target, base_id, right, form);
}
std::optional<checked_result<script_value>> interpreter::object_unary_via_method(const script_value& operand, token_type op) {
    binary_host h{*this};
    return detail::object_unary_via_method(h, operand, op);
}
// a op= b, or a desugared ++/--, on an object through its class's in-place operator (detail::object_operator_in_place)
std::optional<checked_result<script_value>> interpreter::compound_in_place(const script_value& target, const assignment_expr* expr, const script_value& right) {
    if (target.deref().raw_storage_index() != script_value::TYPEID_OBJECT) { return std::nullopt; }
    const detail::incdec_form form = !expr->incdec_desugar ? detail::incdec_form::none
                                     : expr->postfix_old_result ? detail::incdec_form::postfix : detail::incdec_form::prefix;
    return object_operator_in_place(target.deref(), compound_op_symbol(expr->op.type), right, form);
}
script_value interpreter::call_operator_of(const script_value& callee) {
    binary_host h{*this};
    return detail::call_operator_of(h, callee);
}

// a template string's `+` (the global operator table consulted already): a scalar beside the string becomes text.
// KEEP BYTE-PARALLEL with vm_backend::join_text
checked_result<script_value> interpreter::join_text(const script_value& left, const script_value& right) {
    binary_host h{*this, true};
    return detail::binary_arithmetic<detail::arith_op::add>(h, ops_.plus, left, right);
}

checked_result<script_value> interpreter::binary_operator(token_type op, const script_value& left, const script_value& right) {
    binary_host h{*this};
    switch (op) {
        case token_type::plus: return detail::binary_arithmetic<detail::arith_op::add>(h, ops_.plus, left, right);
        case token_type::minus: return detail::binary_arithmetic<detail::arith_op::sub>(h, ops_.minus, left, right);
        case token_type::star: return detail::binary_arithmetic<detail::arith_op::mul>(h, ops_.star, left, right);
        case token_type::slash: return detail::binary_arithmetic<detail::arith_op::div>(h, ops_.slash, left, right);
        case token_type::percent: return detail::binary_arithmetic<detail::arith_op::mod>(h, ops_.percent, left, right);
        case token_type::less: return detail::binary_ordering<detail::order_op::less>(h, ops_.less, left, right);
        case token_type::less_equal: return detail::binary_ordering<detail::order_op::less_equal>(h, ops_.less_equal, left, right);
        case token_type::greater: return detail::binary_ordering<detail::order_op::greater>(h, ops_.greater, left, right);
        case token_type::greater_equal: return detail::binary_ordering<detail::order_op::greater_equal>(h, ops_.greater_equal, left, right);
        case token_type::equal_equal: return detail::binary_equal(h, left, right);
        case token_type::bang_equal: return detail::binary_not_equal(h, left, right);
        case token_type::spaceship: return detail::binary_spaceship(h, ops_.spaceship, left, right);
        case token_type::ampersand: return detail::binary_bitwise<detail::bit_op::bit_and>(h, ops_.ampersand, left, right);
        case token_type::pipe: return detail::binary_bitwise<detail::bit_op::bit_or>(h, ops_.pipe, left, right);
        case token_type::caret: return detail::binary_bitwise<detail::bit_op::bit_xor>(h, ops_.caret, left, right);
        case token_type::left_shift: return detail::binary_bitwise<detail::bit_op::shift_left>(h, ops_.left_shift, left, right);
        case token_type::right_shift: return detail::binary_bitwise<detail::bit_op::shift_right>(h, ops_.right_shift, left, right);
        default: return checked_result<script_value>(make_error_code(runtime_error_code::unknown_operator), "Unknown binary operator");
    }
}

} // namespace jai
