#pragma once

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/static_binder.hpp>
#include <jaiscript/core/static_binder_impl.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/macros.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/serialization/binary_archive.hpp>
#include <memory>
#include <cmath>
#include <algorithm>

namespace jai::foundry::tests {

struct Vector2D {
    double x = 0.0;
    double y = 0.0;

    Vector2D() = default;
    Vector2D(double x_, double y_) : x(x_), y(y_) {}

    double length() const { return std::sqrt(x * x + y * y); }
    void normalize() {
        double len = length();
        if (len > 0) { x /= len; y /= len; }
    }
    Vector2D add(const Vector2D& other) const {
        return Vector2D(x + other.x, y + other.y);
    }
};

class Entity : public property_owner<Entity> {
public:
    JAI_PROPERTY((int), id, 0);
    JAI_PROPERTY((std::string), name, "unnamed");
    JAI_PROPERTY((float), health, 100.0f);

    Entity() = default;
    Entity(int id_, const std::string& name_) {
        id = id_;
        name = name_;
    }

    bool is_alive() const { return health.get() > 0; }
    void damage(float amount) { health = std::max(0.0f, health.get() - amount); }
    void heal(float amount) { health = std::min(100.0f, health.get() + amount); }
    std::string status() const {
        if (health.get() >= 75) return "healthy";
        if (health.get() >= 25) return "wounded";
        if (health.get() > 0) return "critical";
        return "dead";
    }
};

class Player : public property_owner<Player> {
public:
    JAI_PROPERTY((std::string), username, "player");
    JAI_PROPERTY((int), level, 1);
    JAI_PROPERTY((int), experience, 0);

    // Custom data NOT managed by JAI_PROPERTY
    int session_id = 0;  // Transient, not serialized

    Player() = default;

    void add_experience(int amount) {
        experience = experience.get() + amount;
        while (experience.get() >= level_up_threshold()) {
            experience = experience.get() - level_up_threshold();
            level = level.get() + 1;
        }
    }

    int level_up_threshold() const {
        return level.get() * 100;
    }

    template<typename Archive>
    void save(Archive& ar) const {
        property_mgr.save(ar);
    }

    template<typename Archive>
    void load(Archive& ar) {
        property_mgr.load(ar);
    }
};

struct GameConfig {
    int max_players = 4;
    bool friendly_fire = false;
    double round_time = 300.0;
};

template<typename Archive>
inline void save(Archive& ar, const GameConfig& cfg) {
    ar.serialize("max_players", cfg.max_players);
    ar.serialize("friendly_fire", cfg.friendly_fire);
    ar.serialize("round_time", cfg.round_time);
}

template<typename Archive>
inline void load(Archive& ar, GameConfig& cfg) {
    ar.serialize("max_players", cfg.max_players);
    ar.serialize("friendly_fire", cfg.friendly_fire);
    ar.serialize("round_time", cfg.round_time);
}

} // namespace jai::foundry::tests

JAI_STATIC_BINDER(jai::foundry::tests::Vector2D, "Vector2D",
    .property("x", &jai::foundry::tests::Vector2D::x)
    .property("y", &jai::foundry::tests::Vector2D::y)
);

namespace jai::foundry::tests {

class static_binder_tests : public suite {
public:
    static_binder_tests() : suite("Static Binder") {}

