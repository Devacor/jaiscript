#pragma once
#include <jaiscript/detail/structural_equality.hpp>
#include <jaiscript/detail/math_values.hpp>
#include <algorithm>
#include <compare>
#include <cmath>
#include <bit>
#include <optional>

namespace jai::detail {
	inline script_int float_order(script_float a_left, script_float a_right) noexcept {
		// Natural finite order; signed zeros equivalent; all NaNs form the final group.
		if (a_left < a_right) { return -1; }
		if (a_left > a_right) { return 1; }
		if (a_left == a_right) { return 0; }
		return script_int(std::isnan(a_left)) - script_int(std::isnan(a_right));
	}
	template<class T> inline script_int order_sign(const T& a_order) noexcept {
		return a_order < 0 ? -1 : a_order > 0 ? 1 : 0;
	}
	inline std::optional<bool> builtin_identity_equal(const script_value& a_left_in, const script_value& a_right_in) {
		const auto& a_left = a_left_in.deref(); const auto& a_right = a_right_in.deref();
		if (a_left.is_function() && a_right.is_function()) { return (a_left <=> a_right) == 0; }
		if (a_left.is_shared_ptr_type() && a_right.is_shared_ptr_type()) { return (a_left <=> a_right) == 0; }
		if (a_left.is_weak_ptr() && a_right.is_weak_ptr()) {
			const auto left = a_left.get_weak_ptr(), right = a_right.get_weak_ptr();
			return !left.owner_before(right) && !right.owner_before(left);
		}
		return std::nullopt;
	}
	// A lexicographic <=> in progress: one frame per pair of arrays or maps under comparison, each holding the pair's members
	struct order_frame {
		bool is_map = false;
		size_t index = 0;
		std::vector<script_value> left_values, right_values;
		std::vector<std::pair<script_value, script_value>> left_entries, right_entries;
	};
	struct order_walk {
		inline_frame_stack<order_frame> frames;
		bool opened = false;   // the last member pair joined the walk as a frame
	};

