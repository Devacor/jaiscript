// jai::debug::debug_connector — DAP-over-TCP transport. Compiled only when
// JAISCRIPT_ENABLE_DEBUGGER is defined; otherwise this TU is empty and constructing a
// debug_connector is a link error by design (mobile / shipping builds ship no socket code).

#ifdef JAISCRIPT_ENABLE_DEBUGGER

// Platform sockets FIRST — winsock2 must precede anything that might drag in windows.h.
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <poll.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
#endif

#include <jaiscript/debug/connector.hpp>
#include <jaiscript/core/engine.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jai::debug {

namespace {

// ---- platform socket shims -------------------------------------------------
#ifdef _WIN32
    using sock_t = SOCKET;
    using pollfd_t = WSAPOLLFD;
    constexpr sock_t kInvalid = INVALID_SOCKET;
    inline int close_sock(sock_t s) { return ::closesocket(s); }
    inline int last_err() { return ::WSAGetLastError(); }
    inline bool would_block(int e) { return e == WSAEWOULDBLOCK; }
    inline int poll_sock(pollfd_t* fds, int n, int ms) { return ::WSAPoll(fds, static_cast<ULONG>(n), ms); }
    inline void set_nonblocking(sock_t s) { u_long m = 1; ::ioctlsocket(s, FIONBIO, &m); }
#else
    using sock_t = int;
    using pollfd_t = struct pollfd;
    constexpr sock_t kInvalid = -1;
    inline int close_sock(sock_t s) { return ::close(s); }
    inline int last_err() { return errno; }
    inline bool would_block(int e) { return e == EWOULDBLOCK || e == EAGAIN; }
    inline int poll_sock(pollfd_t* fds, int n, int ms) { return ::poll(fds, static_cast<nfds_t>(n), ms); }
    inline void set_nonblocking(sock_t s) { int f = ::fcntl(s, F_GETFL, 0); ::fcntl(s, F_SETFL, f | O_NONBLOCK); }
#endif

std::string socket_error_text(int code) {
    std::string text = std::system_category().message(code);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.' || text.back() == ' ')) text.pop_back();
    return text + ", " + std::to_string(code);
}

inline void set_nodelay(sock_t s) { int yes = 1; ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes)); }

// A self-connected loopback socket pair — identical on Windows/Linux/macOS, and (unlike a
// pipe) pollable by WSAPoll. Used to wake the poll loop for shutdown and outbound writes.
bool make_wake_pair(sock_t& read_end, sock_t& write_end) {
    sock_t listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalid) return false;
    int yes = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { close_sock(listener); return false; }
    if (::listen(listener, 1) != 0) { close_sock(listener); return false; }
    socklen_t len = sizeof(addr);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0) { close_sock(listener); return false; }
    sock_t c = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c == kInvalid) { close_sock(listener); return false; }
    if (::connect(c, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { close_sock(listener); close_sock(c); return false; }
    sock_t a = ::accept(listener, nullptr, nullptr);
    close_sock(listener);
    if (a == kInvalid) { close_sock(c); return false; }
    set_nodelay(a); set_nodelay(c);
    set_nonblocking(a); set_nonblocking(c);
    read_end = a;
    write_end = c;
    return true;
}

// ---- minimal JSON (incoming DAP requests) ----------------------------------
struct jval {
    enum class t { null, boolean, number, string, array, object };
    t kind = t::null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<jval> arr;
    // object members as parallel vectors: a pair<string, jval> member needs jval complete inside its own definition
    std::vector<std::string> keys;
    std::vector<jval> vals;

    const jval* find(const std::string& k) const {
        if (kind != t::object) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i) if (keys[i] == k) return &vals[i];
        return nullptr;
    }
    std::string as_str() const { return kind == t::string ? str : std::string(); }
    // a number outside int (or NaN) is no valid id or line: converting it would be undefined
    int as_int() const { return kind == t::number && num >= INT_MIN && num <= INT_MAX ? static_cast<int>(num) : 0; }
    double as_num() const { return kind == t::number ? num : 0; }
    double get_num(const std::string& k) const { auto* v = find(k); return v ? v->as_num() : 0; }
};

void skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else {
        out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool parse_hex4(const std::string& s, size_t& i, unsigned& cp) {
    if (i + 4 > s.size()) return false;
    cp = 0;
    for (int k = 0; k < 4; ++k) {
        const char h = s[i++];
        cp <<= 4;
        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
        else return false;
    }
    return true;
}

bool parse_string(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return true;
        if (c == '\\') {
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!parse_hex4(s, i, cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && s.compare(i, 2, "\\u") == 0) {   // a high surrogate and its low half: one code point
                        size_t at = i + 2;
                        unsigned low = 0;
                        if (parse_hex4(s, at, low) && low >= 0xDC00 && low < 0xE000) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            i = at;
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: out += e; break;
            }
        } else {
            out += c;
        }
    }
    return false;
}

bool parse_value(const std::string& s, size_t& i, jval& out, int depth) {
    if (depth > 100) return false;
    skip_ws(s, i);
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '"') { out.kind = jval::t::string; return parse_string(s, i, out.str); }
    if (c == '{') {
        ++i; out.kind = jval::t::object;
        skip_ws(s, i);
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        for (;;) {
            skip_ws(s, i);
            std::string key;
            if (!parse_string(s, i, key)) return false;
            skip_ws(s, i);
            if (i >= s.size() || s[i] != ':') return false;
            ++i;
            jval v;
            if (!parse_value(s, i, v, depth + 1)) return false;
            out.keys.push_back(std::move(key));
            out.vals.push_back(std::move(v));
            skip_ws(s, i);
            if (i >= s.size()) return false;
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == '}') { ++i; return true; }
            return false;
        }
    }
    if (c == '[') {
        ++i; out.kind = jval::t::array;
        skip_ws(s, i);
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        for (;;) {
            jval v;
            if (!parse_value(s, i, v, depth + 1)) return false;
            out.arr.push_back(std::move(v));
            skip_ws(s, i);
            if (i >= s.size()) return false;
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == ']') { ++i; return true; }
            return false;
        }
    }
    if (c == 't') { if (s.compare(i, 4, "true") == 0) { i += 4; out.kind = jval::t::boolean; out.b = true; return true; } return false; }
    if (c == 'f') { if (s.compare(i, 5, "false") == 0) { i += 5; out.kind = jval::t::boolean; out.b = false; return true; } return false; }
    if (c == 'n') { if (s.compare(i, 4, "null") == 0) { i += 4; out.kind = jval::t::null; return true; } return false; }
    // number
    size_t start = i;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E')) ++i;
    if (i == start) return false;
    out.kind = jval::t::number;
    out.num = std::strtod(s.substr(start, i - start).c_str(), nullptr);
    return true;
}

bool parse_json(const std::string& s, jval& out) {
    size_t i = 0;
    return parse_value(s, i, out, 0);
}

// ---- outgoing JSON (built by hand; DAP messages are small & fixed-shape) ----
std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    o += buf;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

// A request is a few hundred bytes; a peer past these limits is dropped rather than buffered
constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxMessageBytes = 64 * 1024 * 1024;
constexpr size_t kMaxPendingOutput = 4 * 1024 * 1024;

// A DAP header block is exactly one "Content-Length: N" line: anything else (an HTTP request line a web page's fetch sends,
// any other header) is no debugger, and nullopt drops it. SIZE_MAX for a value past size_t, so the limit check rejects it.
std::optional<size_t> parse_content_length(std::string_view headers) {
    constexpr std::string_view key = "Content-Length:";
    if (headers.substr(0, key.size()) != key) return std::nullopt;
    size_t p = key.size();
    while (p < headers.size() && (headers[p] == ' ' || headers[p] == '\t')) ++p;
    if (p == headers.size()) return std::nullopt;
    size_t v = 0;
    for (; p < headers.size(); ++p) {
        if (!std::isdigit(static_cast<unsigned char>(headers[p]))) return std::nullopt;
        const size_t digit = static_cast<size_t>(headers[p] - '0');
        if (v > (SIZE_MAX - digit) / 10) return SIZE_MAX;
        v = v * 10 + digit;
    }
    return v;
}

} // namespace

// ---------------------------------------------------------------------------
// impl
// ---------------------------------------------------------------------------
struct debug_connector::impl {
    int requested_port = 0;
    std::string host;

    engine* engine_ = nullptr;
    controller* ctrl_ = nullptr;

    std::thread thread_;
    std::atomic<bool> running_{false};
    bool started_ = false;

