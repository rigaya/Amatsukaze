#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace logo {

// 統計収集の半開区間を分割する絶対X座標。空のROIは同じ始点と終点を返す。
inline std::vector<int> BuildLogoCollectXBoundaries(int scanWidth, int edgeMargin,
    int radius, int splits, bool alignBlock32) {
    const int margin = std::max(0, edgeMargin);
    const int innerWidth = static_cast<int>(std::max<int64_t>(0, int64_t(scanWidth) - int64_t(margin) * 2));
    const int partitions = std::max(1, std::min(splits, std::max(1, innerWidth)));
    std::vector<int> boundaries(static_cast<size_t>(partitions) + 1);
    for (int tile = 0; tile <= partitions; tile++) {
        boundaries[tile] = margin + static_cast<int>(int64_t(innerWidth) * tile / partitions);
    }
    if (alignBlock32) {
        constexpr int kBackgroundBlockPixels = 32;
        const int anchor = std::max(margin, radius);
        for (int tile = 1; tile < partitions; tile++) {
            // 既存のバッチ処理と同じ丸め方と順序で、両隣の内側に収まる場合だけ移す。
            const int aligned = anchor + ((boundaries[tile] - anchor) / kBackgroundBlockPixels) * kBackgroundBlockPixels;
            if (aligned > boundaries[tile - 1] && aligned < boundaries[tile + 1]) {
                boundaries[tile] = aligned;
            }
        }
    }
    return boundaries;
}

} // 名前空間 logo
