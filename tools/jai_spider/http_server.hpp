#pragma once
// HTTP/1.1 over the runner's non-blocking sockets: one thread, one poll loop, any number of connections reading and
// writing at once, and one request at a time handed to the handler (the script thread). Keep-alive, pipelining,
// Content-Length and chunked request bodies, Expect: 100-continue, HEAD, and files streamed from disk in slices.
// Limits: the request head (64 KiB), the body (2 MiB by default), the time a request may take to arrive and a
// response to leave (30 s) and the open connections (512). A connection holds one response at a time: the next
// request is read once the last response has left, so a client that never reads cannot pile responses up.
#include "../jaiscript/net_socket.hpp"
#include "http_text.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spider_host {

using jai_runner::net_socket;
using jai_runner::net_bad_socket;

struct http_limits {
	size_t max_head_bytes = 64 * 1024;
	size_t max_body_bytes = 2 * 1024 * 1024;
	size_t max_connections = 512;
	size_t max_connections_per_address = 128;
	double timeout_seconds = 30.0;
};

struct http_request {
	std::string method;
	std::string target;
	std::string path;          // percent-decoded, without the query
	std::string query_string;
	std::string version;
	std::map<std::string, std::string> headers;   // lowercased names; repeats joined with ", " (cookies with "; ")
	std::string body;
	std::string remote_address;
	bool keep_alive = true;

	bool is_head() const { return method == "HEAD"; }
	const std::string* header(const std::string& lower_name) const {
		auto it = headers.find(lower_name);
		return it == headers.end() ? nullptr : &it->second;
	}
};

struct http_response {
	int status = 200;
	std::vector<std::pair<std::string, std::string>> headers;
	std::string body;
	std::string file_path;     // when set, the body is this file, streamed; file_size is its length
	uint64_t file_size = 0;
	bool close = false;
	bool stop_server = false;  // the response is the last one: the loop ends once it is sent

	bool has_header(std::string_view name) const {
		for (const auto& [key, value] : headers) {
			if (iequals(key, name)) { return true; }
		}
		return false;
	}
	void set_header(std::string name, std::string value) {
		for (auto& [key, existing] : headers) {
			if (iequals(key, name)) {
				existing = std::move(value);
				return;
			}
		}
		headers.emplace_back(std::move(name), std::move(value));
	}
};

inline http_response plain_response(int status, std::string text) {
	http_response result;
	result.status = status;
	result.body = std::move(text);
	result.body += "\n";
	result.set_header("Content-Type", "text/plain; charset=utf-8");
	return result;
}

class http_server {
public:
	using handler_type = std::function<http_response(const http_request&)>;
	using clock = std::chrono::steady_clock;

	explicit http_server(http_limits limits) : limits_(limits) {}
	http_server(const http_server&) = delete;
	http_server& operator=(const http_server&) = delete;
	~http_server() { close_all(); }

