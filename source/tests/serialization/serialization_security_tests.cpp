// Security regression tests for deserialization of hostile input.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/serialization/binary_archive.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <jaiscript/detail/text_hash.hpp>

#include <map>
#include <set>
#include <memory>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

struct hostile_unique_node {
    int v = 0;
    std::unique_ptr<hostile_unique_node> child;
    template<typename Archive> void serialize(Archive& ar) { ar("v", v); ar("child", child); }
};

struct hostile_weak_node {
    int v = 0;
    std::weak_ptr<hostile_weak_node> w;
    template<typename Archive> void serialize(Archive& ar) { ar("v", v); ar("w", w); }
};

struct hostile_vector_node {
    int v = 0;
    std::vector<hostile_vector_node> kids;
    template<typename Archive> void serialize(Archive& ar) { ar("v", v); ar("kids", kids); }
};

struct hostile_inner { int secret = 0; template<typename Archive> void serialize(Archive& ar) { ar("secret", secret); } };
struct hostile_outer {
    int secret = 0;
    std::vector<hostile_inner> items;
    int after = 0;
    template<typename Archive> void serialize(Archive& ar) { ar("secret", secret); ar("items", items); ar("after", after); }
};

struct hostile_containers {
    std::vector<int> list;
    std::map<std::string, int> table;
    template<typename Archive> void serialize(Archive& ar) { ar("list", list); ar("table", table); }
};

// Eight-byte keys an attacker would craft against an unseeded text_hash: each one's hash, had the seed been the fixed constant,
// has its low 24 bits zero, so every key would share one bucket of any table up to 2^24 slots
inline std::vector<std::string> keys_colliding_without_a_seed(size_t count) {
    auto inverse = [](uint64_t a) { uint64_t x = a; for (int i = 0; i < 6; ++i) { x *= 2 - a * x; } return x; };
    auto unxorshift = [](uint64_t y, int shift) { uint64_t x = y; for (int i = 0; i < 64 / shift + 1; ++i) { x = y ^ (x >> shift); } return x; };
    std::vector<std::string> result;
    for (uint64_t target = 1; result.size() < count; ++target) {
        uint64_t h = target << 24;
        h = unxorshift(h, 33); h *= inverse(0xc4ceb9fe1a85ec53ull);
        h = unxorshift(h, 33); h *= inverse(0xff51afd7ed558ccdull);
        h = unxorshift(h, 33);
        h = unxorshift(h, 29); h *= inverse(0xbf58476d1ce4e5b9ull);
        const uint64_t word = h ^ (0x9E3779B97F4A7C15ull ^ (uint64_t(8) * 0xff51afd7ed558ccdull));
        std::string key(8, '\0');
        for (int i = 0; i < 8; ++i) { key[i] = char((word >> (8 * i)) & 0xff); }
        result.push_back(std::move(key));
    }
    return result;
}

template<typename Load>
std::string archive_load_error(Load&& load) {
    try {
        load();
        return "loaded";
    } catch (const serialization_error& e) {
        return e.what();
    } catch (const std::exception& e) {
        return std::string("not a serialization_error: ") + e.what();
    }
}

class serialization_security_tests : public suite {
public:
    serialization_security_tests() : suite("Serialization Security") {}

    void forge_tests() override {

        // A malicious `$type` value must NOT be parsed+executed as script source. The old
        // path did `execute(type_name + "()")`, so a `$type` of "g_flag = 99; Dummy" ran the
        // injected statement. The fix resolves the type through the registry (no eval).
        test("from_json_type_name_not_executed", [&]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->add_global("g_flag", script_value((script_int)0, eng.get()));
            eng->add_global("payload", std::string(R"({"$type":"g_flag = 99; Dummy","$val":{}})"));
            (void)eng->execute("from_json(payload)");
            check_eq((script_int)0, eng->execute("g_flag").as_int()); // injection must NOT fire
        });

