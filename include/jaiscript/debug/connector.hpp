#pragma once

// jai::debug::debug_connector — an OPTIONAL, self-contained DAP-over-TCP debug transport.
//
// It speaks the Debug Adapter Protocol directly on a raw OS socket (#ifdef _WIN32 WinSock,
// else BSD sockets), runs on its own std::thread, and drives an engine's debug::controller.
// No asio, no Node adapter, no external tool — VS Code's built-in DAP client attaches to the
// port via a DebugAdapterServer descriptor (see tools/vscode-jaiscript).
//
// Compiled only when JAISCRIPT_ENABLE_DEBUGGER is defined (a CMake option, off in shipping /
// mobile builds). The always-built core links no socket code; constructing a debug_connector
// in a build without the option is a link error by design.

#include <jaiscript/debug/controller.hpp>   // debug::transport base
#include <memory>
#include <string>

namespace jai {
class engine;

namespace debug {

// Default DAP port. 52472 = phone-keypad "JAISC" (well clear of common service ports).
constexpr int default_port = 52472;

class debug_connector : public transport {
public:
    // Listens on host:port (default loopback). Nothing is served until start() (called by
    // engine::set_debug_connector). A session arms the controller on the DAP `attach`.
    explicit debug_connector(int port, std::string host = "127.0.0.1");
    ~debug_connector() override;

    debug_connector(const debug_connector&) = delete;
    debug_connector& operator=(const debug_connector&) = delete;

    void start(engine* eng) override;   // open the listener, launch the connector thread
    void stop() override;               // disarm + detach the controller, join the thread

    int port() const;                   // the bound port (useful when constructed with 0); 0 when not listening
    bool listening() const;             // started and serving
    uint64_t buffered_bytes_moved() const;   // bytes moved within the input and output buffers: linear in the bytes exchanged
    const std::string& error() const;   // why not: "debugger cannot listen on 127.0.0.1:52472: bind failed (...)"

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// Convenience: build a debug_connector on host:port and attach it to `eng`, returning it.
// Collapses the host-side wiring to one line (a host still owns any "wait for attach" UI):
//   auto conn = jai::debug::listen(*eng);            // default port
//   auto conn = jai::debug::listen(*eng, 40000);     // custom port
//   if (!conn->listening()) log(conn->error());      // a taken port does not throw
std::shared_ptr<debug_connector> listen(engine& eng, int port = default_port,
                                        std::string host = "127.0.0.1");

} // namespace debug
} // namespace jai
