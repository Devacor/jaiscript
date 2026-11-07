#pragma once
// A portable jai::http_transport over the operating system's sockets (Winsock, POSIX): HTTP/1.1, one connection per
// exchange (Connection: close), the body framed by Content-Length, chunked or the close, interim 1xx answers skipped,
// every wait bounded by the request's timeout. https goes through a tls_layer the host supplies (openssl_tls.hpp is
// one); without one an https request fails with a clear error. Header-only and opt-in: nothing in the library includes
// it, so a host that never sends HTTP never compiles a socket. Name resolution blocks outside the timeout.
#include <jaiscript/core/http_transport.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace jai {
namespace net {

	using clock = std::chrono::steady_clock;

	// A connected TCP socket in non-blocking mode; every call waits at most until the deadline
	class socket_stream {
	public:
#ifdef _WIN32
		using native_type = SOCKET;
		static constexpr native_type bad_socket = INVALID_SOCKET;
#else
		using native_type = int;
		static constexpr native_type bad_socket = -1;
#endif

		socket_stream() = default;
		socket_stream(const socket_stream&) = delete;
		socket_stream& operator=(const socket_stream&) = delete;
		~socket_stream() { close(); }

		native_type native() const noexcept { return socket_; }

		void close() noexcept {
			if (socket_ != bad_socket) {
#ifdef _WIN32
				::closesocket(socket_);
#else
				::close(socket_);
#endif
				socket_ = bad_socket;
			}
		}

