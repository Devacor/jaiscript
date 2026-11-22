#ifdef JAISCRIPT_ENABLE_IMAGE

#include <jaiscript/detail/image_codec.hpp>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>

// JPEG (ITU-T T.81) with JFIF, the Adobe APP14 transform flag and EXIF orientation, written from those specifications:
// baseline, extended and progressive Huffman decoding of 8-bit samples, and a baseline encoder.

namespace jai {
namespace image_codec {
namespace jpeg_detail {

	// The natural (row-major) index of each zig-zag position
	inline constexpr uint8_t k_zigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,
	                                         7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31,
	                                         39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

	// round(2^13 * C(u) / 2 * cos((2x + 1) u pi / 16)) for sample x (row) and frequency u (column): the T.81 inverse DCT's terms
	inline constexpr int32_t k_cosine[8][8] = {
		{2896, 4017, 3784, 3406, 2896, 2276, 1567, 799},
		{2896, 3406, 1567, -799, -2896, -4017, -3784, -2276},
		{2896, 2276, -1567, -4017, -2896, 799, 3784, 3406},
		{2896, 799, -3784, -2276, 2896, 3406, -1567, -4017},
		{2896, -799, -3784, 2276, 2896, -3406, -1567, 4017},
		{2896, -2276, -1567, 4017, -2896, -799, 3784, -3406},
		{2896, -3406, 1567, 799, -2896, 4017, -3784, 2276},
		{2896, -4017, 3784, -3406, 2896, -2276, 1567, -799},
	};

	inline uint32_t read_16(const uint8_t* a_bytes) { return (uint32_t {a_bytes[0]} << 8) | a_bytes[1]; }

	// The orientation (1 to 8) an EXIF block (an APP1 payload after "Exif\0\0": a TIFF header and IFD0) records; 1 when it records none
	uint8_t exif_orientation(const uint8_t* a_tiff, size_t a_size) {
		if (a_size < 8) { return 1; }
		bool little = false;
		if (a_tiff[0] == 'I' && a_tiff[1] == 'I') {
			little = true;
		} else if (a_tiff[0] != 'M' || a_tiff[1] != 'M') {
			return 1;
		}
		auto u16 = [&](size_t a_at) { return little ? uint32_t {a_tiff[a_at]} | (uint32_t {a_tiff[a_at + 1]} << 8) : read_16(a_tiff + a_at); };
		auto u32 = [&](size_t a_at) { return little ? u16(a_at) | (u16(a_at + 2) << 16) : (u16(a_at) << 16) | u16(a_at + 2); };
		if (u16(2) != 42) { return 1; }
		const uint32_t directory = u32(4);
		if (directory < 8 || directory > a_size - 2) { return 1; }
		const uint32_t entries = u16(directory);
		for (uint32_t i = 0; i < entries; ++i) {
			const uint64_t entry = uint64_t {directory} + 2 + uint64_t {12} * i;
			if (entry + 12 > a_size) { return 1; }
			if (u16(static_cast<size_t>(entry)) != 0x0112) { continue; }
			// a SHORT, one of it, held in the entry itself
			if (u16(static_cast<size_t>(entry) + 2) != 3 || u32(static_cast<size_t>(entry) + 4) != 1) { return 1; }
			const uint32_t value = u16(static_cast<size_t>(entry) + 8);
			return value >= 1 && value <= 8 ? static_cast<uint8_t>(value) : 1;
		}
		return 1;
	}

	struct huffman_table {
		static constexpr unsigned k_fast_bits = 9;
		bool defined = false;
		unsigned total = 0;
		uint8_t values[256] = {};
		// by the next 9 bits: the length of the code they start (0: a longer code, or none) and its value
		uint8_t fast_length[1u << k_fast_bits] = {};
		uint8_t fast_value[1u << k_fast_bits] = {};
		// per length: the last code of that length (-1: none), and the index of its first value minus its first code
		int32_t last_code[17] = {};
		int32_t offset[17] = {};

		// False for an over-subscribed set of lengths (an incomplete one is allowed, as T.81 allows it)
		bool build(const uint8_t (&a_counts)[16], const uint8_t* a_values) {
			total = 0;
			for (unsigned i = 0; i < 16; ++i) { total += a_counts[i]; }
			if (total > 256) { return false; }
			std::memcpy(values, a_values, total);
			std::memset(fast_length, 0, sizeof(fast_length));
			uint32_t code = 0;
			unsigned index = 0;
			for (unsigned length = 1; length <= 16; ++length) {
				const unsigned count = a_counts[length - 1];
				// checked before a code of this length is placed, since the fast table is indexed by them
				if (code + count > (1u << length)) { return false; }
				offset[length] = static_cast<int32_t>(index) - static_cast<int32_t>(code);
				for (unsigned i = 0; i < count; ++i, ++code, ++index) {
					if (length <= k_fast_bits) {
						const uint32_t first = code << (k_fast_bits - length);
						for (uint32_t slot = 0; slot < (1u << (k_fast_bits - length)); ++slot) {
							fast_length[first + slot] = static_cast<uint8_t>(length);
							fast_value[first + slot] = values[index];
						}
					}
				}
				last_code[length] = count ? static_cast<int32_t>(code) - 1 : -1;
				code <<= 1;
			}
			defined = true;
			return true;
		}
	};

	// The entropy-coded bytes of a scan, most significant bit first, with FF 00 read as FF. A marker ends them: from there the
	// reader supplies zero bits and remembers whether a code consumed any of them, which only a cut or damaged file makes it do.
	struct entropy_reader {
		const uint8_t* data;
		size_t size;
		size_t at;
		uint64_t bits = 0;
		int count = 0;
		int padding = 0;
		bool ended = false;
		bool overrun = false;

		void fill() {
			while (count <= 56) {
				uint8_t byte = 0;
				if (!ended) {
					if (at >= size || (data[at] == 0xff && (at + 1 >= size || data[at + 1] != 0x00))) {
						ended = true;
					} else {
						byte = data[at];
						at += data[at] == 0xff ? 2 : 1;
					}
				}
				if (ended) { padding += 8; }
				bits |= uint64_t {byte} << (56 - count);
				count += 8;
			}
		}
		void consume(int a_bits) {
			bits <<= a_bits;
			count -= a_bits;
			if (count < padding) {
				overrun = true;
				padding = count;
			}
		}
		// a_bits at most 16
		uint32_t take(int a_bits) {
			if (a_bits == 0) { return 0; }
			if (count < a_bits) { fill(); }
			const uint32_t value = static_cast<uint32_t>(bits >> (64 - a_bits));
			consume(a_bits);
			return value;
		}
		// The value T.81 F.2.2.1 EXTEND gives a_bits bits
		int32_t extended(int a_bits) {
			if (a_bits == 0) { return 0; }
			const int32_t value = static_cast<int32_t>(take(a_bits));
			return value < (int32_t {1} << (a_bits - 1)) ? value - (int32_t {1} << a_bits) + 1 : value;
		}
		// The next symbol, or -1 for bits no code spells
		int decode(const huffman_table& a_table) {
			if (count < 16) { fill(); }
			const uint32_t peek = static_cast<uint32_t>(bits >> 48);
			const uint32_t fast = peek >> (16 - huffman_table::k_fast_bits);
			if (const unsigned length = a_table.fast_length[fast]) {
				consume(static_cast<int>(length));
				return a_table.fast_value[fast];
			}
			for (unsigned length = huffman_table::k_fast_bits + 1; length <= 16; ++length) {
				const int32_t code = static_cast<int32_t>(peek >> (16 - length));
				if (code <= a_table.last_code[length]) {
					const int32_t index = code + a_table.offset[length];
					if (index < 0 || index >= static_cast<int32_t>(a_table.total)) { return -1; }
					consume(static_cast<int>(length));
					return a_table.values[index];
				}
			}
			return -1;
		}
		// Past the bits left in the buffer to the marker that follows (skipping stray bytes), whose code this answers (-1: none)
		int next_marker() {
			bits = 0;
			count = 0;
			padding = 0;
			while (at + 1 < size) {
				if (data[at] == 0xff && data[at + 1] != 0x00 && data[at + 1] != 0xff) { return data[at + 1]; }
				++at;
			}
			at = size;
			return -1;
		}
	};

