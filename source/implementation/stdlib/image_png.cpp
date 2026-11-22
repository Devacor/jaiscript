#ifdef JAISCRIPT_ENABLE_IMAGE

#include <jaiscript/detail/image_codec.hpp>
#include <array>
#include <cstring>
#include <new>
#include <utility>
#include <stdexcept>

// PNG (ISO/IEC 15948) over zlib (RFC 1950) and deflate (RFC 1951), written from those specifications: the decoder and the encoder.

namespace jai {
namespace image_codec {
namespace png_detail {

	constexpr std::array<uint32_t, 256> make_crc_table() {
		std::array<uint32_t, 256> table {};
		for (uint32_t n = 0; n < 256; ++n) {
			uint32_t c = n;
			for (int k = 0; k < 8; ++k) { c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1; }
			table[n] = c;
		}
		return table;
	}
	inline constexpr std::array<uint32_t, 256> k_crc_table = make_crc_table();

	// Deflate's bits, least significant first; a read past the end answers false
	struct bit_source {
		const uint8_t* at;
		const uint8_t* end;
		uint64_t bits = 0;
		unsigned count = 0;

		void fill() {
			while (count <= 56 && at < end) {
				bits |= uint64_t {*at++} << count;
				count += 8;
			}
		}
		void drop(unsigned a_bits) {
			bits >>= a_bits;
			count -= a_bits;
		}
		// a_bits at most 16
		bool take(unsigned a_bits, uint32_t& a_value) {
			if (count < a_bits) {
				fill();
				if (count < a_bits) { return false; }
			}
			a_value = static_cast<uint32_t>(bits & ((uint64_t {1} << a_bits) - 1));
			drop(a_bits);
			return true;
		}
		void align_to_byte() { drop(count & 7); }
	};

	// A canonical Huffman code over at most 288 symbols with lengths up to 15
	class huffman_decoder {
	public:
		static constexpr unsigned k_fast_bits = 9;
		static constexpr unsigned k_max_length = 15;
		static constexpr unsigned k_max_symbols = 288;

		// False for an over-subscribed set of lengths, and for an incomplete one unless it is a single one-bit code (or
		// empty) and a_single_code_allowed
		bool build(const uint8_t* a_lengths, unsigned a_count, bool a_single_code_allowed) {
			std::memset(counts_, 0, sizeof(counts_));
			std::memset(fast_, 0, sizeof(fast_));
			if (a_count > k_max_symbols) { return false; }
			for (unsigned i = 0; i < a_count; ++i) {
				if (a_lengths[i] > k_max_length) { return false; }
				++counts_[a_lengths[i]];
			}
			counts_[0] = 0;
			int left = 1;
			unsigned total = 0;
			for (unsigned length = 1; length <= k_max_length; ++length) {
				left <<= 1;
				left -= counts_[length];
				if (left < 0) { return false; }
				total += counts_[length];
			}
			if (left > 0) {
				const bool single = total == 0 || (total == 1 && counts_[1] == 1);
				if (!single || !a_single_code_allowed) { return false; }
			}
			uint16_t offsets[k_max_length + 2];
			offsets[1] = 0;
			for (unsigned length = 1; length <= k_max_length; ++length) { offsets[length + 1] = static_cast<uint16_t>(offsets[length] + counts_[length]); }
			uint16_t next_code[k_max_length + 1];
			unsigned code = 0;
			for (unsigned length = 1; length <= k_max_length; ++length) {
				code = (code + counts_[length - 1]) << 1;
				next_code[length] = static_cast<uint16_t>(code);
			}
			for (unsigned symbol = 0; symbol < a_count; ++symbol) {
				const unsigned length = a_lengths[symbol];
				if (length == 0) { continue; }
				symbols_[offsets[length]++] = static_cast<uint16_t>(symbol);
				const unsigned assigned = next_code[length]++;
				if (length > k_fast_bits) { continue; }
				unsigned reversed = 0;
				for (unsigned bit = 0; bit < length; ++bit) { reversed |= ((assigned >> bit) & 1u) << (length - 1 - bit); }
				for (unsigned slot = reversed; slot < (1u << k_fast_bits); slot += 1u << length) { fast_[slot] = static_cast<uint16_t>((length << 9) | symbol); }
			}
			return true;
		}

		// The next symbol, or -1 for bits no code spells and for a code cut off by the end of the input
		int decode(bit_source& a_source) const {
			a_source.fill();
			if (const uint16_t entry = fast_[a_source.bits & ((1u << k_fast_bits) - 1)]) {
				const unsigned length = entry >> 9;
				if (length > a_source.count) { return -1; }
				a_source.drop(length);
				return entry & 511;
			}
			unsigned code = 0;
			unsigned first = 0;
			unsigned index = 0;
			for (unsigned length = 1; length <= k_max_length; ++length) {
				if (length > a_source.count) { return -1; }
				code |= static_cast<unsigned>((a_source.bits >> (length - 1)) & 1);
				const unsigned count = counts_[length];
				if (code < first + count) {
					a_source.drop(length);
					return symbols_[index + (code - first)];
				}
				index += count;
				first = (first + count) << 1;
				code <<= 1;
			}
			return -1;
		}

	private:
		uint16_t counts_[k_max_length + 1];
		uint16_t symbols_[k_max_symbols];
		uint16_t fast_[1u << k_fast_bits];
	};

	inline constexpr uint16_t k_length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
	inline constexpr uint8_t k_length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
	inline constexpr uint16_t k_distance_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
	inline constexpr uint8_t k_distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
	inline constexpr uint8_t k_code_length_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	inline constexpr size_t k_work_stride = size_t {1} << 16;

	struct inflater {
		bit_source source;
		uint8_t* out;
		size_t out_size;
		size_t out_at = 0;
		size_t charged = 0;
		const work_hook& work;
		huffman_decoder literals {};
		huffman_decoder distances {};
		huffman_decoder fixed_literals {};
		huffman_decoder fixed_distances {};
		bool fixed_ready = false;

