#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace logo {

inline constexpr int kThinLineMinLength = 24;
inline constexpr int kThinLineProbeOffset = 2;
inline constexpr double kThinLineMaxOutsideOnRatio = 0.3;
inline constexpr int kThinLineOverhangTolerance = 4;

// 行帯の弱い文字を拾う閾値と、背景ノイズ・縦構造を除外する探索条件。
inline constexpr float kRowExtendOnRatio = 0.15f;
inline constexpr float kRowExtendFloorMultiplier = 3.0f;
inline constexpr double kRowExtendFloorQuantile = 0.99;
inline constexpr int kRowExtendBandPadding = 1;
inline constexpr int kRowExtendProbeOffset = 2;
inline constexpr int kRowExtendMinOffColumns = 6;

struct RowBandRect {
    int x;
    int y;
    int w;
    int h;
};

struct RowBandExtendResult {
    bool extended = false;
    float floor = 0.0f;
    float peak = 0.0f;
    float threshold = 0.0f;
    int beforeMinX = 0;
    int beforeMaxX = -1;
    int minX = 0;
    int maxX = -1;
    int bandH = 0;
};

// 座標の両端は包含。入力画像と縦範囲は変更せず、横範囲と診断値だけを返す。
inline RowBandExtendResult TryExtendRowBand(const std::vector<float>& score, const int width, const int height, const RowBandRect& rect) {
    RowBandExtendResult result;
    result.beforeMinX = result.minX = rect.x;
    if (width <= 0 || height <= 0 || score.size() != (size_t)width * height
        || rect.x < 0 || rect.y < 0 || rect.w <= 0 || rect.h <= 0
        || rect.x >= width || rect.y >= height || rect.w > width - rect.x || rect.h > height - rect.y) return result;
    result.beforeMaxX = result.maxX = rect.x + rect.w - 1;
    const int rectMaxY = rect.y + rect.h - 1;
    const int bandMinY = std::max(0, rect.y - kRowExtendBandPadding);
    const int bandMaxY = std::min(height - 1, rectMaxY + kRowExtendBandPadding);
    result.bandH = bandMaxY - bandMinY + 1;

    // 床は行帯に限らず矩形外の画像全体から求める。ノイズが多い画像は保守的に扱う。
    std::vector<float> outside;
    outside.reserve(score.size() - (size_t)rect.w * rect.h);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const float value = score[x + (size_t)y * width];
            if (!std::isfinite(value)) return result;
            if (x >= rect.x && x <= result.beforeMaxX && y >= rect.y && y <= rectMaxY) {
                result.peak = std::max(result.peak, value);
            } else {
                outside.push_back(value);
            }
        }
    }
    if (outside.empty()) return result;
    const size_t floorIndex = (size_t)((outside.size() - 1) * kRowExtendFloorQuantile);
    std::nth_element(outside.begin(), outside.begin() + floorIndex, outside.end());
    result.floor = outside[floorIndex];
    result.threshold = std::max(kRowExtendFloorMultiplier * result.floor, kRowExtendOnRatio * result.peak);
    // 信号がない画像では、ゼロ同士の比較で画面端まで広がることを防ぐ。
    if (result.peak <= 0.0f || result.threshold <= 0.0f) return result;

    const auto isOnColumn = [&](const int x) {
        float bandPeak = 0.0f;
        for (int y = bandMinY; y <= bandMaxY; y++) bandPeak = std::max(bandPeak, score[x + (size_t)y * width]);
        if (bandPeak < result.threshold) return false;
        // 帯の端から2px離して帯高ぶんを見る。画像外は信号なしとして扱う。
        const int upperEnd = bandMinY - kRowExtendProbeOffset;
        const int lowerStart = bandMaxY + kRowExtendProbeOffset;
        for (int y = std::max(0, upperEnd - result.bandH + 1); y <= upperEnd; y++) {
            if (score[x + (size_t)y * width] >= result.threshold) return false;
        }
        for (int y = lowerStart; y <= std::min(height - 1, lowerStart + result.bandH - 1); y++) {
            if (score[x + (size_t)y * width] >= result.threshold) return false;
        }
        return true;
    };
    const int maxOffColumns = std::max(kRowExtendMinOffColumns, result.bandH);
    const auto extend = [&](const int start, const int direction) {
        int lastOn = start;
        int offColumns = 0;
        for (int x = start + direction; x >= 0 && x < width; x += direction) {
            if (isOnColumn(x)) {
                lastOn = x;
                offColumns = 0;
            } else if (++offColumns >= maxOffColumns) {
                break;
            }
        }
        return lastOn;
    };
    result.minX = extend(result.beforeMinX, -1);
    result.maxX = extend(result.beforeMaxX, 1);
    result.extended = result.minX < result.beforeMinX || result.maxX > result.beforeMaxX;
    return result;
}