	struct component {
		uint8_t id = 0;
		uint8_t h = 1;
		uint8_t v = 1;
		uint8_t table = 0;
		bool scanned = false;
		// the quantization table as it was when the component's first scan began
		uint16_t quantization[64] = {};
		uint32_t blocks_across = 0;
		uint32_t blocks_down = 0;
		// the blocks a scan of this component alone visits: the samples it holds, rounded up to whole blocks
		uint32_t own_across = 0;
		uint32_t own_down = 0;
		uint32_t samples_across = 0;
		uint32_t samples_down = 0;
		int32_t dc_prediction = 0;
		std::vector<int16_t> coefficients;
		int16_t* block(uint32_t a_column, uint32_t a_row) { return coefficients.data() + (size_t {a_row} * blocks_across + a_column) * 64; }
	};

	struct frame_header {
		bool seen = false;
		bool progressive = false;
		uint32_t width = 0;
		uint32_t height = 0;
		unsigned count = 0;
		component components[3];
		unsigned h_max = 1;
		unsigned v_max = 1;
		uint32_t mcus_across = 0;
		uint32_t mcus_down = 0;
	};

	inline int16_t clamp_16(int32_t a_value) { return static_cast<int16_t>(a_value < -32768 ? -32768 : a_value > 32767 ? 32767 : a_value); }

	class decoder {
	public:
		decoder(const uint8_t* a_data, size_t a_size, const limits& a_limits, const work_hook& a_work) : data_(a_data), size_(a_size), limits_(a_limits), work_(a_work) {}

