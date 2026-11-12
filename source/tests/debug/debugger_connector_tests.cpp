// End-to-end DAP attach test: a raw TCP client speaks the Debug Adapter Protocol to a live
// jai::debug::debug_connector and walks the full loop — handshake, setBreakpoints, run,
// stopped, stackTrace, scopes, variables (read-only), continue. Proves the transport +
// routing + park/pump wiring without needing VS Code, on the backend --backend= picks.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/timing.hpp>
#include <jaiscript/core/engine.hpp>

#ifdef JAISCRIPT_ENABLE_DEBUGGER

#include <jaiscript/debug/connector.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/testing/temporary_directory.hpp>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <thread>

using namespace jai::foundry;

namespace jai::foundry::tests {

namespace {

#ifdef _WIN32
    using sock_t = SOCKET;
    constexpr sock_t kBad = INVALID_SOCKET;
    void close_client(sock_t s) { ::closesocket(s); }
#else
    using sock_t = int;
    constexpr sock_t kBad = -1;
    void close_client(sock_t s) { ::close(s); }
#endif

void wsa_up() {
#ifdef _WIN32
    WSADATA w;
    ::WSAStartup(MAKEWORD(2, 2), &w);
#endif
}

sock_t connect_client(int port) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        sock_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != kBad) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<uint16_t>(port));
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                // 100 ms recv timeout so wait_for() polls its stall watch.
#ifdef _WIN32
                DWORD tv = 100;
                ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
                timeval tv{0, 100000};
                ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
                return s;
            }
            close_client(s);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return kBad;
}

void send_dap(sock_t s, const std::string& json) {
    std::string frame = "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
    size_t sent = 0;
    while (sent < frame.size()) {
        int n = ::send(s, frame.data() + sent, static_cast<int>(frame.size() - sent), 0);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

bool recv_timed_out() {
#ifdef _WIN32
    return ::WSAGetLastError() == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// Recv into an accumulating buffer until it contains `needle`; false once the peer closes or sends nothing for the hang guard
bool wait_for(sock_t s, std::string& buf, const std::string& needle) {
    if (buf.find(needle) != std::string::npos) return true;
    stall_watch watch;
    char tmp[4096];
    while (!watch.stalled(buf.size())) {
        int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n == 0 || (n < 0 && !recv_timed_out())) return false;
        if (n > 0) {
            buf.append(tmp, tmp + n);
            if (buf.find(needle) != std::string::npos) return true;
        }
    }
    return false;
}

// Like wait_for, ending early once `done` holds: true when the needle came first
bool wait_for_unless(sock_t s, std::string& buf, const std::string& needle, const std::atomic<bool>& done) {
    stall_watch watch;
    char tmp[4096];
    while (buf.find(needle) == std::string::npos && !watch.stalled(buf.size())) {
        const bool finished = done.load();
        int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n == 0 || (n < 0 && !recv_timed_out())) return false;
        if (n > 0) {
            buf.append(tmp, tmp + n);
        } else if (finished) {
            return false;
        }
    }
    return buf.find(needle) != std::string::npos;
}

std::string dap_frame(const std::string& json) {
    return "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
}

// The bytes sent before an error or a send timeout
size_t send_raw(sock_t s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int chunk = static_cast<int>(std::min<size_t>(data.size() - sent, 1 << 20));
        int n = ::send(s, data.data() + sent, chunk, 0);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    return sent;
}

void set_send_timeout(sock_t s, int timeout_ms) {
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeout_ms);
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// True once the peer closes or resets the connection (pending replies are read and dropped)
bool closed_by_peer(sock_t s) {
    stall_watch watch;
    uint64_t dropped = 0;
    char tmp[4096];
    while (!watch.stalled(dropped)) {
        int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n == 0) return true;
        if (n < 0 && !recv_timed_out()) return true;
        if (n > 0) dropped += static_cast<uint64_t>(n);
    }
    return false;
}

// Like wait_for, keeping only enough of the stream to match the needle (replies may run to many MB)
bool wait_for_streaming(sock_t s, const std::string& needle, size_t* bytes_read = nullptr) {
    stall_watch watch;
    uint64_t received = 0;
    std::string window;
    char tmp[65536];
    while (!watch.stalled(received)) {
        int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n == 0 || (n < 0 && !recv_timed_out())) return false;
        if (n > 0) {
            received += static_cast<uint64_t>(n);
            if (bytes_read) *bytes_read += static_cast<size_t>(n);
            window.append(tmp, tmp + n);
            if (window.find(needle) != std::string::npos) return true;
            if (window.size() > needle.size()) window.erase(0, window.size() - needle.size());
        }
    }
    return false;
}

bool answers_initialize(int port) {
    sock_t s = connect_client(port);
    if (s == kBad) return false;
    std::string buf;
    send_dap(s, R"({"seq":1,"type":"request","command":"initialize"})");
    const bool ok = wait_for(s, buf, "\"event\":\"initialized\"");
    close_client(s);
    return ok;
}

