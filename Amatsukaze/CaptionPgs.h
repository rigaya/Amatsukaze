#pragma once

#include <cstddef>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include "common.h"

// 本体の字幕描画経路を初期化し、日本語グリフを描画して利用可能か確認する。
// fontFamilyがnullならプラットフォーム既定の日本語フォントを使う。
// 戻り値は成功時1、失敗時0。診断は呼出側のUTF-8バッファに書き込む。
extern "C" AMATSUKAZE_API int CaptionPgsCheckRenderer(
    int width, int height, const char* fontFamily,
    char* diagnostic, size_t diagnosticSize) noexcept;
