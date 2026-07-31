#include "Http.h"

#include <Windows.h>

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <system_error>
#include <vector>

#include "Version.h"

namespace fs = std::filesystem;

namespace {
    constexpr DWORD RESOLVE_TIMEOUT_MS = 8'000;
    constexpr DWORD CONNECT_TIMEOUT_MS = 8'000;
    constexpr DWORD SEND_TIMEOUT_MS = 10'000;
    constexpr DWORD RECEIVE_TIMEOUT_MS = 20'000;
    constexpr DWORD READ_CHUNK = 16 * 1024;

    class Handle {
    public:
        Handle() = default;
        explicit Handle(const HINTERNET handle) : handle_(handle) {}
        ~Handle() { Close(); }

        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        Handle(Handle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

        Handle& operator=(Handle&& other) noexcept
        {
            if (this != &other) {
                Close();
                handle_ = other.handle_;
                other.handle_ = nullptr;
            }
            return *this;
        }

        [[nodiscard]] HINTERNET get() const { return handle_; }
        explicit operator bool() const { return handle_ != nullptr; }

    private:
        void Close()
        {
            if (handle_) {
                WinHttpCloseHandle(handle_);
                handle_ = nullptr;
            }
        }

        HINTERNET handle_ = nullptr;
    };

    std::wstring Widen(const std::string& value)
    {
        if (value.empty()) {
            return {};
        }
        const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (needed <= 0) {
            return {};
        }
        std::wstring out(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
        return out;
    }

    std::string Narrow(const std::wstring& value)
    {
        if (value.empty()) {
            return {};
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (needed <= 0) {
            return {};
        }
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                            out.data(), needed, nullptr, nullptr);
        return out;
    }

    std::string LastErrorMessage(const char* what)
    {
        return std::string(what) + " failed (win32 " + std::to_string(GetLastError()) + ")";
    }

    struct Request {
        Handle session;
        Handle connection;
        Handle request;
    };

    bool IsAllowedHost(const std::wstring& host)
    {
        static constexpr const wchar_t* kAllowed[] = {
            L"api.github.com",
            L"github.com",
            L"objects.githubusercontent.com",
            L"release-assets.githubusercontent.com",
            L"data.gwdash.com",
            L"gwdash.com",
        };
        for (const wchar_t* allowed : kAllowed) {
            if (_wcsicmp(host.c_str(), allowed) == 0) {
                return true;
            }
        }
        return false;
    }

    /** Reject CR/LF/controls so a malicious ETag cannot inject request headers. */
    bool IsSafeEtag(const std::string& etag)
    {
        if (etag.empty() || etag.size() > 200) {
            return false;
        }
        for (const unsigned char ch : etag) {
            if (ch < 0x20 || ch == 0x7F) {
                return false;
            }
        }
        return true;
    }

    bool OpenRequest(const std::wstring& url, const std::string& etag, Request& out, std::string& error)
    {
        std::array<wchar_t, 256> host{};
        std::array<wchar_t, 2048> path{};

        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.lpszHostName = host.data();
        components.dwHostNameLength = static_cast<DWORD>(host.size());
        components.lpszUrlPath = path.data();
        components.dwUrlPathLength = static_cast<DWORD>(path.size());

        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
            error = LastErrorMessage("WinHttpCrackUrl");
            return false;
        }

        if (components.nScheme != INTERNET_SCHEME_HTTPS) {
            error = "refusing non-HTTPS URL";
            return false;
        }

        const std::wstring host_name(host.data());
        if (!IsAllowedHost(host_name)) {
            error = "refusing download from unexpected host";
            return false;
        }

        static const std::wstring user_agent = Widen(GWDASH_USER_AGENT);
        out.session = Handle(WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!out.session) {
            error = LastErrorMessage("WinHttpOpen");
            return false;
        }

        WinHttpSetTimeouts(out.session.get(), RESOLVE_TIMEOUT_MS, CONNECT_TIMEOUT_MS,
                           SEND_TIMEOUT_MS, RECEIVE_TIMEOUT_MS);

        out.connection = Handle(WinHttpConnect(out.session.get(), host.data(), components.nPort, 0));
        if (!out.connection) {
            error = LastErrorMessage("WinHttpConnect");
            return false;
        }

        out.request = Handle(WinHttpOpenRequest(out.connection.get(), L"GET", path.data(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE));
        if (!out.request) {
            error = LastErrorMessage("WinHttpOpenRequest");
            return false;
        }

        std::wstring headers = L"Accept: application/json\r\n";
        if (IsSafeEtag(etag)) {
            headers += L"If-None-Match: " + Widen(etag) + L"\r\n";
        }
        WinHttpAddRequestHeaders(out.request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

        if (!WinHttpSendRequest(out.request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            error = LastErrorMessage("WinHttpSendRequest");
            return false;
        }

        if (!WinHttpReceiveResponse(out.request.get(), nullptr)) {
            error = LastErrorMessage("WinHttpReceiveResponse");
            return false;
        }

        return true;
    }

    long QueryStatus(const HINTERNET request)
    {
        DWORD status = 0;
        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
            return 0;
        }
        return static_cast<long>(status);
    }

    std::string QueryEtag(const HINTERNET request)
    {
        DWORD size = 0;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"ETag",
                            WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
        if (size == 0 || size > 1024) {
            return {};
        }

        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"ETag",
                                 value.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
            return {};
        }
        while (!value.empty() && value.back() == L'\0') {
            value.pop_back();
        }
        return Narrow(value);
    }
}

namespace gwdash::http {
    bool Get(const std::wstring& url,
             const std::string& etag,
             const std::size_t max_bytes,
             Response& out,
             std::string& error)
    {
        Request request;
        if (!OpenRequest(url, etag, request, error)) {
            return false;
        }

        out.status = QueryStatus(request.request.get());
        const std::string raw_etag = QueryEtag(request.request.get());
        out.etag = IsSafeEtag(raw_etag) ? raw_etag : std::string{};
        out.body.clear();

        std::vector<char> chunk(READ_CHUNK);
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.request.get(), &available)) {
                error = LastErrorMessage("WinHttpQueryDataAvailable");
                return false;
            }
            if (available == 0) {
                break;
            }

