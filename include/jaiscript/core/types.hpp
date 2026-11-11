#pragma once

#ifndef __JAISCRIPT_CORE_TYPES_HPP__
#define __JAISCRIPT_CORE_TYPES_HPP__

#include <jaiscript/jaiscript_fwd.hpp>
#include <jaiscript/core/checked_result.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <functional>
#include <optional>
#include <stdexcept>

namespace jai {

    // Forward declarations
    class script_value;
    template<typename T> class strong_ptr;   // handle buffers (script_array::strs_) name it before value.hpp
    class engine;

    // Fixed-size primitive types for cross-platform serialization
    using script_int = int64_t;      // Always 64-bit signed
    using script_float = double;     // Always 64-bit double precision
    using script_string = std::string;
    // utf8_string's node: the bytes (UTF-8 text) and the character-position caches its character operations keep: whether every
    // byte is ASCII (then characters are bytes), the last two (character, byte) positions resolved (walks and two-cursor loops step
    // from them) and, for long text indexed at random, the byte offset of every 64th character. A mutation clears them; only the
    // engine's main thread writes them (parallel workers read shared text without caching)
    struct script_utf8 {
        std::string bytes;
        mutable int8_t ascii = -1;                 // -1 unknown, 0 no, 1 yes
        mutable uint8_t hint_next = 0;             // the slot the next remembered position replaces
        mutable uint32_t hint_char[2] = {0, 0};    // remembered positions: character index -> byte offset (0 -> 0 always holds)
        mutable uint32_t hint_byte[2] = {0, 0};
        mutable std::vector<uint32_t> crumbs;      // byte offset of character 64 * k (empty until a long text is indexed at random)
        mutable int64_t chars = -1;                // the character count, -1 unknown
        script_utf8() = default;
        explicit script_utf8(std::string b) : bytes(std::move(b)) {}
        void changed() const noexcept { ascii = -1; chars = -1; hint_next = 0; hint_char[0] = hint_char[1] = 0; hint_byte[0] = hint_byte[1] = 0; crumbs.clear(); }
    };
    // The node behind every math vector (math::vec2 / vec3 / vec4 / ivec2 / ivec3) and math::color: the component count, the
    // element type (binary32, or int32 when integral), whether the four channels are a color's r, g, b, a, and the component
    // words; shared on copy, unshared on the first component write
    struct script_vec_node {
        uint8_t count = 0;
        uint8_t integral = 0;
        uint8_t color = 0;
        uint32_t words[4] = {0, 0, 0, 0};
    };
    using script_char = char;       // 8-bit character
    using script_bool = bool;       // 1 byte boolean
    
    // Container types. script_array is the ONE array node behind TYPEID_ARRAY (the
    // strong_ptr pointee — its address is the array's identity for ordering, borrows,
    // and COW probes). Typed kinds store raw
    // primitive buffers instead of boxed script_values; the airtight array<T> tag
    // and demote-on-stamp keep node kind ⟺ view tag. values() is the HETERO
    // element surface only — every caller either sits behind a kind check or handles
    // typed nodes through get()/set()/typed buffers (kind-dispatch helpers live in
    // value.hpp where script_value is complete).
    // Object slot of a class-typed buffer: the handle plus the element's own class tag
    // (per-slot, so a subclass element costs no layout
    // change). Defined in value.hpp where script_value::object_holder is complete.
    struct obj_slot;

    class alignas(8) script_array {
    public:
        // Live references pinned into this array's elements (make_element_reference /
        // ~reference_holder). Zero lets a parallel fork prove exclusivity of a value-semantic
        // typed object array without walking the elements.
        uint32_t live_elem_refs = 0;
        // b8/c8/u8/i8 share one byte buffer (bool, char, uint8 and int8 payloads); the kind says how to box
        // str holds shared string handles (the same handle a boxed element would carry)
        // obj holds {object handle, class tag} slots (array<UserType>)
        // v2..rgba are the math vector kinds on the byte buffer too: each element is its components' words (binary32, or
        // int32 for ivec) at the kind's stride; kind + 8 is the vector's storage index (TYPEID_VEC2 .. TYPEID_COLOR)
        enum class kind_t : uint8_t { hetero = 0, i64 = 1, f64 = 2, b8 = 3, c8 = 4, str = 5, obj = 6, u8 = 7, i8 = 8,
                                      v2 = 9, iv2 = 10, v3 = 11, iv3 = 12, v4 = 13, rgba = 14 };
        static constexpr bool is_vec_kind(kind_t k) noexcept { return k >= kind_t::v2 && k <= kind_t::rgba; }
        static constexpr size_t vec_stride(kind_t k) noexcept { return k <= kind_t::iv2 ? 2 : k <= kind_t::iv3 ? 3 : 4; }
        static constexpr bool vec_integral(kind_t k) noexcept { return k == kind_t::iv2 || k == kind_t::iv3; }
        static constexpr size_t vec_storage_index(kind_t k) noexcept { return static_cast<size_t>(k) + 8; }
        static constexpr kind_t vec_kind_of(script_value_type t) noexcept {
            switch (t) {
                case script_value_type::jai_vec2_type: return kind_t::v2;
                case script_value_type::jai_ivec2_type: return kind_t::iv2;
                case script_value_type::jai_vec3_type: return kind_t::v3;
                case script_value_type::jai_ivec3_type: return kind_t::iv3;
                case script_value_type::jai_vec4_type: return kind_t::v4;
                case script_value_type::jai_color_type: return kind_t::rgba;
                default: return kind_t::hetero;
            }
        }
        static constexpr script_value_type vec_value_type(kind_t k) noexcept {
            switch (k) {
                case kind_t::v2: return script_value_type::jai_vec2_type;
                case kind_t::iv2: return script_value_type::jai_ivec2_type;
                case kind_t::v3: return script_value_type::jai_vec3_type;
                case kind_t::iv3: return script_value_type::jai_ivec3_type;
                case kind_t::v4: return script_value_type::jai_vec4_type;
                default: return script_value_type::jai_color_type;
            }
        }

