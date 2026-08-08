#include "PriceSnapshot.h"

#include <glaze/glaze.hpp>

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

namespace gwdash {
    bool ParseSnapshot(const std::string& json, PriceSnapshot& out)
    {
        constexpr glz::opts lenient{.error_on_unknown_keys = false};

        PriceSnapshot staged{};
        if (glz::read<lenient>(staged, json)) {
            return false;
        }
        if (staged.v != SNAPSHOT_VERSION) {
            return false;
        }

        out = std::move(staged);
        return true;
    }
}
