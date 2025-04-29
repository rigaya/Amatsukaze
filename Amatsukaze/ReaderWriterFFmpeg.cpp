/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "ReaderWriterFFmpeg.h"

int av::GetFFmpegThreads(int preferred, int height) {
    const int maxThreads = (height > 1080) ? 16 : 8;
    return std::min(maxThreads, std::max(1, preferred));
}

AVStream* av::GetVideoStream(AVFormatContext* pCtx) {
    for (int i = 0; i < (int)pCtx->nb_streams; ++i) {
        if (pCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            return pCtx->streams[i];
        }
    }
    return nullptr;
}

AVStream* av::GetVideoStream(AVFormatContext* ctx, int serviceid) {
    for (int i = 0; i < (int)ctx->nb_programs; ++i) {
        if (ctx->programs[i]->program_num == serviceid) {
            auto prog = ctx->programs[i];
            for (int s = 0; s < (int)prog->nb_stream_indexes; ++s) {
                auto stream = ctx->streams[prog->stream_index[s]];
                if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    return stream;
                }
            }
        }
    }
    return nullptr;
}
av::Frame::Frame()
    : frame_() {
    frame_ = av_frame_alloc();
}
av::Frame::Frame(const Frame& src) {
    frame_ = av_frame_alloc();
    av_frame_ref(frame_, src());
}
av::Frame::~Frame() {
    av_frame_free(&frame_);
}
AVFrame* av::Frame::operator()() {
    return frame_;
}
const AVFrame* av::Frame::operator()() const {
    return frame_;
}
av::Frame& av::Frame::operator=(const Frame& src) {
    av_frame_unref(frame_);
    av_frame_ref(frame_, src());
    return *this;
}
av::CodecContext::CodecContext(const AVCodec* pCodec)
    : ctx_() {
    Set(pCodec);
}
av::CodecContext::CodecContext()
    : ctx_() {}
av::CodecContext::~CodecContext() {
    Free();
}
void av::CodecContext::Set(const AVCodec* pCodec) {
    if (pCodec == NULL) {
        THROW(RuntimeException, "pCodec is NULL");
    }
    Free();
    ctx_ = avcodec_alloc_context3(pCodec);
    if (ctx_ == NULL) {
        THROW(IOException, "failed avcodec_alloc_context3");
    }
}
void av::CodecContext::Free() {
    if (ctx_) {
        avcodec_free_context(&ctx_);
        ctx_ = NULL;
    }
}
AVCodecContext* av::CodecContext::operator()() {
    return ctx_;
}
av::InputContext::InputContext(const tstring& src)
    : ctx_() {
    if (avformat_open_input(&ctx_, tchar_to_string(src).c_str(), NULL, NULL) != 0) {
        THROW(IOException, "failed avformat_open_input");
    }
}
av::InputContext::~InputContext() {
    avformat_close_input(&ctx_);
}
AVFormatContext* av::InputContext::operator()() {
    return ctx_;
}
#if ENABLE_FFMPEG_FILTER
av::FilterGraph::FilterGraph()
    : ctx_() {}
