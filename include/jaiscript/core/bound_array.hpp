#pragma once

#ifndef __JAISCRIPT_CORE_BOUND_ARRAY_HPP__
#define __JAISCRIPT_CORE_BOUND_ARRAY_HPP__

#include "value.hpp"
#include "types.hpp"
#include <jaiscript/detail/numeric_conversion.hpp>
#include <memory>
#include <iterator>
#include <algorithm>
#include <stdexcept>
#include <any>

namespace jai {

class engine;
template<typename K, typename V> class bound_map;

/**
 * @brief Zero-copy wrapper for JaiScript arrays with value semantics
 *
 * When passed by value: Creates a deep copy of the array
 * When passed by reference: Zero-copy access to the underlying array
 * When passed by const reference: Zero-copy read-only access
 *
 * The view sits on the array NODE (script_array) and dispatches by its kind: an
 * array<int>/array<float> raw buffer is mutated in place exactly like an array<var>
 * vector (the node's get/set/push are the funnel), so a C++ function taking
 * bound_array<int64_t>& writes straight into the typed storage. The element type must
 * fit the node: a typed buffer only binds arithmetic element types.
 *
 * @tparam T The element type of the array
 */
template<typename T>
class bound_array {
private:
    // The view keeps a HANDLE on the array value (a shallow copy shares the node, an
    // owned clone is the value-semantics copy) - the node can never outlive the view
    script_value handle_;
    bool owned_ = false;
    engine* engine_ref_ = nullptr;

    engine* get_engine() const {
        return engine_ref_;
    }

    script_array* node() const {
        return handle_.raw_storage_index() == script_value::TYPEID_ARRAY
            ? const_cast<script_value&>(handle_).unchecked_get_array_storage().get()
            : nullptr;
    }

    static void check_kind_fits(const script_array& n) {
        if (!n.is_typed()) { return; }
        // object buffers (array<UserType>) bind class element types: every access boxes the
        // slot through get/set, so the extraction path is the boxed one
        const bool fits = n.kind() == script_array::kind_t::obj ? (!std::is_arithmetic_v<T> && !std::is_same_v<T, std::string>)
                        : n.kind() == script_array::kind_t::str ? std::is_same_v<T, std::string>
                                                                  : std::is_arithmetic_v<T>;
        if (!fits) {
            throw runtime_error("bound_array element type does not fit a typed array buffer "
                                "(numeric buffers bind arithmetic element types, array<string> binds std::string; "
                                "declare array<var> for the dynamic view)");
        }
    }

    [[noreturn]] JAI_NOINLINE static void throw_byte_range(bool is_signed, script_int x) {
        throw runtime_error("Cannot store " + std::to_string(x) + (is_signed ? " in array<int8> (-128..127)" : " in array<uint8> (0..255)"));
    }