		// The markers to the frame header (a_header_only) or through the whole file
		status run(bool a_header_only, header& a_header) {
			if (size_ < 3 || data_[0] != 0xff || data_[1] != 0xd8) { return status::not_an_image; }
			size_t at = 2;
			for (;;) {
				// stray bytes between segments are skipped, as decoders in the field skip them
				while (at < size_ && data_[at] != 0xff) { ++at; }
				while (at < size_ && data_[at] == 0xff) { ++at; }
				if (at >= size_) { return finish_without_end(a_header_only, a_header); }
				const uint8_t marker = data_[at++];
				if (marker == 0x00) { continue; }
				if (marker == 0xd9) { return finish(a_header_only, a_header); }
				if (marker == 0xd8) { return status::jpeg_bad_segment; }
				// RSTn outside a scan, and TEM, carry no length
				if ((marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) { continue; }
				if (size_ - at < 2) { return status::jpeg_truncated; }
				const uint32_t length = read_16(data_ + at);
				if (length < 2) { return status::jpeg_bad_segment; }
				if (length > size_ - at) { return status::jpeg_truncated; }
				const uint8_t* body = data_ + at + 2;
				const size_t body_size = length - 2;
				at += length;
				status result = status::ok;
				switch (marker) {
					case 0xc0:
					case 0xc1:
					case 0xc2:
						result = frame(body, body_size, marker == 0xc2);
						if (result == status::ok && a_header_only) { return finish(true, a_header); }
						break;
					case 0xc3: case 0xc5: case 0xc6: case 0xc7: case 0xde: case 0xdf:
						return status::jpeg_unsupported;
					case 0xc9: case 0xca: case 0xcb: case 0xcd: case 0xce: case 0xcf: case 0xcc:
						return status::jpeg_arithmetic;
					case 0xc4:
						if (!a_header_only) { result = huffman_tables(body, body_size); }
						break;
					case 0xdb:
						if (!a_header_only) { result = quantization_tables(body, body_size); }
						break;
					case 0xdd:
						if (body_size != 2) { return status::jpeg_bad_segment; }
						restart_interval_ = read_16(body);
						break;
					case 0xda:
						if (!frame_.seen) { return status::jpeg_bad_segment; }
						result = scan(body, body_size, at);
						break;
					case 0xe1:
						if (!frame_.seen && !orientation_seen_ && body_size >= 6 && std::memcmp(body, "Exif\0\0", 6) == 0) {
							orientation_ = exif_orientation(body + 6, body_size - 6);
							orientation_seen_ = true;
						}
						break;
					case 0xee:
						if (body_size >= 12 && std::memcmp(body, "Adobe", 5) == 0) {
							adobe_seen_ = true;
							adobe_transform_ = body[11];
						}
						break;
					case 0xe0:
						if (body_size >= 5 && std::memcmp(body, "JFIF\0", 5) == 0) { jfif_seen_ = true; }
						break;
					default:
						// APPn, COM, DNL and the reserved markers: skipped
						break;
				}
				if (result != status::ok) { return result; }
			}
		}

		status to_bitmap(bitmap& a_bitmap);

	private:
		status finish(bool a_header_only, header& a_header) {
			if (!frame_.seen) { return status::jpeg_no_image; }
			a_header.kind = format::jpeg;
			a_header.width = frame_.width;
			a_header.height = frame_.height;
			a_header.orientation = orientation_;
			if (a_header_only) { return status::ok; }
			for (unsigned i = 0; i < frame_.count; ++i) {
				if (!frame_.components[i].scanned) { return scans_ == 0 ? status::jpeg_no_image : status::jpeg_bad_data; }
			}
			return status::ok;
		}
		// The file ends with no EOI: whole when every component had a whole scan, as a progressive file cut between scans is
		status finish_without_end(bool a_header_only, header& a_header) {
			if (a_header_only) { return frame_.seen ? finish(true, a_header) : status::jpeg_truncated; }
			if (!frame_.seen || scans_ == 0) { return status::jpeg_truncated; }
			for (unsigned i = 0; i < frame_.count; ++i) {
				if (!frame_.components[i].scanned) { return status::jpeg_truncated; }
			}
			return finish(false, a_header);
		}

		status frame(const uint8_t* a_body, size_t a_size, bool a_progressive) {
			if (frame_.seen) { return status::jpeg_bad_segment; }
			if (a_size < 6) { return status::jpeg_bad_frame; }
			if (a_body[0] != 8) { return status::jpeg_unsupported; }
			frame_.height = read_16(a_body + 1);
			frame_.width = read_16(a_body + 3);
			const unsigned count = a_body[5];
			// a height of 0 defers it to a DNL marker, which no decoder in the field reads
			if (frame_.width == 0 || frame_.height == 0) { return status::jpeg_bad_frame; }
			if (count == 4) { return status::jpeg_cmyk; }
			if (count != 1 && count != 3) { return status::jpeg_unsupported; }
			if (a_size != 6 + 3 * size_t {count}) { return status::jpeg_bad_frame; }
			if (uint64_t {frame_.width} * frame_.height > pixel_limit(limits_)) { return status::too_many_pixels; }
			frame_.count = count;
			frame_.progressive = a_progressive;
			for (unsigned i = 0; i < count; ++i) {
				component& each = frame_.components[i];
				each.id = a_body[6 + 3 * i];
				each.h = static_cast<uint8_t>(a_body[7 + 3 * i] >> 4);
				each.v = static_cast<uint8_t>(a_body[7 + 3 * i] & 15);
				each.table = a_body[8 + 3 * i];
				if (each.h < 1 || each.h > 4 || each.v < 1 || each.v > 4 || each.table > 3) { return status::jpeg_bad_frame; }
				for (unsigned j = 0; j < i; ++j) {
					if (frame_.components[j].id == each.id) { return status::jpeg_bad_frame; }
				}
				frame_.h_max = each.h > frame_.h_max ? each.h : frame_.h_max;
				frame_.v_max = each.v > frame_.v_max ? each.v : frame_.v_max;
			}
			for (unsigned i = 0; i < count; ++i) {
				// a component whose samples do not divide the frame's evenly (3 against 2) has no upsampling this decoder does
				if (frame_.h_max % frame_.components[i].h != 0 || frame_.v_max % frame_.components[i].v != 0) { return status::jpeg_unsupported; }
			}
			frame_.mcus_across = (frame_.width + 8 * frame_.h_max - 1) / (8 * frame_.h_max);
			frame_.mcus_down = (frame_.height + 8 * frame_.v_max - 1) / (8 * frame_.v_max);
			for (unsigned i = 0; i < count; ++i) {
				component& each = frame_.components[i];
				each.blocks_across = frame_.mcus_across * each.h;
				each.blocks_down = frame_.mcus_down * each.v;
				each.samples_across = (frame_.width * each.h + frame_.h_max - 1) / frame_.h_max;
				each.samples_down = (frame_.height * each.v + frame_.v_max - 1) / frame_.v_max;
				each.own_across = (each.samples_across + 7) / 8;
				each.own_down = (each.samples_down + 7) / 8;
			}
			frame_.seen = true;
			return status::ok;
		}

		status allocate() {
			if (allocated_) { return status::ok; }
			for (unsigned i = 0; i < frame_.count; ++i) {
				component& each = frame_.components[i];
				const uint64_t values = uint64_t {each.blocks_across} * each.blocks_down * 64;
				if (values > SIZE_MAX / 2) { return status::out_of_memory; }
				each.coefficients.assign(static_cast<size_t>(values), 0);
			}
			allocated_ = true;
			return status::ok;
		}

		status quantization_tables(const uint8_t* a_body, size_t a_size) {
			size_t at = 0;
			while (at < a_size) {
				const unsigned precision = a_body[at] >> 4;
				const unsigned slot = a_body[at] & 15;
				++at;
				if (precision > 1 || slot > 3) { return status::jpeg_bad_quantization_table; }
				const size_t bytes = precision ? 128 : 64;
				if (a_size - at < bytes) { return status::jpeg_bad_quantization_table; }
				for (unsigned k = 0; k < 64; ++k) {
					quantization_[slot][k_zigzag[k]] = static_cast<uint16_t>(precision ? read_16(a_body + at + 2 * k) : a_body[at + k]);
				}
				quantization_defined_[slot] = true;
				at += bytes;
			}
			return status::ok;
		}

		status huffman_tables(const uint8_t* a_body, size_t a_size) {
			size_t at = 0;
			while (at < a_size) {
				const unsigned kind = a_body[at] >> 4;
				const unsigned slot = a_body[at] & 15;
				++at;
				if (kind > 1 || slot > 3 || a_size - at < 16) { return status::jpeg_bad_huffman_table; }
				uint8_t counts[16];
				std::memcpy(counts, a_body + at, 16);
				at += 16;
				unsigned total = 0;
				for (uint8_t each : counts) { total += each; }
				if (total > 256 || a_size - at < total) { return status::jpeg_bad_huffman_table; }
				huffman_table& table = kind ? ac_[slot] : dc_[slot];
				if (!table.build(counts, a_body + at)) { return status::jpeg_bad_huffman_table; }
				// a DC category past 11 or an AC size past 10 cannot come from 8-bit samples
				for (unsigned i = 0; i < total; ++i) {
					const uint8_t value = a_body[at + i];
					if (kind == 0 ? value > 11 : (value & 15) > 10) { return status::jpeg_bad_huffman_table; }
				}
				at += total;
			}
			return status::ok;
		}

		// One scan: its header, then its entropy-coded data, leaving a_at at the marker after it
		status scan(const uint8_t* a_body, size_t a_size, size_t& a_at) {
			if (a_size < 1) { return status::jpeg_bad_scan; }
			const unsigned count = a_body[0];
			if (count < 1 || count > frame_.count || a_size != 4 + 2 * size_t {count}) { return status::jpeg_bad_scan; }
			unsigned blocks_per_mcu = 0;
			for (unsigned i = 0; i < count; ++i) {
				const uint8_t id = a_body[1 + 2 * i];
				int found = -1;
				for (unsigned c = 0; c < frame_.count; ++c) {
					if (frame_.components[c].id == id) { found = static_cast<int>(c); }
				}
				if (found < 0) { return status::jpeg_bad_scan; }
				for (unsigned j = 0; j < i; ++j) {
					if (scan_components_[j] == static_cast<unsigned>(found)) { return status::jpeg_bad_scan; }
				}
				scan_components_[i] = static_cast<unsigned>(found);
				scan_dc_[i] = a_body[2 + 2 * i] >> 4;
				scan_ac_[i] = a_body[2 + 2 * i] & 15;
				if (scan_dc_[i] > 3 || scan_ac_[i] > 3) { return status::jpeg_bad_scan; }
				blocks_per_mcu += frame_.components[found].h * frame_.components[found].v;
			}
			scan_count_ = count;
			start_ = a_body[1 + 2 * count];
			end_ = a_body[2 + 2 * count];
			high_ = a_body[3 + 2 * count] >> 4;
			low_ = a_body[3 + 2 * count] & 15;
			if (count > 1 && blocks_per_mcu > 10) { return status::jpeg_bad_scan; }
			if (frame_.progressive) {
				if (start_ > end_ || end_ > 63 || (start_ == 0 && end_ != 0) || (start_ > 0 && count != 1) || high_ > 13 || low_ > 13) { return status::jpeg_bad_scan; }
				if (high_ != 0 && high_ != low_ + 1) { return status::jpeg_bad_scan; }
			} else {
				start_ = 0;
				end_ = 63;
				high_ = 0;
				low_ = 0;
			}
			const bool dc_needed = start_ == 0 && high_ == 0;
			const bool ac_needed = end_ > 0;
			for (unsigned i = 0; i < count; ++i) {
				component& each = frame_.components[scan_components_[i]];
				if ((dc_needed && !dc_[scan_dc_[i]].defined) || (ac_needed && !ac_[scan_ac_[i]].defined)) { return status::jpeg_missing_table; }
				if (!each.scanned) {
					if (!quantization_defined_[each.table]) { return status::jpeg_missing_table; }
					std::memcpy(each.quantization, quantization_[each.table], sizeof(each.quantization));
				}
			}
			if (const status ready = allocate(); ready != status::ok) { return ready; }
			entropy_reader reader {data_, size_, a_at};
			const status result = entropy(reader);
			if (result != status::ok) { return result; }
			for (unsigned i = 0; i < count; ++i) { frame_.components[scan_components_[i]].scanned = true; }
			++scans_;
			reader.next_marker();
			a_at = reader.at;
			return status::ok;
		}

		status entropy(entropy_reader& a_reader) {
			const bool single = scan_count_ == 1;
			const component& first = frame_.components[scan_components_[0]];
			const uint32_t across = single ? first.own_across : frame_.mcus_across;
			const uint32_t down = single ? first.own_down : frame_.mcus_down;
			const uint64_t total = uint64_t {across} * down;
			uint32_t expected_restart = 0;
			uint64_t until_restart = restart_interval_;
			eob_run_ = 0;
			for (unsigned i = 0; i < scan_count_; ++i) { frame_.components[scan_components_[i]].dc_prediction = 0; }
			for (uint64_t mcu = 0; mcu < total; ++mcu) {
				if (restart_interval_ != 0 && until_restart == 0) {
					if (a_reader.overrun) { return a_reader.at >= size_ ? status::jpeg_truncated : status::jpeg_bad_data; }
					const int marker = a_reader.next_marker();
					if (marker < 0) { return status::jpeg_truncated; }
					if (marker != static_cast<int>(0xd0 + expected_restart)) { return status::jpeg_bad_restart; }
					a_reader.at += 2;
					a_reader.ended = false;
					a_reader.overrun = false;
					expected_restart = (expected_restart + 1) & 7;
					until_restart = restart_interval_;
					eob_run_ = 0;
					for (unsigned i = 0; i < scan_count_; ++i) { frame_.components[scan_components_[i]].dc_prediction = 0; }
				}
				const uint32_t column = static_cast<uint32_t>(mcu % across);
				const uint32_t row = static_cast<uint32_t>(mcu / across);
				if (single) {
					if (const status result = block(a_reader, 0, scan_components_[0], column, row); result != status::ok) { return result; }
				} else {
					for (unsigned i = 0; i < scan_count_; ++i) {
						const component& each = frame_.components[scan_components_[i]];
						for (unsigned y = 0; y < each.v; ++y) {
							for (unsigned x = 0; x < each.h; ++x) {
								if (const status result = block(a_reader, i, scan_components_[i], column * each.h + x, row * each.v + y); result != status::ok) { return result; }
							}
						}
					}
				}
				--until_restart;
				if (column + 1 == across && work_.exhausted(across / 4 + 1)) { return status::budget; }
			}
			if (a_reader.overrun) { return a_reader.at >= size_ ? status::jpeg_truncated : status::jpeg_bad_data; }
			return status::ok;
		}

		status block(entropy_reader& a_reader, unsigned a_slot, unsigned a_component, uint32_t a_column, uint32_t a_row) {
			component& each = frame_.components[a_component];
			int16_t* coefficients = each.block(a_column, a_row);
			if (!frame_.progressive) { return sequential_block(a_reader, a_slot, each, coefficients); }
			if (start_ == 0) {
				if (high_ == 0) {
					const int category = a_reader.decode(dc_[scan_dc_[a_slot]]);
					if (category < 0) { return status::jpeg_bad_data; }
					each.dc_prediction = clamp_16(each.dc_prediction + a_reader.extended(category));
					coefficients[0] = clamp_16(each.dc_prediction * (int32_t {1} << low_));
				} else if (a_reader.take(1)) {
					coefficients[0] = clamp_16(coefficients[0] | (int32_t {1} << low_));
				}
				return status::ok;
			}
			return high_ == 0 ? ac_first(a_reader, a_slot, coefficients) : ac_refine(a_reader, a_slot, coefficients);
		}

		status sequential_block(entropy_reader& a_reader, unsigned a_slot, component& a_component, int16_t* a_coefficients) {
			const int category = a_reader.decode(dc_[scan_dc_[a_slot]]);
			if (category < 0) { return status::jpeg_bad_data; }
			// a prediction that wanders past what 8-bit samples allow is held there rather than overflowing
			a_component.dc_prediction = clamp_16(a_component.dc_prediction + a_reader.extended(category));
			a_coefficients[0] = static_cast<int16_t>(a_component.dc_prediction);
			const huffman_table& table = ac_[scan_ac_[a_slot]];
			for (unsigned k = 1; k < 64;) {
				const int symbol = a_reader.decode(table);
				if (symbol < 0) { return status::jpeg_bad_data; }
				const unsigned run = static_cast<unsigned>(symbol) >> 4;
				const int size = symbol & 15;
				if (size == 0) {
					if (run != 15) { break; }
					k += 16;
					continue;
				}
				k += run;
				if (k > 63) { return status::jpeg_bad_data; }
				a_coefficients[k_zigzag[k]] = static_cast<int16_t>(a_reader.extended(size));
				++k;
			}
			return status::ok;
		}

		status ac_first(entropy_reader& a_reader, unsigned a_slot, int16_t* a_coefficients) {
			if (eob_run_ > 0) {
				--eob_run_;
				return status::ok;
			}
			const huffman_table& table = ac_[scan_ac_[a_slot]];
			for (unsigned k = start_; k <= end_;) {
				const int symbol = a_reader.decode(table);
				if (symbol < 0) { return status::jpeg_bad_data; }
				const unsigned run = static_cast<unsigned>(symbol) >> 4;
				const int size = symbol & 15;
				if (size == 0) {
					if (run < 15) {
						eob_run_ = (uint32_t {1} << run) - 1 + a_reader.take(static_cast<int>(run));
						break;
					}
					k += 16;
					continue;
				}
				k += run;
				if (k > end_) { return status::jpeg_bad_data; }
				a_coefficients[k_zigzag[k]] = clamp_16(a_reader.extended(size) * (int32_t {1} << low_));
				++k;
			}
			return status::ok;
		}

		// A correction bit for a coefficient a previous scan made nonzero
		void refine(entropy_reader& a_reader, int16_t& a_coefficient) {
			const int32_t bit = int32_t {1} << low_;
			if (a_reader.take(1) && (a_coefficient & bit) == 0) { a_coefficient = clamp_16(a_coefficient >= 0 ? a_coefficient + bit : a_coefficient - bit); }
		}

		status ac_refine(entropy_reader& a_reader, unsigned a_slot, int16_t* a_coefficients) {
			const huffman_table& table = ac_[scan_ac_[a_slot]];
			const int32_t positive = int32_t {1} << low_;
			unsigned k = start_;
			if (eob_run_ == 0) {
				while (k <= end_) {
					const int symbol = a_reader.decode(table);
					if (symbol < 0) { return status::jpeg_bad_data; }
					int run = symbol >> 4;
					const int size = symbol & 15;
					int32_t value = 0;
					if (size != 0) {
						if (size != 1) { return status::jpeg_bad_data; }
						value = a_reader.take(1) ? positive : -positive;
					} else if (run != 15) {
						eob_run_ = (uint32_t {1} << run) + a_reader.take(run);
						break;
					}
					// past run coefficients that are still zero, refining the nonzero ones on the way; the value goes in the next zero one
					while (k <= end_) {
						int16_t& coefficient = a_coefficients[k_zigzag[k]];
						++k;
						if (coefficient != 0) {
							refine(a_reader, coefficient);
						} else if (run == 0) {
							if (value != 0) { coefficient = clamp_16(value); }
							break;
						} else {
							--run;
						}
					}
				}
			}
			if (eob_run_ > 0) {
				for (; k <= end_; ++k) {
					int16_t& coefficient = a_coefficients[k_zigzag[k]];
					if (coefficient != 0) { refine(a_reader, coefficient); }
				}
				--eob_run_;
			}
			return status::ok;
		}

		const uint8_t* data_;
		size_t size_;
		const limits& limits_;
		const work_hook& work_;
		frame_header frame_;
		uint16_t quantization_[4][64] = {};
		bool quantization_defined_[4] = {};
		huffman_table dc_[4];
		huffman_table ac_[4];
		uint32_t restart_interval_ = 0;
		uint8_t orientation_ = 1;
		bool orientation_seen_ = false;
		bool adobe_seen_ = false;
		uint8_t adobe_transform_ = 1;
		bool jfif_seen_ = false;
		bool allocated_ = false;
		unsigned scans_ = 0;
		unsigned scan_count_ = 0;
		unsigned scan_components_[4] = {};
		unsigned scan_dc_[4] = {};
		unsigned scan_ac_[4] = {};
		unsigned start_ = 0;
		unsigned end_ = 63;
		unsigned high_ = 0;
		unsigned low_ = 0;
		uint32_t eob_run_ = 0;
	};

