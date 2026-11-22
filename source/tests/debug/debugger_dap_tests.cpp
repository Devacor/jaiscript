// What VS Code shows at a stop, over the Debug Adapter Protocol: the Call Stack and every frame's Locals, read through a live
// jai::debug::debug_connector on both backends, which must show the same thing.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>

#ifdef JAISCRIPT_ENABLE_DEBUGGER

#include "dap_client.hpp"

#include <jaiscript/debug/connector.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/signals/signal.hpp>
#include <jaiscript/core/dynamic_binder.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace jai::foundry::tests {

namespace {

class audit_door {
public:
    audit_door() : on_open(on_open_signal) {}
    void open(int width) { on_open_signal(width); }
private:
    jai::signal_emitter<void(int)> on_open_signal;
public:
    jai::signal<void(int)> on_open;
};

std::filesystem::path audit_directory() {
    const auto dir = test_temp_directory() / "dap_audit";
    std::filesystem::create_directories(dir);
    return dir;
}

std::string write_audit_file(const std::string& name, const std::string& content) {
    const auto path = audit_directory() / name;
    std::ofstream(path, std::ios::binary) << content;
    return std::filesystem::canonical(path).string();
}

std::string base_name(const std::string& path) {
    return path.empty() ? std::string() : std::filesystem::path(path).filename().string();
}

} // namespace

// One DAP session over a script: the breakpoints, then at every stop the stack and each frame's locals, then the next action
struct dap_script {
    std::string file;                          // the main script's name in the audit directory
    std::string source;
    std::vector<std::pair<std::string, std::vector<int>>> breakpoints;   // file name -> lines
    std::vector<std::string> actions;          // next, stepIn, stepOut, continue: one per stop, then continue
    std::vector<std::pair<std::string, std::string>> files;   // other files the script includes or imports
    std::function<void(jai::engine&)> setup;
    std::function<void(jai::engine&)> after;   // runs after the script, its stops still recorded (a host's second entry)
    int max_stops = 40;
    // attach and pause once the script calls started(), instead of attaching before it runs
    bool attach_when_started = false;
};

class debugger_dap_tests : public suite {
public:
    debugger_dap_tests() : suite("Debugger DAP") {}

    // The Locals of one frame as "name=value" in the order the reply lists them
    static std::string locals_text(dap::client& cli, int frame_id) {
        const dap::json scopes = cli.request("scopes", "{\"frameId\":" + std::to_string(frame_id) + "}");
        int locals = 0;
        for (const auto& scope : scopes["body"]["scopes"].items) {
            if (scope["name"].str() == "Locals") locals = scope["variablesReference"].as_int();
        }
        if (!locals) return "<no locals scope>";
        const dap::json vars = cli.request("variables", "{\"variablesReference\":" + std::to_string(locals) + "}");
        if (!vars["success"].flag) return "<" + vars["message"].str() + ">";
        std::string out;
        for (const auto& v : vars["body"]["variables"].items) {
            if (!out.empty()) out += ", ";
            out += v["name"].str() + "=" + v["value"].str();
        }
        return out;
    }

    static std::string stop_text(dap::client& cli, const dap::json& stopped) {
        std::string out = stopped["body"]["reason"].str();
        const dap::json trace = cli.request("stackTrace", "{\"threadId\":1}");
        for (const auto& frame : trace["body"]["stackFrames"].items) {
            out += "\n  " + frame["name"].str() + " " + base_name(frame["source"]["path"].str()) + ":" + std::to_string(frame["line"].as_int());
            const std::string locals = frame["id"].as_int() > 0 ? locals_text(cli, frame["id"].as_int()) : "<no id>";
            if (!locals.empty()) out += " | " + locals;
        }
        return out + "\n";
    }

    std::string run(jai::backend_type type, const dap_script& script) {
        auto eng = make_default_engine();
        eng->set_backend(type);
        jai::stdlib::register_all(eng);
        eng->jaibite_cache(false);
        if (script.setup) script.setup(*eng);
        for (const auto& [name, content] : script.files) write_audit_file(name, content);
        const std::string main_path = write_audit_file(script.file, script.source);

        auto conn = std::make_shared<jai::debug::debug_connector>(0);
        eng->set_debug_connector(conn);
        dap::client cli(conn->port());
        check_true(cli.connected());
        if (!cli.connected()) return "<no connection>";
        std::atomic<bool> started{false};
        if (script.attach_when_started) { eng->add_function("started", [&started]() { started = true; }); }
        cli.request("initialize", "{\"adapterID\":\"jaiscript\"}");
        if (!script.attach_when_started) cli.request("attach");
        for (const auto& [name, lines] : script.breakpoints) {
            std::string list;
            for (int line : lines) list += (list.empty() ? "" : ",") + std::string("{\"line\":") + std::to_string(line) + "}";
            cli.request("setBreakpoints", "{\"source\":{\"path\":" + dap::quote((audit_directory() / name).string()) + "},\"breakpoints\":[" + list + "]}");
        }
        cli.request("configurationDone");

        std::string error;
        std::atomic<bool> done{false};
        std::thread runner([&] {
            try {
                eng->execute_file(main_path);
                if (script.after) script.after(*eng);
            } catch (const std::exception& e) {
                error = e.what();
            }
            done = true;
        });
        struct joined {
            jai::engine& eng;
            std::thread& thread;
            ~joined() { eng.debugger().set_enabled(false); if (thread.joinable()) thread.join(); }
        } join{*eng, runner};

        if (script.attach_when_started) {
            stall_watch watch;
            while (!started.load() && !done.load() && !watch.stalled(0)) { std::this_thread::yield(); }
            cli.request("attach");
            cli.request("pause", "{\"threadId\":1}");
        }
        std::string out;
        size_t action = 0;
        for (int stops = 0; stops < script.max_stops; ++stops) {
            auto stopped = cli.wait_event("stopped", [&] { return done.load(); });
            if (!stopped) break;
            out += stop_text(cli, *stopped);
            const std::string next = action < script.actions.size() ? script.actions[action++] : "continue";
            cli.request(next, "{\"threadId\":1}");
        }
        eng->debugger().set_enabled(false);
        runner.join();
        if (!error.empty()) out += "error: " + error + "\n";
        return out;
    }

