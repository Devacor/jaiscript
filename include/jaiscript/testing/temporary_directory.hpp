#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace jai::foundry {

namespace fixture_detail {

class process_directory {
public:
    process_directory() : path_(reserve_directory()) {}
    process_directory(const process_directory&) = delete;
    process_directory& operator=(const process_directory&) = delete;

    ~process_directory() noexcept {
        // Only this process's atomically reserved directory is owned here. Never
        // remove the system temp root or a caller-supplied path. Cleanup is best effort.
        try {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        } catch (...) {}
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    static std::filesystem::path reserve_directory() {
        const auto parent = std::filesystem::canonical(std::filesystem::temp_directory_path());
#if defined(_WIN32)
        const auto process_id = _getpid();
#else
        const auto process_id = getpid();
#endif
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string stem = "jaiscript-tests-" + std::to_string(process_id) + "-" + std::to_string(stamp);
        for (unsigned attempt = 0; attempt < 256; ++attempt) {
            const auto result = parent / (stem + "-" + std::to_string(attempt));
            std::error_code ec;
            if (std::filesystem::create_directory(result, ec)) {
                return result;
            }
            if (ec) {
                throw std::filesystem::filesystem_error("Cannot create test fixture directory", result, ec);
            }
        }
        throw std::filesystem::filesystem_error("Cannot reserve unique test fixture directory", parent,
                                               std::make_error_code(std::errc::file_exists));
    }

    const std::filesystem::path path_;
};

} // namespace fixture_detail

// Immutable process-lifetime owner, shared across translation units. A fresh directory
// is reserved even when a later process reuses a PID or an earlier run left artifacts.
// Lazily initialized, so including this header has no effect on normal runner execution.
inline const std::filesystem::path& test_temp_directory() {
    static const fixture_detail::process_directory owner;
    return owner.path();
}

// The jaibite cache an engine with no cache directory keeps for source (engine::jaibite_cache_path), spelled out on its own
inline std::filesystem::path jaibite_cache_beside(const std::filesystem::path& source) {
    std::filesystem::path name = source.filename();
    name += ".jaibite";
    return source.parent_path() / ".cache" / name;
}

} // namespace jai::foundry
