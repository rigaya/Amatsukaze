/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "AMTSource.h"
#include <string>
#include <cstdint>

namespace av {


const AVCodec* AMTSource::getHWAccelCodec(AVCodecID vcodecId) {
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

void AMTSource::MakeCodecContext(IScriptEnvironment* env) {
    AVCodecID vcodecId = videoStream->codecpar->codec_id;
    const AVCodec *pCodec = getHWAccelCodec(vcodecId);
    if (pCodec == NULL) {
        ctx.warn("指定されたデコーダが使用できないためデフォルトデコーダを使います");
        pCodec = avcodec_find_decoder(vcodecId);
    }
    if (pCodec == NULL) {
        env->ThrowError("Could not find decoder ...");
    }
    codecCtx.Set(pCodec);
    if (avcodec_parameters_to_context(codecCtx(), videoStream->codecpar) != 0) {
        env->ThrowError("avcodec_parameters_to_context failed");
    }
    codecCtx()->pkt_timebase = videoStream->time_base;
    codecCtx()->thread_count = GetFFmpegThreads((decodeThreads) ? decodeThreads : GetProcessorCount(), videoStream->codecpar->height);

    // export_mvs for codecview
    //AVDictionary *opts = NULL;
    //av_dict_set(&opts, "flags2", "+export_mvs", 0);

    if (avcodec_open2(codecCtx(), pCodec, NULL) != 0) {
        env->ThrowError("avcodec_open2 failed");
    }
}

#if ENABLE_FFMPEG_FILTER
void AMTSource::MakeFilterGraph(IScriptEnvironment* env) {
    char args[512];
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    FilterInOut outputs;
    FilterInOut inputs;
    AVRational time_base = videoStream->time_base;

    filterGraph.Create();
    bufferSrcCtx = nullptr;
    bufferSinkCtx = nullptr;

    filterGraph()->nb_threads = 4;

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        codecCtx()->width, codecCtx()->height, codecCtx()->pix_fmt,
        time_base.num, time_base.den,
        codecCtx()->sample_aspect_ratio.num, codecCtx()->sample_aspect_ratio.den);

    if (avfilter_graph_create_filter(&bufferSrcCtx, buffersrc, "in",
        args, NULL, filterGraph()) < 0) {
        env->ThrowError("avfilter_graph_create_filter failed (Cannot create buffer source)");
    }

    /* buffer video sink: to terminate the filter chain. */
    if (avfilter_graph_create_filter(&bufferSinkCtx, buffersink, "out",
        NULL, NULL, filterGraph()) < 0) {
        env->ThrowError("avfilter_graph_create_filter failed (Cannot create buffer sink)");
    }

    if (av_opt_set_bin(bufferSinkCtx, "pix_fmts",
        (uint8_t*)&codecCtx()->pix_fmt, sizeof(codecCtx()->pix_fmt),
        AV_OPT_SEARCH_CHILDREN) < 0) {
        env->ThrowError("av_opt_set_bin failed (cannot set output pixel format)");
    }

    /*
    * Set the endpoints for the filter graph. The filter_graph will
    * be linked to the graph described by filters_descr.
    */

    /*
    * The buffer source output must be connected to the input pad of
    * the first filter described by filters_descr; since the first
    * filter input label is not specified, it is set to "in" by
    * default.
    */
    outputs()->name = av_strdup("in");
    outputs()->filter_ctx = bufferSrcCtx;
    outputs()->pad_idx = 0;
    outputs()->next = NULL;

    /*
    * The buffer sink input must be connected to the output pad of
    * the last filter described by filters_descr; since the last
    * filter output label is not specified, it is set to "out" by
    * default.
    */
    inputs()->name = av_strdup("out");
    inputs()->filter_ctx = bufferSinkCtx;
    inputs()->pad_idx = 0;
    inputs()->next = NULL;

    if (avfilter_graph_parse_ptr(filterGraph(), filterdesc.c_str(),
        &inputs(), &outputs(), NULL) < 0) {
        env->ThrowError("avfilter_graph_parse_ptr failed");
    }

    if (avfilter_graph_config(filterGraph(), NULL) < 0) {
        env->ThrowError("avfilter_graph_config failed");
    }
}
#endif

void AMTSource::MakeVideoInfo(const VideoFormat& vfmt, const AudioFormat& afmt) {
    vi.width = vfmt.width;
    vi.height = vfmt.height;
    vi.SetFPS(vfmt.frameRateNum, vfmt.frameRateDenom);
    vi.num_frames = int(frames.size());

    interlaced = !vfmt.progressive;

    if (audioFrames.size() > 0) {
        audioSamplesPerFrame = 1024;
        // waveLengthはゼロのこともあるので注意
        for (int i = 0; i < (int)audioFrames.size(); i++) {
            if (audioFrames[i].waveLength != 0) {
                audioSamplesPerFrame = audioFrames[i].waveLength / 4; // 16bitステレオ前提
                break;
            }
        }
        vi.audio_samples_per_second = afmt.sampleRate;
        vi.sample_type = SAMPLE_INT16;
        vi.num_audio_samples = audioSamplesPerFrame * audioFrames.size();
        vi.nchannels = 2;
    } else {
        // No audio
        vi.audio_samples_per_second = 0;
        vi.num_audio_samples = 0;
        vi.nchannels = 0;
    }
}

void AMTSource::UpdateVideoInfo(IScriptEnvironment* env) {
    // ビット深度は取得してないのでffmpegから取得する
    const auto streamPixFmt = (AVPixelFormat)videoStream->codecpar->format;
    const auto codecPixFmt = codecCtx()->pix_fmt;
    const int streamBitDepth = (streamPixFmt == AV_PIX_FMT_P010LE) ? 16 : av_pix_fmt_desc_get(streamPixFmt)->comp[0].depth;
    const int codecBitDepth = (codecPixFmt == AV_PIX_FMT_P010LE) ? 16 : av_pix_fmt_desc_get(codecPixFmt)->comp[0].depth;
    if (streamBitDepth > 8 && codecBitDepth > 8 && streamBitDepth < codecBitDepth) {
        // hwデコードの場合、10bitでもP010で返されることがある
        // もとのビット深度を採用するため、streamPixFmtを使用する
        vi.pixel_type = toAVSFormat(streamPixFmt, env);
        convertPix = ConvertPixFuncs(streamBitDepth, codecBitDepth);
    } else {
        vi.pixel_type = toAVSFormat(codecPixFmt, env);
    }

#if ENABLE_FFMPEG_FILTER
    if (bufferSinkCtx) {
        // フィルタがあればフィルタの出力に更新
        const AVFilterLink* outlink = bufferSinkCtx->inputs[0];
        vi.pixel_type = toAVSFormat((AVPixelFormat)outlink->format, env);

        if (outlink->w != vi.width ||
            outlink->h != vi.height) {
            env->ThrowError("ffmpeg filter output is resized, which is not supported on current AMTSource.");
        }
    }
#endif
}

void AMTSource::ResetDecoder(IScriptEnvironment* env) {
    lastDecodeFrame = -1;
    prevFrame = nullptr;
    MakeCodecContext(env);
#if ENABLE_FFMPEG_FILTER
    if (filterdesc.size()) {
        MakeFilterGraph(env);
    }
#endif
}

PVideoFrame AMTSource::MakeFrame(AVFrame* top, AVFrame* bottom, IScriptEnvironment* env) {
    PVideoFrame ret = env->NewVideoFrame(vi);
    const auto srcFormat = (AVPixelFormat)(top->format);
    const int srcBitDepth = (srcFormat == AV_PIX_FMT_P010LE) ? 16 : av_pix_fmt_desc_get(srcFormat)->comp[0].depth;

    if (srcBitDepth > 8) {
        MergeField<uint16_t>(ret, top, bottom, AVSFormatBitdepth(vi.pixel_type), srcBitDepth, env);
    } else {
        MergeField<uint8_t>(ret, top, bottom, AVSFormatBitdepth(vi.pixel_type), srcBitDepth, env);
    }

    // LinuxではAvisynthPlusを使うので設定できなそう?
#if AVISYNTH_MODE == AVISYNTH_NEO
    // フレームタイプ
    ret->SetProperty("FrameType", top->pict_type);
#elif AVISYNTH_MODE == AVISYNTH_PLUS
    auto avsmap = env->getFramePropsRW(ret);
    env->propSetInt(avsmap, "FrameType", top->pict_type, AVSPropAppendMode::PROPAPPENDMODE_REPLACE);
#else
    static_assert(false, "Invalid AVISYNTH_MODE");
#endif

#ifndef AMATSUKAZE2DLL
    // QPテーブル
    if (outputQP) {
        int qp_stride, qp_scale_type;
        const int8_t* qp_table = av_frame_get_qp_table(top, &qp_stride, &qp_scale_type);
        if (qp_table) {
            VideoInfo qpvi = vi;
            if (!qp_stride) {
                qpvi.width = (vi.width + 15) >> 4;
                qpvi.height = 1;
            } else {
                qpvi.width = qp_stride;
                qpvi.height = (vi.height + 15) >> 4;
            }
            qpvi.pixel_type = VideoInfo::CS_Y8;
            PVideoFrame qpframe = env->NewVideoFrame(qpvi);
            env->BitBlt(qpframe->GetWritePtr(), qpframe->GetPitch(),
                (const BYTE*)qp_table, qpvi.width, qpvi.width, qpvi.height);
            if (top->pict_type != AV_PICTURE_TYPE_B) {
                nonBQPTable = qpframe;
            }
#if AVISYNTH_MODE == AVISYNTH_NEO
            ret->SetProperty("QP_Table", qpframe);
            ret->SetProperty("QP_Table_Non_B", nonBQPTable);
            ret->SetProperty("QP_Stride", qp_stride ? qpframe->GetPitch() : 0);
            ret->SetProperty("QP_ScaleType", qp_scale_type);
#elif AVISYNTH_MODE == AVISYNTH_PLUS
            int error;
            auto avsmap = env->getFramePropsRW(ret);
            env->propSetFrame(avsmap, "QP_Table", qpframe, AVSPropAppendMode::PROPAPPENDMODE_REPLACE);
            env->propSetFrame(avsmap, "QP_Table_Non_B", nonBQPTable, AVSPropAppendMode::PROPAPPENDMODE_REPLACE);
            env->propSetInt(avsmap, "QP_Stride", qp_stride ? qpframe->GetPitch() : 0, AVSPropAppendMode::PROPAPPENDMODE_REPLACE);
            env->propSetInt(avsmap, "QP_ScaleType", qp_scale_type, AVSPropAppendMode::PROPAPPENDMODE_REPLACE);
#else
            static_assert(false, "Invalid AVISYNTH_MODE");
#endif
            PVideoFrame dcframe = env->NewVideoFrame(qpvi);
            auto dc_table_data = av_frame_get_side_data(top, AV_FRAME_DATA_MB_DC_TABLE_DATA);
            if (dc_table_data) {
                env->BitBlt(dcframe->GetWritePtr(), dcframe->GetPitch(),
                    (const BYTE*)dc_table_data->data, qpvi.width, qpvi.width, qpvi.height);
#if AVISYNTH_MODE == AVISYNTH_NEO
                ret->SetProperty("DC_Table", dcframe);
#elif AVISYNTH_MODE == AVISYNTH_PLUS
                env->propSetFrame(avsmap, "DC_Table", dcframe, AVSPropAppendMode::PROPAPPENDMODE_REPLACE);
#else
            static_assert(false, "Invalid AVISYNTH_MODE");
#endif
            }
        }
    }
#endif

    return ret;
}

void AMTSource::PutFrame(int n, const PVideoFrame& frame) {
    CacheFrame* pcache = new CacheFrame();
    pcache->data = frame;
    pcache->key = n;
    frameCache[n] = pcache;
    recentAccessed.push_front(pcache);

    if ((int)recentAccessed.size() > seekDistance * 3 / 2) {
        // キャッシュから溢れたら削除
        CacheFrame* pdel = recentAccessed.back();
        frameCache.erase(pdel->key);
        recentAccessed.pop_back();
        delete pdel;
    }
}

int AMTSource::AVSFormatBitdepth(const int avsformat) {
    // ビット深度取得
    switch (avsformat) {
    case VideoInfo::CS_YUV420P12: return 12;
    case VideoInfo::CS_YUV420P10: return 10;
    case VideoInfo::CS_YV12:
    default: return 8;
    }
    return 0;
}

int AMTSource::toAVSFormat(AVPixelFormat format, IScriptEnvironment* env) {
    // ビット深度取得
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(format);
    switch (desc->comp[0].depth) {
    case 8:
        return VideoInfo::CS_YV12;
    case 10:
        return VideoInfo::CS_YUV420P10;
    case 12:
        return VideoInfo::CS_YUV420P12;
    }
    env->ThrowError("対応していないビット深度です");
    return 0;
}

#if ENABLE_FFMPEG_FILTER
void AMTSource::InputFrameFilter(Frame* frame, bool enableOut, IScriptEnvironment* env) {
    /* push the decoded frame into the filtergraph */
    if (av_buffersrc_add_frame_flags(bufferSrcCtx, frame ? (*frame)() : nullptr, 0) < 0) {
        env->ThrowError("av_buffersrc_add_frame_flags failed (Error while feeding the filtergraph)");
    }

    /* pull filtered frames from the filtergraph */
    while (1) {
        Frame filtered;
        int ret = av_buffersink_get_frame(bufferSinkCtx, filtered());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // もっと入力が必要 or もうフレームがない
            break;
        }
        if (ret < 0) {
            env->ThrowError("av_buffersink_get_frame failed");
        }
        if (enableOut) {
            OnFrameOutput(filtered, env);
        }
    }
}