            const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(chunk.size()));
            DWORD read = 0;
            if (!WinHttpReadData(request.request.get(), chunk.data(), wanted, &read)) {
                error = LastErrorMessage("WinHttpReadData");
                return false;
            }
            if (read == 0) {
                break;
            }
            if (out.body.size() + read > max_bytes) {
                error = "response larger than expected";
                return false;
            }
            out.body.append(chunk.data(), read);
        }

        return true;
    }

    bool Download(const std::wstring& url,
                  const fs::path& destination,
                  const std::size_t max_bytes,
                  std::string& error)
    {
        Request request;
        if (!OpenRequest(url, {}, request, error)) {
            return false;
        }

        const long status = QueryStatus(request.request.get());
        if (status != 200) {
            error = "download returned HTTP " + std::to_string(status);
            return false;
        }

        std::error_code ec;
        fs::create_directories(destination.parent_path(), ec);

        FILE* file = nullptr;
        if (_wfopen_s(&file, destination.c_str(), L"wb") != 0 || !file) {
            error = "could not write " + destination.string();
            return false;
        }

        bool ok = true;
        std::size_t total = 0;
        std::vector<char> chunk(READ_CHUNK);
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.request.get(), &available)) {
                error = LastErrorMessage("WinHttpQueryDataAvailable");
                ok = false;
                break;
            }
            if (available == 0) {
                break;
            }

            const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(chunk.size()));
            DWORD read = 0;
            if (!WinHttpReadData(request.request.get(), chunk.data(), wanted, &read)) {
                error = LastErrorMessage("WinHttpReadData");
                ok = false;
                break;
            }
            if (read == 0) {
                break;
            }

            total += read;
            if (total > max_bytes) {
                error = "download larger than expected";
                ok = false;
                break;
            }
            if (fwrite(chunk.data(), 1, read, file) != read) {
                error = "write to " + destination.string() + " failed";
                ok = false;
                break;
            }
        }

        fclose(file);
        if (!ok) {
            fs::remove(destination, ec);
        }
        return ok;
    }

    std::string FileSha256(const fs::path& path)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
            return {};
        }

        DWORD object_length = 0;
        DWORD written = 0;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length),
                              sizeof(object_length), &written, 0) != 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return {};
        }

        std::vector<UCHAR> object(object_length);
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptCreateHash(algorithm, &hash, object.data(), object_length, nullptr, 0, 0) != 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return {};
        }

        std::string result;
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") == 0 && file) {
            std::vector<UCHAR> chunk(READ_CHUNK);
            bool ok = true;
            for (;;) {
                const size_t read = fread(chunk.data(), 1, chunk.size(), file);
                if (read == 0) {
                    break;
                }
                if (BCryptHashData(hash, chunk.data(), static_cast<ULONG>(read), 0) != 0) {
                    ok = false;
                    break;
                }
            }
            fclose(file);

            if (ok) {
                std::array<UCHAR, 32> digest{};
                if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0) {
                    static constexpr char HEX[] = "0123456789abcdef";
                    result.reserve(digest.size() * 2);
                    for (const UCHAR byte : digest) {
                        result.push_back(HEX[byte >> 4]);
                        result.push_back(HEX[byte & 0x0F]);
                    }
                }
            }
        }

        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return result;
    }
}
