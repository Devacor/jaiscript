#pragma once

#ifndef __JAISCRIPT_CORE_SCRIPT_RUN_HPP__
#define __JAISCRIPT_CORE_SCRIPT_RUN_HPP__

#include "value.hpp"
#include "checked_result.hpp"
#include <chrono>
#include <memory>

namespace jai {

    namespace detail { struct run_record; }

    // A suspendable run (engine::start, docs/site/embedding/17-host-api.html#suspendable-runs): the run executes until a sleep
    // suspends it or it finishes, and resume() continues it from that sleep. Its own header because it brings <chrono>.
    class script_run {
    public:
        enum class status { suspended, completed, failed };

        script_run() noexcept;
        script_run(script_run&& other) noexcept;
        script_run& operator=(script_run&& other) noexcept;
        script_run(const script_run&) = delete;
        script_run& operator=(const script_run&) = delete;
        // A run still suspended is cancelled
        ~script_run();

        status state() const noexcept;
        bool done() const noexcept;
        // When the sleep that suspended the run ends, on the engine's time source (0 once the run is done)
        std::chrono::nanoseconds wake_at() const noexcept;
        // Continues a suspended run until its next sleep or its end. Before its wake time it returns suspended at once, switching
        // nothing. A run started from source, a file or a jaibite that fails throws as execute() would; result() holds the text.
        status resume();
        // Ends a suspended run: its sleep raises the terminal error `Run cancelled` (catches are skipped, destructors run)
        void cancel();
        // The run's value once completed, its error once failed
        const checked_result<script_value>& result() const;

    private:
        friend class engine;
        explicit script_run(std::unique_ptr<detail::run_record> record) noexcept;
        void release() noexcept;
        std::unique_ptr<detail::run_record> record_;
    };

} // namespace jai

#endif
