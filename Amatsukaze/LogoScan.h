/*
* Amtasukaze Logo Analyze
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*
* ただし、approxim_line(),GetAB(),med_average()は
* MakKi氏の透過性ロゴ フィルタプラグインより拝借
* https://github.com/makiuchi-d/delogo-aviutl
*/
#pragma once

#include "common.h"
#include "TranscodeSetting.h"
#include "logo.h"
#include "AMTLogo.h"
#include "TsInfo.h"
#include "TextOut.h"
#include "ReaderWriterFFmpeg.h"

#include <cmath>
#include <numeric>
#include <fstream>

float CalcCorrelation5x5(const float* k, const float* Y, int x, int y, int w, float* pavg);

// ComputeKernel.cpp
bool IsAVXAvailable();
float CalcCorrelation5x5_AVX(const float* k, const float* Y, int x, int y, int w, float* pavg);

#if 0
float CalcCorrelation5x5_Debug(const float* k, const float* Y, int x, int y, int w, float* pavg);
#endif

namespace logo {

class LogoDataParam : public LogoData {
    enum {
        KSIZE = 5,
        KLEN = KSIZE * KSIZE,
        CSHIFT = 3,
        CLEN = 256 >> CSHIFT
    };
    int imgw, imgh, imgx, imgy; // この4つはすべて2の倍数
    std::unique_ptr<uint8_t[]> mask;
    std::unique_ptr<float[]> kernels;
    struct ScaleLimit {
        float scale;   // 正規化用スケール（想定される相関が1になるようにするため）
        float scale2;  // キャップ用スケール（想定される相関が小さすぎる場合に値を小さくするため）
    };
    std::unique_ptr<ScaleLimit[]> scales;
    float thresh;
    int maskpixels;
    float blackScore;

    float(*pCalcCorrelation5x5)(const float* k, const float* Y, int x, int y, int w, float* pavg);
public:
    LogoDataParam();

    LogoDataParam(LogoData&& logo, const LogoHeader* header);

    LogoDataParam(LogoData&& logo, int imgw, int imgh, int imgx, int imgy);

    int getImgWidth() const;
    int getImgHeight() const;
    int getImgX() const;
    int getImgY() const;

    const uint8_t* GetMask();
    const float* GetKernels();
    float getThresh() const;
    int getMaskPixels() const;

    // 評価準備
    void CreateLogoMask(float maskratio);

    float EvaluateLogo(const float *src, float maxv, float fade, float* work, int stride = -1);

    std::unique_ptr<LogoDataParam> MakeFieldLogo(bool bottom);

private:

    // 画素ごとにロゴとの相関を計算
    float CorrelationScore(const float *work, float maxv);

    void AddLogo(float* Y, int maxv);
};

void approxim_line(int n, double sum_x, double sum_y, double sum_x2, double sum_xy, double& a, double& b);

class LogoColor {
    double sumF, sumB, sumF2, sumB2, sumFB;
public:
    LogoColor();

    // ピクセルの色を追加 f:前景 b:背景
    void Add(int f, int b);

    // 値を0～1に正規化
    void Normalize(int maxv);

    /*====================================================================
    * 	GetAB_?()
    * 		回帰直線の傾きと切片を返す X軸:前景 Y軸:背景
    *===================================================================*/
    bool GetAB(float& A, float& B, int data_count) const;
};

class LogoScan {
    int scanw;
    int scanh;
    int logUVx;
    int logUVy;
    int thy;

    std::vector<short> tmpY, tmpU, tmpV;

    int nframes;
    std::unique_ptr<LogoColor[]> logoY, logoU, logoV;

    /*--------------------------------------------------------------------
    *	真中らへんを平均
    *-------------------------------------------------------------------*/
    int med_average(const std::vector<short>& s);

    static float calcDist(float a, float b);

    static void maxfilter(float *data, float *work, int w, int h);

public:
    // thy: オリジナルだとデフォルト30*8=240（8bitだと12くらい？）
    LogoScan(int scanw, int scanh, int logUVx, int logUVy, int thy);

    void Normalize(int mavx);

    std::unique_ptr<LogoData> GetLogo(bool clean) const;

