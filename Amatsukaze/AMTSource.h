/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "common.h"
#include "rgy_util.h"
RGY_DISABLE_WARNING_PUSH
RGY_DISABLE_WARNING_STR("-Wsign-compare")
#include "avisynth.h"
#pragma comment(lib, "avisynth.lib")
RGY_DISABLE_WARNING_POP

#include <memory>
#include <vector>
#include <array>
#include <mutex>
#include <set>
#include <deque>
#include <unordered_map>
#include "ConvertPix.h"
#include "StreamReform.h"
#include "ReaderWriterFFmpeg.h"

namespace av {

struct FakeAudioSample {

    enum {
        MAGIC = 0xFACE0D10,
        VERSION = 1
    };

    int32_t magic;
    int32_t version;
    int64_t index;
};

struct AMTSourceData {
    std::vector<FilterSourceFrame> frames;
    std::vector<FilterAudioFrame> audioFrames;
};

class AMTSource : public IClip, AMTObject {
    const std::vector<FilterSourceFrame>& frames;
    const std::vector<FilterAudioFrame>& audioFrames;
    DecoderSetting decoderSetting;
    std::string filterdesc;
    int decodeThreads;
    int audioSamplesPerFrame;
    bool interlaced;

    bool outputQP; // QPテーブルを出力するか

    InputContext inputCtx;
    CodecContext codecCtx;

#if ENABLE_FFMPEG_FILTER
    FilterGraph filterGraph;
    AVFilterContext* bufferSrcCtx;
    AVFilterContext* bufferSinkCtx;
#endif

    AVStream *videoStream;

    std::unique_ptr<AMTSourceData> storage;

    struct CacheFrame {
        PVideoFrame data;
        int key;
    };

    std::map<int, CacheFrame*> frameCache;
    std::deque<CacheFrame*> recentAccessed;

    // デコードできなかったフレームの置換先リスト
    std::map<int, int> failedMap;

    VideoInfo vi;

    std::mutex mutex;

    File waveFile;

    int seekDistance;

    // OnFrameDecodedで直前にデコードされたフレーム
    // まだデコードしてない場合は-1
    int lastDecodeFrame;

    // codecCtxが直前にデコードしたフレーム番号
    // まだデコードしてない場合はnullptr
    std::unique_ptr<Frame> prevFrame;

    // 直前のnon B QPテーブル
    PVideoFrame nonBQPTable;

    ConvertPixFuncs convertPix;

    const AVCodec* getHWAccelCodec(AVCodecID vcodecId);

    void MakeCodecContext(IScriptEnvironment* env);

#if ENABLE_FFMPEG_FILTER
    void MakeFilterGraph(IScriptEnvironment* env);
#endif

    void MakeVideoInfo(const VideoFormat& vfmt, const AudioFormat& afmt);

    void UpdateVideoInfo(IScriptEnvironment* env);

    void ResetDecoder(IScriptEnvironment* env);

    template <typename T>
    void Copy1(T* dst, const T* top, const T* bottom, int w, int h, int dpitch, int tpitch, int bpitch) {
        for (int y = 0; y < h; y += 2) {
            T* dst0 = dst + dpitch * (y + 0);
            T* dst1 = dst + dpitch * (y + 1);
            const T* src0 = top + tpitch * (y + 0);
            const T* src1 = bottom + bpitch * (y + 1);
            memcpy(dst0, src0, sizeof(T) * w);
            memcpy(dst1, src1, sizeof(T) * w);
        }
    }

    template <typename T>
    void Copy2(T* dstU, T* dstV, const T* top, const T* bottom, int w, int h, int dpitch, int tpitch, int bpitch) {
        for (int y = 0; y < h; y += 2) {
            T* dstU0 = dstU + dpitch * (y + 0);
            T* dstU1 = dstU + dpitch * (y + 1);
            T* dstV0 = dstV + dpitch * (y + 0);
            T* dstV1 = dstV + dpitch * (y + 1);
            const T* src0 = top + tpitch * (y + 0);
            const T* src1 = bottom + bpitch * (y + 1);
            for (int x = 0; x < w; x++) {
                dstU0[x] = src0[x * 2 + 0];
                dstV0[x] = src0[x * 2 + 1];
                dstU1[x] = src1[x * 2 + 0];
                dstV1[x] = src1[x * 2 + 1];
            }
        }
    }

    template <typename T>
    void MergeField(PVideoFrame& dst, AVFrame* top, AVFrame* bottom, const int dstBitDepth, const int srcBitDepth, IScriptEnvironment* env) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((AVPixelFormat)(top->format));

        const bool nv12 = top->format == AV_PIX_FMT_NV12 || top->format == AV_PIX_FMT_P010LE;

