#pragma once

/**
* MPEG2 Video parser
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "StreamUtils.h"

#define SEQ_HEADER_START_CODE 0x000001B3
#define PICTURE_START_CODE 0x00000100
#define EXTENSION_START_CODE 0x000001B5

static bool mpeg2_next_start_code(BitReader& reader);

struct MPEG2SequenceHeader {

    bool parse(uint8_t *data, int length);
    bool check();

    // Sequence header
    uint16_t horizontal_size_value;
    uint16_t vertical_size_value;
    uint8_t aspect_ratio_info;
    uint8_t frame_rate_code;
    uint32_t bit_rate_value;
    uint16_t vbv_buffer_size_value;
    uint16_t constrained_parameters_flag;

    // Sequence extension
    uint8_t profile_and_level_indication;
    uint8_t progressive_sequence;
    uint8_t chroma_format;
    uint8_t horizontal_size_extension;
    uint8_t vertical_size_extension;
    uint16_t bit_rate_extension;
    uint8_t vbv_buffer_size_extension;
    uint8_t low_delay;
    uint8_t frame_rate_extension_n;
    uint8_t frame_rate_extension_d;

    // Sequence Display Extension
    uint8_t video_format;
    uint8_t colour_description;
    uint8_t colour_primaries;
    uint8_t transfer_characteristics;
    uint8_t matrix_coefficients;
    uint16_t display_horizontal_size;
    uint16_t display_vertical_size;

    bool has_sequence_display_extension;

    int numReadBytes;

    int width();
    int height();
    int displayWidth();
    int displayHeight();
    std::pair<int, int> frame_rate();
    void getSAR(int& sar_w, int& sar_h);

private:
    void getDAR(int& width, int& height);

    static std::pair<int, int> frame_rate_value(int code);

    int gcd(int a, int b);

    // a > b
    int gcd_(int a, int b);
};

struct MPEG2PictureHeader {

    bool parse(uint8_t *data, int length);
    bool check();

    // Picture header
    uint16_t temporal_reference;
    uint8_t picture_coding_type;
    uint16_t vbv_delay;

    // Picture conding extension
    uint8_t intra_dc_precesion;
    uint8_t picture_structure;
    uint8_t top_field_first;
    uint8_t frame_pred_frame_dct;
    uint8_t concealment_motion_vectors;
    uint8_t q_scale_type;
    uint8_t intra_vlc_format;
    uint8_t alternate_scan;
    uint8_t repeat_first_field;
    uint8_t chroma_420_type;
    uint8_t progressive_frame;
    uint8_t composite_display_flag;

    int numReadBytes;
};

class MPEG2VideoParser : public IVideoParser, public AMTObject {
public:
    MPEG2VideoParser(AMTContext& ctx);

    virtual void reset();

    virtual bool inputFrame(MemoryChunk frame, std::vector<VideoFrameInfo>& info, int64_t PTS, int64_t DTS);

private:
    bool hasSequenceHeader;
    MPEG2SequenceHeader sequenceHeader;
    MPEG2PictureHeader pictureHeader[2];
    VideoFormat format;
};

