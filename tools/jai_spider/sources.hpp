#pragma once
// The app's sources as the engines saw them. Every engine is built fresh, so the host, not an engine, remembers what
// the app was built from:
//
// - stamp_record: the files one build read (modules, includes, templates), each with the bytes the engine parsed and
//   the (mtime, size) stamp of exactly those bytes, handed over by the engine as it loads them
//   (engine::source_file_observer). A build is stale once any of them moves, and only a build that is not stale when
//   it ends is judged: kept as the last good one, or reported as a failure.
// - app_sources: shared by every worker. The last build that checked (its stamps and the bytes it parsed), and the
//   stamps of the files that failed the check since. While an edit does not check, engines are built from the last good
//   bytes, written once into a folder under the app's .cache folder and laid out there as they were around the app
//   (shadow_folder), so imports and templates resolve there as they did beside the app. The host never reads a source
//   file itself.
#include <jaiscript/jaiscript.hpp>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace spider_host {

namespace fs = std::filesystem;

// a file's (mtime, size) stamp from one system call, since a worker asks it of every source before every request;
// nullopt for a file that is not there (a path the engine probed) or a folder
inline std::optional<jai::engine::file_stamp> stamp_of(const std::string& a_path) {
#ifdef _WIN32
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExW(fs::path(reinterpret_cast<const char8_t*>(a_path.c_str())).c_str(), GetFileExInfoStandard, &data)
			|| (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
		return std::nullopt;
	}
	const uint64_t modified = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
	return jai::engine::file_stamp{modified, (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow};
#else
	struct stat data;
	if (::stat(a_path.c_str(), &data) != 0 || !S_ISREG(data.st_mode)) { return std::nullopt; }
#ifdef __APPLE__
	const uint64_t modified = static_cast<uint64_t>(data.st_mtimespec.tv_sec) * 1000000000ull + static_cast<uint64_t>(data.st_mtimespec.tv_nsec);
#else
	const uint64_t modified = static_cast<uint64_t>(data.st_mtim.tv_sec) * 1000000000ull + static_cast<uint64_t>(data.st_mtim.tv_nsec);
#endif
	return jai::engine::file_stamp{modified, static_cast<uint64_t>(data.st_size)};
#endif
}

using stamp_set = std::map<std::string, std::optional<jai::engine::file_stamp>>;

inline bool same_stamp(const std::optional<jai::engine::file_stamp>& a_left, const std::optional<jai::engine::file_stamp>& a_right) {
	if (!a_left || !a_right) { return !a_left && !a_right; }
	return a_left->mtime == a_right->mtime && a_left->size == a_right->size;
}

inline bool same_stamps(const stamp_set& a_left, const stamp_set& a_right) {
	if (a_left.size() != a_right.size()) { return false; }
	for (auto left = a_left.begin(), right = a_right.begin(); left != a_left.end(); ++left, ++right) {
		if (left->first != right->first || !same_stamp(left->second, right->second)) { return false; }
	}
	return true;
}

// every file still carries the stamp it was read under
inline bool stamps_hold(const stamp_set& a_stamps) {
	for (const auto& [path, stamp] : a_stamps) {
		if (!same_stamp(stamp_of(path), stamp)) { return false; }
	}
	return true;
}

// two names are one to the file system (Windows folds case)
inline bool same_name(const fs::path& a_left, const fs::path& a_right) {
#ifdef _WIN32
	const std::wstring& left = a_left.native();
	const std::wstring& right = a_right.native();
	return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
#else
	return a_left == a_right;
#endif
}

// a path without "." and ".." steps or a closing separator, so its parts compare with another's
inline fs::path plain_path(const fs::path& a_path) {
	fs::path result = a_path.lexically_normal();
	return result.has_filename() ? result : result.parent_path();
}

// the deepest folder two plain paths are both in or below; empty when they share no drive or share
inline fs::path shared_folder(const fs::path& a_left, const fs::path& a_right) {
	fs::path result;
	for (auto left = a_left.begin(), right = a_right.begin(); left != a_left.end() && right != a_right.end() && same_name(*left, *right); ++left, ++right) {
		result /= *left;
	}
	return result.has_root_directory() ? result : fs::path();
}

// a plain path's steps from a plain folder it is, or is below; nullopt for a path outside it
inline std::optional<fs::path> path_from(const fs::path& a_folder, const fs::path& a_path) {
	auto step = a_path.begin();
	for (auto part = a_folder.begin(); part != a_folder.end(); ++part, ++step) {
		if (step == a_path.end() || !same_name(*part, *step)) { return std::nullopt; }
	}
	fs::path result;
	for (; step != a_path.end(); ++step) { result /= *step; }
	return result;
}

// Where the copies of a snapshot's texts live. The host library's sit below lib/. Every other source sits below app/ at
// its path from `base`: the app's folder, or the folder above it that holds every source the app read. The copies keep
// their places relative to one another, so a relative import resolves among them, and no copy's path holds the app's
// whole path a second time (on Windows a path ends at 260 characters). A source on another drive or share has no copy:
// only its absolute path reaches it, and that names the file itself.
struct shadow_folder {
	fs::path folder;    // empty: the copies could not be written
	fs::path library;   // plain paths
	fs::path base;

	std::optional<fs::path> of(const fs::path& a_path) const {
		const fs::path path = plain_path(a_path);
		const char* below = "lib";
		std::optional<fs::path> steps = path_from(library, path);
		if (!steps) {
			below = "app";
			steps = path_from(base, path);
		}
		if (!steps) { return std::nullopt; }
		return steps->empty() ? folder / below : folder / below / *steps;
	}
};

// What one engine read while it was built, on that engine's thread: each file it loaded, with the bytes it parsed and
// the stamp of those bytes (engine::source_file_observer), and each path it asked a stamp of and did not load
// (engine::file_stamp_source: a file that is not there yet). A jaibite cache is the engine's own, not a source.
class stamp_record {
public:
	std::optional<jai::engine::file_stamp> stamp(const std::string& a_path) {
		std::optional<jai::engine::file_stamp> found = stamp_of(a_path);
		const fs::path path(a_path);
		if (path.extension() != ".jaibite" && path.parent_path().filename() != ".cache") { asked_.try_emplace(a_path, found); }
		return found;
	}
	// the engine loaded a_path: these bytes, under this stamp. A second load under another stamp is another version of
	// the file in the same engine (a save landed between the two): the first stays the one on record, so that build
	// is stale
	void loaded(const std::string& a_path, const std::optional<jai::engine::file_stamp>& a_stamp, std::string_view a_bytes) {
		if (loaded_.try_emplace(a_path, a_stamp).second) { texts_.emplace(a_path, std::string(a_bytes)); }
	}
	// every path the build depends on: a file it loaded under the stamp of the bytes it parsed, any other path under the
	// stamp it first answered
	stamp_set files() const {
		stamp_set result = loaded_;
		result.insert(asked_.begin(), asked_.end());
		return result;
	}
	// the bytes the engine parsed, by path
	const std::map<std::string, std::string>& texts() const { return texts_; }

private:
	stamp_set loaded_;
	stamp_set asked_;
	std::map<std::string, std::string> texts_;
};

// The last build that checked: what it read, by stamp and text
struct source_snapshot {
	stamp_set stamps;
	std::map<std::string, std::string> texts;
	uint64_t generation = 0;
};

// What a build's outcome means for the console: a line to print, or nothing
struct source_news {
	std::string reloaded;          // "views.jai, card.spider": a new version checked after an earlier one
	bool loads = false;            // the app checked after it never had
	std::string failure;           // a failure not reported before
	bool failure_after_good = false;
	bool current = false;          // the build is the last good one: what it declares (its tasks) stands
};

class app_sources {
public:
	app_sources() = default;
	app_sources(const app_sources&) = delete;
	app_sources& operator=(const app_sources&) = delete;
	~app_sources() {
		std::error_code ec;
		if (!shadow_root_.empty()) { fs::remove_all(shadow_root_, ec); }
	}

	// a build from the files on disk checked: it becomes the last good one when its stamps still hold and are new.
	// a_texts are the bytes that build parsed, as the engine handed them over. Whether the stamps hold is asked under
	// the lock that stores the verdict, so verdicts are stored in the order the files changed: a build of an older
	// version can never replace a newer one's, however late it ends. A stale build is not judged (news.current false)
	source_news note_good(const stamp_set& a_stamps, const std::map<std::string, std::string>& a_texts) {
		source_news news;
		{
			std::lock_guard<std::mutex> hold(lock_);
			if (!stamps_hold(a_stamps)) { return news; }
			if (good_ && same_stamps(good_->stamps, a_stamps)) {
				broken_.reset();
				news.current = true;
				return news;
			}
		}
		auto snapshot = std::make_shared<source_snapshot>();
		snapshot->stamps = a_stamps;
		snapshot->texts = a_texts;
		std::lock_guard<std::mutex> hold(lock_);
		if (!stamps_hold(a_stamps)) { return news; }
		broken_.reset();
		news.current = true;
		if (good_ && same_stamps(good_->stamps, a_stamps)) { return news; }
		if (good_) {
			for (const auto& [path, stamp] : a_stamps) {
				auto before = good_->stamps.find(path);
				if (stamp && (before == good_->stamps.end() || !same_stamp(before->second, stamp))) {
					news.reloaded += (news.reloaded.empty() ? "" : ", ") + fs::path(path).filename().string();
				}
			}
			if (news.reloaded.empty()) { news.reloaded = "the app"; }
		} else {
			news.loads = reported_stamps_.has_value();
		}
		snapshot->generation = ++generation_;
		good_ = std::move(snapshot);
		reported_stamps_.reset();
		return news;
	}

	// a build from the files on disk failed: until these stamps move, builds come from the last good texts. It is
	// reported once per edit, by its files' stamps: builds running at once can list it with or without a cached warning.
	source_news note_failure(const stamp_set& a_stamps, const std::string& a_failure) {
		std::lock_guard<std::mutex> hold(lock_);
		return failure_noted(a_stamps, a_failure);
	}

	// the same for a build from the files on disk, judged only while its stamps hold, asked under the lock as note_good
	// asks: a failure of a version the files have already left is not the edit's verdict
	source_news note_disk_failure(const stamp_set& a_stamps, const std::string& a_failure) {
		std::lock_guard<std::mutex> hold(lock_);
		if (!stamps_hold(a_stamps)) { return source_news(); }
		return failure_noted(a_stamps, a_failure);
	}

	// the stamps of the edit that failed, while they hold, and the last good snapshot: a build should start from it
	std::optional<std::pair<stamp_set, std::shared_ptr<const source_snapshot>>> serving_last_good() {
		std::optional<stamp_set> broken;
		std::shared_ptr<const source_snapshot> good;
		{
			std::lock_guard<std::mutex> hold(lock_);
			broken = broken_;
			good = good_;
		}
		if (!broken || !good || !stamps_hold(*broken)) { return std::nullopt; }
		return std::make_pair(std::move(*broken), std::move(good));
	}

	std::shared_ptr<const source_snapshot> last_good() {
		std::lock_guard<std::mutex> hold(lock_);
		return good_;
	}

	// where the last good texts go when a build needs them: a folder of this process's own below a_folder (the app's
	// .cache), so the copies stay with the app and leave nothing in a shared temporary folder; set before any build
	void keep_copies_in(const fs::path& a_folder) {
		std::lock_guard<std::mutex> hold(lock_);
		shadow_root_ = a_folder / ("jai_spider-last-good-" + std::to_string(process_id()));
	}

	// the folder holding the snapshot's texts, written the first time a build asks, and where each copy is in it
	// (shadow_folder), given the app's folder and the host library's
	shadow_folder shadow_of(const source_snapshot& a_snapshot, const fs::path& a_app, const fs::path& a_library, std::string& a_error) {
		std::lock_guard<std::mutex> hold(lock_);
		shadow_folder result;
		if (shadow_root_.empty()) {
			a_error = "no folder was given for the copies";
			return result;
		}
		result.library = plain_path(a_library);
		result.base = plain_path(a_app);
		for (const auto& [path, text] : a_snapshot.texts) {
			const fs::path source = plain_path(path);
			if (path_from(result.library, source)) { continue; }
			if (const fs::path shared = shared_folder(result.base, source.parent_path()); !shared.empty()) { result.base = shared; }
		}
		result.folder = shadow_root_ / std::to_string(a_snapshot.generation);
		if (written_generation_ == a_snapshot.generation) { return result; }
		std::error_code ec;   // an older generation's folder stays until exit: a build may still be reading it
		for (const auto& [path, text] : a_snapshot.texts) {
			const std::optional<fs::path> target = result.of(path);
			if (!target) { continue; }
			fs::create_directories(target->parent_path(), ec);
			std::ofstream file(*target, std::ios::binary | std::ios::trunc);
			file.write(text.data(), static_cast<std::streamsize>(text.size()));
			if (!file) {
				a_error = "cannot write " + target->string();
				result.folder.clear();
				return result;
			}
		}
		written_generation_ = a_snapshot.generation;
		return result;
	}

private:
	// with lock_ held
	source_news failure_noted(const stamp_set& a_stamps, const std::string& a_failure) {
		source_news news;
		broken_ = a_stamps;
		if (!reported_stamps_ || !same_stamps(*reported_stamps_, a_stamps)) {
			news.failure = a_failure;
			news.failure_after_good = good_ != nullptr;
			reported_stamps_ = a_stamps;
		}
		return news;
	}

	static long long process_id() {
#ifdef _WIN32
		return static_cast<long long>(_getpid());
#else
		return static_cast<long long>(getpid());
#endif
	}

	std::mutex lock_;
	std::shared_ptr<const source_snapshot> good_;
	std::optional<stamp_set> broken_;
	std::optional<stamp_set> reported_stamps_;   // the failing edit last reported
	uint64_t generation_ = 0;
	fs::path shadow_root_;
	uint64_t written_generation_ = 0;
};

} // namespace spider_host
