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

bool IsAVXAvailable() {
    return (get_availableSIMD() & RGY_SIMD::AVX) != RGY_SIMD::NONE;
}

bool IsAVX2Available() {
    return (get_availableSIMD() & RGY_SIMD::AVX2) != RGY_SIMD::NONE;
}