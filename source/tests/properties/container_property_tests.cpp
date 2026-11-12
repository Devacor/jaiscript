#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/properties/property.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/observable_property.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/properties/macros.hpp>
#include <jaiscript/signals/signal_property.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <typeindex>
#include <utility>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// Simple test class
class Cat {
public:
    std::string name;

    Cat() = default;
    explicit Cat(const std::string& n) : name(n) {}

    std::string meow_name() const {
        return "Meow! I'm " + name;
    }
};

// Test class with vector<Cat> property
class CatOwner {
public:
    std::vector<Cat> my_cats;

    CatOwner() = default;
};

class container_property_tests : public suite {
public:
    container_property_tests() : suite("Container Property Tests") {}

    void forge_tests() override {

        test("basic_vector_access", [this]() {
            // Test basic access to pre-populated vector
            auto eng = make_engine();
            stdlib::register_all(eng);

            dynamic_binder<Cat>(*eng, "Cat")
                .constructor<>()
                .constructor<std::string>()
                .method("meow_name", &Cat::meow_name)
                .property("name", &Cat::name)
                .build();

            dynamic_binder<CatOwner>(*eng, "CatOwner")
                .constructor<>()
                .property("my_cats", &CatOwner::my_cats)
                .build();

            // C++: Create and populate
            auto owner = std::make_shared<CatOwner>();
            owner->my_cats.emplace_back("Fred");
            owner->my_cats.emplace_back("Whiskers");
            eng->add_global("owner", eng->make_object(owner));

            // Script: Just access, don't modify yet
            auto result = eng->execute(R"(
                print(`Size: ${owner.my_cats.size()}`);
                print(`First cat: ${owner.my_cats[0].name}`);
                owner.my_cats.size() == 2 && owner.my_cats[0].name == "Fred"
            )");

            check_eq(result.as<bool>(), true);
            std::cout << "✓ Basic vector access works" << std::endl;
        });

        test("vector_registration", [this]() {
            auto eng = make_engine();

            dynamic_binder<Cat>(*eng, "Cat")
                .constructor<>()
                .constructor<std::string>()
                .method("meow_name", &Cat::meow_name)
                .property("name", &Cat::name)
                .build();

            // Should succeed - containers not validated at registration
            dynamic_binder<CatOwner>(*eng, "CatOwner")
                .constructor<>()
                .property("my_cats", &CatOwner::my_cats)
                .build();

            std::cout << "✓ Can register vector<Cat> property" << std::endl;
        });

        test("cpp_populate_script_access", [this]() {
            auto eng = make_engine();

            dynamic_binder<Cat>(*eng, "Cat")
                .constructor<>()
                .constructor<std::string>()
                .method("meow_name", &Cat::meow_name)
                .property("name", &Cat::name)
                .build();

            dynamic_binder<CatOwner>(*eng, "CatOwner")
                .constructor<>()
                .property("my_cats", &CatOwner::my_cats)
                .build();

            // C++ populates
            auto owner = std::make_shared<CatOwner>();
            owner->my_cats.emplace_back("Fred");
            owner->my_cats.emplace_back("Whiskers");
            eng->add_global("owner", eng->make_object(owner));

            // Script accesses
            auto result = eng->execute(R"(
                owner.my_cats.size() == 2 &&
                owner.my_cats[0].name == "Fred"
            )");

            check_eq(result.as<bool>(), true);
            std::cout << "✓ C++ populates, script accesses" << std::endl;
        });

        test("script_populate_and_access", [this]() {
            auto eng = make_engine();

            dynamic_binder<Cat>(*eng, "Cat")
                .constructor<>()
                .constructor<std::string>()
                .method("meow_name", &Cat::meow_name)
                .property("name", &Cat::name)
                .build();

            dynamic_binder<CatOwner>(*eng, "CatOwner")
                .constructor<>()
                .property("my_cats", &CatOwner::my_cats)
                .build();

            // Script creates and populates
            auto result = eng->execute(R"(
                auto owner = CatOwner();
                owner.my_cats.push_back(Cat("Mittens"));
                owner.my_cats.push_back(Cat("Shadow"));
                owner.my_cats.size() == 2 &&
                owner.my_cats[0].meow_name() == "Meow! I'm Mittens"
            )");

            check_eq(result.as<bool>(), true);
            std::cout << "✓ Script creates and populates" << std::endl;
        });
    }
};

