// Part of the engine's closure, which includes it in its order: a translation unit that names it first is sent there
#if !defined(__JAISCRIPT_CORE_ENGINE_HPP__)
#include "engine.hpp"
#elif !defined(__JAISCRIPT_CORE_SCRIPT_MAP_HPP__)
#define __JAISCRIPT_CORE_SCRIPT_MAP_HPP__

// script_map: the map behind TYPEID_MAP. Lookup is hashed through an open-addressing index (keys are equal exactly when
// script_value::operator<=> calls them equivalent: type rank, then payload, a char ranking as its one-character string; floats by value with every NaN one key and both zeros one, heavy values by identity); key order exists only where it is observed -
// iteration and everything built on it (serialization, keys()/values(), structural equality) - through a key-sorted view of the
// entries built on the first iteration after a change. Entries never move: a reference or pointer to a key or value stays valid
// until that entry is erased. An iteration iterator carries its key, so after the map changes under it (the body of a range-for
// inserting or erasing) it continues with the next greater key, as a std::map iterator walks; a find() iterator carries none
// (concurrent readers of a borrowed map only find). A class value key (an instance or a bound object held by value, not a
// handle) keys by its class: through the class's < or <=> (equivalent when neither is less), else its hash() with its ==, else
// by identity. Ordered and hashed class keys are found through class_keys_, one sorted run per class (by the class's order, or
// by hash() value), and sit in the hashed index under their identity as well, so every walk over entries sees them; an
// identity key is an ordinary entry. Included at the end of value.hpp, where script_value is complete.

#include <bit>
#include <cstring>
#include <string_view>
#include <tuple>
#include <optional>
#include <stdexcept>
#include <vector>
#include <memory>
#include <cstddef>
#include <algorithm>
#include <iterator>
#include <utility>
#include <jaiscript/detail/copy_census.hpp>
#include <jaiscript/detail/text_hash.hpp>

namespace jai {

    // operator<=> ranks a string by its type first, so the type seeds the text's hash
    inline uint64_t script_value::map_string_key_hash(std::string_view text) noexcept {
        return static_cast<uint64_t>(detail::text_hash(text)) ^ (static_cast<uint64_t>(script_value_type::jai_string_type) * 0x9E3779B97F4A7C15ull);
    }

    // the payload mix of script_value::map_key_hash (seeded by the type rank); hashes stay 64-bit on a 32-bit host, so a map
    // keyed by numbers lays out (and destroys its entries) in the same order there
    inline uint64_t map_key_mix(script_value_type t, uint64_t x) noexcept {
        x ^= static_cast<uint64_t>(t) * 0x9E3779B97F4A7C15ull ^ detail::k_process_hash_seed;
        x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
        return x;
    }

    struct script_value_hash {
        using is_transparent = void;
        uint64_t operator()(const script_value& v) const noexcept {
            const size_t k = v.raw_storage_index();   // the two common key kinds inline, the rows map_key_hash gives them
            JAI_CENSUS_F(map_key_hash, k);
            if (k == script_value::TYPEID_INT) return map_key_mix(script_value_type::jai_int_type, static_cast<uint64_t>(v.unchecked_as_int()));
            if (k == script_value::TYPEID_STRING) return script_value::map_string_key_hash(v.unchecked_as_string());
            return v.map_key_hash();
        }
        uint64_t operator()(const map_string_key_probe& p) const noexcept { JAI_CENSUS_F(map_key_hash, 0x80); return script_value::map_string_key_hash(p.text); }
    };

    struct script_value_key_eq {
        using is_transparent = void;
        bool operator()(const script_value& a, const script_value& b) const {
            const size_t k = a.raw_storage_index();   // same-kind ints and strings compare their payloads, as operator<=> does
            if (k == b.raw_storage_index()) {
                if (k == script_value::TYPEID_INT) return a.unchecked_as_int() == b.unchecked_as_int();
                if (k == script_value::TYPEID_STRING) {
                    const std::string& x = a.unchecked_as_string();
                    const std::string& y = b.unchecked_as_string();
                    return &x == &y || x == y;   // copies of one string share its node
                }
            }
            return (a <=> b) == 0;
        }
        bool operator()(const script_value& a, const map_string_key_probe& p) const {
            if (a.type() == script_value_type::jai_string_type) { return a.unchecked_as_string() == p.text; }
            return a.type() == script_value_type::jai_char_type && a.key_text() == p.text;
        }
        bool operator()(const map_string_key_probe& p, const script_value& b) const { return (*this)(b, p); }
    };

