#pragma once

// What the image:: tests build files with: a PNG writer that takes its rows as given (stored deflate blocks, any header
// fields, any chunks), the PNG filters by their definition, and a deflate bit writer. Written apart from the module's
// own encoders so a test's file never comes from the code under test.

#include <jaiscript/detail/image_codec.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace jai {
namespace image_test {

	using bytes = std::vector<uint8_t>;

	inline uint64_t fnv(const bytes& a_bytes) {
		uint64_t hash = 14695981039346656037ull;
		for (uint8_t b : a_bytes) {
			hash ^= b;
			hash *= 1099511628211ull;
		}
		return hash;
	}

	inline std::string hex(const bytes& a_bytes) {
		static constexpr char k_digits[] = "0123456789abcdef";
		std::string text;
		for (size_t i = 0; i < a_bytes.size() && i < 64; ++i) {
			text.push_back(k_digits[a_bytes[i] >> 4]);
			text.push_back(k_digits[a_bytes[i] & 15]);
		}
		return a_bytes.size() > 64 ? text + "..." : text;
	}

	// How far a decode is from its reference, channel by channel
	struct difference {
		int largest = 0;
		double mean = 0;
		std::string describe() const { return "largest " + std::to_string(largest) + ", mean " + std::to_string(mean); }
	};

	inline difference compare(const image_codec::bitmap& a_pixels, const image_codec::bitmap& a_reference) {
		difference result;
		if (a_pixels.width != a_reference.width || a_pixels.height != a_reference.height || a_pixels.rgba.size() != a_reference.rgba.size()) {
			result.largest = 256;
			return result;
		}
		uint64_t total = 0;
		for (size_t i = 0; i < a_pixels.rgba.size(); ++i) {
			const int gap = a_pixels.rgba[i] > a_reference.rgba[i] ? a_pixels.rgba[i] - a_reference.rgba[i] : a_reference.rgba[i] - a_pixels.rgba[i];
			result.largest = gap > result.largest ? gap : result.largest;
			total += static_cast<uint64_t>(gap);
		}
		result.mean = a_pixels.rgba.empty() ? 0 : static_cast<double>(total) / static_cast<double>(a_pixels.rgba.size());
		return result;
	}

	// The peak signal-to-noise ratio of the colour channels, in dB (99 for identical pixels)
	inline double psnr(const image_codec::bitmap& a_reference, const image_codec::bitmap& a_pixels) {
		if (a_reference.rgba.size() != a_pixels.rgba.size() || a_pixels.rgba.empty()) { return 0; }
		double squares = 0;
		size_t count = 0;
		for (size_t i = 0; i < a_pixels.rgba.size(); i += 4) {
			for (size_t c = 0; c < 3; ++c, ++count) {
				const double gap = static_cast<double>(a_pixels.rgba[i + c]) - static_cast<double>(a_reference.rgba[i + c]);
				squares += gap * gap;
			}
		}
		if (squares == 0) { return 99; }
		const double mean = squares / static_cast<double>(count);
		// 10 log10(255^2 / mean), without <cmath>: log10 by halving to [1, 2) and a series for the rest
		double ratio = 255.0 * 255.0 / mean;
		double tens = 0;
		while (ratio >= 10) { ratio /= 10; tens += 1; }
		while (ratio < 1) { ratio *= 10; tens -= 1; }
		const double y = (ratio - 1) / (ratio + 1);
		double term = y, sum = 0;
		for (int k = 1; k < 60; k += 2) { sum += term / k; term *= y * y; }
		return 10 * (tens + 2 * sum / 2.302585092994046);
	}

	inline void put_32(bytes& a_out, uint32_t a_value) {
		for (int shift = 24; shift >= 0; shift -= 8) { a_out.push_back(static_cast<uint8_t>(a_value >> shift)); }
	}
	inline void put_16(bytes& a_out, uint32_t a_value) {
		a_out.push_back(static_cast<uint8_t>(a_value >> 8));
		a_out.push_back(static_cast<uint8_t>(a_value));
	}

	// a_raw as a zlib stream of stored blocks
	inline bytes stored_zlib(const bytes& a_raw) {
		bytes out = {0x78, 0x01};
		size_t at = 0;
		do {
			const size_t length = a_raw.size() - at < 65535 ? a_raw.size() - at : 65535;
			out.push_back(at + length == a_raw.size() ? 1 : 0);
			out.push_back(static_cast<uint8_t>(length));
			out.push_back(static_cast<uint8_t>(length >> 8));
			out.push_back(static_cast<uint8_t>(~length));
			out.push_back(static_cast<uint8_t>(~length >> 8));
			out.insert(out.end(), a_raw.begin() + at, a_raw.begin() + at + length);
			at += length;
		} while (at < a_raw.size());
		put_32(out, image_codec::adler32(a_raw.data(), a_raw.size()));
		return out;
	}

	// Deflate blocks a test wrote, as the zlib stream of a_plain
	inline bytes zlib_wrap(const bytes& a_deflate, const bytes& a_plain) {
		bytes out = {0x78, 0x01};
		out.insert(out.end(), a_deflate.begin(), a_deflate.end());
		put_32(out, image_codec::adler32(a_plain.data(), a_plain.size()));
		return out;
	}

