#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace gwdash {
    // Mirrors the payload published by the GWDash worker
    // (worker/lib/plugin-snapshot.ts). Field names match the JSON keys so glaze
    // can reflect them directly. Unknown keys are ignored on read, so the server
    // can add fields without breaking installed builds; `v` only changes on a
    // breaking change.
    constexpr int SNAPSHOT_VERSION = 1;

    struct TraderPrice {
        std::optional<double> price;
        std::optional<double> buy;
        std::optional<double> sell;
        std::string unit;
        std::string src;
        std::optional<int64_t> at;
    };

    struct ChatPrice {
        std::optional<double> price;
        std::string unit;
        int n = 0;
        std::optional<int64_t> at;
    };

    struct PriceSnapshot {
        int v = 0;
        int64_t at = 0;
        TraderPrice ecto;
        ChatPrice armbrace;
        ChatPrice blackdye;
    };

    /** Returns false on malformed JSON or an unsupported schema version. */
    bool ParseSnapshot(const std::string& json, PriceSnapshot& out);

    /** Gold formatting matching the web dashboard: 5600 -> "5.6k". */
    inline std::string FormatGold(const double value)
    {
        const long long gold = static_cast<long long>(value + 0.5);
        if (gold <= 0) {
            return "--";
        }

        char buffer[32];
        if (gold >= 1000) {
            const double k = static_cast<double>(gold) / 1000.0;
            if (k >= 100.0) {
                snprintf(buffer, sizeof(buffer), "%lldk", static_cast<long long>(k + 0.5));
            }
            else {
                snprintf(buffer, sizeof(buffer), "%.1fk", k);
            }
        }
        else {
            snprintf(buffer, sizeof(buffer), "%lld", gold);
        }
        return buffer;
    }

    /** Ecto-denominated formatting, e.g. "27e". */
    inline std::string FormatEcto(const double value)
    {
        if (value <= 0.0) {
            return "--";
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%llde", static_cast<long long>(value + 0.5));
        return buffer;
    }

    /** Compact age, e.g. "42s", "7m", "3h", "2d". */
    inline std::string FormatAge(const int64_t timestamp_ms, const int64_t now_ms)
    {
        if (timestamp_ms <= 0 || now_ms <= timestamp_ms) {
            return "now";
        }

        const int64_t seconds = (now_ms - timestamp_ms) / 1000;
        char buffer[32];
        if (seconds < 60) {
            snprintf(buffer, sizeof(buffer), "%llds", static_cast<long long>(seconds));
        }
        else if (seconds < 3600) {
            snprintf(buffer, sizeof(buffer), "%lldm", static_cast<long long>(seconds / 60));
        }
        else if (seconds < 86400) {
            snprintf(buffer, sizeof(buffer), "%lldh", static_cast<long long>(seconds / 3600));
        }
        else {
            snprintf(buffer, sizeof(buffer), "%lldd", static_cast<long long>(seconds / 86400));
        }
        return buffer;
    }
}