    // A map key's place among class value keys (value.cpp): its class when the class orders or hashes its keys, else null
    const class_definition* map_class_key_class(const script_value& key);
    // a < b, a == b or a > b as map keys (-1, 0, 1): class keys of one class by their class, otherwise operator<=>; a key's
    // operator that fails raises it
    int map_class_key_order(const script_value& a, const script_value& b);
    inline int map_key_order(const script_value& a, const script_value& b) {
        if (a.raw_storage_index() == script_value::TYPEID_OBJECT && b.raw_storage_index() == script_value::TYPEID_OBJECT) [[unlikely]] {
            return map_class_key_order(a, b);
        }
        const auto c = a <=> b;
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }

    class script_map {
    public:
        using key_type = script_value;
        using mapped_type = script_value;
        using value_type = std::pair<const script_value, script_value>;
        using size_type = size_t;
        using difference_type = ptrdiff_t;
        using key_compare = script_value_map_less;

    private:
        // An entry lives in its own allocation until it is erased (references and pointers into it stay valid); the index is
        // open addressing over {hash, entry}: a probe compares the cached hash before it touches an entry
        // A string key of at most sixteen bytes keeps them beside the hash: a probe settles it without reaching the key's text
        static constexpr uint32_t no_short = UINT32_MAX;
        struct short_key { uint64_t low = 0; uint64_t high = 0; uint32_t len = no_short; };
        JAI_FORCEINLINE static short_key short_of(std::string_view t) noexcept {
            short_key k;
            const size_t n = t.size();
            if (n <= 16) {
                k.low = detail::text_pack_short(t.data(), n < 8 ? n : 8);
                k.high = n > 8 ? detail::text_pack_short(t.data() + 8, n - 8) : 0;
                k.len = static_cast<uint32_t>(n);
            }
            return k;
        }
        JAI_FORCEINLINE static short_key short_of(const script_value& v) noexcept {
            if (v.raw_storage_index() == script_value::TYPEID_STRING) { return short_of(std::string_view(v.unchecked_as_string())); }
            return v.raw_storage_index() == script_value::TYPEID_CHAR ? short_of(v.key_text()) : short_key{};
        }
        JAI_FORCEINLINE static short_key short_of(const map_string_key_probe& p) noexcept { return short_of(p.text); }
        struct entry {
            uint64_t hash;
            short_key brief;
            value_type kv;
            template <typename K, typename... V>
            entry(uint64_t h, K&& k, V&&... v) : hash(h), kv(std::piecewise_construct, std::forward_as_tuple(std::forward<K>(k)), std::forward_as_tuple(std::forward<V>(v)...)) {
                brief = short_of(kv.first);
            }
        };
        // the probe's key test: two short string keys by their bytes, anything else by the key equality
        template <typename K>
        static bool same_key(const entry* e, const short_key& b, const K& key) {
            if (b.len != no_short && e->brief.len != no_short) { return b.len == e->brief.len && b.low == e->brief.low && b.high == e->brief.high; }
            return script_value_key_eq{}(e->kv.first, key);
        }
        // the entries' storage: chunks that never move (an entry stays where it was made until it is erased), filled in
        // insertion order so a map built at once keeps its entries together; an erased entry's storage is reused
        class entry_pool {
            struct free_node { free_node* next; };
            static constexpr size_t k_first_chunk = 2;   // most maps are small records; chunks double from here
            std::vector<std::unique_ptr<std::byte[]>> chunks_;
            std::byte* cursor_ = nullptr;
            size_t left_ = 0;
            size_t next_count_ = k_first_chunk;
            free_node* free_ = nullptr;
            void reset() noexcept { cursor_ = nullptr; left_ = 0; next_count_ = k_first_chunk; free_ = nullptr; }
        public:
            entry_pool() = default;
            entry_pool(const entry_pool&) = delete;
            entry_pool& operator=(const entry_pool&) = delete;
            entry_pool(entry_pool&& o) noexcept
                : chunks_(std::move(o.chunks_)), cursor_(o.cursor_), left_(o.left_), next_count_(o.next_count_), free_(o.free_) { o.chunks_.clear(); o.reset(); }
            entry_pool& operator=(entry_pool&& o) noexcept {
                chunks_ = std::move(o.chunks_); o.chunks_.clear();
                cursor_ = o.cursor_; left_ = o.left_; next_count_ = o.next_count_; free_ = o.free_;
                o.reset();
                return *this;
            }
            // the first chunk sized for count entries (a map built at once in one allocation)
            void reserve(size_t count) {
                if (!chunks_.empty() || count == 0) return;
                next_count_ = std::min<size_t>(count, 4096);
            }
            template <typename... A>
            entry* make(A&&... a) {
                void* p;
                if (free_) { p = free_; free_ = free_->next; }
                else {
                    if (left_ == 0) {
                        chunks_.emplace_back(new std::byte[next_count_ * sizeof(entry)]);
                        cursor_ = chunks_.back().get(); left_ = next_count_;
                        next_count_ = std::min<size_t>(next_count_ * 2, 512);
                    }
                    p = cursor_; cursor_ += sizeof(entry); --left_;
                }
                try { return new (p) entry(std::forward<A>(a)...); }
                catch (...) { auto* n = static_cast<free_node*>(p); n->next = free_; free_ = n; throw; }
            }
            void destroy(entry* e) noexcept { e->~entry(); auto* n = reinterpret_cast<free_node*>(e); n->next = free_; free_ = n; }
        };
        struct slot { uint64_t hash; entry* e; };
        struct class_key_index;
        struct class_key_index_deleter { void operator()(class_key_index* index) const noexcept; };
        // where a class key is (found) or goes (run, at) among its class's keys
        struct class_probe { entry* found = nullptr; size_t run = 0; size_t at = 0; uint64_t hash = 0; script_int key_hash = 0; };
        static constexpr size_t min_capacity = 4;

