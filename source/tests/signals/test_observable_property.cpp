#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/observable_property.hpp>
#include <jaiscript/signals/signal_impl.hpp>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class observable_property_tests : public suite {
public:
    observable_property_tests() : suite("Observable Properties") {}

    void forge_tests() override {
        test("basic_change_notification", [this]() {
            property_manager mgr;
            observable_property<int> health{mgr, "health", 100};

            int old_val = 0, new_val = 0;
            auto recv = health.on_change.connect([&](const int& o, const int& n) {
                old_val = o;
                new_val = n;
            });

            health = 75;
            check_eq(old_val, 100, "Old value captured");
            check_eq(new_val, 75, "New value captured");
            check_eq(health.get(), 75, "Property value updated");
        });

        test("no_notification_on_same_value", [this]() {
            property_manager mgr;
            observable_property<int> value{mgr, "value", 42};

            int call_count = 0;
            auto recv = value.on_change.connect([&](const int&, const int&) {
                call_count++;
            });

            value = 42;  // Same value
            check_eq(call_count, 0, "No notification when value unchanged");

            value = 100;  // Different value
            check_eq(call_count, 1, "Notification when value changes");
        });

        test("set_silent_no_notification", [this]() {
            property_manager mgr;
            observable_property<int> value{mgr, "value", 0};

            int call_count = 0;
            auto recv = value.on_change.connect([&](const int&, const int&) {
                call_count++;
            });

            value.set_silent(100);
            check_eq(call_count, 0, "set_silent doesn't trigger notification");
            check_eq(value.get(), 100, "Value still updated");
        });

        test("compound_assignment_notifications", [this]() {
            property_manager mgr;
            observable_property<int> counter{mgr, "counter", 10};

            std::vector<std::pair<int, int>> changes;
            auto recv = counter.on_change.connect([&](const int& o, const int& n) {
                changes.push_back({o, n});
            });

            counter += 5;
            check_eq(changes.size(), 1u, "One notification for +=");
            check_eq(changes.back().first, 10, "+= old value");
            check_eq(changes.back().second, 15, "+= new value");

            counter -= 3;
            check_eq(changes.size(), 2u, "Notification for -=");
            check_eq(changes.back().first, 15, "-= old value");
            check_eq(changes.back().second, 12, "-= new value");

            counter *= 2;
            check_eq(changes.size(), 3u, "Notification for *=");
            check_eq(changes.back().first, 12, "*= old value");
            check_eq(changes.back().second, 24, "*= new value");
        });

        test("increment_decrement_notifications", [this]() {
            property_manager mgr;
            observable_property<int> value{mgr, "value", 10};

            std::vector<std::pair<int, int>> changes;
            auto recv = value.on_change.connect([&](const int& o, const int& n) {
                changes.push_back({o, n});
            });

            ++value;
            check_eq(changes.size(), 1u, "Pre-increment notification");
            check_eq(changes.back().first, 10, "Pre-increment old value");
            check_eq(changes.back().second, 11, "Pre-increment new value");

            value++;
            check_eq(changes.size(), 2u, "Post-increment notification");
            check_eq(changes.back().first, 11, "Post-increment old value");
            check_eq(changes.back().second, 12, "Post-increment new value");

            --value;
            check_eq(changes.size(), 3u, "Pre-decrement notification");

            value--;
            check_eq(changes.size(), 4u, "Post-decrement notification");
        });

        test("string_property_notification", [this]() {
            property_manager mgr;
            observable_property<std::string> name{mgr, "name", "Initial"};

            std::string old_name, new_name;
            auto recv = name.on_change.connect([&](const std::string& o, const std::string& n) {
                old_name = o;
                new_name = n;
            });

            name = std::string("Updated");
            check_eq(old_name, "Initial", "String old value");
            check_eq(new_name, "Updated", "String new value");
        });

        test("multiple_observers", [this]() {
            property_manager mgr;
            observable_property<int> value{mgr, "value", 0};

            int observer1_count = 0;
            int observer2_count = 0;
            int observer3_count = 0;

            auto recv1 = value.on_change.connect([&](const int&, const int&) {
                observer1_count++;
            });
            auto recv2 = value.on_change.connect([&](const int&, const int&) {
                observer2_count++;
            });
            auto recv3 = value.on_change.connect([&](const int&, const int&) {
                observer3_count++;
            });

            value = 100;
            check_eq(observer1_count, 1, "Observer 1 notified");
            check_eq(observer2_count, 1, "Observer 2 notified");
            check_eq(observer3_count, 1, "Observer 3 notified");
        });

        test("disconnect_observer", [this]() {
            property_manager mgr;
            observable_property<int> value{mgr, "value", 0};

            int count = 0;
            auto recv = value.on_change.connect([&](const int&, const int&) {
                count++;
            });

            value = 1;
            check_eq(count, 1, "Observer called");

            value.on_change.disconnect(recv);
            value = 2;
            check_eq(count, 1, "Disconnected observer not called");
        });

        test("conversion_operators_work", [this]() {
            property_manager mgr;
            observable_property<int> value{mgr, "value", 42};

            int x = value;  // Implicit conversion
            check_eq(x, 42, "Conversion to int works");

            value = 100;
            int y = value + 5;
            check_eq(y, 105, "Arithmetic with observable property");
        });

        test("property_in_class", [this]() {
            // Test the typical usage pattern
            struct Player {
                property_manager property_mgr;
                observable_property<int> health{property_mgr, "health", 100};
                observable_property<std::string> name{property_mgr, "name", "Player"};
                std::shared_ptr<receiver<void(const int&, const int&)>> death_check_;

                Player() {
                    // Example: set up death check - must store handle to keep receiver alive
                    death_check_ = health.on_change.connect([this](const int&, const int& new_health) {
                        if (new_health <= 0) {
                            name = std::string("Dead Player");
                        }
                    });
                }
            };

            Player p;
            p.health = 50;
            check_eq(p.name.get(), "Player", "Name unchanged when health > 0");

            p.health = 0;
            check_eq(p.name.get(), "Dead Player", "Name changed when health <= 0");
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::observable_property_tests);
