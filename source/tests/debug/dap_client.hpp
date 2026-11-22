#pragma once

// A Debug Adapter Protocol client for the debugger suites: it attaches to a jai::debug::debug_connector over a loopback
// socket, sends requests and reads each response and event as a parsed JSON value.

#include <jaiscript/testing/timing.hpp>

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
    #include <sys/select.h>
    #include <unistd.h>
    #include <cerrno>
#endif

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace jai::foundry::dap {

struct json {
    enum class kind_t { null, boolean, number, string, array, object };
    kind_t kind = kind_t::null;
    bool flag = false;
    double number = 0;
    std::string text;
    std::vector<json> items;
    std::vector<std::string> keys;

    const json& operator[](const std::string& key) const {
        static const json none;
        if (kind != kind_t::object) return none;
        for (size_t i = 0; i < keys.size(); ++i) if (keys[i] == key) return items[i];
        return none;
    }
    const json& operator[](size_t index) const {
        static const json none;
        return kind == kind_t::array && index < items.size() ? items[index] : none;
    }
    bool has(const std::string& key) const {
        for (const auto& k : keys) if (k == key) return true;
        return false;
    }
    size_t size() const { return items.size(); }
    int as_int() const { return static_cast<int>(number); }
    const std::string& str() const { return text; }
    bool is_null() const { return kind == kind_t::null; }

    static bool parse(const std::string& s, size_t& i, json& out, int depth = 0) {
        auto ws = [&] { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };
        ws();
        if (i >= s.size() || depth > 200) return false;
        const char c = s[i];
        if (c == '{' || c == '[') {
            const bool object = c == '{';
            out.kind = object ? kind_t::object : kind_t::array;
            ++i;
            ws();
            if (i < s.size() && s[i] == (object ? '}' : ']')) { ++i; return true; }
            for (;;) {
                if (object) {
                    json key;
                    if (!parse(s, i, key, depth + 1) || key.kind != kind_t::string) return false;
                    ws();
                    if (i >= s.size() || s[i] != ':') return false;
                    ++i;
                    out.keys.push_back(key.text);
                }
                json item;
                if (!parse(s, i, item, depth + 1)) return false;
                out.items.push_back(std::move(item));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == (object ? '}' : ']')) { ++i; return true; }
                return false;
            }
        }
        if (c == '"') {
            out.kind = kind_t::string;
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    const char e = s[++i];
                    if (e == 'n') out.text += '\n';
                    else if (e == 't') out.text += '\t';
                    else if (e == 'r') out.text += '\r';
                    else if (e == 'u' && i + 4 < s.size()) {
                        out.text += static_cast<char>(std::strtol(s.substr(i + 1, 4).c_str(), nullptr, 16));
                        i += 4;
                    } else out.text += e;
                    ++i;
                } else {
                    out.text += s[i++];
                }
            }
            if (i >= s.size()) return false;
            ++i;
            return true;
        }
        if (s.compare(i, 4, "true") == 0) { out.kind = kind_t::boolean; out.flag = true; i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { out.kind = kind_t::boolean; i += 5; return true; }
        if (s.compare(i, 4, "null") == 0) { i += 4; return true; }
        const size_t start = i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E')) ++i;
        if (i == start) return false;
        out.kind = kind_t::number;
        out.number = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        return true;
    }
};

// A JSON string literal of text
inline std::string quote(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + "\"";
}

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t k_no_socket = INVALID_SOCKET;
inline void close_socket(socket_t s) { ::closesocket(s); }
#else
using socket_t = int;
inline constexpr socket_t k_no_socket = -1;
inline void close_socket(socket_t s) { ::close(s); }
#endif

inline constexpr int k_nothing_yet = -2;

// The bytes read, 0 once the peer closes, -1 on an error, or k_nothing_yet after 50 quiet ms; readiness is waited for, not a
// receive timeout, which leaves a Windows socket indeterminate once it fires
inline int recv_some(socket_t s, char* into, int size) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s, &readable);
    timeval wait{0, 50000};
    const int ready = ::select(static_cast<int>(s) + 1, &readable, nullptr, nullptr, &wait);
    if (ready == 0) return k_nothing_yet;
    if (ready < 0) {
#ifndef _WIN32
        if (errno == EINTR) return k_nothing_yet;
#endif
        return -1;
    }
    return ::recv(s, into, size, 0);
}