struct ThinLineTrimResult {
    std::vector<uint8_t> binary;
    int removedPixels = 0;
    int maxOverhang = 0;
};

inline ThinLineTrimResult TrimOverhangingThinLines(const std::vector<uint8_t>& binary, const int width, const int height) {
    ThinLineTrimResult result{ binary, 0, 0 };
    if (width <= 0 || height <= 0 || binary.size() != (size_t)width * height) return result;
    constexpr int kDirectionCount = 2;
    constexpr uint8_t kHorizontalThinLine = 1;
    constexpr uint8_t kVerticalThinLine = 2;
    std::vector<uint8_t> lineMask(binary.size(), 0);
    // 元マスクだけを読み、両方向の細線候補を列挙する。走査順で結果が変わらないようにする。
    for (int direction = 0; direction < kDirectionCount; direction++) {
        const bool horizontal = direction == 0;
        const int outerSize = horizontal ? height : width;
        const int innerSize = horizontal ? width : height;
        const auto offset = [=](const int outer, const int inner) { return horizontal ? inner + outer * width : outer + inner * width; };
        for (int outer = 0; outer < outerSize; outer++) {
            for (int start = 0; start < innerSize;) {
                if (!binary[offset(outer, start)]) { start++; continue; }
                int end = start;
                while (end < innerSize && binary[offset(outer, end)]) end++;
                const int length = end - start;
                if (length >= kThinLineMinLength) {
                    int beforeOn = 0, afterOn = 0;
                    for (int inner = start; inner < end; inner++) {
                        if (outer >= kThinLineProbeOffset && binary[offset(outer - kThinLineProbeOffset, inner)]) beforeOn++;
                        if (outer + kThinLineProbeOffset < outerSize && binary[offset(outer + kThinLineProbeOffset, inner)]) afterOn++;
                    }
                    // 画面外はOFFとする。片側が文字本体なら、太い構造として細線扱いを避ける。
                    if (beforeOn < kThinLineMaxOutsideOnRatio * length && afterOn < kThinLineMaxOutsideOnRatio * length) {
                        for (int inner = start; inner < end; inner++) lineMask[offset(outer, inner)] |= horizontal ? kHorizontalThinLine : kVerticalThinLine;
                    }
                }
                start = end;
            }
        }
    }
    int minX = width, minY = height, maxX = -1, maxY = -1;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int off = x + y * width;
            if (!binary[off] || lineMask[off]) continue;
            minX = std::min(minX, x); minY = std::min(minY, y);
            maxX = std::max(maxX, x); maxY = std::max(maxY, y);
        }
    }
    // 文字がなく細線しか残らないときは、枠線ロゴを消さず元のマスクを返す。
    if (maxX < minX || maxY < minY) return result;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int off = x + y * width;
            const int overhangX = std::max(0, std::max(minX - x, x - maxX));
            const int overhangY = std::max(0, std::max(minY - y, y - maxY));
            const bool trimHorizontal = (lineMask[off] & kHorizontalThinLine) && overhangX > kThinLineOverhangTolerance;
            const bool trimVertical = (lineMask[off] & kVerticalThinLine) && overhangY > kThinLineOverhangTolerance;
            if (!trimHorizontal && !trimVertical) continue;
            result.binary[off] = 0;
            result.removedPixels++;
            result.maxOverhang = std::max(result.maxOverhang, std::max(trimHorizontal ? overhangX : 0, trimVertical ? overhangY : 0));
        }
    }
    if (std::none_of(result.binary.begin(), result.binary.end(), [](const uint8_t value) { return value != 0; })) return ThinLineTrimResult{ binary, 0, 0 };
    return result;
}

} // 名前空間logo