av::FilterGraph::~FilterGraph() {
    Free();
}
void av::FilterGraph::Create() {
    Free();
    ctx_ = avfilter_graph_alloc();
    if (ctx_ == NULL) {
        THROW(IOException, "failed avfilter_graph_alloc");
    }
}
void av::FilterGraph::Free() {
    if (ctx_) {
        avfilter_graph_free(&ctx_);
        ctx_ = NULL;
    }
}
AVFilterGraph* av::FilterGraph::operator()() {
    return ctx_;
}
av::FilterInOut::FilterInOut()
    : ctx_(avfilter_inout_alloc()) {
    if (ctx_ == nullptr) {
        THROW(IOException, "failed avfilter_inout_alloc");
    }
}
av::FilterInOut::~FilterInOut() {
    avfilter_inout_free(&ctx_);
}
AVFilterInOut*& av::FilterInOut::operator()() {
    return ctx_;
}
#endif
av::WriteIOContext::WriteIOContext(int bufsize)
    : ctx_() {
    unsigned char* buffer = (unsigned char*)av_malloc(bufsize);
#if AMATSUKAZE2DLL
    void *func_ptr = (void *)write_packet_;
    ctx_ = avio_alloc_context(buffer, bufsize, 1, this, NULL, (int (*)(void *, uint8_t *, int))(write_packet_), NULL);
#else
    ctx_ = avio_alloc_context(buffer, bufsize, 1, this, NULL, write_packet_, NULL);
#endif
}
av::WriteIOContext::~WriteIOContext() {
    av_free(ctx_->buffer);
    av_free(ctx_);
}
AVIOContext* av::WriteIOContext::operator()() {
    return ctx_;
}
/* static */ int av::WriteIOContext::write_packet_(void *opaque, uint8_t *buf, int buf_size) {
    ((WriteIOContext*)opaque)->onWrite(MemoryChunk(buf, buf_size));
    return 0;
}
av::OutputContext::OutputContext(WriteIOContext& ioCtx, const char* format)
    : ctx_() {
    if (avformat_alloc_output_context2(&ctx_, NULL, format, "-") < 0) {
        THROW(FormatException, "avformat_alloc_output_context2 failed");
    }
    if (ctx_->pb != NULL) {
        THROW(FormatException, "pb already has ...");
    }
    ctx_->pb = ioCtx();
    // 10bit以上YUV4MPEG対応
    ctx_->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
}
av::OutputContext::~OutputContext() {
    avformat_free_context(ctx_);
}
AVFormatContext* av::OutputContext::operator()() {
    return ctx_;
}
av::VideoReader::VideoReader(AMTContext& ctx)
    : AMTObject(ctx)
    , fmt_()
    , fieldMode_() {}

void av::VideoReader::readAll(const tstring& src, const DecoderSetting& decoderSetting) {
    InputContext inputCtx(src);
    if (avformat_find_stream_info(inputCtx(), NULL) < 0) {
        THROW(FormatException, "avformat_find_stream_info failed");
    }
    onFileOpen(inputCtx());
    AVStream *videoStream = GetVideoStream(inputCtx());
    if (videoStream == NULL) {
        THROW(FormatException, "Could not find video stream ...");
    }
    AVCodecID vcodecId = videoStream->codecpar->codec_id;
    const AVCodec *pCodec = getHWAccelCodec(vcodecId, decoderSetting);
    if (pCodec == NULL) {
        ctx.warn("指定されたデコーダが使用できないためデフォルトデコーダを使います");
        pCodec = avcodec_find_decoder(vcodecId);
    }
    if (pCodec == NULL) {
        THROW(FormatException, "Could not find decoder ...");
    }
    CodecContext codecCtx(pCodec);
    if (avcodec_parameters_to_context(codecCtx(), videoStream->codecpar) != 0) {
        THROW(FormatException, "avcodec_parameters_to_context failed");
    }
    if (avcodec_open2(codecCtx(), pCodec, NULL) != 0) {
        THROW(FormatException, "avcodec_open2 failed");
    }

    bool first = true;
    Frame frame;
    AVPacket packet = AVPacket();
    while (av_read_frame(inputCtx(), &packet) == 0) {
        if (packet.stream_index == videoStream->index) {
            if (avcodec_send_packet(codecCtx(), &packet) != 0) {
                THROW(FormatException, "avcodec_send_packet failed");
            }
            while (avcodec_receive_frame(codecCtx(), frame()) == 0) {
                if (first) {
                    onFirstFrame(videoStream, frame());
                    first = false;
                }
                onFrame(frame);
            }
        } else {
            onAudioPacket(packet);
        }
        av_packet_unref(&packet);
    }

    // flush decoder
    if (avcodec_send_packet(codecCtx(), NULL) != 0) {
        THROW(FormatException, "avcodec_send_packet failed");
    }
    while (avcodec_receive_frame(codecCtx(), frame()) == 0) {
        onFrame(frame);
    }

}
/* virtual */ void av::VideoReader::onFileOpen(AVFormatContext *fmt) {}
/* virtual */ void av::VideoReader::onVideoFormat(AVStream *stream, VideoFormat fmt) {}
/* virtual */ void av::VideoReader::onFrameDecoded(Frame& frame) {}
/* virtual */ void av::VideoReader::onAudioPacket(AVPacket& packet) {}