FOUNDRY_REGISTER(jai::foundry::tests::container_property_tests)

// Owners exercising the erased reflection bridge across an inheritance level.
class reflection_test_base : public property_owner<reflection_test_base> {
public:
    JAI_PROPERTY((int), base_count, 7);
    JAI_PROPERTY((std::string), base_name, "stone");
};

class reflection_test_derived : public property_owner<reflection_test_derived, reflection_test_base> {
public:
    JAI_PROPERTY((double), speed, 1.5);
    JAI_OBSERVABLE_PROPERTY((int), health, 100);
    JAI_DELETED_PROPERTY((float), old_field);
};

class property_reflection_tests : public suite {
public:
    property_reflection_tests() : suite("Property Reflection") {}

    void forge_tests() override {

        test("erased_bridge_typed_access", [this]() {
            reflection_test_derived d;
            property_base* p = d.find_owned_property("speed");
            check_not_null(p);
            check(p->value_type_id() == std::type_index(typeid(double)));

            const double* read = p->value_as<double>();
            check_not_null(read);
            check_near(1.5, *read, 1e-12);

            check_true(p->assign_value(2.5));
            check_near(2.5, d.speed.get(), 1e-12);

            check_null(p->value_as<int>());
            check_false(p->assign_value(5));
        });

        test("owner_helpers_walk_inheritance", [this]() {
            reflection_test_derived d;

            const int* inherited = d.property_value<int>("base_count");
            check_not_null(inherited);
            check_eq(7, *inherited);

            check_true(d.set_property_value("base_name", std::string("iron")));
            check_eq(std::string("iron"), d.base_name.get());

            check(d.property_type_id("base_name") == std::type_index(typeid(std::string)));
            check(d.property_type_id("missing") == std::type_index(typeid(void)));
            check_null(d.property_value<int>("missing"));
            check_false(d.set_property_value("missing", 1));
        });

        test("observable_erased_assign_fires_on_change", [this]() {
            reflection_test_derived d;
            int fires = 0;
            int seen_old = 0;
            int seen_new = 0;
            auto rec = d.health.on_change.connect([&](const int& old_value, const int& new_value) {
                ++fires;
                seen_old = old_value;
                seen_new = new_value;
            });

            check_true(d.set_property_value("health", 55));
            check_eq(1, fires);
            check_eq(100, seen_old);
            check_eq(55, seen_new);

            check_true(d.set_property_value("health", 55));
            check_eq(1, fires);
        });

        test("deleted_property_is_valueless", [this]() {
            reflection_test_derived d;
            property_base* p = d.find_owned_property("old_field");
            check_not_null(p);
            check(p->value_type_id() == std::type_index(typeid(void)));
            check_null(p->value_as<float>());
            check_false(p->assign_value(1.0f));
        });

        test("base_pointer_reflects_derived_chain", [this]() {
            reflection_test_derived d;
            reflection_test_base& as_base = d;

            std::vector<std::string> names;
            as_base.visit_reflected_properties([&](const std::string& name, property_base*) {
                names.push_back(name);
            });
            check_true(std::find(names.begin(), names.end(), "speed") != names.end());
            check_true(std::find(names.begin(), names.end(), "base_count") != names.end());

            property_base* p = as_base.find_reflected_property("speed");
            check_not_null(p);
            check_true(p->assign_value(3.5));
            check_near(3.5, d.speed.get(), 1e-12);
        });

        test("visit_enumerates_all_levels", [this]() {
            reflection_test_derived d;
            std::vector<std::string> names;
            d.visit_owned_properties([&](const std::string& name, property_base*) {
                names.push_back(name);
            });
            auto has = [&](const char* n) {
                return std::find(names.begin(), names.end(), n) != names.end();
            };
            check_true(has("base_count"));
            check_true(has("base_name"));
            check_true(has("speed"));
            check_true(has("health"));
        });
    }
};

