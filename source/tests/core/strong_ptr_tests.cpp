#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/strong_ptr.hpp>
#include <jaiscript/detail/fault_injection.hpp>
#include <jaiscript/detail/thread_pool_access.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <string_view>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <variant>

namespace jai::foundry::tests {

// Test class with destructor tracking
struct TestObject {
    static int instance_count;
    static int destructor_count;

    int value;
    std::string name;

    TestObject(int v = 0, std::string n = "")
        : value(v), name(std::move(n)) {
        ++instance_count;
    }

    ~TestObject() {
        ++destructor_count;
        --instance_count;
    }

    static void reset_counters() {
        instance_count = 0;
        destructor_count = 0;
    }
};

int TestObject::instance_count = 0;
int TestObject::destructor_count = 0;

// Derived class for inheritance tests
struct DerivedObject : TestObject {
    int extra;
    DerivedObject(int v, int e) : TestObject(v, "derived"), extra(e) {}
};

class strong_ptr_tests : public suite {
public:
    strong_ptr_tests() : suite("strong_ptr") {}

    void forge_tests() override {
        // ===== BASIC CONSTRUCTION =====

        test("make_strong_creates_valid_pointer", [this]() {
            TestObject::reset_counters();
            {
                auto ptr = make_strong<TestObject>(42, "test");
                check(ptr != nullptr);
                check_eq(ptr->value, 42);
                check_eq(ptr->name, std::string("test"));
                check_eq(ptr.use_count(), size_t(1));
                check_eq(TestObject::instance_count, 1);
            }
            check_eq(TestObject::instance_count, 0);
            check_eq(TestObject::destructor_count, 1);
        });

        test("default_constructor_creates_null", [this]() {
            strong_ptr<TestObject> ptr;
            check(ptr == nullptr);
            check(!ptr);
            check_eq(ptr.use_count(), size_t(0));
        });

        test("nullptr_constructor", [this]() {
            strong_ptr<TestObject> ptr = nullptr;
            check(ptr == nullptr);
            check(!ptr);
        });

        // ===== COPY SEMANTICS =====

        test("copy_constructor_increments_refcount", [this]() {
            TestObject::reset_counters();
            {
                auto ptr1 = make_strong<TestObject>(10);
                check_eq(ptr1.use_count(), size_t(1));

                auto ptr2 = ptr1;
                check_eq(ptr1.use_count(), size_t(2));
                check_eq(ptr2.use_count(), size_t(2));
                check(ptr1.get() == ptr2.get());
            }
            check_eq(TestObject::destructor_count, 1);
        });

        test("copy_assignment_works", [this]() {
            TestObject::reset_counters();
            {
                auto ptr1 = make_strong<TestObject>(1);
                auto ptr2 = make_strong<TestObject>(2);

                ptr2 = ptr1;
                check_eq(ptr1.use_count(), size_t(2));
                check_eq(ptr1->value, 1);
                check_eq(ptr2->value, 1);
            }
            // Object with value 2 should have been destroyed when ptr2 was reassigned
            check_eq(TestObject::destructor_count, 2);
        });

        test("self_assignment_is_safe", [this]() {
            auto ptr = make_strong<TestObject>(42);
            ptr = ptr;
            check_eq(ptr.use_count(), size_t(1));
            check_eq(ptr->value, 42);
        });

        // ===== MOVE SEMANTICS =====

        test("move_constructor_transfers_ownership", [this]() {
            TestObject::reset_counters();
            auto ptr1 = make_strong<TestObject>(99);
            auto ptr2 = std::move(ptr1);

            check(ptr1 == nullptr);
            check(ptr2 != nullptr);
            check_eq(ptr2->value, 99);
            check_eq(ptr2.use_count(), size_t(1));
        });

        test("move_assignment_transfers_ownership", [this]() {
            TestObject::reset_counters();
            auto ptr1 = make_strong<TestObject>(1);
            auto ptr2 = make_strong<TestObject>(2);

            ptr2 = std::move(ptr1);
            check(ptr1 == nullptr);
            check_eq(ptr2->value, 1);
            // Object with value 2 should have been destroyed
            check_eq(TestObject::destructor_count, 1);
        });

        // ===== WEAK_PTR TESTS =====

        test("weak_ptr_from_strong_ptr", [this]() {
            auto strong = make_strong<TestObject>(42);
            weaker_ptr<TestObject> weak = strong;

            check(!weak.expired());
            check_eq(weak.use_count(), size_t(1));
        });

        test("weak_ptr_lock_returns_strong_ptr", [this]() {
            auto strong = make_strong<TestObject>(42);
            weaker_ptr<TestObject> weak = strong;

            auto locked = weak.lock();
            check(locked != nullptr);
            check_eq(locked->value, 42);
            check_eq(strong.use_count(), size_t(2));
        });

        test("weak_ptr_expires_when_strong_dies", [this]() {
            weaker_ptr<TestObject> weak;
            {
                auto strong = make_strong<TestObject>(42);
                weak = strong;
                check(!weak.expired());
            }
            check(weak.expired());
            check_eq(weak.use_count(), size_t(0));

            auto locked = weak.lock();
            check(locked == nullptr);
        });

        test("weak_ptr_copy", [this]() {
            auto strong = make_strong<TestObject>(42);
            weaker_ptr<TestObject> weak1 = strong;
            weaker_ptr<TestObject> weak2 = weak1;

            check(!weak1.expired());
            check(!weak2.expired());
        });

        // ===== RESET AND UNIQUE =====

        test("reset_releases_reference", [this]() {
            TestObject::reset_counters();
            auto ptr = make_strong<TestObject>(42);
            check_eq(TestObject::instance_count, 1);

            ptr.reset();
            check(ptr == nullptr);
            check_eq(TestObject::instance_count, 0);
        });

        test("unique_returns_true_for_single_owner", [this]() {
            auto ptr = make_strong<TestObject>(42);
            check(ptr.unique());

            auto ptr2 = ptr;
            check(!ptr.unique());
            check(!ptr2.unique());
        });

        // ===== COMPARISON OPERATORS =====

        test("comparison_operators", [this]() {
            auto ptr1 = make_strong<TestObject>(1);
            auto ptr2 = make_strong<TestObject>(2);
            auto ptr3 = ptr1;

            check(ptr1 == ptr3);
            check(ptr1 != ptr2);
            check(ptr1 != nullptr);

            strong_ptr<TestObject> null;
            check(null == nullptr);
        });

        // ===== INHERITANCE =====

        test("derived_to_base_conversion", [this]() {
            // Single-pointer derivation forbids strong_ptr<Derived> -> strong_ptr<Base>
            // (would adjust the pointer off the control-block storage head and slice ~T()).
            static_assert(!std::is_constructible_v<strong_ptr<TestObject>, strong_ptr<DerivedObject>>);
            static_assert(!std::is_constructible_v<strong_ptr<TestObject>, strong_ptr<DerivedObject>&&>);

            auto derived = make_strong<DerivedObject>(10, 20);
            TestObject* base = derived.get();

            check_eq(base->value, 10);
            check_eq(derived.use_count(), size_t(1));
        });

        // ===== STRESS TEST =====

        test("many_copies_and_destroys", [this]() {
            TestObject::reset_counters();
            {
                std::vector<strong_ptr<TestObject>> ptrs;
                auto original = make_strong<TestObject>(42);

                for (int i = 0; i < 1000; ++i) {
                    ptrs.push_back(original);
                }
                check_eq(original.use_count(), size_t(1001));

                ptrs.clear();
                check_eq(original.use_count(), size_t(1));
            }
            check_eq(TestObject::destructor_count, 1);
        });

        // ===== VARIANT STORAGE TESTS =====
        // These simulate how script_value uses strong_ptr in a variant

        test("variant_move_assignment_destroys_old_value", [this]() {
            TestObject::reset_counters();
            using storage = std::variant<std::monostate, strong_ptr<TestObject>>;

            {
                storage s1 = make_strong<TestObject>(1);
                check_eq(TestObject::instance_count, 1);

                // Move-assign a new value - should destroy the old one
                s1 = make_strong<TestObject>(2);
                check_eq(TestObject::instance_count, 1);  // Old destroyed, new created
                check_eq(TestObject::destructor_count, 1);
            }
            check_eq(TestObject::destructor_count, 2);
        });

        test("variant_reset_to_monostate", [this]() {
            TestObject::reset_counters();
            using storage = std::variant<std::monostate, strong_ptr<TestObject>>;

            {
                storage s1 = make_strong<TestObject>(42);
                check_eq(TestObject::instance_count, 1);

                // Assign monostate - should destroy the strong_ptr
                s1 = std::monostate{};
                check_eq(TestObject::instance_count, 0);
                check_eq(TestObject::destructor_count, 1);
            }
        });

        test("variant_shared_ownership_and_move", [this]() {
            TestObject::reset_counters();
            using storage = std::variant<std::monostate, strong_ptr<TestObject>>;

            {
                auto ptr = make_strong<TestObject>(42);
                storage s1 = ptr;  // Copy into variant
                check_eq(ptr.use_count(), size_t(2));

                storage s2 = std::move(s1);  // Move variant
                check_eq(ptr.use_count(), size_t(2));  // Still 2 (s1 now holds monostate after move)

                // s1 should now be valueless or hold moved-from strong_ptr
                s1 = std::monostate{};  // Reset s1
                check_eq(ptr.use_count(), size_t(2));  // s2 and ptr
            }
            check_eq(TestObject::destructor_count, 1);
        });

        test("nested_destruction_with_weak_ptr", [this]() {
            // Simulate: object contains weak_ptr to itself (or parent)
            // This tests the weak_count=1 fix
            TestObject::reset_counters();

            struct Container {
                strong_ptr<TestObject> obj;
                weaker_ptr<TestObject> weak_to_obj;
            };

            {
                auto container = std::make_unique<Container>();
                container->obj = make_strong<TestObject>(42);
                container->weak_to_obj = container->obj;

                check_eq(container->obj.use_count(), size_t(1));
                check(!container->weak_to_obj.expired());
            }
            // Container destroyed - obj and weak_to_obj both go away
            check_eq(TestObject::destructor_count, 1);
        });

        test("pool_recycle_simulation", [this]() {
            // Simulate environment pool recycling:
            // 1. Store an object
            // 2. "Clear" local storage
            // 3. Reset this_object_ to null
            TestObject::reset_counters();
            using storage = std::variant<std::monostate, strong_ptr<TestObject>>;

            struct SimulatedEnv {
                std::vector<storage> local_storage;
                storage this_object;

                void clear_values() {
                    local_storage.clear();
                }

                void reset() {
                    clear_values();
                    this_object = std::monostate{};  // This is where the crash happens
                }
            };

            {
                SimulatedEnv env;

                // Simulate: create object and store in both local and this
                auto obj = make_strong<TestObject>(42);
                env.local_storage.push_back(obj);  // Copy into local storage
                env.this_object = obj;  // Copy into this_object

                check_eq(obj.use_count(), size_t(3));  // obj, local_storage, this_object

                // Simulate pool reset
                env.reset();

                // Object should still be alive because 'obj' holds a reference
                check_eq(obj.use_count(), size_t(1));
                check_eq(TestObject::instance_count, 1);
            }
            // Now obj goes out of scope
            check_eq(TestObject::destructor_count, 1);
        });
    }
};

namespace thread_pool_fault_detail {

struct injected_failure final : std::exception {
    const char* what() const noexcept override { return "Injected thread_pool failure"; }
};

struct injection {
    std::string_view point;
    size_t index = 0;
    size_t hits = 0;

