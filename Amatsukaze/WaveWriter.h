/**
* Wave file writer
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <stdint.h>
#include <stdio.h>

#include "StreamUtils.h"

struct RiffHeader {
    uint32_t riff;
    uint32_t size;
    uint32_t type;
};

struct FormatChunk {
    uint32_t id;
    uint32_t size;
    uint16_t format;
    uint16_t channels;
    uint32_t samplerate;
    uint32_t bytepersec;
    uint16_t blockalign;
    uint16_t bitswidth;
};

struct DataChunk {
    uint32_t id;
    uint32_t size;
    // uint8_t waveformData[];
};

enum {
    WAVE_HEADER_LENGTH = sizeof(RiffHeader) + sizeof(FormatChunk) + sizeof(DataChunk)
};

uint32_t toBigEndian(uint32_t a);

// PCM WAVEのヘッダ (44バイト) を書き込む。
// numSamples: 1チャンネルあたりのサンプル数
// RIFFのサイズ欄は32bit符号なしなので、ファイル全体が2^32-1バイトを超える場合はFormatExceptionを投げる。
// bitswidthは8の倍数、channels・samplerateは正でなければならない。
void writeWaveHeader(FILE* fp, int channels, int samplerate, int bitswidth, int64_t numSamples);

// ネイティブ単体テスト用: writeWaveHeaderの出力を無名の一時ファイル経由で読み戻して返す。
// 成功時0、引数不正・サイズ超過時-1、出力バッファ不足時-2 (headerLengthに必要バイト数) を返す。
extern "C" AMATSUKAZE_API int WriteWaveHeaderForTest(int channels, int samplerate, int bitswidth, int64_t numSamples,
    uint8_t* output, size_t outputCapacity, size_t* headerLength);

