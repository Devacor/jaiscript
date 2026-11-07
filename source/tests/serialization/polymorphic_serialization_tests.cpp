#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/serialization/binary_archive.hpp>
#include <jaiscript/serialization/construct.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/dynamic_binder_serialization.hpp>
#include <jaiscript/core/registrar.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/properties.hpp>
#include <jaiscript/signals/signal_property.hpp>
#include <jaiscript/detail/static_type_name.hpp>
#include "serialization_roundtrip.hpp"

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// ============================================================================
// C++ Base Classes for Testing
// ============================================================================

// Simple C++ base class (renamed to avoid ODR conflict with static_binder_tests Entity)
class SerEntity {
public:
    int id = 0;
    std::string name = "unnamed";

    virtual ~SerEntity() = default;

    virtual std::string get_type_name() const { return "SerEntity"; }
};

// C++ derived class
class SerCharacter : public SerEntity {
public:
    int health = 100;
    int max_health = 100;

    std::string get_type_name() const override { return "SerCharacter"; }
};

// Fresh hierarchies used ONLY to pin that .base_class<>() and register_relation<> register the
// polymorphic assignability edge (the B5 load check). Isolated so the edge can only come from
// the test under it.
struct EdgeBase { virtual ~EdgeBase() = default; };
struct EdgeDerived : EdgeBase {};
struct RelBase { virtual ~RelBase() = default; };
struct RelDerived : RelBase {};

// Another C++ derived class
class SerItem : public SerEntity {
public:
    float weight = 1.0f;
    int stack_count = 1;

    std::string get_type_name() const override { return "SerItem"; }
};

// Deeply nested hierarchy
class SerPlayer : public SerCharacter {
public:
    int experience = 0;
    int level = 1;

    std::string get_type_name() const override { return "SerPlayer"; }
};

// ============================================================================
// Test Suite
// ============================================================================

class polymorphic_serialization_tests : public suite {
public:
    polymorphic_serialization_tests() : suite("Polymorphic Serialization Tests") {}

    void forge_tests() override {

        // =====================================================================
        // PURE SCRIPT CLASS TESTS
        // =====================================================================

        test("script_class_json_round_trip", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            // Define a simple script class
            eng->execute(R"(
                class Monster {
                    var name = "goblin";
                    var health = 50;
                    var damage = 10;
                    var is_boss = false;
                }
            )");

            // Create instance, modify, serialize, deserialize
            eng->execute(R"(
                var monster = Monster();
                monster.name = "Dragon";
                monster.health = 500;
                monster.damage = 75;
                monster.is_boss = true;

                var json_str = to_json(monster);
                var loaded = from_json(json_str);
            )");

            // Verify round-trip
            check_eq(eng->execute("loaded.name").as<std::string>(), std::string("Dragon"));
            check_eq(eng->execute("loaded.health").as<script_int>(), 500);
            check_eq(eng->execute("loaded.damage").as<script_int>(), 75);
            check_eq(eng->execute("loaded.is_boss").as<bool>(), true);
        });

        test("script_class_binary_round_trip", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            eng->execute(R"(
                class Weapon {
                    var name = "sword";
                    var damage = 10;
                    var durability = 100.0;
                }
            )");

            eng->execute(R"(
                var weapon = Weapon();
                weapon.name = "Excalibur";
                weapon.damage = 999;
                weapon.durability = 100.0;

                var binary_data = to_binary(weapon);
                var loaded = from_binary(binary_data);
            )");

            check_eq(eng->execute("loaded.name").as<std::string>(), std::string("Excalibur"));
            check_eq(eng->execute("loaded.damage").as<script_int>(), 999);
            check_eq(eng->execute("loaded.durability").as<script_float>(), 100.0);
        });

        test("script_class_with_nested_objects", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            eng->execute(R"(
                class Vector2 {
                    var x = 0.0;
                    var y = 0.0;
                }

                class Transform {
                    var position = null;
                    var scale = 1.0;
                    var rotation = 0.0;

                    Transform() {
                        position = Vector2();
                    }
                }
            )");

            eng->execute(R"(
                var transform = Transform();
                transform.position.x = 100.0;
                transform.position.y = 200.0;
                transform.scale = 2.5;
                transform.rotation = 45.0;

                var json_str = to_json(transform);
                var loaded = from_json(json_str);
            )");

            check_eq(eng->execute("loaded.position.x").as<script_float>(), 100.0);
            check_eq(eng->execute("loaded.position.y").as<script_float>(), 200.0);
            check_eq(eng->execute("loaded.scale").as<script_float>(), 2.5);
            check_eq(eng->execute("loaded.rotation").as<script_float>(), 45.0);
        });

        // =====================================================================
        // C++ CLASS ROUND-TRIP TESTS
        // =====================================================================

        test("cpp_class_json_round_trip", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            // Register C++ Entity class
            dynamic_binder<SerEntity>(*eng, "Entity")
                .constructor<>()
                .property("id", &SerEntity::id)
                .property("name", &SerEntity::name)
                .build();

            // Create, modify, serialize, deserialize
            eng->execute(R"(
                var entity = Entity();
                entity.id = 42;
                entity.name = "TestEntity";

                var json_str = to_json(entity);
                var loaded = from_json(json_str);
            )");

            check_eq(eng->execute("loaded.id").as<script_int>(), 42);
            check_eq(eng->execute("loaded.name").as<std::string>(), std::string("TestEntity"));
        });

        test("cpp_class_binary_round_trip", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            dynamic_binder<SerEntity>(*eng, "Entity")
                .constructor<>()
                .property("id", &SerEntity::id)
                .property("name", &SerEntity::name)
                .build();

            eng->execute(R"(
                var entity = Entity();
                entity.id = 123;
                entity.name = "BinaryEntity";

                var binary_data = to_binary(entity);
                var loaded = from_binary(binary_data);
            )");

            check_eq(eng->execute("loaded.id").as<script_int>(), 123);
            check_eq(eng->execute("loaded.name").as<std::string>(), std::string("BinaryEntity"));
        });

        // C++ class inheritance with base_class<>() now automatically includes
        // inherited properties in serialization. No need to re-declare base properties.

        test("cpp_derived_class_round_trip", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            // Register hierarchy - inherited properties are automatically included
            dynamic_binder<SerEntity>(*eng, "Entity")
                .constructor<>()
                .property("id", &SerEntity::id)
                .property("name", &SerEntity::name)
                .build();

            dynamic_binder<SerCharacter>(*eng, "Character")
                .constructor<>()
                .base_class<SerEntity>()
                // Only declare Character-specific properties - id/name inherited automatically
                .property("health", &SerCharacter::health)
                .property("max_health", &SerCharacter::max_health)
                .build();

            eng->execute(R"(
                var hero = Character();
                hero.id = 1;
                hero.name = "Hero";
                hero.health = 80;
                hero.max_health = 100;

                var json_str = to_json(hero);
                var loaded = from_json(json_str);
            )");

            // Verify all fields including inherited
            check_eq(eng->execute("loaded.id").as<script_int>(), 1);
            check_eq(eng->execute("loaded.name").as<std::string>(), std::string("Hero"));
            check_eq(eng->execute("loaded.health").as<script_int>(), 80);
            check_eq(eng->execute("loaded.max_health").as<script_int>(), 100);
        });

        test("cpp_deep_inheritance_round_trip", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            // Register full hierarchy: Entity -> Character -> Player
            // Each class only declares its own properties - inheritance is automatic
            dynamic_binder<SerEntity>(*eng, "Entity")
                .constructor<>()
                .property("id", &SerEntity::id)
                .property("name", &SerEntity::name)
                .build();

            dynamic_binder<SerCharacter>(*eng, "Character")
                .constructor<>()
                .base_class<SerEntity>()
                .property("health", &SerCharacter::health)
                .property("max_health", &SerCharacter::max_health)
                .build();

            dynamic_binder<SerPlayer>(*eng, "Player")
                .constructor<>()
                .base_class<SerCharacter>()
                .property("experience", &SerPlayer::experience)
                .property("level", &SerPlayer::level)
                .build();

            eng->execute(R"(
                var player = Player();
                player.id = 999;
                player.name = "Champion";
                player.health = 150;
                player.max_health = 150;
                player.experience = 5000;
                player.level = 10;

                var json_str = to_json(player);
                var loaded = from_json(json_str);
            )");

            // Verify entire inheritance chain
            check_eq(eng->execute("loaded.id").as<script_int>(), 999);
            check_eq(eng->execute("loaded.name").as<std::string>(), std::string("Champion"));
            check_eq(eng->execute("loaded.health").as<script_int>(), 150);
            check_eq(eng->execute("loaded.max_health").as<script_int>(), 150);
            check_eq(eng->execute("loaded.experience").as<script_int>(), 5000);
            check_eq(eng->execute("loaded.level").as<script_int>(), 10);
        });

        // =====================================================================
        // SCRIPT INHERITANCE TESTS
        // =====================================================================

        test("script_class_inheritance_round_trip", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            eng->execute(R"(
                class Animal {
                    var name = "animal";
                    var age = 0;
                }

                class Dog : Animal {
                    var breed = "unknown";
                    var is_good_boy = true;
                }
            )");

            eng->execute(R"(
                var dog = Dog();
                dog.name = "Buddy";
                dog.age = 5;
                dog.breed = "Golden Retriever";
                dog.is_good_boy = true;

                var json_str = to_json(dog);
                var loaded = from_json(json_str);
            )");

            // Verify all fields including inherited
            check_eq(eng->execute("loaded.name").as<std::string>(), std::string("Buddy"));
            check_eq(eng->execute("loaded.age").as<script_int>(), 5);
            check_eq(eng->execute("loaded.breed").as<std::string>(), std::string("Golden Retriever"));
            check_eq(eng->execute("loaded.is_good_boy").as<bool>(), true);
        });

        test("script_deep_inheritance_round_trip", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            eng->execute(R"(
                class Base {
                    var base_val = 1;
                }

                class Middle : Base {
                    var middle_val = 2;
                }

                class Derived : Middle {
                    var derived_val = 3;
                }
            )");

            eng->execute(R"(
                var obj = Derived();
                obj.base_val = 10;
                obj.middle_val = 20;
                obj.derived_val = 30;

                var json_str = to_json(obj);
                var loaded = from_json(json_str);
            )");

            check_eq(eng->execute("loaded.base_val").as<script_int>(), 10);
            check_eq(eng->execute("loaded.middle_val").as<script_int>(), 20);
            check_eq(eng->execute("loaded.derived_val").as<script_int>(), 30);
        });

        // =====================================================================
        // MIXED INHERITANCE TESTS (Script inheriting from C++)
        // Note: Script classes inheriting from C++ classes with serialization
        // is a complex feature that may require additional infrastructure.
        // These tests document the intended behavior for future implementation.
        // =====================================================================

        // TODO: script_inheriting_cpp_class_round_trip
        // A script class extending a C++ class should serialize both the
        // C++ properties and script-defined fields, and correctly restore
        // the mixed inheritance hierarchy on deserialization.

        // TODO: script_inheriting_cpp_derived_class
        // A script class extending a C++ derived class (full chain:
        // C++ base -> C++ derived -> Script class) should serialize and
        // deserialize the complete hierarchy.

        // =====================================================================
        // TYPE PRESERVATION TESTS
        // =====================================================================

        test("type_preserved_after_deserialization", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            eng->execute(R"(
                class TypedClass {
                    var value = 42;
                    var type_marker = "TypedClass";
                }
            )");

            eng->execute(R"(
                var obj = TypedClass();
                obj.value = 99;
                var json_str = to_json(obj);
                var loaded = from_json(json_str);
            )");

            // Verify the loaded object has correct values (type preserved)
            check_eq(eng->execute("loaded.value").as<script_int>(), 99);
            check_eq(eng->execute("loaded.type_marker").as<std::string>(), std::string("TypedClass"));
        });

        test("cpp_type_preserved_after_deserialization", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            dynamic_binder<SerEntity>(*eng, "Entity")
                .constructor<>()
                .property("id", &SerEntity::id)
                .property("name", &SerEntity::name)
                .build();

            eng->execute(R"(
                var entity = Entity();
                entity.id = 1;
                entity.name = "TestEntity";
                var json_str = to_json(entity);
                var loaded = from_json(json_str);
            )");

            // Verify type preserved by checking fields
            check_eq(eng->execute("loaded.id").as<script_int>(), 1);
            check_eq(eng->execute("loaded.name").as<std::string>(), std::string("TestEntity"));
        });

        test("derived_type_preserved_not_sliced", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            dynamic_binder<SerEntity>(*eng, "Entity")
                .constructor<>()
                .property("id", &SerEntity::id)
                .property("name", &SerEntity::name)
                .build();

            dynamic_binder<SerCharacter>(*eng, "Character")
                .constructor<>()
                .base_class<SerEntity>()
                .property("health", &SerCharacter::health)
                .property("max_health", &SerCharacter::max_health)
                .build();

            eng->execute(R"(
                var hero = Character();
                hero.health = 75;
                var json_str = to_json(hero);
                var loaded = from_json(json_str);
            )");

            // Verify deserialized type retained all fields (no slicing)
            // If sliced to Entity, we wouldn't have health field
            check_eq(eng->execute("loaded.health").as<script_int>(), 75);
            check_eq(eng->execute("loaded.max_health").as<script_int>(), 100);  // Character default
        });

        // =====================================================================
        // POST-DESERIALIZE HOOK TESTS WITH INHERITANCE
        // =====================================================================

        test("post_load_called_on_derived_class", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            eng->execute(R"(
                class BaseWithHook {
                    var value = 0;
                    var base_hook_called = false;

                    function post_load(version) {
                        base_hook_called = true;
                    }
                }

                class DerivedWithHook : BaseWithHook {
                    var derived_value = 0;
                    var derived_hook_called = false;

                    override function post_load(version) {
                        derived_hook_called = true;
                        // Note: In current implementation, derived overrides base
                    }
                }
            )");

            eng->execute(R"(
                var obj = DerivedWithHook();
                obj.value = 10;
                obj.derived_value = 20;

                var json_str = to_json(obj);
                var loaded = from_json(json_str);
            )");

            // Derived hook should be called
            check_eq(eng->execute("loaded.derived_hook_called").as<bool>(), true);
            check_eq(eng->execute("loaded.value").as<script_int>(), 10);
            check_eq(eng->execute("loaded.derived_value").as<script_int>(), 20);
        });

        test("cpp_class_post_load_with_inheritance", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            // Use static variables to track hook calls
            static bool character_hook_called = false;
            static int character_hook_version = 0;
            character_hook_called = false;
            character_hook_version = 0;

            dynamic_binder<SerEntity>(*eng, "Entity")
                .constructor<>()
                .property("id", &SerEntity::id)
                .property("name", &SerEntity::name)
                .build();

            dynamic_binder<SerCharacter>(*eng, "Character")
                .constructor<>()
                .base_class<SerEntity>()
                .property("health", &SerCharacter::health)
                .property("max_health", &SerCharacter::max_health)
                .post_load_hook([](SerCharacter& self, int version) {
                    character_hook_called = true;
                    character_hook_version = version;
                    // Ensure health doesn't exceed max
                    if (self.health > self.max_health) {
                        self.health = self.max_health;
                    }
                })
                .build();

            // Create JSON with health > max_health to test hook correction
            std::string json = R"({"$type":"Character","$version":2,"$val":{"id":1,"name":"Test","health":150,"max_health":100}})";
            eng->add_global("test_json", script_value(json, eng.get()));

            eng->execute("var loaded = from_json(test_json);");

            // Hook should have been called and corrected health
            check(character_hook_called);
            check_eq(character_hook_version, 2);
            check_eq(eng->execute("loaded.health").as<script_int>(), 100);  // Clamped to max
        });

        // =====================================================================
        // ARRAY/COLLECTION OF POLYMORPHIC TYPES
        // =====================================================================

        test("array_of_script_objects_round_trip", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            eng->execute(R"(
                class Point {
                    var x = 0;
                    var y = 0;
                }
            )");

            eng->execute(R"(
                var points = [];
                for (auto i = 0; i < 3; i = i + 1) {
                    var p = Point();
                    p.x = i * 10;
                    p.y = i * 20;
                    points.push(p);
                }

                var json_str = to_json(points);
                var loaded = from_json(json_str);
            )");

            check_eq(eng->execute("loaded.size()").as<script_int>(), 3);
            check_eq(eng->execute("loaded[0].x").as<script_int>(), 0);
            check_eq(eng->execute("loaded[1].x").as<script_int>(), 10);
            check_eq(eng->execute("loaded[2].x").as<script_int>(), 20);
        });

        test("map_with_object_values_round_trip", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            eng->execute(R"(
                class Config {
                    var enabled = true;
                    var value = 0;
                }
            )");

            eng->execute(R"(
                var configs = {};

                var audio = Config();
                audio.enabled = true;
                audio.value = 80;
                configs["audio"] = audio;

                var video = Config();
                video.enabled = false;
                video.value = 60;
                configs["video"] = video;

                var json_str = to_json(configs);
                var loaded = from_json(json_str);
            )");

            check_eq(eng->execute("loaded[\"audio\"].enabled").as<bool>(), true);
            check_eq(eng->execute("loaded[\"audio\"].value").as<script_int>(), 80);
            check_eq(eng->execute("loaded[\"video\"].enabled").as<bool>(), false);
            check_eq(eng->execute("loaded[\"video\"].value").as<script_int>(), 60);
        });

        // =====================================================================
        // VERSION MIGRATION WITH INHERITANCE
        // =====================================================================

        test("version_migration_inherited_class", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            eng->execute(R"(
                class Vehicle {
                    var speed = 0;
                    var fuel = 100;
                }

                class Car : Vehicle {
                    var wheels = 4;
                    var doors = 4;
                    var migrated = false;

                    function post_load(version) {
                        if (version < 2) {
                            // Old versions didn't have doors, default to 4
                            doors = 4;
                            migrated = true;
                        }
                    }
                }
            )");

            // Simulate v1 JSON (no doors field)
            std::string v1_json = R"({"$type":"Car","$version":1,"$val":{"speed":60,"fuel":75,"wheels":4}})";
            eng->add_global("v1_json", script_value(v1_json, eng.get()));

            eng->execute("var loaded = from_json(v1_json);");

            check_eq(eng->execute("loaded.speed").as<script_int>(), 60);
            check_eq(eng->execute("loaded.fuel").as<script_int>(), 75);
            check_eq(eng->execute("loaded.wheels").as<script_int>(), 4);
            check_eq(eng->execute("loaded.doors").as<script_int>(), 4);  // Migrated default
            check_eq(eng->execute("loaded.migrated").as<bool>(), true);
        });

        // =====================================================================
        // CROSS-FORMAT CONSISTENCY
        // =====================================================================

        test("json_binary_consistency_with_inheritance", [&]() {
            auto eng = make_engine();
            stdlib::register_json_functions(*eng);

            dynamic_binder<SerEntity>(*eng, "Entity")
                .constructor<>()
                .property("id", &SerEntity::id)
                .property("name", &SerEntity::name)
                .build();

            dynamic_binder<SerCharacter>(*eng, "Character")
                .constructor<>()
                .base_class<SerEntity>()
                .property("health", &SerCharacter::health)
                .property("max_health", &SerCharacter::max_health)
                .build();

            eng->execute(R"(
                var original = Character();
                original.id = 42;
                original.name = "TestChar";
                original.health = 85;
                original.max_health = 100;

                // Round-trip through JSON
                var json_str = to_json(original);
                var from_json_obj = from_json(json_str);

                // Round-trip through Binary
                var binary_data = to_binary(original);
                var from_binary_obj = from_binary(binary_data);
            )");

            // Both should produce identical results
            check_eq(eng->execute("from_json_obj.id").as<script_int>(), 42);
            check_eq(eng->execute("from_binary_obj.id").as<script_int>(), 42);

            check_eq(eng->execute("from_json_obj.name").as<std::string>(),
                     eng->execute("from_binary_obj.name").as<std::string>());

            check_eq(eng->execute("from_json_obj.health").as<script_int>(),
                     eng->execute("from_binary_obj.health").as<script_int>());
        });
    }
};

