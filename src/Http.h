#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

// Minimal WinHTTP wrapper. Toolbox ships a RestClient library, but linking it
// would pull its Core dependency in; a GET plus a file download is all we need.
namespace gwdash::http {
    struct Response {
        long status = 0;
        std::string body;
        std::string etag;
    };

    /**
     * Blocking HTTPS GET to an allowlisted host. `etag` is sent as If-None-Match
     * when it is non-empty and free of CR/LF/control characters (else ignored).
     * Returns false only on transport errors; check `out.status` otherwise.
     * `accept` defaults to application/json; pass star-slash-star for binary assets.
     */
    bool Get(const std::wstring& url, const std::string& etag, std::size_t max_bytes, Response& out,
             std::string& error, const wchar_t* accept = L"application/json");

    /** Blocking HTTPS download to an allowlisted host; creates parent directories. */
    bool Download(const std::wstring& url, const std::filesystem::path& destination,
                  std::size_t max_bytes, std::string& error);

    /** Lowercase hex SHA-256 of a file, empty on failure. */
    std::string FileSha256(const std::filesystem::path& path);
} // namespace gwdash::http