void AMTSource::OnFrameDecoded(Frame& frame, IScriptEnvironment* env) {
    if (bufferSrcCtx) {
        // フィルタ処理
        //frame()->pts = frame()->best_effort_timestamp;
        InputFrameFilter(&frame, true, env);
    } else {
        OnFrameOutput(frame, env);
    }
}
#endif

void AMTSource::OnFrameOutput(Frame& frame, IScriptEnvironment* env) {
    // ffmpegのpts wrapの仕方が謎なので下位33bitのみを見る
    //（26時間以上ある動画だと重複する可能性はあるが無視）
    int64_t pts = frame()->pts & ((int64_t(1) << 33) - 1);
    // FilterSourceFrame の pts はドロップ補正後のPTSなので、ドロップ補正前のoriginalFramePTSを使って照合する
    int64_t headDiff = 0, tailDiff = 0;
    auto it = std::lower_bound(frames.begin(), frames.end(), pts, [](const FilterSourceFrame& e, int64_t pts) {
        return e.originalFramePTS < pts;
        });

    if (it == frames.begin() && pts < it->originalFramePTS) {
        headDiff = it->originalFramePTS - pts;
        // 小さすぎた場合は1周分追加して見る
        pts += (int64_t(1) << 33);
        it = std::lower_bound(frames.begin(), frames.end(), pts, [](const FilterSourceFrame& e, int64_t pts) {
            return e.originalFramePTS < pts;
            });
    }

    if (it == frames.end()) {
        // 最後より後ろだった
        tailDiff = pts - frames.back().originalFramePTS;
        // 前の可能性もあるので、判定
        if (headDiff == 0 || headDiff > tailDiff) {
            lastDecodeFrame = vi.num_frames;
        }
        prevFrame = nullptr; // 連続でなくなる場合はnullリセット
        return;
    }

    if (it->originalFramePTS != pts) {
        // 一致するフレームがない
        ctx.incrementCounter(AMT_ERR_UNKNOWN_PTS);
        ctx.warnF("Unknown PTS frame %lld", pts);
        prevFrame = nullptr; // 連続でなくなる場合はnullリセット
        return;
    }

    int frameIndex = int(it - frames.begin());
    auto cacheit = frameCache.find(frameIndex);

    if (it->halfDelay) {
        // ディレイを適用させる
        if (cacheit != frameCache.end()) {
            // すでにキャッシュにある
            UpdateAccessed(cacheit->second);
            lastDecodeFrame = frameIndex;
        } else if (prevFrame != nullptr) {
            PutFrame(frameIndex, MakeFrame((*prevFrame)(), frame(), env));
            lastDecodeFrame = frameIndex;
        } else {
            // 直前のフレームがないのでフレームを作れない
        }

        // 次のフレームも同じフレームを参照してたらそれも出力
        auto next = it + 1;
        if (next != frames.end() && next->originalFramePTS == it->originalFramePTS) {
            auto cachenext = frameCache.find(frameIndex + 1);
            if (cachenext != frameCache.end()) {
                // すでにキャッシュにある
                UpdateAccessed(cachenext->second);
            } else {
                PutFrame(frameIndex + 1, MakeFrame(frame(), frame(), env));
            }
            lastDecodeFrame = frameIndex + 1;
        }
    } else {
        // そのまま
        if (cacheit != frameCache.end()) {
            // すでにキャッシュにある
            UpdateAccessed(cacheit->second);
        } else {
            PutFrame(frameIndex, MakeFrame(frame(), frame(), env));
        }
        lastDecodeFrame = frameIndex;
    }

    prevFrame = std::unique_ptr<Frame>(new Frame(frame));
}