FOUNDRY_REGISTER(jai::foundry::tests::property_reflection_tests)

// Pins the closure-vs-global semantics an editor script-panel pattern depends on:
// files run on a SHARED engine with add_global("panel", ...) re-injected per file, and
// script lambdas stored into std::function hooks fire long after execution.
class hook_holder {
public:
    std::function<std::string()> fn;
};

class lambda_capture_probes : public suite {
public:
    lambda_capture_probes() : suite("Lambda Capture Probes") {}

    void forge_tests() override {

        auto makeHolderEngine = [](std::shared_ptr<hook_holder>& a_holder) {
            auto eng = make_engine();
            dynamic_binder<hook_holder>(*eng, "HookHolder")
                .property("fn", &hook_holder::fn)
                .build();
            a_holder = std::make_shared<hook_holder>();
            eng->add_global("holder", eng->make_object(a_holder));
            return eng;
        };

        test("global_reference_resolves_at_call_time", [this, makeHolderEngine]() {
            std::shared_ptr<hook_holder> holder;
            auto eng = makeHolderEngine(holder);
            eng->add_global("who", script_value(std::string("first"), eng.get()));
            eng->execute("holder.fn = []() { return who; };");
            eng->add_global("who", script_value(std::string("second"), eng.get()));
            check_eq(std::string("second"), holder->fn());
        });

        test("module_scope_var_is_shared_across_executes_of_the_same_unit", [this, makeHolderEngine]() {
            // another text declaring `alias` shadows (the closure keeps its unit's cell);
            // the same text executed again is the same unit and writes the cell in place
            std::shared_ptr<hook_holder> holder;
            auto eng = makeHolderEngine(holder);
            const std::string first = "var alias = who; holder.fn = []() { return alias; };";
            eng->add_global("who", script_value(std::string("first"), eng.get()));
            eng->execute(first);
            eng->add_global("who", script_value(std::string("second"), eng.get()));
            eng->execute("var alias = who;");
            check_eq(std::string("first"), holder->fn());
            eng->execute(first);
            check_eq(std::string("second"), holder->fn());
        });

        test("function_local_capture_survives_and_pins", [this, makeHolderEngine]() {
            std::shared_ptr<hook_holder> holder;
            auto eng = makeHolderEngine(holder);
            eng->add_global("who", script_value(std::string("first"), eng.get()));
            eng->execute(R"(
                function bind() -> auto {
                    var pinned = who;
                    holder.fn = [pinned]() { return pinned; };
                    return 0;
                }
                bind();
            )");
            eng->add_global("who", script_value(std::string("second"), eng.get()));
            eng->execute("var pinned = \"clobbered-global\";");
            check_eq(std::string("first"), holder->fn());
        });

        test("named_capture_copies_at_creation", [this, makeHolderEngine]() {
            std::shared_ptr<hook_holder> holder;
            auto eng = makeHolderEngine(holder);
            eng->execute(R"(
                function bind() -> auto {
                    var value = "before";
                    holder.fn = [value]() { return value; };
                    value = "after";
                    return 0;
                }
                bind();
            )");
            check_eq(std::string("before"), holder->fn());
        });

        test("reference_capture_cell_shares", [this, makeHolderEngine]() {
            std::shared_ptr<hook_holder> holder;
            auto eng = makeHolderEngine(holder);
            eng->execute(R"(
                function bind() -> auto {
                    var value = "before";
                    holder.fn = [&]() { return value; };
                    value = "after";
                    return 0;
                }
                bind();
            )");
            check_eq(std::string("after"), holder->fn());
        });
    }
};

FOUNDRY_REGISTER(jai::foundry::tests::lambda_capture_probes)

// Edge-sanding pins: move-only properties, name-shadowing dedup, truthful schema flags,
// registry thread-safety (census-backed fixes).
class hardened_base : public property_owner<hardened_base> {
public:
    JAI_PROPERTY((int), shared_name, 1);
    JAI_TRANSIENT_PROPERTY((int), cached_value, 0);
};

