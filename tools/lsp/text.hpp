#pragma once

// Positions, lines and file names as the protocol spells them: 0-based lines, UTF-16 columns,
// file:// URIs. Internally a column is a 0-based byte offset into its line (the lexer's unit).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace jai_lsp {

struct text_pos {
	uint32_t line = 0;
	uint32_t col = 0;
	friend bool operator==(text_pos a, text_pos b) { return a.line == b.line && a.col == b.col; }
	friend bool operator!=(text_pos a, text_pos b) { return !(a == b); }
	friend bool operator<(text_pos a, text_pos b) { return a.line != b.line ? a.line < b.line : a.col < b.col; }
	friend bool operator<=(text_pos a, text_pos b) { return !(b < a); }
};

struct text_span {
	text_pos start;
	text_pos end;
	bool contains(text_pos p) const { return start <= p && p <= end; }
};

class line_index {
public:
	void build(std::string_view text) {
		starts_.assign(1, 0);
		for (size_t i = 0; i < text.size(); ++i) {
			if (text[i] == '\n') { starts_.push_back(i + 1); }
		}
		size_ = text.size();
	}
	size_t line_count() const { return starts_.size(); }
	size_t line_start(uint32_t line) const { return line < starts_.size() ? starts_[line] : size_; }
	size_t line_end(std::string_view text, uint32_t line) const {
		size_t end = line + 1 < starts_.size() ? starts_[line + 1] - 1 : size_;
		if (end > line_start(line) && end <= text.size() && end > 0 && text[end - 1] == '\r') { --end; }
		return end;
	}
	std::string_view line_text(std::string_view text, uint32_t line) const {
		size_t s = line_start(line);
		size_t e = line_end(text, line);
		return s <= e && e <= text.size() ? text.substr(s, e - s) : std::string_view{};
	}
	size_t offset(std::string_view text, text_pos p) const {
		size_t s = line_start(p.line);
		return std::min(s + p.col, std::max(s, line_end(text, p.line)));
	}
	text_pos position(size_t offset) const {
		auto it = std::upper_bound(starts_.begin(), starts_.end(), offset);
		uint32_t line = static_cast<uint32_t>((it - starts_.begin()) - 1);
		return {line, static_cast<uint32_t>(offset - starts_[line])};
	}

	// byte column <-> UTF-16 column within one line
	static uint32_t to_utf16(std::string_view line, uint32_t byte_col) {
		uint32_t units = 0;
		for (size_t i = 0; i < line.size() && i < byte_col;) {
			unsigned char c = static_cast<unsigned char>(line[i]);
			size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
			units += len == 4 ? 2 : 1;
			i += len;
		}
		if (byte_col > line.size()) { units += static_cast<uint32_t>(byte_col - line.size()); }
		return units;
	}
	static uint32_t from_utf16(std::string_view line, uint32_t utf16_col) {
		uint32_t units = 0;
		size_t i = 0;
		while (i < line.size() && units < utf16_col) {
			unsigned char c = static_cast<unsigned char>(line[i]);
			size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
			units += len == 4 ? 2 : 1;
			i += len;
		}
		return static_cast<uint32_t>(std::min(i, line.size()));
	}

private:
	std::vector<size_t> starts_{0};
	size_t size_ = 0;
};

inline int hex_value(char c) {
	if (c >= '0' && c <= '9') { return c - '0'; }
	if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
	if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
	return -1;
}

// file:///d%3A/x/y.jai -> d:/x/y.jai (then canonicalized by the caller)
inline std::string uri_to_path(std::string_view uri) {
	if (uri.rfind("file://", 0) != 0) { return std::string(uri); }
	std::string_view rest = uri.substr(7);
	std::string decoded;
	for (size_t i = 0; i < rest.size(); ++i) {
		if (rest[i] == '%' && i + 2 < rest.size() && hex_value(rest[i + 1]) >= 0 && hex_value(rest[i + 2]) >= 0) {
			decoded += static_cast<char>(hex_value(rest[i + 1]) * 16 + hex_value(rest[i + 2]));
			i += 2;
		} else {
			decoded += rest[i];
		}
	}
#ifdef _WIN32
	if (decoded.size() >= 3 && decoded[0] == '/' && decoded[2] == ':') { decoded.erase(0, 1); }
	std::replace(decoded.begin(), decoded.end(), '/', '\\');
#endif
	return decoded;
}

// VS Code's own spelling: lower-case drive, ':' escaped, forward slashes
inline std::string path_to_uri(std::string_view path) {
	std::string p(path);
#ifdef _WIN32
	std::replace(p.begin(), p.end(), '\\', '/');
	if (p.size() >= 2 && p[1] == ':') {
		p[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(p[0])));
		p = "/" + p;
	}
#endif
	std::string out = "file://";
	static const char* hex = "0123456789ABCDEF";
	for (unsigned char c : p) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		    c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 0xF];
		}
	}
	return out;
}

// One spelling per file: the canonical path when the file exists, else the absolute lexical one.
inline std::string canonical_path(const std::string& path) {
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path p(path);
	fs::path canon = fs::canonical(p, ec);
	if (ec) {
		ec.clear();
		canon = fs::weakly_canonical(p, ec);
		if (ec) { canon = fs::absolute(p, ec); }
	}
	return canon.string();
}

} // namespace jai_lsp
