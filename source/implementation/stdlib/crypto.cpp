#include <jaiscript/stdlib/crypto.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/os_random.hpp>
#include <jaiscript/detail/random_generator.hpp>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#if defined(_MSC_VER)
#pragma comment(lib, "bcrypt.lib")
#endif
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#include <stdlib.h>
#elif defined(__EMSCRIPTEN__)
#include <unistd.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace jai {
namespace detail {

	bool os_random_bytes(void* a_out, size_t a_size) noexcept {
		unsigned char* out = static_cast<unsigned char*>(a_out);
#if defined(_WIN32)
		while (a_size > 0) {
			const ULONG take = static_cast<ULONG>(std::min<size_t>(a_size, 0x40000000));
			if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out, take, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) { return false; }
			out += take;
			a_size -= take;
		}
		return true;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
		arc4random_buf(out, a_size);
		return true;
#elif defined(__EMSCRIPTEN__)
		// getentropy reads the browser's crypto.getRandomValues (or Node's crypto), at most 256 bytes a call
		while (a_size > 0) {
			const size_t take = std::min<size_t>(a_size, 256);
			if (getentropy(out, take) != 0) { return false; }
			out += take;
			a_size -= take;
		}
		return true;
#elif defined(__linux__)
#if defined(SYS_getrandom)
		while (a_size > 0) {
			const long got = syscall(SYS_getrandom, out, a_size, 0);
			if (got > 0) {
				out += got;
				a_size -= static_cast<size_t>(got);
			} else if (got < 0 && errno == EINTR) {
				continue;
			} else if (got < 0 && errno == ENOSYS) {
				break;
			} else {
				return false;
			}
		}
		if (a_size == 0) { return true; }
#endif
		// a kernel before getrandom (3.17): the urandom device
		const int device = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
		if (device < 0) { return false; }
		while (a_size > 0) {
			const ssize_t got = read(device, out, a_size);
			if (got > 0) {
				out += got;
				a_size -= static_cast<size_t>(got);
			} else if (got < 0 && errno == EINTR) {
				continue;
			} else {
				close(device);
				return false;
			}
		}
		close(device);
		return true;
#else
		(void)out;
		return a_size == 0;
#endif
	}

} // namespace detail

namespace stdlib {
namespace crypto_detail {

	inline uint32_t rotate_left_32(uint32_t a_x, int a_n) { return (a_x << a_n) | (a_x >> (32 - a_n)); }
	inline uint32_t rotate_right_32(uint32_t a_x, int a_n) { return (a_x >> a_n) | (a_x << (32 - a_n)); }
	inline uint64_t rotate_right_64(uint64_t a_x, int a_n) { return (a_x >> a_n) | (a_x << (64 - a_n)); }

	inline uint32_t load_big_32(const unsigned char* a_bytes) {
		return (uint32_t {a_bytes[0]} << 24) | (uint32_t {a_bytes[1]} << 16) | (uint32_t {a_bytes[2]} << 8) | uint32_t {a_bytes[3]};
	}
	inline uint32_t load_little_32(const unsigned char* a_bytes) {
		return uint32_t {a_bytes[0]} | (uint32_t {a_bytes[1]} << 8) | (uint32_t {a_bytes[2]} << 16) | (uint32_t {a_bytes[3]} << 24);
	}
	inline uint64_t load_big_64(const unsigned char* a_bytes) {
		return (uint64_t {load_big_32(a_bytes)} << 32) | load_big_32(a_bytes + 4);
	}
	inline void store_big_32(uint32_t a_word, std::string& a_out) {
		for (int shift = 24; shift >= 0; shift -= 8) { a_out.push_back(static_cast<char>((a_word >> shift) & 0xFF)); }
	}
	inline void store_little_32(uint32_t a_word, std::string& a_out) {
		for (int shift = 0; shift < 32; shift += 8) { a_out.push_back(static_cast<char>((a_word >> shift) & 0xFF)); }
	}
	inline void store_big_64(uint64_t a_word, std::string& a_out) {
		store_big_32(static_cast<uint32_t>(a_word >> 32), a_out);
		store_big_32(static_cast<uint32_t>(a_word), a_out);
	}

	// The Merkle-Damgard framing MD5, SHA-1, SHA-256 and SHA-512 share: whole blocks go to Derived::compress, the last one
	// is padded with 0x80, zeros and the message's length in bits (big-endian, or little-endian for MD5)
	template<class Derived, size_t BlockSize, size_t LengthBytes, bool LittleEndianLength>
	class block_digest {
	public:
		static constexpr size_t k_block_size = BlockSize;

		void update(const unsigned char* a_data, size_t a_size) {
			total += a_size;
			if (pending != 0) {
				const size_t take = std::min(a_size, BlockSize - pending);
				std::memcpy(block + pending, a_data, take);
				pending += take;
				a_data += take;
				a_size -= take;
				if (pending < BlockSize) { return; }
				self().compress(block);
				pending = 0;
			}
			for (; a_size >= BlockSize; a_data += BlockSize, a_size -= BlockSize) { self().compress(a_data); }
			std::memcpy(block, a_data, a_size);
			pending = a_size;
		}
		void update(std::string_view a_bytes) { update(reinterpret_cast<const unsigned char*>(a_bytes.data()), a_bytes.size()); }

	protected:
		void pad() {
			const uint64_t bits = total * 8;
			unsigned char tail[BlockSize * 2] = {0x80};
			const size_t padded = pending < BlockSize - LengthBytes ? BlockSize : BlockSize * 2;
			for (size_t i = 0; i < 8; ++i) {
				const unsigned char byte = static_cast<unsigned char>(bits >> (8 * i));
				tail[LittleEndianLength ? padded - pending - LengthBytes + i : padded - pending - 1 - i] = byte;
			}
			update(tail, padded - pending);
		}

	private:
		Derived& self() { return static_cast<Derived&>(*this); }
		unsigned char block[BlockSize] {};
		size_t pending {0};
		uint64_t total {0};
	};

	// SHA-256 (FIPS 180-4)
	class sha256_digest : public block_digest<sha256_digest, 64, 8, false> {
	public:
		static constexpr size_t k_output_size = 32;

		std::string finish() {
			pad();
			std::string result;
			result.reserve(k_output_size);
			for (uint32_t word : state) { store_big_32(word, result); }
			return result;
		}

