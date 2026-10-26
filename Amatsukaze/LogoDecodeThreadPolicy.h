#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <utility>

namespace logo {

constexpr int kMaxLogoParallelDecoders = 15;

// CM解析の配分を共用し、デコーダー数の増え過ぎと過剰な内部並列を抑える。
inline int GetPreferredLogoDecoderCount(int processorCount) {
    processorCount = std::max(processorCount, 1);
    std::array<std::pair<int, int>, kMaxLogoParallelDecoders + 1> candidates;
    candidates[0] = std::make_pair(0, std::numeric_limits<int>::max());
    for (int count = 1; count <= kMaxLogoParallelDecoders; count++) {
        const int decodeThreads = processorCount / count;
        candidates[count] = std::make_pair(count,
            processorCount % count + std::max(0, decodeThreads - 4));
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second < b.second;
        const int distanceA = std::abs(a.first - 8);
        const int distanceB = std::abs(b.first - 8);
        if (distanceA != distanceB) return distanceA < distanceB;
        return a.first < b.first;
    });
    return std::max(candidates[0].first, 1);
}

inline int GetLogoDecoderThreadCount(int processorCount, int decoders) {
    return std::max(1, std::min(8, std::max(processorCount, 1) / std::max(decoders, 1)));
}

} // ロゴ解析の並列配分