void AMTSource::UpdateAccessed(CacheFrame* frame) {
    // recentAccessed にframeがあれば先頭に移動
    auto target = std::find(recentAccessed.begin(), recentAccessed.end(), frame);
    if (target != recentAccessed.end()) {
        recentAccessed.erase(target);
    }
    recentAccessed.push_front(frame);
}

PVideoFrame AMTSource::ForceGetFrame(int n, IScriptEnvironment* env) {
    if (frameCache.size() == 0) {
        return env->NewVideoFrame(vi);
    }
    auto lb = frameCache.lower_bound(n);
    if (lb == frameCache.end() || (lb->first != n && lb != frameCache.begin())) {
        lb--;
    }
    UpdateAccessed(lb->second);
    return lb->second->data;
}

void AMTSource::DecodeLoop(int goal, IScriptEnvironment* env) {
    Frame frame;
    AVPacket packet = AVPacket();

    // CUVIDはpic_typeが適切にセットされないのでキーフレームかどうかが分からない
    // FFmpegは参照フレームが欠如していてもフレームを返してしまうので
    // シーク後の最初のフレームが欠損を含む可能性がある
    // これはFFmpegがその欠如した参照フレームがパケットロスによるものなのか
    // シークによるものなのかの区別ができないため、仕方ないのだが、
    // シークによるものの場合は、本来はデコードできたはずのフレームなので、
    // その欠損を含むフレームを返してしまうのは問題
    // pic_typeが適切にセットされるデコーダの場合は、
    // 「Iフレームからが有効なフレーム」と判定すれば良かったが
    // pic_typeが適切にセットされないデコーダの場合は、
    // 自分でIフレームかどうか判定する必要がある
    // packetからキーフレームのPTSを取得して
    // デコードしたフレームがそのPTSならキーフレームと判断する
    int64_t keyFramePTS = -1;
    auto isFrameReady = [&]() {
        // シーク後最初のフレームでないならOK
        if (lastDecodeFrame != -1) return true;
        // キーフレームならOK
#if LIBAVUTIL_VERSION_MAJOR >= 58
        if (frame()->flags & AV_FRAME_FLAG_KEY) return true;
#else
        if (frame()->key_frame) return true;
#endif
        // タイムスタンプがキーフレームのものならキーフレームと判断
        if (keyFramePTS != -1 && keyFramePTS == frame()->pts) return true;
        // まだキーフレームでないので、欠損を含む可能性がある
        return false;
        };

    while (av_read_frame(inputCtx(), &packet) == 0) {
        if (packet.stream_index == videoStream->index) {
            if ((packet.flags & AV_PKT_FLAG_KEY) && keyFramePTS == -1) {
                // 最初のキーフレームのPTSを覚えておく
                keyFramePTS = packet.pts;
            }
            if (avcodec_send_packet(codecCtx(), &packet) != 0) {
                ctx.incrementCounter(AMT_ERR_DECODE_PACKET_FAILED);
                ctx.warn("avcodec_send_packet failed");
            }
            while (avcodec_receive_frame(codecCtx(), frame()) == 0) {
                // 最初はキーフレームまでスキップ
                if (isFrameReady()) {
#if ENABLE_FFMPEG_FILTER
                    OnFrameDecoded(frame, env);
#else
                    OnFrameOutput(frame, env);
#endif
                }
            }
        }
        av_packet_unref(&packet);
        if (lastDecodeFrame >= goal) {
            return;
        }
    }
#if ENABLE_FFMPEG_FILTER
    if (bufferSrcCtx) {
        // ストリームは全て読み取ったのでフィルタをflush
        InputFrameFilter(nullptr, true, env);
    }
#endif
}