        // Every body that instantiates a vector<script_value> member (including the
        // DEFAULTED special members - construction paths instantiate the vector dtor
        // for unwind cleanup) lives in value.hpp where script_value is complete: MSVC
        // defers those instantiations, Clang/GCC perform them here per the standard.
        script_array();
        explicit script_array(kind_t k);
        script_array(const script_array&);
        script_array(script_array&&) noexcept;
        script_array& operator=(const script_array&);
        script_array& operator=(script_array&&) noexcept;
        ~script_array();

        kind_t kind() const noexcept { return kind_; }
        bool is_typed() const noexcept { return kind_ != kind_t::hetero; }

        size_t size() const noexcept;
        bool empty() const noexcept { return size() == 0; }
        void clear() noexcept;
        void reserve(size_t n);
        inline size_t capacity() const noexcept;
        inline bool append_same_kind(const script_array& a_source);
		inline void retain_range(size_t a_start, size_t a_end);
		inline void retain_selected(const std::vector<uint8_t>& a_selected);
        inline void truncate(size_t n);   // drop elements past n (no default construction)

        // One buffer per node: the kind names the live lane, and only that lane may be touched
        std::vector<script_value>& values() noexcept { assert(lane_of(kind_) == lane_t::values); return values_; }
        const std::vector<script_value>& values() const noexcept { assert(lane_of(kind_) == lane_t::values); return values_; }
        std::vector<script_int>& ints() noexcept { assert(kind_ == kind_t::i64); return ints_; }
        const std::vector<script_int>& ints() const noexcept { assert(kind_ == kind_t::i64); return ints_; }
        std::vector<script_float>& floats() noexcept { assert(kind_ == kind_t::f64); return floats_; }
        const std::vector<script_float>& floats() const noexcept { assert(kind_ == kind_t::f64); return floats_; }
        std::vector<uint8_t>& bytes() noexcept { assert(lane_of(kind_) == lane_t::bytes); return bytes_; }
        const std::vector<uint8_t>& bytes() const noexcept { assert(lane_of(kind_) == lane_t::bytes); return bytes_; }
        std::vector<strong_ptr<script_string>>& strs() noexcept { assert(kind_ == kind_t::str); return strs_; }
        const std::vector<strong_ptr<script_string>>& strs() const noexcept { assert(kind_ == kind_t::str); return strs_; }
        std::vector<obj_slot>& objs() noexcept { assert(kind_ == kind_t::obj); return objs_; }
        const std::vector<obj_slot>& objs() const noexcept { assert(kind_ == kind_t::obj); return objs_; }
        // A vector kind's element i: its component words (count = vec_stride) in and out of the byte buffer
        void vec_words(size_t i, uint32_t* out) const noexcept {
            const size_t n = vec_stride(kind_);
            std::memcpy(out, bytes_.data() + i * n * 4, n * 4);
        }
        void set_vec_words(size_t i, const uint32_t* in) noexcept {
            const size_t n = vec_stride(kind_);
            std::memcpy(bytes_.data() + i * n * 4, in, n * 4);
        }
        uint32_t vec_word(size_t i, size_t c) const noexcept {
            uint32_t w;
            std::memcpy(&w, bytes_.data() + (i * vec_stride(kind_) + c) * 4, 4);
            return w;
        }
        void set_vec_word(size_t i, size_t c, uint32_t w) noexcept { std::memcpy(bytes_.data() + (i * vec_stride(kind_) + c) * 4, &w, 4); }
        void push_vec_words(const uint32_t* in) {
            const size_t n = vec_stride(kind_) * 4;
            const size_t at = bytes_.size();
            bytes_.resize(at + n);
            std::memcpy(bytes_.data() + at, in, n);
        }
        // Same-kind buffer copy (clone / detach): one vector assignment whatever the kind
        inline void assign_buffer(const script_array& other);