    // Both backends show `expected`
    void check_both(const dap_script& script, const std::string& expected) {
        const std::string interpreted = run(jai::backend_type::interpreter, script);
        const std::string compiled = run(jai::backend_type::vm, script);
        check_eq(interpreted, compiled, "the vm shows what the interpreter shows");
        check_eq(expected, interpreted, "interpreter");
    }

    void forge_tests() override {
        test("function_parameters_block_locals_and_loop_variables", [this]() {
            dap_script s;
            s.file = "locals.jai";
            s.source =
                "function walk(int a, int unused) -> int {\n"   // 1
                "    int b = a + 1;\n"                          // 2
                "    {\n"                                       // 3
                "        int inner = b * 2;\n"                  // 4
                "        int b = 50;\n"                         // 5
                "        print(inner + b);\n"                   // 6
                "    }\n"                                       // 7
                "    for (int i = 0; i < 2; ++i) {\n"           // 8
                "        int square = i * i;\n"                 // 9
                "        print(square);\n"                      // 10
                "    }\n"                                       // 11
                "    int late = 3;\n"                           // 12
                "    return b + late;\n"                        // 13
                "}\n"                                           // 14
                "int total = walk(1, 2);\n";                    // 15
            s.breakpoints = {{"locals.jai", {6, 9, 13}}};
            check_both(s,
                "breakpoint\n  walk locals.jai:6 | a=1, b=50, inner=4, unused=2\n  <script> locals.jai:15\n"
                "breakpoint\n  walk locals.jai:9 | a=1, b=2, i=0, unused=2\n  <script> locals.jai:15\n"
                "breakpoint\n  walk locals.jai:9 | a=1, b=2, i=1, unused=2\n  <script> locals.jai:15\n"
                "breakpoint\n  walk locals.jai:13 | a=1, b=2, late=3, unused=2\n  <script> locals.jai:15\n");
        });

        test("constants_and_locals_the_vm_folds_or_never_reads", [this]() {
            dap_script s;
            s.file = "folded.jai";
            s.source =
                "function fold(int a) -> int {\n"               // 1
                "    const int k = 4;\n"                        // 2
                "    int never_read = 7;\n"                     // 3
                "    float half = 0.5;\n"                       // 4
                "    int y = a * k;\n"                          // 5
                "    return y;\n"                               // 6
                "}\n"                                           // 7
                "const int scale = 3;\n"                        // 8
                "int got = fold(scale);\n";                     // 9
            s.breakpoints = {{"folded.jai", {6}}};
            check_both(s, "breakpoint\n  fold folded.jai:6 | a=3, half=0.5, k=4, never_read=7, y=12\n  <script> folded.jai:9\n");
        });

        test("the_top_level_shows_its_block_locals_in_every_frame", [this]() {
            dap_script s;
            s.file = "top.jai";
            s.source =
                "int total = 0;\n"                              // 1
                "function bump(int by) {\n"                     // 2
                "    total += by;\n"                            // 3
                "}\n"                                           // 4
                "for (int i = 0; i < 2; ++i) {\n"               // 5
                "    int twice = i * 2;\n"                      // 6
                "    total += twice;\n"                         // 7
                "    bump(i);\n"                                // 8
                "}\n";                                          // 9
            s.breakpoints = {{"top.jai", {7, 3}}};
            s.max_stops = 3;
            check_both(s,
                "breakpoint\n  <script> top.jai:7 | i=0, twice=0\n"
                "breakpoint\n  bump top.jai:3 | by=0\n  <script> top.jai:8 | i=0, twice=0\n"
                "breakpoint\n  <script> top.jai:7 | i=1, twice=2\n");
        });

        test("stepping_through_calls_that_would_inline", [this]() {
            dap_script s;
            s.file = "inline.jai";
            s.source =
                "function square(int v) -> int {\n"             // 1
                "    int r = v * v;\n"                          // 2
                "    return r;\n"                               // 3
                "}\n"                                           // 4
                "function sum_squares(int n) -> int {\n"        // 5
                "    int acc = 0;\n"                            // 6
                "    for (int k = 0; k < n; ++k) {\n"           // 7
                "        acc += square(k);\n"                   // 8
                "    }\n"                                       // 9
                "    return acc;\n"                             // 10
                "}\n"                                           // 11
                "int result = sum_squares(2);\n";               // 12
            s.breakpoints = {{"inline.jai", {12}}};
            s.actions = {"stepIn", "next", "next", "stepIn", "next", "stepOut"};
            check_both(s,
                "breakpoint\n  <script> inline.jai:12\n"
                "step\n  sum_squares inline.jai:6 | n=2\n  <script> inline.jai:12\n"
                "step\n  sum_squares inline.jai:7 | acc=0, n=2\n  <script> inline.jai:12\n"
                "step\n  sum_squares inline.jai:8 | acc=0, k=0, n=2\n  <script> inline.jai:12\n"
                "step\n  square inline.jai:2 | v=0\n  sum_squares inline.jai:8 | acc=0, k=0, n=2\n  <script> inline.jai:12\n"
                "step\n  square inline.jai:3 | r=0, v=0\n  sum_squares inline.jai:8 | acc=0, k=0, n=2\n  <script> inline.jai:12\n"
                "step\n  sum_squares inline.jai:8 | acc=0, k=1, n=2\n  <script> inline.jai:12\n");
        });

        // An inlined callee runs only while no session is armed: a pause that arrives inside one stops in its body
        test("a_pause_inside_an_inlined_callee_shows_it_as_a_frame_with_its_locals", [this]() {
            dap_script s;
            s.file = "graft.jai";
            s.attach_when_started = true;
            s.source =
                "function square(int v) -> int {\n"             // 1
                "    int r = v * v;\n"                          // 2
                "    sleep(0.2);\n"                             // 3
                "    return r;\n"                               // 4
                "}\n"                                           // 5
                "function run() -> int {\n"                     // 6
                "    int acc = 9;\n"                            // 7
                "    acc += square(3);\n"                       // 8
                "    acc += square(4);\n"                       // 9
                "    return acc;\n"                             // 10
                "}\n"                                           // 11
                "started();\n"                                  // 12
                "int got = run();\n";                           // 13
            s.actions = {"next", "stepOut"};
            check_both(s,
                "pause\n  square graft.jai:3 | r=9, v=3\n  run graft.jai:8 | acc=9\n  <script> graft.jai:13\n"
                "step\n  square graft.jai:4 | r=9, v=3\n  run graft.jai:8 | acc=9\n  <script> graft.jai:13\n"
                "step\n  run graft.jai:9 | acc=18\n  <script> graft.jai:13\n");
        });

        // A typed loop the vm runs in cells: a pause lands in it with every local current (the cells written back to their slots)
        test("a_pause_inside_a_hot_typed_loop_reads_current_locals", [this]() {
            for (const auto type : {jai::backend_type::interpreter, jai::backend_type::vm}) {
                auto eng = make_default_engine();
                eng->set_backend(type);
                jai::stdlib::register_all(eng);
                std::atomic<bool> started{false};
                eng->add_function("started", [&started]() { started = true; });
                auto conn = std::make_shared<jai::debug::debug_connector>(0);
                eng->set_debug_connector(conn);
                dap::client cli(conn->port());
                check_true(cli.connected());
                cli.request("initialize");
                std::atomic<bool> done{false};
                eng->execution_budget(60.0);   // a pause that never lands leaves a loop the budget ends once the session does
                std::string error;
                std::thread runner([&] {
                    try {
                        eng->execute(
                            "function spin(int n) -> int {\n"
                            "    int sum = 0;\n"
                            "    for (int i = 0; i < n; ++i) {\n"
                            "        sum += i;\n"
                            "    }\n"
                            "    return sum;\n"
                            "}\n"
                            "started();\n"
                            "int got = spin(2000000000);\n");
                    } catch (const std::exception& e) {
                        error = e.what();
                    }
                    done = true;
                });
                struct joined { jai::engine& eng; std::thread& thread; ~joined() { eng.debugger().set_enabled(false); if (thread.joinable()) thread.join(); } } join{*eng, runner};
                stall_watch watch;
                while (!started.load() && !watch.stalled(0)) { std::this_thread::yield(); }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));   // well into the loop
                cli.request("attach");
                cli.request("pause", "{\"threadId\":1}");
                check_true(cli.wait_event("stopped").has_value());
                const dap::json trace = cli.request("stackTrace", "{\"threadId\":1}");
                check_eq(std::string("spin"), trace["body"]["stackFrames"][0]["name"].str());
                const int frame = trace["body"]["stackFrames"][0]["id"].as_int();
                const dap::json scopes = cli.request("scopes", "{\"frameId\":" + std::to_string(frame) + "}");
                const dap::json vars = cli.request("variables", "{\"variablesReference\":" + std::to_string(scopes["body"]["scopes"][0]["variablesReference"].as_int()) + "}");
                int64_t i = -1, sum = -1, n = -1;
                for (const auto& v : vars["body"]["variables"].items) {
                    const int64_t value = std::strtoll(v["value"].str().c_str(), nullptr, 10);
                    if (v["name"].str() == "i") i = value;
                    if (v["name"].str() == "sum") sum = value;
                    if (v["name"].str() == "n") n = value;
                }
                check_eq(int64_t(2000000000), n);
                check_gt(i, int64_t(0));
                // the VM may stop where the block missed, at the loop's update (i added, not yet stepped); otherwise at the body
                const bool added = sum == i * (i + 1) / 2;
                check_true(added || sum == i * (i - 1) / 2, "the locals agree with one point of one iteration");
                eng->debugger().post_command([&] { eng->debugger().set_variable("n", eng->make_value(0)); });
                cli.request("continue", "{\"threadId\":1}");
                eng->debugger().set_enabled(false);
                runner.join();
                check_true(error.empty(), error);
                if (error.empty()) { check_eq(added ? sum : sum + i, eng->get_variable("got").as_int(), "the stopped iteration finishes, then n = 0 ends the loop"); }
            }
        });

        test("lambdas_and_their_captures", [this]() {
            dap_script s;
            s.file = "lambda.jai";
            s.source =
                "function make() -> int {\n"                                        // 1
                "    int base = 10;\n"                                              // 2
                "    int counter = 0;\n"                                            // 3
                "    auto add = [base, &counter](int k) -> int {\n"                 // 4
                "        counter += k;\n"                                           // 5
                "        return base + counter;\n"                                  // 6
                "    };\n"                                                          // 7
                "    add(5);\n"                                                     // 8
                "    array<int> xs = [1, 2];\n"                                     // 9
                "    auto ys = array::map(xs, [&](int x) -> int { return x + base; });\n"   // 10
                "    return counter + ys[0];\n"                                     // 11
                "}\n"                                                               // 12
                "int got = make();\n";                                              // 13
            s.breakpoints = {{"lambda.jai", {6, 10}}};
            s.actions = {"continue", "stepIn"};
            check_both(s,
                "breakpoint\n  <lambda> lambda.jai:6 | base=10, counter=5, k=5\n  make lambda.jai:8 | add=<function>, base=10, counter=5\n  <script> lambda.jai:13\n"
                "breakpoint\n  make lambda.jai:10 | add=<function>, base=10, counter=5, xs=[1, 2]\n  <script> lambda.jai:13\n"
                "step\n  <lambda> lambda.jai:10 | base=10, x=1\n  make lambda.jai:10 | add=<function>, base=10, counter=5, xs=[1, 2]\n  <script> lambda.jai:13\n"
                "breakpoint\n  <lambda> lambda.jai:10 | base=10, x=2\n  make lambda.jai:10 | add=<function>, base=10, counter=5, xs=[1, 2]\n  <script> lambda.jai:13\n");
        });

        test("methods_constructors_destructors_and_operators", [this]() {
            dap_script s;
            s.file = "methods.jai";
            s.source =
                "class Monster {\n"                                                 // 1
                "    int hp = 10;\n"                                                // 2
                "    string name;\n"                                                // 3
                "    Monster(string n, int h) {\n"                                  // 4
                "        name = n;\n"                                               // 5
                "        hp = h;\n"                                                 // 6
                "    }\n"                                                           // 7
                "    ~Monster() {\n"                                                // 8
                "        print(\"bye \" + name);\n"                                 // 9
                "    }\n"                                                           // 10
                "    void hit(int d) {\n"                                           // 11
                "        int left = hp - d;\n"                                      // 12
                "        hp = left;\n"                                              // 13
                "    }\n"                                                           // 14
                "    Monster operator+(int extra) {\n"                              // 15
                "        return Monster(name, hp + extra);\n"                       // 16
                "    }\n"                                                           // 17
                "}\n"                                                               // 18
                "function fight() {\n"                                              // 19
                "    Monster m = Monster(\"orc\", 7);\n"                            // 20
                "    m.hit(2);\n"                                                   // 21
                "    Monster n = m + 1;\n"                                          // 22
                "}\n"                                                               // 23
                "fight();\n";                                                       // 24
            s.breakpoints = {{"methods.jai", {6, 9, 13, 16}}};
            check_both(s,
                "breakpoint\n  Monster methods.jai:6 | h=7, n=\"orc\", this=Monster{hp: 10, name: \"orc\"}\n  fight methods.jai:20\n  <script> methods.jai:24\n"
                "breakpoint\n  hit methods.jai:13 | d=2, left=5, this=Monster{hp: 7, name: \"orc\"}\n  fight methods.jai:21 | m=Monster{hp: 7, name: \"orc\"}\n  <script> methods.jai:24\n"
                "breakpoint\n  + methods.jai:16 | extra=1, this=Monster{hp: 5, name: \"orc\"}\n  fight methods.jai:22 | m=Monster{hp: 5, name: \"orc\"}\n  <script> methods.jai:24\n"
                "breakpoint\n  Monster methods.jai:6 | h=6, n=\"orc\", this=Monster{hp: 10, name: \"orc\"}\n  + methods.jai:16 | extra=1, this=Monster{hp: 5, name: \"orc\"}\n  fight methods.jai:22 | m=Monster{hp: 5, name: \"orc\"}\n  <script> methods.jai:24\n"
                "breakpoint\n  ~Monster methods.jai:9 | this=Monster{hp: 6, name: \"orc\"}\n  fight methods.jai:22 | m=Monster{hp: 5, name: \"orc\"}, n=null\n  <script> methods.jai:24\n"
                "breakpoint\n  ~Monster methods.jai:9 | this=Monster{hp: 5, name: \"orc\"}\n  fight methods.jai:22 | m=null, n=null\n  <script> methods.jai:24\n");
        });

        test("coroutines_show_their_frames_and_locals_when_resumed", [this]() {
            dap_script s;
            s.file = "coroutine.jai";
            s.source =
                "coroutine int counter(int start) {\n"          // 1
                "    int at = start;\n"                         // 2
                "    while (true) {\n"                          // 3
                "        int next_value = at + 1;\n"            // 4
                "        yield at;\n"                           // 5
                "        at = next_value;\n"                    // 6
                "    }\n"                                       // 7
                "}\n"                                           // 8
                "function drive() -> int {\n"                   // 9
                "    auto c = counter(5);\n"                    // 10
                "    int first = c.next();\n"                   // 11
                "    int second = c.next();\n"                  // 12
                "    return first + second;\n"                  // 13
                "}\n"                                           // 14
                "int got = drive();\n";                         // 15
            s.breakpoints = {{"coroutine.jai", {5, 12}}};
            s.actions = {"continue", "stepIn", "next", "next", "next"};
            check_both(s,
                "breakpoint\n  counter coroutine.jai:5 | at=5, next_value=6, start=5\n  drive coroutine.jai:11 | c=<coroutine>\n  <script> coroutine.jai:15\n"
                "breakpoint\n  drive coroutine.jai:12 | c=<coroutine>, first=5\n  <script> coroutine.jai:15\n"
                "step\n  counter coroutine.jai:6 | at=5, next_value=6, start=5\n  drive coroutine.jai:12 | c=<coroutine>, first=5\n  <script> coroutine.jai:15\n"
                "step\n  counter coroutine.jai:4 | at=6, start=5\n  drive coroutine.jai:12 | c=<coroutine>, first=5\n  <script> coroutine.jai:15\n"
                "step\n  counter coroutine.jai:5 | at=6, next_value=7, start=5\n  drive coroutine.jai:12 | c=<coroutine>, first=5\n  <script> coroutine.jai:15\n"
                "step\n  drive coroutine.jai:13 | c=<coroutine>, first=5, second=6\n  <script> coroutine.jai:15\n");
        });

        test("a_host_call_back_into_script_and_a_signal_receiver_keep_the_whole_stack", [this]() {
            dap_script s;
            s.file = "host.jai";
            s.setup = [](jai::engine& eng) {
                eng.add_function("host_apply", [](std::function<int64_t(int64_t)> fn, int64_t v) { return fn(v) + 1; });
                jai::dynamic_binder<audit_door>(eng, "AuditDoor")
                    .constructor<>()
                    .method("open", &audit_door::open)
                    .property("on_open", &audit_door::on_open);
            };
            s.source =
                "function triple(int v) -> int {\n"                                 // 1
                "    int t = v * 3;\n"                                              // 2
                "    return t;\n"                                                   // 3
                "}\n"                                                               // 4
                "function outer(int seed) -> int {\n"                               // 5
                "    int got = host_apply(triple, seed);\n"                         // 6
                "    return got;\n"                                                 // 7
                "}\n"                                                               // 8
                "int value = outer(2);\n"                                           // 9
                "var door = AuditDoor();\n"                                         // 10
                "door.on_open.connect(\"watch\", [=](int w) -> void {\n"            // 11
                "    int wide = w * 2;\n"                                           // 12
                "    print(wide);\n"                                                // 13
                "});\n"                                                             // 14
                "door.open(4);\n";                                                  // 15
            s.breakpoints = {{"host.jai", {3, 13}}};
            check_both(s,
                "breakpoint\n  triple host.jai:3 | t=6, v=2\n  outer host.jai:6 | seed=2\n  <script> host.jai:9\n"
                "breakpoint\n  <lambda> host.jai:13 | w=4, wide=8\n  <script> host.jai:15\n");
        });

        test("namespaces_imports_and_includes", [this]() {
            dap_script s;
            s.file = "main.jai";
            s.files = {
                {"helpers.jai", "namespace geo {\n    function area(int w, int h) -> int {\n        int a = w * h;\n        return a;\n    }\n}\n"},
                {"inc.jai", "int included_value = 3;\nint included_twice = included_value * 2;\n"},
            };
            s.source =
                "import \"helpers.jai\";\n"                     // 1
                "include \"inc.jai\";\n"                        // 2
                "int a = geo::area(2, 3);\n"                    // 3
                "print(a + included_twice);\n";                 // 4
            s.breakpoints = {{"helpers.jai", {4}}, {"inc.jai", {2}}};
            check_both(s,
                "breakpoint\n  <script> inc.jai:2\n"
                "breakpoint\n  area helpers.jai:4 | a=6, h=3, w=2\n  <script> main.jai:3\n");
        });

        test("generic_instances", [this]() {
            dap_script s;
            s.file = "generic.jai";
            s.source =
                "T largest<T>(array<T> xs) {\n"                 // 1
                "    T best = xs[0];\n"                         // 2
                "    for (auto x : xs) {\n"                     // 3
                "        if (x > best) { best = x; }\n"         // 4
                "    }\n"                                       // 5
                "    return best;\n"                            // 6
                "}\n"                                           // 7
                "int top = largest<int>([4, 9, 2]);\n";         // 8
            s.breakpoints = {{"generic.jai", {6}}};
            check_both(s, "breakpoint\n  largest<int> generic.jai:6 | best=9, xs=[4, 9, 2]\n  <script> generic.jai:8\n");
        });

        test("a_hot_reload_rebinds_and_shows_the_new_locals", [this]() {
            dap_script s;
            s.file = "reload.jai";
            s.source =
                "function step(int x) -> int {\n"               // 1
                "    int y = x + 1;\n"                          // 2
                "    return y;\n"                               // 3
                "}\n"                                           // 4
                "int got = step(1);\n";                         // 5
            s.breakpoints = {{"reload.jai", {3}}};
            s.after = [](jai::engine& eng) {
                const std::string path = write_audit_file("reload.jai",
                    "function step(int x) -> int {\n"
                    "    int z = x * 10;\n"
                    "    return z;\n"
                    "}\n"
                    "int got = step(2);\n");
                eng.execute_file(path);
            };
            check_both(s,
                "breakpoint\n  step reload.jai:3 | x=1, y=2\n  <script> reload.jai:5\n"
                "breakpoint\n  step reload.jai:3 | x=2, z=20\n  <script> reload.jai:5\n");
        });
    }
};

