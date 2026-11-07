#pragma once

#ifndef __JAISCRIPT_CORE_BOUND_MAP_HPP__
#define __JAISCRIPT_CORE_BOUND_MAP_HPP__

#include "value.hpp"
#include "types.hpp"
#include <memory>
#include <iterator>
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <any>

namespace jai {

// Forward declaration
template<typename T> class bound_array;

/**
 * @brief Zero-copy wrapper for JaiScript maps with value semantics
 * 
 * When passed by value: Creates a deep copy of the map
 * When passed by reference: Zero-copy access to the underlying map
 * When passed by const reference: Zero-copy read-only access
 * 
 * @tparam K The key type of the map
 * @tparam V The value type of the map
 */
template<typename K, typename V>
class bound_map {
private:
    // Either owns the data (for by-value) or references it (for by-reference)
    mutable std::shared_ptr<script_value> owned_value_;
    script_map* map_;
    engine* engine_ref_;  // Engine reference for creating script_values

    // Helper to get engine reference
    engine* get_engine() const {
        return engine_ref_;
    }
    
public:
    // Helper to create script_value from K or V (public for nested access)
    template<typename T>
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
                    arr.push_back(make_script_value(elem, eng));
                }
                return result;
            }
        } else if constexpr (is_bound_map_v<T>) {
            // For nested bound_map, return its underlying script_value  
            if (value.is_owned()) {
                return value.as_script_value();
            } else {
                // If it's a reference wrapper, we need to create a deep copy
                using KT = typename T::key_type;
                using VT = typename T::mapped_type;
                script_value result = script_value::make_map(
                    type_info::make<KT>(), 
                    type_info::make<VT>(),
                    eng
                );
                auto& map = result.as_map();
                for (const auto& [k, v] : value) {
                    map.insert_or_assign(bound_map<KT, VT>::make_script_value(k, eng), bound_map<KT, VT>::make_script_value(v, eng));
                }
                return result;
            }
        } else {
            // Custom object type - wrap in shared_ptr
            // Can't use engine->make_object here due to incomplete type
            // Use the registered type name if available through engine
            auto type_name = typeid(T).name();
            return script_value::make_object(type_name, std::make_shared<T>(value), eng);
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
    
    // Helper to extract K or V from script_value (public for nested access)
    template<typename T>
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
            return val.as<T>();
        }
    }
    
private:
    // Helper to find a key in the map
    script_map::iterator find_key(const K& key) {
        if (!map_) return script_map::iterator();
        
        auto key_val = make_script_value(key, get_engine());
        return map_->find(key_val);
    }
    
    script_map::const_iterator find_key(const K& key) const {
        if (!map_) return script_map::const_iterator();
        
        auto key_val = make_script_value(key, get_engine());
        return map_->find(key_val);
    }
    