    template <typename pixel_t>
    void AddScanFrame(
        const pixel_t* srcY,
        const pixel_t* srcU,
        const pixel_t* srcV,
        int pitchY, int pitchUV,
        int bgY, int bgU, int bgV) {
        int scanUVw = scanw >> logUVx;
        int scanUVh = scanh >> logUVy;

        for (int y = 0; y < scanh; ++y) {
            for (int x = 0; x < scanw; ++x) {
                logoY[x + y * scanw].Add(srcY[x + y * pitchY], bgY);
            }
        }
        for (int y = 0; y < scanUVh; ++y) {
            for (int x = 0; x < scanUVw; ++x) {
                logoU[x + y * scanUVw].Add(srcU[x + y * pitchUV], bgU);
                logoV[x + y * scanUVw].Add(srcV[x + y * pitchUV], bgV);
            }
        }

        ++nframes;
    }

    template <typename pixel_t>
    bool AddFrame(
        const pixel_t* srcY,
        const pixel_t* srcU,
        const pixel_t* srcV,
        int pitchY, int pitchUV) {
        int scanUVw = scanw >> logUVx;
        int scanUVh = scanh >> logUVy;

        tmpY.clear();
        tmpU.clear();
        tmpV.clear();

        tmpY.reserve((scanw + scanh - 1) * 2);
        tmpU.reserve((scanUVw + scanUVh - 1) * 2);
        tmpV.reserve((scanUVw + scanUVh - 1) * 2);

        /*--------------------------------------------------------------------
        *	背景色計算
        *-------------------------------------------------------------------*/

        for (int x = 0; x < scanw; ++x) {
            tmpY.push_back(srcY[x]);
            tmpY.push_back(srcY[x + (scanh - 1) * pitchY]);
        }
        for (int y = 1; y < scanh - 1; ++y) {
            tmpY.push_back(srcY[y * pitchY]);
            tmpY.push_back(srcY[scanw - 1 + y * pitchY]);
        }
        for (int x = 0; x < scanUVw; ++x) {
            tmpU.push_back(srcU[x]);
            tmpU.push_back(srcU[x + (scanUVh - 1) * pitchUV]);
            tmpV.push_back(srcV[x]);
            tmpV.push_back(srcV[x + (scanUVh - 1) * pitchUV]);
        }
        for (int y = 1; y < scanUVh - 1; ++y) {
            tmpU.push_back(srcU[y * pitchUV]);
            tmpU.push_back(srcU[scanUVw - 1 + y * pitchUV]);
            tmpV.push_back(srcV[y * pitchUV]);
            tmpV.push_back(srcV[scanUVw - 1 + y * pitchUV]);
        }

        // 最小と最大が閾値以上離れている場合、単一色でないと判断
        std::sort(tmpY.begin(), tmpY.end());
        if (abs(tmpY.front() - tmpY.back()) > thy) { // オリジナルだと thy * 8
            return false;
        }
        std::sort(tmpU.begin(), tmpU.end());
        if (abs(tmpU.front() - tmpU.back()) > thy) { // オリジナルだと thy * 8
            return false;
        }
        std::sort(tmpV.begin(), tmpV.end());
        if (abs(tmpV.front() - tmpV.back()) > thy) { // オリジナルだと thy * 8
            return false;
        }

        int bgY = med_average(tmpY);
        int bgU = med_average(tmpU);
        int bgV = med_average(tmpV);

        // 有効フレームを追加
        AddScanFrame(srcY, srcU, srcV, pitchY, pitchUV, bgY, bgU, bgV);

        return true;
    }
};

class SimpleVideoReader : AMTObject {
public:
    SimpleVideoReader(AMTContext& ctx);

    int64_t currentPos;

