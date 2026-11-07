#pragma once

#ifndef __JAISCRIPT_DETAIL_STRING_SYMBOLIZER_HPP__
#define __JAISCRIPT_DETAIL_STRING_SYMBOLIZER_HPP__

#include <unordered_map>
#include <deque>
#include <string>
#include <cstdio>
#include <string_view>
#include <cassert>
#include <cstdint>
#include <jaiscript/jaiscript_fwd.hpp>
#include <jaiscript/detail/copy_census.hpp>
#include <limits>
#include <optional>
#include <set>
#include <vector>

namespace jai {

	struct sv_hash {
		using is_transparent = void; // enable heterogeneous lookup
		[[nodiscard]] size_t operator()(std::string_view sv) const noexcept {
			return std::hash<std::string_view>{}(sv);
		}
	};

	struct sv_equal {
		using is_transparent = void;
		[[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept {
			return a == b;
		}
	};

	// String symbolizer (local-only, non-deterministic IDs).
	class string_symbolizer {
	public:
		using id_type = std::uint64_t;

		string_symbolizer() {
			index_.reserve(256);
			// Pre-intern "this" to avoid a lazy branch in hot paths
			this_id_ = intern("this");
		}

		// Intern or fetch an existing ID. No string allocation on hit.
		// Marked const because interning doesn't change observable state - it just ensures
		// a string has an ID. Uses mutable internal storage.
		[[nodiscard]] id_type intern(std::string_view sv) const {
			JAI_CENSUS(sym_intern);
			if (auto it = index_.find(sv); it != index_.end())
				return it->second;

			// While a parallel region runs its threads only read the tables: a miss means the barrier missed a shape
			if (frozen_) [[unlikely]] { throw_frozen_miss(sv); }

			// Copy once into deque (stable address), then map a string_view to it (no second copy).
			auto& stored = storage_.emplace_back(sv);
			const std::string_view key{ stored.data(), stored.size() };

			// Note: size() fits into 32-bit id by design; guard if you need >4B strings.
			const id_type id = static_cast<id_type>(ids_.size());
			ids_.push_back(key);
			index_.emplace(key, id);
			return id;
		}

		// Intern and return both the ID and the interned string_view in one call.
		// Avoids needing a separate get_string() lookup.
		[[nodiscard]] std::pair<id_type, std::string_view> intern_with_view(std::string_view sv) const {
			JAI_CENSUS(sym_intern);
			if (auto it = index_.find(sv); it != index_.end())
				return { it->second, ids_[it->second] };

			if (frozen_) [[unlikely]] { throw_frozen_miss(sv); }

			auto& stored = storage_.emplace_back(sv);
			const std::string_view key{ stored.data(), stored.size() };
			const id_type id = static_cast<id_type>(ids_.size());
			ids_.push_back(key);
			index_.emplace(key, id);
			return { id, key };
		}

		// Read-only lookup (no allocation). Returns nullopt for an unknown name.
		// Unlike intern(), a query never inserts, including on a miss while frozen.
		[[nodiscard]] std::optional<id_type> find(std::string_view sv) const noexcept {
			JAI_CENSUS(sym_find);
			if (auto it = index_.find(sv); it != index_.end()) {
				return it->second;
			}
			return std::nullopt;
		}

		// Fast path for known IDs (no allocation). Returns empty view if OOB; a parked id reads its parked text.
		[[nodiscard]] inline std::string_view get_string(id_type id) const noexcept {
			JAI_CENSUS(sym_get_string);
			if (id < ids_.size()) [[likely]] { return ids_[id]; }
			return (id & k_parked_bit) ? parked_text(id) : std::string_view{};
		}

		// Transient texts (an error message that embeds a value, a host exception's what(), a name nothing declares):
		// a bounded ring of owned texts whose ids live in their own range, so a failing call repeated in a loop never
		// grows the interned table. An id outlived by k_parked_texts newer parks reads as k_parked_expired. The ring
		// is written only while not frozen (a parallel worker must not park; callers take their slot-less text).
		static constexpr id_type k_parked_bit = id_type(1) << 63;
		static constexpr std::size_t k_parked_texts = 256;
		static constexpr std::string_view k_parked_expired = "(the error text is no longer available)";
		[[nodiscard]] id_type park(std::string_view text) const {
			assert(!frozen_ && "string_symbolizer: park during a parallel region");
			if (parked_.empty()) { parked_.resize(k_parked_texts); }
			const id_type seq = parked_next_++;
			parked_slot& slot = parked_[seq % k_parked_texts];
			slot.seq = seq;
			slot.text.assign(text.data(), text.size());
			return k_parked_bit | seq;
		}
		// A text whose views must outlive what made it (a compiled chunk's error text: the error travels past the chunk), kept
		// once per distinct text for the engine's life and never in the id table. Written only by compiles, which a parallel
		// region serializes on the engine's compile mutex
		[[nodiscard]] std::string_view keep_text(std::string_view text) const {
			auto found = kept_texts_.find(text);
			if (found == kept_texts_.end()) { found = kept_texts_.emplace(text).first; }
			return *found;
		}

		// The interned table's size (parked texts never count)
		[[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }

		[[nodiscard]] inline id_type get_this_id() const noexcept { return this_id_; }

		// Engine-wide environment shape epoch: environment construction/reset/clear, a new
		// define, or a parent change bumps it, invalidating the vm's per-instruction
		// env-lookup caches (keyed on {environment*, epoch}).
		[[nodiscard]] inline std::uint64_t env_epoch() const noexcept { return env_epoch_; }
		inline void bump_env_epoch() noexcept { assert(!frozen_ && "a parallel worker bumps the engine's env epoch (use its own sink)"); ++env_epoch_; }
		// Monotonic per-engine serial source for environment generation stamps
		// (environment::local_epoch): never repeats within an engine, so a cache entry
		// keyed {env address, serial} cannot false-hit across env lifetimes even when
		// the allocator recycles the address. Starts past 0 (0 = never-cacheable).
		[[nodiscard]] inline std::uint64_t next_env_serial() noexcept { assert(!frozen_ && "a parallel worker takes the engine's env serial (use its own sink)"); return ++env_epoch_; }

		// The unit whose code is running (0 = the host): a file by path or a literal by content,
		// numbered by the engine at first execution. A program-scope variable redeclared by
		// another unit gets its own cell; shadow_count() nonzero means some name has versions
		// and lookups pick the version by unit.
		[[nodiscard]] inline unit_stamp current_unit() const noexcept { return current_unit_; }
		inline void set_current_unit(unit_stamp unit) noexcept { current_unit_ = unit; }
		// A fresh execution number: the seq of the stamp execute_parsed installs for a run
		[[nodiscard]] inline std::uint32_t next_execution() noexcept { return ++execution_count_; }
		[[nodiscard]] inline std::uint32_t shadow_count() const noexcept { return shadow_count_; }
		inline void note_shadow() noexcept { ++shadow_count_; }

		// Frozen while a parallel region runs: an intern MISS throws (the barrier must make every worker intern a hit);
		// zero cost on the hit path.
		inline void set_frozen(bool frozen) noexcept { frozen_ = frozen; }
		inline bool frozen() const noexcept { return frozen_; }
		// Steps environment lookups took up a parent chain (counted by Debug builds only, never while frozen): tests read it
		// as a deterministic measure of lookup cost
		inline void note_env_hop() const noexcept { if (!frozen_) { ++env_hops_; } }
		[[nodiscard]] inline std::uint64_t env_hops() const noexcept { return env_hops_; }

		// Get/create getter ID for a member - caches to avoid repeated concatenation.
		// Returns both ID and interned string_view for the "_get_<member>" name.
		[[nodiscard]] std::pair<id_type, std::string_view> get_getter_id_with_view(id_type member_id) const {
			if (auto it = getter_cache_.find(member_id); it != getter_cache_.end())
				return { it->second, ids_[it->second] };

			if (frozen_) [[unlikely]] { return frozen_prefixed("_get_", member_id); }   // a region reads, never fills, the cache
			std::string getter_name = "_get_" + std::string(get_string(member_id));
			auto [id, view] = intern_with_view(getter_name);
			getter_cache_[member_id] = id;
			return { id, view };
		}

		// Get/create setter ID for a member - caches to avoid repeated concatenation.
		// Returns both ID and interned string_view for the "_set_<member>" name.
		[[nodiscard]] std::pair<id_type, std::string_view> get_setter_id_with_view(id_type member_id) const {
			if (auto it = setter_cache_.find(member_id); it != setter_cache_.end())
				return { it->second, ids_[it->second] };

			if (frozen_) [[unlikely]] { return frozen_prefixed("_set_", member_id); }   // a region reads, never fills, the cache
			std::string setter_name = "_set_" + std::string(get_string(member_id));
			auto [id, view] = intern_with_view(setter_name);
			setter_cache_[member_id] = id;
			return { id, view };
		}

		// Get/create class variable ID - caches to avoid repeated "__class_" + name concatenation.
		// Returns both ID and interned string_view for the "__class_<name>" variable name.
		[[nodiscard]] std::pair<id_type, std::string_view> get_class_var_id_with_view(id_type class_name_id) const {
			if (auto it = class_var_cache_.find(class_name_id); it != class_var_cache_.end())
				return { it->second, ids_[it->second] };

			if (frozen_) [[unlikely]] { return frozen_prefixed("__class_", class_name_id); }   // a region reads, never fills, the cache
			std::string class_var_name = "__class_" + std::string(get_string(class_name_id));
			auto [id, view] = intern_with_view(class_var_name);
			class_var_cache_[class_name_id] = id;
			return { id, view };
		}

	private:
		[[noreturn]] JAI_NOINLINE static void throw_frozen_miss(std::string_view name);   // engine.cpp
		JAI_NOINLINE std::pair<id_type, std::string_view> frozen_prefixed(std::string_view prefix, id_type base_id) const {
			const std::string name = std::string(prefix) + std::string(get_string(base_id));
			if (auto it = index_.find(std::string_view(name)); it != index_.end()) { return { it->second, ids_[it->second] }; }
			throw_frozen_miss(name);
		}
		JAI_NOINLINE std::string_view parked_text(id_type id) const noexcept {
			const id_type seq = id & ~k_parked_bit;
			if (parked_.empty()) { return k_parked_expired; }
			const parked_slot& slot = parked_[seq % k_parked_texts];
			return slot.seq == seq ? std::string_view(slot.text) : k_parked_expired;
		}
		struct parked_slot {
			id_type seq = ~id_type(0);
			std::string text;
		};
		mutable std::vector<parked_slot> parked_;
		mutable id_type parked_next_ = 0;

		// Mutable to allow const intern() - interning doesn't change observable state
		// Owning storage with stable addresses (push_back/push_front do not invalidate references)
		mutable std::deque<std::string> storage_;
		mutable std::set<std::string, std::less<>> kept_texts_;

		// Map from string_view (into storage_) to ID (no second copy)
		mutable std::unordered_map<std::string_view, id_type, sv_hash, sv_equal> index_;

		// ID -> string_view (into storage_) for reverse lookup
		mutable std::vector<std::string_view> ids_;

		mutable id_type this_id_{ std::numeric_limits<id_type>::max() };

		std::uint64_t env_epoch_ = 1;
		unit_stamp current_unit_{};
		std::uint32_t execution_count_ = 0;
		std::uint32_t shadow_count_ = 0;

		// Parallel-region tripwire (see set_frozen)
		bool frozen_ = false;
		mutable std::uint64_t env_hops_ = 0;

		// Caches for getter/setter/class_var ID lookups (name_id -> derived_id)
		mutable std::unordered_map<id_type, id_type> getter_cache_;
		mutable std::unordered_map<id_type, id_type> setter_cache_;
		mutable std::unordered_map<id_type, id_type> class_var_cache_;

	};

} // namespace jai

#endif // __JAISCRIPT_DETAIL_STRING_SYMBOLIZER_HPP__