		inflate_status stored_block() {
			source.align_to_byte();
			uint32_t length = 0;
			uint32_t complement = 0;
			if (!source.take(16, length) || !source.take(16, complement)) { return inflate_status::truncated; }
			if ((length ^ 0xffffu) != complement) { return inflate_status::bad_stored_length; }
			if (length > out_size - out_at) { return inflate_status::too_much_output; }
			while (length > 0 && source.count >= 8) {
				out[out_at++] = static_cast<uint8_t>(source.bits & 0xff);
				source.drop(8);
				--length;
			}
			if (length > static_cast<size_t>(source.end - source.at)) { return inflate_status::truncated; }
			if (length > 0) {
				std::memcpy(out + out_at, source.at, length);
				source.at += length;
				out_at += length;
			}
			return inflate_status::ok;
		}

		// Built once a stream: a run of empty fixed blocks costs no more than its bits
		void fixed_tables() {
			if (fixed_ready) { return; }
			uint8_t lengths[288];
			for (unsigned i = 0; i < 288; ++i) { lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8; }
			fixed_literals.build(lengths, 288, false);
			// all 32 five-bit codes exist, though 30 and 31 never name a distance
			for (unsigned i = 0; i < 32; ++i) { lengths[i] = 5; }
			fixed_distances.build(lengths, 32, false);
			fixed_ready = true;
		}

		inflate_status dynamic_tables() {
			uint32_t literal_count = 0;
			uint32_t distance_count = 0;
			uint32_t length_count = 0;
			if (!source.take(5, literal_count) || !source.take(5, distance_count) || !source.take(4, length_count)) { return inflate_status::truncated; }
			literal_count += 257;
			distance_count += 1;
			length_count += 4;
			if (literal_count > 286 || distance_count > 30) { return inflate_status::bad_code_lengths; }
			uint8_t code_lengths[19] = {};
			for (uint32_t i = 0; i < length_count; ++i) {
				uint32_t length = 0;
				if (!source.take(3, length)) { return inflate_status::truncated; }
				code_lengths[k_code_length_order[i]] = static_cast<uint8_t>(length);
			}
			huffman_decoder lengths_code;
			if (!lengths_code.build(code_lengths, 19, false)) { return inflate_status::bad_code_lengths; }
			uint8_t lengths[286 + 30] = {};
			const uint32_t total = literal_count + distance_count;
			uint32_t at = 0;
			while (at < total) {
				const int symbol = lengths_code.decode(source);
				if (symbol < 0) { return source.count == 0 && source.at == source.end ? inflate_status::truncated : inflate_status::bad_code_lengths; }
				if (symbol < 16) {
					lengths[at++] = static_cast<uint8_t>(symbol);
					continue;
				}
				uint32_t repeat = 0;
				uint8_t value = 0;
				if (symbol == 16) {
					if (at == 0) { return inflate_status::bad_code_lengths; }
					value = lengths[at - 1];
					if (!source.take(2, repeat)) { return inflate_status::truncated; }
					repeat += 3;
				} else if (symbol == 17) {
					if (!source.take(3, repeat)) { return inflate_status::truncated; }
					repeat += 3;
				} else {
					if (!source.take(7, repeat)) { return inflate_status::truncated; }
					repeat += 11;
				}
				if (repeat > total - at) { return inflate_status::bad_code_lengths; }
				std::memset(lengths + at, value, repeat);
				at += repeat;
			}
			// a block that cannot end is refused before it runs
			if (lengths[256] == 0) { return inflate_status::bad_code_lengths; }
			if (!literals.build(lengths, literal_count, true)) { return inflate_status::bad_code_lengths; }
			if (!distances.build(lengths + literal_count, distance_count, true)) { return inflate_status::bad_code_lengths; }
			return inflate_status::ok;
		}

		inflate_status coded_block(const huffman_decoder& a_literals, const huffman_decoder& a_distances) {
			for (;;) {
				if (out_at - charged >= k_work_stride) {
					if (work.exhausted((out_at - charged) / 16)) { return inflate_status::budget; }
					charged = out_at;
				}
				const int symbol = a_literals.decode(source);
				if (symbol < 0) { return source.count == 0 && source.at == source.end ? inflate_status::truncated : inflate_status::bad_symbol; }
				if (symbol < 256) {
					if (out_at == out_size) { return inflate_status::too_much_output; }
					out[out_at++] = static_cast<uint8_t>(symbol);
					continue;
				}
				if (symbol == 256) { return inflate_status::ok; }
				if (symbol > 285) { return inflate_status::bad_symbol; }
				uint32_t extra = 0;
				if (!source.take(k_length_extra[symbol - 257], extra)) { return inflate_status::truncated; }
				const size_t length = size_t {k_length_base[symbol - 257]} + extra;
				const int distance_symbol = a_distances.decode(source);
				if (distance_symbol < 0) { return source.count == 0 && source.at == source.end ? inflate_status::truncated : inflate_status::bad_symbol; }
				if (distance_symbol > 29) { return inflate_status::bad_symbol; }
				if (!source.take(k_distance_extra[distance_symbol], extra)) { return inflate_status::truncated; }
				const size_t distance = size_t {k_distance_base[distance_symbol]} + extra;
				if (distance > out_at) { return inflate_status::distance_too_far; }
				if (length > out_size - out_at) { return inflate_status::too_much_output; }
				const uint8_t* from = out + out_at - distance;
				uint8_t* to = out + out_at;
				// byte by byte: a match may overlap the bytes it writes
				for (size_t i = 0; i < length; ++i) { to[i] = from[i]; }
				out_at += length;
			}
		}

		inflate_status run() {
			for (;;) {
				uint32_t last = 0;
				uint32_t type = 0;
				if (!source.take(1, last) || !source.take(2, type)) { return inflate_status::truncated; }
				inflate_status result = inflate_status::ok;
				if (type == 0) {
					result = stored_block();
				} else if (type == 1) {
					fixed_tables();
					result = coded_block(fixed_literals, fixed_distances);
				} else if (type == 2) {
					// a block's tables cost about a kilobyte of work however little the block holds
					if (work.exhausted(64)) { return inflate_status::budget; }
					result = dynamic_tables();
					if (result == inflate_status::ok) { result = coded_block(literals, distances); }
				} else {
					return inflate_status::bad_block_type;
				}
				if (result != inflate_status::ok) { return result; }
				if (work.exhausted(1)) { return inflate_status::budget; }
				if (last) { return inflate_status::ok; }
			}
		}
	};

	inline uint32_t big_endian_32(const uint8_t* a_bytes) {
		return (uint32_t {a_bytes[0]} << 24) | (uint32_t {a_bytes[1]} << 16) | (uint32_t {a_bytes[2]} << 8) | uint32_t {a_bytes[3]};
	}

