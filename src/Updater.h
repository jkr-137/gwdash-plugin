#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gwdash {
    enum class UpdateStatus {
        Idle,
        Checking,
        UpToDate,
        Available,
        Downloading,
        Staged,
        Failed,
    };

    struct UpdateState {
        UpdateStatus status = UpdateStatus::Idle;
        std::string installed_version;
        std::string latest_version;
        std::string message;
        /**
         * The release ships a different loader than the one on disk. The loader
         * is locked by Windows while it runs, so it can only be replaced by
         * re-running the installer.
         */
        bool loader_outdated = false;
        int64_t last_check = 0;
    };

    /**
     * Checks GitHub releases and stages a new payload into
     * <plugins>/GWDash/pending/. The loader swaps it in on the next Guild Wars
     * start, which is the only moment nothing holds a file lock on it.
     */
    class Updater {
      public:
        Updater() = default;
        ~Updater();

        Updater(const Updater&) = delete;
        Updater& operator=(const Updater&) = delete;

        void Start();
        void SignalStop();
        /** True when no worker thread is running, or it has run to completion. */
        [[nodiscard]] bool Finished() const { return !running_.load() || finished_.load(); }
        void Join();

        void RequestCheck();
        void SetAutoInstall(bool enabled);
        [[nodiscard]] bool AutoInstall() const { return auto_install_.load(); }

        [[nodiscard]] UpdateState State() const;

        /** Moves queued chat messages out; call from the game thread. */
        std::vector<std::string> TakeNotifications();

      private:
        void Run();
        void CheckOnce();
        void Notify(std::string message);
        void SetStatus(UpdateStatus status, std::string message);

        mutable std::mutex state_mutex_;
        UpdateState state_;

        std::mutex notification_mutex_;
        std::vector<std::string> notifications_;

        std::mutex wait_mutex_;
        std::condition_variable wait_cv_;

        std::thread thread_;
        std::atomic_bool stop_{false};
        std::atomic_bool check_now_{false};
        std::atomic_bool finished_{false};
        std::atomic_bool running_{false};
        std::atomic_bool auto_install_{true};
    };
} // namespace gwdash
