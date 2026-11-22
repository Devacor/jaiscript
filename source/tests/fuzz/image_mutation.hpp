#pragma once

// The image:: decoders against damaged files: every fixture mutated by a seeded generator (bits flipped, bytes set, the
// file truncated, ranges duplicated, deleted and spliced in from other fixtures), each case run through read_header and
// decode. Shared by the "Image" suite and the standalone jaiscript_image_fuzz, so it names neither the engine nor Foundry.

#include <jaiscript/detail/image_codec.hpp>
#include "stdlib/image_fixtures.hpp"
#include "stdlib/image_test_support.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace jai {
namespace image_mutation {

	inline constexpr const char* k_formats[] = {"png", "jpeg", "gif"};
	// what a mutated header may claim before the decoder refuses it: a megapixel keeps a case's memory small
	inline constexpr uint64_t k_case_max_pixels = uint64_t {1} << 20;
	inline constexpr double k_case_seconds = 10.0;

	struct generator {
		uint64_t state;
		uint64_t next() {
			state += 0x9e3779b97f4a7c15ull;
			uint64_t z = state;
			z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
			z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
			return z ^ (z >> 31);
		}
		size_t below(size_t a_count) { return a_count == 0 ? 0 : static_cast<size_t>(next() % a_count); }
	};

	inline std::vector<const image_fixtures::fixture*> fixtures_of(const char* a_format) {
		std::vector<const image_fixtures::fixture*> result;
		for (const image_fixtures::fixture& each : image_fixtures::k_fixtures) {
			if (std::strcmp(each.format, a_format) == 0) { result.push_back(&each); }
		}
		return result;
	}

	// a_edits edits to a_bytes: bits flipped, bytes set, the file cut, ranges duplicated, deleted and spliced in from a_pool
	inline void mutate(std::vector<uint8_t>& bytes, const std::vector<const image_fixtures::fixture*>& a_pool, generator& random, size_t edits) {
		for (size_t edit = 0; edit < edits && !bytes.empty(); ++edit) {
			const size_t at = random.below(bytes.size());
			switch (random.below(9)) {
				case 0: bytes[at] ^= static_cast<uint8_t>(1u << random.below(8)); break;
				case 1: bytes[at] = static_cast<uint8_t>(random.next()); break;
				case 2: {
					static constexpr uint8_t k_edges[] = {0x00, 0xff, 0x7f, 0x80, 0x01, 0xfe};
					bytes[at] = k_edges[random.below(sizeof(k_edges))];
					break;
				}
				case 3: bytes.resize(at); break;
				case 4: {
					const size_t length = 1 + random.below(bytes.size() - at < 64 ? bytes.size() - at : 64);
					const std::vector<uint8_t> copy(bytes.begin() + at, bytes.begin() + at + length);
					const size_t to = random.below(bytes.size() + 1);
					bytes.insert(bytes.begin() + to, copy.begin(), copy.end());
					break;
				}
				case 5: {
					const image_fixtures::fixture& other = *a_pool[random.below(a_pool.size())];
					const size_t from = random.below(other.size);
					const size_t length = 1 + random.below(other.size - from < 96 ? other.size - from : 96);
					for (size_t i = 0; i < length && at + i < bytes.size(); ++i) { bytes[at + i] = other.bytes[from + i]; }
					break;
				}
				case 6: {
					const size_t length = 1 + random.below(bytes.size() - at < 32 ? bytes.size() - at : 32);
					bytes.erase(bytes.begin() + at, bytes.begin() + at + length);
					break;
				}
				case 7: {
					static constexpr uint32_t k_sizes[] = {0, 1, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0xffffu, 0x10000u, 0x00ff00ffu};
					const uint32_t value = k_sizes[random.below(sizeof(k_sizes) / sizeof(k_sizes[0]))];
					const bool little = random.below(2) == 1;
					for (size_t i = 0; i < 4 && at + i < bytes.size(); ++i) { bytes[at + i] = static_cast<uint8_t>(value >> (little ? 8 * i : 24 - 8 * i)); }
					break;
				}
				default: {
					const size_t length = 1 + random.below(8);
					for (size_t i = 0; i < length; ++i) { bytes.insert(bytes.begin() + at, static_cast<uint8_t>(random.next())); }
					break;
				}
			}
		}
	}

