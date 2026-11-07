#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/debug/controller.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/testing/temporary_directory.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
#include <future>
#include <optional>
#include <filesystem>
#include <fstream>
#include <random>

using namespace jai::foundry;

namespace jai::foundry::tests {

// A wait for what must happen fails only past this guard against a hang: it never bounds speed
constexpr auto kHangGuard = std::chrono::minutes(10);

// The old connector performed this normalization before its native setter. Keep
// that baseline spelling available while the new source-path entry point is added.
template<typename Controller>
void set_source_breakpoints_for_test(Controller& dbg, const std::string& path, std::vector<int> lines) {
    if constexpr (requires { dbg.set_source_breakpoints(path, lines); }) {
        dbg.set_source_breakpoints(path, std::move(lines));
    } else {
        dbg.set_breakpoints(std::filesystem::weakly_canonical(path).string(), std::move(lines));
    }
}

// BOTH backends carry the statement hook, driven by the one engine-owned
// controller. The core flows run twice — an interpreter engine and a vm engine built
// explicitly (not via the backend-aware make_engine(); the suite is backend-agnostic
// by construction, so it runs identically under --backend=vm).
class debugger_tests : public suite {
public:
    debugger_tests() : suite("Debugger") {}

    // Benchmark engines (one per cost-model configuration), built lazily inside the
    // benchmark lambdas so setup cost stays out of the measured loop.
    std::shared_ptr<jai::engine> bench_eng_;

    void pre_test() override { bench_eng_.reset(); }

    static std::shared_ptr<jai::engine> make_backend_engine(jai::backend_type type) {
        auto eng = jai::foundry::make_default_engine();
        eng->set_backend(type);
        return eng;
    }

    // Full core flow: breakpoint -> inspect -> step -> inspect -> resume. Identical
    // DAP-visible behavior expected from both backends.
    void breakpoint_inspect_step_resume(jai::backend_type type) {
        auto eng = make_backend_engine(type);
        auto& dbg = eng->debugger();

        std::mutex m;
        std::condition_variable cv;
        bool stopped = false;
        jai::debug::stop_info last;
        dbg.set_on_stopped([&](const jai::debug::stop_info& si) {
            std::lock_guard<std::mutex> lk(m);
            last = si;
            stopped = true;
            cv.notify_all();
        });

        auto wait_for_stop = [&]() -> bool {
            std::unique_lock<std::mutex> lk(m);
            bool got = cv.wait_for(lk, kHangGuard, [&] { return stopped; });
            stopped = false;   // re-arm for the next stop
            return got;
        };

        dbg.set_breakpoints("<script>", {2});
        dbg.set_enabled(true);

        const std::string src =
            "var x = 1;\n"       // line 1
            "var y = 2;\n"       // line 2  (breakpoint: stop BEFORE this runs)
            "var z = x + y;\n";  // line 3

        std::atomic<bool> done{false};
        std::thread script([&] { eng->execute(src); done = true; });

        // --- breakpoint hit at line 2 ---
        check_true(wait_for_stop());
        check_eq(std::string("breakpoint"), last.reason);
        check_eq(2, last.line);
        check_true(dbg.is_paused());

        // inspect on the parked thread: line 1 ran (x==1), line 2 hasn't (y undefined)
        std::promise<int> px;
        dbg.post_command([&] { px.set_value(static_cast<int>(dbg.get_variable("x").as_int())); });
        check_eq(1, px.get_future().get());

        // --- step to line 3 ---
        dbg.step_into();
        check_true(wait_for_stop());
        check_eq(std::string("step"), last.reason);
        check_eq(3, last.line);

        // now line 2 has run: y==2
        std::promise<int> py;
        dbg.post_command([&] { py.set_value(static_cast<int>(dbg.get_variable("y").as_int())); });
        check_eq(2, py.get_future().get());

        // --- resume to completion ---
        dbg.resume();
        script.join();
        check_true(done.load());
        check_eq(static_cast<int64_t>(3), eng->get_variable("z").as_int());
    }

    // A breakpoint on a loop-body line re-fires every iteration (the vm's boundary
    // detector must treat a backward jump into the same statement as a fresh statement).
    void loop_breakpoint_hits_each_iteration(jai::backend_type type) {
        auto eng = make_backend_engine(type);
        auto& dbg = eng->debugger();

        std::mutex m;
        std::condition_variable cv;
        bool stopped = false;
        bool finished = false;
        dbg.set_on_stopped([&](const jai::debug::stop_info&) {
            std::lock_guard<std::mutex> lk(m);
            stopped = true;
            cv.notify_all();
        });

        dbg.set_breakpoints("<script>", {3});
        dbg.set_enabled(true);

        const std::string src =
            "auto total = 0;\n"                      // line 1
            "for (auto i = 0; i < 3; i += 1) {\n"    // line 2
            "    total += i;\n"                      // line 3  (breakpoint)
            "}\n";                                   // line 4

        std::thread script([&] {
            eng->execute(src);
            std::lock_guard<std::mutex> lk(m);
            finished = true;
            cv.notify_all();
        });

        int hits = 0;
        for (;;) {
            std::unique_lock<std::mutex> lk(m);
            if (!cv.wait_for(lk, kHangGuard, [&] { return stopped || finished; })) break;
            if (finished) break;
            stopped = false;
            ++hits;
            lk.unlock();
            dbg.resume();
        }
        dbg.set_enabled(false);   // stray stop can never park past this; join is hang-proof
        script.join();
        check_eq(3, hits);
        check_eq(static_cast<int64_t>(3), eng->get_variable("total").as_int());
    }

    // Named frame locals at a stop inside a function: parameters + reached body locals
    // with their live values. Interpreter reads the frame's function AST; the vm
    // reconstructs slot names lazily from the chunk's decl/load/store operands.
    // A stop inside a lambda made in a function over its locals (the vm's converted form): its captures are not frame locals,
    // a read names them through the stop's scope as the environment form does, and a set writes the program scope
    std::string captured_names_at_a_closure_stop(jai::backend_type type) {
        auto eng = make_backend_engine(type);
        eng->execution_budget(0);
        jai::stdlib::register_all(eng);
        auto& dbg = eng->debugger();
        stop_waiter stops(dbg);
        dbg.set_breakpoints("<script>", {5});
        dbg.set_enabled(true);
        const std::string src =
            "function run() -> string {\n"                              // line 1
            "    int r = 5; int c = 1; int s = 10;\n"                   // line 2
            "    var f = [r, c, &s]() -> int {\n"                       // line 3
            "        c = c + 1;\n"                                      // line 4
            "        s = s + r;\n"                                      // line 5 (breakpoint)
            "        return c + s;\n"                                   // line 6
            "    };\n"
            "    return to_string(f()) + \",\" + to_string(s);\n"
            "}\n"
            "auto out = run();\n";
        std::string script_error;
        std::string out;
        std::promise<void> detached;
        std::thread script([&, detached_future = detached.get_future()] {
            try { eng->execute(src); } catch (const std::exception& e) { script_error = e.what(); }
            // the engine dies on the thread that parsed it: a canary build tags its literals to that thread
            detached_future.wait();
            out = eng->get_variable("out").to_string();
            eng.reset();
        });
        std::string seen = "no stop";
        if (const auto hit = stops.wait()) {
            seen = parked_read(dbg, [&] {
                std::string text = "line " + std::to_string(hit->line) + ";";
                for (const auto& v : dbg.list_locals()) { text += " local " + v.name + "=" + v.value; }
                for (const char* name : {"r", "c", "s"}) {
                    try { text += std::string(" ") + name + "=" + dbg.get_variable(name).to_string(); }
                    catch (const std::exception& e) { text += std::string(" ") + name + ":" + e.what(); }
                }
                dbg.set_variable("r", eng->make_value(7));
                text += " r after set=" + dbg.get_variable("r").to_string();
                return text;
            }).value_or("no read");
            dbg.clear_breakpoints();
            dbg.resume();
        }
        dbg.detach();
        detached.set_value();
        script.join();
        return seen + " | " + script_error + " | " + out;
    }

