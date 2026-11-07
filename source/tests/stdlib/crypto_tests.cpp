#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace jai::foundry::tests {

class crypto_tests : public suite {
public:
	crypto_tests() : suite("Crypto") {}

	void forge_tests() override {
		auto es = [this](const std::string& a_source) { auto engine = make_engine(); stdlib::register_all(*engine); return engine->execute(a_source).as<std::string>(); };
		auto caught = [es](const std::string& a_source) { return es("string m = \"\"; try { " + a_source + " } catch (e) { m = e; } m;"); };

		test("sha256_matches_the_nist_vectors", [this, es]() {
			check_eq(std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), es("crypto::sha256(\"\");"));
			check_eq(std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), es("crypto::sha256(\"abc\");"));
			check_eq(std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), es("crypto::sha256(\"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq\");"));
			check_eq(std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"),
				es("crypto::sha256(\"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu\");"));
			check_eq(std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), es("crypto::sha256(string::repeat(\"a\", 1000000));"));
		});

		test("sha256_pads_at_every_block_boundary_and_hashes_bytes", [this, es]() {
			const std::pair<int, const char*> lengths[] = {
				{55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"}, {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
				{63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"}, {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
				{65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"}, {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
				{120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"}, {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"},
			};
			for (const auto& [length, digest] : lengths) {
				check_eq(std::string(digest), es("crypto::sha256(string::repeat(\"a\", " + std::to_string(length) + "));"), std::to_string(length));
			}
			check_eq(std::string("2c8d07cd986f58eb210bd800133d6645c7340c59865377c8ea431cebca0b3113"), es("crypto::sha256(\"\\x00\\xFF\\x00\");"));
			check_eq(std::string("4a99557e4033c3539de2eb65472017cad5f9557f7a0625a09f1c3f6e2ba69c4c 4a99557e4033c3539de2eb65472017cad5f9557f7a0625a09f1c3f6e2ba69c4c"),
				es("crypto::sha256(utf8_string(\"\xC3\xA9\")) + \" \" + crypto::sha256(\"\xC3\xA9\");"));
			check_eq(std::string("string"), es("type_of(crypto::sha256(\"a\"));"));
		});

		test("sha512_matches_the_nist_vectors_at_every_block_boundary", [this, es]() {
			check_eq(std::string("cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"), es("crypto::sha512(\"\");"));
			check_eq(std::string("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"), es("crypto::sha512(\"abc\");"));
			check_eq(std::string("8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"),
				es("crypto::sha512(\"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu\");"));
			check_eq(std::string("e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b"),
				es("crypto::sha512(string::repeat(\"a\", 1000000));"));
			const std::pair<int, const char*> lengths[] = {
				{111, "fa9121c7b32b9e01733d034cfc78cbf67f926c7ed83e82200ef86818196921760b4beff48404df811b953828274461673c68d04e297b0eb7b2b4d60fc6b566a2"},
				{112, "c01d080efd492776a1c43bd23dd99d0a2e626d481e16782e75d54c2503b5dc32bd05f0f1ba33e568b88fd2d970929b719ecbb152f58f130a407c8830604b70ca"},
				{127, "828613968b501dc00a97e08c73b118aa8876c26b8aac93df128502ab360f91bab50a51e088769a5c1eff4782ace147dce3642554199876374291f5d921629502"},
				{128, "b73d1929aa615934e61a871596b3f3b33359f42b8175602e89f7e06e5f658a243667807ed300314b95cacdd579f3e33abdfbe351909519a846d465c59582f321"},
				{239, "52c853cb8d907f3d4d6b889beb027985d7c273486d75f8baf26f80d24e90c74c6c3de3e22131582380a7d14d43f2941a31385439cd6ddc469f628015e50bf286"},
				{240, "4c296d90c61052a62ffb1dd196f1b7b09373b1f93e71836baebf89690546b7595684dbe9467a8e484fa0d1094272b4344a7c24f5fee8daedeb0bf549c985ab5f"},
			};
			for (const auto& [length, digest] : lengths) {
				check_eq(std::string(digest), es("crypto::sha512(string::repeat(\"a\", " + std::to_string(length) + "));"), std::to_string(length));
			}
		});

		test("sha1_and_md5_match_their_reference_vectors", [this, es]() {
			check_eq(std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"), es("crypto::sha1(\"\");"));
			check_eq(std::string("a9993e364706816aba3e25717850c26c9cd0d89d"), es("crypto::sha1(\"abc\");"));
			check_eq(std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"), es("crypto::sha1(\"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq\");"));
			check_eq(std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"), es("crypto::sha1(string::repeat(\"a\", 1000000));"));
			const std::pair<int, const char*> sha1_lengths[] = {
				{55, "c1c8bbdc22796e28c0e15163d20899b65621d65a"}, {56, "c2db330f6083854c99d4b5bfb6e8f29f201be699"}, {63, "03f09f5b158a7a8cdad920bddc29b81c18a551f5"},
				{64, "0098ba824b5c16427bd7a1122a5a442a25ec644d"}, {65, "11655326c708d70319be2610e8a57d9a5b959d3b"},
			};
			for (const auto& [length, digest] : sha1_lengths) {
				check_eq(std::string(digest), es("crypto::sha1(string::repeat(\"a\", " + std::to_string(length) + "));"), std::to_string(length));
			}
			const std::pair<const char*, const char*> rfc1321[] = {
				{"", "d41d8cd98f00b204e9800998ecf8427e"}, {"a", "0cc175b9c0f1b6a831c399e269772661"}, {"abc", "900150983cd24fb0d6963f7d28e17f72"},
				{"message digest", "f96b697d7cb7938d525a2f31aaf161d0"}, {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
				{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f"},
				{"12345678901234567890123456789012345678901234567890123456789012345678901234567890", "57edf4a22be3c955ac49da2e2107b67a"},
			};
			for (const auto& [text, digest] : rfc1321) {
				check_eq(std::string(digest), es(std::string("crypto::md5(\"") + text + "\");"), text);
			}
			const std::pair<int, const char*> md5_lengths[] = {
				{55, "ef1772b6dff9a122358552954ad0df65"}, {56, "3b0c8ac703f828b04c6c197006d17218"}, {63, "b06521f39153d618550606be297466d5"},
				{64, "014842d480b571495a4a0363793f7367"}, {65, "c743a45e0d2e6a95cb859adae0248435"},
			};
			for (const auto& [length, digest] : md5_lengths) {
				check_eq(std::string(digest), es("crypto::md5(string::repeat(\"a\", " + std::to_string(length) + "));"), std::to_string(length));
			}
		});

		test("hmac_matches_rfc_4231", [this, es]() {
			struct rfc_case { const char* key; const char* data; const char* sha256; const char* sha512; };
			const rfc_case cases[] = {
				{"string::repeat(\"\\x0b\", 20)", "\"Hi There\"", "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
				 "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854"},
				{"\"Jefe\"", "\"what do ya want for nothing?\"", "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
				 "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737"},
				{"string::repeat(\"\\xaa\", 20)", "string::repeat(\"\\xdd\", 50)", "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
				 "fa73b0089d56a284efb0f0756c890be9b1b5dbdd8ee81a3655f83e33b2279d39bf3e848279a722c806b485a47e67c807b946a337bee8942674278859e13292fb"},
				{"\"\\x01\\x02\\x03\\x04\\x05\\x06\\x07\\x08\\x09\\x0a\\x0b\\x0c\\x0d\\x0e\\x0f\\x10\\x11\\x12\\x13\\x14\\x15\\x16\\x17\\x18\\x19\"", "string::repeat(\"\\xcd\", 50)",
				 "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b",
				 "b0ba465637458c6990e5a8c5f61d4af7e576d97ff94b872de76f8050361ee3dba91ca5c11aa25eb4d679275cc5788063a5f19741120c4f2de2adebeb10a298dd"},
				{"string::repeat(\"\\x0c\", 20)", "\"Test With Truncation\"", "a3b6167473100ee06e0c796c2955552bfa6f7c0a6a8aef8b93f860aab0cd20c5",
				 "415fad6271580a531d4179bc891d87a650188707922a4fbb36663a1eb16da008711c5b50ddd0fc235084eb9d3364a1454fb2ef67cd1d29fe6773068ea266e96b"},
				{"string::repeat(\"\\xaa\", 131)", "\"Test Using Larger Than Block-Size Key - Hash Key First\"", "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
				 "80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f3526b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598"},
				{"string::repeat(\"\\xaa\", 131)", "\"This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm.\"",
				 "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
				 "e37b6a775dc87dbaa4dfa9f96e5e3ffddebd71f8867289865df5a32d20cdc944b6022cac3c4982b10d5eeb55c3e4de15134676fb6de0446065c97440fa8c6a58"},
			};
			int number = 1;
			for (const auto& c : cases) {
				const std::string arguments = std::string(c.key) + ", " + c.data;
				check_eq(std::string(c.sha256), es("crypto::hmac_sha256(" + arguments + ");"), "case " + std::to_string(number));
				check_eq(std::string(c.sha512), es("crypto::hmac_sha512(" + arguments + ");"), "case " + std::to_string(number));
				++number;
			}
		});

		test("the_bytes_forms_are_the_raw_digests", [this, es]() {
			check_eq(std::string("32 64 20 16 32 64"), es("`${crypto::sha256_bytes(\"a\").size()} ${crypto::sha512_bytes(\"a\").size()} ${crypto::sha1_bytes(\"a\").size()} "
			                                             "${crypto::md5_bytes(\"a\").size()} ${crypto::hmac_sha256_bytes(\"k\", \"a\").size()} ${crypto::hmac_sha512_bytes(\"k\", \"a\").size()}`;"));
			check_eq(std::string("true"), es(R"(
				function hex_of(string bytes) -> string {
					string digits = "0123456789abcdef";
					string out = "";
					for (int b : bytes.to_bytes()) { out += digits[b >> 4]; out += digits[b & 15]; }
					return out;
				}
				to_string(hex_of(crypto::sha256_bytes("abc")) == crypto::sha256("abc") && hex_of(crypto::sha512_bytes("abc")) == crypto::sha512("abc") &&
				          hex_of(crypto::sha1_bytes("abc")) == crypto::sha1("abc") && hex_of(crypto::md5_bytes("abc")) == crypto::md5("abc") &&
				          hex_of(crypto::hmac_sha256_bytes("Jefe", "what do ya want for nothing?")) == crypto::hmac_sha256("Jefe", "what do ya want for nothing?") &&
				          hex_of(crypto::hmac_sha512_bytes("Jefe", "what do ya want for nothing?")) == crypto::hmac_sha512("Jefe", "what do ya want for nothing?"));)"));
		});

		test("equals_compares_length_and_every_byte", [this, es]() {
			check_eq(std::string("true false false false true true"),
				es("`${crypto::equals(\"abc\", \"abc\")} ${crypto::equals(\"abc\", \"abd\")} ${crypto::equals(\"ab\", \"abc\")} "
				   "${crypto::equals(\"\\x00a\", \"\\x00b\")} ${crypto::equals(\"\", \"\")} ${crypto::equals(utf8_string(\"\xC3\xA9\"), \"\xC3\xA9\")}`;"));
		});

		test("random_bytes_and_random_int_come_from_the_operating_system", [this, es, caught]() {
			check_eq(std::string("0 16 1000 32 true true"), es(R"(
				string a = crypto::random_bytes(32);
				string b = crypto::random_bytes(32);
				string h = crypto::random_hex(16);
				bool hex = true;
				for (int i = 0; i < h.size(); ++i) { if ("0123456789abcdef".find(h.substr(i, 1)) < 0) { hex = false; } }
				`${to_string(crypto::random_bytes(0).size())} ${crypto::random_bytes(16).size()} ${crypto::random_bytes(1000).size()} ${h.size()} ${(a != b)} ${hex}`;)"));
			check_eq(std::string("true 7"), es(R"(
				bool in_range = true;
				for (int i = 0; i < 500; ++i) { int n = crypto::random_int(-3, 3); if (n < -3 || n > 3) { in_range = false; } }
				int full = crypto::random_int(-9223372036854775807 - 1, 9223372036854775807);
				`${to_string(in_range)} ${crypto::random_int(7, 7)}`;)"));
			check_eq(std::string("crypto::random_bytes count cannot be negative"), caught("crypto::random_bytes(-1);"));
			check_eq(std::string("crypto::random_hex count cannot be negative"), caught("crypto::random_hex(-2);"));
			check_eq(std::string("crypto::random_int range is inverted (max < min)"), caught("crypto::random_int(5, 1);"));
		});

		test("password_verify_accepts_php_and_openwall_bcrypt_hashes", [this, es]() {
			const std::pair<const char*, const char*> accepted[] = {
				{"rasmuslerdorf", "$2y$10$.vGA1O9wmRjrwAVXD98HNOgsNpDczlqm3Jq7KnEd1rVAGv3Fykk1a"},   // the PHP manual's password_verify example
				{"U*U", "$2a$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW"},
				{"U*U*", "$2a$05$CCCCCCCCCCCCCCCCCCCCC.VGOzA784oUp/Z0DY336zx7pLYAy0lwK"},
				{"U*U*U", "$2a$05$XXXXXXXXXXXXXXXXXXXXXOAcXxm9kjPGEMsLznoKqmqw7tc8WCx4a"},
				{"", "$2a$05$CCCCCCCCCCCCCCCCCCCCC.7uG0VCzI2bS7j6ymqJi9CdcdxiRTWNy"},
				{"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789chars after 72 are ignored", "$2a$05$abcdefghijklmnopqrstuu5s2v8.iXieOjg/.AySBTTZIIVFJeBui"},
				{"\\xa3", "$2y$05$/OK.fbVrR/bpIqNJ5ianF.Sa7shbm4.OzKpvFnX1pQLmQW96oUlCq"},
				{"\\xa3", "$2a$05$/OK.fbVrR/bpIqNJ5ianF.Sa7shbm4.OzKpvFnX1pQLmQW96oUlCq"},
				{"\\xa3", "$2x$05$/OK.fbVrR/bpIqNJ5ianF.CE5elHaaO4EbggVDjb8P19RukzXSM3e"},
				{"\\xff\\xff\\xa3", "$2y$05$/OK.fbVrR/bpIqNJ5ianF.CE5elHaaO4EbggVDjb8P19RukzXSM3e"},
				{"\\xff\\xff\\xa3", "$2b$05$/OK.fbVrR/bpIqNJ5ianF.CE5elHaaO4EbggVDjb8P19RukzXSM3e"},
				{"\\xff\\xff\\xa3", "$2a$05$/OK.fbVrR/bpIqNJ5ianF.nqd1wy.pTMdcvrRWxyiGL2eMz.2a85."},   // crypt_blowfish's $2a$ countermeasure
				{"correct horse battery staple", "$2b$06$abcdefghijklmnopqrstuu10cNU3mXRamSUZYs8I8M/Szu..qXq5K"},
				{"\xC3\xA9t\xC3\xA9", "$2b$06$abcdefghijklmnopqrstuuzyKEQ1tOpJ6CAKLqnp/7ExaiWun3zhu"},
				{"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "$2b$06$abcdefghijklmnopqrstuueHLWckpI7Ut91D6aWthlKy4VKsT9yUG"},
				{"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxyyyyyyyyyyyyyyyyyyyyyyyyyyyy", "$2b$06$abcdefghijklmnopqrstuueHLWckpI7Ut91D6aWthlKy4VKsT9yUG"},
			};
			for (const auto& [password, hash] : accepted) {
				check_eq(std::string("true"), es(std::string("to_string(crypto::password_verify(\"") + password + "\", \"" + hash + "\"));"), hash);
			}
			const std::pair<const char*, const char*> refused[] = {
				{"U*U", "$2a$05$CCCCCCCCCCCCCCCCCCCCC.VGOzA784oUp/Z0DY336zx7pLYAy0lwK"},
				{"\\xff\\xff\\xa3", "$2a$05$/OK.fbVrR/bpIqNJ5ianF.CE5elHaaO4EbggVDjb8P19RukzXSM3e"},
				{"U*U", "$2a$05$CCCCCCCCCCCCCCCCCCCCC/E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW"},   // a salt whose last digit is not canonical
				{"U*U", "$2a$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOe"},
				{"U*U", "$2c$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW"},
				{"U*U", "$2a$03$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW"},
				{"U*U", "$2a$32$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW"},
				{"U*U", "$2a$5$$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW"},
				{"U*U", "$2a$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOe!"},
				{"U*U", "$argon2id$v=19$m=65536,t=4,p=1$c29tZXNhbHQ$RdescudvJCsgt3ub+b+dWRWJTmaaJObG"},
				{"U*U", ""},
				{"U*U\\x00tail", "$2a$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW"},
			};
			for (const auto& [password, hash] : refused) {
				check_eq(std::string("false"), es(std::string("to_string(crypto::password_verify(\"") + password + "\", \"" + hash + "\"));"), hash);
			}
		});

		test("password_hash_writes_php_compatible_bcrypt", [this, es, caught]() {
			check_eq(std::string("60 $2y$04$ true false true false"), es(R"(
				string h = crypto::password_hash("hunter2", 4);
				string again = crypto::password_hash("hunter2", 4);
				`${to_string(h.size())} ${h.substr(0, 7)} ${crypto::password_verify("hunter2", h)} ${crypto::password_verify("hunter3", h)} ${crypto::password_verify("hunter2", again)} ${(h == again)}`;)"));
			check_eq(std::string("$2y$12$"), es("crypto::password_hash(\"x\").substr(0, 7);"));
			check_eq(std::string("true"), es("string long = string::repeat(\"p\", 72); to_string(crypto::password_verify(`${long}ignored`, crypto::password_hash(long, 4)));"));
			check_eq(std::string("crypto::password_hash cost must be 4 to 31"), caught("crypto::password_hash(\"x\", 3);"));
			check_eq(std::string("crypto::password_hash cost must be 4 to 31"), caught("crypto::password_hash(\"x\", 32);"));
			check_eq(std::string("crypto::password_hash: the password contains a NUL byte"), caught("crypto::password_hash(\"a\\x00b\", 4);"));
		});

		test("arguments_are_checked_at_load_and_at_run_time", [this, caught]() {
			check_eq(std::string("crypto::sha256 expects a string"), caught("var n = 5; var f = crypto::sha256; f(n);"));
			check_eq(std::string("crypto::hmac_sha256 expects 2 arguments (key, message)"), caught("var f = crypto::hmac_sha256; f(\"k\");"));
			check_eq(std::string("crypto::random_bytes expects an int count"), caught("var c = \"x\"; crypto::random_bytes(c);"));
			auto engine = make_engine();
			stdlib::register_all(*engine);
			for (const char* source : {"crypto::sha256(5);", "crypto::sha256();", "crypto::md5(\"a\", \"b\");", "int n = crypto::sha1(\"a\");", "crypto::equals(\"a\");",
			                           "crypto::password_hash(\"x\", \"y\");", "crypto::random_int(1);", "crypto::sha256_file(1.5);"}) {
				bool rejected = false;
				try { engine->execute(source); } catch (const std::exception&) { rejected = true; }
				check_true(rejected, source);
			}
		});

		test("sha256_file_streams_the_file_and_names_what_it_cannot_read", [this, es, caught]() {
			namespace fs = std::filesystem;
			const fs::path dir = test_temp_directory() / "jai_sha256_file";
			std::error_code error;
			fs::remove_all(dir, error);
			fs::create_directories(dir);
			std::string pattern(200000, '\0');
			for (size_t i = 0; i < pattern.size(); ++i) { pattern[i] = static_cast<char>((i * 7 + 3) % 256); }
			std::ofstream(dir / "pattern.bin", std::ios::binary) << pattern;
			std::ofstream(dir / "empty.bin", std::ios::binary).flush();
			const std::string base = dir.generic_string();
			check_eq(std::string("ec0ebf98b6f2954bf0f7b839402b1ba245996c39d18e155414e91a2b4353c157"), es("crypto::sha256_file(\"" + base + "/pattern.bin\");"));
			check_eq(std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), es("crypto::sha256_file(utf8_string(\"" + base + "/empty.bin\"));"));
			check_eq("Cannot read file '" + base + "/missing.bin': not found", caught("crypto::sha256_file(\"" + base + "/missing.bin\");"));
			check_eq("Cannot read file '" + base + "': not a file", caught("crypto::sha256_file(\"" + base + "\");"));
			fs::remove_all(dir, error);
		});

		test("sha256_file_needs_the_filesystem_privilege_and_the_rest_do_not", [this]() {
			auto engine = make_engine();
			stdlib::register_all(*engine);
			engine->disable(privilege::filesystem);
			check_eq(std::string("Insufficient privilege: filesystem."), engine->execute("string m = \"\"; try { crypto::sha256_file(\"anything.bin\"); } catch (e) { m = e; } m;").as<std::string>());
			check_eq(std::string("ba7816bf 4"), engine->execute("`${crypto::sha256(\"abc\").substr(0, 8)} ${crypto::random_bytes(4).size()}`;").as<std::string>());
		});

		test("the_pure_functions_and_secure_randomness_run_in_parallel_bodies_and_sha256_file_stays_serial", [this, caught]() {
			const std::string source = R"(
				array<string> texts = [];
				for (int i = 0; i < 3000; ++i) { texts.push(`<${i}> & '${i}'`); }
				array<string> serial = [];
				for (auto t : texts) { serial.push(crypto::sha256(t) + crypto::hmac_sha512("k", t) + crypto::md5(t) + to_string(crypto::equals(t, "x"))); }
				var parallel = parallel_transform(texts, [](string t) { return crypto::sha256(t) + crypto::hmac_sha512("k", t) + crypto::md5(t) + to_string(crypto::equals(t, "x")); });
				array<string> looped = texts;
				parallel_for (auto& t : looped) { t = crypto::sha256(t) + crypto::hmac_sha512("k", t) + crypto::md5(t) + to_string(crypto::equals(t, "x")); }
				array<int> sizes = [0, 0, 0, 0, 0, 0, 0, 0];
				parallel_for (auto& n : sizes) { n = crypto::random_bytes(8).size() + crypto::random_int(0, 0); }
				`${to_string(to_json(parallel) == to_json(serial))} ${to_string(to_json(looped) == to_json(serial))} ${sizes.join(",")}`;)";
			for (size_t workers : {size_t(1), size_t(4), size_t(8)}) {
				auto engine = make_engine();
				stdlib::register_all(*engine);
				engine->parallel_thread_count(workers);
				check_eq(std::string("true true 8,8,8,8,8,8,8,8"), engine->execute(source).as<std::string>(), "workers " + std::to_string(workers));
			}
			check_true(caught("array<string> paths = [\"a\"]; parallel_for (auto& p : paths) { p = crypto::sha256_file(p); }")
			               .find("parallel_for: crypto::sha256_file cannot be called in a parallel body (it reads a file)") != std::string::npos);
		});
	}
};

} // namespace jai::foundry::tests

using crypto_tests = jai::foundry::tests::crypto_tests;
FOUNDRY_REGISTER(crypto_tests)
