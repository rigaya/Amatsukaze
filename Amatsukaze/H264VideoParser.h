#pragma once

/**
* H.264/AVC Elementary Stream parser
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "StreamUtils.h"

struct H264HRDBitRate {
    uint32_t bit_rate_vlaue_minus1;
    uint32_t cpb_size_value_minus1;
    uint8_t cbr_flag;

    void read(BitReader& reader);
};

struct H264HRDParameters {

    void read(BitReader& reader);

    uint32_t cpb_cnt_minus1;
    uint8_t bit_rate_scale;
    uint8_t cpb_size_scale;

    std::vector<H264HRDBitRate> bit_rate_value_minus1;

    uint8_t initial_cpb_removal_delay_length_minus1;
    uint8_t cpb_removal_delay_length_minus1;
    uint8_t dpb_output_delay_length_minus1;
    uint8_t time_offset_length;
};

struct H264InitialCpbRemoval {
    uint32_t initial_cpb_removal_delay;
    uint32_t initial_cpb_removal_delay_offset;
};

struct H264SequenceParameterSet {

    bool parse(uint8_t *data, int length);
    bool check();

    uint8_t profile_idc;
    uint8_t constraint_set_flag[6];
    uint8_t level_idc;
    uint32_t seq_parameter_set_id;

    uint8_t chroma_format_idc; // 1(4:2:0) or 2(4:2:2)
    uint8_t separate_colour_plane_flag;
    uint8_t bit_depth_luma_minus8; // 0(8bit) or 2(10bit)
    uint8_t bit_depth_chroma_minus8; // 0(8bit) or 2(10bit)
    uint8_t qpprime_y_zero_transform_bypass_flag;
    uint8_t seq_scaling_matrix_present_flag;

    uint32_t log2_max_frame_num_minus4;
    uint32_t pic_order_cnt_type;

    uint32_t max_num_ref_frames;
    uint8_t gaps_in_frame_num_value_allowed_flag;
    uint32_t pic_width_in_mbs_minus1;
    uint32_t pic_height_in_map_units_minus1;
    uint8_t frame_mbs_only_flag;

    uint8_t mb_adaptive_frame_field_flag;
    uint8_t direct_8x8_inference_flag;
    uint8_t frame_cropping_flag;

    RECT frame_crop;

    uint8_t vui_parameters_present_flag; // 1固定

    // vui_parameters
    uint8_t aspect_ratio_info_present_flag; // 1固定
    uint8_t aspect_ratio_idc;
    uint16_t sar_width; // 4固定
    uint16_t sar_height; // 3固定

    uint8_t overscan_info_present_flag;
    uint8_t overscan_appropriate_flag;

    uint8_t video_signal_type_present_flag;
    uint8_t video_format;
    uint8_t video_full_range_flag; // 0固定
    uint8_t colour_description_present_flag;
    uint8_t colour_primaries; // 1固定
    uint8_t transfer_characteristics; // 1 or 11
    uint8_t matrix_coefficients; // 1固定

    uint8_t chroma_loc_info_present_flag; // 0固定
    uint32_t chroma_sample_loc_type_top_field;
    uint32_t chroma_sample_loc_type_bottom_field;

    uint8_t timing_info_present_flag; // 1固定
    uint32_t num_units_in_tick; // 1001固定
    uint32_t time_scale; // 60000(29.97) or 120000(59.94)
    uint8_t fixed_frame_rate_flag; // 1固定

    uint8_t nal_hrd_parameters_present_flag;
    H264HRDParameters nal_hrd_parameters;

    uint8_t vcl_hrd_parameters_present_flag;
    H264HRDParameters vcl_hrd_parameters;

    uint8_t low_delay_hrd_flag;

    uint8_t pic_struct_present_flag; // 1固定

    uint8_t bitstream_restriction_flag;
    uint8_t motion_vectors_over_pic_boundaries_flag;
    uint32_t max_bytes_per_pic_denom;
    uint32_t max_bits_per_mb_denom;
    uint32_t log2_max_mv_length_horizontal;
    uint32_t log2_max_mv_length_vertical;
    uint32_t num_reorder_frames;
    uint32_t max_dec_frame_buffering;

    int numReadBytes;

    void getPicutureSize(int& width, int& height);

    void getSAR(int& width, int& height);

    void getFramteRate(int& frameRateNum, int& frameRateDenom, bool& cfr);

    void getColorDesc(uint8_t& colorPrimaries, uint8_t& transferCharacteristics, uint8_t& colorSpace);

    double getClockTick();
private:
    enum {
        Extended_SAR = 255,
    };

    bool isHighProfile();

    void getSubC(int& width, int& height);

    int getChromaArrayType();

    void getSARFromIdc(int idc, int& w, int& h);

    void scailng_list(BitReader& reader, int sizeOfScalingList);

    void vui_parameters(BitReader& reader);
};

struct H264PicParameterSet {

    bool parse(uint8_t *data, int length);
    bool check();

    int numReadBytes;
};

class H264SuplementaryEnhancementInformation {
public:

    void updateSPS(H264SequenceParameterSet* sps);

    bool parse(uint8_t *data, int length);

    uint8_t nal_hrd_parameters_present_flag;
    uint8_t vcl_hrd_parameters_present_flag;
    uint8_t pic_struct_present_flag;

    int cpb_cnt_minus1;
    int initial_cpb_removal_delay_length_minus1;
    int cpb_removal_delay_length_minus1;
    int dpb_output_delay_length_minus1;

    bool has_buffering_period;
    bool has_pic_timing;
    bool has_pan_scan_rect;
    bool has_recovery_point;

    std::vector<H264InitialCpbRemoval> nal_hrd;

    // pic_timing
    uint32_t cpb_removal_delay;
    uint32_t dpb_output_delay;
    uint8_t pic_struct;

    // pan_scan_rect
    uint8_t pan_scan_rect_cancel_flag;
    uint32_t pan_scan_cnt_minus1;
    std::vector<RECT> pan_scan_rect_offset;
    uint32_t pan_scan_rect_repetition_period;

    // recovery_point
    uint32_t recovery_frame_cnt;
    uint8_t exact_match_flag;
    uint8_t broken_link_flag;
    uint8_t changing_slice_group_idc;

    int numReadBytes;

    int readPayloadTypeSize(BitReader& reader);

    void buffering_period(BitReader& reader_orig, int payloadSize);

    void pic_timing(BitReader& reader_orig, int payloadSize);

    void pan_scan_rect(BitReader& reader_orig, int payloadSize);

    void recovery_point(BitReader& reader_orig, int payloadSize);
};


class H264VideoParser : public AMTObject, public IVideoParser {
    struct NalUnit {
        int offset;
        int length; // rbsp_trailing_bitsを除いた長さ
    };
public:

    H264VideoParser(AMTContext& ctx);

    virtual void reset();

    virtual bool inputFrame(MemoryChunk frame, std::vector<VideoFrameInfo>& info, int64_t PTS, int64_t DTS);

private:
    AutoBuffer buffer;
    std::vector<NalUnit> nalUnits;

    // 直前の beffering period の DTS;
    int64_t beffering_period_DTS;

    H264SequenceParameterSet sps;
    H264PicParameterSet pps;
    H264SuplementaryEnhancementInformation sei;
    VideoFormat format;

    void frameType(uint8_t primary_pic_type, FRAME_TYPE& type);

    void pushNalUnit(int unitStart, int lastNonZero);

    void storeBuffer(MemoryChunk frame);
};

