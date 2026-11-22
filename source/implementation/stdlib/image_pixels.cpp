#ifdef JAISCRIPT_ENABLE_IMAGE

#include <jaiscript/detail/image_codec.hpp>
#include <algorithm>
#include <cstring>
#include <new>
#include <stdexcept>

namespace jai {
namespace image_codec {

	const char* status_text(status a_status) {
		switch (a_status) {
			case status::out_of_memory: return "Image needs more memory than is available";
			case status::not_an_image: return "Image data is not a JPEG, PNG or GIF (read a file with image::load(file::read(path)))";
			case status::input_too_large: return "Image input is larger than the input size limit";
			case status::too_many_pixels: return "Image has more pixels than the pixel limit";
			case status::png_truncated: return "PNG is truncated";
			case status::png_bad_crc: return "PNG chunk has a bad CRC";
			case status::png_bad_header: return "PNG header is invalid";
			case status::png_bad_chunk_order: return "PNG chunks are missing or out of order";
			case status::png_unknown_critical_chunk: return "PNG has a critical chunk this decoder does not know";
			case status::png_bad_palette: return "PNG palette is invalid";
			case status::png_bad_compressed_data: return "PNG compressed data is invalid";
			case status::png_size_mismatch: return "PNG image data does not match its header";
			case status::png_bad_filter: return "PNG row has an invalid filter type";
			case status::jpeg_truncated: return "JPEG is truncated";
			case status::jpeg_bad_segment: return "JPEG marker segment is invalid";
			case status::jpeg_bad_frame: return "JPEG frame header is invalid";
			case status::jpeg_arithmetic: return "JPEG arithmetic coding is not supported";
			case status::jpeg_unsupported: return "JPEG coding process is not supported (only 8-bit baseline and progressive Huffman are)";
			case status::jpeg_cmyk: return "JPEG CMYK images are not supported";
			case status::jpeg_too_large: return "JPEG cannot hold an image wider or taller than 65535 pixels";
			case status::jpeg_bad_quantization_table: return "JPEG quantization table is invalid";
			case status::jpeg_bad_huffman_table: return "JPEG Huffman table is invalid";
			case status::jpeg_missing_table: return "JPEG scan uses a table that was never defined";
			case status::jpeg_bad_scan: return "JPEG scan header is invalid";
			case status::jpeg_bad_data: return "JPEG image data is invalid";
			case status::jpeg_bad_restart: return "JPEG restart markers are out of order";
			case status::jpeg_no_image: return "JPEG has no image";
			case status::gif_truncated: return "GIF is truncated";
			case status::gif_bad_header: return "GIF header is invalid";
			case status::gif_no_image: return "GIF has no image";
			case status::gif_bad_frame: return "GIF frame is invalid";
			case status::gif_bad_data: return "GIF image data is invalid";
			default: return "";
		}
	}

	const char* format_name(format a_format) {
		switch (a_format) {
			case format::jpeg: return "jpeg";
			case format::png: return "png";
			case format::gif: return "gif";
			default: return "";
		}
	}