        // Kind-dispatched element access — defined in value.hpp (script_value complete
        // there; the element-touching vector ops instantiate at definition, so every
        // body that destroys/moves elements lives there too). get materializes;
        // set/push take a PRE-CONVERTED value (the caller ran element-type conversion;
        // typed kinds unbox, hetero stores as-is).
        inline void pop_back();
        inline void erase_at(size_t i);
        inline void reverse();
        inline void swap_elements(size_t i, size_t j);
        inline script_value get(size_t i, engine* eng) const;
        // A boxed view of slot i that already holds the slot's handle (obj/str kinds): a by-ref
        // range-for touches its element several times per iteration, and only a replaced slot
        // needs the view rebuilt. Scalar kinds answer false (their rebuild is a plain store).
        inline bool view_current(size_t i, const script_value& view) const noexcept;
        inline void set(size_t i, script_value v);
        inline void push(script_value pre_converted);
        // Boxed snapshot of every element (typed builtin paths that need a hetero
        // vector: comparator sorts, predicate scans) — defined in value.hpp.
        inline std::vector<script_value> materialize_values(engine* eng) const;
        // Kind-generic element visit: hetero elements by const reference (no copy), typed
        // elements as boxed temporaries - the one loop every boundary reader uses so a
        // typed node never has to demote just to be read.
        template <typename F> inline void for_each(engine* eng, F&& f) const;
        // Rebox a typed buffer into hetero storage IN PLACE (same node identity) — the
        // demote-on-stamp kernel for var-laundered typed arrays.
        inline void demote_to_hetero(engine* eng);

    private:
        enum class lane_t : uint8_t { values, ints, floats, bytes, strs, objs };
        static constexpr lane_t lane_of(kind_t k) noexcept {
            switch (k) {
                case kind_t::hetero: return lane_t::values;
                case kind_t::i64: return lane_t::ints;
                case kind_t::f64: return lane_t::floats;
                case kind_t::str: return lane_t::strs;
                case kind_t::obj: return lane_t::objs;
                default: return lane_t::bytes;
            }
        }
        inline void construct_lane() noexcept;
        inline void destroy_lane() noexcept;
        inline void switch_lane(kind_t k) noexcept;
        kind_t kind_ = kind_t::hetero;
        union {
            std::vector<script_value> values_;
            std::vector<script_int> ints_;
            std::vector<script_float> floats_;
            std::vector<uint8_t> bytes_;
            std::vector<strong_ptr<script_string>> strs_;
            std::vector<obj_slot> objs_;
        };
    public:
        // the engine that made this node (after the hot fields): its typed reads, copies and memory charges run against it
        engine* owner = nullptr;
    };
    // Transparent probe for CONSTANT string keys: map lookups compare against the raw
    // constant bytes instead of materializing a script_value per access (`rec["px"]`
    // was one string allocation per read). Ordering is defined in value.cpp and MUST
    // mirror script_value::operator<=> exactly (type rank first, then content).
    struct map_string_key_probe {
        std::string_view text;
    };
    struct script_value_map_less {
        using is_transparent = void;
        bool operator()(const script_value& a, const script_value& b) const;
        bool operator()(const script_value& a, const map_string_key_probe& p) const;
        bool operator()(const map_string_key_probe& p, const script_value& b) const;
    };
    class script_map;   // core/script_map.hpp: hashed lookup, key order on iteration
    
    // Function type for script functions
    // Returns checked_result to allow proper error propagation without exceptions
    using script_function = std::function<checked_result<script_value>(const std::vector<script_value>&)>;
    // A function value's payload: the callable and the engine that made it (callbacks, signals and conversions run against it)
    struct script_function_node : script_function {
        engine* owner = nullptr;
        script_function_node(script_function&& a_function, engine* a_owner) : script_function(std::move(a_function)), owner(a_owner) {}
    };

    // A host callable with non-const T& parameters (bit i = script argument i): the argument capture binds those
    // positions as lvalues, as it does a script function's T& parameters (detail/arg_capture.hpp)
    struct host_writeback_function {
        script_function fn;
        uint64_t mask = 0;
        checked_result<script_value> operator()(const std::vector<script_value>& args) const;   // core/value.hpp
    };

