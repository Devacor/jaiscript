#pragma once

#ifndef __JAISCRIPT_DETAIL_THREAD_POOL_ACCESS_HPP__
#define __JAISCRIPT_DETAIL_THREAD_POOL_ACCESS_HPP__

// The library's own reach into jai::thread_pool, for parallel regions and their tests: construction
// with fault checkpoints, a region's pinned task set in one submission, and the workers' thread ids.

#include <jaiscript/thread_pool.hpp>
#include <memory>
#include <thread>
#include <utility>

namespace jai::detail {

    struct thread_pool_access {
        static std::unique_ptr<thread_pool> make(size_t worker_count, const fault_injection* faults,
                                                 thread_pool::thread_speed speed = thread_pool::normal_threads);

        // A fork-join region's whole pinned task set in one lock and at most one wake; order within
        // a worker's queue matches the span's order. Moves each task out of the span.
        static void submit_pinned_batch(thread_pool& pool, std::pair<size_t, thread_pool::task_type>* tasks, size_t count,
                                        const fault_injection* faults = nullptr);

        // thread_pool::submit_batch with fault checkpoints
        static void submit_batch(thread_pool& pool, std::vector<thread_pool::task_type> tasks, thread_pool::task_type then,
                                 thread_pool::then_on where, const fault_injection* faults);

        // Every submit_to(worker, ...) task observes exactly this id from std::this_thread::get_id().
        static std::thread::id worker_thread_id(const thread_pool& pool, size_t worker);
    };

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_THREAD_POOL_ACCESS_HPP__
