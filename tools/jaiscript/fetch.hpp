#pragma once
// Runner-side fetch primitives: an asynchronous HTTPS download the script polls from its
// own loop and zip extraction with CRC verification. The runner links
// nothing but the library, so instead of asio+OpenSSL it uses
// the platform's own HTTPS stack: WinHTTP on Windows (system library, OS certificate
// store), libcurl on POSIX when the build found it, else the curl executable every Linux and
// macOS install carries. Registered by register_fetch() next to the console and net bindings.
//
// Script surface (docs/site/reference/index.html#runner-host-api lists the runner's host API):
//   auto d = Download();  d.start(url, path);  d.pump();  d.done()  d.success()  d.error()
//   d.received()  d.total()  d.fraction()  d.cancel()
//   zip_entries(zip) -> array<string>;  zip_extract(zip, entry, dest) -> bool (CRC checked)
#include <jaiscript/jaiscript.hpp>

#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>   // before windows.h, as the runner's own network code requires
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#elif defined(JAISCRIPT_RUNNER_HAS_CURL)
#include <curl/curl.h>
#endif

namespace jai_runner {

namespace fs = std::filesystem;

// -------------------------------------------------------------------- CRC32 ---
inline constexpr auto crc32_table = []() {
	std::array<uint32_t, 256> result {};
	for (uint32_t i = 0; i < 256; ++i) {
		uint32_t c = i;
		for (int k = 0; k < 8; ++k) { c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1); }
		result[i] = c;
	}
	return result;
}();

inline uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
	crc = ~crc;
	for (size_t i = 0; i < n; ++i) { crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8); }
	return ~crc;
}

// ------------------------------------------------------------------ inflate ---
// RFC 1951, the three block types, canonical Huffman tables built per block. Output size
// is known from the zip directory, so the destination is reserved up front.
struct inflater {
	const uint8_t* in;
	size_t in_len;
	size_t pos = 0;
	uint32_t bitbuf = 0;
	int bitcnt = 0;
	std::string& out;
	std::string error;

	inflater(const uint8_t* data, size_t len, std::string& dest) : in(data), in_len(len), out(dest) {}

	int bit() {
		if (bitcnt == 0) {
			if (pos >= in_len) { error = "inflate: input exhausted"; return 0; }
			bitbuf = in[pos++];
			bitcnt = 8;
		}
		const int b = bitbuf & 1;
		bitbuf >>= 1;
		--bitcnt;
		return b;
	}
	uint32_t bits(int n) {
		uint32_t v = 0;
		for (int i = 0; i < n; ++i) { v |= uint32_t(bit()) << i; }
		return v;
	}

