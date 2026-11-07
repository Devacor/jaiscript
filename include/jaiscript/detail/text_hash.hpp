#pragma once

#ifndef __JAISCRIPT_DETAIL_TEXT_HASH_HPP__
#define __JAISCRIPT_DETAIL_TEXT_HASH_HPP__

// The hash of a map's string keys: eight bytes a step, the last word overlapping the one before it, so a key of up to
// sixteen bytes costs two loads and two multiplies. Computed within one process only (key order never depends on it), so it
// starts from a seed each process draws once: a key set built to collide in one run spreads in the next.

#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <jaiscript/jaiscript_fwd.hpp>

namespace jai::detail {

	// value.cpp: drawn once when the library initializes, never written after
	extern const uint64_t k_process_hash_seed;

	JAI_FORCEINLINE uint64_t text_load64(const char* p) noexcept { uint64_t w; std::memcpy(&w, p, 8); return w; }
	JAI_FORCEINLINE uint64_t text_load32(const char* p) noexcept { uint32_t w; std::memcpy(&w, p, 4); return w; }

	// The text's bytes packed into one word (at most eight of them; byte i at bits 8i..8i+7, the rest zero)
	JAI_FORCEINLINE uint64_t text_pack_short(const char* p, size_t n) noexcept {
		if constexpr (std::endian::native == std::endian::little) {
			if (n >= 8) { return text_load64(p); }
			if (n >= 4) { return text_load32(p) | (text_load32(p + n - 4) << (8 * (n - 4))); }
			if (n == 0) { return 0; }
			return static_cast<uint64_t>(static_cast<unsigned char>(p[0])) | (static_cast<uint64_t>(static_cast<unsigned char>(p[n >> 1])) << (8 * (n >> 1))) |
			       (static_cast<uint64_t>(static_cast<unsigned char>(p[n - 1])) << (8 * (n - 1)));
		} else {
			uint64_t w = 0;
			for (size_t i = 0; i < n && i < 8; ++i) { w |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i); }
			return w;
		}
	}

	JAI_FORCEINLINE uint64_t text_hash_step(uint64_t h, uint64_t w) noexcept {
		h = (h ^ w) * 0xbf58476d1ce4e5b9ull;
		return h ^ (h >> 29);
	}

	JAI_FORCEINLINE size_t text_hash(std::string_view text) noexcept {
		const char* p = text.data();
		const size_t n = text.size();
		uint64_t h = k_process_hash_seed ^ (static_cast<uint64_t>(n) * 0xff51afd7ed558ccdull);
		if (n <= 8) {
			h = text_hash_step(h, text_pack_short(p, n));
		} else if constexpr (std::endian::native == std::endian::little) {
			size_t i = 0;
			for (; i + 8 < n; i += 8) { h = text_hash_step(h, text_load64(p + i)); }
			h = text_hash_step(h, text_load64(p + n - 8));
		} else {
			size_t i = 0;
			for (; i < n; i += 8) { h = text_hash_step(h, text_pack_short(p + i, n - i < 8 ? n - i : 8)); }
		}
		h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull; h ^= h >> 33;
		return static_cast<size_t>(h);
	}

} // namespace jai::detail

#endif