// A script under a DAP session that a test drives stop by stop
struct live_session {
    std::shared_ptr<jai::engine> eng;
    std::shared_ptr<jai::debug::debug_connector> conn;
    std::unique_ptr<dap::client> cli;
    std::thread runner;
    std::atomic<bool> done{false};
    std::string error;
    std::vector<std::string> output;

    live_session(jai::backend_type type, const std::string& file, const std::string& source,
                 const std::vector<std::string>& breakpoints, const std::string& exception_filters = "",
                 const std::function<void(jai::engine&)>& setup = {}) {
        eng = make_default_engine();
        eng->set_backend(type);
        jai::stdlib::register_all(eng);
        eng->jaibite_cache(false);
        if (setup) setup(*eng);
        const std::string path = write_audit_file(file, source);
        conn = std::make_shared<jai::debug::debug_connector>(0);
        eng->set_debug_connector(conn);
        cli = std::make_unique<dap::client>(conn->port());
        cli->on_event([this](const dap::json& event) {
            if (event["event"].str() == "output") output.push_back(event["body"]["output"].str());
        });
        cli->request("initialize");
        cli->request("attach");
        cli->request("setBreakpoints", "{\"source\":{\"path\":" + dap::quote(path) + "},\"breakpoints\":[" + join(breakpoints) + "]}");
        if (!exception_filters.empty()) cli->request("setExceptionBreakpoints", "{\"filters\":[" + exception_filters + "]}");
        cli->request("configurationDone");
        runner = std::thread([this, path] {
            try { eng->execute_file(path); } catch (const std::exception& e) { error = e.what(); }
            done = true;
        });
    }
    ~live_session() { finish(); }