	inline uint8_t clamp_8(int32_t a_value) { return static_cast<uint8_t>(a_value < 0 ? 0 : a_value > 255 ? 255 : a_value); }

	// One block's coefficients dequantized and inverse-transformed into 8 x 8 samples a_stride apart
	void inverse_transform(const int16_t* a_coefficients, const uint16_t* a_quantization, uint8_t* a_out, size_t a_stride) {
		int32_t dequantized[64];
		for (int i = 0; i < 64; ++i) { dequantized[i] = clamp_16(int32_t {a_coefficients[i]} * a_quantization[i]); }
		// down each column of frequencies: the samples' rows, kept with 4 fraction bits
		int32_t middle[64];
		for (int u = 0; u < 8; ++u) {
			bool flat = true;
			for (int v = 1; v < 8 && flat; ++v) { flat = dequantized[v * 8 + u] == 0; }
			for (int y = 0; y < 8; ++y) {
				int64_t sum = int64_t {k_cosine[y][0]} * dequantized[u];
				if (!flat) {
					for (int v = 1; v < 8; ++v) { sum += int64_t {k_cosine[y][v]} * dequantized[v * 8 + u]; }
				}
				middle[y * 8 + u] = static_cast<int32_t>((sum + (int64_t {1} << 8)) >> 9);
			}
		}
		// then along each row, with the level shift of 128
		for (int y = 0; y < 8; ++y) {
			const int32_t* row = middle + y * 8;
			uint8_t* out = a_out + y * a_stride;
			for (int x = 0; x < 8; ++x) {
				int64_t sum = 0;
				for (int u = 0; u < 8; ++u) { sum += int64_t {k_cosine[x][u]} * row[u]; }
				out[x] = clamp_8(static_cast<int32_t>((sum + (int64_t {1} << 16)) >> 17) + 128);
			}
		}
	}

