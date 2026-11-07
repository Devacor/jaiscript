#pragma once

#include <string>
#include <utility>
#include <vector>

namespace jai {

	// One HTTP exchange as spider::catch / sling / get / post hand it to the host: the method in capitals, an absolute
	// http or https URL, the request headers (Content-Type and Content-Length included when there is a body; Host,
	// Connection and framing are the transport's), the body bytes and the seconds left for the whole exchange.
	struct http_request {
		std::string method;
		std::string url;
		std::vector<std::pair<std::string, std::string>> headers;
		std::string body;
		double timeout_seconds = 30.0;
	};

	// What came back: the status, every header line in the order it arrived (a repeated name stays repeated) and the
	// whole decoded body (chunked framing removed; no body for HEAD, 1xx, 204 and 304). A failed exchange (no
	// connection, a TLS failure, the time ran out, a malformed answer) leaves status 0 and says why in error.
	struct http_reply {
		int status = 0;
		std::vector<std::pair<std::string, std::string>> headers;
		std::string body;
		std::string error;
	};

	// The host's HTTP stack. exchange() runs one request to its whole answer on the calling thread and follows no
	// redirect: the library applies the redirect policy, the options and the error texts above it, so every transport
	// behaves alike. An exception thrown out of exchange() is reported like an error text. engine::http_transport(t)
	// installs one; jaiscript/net/http_socket_transport.hpp is a portable one over sockets.
	class http_transport {
	public:
		virtual ~http_transport() = default;
		virtual http_reply exchange(const http_request& a_request) = 0;
	};

} // namespace jai
