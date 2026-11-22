#ifdef JAISCRIPT_ENABLE_IMAGE

#include <jaiscript/detail/image_codec.hpp>
#include <cstring>
#include <new>
#include <stdexcept>
#include <utility>

// GIF (87a and 89a) written from the GIF89a specification: the first frame of a file, over the logical screen.

namespace jai {
namespace image_codec {
namespace gif_detail {

	inline uint32_t little_16(const uint8_t* a_bytes) { return uint32_t {a_bytes[0]} | (uint32_t {a_bytes[1]} << 8); }

	struct screen {
		uint32_t width = 0;
		uint32_t height = 0;
		bool has_table = false;
		unsigned table_size = 0;
		const uint8_t* table = nullptr;
	};

	status parse_screen(byte_reader& a_reader, const limits& a_limits, screen& a_screen) {
		const uint8_t* signature = a_reader.take(6);
		if (!signature) { return status::gif_truncated; }
		if (std::memcmp(signature, "GIF87a", 6) != 0 && std::memcmp(signature, "GIF89a", 6) != 0) { return status::not_an_image; }
		const uint8_t* descriptor = a_reader.take(7);
		if (!descriptor) { return status::gif_truncated; }
		a_screen.width = little_16(descriptor);
		a_screen.height = little_16(descriptor + 2);
		if (a_screen.width == 0 || a_screen.height == 0) { return status::gif_bad_header; }
		if (uint64_t {a_screen.width} * a_screen.height > pixel_limit(a_limits)) { return status::too_many_pixels; }
		const uint8_t packed = descriptor[4];
		a_screen.has_table = (packed & 0x80) != 0;
		if (a_screen.has_table) {
			a_screen.table_size = 2u << (packed & 7);
			a_screen.table = a_reader.take(3 * size_t {a_screen.table_size});
			if (!a_screen.table) { return status::gif_truncated; }
		}
		return status::ok;
	}

	// The data sub-blocks that follow, joined (a_out null: skipped); false when the file ends inside them
	bool sub_blocks(byte_reader& a_reader, std::vector<uint8_t>* a_out) {
		for (;;) {
			uint8_t size = 0;
			if (!a_reader.u8(size)) { return false; }
			if (size == 0) { return true; }
			const uint8_t* block = a_reader.take(size);
			if (!block) { return false; }
			if (a_out) { a_out->insert(a_out->end(), block, block + size); }
		}
	}

	// GIF's variable-length LZW (codes of the minimum size plus one, growing to 12 bits) into exactly a_count indexes; a code
	// past the table is refused, and data that ends first is a cut file
	status lzw(const std::vector<uint8_t>& a_data, unsigned a_minimum, uint8_t* a_out, size_t a_count, const work_hook& a_work) {
		if (a_minimum < 2 || a_minimum > 8) { return status::gif_bad_data; }
		constexpr unsigned k_codes = 4096;
		// each code is the code before its last index (always a smaller code) and that index: a string is walked out of the
		// table into a stack, never recursed into
		std::vector<uint16_t> prefix(k_codes);
		std::vector<uint8_t> suffix(k_codes);
		std::vector<uint8_t> stack(k_codes + 1);
		const unsigned clear = 1u << a_minimum;
		const unsigned end = clear + 1;
		for (unsigned i = 0; i < clear; ++i) { suffix[i] = static_cast<uint8_t>(i); }
		unsigned size = a_minimum + 1;
		unsigned next = clear + 2;
		int previous = -1;
		size_t written = 0;
		size_t charged = 0;
		uint32_t bits = 0;
		unsigned count = 0;
		size_t at = 0;
		while (written < a_count) {
			while (count < size) {
				if (at >= a_data.size()) { return status::gif_truncated; }
				bits |= uint32_t {a_data[at++]} << count;
				count += 8;
			}
			const unsigned code = bits & ((1u << size) - 1);
			bits >>= size;
			count -= size;
			if (code == clear) {
				size = a_minimum + 1;
				next = clear + 2;
				previous = -1;
				continue;
			}
			if (code == end) { return status::gif_truncated; }
			// the first code after a clear is a single index; any later one is in the table or, one past it, the previous
			// string followed by its own first index
			if (previous < 0 ? code >= clear : code > next) { return status::gif_bad_data; }
			const bool repeated = previous >= 0 && code == next;
			unsigned walk = repeated ? static_cast<unsigned>(previous) : code;
			size_t top = 0;
			while (walk >= clear) {
				stack[top++] = suffix[walk];
				walk = prefix[walk];
			}
			stack[top++] = suffix[walk];
			const uint8_t first = suffix[walk];
			for (size_t i = 0, j = top - 1; i < j; ++i, --j) { std::swap(stack[i], stack[j]); }
			if (repeated) { stack[top++] = first; }
			if (previous >= 0 && next < k_codes) {
				prefix[next] = static_cast<uint16_t>(previous);
				suffix[next] = first;
				++next;
				// the code size grows as the table reaches its width, to 12 bits; a full table takes no more codes until a clear
				if (next == (1u << size) && size < 12) { ++size; }
			}
			const size_t take = top < a_count - written ? top : a_count - written;
			std::memcpy(a_out + written, stack.data(), take);
			written += take;
			previous = static_cast<int>(code);
			if (written - charged >= (size_t {1} << 16)) {
				if (a_work.exhausted((written - charged) / 16)) { return status::budget; }
				charged = written;
			}
		}
		return status::ok;
	}