    void readAll(const tstring& src, int serviceid);

protected:
    virtual void onFirstFrame(AVStream *videoStream, AVFrame* frame);;
    virtual bool onFrame(AVFrame* frame);;
};

void DeintLogo(LogoData& dst, LogoData& src, int w, int h);

template <typename pixel_t>
void DeintY(float* dst, const pixel_t* src, int srcPitch, int w, int h) {
    auto merge = [](int a, int b, int c) { return (a + 2 * b + c + 2) / 4.0f; };

    for (int x = 0; x < w; ++x) {
        dst[x] = src[x];
        dst[x + (h - 1) * w] = src[x + (h - 1) * srcPitch];
    }
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 0; x < w; ++x) {
            dst[x + y * w] = merge(
                src[x + (y - 1) * srcPitch],
                src[x + y * srcPitch],
                src[x + (y + 1) * srcPitch]);
        }
    }
}

template <typename pixel_t>
void CopyY(float* dst, const pixel_t* src, int srcPitch, int w, int h) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            dst[x + y * w] = src[x + y * srcPitch];
        }
    }
}

typedef bool(*LOGO_ANALYZE_CB)(float progress, int nread, int total, int ngather);

class LogoAnalyzer : AMTObject {
    tstring srcpath;
    int serviceid;

    tstring workfile;
    tstring dstpath;
    LOGO_ANALYZE_CB cb;

    int scanx, scany;
    int scanw, scanh, thy;
    int numMaxFrames;
    int logUVx, logUVy;
    int imgw, imgh;
    int numFrames;
    std::unique_ptr<LogoData> logodata;

    float progressbase;

    // 今の所可逆圧縮が8bitのみなので対応は8bitのみ
    class InitialLogoCreator : SimpleVideoReader {
        LogoAnalyzer* pThis;
        CCodecPointer codec;
        size_t scanDataSize;
        size_t codedSize;
        int readCount;
        int64_t filesize;
        std::unique_ptr<uint8_t[]> memScanData;
        std::unique_ptr<uint8_t[]> memCoded;
        std::unique_ptr<LosslessVideoFile> file;
        std::unique_ptr<LogoScan> logoscan;
    public:
        InitialLogoCreator(LogoAnalyzer* pThis);
        void readAll(const tstring& src, int serviceid);
    protected:
        virtual void onFirstFrame(AVStream *videoStream, AVFrame* frame);;
        virtual bool onFrame(AVFrame* frame);;
    };

    void MakeInitialLogo();

    void ReMakeLogo();

public:
    LogoAnalyzer(AMTContext& ctx, const tchar* srcpath, int serviceid, const tchar* workfile, const tchar* dstpath,
        int imgx, int imgy, int w, int h, int thy, int numMaxFrames,
        LOGO_ANALYZE_CB cb);

    void ScanLogo();
};

struct LogoAnalyzeFrame {
    float p[11], t[11], b[11];
};

// ロゴ除去用解析フィルタ
class AMTAnalyzeLogo : public GenericVideoFilter {
    VideoInfo srcvi;

    std::unique_ptr<LogoDataParam> logo;
    std::unique_ptr<LogoDataParam> deintLogo;
    std::unique_ptr<LogoDataParam> fieldLogoT;
    std::unique_ptr<LogoDataParam> fieldLogoB;
    LogoHeader header;
    float maskratio;

    float logothresh;

