#pragma once

#ifndef __JAISCRIPT_DETAIL_ENV_STORAGE_HPP__
#define __JAISCRIPT_DETAIL_ENV_STORAGE_HPP__

#include <jaiscript/core/value.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jai::detail {

    // The spilled form of env_id_map / env_id_set: one open-addressed allocation (keys, then values), linear probing,
    // grown at half full; a spill is one allocation instead of a bucket array and a node per entry
    template <class V>
    class env_flat_table {
        static constexpr uint64_t k_empty = UINT64_MAX;
        static constexpr uint64_t k_gone = UINT64_MAX - 1;
        uint64_t* keys_ = nullptr;
        V* values_ = nullptr;
        uint32_t mask_ = 0;
        uint32_t used_ = 0;    // live + gone
        uint32_t live_ = 0;

        static size_t index_of(uint64_t key, uint32_t mask) noexcept { return static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> 32) & mask; }
        void release() noexcept {
            if (!keys_) { return; }
            for (uint32_t i = 0; i <= mask_; ++i) { if (keys_[i] < k_gone) { values_[i].~V(); } }
            ::operator delete(static_cast<void*>(keys_));
            keys_ = nullptr;
        }
        void rebuild(uint32_t capacity) {
            uint64_t* old_keys = keys_;
            V* old_values = values_;
            const uint32_t old_mask = mask_;
            void* memory = ::operator new(capacity * (sizeof(uint64_t) + sizeof(V)));
            keys_ = static_cast<uint64_t*>(memory);
            values_ = reinterpret_cast<V*>(keys_ + capacity);
            mask_ = capacity - 1;
            used_ = live_ = 0;
            for (uint32_t i = 0; i < capacity; ++i) { keys_[i] = k_empty; }
            if (old_keys) {
                for (uint32_t i = 0; i <= old_mask; ++i) {
                    if (old_keys[i] < k_gone) {
                        insert_new(old_keys[i]) = std::move(old_values[i]);
                        old_values[i].~V();
                    }
                }
                ::operator delete(static_cast<void*>(old_keys));
            }
        }
        V& insert_new(uint64_t key) {
            size_t i = index_of(key, mask_);
            while (keys_[i] < k_gone) { i = (i + 1) & mask_; }
            if (keys_[i] == k_empty) { ++used_; }
            keys_[i] = key;
            ++live_;
            return *new (values_ + i) V{};
        }

    public:
        explicit env_flat_table(uint32_t capacity) { rebuild(capacity); }
        env_flat_table(const env_flat_table&) = delete;
        env_flat_table& operator=(const env_flat_table&) = delete;
        ~env_flat_table() { release(); }

        V* find(uint64_t key) noexcept {
            for (size_t i = index_of(key, mask_);; i = (i + 1) & mask_) {
                if (keys_[i] == key) { return values_ + i; }
                if (keys_[i] == k_empty) { return nullptr; }
            }
        }
        V& operator[](uint64_t key) {
            if (V* found = find(key)) { return *found; }
            if ((used_ + 1) * 2 > mask_ + 1) { rebuild(live_ * 4 > mask_ + 1 ? (mask_ + 1) * 2 : mask_ + 1); }
            return insert_new(key);
        }
        bool erase(uint64_t key) noexcept {
            V* found = find(key);
            if (!found) { return false; }
            const size_t i = static_cast<size_t>(found - values_);
            found->~V();
            keys_[i] = k_gone;
            --live_;
            return true;
        }
        void clear() noexcept {
            for (uint32_t i = 0; i <= mask_; ++i) {
                if (keys_[i] < k_gone) { values_[i].~V(); }
                keys_[i] = k_empty;
            }
            used_ = live_ = 0;
        }
        size_t size() const noexcept { return live_; }
        template <class F> void for_each(F&& f) const {
            for (uint32_t i = 0; i <= mask_; ++i) { if (keys_[i] < k_gone) { f(keys_[i], values_[i]); } }
        }
        template <class P> void erase_if(P&& drop) {
            for (uint32_t i = 0; i <= mask_; ++i) {
                if (keys_[i] < k_gone && drop(keys_[i], values_[i])) { values_[i].~V(); keys_[i] = k_gone; --live_; }
            }
        }
    };

    // An environment's id -> value table: the first entries live in the environment itself (a scope or a closure
    // defines a handful of names), a flat table takes over once they outgrow it and stays for the env's life
    template <class V, size_t N = 4>
    class env_id_map {
        struct slot { uint64_t key; V value; };
        slot inline_[N];
        uint32_t count_ = 0;
        std::unique_ptr<env_flat_table<V>> big_;

        void spill() {
            big_ = std::make_unique<env_flat_table<V>>(static_cast<uint32_t>(N * 4));
            for (uint32_t i = 0; i < count_; ++i) { (*big_)[inline_[i].key] = inline_[i].value; }
            count_ = 0;
        }

    public:
        env_id_map() = default;
        env_id_map(const env_id_map&) = delete;
        env_id_map& operator=(const env_id_map&) = delete;

        V* find(uint64_t key) noexcept {
            if (big_) { return big_->find(key); }
            for (uint32_t i = 0; i < count_; ++i) {
                if (inline_[i].key == key) { return &inline_[i].value; }
            }
            return nullptr;
        }
        const V* find(uint64_t key) const noexcept { return const_cast<env_id_map*>(this)->find(key); }
        bool contains(uint64_t key) const noexcept { return find(key) != nullptr; }

        V& operator[](uint64_t key) {
            if (V* found = find(key)) { return *found; }
            if (!big_) {
                if (count_ < N) {
                    inline_[count_] = slot{key, V{}};
                    return inline_[count_++].value;
                }
                spill();
            }
            return (*big_)[key];
        }

        bool erase(uint64_t key) {
            if (big_) { return big_->erase(key); }
            for (uint32_t i = 0; i < count_; ++i) {
                if (inline_[i].key == key) {
                    inline_[i] = inline_[--count_];
                    return true;
                }
            }
            return false;
        }

        void clear() noexcept {
            count_ = 0;
            if (big_) { big_->clear(); }
        }
        bool empty() const noexcept { return big_ ? big_->size() == 0 : count_ == 0; }
        size_t size() const noexcept { return big_ ? big_->size() : count_; }

        template <class F> void for_each(F&& f) const {
            if (big_) {
                big_->for_each(f);
                return;
            }
            for (uint32_t i = 0; i < count_; ++i) { f(inline_[i].key, inline_[i].value); }
        }
        template <class P> void erase_if(P&& drop) {
            if (big_) {
                big_->erase_if(drop);
                return;
            }
            for (uint32_t i = 0; i < count_;) {
                if (drop(inline_[i].key, inline_[i].value)) { inline_[i] = inline_[--count_]; } else { ++i; }
            }
        }
    };

    // The ids an environment defines: the first N inline, a flat table past them
    template <size_t N = 4>
    class env_id_set {
        struct present {};
        uint64_t inline_[N];
        uint32_t count_ = 0;
        std::unique_ptr<env_flat_table<present>> big_;

    public:
        env_id_set() = default;
        env_id_set(const env_id_set&) = delete;
        env_id_set& operator=(const env_id_set&) = delete;

        bool contains(uint64_t key) const noexcept {
            if (big_) { return big_->find(key) != nullptr; }
            for (uint32_t i = 0; i < count_; ++i) {
                if (inline_[i] == key) { return true; }
            }
            return false;
        }
        void insert(uint64_t key) {
            if (contains(key)) { return; }
            if (!big_) {
                if (count_ < N) {
                    inline_[count_++] = key;
                    return;
                }
                big_ = std::make_unique<env_flat_table<present>>(static_cast<uint32_t>(N * 4));
                for (uint32_t i = 0; i < count_; ++i) { (*big_)[inline_[i]]; }
                count_ = 0;
            }
            (*big_)[key];
        }
        void clear() noexcept {
            count_ = 0;
            if (big_) { big_->clear(); }
        }
        bool empty() const noexcept { return big_ ? big_->size() == 0 : count_ == 0; }
        size_t size() const noexcept { return big_ ? big_->size() : count_; }
        template <class F> void for_each(F&& f) const {
            if (big_) {
                big_->for_each([&](uint64_t key, const present&) { f(key, true); });
                return;
            }
            for (uint32_t i = 0; i < count_; ++i) { f(inline_[i], true); }
        }
    };

    // An environment's own values in declaration order, each at a fixed address for the env's life (lookups and the vm's
    // caches hold pointers into it): the first few inside the environment, the rest in fixed chunks, none moved on growth
    class env_value_store {
        static constexpr size_t k_inline = 4;
        static constexpr size_t k_chunk = 16;
        struct chunk { alignas(script_value) unsigned char bytes[k_chunk * sizeof(script_value)]; };
        alignas(script_value) unsigned char inline_[k_inline * sizeof(script_value)];
        std::unique_ptr<std::vector<std::unique_ptr<chunk>>> chunks_;   // made when the inline cells fill
        size_t size_ = 0;

        script_value* cell(size_t i) noexcept {
            if (i < k_inline) { return reinterpret_cast<script_value*>(inline_) + i; }
            const size_t rest = i - k_inline;
            return reinterpret_cast<script_value*>((*chunks_)[rest / k_chunk]->bytes) + rest % k_chunk;
        }
        void* next_cell() {
            if (size_ >= k_inline) {
                const size_t rest = size_ - k_inline;
                if (!chunks_) { chunks_ = std::make_unique<std::vector<std::unique_ptr<chunk>>>(); }
                if (rest / k_chunk == chunks_->size()) { chunks_->push_back(std::make_unique<chunk>()); }
            }
            return cell(size_);
        }

    public:
        env_value_store() = default;
        env_value_store(const env_value_store&) = delete;
        env_value_store& operator=(const env_value_store&) = delete;
        ~env_value_store() { while (size_ != 0) { pop_back(); } }   // reverse declaration order (C++)

        script_value& push_back(const script_value& value) {
            void* at = next_cell();
            auto* placed = new (at) script_value(value);
            ++size_;
            return *placed;
        }
        script_value& push_back(script_value&& value) {
            void* at = next_cell();
            auto* placed = new (at) script_value(std::move(value));
            ++size_;
            return *placed;
        }
        void pop_back() noexcept {
            script_value* last = cell(size_ - 1);
            --size_;   // the slot leaves the store before its value dies (a destructor reentering the env sees it gone)
            last->~script_value();
        }
        void clear() noexcept { while (size_ != 0) { pop_back(); } }
        script_value& back() noexcept { return *cell(size_ - 1); }
        bool empty() const noexcept { return size_ == 0; }
        size_t size() const noexcept { return size_; }
        script_value& operator[](size_t i) noexcept { return *cell(i); }
        const script_value& operator[](size_t i) const noexcept { return *const_cast<env_value_store*>(this)->cell(i); }

        template <class Store, class Value>
        class basic_iterator {
            Store* store_;
            size_t i_;
        public:
            basic_iterator(Store* store, size_t i) : store_(store), i_(i) {}
            Value& operator*() const noexcept { return (*store_)[i_]; }
            basic_iterator& operator++() noexcept { ++i_; return *this; }
            bool operator!=(const basic_iterator& other) const noexcept { return i_ != other.i_; }
            bool operator==(const basic_iterator& other) const noexcept { return i_ == other.i_; }
        };
        using iterator = basic_iterator<env_value_store, script_value>;
        using const_iterator = basic_iterator<const env_value_store, const script_value>;
        iterator begin() noexcept { return {this, 0}; }
        iterator end() noexcept { return {this, size_}; }
        const_iterator begin() const noexcept { return {this, 0}; }
        const_iterator end() const noexcept { return {this, size_}; }
    };

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_ENV_STORAGE_HPP__