    static std::string join(const std::vector<std::string>& items) {
        std::string out;
        for (const auto& item : items) out += (out.empty() ? "" : ",") + item;
        return out;
    }

    // The next stop's event, or none once the script ends
    std::optional<dap::json> stop() { return cli->wait_event("stopped", [this] { return done.load(); }); }
    void resume(const std::string& how = "continue") { cli->request(how, "{\"threadId\":1}"); }
    void finish() {
        if (!runner.joinable()) return;
        eng->debugger().set_enabled(false);
        runner.join();
    }

    int frame_id(size_t index) {
        const dap::json trace = cli->request("stackTrace", "{\"threadId\":1}");
        return trace["body"]["stackFrames"][index]["id"].as_int();
    }
    int locals_ref(size_t frame = 0) {
        const dap::json scopes = cli->request("scopes", "{\"frameId\":" + std::to_string(frame_id(frame)) + "}");
        return scopes["body"]["scopes"][0]["variablesReference"].as_int();
    }
    int globals_ref() {
        const dap::json scopes = cli->request("scopes", "{\"frameId\":" + std::to_string(frame_id(0)) + "}");
        return scopes["body"]["scopes"][1]["variablesReference"].as_int();
    }
    dap::json variables(int reference, const std::string& paging = "") {
        return cli->request("variables", "{\"variablesReference\":" + std::to_string(reference) + paging + "}");
    }
    // The variable `name` of a variables reply
    static dap::json find(const dap::json& reply, const std::string& name) {
        for (const auto& v : reply["body"]["variables"].items) if (v["name"].str() == name) return v;
        return dap::json{};
    }
    // "name=value" for each child, in order
    std::string listing(int reference, const std::string& paging = "") {
        std::string out;
        const dap::json reply = variables(reference, paging);
        if (!reply["success"].flag) return "<" + reply["message"].str() + ">";
        for (const auto& v : reply["body"]["variables"].items) {
            out += (out.empty() ? "" : ", ") + v["name"].str() + "=" + v["value"].str();
        }
        return out;
    }
    dap::json evaluate(const std::string& expression, const std::string& context = "watch", int frame = -1) {
        return cli->request("evaluate", "{\"expression\":" + dap::quote(expression) + ",\"context\":\"" + context + "\""
            + (frame >= 0 ? ",\"frameId\":" + std::to_string(frame) : std::string()) + "}");
    }
    // An evaluation's result text, or "error: why"
    std::string value_of(const std::string& expression, const std::string& context = "watch", int frame = -1) {
        const dap::json reply = evaluate(expression, context, frame);
        return reply["success"].flag ? reply["body"]["result"].str() : "error: " + reply["message"].str();
    }
    std::string set(int reference, const std::string& name, const std::string& value) {
        const dap::json reply = cli->request("setVariable", "{\"variablesReference\":" + std::to_string(reference) + ",\"name\":" + dap::quote(name)
            + ",\"value\":" + dap::quote(value) + "}");
        return reply["success"].flag ? reply["body"]["value"].str() : "error: " + reply["message"].str();
    }
};