    template <typename pixel_t>
    PVideoFrame GetFrameT(int n, IScriptEnvironment2* env) {
        size_t YSize = header.w * header.h;
        auto memCopy = std::unique_ptr<float[]>(new float[YSize + 8]);
        auto memDeint = std::unique_ptr<float[]>(new float[YSize + 8]);
        auto memWork = std::unique_ptr<float[]>(new float[YSize + 8]);

        PVideoFrame dst = env->NewVideoFrame(vi);
        LogoAnalyzeFrame* pDst = reinterpret_cast<LogoAnalyzeFrame*>(dst->GetWritePtr());

        float maxv = (float)((1 << srcvi.BitsPerComponent()) - 1);

        for (int i = 0; i < 8; ++i) {
            int nsrc = std::max(0, std::min(srcvi.num_frames - 1, n * 8 + i));
            PVideoFrame frame = child->GetFrame(nsrc, env);

            const pixel_t* srcY = reinterpret_cast<const pixel_t*>(frame->GetReadPtr(PLANAR_Y));
            const pixel_t* srcU = reinterpret_cast<const pixel_t*>(frame->GetReadPtr(PLANAR_U));
            const pixel_t* srcV = reinterpret_cast<const pixel_t*>(frame->GetReadPtr(PLANAR_V));

            int pitchY = frame->GetPitch(PLANAR_Y) / sizeof(pixel_t);
            int pitchUV = frame->GetPitch(PLANAR_U) / sizeof(pixel_t);
            int off = header.imgx + header.imgy * pitchY;
            int offUV = (header.imgx >> header.logUVx) + (header.imgy >> header.logUVy) * pitchUV;

            CopyY(memCopy.get(), srcY + off, pitchY, header.w, header.h);

            // フレームをインタレ解除
            DeintY(memDeint.get(), srcY + off, pitchY, header.w, header.h);

            LogoAnalyzeFrame info;
            for (int f = 0; f <= 10; ++f) {
                info.p[f] = std::abs(deintLogo->EvaluateLogo(memDeint.get(), maxv, (float)f / 10.0f, memWork.get()));
                info.t[f] = std::abs(fieldLogoT->EvaluateLogo(memCopy.get(), maxv, (float)f / 10.0f, memWork.get(), header.w * 2));
                info.b[f] = std::abs(fieldLogoB->EvaluateLogo(memCopy.get() + header.w, maxv, (float)f / 10.0f, memWork.get(), header.w * 2));
            }

            pDst[i] = info;
        }

        return dst;
    }

public:
    AMTAnalyzeLogo(PClip clip, const tstring& logoPath, float maskratio, IScriptEnvironment* env);

    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env_);

    int __stdcall SetCacheHints(int cachehints, int frame_range);

    static AVSValue __cdecl Create(AVSValue args, void* user_data, IScriptEnvironment* env);
};

class AMTEraseLogo : public GenericVideoFilter {
    PClip analyzeclip;

    std::vector<int> frameResult;
    std::unique_ptr<LogoDataParam> logo;
    LogoHeader header;
    int mode;
    int maxFadeLength;

