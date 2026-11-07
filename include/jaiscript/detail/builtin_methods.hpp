#pragma once

#ifndef __JAISCRIPT_DETAIL_BUILTIN_METHODS_HPP__
#define __JAISCRIPT_DETAIL_BUILTIN_METHODS_HPP__

#include <jaiscript/core/value.hpp>
#include <jaiscript/detail/execution_limits.hpp>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>
#include <optional>
#include <string_view>

namespace jai {

    class string_symbolizer;
    class class_definition;

    // shared_ptr HANDLE builtin method names that a user class method now SHADOWS and
    // WINS over. same_as is deliberately EXCLUDED: it is a UNIVERSAL
    // identity primitive available on every value (and excluded from the fast method-call
    // gate), not a per-class-overridable handle method. weak_ptr's lock/expired never
    // collide here - they dispatch on weak_ptr VALUES, which are not user class instances.
    inline constexpr std::array<std::string_view, 4> k_shadowable_handle_method_names = {
        std::string_view{"reset"}, std::string_view{"use_count"},
        std::string_view{"unique"}, std::string_view{"cpp_ref_count"}
    };

    // The string builtins that write their receiver in place and return nothing
    // (C++ `void to_upper()`). ONE list feeds the registry's mutator set and index table;
    // both backends bind the receiver LVALUE for exactly these names
    // (detail/string_mutator_receiver.hpp) - a name missing here mutates a copy and the
    // write is lost, a name wrongly here costs an observer a receiver resolve.
    inline constexpr std::array<std::string_view, 18> k_string_mutator_method_names = {
        std::string_view{"append"}, std::string_view{"to_lower"}, std::string_view{"to_upper"}, std::string_view{"trim"},
        std::string_view{"trim_left"}, std::string_view{"trim_right"}, std::string_view{"pad_left"},
        std::string_view{"pad_right"}, std::string_view{"pad_center"}, std::string_view{"replace_first"},
        std::string_view{"replace_last"}, std::string_view{"replace"}, std::string_view{"insert"},
        std::string_view{"erase"}, std::string_view{"remove_prefix"}, std::string_view{"remove_suffix"},
        std::string_view{"reverse"}, std::string_view{"repeat"}
    };

    // The builtins that change their receiver in place and return nothing, each with the namespace function that returns a
    // copy instead ('@' stands for the receiver as written; empty: none). A call whose result is used raises
    // (detail/in_place_result.hpp). The string table names the twins of k_string_mutator_method_names, which are all in place.
    struct in_place_method { std::string_view name; std::string_view twin; };
    inline constexpr std::array<in_place_method, 14> k_array_in_place_methods = {{
        {"append", "array::append(@, other) returns a joined copy"}, {"assign", {}}, {"clear", {}},
        {"concat", "array::concat(@, other) returns a joined copy"}, {"fill", "array::fill(@, value) returns a filled copy"},
        {"filter", "array::filter(@, f) returns a filtered copy"},
        {"insert", {}}, {"map", "array::map(@, f) returns a mapped copy"}, {"push", {}}, {"reserve", {}},
        {"resize", "array::resize(@, n) returns a resized copy"},
        {"reverse", "array::reverse(@) returns a reversed copy"}, {"slice", "array::slice(@, start, end) returns a copy of the range"},
        {"sort", "array::sort(@) returns a sorted copy"}
    }};
    inline constexpr std::array<in_place_method, 2> k_map_in_place_methods = {{
        {"clear", {}}, {"filter", "map::filter(@, f) returns a filtered copy"}
    }};
    inline constexpr std::array<in_place_method, 8> k_string_in_place_twins = {{
        {"append", "string::append(@, text) returns a joined copy"}, {"pad_left", "string::pad_left(@, width) returns a padded copy"},
        {"pad_right", "string::pad_right(@, width) returns a padded copy"}, {"repeat", "string::repeat(@, n) returns a repeated copy"},
        {"replace", "string::replace(@, from, to) returns a replaced copy"}, {"to_lower", "string::to_lower(@) returns a lowercased copy"},
        {"to_upper", "string::to_upper(@) returns an uppercased copy"}, {"trim", "string::trim(@) returns a trimmed copy"}
    }};
    // The twin of the in-place builtin `a_name` of a receiver kind (0 array, 1 map, 2 string or utf8_string; empty: none), or
    // nullopt when that kind's builtin of the name is not in place
    inline std::optional<std::string_view> in_place_method_twin(int a_kind, std::string_view a_name) noexcept {
        if (a_kind == 0) { for (const auto& m : k_array_in_place_methods) { if (m.name == a_name) { return m.twin; } } }
        if (a_kind == 1) { for (const auto& m : k_map_in_place_methods) { if (m.name == a_name) { return m.twin; } } }
        if (a_kind == 2) {
            for (std::string_view m : k_string_mutator_method_names) {
                if (m != a_name) { continue; }
                for (const auto& t : k_string_in_place_twins) { if (t.name == a_name) { return t.twin; } }
                return std::string_view{};
            }
        }
        return std::nullopt;
    }
    // a name some receiver kind's in-place builtin answers to (a map's are an array's names; a handle's reset): the compile-time gate
    inline bool is_in_place_method_name(std::string_view a_name) noexcept {
        for (const auto& m : k_array_in_place_methods) { if (m.name == a_name) { return true; } }
        for (std::string_view m : k_string_mutator_method_names) { if (m == a_name) { return true; } }
        return a_name == "reset";
    }

