/**
* Amtasukaze Logo GUI Support
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "ReaderWriterFFmpeg.h"
#include "LogoScan.h"
#include "JpegCompress.h"

#if defined(_WIN32) && defined(AMATSUKAZE2DLL)
#define AMT_LOGO_GUI_WITHOUT_SWSCALE 1
#endif

#if defined(AMT_LOGO_GUI_WITHOUT_SWSCALE)
#include <type_traits>

extern "C" {
#include <libavutil/intreadwrite.h>
#include <libavutil/pixdesc.h>
}

namespace {

// Logo GUI プレビュー用: naive に BT.709 で YUV→BGR24 変換する(出力は常に 8bit BGR)。
// - 8bit リミテッド(JPEG以外の通常デコード): Y 16-235 / Cb,Cr は 128 付近を中心とする標準テレビレンジ
// - YUVJ420P はフルレンジ(8bit)
// - 10bit(BS4K 等): BT.709 リミテッドの 64..940(Y) / U,V は 512 中心 に相当する係数へ落としてから同じ行列適用

inline uint8_t ClampU8(float v) {
    if (v <= 0.f)
        return 0;
    if (v >= 255.f)
        return 255;
    return static_cast<uint8_t>(v + 0.5f);
}

template<typename TSample, int BitDepth>
inline void YuvToBgrBt709Limited(TSample y, TSample u, TSample v, uint8_t* bgr) {
    static_assert(BitDepth == 8 || BitDepth == 10);
    float y1, cb, cr;
    if constexpr (BitDepth == 8) {
        static_assert(std::is_same_v<TSample, uint8_t>, "8bit は uint8_t");
        const float fy = static_cast<float>(uint8_t(y));
        const float fu = static_cast<float>(uint8_t(u));
        const float fv = static_cast<float>(uint8_t(v));
        y1 = fy - 16.f;
        cb = fu - 128.f;
        cr = fv - 128.f;
    } else if constexpr (BitDepth == 10) {
        static_assert(std::is_same_v<TSample, uint16_t>, "10bit は uint16_t(下位 Depth bit が有効)");
        const unsigned mask = (unsigned)((1 << BitDepth) - 1);
        const unsigned yi = (unsigned(y)) & mask;
        const unsigned ui = (unsigned(u)) & mask;
        const unsigned vi = (unsigned(v)) & mask;
        const float fy8 = 16.f + (float(yi) - 64.f) * (219.f / 876.f);
        const float fu8 = 128.f + (float(ui) - 512.f) * (112.f / 448.f);
        const float fv8 = 128.f + (float(vi) - 512.f) * (112.f / 448.f);
        y1 = fy8 - 16.f;
        cb = fu8 - 128.f;
        cr = fv8 - 128.f;
    }
    const float r = 1.164383f * y1 + 1.792741f * cr;
    const float g = 1.164383f * y1 - 0.213249f * cb - 0.532909f * cr;
    const float b = 1.164383f * y1 + 2.112402f * cb;
    bgr[0] = ClampU8(b);
    bgr[1] = ClampU8(g);
    bgr[2] = ClampU8(r);
}

// BT.709 / 8bit / full range (YUVJ420P 等)
inline void YuvToBgrBt709Full(uint8_t y, uint8_t u, uint8_t v, uint8_t* bgr) {
    const float fy = static_cast<float>(uint8_t(y));
    const float cb = static_cast<float>(uint8_t(u)) - 128.f;
    const float cr = static_cast<float>(uint8_t(v)) - 128.f;
    const float r = fy + 1.5748f * cr;
    const float g = fy - 0.187324f * cb - 0.468124f * cr;
    const float b = fy + 1.8556f * cb;
    bgr[0] = ClampU8(b);
    bgr[1] = ClampU8(g);
    bgr[2] = ClampU8(r);
}

typedef void (*YuvPixelFunc8bit)(uint8_t y, uint8_t u, uint8_t v, uint8_t* bgr);

inline uint16_t ReadU16At(const uint8_t* pixelBytes, bool bigEndian) {
    return bigEndian ? AV_RB16(pixelBytes) : AV_RL16(pixelBytes);
}

inline int Normalize10bitWord(uint16_t raw, unsigned shift, unsigned depth) {
    if (depth == 0u) depth = 10u;
    if (depth > 14u)
        depth = 14u;
    const unsigned mask = (1u << depth) - 1u;
    return static_cast<int>((unsigned(raw) >> shift) & mask);
}

} // namespace
#endif // defined(AMT_LOGO_GUI_WITHOUT_SWSCALE)

namespace av {

class GUIMediaFile : public AMTObject {
    InputContext inputCtx;
    CodecContext codecCtx;
    AVStream *videoStream;
#if !defined(AMT_LOGO_GUI_WITHOUT_SWSCALE)
    SwsContext * swsctx;
#endif

    // OnFrameDecodedで直前にデコードされたフレーム
    // まだデコードしてない場合は-1
    int lastDecodeFrame;

    int64_t fileSize;

    Frame prevframe;
    int width, height;

    void MakeCodecContext() {
        AVCodecID vcodecId = videoStream->codecpar->codec_id;
        const AVCodec *pCodec = avcodec_find_decoder(vcodecId);
        if (pCodec == NULL) {
            THROW(FormatException, "Could not find decoder ...");
        }
        codecCtx.Set(pCodec);
        if (avcodec_parameters_to_context(codecCtx(), videoStream->codecpar) != 0) {
            THROW(FormatException, "avcodec_parameters_to_context failed");
        }
        if (avcodec_open2(codecCtx(), pCodec, NULL) != 0) {
            THROW(FormatException, "avcodec_open2 failed");
        }
    }

    void init(AVFrame* frame) {
#if !defined(AMT_LOGO_GUI_WITHOUT_SWSCALE)
        if (swsctx) {
            sws_freeContext(swsctx);
            swsctx = nullptr;
        }
#endif
        width = frame->width;
        height = frame->height;
#if !defined(AMT_LOGO_GUI_WITHOUT_SWSCALE)
        swsctx = sws_getCachedContext(NULL, width, height,
            (AVPixelFormat)frame->format, width, height,
            AV_PIX_FMT_BGR24, 0, 0, 0, 0);
#endif
    }

#if defined(AMT_LOGO_GUI_WITHOUT_SWSCALE)
    void ConvertYuv420pToBgr8(uint8_t* bgr, const AVFrame* frame, YuvPixelFunc8bit toBgr) {
        const uint8_t* yp = frame->data[0];
        const uint8_t* up = frame->data[1];
        const uint8_t* vp = frame->data[2];
        const int ly = frame->linesize[0];
        const int lu = frame->linesize[1];
        const int lv = frame->linesize[2];
        const int w = frame->width;
        const int h = frame->height;
        for (int y = 0; y < h; ++y) {
            uint8_t* row = bgr + y * w * 3;
            const int y2 = y >> 1;
            for (int x = 0; x < w; ++x) {
                const int x2 = x >> 1;
                const uint8_t Y = yp[y * ly + x];
                const uint8_t U = up[y2 * lu + x2];
                const uint8_t V = vp[y2 * lv + x2];
                toBgr(Y, U, V, row + x * 3);
            }
        }
    }

    // AV_PIX_FMT_YUV420P10*: プラナ 10bit(各サンプル 16bit 字に格納、デスクリプタで shift/deep)。
    void ConvertYuv420pPlanar10ToBgr(uint8_t* bgr, const AVFrame* frame, bool pixelBigEndian, const AVPixFmtDescriptor* d) {
        const uint8_t* yPlane = frame->data[0];
        const uint8_t* uPlane = frame->data[1];
        const uint8_t* vPlane = frame->data[2];
        const int ly = frame->linesize[0];
        const int lu = frame->linesize[1];
        const int lv = frame->linesize[2];
        const unsigned shY = d->comp[0].shift;
        const unsigned dpY = d->comp[0].depth ? d->comp[0].depth : 10u;
        const unsigned shU = d->comp[1].shift;
        const unsigned dpU = d->comp[1].depth ? d->comp[1].depth : 10u;
        const unsigned shV = d->comp[2].shift;
        const unsigned dpV = d->comp[2].depth ? d->comp[2].depth : 10u;
        const int w = frame->width;
        const int h = frame->height;
        for (int y = 0; y < h; ++y) {
            uint8_t* row = bgr + y * w * 3;
            const int y2 = y >> 1;
            for (int x = 0; x < w; ++x) {
                const int x2 = x >> 1;
                const uint16_t rawY = ReadU16At(yPlane + ly * y + x * 2, pixelBigEndian);
                const uint16_t rawU = ReadU16At(uPlane + lu * y2 + x2 * 2, pixelBigEndian);
                const uint16_t rawV = ReadU16At(vPlane + lv * y2 + x2 * 2, pixelBigEndian);
                const unsigned normY = static_cast<unsigned>(Normalize10bitWord(rawY, shY, dpY)) & 0x3FFu;
                const unsigned normU = static_cast<unsigned>(Normalize10bitWord(rawU, shU, dpU)) & 0x3FFu;
                const unsigned normV = static_cast<unsigned>(Normalize10bitWord(rawV, shV, dpV)) & 0x3FFu;
                YuvToBgrBt709Limited<uint16_t, 10>(static_cast<uint16_t>(normY), static_cast<uint16_t>(normU),
                    static_cast<uint16_t>(normV), row + x * 3);
            }
        }
    }

    // NV12/NV21: 8bit
    void ConvertNvToBgr8(uint8_t* bgr, const AVFrame* frame, bool nv21, YuvPixelFunc8bit toBgr) {
        const uint8_t* yp = frame->data[0];
        const uint8_t* inter = frame->data[1];
        const int ly = frame->linesize[0];
        const int luv = frame->linesize[1];
        const int w = frame->width;
        const int h = frame->height;
        const int uIndex = nv21 ? 1 : 0;
        const int vIndex = nv21 ? 0 : 1;
        for (int y = 0; y < h; ++y) {
            uint8_t* row = bgr + y * w * 3;
            const int y2 = y >> 1;
            for (int x = 0; x < w; ++x) {
                const int x2 = x >> 1;
                const uint8_t Y = yp[y * ly + x];
                const uint8_t U = inter[y2 * luv + x2 * 2 + uIndex];
                const uint8_t V = inter[y2 * luv + x2 * 2 + vIndex];
                toBgr(Y, U, V, row + x * 3);
            }
        }
    }

    // P010: NV12 相当の 10bit(高位に有効 bit、libavutil デスクリプタに従う)
    void ConvertP010ToBgr(uint8_t* bgr, const AVFrame* frame, bool pixelBigEndian, const AVPixFmtDescriptor* d) {
        const uint8_t* yPlane = frame->data[0];
        const uint8_t* uvPlane = frame->data[1];
        const int ly = frame->linesize[0];
        const int luv = frame->linesize[1];
        const unsigned shY = d->comp[0].shift;
        const unsigned dpY = d->comp[0].depth ? d->comp[0].depth : 10u;
        const unsigned shU = d->comp[1].shift;
        const unsigned dpU = d->comp[1].depth ? d->comp[1].depth : 10u;
        const unsigned shV = d->comp[2].shift;
        const unsigned dpV = d->comp[2].depth ? d->comp[2].depth : 10u;
        const int w = frame->width;
        const int h = frame->height;
        for (int y = 0; y < h; ++y) {
            uint8_t* row = bgr + y * w * 3;
            const int y2 = y >> 1;
            for (int x = 0; x < w; ++x) {
                const int x2 = x >> 1;
                const uint16_t rawY = ReadU16At(yPlane + ly * y + x * 2, pixelBigEndian);
                const uint16_t rawU = ReadU16At(uvPlane + luv * y2 + x2 * 4, pixelBigEndian);
                const uint16_t rawV = ReadU16At(uvPlane + luv * y2 + x2 * 4 + 2, pixelBigEndian);
                const unsigned normY = static_cast<unsigned>(Normalize10bitWord(rawY, shY, dpY)) & 0x3FFu;
                const unsigned normU = static_cast<unsigned>(Normalize10bitWord(rawU, shU, dpU)) & 0x3FFu;
                const unsigned normV = static_cast<unsigned>(Normalize10bitWord(rawV, shV, dpV)) & 0x3FFu;
                YuvToBgrBt709Limited<uint16_t, 10>(static_cast<uint16_t>(normY), static_cast<uint16_t>(normU),
                    static_cast<uint16_t>(normV), row + x * 3);
            }
        }
    }
#endif // defined(AMT_LOGO_GUI_WITHOUT_SWSCALE)

    void ConvertToRGB(uint8_t* rgb, AVFrame* frame) {
#if defined(AMT_LOGO_GUI_WITHOUT_SWSCALE)
        const auto fmt = static_cast<AVPixelFormat>(frame->format);
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
        if (d == nullptr) {
            THROW(FormatException, "Logo GUI プレビュー: pixfmtdescriptor が取得できません");
        }
        if (fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P) {
            const bool full = (fmt == AV_PIX_FMT_YUVJ420P);
            YuvPixelFunc8bit func = full ? YuvToBgrBt709Full : &YuvToBgrBt709Limited<uint8_t, 8>;
            ConvertYuv420pToBgr8(rgb, frame, func);
        } else if (fmt == AV_PIX_FMT_YUV420P10LE || fmt == AV_PIX_FMT_YUV420P10BE) {
            const bool be = (fmt == AV_PIX_FMT_YUV420P10BE);
            ConvertYuv420pPlanar10ToBgr(rgb, frame, be, d);
        } else if (fmt == AV_PIX_FMT_NV12) {
            ConvertNvToBgr8(rgb, frame, false, &YuvToBgrBt709Limited<uint8_t, 8>);
        } else if (fmt == AV_PIX_FMT_NV21) {
            ConvertNvToBgr8(rgb, frame, true, &YuvToBgrBt709Limited<uint8_t, 8>);
        } else if (fmt == AV_PIX_FMT_P010LE || fmt == AV_PIX_FMT_P010BE) {
            const bool be = (fmt == AV_PIX_FMT_P010BE);
            ConvertP010ToBgr(rgb, frame, be, d);
        } else {
            THROWF(FormatException,
                "Logo GUI プレビュー: 未対応のピクセル形式です(YUV420 8bit/10bit と NV12/NV21/P010)。format=%d",
                (int)fmt);
        }
#else
        uint8_t * outData[1] = { rgb };
        int outLinesize[1] = { width * 3 };
        sws_scale(swsctx, frame->data, frame->linesize, 0, height, outData, outLinesize);
#endif
    }

    bool DecodeOneFrame(int64_t startpos) {
        Frame frame;
        AVPacket packet = AVPacket();
        bool ok = false;
        while (av_read_frame(inputCtx(), &packet) == 0) {
            if (packet.stream_index == videoStream->index) {
                if (avcodec_send_packet(codecCtx(), &packet) != 0) {
                    THROW(FormatException, "avcodec_send_packet failed");
                }
                while (avcodec_receive_frame(codecCtx(), frame()) == 0) {
#if LIBAVUTIL_VERSION_MAJOR >= 58
                    const int key_frame = (frame()->flags & AV_FRAME_FLAG_KEY) != 0;
#else
                    const int key_frame = frame()->key_frame;
#endif
                    // 最初はIフレームまでスキップ
                    if (lastDecodeFrame != -1 || key_frame) {
                        if (frame()->width != width || frame()->height != height) {
                            init(frame());
                        }
                        prevframe = frame;
                        ok = true;
                    }
                }
            }
            int64_t packetpos = packet.pos;
            av_packet_unref(&packet);
            if (ok) {
                break;
            }
            if (startpos >= 0 && packetpos != -1) {
                if (packetpos - startpos > 50 * 1024 * 1024) {
                    // 50MB読んでもデコードできなかったら終了
                    return false;
                }
            }
        }
        return ok;
    }

    int64_t GetDurationTs() {
        if (videoStream->duration != AV_NOPTS_VALUE) {
            return videoStream->duration;
        }
        if (inputCtx()->duration != AV_NOPTS_VALUE) {
            return av_rescale_q(inputCtx()->duration, av_make_q(1, AV_TIME_BASE), videoStream->time_base);
        }
        return -1;
    }

    bool SeekTo(float pos) {
        if (pos < 0.0f) pos = 0.0f;
        if (pos > 1.0f) pos = 1.0f;
        int64_t fileOffset = int64_t(fileSize * pos);
        if (av_seek_frame(inputCtx(), -1, fileOffset, AVSEEK_FLAG_BYTE) < 0) {
            THROW(FormatException, "av_seek_frame failed");
        }
        avformat_flush(inputCtx());
        avcodec_flush_buffers(codecCtx());
        return true;
    }

public:
    GUIMediaFile(AMTContext& ctx, const tchar* filepath, int serviceid) :
        AMTObject(ctx),
        inputCtx(filepath),
        codecCtx(),
        videoStream(nullptr),
#if !defined(AMT_LOGO_GUI_WITHOUT_SWSCALE)
        swsctx(nullptr),
#endif
        lastDecodeFrame(-1),
        fileSize(0),
        prevframe(),
        width(-1),
        height(-1) {
        {
            File file(tstring(filepath), _T("rb"));
            fileSize = file.size();
        }
        if (avformat_find_stream_info(inputCtx(), NULL) < 0) {
            THROW(FormatException, "avformat_find_stream_info failed");
        }
        videoStream = av::GetVideoStream(inputCtx(), serviceid);
        if (videoStream == NULL) {
            THROW(FormatException, "Could not find video stream ...");
        }
        lastDecodeFrame = -1;
        MakeCodecContext();
        DecodeOneFrame(0);
    }

#if !defined(AMT_LOGO_GUI_WITHOUT_SWSCALE)
    ~GUIMediaFile() {
        sws_freeContext(swsctx);
        swsctx = nullptr;
    }
#endif

    void getFrame(uint8_t* rgb, int width, int height) {
        if (this->width == width && this->height && height) {
            ConvertToRGB(rgb, prevframe());
        }
    }

    bool decodeFrame(float pos, int* pwidth, int* pheight) {
        ctx.setError(Exception());
        try {
            SeekTo(pos);
            lastDecodeFrame = -1;
            int64_t startpos = -1;
            if (inputCtx()->pb) {
                startpos = avio_tell(inputCtx()->pb);
            }
            if (DecodeOneFrame(startpos)) {
                *pwidth = width;
                *pheight = height;
            }
            return true;
        } catch (const Exception& exception) {
            ctx.setError(exception);
        }
        return false;
    }
};

} // namespace av

extern "C" AMATSUKAZE_API void* MediaFile_Create(AMTContext * ctx, const tchar * filepath, int serviceid) {
    try {
        return new av::GUIMediaFile(*ctx, filepath, serviceid);
    } catch (const Exception& exception) {
        ctx->setError(exception);
    }
    return nullptr;
}
extern "C" AMATSUKAZE_API void MediaFile_Delete(av::GUIMediaFile * ptr) { delete ptr; }
extern "C" AMATSUKAZE_API int MediaFile_DecodeFrame(
    av::GUIMediaFile * ptr, float pos, int* pwidth, int* pheight) {
    return ptr->decodeFrame(pos, pwidth, pheight);
}
extern "C" AMATSUKAZE_API void MediaFile_GetFrame(
    av::GUIMediaFile * ptr, uint8_t * rgb, int width, int height) {
    ptr->getFrame(rgb, width, height);
}

namespace logo {

class GUILogoFile : public AMTObject {
    LogoHeader header;
    LogoDataParam logo;
public:
    GUILogoFile(AMTContext& ctx, const tchar* filename)
        : AMTObject(ctx)
        , logo(LogoData::Load(filename, &header), &header) {}

    int getWidth() { return header.w; }
    int getHeight() { return header.h; }
    int getX() { return header.imgx; }
    int getY() { return header.imgy; }
    int getImgWidth() { return header.imgw; }
    int getImgHeight() { return header.imgh; }
    int getServiceId() { return header.serviceId; }
    void setServiceId(int serviceId) { header.serviceId = serviceId; }
    const char* getName() { return header.name; }
    void setName(const char* name) { strcpy_s(header.name, name); }

    void getImage(uint8_t* rgb, int stride, uint8_t bg) {
        const float *logoAY = logo.GetA(PLANAR_Y);
        const float *logoBY = logo.GetB(PLANAR_Y);
        const float *logoAU = logo.GetA(PLANAR_U);
        const float *logoBU = logo.GetB(PLANAR_U);
        const float *logoAV = logo.GetA(PLANAR_V);
        const float *logoBV = logo.GetB(PLANAR_V);

        int widthUV = (header.w >> header.logUVx);

        float bgY = 0.2126f * bg + 0.7152f * bg + 0.0722f * bg;
        float bgU = 0.5389f * (bg - bgY);
        float bgV = 0.6350f * (bg - bgY);

        for (int y = 0; y < header.h; y++) {
            uint8_t* line = rgb + y * stride;
            for (int x = 0; x < header.w; x++) {
                int UVx = (x >> header.logUVx);
                int UVy = (y >> header.logUVy);
                float aY = logoAY[x + y * header.w];
                float bY = logoBY[x + y * header.w];
                float aU = logoAU[UVx + UVy * widthUV];
                float bU = logoBU[UVx + UVy * widthUV];
                float aV = logoAV[UVx + UVy * widthUV];
                float bV = logoBV[UVx + UVy * widthUV];
                float fY = ((bgY - bY * 255) / aY);
                float fU = ((bgU - bU * 255) / aU);
                float fV = ((bgV - bV * 255) / aV);
                // B
                line[x * 3 + 0] = (uint8_t)std::max(0.0f, std::min(255.0f, (fY + 1.8556f * fU + 0.5f)));
                // G
                line[x * 3 + 1] = (uint8_t)std::max(0.0f, std::min(255.0f, (fY - 0.187324f * fU - 0.468124f * fV + 0.5f)));
                // R
                line[x * 3 + 2] = (uint8_t)std::max(0.0f, std::min(255.0f, (fY + 1.5748f * fV + 0.5f)));
            }
        }
    }

    bool save(const tchar* filename) {
        try {
            logo.Save(filename, &header);
            return true;
        } catch (const Exception& exception) {
            ctx.setError(exception);
        }
        return false;
    }

    bool saveAviUtl(const tchar* filename) {
        try {
            logo.SaveAviUtl(filename, &header);
            return true;
        } catch (const Exception& exception) {
            ctx.setError(exception);
        }
        return false;
    }

    bool saveImageJpeg(const tchar* filename, int quality, uint8_t bg) {
        try {
            const int w = header.w;
            const int h = header.h;
            const int stride = w * 3;
            std::vector<uint8_t> rgb(stride * h);
            getImage(rgb.data(), stride, bg);

            std::vector<uint8_t> jpegData;
            if (!jpeg_utils::compressBGRToJpeg(rgb.data(), stride, w, h, quality, jpegData)) {
                ctx.setError(RuntimeException("JPEG圧縮に失敗しました"));
                return false;
            }

            File file(tstring(filename), _T("wb"));
            file.write(MemoryChunk(jpegData.data(), jpegData.size()));
            return true;
        } catch (const Exception& exception) {
            ctx.setError(exception);
        }
        return false;
    }
};

} // namespace logo

static inline bool ParseResolutionFromPath(const tstring& path, int& w, int& h) {
    w = 0; h = 0;
    const auto len = (int)path.size();
    for (int i = 0; i < len; i++) {
        // 数字開始を探す
        if (path[i] < _T('0') || path[i] > _T('9')) continue;
        int j = i;
        int a = 0;
        while (j < len && path[j] >= _T('0') && path[j] <= _T('9')) {
            a = a * 10 + (path[j] - _T('0'));
            j++;
            if (a > 100000) break;
        }
        if (j >= len) continue;
        if (path[j] != _T('x') && path[j] != _T('X')) continue;
        j++;
        if (j >= len || path[j] < _T('0') || path[j] > _T('9')) continue;
        int b = 0;
        while (j < len && path[j] >= _T('0') && path[j] <= _T('9')) {
            b = b * 10 + (path[j] - _T('0'));
            j++;
            if (b > 100000) break;
        }
        if (a >= 16 && b >= 16) {
            w = a; h = b;
            return true;
        }
    }
    return false;
}

extern "C" AMATSUKAZE_API int LogoFile_ConvertAviUtlToExtended(
    AMTContext * ctx, const tchar * srcpath, const tchar * dstpath, int serviceId, int imgw, int imgh) {
    try {
        // AviUtlベース形式(.lgd)を読み込み、Amatsukaze拡張ヘッダ+float配列を付与して保存する。
        // 変換結果は近似（ロスレスではない）が、「それっぽく」表示・利用できることを目的とする。

        // 入力ファイルを丸ごと読み込み（ベース部分はそのまま保持して末尾に拡張を追加する）
        File srcFile(tstring(srcpath), _T("rb"));
        const int64_t totalSize = srcFile.size();
        if (totalSize <= 0) {
            THROWF(IOException, "ロゴファイルが空です: %s", tstring(srcpath));
        }
        std::vector<uint8_t> baseBytes((size_t)totalSize);
        if (srcFile.read(MemoryChunk(baseBytes.data(), baseBytes.size())) != baseBytes.size()) {
            THROWF(IOException, "failed to read from file: %s", tstring(srcpath));
        }

        // ベース形式のヘッダ/ピクセルを解釈
        if (baseBytes.size() < sizeof(LOGO_FILE_HEADER) + sizeof(LOGO_HEADER)) {
            THROWF(IOException, "ロゴファイル形式が不正です(サイズ不足): %s", tstring(srcpath));
        }

        LOGO_HEADER baseH = { 0 };
        memcpy(&baseH, baseBytes.data() + sizeof(LOGO_FILE_HEADER), sizeof(LOGO_HEADER));
        const size_t pixelBytes = (size_t)LOGO_PIXELSIZE(&baseH);
        const size_t baseMinSize = sizeof(LOGO_FILE_HEADER) + sizeof(LOGO_HEADER) + pixelBytes;
        if (baseBytes.size() < baseMinSize) {
            // ベース部分すら満たしていない
            THROWF(IOException, "ロゴファイル形式が不正です(ピクセル不足): %s", tstring(srcpath));
        }

        const LOGO_PIXEL* pixels = reinterpret_cast<const LOGO_PIXEL*>(baseBytes.data() + sizeof(LOGO_FILE_HEADER) + sizeof(LOGO_HEADER));
        const int wOrg = baseH.w;
        const int hOrg = baseH.h;
        int w = wOrg;
        int h = hOrg;
        if (w <= 0 || h <= 0 || (size_t)w * (size_t)h > 20000 * 20000) {
            THROWF(IOException, "ロゴサイズが不正です: %s", tstring(srcpath));
        }

        // ロゴサイズが奇数の場合、UV(2x2)前提の処理で端1ラインが破綻しやすいので、偶数へ切り落とす
        // （ベース部ヘッダ/ピクセルも偶数サイズに作り直す）
        std::vector<uint8_t> outBaseBytes;
        if ((w & 1) || (h & 1)) {
            const int wEven = w & ~1;
            const int hEven = h & ~1;
            if (wEven <= 0 || hEven <= 0) {
                THROWF(IOException, "ロゴサイズが不正です: %s", tstring(srcpath));
            }

            // ヘッダを更新
            LOGO_HEADER baseH2 = baseH;
            baseH2.w = (short)wEven;
            baseH2.h = (short)hEven;

            // 出力用ベース部を再構築（LOGO_FILE_HEADER + LOGO_HEADER + LOGO_PIXEL配列）
            const size_t newPixelBytes = (size_t)wEven * (size_t)hEven * sizeof(LOGO_PIXEL);
            outBaseBytes.resize(sizeof(LOGO_FILE_HEADER) + sizeof(LOGO_HEADER) + newPixelBytes);
            memcpy(outBaseBytes.data(), baseBytes.data(), sizeof(LOGO_FILE_HEADER));
            memcpy(outBaseBytes.data() + sizeof(LOGO_FILE_HEADER), &baseH2, sizeof(LOGO_HEADER));

            // ピクセルを行単位でコピー（右端・下端を切り落とす）
            auto* dstPix = reinterpret_cast<LOGO_PIXEL*>(outBaseBytes.data() + sizeof(LOGO_FILE_HEADER) + sizeof(LOGO_HEADER));
            for (int yy = 0; yy < hEven; yy++) {
                memcpy(dstPix + (size_t)yy * (size_t)wEven,
                    pixels + (size_t)yy * (size_t)wOrg,
                    (size_t)wEven * sizeof(LOGO_PIXEL));
            }

            // 以降の処理も偶数サイズに合わせる
            w = wEven;
            h = hEven;
            pixels = dstPix;
        } else {
            outBaseBytes = baseBytes;
        }

        // 拡張ヘッダを構築
        logo::LogoHeader ext;
        ext.magic = 0x12345;
        ext.version = 1;
        ext.w = w;
        ext.h = h;
        ext.logUVx = 1;
        ext.logUVy = 1;

        // imgw/imgh は呼び出し側（GUI）でユーザー入力させた値を優先する。
        // 不正値(<=0)の場合のみ、ファイル名から 1920x1080 のような形式を拾い、それも無ければ 1920x1080 を仮定。
        if (imgw <= 0 || imgh <= 0) {
            imgw = 1920;
            imgh = 1080;
            int parsedW = 0, parsedH = 0;
            if (ParseResolutionFromPath(tstring(srcpath), parsedW, parsedH)) {
                imgw = parsedW;
                imgh = parsedH;
            }
        }
        // 偶数に丸め
        imgw = std::max(2, (imgw / 2) * 2);
        imgh = std::max(2, (imgh / 2) * 2);

        ext.imgw = imgw;
        ext.imgh = imgh;
        ext.imgx = baseH.x;
        ext.imgy = baseH.y;

        memset(ext.name, 0, sizeof(ext.name));
        // baseH.name は最大32byte
        strncpy_s(ext.name, baseH.name, sizeof(ext.name) - 1);

        ext.serviceId = serviceId;
        memset(ext.reserved, 0, sizeof(ext.reserved));

        // 近似float配列生成
        const int wUV = w >> ext.logUVx;
        const int hUV = h >> ext.logUVy;
        const size_t floatsCount = (size_t)(w * h + wUV * hUV * 2) * 2;
        std::vector<float> data(floatsCount, 0.0f);
        float* aY = data.data();
        float* bY = aY + w * h;
        float* aU = bY + w * h;
        float* bU = aU + wUV * hUV;
        float* aV = bU + wUV * hUV;
        float* bV = aV + wUV * hUV;

        // LOGO_PIXEL(dp/y/cb/cr) -> 拡張float(a/b) 変換
        // NOTE: 拡張形式は内部的に「背景 = a * 前景 + b * maxv」の係数(a,b)を保持している。
        // AviUtl形式は(dp, y/cb/cr)で量子化された値なので、ここでは ToOutLGP の逆を近似して復元する。

        auto clampf = [](float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); };
        auto clamp01 = [&](float v) { return clampf(v, 0.0f, 1.0f); };

        auto toYV12Y = [](int yc48) -> float {
            // logo::LogoData::ToYV12Y と同等の式をfloatで計算（中間も含めて整数切り捨てを行わない）
            // 元: (((yc48 * 219 + 383) >> 12) + 16) / 255
            return (((yc48 * 219.0f + 383.0f) * (1.0f / 4096.0f)) + 16.0f) * (1.0f / 255.0f);
        };
        auto toYV12C = [](int yc48) -> float {
            // logo::LogoData::ToYV12C と同等の式をfloatで計算（中間も含めて整数切り捨てを行わない）
            // 元: (((((yc48 + 2048) * 7 + 66) >> 7) + 16) / 255
            return ((((yc48 + 2048.0f) * 7.0f + 66.0f) * (1.0f / 128.0f) + 16.0f)) * (1.0f / 255.0f);
        };
        auto invToYC48Y = [](float yc48) -> float {
            // logo::LogoData::ToYC48Y の近似逆変換
            // yc48 ≈ (yv12*255)*1197/64 - 299  => yv12 ≈ ((yc48+299)*64/1197)/255
            return ((yc48 + 299.0f) * 64.0f * (1.0f / 1197.0f)) * (1.0f / 255.0f);
        };
        auto invToYC48C = [](float yc48) -> float {
            // logo::LogoData::ToYC48C の近似逆変換（丸め/オフセットは近似）
            // yc48 ≈ ((yv12*255 - 128)*4681)/256  => yv12 ≈ (yc48*256/4681 + 128)/255
            return (yc48 * 256.0f * (1.0f / 4681.0f) + 128.0f) * (1.0f / 255.0f);
        };

        const float x0Y = toYV12Y(0);
        const float x1Y = toYV12Y(2048);
        const float x0C = toYV12C(0);
        const float x1C = toYV12C(2048);

        auto invertAB_Y = [&](float A_yc48, float B_yc48, float& a, float& b) {
            // YC48ドメインのA,B（y = A*x + B, xは0..2048）を内部(YV12正規化)のA,Bへ近似変換
            const float y0c = B_yc48;
            const float y1c = A_yc48 * 2048.0f + B_yc48;
            const float y0 = invToYC48Y(y0c);
            const float y1 = invToYC48Y(y1c);
            const float dy = (y1 - y0);
            if (std::abs(dy) < 1e-6f) {
                a = 1.0f; b = 0.0f;
                return;
            }
            a = (x1Y - x0Y) / dy;
            b = x0Y - a * y0;
            // 極端値を抑制（プレビュー/除去を破綻させない）
            a = clampf(a, 0.001f, 4.0f);
            b = clampf(b, -2.0f, 2.0f);
        };
        auto invertAB_C = [&](float A_yc48, float B_yc48, float& a, float& b) {
            const float y0c = B_yc48;
            const float y1c = A_yc48 * 2048.0f + B_yc48;
            const float y0 = invToYC48C(y0c);
            const float y1 = invToYC48C(y1c);
            const float dy = (y1 - y0);
            if (std::abs(dy) < 1e-6f) {
                a = 1.0f; b = 0.0f;
                return;
            }
            a = (x1C - x0C) / dy;
            b = x0C - a * y0;
            a = clampf(a, 0.001f, 4.0f);
            b = clampf(b, -2.0f, 2.0f);
        };

        auto fromLGP_Y = [&](const LOGO_PIXEL& p, float& a, float& b) {
            const float dp = clampf((float)p.dp_y, 0.0f, (float)LOGO_MAX_DP);
            if (dp <= 0.0f) { a = 1.0f; b = 0.0f; return; }
            const float dp01 = dp / (float)LOGO_MAX_DP;
            const float A_yc48 = 1.0f - dp01;
            const float B_yc48 = ((float)p.y - 0.5f) * dp01;
            invertAB_Y(A_yc48, B_yc48, a, b);
        };
        auto fromLGP_Cb = [&](const LOGO_PIXEL& p, float& a, float& b) {
            const float dp = clampf((float)p.dp_cb, 0.0f, (float)LOGO_MAX_DP);
            if (dp <= 0.0f) { a = 1.0f; b = 0.0f; return; }
            const float dp01 = dp / (float)LOGO_MAX_DP;
            const float A_yc48 = 1.0f - dp01;
            const float B_yc48 = ((float)p.cb - 0.5f) * dp01;
            invertAB_C(A_yc48, B_yc48, a, b);
        };
        auto fromLGP_Cr = [&](const LOGO_PIXEL& p, float& a, float& b) {
            const float dp = clampf((float)p.dp_cr, 0.0f, (float)LOGO_MAX_DP);
            if (dp <= 0.0f) { a = 1.0f; b = 0.0f; return; }
            const float dp01 = dp / (float)LOGO_MAX_DP;
            const float A_yc48 = 1.0f - dp01;
            const float B_yc48 = ((float)p.cr - 0.5f) * dp01;
            invertAB_C(A_yc48, B_yc48, a, b);
        };

        // Y: ピクセル単位（LOGO_PIXELのdp/yから逆算）
        for (int y0 = 0; y0 < h; y0++) {
            for (int x0 = 0; x0 < w; x0++) {
                float a = 1.0f, b = 0.0f;
                fromLGP_Y(pixels[x0 + y0 * w], a, b);
                aY[x0 + y0 * w] = a;
                bY[x0 + y0 * w] = b;
            }
        }

        // U/V: 2x2ブロック平均（logUVx=1, logUVy=1 前提）
        for (int uvY = 0; uvY < hUV; uvY++) {
            for (int uvX = 0; uvX < wUV; uvX++) {
                float sumAU = 0.0f, sumBU = 0.0f;
                float sumAV = 0.0f, sumBV = 0.0f;
                int cnt = 0;
                for (int dy = 0; dy < (1 << ext.logUVy); dy++) {
                    for (int dx = 0; dx < (1 << ext.logUVx); dx++) {
                        const int x0 = (uvX << ext.logUVx) + dx;
                        const int y0 = (uvY << ext.logUVy) + dy;
                        if (x0 < w && y0 < h) {
                            float a = 1.0f, b = 0.0f;
                            fromLGP_Cb(pixels[x0 + y0 * w], a, b);
                            sumAU += a; sumBU += b;
                            fromLGP_Cr(pixels[x0 + y0 * w], a, b);
                            sumAV += a; sumBV += b;
                            cnt++;
                        }
                    }
                }
                if (cnt <= 0) cnt = 1;
                const int idx = uvX + uvY * wUV;
                aU[idx] = clampf(sumAU / (float)cnt, 0.001f, 4.0f);
                bU[idx] = clampf(sumBU / (float)cnt, -2.0f, 2.0f);
                aV[idx] = clampf(sumAV / (float)cnt, 0.001f, 4.0f);
                bV[idx] = clampf(sumBV / (float)cnt, -2.0f, 2.0f);
            }
        }

        // 出力: ベースをコピーし、末尾に拡張ヘッダ+float配列を追加
        File dstFile(tstring(dstpath), _T("wb"));
        dstFile.write(MemoryChunk(outBaseBytes.data(), outBaseBytes.size()));
        dstFile.writeValue(ext);
        dstFile.write(MemoryChunk((uint8_t*)data.data(), data.size() * sizeof(float)));
        return 1;
    } catch (const Exception& exception) {
        ctx->setError(exception);
    }
    return 0;
}

extern "C" AMATSUKAZE_API void* LogoFile_Create(AMTContext * ctx, const tchar * filepath) {
    try {
        return new logo::GUILogoFile(*ctx, filepath);
    } catch (const Exception& exception) {
        ctx->setError(exception);
    }
    return nullptr;
}
extern "C" AMATSUKAZE_API void LogoFile_Delete(logo::GUILogoFile * ptr) { delete ptr; }
extern "C" AMATSUKAZE_API int LogoFile_GetWidth(logo::GUILogoFile * ptr) { return ptr->getWidth(); }
extern "C" AMATSUKAZE_API int LogoFile_GetHeight(logo::GUILogoFile * ptr) { return ptr->getHeight(); }
extern "C" AMATSUKAZE_API int LogoFile_GetX(logo::GUILogoFile * ptr) { return ptr->getX(); }
extern "C" AMATSUKAZE_API int LogoFile_GetY(logo::GUILogoFile * ptr) { return ptr->getY(); }
extern "C" AMATSUKAZE_API int LogoFile_GetImgWidth(logo::GUILogoFile * ptr) { return ptr->getImgWidth(); }
extern "C" AMATSUKAZE_API int LogoFile_GetImgHeight(logo::GUILogoFile * ptr) { return ptr->getImgHeight(); }
extern "C" AMATSUKAZE_API int LogoFile_GetServiceId(logo::GUILogoFile * ptr) { return ptr->getServiceId(); }
extern "C" AMATSUKAZE_API void LogoFile_SetServiceId(logo::GUILogoFile * ptr, int serviceId) { ptr->setServiceId(serviceId); }
extern "C" AMATSUKAZE_API const char* LogoFile_GetName(logo::GUILogoFile * ptr) { return ptr->getName(); }
extern "C" AMATSUKAZE_API void LogoFile_SetName(logo::GUILogoFile * ptr, const char* name) { ptr->setName(name); }
extern "C" AMATSUKAZE_API void LogoFile_GetImage(logo::GUILogoFile * ptr, uint8_t * rgb, int stride, uint8_t bg) { ptr->getImage(rgb, stride, bg); }
extern "C" AMATSUKAZE_API int LogoFile_Save(logo::GUILogoFile * ptr, const tchar * filename) { return ptr->save(filename); }
extern "C" AMATSUKAZE_API int LogoFile_SaveAviUtl(logo::GUILogoFile * ptr, const tchar * filename) { return ptr->saveAviUtl(filename); }
extern "C" AMATSUKAZE_API int LogoFile_SaveImageJpeg(logo::GUILogoFile * ptr, const tchar * filename, int quality, uint8_t bg) { return ptr->saveImageJpeg(filename, quality, bg); }