	inline constexpr uint8_t k_signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	inline constexpr uint32_t k_ihdr = 0x49484452;
	inline constexpr uint32_t k_plte = 0x504c5445;
	inline constexpr uint32_t k_idat = 0x49444154;
	inline constexpr uint32_t k_iend = 0x49454e44;
	inline constexpr uint32_t k_trns = 0x74524e53;

	inline constexpr uint8_t k_pass_x_start[7] = {0, 4, 0, 2, 0, 1, 0};
	inline constexpr uint8_t k_pass_y_start[7] = {0, 0, 4, 0, 2, 0, 1};
	inline constexpr uint8_t k_pass_x_step[7] = {8, 8, 4, 4, 2, 2, 1};
	inline constexpr uint8_t k_pass_y_step[7] = {8, 8, 8, 4, 4, 2, 2};

	struct image_header {
		uint32_t width = 0;
		uint32_t height = 0;
		uint8_t depth = 0;
		uint8_t colour = 0;
		uint8_t interlace = 0;
		unsigned channels() const { return colour == 0 || colour == 3 ? 1u : colour == 2 ? 3u : colour == 4 ? 2u : 4u; }
		unsigned bits_per_pixel() const { return channels() * depth; }
		// a row of a_width pixels, without its filter byte
		uint64_t row_bytes(uint64_t a_width) const { return (a_width * bits_per_pixel() + 7) / 8; }
	};

	struct chunk {
		uint32_t type = 0;
		const uint8_t* data = nullptr;
		uint32_t size = 0;
	};

	// The next chunk with its CRC checked
	status next_chunk(byte_reader& a_reader, chunk& a_chunk) {
		uint32_t size = 0;
		if (!a_reader.u32_be(size)) { return status::png_truncated; }
		if (size > 0x7fffffffu) { return status::png_bad_header; }
		const uint8_t* type = a_reader.take(4);
		if (!type) { return status::png_truncated; }
		const uint8_t* data = a_reader.take(size);
		uint32_t stored_crc = 0;
		if (!data || !a_reader.u32_be(stored_crc)) { return status::png_truncated; }
		if (crc32(data, size, crc32(type, 4)) != stored_crc) { return status::png_bad_crc; }
		a_chunk.type = big_endian_32(type);
		a_chunk.data = data;
		a_chunk.size = size;
		return status::ok;
	}

	status parse_header(byte_reader& a_reader, const limits& a_limits, image_header& a_header) {
		const uint8_t* signature = a_reader.take(8);
		if (!signature) { return status::png_truncated; }
		if (std::memcmp(signature, k_signature, 8) != 0) { return status::not_an_image; }
		chunk first;
		if (const status result = next_chunk(a_reader, first); result != status::ok) { return result; }
		if (first.type != k_ihdr) { return status::png_bad_chunk_order; }
		if (first.size != 13) { return status::png_bad_header; }
		a_header.width = big_endian_32(first.data);
		a_header.height = big_endian_32(first.data + 4);
		a_header.depth = first.data[8];
		a_header.colour = first.data[9];
		a_header.interlace = first.data[12];
		if (a_header.width == 0 || a_header.height == 0 || a_header.width > 0x7fffffffu || a_header.height > 0x7fffffffu) { return status::png_bad_header; }
		const uint8_t depth = a_header.depth;
		bool depth_fits = false;
		switch (a_header.colour) {
			case 0: depth_fits = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16; break;
			case 3: depth_fits = depth == 1 || depth == 2 || depth == 4 || depth == 8; break;
			case 2:
			case 4:
			case 6: depth_fits = depth == 8 || depth == 16; break;
			default: break;
		}
		if (!depth_fits || first.data[10] != 0 || first.data[11] != 0 || a_header.interlace > 1) { return status::png_bad_header; }
		if (uint64_t {a_header.width} * a_header.height > pixel_limit(a_limits)) { return status::too_many_pixels; }
		return status::ok;
	}

	// The bytes the image's rows inflate to, filter bytes included; within 64 bits for any size under the pixel limit
	uint64_t inflated_size(const image_header& a_header) {
		if (!a_header.interlace) { return uint64_t {a_header.height} * (1 + a_header.row_bytes(a_header.width)); }
		uint64_t total = 0;
		for (int pass = 0; pass < 7; ++pass) {
			const uint64_t pass_width = (uint64_t {a_header.width} + k_pass_x_step[pass] - 1 - k_pass_x_start[pass]) / k_pass_x_step[pass];
			const uint64_t pass_height = (uint64_t {a_header.height} + k_pass_y_step[pass] - 1 - k_pass_y_start[pass]) / k_pass_y_step[pass];
			if (pass_width == 0 || pass_height == 0) { continue; }
			total += pass_height * (1 + a_header.row_bytes(pass_width));
		}
		return total;
	}

	inline uint8_t paeth(uint8_t a_left, uint8_t a_up, uint8_t a_up_left) {
		const int estimate = int {a_left} + a_up - a_up_left;
		const int to_left = estimate > a_left ? estimate - a_left : a_left - estimate;
		const int to_up = estimate > a_up ? estimate - a_up : a_up - estimate;
		const int to_up_left = estimate > a_up_left ? estimate - a_up_left : a_up_left - estimate;
		return to_left <= to_up && to_left <= to_up_left ? a_left : to_up <= to_up_left ? a_up : a_up_left;
	}

	// One row's filter undone in place; a_previous is null for a pass's first row. False for a filter type past 4.
	bool unfilter_row(uint8_t a_filter, uint8_t* a_row, const uint8_t* a_previous, size_t a_size, size_t a_pixel_bytes) {
		switch (a_filter) {
			case 0: return true;
			case 1:
				for (size_t i = a_pixel_bytes; i < a_size; ++i) { a_row[i] = static_cast<uint8_t>(a_row[i] + a_row[i - a_pixel_bytes]); }
				return true;
			case 2:
				if (a_previous) {
					for (size_t i = 0; i < a_size; ++i) { a_row[i] = static_cast<uint8_t>(a_row[i] + a_previous[i]); }
				}
				return true;
			case 3:
				for (size_t i = 0; i < a_size; ++i) {
					const unsigned left = i >= a_pixel_bytes ? a_row[i - a_pixel_bytes] : 0u;
					const unsigned up = a_previous ? a_previous[i] : 0u;
					a_row[i] = static_cast<uint8_t>(a_row[i] + ((left + up) >> 1));
				}
				return true;
			case 4:
				for (size_t i = 0; i < a_size; ++i) {
					const uint8_t left = i >= a_pixel_bytes ? a_row[i - a_pixel_bytes] : uint8_t {0};
					const uint8_t up = a_previous ? a_previous[i] : uint8_t {0};
					const uint8_t up_left = a_previous && i >= a_pixel_bytes ? a_previous[i - a_pixel_bytes] : uint8_t {0};
					a_row[i] = static_cast<uint8_t>(a_row[i] + paeth(left, up, up_left));
				}
				return true;
			default: return false;
		}
	}

