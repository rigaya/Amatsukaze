/**
* Amatsukaze AVX Compute Kernel
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#pragma once

#include <immintrin.h>

// CPU機能の確認
bool IsAVXAvailable();
bool IsAVX2Available();

// 関数宣言
__m128 hsum5_256_ps(__m256 x);
float CalcCorrelation5x5_AVX(const float* k, const float* Y, int x, int y, int w, float* pavg);
float CalcCorrelation5x5_AVX2(const float* k, const float* Y, int x, int y, int w, float* pavg);
void removeLogoLineAVX2(float *dst, const float *src, const int srcStride, const float *logoAY, const float *logoBY, const int logowidth, const float maxv, const float fade);
