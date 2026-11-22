#ifdef JAISCRIPT_ENABLE_IMAGE

#include <jaiscript/stdlib/image.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/engine_access.hpp>
#include <jaiscript/detail/execution_limits.hpp>
#include <jaiscript/detail/image_codec.hpp>
#include <jaiscript/detail/work_budget.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jai {
namespace stdlib {
namespace image_detail {

	namespace codec = image_codec;

	// The script's image: 8-bit RGBA shared by its copies until one of them changes, so a copy costs a pointer
	struct script_image {
		std::shared_ptr<const codec::bitmap> pixels;
		codec::format source = codec::format::none;

		uint32_t width() const { return pixels ? pixels->width : 0; }
		uint32_t height() const { return pixels ? pixels->height : 0; }
	};

	// The engine's work budget as the codecs spend it
	struct budget {
		detail::work_meter meter;
		explicit budget(engine* a_engine) : meter(a_engine) {}
		codec::work_hook hook() { return codec::work_hook {[](void* a_meter, size_t a_units) { return static_cast<detail::work_meter*>(a_meter)->spend(a_units); }, &meter}; }
	};

	[[noreturn]] JAI_NOINLINE void raise(const budget& a_budget, codec::status a_status) {
		if (a_status == codec::status::budget) { a_budget.meter.throw_spent(); }
		throw runtime_error(codec::status_text(a_status));
	}

	// a_pixels of new image memory charged to the engine's memory cap before it is made
	void charge(engine* a_engine, uint64_t a_pixels) {
		detail::execution_limits& limits = detail::engine_access::execution_limits(*a_engine);
		if (a_pixels > SIZE_MAX / 4 || !limits.memory_charge_items(static_cast<size_t>(a_pixels), 4)) [[unlikely]] { detail::throw_memory_cap(limits); }
	}

	// The bytes of an image argument: an array<uint8> or a string, either way the file's bytes and never a path (a file is
	// read with file::read, under that function's permission)
	struct source_bytes {
		const uint8_t* data = nullptr;
		size_t size = 0;

		source_bytes(const script_value& a_source, const char* a_type_error) {
			const script_value& source = a_source.deref();
			if (source.is_array()) {
				const script_array* elements = source.unchecked_array_node();
				if (elements->kind() != script_array::kind_t::u8) { throw runtime_error(a_type_error); }
				data = elements->bytes().data();
				size = elements->bytes().size();
				return;
			}
			const std::string* text = source.text_bytes();
			if (!text) { throw runtime_error(a_type_error); }
			data = reinterpret_cast<const uint8_t*>(text->data());
			size = text->size();
		}
	};

	script_image load(engine* a_engine, const script_value& a_source, const codec::limits& a_limits) {
		const source_bytes file(a_source, "image::load expects a string or an array<uint8>");
		budget work(a_engine);
		codec::header header;
		if (const codec::status result = codec::read_header(file.data, file.size, a_limits, header); result != codec::status::ok) { raise(work, result); }
		charge(a_engine, uint64_t {header.width} * header.height);
		auto pixels = std::make_shared<codec::bitmap>();
		if (const codec::status result = codec::decode(file.data, file.size, a_limits, work.hook(), header, *pixels); result != codec::status::ok) { raise(work, result); }
		if (header.orientation != 1) {
			// turned as the file says, as a browser shows it
			auto turned = std::make_shared<codec::bitmap>();
			if (const codec::status result = codec::orient(*pixels, header.orientation, *turned); result != codec::status::ok) { raise(work, result); }
			pixels = std::move(turned);
		}
		script_image result;
		result.pixels = std::move(pixels);
		result.source = header.kind;
		return result;
	}

	script_value data(engine* a_engine, const script_value& a_source, const codec::limits& a_limits) {
		const source_bytes file(a_source, "image::data expects a string or an array<uint8>");
		codec::header header;
		if (const codec::status result = codec::read_header(file.data, file.size, a_limits, header); result != codec::status::ok) { throw runtime_error(codec::status_text(result)); }
		script_value answer = script_value::make_map(detail::engine_access::get_type_info_string(*a_engine), nullptr, a_engine);
		script_map& entries = const_cast<script_map&>(answer.as_map());
		entries[script_value(std::string("format"), a_engine)] = script_value(std::string(codec::format_name(header.kind)), a_engine);
		entries[script_value(std::string("width"), a_engine)] = script_value(static_cast<script_int>(header.oriented_width()), a_engine);
		entries[script_value(std::string("height"), a_engine)] = script_value(static_cast<script_int>(header.oriented_height()), a_engine);
		return answer;
	}