	struct transparency {
		bool present = false;
		uint16_t grey = 0;
		uint16_t rgb[3] = {0, 0, 0};
	};

	struct palette {
		unsigned count = 0;
		uint8_t rgba[256 * 4] = {};
	};

	// A 16-bit sample as the 8-bit sample nearest to it
	inline uint8_t narrow_16(unsigned a_value) { return static_cast<uint8_t>((a_value * 255u + 32895u) >> 16); }
	inline unsigned sample_16(const uint8_t* a_bytes) { return (unsigned {a_bytes[0]} << 8) | a_bytes[1]; }

	// a_count pixels of one unfiltered row written as RGBA at a_out + 4 * a_step * i; false for a palette index past the palette
	bool unpack_row(const image_header& a_header, const palette& a_palette, const transparency& a_transparency, const uint8_t* a_row, size_t a_count, uint8_t* a_out, size_t a_step) {
		const unsigned depth = a_header.depth;
		const size_t stride = 4 * a_step;
		switch (a_header.colour) {
			case 0:
				if (depth == 16) {
					for (size_t i = 0; i < a_count; ++i, a_out += stride) {
						const unsigned value = sample_16(a_row + 2 * i);
						a_out[0] = a_out[1] = a_out[2] = narrow_16(value);
						a_out[3] = a_transparency.present && value == a_transparency.grey ? 0 : 255;
					}
				} else {
					const unsigned mask = (1u << depth) - 1;
					const unsigned scale = 255u / mask;
					for (size_t i = 0; i < a_count; ++i, a_out += stride) {
						const size_t bit = i * depth;
						const unsigned value = (a_row[bit >> 3] >> (8 - depth - (bit & 7))) & mask;
						a_out[0] = a_out[1] = a_out[2] = static_cast<uint8_t>(value * scale);
						a_out[3] = a_transparency.present && value == a_transparency.grey ? 0 : 255;
					}
				}
				return true;
			case 2:
				if (depth == 16) {
					for (size_t i = 0; i < a_count; ++i, a_out += stride) {
						const unsigned red = sample_16(a_row + 6 * i);
						const unsigned green = sample_16(a_row + 6 * i + 2);
						const unsigned blue = sample_16(a_row + 6 * i + 4);
						a_out[0] = narrow_16(red);
						a_out[1] = narrow_16(green);
						a_out[2] = narrow_16(blue);
						a_out[3] = a_transparency.present && red == a_transparency.rgb[0] && green == a_transparency.rgb[1] && blue == a_transparency.rgb[2] ? 0 : 255;
					}
				} else {
					for (size_t i = 0; i < a_count; ++i, a_out += stride) {
						const uint8_t* pixel = a_row + 3 * i;
						a_out[0] = pixel[0];
						a_out[1] = pixel[1];
						a_out[2] = pixel[2];
						a_out[3] = a_transparency.present && pixel[0] == a_transparency.rgb[0] && pixel[1] == a_transparency.rgb[1] && pixel[2] == a_transparency.rgb[2] ? 0 : 255;
					}
				}
				return true;
			case 3: {
				const unsigned mask = (1u << depth) - 1;
				for (size_t i = 0; i < a_count; ++i, a_out += stride) {
					const size_t bit = i * depth;
					const unsigned index = (a_row[bit >> 3] >> (8 - depth - (bit & 7))) & mask;
					if (index >= a_palette.count) { return false; }
					std::memcpy(a_out, a_palette.rgba + 4 * index, 4);
				}
				return true;
			}
			case 4:
				for (size_t i = 0; i < a_count; ++i, a_out += stride) {
					if (depth == 16) {
						a_out[0] = a_out[1] = a_out[2] = narrow_16(sample_16(a_row + 4 * i));
						a_out[3] = narrow_16(sample_16(a_row + 4 * i + 2));
					} else {
						a_out[0] = a_out[1] = a_out[2] = a_row[2 * i];
						a_out[3] = a_row[2 * i + 1];
					}
				}
				return true;
			default:
				for (size_t i = 0; i < a_count; ++i, a_out += stride) {
					if (depth == 16) {
						for (int channel = 0; channel < 4; ++channel) { a_out[channel] = narrow_16(sample_16(a_row + 8 * i + 2 * channel)); }
					} else {
						std::memcpy(a_out, a_row + 4 * i, 4);
					}
				}
				return true;
		}
	}

	status inflate_failure(inflate_status a_status) {
		switch (a_status) {
			case inflate_status::ok: return status::ok;
			case inflate_status::budget: return status::budget;
			case inflate_status::too_much_output:
			case inflate_status::too_little_output: return status::png_size_mismatch;
			default: return status::png_bad_compressed_data;
		}
	}

