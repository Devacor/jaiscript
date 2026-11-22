// The landing page's intrusive tab (docs/site/index.html#one-declaration): the regions below are the page's code
#include <jaiscript/core/registrar.hpp>
#include <jaiscript/properties.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/signals/signal_property.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

// [sample home-bind-intrusive]
class Spawner : public jai::property_owner<Spawner> {
public:
    JAI_PROPERTY((std::string), id, "north-gate");   // scripted and saved
    JAI_PROPERTY((int64_t), level, 1);
    JAI_SIGNAL(void(int64_t), onWave);               // script-connectable, saved too
    JAI_METHOD(void, spawn_wave, ()) { onWaveSignal(level); }
    JAI_METHOD(void, spawn_wave, (int64_t new_level)) { level = new_level; spawn_wave(); }
};
// [/sample]

namespace home_page {
// [sample home-bind-run-intrusive]
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