        entry_pool pool_;
        std::vector<slot> slots_;                   // empty, or a power of two
        size_t size_ = 0;
        unsigned shift_ = 64;                       // 64 - log2(capacity): the home slot is the top bits of hash * golden ratio
        bool enum_values_ = false;                  // an enum's own value table: its keys read with a dot (Direction.north); never copied
        uint64_t gen_ = 1;                          // moves on every insert, erase or clear
        mutable std::vector<value_type*> order_;    // the entries by key, valid while order_gen_ == gen_
        mutable uint64_t order_gen_ = 0;
        std::unique_ptr<class_key_index, class_key_index_deleter> class_keys_;   // null until a class key arrives
        struct entry_memo_storage { const script_map* owner = nullptr; uint64_t gen = 0; const void* e = nullptr; };
        mutable entry_memo_storage lane_memo_;      // the vm's main-thread map lanes' memo (a copy names its source: dead by the owner test)

        void changed() noexcept { ++gen_; }
        class_probe probe_class(const script_value& key, const class_definition* cls) const;
        void insert_class_entry(const class_definition* cls, const class_probe& at, entry* e);
        void erase_class_entry(const class_probe& at) noexcept;
        void refresh_class_runs() const;
        void copy_class_keys(const script_map& o, const std::vector<std::pair<const entry*, entry*>>& copies);
        void build_order_with_class_keys() const;
        size_t index_after_class_key(const script_value& key, const class_definition* cls) const;
        // the index without e (backward shift); e stays allocated
        void unlink_entry(entry* gone) noexcept {
            size_t i = home(gone->hash);
            while (slots_[i].e != gone) { i = (i + 1) & mask(); }
            remove_slot(i);
        }
        // slot i's entry leaves the index: the probe run after it shifts back over the hole
        void remove_slot(size_t i) noexcept {
            for (size_t j = i;;) {
                j = (j + 1) & mask();
                if (!slots_[j].e) break;
                const size_t k = home(slots_[j].hash);
                if ((j > i && (k <= i || k > j)) || (j < i && k <= i && k > j)) { slots_[i] = slots_[j]; i = j; }
            }
            slots_[i] = slot{0, nullptr};
            --size_;
            changed();
        }
        size_t home(uint64_t h) const noexcept { return static_cast<size_t>((h * 0x9E3779B97F4A7C15ull) >> shift_); }
        size_t mask() const noexcept { return slots_.size() - 1; }