    void frame_locals_at_breakpoint(jai::backend_type type) {
        auto eng = make_backend_engine(type);
        auto& dbg = eng->debugger();

        std::mutex m;
        std::condition_variable cv;
        bool stopped = false;
        dbg.set_on_stopped([&](const jai::debug::stop_info&) {
            std::lock_guard<std::mutex> lk(m);
            stopped = true;
            cv.notify_all();
        });

        dbg.set_breakpoints("<script>", {4});
        dbg.set_enabled(true);

        const std::string src =
            "function calc(int a, int b) -> int {\n"   // line 1
            "    int c = a + b;\n"                     // line 2
            "    int d = c * 2;\n"                     // line 3
            "    return d;\n"                          // line 4  (breakpoint: a,b,c,d live)
            "}\n"
            "auto r = calc(3, 4);\n";                  // line 6

        std::atomic<bool> done{false};
        std::thread script([&] { eng->execute(src); done = true; });

        bool got_stop = false;
        {
            std::unique_lock<std::mutex> lk(m);
            got_stop = cv.wait_for(lk, kHangGuard, [&] { return stopped; });
        }

        // Gather while parked, then ALWAYS release the script thread before asserting —
        // a failed check must never leave a parked thread for the std::thread dtor.
        std::vector<jai::debug::variable_info> locals;
        bool got_locals = false;
        if (got_stop) {
            std::promise<std::vector<jai::debug::variable_info>> pv;
            auto fut = pv.get_future();
            dbg.post_command([&] { pv.set_value(dbg.list_locals()); });
            got_locals = fut.wait_for(kHangGuard) == std::future_status::ready;
            if (got_locals) locals = fut.get();
            dbg.resume();
        }
        dbg.detach();
        script.join();

        check_true(got_stop);
        check_true(got_locals);
        auto value_of = [&](const char* name) -> std::string {
            for (const auto& v : locals) { if (v.name == name) return v.value; }
            return "<missing>";
        };
        check_eq(std::string("3"), value_of("a"));
        check_eq(std::string("4"), value_of("b"));
        check_eq(std::string("7"), value_of("c"));
        check_eq(std::string("14"), value_of("d"));
        check_true(done.load());
        check_eq(static_cast<int64_t>(14), eng->get_variable("r").as_int());
    }

    // A parameter whose ONLY use is a bare compound rhs (`total += bump`) keeps its name
    // at a stop: the vm fuses that op_load away, so naming rides the compound_fused
    // proto's bare rhs operand (the interpreter reads the frame's AST as usual).
    void frame_locals_name_bare_compound_rhs(jai::backend_type type) {
        auto eng = make_backend_engine(type);
        auto& dbg = eng->debugger();

        std::mutex m;
        std::condition_variable cv;
        bool stopped = false;
        dbg.set_on_stopped([&](const jai::debug::stop_info&) {
            std::lock_guard<std::mutex> lk(m);
            stopped = true;
            cv.notify_all();
        });

        dbg.set_breakpoints("<script>", {4});
        dbg.set_enabled(true);

        const std::string src =
            "function acc(int seed, int bump) -> int {\n"   // line 1
            "    int total = seed;\n"                       // line 2
            "    total += bump;\n"                          // line 3  (bare compound rhs)
            "    return total;\n"                           // line 4  (breakpoint)
            "}\n"
            "auto r = acc(30, 12);\n";                      // line 6

        std::atomic<bool> done{false};
        std::thread script([&] { eng->execute(src); done = true; });

        bool got_stop = false;
        {
            std::unique_lock<std::mutex> lk(m);
            got_stop = cv.wait_for(lk, kHangGuard, [&] { return stopped; });
        }

        std::vector<jai::debug::variable_info> locals;
        bool got_locals = false;
        if (got_stop) {
            std::promise<std::vector<jai::debug::variable_info>> pv;
            auto fut = pv.get_future();
            dbg.post_command([&] { pv.set_value(dbg.list_locals()); });
            got_locals = fut.wait_for(kHangGuard) == std::future_status::ready;
            if (got_locals) locals = fut.get();
            dbg.resume();
        }
        dbg.detach();
        script.join();

        check_true(got_stop);
        check_true(got_locals);
        auto value_of = [&](const char* name) -> std::string {
            for (const auto& v : locals) { if (v.name == name) return v.value; }
            return "<missing>";
        };
        check_eq(std::string("30"), value_of("seed"));
        check_eq(std::string("12"), value_of("bump"));
        check_eq(std::string("42"), value_of("total"));
        check_true(done.load());
        check_eq(static_cast<int64_t>(42), eng->get_variable("r").as_int());
    }

    // Stops reported on the script thread, waited for one at a time from the driver thread
    struct stop_waiter {
        std::mutex m;
        std::condition_variable cv;
        bool stopped = false;
        jai::debug::stop_info last;

        explicit stop_waiter(jai::debug::controller& dbg) {
            dbg.set_on_stopped([this](const jai::debug::stop_info& si) {
                std::lock_guard<std::mutex> lk(m);
                last = si;
                stopped = true;
                cv.notify_all();
            });
        }

        // The stop, or nothing when none came before the hang guard
        std::optional<jai::debug::stop_info> wait() {
            std::unique_lock<std::mutex> lk(m);
            if (!cv.wait_for(lk, kHangGuard, [&] { return stopped; })) return std::nullopt;
            stopped = false;
            return last;
        }
    };

    // Runs read on the parked script thread and hands back its answer (nothing when the thread never ran it)
    template<typename Read>
    static auto parked_read(jai::debug::controller& dbg, Read read) -> std::optional<decltype(read())> {
        auto answer = std::make_shared<std::promise<decltype(read())>>();
        auto fut = answer->get_future();
        dbg.post_command([answer, read] { answer->set_value(read()); });
        if (fut.wait_for(kHangGuard) != std::future_status::ready) return std::nullopt;
        return fut.get();
    }

    static std::string value_named(const std::vector<jai::debug::variable_info>& vars, const std::string& name) {
        for (const auto& v : vars) { if (v.name == name) return v.value; }
        return "<missing>";
    }

    // The loop head of a sleep-paced frame loop: the driver holds the script there to act between two known iterations, and
    // learns whether a stop or the next iteration came first
    struct paced_loop_gate {
        std::mutex m;
        std::condition_variable cv;
        int64_t ticks = 0;
        bool holding = false;
        bool step = true;          // hold at every tick until released
        bool released = false;
        bool keep_going = true;
        int64_t stops = 0;
        jai::debug::stop_info last;

        bool tick() {
            std::unique_lock<std::mutex> lk(m);
            ++ticks;
            holding = true;
            cv.notify_all();
            cv.wait(lk, [&] { return released || !step; });
            released = false;
            holding = false;
            return keep_going;
        }
        // The tick the script is held at, once it is
        int64_t held() {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, kHangGuard, [&] { return holding && !released; });
            return ticks;
        }
        // Lets the held script run on; true when it stopped before reaching its next tick
        bool release_until_stop_or_next_tick() {
            std::unique_lock<std::mutex> lk(m);
            const int64_t at = ticks;
            const int64_t stops_before = stops;
            released = true;
            cv.notify_all();
            cv.wait_for(lk, kHangGuard, [&] { return stops > stops_before || ticks > at; });
            return stops > stops_before;
        }
        void finish() {
            std::lock_guard<std::mutex> lk(m);
            step = false;
            keep_going = false;
            cv.notify_all();
        }
    };

    // A frame loop paced by sleeps runs a few statements between them, far fewer than the 1024 back edges and calls the
    // budget clock samples at: an attach with a breakpoint, and then a pause, made while it runs stop it within the iteration
    void sleep_paced_loop_stops_promptly(jai::backend_type type) {
        auto eng = make_backend_engine(type);
        eng->execution_budget(0);
        jai::stdlib::register_all(eng);
        auto& dbg = eng->debugger();
        paced_loop_gate gate;
        dbg.set_on_stopped([&gate](const jai::debug::stop_info& si) {
            std::lock_guard<std::mutex> lk(gate.m);
            gate.last = si;
            ++gate.stops;
            gate.cv.notify_all();
        });
        eng->add_function("tick", [&gate] { return gate.tick(); });

        const std::string src =
            "int frames = 0;\n"                         // line 1
            "bool quit = false;\n"                      // line 2
            "while (!quit && tick()) {\n"               // line 3
            "    sleep(0.02);\n"                        // line 4
            "    frames = frames + 1;\n"                // line 5
            "}\n";
        std::string script_error;
        std::thread script([&] {
            try { eng->execute(src); } catch (const std::exception& e) { script_error = e.what(); }
        });

        const int64_t armed_at = gate.held();   // running, no session armed
        dbg.set_breakpoints("<script>", {5});
        dbg.set_enabled(true);
        const bool hit = gate.release_until_stop_or_next_tick();
        jai::debug::stop_info hit_info;
        std::optional<int64_t> frames_at_hit;
        if (hit) {
            hit_info = gate.last;
            frames_at_hit = parked_read(dbg, [&] { return eng->get_variable("frames").as_int(); });
            dbg.clear_breakpoints();
            dbg.resume();
        }

        const int64_t paused_at = gate.held();
        dbg.request_pause();
        const bool pause = gate.release_until_stop_or_next_tick();
        jai::debug::stop_info pause_info;
        std::optional<int64_t> frames_at_pause;
        if (pause) {
            pause_info = gate.last;
            frames_at_pause = parked_read(dbg, [&] {
                dbg.set_variable("quit", eng->make_value(true));
                return eng->get_variable("frames").as_int();
            });
        }
        gate.finish();
        dbg.detach();
        script.join();

        check_eq(std::string(), script_error);
        check_true(hit, "a breakpoint set while a sleep-paced loop runs stops it before the loop comes round again");
        if (hit) {
            check_eq(std::string("breakpoint"), hit_info.reason);
            check_eq(5, hit_info.line);
        }
        check_true(frames_at_hit.has_value() && *frames_at_hit == armed_at - 1, "the stop comes at the sleep after the attach");
        check_true(pause, "a pause while a sleep-paced loop runs stops it before the loop comes round again");
        if (pause) check_eq(std::string("pause"), pause_info.reason);
        check_true(frames_at_pause.has_value() && *frames_at_pause == paused_at - 1, "the pause comes at the sleep after the request");
        check_true(pause && eng->get_variable("quit").as<bool>(), "the paused loop was released through quit");
    }

