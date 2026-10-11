/**
* Amatsukaze AVX Compute Kernel
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#pragma once

#include <immintrin.h>
#include "rgy_osdep.h"

// 既存の水平加算と同じ演算木を、走査融合経路だけでインライン化する。
RGY_FORCEINLINE __m128 hsum5_256_ps_inline(__m256 x) {
    const __m128 hiQuad = _mm256_extractf128_ps(x, 1);
    const __m128 loQuad = _mm256_castps256_ps128(x);
    const __m128 sumDual = _mm_add_ps(loQuad, _mm_movehl_ps(loQuad, loQuad));
    const __m128 lo = _mm_add_ss(sumDual, hiQuad);
    const __m128 hi = _mm_shuffle_ps(sumDual, sumDual, 0x1);
    return _mm_add_ss(lo, hi);
}

// k,Yは後ろに3要素はみ出して読む
template<bool avx2, bool inlineSum = false>
RGY_FORCEINLINE float CalcCorrelation5x5_AVX_AVX2(const float* k, const float* Y, int x, int y, int w, float* pavg) {
    const auto y0 = _mm256_loadu_ps(Y + (x - 2) + w * (y - 2));
    const auto y1 = _mm256_loadu_ps(Y + (x - 2) + w * (y - 1));
    const auto y2 = _mm256_loadu_ps(Y + (x - 2) + w * (y + 0));
    const auto y3 = _mm256_loadu_ps(Y + (x - 2) + w * (y + 1));
    const auto y4 = _mm256_loadu_ps(Y + (x - 2) + w * (y + 2));

    auto vysum =
        _mm256_add_ps(
            _mm256_add_ps(
                _mm256_add_ps(y0, y1),
                _mm256_add_ps(y2, y3)),
            y4);

    const float avgmul = 1.0f / 25.0f;
    const __m128 vsumss = (inlineSum ? hsum5_256_ps_inline(vysum) : hsum5_256_ps(vysum));
    const __m128 vavgss = _mm_mul_ss(vsumss, _mm_load_ss(&avgmul));

    float avg;
    _mm_store_ss(&avg, vavgss);
    __m256 vavg = (avx2) ? _mm256_broadcastss_ps(vavgss) : _mm256_broadcast_ss(&avg);

    const auto k0 = _mm256_loadu_ps(k + 0);
    const auto k1 = _mm256_loadu_ps(k + 5);
    const auto k2 = _mm256_loadu_ps(k + 10);
    const auto k3 = _mm256_loadu_ps(k + 15);
    const auto k4 = _mm256_loadu_ps(k + 20);

    const auto y0diff = _mm256_sub_ps(y0, vavg);
    const auto y1diff = _mm256_sub_ps(y1, vavg);
    const auto y2diff = _mm256_sub_ps(y2, vavg);
    const auto y3diff = _mm256_sub_ps(y3, vavg);
    const auto y4diff = _mm256_sub_ps(y4, vavg);

    auto vsum =
        (avx2)
        ? _mm256_fmadd_ps(k4, y4diff,
            _mm256_add_ps(
                _mm256_fmadd_ps(k0, y0diff, _mm256_mul_ps(k1, y1diff)),
                _mm256_fmadd_ps(k2, y2diff, _mm256_mul_ps(k3, y3diff))))
        : _mm256_add_ps(
            _mm256_add_ps(
                _mm256_add_ps(_mm256_mul_ps(k0, y0diff), _mm256_mul_ps(k1, y1diff)),
                _mm256_add_ps(_mm256_mul_ps(k2, y2diff), _mm256_mul_ps(k3, y3diff))),
            _mm256_mul_ps(k4, y4diff));

    float sum;
    _mm_store_ss(&sum, (inlineSum ? hsum5_256_ps_inline(vsum) : hsum5_256_ps(vsum)));

    if (pavg) *pavg = avg;
    return sum;
} 