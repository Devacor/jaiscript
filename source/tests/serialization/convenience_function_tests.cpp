#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/properties/property.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/properties/macros.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/serialization/binary_archive.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/core/dynamic_binder_serialization.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <limits>
#include <cmath>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// These smart-pointer test types must be at namespace scope for JAI_PROPERTY.
struct sp_inner_data {
    int x = 0;
    std::string label;

    template<typename Archive>
    void save(Archive& ar) const {
        ar.serialize("x", x);
        ar.serialize("label", label);
    }

    template<typename Archive>
    void load(Archive& ar) {
        ar.serialize("x", x);
        ar.serialize("label", label);
    }
};

struct sp_simple_data {
    int value = 0;

    template<typename Archive>
    void save(Archive& ar) const {
        ar.serialize("value", value);
    }

    template<typename Archive>
    void load(Archive& ar) {
        ar.serialize("value", value);
    }
};

struct sp_id_data {
    int id = 0;

    template<typename Archive>
    void save(Archive& ar) const {
        ar.serialize("id", id);
    }

    template<typename Archive>
    void load(Archive& ar) {
        ar.serialize("id", id);
    }
};

struct sp_ptr_holder : public property_owner<sp_ptr_holder> {
    JAI_PROPERTY((std::shared_ptr<sp_simple_data>), data);
    JAI_PROPERTY((int), extra, 0);
};

// Polymorphic property_owner hierarchy (auto-registers, records the Derived->Base edge) used to
// prove the convenience to_*/from_* functions route an owning ROOT through the archive's $type
// path instead of slicing it to the static pointee type (B6).
class conv_poly_base : public property_owner<conv_poly_base> {
public:
    JAI_PROPERTY((int), baseVal, 1);
};

class conv_poly_derived : public property_owner<conv_poly_derived, conv_poly_base> {
public:
    JAI_PROPERTY((std::string), derivedTag, "");
};

class conv_player : public property_owner<conv_player> {
public:
    JAI_PROPERTY((int), health, 100);
    JAI_PROPERTY((float), speed, 5.5f);
    JAI_PROPERTY((std::string), name, "Player");

    conv_player() = default;
};

class conv_inventory : public property_owner<conv_inventory> {
public:
    JAI_PROPERTY((std::vector<std::string>), items);
    JAI_PROPERTY((std::map<std::string, int>), quantities);

    conv_inventory() = default;
};

class explicit_save_load_obj {
public:
    int x = 0;
    int y = 0;
    std::string label = "";

    template<typename Archive>
    void save(Archive& ar) const {
        ar.serialize("x", x);
        ar.serialize("y", y);
        ar.serialize("label", label);
    }

    template<typename Archive>
    void load(Archive& ar) {
        ar.serialize("x", x);
        ar.serialize("y", y);
        ar.serialize("label", label);
    }
};

// `friend jai::access;` is the one declaration a type needs for private serialization
// members — trait detection and dispatch both route through jai::access, cereal-style.
class private_serialize_friend_jai_access {
	friend jai::access;
	int hidden = 0;
	template<typename Archive>
	void serialize(Archive& ar) { ar.serialize("hidden", hidden); }
public:
	void set(int v) { hidden = v; }
	int get() const { return hidden; }
};

struct catalog_like_root_obj {
	std::vector<int64_t> data;
	template<typename Archive>
	void serialize(Archive& ar) {
		ar(jai::serialization::make_nvp("data", data));
	}
};

class member_serialize_obj {
public:
    double value = 0.0;
    std::string tag = "";

    template<typename Archive>
    void serialize(Archive& ar) {
        ar.serialize("value", value);
        ar.serialize("tag", tag);
    }

    template<typename Archive>
    void save(Archive& ar) const {
        ar.serialize("value", value);
        ar.serialize("tag", tag);
    }

    template<typename Archive>
    void load(Archive& ar) {
        ar.serialize("value", value);
        ar.serialize("tag", tag);
    }
};

class blended_object : public property_owner<blended_object> {
public:
    JAI_PROPERTY((int), score, 0);
    JAI_PROPERTY((std::string), player_name, "");

    int extra_data = 0;
    std::vector<float> history;

    blended_object() = default;

    template<typename Archive>
    void save(Archive& ar) const {
        property_mgr.save(ar);
        ar.serialize("extra_data", extra_data);
        ar.serialize("history", history);
    }

    template<typename Archive>
    void load(Archive& ar) {
        property_mgr.load(ar);
        ar.serialize("extra_data", extra_data);
        ar.serialize("history", history);
    }
};

struct conv_test_context {
    std::string service_name = "TestService";
    int multiplier = 1;
};

struct conv_context_scaled {
    int value = 0;

    template<typename Archive>
    void save(Archive& ar) const {
        auto* ctx = ar.template get_user_context<conv_test_context>();
        ar.serialize("value", value * (ctx ? ctx->multiplier : -1));
    }

    template<typename Archive>
    void load(Archive& ar) {
        ar.serialize("value", value);
    }
};

class convenience_function_tests : public suite {
public:
    convenience_function_tests() : suite("Convenience Functions") {}

