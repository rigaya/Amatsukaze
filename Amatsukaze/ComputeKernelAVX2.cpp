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