#pragma once

#ifndef __JAISCRIPT_THREAD_POOL_HPP__
#define __JAISCRIPT_THREAD_POOL_HPP__

// A persistent worker pool for hosts; each engine's parallel regions run on one of their own.
// The workers, queues and wake protocol live in source/implementation/thread_pool.cpp, so this header
// costs a consumer std::function and std::vector and nothing else.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <vector>

namespace jai {

    namespace detail {
        struct fault_injection;
        struct thread_pool_access;
    }

    class thread_pool {
    public:
        using task_type = std::function<void()>;

        // Where a continuation runs: on the pool worker that finished the work, or posted for
        // the thread that calls run_posted().
        enum class then_on { pool, posted };

        // fast_threads asks the OS never to throttle the workers to save power (EcoQoS on Windows,
        // user-initiated QoS on Apple platforms), for work that waits on its slowest worker.
        enum thread_speed { normal_threads, fast_threads };

        // One less than the hardware threads (the submitting thread is the other), at least 1.
        static size_t default_worker_count() noexcept;
        // The cores of the fastest class the platform reports (a hybrid CPU's performance cores), else
        // hardware_concurrency; between 1 and hardware_concurrency, detected on the first call.
        static size_t performance_core_count() noexcept;

        // 0 workers means default_worker_count().
        explicit thread_pool(size_t worker_count = 0, thread_speed speed = normal_threads);
        thread_pool(const thread_pool&) = delete;
        thread_pool& operator=(const thread_pool&) = delete;
        // Drops queued tasks that have not started, finishes the ones running, and joins every worker.
        ~thread_pool();

        size_t worker_count() const noexcept;

        // How long a worker (and wait_idle) spins for the next batch before parking, when batches
        // arrive at least that densely. 0 parks at once.
        void spin_budget_ns(int64_t nanoseconds) noexcept;
        int64_t spin_budget_ns() const noexcept;

        // Shared queue: the first free worker runs it.
        void submit(task_type task);
        // `then` runs once after `task` returns or throws (the exception goes to on_exception).
        void submit(task_type task, task_type then, then_on where = then_on::pool);
        // Runs on worker `worker`'s thread only, in submission order.
        void submit_to(size_t worker, task_type task);
        // The tasks go on the shared queue under one lock; `then` runs once, after the last of them
        // returns or throws. An empty batch continues at once.
        void submit_batch(std::vector<task_type> tasks, task_type then, then_on where = then_on::pool);

        // Queues `task` for the thread that drains run_posted(); any thread may post.
        void post(task_type task);
        // Runs every posted task in order, including tasks posted while it runs, and returns how
        // many ran. A std::exception goes to on_exception; any other exception leaves with the
        // tasks not yet run queued ahead of later posts. Safe to call from inside a posted task.
        size_t run_posted();

        // Receives every exception a task lets escape; the worker stays alive either way.
        void on_exception(std::function<void(std::exception_ptr)> handler);

        // Blocks until every submitted task has finished.
        void wait_idle();

    private:
        friend struct detail::thread_pool_access;
        struct state;

        thread_pool(size_t worker_count, thread_speed speed, const detail::fault_injection* faults);

        std::unique_ptr<state> state_;
    };

} // namespace jai

#endif // __JAISCRIPT_THREAD_POOL_HPP__