    void forge_tests() override {
        test("property_owner_with_methods", [this]() {
            auto eng = make_engine();

            dynamic_binder<Entity>(*eng, "Entity")
                .auto_bind()
                .constructor<>()
                .constructor<int, const std::string&>()
                .method("is_alive", &Entity::is_alive)
                .method("damage", &Entity::damage)
                .method("heal", &Entity::heal)
                .method("status", &Entity::status)
                .build();

            eng->execute("auto e = Entity(1, \"Hero\");");

            check_eq(eng->execute("e.id").as<int>(), 1, "Property id works");
            check_eq(eng->execute("e.name").as<std::string>(), "Hero", "Property name works");
            check_eq(eng->execute("e.health").as<float>(), 100.0f, "Property health works");

            check_eq(eng->execute("e.is_alive()").as<bool>(), true, "Method is_alive() works");
            check_eq(eng->execute("e.status()").as<std::string>(), "healthy", "Method status() works");
        });

        test("property_owner_methods_mutate_properties", [this]() {
            auto eng = make_engine();

            dynamic_binder<Entity>(*eng, "Entity")
                .auto_bind()
                .constructor<>()
                .method("is_alive", &Entity::is_alive)
                .method("damage", &Entity::damage)
                .method("heal", &Entity::heal)
                .method("status", &Entity::status)
                .build();

            auto cpp_entity = std::make_shared<Entity>();
            cpp_entity->health = 50.0f;
            eng->add_global("entity", eng->make_object(cpp_entity));

            check_eq(eng->execute("entity.status()").as<std::string>(), "wounded", "Initial status is wounded");

            eng->execute("entity.damage(30);");
            check_eq(cpp_entity->health.get(), 20.0f, "C++ health updated by script method");
            check_eq(eng->execute("entity.status()").as<std::string>(), "critical", "Status is critical");

            eng->execute("entity.heal(60);");
            check_eq(cpp_entity->health.get(), 80.0f, "Healed to 80");
            check_eq(eng->execute("entity.status()").as<std::string>(), "healthy", "Status is healthy");
        });

        test("property_owner_kill_entity", [this]() {
            auto eng = make_engine();

            dynamic_binder<Entity>(*eng, "Entity")
                .auto_bind()
                .constructor<>()
                .method("is_alive", &Entity::is_alive)
                .method("damage", &Entity::damage)
                .method("status", &Entity::status)
                .build();

            eng->execute("auto e = Entity();");
            check_eq(eng->execute("e.is_alive()").as<bool>(), true, "Initially alive");

            eng->execute("e.damage(150);");  // Overkill
            check_eq(eng->execute("e.health").as<float>(), 0.0f, "Health clamped to 0");
            check_eq(eng->execute("e.is_alive()").as<bool>(), false, "Now dead");
            check_eq(eng->execute("e.status()").as<std::string>(), "dead", "Status is dead");
        });

        test("composable_save_load_property_owner", [this]() {
            Player p1;
            p1.username = "TestUser";
            p1.level = 5;
            p1.experience = 250;
            p1.session_id = 99;  // Transient, should NOT be serialized

            std::vector<uint8_t> buffer;
            {
                serialization::binary_archive_writer ar(buffer);
                ar.begin_object("Player", 1);
                p1.save(ar);
                ar.end_object();
            }

            // binary_archive_reader needs an engine
            auto eng = make_engine();
            Player p2;
            {
                serialization::binary_archive_reader ar(buffer, eng.get());
                std::string type_name;
                uint32_t version;
                ar.begin_object(type_name, version);
                p2.load(ar);
                ar.end_object();
            }

            check_eq(p2.username.get(), "TestUser", "Username restored");
            check_eq(p2.level.get(), 5, "Level restored");
            check_eq(p2.experience.get(), 250, "Experience restored");
            check_eq(p2.session_id, 0, "Session ID NOT serialized (transient)");
        });

        test("composable_methods_update_properties", [this]() {
            auto eng = make_engine();

            dynamic_binder<Player>(*eng, "Player")
                .auto_bind()
                .constructor<>()
                .method("add_experience", &Player::add_experience)
                .method("level_up_threshold", &Player::level_up_threshold)
                .build();

            auto cpp_player = std::make_shared<Player>();
            cpp_player->level = 1;
            cpp_player->experience = 0;
            eng->add_global("player", eng->make_object(cpp_player));

            eng->execute("player.add_experience(150);");  // 150 exp at level 1 (threshold=100)
            check_eq(cpp_player->level.get(), 2, "Leveled up to 2");
            check_eq(cpp_player->experience.get(), 50, "50 exp remaining after level up");

            check_eq(eng->execute("player.level").as<int>(), 2, "Script sees level 2");
            check_eq(eng->execute("player.experience").as<int>(), 50, "Script sees 50 exp");
        });

        test("free_function_save_load", [this]() {
            GameConfig cfg1;
            cfg1.max_players = 8;
            cfg1.friendly_fire = true;
            cfg1.round_time = 600.0;

            std::vector<uint8_t> buffer;
            {
                serialization::binary_archive_writer ar(buffer);
                ar.begin_object("GameConfig", 1);
                save(ar, cfg1);
                ar.end_object();
            }

            // binary_archive_reader needs an engine
            auto eng = make_engine();
            GameConfig cfg2;
            {
                serialization::binary_archive_reader ar(buffer, eng.get());
                std::string type_name;
                uint32_t version;
                ar.begin_object(type_name, version);
                load(ar, cfg2);
                ar.end_object();
            }

            check_eq(cfg2.max_players, 8, "max_players restored");
            check_eq(cfg2.friendly_fire, true, "friendly_fire restored");
            check_eq(cfg2.round_time, 600.0, "round_time restored");
        });

        test("has_static_type_detection", [this]() {
            check(has_static_type_v<Vector2D>, "Vector2D has static type");
            check(!has_static_type_v<GameConfig>, "GameConfig does NOT have static type");
            check(!has_static_type_v<int>, "int does NOT have static type");
            check(!has_static_type_v<std::string>, "string does NOT have static type");
        });

        test("is_property_owner_detection", [this]() {
            check(property_serialization::is_property_owner_v<Entity>, "Entity is property_owner");
            check(property_serialization::is_property_owner_v<Player>, "Player is property_owner");
            check(!property_serialization::is_property_owner_v<Vector2D>, "Vector2D is NOT property_owner");
            check(!property_serialization::is_property_owner_v<GameConfig>, "GameConfig is NOT property_owner");
            check(!property_serialization::is_property_owner_v<int>, "int is NOT property_owner");
        });

        test("static_binder_compile_time_counts", [this]() {
            constexpr size_t vector_props = jai_static_type<Vector2D>::binder.property_count();
            constexpr size_t vector_methods = jai_static_type<Vector2D>::binder.method_count();
            check_eq(vector_props, size_t(2), "Vector2D has 2 properties (x, y)");
            check_eq(vector_methods, size_t(0), "Vector2D has 0 methods in static binder");
        });

        test("static_binder_serialization", [this]() {
            Vector2D v1{3.0, 4.0};

            std::vector<uint8_t> buffer;
            {
                serialization::binary_archive_writer ar(buffer);
                ar.begin_object("Vector2D", 1);
                ar.serialize("x", v1.x);
                ar.serialize("y", v1.y);
                ar.end_object();
            }

            auto eng = make_engine();
            Vector2D v2;
            {
                serialization::binary_archive_reader ar(buffer, eng.get());
                std::string type_name;
                uint32_t version;
                ar.begin_object(type_name, version);
                ar.serialize("x", v2.x);
                ar.serialize("y", v2.y);
                ar.end_object();
            }

            check_eq(v2.x, 3.0, "x restored");
            check_eq(v2.y, 4.0, "y restored");
            check_eq(v2.length(), 5.0, "length() correct (3-4-5 triangle)");
        });

        test("shared_object_property_and_method_access", [this]() {
            auto eng = make_engine();

            dynamic_binder<Entity>(*eng, "Entity")
                .auto_bind()
                .constructor<int, const std::string&>()
                .method("is_alive", &Entity::is_alive)
                .method("damage", &Entity::damage)
                .method("heal", &Entity::heal)
                .method("status", &Entity::status)
                .build();

            auto cpp_entity = std::make_shared<Entity>(42, "Warrior");
            cpp_entity->health = 75.0f;
            eng->add_global("warrior", eng->make_object(cpp_entity));

            check_eq(eng->execute("warrior.id").as<int>(), 42, "Read id from script");
            check_eq(eng->execute("warrior.name").as<std::string>(), "Warrior", "Read name from script");
            check_eq(eng->execute("warrior.status()").as<std::string>(), "healthy", "status() works");

            eng->execute("warrior.name = \"Champion\";");
            check_eq(cpp_entity->name.get(), "Champion", "C++ sees script property change");

            eng->execute("warrior.damage(50);");
            check_eq(cpp_entity->health.get(), 25.0f, "C++ sees method mutation");

            check_eq(eng->execute("warrior.health").as<float>(), 25.0f, "Script sees updated health");
            check_eq(eng->execute("warrior.status()").as<std::string>(), "wounded", "Status reflects new health");
        });

        test("property_bidirectional_sync", [this]() {
            auto eng = make_engine();

            dynamic_binder<Entity>(*eng, "Entity")
                .auto_bind()
                .constructor<>()
                .build();

            auto cpp_entity = std::make_shared<Entity>();
            eng->add_global("e", eng->make_object(cpp_entity));

            eng->execute("e.health = 75;");
            check_eq(cpp_entity->health.get(), 75.0f, "C++ sees script change");

            cpp_entity->health = 25.0f;
            check_eq(eng->execute("e.health").as<float>(), 25.0f, "Script sees C++ change");

            eng->execute("e.health += 10;");
            check_eq(cpp_entity->health.get(), 35.0f, "C++ sees compound assignment");
        });

        test("multiple_types_same_engine", [this]() {
            auto eng = make_engine();

            dynamic_binder<Entity>(*eng, "Entity")
                .auto_bind()
                .constructor<int, const std::string&>()
                .method("status", &Entity::status)
                .build();

            dynamic_binder<Player>(*eng, "Player")
                .auto_bind()
                .constructor<>()
                .method("level_up_threshold", &Player::level_up_threshold)
                .build();

            eng->execute(R"(
                auto entity = Entity(1, "Test");
                auto player = Player();
            )");

            check_eq(eng->execute("entity.status()").as<std::string>(), "healthy", "Entity works");
            check_eq(eng->execute("player.level_up_threshold()").as<int>(), 100, "Player works");
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::static_binder_tests)