class debugger_dap_feature_tests : public suite {
public:
    debugger_dap_feature_tests() : suite("Debugger DAP Features") {}

    static constexpr jai::backend_type k_backends[] = {jai::backend_type::interpreter, jai::backend_type::vm};
    static const char* backend_name(jai::backend_type type) { return type == jai::backend_type::vm ? "vm" : "interpreter"; }

    void forge_tests() override {
        test("structured_values_expand_to_any_depth_and_page", [this]() {
            for (const auto type : k_backends) {
                live_session s(type, "expand.jai",
                    "class Item { string label = \"gem\"; int weight = 3; }\n"                  // 1
                    "class Bag { array<Item> items; map<string, int> counts; shared_ptr<Item> best; shared_ptr<Item> none; }\n"   // 2
                    "function fill() -> int {\n"                                               // 3
                    "    Bag bag;\n"                                                           // 4
                    "    bag.items.push(Item());\n"                                            // 5
                    "    bag.counts[\"gem\"] = 2;\n"                                           // 6
                    "    bag.best = make_shared<Item>();\n"                                    // 7
                    "    array<int> big;\n"                                                    // 8
                    "    for (int i = 0; i < 250; ++i) { big.push(i * 2); }\n"                 // 9
                    "    return bag.items.size() + big.size();\n"                              // 10
                    "}\n"                                                                      // 11
                    "int got = fill();\n",                                                     // 12
                    {"{\"line\":10}"});
                check_true(s.stop().has_value(), backend_name(type));
                const dap::json locals = s.variables(s.locals_ref());
                const dap::json bag = live_session::find(locals, "bag");
                check_gt(bag["variablesReference"].as_int(), 0, "a class value expands");
                check_eq(4, bag["namedVariables"].as_int());
                check_eq(std::string("bag"), bag["evaluateName"].str());
                check_eq(std::string("items=[Item{label: \"gem\", weight: 3}], counts={\"gem\": 2}, best=Item{label: \"gem\", weight: 3}, none=null"),
                         s.listing(bag["variablesReference"].as_int()), backend_name(type));
                const dap::json fields = s.variables(bag["variablesReference"].as_int());
                const dap::json items = live_session::find(fields, "items");
                check_eq(1, items["indexedVariables"].as_int());
                check_eq(std::string("bag.items"), items["evaluateName"].str());
                const dap::json first = live_session::find(s.variables(items["variablesReference"].as_int()), "[0]");
                check_eq(std::string("bag.items[0]"), first["evaluateName"].str());
                check_eq(std::string("label=\"gem\", weight=3"), s.listing(first["variablesReference"].as_int()));
                const dap::json counts = live_session::find(fields, "counts");
                check_eq(std::string("\"gem\"=2"), s.listing(counts["variablesReference"].as_int()));
                check_eq(std::string("bag.counts[\"gem\"]"), live_session::find(s.variables(counts["variablesReference"].as_int()), "\"gem\"")["evaluateName"].str());
                const dap::json best = live_session::find(fields, "best");
                check_eq(std::string("label=\"gem\", weight=3"), s.listing(best["variablesReference"].as_int()), "a handle lists its object's fields");
                check_eq(0, live_session::find(fields, "none")["variablesReference"].as_int(), "a null handle is a leaf");
                const dap::json big = live_session::find(locals, "big");
                check_eq(250, big["indexedVariables"].as_int());
                check_eq(std::string("[100]=200, [101]=202, [102]=204"), s.listing(big["variablesReference"].as_int(), ",\"start\":100,\"count\":3"));
                const int stale = bag["variablesReference"].as_int();
                s.resume();
                s.finish();
                check_true(s.error.empty(), s.error);
                (void)stale;
            }
        });

        test("set_variable_stores_through_the_scripts_own_rules", [this]() {
            for (const auto type : k_backends) {
                live_session s(type, "setvar.jai",
                    "class Hero { int hp = 10; string name = \"ann\"; }\n"                     // 1
                    "int global_score = 1;\n"                                                  // 2
                    "function play(int n) -> int {\n"                                          // 3
                    "    Hero h;\n"                                                            // 4
                    "    array<int> xs = [1, 2, 3];\n"                                         // 5
                    "    map<string, int> m = {\"a\": 1};\n"                                   // 6
                    "    int total = n;\n"                                                     // 7
                    "    return total + h.hp + xs[1] + m[\"a\"] + global_score;\n"             // 8
                    "}\n"                                                                      // 9
                    "int got = play(5);\n",                                                    // 10
                    {"{\"line\":8}"});
                check_true(s.stop().has_value(), backend_name(type));
                const int locals = s.locals_ref();
                check_eq(std::string("40"), s.set(locals, "total", "40"), backend_name(type));
                check_eq(std::string("error: Cannot assign string to 'total' (declared int)"), s.set(locals, "total", "\"text\""), backend_name(type));
                check_eq(std::string("7"), s.set(locals, "n", "3 + 4"));
                const dap::json all = s.variables(locals);
                const int hero = live_session::find(all, "h")["variablesReference"].as_int();
                check_eq(std::string("100"), s.set(hero, "hp", "100"));
                check_eq(std::string("error: Cannot assign string to field 'hp' of Hero (declared int)"), s.set(hero, "hp", "\"lots\""), "a field keeps its type");
                const int xs = live_session::find(all, "xs")["variablesReference"].as_int();
                check_eq(std::string("20"), s.set(xs, "[1]", "20"));
                const int m = live_session::find(all, "m")["variablesReference"].as_int();
                check_eq(std::string("1000"), s.set(m, "\"a\"", "1000"));
                check_eq(std::string("2"), s.set(s.globals_ref(), "global_score", "2"));
                check_eq(std::string("40"), s.value_of("total"));
                s.resume();
                s.finish();
                check_true(s.error.empty(), s.error);
                check_eq(int64_t(40 + 100 + 20 + 1000 + 2), s.eng->get_variable("got").as_int(), backend_name(type));
            }
        });

        test("evaluate_reads_in_a_watch_and_changes_state_in_the_console", [this]() {
            for (const auto type : k_backends) {
                live_session s(type, "eval.jai",
                    "class Point {\n"                                                          // 1
                    "    int x = 1;\n"                                                         // 2
                    "    int y = 2;\n"                                                         // 3
                    "    Point operator+(Point o) { return Point{x + o.x, y + o.y}; }\n"       // 4
                    "    int sum() { return x + y; }\n"                                        // 5
                    "    void grow() {\n"                                                      // 6
                    "        x += 10;\n"                                                       // 7
                    "    }\n"                                                                  // 8
                    "}\n"                                                                      // 9
                    "int counter = 0;\n"                                                       // 10
                    "function bump() -> int { counter += 1; return counter; }\n"               // 11
                    "function outer(int depth) -> int {\n"                                     // 12
                    "    array<int> xs = [3, 1, 2];\n"                                         // 13
                    "    Point p;\n"                                                           // 14
                    "    p.grow();\n"                                                          // 15
                    "    int local = depth * 2;\n"                                             // 16
                    "    return local + xs.size() + p.x;\n"                                    // 17
                    "}\n"                                                                      // 18
                    "int got = outer(4);\n",                                                   // 19
                    {"{\"line\":7}", "{\"line\":17}"});
                const char* name = backend_name(type);
                check_true(s.stop().has_value(), name);
                // in the method: this and its fields, bare
                check_eq(std::string("1"), s.value_of("x"), name);
                check_eq(std::string("3"), s.value_of("this.x + y"), name);
                check_eq(std::string("4"), s.value_of("depth", "watch", s.frame_id(1)), "the selected frame's names");
                s.resume();
                check_true(s.stop().has_value(), name);
                check_eq(std::string("8"), s.value_of("local"), name);
                check_eq(std::string("3"), s.value_of("xs.size()"), name);
                check_eq(std::string("[1, 2, 3]"), s.value_of("array::sort(xs)"), name);
                check_eq(std::string("true"), s.value_of("local > 4 && xs[0] == 3"), name);
                check_eq(std::string("error: A watch only reads: an assignment changes state (evaluate it in the Debug Console)"), s.value_of("local = 9"), name);
                check_eq(std::string("error: A watch only reads: an increment changes state (evaluate it in the Debug Console)"), s.value_of("++local"), name);
                check_eq(std::string("error: A watch only reads: a call of 'push' changes state (evaluate it in the Debug Console)"), s.value_of("xs.push(4)"), name);
                check_eq(std::string("error: A watch only reads: a call of 'bump' changes state (evaluate it in the Debug Console)"), s.value_of("bump()"), name);
                check_eq(std::string("error: A watch only reads: '+' would run script code (evaluate it in the Debug Console)"), s.value_of("p + p"), name);
                check_eq(std::string("0"), s.value_of("counter"), "nothing a refused watch reached ran");
                check_eq(std::string("8"), s.value_of("local"), name);
                // the console changes state, through the variable's own rules
                check_eq(std::string("1"), s.value_of("bump()", "repl"), name);
                check_eq(std::string("1"), s.value_of("counter"), name);
                check_eq(std::string("100"), s.value_of("local = 100", "repl"), name);
                check_eq(std::string("100"), s.value_of("local"), name);
                check_eq(std::string("4"), s.value_of("xs.push(9); xs.size()", "repl"), name);
                check_eq(std::string("13"), s.value_of("p.sum()", "repl"), name);
                check_true(s.value_of("local = \"no\"", "repl").starts_with("error: "), "a typed local keeps its type");
                check_true(s.value_of("missing_name").starts_with("error: "), name);
                check_true(s.value_of("throw \"boom\"", "repl").starts_with("error: "), name);
                check_eq(std::string("error: The evaluation ran past its budget of 1000 ms"), s.value_of("while (true) { }", "repl"), name);
                check_eq(std::string("100"), s.value_of("local"), "a failed evaluation leaves the stop as it was");
                s.resume();
                s.finish();
                check_true(s.error.empty(), s.error);
                check_eq(int64_t(100 + 4 + 11), s.eng->get_variable("got").as_int(), name);
                check_eq(int64_t(1), s.eng->get_variable("counter").as_int(), name);
            }
        });

        test("conditions_hit_counts_and_logpoints", [this]() {
            for (const auto type : k_backends) {
                live_session s(type, "conditional.jai",
                    "int total = 0;\n"                                                         // 1
                    "for (int i = 0; i < 10; ++i) {\n"                                         // 2
                    "    total += i;\n"                                                        // 3
                    "    int twice = i * 2;\n"                                                 // 4
                    "    print(twice);\n"                                                      // 5
                    "}\n",                                                                     // 6
                    {"{\"line\":3,\"condition\":\"i == 4\"}", "{\"line\":4,\"hitCondition\":\"% 4\"}",
                     "{\"line\":5,\"logMessage\":\"i={i} twice={twice} {{literal}}\"}"});
                const char* name = backend_name(type);
                std::vector<std::string> stops;
                while (auto stopped = s.stop()) {
                    stops.push_back(std::to_string(s.cli->request("stackTrace", "{\"threadId\":1}")["body"]["stackFrames"][0]["line"].as_int())
                                    + ":" + s.value_of("i"));
                    s.resume();
                }
                s.finish();
                check_true(s.error.empty(), s.error);
                check_eq(std::string("4:3,3:4,4:7"), live_session::join(stops), name);
                check_eq(size_t(10), s.output.size(), name);
                if (s.output.size() == 10) {
                    check_eq(std::string("i=0 twice=0 {literal}\n"), s.output[0], name);
                    check_eq(std::string("i=9 twice=18 {literal}\n"), s.output[9], name);
                }
            }
        });

        test("exception_breakpoints_stop_where_an_error_is_raised", [this]() {
            const std::string source =
                "function risky(int n) -> int {\n"                                             // 1
                "    array<int> xs = [1, 2];\n"                                                // 2
                "    return xs[n];\n"                                                          // 3
                "}\n"                                                                          // 4
                "function thrower(int code) {\n"                                               // 5
                "    int local_code = code * 2;\n"                                             // 6
                "    throw \"code \" + to_string(local_code);\n"                               // 7
                "}\n"                                                                          // 8
                "int caught = 0;\n"                                                            // 9
                "try { risky(5); } catch (e) { caught += 1; }\n"                               // 10
                "try { thrower(21); } catch (e) { caught += 1; }\n"                            // 11
                "risky(7);\n";                                                                 // 12
            for (const auto type : k_backends) {
                const char* name = backend_name(type);
                std::string seen;
                {
                    live_session s(type, "throws.jai", source, {}, "\"thrown\"");
                    while (auto stopped = s.stop()) {
                        const dap::json trace = s.cli->request("stackTrace", "{\"threadId\":1}");
                        const dap::json& top = trace["body"]["stackFrames"][0];
                        seen += (*stopped)["body"]["reason"].str() + " " + top["name"].str() + ":" + std::to_string(top["line"].as_int())
                              + " [" + (*stopped)["body"]["text"].str() + "] " + s.listing(s.locals_ref()) + "\n";
                        const dap::json info = s.cli->request("exceptionInfo", "{\"threadId\":1}");
                        check_eq((*stopped)["body"]["text"].str(), info["body"]["description"].str());
                        s.resume();
                    }
                    s.finish();
                    check_true(s.error.find("Array index 7 out of bounds") != std::string::npos, s.error);
                    check_eq(int64_t(2), s.eng->get_variable("caught").as_int(), name);
                }
                check_eq(std::string(
                    "exception risky:3 [Array index 5 out of bounds for array of size 2] n=5, xs=[1, 2]\n"
                    "exception thrower:7 [code 42] code=21, local_code=42\n"
                    "exception risky:3 [Array index 7 out of bounds for array of size 2] n=7, xs=[1, 2]\n"), seen, name);
                seen.clear();
                {
                    live_session s(type, "throws.jai", source, {}, "\"uncaught\"");
                    while (auto stopped = s.stop()) {
                        const dap::json trace = s.cli->request("stackTrace", "{\"threadId\":1}");
                        seen += trace["body"]["stackFrames"][0]["name"].str() + ":" + std::to_string(trace["body"]["stackFrames"][0]["line"].as_int()) + "\n";
                        s.resume();
                    }
                }
                check_eq(std::string("risky:3\n"), seen, std::string(name) + ": only the error no catch takes");
            }
        });

        test("stepping_into_and_out_of_a_signal_receiver", [this]() {
            for (const auto type : k_backends) {
                std::string seen;
                live_session s(type, "receiver.jai",
                    "var door = AuditDoor();\n"                                                // 1
                    "door.on_open.connect(\"watch\", [=](int w) -> void {\n"                 // 2
                    "    int wide = w * 2;\n"                                                  // 3
                    "    print(wide);\n"                                                       // 4
                    "});\n"                                                                    // 5
                    "door.open(4);\n"                                                          // 6
                    "int after = 1;\n",                                                        // 7
                    {"{\"line\":6}"}, "", [](jai::engine& eng) {
                        jai::dynamic_binder<audit_door>(eng, "AuditDoor").constructor<>().method("open", &audit_door::open).property("on_open", &audit_door::on_open);
                    });
                for (const char* action : {"stepIn", "next", "stepOut", "continue"}) {
                    if (!s.stop()) break;
                    const dap::json trace = s.cli->request("stackTrace", "{\"threadId\":1}");
                    seen += trace["body"]["stackFrames"][0]["name"].str() + ":" + std::to_string(trace["body"]["stackFrames"][0]["line"].as_int()) + " ";
                    s.resume(action);
                }
                s.finish();
                check_true(s.error.empty(), s.error);
                check_eq(std::string("<script>:6 <lambda>:3 <lambda>:4 <script>:7 "), seen, backend_name(type));
            }
        });

        test("stepping_into_and_out_of_lambdas_receivers_and_coroutines", [this]() {
            for (const auto type : k_backends) {
                const char* name = backend_name(type);
                std::string seen;
                live_session s(type, "steps.jai",
                    "coroutine int ticks() {\n"                                                // 1
                    "    yield 1;\n"                                                           // 2
                    "    yield 2;\n"                                                           // 3
                    "}\n"                                                                      // 4
                    "function run() -> int {\n"                                                // 5
                    "    auto twice = [](int v) -> int {\n"                                    // 6
                    "        int doubled = v * 2;\n"                                           // 7
                    "        return doubled;\n"                                                // 8
                    "    };\n"                                                                 // 9
                    "    int a = twice(3);\n"                                                  // 10
                    "    auto t = ticks();\n"                                                  // 11
                    "    int b = t.next();\n"                                                  // 12
                    "    int c = t.next();\n"                                                  // 13
                    "    return a + b + c;\n"                                                  // 14
                    "}\n"                                                                      // 15
                    "int got = run();\n",                                                      // 16
                    {"{\"line\":10}"});
                const std::vector<std::string> actions = {"stepIn", "stepOut", "next", "next", "stepIn", "stepOut", "continue"};
                for (const auto& action : actions) {
                    if (!s.stop()) break;
                    const dap::json trace = s.cli->request("stackTrace", "{\"threadId\":1}");
                    seen += trace["body"]["stackFrames"][0]["name"].str() + ":" + std::to_string(trace["body"]["stackFrames"][0]["line"].as_int()) + " ";
                    s.resume(action);
                }
                s.finish();
                check_true(s.error.empty(), s.error);
                // out of a callee lands on the caller's next statement; into a resume lands on the statement the coroutine resumes at
                check_eq(std::string("run:10 <lambda>:7 run:11 run:12 run:13 ticks:3 run:14 "), seen, name);
            }
        });
    }
};

} // namespace jai::foundry::tests

using debugger_dap_feature_tests = jai::foundry::tests::debugger_dap_feature_tests;
FOUNDRY_REGISTER(debugger_dap_feature_tests)

namespace jai::foundry::tests {
} // namespace jai::foundry::tests

using debugger_dap_tests = jai::foundry::tests::debugger_dap_tests;
FOUNDRY_REGISTER(debugger_dap_tests)

#endif // JAISCRIPT_ENABLE_DEBUGGER