class hardened_derived : public property_owner<hardened_derived, hardened_base> {
public:
    JAI_PROPERTY((int), shared_name, 2);
    JAI_DELETED_PROPERTY((float), retired_field);
};

class move_only_owner : public property_owner<move_only_owner> {
public:
    JAI_PROPERTY((std::unique_ptr<int>), exclusive, nullptr, [](jai::property<std::unique_ptr<int>>& a_from, jai::property<std::unique_ptr<int>>& a_to) {
        a_to = a_from.get() ? std::make_unique<int>(*a_from.get()) : nullptr;
    });
};

enum class relocated_mood { calm, stormy };

class relocated_item : public property_owner<relocated_item> {
public:
    JAI_PROPERTY((std::string), name);
    JAI_PROPERTY((int), count, 1);
};

class relocated_save : public property_owner<relocated_save> {
public:
    JAI_PROPERTY((int), level, 1);
    JAI_PROPERTY((std::string), hero);
    JAI_PROPERTY((std::vector<relocated_item>), items);
    JAI_PROPERTY((relocated_mood), mood, relocated_mood::calm);
    JAI_PROPERTY((std::string), a_property_name_past_the_small_string_buffer, "long");
};

class relocated_side : public property_owner<relocated_side> {
public:
    JAI_PROPERTY((std::string), side_note, "side");
};

class relocated_both : public property_owner<relocated_both, hardened_base, relocated_side> {
public:
    JAI_PROPERTY((int), own_value, 3);
};

class relocated_listener : public property_owner<relocated_listener> {
public:
    JAI_PROPERTY((int), volume, 5);
    JAI_OBSERVABLE_PROPERTY((int), health, 100);
    JAI_SIGNAL(void(int), onHeard);

    void hear(int a_value) { onHeardSignal(a_value); }
    void follow(jai::signal<void(int)>& a_signal, int& a_total) {
        track(a_signal.connect([&a_total](int a_value) { a_total += a_value; }));
    }
    size_t followed() const { return receivers().size(); }
};

class relocated_hand_copied : public property_owner<relocated_hand_copied> {
public:
    relocated_hand_copied() = default;
    relocated_hand_copied(const relocated_hand_copied& a_other) : property_owner<relocated_hand_copied>(a_other) {}

    JAI_PROPERTY((int), amount, 4);
};

template<typename Owner>
bool registered_in_place(Owner& a_owner, const std::string& a_name, const property_base& a_member) {
    return a_owner.property_mgr.get(a_name) == &a_member;
}

class property_hardening_tests : public suite {
public:
    property_hardening_tests() : suite("Property Hardening") {}