    // A byte buffer (array<uint8> / array<int8>) takes an int inside its kind's range, the script store's rule
    static script_value fit_node(const script_array& n, script_value v) {
        if (n.kind() != script_array::kind_t::u8 && n.kind() != script_array::kind_t::i8) { return v; }
        const bool is_signed = n.kind() == script_array::kind_t::i8;
        script_int x = 0;
        if (v.is_int()) {
            x = v.unchecked_as_int();
        } else if (v.is_float()) {
            auto converted = detail::checked_float_to_integer(v.unchecked_as_float());
            if (!converted) { throw_runtime_error(detail::float_to_integer_error); }
            x = converted.value();
        } else if (v.is_bool()) {
            x = v.unchecked_as_bool() ? 1 : 0;
        } else if (v.is_char()) {
            x = static_cast<script_int>(static_cast<unsigned char>(v.unchecked_as_char()));
        } else {
            throw_runtime_error(is_signed ? "Cannot store a non-number in array<int8>" : "Cannot store a non-number in array<uint8>");
        }
        if (is_signed ? (x < -128 || x > 127) : (x < 0 || x > 255)) {
            throw_byte_range(is_signed, x);
        }
        return script_value(x, n.owner);
    }

public:
    static script_value make_script_value(const T& value, engine* eng) {
        if constexpr (std::is_same_v<T, int>) {
            return script_value(static_cast<script_int>(value), eng);
        } else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, script_int>) {
            return script_value(value, eng);
        } else if constexpr (std::is_same_v<T, float>) {
            return script_value(static_cast<script_float>(value), eng);
        } else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, script_float>) {
            return script_value(value, eng);
        } else if constexpr (std::is_same_v<T, bool>) {
            return script_value(value, eng);
        } else if constexpr (std::is_same_v<T, char>) {
            return script_value(value, eng);
        } else if constexpr (std::is_integral_v<T>) {
            return script_value(static_cast<script_int>(value), eng);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return script_value(value, eng);
        } else if constexpr (std::is_same_v<T, script_value>) {
            return value;
        } else if constexpr (is_bound_array_v<T>) {
            // For nested bound_array, return its underlying script_value
            if (value.is_owned()) {
                return value.as_script_value();
            } else {
                // If it's a reference wrapper, we need to create a deep copy
                script_value result = script_value::make_array(nullptr, eng);
                auto& arr = result.as_array();
                arr.reserve(value.size());
                for (const auto& elem : value) {
                    // Recursively convert each element
                    arr.push_back(bound_array<typename T::value_type>::make_script_value(elem, eng));
                }
                return result;
            }
        } else if constexpr (is_bound_map_v<T>) {
            // For nested bound_map, return its underlying script_value
            if (value.is_owned()) {
                return value.as_script_value();
            } else {
                // If it's a reference wrapper, we need to create a deep copy
                using K = typename T::key_type;
                using V = typename T::mapped_type;
                script_value result = script_value::make_map(
                    type_info::make<K>(),
                    type_info::make<V>(),
                    eng
                );
                auto& map = result.as_map();
                for (const auto& [k, v] : value) {
                    map[bound_array<K>::make_script_value(k, eng)] = bound_array<V>::make_script_value(v, eng);
                }
                return result;
            }
        } else {
            // Custom object type - this should not be used directly
            // Custom objects should be registered via dynamic_binder and created via engine->make_object<T>()
            // If you're seeing this error, register your type first!
            throw runtime_error("Cannot create script_value for unregistered custom type in bound_array. "
                              "Use dynamic_binder<T> to register the type first, then create objects via engine->make_object<T>()");
        }
    }

    // Helper trait to detect bound_array types
    template<typename U>
    struct is_bound_array : std::false_type {};

    template<typename U>
    struct is_bound_array<bound_array<U>> : std::true_type {};

    template<typename U>
    static constexpr bool is_bound_array_v = is_bound_array<U>::value;

    // Helper trait to detect bound_map types
    template<typename U>
    struct is_bound_map : std::false_type {};

    template<typename K2, typename V2>
    struct is_bound_map<bound_map<K2, V2>> : std::true_type {};

    template<typename U>
    static constexpr bool is_bound_map_v = is_bound_map<U>::value;

    // Helper to extract T from script_value (public for nested access)
    static T extract_value(const script_value& val) {
        if constexpr (std::is_same_v<T, script_value>) {
            return val;
        } else if constexpr (is_bound_array_v<T>) {
            // For nested bound_array, create a zero-copy wrapper
            if (!val.is_array()) {
                throw runtime_error("Cannot extract bound_array from non-array script_value");
            }
            return T(const_cast<script_value&>(val), val.payload_engine());
        } else if constexpr (is_bound_map_v<T>) {
            // For nested bound_map, create a zero-copy wrapper
            if (!val.is_map()) {
                throw runtime_error("Cannot extract bound_map from non-map script_value");
            }
            return T(const_cast<script_value&>(val).as_map(), val.payload_engine());
        } else {
            // For custom types, use the same logic as script_value::as<T>()
            // which will check conversion registry if engine reference is available
            return val.as<T>();
        }
    }