// No registrar anywhere: identity comes implicitly from property_owner.
class ImplicitRegBase : public jai::property_owner<ImplicitRegBase> {
public:
    JAI_PROPERTY((int), baseValue, 1);
};

class ImplicitRegDerived : public jai::property_owner<ImplicitRegDerived, ImplicitRegBase> {
public:
    JAI_PROPERTY((std::string), derivedTag, "");
};

// property_owner AND an explicit registrar: the explicit name must win.
class RenamedRegType : public jai::property_owner<RenamedRegType, ImplicitRegBase> {
public:
    JAI_PROPERTY((int), marker, 7);
};
static jai::registrar<RenamedRegType, void> _renamedRegistration("CustomRegName");

namespace {
	// Anonymous-namespace spellings differ per compiler, but stripping to the bare last
	// segment yields the same clean identifier everywhere ("AnonNamespaceProbe").
	struct AnonNamespaceProbe { virtual ~AnonNamespaceProbe() = default; };

	// And a property_owner in an anonymous namespace must round-trip implicitly.
	class AnonImplicitDerived : public jai::property_owner<AnonImplicitDerived, ImplicitRegBase> {
	public:
		JAI_PROPERTY((int), anonValue, 5);
	};
}

// In-class name pin: implicit registration must use jai_type_name when declared
// (the hatch for template instantiations and classes whose identifier may change).
class InClassNamedType : public jai::property_owner<InClassNamedType, ImplicitRegBase> {
public:
    static constexpr const char* jai_type_name = "PinnedName";
    JAI_PROPERTY((int), pinned, 3);
};

// Multiple inheritance: the SECOND base sits at a non-zero offset within the derived
// object, so reconstructing a shared_ptr<PORightBase> from a dynamic POMultiDerived needs
// pointer adjustment. property_owner registers the Derived->Base casters automatically.
class POLeftBase : public jai::property_owner<POLeftBase> {
public:
    JAI_PROPERTY((int), leftVal, 0);
};
class PORightBase : public jai::property_owner<PORightBase> {
public:
    JAI_PROPERTY((int), rightVal, 0);
};
class POMultiDerived : public jai::property_owner<POMultiDerived, POLeftBase, PORightBase> {
public:
    JAI_PROPERTY((int), derivedVal, 0);
};

// Non-property_owner MI hierarchy registered through the manual escape hatches: an explicit
// registrar for identity + a manual base relation for the offset.
struct MIRawLeft {
    virtual ~MIRawLeft() = default;
    int l = 0;
    template<class Ar> void serialize(Ar& ar) { ar(jai::serialization::make_nvp("l", l)); }
};
struct MIRawRight {
    virtual ~MIRawRight() = default;
    int r = 0;
    template<class Ar> void serialize(Ar& ar) { ar(jai::serialization::make_nvp("r", r)); }
};
struct MIRawDerived : MIRawLeft, MIRawRight {
    int d = 0;
    template<class Ar> void serialize(Ar& ar) { ar(jai::serialization::make_nvp("d", d)); MIRawLeft::serialize(ar); MIRawRight::serialize(ar); }
};
static jai::registrar<MIRawDerived, void> _miRawReg("MIRawDerived");
static const bool _miRawRelation = [] {
    jai::serialization::register_relation<MIRawDerived, MIRawRight>{};
    return true;
}();

// Auto-named registrar: no name string passed — the script name and serialization $type
// are both derived from the C++ type. Not a property_owner, so the only registration is
// the registrar's, making the derived name unambiguous.
struct AutoNamedRegType {
    virtual ~AutoNamedRegType() = default;
    int value = 0;
    template<class Archive>
    void serialize(Archive& ar) { ar(jai::serialization::make_nvp("value", value)); }
};
static jai::registrar<AutoNamedRegType, void> _autoNamedRegistration;  // <-- no name

// Polymorphic, serializable, but never registered: saving through a base pointer
// must throw instead of silently slicing.
class UnregPolyBase {
public:
    virtual ~UnregPolyBase() = default;
    int value = 1;
    template<class Archive>
    void serialize(Archive& ar) {
        ar(jai::serialization::make_nvp("value", value));
    }
};

class UnregPolyDerived : public UnregPolyBase {
public:
    int extra = 2;
    template<class Archive>
    void serialize(Archive& ar) {
        ar(jai::serialization::make_nvp("extra", extra));
        UnregPolyBase::serialize(ar);
    }
};

class implicit_registration_tests : public suite {
public:
    implicit_registration_tests() : suite("Implicit Polymorphic Registration Tests") {}

    void forge_tests() override {

        test("static_type_name_extraction", [&]() {
            check_eq(std::string("ImplicitRegBase"), std::string(jai::detail::static_unqualified_type_name<ImplicitRegBase>()));
            check_eq(std::string("jai::foundry::tests::ImplicitRegBase"), std::string(jai::detail::static_type_name<ImplicitRegBase>()));
            check(jai::detail::static_unqualified_type_name<std::vector<int>>().empty(), "template instantiations must be excluded from implicit naming");
            // Anonymous-namespace types strip to their bare identifier (the compiler-specific
            // marker is in a discarded middle segment) — no namespace qualification kept.
            check_eq(std::string("AnonNamespaceProbe"), std::string(jai::detail::static_unqualified_type_name<AnonNamespaceProbe>()));
        });

        test("anonymous_namespace_property_owner_round_trip", [&]() {
            auto eng = make_engine();
            std::shared_ptr<ImplicitRegBase> original = std::make_shared<AnonImplicitDerived>();

            jai::serialization::json_archive_writer w(0, eng.get());
            w(original);
            auto json = w.str();
            check(json.find("\"AnonImplicitDerived\"") != std::string::npos, "expected the bare implicit name: " + json);
            check(json.find("AN::") == std::string::npos, "no namespace qualification should leak into the type name: " + json);

            auto loaded = roundtrip_json(*eng, original);
            check(std::dynamic_pointer_cast<AnonImplicitDerived>(loaded) != nullptr, "dynamic type lost for anonymous-namespace type");
        });

        test("property_owner_implicit_type_tag_written", [&]() {
            auto eng = make_engine();
            auto derived = std::make_shared<ImplicitRegDerived>();
            derived->derivedTag = std::string("tagged");
            std::shared_ptr<ImplicitRegBase> original = derived;

            jai::serialization::json_archive_writer w(0, eng.get());
            w(original);
            auto json = w.str();
            check(json.find("\"$type\"") != std::string::npos, "expected a $type discriminator: " + json);
            check(json.find("ImplicitRegDerived") != std::string::npos, "expected the implicit class name: " + json);
        });

        // A tampered $type naming a registered but UNRELATED type must be rejected on load,
        // not silently reinterpreted as the requested base (type confusion, B5).
        test("polymorphic_load_rejects_unrelated_type", [&]() {
            auto eng = make_engine();
            std::shared_ptr<ImplicitRegBase> original = std::make_shared<ImplicitRegDerived>();
            (void)std::make_shared<POLeftBase>();  // odr-use so POLeftBase is registered

            jai::serialization::json_archive_writer w(0, eng.get());
            w(original);
            std::string json = w.str();

            // Relabel the concrete type as POLeftBase — registered, but not a subtype of
            // ImplicitRegBase — simulating a malicious save/network payload.
            const std::string concrete = "ImplicitRegDerived";
            auto pos = json.find(concrete);
            check(pos != std::string::npos, "expected the concrete $type in: " + json);
            json.replace(pos, concrete.size(), "POLeftBase");

            jai::serialization::json_archive_reader r(json, eng.get());
            std::shared_ptr<ImplicitRegBase> out;
            check_throws([&]() { r(out); }, "unrelated $type must be rejected, not mis-cast");
        });

        test("property_owner_implicit_round_trip", [&]() {
            auto eng = make_engine();
            auto derived = std::make_shared<ImplicitRegDerived>();
            derived->derivedTag = std::string("round");
            std::shared_ptr<ImplicitRegBase> original = derived;

            auto fromJson = roundtrip_json(*eng, original);
            check(fromJson != nullptr, "json round-trip returned null");
            auto jsonDerived = std::dynamic_pointer_cast<ImplicitRegDerived>(fromJson);
            check(jsonDerived != nullptr, "dynamic type lost through json round-trip");
            check_eq(std::string("round"), jsonDerived->derivedTag.get());

            auto fromBinary = roundtrip_binary(*eng, original);
            check(fromBinary != nullptr, "binary round-trip returned null");
            auto binaryDerived = std::dynamic_pointer_cast<ImplicitRegDerived>(fromBinary);
            check(binaryDerived != nullptr, "dynamic type lost through binary round-trip");
            check_eq(std::string("round"), binaryDerived->derivedTag.get());
        });

        test("property_owner_auto_registers_base_casters", [&]() {
            // property_owner<Derived, Left, Right> registers Derived->base adjusters at
            // static init. Verify the second base's adjuster directly (its subobject is at
            // a non-zero offset) — the registry produces the same pointer a real
            // static_cast<PORightBase*> would.
            auto obj = std::make_shared<POMultiDerived>();
            void* derivedAddr = obj.get();
            void* expectedRight = static_cast<PORightBase*>(obj.get());
            check(expectedRight != derivedAddr, "test premise: PORightBase subobject must be at a non-zero offset");

            void* adjusted = jai::serialization::polymorphic_registry::instance().adjust_to_base(
                std::type_index(typeid(POMultiDerived)), std::type_index(typeid(PORightBase)), derivedAddr);
            check(adjusted == expectedRight, "auto-registered caster must adjust a POMultiDerived* to its PORightBase subobject");

            // First base is at offset zero; adjuster (if any) must be identity.
            void* expectedLeft = static_cast<POLeftBase*>(obj.get());
            void* adjustedLeft = jai::serialization::polymorphic_registry::instance().adjust_to_base(
                std::type_index(typeid(POMultiDerived)), std::type_index(typeid(POLeftBase)), derivedAddr);
            check(adjustedLeft == expectedLeft, "first-base adjustment must match static_cast");
        });

        test("multiple_inheritance_manual_base_relation", [&]() {
            auto eng = make_engine();
            auto obj = std::make_shared<MIRawDerived>();
            obj->l = 5; obj->r = 6; obj->d = 7;
            std::shared_ptr<MIRawRight> asRight = obj;
            check(static_cast<const void*>(asRight.get()) != static_cast<const void*>(obj.get()),
                "test premise: MIRawRight subobject must be at a non-zero offset");

            auto loaded = roundtrip_json(*eng, asRight);
            check(loaded != nullptr, "manual MI round-trip returned null");
            check_eq(6, loaded->r);                    // adjusted base data correct
            auto full = std::dynamic_pointer_cast<MIRawDerived>(loaded);
            check(full != nullptr, "manual MI dynamic type lost");
            check_eq(5, full->l);
            check_eq(7, full->d);
        });

        test("registrar_auto_name_no_string_feeds_both_registries", [&]() {
            // Script-name registry got the bare class name, with no string ever passed.
            auto scriptName = jai::type_name_registry::instance().get_name<AutoNamedRegType>();
            check(scriptName.has_value(), "auto-named type missing from type_name_registry");
            check_eq(std::string("AutoNamedRegType"), std::string(*scriptName));
            // Serialization registry got the same name, from the same derivation.
            auto* entry = jai::serialization::polymorphic_registry::instance().find(std::type_index(typeid(AutoNamedRegType)));
            check(entry != nullptr, "auto-named type missing from polymorphic_registry");
            check_eq(std::string("AutoNamedRegType"), entry->name);
        });

        test("explicit_registrar_overrides_implicit_name", [&]() {
            auto eng = make_engine();
            std::shared_ptr<ImplicitRegBase> original = std::make_shared<RenamedRegType>();

            jai::serialization::json_archive_writer w(0, eng.get());
            w(original);
            auto json = w.str();
            check(json.find("CustomRegName") != std::string::npos, "explicit registrar name must win: " + json);
            check(json.find("\"RenamedRegType\"") == std::string::npos, "implicit name must not be used once an explicit one exists: " + json);

            auto loaded = roundtrip_json(*eng, original);
            check(std::dynamic_pointer_cast<RenamedRegType>(loaded) != nullptr, "round-trip through the explicit name failed");
        });

        test("in_class_jai_type_name_pins_the_name", [&]() {
            auto eng = make_engine();
            std::shared_ptr<ImplicitRegBase> original = std::make_shared<InClassNamedType>();

            jai::serialization::json_archive_writer w(0, eng.get());
            w(original);
            auto json = w.str();
            check(json.find("PinnedName") != std::string::npos, "jai_type_name must be used: " + json);
            check(json.find("InClassNamedType") == std::string::npos, "derived class identifier must not leak into the data: " + json);

            auto loaded = roundtrip_json(*eng, original);
            check(std::dynamic_pointer_cast<InClassNamedType>(loaded) != nullptr, "round-trip through the pinned name failed");
        });

        test("unregistered_polymorphic_save_throws", [&]() {
            auto eng = make_engine();
            std::shared_ptr<UnregPolyBase> original = std::make_shared<UnregPolyDerived>();

            check_throws([&]() {
                jai::serialization::json_archive_writer w(0, eng.get());
                w(original);
            }, "saving an unregistered polymorphic type through a base pointer must throw");

            // Static type == dynamic type stays legal without registration.
            auto exact = std::make_shared<UnregPolyDerived>();
            exact->extra = 9;
            auto loaded = roundtrip_json(*eng, exact);
            check(loaded != nullptr, "non-polymorphic-context save must still work");
            check_eq(9, loaded->extra);
        });

        // dynamic_binder::base_class<>() must register the polymorphic assignability edge (not
        // just engine inheritance), so a shared_ptr<Base> with a derived $type passes the B5 load
        // check. Regression for the FileTextureDefinition-through-base load failure.
        test("base_class_registers_polymorphic_assignability_edge", [&]() {
            auto eng = make_engine();
            dynamic_binder<EdgeDerived>(*eng, "EdgeDerived").constructor<>().base_class<EdgeBase>().build();
            auto& reg = jai::serialization::polymorphic_registry::instance();
            check_true(reg.is_assignable(std::type_index(typeid(EdgeDerived)), std::type_index(typeid(EdgeBase))));
            check_false(reg.is_assignable(std::type_index(typeid(EdgeBase)), std::type_index(typeid(EdgeDerived))));
        });

        // register_relation<Derived, Base> declares the same edge externally (for friend-access /
        // third-party types that can't use property_owner or .base_class<>()).
        test("register_relation_registers_assignability_edge", [&]() {
            jai::serialization::register_relation<RelDerived, RelBase> _r;
            auto& reg = jai::serialization::polymorphic_registry::instance();
            check_true(reg.is_assignable(std::type_index(typeid(RelDerived)), std::type_index(typeid(RelBase))));
        });

        // A script handle (new P(), shared_ptr<P>) writes its object once and every later handle to it as its id, in every archive;
        // the load hands back handles to one object, weak_ptrs to it included
        test("script_handles_round_trip_through_every_archive", [&]() {
            auto eng = make_engine();
            stdlib::register_all(*eng);
            auto r = eng->execute(R"(
                class P { int x = 3; string s = "hi"; }
                class N { int v = 0; shared_ptr<N> next; weak_ptr<N> prev; }
                class H { weak_ptr<P> w; shared_ptr<P> s; }
                function trip(var v, string how) -> var {
                    if (how == "json") { return from_json(to_json(v)); }
                    if (how == "binary") { return from_binary(to_binary(v)); }
                    return from_base64(to_base64(v));
                }
                string out = "";
                for (string how : ["json", "binary", "base64"]) {
                    shared_ptr<P> p = new P();
                    p.x = 7;
                    var q = trip(p, how);
                    out += how + ": " + type_of(q) + " " + to_string(q.x) + " " + q.s;
                    var alias = q;
                    alias.x = 9;
                    out += " handle " + to_string(q.x);
                    var held = new P();
                    var held2 = trip(held, how);
                    out += " var " + to_string(held2.x);
                    array<shared_ptr<P>> pair = [p, p];
                    var pair2 = trip(pair, how);
                    out += " aliased " + to_string(same_object(pair2[0], pair2[1]));
                    var a = new N();
                    var b = new N();
                    a.v = 1;
                    b.v = 2;
                    a.next = b;
                    b.prev = a;
                    b.next = a;
                    var a2 = trip(a, how);
                    out += " cycle " + to_string(a2.next.v) + " " + to_string(same_object(a2.next.next, a2)) + " " + to_string(same_object(a2.next.prev.lock(), a2));
                    var h = new H();
                    h.s = p;
                    h.w = p;
                    var h2 = trip(h, how);
                    out += " weak " + to_string(same_object(h2.w.lock(), h2.s)) + " " + to_string(h2.s.x);
                    shared_ptr<P> none = null;
                    out += " null " + to_string(trip(none, how) == null) + "\n";
                }
                out;
            )");
            const std::string line = " P 7 hi handle 9 var 3 aliased true cycle 2 true true weak true 7 null true\n";
            check_eq("json:" + line + "binary:" + line + "base64:" + line, r.as<std::string>());
        });

        test("script_handle_id_without_its_object_is_refused", [&]() {
            auto eng = make_engine();
            stdlib::register_all(*eng);
            auto r = eng->execute(R"(
                string out = "";
                try { from_json("{\"$id\": 4}"); out = "loaded"; } catch (e) { out = e; }
                out += " | " + type_of(from_json("{\"$ref\": 4}"));
                try { from_json("{\"$id\": 1, \"$val\": {\"a\": {\"$id\": 1}}}"); out += " | loaded"; } catch (e) { out += " | " + e; }
                out;
            )");
            check_eq(std::string("Invalid shared_ptr ID: 4 | weak_ptr | Invalid shared_ptr ID: 1"), r.as<std::string>());
        });
    }
};

