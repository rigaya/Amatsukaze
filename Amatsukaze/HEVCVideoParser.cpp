// -----------------------------------------------------------------------------------------
// Amatsukaze改造版 by rigaya (original by nekopanda)
// -----------------------------------------------------------------------------------------
// The MIT License
//
// Copyright (c) 2024 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// --------------------------------------------------------------------------------------------


#include "HEVCVideoParser.h"

HEVCVideoParser::HEVCVideoParser(AMTContext& ctx) :
    AMTObject(ctx),
    IVideoParser(),
    m_parserCtx(nullptr, av_parser_close),
    m_codecCtxParser(nullptr, RGYAVDeleter<AVCodecContext>(avcodec_free_context)),
    m_parse_nal_unit_hevc(get_parse_nal_unit_hevc_func()),
    m_posBytes(0) {

}

void HEVCVideoParser::reset() {
    m_parserCtx.reset();
    m_codecCtxParser.reset();
    m_posBytes = 0;
}

void HEVCVideoParser::initParser() {
    m_parserCtx.reset(av_parser_init(AV_CODEC_ID_HEVC));
    if (!m_parserCtx) {
        THROW(FormatException, "Failed to initialize HEVC parser");
    }
    m_parserCtx->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    m_codecCtxParser.reset(avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_HEVC)));
    if (!m_codecCtxParser) {
        THROW(FormatException, "Failed to allocate HEVC parser context");
    }
    unique_ptr_custom<AVCodecParameters> codecParam(avcodec_parameters_alloc(), [](AVCodecParameters *pCodecPar) {
        avcodec_parameters_free(&pCodecPar);
        });
    avcodec_parameters_from_context(codecParam.get(), m_codecCtxParser.get());

    codecParam->format = AV_PIX_FMT_YUV420P10LE;
    codecParam->width = 3840;
    codecParam->height = 2160;
    codecParam->color_range = AVCOL_RANGE_MPEG;
    codecParam->bits_per_coded_sample = 10;
    codecParam->chroma_location = AVCHROMA_LOC_LEFT;
    codecParam->color_space = AVCOL_SPC_BT2020_NCL;
    codecParam->color_primaries = AVCOL_PRI_BT2020;
    codecParam->color_trc = AVCOL_TRC_ARIB_STD_B67;
    codecParam->field_order = AV_FIELD_PROGRESSIVE;
    codecParam->profile = FF_PROFILE_HEVC_MAIN_10;

    avcodec_parameters_to_context(m_codecCtxParser.get(), codecParam.get());

    m_codecCtxParser->time_base = av_make_q(1, 90000);
    m_codecCtxParser->pkt_timebase = m_codecCtxParser->time_base;
    m_codecCtxParser->framerate = av_make_q(60000, 1001);
}