void AMTSource::registerFailedFrames(int begin, int end, int replace, IScriptEnvironment* env) {
    for (int f = begin; f < end; f++) {
        failedMap[f] = replace;
    }
    // デコード不可フレーム数が１割を超える場合はエラーとする
    if (failedMap.size() * 10 > frames.size()) {
        env->ThrowError("[AMTSource] デコードできないフレーム数が多すぎます -> %dフレームがデコード不可",
            (int)failedMap.size());
    }
}

AMTSource::AMTSource(AMTContext& ctx,
    const tstring& srcpath,
    const tstring& audiopath,
    const VideoFormat& vfmt, const AudioFormat& afmt,
    const std::vector<FilterSourceFrame>& frames,
    const std::vector<FilterAudioFrame>& audioFrames,
    const DecoderSetting& decoderSetting,
    const int threads,
    const char* filterdesc,
    bool outputQP,
    IScriptEnvironment* env) :
    AMTObject(ctx),
    frames(frames),
    audioFrames(audioFrames),
    decoderSetting(decoderSetting),
    filterdesc(filterdesc),
    decodeThreads(threads),
    audioSamplesPerFrame(0),
    interlaced(false),
    outputQP(outputQP),
    inputCtx(srcpath),
    codecCtx(),
#if ENABLE_FFMPEG_FILTER
    filterGraph(),
    bufferSrcCtx(),
    bufferSinkCtx(),