        template <typename K>
        entry* find_entry(const K& key) const {
            if (size_ == 0) return nullptr;
            uint64_t h;
            if constexpr (std::is_same_v<K, script_value>) {
                const size_t k = key.raw_storage_index();
                if (k == script_value::TYPEID_INT) return find_int_entry(key.unchecked_as_int(), &key, nullptr);
                if (k == script_value::TYPEID_STRING) {
                    h = script_value::map_string_key_hash(key.unchecked_as_string());
                } else {
                    if (k == script_value::TYPEID_OBJECT) [[unlikely]] {
                        if (const class_definition* cls = map_class_key_class(key)) { return probe_class(key, cls).found; }
                    }
                    h = key.map_key_hash();
                }
            } else {
                h = script_value_hash{}(key);
            }
            const short_key b = short_of(key);
            for (size_t i = home(h);; i = (i + 1) & mask()) {
                const slot& s = slots_[i];
                if (!s.e) return nullptr;
                if (s.hash == h && same_key(s.e, b, key)) return s.e;
            }
        }
        // an int key's probe: an int entry compares its payload in place; any other kind under the same hash takes the key equality
        entry* find_int_entry(script_int x, const script_value* key, engine* eng) const {
            const uint64_t h = map_key_mix(script_value_type::jai_int_type, static_cast<uint64_t>(x));
            for (size_t i = home(h);; i = (i + 1) & mask()) {
                const slot& s = slots_[i];
                if (!s.e) return nullptr;
                if (s.hash != h) continue;
                const script_value& k = s.e->kv.first;
                if (k.raw_storage_index() == script_value::TYPEID_INT ? k.unchecked_as_int() == x
                                                                     : key ? script_value_key_eq{}(k, *key) : script_value_key_eq{}(k, script_value(x, eng))) return s.e;
            }
        }
        void place(entry* e) noexcept {
            size_t i = home(e->hash);
            while (slots_[i].e) { i = (i + 1) & mask(); }
            slots_[i] = slot{e->hash, e};
        }
        void grow_for(size_t count) {
            if (count * 8 <= slots_.size() * 7) return;
            size_t cap = slots_.empty() ? min_capacity : slots_.size() * 2;
            while (count * 8 > cap * 7) { cap *= 2; }
            std::vector<slot> old(cap, slot{0, nullptr});
            old.swap(slots_);
            shift_ = 64u - static_cast<unsigned>(std::countr_zero(cap));
            for (const slot& s : old) { if (s.e) place(s.e); }
        }
        // the entry for key, inserted (made by make(hash, key)) when absent
        template <typename K, typename Make>
        std::pair<entry*, bool> find_or_insert(K&& key, Make&& make) {
            uint64_t h;
            if constexpr (std::is_same_v<std::decay_t<K>, script_value>) {
                const size_t k = key.raw_storage_index();
                if (k == script_value::TYPEID_INT) {
                    h = map_key_mix(script_value_type::jai_int_type, static_cast<uint64_t>(key.unchecked_as_int()));
                } else if (k == script_value::TYPEID_STRING) {
                    h = script_value::map_string_key_hash(key.unchecked_as_string());
                } else {
                    if (k == script_value::TYPEID_OBJECT) [[unlikely]] {
                        if (const class_definition* cls = map_class_key_class(key)) { return insert_class_key(std::forward<K>(key), make, cls); }
                    }
                    h = key.map_key_hash();
                }
            } else {
                h = script_value_hash{}(key);
            }
            if (size_ != 0) {
                const short_key b = short_of(key);
                for (size_t i = home(h);; i = (i + 1) & mask()) {
                    const slot& s = slots_[i];
                    if (!s.e) break;
                    if (s.hash == h && same_key(s.e, b, key)) return {s.e, false};
                }
            }
            grow_for(size_ + 1);
            entry* e = make(h, std::forward<K>(key));
            place(e);
            ++size_;
            changed();
            return {e, true};
        }
        template <typename K, typename Make>
        JAI_NOINLINE std::pair<entry*, bool> insert_class_key(K&& key, Make& make, const class_definition* cls) {
            const class_probe at = probe_class(key, cls);
            if (at.found) return {at.found, false};
            grow_for(size_ + 1);
            entry* e = make(at.hash, std::forward<K>(key));
            place(e);
            ++size_;
            changed();
            insert_class_entry(cls, at, e);
            return {e, true};
        }
        // the index without key's entry (backward shift: no tombstones); the entry, still allocated, or null
        entry* unlink(const script_value& key) {
            if (size_ == 0) return nullptr;
            if (key.raw_storage_index() == script_value::TYPEID_OBJECT) [[unlikely]] {
                if (const class_definition* cls = map_class_key_class(key)) {
                    const class_probe at = probe_class(key, cls);
                    entry* gone = at.found;
                    if (!gone) return nullptr;
                    erase_class_entry(at);
                    unlink_entry(gone);
                    return gone;
                }
            }
            const uint64_t h = script_value_hash{}(key);
            size_t i = home(h);
            for (;; i = (i + 1) & mask()) {
                const slot& s = slots_[i];
                if (!s.e) return nullptr;
                if (s.hash == h && script_value_key_eq{}(s.e->kv.first, key)) break;
            }
            entry* gone = slots_[i].e;
            remove_slot(i);
            return gone;
        }
        // an entry whose death can run a destructor (an instance, a container or closure that may hold one, a reference's pin)
        static bool dies_observably(const script_value& v) noexcept {
            switch (v.raw_storage_index()) {
                case script_value::TYPEID_ARRAY: return v.unchecked_get_array_storage()->kind() == script_array::kind_t::hetero;
                case script_value::TYPEID_MAP: case script_value::TYPEID_OBJECT: case script_value::TYPEID_FUNCTION:
                case script_value::TYPEID_REFERENCE: case script_value::TYPEID_CPP_BOUND:
                    return true;
                default:
                    return false;
            }
        }
        bool any_dies_observably() const noexcept {
            for (const slot& s : slots_) {
                if (s.e && (dies_observably(s.e->kv.second) || dies_observably(s.e->kv.first))) { return true; }
            }
            return false;
        }
        void order_by_key(std::vector<entry*>& entries) const noexcept;   // value.cpp: iteration's order, no key operator run
        // every entry, detached from the index: the map is empty and consistent before any value dies; entries die last key
        // first when a death can be observed, as a container's elements die last to first (and std::map's nodes do)
        std::vector<entry*> detach_all() noexcept {
            std::vector<entry*> out;
            out.reserve(size_);
            for (const slot& s : slots_) { if (s.e) { out.push_back(s.e); } }
            if (any_dies_observably()) {
                order_by_key(out);
                std::reverse(out.begin(), out.end());
            }
            for (slot& s : slots_) { s = slot{0, nullptr}; }
            size_ = 0;
            class_keys_.reset();
            return out;
        }
        void copy_from(const script_map& o) {
            grow_for(o.size_);
            std::vector<std::pair<const entry*, entry*>> copies;
            if (o.class_keys_) { copies.reserve(o.size_); }
            for (const slot& s : o.slots_) {
                if (!s.e) continue;
                entry* e = pool_.make(s.hash, s.e->kv.first, s.e->kv.second);
                place(e);
                ++size_;
                if (o.class_keys_) { copies.emplace_back(s.e, e); }
            }
            if (o.class_keys_) { copy_class_keys(o, copies); }
        }
        void take_from(script_map& o) noexcept {
            pool_ = std::move(o.pool_);
            slots_ = std::move(o.slots_); o.slots_.clear();
            size_ = o.size_; o.size_ = 0;
            shift_ = o.shift_; o.shift_ = 64;
            class_keys_ = std::move(o.class_keys_);
        }