    void forge_tests() override {
        test("private_serialize_friend_jai_access", [this]() {
            auto eng = make_engine();
            private_serialize_friend_jai_access obj;
            obj.set(42);
            jai::serialization::json_archive_writer writer(0, eng.get());
            writer.begin_object();
            writer("obj", obj);
            writer.end_object();
            private_serialize_friend_jai_access loaded;
            jai::serialization::json_archive_reader reader(writer.str(), eng.get());
            std::string rootType;
            uint32_t rootVersion = 0;
            reader.begin_object(rootType, rootVersion);
            reader("obj", loaded);
            reader.end_object();
            check_eq(42, loaded.get(), "friend jai::access grants the trait probes private access");
        });

        test("root_object_member_serialize_read", [this]() {
            // Reading a foreign root object {"data": [...]} into a member-serialize type:
            // ar(obj) is inline by design, so the caller enters the document root explicitly.
            auto eng = make_engine();
            std::string text = "{\"data\": [1, 2, 3]}";
            jai::serialization::json_archive_reader ar(text, eng.get());
            catalog_like_root_obj loaded;
            std::string rootType;
            uint32_t rootVersion = 0;
            ar.begin_object(rootType, rootVersion);
            loaded.serialize(ar);
            ar.end_object();
            check_eq(size_t(3), loaded.data.size(), "root object member serialize element count");
            check_eq(int64_t(2), loaded.data[1], "root object member serialize element value");
        });

        // from_json results must be first-class values: every key and value carries the
        // engine ref, so deep-copy (script assignment), arg passing, and C++ clone() all
        // work. The old reader minted map KEYS via ast_literal_tag (no engine) — copying
        // ANY deserialized map threw "Cannot clone script_value: missing engine pointer"
        // (bit the jaidoom save system, then the netplay dispatch).
        test("from_json_containers_clone_with_engine_refs", [this]() {
            auto eng = make_engine();
            jai::stdlib::register_all(*eng);
            auto r = eng->execute(R"(
                var flat = from_json("{\"x\": 1, \"y\": \"two\"}");
                var flat_copy = flat;
                flat_copy["x"] = 99;

                var nested = from_json("{\"a\": [1, 2, {\"b\": 3}], \"c\": {\"d\": [4, 5]}}");
                var nested_copy = nested;
                nested_copy["c"]["d"][0] = 40;

                function passes(var m) -> int { return m["a"][2]["b"]; }

                to_string(flat["x"]) + "|" + to_string(flat_copy["x"]) + "|" +
                    to_string(nested["c"]["d"][0]) + "|" + to_string(nested_copy["c"]["d"][0]) + "|" +
                    to_string(passes(nested));
            )");
            check_eq(std::string("1|99|4|40|3"), r.as_string(),
                "deserialized maps deep-copy independently and pass as args");
            // C++-side clone of the raw result (the exact call the crash sites hit)
            script_value direct = eng->execute("from_json(\"{\\\"k\\\": [1, {\\\"n\\\": 2}]}\")");
            script_value cloned = direct.clone();   // threw "missing engine pointer" before the key fix
            check_eq((int64_t)3, eng->execute("1 + 2").as_int(), "engine stays usable after C++ clone");
            // Typed-object reconstruction keys ("$type" probe) still resolve
            auto probe = eng->execute(R"(
                var m = from_json("{\"$type\": \"NoSuchRegisteredClass\", \"$val\": {\"v\": 7}}");
                to_string(m["$val"]["v"]) + "|" + m["$type"];
            )");
            check_eq(std::string("7|NoSuchRegisteredClass"), probe.as_string(),
                "string-content key ordering keeps $type lookup and map reads intact");
        });

        test("base64_raw_string_roundtrip", [this]() {
            std::string original = "Hello, World!";
            std::string encoded = base64_encode(original);
            std::string decoded = base64_decode(encoded);

            check_eq(decoded, original, "Raw base64 roundtrip");
            check_eq(encoded, std::string("SGVsbG8sIFdvcmxkIQ=="), "Expected base64 encoding");
        });

        test("base64_binary_data_with_nulls", [this]() {
            std::string binary_data;
            binary_data.push_back('\x00');
            binary_data.push_back('\x01');
            binary_data.push_back('\xFF');
            binary_data.push_back('\xFE');

            std::string encoded = base64_encode(binary_data);
            std::string decoded = base64_decode(encoded);

            check_eq(decoded.size(), binary_data.size(), "Binary size preserved");
            check_eq(decoded, binary_data, "Binary data preserved through base64");
        });

        test("json_basic_types", [this]() {
            auto eng = make_engine();

            std::string json_int = to_json(*eng, 42);
            check(json_int.find("42") != std::string::npos, "Int serialized");
            int i = from_json<int>(*eng, json_int);
            check_eq(i, 42, "Int roundtrip");

            std::string json_float = to_json(*eng, 3.14);
            check(json_float.find("3.14") != std::string::npos, "Float serialized");

            std::string json_str = to_json(*eng, std::string("hello world"));
            check(json_str.find("hello world") != std::string::npos, "String serialized");
            std::string s = from_json<std::string>(*eng, json_str);
            check_eq(s, std::string("hello world"), "String roundtrip");

            std::string json_bool = to_json(*eng, true);
            check(json_bool.find("true") != std::string::npos, "Bool serialized");
        });

        test("binary_basic_types", [this]() {
            auto eng = make_engine();

            std::string bin_int = to_binary_string(*eng, 12345);
            int i = from_binary_string<int>(*eng, bin_int);
            check_eq(i, 12345, "Int binary roundtrip");

            std::string bin_float = to_binary_string(*eng, 2.718);
            double d = from_binary_string<double>(*eng, bin_float);
            check(std::abs(d - 2.718) < 0.001, "Float binary roundtrip");

            std::string bin_str = to_binary_string(*eng, std::string("binary test"));
            std::string loaded_str = from_binary_string<std::string>(*eng, bin_str);
            check_eq(loaded_str, std::string("binary test"), "String binary roundtrip");
        });

        test("base64_serialization_basic", [this]() {
            auto eng = make_engine();

            std::string base64 = to_base64(*eng, 9999);

            for (char c : base64) {
                bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
                check(valid, "Base64 contains only valid characters");
            }

            int loaded = from_base64<int>(*eng, base64);
            check_eq(loaded, 9999, "Int base64 roundtrip");
        });

        test("property_owner_json_roundtrip", [this]() {
            auto eng = make_engine();
            stdlib::register_all(*eng);

            dynamic_binder<conv_player>(eng, "ConvPlayer")
                .build();

            auto original = std::make_shared<conv_player>();
            original->health = 75;
            original->speed = 10.0f;
            original->name = "Hero";

            serialization::json_archive_writer writer(2, eng.get());
            writer.begin_object("ConvPlayer", 1);
            original->property_mgr.save(writer);
            writer.end_object();
            std::string json = writer.str();

            check(json.find("75") != std::string::npos, "Health in JSON");
            check(json.find("Hero") != std::string::npos, "Name in JSON");

            // Deserialize
            conv_player loaded;
            serialization::json_archive_reader reader(json, eng.get());
            std::string type_name;
            uint32_t version;
            reader.begin_object(type_name, version);
            loaded.property_mgr.load(reader);
            reader.end_object();

            check_eq(loaded.health.get(), 75, "Health restored");
            check_eq(loaded.name.get(), std::string("Hero"), "Name restored");
            check(std::abs(loaded.speed.get() - 10.0f) < 0.001f, "Speed restored");
        });

        test("property_owner_binary_roundtrip", [this]() {
            auto eng = make_engine();
            stdlib::register_all(*eng);

            dynamic_binder<conv_player>(eng, "ConvPlayer").build();

            auto original = std::make_shared<conv_player>();
            original->health = 50;
            original->speed = 7.5f;
            original->name = "Binary Hero";

            std::vector<uint8_t> buffer;
            serialization::binary_archive_writer writer(buffer, eng.get());
            writer.begin_object("ConvPlayer", 1);
            original->property_mgr.save(writer);
            writer.end_object();

            conv_player loaded;
            serialization::binary_archive_reader reader(buffer, eng.get());
            std::string type_name;
            uint32_t version;
            reader.begin_object(type_name, version);
            loaded.property_mgr.load(reader);
            reader.end_object();

            check_eq(loaded.health.get(), 50, "Health from binary");
            check_eq(loaded.name.get(), std::string("Binary Hero"), "Name from binary");
        });

        test("property_owner_containers", [this]() {
            auto eng = make_engine();
            stdlib::register_all(*eng);

            dynamic_binder<conv_inventory>(eng, "ConvInventory").build();

            auto original = std::make_shared<conv_inventory>();
            original->items.get() = {"sword", "shield", "potion"};
            original->quantities.get() = {{"gold", 100}, {"gems", 5}};

            serialization::json_archive_writer writer(2, eng.get());
            writer.begin_object("ConvInventory", 1);
            original->property_mgr.save(writer);
            writer.end_object();
            std::string json = writer.str();

            conv_inventory loaded;
            serialization::json_archive_reader reader(json, eng.get());
            std::string type_name;
            uint32_t version;
            reader.begin_object(type_name, version);
            loaded.property_mgr.load(reader);
            reader.end_object();

            check_eq(loaded.items.get().size(), size_t(3), "Items count");
            check_eq(loaded.items.get()[0], std::string("sword"), "First item");
            check_eq(loaded.quantities.get()["gold"], 100, "Gold quantity");
        });

        test("explicit_save_load_json", [this]() {
            auto eng = make_engine();

            explicit_save_load_obj original;
            original.x = 10;
            original.y = 20;
            original.label = "test point";

            serialization::json_archive_writer writer(2, eng.get());
            writer.begin_object("ExplicitObj", 1);
            original.save(writer);
            writer.end_object();
            std::string json = writer.str();

            check(json.find("10") != std::string::npos, "x in JSON");
            check(json.find("20") != std::string::npos, "y in JSON");
            check(json.find("test point") != std::string::npos, "label in JSON");

            explicit_save_load_obj loaded;
            serialization::json_archive_reader reader(json, eng.get());
            std::string type_name;
            uint32_t version;
            reader.begin_object(type_name, version);
            loaded.load(reader);
            reader.end_object();

            check_eq(loaded.x, 10, "x restored");
            check_eq(loaded.y, 20, "y restored");
            check_eq(loaded.label, std::string("test point"), "label restored");
        });

        test("explicit_save_load_binary", [this]() {
            auto eng = make_engine();

            explicit_save_load_obj original;
            original.x = 100;
            original.y = 200;
            original.label = "binary point";

            std::vector<uint8_t> buffer;
            serialization::binary_archive_writer writer(buffer, eng.get());
            writer.begin_object("ExplicitObj", 1);
            original.save(writer);
            writer.end_object();

            explicit_save_load_obj loaded;
            serialization::binary_archive_reader reader(buffer, eng.get());
            std::string type_name;
            uint32_t version;
            reader.begin_object(type_name, version);
            loaded.load(reader);
            reader.end_object();

            check_eq(loaded.x, 100, "x from binary");
            check_eq(loaded.y, 200, "y from binary");
            check_eq(loaded.label, std::string("binary point"), "label from binary");
        });

        test("member_serialize_json", [this]() {
            auto eng = make_engine();

            member_serialize_obj original;
            original.value = 3.14159;
            original.tag = "pi";

            serialization::json_archive_writer writer(2, eng.get());
            writer.begin_object("MemberSerializeObj", 1);
            original.save(writer);
            writer.end_object();
            std::string json = writer.str();

            check(json.find("3.14159") != std::string::npos, "value in JSON");
            check(json.find("pi") != std::string::npos, "tag in JSON");

            member_serialize_obj loaded;
            serialization::json_archive_reader reader(json, eng.get());
            std::string type_name;
            uint32_t version;
            reader.begin_object(type_name, version);
            loaded.load(reader);
            reader.end_object();

            check(std::abs(loaded.value - 3.14159) < 0.00001, "value restored");
            check_eq(loaded.tag, std::string("pi"), "tag restored");
        });

        test("member_serialize_binary", [this]() {
            auto eng = make_engine();

            member_serialize_obj original;
            original.value = 2.71828;
            original.tag = "euler";

            std::vector<uint8_t> buffer;
            serialization::binary_archive_writer writer(buffer, eng.get());
            writer.begin_object("MemberSerializeObj", 1);
            original.save(writer);
            writer.end_object();

            member_serialize_obj loaded;
            serialization::binary_archive_reader reader(buffer, eng.get());
            std::string type_name;
            uint32_t version;
            reader.begin_object(type_name, version);
            loaded.load(reader);
            reader.end_object();

            check(std::abs(loaded.value - 2.71828) < 0.00001, "value from binary");
            check_eq(loaded.tag, std::string("euler"), "tag from binary");
        });

        test("blended_property_owner_json", [this]() {
            auto eng = make_engine();
            stdlib::register_all(*eng);

            dynamic_binder<blended_object>(eng, "BlendedObject").build();

            auto original = std::make_shared<blended_object>();
            original->score = 1000;
            original->player_name = "Champion";
            original->extra_data = 42;
            original->history = {1.0f, 2.5f, 3.7f, 4.2f};

            serialization::json_archive_writer writer(2, eng.get());
            writer.begin_object("BlendedObject", 1);
            writer.serialize_object_content(*original);
            writer.end_object();
            std::string json = writer.str();

            check(json.find("1000") != std::string::npos, "Score in JSON");
            check(json.find("Champion") != std::string::npos, "Player name in JSON");
            check(json.find("42") != std::string::npos, "Extra data in JSON");
            check(json.find("history") != std::string::npos, "History key in JSON");

            blended_object loaded;
            serialization::json_archive_reader reader(json, eng.get());
            std::string type_name;
            uint32_t version;
            reader.begin_object(type_name, version);
            reader(loaded);
            reader.end_object();

            check_eq(loaded.score.get(), 1000, "Score restored");
            check_eq(loaded.player_name.get(), std::string("Champion"), "Name restored");
            check_eq(loaded.extra_data, 42, "Extra data restored");
            check_eq(loaded.history.size(), size_t(4), "History size");
            check(std::abs(loaded.history[1] - 2.5f) < 0.001f, "History[1] restored");
        });

        test("blended_binary_roundtrip", [this]() {
            auto eng = make_engine();
            stdlib::register_all(*eng);

            dynamic_binder<blended_object>(eng, "BlendedObject").build();

            auto original = std::make_shared<blended_object>();
            original->score = 500;
            original->player_name = "Binary Champ";
            original->extra_data = 99;
            original->history = {10.0f, 20.0f};

            std::vector<uint8_t> buffer;
            serialization::binary_archive_writer writer(buffer, eng.get());
            writer.begin_object("BlendedObject", 1);
            writer.serialize_object_content(*original);
            writer.end_object();

            blended_object loaded;
            serialization::binary_archive_reader reader(buffer, eng.get());
            std::string type_name;
            uint32_t version;
            reader.begin_object(type_name, version);
            loaded.load(reader);
            reader.end_object();

            check_eq(loaded.score.get(), 500, "Score from binary");
            check_eq(loaded.extra_data, 99, "Extra data from binary");
            check_eq(loaded.history.size(), size_t(2), "History size from binary");
        });

        test("property_owner_auto_dispatch", [this]() {
            auto eng = make_engine();
            stdlib::register_all(*eng);

            dynamic_binder<conv_player>(eng, "ConvPlayer").build();

            auto original = std::make_shared<conv_player>();
            original->health = 777;
            original->name = "AutoPlayer";
            original->speed = 12.5f;

            serialization::json_archive_writer writer(2, eng.get());
            writer.begin_object("ConvPlayer", 1);
            writer.serialize_object_content(*original);
            writer.end_object();
            std::string json = writer.str();

            conv_player loaded;
            serialization::json_archive_reader reader(json, eng.get());
            std::string type_name;
            uint32_t version;
            reader.begin_object(type_name, version);
            loaded.property_mgr.load(reader);
            reader.end_object();

            check_eq(loaded.health.get(), 777, "Health auto-restored");
            check_eq(loaded.name.get(), std::string("AutoPlayer"), "Name auto-restored");
            check(std::abs(loaded.speed.get() - 12.5f) < 0.001f, "Speed auto-restored");
        });

        test("json_with_user_context", [this]() {
            auto eng = make_engine();
            stdlib::register_all(*eng);

            dynamic_binder<conv_player>(eng, "ConvPlayer").build();

            auto original = std::make_shared<conv_player>();
            original->health = 60;
            original->name = "Context Player";

            serialization::json_archive_writer writer(2, eng.get());
            writer.begin_object("ConvPlayer", 1);
            original->property_mgr.save(writer);
            writer.end_object();
            std::string json = writer.str();

            conv_test_context ctx;
            ctx.service_name = "GameService";
            ctx.multiplier = 2;

            conv_player loaded;
            serialization::json_archive_reader reader(json, eng.get());
            reader.set_user_context(&ctx);

            std::string type_name;
            uint32_t version;
            reader.begin_object(type_name, version);
            loaded.property_mgr.load(reader);
            reader.end_object();

            check_eq(loaded.health.get(), 60, "Health with context");
            check_eq(loaded.name.get(), std::string("Context Player"), "Name with context");

            conv_context_scaled scaled;
            scaled.value = 7;
            check_eq(14, from_json<conv_context_scaled>(*eng, to_json(*eng, scaled, ctx)).value, "to_json's context reaches save");
            check_eq(14, from_binary<conv_context_scaled>(*eng, to_binary(*eng, scaled, ctx)).value, "to_binary's context reaches save");
            check_eq(14, from_binary_string<conv_context_scaled>(*eng, to_binary_string(*eng, scaled, ctx)).value, "to_binary_string's context reaches save");
            check_eq(14, from_base64<conv_context_scaled>(*eng, to_base64(*eng, scaled, ctx)).value, "to_base64's context reaches save");
        });

        test("binary_little_endian_portable", [this]() {
            auto eng = make_engine();

            int32_t value = 0x12345678;
            std::string binary = to_binary_string(*eng, value);

            int32_t loaded = from_binary_string<int32_t>(*eng, binary);
            check_eq(loaded, value, "Int32 portable binary roundtrip");

            int64_t big_value = 0x123456789ABCDEF0LL;
            std::string big_binary = to_binary_string(*eng, big_value);
            int64_t big_loaded = from_binary_string<int64_t>(*eng, big_binary);
            check_eq(big_loaded, big_value, "Int64 portable binary roundtrip");
        });

        test("shared_ptr_json_roundtrip", [this]() {
            auto eng = make_engine();

            auto ptr1 = std::make_shared<sp_inner_data>();
            ptr1->x = 42;
            ptr1->label = "hello";

            serialization::json_archive_writer writer(2, eng.get());
            writer.begin_object();
            writer.serialize("ptr1", ptr1);
            writer.serialize("ptr1_alias", ptr1);  // Same pointer again - should be a ref
            std::shared_ptr<sp_inner_data> null_ptr;
            writer.serialize("null_ptr", null_ptr);
            writer.end_object();

            std::string json = writer.str();

            check(json.find("$id") != std::string::npos, "$id key present");
            check(json.find("$val") != std::string::npos, "$val key present");
            check(json.find("null") != std::string::npos, "null present for null_ptr");

            std::shared_ptr<sp_inner_data> loaded_ptr1;
            std::shared_ptr<sp_inner_data> loaded_alias;
            std::shared_ptr<sp_inner_data> loaded_null;

            serialization::json_archive_reader reader(json, eng.get());
            reader.begin_object();
            reader.serialize("ptr1", loaded_ptr1);
            reader.serialize("ptr1_alias", loaded_alias);
            reader.serialize("null_ptr", loaded_null);
            reader.end_object();

            check(loaded_ptr1 != nullptr, "ptr1 not null");
            check_eq(loaded_ptr1->x, 42, "ptr1.x preserved");
            check_eq(loaded_ptr1->label, std::string("hello"), "ptr1.label preserved");

            check(loaded_alias != nullptr, "alias not null");
            check(loaded_ptr1.get() == loaded_alias.get(), "alias is same object as ptr1");

            check(loaded_null == nullptr, "null_ptr stays null");
        });

        test("shared_ptr_binary_roundtrip", [this]() {
            auto eng = make_engine();

            auto ptr1 = std::make_shared<sp_inner_data>();
            ptr1->x = 99;
            ptr1->label = "binary_test";

            std::vector<uint8_t> buffer;
            serialization::binary_archive_writer writer(buffer, eng.get());
            writer.begin_object();
            writer.serialize("ptr1", ptr1);
            writer.serialize("ptr1_alias", ptr1);
            std::shared_ptr<sp_inner_data> null_ptr;
            writer.serialize("null_ptr", null_ptr);
            writer.end_object();

            std::shared_ptr<sp_inner_data> loaded_ptr1;
            std::shared_ptr<sp_inner_data> loaded_alias;
            std::shared_ptr<sp_inner_data> loaded_null;

            serialization::binary_archive_reader reader(buffer, eng.get());
            std::string type_name;
            uint32_t version;
            reader.begin_object(type_name, version);
            reader.serialize("ptr1", loaded_ptr1);
            reader.serialize("ptr1_alias", loaded_alias);
            reader.serialize("null_ptr", loaded_null);
            reader.end_object();

            check(loaded_ptr1 != nullptr, "ptr1 not null");
            check_eq(loaded_ptr1->x, 99, "ptr1.x preserved");
            check_eq(loaded_ptr1->label, std::string("binary_test"), "ptr1.label preserved");
            check(loaded_alias != nullptr, "alias not null");
            check(loaded_ptr1.get() == loaded_alias.get(), "alias is same object");
            check(loaded_null == nullptr, "null_ptr stays null");
        });

        test("unique_ptr_json_roundtrip", [this]() {
            auto eng = make_engine();

            auto uptr = std::make_unique<sp_simple_data>();
            uptr->value = 123;

            serialization::json_archive_writer writer(2, eng.get());
            writer.begin_object();
            writer.serialize("uptr", uptr);
            std::unique_ptr<sp_simple_data> null_uptr;
            writer.serialize("null_uptr", null_uptr);
            writer.end_object();

            std::string json = writer.str();
            check(json.find("$val") != std::string::npos, "$val key present for unique_ptr");
            check(json.find("null") != std::string::npos, "null present for null unique_ptr");

            std::unique_ptr<sp_simple_data> loaded_uptr;
            std::unique_ptr<sp_simple_data> loaded_null_uptr;

            serialization::json_archive_reader reader(json, eng.get());
            reader.begin_object();
            reader.serialize("uptr", loaded_uptr);
            reader.serialize("null_uptr", loaded_null_uptr);
            reader.end_object();

            check(loaded_uptr != nullptr, "uptr not null");
            check_eq(loaded_uptr->value, 123, "uptr.value preserved");
            check(loaded_null_uptr == nullptr, "null_uptr stays null");
        });

        test("weak_ptr_json_roundtrip", [this]() {
            auto eng = make_engine();

            auto shared = std::make_shared<sp_simple_data>();
            shared->value = 77;
            std::weak_ptr<sp_simple_data> weak = shared;
            std::weak_ptr<sp_simple_data> null_weak;

            // Serialize - shared_ptr MUST be serialized before weak_ptr
            serialization::json_archive_writer writer(2, eng.get());
            writer.begin_object();
            writer.serialize("shared", shared);
            writer.serialize("weak", weak);
            writer.serialize("null_weak", null_weak);
            writer.end_object();

            std::string json = writer.str();
            check(json.find("$ref") != std::string::npos, "$ref key present for weak_ptr");

            std::shared_ptr<sp_simple_data> loaded_shared;
            std::weak_ptr<sp_simple_data> loaded_weak;
            std::weak_ptr<sp_simple_data> loaded_null_weak;

            serialization::json_archive_reader reader(json, eng.get());
            reader.begin_object();
            reader.serialize("shared", loaded_shared);
            reader.serialize("weak", loaded_weak);
            reader.serialize("null_weak", loaded_null_weak);
            reader.end_object();

            check(loaded_shared != nullptr, "shared not null");
            check_eq(loaded_shared->value, 77, "shared.value preserved");

            auto locked = loaded_weak.lock();
            check(locked != nullptr, "weak can lock");
            check(locked.get() == loaded_shared.get(), "weak points to same shared object");

            check(loaded_null_weak.lock() == nullptr, "null_weak stays expired");
        });

        test("vector_of_shared_ptr_json_roundtrip", [this]() {
            auto eng = make_engine();

            auto a = std::make_shared<sp_id_data>();
            a->id = 1;
            auto b = std::make_shared<sp_id_data>();
            b->id = 2;

            std::vector<std::shared_ptr<sp_id_data>> vec = {a, b, a};  // a appears twice

            serialization::json_archive_writer writer(2, eng.get());
            writer.begin_object();
            writer.serialize("vec", vec);
            writer.end_object();

            std::string json = writer.str();

            std::vector<std::shared_ptr<sp_id_data>> loaded_vec;

            serialization::json_archive_reader reader(json, eng.get());
            reader.begin_object();
            reader.serialize("vec", loaded_vec);
            reader.end_object();

            check_eq(loaded_vec.size(), size_t(3), "vector size preserved");
            check(loaded_vec[0] != nullptr, "vec[0] not null");
            check_eq(loaded_vec[0]->id, 1, "vec[0].id preserved");
            check(loaded_vec[1] != nullptr, "vec[1] not null");
            check_eq(loaded_vec[1]->id, 2, "vec[1].id preserved");
            check(loaded_vec[2] != nullptr, "vec[2] not null");
            // vec[0] and vec[2] should be the same object (de-duplicated)
            check(loaded_vec[0].get() == loaded_vec[2].get(), "vec[0] and vec[2] are same object");
        });

        test("shared_ptr_property_json_roundtrip", [this]() {
            // Test shared_ptr as a JAI_PROPERTY inside a property_owner
            auto eng = make_engine();

            sp_ptr_holder original;
            original.data = std::make_shared<sp_simple_data>();
            original.data.get()->value = 555;
            original.extra = 10;

            serialization::json_archive_writer writer(2, eng.get());
            writer.begin_object("sp_ptr_holder", 1);
            original.property_mgr.save(writer);
            writer.end_object();

            std::string json = writer.str();

            sp_ptr_holder loaded;
            serialization::json_archive_reader reader(json, eng.get());
            std::string type_name;
            uint32_t version;
            reader.begin_object(type_name, version);
            loaded.property_mgr.load(reader);
            reader.end_object();

            check(loaded.data.get() != nullptr, "data not null");
            check_eq(loaded.data.get()->value, 555, "data.value preserved");
            check_eq(loaded.extra.get(), 10, "extra preserved");
        });

        test("convenience_owning_ptr_polymorphic_json", [this]() {
            auto eng = make_engine();

            auto derived = std::make_shared<conv_poly_derived>();
            derived->baseVal = 7;          // inherited from conv_poly_base
            derived->derivedTag = "kept";
            std::shared_ptr<conv_poly_base> original = derived;

            std::string json = to_json(*eng, original);
            check(json.find("$type") != std::string::npos, "polymorphic owning root must emit a $type discriminator");
            check(json.find("conv_poly_derived") != std::string::npos, "derived type name present in JSON");

            auto loaded = from_json<std::shared_ptr<conv_poly_base>>(*eng, json);
            check(loaded != nullptr, "loaded not null");
            auto loadedDerived = std::dynamic_pointer_cast<conv_poly_derived>(loaded);
            check(loadedDerived != nullptr, "convenience from_json sliced the derived type (B6)");
            check_eq(loadedDerived->derivedTag.get(), std::string("kept"), "derived field preserved (not sliced)");
            check_eq(loadedDerived->baseVal.get(), 7, "inherited field preserved");
        });

        test("convenience_owning_ptr_polymorphic_binary", [this]() {
            auto eng = make_engine();

            auto derived = std::make_shared<conv_poly_derived>();
            derived->baseVal = 11;
            derived->derivedTag = "binary";
            std::shared_ptr<conv_poly_base> original = derived;

            std::string blob = to_binary_string(*eng, original);
            auto loaded = from_binary_string<std::shared_ptr<conv_poly_base>>(*eng, blob);
            auto loadedDerived = std::dynamic_pointer_cast<conv_poly_derived>(loaded);
            check(loadedDerived != nullptr, "binary convenience round-trip sliced the derived type (B6)");
            check_eq(loadedDerived->derivedTag.get(), std::string("binary"), "derived field preserved (binary)");
            check_eq(loadedDerived->baseVal.get(), 11, "inherited field preserved (binary)");
        });

        test("convenience_owning_ptr_polymorphic_base64", [this]() {
            auto eng = make_engine();

            auto derived = std::make_shared<conv_poly_derived>();
            derived->baseVal = 13;
            derived->derivedTag = "b64";
            std::shared_ptr<conv_poly_base> original = derived;

            std::string encoded = to_base64(*eng, original);
            auto loaded = from_base64<std::shared_ptr<conv_poly_base>>(*eng, encoded);
            auto loadedDerived = std::dynamic_pointer_cast<conv_poly_derived>(loaded);
            check(loadedDerived != nullptr, "base64 convenience round-trip sliced the derived type (B6)");
            check_eq(loadedDerived->derivedTag.get(), std::string("b64"), "derived field preserved (base64)");
            check_eq(loadedDerived->baseVal.get(), 13, "inherited field preserved (base64)");
        });

        test("convenience_owning_ptr_nonpolymorphic_json", [this]() {
            auto eng = make_engine();
            auto ptr = std::make_shared<sp_simple_data>();
            ptr->value = 321;

            std::string json = to_json(*eng, ptr);
            auto loaded = from_json<std::shared_ptr<sp_simple_data>>(*eng, json);
            check(loaded != nullptr, "non-polymorphic owning root round-trips");
            check_eq(loaded->value, 321, "value preserved through convenience round-trip");
        });

        test("dangling_shared_ptr_id_is_a_load_error_not_null", [this]() {
            auto eng = make_engine();
            // A payload-less $id that nothing in the archive defined: previously loaded as null
            // and crashed the first consumer that dereferenced it.
            int outcome = 0;
            try {
                auto loaded = from_json<std::shared_ptr<sp_simple_data>>(*eng, "{\"value0\": {\"$id\": 7}}");
                outcome = loaded ? 3 : 0;
            } catch (const serialization_error& e) {
                std::string what = e.what();
                outcome = what.find("Dangling") != std::string::npos ? 1 : 2;
                check(outcome == 1, "the error names the dangling id, got: " + what);
            } catch (...) {
                outcome = 4;
            }
            check_eq(1, outcome, "a dangling $id throws serialization_error (0=null, 2=wrong text, 3=object, 4=other exception)");
            auto ptr = std::make_shared<sp_simple_data>();
            ptr->value = 5;
            auto pair = std::make_pair(ptr, ptr);
            auto roundTrip = from_json<std::pair<std::shared_ptr<sp_simple_data>, std::shared_ptr<sp_simple_data>>>(*eng, to_json(*eng, pair));
            check(roundTrip.first != nullptr && roundTrip.first == roundTrip.second, "a genuine back-reference still resolves to the same object");
        });

        // These guard the parse_number/parse_string/parse_json hardening:
        // malformed input must raise jai::serialization_error (NOT a raw
        // std::out_of_range / std::invalid_argument, which derive from a sibling
        // hierarchy and would escape callers catching serialization_error), and
        // must never invoke UB (std::isdigit on a sign-extended char) or crash.

        // Helper: returns 1 if the parse threw serialization_error, 2 if it threw some
        // OTHER exception type (the bug we are guarding against), 0 if it did not throw.
        auto parseOutcome = [](engine* e, const std::string& json) -> int {
            try {
                serialization::json_archive_reader reader(json, e);
                return 0;
            } catch (const serialization_error&) {
                return 1;
            } catch (...) {
                return 2;
            }
        };

        test("json_reader_rejects_trailing_garbage", [this, parseOutcome]() {
            auto eng = make_engine();
            check_eq(parseOutcome(eng.get(), "42 garbage"), 1, "trailing garbage -> serialization_error");
            check_eq(parseOutcome(eng.get(), "{} []"), 1, "two top-level values -> serialization_error");
            check_eq(parseOutcome(eng.get(), "  123  "), 0, "leading/trailing whitespace is fine");
        });

        test("json_reader_bare_minus_is_serialization_error", [this, parseOutcome]() {
            auto eng = make_engine();
            check_eq(parseOutcome(eng.get(), "-"), 1, "bare '-' -> serialization_error");
            check_eq(parseOutcome(eng.get(), "1.2.3"), 1, "malformed number -> serialization_error");
        });

        test("json_reader_huge_int_degrades_to_double", [this]() {
            auto eng = make_engine();
            serialization::json_archive_reader reader("99999999999999999999", eng.get());
            double v = reader.read_value().as<double>();
            check(v > 9.9e19 && v < 1.1e20, "out-of-range integer parses as double");
        });

        test("json_reader_non_ascii_value_byte_no_ub", [this, parseOutcome]() {
            auto eng = make_engine();
            // std::isdigit(signed char) on a non-ASCII byte is UB / debug-CRT abort. Must be a clean throw.
            std::string bad = "\xC3\xA9";   // 'é' bytes, not a valid value start
            check_eq(parseOutcome(eng.get(), bad), 1, "non-ASCII value byte -> serialization_error, no UB");
        });

        test("json_reader_invalid_unicode_escape_is_serialization_error", [this, parseOutcome]() {
            auto eng = make_engine();
            check_eq(parseOutcome(eng.get(), "\"\\uZZZZ\""), 1, "non-hex \\u -> serialization_error");
            check_eq(parseOutcome(eng.get(), "\"\\uD8\""), 1, "truncated \\u -> serialization_error");
        });

        test("json_reader_lone_surrogate_becomes_replacement_char", [this]() {
            auto eng = make_engine();
            // Lone high surrogate must become U+FFFD (valid UTF-8), not WTF-8.
            serialization::json_archive_reader reader("\"\\uD800\"", eng.get());
            std::string s = reader.read_value().as<std::string>();
            check_eq(s, std::string("\xEF\xBF\xBD"), "lone high surrogate -> U+FFFD");
        });

        test("json_reader_valid_surrogate_pair", [this]() {
            auto eng = make_engine();
            // U+1F600 (grinning face) as a UTF-16 surrogate pair -> 4-byte UTF-8.
            serialization::json_archive_reader reader("\"\\uD83D\\uDE00\"", eng.get());
            std::string s = reader.read_value().as<std::string>();
            check_eq(s, std::string("\xF0\x9F\x98\x80"), "surrogate pair -> U+1F600");
        });

        test("json_string_escapes_and_utf8_roundtrip", [this]() {
            auto eng = make_engine();
            // Covers string escapes (quotes, backslash, control chars) and multi-byte UTF-8 roundtrip.
            std::string original = "a\"b\\c\nd\te\xC3\xA9z";
            std::string json = to_json(*eng, original);
            std::string back = from_json<std::string>(*eng, json);
            check_eq(back, original, "string with escapes + UTF-8 round-trips");
        });

        test("json_infinity_roundtrips", [this]() {
            auto eng = make_engine();
            // Writer emits 1e999 as the infinity sentinel; reader treats from_chars result_out_of_range as inf.
            double inf = std::numeric_limits<double>::infinity();
            std::string json = to_json(*eng, inf);
            double back = from_json<double>(*eng, json);
            check(std::isinf(back) && back > 0, "+infinity round-trips via 1e999 sentinel");
        });

        // weak_ptr serialized before its shared_ptr owner must round-trip (forward ref).
        test("weak_ptr_forward_reference_json", [this]() {
            auto eng = make_engine();
            auto shared = std::make_shared<sp_simple_data>();
            shared->value = 77;
            std::weak_ptr<sp_simple_data> weak = shared;

            serialization::json_archive_writer writer(2, eng.get());
            writer.begin_object();
            writer.serialize("weak", weak);       // forward: weak BEFORE shared
            writer.serialize("shared", shared);
            writer.end_object();
            std::string json = writer.str();

            std::weak_ptr<sp_simple_data> loaded_weak;
            std::shared_ptr<sp_simple_data> loaded_shared;
            serialization::json_archive_reader reader(json, eng.get());
            reader.begin_object();
            reader.serialize("weak", loaded_weak);
            reader.serialize("shared", loaded_shared);
            reader.end_object();

            auto locked = loaded_weak.lock();
            check(locked != nullptr, "forward-referenced weak_ptr resolves (was dropped before)");
            check(loaded_shared != nullptr, "shared loaded");
            check(locked.get() == loaded_shared.get(), "forward weak and shared are the same object");
            check_eq(loaded_shared->value, 77, "value preserved through forward ref");
        });

        test("weak_ptr_forward_reference_binary", [this]() {
            auto eng = make_engine();
            auto shared = std::make_shared<sp_simple_data>();
            shared->value = 88;
            std::weak_ptr<sp_simple_data> weak = shared;

            std::vector<uint8_t> buffer;
            serialization::binary_archive_writer writer(buffer, eng.get());
            writer.begin_object();
            writer.serialize("weak", weak);       // forward: weak BEFORE shared
            writer.serialize("shared", shared);
            writer.end_object();

            std::weak_ptr<sp_simple_data> loaded_weak;
            std::shared_ptr<sp_simple_data> loaded_shared;
            serialization::binary_archive_reader reader(buffer, eng.get());
            std::string type_name;
            uint32_t version;
            reader.begin_object(type_name, version);
            reader.serialize("weak", loaded_weak);
            reader.serialize("shared", loaded_shared);
            reader.end_object();

            auto locked = loaded_weak.lock();
            check(locked != nullptr, "forward-referenced weak_ptr resolves (binary)");
            check(loaded_shared != nullptr, "shared loaded (binary)");
            check(locked.get() == loaded_shared.get(), "forward weak and shared same object (binary)");
            check_eq(loaded_shared->value, 88, "value preserved through forward ref (binary)");
        });

        // A script value holding shared_ptr handles round-trips through to_binary as through to_json (it raised Unsupported type
        // for binary serialization: 13); handles to one object stay one object after the load
        test("shared_handles_round_trip_through_to_binary", [this]() {
            auto eng = make_engine();
            stdlib::register_all(*eng);
            eng->execute("class P { int x = 0; string tag = \"p\"; P() {} P(int a) { x = a; } }");
            check_eq(std::string("P 3 p"), eng->execute("var b = from_binary(to_binary(new P(3))); type_of(b) + \" \" + to_string(b.x) + \" \" + b.tag").as<std::string>());
            check_eq(std::string("true 5"), eng->execute(
                "var p = new P(4); var back = from_binary(to_binary([p, p, {\"k\": p}])); back[0].x = 5; "
                "to_string(same_object(back[0], back[1]) && same_object(back[0], back[2][\"k\"])) + \" \" + to_string(back[2][\"k\"].x)").as<std::string>());
            check_eq(std::string("true"), eng->execute("shared_ptr<P> none = null; to_string(from_binary(to_binary([none]))[0] == null)").as<std::string>());
            check_eq(std::string("true 7"), eng->execute(
                "var q = new P(7); weak_ptr<P> w = weak_ptr<P>(q); var pair = from_binary(to_binary([w, q])); "
                "to_string(same_object(pair[0].lock(), pair[1])) + \" \" + to_string(pair[1].x)").as<std::string>());
            // to_json wrote a shared handle as null; it writes the object once under its id, and from_json shares it again
            check_eq(std::string("P 9 true 6"), eng->execute(
                "var j = new P(9); var jb = from_json(to_json([j, {\"again\": j}])); var first = jb[0]; jb[1][\"again\"].x = 6; "
                "type_of(from_json(to_json(j))) + \" \" + to_string(from_json(to_json(j)).x) + \" \" + to_string(same_object(first, jb[1][\"again\"])) + \" \" + to_string(first.x)").as<std::string>());
            check_eq(std::string("true 7"), eng->execute(
                "var r = new P(7); weak_ptr<P> v = weak_ptr<P>(r); var jp = from_json(to_json([v, r])); "
                "to_string(same_object(jp[0].lock(), jp[1])) + \" \" + to_string(jp[1].x)").as<std::string>(),
                "a weak_ptr and its owner come back as one object (the weak's first occurrence raised Invalid shared_ptr ID)");
            check_eq(std::string("true"), eng->execute("shared_ptr<P> gone = null; to_string(from_json(to_json([gone]))[0] == null)").as<std::string>());
            // a handle back into an object still being written (a cycle) is that object
            check_eq(std::string("true true"), eng->execute(
                "class Ring { var next; } var ring = new Ring(); ring.next = ring; var rb = from_binary(to_binary(ring)); var rj = from_json(to_json(ring)); "
                "to_string(same_object(rb.next, rb)) + \" \" + to_string(same_object(rj.next, rj))").as<std::string>());
        });
    }
};

FOUNDRY_REGISTER(convenience_function_tests);

} // namespace jai::foundry::tests