	inline constexpr const char* k_empty = "image: the image is empty (image::load makes one)";
	inline constexpr script_int k_default_quality = 90;

	const codec::bitmap& pixels_of(const script_image& a_image) {
		if (!a_image.pixels || a_image.pixels->rgba.empty()) { throw runtime_error(k_empty); }
		return *a_image.pixels;
	}

	script_image with_pixels(const script_image& a_image, codec::bitmap&& a_pixels) {
		script_image result;
		result.pixels = std::make_shared<const codec::bitmap>(std::move(a_pixels));
		result.source = a_image.source;
		return result;
	}

	// a_image scaled to fit inside a_width x a_height, never enlarged
	script_image fit(engine* a_engine, const script_image& a_image, script_int a_width, script_int a_height, const char* a_size_error) {
		const codec::bitmap& source = pixels_of(a_image);
		if (a_width < 1 || a_height < 1) { throw runtime_error(a_size_error); }
		uint32_t width = 0;
		uint32_t height = 0;
		codec::fit_size(source.width, source.height, static_cast<uint64_t>(a_width), static_cast<uint64_t>(a_height), width, height);
		if (width == source.width && height == source.height) { return a_image; }
		charge(a_engine, uint64_t {width} * height);
		budget work(a_engine);
		codec::bitmap result;
		if (const codec::status outcome = codec::resize(source, width, height, work.hook(), result); outcome != codec::status::ok) { raise(work, outcome); }
		return with_pixels(a_image, std::move(result));
	}

	// a_image scaled to cover a_width x a_height and cut to exactly that size about its centre
	script_image crop_fill(engine* a_engine, const script_image& a_image, script_int a_width, script_int a_height, const codec::limits& a_limits, const char* a_size_error) {
		const codec::bitmap& source = pixels_of(a_image);
		if (a_width < 1 || a_height < 1) { throw runtime_error(a_size_error); }
		const uint64_t limit = codec::pixel_limit(a_limits);
		if (static_cast<uint64_t>(a_width) > limit || static_cast<uint64_t>(a_height) > limit || static_cast<uint64_t>(a_width) * static_cast<uint64_t>(a_height) > limit) {
			throw runtime_error(codec::status_text(codec::status::too_many_pixels));
		}
		const uint32_t width = static_cast<uint32_t>(a_width);
		const uint32_t height = static_cast<uint32_t>(a_height);
		charge(a_engine, uint64_t {width} * height);
		budget work(a_engine);
		codec::bitmap result;
		if (const codec::status outcome = codec::crop_fill(source, width, height, work.hook(), result); outcome != codec::status::ok) { raise(work, outcome); }
		return with_pixels(a_image, std::move(result));
	}

	// a_image as a file of a_format: its bytes, as a string
	std::string encode(engine* a_engine, const script_image& a_image, const std::string& a_format, script_int a_quality) {
		const codec::bitmap& source = pixels_of(a_image);
		const bool jpeg = a_format == "jpeg";
		if (!jpeg && a_format != "png") { throw runtime_error("image::encode expects the format \"jpeg\" or \"png\""); }
		if (a_quality < 1 || a_quality > 100) { throw runtime_error("image::encode expects a quality of 1 to 100"); }
		budget work(a_engine);
		std::vector<uint8_t> bytes;
		// PNG is lossless: a quality is checked and has nothing to change
		const codec::status outcome = jpeg ? codec::encode_jpeg(source, static_cast<int>(a_quality), work.hook(), bytes) : codec::encode_png(source, work.hook(), bytes);
		if (outcome != codec::status::ok) { raise(work, outcome); }
		detail::execution_limits& limits = detail::engine_access::execution_limits(*a_engine);
		if (!limits.memory_charge_items(bytes.size(), 1)) [[unlikely]] { detail::throw_memory_cap(limits); }
		return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}

