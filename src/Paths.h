#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace gwdash {
    /**
     * Directory this DLL lives in, i.e. <Toolbox plugins>/GWDash. Derived from
     * the module handle rather than the folder Toolbox hands to LoadSettings, so
     * it is available before the first callback and independent of the settings
     * profile.
     */
    const std::filesystem::path& DataDirectory();

    /** Toolbox's plugins folder, one level above DataDirectory(). */
    std::filesystem::path PluginsDirectory();

    /** Where the updater stages a downloaded payload for the loader to swap in. */
    std::filesystem::path PendingDirectory();

    /** Last known good snapshot, so the overlay shows numbers immediately. */
    std::filesystem::path SnapshotCacheFile();

    /** User-saved WTB/WTS/WTT trade lines for one-click / command send. */
    std::filesystem::path PresetsFile();

    /** Cached ETag for GitHub releases/latest. */
    std::filesystem::path UpdaterEtagFile();

    /** Reads a whole file, returning an empty string when absent or too large. */
    std::string ReadFile(const std::filesystem::path& path, std::size_t max_bytes);

    /** Writes via a temporary file plus rename so readers never see a partial write. */
    bool WriteFileAtomic(const std::filesystem::path& path, const std::string& contents);
}
