#include <jaiscript/core/script_run.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/engine_time.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/detail/execution_limits.hpp>
#include <jaiscript/detail/run_record.hpp>
#include <jaiscript/detail/native_stack.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <new>

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
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

// The vendored fiber library: its symbols stay in this unit
#define MINICORO_IMPL
#define MCO_API static
#define MCO_NO_DEFAULT_ALLOCATOR
#define MCO_NO_DEBUG
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#include "../third_party/minicoro/minicoro.h"
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace jai {

namespace detail {

    struct run_fiber {
        mco_coro* co = nullptr;
        void* block = nullptr;
        size_t block_size = 0;
        run_record* job = nullptr;
    };

} // namespace detail

namespace {

    size_t memory_page_size() noexcept {
#if defined(_WIN32)
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return info.dwPageSize;
#else
        const long page = sysconf(_SC_PAGESIZE);
        return page > 0 ? static_cast<size_t>(page) : 4096;
#endif
    }

    size_t round_up(size_t value, size_t to) noexcept { return (value + to - 1) / to * to; }

    // A fiber reserves what the creating thread's stack reserves, so the native-stack guard trips at the depth it does there
    size_t creating_thread_stack_reserve() noexcept {
        size_t result = size_t(8) << 20;
#if defined(_WIN32)
        ULONG_PTR low = 0, high = 0;
        GetCurrentThreadStackLimits(&low, &high);
        if (high > low) { result = static_cast<size_t>(high - low); }
#elif defined(__APPLE__)
        result = pthread_get_stacksize_np(pthread_self());
#elif defined(__linux__)
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            size_t size = 0;
            if (pthread_attr_getstacksize(&attr, &size) == 0 && size != 0) { result = size; }
            pthread_attr_destroy(&attr);
        }
#endif
        // glibc reports an unlimited RLIMIT_STACK as the gap below the main thread's stack (terabytes, varying with ASLR): too large to map
        return std::clamp<size_t>(result, MCO_MIN_STACK_SIZE * 4, size_t(1) << 30);
    }

    void run_body(detail::run_record& run) noexcept;

    void run_fiber_main(mco_coro* co) {
        auto* fiber = static_cast<detail::run_fiber*>(mco_get_user_data(co));
        for (;;) {
            run_body(*fiber->job);
            fiber->job->finished = true;
            mco_yield(co);
        }
    }

    // The block holds the fiber's header and then its stack, which grows down from the top: reserved whole, committed as it grows
    // behind a guard page, so an idle fiber holds the pages its deepest run touched and nothing more
    detail::run_fiber* create_run_fiber() {
        auto fiber = std::make_unique<detail::run_fiber>();
        mco_desc desc = mco_desc_init(&run_fiber_main, creating_thread_stack_reserve());
        desc.user_data = fiber.get();
        const size_t page = memory_page_size();
        const size_t total = round_up(desc.coro_size, page);
#if defined(_WIN32) && defined(MCO_USE_ASM)
        char* base = static_cast<char*>(VirtualAlloc(nullptr, total, MEM_RESERVE, PAGE_READWRITE));
        if (!base) { throw std::bad_alloc(); }
        const size_t header = round_up(desc.coro_size - desc.stack_size, page);
        const size_t initial = std::min<size_t>(round_up(64 * 1024, page), total - header - 2 * page);
        char* committed = base + total - initial;
        if (!VirtualAlloc(base, header, MEM_COMMIT, PAGE_READWRITE) || !VirtualAlloc(committed, initial, MEM_COMMIT, PAGE_READWRITE) ||
            !VirtualAlloc(committed - page, page, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD)) {
            VirtualFree(base, 0, MEM_RELEASE);
            throw std::bad_alloc();
        }
#elif !defined(_WIN32) && (defined(MCO_USE_ASM) || defined(MCO_USE_UCONTEXT))
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
        void* mapped = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (mapped == MAP_FAILED) { throw std::bad_alloc(); }
        char* base = static_cast<char*>(mapped);
#else
        char* base = static_cast<char*>(std::calloc(1, total));
        if (!base) { throw std::bad_alloc(); }
#endif
        fiber->block = base;
        fiber->block_size = total;
        fiber->co = reinterpret_cast<mco_coro*>(base);
        if (mco_init(fiber->co, &desc) != MCO_SUCCESS) {
            fiber->co = nullptr;
            detail::destroy_run_fiber(fiber.release());
            throw std::bad_alloc();
        }
#if defined(_WIN32) && defined(MCO_USE_ASM)
        // the thread's stack fields while the fiber runs: the OS grows the stack through the guard page as a thread's
        auto* context = static_cast<_mco_context*>(fiber->co->context);
        context->ctx.stack_limit = committed;
        context->ctx.dealloc_stack = base;
#elif !defined(_WIN32) && (defined(MCO_USE_ASM) || defined(MCO_USE_UCONTEXT))
        const uintptr_t low = (reinterpret_cast<uintptr_t>(fiber->co->stack_base) + page - 1) / page * page;
        mprotect(reinterpret_cast<void*>(low), page, PROT_NONE);
#endif
        return fiber.release();
    }

    detail::run_fiber* acquire_fiber(detail::run_registry& runs) {
        if (!runs.idle_fibers.empty()) {
            detail::run_fiber* fiber = runs.idle_fibers.back();
            runs.idle_fibers.pop_back();
            return fiber;
        }
        return create_run_fiber();
    }

    checked_result<script_value> run_not_finished() {
        return checked_result<script_value>(make_error_code(runtime_error_code::evaluation_failed), "The run has not finished");
    }

    void run_body(detail::run_record& run) noexcept {
        engine& eng = *run.eng;
        try {
            switch (run.kind) {
                case detail::run_record::body_kind::source: run.result.emplace(eng.execute(run.text)); break;
                case detail::run_record::body_kind::file: run.result.emplace(eng.execute_file(run.text)); break;
                case detail::run_record::body_kind::file_source: run.result.emplace(eng.execute_file_source(run.text, run.content)); break;
                case detail::run_record::body_kind::bite: run.result.emplace(eng.execute(*run.bite)); break;
                case detail::run_record::body_kind::callable: run.result.emplace(run.callable->deref().as_function()(run.args)); break;
            }
            run.state = *run.result ? detail::run_record::phase::completed : detail::run_record::phase::failed;
        } catch (const std::exception& e) {
            run.thrown = std::current_exception();
            run.result.emplace(make_error_code(runtime_error_code::evaluation_failed), "{0}", detail::engine_access::park_error_text(eng, e.what()));
            run.state = detail::run_record::phase::failed;
        } catch (...) {
            run.thrown = std::current_exception();
            run.result.emplace(make_error_code(runtime_error_code::evaluation_failed), "The run raised a non-standard C++ exception");
            run.state = detail::run_record::phase::failed;
        }
    }

    // One switch into the run and back: the innermost run becomes this one, its engine and backend state replace the live state
    // (and come back out after), and a resume is a fresh host entry with its own budget
    void switch_into(detail::run_record& run, bool resumed) {
        engine& eng = *run.eng;
        detail::run_registry& runs = detail::engine_access::run_registry(eng);
        execution_backend* backend = detail::engine_access::get_execution_backend(eng);
        run.outer = runs.active;
        runs.active = &run;
        detail::engine_access::active_run_pins(eng, &run.pins);
        detail::engine_access::swap_run_entry_state(eng, run.entry);
        backend->swap_run_state(*run.backend_state);
        if (resumed) { backend->begin_host_call(); }
        {
            detail::engine_access::debug_execution_scope executing(eng);
            mco_resume(run.fiber->co);
        }
        detail::engine_access::swap_run_entry_state(eng, run.entry);
        backend->swap_run_state(*run.backend_state);
        runs.active = run.outer;
        detail::engine_access::active_run_pins(eng, run.outer ? &run.outer->pins : nullptr);
        run.outer = nullptr;
    }

    // After a switch: a finished run hands its fiber and state back to the pools; a source run's C++ failure leaves through the
    // host's call, as execute() would throw it
    void settle(detail::run_record& run, bool rethrow) {
        engine& eng = *run.eng;
        detail::run_registry& runs = detail::engine_access::run_registry(eng);
        auto suspended = std::find(runs.suspended.begin(), runs.suspended.end(), &run);
        if (!run.finished) {
            if (suspended == runs.suspended.end()) { runs.suspended.push_back(&run); }
            return;
        }
        if (suspended != runs.suspended.end()) { runs.suspended.erase(suspended); }
        run.fiber->job = nullptr;
        runs.idle_fibers.push_back(run.fiber);
        run.fiber = nullptr;
        execution_backend* backend = detail::engine_access::get_execution_backend(eng);
        if (run.state == detail::run_record::phase::failed) { backend->adopt_run_trace(*run.backend_state); }
        backend->reset_run_state(*run.backend_state);
        runs.spare_states.push_back(std::move(run.backend_state));
        run.callable.reset();
        run.args.clear();
        run.entry.deferred_post_loads.clear();
        if (rethrow && run.thrown) {
            std::exception_ptr thrown = std::move(run.thrown);
            run.thrown = nullptr;
            std::rethrow_exception(thrown);
        }
        run.thrown = nullptr;
    }

    void cancel_run(detail::run_record& run) {
        if (run.state != detail::run_record::phase::suspended) { return; }
        run.cancel = true;
        switch_into(run, true);
        settle(run, false);
    }

} // namespace