#endif
    videoStream(nullptr),
    storage(),
    frameCache(),
    recentAccessed(),
    failedMap(),
    vi(),
    mutex(),
    waveFile(audiopath, _T("rb")),
    seekDistance(10),
    lastDecodeFrame(-1),
    prevFrame(),
    nonBQPTable(),
    convertPix() {
#if !ENABLE_FFMPEG_FILTER
    if (this->filterdesc.size()) {
        env->ThrowError("This AMTSouce build does not support FFmpeg filter option ...");
    }
#endif
    MakeVideoInfo(vfmt, afmt);

    if (avformat_find_stream_info(inputCtx(), NULL) < 0) {
        env->ThrowError("avformat_find_stream_info failed");
    }
    videoStream = GetVideoStream(inputCtx());
    if (videoStream == NULL) {
        env->ThrowError("Could not find video stream ...");
    }

    // 初期化
    ResetDecoder(env);
    UpdateVideoInfo(env);
}

AMTSource::~AMTSource() {
    // キャッシュを削除
    while (recentAccessed.size() > 0) {
        CacheFrame* pdel = recentAccessed.back();
        frameCache.erase(pdel->key);
        recentAccessed.pop_back();
        delete pdel;
    }
}

void AMTSource::TransferStreamInfo(std::unique_ptr<AMTSourceData>&& streamInfo) {
    storage = std::move(streamInfo);
}