    // A pause while the script sleeps parks it inside the sleep, at the statement that sleeps; the rest of the sleep runs
    // after the resume
    void pause_wakes_a_sleeping_script(jai::backend_type type) {
        using clock = std::chrono::steady_clock;
        const auto give_up = clock::now() + kHangGuard;
        for (;;) {
            if (pause_wakes_a_sleeping_script_attempt(type)) { return; }
            if (clock::now() > give_up) {
                check_true(false, "no attempt requested its pause before the sleep could end");
                return;
            }
        }
    }

    // False when the pause was requested too late to prove anything (a starved driver thread): the sleep may have ended
    bool pause_wakes_a_sleeping_script_attempt(jai::backend_type type) {
        auto eng = make_backend_engine(type);
        eng->execution_budget(0);
        jai::stdlib::register_all(eng);
        auto& dbg = eng->debugger();
        stop_waiter stops(dbg);
        dbg.set_enabled(true);
        // a fixed delay before the pause let a loaded machine pause before line 1 declared done
        std::promise<std::chrono::steady_clock::time_point> declared;
        eng->add_function("declared", [&declared] { declared.set_value(std::chrono::steady_clock::now()); });

        const std::string src =
            "int done = 0; declared();\n"               // line 1
            "sleep(1.5);\n"                             // line 2
            "done = 1;\n";                              // line 3
        std::string script_error;
        std::thread script([&] {
            try { eng->execute(src); } catch (const std::exception& e) { script_error = e.what(); }
        });
        const auto sleep_starts_after = declared.get_future().get();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        dbg.request_pause();
        if (std::chrono::steady_clock::now() - sleep_starts_after > std::chrono::seconds(1)) {
            dbg.detach();
            script.join();
            return false;
        }
        const auto pause = stops.wait();
        std::optional<int64_t> done_at_pause;
        if (pause) {
            done_at_pause = parked_read(dbg, [&] { return eng->get_variable("done").as_int(); });
            dbg.resume();
        }
        dbg.detach();
        script.join();

        check_eq(std::string(), script_error);
        check_true(pause.has_value(), "a pause reaches a sleeping script");
        if (pause) {
            check_eq(std::string("pause"), pause->reason);
            check_eq(2, pause->line);
        }
        check_true(done_at_pause.has_value() && *done_at_pause == 0, "parked inside the sleep, before the next statement");
        check_eq(int64_t(1), eng->get_variable("done").as_int());
        return true;
    }

    // sleep(0) waits for nothing and is the sync point a loop calls: a session armed mid-run takes its breakpoint at the
    // next statement that has one, a pause parks at the sleep(0) itself
    void sleep_zero_is_a_sync_point(jai::backend_type type) {
        auto eng = make_backend_engine(type);
        eng->execution_budget(0);
        jai::stdlib::register_all(eng);
        auto& dbg = eng->debugger();
        stop_waiter stops(dbg);
        eng->add_function("arm_breakpoint", [&dbg] { dbg.set_breakpoints("<script>", {5}); dbg.set_enabled(true); });
        eng->add_function("arm_pause", [&dbg] { dbg.set_enabled(true); dbg.request_pause(); });
        const std::string src =
            "int at = -1;\n"                            // line 1
            "for (int i = 0; i < 10; ++i) {\n"          // line 2
            "    if (i == 3) { arm_breakpoint(); }\n"   // line 3
            "    sleep(0);\n"                           // line 4
            "    at = i;\n"                             // line 5  (breakpoint)
            "}\n";
        std::string script_error;
        std::thread script([&] {
            try { eng->execute(src); } catch (const std::exception& e) { script_error = e.what(); }
        });
        const auto hit = stops.wait();
        std::optional<int64_t> at_hit;
        if (hit) {
            at_hit = parked_read(dbg, [&] { return eng->get_variable("at").as_int(); });
            dbg.clear_breakpoints();
            dbg.resume();
        }
        dbg.detach();
        script.join();
        check_eq(std::string(), script_error);
        check_true(hit.has_value(), "the breakpoint armed mid-loop is hit");
        if (hit) { check_eq(5, hit->line); }
        check_true(at_hit.has_value() && *at_hit == 2, "on the iteration that armed it, right after the sleep(0)");

        auto eng2 = make_backend_engine(type);
        eng2->execution_budget(0);
        jai::stdlib::register_all(eng2);
        auto& dbg2 = eng2->debugger();
        stop_waiter stops2(dbg2);
        eng2->add_function("arm_pause", [&dbg2] { dbg2.set_enabled(true); dbg2.request_pause(); });
        const std::string paused_src =
            "int reached = 0;\n"                        // line 1
            "arm_pause();\n"                            // line 2
            "sleep(0);\n"                               // line 3
            "reached = 1;\n";                           // line 4
        std::thread paused([&] {
            try { eng2->execute(paused_src); } catch (const std::exception& e) { script_error = e.what(); }
        });
        const auto pause = stops2.wait();
        std::optional<int64_t> reached_at_pause;
        if (pause) {
            reached_at_pause = parked_read(dbg2, [&] { return eng2->get_variable("reached").as_int(); });
            dbg2.resume();
        }
        dbg2.detach();
        paused.join();
        check_eq(std::string(), script_error);
        check_true(pause.has_value(), "the pause takes effect at the sleep(0)");
        if (pause) {
            check_eq(std::string("pause"), pause->reason);
            check_eq(3, pause->line);
        }
        check_true(reached_at_pause.has_value() && *reached_at_pause == 0, "before the statement after it");
    }

    struct stack_view {
        std::vector<jai::debug::frame_info> frames;
        std::vector<std::vector<jai::debug::variable_info>> locals;
        std::vector<jai::debug::variable_info> globals;
    };

    static stack_view read_stack(jai::debug::controller& dbg) {
        stack_view result;
        result.frames = dbg.list_frames();
        for (size_t i = 0; i < result.frames.size(); ++i) { result.locals.push_back(dbg.list_locals(i)); }
        result.globals = dbg.list_globals();
        return result;
    }

    // The stack at a stop lists every frame innermost first, named and located as an error trace has it, the top level
    // last; each frame has its own locals and the program scope its globals
    void stack_trace_lists_every_frame_with_its_locals(jai::backend_type type) {
        auto eng = make_backend_engine(type);
        auto& dbg = eng->debugger();
        stop_waiter stops(dbg);
        dbg.set_breakpoints("<script>", {4, 13});
        dbg.set_enabled(true);

        const std::string src =
            "int counter = 7;\n"                        // line 1
            "function inner(int a) -> int {\n"          // line 2
            "    int b = a + 1;\n"                      // line 3
            "    return b * 2;\n"                       // line 4  (breakpoint)
            "}\n"                                       // line 5
            "function tick(int n) -> int {\n"           // line 6
            "    int local_tick = n + 10;\n"            // line 7
            "    return inner(local_tick);\n"           // line 8
            "}\n"                                       // line 9
            "auto result = tick(counter);\n"            // line 10
            "{\n"                                       // line 11
            "    int block_local = result + 1;\n"       // line 12
            "    int twice = block_local * 2;\n"        // line 13 (breakpoint, at the top level)
            "}\n";
        std::string script_error;
        std::thread script([&] {
            try { eng->execute(src); } catch (const std::exception& e) { script_error = e.what(); }
        });

        const auto nested = stops.wait();
        std::optional<stack_view> in_inner;
        if (nested) {
            in_inner = parked_read(dbg, [&] { return read_stack(dbg); });
            dbg.resume();
        }
        const auto top = stops.wait();
        std::optional<stack_view> at_top;
        if (top) {
            at_top = parked_read(dbg, [&] { return read_stack(dbg); });
            dbg.resume();
        }
        dbg.detach();
        script.join();

        check_eq(std::string(), script_error);
        check_true(in_inner.has_value());
        if (in_inner) {
            const auto& s = *in_inner;
            check_eq(size_t(3), s.frames.size());
            if (s.frames.size() == 3) {
                check_eq(std::string("inner"), s.frames[0].name);
                check_eq(4, s.frames[0].line);
                check_eq(std::string("tick"), s.frames[1].name);
                check_eq(8, s.frames[1].line);
                check_eq(std::string("<script>"), s.frames[2].name);
                check_eq(10, s.frames[2].line);
                check_eq(std::string("<script>"), s.frames[1].file);
                check_eq(std::string("17"), value_named(s.locals[0], "a"));
                check_eq(std::string("18"), value_named(s.locals[0], "b"));
                check_eq(std::string("<missing>"), value_named(s.locals[0], "n"));
                check_eq(std::string("7"), value_named(s.locals[1], "n"));
                check_eq(std::string("17"), value_named(s.locals[1], "local_tick"));
                check_eq(std::string("<missing>"), value_named(s.locals[1], "a"));
                check_true(s.locals[2].empty(), "the top level's block scopes are out of reach below a call");
            }
            check_eq(std::string("7"), value_named(s.globals, "counter"));
            check_eq(std::string("<missing>"), value_named(s.globals, "result"));
            check_eq(std::string("<missing>"), value_named(s.globals, "tick"));
        }
        check_true(at_top.has_value());
        if (at_top) {
            const auto& s = *at_top;
            check_eq(size_t(1), s.frames.size());
            if (s.frames.size() == 1) {
                check_eq(std::string("<script>"), s.frames[0].name);
                check_eq(13, s.frames[0].line);
                check_eq(std::string("37"), value_named(s.locals[0], "block_local"));
                check_eq(std::string("<missing>"), value_named(s.locals[0], "counter"));
            }
            check_eq(std::string("36"), value_named(s.globals, "result"));
        }
    }

