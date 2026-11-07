#pragma once

#ifndef __JAISCRIPT_CORE_ENGINE_TIME_HPP__
#define __JAISCRIPT_CORE_ENGINE_TIME_HPP__

#include <chrono>
#include <functional>

namespace jai {

    class engine;

    // An engine's clock, from engine::time(). Its own header because <chrono> is among the costliest standard headers and
    // engine.hpp is in every host translation unit.
    class engine_time {
    public:
        using clock_reading = std::function<std::chrono::nanoseconds()>;
        using clock_sleep = std::function<void(std::chrono::nanoseconds)>;

        explicit engine_time(engine& owner) noexcept : owner_(&owner) {}

        // The clock every time:: call reads: monotonic is a steady reading, wall the time since the Unix epoch
        // (UTC), sleep blocks the script thread. An empty provider selects the system one (steady_clock,
        // system_clock, this_thread::sleep_for). A time:: call reads its provider once; otherwise the engine
        // reads monotonic only at a host entry, and only after a loaded program names time::execute_elapsed.
        // The execution budget and the import window run on the real steady clock whatever the provider; a time::
        // read of the system monotonic clock, and a sleep's wake while monotonic is the system one, also refresh the
        // engine's clock sample.
        void source(clock_reading monotonic, clock_reading wall, clock_sleep sleep = {});
        std::chrono::nanoseconds monotonic_now();
        std::chrono::nanoseconds wall_now();
        void sleep_for(std::chrono::nanoseconds duration);

        // Moves the running execute's deadline later by work done for the script that is not the script's own (the engine
        // starting its worker pool; a host's slow service call); nothing when no budget is armed
        void extend_execution_deadline(std::chrono::nanoseconds by);

    private:
        engine* owner_;
    };

} // namespace jai

#endif
