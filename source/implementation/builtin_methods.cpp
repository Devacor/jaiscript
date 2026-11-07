#include <jaiscript/jaiscript.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/builtin_api_arities.hpp>
#include <jaiscript/stdlib/containers.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/core/class_registry.hpp>
#include <jaiscript/detail/integer_ops.hpp>
#include <jaiscript/detail/string_mutator_receiver.hpp>
#include <jaiscript/detail/string_element.hpp>
#include <jaiscript/detail/arg_capture.hpp>
#include <jaiscript/detail/char_promotion.hpp>
#include <jaiscript/detail/utf8_operands.hpp>
#include <jaiscript/detail/utf8_text.hpp>
#include <jaiscript/detail/container_enforce.hpp>
#include <jaiscript/detail/map_entry_binding.hpp>
#include <jaiscript/detail/float_text.hpp>
#include <jaiscript/detail/structural_ordering.hpp>
#include <jaiscript/detail/value_text.hpp>
#include <jaiscript/detail/string_methods_more.hpp>
#include <jaiscript/detail/utf8_methods.hpp>
#include <jaiscript/detail/binary_data.hpp>
#include <cmath>
#include <functional>
#include <unordered_set>

namespace jai {

namespace {
    // An in-place builtin (it returns nothing) changes its receiver: the API catalog, which the const rules read, says so too
    constexpr bool in_place_methods_write() {
        const auto writes = [](std::string_view a_name) {
            const detail::BuiltinEffect* effect = detail::findBuiltinEffect(a_name);
            return effect && effect->effect == detail::BuiltinReceiverEffect::writes;
        };
        for (const auto& m : k_array_in_place_methods) { if (!writes(m.name)) { return false; } }
        for (const auto& m : k_map_in_place_methods) { if (!writes(m.name)) { return false; } }
        for (std::string_view m : k_string_mutator_method_names) { if (!writes(m)) { return false; } }
        return writes("reset");
    }
    static_assert(in_place_methods_write(), "an in-place builtin the API catalog does not list as mutate");

    struct pad_texts { const char* arity; const char* width; const char* empty; const char* fill; };

    // pad_left (side -1), pad_right (1) and pad_center (0, the odd byte on the right): the fill tiles from its start on each
    // side, as JavaScript's padStart and padEnd do ("ab".pad_left(7, "xyz") is "xyzxyab")
    checked_result<script_value> pad_text(const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args, int side,
                                          const pad_texts& texts) {
        if (args.size() < 1 || args.size() > 2) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), texts.arity);
        }
        if (!args[0].is_int()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), texts.width);
        }
        const script_int target_len = args[0].unchecked_as_int();
        std::string fill = " ";
        if (args.size() > 1) {
            if (args[1].is_char()) {
                fill = std::string(1, args[1].unchecked_as_char());
            } else if (args[1].is_text()) {
                if (args[1].unchecked_text().empty()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::invalid_operation), texts.empty);
                }
                fill = args[1].unchecked_text();
            } else {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), texts.fill);
            }
        }
        auto& str = self.unchecked_text_ref(ctx.get_engine());
        if (target_len <= static_cast<script_int>(str.size())) { return ctx.make_value(); }
        const size_t total = detail::saturating_count(target_len) - str.size();
        if (!detail::builtin_memory_charge(ctx.get_engine(), total)) [[unlikely]] { return detail::builtin_memory_cap(ctx.get_engine()); }
        if (total > std::string().max_size() - 64) { throw std::length_error("pad"); }
        const size_t left = side < 0 ? total : side > 0 ? 0 : total / 2;
        auto tiled = [&](size_t n) {
            std::string out;
            out.reserve(n);
            while (out.size() < n) { out.append(fill, 0, std::min(fill.size(), n - out.size())); }
            return out;
        };
        str = tiled(left) + str + tiled(total - left);
        return ctx.make_value();
    }
    // The argument positions a builtin's callback binds by reference (a script T& parameter, a host T& that writes back):
    // those take the builtin's elements as lvalues, as std::sort hands its comparator the elements it sorts
    uint32_t callback_lvalue_positions(const script_value& callback, size_t argc) {
        uint32_t positions = 0;
        for (size_t i = 0; i < argc; ++i) {
            if (static_cast<uint8_t>(detail::callee_binding(callback, i, false, argc)) & static_cast<uint8_t>(detail::arg_binding::reference)) {
                positions |= 1u << i;
            }
        }
        return positions;
    }

    // A script comparison may answer inconsistently (a predicate with state, one writing the elements it compares):
    // std::sort's unguarded loops then run past the range, while a merge never leaves its runs
    template <typename Less>
    void merge_sort_any_order(std::vector<script_value>& values, Less&& less) {
        const size_t count = values.size();
        if (count < 2) { return; }
        std::vector<script_value> other(values);
        auto merge = [&](std::vector<script_value>& from, std::vector<script_value>& to, size_t low, size_t width, size_t end) {
            const size_t mid = std::min(low + width, end), high = std::min(low + 2 * width, end);
            size_t i = low, j = mid, k = low;
            while (i < mid && j < high) { to[k++] = less(from[j], from[i]) ? std::move(from[j++]) : std::move(from[i++]); }
            while (i < mid) { to[k++] = std::move(from[i++]); }
            while (j < high) { to[k++] = std::move(from[j++]); }
        };
        // The bottom-up passes below a block's width run block by block, so a block's elements stay in cache across them: the
        // merges (and so the comparisons) are the whole-array passes', in another order
        constexpr size_t block = 256;
        const size_t local_widths_end = std::min(block, count);
        size_t local_passes = 0;
        for (size_t width = 1; width < local_widths_end; width *= 2) { ++local_passes; }
        for (size_t block_low = 0; block_low < count; block_low += block) {
            const size_t block_high = std::min(block_low + block, count);
            std::vector<script_value>* from = &values;
            std::vector<script_value>* to = &other;
            for (size_t width = 1; width < local_widths_end; width *= 2) {
                for (size_t low = block_low; low < block_high; low += 2 * width) { merge(*from, *to, low, width, block_high); }
                std::swap(from, to);
            }
        }
        std::vector<script_value>* from = local_passes % 2 ? &other : &values;
        std::vector<script_value>* to = local_passes % 2 ? &values : &other;
        for (size_t width = size_t{1} << local_passes; width < count; width *= 2) {
            for (size_t low = 0; low < count; low += 2 * width) { merge(*from, *to, low, width, count); }
            std::swap(from, to);
        }
        if (from != &values) { values = std::move(*from); }
    }

    // C++'s reference return for a builtin that hands back a stored element (front, back, first, last, map get): an
    // object, array or map comes back as a reference to its slot, so binding it to var/auto/T copies, var&/T& and a
    // member call reach the stored value; scalars and strings are copies already
    bool stored_result_is_heavy(const script_value& v) {
        const size_t k = v.deref().raw_storage_index();
        return k == script_value::TYPEID_OBJECT || k == script_value::TYPEID_ARRAY || k == script_value::TYPEID_MAP;
    }

    script_value stored_element_result(const builtin_method_context& ctx, script_value& self, size_t index) {
        const script_array* node = self.unchecked_array_node();
        script_value element = node->get(index, ctx.get_engine());
        if (!stored_result_is_heavy(element)) { return element; }
        const auto tag = self.get_type_info();
        return script_value::make_element_reference(self.unchecked_get_array_storage(), index, ctx.get_engine(), tag ? tag->element_type() : nullptr);
    }

    // A builtin's callback, prepared by the backend once for the builtin's whole run
    class builtin_callback {
        const script_function& fn_;
        execution_backend* backend_ = nullptr;
        prepared_callback* prepared_ = nullptr;
    public:
        builtin_callback(const script_value& callback, engine* eng) : fn_(callback.deref().unchecked_as_function()) {
            backend_ = eng ? detail::engine_access::get_execution_backend(*eng) : nullptr;
            prepared_ = backend_ ? backend_->prepare_callback(callback) : nullptr;
        }
        ~builtin_callback() { if (prepared_) { backend_->release_callback(prepared_); } }
        builtin_callback(const builtin_callback&) = delete;
        builtin_callback& operator=(const builtin_callback&) = delete;
        checked_result<script_value> operator()(const std::vector<script_value>& args) const { return prepared_ ? prepared_->call(args) : fn_(args); }
    };

    // A callback call whose lvalue positions get a cell holding the element; the element takes the cell's value back
    // after the call (the builtin works on its own copies of the container's elements)
    // call_args: the builtin's buffer for the call's arguments, reused call to call (empty after the call)
    checked_result<script_value> call_with_lvalues(const builtin_callback& callback, uint32_t positions,
                                                   std::initializer_list<script_value*> elements, engine* eng, std::vector<script_value>& call_args) {
        struct release { std::vector<script_value>& a; ~release() { a.clear(); } } done{call_args};
        call_args.clear();
        size_t i = 0;
        for (script_value* e : elements) {
            call_args.push_back((positions >> i++) & 1 ? script_value::make_cell_reference(script_value(e->deref()), eng) : *e);
        }
        auto result = callback(call_args);
        i = 0;
        for (script_value* e : elements) {
            if ((positions >> i) & 1) {
                const script_value& now = call_args[i].deref();
                const bool same_object = now.raw_storage_index() == script_value::TYPEID_OBJECT && e->raw_storage_index() == script_value::TYPEID_OBJECT &&
                                         now.unchecked_object_handle().get() == e->unchecked_object_handle().get() && now.type_info_raw() == e->type_info_raw();
                if (!same_object) {
                    script_value taken = now;
                    *e = std::move(taken);
                }
            }
            ++i;
        }
        return result;
    }
    checked_result<script_value> call_with_lvalues(const builtin_callback& callback, uint32_t positions,
                                                   std::initializer_list<script_value*> elements, engine* eng) {
        std::vector<script_value> call_args;
        call_args.reserve(elements.size());
        return call_with_lvalues(callback, positions, elements, eng, call_args);
    }

    // A script callback's own declaration: the parameters it spells and the return type it declares (null for a host callable)
    const std::vector<parameter>* callback_parameters(const script_value& callback, type_info_ptr* declared_return = nullptr) {
        const script_value& c = callback.deref();
        if (c.raw_storage_index() != script_value::TYPEID_FUNCTION) return nullptr;
        const auto* thunk = c.unchecked_as_function().target<script_callable_thunk>();
        if (!thunk) return nullptr;
        const script_callable& p = thunk->payload;
        if (p.kind == script_callable::kind_type::function && p.fn) {
            if (declared_return) { *declared_return = p.fn->return_type; }
            return &p.fn->parameters();
        }
        if ((p.kind == script_callable::kind_type::method || p.kind == script_callable::kind_type::static_method) && p.ast()) {
            if (declared_return) { *declared_return = p.ast()->return_type; }
            return &p.ast()->parameters;
        }
        return nullptr;
    }

    // The array helpers' element callback (map, for_each, find, find_index, some, every, reduce): the element, then its index when the
    // callback declares a parameter for it, as JavaScript's and C#'s do. A T& element parameter is the live element (an element
    // reference, the array's element type its constraint), as std::for_each hands its function the element itself.
    struct element_callback {
        builtin_callback fn;
        uint32_t lvalues;
        bool with_index;
        size_t leading;   // arguments ahead of the element (reduce's accumulator)
        type_info_ptr element_type;

        element_callback(const script_value& callback, size_t lead, const script_value& self, engine* eng)
            : fn(callback, eng), lvalues(0), with_index(false), leading(lead) {
            const auto* params = callback_parameters(callback);
            with_index = params && params->size() >= lead + 2;
            lvalues = callback_lvalue_positions(callback, lead + (with_index ? 2 : 1)) & ((1u << (lead + 1)) - 1);   // the index is a value
            if (auto tag = self.get_type_info(); tag && tag->base_type == script_value_type::jai_array_type) {
                type_info_ptr et = tag->element_type();
                if (et && et->base_type != script_value_type::jai_any_type && et->base_type != script_value_type::jai_null_type) { element_type = et; }
            }
        }

        mutable std::vector<script_value> args;   // one buffer for the builtin's whole run

        checked_result<script_value> call(const strong_ptr<script_array>& array, size_t i, script_value* lead, engine* eng) const {
            struct release { std::vector<script_value>& a; ~release() { a.clear(); } } done{args};
            if (leading) { args.push_back((lvalues & 1) ? script_value::make_cell_reference(script_value(lead->deref()), eng) : *lead); }
            args.push_back(((lvalues >> leading) & 1) ? script_value::make_element_reference(array, i, eng, element_type) : array->get(i, eng));
            if (with_index) { args.push_back(script_value(static_cast<script_int>(i), eng)); }
            auto result = fn(args);
            if (leading && (lvalues & 1)) {
                script_value taken = args[0].deref();
                *lead = std::move(taken);
            }
            return result;
        }
    };

    // A predicate's answer: true only for a bool true, as filter and remove_if read theirs
    bool predicate_true(const script_value& v) {
        const script_value& d = v.deref();
        return d.is_bool() && d.unchecked_as_bool();
    }

    // A map callback (key, value): the value position aliases the entry (a map-entry reference, the map's value type as its
    // constraint), the key position gets a copy (a map key is const, as std::map's is)
    checked_result<script_value> call_map_entry(const builtin_callback& callback, uint32_t positions, script_value& map_value,
                                                const script_value& key, engine* eng) {
        const auto map_type = map_value.get_type_info();
        const auto& map = map_value.unchecked_as_map();
        const auto it = map.find(key);
        std::vector<script_value> call_args{
            (positions & 1) ? script_value::make_cell_reference(script_value(key), eng) : key,
            (positions & 2) ? script_value::make_map_entry_reference(map_value.unchecked_get_map_storage(), key, it != map.end() ? &it->second : nullptr,
                                                                     eng, map_type ? map_type->value_type() : nullptr)
                            : it->second };
        return callback(call_args);
    }

    // The size argument of resize, assign, reserve and array<T>(n, v): a non-negative int
    checked_result<size_t> array_size_argument(const script_value& a_value, const char* a_not_an_int) {
        const script_value& v = a_value.deref();
        if (!v.is_int()) {
            return checked_result<size_t>(make_error_code(runtime_error_code::type_mismatch), a_not_an_int);
        }
        if (v.unchecked_as_int() < 0) {
            return checked_result<size_t>(make_error_code(runtime_error_code::invalid_operation), "array size cannot be negative");
        }
        return detail::saturating_count(v.unchecked_as_int());
    }

    // The element std::vector<T>(n) and resize(n) append: T's zero, an empty container, a null handle, or a T its default
    // constructor builds
    checked_result<script_value> value_initialized_element(engine* a_engine, const type_info_ptr& a_element_type) {
        script_value made(std::monostate{}, a_engine);
        if (a_element_type) {
            switch (a_element_type->base_type) {
                case script_value_type::jai_int_type: made = script_value(script_int(0), a_engine); break;
                case script_value_type::jai_float_type: made = script_value(script_float(0.0), a_engine); break;
                case script_value_type::jai_bool_type: made = script_value(script_bool(false), a_engine); break;
                case script_value_type::jai_char_type: made = script_value(script_char(0), a_engine); break;
                case script_value_type::jai_string_type: made = script_value(script_string(), a_engine); break;
                case script_value_type::jai_utf8_string_type: made = script_value::make_utf8(std::string(), a_engine); break;
                case script_value_type::jai_array_type: made = script_value::make_array(a_element_type->element_type(), a_engine); break;
                case script_value_type::jai_map_type: made = script_value::make_map(a_element_type->key_type(), a_element_type->value_type(), a_engine); break;
                case script_value_type::jai_vec2_type: case script_value_type::jai_vec3_type: case script_value_type::jai_vec4_type:
                case script_value_type::jai_color_type: case script_value_type::jai_ivec2_type: case script_value_type::jai_ivec3_type: {
                    const uint32_t zero[4]{};
                    made = script_value::make_vec(a_element_type->base_type, zero, a_engine);
                    break;
                }
                case script_value_type::jai_object_type: {
                    auto global = detail::engine_access::get_global_environment(a_engine);
                    auto constructor = global ? global->get(std::string(a_element_type->type_name)) : checked_result<script_value>(make_error_code(runtime_error_code::undefined_variable));
                    if (!constructor || !constructor.value().is_function()) {
                        return checked_result<script_value>(make_error_code(runtime_error_code::undefined_variable), "No constructor found for class '{0}'", a_element_type->id);
                    }
                    auto constructed = constructor.value().as_function()({});
                    if (!constructed) { return constructed.error_value(); }
                    made = std::move(constructed.value());
                    break;
                }
                default: break;
            }
        }
        return detail::convert_container_element_taking(a_engine, made, a_element_type);
    }

    // Grows or shrinks an array to a_size as std::vector::resize does: new elements are copies of *a_fill, or value-initialized
    checked_result<script_value> resize_array(const builtin_method_context& a_context, script_value& a_self, size_t a_size, const script_value* a_fill) {
        engine* eng = a_context.get_engine();
        const strong_ptr<script_array> storage = a_self.unchecked_get_array_storage();
        const size_t old_size = storage->size();
        if (a_size <= old_size) {
            storage->truncate(a_size);
            return a_context.make_value();
        }
        if (!detail::builtin_memory_charge(eng, a_size - old_size, sizeof(script_value))) [[unlikely]] { return detail::builtin_memory_cap(eng); }
        const auto array_type = a_self.get_type_info();
        const type_info_ptr element_type = array_type ? array_type->element_type() : nullptr;
        script_value fill(std::monostate{}, eng);
        if (a_fill) {
            fill = a_fill->deref();
            if (!detail::container_element_compatible(fill, element_type, eng)) {
                return detail::element_type_error(a_context.get_string_symbolizer(), runtime_error_code::array_element_type_mismatch, "Cannot push '{0}' to array<{1}>", fill, element_type);
            }
        }
        storage->reserve(a_size);
        for (size_t i = old_size; i < a_size; ++i) {
            auto element = a_fill ? detail::convert_container_element(eng, fill, element_type) : value_initialized_element(eng, element_type);
            if (!element || a_context.unwinding()) {
                storage->truncate(old_size);   // std::vector::resize's strong guarantee: a failed grow changes nothing
                if (!element) { return element.error_value(); }
                return a_context.make_value();
            }
            storage->push(std::move(element.value()));
        }
        return a_context.make_value();
    }

    // The first a_count elements take a_fill as `xs[i] = a_fill` does (std::fill): a class element runs its operator= or the
    // implicit copy assignment in place, any other element is replaced by a converted copy
    checked_result<script_value> assign_elements(const builtin_method_context& a_context, script_value& a_self, size_t a_count, const script_value& a_fill) {
        engine* eng = a_context.get_engine();
        const strong_ptr<script_array> storage = a_self.unchecked_get_array_storage();
        const auto array_type = a_self.get_type_info();
        const type_info_ptr element_type = array_type ? array_type->element_type() : nullptr;
        const script_value& fill = a_fill.deref();
        if (!detail::container_element_compatible(fill, element_type, eng)) {
            return detail::element_type_error(a_context.get_string_symbolizer(), runtime_error_code::array_element_type_mismatch, "Cannot push '{0}' to array<{1}>", fill, element_type);
        }
        for (size_t i = 0; i < a_count && i < storage->size(); ++i) {
            script_value current = storage->get(i, eng);
            auto in_place = element_type ? detail::user_assign_operator(current, fill, eng, element_type.get()) : detail::var_slot_assign_in_place(current, fill, eng);
            if (in_place) {
                if (!*in_place) { return in_place->error_value(); }
            } else {
                auto element = detail::convert_container_element(eng, fill, element_type);
                if (!element) { return element.error_value(); }
                storage->set(i, std::move(element.value()));
            }
            if (a_context.unwinding()) { return a_context.make_value(); }
        }
        return a_context.make_value();
    }
}