    // The whitespace the string library classifies (trim, split's no-separator form): string::whitespace spells it, and
    // split/explode read exactly this text as "any run of whitespace" rather than a literal separator
    inline constexpr std::string_view k_whitespace_text = " \t\n\v\f\r";
    inline constexpr bool is_string_whitespace(char c) noexcept { return c == ' ' || (c >= '\t' && c <= '\r'); }

    // Backend-neutral invocation context for the builtin container/string/pointer methods
    struct builtin_method_context {
        engine* engine_ref = nullptr;
        string_symbolizer* symbolizer = nullptr;
        // the calling backend's language == and its unwinding flag (a user operator== may throw)
        checked_result<bool> (*equal_fn)(void*, const script_value&, const script_value&) = nullptr;
        bool (*unwinding_fn)(void*) = nullptr;
        void* backend_self = nullptr;
		bool copy_array_result = false; // namespace transforms create a result; dot forms mutate

        engine* get_engine() const { return engine_ref; }
        string_symbolizer* get_string_symbolizer() const { return symbolizer; }
        checked_result<bool> language_equal(const script_value& a, const script_value& b) const {
            if (!equal_fn) { return a == b; }   // a host-built context without a backend: identity
            return equal_fn(backend_self, a, b);
        }
        bool unwinding() const { return unwinding_fn && unwinding_fn(backend_self); }

        script_value make_value() const { return script_value(std::monostate{}, engine_ref); }
        script_value make_value(script_int v) const { return script_value(v, engine_ref); }
        script_value make_value(script_float v) const { return script_value(v, engine_ref); }
        script_value make_value(script_bool v) const { return script_value(v, engine_ref); }
        script_value make_value(script_char v) const { return script_value(v, engine_ref); }
        script_value make_value(const script_string& v) const { return script_value(v, engine_ref); }
        script_value make_value(script_string&& v) const { return script_value(std::move(v), engine_ref); }
    };

    using builtin_method = std::function<checked_result<script_value>(const builtin_method_context&, script_value&, const std::vector<script_value>&)>;

    namespace detail {
        // engine::memory_cap chokepoint for a builtin that grows a value by count items of `each` bytes: false when the
        // growth is denied (nothing was charged; the caller raises builtin_memory_cap). Templates so the engine is complete
        // where they are used.
        template<typename Engine>
        bool builtin_memory_charge(Engine* a_engine, size_t a_count, size_t a_each = 1) {
            return !a_engine || engine_access::execution_limits(*a_engine).memory_charge_items(a_count, a_each);
        }
        template<typename Engine>
        error_propagator builtin_memory_cap(Engine* a_engine) {
            return raise_memory_cap(engine_access::execution_limits(*a_engine));
        }
        // count copies of a text of `size` bytes, saturated: a reserve past max_size reports like any oversized text
        inline size_t saturating_text_size(size_t a_size, size_t a_count) noexcept {
            return a_size != 0 && a_count > SIZE_MAX / a_size ? SIZE_MAX : a_size * a_count;
        }
        // a non-negative script count as a size, saturated where size_t is narrower than script_int (a 32-bit host)
        inline size_t saturating_count(script_int a_count) noexcept {
            return static_cast<uint64_t>(a_count) > SIZE_MAX ? SIZE_MAX : static_cast<size_t>(a_count);
        }
    }

