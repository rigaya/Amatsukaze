/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include <cstdint>
#include "ConvertPix.h"
#include "rgy_simd.h"

void Convert1_16_to_10(void* dst, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch) {
    Convert1<uint16_t, 10, 16, false>((uint16_t*)dst, (const uint16_t*)top, (const uint16_t*)bottom, w, h, dpitch, tpitch, bpitch);
}

void Convert1_16_to_12(void* dst, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch) {
    Convert1<uint16_t, 12, 16, false>((uint16_t*)dst, (const uint16_t*)top, (const uint16_t*)bottom, w, h, dpitch, tpitch, bpitch);
}

void Convert2_16_to_10(void* dstU, void* dstV, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch) {
    Convert2<uint16_t, uint32_t, 10, 16, false>((uint16_t*)dstU, (uint16_t*)dstV, (const uint16_t*)top, (const uint16_t*)bottom, w, h, dpitch, tpitch, bpitch);
}

void Convert2_16_to_12(void* dstU, void* dstV, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch) {
    Convert2<uint16_t, uint32_t, 10, 12, false>((uint16_t*)dstU, (uint16_t*)dstV, (const uint16_t*)top, (const uint16_t*)bottom, w, h, dpitch, tpitch, bpitch);
}

ConvertPixFuncs::ConvertPixFuncs() : convert1(nullptr), convert2(nullptr) {}

ConvertPixFuncs::ConvertPixFuncs(int dstDepth, int srcDepth) :
    convert1(nullptr),
    convert2(nullptr) {
    const bool avx2 = ((get_availableSIMD() & RGY_SIMD::AVX2) == RGY_SIMD::AVX2);
    if (srcDepth == 16) {
        if (dstDepth == 10) {
            convert1 = avx2 ? &Convert1_16_to_10_AVX2 : &Convert1_16_to_10;
            convert2 = avx2 ? &Convert2_16_to_10_AVX2 : &Convert2_16_to_10;
        } else if (dstDepth == 12) {
            convert1 = avx2 ? &Convert1_16_to_12_AVX2 : &Convert1_16_to_12;
            convert2 = avx2 ? &Convert2_16_to_12_AVX2 : &Convert2_16_to_12;
        }
    }
}