	status decode(const uint8_t* a_data, size_t a_size, const limits& a_limits, const work_hook& a_work, header& a_result, bitmap& a_bitmap) {
		byte_reader reader(a_data, a_size);
		image_header image;
		if (const status result = parse_header(reader, a_limits, image); result != status::ok) { return result; }
		a_result.kind = format::png;
		a_result.width = image.width;
		a_result.height = image.height;
		a_result.orientation = 1;

		palette colours;
		transparency transparent;
		bool palette_seen = false;
		bool data_seen = false;
		bool data_ended = false;
		bool end_seen = false;
		std::vector<uint8_t> compressed;
		while (!end_seen) {
			chunk next;
			if (const status result = next_chunk(reader, next); result != status::ok) { return result; }
			if (next.type != k_idat && data_seen) { data_ended = true; }
			switch (next.type) {
				case k_ihdr: return status::png_bad_chunk_order;
				case k_plte:
					if (palette_seen || data_seen) { return status::png_bad_chunk_order; }
					if (image.colour == 0 || image.colour == 4) { return status::png_bad_palette; }
					if (next.size == 0 || next.size % 3 != 0 || next.size > 256 * 3) { return status::png_bad_palette; }
					if (image.colour == 3 && next.size / 3 > (1u << image.depth)) { return status::png_bad_palette; }
					palette_seen = true;
					colours.count = next.size / 3;
					for (unsigned i = 0; i < colours.count; ++i) {
						std::memcpy(colours.rgba + 4 * i, next.data + 3 * i, 3);
						colours.rgba[4 * i + 3] = 255;
					}
					break;
				case k_trns:
					if (data_seen || transparent.present) { return status::png_bad_chunk_order; }
					if (image.colour == 3) {
						if (!palette_seen) { return status::png_bad_chunk_order; }
						if (next.size > colours.count) { return status::png_bad_palette; }
						for (uint32_t i = 0; i < next.size; ++i) { colours.rgba[4 * i + 3] = next.data[i]; }
					} else if (image.colour == 0) {
						if (next.size != 2) { return status::png_bad_header; }
						transparent.grey = static_cast<uint16_t>(sample_16(next.data));
					} else if (image.colour == 2) {
						if (next.size != 6) { return status::png_bad_header; }
						for (int i = 0; i < 3; ++i) { transparent.rgb[i] = static_cast<uint16_t>(sample_16(next.data + 2 * i)); }
					} else {
						// colour types with an alpha channel carry no tRNS: one that does is ignored, as browsers do
						break;
					}
					transparent.present = image.colour != 3;
					break;
				case k_idat:
					if (data_ended) { return status::png_bad_chunk_order; }
					if (image.colour == 3 && !palette_seen) { return status::png_bad_chunk_order; }
					data_seen = true;
					compressed.insert(compressed.end(), next.data, next.data + next.size);
					break;
				case k_iend:
					end_seen = true;
					break;
				default:
					// bit 5 of the first letter clear: a chunk a decoder must understand
					if (((next.type >> 24) & 0x20) == 0) { return status::png_unknown_critical_chunk; }
					break;
			}
		}
		if (!data_seen) { return status::png_bad_chunk_order; }

		const uint64_t raw_size = inflated_size(image);
		// deflate grows a byte to at most 258 in a little over two bits: a stream this short cannot fill the image
		if (raw_size / 1032 > compressed.size()) { return status::png_size_mismatch; }
		std::vector<uint8_t> raw(static_cast<size_t>(raw_size));
		if (const inflate_status inflated = inflate_zlib(compressed.data(), compressed.size(), raw.data(), raw.size(), a_work); inflated != inflate_status::ok) {
			return inflate_failure(inflated);
		}
		compressed = std::vector<uint8_t>();

		a_bitmap.width = image.width;
		a_bitmap.height = image.height;
		a_bitmap.rgba.assign(size_t {image.width} * image.height * 4, 0);
		const size_t pixel_bytes = image.bits_per_pixel() >= 8 ? image.bits_per_pixel() / 8 : 1;
		const int passes = image.interlace ? 7 : 1;
		size_t at = 0;
		for (int pass = 0; pass < passes; ++pass) {
			const size_t x_start = image.interlace ? k_pass_x_start[pass] : 0;
			const size_t y_start = image.interlace ? k_pass_y_start[pass] : 0;
			const size_t x_step = image.interlace ? k_pass_x_step[pass] : 1;
			const size_t y_step = image.interlace ? k_pass_y_step[pass] : 1;
			const size_t pass_width = (size_t {image.width} + x_step - 1 - x_start) / x_step;
			const size_t pass_height = (size_t {image.height} + y_step - 1 - y_start) / y_step;
			if (pass_width == 0 || pass_height == 0) { continue; }
			const size_t row_size = static_cast<size_t>(image.row_bytes(pass_width));
			const uint8_t* previous = nullptr;
			for (size_t row = 0; row < pass_height; ++row) {
				// inflate_zlib filled raw exactly, so every row of every pass is inside it
				uint8_t* current = raw.data() + at + 1;
				if (!unfilter_row(raw[at], current, previous, row_size, pixel_bytes)) { return status::png_bad_filter; }
				uint8_t* out = a_bitmap.rgba.data() + ((y_start + row * y_step) * image.width + x_start) * 4;
				if (!unpack_row(image, colours, transparent, current, pass_width, out, x_step)) { return status::png_bad_palette; }
				previous = current;
				at += 1 + row_size;
				if (a_work.exhausted(pass_width / 4 + 1)) { return status::budget; }
			}
		}
		return status::ok;
	}

	// The writer's half: deflate with LZ77 over hash chains and lazy matching, in blocks of dynamic Huffman codes (or stored,
	// when that is smaller), and PNG around it

	// Deflate's bits, least significant first
	class bit_writer {
	public:
		explicit bit_writer(std::vector<uint8_t>& a_out) : out_(a_out) {}
		void put(uint32_t a_bits, unsigned a_count) {
			buffer_ |= uint64_t {a_bits & ((uint32_t {1} << a_count) - 1)} << count_;
			count_ += a_count;
			while (count_ >= 8) {
				out_.push_back(static_cast<uint8_t>(buffer_));
				buffer_ >>= 8;
				count_ -= 8;
			}
		}
		// a Huffman code, most significant bit first
		void code(uint32_t a_code, unsigned a_length) {
			uint32_t reversed = 0;
			for (unsigned i = 0; i < a_length; ++i) { reversed |= ((a_code >> i) & 1u) << (a_length - 1 - i); }
			put(reversed, a_length);
		}
		void align() {
			if (count_ > 0) { put(0, 8 - count_); }
		}

	private:
		std::vector<uint8_t>& out_;
		uint64_t buffer_ = 0;
		unsigned count_ = 0;
	};

	// Canonical codes for a set of lengths (RFC 1951 3.2.2)
	void canonical_codes(const uint8_t* a_lengths, unsigned a_count, uint16_t* a_codes) {
		unsigned counts[16] = {};
		for (unsigned i = 0; i < a_count; ++i) { ++counts[a_lengths[i]]; }
		counts[0] = 0;
		uint32_t next[16] = {};
		uint32_t code = 0;
		for (unsigned length = 1; length < 16; ++length) {
			code = (code + counts[length - 1]) << 1;
			next[length] = code;
		}
		for (unsigned i = 0; i < a_count; ++i) { a_codes[i] = a_lengths[i] ? static_cast<uint16_t>(next[a_lengths[i]]++) : uint16_t {0}; }
	}