// Pull the integer value following "key":  out of a JSON blob (small, well-formed DAP).
int extract_int(const std::string& buf, const std::string& key) {
    size_t p = buf.find("\"" + key + "\":");
    if (p == std::string::npos) return -1;
    p += key.size() + 3;
    while (p < buf.size() && (buf[p] == ' ')) ++p;
    int sign = 1;
    if (p < buf.size() && buf[p] == '-') { sign = -1; ++p; }
    int v = 0;
    bool any = false;
    while (p < buf.size() && buf[p] >= '0' && buf[p] <= '9') { v = v * 10 + (buf[p] - '0'); ++p; any = true; }
    return any ? sign * v : -1;
}

// The variablesReference of the scope named `name` in a scopes reply
int scope_reference(const std::string& buf, const std::string& name) {
    const size_t p = buf.find("\"name\":\"" + name + "\"");
    return p == std::string::npos ? -1 : extract_int(buf.substr(p), "variablesReference");
}

// A failed check throws: every exit disarms the debugger (a parked script runs on) and joins, since destroying a joinable
// std::thread calls std::terminate
struct joined_script {
    jai::debug::controller& debugger;
    std::thread& thread;
    ~joined_script() {
        debugger.set_enabled(false);
        if (thread.joinable()) { thread.join(); }
    }
};

void set_filesystem_grant(engine& eng, bool enabled) {
    if (enabled) { eng.enable(permissions::filesystem); }
    else { eng.disable(permissions::filesystem); }
}

} // namespace

class debugger_connector_tests : public suite {
public:
    debugger_connector_tests() : suite("Debugger Connector") {}