        void build_order() const {
            if (class_keys_) [[unlikely]] { build_order_with_class_keys(); return; }   // a reload may have changed a class's order
            if (order_gen_ == gen_) return;
            order_.clear();
            order_.reserve(size_);
            for (const slot& s : slots_) { if (s.e) order_.push_back(&s.e->kv); }
            std::sort(order_.begin(), order_.end(), [](const value_type* a, const value_type* b) { return script_value_map_less{}(a->first, b->first); });
            order_gen_ = gen_;
        }
        static constexpr size_t npos = static_cast<size_t>(-1);
        // the position after `key` in key order (the first entry whose key is greater)
        size_t index_after(const script_value& key) const {
            build_order();
            if (class_keys_ && key.raw_storage_index() == script_value::TYPEID_OBJECT) [[unlikely]] {
                if (const class_definition* cls = map_class_key_class(key)) { return index_after_class_key(key, cls); }
            }
            auto it = std::upper_bound(order_.begin(), order_.end(), key,
                [](const script_value& k, const value_type* e) { return script_value_map_less{}(k, e->first); });
            return static_cast<size_t>(it - order_.begin());
        }

    public:
        template <bool Const>
        class basic_iterator {
            friend class script_map;
            template <bool> friend class basic_iterator;
            using map_ptr = std::conditional_t<Const, const script_map*, script_map*>;
            map_ptr m_ = nullptr;
            mutable script_map::value_type* node_ = nullptr;   // null = end
            mutable size_t index_ = npos;          // the node's place in the order view it was read from (npos: a find() result)
            mutable uint64_t gen_ = 0;             // the map generation that view belongs to
            mutable std::optional<script_value> key_;   // an iteration iterator's key (the resync point after a change)
            // an iteration iterator after the map changed: its own entry if the key is still there, else the next greater key
            void revalidate() const {
                if (!key_ || !m_ || gen_ == m_->gen_) return;
                if (entry* f = m_->find_entry(*key_)) {
                    node_ = &f->kv; index_ = npos; gen_ = m_->gen_;
                } else {
                    const_cast<basic_iterator*>(this)->seat(m_->index_after(*key_));
                }
            }