	// A member pair's <=> answer as an order: its sign, 0 while a throw unwinds, or the error
	template<class Host>
	checked_result<script_value> member_order(Host& a_host, checked_result<script_value>&& a_answer) {
		if (!a_answer) { return a_answer.error_value(); }
		engine* eng = a_host.engine();
		if (a_host.unwinding()) { return script_value(script_int(0), eng); }
		const auto& value = a_answer.value().deref();
		if (value.is_int()) { return script_value(order_sign(value.unchecked_as_int()), eng); }
		if (value.is_float()) { return script_value(order_sign(value.unchecked_as_float()), eng); }
		return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "operator<=> must return a numeric ordering");
	}

	// The first nonzero member order, error or unwind anywhere answers the whole comparison, as each level returned it
	template<class Host>
	checked_result<script_value> run_order_walk(Host& a_host, order_walk& a_walk) {
		engine* eng = a_host.engine();
		auto result = [eng](script_int a_order) { return checked_result<script_value>(script_value(a_order, eng)); };
		while (true) {
			order_frame& frame = a_walk.frames.top();
			const size_t ln = frame.is_map ? frame.left_entries.size() : frame.left_values.size();
			const size_t rn = frame.is_map ? frame.right_entries.size() : frame.right_values.size();
			script_int order = 0;
			if (frame.index < std::min(ln, rn)) {
				const size_t i = frame.index++;
				const script_value* x = nullptr;
				const script_value* y = nullptr;
				if (frame.is_map) {
					const auto& li = frame.left_entries[i]; const auto& ri = frame.right_entries[i];
					// Keys order as the map orders them (core/script_map.hpp); values use language ordering.
					order = static_cast<script_int>(map_key_order(li.first, ri.first));
					x = &li.second;
					y = &ri.second;
				} else {
					x = &frame.left_values[i];
					y = &frame.right_values[i];
				}
				if (order == 0) {
					a_walk.opened = false;
					auto answer = a_host.spaceship_member(*x, *y, &a_walk);
					if (a_walk.opened) { continue; }
					auto ordered = member_order(a_host, std::move(answer));
					if (!ordered || a_host.unwinding() || ordered.value().unchecked_as_int() != 0) { return ordered; }
					continue;
				}
			} else {
				order = ln < rn ? -1 : ln > rn ? 1 : 0;
			}
			a_walk.frames.pop();
			if (a_walk.frames.empty()) { return result(order); }
			if (a_host.unwinding()) { return result(0); }
			if (order != 0) { return result(order); }
		}
	}

	// Two arrays or maps: their order when typed buffers settle it, else their members into a_frame
	inline std::optional<script_int> settled_order(const script_value& a_left, const script_value& a_right, engine* a_engine, order_frame& a_frame) {
		if (a_left.is_array()) {
			const auto left = a_left.unchecked_get_array_storage(), right = a_right.unchecked_get_array_storage();
			if (left->kind() == right->kind()) {
				switch (left->kind()) {
					case script_array::kind_t::i64: return order_sign(std::lexicographical_compare_three_way(left->ints().begin(), left->ints().end(), right->ints().begin(), right->ints().end()));
					case script_array::kind_t::f64: return order_sign(std::lexicographical_compare_three_way(left->floats().begin(), left->floats().end(), right->floats().begin(), right->floats().end(), [](auto a, auto b) { const auto cmp = float_order(a,b); return cmp < 0 ? std::weak_ordering::less : cmp > 0 ? std::weak_ordering::greater : std::weak_ordering::equivalent; }));
					case script_array::kind_t::b8: case script_array::kind_t::c8: case script_array::kind_t::u8:
						return order_sign(std::lexicographical_compare_three_way(left->bytes().begin(), left->bytes().end(), right->bytes().begin(), right->bytes().end()));
					case script_array::kind_t::i8:
						return order_sign(std::lexicographical_compare_three_way(left->bytes().begin(), left->bytes().end(), right->bytes().begin(), right->bytes().end(), [](uint8_t a, uint8_t b) { return static_cast<int8_t>(a) <=> static_cast<int8_t>(b); }));
					case script_array::kind_t::str:
						return order_sign(std::lexicographical_compare_three_way(left->strs().begin(), left->strs().end(), right->strs().begin(), right->strs().end(), [](const auto& a, const auto& b) { return *a <=> *b; }));
					case script_array::kind_t::v2: case script_array::kind_t::iv2: case script_array::kind_t::v3: case script_array::kind_t::iv3: case script_array::kind_t::v4: case script_array::kind_t::rgba: {
						const size_t count = std::min(left->size(), right->size()), stride = script_array::vec_stride(left->kind());
						for (size_t i = 0; i < count; ++i) {
							for (size_t lane = 0; lane < stride; ++lane) {
								const uint32_t lw = left->vec_word(i,lane), rw = right->vec_word(i,lane);
								const auto cmp = script_array::vec_integral(left->kind()) ? order_sign(std::bit_cast<int32_t>(lw) <=> std::bit_cast<int32_t>(rw)) : float_order(std::bit_cast<float>(lw), std::bit_cast<float>(rw));
								if (cmp) { return cmp; }
							}
						}
						return script_int(left->size() < right->size() ? -1 : left->size() > right->size() ? 1 : 0);
					}
					default: break;
				}
			}
			a_frame.left_values = left->materialize_values(a_engine);
			a_frame.right_values = right->materialize_values(a_engine);
			return std::nullopt;
		}
		const auto left = a_left.unchecked_get_map_storage(), right = a_right.unchecked_get_map_storage();
		a_frame.is_map = true;
		a_frame.left_entries.assign(left->begin(), left->end());
		a_frame.right_entries.assign(right->begin(), right->end());
		return std::nullopt;
	}

	inline std::optional<script_int> open_order(const script_value& a_left, const script_value& a_right, engine* a_engine, order_walk& a_walk) {
		order_frame frame;
		if (auto settled = settled_order(a_left, a_right, a_engine, frame)) { return settled; }
		a_walk.frames.push() = std::move(frame);
		return std::nullopt;
	}

	// A native level, while the comparison is shallow
	template<class Host>
	checked_result<script_value> native_order(Host& a_host, const script_value& a_left, const script_value& a_right) {
		engine* eng = a_host.engine();
		order_frame frame;
		if (auto settled = settled_order(a_left, a_right, eng, frame)) { return script_value(*settled, eng); }
		compare_depth_scope scope(a_host.equality_depth());
		const size_t ln = frame.is_map ? frame.left_entries.size() : frame.left_values.size();
		const size_t rn = frame.is_map ? frame.right_entries.size() : frame.right_values.size();
		for (size_t i = 0; i < std::min(ln, rn); ++i) {
			const script_value* x = nullptr;
			const script_value* y = nullptr;
			if (frame.is_map) {
				const auto& li = frame.left_entries[i]; const auto& ri = frame.right_entries[i];
				// Keys order as the map orders them (core/script_map.hpp); values use language ordering.
				if (const auto cmp = static_cast<script_int>(map_key_order(li.first, ri.first))) { return script_value(cmp, eng); }
				x = &li.second;
				y = &ri.second;
			} else {
				x = &frame.left_values[i];
				y = &frame.right_values[i];
			}
			auto answer = member_order(a_host, a_host.spaceship_member(*x, *y, nullptr));
			if (!answer || a_host.unwinding() || answer.value().unchecked_as_int() != 0) { return answer; }
		}
		return script_value(script_int(ln < rn ? -1 : ln > rn ? 1 : 0), eng);
	}

	// Inside a walk (a_walk set) a pair of arrays or maps joins it as a frame and the walk carries the answer
	template<class Host>
	std::optional<checked_result<script_value>> builtin_order(Host& a_host, const script_value& a_left_in, const script_value& a_right_in, order_walk* a_walk) {
		const auto& a_left = a_left_in.deref(); const auto& a_right = a_right_in.deref();
		engine* const a_engine = a_host.engine();
		auto result = [&](script_int a_order) { return script_value(a_order, a_engine); };
		if (a_left.is_null() && a_right.is_null()) { return result(0); }
		if (a_left.is_bool() && a_right.is_bool()) { return result(script_int(a_left.unchecked_as_bool()) - script_int(a_right.unchecked_as_bool())); }
		if (a_left.is_function() && a_right.is_function()) { return result(order_sign(a_left <=> a_right)); }
		if (a_left.is_object() && a_right.is_object()) {
			const auto left = a_left.get_object_holder(), right = a_right.get_object_holder();
			if (left && right && left->is_coroutine_handle() && right->is_coroutine_handle()) { return result(order_sign(a_left <=> a_right)); }
		}
		if (a_left.is_shared_ptr_type() && a_right.is_shared_ptr_type()) { return result(order_sign(a_left <=> a_right)); }
		if (a_left.is_weak_ptr() && a_right.is_weak_ptr()) {
			const auto left = a_left.get_weak_ptr(), right = a_right.get_weak_ptr();
			return result(left.owner_before(right) ? -1 : right.owner_before(left) ? 1 : 0);
		}
		if (math::is_kind(a_left.raw_storage_index()) && math::is_kind(a_right.raw_storage_index())) {
			const auto left = a_left.unchecked_vec_kind(), right = a_right.unchecked_vec_kind();
			if (math::is_color(left) != math::is_color(right)) { return std::nullopt; }
			const size_t ln = math::dims(left), rn = math::dims(right);
			for (size_t i = 0; i < std::min(ln, rn); ++i) {
				const auto lv = math::integral(left) ? script_float(math::i32_at(a_left, i)) : script_float(math::f32_at(a_left, i));
				const auto rv = math::integral(right) ? script_float(math::i32_at(a_right, i)) : script_float(math::f32_at(a_right, i));
				if (const auto cmp = float_order(lv, rv)) { return result(cmp); }
			}
			return result(ln < rn ? -1 : ln > rn ? 1 : 0);
		}
		if (!((a_left.is_array() && a_right.is_array()) || (a_left.is_map() && a_right.is_map()))) { return std::nullopt; }
		if (!a_walk && a_host.equality_depth() < k_inline_compare_depth) { return native_order(a_host, a_left, a_right); }
		if (a_walk) {
			if (auto answered = open_order(a_left, a_right, a_engine, *a_walk)) { return result(*answered); }
			a_walk->opened = true;
			return result(0);
		}
		order_walk own;
		if (auto answered = open_order(a_left, a_right, a_engine, own)) { return result(*answered); }
		return run_order_walk(a_host, own);
	}
}
