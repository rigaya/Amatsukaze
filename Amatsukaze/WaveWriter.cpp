/**
* Wave file writer
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "WaveWriter.h"

uint32_t toBigEndian(uint32_t a) {
    return (a >> 24) | ((a & 0xFF0000) >> 8) | ((a & 0xFF00) << 8) | (a << 24);
}

// numSamples: 1チャンネルあたりのサンプル数
void writeWaveHeader(FILE* fp, int channels, int samplerate, int bitswidth, int64_t numSamples) {
    if (channels <= 0 || channels > 0xFFFF || samplerate <= 0 || bitswidth <= 0 || bitswidth % 8 != 0 || bitswidth > 0xFFFF ||
        numSamples < 0) {
        THROW(FormatException, "Waveヘッダのパラメータが不正です");
    }

    // RIFFのサイズ欄は32bit符号なしなので、ファイル全体が2^32-1バイトを超えてはならない
    const int64_t MAX_SIZE = (int64_t(1) << 32) - 1;

    const int blockAlign = channels * (bitswidth / 8);
    if (blockAlign > 0xFFFF || int64_t(samplerate) * blockAlign > 0xFFFFFFFFLL) {
        THROW(FormatException, "Waveヘッダのブロックサイズまたはバイトレートが範囲外です");
    }
    const int headerSize = sizeof(RiffHeader) + sizeof(FormatChunk) + sizeof(DataChunk);
    if (numSamples > MAX_SIZE) {
        THROW(FormatException, "4GB超のWaveデータファイルはサポートしていません");
    }
    const int64_t dataSize = int64_t(blockAlign) * numSamples; // blockAlignは2^29未満、numSamplesは2^32未満なので溢れない
    const int64_t fileSize = headerSize + dataSize;

    if (fileSize > MAX_SIZE) {
        THROW(FormatException, "4GB超のWaveデータファイルはサポートしていません");
    }

    struct Header {
        RiffHeader riffHeader;
        FormatChunk formatChunk;
        DataChunk dataChunk;
    } header = {
        { // RiffHeader
            toBigEndian('RIFF'),
            (uint32_t)(fileSize - 8),
            toBigEndian('WAVE')
        },
        { // FormatChunk
            toBigEndian('fmt '),
            (uint32_t)(sizeof(FormatChunk) - 8),
            0x0001, // Microsoft PCM format
            (uint16_t)channels,
            (uint32_t)samplerate,
            (uint32_t)((int64_t)samplerate * blockAlign),
            (uint16_t)blockAlign,
            (uint16_t)bitswidth
        },
        { // DataChunk
            toBigEndian('data'),
            (uint32_t)dataSize
        }
    };

    if (fwrite(&header, headerSize, 1, fp) != 1) {
        throw IOException("Waveヘッダの書き込みに失敗");
    }
}

extern "C" int WriteWaveHeaderForTest(int channels, int samplerate, int bitswidth, int64_t numSamples,
    uint8_t* output, size_t outputCapacity, size_t* headerLength) {
    constexpr int RESULT_INVALID = -1;
    constexpr int RESULT_BUFFER_TOO_SMALL = -2;
    if (headerLength == nullptr) return RESULT_INVALID;
    *headerLength = 0;
    FILE* fp = tmpfile(); // 閉じると自動的に削除される
    if (fp == nullptr) return RESULT_INVALID;
    int result = RESULT_INVALID;
    try {
        writeWaveHeader(fp, channels, samplerate, bitswidth, numSamples);
        const long length = ftell(fp);
        if (length > 0 && fseek(fp, 0, SEEK_SET) == 0) {
            *headerLength = (size_t)length;
            if (output == nullptr || outputCapacity < (size_t)length) {
                result = RESULT_BUFFER_TOO_SMALL;
            } else if (fread(output, 1, (size_t)length, fp) == (size_t)length) {
                result = 0;
            }
        }
    } catch (...) {
        result = RESULT_INVALID;
    }
    fclose(fp);
    return result;
}
