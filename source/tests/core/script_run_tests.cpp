#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/engine_time.hpp>
#include <jaiscript/core/script_run.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <chrono>
#include <climits>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

namespace {
    using ns = std::chrono::nanoseconds;

    // A virtual clock the host drives: a run's sleep reads it, a blocking sleep advances it (and is counted)
    struct sim_clock {
        ns now = std::chrono::seconds(10);
        int reads = 0;
        std::vector<ns> blocking_sleeps;
        void advance(double seconds) { now += std::chrono::duration_cast<ns>(std::chrono::duration<double>(seconds)); }
    };

    struct run_host {
        std::shared_ptr<engine> eng;
        std::shared_ptr<std::ostringstream> out = std::make_shared<std::ostringstream>();
        std::vector<std::string> warnings;
        sim_clock clock;

        run_host() {
            eng = make_engine();
            eng->execution_budget(0);
            stdlib::register_all(eng);
            eng->set_output_stream(out);
            eng->set_script_warning_handler([this](const std::string& w) { warnings.push_back(w); });
            eng->time().source([this] { ++clock.reads; return clock.now; },
                               [this] { return ns(1700000000000000000LL) + clock.now; },
                               [this](ns d) { clock.blocking_sleeps.push_back(d); clock.now += d; });
        }
        std::string take() {
            std::string s = out->str();
            out->str("");
            return s;
        }
        // resumes until done, advancing the clock to each wake time
        void finish(script_run& run) {
            while (!run.done()) {
                if (run.wake_at() > clock.now) { clock.now = run.wake_at(); }
                run.resume();
            }
        }
    };

    std::string backend_label(const engine& e) { return e.get_backend_type() == backend_type::vm ? " (vm)" : " (interpreter)"; }
}

class script_run_tests : public suite {
public:
    script_run_tests() : suite("Script Run") {}

    void forge_tests() override {
        test("a_run_suspends_at_sleep_and_resumes_there", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            script_run run = h.eng->start(R"(
                int total = 1;
                print("a");
                sleep(1.0);
                total += 10;
                print("b");
                sleep(0.5);
                print("c");
                total + 31;)");
            check_true(run.state() == script_run::status::suspended, "suspended at the first sleep" + be);
            check_eq(std::string("a\n"), h.take(), "ran to the sleep" + be);
            check_eq((int64_t)(h.clock.now + std::chrono::seconds(1)).count(), (int64_t)run.wake_at().count(), "wake time on the time source" + be);
            check_true(h.clock.blocking_sleeps.empty(), "the sleep did not block" + be);
            check_eq(std::string("1"), h.eng->execute("total;").to_string(), "the host's own execute runs while the run waits" + be);
            const int reads = h.clock.reads;
            check_true(run.resume() == script_run::status::suspended, "not due: still suspended" + be);
            check_eq(reads + 1, h.clock.reads, "a resume before the wake time reads the clock and switches nothing" + be);
            check_eq(std::string(), h.take(), "nothing ran" + be);
            h.clock.advance(1.0);
            check_true(run.resume() == script_run::status::suspended, "the second sleep" + be);
            check_eq(std::string("b\n"), h.take(), "resumed after the first sleep" + be);
            h.clock.advance(0.5);
            check_true(run.resume() == script_run::status::completed, "completed" + be);
            check_eq(std::string("c\n"), h.take(), "ran to the end" + be);
            check_true(run.done() && run.result().has_value(), "a value" + be);
            check_eq(std::string("42"), run.result().value().to_string(), "the program's value" + be);
            check_eq((int64_t)0, (int64_t)run.wake_at().count(), "a finished run has no wake time" + be);
        });