	format sniff(const uint8_t* a_data, size_t a_size) {
		static constexpr uint8_t k_png[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
		if (a_size >= 3 && a_data[0] == 0xff && a_data[1] == 0xd8 && a_data[2] == 0xff) { return format::jpeg; }
		if (a_size >= 8 && std::memcmp(a_data, k_png, 8) == 0) { return format::png; }
		if (a_size >= 6 && (std::memcmp(a_data, "GIF87a", 6) == 0 || std::memcmp(a_data, "GIF89a", 6) == 0)) { return format::gif; }
		return format::none;
	}

	status read_header(const uint8_t* a_data, size_t a_size, const limits& a_limits, header& a_header) {
		if (a_size > a_limits.max_input_bytes) { return status::input_too_large; }
		switch (sniff(a_data, a_size)) {
			case format::png: return read_png_header(a_data, a_size, a_limits, a_header);
			case format::jpeg: return read_jpeg_header(a_data, a_size, a_limits, a_header);
			case format::gif: return read_gif_header(a_data, a_size, a_limits, a_header);
			default: return status::not_an_image;
		}
	}

	status decode(const uint8_t* a_data, size_t a_size, const limits& a_limits, const work_hook& a_work, header& a_header, bitmap& a_bitmap) {
		if (a_size > a_limits.max_input_bytes) { return status::input_too_large; }
		switch (sniff(a_data, a_size)) {
			case format::png: return decode_png(a_data, a_size, a_limits, a_work, a_header, a_bitmap);
			case format::jpeg: return decode_jpeg(a_data, a_size, a_limits, a_work, a_header, a_bitmap);
			case format::gif: return decode_gif(a_data, a_size, a_limits, a_work, a_header, a_bitmap);
			default: return status::not_an_image;
		}
	}

namespace resample_detail {

	inline constexpr int k_weight_bits = 14;
	inline constexpr int32_t k_weight_one = int32_t {1} << k_weight_bits;
	// a premultiplied sample: an 8-bit colour times an 8-bit alpha, or an alpha times 255
	inline constexpr int32_t k_sample_max = 255 * 255;

	// Catmull-Rom (the cubic convolution kernel with a = -0.5): interpolating, so a sample at a source pixel's centre is that pixel
	inline double cubic(double a_x) {
		const double x = a_x < 0 ? -a_x : a_x;
		if (x < 1) { return (1.5 * x - 2.5) * x * x + 1; }
		if (x < 2) { return ((-0.5 * x + 2.5) * x - 4) * x + 2; }
		return 0;
	}

	// Where each output pixel of one axis reads: its first source pixel, how many, and their weights (summing exactly to
	// k_weight_one, so a flat region stays flat)
	struct axis_taps {
		std::vector<uint32_t> first;
		std::vector<uint32_t> count;
		std::vector<size_t> offset;
		std::vector<int32_t> weights;
	};

	// a_in source pixels to a_out: for a reduction the kernel is stretched by the scale, so every source pixel counts
	void make_taps(uint32_t a_in, uint32_t a_out, axis_taps& a_taps) {
		const double scale = static_cast<double>(a_in) / a_out;
		const double stretch = scale > 1 ? scale : 1;
		const double support = 2 * stretch;
		a_taps.first.resize(a_out);
		a_taps.count.resize(a_out);
		a_taps.offset.resize(a_out);
		a_taps.weights.clear();
		std::vector<double> raw;
		for (uint32_t i = 0; i < a_out; ++i) {
			const double centre = (i + 0.5) * scale;
			const double low = centre - support + 0.5;
			const double high = centre + support + 0.5;
			const uint32_t first = low <= 0 ? 0u : static_cast<uint32_t>(low);
			const uint32_t end = high >= a_in ? a_in : static_cast<uint32_t>(high);
			raw.clear();
			double sum = 0;
			for (uint32_t at = first; at < end; ++at) {
				const double weight = cubic((at + 0.5 - centre) / stretch);
				raw.push_back(weight);
				sum += weight;
			}
			a_taps.first[i] = first;
			a_taps.count[i] = end - first;
			a_taps.offset[i] = a_taps.weights.size();
			int32_t total = 0;
			size_t largest = 0;
			for (size_t k = 0; k < raw.size(); ++k) {
				const double scaled = raw[k] / sum * k_weight_one;
				const int32_t weight = static_cast<int32_t>(scaled < 0 ? scaled - 0.5 : scaled + 0.5);
				a_taps.weights.push_back(weight);
				total += weight;
				if (raw[k] > raw[largest]) { largest = k; }
			}
			// the rounding's remainder goes to the heaviest tap
			a_taps.weights[a_taps.offset[i] + largest] += k_weight_one - total;
		}
	}

	inline int32_t clamp_sample(int64_t a_sum) {
		const int64_t value = (a_sum + (int64_t {1} << (k_weight_bits - 1))) >> k_weight_bits;
		return value < 0 ? 0 : value > k_sample_max ? k_sample_max : static_cast<int32_t>(value);
	}

} // namespace resample_detail

	void code_lengths(const uint32_t* a_frequency, unsigned a_count, unsigned a_longest, bool a_reserve, uint8_t* a_lengths) {
		// the reserved code point is one more symbol, of the least frequency, which sorts last and is dropped at the end
		const unsigned symbols = a_count + (a_reserve ? 1 : 0);
		std::vector<uint64_t> frequency(symbols);
		std::vector<unsigned> size(symbols, 0);
		std::vector<int> next(symbols, -1);
		unsigned used = 0;
		for (unsigned i = 0; i < a_count; ++i) {
			frequency[i] = a_frequency[i];
			used += a_frequency[i] ? 1 : 0;
		}
		if (a_reserve) { frequency[a_count] = 1; }
		for (unsigned i = 0; i < a_count; ++i) { a_lengths[i] = 0; }
		if (used == 0) { return; }
		if (used == 1 && !a_reserve) {
			for (unsigned i = 0; i < a_count; ++i) {
				if (a_frequency[i]) { a_lengths[i] = 1; }
			}
			return;
		}
		// T.81 K.2: merge the two least frequent (the later symbol on a tie), each member of a merged set one bit deeper
		for (;;) {
			int first = -1;
			for (unsigned i = 0; i < symbols; ++i) {
				if (frequency[i] && (first < 0 || frequency[i] <= frequency[static_cast<size_t>(first)])) { first = static_cast<int>(i); }
			}
			int second = -1;
			for (unsigned i = 0; i < symbols; ++i) {
				if (frequency[i] && static_cast<int>(i) != first && (second < 0 || frequency[i] <= frequency[static_cast<size_t>(second)])) { second = static_cast<int>(i); }
			}
			if (second < 0) { break; }
			frequency[static_cast<size_t>(first)] += frequency[static_cast<size_t>(second)];
			frequency[static_cast<size_t>(second)] = 0;
			for (int at = first;; at = next[static_cast<size_t>(at)]) {
				++size[static_cast<size_t>(at)];
				if (next[static_cast<size_t>(at)] < 0) {
					next[static_cast<size_t>(at)] = second;
					break;
				}
			}
			for (int at = second; at >= 0; at = next[static_cast<size_t>(at)]) { ++size[static_cast<size_t>(at)]; }
		}
		// the count of codes per length, then the longest moved up the tree two at a time until none is past a_longest
		std::vector<unsigned> counts(symbols + 2, 0);
		unsigned deepest = 0;
		for (unsigned i = 0; i < symbols; ++i) {
			if (size[i]) {
				++counts[size[i]];
				deepest = size[i] > deepest ? size[i] : deepest;
			}
		}
		for (unsigned i = deepest; i > a_longest; --i) {
			while (counts[i] > 0) {
				unsigned j = i - 2;
				while (counts[j] == 0) { --j; }
				counts[i] -= 2;
				++counts[i - 1];
				counts[j + 1] += 2;
				--counts[j];
			}
		}
		if (a_reserve) {
			unsigned longest = a_longest;
			while (counts[longest] == 0) { --longest; }
			--counts[longest];
		}
		// the lengths handed out in order of the merged depth, a tie by symbol
		unsigned length = 1;
		for (unsigned depth = 1; depth <= deepest; ++depth) {
			for (unsigned i = 0; i < a_count; ++i) {
				if (size[i] != depth) { continue; }
				while (length <= a_longest && counts[length] == 0) { ++length; }
				a_lengths[i] = static_cast<uint8_t>(length);
				--counts[length];
			}
		}
	}

	status orient(const bitmap& a_source, uint8_t a_orientation, bitmap& a_result) {
		try {
			const uint32_t width = a_source.width;
			const uint32_t height = a_source.height;
			const bool turned = a_orientation >= 5 && a_orientation <= 8;
			a_result.width = turned ? height : width;
			a_result.height = turned ? width : height;
			a_result.rgba.assign(a_source.rgba.size(), 0);
			const uint8_t* in = a_source.rgba.data();
			uint8_t* out = a_result.rgba.data();
			for (uint32_t y = 0; y < a_result.height; ++y) {
				for (uint32_t x = 0; x < a_result.width; ++x) {
					// the stored pixel the displayed pixel (x, y) shows
					uint32_t from_x = x;
					uint32_t from_y = y;
					switch (a_orientation) {
						case 2: from_x = width - 1 - x; break;
						case 3: from_x = width - 1 - x; from_y = height - 1 - y; break;
						case 4: from_y = height - 1 - y; break;
						case 5: from_x = y; from_y = x; break;
						case 6: from_x = y; from_y = height - 1 - x; break;
						case 7: from_x = width - 1 - y; from_y = height - 1 - x; break;
						case 8: from_x = width - 1 - y; from_y = x; break;
						default: break;
					}
					std::memcpy(out + (size_t {y} * a_result.width + x) * 4, in + (size_t {from_y} * width + from_x) * 4, 4);
				}
			}
			return status::ok;
		} catch (const std::bad_alloc&) {
			return status::out_of_memory;
		}
	}

	status crop(const bitmap& a_source, uint32_t a_x, uint32_t a_y, uint32_t a_width, uint32_t a_height, bitmap& a_result) {
		try {
			a_result.width = a_width;
			a_result.height = a_height;
			a_result.rgba.resize(size_t {a_width} * a_height * 4);
			for (uint32_t row = 0; row < a_height; ++row) {
				std::memcpy(a_result.rgba.data() + size_t {row} * a_width * 4, a_source.rgba.data() + ((size_t {a_y} + row) * a_source.width + a_x) * 4, size_t {a_width} * 4);
			}
			return status::ok;
		} catch (const std::bad_alloc&) {
			return status::out_of_memory;
		}
	}

	status resize(const bitmap& a_source, uint32_t a_width, uint32_t a_height, const work_hook& a_work, bitmap& a_result) {
		using namespace resample_detail;
		try {
			if (a_width == a_source.width && a_height == a_source.height) {
				a_result = a_source;
				return status::ok;
			}
			const uint32_t in_width = a_source.width;
			const uint32_t in_height = a_source.height;
			axis_taps across;
			axis_taps down;
			make_taps(in_width, a_width, across);
			make_taps(in_height, a_height, down);
			// the horizontal pass first, over every source row, into premultiplied samples a_width wide
			std::vector<uint16_t> middle(size_t {a_width} * in_height * 4);
			std::vector<int32_t> row(size_t {in_width} * 4);
			const size_t across_cost = across.weights.size() / 4 + in_width / 16 + 1;
			for (uint32_t y = 0; y < in_height; ++y) {
				const uint8_t* in = a_source.rgba.data() + size_t {y} * in_width * 4;
				for (size_t x = 0; x < in_width; ++x) {
					const int32_t alpha = in[4 * x + 3];
					row[4 * x] = in[4 * x] * alpha;
					row[4 * x + 1] = in[4 * x + 1] * alpha;
					row[4 * x + 2] = in[4 * x + 2] * alpha;
					row[4 * x + 3] = alpha * 255;
				}
				uint16_t* out = middle.data() + size_t {y} * a_width * 4;
				for (uint32_t x = 0; x < a_width; ++x) {
					const int32_t* weights = across.weights.data() + across.offset[x];
					const int32_t* from = row.data() + size_t {across.first[x]} * 4;
					int64_t sum[4] = {0, 0, 0, 0};
					for (uint32_t k = 0; k < across.count[x]; ++k, from += 4) {
						for (int channel = 0; channel < 4; ++channel) { sum[channel] += int64_t {weights[k]} * from[channel]; }
					}
					for (int channel = 0; channel < 4; ++channel) { out[4 * x + channel] = static_cast<uint16_t>(clamp_sample(sum[channel])); }
				}
				if (a_work.exhausted(across_cost)) { return status::budget; }
			}
			// then down the columns, back to straight 8-bit RGBA
			a_result.width = a_width;
			a_result.height = a_height;
			a_result.rgba.assign(size_t {a_width} * a_height * 4, 0);
			const size_t row_samples = size_t {a_width} * 4;
			std::vector<int64_t> sums(row_samples);
			for (uint32_t y = 0; y < a_height; ++y) {
				std::fill(sums.begin(), sums.end(), int64_t {0});
				const int32_t* weights = down.weights.data() + down.offset[y];
				for (uint32_t k = 0; k < down.count[y]; ++k) {
					const uint16_t* from = middle.data() + (size_t {down.first[y]} + k) * row_samples;
					const int64_t weight = weights[k];
					for (size_t i = 0; i < row_samples; ++i) { sums[i] += weight * from[i]; }
				}
				uint8_t* out = a_result.rgba.data() + size_t {y} * row_samples;
				for (uint32_t x = 0; x < a_width; ++x) {
					const int32_t alpha = clamp_sample(sums[4 * x + 3]);
					const int32_t alpha_8 = (alpha + 127) / 255;
					if (alpha_8 == 0) { continue; }
					for (int channel = 0; channel < 3; ++channel) {
						int32_t colour = clamp_sample(sums[4 * x + channel]);
						// a negative lobe can leave a colour above its alpha, which no premultiplied colour is
						if (colour > alpha) { colour = alpha; }
						const int32_t straight = static_cast<int32_t>((int64_t {colour} * 255 + alpha / 2) / alpha);
						out[4 * x + channel] = static_cast<uint8_t>(straight > 255 ? 255 : straight);
					}
					out[4 * x + 3] = static_cast<uint8_t>(alpha_8);
				}
				if (a_work.exhausted(down.count[y] * row_samples / 16 + 1)) { return status::budget; }
			}
			return status::ok;
		} catch (const std::bad_alloc&) {
			return status::out_of_memory;
		} catch (const std::length_error&) {
			return status::out_of_memory;
		}
	}

	void fit_size(uint32_t a_width, uint32_t a_height, uint64_t a_box_width, uint64_t a_box_height, uint32_t& a_fit_width, uint32_t& a_fit_height) {
		a_fit_width = a_width;
		a_fit_height = a_height;
		if (a_width <= a_box_width && a_height <= a_box_height) { return; }
		// a box side past 2^32 changes nothing here, and keeps the products below inside 64 bits
		const uint64_t box_width = a_box_width < (uint64_t {1} << 32) ? a_box_width : uint64_t {1} << 32;
		const uint64_t box_height = a_box_height < (uint64_t {1} << 32) ? a_box_height : uint64_t {1} << 32;
		if (uint64_t {a_width} * box_height >= uint64_t {a_height} * box_width) {
			a_fit_width = static_cast<uint32_t>(box_width);
			const uint64_t height = (uint64_t {a_height} * box_width + a_width / 2) / a_width;
			a_fit_height = static_cast<uint32_t>(height < 1 ? 1 : height);
		} else {
			a_fit_height = static_cast<uint32_t>(box_height);
			const uint64_t width = (uint64_t {a_width} * box_height + a_height / 2) / a_height;
			a_fit_width = static_cast<uint32_t>(width < 1 ? 1 : width);
		}
	}

	status crop_fill(const bitmap& a_source, uint32_t a_width, uint32_t a_height, const work_hook& a_work, bitmap& a_result) {
		// the centred part of the source with the target's shape, then that part resampled to the target
		uint64_t part_width = a_source.width;
		uint64_t part_height = a_source.height;
		if (uint64_t {a_source.width} * a_height > uint64_t {a_source.height} * a_width) {
			part_width = (uint64_t {a_source.height} * a_width + a_height / 2) / a_height;
		} else {
			part_height = (uint64_t {a_source.width} * a_height + a_width / 2) / a_width;
		}
		part_width = part_width < 1 ? 1 : part_width > a_source.width ? a_source.width : part_width;
		part_height = part_height < 1 ? 1 : part_height > a_source.height ? a_source.height : part_height;
		if (part_width == a_source.width && part_height == a_source.height) { return resize(a_source, a_width, a_height, a_work, a_result); }
		bitmap part;
		const uint32_t x = static_cast<uint32_t>((a_source.width - part_width) / 2);
		const uint32_t y = static_cast<uint32_t>((a_source.height - part_height) / 2);
		if (const status result = crop(a_source, x, y, static_cast<uint32_t>(part_width), static_cast<uint32_t>(part_height), part); result != status::ok) { return result; }
		return resize(part, a_width, a_height, a_work, a_result);
	}

} // namespace image_codec
} // namespace jai

#endif
