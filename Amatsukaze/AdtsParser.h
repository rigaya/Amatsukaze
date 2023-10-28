#pragma once

/**
* ADTS AAC parser
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <stdint.h>

#include <vector>
#include <map>

#include "StreamUtils.h"

enum AAC_SYNTAX_ELEMENTS {
    ID_SCE = 0x0,
    ID_CPE = 0x1,
    ID_CCE = 0x2,
    ID_LFE = 0x3,
    ID_DSE = 0x4,
    ID_PCE = 0x5,
    ID_FIL = 0x6,
    ID_END = 0x7,
};

#if 1
struct AdtsHeader {

    bool parse(uint8_t *data, int length);

    bool check();

    uint8_t protection_absent;
    uint8_t profile;
    uint8_t sampling_frequency_index;
    uint8_t channel_configuration;
    uint16_t frame_length;
    uint8_t number_of_raw_data_blocks_in_frame;

    int numBytesRead;

    void getSamplingRate(int& rate);
};
#endif

struct NeAACDecFrameInfo;
typedef void *NeAACDecHandle;

class AdtsParser : public AMTObject {
public:
    AdtsParser(AMTContext&ctx);
    ~AdtsParser();

    virtual void reset();

    virtual bool inputFrame(MemoryChunk frame__, std::vector<AudioFrameData>& info, int64_t PTS);

private:
    NeAACDecHandle hAacDec;
    AdtsHeader header;
    std::map<int64_t, AUDIO_CHANNELS> channelsMap;

    // パケット間での情報保持
    AutoBuffer codedBuffer;
    int bytesConsumed_;
    int64_t lastPTS_;

    AutoBuffer decodedBuffer;
    bool syncOK;

    void closeDecoder();

    bool resetDecoder(MemoryChunk data);

    AUDIO_CHANNELS getAudioChannels(const AdtsHeader& header, const NeAACDecFrameInfo *frameInfo);

    int64_t channelCanonical(int numElem, const uint8_t* elems);

    void createChannelsMap();
};

// デュアルモノAACを2つのAACに無劣化分離する
class DualMonoSplitter : AMTObject {
public:
    DualMonoSplitter(AMTContext& ctx);

    ~DualMonoSplitter();

    void inputPacket(MemoryChunk frame);

    virtual void OnOutFrame(int index, MemoryChunk mc) = 0;

private:
    NeAACDecHandle hAacDec;
    AutoBuffer buf;

    void closeDecoder();

    bool resetDecoder(MemoryChunk data);
};