	bool listen(const std::string& host, int port, std::string& error) {
		if (!jai_runner::net_ready()) {
			error = "winsock unavailable";
			return false;
		}
		listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listener_ == net_bad_socket) {
			error = "socket() failed";
			return false;
		}
		jai_runner::net_listen_options(listener_);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(static_cast<unsigned short>(port));
		if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
			error = "not an IPv4 address: " + host;
			close_all();
			return false;
		}
		if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listener_, 64) != 0) {
			error = "cannot listen on " + host + ":" + std::to_string(port) + " (port taken?)";
			close_all();
			return false;
		}
		jai_runner::net_set_nonblocking(listener_);
		sockaddr_in bound{};
		socklen_t length = sizeof(bound);
		::getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &length);
		bound_port_ = ntohs(bound.sin_port);
		return true;
	}

	int port() const { return bound_port_; }

	// the largest request body from the next request read on
	void max_body(size_t a_bytes) { limits_.max_body_bytes = a_bytes; }
	size_t max_body() const { return limits_.max_body_bytes; }

	// told of every request the server answers itself (400, 413, 431, 503, ...): the peer, the status and why
	void on_rejected(std::function<void(const std::string&, int, const std::string&)> callback) { rejected_ = std::move(callback); }

	// runs until stop is raised or a response asks the server to stop; the handler runs on this thread
	void run(const std::atomic<bool>& stop, const handler_type& handler) {
		std::vector<pollfd> polled;
		while (!stop.load() && !stopping_after_flush_done()) {
			polled.clear();
			polled.push_back(pollfd{listener_, POLLIN, 0});
			for (auto& c : connections_) {
				short events = 0;
				if (c->closing || (!c->busy_writing() && !inbox_full(*c))) { events |= POLLIN; }
				if (c->busy_writing()) { events |= POLLOUT; }
				polled.push_back(pollfd{c->sock, events, 0});
			}
#ifdef _WIN32
			int ready = ::WSAPoll(polled.data(), static_cast<ULONG>(polled.size()), 100);
#else
			int ready = ::poll(polled.data(), static_cast<nfds_t>(polled.size()), 100);
#endif
			const auto now = clock::now();
			if (ready > 0 && (polled[0].revents & POLLIN)) { accept_pending(now); }
			for (size_t i = 1; i < polled.size(); ++i) {
				connection& c = *connections_[i - 1];
				const short revents = polled[i].revents;
				if (revents & (POLLIN | POLLHUP | POLLERR)) { read_from(c, now); }
				if (!c.dead && (revents & POLLOUT)) { write_to(c, now); }
			}
			for (auto& c : connections_) {
				if (!c->dead) { advance(*c, handler, now); }
				if (!c->dead && c->busy_writing()) { write_to(*c, now); }
				if (!c->dead && now > c->deadline) { c->dead = true; }
				if (!c->dead && c->closing && !c->busy_writing() && !c->shut_down) {
					// a lingering close: end our side, then drain until the client closes, so unread request bytes
					// cannot turn the close into a reset that loses the response
#ifdef _WIN32
					::shutdown(c->sock, SD_SEND);
#else
					::shutdown(c->sock, SHUT_WR);
#endif
					c->shut_down = true;
					c->deadline = now + std::chrono::seconds(2);
				}
			}
			std::erase_if(connections_, [](const std::unique_ptr<connection>& c) {
				if (c->dead) { jai_runner::net_close_socket(c->sock); }
				return c->dead;
			});
		}
		close_all();
	}