    // Every stop of a construction, as frames (innermost first: name:line) and each frame's local names: a base level's
    // initializer-list argument, field initializer and body, then the class's own
    std::string construction_stops(jai::backend_type type) {
        auto eng = make_backend_engine(type);
        auto& dbg = eng->debugger();
        stop_waiter stops(dbg);
        dbg.set_breakpoints("<script>", {2, 7, 13});
        dbg.set_enabled(true);
        const std::string src =
            "int hook(int v) {\n"                       // line 1
            "    return v;\n"                           // line 2  (breakpoint)
            "}\n"                                       // line 3
            "class A {\n"                               // line 4
            "    int a = hook(1);\n"                    // line 5
            "    A(int x) {\n"                          // line 6
            "        int ax = x;\n"                     // line 7  (breakpoint)
            "    }\n"                                   // line 8
            "}\n"                                       // line 9
            "class B : A {\n"                           // line 10
            "    int b = hook(2);\n"                    // line 11
            "    B(int y) : super(hook(y + 2)) {\n"     // line 12
            "        int by = y;\n"                     // line 13 (breakpoint)
            "    }\n"                                   // line 14
            "}\n"                                       // line 15
            "B made = B(1);\n";                         // line 16
        std::string script_error;
        std::thread script([&] {
            try { eng->execute(src); } catch (const std::exception& e) { script_error = e.what(); }
        });
        std::string result;
        for (int stop = 0; stop < 5; ++stop) {
            const auto at = stops.wait();
            if (!at) { result += "<no stop>"; break; }
            auto seen = parked_read(dbg, [&] { return read_stack(dbg); });
            if (seen) {
                for (size_t i = 0; i < seen->frames.size(); ++i) {
                    result += (i ? " " : "") + seen->frames[i].name + ":" + std::to_string(seen->frames[i].line) + "(";
                    for (size_t j = 0; j < seen->locals[i].size(); ++j) { result += (j ? "," : "") + seen->locals[i][j].name; }
                    result += ")";
                }
            }
            result += " | ";
            dbg.resume();
        }
        dbg.detach();
        script.join();
        return result + script_error;
    }

    // Each stop's frames as name:line(local=value,...): only what is in scope at the frame's statement, an inner name hiding
    // an outer one, a parameter whether or not the body reads it; then reads, writes and evaluations through the stop's scope
    std::string scoped_locals(jai::backend_type type) {
        auto eng = make_backend_engine(type);
        eng->add_global("host_value", eng->make_value(1));
        auto& dbg = eng->debugger();
        stop_waiter stops(dbg);
        dbg.set_breakpoints("<script>", {4, 9, 13});
        dbg.set_enabled(true);
        const std::string src =
            "int g = 5;\n"                                                   // line 1
            "class Token { string n; Token(string a) { n = a; } }\n"         // line 2
            "function take(Token t) -> string {\n"                           // line 3
            "    return t.n;\n"                                              // line 4  (breakpoint)
            "}\n"                                                            // line 5
            "function f(int x) -> int {\n"                                   // line 6
            "    int v = 1;\n"                                               // line 7
            "    { int v = 2;\n"                                             // line 8
            "        g = v; }\n"                                             // line 9  (breakpoint)
            "    for (int i = 0; i < 1; i++) { int t = i; g += t; }\n"       // line 10
            "    for (int i = 5; i < 6; i++) { string t = \"s\"; g += 1; }\n" // line 11
            "    string s = take(Token(\"tk\"));\n"                          // line 12
            "    return v;\n"                                                // line 13 (breakpoint)
            "}\n"                                                            // line 14
            "auto r = f(3);\n";                                              // line 15
        std::string script_error;
        std::thread script([&] {
            try { eng->execute(src); } catch (const std::exception& e) { script_error = e.what(); }
        });
        auto attempt = [&](auto&& step) -> std::string {
            try { return step(); } catch (const std::exception& e) { return std::string("error: ") + e.what(); }
        };
        std::string result;
        for (int stop = 0; stop < 3; ++stop) {
            const auto at = stops.wait();
            if (!at) { result += "<no stop>"; break; }
            auto seen = parked_read(dbg, [&] {
                std::string text;
                const auto stack = read_stack(dbg);
                for (size_t i = 0; i < stack.frames.size(); ++i) {
                    text += (i ? " " : "") + stack.frames[i].name + ":" + std::to_string(stack.frames[i].line) + "(";
                    for (size_t j = 0; j < stack.locals[i].size(); ++j) { text += (j ? "," : "") + stack.locals[i][j].name + "=" + stack.locals[i][j].value; }
                    text += ")";
                }
                text += " globals(";
                for (const auto& g : stack.globals) { text += g.name + ","; }
                text += ")";
                if (at->line == 4) {
                    text += " t.n=" + attempt([&] { const auto v = dbg.evaluate("t.n"); return v.value + ":" + v.type; });
                    text += " t.m=" + attempt([&] { return dbg.evaluate("t.m").value; });
                } else if (at->line == 9) {
                    text += " v=" + attempt([&] { return dbg.get_variable("v").to_string(); });
                    text += " x=" + attempt([&] { return dbg.get_variable("x").to_string(); });
                    text += " g=" + attempt([&] { return dbg.get_variable("g").to_string(); });
                    text += " set v=" + attempt([&] { dbg.set_variable("v", eng->make_value(20)); return dbg.get_variable("v").to_string(); });
                } else {
                    text += " set v=" + attempt([&] { dbg.set_variable("v", eng->make_value(7)); return std::string("7"); });
                    text += " nope=" + attempt([&] { dbg.set_variable("nope", eng->make_value(1)); return std::string("set"); });
                }
                return text;
            });
            result += (seen ? *seen : std::string("<no read>")) + " | ";
            dbg.resume();
        }
        dbg.detach();
        script.join();
        result += "r=" + attempt([&] { return eng->get_variable("r").to_string(); }) + " g=" + attempt([&] { return eng->get_variable("g").to_string(); });
        result += " nope=" + attempt([&] { return eng->get_variable("nope").to_string(); }).substr(0, 5);
        return result + script_error;
    }

    // a reserved word naming a method or a field is a member like any other: the frame names it, its reads show in the locals
    void keyword_named_members_in_frames_and_locals(jai::backend_type type) {
        auto eng = make_backend_engine(type);
        auto& dbg = eng->debugger();
        stop_waiter stops(dbg);
        dbg.set_breakpoints("<script>", {5});
        dbg.set_enabled(true);

        const std::string src =
            "class Promise {\n"                         // line 1
            "    int default = 7;\n"                    // line 2
            "    int catch(int code) {\n"               // line 3
            "        int settled = this.default;\n"     // line 4
            "        return code + settled;\n"          // line 5  (breakpoint)
            "    }\n"                                   // line 6
            "}\n"                                       // line 7
            "Promise p;\n"                              // line 8
            "auto r = p.catch(3);\n";                   // line 9
        std::string script_error;
        std::thread script([&] {
            try { eng->execute(src); } catch (const std::exception& e) { script_error = e.what(); }
        });

        const auto stop = stops.wait();
        std::optional<stack_view> in_catch;
        if (stop) {
            in_catch = parked_read(dbg, [&] { return read_stack(dbg); });
            dbg.resume();
        }
        dbg.detach();
        script.join();

        check_eq(std::string(), script_error);
        check_true(in_catch.has_value());
        if (in_catch) {
            const auto& s = *in_catch;
            check_eq(size_t(2), s.frames.size());
            if (s.frames.size() == 2) {
                check_eq(std::string("catch"), s.frames[0].name);
                check_eq(5, s.frames[0].line);
                check_eq(9, s.frames[1].line);
                check_eq(std::string("3"), value_named(s.locals[0], "code"));
                check_eq(std::string("7"), value_named(s.locals[0], "settled"));
            }
        }
        check_eq(static_cast<int64_t>(10), eng->get_variable("r").as_int());
    }