	// One token: a literal byte (distance 0), or a match of length 3 to 258 at distance 1 to 32768
	struct token {
		uint16_t value;
		uint16_t distance;
	};

	inline unsigned length_symbol(unsigned a_length) {
		unsigned symbol = 0;
		while (symbol + 1 < 29 && k_length_base[symbol + 1] <= a_length) { ++symbol; }
		return symbol;
	}
	inline unsigned distance_symbol(unsigned a_distance) {
		unsigned symbol = 0;
		while (symbol + 1 < 30 && k_distance_base[symbol + 1] <= a_distance) { ++symbol; }
		return symbol;
	}

	class deflater {
	public:
		static constexpr size_t k_window = 32768;
		static constexpr unsigned k_hash_bits = 15;
		static constexpr unsigned k_chain = 96;
		static constexpr unsigned k_nice = 160;
		static constexpr size_t k_block_tokens = 1u << 15;

		deflater(const uint8_t* a_data, size_t a_size, const work_hook& a_work, std::vector<uint8_t>& a_out)
			: data_(a_data), size_(a_size), work_(a_work), bits_(a_out), head_(size_t {1} << k_hash_bits, -1), previous_(k_window, -1) {}

		bool run() {
			tokens_.reserve(k_block_tokens + 2);
			size_t at = 0;
			size_t block_start = 0;
			size_t charged = 0;
			while (at < size_) {
				size_t length = 0;
				size_t distance = 0;
				longest_match(at, length, distance);
				// lazy: a longer match one byte on makes this byte a literal
				while (length >= 3 && length < k_nice && at + 1 < size_) {
					insert(at);
					size_t next_length = 0;
					size_t next_distance = 0;
					longest_match(at + 1, next_length, next_distance);
					if (next_length <= length) {
						break;
					}
					tokens_.push_back({data_[at], 0});
					++at;
					length = next_length;
					distance = next_distance;
				}
				if (length >= 3) {
					tokens_.push_back({static_cast<uint16_t>(length), static_cast<uint16_t>(distance)});
					insert(at);
					for (size_t i = 1; i < length; ++i) { insert(at + i); }
					at += length;
				} else {
					tokens_.push_back({data_[at], 0});
					insert(at);
					++at;
				}
				if (tokens_.size() >= k_block_tokens) {
					block(block_start, at, false);
					block_start = at;
				}
				if (at - charged >= (size_t {1} << 16)) {
					if (work_.exhausted((at - charged) / 4)) { return false; }
					charged = at;
				}
			}
			block(block_start, at, true);
			bits_.align();
			return true;
		}

	private:
		unsigned hash(size_t a_at) const {
			return ((uint32_t {data_[a_at]} << 10) ^ (uint32_t {data_[a_at + 1]} << 5) ^ data_[a_at + 2]) & ((1u << k_hash_bits) - 1);
		}
		void insert(size_t a_at) {
			if (a_at + 3 > size_ || a_at == last_inserted_) { return; }
			last_inserted_ = a_at;
			const unsigned slot = hash(a_at);
			previous_[a_at % k_window] = head_[slot];
			head_[slot] = static_cast<int64_t>(a_at);
		}
		void longest_match(size_t a_at, size_t& a_length, size_t& a_distance) const {
			a_length = 0;
			if (a_at + 3 > size_) { return; }
			const size_t most = size_ - a_at < 258 ? size_ - a_at : 258;
			int64_t candidate = head_[hash(a_at)];
			for (unsigned chain = 0; chain < k_chain && candidate >= 0; ++chain) {
				const size_t from = static_cast<size_t>(candidate);
				if (from >= a_at || a_at - from > k_window) { break; }
				if (data_[from + a_length] == data_[a_at + a_length] || a_length == 0) {
					size_t length = 0;
					while (length < most && data_[from + length] == data_[a_at + length]) { ++length; }
					if (length > a_length) {
						a_length = length;
						a_distance = a_at - from;
						if (length >= most) { break; }
					}
				}
				const int64_t next = previous_[from % k_window];
				// a slot the window has since reused points forward, past the chain's end
				if (next >= candidate) { break; }
				candidate = next;
			}
			if (a_length < 3) { a_length = 0; }
		}

