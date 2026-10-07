#pragma once

#include "PgsPalette.h"
#include <algorithm>
#include <array>
#include <cstddef>

namespace amatsukaze::pgs {

// 整数除算と同じ丸め・飽和を全組合せで事前計算する。
// 静的初期化はスレッド安全で、字幕画像間でも表を共有する。
inline const std::array<std::array<uint8_t, 256>, 256>& StraightAlphaTable() {
    static const auto table = [] {
        std::array<std::array<uint8_t, 256>, 256> result{};
        for (unsigned alpha = 1; alpha < 256; alpha++) {
            for (unsigned value = 0; value < 256; value++) {
                result[alpha][value] = static_cast<uint8_t>(
                    (std::min)(255u, (value * 255u + alpha / 2u) / alpha));
            }
        }
        return result;
    }();
    return table;
}

inline void RestoreStraightAlphaRow(Rgba* destination, const uint8_t* source, size_t width) {
    const auto& table = StraightAlphaTable();
    for (size_t x = 0; x < width; x++, source += 4) {
        const auto& channels = table[source[3]];
        destination[x] = {channels[source[0]], channels[source[1]], channels[source[2]], source[3]};
    }
}

}