    // Parallel regions are ATOMIC to the debugger. A breakpoint inside the
    // body never fires while the body runs as part of a region — including chunk 0 on
    // the calling thread (structural: every region context is a fresh slot backend the
    // controller was never wired to) — but the SAME body called serially hits normally.
    void parallel_region_atomic_to_debugger(jai::backend_type type) {
        auto eng = make_backend_engine(type);
        auto& dbg = eng->debugger();

        std::mutex m;
        std::condition_variable cv;
        bool stopped = false;
        bool finished = false;
        std::vector<jai::debug::stop_info> stops;
        dbg.set_on_stopped([&](const jai::debug::stop_info& si) {
            std::lock_guard<std::mutex> lk(m);
            stops.push_back(si);
            stopped = true;
            cv.notify_all();
        });

        dbg.set_breakpoints("<script>", {2, 6});

        dbg.set_enabled(true);

        const std::string src =
            "int double_it(int x) {\n"                            // line 1
            "    return x * 2;\n"                                 // line 2  (breakpoint)
            "}\n"                                                 // line 3
            "auto arr = [1, 2, 3, 4];\n"                          // line 4
            "auto pout = parallel_transform(arr, double_it);\n"   // line 5 (region: MUST be silent)
            "auto marker = 0;\n"                                  // line 6  (breakpoint: order sentinel)
            "auto serial = double_it(21);\n";                     // line 7 (serial: hits)

        std::thread script([&] {
            eng->execute(src);
            std::lock_guard<std::mutex> lk(m);
            finished = true;
            cv.notify_all();
        });

        int hits = 0;
        for (;;) {
            std::unique_lock<std::mutex> lk(m);
            if (!cv.wait_for(lk, kHangGuard, [&] { return stopped || finished; })) break;
            if (finished) break;
            stopped = false;
            ++hits;
            lk.unlock();
            dbg.resume();
        }
        dbg.set_enabled(false);   // stray stop can never park past this; join is hang-proof
        script.join();
        if (hits != 2) {
            for (const auto& s : stops) {
                std::cout << "    stop: " << s.reason << " " << s.file << ":" << s.line
                          << " depth=" << s.depth << std::endl;
            }
        }
        check_eq(2, hits);   // marker + serial call ONLY; the region ran the body 4x silently
        check_eq(static_cast<size_t>(2), stops.size());
        check_eq(6, stops[0].line);   // region produced no stop before the sentinel
        check_eq(2, stops[1].line);
        check_eq(static_cast<int64_t>(42), eng->get_variable("serial").as_int());
        // pout is an inferred array<int> (raw buffer): the host extracts the typed vector
        const auto pout = eng->get_variable("pout").as<std::vector<jai::script_int>>();
        check_eq(static_cast<size_t>(4), pout.size());
        check_eq(static_cast<int64_t>(8), pout[3]);
    }

    // How a run is driven at each stop: one step kind, a fixed cycle of them, or breakpoints on the odd lines and a resume.
    enum class drive { into, over, mixed, odd_breakpoints };

    // Drives a whole run from its first statement on the script thread (the stop callback queues the next step before the park
    // waits) and returns the stop lines. Stops past the cap resume, so a runaway sequence still ends.
    static std::vector<int> step_line_sequence(jai::backend_type type, const std::string& src, drive how = drive::into) {
        auto eng = make_backend_engine(type);
        auto& dbg = eng->debugger();
        std::vector<int> lines;
        dbg.set_on_stopped([&](const jai::debug::stop_info& si) {
            lines.push_back(si.line);
            if (lines.size() > 4000 || how == drive::odd_breakpoints) { dbg.resume(); return; }
            static constexpr jai::debug::step_mode mixed[] = { jai::debug::step_mode::into, jai::debug::step_mode::into,
                jai::debug::step_mode::into, jai::debug::step_mode::over, jai::debug::step_mode::into, jai::debug::step_mode::out };
            const auto mode = how == drive::mixed ? mixed[(lines.size() - 1) % 6]
                : how == drive::over ? jai::debug::step_mode::over : jai::debug::step_mode::into;
            switch (mode) {
                case jai::debug::step_mode::over: dbg.step_over(); break;
                case jai::debug::step_mode::out: dbg.step_out(); break;
                default: dbg.step_into(); break;
            }
        });
        if (how == drive::odd_breakpoints) {
            std::vector<int> odd;
            for (int line = 1; line < 64; line += 2) { odd.push_back(line); }
            dbg.set_breakpoints("<script>", odd);
        }
        dbg.set_enabled(true);
        if (how != drive::odd_breakpoints) { dbg.request_pause(); }
        try { eng->execute(src); } catch (...) { lines.push_back(-1); }
        dbg.set_enabled(false);
        return lines;
    }

    static std::string line_list(const std::vector<int>& lines) {
        std::string out;
        for (int l : lines) { out += (out.empty() ? "" : " ") + std::to_string(l); }
        return out;
    }

