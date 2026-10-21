#pragma once

#include <algorithm>
#include <cstddef>

namespace logo {

// ROI内の座標を、余白を除いた内側の連番へ変換する。領域外は-1。
inline int LogoTemporalInnerPixelIndex(int width, int height, int margin, int x, int y) {
    if (width <= 0 || height <= 0 || margin < 0
        || width - margin <= margin || height - margin <= margin) return -1;
    if (x < margin || x >= width - margin || y < margin || y >= height - margin) return -1;
    return (y - margin) * (width - margin * 2) + x - margin;
}

// ROI全体の画素番号から変換する場合だけ、除算で座標を求める。
inline int LogoTemporalInnerPixelIndex(int width, int height, int margin, int off) {
    if (width <= 0 || off < 0) return -1;
    return LogoTemporalInnerPixelIndex(width, height, margin, off % width, off / width);
}

// 16画素のブロック内ではbinを先に並べる。末尾ブロックには余分な画素を確保しない。
// pixelはpixels未満、binは0以上256未満を呼び出し側で保証する。
inline size_t LogoTemporalHistogramIndex(size_t pixel, size_t pixels, int bin) {
    constexpr size_t kBlockPixels = 16;
    constexpr size_t kBins = 256;
    const size_t first = pixel / kBlockPixels * kBlockPixels;
    const size_t lanes = std::min(kBlockPixels, pixels - first);
    return first * kBins + static_cast<size_t>(bin) * lanes + pixel - first;
}

} // 名前空間 logo
