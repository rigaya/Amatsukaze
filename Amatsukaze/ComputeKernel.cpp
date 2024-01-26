/**
* Amtasukaze AVX Compute Kernel
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

// このファイルはAVXでコンパイル
#include <intrin.h>
#include <immintrin.h>
#include <stdio.h>

struct CPUInfo {
    bool initialized, avx, avx2;
};

static CPUInfo g_cpuinfo;

static inline void InitCPUInfo() {
    if (g_cpuinfo.initialized == false) {
        int cpuinfo[4];
        __cpuid(cpuinfo, 1);
        g_cpuinfo.avx = cpuinfo[2] & (1 << 28) || false;
        bool osxsaveSupported = cpuinfo[2] & (1 << 27) || false;
        g_cpuinfo.avx2 = false;
        if (osxsaveSupported && g_cpuinfo.avx)
        {
            // _XCR_XFEATURE_ENABLED_MASK = 0
            unsigned long long xcrFeatureMask = _xgetbv(0);
            g_cpuinfo.avx = (xcrFeatureMask & 0x6) == 0x6;
            if (g_cpuinfo.avx) {
                __cpuid(cpuinfo, 7);
                g_cpuinfo.avx2 = cpuinfo[1] & (1 << 5) || false;
            }
        }
        g_cpuinfo.initialized = true;
    }
}

bool IsAVXAvailable() {
    InitCPUInfo();
    return g_cpuinfo.avx;
}

bool IsAVX2Available() {
    InitCPUInfo();
    return g_cpuinfo.avx2;
}

#if 0
// https://qiita.com/beru/items/fff00c19968685dada68
// in  : ( x7, x6, x5, x4, x3, x2, x1, x0 )
// out : ( -,  -,  -, xsum )
inline __m128 hsum256_ps(__m256 x) {
    // hiQuad = ( x7, x6, x5, x4 )
    const __m128 hiQuad = _mm256_extractf128_ps(x, 1);
    // loQuad = ( x3, x2, x1, x0 )
    const __m128 loQuad = _mm256_castps256_ps128(x);
    // sumQuad = ( x3+x7, x2+x6, x1+x5, x0+x4 )
    const __m128 sumQuad = _mm_add_ps(loQuad, hiQuad);
    // loDual = ( -, -, x1+x5, x0+x4 )
    const __m128 loDual = sumQuad;
    // hiDual = ( -, -, x3+x7, x2+x6 )
    const __m128 hiDual = _mm_movehl_ps(sumQuad, sumQuad);
    // sumDual = ( -, -, x1+x3 + x5+x7, x0+x2 + x4+x6 )
    const __m128 sumDual = _mm_add_ps(loDual, hiDual);
    // lo = ( -, -, -, x0+x2 + x4+x6 )
    const __m128 lo = sumDual;
    // hi = ( -, -, -, x1+x3 + x5+x7 )
    const __m128 hi = _mm_shuffle_ps(sumDual, sumDual, 0x1);
    // sum = ( -, -, -, x0+x1+x2+x3 + x4+x5+x6+x7 )
    const __m128 sum = _mm_add_ss(lo, hi);
    return sum;
}
#endif

// 5つだけの水平加算
inline __m128 hsum5_256_ps(__m256 x) {
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

// k,Yは後ろに3要素はみ出して読む
template<bool avx2>
__forceinline float CalcCorrelation5x5_AVX_AVX2(const float* k, const float* Y, int x, int y, int w, float* pavg) {
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
    const __m128 vsumss = hsum5_256_ps(vysum);
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
    _mm_store_ss(&sum, hsum5_256_ps(vsum));

    if (pavg) *pavg = avg;
    return sum;
};

float CalcCorrelation5x5_AVX(const float* k, const float* Y, int x, int y, int w, float* pavg) {
    return CalcCorrelation5x5_AVX_AVX2<false>(k, Y, x, y, w, pavg);
}

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
