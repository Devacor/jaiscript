#pragma once

#ifndef __JAISCRIPT_DETAIL_RUN_RECORD_HPP__
#define __JAISCRIPT_DETAIL_RUN_RECORD_HPP__

#include <jaiscript/core/value.hpp>
#include <jaiscript/core/checked_result.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/detail/run_texts.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace jai {
    class engine;
    class jaibite;
}

namespace jai::detail {

    // A pooled native fiber (script_run.cpp): its stack holds a suspended run's native frames
    struct run_fiber;
    void destroy_run_fiber(run_fiber* fiber) noexcept;

    // What a run changes in the engine itself as it executes: exchanged with the engine's own at every switch
    // (engine::swap_run_entry_state), so a run's host entry, its unit, its limits and its debugger step outlive a suspension
    struct run_entry_state {
        uint64_t entry_generation = 0;
        std::chrono::nanoseconds entry_start{};
        uint64_t stamped_generation = UINT64_MAX;
        unit_stamp unit{};
        int load_depth = 0;
        std::vector<std::pair<script_value, script_int>> deferred_post_loads;
        bool terminal_error = false;
        uint64_t steps_taken = 0;
        int vm_depth_gate = 0;
        size_t vm_frame_bytes = 0;
        size_t memory_used = 0;
        uint32_t memory_raises = 0;
        bool import_pass_open = false;
        int step_mode = 0;
        int step_base_depth = 0;
        int stop_depth = 0;
    };

    // One suspendable run (engine::start): the script_run the host holds owns it
    struct run_record {
        enum class body_kind : uint8_t { source, file, file_source, bite, callable };
        enum class phase : uint8_t { running, suspended, completed, failed };

        engine* eng = nullptr;
        std::weak_ptr<const void> alive;
        run_fiber* fiber = nullptr;
        body_kind kind = body_kind::source;
        std::string text;
        std::string content;   // file_source: the file's text (text is its path)
        jai::jaibite* bite = nullptr;
        std::optional<script_value> callable;
        std::vector<script_value> args;
        phase state = phase::running;
        std::optional<checked_result<script_value>> result;
        std::exception_ptr thrown;
        std::chrono::nanoseconds sleep_started{};
        std::chrono::nanoseconds wake{};
        std::thread::id thread;
        int pins = 0;
        bool warned = false;
        bool cancel = false;
        bool finished = false;   // the body returned (the fiber is idle again)
        run_record* outer = nullptr;   // the run that was innermost when this one was entered
        std::unique_ptr<backend_run_state> backend_state;
        run_entry_state entry;
    };

    // The engine's runs: the innermost active one, the idle fibers and records kept for reuse, the suspended runs a teardown cancels,
    // and the core sleep function's identity (a call whose callee is it sleeps through the builtin)
    struct run_registry {
        run_record* active = nullptr;
        std::vector<run_fiber*> idle_fibers;
        std::vector<std::unique_ptr<backend_run_state>> spare_states;
        std::vector<std::unique_ptr<run_record>> spare_records;
        std::vector<run_record*> suspended;
        script_value sleep_function{std::monostate{}, nullptr};
        const void* sleep_identity = nullptr;

        run_registry() = default;
        run_registry(const run_registry&) = delete;
        run_registry& operator=(const run_registry&) = delete;
        ~run_registry() { release_pools(); }
        // the pooled states hold environments: they go while the engine still runs what their release runs
        void release_pools() noexcept {
            spare_records.clear();
            spare_states.clear();
            for (run_fiber* f : idle_fibers) { destroy_run_fiber(f); }
            idle_fibers.clear();
        }
    };

} // namespace jai::detail

#endif