    void forge_tests() override {
        test("step_into_line_sequence_through_hot_loops", [this]() {
            const std::string src =
                "int s = 0;\n"                                   // 1
                "int t = 0;\n"                                   // 2
                "for (int i = 0; i < 4; i += 1) {\n"             // 3
                "    t = t + 60;\n"                              // 4
                "    if (t > 100) { s = 0; }\n"                  // 5
                "}\n"
                "auto done = s + t;\n";                          // 7
            // one stop per statement execution: the header once, then each iteration's statements (the then-branch's
            // block holds a statement, it is not one)
            const std::string expected = "1 2 3 4 5 4 5 5 4 5 5 4 5 5 7";
            check_eq(expected, line_list(step_line_sequence(jai::backend_type::interpreter, src)));
            check_eq(expected, line_list(step_line_sequence(jai::backend_type::vm, src)), "the vm stops where the interpreter stops");
        });

        test("step_line_sequences_match_the_interpreter_over_statement_shapes", [this]() {
            struct shape { const char* name; const char* src; };
            const shape shapes[] = {
                { "untyped loop, if last", "var s = 0;\nvar t = 0;\nfor (var i = 0; i < 4; i += 1) {\n    t = t + 60;\n    if (t > 100) { s = 0; }\n}\nvar done = s;\n" },
                { "if/else blocks", "var a = 3;\nif (a > 2) {\n    a = 1;\n} else {\n    a = 2;\n}\nif (a > 2) {\n    a = 5;\n} else {\n    a = 6;\n}\nvar b = a;\n" },
                { "if/else one line", "var a = 3;\nif (a > 2) a = 1; else a = 2;\nif (a > 2) a = 5; else { a = 6; }\nvar b = a;\n" },
                { "else if chain", "var a = 0;\nfor (var i = 0; i < 4; i += 1) {\n    if (i == 0) a += 1;\n    else if (i == 1) a += 2;\n    else if (i == 2) { a += 3; }\n    else a += 4;\n}\nvar b = a;\n" },
                { "while", "var n = 0;\nwhile (n < 3) {\n    n += 1;\n}\nwhile (n < 6) n += 1;\nvar b = n;\n" },
                { "typed while", "int n = 0;\nint m = 0;\nwhile (n < 5) {\n    n += 1;\n    m += n;\n}\nauto b = m;\n" },
                { "do while", "var n = 0;\ndo {\n    n += 1;\n} while (n < 3);\nvar b = n;\n" },
                { "counted for, typed", "int s = 0;\nfor (int i = 0; i < 5; i++) {\n    s += i;\n}\nauto b = s;\n" },
                { "nested loops break continue", "var s = 0;\nfor (var i = 0; i < 3; i += 1) {\n    for (var j = 0; j < 3; j += 1) {\n        if (j == 1) continue;\n        if (i == 2) break;\n        s += j;\n    }\n}\nvar b = s;\n" },
                { "range for", "var s = 0;\nvar arr = [1, 2, 3];\nfor (auto x : arr) {\n    s += x;\n}\nfor (auto& y : arr) y *= 2;\nvar b = s;\n" },
                { "switch", "var r = 0;\nfor (var i = 0; i < 4; i += 1) {\n    switch (i) {\n        case 0:\n            r += 1;\n        case 1:\n            r += 2;\n            fallthrough;\n        case 2:\n            r += 3;\n        default:\n            r += 4;\n    }\n}\nvar b = r;\n" },
                { "and or chains", "var a = 1;\nvar b = 0;\nfor (var i = 0; i < 3; i += 1) {\n    if (a > 0 && b < 2 || i == 7) {\n        b += 1;\n    }\n    var c = a > 0 && b > 1;\n    if (!(a < 0 || b > 5)) a += 1;\n}\nvar d = b;\n" },
                { "ternary", "var a = 0;\nfor (var i = 0; i < 3; i += 1) {\n    a = i > 1 ? a + 10 : a + 1;\n    var c = (a > 5) ? 1 : 2;\n}\nvar d = a;\n" },
                { "return with expression", "function f(int x) -> int {\n    if (x > 2) {\n        return x * 2;\n    }\n    return x + 1;\n}\nvar s = 0;\nfor (var i = 0; i < 4; i += 1) {\n    s += f(i);\n}\nvar d = s;\n" },
                { "a graft whose body calls", "int far(int x) {\n    return x * 2;\n}\nint near(int x) {\n    if (x > 1) {\n        return far(x);\n    }\n    return x + 1;\n}\nint s = 0;\nfor (int i = 0; i < 4; i++) {\n    s = s + near(i);\n}\nauto d = s + near(5);\n" },
                { "typed calls in a loop", "int add(int a, int b) {\n    return a + b;\n}\nint s = 0;\nfor (int i = 0; i < 4; i++) {\n    s = add(s, i);\n}\nauto d = s;\n" },
                { "void call statement", "var total = 0;\nfunction bump(int by) {\n    total += by;\n}\nfor (var i = 0; i < 3; i += 1) {\n    bump(i);\n}\nvar d = total;\n" },
                { "lambda", "var k = 2;\nauto mul = [k](int x) {\n    return x * k;\n};\nvar s = 0;\nfor (var i = 0; i < 3; i += 1) {\n    s += mul(i);\n}\nvar d = s;\n" },
                { "class method", "class C {\n    int v = 0;\n    void add(int x) {\n        v += x;\n    }\n    int get() { return v; }\n}\nauto c = C();\nfor (var i = 0; i < 3; i += 1) {\n    c.add(i);\n}\nvar d = c.get();\n" },
                { "sibling and field receiver grafts", "class N {\n    int v = 0;\n    int twice() {\n        return v * 2;\n    }\n    void add(int x) {\n        v = v + x;\n    }\n}\nclass W {\n    shared_ptr<N> n = new N();\n    int t = 0;\n    void step(int x) {\n        n.add(x);\n        t = t + n.twice() + more();\n    }\n    int more() {\n        return t + 1;\n    }\n}\nW w = W();\nfor (int i = 0; i < 3; i++) {\n    w.step(i);\n}\nauto d = w.t;\n" },
                { "condition graft", "class N {\n    int v = 0;\n    bool big() {\n        return v > 1;\n    }\n}\nclass W {\n    shared_ptr<N> n = new N();\n    int t = 0;\n    void step(int x) {\n        n.v = x;\n        if (n.big()) {\n            t = t + 1;\n        }\n    }\n}\nW w = W();\nfor (int i = 0; i < 4; i++) {\n    w.step(i);\n}\nauto d = w.t;\n" },
                { "try catch", "var r = 0;\ntry {\n    r = 1;\n    throw \"x\";\n    r = 2;\n} catch (e) {\n    r = 3;\n}\nvar d = r;\n" },
                { "blocks and empty branches", "var a = 1;\n{\n    var b = 2;\n    a += b;\n}\nif (a > 0) { }\nif (a < 0) { } else { a += 1; }\nvar d = a;\n" },
                { "while first in a function", "function drain(int n) -> int {\n    while (n > 0) {\n        n -= 1;\n    }\n    return n;\n}\nvar s = 0;\nfor (var i = 0; i < 3; i += 1) {\n    s += drain(i);\n}\nvar d = s;\n" },
                { "single statement function called twice", "function twice(int x) -> int { return x * 2; }\nvar a = twice(1);\nvar b = twice(a);\nvar c = twice(twice(b));\n" },
                { "recursion", "function fact(int n) -> int {\n    if (n <= 1) return 1;\n    return n * fact(n - 1);\n}\nvar f = fact(4);\n" },
                { "builtin callback", "function g(int x) -> int {\n    return x * 2;\n}\nvar arr = [1, 2, 3];\nvar m = array::map(arr, g);\nvar n = m.size();\n" },
                { "dot builtin callbacks", "function less(int a, int b) -> bool { return a < b; }\narray<int> arr = [3, 1, 2];\narr.sort(less);\nvar t = 0;\narr.for_each([&](int x) {\n    t += x;\n});\narr.sort([](int a, int b) -> bool { return a > b; });\nvar n = arr.filter((x) => x > 1).size();\n" },
                { "nested function and class declarations", "var r = 0;\nfor (var i = 0; i < 2; i += 1) {\n    function inner(int v) -> int { return v + 1; }\n    r = inner(r);\n}\nclass K { int z = 3; }\nauto k = K();\nvar d = r + k.z;\n" },
                { "coroutine", "coroutine gen(int n) -> int {\n    for (int i = 0; i < n; i++) {\n        yield i;\n    }\n}\nvar s = 0;\nfor (auto v : gen(3)) {\n    s += v;\n}\nvar d = s;\n" },
                { "namespace", "namespace ns {\n    int base = 4;\n    int twice(int x) { return x * 2; }\n    class P { int q = 1; }\n    int third = base + 1;\n}\nvar d = ns::twice(ns::base);\n" },
                { "return from inside a loop", "function find(int target) -> int {\n    for (int i = 0; i < 10; i++) {\n        if (i == target) {\n            return i;\n        }\n    }\n    return -1;\n}\nvar a = find(2);\nvar b = find(20);\n" },
                { "throw through a call", "function boom(int x) {\n    if (x > 1) {\n        throw \"x\";\n    }\n}\nvar r = 0;\nfor (var i = 0; i < 3; i += 1) {\n    try {\n        boom(i);\n        r += 1;\n    } catch (e) {\n        r += 10;\n    }\n}\nvar d = r;\n" },
                { "typed cell loop body", "int s = 0;\nint t = 0;\nint i = 0;\nwhile (i < 6) {\n    t = t + 3;\n    s = s + t;\n    i = i + 1;\n}\nauto d = s;\n" },
            };
            std::string mismatches;
            for (const auto& sh : shapes) {
                for (auto how : { drive::into, drive::over, drive::mixed, drive::odd_breakpoints }) {
                    const auto interp = line_list(step_line_sequence(jai::backend_type::interpreter, sh.src, how));
                    const auto vm = line_list(step_line_sequence(jai::backend_type::vm, sh.src, how));
                    if (interp != vm) {
                        mismatches += std::string("\n    ") + sh.name + (how == drive::into ? " (into)" : how == drive::over ? " (over)" : how == drive::mixed ? " (mixed)" : " (breakpoints)")
                            + "\n      interpreter: " + interp + "\n      vm:          " + vm;
                    }
                }
            }
            check_eq(std::string(), mismatches);
        });

        test("native_and_source_breakpoints_with_same_spelling_remain_distinct", [this]() {
            auto eng = make_engine();
            eng->jaibite_cache(false);
            auto& dbg = eng->debugger();
            const std::string raw = "./virtual/../mixed.jai";
            const std::string canonical = std::filesystem::weakly_canonical(raw).string();
            std::vector<jai::debug::stop_info> stops;
            dbg.set_on_stopped([&](const jai::debug::stop_info& info) {
                stops.push_back(info);
                dbg.resume();
            });
            dbg.set_breakpoints(raw, {1});
            set_source_breakpoints_for_test(dbg, raw, {2});
            dbg.set_enabled(true);
            eng->execute_file_source(raw, "1;\n2;\n");
            eng->execute_file_source(canonical, "1;\n2;\n");
            dbg.set_enabled(false);
            check_eq(size_t(2), stops.size());
            check_eq(raw, stops[0].file);
            check_eq(1, stops[0].line);
            check_eq(canonical, stops[1].file);
            check_eq(2, stops[1].line);
        });

        test("execute_file_attributes_the_canonical_path_so_source_breakpoints_bind", [this]() {
            auto dir = jai::foundry::test_temp_directory() / "execute_file_canonical";
            std::filesystem::create_directories(dir);
            { std::ofstream(dir / "mixed.jai") << "var a = 1;\nvar b = 2;\n"; }
            const std::string canonical = std::filesystem::canonical(dir / "mixed.jai").string();
            const std::string mixed = dir.generic_string() + "/./" + "mixed.jai";   // forward slashes and a dot segment
            auto eng = make_engine();
            eng->jaibite_cache(false);
            auto& dbg = eng->debugger();
            std::vector<jai::debug::stop_info> stops;
            dbg.set_on_stopped([&](const jai::debug::stop_info& info) {
                stops.push_back(info);
                dbg.resume();
            });
            set_source_breakpoints_for_test(dbg, canonical, {2});
            dbg.set_enabled(true);
            eng->execute_file(mixed);
            dbg.set_enabled(false);
            check_eq(size_t(1), stops.size(), "a breakpoint set on the canonical path binds a file run through another spelling");
            if (!stops.empty()) { check_eq(canonical, stops[0].file); }
            { std::ofstream(dir / "throws.jai") << "function f() { throw \"x\"; }\nf();\n"; }
            try {
                eng->execute_file(dir.generic_string() + "/./throws.jai");
            } catch (...) {}
            auto trace = eng->last_stack_trace();
            check_false(trace.empty());
            if (!trace.empty()) { check_eq(std::filesystem::canonical(dir / "throws.jai").string(), trace[0].file); }
        });

        test("native_breakpoint_labels_survive_disable_and_rearm", [this]() {
            auto eng = make_engine();
            eng->jaibite_cache(false);
            auto& dbg = eng->debugger();
            const std::string first = "./virtual/../first.jai";
            const std::string second = "./virtual/../second.jai";
            std::vector<jai::debug::stop_info> stops;
            dbg.set_on_stopped([&](const jai::debug::stop_info& info) {
                stops.push_back(info);
                dbg.resume();
            });
            dbg.set_breakpoints(first, {1});
            dbg.set_enabled(true);
            eng->execute_file_source(first, "1;\n2;\n");
            check_eq(size_t(1), stops.size());

            dbg.set_enabled(false);
            check_false(dbg.sync_hot_state());
            dbg.set_breakpoints(second, {2});
            check_false(dbg.sync_hot_state());
            dbg.set_enabled(true);
            eng->execute_file_source(first, "1;\n2;\n");
            eng->execute_file_source(second, "1;\n2;\n");
            check_eq(size_t(3), stops.size(), "rearm must retain installed and queued breakpoints");
            check_eq(first, stops[1].file, "native breakpoint labels are exact, never filesystem-normalized");
            check_eq(second, stops[2].file);
            check_eq(2, stops[2].line);

            dbg.set_breakpoints(first, {2});
            dbg.clear_breakpoints();
            dbg.set_breakpoints(second, {1});
            eng->execute_file_source(first, "1;\n2;\n");
            eng->execute_file_source(second, "1;\n2;\n");
            check_eq(size_t(4), stops.size(), "clear-all must preserve its order among queued updates");
            check_eq(second, stops[3].file);
            check_eq(1, stops[3].line);
            dbg.set_enabled(false);
        });

        test("breakpoint_edit_while_paused_applies_at_park_exit", [this]() {
            auto eng = make_engine();
            auto& dbg = eng->debugger();
            std::vector<int> lines;
            dbg.set_on_stopped([&](const jai::debug::stop_info& info) {
                lines.push_back(info.line);
                if (lines.size() == 1) dbg.set_breakpoints("<script>", {2});
                dbg.resume();
            });
            dbg.set_breakpoints("<script>", {1});
            dbg.set_enabled(true);
            eng->execute("1;\n2;\n3;\n");
            dbg.set_enabled(false);
            check_eq(size_t(2), lines.size());
            check_eq(1, lines[0]);
            check_eq(2, lines[1]);
        });

        test("breakpoint_inspect_step_resume", [this]() {
            breakpoint_inspect_step_resume(jai::backend_type::interpreter);
        });
        test("vm_breakpoint_inspect_step_resume", [this]() {
            breakpoint_inspect_step_resume(jai::backend_type::vm);
        });
        test("loop_breakpoint_hits_each_iteration", [this]() {
            loop_breakpoint_hits_each_iteration(jai::backend_type::interpreter);
        });
        test("vm_loop_breakpoint_hits_each_iteration", [this]() {
            loop_breakpoint_hits_each_iteration(jai::backend_type::vm);
        });
        test("captured_names_at_a_closure_stop_read_alike_on_both_backends", [this]() {
            const std::string interp = captured_names_at_a_closure_stop(jai::backend_type::interpreter);
            const std::string vm = captured_names_at_a_closure_stop(jai::backend_type::vm);
            check_eq(interp, vm, "the vm's converted closure reads and sets its captured names as the interpreter's environment does");
            check_true(interp.find("r=5 c=2 s=10") != std::string::npos, interp);
        });
        test("frame_locals_at_breakpoint", [this]() {
            frame_locals_at_breakpoint(jai::backend_type::interpreter);
        });
        test("vm_frame_locals_at_breakpoint", [this]() {
            frame_locals_at_breakpoint(jai::backend_type::vm);
        });
        test("frame_locals_name_bare_compound_rhs", [this]() {
            frame_locals_name_bare_compound_rhs(jai::backend_type::interpreter);
        });
        test("vm_frame_locals_name_bare_compound_rhs", [this]() {
            frame_locals_name_bare_compound_rhs(jai::backend_type::vm);
        });
        test("sleep_paced_loop_stops_promptly", [this]() {
            sleep_paced_loop_stops_promptly(jai::backend_type::interpreter);
        });
        test("vm_sleep_paced_loop_stops_promptly", [this]() {
            sleep_paced_loop_stops_promptly(jai::backend_type::vm);
        });
        test("pause_wakes_a_sleeping_script", [this]() {
            pause_wakes_a_sleeping_script(jai::backend_type::interpreter);
        });
        test("vm_pause_wakes_a_sleeping_script", [this]() {
            pause_wakes_a_sleeping_script(jai::backend_type::vm);
        });
        test("sleep_zero_is_a_sync_point", [this]() {
            sleep_zero_is_a_sync_point(jai::backend_type::interpreter);
        });
        test("vm_sleep_zero_is_a_sync_point", [this]() {
            sleep_zero_is_a_sync_point(jai::backend_type::vm);
        });
        test("stack_trace_lists_every_frame_with_its_locals", [this]() {
            stack_trace_lists_every_frame_with_its_locals(jai::backend_type::interpreter);
        });
        test("vm_stack_trace_lists_every_frame_with_its_locals", [this]() {
            stack_trace_lists_every_frame_with_its_locals(jai::backend_type::vm);
        });
        test("a_construction_stops_in_each_level_with_its_frames", [this]() {
            // each constructor's activation is a frame with its parameters, the level answering at its argument's or field's line
            const std::string levels = "hook:2(v) B:12(y) <script>:16() | hook:2(v) A:5(x) B:12(y) <script>:16() | A:7(x) B:12(y) <script>:16() | "
                                       "hook:2(v) B:11(y) <script>:16() | B:13(y) <script>:16() | ";
            check_eq(levels, construction_stops(jai::backend_type::interpreter), "interpreter");
            check_eq(levels, construction_stops(jai::backend_type::vm), "vm");
        });
        test("locals_follow_scope_and_reads_resolve_through_it_alike_on_both_backends", [this]() {
            const std::string interp = scoped_locals(jai::backend_type::interpreter);
            check_eq(interp, scoped_locals(jai::backend_type::vm), "the vm's locals table answers as the interpreter's scopes do");
            check_eq(std::string("f:9(v=2,x=3) <script>:15() globals(g,) v=2 x=3 g=5 set v=20 | "
                                    "take:4(t=Token{n: \"tk\"}) f:12(v=1,x=3) <script>:15() globals(g,) t.n=\"tk\":string t.m=error: 't' has no field 'm' | "
                                    "f:13(s=\"tk\",v=1,x=3) <script>:15() globals(g,) set v=7 nope=error: No variable 'nope' is in scope at this stop | "
                                    "r=7 g=21 nope=error"), interp);
        });
        test("keyword_named_members_in_frames_and_locals", [this]() {
            keyword_named_members_in_frames_and_locals(jai::backend_type::interpreter);
        });
        test("vm_keyword_named_members_in_frames_and_locals", [this]() {
            keyword_named_members_in_frames_and_locals(jai::backend_type::vm);
        });
        test("parallel_region_atomic_to_debugger", [this]() {
            parallel_region_atomic_to_debugger(jai::backend_type::interpreter);
        });
        test("vm_parallel_region_atomic_to_debugger", [this]() {
            parallel_region_atomic_to_debugger(jai::backend_type::vm);
        });

        test("detach_wakes_a_parked_thread", [this]() {
            // A client disconnect / shutdown must force-resume a parked script, never hang.
            auto eng = jai::foundry::make_engine();
            auto& dbg = eng->debugger();

            std::mutex m;
            std::condition_variable cv;
            bool stopped = false;
            dbg.set_on_stopped([&](const jai::debug::stop_info&) {
                std::lock_guard<std::mutex> lk(m);
                stopped = true;
                cv.notify_all();
            });

            dbg.set_breakpoints("<script>", {1});
            dbg.set_enabled(true);

            std::atomic<bool> done{false};
            std::thread script([&] { eng->execute("var a = 1;\nvar b = 2;\n"); done = true; });

            {
                std::unique_lock<std::mutex> lk(m);
                check_true(cv.wait_for(lk, kHangGuard, [&] { return stopped; }));
            }
            check_true(dbg.is_paused());

            dbg.detach();                 // no resume() — detach must unblock it
            script.join();
            check_true(done.load());
        });

        test("a_park_only_command_after_the_resume_is_refused_not_kept_for_the_next_stop", [this]() {
            // The paused test and the queueing are one step under the park's lock: a command posted once the script thread has
            // left its park is refused (the connector answers "not stopped") instead of running at a later stop against it
            auto eng = jai::foundry::make_engine();
            auto& dbg = eng->debugger();
            std::mutex m;
            std::condition_variable cv;
            int stops = 0;
            bool between = false;
            bool posted = false;
            dbg.set_on_stopped([&](const jai::debug::stop_info&) { std::lock_guard<std::mutex> lk(m); ++stops; cv.notify_all(); });
            // the script holds between its stops until the late command is posted, so a loaded machine cannot miss the window
            eng->add_function("between_stops", [&] {
                std::unique_lock<std::mutex> lk(m);
                between = true;
                cv.notify_all();
                cv.wait_for(lk, kHangGuard, [&] { return posted; });
            });
            dbg.set_breakpoints("<script>", {1, 3});
            dbg.set_enabled(true);
            std::thread script([&] { eng->execute("var a = 1;\nbetween_stops();\nvar c = 3;\n"); });
            const auto wait_stops = [&](int n) {
                std::unique_lock<std::mutex> lk(m);
                return cv.wait_for(lk, kHangGuard, [&] { return stops >= n && dbg.is_paused(); });
            };
            check_true(wait_stops(1));
            int ran = 0;
            check_true(dbg.post_command_while_paused([&] { ++ran; }), "a parked thread takes the command");
            dbg.resume();
            bool left_park = false;
            {
                std::unique_lock<std::mutex> lk(m);
                left_park = cv.wait_for(lk, kHangGuard, [&] { return between; });
            }
            const bool stale = dbg.post_command_while_paused([&] { ran += 100; });
            {
                std::lock_guard<std::mutex> lk(m);
                posted = true;
            }
            cv.notify_all();
            check_true(left_park, "the script left its first park");
            check_true(wait_stops(2));
            dbg.set_enabled(false);
            script.join();
            check_false(stale, "a command posted after the park ended is refused");
            check_eq(1, ran, "only the command posted while parked ran");
        });

        test("a_detach_racing_a_stop_never_strands_the_script", [this]() {
            // A session that ends while the script thread is between its stop test and its park must not leave it parked: the park
            // used to clear the resume the detach had just requested
            auto eng = jai::foundry::make_engine();
            eng->execution_budget(0);
            auto& dbg = eng->debugger();
            std::atomic<size_t> stops{0};
            dbg.set_on_stopped([&stops](const jai::debug::stop_info&) { ++stops; });
            std::atomic<bool> finish{false};
            std::atomic<size_t> loop_heads{0};
            eng->add_function("keep_going", [&finish, &loop_heads]() { ++loop_heads; return !finish.load(); });
            std::thread script([&] { eng->execute("while (keep_going()) { sleep(0.00001); }"); });
            // once a session ends the script reaches two more loop heads (the first may have begun before the end), so no park
            // outlived it; a script that reaches none for the hang guard is parked for a session that is gone
            const auto runs_on = [&] {
                const size_t target = loop_heads.load() + 2;
                size_t seen = loop_heads.load();
                auto progressed_at = std::chrono::steady_clock::now();
                while (loop_heads.load() < target) {
                    const size_t now_seen = loop_heads.load();
                    const auto now = std::chrono::steady_clock::now();
                    if (now_seen != seen) {
                        seen = now_seen;
                        progressed_at = now;
                    } else if (now - progressed_at > kHangGuard) {
                        return false;
                    }
                    std::this_thread::yield();
                }
                return true;
            };
            size_t stranded = 0;
            std::mt19937 rng(7);
            const size_t rounds = 1000;
            for (size_t round = 0; round < rounds; ++round) {
                dbg.set_enabled(true);
                dbg.request_pause();
                const int spins = static_cast<int>(rng() % 200000);
                for (volatile int spin = 0; spin < spins; ++spin) {}
                dbg.set_enabled(false);
                if (!runs_on()) {
                    ++stranded;
                    dbg.resume();
                }
            }
            // a session that waits for its pause sees the script stop, then run on once it ends
            dbg.set_enabled(true);
            const size_t raced_stops = stops.load();
            dbg.request_pause();
            {
                size_t seen = loop_heads.load();
                auto progressed_at = std::chrono::steady_clock::now();
                while (stops.load() == raced_stops) {
                    const auto now = std::chrono::steady_clock::now();
                    if (loop_heads.load() != seen) {
                        seen = loop_heads.load();
                        progressed_at = now;
                    } else if (now - progressed_at > kHangGuard) {
                        break;
                    }
                    std::this_thread::yield();
                }
            }
            const bool waited_stop = stops.load() > raced_stops;
            dbg.set_enabled(false);
            const bool ran_after_waited_stop = runs_on();
            finish = true;
            dbg.detach();
            script.join();
            check_eq(size_t(0), stranded, "a parked thread outlived the session that stopped it (" + std::to_string(raced_stops) + " stops in " +
                std::to_string(rounds) + " rounds)");
            check_true(waited_stop, "the requested pause stopped the script");
            check_true(ran_after_waited_stop, "the script ran on after the session that stopped it ended");
        });

        // ---- cost-model benchmarks (verbose runs only; non-asserting) ----
        // The same script as Performance Benchmarks' "Hot Loop (1000 iterations)", run
        // under each debugger configuration from docs/site/embedding/11-performance.html
        // "What the debugger costs". The loop-body statement sits on line 4 of the source.
        static constexpr const char* hot_loop_src = R"(
                auto sum = 0;
                for (auto i = 0; i < 1000; i += 1) {
                    sum += i * 2;
                }
            )";

