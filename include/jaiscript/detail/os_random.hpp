#pragma once

#ifndef __JAISCRIPT_DETAIL_OS_RANDOM_HPP__
#define __JAISCRIPT_DETAIL_OS_RANDOM_HPP__

#include <cstddef>

namespace jai::detail {

	// a_size bytes from the operating system's secure generator (BCryptGenRandom, getrandom, arc4random_buf); false when it
	// fails. Safe on any thread. Defined in source/implementation/stdlib/crypto.cpp.
	bool os_random_bytes(void* a_out, size_t a_size) noexcept;

} // namespace jai::detail

#endif
