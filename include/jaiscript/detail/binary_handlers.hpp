#pragma once

#ifndef __JAISCRIPT_DETAIL_BINARY_HANDLERS_HPP__
#define __JAISCRIPT_DETAIL_BINARY_HANDLERS_HPP__

// The general binary operators, run once a backend's fast lanes decline: one body per operator family, shared by both
// backends. Each entry normalizes a cpp-bound operand, a utf8_string operand and a char pair
// first, then answers numbers and strings, then (only when an operand holds an object) unwraps transparent wrappers,
// answers again and asks the class's operator method. The host supplies:
//   script_value make_int(script_int), make_float(script_float), make_bool(bool); engine* engine()
//   checked_result<script_value> concat(const script_value&, const script_value&)   // string +, memory-charged
//   bool joins_text   // a template string's '+': a scalar beside the string becomes text instead of the error
//   std::optional<checked_result<script_value>> object_arithmetic(l, r, uint64_t method_id)
//   bool operator_takes(l, r, uint64_t method_id)   // l's class chain has an overload of the operator that takes r
//   std::optional<checked_result<bool>> object_comparison(l, r, uint64_t method_id)
//   std::optional<checked_result<bool>> object_equality(l, r); bool defines_custom_equality(const script_value&)
//   std::optional<checked_result<bool>> object_inequality(l, r)   // a declared operator!=, ahead of !(l == r)
//   object_comparison_reversed(l, r, id), object_spaceship_reversed(l, r), object_spaceship_equality(l, r)   // <=> answers
//   std::optional<checked_result<script_value>> object_reflected(l, r, id)   // r's bound reflected operator taking l (C++'s op(L, const R&))
//   const class_definition* operand_class(const script_value&); uint64_t equality_id()   // an operand's class (null: no object)
//   bool unwinding(); bool parallel_worker(); size_t& equality_depth()   // memberwise ==/<=> levels nested natively
//   checked_result<script_value> equal(l, r)   // the backend's == entry
//   checked_result<script_value> spaceship_member(l, r, order_walk*)   // the backend's <=> on a member pair of a walk
// and the class-operator hooks listed above object_equality_via_method below.

#include <jaiscript/core/value.hpp>
#include <jaiscript/detail/structural_ordering.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/scalar_ops.hpp>
#include <jaiscript/detail/char_promotion.hpp>
#include <jaiscript/detail/string_join.hpp>
#include <jaiscript/detail/utf8_operands.hpp>
#include <jaiscript/detail/structural_equality.hpp>
#include <jaiscript/detail/math_values.hpp>
#include <jaiscript/core/overload_resolution.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/math_component_store.hpp>
#include <jaiscript/detail/operator_ids.hpp>
#include <jaiscript/detail/operator_table.hpp>
#include <jaiscript/detail/parallel_transform.hpp>
#include <cmath>
#include <compare>
#include <optional>

namespace jai::detail {

	static_assert(math::k_checked_overflow == kCheckedOverflow, "math_values.hpp restates the integer policy");

	enum class arith_op : uint8_t { add, sub, mul, div, mod };
	enum class order_op : uint8_t { less, less_equal, greater, greater_equal };
	enum class bit_op : uint8_t { bit_and, bit_or, bit_xor, shift_left, shift_right };