	struct huffman {
		uint16_t count[16] = {};
		uint16_t symbol[320] = {};
	};
	static bool build(huffman& h, const uint8_t* lengths, int n) {
		std::memset(h.count, 0, sizeof(h.count));
		for (int i = 0; i < n; ++i) { ++h.count[lengths[i]]; }
		h.count[0] = 0;
		uint16_t offs[16];
		offs[1] = 0;
		for (int i = 1; i < 15; ++i) { offs[i + 1] = offs[i] + h.count[i]; }
		for (int i = 0; i < n; ++i) {
			if (lengths[i]) { h.symbol[offs[lengths[i]]++] = uint16_t(i); }
		}
		return true;
	}
	int decode(const huffman& h) {
		int code = 0, first = 0, index = 0;
		for (int len = 1; len < 16; ++len) {
			code |= bit();
			const int count = h.count[len];
			if (code - count < first) { return h.symbol[index + (code - first)]; }
			index += count;
			first += count;
			first <<= 1;
			code <<= 1;
			if (!error.empty()) { return -1; }
		}
		error = "inflate: bad Huffman code";
		return -1;
	}
	bool codes(const huffman& lit, const huffman& dist) {
		static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
		static const uint16_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
		static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
		static const uint16_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
		for (;;) {
			int sym = decode(lit);
			if (sym < 0) { return false; }
			if (sym < 256) { out.push_back(char(sym)); continue; }
			if (sym == 256) { return true; }
			sym -= 257;
			if (sym >= 29) { error = "inflate: bad length symbol"; return false; }
			const size_t len = lbase[sym] + bits(lext[sym]);
			const int ds = decode(dist);
			if (ds < 0 || ds >= 30) { error = "inflate: bad distance symbol"; return false; }
			const size_t distv = dbase[ds] + bits(dext[ds]);
			if (distv > out.size()) { error = "inflate: distance beyond output"; return false; }
			const size_t from = out.size() - distv;
			for (size_t i = 0; i < len; ++i) { out.push_back(out[from + i]); }   // overlapping copies by design
			if (!error.empty()) { return false; }
		}
	}
	bool stored() {
		bitbuf = 0; bitcnt = 0;
		if (pos + 4 > in_len) { error = "inflate: truncated stored block"; return false; }
		const uint32_t len = in[pos] | (uint32_t(in[pos + 1]) << 8);
		const uint32_t nlen = in[pos + 2] | (uint32_t(in[pos + 3]) << 8);
		pos += 4;
		if ((len ^ 0xFFFF) != nlen || pos + len > in_len) { error = "inflate: stored block length mismatch"; return false; }
		out.append(reinterpret_cast<const char*>(in + pos), len);
		pos += len;
		return true;
	}
	bool fixed() {
		huffman lit, dist;
		uint8_t lengths[320];
		int i = 0;
		for (; i < 144; ++i) lengths[i] = 8;
		for (; i < 256; ++i) lengths[i] = 9;
		for (; i < 280; ++i) lengths[i] = 7;
		for (; i < 288; ++i) lengths[i] = 8;
		build(lit, lengths, 288);
		for (i = 0; i < 30; ++i) lengths[i] = 5;
		build(dist, lengths, 30);
		return codes(lit, dist);
	}
	bool dynamic() {
		static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
		const int nlen = int(bits(5)) + 257;
		const int ndist = int(bits(5)) + 1;
		const int ncode = int(bits(4)) + 4;
		if (nlen > 286 || ndist > 30) { error = "inflate: bad dynamic header"; return false; }
		uint8_t lengths[320] = {};
		for (int i = 0; i < ncode; ++i) { lengths[order[i]] = uint8_t(bits(3)); }
		huffman lencode;
		build(lencode, lengths, 19);
		int idx = 0;
		while (idx < nlen + ndist) {
			int sym = decode(lencode);
			if (sym < 0) { return false; }
			if (sym < 16) { lengths[idx++] = uint8_t(sym); continue; }
			int len = 0, rep = 0;
			if (sym == 16) {
				if (idx == 0) { error = "inflate: repeat with no previous length"; return false; }
				len = lengths[idx - 1]; rep = 3 + int(bits(2));
			} else if (sym == 17) { rep = 3 + int(bits(3)); }
			else { rep = 11 + int(bits(7)); }
			if (idx + rep > nlen + ndist) { error = "inflate: too many lengths"; return false; }
			while (rep--) { lengths[idx++] = uint8_t(len); }
		}
		huffman lit, dist;
		build(lit, lengths, nlen);
		build(dist, lengths + nlen, ndist);
		return codes(lit, dist);
	}
	bool run() {
		int last = 0;
		do {
			last = bit();
			const uint32_t type = bits(2);
			bool ok = false;
			if (type == 0) ok = stored();
			else if (type == 1) ok = fixed();
			else if (type == 2) ok = dynamic();
			else { error = "inflate: reserved block type"; }
			if (!ok || !error.empty()) { return false; }
		} while (!last);
		return true;
	}
};

// ---------------------------------------------------------------------- zip ---
struct zip_entry {
	std::string name;
	uint16_t method = 0;
	uint32_t crc = 0;
	uint32_t csize = 0;
	uint32_t usize = 0;
	uint32_t local_offset = 0;
};

inline uint16_t rd16(const std::string& s, size_t o) { return uint16_t(uint8_t(s[o])) | (uint16_t(uint8_t(s[o + 1])) << 8); }
inline uint32_t rd32(const std::string& s, size_t o) { return uint32_t(rd16(s, o)) | (uint32_t(rd16(s, o + 2)) << 16); }

inline bool zip_directory(const std::string& bytes, std::vector<zip_entry>& entries, std::string& error) {
	if (bytes.size() < 22) { error = "zip: file too small"; return false; }
	size_t eocd = std::string::npos;
	const size_t stop = bytes.size() >= 22 + 65535 ? bytes.size() - 22 - 65535 : 0;
	for (size_t i = bytes.size() - 22; ; --i) {
		if (rd32(bytes, i) == 0x06054b50u) { eocd = i; break; }
		if (i == stop) { break; }
	}
	if (eocd == std::string::npos) { error = "zip: no central directory"; return false; }
	const uint16_t count = rd16(bytes, eocd + 10);
	const uint32_t cd_size = rd32(bytes, eocd + 12);
	size_t p = rd32(bytes, eocd + 16);
	if (p + cd_size > bytes.size()) { error = "zip: central directory out of range"; return false; }
	for (uint16_t i = 0; i < count; ++i) {
		if (p + 46 > bytes.size() || rd32(bytes, p) != 0x02014b50u) { error = "zip: bad central directory entry"; return false; }
		zip_entry e;
		e.method = rd16(bytes, p + 10);
		e.crc = rd32(bytes, p + 16);
		e.csize = rd32(bytes, p + 20);
		e.usize = rd32(bytes, p + 24);
		const uint16_t nlen = rd16(bytes, p + 28), elen = rd16(bytes, p + 30), clen = rd16(bytes, p + 32);
		e.local_offset = rd32(bytes, p + 42);
		if (p + 46 + nlen > bytes.size()) { error = "zip: bad entry name"; return false; }
		e.name = bytes.substr(p + 46, nlen);
		entries.push_back(std::move(e));
		p += 46 + size_t(nlen) + elen + clen;
	}
	return true;
}