		void compress(const unsigned char* a_block) {
			static constexpr uint32_t k_round_constants[64] = {
				0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
				0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
				0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
				0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
				0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
				0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
				0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
				0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
			};
			uint32_t w[64];
			for (int i = 0; i < 16; ++i) { w[i] = load_big_32(a_block + i * 4); }
			for (int i = 16; i < 64; ++i) {
				const uint32_t s0 = rotate_right_32(w[i - 15], 7) ^ rotate_right_32(w[i - 15], 18) ^ (w[i - 15] >> 3);
				const uint32_t s1 = rotate_right_32(w[i - 2], 17) ^ rotate_right_32(w[i - 2], 19) ^ (w[i - 2] >> 10);
				w[i] = w[i - 16] + s0 + w[i - 7] + s1;
			}
			uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
			for (int i = 0; i < 64; ++i) {
				const uint32_t t1 = h + (rotate_right_32(e, 6) ^ rotate_right_32(e, 11) ^ rotate_right_32(e, 25)) + ((e & f) ^ (~e & g)) + k_round_constants[i] + w[i];
				const uint32_t t2 = (rotate_right_32(a, 2) ^ rotate_right_32(a, 13) ^ rotate_right_32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
				h = g;
				g = f;
				f = e;
				e = d + t1;
				d = c;
				c = b;
				b = a;
				a = t1 + t2;
			}
			state[0] += a;
			state[1] += b;
			state[2] += c;
			state[3] += d;
			state[4] += e;
			state[5] += f;
			state[6] += g;
			state[7] += h;
		}

	private:
		uint32_t state[8] {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	};

	// SHA-512 (FIPS 180-4)
	class sha512_digest : public block_digest<sha512_digest, 128, 16, false> {
	public:
		static constexpr size_t k_output_size = 64;

		std::string finish() {
			pad();
			std::string result;
			result.reserve(k_output_size);
			for (uint64_t word : state) { store_big_64(word, result); }
			return result;
		}

		void compress(const unsigned char* a_block) {
			static constexpr uint64_t k_round_constants[80] = {
			0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
			0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
			0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
			0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
			0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
			0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
			0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
			0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
			0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
			0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
			0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
			0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
			0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
			0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
			0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
			0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
			0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
			0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
			0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
			0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
			};
			uint64_t w[80];
			for (int i = 0; i < 16; ++i) { w[i] = load_big_64(a_block + i * 8); }
			for (int i = 16; i < 80; ++i) {
				const uint64_t s0 = rotate_right_64(w[i - 15], 1) ^ rotate_right_64(w[i - 15], 8) ^ (w[i - 15] >> 7);
				const uint64_t s1 = rotate_right_64(w[i - 2], 19) ^ rotate_right_64(w[i - 2], 61) ^ (w[i - 2] >> 6);
				w[i] = w[i - 16] + s0 + w[i - 7] + s1;
			}
			uint64_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
			for (int i = 0; i < 80; ++i) {
				const uint64_t t1 = h + (rotate_right_64(e, 14) ^ rotate_right_64(e, 18) ^ rotate_right_64(e, 41)) + ((e & f) ^ (~e & g)) + k_round_constants[i] + w[i];
				const uint64_t t2 = (rotate_right_64(a, 28) ^ rotate_right_64(a, 34) ^ rotate_right_64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
				h = g;
				g = f;
				f = e;
				e = d + t1;
				d = c;
				c = b;
				b = a;
				a = t1 + t2;
			}
			state[0] += a;
			state[1] += b;
			state[2] += c;
			state[3] += d;
			state[4] += e;
			state[5] += f;
			state[6] += g;
			state[7] += h;
		}

	private:
		uint64_t state[8] {
		0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
		0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
		};
	};

	// SHA-1 (FIPS 180-4): for interoperability only
	class sha1_digest : public block_digest<sha1_digest, 64, 8, false> {
	public:
		static constexpr size_t k_output_size = 20;

		std::string finish() {
			pad();
			std::string result;
			result.reserve(k_output_size);
			for (uint32_t word : state) { store_big_32(word, result); }
			return result;
		}

		void compress(const unsigned char* a_block) {
			uint32_t w[80];
			for (int i = 0; i < 16; ++i) { w[i] = load_big_32(a_block + i * 4); }
			for (int i = 16; i < 80; ++i) { w[i] = rotate_left_32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1); }
			uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
			for (int i = 0; i < 80; ++i) {
				uint32_t f = 0, k = 0;
				if (i < 20) {
					f = (b & c) | (~b & d);
					k = 0x5a827999;
				} else if (i < 40) {
					f = b ^ c ^ d;
					k = 0x6ed9eba1;
				} else if (i < 60) {
					f = (b & c) | (b & d) | (c & d);
					k = 0x8f1bbcdc;
				} else {
					f = b ^ c ^ d;
					k = 0xca62c1d6;
				}
				const uint32_t t = rotate_left_32(a, 5) + f + e + k + w[i];
				e = d;
				d = c;
				c = rotate_left_32(b, 30);
				b = a;
				a = t;
			}
			state[0] += a;
			state[1] += b;
			state[2] += c;
			state[3] += d;
			state[4] += e;
		}

	private:
		uint32_t state[5] {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
	};

	// MD5 (RFC 1321): for interoperability only
	class md5_digest : public block_digest<md5_digest, 64, 8, true> {
	public:
		static constexpr size_t k_output_size = 16;

		std::string finish() {
			pad();
			std::string result;
			result.reserve(k_output_size);
			for (uint32_t word : state) { store_little_32(word, result); }
			return result;
		}

		void compress(const unsigned char* a_block) {
			static constexpr uint32_t k_sines[64] = {
			0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
			0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
			0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
			0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
			0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
			0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
			0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
			0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
			};
			static constexpr int k_shifts[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
			uint32_t m[16];
			for (int i = 0; i < 16; ++i) { m[i] = load_little_32(a_block + i * 4); }
			uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
			for (int i = 0; i < 64; ++i) {
				uint32_t f = 0;
				int g = 0;
				if (i < 16) {
					f = (b & c) | (~b & d);
					g = i;
				} else if (i < 32) {
					f = (d & b) | (~d & c);
					g = (5 * i + 1) % 16;
				} else if (i < 48) {
					f = b ^ c ^ d;
					g = (3 * i + 5) % 16;
				} else {
					f = c ^ (b | ~d);
					g = (7 * i) % 16;
				}
				const uint32_t rotated = rotate_left_32(a + f + k_sines[i] + m[g], k_shifts[(i / 16) * 4 + i % 4]);
				a = d;
				d = c;
				c = b;
				b += rotated;
			}
			state[0] += a;
			state[1] += b;
			state[2] += c;
			state[3] += d;
		}

	private:
		uint32_t state[4] {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
	};

	template<class Digest>
	std::string digest_of(std::string_view a_bytes) {
		Digest digest;
		digest.update(a_bytes);
		return digest.finish();
	}

	// HMAC (RFC 2104) over a block digest
	template<class Digest>
	std::string hmac_of(std::string_view a_key, std::string_view a_message) {
		unsigned char key[Digest::k_block_size] {};
		if (a_key.size() > Digest::k_block_size) {
			const std::string hashed = digest_of<Digest>(a_key);
			std::memcpy(key, hashed.data(), hashed.size());
		} else {
			std::memcpy(key, a_key.data(), a_key.size());
		}
		unsigned char pad[Digest::k_block_size];
		Digest inner;
		for (size_t i = 0; i < Digest::k_block_size; ++i) { pad[i] = static_cast<unsigned char>(key[i] ^ 0x36); }
		inner.update(pad, sizeof(pad));
		inner.update(a_message);
		const std::string inner_hash = inner.finish();
		Digest outer;
		for (size_t i = 0; i < Digest::k_block_size; ++i) { pad[i] = static_cast<unsigned char>(key[i] ^ 0x5c); }
		outer.update(pad, sizeof(pad));
		outer.update(inner_hash);
		return outer.finish();
	}

	std::string lowercase_hex(std::string_view a_bytes) {
		static constexpr char k_hex_digits[] = "0123456789abcdef";
		std::string result(a_bytes.size() * 2, '0');
		for (size_t i = 0; i < a_bytes.size(); ++i) {
			const unsigned char byte = static_cast<unsigned char>(a_bytes[i]);
			result[2 * i] = k_hex_digits[byte >> 4];
			result[2 * i + 1] = k_hex_digits[byte & 15];
		}
		return result;
	}

	// Equal length and bytes, in time that depends on the length alone
	bool constant_time_equals(std::string_view a_left, std::string_view a_right) {
		if (a_left.size() != a_right.size()) { return false; }
		const volatile unsigned char* left = reinterpret_cast<const volatile unsigned char*>(a_left.data());
		const volatile unsigned char* right = reinterpret_cast<const volatile unsigned char*>(a_right.data());
		unsigned char difference = 0;
		for (size_t i = 0; i < a_left.size(); ++i) { difference |= static_cast<unsigned char>(left[i] ^ right[i]); }
		return difference == 0;
	}

	// The file streamed through SHA-256 in 64 KiB chunks into a_hex; null, else why it cannot be read (read_file's reasons)
	const char* sha256_file_hex(const std::string& a_path, std::string& a_hex) {
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::status(a_path, error);
		if (status.type() == std::filesystem::file_type::not_found) { return "not found"; }
		if (error) { return error == std::errc::permission_denied ? "permission denied" : "cannot be opened"; }
		if (!std::filesystem::is_regular_file(status)) { return "not a file"; }
		errno = 0;
		std::ifstream in(a_path, std::ios::binary);
		if (!in) { return errno == EACCES ? "permission denied" : "cannot be opened"; }
		constexpr size_t k_chunk = size_t {1} << 16;
		const auto chunk = std::make_unique<char[]>(k_chunk);
		sha256_digest digest;
		while (in) {
			in.read(chunk.get(), static_cast<std::streamsize>(k_chunk));
			digest.update(reinterpret_cast<const unsigned char*>(chunk.get()), static_cast<size_t>(in.gcount()));
		}
		if (in.bad()) { return "read failed"; }
		a_hex = lowercase_hex(digest.finish());
		return nullptr;
	}

	// bcrypt (Provos, Mazieres 1999) as OpenBSD and crypt_blowfish (PHP's password_hash) compute it
	class bcrypt {
	public:
		static constexpr int k_default_cost = 12;
		static constexpr size_t k_hash_size = 60;

		enum class variant : unsigned char { a = 'a', b = 'b', x = 'x', y = 'y' };

		// The 60-character hash of a_password under a 22-character salt setting; the password's first 72 bytes count
		static std::string hash(std::string_view a_password, variant a_variant, int a_cost, const uint32_t (&a_salt)[4]) {
			blowfish state;
			uint32_t expanded[18];
			set_key(a_password, a_variant, state.p, expanded);
			std::memcpy(state.s, k_initial + 18, sizeof(state.s));
			state.expand(a_salt);
			for (uint64_t round = uint64_t {1} << a_cost; round > 0; --round) {
				for (int i = 0; i < 18; ++i) { state.p[i] ^= expanded[i]; }
				state.chain();
				for (int i = 0; i < 18; ++i) { state.p[i] ^= a_salt[i % 4]; }
				state.chain();
			}
			unsigned char output[24];
			for (int i = 0; i < 6; i += 2) {
				uint32_t left = k_magic[i], right = k_magic[i + 1];
				for (int n = 0; n < 64; ++n) { state.encipher(left, right); }
				store_word(left, output + 4 * i);
				store_word(right, output + 4 * i + 4);
			}
			unsigned char salt_bytes[16];
			for (int i = 0; i < 4; ++i) { store_word(a_salt[i], salt_bytes + 4 * i); }
			std::string result = "$2";
			result.push_back(static_cast<char>(a_variant));
			result.push_back('$');
			result.push_back(static_cast<char>('0' + a_cost / 10));
			result.push_back(static_cast<char>('0' + a_cost % 10));
			result.push_back('$');
			encode(salt_bytes, 16, result);
			encode(output, 23, result);
			return result;
		}

		// A stored hash's variant, cost and salt: false for anything but $2a$, $2b$, $2x$ or $2y$, a two-digit cost from 04
		// to 31 and 53 characters of bcrypt's base64
		static bool parse(std::string_view a_hash, variant& a_variant, int& a_cost, uint32_t (&a_salt)[4]) {
			if (a_hash.size() != k_hash_size || a_hash[0] != '$' || a_hash[1] != '2' || a_hash[3] != '$' || a_hash[6] != '$') { return false; }
			if (a_hash[2] != 'a' && a_hash[2] != 'b' && a_hash[2] != 'x' && a_hash[2] != 'y') { return false; }
			if (a_hash[4] < '0' || a_hash[4] > '9' || a_hash[5] < '0' || a_hash[5] > '9') { return false; }
			a_variant = static_cast<variant>(a_hash[2]);
			a_cost = (a_hash[4] - '0') * 10 + (a_hash[5] - '0');
			if (a_cost < 4 || a_cost > 31) { return false; }
			for (size_t i = 7; i < k_hash_size; ++i) {
				if (decode_digit(a_hash[i]) < 0) { return false; }
			}
			unsigned char salt_bytes[16];
			decode(a_hash.substr(7, 22), salt_bytes, 16);
			for (int i = 0; i < 4; ++i) { a_salt[i] = load_big_32(salt_bytes + 4 * i); }
			return true;
		}

		static void salt_from_bytes(const unsigned char (&a_bytes)[16], uint32_t (&a_salt)[4]) {
			for (int i = 0; i < 4; ++i) { a_salt[i] = load_big_32(a_bytes + 4 * i); }
		}

	private:
		static constexpr uint32_t k_magic[6] = {0x4f727068, 0x65616e42, 0x65686f6c, 0x64657253, 0x63727944, 0x6f756274};   // "OrpheanBeholderScryDoubt"
		static constexpr char k_alphabet[] = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

		// The P-array and S-boxes start as the fractional hex digits of pi
		static constexpr uint32_t k_initial[18 + 1024] = {
		0x243f6a88, 0x85a308d3, 0x13198a2e, 0x03707344, 0xa4093822, 0x299f31d0, 0x082efa98, 0xec4e6c89,
		0x452821e6, 0x38d01377, 0xbe5466cf, 0x34e90c6c, 0xc0ac29b7, 0xc97c50dd, 0x3f84d5b5, 0xb5470917,
		0x9216d5d9, 0x8979fb1b, 0xd1310ba6, 0x98dfb5ac, 0x2ffd72db, 0xd01adfb7, 0xb8e1afed, 0x6a267e96,
		0xba7c9045, 0xf12c7f99, 0x24a19947, 0xb3916cf7, 0x0801f2e2, 0x858efc16, 0x636920d8, 0x71574e69,
		0xa458fea3, 0xf4933d7e, 0x0d95748f, 0x728eb658, 0x718bcd58, 0x82154aee, 0x7b54a41d, 0xc25a59b5,
		0x9c30d539, 0x2af26013, 0xc5d1b023, 0x286085f0, 0xca417918, 0xb8db38ef, 0x8e79dcb0, 0x603a180e,
		0x6c9e0e8b, 0xb01e8a3e, 0xd71577c1, 0xbd314b27, 0x78af2fda, 0x55605c60, 0xe65525f3, 0xaa55ab94,
		0x57489862, 0x63e81440, 0x55ca396a, 0x2aab10b6, 0xb4cc5c34, 0x1141e8ce, 0xa15486af, 0x7c72e993,
		0xb3ee1411, 0x636fbc2a, 0x2ba9c55d, 0x741831f6, 0xce5c3e16, 0x9b87931e, 0xafd6ba33, 0x6c24cf5c,
		0x7a325381, 0x28958677, 0x3b8f4898, 0x6b4bb9af, 0xc4bfe81b, 0x66282193, 0x61d809cc, 0xfb21a991,
		0x487cac60, 0x5dec8032, 0xef845d5d, 0xe98575b1, 0xdc262302, 0xeb651b88, 0x23893e81, 0xd396acc5,
		0x0f6d6ff3, 0x83f44239, 0x2e0b4482, 0xa4842004, 0x69c8f04a, 0x9e1f9b5e, 0x21c66842, 0xf6e96c9a,
		0x670c9c61, 0xabd388f0, 0x6a51a0d2, 0xd8542f68, 0x960fa728, 0xab5133a3, 0x6eef0b6c, 0x137a3be4,
		0xba3bf050, 0x7efb2a98, 0xa1f1651d, 0x39af0176, 0x66ca593e, 0x82430e88, 0x8cee8619, 0x456f9fb4,
		0x7d84a5c3, 0x3b8b5ebe, 0xe06f75d8, 0x85c12073, 0x401a449f, 0x56c16aa6, 0x4ed3aa62, 0x363f7706,
		0x1bfedf72, 0x429b023d, 0x37d0d724, 0xd00a1248, 0xdb0fead3, 0x49f1c09b, 0x075372c9, 0x80991b7b,
		0x25d479d8, 0xf6e8def7, 0xe3fe501a, 0xb6794c3b, 0x976ce0bd, 0x04c006ba, 0xc1a94fb6, 0x409f60c4,
		0x5e5c9ec2, 0x196a2463, 0x68fb6faf, 0x3e6c53b5, 0x1339b2eb, 0x3b52ec6f, 0x6dfc511f, 0x9b30952c,
		0xcc814544, 0xaf5ebd09, 0xbee3d004, 0xde334afd, 0x660f2807, 0x192e4bb3, 0xc0cba857, 0x45c8740f,
		0xd20b5f39, 0xb9d3fbdb, 0x5579c0bd, 0x1a60320a, 0xd6a100c6, 0x402c7279, 0x679f25fe, 0xfb1fa3cc,
		0x8ea5e9f8, 0xdb3222f8, 0x3c7516df, 0xfd616b15, 0x2f501ec8, 0xad0552ab, 0x323db5fa, 0xfd238760,
		0x53317b48, 0x3e00df82, 0x9e5c57bb, 0xca6f8ca0, 0x1a87562e, 0xdf1769db, 0xd542a8f6, 0x287effc3,
		0xac6732c6, 0x8c4f5573, 0x695b27b0, 0xbbca58c8, 0xe1ffa35d, 0xb8f011a0, 0x10fa3d98, 0xfd2183b8,
		0x4afcb56c, 0x2dd1d35b, 0x9a53e479, 0xb6f84565, 0xd28e49bc, 0x4bfb9790, 0xe1ddf2da, 0xa4cb7e33,
		0x62fb1341, 0xcee4c6e8, 0xef20cada, 0x36774c01, 0xd07e9efe, 0x2bf11fb4, 0x95dbda4d, 0xae909198,
		0xeaad8e71, 0x6b93d5a0, 0xd08ed1d0, 0xafc725e0, 0x8e3c5b2f, 0x8e7594b7, 0x8ff6e2fb, 0xf2122b64,
		0x8888b812, 0x900df01c, 0x4fad5ea0, 0x688fc31c, 0xd1cff191, 0xb3a8c1ad, 0x2f2f2218, 0xbe0e1777,
		0xea752dfe, 0x8b021fa1, 0xe5a0cc0f, 0xb56f74e8, 0x18acf3d6, 0xce89e299, 0xb4a84fe0, 0xfd13e0b7,
		0x7cc43b81, 0xd2ada8d9, 0x165fa266, 0x80957705, 0x93cc7314, 0x211a1477, 0xe6ad2065, 0x77b5fa86,
		0xc75442f5, 0xfb9d35cf, 0xebcdaf0c, 0x7b3e89a0, 0xd6411bd3, 0xae1e7e49, 0x00250e2d, 0x2071b35e,
		0x226800bb, 0x57b8e0af, 0x2464369b, 0xf009b91e, 0x5563911d, 0x59dfa6aa, 0x78c14389, 0xd95a537f,
		0x207d5ba2, 0x02e5b9c5, 0x83260376, 0x6295cfa9, 0x11c81968, 0x4e734a41, 0xb3472dca, 0x7b14a94a,
		0x1b510052, 0x9a532915, 0xd60f573f, 0xbc9bc6e4, 0x2b60a476, 0x81e67400, 0x08ba6fb5, 0x571be91f,
		0xf296ec6b, 0x2a0dd915, 0xb6636521, 0xe7b9f9b6, 0xff34052e, 0xc5855664, 0x53b02d5d, 0xa99f8fa1,
		0x08ba4799, 0x6e85076a, 0x4b7a70e9, 0xb5b32944, 0xdb75092e, 0xc4192623, 0xad6ea6b0, 0x49a7df7d,
		0x9cee60b8, 0x8fedb266, 0xecaa8c71, 0x699a17ff, 0x5664526c, 0xc2b19ee1, 0x193602a5, 0x75094c29,
		0xa0591340, 0xe4183a3e, 0x3f54989a, 0x5b429d65, 0x6b8fe4d6, 0x99f73fd6, 0xa1d29c07, 0xefe830f5,
		0x4d2d38e6, 0xf0255dc1, 0x4cdd2086, 0x8470eb26, 0x6382e9c6, 0x021ecc5e, 0x09686b3f, 0x3ebaefc9,
		0x3c971814, 0x6b6a70a1, 0x687f3584, 0x52a0e286, 0xb79c5305, 0xaa500737, 0x3e07841c, 0x7fdeae5c,
		0x8e7d44ec, 0x5716f2b8, 0xb03ada37, 0xf0500c0d, 0xf01c1f04, 0x0200b3ff, 0xae0cf51a, 0x3cb574b2,
		0x25837a58, 0xdc0921bd, 0xd19113f9, 0x7ca92ff6, 0x94324773, 0x22f54701, 0x3ae5e581, 0x37c2dadc,
		0xc8b57634, 0x9af3dda7, 0xa9446146, 0x0fd0030e, 0xecc8c73e, 0xa4751e41, 0xe238cd99, 0x3bea0e2f,
		0x3280bba1, 0x183eb331, 0x4e548b38, 0x4f6db908, 0x6f420d03, 0xf60a04bf, 0x2cb81290, 0x24977c79,
		0x5679b072, 0xbcaf89af, 0xde9a771f, 0xd9930810, 0xb38bae12, 0xdccf3f2e, 0x5512721f, 0x2e6b7124,
		0x501adde6, 0x9f84cd87, 0x7a584718, 0x7408da17, 0xbc9f9abc, 0xe94b7d8c, 0xec7aec3a, 0xdb851dfa,
		0x63094366, 0xc464c3d2, 0xef1c1847, 0x3215d908, 0xdd433b37, 0x24c2ba16, 0x12a14d43, 0x2a65c451,
		0x50940002, 0x133ae4dd, 0x71dff89e, 0x10314e55, 0x81ac77d6, 0x5f11199b, 0x043556f1, 0xd7a3c76b,
		0x3c11183b, 0x5924a509, 0xf28fe6ed, 0x97f1fbfa, 0x9ebabf2c, 0x1e153c6e, 0x86e34570, 0xeae96fb1,
		0x860e5e0a, 0x5a3e2ab3, 0x771fe71c, 0x4e3d06fa, 0x2965dcb9, 0x99e71d0f, 0x803e89d6, 0x5266c825,
		0x2e4cc978, 0x9c10b36a, 0xc6150eba, 0x94e2ea78, 0xa5fc3c53, 0x1e0a2df4, 0xf2f74ea7, 0x361d2b3d,
		0x1939260f, 0x19c27960, 0x5223a708, 0xf71312b6, 0xebadfe6e, 0xeac31f66, 0xe3bc4595, 0xa67bc883,
		0xb17f37d1, 0x018cff28, 0xc332ddef, 0xbe6c5aa5, 0x65582185, 0x68ab9802, 0xeecea50f, 0xdb2f953b,
		0x2aef7dad, 0x5b6e2f84, 0x1521b628, 0x29076170, 0xecdd4775, 0x619f1510, 0x13cca830, 0xeb61bd96,
		0x0334fe1e, 0xaa0363cf, 0xb5735c90, 0x4c70a239, 0xd59e9e0b, 0xcbaade14, 0xeecc86bc, 0x60622ca7,
		0x9cab5cab, 0xb2f3846e, 0x648b1eaf, 0x19bdf0ca, 0xa02369b9, 0x655abb50, 0x40685a32, 0x3c2ab4b3,
		0x319ee9d5, 0xc021b8f7, 0x9b540b19, 0x875fa099, 0x95f7997e, 0x623d7da8, 0xf837889a, 0x97e32d77,
		0x11ed935f, 0x16681281, 0x0e358829, 0xc7e61fd6, 0x96dedfa1, 0x7858ba99, 0x57f584a5, 0x1b227263,
		0x9b83c3ff, 0x1ac24696, 0xcdb30aeb, 0x532e3054, 0x8fd948e4, 0x6dbc3128, 0x58ebf2ef, 0x34c6ffea,
		0xfe28ed61, 0xee7c3c73, 0x5d4a14d9, 0xe864b7e3, 0x42105d14, 0x203e13e0, 0x45eee2b6, 0xa3aaabea,
		0xdb6c4f15, 0xfacb4fd0, 0xc742f442, 0xef6abbb5, 0x654f3b1d, 0x41cd2105, 0xd81e799e, 0x86854dc7,
		0xe44b476a, 0x3d816250, 0xcf62a1f2, 0x5b8d2646, 0xfc8883a0, 0xc1c7b6a3, 0x7f1524c3, 0x69cb7492,
		0x47848a0b, 0x5692b285, 0x095bbf00, 0xad19489d, 0x1462b174, 0x23820e00, 0x58428d2a, 0x0c55f5ea,
		0x1dadf43e, 0x233f7061, 0x3372f092, 0x8d937e41, 0xd65fecf1, 0x6c223bdb, 0x7cde3759, 0xcbee7460,
		0x4085f2a7, 0xce77326e, 0xa6078084, 0x19f8509e, 0xe8efd855, 0x61d99735, 0xa969a7aa, 0xc50c06c2,
		0x5a04abfc, 0x800bcadc, 0x9e447a2e, 0xc3453484, 0xfdd56705, 0x0e1e9ec9, 0xdb73dbd3, 0x105588cd,
		0x675fda79, 0xe3674340, 0xc5c43465, 0x713e38d8, 0x3d28f89e, 0xf16dff20, 0x153e21e7, 0x8fb03d4a,
		0xe6e39f2b, 0xdb83adf7, 0xe93d5a68, 0x948140f7, 0xf64c261c, 0x94692934, 0x411520f7, 0x7602d4f7,
		0xbcf46b2e, 0xd4a20068, 0xd4082471, 0x3320f46a, 0x43b7d4b7, 0x500061af, 0x1e39f62e, 0x97244546,
		0x14214f74, 0xbf8b8840, 0x4d95fc1d, 0x96b591af, 0x70f4ddd3, 0x66a02f45, 0xbfbc09ec, 0x03bd9785,
		0x7fac6dd0, 0x31cb8504, 0x96eb27b3, 0x55fd3941, 0xda2547e6, 0xabca0a9a, 0x28507825, 0x530429f4,
		0x0a2c86da, 0xe9b66dfb, 0x68dc1462, 0xd7486900, 0x680ec0a4, 0x27a18dee, 0x4f3ffea2, 0xe887ad8c,
		0xb58ce006, 0x7af4d6b6, 0xaace1e7c, 0xd3375fec, 0xce78a399, 0x406b2a42, 0x20fe9e35, 0xd9f385b9,
		0xee39d7ab, 0x3b124e8b, 0x1dc9faf7, 0x4b6d1856, 0x26a36631, 0xeae397b2, 0x3a6efa74, 0xdd5b4332,
		0x6841e7f7, 0xca7820fb, 0xfb0af54e, 0xd8feb397, 0x454056ac, 0xba489527, 0x55533a3a, 0x20838d87,
		0xfe6ba9b7, 0xd096954b, 0x55a867bc, 0xa1159a58, 0xcca92963, 0x99e1db33, 0xa62a4a56, 0x3f3125f9,
		0x5ef47e1c, 0x9029317c, 0xfdf8e802, 0x04272f70, 0x80bb155c, 0x05282ce3, 0x95c11548, 0xe4c66d22,
		0x48c1133f, 0xc70f86dc, 0x07f9c9ee, 0x41041f0f, 0x404779a4, 0x5d886e17, 0x325f51eb, 0xd59bc0d1,
		0xf2bcc18f, 0x41113564, 0x257b7834, 0x602a9c60, 0xdff8e8a3, 0x1f636c1b, 0x0e12b4c2, 0x02e1329e,
		0xaf664fd1, 0xcad18115, 0x6b2395e0, 0x333e92e1, 0x3b240b62, 0xeebeb922, 0x85b2a20e, 0xe6ba0d99,
		0xde720c8c, 0x2da2f728, 0xd0127845, 0x95b794fd, 0x647d0862, 0xe7ccf5f0, 0x5449a36f, 0x877d48fa,
		0xc39dfd27, 0xf33e8d1e, 0x0a476341, 0x992eff74, 0x3a6f6eab, 0xf4f8fd37, 0xa812dc60, 0xa1ebddf8,
		0x991be14c, 0xdb6e6b0d, 0xc67b5510, 0x6d672c37, 0x2765d43b, 0xdcd0e804, 0xf1290dc7, 0xcc00ffa3,
		0xb5390f92, 0x690fed0b, 0x667b9ffb, 0xcedb7d9c, 0xa091cf0b, 0xd9155ea3, 0xbb132f88, 0x515bad24,
		0x7b9479bf, 0x763bd6eb, 0x37392eb3, 0xcc115979, 0x8026e297, 0xf42e312d, 0x6842ada7, 0xc66a2b3b,
		0x12754ccc, 0x782ef11c, 0x6a124237, 0xb79251e7, 0x06a1bbe6, 0x4bfb6350, 0x1a6b1018, 0x11caedfa,
		0x3d25bdd8, 0xe2e1c3c9, 0x44421659, 0x0a121386, 0xd90cec6e, 0xd5abea2a, 0x64af674e, 0xda86a85f,
		0xbebfe988, 0x64e4c3fe, 0x9dbc8057, 0xf0f7c086, 0x60787bf8, 0x6003604d, 0xd1fd8346, 0xf6381fb0,
		0x7745ae04, 0xd736fccc, 0x83426b33, 0xf01eab71, 0xb0804187, 0x3c005e5f, 0x77a057be, 0xbde8ae24,
		0x55464299, 0xbf582e61, 0x4e58f48f, 0xf2ddfda2, 0xf474ef38, 0x8789bdc2, 0x5366f9c3, 0xc8b38e74,
		0xb475f255, 0x46fcd9b9, 0x7aeb2661, 0x8b1ddf84, 0x846a0e79, 0x915f95e2, 0x466e598e, 0x20b45770,
		0x8cd55591, 0xc902de4c, 0xb90bace1, 0xbb8205d0, 0x11a86248, 0x7574a99e, 0xb77f19b6, 0xe0a9dc09,
		0x662d09a1, 0xc4324633, 0xe85a1f02, 0x09f0be8c, 0x4a99a025, 0x1d6efe10, 0x1ab93d1d, 0x0ba5a4df,
		0xa186f20f, 0x2868f169, 0xdcb7da83, 0x573906fe, 0xa1e2ce9b, 0x4fcd7f52, 0x50115e01, 0xa70683fa,
		0xa002b5c4, 0x0de6d027, 0x9af88c27, 0x773f8641, 0xc3604c06, 0x61a806b5, 0xf0177a28, 0xc0f586e0,
		0x006058aa, 0x30dc7d62, 0x11e69ed7, 0x2338ea63, 0x53c2dd94, 0xc2c21634, 0xbbcbee56, 0x90bcb6de,
		0xebfc7da1, 0xce591d76, 0x6f05e409, 0x4b7c0188, 0x39720a3d, 0x7c927c24, 0x86e3725f, 0x724d9db9,
		0x1ac15bb4, 0xd39eb8fc, 0xed545578, 0x08fca5b5, 0xd83d7cd3, 0x4dad0fc4, 0x1e50ef5e, 0xb161e6f8,
		0xa28514d9, 0x6c51133c, 0x6fd5c7e7, 0x56e14ec4, 0x362abfce, 0xddc6c837, 0xd79a3234, 0x92638212,
		0x670efa8e, 0x406000e0, 0x3a39ce37, 0xd3faf5cf, 0xabc27737, 0x5ac52d1b, 0x5cb0679e, 0x4fa33742,
		0xd3822740, 0x99bc9bbe, 0xd5118e9d, 0xbf0f7315, 0xd62d1c7e, 0xc700c47b, 0xb78c1b6b, 0x21a19045,
		0xb26eb1be, 0x6a366eb4, 0x5748ab2f, 0xbc946e79, 0xc6a376d2, 0x6549c2c8, 0x530ff8ee, 0x468dde7d,
		0xd5730a1d, 0x4cd04dc6, 0x2939bbdb, 0xa9ba4650, 0xac9526e8, 0xbe5ee304, 0xa1fad5f0, 0x6a2d519a,
		0x63ef8ce2, 0x9a86ee22, 0xc089c2b8, 0x43242ef6, 0xa51e03aa, 0x9cf2d0a4, 0x83c061ba, 0x9be96a4d,
		0x8fe51550, 0xba645bd6, 0x2826a2f9, 0xa73a3ae1, 0x4ba99586, 0xef5562e9, 0xc72fefd3, 0xf752f7da,
		0x3f046f69, 0x77fa0a59, 0x80e4a915, 0x87b08601, 0x9b09e6ad, 0x3b3ee593, 0xe990fd5a, 0x9e34d797,
		0x2cf0b7d9, 0x022b8b51, 0x96d5ac3a, 0x017da67d, 0xd1cf3ed6, 0x7c7d2d28, 0x1f9f25cf, 0xadf2b89b,
		0x5ad6b472, 0x5a88f54c, 0xe029ac71, 0xe019a5e6, 0x47b0acfd, 0xed93fa9b, 0xe8d3c48d, 0x283b57cc,
		0xf8d56629, 0x79132e28, 0x785f0191, 0xed756055, 0xf7960e44, 0xe3d35e8c, 0x15056dd4, 0x88f46dba,
		0x03a16125, 0x0564f0bd, 0xc3eb9e15, 0x3c9057a2, 0x97271aec, 0xa93a072a, 0x1b3f6d9b, 0x1e6321f5,
		0xf59c66fb, 0x26dcf319, 0x7533d928, 0xb155fdf5, 0x03563482, 0x8aba3cbb, 0x28517711, 0xc20ad9f8,
		0xabcc5167, 0xccad925f, 0x4de81751, 0x3830dc8e, 0x379d5862, 0x9320f991, 0xea7a90c2, 0xfb3e7bce,
		0x5121ce64, 0x774fbe32, 0xa8b6e37e, 0xc3293d46, 0x48de5369, 0x6413e680, 0xa2ae0810, 0xdd6db224,
		0x69852dfd, 0x09072166, 0xb39a460a, 0x6445c0dd, 0x586cdecf, 0x1c20c8ae, 0x5bbef7dd, 0x1b588d40,
		0xccd2017f, 0x6bb4e3bb, 0xdda26a7e, 0x3a59ff45, 0x3e350a44, 0xbcb4cdd5, 0x72eacea8, 0xfa6484bb,
		0x8d6612ae, 0xbf3c6f47, 0xd29be463, 0x542f5d9e, 0xaec2771b, 0xf64e6370, 0x740e0d8d, 0xe75b1357,
		0xf8721671, 0xaf537d5d, 0x4040cb08, 0x4eb4e2cc, 0x34d2466a, 0x0115af84, 0xe1b00428, 0x95983a1d,
		0x06b89fb4, 0xce6ea048, 0x6f3f3b82, 0x3520ab82, 0x011a1d4b, 0x277227f8, 0x611560b1, 0xe7933fdc,
		0xbb3a792b, 0x344525bd, 0xa08839e1, 0x51ce794b, 0x2f32c9b7, 0xa01fbac9, 0xe01cc87e, 0xbcc7d1f6,
		0xcf0111c3, 0xa1e8aac7, 0x1a908749, 0xd44fbd9a, 0xd0dadecb, 0xd50ada38, 0x0339c32a, 0xc6913667,
		0x8df9317c, 0xe0b12b4f, 0xf79e59b7, 0x43f5bb3a, 0xf2d519ff, 0x27d9459c, 0xbf97222c, 0x15e6fc2a,
		0x0f91fc71, 0x9b941525, 0xfae59361, 0xceb69ceb, 0xc2a86459, 0x12baa8d1, 0xb6c1075e, 0xe3056a0c,
		0x10d25065, 0xcb03a442, 0xe0ec6e0e, 0x1698db3b, 0x4c98a0be, 0x3278e964, 0x9f1f9532, 0xe0d392df,
		0xd3a0342b, 0x8971f21e, 0x1b0a7441, 0x4ba3348c, 0xc5be7120, 0xc37632d8, 0xdf359f8d, 0x9b992f2e,
		0xe60b6f47, 0x0fe3f11d, 0xe54cda54, 0x1edad891, 0xce6279cf, 0xcd3e7e6f, 0x1618b166, 0xfd2c1d05,
		0x848fd2c5, 0xf6fb2299, 0xf523f357, 0xa6327623, 0x93a83531, 0x56cccd02, 0xacf08162, 0x5a75ebb5,
		0x6e163697, 0x88d273cc, 0xde966292, 0x81b949d0, 0x4c50901b, 0x71c65614, 0xe6c6c7bd, 0x327a140a,
		0x45e1d006, 0xc3f27b9a, 0xc9aa53fd, 0x62a80f00, 0xbb25bfe2, 0x35bdd2f6, 0x71126905, 0xb2040222,
		0xb6cbcf7c, 0xcd769c2b, 0x53113ec0, 0x1640e3d3, 0x38abbd60, 0x2547adf0, 0xba38209c, 0xf746ce76,
		0x77afa1c5, 0x20756060, 0x85cbfe4e, 0x8ae88dd8, 0x7aaaf9b0, 0x4cf9aa7e, 0x1948c25c, 0x02fb8a8c,
		0x01c36ae4, 0xd6ebe1f9, 0x90d4f869, 0xa65cdea0, 0x3f09252d, 0xc208e69f, 0xb74e6132, 0xce77e25b,
		0x578fdfe3, 0x3ac372e6,
		};

		struct blowfish {
			uint32_t p[18];
			uint32_t s[4][256];

			uint32_t round_function(uint32_t a_x) const {
				return ((s[0][a_x >> 24] + s[1][(a_x >> 16) & 0xFF]) ^ s[2][(a_x >> 8) & 0xFF]) + s[3][a_x & 0xFF];
			}
			void encipher(uint32_t& a_left, uint32_t& a_right) const {
				uint32_t left = a_left, right = a_right;
				for (int i = 0; i < 16; i += 2) {
					left ^= p[i];
					right ^= round_function(left);
					right ^= p[i + 1];
					left ^= round_function(right);
				}
				a_left = right ^ p[17];
				a_right = left ^ p[16];
			}
			// The keyed P-array encrypts a running block, salted, through P and every S-box (EksBlowfishSetup's first pass)
			void expand(const uint32_t (&a_salt)[4]) {
				uint32_t left = 0, right = 0;
				size_t salt_word = 0;
				auto step = [&](uint32_t& a_first, uint32_t& a_second) {
					left ^= a_salt[salt_word];
					right ^= a_salt[salt_word + 1];
					salt_word ^= 2;
					encipher(left, right);
					a_first = left;
					a_second = right;
				};
				for (int i = 0; i < 18; i += 2) { step(p[i], p[i + 1]); }
				for (auto& box : s) {
					for (int i = 0; i < 256; i += 2) { step(box[i], box[i + 1]); }
				}
			}
			// The same walk with no salt (ExpandKey with a zero salt)
			void chain() {
				uint32_t left = 0, right = 0;
				for (int i = 0; i < 18; i += 2) {
					encipher(left, right);
					p[i] = left;
					p[i + 1] = right;
				}
				for (auto& box : s) {
					for (int i = 0; i < 256; i += 2) {
						encipher(left, right);
						box[i] = left;
						box[i + 1] = right;
					}
				}
			}
		};

		// The password's bytes as 18 key words, cycling through it and its terminating NUL; $2x$ reproduces the sign-extension
		// bug of crypt_blowfish before 1.1, and $2a$ its countermeasure for the passwords that bug affected
		static void set_key(std::string_view a_password, variant a_variant, uint32_t (&a_initial)[18], uint32_t (&a_expanded)[18]) {
			const bool bug = a_variant == variant::x;
			const uint32_t safety = a_variant == variant::a ? 0x10000 : 0;
			uint32_t sign = 0, difference = 0;
			size_t at = 0;
			const size_t length = std::min<size_t>(a_password.size(), 72);
			for (int i = 0; i < 18; ++i) {
				uint32_t correct = 0, buggy = 0;
				for (int j = 0; j < 4; ++j) {
					const unsigned char byte = at < length ? static_cast<unsigned char>(a_password[at]) : 0;
					correct = (correct << 8) | byte;
					buggy = (buggy << 8) | static_cast<uint32_t>(static_cast<int32_t>(static_cast<signed char>(byte)));
					if (j != 0) { sign |= buggy & 0x80; }
					at = at < length ? at + 1 : 0;
				}
				difference |= correct ^ buggy;
				a_expanded[i] = bug ? buggy : correct;
				a_initial[i] = k_initial[i] ^ a_expanded[i];
			}
			difference |= difference >> 16;
			difference &= 0xFFFF;
			difference += 0xFFFF;
			sign <<= 9;
			sign &= ~difference & safety;
			a_initial[0] ^= sign;
		}

		static void store_word(uint32_t a_word, unsigned char* a_out) {
			a_out[0] = static_cast<unsigned char>(a_word >> 24);
			a_out[1] = static_cast<unsigned char>(a_word >> 16);
			a_out[2] = static_cast<unsigned char>(a_word >> 8);
			a_out[3] = static_cast<unsigned char>(a_word);
		}

		static int decode_digit(char a_c) {
			if (a_c == '.') { return 0; }
			if (a_c == '/') { return 1; }
			if (a_c >= 'A' && a_c <= 'Z') { return a_c - 'A' + 2; }
			if (a_c >= 'a' && a_c <= 'z') { return a_c - 'a' + 28; }
			if (a_c >= '0' && a_c <= '9') { return a_c - '0' + 54; }
			return -1;
		}

		// bcrypt's base64: the ordinary bit grouping over its own alphabet, unpadded
		static void encode(const unsigned char* a_bytes, size_t a_size, std::string& a_out) {
			for (size_t i = 0; i < a_size; i += 3) {
				const uint32_t first = a_bytes[i];
				a_out.push_back(k_alphabet[first >> 2]);
				if (i + 1 >= a_size) {
					a_out.push_back(k_alphabet[(first & 0x03) << 4]);
					break;
				}
				const uint32_t second = a_bytes[i + 1];
				a_out.push_back(k_alphabet[((first & 0x03) << 4) | (second >> 4)]);
				if (i + 2 >= a_size) {
					a_out.push_back(k_alphabet[(second & 0x0F) << 2]);
					break;
				}
				const uint32_t third = a_bytes[i + 2];
				a_out.push_back(k_alphabet[((second & 0x0F) << 2) | (third >> 6)]);
				a_out.push_back(k_alphabet[third & 0x3F]);
			}
		}

		static void decode(std::string_view a_text, unsigned char* a_out, size_t a_size) {
			size_t at = 0, written = 0;
			while (written < a_size) {
				const uint32_t c1 = static_cast<uint32_t>(decode_digit(a_text[at++]));
				const uint32_t c2 = static_cast<uint32_t>(decode_digit(a_text[at++]));
				a_out[written++] = static_cast<unsigned char>((c1 << 2) | ((c2 & 0x30) >> 4));
				if (written >= a_size) { break; }
				const uint32_t c3 = static_cast<uint32_t>(decode_digit(a_text[at++]));
				a_out[written++] = static_cast<unsigned char>(((c2 & 0x0F) << 4) | ((c3 & 0x3C) >> 2));
				if (written >= a_size) { break; }
				const uint32_t c4 = static_cast<uint32_t>(decode_digit(a_text[at++]));
				a_out[written++] = static_cast<unsigned char>(((c3 & 0x03) << 6) | c4);
			}
		}
	};

	using function_result = checked_result<script_value>;

	// Every text a crypto:: function raises is a literal: a failure on a parallel worker keeps its text (no symbol is minted there)
	struct entry_texts {
		const char* name;
		const char* count;
		const char* type;
	};

	JAI_NOINLINE function_result count_error(const entry_texts& a_texts) {
		return function_result(make_error_code(runtime_error_code::argument_count_mismatch), a_texts.count);
	}
	JAI_NOINLINE function_result type_error(const entry_texts& a_texts) {
		return function_result(make_error_code(runtime_error_code::type_mismatch), a_texts.type);
	}
	JAI_NOINLINE function_result value_error(const char* a_text) {
		return function_result(make_error_code(runtime_error_code::invalid_operation), a_text);
	}
	JAI_NOINLINE function_result unreadable_file_error(engine* a_engine, const std::string& a_path, const char* a_reason) {
		return function_result(make_error_code(runtime_error_code::file_not_found), "{0}", detail::engine_access::park_error_text(*a_engine, "Cannot read file '" + a_path + "': " + a_reason));
	}

	// n random bytes from the operating system for random_bytes and random_hex
	function_result random_bytes_of(engine* a_engine, const entry_texts& a_texts, const char* a_negative, const char* a_failed, const script_value& a_count, std::string& a_out) {
		if (!a_count.is_int()) { return type_error(a_texts); }
		const script_int count = a_count.as_int();
		if (count < 0) { return value_error(a_negative); }
		if (!detail::builtin_memory_charge(a_engine, static_cast<size_t>(count))) [[unlikely]] { return detail::builtin_memory_cap(a_engine); }
		if (static_cast<uint64_t>(count) > std::string().max_size() - 64) { throw std::length_error("random_bytes"); }
		a_out.assign(static_cast<size_t>(count), '\0');
		if (!detail::os_random_bytes(a_out.data(), a_out.size())) { return value_error(a_failed); }
		return script_value(std::monostate{}, a_engine);
	}

	// The text arguments of a call (a string or a utf8_string each, their bytes), else the error to raise
	template<size_t Count>
	bool text_arguments(const std::vector<script_value>& a_raw, std::string_view (&a_texts)[Count]) {
		for (size_t i = 0; i < Count; ++i) {
			const std::string* bytes = a_raw[i].deref().text_bytes();
			if (!bytes) { return false; }
			a_texts[i] = *bytes;
		}
		return true;
	}

} // namespace crypto_detail

	void register_crypto_functions(engine& eng_ref) {
		using namespace crypto_detail;
		engine* eng = &eng_ref;
		detail::engine_access::serial_function(eng_ref, "crypto::sha256_file");
		auto& ns = detail::engine_access::script_namespaces(eng_ref)[detail::engine_access::symbolize(eng_ref, "crypto")];
		if (!ns) { ns = std::make_shared<script_namespace_data>(); }
		using svt = script_value_type;
		auto define = [&](const char* a_name, uint8_t a_min, uint8_t a_max, std::vector<svt> a_params, svt a_result, script_function a_function) {
			host_entry_signature signature;
			signature.min_args = a_min;
			signature.max_args = a_max;
			signature.params = std::move(a_params);
			signature.result = a_result;
			const uint64_t id = detail::engine_access::symbolize(eng_ref, a_name);
			ns->variables[id] = script_value::make_function(std::move(a_function), &eng_ref);
			ns->signatures[id] = std::move(signature);
		};
		// a function of one text: its hex digest and its raw bytes
		auto digest_pair = [&](const entry_texts& a_hex, const entry_texts& a_bytes, auto a_digest) {
			for (const bool raw : {false, true}) {
				const entry_texts& texts = raw ? a_bytes : a_hex;
				define(texts.name, 1, 1, {svt::jai_string_type}, svt::jai_string_type, [eng, texts, raw, a_digest](const std::vector<script_value>& a_raw) -> function_result {
					if (a_raw.size() != 1) { return count_error(texts); }
					std::string_view text[1];
					if (!text_arguments(a_raw, text)) { return type_error(texts); }
					return script_value(raw ? a_digest(text[0]) : lowercase_hex(a_digest(text[0])), eng);
				});
			}
		};
		digest_pair({"sha256", "crypto::sha256 expects 1 argument (text)", "crypto::sha256 expects a string"},
		            {"sha256_bytes", "crypto::sha256_bytes expects 1 argument (text)", "crypto::sha256_bytes expects a string"},
		            [](std::string_view a_text) { return digest_of<sha256_digest>(a_text); });
		digest_pair({"sha512", "crypto::sha512 expects 1 argument (text)", "crypto::sha512 expects a string"},
		            {"sha512_bytes", "crypto::sha512_bytes expects 1 argument (text)", "crypto::sha512_bytes expects a string"},
		            [](std::string_view a_text) { return digest_of<sha512_digest>(a_text); });
		digest_pair({"sha1", "crypto::sha1 expects 1 argument (text)", "crypto::sha1 expects a string"},
		            {"sha1_bytes", "crypto::sha1_bytes expects 1 argument (text)", "crypto::sha1_bytes expects a string"},
		            [](std::string_view a_text) { return digest_of<sha1_digest>(a_text); });
		digest_pair({"md5", "crypto::md5 expects 1 argument (text)", "crypto::md5 expects a string"},
		            {"md5_bytes", "crypto::md5_bytes expects 1 argument (text)", "crypto::md5_bytes expects a string"},
		            [](std::string_view a_text) { return digest_of<md5_digest>(a_text); });

		auto hmac_pair = [&](const entry_texts& a_hex, const entry_texts& a_bytes, auto a_hmac) {
			for (const bool raw : {false, true}) {
				const entry_texts& texts = raw ? a_bytes : a_hex;
				define(texts.name, 2, 2, {svt::jai_string_type, svt::jai_string_type}, svt::jai_string_type, [eng, texts, raw, a_hmac](const std::vector<script_value>& a_raw) -> function_result {
					if (a_raw.size() != 2) { return count_error(texts); }
					std::string_view text[2];
					if (!text_arguments(a_raw, text)) { return type_error(texts); }
					return script_value(raw ? a_hmac(text[0], text[1]) : lowercase_hex(a_hmac(text[0], text[1])), eng);
				});
			}
		};
		hmac_pair({"hmac_sha256", "crypto::hmac_sha256 expects 2 arguments (key, message)", "crypto::hmac_sha256 expects strings"},
		          {"hmac_sha256_bytes", "crypto::hmac_sha256_bytes expects 2 arguments (key, message)", "crypto::hmac_sha256_bytes expects strings"},
		          [](std::string_view a_key, std::string_view a_message) { return hmac_of<sha256_digest>(a_key, a_message); });
		hmac_pair({"hmac_sha512", "crypto::hmac_sha512 expects 2 arguments (key, message)", "crypto::hmac_sha512 expects strings"},
		          {"hmac_sha512_bytes", "crypto::hmac_sha512_bytes expects 2 arguments (key, message)", "crypto::hmac_sha512_bytes expects strings"},
		          [](std::string_view a_key, std::string_view a_message) { return hmac_of<sha512_digest>(a_key, a_message); });

		static constexpr entry_texts k_equals {"equals", "crypto::equals expects 2 arguments (a, b)", "crypto::equals expects strings"};
		define("equals", 2, 2, {svt::jai_string_type, svt::jai_string_type}, svt::jai_bool_type, [eng](const std::vector<script_value>& a_raw) -> function_result {
			if (a_raw.size() != 2) { return count_error(k_equals); }
			std::string_view text[2];
			if (!text_arguments(a_raw, text)) { return type_error(k_equals); }
			return script_value(constant_time_equals(text[0], text[1]), eng);
		});

		static constexpr entry_texts k_sha256_file {"sha256_file", "crypto::sha256_file expects 1 argument (path)", "crypto::sha256_file expects a string path"};
		define("sha256_file", 1, 1, {svt::jai_string_type}, svt::jai_string_type, detail::engine_access::guard_function(eng_ref, [eng](const std::vector<script_value>& a_raw) -> function_result {
			if (a_raw.size() != 1) { return count_error(k_sha256_file); }
			const std::string* path = a_raw[0].deref().text_bytes();
			if (!path) { return type_error(k_sha256_file); }
			std::string hex;
			if (const char* reason = sha256_file_hex(*path, hex)) { return unreadable_file_error(eng, *path, reason); }
			return script_value(std::move(hex), eng);
		}, permissions::filesystem));

		static constexpr entry_texts k_random_bytes {"random_bytes", "crypto::random_bytes expects 1 argument (count)", "crypto::random_bytes expects an int count"};
		define("random_bytes", 1, 1, {svt::jai_int_type}, svt::jai_string_type, [eng](const std::vector<script_value>& a_raw) -> function_result {
			if (a_raw.size() != 1) { return count_error(k_random_bytes); }
			std::string bytes;
			if (auto made = random_bytes_of(eng, k_random_bytes, "crypto::random_bytes count cannot be negative",
			                                "crypto::random_bytes: the operating system's secure random generator failed", a_raw[0].deref(), bytes); !made) { return made; }
			return script_value(std::move(bytes), eng);
		});
		static constexpr entry_texts k_random_hex {"random_hex", "crypto::random_hex expects 1 argument (count of bytes)", "crypto::random_hex expects an int count"};
		define("random_hex", 1, 1, {svt::jai_int_type}, svt::jai_string_type, [eng](const std::vector<script_value>& a_raw) -> function_result {
			if (a_raw.size() != 1) { return count_error(k_random_hex); }
			std::string bytes;
			if (auto made = random_bytes_of(eng, k_random_hex, "crypto::random_hex count cannot be negative",
			                                "crypto::random_hex: the operating system's secure random generator failed", a_raw[0].deref(), bytes); !made) { return made; }
			return script_value(lowercase_hex(bytes), eng);
		});
		static constexpr entry_texts k_random_int {"random_int", "crypto::random_int expects 2 arguments (lo, hi)", "crypto::random_int expects int bounds"};
		define("random_int", 2, 2, {svt::jai_int_type, svt::jai_int_type}, svt::jai_int_type, [eng](const std::vector<script_value>& a_raw) -> function_result {
			if (a_raw.size() != 2) { return count_error(k_random_int); }
			const script_value& lo = a_raw[0].deref();
			const script_value& hi = a_raw[1].deref();
			if (!lo.is_int() || !hi.is_int()) { return type_error(k_random_int); }
			if (hi.as_int() < lo.as_int()) { return value_error("crypto::random_int range is inverted (max < min)"); }
			bool failed = false;
			const int64_t result = detail::uniform_between(lo.as_int(), hi.as_int(), [&failed]() {
				uint64_t word = 0;
				failed = failed || !detail::os_random_bytes(&word, sizeof(word));
				return word;
			});
			if (failed) { return value_error("crypto::random_int: the operating system's secure random generator failed"); }
			return script_value(static_cast<script_int>(result), eng);
		});

		static constexpr entry_texts k_password_hash {"password_hash", "crypto::password_hash expects 1 or 2 arguments (password, [cost])", "crypto::password_hash expects a string password and an int cost"};
		define("password_hash", 1, 2, {svt::jai_string_type, svt::jai_int_type}, svt::jai_string_type, [eng](const std::vector<script_value>& a_raw) -> function_result {
			if (a_raw.empty() || a_raw.size() > 2) { return count_error(k_password_hash); }
			const std::string* password = a_raw[0].deref().text_bytes();
			if (!password || (a_raw.size() == 2 && !a_raw[1].deref().is_int())) { return type_error(k_password_hash); }
			int cost = bcrypt::k_default_cost;
			if (a_raw.size() == 2) {
				const script_int given = a_raw[1].deref().as_int();
				if (given < 4 || given > 31) { return value_error("crypto::password_hash cost must be 4 to 31"); }
				cost = static_cast<int>(given);
			}
			if (password->find('\0') != std::string::npos) { return value_error("crypto::password_hash: the password contains a NUL byte"); }
			unsigned char salt_bytes[16];
			if (!detail::os_random_bytes(salt_bytes, sizeof(salt_bytes))) { return value_error("crypto::password_hash: the operating system's secure random generator failed"); }
			uint32_t salt[4];
			bcrypt::salt_from_bytes(salt_bytes, salt);
			return script_value(bcrypt::hash(*password, bcrypt::variant::y, cost, salt), eng);
		});
		static constexpr entry_texts k_password_verify {"password_verify", "crypto::password_verify expects 2 arguments (password, hash)", "crypto::password_verify expects strings"};
		define("password_verify", 2, 2, {svt::jai_string_type, svt::jai_string_type}, svt::jai_bool_type, [eng](const std::vector<script_value>& a_raw) -> function_result {
			if (a_raw.size() != 2) { return count_error(k_password_verify); }
			std::string_view text[2];
			if (!text_arguments(a_raw, text)) { return type_error(k_password_verify); }
			bcrypt::variant variant = bcrypt::variant::y;
			int cost = 0;
			uint32_t salt[4];
			if (text[0].find('\0') != std::string_view::npos || !bcrypt::parse(text[1], variant, cost, salt)) { return script_value(false, eng); }
			return script_value(constant_time_equals(bcrypt::hash(text[0], variant, cost, salt), text[1]), eng);
		});
	}

} // namespace stdlib
} // namespace jai