            basic_iterator(map_ptr m, script_map::value_type* n, size_t i, uint64_t g, bool keep_key) : m_(m), node_(n), index_(i), gen_(g) {
                if (keep_key && n) key_.emplace(n->first);
            }
            void seat(size_t i) const {
                if (i < m_->order_.size()) {
                    node_ = m_->order_[i]; index_ = i; gen_ = m_->gen_;
                    key_.emplace(node_->first);
                } else {
                    node_ = nullptr; index_ = npos; key_.reset();
                }
            }
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = script_map::value_type;
            using difference_type = ptrdiff_t;
            using pointer = std::conditional_t<Const, const script_map::value_type*, script_map::value_type*>;
            using reference = std::conditional_t<Const, const script_map::value_type&, script_map::value_type&>;

            basic_iterator() = default;
            template <bool C = Const, typename = std::enable_if_t<C>>
            basic_iterator(const basic_iterator<false>& o) : m_(o.m_), node_(o.node_), index_(o.index_), gen_(o.gen_), key_(o.key_) {}

            reference operator*() const { revalidate(); return *node_; }
            pointer operator->() const { revalidate(); return node_; }
            basic_iterator& operator++() {
                if (key_ && gen_ != m_->gen_) {   // the entry may be gone: continue after its key
                    seat(m_->index_after(*key_));
                } else if (m_->order_gen_ == m_->gen_ && gen_ == m_->gen_ && index_ != npos) {
                    seat(index_ + 1);
                } else {
                    seat(m_->index_after(key_ ? *key_ : node_->first));
                }
                return *this;
            }
            basic_iterator operator++(int) { basic_iterator t = *this; ++*this; return t; }
            template <bool C2>
            bool operator==(const basic_iterator<C2>& o) const { revalidate(); o.revalidate(); return node_ == o.node_; }
            template <bool C2>
            bool operator!=(const basic_iterator<C2>& o) const { return !(*this == o); }
        };
        using iterator = basic_iterator<false>;
        using const_iterator = basic_iterator<true>;

        // the engine that made this node: its copies and memory charges run against it
        engine* owner = nullptr;

        script_map() = default;
        explicit script_map(engine* a_owner) : owner(a_owner) {}
        script_map(const script_map& o) : owner(o.owner) { copy_from(o); }
        script_map(script_map&& o) noexcept : owner(o.owner) { take_from(o); o.changed(); }
        bool has_class_keys() const noexcept { return class_keys_ != nullptr; }
        void mark_enum_values() noexcept { enum_values_ = true; }
        bool enum_values() const noexcept { return enum_values_; }
        script_map& operator=(const script_map& o) {
            if (this != &o) {
                script_map fresh(o);
                std::vector<entry*> dying = detach_all();
                entry_pool old = std::move(pool_);
                take_from(fresh);
                changed();
                for (entry* e : dying) { old.destroy(e); }
            }
            return *this;
        }
        script_map& operator=(script_map&& o) noexcept {
            if (this != &o) {
                std::vector<entry*> dying = detach_all();
                entry_pool old = std::move(pool_);
                take_from(o);
                changed(); o.changed();
                for (entry* e : dying) { old.destroy(e); }
            }
            return *this;
        }
        inline ~script_map();
        // Every entry onto `work` (back = next to die) in the order clear() destroys them: entry by entry, value then key
        void hand_off_entries(std::vector<script_value>& work) noexcept {
            const size_t base = work.size();
            std::vector<entry*> dying = detach_all();
            for (entry* e : dying) {
                work.push_back(std::move(e->kv.second));
                work.push_back(std::move(const_cast<script_value&>(e->kv.first)));
                pool_.destroy(e);
            }
            changed();
            order_.clear();
            std::reverse(work.begin() + static_cast<ptrdiff_t>(base), work.end());
        }

