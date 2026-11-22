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

// load_and_construct binding a reference member: construct<T> forwards its arguments, so construct(target) reaches T(int&); the
// referent comes from the archive's user context
struct counter_context { int target = 0; };
class counted_ref : public property_owner<counted_ref> {
    friend jai::access;
public:
    explicit counted_ref(int& a_target) : target(a_target) {}
    int& target;
    JAI_PROPERTY((int), steps, 0);
protected:
    template<class Archive> void save(Archive& archive, std::uint32_t const) const { property_mgr.save(archive); }
    template<class Archive> void load(Archive& archive, std::uint32_t const) { property_mgr.load(archive); }
    template<class Archive> static void load_and_construct(Archive& archive, jai::serialization::construct<counted_ref>& construct) {
        construct(archive.template get_user_context<counter_context>()->target);
        construct->load(archive, 0);
    }
};

// The archive names the referent and the user context holds it: the saved key picks the context's object
struct keyed_context { std::map<std::string, int> targets; };
struct keyed_ref {
    std::string key;
    int& target;
    keyed_ref(std::string a_key, int& a_target) : key(std::move(a_key)), target(a_target) {}
    template<class Archive> void serialize(Archive& archive) { archive(jai::serialization::make_nvp("key", key)); }
    template<class Archive> static void load_and_construct(Archive& archive, jai::serialization::construct<keyed_ref>& construct) {
        std::string savedKey;
        archive(jai::serialization::make_nvp("key", savedKey));
        construct(savedKey, archive.template get_user_context<keyed_context>()->targets.at(savedKey));
    }
};

// A clickable widget shape: a property_owner whose JAI_SIGNAL persists owned text receivers.
class context_receiver_owner : public property_owner<context_receiver_owner> {
public:
    typedef void PingSignature(int);
    JAI_SIGNAL(PingSignature, onPing);

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
        test("load_and_construct_binds_a_reference_member_from_the_user_context", [this]() {
            auto eng = make_engine();
            int original_target = 1;
            auto saved = std::make_shared<counted_ref>(original_target);
            saved->steps = 4;
            const std::string json = to_json(*eng, saved);
            counter_context context;
            auto loaded = from_json<std::shared_ptr<counted_ref>>(*eng, json, context);
            check_true(loaded != nullptr, json);
            check_eq((int)4, loaded->steps.get(), "the record's fields restore after the construction");
            loaded->target = 9;
            check_eq((int)9, context.target, "the reference names the context's object");
            check_eq((int)1, original_target, "never the saved object's referent");
            counter_context binary_context;
            auto from_bytes = from_binary<std::shared_ptr<counted_ref>>(*eng, to_binary(*eng, saved), binary_context);
            check_true(from_bytes != nullptr && from_bytes->steps.get() == 4, "the binary archive loads the record");
            from_bytes->target += 2;
            check_eq((int)2, binary_context.target, "the binary load binds its own context's object");
        });
        test("load_and_construct_binds_a_reference_member_the_archive_names", [this]() {
            auto eng = make_engine();
            int original_target = 0;
            auto saved = std::make_shared<keyed_ref>("b", original_target);
            keyed_context context;
            context.targets["a"] = 1;
            context.targets["b"] = 2;
            auto loaded = from_json<std::shared_ptr<keyed_ref>>(*eng, to_json(*eng, saved), context);
            check_true(loaded != nullptr && loaded->key == "b", "the record's key loads");
            loaded->target = 7;
            check_eq((int)7, context.targets["b"], "the reference names the object the saved key picks");
            check_eq((int)1, context.targets["a"]);
            auto from_bytes = from_binary<std::shared_ptr<keyed_ref>>(*eng, to_binary(*eng, saved), context);
            from_bytes->target += 1;
            check_eq((int)8, context.targets["b"], "the binary load binds the same object");
            check_eq((int)0, original_target, "never the saved object's referent");
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