    void forge_tests() override {

        test("move_only_property_compiles_and_custom_clones", [this]() {
            move_only_owner source;
            source.exclusive = std::make_unique<int>(42);
            move_only_owner target;
            source.property_mgr.clone_to_target(target.property_mgr);
            check_not_null(target.exclusive.get().get());
            check_eq(42, *target.exclusive.get());
        });

        test("shadowed_property_visits_once_derived_wins", [this]() {
            hardened_derived d;
            int visits = 0;
            jai::property_base* winner = nullptr;
            d.visit_owned_properties([&](const std::string& a_name, jai::property_base* a_property) {
                if (a_name == "shared_name") {
                    ++visits;
                    winner = a_property;
                }
            });
            check_eq(1, visits);
            check_not_null(winner);
            check_eq(2, *winner->value_as<int>());
        });

        test("schema_flags_are_truthful", [this]() {
            hardened_derived instance;
            (void)instance;
            bool checked_transient = false;
            bool checked_deleted = false;
            bool checked_plain = false;
            for (const auto* meta : type_registry::instance().all_properties<hardened_derived>()) {
                if (meta->name == "cached_value") {
                    check_false(meta->default_allow_serialization);
                    checked_transient = true;
                } else if (meta->name == "retired_field") {
                    check_false(meta->default_allow_serialization);
                    checked_deleted = true;
                } else if (meta->name == "shared_name") {
                    check_true(meta->default_allow_serialization);
                    checked_plain = true;
                }
            }
            check_true(checked_transient);
            check_true(checked_deleted);
            check_true(checked_plain);
        });

        test("an_owner_moves_and_copies_with_its_properties_registered_in_place", [this]() {
            static_assert(std::is_nothrow_move_constructible_v<relocated_save> && std::is_nothrow_move_assignable_v<relocated_save>);
            static_assert(std::is_copy_constructible_v<relocated_save> && std::is_copy_assignable_v<relocated_save>);
            static_assert(std::is_nothrow_move_constructible_v<relocated_both> && std::is_copy_constructible_v<relocated_both>);
            static_assert(std::is_nothrow_move_constructible_v<move_only_owner> && std::is_nothrow_move_assignable_v<move_only_owner>);
            static_assert(!std::is_copy_constructible_v<move_only_owner> && !std::is_copy_assignable_v<move_only_owner>, "an owner of a move-only property is move-only: a copy fails where it is written");
            static_assert(!std::is_copy_constructible_v<property<std::unique_ptr<int>>> && std::is_move_constructible_v<property<std::unique_ptr<int>>>);

            auto eng = make_engine();
            std::vector<relocated_save> saves;
            const char* heroText = nullptr;
            const relocated_item* itemStorage = nullptr;
            for (int i = 0; i < 9; ++i) {
                relocated_save save;
                save.level = i;
                save.hero = "a hero whose name is longer than any small string buffer " + std::to_string(i);
                save.items->emplace_back();
                save.items->back().name = "potion";
                save.items->back().count = i + 1;
                save.mood = relocated_mood::stormy;
                if (i == 0) {
                    heroText = save.hero->data();
                    itemStorage = save.items->data();
                }
                saves.push_back(std::move(save));
                check_eq((size_t)0, save.items->size(), "a moved-from owner gave its values away");
                check_true(registered_in_place(save, "level", save.level), "and keeps its own properties registered");
                save = relocated_save{};
                check_eq(1, save.level.get(), "a moved-from owner takes a new value");
            }
            check_true(heroText == saves[0].hero->data(), "a move carries a string's storage");
            check_true(itemStorage == saves[0].items->data(), "a move carries a vector's storage");
            for (int i = 0; i < 9; ++i) {
                check_eq(i, saves[i].level.get());
                check_eq((size_t)5, saves[i].property_mgr.all().size());
                check_true(registered_in_place(saves[i], "level", saves[i].level));
                check_true(registered_in_place(saves[i], "hero", saves[i].hero));
                check_true(registered_in_place(saves[i], "items", saves[i].items));
                check_true(registered_in_place(saves[i], "mood", saves[i].mood));
                check_true(registered_in_place(saves[i], "a_property_name_past_the_small_string_buffer", saves[i].a_property_name_past_the_small_string_buffer));
                check_true(registered_in_place(saves[i].items.get()[0], "count", saves[i].items.get()[0].count));
            }

            saves.front().hero = "Ada";
            std::string text = jai::to_json(*eng, saves.front(), 0);
            check_eq(std::string(R"({"a_property_name_past_the_small_string_buffer": "long","hero": "Ada","items": [{"count": 1,"name": "potion"}],"level": 0,"mood": 1})"), text);
            relocated_save fromText = jai::from_json<relocated_save>(*eng, text);
            check_eq(std::string("Ada"), fromText.hero.get());
            check_eq(std::string("potion"), fromText.items.get()[0].name.get());
            check_true(fromText.mood.get() == relocated_mood::stormy);
            relocated_save fromBytes = jai::from_binary<relocated_save>(*eng, jai::to_binary(*eng, saves[3]));
            check_eq(3, fromBytes.level.get());
            check_eq(4, fromBytes.items.get()[0].count.get());
            check_true(registered_in_place(fromBytes, "level", fromBytes.level));

            relocated_save copy = saves[2];
            check_eq(2, copy.level.get());
            check_eq(2, saves[2].level.get());
            check_true(copy.hero->data() != saves[2].hero->data(), "a copy owns its values");
            check_true(registered_in_place(copy, "items", copy.items));
            check_true(registered_in_place(copy.items.get()[0], "name", copy.items.get()[0].name));
            copy.level = 40;
            check_eq(2, saves[2].level.get());
            copy = saves[4];
            check_eq(4, copy.level.get());
            copy = std::move(saves[5]);
            check_eq(5, copy.level.get());
            check_eq(saves[4].hero.get(), jai::from_json<relocated_save>(*eng, jai::to_json(*eng, saves[4], 0)).hero.get(), "an owner assigned from still saves itself");
        });

        test("every_level_of_a_moved_or_copied_owner_registers_in_place", [this]() {
            relocated_both source;
            source.shared_name = 11;
            source.cached_value = 12;
            source.side_note = "a side note longer than the small string buffer";
            source.own_value = 13;
            source.shared_name.serialize_enabled(false);

            relocated_both copy = source;
            check_eq(11, copy.shared_name.get());
            check_eq(12, copy.cached_value.get());
            check_eq(13, copy.own_value.get());
            check_eq(source.side_note.get(), copy.side_note.get());
            check_true(!copy.shared_name.allow_save(), "a copy keeps a property's save switch");
            check_true(copy.find_owned_property("shared_name") == &copy.shared_name);
            check_true(copy.find_owned_property("side_note") == &copy.side_note);
            check_true(copy.find_owned_property("own_value") == &copy.own_value);

            const char* noteText = source.side_note->data();
            relocated_both moved = std::move(source);
            check_true(noteText == moved.side_note->data());
            check_eq(11, moved.shared_name.get());
            check_true(moved.find_owned_property("cached_value") == &moved.cached_value);
            check_true(moved.find_owned_property("side_note") == &moved.side_note);
            check_true(source.find_owned_property("side_note") == &source.side_note);

            relocated_both assigned;
            assigned = std::move(moved);
            check_eq(13, assigned.own_value.get());
            check_eq(12, assigned.cached_value.get());
            check_true(noteText == assigned.side_note->data());
            assigned = copy;
            check_eq(copy.side_note.get(), assigned.side_note.get());

            hardened_base sliced = copy;
            check_eq(11, sliced.shared_name.get());
            check_eq((size_t)2, sliced.property_mgr.all().size());
            check_true(registered_in_place(sliced, "shared_name", sliced.shared_name));

            relocated_hand_copied first;
            first.amount = 9;
            relocated_hand_copied second = first;
            check_true(registered_in_place(second, "amount", second.amount));
            check_eq((size_t)1, second.property_mgr.all().size());

            move_only_owner exclusiveSource;
            exclusiveSource.exclusive = std::make_unique<int>(8);
            move_only_owner exclusiveTarget = std::move(exclusiveSource);
            check_eq(8, *exclusiveTarget.exclusive.get());
            check_true(registered_in_place(exclusiveTarget, "exclusive", exclusiveTarget.exclusive));
            check_true(exclusiveSource.exclusive.get() == nullptr);
            exclusiveSource = std::move(exclusiveTarget);
            check_eq(8, *exclusiveSource.exclusive.get());
            std::vector<move_only_owner> exclusives;
            exclusives.push_back(std::move(exclusiveSource));
            exclusives.resize(6);
            check_eq(8, *exclusives.front().exclusive.get());
            check_true(registered_in_place(exclusives.front(), "exclusive", exclusives.front().exclusive));
        });

        test("a_move_carries_connections_and_a_copy_starts_unconnected", [this]() {
            static_assert(std::is_nothrow_move_constructible_v<relocated_listener> && std::is_nothrow_move_assignable_v<relocated_listener>);
            static_assert(std::is_copy_constructible_v<relocated_listener> && std::is_copy_assignable_v<relocated_listener>);

            signal_emitter<void(int)> outside;
            signal<void(int)> outsideView{ outside };
            int followedTotal = 0;
            int heard = 0;
            int healthChanges = 0;

            relocated_listener source;
            source.follow(outsideView, followedTotal);
            auto heardConnection = source.onHeard.connect([&heard](int a_value) { heard += a_value; });
            auto healthConnection = source.health.on_change.connect([&healthChanges](const int&, const int&) { ++healthChanges; });
            source.onHeard.connect("named", [&heard](int a_value) { heard += 100 * a_value; });
            source.health = 90;
            check_eq(1, healthChanges);

            relocated_listener copy = source;
            check_eq(90, copy.health.get());
            check_eq((size_t)0, copy.onHeard.observer_count(), "a copy has no receivers");
            check_eq((size_t)0, copy.health.on_change.observer_count());
            check_eq((size_t)0, copy.followed());
            copy.hear(1);
            copy.health = 80;
            check_eq(0, heard);
            check_eq(1, healthChanges);
            int copyHeard = 0;
            auto copyConnection = copy.onHeard.connect([&copyHeard](int a_value) { copyHeard += a_value; });
            copy.hear(2);
            check_eq(2, copyHeard, "a copy's signal is its own emitter's");
            check_eq(0, heard);
            signal_script_ops copyOps;
            check_true(copy.property_mgr.get("onHeard")->signal_ops(copyOps));
            check_true(copyOps.connected_fn(copyOps.source.get(), "named") == false);

            relocated_listener moved = std::move(source);
            check_eq((size_t)2, moved.onHeard.observer_count(), "a move carries the receivers");
            check_eq((size_t)0, source.onHeard.observer_count());
            check_true(heardConnection->connected());
            check_true(moved.onHeard.connected("named"));
            moved.hear(3);
            check_eq(303, heard);
            source.hear(50);
            check_eq(303, heard, "the moved-from owner emits to nobody");
            int sourceHeard = 0;
            auto sourceConnection = source.onHeard.connect([&sourceHeard](int a_value) { sourceHeard += a_value; });
            source.hear(6);
            check_eq(6, sourceHeard, "the moved-from owner's signal is its own emitter's");
            check_eq(303, heard);
            check_eq((size_t)2, moved.onHeard.observer_count());
            moved.health = 70;
            check_eq(2, healthChanges);
            source.health = 60;
            check_eq(2, healthChanges);
            check_eq((size_t)1, moved.followed());
            check_eq((size_t)0, source.followed());
            outside(4);
            check_eq(4, followedTotal);
            signal_script_ops movedOps;
            check_true(moved.property_mgr.get("onHeard")->signal_ops(movedOps));
            check_true(movedOps.connected_fn(movedOps.source.get(), "named"));

            relocated_listener assigned;
            int dropped = 0;
            auto droppedConnection = assigned.onHeard.connect([&dropped](int a_value) { dropped += a_value; });
            assigned = std::move(moved);
            assigned.hear(1);
            check_eq(404, heard, "move assignment takes the source's receivers");
            check_eq(0, dropped, "and lets go of the target's");
            check_true(!droppedConnection->connected());
            assigned.health = 50;
            check_eq(3, healthChanges);

            assigned = copy;
            check_eq(80, assigned.health.get());
            check_eq(3, healthChanges, "copy assignment keeps the target's receivers and clones the value in without a change signal");
            assigned.health = 81;
            check_eq(4, healthChanges);
            assigned.hear(1);
            check_eq(505, heard);
            check_eq(2, copyHeard);

            std::vector<relocated_listener> listeners;
            listeners.push_back(std::move(assigned));
            for (int i = 0; i < 8; ++i) {
                listeners.emplace_back();
            }
            listeners.front().hear(1);
            check_eq(606, heard, "receivers follow an element through a vector's growth");
        });

        test("type_registry_concurrent_first_touch", [this]() {
            std::atomic<bool> go{ false };
            auto hammer = [&](int a_salt) {
                while (!go) {}
                for (int i = 0; i < 200; ++i) {
                    type_registry::instance().for_type(std::type_index(typeid(std::pair<int, int>)));
                    type_registry::instance().all_properties<hardened_derived>();
                    (void)a_salt;
                }
            };
            std::thread first(hammer, 1);
            std::thread second(hammer, 2);
            go = true;
            first.join();
            second.join();
            check_true(type_registry::instance().has_schema<hardened_derived>());
        });
    }
};

FOUNDRY_REGISTER(jai::foundry::tests::property_hardening_tests)

} // namespace jai::foundry::tests
