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

    // amts0.datから読み込んだ音声データ（波形表示用）
    std::vector<FilterAudioFrame> audioFrames;
    AudioFormat afmt;
    tstring audioFilePath; // waveファイルパス
    int audioSamplesPerFrame; // 通常1024(AAC)

    // JPEG出力バッファ（getFrameJpeg呼び出し間で再利用）
    static constexpr int JPEG_QUALITY = 85;
    std::vector<uint8_t> jpegBuffer;

    // 波形レンダリング定数・バッファ
    static constexpr int WAVEFORM_HEIGHT = 56;
    static constexpr int WAVEFORM_JPEG_QUALITY = 85;
    static constexpr double WAVEFORM_WINDOW_HALF = 0.25; // 前後0.25秒（計約0.5秒）
    // 背景: 濃いグレー #404040 → YCbCr(64, 128, 128)
    static constexpr uint8_t WF_BG_Y = 64, WF_BG_CB = 128, WF_BG_CR = 128;
    // 波形: 薄い水色 #80C8E8 → YCbCr(183, 156, 109)
    static constexpr uint8_t WF_FG_Y = 183, WF_FG_CB = 156, WF_FG_CR = 109;
    // センターライン: グレー #808080 → YCbCr(128, 128, 128)
    static constexpr uint8_t WF_CL_Y = 128, WF_CL_CB = 128, WF_CL_CR = 128;
    std::vector<uint8_t> waveformJpegBuffer;

    std::mutex mutex;

    // AviSynth環境を作成しAMTSourceプラグイン経由でクリップを構築
    void initAviSynth(const tstring& datFilePath, int scaleMode) {
        env = CreateScriptEnvironment2();
        if (env == nullptr) {
            THROW(RuntimeException, "AviSynth環境の作成に失敗しました");
        }

        try {
            // 他のAviSynth経路と同じくDLLをLoadPluginしてAMTSourceを登録する
            AVSValue loadPluginResult;
            env->LoadPlugin(tchar_to_string(GetModulePath()).c_str(), true, &loadPluginResult);

            // AMTSourceクリップを作成
            const auto amtPath = tchar_to_string(datFilePath);
            AVSValue loadArgs[] = { amtPath.c_str(), "", false, 0 };
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

            // scaleMode!=0: 縮小プレビュー
            int scaleNum = 1;
            int scaleDen = 1;
            if (scaleMode == 1) {
                scaleNum = 1;
                scaleDen = 2;
            } else if (scaleMode == 2) {
                scaleNum = 2;
                scaleDen = 3;
            }

            if (scaleDen > 1) {
                const int resizeWidth = std::max(1, (width * scaleNum) / scaleDen);
                const int resizeHeight = std::max(1, (height * scaleNum) / scaleDen);
                if (interlaced) {
                    // interlaced=true: SeparateFields -> BilinearResize -> Weave
                    AVSValue sepArgs[] = { yuvResult };
                    AVSValue sepResult = env->Invoke("SeparateFields", AVSValue(sepArgs, 1));

                    const int resizeFieldHeight = std::max(1, resizeHeight / 2);
                    AVSValue resizeArgs[] = { sepResult, resizeWidth, resizeFieldHeight };
                    AVSValue resizeResult = env->Invoke("BilinearResize", AVSValue(resizeArgs, 3));

                    AVSValue weaveArgs[] = { resizeResult };
                    AVSValue weaveResult = env->Invoke("Weave", AVSValue(weaveArgs, 1));
                    yuvClip = weaveResult.AsClip();
                } else {
                    // interlaced=false: 直接BilinearResize
                    AVSValue resizeArgs[] = { yuvResult, resizeWidth, resizeHeight };
                    AVSValue resizeResult = env->Invoke("BilinearResize", AVSValue(resizeArgs, 3));
                    yuvClip = resizeResult.AsClip();
                }
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
        , numFrames(0)
        , audioSamplesPerFrame(0) {
        // amts0.datを読み込んでフレーム情報とVideoFormatを取得
        tstring path(datFilePath);
        File file(path, _T("rb"));
        auto srcpathv = file.readArray<tchar>();
        auto audiopathv = file.readArray<tchar>();
        audioFilePath = tstring(audiopathv.begin(), audiopathv.end());
        vfmt = file.readValue<VideoFormat>();
        afmt = file.readValue<AudioFormat>();
        frames = file.readArray<FilterSourceFrame>();
        audioFrames = file.readArray<FilterAudioFrame>();
        file.readValue<DecoderSetting>();

        // audioSamplesPerFrameを算出 (AMTSource.cppと同じロジック)
        if (!audioFrames.empty()) {
            audioSamplesPerFrame = 1024; // AACデフォルト
            for (const auto& af : audioFrames) {
                if (af.waveLength != 0) {
                    audioSamplesPerFrame = af.waveLength / 4; // 16bitステレオ前提
                    break;
                }
            }
        }

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

    // フレームnに対応する音声波形をJPEG画像として取得
    // 前後WAVEFORM_WINDOW_HALF秒の範囲をレンダリングし、中央にセンターラインを描画
    bool getWaveformJpeg(int frameNumber, const uint8_t** jpegData, int* jpegSize) {
        std::lock_guard<std::mutex> lock(mutex);
        if (audioFrames.empty() || afmt.sampleRate == 0 || audioFilePath.empty()) {
            return false; // 音声データなし
        }
        if (frameNumber < 0 || frameNumber >= (int)frames.size()) {
            return false;
        }

        // 1. フレームnの時間位置（秒）を算出
        const double basePts = (!frames.empty()) ? (double)frames[0].framePTS : 0.0;
        const double frameSec = ((double)frames[frameNumber].framePTS - basePts) / 90000.0;
        double frameDurSec;
        if (frameNumber + 1 < (int)frames.size()) {
            frameDurSec = ((double)frames[frameNumber + 1].framePTS
                         - (double)frames[frameNumber].framePTS) / 90000.0;
        } else {
            frameDurSec = (frameNumber > 0)
                ? ((double)frames[frameNumber].framePTS
                 - (double)frames[frameNumber - 1].framePTS) / 90000.0
                : 1.0 / 30.0;
        }

        // 2. 表示範囲: 前後WAVEFORM_WINDOW_HALF秒、フレーム中心
        const double centerSec = frameSec + frameDurSec * 0.5;
        const double startSec = std::max(0.0, centerSec - WAVEFORM_WINDOW_HALF);
        const double endSec = centerSec + WAVEFORM_WINDOW_HALF;

        // 3. サンプル範囲を算出
        const int totalAudioSamples = audioSamplesPerFrame * (int)audioFrames.size();
        int sampleStart = clamp((int)(startSec * afmt.sampleRate), 0, totalAudioSamples);
        int sampleEnd   = clamp((int)(endSec   * afmt.sampleRate), 0, totalAudioSamples);
        if (sampleStart >= sampleEnd) return false;
        const int sampleCount = sampleEnd - sampleStart;

        // 4. waveファイルからPCMサンプルを読み込み（audioFrame単位でまとめて読む）
        const int sampleBytes = 4; // 16bitステレオ: int16_t L + int16_t R
        std::vector<int16_t> monoSamples(sampleCount, 0);
        {
            File waveFile(audioFilePath, _T("rb"));
            const int afIdxStart = sampleStart / audioSamplesPerFrame;
            const int afIdxEnd = std::min((sampleEnd - 1) / audioSamplesPerFrame + 1,
                                          (int)audioFrames.size());
            // audioFrame単位でバッファに読み込み
            std::vector<int16_t> frameBuf(audioSamplesPerFrame * 2); // L,R交互
            for (int afIdx = afIdxStart; afIdx < afIdxEnd; afIdx++) {
                const auto& af = audioFrames[afIdx];
                const int afSampleStart = afIdx * audioSamplesPerFrame;
                const int afSampleEnd = afSampleStart + audioSamplesPerFrame;
                // 表示範囲とaudioFrameの重複部分
                const int overlapStart = std::max(sampleStart, afSampleStart);
                const int overlapEnd   = std::min(sampleEnd, afSampleEnd);
                if (overlapStart >= overlapEnd) continue;

                if (af.waveLength != 0) {
                    const int ofsInFrame = overlapStart - afSampleStart;
                    const int readSamples = overlapEnd - overlapStart;
                    waveFile.seek(af.waveOffset + ofsInFrame * sampleBytes, SEEK_SET);
                    waveFile.read(MemoryChunk(
                        (uint8_t*)frameBuf.data(), readSamples * sampleBytes));
                    // L+R → mono
                    for (int i = 0; i < readSamples; i++) {
                        monoSamples[overlapStart - sampleStart + i] =
                            (int16_t)(((int)frameBuf[i * 2] + (int)frameBuf[i * 2 + 1]) / 2);
                    }
                }
                // waveLength==0 のフレームは無音(0のまま)
            }
        }

        // 5. YUV444平面バッファに波形を描画
        const int wfWidth = width;
        const int wfHeight = WAVEFORM_HEIGHT;
        std::vector<uint8_t> yPlane(wfWidth * wfHeight);
        std::vector<uint8_t> cbPlane(wfWidth * wfHeight, WF_BG_CB);
        std::vector<uint8_t> crPlane(wfWidth * wfHeight, WF_BG_CR);

        // 背景Y輝度: 上下端を少し明るく、中央を暗くするグラデーション
        static constexpr uint8_t WF_BG_Y_EDGE = 80;   // 上下端の輝度
        static constexpr uint8_t WF_BG_Y_CENTER = 56;  // 中央の輝度
        for (int y = 0; y < wfHeight; y++) {
            // 中央からの距離を0.0〜1.0に正規化 (端=1.0, 中央=0.0)
            const float dist = std::abs(y - (wfHeight - 1) * 0.5f) / ((wfHeight - 1) * 0.5f);
            const uint8_t bgY = (uint8_t)(WF_BG_Y_CENTER + (WF_BG_Y_EDGE - WF_BG_Y_CENTER) * dist);
            std::memset(yPlane.data() + y * wfWidth, bgY, wfWidth);
        }

        // 振幅→ピクセル変換: 平方根スケーリング (γ=0.5)
        // 小さい音量でも波形が視認しやすくなる
        auto sqrtScale = [](float amplitude, int halfHeight) -> int {
            const float norm = std::min(std::abs(amplitude) / 32768.0f, 1.0f);
            const float scaled = std::sqrt(norm) * halfHeight;
            return (amplitude >= 0) ? -(int)scaled : (int)scaled;
        };

        const int monoCount = (int)monoSamples.size();
        const int yCenter = wfHeight / 2;
        for (int x = 0; x < wfWidth; x++) {
            const int s0 = (int)((int64_t)x * monoCount / wfWidth);
            const int s1 = std::max((int)((int64_t)(x + 1) * monoCount / wfWidth), s0 + 1);
            // min/max振幅を取得
            int16_t minVal = 0, maxVal = 0;
            for (int s = s0; s < s1 && s < monoCount; s++) {
                minVal = std::min(minVal, monoSamples[s]);
                maxVal = std::max(maxVal, monoSamples[s]);
            }
            int yTop    = yCenter + sqrtScale((float)maxVal, yCenter);
            int yBottom = yCenter + sqrtScale((float)minVal, yCenter);
            yTop    = clamp(yTop, 0, wfHeight - 1);
            yBottom = clamp(yBottom, 0, wfHeight - 1);
            for (int y = yTop; y <= yBottom; y++) {
                const int idx = y * wfWidth + x;
                yPlane[idx]  = WF_FG_Y;
                cbPlane[idx] = WF_FG_CB;
                crPlane[idx] = WF_FG_CR;
            }
        }

        // 6. 現在フレームの範囲を示す2本線を描画
        const double windowDuration = endSec - startSec;
        if (windowDuration > 0.0) {
            const double frameStartRatio = (frameSec - startSec) / windowDuration;
            const double frameEndRatio = (frameSec + frameDurSec - startSec) / windowDuration;
            const int xLeft  = clamp((int)(frameStartRatio * wfWidth), 0, wfWidth - 1);
            const int xRight = clamp((int)(frameEndRatio   * wfWidth), 0, wfWidth - 1);
            for (int y = 0; y < wfHeight; y++) {
                int idx = y * wfWidth + xLeft;
                yPlane[idx]  = WF_CL_Y;
                cbPlane[idx] = WF_CL_CB;
                crPlane[idx] = WF_CL_CR;
                idx = y * wfWidth + xRight;
                yPlane[idx]  = WF_CL_Y;
                cbPlane[idx] = WF_CL_CB;
                crPlane[idx] = WF_CL_CR;
            }
        }

        // 7. JPEG圧縮 (YUV444)
        const uint8_t* planes[3] = { yPlane.data(), cbPlane.data(), crPlane.data() };
        const int strides[3] = { wfWidth, wfWidth, wfWidth };
        if (!jpeg_utils::compressYUV444PToJpeg(planes, strides, wfWidth, wfHeight,
                                                WAVEFORM_JPEG_QUALITY, waveformJpegBuffer)) {
            return false;
        }
        *jpegData = waveformJpegBuffer.data();
        *jpegSize = static_cast<int>(waveformJpegBuffer.size());
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
    } catch (...) {
        ctx->setError(RuntimeException("TrimAdjust_Createで不明なネイティブ例外が発生しました"));
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

extern "C" AMATSUKAZE_API int TrimAdjust_GetWaveformJpeg(void* ptr, int frameNumber,
    const uint8_t** jpegData, int* jpegSize) {
    return static_cast<trimadjust::GUITrimAdjust*>(ptr)->getWaveformJpeg(frameNumber, jpegData, jpegSize) ? 1 : 0;
}
