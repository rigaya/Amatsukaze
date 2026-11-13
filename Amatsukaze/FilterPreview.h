#pragma once
#include "StreamUtils.h"
#include <cstdint>

// pixelTypeは平面YUVの420、422、444をそれぞれ1、2、3で表す。
// SARが未設定の場合はsarWとsarHをともに0とする。
// フレームのポインタは同じオブジェクトへの次の取得呼び出しまで有効。
// CUDAフレームはスクリプト側でOnCPUによりCPUへ転送する。
// GetKFMPropsは成功1、プロパティなし0、取得エラー-1を返す。
enum FilterPreviewPixelType { FILTER_PREVIEW_YUV420 = 1, FILTER_PREVIEW_YUV422 = 2, FILTER_PREVIEW_YUV444 = 3 };
struct FilterPreviewInfo {
    int numFrames, fpsNum, fpsDen, width, height, sarW, sarH, bitDepth, pixelType;
};
extern "C" AMATSUKAZE_API void* FilterPreview_Open(AMTContext* ctx, const tchar* avsPath, FilterPreviewInfo* info);
extern "C" AMATSUKAZE_API int FilterPreview_GetFrame(void* p, int n, const uint8_t** planes, int* pitches);
extern "C" AMATSUKAZE_API int FilterPreview_GetKFMProps(void* p, int n, int* frameType, int* duration);
extern "C" AMATSUKAZE_API void FilterPreview_Close(void* p);