        test("zero_and_negative_sleeps_return_at_once_and_execute_blocks", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            script_run run = h.eng->start("sleep(0); sleep(-1.0); sleep(0.0); print(\"x\");");
            check_true(run.state() == script_run::status::completed, "no suspension" + be);
            h.eng->execute("sleep(0.25); sleep(1);");
            check_eq((size_t)2, h.clock.blocking_sleeps.size(), "execute() blocks on the time source" + be);
            check_true(h.warnings.empty(), "a plain execute warns nothing" + be);
        });

        test("several_runs_interleave_in_any_order", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            auto make = [&](const std::string& name, double step) {
                return h.eng->start("void worker_" + name + "() { int n = 0; for (int i = 0; i < 3; ++i) { n += 1; print(\"" + name +
                                    "{}\", n); sleep(" + std::to_string(step) + "); } }\nworker_" + name + "();");
            };
            script_run a = make("a", 1.0);
            script_run b = make("b", 0.25);
            script_run c = make("c", 0.5);
            check_eq(std::string("a1\nb1\nc1\n"), h.take(), "each ran to its first sleep" + be);
            h.clock.advance(1.0);
            c.resume();
            a.resume();
            b.resume();
            check_eq(std::string("c2\na2\nb2\n"), h.take(), "resumed in another order, each with its own locals" + be);
            h.eng->execute("print(\"host\");");
            h.clock.advance(0.25);
            b.resume();
            h.finish(c);
            h.finish(a);
            h.finish(b);
            check_eq(std::string("host\nb3\nc3\na3\n"), h.take(), "every run finished" + be);
            check_true(a.state() == script_run::status::completed && b.state() == script_run::status::completed &&
                       c.state() == script_run::status::completed, "all completed" + be);
        });

        test("sleeps_in_script_calls_suspend_the_run", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            script_run run = h.eng->start(R"(
                class V {
                    int x = 0;
                    V(int a) { x = a; sleep(0.1); }
                    V operator+(V o) { sleep(0.1); return V(x + o.x); }
                    int get_twice() { sleep(0.1); return x * 2; }
                    static int make() { sleep(0.1); return 7; }
                }
                int deep(int n) { if (n == 0) { sleep(0.1); return 0; } return deep(n - 1) + 1; }
                int with_default(int a = deep(3)) { return a; }
                namespace util { int nap() { sleep(0.1); return 5; } }
                var lambda = [](int v) { sleep(0.1); return v + 1; };
                map<string, var> table = { "f": lambda };
                V v = V(2) + V(3);
                var method = reflect::bind(v, "get_twice");
                int r = deep(40) + with_default() + lambda(1) + table["f"](2) + v.get_twice() + method() + V::make() + util::nap();
                r;)");
            int suspensions = 0;
            while (!run.done()) {
                check_true(run.state() == script_run::status::suspended, "each sleep suspends" + be);
                ++suspensions;
                h.clock.advance(0.1);
                run.resume();
            }
            check_true(h.warnings.empty(), "no script call pins the run" + be + (h.warnings.empty() ? std::string() : ": " + h.warnings[0]));
            check_true(h.clock.blocking_sleeps.empty(), "nothing blocked" + be);
            check_eq(12, suspensions, "every sleep suspended" + be);
            check_eq(std::string("80"), run.result().value().to_string(), "the value" + be);
        });

        test("sleeps_in_construction_suspend_the_run", [this]() {
            // every constructor of a chain runs in the run: an initializer-list argument, a field initializer, a base's body, a
            // delegated-to constructor and a class without a constructor each suspend it
            run_host h;
            const std::string be = backend_label(*h.eng);
            script_run run = h.eng->start(R"(
                int pause(int v) { sleep(0.1); return v; }
                class A { int a = pause(1); A(int x) { sleep(0.1); a += x; } }
                class B : A { int b = pause(2); B(int y) : super(pause(y)) { sleep(0.1); } B() : this(pause(3)) {} }
                class C { int c = pause(4); }
                class D : C {}
                B made = B();
                D plain = D();
                to_string(made.a) + " " + to_string(made.b) + " " + to_string(plain.c);)");
            int suspensions = 0;
            while (!run.done()) {
                check_true(run.state() == script_run::status::suspended, "each sleep suspends" + be);
                ++suspensions;
                h.clock.advance(0.1);
                run.resume();
            }
            check_true(h.warnings.empty(), "no construction pins the run" + be + (h.warnings.empty() ? std::string() : ": " + h.warnings[0]));
            check_true(h.clock.blocking_sleeps.empty(), "nothing blocked" + be);
            check_eq(7, suspensions, "every sleep suspended" + be);
            check_eq(std::string("4 2 4"), run.result().value().to_string(), "the value" + be);
        });

        test("runs_suspended_mid_construction_interleave", [this]() {
            // each run's constructions are its own: one suspended inside a base's constructor, its field initializer or its
            // initializer-list argument while another constructs (and suspends) meanwhile, a throw destroying only what its own
            // construction finished
            run_host h;
            const std::string be = backend_label(*h.eng);
            h.eng->execute(R"(
                string log = "";
                int pause(int v) { sleep(0.1); return v; }
                class A { int a = pause(1); A(int x) { sleep(0.1); a += x; } ~A() { log += "~A" + to_string(a); } }
                class B : A { int b = pause(2); B(int y) : super(pause(y)) { sleep(0.1); b += y; } }
                class Z { Z() { throw "z"; } }
                class C : B, Z { int c = 0; C() : super(pause(5)), Z() { c = a + b; } }
                class P { int p = 0; P() { sleep(0.1); sleep(0.1); p = 8; } ~P() { log += "~P"; } }
                class Q : P { int q = 9; Q() : super() {} })");
            script_run first = h.eng->start("string r = \"\"; try { C x = C(); } catch (e) { r = e; } r + \" \" + log;");
            script_run second = h.eng->start("Q y = Q(); to_string(y.p) + \" \" + to_string(y.q);");
            int steps = 0;
            while ((!first.done() || !second.done()) && steps < 64) {
                h.clock.advance(0.1);
                if (!first.done()) { first.resume(); }
                if (!second.done()) { second.resume(); }
                ++steps;
            }
            check_true(first.state() == script_run::status::completed && second.state() == script_run::status::completed, "both completed" + be);
            check_true(h.clock.blocking_sleeps.empty(), "nothing blocked" + be);
            if (first.state() == script_run::status::completed) { check_eq(std::string("z ~A6"), first.result().value().to_string(), "the first" + be); }
            if (second.state() == script_run::status::completed) { check_eq(std::string("8 9"), second.result().value().to_string(), "the second" + be); }
        });

        test("pinned_sleeps_block_and_warn_once_per_run", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            engine* raw = h.eng.get();
            h.eng->add_function("call_back", [raw](const script_value& fn) { auto r = fn.as_function()({}); (void)r; });
            h.eng->add_function("call_converted", [](std::function<void()> fn) { fn(); });
            script_run run = h.eng->start(R"(
                class D { ~D() { sleep(0.5); } }
                { D d; }
                call_back([]() { sleep(0.5); });
                call_converted([]() { sleep(0.5); });
                array<int> xs = [3, 1, 2];
                xs.sort([](int a, int b) { sleep(0.5); return a < b; });
                print("{}", xs[0]);)");
            check_true(run.state() == script_run::status::completed, "nothing suspended" + be);
            check_true(h.clock.blocking_sleeps.size() >= 4, "each pinned sleep blocked" + be);
            check_eq((size_t)1, h.warnings.size(), "one warning per run" + be);
            if (!h.warnings.empty()) {
                check_eq(std::string("sleep blocks here: a run suspends only where no host callback, destructor, signal receiver, include, import or hot_reload_migrate is running"),
                         h.warnings[0], "the warning's text" + be);
            }
            script_run second = h.eng->start("class E { ~E() { sleep(0.1); } } { E e; } sleep(1.0); print(\"after\");");
            check_true(second.state() == script_run::status::suspended, "the unpinned sleep after the destructor suspends" + be);
            check_eq((size_t)2, h.warnings.size(), "another run warns once of its own" + be);
            h.finish(second);
        });

        test("an_import_pins_its_module_body", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            const auto dir = test_temp_directory() / ("script_run_import" + std::string(h.eng->get_backend_type() == backend_type::vm ? "_vm" : "_interp"));
            std::filesystem::create_directories(dir);
            { std::ofstream(dir / "napper.jai") << "sleep(0.5);\nint napped = 1;\n"; }
            h.eng->add_include_path(dir.string());
            script_run run = h.eng->start("import(\"napper.jai\"); sleep(1.0); napped;");
            check_true(run.state() == script_run::status::suspended, "the program's own sleep suspends" + be);
            check_eq((size_t)1, h.clock.blocking_sleeps.size(), "the module's sleep blocked" + be);
            check_eq((size_t)1, h.warnings.size(), "and warned" + be);
            h.finish(run);
            check_eq(std::string("1"), run.result().value().to_string(), "completed" + be);
        });

        test("a_sleep_in_a_coroutine_body_suspends_the_whole_run", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            script_run run = h.eng->start(R"(
                coroutine int counter() { for (int i = 1; i <= 3; ++i) { sleep(1.0); yield i; } }
                auto g = counter();
                int sum = 0;
                for (int i = 0; i < 3; ++i) { sum += g.next(); print("got {}", sum); }
                sum;)");
            check_true(run.state() == script_run::status::suspended, "suspended inside next()" + be);
            check_eq(std::string(), h.take(), "the caller of next() waits with it" + be);
            check_eq(std::string("Coroutine is already running"),
                     h.eng->execute("var m = \"\"; try { g.next(); } catch (e) { m = e; } m;").to_string(), "another next() meanwhile" + be);
            h.finish(run);
            check_eq(std::string("got 1\ngot 3\ngot 6\n"), h.take(), "the coroutine's values" + be);
            check_eq(std::string("6"), run.result().value().to_string(), "the value" + be);
        });

        test("errors_and_throws_after_a_resume", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            script_run caught = h.eng->start("var m = \"\"; try { sleep(1.0); throw \"late\"; } catch (e) { m = e; } print(m);");
            h.finish(caught);
            check_eq(std::string("late\n"), h.take(), "a catch around the sleep sees the throw" + be);
            script_run failing = h.eng->start("sleep(1.0);\nthrow \"escaped\";");
            h.clock.advance(1.0);
            bool threw = false;
            try { failing.resume(); } catch (const std::exception& e) {
                threw = true;
                check_true(std::string(e.what()).find("escaped") != std::string::npos, std::string("the throw leaves the resume: ") + e.what() + be);
            }
            check_true(threw, "a source run's failure throws from the resume, as execute() would" + be);
            check_true(failing.state() == script_run::status::failed, "failed" + be);
            check_true(!failing.result().has_value(), "the result holds the error" + be);
            check_true(h.eng->format_error(failing.result()).find("escaped") != std::string::npos, "with its text" + be);
            check_eq(std::string("2"), h.eng->execute("1 + 1;").to_string(), "the engine is ready for the next entry" + be);
        });

        test("each_resume_is_a_host_entry_with_a_fresh_budget", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            h.eng->execution_step_budget(6);
            const std::string segment = "for (int i = 0; i < 4000; ++i) { spin += i; }";
            script_run run = h.eng->start("int spin = 0; " + segment + " sleep(1.0); " + segment + " sleep(1.0); " + segment + " spin;");
            h.finish(run);
            check_true(run.state() == script_run::status::completed, "no segment exceeds the budget alone" + be);
            bool overran = false;
            try { h.eng->execute("int spin2 = 0; " + segment + " " + segment + " " + segment + " " + segment + " spin2;"); } catch (const std::exception&) { overran = true; }
            check_true(overran, "the same work in one entry exceeds it" + be);
            h.eng->execution_step_budget(0);
            script_run elapsed = h.eng->start("sleep(1.0); time::execute_elapsed();");
            h.clock.advance(1.5);
            elapsed.resume();
            check_eq(std::string("1.5"), elapsed.result().value().to_string(), "execute_elapsed counts from the start of the run" + be);
        });

        test("cancel_skips_catches_and_runs_destructors", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            script_run run = h.eng->start(R"(
                class D { string name; D(string n) { name = n; } ~D() { print("dtor {}", name); } }
                void inner() { D b = D("b"); try { sleep(1.0); } catch (e) { print("caught {}", e); } print("never"); }
                D a = D("a");
                { D c = D("c"); inner(); })");
            check_true(run.state() == script_run::status::suspended, "suspended" + be);
            run.cancel();
            check_true(run.state() == script_run::status::failed, "a cancelled run fails" + be);
            check_eq(std::string("dtor b\ndtor c\n"), h.take(), "the frames unwound through their destructors, no catch ran" + be);
            check_eq(std::string("Run cancelled"), h.eng->format_error(run.result()), "the terminal error" + be);
            run.cancel();
            {
                script_run dropped = h.eng->start("{ D d = D(\"dropped\"); sleep(5.0); }");
            }
            check_eq(std::string("dtor dropped\n"), h.take(), "a run dropped while suspended is cancelled" + be);
        });

        test("a_suspended_frame_finishes_its_old_body_after_a_reload", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            script_run run = h.eng->start(R"(
                int step() { return 1; }
                void worker() { print("old before"); sleep(1.0); print("old after"); }
                worker();
                for (int i = 0; i < 2; ++i) { print("step {}", step()); sleep(1.0); })");
            check_eq(std::string("old before\n"), h.take(), "the worker sleeps" + be);
            h.eng->execute("int step() { return 2; }\nvoid worker() { print(\"new\"); }");
            h.finish(run);
            check_eq(std::string("old after\nstep 2\nstep 2\n"), h.take(), "the suspended body ran on, later calls took the new definitions" + be);
        });

        test("a_run_resumes_only_on_its_thread", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            script_run run = h.eng->start("sleep(1.0); 5;");
            h.clock.advance(1.0);
            std::string message;
            std::thread other([&] {
                try { run.resume(); } catch (const std::exception& e) { message = e.what(); }
            });
            other.join();
            check_eq(std::string("A run resumes only on the thread that started it"), message, "refused" + be);
            check_true(run.resume() == script_run::status::completed, "the starting thread resumes it" + be);
        });

        test("an_engine_torn_down_cancels_its_runs", [this]() {
            auto out = std::make_shared<std::ostringstream>();
            script_run survivor;
            {
                auto e = make_engine();
                stdlib::register_all(e);
                e->set_output_stream(out);
                survivor = e->start("class D { ~D() { print(\"dtor\"); } } { D d; sleep(1.0); print(\"never\"); }");
                check_true(survivor.state() == script_run::status::suspended, "suspended");
                script_run second = e->start("sleep(2.0);");
                (void)second;
            }
            check_eq(std::string("dtor\n"), out->str(), "the teardown unwound the run");
            check_true(survivor.state() == script_run::status::failed, "the run outlives its engine as failed");
            script_run moved = std::move(survivor);
            check_true(moved.done(), "and stays done");
        });

        test("deep_recursion_in_a_run_meets_the_same_guard", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            h.eng->max_call_depth(backend_type::interpreter, INT_MAX - 1);   // the native-stack guard stops it
            h.eng->max_call_depth(backend_type::vm, 200000);
            const std::string src = "int deepest = 0;\nvoid r(int n) { deepest = n; r(n + 1); }\nvar m = \"\";\ntry { r(0); } catch (e) { m = e; }\n`${m}|${deepest}`;";
            const std::string direct = h.eng->execute(src).to_string();
            script_run run = h.eng->start(src);
            check_true(run.state() == script_run::status::completed, "completed" + be);
            const std::string in_run = run.result().value().to_string();
            const auto text = [](const std::string& s) { return s.substr(0, s.find('|')); };
            const auto depth = [](const std::string& s) { return std::stoll(s.substr(s.find('|') + 1)); };
            check_eq(text(direct), text(in_run), "the same error" + be);
            const long long a = depth(direct), b = depth(in_run);
            check_true(b * 10 >= a * 9 && b * 10 <= a * 11, "about the same depth: " + std::to_string(a) + " and " + std::to_string(b) + be);
        });

        test("start_takes_a_callable_and_a_jaibite", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            h.eng->execute("int twice_later(int x) { sleep(1.0); return x * 2; }");
            script_run call = h.eng->start(h.eng->get_variable("twice_later"), {h.eng->make_value(21)});
            check_true(call.state() == script_run::status::suspended, "a callable's run suspends" + be);
            h.finish(call);
            check_eq(std::string("42"), call.result().value().to_string(), "its value" + be);
            jai::jaibite bite = h.eng->jaibite("sleep(0.5); print(\"bite\"); 3;");
            script_run from_bite = h.eng->start(bite);
            h.finish(from_bite);
            check_eq(std::string("bite\n"), h.take(), "a jaibite's run" + be);
            check_eq(std::string("3"), from_bite.result().value().to_string(), "its value" + be);
            bool refused = false;
            try { (void)h.eng->start(h.eng->make_value(5)); } catch (const std::exception&) { refused = true; }
            check_true(refused, "a value that is not a function is refused" + be);
        });

        test("a_host_function_may_start_a_run_inside_a_run", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            std::vector<script_run> spawned;
            engine* raw = h.eng.get();
            h.eng->add_function("spawn", [raw, &spawned](const std::string& src) { spawned.push_back(raw->start(src)); });
            script_run outer = h.eng->start(R"(spawn("print(\"inner 1\"); sleep(2.0); print(\"inner 2\");"); print("outer 1"); sleep(1.0); print("outer 2");)");
            check_eq(std::string("inner 1\nouter 1\n"), h.take(), "the inner run suspended back into the host function" + be);
            h.finish(outer);
            h.finish(spawned.at(0));
            check_eq(std::string("outer 2\ninner 2\n"), h.take(), "both finished" + be);
        });

        test("the_checker_knows_sleep", [this]() {
            auto e = make_engine();
            const std::string be = backend_label(*e);
            auto diags = [&](const std::string& src) {
                std::string out;
                for (const auto& d : e->check(src).diagnostics) {
                    out += (d.severity == check_diagnostic::level::error ? "E: " : "W: ") + d.message + "\n";
                }
                return out;
            };
            check_eq(std::string("E: 'sleep' expects 1 argument(s), got 2\n"), diags("sleep(1, 2);"), "arity" + be);
            check_eq(std::string("E: argument 1 of 'sleep': cannot pass 'string' where a number is expected\n"), diags("sleep(\"x\");"), "type" + be);
            check_eq(std::string("W: 'sleep' in a destructor blocks: a run never suspends inside a destructor\n"),
                     diags("class D { ~D() { sleep(1); } }"), "a destructor" + be);
            check_eq(std::string("W: 'sleep' in hot_reload_migrate blocks: a run never suspends inside the reload hook\n"),
                     diags("int x = 0;\nvoid hot_reload_migrate(map<string, var> old) { sleep(1); }"), "the reload hook" + be);
            check_eq(std::string("E: 'sleep' cannot be called in a parallel body\n"),
                     diags("array<int> xs = [1, 2];\nparallel_for (auto& x : xs) { sleep(0.1); }"), "a parallel_for body" + be);
            check_eq(std::string("E: 'sleep' cannot be called in a parallel body\n"),
                     diags("var r = parallel_transform([1, 2], [](int x) { sleep(0.1); return x; });"), "a parallel_transform lambda" + be);
            check_eq(std::string(), diags("void f() { sleep(1); sleep(0.5); }\nauto l = []() { sleep(1); };"), "everywhere else" + be);
            check_eq(std::string(), diags("int sleep(int a, int b) { return a + b; }\nsleep(1, 2);"), "a script's own sleep is its function" + be);
        });

        test("a_script_function_named_sleep_replaces_the_builtin", [this]() {
            run_host h;
            const std::string be = backend_label(*h.eng);
            script_run run = h.eng->start("void sleep(float s) { print(\"mine {}\", s); }\nsleep(2.5);\n7;");
            check_true(run.state() == script_run::status::completed, "no suspension" + be);
            check_eq(std::string("mine 2.5\n"), h.take(), "the script's function ran" + be);
        });
    }
};

} // namespace jai::foundry::tests

using script_run_tests = jai::foundry::tests::script_run_tests;
FOUNDRY_REGISTER(script_run_tests)