        T* srctY = (T*)top->data[0];
        T* srctU = (T*)top->data[1];
        T* srctV = (!nv12) ? (T*)top->data[2] : ((T*)top->data[1] + 1);
        T* srcbY = (T*)bottom->data[0];
        T* srcbU = (T*)bottom->data[1];
        T* srcbV = (!nv12) ? (T*)bottom->data[2] : ((T*)bottom->data[1] + 1);
        T* dstY = (T*)dst->GetWritePtr(PLANAR_Y);
        T* dstU = (T*)dst->GetWritePtr(PLANAR_U);
        T* dstV = (T*)dst->GetWritePtr(PLANAR_V);

        const int srctPitchY = top->linesize[0] / sizeof(T);
        const int srctPitchUV = top->linesize[1] / sizeof(T);
        const int srcbPitchY = bottom->linesize[0] / sizeof(T);
        const int srcbPitchUV = bottom->linesize[1] / sizeof(T);
        const int dstPitchY = dst->GetPitch(PLANAR_Y) / sizeof(T);
        const int dstPitchUV = dst->GetPitch(PLANAR_U) / sizeof(T);
        const int widthUV = vi.width >> desc->log2_chroma_w;
        const int heightUV = vi.height >> desc->log2_chroma_h;

        if (dstBitDepth != srcBitDepth) {
            if (srcBitDepth == 16 && convertPix.convert1) {
                convertPix.convert1(dstY, srctY, srcbY, vi.width, vi.height, dstPitchY, srctPitchY, srcbPitchY);

                if (nv12) {
                    convertPix.convert2(dstU, dstV, srctU, srcbU, widthUV, heightUV, dstPitchUV, srctPitchUV, srcbPitchUV);
                } else {
                    convertPix.convert1(dstU, srctU, srcbU, widthUV, heightUV, dstPitchUV, srctPitchUV, srcbPitchUV);
                    convertPix.convert1(dstV, srctV, srcbV, widthUV, heightUV, dstPitchUV, srctPitchUV, srcbPitchUV);
                }
            } else {
                env->ThrowError("not supported conversion.");
            }
        } else {
            Copy1<T>(dstY, srctY, srcbY, vi.width, vi.height, dstPitchY, srctPitchY, srcbPitchY);

            if (nv12) {
                Copy2<T>(dstU, dstV, srctU, srcbU, widthUV, heightUV, dstPitchUV, srctPitchUV, srcbPitchUV);
            } else {
                Copy1<T>(dstU, srctU, srcbU, widthUV, heightUV, dstPitchUV, srctPitchUV, srcbPitchUV);
                Copy1<T>(dstV, srctV, srcbV, widthUV, heightUV, dstPitchUV, srctPitchUV, srcbPitchUV);
            }
        }
    }

    PVideoFrame MakeFrame(AVFrame * top, AVFrame * bottom, IScriptEnvironment * env);

    void PutFrame(int n, const PVideoFrame & frame);

    int AVSFormatBitdepth(const int avsformat);
    int toAVSFormat(AVPixelFormat format, IScriptEnvironment * env);

#if ENABLE_FFMPEG_FILTER
    void InputFrameFilter(Frame* frame, bool enableOut, IScriptEnvironment* env);

    void OnFrameDecoded(Frame& frame, IScriptEnvironment* env);
#endif

    void OnFrameOutput(Frame& frame, IScriptEnvironment* env);

    void UpdateAccessed(CacheFrame* frame);

    PVideoFrame ForceGetFrame(int n, IScriptEnvironment* env);

    void DecodeLoop(int goal, IScriptEnvironment* env);

    void registerFailedFrames(int begin, int end, int replace, IScriptEnvironment* env);

public:
    AMTSource(AMTContext& ctx,
        const tstring& srcpath,
        const tstring& audiopath,
        const VideoFormat& vfmt, const AudioFormat& afmt,
        const std::vector<FilterSourceFrame>& frames,
        const std::vector<FilterAudioFrame>& audioFrames,
        const DecoderSetting& decoderSetting,
        const int threads,
        const char* filterdesc,
        bool outputQP,
        IScriptEnvironment* env);

    ~AMTSource();

    void TransferStreamInfo(std::unique_ptr<AMTSourceData>&& streamInfo);

    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env);

    void __stdcall GetAudio(void* buf, int64_t start, int64_t count, IScriptEnvironment* env);

    const VideoInfo& __stdcall GetVideoInfo();

    bool __stdcall GetParity(int n);

    int __stdcall SetCacheHints(int cachehints, int frame_range);
};

extern AMTContext* g_ctx_for_plugin_filter;

void SaveAMTSource(
    const tstring& savepath,
    const tstring& srcpath,
    const tstring& audiopath,
    const VideoFormat& vfmt, const AudioFormat& afmt,
    const std::vector<FilterSourceFrame>& frames,
    const std::vector<FilterAudioFrame>& audioFrames,
    const DecoderSetting& decoderSetting);

PClip LoadAMTSource(const tstring& loadpath, const char* filterdesc, bool outputQP, IScriptEnvironment* env);

AVSValue CreateAMTSource(AVSValue args, void* user_data, IScriptEnvironment* env);

} // namespace av {