		void block(size_t a_start, size_t a_end, bool a_last) {
			uint32_t literals[286] = {};
			uint32_t distances[30] = {};
			for (const token& each : tokens_) {
				if (each.distance == 0) {
					++literals[each.value];
				} else {
					++literals[257 + length_symbol(each.value)];
					++distances[distance_symbol(each.distance)];
				}
			}
			literals[256] = 1;
			uint8_t lengths[286 + 30] = {};
			code_lengths(literals, 286, 15, false, lengths);
			code_lengths(distances, 30, 15, false, lengths + 286);
			unsigned literal_count = 286;
			while (literal_count > 257 && lengths[literal_count - 1] == 0) { --literal_count; }
			unsigned distance_count = 30;
			while (distance_count > 1 && lengths[286 + distance_count - 1] == 0) { --distance_count; }
			// the lengths run-length coded with symbols 16, 17 and 18
			uint8_t all[286 + 30];
			std::memcpy(all, lengths, literal_count);
			std::memcpy(all + literal_count, lengths + 286, distance_count);
			const unsigned total = literal_count + distance_count;
			std::vector<std::pair<uint8_t, uint8_t>> runs;
			for (unsigned i = 0; i < total;) {
				unsigned repeat = 1;
				while (i + repeat < total && all[i + repeat] == all[i]) { ++repeat; }
				if (all[i] == 0 && repeat >= 3) {
					const unsigned take = repeat > 138 ? 138 : repeat;
					runs.push_back(take >= 11 ? std::pair<uint8_t, uint8_t> {18, static_cast<uint8_t>(take - 11)} : std::pair<uint8_t, uint8_t> {17, static_cast<uint8_t>(take - 3)});
					i += take;
				} else if (all[i] != 0 && repeat >= 4) {
					runs.push_back({all[i], 0});
					const unsigned take = repeat - 1 > 6 ? 6 : repeat - 1;
					runs.push_back({16, static_cast<uint8_t>(take - 3)});
					i += 1 + take;
				} else {
					runs.push_back({all[i], 0});
					++i;
				}
			}
			uint32_t length_frequency[19] = {};
			for (const auto& run : runs) { ++length_frequency[run.first]; }
			// the code-length code must be complete: a second symbol when the runs use one
			unsigned used = 0;
			for (uint32_t each : length_frequency) { used += each ? 1 : 0; }
			if (used == 1) { ++length_frequency[length_frequency[0] ? 1 : 0]; }
			uint8_t length_lengths[19];
			code_lengths(length_frequency, 19, 7, false, length_lengths);
			unsigned length_count = 19;
			while (length_count > 4 && length_lengths[k_code_length_order[length_count - 1]] == 0) { --length_count; }

			// its size coded, against its size stored
			uint64_t coded_bits = 3 + 5 + 5 + 4 + 3 * uint64_t {length_count};
			for (const auto& run : runs) { coded_bits += length_lengths[run.first] + (run.first == 16 ? 2 : run.first == 17 ? 3 : run.first == 18 ? 7 : 0); }
			for (const token& each : tokens_) {
				if (each.distance == 0) {
					coded_bits += lengths[each.value];
				} else {
					const unsigned length = length_symbol(each.value);
					const unsigned distance = distance_symbol(each.distance);
					coded_bits += lengths[257 + length] + k_length_extra[length] + lengths[286 + distance] + k_distance_extra[distance];
				}
			}
			coded_bits += lengths[256];
			const size_t raw = a_end - a_start;
			const uint64_t stored_bits = (raw + 5 * ((raw + 65534) / 65535 + (raw == 0 ? 1 : 0))) * 8 + 8;
			if (stored_bits < coded_bits) {
				stored(a_start, a_end, a_last);
				tokens_.clear();
				return;
			}
			uint16_t codes[286 + 30];
			canonical_codes(lengths, 286, codes);
			canonical_codes(lengths + 286, 30, codes + 286);
			uint16_t length_codes[19];
			canonical_codes(length_lengths, 19, length_codes);
			bits_.put(a_last ? 1 : 0, 1);
			bits_.put(2, 2);
			bits_.put(literal_count - 257, 5);
			bits_.put(distance_count - 1, 5);
			bits_.put(length_count - 4, 4);
			for (unsigned i = 0; i < length_count; ++i) { bits_.put(length_lengths[k_code_length_order[i]], 3); }
			for (const auto& run : runs) {
				bits_.code(length_codes[run.first], length_lengths[run.first]);
				if (run.first == 16) { bits_.put(run.second, 2); }
				if (run.first == 17) { bits_.put(run.second, 3); }
				if (run.first == 18) { bits_.put(run.second, 7); }
			}
			for (const token& each : tokens_) {
				if (each.distance == 0) {
					bits_.code(codes[each.value], lengths[each.value]);
					continue;
				}
				const unsigned length = length_symbol(each.value);
				bits_.code(codes[257 + length], lengths[257 + length]);
				bits_.put(each.value - k_length_base[length], k_length_extra[length]);
				const unsigned distance = distance_symbol(each.distance);
				bits_.code(codes[286 + distance], lengths[286 + distance]);
				bits_.put(each.distance - k_distance_base[distance], k_distance_extra[distance]);
			}
			bits_.code(codes[256], lengths[256]);
			tokens_.clear();
		}

		void stored(size_t a_start, size_t a_end, bool a_last) {
			size_t at = a_start;
			do {
				const size_t length = a_end - at < 65535 ? a_end - at : 65535;
				const bool last = a_last && at + length == a_end;
				bits_.put(last ? 1 : 0, 1);
				bits_.put(0, 2);
				bits_.align();
				bits_.put(static_cast<uint32_t>(length), 16);
				bits_.put(static_cast<uint32_t>(~length) & 0xffffu, 16);
				for (size_t i = 0; i < length; ++i) { bits_.put(data_[at + i], 8); }
				at += length;
			} while (at < a_end);
		}

		const uint8_t* data_;
		size_t size_;
		const work_hook& work_;
		bit_writer bits_;
		std::vector<int64_t> head_;
		std::vector<int64_t> previous_;
		std::vector<token> tokens_;
		size_t last_inserted_ = SIZE_MAX;
	};

	inline void put_big_32(std::vector<uint8_t>& a_out, uint32_t a_value) {
		for (int shift = 24; shift >= 0; shift -= 8) { a_out.push_back(static_cast<uint8_t>(a_value >> shift)); }
	}

	void put_chunk(std::vector<uint8_t>& a_out, const char* a_type, const uint8_t* a_data, size_t a_size) {
		put_big_32(a_out, static_cast<uint32_t>(a_size));
		const size_t start = a_out.size();
		a_out.insert(a_out.end(), a_type, a_type + 4);
		a_out.insert(a_out.end(), a_data, a_data + a_size);
		put_big_32(a_out, crc32(a_out.data() + start, a_size + 4));
	}

