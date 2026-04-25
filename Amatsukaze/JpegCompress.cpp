/**
* libjpeg-turbo を使用した JPEG 圧縮ユーティリティ
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "JpegCompress.h"
#include <turbojpeg.h>

namespace jpeg_utils {

bool compressYUV422PToJpeg(
    const uint8_t* const planes[3],
    const int strides[3],
    int width, int height,
    int quality,
    std::vector<uint8_t>& output)
{
    tjhandle handle = tj3Init(TJINIT_COMPRESS);
    if (!handle) {
        return false;
    }

    bool success = false;

    // TurboJPEG 3.x パラメータ設定
    tj3Set(handle, TJPARAM_SUBSAMP, TJSAMP_422);
    tj3Set(handle, TJPARAM_QUALITY, quality);

    // libjpeg-turbo にバッファ割り当てを任せる
    unsigned char* jpegBuf = nullptr;
    size_t jpegSize = 0;

    // planar YUV からJPEGに圧縮
    if (tj3CompressFromYUVPlanes8(handle, planes, width, strides, height, &jpegBuf, &jpegSize) == 0) {
        output.assign(jpegBuf, jpegBuf + jpegSize);
        success = true;
    } else {
        output.clear();
    }

    tj3Free(jpegBuf);
    tj3Destroy(handle);
    return success;
}

bool compressYUV444PToJpeg(
    const uint8_t* const planes[3],
    const int strides[3],
    int width, int height,
    int quality,
    std::vector<uint8_t>& output)
{
    tjhandle handle = tj3Init(TJINIT_COMPRESS);
    if (!handle) {
        return false;
    }

    bool success = false;

    // TurboJPEG 3.x パラメータ設定 (4:4:4サブサンプリング)
    tj3Set(handle, TJPARAM_SUBSAMP, TJSAMP_444);
    tj3Set(handle, TJPARAM_QUALITY, quality);

    // libjpeg-turbo にバッファ割り当てを任せる
    unsigned char* jpegBuf = nullptr;
    size_t jpegSize = 0;

    // planar YUV444 からJPEGに圧縮
    if (tj3CompressFromYUVPlanes8(handle, planes, width, strides, height, &jpegBuf, &jpegSize) == 0) {
        output.assign(jpegBuf, jpegBuf + jpegSize);
        success = true;
    } else {
        output.clear();
    }

    tj3Free(jpegBuf);
    tj3Destroy(handle);
    return success;
}

bool compressBGRToJpeg(
    const uint8_t* bgrData, int stride,
    int width, int height,
    int quality,
    std::vector<uint8_t>& output)
{
    tjhandle handle = tj3Init(TJINIT_COMPRESS);
    if (!handle) {
        return false;
    }

    bool success = false;

    tj3Set(handle, TJPARAM_QUALITY, quality);

    unsigned char* jpegBuf = nullptr;
    size_t jpegSize = 0;

    // packed BGR からJPEGに圧縮
    if (tj3Compress8(handle, bgrData, width, stride, height, TJPF_BGR, &jpegBuf, &jpegSize) == 0) {
        output.assign(jpegBuf, jpegBuf + jpegSize);
        success = true;
    } else {
        output.clear();
    }

    tj3Free(jpegBuf);
    tj3Destroy(handle);
    return success;
}

} // namespace jpeg_utils