    namespace detail {
        // Same-kind scalars answer without the backend; everything else is the language's == (a user operator==, memberwise)
        inline std::optional<bool> quick_language_equal(const script_value& a, const script_value& b) {
            const size_t ka = a.raw_storage_index(), kb = b.raw_storage_index();
            if (ka == script_value::TYPEID_INT && kb == script_value::TYPEID_INT) { return a.unchecked_as_int() == b.unchecked_as_int(); }
            if (ka == script_value::TYPEID_STRING && kb == script_value::TYPEID_STRING) { return a.unchecked_as_string() == b.unchecked_as_string(); }
            if (ka == script_value::TYPEID_FLOAT && kb == script_value::TYPEID_FLOAT) { return a.unchecked_as_float() == b.unchecked_as_float(); }
            if (ka == script_value::TYPEID_BOOL && kb == script_value::TYPEID_BOOL) { return a.unchecked_as_bool() == b.unchecked_as_bool(); }
            return std::nullopt;
        }
        // The first index whose element == probe (the language's ==), -1 when none; nullopt when a user operator== threw
        inline checked_result<std::optional<script_int>> array_index_of_equal(const builtin_method_context& ctx, const script_value& self, const script_value& probe_in) {
            const script_array* node = self.unchecked_array_node();
            const script_value& probe = probe_in.deref();
            for (size_t i = 0; i < node->size(); ++i) {
                const script_value elem = node->get(i, ctx.get_engine());
                const script_value& e = elem.deref();
                bool equal = false;
                if (auto quick = quick_language_equal(e, probe)) { equal = *quick; }
                else {
                    auto r = ctx.language_equal(e, probe);
                    if (!r) { return r.error_value(); }
                    if (ctx.unwinding()) { return std::optional<script_int>{}; }
                    equal = r.value();
                }
                if (equal) { return std::optional<script_int>{static_cast<script_int>(i)}; }
            }
            return std::optional<script_int>{script_int(-1)};
        }
    }

    struct builtin_method_registries {
        std::unordered_map<uint64_t, builtin_method> array_methods;
        std::unordered_map<uint64_t, builtin_method> map_methods;
        std::unordered_map<uint64_t, builtin_method> string_methods;
        std::unordered_map<uint64_t, builtin_method> utf8_methods;
        std::unordered_map<uint64_t, builtin_method> weak_ptr_methods;
        std::unordered_map<uint64_t, builtin_method> shared_ptr_methods;

        // Dense dispatch tables, NAME-SORTED so indices are identical for every engine
        // of the same binary (symbol ids are engine-local; names are not) — call sites
        // bake indices into chunks that worker backends read through their own
        // registries. The name→index maps are consulted at COMPILE time only; dispatch
        // is one indexed load. Pointers into the maps are node-stable after init.
        static constexpr uint16_t k_no_builtin = 0xFFFF;
        std::vector<const builtin_method*> array_by_index;
        std::vector<const builtin_method*> map_by_index;
        std::vector<const builtin_method*> string_by_index;
        std::vector<const builtin_method*> utf8_by_index;   // utf8_string's methods: the string library in characters
        std::unordered_map<uint64_t, uint16_t> array_index;
        std::unordered_map<uint64_t, uint16_t> map_index;
        std::unordered_map<uint64_t, uint16_t> string_index;
        std::unordered_map<uint64_t, uint16_t> utf8_index;
        // k_string_mutator_method_names by symbol id and by string_by_index position
        std::unordered_set<uint64_t> string_mutator_ids;
        std::vector<uint8_t> string_mutates_by_index;
        // the in-place builtins by receiver kind and symbol id, each to its twin text (k_array_in_place_methods ...)
        struct in_place_ids {
            std::unordered_map<uint64_t, std::string_view> array;
            std::unordered_map<uint64_t, std::string_view> map;
            std::unordered_map<uint64_t, std::string_view> string;
            uint64_t reset = 0;
        } in_place;
    };

    // ONE implementation shared by every backend (defined in interpreter.cpp)
    void init_builtin_method_registries(string_symbolizer* symbolizer, builtin_method_registries& out);

    namespace detail {
        // v.same_as(other) bound to v: one argument; nulls, scalars and strings compare by value (a bound value decoded),
        // objects by identity, anything else is false. Both backends mint it.
        script_value same_as_method(const script_value& self, engine* eng);

        // Builtin args are BY-VALUE. A subscript read in argument position mints an
        // owner-pinned element/map-entry reference (transient_read.hpp leaves call args
        // unmarked so ref params can bind), and script/native parameter binding derefs
        // it - but builtins receive the raw args vector, where a reference-typed key
        // never matches map.find (operator<=> doesn't deref) and unchecked_as_string
        // misreads the holder. Normalize at EVERY builtin dispatch boundary, one shared
        // spelling for both backends, so builtins see what a hoisted temp would.
        inline void deref_builtin_args_in_place(std::vector<script_value>& args) {
            for (auto& arg : args) {
                if (arg.is_reference()) [[unlikely]] {
                    script_value resolved(arg.deref());
                    arg = std::move(resolved);
                }
            }
        }
        inline const std::vector<script_value>& deref_builtin_args(
                const std::vector<script_value>& args, std::vector<script_value>& scratch) {
            bool needs_resolve = false;
            for (const auto& arg : args) {
                if (arg.is_reference()) { needs_resolve = true; break; }
            }
            if (!needs_resolve) [[likely]] {
                return args;
            }
            scratch.reserve(args.size());
            for (const auto& arg : args) {
                scratch.emplace_back(arg.is_reference() ? script_value(arg.deref()) : arg);
            }
            return scratch;
        }
    }

} // namespace jai

#endif // __JAISCRIPT_DETAIL_BUILTIN_METHODS_HPP__