	// A component's samples at the frame's size. Smooth (a scale of 2 across, down or both): each output pixel takes 3/4 of the
	// sample it lies in and 1/4 of the next one toward it on each axis, the triangle filter over samples sited at the centre of
	// the pixels they cover, rounded as browsers' decoders round it. Otherwise each sample is repeated over its pixels.
	void upsample_row(const std::vector<uint8_t>& a_plane, size_t a_stride, uint32_t a_samples_across, uint32_t a_samples_down, unsigned a_scale_x, unsigned a_scale_y,
	                  bool a_smooth, uint32_t a_y, uint32_t a_width, std::vector<int32_t>& a_scratch, uint8_t* a_out) {
		if (!a_smooth) {
			const uint8_t* row = a_plane.data() + size_t {a_y / a_scale_y} * a_stride;
			for (uint32_t x = 0; x < a_width; ++x) { a_out[x] = row[x / a_scale_x]; }
			return;
		}
		const uint32_t last_column = a_samples_across - 1;
		const uint32_t last_row = a_samples_down - 1;
		const uint32_t near_row = a_scale_y == 2 ? a_y / 2 : a_y;
		const uint8_t* nearer = a_plane.data() + size_t {near_row} * a_stride;
		if (a_scale_y == 2) {
			const uint32_t far_row = (a_y & 1) == 0 ? (near_row == 0 ? 0 : near_row - 1) : (near_row == last_row ? last_row : near_row + 1);
			const uint8_t* further = a_plane.data() + size_t {far_row} * a_stride;
			for (uint32_t x = 0; x <= last_column; ++x) { a_scratch[x] = 3 * nearer[x] + further[x]; }
		} else {
			for (uint32_t x = 0; x <= last_column; ++x) { a_scratch[x] = nearer[x]; }
		}
		if (a_scale_x == 1) {
			// down only: the halfway rounding alternates by row
			const int32_t bias = (a_y & 1) == 0 ? 1 : 2;
			for (uint32_t x = 0; x < a_width; ++x) { a_out[x] = static_cast<uint8_t>((a_scratch[x] + bias) >> 2); }
			return;
		}
		const int shift = a_scale_y == 2 ? 4 : 2;
		for (uint32_t x = 0; x < a_width; ++x) {
			const uint32_t near_column = x / 2;
			const bool even = (x & 1) == 0;
			const uint32_t far_column = even ? (near_column == 0 ? 0 : near_column - 1) : (near_column == last_column ? last_column : near_column + 1);
			// the halfway rounding alternates by column: up then down for both axes, down then up for one
			const int32_t bias = a_scale_y == 2 ? (even ? 8 : 7) : (even ? 1 : 2);
			a_out[x] = static_cast<uint8_t>((3 * a_scratch[near_column] + a_scratch[far_column] + bias) >> shift);
		}
	}