public:
    // Type aliases for STL compatibility
    using key_type = K;
    using mapped_type = V;
    using value_type = std::pair<const K, V>;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    
    /**
     * @brief Construct from map reference (zero-copy)
     * @param m Reference to existing script map
     * @param eng Engine reference for creating new script_values
     */
    explicit bound_map(script_map& m, engine* eng)
        : owned_value_(nullptr), map_(&m), engine_ref_(eng) {}
    
    /**
     * @brief Construct from script_value (creates deep copy for value semantics)
     * @param val Script value containing a map
     */
    explicit bound_map(const script_value& val) {
        if (!val.is_map()) {
            throw runtime_error("Cannot create bound_map from non-map script_value");
        }
        engine_ref_ = val.payload_engine();
        owned_value_ = std::make_shared<script_value>(val.clone());
        map_ = &owned_value_->as_map();
    }
    
    /**
     * @brief Copy constructor
     */
    bound_map(const bound_map& other) {
        engine_ref_ = other.engine_ref_;  // Copy engine reference
        if (other.owned_value_) {
            // Other owns its data, so we need to deep copy
            owned_value_ = std::make_shared<script_value>(other.owned_value_->clone());
            map_ = &owned_value_->as_map();
        } else {
            // Other is a reference, we become a reference too
            owned_value_ = nullptr;
            map_ = other.map_;
        }
    }
    
    /**
     * @brief Move constructor
     */
    bound_map(bound_map&& other) noexcept
        : owned_value_(std::move(other.owned_value_)), map_(other.map_), engine_ref_(std::move(other.engine_ref_)) {
        if (owned_value_) {
            map_ = &owned_value_->as_map();
        }
        other.map_ = nullptr;
    }
    
    /**
     * @brief Copy assignment
     */
    bound_map& operator=(const bound_map& other) {
        if (this != &other) {
            if (other.owned_value_) {
                owned_value_ = std::make_shared<script_value>(other.owned_value_->clone());
                map_ = &owned_value_->as_map();
            } else {
                owned_value_ = nullptr;
                map_ = other.map_;
            }
        }
        return *this;
    }
    
    /**
     * @brief Move assignment
     */
    bound_map& operator=(bound_map&& other) noexcept {
        if (this != &other) {
            owned_value_ = std::move(other.owned_value_);
            map_ = other.map_;
            if (owned_value_) {
                map_ = &owned_value_->as_map();
            }
            other.map_ = nullptr;
        }
        return *this;
    }
    
    /**
     * @brief Check if this map owns its data (is a copy) or references external data
     */
    bool is_owned() const { return owned_value_ != nullptr; }
    
    // ===== Capacity =====
    
    size_type size() const { return map_ ? map_->size() : 0; }
    bool empty() const { return !map_ || map_->empty(); }
    
    // ===== Modifiers =====
    
    void clear() {
        if (map_) map_->clear();
    }
    
    std::pair<bool, bool> insert(const K& key, const V& value) {
        if (!map_) throw runtime_error("Cannot insert into null bound_map");
        
        auto key_val = make_script_value(key, get_engine());
        auto value_val = make_script_value(value, get_engine());
        
        auto result = map_->insert({key_val, value_val});
        return {result.second, result.second};
    }
    
    template<typename InputIt>
    void insert(InputIt first, InputIt last) {
        for (auto it = first; it != last; ++it) {
            insert(it->first, it->second);
        }
    }
    
    size_type erase(const K& key) {
        if (!map_) return 0;
        
        auto key_val = make_script_value(key, get_engine());
        return map_->erase(key_val);
    }
    
    // ===== Element access =====
    
    V at(const K& key) const {
        auto it = find_key(key);
        if (it == map_->end()) {
            throw std::out_of_range("bound_map::at: key not found");
        }
        return extract_value<V>(it->second);
    }
    
    bool contains(const K& key) const {
        return find_key(key) != (map_ ? map_->end() : script_map::const_iterator());
    }
    
    size_type count(const K& key) const {
        return contains(key) ? 1 : 0;
    }
    
    // ===== Element modification proxy =====
    
    class element_proxy {
        bound_map& parent_;
        K key_;
        // Storage for nested bound_map wrappers (no copying, just wrapper objects)
        mutable std::unique_ptr<std::any> nested_wrapper_storage_;
        
    public:
        element_proxy(bound_map& parent, const K& key) 
            : parent_(parent), key_(key) {}
        
        element_proxy& operator=(const V& value) {
            if (!parent_.map_) throw runtime_error("Cannot assign to null bound_map");
            
            auto key_val = make_script_value(key_, parent_.get_engine());
            auto value_val = make_script_value(value, parent_.get_engine());
            (*parent_.map_)[key_val] = value_val;
            return *this;
        }
        
        element_proxy& operator=(V&& value) {
            if (!parent_.map_) throw runtime_error("Cannot assign to null bound_map");
            
            auto key_val = make_script_value(key_, parent_.get_engine());
            auto value_val = make_script_value(std::forward<V>(value), parent_.get_engine());
            parent_.map_->insert_or_assign(key_val, value_val);
            return *this;
        }
        
        operator V() const {
            auto it = parent_.find_key(key_);
            if (it != parent_.map_->end()) {
                return extract_value<V>(it->second);
            }
            // Key doesn't exist, return default value
            return V{};
        }
        
        V get() const {
            return operator V();
        }
        
        // Support for nested bound_map operations
        template<typename U = V>
        typename std::enable_if<is_bound_map_v<U>, typename U::element_proxy>::type
        operator[](const typename U::key_type& nested_key) {
            auto key_val = make_script_value(key_, parent_.get_engine());
            auto it = parent_.map_->find(key_val);
            
            if (it == parent_.map_->end()) {
                // Create new nested map if it doesn't exist
                using nested_key_type = typename U::key_type;
                using nested_value_type = typename U::mapped_type;
                script_value nested_map = script_value::make_map(
                    type_info::make<nested_key_type>(),
                    type_info::make<nested_value_type>()
                );
                (*parent_.map_)[key_val] = nested_map;
                it = parent_.map_->find(key_val);
            }
            
            if (!it->second.is_map()) {
                throw runtime_error("Element is not a map");
            }
            
            // Store the wrapper in our storage to keep it alive (zero-copy wrapper around existing script_value)
            nested_wrapper_storage_ = std::make_unique<std::any>(U(it->second.as_map()));
            U& nested_bound_map = std::any_cast<U&>(*nested_wrapper_storage_);
            return typename U::element_proxy(nested_bound_map, nested_key);
        }
        
        // Support for nested bound_array operations  
        template<typename U = V>
        typename std::enable_if<is_bound_array_v<U>, typename U::element_proxy>::type
        operator[](size_type pos) {
            auto key_val = make_script_value(key_, parent_.get_engine());
            auto it = parent_.map_->find(key_val);
            
            if (it == parent_.map_->end() || !it->second.is_array()) {
                throw runtime_error("Element is not an array");
            }
            
            script_array& nested = *it->second.unchecked_get_array_storage();
            return typename U::element_proxy(nested, pos, it->second.get_engine());
        }
        
        // Const version for reading nested arrays
        template<typename U = V>
        typename std::enable_if<is_bound_array_v<U>, typename U::value_type>::type
        operator[](size_type pos) const {
            auto it = parent_.find_key(key_);
            if (it == parent_.map_->end() || !it->second.is_array()) {
                throw runtime_error("Element is not an array");
            }
            
            const script_array& nested = *it->second.unchecked_array_node();
            return U::extract_value(nested.get(pos, it->second.get_engine()));
        }
        
        // Support for iterating nested bound_map
        template<typename U = V>
        typename std::enable_if<is_bound_map_v<U>, typename U::iterator>::type
        begin() {
            auto key_val = make_script_value(key_, parent_.get_engine());
            auto it = parent_.map_->find(key_val);
            
            if (it == parent_.map_->end() || !it->second.is_map()) {
                throw runtime_error("Element is not a map");
            }
            
            auto& nested_map = it->second.as_map();
            return typename U::iterator(nested_map.begin());
        }
        
        template<typename U = V>
        typename std::enable_if<is_bound_map_v<U>, typename U::iterator>::type
        end() {
            auto key_val = make_script_value(key_, parent_.get_engine());
            auto it = parent_.map_->find(key_val);
            
            if (it == parent_.map_->end() || !it->second.is_map()) {
                throw runtime_error("Element is not a map");
            }
            
            auto& nested_map = it->second.as_map();
            return typename U::iterator(nested_map.end());
        }
    };
    
    element_proxy operator[](const K& key) {
        return element_proxy(*this, key);
    }
    
    V operator[](const K& key) const {
        auto it = find_key(key);
        if (it != map_->end()) {
            return extract_value<V>(it->second);
        }
        return V{};
    }
    
    // ===== Iterators =====
    
    class iterator {
        script_map::iterator it_;
        
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::pair<K, V>;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type;
        
        iterator() = default;
        explicit iterator(script_map::iterator it) : it_(it) {}
        
        std::pair<K, V> operator*() const {
            return {extract_value<K>(it_->first), extract_value<V>(it_->second)};
        }
        
        iterator& operator++() { ++it_; return *this; }
        iterator operator++(int) { iterator tmp = *this; ++it_; return tmp; }
        
        bool operator==(const iterator& other) const { return it_ == other.it_; }
        bool operator!=(const iterator& other) const { return it_ != other.it_; }
    };
    
    class const_iterator {
        script_map::const_iterator it_;
        
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::pair<K, V>;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type;
        
        const_iterator() = default;
        explicit const_iterator(script_map::const_iterator it) : it_(it) {}
        const_iterator(const iterator& other) : it_(other.it_) {}
        
        std::pair<K, V> operator*() const {
            return {extract_value<K>(it_->first), extract_value<V>(it_->second)};
        }
        
        const_iterator& operator++() { ++it_; return *this; }
        const_iterator operator++(int) { const_iterator tmp = *this; ++it_; return tmp; }
        
        bool operator==(const const_iterator& other) const { return it_ == other.it_; }
        bool operator!=(const const_iterator& other) const { return it_ != other.it_; }
    };
    
    iterator begin() { return map_ ? iterator(map_->begin()) : iterator(); }
    iterator end() { return map_ ? iterator(map_->end()) : iterator(); }
    
    const_iterator begin() const { return map_ ? const_iterator(map_->begin()) : const_iterator(); }
    const_iterator end() const { return map_ ? const_iterator(map_->end()) : const_iterator(); }
    
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }
    
    iterator find(const K& key) {
        auto it = find_key(key);
        return iterator(it);
    }
    
    const_iterator find(const K& key) const {
        auto it = find_key(key);
        return const_iterator(it);
    }
    
    // ===== Conversion =====
    
    /**
     * @brief Convert to std::map<K, V> (always creates a copy)
     */
    std::map<K, V> to_map() const {
        std::map<K, V> result;
        if (map_) {
            for (const auto& [key, value] : *map_) {
                result[extract_value<K>(key)] = extract_value<V>(value);
            }
        }
        return result;
    }
    
    /**
     * @brief Get underlying script_value (for owned maps only)
     */
    const script_value& as_script_value() const {
        if (!owned_value_) {
            throw runtime_error("Cannot get script_value from non-owned bound_map");
        }
        return *owned_value_;
    }
};

} // namespace jai

#endif // __JAISCRIPT_CORE_BOUND_MAP_HPP__