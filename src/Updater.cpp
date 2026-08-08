#include "Updater.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <system_error>

#include <glaze/glaze.hpp>

#include "Http.h"
#include "Manifest.h"
#include "Paths.h"
#include "TimeUtil.h"
#include "Version.h"

namespace fs = std::filesystem;

namespace gwdash::github {
    struct Asset {
        std::string name;
        std::string browser_download_url;
    };

    struct Release {
        std::string tag_name;
        bool draft = false;
        bool prerelease = false;
        std::vector<Asset> assets;
    };
}

template <>
struct glz::meta<gwdash::github::Asset> {
    using T = gwdash::github::Asset;
    static constexpr auto value = object("name", &T::name, "browser_download_url", &T::browser_download_url);
};

template <>
struct glz::meta<gwdash::github::Release> {
    using T = gwdash::github::Release;
    static constexpr auto value = object(
        "tag_name", &T::tag_name,
        "draft", &T::draft,
        "prerelease", &T::prerelease,
        "assets", &T::assets);
};

namespace {
    constexpr wchar_t CORE_ASSET[] = L"GWDash.core.dll";
    constexpr char CORE_ASSET_NARROW[] = "GWDash.core.dll";
    constexpr char LOADER_ASSET[] = "GWDash.dll";
    constexpr char CHECKSUM_ASSET[] = "SHA256SUMS";
    constexpr char MANIFEST_ASSET[] = "manifest.json";
    constexpr char MANIFEST_SIG_ASSET[] = "manifest.sig";

    constexpr std::size_t MAX_RELEASE_BYTES = 512 * 1024;
    constexpr std::size_t MAX_CHECKSUM_BYTES = 16 * 1024;
    constexpr std::size_t MAX_MANIFEST_BYTES = 16 * 1024;
    constexpr std::size_t MAX_PAYLOAD_BYTES = 24 * 1024 * 1024;

    constexpr int CHECK_INTERVAL_SECONDS = 6 * 60 * 60;
    /** Small head start so the first price fetch gets the network to itself. */
    constexpr int FIRST_CHECK_DELAY_SECONDS = 20;

    int64_t NowMs()
    {
        return gwdash::NowMs();
    }

    std::wstring Widen(const std::string& value)
    {
        if (value.empty()) {
            return {};
        }
        const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                               nullptr, 0);
        if (needed <= 0) {
            return {};
        }
        std::wstring out(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
        return out;
    }

    std::string Trim(std::string value)
    {
        const auto is_space = [](const unsigned char c) { return std::isspace(c) != 0; };
        while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        const auto begin = std::ranges::find_if_not(value, [&](const char c) {
            return is_space(static_cast<unsigned char>(c));
        });
        value.erase(value.begin(), begin);
        return value;
    }