    template <typename pixel_t>
    void Delogo(pixel_t* dst, int w, int h, int logopitch, int imgpitch, float maxv, const float* A, const float* B, float fade) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float srcv = dst[x + y * imgpitch];
                float a = A[x + y * logopitch];
                float b = B[x + y * logopitch];
                float bg = a * srcv + b * maxv;
                float tmp = fade * bg + (1 - fade) * srcv;
                dst[x + y * imgpitch] = (pixel_t)std::min(std::max(tmp + 0.5f, 0.0f), maxv);
            }
        }
    }

    void CalcFade2(int n, float& fadeT, float& fadeB, IScriptEnvironment2* env);

    void CalcFade(int n, float& fadeT, float& fadeB, IScriptEnvironment2* env);

    template <typename pixel_t>
    PVideoFrame GetFrameT(int n, IScriptEnvironment2* env) {
        PVideoFrame frame = child->GetFrame(n, env);
        env->MakeWritable(&frame);

        float maxv = (float)((1 << vi.BitsPerComponent()) - 1);
        pixel_t* dstY = reinterpret_cast<pixel_t*>(frame->GetWritePtr(PLANAR_Y));
        pixel_t* dstU = reinterpret_cast<pixel_t*>(frame->GetWritePtr(PLANAR_U));
        pixel_t* dstV = reinterpret_cast<pixel_t*>(frame->GetWritePtr(PLANAR_V));

        int pitchY = frame->GetPitch(PLANAR_Y) / sizeof(pixel_t);
        int pitchUV = frame->GetPitch(PLANAR_U) / sizeof(pixel_t);
        int off = header.imgx + header.imgy * pitchY;
        int offUV = (header.imgx >> header.logUVx) + (header.imgy >> header.logUVy) * pitchUV;

        if (mode == 0) { // 通常
            float fadeT, fadeB;
            CalcFade(n, fadeT, fadeB, env);

            // 最適Fade値でロゴ除去
            const float *logoAY = logo->GetA(PLANAR_Y);
            const float *logoBY = logo->GetB(PLANAR_Y);
            const float *logoAU = logo->GetA(PLANAR_U);
            const float *logoBU = logo->GetB(PLANAR_U);
            const float *logoAV = logo->GetA(PLANAR_V);
            const float *logoBV = logo->GetB(PLANAR_V);

            int wUV = (header.w >> header.logUVx);
            int hUV = (header.h >> header.logUVy);

            if (fadeT == fadeB) {
                // フレーム処理
                Delogo(dstY + off, header.w, header.h, header.w, pitchY, maxv, logoAY, logoBY, fadeT);
                Delogo(dstU + offUV, wUV, hUV, wUV, pitchUV, maxv, logoAU, logoBU, fadeT);
                Delogo(dstV + offUV, wUV, hUV, wUV, pitchUV, maxv, logoAV, logoBV, fadeT);
            } else {
                // フィールド処理

                Delogo(dstY + off, header.w, header.h / 2, header.w * 2, pitchY * 2, maxv, logoAY, logoBY, fadeT);
                Delogo(dstY + off + pitchY, header.w, header.h / 2, header.w * 2, pitchY * 2, maxv, logoAY + header.w, logoBY + header.w, fadeB);

                int uvparity = ((header.imgy / 2) % 2);
                int tuvoff = uvparity * pitchUV;
                int buvoff = !uvparity * pitchUV;
                int tuvoffl = uvparity * wUV;
                int buvoffl = !uvparity * wUV;

                Delogo(dstU + offUV + tuvoff, wUV, hUV / 2, wUV * 2, pitchUV * 2, maxv, logoAU + tuvoffl, logoBU + tuvoffl, fadeT);
                Delogo(dstV + offUV + tuvoff, wUV, hUV / 2, wUV * 2, pitchUV * 2, maxv, logoAV + tuvoffl, logoBV + tuvoffl, fadeT);

                Delogo(dstU + offUV + buvoff, wUV, hUV / 2, wUV * 2, pitchUV * 2, maxv, logoAU + buvoffl, logoBU + buvoffl, fadeB);
                Delogo(dstV + offUV + buvoff, wUV, hUV / 2, wUV * 2, pitchUV * 2, maxv, logoAV + buvoffl, logoBV + buvoffl, fadeB);
            }

            return frame;
        }

        // ロゴフレームデバッグ用
        float fadeT, fadeB;
        CalcFade(n, fadeT, fadeB, env);

        const char* str = "X";
        if (fadeT == fadeB) {
            str = (fadeT < 0.5) ? "X" : "O";
        } else {
            str = (fadeT < fadeB) ? "BTM" : "TOP";
        }

        char buf[200];
        sprintf_s(buf, "%s %.1f vs %.1f", str, fadeT, fadeB);
        DrawText(frame, true, 0, 0, buf);

        return frame;
    }

    void ReadLogoFrameFile(const tstring& logofPath, IScriptEnvironment* env);

public:
    AMTEraseLogo(PClip clip, PClip analyzeclip, const tstring& logoPath, const tstring& logofPath, int mode, int maxFadeLength, IScriptEnvironment* env);

    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env_);

    int __stdcall SetCacheHints(int cachehints, int frame_range);

    static AVSValue __cdecl Create(AVSValue args, void* user_data, IScriptEnvironment* env);
};

class LogoFrame : AMTObject {
    int numLogos;
    std::vector<LogoDataParam> logoArr;
    std::vector<LogoDataParam> deintArr;

    int maxYSize;
    int numFrames;
    int framesPerSec;
    VideoInfo vi;

    struct EvalResult {
        float corr0, corr1;
    };
    std::vector<EvalResult> evalResults;

    // 絶対値<0.2fは不明とみなす
    const float THRESH = 0.2f;

    int bestLogo;
    float logoRatio;