inline std::string slurp_binary(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) { return ""; }
	std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return s;
}

inline std::vector<std::string> zip_entries(const std::string& zip_path) {
	std::vector<std::string> names;
	const std::string bytes = slurp_binary(zip_path);
	std::vector<zip_entry> entries;
	std::string error;
	if (!bytes.empty() && zip_directory(bytes, entries, error)) {
		for (const auto& e : entries) { names.push_back(e.name); }
	}
	return names;
}

// Extracts one entry (stored or deflated), verifies its CRC-32, writes dest (parents created).
inline bool zip_extract(const std::string& zip_path, const std::string& entry_name, const std::string& dest, std::string* why = nullptr) {
	auto fail = [&](const std::string& m) { if (why) { *why = m; } return false; };
	const std::string bytes = slurp_binary(zip_path);
	if (bytes.empty()) { return fail("zip: cannot read " + zip_path); }
	std::vector<zip_entry> entries;
	std::string error;
	if (!zip_directory(bytes, entries, error)) { return fail(error); }
	const zip_entry* e = nullptr;
	for (const auto& cand : entries) { if (cand.name == entry_name) { e = &cand; break; } }
	if (!e) { return fail("zip: no entry named " + entry_name); }
	const size_t lh = e->local_offset;
	if (lh + 30 > bytes.size() || rd32(bytes, lh) != 0x04034b50u) { return fail("zip: bad local header"); }
	const size_t data = lh + 30 + rd16(bytes, lh + 26) + rd16(bytes, lh + 28);
	if (data + e->csize > bytes.size()) { return fail("zip: entry data out of range"); }
	std::string out;
	if (e->method == 0) {
		out.assign(bytes, data, e->csize);
	} else if (e->method == 8) {
		out.reserve(e->usize);
		inflater inf(reinterpret_cast<const uint8_t*>(bytes.data()) + data, e->csize, out);
		if (!inf.run()) { return fail(inf.error.empty() ? "inflate failed" : inf.error); }
	} else {
		return fail("zip: unsupported compression method " + std::to_string(e->method));
	}
	if (out.size() != e->usize) { return fail("zip: size mismatch after extraction"); }
	if (crc32_update(0, reinterpret_cast<const uint8_t*>(out.data()), out.size()) != e->crc) { return fail("zip: CRC mismatch"); }
	std::error_code ec;
	if (const fs::path parent = fs::path(dest).parent_path(); !parent.empty()) { fs::create_directories(parent, ec); }
	std::ofstream f(dest, std::ios::binary | std::ios::trunc);
	if (!f) { return fail("zip: cannot write " + dest); }
	f.write(out.data(), static_cast<std::streamsize>(out.size()));
	return f.good();
}

// ------------------------------------------------------------------ download ---
// One request, one background thread, polled from the script's loop. Progress and
// completion are atomics; the error text is written before `done` publishes it.
class download {
public:
	download() = default;
	~download() { cancel(); join(); }

	bool start(const std::string& url, const std::string& path) {
		if (started_) { return false; }
		if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
			error_ = "download: only http and https URLs are supported";
			done_.store(true);
			return false;
		}
		url_ = url; path_ = path; started_ = true;
		std::error_code ec;
		if (const fs::path parent = fs::path(path).parent_path(); !parent.empty()) { fs::create_directories(parent, ec); }
		worker_ = std::thread([this] { run(); });
		return true;
	}
	bool pump() { return done_.load(); }
	bool done() const { return done_.load(); }
	bool success() const { return done_.load() && ok_.load(); }
	std::string error() const { return done_.load() ? error_ : std::string(); }
	jai::script_int received() const {
#if !defined(_WIN32) && !defined(JAISCRIPT_RUNNER_HAS_CURL)
		if (started_ && !done_.load()) {   // the curl process owns the file: its size is the progress
			std::error_code ec;
			const auto size = fs::file_size(path_, ec);
			if (!ec) { return static_cast<jai::script_int>(size); }
		}
#endif
		return static_cast<jai::script_int>(received_.load());
	}
	jai::script_int total() const { return static_cast<jai::script_int>(total_.load()); }
	double fraction() const {
		const int64_t t = total_.load();
		if (t <= 0) { return done_.load() && ok_.load() ? 1.0 : 0.0; }
		const double f = double(received_.load()) / double(t);
		return f > 1.0 ? 1.0 : f;
	}
	void cancel() { cancel_.store(true); }