	status decoder::to_bitmap(bitmap& a_bitmap) {
		const uint32_t width = frame_.width;
		const uint32_t height = frame_.height;
		std::vector<uint8_t> planes[3];
		for (unsigned i = 0; i < frame_.count; ++i) {
			component& each = frame_.components[i];
			const size_t stride = size_t {each.blocks_across} * 8;
			planes[i].assign(stride * each.blocks_down * 8, 0);
			for (uint32_t row = 0; row < each.blocks_down; ++row) {
				for (uint32_t column = 0; column < each.blocks_across; ++column) {
					inverse_transform(each.block(column, row), each.quantization, planes[i].data() + size_t {row} * 8 * stride + size_t {column} * 8, stride);
				}
				if (work_.exhausted(each.blocks_across * 4 + 1)) { return status::budget; }
			}
			each.coefficients = std::vector<int16_t>();
		}
		a_bitmap.width = width;
		a_bitmap.height = height;
		a_bitmap.rgba.assign(size_t {width} * height * 4, 255);
		// three components are YCbCr, but RGB when an Adobe marker says transform 0, or no JFIF or Adobe marker speaks and their ids spell R, G, B
		const bool rgb = frame_.count == 3 &&
			((adobe_seen_ && adobe_transform_ == 0) || (!adobe_seen_ && !jfif_seen_ && frame_.components[0].id == 'R' && frame_.components[1].id == 'G' && frame_.components[2].id == 'B'));
		std::vector<uint8_t> rows[3];
		std::vector<int32_t> scratch;
		for (unsigned i = 0; i < frame_.count; ++i) {
			rows[i].resize(width);
			if (frame_.components[i].samples_across > scratch.size()) { scratch.resize(frame_.components[i].samples_across); }
		}
		for (uint32_t y = 0; y < height; ++y) {
			for (unsigned i = 0; i < frame_.count; ++i) {
				const component& each = frame_.components[i];
				const unsigned scale_x = frame_.h_max / each.h;
				const unsigned scale_y = frame_.v_max / each.v;
				// interpolated where the scale is 2:1 across, down or both and the component is more than 2 samples wide, as
				// browsers' decoders do; repeated otherwise (4:1:1 among them), so a picture looks as it does there
				const bool smooth = (scale_x == 2 || scale_y == 2) && scale_x <= 2 && scale_y <= 2 && each.samples_across > 2;
				upsample_row(planes[i], size_t {each.blocks_across} * 8, each.samples_across, each.samples_down, scale_x, scale_y, smooth, y, width, scratch, rows[i].data());
			}
			uint8_t* out = a_bitmap.rgba.data() + size_t {y} * width * 4;
			for (uint32_t x = 0; x < width; ++x, out += 4) {
				if (frame_.count == 1) {
					out[0] = out[1] = out[2] = rows[0][x];
				} else if (rgb) {
					out[0] = rows[0][x];
					out[1] = rows[1][x];
					out[2] = rows[2][x];
				} else {
					// JFIF's YCbCr, the full-range BT.601 matrix, in 16-bit fixed point
					const int32_t luma = int32_t {rows[0][x]} << 16;
					const int32_t blue = int32_t {rows[1][x]} - 128;
					const int32_t red = int32_t {rows[2][x]} - 128;
					out[0] = clamp_8((luma + 91881 * red + 32768) >> 16);
					out[1] = clamp_8((luma - 22554 * blue - 46802 * red + 32768) >> 16);
					out[2] = clamp_8((luma + 116130 * blue + 32768) >> 16);
				}
			}
			if (work_.exhausted(width / 8 + 1)) { return status::budget; }
		}
		return status::ok;
	}

	// The writer's half: baseline JPEG with the quality-scaled tables of T.81 Annex K.1 and Huffman tables built per image (K.2)

	inline constexpr uint8_t k_luma_table[64] = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
	                                             18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
	inline constexpr uint8_t k_chroma_table[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
	                                               99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};
	// At this quality and above the chroma keeps every sample (4:4:4); below it, one sample a 2 x 2 square (4:2:0)
	inline constexpr int k_full_chroma_quality = 90;

	// A table of Annex K.1 scaled as quality 1 to 100 scales it (50 is the table itself), natural order, each 1 to 255
	void scaled_table(const uint8_t (&a_table)[64], int a_quality, uint16_t (&a_out)[64]) {
		const int scale = a_quality < 50 ? 5000 / a_quality : 200 - 2 * a_quality;
		for (int i = 0; i < 64; ++i) {
			const int value = (a_table[i] * scale + 50) / 100;
			a_out[i] = static_cast<uint16_t>(value < 1 ? 1 : value > 255 ? 255 : value);
		}
	}

	// The forward DCT of 8 x 8 level-shifted samples (the transpose of the inverse's terms) and each coefficient divided by its
	// quantizer, rounded to nearest, in zig-zag order
	void forward_transform(const int32_t (&a_samples)[64], const uint16_t (&a_quantization)[64], int16_t (&a_out)[64]) {
		int32_t middle[64];
		for (int y = 0; y < 8; ++y) {
			for (int u = 0; u < 8; ++u) {
				int32_t sum = 0;
				for (int x = 0; x < 8; ++x) { sum += k_cosine[x][u] * a_samples[y * 8 + x]; }
				middle[y * 8 + u] = (sum + (1 << 5)) >> 6;
			}
		}
		for (int k = 0; k < 64; ++k) {
			const int at = k_zigzag[k];
			const int v = at >> 3;
			const int u = at & 7;
			int64_t sum = 0;
			for (int y = 0; y < 8; ++y) { sum += int64_t {k_cosine[y][v]} * middle[y * 8 + u]; }
			// the sum carries 2^20; the quotient rounds half away from zero, as the reference encoder does
			const int64_t divisor = int64_t {a_quantization[at]} << 20;
			const int64_t quotient = sum >= 0 ? (sum + divisor / 2) / divisor : -((-sum + divisor / 2) / divisor);
			a_out[k] = static_cast<int16_t>(quotient);
		}
	}

	inline unsigned bit_length(uint32_t a_value) {
		unsigned length = 0;
		while (a_value) {
			++length;
			a_value >>= 1;
		}
		return length;
	}

	// The code of every symbol a table holds, from its counts per length and its values, as T.81 C.2 assigns them
	struct huffman_code {
		uint8_t counts[16] = {};
		uint8_t values[256] = {};
		unsigned total = 0;
		uint16_t code[256] = {};
		uint8_t length[256] = {};

		// Optimal lengths for these symbol frequencies, held to 16 bits, no code all ones (T.81 K.2)
		void build(const uint32_t (&a_frequency)[256]) {
			uint8_t lengths[256];
			code_lengths(a_frequency, 256, 16, true, lengths);
			total = 0;
			for (unsigned bits = 1; bits <= 16; ++bits) {
				counts[bits - 1] = 0;
				for (unsigned symbol = 0; symbol < 256; ++symbol) {
					if (lengths[symbol] != bits) { continue; }
					values[total++] = static_cast<uint8_t>(symbol);
					++counts[bits - 1];
				}
			}
			assign();
		}

		void assign() {
			uint32_t next_code = 0;
			unsigned index = 0;
			for (unsigned bits = 1; bits <= 16; ++bits) {
				for (unsigned i = 0; i < counts[bits - 1]; ++i, ++index) {
					code[values[index]] = static_cast<uint16_t>(next_code++);
					length[values[index]] = static_cast<uint8_t>(bits);
				}
				next_code <<= 1;
			}
		}
	};

