/**
* Amatsukaze AVX2 Compute Kernel
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

// このファイルはAVX2でコンパイル
#include "ComputeKernel.h"
#include "ComputeKernelSIMD.h"
#include <algorithm>

float CalcCorrelation5x5_AVX2(const float* k, const float* Y, int x, int y, int w, float* pavg) {
    return CalcCorrelation5x5_AVX_AVX2<true>(k, Y, x, y, w, pavg);
}

void removeLogoLineAVX2(float *dst, const float *src, const int srcStride, const float *logoAY, const float *logoBY, const int logowidth, const float maxv, const float fade) {
    const float invfade = 1.0f - fade;
    const __m256 vmaxv = _mm256_broadcast_ss(&maxv);
    const __m256 vfade = _mm256_broadcast_ss(&fade);
    const __m256 v1_fade = _mm256_broadcast_ss(&invfade);
    const int x_fin = logowidth & ~7;
    int x = 0;
    for (; x < x_fin; x += 8) {
        const __m256 srcv = _mm256_loadu_ps(src + x);
        const __m256 a = _mm256_loadu_ps(logoAY + x);
        const __m256 b = _mm256_loadu_ps(logoBY + x);
        const __m256 bg = _mm256_fmadd_ps(a, srcv, _mm256_mul_ps(b, vmaxv));
        const __m256 dstv = _mm256_fmadd_ps(vfade, bg, _mm256_mul_ps(v1_fade, srcv));
        _mm256_storeu_ps(dst + x, dstv);
    }
    for (; x < logowidth; x++) {
        const __m128 srcv = _mm_load_ss(src + x);
        const __m128 a = _mm_load_ss(logoAY + x);
        const __m128 b = _mm_load_ss(logoBY + x);
        const __m128 bg = _mm_fmadd_ss(a, srcv, _mm_mul_ss(b, _mm256_castps256_ps128(vmaxv)));
        const __m128 dstv = _mm_fmadd_ss(_mm256_castps256_ps128(vfade), bg, _mm_mul_ss(_mm256_castps256_ps128(v1_fade), srcv));
        _mm_store_ss(dst + x, dstv);
    }
}

namespace {

RGY_FORCEINLINE uint8_t BilateralFilter5x5U8RangeLUTScalarPixel(const uint8_t* srcBase, const int srcPitch, const int w, const int h, const int x, const int y, const float* spatial, const float* rangeWeight) {
    const int center = srcBase[y * srcPitch + x];
    float wsum = 0.0f;
    float vsum = 0.0f;
    int k = 0;
    for (int dy = -2; dy <= 2; dy++) {
        const int yy = std::clamp(y + dy, 0, h - 1);
        const uint8_t* srcRow = srcBase + yy * srcPitch;
        for (int dx = -2; dx <= 2; dx++, k++) {
            const int xx = std::clamp(x + dx, 0, w - 1);
            const int v = srcRow[xx];
            const float ww = spatial[k] * rangeWeight[std::abs(v - center)];
            wsum += ww;
            vsum += ww * (float)v;
        }
    }
    const float outv = (wsum > 1e-8f) ? (vsum / wsum) : (float)center;
    return (uint8_t)std::clamp((int)(outv + 0.5f), 0, 255);
}

}

void BilateralFilter5x5U8RangeLUT_AVX2(uint8_t* dst, const uint8_t* srcBase, int srcPitch, int w, int h, const float* spatial, const float* rangeWeight, uint8_t maxv, int y0, int y1) {
    (void)maxv;
    constexpr int radius = 2;
    constexpr int lanes = 8;
    alignas(32) float outBuf[lanes];

    for (int y = y0; y < y1; y++) {
        uint8_t* dstRow = dst + y * w;
        const uint8_t* centerRow = srcBase + y * srcPitch;
        const uint8_t* rowPtrs[5];
        for (int dy = -radius; dy <= radius; dy++) {
            const int yy = std::clamp(y + dy, 0, h - 1);
            rowPtrs[dy + radius] = srcBase + yy * srcPitch;
        }

        int x = 0;
        for (; x < std::min(radius, w); x++) {
            dstRow[x] = BilateralFilter5x5U8RangeLUTScalarPixel(srcBase, srcPitch, w, h, x, y, spatial, rangeWeight);
        }

        const int xVecEnd = std::max(radius, w - radius);
        for (; x + lanes <= xVecEnd; x += lanes) {
            const __m128i center8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(centerRow + x));
            const __m256 centerf = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(center8));
            __m256 wsum = _mm256_setzero_ps();
            __m256 vsum = _mm256_setzero_ps();
            int k = 0;

            for (int row = 0; row < 5; row++) {
                for (int dx = -radius; dx <= radius; dx++, k++) {
                    const __m128i ref8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(rowPtrs[row] + x + dx));
                    const __m128i diff8 = _mm_add_epi8(_mm_subs_epu8(center8, ref8), _mm_subs_epu8(ref8, center8));
                    const __m256i diff32 = _mm256_cvtepu8_epi32(diff8);
                    const __m256 ww = _mm256_mul_ps(_mm256_set1_ps(spatial[k]), _mm256_i32gather_ps(rangeWeight, diff32, sizeof(float)));
                    const __m256 refv = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(ref8));
                    wsum = _mm256_add_ps(wsum, ww);
                    vsum = _mm256_fmadd_ps(ww, refv, vsum);
                }
            }

            const __m256 outv = _mm256_div_ps(vsum, wsum);
            _mm256_store_ps(outBuf, outv);
            for (int i = 0; i < lanes; i++) {
                dstRow[x + i] = (uint8_t)std::clamp((int)(outBuf[i] + 0.5f), 0, 255);
            }
        }

        for (; x < w; x++) {
            dstRow[x] = BilateralFilter5x5U8RangeLUTScalarPixel(srcBase, srcPitch, w, h, x, y, spatial, rangeWeight);
        }
    }
}
