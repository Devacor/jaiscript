#pragma once

#ifndef __JAISCRIPT_STDLIB_CRYPTO_HPP__
#define __JAISCRIPT_STDLIB_CRYPTO_HPP__

namespace jai {
class engine;
namespace stdlib {

	// The crypto:: namespace (source/implementation/stdlib/crypto.cpp): hashes, HMAC, constant-time comparison, the operating
	// system's secure random bytes and bcrypt password hashing
	void register_crypto_functions(engine& eng_ref);

} // namespace stdlib
} // namespace jai

#endif
