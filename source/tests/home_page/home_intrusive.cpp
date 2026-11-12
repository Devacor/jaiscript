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
    JAI_METHOD(void, spawn_wave, ()) { spawn_wave(level, false); }
    JAI_METHOD(void, spawn_wave, (int64_t size, bool elite)) { level = size; onWaveSignal(elite ? size * 2 : size); }
};
// [/sample]

namespace home_page {
// [sample home-bind-run-intrusive]
int main() {
    auto engine = jai::engine::make();
    jai::bind_registrar(engine);   // every property_owner and registrar in the program
    auto gate = engine->execute<std::shared_ptr<Spawner>>(R"(
        Spawner gate;
        gate.onWave.connect("hud", [](int size) { print("wave of ", size); });
        gate.level = 3;
        gate.spawn_wave();
        gate.spawn_wave(4, true);
        gate
    )");
    gate->spawn_wave(5, false);     // Emits and is caught by the script-bound lambda

    //Serialization Example:
    std::string spawnerJson = jai::to_json(engine, *gate);
    Spawner roundTrip = jai::from_json<Spawner>(engine, spawnerJson);
    std::cout << "Wave Level: " << roundTrip.level << "\nContents:\n" << spawnerJson << "\n";
    return 0;
}
// [/sample]
}

#include "home_main.hpp"