const AVCodec* av::VideoReader::getHWAccelCodec(AVCodecID vcodecId, const DecoderSetting& decoderSetting) {
    switch (vcodecId) {
    case AV_CODEC_ID_MPEG2VIDEO:
        switch (decoderSetting.mpeg2) {
        case DECODER_QSV:
            return avcodec_find_decoder_by_name("mpeg2_qsv");
        case DECODER_CUVID:
            return avcodec_find_decoder_by_name("mpeg2_cuvid");
        default:
            break;
        }
        break;
    case AV_CODEC_ID_H264:
        switch (decoderSetting.h264) {
        case DECODER_QSV:
            return avcodec_find_decoder_by_name("h264_qsv");
        case DECODER_CUVID:
            return avcodec_find_decoder_by_name("h264_cuvid");
        default:
            break;
        }
        break;
    case AV_CODEC_ID_HEVC:
        switch (decoderSetting.hevc) {
        case DECODER_QSV:
            return avcodec_find_decoder_by_name("hevc_qsv");
        case DECODER_CUVID:
            return avcodec_find_decoder_by_name("hevc_cuvid");
        default:
            break;
        }
        break;
    default:
        break;
    }
    return avcodec_find_decoder(vcodecId);
}

void av::VideoReader::onFrame(Frame& frame) {
    if (fieldMode_) {
#ifdef AMATSUKAZE2DLL
        const bool interlaced = (frame()->flags & AV_FRAME_FLAG_INTERLACED) != 0;
        const bool tff = (frame()->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;
#else
        const bool interlaced = frame()->interlaced_frame != 0;
        const bool tff = frame()->top_field_first != 0;
#endif
        if (interlaced == false) {
            // フレームがインタレースでなかったらそのまま出力
            prevFrame_ = nullptr;
            onFrameDecoded(frame);
        } else if (prevFrame_ == nullptr) {
            // トップフィールドでなかったら破棄
            // 仕様かどうかは不明だけどFFMPEG ver.3.2.2現在
            // top_field_first=1: top field
            // top_field_first=0: bottom field
            // となっているようである
            if (tff) {
                prevFrame_ = std::unique_ptr<av::Frame>(new av::Frame(frame));
            } else {
                ctx.warn("トップフィールドを想定していたがそうではなかったのでフィールドを破棄");
            }
        } else {
            // 2枚のフィールドを合成
            auto merged = mergeFields(*prevFrame_, frame);
            onFrameDecoded(*merged);
            prevFrame_ = nullptr;
        }
    } else {
        onFrameDecoded(frame);
    }
}

void av::VideoReader::onFirstFrame(AVStream *stream, AVFrame *frame) {
    VIDEO_STREAM_FORMAT srcFormat = VS_UNKNOWN;
    switch (stream->codecpar->codec_id) {
    case AV_CODEC_ID_H264:
        srcFormat = VS_H264;
        break;
    case AV_CODEC_ID_HEVC:
        srcFormat = VS_H265;
        break;
    case AV_CODEC_ID_MPEG2VIDEO:
        srcFormat = VS_MPEG2;
        break;
    default:
        break;
    }

    fmt_.format = srcFormat;
#ifdef AMATSUKAZE2DLL
    fmt_.progressive = (frame->flags & AV_FRAME_FLAG_INTERLACED) == 0;
#else
    fmt_.progressive = !(frame->interlaced_frame);
#endif
    fmt_.width = frame->width;
    fmt_.height = frame->height;
    fmt_.sarWidth = frame->sample_aspect_ratio.num;
    fmt_.sarHeight = frame->sample_aspect_ratio.den;
    fmt_.colorPrimaries = frame->color_primaries;
    fmt_.transferCharacteristics = frame->color_trc;
    fmt_.colorSpace = frame->colorspace;
    // 今のところ固定フレームレートしか対応しない
    fmt_.fixedFrameRate = true;
    fmt_.frameRateNum = stream->r_frame_rate.num;
    fmt_.frameRateDenom = stream->r_frame_rate.den;

    // x265でインタレースの場合はfield mode
    fieldMode_ = (fmt_.format == VS_H265 && fmt_.progressive == false);

    if (fieldMode_) {
        fmt_.height *= 2;
        fmt_.frameRateNum /= 2;
    }

    onVideoFormat(stream, fmt_);
}

// 2つのフレームのトップフィールド、ボトムフィールドを合成
/* static */ std::unique_ptr<av::Frame> av::VideoReader::mergeFields(av::Frame& topframe, av::Frame& bottomframe) {
    auto dstframe = std::unique_ptr<av::Frame>(new av::Frame());

    AVFrame* top = topframe();
    AVFrame* bottom = bottomframe();
    AVFrame* dst = (*dstframe)();

    // フレームのプロパティをコピー
    av_frame_copy_props(dst, top);

    // メモリサイズに関する情報をコピー
    dst->format = top->format;
    dst->width = top->width;
    dst->height = top->height * 2;

    // メモリ確保
    if (av_frame_get_buffer(dst, 64) != 0) {
        THROW(RuntimeException, "failed to allocate frame buffer");
    }

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((AVPixelFormat)(dst->format));
    int pixel_shift = (desc->comp[0].depth > 8) ? 1 : 0;
    int nplanes = (dst->format != AV_PIX_FMT_NV12) ? 3 : 2;

    for (int i = 0; i < nplanes; ++i) {
        int hshift = (i > 0 && dst->format != AV_PIX_FMT_NV12) ? desc->log2_chroma_w : 0;
        int vshift = (i > 0) ? desc->log2_chroma_h : 0;
        int wbytes = (dst->width >> hshift) << pixel_shift;
        int height = dst->height >> vshift;

        for (int y = 0; y < height; y += 2) {
            uint8_t* dst0 = dst->data[i] + dst->linesize[i] * (y + 0);
            uint8_t* dst1 = dst->data[i] + dst->linesize[i] * (y + 1);
            uint8_t* src0 = top->data[i] + top->linesize[i] * (y >> 1);
            uint8_t* src1 = bottom->data[i] + bottom->linesize[i] * (y >> 1);
            memcpy(dst0, src0, wbytes);
            memcpy(dst1, src1, wbytes);
        }
    }

    return dstframe;
}
av::VideoWriter::VideoWriter(VideoFormat fmt, int bufsize)
    : ioCtx_(this, bufsize)
    , outputCtx_(ioCtx_, "yuv4mpegpipe")
    , codecCtx_(avcodec_find_encoder_by_name("wrapped_avframe"))
    , fmt_(fmt)
    , initialized_(false)
    , frameCount_(0) {}

void av::VideoWriter::inputFrame(Frame& frame) {

    // フォーマットチェック
    ASSERT(fmt_.width == frame()->width);
    ASSERT(fmt_.height == frame()->height);
    ASSERT(fmt_.sarWidth == frame()->sample_aspect_ratio.num);
    ASSERT(fmt_.sarHeight == frame()->sample_aspect_ratio.den);
    ASSERT(fmt_.colorPrimaries == frame()->color_primaries);
    ASSERT(fmt_.transferCharacteristics == frame()->color_trc);
    ASSERT(fmt_.colorSpace == frame()->colorspace);

    // PTS再定義
    frame()->pts = frameCount_++;

    init(frame);

    if (avcodec_send_frame(codecCtx_(), frame()) != 0) {
        THROW(FormatException, "avcodec_send_frame failed");
    }
    AVPacket packet = AVPacket();
    while (avcodec_receive_packet(codecCtx_(), &packet) == 0) {
        packet.stream_index = 0;
        av_interleaved_write_frame(outputCtx_(), &packet);
        av_packet_unref(&packet);
    }
}

void av::VideoWriter::flush() {
    if (initialized_) {
        // flush encoder
        if (avcodec_send_frame(codecCtx_(), NULL) != 0) {
            THROW(FormatException, "avcodec_send_frame failed");
        }
        AVPacket packet = AVPacket();
        while (avcodec_receive_packet(codecCtx_(), &packet) == 0) {
            packet.stream_index = 0;
            av_interleaved_write_frame(outputCtx_(), &packet);
            av_packet_unref(&packet);
        }
        // flush muxer
        av_interleaved_write_frame(outputCtx_(), NULL);
    }
}

int av::VideoWriter::getFrameCount() const {
    return frameCount_;
}

AVRational av::VideoWriter::getAvgFrameRate() const {
    return av_make_q(fmt_.frameRateNum, fmt_.frameRateDenom);
}
av::VideoWriter::TransWriteContext::TransWriteContext(VideoWriter* this_, int bufsize)
    : WriteIOContext(bufsize)
    , this_(this_) {}
/* virtual */ void av::VideoWriter::TransWriteContext::onWrite(MemoryChunk mc) {
    this_->onWrite(mc);
}

void av::VideoWriter::init(Frame& frame) {
    if (initialized_ == false) {
        AVStream* st = avformat_new_stream(outputCtx_(), NULL);
        if (st == NULL) {
            THROW(FormatException, "avformat_new_stream failed");
        }

        AVCodecContext* enc = codecCtx_();

        enc->pix_fmt = (AVPixelFormat)frame()->format;
        enc->width = frame()->width;
        enc->height = frame()->height;
        enc->field_order = fmt_.progressive ? AV_FIELD_PROGRESSIVE : AV_FIELD_TT;
        enc->color_range = frame()->color_range;
        enc->color_primaries = frame()->color_primaries;
        enc->color_trc = frame()->color_trc;
        enc->colorspace = frame()->colorspace;
        enc->chroma_sample_location = frame()->chroma_location;
        st->sample_aspect_ratio = enc->sample_aspect_ratio = frame()->sample_aspect_ratio;

        st->time_base = enc->time_base = av_make_q(fmt_.frameRateDenom, fmt_.frameRateNum);
        st->avg_frame_rate = av_make_q(fmt_.frameRateNum, fmt_.frameRateDenom);

        if (avcodec_open2(codecCtx_(), codecCtx_()->codec, NULL) != 0) {
            THROW(FormatException, "avcodec_open2 failed");
        }

        // muxerにエンコーダパラメータを渡す
        avcodec_parameters_from_context(st->codecpar, enc);

        // for debug
        av_dump_format(outputCtx_(), 0, "-", 1);

        if (avformat_write_header(outputCtx_(), NULL) < 0) {
            THROW(FormatException, "avformat_write_header failed");
        }
        initialized_ = true;
    }
}
av::AudioWriter::AudioWriter(AVStream* src, int bufsize)
    : ioCtx_(this, bufsize)
    , outputCtx_(ioCtx_, "adts")
    , frameCount_(0) {
    AVStream* st = avformat_new_stream(outputCtx_(), NULL);
    if (st == NULL) {
        THROW(FormatException, "avformat_new_stream failed");
    }

    // コーデックパラメータをコピー
    avcodec_parameters_copy(st->codecpar, src->codecpar);

    // for debug
    av_dump_format(outputCtx_(), 0, "-", 1);

    if (avformat_write_header(outputCtx_(), NULL) < 0) {
        THROW(FormatException, "avformat_write_header failed");
    }
}

void av::AudioWriter::inputFrame(AVPacket& frame) {
    // av_interleaved_write_frameにpacketのownershipを渡すので
    AVPacket outpacket = AVPacket();
    av_packet_ref(&outpacket, &frame);
    outpacket.stream_index = 0;
    outpacket.pos = -1;
    if (av_interleaved_write_frame(outputCtx_(), &outpacket) < 0) {
        THROW(FormatException, "av_interleaved_write_frame failed");
    }
}

void av::AudioWriter::flush() {
    // flush muxer
    if (av_interleaved_write_frame(outputCtx_(), NULL) < 0) {
        THROW(FormatException, "av_interleaved_write_frame failed");
    }
}
av::AudioWriter::TransWriteContext::TransWriteContext(AudioWriter* this_, int bufsize)
    : WriteIOContext(bufsize)
    , this_(this_) {}
/* virtual */ void av::AudioWriter::TransWriteContext::onWrite(MemoryChunk mc) {
    this_->onWrite(mc);
}
void av::Y4MParser::clear() {
    y4mcur = 0;
    frameSize = 0;
    frameCount = 0;
    y4mbuf.clear();
}
void av::Y4MParser::inputData(MemoryChunk mc) {
    y4mbuf.add(mc);
    while (true) {
        if (y4mcur == 0) {
            // ストリームヘッダ
            auto data = y4mbuf.get();
            uint8_t* end = (uint8_t*)memchr(data.data, 0x0a, data.length);
            if (end == nullptr) {
                break;
            }
            *end = 0; // terminate
            char* next_token = nullptr;
            char* token = strtok_s((char*)data.data, " ", &next_token);
            // token == "YUV4MPEG2"
            int width = 0, height = 0, bp4p = 0;
            while (true) {
                token = strtok_s(nullptr, " ", &next_token);
                if (token == nullptr) {
                    break;
                }
                switch (*(token++)) {
                case 'W':
                    width = atoi(token);
                    break;
                case 'H':
                    height = atoi(token);
                    break;
                case 'C':
                    if (strcmp(token, "420jpeg") == 0 ||
                        strcmp(token, "420mpeg2") == 0 ||
                        strcmp(token, "420paldv") == 0) {
                        bp4p = 6;
                    } else if (strcmp(token, "mono") == 0) {
                        bp4p = 4;
                    } else if (strcmp(token, "mono16") == 0) {
                        bp4p = 8;
                    } else if (strcmp(token, "411") == 0) {
                        bp4p = 6;
                    } else if (strcmp(token, "422") == 0) {
                        bp4p = 8;
                    } else if (strcmp(token, "444") == 0) {
                        bp4p = 12;
                    } else if (strcmp(token, "420p9") == 0 ||
                        strcmp(token, "420p10") == 0 ||
                        strcmp(token, "420p12") == 0 ||
                        strcmp(token, "420p14") == 0 ||
                        strcmp(token, "420p16") == 0) {
                        bp4p = 12;
                    } else if (strcmp(token, "422p9") == 0 ||
                        strcmp(token, "422p10") == 0 ||
                        strcmp(token, "422p12") == 0 ||
                        strcmp(token, "422p14") == 0 ||
                        strcmp(token, "422p16") == 0) {
                        bp4p = 16;
                    } else if (strcmp(token, "444p9") == 0 ||
                        strcmp(token, "444p10") == 0 ||
                        strcmp(token, "444p12") == 0 ||
                        strcmp(token, "444p14") == 0 ||
                        strcmp(token, "444p16") == 0) {
                        bp4p = 24;
                    } else {
                        THROWF(FormatException, "[y4m] Unknown pixel format: %s", token);
                    }
                    break;
                }
            }
            if (width == 0 || height == 0 || bp4p == 0) {
                THROW(FormatException, "[y4m] missing stream information");
            }
            frameSize = (width * height * bp4p) >> 2;
            y4mbuf.trimHead(end - data.data + 1);
            y4mcur = 1; // 次はフレームヘッダ
        }
        if (y4mcur == 1) {
            // フレームヘッダ
            auto data = y4mbuf.get();
            uint8_t* end = (uint8_t*)memchr(data.data, 0x0a, data.length);
            if (end == nullptr) {
                break;
            }
            y4mbuf.trimHead(end - data.data + 1);
            y4mcur = 2; // 次はフレームデータ
        }
        if (y4mcur == 2) {
            // フレームデータ
            if (y4mbuf.size() < frameSize) {
                break;
            }
            y4mbuf.trimHead(frameSize);
            frameCount++;
            y4mcur = 1; // 次はフレームヘッダ
        }
    }
}
int av::Y4MParser::getFrameCount() const {
    return frameCount;
}
av::EncodeWriter::EncodeWriter(AMTContext& ctx)
    : AMTObject(ctx)
    , videoWriter_(NULL)
    , process_(NULL)
    , error_(false) {}
av::EncodeWriter::~EncodeWriter() {
    if (process_ != NULL && process_->isRunning()) {
        THROW(InvalidOperationException, "call finish before destroy object ...");
    }

    delete videoWriter_;
    delete process_;
}

void av::EncodeWriter::start(const tstring& encoder_args, VideoFormat fmt, bool fieldMode, int bufsize) {
    if (videoWriter_ != NULL) {
        THROW(InvalidOperationException, "start method called multiple times");
    }
    fieldMode_ = fieldMode;
    if (fieldMode) {
        // フィールドモードのときは解像度は縦1/2でFPSは2倍
        fmt.height /= 2;
        fmt.frameRateNum *= 2;
    }
    y4mparser.clear();
    videoWriter_ = new MyVideoWriter(this, fmt, bufsize);
    process_ = new StdRedirectedSubProcess(encoder_args, 5);
}

void av::EncodeWriter::inputFrame(Frame& frame) {
    if (videoWriter_ == NULL) {
        THROW(InvalidOperationException, "you need to call start method before input frame");
    }
    if (error_) {
        THROW(RuntimeException, "failed to input frame due to encoder error ...");
    }
    if (fieldMode_) {
        // フィールドモードのときはtop,bottomの2つに分けて出力
        av::Frame top = av::Frame();
        av::Frame bottom = av::Frame();
        splitFrameToFields(frame, top, bottom);
        videoWriter_->inputFrame(top);
        videoWriter_->inputFrame(bottom);
    } else {
        videoWriter_->inputFrame(frame);
    }
}

void av::EncodeWriter::finish() {
    if (videoWriter_ != NULL) {
        videoWriter_->flush();
        process_->finishWrite();
        int ret = process_->join();
        if (ret != 0) {
            ctx.error("↓↓↓↓↓↓エンコーダ最後の出力↓↓↓↓↓↓");
            for (auto v : process_->getLastLines()) {
                v.push_back(0); // null terminate
                ctx.errorF("%s", v.data());
            }
            ctx.error("↑↑↑↑↑↑エンコーダ最後の出力↑↑↑↑↑↑");
            THROWF(RuntimeException, "エンコーダ終了コード: 0x%x", ret);
        }
        int inFrame = getFrameCount();
        int outFrame = y4mparser.getFrameCount();
        if (inFrame != outFrame) {
            THROWF(RuntimeException, "フレーム数が合いません(%d vs %d)", inFrame, outFrame);
        }
    }
}

int av::EncodeWriter::getFrameCount() const {
    return videoWriter_->getFrameCount();
}

AVRational av::EncodeWriter::getFrameRate() const {
    return videoWriter_->getAvgFrameRate();
}

const std::deque<std::vector<char>>& av::EncodeWriter::getLastLines() {
    return process_->getLastLines();
}
av::EncodeWriter::MyVideoWriter::MyVideoWriter(EncodeWriter* this_, VideoFormat fmt, int bufsize)
    : VideoWriter(fmt, bufsize)
    , this_(this_) {}
/* virtual */ void av::EncodeWriter::MyVideoWriter::onWrite(MemoryChunk mc) {
    this_->onVideoWrite(mc);
}

void av::EncodeWriter::onVideoWrite(MemoryChunk mc) {
    y4mparser.inputData(mc);
    try {
        process_->write(mc);
    } catch (Exception&) {
        error_ = true;
    }
}

VideoFormat av::EncodeWriter::getEncoderInputVideoFormat(VideoFormat format) {
    if (fieldMode_) {
        // フィールドモードのときは解像度は縦1/2でFPSは2倍
        format.height /= 2;
        format.frameRateNum *= 2;
    }
    return format;
}

// 1つのフレームをトップフィールド、ボトムフィールドの2つのフレームに分解
/* static */ void av::EncodeWriter::splitFrameToFields(av::Frame& frame, av::Frame& topfield, av::Frame& bottomfield) {
    AVFrame* src = frame();
    AVFrame* top = topfield();
    AVFrame* bottom = bottomfield();

    // フレームのプロパティをコピー
    av_frame_copy_props(top, src);
    av_frame_copy_props(bottom, src);

    // メモリサイズに関する情報をコピー
    top->format = bottom->format = src->format;
    top->width = bottom->width = src->width;
    top->height = bottom->height = src->height / 2;

    // メモリ確保
    if (av_frame_get_buffer(top, 64) != 0) {
        THROW(RuntimeException, "failed to allocate frame buffer");
    }
    if (av_frame_get_buffer(bottom, 64) != 0) {
        THROW(RuntimeException, "failed to allocate frame buffer");
    }

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((AVPixelFormat)(src->format));
    int pixel_shift = (desc->comp[0].depth > 8) ? 1 : 0;
    int nplanes = (src->format != AV_PIX_FMT_NV12) ? 3 : 2;

    for (int i = 0; i < nplanes; ++i) {
        int hshift = (i > 0 && src->format != AV_PIX_FMT_NV12) ? desc->log2_chroma_w : 0;
        int vshift = (i > 0) ? desc->log2_chroma_h : 0;
        int wbytes = (src->width >> hshift) << pixel_shift;
        int height = src->height >> vshift;

        for (int y = 0; y < height; y += 2) {
            uint8_t* src0 = src->data[i] + src->linesize[i] * (y + 0);
            uint8_t* src1 = src->data[i] + src->linesize[i] * (y + 1);
            uint8_t* dst0 = top->data[i] + top->linesize[i] * (y >> 1);
            uint8_t* dst1 = bottom->data[i] + bottom->linesize[i] * (y >> 1);
            memcpy(dst0, src0, wbytes);
            memcpy(dst1, src1, wbytes);
        }
    }
}
