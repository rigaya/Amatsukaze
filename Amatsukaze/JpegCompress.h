/**
* libjpeg-turbo を使用した JPEG 圧縮ユーティリティ
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <cstdint>
#include <vector>

namespace jpeg_utils {

// Planar YUV422P (YV16等) のフレームデータを JPEG に圧縮する
// planes[3]: Y, U, V 各プレーンへのポインタ
// strides[3]: 各プレーンの行ピッチ(bytes)
// width, height: 画像サイズ
// quality: JPEG品質 (1-100)
// output: 圧縮結果の JPEG バイト列（appendではなく上書き）
// 戻り値: 成功時 true
bool compressYUV422PToJpeg(
    const uint8_t* const planes[3],
    const int strides[3],
    int width, int height,
    int quality,
    std::vector<uint8_t>& output);

// Planar YUV444P のフレームデータを JPEG に圧縮する (4:4:4サブサンプリング)
// planes[3]: Y, Cb, Cr 各プレーンへのポインタ
// strides[3]: 各プレーンの行ピッチ(bytes)
// width, height: 画像サイズ
// quality: JPEG品質 (1-100)
// output: 圧縮結果の JPEG バイト列（appendではなく上書き）
// 戻り値: 成功時 true
bool compressYUV444PToJpeg(
    const uint8_t* const planes[3],
    const int strides[3],
    int width, int height,
    int quality,
    std::vector<uint8_t>& output);

} // namespace jpeg_utils
