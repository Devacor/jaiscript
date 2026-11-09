#include "../../include/jaiscript/thread_pool.hpp"
#include "../../include/jaiscript/jaiscript_fwd.hpp"
#include "../../include/jaiscript/detail/fault_injection.hpp"
#include "../../include/jaiscript/detail/thread_pool_access.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <cstdint>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <fstream>
#include <set>
#endif

// Standard C++ (std::thread/mutex/condition_variable) apart from performance_core_count's platform queries and the
// fast_threads power-throttling opt-out, and no static state beyond that function's cached answer: every queue, lock and
// handler lives in the pool's state. Task exceptions go to on_exception (or are
// swallowed), never allowed to kill a worker.
//
// Two submission modes, because the parallel design's pinned worker engines must always run on their
// own thread: submit (shared queue, any worker) and submit_to (that
// worker's queue, only its thread). The posted queue has its own lock, so posting never contends with
// the workers.
//
// Wake protocol: a worker that just finished a task spins on its own flag for up to spin_budget before
// parking on the condition variable, and only when the pool's last two batches arrived within that
// budget (dense fork-join regions, a frame's worth of them); a pool whose regions come one per frame
// parks at once and burns nothing between them. The submitter wakes the kernel only for parked
// targets (the parked flags are read under the mutex the worker parks under), so no wake is lost.
// A worker takes its pinned tasks under its own queue's lock and counts a finished task down without
// the pool's mutex, so a region's workers never queue behind one another on it; a finished task pings
// the joiners only when one is parked (the count and the outstanding tasks are sequentially consistent:
// a parking joiner sees the count reach zero, or the worker sees the joiner and takes the mutex before
// notifying). Task order and placement never change.

namespace jai {

    namespace {
        [[noreturn]] JAI_NOINLINE void throw_worker_out_of_range(size_t worker, size_t workers) {
            throw std::out_of_range("thread_pool::submit_to: worker " + std::to_string(worker) + " is out of range (the pool has " +
                                    std::to_string(workers) + " workers)");
        }

        size_t platform_performance_cores() {
#if defined(_WIN32)
            DWORD length = 0;
            GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
            if (length == 0) { return 0; }
            std::vector<std::uint64_t> buffer(length / sizeof(std::uint64_t) + 1);
            const char* records = reinterpret_cast<const char*>(buffer.data());
            if (!GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &length)) { return 0; }
            // One record per physical core; the higher the efficiency class, the faster the core
            size_t result = 0;
            BYTE fastest = 0;
            for (DWORD offset = 0; offset < length;) {
                const auto& core = *reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(records + offset);
                if (core.Size == 0) { break; }
                if (core.Processor.EfficiencyClass > fastest) {
                    fastest = core.Processor.EfficiencyClass;
                    result = 0;
                }
                if (core.Processor.EfficiencyClass == fastest) { ++result; }
                offset += core.Size;
            }
            return result;
#elif defined(__APPLE__)
            int value = 0;
            size_t size = sizeof(value);
            return sysctlbyname("hw.perflevel0.physicalcpu", &value, &size, nullptr, 0) == 0 && value > 0 ? static_cast<size_t>(value) : 0;
#elif defined(__linux__)
            // Intel hybrid chips list their performance cores' logical CPUs here, every other machine all of its CPUs there, as
            // ranges ("0-11" or "0-7,16-19"); SMT siblings share a core, so the count is the distinct (package, core) pairs
            std::ifstream file("/sys/devices/cpu_core/cpus");
            if (!file) { file.open("/sys/devices/system/cpu/online"); }
            auto topology = [](long cpu, const char* field) {
                std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/" + field);
                long value = -1;
                return in >> value ? value : -1;
            };
            std::set<std::pair<long, long>> cores;
            long first = 0;
            while (file >> first) {
                long last = first;
                if (file.peek() == '-') {
                    file.get();
                    if (!(file >> last)) { return 0; }
                }
                if (first < 0 || last < first) { return 0; }
                for (long cpu = first; cpu <= last; ++cpu) {
                    const long core = topology(cpu, "core_id");
                    cores.emplace(core < 0 ? -1 : topology(cpu, "physical_package_id"), core < 0 ? cpu : core);   // no topology: each CPU its own
                }
                if (file.peek() != ',') { break; }
                file.get();
            }
            return cores.size();
#else
            return 0;
#endif
        }