        void prepare_order() const { build_order(); }   // the order view built now (ahead of concurrent readers)
        size_type size() const noexcept { return size_; }
        bool empty() const noexcept { return size_ == 0; }
        // room for count entries: the index sized once, the entries' first chunk holding them all
        void reserve(size_t count) {
            grow_for(count);
            pool_.reserve(count);
        }
        void clear() noexcept {
            std::vector<entry*> dying = detach_all();
            changed();
            for (entry* e : dying) { pool_.destroy(e); }
        }

        // every entry once, in the index's order: for a walk whose outcome no order can change (no key order is built)
        template <typename F>
        void for_each_unordered(F&& f) const {
            for (const slot& s : slots_) { if (s.e) { f(static_cast<const value_type&>(s.e->kv)); } }
        }

        iterator begin() { build_order(); iterator it(this, nullptr, npos, gen_, false); it.seat(0); return it; }
        const_iterator begin() const { build_order(); const_iterator it(this, nullptr, npos, gen_, false); it.seat(0); return it; }
        const_iterator cbegin() const { return begin(); }
        iterator end() noexcept { return iterator(); }
        const_iterator end() const noexcept { return const_iterator(); }
        const_iterator cend() const noexcept { return const_iterator(); }

        // a remembered entry of one map: valid while the map that took it is unchanged (gen_ moves on every insert, erase, clear and
        // assignment); a holder outside the map pins the map (the owner test is an address)
        using entry_memo = entry_memo_storage;
        entry_memo& lane_memo() const noexcept { return lane_memo_; }
        // key's entry: memo's when memo was taken on this map at this generation and its key equals key under the map's key equality
        // (the generation is tested before the entry is touched), else the map's lookup, recorded in memo; null when key is absent
        template <typename K>
        value_type* recall(entry_memo& memo, const K& key) const {
            if (memo.owner == this && memo.gen == gen_ && memo.e) {
                entry* e = static_cast<entry*>(const_cast<void*>(memo.e));
                if (same_key(e, short_of(key), key)) return &e->kv;
            }
            entry* e = find_entry(key);
            memo = entry_memo{this, gen_, e};
            return e ? &e->kv : nullptr;
        }
        // the memo of the entry whose value is at `value`, found along its key's probe run by address: no key operator runs
        entry_memo memo_at(const script_value& key, const script_value* value) const noexcept {
            if (size_ == 0) return {};
            for (size_t i = home(script_value_hash{}(key));; i = (i + 1) & mask()) {
                const slot& s = slots_[i];
                if (!s.e) return {};
                if (&s.e->kv.second == value) return entry_memo{this, gen_, s.e};
            }
        }
        // the same for a store: an absent key is inserted (operator[]'s rule) and its new entry recorded
        value_type* recall_or_insert(entry_memo& memo, const script_value& key) {
            if (memo.owner == this && memo.gen == gen_ && memo.e) {
                entry* e = static_cast<entry*>(const_cast<void*>(memo.e));
                if (same_key(e, short_of(key), key)) return &e->kv;
            }
            entry* e = find_or_insert(key, [this](uint64_t h, const script_value& k) { return pool_.make(h, k); }).first;
            memo = entry_memo{this, gen_, e};
            return &e->kv;
        }

        iterator find(const script_value& k) {
            entry* e = find_entry(k);
            return e ? iterator(this, &e->kv, npos, gen_, false) : end();
        }
        const_iterator find(const script_value& k) const {
            entry* e = find_entry(k);
            return e ? const_iterator(this, &e->kv, npos, gen_, false) : end();
        }
        iterator find(const map_string_key_probe& p) {
            entry* e = find_entry(p);
            return e ? iterator(this, &e->kv, npos, gen_, false) : end();
        }
        const_iterator find(const map_string_key_probe& p) const {
            entry* e = find_entry(p);
            return e ? const_iterator(this, &e->kv, npos, gen_, false) : end();
        }
        // key's entry where it lives, or null: the lookup with no iterator
        const value_type* lookup(const script_value& k) const { entry* e = find_entry(k); return e ? &e->kv : nullptr; }
        const value_type* lookup_int(script_int x, engine* eng) const { if (size_ == 0) return nullptr; entry* e = find_int_entry(x, nullptr, eng); return e ? &e->kv : nullptr; }
        size_type count(const script_value& k) const { return find_entry(k) ? 1 : 0; }
        bool contains(const script_value& k) const { return find_entry(k) != nullptr; }