	// Each row's filter: the one whose bytes, read as signed, sum smallest in magnitude (the heuristic the PNG specification
	// suggests), then the rows deflated
	status encode(const bitmap& a_bitmap, const work_hook& a_work, std::vector<uint8_t>& a_bytes) {
		const uint32_t width = a_bitmap.width;
		const uint32_t height = a_bitmap.height;
		const uint8_t* pixels = a_bitmap.rgba.data();
		const size_t count = size_t {width} * height;
		bool opaque = true;
		bool grey = true;
		for (size_t i = 0; i < count && (opaque || grey); ++i) {
			opaque = opaque && pixels[4 * i + 3] == 255;
			grey = grey && pixels[4 * i] == pixels[4 * i + 1] && pixels[4 * i + 1] == pixels[4 * i + 2];
		}
		const uint8_t colour = grey ? (opaque ? 0 : 4) : (opaque ? 2 : 6);
		const size_t channels = grey ? (opaque ? 1 : 2) : (opaque ? 3 : 4);
		const size_t row_bytes = size_t {width} * channels;
		std::vector<uint8_t> raw(height * (1 + row_bytes));
		std::vector<uint8_t> current(row_bytes);
		std::vector<uint8_t> previous(row_bytes, 0);
		std::vector<uint8_t> trial(row_bytes);
		for (uint32_t y = 0; y < height; ++y) {
			const uint8_t* in = pixels + size_t {y} * width * 4;
			for (size_t x = 0; x < width; ++x) {
				uint8_t* out = current.data() + x * channels;
				if (grey) {
					out[0] = in[4 * x];
					if (!opaque) { out[1] = in[4 * x + 3]; }
				} else {
					std::memcpy(out, in + 4 * x, channels);
				}
			}
			uint8_t* row = raw.data() + size_t {y} * (1 + row_bytes);
			uint64_t best = UINT64_MAX;
			for (uint8_t filter = 0; filter <= 4; ++filter) {
				uint64_t cost = 0;
				for (size_t i = 0; i < row_bytes; ++i) {
					const unsigned left = i >= channels ? current[i - channels] : 0u;
					const unsigned up = y > 0 ? previous[i] : 0u;
					const unsigned up_left = y > 0 && i >= channels ? previous[i - channels] : 0u;
					unsigned predicted = 0;
					switch (filter) {
						case 1: predicted = left; break;
						case 2: predicted = up; break;
						case 3: predicted = (left + up) >> 1; break;
						case 4: predicted = paeth(static_cast<uint8_t>(left), static_cast<uint8_t>(up), static_cast<uint8_t>(up_left)); break;
						default: break;
					}
					trial[i] = static_cast<uint8_t>(current[i] - predicted);
					cost += trial[i] < 128 ? trial[i] : 256 - trial[i];
				}
				if (cost < best) {
					best = cost;
					row[0] = filter;
					std::memcpy(row + 1, trial.data(), row_bytes);
				}
			}
			std::swap(previous, current);
			if (a_work.exhausted(row_bytes * 5 / 16 + 1)) { return status::budget; }
		}
		std::vector<uint8_t> stream;
		if (!deflate_zlib(raw.data(), raw.size(), a_work, stream)) { return status::budget; }
		std::vector<uint8_t>& out = a_bytes;
		out.assign(k_signature, k_signature + 8);
		uint8_t header_data[13];
		for (int i = 0; i < 4; ++i) {
			header_data[i] = static_cast<uint8_t>(width >> (24 - 8 * i));
			header_data[4 + i] = static_cast<uint8_t>(height >> (24 - 8 * i));
		}
		header_data[8] = 8;
		header_data[9] = colour;
		header_data[10] = header_data[11] = header_data[12] = 0;
		put_chunk(out, "IHDR", header_data, 13);
		for (size_t at = 0; at < stream.size(); at += size_t {1} << 16) {
			const size_t length = stream.size() - at < (size_t {1} << 16) ? stream.size() - at : size_t {1} << 16;
			put_chunk(out, "IDAT", stream.data() + at, length);
		}
		put_chunk(out, "IEND", nullptr, 0);
		return status::ok;
	}

} // namespace png_detail

	uint32_t crc32(const uint8_t* a_data, size_t a_size, uint32_t a_crc) {
		uint32_t c = a_crc ^ 0xffffffffu;
		for (size_t i = 0; i < a_size; ++i) { c = png_detail::k_crc_table[(c ^ a_data[i]) & 0xff] ^ (c >> 8); }
		return c ^ 0xffffffffu;
	}

	uint32_t adler32(const uint8_t* a_data, size_t a_size, uint32_t a_adler) {
		uint32_t low = a_adler & 0xffff;
		uint32_t high = a_adler >> 16;
		// 5552 bytes is the longest run whose sums stay inside 32 bits before the modulo
		while (a_size > 0) {
			const size_t run = a_size < 5552 ? a_size : 5552;
			for (size_t i = 0; i < run; ++i) {
				low += a_data[i];
				high += low;
			}
			low %= 65521;
			high %= 65521;
			a_data += run;
			a_size -= run;
		}
		return (high << 16) | low;
	}

	inflate_status inflate_zlib(const uint8_t* a_data, size_t a_size, uint8_t* a_out, size_t a_out_size, const work_hook& a_work) {
		if (a_size < 2) { return inflate_status::truncated; }
		const unsigned method = a_data[0];
		const unsigned flags = a_data[1];
		if ((method & 0x0f) != 8 || (method >> 4) > 7 || ((method << 8) | flags) % 31 != 0 || (flags & 0x20) != 0) { return inflate_status::bad_stream_header; }
		png_detail::inflater state {png_detail::bit_source {a_data + 2, a_data + a_size}, a_out, a_out_size, 0, 0, a_work};
		if (const inflate_status result = state.run(); result != inflate_status::ok) { return result; }
		if (state.out_at != a_out_size) { return inflate_status::too_little_output; }
		state.source.align_to_byte();
		uint32_t stored = 0;
		for (int i = 0; i < 4; ++i) {
			uint32_t byte = 0;
			if (!state.source.take(8, byte)) { return inflate_status::truncated; }
			stored = (stored << 8) | byte;
		}
		if (stored != adler32(a_out, a_out_size)) { return inflate_status::bad_checksum; }
		return inflate_status::ok;
	}

	bool deflate_zlib(const uint8_t* a_data, size_t a_size, const work_hook& a_work, std::vector<uint8_t>& a_out) {
		a_out.assign({0x78, 0x9c});
		png_detail::deflater squeeze(a_data, a_size, a_work, a_out);
		if (!squeeze.run()) { return false; }
		png_detail::put_big_32(a_out, adler32(a_data, a_size));
		return true;
	}

	status encode_png(const bitmap& a_bitmap, const work_hook& a_work, std::vector<uint8_t>& a_bytes) {
		try {
			return png_detail::encode(a_bitmap, a_work, a_bytes);
		} catch (const std::bad_alloc&) {
			return status::out_of_memory;
		} catch (const std::length_error&) {
			return status::out_of_memory;
		}
	}

	status read_png_header(const uint8_t* a_data, size_t a_size, const limits& a_limits, header& a_header) {
		byte_reader reader(a_data, a_size);
		png_detail::image_header image;
		if (const status result = png_detail::parse_header(reader, a_limits, image); result != status::ok) { return result; }
		a_header.kind = format::png;
		a_header.width = image.width;
		a_header.height = image.height;
		a_header.orientation = 1;
		return status::ok;
	}

	status decode_png(const uint8_t* a_data, size_t a_size, const limits& a_limits, const work_hook& a_work, header& a_header, bitmap& a_bitmap) {
		try {
			return png_detail::decode(a_data, a_size, a_limits, a_work, a_header, a_bitmap);
		} catch (const std::bad_alloc&) {
			return status::out_of_memory;
		} catch (const std::length_error&) {
			// a buffer past what a 32-bit host can address
			return status::out_of_memory;
		}
	}

} // namespace image_codec
} // namespace jai

#endif
