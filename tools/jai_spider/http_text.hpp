#pragma once
// The text forms HTTP speaks that a web host reads and writes itself: the request path's percent-decoding, cookie
// headers, multipart bodies, HTTP dates and the content types static files are served with. Pure functions over bytes;
// the URL functions scripts call (url_encode, parse_query, ...) are the standard library's.
#include <jaiscript/jaiscript.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace spider_host {

inline int hex_digit_value(char c) {
	if (c >= '0' && c <= '9') { return c - '0'; }
	if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
	if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
	return -1;
}

inline std::string lowercase(std::string_view text) {
	std::string result(text);
	for (char& c : result) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
	return result;
}

inline bool iequals(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) { return false; }
	for (size_t i = 0; i < a.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) { return false; }
	}
	return true;
}

// the headers that describe one connection, never forwarded (RFC 9110 7.6.1); the server writes its own framing
inline bool is_hop_header(std::string_view name) {
	for (std::string_view hop : {"connection", "keep-alive", "proxy-connection", "te", "trailer", "transfer-encoding", "upgrade", "content-length"}) {
		if (iequals(name, hop)) { return true; }
	}
	return false;
}

inline std::string_view trim_spaces(std::string_view text) {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) { text.remove_prefix(1); }
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) { text.remove_suffix(1); }
	return text;
}

// %XX becomes its byte; a '%' not followed by two hex digits stays as written ('+' stays: a path is not a form)
inline std::string percent_decode(std::string_view text) {
	std::string result;
	result.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i) {
		char c = text[i];
		if (c == '%' && i + 2 < text.size()) {
			int high = hex_digit_value(text[i + 1]);
			int low = hex_digit_value(text[i + 2]);
			if (high >= 0 && low >= 0) {
				result += static_cast<char>(high * 16 + low);
				i += 2;
				continue;
			}
		}
		result += c;
	}
	return result;
}

// Cookie: a=1; b=2 (a value may be quoted; the first of a repeated name wins, as browsers send the most specific first)
inline std::map<std::string, std::string> parse_cookies(std::string_view header) {
	std::map<std::string, std::string> result;
	size_t start = 0;
	while (start < header.size()) {
		size_t end = header.find(';', start);
		if (end == std::string_view::npos) { end = header.size(); }
		std::string_view pair = trim_spaces(header.substr(start, end - start));
		size_t equals = pair.find('=');
		if (equals != std::string_view::npos && equals > 0) {
			std::string_view value = trim_spaces(pair.substr(equals + 1));
			if (value.size() >= 2 && value.front() == '"' && value.back() == '"') { value = value.substr(1, value.size() - 2); }
			result.emplace(std::string(trim_spaces(pair.substr(0, equals))), percent_decode(value));
		}
		start = end + 1;
	}
	return result;
}

// One parameter of a header value: boundary from `multipart/form-data; boundary=x`, name from a Content-Disposition
inline std::string header_parameter(std::string_view value, std::string_view name) {
	size_t start = value.find(';');
	while (start != std::string_view::npos) {
		size_t end = start + 1;
		bool quoted = false;
		while (end < value.size() && (quoted || value[end] != ';')) {
			if (value[end] == '"') { quoted = !quoted; }
			++end;
		}
		std::string_view pair = trim_spaces(value.substr(start + 1, end - start - 1));
		size_t equals = pair.find('=');
		if (equals != std::string_view::npos && iequals(trim_spaces(pair.substr(0, equals)), name)) {
			std::string_view text = trim_spaces(pair.substr(equals + 1));
			if (text.size() >= 2 && text.front() == '"' && text.back() == '"') { text = text.substr(1, text.size() - 2); }
			return std::string(text);
		}
		start = end < value.size() ? end : std::string_view::npos;
	}
	return "";
}

struct multipart_part {
	std::string name;
	std::string filename;
	std::string content_type;
	std::string data;
	bool has_filename = false;
};

