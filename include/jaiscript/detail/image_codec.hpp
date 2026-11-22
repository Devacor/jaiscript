#pragma once

#ifndef __JAISCRIPT_DETAIL_IMAGE_CODEC_HPP__
#define __JAISCRIPT_DETAIL_IMAGE_CODEC_HPP__

#include <cstddef>
#include <cstdint>
#include <vector>

// The image:: module's codecs (source/implementation/stdlib/image_*.cpp), free of the engine: decoders for PNG, JPEG and
// GIF, encoders for PNG and JPEG, and the resampler, all over 8-bit RGBA. Every function reads untrusted bytes: a
// malformed file is a status, never a throw, a crash or an uninitialized pixel.

namespace jai {
namespace image_codec {

	inline constexpr uint64_t k_default_max_pixels = 25'000'000;
	// what a host's limit is held to: four bytes a pixel stay inside a 32-bit size
	inline constexpr uint64_t k_hard_max_pixels = uint64_t {1} << 28;
	inline constexpr uint64_t k_default_max_input_bytes = uint64_t {64} << 20;

	enum class format : uint8_t { none, jpeg, png, gif };

	enum class status : uint8_t {
		ok,
		budget,
		out_of_memory,
		not_an_image,
		input_too_large,
		too_many_pixels,
		png_truncated,
		png_bad_crc,
		png_bad_header,
		png_bad_chunk_order,
		png_unknown_critical_chunk,
		png_bad_palette,
		png_bad_compressed_data,
		png_size_mismatch,
		png_bad_filter,
		jpeg_truncated,
		jpeg_bad_segment,
		jpeg_bad_frame,
		jpeg_arithmetic,
		jpeg_unsupported,
		jpeg_cmyk,
		jpeg_too_large,
		jpeg_bad_quantization_table,
		jpeg_bad_huffman_table,
		jpeg_missing_table,
		jpeg_bad_scan,
		jpeg_bad_data,
		jpeg_bad_restart,
		jpeg_no_image,
		gif_truncated,
		gif_bad_header,
		gif_no_image,
		gif_bad_frame,
		gif_bad_data,
		status_count
	};

	// The catalogued text of a failure ("" for ok and budget, which the engine words)
	const char* status_text(status a_status);
	// "jpeg", "png" or "gif"
	const char* format_name(format a_format);

	struct limits {
		uint64_t max_pixels = k_default_max_pixels;
		uint64_t max_input_bytes = k_default_max_input_bytes;
	};

	// The pixel count a decoder allows: the host's limit, held to k_hard_max_pixels
	inline uint64_t pixel_limit(const limits& a_limits) { return a_limits.max_pixels < k_hard_max_pixels ? a_limits.max_pixels : k_hard_max_pixels; }

	// How a long call spends the engine's work budget: spend answers true once the budget is gone
	struct work_hook {
		bool (*spend)(void* a_context, size_t a_units) = nullptr;
		void* context = nullptr;
		bool exhausted(size_t a_units) const { return spend && spend(context, a_units); }
	};

	// What a file's header says: the stored width and height, and its EXIF orientation (1 to 8; 1 where none is recorded)
	struct header {
		format kind = format::none;
		uint32_t width = 0;
		uint32_t height = 0;
		uint8_t orientation = 1;
		// The size after the orientation is applied: 5 to 8 turn the image on its side
		uint32_t oriented_width() const { return orientation >= 5 ? height : width; }
		uint32_t oriented_height() const { return orientation >= 5 ? width : height; }
	};

	// 8-bit RGBA rows, top to bottom, alpha not premultiplied; rgba.size() is always width * height * 4
	struct bitmap {
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> rgba;
	};

	// A bounds-checked cursor over the input: every read answers false past the end and moves nothing
	class byte_reader {
	public:
		byte_reader(const uint8_t* a_data, size_t a_size) : data_(a_data), size_(a_size) {}
		size_t position() const { return at_; }
		size_t remaining() const { return size_ - at_; }
		bool at_end() const { return at_ == size_; }
		bool skip(size_t a_count) {
			if (a_count > remaining()) { return false; }
			at_ += a_count;
			return true;
		}
		bool u8(uint8_t& a_value) {
			if (remaining() < 1) { return false; }
			a_value = data_[at_++];
			return true;
		}
		bool u16_be(uint16_t& a_value) {
			if (remaining() < 2) { return false; }
			a_value = static_cast<uint16_t>((data_[at_] << 8) | data_[at_ + 1]);
			at_ += 2;
			return true;
		}
		bool u16_le(uint16_t& a_value) {
			if (remaining() < 2) { return false; }
			a_value = static_cast<uint16_t>(data_[at_] | (data_[at_ + 1] << 8));
			at_ += 2;
			return true;
		}
		bool u32_be(uint32_t& a_value) {
			if (remaining() < 4) { return false; }
			a_value = (uint32_t {data_[at_]} << 24) | (uint32_t {data_[at_ + 1]} << 16) | (uint32_t {data_[at_ + 2]} << 8) | uint32_t {data_[at_ + 3]};
			at_ += 4;
			return true;
		}
		// The next a_count bytes, or null when fewer remain
		const uint8_t* take(size_t a_count) {
			if (a_count > remaining()) { return nullptr; }
			const uint8_t* result = data_ + at_;
			at_ += a_count;
			return result;
		}
		const uint8_t* peek() const { return data_ + at_; }