bool detail::running_fiber_stack(std::uintptr_t& a_low, std::uintptr_t& a_high) noexcept {
    mco_coro* co = mco_running();
    if (!co || !co->stack_base || co->stack_size == 0) { return false; }
    a_low = reinterpret_cast<std::uintptr_t>(co->stack_base);
    a_high = a_low + co->stack_size;
    return true;
}

void detail::destroy_run_fiber(run_fiber* fiber) noexcept {
    if (!fiber) { return; }
    if (fiber->co) { mco_uninit(fiber->co); }
    if (fiber->block) {
#if defined(_WIN32) && defined(MCO_USE_ASM)
        VirtualFree(fiber->block, 0, MEM_RELEASE);
#elif !defined(_WIN32) && (defined(MCO_USE_ASM) || defined(MCO_USE_UCONTEXT))
        munmap(fiber->block, fiber->block_size);
#else
        std::free(fiber->block);
#endif
    }
    delete fiber;
}

// ---- engine side ----

checked_result<script_value> engine::sleep_builtin(const script_value& a_seconds) {
    const script_value& v = a_seconds.deref();
    double seconds = 0.0;
    if (v.is_int()) { seconds = static_cast<double>(v.unchecked_as_int()); }
    else if (v.is_float()) { seconds = v.unchecked_as_float(); }
    else { return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), detail::k_sleep_expects_number); }
    detail::run_record* run = run_registry().active;
    if (!(seconds > 0.0)) {   // no wait: the debugger's sync point, never a suspension
        debug_sync_point();
        return script_value(std::monostate{}, this);
    }
    const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(std::min(seconds, 1e9)));
    if (!run || parallel_region_active()) {
        time().sleep_for(duration);
        return script_value(std::monostate{}, this);
    }
    if (run->pins > 0) {
        if (!run->warned) {
            run->warned = true;
            report_script_warning(std::string(detail::k_sleep_pinned_warning));
        }
        time().sleep_for(duration);
        return script_value(std::monostate{}, this);
    }
    if (run->cancel) {   // a cancelled run that reaches another sleep ends there too
        execution_limits().terminal_error = true;
        return checked_result<script_value>(make_error_code(runtime_error_code::run_cancelled), detail::k_run_cancelled);
    }
    const std::chrono::nanoseconds now = time().monotonic_now();
    run->sleep_started = now;
    run->wake = now + duration;
    run->state = detail::run_record::phase::suspended;
    mco_yield(run->fiber->co);
    run->state = detail::run_record::phase::running;
    if (run->cancel) {
        execution_limits().terminal_error = true;
        return checked_result<script_value>(make_error_code(runtime_error_code::run_cancelled), detail::k_run_cancelled);
    }
    return script_value(std::monostate{}, this);
}

