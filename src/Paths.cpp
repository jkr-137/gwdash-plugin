#include "Paths.h"

#include <Windows.h>

#include <cstdio>
#include <system_error>
#include <vector>

#include <ToolboxPlugin.h>

namespace fs = std::filesystem;

namespace {
    fs::path ResolveDataDirectory()
    {
        std::vector<wchar_t> buffer(MAX_PATH);
        for (;;) {
            const DWORD length = GetModuleFileNameW(plugin_handle, buffer.data(),
                                                    static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                return fs::current_path();
            }
            if (length < buffer.size()) {
                return fs::path(buffer.data(), buffer.data() + length).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
    }
}

namespace gwdash {
    const fs::path& DataDirectory()
    {
        static const fs::path directory = ResolveDataDirectory();
        return directory;
    }

    fs::path PluginsDirectory()
    {
        return DataDirectory().parent_path();
    }

    fs::path PendingDirectory()
    {
        return DataDirectory() / L"pending";
    }

    fs::path SnapshotCacheFile()
    {
        return DataDirectory() / L"cache.json";
    }

    std::string ReadFile(const fs::path& path, const std::size_t max_bytes)
    {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec || size == 0 || size > max_bytes) {
            return {};
        }

        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) {
            return {};
        }

        std::string contents(static_cast<size_t>(size), '\0');
        const size_t read = fread(contents.data(), 1, contents.size(), file);
        fclose(file);
        contents.resize(read);
        return contents;
    }

    bool WriteFileAtomic(const fs::path& path, const std::string& contents)
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);

        fs::path temporary = path;
        temporary += L".tmp";

        FILE* file = nullptr;
        if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || !file) {
            return false;
        }
        const bool written = fwrite(contents.data(), 1, contents.size(), file) == contents.size();
        fclose(file);

        if (!written) {
            fs::remove(temporary, ec);
            return false;
        }

        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            fs::remove(temporary, ec);
            return false;
        }
        return true;
    }
}