        script_value& operator[](const script_value& k) {
            return find_or_insert(k, [this](uint64_t h, const script_value& key) { return pool_.make(h, key); }).first->kv.second;
        }
        script_value& operator[](script_value&& k) {
            return find_or_insert(std::move(k), [this](uint64_t h, script_value&& key) { return pool_.make(h, std::move(key)); }).first->kv.second;
        }
        script_value& at(const script_value& k) {
            entry* e = find_entry(k);
            if (!e) throw std::out_of_range("script_map::at");
            return e->kv.second;
        }
        const script_value& at(const script_value& k) const {
            entry* e = find_entry(k);
            if (!e) throw std::out_of_range("script_map::at");
            return e->kv.second;
        }

        template <typename K, typename V>
        std::pair<iterator, bool> emplace(K&& k, V&& v) {
            auto [e, inserted] = find_or_insert(script_value(std::forward<K>(k)), [&](uint64_t h, script_value&& key) { return pool_.make(h, std::move(key), std::forward<V>(v)); });
            return {iterator(this, &e->kv, npos, gen_, false), inserted};
        }
        template <typename K, typename V>
        std::pair<iterator, bool> insert_or_assign(K&& k, V&& v) {
            bool fresh = false;
            auto [e, inserted] = find_or_insert(script_value(std::forward<K>(k)), [&](uint64_t h, script_value&& key) { fresh = true; return pool_.make(h, std::move(key), std::forward<V>(v)); });
            if (!fresh) { e->kv.second = std::forward<V>(v); }
            return {iterator(this, &e->kv, npos, gen_, false), inserted};
        }
        std::pair<iterator, bool> insert(const value_type& v) { return emplace(v.first, v.second); }
        std::pair<iterator, bool> insert(value_type&& v) { return emplace(v.first, std::move(v.second)); }

        size_type erase(const script_value& k) {
            entry* gone = unlink(k);
            if (!gone) return 0;
            pool_.destroy(gone);
            return 1;
        }
        // the entry after `pos` in key order
        iterator erase(const_iterator pos) {
            pos.revalidate();
            iterator next(this, pos.node_, pos.index_, pos.gen_, false);
            next.key_ = pos.key_;
            ++next;   // seated (and keyed) before the erase: its node stays valid
            erase(pos.node_->first);
            return next;
        }
        iterator erase(iterator pos) { return erase(const_iterator(pos)); }
        // the key at pos, writable for a store of an equal value (same hash, equality and order): a parallel region's
        // admission detaches a shared key string in place
        script_value& equal_key_storage(const_iterator pos) { pos.revalidate(); return const_cast<script_value&>(pos.node_->first); }
    };

    namespace detail {
        // work: values to destroy, back first; a nested last-handle array<var> or map hands its elements on instead of
        // destroying them inside its own destructor, so nesting costs no native depth and the order is unchanged (an array's
        // elements last to first)
        inline void release_work(std::vector<script_value>& work) noexcept {
            while (!work.empty()) {
                script_value v = std::move(work.back());
                work.pop_back();
                if (!v.release_flattens()) { continue; }
                if (v.raw_storage_index() == script_value::TYPEID_ARRAY) {
                    auto& vals = v.unchecked_get_array_storage()->values();
                    for (script_value& element : vals) { work.push_back(std::move(element)); }
                    vals.clear();
                } else {
                    v.unchecked_get_map_storage()->hand_off_entries(work);
                }
            }
        }
        inline void release_values_flattened(std::vector<script_value>& values) noexcept {
            std::vector<script_value> work;
            work.reserve(values.size());
            for (script_value& element : values) { work.push_back(std::move(element)); }
            values.clear();
            release_work(work);
        }
    }

    inline script_map::~script_map() {
        if (any_dies_observably()) {
            std::vector<script_value> work;
            hand_off_entries(work);
            detail::release_work(work);
            return;
        }
        // the index leaves the map whole (no copy of the entry list): the map is empty and consistent before any value dies
        std::vector<slot> index;
        index.swap(slots_);
        size_ = 0;
        class_keys_.reset();
        changed();
        for (const slot& s : index) { if (s.e) { pool_.destroy(s.e); } }
    }

} // namespace jai

#endif