	private:
		const uint8_t* data_;
		size_t size_;
		size_t at_ = 0;
	};

	// The format a_data's first bytes name: JPEG FF D8 FF, the 8-byte PNG signature, GIF87a or GIF89a
	format sniff(const uint8_t* a_data, size_t a_size);

	// The header alone: no pixel is decoded and nothing is allocated for one
	status read_header(const uint8_t* a_data, size_t a_size, const limits& a_limits, header& a_header);

	// The whole image as stored (its orientation not applied)
	status decode(const uint8_t* a_data, size_t a_size, const limits& a_limits, const work_hook& a_work, header& a_header, bitmap& a_bitmap);

	// a_source turned as EXIF orientation a_orientation (1 to 8) says
	status orient(const bitmap& a_source, uint8_t a_orientation, bitmap& a_result);

	// a_source resampled to a_width x a_height (each at least 1, their product within k_hard_max_pixels): Catmull-Rom over
	// premultiplied alpha, its kernel stretched by the scale for a reduction, in fixed point so every platform gives the same
	// bytes; the identity size is an exact copy
	status resize(const bitmap& a_source, uint32_t a_width, uint32_t a_height, const work_hook& a_work, bitmap& a_result);

	// The rectangle, which the caller has checked lies inside a_source
	status crop(const bitmap& a_source, uint32_t a_x, uint32_t a_y, uint32_t a_width, uint32_t a_height, bitmap& a_result);

	// The size a_width x a_height takes scaled to fit inside the box with its shape kept, never larger than it is
	void fit_size(uint32_t a_width, uint32_t a_height, uint64_t a_box_width, uint64_t a_box_height, uint32_t& a_fit_width, uint32_t& a_fit_height);

	// a_source scaled to cover a_width x a_height and centre-cropped to exactly that, enlarged when it is smaller
	status crop_fill(const bitmap& a_source, uint32_t a_width, uint32_t a_height, const work_hook& a_work, bitmap& a_result);

	// 8-bit PNG, greyscale when every pixel is grey and RGB when every pixel is opaque (with alpha otherwise), each row under the
	// filter whose bytes sum smallest, deflated; no chunk but IHDR, IDAT and IEND, so no metadata
	status encode_png(const bitmap& a_bitmap, const work_hook& a_work, std::vector<uint8_t>& a_bytes);
	// Baseline JPEG at a_quality 1 to 100 (the Annex K tables scaled as the reference encoder scales them), Huffman tables built
	// for the image, chroma 4:2:0 below quality 90 and 4:4:4 from it, one component when every pixel is grey, alpha composited
	// over white, and no segment but JFIF's: no metadata is ever written
	status encode_jpeg(const bitmap& a_bitmap, int a_quality, const work_hook& a_work, std::vector<uint8_t>& a_bytes);

	// Huffman code lengths for a_count (at most 288) symbol frequencies, none longer than a_longest, by T.81 K.2's procedure; a
	// symbol of frequency 0 gets 0, a lone symbol 1. With a_reserve one more code point is held back, so no code is all ones.
	void code_lengths(const uint32_t* a_frequency, unsigned a_count, unsigned a_longest, bool a_reserve, uint8_t* a_lengths);

	status read_png_header(const uint8_t* a_data, size_t a_size, const limits& a_limits, header& a_header);
	status decode_png(const uint8_t* a_data, size_t a_size, const limits& a_limits, const work_hook& a_work, header& a_header, bitmap& a_bitmap);
	status read_jpeg_header(const uint8_t* a_data, size_t a_size, const limits& a_limits, header& a_header);
	status decode_jpeg(const uint8_t* a_data, size_t a_size, const limits& a_limits, const work_hook& a_work, header& a_header, bitmap& a_bitmap);
	status read_gif_header(const uint8_t* a_data, size_t a_size, const limits& a_limits, header& a_header);
	status decode_gif(const uint8_t* a_data, size_t a_size, const limits& a_limits, const work_hook& a_work, header& a_header, bitmap& a_bitmap);

	// The pieces under PNG, named for their tests
	uint32_t crc32(const uint8_t* a_data, size_t a_size, uint32_t a_crc = 0);
	uint32_t adler32(const uint8_t* a_data, size_t a_size, uint32_t a_adler = 1);

	enum class inflate_status : uint8_t {
		ok,
		budget,
		truncated,
		bad_stream_header,
		bad_block_type,
		bad_stored_length,
		bad_code_lengths,
		bad_symbol,
		distance_too_far,
		too_much_output,
		too_little_output,
		bad_checksum
	};
	// An RFC 1950 stream inflated into exactly a_out_size bytes: more or fewer is a failure
	inflate_status inflate_zlib(const uint8_t* a_data, size_t a_size, uint8_t* a_out, size_t a_out_size, const work_hook& a_work);
	// a_data as an RFC 1950 stream: LZ77 over hash chains with lazy matching, in dynamic Huffman blocks or stored ones where
	// those are smaller; false when the budget is gone
	bool deflate_zlib(const uint8_t* a_data, size_t a_size, const work_hook& a_work, std::vector<uint8_t>& a_out);

} // namespace image_codec
} // namespace jai

#endif