    template <typename pixel_t>
    void ScanFrame(PVideoFrame& frame, float* memDeint, float* memWork, const float maxv, std::vector<EvalResult>& outResult) {
        const pixel_t* srcY = reinterpret_cast<const pixel_t*>(frame->GetReadPtr(PLANAR_Y));
        int pitchY = frame->GetPitch(PLANAR_Y);

        outResult.resize(numLogos, { 0.0f, 0.0f });
        for (int i = 0; i < numLogos; ++i) {
            LogoDataParam& logo = deintArr[i];
            if (logo.isValid() == false ||
                logo.getImgWidth() != vi.width ||
                logo.getImgHeight() != vi.height) {
                outResult[i].corr0 = 0;
                outResult[i].corr1 = -1;
                continue;
            }

            // フレームをインタレ解除
            int off = logo.getImgX() + logo.getImgY() * pitchY;
            DeintY(memDeint, srcY + off, pitchY, logo.getWidth(), logo.getHeight());

            // ロゴ評価
            outResult[i].corr0 = logo.EvaluateLogo(memDeint, maxv, 0, memWork);
            outResult[i].corr1 = logo.EvaluateLogo(memDeint, maxv, 1, memWork);
        }
    }

    bool inTrimRange(const int n, const std::vector<int>& trims) const {
        // trimsの長さが0の場合は、すべてのフレームを対象とする
        if (trims.size() == 0 || (trims.size() % 2) != 0) {
            return true;
        }
        // n が trimの範囲内か確認し、範囲内ならtrueを返す
        for (int i = 0; i < trims.size()/2; i++) {
            if (trims[2*i] <= n && n <= trims[2*i + 1]) {
                return true;
            }
        }
        return false;
    }

    template <typename pixel_t>
    void IterateFrames(PClip clip, const std::vector<int>& trims, IScriptEnvironment2* env) {
        std::vector<float> memDeint(maxYSize + 8, 0.0f);
        std::vector<float> memWork(maxYSize + 8, 0.0f);
        const float maxv = (float)((1 << vi.BitsPerComponent()) - 1);
        evalResults.clear();
        evalResults.reserve(vi.num_frames * numLogos);

        if (trims.size() > 0 && (trims.size() % 2) == 0) {
            ctx.infoF("解析範囲");
            for (int i = 0; i < trims.size() / 2; i++) {
                ctx.infoF(" %6d-%6d", trims[2 * i], trims[2 * i + 1]);
            }
        }

        std::vector<EvalResult> frameResults;
        for (int n = 0; n < vi.num_frames; n++) {
            if (!inTrimRange(n, trims)) {
                continue;
            }
            PVideoFrame frame = clip->GetFrame(n, env);
            ScanFrame<pixel_t>(frame, memDeint.data(), memWork.data(), maxv, frameResults);

            if ((n % 5000) == 0) {
                ctx.infoF("%6d/%d", n, vi.num_frames);
            }
            evalResults.insert(evalResults.end(), frameResults.begin(), frameResults.end());
        }
        numFrames = (int)(evalResults.size() / numLogos);
        framesPerSec = (int)std::round((float)vi.fps_numerator / vi.fps_denominator);

        ctx.infoF("Finished %d frames", numFrames);
    }

    struct LogoScore {
        float cost;    // 消した後のゴミの量
        int numFrames;  // 検出したフレーム数
        float score;
    };

    // 直近のtargetFramesフレームのうち、
    // 0番目～numCandidatesまでのロゴから最も合っているロゴ(bestLogo)を選択
    // numCandidatesの指定がない場合(-1)は、すべてのロゴから検索
    // targetFramesの指定がない場合(-1)は、すべてのフレームから検索
    std::vector<LogoScore> calcLogoScore(int& targetFrames, int numCandidates) const;

public:
    LogoFrame(AMTContext& ctx, const std::vector<tstring>& logofiles, float maskratio);

    void scanFrames(PClip clip, const std::vector<int>& trims, IScriptEnvironment2* env);

    void dumpResult(const tstring& basepath);

    // 0番目～numCandidatesまでのロゴから最も合っているロゴ(bestLogo)を選択
    // numCandidatesの指定がない場合(-1)は、すべてのロゴから検索
    void selectLogo(int numCandidates = -1);

    // logoIndexに指定したロゴのlogoframeファイルを出力
    // logoIndexの指定がない場合(-1)は、bestLogoを出力
    void writeResult(const tstring& outpath, int logoIndex = -1);

    int getBestLogo() const;

    float getLogoRatio() const;
};

} // namespace logo