PVideoFrame __stdcall AMTSource::GetFrame(int n, IScriptEnvironment* env) {
    std::lock_guard<std::mutex> guard(mutex);

    // キャッシュにあれば返す
    auto it = frameCache.find(n);
    if (it != frameCache.end()) {
        UpdateAccessed(it->second);
        return it->second->data;
    }

    // デコードできないフレームは置換フレームに置き換える
    if (failedMap.find(n) != failedMap.end()) {
        n = failedMap[n];
    }

    // キャッシュにないのでデコードする
    if (lastDecodeFrame != -1 && n > lastDecodeFrame && n < lastDecodeFrame + seekDistance) {
        // 前にすすめる
        DecodeLoop(n, env);
    } else {
        // シークしてデコードする
        int keyNum = frames[n].keyFrame;
        for (int i = 0; ; i++) {
            int64_t fileOffset = frames[keyNum].fileOffset / 188 * 188;
            if (av_seek_frame(inputCtx(), -1, fileOffset, AVSEEK_FLAG_BYTE) < 0) {
                THROW(FormatException, "av_seek_frame failed");
            }
            ResetDecoder(env);
            DecodeLoop(n, env);
            if (frameCache.find(n) != frameCache.end()) {
                // デコード成功
                seekDistance = std::max(seekDistance, n - keyNum);
                break;
            }
            if (keyNum <= 0) {
                // これ以上戻れない
                // nからlastDecodeFrameまでをデコード不可とする
                registerFailedFrames(n, lastDecodeFrame, lastDecodeFrame, env);
                break;
            }
            if (lastDecodeFrame >= 0 && lastDecodeFrame < n) {
                // データが足りなくてゴールに到達できなかった
                // このフレームより後ろは全てデコード不可とする
                registerFailedFrames(lastDecodeFrame + 1, (int)frames.size(), lastDecodeFrame, env);
                break;
            }
            if (i == 2) {
                // デコード失敗
                // nからlastDecodeFrameまでをデコード不可とする
                registerFailedFrames(n, lastDecodeFrame, lastDecodeFrame, env);
                break;
            }
            keyNum -= std::max(5, keyNum - frames[keyNum - 1].keyFrame);
        }
    }

    return ForceGetFrame(n, env);
}