// Initialize built-in method registries with interned method names for O(1) lookup
void init_builtin_method_registries(string_symbolizer* symbolizer, builtin_method_registries& out) {
    // Array methods
    out.array_methods = {
        {symbolizer->intern("size"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "size() takes no arguments");
        }
        return ctx.make_value(static_cast<script_int>(self.unchecked_array_node()->size()));
    }},

        {symbolizer->intern("push"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "push() takes exactly one argument");
        }

        // Get element type from array's type_info
        auto array_type_info = self.get_type_info();
        type_info_ptr element_type = array_type_info ? array_type_info->element_type() : nullptr;

        // Validate element type compatibility
        if (!detail::container_element_compatible(args[0], element_type, ctx.get_engine())) {
            return detail::element_type_error(ctx.get_string_symbolizer(), runtime_error_code::array_element_type_mismatch, "Cannot push '{0}' to array<{1}>", args[0], element_type);
        }

        // Convert element if needed (e.g., int -> float for array<float>)
        auto converted_result = detail::convert_container_element(ctx.get_engine(), args[0], element_type);
        if (!converted_result) { return converted_result.error_value(); }
        script_value converted = std::move(converted_result.value());

        // engine::memory_cap chokepoint: deny the growth before it exists
        if (engine* eng = ctx.get_engine()) {
            auto& limits = detail::engine_access::execution_limits(*eng);
            const size_t bytes = sizeof(script_value) + (converted.is_string() ? converted.unchecked_as_string().size() : 0);
            if (!limits.memory_charge(bytes)) [[unlikely]] {
                return detail::raise_memory_cap(limits);
            }
        }

        self.unchecked_get_array_storage()->push(std::move(converted));
        return ctx.make_value();
    }},

        {symbolizer->intern("resize"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.empty() || args.size() > 2) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "resize() takes one or two arguments");
        }
        auto size = array_size_argument(args[0], "resize() size must be an int");
        if (!size) { return size.error_value(); }
        if (ctx.copy_array_result) {
            script_value result = self.clone();
            auto done = resize_array(ctx, result, size.value(), args.size() == 2 ? &args[1] : nullptr);
            if (!done) { return done.error_value(); }
            return result;
        }
        return resize_array(ctx, self, size.value(), args.size() == 2 ? &args[1] : nullptr);
    }},

        {symbolizer->intern("assign"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 2) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "assign() takes exactly two arguments");
        }
        auto size = array_size_argument(args[0], "assign() size must be an int");
        if (!size) { return size.error_value(); }
        auto assigned = assign_elements(ctx, self, size.value(), args[1]);
        if (!assigned || ctx.unwinding()) { return assigned; }
        return resize_array(ctx, self, size.value(), &args[1]);
    }},

        {symbolizer->intern("fill"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "fill() takes exactly one argument");
        }
        script_value target = ctx.copy_array_result ? self.clone() : script_value(self);
        auto done = assign_elements(ctx, target, target.unchecked_array_node()->size(), args[0]);
        if (!done) { return done.error_value(); }
        return ctx.copy_array_result ? target : ctx.make_value();
    }},

        {symbolizer->intern("reserve"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "reserve() takes exactly one argument");
        }
        auto size = array_size_argument(args[0], "reserve() size must be an int");
        if (!size) { return size.error_value(); }
        const strong_ptr<script_array> storage = self.unchecked_get_array_storage();
        if (size.value() <= storage->size()) { return ctx.make_value(); }
        if (engine* eng = ctx.get_engine()) {
            // capacity is charged when elements arrive; a reserve past the cap is denied without charging
            const auto& limits = detail::engine_access::execution_limits(*eng);
            const size_t extra = size.value() - storage->size();
            if (limits.memory_limit != SIZE_MAX && extra > (limits.memory_limit - std::min(limits.memory_used, limits.memory_limit)) / sizeof(script_value)) [[unlikely]] {
                return detail::builtin_memory_cap(eng);
            }
        }
        storage->reserve(size.value());
        return ctx.make_value();
    }},

        {symbolizer->intern("pop"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "pop() takes no arguments");
        }
        auto& node = *self.unchecked_get_array_storage();
        if (node.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::array_empty), "Cannot pop from empty array");
        }
        script_value last = node.get(node.size() - 1, ctx.get_engine());
        node.pop_back();
        return last;
    }},

        {symbolizer->intern("empty"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "empty() takes no arguments");
        }
        return ctx.make_value(self.unchecked_array_node()->empty());
    }},

        {symbolizer->intern("clear"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "clear() takes no arguments");
        }
        self.unchecked_get_array_storage()->clear();
        return ctx.make_value();
    }},

        {symbolizer->intern("front"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "front() takes no arguments");
        }
        const script_array* node = self.unchecked_array_node();
        if (node->empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::array_empty), "Cannot get front of empty array");
        }
        return stored_element_result(ctx, self, 0);
    }},

        {symbolizer->intern("back"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "back() takes no arguments");
        }
        const script_array* node = self.unchecked_array_node();
        if (node->empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::array_empty), "Cannot get back of empty array");
        }
        return stored_element_result(ctx, self, node->size() - 1);
    }},

        {symbolizer->intern("index_of"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "index_of() takes exactly one argument");
        }
        auto found = detail::array_index_of_equal(ctx, self, args[0]);   // the language's ==
        if (!found) { return found.error_value(); }
        if (!found.value()) { return ctx.make_value(); }
        return ctx.make_value(*found.value());
    }},

        {symbolizer->intern("has"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "has() takes exactly one argument");
        }
        auto found = detail::array_index_of_equal(ctx, self, args[0]);   // the language's ==
        if (!found) { return found.error_value(); }
        if (!found.value()) { return ctx.make_value(); }
        return ctx.make_value(*found.value() >= 0);
    }},

        {symbolizer->intern("contains"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "contains() takes exactly one argument");
        }
        auto found = detail::array_index_of_equal(ctx, self, args[0]);   // the language's ==
        if (!found) { return found.error_value(); }
        if (!found.value()) { return ctx.make_value(); }
        return ctx.make_value(*found.value() >= 0);
    }},

        {symbolizer->intern("first"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "first() takes no arguments");
        }
        const script_array* node = self.unchecked_array_node();
        if (node->empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::array_empty), "Cannot get first of empty array");
        }
        return stored_element_result(ctx, self, 0);
    }},

        {symbolizer->intern("last"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "last() takes no arguments");
        }
        const script_array* node = self.unchecked_array_node();
        if (node->empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::array_empty), "Cannot get last of empty array");
        }
        return stored_element_result(ctx, self, node->size() - 1);
    }},

        {symbolizer->intern("length"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "length() takes no arguments");
        }
        return ctx.make_value(static_cast<script_int>(self.unchecked_array_node()->size()));
    }},

		{symbolizer->intern("slice"), [](const builtin_method_context& a_context, script_value& a_self, const std::vector<script_value>& a_args) -> checked_result<script_value> {
			if (a_args.size() > 2) {
				return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "slice() takes zero to two arguments");
			}
			if ((!a_args.empty() && !a_args[0].is_int()) || (a_args.size() == 2 && !a_args[1].is_int())) {
				return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "slice() arguments must be integers");
			}
			const script_array* node = a_self.unchecked_array_node();
			script_int start = a_args.empty() ? 0 : a_args[0].unchecked_as_int();
			script_int end = a_args.size() == 2 ? a_args[1].unchecked_as_int() : static_cast<script_int>(node->size());

			if (start < 0) {
				start = static_cast<script_int>(node->size()) + start;
			}
			if (end < 0) {
				end = static_cast<script_int>(node->size()) + end;
			}
			start = std::max<script_int>(0, std::min<script_int>(start, static_cast<script_int>(node->size())));
			end = std::max<script_int>(0, std::min<script_int>(end, static_cast<script_int>(node->size())));
			if (start > end) {
				start = end;
			}

			if (!a_context.copy_array_result) {
				a_self.unchecked_get_array_storage()->retain_range(static_cast<size_t>(start), static_cast<size_t>(end));
				return a_context.make_value();
			}
			// Preserve the receiver's declared element type in the copy.
			auto self_type = a_self.get_type_info();
			type_info_ptr element_type = self_type && self_type->base_type == script_value_type::jai_array_type ? self_type->element_type() : nullptr;
			script_value result = script_value::make_array(element_type, a_context.get_engine());
			auto& result_node = *result.unchecked_get_array_storage();
			for (script_int i = start; i < end; ++i) {
				result_node.push(node->get(static_cast<size_t>(i), a_context.get_engine()).clone_element());
			}
			return result;
		}},

		{symbolizer->intern("filter"), [](const builtin_method_context& a_context, script_value& a_self, const std::vector<script_value>& a_args) -> checked_result<script_value> {
			if (a_args.size() != 1) { return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "filter() takes exactly one argument"); }
			if (!a_args[0].is_function()) { return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "filter() requires a function argument"); }
			auto* eng = a_context.get_engine();
			const auto array = a_self.unchecked_get_array_storage();
			const element_callback callback(a_args[0], 0, a_self, eng);
			script_value result = a_context.copy_array_result ? script_value::make_array(callback.element_type, eng) : a_context.make_value();
			std::vector<uint8_t> selected;
			const size_t count = array->size();
			if (!a_context.copy_array_result) { selected.reserve(count); }
			for (size_t i = 0; i < count && i < array->size(); ++i) {
				auto answer = callback.call(array, i, nullptr, eng);
				if (!answer) { return answer.error_value(); }
				if (a_context.unwinding()) { return a_context.make_value(); }
				const bool keep = predicate_true(answer.value()) && i < array->size();
				if (a_context.copy_array_result) {
					if (keep) { result.unchecked_get_array_storage()->push(array->get(i, eng).clone_element()); }
				} else { selected.push_back(keep ? uint8_t{1} : uint8_t{0}); }
			}
			if (a_context.copy_array_result) { return result; }
			array->retain_selected(selected);
			return a_context.make_value();
		}},

		{symbolizer->intern("sort"), [](const builtin_method_context& a_context, script_value& a_self, const std::vector<script_value>& a_args) -> checked_result<script_value> {
			if (a_args.size() > 1) {
				return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "sort() takes zero or one argument");
			}
			if (!a_args.empty() && !a_args[0].is_function()) {
				return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "sort() comparator must be a function");
			}
			auto& node = *a_self.unchecked_get_array_storage();
			auto* backend = detail::engine_access::get_execution_backend(*a_context.get_engine());
			if (a_args.empty() && node.is_typed() && backend && backend->native_less_available()) {
				// Direct buffer sorting: no boxing, snapshot or script comparison calls.
				switch (node.kind()) {
					case script_array::kind_t::i64:
						std::sort(node.ints().begin(), node.ints().end()); return a_context.make_value();
					case script_array::kind_t::f64:
						if (std::any_of(node.floats().begin(), node.floats().end(), [](script_float a_value) { return std::isnan(a_value); })) {
							std::sort(node.floats().begin(), node.floats().end(), [](script_float a_left, script_float a_right) { return detail::float_order(a_left,a_right) < 0; });
						} else { std::sort(node.floats().begin(), node.floats().end()); }
						return a_context.make_value();
					case script_array::kind_t::b8:
					case script_array::kind_t::c8:
					case script_array::kind_t::u8:
						// Language chars promote to unsigned bytes (0..255) before <.
						std::sort(node.bytes().begin(), node.bytes().end()); return a_context.make_value();
					case script_array::kind_t::i8:
						std::sort(node.bytes().begin(), node.bytes().end(), [](uint8_t a_left, uint8_t a_right) {
							return static_cast<int8_t>(a_left) < static_cast<int8_t>(a_right);
						});
						return a_context.make_value();
					case script_array::kind_t::str:
						std::sort(node.strs().begin(), node.strs().end(), [](const strong_ptr<script_string>& a_left, const strong_ptr<script_string>& a_right) {
							return *a_left < *a_right;
						});
						return a_context.make_value();
					default: break;
				}
			}

			// Pin the starting receiver even if callback reentry replaces its variable.
			auto pinnedNode = a_self.unchecked_get_array_storage();
			// User comparisons operate on a snapshot: callback reentry cannot invalidate
			// std::sort's iterators. Commit the ordering only after every comparison succeeds.
			std::vector<script_value> values = node.is_typed() ? node.materialize_values(a_context.get_engine()) : node.values();
			std::optional<builtin_callback> comparator;
			if (!a_args.empty()) { comparator.emplace(a_args[0], a_context.get_engine()); }
			const builtin_callback* predicate = comparator ? &*comparator : nullptr;
			const bool hasNaN = a_args.empty() && backend && backend->native_less_available() && std::any_of(values.begin(), values.end(), [](const script_value& a_value) { return a_value.is_float() && std::isnan(a_value.unchecked_as_float()); });
			const uint32_t lvalues = predicate ? callback_lvalue_positions(a_args[0], 2) : 0;
			std::vector<script_value> arguments;
			if (predicate && !lvalues) { arguments.resize(2, a_context.make_value()); }
			if (lvalues) { arguments.reserve(2); }
			std::optional<error_propagator> error;
			struct SortAborted {};
			detail::engine_access::run_pin pin(a_context.get_engine());   // the comparisons are the builtin's callbacks
			try {
				merge_sort_any_order(values, [&](const script_value& a_left, const script_value& a_right) {
					if (!predicate) {
						if (!backend) {
							error = error_propagator{make_error_code(runtime_error_code::type_mismatch), "Invalid operands for < operator"};
							throw SortAborted{};
						}
						if (hasNaN && (a_left.is_int() || a_left.is_float()) && (a_right.is_int() || a_right.is_float())) {
							const auto left = a_left.is_int() ? static_cast<script_float>(a_left.unchecked_as_int()) : a_left.unchecked_as_float();
							const auto right = a_right.is_int() ? static_cast<script_float>(a_right.unchecked_as_int()) : a_right.unchecked_as_float();
							if (std::isnan(left) || std::isnan(right)) { return detail::float_order(left,right) < 0; }
						}
						auto result = backend->language_less(a_left, a_right);
						if (!result) { error = result.error_value(); throw SortAborted{}; }
						if (backend->is_unwinding()) { throw SortAborted{}; }
						return result.value();
					}
					checked_result<script_value> result = [&]() {
						if (lvalues) {
							return call_with_lvalues(*predicate, lvalues, {const_cast<script_value*>(&a_left), const_cast<script_value*>(&a_right)}, a_context.get_engine(), arguments);
						}
						arguments[0] = a_left;
						arguments[1] = a_right;
						return (*predicate)(arguments);
					}();
					if (!result) { error = result.error_value(); throw SortAborted{}; }
					if (backend && backend->is_unwinding()) { throw SortAborted{}; }
					if (!result.value().is_bool()) {
						error = error_propagator{make_error_code(runtime_error_code::type_mismatch), "sort() comparator must return bool"};
						throw SortAborted{};
					}
					return result.value().unchecked_as_bool();
				});
			} catch (const SortAborted&) {
				if (error) { return *error; }
				return a_context.make_value(); // preserve the backend's script throw
			}
			if (node.is_typed()) {
				node.clear();
				for (auto& value : values) { node.push(std::move(value)); }
			} else {
				node.values() = std::move(values);
			}
			return a_context.make_value();
		}},

        {symbolizer->intern("reverse"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "reverse() takes no arguments");
        }
        self.unchecked_get_array_storage()->reverse();
        return ctx.make_value();
    }},

        {symbolizer->intern("remove"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "remove() takes exactly one argument");
        }
        if (!args[0].is_int()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "remove() argument must be an integer");
        }
        auto& node = *self.unchecked_get_array_storage();
        script_int index = args[0].unchecked_as_int();

        if (index < 0 || index >= static_cast<script_int>(node.size())) {
            return ctx.make_value(false);
        }

        node.erase_at(static_cast<size_t>(index));
        return ctx.make_value(true);
    }},

        {symbolizer->intern("remove_if"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
		if (args.size() != 1) {
			return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "remove_if() takes exactly one argument");
		}
		if (!args[0].is_function()) {
			return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "remove_if() requires a function argument");
		}

		const auto storage = self.unchecked_get_array_storage();
		auto& node = *storage;
		const builtin_callback predicate(args[0], ctx.get_engine());
		const uint32_t lvalues = callback_lvalue_positions(args[0], 1);
		script_int removed_count = 0;

		// Snapshot the elements before iterating: the predicate is user script that
		// can mutate THIS array (arrays share strong_ptr storage), and a push/clear
		// would reallocate the buffer and dangle a live iterator. Evaluate against the
		// snapshot, collect survivors, then write them back.
		std::vector<script_value> snapshot = node.is_typed()
			? node.materialize_values(ctx.get_engine()) : node.values();
		std::vector<script_value> kept;
		kept.reserve(snapshot.size());
		for (auto& elem : snapshot) {
			auto call_result = lvalues ? call_with_lvalues(predicate, lvalues, {&elem}, ctx.get_engine()) : predicate({elem});
			if (!call_result) {
				return call_result.error_value();
			}
			if (ctx.unwinding()) { return ctx.make_value(); }
			if (call_result.value().is_bool() && call_result.value().unchecked_as_bool()) {
				++removed_count;          // dropped
			} else {
				kept.push_back(elem);     // survivor
			}
		}
		if (node.is_typed()) {
			node.clear();
			for (auto& v : kept) { node.push(std::move(v)); }
		} else {
			node.values() = std::move(kept);
		}
		return ctx.make_value(removed_count);
	}},


        {symbolizer->intern("join"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() > 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "join() takes 0 or 1 argument (separator)");
        }

        std::string_view sep;
        if (args.size() > 0) {
            if (!args[0].is_string()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "join() argument must be a string");
            }
            sep = args[0].unchecked_as_string();
        }

        const script_array* node = self.unchecked_array_node();
        engine* eng = ctx.get_engine();
        if (node->kind() == script_array::kind_t::str) {   // a string buffer's texts read where they are: no element is boxed
            const auto& strs = node->strs();
            size_t total = strs.size() > 1 ? sep.size() * (strs.size() - 1) : 0;
            for (const auto& h : strs) { total += h->size(); }
            if (!detail::builtin_memory_charge(eng, total)) [[unlikely]] { return detail::builtin_memory_cap(eng); }
            std::string result;
            result.reserve(total);
            for (size_t i = 0; i < strs.size(); ++i) {
                if (i) { result += sep; }
                result += *strs[i];
            }
            return ctx.make_value(std::move(result));
        }
        // one buffer: string elements by their length, everything else by a short estimate;
        // hetero elements read in place, typed kinds box once per element
        size_t total = node->size() > 1 ? sep.size() * (node->size() - 1) : 0;
        bool walked = false;
        node->for_each(eng, [&](const script_value& e) {
            total += e.is_string() ? e.unchecked_as_string().size() : 8;
            walked = walked || !detail::plain_text_value(e);
        });
        if (!detail::builtin_memory_charge(eng, total)) [[unlikely]] { return detail::builtin_memory_cap(eng); }
        std::string result;
        result.reserve(total);

        bool first = true;
        std::optional<checked_result<script_value>> stopped;   // an element's to_string() failed or threw
        auto append = [&](const script_value& elem) {
            if (stopped) { return; }
            if (!first) {
                result += sep;
            }
            first = false;
            if (elem.is_string()) {
                result += elem.unchecked_as_string();
            } else if (elem.is_int()) {
                result += std::to_string(elem.unchecked_as_int());
            } else if (elem.is_float()) {
                result += script_value::float_text(elem.unchecked_as_float());
            } else if (elem.is_bool()) {
                result += elem.unchecked_as_bool() ? "true" : "false";
            } else if (elem.is_char()) {
                result += elem.unchecked_as_char();
            } else if (elem.is_null()) {
                result += "null";
            } else if (detail::plain_text_value(elem)) {
                result += elem.to_string();
            } else if (auto text = eng ? eng->text_of(elem) : checked_result<std::string>(elem.to_string()); !text) {
                stopped.emplace(text.error_value());
            } else if (ctx.unwinding()) {
                stopped.emplace(ctx.make_value());
            } else {
                result += text.value();
            }
        };
        if (walked) {
            // a to_string() is script that may change this array: the elements are read from a snapshot
            for (const script_value& elem : node->materialize_values(eng)) { append(elem); }
        } else {
            node->for_each(eng, append);
        }
        if (stopped) { return std::move(*stopped); }

        return ctx.make_value(std::move(result));
    }},

		// Dot map retains the input T in place; array::map selects the callback's declared result type.
        {symbolizer->intern("map"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
		if (args.size() != 1) {
			return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "map() takes exactly one argument");
		}
		if (!args[0].is_function()) {
			return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "map() requires a function argument");
		}
		engine* eng = ctx.get_engine();
		const strong_ptr<script_array> array = self.unchecked_get_array_storage();
		const element_callback callback(args[0], 0, self, eng);
		type_info_ptr declared;
		callback_parameters(args[0], &declared);
		const bool typed = declared && declared->base_type != script_value_type::jai_any_type && declared->base_type != script_value_type::jai_null_type &&
						   declared->base_type != script_value_type::jai_reference_type && declared->base_type != script_value_type::jai_function_type &&
						   declared->base_type != script_value_type::jai_invalid_type && declared->base_type != script_value_type::jai_cpp_bound_type;
		type_info_ptr element_type = ctx.copy_array_result ? (typed ? declared : nullptr) : callback.element_type;
		script_value result = script_value::make_array(element_type, eng);
		const size_t count = array->size();
		for (size_t i = 0; i < count && i < array->size(); ++i) {
			auto mapped = callback.call(array, i, nullptr, eng);
			if (!mapped) { return mapped.error_value(); }
			if (ctx.unwinding()) { return ctx.make_value(); }
			script_value value = mapped.value().is_reference() ? mapped.value().deref().clone_element() : std::move(mapped.value());
			if (element_type) {
				if (!detail::container_element_compatible(value, element_type, eng)) {
					return detail::element_type_error(ctx.get_string_symbolizer(), runtime_error_code::array_element_type_mismatch, "Cannot push '{0}' to array<{1}>", value, element_type);
				}
				auto converted = detail::convert_container_element(eng, value, element_type);
				if (!converted) { return converted.error_value(); }
				value = std::move(converted.value());
			}
			result.unchecked_get_array_storage()->push(std::move(value));
		}
		if (ctx.copy_array_result) { return result; }
		// Move the completed storage, preserving the original node's live reference count.
		auto& target = *array;
		auto& output = *result.unchecked_get_array_storage();
		const uint32_t refs = target.live_elem_refs;
		target = std::move(output);
		target.live_elem_refs = refs;
		return ctx.make_value();
	}},

		// reduce(f, init): f(accumulator, element) over the elements in order, starting from init; reduce(f) starts from the first element

        {symbolizer->intern("reduce"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.empty() || args.size() > 2) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "reduce() takes a function and an optional initial value");
        }
        if (!args[0].is_function()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "reduce() requires a function argument");
        }
        engine* eng = ctx.get_engine();
        const strong_ptr<script_array> array = self.unchecked_get_array_storage();
        size_t start = 0;
        script_value accumulator = ctx.make_value();
        // the accumulator is a copy (std::accumulate takes its initial value by value): the callback never writes the
        // initial argument or the first element through it
        if (args.size() == 2) {
            accumulator = args[1].deref().clone_element();
        } else {
            if (array->empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::array_empty), "Cannot reduce an empty array without an initial value");
            }
            accumulator = array->get(0, eng).clone_element();
            start = 1;
        }
        const element_callback callback(args[0], 1, self, eng);
        const size_t count = array->size();
        for (size_t i = start; i < count && i < array->size(); ++i) {
            auto next = callback.call(array, i, &accumulator, eng);
            if (!next) { return next.error_value(); }
            if (ctx.unwinding()) { return ctx.make_value(); }
            accumulator = next.value().is_reference() ? script_value(next.value().deref()) : std::move(next.value());
        }
        return accumulator;
    }},

        // for_each(f): f(element) for every element in order (a T& parameter changes the element)
        {symbolizer->intern("for_each"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "for_each() takes exactly one argument");
        }
        if (!args[0].is_function()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "for_each() requires a function argument");
        }
        engine* eng = ctx.get_engine();
        const strong_ptr<script_array> array = self.unchecked_get_array_storage();
        const element_callback callback(args[0], 0, self, eng);
        const size_t count = array->size();
        for (size_t i = 0; i < count && i < array->size(); ++i) {
            auto done = callback.call(array, i, nullptr, eng);
            if (!done) { return done.error_value(); }
            if (ctx.unwinding()) { return ctx.make_value(); }
        }
        return ctx.make_value();
    }},

        // find(f): the first element f answers true for, or null
        {symbolizer->intern("find"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "find() takes exactly one argument");
        }
        if (!args[0].is_function()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "find() requires a function argument");
        }
        engine* eng = ctx.get_engine();
        const strong_ptr<script_array> array = self.unchecked_get_array_storage();
        const element_callback callback(args[0], 0, self, eng);
        const size_t count = array->size();
        for (size_t i = 0; i < count && i < array->size(); ++i) {
            auto answer = callback.call(array, i, nullptr, eng);
            if (!answer) { return answer.error_value(); }
            if (ctx.unwinding()) { return ctx.make_value(); }
            if (predicate_true(answer.value()) && i < array->size()) { return array->get(i, eng).clone_element(); }
        }
        return ctx.make_value();
    }},

        // find_index(f): the index of the first element f answers true for, or -1
        {symbolizer->intern("find_index"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "find_index() takes exactly one argument");
        }
        if (!args[0].is_function()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "find_index() requires a function argument");
        }
        engine* eng = ctx.get_engine();
        const strong_ptr<script_array> array = self.unchecked_get_array_storage();
        const element_callback callback(args[0], 0, self, eng);
        const size_t count = array->size();
        for (size_t i = 0; i < count && i < array->size(); ++i) {
            auto answer = callback.call(array, i, nullptr, eng);
            if (!answer) { return answer.error_value(); }
            if (ctx.unwinding()) { return ctx.make_value(); }
            if (predicate_true(answer.value())) { return ctx.make_value(static_cast<script_int>(i)); }
        }
        return ctx.make_value(script_int(-1));
    }},

        // some(f): whether f answers true for any element (false for an empty array); stops at the first
        {symbolizer->intern("some"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "some() takes exactly one argument");
        }
        if (!args[0].is_function()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "some() requires a function argument");
        }
        engine* eng = ctx.get_engine();
        const strong_ptr<script_array> array = self.unchecked_get_array_storage();
        const element_callback callback(args[0], 0, self, eng);
        const size_t count = array->size();
        for (size_t i = 0; i < count && i < array->size(); ++i) {
            auto answer = callback.call(array, i, nullptr, eng);
            if (!answer) { return answer.error_value(); }
            if (ctx.unwinding()) { return ctx.make_value(); }
            if (predicate_true(answer.value())) { return ctx.make_value(true); }
        }
        return ctx.make_value(false);
    }},

        // every(f): whether f answers true for every element (true for an empty array); stops at the first that is not
        {symbolizer->intern("every"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "every() takes exactly one argument");
        }
        if (!args[0].is_function()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "every() requires a function argument");
        }
        engine* eng = ctx.get_engine();
        const strong_ptr<script_array> array = self.unchecked_get_array_storage();
        const element_callback callback(args[0], 0, self, eng);
        const size_t count = array->size();
        for (size_t i = 0; i < count && i < array->size(); ++i) {
            auto answer = callback.call(array, i, nullptr, eng);
            if (!answer) { return answer.error_value(); }
            if (ctx.unwinding()) { return ctx.make_value(); }
            if (!predicate_true(answer.value())) { return ctx.make_value(false); }
        }
        return ctx.make_value(true);
    }},

        // concat(other): a new array, this one's elements then other's (copies), with this array's element type
		{symbolizer->intern("concat"), [](const builtin_method_context& a_context, script_value& a_self, const std::vector<script_value>& a_args) -> checked_result<script_value> {
			if (a_args.size() != 1) { return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "append/concat() takes one array argument"); }
			const auto& other = a_args[0].deref();
			if (!other.is_array()) { return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "append/concat() requires an array argument"); }
			auto* eng = a_context.get_engine();
			if (!detail::builtin_memory_charge(eng, other.unchecked_get_array_storage()->size(), sizeof(script_value))) [[unlikely]] {
				return detail::builtin_memory_cap(eng);
			}
			script_value result = a_context.copy_array_result ? a_self.clone() : script_value(a_self);
			const auto target = result.unchecked_get_array_storage();
			const auto input = other.unchecked_get_array_storage();
			if (target->append_same_kind(*input)) { return a_context.copy_array_result ? result : a_context.make_value(); }
			auto tag = result.get_type_info();
			type_info_ptr type = tag ? tag->element_type() : nullptr;
			if (type && (type->base_type == script_value_type::jai_any_type || type->base_type == script_value_type::jai_null_type)) { type = nullptr; }
			std::vector<script_value> values;
			const size_t count = input->size();
			values.reserve(count);
			for (size_t i = 0; i < count && i < input->size(); ++i) {
				script_value value = input->get(i, eng).clone_element();
				if (type) {
					if (!detail::container_element_compatible(value, type, eng)) {
						return detail::element_type_error(a_context.get_string_symbolizer(), runtime_error_code::array_element_type_mismatch, "Cannot push '{0}' to array<{1}>", value, type);
					}
					auto converted = detail::convert_container_element(eng, value, type);
					if (!converted) { return converted.error_value(); }
					value = std::move(converted.value());
				}
				values.push_back(std::move(value));
			}
			target->reserve(target->size() + values.size());
			for (auto& value : values) { target->push(std::move(value)); }
			return a_context.copy_array_result ? result : a_context.make_value();
		}},

        {symbolizer->intern("insert"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 2) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "insert() takes an index and a value");
        }
        if (!args[0].is_int()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "insert() index must be an integer");
        }
        engine* eng = ctx.get_engine();
        auto& node = *self.unchecked_get_array_storage();
        const script_int index = args[0].unchecked_as_int();
        if (index < 0 || index > static_cast<script_int>(node.size())) {
            return detail::index_out_of_bounds_error(ctx.get_string_symbolizer(), "Cannot insert at index {0} of an array of size {1}", "Cannot insert outside the array", index, node.size());
        }
        auto array_type_info = self.get_type_info();
        type_info_ptr element_type = array_type_info ? array_type_info->element_type() : nullptr;
        if (!detail::container_element_compatible(args[1], element_type, eng)) {
            return detail::element_type_error(ctx.get_string_symbolizer(), runtime_error_code::array_element_type_mismatch, "Cannot insert '{0}' into array<{1}>", args[1], element_type);
        }
        auto converted = detail::convert_container_element(eng, args[1], element_type);
        if (!converted) { return converted.error_value(); }
        if (!detail::engine_access::execution_limits(*eng).memory_charge(sizeof(script_value))) [[unlikely]] {
            return detail::raise_memory_cap(detail::engine_access::execution_limits(*eng));
        }
        if (!node.is_typed()) {
            node.values().insert(node.values().begin() + static_cast<std::ptrdiff_t>(index), std::move(converted.value()));
            return ctx.make_value();
        }
        // a typed buffer: push, then rotate the new last element down to index
        node.push(std::move(converted.value()));
        for (size_t at = node.size() - 1; at > static_cast<size_t>(index); --at) {
            script_value below = node.get(at - 1, eng);
            script_value above = node.get(at, eng);
            node.set(at, std::move(below));
            node.set(at - 1, std::move(above));
        }
        return ctx.make_value();
    }}
    };

	// Index-removal spelling shared with the existing remove implementation.
	out.array_methods.emplace(symbolizer->intern("erase"), out.array_methods.at(symbolizer->intern("remove")));
	out.array_methods.emplace(symbolizer->intern("append"), out.array_methods.at(symbolizer->intern("concat")));

    // Map methods
    out.map_methods = {
        {symbolizer->intern("size"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "size() takes no arguments");
        }
        return ctx.make_value(static_cast<script_int>(self.unchecked_as_map().size()));
    }},

        {symbolizer->intern("empty"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "empty() takes no arguments");
        }
        return ctx.make_value(self.unchecked_as_map().empty());
    }},

        {symbolizer->intern("clear"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "clear() takes no arguments");
        }
        auto& mapPtr = self.unchecked_get_map_storage();
        mapPtr->clear();
        return ctx.make_value();
    }},

        {symbolizer->intern("contains"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "contains() takes exactly one argument");
        }
        const auto& map = self.unchecked_as_map();
        return ctx.make_value(map.find(args[0]) != map.end());
    }},

        {symbolizer->intern("keys"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "keys() takes no arguments");
        }
        const auto& map = self.unchecked_as_map();
        script_value result = script_value::make_array(nullptr, ctx.get_engine());
        auto& arrayPtr = result.unchecked_get_array_storage()->values();
        arrayPtr.reserve(map.size());
        for (const auto& [key, value] : map) {
            arrayPtr.push_back(key.clone_element());
        }
        return result;
    }},

        {symbolizer->intern("values"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "values() takes no arguments");
        }
        const auto& map = self.unchecked_as_map();
        script_value result = script_value::make_array(nullptr, ctx.get_engine());
        auto& arrayPtr = result.unchecked_get_array_storage()->values();
        arrayPtr.reserve(map.size());
        for (const auto& [key, value] : map) {
            arrayPtr.push_back(value.clone_element());
        }
        return result;
    }},

        {symbolizer->intern("has"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "has() takes exactly one argument");
        }
        const auto& map = self.unchecked_as_map();
        return ctx.make_value(map.find(args[0]) != map.end());
    }},

        {symbolizer->intern("get"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.empty() || args.size() > 2) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "get() takes one or two arguments (key, [default])");
        }
        const auto& map = self.unchecked_as_map();
        auto it = map.find(args[0]);
        if (it != map.end()) {
            if (!stored_result_is_heavy(it->second)) { return it->second; }
            const auto tag = self.get_type_info();
            return script_value::make_map_entry_reference(self.unchecked_get_map_storage(), it->first, &it->second, ctx.get_engine(), tag ? tag->value_type() : nullptr);
        }
        if (args.size() < 2) { return ctx.make_value(); }  // Missing keys never insert
        if (!stored_result_is_heavy(args[1])) { return args[1]; }
        return script_value::make_cell_reference(script_value(args[1].deref()), ctx.get_engine());   // the default argument, named
    }},

        {symbolizer->intern("length"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "length() takes no arguments");
        }
        return ctx.make_value(static_cast<script_int>(self.unchecked_as_map().size()));
    }},

        {symbolizer->intern("remove"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "remove() takes exactly one argument");
        }
        auto& mapPtr = self.unchecked_get_map_storage();
        auto it = mapPtr->find(args[0]);
        if (it != mapPtr->end()) {
            mapPtr->erase(it);
            return ctx.make_value(true);
        }
        return ctx.make_value(false);
    }},

        {symbolizer->intern("remove_if"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "remove_if() takes exactly one argument");
        }
        if (!args[0].is_function()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "remove_if() requires a function argument");
        }

        auto& mapPtr = self.unchecked_get_map_storage();
        const builtin_callback predicate(args[0], ctx.get_engine());
        const uint32_t lvalues = callback_lvalue_positions(args[0], 2);
        script_int removed_count = 0;

        // the predicate is script that may write this map: walk a snapshot of the keys, never a live iterator
        const auto storage = mapPtr;
        std::vector<script_value> keys;
        keys.reserve(storage->size());
        for (const auto& [key, value] : *storage) { keys.push_back(key); }
        for (const auto& key : keys) {
            auto found = storage->find(key);
            if (found == storage->end()) { continue; }
            auto call_result = lvalues ? call_map_entry(predicate, lvalues, self, key, ctx.get_engine()) : predicate({key, found->second});
            if (!call_result) {
                return call_result.error_value();
            }
            if (ctx.unwinding()) { return ctx.make_value(); }
            const auto& val = call_result.value();
            if (val.is_bool() && val.unchecked_as_bool()) {
                auto current = storage->find(key);
                if (current != storage->end()) { storage->erase(current); ++removed_count; }
            }
        }
        return ctx.make_value(removed_count);
    }},

        {symbolizer->intern("filter"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "filter() takes exactly one argument");
        }
        if (!args[0].is_function()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "filter() requires a function argument");
        }

        // the dot form keeps the entries the predicate accepts and returns the receiver; map::filter returns a copy
        script_value receiver = self;
        const auto source = receiver.unchecked_get_map_storage();
        const auto& map = *source;
        const builtin_callback predicate(args[0], ctx.get_engine());
        const uint32_t lvalues = callback_lvalue_positions(args[0], 2);
        auto tag = self.get_type_info();
        script_value result = ctx.copy_array_result
            ? script_value::make_map(tag ? tag->key_type() : nullptr, tag ? tag->value_type() : nullptr, ctx.get_engine())
            : ctx.make_value();

        std::vector<script_value> keys;
        keys.reserve(map.size());
        for (const auto& [key, value] : map) { keys.push_back(key); }
        std::vector<script_value> rejected;
        for (const auto& key : keys) {
            auto found = map.find(key);
            if (found == map.end()) { continue; }
            auto call_result = lvalues ? call_map_entry(predicate, lvalues, receiver, key, ctx.get_engine()) : predicate({key, found->second});
            if (!call_result) {
                return call_result.error_value();
            }
            if (ctx.unwinding()) { return ctx.make_value(); }
            const auto& val = call_result.value();
            const bool keep = val.is_bool() && val.unchecked_as_bool();
            if (!ctx.copy_array_result) {
                if (!keep) { rejected.push_back(key); }
            } else if (keep) {
                auto current = map.find(key);
                if (current != map.end()) { (*result.unchecked_get_map_storage())[key.clone_element()] = current->second.clone_element(); }
            }
        }
        if (ctx.copy_array_result) { return result; }
        for (const auto& key : rejected) {
            auto it = source->find(key);
            if (it != source->end()) { source->erase(it); }
        }
        return ctx.make_value();
    }},

        {symbolizer->intern("to_array"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "to_array() takes no arguments");
        }
        const auto& map = self.unchecked_as_map();
        script_value result = script_value::make_array(nullptr, ctx.get_engine());
        auto& arrayPtr = result.unchecked_get_array_storage()->values();
        arrayPtr.reserve(map.size());

        // Return array of [key, value] pairs
        for (const auto& [key, value] : map) {
            script_value pair = script_value::make_array(nullptr, ctx.get_engine());
            auto& pairPtr = pair.unchecked_get_array_storage()->values();
            pairPtr.push_back(key.clone_element());
            pairPtr.push_back(value.clone_element());
            arrayPtr.push_back(std::move(pair));
        }
        return result;
    }}
    };

	// Both map removal spellings report whether an entry existed.
	out.map_methods.emplace(symbolizer->intern("erase"), out.map_methods.at(symbolizer->intern("remove")));

    // String methods - enable str.length(), str.substr(), etc.
    // Observer methods return values; mutating methods modify in place and return nothing
    out.string_methods = {
		{symbolizer->intern("append"), [](const builtin_method_context& a_context, script_value& a_self, const std::vector<script_value>& a_args) -> checked_result<script_value> {
			if (a_args.empty()) { return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "append() requires at least one text argument"); }
			size_t extra = 0;
			for (const auto& argument : a_args) {
				if (!argument.is_text()) { return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "append() requires text arguments"); }
				extra += argument.unchecked_text().size();
			}
			if (!detail::builtin_memory_charge(a_context.get_engine(), extra)) [[unlikely]] { return detail::builtin_memory_cap(a_context.get_engine()); }
			auto& text = a_self.unchecked_text_ref(a_context.get_engine());
			text.reserve(text.size() + extra);
			for (const auto& argument : a_args) { text.append(argument.unchecked_text()); }
			return a_context.make_value();
		}},
        // ============================================================
        // Observer methods (non-mutating)
        // ============================================================

        {symbolizer->intern("length"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (!args.empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "length() takes no arguments");
            }
            return ctx.make_value(static_cast<script_int>(self.unchecked_text().size()));
        }},

        {symbolizer->intern("size"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (!args.empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "size() takes no arguments");
            }
            return ctx.make_value(static_cast<script_int>(self.unchecked_text().size()));
        }},

        {symbolizer->intern("empty"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (!args.empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "empty() takes no arguments");
            }
            return ctx.make_value(self.unchecked_text().empty());
        }},

        {symbolizer->intern("at"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 1) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "at() takes exactly one argument");
            }
            if (!args[0].is_int()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "at() argument must be an integer");
            }
            const auto& str = self.unchecked_text();
            script_int idx = args[0].unchecked_as_int();
            script_int len = static_cast<script_int>(str.size());

            // Normalize negative index
            if (idx < 0) idx += len;

            // Throw if out of bounds
            if (idx < 0 || idx >= len) {
                return checked_result<script_value>(make_error_code(runtime_error_code::index_out_of_bounds), "at() index out of bounds");
            }

            // A byte char, like []: inline value, no temporary string or allocation.
            return ctx.make_value(str[static_cast<size_t>(idx)]);
        }},

        {symbolizer->intern("front"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (!args.empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "front() takes no arguments");
            }
            const auto& str = self.unchecked_text();
            if (str.empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::index_out_of_bounds), "front() called on empty string");
            }
            return ctx.make_value(str.front());
        }},

        {symbolizer->intern("back"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (!args.empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "back() takes no arguments");
            }
            const auto& str = self.unchecked_text();
            if (str.empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::index_out_of_bounds), "back() called on empty string");
            }
            return ctx.make_value(str.back());
        }},

        {symbolizer->intern("substr"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() < 1 || args.size() > 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "substr() takes 1 or 2 arguments (start, [length])");
            }
            if (!args[0].is_int()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "substr() start argument must be an integer");
            }
            const auto& str = self.unchecked_text();
            script_int start = args[0].unchecked_as_int();
            script_int len = static_cast<script_int>(str.size());

            // Handle negative start index
            if (start < 0) start += len;
            if (start < 0) start = 0;
            if (start >= len) {
                return ctx.make_value(std::string(""));
            }

            if (args.size() == 2) {
                if (!args[1].is_int()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "substr() length argument must be an integer");
                }
                script_int sub_len = args[1].unchecked_as_int();
                if (sub_len < 0) sub_len = 0;
                return ctx.make_value(str.substr(static_cast<size_t>(start), static_cast<size_t>(sub_len)));
            }
            return ctx.make_value(str.substr(static_cast<size_t>(start)));
        }},

        // ============================================================
        // Search methods
        // ============================================================

        {symbolizer->intern("find"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() < 1 || args.size() > 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "find() takes 1 or 2 arguments (substr, [start])");
            }
            if (!args[0].is_text()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "find() first argument must be a string");
            }
            const auto& str = self.unchecked_text();
            const auto& search = args[0].unchecked_text();
            script_int len = static_cast<script_int>(str.size());

            size_t start_pos = 0;
            if (args.size() == 2) {
                if (!args[1].is_int()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "find() second argument must be an integer");
                }
                script_int start = args[1].unchecked_as_int();
                if (start < 0) start += len;
                if (start < 0) start = 0;
                start_pos = static_cast<size_t>(start);
            }

            auto pos = str.find(search, start_pos);
            if (pos == std::string::npos) {
                return ctx.make_value(static_cast<script_int>(-1));
            }
            return ctx.make_value(static_cast<script_int>(pos));
        }},

        {symbolizer->intern("rfind"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() < 1 || args.size() > 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "rfind() takes 1 or 2 arguments (substr, [start])");
            }
            if (!args[0].is_text()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "rfind() first argument must be a string");
            }
            const auto& str = self.unchecked_text();
            const auto& search = args[0].unchecked_text();
            script_int len = static_cast<script_int>(str.size());

            size_t start_pos = std::string::npos;
            if (args.size() == 2) {
                if (!args[1].is_int()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "rfind() second argument must be an integer");
                }
                script_int start = args[1].unchecked_as_int();
                if (start < 0) start += len;
                if (start < 0) {
                    return ctx.make_value(static_cast<script_int>(-1));
                }
                start_pos = static_cast<size_t>(start);
            }

            auto pos = str.rfind(search, start_pos);
            if (pos == std::string::npos) {
                return ctx.make_value(static_cast<script_int>(-1));
            }
            return ctx.make_value(static_cast<script_int>(pos));
        }},

        {symbolizer->intern("find_first_of"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() < 1 || args.size() > 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "find_first_of() takes 1 or 2 arguments (chars, [start])");
            }
            if (!args[0].is_text()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "find_first_of() first argument must be a string");
            }
            const auto& str = self.unchecked_text();
            const auto& chars = args[0].unchecked_text();
            script_int len = static_cast<script_int>(str.size());

            size_t start_pos = 0;
            if (args.size() == 2) {
                if (!args[1].is_int()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "find_first_of() second argument must be an integer");
                }
                script_int start = args[1].unchecked_as_int();
                if (start < 0) start += len;
                if (start < 0) start = 0;
                start_pos = static_cast<size_t>(start);
            }

            auto pos = str.find_first_of(chars, start_pos);
            if (pos == std::string::npos) {
                return ctx.make_value(static_cast<script_int>(-1));
            }
            return ctx.make_value(static_cast<script_int>(pos));
        }},

        {symbolizer->intern("find_last_of"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() < 1 || args.size() > 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "find_last_of() takes 1 or 2 arguments (chars, [start])");
            }
            if (!args[0].is_text()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "find_last_of() first argument must be a string");
            }
            const auto& str = self.unchecked_text();
            const auto& chars = args[0].unchecked_text();
            script_int len = static_cast<script_int>(str.size());

            size_t start_pos = std::string::npos;
            if (args.size() == 2) {
                if (!args[1].is_int()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "find_last_of() second argument must be an integer");
                }
                script_int start = args[1].unchecked_as_int();
                if (start < 0) start += len;
                if (start < 0) {
                    return ctx.make_value(static_cast<script_int>(-1));
                }
                start_pos = static_cast<size_t>(start);
            }

            auto pos = str.find_last_of(chars, start_pos);
            if (pos == std::string::npos) {
                return ctx.make_value(static_cast<script_int>(-1));
            }
            return ctx.make_value(static_cast<script_int>(pos));
        }},

        {symbolizer->intern("find_first_not_of"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() < 1 || args.size() > 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "find_first_not_of() takes 1 or 2 arguments (chars, [start])");
            }
            if (!args[0].is_text()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "find_first_not_of() first argument must be a string");
            }
            const auto& str = self.unchecked_text();
            const auto& chars = args[0].unchecked_text();
            script_int len = static_cast<script_int>(str.size());

            size_t start_pos = 0;
            if (args.size() == 2) {
                if (!args[1].is_int()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "find_first_not_of() second argument must be an integer");
                }
                script_int start = args[1].unchecked_as_int();
                if (start < 0) start += len;
                if (start < 0) start = 0;
                start_pos = static_cast<size_t>(start);
            }

            auto pos = str.find_first_not_of(chars, start_pos);
            if (pos == std::string::npos) {
                return ctx.make_value(static_cast<script_int>(-1));
            }
            return ctx.make_value(static_cast<script_int>(pos));
        }},

        {symbolizer->intern("find_last_not_of"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() < 1 || args.size() > 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "find_last_not_of() takes 1 or 2 arguments (chars, [start])");
            }
            if (!args[0].is_text()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "find_last_not_of() first argument must be a string");
            }
            const auto& str = self.unchecked_text();
            const auto& chars = args[0].unchecked_text();
            script_int len = static_cast<script_int>(str.size());

            size_t start_pos = std::string::npos;
            if (args.size() == 2) {
                if (!args[1].is_int()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "find_last_not_of() second argument must be an integer");
                }
                script_int start = args[1].unchecked_as_int();
                if (start < 0) start += len;
                if (start < 0) {
                    return ctx.make_value(static_cast<script_int>(-1));
                }
                start_pos = static_cast<size_t>(start);
            }

            auto pos = str.find_last_not_of(chars, start_pos);
            if (pos == std::string::npos) {
                return ctx.make_value(static_cast<script_int>(-1));
            }
            return ctx.make_value(static_cast<script_int>(pos));
        }},

        {symbolizer->intern("contains"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 1) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "contains() takes exactly one argument");
            }
            if (!args[0].is_text()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "contains() argument must be a string");
            }
            const auto& str = self.unchecked_text();
            const auto& search = args[0].unchecked_text();
            return ctx.make_value(str.find(search) != std::string::npos);
        }},

        {symbolizer->intern("starts_with"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 1) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "starts_with() takes exactly one argument");
            }
            if (!args[0].is_text()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "starts_with() argument must be a string");
            }
            const auto& str = self.unchecked_text();
            const auto& prefix = args[0].unchecked_text();
            if (prefix.size() > str.size()) {
                return ctx.make_value(false);
            }
            return ctx.make_value(str.compare(0, prefix.size(), prefix) == 0);
        }},

        {symbolizer->intern("ends_with"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 1) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "ends_with() takes exactly one argument");
            }
            if (!args[0].is_text()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "ends_with() argument must be a string");
            }
            const auto& str = self.unchecked_text();
            const auto& suffix = args[0].unchecked_text();
            if (suffix.size() > str.size()) {
                return ctx.make_value(false);
            }
            return ctx.make_value(str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0);
        }},

        {symbolizer->intern("count"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 1) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "count() takes exactly one argument");
            }
            if (!args[0].is_text()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "count() argument must be a string");
            }
            const auto& str = self.unchecked_text();
            const auto& search = args[0].unchecked_text();

            if (search.empty()) {
                return ctx.make_value(static_cast<script_int>(0));
            }

            script_int count = 0;
            size_t pos = 0;
            while ((pos = str.find(search, pos)) != std::string::npos) {
                ++count;
                pos += search.size();  // Non-overlapping
            }
            return ctx.make_value(count);
        }},

        // ============================================================
        // Parsing methods
        // ============================================================

        {symbolizer->intern("to_int"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            // to_int(default=null, base=10)
            script_value default_val(std::monostate{}, ctx.get_engine());
            int base = 10;

            if (args.size() > 0) {
                default_val = args[0];
            }
            if (args.size() > 1) {
                if (!args[1].is_int()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "to_int() base argument must be an integer");
                }
                base = static_cast<int>(args[1].unchecked_as_int());
                if (base != 0 && (base < 2 || base > 36)) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::invalid_operation), "to_int() base must be 0 or between 2 and 36");
                }
            }
            if (args.size() > 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "to_int() takes at most 2 arguments (default, base)");
            }

            const auto& str = self.unchecked_text();
            try {
                size_t pos = 0;
                long long val = std::stoll(str, &pos, base);
                // Check if entire string was consumed (ignoring trailing whitespace).
                // isspace requires an unsigned-char value (or EOF); a negative char
                // (bytes >= 0x80 in UTF-8) is undefined behavior.
                while (pos < str.size() && std::isspace(static_cast<unsigned char>(str[pos]))) ++pos;
                if (pos != str.size()) {
                    return default_val;
                }
                return ctx.make_value(static_cast<script_int>(val));
            } catch (...) {
                return default_val;
            }
        }},

        {symbolizer->intern("to_float"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            // to_float(default=null)
            script_value default_val(std::monostate{}, ctx.get_engine());

            if (args.size() > 0) {
                default_val = args[0];
            }
            if (args.size() > 1) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "to_float() takes at most 1 argument (default)");
            }

            const auto parsed = detail::parse_complete_float_text(self.unchecked_text());
            return parsed ? ctx.make_value(*parsed) : default_val;
        }},

        // ============================================================
        // Mutating methods (modify in place, return nothing)
        // ============================================================

        {symbolizer->intern("to_lower"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (!args.empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "to_lower() takes no arguments");
            }
            auto& str = self.unchecked_text_ref(ctx.get_engine());
            for (auto& c : str) {
                if (c >= 'A' && c <= 'Z') {
                    c = c + ('a' - 'A');
                }
            }
            return ctx.make_value();
        }},

        {symbolizer->intern("to_upper"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (!args.empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "to_upper() takes no arguments");
            }
            auto& str = self.unchecked_text_ref(ctx.get_engine());
            for (auto& c : str) {
                if (c >= 'a' && c <= 'z') {
                    c = c - ('a' - 'A');
                }
            }
            return ctx.make_value();
        }},

        {symbolizer->intern("trim"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            std::string_view chars = k_whitespace_text;
            if (args.size() > 0) {
                if (!args[0].is_text()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "trim() argument must be a string");
                }
                chars = args[0].unchecked_text();
            }
            if (args.size() > 1) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "trim() takes at most 1 argument");
            }

            auto& str = self.unchecked_text_ref(ctx.get_engine());
            size_t start = str.find_first_not_of(chars);
            if (start == std::string::npos) {
                str.clear();
            } else {
                size_t end = str.find_last_not_of(chars);
                str = str.substr(start, end - start + 1);
            }
            return ctx.make_value();
        }},

        {symbolizer->intern("trim_left"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            std::string_view chars = k_whitespace_text;
            if (args.size() > 0) {
                if (!args[0].is_text()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "trim_left() argument must be a string");
                }
                chars = args[0].unchecked_text();
            }
            if (args.size() > 1) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "trim_left() takes at most 1 argument");
            }

            auto& str = self.unchecked_text_ref(ctx.get_engine());
            size_t start = str.find_first_not_of(chars);
            if (start == std::string::npos) {
                str.clear();
            } else {
                str.erase(0, start);
            }
            return ctx.make_value();
        }},

        {symbolizer->intern("trim_right"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            std::string_view chars = k_whitespace_text;
            if (args.size() > 0) {
                if (!args[0].is_text()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "trim_right() argument must be a string");
                }
                chars = args[0].unchecked_text();
            }
            if (args.size() > 1) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "trim_right() takes at most 1 argument");
            }

            auto& str = self.unchecked_text_ref(ctx.get_engine());
            size_t end = str.find_last_not_of(chars);
            if (end == std::string::npos) {
                str.clear();
            } else {
                str.erase(end + 1);
            }
            return ctx.make_value();
        }},

        {symbolizer->intern("pad_left"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            static constexpr pad_texts texts{"pad_left() takes 1 or 2 arguments (target_len, [char])", "pad_left() first argument must be an integer",
                                            "pad_left() padding character cannot be empty", "pad_left() second argument must be a char or string"};
            return pad_text(ctx, self, args, -1, texts);
        }},

        {symbolizer->intern("pad_right"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            static constexpr pad_texts texts{"pad_right() takes 1 or 2 arguments (target_len, [char])", "pad_right() first argument must be an integer",
                                            "pad_right() padding character cannot be empty", "pad_right() second argument must be a char or string"};
            return pad_text(ctx, self, args, 1, texts);
        }},

        {symbolizer->intern("pad_center"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            static constexpr pad_texts texts{"pad_center() takes 1 or 2 arguments (target_len, [char])", "pad_center() first argument must be an integer",
                                            "pad_center() padding character cannot be empty", "pad_center() second argument must be a char or string"};
            return pad_text(ctx, self, args, 0, texts);
        }},

        {symbolizer->intern("replace_first"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "replace_first() takes exactly 2 arguments (old, new)");
            }
            if (!args[0].is_text() || !args[1].is_text()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "replace_first() arguments must be strings");
            }
            const auto& old_str = args[0].unchecked_text();
            const auto& new_str = args[1].unchecked_text();

            if (old_str.empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::invalid_operation), "replace_first() search string cannot be empty");
            }

            auto& str = self.unchecked_text_ref(ctx.get_engine());
            size_t pos = str.find(old_str);
            if (pos != std::string::npos) {
                str.replace(pos, old_str.size(), new_str);
            }
            return ctx.make_value();
        }},

        {symbolizer->intern("replace_last"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "replace_last() takes exactly 2 arguments (old, new)");
            }
            if (!args[0].is_text() || !args[1].is_text()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "replace_last() arguments must be strings");
            }
            const auto& old_str = args[0].unchecked_text();
            const auto& new_str = args[1].unchecked_text();

            if (old_str.empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::invalid_operation), "replace_last() search string cannot be empty");
            }

            auto& str = self.unchecked_text_ref(ctx.get_engine());
            size_t pos = str.rfind(old_str);
            if (pos != std::string::npos) {
                str.replace(pos, old_str.size(), new_str);
            }
            return ctx.make_value();
        }},

        {symbolizer->intern("replace"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "replace() takes exactly 2 arguments (old, new)");
            }
            if (!args[0].is_text() || !args[1].is_text()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "replace() arguments must be strings");
            }
            const auto& old_str = args[0].unchecked_text();
            const auto& new_str = args[1].unchecked_text();

            if (old_str.empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::invalid_operation), "replace() search string cannot be empty");
            }

            // One pass builds the result: replacing in place shifts the tail at every hit, quadratic in the hits
            auto& str = self.unchecked_text_ref(ctx.get_engine());
            size_t hits = 0;
            for (size_t at = str.find(old_str); at != std::string::npos; at = str.find(old_str, at + old_str.size())) { ++hits; }
            if (hits == 0) { return ctx.make_value(); }
            if (new_str.size() > old_str.size() && !detail::builtin_memory_charge(ctx.get_engine(), hits, new_str.size() - old_str.size())) [[unlikely]] {
                return detail::builtin_memory_cap(ctx.get_engine());
            }
            std::string out;
            out.reserve(str.size() - hits * old_str.size() + hits * new_str.size());
            size_t at = 0;
            for (size_t hit = str.find(old_str); hit != std::string::npos; hit = str.find(old_str, at)) {
                out.append(str, at, hit - at);
                out += new_str;
                at = hit + old_str.size();
            }
            out.append(str, at, std::string::npos);
            str = std::move(out);
            return ctx.make_value();
        }},

        {symbolizer->intern("insert"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "insert() takes exactly 2 arguments (pos, text)");
            }
            if (!args[0].is_int()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "insert() first argument must be an integer");
            }
            if (!args[1].is_text()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "insert() second argument must be a string");
            }

            auto& str = self.unchecked_text_ref(ctx.get_engine());
            script_int pos = args[0].unchecked_as_int();
            const auto& text = args[1].unchecked_text();
            script_int len = static_cast<script_int>(str.size());

            // Normalize negative index
            if (pos < 0) pos += len;
            // Clamp to valid range
            if (pos < 0) pos = 0;
            if (pos > len) pos = len;

            // engine::memory_cap chokepoint: deny the growth before it exists
            if (engine* eng = ctx.get_engine()) {
                auto& limits = detail::engine_access::execution_limits(*eng);
                if (!limits.memory_charge(text.size())) [[unlikely]] {
                    return detail::raise_memory_cap(limits);
                }
            }

            str.insert(static_cast<size_t>(pos), text);
            return ctx.make_value();
        }},

        {symbolizer->intern("erase"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() < 1 || args.size() > 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "erase() takes 1 or 2 arguments (pos, [count])");
            }
            if (!args[0].is_int()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "erase() first argument must be an integer");
            }

            auto& str = self.unchecked_text_ref(ctx.get_engine());
            script_int pos = args[0].unchecked_as_int();
            script_int len = static_cast<script_int>(str.size());

            // Normalize negative index
            if (pos < 0) pos += len;
            if (pos < 0 || pos >= len) {
                return ctx.make_value();  // Out of bounds, no change
            }

            size_t count = std::string::npos;
            if (args.size() > 1) {
                if (!args[1].is_int()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "erase() second argument must be an integer");
                }
                script_int cnt = args[1].unchecked_as_int();
                if (cnt < 0) cnt = 0;
                count = static_cast<size_t>(cnt);
            }

            str.erase(static_cast<size_t>(pos), count);
            return ctx.make_value();
        }},

        {symbolizer->intern("remove_prefix"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 1) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "remove_prefix() takes exactly one argument");
            }
            if (!args[0].is_int()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "remove_prefix() argument must be an integer");
            }

            auto& str = self.unchecked_text_ref(ctx.get_engine());
            script_int n = args[0].unchecked_as_int();
            if (n < 0) n = 0;
            if (n > 0) {
                size_t to_remove = std::min(static_cast<size_t>(n), str.size());
                str.erase(0, to_remove);
            }
            return ctx.make_value();
        }},

        {symbolizer->intern("remove_suffix"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 1) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "remove_suffix() takes exactly one argument");
            }
            if (!args[0].is_int()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "remove_suffix() argument must be an integer");
            }

            auto& str = self.unchecked_text_ref(ctx.get_engine());
            script_int n = args[0].unchecked_as_int();
            if (n < 0) n = 0;
            if (n > 0) {
                size_t to_remove = std::min(static_cast<size_t>(n), str.size());
                str.erase(str.size() - to_remove);
            }
            return ctx.make_value();
        }},

        {symbolizer->intern("reverse"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (!args.empty()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "reverse() takes no arguments");
            }
            auto& str = self.unchecked_text_ref(ctx.get_engine());
            std::reverse(str.begin(), str.end());
            return ctx.make_value();
        }},

        {symbolizer->intern("repeat"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() != 1) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "repeat() takes exactly one argument");
            }
            if (!args[0].is_int()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "repeat() argument must be an integer");
            }

            script_int count = args[0].unchecked_as_int();
            if (count < 0) {
                return checked_result<script_value>(make_error_code(runtime_error_code::invalid_operation), "string::repeat count cannot be negative");
            }
            if (count == 0) {
                self.unchecked_text_ref(ctx.get_engine()).clear();
                return ctx.make_value();
            }

            auto& str = self.unchecked_text_ref(ctx.get_engine());
            // engine::memory_cap chokepoint: deny the growth before it exists
            if (count > 1 && !detail::builtin_memory_charge(ctx.get_engine(), str.size(), detail::saturating_count(count - 1))) [[unlikely]] {
                return detail::builtin_memory_cap(ctx.get_engine());
            }
            std::string original = str;
            str.clear();
            str.reserve(detail::saturating_text_size(original.size(), detail::saturating_count(count)));
            for (script_int i = 0; i < count; ++i) {
                str += original;
            }
            return ctx.make_value();
        }},

        // ============================================================
        // Split method (returns array)
        // ============================================================

        {symbolizer->intern("split"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
            if (args.size() > 2) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "split() takes 0 to 2 arguments (delimiter, [max_splits])");
            }
            script_int max_splits = -1;   // -1: no limit; the last element keeps the rest
            if (args.size() > 1) {
                if (!args[1].is_int()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "split() second argument must be an integer");
                }
                max_splits = args[1].unchecked_as_int();
            }

            std::string_view delim;
            bool whitespace = args.empty();
            if (args.size() > 0) {
                if (!args[0].is_text()) {
                    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "split() argument must be a string");
                }
                delim = args[0].unchecked_text();
                whitespace = delim == k_whitespace_text;
            }

            const auto& str = self.unchecked_text();

            // Create array with string element type
            auto eng = ctx.get_engine();
            if (!eng) {
                return checked_result<script_value>(make_error_code(runtime_error_code::internal_error), "Engine reference expired");
            }

            script_value array_val = script_value::make_array(detail::engine_access::get_type_info_string(*eng), eng);
            script_array& result = *array_val.unchecked_get_array_storage();   // array<string> is a handle buffer: push through the node
            // each piece is the array's growth (engine::memory_cap): a denied piece is never added
            auto& limits = detail::engine_access::execution_limits(*eng);
            auto push_piece = [&](size_t a_at, size_t a_count) {
                const size_t length = std::min(a_count, str.size() - a_at);
                if (!limits.memory_charge(sizeof(script_value) + length)) [[unlikely]] { return false; }
                result.strs().push_back(make_strong<script_string>(str, a_at, length));   // the piece's handle, as push stores a string
                return true;
            };

            if (whitespace) {
                // runs of whitespace separate the pieces and no piece is empty; past max_splits the rest is the last piece
                size_t at = 0;
                script_int splits = 0;
                while (true) {
                    while (at < str.size() && is_string_whitespace(str[at])) { ++at; }
                    if (at == str.size()) { break; }
                    if (max_splits >= 0 && splits == max_splits) {
                        if (!push_piece(at, std::string::npos)) [[unlikely]] { return detail::builtin_memory_cap(eng); }
                        break;
                    }
                    size_t end = at;
                    while (end < str.size() && !is_string_whitespace(str[end])) { ++end; }
                    if (!push_piece(at, end - at)) [[unlikely]] { return detail::builtin_memory_cap(eng); }
                    at = end;
                    ++splits;
                }
            } else if (delim.empty()) {
                // Split into individual characters
                for (size_t at = 0; at < str.size(); ++at) {
                    if (!push_piece(at, 1)) [[unlikely]] { return detail::builtin_memory_cap(eng); }
                }
            } else {
                size_t pieces = 1;   // counted first: one sized buffer
                for (size_t at = str.find(delim); at != std::string::npos && (max_splits < 0 || static_cast<script_int>(pieces) <= max_splits); at = str.find(delim, at + delim.size())) { ++pieces; }
                result.reserve(pieces);
                size_t start = 0;
                size_t end;
                script_int splits = 0;
                while ((max_splits < 0 || splits < max_splits) && (end = str.find(delim, start)) != std::string::npos) {
                    if (!push_piece(start, end - start)) [[unlikely]] { return detail::builtin_memory_cap(eng); }
                    start = end + delim.size();
                    ++splits;
                }
                if (!push_piece(start, std::string::npos)) [[unlikely]] { return detail::builtin_memory_cap(eng); }
            }

            return array_val;
        }}
    };

    // Weak pointer methods
    out.weak_ptr_methods = {
        {symbolizer->intern("lock"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "lock() takes no arguments");
        }

        if (!self.is_weak_ptr()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "lock() can only be called on weak_ptr");
        }

        auto weak_ptr = self.get_weak_ptr();
        if (auto locked = weak_ptr.lock()) {
            // Reconstruct a script_value from the locked object_holder
            // IMPORTANT: Reuse the same std::shared_ptr<object_holder> to maintain reference semantics
            script_value result(std::monostate{}, ctx.get_engine());

            // Preserve the original type info (including shared_ptr marker if present)
            // the handle type: a declared weak_ptr<T> names T itself
            auto weak_type_info = self.get_type_info();
            if (weak_type_info && weak_type_info->element_type() && weak_type_info->element_type()->base_type == script_value_type::jai_object_type) {
                result.set_type_info(type_info_ptr{detail::engine_access::get_type_info_shared_ptr(*ctx.get_engine(), weak_type_info->element_type().get())});
            } else if (weak_type_info && weak_type_info->element_type()) {
                result.set_type_info(weak_type_info->element_type());
            } else {
                // Fallback: use the object type
                if (auto eng = ctx.get_engine()) {
                    result.set_type_info(detail::engine_access::get_type_info_object(*eng, locked->type_id()));
                }
            }

            // Directly assign the locked shared_ptr
            // Works for both regular objects and shared_ptr<T> since they use the same storage
            result.set_object_holder(locked);

            return result;
        } else {
            // weak_ptr is expired, return null
            return ctx.make_value();
        }
    }},

        {symbolizer->intern("expired"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "expired() takes no arguments");
        }

        if (!self.is_weak_ptr()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "expired() can only be called on weak_ptr");
        }

        auto weak_ptr = self.get_weak_ptr();
        return ctx.make_value(weak_ptr.expired());
    }},

        {symbolizer->intern("reset"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "reset() takes no arguments");
        }

        if (!self.is_weak_ptr()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "reset() can only be called on weak_ptr");
        }

        // Reset the weak_ptr to null
        auto& weak_storage = self.get_weak_ptr_storage();
        weak_storage.reset();

        return ctx.make_value();
    }},

        {symbolizer->intern("same_as"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "same_as() takes exactly 1 argument");
        }
        return ctx.make_value(detail::same_object(self, args[0]));
    }}
    };

    // Shared pointer methods
    out.shared_ptr_methods = {
        {symbolizer->intern("reset"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "reset() takes no arguments");
        }

        // In JaiScript, all objects are internally shared_ptr<object_holder>
        // Reset it to null while preserving the shared_ptr type
        auto current_type_info = self.get_type_info();
        self = ctx.make_value();
        self.set_type_info(current_type_info); // Preserve the shared_ptr<T> type

        return ctx.make_value();
    }},

        // use_count() reports the OUTER
        // strong_ptr<object_holder> count — the layer script alias copies bump and the
        // layer script weak_ptr expiry follows — so `shared_ptr<T> a = T()` reports 1
        // and `var b = a;` makes it 2. The builtin's own call path holds transient
        // receiver copies (bound-method capture + this local), so the raw count is
        // aliases + K with K constant per backend (interp binds one fewer stack copy
        // than the vm); the offset below is pinned by the alias-ladder parity tests.
        // The inner std::shared_ptr count (C++-side shares) is cpp_ref_count().
        {symbolizer->intern("use_count"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "use_count() takes no arguments");
        }

        if (self.is_object()) {
            auto obj_holder = self.get_object_holder();
            if (obj_holder && obj_holder->holds_object()) {
                const long transient_k =
                    (ctx.engine_ref && ctx.engine_ref->get_backend_type() == backend_type::vm) ? 3 : 2;
                long count = static_cast<long>(obj_holder.use_count()) - transient_k;
                if (count < 1) count = 1;   // the receiver itself always counts
                return ctx.make_value(static_cast<script_int>(count));
            }
        }

        // Not a valid shared_ptr
        return ctx.make_value(static_cast<script_int>(0));
    }},

        // The inner std::shared_ptr count: how many C++-side owners share
        // the object (as<std::shared_ptr<T>>() / get_class_instance() extractions)
        {symbolizer->intern("cpp_ref_count"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "cpp_ref_count() takes no arguments");
        }

        if (self.is_object()) {
            auto obj_holder = self.get_object_holder();
            if (obj_holder && obj_holder->holds_object()) {
                if (obj_holder->is_instance()) {   // an instance's one count is every owner's, script handles and C++ shares alike
                    const long transient_k =
                        (ctx.engine_ref && ctx.engine_ref->get_backend_type() == backend_type::vm) ? 3 : 2;
                    return ctx.make_value(static_cast<script_int>(std::max(1L, static_cast<long>(obj_holder.use_count()) - transient_k)));
                }
                return ctx.make_value(static_cast<script_int>(obj_holder->object_use_count()));
            }
        }

        return ctx.make_value(static_cast<script_int>(0));
    }},

        {symbolizer->intern("unique"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "unique() takes no arguments");
        }

        if (self.is_object()) {
            auto obj_holder = self.get_object_holder();
            if (obj_holder && obj_holder->holds_object()) {
                // unique = exactly one script handle (mirrors the ruled use_count layer)
                const long transient_k =
                    (ctx.engine_ref && ctx.engine_ref->get_backend_type() == backend_type::vm) ? 3 : 2;
                long count = static_cast<long>(obj_holder.use_count()) - transient_k;
                return ctx.make_value(count <= 1);
            }
        }

        // Not a valid shared_ptr
        return ctx.make_value(false);
    }},

        {symbolizer->intern("same_as"), [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "same_as() takes exactly 1 argument");
        }
        return ctx.make_value(detail::same_object(self, args[0]));
    }}
    };

    auto build_index = [symbolizer](const std::unordered_map<uint64_t, builtin_method>& methods,
                                    std::vector<const builtin_method*>& by_index,
                                    std::unordered_map<uint64_t, uint16_t>& index) {
        by_index.clear();
        index.clear();
        std::vector<std::pair<std::string_view, uint64_t>> names;
        names.reserve(methods.size());
        for (const auto& [id, fn] : methods) {
            names.emplace_back(symbolizer->get_string(id), id);
        }
        std::sort(names.begin(), names.end());
        by_index.reserve(names.size());
        for (const auto& [name, id] : names) {
            index.emplace(id, static_cast<uint16_t>(by_index.size()));
            by_index.push_back(&methods.at(id));
        }
    };
    detail::add_more_string_methods(out.string_methods, symbolizer);
    detail::add_binary_string_methods(out.string_methods, symbolizer);
    out.string_methods[symbolizer->intern("explode")] = out.string_methods[symbolizer->intern("split")];
    out.string_methods[symbolizer->intern("to_utf8")] = [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> checked_result<script_value> {
        if (!args.empty()) return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "to_utf8() takes no arguments");
        return script_value::make_utf8(self.unchecked_text(), ctx.get_engine());
    };
    out.utf8_methods = detail::make_utf8_methods(out.string_methods, symbolizer);
    out.utf8_methods.erase(symbolizer->intern("to_utf8"));
    build_index(out.array_methods, out.array_by_index, out.array_index);
    build_index(out.map_methods, out.map_by_index, out.map_index);
    build_index(out.string_methods, out.string_by_index, out.string_index);
    out.utf8_by_index.assign(out.string_by_index.size(), nullptr);   // aligned: a baked bi_string serves both kinds
    out.utf8_index.clear();
    for (const auto& [id, at] : out.string_index) {
        auto it = out.utf8_methods.find(id);
        if (it != out.utf8_methods.end()) { out.utf8_by_index[at] = &it->second; out.utf8_index.emplace(id, at); }
    }
    out.string_mutator_ids.clear();
    out.in_place.string.clear();
    out.string_mutates_by_index.assign(out.string_by_index.size(), 0);
    for (std::string_view name : k_string_mutator_method_names) {
        const uint64_t id = symbolizer->intern(name);
        assert(out.string_methods.count(id) && "k_string_mutator_method_names names a builtin the registry lacks");
        out.string_mutator_ids.insert(id);
        out.string_mutates_by_index[out.string_index.at(id)] = 1;
        out.in_place.string[id] = *in_place_method_twin(2, name);
    }
    out.in_place.array.clear();
    out.in_place.map.clear();
    for (const auto& m : k_array_in_place_methods) {
        assert(out.array_methods.count(symbolizer->intern(m.name)) && "k_array_in_place_methods names a builtin the registry lacks");
        out.in_place.array[symbolizer->intern(m.name)] = m.twin;
    }
    for (const auto& m : k_map_in_place_methods) {
        assert(out.map_methods.count(symbolizer->intern(m.name)) && "k_map_in_place_methods names a builtin the registry lacks");
        out.in_place.map[symbolizer->intern(m.name)] = m.twin;
    }
    out.in_place.reset = symbolizer->intern("reset");
}

namespace detail {

script_value same_as_method(const script_value& self, engine* eng) {
    script_function same_as = [eng, captured = self](const std::vector<script_value>& args) -> checked_result<script_value> {
        if (args.size() != 1) {
            return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "same_as() takes exactly 1 argument");
        }
        return script_value(same_object(captured, args[0]), eng);
    };
    return script_value::make_function(std::move(same_as), eng);
}

} // namespace detail

} // namespace jai