    sock_t listener_ = kInvalid;
    sock_t client_ = kInvalid;
    sock_t wake_r_ = kInvalid;
    sock_t wake_w_ = kInvalid;
    int bound_port_ = 0;
    std::string error_;   // why the listener is not serving; empty while it is

    bool session_active_ = false;
    bool want_close_client_ = false;
    std::string in_buf_;
    std::string send_buf_;
    size_t send_off_ = 0;   // bytes of send_buf_ already sent
    std::atomic<uint64_t> buffered_bytes_moved_{0};   // written by the connector thread only
    std::atomic<std::thread::id> connector_thread_{};

    // Breakpoints answered unverified because no loaded unit carries their path yet; a
    // `breakpoint` event verifies them when the engine parses that path (script thread).
    struct pending_breakpoint { int id; int line; std::string path; };
    std::mutex pending_mtx_;
    std::unordered_map<std::string, std::vector<pending_breakpoint>> pending_bps_;
    int next_bp_id_ = 1;   // connector thread only

    std::mutex out_mtx_;
    std::deque<std::string> out_q_;
    std::atomic<int> out_seq_{1};
    std::atomic<int> gen_{0};   // suspension generation; encoded in frame/scope handles

#ifdef _WIN32
    bool wsa_ready_ = false;
#endif