bool HEVCVideoParser::inputFrame(MemoryChunk frame, std::vector<VideoFrameInfo>& info, int64_t PTS, int64_t DTS) {
    info.clear();
    if (!m_parserCtx) {
        initParser();
    }
    auto nal_units = m_parse_nal_unit_hevc(frame.data, frame.length);
    //nal_unitのtypeをチェックしてNALU_HEVC_VPS,NALU_HEVC_SPS,NALU_HEVC_PPSの3つがそろっているか確認する
    auto nal_vps = std::find_if(nal_units.begin(), nal_units.end(), [](const auto& n) { return n.type == NALU_HEVC_VPS; });
    auto nal_sps = std::find_if(nal_units.begin(), nal_units.end(), [](const auto& n) { return n.type == NALU_HEVC_SPS; });
    auto nal_pps = std::find_if(nal_units.begin(), nal_units.end(), [](const auto& n) { return n.type == NALU_HEVC_PPS; });

    if (nal_vps != nal_units.end() && nal_sps != nal_units.end() && nal_pps != nal_units.end()) {
        std::vector<uint8_t> vps_sps_pps;
        vps_sps_pps.insert(vps_sps_pps.end(), nal_vps->ptr, nal_vps->ptr + nal_vps->size);
        vps_sps_pps.insert(vps_sps_pps.end(), nal_sps->ptr, nal_sps->ptr + nal_sps->size);
        vps_sps_pps.insert(vps_sps_pps.end(), nal_pps->ptr, nal_pps->ptr + nal_pps->size);
        if (m_codecCtxParser->extradata) {
            av_free(m_codecCtxParser->extradata);
        }
        m_codecCtxParser->extradata_size = (int)vps_sps_pps.size();
        m_codecCtxParser->extradata = (uint8_t*)av_malloc(m_codecCtxParser->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!m_codecCtxParser->extradata) {
            THROW(FormatException, "Failed to allocate extradata");
        }
        memcpy(m_codecCtxParser->extradata, vps_sps_pps.data(), vps_sps_pps.size());
    }
    if (!m_codecCtxParser->extradata) {
        return true;
    }

    int data_offset = 0;
    while (frame.length - data_offset > 0) {
        uint8_t* dummy = nullptr;
        int dummy_size = 0;
        const auto len = av_parser_parse2(m_parserCtx.get(), m_codecCtxParser.get(), &dummy, &dummy_size, frame.data + data_offset, (int)(frame.length - data_offset), PTS, DTS, m_posBytes);
        if (len == 0) {
            break;
        }
        data_offset += len;
        m_posBytes += len;

        VideoFrameInfo vfinfo;
        vfinfo.codedDataSize = len;
        vfinfo.DTS = m_parserCtx->dts != AV_NOPTS_VALUE ? m_parserCtx->dts : DTS;
        vfinfo.PTS = m_parserCtx->pts != AV_NOPTS_VALUE ? m_parserCtx->pts : PTS;
        vfinfo.isGopStart = m_parserCtx->pict_type == AV_PICTURE_TYPE_I;
        vfinfo.format.format = VS_H265;
        vfinfo.format.width = m_codecCtxParser->width;
        vfinfo.format.height = m_codecCtxParser->height;
        vfinfo.format.displayWidth = m_codecCtxParser->coded_width ? m_codecCtxParser->coded_width : vfinfo.format.width;
        vfinfo.format.displayHeight = m_codecCtxParser->coded_height ? m_codecCtxParser->coded_height : vfinfo.format.height;
        if (m_codecCtxParser->sample_aspect_ratio.num > 0 && m_codecCtxParser->sample_aspect_ratio.den > 0) {
            vfinfo.format.sarWidth = m_codecCtxParser->sample_aspect_ratio.num;
            vfinfo.format.sarHeight = m_codecCtxParser->sample_aspect_ratio.den;
        } else {
            vfinfo.format.sarWidth = 1;
            vfinfo.format.sarHeight = 1;
        }
        if (m_codecCtxParser->framerate.num > 0 && m_codecCtxParser->framerate.den > 0) {
            vfinfo.format.frameRateNum = m_codecCtxParser->framerate.num;
            vfinfo.format.frameRateDenom = m_codecCtxParser->framerate.den;
        } else {
            vfinfo.format.frameRateNum = 60000;
            vfinfo.format.frameRateDenom = 1001;
        }
        vfinfo.format.fixedFrameRate = 1;
        vfinfo.format.colorSpace = m_codecCtxParser->colorspace;
        vfinfo.format.colorPrimaries = m_codecCtxParser->color_primaries;
        vfinfo.format.transferCharacteristics = m_codecCtxParser->color_trc;
        vfinfo.format.progressive = m_parserCtx->picture_structure == AV_PICTURE_STRUCTURE_FRAME || m_parserCtx->picture_structure == AV_PICTURE_STRUCTURE_UNKNOWN;
        vfinfo.progressive = vfinfo.format.progressive;
        switch (m_parserCtx->pict_type) {
        case AV_PICTURE_TYPE_I: vfinfo.type = FRAME_I; break;
        case AV_PICTURE_TYPE_P: vfinfo.type = FRAME_P; break;
        case AV_PICTURE_TYPE_B: vfinfo.type = FRAME_B; break;
        default: vfinfo.type = FRAME_OTHER; break;
        }
        switch (m_parserCtx->picture_structure) {
        case AV_PICTURE_STRUCTURE_UNKNOWN:      vfinfo.pic = PIC_FRAME; break;
        case AV_PICTURE_STRUCTURE_TOP_FIELD:    vfinfo.pic = PIC_TFF; break;
        case AV_PICTURE_STRUCTURE_BOTTOM_FIELD: vfinfo.pic = PIC_BFF; break;
        default:
            switch (m_parserCtx->field_order) {
            case AV_FIELD_TT:
            case AV_FIELD_TB: vfinfo.pic = PIC_TFF; break;
            case AV_FIELD_BT:
            case AV_FIELD_BB: vfinfo.pic = PIC_BFF; break;
            default:          vfinfo.pic = PIC_FRAME; break;
            }
        }
        info.push_back(vfinfo);
    }

    return info.size() > 0;
}