public:
    // Type aliases for STL compatibility
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;

    /**
     * @brief Zero-copy view of an array value: shares the node, mutations land in it
     * @param val Script value holding the array (any kind)
     * @param eng Engine reference for creating new script_values
     */
    explicit bound_array(script_value& val, engine* eng)
        : handle_(val), owned_(false), engine_ref_(eng) {
        if (!val.is_array()) {
            throw runtime_error("Cannot create bound_array from non-array script_value");
        }
        check_kind_fits(*node());
    }

    /**
     * @brief Construct from script_value (creates deep copy for value semantics)
     * @param val Script value containing an array
     */
    explicit bound_array(const script_value& val)
        : handle_(std::monostate{}, val.payload_engine()), owned_(true), engine_ref_(val.payload_engine()) {
        if (!val.is_array()) {
            throw runtime_error("Cannot create bound_array from non-array script_value");
        }
        handle_ = val.clone();
        check_kind_fits(*node());
    }

    /**
     * @brief Copy constructor
     * Always creates an owned deep copy (value semantics)
     */
    bound_array(const bound_array& other)
        : handle_(other.handle_.deref().clone()), owned_(true), engine_ref_(other.engine_ref_) {}

    /**
     * @brief Move constructor
     */
    bound_array(bound_array&& other) noexcept
        : handle_(std::move(other.handle_)), owned_(other.owned_), engine_ref_(other.engine_ref_) {}

    /**
     * @brief Copy assignment
     */
    bound_array& operator=(const bound_array& other) {
        if (this != &other) {
            engine_ref_ = other.engine_ref_;
            if (other.owned_) {
                handle_ = other.handle_.clone();
                owned_ = true;
            } else {
                handle_ = other.handle_;
                owned_ = false;
            }
        }
        return *this;
    }

    /**
     * @brief Move assignment
     */
    bound_array& operator=(bound_array&& other) noexcept {
        if (this != &other) {
            engine_ref_ = other.engine_ref_;
            handle_ = std::move(other.handle_);
            owned_ = other.owned_;
        }
        return *this;
    }

    /**
     * @brief Check if this array owns its data (is a copy) or references external data
     */
    bool is_owned() const { return owned_; }

    // ===== Capacity =====

    size_type size() const { script_array* n = node(); return n ? n->size() : 0; }
    bool empty() const { return size() == 0; }

    void reserve(size_type new_cap) {
        if (script_array* n = node()) n->reserve(new_cap);
    }

    size_type capacity() const { script_array* n = node(); return n ? n->capacity() : 0; }

    // ===== Modifiers =====

    void clear() {
        if (script_array* n = node()) n->clear();
    }

    void push_back(const T& value) {
        script_array* n = node();
        if (!n) throw runtime_error("Cannot push_back to null bound_array");
        n->push(fit_node(*n, make_script_value(value, get_engine())));
    }

    void push_back(T&& value) {
        script_array* n = node();
        if (!n) throw runtime_error("Cannot push_back to null bound_array");
        n->push(fit_node(*n, make_script_value(std::forward<T>(value), get_engine())));
    }

    void pop_back() {
        script_array* n = node();
        if (n && !n->empty()) {
            n->pop_back();
        }
    }

    void resize(size_type count) {
        script_array* n = node();
        if (!n) throw runtime_error("Cannot resize null bound_array");
        if (count > n->size()) {
            while (n->size() < count) {
                n->push(fit_node(*n, make_script_value(T{}, get_engine())));
            }
        } else {
            n->truncate(count);
        }
    }

    void resize(size_type count, const T& value) {
        script_array* n = node();
        if (!n) throw runtime_error("Cannot resize null bound_array");
        if (count > n->size()) {
            const script_value fitted = fit_node(*n, make_script_value(value, get_engine()));
            while (n->size() < count) {
                n->push(fitted);
            }
        } else {
            n->truncate(count);
        }
    }

    // ===== Element access =====

    T operator[](size_type pos) const {
        return extract_value(node()->get(pos, get_engine()));
    }

    T at(size_type pos) const {
        script_array* n = node();
        if (!n || pos >= n->size()) {
            throw std::out_of_range("bound_array::at: index out of range");
        }
        return extract_value(n->get(pos, get_engine()));
    }

    T front() const {
        script_array* n = node();
        if (!n || n->empty()) {
            throw runtime_error("bound_array::front: array is empty");
        }
        return extract_value(n->get(0, get_engine()));
    }

    T back() const {
        script_array* n = node();
        if (!n || n->empty()) {
            throw runtime_error("bound_array::back: array is empty");
        }
        return extract_value(n->get(n->size() - 1, get_engine()));
    }

    // ===== Element modification proxy =====

    class element_proxy {
        script_array& node_;
        size_type idx_;
        engine* engine_ref_;
        // Storage for nested bound_map wrappers (no copying, just wrapper objects)
        mutable std::unique_ptr<std::any> nested_wrapper_storage_;

        // Nested containers only exist in dynamic nodes (typed buffers hold scalars)
        script_value& dynamic_slot() const {
            if (node_.is_typed()) {
                throw runtime_error("Element is not a container (typed array buffer)");
            }
            return node_.values()[idx_];
        }

    public:
        element_proxy(script_array& node, size_type idx, engine* eng)
            : node_(node), idx_(idx), engine_ref_(eng) {}

        // Copy constructor
        element_proxy(const element_proxy& other)
            : node_(other.node_), idx_(other.idx_), engine_ref_(other.engine_ref_) {}

        // Move constructor - efficient for temporary proxies
        element_proxy(element_proxy&& other) noexcept
            : node_(other.node_), idx_(other.idx_), engine_ref_(other.engine_ref_),
              nested_wrapper_storage_(std::move(other.nested_wrapper_storage_)) {}

        element_proxy& operator=(const T& value) {
            node_.set(idx_, bound_array::fit_node(node_, bound_array::make_script_value(value, engine_ref_)));
            return *this;
        }

        element_proxy& operator=(T&& value) {
            node_.set(idx_, bound_array::fit_node(node_, bound_array::make_script_value(std::forward<T>(value), engine_ref_)));
            return *this;
        }

        // Copy assignment from other element_proxy
        element_proxy& operator=(const element_proxy& other) {
            node_.set(idx_, bound_array::fit_node(node_, bound_array::make_script_value(other.get(), engine_ref_)));
            return *this;
        }

        // Move assignment from other element_proxy: the element value moves node to node
        element_proxy& operator=(element_proxy&& other) {
            node_.set(idx_, bound_array::fit_node(node_, other.node_.get(other.idx_, other.engine_ref_)));
            return *this;
        }

        operator T() const {
            return bound_array::extract_value(node_.get(idx_, engine_ref_));
        }

        T get() const {
            return bound_array::extract_value(node_.get(idx_, engine_ref_));
        }

        // Support for nested bound_array operations
        template<typename U = T>
        typename std::enable_if<is_bound_array_v<U>, typename U::element_proxy>::type
        operator[](size_type pos) {
            script_value& nested_val = dynamic_slot();
            if (!nested_val.is_array()) {
                throw runtime_error("Element is not an array");
            }
            return typename U::element_proxy(*nested_val.unchecked_get_array_storage(), pos, engine_ref_);
        }

        // Const version for reading
        template<typename U = T>
        typename std::enable_if<is_bound_array_v<U>, typename U::value_type>::type
        operator[](size_type pos) const {
            const script_value& nested_val = dynamic_slot();
            if (!nested_val.is_array()) {
                throw runtime_error("Element is not an array");
            }
            return U::extract_value(nested_val.unchecked_array_node()->get(pos, engine_ref_));
        }

        // Support for size() on nested arrays
        template<typename U = T>
        typename std::enable_if<is_bound_array_v<U>, size_type>::type
        size() const {
            const script_value& nested_val = dynamic_slot();
            if (!nested_val.is_array()) {
                throw runtime_error("Element is not an array");
            }
            return nested_val.unchecked_array_node()->size();
        }

        // Support for push_back on nested arrays
        template<typename U = T, typename V>
        typename std::enable_if<is_bound_array_v<U>, void>::type
        push_back(V&& value) {
            script_value& nested_val = dynamic_slot();
            if (!nested_val.is_array()) {
                throw runtime_error("Element is not an array");
            }
            nested_val.unchecked_get_array_storage()->push(U::make_script_value(std::forward<V>(value), engine_ref_));
        }

        // Support for nested bound_map subscript operations
        template<typename U = T>
        typename std::enable_if<is_bound_map_v<U>, typename U::element_proxy>::type
        operator[](const typename U::key_type& key) {
            script_value& nested_val = dynamic_slot();
            if (!nested_val.is_map()) {
                throw runtime_error("Element is not a map");
            }
            auto& nested_map = nested_val.as_map();
            // Store the wrapper in our storage to keep it alive (zero-copy wrapper around existing script_value)
            nested_wrapper_storage_ = std::make_unique<std::any>(U(nested_map));
            U& bound_map_wrapper = std::any_cast<U&>(*nested_wrapper_storage_);
            return bound_map_wrapper[key];
        }

        // Const version for reading nested maps
        template<typename U = T>
        typename std::enable_if<is_bound_map_v<U>, typename U::mapped_type>::type
        operator[](const typename U::key_type& key) const {
            script_value& nested_val = dynamic_slot();
            if (!nested_val.is_map()) {
                throw runtime_error("Element is not a map");
            }
            auto& nested_map = nested_val.as_map();
            U bound_map_wrapper(nested_map);
            return bound_map_wrapper[key];
        }

        // Support for iterating nested arrays
        template<typename U = T>
        typename std::enable_if<is_bound_array_v<U>, typename U::iterator>::type
        begin() {
            script_value& nested_val = dynamic_slot();
            if (!nested_val.is_array()) {
                throw runtime_error("Element is not an array");
            }
            return typename U::iterator(nested_val.unchecked_array_node(), 0, engine_ref_);
        }

        template<typename U = T>
        typename std::enable_if<is_bound_array_v<U>, typename U::iterator>::type
        end() {
            script_value& nested_val = dynamic_slot();
            if (!nested_val.is_array()) {
                throw runtime_error("Element is not an array");
            }
            const script_array* nested = nested_val.unchecked_array_node();
            return typename U::iterator(nested, nested->size(), engine_ref_);
        }

        template<typename U = T>
        typename std::enable_if<is_bound_array_v<U>, typename U::const_iterator>::type
        begin() const {
            const script_value& nested_val = dynamic_slot();
            if (!nested_val.is_array()) {
                throw runtime_error("Element is not an array");
            }
            return typename U::const_iterator(nested_val.unchecked_array_node(), 0, engine_ref_);
        }

        template<typename U = T>
        typename std::enable_if<is_bound_array_v<U>, typename U::const_iterator>::type
        end() const {
            const script_value& nested_val = dynamic_slot();
            if (!nested_val.is_array()) {
                throw runtime_error("Element is not an array");
            }
            const script_array* nested = nested_val.unchecked_array_node();
            return typename U::const_iterator(nested, nested->size(), engine_ref_);
        }

        // Support for iterating nested maps
        template<typename U = T>
        typename std::enable_if<is_bound_map_v<U>, typename U::iterator>::type
        begin() {
            script_value& nested_val = dynamic_slot();
            if (!nested_val.is_map()) {
                throw runtime_error("Element is not a map");
            }
            auto& nested_map = nested_val.as_map();
            return typename U::iterator(nested_map.begin());
        }

        template<typename U = T>
        typename std::enable_if<is_bound_map_v<U>, typename U::iterator>::type
        end() {
            script_value& nested_val = dynamic_slot();
            if (!nested_val.is_map()) {
                throw runtime_error("Element is not a map");
            }
            auto& nested_map = nested_val.as_map();
            return typename U::iterator(nested_map.end());
        }

        template<typename U = T>
        typename std::enable_if<is_bound_map_v<U>, typename U::const_iterator>::type
        begin() const {
            script_value& nested_val = dynamic_slot();
            if (!nested_val.is_map()) {
                throw runtime_error("Element is not a map");
            }
            auto& nested_map = nested_val.as_map();
            return typename U::const_iterator(nested_map.begin());
        }

        template<typename U = T>
        typename std::enable_if<is_bound_map_v<U>, typename U::const_iterator>::type
        end() const {
            script_value& nested_val = dynamic_slot();
            if (!nested_val.is_map()) {
                throw runtime_error("Element is not a map");
            }
            auto& nested_map = nested_val.as_map();
            return typename U::const_iterator(nested_map.end());
        }
    };

    element_proxy operator[](size_type pos) {
        return element_proxy(*node(), pos, engine_ref_);
    }

    // ===== Iterators (index-based: one shape for every node kind) =====

    class iterator {
        const script_array* node_ = nullptr;
        size_type i_ = 0;
        engine* engine_ref_ = nullptr;

    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using reference = T;

        iterator() = default;
        iterator(const script_array* node, size_type i, engine* eng) : node_(node), i_(i), engine_ref_(eng) {}

        T operator*() const { return extract_value(node_->get(i_, engine_ref_)); }

        iterator& operator++() { ++i_; return *this; }
        iterator operator++(int) { iterator tmp = *this; ++i_; return tmp; }
        iterator& operator--() { --i_; return *this; }
        iterator operator--(int) { iterator tmp = *this; --i_; return tmp; }

        iterator& operator+=(difference_type n) { i_ = static_cast<size_type>(static_cast<difference_type>(i_) + n); return *this; }
        iterator& operator-=(difference_type n) { i_ = static_cast<size_type>(static_cast<difference_type>(i_) - n); return *this; }

        iterator operator+(difference_type n) const { iterator tmp = *this; tmp += n; return tmp; }
        iterator operator-(difference_type n) const { iterator tmp = *this; tmp -= n; return tmp; }

        difference_type operator-(const iterator& other) const { return static_cast<difference_type>(i_) - static_cast<difference_type>(other.i_); }

        bool operator==(const iterator& other) const { return node_ == other.node_ && i_ == other.i_; }
        bool operator!=(const iterator& other) const { return !(*this == other); }
        bool operator<(const iterator& other) const { return i_ < other.i_; }
        bool operator<=(const iterator& other) const { return i_ <= other.i_; }
        bool operator>(const iterator& other) const { return i_ > other.i_; }
        bool operator>=(const iterator& other) const { return i_ >= other.i_; }

        T operator[](difference_type n) const { return extract_value(node_->get(static_cast<size_type>(static_cast<difference_type>(i_) + n), engine_ref_)); }

        const script_array* node() const { return node_; }
        size_type index() const { return i_; }
        engine* eng() const { return engine_ref_; }
    };

    class const_iterator {
        const script_array* node_ = nullptr;
        size_type i_ = 0;
        engine* engine_ref_ = nullptr;

    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T;

        const_iterator() = default;
        const_iterator(const script_array* node, size_type i, engine* eng) : node_(node), i_(i), engine_ref_(eng) {}
        const_iterator(const iterator& other) : node_(other.node()), i_(other.index()), engine_ref_(other.eng()) {}

        T operator*() const { return extract_value(node_->get(i_, engine_ref_)); }

        const_iterator& operator++() { ++i_; return *this; }
        const_iterator operator++(int) { const_iterator tmp = *this; ++i_; return tmp; }
        const_iterator& operator--() { --i_; return *this; }
        const_iterator operator--(int) { const_iterator tmp = *this; --i_; return tmp; }

        const_iterator& operator+=(difference_type n) { i_ = static_cast<size_type>(static_cast<difference_type>(i_) + n); return *this; }
        const_iterator& operator-=(difference_type n) { i_ = static_cast<size_type>(static_cast<difference_type>(i_) - n); return *this; }

        const_iterator operator+(difference_type n) const { const_iterator tmp = *this; tmp += n; return tmp; }
        const_iterator operator-(difference_type n) const { const_iterator tmp = *this; tmp -= n; return tmp; }

        difference_type operator-(const const_iterator& other) const { return static_cast<difference_type>(i_) - static_cast<difference_type>(other.i_); }

        bool operator==(const const_iterator& other) const { return node_ == other.node_ && i_ == other.i_; }
        bool operator!=(const const_iterator& other) const { return !(*this == other); }
        bool operator<(const const_iterator& other) const { return i_ < other.i_; }
        bool operator<=(const const_iterator& other) const { return i_ <= other.i_; }
        bool operator>(const const_iterator& other) const { return i_ > other.i_; }
        bool operator>=(const const_iterator& other) const { return i_ >= other.i_; }

        T operator[](difference_type n) const { return extract_value(node_->get(static_cast<size_type>(static_cast<difference_type>(i_) + n), engine_ref_)); }
    };

    iterator begin() { script_array* n = node(); return n ? iterator(n, 0, engine_ref_) : iterator(); }
    iterator end() { script_array* n = node(); return n ? iterator(n, n->size(), engine_ref_) : iterator(); }

    const_iterator begin() const { script_array* n = node(); return n ? const_iterator(n, 0, engine_ref_) : const_iterator(); }
    const_iterator end() const { script_array* n = node(); return n ? const_iterator(n, n->size(), engine_ref_) : const_iterator(); }

    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    // ===== Conversion =====

    /**
     * @brief Convert to std::vector<T> (always creates a copy)
     */
    std::vector<T> to_vector() const {
        std::vector<T> result;
        if (script_array* n = node()) {
            result.reserve(n->size());
            n->for_each(get_engine(), [&](const script_value& val) { result.push_back(extract_value(val)); });
        }
        return result;
    }

    /**
     * @brief Get underlying script_value (for owned arrays only)
     */
    const script_value& as_script_value() const {
        if (!owned_) {
            throw runtime_error("Cannot get script_value from non-owned bound_array");
        }
        return handle_;
    }
};

} // namespace jai

#endif // __JAISCRIPT_CORE_BOUND_ARRAY_HPP__