class client {
public:
    explicit client(int port) {
#ifdef _WIN32
        WSADATA w;
        ::WSAStartup(MAKEWORD(2, 2), &w);
#endif
        for (int attempt = 0; attempt < 100 && socket_ == k_no_socket; ++attempt) {
            socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<uint16_t>(port));
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            if (s != k_no_socket && ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                socket_ = s;
                break;
            }
            if (s != k_no_socket) close_socket(s);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    ~client() { if (socket_ != k_no_socket) close_socket(socket_); }
    client(const client&) = delete;
    client& operator=(const client&) = delete;

    bool connected() const { return socket_ != k_no_socket; }

    // Sends a request and returns its response (an empty object when the connection ends or stalls); events read on the way are kept
    json request(const std::string& command, const std::string& arguments = "{}") {
        const int seq = next_seq_++;
        send("{\"seq\":" + std::to_string(seq) + ",\"type\":\"request\",\"command\":\"" + command + "\",\"arguments\":" + arguments + "}");
        for (;;) {
            std::optional<json> message = read_message();
            if (!message) return json{};
            if ((*message)["type"].str() == "response" && (*message)["request_seq"].as_int() == seq) return *message;
            if ((*message)["type"].str() == "event") events_.push_back(std::move(*message));
        }
    }

    // The next event named `name` (others before it are dropped); nullopt once `done` holds with nothing pending, or on a stall
    std::optional<json> wait_event(const std::string& name, const std::function<bool()>& done) {
        for (;;) {
            while (!events_.empty()) {
                json event = std::move(events_.front());
                events_.pop_front();
                if (event["event"].str() == name) return event;
                if (on_event_) on_event_(event);
            }
            std::optional<json> message = read_message(done);
            if (!message) return std::nullopt;
            if ((*message)["type"].str() == "event") events_.push_back(std::move(*message));
        }
    }
    std::optional<json> wait_event(const std::string& name) { return wait_event(name, [] { return false; }); }

    // Every event other than the one waited for passes through here (output events, breakpoint changes)
    void on_event(std::function<void(const json&)> fn) { on_event_ = std::move(fn); }

private:
    void send(const std::string& body) {
        const std::string frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        size_t sent = 0;
        while (sent < frame.size()) {
            const int n = ::send(socket_, frame.data() + sent, static_cast<int>(frame.size() - sent), 0);
            if (n <= 0) return;
            sent += static_cast<size_t>(n);
        }
    }

    // One framed message; nullopt when the peer closes, on a stall, or (quiet_ends) once nothing more arrives
    std::optional<json> read_message(const std::function<bool()>& quiet_ends = {}) {
        stall_watch watch;
        char tmp[8192];
        for (;;) {
            const size_t header_end = buffer_.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const size_t length = std::strtoul(buffer_.c_str() + buffer_.find(':') + 1, nullptr, 10);
                if (buffer_.size() >= header_end + 4 + length) {
                    json message;
                    size_t at = 0;
                    const std::string body = buffer_.substr(header_end + 4, length);
                    buffer_.erase(0, header_end + 4 + length);
                    if (json::parse(body, at, message)) return message;
                    continue;
                }
            }
            if (watch.stalled(buffer_.size())) return std::nullopt;
            const int n = recv_some(socket_, tmp, sizeof(tmp));
            if (n == 0 || n == -1) return std::nullopt;
            if (n > 0) buffer_.append(tmp, tmp + n);
            else if (quiet_ends && quiet_ends()) return std::nullopt;
        }
    }

    socket_t socket_ = k_no_socket;
    std::string buffer_;
    int next_seq_ = 1;
    std::deque<json> events_;
    std::function<void(const json&)> on_event_;
};

} // namespace jai::foundry::dap
