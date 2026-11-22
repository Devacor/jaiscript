#pragma once

#ifndef __JAISCRIPT_STDLIB_IMAGE_HPP__
#define __JAISCRIPT_STDLIB_IMAGE_HPP__

#include <cstdint>

namespace jai {
class engine;
namespace stdlib {

	// What image::load and image::data accept: more bytes than max_input_bytes, or a header naming more than max_pixels pixels, is
	// refused before anything is decoded. max_pixels is held to 2^28 whatever the host asks.
	struct image_limits {
		uint64_t max_pixels = 25'000'000;
		uint64_t max_input_bytes = uint64_t {64} << 20;
	};

	// The image:: module (source/implementation/stdlib/image.cpp), registered only by this call (never by register_all), and
	// compiled only when JAISCRIPT_ENABLE_IMAGE is defined: the image class, image::load and image::data over the in-house
	// PNG, JPEG and GIF codecs
	void register_image(engine& eng_ref, const image_limits& a_limits = {});

} // namespace stdlib
} // namespace jai

#endif
