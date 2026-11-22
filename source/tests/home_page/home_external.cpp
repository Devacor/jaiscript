// The landing page's non-intrusive tab (docs/site/index.html#one-declaration): the regions below are the page's code
#include <jaiscript/core/registrar.hpp>
#include <jaiscript/core/overload.hpp>
#include <jaiscript/signals/signal.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

// [sample home-bind-external]
// spawner.h belongs to another module: you bind it without editing it
class Spawner {
public:
    std::string id = "north-gate";
    int64_t level = 1;
    jai::signal_emitter<void(int64_t)> onWave;
    void spawn_wave() { onWave(level); }
    void spawn_wave(int64_t new_level) { level = new_level; spawn_wave(); }
};

// your binding file
static jai::registrar<Spawner, void> spawnerBinding("Spawner", [](jai::dynamic_binder<Spawner>& b) {
    b.property("id", &Spawner::id)          // scripted and saved
     .property("level", &Spawner::level)
     .property("onWave", &Spawner::onWave)  // script-connectable, saved too
     .method("spawn_wave", jai::overload<>(&Spawner::spawn_wave))
     .method("spawn_wave", jai::overload<int64_t>(&Spawner::spawn_wave));
});
// [/sample]

namespace home_page {
// [sample home-bind-run-external]
int main() {
    auto engine = jai::engine::make();
    jai::bind_registrar(engine);   // every property_owner and registrar in the program
    auto gate = engine->execute<std::shared_ptr<Spawner>>(R"(
        void on_wave(int level) { print("wave ", level); }
        Spawner gate;
        gate.onWave.connect("hud", on_wave);
        gate.spawn_wave();
        gate.level = 2;
        gate.spawn_wave();
        gate
    )");
    gate->spawn_wave(3);     // Emits and is caught by the script function

    //Serialization Example:
    std::string spawnerJson = jai::to_json(engine, *gate);
    Spawner roundTrip = jai::from_json<Spawner>(engine, spawnerJson);
    roundTrip.spawn_wave(4);  // the loaded copy's receiver calls on_wave too
    std::cout << "Wave Level: " << roundTrip.level << "\nContents:\n" << spawnerJson << "\n";
    return 0;
}
// [/sample]
}

#include "home_main.hpp"