namespace {

    std::unique_ptr<detail::run_record> prepare_run(engine& eng, detail::run_record::body_kind kind) {
        detail::run_registry& runs = detail::engine_access::run_registry(eng);
        std::unique_ptr<detail::run_record> run;
        if (!runs.spare_records.empty()) {
            run = std::move(runs.spare_records.back());
            runs.spare_records.pop_back();
        } else {
            run = std::make_unique<detail::run_record>();
        }
        run->eng = &eng;
        run->alive = detail::engine_access::lifetime_token(eng);
        run->kind = kind;
        run->bite = nullptr;
        run->state = detail::run_record::phase::running;
        run->result.emplace(run_not_finished());
        run->thrown = nullptr;
        run->sleep_started = run->wake = std::chrono::nanoseconds{};
        run->thread = std::this_thread::get_id();
        run->pins = 0;
        run->warned = run->cancel = run->finished = false;
        run->outer = nullptr;
        run->entry = detail::run_entry_state{};
        execution_backend* backend = detail::engine_access::get_execution_backend(eng);
        if (!runs.spare_states.empty()) {
            run->backend_state = std::move(runs.spare_states.back());
            runs.spare_states.pop_back();
        } else {
            run->backend_state = backend->make_run_state();
        }
        return run;
    }

    void recycle_run(std::unique_ptr<detail::run_record> run) {
        if (!run || run->alive.expired() || !run->eng) { return; }
        detail::run_registry& runs = detail::engine_access::run_registry(*run->eng);
        run->result.reset();
        run->callable.reset();
        run->args.clear();
        if (runs.spare_records.size() < 8) { runs.spare_records.push_back(std::move(run)); }
    }

} // namespace