        void opt_out_of_power_throttling() {
#if defined(_WIN32)
            THREAD_POWER_THROTTLING_STATE throttling {THREAD_POWER_THROTTLING_CURRENT_VERSION, THREAD_POWER_THROTTLING_EXECUTION_SPEED, 0};
            SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof(throttling));
#elif defined(__APPLE__)
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
        }

        // macOS starts a std::thread on 512 KB where Linux gives 8 MB, so a parallel body's native-depth guard would trip sixteen
        // times sooner there: an Apple worker asks for Linux's stack
        class worker_thread {
        public:
            template<class Body>
            void start(Body body) {
#if defined(__APPLE__)
                struct start_block {
                    Body body;
                    std::promise<std::thread::id> id;
                };
                auto block = std::make_unique<start_block>(start_block{std::move(body), {}});
                std::future<std::thread::id> id = block->id.get_future();
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setstacksize(&attr, size_t(8) << 20);
                const int error = pthread_create(&handle_, &attr, [](void* raw) -> void* {
                    std::unique_ptr<start_block> start(static_cast<start_block*>(raw));
                    start->id.set_value(std::this_thread::get_id());
                    start->body();
                    return nullptr;
                }, block.get());
                pthread_attr_destroy(&attr);
                if (error != 0) { throw std::system_error(error, std::generic_category(), "thread_pool: a worker thread did not start"); }
                block.release();
                started_ = true;
                id_ = id.get();
#else
                thread_ = std::thread(std::move(body));
                id_ = thread_.get_id();
#endif
            }

            std::thread::id get_id() const noexcept { return id_; }
            bool joinable() const noexcept {
#if defined(__APPLE__)
                return started_;
#else
                return thread_.joinable();
#endif
            }
            void join() {
#if defined(__APPLE__)
                pthread_join(handle_, nullptr);
                started_ = false;
#else
                thread_.join();
#endif
            }

        private:
#if defined(__APPLE__)
            pthread_t handle_{};
            bool started_ = false;
#else
            std::thread thread_;
#endif
            std::thread::id id_;
        };
    }

    struct thread_pool::state {
        // A worker's pinned queue: a ring that keeps its storage, so a region's submit allocates nothing once the queue has held a region
        struct task_ring {
            std::vector<task_type> slots;
            size_t head = 0;
            size_t count = 0;
            bool empty() const { return count == 0; }
            size_t size() const { return count; }
            void push_back(task_type&& task) {
                if (count == slots.size()) {
                    std::vector<task_type> grown(slots.empty() ? 8 : slots.size() * 2);
                    for (size_t i = 0; i < count; ++i) { grown[i] = std::move(slots[(head + i) & (slots.size() - 1)]); }
                    slots.swap(grown);
                    head = 0;
                }
                slots[(head + count) & (slots.size() - 1)] = std::move(task);
                ++count;
            }
            void pop_front(task_type& task) {
                task = std::move(slots[head]);
                slots[head] = nullptr;
                head = (head + 1) & (slots.size() - 1);
                --count;
            }
            void clear() {
                for (auto& slot : slots) { slot = nullptr; }
                head = 0;
                count = 0;
            }
        };

        struct worker {
            worker_thread thread;
            std::thread::id id;
            std::mutex pinned_mutex;             // guards pinned; a submitter holding mutex_ takes it inside
            task_ring pinned;
            std::atomic<bool> has_work{false};   // pinned queue non-empty: set under both locks, cleared under pinned_mutex
            bool parked = false;                 // waiting on work_cv_; read and written under mutex_
        };

        struct batch_state {
            batch_state(size_t count, task_type continuation, then_on continuation_place) :
                remaining(count), then(std::move(continuation)), where(continuation_place) {}
            std::atomic<size_t> remaining;
            task_type then;
            then_on where;
        };

        state(size_t worker_count, thread_speed speed, const detail::fault_injection* faults) {
            if (worker_count == 0) worker_count = default_worker_count();
            try {
                if (faults) { faults->checkpoint("pool.worker_storage"); }
                workers_.reserve(worker_count);
                for (size_t i = 0; i < worker_count; ++i) {
                    if (faults) { faults->checkpoint("pool.worker_create", i); }
                    workers_.push_back(std::make_unique<worker>());
                }
                for (size_t i = 0; i < worker_count; ++i) {
                    if (faults) { faults->checkpoint("pool.thread_start", i); }
                    workers_[i]->thread.start([this, i, speed] {
                        if (speed == fast_threads) { opt_out_of_power_throttling(); }
                        run_worker(i);
                    });
                    workers_[i]->id = workers_[i]->thread.get_id();
                }
            } catch (...) {
                stop_and_join();
                throw;
            }
        }

        state(const state&) = delete;
        state& operator=(const state&) = delete;

        ~state() {
            stop_and_join();
        }

        void submit(task_type task) {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (stop_) return;
                shared_.push_back(std::move(task));
                outstanding_.fetch_add(1, std::memory_order_relaxed);
                shared_pending_.store(true, std::memory_order_release);
                note_batch();
            }
            work_cv_.notify_one();
        }

        // Pinned queue: runs on worker `worker`'s thread, in submission order,
        // interleaved with any shared tasks that worker also picks up.
        void submit_to(size_t worker, task_type task) {
            if (worker >= workers_.size()) [[unlikely]] { throw_worker_out_of_range(worker, workers_.size()); }
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (stop_) return;
                // counted before a spinning target can run and uncount it (a pinned pop takes only the worker's lock)
                outstanding_.fetch_add(1, std::memory_order_relaxed);
                try {
                    push_pinned(*workers_[worker], std::move(task));
                } catch (...) {
                    if (outstanding_.fetch_sub(1) == 1) { idle_cv_.notify_all(); }
                    throw;
                }
                note_batch();
            }
            // Pinned work satisfies only one worker's wake predicate; notify_all so the
            // target cannot lose the wake to an uninterested sibling consuming notify_one.
            work_cv_.notify_all();
        }

        // The per-task submit_to costs a W-worker region W lock round-trips and W notify_alls (W x W wake
        // predicate evaluations). Spinning targets see their flag; the kernel is asked only when a target
        // is parked.
        void submit_pinned_batch(std::pair<size_t, task_type>* tasks, size_t count, const detail::fault_injection* faults) {
            // Declare before the lock: exceptional unwinding unlocks FIRST, then wakes
            // the accepted prefix. Otherwise its owner can wait forever trying to join.
            struct notify_on_exit {
                std::condition_variable& changed;
                bool wake = true;
                ~notify_on_exit() { if (wake) { changed.notify_all(); } }
            } notify{work_cv_};
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (stop_) { notify.wake = false; return; }
                bool any_parked = false;
                // counted before any task can run and finish; an enqueue that throws uncounts the rest
                size_t queued = 0;
                outstanding_.fetch_add(count, std::memory_order_relaxed);
                struct uncount_on_exit {
                    std::atomic<size_t>& outstanding;
                    std::condition_variable& idle;
                    const size_t& queued;
                    size_t count;
                    ~uncount_on_exit() {
                        if (queued != count && outstanding.fetch_sub(count - queued) == count - queued) { idle.notify_all(); }
                    }
                } uncounted{outstanding_, idle_cv_, queued, count};
                for (size_t i = 0; i < count; ++i) {
                    if (faults) { faults->checkpoint("pool.batch_enqueue", i); }
                    worker& w = *workers_[tasks[i].first];
                    push_pinned(w, std::move(tasks[i].second));
                    any_parked = any_parked || w.parked;
                    ++queued;
                }
                note_batch();
                notify.wake = any_parked;
            }
        }

        void submit(task_type task, task_type then, then_on where) {
            submit([this, task = std::move(task), then = std::move(then), where]() mutable {
                run_reporting(task);
                continue_with(std::move(then), where);
            });
        }

        void submit_batch(std::vector<task_type> tasks, task_type then, then_on where, const detail::fault_injection* faults = nullptr) {
            if (tasks.empty()) {
                if (then && where == then_on::posted) {
                    post(std::move(then));
                } else if (then) {
                    submit(std::move(then));
                }
                return;
            }
            auto batch = std::make_shared<batch_state>(tasks.size(), std::move(then), where);
            const size_t count = tasks.size();
            size_t queued = 0;
            try {
                std::lock_guard<std::mutex> guard(mutex_);
                if (stop_) return;
                for (auto& task : tasks) {
                    if (faults) { faults->checkpoint("pool.shared_batch_enqueue", queued); }
                    shared_.push_back([this, batch, task = std::move(task)]() mutable {
                        run_reporting(task);
                        task = nullptr;
                        if (batch->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                            continue_with(std::move(batch->then), batch->where);
                        }
                    });
                    outstanding_.fetch_add(1, std::memory_order_relaxed);
                    ++queued;
                }
                shared_pending_.store(true, std::memory_order_release);
                note_batch();
            } catch (...) {
                // A batch that fails to enqueue drops its continuation and wakes workers for the accepted prefix; until the
                // subtraction no accepted task can be the last, so none reads `then` before it is cleared

                if (queued != 0) {
                    batch->then = nullptr;
                    batch->remaining.fetch_sub(count - queued, std::memory_order_acq_rel);
                    shared_pending_.store(true, std::memory_order_release);
                    work_cv_.notify_all();
                }
                throw;
            }
            if (count >= workers_.size()) {
                work_cv_.notify_all();
            } else {
                for (size_t i = 0; i < count; ++i) { work_cv_.notify_one(); }
            }
        }

        void post(task_type task) {
            std::lock_guard<std::mutex> guard(posted_mutex_);
            posted_.push_back(std::move(task));
        }

        // One task off the queue at a time: a posted task that calls run_posted continues the same queue in posting order,
        // and a task's non-std exception leaves the rest queued
        size_t run_posted() {
            size_t result = 0;
            for (;;) {
                task_type task;
                {
                    std::lock_guard<std::mutex> guard(posted_mutex_);
                    if (posted_.empty()) { return result; }
                    task = std::move(posted_.front());
                    posted_.pop_front();
                }
                ++result;
                try {
                    task();
                } catch (const std::exception&) {
                    dispatch_exception(std::current_exception());
                }
            }
        }

        void on_exception(std::function<void(std::exception_ptr)> handler) {
            std::lock_guard<std::mutex> guard(mutex_);
            exception_handler_ = std::move(handler);
        }

        void wait_idle() {
            if (spin_allowed()) {
                const int64_t budget = spin_budget_ns_.load(std::memory_order_relaxed);
                const auto t0 = std::chrono::steady_clock::now();
                while (outstanding_.load(std::memory_order_acquire) != 0) {
                    if (elapsed_ns(t0) > budget) { break; }
                    cpu_relax();
                }
            }
            if (outstanding_.load(std::memory_order_acquire) == 0) { return; }
            std::unique_lock<std::mutex> guard(mutex_);
            joiners_parked_.fetch_add(1);
            idle_cv_.wait(guard, [this] { return outstanding_.load() == 0; });
            joiners_parked_.fetch_sub(1);
        }

        // Under mutex_ (the parked flag the caller reads next stays consistent with has_work)
        static void push_pinned(worker& w, task_type&& task) {
            std::lock_guard<std::mutex> guard(w.pinned_mutex);
            w.pinned.push_back(std::move(task));
            w.has_work.store(true, std::memory_order_release);
        }
        bool pop_pinned(worker& me, task_type& task) {
            std::lock_guard<std::mutex> guard(me.pinned_mutex);
            if (me.pinned.empty() || stop_flag_.load(std::memory_order_acquire)) { return false; }
            me.pinned.pop_front(task);
            if (me.pinned.empty()) { me.has_work.store(false, std::memory_order_relaxed); }
            return true;
        }

        void run_reporting(task_type& task) noexcept {
            try {
                task();
            } catch (...) {
                dispatch_exception(std::current_exception());
            }
        }

        // Called on the worker that finished the work: a pool continuation runs right there.
        void continue_with(task_type then, then_on where) {
            if (!then) { return; }
            if (where == then_on::posted) {
                post(std::move(then));
            } else {
                run_reporting(then);
            }
        }

        static void cpu_relax() noexcept {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }
        static int64_t elapsed_ns(std::chrono::steady_clock::time_point t0) noexcept {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        }
        // Under mutex_: the gap between this batch and the previous one decides whether the
        // workers spin for the next
        void note_batch() noexcept {
            const auto now = std::chrono::steady_clock::now();
            if (last_batch_.time_since_epoch().count() != 0) {
                last_gap_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_batch_).count(), std::memory_order_relaxed);
            }
            last_batch_ = now;
        }
        bool spin_allowed() const noexcept {
            const int64_t budget = spin_budget_ns_.load(std::memory_order_relaxed);
            return budget > 0 && last_gap_ns_.load(std::memory_order_relaxed) <= budget;
        }

        // Shared by normal destruction and failed construction. All worker storage
        // stays alive until every thread started so far has observed stop and joined.
        void stop_and_join() noexcept {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                stop_ = true;
                stop_flag_.store(true, std::memory_order_release);
                outstanding_.fetch_sub(shared_.size(), std::memory_order_relaxed);
                shared_.clear();
                for (auto& w : workers_) {
                    std::lock_guard<std::mutex> pinned_guard(w->pinned_mutex);
                    outstanding_.fetch_sub(w->pinned.size(), std::memory_order_relaxed);
                    w->pinned.clear();
                }
            }
            work_cv_.notify_all();
            idle_cv_.notify_all();
            for (auto& w : workers_) {
                if (w->thread.joinable()) w->thread.join();
            }
        }

        void run_worker(size_t index) {
            worker& me = *workers_[index];
            for (;;) {
                task_type task;
                if (!me.has_work.load(std::memory_order_acquire) || !pop_pinned(me, task)) {
                    std::unique_lock<std::mutex> guard(mutex_);
                    if (!stop_ && !me.has_work.load(std::memory_order_relaxed) && shared_.empty() && spin_allowed()) {
                        guard.unlock();
                        const int64_t budget = spin_budget_ns_.load(std::memory_order_relaxed);
                        const auto t0 = std::chrono::steady_clock::now();
                        while (!me.has_work.load(std::memory_order_acquire) &&
                               !shared_pending_.load(std::memory_order_acquire) &&
                               !stop_flag_.load(std::memory_order_acquire)) {
                            if (elapsed_ns(t0) > budget) { break; }
                            cpu_relax();
                        }
                        if (me.has_work.load(std::memory_order_acquire)) { continue; }
                        guard.lock();
                    }
                    while (!stop_ && !me.has_work.load(std::memory_order_relaxed) && shared_.empty()) {
                        me.parked = true;
                        work_cv_.wait(guard);
                        me.parked = false;
                    }
                    if (stop_) return;
                    if (me.has_work.load(std::memory_order_relaxed)) { continue; }   // its own queue, under its own lock
                    task = std::move(shared_.front());
                    shared_.pop_front();
                    if (shared_.empty()) { shared_pending_.store(false, std::memory_order_relaxed); }
                }
                try {
                    task();
                } catch (...) {
                    dispatch_exception(std::current_exception());
                }
                task = nullptr;   // the task's captures die before it counts as finished
                outstanding_.fetch_sub(1);
                if (joiners_parked_.load() != 0) {
                    { std::lock_guard<std::mutex> guard(mutex_); }
                    idle_cv_.notify_all();
                }
            }
        }

        void dispatch_exception(std::exception_ptr e) noexcept {
            std::function<void(std::exception_ptr)> handler;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                handler = exception_handler_;
            }
            if (handler) {
                try { handler(e); } catch (...) {}  // a throwing handler must not kill the worker either
            }
        }

        std::mutex mutex_;
        std::condition_variable work_cv_;
        std::condition_variable idle_cv_;
        std::vector<std::unique_ptr<worker>> workers_;
        std::deque<task_type> shared_;
        std::function<void(std::exception_ptr)> exception_handler_;
        std::mutex posted_mutex_;
        std::deque<task_type> posted_;
        std::atomic<size_t> outstanding_{0};       // raised under mutex_, counted down by the worker that finished
        std::atomic<bool> shared_pending_{false};
        std::atomic<bool> stop_flag_{false};
        std::atomic<int64_t> spin_budget_ns_{1000000};   // 1 ms
        std::atomic<int64_t> last_gap_ns_{INT64_MAX};
        std::chrono::steady_clock::time_point last_batch_{};
        std::atomic<size_t> joiners_parked_{0};    // changed under mutex_; a count: a waiter that returns never hides one still parked
        bool stop_ = false;
    };

    size_t thread_pool::performance_core_count() noexcept {
        static const size_t result = [] {
            const size_t hardware = std::max<size_t>(1, std::thread::hardware_concurrency());
            const size_t detected = platform_performance_cores();
            return detected == 0 ? hardware : std::min(detected, hardware);
        }();
        return result;
    }

    size_t thread_pool::default_worker_count() noexcept {
        const unsigned hw = std::thread::hardware_concurrency();
        return hw > 1 ? hw - 1 : 1;
    }

    thread_pool::thread_pool(size_t worker_count, thread_speed speed) : thread_pool(worker_count, speed, nullptr) {}

    thread_pool::thread_pool(size_t worker_count, thread_speed speed, const detail::fault_injection* faults) :
        state_(std::make_unique<state>(worker_count, speed, faults)) {}

    // joined while state_ still points at the state: a task running during destruction may submit or post (libc++'s unique_ptr
    // nulls itself before it deletes)
    thread_pool::~thread_pool() { state_->stop_and_join(); }

    size_t thread_pool::worker_count() const noexcept { return state_->workers_.size(); }

    void thread_pool::spin_budget_ns(int64_t nanoseconds) noexcept { state_->spin_budget_ns_.store(nanoseconds, std::memory_order_relaxed); }
    int64_t thread_pool::spin_budget_ns() const noexcept { return state_->spin_budget_ns_.load(std::memory_order_relaxed); }

    void thread_pool::submit(task_type task) { state_->submit(std::move(task)); }
    void thread_pool::submit(task_type task, task_type then, then_on where) { state_->submit(std::move(task), std::move(then), where); }
    void thread_pool::submit_to(size_t worker, task_type task) { state_->submit_to(worker, std::move(task)); }
    void thread_pool::submit_batch(std::vector<task_type> tasks, task_type then, then_on where) { state_->submit_batch(std::move(tasks), std::move(then), where); }
    void thread_pool::post(task_type task) { state_->post(std::move(task)); }
    size_t thread_pool::run_posted() { return state_->run_posted(); }
    void thread_pool::on_exception(std::function<void(std::exception_ptr)> handler) { state_->on_exception(std::move(handler)); }
    void thread_pool::wait_idle() { state_->wait_idle(); }

    namespace detail {

        std::unique_ptr<thread_pool> thread_pool_access::make(size_t worker_count, const fault_injection* faults, thread_pool::thread_speed speed) {
            return std::unique_ptr<thread_pool>(new thread_pool(worker_count, speed, faults));
        }

        void thread_pool_access::submit_pinned_batch(thread_pool& pool, std::pair<size_t, thread_pool::task_type>* tasks, size_t count,
                                                     const fault_injection* faults) {
            pool.state_->submit_pinned_batch(tasks, count, faults);
        }

        void thread_pool_access::submit_batch(thread_pool& pool, std::vector<thread_pool::task_type> tasks, thread_pool::task_type then,
                                              thread_pool::then_on where, const fault_injection* faults) {
            pool.state_->submit_batch(std::move(tasks), std::move(then), where, faults);
        }

        std::thread::id thread_pool_access::worker_thread_id(const thread_pool& pool, size_t worker) {
            return pool.state_->workers_[worker]->id;
        }

    } // namespace detail

} // namespace jai