private:
	void join() { if (worker_.joinable()) { worker_.join(); } }
	void finish(bool ok, std::string err) {
		if (!ok) {
			std::error_code ec;
			fs::remove(path_, ec);   // never leave a partial file behind
		}
		error_ = std::move(err);
		ok_.store(ok);
		done_.store(true, std::memory_order_release);
	}
	bool write_chunk(std::ofstream& out, const char* data, size_t n) {
		out.write(data, static_cast<std::streamsize>(n));
		received_.fetch_add(static_cast<int64_t>(n));
		return out.good();
	}

#ifdef _WIN32
	static std::wstring widen(const std::string& s) {
		if (s.empty()) { return L""; }
		const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
		std::wstring w(size_t(n), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
		return w;
	}
	void run() {
		const std::wstring wurl = widen(url_);
		URL_COMPONENTS parts{};
		parts.dwStructSize = sizeof(parts);
		wchar_t host[256] = {}, urlpath[4096] = {};
		parts.lpszHostName = host; parts.dwHostNameLength = 256;
		parts.lpszUrlPath = urlpath; parts.dwUrlPathLength = 4096;
		if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) { return finish(false, "download: malformed URL"); }
		HINTERNET session = WinHttpOpen(L"jaiscript/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session) { return finish(false, "download: WinHttpOpen failed"); }
		HINTERNET conn = WinHttpConnect(session, host, parts.nPort, 0);
		if (!conn) { WinHttpCloseHandle(session); return finish(false, "download: cannot connect to " + url_); }
		const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
		HINTERNET req = WinHttpOpenRequest(conn, L"GET", urlpath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
		bool ok = false;
		std::string err;
		if (req) {
			DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
			WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
			if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
				DWORD status = 0, size = sizeof(status);
				WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
				if (status != 200) {
					err = "download: HTTP " + std::to_string(status) + " for " + url_;
				} else {
					wchar_t lenbuf[32] = {};
					DWORD lensize = sizeof(lenbuf);
					if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenbuf, &lensize, WINHTTP_NO_HEADER_INDEX)) {
						total_.store(_wtoi64(lenbuf));
					}
					std::ofstream out(path_, std::ios::binary | std::ios::trunc);
					if (!out) { err = "download: cannot write " + path_; }
					else {
						std::vector<char> buf(1 << 16);
						ok = true;
						for (;;) {
							if (cancel_.load()) { ok = false; err = "download: cancelled"; break; }
							DWORD avail = 0;
							if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; err = "download: read failed"; break; }
							if (avail == 0) { break; }
							while (avail > 0) {
								const DWORD want = (std::min<DWORD>)(avail, DWORD(buf.size()));
								DWORD got = 0;
								if (!WinHttpReadData(req, buf.data(), want, &got)) { ok = false; err = "download: read failed"; break; }
								if (got == 0) { break; }
								if (!write_chunk(out, buf.data(), got)) { ok = false; err = "download: write failed"; break; }
								avail -= got;
							}
							if (!ok) { break; }
						}
						out.close();
						if (ok && total_.load() > 0 && received_.load() != total_.load()) { ok = false; err = "download: incomplete transfer"; }
					}
				}
			} else {
				err = "download: request failed (WinHTTP error " + std::to_string(GetLastError()) + ") for " + url_;
			}
			WinHttpCloseHandle(req);
		} else {
			err = "download: WinHttpOpenRequest failed";
		}
		WinHttpCloseHandle(conn);
		WinHttpCloseHandle(session);
		finish(ok, err);
	}