	inline bool numeric_storage(size_t index) noexcept {
		return index == script_value::TYPEID_INT || index == script_value::TYPEID_FLOAT;
	}
	inline bool object_storage(size_t index) noexcept {
		return index == script_value::TYPEID_OBJECT;
	}
	inline script_float as_script_float(const script_value& v, size_t index) noexcept {
		return index == script_value::TYPEID_INT ? script_float(v.unchecked_as_int()) : v.unchecked_as_float();
	}
	inline checked_result<script_value> type_mismatch_error(const char* text) {
		return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), text);
	}

	template <arith_op Op>
	constexpr const char* arith_invalid_text() {
		if constexpr (Op == arith_op::add) { return "Invalid operands for + operator"; }
		else if constexpr (Op == arith_op::sub) { return "Invalid operands for - operator"; }
		else if constexpr (Op == arith_op::mul) { return "Invalid operands for * operator"; }
		else if constexpr (Op == arith_op::div) { return "Invalid operands for / operator"; }
		else { return "Invalid operands for % operator"; }
	}
	template <arith_op Op>
	constexpr token_type arith_token() {
		if constexpr (Op == arith_op::add) { return token_type::plus; }
		else if constexpr (Op == arith_op::sub) { return token_type::minus; }
		else if constexpr (Op == arith_op::mul) { return token_type::star; }
		else if constexpr (Op == arith_op::div) { return token_type::slash; }
		else { return token_type::percent; }
	}

	// Numbers (and a string on either side of +); nullopt for anything else
	template <arith_op Op, class Host>
	inline std::optional<checked_result<script_value>> arithmetic_on_values(Host& h, const script_value& l, const script_value& r, size_t li, size_t ri) {
		constexpr token_type op = arith_token<Op>();
		if (li == script_value::TYPEID_INT && ri == script_value::TYPEID_INT) {
			script_int out;
			if (const auto fault = int_arith(op, l.unchecked_as_int(), r.unchecked_as_int(), out); fault != scalar_fault::none) {
				return scalar_fault_error<checked_result<script_value>>(op, fault);
			}
			return h.make_int(out);
		}
		if (numeric_storage(li) && numeric_storage(ri)) {
			script_float out;
			if (const auto fault = float_arith(op, as_script_float(l, li), as_script_float(r, ri), out); fault != scalar_fault::none) {
				return scalar_fault_error<checked_result<script_value>>(op, fault);
			}
			return h.make_float(out);
		}
		if constexpr (Op == arith_op::add) {
			if (li == script_value::TYPEID_STRING || ri == script_value::TYPEID_STRING) {
				if (!h.joins_text && string_beside_scalar(li, ri)) [[unlikely]] { return string_plus_refused(li, ri); }
				return h.concat(l, r);
			}
		}
		if (math::owns_arithmetic(li, ri)) [[unlikely]] { return math::binary(static_cast<math::bop>(Op), l, r, h.engine()); }
		return std::nullopt;
	}

	// a worker reads only the instance's class (no method-value copy); the operator call that follows raises the parallel verdict
	inline bool worker_operator_takes(const script_value& l, const script_value& r, uint64_t id) {
		const class_definition* cd = operand_instance_class(l);
		return cd && binary_operator_accepts(cd, id, r);
	}

	// No bound operator takes these operands: the text names the operation and the binding to add (a bound left operand's
	// operator taking the right one, else a bound right operand's reflected operator or <=>); nullopt when neither is bound
	template <class Host>
	JAI_NOINLINE std::optional<checked_result<script_value>> bound_operator_miss(Host& h, const script_value& l, const script_value& r, uint64_t id) {
		engine* eng = h.engine();
		string_symbolizer* symbols = eng ? engine_access::get_symbolizer(*eng) : nullptr;
		if (!symbols || symbols->frozen()) { return std::nullopt; }
		const std::string op(symbols->get_string(id));
		const class_definition* lc = h.operand_class(l);
		const std::string lt = slot_source_type_name(l.deref(), eng);
		const std::string rt = slot_source_type_name(r.deref(), eng);
		std::string fix;
		if (lc && host_operator_dispatch(lc->get_method(id, false))) {
			fix = "\"" + op + "\" taking " + rt + " on " + lt;
		} else if (const class_definition* rc = h.operand_class(r); rc && !rc->is_script_class() && !lc) {
			const bool relational = op == "<" || op == "<=" || op == ">" || op == ">=" || op == "<=>";
			fix = relational ? "\"<=>\" taking " + lt + " on " + rt : "\"" + op + "\" as (" + lt + ", " + rt + ") on " + rt;
		} else {
			return std::nullopt;
		}
		return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "No bound '{0}': bind operator {1}, or convert the operand",
		                                    symbols->intern(lt + " " + op + " " + rt), symbols->intern(fix));
	}

	template <arith_op Op>
	inline bool arithmetic_has_builtin(size_t li, size_t ri) noexcept {
		if (numeric_storage(li) && numeric_storage(ri)) { return true; }
		if constexpr (Op == arith_op::add) {
			if (li == script_value::TYPEID_STRING || ri == script_value::TYPEID_STRING) { return true; }
		}
		return math::owns_arithmetic(li, ri);
	}

	template <arith_op Op, class Host>
	checked_result<script_value> binary_arithmetic(Host& h, uint64_t method_id, const script_value& left, const script_value& right) {
		const size_t li = left.raw_storage_index();
		const size_t ri = right.raw_storage_index();
		// invariant 4: a bound operand decodes to a detached temp (re-entry depth is exactly 1)
		if (li == script_value::TYPEID_CPP_BOUND || ri == script_value::TYPEID_CPP_BOUND) [[unlikely]] {
			return binary_arithmetic<Op>(h, method_id, li == script_value::TYPEID_CPP_BOUND ? left.bound_decoded_temp() : left,
			                             ri == script_value::TYPEID_CPP_BOUND ? right.bound_decoded_temp() : right);
		}
		if (utf8_operands(li, ri)) [[unlikely]] {
			return utf8_rewrapped(binary_arithmetic<Op>(h, method_id, utf8_unwrapped(left, h.engine()), utf8_unwrapped(right, h.engine())), h.engine());
		}
		if (char_operands_promote(li, ri)) [[unlikely]] {
			return binary_arithmetic<Op>(h, method_id, char_promoted(left, h.engine()), char_promoted(right, h.engine()));
		}
		if (!object_storage(li) && !object_storage(ri)) {
			if (auto result = arithmetic_on_values<Op>(h, left, right, li, ri)) { return std::move(*result); }
			return type_mismatch_error(arith_invalid_text<Op>());
		}
		const auto wrapped_left = left.try_unwrap_transparent_wrapper(), wrapped_right = right.try_unwrap_transparent_wrapper();
		const script_value& unwrapped_left = wrapped_left ? *wrapped_left : left;
		const script_value& unwrapped_right = wrapped_right ? *wrapped_right : right;
		const size_t uli = unwrapped_left.raw_storage_index();
		const size_t uri = unwrapped_right.raw_storage_index();
		// overload resolution: the left operand's operator beats the builtin meaning when one of its overloads takes the right operand
		if (object_storage(li) && arithmetic_has_builtin<Op>(uli, uri) && h.operator_takes(left, right, method_id)) {
			if (auto custom = h.object_arithmetic(left, right, method_id)) { return std::move(*custom); }
		}
		if (!object_storage(li) && object_storage(ri) && arithmetic_has_builtin<Op>(uli, uri)) {
			if (auto reflected = h.object_reflected(left, right, method_id)) { return std::move(*reflected); }
		}
		if (auto result = arithmetic_on_values<Op>(h, unwrapped_left, unwrapped_right, uli, uri)) {
			return std::move(*result);
		}
		if (auto custom = h.object_arithmetic(left, right, method_id)) { return std::move(*custom); }
		if (h.unwinding()) { return h.make_int(0); }
		if (auto reflected = h.object_reflected(left, right, method_id)) { return std::move(*reflected); }
		if (auto miss = bound_operator_miss(h, left, right, method_id)) { return std::move(*miss); }
		return type_mismatch_error(arith_invalid_text<Op>());
	}

	template <order_op Op>
	constexpr const char* order_invalid_text() {
		if constexpr (Op == order_op::less) { return "Invalid operands for < operator"; }
		else if constexpr (Op == order_op::less_equal) { return "Invalid operands for <= operator"; }
		else if constexpr (Op == order_op::greater) { return "Invalid operands for > operator"; }
		else { return "Invalid operands for >= operator"; }
	}
	template <order_op Op, class T>
	constexpr bool ordered(const T& a, const T& b) {
		if constexpr (Op == order_op::less) { return a < b; }
		else if constexpr (Op == order_op::less_equal) { return a <= b; }
		else if constexpr (Op == order_op::greater) { return a > b; }
		else { return a >= b; }
	}

	template <order_op Op, class Host>
	inline std::optional<script_value> ordering_on_values(Host& h, const script_value& l, const script_value& r, size_t li, size_t ri) {
		if (li == script_value::TYPEID_INT && ri == script_value::TYPEID_INT) { return h.make_bool(ordered<Op>(l.unchecked_as_int(), r.unchecked_as_int())); }
		if (numeric_storage(li) && numeric_storage(ri)) { return h.make_bool(ordered<Op>(as_script_float(l, li), as_script_float(r, ri))); }
		if (li == script_value::TYPEID_STRING && ri == script_value::TYPEID_STRING) { return h.make_bool(ordered<Op>(l.unchecked_as_string(), r.unchecked_as_string())); }
		if (li == script_value::TYPEID_BOOL && ri == script_value::TYPEID_BOOL) { return h.make_bool(ordered<Op>(l.unchecked_as_bool(), r.unchecked_as_bool())); }
		return std::nullopt;
	}

	template <order_op Op, class Host>
	checked_result<script_value> binary_ordering(Host& h, uint64_t method_id, const script_value& left, const script_value& right) {
		const size_t li = left.raw_storage_index();
		const size_t ri = right.raw_storage_index();
		if (li == script_value::TYPEID_CPP_BOUND || ri == script_value::TYPEID_CPP_BOUND) [[unlikely]] {
			return binary_ordering<Op>(h, method_id, li == script_value::TYPEID_CPP_BOUND ? left.bound_decoded_temp() : left,
			                           ri == script_value::TYPEID_CPP_BOUND ? right.bound_decoded_temp() : right);
		}
		if (utf8_operands(li, ri)) [[unlikely]] {
			return utf8_rewrapped(binary_ordering<Op>(h, method_id, utf8_unwrapped(left, h.engine()), utf8_unwrapped(right, h.engine())), h.engine());
		}
		if (char_operand(li, ri)) [[unlikely]] {
			if (char_operands_promote(li, ri)) { return binary_ordering<Op>(h, method_id, char_promoted(left, h.engine()), char_promoted(right, h.engine())); }
			if (char_beside_string(li, ri)) { return binary_ordering<Op>(h, method_id, char_as_text(left, h.engine()), char_as_text(right, h.engine())); }
		}
		if (auto fast = ordering_on_values<Op>(h, left, right, li, ri)) { return std::move(*fast); }
		if (auto builtin = builtin_order(h, left, right, nullptr)) {
			if (!*builtin) { return builtin->error_value(); }
			return h.make_bool(ordered<Op>(builtin->value().unchecked_as_int(), script_int{0}));
		}
		if (!object_storage(li) && !object_storage(ri)) {
			if (auto result = ordering_on_values<Op>(h, left, right, li, ri)) { return std::move(*result); }
			if (math::owns_comparison(li, ri)) [[unlikely]] { return math::ordering_error(left, right); }
			return type_mismatch_error(order_invalid_text<Op>());
		}
		const auto wrapped_left = left.try_unwrap_transparent_wrapper(), wrapped_right = right.try_unwrap_transparent_wrapper();
		const script_value& unwrapped_left = wrapped_left ? *wrapped_left : left;
		const script_value& unwrapped_right = wrapped_right ? *wrapped_right : right;
		if (auto result = ordering_on_values<Op>(h, unwrapped_left, unwrapped_right, unwrapped_left.raw_storage_index(), unwrapped_right.raw_storage_index())) {
			return std::move(*result);
		}
		if (auto custom = h.object_comparison(left, right, method_id)) {
			if (!*custom) { return custom->error_value(); }
			return h.make_bool(custom->value());
		}
		// a script throw inside the operator method is in flight: it leaves, not the invalid-operands error
		if (h.unwinding()) { return h.make_bool(false); }
		if (auto reflected = h.object_reflected(left, right, method_id)) {
			if (!*reflected) { return reflected->error_value(); }
			const script_value& answer = reflected->value().deref();
			return h.make_bool(answer.is_bool() ? answer.unchecked_as_bool() : true);
		}
		if (h.unwinding()) { return h.make_bool(false); }
		if (auto reversed = h.object_comparison_reversed(left, right, method_id)) {
			if (!*reversed) { return reversed->error_value(); }
			return h.make_bool(reversed->value());
		}
		if (h.unwinding()) { return h.make_bool(false); }
		if (auto miss = bound_operator_miss(h, left, right, method_id)) { return std::move(*miss); }
		return type_mismatch_error(order_invalid_text<Op>());
	}

	// weak_ptr against null: equal when the handle expired (a default-constructed one included)
	inline std::optional<bool> weak_equals_null(const script_value& left, const script_value& right) {
		if (!((left.is_weak_ptr() && right.is_null()) || (left.is_null() && right.is_weak_ptr()))) { return std::nullopt; }
		return (left.is_weak_ptr() ? left : right).get_weak_ptr().expired();
	}

	// a == b through an operator==: the left operand's, then the right operand's bound reflected one, then the right operand's
	// own with the operands reversed (C++20); each only when one of its overloads takes the other operand
	template <class Host>
	inline std::optional<checked_result<bool>> equality_candidates(Host& h, const script_value& left, const script_value& right) {
		if (auto custom = h.object_equality(left, right)) { return custom; }
		if (h.unwinding()) { return checked_result<bool>(false); }
		if (auto reflected = h.object_reflected(left, right, h.equality_id())) {
			if (!*reflected) { return checked_result<bool>(reflected->error_value()); }
			const script_value& answer = reflected->value().deref();
			return checked_result<bool>(answer.is_bool() ? answer.unchecked_as_bool() : true);
		}
		if (h.unwinding()) { return checked_result<bool>(false); }
		if (h.operand_class(left) == h.operand_class(right)) { return std::nullopt; }
		return h.object_equality(right, left);
	}

	template <class Host>
	checked_result<script_value> equal_operands(Host& h, const script_value& left, const script_value& right, equality_walk* walk);

	// A worker's refusal of the operand's operator (the first of ids its class declares): its violation, else the verdict
	template <class T, class Host>
	inline checked_result<T> parallel_operator_refused(const Host& h, const script_value& operand, std::initializer_list<uint64_t> ids) {
		return checked_result<T>(make_error_code(runtime_error_code::unsupported_operation), h.worker_operator_refusal(operand, ids));
	}

	// == with a parallel worker's borrow of a shared object: two handles are one object when they are one wrap, as on the serial
	// path; a value compares as the worker's own copy of it (parallel_by_value), whose class's == runs through its pins
	template <class Host>
	checked_result<script_value> shared_object_equal(Host& h, const script_value& left, const script_value& right, equality_walk* walk) {
		const script_value& l = left.deref();
		const script_value& r = right.deref();
		const object_borrow_record* lb = l.parallel_object_borrow();
		const object_borrow_record* rb = r.parallel_object_borrow();
		if (l.is_shared_ptr_type() || r.is_shared_ptr_type()) {
			if (h.defines_custom_equality(l) || h.defines_custom_equality(r)) {
				return parallel_operator_refused<script_value>(h, h.defines_custom_equality(l) ? l : r, { h.ids().equal_equal, h.ids().spaceship });
			}
			return h.make_bool(lb && rb && lb == rb);
		}
		const script_value lc = lb ? parallel_by_value(l) : l;
		const script_value rc = rb ? parallel_by_value(r) : r;
		return equal_operands(h, lc, rc, walk);
	}

	// == : memberwise on heavy values (detail/structural_equality.hpp), identity for handles and host objects. A bound
	// operand is not decoded: an opaque host pointer compares by identity. walk is the memberwise comparison this pair is a
	// member of (null for an operator's own operands).
	template <class Host>
	checked_result<script_value> equal_operands(Host& h, const script_value& left, const script_value& right, equality_walk* walk) {
		if (utf8_operands(left.raw_storage_index(), right.raw_storage_index())) [[unlikely]] {
			return utf8_rewrapped(equal_operands(h, utf8_unwrapped(left, h.engine()), utf8_unwrapped(right, h.engine()), walk), h.engine());
		}
		if (const size_t li = left.raw_storage_index(), ri = right.raw_storage_index(); char_operand(li, ri)) [[unlikely]] {
			if (char_operands_promote(li, ri)) { return equal_operands(h, char_promoted(left, h.engine()), char_promoted(right, h.engine()), walk); }
			if (char_beside_string(li, ri)) { return equal_operands(h, char_as_text(left, h.engine()), char_as_text(right, h.engine()), walk); }
		}
		if (auto expired = weak_equals_null(left, right)) { return h.make_bool(*expired); }
		if (auto identity = builtin_identity_equal(left, right)) { return h.make_bool(*identity); }

		const auto wrapped_left = left.try_unwrap_transparent_wrapper(), wrapped_right = right.try_unwrap_transparent_wrapper();
		const script_value& unwrapped_left = wrapped_left ? *wrapped_left : left;
		const script_value& unwrapped_right = wrapped_right ? *wrapped_right : right;
		if ((unwrapped_left.is_int() || unwrapped_left.is_float()) && (unwrapped_right.is_int() || unwrapped_right.is_float())) {
			// two ints compare exactly: a double loses precision beyond 2^53
			if (unwrapped_left.is_int() && unwrapped_right.is_int()) { return h.make_bool(unwrapped_left.unchecked_as_int() == unwrapped_right.unchecked_as_int()); }
			const script_float lf = unwrapped_left.is_int() ? script_float(unwrapped_left.unchecked_as_int()) : unwrapped_left.unchecked_as_float();
			const script_float rf = unwrapped_right.is_int() ? script_float(unwrapped_right.unchecked_as_int()) : unwrapped_right.unchecked_as_float();
			return h.make_bool(lf == rf);
		}
		// null-ness is a storage property: a typed-but-uninitialized slot compares like null
		if (unwrapped_left.is_null() || unwrapped_right.is_null()) { return h.make_bool(unwrapped_left.is_null() && unwrapped_right.is_null()); }
		if (math::owns_comparison(left.raw_storage_index(), right.raw_storage_index())) [[unlikely]] { return h.make_bool(math::equal(left, right)); }
		if (unwrapped_left.type() != unwrapped_right.type()) {
			if (unwrapped_left.is_object() || unwrapped_right.is_object()) {   // a class's ==, or its <=>, may take the other kind (either side, C++20)
				if (auto custom = equality_candidates(h, left, right)) {
					if (!*custom) { return custom->error_value(); }
					return h.make_bool(custom->value());
				}
				if (auto mixed = h.object_spaceship_equality(left, right)) {
					if (!*mixed) { return mixed->error_value(); }
					return h.make_bool(mixed->value());
				}
			}
			return h.make_bool(false);
		}
		if (unwrapped_left.is_string() && unwrapped_right.is_string()) { return h.make_bool(unwrapped_left.unchecked_as_string() == unwrapped_right.unchecked_as_string()); }
		if (unwrapped_left.is_bool() && unwrapped_right.is_bool()) { return h.make_bool(unwrapped_left.unchecked_as_bool() == unwrapped_right.unchecked_as_bool()); }
		// opaque host pointers (an unregistered make_value(T*)): the same live pointer
		if (unwrapped_left.is_cpp_bound_primitive() && unwrapped_right.is_cpp_bound_primitive()) {
			return h.make_bool(unwrapped_left.get_cpp_bound_ptr() == unwrapped_right.get_cpp_bound_ptr());
		}

		const bool native = !walk && h.equality_depth() < k_inline_compare_depth;
		if (left.is_array() && right.is_array()) {
			if (native) { return arrays_equal(h, left, right); }
			return compare_members(h, walk, [&](equality_walk& w) { return open_arrays_equal(left, right, w); });
		}
		if (left.is_map() && right.is_map()) {
			if (native) { return maps_equal(h, left, right); }
			return compare_members(h, walk, [&](equality_walk& w) { return open_maps_equal(left, right, w); });
		}

		if (h.parallel_worker()) [[unlikely]] {
			if (left.deref().parallel_object_borrow() || right.deref().parallel_object_borrow()) { return shared_object_equal(h, left, right, walk); }
			// safe-mode workers: the operator wall would skip a custom operator== and the structural answer would diverge
			// from the serial loop
			if (h.worker_equality_refused(left) || h.worker_equality_refused(right)) {
				return parallel_operator_refused<script_value>(h, h.worker_equality_refused(left) ? left : right, { h.ids().equal_equal, h.ids().spaceship });
			}
		}
		if (auto custom = equality_candidates(h, left, right)) {
			if (!*custom) { return custom->error_value(); }
			return h.make_bool(custom->value());
		}
		if (auto from_spaceship = h.object_spaceship_equality(left, right)) {
			if (!*from_spaceship) { return from_spaceship->error_value(); }
			return h.make_bool(from_spaceship->value());
		}
		// script class instances: same class, fieldwise
		if (auto instances = script_instance_pair(left, right)) {
			if (native) { return instances_equal(h, left, right, instances->first, instances->second); }
			return compare_members(h, walk, [&](equality_walk& w) { return open_instances_equal(left, right, instances->first, instances->second, w); });
		}
		// host objects and other holders: the same object_holder
		auto left_holder = const_cast<script_value&>(left).get_object_holder();
		auto right_holder = const_cast<script_value&>(right).get_object_holder();
		if (left_holder && right_holder) { return h.make_bool(left_holder == right_holder); }
		return h.make_bool(false);
	}

	template <class Host>
	checked_result<script_value> binary_equal(Host& h, const script_value& left, const script_value& right) {
		return equal_operands(h, left, right, nullptr);
	}

	template <class Host>
	checked_result<script_value> binary_not_equal(Host& h, const script_value& left, const script_value& right) {
		if (auto expired = weak_equals_null(left, right)) { return h.make_bool(!*expired); }
		if (auto custom = h.object_inequality(left, right)) {
			if (!*custom) { return custom->error_value(); }
			return h.make_bool(custom->value());
		}
		auto equal = h.equal(left, right);
		if (!equal) [[unlikely]] { return equal.error_value(); }
		return h.make_bool(!equal.value().unchecked_as_bool());
	}

	// <=>; walk is the lexicographic comparison this pair is a member of (null for an operator's own operands)
	template <class Host>
	checked_result<script_value> spaceship_operands(Host& h, uint64_t method_id, const script_value& left, const script_value& right, order_walk* walk) {
		if (left.raw_storage_index() == script_value::TYPEID_CPP_BOUND || right.raw_storage_index() == script_value::TYPEID_CPP_BOUND) {
			return spaceship_operands(h, method_id, left.raw_storage_index() == script_value::TYPEID_CPP_BOUND ? left.bound_decoded_temp() : left, right.raw_storage_index() == script_value::TYPEID_CPP_BOUND ? right.bound_decoded_temp() : right, walk);
		}
		if (utf8_operands(left.raw_storage_index(), right.raw_storage_index())) [[unlikely]] {
			return utf8_rewrapped(spaceship_operands(h, method_id, utf8_unwrapped(left, h.engine()), utf8_unwrapped(right, h.engine()), walk), h.engine());
		}
		if (const size_t li = left.raw_storage_index(), ri = right.raw_storage_index(); char_operand(li, ri)) [[unlikely]] {
			if (char_operands_promote(li, ri)) { return spaceship_operands(h, method_id, char_promoted(left, h.engine()), char_promoted(right, h.engine()), walk); }
			if (char_beside_string(li, ri)) { return spaceship_operands(h, method_id, char_as_text(left, h.engine()), char_as_text(right, h.engine()), walk); }
		}
		auto sign = [&h](auto cmp) { return h.make_int(cmp < 0 ? script_int(-1) : (cmp > 0 ? script_int(1) : script_int(0))); };
		if (left.raw_storage_index() == script_value::TYPEID_INT && right.raw_storage_index() == script_value::TYPEID_INT) {
			return sign(left.unchecked_as_int() <=> right.unchecked_as_int());
		}
		if ((left.is_int() || left.is_float()) && (right.is_int() || right.is_float())) {
			const script_float lf = left.is_int() ? script_float(left.unchecked_as_int()) : left.unchecked_as_float();
			const script_float rf = right.is_int() ? script_float(right.unchecked_as_int()) : right.unchecked_as_float();
			return h.make_int(float_order(lf, rf));
		}
		if (left.is_string() && right.is_string()) { return sign(left.unchecked_as_string().compare(right.unchecked_as_string())); }
		if (auto builtin = builtin_order(h, left, right, walk)) { return std::move(*builtin); }
		if (math::owns_comparison(left.raw_storage_index(), right.raw_storage_index())) [[unlikely]] { return math::ordering_error(left, right); }
		if (auto custom = h.object_arithmetic(left, right, method_id)) { return std::move(*custom); }
		if (h.unwinding()) { return h.make_int(0); }
		if (auto reflected = h.object_reflected(left, right, method_id)) { return std::move(*reflected); }
		if (h.unwinding()) { return h.make_int(0); }
		if (auto reversed = h.object_spaceship_reversed(left, right)) { return std::move(*reversed); }
		if (h.unwinding()) { return h.make_int(0); }
		if (auto miss = bound_operator_miss(h, left, right, method_id)) { return std::move(*miss); }
		return type_mismatch_error("Invalid operands for <=> operator");
	}

	template <class Host>
	checked_result<script_value> binary_spaceship(Host& h, uint64_t method_id, const script_value& left, const script_value& right) {
		return spaceship_operands(h, method_id, left, right, nullptr);
	}

	template <bit_op Op>
	constexpr const char* bit_invalid_text() {
		if constexpr (Op == bit_op::bit_and) { return "Bitwise & requires integer operands"; }
		else if constexpr (Op == bit_op::bit_or) { return "Bitwise | requires integer operands"; }
		else if constexpr (Op == bit_op::bit_xor) { return "Bitwise ^ requires integer operands"; }
		else if constexpr (Op == bit_op::shift_left) { return "Left shift requires integer operands"; }
		else { return "Right shift requires integer operands"; }
	}

	// Shifts wrap mod 2^64 by contract (docs/site/guide/04-operators.html); the amount must be 0..63
	template <bit_op Op, class Host>
	checked_result<script_value> binary_bitwise(Host& h, uint64_t method_id, const script_value& left, const script_value& right) {
		if (utf8_operands(left.raw_storage_index(), right.raw_storage_index())) [[unlikely]] {
			return utf8_rewrapped(binary_bitwise<Op>(h, method_id, utf8_unwrapped(left, h.engine()), utf8_unwrapped(right, h.engine())), h.engine());
		}
		if (char_operands_promote(left.raw_storage_index(), right.raw_storage_index())) [[unlikely]] {
			return binary_bitwise<Op>(h, method_id, char_promoted(left, h.engine()), char_promoted(right, h.engine()));
		}
		if (!left.is_int() || !right.is_int()) {
			if (auto custom = h.object_arithmetic(left, right, method_id)) { return std::move(*custom); }
			if (h.unwinding()) { return h.make_int(0); }
			if (auto reflected = h.object_reflected(left, right, method_id)) { return std::move(*reflected); }
			if (auto miss = bound_operator_miss(h, left, right, method_id)) { return std::move(*miss); }
			return type_mismatch_error(bit_invalid_text<Op>());
		}
		const script_int a = left.unchecked_as_int(), b = right.unchecked_as_int();
		if constexpr (Op == bit_op::bit_and) { return h.make_int(a & b); }
		else if constexpr (Op == bit_op::bit_or) { return h.make_int(a | b); }
		else if constexpr (Op == bit_op::bit_xor) { return h.make_int(a ^ b); }
		else {
			if (b < 0 || b > 63) { return type_mismatch_error("Shift amount must be between 0 and 63"); }
			if constexpr (Op == bit_op::shift_left) { return h.make_int(a << b); }
			else { return h.make_int(a >> b); }
		}
	}

	// ---- A class operand's operator methods, one body for both backends. Beyond the hooks above the host supplies
	//   member_target resolve_member_target(v); script_value operator_value(target, id)   // a worker reads only its pins
	//   checked_result<script_value> operator_call(self, method, id, std::vector<script_value> args)
	//   bool worker_operators_pinned(v, {ids}); const parallel_method_pin* worker_operator_pin(v, id)
	//   std::string_view worker_operator_refusal(v, {ids})   // what a worker raises for v's unpinned operator
	//   bool comparison_free(v)   // v's class answers none of ==, != and <=> (a memo; false where the host keeps none)
	//   const operator_ids& ids(); script_value make_null(); script_value bound_method(self, method); execution_backend& backend()

	inline token_type comparison_token(const operator_ids& ids, uint64_t id) noexcept {
		return id == ids.less ? token_type::less : id == ids.less_equal ? token_type::less_equal : id == ids.greater ? token_type::greater : token_type::greater_equal;
	}

	// a == b through the left operand's operator== (a transparent wrapper's, unwrapped); nullopt when none takes b
	template <class Host>
	std::optional<checked_result<bool>> object_equality_via_method(Host& h, const script_value& left, const script_value& right) {
		const operator_ids& ids = h.ids();
		// a worker whose region did not pin the class's == leaves it to the == entry, which raises the verdict
		if (h.parallel_worker() && !h.worker_operators_pinned(left, { ids.equal_equal, ids.spaceship })) [[unlikely]] { return std::nullopt; }
		if (h.comparison_free(left)) { return std::nullopt; }
		auto target = h.resolve_member_target(left);
		if (!target) { return std::nullopt; }
		script_value method_val = h.operator_value(target, ids.equal_equal);
		if (!method_val.is_function()) {
			if (target.class_def && target.class_def->is_transparent_wrapper()) {
				script_value mutable_left = left;
				script_value unwrapped = target.class_def->unwrap(mutable_left);
				if (!unwrapped.is_null()) { return object_equality_via_method(h, unwrapped, right); }
			}
			return std::nullopt;
		}
		if (!equality_operator_candidate(target.class_def, ids.equal_equal, ids.spaceship, right)) { return std::nullopt; }
		auto result = h.operator_call(left, method_val, ids.equal_equal, { right });
		// the method's runtime error is the comparison's (a script throw in flight keeps travelling on its own)
		if (!result && !h.unwinding()) { return checked_result<bool>(result.error_value()); }
		if (result.has_value() && result.value().is_bool()) { return checked_result<bool>(result.value().unchecked_as_bool()); }
		return std::nullopt;
	}

	// A declared operator!= wins over !(a == b) (C++20), asked where operator== would be: a class instance against a value of its
	// class, or any value one of its != overloads takes when the chain declares <=>
	template <class Host>
	std::optional<checked_result<bool>> object_inequality_via_method(Host& h, const script_value& left, const script_value& right) {
		const operator_ids& ids = h.ids();
		if (h.parallel_worker() && !h.worker_operators_pinned(left, { ids.bang_equal })) [[unlikely]] {
			if (!right.is_null()) { return parallel_operator_refused<bool>(h, left, { ids.bang_equal }); }
			return std::nullopt;
		}
		if (!left.is_object() || right.is_null() || h.comparison_free(left)) { return std::nullopt; }
		auto target = h.resolve_member_target(left);
		if (!target) { return std::nullopt; }
		script_value method_val = h.operator_value(target, ids.bang_equal);
		if (!method_val.is_function() || !equality_operator_candidate(target.class_def, ids.bang_equal, ids.spaceship, right)) { return std::nullopt; }
		auto result = h.operator_call(left, method_val, ids.bang_equal, { right });
		if (!result) {
			if (h.unwinding()) { return checked_result<bool>(false); }
			return checked_result<bool>(result.error_value());
		}
		if (result.value().is_bool()) { return checked_result<bool>(result.value().unchecked_as_bool()); }
		return std::nullopt;
	}

	// The right operand's <=> where the left operand has no operator: 5 < v is (v <=> 5) > 0, a candidate only when its parameter
	// takes the left operand; the call's answer, or nullopt
	template <class Host>
	std::optional<checked_result<script_value>> reversed_spaceship_call(Host& h, const script_value& left, const script_value& right) {
		const uint64_t id = h.ids().spaceship;
		auto target = h.resolve_member_target(right);
		if (!target || !binary_operator_accepts(target.class_def, id, left)) { return std::nullopt; }
		script_value method_val = h.operator_value(target, id);
		if (!method_val.is_function()) { return std::nullopt; }
		return h.operator_call(right, method_val, id, { left });
	}

	template <class Host>
	std::optional<checked_result<bool>> object_comparison_reversed_via_method(Host& h, const script_value& left, const script_value& right, uint64_t op_symbol_id) {
		if (h.parallel_worker() && !h.worker_operators_pinned(right, { h.ids().spaceship })) [[unlikely]] { return parallel_operator_refused<bool>(h, right, { h.ids().spaceship }); }
		auto result = reversed_spaceship_call(h, left, right);
		if (!result) { return std::nullopt; }
		if (!*result) {
			if (h.unwinding()) { return std::nullopt; }
			return checked_result<bool>(result->error_value());
		}
		if (auto answer = spaceship_rewritten(reversed_comparison(comparison_token(h.ids(), op_symbol_id)), result->value())) { return checked_result<bool>(*answer); }
		return std::nullopt;
	}

	template <class Host>
	std::optional<checked_result<script_value>> object_spaceship_reversed_via_method(Host& h, const script_value& left, const script_value& right) {
		if (h.parallel_worker() && !h.worker_operators_pinned(right, { h.ids().spaceship })) [[unlikely]] { return parallel_operator_refused<script_value>(h, right, { h.ids().spaceship }); }
		auto result = reversed_spaceship_call(h, left, right);
		if (!result) { return std::nullopt; }
		if (!*result) {
			if (h.unwinding()) { return std::nullopt; }
			return checked_result<script_value>(result->error_value());
		}
		if (auto sign = reversed_spaceship_sign(result->value())) { return checked_result<script_value>(h.make_int(*sign)); }
		return std::nullopt;
	}

	// <=> drives ==: an operand whose chain declares <=> answers a == b, the left operand first, then the right one: its operator==
	// when an overload takes the other operand, else its <=> as (a <=> b) == 0 when one does; any other operand, a class without
	// <=> included, keeps its own == (memberwise between values of one class, false otherwise)
	template <class Host>
	std::optional<checked_result<bool>> object_spaceship_equality_via_method(Host& h, const script_value& left, const script_value& right) {
		if (left.is_null() || right.is_null()) { return std::nullopt; }
		const operator_ids& ids = h.ids();
		const auto call = [&h, &ids](const script_value& self, const script_value& method_val, const script_value& arg, bool spaceship) -> std::optional<checked_result<bool>> {
			auto result = h.operator_call(self, method_val, spaceship ? ids.spaceship : ids.equal_equal, { arg });
			if (!result) {
				if (h.unwinding()) { return checked_result<bool>(false); }
				return checked_result<bool>(result.error_value());
			}
			const script_value& r = result.value().deref();
			if (!spaceship) { return r.is_bool() ? std::optional<checked_result<bool>>(checked_result<bool>(r.unchecked_as_bool())) : std::nullopt; }
			if (r.is_int()) { return checked_result<bool>(r.as_int() == 0); }
			if (r.is_float()) { return checked_result<bool>(r.as_float() == 0.0); }
			return std::nullopt;
		};
		const auto side = [&](const script_value& self, const script_value& other) -> std::optional<checked_result<bool>> {
			if (!self.is_object() || h.comparison_free(self)) { return std::nullopt; }
			auto target = h.resolve_member_target(self);
			if (!target) { return std::nullopt; }
			if (h.parallel_worker() && !h.worker_operators_pinned(self, { ids.spaceship, ids.equal_equal })) [[unlikely]] {
				if (!instance_declares_operator(self, ids.spaceship)) { return std::nullopt; }
				return parallel_operator_refused<bool>(h, self, { ids.spaceship, ids.equal_equal });
			}
			script_value spaceship = h.operator_value(target, ids.spaceship);
			if (!spaceship.is_function()) { return std::nullopt; }
			if (script_value eq = h.operator_value(target, ids.equal_equal); eq.is_function() && binary_operator_accepts(target.class_def, ids.equal_equal, other)) {
				return call(self, eq, other, false);
			}
			if (binary_operator_accepts(target.class_def, ids.spaceship, other)) { return call(self, spaceship, other, true); }
			return std::nullopt;
		};
		if (auto answer = side(left, right)) { return answer; }
		return side(right, left);
	}

	// <, <=, >, >= through the left operand's operator, else its <=> (C++20: an explicit operator no overload of which takes the
	// operand is not viable), else a transparent wrapper's
	template <class Host>
	std::optional<checked_result<bool>> object_comparison_via_method(Host& h, const script_value& left, const script_value& right, uint64_t op_symbol_id) {
		const operator_ids& ids = h.ids();
		if (h.parallel_worker() && !h.worker_operators_pinned(left, { op_symbol_id, ids.spaceship })) [[unlikely]] {
			if (instance_declares_operator(left, op_symbol_id)) { return parallel_operator_refused<bool>(h, left, { op_symbol_id, ids.spaceship }); }
			return std::nullopt;
		}
		auto target = h.resolve_member_target(left);
		if (!target) { return std::nullopt; }
		script_value method_val = h.operator_value(target, op_symbol_id);
		const bool declared = method_val.is_function();
		const bool host_refuses = declared && host_operator_dispatch(method_val) && !host_operator_accepts(method_val, right);
		if (!declared || host_refuses || (h.operator_value(target, ids.spaceship).is_function() && !binary_operator_accepts(target.class_def, op_symbol_id, right))) {
			if (script_value spaceship = h.operator_value(target, ids.spaceship); spaceship.is_function() && binary_operator_accepts(target.class_def, ids.spaceship, right)) {
				auto result = h.operator_call(left, spaceship, ids.spaceship, { right });
				if (!result) {
					if (h.unwinding()) { return std::nullopt; }
					return checked_result<bool>(result.error_value());
				}
				if (auto answer = spaceship_rewritten(comparison_token(ids, op_symbol_id), result.value())) { return checked_result<bool>(*answer); }
				return std::nullopt;
			}
			if (target.class_def && target.class_def->is_transparent_wrapper()) {
				script_value mutable_left = left;
				script_value unwrapped = target.class_def->unwrap(mutable_left);
				if (!unwrapped.is_null()) { return object_comparison_via_method(h, unwrapped, right, op_symbol_id); }
			}
			if (!declared || host_refuses) { return std::nullopt; }
		}
		auto result = h.operator_call(left, method_val, op_symbol_id, { right });
		if (!result && !h.unwinding()) { return checked_result<bool>(result.error_value()); }
		if (result.has_value() && result.value().is_bool()) { return checked_result<bool>(result.value().unchecked_as_bool()); }
		return std::nullopt;
	}

	// +, -, *, /, %, the bitwise operators and <=> through the left operand's operator method (a transparent wrapper's, unwrapped)
	template <class Host>
	std::optional<checked_result<script_value>> object_arithmetic_via_method(Host& h, const script_value& left, const script_value& right, uint64_t op_symbol_id) {
		// a worker dispatches a class operator through its region pin; one the class declares that the region could not pin raises the
		// verdict, never the builtin meaning
		if (h.parallel_worker() && !h.worker_operators_pinned(left, { op_symbol_id })) [[unlikely]] { return parallel_operator_refused<script_value>(h, left, { op_symbol_id }); }
		auto target = h.resolve_member_target(left);
		if (!target) { return std::nullopt; }
		script_value method_val = h.operator_value(target, op_symbol_id);
		if (!method_val.is_function()) {
			if (target.class_def && target.class_def->is_transparent_wrapper()) {
				script_value mutable_left = left;
				script_value unwrapped = target.class_def->unwrap(mutable_left);
				if (!unwrapped.is_null()) { return object_arithmetic_via_method(h, unwrapped, right, op_symbol_id); }
			}
			return std::nullopt;
		}
		if (host_operator_dispatch(method_val) && !host_operator_accepts(method_val, right)) { return std::nullopt; }
		auto result = h.operator_call(left, method_val, op_symbol_id, { right });
		if (!result && !h.unwinding()) { return checked_result<script_value>(result.error_value()); }
		if (result.has_value()) { return checked_result<script_value>(std::move(result.value())); }
		return std::nullopt;
	}

	// The right operand's bound reflected operator taking the left one (C++'s free op(L, const R&)), for a left operand whose own
	// operator does not answer
	template <class Host>
	std::optional<checked_result<script_value>> object_reflected_via_method(Host& h, const script_value& left, const script_value& right, uint64_t op_symbol_id) {
		if (!right.deref().is_object()) { return std::nullopt; }
		const uint64_t reflected_id = engine_access::reflected_operator_id(*h.engine(), op_symbol_id);
		if (!reflected_id) { return std::nullopt; }
		if (h.parallel_worker() && !h.worker_operators_pinned(right, { reflected_id })) [[unlikely]] { return parallel_operator_refused<script_value>(h, right, { reflected_id }); }
		auto target = h.resolve_member_target(right);
		if (!target) { return std::nullopt; }
		script_value method_val = h.operator_value(target, reflected_id);
		if (!method_val.is_function() || !host_operator_accepts(method_val, left)) { return std::nullopt; }
		auto result = h.operator_call(right, method_val, reflected_id, { left });
		if (!result) {
			if (h.unwinding()) { return checked_result<script_value>(h.make_null()); }
			return checked_result<script_value>(result.error_value());
		}
		return checked_result<script_value>(std::move(result.value()));
	}

	// a op= b, ++a, a++, --a and a-- on an object whose class binds that operator, as C++ calls operator+=, operator++() and
	// operator++(int): it runs on the object once, in place, and the expression is what it returns, never stored back. base_id is
	// the binary operator's id (+ for ++). nullopt when no overload takes the operation: a op= b is then a = a op b, ++ and -- raise
	template <class Host>
	std::optional<checked_result<script_value>> object_operator_in_place(Host& h, const script_value& target, uint64_t base_id, const script_value& right,
	                                                                     incdec_form form) {
		if (target.deref().raw_storage_index() != script_value::TYPEID_OBJECT) { return std::nullopt; }
		const operator_ids& ids = h.ids();
		const uint64_t id = form == incdec_form::none ? engine_access::compound_operator_id(*h.engine(), base_id)
		                    : base_id == ids.plus ? ids.plus_plus : ids.minus_minus;
		if (!id) { return std::nullopt; }
		if (h.parallel_worker() && !h.worker_operators_pinned(target, { id })) [[unlikely]] { return parallel_operator_refused<script_value>(h, target, { id }); }
		auto owner = h.resolve_member_target(target);
		if (!owner) { return std::nullopt; }
		script_value method_val = h.operator_value(owner, id);
		if (!method_val.is_function()) { return std::nullopt; }
		std::vector<script_value> args;
		if (form == incdec_form::prefix) {
			const auto* dispatch = host_operator_dispatch(method_val);
			if (dispatch && dispatch->overloads->find(0) == dispatch->overloads->end()) { return std::nullopt; }
		} else {
			args.push_back(form == incdec_form::postfix ? h.make_int(0) : right);
			if (!host_operator_accepts(method_val, args.back())) { return std::nullopt; }
		}
		auto result = h.operator_call(target, method_val, id, std::move(args));
		if (!result) {
			if (h.unwinding()) { return checked_result<script_value>(h.make_null()); }
			return checked_result<script_value>(result.error_value());
		}
		return checked_result<script_value>(std::move(result.value()));
	}

	// A compound whose target or operand is an object (an element's, a reference's, a map value's), past object_operator_in_place:
	// the target class's binary operator, else the operand's reflected one; nullopt keeps the compound's text
	template <class Host>
	std::optional<checked_result<script_value>> object_compound_operand(Host& h, const script_value& left, token_type op, const script_value& right) {
		const operator_ids& ids = h.ids();
		uint64_t id = 0;
		switch (op) {
			case token_type::plus: id = ids.plus; break;
			case token_type::minus: id = ids.minus; break;
			case token_type::star: id = ids.star; break;
			case token_type::slash: id = ids.slash; break;
			case token_type::percent: id = ids.percent; break;
			case token_type::ampersand: id = ids.ampersand; break;
			case token_type::pipe: id = ids.pipe; break;
			case token_type::caret: id = ids.caret; break;
			case token_type::left_shift: id = ids.left_shift; break;
			case token_type::right_shift: id = ids.right_shift; break;
			default: return std::nullopt;
		}
		if (left.is_object()) {
			if (auto custom = object_arithmetic_via_method(h, left, right, id)) { return custom; }
		}
		if (right.is_object()) { return object_reflected_via_method(h, left, right, id); }
		return std::nullopt;
	}

	// A class's unary operator: the chain's overload taking no argument, called on the operand; nullopt when the class declares none
	// (the builtin meaning and text follow)
	template <class Host>
	std::optional<checked_result<script_value>> object_unary_via_method(Host& h, const script_value& operand, token_type op) {
		const operator_ids& ids = h.ids();
		const uint64_t id = op == token_type::minus ? ids.minus : op == token_type::plus ? ids.plus : ids.tilde;
		if (h.parallel_worker() && !h.worker_operators_pinned(operand, { id })) [[unlikely]] {
			if (declares_unary_operator(operand_instance_class(operand), id)) { return parallel_operator_refused<script_value>(h, operand, { id }); }
			return std::nullopt;
		}
		auto target = h.resolve_member_target(operand);
		if (!target || !declares_unary_operator(target.class_def, id)) { return std::nullopt; }
		script_value method_val = h.operator_value(target, id);
		if (!method_val.is_function()) { return std::nullopt; }
		auto result = h.operator_call(operand, method_val, id, {});
		if (!result) {
			if (h.unwinding()) { return checked_result<script_value>(h.make_null()); }
			return checked_result<script_value>(result.error_value());
		}
		return checked_result<script_value>(std::move(result.value()));
	}

	// A callable object: only a call that would raise not callable asks for it, so ordinary calls never pay for it
	template <class Host>
	script_value call_operator_of(Host& h, const script_value& callee) {
		const script_value& v = callee.deref();
		if (v.type() != script_value_type::jai_object_type && v.type() != script_value_type::jai_shared_ptr_type) { return h.make_null(); }
		const uint64_t id = h.ids().call;
		if (h.parallel_worker()) [[unlikely]] {
			// a worker calls the region's pin of operator() on this backend (the class's own method value is shared by every worker)
			const parallel_method_pin* pin = h.worker_operator_pin(v, id);
			if (!pin) { return h.make_null(); }
			return script_value::make_function(script_function([backend = &h.backend(), pin, receiver = v, id, eng = h.engine()](const std::vector<script_value>& args) {
				return call_parallel_operator_pin(*backend, *pin, id, receiver, args, eng);
			}), h.engine());
		}
		auto target = h.resolve_member_target(v);
		if (!target) { return h.make_null(); }
		script_value method_val = target.method(id);
		return method_val.is_function() ? h.bound_method(v, method_val) : h.make_null();
	}

	// a op b and a op= b on any operands (a compound names its index in compound): the bitwise operators' kernel, numbers through the
	// scalar kernels, string + through the host's concat, math values, and an object's operator; "Left/Right operand must be numeric"
	// otherwise
	template <class Host>
	checked_result<script_value> evaluate_arithmetic(Host& h, const script_value& left_in, token_type op, const script_value& right_in, int compound) {
		// a compound through a subscript (m["k"] -= dt) evaluates its target to a reference
		const script_value& left = left_in.deref();
		const script_value& right = right_in.deref();
		const size_t li = left.raw_storage_index();
		const size_t ri = right.raw_storage_index();
		if (li == script_value::TYPEID_CPP_BOUND || ri == script_value::TYPEID_CPP_BOUND) [[unlikely]] {
			return evaluate_arithmetic(h, li == script_value::TYPEID_CPP_BOUND ? left.bound_decoded_temp() : left, op,
			                           ri == script_value::TYPEID_CPP_BOUND ? right.bound_decoded_temp() : right, compound);
		}
		// &= |= ^= <<= >>=: an operand that is not an integer (a char promotes) takes the compound's text
		if (bitwise_token(op)) [[unlikely]] {
			const auto integral = [](size_t idx) { return idx == script_value::TYPEID_INT || idx == script_value::TYPEID_CHAR; };
			if (compound >= 0 && (!integral(li) || !integral(ri))) {
				if (auto custom = object_compound_operand(h, left, op, right)) { return std::move(*custom); }
				return compound_mismatch(static_cast<size_t>(compound), left, right, h.engine());
			}
			const operator_ids& ids = h.ids();
			switch (op) {
				case token_type::ampersand: return binary_bitwise<bit_op::bit_and>(h, ids.ampersand, left, right);
				case token_type::pipe: return binary_bitwise<bit_op::bit_or>(h, ids.pipe, left, right);
				case token_type::caret: return binary_bitwise<bit_op::bit_xor>(h, ids.caret, left, right);
				case token_type::left_shift: return binary_bitwise<bit_op::shift_left>(h, ids.left_shift, left, right);
				default: return binary_bitwise<bit_op::shift_right>(h, ids.right_shift, left, right);
			}
		}
		if (utf8_operands(li, ri)) [[unlikely]] {
			return utf8_rewrapped(evaluate_arithmetic(h, utf8_unwrapped(left, h.engine()), op, utf8_unwrapped(right, h.engine()), compound), h.engine());
		}
		if (char_operands_promote(li, ri)) [[unlikely]] {
			return evaluate_arithmetic(h, char_promoted(left, h.engine()), op, char_promoted(right, h.engine()), compound);
		}
		if (op == token_type::plus && (li == script_value::TYPEID_STRING || ri == script_value::TYPEID_STRING)) {
			if (!h.joins_text && string_beside_scalar(li, ri)) [[unlikely]] {
				return compound >= 0 ? compound_mismatch(static_cast<size_t>(compound), left, right, h.engine()) : string_plus_refused(li, ri);
			}
			return h.concat(left, right);
		}
		if (li == script_value::TYPEID_INT && ri == script_value::TYPEID_INT) {
			if (!scalar_arith_op(op)) { return checked_result<script_value>(make_error_code(runtime_error_code::unknown_operator), "Unknown arithmetic operator"); }
			script_int out;
			if (const auto fault = int_arith(op, left.unchecked_as_int(), right.unchecked_as_int(), out); fault != scalar_fault::none) {
				return scalar_fault_error<checked_result<script_value>>(op, fault, compound);
			}
			return h.make_int(out);
		}
		const bool left_numeric = numeric_storage(li);
		if (!left_numeric || !numeric_storage(ri)) {
			if (math::owns_arithmetic(li, ri)) [[unlikely]] { return math::binary(math::bop_of(op), left, right, h.engine()); }
			if (compound >= 0) {
				if (auto custom = object_compound_operand(h, left, op, right)) { return std::move(*custom); }
				return compound_mismatch(static_cast<size_t>(compound), left, right, h.engine());
			}
			return checked_result<script_value>(make_error_code(runtime_error_code::invalid_numeric_operand), left_numeric ? "Right operand must be numeric" : "Left operand must be numeric");
		}
		if (!scalar_arith_op(op)) { return checked_result<script_value>(make_error_code(runtime_error_code::unknown_operator), "Unknown arithmetic operator"); }
		script_float out;
		if (const auto fault = float_arith(op, as_script_float(left, li), as_script_float(right, ri), out); fault != scalar_fault::none) {
			return scalar_fault_error<checked_result<script_value>>(op, fault);
		}
		return h.make_float(out);
	}

	// a op= b on a member, a field, a static or an element, whose store converts the result (`op` is the compound's binary operator):
	// text += appends by the one '+=' table, ++ and -- step (incdec_step), every other pairing is evaluate_arithmetic under the
	// compound's texts
	template <class Host>
	checked_result<script_value> compound_value(Host& h, token_type op, const script_value& current_in, const script_value& right_in, bool incdec = false,
	                                            bool text_slot = false) {
		if (incdec) { return incdec_step(current_in, op == token_type::plus, h.engine()); }
		const script_value& current = current_in.deref();
		const script_value& right = right_in.deref();
		if (op == token_type::plus) {
			const script_value c = current.raw_storage_index() == script_value::TYPEID_CPP_BOUND ? current.bound_decoded_temp() : current;
			const script_value r = right.raw_storage_index() == script_value::TYPEID_CPP_BOUND ? right.bound_decoded_temp() : right;
			if (compound_plus_text(c, r, text_slot) == compound_text::mismatch) { return compound_mismatch(0, c, r, h.engine()); }
		}
		return evaluate_arithmetic(h, current, op, right, compound_index(op));
	}

}

#endif
