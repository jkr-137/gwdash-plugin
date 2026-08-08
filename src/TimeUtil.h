#pragma once

#include <chrono>
#include <cstdint>

namespace gwdash {
    inline int64_t NowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
} // namespace gwdash
