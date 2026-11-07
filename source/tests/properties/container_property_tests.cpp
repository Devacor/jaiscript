#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/properties/property.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/observable_property.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/properties/macros.hpp>
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
