#pragma once
// Script `==` on heavy operands is memberwise, like C++ containers:
// arrays elementwise, maps entrywise in key order, class instances fieldwise in slot order,
// each with an early out on a size or class mismatch and on the first unequal member. Each
// member pair runs the backend's full `==` (equal_operands), so a nested user operator== is
// honored and the parallel operator wall keeps its verdict. Pairs nest natively up to
// k_inline_compare_depth; deeper, the rest of the comparison runs on an explicit frame stack,
// so values compare as deep as memory allows. Identity is a separate question, answered by
// same_object(x, y) (engine.cpp).
// KEEP BYTE-PARALLEL: both backends reach these through binary_equal (binary_handlers.hpp).
#include <jaiscript/core/value.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/object_borrow.hpp>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace jai::detail {

    // The class of a class-instance operand (null for anything else); the parallel operator wall reads it without copying a
    // method value
    inline const class_definition* operand_instance_class(const script_value& v) {
        const script_value& d = v.is_reference() ? v.deref() : v;
        if (!d.is_object()) {
            const object_borrow_record* borrowed = d.parallel_object_borrow();
            return borrowed ? borrowed->source->get_class_definition() : nullptr;
        }
        auto holder = const_cast<script_value&>(d).get_object_holder();
        if (!holder || !holder->is_instance()) { return nullptr; }
        return holder->unchecked_instance()->get_class_definition();
    }

    inline bool instance_declares_operator(const script_value& v, uint64_t id) {
        const class_definition* cd = operand_instance_class(v);
        return cd && cd->defines_method(id);
    }

    // Memberwise == and <=> levels that nest natively (the host's equality_depth counts them) before a walk takes the rest
    inline constexpr size_t k_inline_compare_depth = 32;
    struct compare_depth_scope {
        size_t& depth;
        explicit compare_depth_scope(size_t& a_depth) : depth(a_depth) { ++depth; }
        ~compare_depth_scope() { --depth; }
    };

    // A frame stack whose first frames live inline: a comparison that nests a few levels allocates nothing
    template <class Frame>
    class inline_frame_stack {
    public:
        bool empty() const noexcept { return depth_ == 0; }
        Frame& top() noexcept { return depth_ <= k_inline ? *inline_[depth_ - 1] : deeper_.back(); }
        Frame& push() {
            if (depth_ < k_inline) { return inline_[depth_++].emplace(); }
            ++depth_;
            return deeper_.emplace_back();
        }
        void pop() {
            if (depth_ > k_inline) { deeper_.pop_back(); } else { inline_[depth_ - 1].reset(); }
            --depth_;
        }
    private:
        static constexpr size_t k_inline = 4;
        std::optional<Frame> inline_[k_inline];
        std::vector<Frame> deeper_;
        size_t depth_ = 0;
    };

    // A memberwise == in progress: one frame per pair of containers or instances under comparison. The frame owns both
    // nodes, so a user operator== on a member cannot free what the comparison still walks
    struct equality_frame {
        enum class shape : uint8_t { values, elements, map, fields };
        shape kind = shape::values;
        bool native_compared = false;
        size_t index = 0;
        size_t count = 0;
        strong_ptr<script_array> left_array, right_array;
        strong_ptr<script_map> left_map, right_map;
        script_map::const_iterator left_entry{}, right_entry{};
        strong_ptr<script_value::object_holder> left_holder, right_holder;
        std::vector<std::pair<uint64_t, const script_value*>> fields;
    };
    struct equality_walk {
        inline_frame_stack<equality_frame> frames;
        bool opened = false;   // the last member pair joined the walk as a frame
    };

    template <class Host>
    checked_result<script_value> equal_operands(Host& h, const script_value& left, const script_value& right, equality_walk* walk);

    // Two arrays' answer when a size or a typed buffer settles it
    inline std::optional<bool> settled_arrays_equal(const script_array* a, const script_array* b) {
        const size_t n = a->size();
        if (n != b->size()) { return false; }
        if (a->is_typed() && b->is_typed() && a->kind() == b->kind()) {
            switch (a->kind()) {
            case script_array::kind_t::i64: return a->ints() == b->ints();
            case script_array::kind_t::f64: {
                const auto& x = a->floats(); const auto& y = b->floats();
                for (size_t i = 0; i < n; ++i) { if (!(x[i] == y[i])) { return false; } }   // == on doubles: NaN never equal
                return true;
            }
            case script_array::kind_t::b8:
            case script_array::kind_t::c8:
            case script_array::kind_t::u8:
            case script_array::kind_t::i8: return a->bytes() == b->bytes();
            case script_array::kind_t::str: {
                const auto& x = a->strs(); const auto& y = b->strs();
                for (size_t i = 0; i < n; ++i) {
                    const bool same = (x[i].get() == y[i].get()) || (x[i] && y[i] && *x[i] == *y[i]) || (!x[i] && !y[i]);
                    if (!same) { return false; }
                }
                return true;
            }
            default: break;   // obj: element `==` (a user operator== may apply)
            }
        }
        return std::nullopt;
    }

    // Two arrays: their settled answer, else a frame on the walk
    inline std::optional<bool> open_arrays_equal(const script_value& lv, const script_value& rv, equality_walk& walk) {
        const auto& la = lv.deref().unchecked_get_array_storage();
        const auto& ra = rv.deref().unchecked_get_array_storage();
        if (auto settled = settled_arrays_equal(la.get(), ra.get())) { return settled; }
        equality_frame& frame = walk.frames.push();
        frame.kind = !la->is_typed() && !ra->is_typed() ? equality_frame::shape::values : equality_frame::shape::elements;
        frame.count = la->size();
        frame.left_array = la;
        frame.right_array = ra;
        return std::nullopt;
    }

    // Keys compare under the map's own equivalence (the ordering that placed them), values
    // under script `==`: {1: "a"} and {1.0: "a"} hold different keys and stay unequal.
    inline std::optional<bool> open_maps_equal(const script_value& lv, const script_value& rv, equality_walk& walk) {
        const auto& lm = lv.deref().unchecked_get_map_storage();
        const auto& rm = rv.deref().unchecked_get_map_storage();
        if (lm->size() != rm->size()) { return false; }
        equality_frame& frame = walk.frames.push();
        frame.kind = equality_frame::shape::map;
        frame.left_map = lm;
        frame.right_map = rm;
        frame.left_entry = frame.left_map->begin();
        frame.right_entry = frame.right_map->begin();
        return std::nullopt;
    }

    // Two script class instance VALUES compare fieldwise; null when either side is a shared_ptr handle or not a script class
    // instance (identity rules apply)
    inline std::optional<std::pair<const class_instance*, const class_instance*>> script_instance_pair(const script_value& lv, const script_value& rv) {
        const script_value& l = lv.deref();
        const script_value& r = rv.deref();
        if (l.raw_storage_index() != script_value::TYPEID_OBJECT || r.raw_storage_index() != script_value::TYPEID_OBJECT) { return std::nullopt; }
        // shared_ptr handles compare by pointer, as in C++ (the value behind them compares fieldwise
        // only when both sides are values)
        if (l.type() == script_value_type::jai_shared_ptr_type || r.type() == script_value_type::jai_shared_ptr_type) { return std::nullopt; }
        const auto& lh = l.unchecked_object_handle();
        const auto& rh = r.unchecked_object_handle();
        if (!lh || !rh || !lh->is_instance() || !rh->is_instance()) { return std::nullopt; }
        const auto* a = lh->unchecked_instance();
        const auto* b = rh->unchecked_instance();
        const class_definition* ca = a->get_class_definition();
        const class_definition* cb = b->get_class_definition();
        if (!ca || !cb || !ca->is_script_class() || !cb->is_script_class()) { return std::nullopt; }
        return std::make_pair(a, b);
    }

    // Two instances of the SAME class (a derived instance is never equal to a base one) compare their fields in slot order,
    // then any overflow fields by id, then their C++ objects; false when the class or the field count already differs
    inline bool compared_fields(const class_instance* a, const class_instance* b, std::vector<std::pair<uint64_t, const script_value*>>& fields) {
        if (a->get_class_definition() != b->get_class_definition()) { return false; }
        a->for_each_field([&](uint64_t id, const script_value& v) { fields.emplace_back(id, &v); });
        size_t nb = 0;
        b->for_each_field([&](uint64_t, const script_value&) { ++nb; });
        return fields.size() + (a->native() ? 1 : 0) == nb + (b->native() ? 1 : 0);
    }

    inline std::optional<bool> open_instances_equal(const script_value& lv, const script_value& rv, const class_instance* a, const class_instance* b,
                                                    equality_walk& walk) {
        std::vector<std::pair<uint64_t, const script_value*>> fields;
        if (!compared_fields(a, b, fields)) { return false; }
        equality_frame& frame = walk.frames.push();
        frame.kind = equality_frame::shape::fields;
        frame.left_holder = lv.deref().unchecked_object_handle();
        frame.right_holder = rv.deref().unchecked_object_handle();
        frame.fields = std::move(fields);
        return std::nullopt;
    }

    enum class member_step : uint8_t { pair, unequal, done };

    // The frame's next member pair into x and y (the element temporaries own a materialized pair)
    inline member_step next_equality_member(equality_frame& frame, const script_value*& x, const script_value*& y,
                                            script_value& left_element, script_value& right_element, engine* eng) {
        switch (frame.kind) {
        case equality_frame::shape::values:
        case equality_frame::shape::elements: {
            if (frame.index == frame.count) { return member_step::done; }
            const script_array& a = *frame.left_array;
            const script_array& b = *frame.right_array;
            const size_t i = frame.index++;
            if (i >= a.size() || i >= b.size()) { return member_step::unequal; }   // an operator== resized an array mid-walk
            if (!a.is_typed() && !b.is_typed()) {
                x = &a.values()[i];
                y = &b.values()[i];
            } else {
                left_element = a.get(i, eng);
                right_element = b.get(i, eng);
                x = &left_element;
                y = &right_element;
            }
            return member_step::pair;
        }
        case equality_frame::shape::map: {
            if (frame.left_entry == frame.left_map->end()) { return member_step::done; }
            if (frame.right_entry == frame.right_map->end()) { return member_step::unequal; }
            if (map_key_order(frame.left_entry->first, frame.right_entry->first) != 0) { return member_step::unequal; }
            x = &frame.left_entry->second;
            y = &frame.right_entry->second;
            ++frame.left_entry;
            ++frame.right_entry;
            return member_step::pair;
        }
        case equality_frame::shape::fields: {
            const auto* a = frame.left_holder->unchecked_instance();
            const auto* b = frame.right_holder->unchecked_instance();
            if (frame.index < frame.fields.size()) {
                const auto& [id, field] = frame.fields[frame.index++];
                const script_value* other = b->find_field_value(id);
                if (!other) { return member_step::unequal; }
                x = field;
                y = other;
                return member_step::pair;
            }
            if (frame.native_compared) { return member_step::done; }
            frame.native_compared = true;
            const script_value* native = a->native();
            if (!native) { return member_step::done; }
            const script_value* other = b->native();
            if (!other) { return member_step::unequal; }
            x = native;
            y = other;
            return member_step::pair;
        }
        }
        return member_step::done;
    }

    // The first unequal member, or error, anywhere answers the whole comparison: each level returned it unchanged
    template <class Host>
    checked_result<script_value> run_equality_walk(Host& h, equality_walk& walk) {
        engine* eng = h.engine();
        script_value left_element(std::monostate{}, eng);
        script_value right_element(std::monostate{}, eng);
        while (!walk.frames.empty()) {
            const script_value* x = nullptr;
            const script_value* y = nullptr;
            const member_step step = next_equality_member(walk.frames.top(), x, y, left_element, right_element, eng);
            if (step == member_step::done) {
                walk.frames.pop();
                continue;
            }
            if (step == member_step::unequal) { return script_value(false, eng); }
            walk.opened = false;
            auto answer = equal_operands(h, *x, *y, &walk);
            if (!answer) { return answer; }
            if (walk.opened) { continue; }
            if (!answer.value().unchecked_as_bool()) { return script_value(false, eng); }
        }
        return script_value(true, eng);
    }

    // The native levels: each holds its containers, so an operator== that empties or replaces one frees nothing it walks
    template <class Host>
    checked_result<script_value> arrays_equal(Host& h, const script_value& lv, const script_value& rv) {
        engine* eng = h.engine();
        const strong_ptr<script_array> a = lv.deref().unchecked_get_array_storage();
        const strong_ptr<script_array> b = rv.deref().unchecked_get_array_storage();
        if (auto settled = settled_arrays_equal(a.get(), b.get())) { return script_value(*settled, eng); }
        compare_depth_scope scope(h.equality_depth());
        const size_t n = a->size();
        for (size_t i = 0; i < n; ++i) {
            if (i >= a->size() || i >= b->size()) { return script_value(false, eng); }   // an operator== resized an array
            checked_result<script_value> answer = !a->is_typed() && !b->is_typed()
                ? equal_operands(h, a->values()[i], b->values()[i], nullptr)
                : equal_operands(h, a->get(i, eng), b->get(i, eng), nullptr);
            if (!answer) { return answer; }
            if (!answer.value().unchecked_as_bool()) { return script_value(false, eng); }
        }
        return script_value(true, eng);
    }

    template <class Host>
    checked_result<script_value> maps_equal(Host& h, const script_value& lv, const script_value& rv) {
        engine* eng = h.engine();
        const strong_ptr<script_map> a = lv.deref().unchecked_get_map_storage();
        const strong_ptr<script_map> b = rv.deref().unchecked_get_map_storage();
        if (a->size() != b->size()) { return script_value(false, eng); }
        compare_depth_scope scope(h.equality_depth());
        auto ib = b->begin();
        for (auto ia = a->begin(); ia != a->end(); ++ia, ++ib) {
            if (ib == b->end() || map_key_order(ia->first, ib->first) != 0) { return script_value(false, eng); }
            auto answer = equal_operands(h, ia->second, ib->second, nullptr);
            if (!answer) { return answer; }
            if (!answer.value().unchecked_as_bool()) { return script_value(false, eng); }
        }
        return script_value(true, eng);
    }

    template <class Host>
    checked_result<script_value> instances_equal(Host& h, const script_value& lv, const script_value& rv, const class_instance* a, const class_instance* b) {
        engine* eng = h.engine();
        std::vector<std::pair<uint64_t, const script_value*>> fields;
        if (!compared_fields(a, b, fields)) { return script_value(false, eng); }
        const strong_ptr<script_value::object_holder> left_holder = lv.deref().unchecked_object_handle();
        const strong_ptr<script_value::object_holder> right_holder = rv.deref().unchecked_object_handle();
        compare_depth_scope scope(h.equality_depth());
        for (const auto& [id, field] : fields) {
            const script_value* other = b->find_field_value(id);
            if (!other) { return script_value(false, eng); }
            auto answer = equal_operands(h, *field, *other, nullptr);
            if (!answer) { return answer; }
            if (!answer.value().unchecked_as_bool()) { return script_value(false, eng); }
        }
        if (const script_value* native = a->native()) {
            const script_value* other = b->native();
            if (!other) { return script_value(false, eng); }
            auto answer = equal_operands(h, *native, *other, nullptr);
            if (!answer) { return answer; }
            if (!answer.value().unchecked_as_bool()) { return script_value(false, eng); }
        }
        return script_value(true, eng);
    }

    // A pair binary_equal compares memberwise: inside a walk its frame joins it (the walk carries the answer), else it starts one
    template <class Host, class Open>
    checked_result<script_value> compare_members(Host& h, equality_walk* walk, Open&& open) {
        if (walk) {
            if (auto answered = open(*walk)) { return script_value(*answered, h.engine()); }
            walk->opened = true;
            return script_value(true, h.engine());
        }
        equality_walk own;
        if (auto answered = open(own)) { return script_value(*answered, h.engine()); }
        return run_equality_walk(h, own);
    }

    // The object a holder names: holders are boxes, and several (a handle, an extend result, a re-boxed slot, two wraps of
    // one C++ object) can hold one object
    inline const void* holder_identity(const script_value::object_holder* holder) {
        if (holder->holds_object()) { return holder->object(); }
        if (holder->bound_object()) { return holder->bound_object(); }
        return holder;
    }
    // A weak handle names its owner's object while it lives, and its owner after (an expired handle is no other object's)
    inline const void* object_identity(const script_value& v) {
        if (v.raw_storage_index() == script_value::TYPEID_WEAK_PTR) {
            const auto weak = v.get_weak_ptr();
            if (const auto held = weak.lock()) { return holder_identity(held.get()); }
            return weak.owner_identity();
        }
        const auto holder = v.get_object_holder();
        return holder ? holder_identity(holder.get()) : nullptr;
    }

    // same_object(x, y), and every same_as: identity. References resolve to their referents and handles (shared or weak) to
    // the object they name; arrays, maps, class instances, host objects and functions are the same object when they are one
    // allocation. Values without identity (numbers, chars, bools, strings, which share buffers copy-on-write, math values)
    // are "the same" when they are equal values of one kind. A typed null is the same as null.
    inline bool same_object(const script_value& lv, const script_value& rv) {
        script_value l = lv.deref().try_unwrap_transparent_wrapper().value_or(lv.deref());
        script_value r = rv.deref().try_unwrap_transparent_wrapper().value_or(rv.deref());
        if (l.raw_storage_index() == script_value::TYPEID_CPP_BOUND) [[unlikely]] { l = l.bound_decoded_temp(); }
        if (r.raw_storage_index() == script_value::TYPEID_CPP_BOUND) [[unlikely]] { r = r.bound_decoded_temp(); }
        if (l.is_null() || r.is_null()) { return l.is_null() && r.is_null(); }
        const size_t li = l.raw_storage_index();
        const size_t ri = r.raw_storage_index();
        const bool l_handle = li == script_value::TYPEID_OBJECT || li == script_value::TYPEID_WEAK_PTR;
        const bool r_handle = ri == script_value::TYPEID_OBJECT || ri == script_value::TYPEID_WEAK_PTR;
        if (l_handle || r_handle) { return l_handle && r_handle && object_identity(l) == object_identity(r); }
        if (li != ri || li == script_value::TYPEID_INVALID) { return false; }
        switch (li) {
        case script_value::TYPEID_ARRAY:
            return l.unchecked_array_node() == r.unchecked_array_node();
        case script_value::TYPEID_MAP:
            return &l.unchecked_as_map() == &r.unchecked_as_map();
        case script_value::TYPEID_FUNCTION:
        case script_value::TYPEID_PARALLEL_BORROW:
            return (l <=> r) == 0;
        default:
            return l == r;
        }
    }

} // namespace jai::detail
