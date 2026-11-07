#include <jaiscript/testing/foundry.hpp>
#include <map>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/properties/property.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/properties/macros.hpp>
#include <jaiscript/signals/signal_property.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/serialization/binary_archive.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

struct unrelated_archive_context {};

// A clickable widget shape: a property_owner whose JAI_SIGNAL_PROPERTY persists owned text receivers.
class context_receiver_owner : public property_owner<context_receiver_owner> {
public:
    typedef void PingSignature(int);
    JAI_SIGNAL_PROPERTY(PingSignature, onPing);

    void addOwned(const std::string& id, const std::string& script) { onPingSignal.connect(id, script); }
    bool hasOwned(const std::string& id) { return onPingSignal.connected(id); }
    void ping(int v) { onPingSignal(v); }
};

struct empty_map_holder {
    std::map<std::string, int> ratings;
    int games = 3;
    template<class Archive> void serialize(Archive& ar) { ar(jai::serialization::make_nvp("ratings", ratings), jai::serialization::make_nvp("games", games)); }
};

class archive_engine_context_tests : public suite {
public:
    archive_engine_context_tests() : suite("Archive Engine Context") {}

    void forge_tests() override {
        // Lobby ServerPlayer shape: an nvp whose value is an EMPTY map ({}) must round-trip, not read as 'not found'.
        test("empty_map_property_round_trips_through_json", [this]() {
            auto eng = make_engine();
            empty_map_holder original;
            auto json = to_json(*eng, original);
            auto loaded = from_json<empty_map_holder>(*eng, json);
            check_eq((int)3, loaded.games, "scalar after the empty map: " + json);
            check_true(loaded.ratings.empty(), "empty map stays empty");
        });
        test("json_reader_ctor_engine_is_implicit_context", [this]() {
            auto eng = make_engine();
            serialization::json_archive_reader reader("{}", eng.get());
            check(reader.get_user_context<engine>() == eng.get(), "json reader answers its ctor engine without set_user_context");
            check_null(reader.get_user_context<unrelated_archive_context>(), "unset contexts stay null");
            serialization::any_archive_reader erased(reader);
            check(erased.get_user_context<engine>() == eng.get(), "type-erased json reader routes engine context to the ctor engine");
        });

        test("binary_reader_ctor_engine_is_implicit_context", [this]() {
            auto eng = make_engine();
            std::vector<uint8_t> empty;
            serialization::binary_archive_reader reader(empty, eng.get());
            check(reader.get_user_context<engine>() == eng.get(), "binary reader answers its ctor engine without set_user_context");
            check_null(reader.get_user_context<unrelated_archive_context>(), "unset contexts stay null");
            serialization::any_archive_reader erased(reader);
            check(erased.get_user_context<engine>() == eng.get(), "type-erased binary reader routes engine context to the ctor engine");
        });

        test("writers_expose_ctor_engine_context", [this]() {
            auto eng = make_engine();
            serialization::json_archive_writer json_writer(eng.get());
            check(json_writer.get_user_context<engine>() == eng.get(), "json writer answers its ctor engine");
            std::vector<uint8_t> buffer;
            serialization::binary_archive_writer binary_writer(buffer, eng.get());
            check(binary_writer.get_user_context<engine>() == eng.get(), "binary writer answers its ctor engine");
            serialization::json_archive_writer engineless;
            check_null(engineless.get_user_context<engine>(), "engineless writer has no engine context");
        });

        test("explicit_engine_context_overrides_ctor_engine", [this]() {
            auto ctor_engine = make_engine();
            auto other = make_engine();
            serialization::json_archive_reader reader("{}", ctor_engine.get());
            reader.set_user_context<engine>(other.get());
            check(reader.get_user_context<engine>() == other.get(), "explicit set_user_context<engine> wins");
            serialization::any_archive_reader erased(reader);
            check(erased.get_user_context<engine>() == other.get(), "explicit engine context wins through the type-erased reader");
            check(reader.get_engine() == ctor_engine.get(), "get_engine still names the ctor engine");
        });

        test("signal_property_receiver_fires_after_plain_from_json", [this]() {
            auto eng = make_engine();
            int64_t fired = 0;
            eng->add_function("mark", [counter = &fired](int v) { *counter += v; });

            auto original = std::make_shared<context_receiver_owner>();
            original->addOwned("bump", "mark(arg_0);");
            std::string json = to_json(*eng, original);

            auto restored = from_json<std::shared_ptr<context_receiver_owner>>(*eng, json);
            check(restored != nullptr, "loaded not null");
            check_true(restored->hasOwned("bump"), "owned text receiver persisted");
            restored->ping(5);
            check_eq((int64_t)5, fired, "persisted text receiver runs on the reader's ctor engine (json)");
        });

        test("signal_property_receiver_fires_after_plain_from_binary", [this]() {
            auto eng = make_engine();
            int64_t fired = 0;
            eng->add_function("mark", [counter = &fired](int v) { *counter += v; });

            auto original = std::make_shared<context_receiver_owner>();
            original->addOwned("bump", "mark(arg_0);");
            auto blob = to_binary(*eng, original);

            auto restored = from_binary<std::shared_ptr<context_receiver_owner>>(*eng, blob);
            check(restored != nullptr, "loaded not null");
            check_true(restored->hasOwned("bump"), "owned text receiver persisted");
            restored->ping(3);
            check_eq((int64_t)3, fired, "persisted text receiver runs on the reader's ctor engine (binary)");
        });
    }
};

FOUNDRY_REGISTER(archive_engine_context_tests);

} // namespace jai::foundry::tests
