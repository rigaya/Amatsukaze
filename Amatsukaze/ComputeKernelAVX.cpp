/**
* Amatsukaze AVX Compute Kernel
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

// このファイルはAVXでコンパイル
#include "ComputeKernel.h"
#include "ComputeKernelSIMD.h"

// 5つだけの水平加算
__m128 hsum5_256_ps(__m256 x) {
    // hiQuad = ( -, -, -, x4 )
    const __m128 hiQuad = _mm256_extractf128_ps(x, 1);
    // loQuad = ( x3, x2, x1, x0 )
    const __m128 loQuad = _mm256_castps256_ps128(x);
    // loDual = ( -, -, x1, x0 )
    const __m128 loDual = loQuad;
    // hiDual = ( -, -, x3, x2 )
    const __m128 hiDual = _mm_movehl_ps(loQuad, loQuad);
    // sumDual = ( -, -, x1+x3, x0+x2 )
    const __m128 sumDual = _mm_add_ps(loDual, hiDual);
    // lo = ( -, -, -, x0+x2+x4 )
    const __m128 lo = _mm_add_ss(sumDual, hiQuad);
    // hi = ( -, -, -, x1+x3 )
    const __m128 hi = _mm_shuffle_ps(sumDual, sumDual, 0x1);
    // sum = ( -, -, -, x0+x1+x2+x3+x4 )
    const __m128 sum = _mm_add_ss(lo, hi);
    return sum;
}

float CalcCorrelation5x5_AVX(const float* k, const float* Y, int x, int y, int w, float* pavg) {
    return CalcCorrelation5x5_AVX_AVX2<false>(k, Y, x, y, w, pavg);
} 