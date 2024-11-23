/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "convert_csp.h"
#if ENABLE_AVX2
#include <immintrin.h>
#endif

template <typename T, int dstdepth, int srcdepth, bool avx2 /*最適化により、SSE版とAVX2版が同じになるのを防止するためのダミー*/>
void Convert1(T* dst, const T* top, const T* bottom, int w, int h, int dpitch, int tpitch, int bpitch) {
    for (int y = 0; y < h; y += 2) {
        T* dst0 = dst + dpitch * (y + 0);
        T* dst1 = dst + dpitch * (y + 1);
        const T* src0 = top + tpitch * (y + 0);
        const T* src1 = bottom + bpitch * (y + 1);
        for (int x = 0; x < w; x++) {
            dst0[x] = conv_bit_depth_<dstdepth, srcdepth, 0>(src0[x]);
            dst1[x] = conv_bit_depth_<dstdepth, srcdepth, 0>(src1[x]);
        }
    }
#if ENABLE_AVX2
    if (avx2) _mm256_zeroupper();
#endif
}

#if 0
template <typename T, int dstdepth, int srcdepth, bool avx2 /*最適化により、SSE版とAVX2版が同じになるのを防止するためのダミー*/>
void Convert2(T* dstU, T* dstV, const T* top, const T* bottom, int w, int h, int dpitch, int tpitch, int bpitch) {
    for (int y = 0; y < h; y += 2) {
        T* dstU0 = dstU + dpitch * (y + 0);
        T* dstU1 = dstU + dpitch * (y + 1);
        T* dstV0 = dstV + dpitch * (y + 0);
        T* dstV1 = dstV + dpitch * (y + 1);
        const T* src0 = top + tpitch * (y + 0);
        const T* src1 = bottom + bpitch * (y + 1);
        for (int x = 0; x < w; x++) {
            dstU0[x] = conv_bit_depth_<dstdepth, srcdepth, 0>(src0[x * 2 + 0]);
            dstV0[x] = conv_bit_depth_<dstdepth, srcdepth, 0>(src0[x * 2 + 1]);
            dstU1[x] = conv_bit_depth_<dstdepth, srcdepth, 0>(src1[x * 2 + 0]);
            dstV1[x] = conv_bit_depth_<dstdepth, srcdepth, 0>(src1[x * 2 + 1]);
        }
    }
}
#else
// なぜかこっちでないと自動ベクトル化されない
template <typename T, typename Tx2, int dstdepth, int srcdepth, bool avx2 /*最適化により、SSE版とAVX2版が同じになるのを防止するためのダミー*/>
void Convert2(T* dstU, T* dstV, const T* top, const T* bottom, int w, int h, int dpitch, int tpitch, int bpitch) {
    for (int y = 0; y < h; y += 2) {
        T* dstU0 = dstU + dpitch * (y + 0);
        T* dstU1 = dstU + dpitch * (y + 1);
        T* dstV0 = dstV + dpitch * (y + 0);
        T* dstV1 = dstV + dpitch * (y + 1);
        const Tx2* src0 = (const Tx2*)(top + tpitch * (y + 0));
        const Tx2* src1 = (const Tx2*)(bottom + bpitch * (y + 1));
        for (int x = 0; x < w; x++) {
            dstU0[x] = conv_bit_depth_<dstdepth, srcdepth, 0>(src0[x] & ((T)-1));
            dstV0[x] = conv_bit_depth_<dstdepth, srcdepth, 0>(src0[x] >> ((sizeof(Tx2) - sizeof(T)) * 8));
        }
        for (int x = 0; x < w; x++) {
            dstU1[x] = conv_bit_depth_<dstdepth, srcdepth, 0>(src1[x] & ((T)-1));
            dstV1[x] = conv_bit_depth_<dstdepth, srcdepth, 0>(src1[x] >> ((sizeof(Tx2) - sizeof(T)) * 8));
        }
    }
#if ENABLE_AVX2
    if (avx2) _mm256_zeroupper();
#endif
}
#endif

void Convert1_16_to_10(void* dst, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch);
void Convert1_16_to_12(void* dst, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch);
void Convert2_16_to_10(void* dstU, void* dstV, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch);
void Convert2_16_to_12(void* dstU, void* dstV, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch);

void Convert1_16_to_10_AVX2(void* dst, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch);
void Convert1_16_to_12_AVX2(void* dst, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch);
void Convert2_16_to_10_AVX2(void* dstU, void* dstV, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch);
void Convert2_16_to_12_AVX2(void* dstU, void* dstV, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch);

struct ConvertPixFuncs {
    decltype(&Convert1_16_to_10) convert1;
    decltype(&Convert2_16_to_10) convert2;

    ConvertPixFuncs();
    ConvertPixFuncs(int dstDepth, int srcDepth);
};