	// Every chunk's CRC made right again, so an edit inside a chunk reaches the code behind the CRC check
	inline void repair_png_crcs(std::vector<uint8_t>& a_bytes) {
		size_t at = 8;
		while (at + 12 <= a_bytes.size()) {
			const size_t length = (size_t {a_bytes[at]} << 24) | (size_t {a_bytes[at + 1]} << 16) | (size_t {a_bytes[at + 2]} << 8) | a_bytes[at + 3];
			if (length > a_bytes.size() - at - 12) { return; }
			const uint32_t crc = image_codec::crc32(a_bytes.data() + at + 4, length + 4);
			for (int i = 0; i < 4; ++i) { a_bytes[at + 8 + length + i] = static_cast<uint8_t>(crc >> (24 - 8 * i)); }
			at += 12 + length;
		}
	}

	// The pass each pixel of an 8 x 8 tile belongs to, as the PNG specification draws Adam7
	inline constexpr uint8_t k_adam7[8][8] = {
		{1, 6, 4, 6, 2, 6, 4, 6}, {7, 7, 7, 7, 7, 7, 7, 7}, {5, 6, 5, 6, 5, 6, 5, 6}, {7, 7, 7, 7, 7, 7, 7, 7},
		{3, 6, 4, 6, 3, 6, 4, 6}, {7, 7, 7, 7, 7, 7, 7, 7}, {5, 6, 5, 6, 5, 6, 5, 6}, {7, 7, 7, 7, 7, 7, 7, 7}};

	// A PNG that is well formed by construction and random in content: any colour type and depth, either interlace, random
	// filter types and sample bytes, a palette and tRNS where they belong
	inline std::vector<uint8_t> generated_png(generator& random) {
		static constexpr uint8_t k_shapes[][2] = {{1, 0}, {2, 0}, {4, 0}, {8, 0}, {16, 0}, {8, 2}, {16, 2}, {1, 3}, {2, 3}, {4, 3}, {8, 3}, {8, 4}, {16, 4}, {8, 6}, {16, 6}};
		const uint8_t* shape = k_shapes[random.below(sizeof(k_shapes) / sizeof(k_shapes[0]))];
		const unsigned depth = shape[0];
		const unsigned colour = shape[1];
		const uint32_t width = static_cast<uint32_t>(1 + random.below(40));
		const uint32_t height = static_cast<uint32_t>(1 + random.below(40));
		const bool interlaced = random.below(2) == 1;
		const unsigned bits = depth * (colour == 2 ? 3u : colour == 4 ? 2u : colour == 6 ? 4u : 1u);
		const bool quiet = random.below(4) == 0;
		const bool odd_filters = random.below(50) == 0;
		std::vector<uint8_t> raw;
		for (unsigned pass = interlaced ? 1 : 0; pass <= (interlaced ? 7u : 0u); ++pass) {
			size_t columns = 0;
			size_t lines = 0;
			for (uint32_t x = 0; x < width; ++x) {
				bool in_pass = pass == 0;
				for (int y = 0; y < 8 && !in_pass; ++y) { in_pass = k_adam7[y][x % 8] == pass; }
				columns += in_pass ? 1 : 0;
			}
			for (uint32_t y = 0; y < height; ++y) {
				bool in_pass = pass == 0;
				for (int x = 0; x < 8 && !in_pass; ++x) { in_pass = k_adam7[y % 8][x] == pass; }
				lines += in_pass ? 1 : 0;
			}
			if (columns == 0 || lines == 0) { continue; }
			for (size_t line = 0; line < lines; ++line) {
				raw.push_back(static_cast<uint8_t>(odd_filters ? random.below(8) : random.below(5)));
				for (size_t i = 0; i < (columns * bits + 7) / 8; ++i) { raw.push_back(static_cast<uint8_t>(quiet ? random.below(3) : random.next())); }
			}
		}
		image_test::png_parts parts;
		if (colour == 3) {
			const size_t entries = random.below(8) == 0 ? 1 + random.below(size_t {1} << depth) : size_t {1} << depth;
			image_test::bytes palette;
			for (size_t i = 0; i < entries * 3; ++i) { palette.push_back(static_cast<uint8_t>(random.next())); }
			parts.before_data.push_back({"PLTE", palette});
			if (random.below(2) == 1) {
				image_test::bytes alpha;
				for (size_t i = 0, count = 1 + random.below(entries); i < count; ++i) { alpha.push_back(static_cast<uint8_t>(random.next())); }
				parts.before_data.push_back({"tRNS", alpha});
			}
		} else if ((colour == 0 || colour == 2) && random.below(2) == 1) {
			image_test::bytes key;
			for (size_t i = 0; i < (colour == 0 ? 2u : 6u); ++i) { key.push_back(static_cast<uint8_t>(i % 2 == 0 && depth < 16 ? 0 : random.below(4))); }
			parts.before_data.push_back({"tRNS", key});
		}
		parts.data_pieces = 1 + random.below(3);
		return image_test::png_file(width, height, static_cast<uint8_t>(depth), static_cast<uint8_t>(colour), interlaced ? 1 : 0, raw, parts);
	}