    // A frame-slot local a host T& argument names, as the backend re-finds it after the call (a frame and a slot): an
    // unboxed local nothing else can reach, so the call stores back into it without boxing it in a cell
    struct host_slot {
        const void* frame = nullptr;
        size_t slot = 0;
    };

    // For serializable functions, we need to track additional info
    struct function_info {
        std::string name;           // Function name (for debugging)
        std::string script_text;     // Original script text (for script-defined functions)
        
        // Captured variables with their types and capture method
        struct captured_var {
            std::string name;
            script_value_type type;
            bool by_reference;       // true = captured by reference, false = by value
        };
        std::vector<captured_var> captured_vars;
        
        bool is_native = false;     // True for C++ functions (can't serialize)
    };
    
    // Instance variables for script execution (per-execution context)
    using instance_variables = std::unordered_map<std::string, script_value>;
    
    // Source location for error reporting
    struct source_location {
        std::string filename;
        size_t line = 1;
        size_t column = 1;
        
        std::string to_string() const {
            return filename + ":" + std::to_string(line) + ":" + std::to_string(column);
        }
    };
    
    // One frame of a captured script stack trace (innermost first)
    struct stack_frame {
        std::string function;
        std::string file;
        size_t line = 0;
        size_t column = 0;
        size_t elided = 0;   // the final marker: this many outer frames are not listed
    };

    // A frame of `function` standing at `at` (null: no statement located yet)
    inline stack_frame trace_frame_at(std::string function, const source_location* at) {
        return at ? stack_frame{ std::move(function), at->filename, at->line, at->column, 0 } : stack_frame{ std::move(function), {}, 0, 0, 0 };
    }

    // A trace lists the innermost frames and one final marker for the rest, so a catch deep in a recursion costs the same
    // as a shallow one
    inline constexpr size_t k_stack_trace_frames = 64;
    inline std::string elided_frames_text(size_t count) { return "... " + std::to_string(count) + " more frames"; }
    // uncounted: outer frames the capture did not walk
    inline void cap_stack_trace(std::vector<stack_frame>& trace, size_t uncounted = 0) {
        size_t total = uncounted, leading = 0;
        bool listing = true;
        for (const auto& f : trace) {
            total += f.elided ? f.elided : 1;
            if (f.elided) { listing = false; } else if (listing) { ++leading; }
        }
        const size_t kept = leading < k_stack_trace_frames ? leading : k_stack_trace_frames;
        if (kept == total) { return; }
        trace.resize(kept);
        trace.push_back({ "...", std::string(), 0, 0, total - kept });
    }

    // Base exception class with source location tracking
    // Uses std::string for nice formatted messages (cold path, user-facing)
    class exception : public std::runtime_error {
    public:
        exception(const std::string& message, const source_location& location = {})
            : std::runtime_error(message), location_(location) {}
        // a literal throws without a temporary std::string or source_location at the throw site
        explicit exception(const char* message) : std::runtime_error(message) {}

        const source_location& location() const { return location_; }

    private:
        source_location location_;
    };

    // Specific exception types
    class parse_error : public exception {
        using exception::exception;
    };

    class runtime_error : public exception {
        using exception::exception;
    };

    // Cold throwers (value.cpp): a hot function's failure branch is one call, its text is built out of line
    [[noreturn]] JAI_NOINLINE void throw_runtime_error(std::string_view message);
    // runtime_error(format_error_message_numeric(message, id1, id2))
    [[noreturn]] JAI_NOINLINE void throw_numeric_error(std::string_view message, uint64_t id1, uint64_t id2);
    class engine;
    // throw_numeric_error, except a "{0}" text (a parked host or conversion exception's own text) reads its symbol
    [[noreturn]] JAI_NOINLINE void throw_checked_error(engine* eng, std::string_view message, uint64_t id1, uint64_t id2);
    // runtime_error(prefix + detail::arity_detail(expected, got)): a bound thunk's argument count
    [[noreturn]] JAI_NOINLINE void throw_binder_arity(const char* prefix, size_t expected, size_t got);
    // runtime_error(text + count)
    [[noreturn]] JAI_NOINLINE void throw_text_with_count(const char* text, size_t count);
    // runtime_error(a + b + c)
    [[noreturn]] JAI_NOINLINE void throw_concat(std::string_view a, std::string_view b, std::string_view c = {});

    namespace detail { struct engine_access; }
    // Script exception for try/catch/throw
    class script_exception : public runtime_error {
        using runtime_error::runtime_error;
    public:
        bool thrown = false;   // a script `throw value` (the catch variable is the value), not a run-time error
    private:
        friend struct detail::engine_access;
        error_kind kind_ = error_kind::other;   // a run-time error's family (e.kind), carried from the raise to the catch
    };
    
} // namespace jai

#endif // __JAISCRIPT_CORE_TYPES_HPP__