	class bit_sink {
	public:
		explicit bit_sink(std::vector<uint8_t>& a_out) : out_(a_out) {}
		void put(uint32_t a_bits, unsigned a_count) {
			if (a_count == 0) { return; }
			buffer_ = (buffer_ << a_count) | (a_bits & ((uint32_t {1} << a_count) - 1));
			count_ += a_count;
			while (count_ >= 8) {
				const uint8_t byte = static_cast<uint8_t>(buffer_ >> (count_ - 8));
				out_.push_back(byte);
				// a data byte of FF is stuffed with 00, so it is never read as a marker
				if (byte == 0xff) { out_.push_back(0); }
				count_ -= 8;
			}
		}
		// to a byte boundary with one bits, as T.81 F.1.2.3 pads
		void flush() {
			if (count_ > 0) { put(0x7f, 8 - count_); }
		}

	private:
		std::vector<uint8_t>& out_;
		uint64_t buffer_ = 0;
		unsigned count_ = 0;
	};

	// The quantized coefficients of every block of one component, in the order the scan visits them, and the symbol counts
	struct encoded_component {
		uint32_t blocks_across = 0;
		uint32_t blocks_down = 0;
		unsigned h = 1;
		unsigned v = 1;
		unsigned table = 0;
		std::vector<int16_t> blocks;
	};

	void count_symbols(const int16_t* a_block, int32_t& a_prediction, uint32_t (&a_dc)[256], uint32_t (&a_ac)[256]) {
		const int32_t difference = a_block[0] - a_prediction;
		a_prediction = a_block[0];
		++a_dc[bit_length(static_cast<uint32_t>(difference < 0 ? -difference : difference))];
		unsigned run = 0;
		for (int k = 1; k < 64; ++k) {
			const int32_t value = a_block[k];
			if (value == 0) {
				++run;
				continue;
			}
			while (run > 15) {
				++a_ac[0xf0];
				run -= 16;
			}
			++a_ac[(run << 4) | bit_length(static_cast<uint32_t>(value < 0 ? -value : value))];
			run = 0;
		}
		if (run > 0) { ++a_ac[0x00]; }
	}

	void write_block(bit_sink& a_bits, const int16_t* a_block, int32_t& a_prediction, const huffman_code& a_dc, const huffman_code& a_ac) {
		const int32_t difference = a_block[0] - a_prediction;
		a_prediction = a_block[0];
		const unsigned category = bit_length(static_cast<uint32_t>(difference < 0 ? -difference : difference));
		a_bits.put(a_dc.code[category], a_dc.length[category]);
		a_bits.put(static_cast<uint32_t>(difference < 0 ? difference - 1 : difference), category);
		unsigned run = 0;
		for (int k = 1; k < 64; ++k) {
			const int32_t value = a_block[k];
			if (value == 0) {
				++run;
				continue;
			}
			while (run > 15) {
				a_bits.put(a_ac.code[0xf0], a_ac.length[0xf0]);
				run -= 16;
			}
			const unsigned size = bit_length(static_cast<uint32_t>(value < 0 ? -value : value));
			const unsigned symbol = (run << 4) | size;
			a_bits.put(a_ac.code[symbol], a_ac.length[symbol]);
			a_bits.put(static_cast<uint32_t>(value < 0 ? value - 1 : value), size);
			run = 0;
		}
		if (run > 0) { a_bits.put(a_ac.code[0x00], a_ac.length[0x00]); }
	}

	void put_16(std::vector<uint8_t>& a_out, uint32_t a_value) {
		a_out.push_back(static_cast<uint8_t>(a_value >> 8));
		a_out.push_back(static_cast<uint8_t>(a_value));
	}

	void put_tables(std::vector<uint8_t>& a_out, unsigned a_kind_slot, const huffman_code& a_code) {
		a_out.push_back(static_cast<uint8_t>(a_kind_slot));
		a_out.insert(a_out.end(), a_code.counts, a_code.counts + 16);
		a_out.insert(a_out.end(), a_code.values, a_code.values + a_code.total);
	}