#elif defined(JAISCRIPT_RUNNER_HAS_CURL)
	static size_t curl_write(char* data, size_t size, size_t n, void* self_ptr) {
		auto* self = static_cast<download*>(self_ptr);
		if (self->cancel_.load()) { return 0; }
		return self->write_chunk(*self->out_, data, size * n) ? size * n : 0;
	}
	static int curl_progress(void* self_ptr, curl_off_t dltotal, curl_off_t, curl_off_t, curl_off_t) {
		auto* self = static_cast<download*>(self_ptr);
		if (dltotal > 0) { self->total_.store(static_cast<int64_t>(dltotal)); }
		return self->cancel_.load() ? 1 : 0;
	}
	std::ofstream* out_ = nullptr;
	void run() {
		std::ofstream out(path_, std::ios::binary | std::ios::trunc);
		if (!out) { return finish(false, "download: cannot write " + path_); }
		out_ = &out;
		CURL* c = curl_easy_init();
		if (!c) { return finish(false, "download: curl init failed"); }
		curl_easy_setopt(c, CURLOPT_URL, url_.c_str());
		curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
		curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
		curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
		curl_easy_setopt(c, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
		curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
		curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
		curl_easy_setopt(c, CURLOPT_USERAGENT, "jaiscript/1.0");
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &download::curl_write);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, this);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, &download::curl_progress);
		curl_easy_setopt(c, CURLOPT_XFERINFODATA, this);
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		const CURLcode rc = curl_easy_perform(c);
		std::string err;
		if (rc != CURLE_OK) { err = std::string("download: ") + curl_easy_strerror(rc) + " for " + url_; }
		curl_easy_cleanup(c);
		out.close();
		out_ = nullptr;
		finish(rc == CURLE_OK, err);
	}
#else
	// No HTTPS stack in the build: the curl executable does the transfer, the file's growth
	// is the progress, and a HEAD request beforehand supplies the total when the server says.
	static std::string shell_quote(const std::string& s) {
		std::string q = "'";
		for (char ch : s) { if (ch == '\'') { q += "'\\''"; } else { q += ch; } }
		return q + "'";
	}
	void run() {
		{
			const std::string head = "curl -sIL --proto =http,https --proto-redir =http,https --max-time 30 " + shell_quote(url_) + " 2>/dev/null";
			if (FILE* p = popen(head.c_str(), "r")) {
				char line[512];
				int64_t last = -1;
				while (fgets(line, sizeof(line), p)) {
					std::string l(line);
					for (auto& ch : l) { ch = char(tolower(static_cast<unsigned char>(ch))); }
					if (l.rfind("content-length:", 0) == 0) { last = std::atoll(l.c_str() + 15); }
				}
				pclose(p);
				if (last > 0) { total_.store(last); }
			}
		}
		const std::string cmd = "curl -fsSL --proto =http,https --proto-redir =http,https --retry 3 -o " + shell_quote(path_) + " " + shell_quote(url_) + " 2>&1";
		FILE* p = popen(cmd.c_str(), "r");
		if (!p) { return finish(false, "download: curl is required on this platform and could not be started"); }
		std::string output;
		char buf[256];
		while (fgets(buf, sizeof(buf), p)) { output += buf; }   // curl -s prints only on failure; received() samples the file meanwhile
		std::error_code ec;
		const auto size = fs::file_size(path_, ec);
		if (!ec) { received_.store(static_cast<int64_t>(size)); }
		const int rc = pclose(p);
		const bool ok = rc == 0 && !cancel_.load();
		finish(ok, ok ? "" : "download: curl failed for " + url_ + (output.empty() ? "" : ": " + output));
	}
#endif

	std::string url_, path_, error_;
	bool started_ = false;
	std::thread worker_;
	std::atomic<bool> done_{ false }, ok_{ false }, cancel_{ false };
	std::atomic<int64_t> received_{ 0 }, total_{ -1 };
};

// A download reaches the network and writes a file: start needs both privileges
inline void register_fetch(jai::engine& eng) {
	eng.register_privilege("network");
	jai::dynamic_binder<download>(eng, "Download")
		.constructor<>()
		.method("start", &download::start, {"filesystem", "network"})
		.method("pump", &download::pump)
		.method("done", &download::done)
		.method("success", &download::success)
		.method("error", &download::error)
#if !defined(_WIN32) && !defined(JAISCRIPT_RUNNER_HAS_CURL)
		// The curl-executable fallback polls file_size; WinHTTP/libcurl read counters.
		.method("received", &download::received, jai::filesystem)
#else
		.method("received", &download::received)
#endif
		.method("total", &download::total)
		.method("fraction", &download::fraction)
		.method("cancel", &download::cancel)
		.build();
	eng.add_function("zip_entries", [](const std::string& zip) -> std::vector<std::string> { return zip_entries(zip); }, jai::filesystem);
	eng.add_function("zip_extract", [](const std::string& zip, const std::string& entry, const std::string& dest) -> bool {
		return zip_extract(zip, entry, dest);
	}, jai::filesystem);
}

} // namespace jai_runner