void __stdcall AMTSource::GetAudio(void* buf, int64_t start, int64_t count, IScriptEnvironment* env) {
    std::lock_guard<std::mutex> guard(mutex);

    if (audioFrames.size() == 0) return;

    const int sampleBytes = 4; // 16bitステレオ前提
    int frameWaveLength = audioSamplesPerFrame * sampleBytes;
    uint8_t* ptr = (uint8_t*)buf;
    for (int64_t frameIndex = start / audioSamplesPerFrame, frameOffset = start % audioSamplesPerFrame;
        count > 0 && frameIndex < (int64_t)audioFrames.size();
        frameIndex++, frameOffset = 0) {
        // このフレームで埋めるべきバイト数
        int readBytes = std::min<int>(
            (int)(frameWaveLength - frameOffset * sampleBytes),
            (int)count * sampleBytes);

        if (audioFrames[(size_t)frameIndex].waveLength != 0) {
            // waveがあるなら読む
            waveFile.seek(audioFrames[(size_t)frameIndex].waveOffset + frameOffset * sampleBytes, SEEK_SET);
            waveFile.read(MemoryChunk(ptr, readBytes));
        } else {
            // ない場合はゼロ埋めする
            memset(ptr, 0x00, readBytes);
        }

        ptr += readBytes;
        count -= readBytes / sampleBytes;
    }
    if (count > 0) {
        // ファイルの終わりまで到達したら残りはゼロで埋める
        memset(ptr, 0, (size_t)count * sampleBytes);
    }
}

const VideoInfo& __stdcall AMTSource::GetVideoInfo() { return vi; }

bool __stdcall AMTSource::GetParity(int n) {
    return interlaced;
}

int __stdcall AMTSource::SetCacheHints(int cachehints, int frame_range) {
    // 直接インスタンス化される場合、MTGuardが入らないのでMT_NICE_FILTER以外ダメ
    if (cachehints == CACHE_GET_MTMODE) return MT_NICE_FILTER;
    return 0;
};

AMTContext* g_ctx_for_plugin_filter = nullptr;

void SaveAMTSource(
    const tstring& savepath,
    const tstring& srcpath,
    const tstring& audiopath,
    const VideoFormat& vfmt, const AudioFormat& afmt,
    const std::vector<FilterSourceFrame>& frames,
    const std::vector<FilterAudioFrame>& audioFrames,
    const DecoderSetting& decoderSetting) {
    File file(savepath, _T("wb"));
    file.writeArray(std::vector<tchar>(srcpath.begin(), srcpath.end()));
    file.writeArray(std::vector<tchar>(audiopath.begin(), audiopath.end()));
    file.writeValue(vfmt);
    file.writeValue(afmt);
    file.writeArray(frames);
    file.writeArray(audioFrames);
    file.writeValue(decoderSetting);
}

PClip LoadAMTSource(const tstring& loadpath, const char* filterdesc, bool outputQP, int threads, IScriptEnvironment* env) {
    File file(loadpath, _T("rb"));
    auto srcpathv = file.readArray<tchar>();
    tstring srcpath(srcpathv.begin(), srcpathv.end());
    auto audiopathv = file.readArray<tchar>();
    tstring audiopath(audiopathv.begin(), audiopathv.end());
    VideoFormat vfmt = file.readValue<VideoFormat>();
    AudioFormat afmt = file.readValue<AudioFormat>();
    auto data = std::unique_ptr<AMTSourceData>(new AMTSourceData());
    data->frames = file.readArray<FilterSourceFrame>();
    data->audioFrames = file.readArray<FilterAudioFrame>();
    DecoderSetting decoderSetting = file.readValue<DecoderSetting>();
    AMTSource* src = new AMTSource(*g_ctx_for_plugin_filter,
        srcpath, audiopath, vfmt, afmt, data->frames, data->audioFrames, decoderSetting, threads, filterdesc, outputQP, env);
    src->TransferStreamInfo(std::move(data));
    return src;
}

AVSValue CreateAMTSource(AVSValue args, void* user_data, IScriptEnvironment* env) {
    if (g_ctx_for_plugin_filter == nullptr) {
        g_ctx_for_plugin_filter = new AMTContext();
    }
    tstring filename = char_to_tstring(args[0].AsString());
    const char* filterdesc = args[1].AsString("");
    const bool outputQP = args[2].AsBool(true);
    const int threads = args[3].AsInt(0);
    return LoadAMTSource(filename, filterdesc, outputQP, threads, env);
}

} // namespace av {
