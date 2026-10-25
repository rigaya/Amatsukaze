#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace logo {

// 8bitは元の値と変換結果が一致するので、行ピッチを考慮したコピーだけで保存する。
template<typename pixel_t>
inline void CopyLogoRoiToCache(uint8_t* dst, const pixel_t* src, const int pitch,
    const int width, const int height, const int bitDepth) {
    if (width <= 0 || height <= 0) return;
    if constexpr (std::is_same_v<pixel_t, uint8_t>) {
        if (bitDepth == 8) {
            if (pitch == width) {
                std::memcpy(dst, src, (size_t)width * height);
            } else {
                for (int y = 0; y < height; y++) {
                    std::memcpy(dst + (size_t)y * width, src + (std::ptrdiff_t)y * pitch, width);
                }
            }
            return;
        }
    }
    // 高bit入力などは、従来と同じ丸めと飽和で8bitへ変換する。
    const int maxv = std::max(1, (1 << bitDepth) - 1);
    for (int y = 0; y < height; y++) {
        const pixel_t* srcLine = src + (std::ptrdiff_t)y * pitch;
        uint8_t* dstLine = dst + (size_t)y * width;
        for (int x = 0; x < width; x++) {
            const int value = (int)srcLine[x];
            dstLine[x] = (uint8_t)(std::clamp)((value * 255 + maxv / 2) / maxv, 0, 255);
        }
    }
}

} // 名前空間 logo
