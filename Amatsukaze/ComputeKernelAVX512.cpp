/**
* Amatsukaze AVX512 Compute Kernel
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

// このファイルはAVX512でコンパイル
#include "ComputeKernel.h"
#include <algorithm>

namespace {

inline uint8_t BilateralFilter5x5U8RangeLUTScalarPixel(const uint8_t* srcBase, const int srcPitch, const int w, const int h, const int x, const int y, const float* spatial, const float* rangeWeight) {
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

void BilateralFilter5x5U8RangeLUT_AVX512(uint8_t* dst, const uint8_t* srcBase, int srcPitch, int w, int h, const float* spatial, const float* rangeWeight, uint8_t maxv, int y0, int y1) {
    (void)maxv;
    constexpr int radius = 2;
    constexpr int lanes = 16;
    alignas(64) float outBuf[lanes];

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
            const __m128i center16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(centerRow + x));
            const __m512 centerf = _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(center16));
            __m512 wsum = _mm512_setzero_ps();
            __m512 vsum = _mm512_setzero_ps();
            int k = 0;

            for (int row = 0; row < 5; row++) {
                for (int dx = -radius; dx <= radius; dx++, k++) {
                    const __m128i ref16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rowPtrs[row] + x + dx));
                    const __m128i diff16 = _mm_add_epi8(_mm_subs_epu8(center16, ref16), _mm_subs_epu8(ref16, center16));
                    const __m512i diff32 = _mm512_cvtepu8_epi32(diff16);
                    const __m512 ww = _mm512_mul_ps(_mm512_set1_ps(spatial[k]), _mm512_i32gather_ps(diff32, rangeWeight, sizeof(float)));
                    const __m512 refv = _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(ref16));
                    wsum = _mm512_add_ps(wsum, ww);
                    vsum = _mm512_fmadd_ps(ww, refv, vsum);
                }
            }

            const __m512 outv = _mm512_div_ps(vsum, wsum);
            _mm512_store_ps(outBuf, outv);
            for (int i = 0; i < lanes; i++) {
                dstRow[x + i] = (uint8_t)std::clamp((int)(outBuf[i] + 0.5f), 0, 255);
            }
        }

        for (; x < w; x++) {
            dstRow[x] = BilateralFilter5x5U8RangeLUTScalarPixel(srcBase, srcPitch, w, h, x, y, spatial, rangeWeight);
        }
    }
}
