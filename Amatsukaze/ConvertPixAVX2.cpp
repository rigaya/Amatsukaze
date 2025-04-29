/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include <cstdint>
#define ENABLE_AVX2 1
#include "ConvertPix.h"

void Convert1_16_to_10_AVX2(void* dst, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch) {
    Convert1<uint16_t, 10, 16, true>((uint16_t*)dst, (const uint16_t*)top, (const uint16_t*)bottom, w, h, dpitch, tpitch, bpitch);
}

void Convert1_16_to_12_AVX2(void* dst, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch) {
    Convert1<uint16_t, 12, 16, true>((uint16_t*)dst, (const uint16_t*)top, (const uint16_t*)bottom, w, h, dpitch, tpitch, bpitch);
}

void Convert2_16_to_10_AVX2(void* dstU, void* dstV, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch) {
    Convert2<uint16_t, uint32_t, 10, 16, true>((uint16_t*)dstU, (uint16_t*)dstV, (const uint16_t*)top, (const uint16_t*)bottom, w, h, dpitch, tpitch, bpitch);
}

void Convert2_16_to_12_AVX2(void* dstU, void* dstV, const void* top, const void* bottom, int w, int h, int dpitch, int tpitch, int bpitch) {
    Convert2<uint16_t, uint32_t, 10, 12, true>((uint16_t*)dstU, (uint16_t*)dstV, (const uint16_t*)top, (const uint16_t*)bottom, w, h, dpitch, tpitch, bpitch);
}