    std::string StripVersionPrefix(std::string value)
    {
        value = Trim(std::move(value));
        if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) {
            value.erase(value.begin());
        }
        return value;
    }

    std::array<int, 3> ParseVersion(const std::string& value)
    {
        std::array<int, 3> parts{0, 0, 0};
        std::istringstream stream(StripVersionPrefix(value));
        std::string token;
        for (size_t i = 0; i < parts.size() && std::getline(stream, token, '.'); i += 1) {
            parts[i] = std::atoi(token.c_str());
        }
        return parts;
    }

    /** True when `candidate` is strictly newer than `current`. */
    bool IsNewer(const std::string& candidate, const std::string& current)
    {
        return ParseVersion(candidate) > ParseVersion(current);
    }

    std::string FindChecksum(const std::string& sums, const std::string& filename)
    {
        std::istringstream stream(sums);
        std::string line;
        while (std::getline(stream, line)) {
            std::istringstream fields(line);
            std::string hash;
            std::string name;
            if (!(fields >> hash >> name)) {
                continue;
            }
            if (!name.empty() && name.front() == '*') {
                name.erase(name.begin());
            }
            if (name == filename) {
                std::ranges::transform(hash, hash.begin(), [](const unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return hash;
            }
        }
        return {};
    }

    const gwdash::github::Asset* FindAsset(const gwdash::github::Release& release, const std::string& name)
    {
        for (const auto& asset : release.assets) {
            if (asset.name == name) {
                return &asset;
            }
        }
        return nullptr;
    }
}

namespace gwdash {
    Updater::~Updater()
    {
        SignalStop();
        Join();
    }

    void Updater::Start()
    {
        if (running_.exchange(true)) {
            return;
        }
        stop_ = false;
        finished_ = false;

        {
            std::lock_guard lock(state_mutex_);
            state_.installed_version = GWDASH_VERSION;
        }

        thread_ = std::thread(&Updater::Run, this);
    }

    void Updater::SignalStop()
    {
        {
            std::lock_guard lock(wait_mutex_);
            stop_ = true;
        }
        wait_cv_.notify_all();
    }

    void Updater::Join()
    {
        if (thread_.joinable()) {
            thread_.join();
        }
        running_ = false;
    }

    void Updater::RequestCheck()
    {
        {
            std::lock_guard lock(wait_mutex_);
            check_now_ = true;
        }
        wait_cv_.notify_all();
    }

    void Updater::SetAutoInstall(const bool enabled)
    {
        auto_install_ = enabled;
    }

    UpdateState Updater::State() const
    {
        std::lock_guard lock(state_mutex_);
        return state_;
    }

    std::vector<std::string> Updater::TakeNotifications()
    {
        std::lock_guard lock(notification_mutex_);
        std::vector<std::string> out;
        out.swap(notifications_);
        return out;
    }

    void Updater::Notify(std::string message)
    {
        std::lock_guard lock(notification_mutex_);
        if (notifications_.size() < 8) {
            notifications_.push_back(std::move(message));
        }
    }

    void Updater::SetStatus(const UpdateStatus status, std::string message)
    {
        std::lock_guard lock(state_mutex_);
        state_.status = status;
        state_.message = std::move(message);
    }

    void Updater::CheckOnce()
    {
        SetStatus(UpdateStatus::Checking, "Checking for updates...");

        const std::wstring releases_url =
            L"https://api.github.com/repos/" + Widen(GWDASH_UPDATE_REPO) + L"/releases/latest";

        std::string etag = Trim(ReadFile(UpdaterEtagFile(), 256));
        http::Response response;
        std::string error;
        if (!http::Get(releases_url, etag, MAX_RELEASE_BYTES, response, error)) {
            SetStatus(UpdateStatus::Failed, "Update check failed: " + error);
            return;
        }
        if (response.status == 304) {
            SetStatus(UpdateStatus::UpToDate, "GWDash " GWDASH_VERSION " is up to date.");
            return;
        }
        if (response.status == 404) {
            SetStatus(UpdateStatus::UpToDate, "No releases published yet.");
            return;
        }
        if (response.status != 200) {
            SetStatus(UpdateStatus::Failed, "Update check failed: HTTP " + std::to_string(response.status));
            return;
        }
        if (!response.etag.empty()) {
            WriteFileAtomic(UpdaterEtagFile(), response.etag);
        }

        constexpr glz::opts lenient{.error_on_unknown_keys = false};
        github::Release release{};
        if (glz::read<lenient>(release, response.body)) {
            SetStatus(UpdateStatus::Failed, "Could not read the release info from GitHub.");
            return;
        }
        if (release.draft || release.prerelease) {
            SetStatus(UpdateStatus::UpToDate, "Latest GitHub release is draft/prerelease - ignoring.");
            return;
        }

        const std::string latest = StripVersionPrefix(release.tag_name);
        {
            std::lock_guard lock(state_mutex_);
            state_.latest_version = latest;
            state_.last_check = NowMs();
        }

        if (!IsNewer(latest, GWDASH_VERSION)) {
            SetStatus(UpdateStatus::UpToDate, "GWDash " GWDASH_VERSION " is up to date.");
            return;
        }

        if (!auto_install_.load()) {
            SetStatus(UpdateStatus::Available, "Version " + latest + " is available.");
            Notify("GWDash " + latest + " is available. Enable auto-update or run the installer again.");
            return;
        }

        // Already staged from an earlier session? Nothing left to do until restart.
        const fs::path pending_dir = PendingDirectory();
        const std::string staged_version = ReadFile(pending_dir / L"version.txt", 64);
        if (StripVersionPrefix(staged_version) == latest) {
            SetStatus(UpdateStatus::Staged, "Version " + latest + " is ready - restart Guild Wars to apply.");
            return;
        }

        const github::Asset* core = FindAsset(release, CORE_ASSET_NARROW);
        const github::Asset* sums = FindAsset(release, CHECKSUM_ASSET);
        const github::Asset* manifest_asset = FindAsset(release, MANIFEST_ASSET);
        const github::Asset* manifest_sig_asset = FindAsset(release, MANIFEST_SIG_ASSET);
        if (!core || !sums || !manifest_asset || !manifest_sig_asset) {
            SetStatus(UpdateStatus::Failed,
                      "Release " + latest + " is missing signed manifest assets.");
            return;
        }

        SetStatus(UpdateStatus::Downloading, "Downloading version " + latest + "...");

        http::Response manifest_response;
        if (!http::Get(Widen(manifest_asset->browser_download_url), {}, MAX_MANIFEST_BYTES,
                       manifest_response, error) ||
            manifest_response.status != 200) {
            SetStatus(UpdateStatus::Failed, "Could not download manifest.json.");
            return;
        }
        http::Response manifest_sig_response;
        if (!http::Get(Widen(manifest_sig_asset->browser_download_url), {}, MAX_MANIFEST_BYTES,
                       manifest_sig_response, error) ||
            manifest_sig_response.status != 200) {
            SetStatus(UpdateStatus::Failed, "Could not download manifest.sig.");
            return;
        }

        UpdateManifest manifest{};
        if (!ParseManifest(manifest_response.body, manifest, error)) {
            SetStatus(UpdateStatus::Failed, error);
            return;
        }
        if (StripVersionPrefix(manifest.version) != latest) {
            SetStatus(UpdateStatus::Failed, "manifest version does not match release tag.");
            return;
        }

        std::string signing_bytes;
        if (!ManifestSigningBytes(manifest, signing_bytes, error)) {
            SetStatus(UpdateStatus::Failed, error);
            return;
        }
        const std::string sig_hex = Trim(manifest_sig_response.body);
        if (!VerifyManifestSignature(signing_bytes, sig_hex, error)) {
            SetStatus(UpdateStatus::Failed, error);
            return;
        }

        http::Response checksums;
        if (!http::Get(Widen(sums->browser_download_url), {}, MAX_CHECKSUM_BYTES, checksums, error,
                       L"*/*") ||
            checksums.status != 200) {
            SetStatus(UpdateStatus::Failed, "Could not download SHA256SUMS.");
            return;
        }

        std::string expected;
        for (const ManifestFile& file : manifest.files) {
            if (file.name == CORE_ASSET_NARROW) {
                expected = file.sha256;
                break;
            }
        }
        if (expected.empty()) {
            expected = FindChecksum(checksums.body, CORE_ASSET_NARROW);
        }
        if (expected.empty()) {
            SetStatus(UpdateStatus::Failed, std::string("No checksum for ") + CORE_ASSET_NARROW);
            return;
        }

        std::error_code ec;
        fs::path download = pending_dir / CORE_ASSET;
        download += L".part";
        if (!http::Download(Widen(core->browser_download_url), download, MAX_PAYLOAD_BYTES, error)) {
            SetStatus(UpdateStatus::Failed, "Download failed: " + error);
            return;
        }

        if (http::FileSha256(download) != expected) {
            fs::remove(download, ec);
            SetStatus(UpdateStatus::Failed, "Checksum mismatch - update discarded.");
            return;
        }

        const fs::path target = pending_dir / CORE_ASSET;
        fs::remove(target, ec);
        fs::rename(download, target, ec);
        if (ec) {
            fs::remove(download, ec);
            SetStatus(UpdateStatus::Failed, "Could not stage the update.");
            return;
        }

        WriteFileAtomic(pending_dir / L"version.txt", latest);

        // The loader cannot replace itself while Windows holds it open, so just
        // flag a mismatch instead of pretending we can update it.
        const std::string loader_expected = FindChecksum(checksums.body, LOADER_ASSET);
        const std::string loader_actual = http::FileSha256(PluginsDirectory() / LOADER_ASSET);
        const bool loader_outdated =
            !loader_expected.empty() && !loader_actual.empty() && loader_expected != loader_actual;
        {
            std::lock_guard lock(state_mutex_);
            state_.loader_outdated = loader_outdated;
        }

        SetStatus(UpdateStatus::Staged, "Version " + latest + " is ready - restart Guild Wars to apply.");
        Notify("GWDash " + latest + " downloaded. It will be active the next time you start Guild Wars.");
        if (loader_outdated) {
            Notify("This release also updates the loader. Run the installer once to pick it up: "
                   "irm https://gwdash.com/install.ps1 | iex");
        }
    }

    void Updater::Run()
    {
        int wait_seconds = FIRST_CHECK_DELAY_SECONDS;

        for (;;) {
            {
                std::unique_lock lock(wait_mutex_);
                wait_cv_.wait_for(lock, std::chrono::seconds(wait_seconds), [this] {
                    return stop_.load() || check_now_.exchange(false);
                });
            }
            if (stop_.load()) {
                break;
            }

            CheckOnce();
            wait_seconds = CHECK_INTERVAL_SECONDS;
        }

        finished_ = true;
    }
}