        // A malicious property NAME must NOT be parsed+executed. The old path did
        // `execute(obj + "." + propName + " = " + tmp)`, so a key of "x = 1; g_flag = 99; z"
        // ran the injected statement. The fix sets fields through the symbolized setter, only
        // for fields the class actually declares.
        test("from_json_property_name_not_executed", [&]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->execute("class Dummy { int x = 0; }");
            eng->add_global("g_flag", script_value((script_int)0, eng.get()));
            eng->add_global("payload",
                std::string(R"({"$type":"Dummy","$val":{"x = 1; g_flag = 99; z":5}})"));
            (void)eng->execute("from_json(payload)");
            check_eq((script_int)0, eng->execute("g_flag").as_int()); // injection must NOT fire
        });

        // Same injection through the binary path (from_binary reconstructs `$type` maps too).
        // The blob is produced from a map, so we don't hand-craft binary bytes.
        test("from_binary_type_name_not_executed", [&]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->add_global("g_flag", script_value((script_int)0, eng.get()));
            eng->execute(R"( auto blob = to_binary({"$type": "g_flag = 99; Dummy", "$val": {}}); )");
            (void)eng->execute("from_binary(blob)");
            check_eq((script_int)0, eng->execute("g_flag").as_int());
        });

        test("from_base64_type_name_not_executed", [&]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->add_global("g_flag", script_value((script_int)0, eng.get()));
            eng->execute(R"( auto blob = to_base64({"$type": "g_flag = 99; Dummy", "$val": {}}); )");
            (void)eng->execute("from_base64(blob)");
            check_eq((script_int)0, eng->execute("g_flag").as_int());
        });

        // Legitimate object reconstruction must still work end-to-end (guards the fix against
        // regressing the real round-trip path).
        test("from_json_object_roundtrip_intact", [&]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            eng->execute(R"(
                class Pt { int x = 0; int y = 0; }
                auto p = Pt(); p.x = 11; p.y = 22;
                auto j = to_json(p);
                auto q = from_json(j);
            )");
            check_eq((script_int)11, eng->execute("q.x").as_int());
            check_eq((script_int)22, eng->execute("q.y").as_int());
        });

        // A length prefix bounded only by uint32 lets a ~5-byte blob claim 4 billion elements;
        // the reader must reject it against the remaining buffer, not reserve/loop into a
        // multi-GB allocation on the live scene-load path. B5.
        test("binary_oversized_array_length_rejected", [&]() {
            auto eng = make_engine();
            std::vector<uint8_t> blob = { 0x03, 0xFF, 0xFF, 0xFF, 0xFF }; // array marker + 4B count
            jai::serialization::binary_archive_reader r(blob.data(), blob.size(), eng.get());
            check_throws([&]() { (void)r.begin_array(); }, "oversized array length must be rejected");
        });

        // A scalar where a record, an array or a map belongs is refused: the readers used to report "no object" and go on
        // reading the enclosing record's keys, so the same path ran again until the stack overflowed (a 26-byte payload),
        // or an element read the outer record's fields
        test("wrong_kind_where_a_record_belongs_is_refused", [&]() {
            auto eng = make_engine();
            check_eq(std::string("Expected an object in the archive, found an integer"),
                archive_load_error([&]() { (void)from_json<hostile_unique_node>(*eng, R"({"v":1,"child":1,"$val":1})"); }));
            check_eq(std::string("Expected an object in the archive, found an integer"),
                archive_load_error([&]() { (void)from_json<hostile_weak_node>(*eng, R"({"v":1,"w":1,"$ref":1})"); }));
            check_eq(std::string("Expected an object in the archive, found an integer"),
                archive_load_error([&]() { (void)from_json<hostile_vector_node>(*eng, R"({"v":1,"kids":[1]})"); }));
            check_eq(std::string("Expected an object in the archive, found an integer"),
                archive_load_error([&]() { (void)from_json<hostile_outer>(*eng, R"({"secret":42,"items":[7],"after":9})"); }));
            check_eq(std::string("Expected an array in the archive, found an object"),
                archive_load_error([&]() { (void)from_json<hostile_containers>(*eng, R"({"list":{},"table":{}})"); }));
            check_eq(std::string("Expected a map in the archive, found an array"),
                archive_load_error([&]() { (void)from_json<hostile_containers>(*eng, R"({"list":[],"table":[1]})"); }));
            check_eq(std::string("Expected an object in the archive, found a string"),
                archive_load_error([&]() { (void)from_json<hostile_vector_node>(*eng, R"("v")"); }));

            auto put32 = [](std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) { b.push_back(uint8_t(v >> (8 * i))); } };
            auto putstr = [&](std::vector<uint8_t>& b, const std::string& t) { put32(b, uint32_t(t.size())); b.insert(b.end(), t.begin(), t.end()); };
            std::vector<uint8_t> blob;   // a root record whose "child" and "$val" sit at the end of the data
            blob.push_back(0x01); putstr(blob, ""); put32(blob, 1); put32(blob, 3);
            putstr(blob, "v"); putstr(blob, "child"); putstr(blob, "$val");
            put32(blob, 4); put32(blob, 0); put32(blob, 0);
            put32(blob, 1);
            check_eq(std::string("Expected an object in the archive, found the end of the data"),
                archive_load_error([&]() { (void)from_binary<hostile_unique_node>(*eng, blob); }));
            std::vector<uint8_t> wrong_marker = { 0x03, 0, 0, 0, 0, 0x04 };
            check_eq(std::string("Expected an object in the archive, found marker 3"),
                archive_load_error([&]() { (void)from_binary<hostile_unique_node>(*eng, wrong_marker); }));
            hostile_containers good;
            good.list = { 1, 2 };
            good.table["a"] = 3;
            auto round = from_json<hostile_containers>(*eng, to_json(*eng, good));
            check_eq(size_t(2), round.list.size());
            check_eq(3, round.table["a"]);
        });

        // The text hash starts from a per-process seed, so keys inverted against the fixed constant spread over the table
        test("text_hash_is_seeded_per_process", [&]() {
            std::set<uint64_t> buckets;
            for (const std::string& key : keys_colliding_without_a_seed(256)) { buckets.insert(jai::detail::text_hash(key) & 0xffff); }
            check_true(buckets.size() > 200, "crafted keys spread over " + std::to_string(buckets.size()) + " buckets of 65536");
        });

        test("binary_oversized_map_length_rejected", [&]() {
            auto eng = make_engine();
            std::vector<uint8_t> blob = { 0x05, 0xFF, 0xFF, 0xFF, 0xFF }; // map marker + 4B count
            jai::serialization::binary_archive_reader r(blob.data(), blob.size(), eng.get());
            check_throws([&]() { (void)r.begin_map(); }, "oversized map length must be rejected");
        });
    }
};

FOUNDRY_REGISTER(jai::foundry::tests::serialization_security_tests)

} // namespace jai::foundry::tests
