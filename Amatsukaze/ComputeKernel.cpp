/**
* Amatsukaze AVX Compute Kernel
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

// 実装はComputeKernelAVX.cppとComputeKernelAVX2.cppに移動しました
#include "ComputeKernel.h"

// このファイルはCPUの機能検出のコードまたはフォールバック実装のためのプレースホルダーです
// 実際の実装はAVX, AVX2専用のファイルに分割されています

#include "rgy_simd.h"

#if !ENABLE_X86_SIMD
#include <algorithm>
#include <cmath>
#include <limits>
#endif

static RGY_SIMD GetAvailableSIMDCached() {
    static const RGY_SIMD simd = get_availableSIMD();
    return simd;
}

bool IsAVXAvailable() {
    return (GetAvailableSIMDCached() & RGY_SIMD::AVX) != RGY_SIMD::NONE;
}

bool IsAVX2Available() {
    return (GetAvailableSIMDCached() & RGY_SIMD::AVX2) != RGY_SIMD::NONE;
}

bool IsAVX512BWAvailable() {
    return (GetAvailableSIMDCached() & RGY_SIMD::AVX512BW) == RGY_SIMD::AVX512BW;
}

#if !ENABLE_X86_SIMD
// ARMではx86 SIMD版と同じ公開関数をスカラー処理で提供する
extern float CalcCorrelation5x5(const float* k, const float* y, int x, int row, int width, float* average);
extern void removeLogoLine(float* dst, const float* src, int srcStride, const float* logoAY,
    const float* logoBY, int logoWidth, float maxValue, float fade);

float CalcCorrelation5x5_AVX(const float* k, const float* y, int x, int row, int width, float* average) {
    return CalcCorrelation5x5(k, y, x, row, width, average);
}

float CalcCorrelation5x5_AVX2(const float* k, const float* y, int x, int row, int width, float* average) {
    return CalcCorrelation5x5(k, y, x, row, width, average);
}

void removeLogoLineAVX2(float* dst, const float* src, int srcStride, const float* logoAY,
    const float* logoBY, int logoWidth, float maxValue, float fade) {
    removeLogoLine(dst, src, srcStride, logoAY, logoBY, logoWidth, maxValue, fade);
}

void prepareLogoBackgroundLineAVX2(float *dst, const float *src, const float *logoAY,
    const float *logoBY, int width, float maxv) {
    for (int x = 0; x < width; x++) dst[x] = logoAY[x] * src[x] + logoBY[x] * maxv;
}

void blendLogoBackgroundLineAVX2(float *dst, const float *src, const float *background,
    int width, float fade) {
    for (int x = 0; x < width; x++) dst[x] = fade * background[x] + (1.0f - fade) * src[x];
}

static uint8_t BilateralFilterPixel(const uint8_t* src, int pitch, int width, int height,
    int x, int y, const float* spatial, const float* rangeWeight) {
    const int center = src[y * pitch + x];
    float weightSum = 0.0f;
    float valueSum = 0.0f;
    int kernelIndex = 0;
    for (int dy = -2; dy <= 2; dy++) {
        const int sampleY = std::clamp(y + dy, 0, height - 1);
        for (int dx = -2; dx <= 2; dx++, kernelIndex++) {
            const int sampleX = std::clamp(x + dx, 0, width - 1);
            const int value = src[sampleY * pitch + sampleX];
            const float weight = spatial[kernelIndex] * rangeWeight[std::abs(value - center)];
            weightSum += weight;
            valueSum += weight * value;
        }
    }
    const float result = weightSum > 1e-8f ? valueSum / weightSum : (float)center;
    return (uint8_t)std::clamp((int)(result + 0.5f), 0, 255);
}

static void BilateralFilterFallback(uint8_t* dst, const uint8_t* src, int pitch, int width, int height,
    const float* spatial, const float* rangeWeight, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < width; x++) {
            dst[y * width + x] = BilateralFilterPixel(src, pitch, width, height, x, y, spatial, rangeWeight);
        }
    }
}

void BilateralFilter5x5U8RangeLUT_AVX2(uint8_t* dst, const uint8_t* src, int pitch, int width,
    int height, const float* spatial, const float* rangeWeight, uint8_t, int y0, int y1) {
    BilateralFilterFallback(dst, src, pitch, width, height, spatial, rangeWeight, y0, y1);
}

void BilateralFilter5x5U8RangeLUT_AVX512(uint8_t* dst, const uint8_t* src, int pitch, int width,
    int height, const float* spatial, const float* rangeWeight, uint8_t, int y0, int y1) {
    BilateralFilterFallback(dst, src, pitch, width, height, spatial, rangeWeight, y0, y1);
}

bool TryEstimateBgEvalSideContiguousU8_AVX2(const uint8_t* ptr, int length, int threshold,
    float& average, uint8_t& minValue, uint8_t& maxValue) {
    uint32_t sum = 0;
    minValue = std::numeric_limits<uint8_t>::max();
    maxValue = 0;
    for (int i = 0; i < length; i++) {
        sum += ptr[i];
        minValue = std::min(minValue, ptr[i]);
        maxValue = std::max(maxValue, ptr[i]);
    }
    average = (float)sum / length;
    return (int)maxValue - (int)minValue <= threshold;
}

// 非x86向けの補正edge計算。各画素内の順序を維持する。
void CalcCorrectedEdges32U8_AVX2(const uint8_t* src, int stride, float invMaxv, float* edges) {
    const int offsets[4] = {-1, 1, -stride, stride};
    for (int lane = 0; lane < 32; lane++) {
        const float center = (float)src[lane] * invMaxv;
        float maximum = 0.0f;
        for (int side = 0; side < 4; side++) {
            const float neighbor = (float)src[lane + offsets[side]] * invMaxv;
            const float raw = center - neighbor;
            if (raw > 0.0f) maximum = std::max(maximum, raw / (1.0f - neighbor + 1e-4f));
        }
        edges[lane] = maximum;
    }
}

// AVX2を使用しない環境でも同じ短い区間を集計する。
void CalcBgSideStatsVerticalBoundary32U8_AVX2(const uint8_t* src, int stride, int height,
    int x, int y, int radius, uint16_t* sums, uint8_t* mins, uint8_t* maxs) {
    const int first = std::max(0, y - radius);
    const int last = std::min(height - 1, y + radius);
    for (int side = 0; side < 4; side++) {
        for (int lane = 0; lane < 32; lane++) {
            unsigned sum = 0;
            uint8_t minv = 255, maxv = 0;
            const int len = side < 2 ? 2 * radius + 1 : last - first + 1;
            for (int i = 0; i < len; i++) {
                const int sx = side < 2 ? x + lane - radius + i : x + lane + (side == 2 ? -radius : radius);
                const int sy = side < 2 ? (side == 0 ? first : last) : first + i;
                const uint8_t value = src[sy * stride + sx];
                sum += value;
                minv = std::min(minv, value);
                maxv = std::max(maxv, value);
            }
            const int index = side * 32 + lane;
            sums[index] = (uint16_t)sum;
            mins[index] = minv;
            maxs[index] = maxv;
        }
    }
}

void PackBackground32Fixed88_AVX2(const float* bg, uint32_t validMask, uint16_t* out) {
    for (int lane = 0; lane < 32; lane++) {
        out[lane] = (validMask & (1u << lane))
            ? (uint16_t)(std::max(0.0f, std::min(255.0f, bg[lane])) * 256.0f + 0.5f) : 0xfffe;
    }
}

void CalcBgSideStatsBlock32U8_AVX2(const uint8_t* src, int stride, int x, int y, int radius,
    uint16_t* sideSums, uint8_t* sideMins, uint8_t* sideMaxs) {
    constexpr int lanes = 32;
    const int length = radius * 2 + 1;
    const uint8_t* sideStart[4] = {
        src + (y - radius) * stride + x - radius,
        src + (y + radius) * stride + x - radius,
        src + (y - radius) * stride + x - radius,
        src + (y - radius) * stride + x + radius,
    };
    const int sideStep[4] = { 1, 1, stride, stride };
    for (int side = 0; side < 4; side++) {
        for (int lane = 0; lane < lanes; lane++) {
            uint16_t sum = 0;
            uint8_t minValue = std::numeric_limits<uint8_t>::max();
            uint8_t maxValue = 0;
            for (int i = 0; i < length; i++) {
                const uint8_t value = sideStart[side][lane + i * sideStep[side]];
                sum += value;
                minValue = std::min(minValue, value);
                maxValue = std::max(maxValue, value);
            }
            const int index = side * lanes + lane;
            sideSums[index] = sum;
            sideMins[index] = minValue;
            sideMaxs[index] = maxValue;
        }
    }
}
#endif