        benchmark("Hot Loop: no debugger constructed", [this]() {
            if (!bench_eng_) bench_eng_ = jai::foundry::make_engine();
            bench_eng_->execute(hot_loop_src);
        });

        benchmark("Hot Loop: controller constructed, no session", [this]() {
            if (!bench_eng_) {
                bench_eng_ = jai::foundry::make_engine();
                bench_eng_->debugger();   // = connector listening, no client attached
            }
            bench_eng_->execute(hot_loop_src);
        });

        benchmark("Hot Loop: session enabled, no breakpoints", [this]() {
            if (!bench_eng_) {
                bench_eng_ = jai::foundry::make_engine();
                bench_eng_->debugger().set_enabled(true);
            }
            bench_eng_->execute(hot_loop_src);
        });

        benchmark("Hot Loop: session enabled, bp in cold file", [this]() {
            if (!bench_eng_) {
                bench_eng_ = jai::foundry::make_engine();
                auto& dbg = bench_eng_->debugger();
                dbg.set_breakpoints("cold.jai", {50});   // no line in the script matches
                dbg.set_enabled(true);
            }
            bench_eng_->execute(hot_loop_src);
        });

        benchmark("Hot Loop: session enabled, bp line collides", [this]() {
            if (!bench_eng_) {
                bench_eng_ = jai::foundry::make_engine();
                auto& dbg = bench_eng_->debugger();
                dbg.set_breakpoints("cold.jai", {4});    // same line as the hot statement
                dbg.set_enabled(true);
            }
            bench_eng_->execute(hot_loop_src);
        });