private:
	enum class phase { head, body_length, body_chunked, ready };
	enum class chunk_phase { size_line, data, data_end, trailers };

	struct connection {
		net_socket sock = net_bad_socket;
		std::string remote;
		std::string inbox;
		std::string outbox;
		size_t outbox_sent = 0;
		std::unique_ptr<std::ifstream> file;
		uint64_t file_remaining = 0;
		phase state = phase::head;
		chunk_phase chunk_state = chunk_phase::size_line;
		uint64_t chunk_remaining = 0;
		size_t body_wanted = 0;
		size_t scan = 0;                 // where the next parse of inbox begins
		http_request request;
		clock::time_point deadline;
		bool closing = false;            // send what is queued, then close
		bool shut_down = false;          // our side has ended; reading until the client's side ends
		bool dead = false;
		bool continue_sent = false;

		bool busy_writing() const { return outbox_sent < outbox.size() || file_remaining > 0; }
		size_t unsent() const { return outbox.size() - outbox_sent; }
	};

	bool stopping_after_flush_done() const {
		if (!stop_requested_) { return false; }
		for (const auto& c : connections_) {
			if (!c->dead && c->busy_writing()) { return false; }
		}
		return true;
	}

	void close_all() {
		for (auto& c : connections_) { jai_runner::net_close_socket(c->sock); }
		connections_.clear();
		if (listener_ != net_bad_socket) {
			jai_runner::net_close_socket(listener_);
			listener_ = net_bad_socket;
		}
	}

	std::chrono::steady_clock::duration timeout() const {
		return std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(limits_.timeout_seconds));
	}

	void accept_pending(clock::time_point now) {
		for (;;) {
			sockaddr_in peer{};
			socklen_t length = sizeof(peer);
			net_socket accepted = ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &length);
			if (accepted == net_bad_socket) { return; }
			jai_runner::net_set_nonblocking(accepted);
			int yes = 1;
			::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
#ifdef SO_NOSIGPIPE
			::setsockopt(accepted, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif
			auto c = std::make_unique<connection>();
			c->sock = accepted;
			char text[INET_ADDRSTRLEN] = {0};
			if (::inet_ntop(AF_INET, &peer.sin_addr, text, sizeof(text))) { c->remote = text; }
			c->deadline = now + timeout();
			const size_t from_peer = static_cast<size_t>(std::count_if(connections_.begin(), connections_.end(), [&](const auto& open) { return open->remote == c->remote; }));
			if (connections_.size() >= limits_.max_connections) {
				reject(*c, 503, "too many connections");
			} else if (from_peer >= limits_.max_connections_per_address) {
				reject(*c, 503, "too many connections from one address");
			}
			connections_.push_back(std::move(c));
		}
	}

	void read_from(connection& c, clock::time_point now) {
		if (c.closing) {
			char discard[16384];
			for (;;) {
				int got = static_cast<int>(::recv(c.sock, discard, sizeof(discard), 0));
				if (got > 0) { continue; }
				if ((got == 0 || !jai_runner::net_would_block()) && c.shut_down) { c.dead = true; }
				return;
			}
		}
		char chunk[16384];
		while (!inbox_full(c)) {
			int got = static_cast<int>(::recv(c.sock, chunk, sizeof(chunk), 0));
			if (got > 0) {
				c.inbox.append(chunk, static_cast<size_t>(got));
				continue;
			}
			if (got == 0 || !jai_runner::net_would_block()) {
				if (c.busy_writing()) { c.closing = true; } else { c.dead = true; }
			}
			break;
		}
	}

	void write_to(connection& c, clock::time_point now) {
		for (;;) {
			if (c.outbox_sent == c.outbox.size()) {
				c.outbox.clear();
				c.outbox_sent = 0;
				if (c.file_remaining == 0) { return; }
				const size_t slice = static_cast<size_t>(std::min<uint64_t>(c.file_remaining, 256 * 1024));
				c.outbox.resize(slice);
				c.file->read(c.outbox.data(), static_cast<std::streamsize>(slice));
				const size_t got = static_cast<size_t>(c.file->gcount());
				c.outbox.resize(got);
				c.file_remaining = got == 0 ? 0 : c.file_remaining - got;
				if (got == 0) {
					c.dead = true;   // the file shrank under us: the promised length cannot be kept
					return;
				}
				if (c.file_remaining == 0) { c.file.reset(); }
			}
			const size_t left = c.outbox.size() - c.outbox_sent;
#ifdef MSG_NOSIGNAL
			const int flags = MSG_NOSIGNAL;
#else
			const int flags = 0;
#endif
			int sent = static_cast<int>(::send(c.sock, c.outbox.data() + c.outbox_sent, static_cast<int>(std::min<size_t>(left, 1 << 20)), flags));
			if (sent > 0) {
				c.outbox_sent += static_cast<size_t>(sent);
				c.deadline = now + timeout();
				continue;
			}
			if (!jai_runner::net_would_block()) { c.dead = true; }
			return;
		}
	}

	// unread bytes past one whole request (head and body) wait in the socket: a pipelining client is read as it is answered
	bool inbox_full(const connection& c) const { return c.inbox.size() - c.scan >= limits_.max_head_bytes + limits_.max_body_bytes + 65536; }

	void reject(connection& c, int status, const std::string& why) {
		if (rejected_) { rejected_(c.remote, status, why); }
		queue_response(c, plain_response(status, std::to_string(status) + " " + status_reason(status) + ": " + why), false, true);
		c.closing = true;
		c.inbox.clear();
	}

	// parses what has arrived and answers each complete request, one at a time, while nothing is left to send
	void advance(connection& c, const handler_type& handler, clock::time_point now) {
		while (!c.closing && !c.busy_writing()) {
			if (c.state == phase::head && !parse_head(c)) { return; }
			if (c.closing) { return; }
			if (c.state == phase::body_length) {
				if (c.inbox.size() - c.scan < c.body_wanted) {
					send_continue(c);
					return;
				}
				c.request.body = c.inbox.substr(c.scan, c.body_wanted);
				c.scan += c.body_wanted;
				c.state = phase::ready;
			}
			if (c.state == phase::body_chunked) {
				send_continue(c);
				if (!parse_chunks(c)) { return; }
			}
			if (c.state != phase::ready) { return; }
			if (c.scan >= 65536 && c.scan * 2 >= c.inbox.size()) {   // compacted only once the answered part dominates, so pipelining stays linear
				c.inbox.erase(0, c.scan);
				c.scan = 0;
			}
			http_response response = handler(c.request);
			const bool keep = c.request.keep_alive && !response.close;
			if (response.stop_server) { stop_requested_ = true; }
			queue_response(c, std::move(response), c.request.is_head(), !keep);
			if (!keep) {
				c.closing = true;
				return;
			}
			c.request = http_request{};
			c.state = phase::head;
			c.continue_sent = false;
			c.deadline = now + timeout();
		}
	}

	void send_continue(connection& c) {
		if (c.continue_sent) { return; }
		const std::string* expect = c.request.header("expect");
		if (expect && iequals(*expect, "100-continue") && c.request.version == "HTTP/1.1") {
			c.outbox += "HTTP/1.1 100 Continue\r\n\r\n";
		}
		c.continue_sent = true;
	}

	static bool is_token_char(unsigned char ch) {
		return std::isalnum(ch) || std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(ch)) != std::string_view::npos;
	}

	bool parse_head(connection& c) {
		while (c.inbox.compare(c.scan, 2, "\r\n") == 0) { c.scan += 2; }   // RFC 9112 2.2: empty lines before a request
		const size_t end = c.inbox.find("\r\n\r\n", c.scan);
		if (end == std::string::npos) {
			if (c.inbox.size() - c.scan > limits_.max_head_bytes) {
				const bool line_done = c.inbox.find("\r\n", c.scan) != std::string::npos;
				reject(c, line_done ? 431 : 414, line_done ? "the request head is over the limit" : "the request line is over the limit");
			}
			return false;
		}
		if (end - c.scan > limits_.max_head_bytes) {
			reject(c, 431, "the request head is over the limit");
			return false;
		}
		std::string_view head(c.inbox.data() + c.scan, end - c.scan);
		c.scan = end + 4;
		const size_t line_end = head.find("\r\n");
		std::string_view line = head.substr(0, line_end);
		const size_t first_space = line.find(' ');
		const size_t second_space = first_space == std::string_view::npos ? first_space : line.find(' ', first_space + 1);
		if (second_space == std::string_view::npos || line.find(' ', second_space + 1) != std::string_view::npos) {
			reject(c, 400, "malformed request line");
			return false;
		}
		http_request& request = c.request;
		request.method = std::string(line.substr(0, first_space));
		request.target = std::string(line.substr(first_space + 1, second_space - first_space - 1));
		request.version = std::string(line.substr(second_space + 1));
		request.remote_address = c.remote;
		if (request.method.empty() || !std::all_of(request.method.begin(), request.method.end(), [](char ch) { return is_token_char(static_cast<unsigned char>(ch)); })) {
			reject(c, 400, "malformed method");
			return false;
		}
		if (request.version.rfind("HTTP/", 0) != 0) {
			reject(c, 400, "malformed request line");
			return false;
		}
		if (request.version != "HTTP/1.1" && request.version != "HTTP/1.0") {
			reject(c, 505, "only HTTP/1.0 and HTTP/1.1 are served");
			return false;
		}
		if (!split_target(request)) {
			reject(c, 400, "malformed request target");
			return false;
		}
		size_t position = line_end == std::string_view::npos ? head.size() : line_end + 2;
		while (position < head.size()) {
			size_t next = head.find("\r\n", position);
			if (next == std::string_view::npos) { next = head.size(); }
			std::string_view field = head.substr(position, next - position);
			position = next + 2;
			if (field.find_first_of(std::string_view("\r\n\0", 3)) != std::string_view::npos) {
				reject(c, 400, "a bare CR, LF or NUL in a header field");
				return false;
			}
			if (!field.empty() && (field.front() == ' ' || field.front() == '\t')) {
				reject(c, 400, "obsolete header line folding");
				return false;
			}
			const size_t colon = field.find(':');
			if (colon == std::string_view::npos || colon == 0
					|| !std::all_of(field.begin(), field.begin() + static_cast<std::ptrdiff_t>(colon), [](char ch) { return is_token_char(static_cast<unsigned char>(ch)); })) {
				reject(c, 400, "malformed header field");
				return false;
			}
			std::string name = lowercase(field.substr(0, colon));
			std::string value(trim_spaces(field.substr(colon + 1)));
			auto [it, inserted] = request.headers.emplace(name, value);
			if (!inserted) {
				if (name == "content-length") {
					if (it->second != value) {
						reject(c, 400, "conflicting Content-Length headers");
						return false;
					}
				} else {
					it->second += name == "cookie" ? "; " : ", ";
					it->second += value;
				}
			}
		}
		const std::string* connection_header = request.header("connection");
		const std::string connection_tokens = connection_header ? lowercase(*connection_header) : std::string();
		request.keep_alive = request.version == "HTTP/1.1"
			? connection_tokens.find("close") == std::string::npos
			: connection_tokens.find("keep-alive") != std::string::npos;
		const std::string* transfer = request.header("transfer-encoding");
		const std::string* length = request.header("content-length");
		if (transfer) {
			if (length) {
				reject(c, 400, "both Transfer-Encoding and Content-Length");
				return false;
			}
			if (!iequals(*transfer, "chunked")) {
				reject(c, 501, "the only transfer coding served is chunked");
				return false;
			}
			c.state = phase::body_chunked;
			c.chunk_state = chunk_phase::size_line;
			return true;
		}
		if (length) {
			if (length->empty() || length->size() > 18 || !std::all_of(length->begin(), length->end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
				reject(c, 400, "malformed Content-Length");
				return false;
			}
			const uint64_t wanted = std::stoull(*length);
			if (wanted > limits_.max_body_bytes) {
				reject(c, 413, "the body is over the limit of " + std::to_string(limits_.max_body_bytes) + " bytes");
				return false;
			}
			c.body_wanted = static_cast<size_t>(wanted);
			c.state = c.body_wanted == 0 ? phase::ready : phase::body_length;
			return true;
		}
		c.state = phase::ready;
		return true;
	}

	static bool split_target(http_request& request) {
		std::string_view target = request.target;
		if (target == "*") {
			request.path = "*";
			return true;
		}
		if (target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0) {
			const size_t authority = target.find("//") + 2;
			const size_t slash = target.find('/', authority);
			target = slash == std::string_view::npos ? std::string_view("/") : target.substr(slash);
		}
		if (target.empty() || target.front() != '/') { return false; }
		for (unsigned char ch : target) {
			if (ch <= 0x20 || ch == 0x7F) { return false; }
		}
		const size_t question = target.find('?');
		std::string_view path = target.substr(0, question);
		if (question != std::string_view::npos) { request.query_string = std::string(target.substr(question + 1)); }
		if (const size_t hash = request.query_string.find('#'); hash != std::string::npos) { request.query_string.resize(hash); }
		// each segment decodes on its own: a "." or ".." segment, or a %2F, %5C or NUL inside one, is refused, so the decoded
		// path a handler routes on (a *rest capture handed to spider::file) never climbs
		request.path.clear();
		size_t start = 1;
		while (start <= path.size()) {
			const size_t end = std::min(path.find('/', start), path.size());
			const std::string segment = percent_decode(path.substr(start, end - start));
			if (segment == "." || segment == ".." || segment.find_first_of(std::string_view("/\\\0", 3)) != std::string::npos) { return false; }
			request.path += "/" + segment;
			start = end + 1;
		}
		return true;
	}

	bool parse_chunks(connection& c) {
		http_request& request = c.request;
		for (;;) {
			switch (c.chunk_state) {
				case chunk_phase::size_line: {
					const size_t end = c.inbox.find("\r\n", c.scan);
					if (end == std::string::npos) {
						if (c.inbox.size() - c.scan > 1024) { reject(c, 400, "malformed chunk size"); }
						return false;
					}
					std::string_view line(c.inbox.data() + c.scan, end - c.scan);
					line = line.substr(0, line.find(';'));
					line = trim_spaces(line);
					if (line.empty() || line.size() > 15) {
						reject(c, 400, "malformed chunk size");
						return false;
					}
					uint64_t size = 0;
					for (char ch : line) {
						const int digit = hex_digit_value(ch);
						if (digit < 0) {
							reject(c, 400, "malformed chunk size");
							return false;
						}
						size = size * 16 + static_cast<uint64_t>(digit);
					}
					c.scan = end + 2;
					if (size == 0) {
						c.chunk_state = chunk_phase::trailers;
						break;
					}
					if (request.body.size() + size > limits_.max_body_bytes) {
						reject(c, 413, "the body is over the limit of " + std::to_string(limits_.max_body_bytes) + " bytes");
						return false;
					}
					c.chunk_remaining = size;
					c.chunk_state = chunk_phase::data;
					break;
				}
				case chunk_phase::data: {
					const size_t available = c.inbox.size() - c.scan;
					const size_t take = static_cast<size_t>(std::min<uint64_t>(available, c.chunk_remaining));
					request.body.append(c.inbox, c.scan, take);
					c.scan += take;
					c.chunk_remaining -= take;
					if (c.chunk_remaining > 0) {
						c.inbox.erase(0, c.scan);
						c.scan = 0;
						return false;
					}
					c.chunk_state = chunk_phase::data_end;
					break;
				}
				case chunk_phase::data_end: {
					if (c.inbox.size() - c.scan < 2) { return false; }
					if (c.inbox.compare(c.scan, 2, "\r\n") != 0) {
						reject(c, 400, "malformed chunk");
						return false;
					}
					c.scan += 2;
					c.chunk_state = chunk_phase::size_line;
					break;
				}
				case chunk_phase::trailers: {
					if (c.inbox.compare(c.scan, 2, "\r\n") == 0) {
						c.scan += 2;
						c.state = phase::ready;
						return true;
					}
					const size_t end = c.inbox.find("\r\n\r\n", c.scan);
					if (end == std::string::npos) {
						if (c.inbox.size() - c.scan > limits_.max_head_bytes) { reject(c, 431, "the trailers are over the limit"); }
						return false;
					}
					c.scan = end + 4;
					c.state = phase::ready;
					return true;
				}
			}
		}
	}

	void queue_response(connection& c, http_response response, bool head_only, bool close_after) {
		std::string& out = c.outbox;
		if (c.outbox_sent == out.size()) {
			out.clear();
			c.outbox_sent = 0;
		}
		const bool bodiless = response.status == 204 || response.status == 304 || (response.status >= 100 && response.status < 200);
		uint64_t length = response.file_path.empty() ? response.body.size() : response.file_size;
		out += "HTTP/1.1 " + std::to_string(response.status) + " " + status_reason(response.status) + "\r\n";
		for (const auto& [name, value] : response.headers) {
			if (is_hop_header(name)) { continue; }
			out += name;
			out += ": ";
			out += value;
			out += "\r\n";
		}
		if (!response.has_header("date")) { out += "Date: " + http_date(std::time(nullptr)) + "\r\n"; }
		if (!response.has_header("server")) { out += "Server: jai_spider\r\n"; }
		if (!bodiless) { out += "Content-Length: " + std::to_string(length) + "\r\n"; }
		out += close_after ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n";
		if (head_only || bodiless) { return; }
		if (response.file_path.empty()) {
			out += response.body;
			return;
		}
		auto file = std::make_unique<std::ifstream>(response.file_path, std::ios::binary);
		if (!*file) {
			c.dead = true;
			return;
		}
		c.file = std::move(file);
		c.file_remaining = length;
	}

	http_limits limits_;
	net_socket listener_ = net_bad_socket;
	int bound_port_ = 0;
	std::vector<std::unique_ptr<connection>> connections_;
	bool stop_requested_ = false;
	std::function<void(const std::string&, int, const std::string&)> rejected_;
};

} // namespace spider_host