	status encode(const bitmap& a_bitmap, int a_quality, const work_hook& a_work, std::vector<uint8_t>& a_bytes) {
		const uint32_t width = a_bitmap.width;
		const uint32_t height = a_bitmap.height;
		const uint8_t* pixels = a_bitmap.rgba.data();
		const size_t count = size_t {width} * height;
		// JPEG has no alpha: each pixel is composited over white first
		auto flattened = [&](size_t a_pixel, int a_channel) {
			const int32_t alpha = pixels[4 * a_pixel + 3];
			return (pixels[4 * a_pixel + a_channel] * alpha + 255 * (255 - alpha) + 127) / 255;
		};
		bool grey = true;
		for (size_t i = 0; i < count && grey; ++i) { grey = pixels[4 * i] == pixels[4 * i + 1] && pixels[4 * i + 1] == pixels[4 * i + 2]; }
		const bool subsampled = !grey && a_quality < k_full_chroma_quality;
		const unsigned components = grey ? 1 : 3;
		const unsigned h_max = subsampled ? 2 : 1;
		const uint32_t mcus_across = (width + 8 * h_max - 1) / (8 * h_max);
		const uint32_t mcus_down = (height + 8 * h_max - 1) / (8 * h_max);
		uint16_t tables[2][64];
		scaled_table(k_luma_table, a_quality, tables[0]);
		scaled_table(k_chroma_table, a_quality, tables[1]);
		// the planes at full size (Y, Cb, Cr), the frame's edge pixels repeated out to whole MCUs
		const uint32_t padded_width = mcus_across * 8 * h_max;
		const uint32_t padded_height = mcus_down * 8 * h_max;
		std::vector<uint8_t> planes[3];
		for (unsigned c = 0; c < components; ++c) { planes[c].resize(size_t {padded_width} * padded_height); }
		for (uint32_t y = 0; y < padded_height; ++y) {
			const uint32_t from_y = y < height ? y : height - 1;
			for (uint32_t x = 0; x < padded_width; ++x) {
				const size_t pixel = size_t {from_y} * width + (x < width ? x : width - 1);
				const size_t at = size_t {y} * padded_width + x;
				const int32_t red = flattened(pixel, 0);
				if (grey) {
					planes[0][at] = static_cast<uint8_t>(red);
					continue;
				}
				const int32_t green = flattened(pixel, 1);
				const int32_t blue = flattened(pixel, 2);
				// JFIF's full-range BT.601 matrix in 16-bit fixed point; Cb and Cr centred on 128
				planes[0][at] = clamp_8((19595 * red + 38470 * green + 7471 * blue + 32768) >> 16);
				planes[1][at] = clamp_8((-11059 * red - 21709 * green + 32768 * blue + (128 << 16) + 32768) >> 16);
				planes[2][at] = clamp_8((32768 * red - 27439 * green - 5329 * blue + (128 << 16) + 32768) >> 16);
			}
			if (a_work.exhausted(padded_width / 16 + 1)) { return status::budget; }
		}
		encoded_component parts[3];
		for (unsigned c = 0; c < components; ++c) {
			encoded_component& part = parts[c];
			part.h = c == 0 ? h_max : 1;
			part.v = part.h;
			part.table = c == 0 ? 0 : 1;
			part.blocks_across = mcus_across * part.h;
			part.blocks_down = mcus_down * part.v;
			part.blocks.resize(size_t {part.blocks_across} * part.blocks_down * 64);
			const unsigned step = h_max / part.h;
			for (uint32_t row = 0; row < part.blocks_down; ++row) {
				for (uint32_t column = 0; column < part.blocks_across; ++column) {
					int32_t samples[64];
					for (int y = 0; y < 8; ++y) {
						for (int x = 0; x < 8; ++x) {
							// a subsampled chroma sample is the mean of the 2 x 2 pixels it covers
							const size_t top = (size_t {row} * 8 + y) * step;
							const size_t left = (size_t {column} * 8 + x) * step;
							int32_t sum = 0;
							for (unsigned dy = 0; dy < step; ++dy) {
								for (unsigned dx = 0; dx < step; ++dx) { sum += planes[c][(top + dy) * padded_width + left + dx]; }
							}
							const int32_t area = static_cast<int32_t>(step * step);
							samples[y * 8 + x] = (sum + area / 2) / area - 128;
						}
					}
					int16_t coefficients[64];
					forward_transform(samples, tables[part.table], coefficients);
					std::memcpy(part.blocks.data() + (size_t {row} * part.blocks_across + column) * 64, coefficients, sizeof(coefficients));
				}
				if (a_work.exhausted(part.blocks_across * 8 + 1)) { return status::budget; }
			}
			planes[c] = std::vector<uint8_t>();
		}
		// the MCUs in scan order, once to count the symbols and once to write them
		auto each_block = [&](auto&& a_visit) {
			for (uint32_t my = 0; my < mcus_down; ++my) {
				for (uint32_t mx = 0; mx < mcus_across; ++mx) {
					for (unsigned c = 0; c < components; ++c) {
						const encoded_component& part = parts[c];
						for (unsigned y = 0; y < part.v; ++y) {
							for (unsigned x = 0; x < part.h; ++x) {
								a_visit(c, part.blocks.data() + ((size_t {my} * part.v + y) * part.blocks_across + size_t {mx} * part.h + x) * 64);
							}
						}
					}
				}
			}
		};
		uint32_t dc_counts[2][256] = {};
		uint32_t ac_counts[2][256] = {};
		int32_t predictions[3] = {0, 0, 0};
		each_block([&](unsigned a_component, const int16_t* a_block) {
			const unsigned table = parts[a_component].table;
			count_symbols(a_block, predictions[a_component], dc_counts[table], ac_counts[table]);
		});
		const unsigned table_count = grey ? 1 : 2;
		huffman_code dc_codes[2];
		huffman_code ac_codes[2];
		for (unsigned t = 0; t < table_count; ++t) {
			dc_codes[t].build(dc_counts[t]);
			ac_codes[t].build(ac_counts[t]);
		}

		std::vector<uint8_t>& out = a_bytes;
		out.clear();
		out.insert(out.end(), {0xff, 0xd8});
		// JFIF 1.01, square pixels, no thumbnail; and no other segment, so no EXIF (camera, place, time) is ever written
		out.insert(out.end(), {0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00});
		out.insert(out.end(), {0xff, 0xdb});
		put_16(out, 2 + 65 * table_count);
		for (unsigned t = 0; t < table_count; ++t) {
			out.push_back(static_cast<uint8_t>(t));
			for (int k = 0; k < 64; ++k) { out.push_back(static_cast<uint8_t>(tables[t][k_zigzag[k]])); }
		}
		out.insert(out.end(), {0xff, 0xc0});
		put_16(out, 8 + 3 * components);
		out.push_back(8);
		put_16(out, height);
		put_16(out, width);
		out.push_back(static_cast<uint8_t>(components));
		for (unsigned c = 0; c < components; ++c) {
			out.push_back(static_cast<uint8_t>(c + 1));
			out.push_back(static_cast<uint8_t>((parts[c].h << 4) | parts[c].v));
			out.push_back(static_cast<uint8_t>(parts[c].table));
		}
		size_t length_at = out.size() + 2;
		out.insert(out.end(), {0xff, 0xc4, 0x00, 0x00});
		for (unsigned t = 0; t < table_count; ++t) {
			put_tables(out, t, dc_codes[t]);
			put_tables(out, 0x10 | t, ac_codes[t]);
		}
		const size_t tables_length = out.size() - length_at;
		out[length_at] = static_cast<uint8_t>(tables_length >> 8);
		out[length_at + 1] = static_cast<uint8_t>(tables_length);
		out.insert(out.end(), {0xff, 0xda});
		put_16(out, 6 + 2 * components);
		out.push_back(static_cast<uint8_t>(components));
		for (unsigned c = 0; c < components; ++c) {
			out.push_back(static_cast<uint8_t>(c + 1));
			out.push_back(static_cast<uint8_t>((parts[c].table << 4) | parts[c].table));
		}
		out.insert(out.end(), {0x00, 0x3f, 0x00});
		bit_sink bits(out);
		predictions[0] = predictions[1] = predictions[2] = 0;
		size_t written = 0;
		bool spent = false;
		each_block([&](unsigned a_component, const int16_t* a_block) {
			const unsigned table = parts[a_component].table;
			write_block(bits, a_block, predictions[a_component], dc_codes[table], ac_codes[table]);
			if (++written % 1024 == 0 && !spent) { spent = a_work.exhausted(1024); }
		});
		if (spent) { return status::budget; }
		bits.flush();
		out.insert(out.end(), {0xff, 0xd9});
		return status::ok;
	}

} // namespace jpeg_detail

	status read_jpeg_header(const uint8_t* a_data, size_t a_size, const limits& a_limits, header& a_header) {
		const work_hook none;
		jpeg_detail::decoder reader(a_data, a_size, a_limits, none);
		return reader.run(true, a_header);
	}

	status encode_jpeg(const bitmap& a_bitmap, int a_quality, const work_hook& a_work, std::vector<uint8_t>& a_bytes) {
		if (a_bitmap.width > 65535 || a_bitmap.height > 65535) { return status::jpeg_too_large; }
		try {
			return jpeg_detail::encode(a_bitmap, a_quality < 1 ? 1 : a_quality > 100 ? 100 : a_quality, a_work, a_bytes);
		} catch (const std::bad_alloc&) {
			return status::out_of_memory;
		} catch (const std::length_error&) {
			return status::out_of_memory;
		}
	}

	status decode_jpeg(const uint8_t* a_data, size_t a_size, const limits& a_limits, const work_hook& a_work, header& a_header, bitmap& a_bitmap) {
		try {
			// the decoder holds its tables and every coefficient: on the heap, not the stack
			auto reader = std::make_unique<jpeg_detail::decoder>(a_data, a_size, a_limits, a_work);
			header found;
			if (const status result = reader->run(false, found); result != status::ok) { return result; }
			a_header = found;
			return reader->to_bitmap(a_bitmap);
		} catch (const std::bad_alloc&) {
			return status::out_of_memory;
		} catch (const std::length_error&) {
			return status::out_of_memory;
		}
	}

} // namespace image_codec
} // namespace jai

#endif
