#pragma once
// The host file IO the runner and the jai_spider host register: read_file, write_file, file_exists, delete_file,
// move_file, dir_exists, make_dirs, writable_dir and list_dir, each requiring the engine's filesystem permission.
#include <jaiscript/jaiscript.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#endif

namespace jai_runner {

// read_file: the file's bytes, "" for an empty file; a missing or unreadable one raises the catchable
// "Cannot read file '<path>': <reason>" (docs/site/guide/10-modules.html "Where files are")
inline std::string read_file_or_raise(const std::string& path) {
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::file_status st = fs::status(path, ec);
	const char* reason = nullptr;
	if (st.type() == fs::file_type::not_found) { reason = "not found"; }
	else if (ec) { reason = ec == std::errc::permission_denied ? "permission denied" : "cannot be opened"; }
	else if (!fs::is_regular_file(st)) { reason = "not a file"; }
	if (!reason) {
		errno = 0;
		std::ifstream in(path, std::ios::binary);
		if (!in) {
			reason = errno == EACCES ? "permission denied" : "cannot be opened";
		} else {
			std::ostringstream ss;
			ss << in.rdbuf();
			if (!in.bad()) { return ss.str(); }
			reason = "read failed";
		}
	}
	throw std::runtime_error("Cannot read file '" + path + "': " + reason);
}

inline bool spill_file(const std::string& path, const std::string& content) {
	namespace fs = std::filesystem;
	if (const fs::path parent = fs::path(path).parent_path(); !parent.empty()) {
		std::error_code ec;
		fs::create_directories(parent, ec);   // a missing folder is the ofstream's failure to report
	}
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) { return false; }
	out.write(content.data(), static_cast<std::streamsize>(content.size()));
	return out.good();
}

inline void register_file_io(jai::engine& eng) {
	namespace fs = std::filesystem;
	eng.add_function("read_file", [](const std::string& path) -> std::string { return read_file_or_raise(path); }, jai::permissions::filesystem);
	eng.add_function("write_file", [](const std::string& path, const std::string& content) -> bool {
		return spill_file(path, content);
	}, jai::permissions::filesystem);
	eng.add_function("file_exists", [](const std::string& path) -> bool { return fs::exists(path); }, jai::permissions::filesystem);
	eng.add_function("delete_file", [](const std::string& path) -> bool {
		std::error_code ec;
		return fs::remove(path, ec);
	}, jai::permissions::filesystem);
	// the safe-save step: the destination is replaced in one operation on the same volume (MoveFileEx / rename(2)), a
	// reader sees the old file or the new one; across volumes the move copies, then removes the source
	eng.add_function("move_file", [](const std::string& from, const std::string& to) -> bool {
		std::error_code ec;
		if (!fs::is_regular_file(from, ec)) { return false; }
#ifdef _WIN32
		return MoveFileExW(fs::path(from).c_str(), fs::path(to).c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED) != 0;
#else
		fs::rename(from, to, ec);
		if (!ec) { return true; }
		if (ec != std::errc::cross_device_link) { return false; }
		ec.clear();
		fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
		return !ec && fs::remove(from, ec);
#endif
	}, jai::permissions::filesystem);
	eng.add_function("dir_exists", [](const std::string& path) -> bool {
		std::error_code ec;
		return fs::is_directory(path, ec);
	}, jai::permissions::filesystem);
	eng.add_function("make_dirs", [](const std::string& path) -> bool {
		std::error_code ec;
		fs::create_directories(path, ec);
		return fs::is_directory(path, ec);
	}, jai::permissions::filesystem);
	// The save-side question resolve_path cannot answer: can this folder take a file? Creates it
	// if missing, writes and removes a probe. A program picks its save anchor with it:
	// root_script_dir() when writable, app_data_dir(name) otherwise.
	eng.add_function("writable_dir", [](const std::string& path) -> bool {
		std::error_code ec;
		fs::create_directories(path, ec);
		if (!fs::is_directory(path, ec)) { return false; }
		fs::path probe = fs::path(path) / (".jai_write_probe_" + std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
		{
			std::ofstream out(probe, std::ios::binary | std::ios::trunc);
			if (!out) { return false; }
		}
		fs::remove(probe, ec);
		return true;
	}, jai::permissions::filesystem);
	eng.add_function("list_dir", [](const std::string& path) -> std::vector<std::string> {
		std::vector<std::string> names;
		std::error_code ec;
		for (const auto& entry : fs::directory_iterator(path, ec)) { names.push_back(entry.path().filename().generic_string()); }
		std::sort(names.begin(), names.end());
		return names;
	}, jai::permissions::filesystem);
}

} // namespace jai_runner