    void check_breakpoint_path(bool filesystem_enabled, int alias_update = 0,
                               bool revoke_after_ack = false) {
        wsa_up();
        // '.' exists, so baseline normalization necessarily changes this spelling.
        // The source is supplied in memory; no fixture file or cache is written.
        const std::string requested_path = "./__jai_dap_virtual__/../breakpoint.jai";
        const std::string normalized_path = std::filesystem::weakly_canonical(requested_path).string();
        check_ne(requested_path, normalized_path, "fixture must distinguish raw and normalized labels");

        auto eng = make_engine();
        eng->jaibite_cache(false);
        auto conn = std::make_shared<jai::debug::debug_connector>(0);
        eng->set_debug_connector(conn);
        sock_t cli = connect_client(conn->port());
        check_true(cli != kBad);
        if (cli == kBad) return;
        struct client_guard {
            sock_t socket;
            ~client_guard() { close_client(socket); }
        } close{cli};

        std::string buf;
        send_dap(cli, R"({"seq":1,"type":"request","command":"initialize"})");
        check_true(wait_for(cli, buf, "\"event\":\"initialized\""));
        send_dap(cli, R"({"seq":2,"type":"request","command":"attach"})");
        check_true(wait_for(cli, buf, "\"command\":\"attach\""));

        // Change grants after transport attachment: each new request must consult
        // the live engine setting, not a snapshot taken when the connector began.
        set_filesystem_grant(*eng, filesystem_enabled || revoke_after_ack);
        send_dap(cli, R"({"seq":3,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":")"
            + requested_path + R"("},"breakpoints":[{"line":1}]}})");
        check_true(wait_for(cli, buf, "\"command\":\"setBreakpoints\""));
        check_true(wait_for(cli, buf, "\"verified\":false"), "no loaded unit carries the path yet");

        if (alias_update != 0) {
            std::string alias_buf;
            send_dap(cli, R"({"seq":4,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"./breakpoint.jai"},"breakpoints":[)"
                + std::string(alias_update > 0 ? R"({"line":2})" : "") + "]}}");
            check_true(wait_for(cli, alias_buf, "\"command\":\"setBreakpoints\""));
        }
        if (revoke_after_ack) set_filesystem_grant(*eng, false);

        std::string script_error;
        std::atomic<bool> script_done{false};
        std::thread script([&]() {
            try {
                eng->execute_file_source(filesystem_enabled ? normalized_path : requested_path,
                    "int breakpoint_value = 41;\nbreakpoint_value += 1;\n");
            } catch (const std::exception& error) {
                script_error = error.what();
            }
            script_done = true;
        });
        joined_script join{eng->debugger(), script};

        // a script that ran to its end never parked: no stop is coming
        const bool stopped = wait_for_unless(cli, buf, "\"event\":\"stopped\"", script_done);
        int stopped_line = -1;
        if (stopped) {
            std::string frames_buf;
            send_dap(cli, R"({"seq":5,"type":"request","command":"stackTrace","arguments":{"threadId":1}})");
            if (wait_for(cli, frames_buf, "\"stackFrames\"")) {
                stopped_line = extract_int(frames_buf, "line");
            }
        }
        // Disarm before asserting, including the expected-red path: a failed test
        // must not leave a parked script thread behind or destroy a joinable thread.
        eng->debugger().set_enabled(false);
        script.join();
        check_true(script_error.empty(), "in-memory script failed: " + script_error);
        if (alias_update < 0) {
            check_false(stopped, "clearing an equivalent path spelling must remove the earlier breakpoint");
        } else {
            check_true(stopped, filesystem_enabled
                ? "enabled debugger must preserve filesystem path normalization"
                : "restricted debugger must preserve the exact virtual source label");
            check_eq(alias_update > 0 ? 2 : 1, stopped_line,
                "later equivalent path spelling must replace, not merge, the earlier line set");
        }
        check_eq(int64_t(42), eng->get_variable("breakpoint_value").as_int());
        if (alias_update == 0) {
            check_true(wait_for(cli, buf, "\"event\":\"breakpoint\""), "loading the unit verifies the breakpoint");
            check_true(buf.find("\"verified\":true") != std::string::npos);
        }
    }

    void forge_tests() override {
        test("dap_restricted_breakpoints_keep_virtual_source_labels", [this]() {
            check_breakpoint_path(false);
        });

        test("dap_enabled_breakpoints_keep_path_normalization", [this]() {
            check_breakpoint_path(true);
        });

        test("dap_breakpoint_alias_updates_preserve_request_order", [this]() {
            check_breakpoint_path(true, 1);
        });

        test("dap_breakpoint_alias_clear_replaces_previous_spelling", [this]() {
            check_breakpoint_path(true, -1);
        });

        test("dap_breakpoint_normalization_observes_grant_at_execution_sync", [this]() {
            check_breakpoint_path(false, 0, true);
        });

        test("dap_attach_breakpoint_inspect_continue", [this]() {
            wsa_up();

            auto eng = jai::foundry::make_engine();
            auto conn = std::make_shared<jai::debug::debug_connector>(0);   // ephemeral port
            eng->set_debug_connector(conn);
            int port = conn->port();
            check_gt(port, 0);

            sock_t cli = connect_client(port);
            check_true(cli != kBad);
            if (cli == kBad) return;

            std::string buf;

            // --- handshake ---
            send_dap(cli, R"({"seq":1,"type":"request","command":"initialize","arguments":{"adapterID":"jaiscript"}})");
            check_true(wait_for(cli, buf, "\"command\":\"initialize\""));
            check_true(wait_for(cli, buf, "\"event\":\"initialized\""));

            send_dap(cli, R"({"seq":2,"type":"request","command":"attach","arguments":{}})");
            check_true(wait_for(cli, buf, "\"command\":\"attach\""));

            // breakpoint on line 1, keyed to "<script>" (what execute() stamps)
            send_dap(cli, R"({"seq":3,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"<script>"},"breakpoints":[{"line":1}]}})");
            check_true(wait_for(cli, buf, "\"command\":\"setBreakpoints\""));
            check_true(wait_for(cli, buf, "\"verified\":true"));

            send_dap(cli, R"({"seq":4,"type":"request","command":"configurationDone"})");
            check_true(wait_for(cli, buf, "\"command\":\"configurationDone\""));

            // --- run the script on its own thread; it parks at line 1 ---
            std::atomic<bool> done{false};
            std::thread script([&] {
                eng->execute("var x = 1;\nvar y = 2;\nvar z = x + y;\n");
                done = true;
            });
            joined_script join{eng->debugger(), script};

            check_true(wait_for(cli, buf, "\"event\":\"stopped\""));
            check_true(buf.find("\"reason\":\"breakpoint\"") != std::string::npos);

            send_dap(cli, R"({"seq":5,"type":"request","command":"threads"})");
            check_true(wait_for(cli, buf, "\"name\":\"script\""));

            // --- step over the wire: stepIn advances to line 2 (reason "step") ---
            std::string step_buf;
            send_dap(cli, R"({"seq":6,"type":"request","command":"stepIn","arguments":{"threadId":1}})");
            check_true(wait_for(cli, step_buf, "\"event\":\"stopped\""));
            check_true(step_buf.find("\"reason\":\"step\"") != std::string::npos);

            send_dap(cli, R"({"seq":7,"type":"request","command":"stackTrace","arguments":{"threadId":1}})");
            check_true(wait_for(cli, step_buf, "\"stackFrames\""));
            check_true(wait_for(cli, step_buf, "\"line\":2"));
            int frame_id = extract_int(step_buf, "id");   // generation-scoped; changes per stop
            check_gt(frame_id, 0);

            // scopes -> Locals + Globals; the top level's x==1 now (line 1 ran), y/z not declared yet
            std::string scopes_buf;
            std::string sreq = R"({"seq":8,"type":"request","command":"scopes","arguments":{"frameId":)"
                + std::to_string(frame_id) + "}}";
            send_dap(cli, sreq);
            check_true(wait_for(cli, scopes_buf, "\"name\":\"Globals\""));
            check_true(scopes_buf.find("\"name\":\"Locals\"") != std::string::npos);
            int globals_ref = scope_reference(scopes_buf, "Globals");
            check_gt(globals_ref, 0);

            std::string vars_buf;
            std::string vreq = R"({"seq":9,"type":"request","command":"variables","arguments":{"variablesReference":)"
                + std::to_string(globals_ref) + "}}";
            send_dap(cli, vreq);
            check_true(wait_for(cli, vars_buf, "\"command\":\"variables\""));
            check_true(vars_buf.find("\"name\":\"x\"") != std::string::npos);
            check_true(vars_buf.find("\"value\":\"1\"") != std::string::npos);
            check_true(vars_buf.find("\"name\":\"y\"") == std::string::npos);

            std::string eval_buf;
            send_dap(cli, R"({"seq":20,"type":"request","command":"evaluate","arguments":{"expression":" x ","context":"hover"}})");
            check_true(wait_for(cli, eval_buf, "\"command\":\"evaluate\""));
            check_true(eval_buf.find("\"result\":\"1\",\"type\":\"int\"") != std::string::npos, eval_buf);
            eval_buf.clear();
            send_dap(cli, R"({"seq":21,"type":"request","command":"evaluate","arguments":{"expression":"x + 1"}})");
            check_true(wait_for(cli, eval_buf, "\"command\":\"evaluate\""));
            check_true(eval_buf.find("\"success\":false") != std::string::npos, eval_buf);

            // --- next (step over) advances to line 3 ---
            std::string next_buf;
            send_dap(cli, R"({"seq":10,"type":"request","command":"next","arguments":{"threadId":1}})");
            check_true(wait_for(cli, next_buf, "\"event\":\"stopped\""));
            send_dap(cli, R"({"seq":11,"type":"request","command":"stackTrace","arguments":{"threadId":1}})");
            check_true(wait_for(cli, next_buf, "\"line\":3"));

            // --- continue to completion ---
            std::string cont_buf;
            send_dap(cli, R"({"seq":12,"type":"request","command":"continue","arguments":{"threadId":1}})");
            check_true(wait_for(cli, cont_buf, "\"command\":\"continue\""));

            script.join();
            check_true(done.load());
            check_eq(static_cast<int64_t>(3), eng->get_variable("z").as_int());

            send_dap(cli, R"({"seq":13,"type":"request","command":"disconnect"})");
            check_true(wait_for(cli, cont_buf, "\"command\":\"disconnect\""), "the disconnect is answered before the close");
            close_client(cli);
            // eng's destructor stops the connector (joins its thread, detaches the controller).
        });

        // The reviewer's frame loop: a script that sleeps between a few statements is already running when the editor
        // attaches; the breakpoint stops it, and a pause after continue stops it again
        test("dap_sleep_paced_loop_stops_at_a_breakpoint_and_pauses", [this]() {
            wsa_up();
            auto eng = make_engine();
            eng->execution_budget(0);
            jai::stdlib::register_all(eng);
            auto conn = std::make_shared<jai::debug::debug_connector>(0);
            eng->set_debug_connector(conn);
            sock_t cli = connect_client(conn->port());
            check_true(cli != kBad);
            if (cli == kBad) return;
            struct client_guard { sock_t socket; ~client_guard() { close_client(socket); } } close{cli};

            std::string buf;
            send_dap(cli, R"({"seq":1,"type":"request","command":"initialize"})");
            check_true(wait_for(cli, buf, "\"event\":\"initialized\""));

            // the loop runs until quit, however long the attach takes; the host ends it only when the test gives up
            std::atomic<bool> keep_looping{true};
            std::promise<void> looping;
            std::once_flag first_iteration;
            eng->add_function("keep_looping", [&] {
                std::call_once(first_iteration, [&] { looping.set_value(); });
                return keep_looping.load();
            });
            std::string script_error;
            std::thread script([&] {
                try {
                    eng->execute("int frames = 0;\nbool quit = false;\nwhile (!quit && keep_looping()) {\n"
                                 "    sleep(0.02);\n    frames = frames + 1;\n}\n");
                } catch (const std::exception& e) { script_error = e.what(); }
            });
            struct script_guard {
                jai::debug::controller& debugger;
                std::thread& thread;
                std::atomic<bool>& keep_looping;
                ~script_guard() {
                    keep_looping = false;
                    debugger.set_enabled(false);
                    if (thread.joinable()) thread.join();
                }
            } join{eng->debugger(), script, keep_looping};
            check_true(looping.get_future().wait_for(k_hang_guard) == std::future_status::ready);

            send_dap(cli, R"({"seq":2,"type":"request","command":"attach"})");
            check_true(wait_for(cli, buf, "\"command\":\"attach\""));
            send_dap(cli, R"({"seq":3,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"<script>"},"breakpoints":[{"line":5}]}})");
            check_true(wait_for(cli, buf, "\"verified\":true"));
            send_dap(cli, R"({"seq":4,"type":"request","command":"configurationDone"})");
            check_true(wait_for(cli, buf, "\"event\":\"stopped\""), "the breakpoint stops the running loop");
            check_true(buf.find("\"reason\":\"breakpoint\"") != std::string::npos);

            std::string pause_buf;
            send_dap(cli, R"({"seq":5,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"<script>"},"breakpoints":[]}})");
            check_true(wait_for(cli, pause_buf, "\"request_seq\":5"));
            send_dap(cli, R"({"seq":6,"type":"request","command":"continue","arguments":{"threadId":1}})");
            check_true(wait_for(cli, pause_buf, "\"request_seq\":6"));
            send_dap(cli, R"({"seq":7,"type":"request","command":"pause","arguments":{"threadId":1}})");
            check_true(wait_for(cli, pause_buf, "\"event\":\"stopped\""), "the pause stops the running loop");
            check_true(pause_buf.find("\"reason\":\"pause\"") != std::string::npos);

            auto& dbg = eng->debugger();
            dbg.post_command([&] { dbg.set_variable("quit", eng->make_value(true)); });
            std::string end_buf;
            send_dap(cli, R"({"seq":8,"type":"request","command":"continue","arguments":{"threadId":1}})");
            check_true(wait_for(cli, end_buf, "\"request_seq\":8"));
            script.join();
            check_eq(std::string(), script_error);
            check_true(eng->get_variable("quit").as<bool>(), "the paused loop was released through quit");
        });

        // Stopped two calls deep: every frame lists innermost first, each frame's scopes answer its own locals, and the
        // Globals scope answers the program scope
        test("dap_stack_trace_lists_every_frame_with_its_locals_and_globals", [this]() {
            wsa_up();
            auto eng = make_engine();
            auto conn = std::make_shared<jai::debug::debug_connector>(0);
            eng->set_debug_connector(conn);
            sock_t cli = connect_client(conn->port());
            check_true(cli != kBad);
            if (cli == kBad) return;
            struct client_guard { sock_t socket; ~client_guard() { close_client(socket); } } close{cli};

            std::string buf;
            send_dap(cli, R"({"seq":1,"type":"request","command":"initialize"})");
            check_true(wait_for(cli, buf, "\"event\":\"initialized\""));
            send_dap(cli, R"({"seq":2,"type":"request","command":"attach"})");
            check_true(wait_for(cli, buf, "\"command\":\"attach\""));
            send_dap(cli, R"({"seq":3,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"<script>"},"breakpoints":[{"line":4}]}})");
            check_true(wait_for(cli, buf, "\"verified\":true"));

            std::thread script([&] {
                eng->execute("int counter = 7;\n"
                             "function inner(int a) -> int {\n"
                             "    int b = a + 1;\n"
                             "    return b * 2;\n"
                             "}\n"
                             "function tick(int n) -> int {\n"
                             "    int local_tick = n + 10;\n"
                             "    return inner(local_tick);\n"
                             "}\n"
                             "auto result = tick(counter);\n");
            });
            joined_script join{eng->debugger(), script};
            check_true(wait_for(cli, buf, "\"event\":\"stopped\""));

            std::string frames_buf;
            send_dap(cli, R"({"seq":4,"type":"request","command":"stackTrace","arguments":{"threadId":1}})");
            check_true(wait_for(cli, frames_buf, "\"totalFrames\":3"));
            const size_t inner_at = frames_buf.find("\"name\":\"inner\"");
            const size_t tick_at = frames_buf.find("\"name\":\"tick\"");
            const size_t top_at = frames_buf.find("\"name\":\"<script>\",\"line\"");   // the frame, not a source's name
            check_true(inner_at != std::string::npos && tick_at != std::string::npos && top_at != std::string::npos);
            check_true(inner_at < tick_at && tick_at < top_at, "innermost first, the top level last");
            const size_t tick_id_at = frames_buf.rfind("\"id\":", tick_at);
            const int tick_frame = tick_id_at == std::string::npos ? -1 : extract_int(frames_buf.substr(tick_id_at), "id");
            check_gt(tick_frame, 0);
            check_eq(8, extract_int(frames_buf.substr(tick_at), "line"));

            std::string scopes_buf;
            send_dap(cli, R"({"seq":5,"type":"request","command":"scopes","arguments":{"frameId":)" + std::to_string(tick_frame) + "}}");
            check_true(wait_for(cli, scopes_buf, "\"name\":\"Globals\""));
            const int tick_locals = scope_reference(scopes_buf, "Locals");
            const int globals = scope_reference(scopes_buf, "Globals");
            check_gt(tick_locals, 0);
            check_gt(globals, 0);

            std::string locals_buf;
            send_dap(cli, R"({"seq":6,"type":"request","command":"variables","arguments":{"variablesReference":)" + std::to_string(tick_locals) + "}}");
            check_true(wait_for(cli, locals_buf, "\"name\":\"local_tick\",\"value\":\"17\""));
            check_true(locals_buf.find("\"name\":\"n\",\"value\":\"7\"") != std::string::npos);
            check_true(locals_buf.find("\"name\":\"a\"") == std::string::npos, "the caller's frame shows its own locals");

            std::string globals_buf;
            send_dap(cli, R"({"seq":7,"type":"request","command":"variables","arguments":{"variablesReference":)" + std::to_string(globals) + "}}");
            check_true(wait_for(cli, globals_buf, "\"name\":\"counter\",\"value\":\"7\""));

            std::string page_buf;
            send_dap(cli, R"({"seq":8,"type":"request","command":"stackTrace","arguments":{"threadId":1,"startFrame":1,"levels":1}})");
            check_true(wait_for(cli, page_buf, "\"totalFrames\":3"));
            check_true(page_buf.find("\"name\":\"tick\"") != std::string::npos);
            check_true(page_buf.find("\"name\":\"inner\"") == std::string::npos, "startFrame and levels page the stack");

            std::string end_buf;
            send_dap(cli, R"({"seq":9,"type":"request","command":"continue","arguments":{"threadId":1}})");
            check_true(wait_for(cli, end_buf, "\"request_seq\":9"));
            script.join();
            check_eq(int64_t(36), eng->get_variable("result").as_int());
        });

        test("dap_breakpoint_on_a_path_no_unit_carries_stays_unverified", [this]() {
            wsa_up();
            auto eng = make_engine();
            eng->jaibite_cache(false);
            auto conn = std::make_shared<jai::debug::debug_connector>(0);
            eng->set_debug_connector(conn);
            sock_t cli = connect_client(conn->port());
            check_true(cli != kBad);
            if (cli == kBad) return;
            struct client_guard { sock_t socket; ~client_guard() { close_client(socket); } } close{cli};

            std::string buf;
            send_dap(cli, R"({"seq":1,"type":"request","command":"initialize"})");
            check_true(wait_for(cli, buf, "\"event\":\"initialized\""));
            send_dap(cli, R"({"seq":2,"type":"request","command":"attach"})");
            check_true(wait_for(cli, buf, "\"command\":\"attach\""));
            send_dap(cli, R"({"seq":3,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"./__jai_dap_nowhere__/never.jai"},"breakpoints":[{"line":1}]}})");
            check_true(wait_for(cli, buf, "\"command\":\"setBreakpoints\""));
            check_true(buf.find("\"verified\":false") != std::string::npos);
            check_true(buf.find("\"verified\":true") == std::string::npos);
            eng->execute_file_source("./__jai_dap_elsewhere__/other.jai", "int other_value = 1;\n");
            // replies leave in order: a breakpoint event the load raised precedes the reply to a request sent after it
            send_dap(cli, R"({"seq":4,"type":"request","command":"threads"})");
            check_true(wait_for(cli, buf, "\"request_seq\":4"));
            check_true(buf.find("\"event\":\"breakpoint\"") == std::string::npos, "another unit loading verifies nothing");
            eng->debugger().set_enabled(false);
        });

        // A web page can reach a loopback port, but only with HTTP: its request line and headers drop it. A client that has
        // not attached may configure, not drive or read the script, and a pause asked for before the session never stops it.
        test("dap_a_web_request_is_dropped_and_a_client_before_attach_only_configures", [this]() {
            wsa_up();
            auto eng = make_engine();
            auto conn = std::make_shared<jai::debug::debug_connector>(0);
            eng->set_debug_connector(conn);
            sock_t web = connect_client(conn->port());
            check_true(web != kBad);
            if (web == kBad) return;
            const std::string body = R"({"seq":1,"type":"request","command":"attach"})";
            send_raw(web, "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
            check_true(closed_by_peer(web), "an HTTP request is no DAP client");
            close_client(web);

            sock_t cli = connect_client(conn->port());
            check_true(cli != kBad);
            if (cli == kBad) return;
            struct client_guard { sock_t socket; ~client_guard() { close_client(socket); } } close{cli};
            std::string buf;
            send_dap(cli, R"({"seq":1,"type":"request","command":"initialize"})");
            check_true(wait_for(cli, buf, "\"event\":\"initialized\""));
            send_dap(cli, R"({"seq":2,"type":"request","command":"pause","arguments":{"threadId":1}})");
            check_true(wait_for(cli, buf, "\"request_seq\":2"));
            check_true(buf.find("not attached") != std::string::npos, buf);
            send_dap(cli, R"({"seq":3,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"<script>"},"breakpoints":[{"line":9}]}})");
            check_true(wait_for(cli, buf, "\"command\":\"setBreakpoints\""));
            eng->debugger().request_pause();
            send_dap(cli, R"({"seq":4,"type":"request","command":"attach"})");
            check_true(wait_for(cli, buf, "\"command\":\"attach\""));
            eng->execute("var a = 1;\nvar b = 2;\n");
            send_dap(cli, R"({"seq":5,"type":"request","command":"threads"})");
            check_true(wait_for(cli, buf, "\"request_seq\":5"));
            check_false(buf.find("\"event\":\"stopped\"") != std::string::npos, "the pause from before the session is gone");
            eng->debugger().set_enabled(false);
        });

        // A breakpoint binds to the first line at or after it that starts a statement, as the editor is told; one past the last
        // statement stays unverified
        test("dap_a_breakpoint_off_any_statement_moves_to_the_next_one", [this]() {
            wsa_up();
            const auto dir = jai::foundry::test_temp_directory() / "dap_breakpoint_lines";
            std::filesystem::create_directories(dir);
            { std::ofstream(dir / "lines.jai") << "var a = 1;\n\n// a note\nvar b = 2;\n// the end\n"; }
            const std::string path = std::filesystem::canonical(dir / "lines.jai").string();
            auto eng = make_engine();
            eng->jaibite_cache(false);
            auto conn = std::make_shared<jai::debug::debug_connector>(0);
            eng->set_debug_connector(conn);
            sock_t cli = connect_client(conn->port());
            check_true(cli != kBad);
            if (cli == kBad) return;
            struct client_guard { sock_t socket; ~client_guard() { close_client(socket); } } close{cli};
            std::string buf;
            send_dap(cli, R"({"seq":1,"type":"request","command":"initialize"})");
            check_true(wait_for(cli, buf, "\"event\":\"initialized\""));
            send_dap(cli, R"({"seq":2,"type":"request","command":"attach"})");
            check_true(wait_for(cli, buf, "\"command\":\"attach\""));
            std::string escaped;
            for (char c : path) { if (c == '\\') escaped += '\\'; escaped += c; }
            send_dap(cli, R"({"seq":3,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":")" + escaped + R"("},"breakpoints":[{"line":2},{"line":5}]}})");
            check_true(wait_for(cli, buf, "\"command\":\"setBreakpoints\""));
            std::thread script([&] { eng->execute_file(path); });
            joined_script join{eng->debugger(), script};
            std::string stop_buf;
            check_true(wait_for(cli, stop_buf, "\"event\":\"stopped\""));
            check_true(stop_buf.find("\"verified\":true,\"line\":4") != std::string::npos, "the blank line's breakpoint binds to line 4: " + stop_buf);
            check_true(stop_buf.find("\"verified\":false,\"line\":5") != std::string::npos, "nothing starts after the last statement: " + stop_buf);
            send_dap(cli, R"({"seq":4,"type":"request","command":"stackTrace","arguments":{"threadId":1}})");
            check_true(wait_for(cli, stop_buf, "\"stackFrames\""));
            check_true(wait_for(cli, stop_buf, "\"line\":4,"), stop_buf);
            send_dap(cli, R"({"seq":5,"type":"request","command":"continue","arguments":{"threadId":1}})");
            script.join();
            eng->debugger().set_enabled(false);
        });

        // Another process's wildcard listener holds the port: a loopback bind beside it would take its local connections
        test("listen_beside_another_wildcard_listener_on_the_port_fails", [this]() {
            wsa_up();
            sock_t game = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            check_true(game != kBad);
            if (game == kBad) return;
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = 0;
            check_eq(0, ::bind(game, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
            check_eq(0, ::listen(game, 1));
            socklen_t len = sizeof(addr);
            ::getsockname(game, reinterpret_cast<sockaddr*>(&addr), &len);
            const int port = ntohs(addr.sin_port);
            auto eng = make_engine();
            auto conn = jai::debug::listen(*eng, port);
            check_false(conn->listening(), "the debugger does not listen beside another listener on port " + std::to_string(port));
            check_eq(0, conn->port());
            close_client(game);
        });

        test("listen_on_a_busy_port_reports_why", [this]() {
            auto first_engine = jai::foundry::make_engine();
            auto first = jai::debug::listen(*first_engine, 0);
            check_true(first->listening());
            check_true(first->error().empty());
            const int port = first->port();
            check_gt(port, 0);

            auto second_engine = jai::foundry::make_engine();
            auto second = jai::debug::listen(*second_engine, port);
            check_false(second->listening(), "a second listener on a taken port must say it is not listening");
            check_eq(0, second->port());
            check_true(second->error().find("127.0.0.1:" + std::to_string(port)) != std::string::npos,
                "the failure names the address: " + second->error());

            // the failed connector left no callback behind: a stop after it is gone reaches nothing of it
            second_engine->set_debug_connector(nullptr);
            second.reset();
            auto& dbg = second_engine->debugger();
            dbg.set_breakpoints("<script>", {1});
            dbg.set_enabled(true);
            std::thread script([&] { second_engine->execute("var q = 1;\n"); });
            joined_script join{dbg, script};
            for (stall_watch watch; !dbg.is_paused() && !watch.stalled(0);) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            check_true(dbg.is_paused());
            dbg.set_enabled(false);
            script.join();
        });

        test("park_only_request_while_running_errors", [this]() {
            // A stackTrace sent before any stop must get an immediate "not stopped" error,
            // never a silently-queued closure that hangs the client.
            wsa_up();
            auto eng = jai::foundry::make_engine();
            auto conn = std::make_shared<jai::debug::debug_connector>(0);
            eng->set_debug_connector(conn);
            sock_t cli = connect_client(conn->port());
            check_true(cli != kBad);
            if (cli == kBad) return;

            std::string buf;
            send_dap(cli, R"({"seq":1,"type":"request","command":"initialize"})");
            check_true(wait_for(cli, buf, "\"event\":\"initialized\""));
            send_dap(cli, R"({"seq":2,"type":"request","command":"attach"})");
            check_true(wait_for(cli, buf, "\"command\":\"attach\""));

            send_dap(cli, R"({"seq":3,"type":"request","command":"stackTrace","arguments":{"threadId":1}})");
            check_true(wait_for(cli, buf, "\"command\":\"stackTrace\""));
            check_true(buf.find("\"success\":false") != std::string::npos);

            close_client(cli);
        });

        // The listener takes any local client: a header that never ends or a body past any DAP message drops that client
        // (the buffer grew without bound), the listener accepts the next one, and an out-of-range seq answers as seq 0
        test("an_oversized_or_endless_message_drops_the_client", [this]() {
            wsa_up();
            auto eng = jai::foundry::make_engine();
            auto conn = std::make_shared<jai::debug::debug_connector>(0);
            eng->set_debug_connector(conn);
            auto dropped = [&](const std::string& payload) {
                sock_t cli = connect_client(conn->port());
                if (cli == kBad) { return false; }
                for (size_t sent = 0; sent < payload.size();) {
                    int n = ::send(cli, payload.data() + sent, static_cast<int>(std::min<size_t>(payload.size() - sent, 65536)), 0);
                    if (n <= 0) { break; }
                    sent += static_cast<size_t>(n);
                }
                const bool closed = closed_by_peer(cli);
                close_client(cli);
                return closed;
            };
            check_true(dropped("Content-Length: 99999999999\r\n\r\n{}"), "a body past any DAP message");
            check_true(dropped(std::string(70000, 'x')), "a header that never ends");

            sock_t cli = connect_client(conn->port());
            check_true(cli != kBad, "the listener accepts the next client");
            if (cli == kBad) return;
            std::string buf;
            send_dap(cli, R"({"seq":1e300,"type":"request","command":"initialize"})");
            check_true(wait_for(cli, buf, "\"request_seq\":0"), buf);
            close_client(cli);
        });

        test("dap_an_oversized_content_length_drops_the_client", [this]() {
            wsa_up();
            auto eng = jai::foundry::make_engine();
            auto conn = std::make_shared<jai::debug::debug_connector>(0);
            eng->set_debug_connector(conn);
            sock_t cli = connect_client(conn->port());
            check_true(cli != kBad);
            if (cli == kBad) return;
            send_raw(cli, "Content-Length: 1099511627776\r\n\r\n{");
            check_true(closed_by_peer(cli), "a body past the limit drops the client instead of buffering it");
            close_client(cli);
            check_true(answers_initialize(conn->port()), "the listener takes the next client");
        });

        test("dap_a_content_length_past_size_t_drops_the_client", [this]() {
            wsa_up();
            auto eng = jai::foundry::make_engine();
            auto conn = std::make_shared<jai::debug::debug_connector>(0);
            eng->set_debug_connector(conn);
            sock_t cli = connect_client(conn->port());
            check_true(cli != kBad);
            if (cli == kBad) return;
            send_raw(cli, "Content-Length: 18446744073709551620\r\n\r\n{}");
            check_true(closed_by_peer(cli), "a length that wraps size_t is not read as a small one");
            close_client(cli);
        });

        test("dap_an_unterminated_header_block_drops_the_client", [this]() {
            wsa_up();
            auto eng = jai::foundry::make_engine();
            auto conn = std::make_shared<jai::debug::debug_connector>(0);
            eng->set_debug_connector(conn);
            sock_t cli = connect_client(conn->port());
            check_true(cli != kBad);
            if (cli == kBad) return;
            send_raw(cli, std::string(256 * 1024, 'H'));
            check_true(closed_by_peer(cli), "header bytes with no blank line are not buffered without bound");
            close_client(cli);
            check_true(answers_initialize(conn->port()), "the listener takes the next client");
        });

        test("dap_pipelined_requests_and_a_large_one_are_framed_in_linear_work", [this]() {
            // re-copying the rest of the buffer per message, or moving a large body's whole buffer per chunk, is quadratic
            wsa_up();
            auto eng = jai::foundry::make_engine();
            auto conn = std::make_shared<jai::debug::debug_connector>(0);
            eng->set_debug_connector(conn);
            sock_t cli = connect_client(conn->port());
            check_true(cli != kBad);
            if (cli == kBad) return;
            const int requests = 200000;
            std::string burst;
            const std::string one = dap_frame(R"({"seq":1,"type":"request","command":"threads"})");
            burst.reserve(one.size() * requests + 64);
            for (int i = 0; i < requests; ++i) burst += one;
            burst += dap_frame(R"({"seq":2,"type":"request","command":"threads","arguments":{"padding":")" + std::string(8 << 20, 'x') + "\"}}");
            burst += dap_frame(R"({"seq":424242,"type":"request","command":"threads"})");
            std::thread sender([&] { send_raw(cli, burst); });
            size_t replies = 0;
            const bool last = wait_for_streaming(cli, "\"request_seq\":424242", &replies);
            const uint64_t moved = conn->buffered_bytes_moved();
            close_client(cli);
            sender.join();
            check_true(last, "the burst's last request is answered");
            // input moves at most the partial message behind each framed one; output moves less than it sent before each move
            check_true(moved <= burst.size() + 2 * replies, "buffers moved " + std::to_string(moved) + " bytes for "
                + std::to_string(burst.size()) + " received and " + std::to_string(replies) + " sent");
        });

        test("dap_a_client_that_stops_reading_stalls_its_requests_not_the_heap", [this]() {
            wsa_up();
            auto eng = jai::foundry::make_engine();
            auto conn = std::make_shared<jai::debug::debug_connector>(0);
            eng->set_debug_connector(conn);
            sock_t cli = connect_client(conn->port());
            check_true(cli != kBad);
            if (cli == kBad) return;
            set_send_timeout(cli, 2000);
            const std::string one = dap_frame(R"({"seq":1,"type":"request","command":"threads"})");
            std::string batch;
            for (int i = 0; i < 10000; ++i) batch += one;
            size_t sent = 0;
            std::string unsent;
            const size_t ceiling = size_t(256) << 20;
            while (sent < ceiling) {
                const size_t n = send_raw(cli, batch);
                sent += n;
                if (n < batch.size()) {
                    unsent = batch.substr(n);
                    break;
                }
            }
            check_true(sent < ceiling, "the connector stops reading while its replies are unread");
            set_send_timeout(cli, 0);
            std::thread sender([&] { send_raw(cli, unsent + dap_frame(R"({"seq":424242,"type":"request","command":"threads"})")); });
            const bool resumed = wait_for_streaming(cli, "\"request_seq\":424242");
            close_client(cli);
            sender.join();
            check_true(resumed, "reading the replies resumes the connector");
        });
    }
};

} // namespace jai::foundry::tests

using debugger_connector_tests = jai::foundry::tests::debugger_connector_tests;
FOUNDRY_REGISTER(debugger_connector_tests)

#endif // JAISCRIPT_ENABLE_DEBUGGER