    explicit impl(int port, std::string h) : requested_port(port), host(std::move(h)) {
#ifdef _WIN32
        WSADATA wsa;
        wsa_ready_ = (::WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    }

    ~impl() {
#ifdef _WIN32
        if (wsa_ready_) ::WSACleanup();
#endif
    }

    int next_seq() { return out_seq_.fetch_add(1, std::memory_order_relaxed); }

    void poke_wake() {
        if (wake_w_ == kInvalid) return;
        char b = 1;
        ::send(wake_w_, &b, 1, 0);   // 1 byte; a pending byte already wakes the loop
    }

    void drain_wake() {
        char buf[64];
        for (;;) {
            int n = ::recv(wake_r_, buf, sizeof(buf), 0);
            if (n > 0) continue;
            break;
        }
    }

    // Frame a JSON object and queue it. Thread-safe: callable from the script thread
    // (stopped events, posted-closure replies) and the connector thread alike.
    void enqueue(const std::string& json_object) {
        std::string frame = "Content-Length: " + std::to_string(json_object.size()) + "\r\n\r\n" + json_object;
        {
            std::lock_guard<std::mutex> lk(out_mtx_);
            out_q_.push_back(std::move(frame));
        }
        // the connector thread drains its own replies each pass; a wake byte per reply is a syscall per reply
        if (std::this_thread::get_id() != connector_thread_.load(std::memory_order_relaxed)) poke_wake();
    }

    size_t pending_output() const { return send_buf_.size() - send_off_; }

    std::string make_response(int req_seq, const std::string& cmd, bool success,
                              const std::string& body, const std::string& message = "") {
        std::string s = "{\"seq\":" + std::to_string(next_seq())
            + ",\"type\":\"response\",\"request_seq\":" + std::to_string(req_seq)
            + ",\"success\":" + (success ? "true" : "false")
            + ",\"command\":\"" + json_escape(cmd) + "\"";
        if (!message.empty()) s += ",\"message\":\"" + json_escape(message) + "\"";
        if (!body.empty()) s += ",\"body\":" + body;
        s += "}";
        return s;
    }

    std::string make_event(const std::string& event, const std::string& body) {
        std::string s = "{\"seq\":" + std::to_string(next_seq())
            + ",\"type\":\"event\",\"event\":\"" + json_escape(event) + "\"";
        if (!body.empty()) s += ",\"body\":" + body;
        s += "}";
        return s;
    }

    // ---- lifecycle ----
    bool fail_listener(const char* step) {
        const int code = last_err();
        error_ = std::string("debugger cannot listen on ") + host + ":" + std::to_string(requested_port) + ": " + step
            + " failed (" + socket_error_text(code) + ")";
        if (listener_ != kInvalid) { close_sock(listener_); listener_ = kInvalid; }
        return false;
    }

    // Windows' SO_EXCLUSIVEADDRUSE refuses a bind onto a taken port; elsewhere SO_REUSEADDR only lets a TIME_WAIT go
    static void claim_port_options(sock_t s) {
        int yes = 1;
#ifdef _WIN32
        ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif
    }

    bool open_listener() {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(requested_port));
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (requested_port != 0 && addr.sin_addr.s_addr != htonl(INADDR_ANY)) {
            // A specific address binds beside another process's wildcard listener on the port (Windows, BSD) and then takes
            // its loopback connections: a wildcard bind of our own first, which fails exactly when the port is taken
            listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener_ == kInvalid) return fail_listener("socket");
            claim_port_options(listener_);
            sockaddr_in any = addr;
            any.sin_addr.s_addr = htonl(INADDR_ANY);
            if (::bind(listener_, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0) return fail_listener("bind");
            close_sock(listener_);
        }
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == kInvalid) return fail_listener("socket");
        claim_port_options(listener_);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return fail_listener("bind");
        if (::listen(listener_, 1) != 0) return fail_listener("listen");
        socklen_t len = sizeof(addr);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
            bound_port_ = ntohs(addr.sin_port);
        set_nonblocking(listener_);
        return true;
    }

    void begin(engine* eng) {
        engine_ = eng;
        ctrl_ = &eng->debugger();
        if (!open_listener()) return;
        if (!make_wake_pair(wake_r_, wake_w_)) { fail_listener("wake pair"); bound_port_ = 0; return; }
        ctrl_->set_on_stopped([this](const stop_info& si) { on_stop(si); });
        ctrl_->set_on_source_loaded([this](const std::string&) { verify_pending(); });
        running_.store(true, std::memory_order_release);
        started_ = true;
        thread_ = std::thread([this] { run(); });
    }

    void end() {
        running_.store(false, std::memory_order_release);
        poke_wake();
        if (thread_.joinable()) thread_.join();
        // Disarmed once the connector thread is gone (it owns session_active_ and could re-arm a session it was still dispatching);
        // the disarm wakes a parked script thread, so nothing stays blocked in the hook once the engine starts to die
        if (ctrl_ && session_active_) { ctrl_->set_enabled(false); session_active_ = false; }
        if (ctrl_) {
            ctrl_->set_on_stopped(nullptr);
            ctrl_->set_on_source_loaded(nullptr);
        }
        started_ = false;
    }

    void verify_pending() {
        if (!running_.load(std::memory_order_acquire)) return;
        std::vector<pending_breakpoint> ready;
        {
            std::lock_guard<std::mutex> lk(pending_mtx_);
            for (auto it = pending_bps_.begin(); it != pending_bps_.end();) {
                if (ctrl_->source_known(it->first)) {
                    ready.insert(ready.end(), it->second.begin(), it->second.end());
                    it = pending_bps_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& bp : ready) {
            enqueue(make_event("breakpoint", "{\"reason\":\"changed\",\"breakpoint\":" + breakpoint_json(bp.id, bp.path, bp.line, true) + "}"));
        }
    }

    // A breakpoint as the editor shows it: on the line a statement starts at or after the one asked for, unverified when
    // none does or no loaded unit has the path yet
    std::string breakpoint_json(int id, const std::string& path, int line, bool known) const {
        const std::optional<int> bound = known ? ctrl_->breakpoint_line(path, line) : std::optional<int>(line);
        std::string out = "{\"id\":" + std::to_string(id) + ",\"verified\":" + (known && bound ? "true" : "false")
            + ",\"line\":" + std::to_string(bound.value_or(line));
        if (!known) out += ",\"message\":\"" + json_escape("no loaded script has this path yet: " + path) + "\"";
        else if (!bound) out += ",\"message\":\"" + json_escape("no statement starts on or after line " + std::to_string(line)) + "\"";
        return out + "}";
    }

    void end_session() {
        if (session_active_) { ctrl_->set_enabled(false); session_active_ = false; }
    }

    void on_stop(const stop_info& si) {
        if (!running_.load(std::memory_order_acquire)) return;
        gen_.fetch_add(1, std::memory_order_acq_rel);
        std::string reason = si.reason.empty() ? "breakpoint" : si.reason;
        std::string body = "{\"reason\":\"" + json_escape(reason) + "\",\"threadId\":1,\"allThreadsStopped\":true}";
        enqueue(make_event("stopped", body));
    }

    // ---- connector thread ----
    void run() {
        connector_thread_.store(std::this_thread::get_id(), std::memory_order_relaxed);
        while (running_.load(std::memory_order_acquire)) {
            drain_out_into_send_buf();

            pollfd_t fds[3];
            int n = 0, idx_wake = -1, idx_listener = -1, idx_client = -1;
            fds[n].fd = wake_r_; fds[n].events = POLLIN; fds[n].revents = 0; idx_wake = n; ++n;
            if (client_ == kInvalid) {
                fds[n].fd = listener_; fds[n].events = POLLIN; fds[n].revents = 0; idx_listener = n; ++n;
            } else {
                // replies the peer is not reading stop its requests from being read: backpressure, not a growing queue
                short ev = pending_output() < kMaxPendingOutput ? POLLIN : 0;
                if (pending_output() > 0) ev |= POLLOUT;
                fds[n].fd = client_; fds[n].events = ev; fds[n].revents = 0; idx_client = n; ++n;
            }

            int r = poll_sock(fds, n, 1000);
            if (r < 0) { if (would_block(last_err())) continue; break; }
            if (!running_.load(std::memory_order_acquire)) break;

            if (idx_wake >= 0 && (fds[idx_wake].revents & POLLIN)) drain_wake();
            if (idx_listener >= 0 && (fds[idx_listener].revents & POLLIN)) accept_client();
            if (idx_client >= 0) {
                short re = fds[idx_client].revents;
                if (re & POLLIN) read_client();
                if (client_ != kInvalid && (re & POLLOUT)) try_send();
                if (client_ != kInvalid && (re & (POLLERR | POLLHUP | POLLNVAL))) drop_client(true);
            }
            if (want_close_client_) {
                drain_out_into_send_buf();
                if (client_ != kInvalid) try_send();   // the disconnect reply goes out before the close
                drop_client(true);
                want_close_client_ = false;
            }

            drain_out_into_send_buf();
            if (client_ != kInvalid) try_send();
        }

        if (client_ != kInvalid) { close_sock(client_); client_ = kInvalid; }
        if (listener_ != kInvalid) { close_sock(listener_); listener_ = kInvalid; }
        if (wake_r_ != kInvalid) { close_sock(wake_r_); wake_r_ = kInvalid; }
        if (wake_w_ != kInvalid) { close_sock(wake_w_); wake_w_ = kInvalid; }
    }

    void accept_client() {
        sock_t c = ::accept(listener_, nullptr, nullptr);
        if (c == kInvalid) return;
        if (client_ != kInvalid) { close_sock(c); return; }   // single client
        set_nonblocking(c);
        set_nodelay(c);
        client_ = c;
        in_buf_.clear();
        send_buf_.clear();
        send_off_ = 0;
        std::lock_guard<std::mutex> lk(out_mtx_);
        out_q_.clear();
    }

    void drop_client(bool detach) {
        if (client_ == kInvalid) return;
        if (detach) end_session();   // set_enabled(false) -> detach: a parked thread wakes
        close_sock(client_);
        client_ = kInvalid;
        in_buf_.clear();
        send_buf_.clear();
        send_off_ = 0;
        std::lock_guard<std::mutex> lk(out_mtx_);
        out_q_.clear();
    }

    void drain_out_into_send_buf() {
        std::lock_guard<std::mutex> lk(out_mtx_);
        while (!out_q_.empty()) { send_buf_ += out_q_.front(); out_q_.pop_front(); }
    }

    void try_send() {
        while (pending_output() > 0) {
            const int chunk = static_cast<int>(std::min<size_t>(pending_output(), size_t(1) << 20));
            int n = ::send(client_, send_buf_.data() + send_off_, chunk, 0);
            if (n > 0) { send_off_ += static_cast<size_t>(n); continue; }
            if (would_block(last_err())) break;
            drop_client(true);
            return;
        }
        if (send_off_ == send_buf_.size()) {
            send_buf_.clear();
            send_off_ = 0;
        } else if (send_off_ >= kMaxPendingOutput) {
            count_moved(pending_output());
            send_buf_.erase(0, send_off_);
            send_off_ = 0;
        }
    }

    // Each chunk is framed as it arrives, so a peer that streams requests never piles up unparsed input
    void read_client() {
        char buf[4096];
        for (;;) {
            int n = ::recv(client_, buf, sizeof(buf), 0);
            if (n > 0) {
                in_buf_.append(buf, buf + n);
                process_in_buf();
                if (client_ == kInvalid) return;
                drain_out_into_send_buf();
                if (pending_output() >= kMaxPendingOutput) return;
                continue;
            }
            if (n == 0) { drop_client(true); return; }   // EOF
            if (would_block(last_err())) break;
            drop_client(true);
            return;
        }
    }

    void process_in_buf() {
        size_t at = 0;
        for (;;) {
            const size_t hdr_end = in_buf_.find("\r\n\r\n", at);
            if ((hdr_end == std::string::npos ? in_buf_.size() : hdr_end) - at > kMaxHeaderBytes) { drop_client(true); return; }
            if (hdr_end == std::string::npos) break;
            const auto length = parse_content_length(std::string_view(in_buf_).substr(at, hdr_end - at));
            if (!length || *length > kMaxMessageBytes) { drop_client(true); return; }
            const size_t cl = *length;
            const size_t body_start = hdr_end + 4;
            if (in_buf_.size() - body_start < cl) break;   // wait for the rest of the body
            jval msg;
            if (parse_json(in_buf_.substr(body_start, cl), msg)) dispatch(msg);
            at = body_start + cl;
        }
        // an erase of nothing still moves the whole buffer, once per chunk of a large body
        if (at > 0) {
            count_moved(in_buf_.size() - at);
            in_buf_.erase(0, at);
        }
    }

    void count_moved(size_t bytes) {
        buffered_bytes_moved_.store(buffered_bytes_moved_.load(std::memory_order_relaxed) + bytes, std::memory_order_relaxed);
    }

    // ---- DAP request routing ----
    void dispatch(const jval& msg) {
        const jval* cmdv = msg.find("command");
        if (!cmdv) return;
        std::string cmd = cmdv->as_str();
        const jval* seqv = msg.find("seq");
        int req_seq = seqv ? seqv->as_int() : 0;
        const jval* args = msg.find("arguments");

        // before attach a client configures; it neither drives nor reads the script
        static constexpr std::string_view k_session_free[] = {"initialize", "attach", "launch", "disconnect", "setBreakpoints",
                                                              "setExceptionBreakpoints", "configurationDone", "threads"};
        if (!session_active_ && std::find(std::begin(k_session_free), std::end(k_session_free), cmd) == std::end(k_session_free)) {
            enqueue(make_response(req_seq, cmd, false, "", "not attached: send attach first"));
            return;
        }

        if (cmd == "initialize") {
            enqueue(make_response(req_seq, cmd, true, "{\"supportsConfigurationDoneRequest\":true,\"supportsEvaluateForHovers\":true}"));
            enqueue(make_event("initialized", ""));   // tell VS Code to send breakpoints + configurationDone
        } else if (cmd == "attach" || cmd == "launch") {
            ctrl_->set_enabled(true);   // arm breakpoints; suspends the wall-clock budget
            session_active_ = true;
            enqueue(make_response(req_seq, cmd, true, ""));
        } else if (cmd == "configurationDone") {
            enqueue(make_response(req_seq, cmd, true, ""));
        } else if (cmd == "setBreakpoints") {
            handle_set_breakpoints(req_seq, args);
        } else if (cmd == "setExceptionBreakpoints") {
            enqueue(make_response(req_seq, cmd, true, "{\"breakpoints\":[]}"));
        } else if (cmd == "threads") {
            enqueue(make_response(req_seq, cmd, true, "{\"threads\":[{\"id\":1,\"name\":\"script\"}]}"));
        } else if (cmd == "stackTrace") {
            const jval* start = args ? args->find("startFrame") : nullptr;
            const jval* levels = args ? args->find("levels") : nullptr;
            const int first = start ? std::max(0, start->as_int()) : 0;
            const int count = levels ? std::max(0, levels->as_int()) : 0;
            post_park_only(req_seq, cmd, [this, req_seq, first, count] { reply_stack_trace(req_seq, first, count); });
        } else if (cmd == "scopes") {
            const int frame_id = handle_payload(args, "frameId");
            if (frame_id < 0) { enqueue(make_response(req_seq, cmd, false, "", "stale frame")); return; }
            post_park_only(req_seq, cmd, [this, req_seq, frame_id] { reply_scopes(req_seq, frame_id - 1); });
        } else if (cmd == "variables") {
            const int reference = handle_payload(args, "variablesReference");
            if (reference < 0) { enqueue(make_response(req_seq, cmd, false, "", "stale reference")); return; }
            post_park_only(req_seq, cmd, [this, req_seq, reference] { reply_variables(req_seq, reference); });
        } else if (cmd == "continue") {
            ctrl_->resume();
            enqueue(make_response(req_seq, cmd, true, "{\"allThreadsContinued\":true}"));
        } else if (cmd == "next") {
            ctrl_->step_over();
            enqueue(make_response(req_seq, cmd, true, ""));
        } else if (cmd == "stepIn") {
            ctrl_->step_into();
            enqueue(make_response(req_seq, cmd, true, ""));
        } else if (cmd == "stepOut") {
            ctrl_->step_out();
            enqueue(make_response(req_seq, cmd, true, ""));
        } else if (cmd == "pause") {
            ctrl_->request_pause();
            enqueue(make_response(req_seq, cmd, true, ""));
        } else if (cmd == "evaluate") {
            const jval* expression = args ? args->find("expression") : nullptr;
            std::string text = expression ? expression->as_str() : std::string();
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(0, 1);
            post_park_only(req_seq, cmd, [this, req_seq, text] { reply_evaluate(req_seq, text); });
        } else if (cmd == "disconnect") {
            enqueue(make_response(req_seq, cmd, true, ""));
            want_close_client_ = true;   // detach + drop after this reply flushes; listener re-accepts
        } else {
            enqueue(make_response(req_seq, cmd, false, "", "unsupported request: " + cmd));
        }
    }

    // A park-only request needs the parked script thread to read live engine state. If not
    // stopped, answer immediately with an error rather than silently queueing a closure.
    void post_park_only(int req_seq, const std::string& cmd, std::function<void()> reply) {
        if (!ctrl_->post_command_while_paused(std::move(reply))) { enqueue(make_response(req_seq, cmd, false, "", "not stopped")); }
    }

    // A frame id or variables reference: the stop's generation (mod 2048) over a 20-bit payload, a positive int32
    static constexpr int kHandlePayloadBits = 20;
    static constexpr int kHandlePayloadMask = (1 << kHandlePayloadBits) - 1;
    static constexpr int kGlobalsPayload = 1;   // a frame's Locals is 2 + its index; frame ids are 1 + the index
    int handle(int payload) const {
        return ((gen_.load(std::memory_order_acquire) & 0x7FF) << kHandlePayloadBits) | payload;
    }

    // The payload of the handle under `key` when it belongs to this stop, else -1
    int handle_payload(const jval* args, const char* key) const {
        if (!args) return -1;
        const jval* v = args->find(key);
        if (!v) return -1;
        const int h = v->as_int();
        if (h <= 0 || (h >> kHandlePayloadBits) != (gen_.load(std::memory_order_acquire) & 0x7FF)) return -1;
        const int payload = h & kHandlePayloadMask;
        return payload > 0 ? payload : -1;
    }

    void handle_set_breakpoints(int req_seq, const jval* args) {
        std::string path;
        std::vector<int> lines;
        if (args) {
            if (const jval* src = args->find("source"))
                if (const jval* p = src->find("path")) path = p->as_str();
            if (const jval* bps = args->find("breakpoints"); bps && bps->kind == jval::t::array)
                for (const auto& bp : bps->arr)
                    if (const jval* ln = bp.find("line")) lines.push_back(ln->as_int());
            if (lines.empty())
                if (const jval* lv = args->find("lines"); lv && lv->kind == jval::t::array)
                    for (const auto& l : lv->arr) lines.push_back(l.as_int());
        }
        ctrl_->set_source_breakpoints(path, lines);
        const bool known = ctrl_->source_known(path);
        std::vector<pending_breakpoint> answered;
        std::string arr = "[";
        for (size_t i = 0; i < lines.size(); ++i) {
            const int id = next_bp_id_++;
            answered.push_back({id, lines[i], path});
            if (i) arr += ",";
            arr += breakpoint_json(id, path, lines[i], known);
        }
        arr += "]";
        {
            std::lock_guard<std::mutex> lk(pending_mtx_);
            if (known || answered.empty()) pending_bps_.erase(path);
            else pending_bps_[path] = std::move(answered);
        }
        enqueue(make_response(req_seq, "setBreakpoints", true, "{\"breakpoints\":" + arr + "}"));
        if (!known) verify_pending();   // a unit that loaded between the check and the record
    }

    // ---- park-only replies (run on the parked script thread) ----
    // levels 0: every frame from first
    void reply_stack_trace(int req_seq, int first, int levels) {
        auto frames = ctrl_->list_frames();
        const size_t begin = std::min(frames.size(), static_cast<size_t>(first));
        const size_t end = levels > 0 ? std::min(frames.size(), begin + static_cast<size_t>(levels)) : frames.size();
        std::string arr = "[";
        for (size_t i = begin; i < end; ++i) {
            if (i > begin) arr += ",";
            // a frame past the handle space still lists; its scopes answer stale
            const int frame_id = i + 1 <= static_cast<size_t>(kHandlePayloadMask - 2) ? handle(static_cast<int>(i + 1)) : 0;
            arr += "{\"id\":" + std::to_string(frame_id)
                + ",\"name\":\"" + json_escape(frames[i].name) + "\""
                + ",\"line\":" + std::to_string(frames[i].line)
                + ",\"column\":1";
            if (!frames[i].file.empty()) {
                arr += ",\"source\":{\"name\":\"" + json_escape(frames[i].file) + "\",\"path\":\"" + json_escape(frames[i].file) + "\"}";
            }
            arr += "}";
        }
        arr += "]";
        enqueue(make_response(req_seq, "stackTrace", true,
                              "{\"stackFrames\":" + arr + ",\"totalFrames\":" + std::to_string(frames.size()) + "}"));
    }

    void reply_scopes(int req_seq, int frame) {
        std::string body = "{\"scopes\":[{\"name\":\"Locals\",\"presentationHint\":\"locals\",\"variablesReference\":"
            + std::to_string(handle(kGlobalsPayload + 1 + frame)) + ",\"expensive\":false},"
            + "{\"name\":\"Globals\",\"variablesReference\":" + std::to_string(handle(kGlobalsPayload)) + ",\"expensive\":false}]}";
        enqueue(make_response(req_seq, "scopes", true, body));
    }

    // A name and its .field / [index] steps read at the stop; anything else answers unsuccessfully with why
    void reply_evaluate(int req_seq, const std::string& expression) {
        try {
            const variable_info v = ctrl_->evaluate(expression);
            std::string body = "{\"result\":\"" + json_escape(v.value) + "\"";
            if (!v.type.empty()) body += ",\"type\":\"" + json_escape(v.type) + "\"";
            enqueue(make_response(req_seq, "evaluate", true, body + ",\"variablesReference\":0}"));
        } catch (const std::exception& e) {
            enqueue(make_response(req_seq, "evaluate", false, "", e.what()));
        }
    }

    void reply_variables(int req_seq, int reference) {
        auto vars = reference == kGlobalsPayload ? ctrl_->list_globals()
                                                 : ctrl_->list_locals(static_cast<size_t>(reference - kGlobalsPayload - 1));
        std::string arr = "[";
        for (size_t i = 0; i < vars.size(); ++i) {
            if (i) arr += ",";
            arr += "{\"name\":\"" + json_escape(vars[i].name) + "\""
                + ",\"value\":\"" + json_escape(vars[i].value) + "\"";
            if (!vars[i].type.empty()) arr += ",\"type\":\"" + json_escape(vars[i].type) + "\"";
            arr += ",\"variablesReference\":0}";
        }
        arr += "]";
        enqueue(make_response(req_seq, "variables", true, "{\"variables\":" + arr + "}"));
    }
};

// ---------------------------------------------------------------------------
// debug_connector
// ---------------------------------------------------------------------------
debug_connector::debug_connector(int port, std::string host)
    : impl_(std::make_unique<impl>(port, std::move(host))) {}

debug_connector::~debug_connector() {
    if (impl_ && impl_->started_) impl_->end();
}

void debug_connector::start(engine* eng) {
    impl_->begin(eng);
}

void debug_connector::stop() {
    if (impl_->started_) impl_->end();
}

int debug_connector::port() const {
    return impl_->bound_port_;
}

uint64_t debug_connector::buffered_bytes_moved() const {
    return impl_->buffered_bytes_moved_.load(std::memory_order_relaxed);
}

bool debug_connector::listening() const {
    return impl_->started_ && impl_->error_.empty();
}

const std::string& debug_connector::error() const {
    return impl_->error_;
}

std::shared_ptr<debug_connector> listen(engine& eng, int port, std::string host) {
    auto conn = std::make_shared<debug_connector>(port, std::move(host));
    eng.set_debug_connector(conn);
    return conn;
}

} // namespace jai::debug

#endif // JAISCRIPT_ENABLE_DEBUGGER
