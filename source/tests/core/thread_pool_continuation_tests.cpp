#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/thread_pool.hpp>
#include <jaiscript/detail/fault_injection.hpp>
#include <jaiscript/detail/thread_pool_access.hpp>
#include <atomic>
#include <string>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace jai::foundry::tests {

namespace thread_pool_continuation_detail {

// what must happen gets the test budget as a hang rail, never a speed bound
template<typename Condition, typename Pump>
bool wait_for(Condition condition, Pump pump, std::chrono::milliseconds timeout = std::chrono::milliseconds(static_cast<long long>(k_test_budget_seconds * 1000))) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) { return false; }
        pump();
        std::this_thread::yield();
    }
    return true;
}

template<typename Condition>
bool wait_for(Condition condition) {
    return wait_for(condition, [] {});
}

struct not_a_std_exception {};

} // namespace thread_pool_continuation_detail

class thread_pool_continuation_tests : public suite {
public:
    thread_pool_continuation_tests() : suite("thread_pool continuations") {}

    void forge_tests() override {
        using namespace thread_pool_continuation_detail;
        using then_on = thread_pool::then_on;

        test("a_task_continues_once_on_the_place_it_names", [this]() {
            for (auto where : {then_on::pool, then_on::posted}) {
                thread_pool pool(1);
                std::atomic<int> exceptions{0};
                pool.on_exception([&](std::exception_ptr) { ++exceptions; });
                std::atomic<int> ran{0};
                std::atomic<int> continued{0};
                std::thread::id ran_on;
                std::thread::id continued_on;
                pool.submit([&]() { ran_on = std::this_thread::get_id(); ++ran; }, [&]() {
                    continued_on = std::this_thread::get_id();
                    if (ran.load() == 1) { ++continued; }
                }, where);
                check(wait_for([&]() { return continued.load() == 1; }, [&]() { pool.run_posted(); }), "the continuation must run after the task");
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                pool.run_posted();
                check_eq(1, continued.load());
                const auto expected_thread = where == then_on::pool ? ran_on : std::this_thread::get_id();
                check(continued_on == expected_thread, "pool continues on the worker that ran the task, posted on the draining thread");
                check_eq(0, exceptions.load());
            }
        });

        test("a_batch_continues_once_after_its_last_task_and_the_tasks_are_gone", [this]() {
            for (auto where : {then_on::pool, then_on::posted}) {
                thread_pool pool(1);
                std::atomic<int> continued{0};
                auto capture = std::make_shared<int>(7);
                std::weak_ptr<int> capture_lifetime = capture;
                std::atomic<bool> tasks_released{false};
                std::vector<thread_pool::task_type> tasks;
                tasks.emplace_back([keep = capture]() {});
                capture.reset();
                pool.submit_batch(std::move(tasks), [&]() {
                    tasks_released = capture_lifetime.expired();
                    ++continued;
                }, where);
                check(wait_for([&]() { return continued.load() == 1; }, [&]() { pool.run_posted(); }), "the batch must continue");
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                pool.run_posted();
                check_eq(1, continued.load());
                check(tasks_released.load(), "the batch's tasks are released before it continues");
            }
        });

        test("a_large_batch_continues_exactly_once_after_every_task", [this]() {
            thread_pool pool(4);
            for (int round = 0; round < 200; ++round) {
                std::atomic<int> ran{0};
                std::atomic<int> saw_all{0};
                std::atomic<int> continued{0};
                std::vector<thread_pool::task_type> tasks;
                for (int i = 0; i < 16; ++i) {
                    tasks.emplace_back([&]() { ++ran; });
                }
                pool.submit_batch(std::move(tasks), [&]() {
                    if (ran.load() == 16) { ++saw_all; }
                    ++continued;
                });
                check(wait_for([&]() { return continued.load() >= 1; }), "the batch must continue");
                pool.wait_idle();
                check_eq(16, ran.load());
                check_eq(1, saw_all.load());
                check_eq(1, continued.load());
            }
        });

        test("normal_and_fast_thread_pools_both_run_their_tasks", [this]() {
            thread_pool normal{2};
            thread_pool fast{2, thread_pool::fast_threads};
            std::atomic<int> ran{0};
            for (int i = 0; i < 8; ++i) {
                normal.submit([&]() { ++ran; });
                fast.submit([&]() { ++ran; });
            }
            normal.wait_idle();
            fast.wait_idle();
            check_eq(16, ran.load());
        });

        test("an_empty_batch_continues_at_once", [this]() {
            thread_pool pool(1);
            std::atomic<int> on_pool{0};
            std::atomic<int> posted{0};
            pool.submit_batch({}, [&]() { ++on_pool; }, then_on::pool);
            pool.submit_batch({}, [&]() { ++posted; }, then_on::posted);
            pool.submit_batch({}, nullptr, then_on::pool);
            check(wait_for([&]() { return on_pool.load() == 1 && posted.load() == 1; }, [&]() { pool.run_posted(); }), "both continuations must run");
        });

        test("a_throwing_task_still_continues_and_reports", [this]() {
            for (auto where : {then_on::pool, then_on::posted}) {
                thread_pool pool(2);
                std::atomic<int> reported{0};
                pool.on_exception([&](std::exception_ptr) { ++reported; });
                std::atomic<int> continued{0};
                pool.submit([]() { throw std::runtime_error("task"); }, [&]() { ++continued; }, where);
                std::vector<thread_pool::task_type> tasks;
                tasks.emplace_back([]() { throw std::runtime_error("batch task"); });
                tasks.emplace_back([]() { throw not_a_std_exception{}; });
                tasks.emplace_back([]() {});
                pool.submit_batch(std::move(tasks), [&]() { ++continued; }, where);
                check(wait_for([&]() { return continued.load() == 2; }, [&]() { pool.run_posted(); }), "both continuations must run");
                pool.wait_idle();
                check_eq(3, reported.load());
            }
        });

        test("a_posted_task_that_drains_again_runs_nothing_twice", [this]() {
            thread_pool pool(1);
            std::vector<char> order;
            pool.post([&]() {
                order.push_back('a');
                pool.run_posted();
            });
            pool.post([&]() { order.push_back('b'); });
            check_eq(size_t{1}, pool.run_posted(), "the nested drain ran 'b'");
            check_eq(size_t{0}, pool.run_posted());
            check(order == std::vector<char>{'a', 'b'}, "each posted task runs once, in order");
        });

        test("a_task_posted_while_draining_runs_in_the_same_drain", [this]() {
            thread_pool pool(1);
            std::vector<int> order;
            pool.post([&]() {
                order.push_back(1);
                pool.post([&]() { order.push_back(3); });
            });
            pool.post([&]() { order.push_back(2); });
            pool.run_posted();
            check(order == std::vector<int>{1, 2, 3}, "FIFO, including work posted during the drain");
        });

        test("a_posted_std_exception_reports_and_the_drain_goes_on", [this]() {
            thread_pool pool(1);
            std::atomic<int> reported{0};
            pool.on_exception([&](std::exception_ptr) { ++reported; });
            std::vector<int> order;
            pool.post([&]() { order.push_back(1); throw std::runtime_error("posted"); });
            pool.post([&]() { order.push_back(2); });
            pool.run_posted();
            check_eq(1, reported.load());
            check(order == std::vector<int>{1, 2});
        });

        test("any_other_posted_exception_leaves_the_drain_and_keeps_the_rest_queued_first", [this]() {
            thread_pool pool(1);
            std::vector<int> order;
            pool.post([&]() { order.push_back(1); });
            pool.post([&]() {
                order.push_back(2);
                pool.post([&]() { order.push_back(5); });
                throw not_a_std_exception{};
            });
            pool.post([&]() { order.push_back(3); });
            pool.post([&]() { order.push_back(4); });
            bool escaped = false;
            try {
                pool.run_posted();
            } catch (const not_a_std_exception&) {
                escaped = true;
            }
            check(escaped, "a non-std exception leaves run_posted");
            check(order == std::vector<int>{1, 2});
            pool.run_posted();
            check(order == std::vector<int>{1, 2, 3, 4, 5}, "the unrun tasks run ahead of later posts");
        });

        test("posts_from_other_threads_while_draining_each_run_once_in_each_threads_order", [this]() {
            thread_pool pool(1);
            const int producers = 4;
            const int per_producer = 20000;
            std::vector<std::vector<int>> seen(producers);
            std::atomic<int> total{0};
            std::atomic<bool> go{false};
            std::vector<std::thread> threads;
            for (int p = 0; p < producers; ++p) {
                threads.emplace_back([&, p]() {
                    while (!go.load()) { std::this_thread::yield(); }
                    for (int i = 0; i < per_producer; ++i) {
                        pool.post([&, p, i]() {
                            seen[p].push_back(i);
                            ++total;
                        });
                    }
                });
            }
            go = true;
            const bool drained = wait_for([&]() { return total.load() == producers * per_producer; }, [&]() { pool.run_posted(); });
            for (auto& thread : threads) { thread.join(); }
            pool.run_posted();
            check(drained, "every posted task must run");
            check_eq(producers * per_producer, total.load());
            for (int p = 0; p < producers; ++p) {
                bool in_order = static_cast<int>(seen[p].size()) == per_producer;
                for (int i = 0; in_order && i < per_producer; ++i) {
                    in_order = seen[p][i] == i;
                }
                check(in_order, "one producer's posts run once each in its order");
            }
        });

        test("tasks_that_post_and_submit_more_work_under_load_lose_nothing", [this]() {
            thread_pool pool(4);
            const int tasks = 5000;
            std::atomic<int> posted{0};
            std::atomic<int> chained{0};
            for (int i = 0; i < tasks; ++i) {
                pool.submit([&]() {
                    pool.submit([&]() { ++chained; });
                }, [&]() { ++posted; }, then_on::posted);
            }
            check(wait_for([&]() { return posted.load() == tasks && chained.load() == tasks; }, [&]() { pool.run_posted(); }), "every continuation must run");
        });

        test("a_batch_that_fails_to_queue_runs_its_prefix_and_never_strands_wait_idle", [this]() {
            // A batch whose queueing throws partway runs the tasks it queued, drops its continuation and wakes the workers: with
            // every worker parked, the queued prefix used to sit unannounced and wait_idle never returned
            struct fail_at_second { static void hit(void*, const char* point, size_t index) {
                if (std::string(point) == "pool.shared_batch_enqueue" && index == 1) { throw std::runtime_error("enqueue failed"); }
            } };
            detail::fault_injection faults{ nullptr, &fail_at_second::hit };
            auto pool = detail::thread_pool_access::make(2, nullptr);
            pool->spin_budget_ns(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::atomic<int> ran{0};
            std::atomic<int> continued{0};
            std::vector<thread_pool::task_type> tasks;
            for (int i = 0; i < 4; ++i) { tasks.push_back([&ran] { ++ran; }); }
            std::string error = "no error";
            try {
                detail::thread_pool_access::submit_batch(*pool, std::move(tasks), [&continued] { ++continued; }, then_on::pool, &faults);
            } catch (const std::exception& ex) { error = ex.what(); }
            std::atomic<bool> idle{false};
            std::thread waiter([&] { pool->wait_idle(); idle = true; });
            const bool returned = wait_for([&] { return idle.load(); }, [] {});
            if (!returned) { pool->submit([] {}); }
            waiter.join();
            check_eq(std::string("enqueue failed"), error);
            check_true(returned, "wait_idle returns once the queued prefix ran");
            check_eq(1, ran.load(), "the task queued before the failure ran");
            check_eq(0, continued.load(), "a batch that failed to queue drops its continuation");
        });

        test("submit_to_an_index_past_the_workers_throws", [this]() {
            thread_pool pool(2);
            std::string error = "no error";
            try { pool.submit_to(2, [] {}); } catch (const std::out_of_range& ex) { error = ex.what(); }
            check_eq(std::string("thread_pool::submit_to: worker 2 is out of range (the pool has 2 workers)"), error);
        });

        test("a_nested_drain_continues_the_posted_queue_in_posting_order", [this]() {
            thread_pool pool(1);
            std::vector<char> order;
            pool.post([&] {
                order.push_back('a');
                pool.post([&] { order.push_back('c'); });
                pool.run_posted();
            });
            pool.post([&] { order.push_back('b'); });
            const size_t ran = pool.run_posted();
            check_eq(std::string("abc"), std::string(order.begin(), order.end()));
            check_eq(size_t(1), ran, "the outer drain counts the task it ran; the nested one ran the rest");
        });

        test("wait_idle_waits_for_a_task_whose_pinned_follow_up_finishes_first", [this]() {
            // A task's submit_to counts the follow-up before a spinning target can run and uncount it: otherwise the count passes
            // through zero while the submitting task still runs, and wait_idle returns early
            thread_pool pool(2);
            pool.spin_budget_ns(50'000'000);
            size_t early = 0;
            for (int round = 0; round < 20000; ++round) {
                std::atomic<bool> done{false};
                pool.submit_to(1, [&]() {
                    pool.submit_to(0, []() {});
                    for (volatile int spin = 0; spin < 200; ++spin) {}
                    done.store(true);
                });
                pool.wait_idle();
                if (!done.load()) {
                    ++early;
                    while (!done.load()) { std::this_thread::yield(); }
                    pool.wait_idle();
                }
            }
            check_eq(size_t(0), early, "wait_idle returned while the submitting task still ran");
        });

        test("destruction_finishes_an_in_flight_task_against_a_live_pool", [this]() {
            std::atomic<int> reported{0};
            std::atomic<bool> started{false};
            std::atomic<int> continued{0};
            {
                thread_pool pool(1);
                pool.on_exception([&](std::exception_ptr) { ++reported; });
                pool.submit([&]() {
                    started = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    pool.post([]() {});
                    pool.submit([]() {});
                    throw std::runtime_error("late failure");
                }, [&]() { ++continued; }, then_on::pool);
                check(wait_for([&]() { return started.load(); }), "the task must start");
            }
            check_eq(1, reported.load(), "the in-flight task's exception reaches the live handler");
            check_eq(1, continued.load(), "an in-flight task's pool continuation still runs");
        });

        test("destruction_drops_queued_batches_and_posts", [this]() {
            std::atomic<int> ran{0};
            std::atomic<int> continued{0};
            {
                thread_pool pool(2);
                std::vector<thread_pool::task_type> tasks;
                for (int i = 0; i < 1000; ++i) {
                    tasks.emplace_back([&]() {
                        ++ran;
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    });
                }
                pool.submit_batch(std::move(tasks), [&]() { ++continued; });
                pool.post([&]() { ++continued; });
            }
            check(ran.load() <= 1000);
            check_eq(ran.load() == 1000 ? 1 : 0, continued.load(), "a dropped batch never continues and the destructor runs no posts");
        });
    }
};

} // namespace jai::foundry::tests

using thread_pool_continuation_tests = jai::foundry::tests::thread_pool_continuation_tests;
FOUNDRY_REGISTER(thread_pool_continuation_tests)