    jai::detail::fault_injection hook() {
        return {this, [](void* context, const char* point, size_t index) {
            auto& self = *static_cast<injection*>(context);
            if (self.point == point && self.index == index) {
                ++self.hits;
                throw injected_failure{};
            }
        }};
    }
};

} // namespace thread_pool_fault_detail

class thread_pool_tests : public suite {
public:
    thread_pool_tests() : suite("thread_pool") {}

    void forge_tests() override {
        test("partial_batch_failure_wakes_accepted_tasks", [this]() {
            // Baseline lacks the exceptional wake. Let its worker park before the
            // injected partial enqueue, then observe a completion wait bounded by the test budget.
            // A rescue submission wakes/drains accepted work before assertions, so
            // this regression fails red without hanging or leaking captured storage.
            for (size_t fail_at : {size_t{1}, size_t{2}, size_t{3}}) {
                thread_pool pool(1);
                pool.submit([] {});
                pool.wait_idle();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

                std::atomic<size_t> completed{0};
                std::promise<void> accepted_done;
                auto done = accepted_done.get_future();
                auto storage = std::make_shared<int>(42);
                std::weak_ptr<int> storage_lifetime = storage;
                std::vector<std::pair<size_t, thread_pool::task_type>> batch;
                for (size_t i = 0; i < 4; ++i) {
                    batch.emplace_back(0, [&, keep_alive = storage]() mutable {
                        keep_alive.reset();
                        if (completed.fetch_add(1) + 1 == fail_at) { accepted_done.set_value(); }
                    });
                }
                thread_pool_fault_detail::injection injection{"pool.batch_enqueue", fail_at};
                auto faults = injection.hook();
                bool caught_original = false;
                try {
                    jai::detail::thread_pool_access::submit_pinned_batch(pool, batch.data(), batch.size(), &faults);
                } catch (const thread_pool_fault_detail::injected_failure& error) {
                    caught_original = std::string_view(error.what()) == "Injected thread_pool failure";
                }
                batch.clear();   // drop never-submitted captures before testing worker ownership
                storage.reset();
                const bool completed_without_rescue =
                    done.wait_for(std::chrono::duration<double>(k_test_budget_seconds)) == std::future_status::ready;
                if (!completed_without_rescue) {
                    pool.submit_to(0, [] {});   // known-good wake, even on the broken baseline
                }
                pool.wait_idle();
                check(caught_original, "submit must rethrow the original failure");
                check_eq(size_t{1}, injection.hits);
                check_eq(fail_at, completed.load(), "only the accepted prefix may execute");
                check(storage_lifetime.expired(), "no accepted task may retain region storage after completion");
                check(completed_without_rescue, "accepted prefix must wake without a later submission");

                std::atomic<bool> following_done{false};
                std::pair<size_t, thread_pool::task_type> following{0, [&]() { following_done = true; }};
                jai::detail::thread_pool_access::submit_pinned_batch(pool, &following, 1);
                pool.wait_idle();
                check(following_done.load(), "following batch must execute normally");
            }
        });

        test("first_batch_enqueue_failure_accepts_nothing", [this]() {
            thread_pool pool(1);
            std::atomic<int> completed{0};
            std::pair<size_t, thread_pool::task_type> batch{0, [&]() { ++completed; }};
            thread_pool_fault_detail::injection injection{"pool.batch_enqueue", 0};
            auto faults = injection.hook();
            bool caught_original = false;
            try {
                jai::detail::thread_pool_access::submit_pinned_batch(pool, &batch, 1, &faults);
            } catch (const thread_pool_fault_detail::injected_failure&) {
                caught_original = true;
            }
            pool.wait_idle();   // there was no accepted task to leave outstanding
            check(caught_original);
            check_eq(size_t{1}, injection.hits);
            check_eq(0, completed.load());
            pool.submit_to(0, [&]() { ++completed; });
            pool.wait_idle();
            check_eq(1, completed.load());
        });

        test("constructor_startup_failure_joins_partial_threads", [this]() {
            // RUN THIS TEST ALONE IN THE BASELINE EXECUTABLE: the unfixed constructor
            // destroys a joinable std::thread when thread_start index 1 throws, and
            // std::terminate kills that process. After the fix every site is catchable.
            const std::pair<const char*, size_t> cases[] = {
                {"pool.worker_storage", 0},
                {"pool.worker_create", 0}, {"pool.worker_create", 1}, {"pool.worker_create", 2},
                {"pool.thread_start", 0}, {"pool.thread_start", 1}, {"pool.thread_start", 2}
            };
            for (const auto& [point, index] : cases) {
                thread_pool_fault_detail::injection injection{point, index};
                auto faults = injection.hook();
                bool caught_original = false;
                try {
                    auto broken = jai::detail::thread_pool_access::make(3, &faults);
                } catch (const thread_pool_fault_detail::injected_failure& error) {
                    caught_original = std::string_view(error.what()) == "Injected thread_pool failure";
                }
                check(caught_original, "construction must rethrow the original failure after joining");
                check_eq(size_t{1}, injection.hits);
                thread_pool following(1);
                std::atomic<bool> completed{false};
                following.submit_to(0, [&]() { completed = true; });
                following.wait_idle();
                check(completed.load(), "a following pool must remain usable");
            }
        });

        test("construction_and_teardown", [this]() {
            thread_pool one(1);
            check_eq(one.worker_count(), size_t(1));
            thread_pool four(4);
            check_eq(four.worker_count(), size_t(4));
            thread_pool defaulted;
            check(defaulted.worker_count() >= 1);
        });  // all three destruct here with no submitted work

        test("n_tasks_complete", [this]() {
            thread_pool pool(4);
            std::atomic<int> completed{0};
            for (int i = 0; i < 200; ++i) {
                pool.submit([&completed] { ++completed; });
            }
            pool.wait_idle();
            check_eq(completed.load(), 200);
        });

        test("pinned_tasks_run_on_consistent_threads", [this]() {
            thread_pool pool(3);
            std::vector<std::vector<std::thread::id>> seen(pool.worker_count());
            for (int round = 0; round < 20; ++round) {
                for (size_t w = 0; w < pool.worker_count(); ++w) {
                    pool.submit_to(w, [&seen, w] { seen[w].push_back(std::this_thread::get_id()); });
                }
            }
            pool.wait_idle();
            for (size_t w = 0; w < pool.worker_count(); ++w) {
                check_eq(seen[w].size(), size_t(20));
                for (const auto& id : seen[w]) {
                    check(id == jai::detail::thread_pool_access::worker_thread_id(pool, w));
                }
            }
            // distinct workers really are distinct threads
            check(jai::detail::thread_pool_access::worker_thread_id(pool, 0) != jai::detail::thread_pool_access::worker_thread_id(pool, 1));
            check(jai::detail::thread_pool_access::worker_thread_id(pool, 1) != jai::detail::thread_pool_access::worker_thread_id(pool, 2));
        });

        test("pinned_tasks_preserve_submission_order", [this]() {
            thread_pool pool(2);
            std::vector<int> order;
            for (int i = 0; i < 50; ++i) {
                pool.submit_to(0, [&order, i] { order.push_back(i); });  // single consumer: no lock needed
            }
            pool.wait_idle();
            check_eq(order.size(), size_t(50));
            for (int i = 0; i < 50; ++i) { check_eq(order[size_t(i)], i); }
        });

        test("task_exceptions_do_not_kill_workers", [this]() {
            thread_pool pool(2);
            std::atomic<int> caught{0};
            std::atomic<int> completed{0};
            pool.on_exception([&caught](std::exception_ptr) { ++caught; });
            for (int i = 0; i < 10; ++i) {
                pool.submit([] { throw std::runtime_error("task boom"); });
            }
            pool.wait_idle();
            check_eq(caught.load(), 10);
            // both workers (and the pinned path) still alive and working afterwards
            for (int i = 0; i < 20; ++i) { pool.submit([&completed] { ++completed; }); }
            pool.submit_to(0, [&completed] { ++completed; });
            pool.submit_to(1, [&completed] { ++completed; });
            pool.wait_idle();
            check_eq(completed.load(), 22);
        });

        test("unhandled_task_exception_is_swallowed", [this]() {
            thread_pool pool(1);
            pool.submit([] { throw std::runtime_error("no handler installed"); });
            pool.wait_idle();
            std::atomic<bool> alive{false};
            pool.submit([&alive] { alive = true; });
            pool.wait_idle();
            check_true(alive.load());
        });

        test("clean_shutdown_with_pending_tasks", [this]() {
            std::atomic<int> started{0};
            {
                thread_pool pool(2);
                for (int i = 0; i < 100; ++i) {
                    pool.submit([&started] {
                        ++started;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    });
                    pool.submit_to(i % 2, [&started] {
                        ++started;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    });
                }
            }  // destructor: drops queued tasks, finishes in-flight ones, joins — must not hang
            check(started.load() <= 200);
        });
    }
};

} // namespace jai::foundry::tests

FOUNDRY_REGISTER(jai::foundry::tests::strong_ptr_tests)
FOUNDRY_REGISTER(jai::foundry::tests::thread_pool_tests)