        // Same 5 configurations on the vm backend (the per-dispatch gate must hold the
        // vm's hot-loop band).
        benchmark("Hot Loop [vm]: no debugger constructed", [this]() {
            if (!bench_eng_) bench_eng_ = make_backend_engine(jai::backend_type::vm);
            bench_eng_->execute(hot_loop_src);
        });

        benchmark("Hot Loop [vm]: controller constructed, no session", [this]() {
            if (!bench_eng_) {
                bench_eng_ = make_backend_engine(jai::backend_type::vm);
                bench_eng_->debugger();
            }
            bench_eng_->execute(hot_loop_src);
        });

        benchmark("Hot Loop [vm]: session enabled, no breakpoints", [this]() {
            if (!bench_eng_) {
                bench_eng_ = make_backend_engine(jai::backend_type::vm);
                bench_eng_->debugger().set_enabled(true);
            }
            bench_eng_->execute(hot_loop_src);
        });

        benchmark("Hot Loop [vm]: session enabled, bp in cold file", [this]() {
            if (!bench_eng_) {
                bench_eng_ = make_backend_engine(jai::backend_type::vm);
                auto& dbg = bench_eng_->debugger();
                dbg.set_breakpoints("cold.jai", {50});
                dbg.set_enabled(true);
            }
            bench_eng_->execute(hot_loop_src);
        });

        benchmark("Hot Loop [vm]: session enabled, bp line collides", [this]() {
            if (!bench_eng_) {
                bench_eng_ = make_backend_engine(jai::backend_type::vm);
                auto& dbg = bench_eng_->debugger();
                dbg.set_breakpoints("cold.jai", {4});
                dbg.set_enabled(true);
            }
            bench_eng_->execute(hot_loop_src);
        });
    }
};

} // namespace jai::foundry::tests

using debugger_tests = jai::foundry::tests::debugger_tests;
FOUNDRY_REGISTER(debugger_tests)
