#include "PriceClient.h"

#include <algorithm>
#include <chrono>

#include <glaze/glaze.hpp>

#include "Http.h"
#include "Paths.h"

// Explicit glaze mappings rather than relying on reflection, so the JSON keys
// stay pinned to the payload contract in worker/lib/plugin-snapshot.ts even if
// the C++ members are ever renamed.
template <>
struct glz::meta<gwdash::TraderPrice> {
    using T = gwdash::TraderPrice;
    static constexpr auto value = object(
        "price", &T::price,
        "buy", &T::buy,
        "sell", &T::sell,
        "unit", &T::unit,
        "src", &T::src,
        "at", &T::at);
};

template <>
struct glz::meta<gwdash::ChatPrice> {
    using T = gwdash::ChatPrice;
    static constexpr auto value = object(
        "price", &T::price,
        "unit", &T::unit,
        "n", &T::n,
        "at", &T::at);
};

template <>
struct glz::meta<gwdash::PriceSnapshot> {
    using T = gwdash::PriceSnapshot;
    static constexpr auto value = object(
        "v", &T::v,
        "at", &T::at,
        "ecto", &T::ecto,
        "armbrace", &T::armbrace,
        "blackdye", &T::blackdye);
};

namespace {
    // R2 object behind a cached custom domain: no Worker invocation, so polling
    // costs nothing regardless of how many players run the plugin.
    constexpr wchar_t PRIMARY_URL[] = L"https://data.gwdash.com/prices.json";
    // Fallback for when the CDN/bucket is unreachable. Edge-cached with an ETag.
    constexpr wchar_t FALLBACK_URL[] = L"https://gwdash.com/api/plugin/prices";

    constexpr std::size_t MAX_SNAPSHOT_BYTES = 64 * 1024;
    constexpr int MIN_INTERVAL_SECONDS = 30;
    constexpr int IDLE_INTERVAL_SECONDS = 600;
    constexpr int MAX_BACKOFF_SECONDS = 900;

    int64_t NowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
}

namespace gwdash {
    bool ParseSnapshot(const std::string& json, PriceSnapshot& out)
    {
        constexpr glz::opts lenient{.error_on_unknown_keys = false};

        PriceSnapshot staged{};
        if (glz::read<lenient>(staged, json)) {
            return false;
        }
        if (staged.v < SNAPSHOT_VERSION) {
            return false;
        }

        out = std::move(staged);
        return true;
    }

    PriceClient::~PriceClient()
    {
        SignalStop();
        Join();
    }

    void PriceClient::Start()
    {
        if (running_.exchange(true)) {
            return;
        }
        stop_ = false;
        finished_ = false;
        thread_ = std::thread(&PriceClient::Run, this);
    }

    void PriceClient::SignalStop()
    {
        {
            // Taking the wait mutex around the flag is what makes the wakeup
            // reliable: without it the worker could start a five-minute sleep
            // right after checking the predicate and stall Toolbox's shutdown.
            std::lock_guard lock(wait_mutex_);
            stop_ = true;
        }
        wait_cv_.notify_all();
    }

    void PriceClient::Join()
    {
        if (thread_.joinable()) {
            thread_.join();
        }
        running_ = false;
    }

    void PriceClient::RequestRefresh()
    {
        {
            std::lock_guard lock(wait_mutex_);
            refresh_ = true;
        }
        wait_cv_.notify_all();
    }

    void PriceClient::SetIntervalSeconds(const int seconds)
    {
        interval_seconds_ = std::max(MIN_INTERVAL_SECONDS, seconds);
    }

    void PriceClient::SetIdle(const bool idle)
    {
        idle_ = idle;
    }

    PriceState PriceClient::State() const
    {
        std::lock_guard lock(state_mutex_);
        return state_;
    }

    void PriceClient::LoadCache()
    {
        const std::string cached = ReadFile(SnapshotCacheFile(), MAX_SNAPSHOT_BYTES);
        if (cached.empty()) {
            return;
        }

        PriceSnapshot snapshot{};
        if (!ParseSnapshot(cached, snapshot)) {
            return;
        }

        std::lock_guard lock(state_mutex_);
        state_.has_data = true;
        state_.from_cache = true;
        state_.snapshot = std::move(snapshot);
        state_.received_at = NowMs();
        state_.source = "cache";
    }

    bool PriceClient::FetchOnce(const std::wstring& url, const char* source, std::string& etag)
    {
        http::Response response;
        std::string error;
        if (!http::Get(url, etag, MAX_SNAPSHOT_BYTES, response, error)) {
            std::lock_guard lock(state_mutex_);
            state_.error = error;
            return false;
        }

        if (response.status == 304) {
            std::lock_guard lock(state_mutex_);
            state_.received_at = NowMs();
            state_.from_cache = false;
            state_.error.clear();
            state_.source = source;
            return true;
        }

        if (response.status != 200) {
            std::lock_guard lock(state_mutex_);
            state_.error = "HTTP " + std::to_string(response.status);
            return false;
        }

        PriceSnapshot snapshot{};
        if (!ParseSnapshot(response.body, snapshot)) {
            std::lock_guard lock(state_mutex_);
            state_.error = "unexpected snapshot format";
            return false;
        }

        etag = response.etag;
        WriteFileAtomic(SnapshotCacheFile(), response.body);

        std::lock_guard lock(state_mutex_);
        state_.has_data = true;
        state_.from_cache = false;
        state_.snapshot = std::move(snapshot);
        state_.received_at = NowMs();
        state_.error.clear();
        state_.source = source;
        return true;
    }

    void PriceClient::Run()
    {
        LoadCache();

        while (!stop_.load()) {
            {
                std::lock_guard lock(state_mutex_);
                state_.last_attempt = NowMs();
            }

            bool ok = FetchOnce(PRIMARY_URL, "data.gwdash.com", primary_etag_);
            if (!ok && !stop_.load()) {
                ok = FetchOnce(FALLBACK_URL, "gwdash.com/api", fallback_etag_);
            }

            const int base = idle_.load()
                                 ? std::max(interval_seconds_.load(), IDLE_INTERVAL_SECONDS)
                                 : interval_seconds_.load();
            int wait_seconds = base;
            {
                std::lock_guard lock(state_mutex_);
                if (ok) {
                    state_.consecutive_failures = 0;
                }
                else {
                    if (state_.consecutive_failures < 100) {
                        state_.consecutive_failures += 1;
                    }
                    // Exponential backoff, capped, so an outage does not turn
                    // into a tight retry loop.
                    const int shift = std::min(state_.consecutive_failures, 6);
                    wait_seconds = std::min(base * (1 << shift), MAX_BACKOFF_SECONDS);
                }
            }

            std::unique_lock lock(wait_mutex_);
            wait_cv_.wait_for(lock, std::chrono::seconds(wait_seconds), [this] {
                return stop_.load() || refresh_.exchange(false);
            });
        }

        finished_ = true;
    }
}