// ============================================================================
// Script layers: a script class deriving a bound C++ class (docs/site/embedding/13-script-extensible.html "Serialization")
// ============================================================================

// Two C++ bases, one per lookup path: HybridCreature carries no slot (the engine map),
// SlotCreature derives script_extensible (the slot). Both registered polymorphically for
// the archive $type path and bound per-engine so scripts can derive them.
struct HybridCreature {
    virtual ~HybridCreature() = default;
    int health = 100;
    std::string label = "creature";
    virtual std::string speak() { return "grr"; }
    template<class Ar> void serialize(Ar& ar) {
        ar(jai::serialization::make_nvp("health", health), jai::serialization::make_nvp("label", label));
    }
};
static jai::registrar<HybridCreature, void> _hybridCreatureReg("HybridCreature");

struct SlotCreature : jai::script_extensible {
    virtual ~SlotCreature() = default;
    int health = 100;
    std::string label = "creature";
    virtual std::string speak() { return "grr"; }
    template<class Ar> void serialize(Ar& ar) {
        ar(jai::serialization::make_nvp("health", health), jai::serialization::make_nvp("label", label));
    }
};
static jai::registrar<SlotCreature, void> _slotCreatureReg("SlotCreature");

// A base with no default constructor: implicit base construction must name it
struct ArgOnlyCreature : jai::script_extensible {
    explicit ArgOnlyCreature(int h) : health(h) {}
    virtual ~ArgOnlyCreature() = default;
    int health;
};

// A base built only through load_and_construct: its one constructor is private and takes the saved health
struct LacCreature : jai::script_extensible {
    virtual ~LacCreature() = default;
    int health = 0;
    std::string label = "lac";
    static std::shared_ptr<LacCreature> make(int a_health) { return std::shared_ptr<LacCreature>(new LacCreature(a_health)); }
    template<class Ar> void serialize(Ar& ar) {
        ar(jai::serialization::make_nvp("health", health), jai::serialization::make_nvp("label", label));
    }
private:
    friend jai::access;
    explicit LacCreature(int a_health) : health(a_health) {}
    template<class Ar> static void load_and_construct(Ar& ar, jai::serialization::construct<LacCreature>& a_construct) {
        int savedHealth = 0;
        std::string savedLabel;
        ar(jai::serialization::make_nvp("health", savedHealth), jai::serialization::make_nvp("label", savedLabel));
        a_construct(savedHealth);
        a_construct->label = savedLabel;
    }
};
static jai::registrar<LacCreature, void> _lacCreatureReg("LacCreature");

// A base whose default constructor is private: a load reaches it through friend jai::access
struct SealedCreature : jai::script_extensible {
    virtual ~SealedCreature() = default;
    int health = 100;
    std::string label = "sealed";
    static std::shared_ptr<SealedCreature> make() { return std::shared_ptr<SealedCreature>(new SealedCreature()); }
    template<class Ar> void serialize(Ar& ar) {
        ar(jai::serialization::make_nvp("health", health), jai::serialization::make_nvp("label", label));
    }
private:
    friend jai::access;
    SealedCreature() = default;
};
static jai::registrar<SealedCreature, void> _sealedCreatureReg("SealedCreature");

// A base with neither: a load says what is missing
struct StubbornCreature : jai::script_extensible {
    explicit StubbornCreature(int a_health) : health(a_health) {}
    virtual ~StubbornCreature() = default;
    int health;
    template<class Ar> void serialize(Ar& ar) { ar(jai::serialization::make_nvp("health", health)); }
};
static jai::registrar<StubbornCreature, void> _stubbornCreatureReg("StubbornCreature");

// A base with an observable property: a load writes it without notifying
struct WatchCreature : jai::property_owner<WatchCreature> {
    JAI_OBSERVABLE_PROPERTY((int), health, 100);
    JAI_PROPERTY((int), armor, 1);
    virtual ~WatchCreature() = default;
};
static jai::registrar<WatchCreature, void> _watchCreatureReg("WatchCreature");

// A base whose copy constructor is deleted: copying its hybrid must refuse (S4).
struct NoCopyCreature : jai::script_extensible {
    NoCopyCreature() = default;
    NoCopyCreature(const NoCopyCreature&) = delete;
    NoCopyCreature& operator=(const NoCopyCreature&) = delete;
    virtual ~NoCopyCreature() = default;
    int health = 100;
};

// The extend / add_global order matrix: one base per lookup path (the engine map for a polymorphic type, the slot, the map
// for a non-polymorphic type) and a derived C++ type whose binder registers after its object was extended
struct OrderPoly {
    virtual ~OrderPoly() = default;
    int health = 100;
};
struct OrderElite : OrderPoly {
    int rank = 1;
};
struct OrderSlot : jai::script_extensible {
    virtual ~OrderSlot() = default;
    int health = 100;
};
struct OrderPlain {
    int health = 100;
};
// A member at offset 0 shares its enclosing object's address and, through an aliasing handle, its owner
struct NestedPart {
    int depth = 7;
};
struct NestedWhole {
    NestedPart part;
    int width = 3;
};
struct NestedPolyPart {
    virtual ~NestedPolyPart() = default;
    int depth = 8;
};
struct NestedPolyWhole {
    NestedPolyPart part;
    int width = 4;
};

struct hybrid_holder {
    std::shared_ptr<HybridCreature> pet;
    int tag = 0;
    template<class Ar> void serialize(Ar& ar) {
        ar(jai::serialization::make_nvp("pet", pet), jai::serialization::make_nvp("tag", tag));
    }
};

struct slot_holder {
    std::shared_ptr<SlotCreature> pet;
    int tag = 0;
    template<class Ar> void serialize(Ar& ar) {
        ar(jai::serialization::make_nvp("pet", pet), jai::serialization::make_nvp("tag", tag));
    }
};

static_assert(std::is_base_of_v<jai::script_extensible, ImplicitRegDerived>, "every property_owner is script_extensible");
static_assert(std::is_base_of_v<jai::script_extensible, POMultiDerived>, "multi-base property_owner reaches a slot through its bases");

class hybrid_serialization_tests : public suite {
public:
    hybrid_serialization_tests() : suite("Hybrid Serialization Tests") {}

    // Every assertion runs on both bases so the slot and map paths cannot drift.
    std::shared_ptr<engine> hybrid_engine() {
        auto eng = make_engine();
        stdlib::register_json_functions(*eng);
        dynamic_binder<HybridCreature>(*eng, "HybridCreature")
            .constructor<>()
            .property("health", &HybridCreature::health)
            .property("label", &HybridCreature::label)
            .method("speak", &HybridCreature::speak)
            .build();
        dynamic_binder<SlotCreature>(*eng, "SlotCreature")
            .constructor<>()
            .property("health", &SlotCreature::health)
            .property("label", &SlotCreature::label)
            .method("speak", &SlotCreature::speak)
            .build();
        dynamic_binder<NoCopyCreature>(*eng, "NoCopyCreature")
            .constructor<>()
            .property("health", &NoCopyCreature::health)
            .build();
        eng->execute(R"(
            class Voidkin : HybridCreature {
                int chomp = 3;
                int untouched = 11;
                Voidkin() : super() {}
                string speak() override { return "voidkin"; }
            }
            class Slotkin : SlotCreature {
                int chomp = 3;
                int untouched = 11;
                Slotkin() : super() {}
                string speak() override { return "slotkin"; }
            }
            class Lockkin : NoCopyCreature {
                int chomp = 3;
                Lockkin() : super() {}
            }
            var v = Voidkin();
            v.health = 42;
            v.label = "gnasher";
            v.chomp = 7;
            var s = Slotkin();
            s.health = 42;
            s.label = "gnasher";
            s.chomp = 7;
        )");
        return eng;
    }

    std::shared_ptr<engine> order_engine() {
        auto eng = make_engine();
        stdlib::register_all(*eng);
        dynamic_binder<OrderPoly>(*eng, "OrderPoly").constructor<>().property("health", &OrderPoly::health).build();
        dynamic_binder<OrderSlot>(*eng, "OrderSlot").constructor<>().property("health", &OrderSlot::health).build();
        dynamic_binder<OrderPlain>(*eng, "OrderPlain").constructor<>().property("health", &OrderPlain::health).build();
        eng->execute(R"(
            class PolyKin : OrderPoly { int chomp = 3; PolyKin() : super() {} }
            class SlotKin : OrderSlot { int chomp = 3; SlotKin() : super() {} }
            class PlainKin : OrderPlain { int chomp = 3; PlainKin() : super() {} }
        )");
        return eng;
    }

    // Every entry hands script the object's one identity: its layer once extended, whatever the order
    template<typename T>
    void every_entry_is_the_layer(const std::shared_ptr<engine>& eng, const std::shared_ptr<T>& object, const std::string& kin,
                                  const std::string& order) {
        try { every_entry_is_the_layer_checked(eng, object, kin, order); }
        catch (const std::exception& e) { check(false, order + " threw: " + e.what()); }
    }
    template<typename T>
    void every_entry_is_the_layer_checked(const std::shared_ptr<engine>& eng, const std::shared_ptr<T>& object, const std::string& kin,
                                          const std::string& order) {
        auto is_kin = [&](const std::string& expr, const std::string& what) {
            check_eq(kin, eng->execute("type_of(" + expr + ")").as<std::string>(), order + ": " + what);
            check_eq((script_int)3, eng->execute(expr + ".chomp").as_int(), order + ": " + what + " reaches the script field");
        };
        is_kin("hero", "the global");
        eng->execute("hero.chomp = 9;");
        check_eq((script_int)9, eng->execute("hero.chomp").as_int(), order + ": the global is the stored layer");
        eng->execute("hero.chomp = 3;");
        eng->add_global("via_make", eng->make_object(object));
        is_kin("via_make", "make_object");
        check_true(eng->execute("same_object(hero, via_make)").as<bool>(), order + ": make_object is the same object");
        eng->add_global_ref("via_ref", *object);
        is_kin("via_ref", "add_global_ref");
        eng->add_function("crowd", [object]() { return std::vector<std::shared_ptr<T>>{object}; });
        is_kin("crowd()[0]", "a container element");
        eng->add_function("visit", [object, e = eng.get()](const script_value& f) -> std::string {
            auto ran = f.as_function()({e->make_value(object)});
            return ran ? ran.value().template as<std::string>() : std::string("call failed");
        });
        check_eq(kin, eng->execute("visit(function(var x) -> string { return type_of(x); })").as<std::string>(), order + ": a callback argument");
        check_eq((script_int)3, eng->execute("hero.health - 97").as_int(), order + ": the C++ property reads through the layer");
    }

