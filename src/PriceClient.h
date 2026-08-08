#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "PriceSnapshot.h"

namespace gwdash {
    struct PriceState {
        bool has_data = false;
        bool from_cache = false;
        PriceSnapshot snapshot;
        /** Local clock, ms: when the snapshot arrived (or was loaded from cache). */
        int64_t received_at = 0;
        int64_t last_attempt = 0;
        int consecutive_failures = 0;
        std::string error;
        std::string source;
    };

    /**
     * Polls the price snapshot on a background thread.
     *
     * Reads go to data.gwdash.com, a cached public R2 object, so they never hit
     * the GWDash Worker and cost nothing per player. /api/plugin/prices is only
     * used when that fails.
     */
    class PriceClient {
      public:
        PriceClient() = default;
        ~PriceClient();

        PriceClient(const PriceClient&) = delete;
        PriceClient& operator=(const PriceClient&) = delete;

        void Start();
        /** Asks the worker thread to finish; does not block. */
        void SignalStop();
        /** True when no worker thread is running, or it has run to completion. */
        [[nodiscard]] bool Finished() const { return !running_.load() || finished_.load(); }
        /** Joins the worker thread. Only call once Finished() is true. */
        void Join();

        void RequestRefresh();
        void SetIntervalSeconds(int seconds);
        /** Throttles polling while Guild Wars has been in the background. */
        void SetIdle(bool idle);

        [[nodiscard]] PriceState State() const;

      private:
        void Run();
        void LoadCache();
        bool FetchOnce(const std::wstring& url, const char* source, std::string& etag);

        mutable std::mutex state_mutex_;
        PriceState state_;

        std::mutex wait_mutex_;
        std::condition_variable wait_cv_;

        std::thread thread_;
        std::atomic_bool stop_{false};
        std::atomic_bool refresh_{false};
        std::atomic_bool finished_{false};
        std::atomic_bool running_{false};
        std::atomic_int interval_seconds_{120};
        std::atomic_bool idle_{false};

        std::string primary_etag_;
        std::string fallback_etag_;
    };
} // namespace gwdash