// RFC 7578: parts between --boundary lines, each with its own headers; false when the body is not well formed
inline bool parse_multipart(std::string_view body, std::string_view content_type, std::vector<multipart_part>& parts) {
	const std::string boundary = header_parameter(content_type, "boundary");
	if (boundary.empty()) { return false; }
	const std::string delimiter = "--" + boundary;
	size_t position = body.find(delimiter);
	if (position == std::string_view::npos) { return false; }
	for (;;) {
		position += delimiter.size();
		if (body.substr(position, 2) == "--") { return true; }
		if (body.substr(position, 2) != "\r\n") { return false; }
		position += 2;
		size_t headers_end = body.find("\r\n\r\n", position);
		if (headers_end == std::string_view::npos) { return false; }
		multipart_part part;
		std::string_view headers = body.substr(position, headers_end - position);
		size_t line_start = 0;
		while (line_start <= headers.size()) {
			size_t line_end = headers.find("\r\n", line_start);
			if (line_end == std::string_view::npos) { line_end = headers.size(); }
			std::string_view line = headers.substr(line_start, line_end - line_start);
			size_t colon = line.find(':');
			if (colon != std::string_view::npos) {
				std::string_view header_name = trim_spaces(line.substr(0, colon));
				std::string_view header_value = trim_spaces(line.substr(colon + 1));
				if (iequals(header_name, "content-disposition")) {
					part.name = header_parameter(header_value, "name");
					std::string disposition_lower = lowercase(header_value);
					part.has_filename = disposition_lower.find("filename=") != std::string::npos;
					part.filename = header_parameter(header_value, "filename");
				} else if (iequals(header_name, "content-type")) {
					part.content_type = std::string(header_value);
				}
			}
			line_start = line_end + 2;
		}
		size_t data_start = headers_end + 4;
		size_t next = body.find("\r\n" + delimiter, data_start);
		if (next == std::string_view::npos) { return false; }
		part.data = std::string(body.substr(data_start, next - data_start));
		parts.push_back(std::move(part));
		position = next + 2;
	}
}

// RFC 7231 IMF-fixdate: Sun, 06 Nov 1994 08:49:37 GMT
inline std::string http_date(std::time_t when) {
	std::tm utc{};
#ifdef _WIN32
	gmtime_s(&utc, &when);
#else
	gmtime_r(&when, &utc);
#endif
	static const char* const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	char text[40];
	std::snprintf(text, sizeof(text), "%s, %02d %s %04d %02d:%02d:%02d GMT", days[utc.tm_wday], utc.tm_mday, months[utc.tm_mon],
		utc.tm_year + 1900, utc.tm_hour, utc.tm_min, utc.tm_sec);
	return text;
}

inline std::string content_type_for(std::string_view extension) {
	static const std::pair<std::string_view, std::string_view> types[] = {
		{".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
		{".js", "text/javascript; charset=utf-8"}, {".mjs", "text/javascript; charset=utf-8"}, {".json", "application/json"},
		{".txt", "text/plain; charset=utf-8"}, {".md", "text/markdown; charset=utf-8"}, {".csv", "text/csv; charset=utf-8"},
		{".xml", "application/xml"}, {".svg", "image/svg+xml"}, {".png", "image/png"}, {".jpg", "image/jpeg"},
		{".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".webp", "image/webp"}, {".avif", "image/avif"}, {".ico", "image/x-icon"},
		{".bmp", "image/bmp"}, {".woff", "font/woff"}, {".woff2", "font/woff2"}, {".ttf", "font/ttf"}, {".otf", "font/otf"},
		{".wasm", "application/wasm"}, {".pdf", "application/pdf"}, {".zip", "application/zip"}, {".mp3", "audio/mpeg"},
		{".ogg", "audio/ogg"}, {".wav", "audio/wav"}, {".mp4", "video/mp4"}, {".webm", "video/webm"}, {".map", "application/json"},
		{".jai", "text/plain; charset=utf-8"},
	};
	const std::string lower = lowercase(extension);
	for (const auto& [ext, type] : types) {
		if (lower == ext) { return std::string(type); }
	}
	return "application/octet-stream";
}

inline std::string status_reason(int status) {
	switch (status) {
		case 100: return "Continue";
		case 200: return "OK";
		case 201: return "Created";
		case 202: return "Accepted";
		case 204: return "No Content";
		case 206: return "Partial Content";
		case 301: return "Moved Permanently";
		case 302: return "Found";
		case 303: return "See Other";
		case 304: return "Not Modified";
		case 307: return "Temporary Redirect";
		case 308: return "Permanent Redirect";
		case 400: return "Bad Request";
		case 401: return "Unauthorized";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 408: return "Request Timeout";
		case 409: return "Conflict";
		case 410: return "Gone";
		case 411: return "Length Required";
		case 413: return "Content Too Large";
		case 414: return "URI Too Long";
		case 415: return "Unsupported Media Type";
		case 422: return "Unprocessable Content";
		case 429: return "Too Many Requests";
		case 431: return "Request Header Fields Too Large";
		case 500: return "Internal Server Error";
		case 501: return "Not Implemented";
		case 503: return "Service Unavailable";
		case 505: return "HTTP Version Not Supported";
		default: return status >= 200 && status < 600 ? "Status" : "Unknown";
	}
}

} // namespace spider_host