	// Edits to the entropy-coded bytes after a JPEG's scan headers alone, none of them an FF, so the marker structure stands and
	// the Huffman and coefficient decoding see the damage
	inline void mutate_jpeg_data(std::vector<uint8_t>& bytes, generator& random, size_t edits) {
		std::vector<std::pair<size_t, size_t>> spans;
		size_t at = 2;
		while (at + 4 <= bytes.size() && bytes[at] == 0xff) {
			const size_t length = (size_t {bytes[at + 2]} << 8) | bytes[at + 3];
			if (bytes[at + 1] != 0xda) {
				at += 2 + length;
				continue;
			}
			size_t end = at + 2 + length;
			const size_t begin = end;
			while (end + 1 < bytes.size() && !(bytes[end] == 0xff && bytes[end + 1] != 0x00 && (bytes[end + 1] < 0xd0 || bytes[end + 1] > 0xd7))) { ++end; }
			if (end > begin) { spans.push_back({begin, end}); }
			at = end;
		}
		if (spans.empty()) { return; }
		for (size_t edit = 0; edit < edits; ++edit) {
			const std::pair<size_t, size_t> span = spans[random.below(spans.size())];
			const size_t where = span.first + random.below(span.second - span.first);
			if (bytes[where] == 0xff || (where > 0 && bytes[where - 1] == 0xff)) { continue; }
			uint8_t value = random.below(2) == 0 ? static_cast<uint8_t>(bytes[where] ^ (1u << random.below(8))) : static_cast<uint8_t>(random.next());
			if (value == 0xff) { value = 0xfe; }
			bytes[where] = value;
		}
	}

	// The bytes of case a_seed of a format: a fixture with one to four edits. A chunk CRC would stop nearly every edit to a
	// PNG at the door, so two PNG cases in three have their CRCs repaired, and one of those starts from a generated file; one
	// JPEG case in three damages only the image data.
	inline std::vector<uint8_t> mutated_case(const char* a_format, const std::vector<const image_fixtures::fixture*>& a_pool, uint64_t a_seed) {
		generator random {a_seed * 0x2545f4914f6cdd1dull + 0x1b873593};
		const bool png = std::strcmp(a_format, "png") == 0;
		const bool jpeg = std::strcmp(a_format, "jpeg") == 0;
		const size_t kind = png || jpeg ? static_cast<size_t>(a_seed % 3) : 2;
		std::vector<uint8_t> bytes;
		if (kind == 0 && png) {
			bytes = generated_png(random);
			mutate(bytes, a_pool, random, random.below(3));
		} else if (kind == 0 && jpeg) {
			const image_fixtures::fixture& base = *a_pool[random.below(a_pool.size())];
			bytes.assign(base.bytes, base.bytes + base.size);
			mutate_jpeg_data(bytes, random, 1 + random.below(12));
		} else {
			const image_fixtures::fixture& base = *a_pool[random.below(a_pool.size())];
			bytes.assign(base.bytes, base.bytes + base.size);
			mutate(bytes, a_pool, random, 1 + random.below(4));
		}
		if (png && kind != 2) { repair_png_crcs(bytes); }
		return bytes;
	}

	struct summary {
		uint64_t cases = 0;
		uint64_t images = 0;
		uint64_t errors[static_cast<size_t>(image_codec::status::status_count)] = {};
		uint64_t wrong = 0;
		std::string first_wrong;
		double seconds = 0;
	};

