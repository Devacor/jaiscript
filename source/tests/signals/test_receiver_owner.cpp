#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/signals/signal.hpp>
#include <jaiscript/signals/receiver_owner.hpp>
#include <jaiscript/properties/property_manager.hpp>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class receiver_owner_tests : public suite {
public:
    receiver_owner_tests() : suite("Receiver Owner") {}

    void forge_tests() override {
        test("standalone_basic_tracking", [this]() {
            signal_emitter<void(int)> sig;
            int call_count = 0;

            {
                receiver_owner owner;
                owner.track(sig.connect([&](int x) { call_count += x; }));

                check_eq(owner.size(), 1u, "One receiver tracked");
                check_eq(owner.connected_count(), 1u, "One connected");

                sig.emit(10);
                check_eq(call_count, 10, "Receiver called while owner alive");
            }
            // owner destroyed - receiver should be disconnected

            sig.emit(5);
            check_eq(call_count, 10, "Receiver not called after owner destroyed");
        });

        test("standalone_multiple_receivers", [this]() {
            signal_emitter<void()> sig1;
            signal_emitter<void(int)> sig2;
            int count1 = 0, count2 = 0;

            {
                receiver_owner owner;
                owner.track(sig1.connect([&]() { count1++; }));
                owner.track(sig2.connect([&](int x) { count2 += x; }));

                check_eq(owner.size(), 2u, "Two receivers tracked");

                sig1.emit();
                sig2.emit(5);
                check_eq(count1, 1, "First signal fired");
                check_eq(count2, 5, "Second signal fired");
            }

            sig1.emit();
            sig2.emit(10);
            check_eq(count1, 1, "First signal not fired after owner death");
            check_eq(count2, 5, "Second signal not fired after owner death");
        });

        test("standalone_clear", [this]() {
            signal_emitter<void()> sig;
            int count = 0;

            receiver_owner owner;
            owner.track(sig.connect([&]() { count++; }));
            owner.track(sig.connect([&]() { count++; }));

            sig.emit();
            check_eq(count, 2, "Both receivers fire");

            owner.clear();
            check_eq(owner.size(), 0u, "No receivers after clear");

            count = 0;
            sig.emit();
            check_eq(count, 0, "No receivers fire after clear");
        });

        test("standalone_cull_disconnected", [this]() {
            signal_emitter<void()> sig;

            receiver_owner owner;
            auto recv1 = owner.track(sig.connect([&]() {}));
            owner.track(sig.connect([&]() {}));

            check_eq(owner.size(), 2u, "Two tracked");
            check_eq(owner.connected_count(), 2u, "Two connected");

            sig.disconnect(recv1);
            check_eq(owner.size(), 2u, "Still two tracked");
            check_eq(owner.connected_count(), 1u, "Only one connected");

            owner.cull_disconnected();
            check_eq(owner.size(), 1u, "One after cull");
        });

        test("property_owner_track", [this]() {
            signal_emitter<void(int)> sig;
            int damage_taken = 0;

            struct Player : public property_owner<Player> {
                int& damage_ref;
                Player(signal<void(int)>& damage_signal, int& damage)
                    : damage_ref(damage) {
                    track(damage_signal.connect([this](int dmg) {
                        damage_ref += dmg;
                    }));
                }
            };

            signal<void(int)> damage_signal{sig};

            {
                Player player(damage_signal, damage_taken);

                sig.emit(10);
                check_eq(damage_taken, 10, "Player received damage");

                sig.emit(5);
                check_eq(damage_taken, 15, "Player received more damage");
            }
            // Player destroyed

            sig.emit(100);
            check_eq(damage_taken, 15, "No damage after player destroyed");
        });

        test("property_owner_multiple_signals", [this]() {
            signal_emitter<void()> heal_emitter;
            signal_emitter<void(int)> damage_emitter;
            signal<void()> heal_signal{heal_emitter};
            signal<void(int)> damage_signal{damage_emitter};

            int health = 100;

            struct Entity : public property_owner<Entity> {
                int& health_ref;

                Entity(signal<void()>& heal, signal<void(int)>& damage, int& h)
                    : health_ref(h) {
                    track(heal.connect([this]() { health_ref += 10; }));
                    track(damage.connect([this](int dmg) { health_ref -= dmg; }));
                }
            };

            {
                Entity entity(heal_signal, damage_signal, health);

                damage_emitter.emit(30);
                check_eq(health, 70, "Took 30 damage");

                heal_emitter.emit();
                check_eq(health, 80, "Healed 10");
            }

            // Entity destroyed - signals should have no effect
            damage_emitter.emit(50);
            heal_emitter.emit();
            check_eq(health, 80, "Health unchanged after entity death");
        });

        test("track_returns_receiver", [this]() {
            signal_emitter<void()> sig;

            receiver_owner owner;
            auto recv = owner.track(sig.connect([&]() {}));

            check_not_null(recv, "track() returns the receiver");
            check_true(recv->connected(), "Returned receiver is connected");
        });

        test("empty_check", [this]() {
            receiver_owner owner;
            check_true(owner.empty(), "New owner is empty");

            signal_emitter<void()> sig;
            owner.track(sig.connect([&]() {}));
            check_false(owner.empty(), "Owner not empty after track");

            owner.clear();
            check_true(owner.empty(), "Owner empty after clear");
        });

        test("move_semantics", [this]() {
            signal_emitter<void()> sig;
            int count = 0;

            receiver_owner owner1;
            owner1.track(sig.connect([&]() { count++; }));

            receiver_owner owner2 = std::move(owner1);
            check_eq(owner2.size(), 1u, "Moved owner has receiver");

            sig.emit();
            check_eq(count, 1, "Receiver still works after move");
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::receiver_owner_tests);
