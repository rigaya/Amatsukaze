/**
* Amatsukaze Trim Viewer GUI Support
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "AMTSource.h"
#include "JpegCompress.h"

#include <mutex>

// Amatsukaze.cppで定義されたAviSynthプラグイン初期化関数の前方宣言
// AMTSourceフィルタをAviSynth環境に登録するために使用
extern "C" AMATSUKAZE_API const char*
#if defined(_WIN32) || defined(_WIN64)
__stdcall
#endif
AvisynthPluginInit3(IScriptEnvironment* env, const AVS_Linkage* const vectors);

namespace trimadjust {

// amts0.datを読み込みAviSynth経由でフレーム取得を行うクラス
class GUITrimAdjust : public AMTObject {
    // amts0.datから読み込んだデータ
    std::vector<FilterSourceFrame> frames;
    VideoFormat vfmt;

    // AviSynth環境
    IScriptEnvironment2* env;
    PClip yuvClip; // ConvertToYV16適用済みクリップ (planar YUV422P)
    int width, height;
    int numFrames;

    // JPEG出力バッファ（getFrameJpeg呼び出し間で再利用）
    static constexpr int JPEG_QUALITY = 85;
    std::vector<uint8_t> jpegBuffer;

    std::mutex mutex;

    // AviSynth環境を作成しAMTSourceプラグイン経由でクリップを構築
    void initAviSynth(const tstring& datFilePath, int scaleMode) {
        env = CreateScriptEnvironment2();
        if (env == nullptr) {
            THROW(RuntimeException, "AviSynth環境の作成に失敗しました");
        }

        try {
            // AMTSourceフィルタを環境に登録（同一DLL内のAvisynthPluginInit3を直接呼ぶ）
            AvisynthPluginInit3(env, nullptr);

            // AMTSourceクリップを作成
            AVSValue loadArgs[] = { datFilePath.c_str(), "", false, 0 };
            const char* loadNames[] = { nullptr, nullptr, nullptr, nullptr };
            AVSValue amtClip = env->Invoke("AMTSource", AVSValue(loadArgs, 4), loadNames);
            PClip srcClip = amtClip.AsClip();

            const VideoInfo& srcVi = srcClip->GetVideoInfo();
            numFrames = srcVi.num_frames;
            width = srcVi.width;
            height = srcVi.height;

            // インタレース判定に基づきConvertToYV16 (planar YUV422P) を適用
            const bool interlaced = !vfmt.progressive;
            AVSValue convertArgs[] = { amtClip, interlaced };
            const char* convertNames[] = { nullptr, "interlaced" };
            AVSValue yuvResult = env->Invoke("ConvertToYV16", AVSValue(convertArgs, 2), convertNames);
            PClip convertedClip = yuvResult.AsClip();

            // scaleMode==1: フィールド分離→リサイズ→フィールドマージ
            if (scaleMode == 1 && interlaced) {
                // SeparateFields
                AVSValue sepArgs[] = { yuvResult };
                AVSValue sepResult = env->Invoke("SeparateFields", AVSValue(sepArgs, 1));

                // 元の解像度にBicubicResize
                AVSValue resizeArgs[] = { sepResult, width, height / 2 };
                AVSValue resizeResult = env->Invoke("BicubicResize", AVSValue(resizeArgs, 3));

                // Weave（フィールドマージ）
                AVSValue weaveArgs[] = { resizeResult };
                AVSValue weaveResult = env->Invoke("Weave", AVSValue(weaveArgs, 1));

                yuvClip = weaveResult.AsClip();
            } else {
                yuvClip = convertedClip;
            }

            // 最終的なサイズを取得
            const VideoInfo& finalVi = yuvClip->GetVideoInfo();
            width = finalVi.width;
            height = finalVi.height;
        } catch (const AvisynthError& e) {
            if (env) {
                env->DeleteScriptEnvironment();
                env = nullptr;
            }
            THROWF(RuntimeException, "AviSynthエラー: %s", e.msg);
        }
    }

public:
    GUITrimAdjust(AMTContext& ctx, const tchar* datFilePath, int scaleMode)
        : AMTObject(ctx)
        , env(nullptr)
        , yuvClip()
        , width(0)
        , height(0)
        , numFrames(0) {
        // amts0.datを読み込んでフレーム情報とVideoFormatを取得
        tstring path(datFilePath);
        File file(path, _T("rb"));
        auto srcpathv = file.readArray<tchar>();
        auto audiopathv = file.readArray<tchar>();
        vfmt = file.readValue<VideoFormat>();
        // AudioFormatは読み飛ばし
        file.readValue<AudioFormat>();
        frames = file.readArray<FilterSourceFrame>();
        // audioFrames, decoderSettingは不要だが読み飛ばし
        file.readArray<FilterAudioFrame>();
        file.readValue<DecoderSetting>();

        // AviSynth環境の初期化
        initAviSynth(path, scaleMode);
    }

    ~GUITrimAdjust() {
        yuvClip = PClip();
        if (env) {
            env->DeleteScriptEnvironment();
            env = nullptr;
        }
    }

    int getNumFrames() const { return numFrames; }
    int getWidth() const { return width; }
    int getHeight() const { return height; }

    // フレームをデコードし、サイズを返す
    bool decodeFrame(int frameNumber, int* pwidth, int* pheight) {
        std::lock_guard<std::mutex> lock(mutex);
        ctx.setError(Exception());
        try {
            if (frameNumber < 0 || frameNumber >= numFrames) {
                return false;
            }
            *pwidth = width;
            *pheight = height;
            return true;
        } catch (const Exception& exception) {
            ctx.setError(exception);
        }
        return false;
    }

    // デコード済みフレームをJPEGに圧縮して内部バッファに格納
    // 成功時は jpegData にポインタ、jpegSize にサイズを返す
    bool getFrameJpeg(int frameNumber, const uint8_t** jpegData, int* jpegSize) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!yuvClip) {
            return false;
        }
        try {
            PVideoFrame frame = yuvClip->GetFrame(frameNumber, env);
            // YV16 (planar YUV422P) の各プレーンを取得
            const uint8_t* planes[3] = {
                frame->GetReadPtr(PLANAR_Y),
                frame->GetReadPtr(PLANAR_U),
                frame->GetReadPtr(PLANAR_V),
            };
            const int strides[3] = {
                frame->GetPitch(PLANAR_Y),
                frame->GetPitch(PLANAR_U),
                frame->GetPitch(PLANAR_V),
            };
            if (!jpeg_utils::compressYUV422PToJpeg(planes, strides, width, height, JPEG_QUALITY, jpegBuffer)) {
                return false;
            }
            *jpegData = jpegBuffer.data();
            *jpegSize = static_cast<int>(jpegBuffer.size());
            return true;
        } catch (const AvisynthError&) {
            return false;
        }
    }

    // フレームのメタ情報を取得
    bool getFrameInfo(int frameNumber, int64_t* pts, int64_t* duration,
                      int* keyFrame, int* cmType) const {
        if (frameNumber < 0 || frameNumber >= (int)frames.size()) {
            return false;
        }
        const auto& f = frames[frameNumber];
        *pts = f.framePTS;
        // durationはframeDurationをそのまま使う（内部用だがPTS間隔として利用可能）
        // 90kHzタイムスタンプ単位
        if (frameNumber + 1 < (int)frames.size()) {
            *duration = frames[frameNumber + 1].framePTS - f.framePTS;
        } else {
            // 最終フレームは直前のフレームのdurationを使う
            if (frameNumber > 0) {
                *duration = f.framePTS - frames[frameNumber - 1].framePTS;
            } else {
                *duration = 0;
            }
        }
        *keyFrame = f.keyFrame;
        *cmType = (int)f.cmType;
        return true;
    }
};

} // namespace trimadjust

// DLLエクスポート関数

extern "C" AMATSUKAZE_API void* TrimAdjust_Create(AMTContext* ctx, const tchar* datFilePath, int scaleMode) {
    try {
        return new trimadjust::GUITrimAdjust(*ctx, datFilePath, scaleMode);
    } catch (const Exception& exception) {
        ctx->setError(exception);
    }
    return nullptr;
}

extern "C" AMATSUKAZE_API void TrimAdjust_Delete(void* ptr) {
    delete static_cast<trimadjust::GUITrimAdjust*>(ptr);
}

extern "C" AMATSUKAZE_API int TrimAdjust_GetNumFrames(void* ptr) {
    return static_cast<trimadjust::GUITrimAdjust*>(ptr)->getNumFrames();
}

extern "C" AMATSUKAZE_API int TrimAdjust_GetWidth(void* ptr) {
    return static_cast<trimadjust::GUITrimAdjust*>(ptr)->getWidth();
}

extern "C" AMATSUKAZE_API int TrimAdjust_GetHeight(void* ptr) {
    return static_cast<trimadjust::GUITrimAdjust*>(ptr)->getHeight();
}

extern "C" AMATSUKAZE_API int TrimAdjust_DecodeFrame(void* ptr, int frameNumber, int* pwidth, int* pheight) {
    return static_cast<trimadjust::GUITrimAdjust*>(ptr)->decodeFrame(frameNumber, pwidth, pheight) ? 1 : 0;
}

extern "C" AMATSUKAZE_API int TrimAdjust_GetFrameJpeg(void* ptr, int frameNumber,
    const uint8_t** jpegData, int* jpegSize) {
    return static_cast<trimadjust::GUITrimAdjust*>(ptr)->getFrameJpeg(frameNumber, jpegData, jpegSize) ? 1 : 0;
}

extern "C" AMATSUKAZE_API int TrimAdjust_GetFrameInfo(void* ptr, int frameNumber,
    int64_t* pts, int64_t* duration, int* keyFrame, int* cmType) {
    return static_cast<trimadjust::GUITrimAdjust*>(ptr)->getFrameInfo(frameNumber, pts, duration, keyFrame, cmType) ? 1 : 0;
}
