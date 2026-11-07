#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/serialization/binary_archive.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// Each element costs >= 1 byte, so a count past remaining_bytes() must fail before reserve().
class binary_size_guard_tests : public suite {
public:
	binary_size_guard_tests() : suite("Binary Size Guard") {}

	void forge_tests() override {
		test("five_byte_blob_claiming_huge_array_throws_serialization_error", [this]() {
			auto eng = make_engine();
			std::vector<uint8_t> blob = { 0x03, 0xFF, 0xFF, 0xFF, 0xFF };
			serialization::binary_archive_reader reader(blob, eng.get());
			std::vector<int> values;
			check_throws<serialization_error>([&]() { reader(values); }, "array count past remaining bytes must throw serialization_error");
			check_eq((size_t)0, values.capacity(), "rejected before reserve");
		});

		test("five_byte_blob_claiming_huge_map_throws_serialization_error", [this]() {
			auto eng = make_engine();
			std::vector<uint8_t> blob = { 0x05, 0xFF, 0xFF, 0xFF, 0xFF };
			serialization::binary_archive_reader reader(blob, eng.get());
			std::map<std::string, int> values;
			check_throws<serialization_error>([&]() { reader(values); }, "map count past remaining bytes must throw serialization_error");
			check_true(values.empty());
		});

		test("array_count_one_past_remaining_bytes_is_rejected", [this]() {
			auto eng = make_engine();
			std::vector<uint8_t> blob = { 0x03, 0x02, 0x00, 0x00, 0x00, 0x00 };
			serialization::binary_archive_reader reader(blob, eng.get());
			check_throws<serialization_error>([&]() { (void)reader.begin_array(); }, "count 2 with 1 byte left is rejected");
		});

		test("array_count_within_remaining_bytes_is_accepted", [this]() {
			auto eng = make_engine();
			std::vector<uint8_t> blob = { 0x03, 0x01, 0x00, 0x00, 0x00, 0x00 };
			serialization::binary_archive_reader reader(blob, eng.get());
			check_eq((size_t)1, reader.begin_array(), "count 1 with 1 byte left passes the bound");
		});

		test("map_count_one_past_remaining_bytes_is_rejected", [this]() {
			auto eng = make_engine();
			std::vector<uint8_t> blob = { 0x05, 0x02, 0x00, 0x00, 0x00, 0x00 };
			serialization::binary_archive_reader reader(blob, eng.get());
			check_throws<serialization_error>([&]() { (void)reader.begin_map(); }, "count 2 with 1 byte left is rejected");
		});
	}
};

FOUNDRY_REGISTER(jai::foundry::tests::binary_size_guard_tests);

} // namespace jai::foundry::tests