    template<typename T>
    void order_matrix(const std::string& kin) {
        auto run = [&](const std::string& order, auto&& steps) {
            auto eng = order_engine();
            auto object = std::make_shared<T>();
            try { steps(eng, object); }
            catch (const std::exception& e) { check(false, kin + " " + order + " threw: " + e.what()); return; }
            every_entry_is_the_layer(eng, object, kin, kin + " " + order);
        };
        run("extend then add_global", [&](auto& eng, auto& object) { (void)eng->extend(object, kin); eng->add_global("hero", object); });
        run("add_global then C++ extend", [&](auto& eng, auto& object) { eng->add_global("hero", object); (void)eng->extend(object, kin); });
        run("add_global then script extend", [&](auto& eng, auto& object) { eng->add_global("hero", object); eng->execute("extend(hero, " + kin + ");"); });
        run("two globals then extend", [&](auto& eng, auto& object) {
            eng->add_global("hero", eng->make_object(object));
            eng->add_global("again", object);
            (void)eng->extend(object, kin);
            check_eq(kin, eng->execute("type_of(again)").template as<std::string>(), kin + " two globals then extend: the second global follows");
        });
    }
    // The observable identity of a layer reached from C++: the boundary return IS the layer
    // (script field, override, inherited C++ property all resolve through it).
    template<typename Base>
    void check_layer(const std::shared_ptr<engine>& eng, const std::shared_ptr<Base>& object, const char* global_name,
                     const char* speak, script_int chomp = 7, script_int health = 42) {
        check(eng->script_layer(*object) != nullptr, "object carries a script layer");
        eng->add_global(global_name, eng->make_object(object));
        std::string g(global_name);
        check_eq(chomp, eng->execute(g + ".chomp").as_int(), "script field through the layer");
        check_eq(std::string(speak), eng->execute(g + ".speak()").as<std::string>(), "script override dispatches");
        check_eq(health, eng->execute(g + ".health").as_int(), "C++ property reads through _cpp_object");
    }

    template<typename Base>
    void archive_round_trips(const char* var, const char* cls, const char* speak) {
        auto eng = hybrid_engine();
        auto original = eng->execute(var).as<std::shared_ptr<Base>>();
        check(original != nullptr, "hybrid extracts to its C++ base");
        check_eq(42, original->health);

        jai::serialization::json_archive_writer w(0, eng.get());
        w(original);
        std::string json = w.str();
        check(json.find(cls) != std::string::npos, "$type is the script class: " + json);
        check(json.find("$script") != std::string::npos, "script fields ride along: " + json);

        jai::serialization::json_archive_reader r(json, eng.get());
        std::shared_ptr<Base> loaded;
        r(loaded);
        check(loaded != nullptr);
        check(loaded.get() != original.get(), "a fresh object, not the original");
        check_eq(42, loaded->health);
        check_eq(std::string("gnasher"), loaded->label);
        check_layer(eng, loaded, "loadedJson", speak);

        auto loadedBin = roundtrip_binary(*eng, original);
        check(loadedBin != nullptr);
        check_eq(42, loadedBin->health);
        check_layer(eng, loadedBin, "loadedBinary", speak);
    }

    template<typename Holder, typename Base>
    void holder_round_trips(const char* var, const char* speak) {
        auto eng = hybrid_engine();
        Holder original;
        original.pet = eng->execute(var).as<std::shared_ptr<Base>>();
        original.tag = 5;

        auto json = jai::to_json(*eng, original);
        auto loaded = jai::from_json<Holder>(*eng, json);
        check_eq(5, loaded.tag);
        check(loaded.pet != nullptr);
        check_eq(42, loaded.pet->health);
        check_layer(eng, loaded.pet, "loadedHeldJson", speak);

        auto binary = jai::to_binary(*eng, original);
        auto loadedBin = jai::from_binary<Holder>(*eng, binary);
        check_eq(5, loadedBin.tag);
        check(loadedBin.pet != nullptr);
        check_eq(42, loadedBin.pet->health);
        check_layer(eng, loadedBin.pet, "loadedHeldBinary", speak);
    }

