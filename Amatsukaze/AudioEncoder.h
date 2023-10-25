#pragma once

#pragma once

#include <stdint.h>

#include "ProcessThread.h"
#include "StreamReform.h"

namespace wave {

struct Header {
    int8_t chunkID[4]; //"RIFF" = 0x46464952
    uint32_t chunkSize; //28 [+ sizeof(wExtraFormatBytes) + wExtraFormatBytes] + sum(sizeof(chunk.id) + sizeof(chunk.size) + chunk.size)
    int8_t format[4]; //"WAVE" = 0x45564157
    int8_t subchunk1ID[4]; //"fmt " = 0x20746D66
    uint32_t subchunk1Size; //16 [+ sizeof(wExtraFormatBytes) + wExtraFormatBytes]
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    int8_t subchunk2ID[4];
    uint32_t subchunk2Size;
};

void set4(int8_t dst[4], const char* src);

} // namespace wave {

void EncodeAudio(AMTContext& ctx, const tstring& encoder_args,
    const tstring& audiopath, const AudioFormat& afmt,
    const std::vector<FilterAudioFrame>& audioFrames);