	// Deflate's bit order: plain fields least significant bit first, Huffman codes most significant bit first
	class bit_writer {
	public:
		void bits(unsigned a_value, unsigned a_count) {
			for (unsigned i = 0; i < a_count; ++i) { bit((a_value >> i) & 1); }
		}
		void code(unsigned a_code, unsigned a_length) {
			for (unsigned i = a_length; i-- > 0;) { bit((a_code >> i) & 1); }
		}
		void fixed_literal(unsigned a_byte) {
			if (a_byte < 144) { code(0x30 + a_byte, 8); } else { code(0x190 + (a_byte - 144), 9); }
		}
		// 256 (the end of block) to 287
		void fixed_length_symbol(unsigned a_symbol) {
			if (a_symbol < 280) { code(a_symbol - 256, 7); } else { code(0xc0 + (a_symbol - 280), 8); }
		}
		bytes finish() const { return out_; }

	private:
		void bit(unsigned a_bit) {
			if (used_ == 0) { out_.push_back(0); }
			out_.back() = static_cast<uint8_t>(out_.back() | (a_bit << used_));
			used_ = (used_ + 1) & 7;
		}
		bytes out_;
		unsigned used_ = 0;
	};

	// Unfiltered rows as a PNG's raw stream: each row behind filter type 0
	inline bytes rows(const std::vector<bytes>& a_rows) {
		bytes out;
		for (const bytes& row : a_rows) {
			out.push_back(0);
			out.insert(out.end(), row.begin(), row.end());
		}
		return out;
	}

	// a_plain's rows of a_row_bytes each filtered as a_filters says, by the definitions in the PNG specification
	inline bytes filter_rows(const bytes& a_plain, size_t a_row_bytes, size_t a_pixel_bytes, const std::vector<uint8_t>& a_filters) {
		bytes out;
		for (size_t row = 0; row < a_filters.size(); ++row) {
			out.push_back(a_filters[row]);
			for (size_t i = 0; i < a_row_bytes; ++i) {
				const int value = a_plain[row * a_row_bytes + i];
				const int left = i >= a_pixel_bytes ? a_plain[row * a_row_bytes + i - a_pixel_bytes] : 0;
				const int up = row > 0 ? a_plain[(row - 1) * a_row_bytes + i] : 0;
				const int corner = row > 0 && i >= a_pixel_bytes ? a_plain[(row - 1) * a_row_bytes + i - a_pixel_bytes] : 0;
				int predicted = 0;
				switch (a_filters[row]) {
					case 1: predicted = left; break;
					case 2: predicted = up; break;
					case 3: predicted = (left + up) / 2; break;
					case 4: {
						const int p = left + up - corner;
						const int pa = p > left ? p - left : left - p;
						const int pb = p > up ? p - up : up - p;
						const int pc = p > corner ? p - corner : corner - p;
						predicted = pa <= pb && pa <= pc ? left : pb <= pc ? up : corner;
						break;
					}
					default: break;
				}
				out.push_back(static_cast<uint8_t>(value - predicted));
			}
		}
		return out;
	}

	using named_chunk = std::pair<std::string, bytes>;

	// What a test bends in the file png_file writes
	struct png_parts {
		std::vector<named_chunk> before_data;
		std::vector<named_chunk> between_data;
		std::vector<named_chunk> after_data;
		// the IDAT chunks the compressed stream is cut into (0: none at all)
		size_t data_pieces = 1;
		bool empty_data_chunk = false;
		bool end = true;
		uint8_t compression_method = 0;
		uint8_t filter_method = 0;
		// the byte of the compressed stream to damage (-1: none)
		long corrupt_data_byte = -1;
	};

	inline void put_chunk(bytes& a_out, const std::string& a_type, const bytes& a_data) {
		put_32(a_out, static_cast<uint32_t>(a_data.size()));
		bytes body(a_type.begin(), a_type.end());
		body.insert(body.end(), a_data.begin(), a_data.end());
		a_out.insert(a_out.end(), body.begin(), body.end());
		put_32(a_out, image_codec::crc32(body.data(), body.size()));
	}

	// A PNG whose header says what the caller says and whose data is a_raw (filter bytes included) in stored blocks
	inline bytes png_file(uint32_t a_width, uint32_t a_height, uint8_t a_depth, uint8_t a_colour, uint8_t a_interlace, const bytes& a_raw, const png_parts& a_parts = {}) {
		bytes out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
		bytes header;
		put_32(header, a_width);
		put_32(header, a_height);
		header.insert(header.end(), {a_depth, a_colour, a_parts.compression_method, a_parts.filter_method, a_interlace});
		put_chunk(out, "IHDR", header);
		for (const named_chunk& each : a_parts.before_data) { put_chunk(out, each.first, each.second); }
		bytes stream = stored_zlib(a_raw);
		if (a_parts.corrupt_data_byte >= 0) { stream[static_cast<size_t>(a_parts.corrupt_data_byte)] ^= 0x0f; }
		for (size_t piece = 0; piece < a_parts.data_pieces; ++piece) {
			const size_t from = stream.size() * piece / a_parts.data_pieces;
			const size_t to = stream.size() * (piece + 1) / a_parts.data_pieces;
			put_chunk(out, "IDAT", bytes(stream.begin() + from, stream.begin() + to));
			if (piece == 0) {
				if (a_parts.empty_data_chunk) { put_chunk(out, "IDAT", {}); }
				for (const named_chunk& each : a_parts.between_data) { put_chunk(out, each.first, each.second); }
			}
		}
		for (const named_chunk& each : a_parts.after_data) { put_chunk(out, each.first, each.second); }
		if (a_parts.end) { put_chunk(out, "IEND", {}); }
		return out;
	}

} // namespace image_test
} // namespace jai
