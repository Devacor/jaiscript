#pragma once

#ifndef __JAISCRIPT_DETAIL_RUN_TEXTS_HPP__
#define __JAISCRIPT_DETAIL_RUN_TEXTS_HPP__

#include <string_view>

namespace jai::detail {

    // The texts of sleep and the suspendable runs (docs/site/reference/index.html: the run-time errors, the host errors and the
    // static listing)
    inline constexpr std::string_view k_sleep_expects_number = "sleep expects a number of seconds";
    inline constexpr std::string_view k_run_cancelled = "Run cancelled";
    inline constexpr std::string_view k_run_wrong_thread = "A run resumes only on the thread that started it";
    inline constexpr std::string_view k_run_running = "The run is running: it resumes only while suspended";
    inline constexpr std::string_view k_run_empty = "The script_run holds no run";
    inline constexpr std::string_view k_sleep_pinned_warning =
        "sleep blocks here: a run suspends only where no host callback, destructor, signal receiver, include, import or hot_reload_migrate is running";
    inline constexpr std::string_view k_sleep_in_parallel_body = "'sleep' cannot be called in a parallel body";
    inline constexpr std::string_view k_sleep_in_destructor = "'sleep' in a destructor blocks: a run never suspends inside a destructor";
    inline constexpr std::string_view k_sleep_in_reload_hook = "'sleep' in hot_reload_migrate blocks: a run never suspends inside the reload hook";

} // namespace jai::detail

#endif