	[[noreturn]] JAI_NOINLINE void outside(const char* a_who, script_int a_x, script_int a_y, script_int a_width, script_int a_height, const codec::bitmap& a_source) {
		throw runtime_error(std::string(a_who) + ": the rectangle at (" + std::to_string(a_x) + ", " + std::to_string(a_y) + ") of " + std::to_string(a_width) + " x " +
		                    std::to_string(a_height) + " is not inside the " + std::to_string(a_source.width) + " x " + std::to_string(a_source.height) + " image");
	}

	script_image crop(engine* a_engine, const script_image& a_image, script_int a_x, script_int a_y, script_int a_width, script_int a_height, const char* a_who) {
		const codec::bitmap& source = pixels_of(a_image);
		// each bound checked against the size before any sum, so nothing overflows
		if (a_x < 0 || a_y < 0 || a_width < 1 || a_height < 1 || a_x >= source.width || a_y >= source.height || a_width > source.width - a_x || a_height > source.height - a_y) {
			outside(a_who, a_x, a_y, a_width, a_height, source);
		}
		if (a_width == source.width && a_height == source.height) { return a_image; }
		charge(a_engine, static_cast<uint64_t>(a_width) * static_cast<uint64_t>(a_height));
		codec::bitmap result;
		if (const codec::status outcome = codec::crop(source, static_cast<uint32_t>(a_x), static_cast<uint32_t>(a_y), static_cast<uint32_t>(a_width), static_cast<uint32_t>(a_height), result);
		    outcome != codec::status::ok) {
			throw runtime_error(codec::status_text(outcome));
		}
		return with_pixels(a_image, std::move(result));
	}

} // namespace image_detail

	void register_image(engine& eng_ref, const image_limits& a_limits) {
		using namespace image_detail;
		engine* eng = &eng_ref;
		codec::limits limits;
		limits.max_pixels = a_limits.max_pixels;
		limits.max_input_bytes = a_limits.max_input_bytes;
		// final: a script class holds an image as a field rather than derive from it
		dynamic_binder<script_image>(eng_ref, "image")
			.final()
			.property("width", [](const script_image& a_image) -> script_int { return a_image.width(); }, nullptr)
			.property("height", [](const script_image& a_image) -> script_int { return a_image.height(); }, nullptr)
			.property("format", [](const script_image& a_image) -> std::string { return codec::format_name(a_image.source); }, nullptr)
			// the dot methods change the image; the image:: functions of the same names return a new one
			.method("fit", [eng](script_image& a_image, script_int a_width, script_int a_height) {
				a_image = fit(eng, a_image, a_width, a_height, "image::fit expects a width and a height of at least 1");
			})
			.method("crop_fill", [eng, limits](script_image& a_image, script_int a_width, script_int a_height) {
				a_image = crop_fill(eng, a_image, a_width, a_height, limits, "image::crop_fill expects a width and a height of at least 1");
			})
			.method("crop", [eng](script_image& a_image, script_int a_x, script_int a_y, script_int a_width, script_int a_height) {
				a_image = crop(eng, a_image, a_x, a_y, a_width, a_height, "image::crop");
			})
			.method("encode", [eng](const script_image& a_image, const std::string& a_format) -> std::string { return encode(eng, a_image, a_format, k_default_quality); })
			.method("encode", [eng](const script_image& a_image, const std::string& a_format, script_int a_quality) -> std::string { return encode(eng, a_image, a_format, a_quality); })
			.static_method("fit", [eng](const script_image& a_image, script_int a_width, script_int a_height) -> script_image {
				return fit(eng, a_image, a_width, a_height, "image::fit expects a width and a height of at least 1");
			})
			.static_method("crop_fill", [eng, limits](const script_image& a_image, script_int a_width, script_int a_height) -> script_image {
				return crop_fill(eng, a_image, a_width, a_height, limits, "image::crop_fill expects a width and a height of at least 1");
			})
			.static_method("crop", [eng](const script_image& a_image, script_int a_x, script_int a_y, script_int a_width, script_int a_height) -> script_image {
				return crop(eng, a_image, a_x, a_y, a_width, a_height, "image::crop");
			})
			.static_method("load", [eng, limits](const script_value& a_source) -> script_image { return load(eng, a_source, limits); })
			.static_method("data", [eng, limits](const script_value& a_source) -> script_value { return data(eng, a_source, limits); })
			.build();
	}

} // namespace stdlib
} // namespace jai

#endif