void engine::cancel_suspended_runs() noexcept {
    detail::run_registry& runs = run_registry();
    while (!runs.suspended.empty()) {
        detail::run_record* run = runs.suspended.back();
        try { cancel_run(*run); } catch (...) {}
        if (!runs.suspended.empty() && runs.suspended.back() == run) { runs.suspended.pop_back(); }
    }
}

// The first switch: the run goes as far as its first sleep or its end before the host holds it
script_run engine::launch_run(std::unique_ptr<detail::run_record> run) {
    detail::run_registry& runs = run_registry();
    try {
        run->fiber = acquire_fiber(runs);
    } catch (...) {
        runs.spare_states.push_back(std::move(run->backend_state));
        throw;
    }
    run->fiber->job = run.get();
    try {
        switch_into(*run, false);
        settle(*run, true);
    } catch (...) {
        recycle_run(std::move(run));
        throw;
    }
    return script_run(std::move(run));
}

script_run engine::start(const std::string& scriptContent) {
    auto run = prepare_run(*this, detail::run_record::body_kind::source);
    run->text = scriptContent;
    return launch_run(std::move(run));
}

script_run engine::start_file(const std::string& scriptPath) {
    auto run = prepare_run(*this, detail::run_record::body_kind::file);
    run->text = scriptPath;
    return launch_run(std::move(run));
}

script_run engine::start_file_source(const std::string& resolvedPath, const std::string& content) {
    auto run = prepare_run(*this, detail::run_record::body_kind::file_source);
    run->text = resolvedPath;
    run->content = content;
    return launch_run(std::move(run));
}

script_run engine::start(jai::jaibite& bite) {
    auto run = prepare_run(*this, detail::run_record::body_kind::bite);
    run->bite = &bite;
    return launch_run(std::move(run));
}

script_run engine::start(const script_value& callable, std::vector<script_value> args) {
    if (!callable.deref().is_function()) { throw runtime_error("Cannot start a run of a value that is not a function"); }
    auto run = prepare_run(*this, detail::run_record::body_kind::callable);
    run->callable.emplace(callable);
    run->args = std::move(args);
    return launch_run(std::move(run));
}

script_run::script_run(std::unique_ptr<detail::run_record> record) noexcept : record_(std::move(record)) {}

script_run::script_run() noexcept = default;

script_run::script_run(script_run&& other) noexcept : record_(std::move(other.record_)) {}

script_run& script_run::operator=(script_run&& other) noexcept {
    if (this != &other) {
        release();
        record_ = std::move(other.record_);
    }
    return *this;
}

script_run::~script_run() { release(); }

void script_run::release() noexcept {
    if (!record_) { return; }
    if (!record_->alive.expired() && record_->eng && record_->state == detail::run_record::phase::suspended) {
        try { cancel_run(*record_); } catch (...) {}
    }
    recycle_run(std::move(record_));
}

script_run::status script_run::state() const noexcept {
    if (!record_) { return status::completed; }
    switch (record_->state) {
        case detail::run_record::phase::completed: return status::completed;
        case detail::run_record::phase::failed: return status::failed;
        default: return status::suspended;
    }
}

bool script_run::done() const noexcept {
    return !record_ || record_->state == detail::run_record::phase::completed || record_->state == detail::run_record::phase::failed;
}

std::chrono::nanoseconds script_run::wake_at() const noexcept {
    return record_ && record_->state == detail::run_record::phase::suspended ? record_->wake : std::chrono::nanoseconds{};
}

script_run::status script_run::resume() {
    if (!record_) { throw runtime_error(std::string(detail::k_run_empty)); }
    detail::run_record& run = *record_;
    if (done()) { return state(); }
    if (run.state == detail::run_record::phase::running) { throw runtime_error(std::string(detail::k_run_running)); }
    if (std::this_thread::get_id() != run.thread) { throw runtime_error(std::string(detail::k_run_wrong_thread)); }
    if (run.eng->time().monotonic_now() < run.wake) { return status::suspended; }
    switch_into(run, true);
    settle(run, true);
    return state();
}

void script_run::cancel() {
    if (!record_) { throw runtime_error(std::string(detail::k_run_empty)); }
    detail::run_record& run = *record_;
    if (done()) { return; }
    if (run.state == detail::run_record::phase::running) { throw runtime_error(std::string(detail::k_run_running)); }
    if (std::this_thread::get_id() != run.thread) { throw runtime_error(std::string(detail::k_run_wrong_thread)); }
    cancel_run(run);
}

const checked_result<script_value>& script_run::result() const {
    if (!record_) { throw runtime_error(std::string(detail::k_run_empty)); }
    return *record_->result;
}

} // namespace jai
