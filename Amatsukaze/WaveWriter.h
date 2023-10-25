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
    int32_t  size;
    uint32_t type;
};

struct FormatChunk {
    uint32_t id;
    int32_t size;
    int16_t format;
    uint16_t channels;
    uint32_t samplerate;
    uint32_t bytepersec;
    uint16_t blockalign;
    uint16_t bitswidth;
};

struct DataChunk {
    uint32_t id;
    int32_t size;
    // uint8_t waveformData[];
};

enum {
    WAVE_HEADER_LENGTH = sizeof(RiffHeader) + sizeof(FormatChunk) + sizeof(DataChunk)
};

uint32_t toBigEndian(uint32_t a);

// numSamples: 1チャンネルあたりのサンプル数
// エラー検出のため numSamples が64bitになっているがintを超える範囲に対応している訳ではないことに注意
void writeWaveHeader(FILE* fp, int channels, int samplerate, int bitswidth, int64_t numSamples);