	// One case through the header reader and the decoder: "" for an image or a catalogued error, else what is wrong
	inline std::string check_case(const uint8_t* a_data, size_t a_size, image_codec::status& a_status) {
		namespace codec = image_codec;
		codec::limits limits;
		limits.max_pixels = k_case_max_pixels;
		const auto started = std::chrono::steady_clock::now();
		codec::header probed;
		const codec::status probe = codec::read_header(a_data, a_size, limits, probed);
		codec::header decoded;
		codec::bitmap pixels;
		a_status = codec::decode(a_data, a_size, limits, codec::work_hook {}, decoded, pixels);
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		if (seconds > k_case_seconds) { return "took " + std::to_string(seconds) + " seconds"; }
		if (a_status >= codec::status::status_count) { return "a status past the catalogue"; }
		if (a_status != codec::status::ok) {
			if (a_status == codec::status::budget) { return "a budget failure with no budget"; }
			if (codec::status_text(a_status)[0] == '\0') { return "an error with no catalogued text"; }
			return "";
		}
		if (pixels.width == 0 || pixels.height == 0) { return "an image with no pixels"; }
		if (pixels.width != decoded.width || pixels.height != decoded.height) { return "pixels that disagree with the decoded header"; }
		if (uint64_t {pixels.width} * pixels.height > limits.max_pixels) { return "an image over the pixel limit"; }
		if (pixels.rgba.size() != size_t {pixels.width} * pixels.height * 4) { return "a pixel buffer of the wrong size"; }
		if (probe != codec::status::ok) { return std::string("an image whose header alone is refused: ") + codec::status_text(probe); }
		if (probed.width != decoded.width || probed.height != decoded.height || probed.kind != decoded.kind || probed.orientation != decoded.orientation) {
			return "an image whose dimensions disagree with its header";
		}
		if (decoded.orientation < 1 || decoded.orientation > 8) { return "an orientation outside 1 to 8"; }
		return "";
	}

	// Cases a_first_seed to a_first_seed + a_count - 1 of one format, stopping early after a_seconds (0: no cap)
	inline summary run(const char* a_format, uint64_t a_first_seed, uint64_t a_count, double a_seconds) {
		summary result;
		const std::vector<const image_fixtures::fixture*> pool = fixtures_of(a_format);
		if (pool.empty()) { return result; }
		const auto started = std::chrono::steady_clock::now();
		for (uint64_t i = 0; i < a_count; ++i) {
			const std::vector<uint8_t> bytes = mutated_case(a_format, pool, a_first_seed + i);
			image_codec::status outcome = image_codec::status::ok;
			const std::string wrong = check_case(bytes.data(), bytes.size(), outcome);
			++result.cases;
			if (!wrong.empty()) {
				if (result.wrong++ == 0) { result.first_wrong = std::string(a_format) + " seed " + std::to_string(a_first_seed + i) + ": " + wrong; }
			} else if (outcome == image_codec::status::ok) {
				++result.images;
			} else {
				++result.errors[static_cast<size_t>(outcome)];
			}
			result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
			if (a_seconds > 0 && result.seconds > a_seconds) { break; }
		}
		return result;
	}

	// "png: 4000 cases, 312 images, 3688 errors, 0 wrong, 1.20 s" and then each error text with its count
	inline std::string describe(const char* a_format, const summary& a_summary) {
		uint64_t errors = 0;
		std::string breakdown;
		for (size_t i = 0; i < static_cast<size_t>(image_codec::status::status_count); ++i) {
			if (a_summary.errors[i] == 0) { continue; }
			errors += a_summary.errors[i];
			breakdown += std::string(breakdown.empty() ? " [" : "; ") + image_codec::status_text(static_cast<image_codec::status>(i)) + ": " + std::to_string(a_summary.errors[i]);
		}
		if (!breakdown.empty()) { breakdown += "]"; }
		char time[32];
		std::snprintf(time, sizeof(time), "%.2f", a_summary.seconds);
		return std::string(a_format) + ": " + std::to_string(a_summary.cases) + " cases, " + std::to_string(a_summary.images) + " images, " + std::to_string(errors) + " errors, " +
			std::to_string(a_summary.wrong) + " wrong, " + time + " s" + breakdown + (a_summary.wrong ? " FIRST WRONG " + a_summary.first_wrong : std::string());
	}

} // namespace image_mutation
} // namespace jai