	status decode(const uint8_t* a_data, size_t a_size, const limits& a_limits, const work_hook& a_work, header& a_header, bitmap& a_bitmap) {
		byte_reader reader(a_data, a_size);
		screen canvas;
		if (const status result = parse_screen(reader, a_limits, canvas); result != status::ok) { return result; }
		a_header.kind = format::gif;
		a_header.width = canvas.width;
		a_header.height = canvas.height;
		a_header.orientation = 1;
		int transparent = -1;
		for (;;) {
			uint8_t introducer = 0;
			if (!reader.u8(introducer)) { return status::gif_truncated; }
			if (introducer == 0x3b) { return status::gif_no_image; }
			if (introducer == 0x21) {
				uint8_t label = 0;
				if (!reader.u8(label)) { return status::gif_truncated; }
				if (label == 0xf9) {
					// the graphic control extension: the transparent index of the image that follows
					uint8_t size = 0;
					if (!reader.u8(size)) { return status::gif_truncated; }
					const uint8_t* control = reader.take(size);
					if (!control) { return status::gif_truncated; }
					if (size >= 4) { transparent = (control[0] & 1) ? control[3] : -1; }
				}
				if (!sub_blocks(reader, nullptr)) { return status::gif_truncated; }
				continue;
			}
			if (introducer != 0x2c) { return status::gif_bad_frame; }
			const uint8_t* descriptor = reader.take(9);
			if (!descriptor) { return status::gif_truncated; }
			const uint32_t left = little_16(descriptor);
			const uint32_t top = little_16(descriptor + 2);
			const uint32_t width = little_16(descriptor + 4);
			const uint32_t height = little_16(descriptor + 6);
			const uint8_t packed = descriptor[8];
			if (width == 0 || height == 0) { return status::gif_bad_frame; }
			const uint8_t* table = canvas.table;
			unsigned table_size = canvas.has_table ? canvas.table_size : 0;
			if (packed & 0x80) {
				table_size = 2u << (packed & 7);
				table = reader.take(3 * size_t {table_size});
				if (!table) { return status::gif_truncated; }
			}
			if (table_size == 0) { return status::gif_bad_frame; }
			uint8_t minimum = 0;
			if (!reader.u8(minimum)) { return status::gif_truncated; }
			std::vector<uint8_t> compressed;
			const bool whole = sub_blocks(reader, &compressed);
			// the frame's indexes; a frame is at most 65535 x 65535, but its pixels count against the limit like the screen's
			const uint64_t pixels = uint64_t {width} * height;
			if (pixels > pixel_limit(a_limits)) { return status::too_many_pixels; }
			std::vector<uint8_t> indexes(static_cast<size_t>(pixels));
			if (const status result = lzw(compressed, minimum, indexes.data(), indexes.size(), a_work); result != status::ok) {
				return result == status::gif_truncated && !whole ? status::gif_truncated : result;
			}
			// the screen starts transparent; the frame's pixels land where it says, and what lies off the screen is cut
			a_bitmap.width = canvas.width;
			a_bitmap.height = canvas.height;
			a_bitmap.rgba.assign(size_t {canvas.width} * canvas.height * 4, 0);
			const bool interlaced = (packed & 0x40) != 0;
			for (uint32_t row = 0; row < height; ++row) {
				// an interlaced frame's rows come every 8th from 0, every 8th from 4, every 4th from 2, then every 2nd from 1
				uint32_t y = row;
				if (interlaced) {
					const uint32_t first = (height + 7) / 8;
					const uint32_t second = first + (height + 3) / 8;
					const uint32_t third = second + (height + 1) / 4;
					y = row < first ? row * 8 : row < second ? (row - first) * 8 + 4 : row < third ? (row - second) * 4 + 2 : (row - third) * 2 + 1;
				}
				if (a_work.exhausted(width / 4 + 1)) { return status::budget; }
				const uint64_t screen_y = uint64_t {top} + y;
				if (screen_y >= canvas.height) { continue; }
				const uint8_t* in = indexes.data() + size_t {row} * width;
				uint8_t* out = a_bitmap.rgba.data() + size_t(screen_y) * canvas.width * 4;
				for (uint32_t x = 0; x < width; ++x) {
					const uint64_t screen_x = uint64_t {left} + x;
					if (screen_x >= canvas.width) { break; }
					const unsigned index = in[x];
					// the transparent index, and an index past the table, leave the screen transparent there
					if (static_cast<int>(index) == transparent || index >= table_size) { continue; }
					uint8_t* pixel = out + size_t(screen_x) * 4;
					std::memcpy(pixel, table + 3 * size_t {index}, 3);
					pixel[3] = 255;
				}
			}
			return status::ok;
		}
	}

} // namespace gif_detail

	status read_gif_header(const uint8_t* a_data, size_t a_size, const limits& a_limits, header& a_header) {
		byte_reader reader(a_data, a_size);
		gif_detail::screen canvas;
		if (const status result = gif_detail::parse_screen(reader, a_limits, canvas); result != status::ok) { return result; }
		a_header.kind = format::gif;
		a_header.width = canvas.width;
		a_header.height = canvas.height;
		a_header.orientation = 1;
		return status::ok;
	}

	status decode_gif(const uint8_t* a_data, size_t a_size, const limits& a_limits, const work_hook& a_work, header& a_header, bitmap& a_bitmap) {
		try {
			return gif_detail::decode(a_data, a_size, a_limits, a_work, a_header, a_bitmap);
		} catch (const std::bad_alloc&) {
			return status::out_of_memory;
		} catch (const std::length_error&) {
			return status::out_of_memory;
		}
	}

} // namespace image_codec
} // namespace jai

#endif