		// false when no address of host:port took the connection before the deadline; a_error says why
		bool connect(const std::string& a_host, int a_port, clock::time_point a_deadline, std::string& a_error) {
			if (!sockets_ready()) {
				a_error = "the socket library did not start";
				return false;
			}
			addrinfo hints {};
			hints.ai_family = AF_UNSPEC;
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_protocol = IPPROTO_TCP;
			addrinfo* found = nullptr;
			const std::string port_text = std::to_string(a_port);
			if (::getaddrinfo(a_host.c_str(), port_text.c_str(), &hints, &found) != 0 || !found) {
				a_error = "cannot resolve host '" + a_host + "'";
				return false;
			}
			bool timed_out = false;
			for (addrinfo* candidate = found; candidate && socket_ == bad_socket; candidate = candidate->ai_next) {
				native_type s = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
				if (s == bad_socket) { continue; }
				set_nonblocking(s);
				int no_delay = 1;
				::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
#ifdef SO_NOSIGPIPE
				int no_sigpipe = 1;
				::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
				socket_ = s;
				const int started = ::connect(s, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen));
				if (started == 0) { break; }
				if (!in_progress()) {
					close();
					continue;
				}
				const int ready = wait(true, a_deadline);
				int failure = 0;
				socklen_t length = sizeof(failure);
				if (ready > 0 && ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&failure), &length) == 0 && failure == 0) { break; }
				timed_out = timed_out || ready == 0;
				close();
			}
			::freeaddrinfo(found);
			if (socket_ == bad_socket) {
				a_error = timed_out ? "timed out" : "cannot connect to " + a_host + ":" + port_text;
				return false;
			}
			return true;
		}

		// 1 ready, 0 the deadline passed, -1 the socket failed
		int wait(bool a_for_write, clock::time_point a_deadline) const {
			while (true) {
				const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(a_deadline - clock::now()).count();
				if (left <= 0) { return 0; }
				const int slice = static_cast<int>(std::min<long long>(left, 1000));
#ifdef _WIN32
				WSAPOLLFD entry {};
				entry.fd = socket_;
				entry.events = a_for_write ? POLLWRNORM : POLLRDNORM;
				const int ready = ::WSAPoll(&entry, 1, slice);
#else
				pollfd entry {};
				entry.fd = socket_;
				entry.events = a_for_write ? POLLOUT : POLLIN;
				const int ready = ::poll(&entry, 1, slice);
				if (ready < 0 && errno == EINTR) { continue; }
#endif
				if (ready < 0) { return -1; }
				if (ready > 0) { return 1; }
			}
		}

		// bytes sent (at least 1), 0 when the socket would block, -1 on failure
		long send_some(const char* a_data, size_t a_size) {
#ifdef _WIN32
			const int sent = ::send(socket_, a_data, static_cast<int>(std::min<size_t>(a_size, 1 << 30)), 0);
			if (sent == SOCKET_ERROR) { return ::WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1; }
#else
#ifdef MSG_NOSIGNAL
			const ssize_t sent = ::send(socket_, a_data, a_size, MSG_NOSIGNAL);
#else
			const ssize_t sent = ::send(socket_, a_data, a_size, 0);
#endif
			if (sent < 0) { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR ? 0 : -1; }
#endif
			return static_cast<long>(sent);
		}

		// bytes read (at least 1), 0 at the peer's close, -1 on failure, -2 when nothing is there yet
		long receive_some(char* a_data, size_t a_size) {
#ifdef _WIN32
			const int got = ::recv(socket_, a_data, static_cast<int>(std::min<size_t>(a_size, 1 << 30)), 0);
			if (got == SOCKET_ERROR) { return ::WSAGetLastError() == WSAEWOULDBLOCK ? -2 : -1; }
#else
			const ssize_t got = ::recv(socket_, a_data, a_size, 0);
			if (got < 0) { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR ? -2 : -1; }
#endif
			return static_cast<long>(got);
		}

	private:
		static bool sockets_ready() {
#ifdef _WIN32
			static const bool ready = [] { WSADATA data; return ::WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
			return ready;
#else
			return true;
#endif
		}

		static void set_nonblocking(native_type a_socket) {
#ifdef _WIN32
			u_long mode = 1;
			::ioctlsocket(a_socket, FIONBIO, &mode);
#else
			::fcntl(a_socket, F_SETFL, ::fcntl(a_socket, F_GETFL, 0) | O_NONBLOCK);
#endif
		}

		static bool in_progress() {
#ifdef _WIN32
			const int code = ::WSAGetLastError();
			return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
#else
			return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EINTR;
#endif
		}

		native_type socket_ = bad_socket;
	};

	// The bytes of one exchange: plain over the socket, or through TLS
	class byte_stream {
	public:
		virtual ~byte_stream() = default;
		// false with a_error set ("timed out" when the deadline passed)
		virtual bool write_all(std::string_view a_bytes, clock::time_point a_deadline, std::string& a_error) = 0;
		// bytes read (at least 1), 0 at the end of the stream, -1 with a_error set
		virtual long read_some(char* a_data, size_t a_size, clock::time_point a_deadline, std::string& a_error) = 0;
	};

	class plain_stream : public byte_stream {
	public:
		explicit plain_stream(socket_stream& a_socket) : socket_(a_socket) {}

		bool write_all(std::string_view a_bytes, clock::time_point a_deadline, std::string& a_error) override {
			while (!a_bytes.empty()) {
				const long sent = socket_.send_some(a_bytes.data(), a_bytes.size());
				if (sent < 0) {
					a_error = "the connection failed while sending";
					return false;
				}
				if (sent == 0) {
					const int ready = socket_.wait(true, a_deadline);
					if (ready <= 0) {
						a_error = ready == 0 ? "timed out" : "the connection failed while sending";
						return false;
					}
					continue;
				}
				a_bytes.remove_prefix(static_cast<size_t>(sent));
			}
			return true;
		}

		long read_some(char* a_data, size_t a_size, clock::time_point a_deadline, std::string& a_error) override {
			while (true) {
				const long got = socket_.receive_some(a_data, a_size);
				if (got >= 0) { return got; }
				if (got == -1) {
					a_error = "the connection failed while receiving";
					return -1;
				}
				const int ready = socket_.wait(false, a_deadline);
				if (ready <= 0) {
					a_error = ready == 0 ? "timed out" : "the connection failed while receiving";
					return -1;
				}
			}
		}

	private:
		socket_stream& socket_;
	};

	// Turns a connected socket into a TLS stream for host (SNI, certificate and name verified), or fails with a_error
	class tls_layer {
	public:
		virtual ~tls_layer() = default;
		virtual std::unique_ptr<byte_stream> secure(socket_stream& a_socket, const std::string& a_host, clock::time_point a_deadline, std::string& a_error) = 0;
	};

	namespace http_wire {

		struct target {
			bool https = false;
			std::string host;          // without IPv6 brackets
			std::string host_header;   // as the Host header spells it
			int port = 80;
			std::string path = "/";
		};

		inline bool split_target(const std::string& a_url, target& a_out, std::string& a_error) {
			std::string_view rest(a_url);
			if (rest.substr(0, 7) == "http://") {
				rest.remove_prefix(7);
			} else if (rest.substr(0, 8) == "https://") {
				rest.remove_prefix(8);
				a_out.https = true;
				a_out.port = 443;
			} else {
				a_error = "not an http or https URL";
				return false;
			}
			const size_t path_start = rest.find_first_of("/?#");
			std::string_view authority = rest.substr(0, path_start);
			if (path_start != std::string_view::npos) {
				std::string_view path = rest.substr(path_start);
				path = path.substr(0, path.find('#'));
				a_out.path = path.empty() || path.front() != '/' ? "/" + std::string(path) : std::string(path);
			}
			if (const size_t at = authority.rfind('@'); at != std::string_view::npos) { authority.remove_prefix(at + 1); }
			std::string_view port_text;
			if (!authority.empty() && authority.front() == '[') {
				const size_t close = authority.find(']');
				if (close == std::string_view::npos) {
					a_error = "malformed host";
					return false;
				}
				a_out.host = std::string(authority.substr(1, close - 1));
				if (close + 1 < authority.size() && authority[close + 1] == ':') { port_text = authority.substr(close + 2); }
			} else {
				const size_t colon = authority.rfind(':');
				a_out.host = std::string(authority.substr(0, colon));
				if (colon != std::string_view::npos) { port_text = authority.substr(colon + 1); }
			}
			if (!port_text.empty()) {
				int port = 0;
				for (char c : port_text) {
					if (c < '0' || c > '9' || port > 65535) {
						a_error = "malformed port";
						return false;
					}
					port = port * 10 + (c - '0');
				}
				if (port < 1 || port > 65535) {
					a_error = "malformed port";
					return false;
				}
				a_out.port = port;
			}
			if (a_out.host.empty()) {
				a_error = "no host";
				return false;
			}
			const bool default_port = a_out.port == (a_out.https ? 443 : 80);
			const std::string bracketed = a_out.host.find(':') != std::string::npos ? "[" + a_out.host + "]" : a_out.host;
			a_out.host_header = default_port ? bracketed : bracketed + ":" + std::to_string(a_out.port);
			return true;
		}

		inline bool same_name(std::string_view a_left, std::string_view a_right) {
			if (a_left.size() != a_right.size()) { return false; }
			for (size_t i = 0; i < a_left.size(); ++i) {
				char l = a_left[i];
				char r = a_right[i];
				if (l >= 'A' && l <= 'Z') { l = static_cast<char>(l - 'A' + 'a'); }
				if (r >= 'A' && r <= 'Z') { r = static_cast<char>(r - 'A' + 'a'); }
				if (l != r) { return false; }
			}
			return true;
		}

		inline std::string request_text(const http_request& a_request, const target& a_target) {
			std::string result = a_request.method + " " + a_target.path + " HTTP/1.1\r\nHost: " + a_target.host_header + "\r\n";
			for (const auto& [name, value] : a_request.headers) {
				if (same_name(name, "host") || same_name(name, "connection") || same_name(name, "content-length") || same_name(name, "transfer-encoding")) { continue; }
				result += name + ": " + value + "\r\n";
			}
			if (!a_request.body.empty() || a_request.method == "POST" || a_request.method == "PUT" || a_request.method == "PATCH") {
				result += "Content-Length: " + std::to_string(a_request.body.size()) + "\r\n";
			}
			result += "Connection: close\r\n\r\n";
			result += a_request.body;
			return result;
		}

		inline std::string_view trimmed(std::string_view a_text) {
			while (!a_text.empty() && (a_text.front() == ' ' || a_text.front() == '\t')) { a_text.remove_prefix(1); }
			while (!a_text.empty() && (a_text.back() == ' ' || a_text.back() == '\t' || a_text.back() == '\r')) { a_text.remove_suffix(1); }
			return a_text;
		}

		// Reads one answer from a stream: the head, then the body as its framing says
		class reader {
		public:
			static constexpr size_t k_max_head = 64 * 1024;
			static constexpr size_t k_max_body = size_t{512} * 1024 * 1024;

			reader(byte_stream& a_stream, clock::time_point a_deadline) : stream_(a_stream), deadline_(a_deadline) {}

			bool read_answer(const std::string& a_method, http_reply& a_reply, std::string& a_error) {
				while (true) {
					a_reply.headers.clear();
					if (!read_head(a_reply, a_error)) { return false; }
					if (a_reply.status >= 200 || a_reply.status == 101) { break; }
				}
				if (a_method == "HEAD" || a_reply.status == 204 || a_reply.status == 304 || a_reply.status < 200) { return true; }
				const std::string* encoding = nullptr;
				const std::string* length = nullptr;
				for (const auto& [name, value] : a_reply.headers) {
					if (same_name(name, "transfer-encoding")) { encoding = &value; }
					if (same_name(name, "content-length")) { length = &value; }
				}
				if (encoding && encoding->find("chunked") != std::string::npos) { return read_chunked(a_reply.body, a_error); }
				if (length) {
					size_t wanted = 0;
					for (char c : trimmed(*length)) {
						if (c < '0' || c > '9' || wanted > k_max_body) {
							a_error = "the answer's Content-Length is malformed";
							return false;
						}
						wanted = wanted * 10 + static_cast<size_t>(c - '0');
					}
					if (wanted > k_max_body) {
						a_error = "the answer is over 512 MiB";
						return false;
					}
					return read_exactly(wanted, a_reply.body, a_error);
				}
				return read_to_close(a_reply.body, a_error);
			}

		private:
			bool fill(std::string& a_error) {
				char chunk[16 * 1024];
				const long got = stream_.read_some(chunk, sizeof(chunk), deadline_, a_error);
				if (got < 0) { return false; }
				if (got == 0) {
					closed_ = true;
					return false;
				}
				buffer_.append(chunk, static_cast<size_t>(got));
				return true;
			}

			bool read_line(std::string& a_line, std::string& a_error) {
				while (true) {
					const size_t end = buffer_.find("\r\n", start_);
					if (end != std::string::npos) {
						a_line.assign(buffer_, start_, end - start_);
						start_ = end + 2;
						return true;
					}
					if (buffer_.size() - start_ > k_max_head) {
						a_error = "the answer's head is over 64 KiB";
						return false;
					}
					if (!fill(a_error)) {
						if (closed_) { a_error = "the connection closed before the answer was complete"; }
						return false;
					}
				}
			}

			bool read_head(http_reply& a_reply, std::string& a_error) {
				std::string line;
				if (!read_line(line, a_error)) { return false; }
				if (line.rfind("HTTP/1.", 0) != 0 || line.size() < 12 || line[8] != ' ') {
					a_error = "the answer is not HTTP/1.x";
					return false;
				}
				int status = 0;
				for (size_t i = 9; i < 12; ++i) {
					if (line[i] < '0' || line[i] > '9') {
						a_error = "the answer's status is malformed";
						return false;
					}
					status = status * 10 + (line[i] - '0');
				}
				a_reply.status = status;
				while (true) {
					if (!read_line(line, a_error)) { return false; }
					if (line.empty()) { return true; }
					const size_t colon = line.find(':');
					if (colon == std::string::npos || colon == 0) {
						a_error = "the answer has a malformed header line";
						return false;
					}
					a_reply.headers.emplace_back(std::string(trimmed(std::string_view(line).substr(0, colon))), std::string(trimmed(std::string_view(line).substr(colon + 1))));
				}
			}

			bool read_exactly(size_t a_count, std::string& a_out, std::string& a_error) {
				while (buffer_.size() - start_ < a_count) {
					if (!fill(a_error)) {
						if (closed_) { a_error = "the connection closed before the answer was complete"; }
						return false;
					}
				}
				a_out.append(buffer_, start_, a_count);
				start_ += a_count;
				return true;
			}

			bool read_to_close(std::string& a_out, std::string& a_error) {
				a_out.append(buffer_, start_, std::string::npos);
				buffer_.clear();
				start_ = 0;
				while (true) {
					char chunk[16 * 1024];
					const long got = stream_.read_some(chunk, sizeof(chunk), deadline_, a_error);
					if (got < 0) { return false; }
					if (got == 0) { return true; }
					a_out.append(chunk, static_cast<size_t>(got));
					if (a_out.size() > k_max_body) {
						a_error = "the answer is over 512 MiB";
						return false;
					}
				}
			}

			bool read_chunked(std::string& a_out, std::string& a_error) {
				std::string line;
				while (true) {
					if (!read_line(line, a_error)) { return false; }
					size_t size = 0;
					size_t digits = 0;
					for (char c : line) {
						const int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
						if (value < 0) { break; }
						size = size * 16 + static_cast<size_t>(value);
						if (++digits > 12) { break; }
					}
					if (digits == 0 || digits > 12 || a_out.size() + size > k_max_body) {
						a_error = digits == 0 || digits > 12 ? "the answer's chunk size is malformed" : "the answer is over 512 MiB";
						return false;
					}
					if (size == 0) {
						while (true) {
							if (!read_line(line, a_error)) { return false; }
							if (line.empty()) { return true; }
						}
					}
					if (!read_exactly(size, a_out, a_error)) { return false; }
					if (!read_line(line, a_error)) { return false; }
					if (!line.empty()) {
						a_error = "the answer's chunk framing is malformed";
						return false;
					}
				}
			}

			byte_stream& stream_;
			clock::time_point deadline_;
			std::string buffer_;
			size_t start_ = 0;
			bool closed_ = false;
		};

	} // namespace http_wire

	class http_socket_transport : public http_transport {
	public:
		explicit http_socket_transport(std::shared_ptr<tls_layer> a_tls = nullptr) : tls_(std::move(a_tls)) {}

		http_reply exchange(const http_request& a_request) override {
			http_reply result;
			const auto deadline = clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(std::max(a_request.timeout_seconds, 0.0)));
			http_wire::target where;
			if (!http_wire::split_target(a_request.url, where, result.error)) { return result; }
			if (where.https && !tls_) {
				result.error = "HTTPS needs a transport with TLS: this one speaks plain HTTP only";
				return result;
			}
			socket_stream socket;
			if (!socket.connect(where.host, where.port, deadline, result.error)) { return result; }
			std::unique_ptr<byte_stream> secured;
			plain_stream plain(socket);
			byte_stream* stream = &plain;
			if (where.https) {
				secured = tls_->secure(socket, where.host, deadline, result.error);
				if (!secured) {
					if (result.error.empty()) { result.error = "the TLS handshake failed"; }
					return result;
				}
				stream = secured.get();
			}
			if (!stream->write_all(http_wire::request_text(a_request, where), deadline, result.error)) { return result; }
			http_wire::reader answer(*stream, deadline);
			if (!answer.read_answer(a_request.method, result, result.error)) {
				result.status = 0;
				result.headers.clear();
				result.body.clear();
			}
			return result;
		}

	private:
		std::shared_ptr<tls_layer> tls_;
	};

} // namespace net
} // namespace jai