    // A script class deriving a SCRIPT class that derives the C++ base: cpp_base_class_ lives
    // only on the middle class, so every layer path chain-walks (find_cpp_base_class).
    template<typename Base>
    void two_level_chain(const char* mid, const char* leaf, const char* speak) {
        auto eng = hybrid_engine();
        std::string decl = std::string("class ") + leaf + " : " + mid + R"( {
                int fangs = 1;
                )" + leaf + R"(() : super() {}
                string speak() override { return ")" + speak + R"("; }
            }
            var deep = )" + leaf + R"(();
            deep.health = 64;
            deep.chomp = 8;
            deep.fangs = 4;)";
        eng->execute(decl);
        auto original = eng->execute("deep").as<std::shared_ptr<Base>>();
        check(original != nullptr, "two-level hybrid extracts to the C++ base");

        jai::serialization::json_archive_writer w(0, eng.get());
        w(original);
        std::string json = w.str();
        check(json.find(leaf) != std::string::npos, "$type is the most-derived script class: " + json);

        jai::serialization::json_archive_reader r(json, eng.get());
        std::shared_ptr<Base> loaded;
        r(loaded);
        check(loaded != nullptr && loaded->health == 64);
        check_layer(eng, loaded, "loadedDeep", speak, 8, 64);
        check_eq((script_int)4, eng->execute("loadedDeep.fangs").as_int(), "leaf-class script field survives");

        auto loadedBin = roundtrip_binary(*eng, original);
        check(loadedBin != nullptr && loadedBin->health == 64, "binary round trip");
        check(eng->script_layer(*loadedBin) != nullptr, "binary re-attach");
    }

    // A C++ boundary return of a base pointer hands back THE layer: fields and overrides
    // visible, mutation through the return reaches the live instance, a local bound to the
    // return shares it.
    template<typename Base>
    void boundary_identity(const char* var, const char* speak) {
        auto eng = hybrid_engine();
        auto ptr = eng->execute(var).as<std::shared_ptr<Base>>();
        eng->add_function("fetch", [ptr]() { return ptr; });
        check_eq((script_int)7, eng->execute("fetch().chomp").as_int(), "script field visible through boundary return");
        check_eq(std::string(speak), eng->execute("fetch().speak()").as<std::string>(), "override dispatches through boundary return");
        eng->execute("fetch().chomp = 9;");
        check_eq((script_int)9, eng->execute(std::string(var) + ".chomp").as_int(), "mutation through the return reaches the live instance");
        check_eq((script_int)9, eng->execute("var held = fetch(); held.chomp").as_int(), "a local bound to the return shares the instance");
    }

    // engine::extend: an existing C++ object gains a layer with no constructor - field
    // defaults apply, the object carries the layer, the identity survives serialization.
    template<typename Base>
    void extend_existing(const char* cls, const char* speak) {
        auto eng = hybrid_engine();
        auto cpp = std::make_shared<Base>();
        cpp->health = 55;
        auto layer = eng->extend(cpp, cls);
        check(layer.is_object(), "extend returns the layer");
        auto first = eng->script_layer(*cpp);
        check(first != nullptr, "the object carries its layer");
        (void)eng->extend(cpp, cls);
        check(eng->script_layer(*cpp) == first, "extending again with the same class keeps the layer");
        eng->add_function("adopted", [cpp]() { return cpp; });
        check_eq((script_int)3, eng->execute("adopted().chomp").as_int(), "field initializers ran on extend");
        check_eq((script_int)11, eng->execute("adopted().untouched").as_int(), "every declared initializer ran");
        check_eq((script_int)55, eng->execute("adopted().health").as_int(), "C++ state reads through the layer");
        check_eq(std::string(speak), eng->execute("adopted().speak()").as<std::string>(), "override dispatches on the extended object");
        eng->execute("adopted().chomp = 12;");

        std::shared_ptr<Base> base = cpp;
        auto loaded = roundtrip_binary(*eng, base);
        check(loaded != nullptr && loaded->health == 55);
        check_layer(eng, loaded, "loadedExtended", speak, 12, 55);
        check_eq((script_int)11, eng->execute("loadedExtended.untouched").as_int(), "an initialized, never-written field survives the round trip");
    }

    // A plain C++ object crossing the boundary registers no layer.
    template<typename Base>
    void no_pollution() {
        auto eng = hybrid_engine();
        auto plain = std::make_shared<Base>();
        eng->add_global("plainObj", eng->make_object(plain));
        check_eq((script_int)100, eng->execute("plainObj.health").as_int());
        check(eng->script_layer(*plain) == nullptr, "a pure C++ wrapper must not claim a layer");
    }

    // Copy semantics follow the C++ base (S4): assignment deep-copies a copyable base into a
    // second identity; a non-copyable base refuses at the copy site; a by-value capture copies as assignment does.
    template<typename Base>
    void copy_semantics(const char* var) {
        auto eng = hybrid_engine();
        std::string v(var);
        eng->execute("var copy = " + v + "; copy.chomp = 99; copy.health = 1;");
        check_eq((script_int)7, eng->execute(v + ".chomp").as_int(), "assignment copied the script layer");
        check_eq((script_int)42, eng->execute(v + ".health").as_int(), "assignment copied the C++ object too");
        auto original = eng->execute(var).as<std::shared_ptr<Base>>();
        auto copied = eng->execute("copy").as<std::shared_ptr<Base>>();
        check(original.get() != copied.get(), "two C++ identities after a copy");
        check(eng->script_layer(*copied) != nullptr, "the copy carries its own layer");
        eng->execute("auto pin = [" + v + "]() -> void { " + v + ".chomp = 55; }; pin();");
        check_eq((script_int)7, eng->execute(v + ".chomp").as_int(), "a by-value capture holds its own copy of the hybrid");
    }

    void forge_tests() override {

        test("hybrid_archive_round_trip", [&]() {
            archive_round_trips<HybridCreature>("v", "Voidkin", "voidkin");
            archive_round_trips<SlotCreature>("s", "Slotkin", "slotkin");
        });

        test("hybrid_inside_owning_struct_round_trip", [&]() {
            holder_round_trips<hybrid_holder, HybridCreature>("v", "voidkin");
            holder_round_trips<slot_holder, SlotCreature>("s", "slotkin");
        });

        // Property reads are by name, not position: consuming the holder's members in the
        // opposite of written order must work in both archive formats (the $type/$script/$val
        // block included).
        test("hybrid_out_of_order_property_reads", [&]() {
            auto eng = hybrid_engine();
            hybrid_holder original;
            original.pet = eng->execute("v").as<std::shared_ptr<HybridCreature>>();
            original.tag = 5;

            {
                jai::serialization::json_archive_writer w(0, eng.get());
                w(original);
                jai::serialization::json_archive_reader r(w.str(), eng.get());
                std::string tn; uint32_t ver{};
                r.begin_object(tn, ver);
                int tag{};
                r.serialize("tag", tag);
                std::shared_ptr<HybridCreature> pet;
                r.serialize("pet", pet);
                r.end_object();
                check_eq(5, tag);
                check(pet != nullptr && pet->health == 42, "json member read out of written order");
            }
            {
                jai::serialization::binary_archive_writer w(eng.get());
                w(original);
                auto& d = w.data();
                jai::serialization::binary_archive_reader r(d.data(), d.size(), eng.get());
                std::string tn; uint32_t ver{};
                r.begin_object(tn, ver);
                int tag{};
                r.serialize("tag", tag);
                std::shared_ptr<HybridCreature> pet;
                r.serialize("pet", pet);
                r.end_object();
                check_eq(5, tag);
                check(pet != nullptr && pet->health == 42, "binary member read out of written order");
            }
        });

        test("hybrid_stdlib_value_round_trip", [&]() {
            auto eng = hybrid_engine();
            for (const char* var : {"v", "s"}) {
                std::string x(var);
                eng->execute("var " + x + "j = from_json(to_json(" + x + ")); var " + x + "b = from_binary(to_binary(" + x + "));");
                check_eq((script_int)42, eng->execute(x + "j.health").as_int());
                check_eq(std::string("gnasher"), eng->execute(x + "j.label").as<std::string>());
                check_eq((script_int)7, eng->execute(x + "j.chomp").as_int());
                check_eq((script_int)42, eng->execute(x + "b.health").as_int());
                check_eq((script_int)7, eng->execute(x + "b.chomp").as_int());
            }
            check_eq(std::string("voidkin"), eng->execute("vj.speak()").as<std::string>());
            check_eq(std::string("slotkin"), eng->execute("sb.speak()").as<std::string>());
        });

        test("cpp_object_handle_stays_out_of_json", [&]() {
            auto eng = hybrid_engine();
            for (const char* var : {"v", "s"}) {
                std::string json = eng->execute(std::string("to_json(") + var + ")").as<std::string>();
                check(json.find("_cpp_object") == std::string::npos, "the _cpp_object handle must not leak: " + json);
                check(json.find("chomp") != std::string::npos, "script fields serialize: " + json);
                check(json.find("health") != std::string::npos, "inherited C++ properties serialize: " + json);
            }
        });

        test("hybrid_two_level_script_chain_round_trip", [&]() {
            two_level_chain<HybridCreature>("Voidkin", "Voidspawn", "voidspawn");
            two_level_chain<SlotCreature>("Slotkin", "Slotspawn", "slotspawn");
        });

        test("hybrid_boundary_return_is_owning_instance", [&]() {
            boundary_identity<HybridCreature>("v", "voidkin");
            boundary_identity<SlotCreature>("s", "slotkin");
        });

        test("extend_existing_cpp_object", [&]() {
            extend_existing<HybridCreature>("Voidkin", "voidkin");
            extend_existing<SlotCreature>("Slotkin", "slotkin");
        });

        test("pure_cpp_crossing_registers_no_layer", [&]() {
            no_pollution<HybridCreature>();
            no_pollution<SlotCreature>();
        });

        test("hybrid_copy_semantics_follow_the_cpp_base", [&]() {
            copy_semantics<HybridCreature>("v");
            copy_semantics<SlotCreature>("s");
            auto eng = hybrid_engine();
            eng->execute("var locked = Lockkin();");
            std::string message;
            try { eng->execute("var stolen = locked;"); } catch (const std::exception& e) { message = e.what(); }
            check(message.find("nocopy class 'Lockkin'") != std::string::npos,
                  "a hybrid over a non-copyable base is nocopy by inheritance: " + message);
            check_eq((script_int)3, eng->execute("locked.chomp").as_int(), "the original is untouched");
        });

        test("extend_errors_name_both_types", [&]() {
            auto eng = hybrid_engine();
            auto slot = std::make_shared<SlotCreature>();
            std::string message;
            try { (void)eng->extend(slot, "Voidkin"); } catch (const std::exception& e) { message = e.what(); }
            check(message.find("Voidkin") != std::string::npos && message.find("SlotCreature") != std::string::npos,
                  "a class over the wrong C++ base names both: " + message);
            check(eng->script_layer(*slot) == nullptr, "a refused extend attaches nothing");

            message.clear();
            try { (void)eng->extend(slot, "NoSuchClass"); } catch (const std::exception& e) { message = e.what(); }
            check(message.find("NoSuchClass") != std::string::npos, "unknown script class is named: " + message);

            std::shared_ptr<SlotCreature> null_object;
            message.clear();
            try { (void)eng->extend(null_object, "Slotkin"); } catch (const std::exception& e) { message = e.what(); }
            check_eq(std::string("extend: 'Slotkin' needs an object to extend"), message, "the host and the script builtin refuse null alike");

            // the script builtin reports through the error channel: catchable in script
            eng->add_global("rawSlot", eng->make_object(slot));
            check_eq(std::string("refused"), eng->execute(R"(
                var outcome = "silent";
                try { extend(rawSlot, Voidkin); } catch (e) { outcome = "refused"; }
                outcome;
            )").as<std::string>(), "script sees the rejection as an exception");
            check_throws([&]() { eng->execute("extend(rawSlot, 5);"); }, "a non-class second argument is refused");
            check_throws([&]() { eng->execute("extend(rawSlot);"); }, "arity is enforced");
        });

        // Re-derive: another class over an already layered object replaces the layer, carrying
        // the fields both classes declare and retiring the old layer; the same class is idempotent.
        test("extend_rederives_and_is_idempotent", [&]() {
            auto eng = hybrid_engine();
            eng->execute(R"(
                class Slotspawn : Slotkin {
                    int fangs = 1;
                    Slotspawn() : super() {}
                    string speak() override { return "slotspawn"; }
                }
            )");
            auto slot = std::make_shared<SlotCreature>();
            eng->add_global("raw", eng->make_object(slot));
            eng->add_global("same", eng->make_object(slot));
            eng->execute("extend(raw, Slotkin); raw.chomp = 9;");
            check(eng->script_layer(*slot) != nullptr, "script extend attached the layer");
            check_eq((script_int)9, eng->execute("extend(raw, Slotkin).chomp").as_int(), "same class returns the live layer");
            check_eq((script_int)9, eng->execute("extend(raw, \"Slotkin\").chomp").as_int(), "a string names the class too");
            check_eq((script_int)9, eng->execute("extend(same, Slotkin).chomp").as_int(), "another handle to the object is the same layer");

            check(eng->execute("raw").get_class_instance() == eng->script_layer(*slot), "raw is the layer after the first extend");
            eng->execute("extend(raw, Slotspawn);");
            check_eq(std::string("Slotspawn"), eng->script_layer(*slot)->get_class_definition()->get_name(), "the object carries the new layer");
            check(eng->execute("raw").get_class_instance() == eng->script_layer(*slot), "raw is still the layer after the re-derive");
            check_eq(std::string("Slotspawn"), eng->execute("raw").get_class_instance()->get_class_definition()->get_name(), "raw's instance is Slotspawn");
            check_eq((script_int)9, eng->execute("raw.chomp").as_int(), "a field both classes declare carries over");
            check(eng->script_layer(*slot)->has_field(detail::engine_access::symbolize(*eng, "fangs")), "the new class's field exists on the instance");
            check_eq((script_int)1, eng->execute("raw.fangs").as_int(), "the new class's own fields take their initializers");
            check_eq(std::string("slotspawn"), eng->execute("raw.speak()").as<std::string>(), "the new class dispatches");
            check_eq((script_int)1, eng->execute("same.fangs").as_int(), "a wrapper made before the extend resolves to the layer");
            check_eq(std::string("slotspawn"), eng->execute("same.speak()").as<std::string>(), "and dispatches the layer's overrides");
            check_eq((script_int)100, eng->execute("same.health").as_int(), "and still reaches the object");
            eng->execute("same.chomp = 12;");
            check_eq((script_int)12, eng->execute("raw.chomp").as_int(), "writes through the stale wrapper land on the layer");
        });

        // The handle you passed in IS the layer afterwards: no rebinding.
        test("extend_upgrades_the_handle_in_place", [&]() {
            auto eng = hybrid_engine();
            auto slot = std::make_shared<SlotCreature>();
            eng->add_global("kin", eng->make_object(slot));
            eng->execute("extend(kin, Slotkin); kin.chomp = 17;");
            eng->add_global("again", eng->make_object(slot));
            check_eq((script_int)17, eng->execute("again.chomp").as_int(), "the object's layer is the handle that was extended");
            check_eq(std::string("slotkin"), eng->execute("kin.speak()").as<std::string>(), "overrides dispatch on the upgraded handle");
            auto plain = std::make_shared<HybridCreature>();
            eng->add_global("pk", eng->make_object(plain));
            eng->execute("extend(pk, Voidkin); pk.chomp = 21;");
            eng->add_global("pk2", eng->make_object(plain));
            check_eq((script_int)21, eng->execute("pk2.chomp").as_int(), "the map path upgrades in place too");
        });

        // A pure script instance re-derives in place too: the instance keeps its fields and
        // gains the derived class's.
        test("extend_rederives_pure_script_instances", [&]() {
            auto eng = hybrid_engine();
            eng->execute(R"(
                class Pt { int a = 1; }
                class Pt3 : Pt { int b = 2; Pt3() : super() {} }
                var p = Pt();
                p.a = 5;
            )");
            check_eq((script_int)5, eng->execute("extend(p, Pt).a").as_int(), "same class returns the instance");
            eng->execute("extend(p, Pt3);");
            check_eq((script_int)5, eng->execute("p.a").as_int(), "base field kept");
            check_eq((script_int)2, eng->execute("p.b").as_int(), "derived field initialized on the same instance");
            eng->execute("extend(p, Pt3).a = 6;");
            check_eq((script_int)6, eng->execute("p.a").as_int(), "chained access reaches the instance");
            check_throws([&]() { eng->execute("extend(p, Pt);"); }, "a class that does not derive the instance's class is refused");
        });

        // extend and add_global agree in every order: extend before or after the object is bound, from C++ or
        // script, and every entry (add_global, make_object, add_global_ref, a container element, a callback argument) hands
        // script the one object, re-classed in place
        test("extend_and_add_global_agree_in_every_order", [&]() {
            order_matrix<OrderPoly>("PolyKin");
            order_matrix<OrderSlot>("SlotKin");
            order_matrix<OrderPlain>("PlainKin");
        });

        // An object extended through its base before its own C++ type is bound keeps its layer once the binder registers
        // The one-identity record is by address: a member at offset 0, handed out through an aliasing handle that shares the
        // whole's owner, is its own object of its own class (it was handed back as the whole, or as the whole's layer)
        test("a_member_at_the_wholes_address_is_its_own_object", [&]() {
            auto eng = order_engine();
            dynamic_binder<NestedPart>(*eng, "NestedPart").constructor<>().property("depth", &NestedPart::depth).build();
            dynamic_binder<NestedWhole>(*eng, "NestedWhole").constructor<>().property("width", &NestedWhole::width).build();
            dynamic_binder<NestedPolyPart>(*eng, "NestedPolyPart").constructor<>().property("depth", &NestedPolyPart::depth).build();
            dynamic_binder<NestedPolyWhole>(*eng, "NestedPolyWhole").constructor<>().property("width", &NestedPolyWhole::width).build();
            eng->execute("class WholeKin : NestedWhole { int chomp = 3; WholeKin() : super() {} }");

            auto whole = std::make_shared<NestedWhole>();
            eng->add_global("whole", whole);
            eng->add_global("part", std::shared_ptr<NestedPart>(whole, &whole->part));
            check_eq(std::string("NestedPart"), eng->execute("type_of(part)").as<std::string>());
            check_eq((script_int)7, eng->execute("part.depth").as_int());
            eng->add_global("whole_again", whole);
            check_eq(std::string("NestedWhole"), eng->execute("type_of(whole_again)").as<std::string>());
            check_true(eng->execute("same_object(whole, whole_again)").as<bool>(), "the whole keeps its one identity");

            auto poly = std::make_shared<NestedPolyWhole>();
            eng->add_global("poly", poly);
            eng->add_global("poly_part", std::shared_ptr<NestedPolyPart>(poly, &poly->part));
            check_eq(std::string("NestedPolyPart"), eng->execute("type_of(poly_part)").as<std::string>());
            check_eq((script_int)8, eng->execute("poly_part.depth").as_int());

            auto extended = std::make_shared<NestedWhole>();
            (void)eng->extend(extended, "WholeKin");
            eng->add_global("kin", extended);
            eng->add_global("kin_part", std::shared_ptr<NestedPart>(extended, &extended->part));
            check_eq(std::string("NestedPart"), eng->execute("type_of(kin_part)").as<std::string>(), "the part is not the whole's layer");
            eng->add_global("kin_again", extended);
            check_eq(std::string("WholeKin"), eng->execute("type_of(kin_again)").as<std::string>(), "the whole keeps its layer");
            check_eq((script_int)3, eng->execute("kin_again.chomp").as_int());
        });

        test("extend_before_the_derived_binder_registers", [&]() {
            auto eng = order_engine();
            auto elite = std::make_shared<OrderElite>();
            std::shared_ptr<OrderPoly> as_base = elite;
            (void)eng->extend(as_base, "PolyKin");
            dynamic_binder<OrderElite>(*eng, "OrderElite").constructor<>().base_class<OrderPoly>().property("rank", &OrderElite::rank).build();
            eng->add_global("hero", elite);
            check_eq(std::string("PolyKin"), eng->execute("type_of(hero)").as<std::string>(), "the global is the layer");
            check_eq((script_int)3, eng->execute("hero.chomp").as_int(), "the layer's script field");
            eng->add_global("via_base", as_base);
            check_true(eng->execute("same_object(hero, via_base)").as<bool>(), "the base pointer reaches the same layer");
        });

        // An extended object owns its layer: when the object dies, a layer a script still holds
        // is retired (C++ access reports, fields stay). A slotless object's layer is engine-owned
        // and keeps the object alive until script_layer_detach.
        test("extended_object_owns_its_layer", [&]() {
            auto eng = hybrid_engine();
            auto slot = std::make_shared<SlotCreature>();
            eng->add_global("dying", eng->extend(slot, "Slotkin"));
            eng->execute("dying.chomp = 4;");
            slot.reset();
            check_eq((script_int)4, eng->execute("dying.chomp").as_int(), "script fields survive the object");
            check_throws([&]() { eng->execute("dying.health"); }, "C++ access through a dead object's layer reports");

            auto plain = std::make_shared<HybridCreature>();
            HybridCreature* raw = plain.get();
            eng->add_global("kept", eng->extend(plain, "Voidkin"));
            plain.reset();
            check_eq((script_int)100, eng->execute("kept.health").as_int(), "the engine-owned layer keeps a slotless object alive");
            std::shared_ptr<HybridCreature> alias(std::shared_ptr<HybridCreature>(), raw);
            check(!eng->script_layer_detach(alias).empty(), "detach releases it");
            check_throws([&]() { eng->execute("kept.health"); }, "the detached layer is retired");
        });

        // Ownership follows creation: an object the script constructed and then extends stays the script's, so its layer keeps it
        // alive (the constructed-hybrid direction) instead of the object owning a layer nobody else keeps the object for
        test("extending_a_script_constructed_object_keeps_it_alive", [&]() {
            auto eng = hybrid_engine();
            eng->execute(R"(
                shared_ptr<SlotCreature> made = new SlotCreature();
                made.health = 44;
                extend(made, Slotkin);
                made.chomp = 9;
                var local = SlotCreature();
                local.health = 45;
                extend(local, Slotkin);
            )");
            check_eq((script_int)44, eng->execute("made.health").as_int(), "a new-made object survives its extend");
            check_eq(std::string("slotkin"), eng->execute("made.speak()").as<std::string>(), "and speaks as its layer");
            check_eq((script_int)9, eng->execute("made.chomp").as_int(), "its layer's fields stay");
            check_eq((script_int)45, eng->execute("local.health").as_int(), "a constructed value survives its extend");
            check_eq(std::string("slotkin"), eng->execute("local.speak()").as<std::string>(), "and speaks as its layer");
            auto held = std::make_shared<SlotCreature>();
            eng->add_global("shared_held", eng->make_object(held));
            eng->execute("extend(shared_held, Slotkin);");
            check(eng->script_layer(*held) != nullptr, "an object the host also owns still owns its layer");
            held.reset();
            check_throws([&]() { eng->execute("shared_held.health"); }, "and its layer retires when the host drops it");
        });

        test("a_retired_layer_reports_the_catalogued_text_once", [&]() {
            auto eng = hybrid_engine();
            auto slot = std::make_shared<SlotCreature>();
            eng->add_global("dying", eng->extend(slot, "Slotkin"));
            eng->execute("function bare_read() -> int { return dying.health; }");
            slot.reset();
            const std::string expected = "Script layer 'Slotkin' was detached (script_layer_detach, or a move to another engine): this handle no longer reaches its object";
            for (const char* source : {"dying.health", "dying.health = 3;", "bare_read()", "var h = dying.health;"}) {
                std::string message;
                try { eng->execute(source); } catch (const std::exception& e) { message = e.what(); }
                check_eq(expected, message, std::string("the text for ") + source);
            }
            check_eq(std::string("slotkin"), eng->execute("dying.speak()").as<std::string>(), "a script override still answers on the retired layer");
        });

        // S6/D7: the layer moves between engines through the name-keyed representation. Slot
        // types move on extend; map types move through the explicit detach + extend pair.
        test("script_layer_moves_between_engines", [&]() {
            auto a = hybrid_engine();
            auto b = hybrid_engine();

            auto slot = std::make_shared<SlotCreature>();
            slot->health = 33;
            (void)a->extend(slot, "Slotkin");
            a->add_global("ka", a->make_object(slot));
            a->execute("ka.chomp = 21;");

            (void)b->extend(slot, "Slotkin");
            check(b->script_layer(*slot) != nullptr, "b owns the layer now");
            check(a->script_layer(*slot) == nullptr, "a no longer owns a layer for it");
            b->add_global("kb", b->make_object(slot));
            check_eq((script_int)21, b->execute("kb.chomp").as_int(), "script state moved by name");
            check_eq((script_int)33, b->execute("kb.health").as_int(), "the C++ object is the same one");
            b->execute("kb.chomp = 22;");
            check_eq((script_int)21, a->execute("ka.chomp").as_int(), "a's retired layer keeps its own fields");
            check_throws([&]() { a->execute("ka.health"); }, "a's retired layer no longer reaches the object");
            check_eq(33, slot->health, "the object itself is untouched");

            auto plain = std::make_shared<HybridCreature>();
            (void)a->extend(plain, "Voidkin");
            a->add_global("pa", a->make_object(plain));
            a->execute("pa.chomp = 31;");
            std::string carried = a->script_layer_detach(plain);
            check(carried.find("31") != std::string::npos, "detach returns the fields: " + carried);
            check(a->script_layer(*plain) == nullptr, "detach cleared a's map entry");
            (void)b->extend(plain, "Voidkin", carried);
            b->add_global("pb", b->make_object(plain));
            check_eq((script_int)31, b->execute("pb.chomp").as_int(), "explicit two-step migration carries fields");
            check_eq(std::string(""), a->script_layer_detach(plain), "detaching an unlayered object is a no-op");
        });

        // The mixin's own state door: to_json gives {"$type","$script"}, from_json hydrates the
        // live layer, or re-derives first when $type names another class.
        test("a_constructorless_script_class_constructs_its_cpp_base", [&]() {
            // C++: the implicit default constructor runs the base's default constructor first
            auto eng = hybrid_engine();
            eng->execute(R"(
                class Plainkin : SlotCreature { int chomp = 3; }
                var p = Plainkin();
                p.health = 7;
                p.chomp = 17;
                class Deepkin : Plainkin { int fangs = 2; }
                var d = Deepkin();
                d.health = 9;
            )");
            check_eq((script_int)7, eng->execute("p.health").as_int(), "the inherited C++ property stores and reads");
            check_eq((script_int)17, eng->execute("p.chomp").as_int());
            auto object = eng->execute("p").as<std::shared_ptr<SlotCreature>>();
            check(object != nullptr, "the instance extracts to its C++ base");
            check_eq(7, object->health, "the store reached the C++ object");
            check_eq((script_int)9, eng->execute("d.health").as_int(), "a two-level constructor-less chain too");
            check_eq((script_int)2, eng->execute("d.fangs").as_int());
        });

        test("a_constructor_without_super_constructs_its_cpp_base", [&]() {
            auto eng = hybrid_engine();
            dynamic_binder<ArgOnlyCreature>(*eng, "ArgOnlyCreature")
                .constructor<int>()
                .property("health", &ArgOnlyCreature::health)
                .build();
            auto r = eng->execute(R"(
                [[warn_mode]]
                function t(function f) -> string { string r = "none"; try { f(); } catch (e) { r = e; } return r; }
                class Bodykin : SlotCreature { int chomp = 3; int seen = this.health; Bodykin() { chomp = health + 1; } }
                var b = Bodykin();
                b.health = 7;
                class Argkin : ArgOnlyCreature { Argkin() {} }
                class Bareargkin : ArgOnlyCreature { }
                class Fineargkin : ArgOnlyCreature { Fineargkin() : super(9) {} }
                return [b.chomp, b.seen, b.health, t([]() { var a = Argkin(); }), t([]() { var a = Bareargkin(); }), Fineargkin().health];
            )");
            const auto& v = r.as_array();
            check_eq((script_int)101, v[0].as_int(), "the C++ base was constructed before the body");
            check_eq((script_int)100, v[1].as_int(), "and before the derived class's field initializers");
            check_eq((script_int)7, v[2].as_int(), "the inherited property stores");
            const std::string text = "Base 'ArgOnlyCreature' has no default constructor; call super(...)";
            check_eq(text, v[3].as<std::string>(), "a host base without a default constructor");
            check_eq(text, v[4].as<std::string>(), "the same for a constructor-less class");
            check_eq((script_int)9, v[5].as_int(), "an explicit super(...) still passes its arguments");
        });

        // A load of a script class over a C++ base builds the base the way the archive layer builds a host type
        // (load_and_construct, else its default constructor whatever its access) and runs no script constructor; a base
        // with neither is refused by name. Every load leaves a whole object: the C++ base and the layer's fields.
        test("a_hybrid_load_builds_its_cpp_base_through_load_and_construct_or_its_default_constructor", [&]() {
            auto eng = hybrid_engine();
            dynamic_binder<LacCreature>(*eng, "LacCreature")
                .property("health", &LacCreature::health)
                .property("label", &LacCreature::label)
                .build();
            dynamic_binder<SealedCreature>(*eng, "SealedCreature")
                .property("health", &SealedCreature::health)
                .property("label", &SealedCreature::label)
                .build();
            eng->execute(R"(
                class Lackin : LacCreature { int chomp = 3; }
                class Sealedkin : SealedCreature { int chomp = 3; }
                class Argful : SlotCreature { int chomp = 3; Argful(int c) : super() { chomp = c; } }
                var a = Argful(7);
                a.health = 42;
                a.label = "gnasher";
            )");
            auto lac = LacCreature::make(42);
            lac->label = "gnasher";
            eng->add_global("l", eng->extend(eng->make_object(lac), script_value(std::string("Lackin"), eng.get())));
            auto sealed = SealedCreature::make();
            sealed->health = 42;
            sealed->label = "gnasher";
            eng->add_global("s2", eng->extend(eng->make_object(sealed), script_value(std::string("Sealedkin"), eng.get())));
            eng->execute("l.chomp = 7; s2.chomp = 7;");

            auto script_round_trips = [&](const char* var, const char* label) {
                for (const char* format : { "from_json(to_json(", "from_binary(to_binary(" }) {
                    const std::string spelled = std::string(format) + var + "))";
                    const std::string where = std::string(label) + " through " + format;
                    auto loaded = eng->execute(spelled);
                    eng->add_global("loaded", loaded);
                    check_eq((script_int)42, eng->execute("loaded.health").as_int(), where + ": the C++ base's property");
                    check_eq(std::string("gnasher"), eng->execute("loaded.label").as<std::string>(), where);
                    check_eq((script_int)7, eng->execute("loaded.chomp").as_int(), where + ": the layer's field");
                }
            };
            script_round_trips("l", "load_and_construct");
            script_round_trips("s2", "a private default constructor");
            script_round_trips("a", "a script class whose constructors take arguments");
            auto lacLoaded = eng->execute("from_json(to_json(l))").as<std::shared_ptr<LacCreature>>();
            check(lacLoaded != nullptr && lacLoaded.get() != lac.get(), "a fresh C++ object, never a null one");
            auto sealedLoaded = eng->execute("from_binary(to_binary(s2))").as<std::shared_ptr<SealedCreature>>();
            check(sealedLoaded != nullptr && sealedLoaded.get() != sealed.get());

            auto archive_round_trips_of = [&](auto original, const char* label) {
                using Base = typename decltype(original)::element_type;
                jai::serialization::json_archive_writer w(0, eng.get());
                w(original);
                jai::serialization::json_archive_reader r(w.str(), eng.get());
                std::shared_ptr<Base> fromJson;
                r(fromJson);
                auto fromBinary = roundtrip_binary(*eng, original);
                for (const auto& loaded : { fromJson, fromBinary }) {
                    check(loaded != nullptr && loaded.get() != original.get(), std::string(label) + ": a fresh C++ object");
                    check_eq(42, loaded->health, label);
                    check_eq(std::string("gnasher"), loaded->label, label);
                    check(eng->script_layer(*loaded) != nullptr, std::string(label) + ": the layer is attached");
                    eng->add_global("held", eng->make_object(loaded));
                    check_eq((script_int)7, eng->execute("held.chomp").as_int(), label);
                }
            };
            archive_round_trips_of(lac, "load_and_construct, C++ archive");
            archive_round_trips_of(sealed, "private default constructor, C++ archive");
        });

        // A load constructs, then loads the saved values over the result, then runs post_load (C++ semantics, as Cereal):
        // a class with a zero-argument constructor runs it (for a hybrid, its initializer list builds the C++ base), and
        // every saved value wins over what the constructor set, the C++ base's properties included; a class with only
        // argument-taking constructors loads without running one
        test("a_load_runs_the_zero_argument_constructor_then_loads_every_saved_value_over_it", [&]() {
            auto eng = hybrid_engine();
            eng->execute(R"(
                int built = 0;
                int loaded_hooks = 0;
                class Ctorkin : SlotCreature {
                    int chomp = 3;
                    Ctorkin() : super() { built += 1; chomp = 50; health = 77; label = "fresh"; }
                    void post_load(int version) { loaded_hooks += 1; }
                }
                class Plain {
                    int a = 1;
                    string s = "x";
                    Plain() { built += 10; a = 99; s = "ctor"; }
                    void post_load(int version) { loaded_hooks += 1; }
                }
                class Argkin : SlotCreature {
                    int chomp = 3;
                    Argkin(int c) : super() { built += 100; chomp = c; }
                }
                var c = Ctorkin();
                c.chomp = 7;
                c.health = 42;
                c.label = "gnasher";
                var p = Plain();
                p.a = 5;
                p.s = "saved";
                var g = Argkin(1);
                g.chomp = 8;
                g.health = 43;
                built = 0;
            )");
            for (const char* format : { "from_json(to_json(", "from_binary(to_binary(" }) {
                const std::string f(format);
                eng->execute("built = 0; loaded_hooks = 0;");
                eng->add_global("lc", eng->execute(f + "c))"));
                check_eq((script_int)1, eng->execute("built").as_int(), f + ": the hybrid's zero-argument constructor ran");
                check_eq((script_int)7, eng->execute("lc.chomp").as_int(), f + ": the saved script field wins");
                check_eq((script_int)42, eng->execute("lc.health").as_int(), f + ": the saved base property wins");
                check_eq(std::string("gnasher"), eng->execute("lc.label").as<std::string>(), f);
                check(eng->execute("lc").as<std::shared_ptr<SlotCreature>>() != nullptr, f + ": the C++ base its constructor built");
                eng->add_global("lp", eng->execute(f + "p))"));
                check_eq((script_int)11, eng->execute("built").as_int(), f + ": the pure class's constructor ran");
                check_eq((script_int)5, eng->execute("lp.a").as_int(), f);
                check_eq(std::string("saved"), eng->execute("lp.s").as<std::string>(), f);
                check_eq((script_int)2, eng->execute("loaded_hooks").as_int(), f + ": post_load once per object");
                eng->add_global("lg", eng->execute(f + "g))"));
                check_eq((script_int)11, eng->execute("built").as_int(), f + ": an argument-only class loads without a constructor");
                check_eq((script_int)8, eng->execute("lg.chomp").as_int(), f);
                check_eq((script_int)43, eng->execute("lg.health").as_int(), f);
            }

            auto original = eng->execute("c").as<std::shared_ptr<SlotCreature>>();
            jai::serialization::json_archive_writer w(0, eng.get());
            w(original);
            const std::string json = w.str();
            for (int round = 0; round < 2; ++round) {
                eng->execute("built = 0; loaded_hooks = 0;");
                std::shared_ptr<SlotCreature> loaded;
                if (round == 0) {
                    jai::serialization::json_archive_reader r(json, eng.get());
                    r(loaded);
                } else {
                    loaded = roundtrip_binary(*eng, original);
                }
                const std::string where = round == 0 ? "C++ JSON archive" : "C++ binary archive";
                check(loaded != nullptr && loaded.get() != original.get(), where + ": a fresh object");
                check_eq((script_int)1, eng->execute("built").as_int(), where + ": the zero-argument constructor ran");
                check_eq((script_int)1, eng->execute("loaded_hooks").as_int(), where + ": post_load once");
                check_eq(42, loaded->health, where + ": the saved base property wins over the constructor's");
                check_eq(std::string("gnasher"), loaded->label, where);
                eng->add_global("held", eng->make_object(loaded));
                check_eq((script_int)7, eng->execute("held.chomp").as_int(), where + ": the saved script field wins");
            }
        });

        // A load writes state and notifies nothing: an observer the zero-argument constructor connected stays quiet
        // through the load on every path (it fires afterwards), so a field it would clobber keeps its saved value; a
        // saved value that no longer fits keeps the constructed value and is recorded as a retype
        test("a_load_notifies_no_observer_and_records_a_value_that_no_longer_fits", [&]() {
            auto eng = hybrid_engine();
            dynamic_binder<WatchCreature>(*eng, "WatchCreature").auto_bind().constructor<>().build();
            eng->execute(R"(
                int fired = 0;
                class Watchkin : WatchCreature {
                    int mood = 5;
                    Watchkin() : super() { on_health_change([this](auto o, auto n) { fired += 1; mood = -1; }); }
                }
                var w = Watchkin();
                w.health = 42;
                w.armor = 9;
                w.mood = 7;
                fired = 0;
            )");
            for (const char* format : { "from_json(to_json(", "from_binary(to_binary(" }) {
                const std::string f(format);
                eng->execute("fired = 0;");
                eng->add_global("lw", eng->execute(f + "w))"));
                check_eq((script_int)0, eng->execute("fired").as_int(), f + ": no observer fires during the load");
                check_eq((script_int)42, eng->execute("lw.health").as<script_int>(), f);
                check_eq((script_int)9, eng->execute("lw.armor").as_int(), f);
                check_eq((script_int)7, eng->execute("lw.mood").as_int(), f + ": the field an observer would clobber keeps its saved value");
                eng->execute("lw.health = 1;");
                check_eq((script_int)1, eng->execute("fired").as_int(), f + ": the observer works once the load is done");
            }

            auto original = eng->execute("w").as<std::shared_ptr<WatchCreature>>();
            jai::serialization::json_archive_writer writer(0, eng.get());
            writer(original);
            const std::string json = writer.str();
            for (int round = 0; round < 2; ++round) {
                eng->execute("fired = 0;");
                std::shared_ptr<WatchCreature> loaded;
                if (round == 0) {
                    jai::serialization::json_archive_reader reader(json, eng.get());
                    reader(loaded);
                } else {
                    loaded = roundtrip_binary(*eng, original);
                }
                const std::string where = round == 0 ? "C++ JSON archive" : "C++ binary archive";
                check(loaded != nullptr, where);
                check_eq((script_int)0, eng->execute("fired").as_int(), where + ": no observer fires during the load");
                check_eq(42, loaded->health.get(), where);
                eng->add_global("held", eng->make_object(loaded));
                check_eq((script_int)7, eng->execute("held.mood").as_int(), where);
            }

            eng->hot_reload_reporting(true);
            eng->clear_hot_reload_reports();
            eng->add_global("bad", eng->execute(R"(from_json("{\"$type\":\"Watchkin\",\"$val\":{\"health\":\"oops\",\"armor\":4,\"mood\":\"sad\"}}"))"));
            check_eq((script_int)100, eng->execute("bad.health").as<script_int>(), "a C++ property keeps its constructed value");
            check_eq((script_int)4, eng->execute("bad.armor").as_int(), "the values that fit load");
            check_eq((script_int)5, eng->execute("bad.mood").as_int(), "a script field keeps its constructed value");
            std::string rows;
            for (const auto& report : eng->hot_reload_reports()) {
                rows += report.class_name + "." + report.field_name + ":" + report.old_type + "->" +
                        (report.outcome == field_migration_outcome::reset ? "kept" : "other") + " ";
            }
            check(rows.find("Watchkin.health:string->kept") != std::string::npos, "the C++ property's retype is recorded: " + rows);
            check(rows.find("Watchkin.mood:string->kept") != std::string::npos, "the script field's retype is recorded: " + rows);
        });

        // The load contract's oracle: whatever the constructors, accessors, setters and observers do, a load restores
        // every saved value, so saving what was loaded writes the same bytes (each class shape, alone and in one archive,
        // JSON and binary, script values and the C++ archive)
        test("saving_what_a_load_restored_writes_the_same_bytes_for_every_class_shape", [&]() {
            auto eng = hybrid_engine();
            dynamic_binder<WatchCreature>(*eng, "WatchCreature").auto_bind().constructor<>().build();
            eng->execute(R"(
                int hooks = 0;
                class Pt { int x = 0; int y = 0; }
                class Every {
                    int i = 1; float f = 2.5; string s = "s"; bool b = true; char c = 'q';
                    array<int> nums = [1, 2]; map<int, string> names = {}; var anything = null; Pt at = Pt();
                }
                class ZetaFirst { int zeta = 1; int alpha = 2; }
                class Base0 { int base_a = 1; }
                class Mid : Base0 { int mid_b = 2; }
                class Leaf : Mid { int leaf_c = 3; }
                class Secret { private: int hidden = 4; public: int shown = 5; void reveal(int v) { hidden = v; } }
                class Accessed {
                    int raw = 2;
                    int _get_raw() { return raw * 10; }
                    void _set_raw(int v) { raw = v + 1; }
                    int _get_computed() { return raw + 1000; }
                }
                class Clobbers { int a = 0; int b = 0; void _set_b(int v) { b = v; a = -1; } }
                class Filled { array<int> items = []; Pt spot = Pt(); Filled() { items = [9, 9, 9]; spot.x = 77; } }
                class OnlyArgs { int v = 0; OnlyArgs(int a) { v = a; } }
                class Sealed2 { private: Sealed2() {} public: int n = 1; static Sealed2 make() { Sealed2 s = Sealed2(); s.n = 2; return s; } }
                class Graph { string name = ""; var next = null; var peer = null; }
                class Hooked { int n = 0; void post_load(int version) { hooks += 1; } }
                class Hyb : SlotCreature { int chomp = 3; Hyb() : super() { chomp = 50; health = 77; } }
                class HybArgs : SlotCreature { int chomp = 3; HybArgs(int c) : super() { chomp = c; } }
                class Watchy : WatchCreature { int mood = 5; Watchy() : super() { on_health_change([this](auto o, auto n) { mood = -1; }); } }

                var e = Every();
                e.i = 7; e.f = 0.125; e.s = "seven"; e.b = false; e.c = 'z'; e.nums = [3, 4, 5]; e.names[2] = "two";
                e.anything = [1, "x", {"k": 2}]; e.at.x = 5;
                var z = ZetaFirst(); z.zeta = 10; z.alpha = 20;
                var l = Leaf(); l.base_a = 11; l.mid_b = 22; l.leaf_c = 33;
                var sc = Secret(); sc.reveal(44); sc.shown = 55;
                var ac = Accessed(); ac.raw = 4;
                var cl = Clobbers(); cl.b = 8; cl.a = 6;
                var fi = Filled(); fi.items = [1]; fi.spot = Pt(); fi.spot.y = 3;
                var oa = OnlyArgs(12);
                var s2 = Sealed2::make();
                var g1 = new Graph(); g1.name = "a";
                var g2 = new Graph(); g2.name = "b";
                g1.next = g2; g2.next = g1; g1.peer = g2;
                var hk = Hooked(); hk.n = 3;
                var hy = Hyb(); hy.chomp = 7; hy.health = 42; hy.label = "gnasher";
                var ha = HybArgs(1); ha.chomp = 8; ha.health = 43;
                var wa = new Watchy(); wa.health = 41; wa.armor = 2; wa.mood = 6;
            )");
            eng->execute(R"(
                function same(var v) -> string {
                    string s = to_json(v);
                    string again = to_json(from_json(s));
                    string b = to_binary(v);
                    string bagain = to_binary(from_binary(b));
                    return (s == again ? "J" : "[" + s + " became " + again + "]") + (b == bagain ? "B" : "b");
                }
            )");
            for (const char* var : { "e", "z", "l", "sc", "ac", "cl", "fi", "oa", "s2", "g1", "hk", "hy", "ha", "wa" }) {
                std::string got;
                try { got = eng->execute(std::string("same(") + var + ")").as<std::string>(); } catch (const std::exception& ex) { got = ex.what(); }
                check_eq(std::string("JB"), got, std::string(var) + " saves back byte for byte");
            }
            std::string together;
            try { together = eng->execute("same([e, z, l, sc, ac, cl, fi, oa, s2, g1, hk, hy, ha, wa])").as<std::string>(); } catch (const std::exception& ex) { together = ex.what(); }
            check_eq(std::string("JB"), together, "every shape in one archive saves back byte for byte");

            for (const char* var : { "hy", "ha" }) {
                auto original = eng->execute(var).as<std::shared_ptr<SlotCreature>>();
                jai::serialization::json_archive_writer first(0, eng.get());
                first(original);
                jai::serialization::json_archive_reader reader(first.str(), eng.get());
                std::shared_ptr<SlotCreature> loaded;
                reader(loaded);
                jai::serialization::json_archive_writer second(0, eng.get());
                second(loaded);
                check_eq(first.str(), second.str(), std::string(var) + ": the C++ JSON archive saves back what it loaded");
                check(jai::to_binary(*eng, original) == jai::to_binary(*eng, roundtrip_binary(*eng, original)),
                      std::string(var) + ": and the C++ binary archive");
            }
        });

        // post_load runs once the whole archive is loaded, once per object, children before parents: inside a cycle each
        // object's hook sees the other whole, and a C++ archive's script layers wait for the outermost read to finish
        test("post_load_runs_once_the_whole_archive_is_loaded", [&]() {
            auto eng = hybrid_engine();
            auto r = eng->execute(R"(
                string pl = "";
                class Node {
                    string name = "";
                    var other = null;
                    int tail = 0;
                    void post_load(int version) { pl += `${name}:${other.name}/${other.tail} `; }
                }
                var p = new Node();
                p.name = "parent";
                p.tail = 5;
                var k = new Node();
                k.name = "child";
                k.tail = 6;
                p.other = k;
                k.other = p;
                string out = "";
                pl = "";
                var lj = from_json(to_json(p));
                out += `${pl}| `;
                pl = "";
                var lb = from_binary(to_binary(p));
                out += pl;
                return out;
            )");
            check_eq(std::string("child:parent/5 parent:child/6 | child:parent/5 parent:child/6 "), r.as<std::string>(),
                     "each hook sees the other object whole, children first");

            eng->execute(R"(
                int built = 0;
                string seen = "";
                class Countkin : SlotCreature {
                    int chomp = 3;
                    Countkin() : super() { built += 1; }
                    void post_load(int version) { seen += `${built} `; }
                }
                var c1 = Countkin();
                var c2 = Countkin();
            )");
            std::vector<std::shared_ptr<SlotCreature>> pair = { eng->execute("c1").as<std::shared_ptr<SlotCreature>>(),
                                                                eng->execute("c2").as<std::shared_ptr<SlotCreature>>() };
            const std::string json = jai::to_json(*eng, pair);
            eng->execute("built = 0; seen = \"\";");
            auto loaded = jai::from_json<std::vector<std::shared_ptr<SlotCreature>>>(*eng, json);
            check_eq((size_t)2, loaded.size());
            check_eq(std::string("2 2 "), eng->execute("seen").as<std::string>(), "both objects exist before either hook runs");
            eng->execute("built = 0; seen = \"\";");
            auto fromBinary = jai::from_binary<std::vector<std::shared_ptr<SlotCreature>>>(*eng, jai::to_binary(*eng, pair));
            check_eq(std::string("2 2 "), eng->execute("seen").as<std::string>(), "the binary archive too");
        });

        // A handle to a nocopy hybrid in an array literal stays a handle through a range-for: passing or copying the loop
        // variable shares the object (the loop stamped it var, and the copy refused the nocopy class)
        test("a_range_for_over_a_handle_to_a_nocopy_hybrid_never_copies_it", [&]() {
            auto eng = hybrid_engine();
            auto r = eng->execute(R"(
                class V { int a = 1; }
                class Lk : NoCopyCreature { int chomp = 3; Lk() : super() {} }
                class Hy : SlotCreature { int chomp = 3; Hy() : super() {} }
                var e = V();
                var hy = Hy();
                var wa = new Lk();
                function take(var v) -> int { return 1; }
                int k = 0;
                for (var v : [e, hy, wa]) { k += take(v); }
                for (var v : [wa]) { var u = v; u.chomp = 9; }
                return `${k} ${wa.chomp}`;
            )");
            check_eq(std::string("3 9"), r.as<std::string>(), "the loop variable shares the nocopy object");
        });

        test("a_hybrid_load_over_a_base_with_neither_constructor_names_what_is_missing", [&]() {
            auto eng = hybrid_engine();
            dynamic_binder<StubbornCreature>(*eng, "StubbornCreature")
                .constructor<int>()
                .property("health", &StubbornCreature::health)
                .build();
            eng->execute("class Stubbornkin : StubbornCreature { int chomp = 3; Stubbornkin(int h) : super(h) {} } var k = Stubbornkin(42);");
            const std::string text = "Cannot load hybrid script class 'Stubbornkin': its C++ base 'StubbornCreature' is not default constructible and has no load_and_construct";
            for (const char* spelled : { "from_json(to_json(k))", "from_binary(to_binary(k))" }) {
                std::string caught;
                try { eng->execute(spelled); } catch (const std::exception& e) { caught = e.what(); }
                check(caught.find(text) != std::string::npos, std::string(spelled) + " is refused by name: " + caught);
            }
            auto original = eng->execute("k").as<std::shared_ptr<StubbornCreature>>();
            jai::serialization::json_archive_writer w(0, eng.get());
            w(original);
            std::string caught;
            try {
                jai::serialization::json_archive_reader r(w.str(), eng.get());
                std::shared_ptr<StubbornCreature> loaded;
                r(loaded);
            } catch (const std::exception& e) {
                caught = e.what();
            }
            check_eq(text, caught, "the C++ archive gives the same text");
        });

        test("a_script_layer_saves_only_its_script_fields", [&]() {
            auto eng = hybrid_engine();
            auto slot = std::make_shared<SlotCreature>();
            slot->health = 55;
            eng->add_global("kin", eng->extend(slot, "Slotkin"));
            eng->execute("kin.chomp = 17;");
            std::string state = slot->script_layer_to_json();
            check(state.find("\"chomp\":17") != std::string::npos, "the script field: " + state);
            check(state.find("health") == std::string::npos, "an inherited C++ property is the C++ object's, not the layer's: " + state);
            jai::serialization::json_archive_writer w(0, eng.get());
            w(std::shared_ptr<SlotCreature>(slot));
            std::string json = w.str();
            check(json.find("null") == std::string::npos, "no null inherited property in the archive: " + json);
            check(json.find("55") != std::string::npos, "the C++ property saves through the C++ object: " + json);
        });

        // A layer field holding a shared handle saved as null; it saves the object once and two fields sharing it share it again
        test("a_script_layers_shared_handles_round_trip", [&]() {
            auto eng = hybrid_engine();
            eng->execute(R"(
                class Pal { int mood = 1; }
                class Pairkin : Slotkin { var best; var also; Pairkin() : super() {} }
            )");
            auto slot = std::make_shared<SlotCreature>();
            eng->add_global("kin", eng->extend(slot, "Pairkin"));
            eng->execute("var pal = new Pal(); pal.mood = 4; kin.best = pal; kin.also = pal;");
            std::string state = slot->script_layer_to_json();
            auto twin = std::make_shared<SlotCreature>();
            eng->add_global("twin", eng->extend(twin, "Pairkin"));
            check(twin->script_layer_from_json(state), "hydrates: " + state);
            check_eq((script_int)4, eng->execute("twin.best.mood").as_int(), state);
            check_true(eng->execute("same_object(twin.best, twin.also)").as<bool>(), "the two fields share one object again");
        });

        test("script_extensible_json_state_round_trip", [&]() {
            auto eng = hybrid_engine();
            eng->execute(R"(
                class Slotspawn : Slotkin {
                    int fangs = 1;
                    Slotspawn() : super() {}
                }
            )");
            auto slot = std::make_shared<SlotCreature>();
            check_eq(std::string(""), slot->script_layer_to_json(), "no layer, no state");
            check(!slot->script_layer_from_json("{}"), "no layer to hydrate");
            eng->add_global("kin", eng->extend(slot, "Slotkin"));
            eng->execute("kin.chomp = 17;");
            std::string state = slot->script_layer_to_json();
            check(state.find("\"$type\"") != std::string::npos && state.find("Slotkin") != std::string::npos &&
                  state.find("17") != std::string::npos, "state carries the class and the fields: " + state);

            auto twin = std::make_shared<SlotCreature>();
            eng->add_global("twin", eng->extend(twin, "Slotkin"));
            check(twin->script_layer_from_json(state), "same class hydrates in place");
            check_eq((script_int)17, eng->execute("twin.chomp").as_int(), "fields hydrated by name");

            eng->execute("kin.chomp = 18;");
            std::string spawnState = std::string("{\"$type\":\"Slotspawn\",\"$script\":{\"chomp\":18,\"fangs\":7}}");
            check(slot->script_layer_from_json(spawnState), "another $type re-derives");
            check_eq(std::string("Slotspawn"), eng->script_layer(*slot)->get_class_definition()->get_name());
            eng->add_global("spawn", eng->make_object(slot));
            check_eq((script_int)7, eng->execute("spawn.fangs").as_int(), "the re-derived layer took the new class's fields");
            check_eq((script_int)18, eng->execute("spawn.chomp").as_int());
        });

        test("a_script_layer_state_keeps_chars_and_handles", [&]() {
            auto eng = hybrid_engine();
            stdlib::register_all(*eng);
            eng->execute(R"(
                class Pal { int n = 1; }
                class Markkin : SlotCreature {
                    char mark = 'a';
                    shared_ptr<Pal> pal;
                    weak_ptr<Pal> seen;
                }
            )");
            auto slot = std::make_shared<SlotCreature>();
            eng->add_global("kin", eng->extend(slot, "Markkin"));
            eng->execute("kin.mark = 'q'; kin.pal = new Pal(); kin.pal.n = 6; kin.seen = kin.pal;");
            std::string state = slot->script_layer_to_json();
            auto twin = std::make_shared<SlotCreature>();
            eng->add_global("twin", eng->extend(twin, "Markkin"));
            check(twin->script_layer_from_json(state), "the state hydrates: " + state);
            check_eq(std::string("char q 6 true"), eng->execute(
                "`${type_of(twin.mark)} ${twin.mark} ${to_string(twin.pal.n)} ${to_string(same_object(twin.seen.lock(), twin.pal))}`").as<std::string>(),
                "a char field stays a char, a handle field its object, a weak_ptr to it the same object: " + state);
        });

        test("property_owner_carries_the_slot", [&]() {
            auto eng = hybrid_engine();
            POMultiDerived multi;
            check(!multi.has_script_layer(), "fresh object: no layer");
            auto probe = eng->execute("Slotkin()");
            auto layer = probe.as<strong_ptr<class_instance>>();
            multi.jai_script_slot().attach_script_layer(layer);
            check(multi.script_layer() == layer, "the multi-base slot resolves through the first extensible base");
            check(eng->script_layer(multi) == layer, "engine::script_layer reads the same slot");
            multi.jai_script_slot().detach_script_layer();
            check(!multi.has_script_layer());
        });

        test("binary_writer_unresolvable_type_still_throws", [&]() {
            // The widened door admits types the WRITER's engine can resolve (metadata or a
            // class definition); a foreign engine's writer resolves neither, so the
            // throw-on-unregistered guarantee holds instead of emitting an unloadable payload.
            auto eng = hybrid_engine();
            auto other = make_engine();
            script_value v = eng->execute("v");
            check_throws([&]() {
                jai::serialization::binary_archive_writer writer(other.get());
                writer.write_value(v);
            }, "a type the writer's engine cannot resolve still throws");
        });
    }
};

// ============================================================================
// Native object access: everything a host object registers reaches script through every spelling, on both
// backends, and equality, copies and archives keep their results (golden output of the pre-change runtime)
// ============================================================================

struct NativeGadget : jai::property_owner<NativeGadget> {
    JAI_PROPERTY((int), charge, 5);
    JAI_SIGNAL_PROPERTY(void(int), on_spark);
    void spark(int amount) { charge = charge.get() + amount; on_sparkSignal(amount); }
    void note(const std::string& id, const std::string& script) { on_sparkSignal.connect(id, script); }
};

class native_object_access_tests : public suite {
public:
    native_object_access_tests() : suite("Native Object Access") {}

    static std::string hex(const std::vector<uint8_t>& bytes) {
        constexpr const char digits[] = "0123456789abcdef";
        std::string out;
        for (uint8_t b : bytes) { out += digits[b >> 4]; out += digits[b & 15]; }
        return out;
    }
    static std::vector<uint8_t> unhex(const std::string& text) {
        std::vector<uint8_t> out;
        auto nibble = [](char c) { return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10); };
        for (size_t i = 0; i + 1 < text.size(); i += 2) { out.push_back(static_cast<uint8_t>((nibble(text[i]) << 4) | nibble(text[i + 1]))); }
        return out;
    }

    std::shared_ptr<engine> native_engine() {
        auto eng = make_engine();
        stdlib::register_all(*eng);
        dynamic_binder<HybridCreature>(*eng, "HybridCreature")
            .constructor<>()
            .property("health", &HybridCreature::health)
            .property("label", &HybridCreature::label)
            .method("speak", &HybridCreature::speak)
            .build();
        dynamic_binder<NativeGadget>(*eng, "NativeGadget")
            .constructor<>()
            .method("spark", &NativeGadget::spark)
            .auto_bind()
            .build();
        eng->execute(R"(
            class Voidkin : HybridCreature {
                int chomp = 3;
                Voidkin() : super() {}
                string speak() override { return "voidkin"; }
            }
        )");
        return eng;
    }

    // Each spelling's result as text, one line per spelling
    static constexpr const char* k_report = R"(
        var sparks = 0;
        function report() -> string {
            string r = "";
            var h = HybridCreature();
            h.health = 7;
            r += "h.health " + to_string(h.health) + "\n";
            r += "get " + to_string(reflect::get(h, "health")) + "\n";
            reflect::set(h, "label", "set");
            r += "set " + h.label + "\n";
            h.label += "!";
            r += "compound " + h.label + "\n";
            r += "has_field " + to_string(reflect::has_field(h, "health")) + " " + to_string(reflect::has_field(h, "label")) + "\n";
            r += "fields " + to_json(reflect::fields(h)) + "\n";
            r += "methods " + to_json(reflect::methods(h)) + "\n";
            r += "speak " + h.speak() + "\n";
            auto bound = reflect::bind(h, "speak");
            r += "bind " + bound() + "\n";
            r += "invoke " + to_string(reflect::invoke(h, "speak")) + "\n";
            r += "type " + reflect::type_name(h) + " " + to_string(reflect::is_cpp_bound(h)) + "\n";
            var h2 = HybridCreature();
            r += "eq " + to_string(h == h) + " " + to_string(h == h2) + "\n";
            var hc = h;
            r += "copy " + to_string(hc == h) + " " + to_string(same_object(hc, h)) + " " + to_string(hc.health) + " " + hc.label + "\n";
            hc.health = 1;
            r += "copy independent " + to_string(h.health) + "\n";
            r += "json " + to_json(h) + "\n";
            var hj = from_json(to_json(h));
            r += "from_json " + to_string(hj.health) + " " + hj.label + "\n";
            var hb = from_binary(to_binary(h));
            r += "from_binary " + to_string(hb.health) + " " + hb.label + "\n";

            var v = Voidkin();
            v.health = 42;
            v.chomp = 8;
            r += "v.health " + to_string(v.health) + " " + to_string(v.chomp) + " " + v.speak() + "\n";
            r += "v.get " + to_string(reflect::get(v, "health")) + " " + to_string(reflect::get(v, "chomp")) + "\n";
            reflect::set(v, "health", 43);
            r += "v.set " + to_string(v.health) + "\n";
            r += "v.fields " + to_json(reflect::fields(v)) + "\n";
            r += "v.methods " + to_json(reflect::methods(v)) + "\n";
            auto vb = reflect::bind(v, "speak");
            r += "v.bind " + vb() + " " + to_string(reflect::invoke(v, "speak")) + "\n";
            r += "v.type " + reflect::type_name(v) + " " + to_string(reflect::is_cpp_bound(v)) + "\n";
            var v2 = Voidkin();
            r += "v.eq " + to_string(v == v) + " " + to_string(v == v2) + "\n";
            var vc = v;
            r += "v.copy " + to_string(vc == v) + " " + to_string(same_object(vc, v)) + " " + to_string(vc.health) + " " + to_string(vc.chomp) + "\n";
            r += "v.json " + to_json(v) + "\n";
            var vj = from_json(to_json(v));
            r += "v.from_json " + to_string(vj.health) + " " + to_string(vj.chomp) + " " + vj.speak() + "\n";
            var vbin = from_binary(to_binary(v));
            r += "v.from_binary " + to_string(vbin.health) + " " + to_string(vbin.chomp) + " " + vbin.speak() + "\n";

            var x = HybridCreature();
            x.health = 9;
            extend(x, Voidkin);
            r += "extend " + to_string(x.health) + " " + to_string(x.chomp) + " " + x.speak() + " " + reflect::type_name(x) + "\n";
            r += "extend.get " + to_string(reflect::get(x, "health")) + "\n";
            r += "extend.json " + to_json(x) + "\n";

            var g = NativeGadget();
            r += "g.charge " + to_string(g.charge) + "\n";
            g.charge = 9;
            r += "g.set " + to_string(g.charge) + " " + to_string(reflect::get(g, "charge")) + "\n";
            reflect::set(g, "charge", 11);
            g.on_spark.connect("count", [](int n) -> void { sparks = sparks + n; });
            g.spark(3);
            r += "g.spark " + to_string(g.charge) + " " + to_string(sparks) + "\n";
            r += "g.fields " + to_json(reflect::fields(g)) + "\n";
            r += "g.methods " + to_json(reflect::methods(g)) + "\n";
            r += "g.json " + to_json(g) + "\n";
            var g2 = NativeGadget();
            r += "g.eq " + to_string(g == g) + " " + to_string(g == g2) + "\n";
            return r;
        }
    )";

    // A hot reload of the script subclass keeps each live hybrid's C++ object; identity, copies and equality after it
    static constexpr const char* k_reload_before = R"(
        var r1 = Voidkin();
        r1.health = 61;
        r1.chomp = 4;
        var keep = r1;
        var& alias = r1;
        auto same_before = same_object(keep, r1);
        auto alias_before = same_object(alias, r1);
    )";
    static constexpr const char* k_reload_class = R"(
        class Voidkin : HybridCreature {
            int chomp = 3;
            int fangs = 2;
            Voidkin() : super() {}
            string speak() override { return "reloaded"; }
        }
    )";
    static constexpr const char* k_reload_after = R"(
        function after_reload() -> string {
            string r = "";
            r += "fields " + to_string(r1.health) + " " + to_string(r1.chomp) + " " + to_string(r1.fangs) + " " + r1.speak() + "\n";
            r += "same " + to_string(same_before) + " " + to_string(same_object(keep, r1)) + "\n";
            r += "alias " + to_string(alias_before) + " " + to_string(same_object(alias, r1)) + " " + to_string(alias.health) + " " + alias.speak() + "\n";
            var c = r1;
            c.health = 5;
            r += "copy " + to_string(same_object(c, r1)) + " " + to_string(c == r1) + " " + to_string(r1.health) + " " + to_string(c.health) + " " + c.speak() + "\n";
            r += "eq " + to_string(r1 == r1) + " " + to_string(r1 == keep) + "\n";
            var fresh = Voidkin();
            r += "fresh " + to_string(fresh.health) + " " + to_string(fresh.fangs) + " " + to_string(fresh == r1) + "\n";
            r += "json " + to_json(r1) + "\n";
            var back = from_binary(to_binary(r1));
            r += "binary " + to_string(back.health) + " " + to_string(back.chomp) + " " + to_string(back.fangs) + " " + back.speak() + "\n";
            return r;
        }
    )";

    static constexpr const char* k_reload_golden = "fields 61 4 2 reloaded\nsame false false\nalias true true 61 reloaded\ncopy false false 61 5 reloaded\neq true false\nfresh 100 2 false\njson {\"$type\":\"Voidkin\",\"$val\":{\"health\":61,\"label\":\"creature\",\"chomp\":4,\"fangs\":2}}\nbinary 61 4 2 reloaded\n";
    static constexpr const char* k_report_golden = "h.health 7\nget 7\nset set\ncompound set!\nhas_field true true\nfields []\nmethods [{\"access\":\"public\",\"arity\":0,\"from\":\"HybridCreature\",\"name\":\"speak\",\"params\":[],\"static\":false}]\nspeak grr\nbind grr\ninvoke grr\ntype HybridCreature true\neq true false\ncopy false false 7 set!\ncopy independent 7\njson {\"$type\":\"HybridCreature\",\"$val\":{\"health\":7,\"label\":\"set!\"}}\nfrom_json 7 set!\nfrom_binary 7 set!\nv.health 42 8 voidkin\nv.get 42 8\nv.set 43\nv.fields [{\"access\":\"public\",\"from\":\"Voidkin\",\"kind\":\"field\",\"name\":\"chomp\",\"static\":false,\"type\":\"int\"}]\nv.methods [{\"access\":\"public\",\"arity\":0,\"from\":\"Voidkin\",\"name\":\"speak\",\"params\":[],\"static\":false}]\nv.bind voidkin voidkin\nv.type Voidkin false\nv.eq true false\nv.copy false false 43 8\nv.json {\"$type\":\"Voidkin\",\"$val\":{\"health\":43,\"label\":\"creature\",\"chomp\":8}}\nv.from_json 43 8 voidkin\nv.from_binary 43 8 voidkin\nextend 9 3 voidkin Voidkin\nextend.get 9\nextend.json {\"$type\":\"Voidkin\",\"$val\":{\"health\":9,\"label\":\"creature\",\"chomp\":3}}\ng.charge 5\ng.set 9 9\ng.spark 14 3\ng.fields [{\"access\":\"public\",\"from\":\"NativeGadget\",\"kind\":\"field\",\"name\":\"charge\",\"static\":false,\"type\":\"int\"},{\"access\":\"public\",\"from\":\"NativeGadget\",\"kind\":\"signal\",\"name\":\"on_spark\",\"static\":false,\"type\":\"var\"}]\ng.methods [{\"access\":\"public\",\"arity\":1,\"from\":\"NativeGadget\",\"name\":\"spark\",\"params\":[{\"default\":false,\"name\":\"arg0\",\"type\":\"int\"}],\"static\":false}]\ng.json {\"$type\":\"NativeGadget\",\"$val\":{\"charge\":14,\"on_spark\":{\"$type\":\"Signal\",\"$val\":{}}}}\ng.eq true false\n";
    // hybrid JSON, hybrid binary (hex), host JSON, host binary (hex), gadget JSON: one line each
    static constexpr const char* k_archives_golden = "{\"$id\": 1,\"$type\": \"Voidkin\",\"$script\": \"{\\\"chomp\\\":7}\",\"$val\": {\"health\": 42,\"label\": \"gnasher\"}}\n01000000000000000004000000030000002469640500000024747970650700000024736372697074040000002476616c040000000b0000000f000000380000000100000007000000566f69646b696e0b0000007b2263686f6d70223a377d01000000000000000002000000060000006865616c7468050000006c6162656c040000000b0000002a00000007000000676e61736865720202\n{\"$id\": 1,\"$val\": {\"health\": 13,\"label\": \"plain\"}}\n0100000000000000000200000003000000246964040000002476616c04000000360000000100000001000000000000000002000000060000006865616c7468050000006c6162656c04000000090000000d00000005000000706c61696e0202\n{\"$id\": 1,\"$val\": {\"charge\": 21,\"on_spark\": {\"parameter_names\": [],\"observers\": [],\"owned_observers\": {\"note\": {\"parameter_names\": [],\"script\": \"mark(arg_0);\"}}}}}";
    static std::vector<std::string> golden_lines() {
        std::vector<std::string> out;
        std::string text = k_archives_golden;
        size_t start = 0;
        for (size_t nl = text.find('\n'); nl != std::string::npos; nl = text.find('\n', start)) { out.push_back(text.substr(start, nl - start)); start = nl + 1; }
        out.push_back(text.substr(start));
        out.resize(5);
        return out;
    }

    void forge_tests() override {
        test("every_spelling_reaches_the_native_object", [&]() {
            auto eng = native_engine();
            eng->execute(k_report);
            check_eq(std::string(k_report_golden), eng->execute("report()").as<std::string>());
        });

        test("a_hot_reload_keeps_the_native_object", [&]() {
            auto eng = native_engine();
            eng->execute(k_reload_before);
            eng->execute(k_reload_class);
            eng->execute(k_reload_after);
            check_eq(std::string(k_reload_golden), eng->execute("after_reload()").as<std::string>());
            auto held = eng->execute("r1").as<std::shared_ptr<HybridCreature>>();
            check(held != nullptr && held->health == 61, "the C++ object behind the reloaded instance is the one it had");
        });

        test("the_native_object_is_no_member", [&]() {
            auto eng = native_engine();
            eng->execute(R"(
                function raised(function f) -> string {
                    try { f(); } catch (e) { return e; }
                    return "no error";
                }
                function probe(var o, string name) -> string {
                    string r = raised(() => reflect::get(o, name)) + "\n";
                    r += to_string(reflect::has_field(o, name)) + "\n";
                    r += to_string(reflect::fields(o).some((f) => f["name"] == name)) + "\n";
                    return r;
                }
            )");
            for (const char* make : {"HybridCreature()", "Voidkin()"}) {
                eng->execute(std::string("var probed = ") + make + ";");
                const std::string absent = eng->execute("probe(probed, \"nothere\")").as<std::string>();
                check(absent.find("has no member 'nothere'") != std::string::npos, "reflect::get raises its no-member text");
                std::string expected = absent;
                for (size_t at = expected.find("nothere"); at != std::string::npos; at = expected.find("nothere", at)) {
                    expected.replace(at, 7, "_cpp_object");
                }
                check_eq(expected, eng->execute("probe(probed, \"_cpp_object\")").as<std::string>(), "reflect sees no _cpp_object");
                auto member_text = [&](const std::string& name) {
                    try { eng->execute("var read_" + name + " = probed." + name + ";"); }
                    catch (const std::exception& e) {
                        std::string text = e.what();
                        for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) { text.replace(at, name.size(), "NAME"); }
                        return text;
                    }
                    return std::string("no error");
                };
                check_ne(std::string("no error"), member_text("nothere"));
                check_eq(member_text("nothere"), member_text("_cpp_object"), "a member read of _cpp_object raises the absent-member text");
            }
            // The checker leaves a C++-based chain's members open; a script-only class reports an absent member at load
            auto checker_text = [&](const std::string& type, const std::string& name) {
                try { eng->execute("class Plain { int a = 1; }\nfunction typed_read() -> int { " + type + " v = " + type + "(); var x = v." + name + "; return 0; }"); }
                catch (const std::exception& e) {
                    std::string text = e.what();
                    for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) { text.replace(at, name.size(), "NAME"); }
                    return text;
                }
                return std::string("no error");
            };
            check_ne(std::string("no error"), checker_text("Plain", "nothere"), "the checker reports an absent member of a script class");
            check_eq(checker_text("Plain", "nothere"), checker_text("Plain", "_cpp_object"), "the checker reports _cpp_object like any absent member");
            check_eq(checker_text("Voidkin", "nothere"), checker_text("Voidkin", "_cpp_object"), "and leaves it open on a hybrid like any other name");
        });

        test("archives_written_before_read_after", [&]() {
            auto eng = native_engine();
            eng->execute("var v = Voidkin(); v.health = 42; v.label = \"gnasher\"; v.chomp = 7;");
            auto hybrid = eng->execute("v").as<std::shared_ptr<HybridCreature>>();
            auto host = std::make_shared<HybridCreature>();
            host->health = 13;
            host->label = "plain";
            auto gadget = std::make_shared<NativeGadget>();
            gadget->charge = 21;
            gadget->note("note", "mark(arg_0);");

            std::string written;
            {
                jai::serialization::json_archive_writer w(0, eng.get());
                w(hybrid);
                written += w.str() + "\n";
            }
            {
                jai::serialization::binary_archive_writer w(eng.get());
                w(hybrid);
                written += hex(w.data()) + "\n";
            }
            {
                jai::serialization::json_archive_writer w(0, eng.get());
                w(host);
                written += w.str() + "\n";
            }
            {
                jai::serialization::binary_archive_writer w(eng.get());
                w(host);
                written += hex(w.data()) + "\n";
            }
            {
                jai::serialization::json_archive_writer w(0, eng.get());
                w(gadget);
                written += w.str();
            }
            check_eq(std::string(k_archives_golden), written, "the writers emit what the pre-change runtime wrote");
            const auto golden = golden_lines();

            auto read_json = [&](const std::string& text) {
                jai::serialization::json_archive_reader r(text, eng.get());
                std::shared_ptr<HybridCreature> out;
                r(out);
                return out;
            };
            auto read_binary = [&](const std::string& text) {
                auto bytes = unhex(text);
                jai::serialization::binary_archive_reader r(bytes.data(), bytes.size(), eng.get());
                std::shared_ptr<HybridCreature> out;
                r(out);
                return out;
            };
            for (auto loaded : {read_json(golden[0]), read_binary(golden[1])}) {
                check(loaded != nullptr, "the golden hybrid loads");
                check_eq(42, loaded->health);
                check_eq(std::string("gnasher"), loaded->label);
                eng->add_global("loadedGolden", eng->make_object(loaded));
                check_eq((script_int)7, eng->execute("loadedGolden.chomp").as_int(), "its script field");
                check_eq(std::string("voidkin"), eng->execute("loadedGolden.speak()").as<std::string>(), "its override");
                check_eq((script_int)42, eng->execute("loadedGolden.health").as_int(), "its C++ property");
            }
            for (auto loaded : {read_json(golden[2]), read_binary(golden[3])}) {
                check(loaded != nullptr, "the golden host object loads");
                check_eq(13, loaded->health);
                check_eq(std::string("plain"), loaded->label);
                check(eng->script_layer(*loaded) == nullptr, "a plain host object loads without a layer");
            }
            int64_t fired = 0;
            eng->add_function("mark", [counter = &fired](int v) { *counter += v; });
            jai::serialization::json_archive_reader r(golden[4], eng.get());
            std::shared_ptr<NativeGadget> loaded;
            r(loaded);
            check(loaded != nullptr, "the golden gadget loads");
            check_eq(21, loaded->charge.get());
            loaded->spark(4);
            check_eq((int64_t)4, fired, "its persisted text receiver fires");
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::native_object_access_tests)
FOUNDRY_REGISTER(jai::foundry::tests::polymorphic_serialization_tests)
FOUNDRY_REGISTER(jai::foundry::tests::implicit_registration_tests)
FOUNDRY_REGISTER(jai::foundry::tests::hybrid_serialization_tests)
