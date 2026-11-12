#pragma once
// The HTTP transport the runner and jai_spider install for spider::catch / sling / get / post: WinHTTP on Windows (the
// system's TLS and certificate store), elsewhere jai::net::http_socket_transport, with OpenSSL's TLS when the build found
// OpenSSL (JAISCRIPT_TOOLS_HAVE_OPENSSL) and plain HTTP only otherwise. JAISCRIPT_HTTP_TRANSPORT=socket picks the socket
// transport on Windows too (plain HTTP), so its code runs in the Windows gate.
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/core/http_transport.hpp>
#include <jaiscript/stdlib/spider.hpp>
#include <jaiscript/net/http_socket_transport.hpp>
#ifdef JAISCRIPT_TOOLS_HAVE_OPENSSL
#include <jaiscript/net/openssl_tls.hpp>
#endif

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif
#endif

namespace jai_runner {

#ifdef _WIN32
class winhttp_transport : public jai::http_transport {
public:
	winhttp_transport() {
		session_ = WinHttpOpen(L"jaiscript-spider", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	}
	winhttp_transport(const winhttp_transport&) = delete;
	winhttp_transport& operator=(const winhttp_transport&) = delete;
	~winhttp_transport() override {
		if (session_) { WinHttpCloseHandle(session_); }
	}

	jai::http_reply exchange(const jai::http_request& a_request) override {
		jai::http_reply result;
		if (!session_) {
			result.error = "WinHTTP did not start";
			return result;
		}
		const std::wstring url = widen(a_request.url);
		URL_COMPONENTS parts {};
		parts.dwStructSize = sizeof(parts);
		wchar_t host[256] = {};
		wchar_t path[8192] = {};
		wchar_t extra[8192] = {};
		parts.lpszHostName = host;
		parts.dwHostNameLength = 255;
		parts.lpszUrlPath = path;
		parts.dwUrlPathLength = 8191;
		parts.lpszExtraInfo = extra;
		parts.dwExtraInfoLength = 8191;
		if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
			result.error = "malformed URL";
			return result;
		}
		const int timeout_ms = static_cast<int>(std::max(a_request.timeout_seconds, 0.001) * 1000.0);
		handle connection(WinHttpConnect(session_, host, parts.nPort, 0));
		if (!connection.value) {
			result.error = "cannot connect to " + narrow(host) + ":" + std::to_string(parts.nPort);
			return result;
		}
		std::wstring target = std::wstring(path) + extra;
		const size_t fragment = target.find(L'#');
		if (fragment != std::wstring::npos) { target.resize(fragment); }
		if (target.empty()) { target = L"/"; }
		handle request(WinHttpOpenRequest(connection.value, widen(a_request.method).c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
		if (!request.value) {
			result.error = failure("cannot open the request");
			return result;
		}
		deadline_watch watch(request.value, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
		result = run(request.value, a_request, timeout_ms);
		if (watch.finish()) {
			request.value = nullptr;
			result = jai::http_reply{};
			result.error = "timed out";
		}
		return result;
	}

private:
	// WinHTTP's own timeouts do not hold under a few seconds; at the deadline the watch closes the request, ending the blocked call
	class deadline_watch {
	public:
		deadline_watch(HINTERNET a_request, std::chrono::steady_clock::time_point a_deadline) :
			request_(a_request),
			watcher_([this, a_deadline] {
				std::unique_lock<std::mutex> hold(lock_);
				if (!wake_.wait_until(hold, a_deadline, [this] { return done_; })) {
					fired_ = true;
					WinHttpCloseHandle(request_);
				}
			}) {
		}
		deadline_watch(const deadline_watch&) = delete;
		deadline_watch& operator=(const deadline_watch&) = delete;
		~deadline_watch() { finish(); }

		// true when the deadline closed the request
		bool finish() {
			{
				std::lock_guard<std::mutex> hold(lock_);
				done_ = true;
			}
			wake_.notify_one();
			if (watcher_.joinable()) { watcher_.join(); }
			return fired_;
		}

	private:
		HINTERNET request_;
		std::mutex lock_;
		std::condition_variable wake_;
		bool done_ = false;
		bool fired_ = false;
		std::thread watcher_;
	};

	jai::http_reply run(HINTERNET a_request, const jai::http_request& a_exchange, int a_timeout_ms) {
		jai::http_reply result;
		WinHttpSetTimeouts(a_request, a_timeout_ms, a_timeout_ms, a_timeout_ms, a_timeout_ms);
		DWORD response_timeout = static_cast<DWORD>(a_timeout_ms);
		WinHttpSetOption(a_request, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &response_timeout, sizeof(response_timeout));
		DWORD no_redirects = WINHTTP_DISABLE_REDIRECTS;
		WinHttpSetOption(a_request, WINHTTP_OPTION_DISABLE_FEATURE, &no_redirects, sizeof(no_redirects));
		DWORD no_cookies = WINHTTP_DISABLE_COOKIES;
		WinHttpSetOption(a_request, WINHTTP_OPTION_DISABLE_FEATURE, &no_cookies, sizeof(no_cookies));
		std::wstring headers;
		for (const auto& [name, value] : a_exchange.headers) { headers += widen(name) + L": " + widen(value) + L"\r\n"; }
		const BOOL sent = WinHttpSendRequest(a_request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : static_cast<DWORD>(-1L),
			a_exchange.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(a_exchange.body.data()), static_cast<DWORD>(a_exchange.body.size()),
			static_cast<DWORD>(a_exchange.body.size()), 0);
		if (!sent || !WinHttpReceiveResponse(a_request, nullptr)) {
			result.error = failure("the request failed");
			return result;
		}
		DWORD status = 0;
		DWORD size = sizeof(status);
		WinHttpQueryHeaders(a_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
		result.status = static_cast<int>(status);
		DWORD raw_size = 0;
		WinHttpQueryHeaders(a_request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &raw_size, WINHTTP_NO_HEADER_INDEX);
		if (raw_size > 0) {
			std::wstring raw(raw_size / sizeof(wchar_t), L'\0');
			if (WinHttpQueryHeaders(a_request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &raw_size, WINHTTP_NO_HEADER_INDEX)) {
				read_header_lines(narrow(raw.c_str()), result);
			}
		}
		while (true) {
			DWORD available = 0;
			if (!WinHttpQueryDataAvailable(a_request, &available)) {
				result = jai::http_reply{};
				result.error = failure("reading the answer failed");
				return result;
			}
			if (available == 0) { break; }
			const size_t at = result.body.size();
			if (at + available > jai::net::http_wire::reader::k_max_body) {   // the socket transport's cap
				result = jai::http_reply{};
				result.error = "the answer is over 512 MiB";
				return result;
			}
			result.body.resize(at + available);
			DWORD got = 0;
			if (!WinHttpReadData(a_request, result.body.data() + at, available, &got)) {
				result = jai::http_reply{};
				result.error = failure("reading the answer failed");
				return result;
			}
			result.body.resize(at + got);
		}
		return result;
	}

	struct handle {
		explicit handle(HINTERNET a_value) : value(a_value) {}
		handle(const handle&) = delete;
		handle& operator=(const handle&) = delete;
		~handle() {
			if (value) { WinHttpCloseHandle(value); }
		}
		HINTERNET value;
	};

	static std::wstring widen(const std::string& a_text) {
		if (a_text.empty()) { return {}; }
		const int length = MultiByteToWideChar(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0);
		std::wstring result(static_cast<size_t>(length), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), result.data(), length);
		return result;
	}

	static std::string narrow(const wchar_t* a_text) {
		const int length = WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
		if (length <= 1) { return {}; }
		std::string result(static_cast<size_t>(length - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, a_text, -1, result.data(), length, nullptr, nullptr);
		return result;
	}

	static std::string failure(const char* a_what) {
		const DWORD code = GetLastError();
		switch (code) {
			case ERROR_WINHTTP_TIMEOUT: return "timed out";
			case ERROR_WINHTTP_CANNOT_CONNECT: return "cannot connect";
			case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "cannot resolve the host";
			case ERROR_WINHTTP_SECURE_FAILURE: return "the TLS certificate did not verify";
			default: return std::string(a_what) + " (WinHTTP error " + std::to_string(code) + ")";
		}
	}

	// the status line first, then name: value lines; WinHTTP keeps repeated names as separate lines
	static void read_header_lines(const std::string& a_raw, jai::http_reply& a_reply) {
		size_t start = a_raw.find("\r\n");
		while (start != std::string::npos && start + 2 < a_raw.size()) {
			start += 2;
			const size_t end = a_raw.find("\r\n", start);
			const std::string line = a_raw.substr(start, end == std::string::npos ? std::string::npos : end - start);
			const size_t colon = line.find(':');
			if (colon != std::string::npos && colon > 0) {
				size_t value_start = colon + 1;
				while (value_start < line.size() && line[value_start] == ' ') { ++value_start; }
				a_reply.headers.emplace_back(line.substr(0, colon), line.substr(value_start));
			}
			start = end;
		}
	}

	HINTERNET session_ = nullptr;
};
#endif

// a_description names what this build's transport speaks: "WinHTTP", "sockets with OpenSSL" or "sockets (plain HTTP)"
inline std::shared_ptr<jai::http_transport> make_http_transport(std::string* a_description = nullptr) {
#ifdef _WIN32
	char* choice = nullptr;
	size_t length = 0;
	const bool sockets_chosen = _dupenv_s(&choice, &length, "JAISCRIPT_HTTP_TRANSPORT") == 0 && choice && std::strcmp(choice, "socket") == 0;
	std::free(choice);
	if (!sockets_chosen) {
		if (a_description) { *a_description = "WinHTTP"; }
		return std::make_shared<winhttp_transport>();
	}
#endif
#ifdef JAISCRIPT_TOOLS_HAVE_OPENSSL
	if (a_description) { *a_description = "sockets with OpenSSL"; }
	return std::make_shared<jai::net::http_socket_transport>(std::make_shared<jai::net::openssl_tls>());
#else
	if (a_description) { *a_description = "sockets (plain HTTP)"; }
	return std::make_shared<jai::net::http_socket_transport>();
#endif
}

// One transport for the process, shared by every engine on every thread: a WinHTTP session takes concurrent requests, the
// socket transport keeps each exchange's state on its caller's stack and openssl_tls only reads its context
inline const std::shared_ptr<jai::http_transport>& shared_http_transport() {
	static const std::shared_ptr<jai::http_transport> result = make_http_transport();
	return result;
}

// spider:: with this build's transport and the network permission granted
inline void register_http_client(jai::engine& a_engine) {
	jai::stdlib::register_spider(a_engine);
	a_engine.http_transport(shared_http_transport());
	a_engine.enable(jai::permissions::network);
}

} // namespace jai_runner
