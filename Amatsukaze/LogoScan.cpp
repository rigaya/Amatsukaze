/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "LogoScan.h"
#include <cstdlib>
#include <regex>
#include <array>
#include <queue>
#include <thread>
#include <deque>
#include <mutex>
#include <functional>
#include <numeric>
#include <cassert>
#include <exception>
#include <limits>
#include <type_traits>
#include <atomic>
#include <cstring>
#include "zlib.h"
#include "rgy_event.h"
#include "rgy_thread_pool.h"

void removeLogoLine(float *dst, const float *src, const int srcStride, const float *logoAY, const float *logoBY, const int logowidth, const float maxv, const float fade) {
    for (int x = 0; x < logowidth; x++) {
        const float srcv = src[x];
        const float a = logoAY[x];
        const float b = logoBY[x];
        const float bg = a * srcv + b * maxv;
        const float dstv = fade * bg + (1 - fade) * srcv;
        dst[x] = dstv;
    }
}

float CalcCorrelation5x5(const float* k, const float* Y, int x, int y, int w, float* pavg) {
    float avg = 0.0f;
    for (int ky = -2; ky <= 2; ky++) {
        for (int kx = -2; kx <= 2; kx++) {
            avg += Y[(x + kx) + (y + ky) * w];
        }
    }
    avg /= 25;
    float sum = 0.0f;
    for (int ky = -2; ky <= 2; ky++) {
        for (int kx = -2; kx <= 2; kx++) {
            sum += k[(kx + 2) + (ky + 2) * 5] * (Y[(x + kx) + (y + ky) * w] - avg);
        }
    }
    if (pavg) *pavg = avg;
    return sum;
}
float CalcCorrelation5x5_Debug(const float* k, const float* Y, int x, int y, int w, float* pavg) {
    float f0 = CalcCorrelation5x5(k, Y, x, y, w, pavg);
    float f1 = CalcCorrelation5x5_AVX(k, Y, x, y, w, pavg);
    if (f0 != f1) {
        printf("Error!!!\n");
    }
    return f1;
}
logo::LogoDataParam::LogoDataParam() :
    LogoData(),
    imgw(0),
    imgh(0),
    imgx(0),
    imgy(0),
    mask(),
    kernels(),
    scales(),
    thresh(0.0f),
    maskpixels(0),
    blackScore(0.0f),
    pCalcCorrelation5x5(nullptr),
    pRemoveLogoLine(nullptr) {}

logo::LogoDataParam::LogoDataParam(LogoData&& logo, const LogoHeader* header) :
    LogoData(std::move(logo)),
    imgw(header->imgw),
    imgh(header->imgh),
    imgx(header->imgx),
    imgy(header->imgy),
    mask(),
    kernels(),
    scales(),
    thresh(0.0f),
    maskpixels(0),
    blackScore(0.0f),
    pCalcCorrelation5x5(nullptr),
    pRemoveLogoLine(nullptr) {}

logo::LogoDataParam::LogoDataParam(LogoData&& logo, int imgw, int imgh, int imgx, int imgy) :
    LogoData(std::move(logo)),
    imgw(imgw),
    imgh(imgh),
    imgx(imgx),
    imgy(imgy),
    mask(),
    kernels(),
    scales(),
    thresh(0.0f),
    maskpixels(0),
    blackScore(0.0f),
    pCalcCorrelation5x5(nullptr),
    pRemoveLogoLine(nullptr) {}

int logo::LogoDataParam::getImgWidth() const { return imgw; }
int logo::LogoDataParam::getImgHeight() const { return imgh; }
int logo::LogoDataParam::getImgX() const { return imgx; }
int logo::LogoDataParam::getImgY() const { return imgy; }

const uint8_t* logo::LogoDataParam::GetMask() { return mask.get(); }
const float* logo::LogoDataParam::GetKernels() { return kernels.get(); }
float logo::LogoDataParam::getThresh() const { return thresh; }
int logo::LogoDataParam::getMaskPixels() const { return maskpixels; }

// 評価準備
void logo::LogoDataParam::CreateLogoMask(float maskratio) {
    // ロゴカーネルの考え方
    // ロゴとの相関を取りたい
    // これだけならロゴと画像を単に画素ごとに掛け合わせて足せばいいが
    // それだと、画像背景の濃淡に大きく影響を受けてしまう
    // なので、ロゴのエッジだけ画素ごとに相関を見ていく

    // 相関下限パラメータ
    const float corrLowerLimit = 0.2f;

    pCalcCorrelation5x5 = IsAVX2Available() ? CalcCorrelation5x5_AVX2 : (IsAVXAvailable() ? CalcCorrelation5x5_AVX : CalcCorrelation5x5);
    pRemoveLogoLine = IsAVX2Available() ? removeLogoLineAVX2 : removeLogoLine;

    int YSize = w * h;
    auto memWork = std::unique_ptr<float[]>(new float[YSize * CLEN + 8]);

    // 各単色背景にロゴを乗せる
    for (int c = 0; c < CLEN; c++) {
        float *slice = &memWork[c * YSize];
        std::fill_n(slice, YSize, (float)(c << CSHIFT));
        AddLogo(slice, 255);
    }

    auto makeKernel = [](float* k, float* Y, int x, int y, int w) {
        // コピー
        for (int ky = -2; ky <= 2; ky++) {
            for (int kx = -2; kx <= 2; kx++) {
                k[(kx + 2) + (ky + 2) * KSIZE] = Y[(x + kx) + (y + ky) * w];
            }
        }
        // 平均値
        float avg = std::accumulate(k, k + KLEN, 0.0f) / KLEN;
        // 平均値をゼロにする
        std::transform(k, k + KLEN, k, [avg](float p) { return p - avg; });
        };

    // 特徴点の抽出 //
         // 単色背景にロゴを乗せた画像の各ピクセルを中心とする5x5ウィンドウの
         // 画素値の分散の大きい順にmaskratio割合のピクセルを着目点とする
    std::vector<std::pair<float, int>> variance(YSize);
    // 各ピクセルの分散を計算（計算されていないところはゼロ初期化されてる）
    for (int y = 2; y < h - 2; y++) {
        for (int x = 2; x < w - 2; x++) {
            // 真ん中の色を取る
            float *slice = &memWork[(CLEN >> 1) * YSize];
            float k[KLEN];
            makeKernel(k, slice, x, y, w);
            variance[x + y * w].first = std::accumulate(k, k + KLEN, 0.0f,
                [](float sum, float val) { return sum + val * val; });
        }
    }
    // ピクセルインデックスを生成
    for (int i = 0; i < YSize; i++) {
        variance[i].second = i;
    }
    // 降順ソート
    std::sort(variance.begin(), variance.end(), std::greater<std::pair<float, int>>());
    // 計算結果からmask生成
    mask = std::unique_ptr<uint8_t[]>(new uint8_t[YSize]());
    maskpixels = std::min(YSize, (int)(YSize * maskratio));
    for (int i = 0; i < maskpixels; i++) {
        mask[variance[i].second] = 1;
    }
#if 0
    WriteGrayBitmap("hoge.bmp", w, h, [&](int x, int y) {
        return mask[x + y * w] ? 255 : 0;
        });
#endif

    // ピクセル周辺の特徴
    kernels = std::unique_ptr<float[]>(new float[maskpixels * KLEN + 8]);
    // 各ピクセルx各単色背景での相関値スケール
    scales = std::unique_ptr<ScaleLimit[]>(new ScaleLimit[maskpixels * CLEN]);
    int count = 0;
    float avgCorr = 0.0f;
    for (int y = 2; y < h - 2; y++) {
        for (int x = 2; x < w - 2; x++) {
            if (mask[x + y * w]) {
                float* k = &kernels[count * KLEN];
                ScaleLimit* s = &scales[count * CLEN];
                makeKernel(k, memWork.get(), x, y, w);
                for (int i = 0; i < CLEN; i++) {
                    float *slice = &memWork[i * YSize];
                    avgCorr += s[i].scale = std::abs(pCalcCorrelation5x5(k, slice, x, y, w, nullptr));
                }
                count++;
            }
        }
    }
    avgCorr /= maskpixels * CLEN;
    // 相関下限（これより小さい相関のピクセルはスケールしない）
    float limitCorr = avgCorr * corrLowerLimit;
    for (int i = 0; i < maskpixels * CLEN; i++) {
        float corr = scales[i].scale;
        scales[i].scale = (corr > 0) ? (1.0f / corr) : 0.0f;
        scales[i].scale2 = std::min(1.0f, corr / limitCorr);
    }

#if 0
    // ロゴカーネルをチェック
    for (int idx = 0; idx < count; idx++) {
        float* k = &kernels[idx++ * KLEN];
        float sum = std::accumulate(k, k + KLEN, 0.0f);
        //float abssum = std::accumulate(k, k + KLEN, 0.0f,
        //  [](float sum, float val) { return sum + std::abs(val); });
        // チェック
        if (std::abs(sum) > 0.00001f /*&& std::abs(abssum - 1.0f) > 0.00001f*/) {
            //printf("Error: %d => sum: %f, abssum: %f\n", idx, sum, abssum);
            printf("Error: %d => sum: %f\n", idx, sum);
        }
    }
#endif

    // 黒背景の評価値（これがはっきり出たときの基準）
    float *slice = &memWork[(16 >> CSHIFT) * YSize];
    blackScore = CorrelationScore(slice, 255);
}

float logo::LogoDataParam::EvaluateLogo(const float *src, float maxv, float fade, float* work, int stride) {
    // ロゴを評価 //
    const float *logoAY = GetA(PLANAR_Y);
    const float *logoBY = GetB(PLANAR_Y);

    if (stride == -1) {
        stride = w;
    }

    // ロゴを除去
    for (int y = 0; y < h; y++) {
        pRemoveLogoLine(&work[y * w], &src[y * stride], stride, &logoAY[y * w], &logoBY[y * w], w, maxv, fade);
    }

    // 正規化
    return CorrelationScore(work, maxv) / blackScore;
}

std::unique_ptr<logo::LogoDataParam> logo::LogoDataParam::MakeFieldLogo(bool bottom) {
    auto logo = std::unique_ptr<logo::LogoDataParam>(
        new logo::LogoDataParam(LogoData(w, h / 2, logUVx, logUVy), imgw, imgh / 2, imgx, imgy / 2));

    for (int y = 0; y < logo->h; y++) {
        for (int x = 0; x < logo->w; x++) {
            logo->aY[x + y * w] = aY[x + (bottom + y * 2) * w];
            logo->bY[x + y * w] = bY[x + (bottom + y * 2) * w];
        }
    }

    int UVoffset = ((int)bottom ^ (logo->imgy % 2));
    int wUV = logo->w >> logUVx;
    int hUV = logo->h >> logUVy;

    for (int y = 0; y < hUV; y++) {
        for (int x = 0; x < wUV; x++) {
            logo->aU[x + y * wUV] = aU[x + (UVoffset + y * 2) * wUV];
            logo->bU[x + y * wUV] = bU[x + (UVoffset + y * 2) * wUV];
            logo->aV[x + y * wUV] = aV[x + (UVoffset + y * 2) * wUV];
            logo->bV[x + y * wUV] = bV[x + (UVoffset + y * 2) * wUV];
        }
    }

    return logo;
}

// 画素ごとにロゴとの相関を計算
float logo::LogoDataParam::CorrelationScore(const float *work, float maxv) {
    const uint8_t* mask = GetMask();
    const float* kernels = GetKernels();

    // ロゴとの相関を評価
    int count = 0;
    float result = 0;
    for (int y = 2; y < h - 2; y++) {
        for (int x = 2; x < w - 2; x++) {
            if (mask[x + y * w]) {
                const float* k = &kernels[count * KLEN];

                float avg;
                float sum = pCalcCorrelation5x5(k, work, x, y, w, &avg);
                // avg単色の場合の相関値が1になるように正規化
                const int idx = std::max(0, std::min((int)CLEN, (int)(avg * CLEN / maxv)));
                ScaleLimit s = scales[count * CLEN + idx];
                // 1を超える部分は捨てる（ロゴによる相関ではない部分なので）
                float normalized = std::max(-1.0f, std::min(1.0f, sum * s.scale));
                // 相関が下限値以下の場合は一部元に戻す
                float score = normalized * s.scale2;

                result += score;

                count++;
            }
        }
    }

    return result;
}

void logo::LogoDataParam::AddLogo(float* Y, int maxv) {
    const float *logoAY = GetA(PLANAR_Y);
    const float *logoBY = GetB(PLANAR_Y);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float a = logoAY[x + y * w];
            float b = logoBY[x + y * w];
            if (a > 0) {
                Y[x + y * w] = (Y[x + y * w] - b * maxv) / a;
            }
        }
    }
}

/* static */ void logo::approxim_line(int n, double sum_x, double sum_y, double sum_x2, double sum_xy, double& a, double& b) {
    double temp = (double)n * sum_x2 - sum_x * sum_x; // n^2 * x^2
    a = ((double)n * sum_xy - sum_x * sum_y) / temp;  // (n^2 * x^2) / (n^2 * x^2) = 1
    b = (sum_x2 * sum_y - sum_x * sum_xy) / temp;     // (n^2 * x^3) / (n^2 * x^2) = x
}
logo::LogoColor::LogoColor() : sumF(), sumB(), sumF2(), sumB2(), sumFB() {}

// ピクセルの色を追加 f:前景 b:背景
void logo::LogoColor::Add(double f, double b) {
    sumF += f;
    sumB += b;
    sumF2 += f * f;
    sumB2 += b * b;
    sumFB += f * b;
}

/*====================================================================
* 	GetAB_?()
* 		回帰直線の傾きと切片を返す X軸:前景 Y軸:背景
*===================================================================*/
bool logo::LogoColor::GetAB(float& A, float& B, int data_count) const {
    double A1, A2;
    double B1, B2;
    approxim_line(data_count, sumF, sumB, sumF2, sumFB, A1, B1);
    approxim_line(data_count, sumB, sumF, sumB2, sumFB, A2, B2);

    // XY入れ替えたもの両方で平均を取る
    A = (float)((A1 + (1 / A2)) / 2);   // 傾きを平均
    B = (float)((B1 + (-B2 / A2)) / 2); // 切片も平均

    if (std::isnan(A) || std::isnan(B) || std::isinf(A) || std::isinf(B) || A == 0)
        return false;

    return true;
}

/*--------------------------------------------------------------------
*	真中らへんを平均
*-------------------------------------------------------------------*/
int logo::LogoScan::med_average(const std::vector<int>& s) {
    double t = 0;
    int nn = 0;

    int n = (int)s.size();

    // 真中らへんを平均
    for (int i = n / 4; i < n - (n / 4); i++, nn++)
        t += s[i];

    t = (t + nn / 2) / nn;

    return ((int)t);
}

/* static */ float logo::LogoScan::calcDist(float a, float b) {
    return (1.0f / 3.0f) * (a - 1) * (a - 1) + (a - 1) * b + b * b;
}

/* static */ void logo::LogoScan::maxfilter(float *data, float *work, int w, int h) {
    for (int y = 0; y < h; y++) {
        work[0 + y * w] = data[0 + y * w];
        for (int x = 1; x < w - 1; x++) {
            float a = data[x - 1 + y * w];
            float b = data[x + y * w];
            float c = data[x + 1 + y * w];
            work[x + y * w] = std::max(a, std::max(b, c));
        }
        work[w - 1 + y * w] = data[w - 1 + y * w];
    }
    for (int y = 1; y < h - 1; y++) {
        for (int x = 0; x < w; x++) {
            float a = data[x + (y - 1) * w];
            float b = data[x + y * w];
            float c = data[x + (y + 1) * w];
            work[x + y * w] = std::max(a, std::max(b, c));
        }
    }
}
// thy: オリジナルだとデフォルト30*8=240（8bitだと12くらい？）
logo::LogoScan::LogoScan(int scanw, int scanh, int logUVx, int logUVy, int thy) :
    scanw(scanw),
    scanh(scanh),
    logUVx(logUVx),
    logUVy(logUVy),
    thy(thy),
    nframes(),
    logoY(new LogoColor[scanw * scanh]),
    logoU(new LogoColor[scanw * scanh >> (logUVx + logUVy)]),
    logoV(new LogoColor[scanw * scanh >> (logUVx + logUVy)]) {}

std::unique_ptr<logo::LogoData> logo::LogoScan::GetLogo(bool clean) const {
    int scanUVw = scanw >> logUVx;
    int scanUVh = scanh >> logUVy;
    auto data = std::unique_ptr<logo::LogoData>(new logo::LogoData(scanw, scanh, logUVx, logUVy));
    float *aY = data->GetA(PLANAR_Y);
    float *aU = data->GetA(PLANAR_U);
    float *aV = data->GetA(PLANAR_V);
    float *bY = data->GetB(PLANAR_Y);
    float *bU = data->GetB(PLANAR_U);
    float *bV = data->GetB(PLANAR_V);

    for (int y = 0; y < scanh; y++) {
        for (int x = 0; x < scanw; x++) {
            int off = x + y * scanw;
            if (!logoY[off].GetAB(aY[off], bY[off], nframes)) return nullptr;
        }
    }
    for (int y = 0; y < scanUVh; y++) {
        for (int x = 0; x < scanUVw; x++) {
            int off = x + y * scanUVw;
            if (!logoU[off].GetAB(aU[off], bU[off], nframes)) return nullptr;
            if (!logoV[off].GetAB(aV[off], bV[off], nframes)) return nullptr;
        }
    }

    if (clean) {

        // ロゴ周辺を消す
        // メリット
        //   ロゴを消したときにロゴを含む長方形がうっすら出るのが防げる
        // デメリット
        //   ロゴ周辺のノイズっぽい成分はソース画像の圧縮により発生したノイズである
        //   ロゴの圧縮により発生した平均値が現れているので、
        //   これを残すことによりロゴ除去すると周辺のノイズも低減できるが、
        //   消してしまうとノイズは残ることになる

        int sizeY = scanw * scanh;
        auto dist = std::unique_ptr<float[]>(new float[sizeY]());
        for (int y = 0; y < scanh; y++) {
            for (int x = 0; x < scanw; x++) {
                int off = x + y * scanw;
                int offUV = (x >> logUVx) + (y >> logUVy) * scanUVw;
                dist[off] = calcDist(aY[off], bY[off]) +
                    calcDist(aU[offUV], bU[offUV]) +
                    calcDist(aV[offUV], bV[offUV]);

                // 値が小さすぎて分かりにくいので大きくしてあげる
                dist[off] *= 1000;
            }
        }

        // maxフィルタを掛ける
        auto work = std::unique_ptr<float[]>(new float[sizeY]);
        maxfilter(dist.get(), work.get(), scanw, scanh);
        maxfilter(dist.get(), work.get(), scanw, scanh);
        maxfilter(dist.get(), work.get(), scanw, scanh);

        // 小さいところはゼロにする
        for (int y = 0; y < scanh; y++) {
            for (int x = 0; x < scanw; x++) {
                int off = x + y * scanw;
                int offUV = (x >> logUVx) + (y >> logUVy) * scanUVw;
                if (dist[off] < 0.3f) {
                    aY[off] = 1;
                    bY[off] = 0;
                    aU[offUV] = 1;
                    bU[offUV] = 0;
                    aV[offUV] = 1;
                    bV[offUV] = 0;
                }
            }
        }
    }

    return  data;
}
logo::SimpleVideoReader::SimpleVideoReader(AMTContext& ctx)
    : AMTObject(ctx) {}

void logo::SimpleVideoReader::readAll(const tstring& src, int serviceid) {
    readAll(src, serviceid,
        [&](AVStream *videoStream, AVFrame* frame) { onFirstFrame(videoStream, frame); },
        [&](AVFrame* frame) { return onFrame(frame); });
}

void logo::SimpleVideoReader::readAll(const tstring& src, int serviceid, const FirstFrameCallback& onFirstFrameCb, const FrameCallback& onFrameCb) {
    using namespace av;

    InputContext inputCtx(src);
    if (avformat_find_stream_info(inputCtx(), NULL) < 0) {
        THROW(FormatException, "avformat_find_stream_info failed");
    }
    AVStream *videoStream = av::GetVideoStream(inputCtx(), serviceid);
    if (videoStream == NULL) {
        THROW(FormatException, "Could not find video stream ...");
    }
    AVCodecID vcodecId = videoStream->codecpar->codec_id;
    const AVCodec *pCodec = avcodec_find_decoder(vcodecId);
    if (pCodec == NULL) {
        THROW(FormatException, "Could not find decoder ...");
    }
    CodecContext codecCtx(pCodec);
    if (avcodec_parameters_to_context(codecCtx(), videoStream->codecpar) != 0) {
        THROW(FormatException, "avcodec_parameters_to_context failed");
    }
    codecCtx()->thread_count = GetFFmpegThreads(GetProcessorCount() - 2, videoStream->codecpar->height);
    if (avcodec_open2(codecCtx(), pCodec, NULL) != 0) {
        THROW(FormatException, "avcodec_open2 failed");
    }

    struct QueuedFrame {
        AVFrame* frame = nullptr;
        int64_t pos = -1;
    };

    constexpr size_t kPipelineDepth = 6;
    std::deque<QueuedFrame> frameQueue;
    std::mutex queueMtx;
    std::atomic<bool> stopRequested(false);
    std::exception_ptr workerException = nullptr;

    auto evQueueHasFrame = CreateEventUnique(nullptr, 1, 0);
    auto evQueueHasRoom = CreateEventUnique(nullptr, 1, 1);
    auto evDecodeDone = CreateEventUnique(nullptr, 1, 0);
    auto evStopRequested = CreateEventUnique(nullptr, 1, 0);

    auto updateQueueEventsLocked = [&]() {
        if (frameQueue.empty()) {
            ResetEvent(evQueueHasFrame.get());
        } else {
            SetEvent(evQueueHasFrame.get());
        }
        if (frameQueue.size() < kPipelineDepth) {
            SetEvent(evQueueHasRoom.get());
        } else {
            ResetEvent(evQueueHasRoom.get());
        }
    };
    auto clearQueueLocked = [&]() {
        while (!frameQueue.empty()) {
            auto& qf = frameQueue.front();
            if (qf.frame != nullptr) {
                av_frame_free(&qf.frame);
            }
            frameQueue.pop_front();
        }
        updateQueueEventsLocked();
    };

    std::thread worker([&]() {
        try {
            while (true) {
                if (WaitForSingleObject(evQueueHasFrame.get(), 10) == WAIT_TIMEOUT) {
                    const bool decodeDone = (WaitForSingleObject(evDecodeDone.get(), 0) == WAIT_OBJECT_0);
                    const bool stopped = (WaitForSingleObject(evStopRequested.get(), 0) == WAIT_OBJECT_0);
                    if (decodeDone || stopped) {
                        std::lock_guard<std::mutex> lock(queueMtx);
                        if (frameQueue.empty()) {
                            break;
                        }
                    }
                    continue;
                }

                QueuedFrame qf{};
                {
                    std::lock_guard<std::mutex> lock(queueMtx);
                    if (frameQueue.empty()) {
                        updateQueueEventsLocked();
                        continue;
                    }
                    qf = std::move(frameQueue.front());
                    frameQueue.pop_front();
                    updateQueueEventsLocked();
                }

                currentPos = qf.pos;
                const bool keepReading = onFrameCb ? onFrameCb(qf.frame) : true;
                if (qf.frame != nullptr) {
                    av_frame_free(&qf.frame);
                }
                if (!keepReading) {
                    stopRequested.store(true);
                    SetEvent(evStopRequested.get());
                    std::lock_guard<std::mutex> lock(queueMtx);
                    clearQueueLocked();
                    break;
                }
            }
        } catch (...) {
            workerException = std::current_exception();
            stopRequested.store(true);
            SetEvent(evStopRequested.get());
            SetEvent(evDecodeDone.get());
            std::lock_guard<std::mutex> lock(queueMtx);
            clearQueueLocked();
        }
    });

    auto enqueueFrame = [&](AVFrame* src, const int64_t pos) -> bool {
        AVFrame* copy = av_frame_alloc();
        if (copy == nullptr) {
            THROW(RuntimeException, "av_frame_alloc failed");
        }
        if (av_frame_ref(copy, src) != 0) {
            av_frame_free(&copy);
            THROW(RuntimeException, "av_frame_ref failed");
        }
        while (!stopRequested.load(std::memory_order_relaxed)) {
            if (WaitForSingleObject(evQueueHasRoom.get(), 10) == WAIT_TIMEOUT) {
                continue;
            }
            std::lock_guard<std::mutex> lock(queueMtx);
            if (frameQueue.size() < kPipelineDepth) {
                frameQueue.push_back(QueuedFrame{ copy, pos });
                updateQueueEventsLocked();
                return true;
            }
            updateQueueEventsLocked();
        }
        av_frame_free(&copy);
        return false;
    };

    bool first = true;
    int64_t lastPacketPos = -1;
    Frame frame;
    AVPacket packet = AVPacket();
    try {
        while (!stopRequested.load(std::memory_order_relaxed) && av_read_frame(inputCtx(), &packet) == 0) {
            if (packet.stream_index == videoStream->index) {
                lastPacketPos = packet.pos;
                if (avcodec_send_packet(codecCtx(), &packet) != 0) {
                    THROW(FormatException, "avcodec_send_packet failed");
                }
                while (!stopRequested.load(std::memory_order_relaxed) && avcodec_receive_frame(codecCtx(), frame()) == 0) {
                    if (first) {
                        if (onFirstFrameCb) {
                            onFirstFrameCb(videoStream, frame());
                        }
                        first = false;
                    }
                    if (!enqueueFrame(frame(), lastPacketPos)) {
                        break;
                    }
                }
            }
            av_packet_unref(&packet);
        }

        if (!stopRequested.load(std::memory_order_relaxed)) {
            // flush decoder
            if (avcodec_send_packet(codecCtx(), NULL) != 0) {
                THROW(FormatException, "avcodec_send_packet failed");
            }
            while (!stopRequested.load(std::memory_order_relaxed) && avcodec_receive_frame(codecCtx(), frame()) == 0) {
                if (first) {
                    if (onFirstFrameCb) {
                        onFirstFrameCb(videoStream, frame());
                    }
                    first = false;
                }
                if (!enqueueFrame(frame(), lastPacketPos)) {
                    break;
                }
            }
        }
    } catch (...) {
        av_packet_unref(&packet);
        stopRequested.store(true);
        SetEvent(evStopRequested.get());
        SetEvent(evDecodeDone.get());
        {
            std::lock_guard<std::mutex> lock(queueMtx);
            clearQueueLocked();
        }
        if (worker.joinable()) {
            worker.join();
        }
        if (workerException) {
            std::rethrow_exception(workerException);
        }
        throw;
    }

    SetEvent(evDecodeDone.get());
    if (worker.joinable()) {
        worker.join();
    }
    if (workerException) {
        std::rethrow_exception(workerException);
    }
}
/* virtual */ void logo::SimpleVideoReader::onFirstFrame(AVStream *videoStream, AVFrame* frame) {}
/* virtual */ bool logo::SimpleVideoReader::onFrame(AVFrame* frame) { return true; }

/* static */ void logo::DeintLogo(LogoData& dst, LogoData& src, int w, int h) {
    const float *srcAY = src.GetA(PLANAR_Y);
    float *dstAY = dst.GetA(PLANAR_Y);
    const float *srcBY = src.GetB(PLANAR_Y);
    float *dstBY = dst.GetB(PLANAR_Y);

    auto merge = [](float a, float b, float c) { return (a + 2 * b + c) / 4.0f; };

    for (int x = 0; x < w; x++) {
        dstAY[x] = srcAY[x];
        dstBY[x] = srcBY[x];
        dstAY[x + (h - 1) * w] = srcAY[x + (h - 1) * w];
        dstBY[x + (h - 1) * w] = srcBY[x + (h - 1) * w];
    }
    for (int y = 1; y < h - 1; y++) {
        for (int x = 0; x < w; x++) {
            dstAY[x + y * w] = merge(
                srcAY[x + (y - 1) * w],
                srcAY[x + y * w],
                srcAY[x + (y + 1) * w]);
            dstBY[x + y * w] = merge(
                srcBY[x + (y - 1) * w],
                srcBY[x + y * w],
                srcBY[x + (y + 1) * w]);
        }
    }
}

logo::LogoScanDataCompressed::LogoScanDataCompressed() : compressed_data(), original_size(0) {};

logo::LogoScanDataCompressed::~LogoScanDataCompressed() {};

void logo::LogoScanDataCompressed::compress(const void *ptr, size_t datasize) {
    original_size = (unsigned long)datasize;
    std::vector<uint8_t> tmp(datasize * 3 / 2);
    unsigned long compressed_size = (unsigned long)tmp.size();
    compress2(tmp.data(), &compressed_size, (BYTE *)ptr, (unsigned long)datasize, Z_DEFAULT_COMPRESSION);

    compressed_data.resize(compressed_size);
    memcpy(compressed_data.data(), tmp.data(), compressed_size);
}

void logo::LogoScanDataCompressed::decompress(void *ptr) {
    unsigned long buf_size = original_size;
    uncompress((BYTE *)ptr, &buf_size, (BYTE *)compressed_data.data(), (unsigned long)compressed_data.size());
}

logo::LogoAnalyzer::InitialLogoCreator::InitialLogoCreator(LogoAnalyzer* pThis) :
    SimpleVideoReader(pThis->ctx),
    pThis(pThis),
    scanDataSize(pThis->scanw * pThis->scanh * 3 / 2),
    bitDepth(8),
    readCount(0),
    filesize(0),
    memScanData(),
    scanData() {}

void logo::LogoAnalyzer::InitialLogoCreator::readAll(const tstring& src, int serviceid) {
    { File file(src, _T("rb")); filesize = file.size(); }

    SimpleVideoReader::readAll(src, serviceid);

    pThis->logodata = logoscan->GetLogo(false);
    if (pThis->logodata == nullptr) {
        THROW(RuntimeException, "Insufficient logo frames");
    }
}
/* virtual */ void logo::LogoAnalyzer::InitialLogoCreator::onFirstFrame(AVStream *videoStream, AVFrame* frame) {
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((AVPixelFormat)(frame->format));

    bitDepth = desc->comp[0].depth;

    pThis->logUVx = desc->log2_chroma_w;
    pThis->logUVy = desc->log2_chroma_h;
    pThis->imgw = frame->width;
    pThis->imgh = frame->height;

    logoscan = std::unique_ptr<LogoScan>(
        new LogoScan(pThis->scanw, pThis->scanh, pThis->logUVx, pThis->logUVy, pThis->thy));

    pThis->numFrames = 0;
}
/* virtual */ bool logo::LogoAnalyzer::InitialLogoCreator::onFrame(AVFrame* frame) {
    readCount++;

    if (pThis->numFrames >= pThis->numMaxFrames) return false;

    (isHighBitDepth()) ? AddFrame<uint16_t>(frame) : AddFrame<uint8_t>(frame);

    if ((readCount % 200) == 0) {
        float progress = (float)currentPos / filesize * 50;
        if (pThis->cb(progress, readCount, 0, pThis->numFrames) == false) {
            THROW(RuntimeException, "Cancel requested");
        }
    }

    return true;
}

void logo::LogoAnalyzer::MakeInitialLogo() {
    creator = std::make_unique<InitialLogoCreator>(this);
    creator->readAll(srcpath, serviceid);
}

logo::LogoAnalyzer::LogoAnalyzer(AMTContext& ctx, const tchar* srcpath, int serviceid, const tchar* workfile, const tchar* dstpath,
    int imgx, int imgy, int w, int h, int thy, int numMaxFrames,
    LOGO_ANALYZE_CB cb) :
    AMTObject(ctx),
    srcpath(srcpath),
    serviceid(serviceid),
    workfile(workfile),
    dstpath(dstpath),
    cb(cb),
    scanx(imgx),
    scany(imgy),
    scanw(w),
    scanh(h),
    thy(thy),
    numMaxFrames(numMaxFrames),
    logUVx(),
    logUVy(),
    imgw(),
    imgh(),
    numFrames(),
    logodata(),
    progressbase(0),
    creator() {
}

void logo::LogoAnalyzer::ScanLogo() {
    // 有効フレームデータと初期ロゴの取得
    progressbase = 0;
    MakeInitialLogo();

    // データ解析とロゴの作り直し
    progressbase = 50;
    //MultiCandidate();
    (creator->isHighBitDepth()) ? ReMakeLogo<uint16_t>() : ReMakeLogo<uint8_t>();
    progressbase = 75;
    (creator->isHighBitDepth()) ? ReMakeLogo<uint16_t>() : ReMakeLogo<uint8_t>();
    //ReMakeLogo();

    if (cb(1, numFrames, numFrames, numFrames) == false) {
        THROW(RuntimeException, "Cancel requested");
    }

    LogoHeader header(scanw, scanh, logUVx, logUVy, imgw, imgh, scanx, scany, "No Name");
    header.serviceId = serviceid;
    logodata->Save(dstpath, &header);
}
logo::AMTAnalyzeLogo::AMTAnalyzeLogo(PClip clip, const tstring& logoPath, float maskratio, IScriptEnvironment* env)
    : GenericVideoFilter(clip)
    , srcvi(vi)
    , maskratio(maskratio) {
    try {
        logo = std::unique_ptr<LogoDataParam>(
            new LogoDataParam(LogoData::Load(logoPath, &header), &header));
    } catch (const IOException&) {
        env->ThrowError("Failed to read logo file (%s)", logoPath.c_str());
    }

    deintLogo = std::unique_ptr<LogoDataParam>(
        new LogoDataParam(LogoData(header.w, header.h, header.logUVx, header.logUVy), &header));
    DeintLogo(*deintLogo, *logo, header.w, header.h);
    deintLogo->CreateLogoMask(maskratio);

    fieldLogoT = logo->MakeFieldLogo(false);
    fieldLogoT->CreateLogoMask(maskratio);
    fieldLogoB = logo->MakeFieldLogo(true);
    fieldLogoB->CreateLogoMask(maskratio);

    // for debug
    //LogoHeader hT = header;
    //hT.h /= 2;
    //hT.imgh /= 2;
    //hT.imgy /= 2;
    //fieldLogoT->Save("logoT.lgd", &hT);
    //fieldLogoB->Save("logoB.lgd", &hT);

    int out_bytes = sizeof(LogoAnalyzeFrame) * 8;
    vi.pixel_type = VideoInfo::CS_BGR32;
    vi.width = 64;
    vi.height = nblocks(out_bytes, vi.width * 4);

    vi.num_frames = nblocks(vi.num_frames, 8);
}

PVideoFrame __stdcall logo::AMTAnalyzeLogo::GetFrame(int n, IScriptEnvironment* env_) {
    IScriptEnvironment2* env = static_cast<IScriptEnvironment2*>(env_);

    int pixelSize = srcvi.ComponentSize();
    switch (pixelSize) {
    case 1:
        return GetFrameT<uint8_t>(n, env);
    case 2:
        return GetFrameT<uint16_t>(n, env);
    default:
        env->ThrowError("[AMTAnalyzeLogo] Unsupported pixel format");
    }

    return PVideoFrame();
}

int __stdcall logo::AMTAnalyzeLogo::SetCacheHints(int cachehints, int frame_range) {
    if (cachehints == CACHE_GET_MTMODE) {
        return MT_NICE_FILTER;
    }
    return 0;
}

/* static */ AVSValue __cdecl logo::AMTAnalyzeLogo::Create(AVSValue args, void* user_data, IScriptEnvironment* env) {
    return new AMTAnalyzeLogo(
        args[0].AsClip(),       // source
        char_to_tstring(args[1].AsString()),			// logopath
        (float)args[2].AsFloat(35) / 100.0f, // maskratio
        env
    );
}

void logo::AMTEraseLogo::CalcFade2(int n, float& fadeT, float& fadeB, IScriptEnvironment2* env) {
    enum {
        DIST = 4,
    };
    LogoAnalyzeFrame frames[DIST * 2 + 1];

    int prev_n = INT_MAX;
    PVideoFrame frame;
    for (int i = -DIST; i <= DIST; i++) {
        int nsrc = std::max(0, std::min(vi.num_frames - 1, n + i));
        int analyze_n = (nsrc + i) >> 3;
        int idx = (nsrc + i) & 7;

        if (analyze_n != prev_n) {
            frame = analyzeclip->GetFrame(analyze_n, env);
            prev_n = analyze_n;
        }

        const LogoAnalyzeFrame* pInfo =
            reinterpret_cast<const LogoAnalyzeFrame*>(frame->GetReadPtr());
        frames[i + DIST] = pInfo[idx];
    }
    frame = nullptr;

    int minfades[DIST * 2 + 1];
    for (int i = 0; i < DIST * 2 + 1; i++) {
        minfades[i] = (int)(std::min_element(frames[i].p, frames[i].p + 11) - frames[i].p);
    }
    int minT = (int)(std::min_element(frames[DIST].t, frames[DIST].t + 11) - frames[DIST].t);
    int minB = (int)(std::min_element(frames[DIST].b, frames[DIST].b + 11) - frames[DIST].b);
    // 前後4フレームを見てフェードしてるか突然消えてるか判断
    float before_fades = 0;
    float after_fades = 0;
    for (int i = 1; i <= 4; i++) {
        before_fades += minfades[DIST - i];
        after_fades += minfades[DIST + i];
    }
    before_fades /= 4 * 10;
    after_fades /= 4 * 10;
    // しきい値は適当
    if ((before_fades < 0.3 && after_fades > 0.7) ||
        (before_fades > 0.7 && after_fades < 0.3)) {
        // 急な変化 -> フィールドごとに見る
        fadeT = minT / 10.0f;
        fadeB = minB / 10.0f;
    } else {
        // 緩やかな変化 -> フレームごとに見る
        fadeT = fadeB = (minfades[DIST] / 10.0f);
    }
}

void logo::AMTEraseLogo::CalcFade(int n, float& fadeT, float& fadeB, IScriptEnvironment2* env) {
    if (frameResult.size() == 0) {
        // ロゴ解析結果がない場合は常にリアルタイム解析
        CalcFade2(n, fadeT, fadeB, env);
    } else {
        // ロゴ解析結果を大局的に使って、
        // 切り替わり周辺だけリアルタイム解析結果を使う
        int halfWidth = (maxFadeLength >> 1);
        std::vector<int> frames(halfWidth * 2 + 1);
        for (int i = -halfWidth; i <= halfWidth; i++) {
            int nsrc = std::max(0, std::min(vi.num_frames - 1, n + i));
            frames[i + halfWidth] = frameResult[nsrc];
        }
        if (std::all_of(frames.begin(), frames.end(), [&](int p) { return p == frames[0]; })) {
            // ON or OFF
            fadeT = fadeB = ((frames[halfWidth] == 2) ? 1.0f : 0.0f);
        } else {
            // 切り替わりを含む
            CalcFade2(n, fadeT, fadeB, env);
        }
    }
}

void logo::AMTEraseLogo::ReadLogoFrameFile(const tstring& logofPath, IScriptEnvironment* env) {
    struct LogoFrameElement {
        bool isStart;
        int best, start, end;
    };
    std::vector<LogoFrameElement> elements;
    try {
        File file(logofPath, _T("r"));
        std::regex re("^\\s*(\\d+)\\s+(\\S)\\s+(\\d+)\\s+(\\S+)\\s+(\\d+)\\s+(\\d+)");
        std::string str;
        while (file.getline(str)) {
            std::smatch m;
            if (std::regex_search(str, m, re)) {
                LogoFrameElement elem = {
                    std::tolower(m[2].str()[0]) == 's', // isStart
                    std::stoi(m[1].str()), // best
                    std::stoi(m[5].str()), // start
                    std::stoi(m[6].str())  // end
                };
                elements.push_back(elem);
            }
        }
    } catch (const IOException&) {
        env->ThrowError("Failed to read dat file (%s)", logofPath.c_str());
    }
    frameResult.resize(vi.num_frames);
    std::fill(frameResult.begin(), frameResult.end(), 0);
    for (int i = 0; i < (int)elements.size(); i += 2) {
        if (elements[i].isStart == false || elements[i + 1].isStart) {
            env->ThrowError("Invalid logoframe file. Start and End must be cyclic.");
        }
        std::fill(frameResult.begin() + std::min(vi.num_frames, elements[i].start),
            frameResult.begin() + std::min(vi.num_frames, elements[i].end + 1), 1);
        std::fill(frameResult.begin() + std::min(vi.num_frames, elements[i].end),
            frameResult.begin() + std::min(vi.num_frames, elements[i + 1].start + 1), 2);
        std::fill(frameResult.begin() + std::min(vi.num_frames, elements[i + 1].start + 1),
            frameResult.begin() + std::min(vi.num_frames, elements[i + 1].end + 1), 1);
    }
}
logo::AMTEraseLogo::AMTEraseLogo(PClip clip, PClip analyzeclip, const tstring& logoPath, const tstring& logofPath, int mode, int maxFadeLength, IScriptEnvironment* env)
    : GenericVideoFilter(clip)
    , analyzeclip(analyzeclip)
    , mode(mode)
    , maxFadeLength(maxFadeLength) {
    try {
        logo = std::unique_ptr<LogoDataParam>(
            new LogoDataParam(LogoData::Load(logoPath, &header), &header));
    } catch (const IOException&) {
        env->ThrowError("Failed to read logo file (%s)", logoPath.c_str());
    }

    if (logofPath.size() > 0) {
        ReadLogoFrameFile(logofPath, env);
    }
}

PVideoFrame __stdcall logo::AMTEraseLogo::GetFrame(int n, IScriptEnvironment* env_) {
    IScriptEnvironment2* env = static_cast<IScriptEnvironment2*>(env_);

    int pixelSize = vi.ComponentSize();
    switch (pixelSize) {
    case 1:
        return GetFrameT<uint8_t>(n, env);
    case 2:
        return GetFrameT<uint16_t>(n, env);
    default:
        env->ThrowError("[AMTEraseLogo] Unsupported pixel format");
    }

    return PVideoFrame();
}

int __stdcall logo::AMTEraseLogo::SetCacheHints(int cachehints, int frame_range) {
    if (cachehints == CACHE_GET_MTMODE) {
        return MT_NICE_FILTER;
    }
    return 0;
}

/* static */ AVSValue __cdecl logo::AMTEraseLogo::Create(AVSValue args, void* user_data, IScriptEnvironment* env) {
    return new AMTEraseLogo(
        args[0].AsClip(),       // source
        args[1].AsClip(),       // analyzeclip
        char_to_tstring(args[2].AsString()),			// logopath
        char_to_tstring(args[3].AsString("")),		// logofpath
        args[4].AsInt(0),       // mode
        args[5].AsInt(16),      // maxfade
        env
    );
}

// 絶対値<0.2fは不明とみなす
logo::LogoFrame::LogoFrame(AMTContext& ctx, const std::vector<tstring>& logofiles, float maskratio) :
    AMTObject(ctx),
    numLogos((int)logofiles.size()),
    logoArr(logofiles.size()),
    deintArr(logofiles.size()),
    maxYSize(0),
    numFrames(0),
    framesPerSec(0),
    vi(),
    evalResults(),
    bestLogo(-1),
    logoRatio(0.0) {
    vi.num_frames = 0;
    for (int i = 0; i < (int)logofiles.size(); i++) {
        try {
            LogoHeader header;
            logoArr[i] = LogoDataParam(LogoData::Load(logofiles[i], &header), &header);
            deintArr[i] = LogoDataParam(LogoData(header.w, header.h, header.logUVx, header.logUVy), &header);
            DeintLogo(deintArr[i], logoArr[i], header.w, header.h);
            deintArr[i].CreateLogoMask(maskratio);

            int YSize = header.w * header.h;
            maxYSize = std::max(maxYSize, YSize);
        } catch (const IOException&) {
            // 読み込みエラーは無視
        }
    }
}

void logo::LogoFrame::setClipInfo(PClip clip) {
    vi = clip->GetVideoInfo();
    evalResults.clear();
    evalResults.resize(vi.num_frames * numLogos, EvalResult{ 0, -1 });
    numFrames = vi.num_frames;
    framesPerSec = (int)std::round((float)vi.fps_numerator / vi.fps_denominator);
}

void logo::LogoFrame::scanFrames(PClip clip, const std::vector<int>& trims, const int threadId, const int totalThreads, IScriptEnvironment2* env) {
    if (vi.num_frames == 0) {
        setClipInfo(clip);
    }
    const int pixelSize = vi.ComponentSize();
    switch (pixelSize) {
    case 1: return IterateFrames<uint8_t>(clip, trims, threadId, totalThreads, env);
    case 2: return IterateFrames<uint16_t>(clip, trims, threadId, totalThreads, env);
    default:
        env->ThrowError("[LogoFrame] Unsupported pixel format");
    }
}

void logo::LogoFrame::dumpResult(const tstring& basepath) {
    for (int i = 0; i < numLogos; i++) {
        StringBuilder sb;
        for (int n = 0; n < numFrames; n++) {
            auto& r = evalResults[n * numLogos + i];
            sb.append("%f,%f\n", r.corr0, r.corr1);
        }
        File file(StringFormat(_T("%s%d"), basepath, i), _T("w"));
        file.write(sb.getMC());
    }
}

// 0番目～numCandidatesまでのロゴから最も合っているロゴ(bestLogo)を選択
// numCandidatesの指定がない場合(-1)は、すべてのロゴから検索
// targetFramesの指定がない場合(-1)は、すべてのフレームから検索
std::vector<logo::LogoFrame::LogoScore> logo::LogoFrame::calcLogoScore(const std::vector<std::pair<int, int>>& range, int numCandidates) const {
    if (numCandidates < 0) {
        numCandidates = numLogos;
    }
    std::vector<LogoScore> logoScore(numCandidates);
    for (const auto& r : range) {
        const int n_fin = std::min(r.second, (int)evalResults.size() / numLogos - 1);
        for (int n = r.first; n <= n_fin; n++) {
            for (int i = 0; i < numCandidates; i++) {
                const auto& r = evalResults[n * numLogos + i];
                // ロゴを検出 かつ 消せてる
                if (r.corr0 > THRESH && std::abs(r.corr1) < THRESH) {
                    logoScore[i].numFrames++;
                    logoScore[i].cost += std::abs(r.corr1);
                }
            }
        }
    }
    const int targetFrames = getTotalFrames(range);
    for (int i = 0; i < numCandidates; i++) {
        auto& s = logoScore[i];
        // (消した後のゴミの量の平均) * (検出したフレーム割合の逆数)
        s.score = (s.numFrames == 0) ? INFINITY :
            (s.cost / s.numFrames) * (targetFrames / (float)s.numFrames);
    }
    return logoScore;
}

void logo::LogoFrame::selectLogo(const std::vector<int>& trims, int numCandidates) {
    const auto targetRange = trimAllTargets(trims);
    const int targetFrames = getTotalFrames(targetRange);
    const auto logoScore = calcLogoScore(targetRange, numCandidates);
    for (int i = 0; i < numCandidates; i++) {
        const auto& s = logoScore[i];
#if 1
        ctx.debugF("logo%d: %f * %f = %f", i + 1,
            (s.cost / s.numFrames), (targetFrames / (float)s.numFrames), s.score);
#endif
    }
    bestLogo = (int)(std::min_element(logoScore.begin(), logoScore.end(), [](const auto& a, const auto& b) {
        return a.score < b.score;
    }) - logoScore.begin());
    logoRatio = (float)logoScore[bestLogo].numFrames / targetFrames;
}

// logoIndexに指定したロゴのlogoframeファイルを出力
// logoIndexの指定がない場合(-1)は、bestLogoを出力
void logo::LogoFrame::writeResult(const tstring& outpath, int logoIndex) {
    if (logoIndex < 0) {
        if (bestLogo < 0) {
            selectLogo({});
        }
        logoIndex = bestLogo;
    }

    const float threshL = 0.5f; // MinMax評価用
    // MinMax幅
    const float avgDur = 1.0f;
    const float medianDur = 0.5f;

    // スコアに変換
    int halfAvgFrames = int(framesPerSec * avgDur / 2 + 0.5f);
    int aveFrames = halfAvgFrames * 2 + 1;
    int halfMedianFrames = int(framesPerSec * medianDur / 2 + 0.5f);
    int medianFrames = halfMedianFrames * 2 + 1;
    int winFrames = std::max(aveFrames, medianFrames);
    int halfWinFrames = winFrames / 2;
    std::vector<float> rawScores_(numFrames + winFrames);
    auto rawScores = rawScores_.begin() + halfWinFrames;
    for (int n = 0; n < numFrames; n++) {
        auto& r = evalResults[n * numLogos + logoIndex];
        // corr0のマイナスとcorr1のプラスはノイズなので消す
        rawScores[n] = std::max(0.0f, r.corr0) + std::min(0.0f, r.corr1);
    }
    // 両端を端の値で埋める
    std::fill(rawScores_.begin(), rawScores, rawScores[0]);
    std::fill(rawScores + numFrames, rawScores_.end(), rawScores[numFrames - 1]);

    struct FrameResult {
        int result;
        float score;
    };

    // フィルタで均す
    std::vector<FrameResult> frameResult(numFrames);
    std::vector<float> medianBuf(medianFrames);
    for (int i = 0; i < numFrames; i++) {
        // MinMax
        // 前の最大値と後ろの最大値の小さい方を取る
        // 動きの多い映像でロゴがかき消されることがあるので、それを救済する
        float beforeMax = *std::max_element(rawScores + i - halfAvgFrames, rawScores + i);
        float afterMax = *std::max_element(rawScores + i + 1, rawScores + i + 1 + halfAvgFrames);
        float minMax = std::min(beforeMax, afterMax);
        int minMaxResult = (std::abs(minMax) < threshL) ? 1 : (minMax < 0.0f) ? 0 : 2;

        // 移動平均
        // MinMaxだけだと薄くても安定して表示されてるとかが識別できないので
        // これも必要
        float avg = std::accumulate(rawScores + i - halfAvgFrames,
            rawScores + i + halfAvgFrames + 1, 0.0f) / aveFrames;
        int avgResult = (std::abs(avg) < THRESH) ? 1 : (avg < 0.0f) ? 0 : 2;

        // 両者が違ってたら不明とする
        frameResult[i].result = (minMaxResult != avgResult) ? 1 : minMaxResult;

        // 生の値は動きが激しいので少しメディアンフィルタをかけておく
        std::copy(rawScores + i - halfMedianFrames,
            rawScores + i + halfMedianFrames + 1, medianBuf.begin());
        std::sort(medianBuf.begin(), medianBuf.end());
        frameResult[i].score = medianBuf[halfMedianFrames];
    }

    // 不明部分を推測
    // 両側がロゴありとなっていたらロゴありとする
    for (auto it = frameResult.begin(); it != frameResult.end();) {
        auto first1 = std::find_if(it, frameResult.end(), [](FrameResult r) { return r.result == 1; });
        it = std::find_if_not(first1, frameResult.end(), [](FrameResult r) { return r.result == 1; });
        int prevResult = (first1 == frameResult.begin()) ? 0 : (first1 - 1)->result;
        int nextResult = (it == frameResult.end()) ? 0 : it->result;
        if (prevResult == nextResult) {
            std::transform(first1, it, first1, [=](FrameResult r) {
                r.result = prevResult;
                return r;
                });
        }
    }

    // ロゴ区間を出力
    StringBuilder sb;
    for (auto it = frameResult.begin(); it != frameResult.end();) {
        auto sEnd_ = std::find_if(it, frameResult.end(), [](FrameResult r) { return r.result == 2; });
        auto eEnd_ = std::find_if(sEnd_, frameResult.end(), [](FrameResult r) { return r.result == 0; });

        // 移動平均なので実際の値とは時間差がある場合があるので、フレーム時刻を精緻化
        auto sEnd = sEnd_;
        auto eEnd = eEnd_;
        if (sEnd != frameResult.end()) {
            if (sEnd->score >= THRESH) {
                // すでに始まっているので戻ってみる
                sEnd = std::find_if(std::make_reverse_iterator(sEnd), frameResult.rend(),
                    [=](FrameResult r) { return r.score < THRESH; }).base();
            } else {
                // まだ始まっていないので進んでみる
                sEnd = std::find_if(sEnd, frameResult.end(),
                    [=](FrameResult r) { return r.score >= THRESH; });
            }
        }
        if (eEnd != frameResult.end()) {
            if (eEnd->score <= -THRESH) {
                // すでに終わっているので戻ってみる
                eEnd = std::find_if(std::make_reverse_iterator(eEnd), std::make_reverse_iterator(sEnd),
                    [=](FrameResult r) { return r.score > -THRESH; }).base();
            } else {
                // まだ終わっていないので進んでみる
                eEnd = std::find_if(eEnd, frameResult.end(),
                    [=](FrameResult r) { return r.score <= -THRESH; });
            }
        }

        auto sStart = std::find_if(std::make_reverse_iterator(sEnd),
            std::make_reverse_iterator(it), [=](FrameResult r) { return r.score <= -THRESH; }).base();
        auto eStart = std::find_if(std::make_reverse_iterator(eEnd),
            std::make_reverse_iterator(sEnd), [=](FrameResult r) { return r.score >= THRESH; }).base();

        auto sBest = std::find_if(sStart, sEnd, [](FrameResult r) { return r.score > 0; });
        auto eBest = std::find_if(std::make_reverse_iterator(eEnd),
            std::make_reverse_iterator(eStart), [](FrameResult r) { return r.score > 0; }).base();

        // 区間がある場合だけ
        if (sEnd != eEnd) {
            int sStarti = int(sStart - frameResult.begin());
            int sBesti = int(sBest - frameResult.begin());
            int sEndi = int(sEnd - frameResult.begin());
            int eStarti = int(eStart - frameResult.begin()) - 1;
            int eBesti = int(eBest - frameResult.begin()) - 1;
            int eEndi = int(eEnd - frameResult.begin()) - 1;
            sb.append("%6d S 0 ALL %6d %6d\n", sBesti, sStarti, sEndi);
            sb.append("%6d E 0 ALL %6d %6d\n", eBesti, eStarti, eEndi);
        }

        it = eEnd_;
    }

    File file(outpath, _T("w"));
    file.write(sb.getMC());
}

int logo::LogoFrame::getBestLogo() const {
    return bestLogo;
}

float logo::LogoFrame::getLogoRatio() const {
    return logoRatio;
}

namespace {
    struct AutoDetectRect {
        int x, y, w, h;
    };

    struct AutoDetectStats {
        double sumF, sumB, sumF2, sumB2, sumFB;
        double sumW;
        int rawSampleCount;
        int effectiveBinCount;
        int observed;
        int fgTransition;
        int totalCandidates;
        int rejectedExtreme;
        AutoDetectStats() : sumF(0), sumB(0), sumF2(0), sumB2(0), sumFB(0), sumW(0), rawSampleCount(0), effectiveBinCount(0), observed(0), fgTransition(0), totalCandidates(0), rejectedExtreme(0) {}
    };

    enum class LogoRectDetectFail {
        None,
        InsufficientScorePixels,
        NoSeed,
        BinaryFallbackUsed,
        NoBestComponent,
        RectSizeAbnormal,
    };

    enum class LogoAnalyzeFail {
        None,
        Pass2RoiTooSmall,
        GetLogoNull,
        CorrSequenceInvalid,
        FrameMaskEmpty,
        TooFewAcceptedFrames,
        Pass2RectDiverged,
    };

    static const char* ToString(const LogoRectDetectFail fail) {
        switch (fail) {
        case LogoRectDetectFail::None: return "None";
        case LogoRectDetectFail::InsufficientScorePixels: return "InsufficientScorePixels";
        case LogoRectDetectFail::NoSeed: return "NoSeed";
        case LogoRectDetectFail::BinaryFallbackUsed: return "BinaryFallbackUsed";
        case LogoRectDetectFail::NoBestComponent: return "NoBestComponent";
        case LogoRectDetectFail::RectSizeAbnormal: return "RectSizeAbnormal";
        default: return "Unknown";
        }
    }

    static const char* ToString(const LogoAnalyzeFail fail) {
        switch (fail) {
        case LogoAnalyzeFail::None: return "None";
        case LogoAnalyzeFail::Pass2RoiTooSmall: return "Pass2RoiTooSmall";
        case LogoAnalyzeFail::GetLogoNull: return "GetLogoNull";
        case LogoAnalyzeFail::CorrSequenceInvalid: return "CorrSequenceInvalid";
        case LogoAnalyzeFail::FrameMaskEmpty: return "FrameMaskEmpty";
        case LogoAnalyzeFail::TooFewAcceptedFrames: return "TooFewAcceptedFrames";
        case LogoAnalyzeFail::Pass2RectDiverged: return "Pass2RectDiverged";
        default: return "Unknown";
        }
    }

    static int ClampInt(const int v, const int minv, const int maxv) {
        return std::max(minv, std::min(maxv, v));
    }

    static int RoundDownBy(const int value, const int unit) {
        if (unit <= 1) return value;
        return std::max(0, value / unit * unit);
    }

    static int RoundUpBy(const int value, const int unit) {
        if (unit <= 1) return value;
        return std::max(0, ((value + unit - 1) / unit) * unit);
    }

    static float AutoCalcDist(const float a, const float b) {
        return (1.0f / 3.0f) * (a - 1.0f) * (a - 1.0f) + (a - 1.0f) * b + b * b;
    }

    static float Smoothstep01(const float t) {
        const float x = std::max(0.0f, std::min(1.0f, t));
        return x * x * (3.0f - 2.0f * x);
    }

    static void BoxFilter3x3(std::vector<float>& dst, const std::vector<float>& src, const int w, const int h) {
        dst.resize(w * h);
        for (int y = 0; y < h; y++) {
            const int y0 = std::max(0, y - 1);
            const int y1 = std::min(h - 1, y + 1);
            for (int x = 0; x < w; x++) {
                const int x0 = std::max(0, x - 1);
                const int x1 = std::min(w - 1, x + 1);
                float sum = 0.0f;
                int count = 0;
                for (int yy = y0; yy <= y1; yy++) {
                    for (int xx = x0; xx <= x1; xx++) {
                        sum += src[xx + yy * w];
                        count++;
                    }
                }
                dst[x + y * w] = sum / std::max(1, count);
            }
        }
    }

    static void MedianFilter3x3(std::vector<float>& dst, const std::vector<float>& src, const int w, const int h) {
        dst.resize(w * h);
        std::array<float, 9> window = {};
        for (int y = 0; y < h; y++) {
            const int y0 = std::max(0, y - 1);
            const int y1 = std::min(h - 1, y + 1);
            for (int x = 0; x < w; x++) {
                const int x0 = std::max(0, x - 1);
                const int x1 = std::min(w - 1, x + 1);
                int n = 0;
                for (int yy = y0; yy <= y1; yy++) {
                    for (int xx = x0; xx <= x1; xx++) {
                        window[n++] = src[xx + yy * w];
                    }
                }
                std::sort(window.begin(), window.begin() + n);
                dst[x + y * w] = window[n / 2];
            }
        }
    }

    template<typename pixel_t, int radius>
    static void BilateralFilter(std::vector<pixel_t>& dst, const pixel_t* srcBase, const int srcPitch, const int w, const int h, const float sigmaSpace, const float sigmaRange, const pixel_t maxv, RGYThreadPool* pool = nullptr, const int threadN = 1) {
        static_assert(radius > 0, "radius must be positive");
        dst.resize(w * h);
        constexpr int ksize = radius * 2 + 1;
        std::array<float, ksize * ksize> spatial = {};
        const float inv2SigmaSpace2 = 1.0f / std::max(1e-6f, 2.0f * sigmaSpace * sigmaSpace);
        const float inv2SigmaRange2 = 1.0f / std::max(1e-6f, 2.0f * sigmaRange * sigmaRange);
        for (int dy = -radius; dy <= radius; dy++) {
            for (int dx = -radius; dx <= radius; dx++) {
                const float r2 = (float)(dx * dx + dy * dy);
                spatial[(dx + radius) + (dy + radius) * ksize] = std::exp(-r2 * inv2SigmaSpace2);
            }
        }
        auto filterRange = [&](const int y0, const int y1) {
            for (int y = y0; y < y1; y++) {
                const pixel_t* centerRow = srcBase + y * srcPitch;
                for (int x = 0; x < w; x++) {
                    const int center = centerRow[x];
                    float wsum = 0.0f;
                    float vsum = 0.0f;
                    const float* srow = spatial.data();
                    for (int dy = -radius; dy <= radius; dy++) {
                        const int yy = ClampInt(y + dy, 0, h - 1);
                        const pixel_t* srcRow = srcBase + yy * srcPitch;
                        const float* sw = srow;
                        for (int dx = -radius; dx <= radius; dx++) {
                            const int xx = ClampInt(x + dx, 0, w - 1);
                            const int v = srcRow[xx];
                            const float diff = (float)(v - center);
                            const float rangeW = std::exp(-(diff * diff) * inv2SigmaRange2);
                            const float ww = (*sw++) * rangeW;
                            wsum += ww;
                            vsum += ww * v;
                        }
                        srow += ksize;
                    }
                    const float outv = (wsum > 1e-8f) ? (vsum / wsum) : (float)center;
                    dst[x + y * w] = (pixel_t)ClampInt((int)(outv + 0.5f), 0, maxv);
                }
            }
        };
        if (pool == nullptr || threadN <= 1 || h <= 1) {
            filterRange(0, h);
            return;
        }
        const int workers = std::max(1, std::min(threadN, h));
        const int chunk = (h + workers - 1) / workers;
        std::vector<std::future<void>> futures;
        futures.reserve(workers);
        for (int worker = 0; worker < workers; worker++) {
            const int start = worker * chunk;
            const int end = std::min(h, start + chunk);
            if (start >= end) break;
            futures.push_back(pool->enqueue([&filterRange, start, end]() {
                filterRange(start, end);
            }));
        }
        for (auto& f : futures) {
            f.get();
        }
    }

    static bool TryGetAB(const AutoDetectStats& s, float& A, float& B) {
        if (s.rawSampleCount < 8 || s.sumW < 4.0) {
            return false;
        }

        auto approxWeighted = [](const double n, const double sum_x, const double sum_y, const double sum_x2, const double sum_xy, double& a, double& b) {
            const double temp = n * sum_x2 - sum_x * sum_x;
            if (!std::isfinite(temp) || std::abs(temp) < 1e-12) {
                return false;
            }
            a = (n * sum_xy - sum_x * sum_y) / temp;
            b = (sum_x2 * sum_y - sum_x * sum_xy) / temp;
            return std::isfinite(a) && std::isfinite(b);
        };

        double A1, A2;
        double B1, B2;
        if (!approxWeighted(s.sumW, s.sumF, s.sumB, s.sumF2, s.sumFB, A1, B1)) {
            return false;
        }
        if (!approxWeighted(s.sumW, s.sumB, s.sumF, s.sumB2, s.sumFB, A2, B2)) {
            return false;
        }
        if (!std::isfinite(A2) || std::abs(A2) < 1e-12) {
            return false;
        }

        A = (float)((A1 + (1.0 / A2)) * 0.5);
        B = (float)((B1 + (-B2 / A2)) * 0.5);

        return !(std::isnan(A) || std::isinf(A) || std::isnan(B) || std::isinf(B) || std::abs(A) < 1e-8f);
    }

    static bool TryGetAlphaLogo(const float A, const float B, float& alpha, float& logoY) {
        if (!std::isfinite(A) || !std::isfinite(B) || A <= 1e-6f) {
            return false;
        }
        alpha = 1.0f - (1.0f / A);
        if (!std::isfinite(alpha)) {
            return false;
        }
        if (alpha < -0.05f || alpha > 1.05f) {
            return false;
        }
        const float denom = A - 1.0f;
        if (std::abs(denom) < 1e-6f) {
            logoY = 0.0f;
            return alpha < 1e-4f;
        }
        logoY = -B / denom;
        if (!std::isfinite(logoY)) {
            return false;
        }
        return true;
    }

    static bool IsExtremeContrastSample(const double fg, const double bg) {
        static const double kDiffThreshold = 0.20;
        // Y は 16-235 の範囲であることが前提
        static const double kBlack = 25.0 / 255.0;
        static const double kWhite = 225.0 / 255.0;
        const double diff = std::abs(fg - bg);
        if (diff < kDiffThreshold) {
            return false;
        }
        const bool fgExtreme = (fg <= kBlack || fg >= kWhite);
        const bool bgExtreme = (bg <= kBlack || bg >= kWhite);
        return fgExtreme || bgExtreme;
    }

    static bool TryFindLongestRun(const std::vector<uint8_t>& arr, int& start, int& end) {
        start = 0;
        end = -1;
        int bestLen = 0;
        int curStart = -1;
        for (int i = 0; i < (int)arr.size(); i++) {
            if (arr[i]) {
                if (curStart < 0) curStart = i;
            } else if (curStart >= 0) {
                const int len = i - curStart;
                if (len > bestLen) {
                    bestLen = len;
                    start = curStart;
                    end = i - 1;
                }
                curStart = -1;
            }
        }
        if (curStart >= 0) {
            const int len = (int)arr.size() - curStart;
            if (len > bestLen) {
                bestLen = len;
                start = curStart;
                end = (int)arr.size() - 1;
            }
        }
        return bestLen > 0;
    }

    struct BgDebugInfo {
        int sideCount;
        uint8_t sideValid[4];
        float sideMin[4];
        float sideMax[4];
        float sideAvg[4];
    };

    // TryEstimateBg 内で使う作業状態をまとめた構造体。
    // sideValid は「単色判定に通ったか」、sideEvaluated は「評価済みか」を保持する。
    // firstSide は最初に有効だった辺で、以降の評価順(反対側を最後)を決める起点になる。
    template<typename pixel_t>
    struct TryEstimateBgState {
        float sideAvg[4] = {};
        pixel_t sideMin[4] = {};
        pixel_t sideMax[4] = {};
        uint8_t sideValid[4] = {};
        uint8_t sideEvaluated[4] = {};
        int sideCount = 0;
        float sideSum = 0.0f;
        int firstSide = -1;
    };

    // 1辺分の画素列を評価し、平均/最小/最大を返す。
    // max-min が threshold 以下なら「近い色(ほぼ単色)」として true を返す。
    // 範囲外を含む場合は clamp 参照で安全に評価する。
    template<typename pixel_t>
    static bool TryEstimateBgEvalSide(const std::vector<pixel_t>& y, const int w, const int h, const int sx, const int sy, const int dx, const int dy, const int sideLen, const float threshold, float& avg, pixel_t& minvOut, pixel_t& maxvOut) {
        pixel_t minv = std::numeric_limits<pixel_t>::max();
        pixel_t maxv = std::numeric_limits<pixel_t>::lowest();
        int sum = 0;
        const int ex = sx + dx * (sideLen - 1);
        const int ey = sy + dy * (sideLen - 1);
        const bool inRange = sx >= 0 && sx < w && sy >= 0 && sy < h && ex >= 0 && ex < w && ey >= 0 && ey < h;
        if (inRange) {
            const pixel_t* ptr = &y[sx + sy * w];
            const int step = dx + dy * w;
            for (int i = 0; i < sideLen; i++, ptr += step) {
                const pixel_t v = *ptr;
                minv = std::min(minv, v);
                maxv = std::max(maxv, v);
                sum += v;
            }
        } else {
            for (int i = 0; i < sideLen; i++) {
                const int px = ClampInt(sx + dx * i, 0, w - 1);
                const int py = ClampInt(sy + dy * i, 0, h - 1);
                const pixel_t v = y[px + py * w];
                minv = std::min(minv, v);
                maxv = std::max(maxv, v);
                sum += v;
            }
        }
        avg = (float)sum / sideLen;
        minvOut = minv;
        maxvOut = maxv;
        return ((float)maxv - (float)minv) <= threshold;
    }

    // 指定辺の「基準位置」での評価を実施する。
    // 既評価なら再計算せずキャッシュ結果(sideValid)を返す。
    template<typename pixel_t>
    static bool TryEstimateBgEvalBaseSide(const std::vector<pixel_t>& y, const int w, const int h, const int side, const int baseSx[4], const int baseSy[4], const int sideDx[4], const int sideDy[4], const int sideLen, const float threshold, TryEstimateBgState<pixel_t>& st) {
        if (st.sideEvaluated[side]) {
            return st.sideValid[side] != 0;
        }
        const bool ok = TryEstimateBgEvalSide(y, w, h, baseSx[side], baseSy[side], sideDx[side], sideDy[side], sideLen, threshold, st.sideAvg[side], st.sideMin[side], st.sideMax[side]);
        st.sideEvaluated[side] = 1;
        st.sideValid[side] = ok ? 1 : 0;
        return ok;
    }

    // firstSide と指定 side の平均輝度差が閾値以内なら「同じ近い色の2辺」とみなして確定する。
    // 確定時は有効辺を2辺に絞り、sideCount/sideSum を確定値に更新する。
    template<typename pixel_t>
    static bool TryEstimateBgTryPairWithFirst(const int side, const float threshold, TryEstimateBgState<pixel_t>& st) {
        if (st.firstSide < 0 || !st.sideValid[side]) {
            return false;
        }
        if (std::abs(st.sideAvg[side] - st.sideAvg[st.firstSide]) > threshold) {
            return false;
        }
        for (int i = 0; i < 4; i++) {
            st.sideValid[i] = (i == st.firstSide || i == side) ? 1 : 0;
        }
        st.sideCount = 2;
        st.sideSum = st.sideAvg[st.firstSide] + st.sideAvg[side];
        return true;
    }

    // firstSide が決まった後の評価順を作る。
    // ルール: firstSide の反対側は最後に回し、残り2辺を先に試す。
    // 通常時・救済時の両方で同じ順序規則を使う。
    static void TryEstimateBgBuildRetryOrder(const int firstSide, int order[3]) {
        static constexpr int opposite[4] = { 1, 0, 3, 2 };
        int idx = 0;
        for (int side = 0; side < 4; side++) {
            if (side == firstSide || side == opposite[firstSide]) {
                continue;
            }
            order[idx++] = side;
        }
        order[2] = opposite[firstSide];
    }

    // firstSide 決定後、基準位置のまま残り3辺を評価して 2辺一致を探す。
    // 評価順は TryEstimateBgBuildRetryOrder に従う。
    template<typename pixel_t>
    static bool TryEstimateBgTryRemainingBase(const std::vector<pixel_t>& y, const int w, const int h, const int baseSx[4], const int baseSy[4], const int sideDx[4], const int sideDy[4], const int sideLen, const float threshold, TryEstimateBgState<pixel_t>& st) {
        int order[3] = {};
        TryEstimateBgBuildRetryOrder(st.firstSide, order);
        for (int i = 0; i < 3; i++) {
            const int side = order[i];
            if (!TryEstimateBgEvalBaseSide(y, w, h, side, baseSx, baseSy, sideDx, sideDy, sideLen, threshold, st)) {
                continue;
            }
            if (TryEstimateBgTryPairWithFirst(side, threshold, st)) {
                return true;
            }
        }
        return false;
    }

    // 通常時の探索。
    // 最初の有効辺は「上→下→左→右」の順で決定し、その後は共通ルールで残り辺を評価する。
    template<typename pixel_t>
    static bool TryEstimateBgFindInitialPair(const std::vector<pixel_t>& y, const int w, const int h, const int baseSx[4], const int baseSy[4], const int sideDx[4], const int sideDy[4], const int sideLen, const float threshold, TryEstimateBgState<pixel_t>& st) {
        // 通常時は 上 -> 下 -> 左 -> 右 の順で最初の有効辺を探す。
        // 最初の有効辺が見つかったら、以後は「反対側を最後」に回して2辺一致を探索する。
        const int initialOrder[4] = { 0, 1, 2, 3 };
        for (int i = 0; i < 4; i++) {
            const int side = initialOrder[i];
            if (!TryEstimateBgEvalBaseSide(y, w, h, side, baseSx, baseSy, sideDx, sideDy, sideLen, threshold, st)) {
                continue;
            }
            st.firstSide = side;
            return TryEstimateBgTryRemainingBase(y, w, h, baseSx, baseSy, sideDx, sideDy, sideLen, threshold, st);
        }
        return false;
    }

    // 1辺しか取れない場合の救済探索。
    // firstSide 以外の辺を外側にシフトして再評価し、2辺一致を探す。
    // 評価順は通常時と同じ規則(反対側を最後)を使う。
    template<typename pixel_t>
    static bool TryEstimateBgFindRetryPair(const std::vector<pixel_t>& y, const int w, const int h, const int sideLen, const float threshold, const int baseSx[4], const int baseSy[4], const int sideDx[4], const int sideDy[4], const int retryOutX[4], const int retryOutY[4], TryEstimateBgState<pixel_t>& st) {
        constexpr int kSideRetryCount = 1;
        constexpr int kSideRetryShift = 16;
        int order[3] = {};
        TryEstimateBgBuildRetryOrder(st.firstSide, order);
        for (int retry = 0; retry < kSideRetryCount; retry++) {
            const int shift = kSideRetryShift * (retry + 1);
            for (int i = 0; i < 3; i++) {
                const int side = order[i];
                const int sx = baseSx[side] + retryOutX[side] * shift;
                const int sy = baseSy[side] + retryOutY[side] * shift;
                const int ex = sx + sideDx[side] * (sideLen - 1);
                const int ey = sy + sideDy[side] * (sideLen - 1);
                if (sx < 0 || sx >= w || sy < 0 || sy >= h || ex < 0 || ex >= w || ey < 0 || ey >= h) {
                    continue;
                }
                const bool ok = TryEstimateBgEvalSide(y, w, h, sx, sy, sideDx[side], sideDy[side], sideLen, threshold, st.sideAvg[side], st.sideMin[side], st.sideMax[side]);
                st.sideEvaluated[side] = 1;
                st.sideValid[side] = ok ? 1 : 0;
                if (ok && TryEstimateBgTryPairWithFirst(side, threshold, st)) {
                    return true;
                }
            }
        }
        return false;
    }

    // pair が確定できなかったときの後処理。
    // その時点で有効だった辺を集計し、sideCount/sideSum を算出する。
    template<typename pixel_t>
    static void TryEstimateBgFinalizeSideStats(TryEstimateBgState<pixel_t>& st) {
        st.sideCount = 0;
        st.sideSum = 0.0f;
        for (int i = 0; i < 4; i++) {
            if (st.sideValid[i]) {
                st.sideCount++;
                st.sideSum += st.sideAvg[i];
            }
        }
    }

    // 画素(x, y0)の背景輝度を、周辺ブロック外周4辺から推定する。
    // 方針:
    // - 半径radiusの正方ブロックを取り、その外周(上/下/左/右)を候補として評価
    // - 辺内のmin-maxがthreshold以下の「ほぼ単色の辺」だけを背景候補として採用
    // - 採用できた辺の平均を背景bgとする
    // 背景:
    // - ロゴ混入やテクスチャの強い辺を除外し、安定した背景推定点だけを使うため
    template<typename pixel_t>
    static bool TryEstimateBg(const std::vector<pixel_t>& y, const int w, const int h, const int x, const int y0, const int radius, const float threshold, float& bg, BgDebugInfo* dbg = nullptr) {
        const int baseSx[4] = { x - radius, x - radius, x - radius, x + radius };
        const int baseSy[4] = { y0 - radius, y0 + radius, y0 - radius, y0 - radius };
        const int sideDx[4] = { 1, 1, 0, 0 };
        const int sideDy[4] = { 0, 0, 1, 1 };
        // 上下左右の外側方向オフセット（top/bottom/left/right）
        const int retryOutX[4] = { 0, 0, -1, 1 };
        const int retryOutY[4] = { -1, 1, 0, 0 };
        const int sideLen = radius * 2 + 1;

        TryEstimateBgState<pixel_t> st;
        bool pairFound = TryEstimateBgFindInitialPair(y, w, h, baseSx, baseSy, sideDx, sideDy, sideLen, threshold, st);
        if (!pairFound && st.firstSide >= 0) {
            // 1辺しか取れないときの救済:
            // 最初の1辺を基準に、反対側を最後にする順で非基準辺を外側へ移動して再評価する。
            pairFound = TryEstimateBgFindRetryPair(y, w, h, sideLen, threshold, baseSx, baseSy, sideDx, sideDy, retryOutX, retryOutY, st);
        }
        if (!pairFound) {
            TryEstimateBgFinalizeSideStats(st);
        }

        if (dbg) {
            dbg->sideCount = st.sideCount;
            for (int i = 0; i < 4; i++) {
                dbg->sideValid[i] = st.sideValid[i];
                dbg->sideMin[i] = (float)st.sideMin[i];
                dbg->sideMax[i] = (float)st.sideMax[i];
                dbg->sideAvg[i] = st.sideAvg[i];
            }
        }

        if (st.sideCount < 2) {
            return false;
        }

        bg = st.sideSum / st.sideCount;
        return true;
    }

    static void RunParallelRange(RGYThreadPool& pool, const int threadN, const int total, const std::function<void(int, int)>& fn, const int blockSize = 0) {
        const int workers = std::max(1, std::min(threadN, total));
        if (workers <= 1 || total <= 1) {
            fn(0, total);
            return;
        }
        std::vector<std::future<void>> futures;
        const int chunk = (blockSize > 0) ? blockSize : (total + workers - 1) / workers;
        const int numTasks = (total + chunk - 1) / chunk;
        futures.reserve(numTasks);
        for (int task = 0; task < numTasks; task++) {
            const int start = task * chunk;
            const int end = std::min(total, start + chunk);
            if (start >= end) {
                break;
            }
            futures.push_back(pool.enqueue([&fn, start, end]() {
                fn(start, end);
            }));
        }
        for (auto& f : futures) {
            f.get();
        }
    }

    struct ScoreDebugStats {
        double minv;
        double maxv;
        double mean;
        double p50;
        double p90;
        double p99;
    };

    static ScoreDebugStats CalcScoreDebugStats(const std::vector<float>& score, const std::vector<uint8_t>& valid) {
        ScoreDebugStats st{ 0, 0, 0, 0, 0, 0 };
        std::vector<double> vals;
        vals.reserve(score.size());
        for (int i = 0; i < (int)score.size(); i++) {
            if (i < (int)valid.size() && valid[i]) {
                vals.push_back(score[i]);
            }
        }
        if (vals.empty()) {
            return st;
        }
        std::sort(vals.begin(), vals.end());
        st.minv = vals.front();
        st.maxv = vals.back();
        st.mean = std::accumulate(vals.begin(), vals.end(), 0.0) / vals.size();
        auto getPerc = [&](double p) {
            const int idx = (int)std::round((vals.size() - 1) * p);
            return vals[ClampInt(idx, 0, (int)vals.size() - 1)];
        };
        st.p50 = getPerc(0.50);
        st.p90 = getPerc(0.90);
        st.p99 = getPerc(0.99);
        return st;
    }

    static void CalcRangeValid(const std::vector<float>& values, const std::vector<uint8_t>& valid, float& minv, float& maxv, const float fallbackMin, const float fallbackMax) {
        minv = std::numeric_limits<float>::max();
        maxv = std::numeric_limits<float>::lowest();
        for (int i = 0; i < (int)values.size(); i++) {
            if (i >= (int)valid.size() || !valid[i]) continue;
            const float v = values[i];
            if (!std::isfinite(v)) continue;
            minv = std::min(minv, v);
            maxv = std::max(maxv, v);
        }
        if (!std::isfinite(minv) || !std::isfinite(maxv) || minv >= maxv) {
            minv = fallbackMin;
            maxv = fallbackMax;
        }
    }

    static float PercentileOfSorted(const std::vector<float>& sortedVals, const float p) {
        if (sortedVals.empty()) {
            return 0.0f;
        }
        const int idx = ClampInt((int)std::round((sortedVals.size() - 1) * std::max(0.0f, std::min(1.0f, p))), 0, (int)sortedVals.size() - 1);
        return sortedVals[idx];
    }

    static bool ParseEnvBoolDefault(const char* name, const bool defaultValue) {
        const char* v = std::getenv(name);
        if (v == nullptr || v[0] == '\0') {
            return defaultValue;
        }
        if (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 || std::strcmp(v, "TRUE") == 0
            || std::strcmp(v, "on") == 0 || std::strcmp(v, "ON") == 0 || std::strcmp(v, "yes") == 0 || std::strcmp(v, "YES") == 0) {
            return true;
        }
        if (std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0 || std::strcmp(v, "FALSE") == 0
            || std::strcmp(v, "off") == 0 || std::strcmp(v, "OFF") == 0 || std::strcmp(v, "no") == 0 || std::strcmp(v, "NO") == 0) {
            return false;
        }
        return defaultValue;
    }

    static std::vector<std::pair<int, int>> ParseEnvPointList(const char* name) {
        std::vector<std::pair<int, int>> points;
        const char* v = std::getenv(name);
        if (v == nullptr || v[0] == '\0') {
            return points;
        }
        const std::string s(v);
        const std::regex re(R"((-?\d+)\s*,\s*(-?\d+))");
        for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it) {
            const int x = std::atoi((*it)[1].str().c_str());
            const int y = std::atoi((*it)[2].str().c_str());
            points.emplace_back(x, y);
        }
        return points;
    }

    static bool EstimateGaussianMixturePosterior1D(const std::vector<float>& samples, std::vector<float>& posteriorLogoOut) {
        if (samples.size() < 32) {
            return false;
        }
        std::vector<float> sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        const float p15 = PercentileOfSorted(sorted, 0.15f);
        const float p85 = PercentileOfSorted(sorted, 0.85f);
        const float p50 = PercentileOfSorted(sorted, 0.50f);
        const float m0Init = std::min(p15, p50);
        const float m1Init = std::max(p85, p50 + 1e-4f);
        double mean = 0.0;
        for (const auto v : samples) {
            mean += v;
        }
        mean /= samples.size();
        double var = 0.0;
        for (const auto v : samples) {
            const double d = v - mean;
            var += d * d;
        }
        var = std::max(1e-4, var / samples.size());
        double m0 = m0Init;
        double m1 = m1Init;
        double v0 = std::max(2e-4, var * 0.5);
        double v1 = std::max(2e-4, var * 0.5);
        double pi1 = 0.5;

        std::vector<double> gamma(samples.size(), 0.5);
        for (int iter = 0; iter < 24; iter++) {
            double sumGamma = 0.0;
            double sumOneMinus = 0.0;
            double sumGammaX = 0.0;
            double sumOneMinusX = 0.0;
            for (int i = 0; i < (int)samples.size(); i++) {
                const double x = samples[i];
                const double p1 = pi1 * std::exp(-0.5 * (x - m1) * (x - m1) / v1) / std::sqrt(v1);
                const double p0 = (1.0 - pi1) * std::exp(-0.5 * (x - m0) * (x - m0) / v0) / std::sqrt(v0);
                const double norm = std::max(1e-12, p0 + p1);
                const double g = p1 / norm;
                gamma[i] = g;
                sumGamma += g;
                sumOneMinus += (1.0 - g);
                sumGammaX += g * x;
                sumOneMinusX += (1.0 - g) * x;
            }
            if (sumGamma < 1e-3 || sumOneMinus < 1e-3) {
                return false;
            }
            m1 = sumGammaX / sumGamma;
            m0 = sumOneMinusX / sumOneMinus;
            double numV1 = 0.0;
            double numV0 = 0.0;
            for (int i = 0; i < (int)samples.size(); i++) {
                const double x = samples[i];
                const double g = gamma[i];
                numV1 += g * (x - m1) * (x - m1);
                numV0 += (1.0 - g) * (x - m0) * (x - m0);
            }
            v1 = std::max(1e-4, numV1 / sumGamma);
            v0 = std::max(1e-4, numV0 / sumOneMinus);
            pi1 = std::max(0.05, std::min(0.95, sumGamma / samples.size()));
        }

        if (m1 < m0) {
            for (auto& g : gamma) {
                g = 1.0 - g;
            }
            std::swap(m0, m1);
        }

        posteriorLogoOut.resize(samples.size());
        for (int i = 0; i < (int)samples.size(); i++) {
            posteriorLogoOut[i] = (float)std::max(0.0, std::min(1.0, gamma[i]));
        }
        return true;
    }

    class AutoDetectLogoReader : logo::SimpleVideoReader {
        const int serviceid;
        const int divx;
        const int divy;
        const int searchFrames;
        const int blockSize;
        const int threshold;
        const int marginX;
        const int marginY;
        const int threadN;
        const bool detailedDebug;
        logo::LOGO_AUTODETECT_CB cb;
        RGYThreadPool threadPool;

        int imgw;
        int imgh;
        int scanx;
        int scany;
        int scanw;
        int scanh;
        int radius;
        int bitDepth;
        int logUVx;
        int logUVy;
        int framesPerSec;
        int readFrames;
        static constexpr int kScanEdgeMargin = 16;
        // fg値をkHistBins個のbinに分割してサンプル蓄積する
        static constexpr int kHistBins = 32;
        static constexpr bool kEnableTwoPassFrameGate = true;
        static constexpr bool kEnablePruneBinaryByAnchor = true;
        const bool enableTwoPassFrameGate;
        const bool enablePruneBinaryByAnchor;
        const std::vector<std::pair<int, int>> tracePointEnv;

        struct TracePointConfig {
            int id = 0;
            int x = 0; // scan-local
            int y = 0; // scan-local
        };
        struct TraceSampleRecord {
            int pass = 0;
            int frame = 0;
            int pointId = 0;
            int x = 0;
            int y = 0;
            int absX = 0;
            int absY = 0;
            float fgRaw = 0.0f;
            float fgFiltered = 0.0f;
            float bg = 0.0f;
            int bgOk = 0;
            int bgSideCount = 0;
            int bgSideValidMask = 0;
            float bgSideRangeMax = 0.0f;
            float bgSideAvgSpread = 0.0f;
            float fgBgDiff = 0.0f;
            int histBin = -1;
            int extreme = 0;
            int accepted = 0;
            int rejectCode = 0; // 0:accepted,1:bg_fail,2:extreme,3:pass3_mask,4:edge_skip
            int observedAfter = 0;
            int countAfter = 0;
            int totalCandidatesAfter = 0;
        };
        struct TraceBinRepresentativeRecord {
            int pointId = 0;
            int x = 0;
            int y = 0;
            int absX = 0;
            int absY = 0;
            int histBin = 0;
            int count = 0;
            float avgFg = 0.0f;
            float rawBg = 0.0f;
            float adjustedBg = 0.0f;
            float weightScale = 1.0f;
            float provisionalA = 0.0f;
            float provisionalB = 0.0f;
            float provisionalConsistency = 0.0f;
        };
        std::vector<TracePointConfig> tracePoints;
        std::vector<int> tracePointIndexByOffset;

        // mode1 はサンプルを fg の近い値ごとに束ねてから回帰へ渡す。
        // 静止した不透明枠や固定文字が大量に混ざると、そのままでは
        // 「同じような fg のサンプル数が多いだけ」で回帰を引っ張ってしまうため、
        // 画素生サンプルを直接使わず binAccum + Gompertz で代表点へ圧縮する。
        struct BinAccum {
            int    count;
            double sum_fg;
            double sum_bg;
            double sum_weight;
            double sum_weighted_fg;
            double sum_weighted_bg;
            BinAccum() : count(0), sum_fg(0.0), sum_bg(0.0), sum_weight(0.0), sum_weighted_fg(0.0), sum_weighted_bg(0.0) {}
        };

        // 空間edge時系列統計の蓄積バッファ (画素ごと)
        struct SpatialEdgeAccum {
            float sumEdge;   // 正方向 corrected edge の和
            float sumEdge2;  // 正方向 corrected edge の二乗和
            int   edgeCount; // 正方向 edge が存在したフレーム数
            SpatialEdgeAccum() : sumEdge(0.0f), sumEdge2(0.0f), edgeCount(0) {}
        };

        struct StatsPassBuffers {
            std::vector<uint8_t> frameWork8;
            std::vector<uint16_t> frameWork16;
            std::vector<AutoDetectStats> stats;
            // ヒストグラムbin蓄積バッファ (scanw * scanh * kHistBins)
            std::vector<BinAccum> binAccumBuf;
            std::vector<float> lastObservedFg;
            std::vector<uint8_t> lastObservedValid;
            std::vector<int> frameValidCounts;
            std::vector<TraceSampleRecord> traceRecords;
            // 空間edge時系列統計バッファ (scanw * scanh)
            std::vector<SpatialEdgeAccum> edgeAccumBuf;

            void reset(const int scanw, const int scanh, const int bitDepth) {
                if (bitDepth <= 8) {
                    frameWork8.resize(scanw * scanh);
                    frameWork16.clear();
                    frameWork16.shrink_to_fit();
                } else {
                    frameWork16.resize(scanw * scanh);
                    frameWork8.clear();
                    frameWork8.shrink_to_fit();
                }
                stats.assign(scanw * scanh, AutoDetectStats());
                binAccumBuf.assign(scanw * scanh * kHistBins, BinAccum());
                lastObservedFg.assign(scanw * scanh, 0.0f);
                lastObservedValid.assign(scanw * scanh, 0);
                frameValidCounts.clear();
                traceRecords.clear();
                edgeAccumBuf.assign(scanw * scanh, SpatialEdgeAccum());
            }
        };

        std::vector<AutoDetectStats> debugStats;
        std::vector<TraceSampleRecord> debugTraceRecords;
        std::vector<TraceBinRepresentativeRecord> debugTraceBinRepresentatives;
        std::vector<uint8_t> provisionalLineValid;
        std::vector<float> provisionalLineA;
        std::vector<float> provisionalLineB;
        std::vector<float> provisionalLineConsistency;
        std::vector<float> provisionalLineStdDiff;
        bool sampleResidualReweightActive = false;

        struct ScoreStageBuffers {
            std::vector<float> score;
            std::vector<float> mapA;
            std::vector<float> mapB;
            std::vector<float> mapAlpha;
            std::vector<float> mapLogoY;
            std::vector<float> mapMeanDiff;
            std::vector<float> mapConsistency;
            std::vector<float> mapFgVar;
            std::vector<float> mapBgVar;
            std::vector<float> mapTransitionRate;
            std::vector<float> mapKeepRate;
            std::vector<float> mapAccepted;
            std::vector<float> mapDiffGain;
            std::vector<float> mapDiffGainRaw;
            std::vector<float> mapResidualGain;
            std::vector<float> mapLogoGain;
            std::vector<float> mapConsistencyGain;
            std::vector<float> mapAlphaGain;
            std::vector<float> mapBgGain;
            std::vector<float> mapExtremeGain;
            std::vector<float> mapTemporalGain;
            std::vector<float> mapOpaquePenalty;
            std::vector<float> mapOpaqueStaticPenalty;
            // 空間edge時系列統計マップ
            std::vector<float> mapEdgePresence;
            std::vector<float> mapEdgeMean;
            std::vector<float> mapEdgeVar;
            std::vector<float> mapRescueScore;
            // rescue score 中間ゲインマップ (診断用)
            std::vector<float> mapPresenceGain;  // 存在率ゲイン
            std::vector<float> mapMagGain;        // 大きさゲイン
            std::vector<float> mapUpperGate;      // 上限ゲート (不透明構造除外)
            std::vector<float> mapConsistGain;    // 一貫性ゲイン (低分散ほど高い)
            std::vector<float> mapBgVarGain;      // 背景分散ゲイン (不透明テロップ除外)
            std::vector<uint8_t> mapIsWall;       // 壁マスク (膨張後)
            std::vector<uint8_t> mapIsInterior;   // テロップ内部マスク (flood fill 結果)
            std::vector<float> mapUpperGateFilled; // upperGate (壁・内部を 0 に抑制済み)
            std::vector<uint8_t> validAB;

            void reset(const int scanw, const int scanh) {
                const int n = scanw * scanh;
                score.assign(n, 0.0f);
                validAB.assign(n, 0);
                mapA.assign(n, 0.0f);
                mapB.assign(n, 0.0f);
                mapAlpha.assign(n, 0.0f);
                mapLogoY.assign(n, 0.0f);
                mapMeanDiff.assign(n, 0.0f);
                mapConsistency.assign(n, 0.0f);
                mapFgVar.assign(n, 0.0f);
                mapBgVar.assign(n, 0.0f);
                mapTransitionRate.assign(n, 0.0f);
                mapKeepRate.assign(n, 0.0f);
                mapAccepted.assign(n, 0.0f);
                mapDiffGain.assign(n, 0.0f);
                mapDiffGainRaw.assign(n, 0.0f);
                mapResidualGain.assign(n, 0.0f);
                mapLogoGain.assign(n, 0.0f);
                mapConsistencyGain.assign(n, 0.0f);
                mapAlphaGain.assign(n, 0.0f);
                mapBgGain.assign(n, 0.0f);
                mapExtremeGain.assign(n, 0.0f);
                mapTemporalGain.assign(n, 0.0f);
                mapOpaquePenalty.assign(n, 0.0f);
                mapOpaqueStaticPenalty.assign(n, 0.0f);
                mapEdgePresence.assign(n, 0.0f);
                mapEdgeMean.assign(n, 0.0f);
                mapEdgeVar.assign(n, 0.0f);
                mapRescueScore.assign(n, 0.0f);
                mapPresenceGain.assign(n, 0.0f);
                mapMagGain.assign(n, 0.0f);
                mapUpperGate.assign(n, 0.0f);
                mapConsistGain.assign(n, 0.0f);
                mapBgVarGain.assign(n, 0.0f);
                mapIsWall.assign(n, 0);
                mapIsInterior.assign(n, 0);
                mapUpperGateFilled.assign(n, 0.0f);
            }
        };

        ScoreStageBuffers debugScore;
        struct BinaryStageBuffers {
            std::vector<uint8_t> binary;
        };
        std::vector<uint8_t> debugBinary;
        struct DebugStageSnapshot {
            std::vector<AutoDetectStats> stats;
            std::vector<TraceSampleRecord> traceRecords;
            std::vector<TraceBinRepresentativeRecord> traceBinRepresentatives;
            ScoreStageBuffers score;
            std::vector<uint8_t> binary;
            AutoDetectRect rectAbs{ 0, 0, 0, 0 };
            AutoDetectRect rectLocal{ 0, 0, 0, 0 };
            bool valid = false;
        };
        struct DebugPathSet {
            std::string score;
            std::string binary;
            std::string ccl;
            std::string count;
            std::string a;
            std::string b;
            std::string alpha;
            std::string logoY;
            std::string meanDiff;
            std::string consistency;
            std::string fgVar;
            std::string bgVar;
            std::string transition;
            std::string keepRate;
            std::string accepted;
            std::string diffGain;
            std::string diffGainRaw;
            std::string residualGain;
            std::string logoGain;
            std::string consistencyGain;
            std::string alphaGain;
            std::string bgGain;
            std::string extremeGain;
            std::string temporalGain;
            std::string opaquePenalty;
            std::string opaqueStaticPenalty;
            std::string tracePlot;
            // 空間edge時系列統計デバッグパス
            std::string edgePresence;
            std::string edgeMean;
            std::string edgeVar;
            std::string rescueScore;
            // rescue score 中間ゲインデバッグパス (診断用)
            std::string presenceGain;
            std::string magGain;
            std::string upperGate;
            std::string consistGain;
            std::string bgVarGain;
            std::string isWall;
            std::string isInterior;
            std::string upperGateFilled;
        };
        struct Pass2PrepareDebug {
            int logoW = 0;
            int logoH = 0;
            std::vector<float> logoA;
            std::vector<float> logoB;
            std::vector<uint8_t> logoMask;
            std::vector<float> corr0;
            std::vector<float> corr1;
            std::vector<float> raw;
            std::vector<float> median;
            std::vector<int> judge;
            std::vector<uint8_t> frameMask;

            void clear() {
                logoW = 0;
                logoH = 0;
                logoA.clear();
                logoB.clear();
                logoMask.clear();
                corr0.clear();
                corr1.clear();
                raw.clear();
                median.clear();
                judge.clear();
                frameMask.clear();
            }
        };
        DebugStageSnapshot debugPass1;
        DebugStageSnapshot debugPass2;
        Pass2PrepareDebug debugPass2Prepare;
        int passIndex;
        struct IterThresholdDebug {
            float highTh;
            float lowTh;
            int seedOnCount;
            int lowOnCount;
            int grownOnCount;
            int promotedOnCount;
            int newPixelCount;
            int nearCompCount;
            int farCompCount;
            int acceptedOnCount;
            int stopReason; // 0:continue, 1:stop_far_only, 2:no_delta
        };
        struct PromoteCompDebug {
            int iter;
            float highTh;
            float lowTh;
            int minX;
            int minY;
            int maxX;
            int maxY;
            int area;
            int compW;
            int compH;
            int overlapW;
            int overlapH;
            int gapX;
            int gapY;
            float peakScore;
            float meanAccepted;
            int nearHorizontal;
            int nearVertical;
            int nearDiagonal;
            int nearAnchor;
            int shapeOk;
            int signalOk;
            int accepted;
        };
        struct DeltaCompDebug {
            int iter;
            float highTh;
            float lowTh;
            int minX;
            int minY;
            int maxX;
            int maxY;
            int compW;
            int compH;
            int overlapW;
            int overlapH;
            int gapX;
            int gapY;
            int nearHorizontal;
            int nearVertical;
            int nearDiagonal;
            int nearInitial;
            int withinInitialGuard;
            int accepted;
        };
        struct RectMergeDebug {
            int iter;
            int minX;
            int minY;
            int maxX;
            int maxY;
            int area;
            int overlapW;
            int overlapH;
            int gapX;
            int gapY;
            int nearHorizontal;
            int nearVertical;
            int nearDiagonal;
            int overlap;
            int withinSeedCenterGuard;
            int withinFinalCenterGuard;
            int sizeGuardOk;
            int accepted;
        };
        std::vector<std::vector<uint8_t>> iterBinaryHistory;
        std::vector<IterThresholdDebug> iterThresholdDebug;
        std::vector<PromoteCompDebug> promoteCompDebug;
        std::vector<DeltaCompDebug> deltaCompDebug;
        std::vector<RectMergeDebug> rectMergeDebug;
        int debugAbsX;
        int debugAbsY;

        AutoDetectRect rectAbs;
        AutoDetectRect rectLocal;
        LogoRectDetectFail rectDetectFail;
        LogoAnalyzeFail logoAnalyzeFail;
        int scoreValidPixelCount;
        int scorePositivePixelCount;
        int initialSeedCount;
        int initialGrownCount;
        bool usedBinaryFallback;
        struct Pass2Buffers {
            AutoDetectRect logoRectAbs{ 0, 0, 0, 0 };
            std::unique_ptr<logo::LogoScan> logoScan;
            std::unique_ptr<logo::LogoDataParam> deintLogo;
            std::vector<float> corr0;
            std::vector<float> corr1;
            std::vector<float> evalDeint;
            std::vector<float> evalWork;
            std::vector<uint8_t> frameMask;
            int acceptedFrames = 0;
            int skippedFrames = 0;
            int addFrameRejected = 0;
        };

        struct BuildBinaryDiag {
            int seedOn = 0;
            int lowOn = 0;
            int grownOn = 0;
            int promotedOn = 0;
        };

        struct RectStageCompCandidate {
            AutoDetectRect rect;
            double score;
            int area;
        };

        bool getMaskRect(const std::vector<uint8_t>& mask, int& minX, int& minY, int& maxX, int& maxY) const;
        int countMaskOn(const std::vector<uint8_t>& mask) const;
        void buildBinaryFromThreshold(const ScoreStageBuffers& scoreStage, const int iterIndex, const float highTh, const float lowTh, std::vector<uint8_t>& outBinary, BuildBinaryDiag* dbg);
        void pruneBinaryByAnchor(const ScoreStageBuffers& scoreStage, BinaryStageBuffers& binaryStage);
        bool applyBinaryFallbackIfEmpty(const ScoreStageBuffers& scoreStage, BinaryStageBuffers& binaryStage);

        void collectBinaryProjection(std::vector<uint8_t>& xOn, std::vector<uint8_t>& yOn, const BinaryStageBuffers& binaryStage) const;
        void collectRectCandidates(std::vector<RectStageCompCandidate>& candidates, std::vector<RectStageCompCandidate>& mergeCandidates, AutoDetectRect& best, bool& hasBest, double& bestScore, const ScoreStageBuffers& scoreStage, const BinaryStageBuffers& binaryStage);
        AutoDetectRect buildAcceptedFallbackRect(const ScoreStageBuffers& scoreStage) const;
        void mergeRectCandidates(const std::vector<RectStageCompCandidate>& mergeCandidates, const AutoDetectRect& best, AutoDetectRect& finalRect);
        AutoDetectRect makeAbsRectFromLocal(const AutoDetectRect& localRect) const;
        void writeTracePlotBitmap(const std::string& path, const std::vector<TraceSampleRecord>& traceRecords, const std::vector<TraceBinRepresentativeRecord>& traceBinRepresentatives) const;
        void setRectDetectFail(const LogoRectDetectFail fail) {
            if (fail != LogoRectDetectFail::None && rectDetectFail == LogoRectDetectFail::None) {
                rectDetectFail = fail;
            }
        }
        void setLogoAnalyzeFail(const LogoAnalyzeFail fail) {
            if (fail != LogoAnalyzeFail::None && logoAnalyzeFail == LogoAnalyzeFail::None) {
                logoAnalyzeFail = fail;
            }
        }
        void throwIfRectDetectFailed() const {
            if (rectDetectFail != LogoRectDetectFail::None) {
                THROWF(RuntimeException, "Logo rect detect failed: %s", ToString(rectDetectFail));
            }
        }
        bool isRectSizeAbnormal(const AutoDetectRect& rect) const {
            if (rect.w <= 0 || rect.h <= 0) {
                return true;
            }
            const int area = rect.w * rect.h;
            return rect.w < 8
                || rect.h < 8
                || area < 24
                || rect.w > (int)std::round(scanw * 0.88)
                || rect.h > (int)std::round(scanh * 0.62);
        }

    public:
        AutoDetectLogoReader(AMTContext& ctx, int serviceid, int divx, int divy, int searchFrames, int blockSize, int threshold, int marginX, int marginY, int threadN, bool detailedDebug, logo::LOGO_AUTODETECT_CB cb)
            : SimpleVideoReader(ctx)
            , serviceid(serviceid)
            , divx(std::max(1, divx))
            , divy(std::max(1, divy))
            , searchFrames(std::max(100, searchFrames))
            , blockSize(std::max(4, blockSize))
            , threshold(std::max(1, threshold))
            , marginX(std::max(0, marginX))
            , marginY(std::max(0, marginY))
            , threadN(std::max(1, threadN))
            , detailedDebug(detailedDebug)
            , cb(cb)
            , threadPool(std::max(1, threadN))
            , imgw(0), imgh(0), scanx(0), scany(0), scanw(0), scanh(0), radius(0), bitDepth(8), logUVx(1), logUVy(1), framesPerSec(30), readFrames(0), enableTwoPassFrameGate(ParseEnvBoolDefault("AMT_LOGO_AUTODETECT_TWOPASS", kEnableTwoPassFrameGate)), enablePruneBinaryByAnchor(ParseEnvBoolDefault("AMT_LOGO_AUTODETECT_PRUNE_BY_ANCHOR", kEnablePruneBinaryByAnchor)), tracePointEnv(ParseEnvPointList("AMT_LOGO_AUTODETECT_TRACE_POINTS")), tracePoints(), tracePointIndexByOffset(), debugStats(), debugTraceRecords(), debugScore(), debugBinary(), passIndex(0), iterBinaryHistory(), iterThresholdDebug(), promoteCompDebug(), deltaCompDebug(), rectMergeDebug(), debugAbsX(1380), debugAbsY(67), rectAbs{ 0, 0, 0, 0 }, rectLocal{ 0, 0, 0, 0 }, rectDetectFail(LogoRectDetectFail::None), logoAnalyzeFail(LogoAnalyzeFail::None), scoreValidPixelCount(0), scorePositivePixelCount(0), initialSeedCount(0), initialGrownCount(0), usedBinaryFallback(false) {
        }

        int getRectDetectFailCode() const {
            return (int)rectDetectFail;
        }

        int getLogoAnalyzeFailCode() const {
            return (int)logoAnalyzeFail;
        }

        AutoDetectRect run(const tstring& srcpath) {
            if (cb && cb(1, 0.0f, 0.0f, 0, searchFrames) == false) {
                THROW(RuntimeException, "Cancel requested");
            }
            // 1) 初期化
            resetRunState();
            StatsPassBuffers pass1Stats{};
            resetAccumulationState(&pass1Stats, nullptr);
            // 2) pass1: 全フレームで score/binary/rect を推定
            readAll(srcpath, serviceid,
                [&](AVStream *videoStream, AVFrame* frame) { processFirstFrame(videoStream, frame, &pass1Stats, nullptr); },
                [&](AVFrame* frame) { return processFrame(frame, &pass1Stats, nullptr); });
            if (readFrames <= 0 || scanw <= 0 || scanh <= 0) {
                THROW(RuntimeException, "No frame decoded");
            }
            // 1回目の回帰は「粗い回帰線」を得るための準備段階。
            // まずは純粋な binAccum+Gompertz だけで provisional line を作り、
            // その線から外れる別枝サンプルを 2回目の走査で弱める。
            convertBinAccumToStats(pass1Stats);
            prepareResidualReweightMaps(pass1Stats);
            rerunResidualWeightedPass(srcpath, pass1Stats, nullptr);
            if (readFrames <= 0) {
                THROW(RuntimeException, "No frame decoded");
            }
            estimateScoreAndRect(pass1Stats);
            captureCurrentDebugSnapshot(debugPass1);
            debugPass2 = debugPass1;
            const AutoDetectRect pass1RectAbs = rectAbs;
            const AutoDetectRect pass1RectLocal = rectLocal;

            // 3) pass2: pass1で得たロゴ近傍のみで「ロゴあり」フレームを再抽出
            Pass2Buffers pass2{};
            bool pass2Entered = false;
            if (enableTwoPassFrameGate && runPass2PrepareMask(srcpath, pass1RectLocal, pass2)) {
                pass2Entered = true;
                runPass2CollectAndEstimate(srcpath, pass1RectAbs, pass1RectLocal, pass2);
            }
            // 4) pass2 に全く進めなかった場合（FrameMaskEmpty 等）、
            //    pass1 結果に rescue blending を適用して score/binary/rect を再計算する。
            //    pass2 不成立では passIndex==3 に到達しないため rescue が一切使われず、
            //    静止構造と重なるロゴ部分が欠落する。rescue 付きで再推定して救済する。
            //    注: pass2 に進んだが結果が不適切なケース(TooFewAcceptedFrames,
            //    Pass2RectDiverged)は既に passIndex==3 で rescue 済みなので対象外。
            if (!pass2Entered) {
                fprintf(stderr, "[LogoScan] pass2 not available, re-estimating pass1 with rescue blending\n");
                estimateScoreAndRect(pass1Stats, true, pass1RectLocal);
                captureCurrentDebugSnapshot(debugPass2);
            }
            return rectAbs;
        }

        void resetRunState() {
            passIndex = 0;
            readFrames = 0;
            debugStats.clear();
            debugTraceRecords.clear();
            debugTraceBinRepresentatives.clear();
            provisionalLineValid.clear();
            provisionalLineA.clear();
            provisionalLineB.clear();
            provisionalLineConsistency.clear();
            provisionalLineStdDiff.clear();
            sampleResidualReweightActive = false;
            debugScore = ScoreStageBuffers{};
            debugBinary.clear();
            debugPass1 = DebugStageSnapshot{};
            debugPass2 = DebugStageSnapshot{};
            debugPass2Prepare.clear();
            rectDetectFail = LogoRectDetectFail::None;
            logoAnalyzeFail = LogoAnalyzeFail::None;
            scoreValidPixelCount = 0;
            scorePositivePixelCount = 0;
            initialSeedCount = 0;
            initialGrownCount = 0;
            usedBinaryFallback = false;
        }

        void captureCurrentDebugSnapshot(DebugStageSnapshot& snapshot) {
            snapshot.stats = debugStats;
            snapshot.traceRecords = debugTraceRecords;
            snapshot.traceBinRepresentatives = debugTraceBinRepresentatives;
            snapshot.score = debugScore;
            snapshot.binary = debugBinary;
            snapshot.rectAbs = rectAbs;
            snapshot.rectLocal = rectLocal;
            snapshot.valid = (scanw > 0 && scanh > 0);
        }

        void configureTracePoints() {
            tracePoints.clear();
            tracePointIndexByOffset.assign(std::max(0, scanw * scanh), -1);
            int pointId = 0;
            for (const auto& p : tracePointEnv) {
                const int x = p.first;
                const int y = p.second;
                if (x < 0 || y < 0 || x >= scanw || y >= scanh) {
                    continue;
                }
                const int off = x + y * scanw;
                if (off < 0 || off >= (int)tracePointIndexByOffset.size()) {
                    continue;
                }
                if (tracePointIndexByOffset[off] >= 0) {
                    continue;
                }
                TracePointConfig tp{};
                tp.id = pointId++;
                tp.x = x;
                tp.y = y;
                tracePointIndexByOffset[off] = (int)tracePoints.size();
                tracePoints.push_back(tp);
            }
        }

        AutoDetectRect expandPass1RectForSecondPass(const AutoDetectRect& inRect) const {
            AutoDetectRect out = inRect;
            if (inRect.w <= 0 || inRect.h <= 0) return AutoDetectRect{ 0, 0, 0, 0 };
            const int padX = std::max(8, (int)std::round(inRect.w * 0.20f));
            const int padY = std::max(8, (int)std::round(inRect.h * 0.20f));
            out.x = ClampInt(inRect.x - padX, 0, std::max(0, scanw - 2));
            out.y = ClampInt(inRect.y - padY, 0, std::max(0, scanh - 2));
            const int maxX = ClampInt(inRect.x + inRect.w - 1 + padX, 1, std::max(1, scanw - 1));
            const int maxY = ClampInt(inRect.y + inRect.h - 1 + padY, 1, std::max(1, scanh - 1));
            out.w = std::max(2, maxX - out.x + 1);
            out.h = std::max(2, maxY - out.y + 1);
            out.x = RoundDownBy(out.x, 2);
            out.y = RoundDownBy(out.y, 2);
            out.w = RoundUpBy(out.w, 2);
            out.h = RoundUpBy(out.h, 2);
            out.w = std::min(out.w, std::max(2, scanw - out.x));
            out.h = std::min(out.h, std::max(2, scanh - out.y));
            return out;
        }

        bool runPass2PrepareMask(const tstring& srcpath, const AutoDetectRect& pass1RectLocal, Pass2Buffers& pass2) {
            debugPass2Prepare.clear();
            // 1) pass1で得た候補矩形を少し広げ、pass2で使う評価ROI(絶対座標/相対座標)を決める。
            const AutoDetectRect pass2LogoRectLocal = expandPass1RectForSecondPass(pass1RectLocal);
            pass2.logoRectAbs = AutoDetectRect{
                pass2LogoRectLocal.x + scanx,
                pass2LogoRectLocal.y + scany,
                pass2LogoRectLocal.w,
                pass2LogoRectLocal.h
            };
            // ROIが小さすぎる場合は pass2 を実施しても安定しないので中止する。
            if (pass2LogoRectLocal.w < 8 || pass2LogoRectLocal.h < 8) {
                setLogoAnalyzeFail(LogoAnalyzeFail::Pass2RoiTooSmall);
                return false;
            }

            // 2) pass1モードで全フレームを走査し、ROI内だけで仮ロゴ(LogoScan)を再構築する。
            pass2.logoScan = std::make_unique<logo::LogoScan>(pass2.logoRectAbs.w, pass2.logoRectAbs.h, logUVx, logUVy, threshold);
            passIndex = 1;
            resetAccumulationState(nullptr, &pass2);
            readAll(srcpath, serviceid,
                [&](AVStream *videoStream, AVFrame* frame) { processFirstFrame(videoStream, frame, nullptr, &pass2); },
                [&](AVFrame* frame) { return processFrame(frame, nullptr, &pass2); });

            // 3) 仮ロゴをデインタレースして評価用LogoDataParamへ変換し、maskを作る。
            if (readFrames > 0 && pass2.logoScan) {
                auto logoData = pass2.logoScan->GetLogo(false);
                if (logoData != nullptr) {
                    logo::LogoHeader hdr(pass2.logoRectAbs.w, pass2.logoRectAbs.h, logUVx, logUVy, imgw, imgh, pass2.logoRectAbs.x, pass2.logoRectAbs.y, "autodetect-pass2");
                    logo::LogoDataParam tempLogo(std::move(*logoData), &hdr);
                    auto deintLogo = std::make_unique<logo::LogoDataParam>(logo::LogoData(hdr.w, hdr.h, hdr.logUVx, hdr.logUVy), &hdr);
                    logo::DeintLogo(*deintLogo, tempLogo, hdr.w, hdr.h);
                    deintLogo->CreateLogoMask(0.35f);
                    const int logoPixels = hdr.w * hdr.h;
                    debugPass2Prepare.logoW = hdr.w;
                    debugPass2Prepare.logoH = hdr.h;
                    if (logoPixels > 0) {
                        const float* pA = deintLogo->GetA(PLANAR_Y);
                        const float* pB = deintLogo->GetB(PLANAR_Y);
                        debugPass2Prepare.logoA.assign(pA, pA + logoPixels);
                        debugPass2Prepare.logoB.assign(pB, pB + logoPixels);
                        const uint8_t* pMask = deintLogo->GetMask();
                        if (pMask != nullptr) {
                            debugPass2Prepare.logoMask.assign(pMask, pMask + logoPixels);
                        }
                    }
                    pass2.deintLogo = std::move(deintLogo);
                }
            }
            if (!pass2.deintLogo) {
                setLogoAnalyzeFail(LogoAnalyzeFail::GetLogoNull);
                return false;
            }

            // 4) pass2モードで全フレームの相関列(corr0/corr1)を収集する。
            passIndex = 2;
            resetAccumulationState(nullptr, &pass2);
            pass2.corr0.clear();
            pass2.corr1.clear();
            readAll(srcpath, serviceid,
                [&](AVStream *videoStream, AVFrame* frame) { processFirstFrame(videoStream, frame, nullptr, &pass2); },
                [&](AVFrame* frame) { return processFrame(frame, nullptr, &pass2); });
            if (pass2.corr0.empty() || pass2.corr0.size() != pass2.corr1.size()) {
                setLogoAnalyzeFail(LogoAnalyzeFail::CorrSequenceInvalid);
                return false;
            }

            // 5) 相関列を統合し、前後窓の統計を使って各フレームを
            //    0=ロゴなし / 1=不確定 / 2=ロゴあり に一次判定する。
            const float kThresh = 0.2f;
            const float kThreshL = 0.5f;
            const int num = (int)pass2.corr0.size();
            const int fps = std::max(1, framesPerSec);
            const int halfAvg = std::max(1, (int)std::round(fps * 1.0f / 2.0f));
            const int avgFrames = halfAvg * 2 + 1;
            const int halfMedian = std::max(1, (int)std::round(fps * 0.5f / 2.0f));
            const int medianFrames = halfMedian * 2 + 1;
            const int halfWin = std::max(avgFrames, medianFrames) / 2;
            std::vector<float> rawBuf(num + halfWin * 2, 0.0f);
            auto raw = rawBuf.data() + halfWin;
            for (int i = 0; i < num; i++) {
                raw[i] = std::max(0.0f, pass2.corr0[i]) + std::min(0.0f, pass2.corr1[i]);
            }
            std::fill(rawBuf.begin(), rawBuf.begin() + halfWin, raw[0]);
            std::fill(raw + num, rawBuf.data() + rawBuf.size(), raw[num - 1]);

            // r: 離散判定結果(0/1/2)、s: 予備スコア(中央値)。
            struct FrameJudge { int r; float s; };
            std::vector<FrameJudge> judge(num);
            std::vector<float> medbuf(medianFrames, 0.0f);
            for (int i = 0; i < num; i++) {
                float beforeMax = *std::max_element(raw + i - halfAvg, raw + i);
                float afterMax = *std::max_element(raw + i + 1, raw + i + 1 + halfAvg);
                float minMax = std::min(beforeMax, afterMax);
                int minMaxResult = (std::abs(minMax) < kThreshL) ? 1 : (minMax < 0.0f ? 0 : 2);
                float avg = std::accumulate(raw + i - halfAvg, raw + i + halfAvg + 1, 0.0f) / avgFrames;
                int avgResult = (std::abs(avg) < kThresh) ? 1 : (avg < 0.0f ? 0 : 2);
                judge[i].r = (minMaxResult != avgResult) ? 1 : avgResult;
                std::copy(raw + i - halfMedian, raw + i + halfMedian + 1, medbuf.begin());
                std::sort(medbuf.begin(), medbuf.end());
                judge[i].s = medbuf[halfMedian];
            }

            // 6) 連続する不確定区間(1)は、前後が同一判定ならその値で埋める。
            for (auto it = judge.begin(); it != judge.end();) {
                auto first1 = std::find_if(it, judge.end(), [](const FrameJudge& v) { return v.r == 1; });
                it = std::find_if_not(first1, judge.end(), [](const FrameJudge& v) { return v.r == 1; });
                const int prev = (first1 == judge.begin()) ? 0 : (first1 - 1)->r;
                const int next = (it == judge.end()) ? 0 : it->r;
                if (prev == next) {
                    for (auto p = first1; p != it; ++p) p->r = prev;
                }
            }

            // 7) 最終的に「ロゴあり(2)」だけを pass3投入マスクとして保持する。
            pass2.frameMask.assign(num, 0);
            debugPass2Prepare.corr0 = pass2.corr0;
            debugPass2Prepare.corr1 = pass2.corr1;
            debugPass2Prepare.raw.assign(num, 0.0f);
            debugPass2Prepare.median.assign(num, 0.0f);
            debugPass2Prepare.judge.assign(num, 0);
            for (int i = 0; i < num; i++) {
                debugPass2Prepare.raw[i] = raw[i];
                debugPass2Prepare.median[i] = judge[i].s;
                debugPass2Prepare.judge[i] = judge[i].r;
                pass2.frameMask[i] = (judge[i].r == 2) ? 1 : 0;
            }
            debugPass2Prepare.frameMask = pass2.frameMask;
            const bool hasFrameMask = std::any_of(pass2.frameMask.begin(), pass2.frameMask.end(), [](uint8_t v) { return v != 0; });
            if (!hasFrameMask) {
                setLogoAnalyzeFail(LogoAnalyzeFail::FrameMaskEmpty);
            }
            return hasFrameMask;
        }

        bool shouldFallbackToPass1Rect(const AutoDetectRect& pass1RectLocal) const {
            if (pass1RectLocal.w <= 0 || pass1RectLocal.h <= 0 || rectLocal.w <= 0 || rectLocal.h <= 0) {
                return false;
            }
            const double pass1Area = (double)pass1RectLocal.w * pass1RectLocal.h;
            const double pass2Area = (double)rectLocal.w * rectLocal.h;
            const double areaRatio = pass2Area / std::max(1.0, pass1Area);
            const double wRatio = (double)rectLocal.w / std::max(1, pass1RectLocal.w);
            const double hRatio = (double)rectLocal.h / std::max(1, pass1RectLocal.h);
            const int l1 = pass1RectLocal.x;
            const int t1 = pass1RectLocal.y;
            const int r1 = pass1RectLocal.x + pass1RectLocal.w - 1;
            const int b1 = pass1RectLocal.y + pass1RectLocal.h - 1;
            const int l2 = rectLocal.x;
            const int t2 = rectLocal.y;
            const int r2 = rectLocal.x + rectLocal.w - 1;
            const int b2 = rectLocal.y + rectLocal.h - 1;
            const int iw = std::max(0, std::min(r1, r2) - std::max(l1, l2) + 1);
            const int ih = std::max(0, std::min(b1, b2) - std::max(t1, t2) + 1);
            const double inter = (double)iw * ih;
            const double uni = pass1Area + pass2Area - inter;
            const double iou = (uni > 1e-6) ? (inter / uni) : 0.0;
            const bool tooLarge = (areaRatio > 1.45) && (wRatio > 1.20 || hRatio > 1.20);
            const bool tooShift = (areaRatio > 1.20) && (iou < 0.35);
            return tooLarge || tooShift;
        }

        bool runPass2CollectAndEstimate(const tstring& srcpath, const AutoDetectRect& pass1RectAbs, const AutoDetectRect& pass1RectLocal, Pass2Buffers& pass2) {
            // 1) pass3モードで再走査し、pass2FrameMaskに基づいて
            //    「ロゴあり」と判定されたフレームだけを統計へ投入する。
            pass2.acceptedFrames = 0;
            pass2.skippedFrames = 0;
            passIndex = 3;
            StatsPassBuffers pass3Stats{};
            resetAccumulationState(&pass3Stats, &pass2);
            readAll(srcpath, serviceid,
                [&](AVStream *videoStream, AVFrame* frame) { processFirstFrame(videoStream, frame, &pass3Stats, &pass2); },
                [&](AVFrame* frame) { return processFrame(frame, &pass3Stats, &pass2); });

            // 2) 採用フレームが少なすぎる場合は推定が不安定なため、
            //    pass1で得た矩形へフォールバックする。
            const int minAcceptedFrames = std::max(8, readFrames / 50);
            if (pass2.acceptedFrames < minAcceptedFrames) {
                setLogoAnalyzeFail(LogoAnalyzeFail::TooFewAcceptedFrames);
                rectAbs = pass1RectAbs;
                rectLocal = pass1RectLocal;
                return false;
            }

            // 3) 採用フレームのみで score/binary/rect を再推定する。
            // pass3 でも同じく provisional line -> 重み付き再走査の2段にする。
            // frame gate 済みフレームだけで別枝抑制をやり直すことで、
            // q43 のような「下側だけ別の bg 系列が混ざる」点を押し上げる。
            convertBinAccumToStats(pass3Stats);
            prepareResidualReweightMaps(pass3Stats);
            rerunResidualWeightedPass(srcpath, pass3Stats, &pass2);
            estimateScoreAndRect(pass3Stats, false, pass1RectLocal);
            captureCurrentDebugSnapshot(debugPass2);

            // 4) pass2結果がpass1に対して過大/過シフトなら安全側でpass1矩形を採用する。
            if (shouldFallbackToPass1Rect(pass1RectLocal)) {
                setLogoAnalyzeFail(LogoAnalyzeFail::Pass2RectDiverged);
                rectAbs = pass1RectAbs;
                rectLocal = pass1RectLocal;
                return false;
            }
            return true;
        }

        static std::string addSuffixBeforeExtension(const std::string& path, const std::string& suffix) {
            if (path.empty() || suffix.empty()) {
                return path;
            }
            const size_t dot = path.find_last_of('.');
            if (dot == std::string::npos || dot == 0) {
                return path + suffix;
            }
            return path.substr(0, dot) + suffix + path.substr(dot);
        }

        static std::string replaceExtensionWithSuffix(const std::string& path, const std::string& suffixWithExt) {
            if (path.empty()) {
                return path;
            }
            const size_t dot = path.find_last_of('.');
            if (dot == std::string::npos || dot == 0) {
                return path + suffixWithExt;
            }
            return path.substr(0, dot) + suffixWithExt;
        }

        static DebugPathSet makeSuffixedPathSet(const DebugPathSet& base, const std::string& suffix) {
            DebugPathSet out{};
            out.score = addSuffixBeforeExtension(base.score, suffix);
            out.binary = addSuffixBeforeExtension(base.binary, suffix);
            out.ccl = addSuffixBeforeExtension(base.ccl, suffix);
            out.count = addSuffixBeforeExtension(base.count, suffix);
            out.a = addSuffixBeforeExtension(base.a, suffix);
            out.b = addSuffixBeforeExtension(base.b, suffix);
            out.alpha = addSuffixBeforeExtension(base.alpha, suffix);
            out.logoY = addSuffixBeforeExtension(base.logoY, suffix);
            out.meanDiff = addSuffixBeforeExtension(base.meanDiff, suffix);
            out.consistency = addSuffixBeforeExtension(base.consistency, suffix);
            out.fgVar = addSuffixBeforeExtension(base.fgVar, suffix);
            out.bgVar = addSuffixBeforeExtension(base.bgVar, suffix);
            out.transition = addSuffixBeforeExtension(base.transition, suffix);
            out.keepRate = addSuffixBeforeExtension(base.keepRate, suffix);
            out.accepted = addSuffixBeforeExtension(base.accepted, suffix);
            out.diffGain = addSuffixBeforeExtension(base.diffGain, suffix);
            out.diffGainRaw = addSuffixBeforeExtension(base.diffGainRaw, suffix);
            out.residualGain = addSuffixBeforeExtension(base.residualGain, suffix);
            out.logoGain = addSuffixBeforeExtension(base.logoGain, suffix);
            out.consistencyGain = addSuffixBeforeExtension(base.consistencyGain, suffix);
            out.alphaGain = addSuffixBeforeExtension(base.alphaGain, suffix);
            out.bgGain = addSuffixBeforeExtension(base.bgGain, suffix);
            out.extremeGain = addSuffixBeforeExtension(base.extremeGain, suffix);
            out.temporalGain = addSuffixBeforeExtension(base.temporalGain, suffix);
            out.opaquePenalty = addSuffixBeforeExtension(base.opaquePenalty, suffix);
            out.opaqueStaticPenalty = addSuffixBeforeExtension(base.opaqueStaticPenalty, suffix);
            out.tracePlot = addSuffixBeforeExtension(base.tracePlot, suffix);
            out.edgePresence = addSuffixBeforeExtension(base.edgePresence, suffix);
            out.edgeMean = addSuffixBeforeExtension(base.edgeMean, suffix);
            out.edgeVar = addSuffixBeforeExtension(base.edgeVar, suffix);
            out.rescueScore = addSuffixBeforeExtension(base.rescueScore, suffix);
            out.presenceGain = addSuffixBeforeExtension(base.presenceGain, suffix);
            out.magGain = addSuffixBeforeExtension(base.magGain, suffix);
            out.upperGate = addSuffixBeforeExtension(base.upperGate, suffix);
            out.consistGain = addSuffixBeforeExtension(base.consistGain, suffix);
            out.bgVarGain = addSuffixBeforeExtension(base.bgVarGain, suffix);
            out.isWall = addSuffixBeforeExtension(base.isWall, suffix);
            out.isInterior = addSuffixBeforeExtension(base.isInterior, suffix);
            out.upperGateFilled = addSuffixBeforeExtension(base.upperGateFilled, suffix);
            return out;
        }

        void writeDebugStage(const std::vector<AutoDetectStats>& statsForDebug, const std::vector<TraceSampleRecord>& traceRecords, const std::vector<TraceBinRepresentativeRecord>& traceBinRepresentatives, const ScoreStageBuffers& score, const std::vector<uint8_t>& binary, const AutoDetectRect& rectLocalForDebug, const DebugPathSet& path, const bool withDetailMaps, const bool withIterationArtifacts) {
            float maxScore = 0.0f;
            for (auto v : score.score) maxScore = std::max(maxScore, v);
            if (maxScore <= 0) maxScore = 1.0f;

            if (!path.score.empty()) {
                WriteGrayBitmap(path.score, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const float v = (off < (int)score.score.size()) ? score.score[off] : 0.0f;
                    return (uint8_t)ClampInt((int)std::round(v / maxScore * 255.0f), 0, 255);
                });
            }
            if (!path.binary.empty()) {
                WriteGrayBitmap(path.binary, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    return (off < (int)binary.size() && binary[off]) ? 255 : 0;
                });
            }
            if (!path.ccl.empty()) {
                WriteGrayBitmap(path.ccl, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const bool hasRect = rectLocalForDebug.w > 0 && rectLocalForDebug.h > 0;
                    const bool onBorder = hasRect
                        && (x == rectLocalForDebug.x || x == rectLocalForDebug.x + rectLocalForDebug.w - 1
                            || y == rectLocalForDebug.y || y == rectLocalForDebug.h + rectLocalForDebug.y - 1);
                    if (onBorder) return (uint8_t)255;
                    return (off < (int)binary.size() && binary[off]) ? (uint8_t)190 : (uint8_t)24;
                });
            }

            if (!withDetailMaps) {
                return;
            }

            int maxCount = 0;
            for (const auto& s : statsForDebug) {
                maxCount = std::max(maxCount, s.rawSampleCount);
            }
            if (maxCount <= 0) maxCount = 1;
            if (!path.count.empty()) {
                WriteGrayBitmap(path.count, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const int c = (off >= 0 && off < (int)statsForDebug.size()) ? statsForDebug[off].rawSampleCount : 0;
                    return (uint8_t)ClampInt((int)std::round((double)c * 255.0 / maxCount), 0, 255);
                });
            }

            float aMin, aMax, bMin, bMax;
            CalcRangeValid(score.mapA, score.validAB, aMin, aMax, 0.8f, 1.2f);
            CalcRangeValid(score.mapB, score.validAB, bMin, bMax, -0.2f, 0.2f);
            if (!path.a.empty()) {
                WriteGrayBitmap(path.a, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapA.size()) {
                        return (uint8_t)0;
                    }
                    const float t = (score.mapA[off] - aMin) / (aMax - aMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.b.empty()) {
                WriteGrayBitmap(path.b, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapB.size()) {
                        return (uint8_t)0;
                    }
                    const float t = (score.mapB[off] - bMin) / (bMax - bMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.alpha.empty()) {
                WriteGrayBitmap(path.alpha, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapAlpha.size()) {
                        return (uint8_t)0;
                    }
                    return (uint8_t)ClampInt((int)std::round(score.mapAlpha[off] * 255.0f), 0, 255);
                });
            }
            if (!path.logoY.empty()) {
                WriteGrayBitmap(path.logoY, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapLogoY.size()) {
                        return (uint8_t)0;
                    }
                    return (uint8_t)ClampInt((int)std::round(score.mapLogoY[off] * 255.0f), 0, 255);
                });
            }
            float mdMin, mdMax, csMin, csMax, fgvMin, fgvMax, bgvMin, bgvMax, trMin, trMax, krMin, krMax;
            float dgMin, dgMax, dgrMin, dgrMax, rgMin, rgMax, lgMin, lgMax, cgMin, cgMax, agMin, agMax, bgGainMin, bgGainMax, egMin, egMax, tgMin, tgMax, opMin, opMax, ospMin, ospMax;
            CalcRangeValid(score.mapMeanDiff, score.validAB, mdMin, mdMax, -0.05f, 0.05f);
            CalcRangeValid(score.mapConsistency, score.validAB, csMin, csMax, 0.0f, 2.0f);
            CalcRangeValid(score.mapFgVar, score.validAB, fgvMin, fgvMax, 0.0f, 0.02f);
            CalcRangeValid(score.mapBgVar, score.validAB, bgvMin, bgvMax, 0.0f, 0.02f);
            CalcRangeValid(score.mapTransitionRate, score.validAB, trMin, trMax, 0.0f, 0.20f);
            CalcRangeValid(score.mapKeepRate, score.validAB, krMin, krMax, 0.0f, 0.20f);
            CalcRangeValid(score.mapDiffGain, score.validAB, dgMin, dgMax, 0.0f, 1.0f);
            CalcRangeValid(score.mapDiffGainRaw, score.validAB, dgrMin, dgrMax, 0.0f, 1.0f);
            CalcRangeValid(score.mapResidualGain, score.validAB, rgMin, rgMax, 0.0f, 1.0f);
            CalcRangeValid(score.mapLogoGain, score.validAB, lgMin, lgMax, 0.0f, 1.0f);
            CalcRangeValid(score.mapConsistencyGain, score.validAB, cgMin, cgMax, 0.0f, 1.0f);
            CalcRangeValid(score.mapAlphaGain, score.validAB, agMin, agMax, 0.0f, 1.0f);
            CalcRangeValid(score.mapBgGain, score.validAB, bgGainMin, bgGainMax, 0.0f, 1.0f);
            CalcRangeValid(score.mapExtremeGain, score.validAB, egMin, egMax, 0.0f, 1.0f);
            CalcRangeValid(score.mapTemporalGain, score.validAB, tgMin, tgMax, 0.0f, 1.0f);
            CalcRangeValid(score.mapOpaquePenalty, score.validAB, opMin, opMax, 0.0f, 1.0f);
            CalcRangeValid(score.mapOpaqueStaticPenalty, score.validAB, ospMin, ospMax, 0.0f, 1.0f);
            if (!path.meanDiff.empty()) {
                WriteGrayBitmap(path.meanDiff, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapMeanDiff.size()) {
                        return (uint8_t)0;
                    }
                    const float t = (score.mapMeanDiff[off] - mdMin) / (mdMax - mdMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.consistency.empty()) {
                WriteGrayBitmap(path.consistency, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapConsistency.size()) {
                        return (uint8_t)0;
                    }
                    const float t = (score.mapConsistency[off] - csMin) / (csMax - csMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.fgVar.empty()) {
                WriteGrayBitmap(path.fgVar, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapFgVar.size()) {
                        return (uint8_t)0;
                    }
                    const float t = (score.mapFgVar[off] - fgvMin) / (fgvMax - fgvMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.bgVar.empty()) {
                WriteGrayBitmap(path.bgVar, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapBgVar.size()) {
                        return (uint8_t)0;
                    }
                    const float t = (score.mapBgVar[off] - bgvMin) / (bgvMax - bgvMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.transition.empty()) {
                WriteGrayBitmap(path.transition, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapTransitionRate.size()) {
                        return (uint8_t)0;
                    }
                    const float t = (score.mapTransitionRate[off] - trMin) / (trMax - trMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.keepRate.empty()) {
                WriteGrayBitmap(path.keepRate, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapKeepRate.size()) {
                        return (uint8_t)0;
                    }
                    const float t = (score.mapKeepRate[off] - krMin) / (krMax - krMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.accepted.empty()) {
                WriteGrayBitmap(path.accepted, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.mapAccepted.size()) return (uint8_t)0;
                    return (uint8_t)ClampInt((int)std::round(score.mapAccepted[off] * 255.0f), 0, 255);
                });
            }
            if (!path.diffGain.empty()) {
                WriteGrayBitmap(path.diffGain, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapDiffGain.size()) return (uint8_t)0;
                    const float t = (score.mapDiffGain[off] - dgMin) / (dgMax - dgMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.diffGainRaw.empty()) {
                WriteGrayBitmap(path.diffGainRaw, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapDiffGainRaw.size()) return (uint8_t)0;
                    const float t = (score.mapDiffGainRaw[off] - dgrMin) / (dgrMax - dgrMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.residualGain.empty()) {
                WriteGrayBitmap(path.residualGain, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapResidualGain.size()) return (uint8_t)0;
                    const float t = (score.mapResidualGain[off] - rgMin) / (rgMax - rgMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.logoGain.empty()) {
                WriteGrayBitmap(path.logoGain, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapLogoGain.size()) return (uint8_t)0;
                    const float t = (score.mapLogoGain[off] - lgMin) / (lgMax - lgMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.consistencyGain.empty()) {
                WriteGrayBitmap(path.consistencyGain, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapConsistencyGain.size()) return (uint8_t)0;
                    const float t = (score.mapConsistencyGain[off] - cgMin) / (cgMax - cgMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.alphaGain.empty()) {
                WriteGrayBitmap(path.alphaGain, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapAlphaGain.size()) return (uint8_t)0;
                    const float t = (score.mapAlphaGain[off] - agMin) / (agMax - agMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.bgGain.empty()) {
                WriteGrayBitmap(path.bgGain, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapBgGain.size()) return (uint8_t)0;
                    const float t = (score.mapBgGain[off] - bgGainMin) / (bgGainMax - bgGainMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.extremeGain.empty()) {
                WriteGrayBitmap(path.extremeGain, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapExtremeGain.size()) return (uint8_t)0;
                    const float t = (score.mapExtremeGain[off] - egMin) / (egMax - egMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.temporalGain.empty()) {
                WriteGrayBitmap(path.temporalGain, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapTemporalGain.size()) return (uint8_t)0;
                    const float t = (score.mapTemporalGain[off] - tgMin) / (tgMax - tgMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.opaquePenalty.empty()) {
                WriteGrayBitmap(path.opaquePenalty, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapOpaquePenalty.size()) return (uint8_t)0;
                    const float t = (score.mapOpaquePenalty[off] - opMin) / (opMax - opMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            if (!path.opaqueStaticPenalty.empty()) {
                WriteGrayBitmap(path.opaqueStaticPenalty, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    if (off >= (int)score.validAB.size() || !score.validAB[off] || off >= (int)score.mapOpaqueStaticPenalty.size()) return (uint8_t)0;
                    const float t = (score.mapOpaqueStaticPenalty[off] - ospMin) / (ospMax - ospMin);
                    return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
                });
            }
            // 空間edge時系列統計マップ: validAB に関わらず全画素に対して出力
            if (!path.edgePresence.empty()) {
                WriteGrayBitmap(path.edgePresence, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const float v = (off < (int)score.mapEdgePresence.size()) ? score.mapEdgePresence[off] : 0.0f;
                    // 0.0-1.0 を 0-255 に線形マップ
                    return (uint8_t)ClampInt((int)std::round(v * 255.0f), 0, 255);
                });
            }
            if (!path.edgeMean.empty()) {
                WriteGrayBitmap(path.edgeMean, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const float v = (off < (int)score.mapEdgeMean.size()) ? score.mapEdgeMean[off] : 0.0f;
                    // 0.0-0.8 を 0-255 に線形マップ
                    return (uint8_t)ClampInt((int)std::round(v / 0.8f * 255.0f), 0, 255);
                });
            }
            if (!path.edgeVar.empty()) {
                WriteGrayBitmap(path.edgeVar, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const float v = (off < (int)score.mapEdgeVar.size()) ? score.mapEdgeVar[off] : 0.0f;
                    // 0.0-0.04 を 0-255 に線形マップ
                    return (uint8_t)ClampInt((int)std::round(v / 0.04f * 255.0f), 0, 255);
                });
            }
            if (!path.rescueScore.empty()) {
                WriteGrayBitmap(path.rescueScore, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const float v = (off < (int)score.mapRescueScore.size()) ? score.mapRescueScore[off] : 0.0f;
                    // 0.0-1.0 を 0-255 に線形マップ
                    return (uint8_t)ClampInt((int)std::round(v * 255.0f), 0, 255);
                });
            }
            // rescue score 中間ゲイン画像 (すべて 0.0-1.0 を 0-255 に線形マップ)
            if (!path.presenceGain.empty()) {
                WriteGrayBitmap(path.presenceGain, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const float v = (off < (int)score.mapPresenceGain.size()) ? score.mapPresenceGain[off] : 0.0f;
                    return (uint8_t)ClampInt((int)std::round(v * 255.0f), 0, 255);
                });
            }
            if (!path.magGain.empty()) {
                WriteGrayBitmap(path.magGain, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const float v = (off < (int)score.mapMagGain.size()) ? score.mapMagGain[off] : 0.0f;
                    return (uint8_t)ClampInt((int)std::round(v * 255.0f), 0, 255);
                });
            }
            if (!path.upperGate.empty()) {
                WriteGrayBitmap(path.upperGate, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const float v = (off < (int)score.mapUpperGate.size()) ? score.mapUpperGate[off] : 0.0f;
                    return (uint8_t)ClampInt((int)std::round(v * 255.0f), 0, 255);
                });
            }
            if (!path.consistGain.empty()) {
                WriteGrayBitmap(path.consistGain, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const float v = (off < (int)score.mapConsistGain.size()) ? score.mapConsistGain[off] : 0.0f;
                    return (uint8_t)ClampInt((int)std::round(v * 255.0f), 0, 255);
                });
            }
            if (!path.bgVarGain.empty()) {
                WriteGrayBitmap(path.bgVarGain, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const float v = (off < (int)score.mapBgVarGain.size()) ? score.mapBgVarGain[off] : 0.0f;
                    return (uint8_t)ClampInt((int)std::round(v * 255.0f), 0, 255);
                });
            }
            if (!path.isWall.empty()) {
                WriteGrayBitmap(path.isWall, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const uint8_t v = (off < (int)score.mapIsWall.size()) ? score.mapIsWall[off] : 0;
                    return v ? (uint8_t)255 : (uint8_t)0;
                });
            }
            if (!path.isInterior.empty()) {
                WriteGrayBitmap(path.isInterior, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const uint8_t v = (off < (int)score.mapIsInterior.size()) ? score.mapIsInterior[off] : 0;
                    return v ? (uint8_t)255 : (uint8_t)0;
                });
            }
            if (!path.upperGateFilled.empty()) {
                WriteGrayBitmap(path.upperGateFilled, scanw, scanh, [&](int x, int y) {
                    const int off = x + y * scanw;
                    const float v = (off < (int)score.mapUpperGateFilled.size()) ? score.mapUpperGateFilled[off] : 0.0f;
                    return (uint8_t)ClampInt((int)std::round(v * 255.0f), 0, 255);
                });
            }

            if (!path.binary.empty() && !traceRecords.empty()) {
                const auto rejectReasonStr = [](const int code) {
                    switch (code) {
                    case 0: return "accepted";
                    case 1: return "bg_fail";
                    case 2: return "extreme";
                    case 3: return "pass3_mask";
                    case 4: return "edge_skip";
                    default: return "unknown";
                    }
                };
                const std::string traceCsvPath = replaceExtensionWithSuffix(path.binary, ".trace.csv");
                FILE* ftrace = fopen(traceCsvPath.c_str(), "w");
                if (ftrace != nullptr) {
                    fprintf(ftrace, "pass,frame,point_id,x,y,abs_x,abs_y,fg_raw,fg_filtered,bg,bg_ok,bg_side_count,bg_side_valid_mask,bg_side_range_max,bg_side_avg_spread,fg_bg_diff,hist_bin,extreme,accepted,reject_code,reject\n");
                    for (const auto& r : traceRecords) {
                        fprintf(ftrace, "%d,%d,%d,%d,%d,%d,%d,%.8f,%.8f,%.8f,%d,%d,%d,%.8f,%.8f,%.8f,%d,%d,%d,%d,%s\n",
                            r.pass, r.frame, r.pointId, r.x, r.y, r.absX, r.absY,
                            r.fgRaw, r.fgFiltered, r.bg, r.bgOk, r.bgSideCount, r.bgSideValidMask,
                            r.bgSideRangeMax, r.bgSideAvgSpread, r.fgBgDiff, r.histBin,
                            r.extreme, r.accepted, r.rejectCode, rejectReasonStr(r.rejectCode));
                    }
                    fclose(ftrace);
                }

                struct TraceSummaryRow {
                    int pointId = 0;
                    int x = 0;
                    int y = 0;
                    int absX = 0;
                    int absY = 0;
                    int frames = 0;
                    int accepted = 0;
                    int rejectedBgFail = 0;
                    int rejectedExtreme = 0;
                    int rejectedPassMask = 0;
                    int rejectedEdge = 0;
                    double sumDiff = 0.0;
                    std::vector<float> acceptedDiff;
                };
                std::vector<TraceSummaryRow> rows;
                rows.reserve(tracePoints.size());
                for (const auto& tp : tracePoints) {
                    TraceSummaryRow row{};
                    row.pointId = tp.id;
                    row.x = tp.x;
                    row.y = tp.y;
                    row.absX = scanx + tp.x;
                    row.absY = scany + tp.y;
                    rows.push_back(row);
                }
                for (const auto& r : traceRecords) {
                    auto it = std::find_if(rows.begin(), rows.end(), [&](const TraceSummaryRow& v) { return v.pointId == r.pointId; });
                    if (it == rows.end()) continue;
                    it->frames++;
                    if (r.accepted) {
                        it->accepted++;
                        it->sumDiff += r.fgBgDiff;
                        it->acceptedDiff.push_back(r.fgBgDiff);
                    } else if (r.rejectCode == 1) {
                        it->rejectedBgFail++;
                    } else if (r.rejectCode == 2) {
                        it->rejectedExtreme++;
                    } else if (r.rejectCode == 3) {
                        it->rejectedPassMask++;
                    } else if (r.rejectCode == 4) {
                        it->rejectedEdge++;
                    }
                }
                const std::string summaryCsvPath = replaceExtensionWithSuffix(path.binary, ".trace.summary.csv");
                FILE* fsum = fopen(summaryCsvPath.c_str(), "w");
                if (fsum != nullptr) {
                    fprintf(fsum, "point_id,x,y,abs_x,abs_y,frames,accepted,accept_rate,reject_bg_fail,reject_extreme,reject_pass3_mask,reject_edge,mean_fg_bg_diff,fg_bg_diff_p50,fg_bg_diff_p90,A,B,alpha,logoy,consistency,fgvar,bgvar,transition,keeprate,score,accepted_score\n");
                    for (auto& row : rows) {
                        float p50 = 0.0f;
                        float p90 = 0.0f;
                        if (!row.acceptedDiff.empty()) {
                            std::sort(row.acceptedDiff.begin(), row.acceptedDiff.end());
                            p50 = PercentileOfSorted(row.acceptedDiff, 0.50f);
                            p90 = PercentileOfSorted(row.acceptedDiff, 0.90f);
                        }
                        const int off = row.x + row.y * scanw;
                        float A = 0.0f;
                        float B = 0.0f;
                        float alpha = 0.0f;
                        float logoY = 0.0f;
                        float consistency = 0.0f;
                        float fgvar = 0.0f;
                        float bgvar = 0.0f;
                        float transition = 0.0f;
                        float keeprate = 0.0f;
                        float scorev = 0.0f;
                        float acceptedScore = 0.0f;
                        if (off >= 0 && off < (int)statsForDebug.size() && off < (int)score.validAB.size() && score.validAB[off]) {
                            A = (off < (int)score.mapA.size()) ? score.mapA[off] : 0.0f;
                            B = (off < (int)score.mapB.size()) ? score.mapB[off] : 0.0f;
                            alpha = (off < (int)score.mapAlpha.size()) ? score.mapAlpha[off] : 0.0f;
                            logoY = (off < (int)score.mapLogoY.size()) ? score.mapLogoY[off] : 0.0f;
                            consistency = (off < (int)score.mapConsistency.size()) ? score.mapConsistency[off] : 0.0f;
                            fgvar = (off < (int)score.mapFgVar.size()) ? score.mapFgVar[off] : 0.0f;
                            bgvar = (off < (int)score.mapBgVar.size()) ? score.mapBgVar[off] : 0.0f;
                            transition = (off < (int)score.mapTransitionRate.size()) ? score.mapTransitionRate[off] : 0.0f;
                            keeprate = (off < (int)score.mapKeepRate.size()) ? score.mapKeepRate[off] : 0.0f;
                            scorev = (off < (int)score.score.size()) ? score.score[off] : 0.0f;
                            acceptedScore = (off < (int)score.mapAccepted.size()) ? score.mapAccepted[off] : 0.0f;
                        }
                        const double meanDiff = row.accepted > 0 ? row.sumDiff / row.accepted : 0.0;
                        const double acceptRate = row.frames > 0 ? (double)row.accepted / row.frames : 0.0;
                        fprintf(fsum, "%d,%d,%d,%d,%d,%d,%d,%.8f,%d,%d,%d,%d,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f\n",
                            row.pointId, row.x, row.y, row.absX, row.absY, row.frames, row.accepted, acceptRate,
                            row.rejectedBgFail, row.rejectedExtreme, row.rejectedPassMask, row.rejectedEdge,
                            meanDiff, p50, p90, A, B, alpha, logoY, consistency, fgvar, bgvar, transition, keeprate, scorev, acceptedScore);
                    }
                    fclose(fsum);
                }

                if (!traceBinRepresentatives.empty()) {
                    const std::string binCsvPath = replaceExtensionWithSuffix(path.binary, ".trace.bin.csv");
                    FILE* fbin = fopen(binCsvPath.c_str(), "w");
                    if (fbin != nullptr) {
                        fprintf(fbin, "point_id,x,y,abs_x,abs_y,hist_bin,count,avg_fg,raw_bg,adjusted_bg,weight_scale,provisional_a,provisional_b,provisional_consistency\n");
                        for (const auto& r : traceBinRepresentatives) {
                            fprintf(fbin, "%d,%d,%d,%d,%d,%d,%d,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f\n",
                                r.pointId, r.x, r.y, r.absX, r.absY, r.histBin, r.count,
                                r.avgFg, r.rawBg, r.adjustedBg, r.weightScale,
                                r.provisionalA, r.provisionalB, r.provisionalConsistency);
                        }
                        fclose(fbin);
                    }
                }

                if (!path.tracePlot.empty()) {
                    writeTracePlotBitmap(path.tracePlot, traceRecords, traceBinRepresentatives);
                }
            }

            // ピクセルレベルのスコア/ゲインダンプ CSV
            // score > 0 または rescueScore > 0 の全ピクセルを出力し、スケール比較に使う。
            if (!path.score.empty() && withDetailMaps) {
                const std::string pixelCsvPath = replaceExtensionWithSuffix(path.score, ".pixeldump.csv");
                FILE* fpx = fopen(pixelCsvPath.c_str(), "w");
                if (fpx != nullptr) {
                    fprintf(fpx, "x,y,validAB,score,rescueScore,diffGain,consistencyGain,alphaGain,logoGain,extremeGain,temporalGain,opaquePenalty,opaqueStaticPenalty,alpha,presenceGain,magGain,upperGate,consistGain,bgVarGain\n");
                    for (int y = 0; y < scanh; y++) {
                        for (int x = 0; x < scanw; x++) {
                            const int off = x + y * scanw;
                            const float sc = (off < (int)score.score.size()) ? score.score[off] : 0.0f;
                            const float rs = (off < (int)score.mapRescueScore.size()) ? score.mapRescueScore[off] : 0.0f;
                            if (sc <= 0.0f && rs <= 0.0f) continue;
                            const bool vab = (off < (int)score.validAB.size()) ? score.validAB[off] : false;
                            const float dg  = (off < (int)score.mapDiffGain.size()) ? score.mapDiffGain[off] : 0.0f;
                            const float cg  = (off < (int)score.mapConsistencyGain.size()) ? score.mapConsistencyGain[off] : 0.0f;
                            const float ag  = (off < (int)score.mapAlphaGain.size()) ? score.mapAlphaGain[off] : 0.0f;
                            const float lg  = (off < (int)score.mapLogoGain.size()) ? score.mapLogoGain[off] : 0.0f;
                            const float eg  = (off < (int)score.mapExtremeGain.size()) ? score.mapExtremeGain[off] : 0.0f;
                            const float tg  = (off < (int)score.mapTemporalGain.size()) ? score.mapTemporalGain[off] : 0.0f;
                            const float op  = (off < (int)score.mapOpaquePenalty.size()) ? score.mapOpaquePenalty[off] : 0.0f;
                            const float osp = (off < (int)score.mapOpaqueStaticPenalty.size()) ? score.mapOpaqueStaticPenalty[off] : 0.0f;
                            const float al  = (off < (int)score.mapAlpha.size()) ? score.mapAlpha[off] : 0.0f;
                            const float pg  = (off < (int)score.mapPresenceGain.size()) ? score.mapPresenceGain[off] : 0.0f;
                            const float mg  = (off < (int)score.mapMagGain.size()) ? score.mapMagGain[off] : 0.0f;
                            const float ug  = (off < (int)score.mapUpperGate.size()) ? score.mapUpperGate[off] : 0.0f;
                            const float csg = (off < (int)score.mapConsistGain.size()) ? score.mapConsistGain[off] : 0.0f;
                            // bgVarGain は保存されていないので rescueScore / (pg*mg*ug*csg) から逆算
                            float bgvg = 0.0f;
                            const float denom = pg * mg * ug * csg;
                            if (denom > 1e-8f && rs > 0.0f) bgvg = rs / denom;
                            fprintf(fpx, "%d,%d,%d,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f\n",
                                x, y, vab ? 1 : 0, sc, rs, dg, cg, ag, lg, eg, tg, op, osp, al, pg, mg, ug, csg, bgvg);
                        }
                    }
                    fclose(fpx);
                }
            }

            if (!withIterationArtifacts || path.binary.empty()) {
                return;
            }

            if (!iterBinaryHistory.empty()) {
                for (int i = 0; i < (int)iterBinaryHistory.size(); i++) {
                    const auto& mask = iterBinaryHistory[i];
                    char suffix[64];
                    sprintf_s(suffix, ".iter_%02d", i);
                    const std::string outPath = addSuffixBeforeExtension(path.binary, suffix);
                    WriteGrayBitmap(outPath, scanw, scanh, [&](int x, int y) {
                        const int off = x + y * scanw;
                        if (off >= (int)mask.size()) return (uint8_t)0;
                        return mask[off] ? (uint8_t)255 : (uint8_t)0;
                    });
                }

                const std::string iterCsvPath = replaceExtensionWithSuffix(path.binary, ".iter.csv");
                FILE* fiter = fopen(iterCsvPath.c_str(), "w");
                if (fiter != nullptr) {
                    fprintf(fiter, "iter,high,low,seed_on,low_on,grown_on,promoted_on,new_pixels,near_comp,far_comp,accepted_on,stop_reason\n");
                    for (int i = 0; i < (int)iterThresholdDebug.size(); i++) {
                        const auto& d = iterThresholdDebug[i];
                        const char* reason = "continue";
                        if (d.stopReason == 1) reason = "stop_far_only";
                        if (d.stopReason == 2) reason = "no_delta";
                        fprintf(fiter, "%d,%.8f,%.8f,%d,%d,%d,%d,%d,%d,%d,%d,%s\n",
                            i, d.highTh, d.lowTh, d.seedOnCount, d.lowOnCount, d.grownOnCount, d.promotedOnCount,
                            d.newPixelCount, d.nearCompCount, d.farCompCount, d.acceptedOnCount, reason);
                    }
                    fclose(fiter);
                }
            }

            if (!promoteCompDebug.empty()) {
                const std::string promoteCsvPath = replaceExtensionWithSuffix(path.binary, ".promote.csv");
                FILE* fprom = fopen(promoteCsvPath.c_str(), "w");
                if (fprom != nullptr) {
                    fprintf(fprom, "iter,high,low,minX,minY,maxX,maxY,area,w,h,overlapW,overlapH,gapX,gapY,peakScore,meanAccepted,nearH,nearV,nearD,nearAnchor,shapeOk,signalOk,accepted\n");
                    for (const auto& d : promoteCompDebug) {
                        fprintf(fprom, "%d,%.8f,%.8f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.8f,%.8f,%d,%d,%d,%d,%d,%d,%d\n",
                            d.iter, d.highTh, d.lowTh, d.minX, d.minY, d.maxX, d.maxY, d.area, d.compW, d.compH,
                            d.overlapW, d.overlapH, d.gapX, d.gapY, d.peakScore, d.meanAccepted,
                            d.nearHorizontal, d.nearVertical, d.nearDiagonal, d.nearAnchor, d.shapeOk, d.signalOk, d.accepted);
                    }
                    fclose(fprom);
                }
            }

            if (!deltaCompDebug.empty()) {
                const std::string deltaCsvPath = replaceExtensionWithSuffix(path.binary, ".delta.csv");
                FILE* fdelta = fopen(deltaCsvPath.c_str(), "w");
                if (fdelta != nullptr) {
                    fprintf(fdelta, "iter,high,low,minX,minY,maxX,maxY,w,h,overlapW,overlapH,gapX,gapY,nearH,nearV,nearD,nearInit,withinGuard,accepted\n");
                    for (const auto& d : deltaCompDebug) {
                        fprintf(fdelta, "%d,%.8f,%.8f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                            d.iter, d.highTh, d.lowTh, d.minX, d.minY, d.maxX, d.maxY, d.compW, d.compH,
                            d.overlapW, d.overlapH, d.gapX, d.gapY,
                            d.nearHorizontal, d.nearVertical, d.nearDiagonal, d.nearInitial, d.withinInitialGuard, d.accepted);
                    }
                    fclose(fdelta);
                }
            }

            if (!rectMergeDebug.empty()) {
                const std::string rectMergeCsvPath = replaceExtensionWithSuffix(path.binary, ".rectmerge.csv");
                FILE* frect = fopen(rectMergeCsvPath.c_str(), "w");
                if (frect != nullptr) {
                    fprintf(frect, "iter,minX,minY,maxX,maxY,area,overlapW,overlapH,gapX,gapY,nearH,nearV,nearD,overlap,seedGuard,finalGuard,sizeGuard,accepted\n");
                    for (const auto& d : rectMergeDebug) {
                        fprintf(frect, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                            d.iter, d.minX, d.minY, d.maxX, d.maxY, d.area, d.overlapW, d.overlapH, d.gapX, d.gapY,
                            d.nearHorizontal, d.nearVertical, d.nearDiagonal, d.overlap,
                            d.withinSeedCenterGuard, d.withinFinalCenterGuard, d.sizeGuardOk, d.accepted);
                    }
                    fclose(frect);
                }
            }
        }

        void writePass2PrepareDebug(const DebugPathSet& path) {
            if (debugPass2Prepare.logoW > 0 && debugPass2Prepare.logoH > 0
                && (int)debugPass2Prepare.logoA.size() == debugPass2Prepare.logoW * debugPass2Prepare.logoH
                && (int)debugPass2Prepare.logoB.size() == debugPass2Prepare.logoW * debugPass2Prepare.logoH) {
                const std::string logoPath = addSuffixBeforeExtension(path.score, ".pass2prep-logo");
                if (!logoPath.empty()) {
                    WriteGrayBitmap(logoPath, debugPass2Prepare.logoW, debugPass2Prepare.logoH, [&](int x, int y) {
                        const int off = x + y * debugPass2Prepare.logoW;
                        float alpha = 0.0f;
                        float logoY = 0.0f;
                        if (!TryGetAlphaLogo(debugPass2Prepare.logoA[off], debugPass2Prepare.logoB[off], alpha, logoY)) {
                            return (uint8_t)0;
                        }
                        return (uint8_t)ClampInt((int)std::round(std::max(0.0f, std::min(1.0f, logoY)) * 255.0f), 0, 255);
                    });
                }
            }

            if (debugPass2Prepare.logoW > 0 && debugPass2Prepare.logoH > 0
                && (int)debugPass2Prepare.logoMask.size() == debugPass2Prepare.logoW * debugPass2Prepare.logoH) {
                const std::string maskPath = addSuffixBeforeExtension(path.binary, ".pass2prep-mask");
                if (!maskPath.empty()) {
                    WriteGrayBitmap(maskPath, debugPass2Prepare.logoW, debugPass2Prepare.logoH, [&](int x, int y) {
                        const int off = x + y * debugPass2Prepare.logoW;
                        return debugPass2Prepare.logoMask[off] ? (uint8_t)255 : (uint8_t)0;
                    });
                }
            }

            if (!path.binary.empty() && !debugPass2Prepare.frameMask.empty()
                && debugPass2Prepare.corr0.size() == debugPass2Prepare.frameMask.size()
                && debugPass2Prepare.corr1.size() == debugPass2Prepare.frameMask.size()) {
                const std::string frameGateCsvPath = replaceExtensionWithSuffix(path.binary, ".framegate.csv");
                FILE* fgate = fopen(frameGateCsvPath.c_str(), "w");
                if (fgate != nullptr) {
                    fprintf(fgate, "frame,corr0,corr1,raw,median,judge,pass3_mask\n");
                    const int n = (int)debugPass2Prepare.frameMask.size();
                    for (int i = 0; i < n; i++) {
                        const float raw = (i < (int)debugPass2Prepare.raw.size()) ? debugPass2Prepare.raw[i] : 0.0f;
                        const float med = (i < (int)debugPass2Prepare.median.size()) ? debugPass2Prepare.median[i] : 0.0f;
                        const int judge = (i < (int)debugPass2Prepare.judge.size()) ? debugPass2Prepare.judge[i] : 0;
                        fprintf(fgate, "%d,%.8f,%.8f,%.8f,%.8f,%d,%d\n",
                            i, debugPass2Prepare.corr0[i], debugPass2Prepare.corr1[i], raw, med, judge, debugPass2Prepare.frameMask[i] ? 1 : 0);
                    }
                    fclose(fgate);
                }
            }
        }

        void writeDebug(const tchar* scorePath, const tchar* binaryPath, const tchar* cclPath, const tchar* countPath, const tchar* aPath, const tchar* bPath, const tchar* alphaPath, const tchar* logoYPath, const tchar* consistencyPath, const tchar* fgVarPath, const tchar* bgVarPath, const tchar* transitionPath, const tchar* keepRatePath, const tchar* acceptedPath) {
            if (scanw <= 0 || scanh <= 0) {
                return;
            }

            DebugPathSet basePath{};
            basePath.score = (scorePath != nullptr) ? tchar_to_string(scorePath) : std::string();
            basePath.binary = (binaryPath != nullptr) ? tchar_to_string(binaryPath) : std::string();
            basePath.ccl = (cclPath != nullptr) ? tchar_to_string(cclPath) : std::string();
            basePath.count = (countPath != nullptr) ? tchar_to_string(countPath) : std::string();
            basePath.a = (aPath != nullptr) ? tchar_to_string(aPath) : std::string();
            basePath.b = (bPath != nullptr) ? tchar_to_string(bPath) : std::string();
            basePath.alpha = (alphaPath != nullptr) ? tchar_to_string(alphaPath) : std::string();
            basePath.logoY = (logoYPath != nullptr) ? tchar_to_string(logoYPath) : std::string();
            basePath.meanDiff = addSuffixBeforeExtension(basePath.score, ".meandiff");
            basePath.consistency = (consistencyPath != nullptr) ? tchar_to_string(consistencyPath) : std::string();
            basePath.fgVar = (fgVarPath != nullptr) ? tchar_to_string(fgVarPath) : std::string();
            basePath.bgVar = (bgVarPath != nullptr) ? tchar_to_string(bgVarPath) : std::string();
            basePath.transition = (transitionPath != nullptr) ? tchar_to_string(transitionPath) : std::string();
            basePath.keepRate = (keepRatePath != nullptr) ? tchar_to_string(keepRatePath) : std::string();
            basePath.accepted = (acceptedPath != nullptr) ? tchar_to_string(acceptedPath) : std::string();
            basePath.diffGain = addSuffixBeforeExtension(basePath.score, ".diffgain");
            basePath.diffGainRaw = addSuffixBeforeExtension(basePath.score, ".diffgainraw");
            basePath.residualGain = addSuffixBeforeExtension(basePath.score, ".residualgain");
            basePath.logoGain = addSuffixBeforeExtension(basePath.score, ".logogain");
            basePath.consistencyGain = addSuffixBeforeExtension(basePath.score, ".consistencygain");
            basePath.alphaGain = addSuffixBeforeExtension(basePath.score, ".alphagain");
            basePath.bgGain = addSuffixBeforeExtension(basePath.score, ".bggain");
            basePath.extremeGain = addSuffixBeforeExtension(basePath.score, ".extremegain");
            basePath.temporalGain = addSuffixBeforeExtension(basePath.score, ".temporalgain");
            basePath.opaquePenalty = addSuffixBeforeExtension(basePath.score, ".opaquepenalty");
            basePath.opaqueStaticPenalty = addSuffixBeforeExtension(basePath.score, ".opaquestaticpenalty");
            basePath.tracePlot = addSuffixBeforeExtension(basePath.binary, ".traceplot");
            basePath.edgePresence = addSuffixBeforeExtension(basePath.score, ".edgepresence");
            basePath.edgeMean     = addSuffixBeforeExtension(basePath.score, ".edgemean");
            basePath.edgeVar      = addSuffixBeforeExtension(basePath.score, ".edgevar");
            basePath.rescueScore  = addSuffixBeforeExtension(basePath.score, ".rescuescore");
            basePath.presenceGain = addSuffixBeforeExtension(basePath.score, ".presencegain");
            basePath.magGain      = addSuffixBeforeExtension(basePath.score, ".maggain");
            basePath.upperGate    = addSuffixBeforeExtension(basePath.score, ".uppergate");
            basePath.consistGain  = addSuffixBeforeExtension(basePath.score, ".consistgain");
            basePath.bgVarGain    = addSuffixBeforeExtension(basePath.score, ".bgvargain");
            basePath.isWall           = addSuffixBeforeExtension(basePath.score, ".iswall");
            basePath.isInterior       = addSuffixBeforeExtension(basePath.score, ".isinterior");
            basePath.upperGateFilled  = addSuffixBeforeExtension(basePath.score, ".uppergatefilled");

            // 互換維持: 既存パスは最終結果(final)を出力。
            writeDebugStage(debugStats, debugTraceRecords, debugTraceBinRepresentatives, debugScore, debugBinary, rectLocal, basePath, detailedDebug, true);

            // 追加: pass1/pass2の同種デバッグ出力を suffix で並列管理。
            if (debugPass1.valid) {
                const DebugPathSet pass1Path = makeSuffixedPathSet(basePath, ".pass1");
                writeDebugStage(debugPass1.stats, debugPass1.traceRecords, debugPass1.traceBinRepresentatives, debugPass1.score, debugPass1.binary, debugPass1.rectLocal, pass1Path, detailedDebug, false);
            }
            if (debugPass2.valid) {
                const DebugPathSet pass2Path = makeSuffixedPathSet(basePath, ".pass2");
                writeDebugStage(debugPass2.stats, debugPass2.traceRecords, debugPass2.traceBinRepresentatives, debugPass2.score, debugPass2.binary, debugPass2.rectLocal, pass2Path, detailedDebug, false);
            }

            // 追加: runPass2PrepareMask で得た仮ロゴ/ロゴマスク/frame gate を保存。
            writePass2PrepareDebug(basePath);
        }

        int getReadFrames() const { return readFrames; }
        int getTotalFrames() const { return searchFrames; }
        const std::vector<AutoDetectStats>& getStatsForDebug() const {
            return debugStats;
        }

    protected:
        void processFirstFrame(AVStream *videoStream, AVFrame* frame, StatsPassBuffers* statsPass, Pass2Buffers* pass2) {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get((AVPixelFormat)(frame->format));
            bitDepth = desc->comp[0].depth;
            logUVx = std::max(0, (int)desc->log2_chroma_w);
            logUVy = std::max(0, (int)desc->log2_chroma_h);
            AVRational fps = { 0, 1 };
            if (videoStream != nullptr) {
                fps = videoStream->avg_frame_rate;
                if (fps.num <= 0 || fps.den <= 0) {
                    fps = videoStream->r_frame_rate;
                }
            }
            if (fps.num > 0 && fps.den > 0) {
                framesPerSec = ClampInt((int)std::round(av_q2d(fps)), 1, 240);
            } else {
                framesPerSec = 30;
            }
            imgw = frame->width;
            imgh = frame->height;
            scanw = RoundDownBy(std::max(32, imgw / divx), 4);
            scanh = RoundDownBy(std::max(32, imgh / divy), 4);
            scanx = std::max(0, imgw - scanw);
            scany = 0;
            radius = std::max(4, std::min(blockSize, std::min(scanw, scanh) / 4));
            configureTracePoints();
            if (statsPass != nullptr) {
                statsPass->reset(scanw, scanh, bitDepth);
            }
            if (pass2 != nullptr) {
                pass2->evalDeint.clear();
                pass2->evalWork.clear();
            }
        }

        void resetAccumulationState(StatsPassBuffers* statsPass, Pass2Buffers* pass2) {
            readFrames = 0;
            if (statsPass != nullptr && scanw > 0 && scanh > 0) {
                statsPass->reset(scanw, scanh, bitDepth);
            }
            if (pass2 != nullptr) {
                pass2->acceptedFrames = 0;
                pass2->skippedFrames = 0;
            }
        }

        // 1フレームを取り込み、現在passに応じて統計へ反映する。
        // pass1: 仮lgd構築 / pass2: 相関列構築 / pass3: フィルタ後サンプル収集。
        template<typename pixel_t>
        int addFrame(const AVFrame* frame, StatsPassBuffers* statsPass, Pass2Buffers* pass2) {
            const int pitchY = frame->linesize[0] / sizeof(pixel_t);
            const pixel_t* srcY = reinterpret_cast<const pixel_t*>(frame->data[0]);
            const pixel_t maxv = (pixel_t)((1 << bitDepth) - 1);
            const float invMaxv = 1.0f / std::max(1.0f, (float)maxv);
            const float rawScale = maxv / 255.0f;
            const float thresholdRaw = threshold * rawScale;
            std::vector<float> traceFgRaw;
            if (statsPass != nullptr && !tracePoints.empty()) {
                collectTraceRawFromSource<pixel_t>(srcY, pitchY, traceFgRaw);
            }

            // pass1: pass2用の仮ロゴを LogoScan に蓄積する。
            if (passIndex == 1) {
                if (pass2 == nullptr) return 0;
                return addFramePass1LogoScan<pixel_t>(frame, srcY, pitchY, *pass2);
            }

            // pass2: 仮ロゴとの相関列(corr0/corr1)を構築する。
            if (passIndex == 2) {
                if (pass2 == nullptr) return 0;
                return addFramePass2Correlation<pixel_t>(srcY, pitchY, maxv, *pass2);
            }

            // pass3: ロゴ無し判定フレームは統計への投入をスキップする。
            if (passIndex == 3) {
                if (pass2 != nullptr && !pass2->frameMask.empty()) {
                    if (shouldSkipPass3FrameByMask(*pass2)) {
                        pass2->skippedFrames++;
                        if (statsPass != nullptr && !tracePoints.empty()) {
                            for (size_t ti = 0; ti < tracePoints.size(); ti++) {
                                const auto& tp = tracePoints[ti];
                                TraceSampleRecord rec{};
                                rec.pass = 2;
                                rec.frame = readFrames;
                                rec.pointId = tp.id;
                                rec.x = tp.x;
                                rec.y = tp.y;
                                rec.absX = scanx + tp.x;
                                rec.absY = scany + tp.y;
                                rec.fgRaw = (ti < traceFgRaw.size()) ? traceFgRaw[ti] : 0.0f;
                                rec.rejectCode = 3;
                                statsPass->traceRecords.push_back(rec);
                            }
                        }
                        return 0;
                    }
                    pass2->acceptedFrames++;
                }
            }

            // 通常統計: 前処理→背景推定→サンプル採用/棄却を実施する。
            if (statsPass == nullptr) {
                return 0;
            }
            auto& frameWork = getFrameWorkBuffer<pixel_t>(*statsPass);
            preprocessFrame<pixel_t>(srcY, pitchY, frameWork, maxv, rawScale, thresholdRaw);
            return collectFrameSamples<pixel_t>(frameWork, invMaxv, rawScale, thresholdRaw, *statsPass, traceFgRaw);
        }

        template<typename pixel_t>
        int addFramePass1LogoScan(const AVFrame* frame, const pixel_t* srcY, const int pitchY, Pass2Buffers& pass2) {
            if (!pass2.logoScan || pass2.logoRectAbs.w <= 0 || pass2.logoRectAbs.h <= 0) {
                return 0;
            }
            if (frame->data[1] == nullptr || frame->data[2] == nullptr || frame->linesize[1] <= 0) {
                return 0;
            }
            const int pitchUV = frame->linesize[1] / sizeof(pixel_t);
            const pixel_t* srcU = reinterpret_cast<const pixel_t*>(frame->data[1]);
            const pixel_t* srcV = reinterpret_cast<const pixel_t*>(frame->data[2]);

            const int offY = pass2.logoRectAbs.x + pass2.logoRectAbs.y * pitchY;
            const int offUV = (pass2.logoRectAbs.x >> logUVx) + (pass2.logoRectAbs.y >> logUVy) * pitchUV;
            return pass2.logoScan->AddFrame(srcY + offY, srcU + offUV, srcV + offUV, pitchY, pitchUV, bitDepth) ? 1 : 0;
        }

        // pass2専用: 仮ロゴ(EvaluateLogo)の相関値を1フレーム分だけ記録する。
        template<typename pixel_t>
        int addFramePass2Correlation(const pixel_t* srcY, const int pitchY, const pixel_t maxv, Pass2Buffers& pass2) {
            // 仮ロゴが未構築なら何もしない。
            if (!pass2.deintLogo || !pass2.deintLogo->isValid()) {
                return 0;
            }
            // ワークバッファを必要サイズへ拡張し、Y面をdeintする。
            const int w = pass2.deintLogo->getWidth();
            const int h = pass2.deintLogo->getHeight();
            const int required = w * h + 8;
            if ((int)pass2.evalDeint.size() < required) {
                pass2.evalDeint.resize(required, 0.0f);
            }
            if ((int)pass2.evalWork.size() < required) {
                pass2.evalWork.resize(required, 0.0f);
            }
            const int off = pass2.deintLogo->getImgX() + pass2.deintLogo->getImgY() * pitchY;
            logo::DeintY(pass2.evalDeint.data(), srcY + off, pitchY, w, h);
            // alpha=0/1 の相関を評価し、後段のロゴ有無推定用に保存する。
            const float maxvf = (float)maxv;
            const float corr0 = pass2.deintLogo->EvaluateLogo(pass2.evalDeint.data(), maxvf, 0.0f, pass2.evalWork.data());
            const float corr1 = pass2.deintLogo->EvaluateLogo(pass2.evalDeint.data(), maxvf, 1.0f, pass2.evalWork.data());
            pass2.corr0.push_back(corr0);
            pass2.corr1.push_back(corr1);
            return 0;
        }

        bool shouldSkipPass3FrameByMask(const Pass2Buffers& pass2) const {
            const bool hasLogo = (readFrames >= 0 && readFrames < (int)pass2.frameMask.size()) ? (pass2.frameMask[readFrames] != 0) : false;
            return !hasLogo;
        }

        template<typename pixel_t>
        std::vector<pixel_t>& getFrameWorkBuffer(StatsPassBuffers& statsPass) {
            if constexpr (std::is_same_v<pixel_t, uint8_t>) {
                return statsPass.frameWork8;
            } else {
                return statsPass.frameWork16;
            }
        }

        template<typename pixel_t>
        void collectTraceRawFromSource(const pixel_t* srcY, const int pitchY, std::vector<float>& outFgRaw) const {
            outFgRaw.assign(tracePoints.size(), 0.0f);
            if (tracePoints.empty()) {
                return;
            }
            for (size_t i = 0; i < tracePoints.size(); i++) {
                const auto& tp = tracePoints[i];
                const int absX = scanx + tp.x;
                const int absY = scany + tp.y;
                outFgRaw[i] = (float)srcY[absX + absY * pitchY];
            }
        }

        template<typename pixel_t>
        void preprocessFrame(const pixel_t* srcY, const int pitchY, std::vector<pixel_t>& frameWork, const pixel_t maxv, const float rawScale, const float thresholdRaw) {
            // 前処理: 5x5 バイラテラルでノイズを落としつつ輪郭を保持する。
            constexpr int kBilateralRadius = 2;
            const float sigmaRange = std::max(6.0f * rawScale, thresholdRaw * 0.6f);
            const pixel_t* srcRoiBase = srcY + scanx + scany * pitchY;
            BilateralFilter<pixel_t, kBilateralRadius>(frameWork, srcRoiBase, pitchY, scanw, scanh, 1.4f, sigmaRange, (pixel_t)maxv, &threadPool, threadN);
        }

        template<typename pixel_t>
        int collectFrameSamples(const std::vector<pixel_t>& frameWork, const float invMaxv, const float rawScale, const float thresholdRaw, StatsPassBuffers& statsPass, const std::vector<float>& traceFgRaw) {
            auto& stats = statsPass.stats;
            auto& binAccumBuf = statsPass.binAccumBuf;
            auto& lastObservedFg = statsPass.lastObservedFg;
            auto& lastObservedValid = statsPass.lastObservedValid;
            const int pixelCount = scanw * scanh;
            assert((int)frameWork.size() == pixelCount);
            assert((int)stats.size() == pixelCount);
            assert((int)lastObservedFg.size() == pixelCount);
            assert((int)lastObservedValid.size() == pixelCount);
            assert((int)binAccumBuf.size() == pixelCount * kHistBins);

            std::atomic<int> frameCount(0);
            // 遷移検出閾値は rawScale から直接算出する。
            const float transitionThreshold = std::max(0.75f * rawScale, thresholdRaw * 0.125f);
            // 小さすぎるチャンク分割はタスク投入オーバーヘッドが支配的になるため、
            // 既定チャンク(スレッド数に応じた分割)を使う。
            RunParallelRange(threadPool, threadN, std::max(0, scanh - 2 * kScanEdgeMargin), [&](int y0, int y1) {
                for (int y = y0 + kScanEdgeMargin; y < y1 + kScanEdgeMargin; y++) {
                    for (int x = kScanEdgeMargin; x < scanw - kScanEdgeMargin; x++) {
                        const int off = x + y * scanw;
                        if (off >= 0 && off < (int)tracePointIndexByOffset.size() && tracePointIndexByOffset[off] >= 0) {
                            continue;
                        }
                        const float fgRaw = (float)frameWork[off];
                        AutoDetectStats& s = stats[off];
                        s.observed++;
                        if (lastObservedValid[off]) {
                            if (std::abs(lastObservedFg[off] - fgRaw) > transitionThreshold) {
                                s.fgTransition++;
                            }
                        }
                        lastObservedFg[off] = fgRaw;
                        lastObservedValid[off] = 1;

                        // 4方向の corrected edge を計算し最大値を edgeAccumBuf に蓄積する
                        {
                            const float fg_i = fgRaw * invMaxv;
                            float maxCorrected = 0.0f;
                            bool anyPositive = false;
                            // left
                            if (x > 0) {
                                const float fg_j = (float)frameWork[off - 1] * invMaxv;
                                const float raw = fg_i - fg_j;
                                if (raw > 0.0f) {
                                    const float corrected = raw / (1.0f - fg_j + 1e-4f);
                                    maxCorrected = std::max(maxCorrected, corrected);
                                    anyPositive = true;
                                }
                            }
                            // right
                            if (x < scanw - 1) {
                                const float fg_j = (float)frameWork[off + 1] * invMaxv;
                                const float raw = fg_i - fg_j;
                                if (raw > 0.0f) {
                                    const float corrected = raw / (1.0f - fg_j + 1e-4f);
                                    maxCorrected = std::max(maxCorrected, corrected);
                                    anyPositive = true;
                                }
                            }
                            // up
                            if (y > 0) {
                                const float fg_j = (float)frameWork[off - scanw] * invMaxv;
                                const float raw = fg_i - fg_j;
                                if (raw > 0.0f) {
                                    const float corrected = raw / (1.0f - fg_j + 1e-4f);
                                    maxCorrected = std::max(maxCorrected, corrected);
                                    anyPositive = true;
                                }
                            }
                            // down
                            if (y < scanh - 1) {
                                const float fg_j = (float)frameWork[off + scanw] * invMaxv;
                                const float raw = fg_i - fg_j;
                                if (raw > 0.0f) {
                                    const float corrected = raw / (1.0f - fg_j + 1e-4f);
                                    maxCorrected = std::max(maxCorrected, corrected);
                                    anyPositive = true;
                                }
                            }
                            if (anyPositive) {
                                auto& ea = statsPass.edgeAccumBuf[off];
                                ea.sumEdge  += maxCorrected;
                                ea.sumEdge2 += maxCorrected * maxCorrected;
                                ea.edgeCount++;
                            }
                        }

                        float bg = 0.0f;
                        // 背景推定不可(周辺辺が不一致など)な点は無効サンプルとして棄却。
                        // 例: ブロック辺にロゴ形状や高周波模様が入り、単色背景を仮定できない点。
                        if (!TryEstimateBg(frameWork, scanw, scanh, x, y, radius, thresholdRaw, bg)) {
                            continue;
                        }
                        s.totalCandidates++;
                        const double f = (double)frameWork[off] * invMaxv;
                        const double b = (double)bg * invMaxv;

                        // 極端コントラスト点を棄却。
                        // 具体例:
                        //   黒帯/白飛び/フラッシュに起因する「ロゴとは無関係な巨大差分」。
                        //   これを残すと score の裾が広がり、2値化が不安定になる。
                        if (IsExtremeContrastSample(f, b)) {
                            s.rejectedExtreme++;
                            continue;
                        }

                        s.rawSampleCount++;
                        const int binIdx = std::min(kHistBins - 1, (int)(fgRaw * invMaxv * kHistBins));
                        auto& bin = binAccumBuf[off * kHistBins + binIdx];
                        AddBinAccumSample(bin, f, b, calcSampleResidualWeight(off, f, b));
                        // このフレームで有効だった画素数（デバッグ可視化用）。
                        frameCount.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                });

            // 追跡点は理由付きログを残すため、並列本体から除外して逐次で同一ロジックを適用する。
            for (size_t ti = 0; ti < tracePoints.size(); ti++) {
                const auto& tp = tracePoints[ti];
                TraceSampleRecord rec{};
                rec.pass = (passIndex == 3) ? 2 : 1;
                rec.frame = readFrames;
                rec.pointId = tp.id;
                rec.x = tp.x;
                rec.y = tp.y;
                rec.absX = scanx + tp.x;
                rec.absY = scany + tp.y;
                rec.fgRaw = (ti < traceFgRaw.size()) ? traceFgRaw[ti] : 0.0f;

                if (tp.x < kScanEdgeMargin || tp.y < kScanEdgeMargin || tp.x >= scanw - kScanEdgeMargin || tp.y >= scanh - kScanEdgeMargin) {
                    rec.rejectCode = 4;
                    statsPass.traceRecords.push_back(rec);
                    continue;
                }

                const int off = tp.x + tp.y * scanw;
                const float fgFiltered = (float)frameWork[off];
                rec.fgFiltered = fgFiltered;

                AutoDetectStats& s = stats[off];
                s.observed++;
                if (lastObservedValid[off]) {
                    if (std::abs(lastObservedFg[off] - fgFiltered) > transitionThreshold) {
                        s.fgTransition++;
                    }
                }
                lastObservedFg[off] = fgFiltered;
                lastObservedValid[off] = 1;

                // 4方向の corrected edge を計算し最大値を edgeAccumBuf に蓄積する (tracePoint 逐次ループ)
                {
                    const int tx = tp.x;
                    const int ty = tp.y;
                    const float fg_i = fgFiltered * invMaxv;
                    float maxCorrected = 0.0f;
                    bool anyPositive = false;
                    if (tx > 0) {
                        const float fg_j = (float)frameWork[off - 1] * invMaxv;
                        const float raw = fg_i - fg_j;
                        if (raw > 0.0f) {
                            const float corrected = raw / (1.0f - fg_j + 1e-4f);
                            maxCorrected = std::max(maxCorrected, corrected);
                            anyPositive = true;
                        }
                    }
                    if (tx < scanw - 1) {
                        const float fg_j = (float)frameWork[off + 1] * invMaxv;
                        const float raw = fg_i - fg_j;
                        if (raw > 0.0f) {
                            const float corrected = raw / (1.0f - fg_j + 1e-4f);
                            maxCorrected = std::max(maxCorrected, corrected);
                            anyPositive = true;
                        }
                    }
                    if (ty > 0) {
                        const float fg_j = (float)frameWork[off - scanw] * invMaxv;
                        const float raw = fg_i - fg_j;
                        if (raw > 0.0f) {
                            const float corrected = raw / (1.0f - fg_j + 1e-4f);
                            maxCorrected = std::max(maxCorrected, corrected);
                            anyPositive = true;
                        }
                    }
                    if (ty < scanh - 1) {
                        const float fg_j = (float)frameWork[off + scanw] * invMaxv;
                        const float raw = fg_i - fg_j;
                        if (raw > 0.0f) {
                            const float corrected = raw / (1.0f - fg_j + 1e-4f);
                            maxCorrected = std::max(maxCorrected, corrected);
                            anyPositive = true;
                        }
                    }
                    if (anyPositive) {
                        auto& ea = statsPass.edgeAccumBuf[off];
                        ea.sumEdge  += maxCorrected;
                        ea.sumEdge2 += maxCorrected * maxCorrected;
                        ea.edgeCount++;
                    }
                }

                float bg = 0.0f;
                BgDebugInfo dbgBg{};
                const bool bgOk = TryEstimateBg(frameWork, scanw, scanh, tp.x, tp.y, radius, thresholdRaw, bg, &dbgBg);
                rec.bgOk = bgOk ? 1 : 0;
                rec.bg = bg;
                rec.bgSideCount = dbgBg.sideCount;
                int sideMask = 0;
                float sideRangeMax = 0.0f;
                float sideAvgMin = std::numeric_limits<float>::max();
                float sideAvgMax = std::numeric_limits<float>::lowest();
                bool hasSideAvg = false;
                for (int si = 0; si < 4; si++) {
                    if (dbgBg.sideValid[si]) {
                        sideMask |= (1 << si);
                        sideRangeMax = std::max(sideRangeMax, dbgBg.sideMax[si] - dbgBg.sideMin[si]);
                        sideAvgMin = std::min(sideAvgMin, dbgBg.sideAvg[si]);
                        sideAvgMax = std::max(sideAvgMax, dbgBg.sideAvg[si]);
                        hasSideAvg = true;
                    }
                }
                rec.bgSideValidMask = sideMask;
                rec.bgSideRangeMax = sideRangeMax;
                rec.bgSideAvgSpread = hasSideAvg ? (sideAvgMax - sideAvgMin) : 0.0f;
                if (!bgOk) {
                    rec.rejectCode = 1;
                    rec.observedAfter = s.observed;
                    rec.countAfter = s.rawSampleCount;
                    rec.totalCandidatesAfter = s.totalCandidates;
                    statsPass.traceRecords.push_back(rec);
                    continue;
                }

                s.totalCandidates++;
                const double f = (double)fgFiltered * invMaxv;
                const double b = (double)bg * invMaxv;
                rec.fgBgDiff = (float)std::abs(f - b);

                if (IsExtremeContrastSample(f, b)) {
                    s.rejectedExtreme++;
                    rec.extreme = 1;
                    rec.rejectCode = 2;
                    rec.observedAfter = s.observed;
                    rec.countAfter = s.rawSampleCount;
                    rec.totalCandidatesAfter = s.totalCandidates;
                    statsPass.traceRecords.push_back(rec);
                    continue;
                }

                s.rawSampleCount++;
                const int binIdx = std::min(kHistBins - 1, (int)(fgFiltered * invMaxv * kHistBins));
                auto& bin = binAccumBuf[off * kHistBins + binIdx];
                AddBinAccumSample(bin, f, b, calcSampleResidualWeight(off, f, b));
                rec.histBin = binIdx;
                frameCount.fetch_add(1, std::memory_order_relaxed);
                rec.accepted = 1;
                rec.rejectCode = 0;
                rec.observedAfter = s.observed;
                rec.countAfter = s.rawSampleCount;
                rec.totalCandidatesAfter = s.totalCandidates;
                statsPass.traceRecords.push_back(rec);
            }
            return frameCount.load(std::memory_order_relaxed);
        }

        bool processFrame(AVFrame* frame, StatsPassBuffers* statsPass, Pass2Buffers* pass2) {
            if (readFrames >= searchFrames) {
                return false;
            }

            const int pixelSize = av_pix_fmt_desc_get((AVPixelFormat)(frame->format))->comp[0].step;
            int frameCount = 0;
            if (pixelSize == 1) {
                frameCount = addFrame<uint8_t>(frame, statsPass, pass2);
            } else {
                frameCount = addFrame<uint16_t>(frame, statsPass, pass2);
            }
            if (statsPass != nullptr) {
                statsPass->frameValidCounts.push_back(frameCount);
            }

            readFrames++;
            if (cb && (readFrames % 8) == 0) {
                float stageProg = readFrames / (float)searchFrames;
                stageProg = std::min(1.0f, stageProg);
                float prog = stageProg * 0.5f;
                if (!cb(1, stageProg, prog, readFrames, searchFrames)) {
                    THROW(RuntimeException, "Cancel requested");
                }
            }
            return true;
        }

    private:
        // Gompertz関数によるビン重み計算
        // w(n) = exp(-exp(-c * (n - n0)))
        // n0: 変曲点（このサンプル数で weight ≈ 1/e ≈ 0.37）
        // c:  急峻さ（大きいほど閾値的な挙動になる）
        // 少数 bin の代表点は bg が不安定になりやすいので、sample 数だけではなく
        // 「少なすぎる bin を自然に弱める」目的で Gompertz を使う。
        static double GompertzWeight(int n, double n0, double c) {
            return std::exp(-std::exp(-c * ((double)n - n0)));
        }

        static void AddBinAccumSample(BinAccum& bin, const double fg, const double bg, const double sampleWeight = 1.0) {
            bin.count++;
            bin.sum_fg += fg;
            bin.sum_bg += bg;
            const double w = std::max(0.0, sampleWeight);
            bin.sum_weight += w;
            bin.sum_weighted_fg += w * fg;
            bin.sum_weighted_bg += w * bg;
        }

        static bool TryGetMeanDiffStdConsistency(const AutoDetectStats& s, double& meanDiff, double& stdDiff, double& consistency) {
            if (s.sumW <= 1e-6) {
                meanDiff = 0.0;
                stdDiff = 0.0;
                consistency = 0.0;
                return false;
            }
            const double invN = 1.0 / s.sumW;
            const double meanF = s.sumF * invN;
            const double meanBg = s.sumB * invN;
            meanDiff = meanF - meanBg;
            const double diff2 = (s.sumF2 - 2.0 * s.sumFB + s.sumB2) * invN;
            const double varDiff = std::max(0.0, diff2 - meanDiff * meanDiff);
            stdDiff = std::sqrt(varDiff);
            consistency = std::abs(meanDiff) / (stdDiff + 1e-6);
            return std::isfinite(consistency);
        }

        // trust は「この provisional line をどこまで信用してよいか」を表す係数。
        // q43 の低スコア群では旧閾値(0.55)だと補正がまったく発火しなかったため、
        // 0.20 からゆるやかに立ち上げ、1.40 で飽和するようにしている。
        static double CalcResidualReweightTrust(const double provisionalConsistency) {
            return std::max(0.0, std::min(1.0, (provisionalConsistency - 0.20) / 1.20));
        }

        // sigma は provisional line まわりの許容幅。
        // provisionalStdDiff は「回帰残差」ではなく点全体の fg-bg 差の散らばりで、
        // それを基準幅にしつつ、line を信用できる点ほど狭めて外れ枝を落とす。
        // ただし下限を持たせ、誤った provisional line で潰しすぎないようにする。
        static double CalcResidualReweightSigma(const double provisionalStdDiff, const double trust) {
            const double sigmaBase = std::max(6.0 / 255.0, provisionalStdDiff);
            const double sigmaShrink = 1.0 + 1.65 * trust;
            return std::max(4.0 / 255.0, sigmaBase / sigmaShrink);
        }

        // 2回目の走査では sample 単位で重みを掛けてから bin 平均を作る。
        // これにより、同じ fg bin に混ざる「時間連続の別 bg 枝」を hard reject せず
        // soft に弱めつつ、本流の representative point を保ちやすくする。
        static void GetBinRepresentative(const BinAccum& bin, double& avgFg, double& avgBg, double& weightScale) {
            avgFg = bin.sum_fg / std::max(1, bin.count);
            avgBg = bin.sum_bg / std::max(1, bin.count);
            weightScale = 1.0;
            if (bin.sum_weight > 1e-8) {
                avgFg = bin.sum_weighted_fg / bin.sum_weight;
                avgBg = bin.sum_weighted_bg / bin.sum_weight;
                weightScale = bin.sum_weight / std::max(1, bin.count);
            }
        }

        // binAccumBuf に蓄積した各binのサンプルをbin代表点として stats に集約する。
        // provisional 回帰線を作る段階では raw 平均、2回目走査の後は weighted 平均が
        // 入ってくるので、同じ変換器で 1回目/2回目の両方を扱えるようにしている。
        // 呼び出し後は stats の sumF/sumB/sumF2/sumB2/sumFB/sumW/count が有効になる。
        void convertBinAccumToStats(StatsPassBuffers& statsPass) {
            RunParallelRange(threadPool, threadN, scanh, [&](int y0, int y1) {
                for (int y = y0; y < y1; y++) {
                    for (int x = 0; x < scanw; x++) {
                        const int off = x + y * scanw;
                        AutoDetectStats& s = statsPass.stats[off];
                        AutoDetectStats provisional{};
                        provisional.rawSampleCount = s.rawSampleCount;
                        for (int b = 0; b < kHistBins; b++) {
                            const auto& bin = statsPass.binAccumBuf[off * kHistBins + b];
                            if (bin.count == 0) continue;
                            const double avg_fg = bin.sum_fg / bin.count;
                            const double avg_bg = bin.sum_bg / bin.count;
                            const double w = GompertzWeight(bin.count, /*n0=*/5.0, /*c=*/0.7);
                            if (w <= 1e-8) continue;
                            provisional.sumF += w * avg_fg;
                            provisional.sumB += w * avg_bg;
                            provisional.sumF2 += w * avg_fg * avg_fg;
                            provisional.sumB2 += w * avg_bg * avg_bg;
                            provisional.sumFB += w * avg_fg * avg_bg;
                            provisional.sumW += w;
                            provisional.effectiveBinCount++;
                        }

                        // sumF/sumB/sumF2/sumB2/sumFB/sumW/effectiveBinCount を binAccumBuf から算出し直す。
                        // observed/fgTransition/totalCandidates/rejectedExtreme はそのまま保持。
                        s.sumF = 0.0; s.sumB = 0.0; s.sumF2 = 0.0; s.sumB2 = 0.0; s.sumFB = 0.0;
                        s.sumW = 0.0; s.effectiveBinCount = 0;
                        for (int b = 0; b < kHistBins; b++) {
                            const auto& bin = statsPass.binAccumBuf[off * kHistBins + b];
                            if (bin.count == 0) continue;
                            double avg_fg = 0.0;
                            double avg_bg = 0.0;
                            double modeWeightScale = 1.0;
                            GetBinRepresentative(bin, avg_fg, avg_bg, modeWeightScale);
                            const double w = GompertzWeight(bin.count, /*n0=*/5.0, /*c=*/0.7);
                            if (w <= 1e-8) continue;
                            s.sumF  += w * avg_fg;
                            s.sumB  += w * avg_bg;
                            s.sumF2 += w * avg_fg * avg_fg;
                            s.sumB2 += w * avg_bg * avg_bg;
                            s.sumFB += w * avg_fg * avg_bg;
                            s.sumW  += w;  // Gompertz重みでビンを加重
                            s.effectiveBinCount++;
                        }
                    }
                }
            });
        }

        void prepareResidualReweightMaps(const StatsPassBuffers& statsPass) {
            const int total = scanw * scanh;
            provisionalLineValid.assign(total, 0);
            provisionalLineA.assign(total, 0.0f);
            provisionalLineB.assign(total, 0.0f);
            provisionalLineConsistency.assign(total, 0.0f);
            provisionalLineStdDiff.assign(total, 0.0f);
            for (int off = 0; off < total; off++) {
                const auto& s = statsPass.stats[off];
                float A = 0.0f;
                float B = 0.0f;
                double meanDiff = 0.0;
                double stdDiff = 0.0;
                double consistency = 0.0;
                if (!TryGetAB(s, A, B) || !TryGetMeanDiffStdConsistency(s, meanDiff, stdDiff, consistency)) {
                    continue;
                }
                provisionalLineValid[off] = 1;
                provisionalLineA[off] = A;
                provisionalLineB[off] = B;
                provisionalLineConsistency[off] = (float)consistency;
                provisionalLineStdDiff[off] = (float)stdDiff;
            }
        }

        double calcSampleResidualWeight(const int off, const double fg, const double bg) const {
            if (!sampleResidualReweightActive || off < 0 || off >= (int)provisionalLineValid.size() || !provisionalLineValid[off]) {
                return 1.0;
            }
            const double trust = CalcResidualReweightTrust(provisionalLineConsistency[off]);
            if (trust <= 1e-4) {
                // provisional line が弱い点は「どの枝を本流とみなすか」がまだ不明なので、
                // 無理に潰さず一旦そのまま通す。
                return 1.0;
            }
            const double sigma = CalcResidualReweightSigma(provisionalLineStdDiff[off], trust);
            const double predBg = provisionalLineA[off] * fg + provisionalLineB[off];
            const double residual = (bg - predBg) / sigma;
            // bg_desire=predBg を中心とするガウス重み。
            // 回帰線から離れた枝は指数的に弱めるが、hard reject はせず平均化へ残す。
            const double residualWeight = std::exp(-0.5 * residual * residual);
            return std::max(1e-4, residualWeight);
        }

        void rerunResidualWeightedPass(const tstring& srcpath, StatsPassBuffers& statsPass, Pass2Buffers* pass2) {
            // provisional line ができた後にだけ raw sample へ戻って重み付けし直す。
            // q43 のように「bin代表点だけ見るとまだ二股だが、sample 単位では本流が見える」
            // ケースを拾うため、ここは再走査してでも sample 単位で処理する。
            sampleResidualReweightActive = true;
            resetAccumulationState(&statsPass, pass2);
            readAll(srcpath, serviceid,
                [&](AVStream *videoStream, AVFrame* frame) { processFirstFrame(videoStream, frame, &statsPass, pass2); },
                [&](AVFrame* frame) { return processFrame(frame, &statsPass, pass2); });
            sampleResidualReweightActive = false;
        }

        void collectTraceBinRepresentatives(const StatsPassBuffers& statsPass, std::vector<TraceBinRepresentativeRecord>& out) const {
            out.clear();
            if (tracePoints.empty()) {
                return;
            }
            out.reserve(tracePoints.size() * kHistBins);
            for (const auto& tp : tracePoints) {
                const int off = tp.x + tp.y * scanw;
                if (off < 0 || off >= (int)statsPass.stats.size()) {
                    continue;
                }
                const bool hasProvisionalLine = (off >= 0 && off < (int)provisionalLineValid.size() && provisionalLineValid[off] != 0);
                const float provisionalA = hasProvisionalLine ? provisionalLineA[off] : 0.0f;
                const float provisionalB = hasProvisionalLine ? provisionalLineB[off] : 0.0f;
                const float provisionalConsistency = hasProvisionalLine ? provisionalLineConsistency[off] : 0.0f;

                for (int b = 0; b < kHistBins; b++) {
                    const auto& bin = statsPass.binAccumBuf[off * kHistBins + b];
                    if (bin.count == 0) continue;
                    double avgFg = 0.0;
                    double adjustedBg = 0.0;
                    double weightScale = 1.0;
                    GetBinRepresentative(bin, avgFg, adjustedBg, weightScale);

                    TraceBinRepresentativeRecord rec{};
                    rec.pointId = tp.id;
                    rec.x = tp.x;
                    rec.y = tp.y;
                    rec.absX = scanx + tp.x;
                    rec.absY = scany + tp.y;
                    rec.histBin = b;
                    rec.count = bin.count;
                    rec.avgFg = (float)avgFg;
                    rec.rawBg = (float)(bin.sum_bg / std::max(1, bin.count));
                    rec.adjustedBg = (float)adjustedBg;
                    rec.weightScale = (float)weightScale;
                    rec.provisionalA = provisionalA;
                    rec.provisionalB = provisionalB;
                    rec.provisionalConsistency = (float)provisionalConsistency;
                    out.push_back(rec);
                }
            }
        }

        // enableRescue: rescue blending を有効にするか
        // rescueAnchorRect: contaminated rescue の空間制約に使う矩形 (scan-local 座標)。
        //   enableRescue=true 時、この矩形からの距離以内のみに contaminated rescue を適用する。
        //   幅0 の場合は制約なし。
        void estimateScoreAndRect(StatsPassBuffers& statsPass, bool enableRescue = false, const AutoDetectRect& rescueAnchorRect = AutoDetectRect{0,0,0,0}) {
            if (cb && !cb(2, 0.0f, 0.5f, readFrames, searchFrames)) {
                THROW(RuntimeException, "Cancel requested");
            }
            convertBinAccumToStats(statsPass);
            float thHigh = 0.0f;
            float thLow = 0.0f;
            ScoreStageBuffers scoreStage{};
            BinaryStageBuffers binaryStage{};

            runScoreStage(statsPass, scoreStage, thHigh, thLow, enableRescue, rescueAnchorRect);

            runBinaryStage(scoreStage, binaryStage, thHigh, thLow, enableRescue);

            runRectStage(scoreStage, binaryStage);
            debugStats = statsPass.stats;
            debugTraceRecords = statsPass.traceRecords;
            collectTraceBinRepresentatives(statsPass, debugTraceBinRepresentatives);
            debugScore = std::move(scoreStage);
            debugBinary = std::move(binaryStage.binary);
            throwIfRectDetectFailed();
        }

        // ステージ1: 画素ごとの統計(stats)から評価マップを作り、scoreを算出し、
        // 2値化用の初期閾値(thHigh/thLow)を決める。
        // ここでは「どの画素がロゴ候補としてどれだけ妥当か」を定量化することが目的。
        void runScoreStage(const StatsPassBuffers& statsPass, ScoreStageBuffers& scoreStage, float& thHigh, float& thLow, bool enableRescue = false, const AutoDetectRect& rescueAnchorRect = AutoDetectRect{0,0,0,0}) {
            // 診断ログ: passIndex確認
            fprintf(stderr, "[LogoScan] runScoreStage: passIndex=%d enableRescue=%d anchorRect=(%d,%d,%d,%d)\n",
                passIndex, (int)enableRescue, rescueAnchorRect.x, rescueAnchorRect.y, rescueAnchorRect.w, rescueAnchorRect.h);
            // 各種出力マップをクリアする。
            scoreStage.reset(scanw, scanh);
            // 画素単位で A/B 推定と特徴量計算を行い、score を合成する。
            auto smoothstep = [](const double t) {
                const double tc = std::max(0.0, std::min(1.0, t));
                return tc * tc * (3.0 - 2.0 * tc);
            };
            RunParallelRange(threadPool, threadN, scanh, [&](int y0, int y1) {
                for (int y = y0; y < y1; y++) {
                    for (int x = 0; x < scanw; x++) {
                        const int i = x + y * scanw;
                        float A, B;
                        if (TryGetAB(statsPass.stats[i], A, B)) {
                            scoreStage.validAB[i] = 1;
                            scoreStage.mapA[i] = A;
                            scoreStage.mapB[i] = B;
                            float alpha = 0.0f;
                            float logoY = 0.0f;
                            if (!TryGetAlphaLogo(A, B, alpha, logoY)) {
                                continue;
                            }
                            scoreStage.mapAlpha[i] = std::max(0.0f, std::min(1.0f, alpha));
                            scoreStage.mapLogoY[i] = std::max(0.0f, std::min(1.0f, logoY));

                            auto sat01 = [](const double v) {
                                return std::max(0.0, std::min(1.0, v));
                            };
                            const auto& s = statsPass.stats[i];
                            const double invN = (s.sumW > 1e-6) ? 1.0 / s.sumW : 0.0;
                            const double meanF = s.sumF * invN;
                            const double meanBg = s.sumB * invN;
                            const double meanDiff = meanF - meanBg;
                            const double diff2 = (s.sumF2 - 2.0 * s.sumFB + s.sumB2) * invN;
                            const double varDiff = std::max(0.0, diff2 - meanDiff * meanDiff);
                            const double stdDiff = std::sqrt(varDiff);
                            const double consistency = std::abs(meanDiff) / (stdDiff + 1e-6);
                            const double varFg = std::max(0.0, s.sumF2 * invN - meanF * meanF);
                            const double varBg = std::max(0.0, s.sumB2 * invN - meanBg * meanBg);
                            const double fgBgVarRatio = varFg / (varBg + 1e-6);
                            const double transitionRate = (s.observed > 1) ? (double)s.fgTransition / (double)(s.observed - 1) : 0.0;
                            const double keepRate = (s.observed > 0) ? (double)s.rawSampleCount / (double)s.observed : 0.0;
                            const double yRatio = (scanh > 1) ? (double)y / (double)(scanh - 1) : 0.0;
                            scoreStage.mapMeanDiff[i] = (float)meanDiff;
                            scoreStage.mapConsistency[i] = (float)consistency;
                            scoreStage.mapFgVar[i] = (float)varFg;
                            scoreStage.mapBgVar[i] = (float)varBg;
                            scoreStage.mapTransitionRate[i] = (float)transitionRate;
                            scoreStage.mapKeepRate[i] = (float)keepRate;

                            const double extremeRejectRatio = (s.totalCandidates > 0) ? (double)s.rejectedExtreme / s.totalCandidates : 0.0;
                            // alphaGain: alpha が 0.15〜0.35 付近（半透明ロゴ帯域）を最大評価し、
                            // 完全不透明（alpha >= 0.75）と完全透明（alpha <= 0.03）を排除する。
                            // f(alpha) = S((a-0.03)/0.12) [0.03,0.15), 1 [0.15,0.35], 1-S((a-0.35)/0.40) (0.35,0.75), 0 otherwise
                            const double alphaGain = (alpha <= 0.03) ? 0.0
                                : (alpha < 0.15)  ? smoothstep((alpha - 0.03) / 0.12)
                                : (alpha <= 0.35)  ? 1.0
                                : (alpha < 0.75)  ? 1.0 - smoothstep((alpha - 0.35) / 0.40)
                                : 0.0;
                            const double logoGain = sat01((logoY - 0.45) / 0.30);
                            const double consistencyGain = sat01((consistency - 0.35) / 1.65);
                            const double bgGain = sat01((0.080 - varBg) / 0.080);
                            const double extremeGain = sat01(1.0 - extremeRejectRatio);

                            const double residualDiff = meanDiff;
                            const double residualBgGain = sat01((0.080 - varBg) / 0.080);
                            const double diffGainRaw = sat01((meanDiff - 0.003) / 0.120);
                            const double residualGain = sat01((residualDiff - 0.001) / 0.105);
                            const double diffGain = sat01(residualGain * (0.25 + 0.75 * diffGainRaw) * (0.35 + 0.65 * residualBgGain));
                            const double lowTransition = sat01((0.10 - transitionRate) / 0.10);
                            const double lowKeep = sat01((0.08 - keepRate) / 0.08);
                            const double staticStrength = std::sqrt(lowTransition * lowKeep);
                            // 不透明固定文字の事前ゲート(保守的):
                            // 下側帯域に限定し、かつ keep/transition がともに低い強条件のみ除外する。
                            // ロゴ欠落を避けるため、上側のロゴ帯にはゲートを適用しない。
                            if (varBg >= 0.0012 && alpha > 0.62) {
                                const double lowerBand = sat01((yRatio - 0.28) / 0.30);
                                const bool gateByStaticPair = (lowerBand > 0.0) && (staticStrength > 0.88);
                                const bool gateByLockedKeep = (lowerBand > 0.35) && (keepRate < 0.020);
                                if (gateByStaticPair || gateByLockedKeep) {
                                    continue;
                                }
                            }
                            // 時間変動由来の抑制:
                            // 透明ロゴでは fg が bg 変動に追従しやすく、opaque固定文字では fg 変動が小さくなりやすい。
                            double temporalGain = 1.0;
                            if (varBg >= 0.0012) {
                                const double ratioGain = sat01((fgBgVarRatio - 0.08) / 0.44);
                                temporalGain = 0.85 + 0.15 * ratioGain;
                            }
                            double opaquePenalty = 1.0;
                            if (alpha > 0.68) {
                                const double alphaOpaque = sat01((alpha - 0.68) / 0.22);
                                const double temporalOpaque = (varBg >= 0.0012) ? sat01((0.22 - fgBgVarRatio) / 0.22) : 0.0;
                                const double penaltyScale = 1.0 + alphaOpaque * (0.20 + 0.40 * temporalOpaque);
                                opaquePenalty = 1.0 / penaltyScale;
                            }
                            double opaqueStaticPenalty = 1.0;
                            if (alpha > 0.55 && varBg >= 0.0012) {
                                const double alphaOpaque = sat01((alpha - 0.55) / 0.35);
                                // transition 単独の低値による過抑制を避けるため、閾値と寄与を緩める。
                                const double lowTransition = sat01((0.06 - transitionRate) / 0.06);
                                const double lowKeep = sat01((0.07 - keepRate) / 0.07);
                                const double staticness = std::max(0.45 * lowTransition, lowKeep);
                                const double penaltyScale = 1.0 + alphaOpaque * (0.20 + 1.40 * staticness);
                                opaqueStaticPenalty = 1.0 / penaltyScale;
                            }
                            scoreStage.mapDiffGain[i] = (float)diffGain;
                            scoreStage.mapDiffGainRaw[i] = (float)diffGainRaw;
                            scoreStage.mapResidualGain[i] = (float)residualGain;
                            scoreStage.mapLogoGain[i] = (float)logoGain;
                            scoreStage.mapConsistencyGain[i] = (float)consistencyGain;
                            scoreStage.mapAlphaGain[i] = (float)alphaGain;
                            scoreStage.mapBgGain[i] = (float)bgGain;
                            scoreStage.mapExtremeGain[i] = (float)extremeGain;
                            scoreStage.mapTemporalGain[i] = (float)temporalGain;
                            scoreStage.mapOpaquePenalty[i] = (float)opaquePenalty;
                            scoreStage.mapOpaqueStaticPenalty[i] = (float)opaqueStaticPenalty;
                            const float d = (float)(diffGain * (0.25 + 0.75 * consistencyGain) * (0.15 + 0.85 * alphaGain) * (0.6 + 0.4 * logoGain) * (0.20 + 0.80 * extremeGain) * temporalGain * opaquePenalty * opaqueStaticPenalty);
                            if (d <= 0.0f) continue;
                            scoreStage.mapAccepted[i] = d;
                            scoreStage.score[i] = d;
                        }
                    }
                }
                });

            // alpha 3x3 max filter による alphaGain 補正:
            // 近傍の最大 alpha を使い、不透明要素の輪郭 1px を追加抑制する。
            // alphaGain = min(f(alpha), f(alpha3x3max)) とすることで、
            // 自身は低 alpha でも近傍に高 alpha がある画素（＝不透明要素の縁）を抑制する。
            {
                const int pixelCount = scanw * scanh;
                std::vector<float> alpha3x3max(pixelCount, 0.0f);
                for (int y = 0; y < scanh; y++) {
                    for (int x = 0; x < scanw; x++) {
                        float maxVal = 0.0f;
                        for (int dy = -1; dy <= 1; dy++) {
                            for (int dx = -1; dx <= 1; dx++) {
                                const int nx = x + dx, ny = y + dy;
                                if (nx >= 0 && nx < scanw && ny >= 0 && ny < scanh) {
                                    maxVal = std::max(maxVal, scoreStage.mapAlpha[nx + ny * scanw]);
                                }
                            }
                        }
                        alpha3x3max[x + y * scanw] = maxVal;
                    }
                }
                auto falpha = [&smoothstep](const double a) -> double {
                    if (a <= 0.03) return 0.0;
                    if (a < 0.15)  return smoothstep((a - 0.03) / 0.12);
                    if (a <= 0.35) return 1.0;
                    if (a < 0.75)  return 1.0 - smoothstep((a - 0.35) / 0.40);
                    return 0.0;
                };
                for (int i = 0; i < pixelCount; i++) {
                    if (!scoreStage.validAB[i]) continue;
                    const float alphaGainOrig = scoreStage.mapAlphaGain[i];
                    const float alphaGainNeighbor = (float)falpha(alpha3x3max[i]);
                    const float alphaGainNew = std::min(alphaGainOrig, alphaGainNeighbor);
                    if (alphaGainNew >= alphaGainOrig) continue;
                    scoreStage.mapAlphaGain[i] = alphaGainNew;
                    const float d = scoreStage.mapDiffGain[i]
                        * (0.25f + 0.75f * scoreStage.mapConsistencyGain[i])
                        * (0.15f + 0.85f * alphaGainNew)
                        * (0.6f + 0.4f * scoreStage.mapLogoGain[i])
                        * (0.20f + 0.80f * scoreStage.mapExtremeGain[i])
                        * scoreStage.mapTemporalGain[i]
                        * scoreStage.mapOpaquePenalty[i]
                        * scoreStage.mapOpaqueStaticPenalty[i];
                    scoreStage.mapAccepted[i] = (d > 0.0f) ? d : 0.0f;
                    scoreStage.score[i] = (d > 0.0f) ? d : 0.0f;
                }
            }

            // 空間edge時系列統計を全画素に対して計算する (validAB の有無に関わらず)。
            // ステップ1: edgeMean / edgeVar / edgePresence と各ゲインを計算して保存する。
            //            upperGate は 3x3 localmax 後に計算するため、ここでは保存しない。
            {
                auto sat01f = [](const float v) { return std::max(0.0f, std::min(1.0f, v)); };
                const int pixelCount = scanw * scanh;
                for (int i = 0; i < pixelCount; i++) {
                    const auto& ea = statsPass.edgeAccumBuf[i];
                    const auto& s = statsPass.stats[i];
                    if (ea.edgeCount <= 0 || s.observed <= 0) continue;

                    const float edgeMean     = ea.sumEdge / ea.edgeCount;
                    const float edgeVar      = std::max(0.0f, ea.sumEdge2 / ea.edgeCount - edgeMean * edgeMean);
                    const float edgePresence = (float)ea.edgeCount / (float)s.observed;

                    scoreStage.mapEdgePresence[i] = edgePresence;
                    scoreStage.mapEdgeMean[i]     = edgeMean;
                    scoreStage.mapEdgeVar[i]       = edgeVar;
                    scoreStage.mapPresenceGain[i]  = sat01f((edgePresence - 0.08f) / 0.42f);
                    scoreStage.mapMagGain[i]       = sat01f((edgeMean     - 0.05f) / 0.30f);
                    // consistGain: 低分散ほど高スコアの線形降下に、高分散側への急峻な上限ゲートを乗算。
                    // 上限ゲート: 1/(1+exp(c*(x-d)))  c=500, d=0.015
                    //   透明ロゴの背景変動 (edgeVar≈0.010-0.018) は通し、
                    //   動的/変動コンテンツ (edgeVar>0.020) を強く抑制する。
                    // 全データ sep_gmean: current=45.3 → 本式=62.6 (+38%)。
                    static constexpr float kConsistUpperGateC = 500.0f;
                    static constexpr float kConsistUpperGateD = 0.015f;
                    const float consistLinear   = sat01f((0.040f - edgeVar) / 0.035f);
                    const float consistUpperGate = 1.0f / (1.0f + std::exp(kConsistUpperGateC * (edgeVar - kConsistUpperGateD)));
                    scoreStage.mapConsistGain[i]   = consistLinear * consistUpperGate;
                }

                // ステップ2: 3x3 近傍最大 edgeMean を計算する。
                // 静止構造の内部は edgeMean が高く sigmoid でゲートされるが、その外周 1px は
                // edgeMean が低く素通りしてしまう。近傍最大を使うことで外周まで抑制できる。
                const int pixelCount2 = pixelCount;  // for clarity in loop below
                std::vector<float> edgeMeanLocalMax(pixelCount2, 0.0f);
                for (int y = 0; y < scanh; y++) {
                    for (int x = 0; x < scanw; x++) {
                        float maxVal = 0.0f;
                        for (int dy = -1; dy <= 1; dy++) {
                            for (int dx = -1; dx <= 1; dx++) {
                                const int nx = x + dx, ny = y + dy;
                                if (nx >= 0 && nx < scanw && ny >= 0 && ny < scanh) {
                                    maxVal = std::max(maxVal, scoreStage.mapEdgeMean[nx + ny * scanw]);
                                }
                            }
                        }
                        edgeMeanLocalMax[x + y * scanw] = maxVal;
                    }
                }

                // ステップ3: upperGate・bgVarGain・rescueScore を計算する。
                // upperGate: sigmoid(a*(x-b)) で不透明な静止構造を除外する降下特性ゲート。
                // 3x3 localmax edgeMean を [0,0.8] → [0,1] に正規化して入力する。
                // a=25, b=0.40: 全データ sep_gmean 最大パラメータ。
                // bgVarGain: 背景分散が十分ある(半透明)ロゴのみを rescue 対象とするゲート。
                // bgVar を [0, kBgVarScale] → [0,1] に正規化後に sigmoid(a=30, b=0.10) を適用する。
                // 不透明テロップ (bgVar≈0) は bgVarGain≈0.047 となり rescueScore が抑制される。
                // 半透明ロゴ (bgVar≈0.004以上) は bgVarGain≈0.95以上となりほぼ通過する。
                static constexpr float kUpperGateSigmoidA = 25.0f;
                static constexpr float kUpperGateSigmoidB = 0.40f;
                static constexpr float kBgVarGainSigmoidA = 30.0f;
                static constexpr float kBgVarGainSigmoidB = 0.10f;
                static constexpr float kBgVarScale        = 0.020f;  // bgVar 正規化上限 (0〜1 スケール変換)
                for (int i = 0; i < pixelCount; i++) {
                    if (scoreStage.mapEdgePresence[i] <= 0.0f) continue;  // edgeCount==0 画素はスキップ
                    const float edgeMeanNorm = std::min(edgeMeanLocalMax[i] / 0.80f, 1.0f);
                    const float upperGate    = 1.0f / (1.0f + std::exp(kUpperGateSigmoidA * (edgeMeanNorm - kUpperGateSigmoidB)));
                    const float bgVarNorm    = std::min(scoreStage.mapBgVar[i] / kBgVarScale, 1.0f);
                    const float bgVarGain    = 1.0f / (1.0f + std::exp(-kBgVarGainSigmoidA * (bgVarNorm - kBgVarGainSigmoidB)));
                    const float rescueScore  = scoreStage.mapPresenceGain[i] * scoreStage.mapMagGain[i]
                                             * upperGate * scoreStage.mapConsistGain[i] * bgVarGain;
                    scoreStage.mapUpperGate[i]   = upperGate;
                    scoreStage.mapBgVarGain[i]   = bgVarGain;
                    scoreStage.mapRescueScore[i] = rescueScore;
                }
            }

            // upperGateFilled 構築:
            // 不透明テロップは輪郭部が低 upperGate (静止エッジ検出)、内部は高 upperGate。
            // テロップ内部も静止構造なので rescueScore を抑制したい。
            // 手法: upperGate の低い画素（壁）で Flood Fill し、壁と内部の画素を特定。
            //       壁・内部の画素では upperGateFilled = 0 として rescueScore を再計算する。
            std::vector<uint8_t> isInterior;
            {
                const int pixelCount = scanw * scanh;
                static constexpr float kFillWallThresh = 0.30f;  // upperGate がこの値未満 → 壁
                static constexpr int   kFillWallDilate = 1;      // 壁の膨張回数（隙間を塞ぐ）

                // 1. 壁マスク作成: edgePresence > 0 かつ upperGate < 閾値 の画素を壁とする
                std::vector<uint8_t> isWall(pixelCount, 0);
                for (int i = 0; i < pixelCount; i++) {
                    if (scoreStage.mapEdgePresence[i] > 0.0f && scoreStage.mapUpperGate[i] < kFillWallThresh) {
                        isWall[i] = 1;
                    }
                }

                // 壁を膨張（小さな隙間を塞ぐ）
                if (kFillWallDilate > 0) {
                    std::vector<uint8_t> wallTmp(pixelCount);
                    for (int iter = 0; iter < kFillWallDilate; iter++) {
                        for (int y = 0; y < scanh; y++) {
                            for (int x = 0; x < scanw; x++) {
                                const int idx = x + y * scanw;
                                uint8_t v = isWall[idx];
                                if (!v && x > 0)          v |= isWall[(x-1) + y * scanw];
                                if (!v && x < scanw - 1)  v |= isWall[(x+1) + y * scanw];
                                if (!v && y > 0)          v |= isWall[x + (y-1) * scanw];
                                if (!v && y < scanh - 1)  v |= isWall[x + (y+1) * scanw];
                                wallTmp[idx] = v;
                            }
                        }
                        std::swap(isWall, wallTmp);
                    }
                }

                // 2. 画像境界から非壁画素を通って Flood Fill → 外側をマーク
                //    4-connectivity: テロップ輪郭の対角隙間を壁として尊重する
                std::vector<uint8_t> isExterior(pixelCount, 0);
                std::vector<int> stack;
                stack.reserve(pixelCount / 4);
                for (int x = 0; x < scanw; x++) {
                    for (const int y : {0, scanh - 1}) {
                        const int idx = x + y * scanw;
                        if (!isWall[idx] && !isExterior[idx]) {
                            isExterior[idx] = 1;
                            stack.push_back(idx);
                        }
                    }
                }
                for (int y = 1; y < scanh - 1; y++) {
                    for (const int x : {0, scanw - 1}) {
                        const int idx = x + y * scanw;
                        if (!isWall[idx] && !isExterior[idx]) {
                            isExterior[idx] = 1;
                            stack.push_back(idx);
                        }
                    }
                }
                while (!stack.empty()) {
                    const int idx = stack.back(); stack.pop_back();
                    const int x = idx % scanw, y = idx / scanw;
                    const int dx[] = {-1, 1, 0, 0};
                    const int dy[] = {0, 0, -1, 1};
                    for (int d = 0; d < 4; d++) {
                        const int nx = x + dx[d], ny = y + dy[d];
                        if (nx < 0 || nx >= scanw || ny < 0 || ny >= scanh) continue;
                        const int ni = nx + ny * scanw;
                        if (!isExterior[ni] && !isWall[ni]) {
                            isExterior[ni] = 1;
                            stack.push_back(ni);
                        }
                    }
                }

                // 3. 内部マスクを構築（壁でも外側でもない画素 = テロップ内部）
                isInterior.resize(pixelCount, 0);
                int filledCount = 0;
                for (int i = 0; i < pixelCount; i++) {
                    if (!isWall[i] && !isExterior[i]) {
                        isInterior[i] = 1;
                        filledCount++;
                    }
                }
                fprintf(stderr, "[LogoScan] upperGate interior mask: wallThresh=%.2f dilate=%d interior=%d\n",
                    kFillWallThresh, kFillWallDilate, filledCount);

                // 4. upperGateFilled: 壁・内部の画素は 0、それ以外は元の upperGate
                //    これを使って rescueScore を再計算し、テロップ内部の rescueScore を抑制する。
                scoreStage.mapUpperGateFilled.resize(pixelCount);
                int suppressedCount = 0;
                for (int i = 0; i < pixelCount; i++) {
                    if (isWall[i] || isInterior[i]) {
                        scoreStage.mapUpperGateFilled[i] = 0.0f;
                        suppressedCount++;
                    } else {
                        scoreStage.mapUpperGateFilled[i] = scoreStage.mapUpperGate[i];
                    }
                }
                fprintf(stderr, "[LogoScan] upperGateFilled: suppressed=%d (wall+interior)\n", suppressedCount);

                // 5. rescueScore を upperGateFilled で再計算
                int rescueZeroedCount = 0;
                for (int i = 0; i < pixelCount; i++) {
                    if (scoreStage.mapEdgePresence[i] <= 0.0f) continue;
                    const float rescueScoreNew = scoreStage.mapPresenceGain[i] * scoreStage.mapMagGain[i]
                                               * scoreStage.mapUpperGateFilled[i] * scoreStage.mapConsistGain[i]
                                               * scoreStage.mapBgVarGain[i];
                    if (scoreStage.mapRescueScore[i] > 0.0f && rescueScoreNew <= 0.0f) rescueZeroedCount++;
                    scoreStage.mapRescueScore[i] = rescueScoreNew;
                }
                fprintf(stderr, "[LogoScan] rescueScore recalc with upperGateFilled: zeroed=%d\n", rescueZeroedCount);

                // デバッグ出力用にバッファに保存
                scoreStage.mapIsWall = isWall;
                scoreStage.mapIsInterior = isInterior;
            }

            // rescueScore ベースのスコア補正 (noise suppression + logo boost):
            // rescueScore は edge ベースの指標で、半透明ロゴの輪郭を正確に捉える一方、
            // 不透明テロップ等のノイズには低い値を返す。この特性を利用して:
            // (A) score が正なのに rescueScore が低い画素 → ノイズ候補として抑制
            // (B) score が弱いが rescueScore が高い画素 → ロゴ候補として底上げ
            //
            // ノイズテロップでは rescueScore が低くなる理由:
            // - エッジ部: edgeMean 高 → upperGate 低 → rescueScore 低
            // - 内部: edgeMean 低 → magGain 低 → rescueScore 低
            // いずれの場合も rescueScore の積構造により少なくとも1因子が低くなる。
            {
                auto sat01f = [](const float v) { return std::max(0.0f, std::min(1.0f, v)); };
                const int pixelCount = scanw * scanh;

                // rescueScore の空間膨張 (max filter × kDilateIter 回):
                // rescueScore はエッジベースなので、ロゴ内部では ≈ 0 になる。
                // しかしロゴ内部はロゴ輪郭（高 rescueScore）の近傍にあるため、
                // 空間膨張で輪郭の値を内部に伝播させればロゴ内部を保護できる。
                // ノイズテロップは全域で rescueScore が低いため、膨張しても低いまま。
                static constexpr int kDilateIter = 4;  // 3x3 max filter × 4 回 = 実効半径 4px
                std::vector<float> rescueDilated(scoreStage.mapRescueScore.begin(),
                                                 scoreStage.mapRescueScore.begin() + pixelCount);
                std::vector<float> rescueTmp(pixelCount);
                for (int iter = 0; iter < kDilateIter; iter++) {
                    for (int y = 0; y < scanh; y++) {
                        for (int x = 0; x < scanw; x++) {
                            float maxVal = rescueDilated[x + y * scanw];
                            if (x > 0)          maxVal = std::max(maxVal, rescueDilated[(x-1) + y * scanw]);
                            if (x < scanw - 1)  maxVal = std::max(maxVal, rescueDilated[(x+1) + y * scanw]);
                            if (y > 0)          maxVal = std::max(maxVal, rescueDilated[x + (y-1) * scanw]);
                            if (y < scanh - 1)  maxVal = std::max(maxVal, rescueDilated[x + (y+1) * scanw]);
                            rescueTmp[x + y * scanw] = maxVal;
                        }
                    }
                    std::swap(rescueDilated, rescueTmp);
                }

                // (A) ノイズ抑制: 膨張済み rescueScore が低い画素のメインスコアを抑制する。
                static constexpr float kGateThresh = 0.005f;
                static constexpr float kGateRange  = 0.035f;
                static constexpr float kGateFloor  = 0.15f;
                // (B) ロゴ底上げ: 元の rescueScore が高い画素のメインスコアを底上げする。
                static constexpr float kBoostWeight  = 0.6f;
                static constexpr float kBoostCeiling = 0.10f;
                int gatedCount = 0, boostedCount = 0;
                for (int i = 0; i < pixelCount; i++) {
                    // (A) ノイズ抑制: 膨張済み rescueScore でゲート
                    if (scoreStage.score[i] > 0.0f) {
                        const float rescueGate = sat01f((rescueDilated[i] - kGateThresh) / kGateRange);
                        const float factor = kGateFloor + (1.0f - kGateFloor) * rescueGate;
                        if (factor < 0.9f) gatedCount++;
                        scoreStage.score[i] *= factor;
                    }
                    // (B) ロゴ底上げ: rescueScore > 0 でメインスコアが弱い画素を底上げ。
                    //     テロップ内部マスク (isInterior) に該当する画素はブースト対象外。
                    if (scoreStage.mapRescueScore[i] > 0.0f && scoreStage.score[i] < kBoostCeiling
                        && !isInterior[i]) {
                        const float boostVal = kBoostWeight * scoreStage.mapRescueScore[i];
                        if (boostVal > scoreStage.score[i]) boostedCount++;
                        scoreStage.score[i] = std::max(scoreStage.score[i], boostVal);
                    }
                }
                fprintf(stderr, "[LogoScan] rescueScore gate: dilateIter=%d thresh=%.3f range=%.3f floor=%.2f gated=%d boosted=%d\n",
                    kDilateIter, kGateThresh, kGateRange, kGateFloor, gatedCount, boostedCount);
                // ファイル出力: rescueScore 分布とゲート効果の確認
                {
                    FILE* fp = fopen("/tmp/rescue_gate_diag.txt", "w");
                    if (fp) {
                        float maxRS = 0, maxDilRS = 0, maxSc = 0, maxAcc = 0;
                        int nonzeroRS = 0, nonzeroDilRS = 0, nonzeroSc = 0;
                        for (int i = 0; i < pixelCount; i++) {
                            if (scoreStage.mapRescueScore[i] > 0) nonzeroRS++;
                            if (rescueDilated[i] > 0) nonzeroDilRS++;
                            if (scoreStage.score[i] > 0) nonzeroSc++;
                            maxRS = std::max(maxRS, scoreStage.mapRescueScore[i]);
                            maxDilRS = std::max(maxDilRS, rescueDilated[i]);
                            maxSc = std::max(maxSc, scoreStage.score[i]);
                            maxAcc = std::max(maxAcc, scoreStage.mapAccepted[i]);
                        }
                        fprintf(fp, "pixelCount=%d\n", pixelCount);
                        fprintf(fp, "nonzeroRS=%d nonzeroDilRS=%d nonzeroSc=%d\n", nonzeroRS, nonzeroDilRS, nonzeroSc);
                        fprintf(fp, "maxRS=%.6f maxDilRS=%.6f maxScore=%.6f maxAccepted=%.6f\n", maxRS, maxDilRS, maxSc, maxAcc);
                        fprintf(fp, "gated=%d boosted=%d\n", gatedCount, boostedCount);
                        // サンプル: ノイズ領域(y=40付近) と ロゴ領域(y=100付近) の値
                        for (int y : {30, 35, 40, 45, 70, 90, 100, 110}) {
                            if (y >= scanh) continue;
                            for (int x : {80, 100, 120, 140, 160, 180, 200}) {
                                if (x >= scanw) continue;
                                const int idx = x + y * scanw;
                                fprintf(fp, "  [%d,%d] accepted=%.5f score=%.5f rescue=%.6f dilRS=%.6f ugate=%.4f interior=%d\n",
                                    x, y, scoreStage.mapAccepted[idx], scoreStage.score[idx],
                                    scoreStage.mapRescueScore[idx], rescueDilated[idx],
                                    scoreStage.mapUpperGate[idx], (int)isInterior[idx]);
                            }
                        }
                        // isInterior / isWall / upperGate / rescueScore 詳細ラスターダンプ
                        fprintf(fp, "\n--- raster (y=45..95, x=10..200) ---\n");
                        fprintf(fp, "legend: wall=W int=# ext=. ugt=0-9 ep=0-9(edgePresence*10) rs=0-9(rescueScore*100)\n");
                        for (int y = 45; y <= 95 && y < scanh; y++) {
                            fprintf(fp, "y=%3d wall: ", y);
                            for (int x = 10; x <= 200 && x < scanw; x++) {
                                const int idx = x + y * scanw;
                                fprintf(fp, "%c", isInterior[idx] ? '#' : (scoreStage.mapIsWall[idx] ? 'W' : '.'));
                            }
                            fprintf(fp, "\n");
                            fprintf(fp, "y=%3d ep:   ", y);
                            for (int x = 10; x <= 200 && x < scanw; x++) {
                                const int idx = x + y * scanw;
                                const float ep = scoreStage.mapEdgePresence[idx];
                                const int level = (ep <= 0.0f) ? 0 : std::min(9, (int)(ep * 10.0f));
                                fprintf(fp, "%d", level);
                            }
                            fprintf(fp, "\n");
                            fprintf(fp, "y=%3d ugt:  ", y);
                            for (int x = 10; x <= 200 && x < scanw; x++) {
                                const int idx = x + y * scanw;
                                const float ug = scoreStage.mapUpperGate[idx];
                                const int level = (ug <= 0.0f) ? 0 : std::min(9, (int)(ug * 10.0f));
                                fprintf(fp, "%d", level);
                            }
                            fprintf(fp, "\n");
                            fprintf(fp, "y=%3d rs:   ", y);
                            for (int x = 10; x <= 200 && x < scanw; x++) {
                                const int idx = x + y * scanw;
                                const float rs = scoreStage.mapRescueScore[idx];
                                const int level = (rs <= 0.0f) ? 0 : std::min(9, (int)(rs * 100.0f));
                                fprintf(fp, "%d", level);
                            }
                            fprintf(fp, "\n");
                        }
                        fclose(fp);
                    }
                }
            }

            // Stage 2: rescue score を適用する。
            // 通常は pass2 (passIndex==3) のみで適用する。pass1 では scan region にテロップが
            // 含まれるためロゴと誤判定するリスクが高い。pass2 ではフレームゲートにより
            // テロップのあるフレームが除外されるため、rescue の誤爆リスクが低い。
            // ただし pass2 に進めなかった場合(FrameMaskEmpty 等)は、pass1 フォールバック時
            // にも enableRescue=true で rescue を適用する。pass2 が不成立の場合、他に手段が
            // ないため、テロップ誤判定リスクを許容して rescue で救済する。
            // (1) validAB=false: 回帰不成立の画素 → rescue score で代替
            // (2) validAB=true だが score が低い(汚染推定)→ rescue score でブレンド
            //     テロップ重畳等で bg 推定が汚染され diffGain ≈ 0 に潰された画素を
            //     edgePresence ベースの rescue で救済する。
            //
            // スケール合わせ: rescue score は gains の積のみで baselines がなく、
            // 通常 score より値が低い。validAB=true かつ両方正の画素から
            // score/rescue 比の p75 を求め、rescue にそのスケールを掛けてから適用する。
            if (passIndex == 3 || enableRescue) {
                fprintf(stderr, "[LogoScan] rescue blending: passIndex=%d enableRescue=%d, starting blend\n", passIndex, (int)enableRescue);
                const int pixelCount = scanw * scanh;

                // rectGate: アンカー矩形からの距離マップを構築し、遠い画素への rescue 適用を抑制する。
                // enableRescue (pass1 fallback) と pass2 (passIndex==3) の両方で使用する。
                static constexpr int kRescueRectMaxDist = 30;
                const bool useRectGate = rescueAnchorRect.w > 0 && rescueAnchorRect.h > 0;
                std::vector<int> rectDistMap;
                if (useRectGate) {
                    rectDistMap.assign(pixelCount, INT_MAX);
                    std::queue<int> bfsQ;
                    for (int y = rescueAnchorRect.y; y < rescueAnchorRect.y + rescueAnchorRect.h && y < scanh; y++) {
                        for (int x = rescueAnchorRect.x; x < rescueAnchorRect.x + rescueAnchorRect.w && x < scanw; x++) {
                            if (x >= 0 && y >= 0) {
                                const int idx = x + y * scanw;
                                rectDistMap[idx] = 0;
                                bfsQ.push(idx);
                            }
                        }
                    }
                    while (!bfsQ.empty()) {
                        const int cur = bfsQ.front(); bfsQ.pop();
                        if (rectDistMap[cur] >= kRescueRectMaxDist) continue;
                        const int cx = cur % scanw, cy = cur / scanw;
                        for (int dy = -1; dy <= 1; dy++) {
                            for (int dx = -1; dx <= 1; dx++) {
                                if (!dx && !dy) continue;
                                const int nx = cx + dx, ny = cy + dy;
                                if (nx < 0 || nx >= scanw || ny < 0 || ny >= scanh) continue;
                                const int nidx = nx + ny * scanw;
                                if (rectDistMap[nidx] > rectDistMap[cur] + 1) {
                                    rectDistMap[nidx] = rectDistMap[cur] + 1;
                                    bfsQ.push(nidx);
                                }
                            }
                        }
                    }
                    fprintf(stderr, "[LogoScan] rescue rect gate: rect=(%d,%d,%d,%d) maxDist=%d\n",
                        rescueAnchorRect.x, rescueAnchorRect.y, rescueAnchorRect.w, rescueAnchorRect.h, kRescueRectMaxDist);
                }

                if (enableRescue) {
                    // pass1 フォールバック: scaleFactor で rescue score を補正し、
                    // contaminated 閾値を高めに設定して pass1 rect 近傍のみ救済する。
                    std::vector<float> scaleRatios;
                    scaleRatios.reserve(4096);
                    static constexpr float kScaleRatioMinScore  = 0.02f;
                    static constexpr float kScaleRatioMinRescue = 0.005f;
                    for (int i = 0; i < pixelCount; i++) {
                        if (!scoreStage.validAB[i]) continue;
                        const float sc = scoreStage.score[i];
                        const float rs = scoreStage.mapRescueScore[i];
                        if (sc >= kScaleRatioMinScore && rs >= kScaleRatioMinRescue) {
                            scaleRatios.push_back(sc / rs);
                        }
                    }
                    float scaleFactor = 1.0f;
                    if (!scaleRatios.empty()) {
                        std::sort(scaleRatios.begin(), scaleRatios.end());
                        const int idx75 = std::min((int)(scaleRatios.size() * 3 / 4), (int)scaleRatios.size() - 1);
                        scaleFactor = std::max(1.0f, std::min(scaleRatios[idx75], 20.0f));
                    }
                    fprintf(stderr, "[LogoScan] rescue blending (pass1 fallback): scaleRatios.size()=%zu scaleFactor=%.4f\n",
                        scaleRatios.size(), (double)scaleFactor);

                    static constexpr float kRescueBlendWeight       = 0.7f;
                    static constexpr float kContaminatedScoreThresh = 0.05f;
                    for (int i = 0; i < pixelCount; i++) {
                        if (scoreStage.mapRescueScore[i] <= 0.0f) continue;
                        const float rescueScaled = std::min(scaleFactor * scoreStage.mapRescueScore[i], 1.0f);
                        const float rescueVal = kRescueBlendWeight * rescueScaled;
                        if (!scoreStage.validAB[i]) {
                            scoreStage.score[i] = rescueVal;
                        } else if (scoreStage.score[i] < kContaminatedScoreThresh) {
                            if (useRectGate && rectDistMap[i] > kRescueRectMaxDist) continue;
                            scoreStage.score[i] = std::max(scoreStage.score[i], rescueVal);
                        }
                    }
                } else {
                    // pass2 (passIndex==3): フレームゲート済みのためテロップノイズは少なく、
                    // スケール補正は不要。weight=0.6, threshold=0.01 で控えめに適用する。
                    // rectGate があれば、ロゴ矩形近傍のみに rescue を制限して
                    // 散在する偽陽性ノイズによる矩形検出失敗を防ぐ。
                    static constexpr float kRescueWeight = 0.6f;
                    static constexpr float kContaminatedThreshold = 0.01f;
                    for (int i = 0; i < pixelCount; i++) {
                        if (scoreStage.mapRescueScore[i] <= 0.0f) continue;
                        if (useRectGate && rectDistMap[i] > kRescueRectMaxDist) continue;
                        const float rescueVal = kRescueWeight * scoreStage.mapRescueScore[i];
                        if (!scoreStage.validAB[i]) {
                            scoreStage.score[i] = rescueVal;
                        } else if (scoreStage.score[i] < kContaminatedThreshold) {
                            scoreStage.score[i] = rescueVal;
                        }
                    }
                }
            }

            // score 分布の基礎統計を集計し、成立しない場合は詳細情報付きで例外化する。
            // 閾値は回帰ベーススコア (validAB=true) のみで計算する。
            // rescue でスケール補正済のスコアも validAB=true の分布に混入させてよいが、
            // rescue のみ由来の画素 (validAB=false) は除外する。
            double sum = 0.0;
            double sum2 = 0.0;
            int count = 0;
            int validPixels = 0;
            for (int i = 0; i < scanw * scanh; i++) {
                if (scoreStage.validAB[i]) {
                    validPixels++;
                }
                if (!scoreStage.validAB[i] || scoreStage.score[i] <= 0) continue;
                sum += scoreStage.score[i];
                sum2 += scoreStage.score[i] * scoreStage.score[i];
                count++;
            }
            scoreValidPixelCount = validPixels;
            scorePositivePixelCount = count;
            const int minRequiredPixels = std::max(24, scanw * scanh / 2048);
            if (validPixels < minRequiredPixels || count < minRequiredPixels) {
                setRectDetectFail(LogoRectDetectFail::InsufficientScorePixels);
                int finalPos = 0;
                for (int i = 0; i < scanw * scanh; i++) {
                    if (!scoreStage.validAB[i]) continue;
                    if (scoreStage.score[i] > 0) finalPos++;
                }
                const auto finalStats = CalcScoreDebugStats(scoreStage.score, scoreStage.validAB);
                int sampledPixels = 0;
                int maxSampleCount = 0;
                long long totalSampleCount = 0;
                for (const auto& s : statsPass.stats) {
                    if (s.rawSampleCount > 0) {
                        sampledPixels++;
                        totalSampleCount += s.rawSampleCount;
                        maxSampleCount = std::max(maxSampleCount, s.rawSampleCount);
                    }
                }
                int framesNonZero = 0;
                int frameMax = 0;
                long long frameSum = 0;
                for (const int v : statsPass.frameValidCounts) {
                    if (v > 0) framesNonZero++;
                    frameMax = std::max(frameMax, v);
                    frameSum += v;
                }
                const double frameAvg = statsPass.frameValidCounts.empty() ? 0.0 : (double)frameSum / statsPass.frameValidCounts.size();
                const double sampleAvg = sampledPixels > 0 ? (double)totalSampleCount / sampledPixels : 0.0;
                THROWF(RuntimeException,
                    "Logo rect detect failed: %s: frames=%d/%d, roi=%dx%d@(%d,%d), "
                    "validAB=%d, scorePos=%d, minRequired=%d, sampledPixels=%d, sampleCount(avg=%.2f,max=%d), "
                    "frameValid(nonZero=%d,avg=%.2f,max=%d), "
                    "scoreFinal(pos=%d,min=%.6f,p50=%.6f,p90=%.6f,p99=%.6f,max=%.6f,mean=%.6f)",
                    ToString(rectDetectFail), readFrames, searchFrames, scanw, scanh, scanx, scany,
                    validPixels, count, minRequiredPixels, sampledPixels, sampleAvg, maxSampleCount,
                    framesNonZero, frameAvg, frameMax,
                    finalPos, finalStats.minv, finalStats.p50, finalStats.p90, finalStats.p99, finalStats.maxv, finalStats.mean);
            }
            // まず平均/分散から暫定閾値を置く。
            // score の平均/分散ベース閾値は「最低限の保険」。
            // 実際の採用閾値は後段で score 分布から再計算する。
            // 背景: 番組によって score の絶対値レンジが大きく変わるため、
            // 固定係数(mean/stddev)だけでは過検出/未検出が起きやすい。
            const double mean = sum / count;
            const double var = std::max(0.0, sum2 / count - mean * mean);
            const double stddev = std::sqrt(var);
            thHigh = (float)(mean + stddev * 0.8);
            thLow = (float)std::max(mean + stddev * 0.2, thHigh * 0.50);
            {
                // 実分布の上位率から high/low を再推定して番組差に追従する。
                // score 実分布から、目標ピクセル率で閾値を逆算して決定。
                // 背景: 「統計値(mean/stddev)だけ」で閾値を作ると、
                // score が全体に低いケースで binary が全消しになりやすかった。
                // 例: ロゴ中心は出るが周辺の細字が落ちるケース(result1/result3)。
                std::vector<float> distVals;
                distVals.reserve(scanw * scanh);
                for (int i = 0; i < scanw * scanh; i++) {
                    // 閾値は回帰ベーススコアのみで決定する
                    if (!scoreStage.validAB[i]) continue;
                    const float v = scoreStage.score[i];
                    if (v <= 0.0f || !std::isfinite(v)) continue;
                    distVals.push_back(v);
                }
                if (distVals.size() >= 32) {
                    std::sort(distVals.begin(), distVals.end());
                    const int n = (int)distVals.size();
                    // 目標: seed=0.20%前後、lowMask=2.4%前後
                    // low側を厚めに取り、ロゴ周辺の弱スコア（細字・隙間）を接続しやすくする
                    const int seedTarget = ClampInt((int)std::round(n * 0.0020), 8, std::max(8, n / 4));
                    const int lowTarget = ClampInt((int)std::round(n * 0.0240), std::max(seedTarget + 12, seedTarget * 3), std::max(seedTarget + 12, n / 2));
                    const int highIdx = ClampInt(n - seedTarget, 0, n - 1);
                    const int lowIdx = ClampInt(n - lowTarget, 0, n - 1);

                    thHigh = distVals[highIdx];
                    thLow = distVals[lowIdx];
                    // 異常ケース保護: 並び順上は low<=high だが、丸めや外れ値で逆転したら補正。
                    if (thLow > thHigh) {
                        thLow = thHigh;
                    }
                    // ヒステリシスとしての差を確保（低閾値をやや緩めに）
                    // 背景: high/low が近すぎると seed から領域成長できず、
                    // ロゴの文字間・細線が切れて小さすぎる矩形になりやすい。
                    thLow = std::min(thLow, thHigh * 0.72f);
                }
            }

            // ステージ1の進捗を通知する。
            if (cb && !cb(2, 1.0f, 0.7f, readFrames, searchFrames)) {
                THROW(RuntimeException, "Cancel requested");
            }
        }

        void runBinaryStage(const ScoreStageBuffers& scoreStage, BinaryStageBuffers& binaryStage, const float thHigh, const float thLow, bool enableRescue = false);
        void runRectStage(const ScoreStageBuffers& scoreStage, const BinaryStageBuffers& binaryStage);
    };
}

namespace {
    bool AutoDetectLogoReader::getMaskRect(const std::vector<uint8_t>& mask, int& minX, int& minY, int& maxX, int& maxY) const {
        minX = scanw;
        minY = scanh;
        maxX = -1;
        maxY = -1;
        for (int y = 0; y < scanh; y++) {
            for (int x = 0; x < scanw; x++) {
                if (!mask[x + y * scanw]) continue;
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
        return (maxX >= minX && maxY >= minY);
    }

    int AutoDetectLogoReader::countMaskOn(const std::vector<uint8_t>& mask) const {
        int c = 0;
        for (const auto v : mask) {
            if (v) c++;
        }
        return c;
    }

    void AutoDetectLogoReader::writeTracePlotBitmap(const std::string& path, const std::vector<TraceSampleRecord>& traceRecords, const std::vector<TraceBinRepresentativeRecord>& traceBinRepresentatives) const {
        if (path.empty() || tracePoints.empty() || traceRecords.empty()) {
            return;
        }

        const int cols = std::min(2, std::max(1, (int)tracePoints.size()));
        const int rows = ((int)tracePoints.size() + cols - 1) / cols;
        const int panelW = 320;
        const int panelH = 320;
        const int width = panelW * cols;
        const int height = panelH * rows;
        std::vector<std::array<uint8_t, 3>> img(width * height, { { 248, 249, 252 } });

        const auto clamp255 = [](const int v) {
            return (uint8_t)ClampInt(v, 0, 255);
        };
        const auto setPixel = [&](const int x, const int y, const std::array<uint8_t, 3>& c) {
            if (x < 0 || x >= width || y < 0 || y >= height) {
                return;
            }
            img[x + y * width] = c;
        };
        const auto blendPixel = [&](const int x, const int y, const std::array<uint8_t, 3>& c, const float alpha) {
            if (x < 0 || x >= width || y < 0 || y >= height) {
                return;
            }
            auto& dst = img[x + y * width];
            const float a = std::max(0.0f, std::min(1.0f, alpha));
            for (int i = 0; i < 3; i++) {
                dst[i] = clamp255((int)std::round(dst[i] * (1.0f - a) + c[i] * a));
            }
        };
        const auto fillRect = [&](const int x0, const int y0, const int x1, const int y1, const std::array<uint8_t, 3>& c) {
            for (int y = std::max(0, y0); y <= std::min(height - 1, y1); y++) {
                for (int x = std::max(0, x0); x <= std::min(width - 1, x1); x++) {
                    setPixel(x, y, c);
                }
            }
        };
        const auto drawRect = [&](const int x0, const int y0, const int x1, const int y1, const std::array<uint8_t, 3>& c) {
            for (int x = x0; x <= x1; x++) {
                setPixel(x, y0, c);
                setPixel(x, y1, c);
            }
            for (int y = y0; y <= y1; y++) {
                setPixel(x0, y, c);
                setPixel(x1, y, c);
            }
        };
        const auto drawLine = [&](int x0, int y0, int x1, int y1, const std::array<uint8_t, 3>& c, const float alpha = 1.0f) {
            int dx = std::abs(x1 - x0);
            int sx = x0 < x1 ? 1 : -1;
            int dy = -std::abs(y1 - y0);
            int sy = y0 < y1 ? 1 : -1;
            int err = dx + dy;
            while (true) {
                blendPixel(x0, y0, c, alpha);
                if (x0 == x1 && y0 == y1) {
                    break;
                }
                const int e2 = err * 2;
                if (e2 >= dy) {
                    err += dy;
                    x0 += sx;
                }
                if (e2 <= dx) {
                    err += dx;
                    y0 += sy;
                }
            }
        };
        const auto glyphRows = [](const char ch) -> std::array<uint8_t, 5> {
            switch (ch) {
            case '0': return { { 0x7, 0x5, 0x5, 0x5, 0x7 } };
            case '1': return { { 0x2, 0x6, 0x2, 0x2, 0x7 } };
            case '2': return { { 0x7, 0x1, 0x7, 0x4, 0x7 } };
            case '3': return { { 0x7, 0x1, 0x7, 0x1, 0x7 } };
            case '4': return { { 0x5, 0x5, 0x7, 0x1, 0x1 } };
            case '5': return { { 0x7, 0x4, 0x7, 0x1, 0x7 } };
            case '6': return { { 0x7, 0x4, 0x7, 0x5, 0x7 } };
            case '7': return { { 0x7, 0x1, 0x1, 0x1, 0x1 } };
            case '8': return { { 0x7, 0x5, 0x7, 0x5, 0x7 } };
            case '9': return { { 0x7, 0x5, 0x7, 0x1, 0x7 } };
            case 'P': return { { 0x7, 0x5, 0x7, 0x4, 0x4 } };
            default: return { { 0, 0, 0, 0, 0 } };
            }
        };
        const auto drawText = [&](int x, int y, const std::string& text, const std::array<uint8_t, 3>& c) {
            for (const char ch : text) {
                const auto rowsBits = glyphRows(ch);
                for (int gy = 0; gy < 5; gy++) {
                    for (int gx = 0; gx < 3; gx++) {
                        if (rowsBits[gy] & (1 << (2 - gx))) {
                            fillRect(x + gx * 2, y + gy * 2, x + gx * 2 + 1, y + gy * 2 + 1, c);
                        }
                    }
                }
                x += 8;
            }
        };
        const auto hsvToRgb = [&](const float h, const float s, const float v) {
            const float hh = std::fmod(std::max(0.0f, h), 1.0f) * 6.0f;
            const int sector = (int)std::floor(hh);
            const float frac = hh - sector;
            const float p = v * (1.0f - s);
            const float q = v * (1.0f - s * frac);
            const float t = v * (1.0f - s * (1.0f - frac));
            float r = 0.0f, g = 0.0f, b = 0.0f;
            switch (sector % 6) {
            case 0: r = v; g = t; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = t; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = t; g = p; b = v; break;
            default: r = v; g = p; b = q; break;
            }
            return std::array<uint8_t, 3>{ clamp255((int)std::round(r * 255.0f)), clamp255((int)std::round(g * 255.0f)), clamp255((int)std::round(b * 255.0f)) };
        };

        std::array<std::array<uint8_t, 3>, kHistBins> binPalette = {};
        for (int i = 0; i < kHistBins; i++) {
            binPalette[i] = hsvToRgb((float)i / std::max(1, kHistBins), 0.78f, 0.90f);
        }
        int frameMin = std::numeric_limits<int>::max();
        int frameMax = std::numeric_limits<int>::lowest();
        for (const auto& rec : traceRecords) {
            if (!rec.bgOk) {
                continue;
            }
            frameMin = std::min(frameMin, rec.frame);
            frameMax = std::max(frameMax, rec.frame);
        }
        if (frameMin == std::numeric_limits<int>::max()) {
            frameMin = 0;
            frameMax = 0;
        }
        const int frameRange = std::max(1, frameMax - frameMin);
        const auto frameColorFor = [&](const int frame, const bool accepted) {
            static constexpr int kFrameColorBuckets = 24;
            const float norm = (float)(frame - frameMin) / (float)frameRange;
            const int bucket = ClampInt((int)std::floor(norm * (float)kFrameColorBuckets), 0, kFrameColorBuckets - 1);
            const float bucketNorm = ((float)bucket + 0.5f) / (float)kFrameColorBuckets;
            return hsvToRgb(0.67f * (1.0f - bucketNorm), accepted ? 0.82f : 0.38f, accepted ? 0.95f : 0.72f);
        };

        const float maxv = (float)((1 << bitDepth) - 1);
        const std::array<uint8_t, 3> panelBorder = { { 92, 102, 122 } };
        const std::array<uint8_t, 3> axisColor = { { 120, 132, 150 } };
        const std::array<uint8_t, 3> diagColor = { { 180, 188, 204 } };
        const std::array<uint8_t, 3> labelColor = { { 38, 43, 52 } };
        const auto drawDot = [&](const int px, const int py, const std::array<uint8_t, 3>& c, const float alpha) {
            blendPixel(px, py, c, alpha);
            blendPixel(px - 1, py, c, alpha * 0.52f);
            blendPixel(px + 1, py, c, alpha * 0.52f);
            blendPixel(px, py - 1, c, alpha * 0.52f);
            blendPixel(px, py + 1, c, alpha * 0.52f);
            blendPixel(px - 1, py - 1, c, alpha * 0.26f);
            blendPixel(px + 1, py - 1, c, alpha * 0.26f);
            blendPixel(px - 1, py + 1, c, alpha * 0.26f);
            blendPixel(px + 1, py + 1, c, alpha * 0.26f);
        };
        const auto drawCross = [&](const int px, const int py, const std::array<uint8_t, 3>& c) {
            const std::array<uint8_t, 3> outline = { { 32, 36, 44 } };
            drawLine(px - 5, py, px + 5, py, outline, 0.55f);
            drawLine(px, py - 5, px, py + 5, outline, 0.55f);
            drawLine(px - 4, py, px + 4, py, c, 0.96f);
            drawLine(px, py - 4, px, py + 4, c, 0.96f);
            blendPixel(px, py, { { 255, 255, 255 } }, 0.65f);
        };
        const auto drawSquareMarker = [&](const int px, const int py, const std::array<uint8_t, 3>& c) {
            const std::array<uint8_t, 3> outline = { { 24, 28, 36 } };
            fillRect(px - 2, py - 2, px + 2, py + 2, outline);
            fillRect(px - 1, py - 1, px + 1, py + 1, c);
            blendPixel(px, py, { { 255, 255, 255 } }, 0.45f);
        };

        for (size_t pointIdx = 0; pointIdx < tracePoints.size(); pointIdx++) {
            const int col = (int)pointIdx % cols;
            const int row = (int)pointIdx / cols;
            const int panelX = col * panelW;
            const int panelY = row * panelH;
            const int plotX0 = panelX + 34;
            const int plotY0 = panelY + 22;
            const int plotX1 = panelX + panelW - 18;
            const int plotY1 = panelY + panelH - 28;
            const int pointId = tracePoints[pointIdx].id;

            fillRect(panelX, panelY, panelX + panelW - 1, panelY + panelH - 1, { { 248, 249, 252 } });
            drawRect(panelX + 1, panelY + 1, panelX + panelW - 2, panelY + panelH - 2, panelBorder);
            drawRect(plotX0, plotY0, plotX1, plotY1, axisColor);
            drawLine(plotX0, plotY1, plotX1, plotY0, diagColor, 0.55f);
            drawText(panelX + 10, panelY + 8, "P" + std::to_string(pointId), labelColor);

            for (int gx = 1; gx < 4; gx++) {
                const int x = plotX0 + (plotX1 - plotX0) * gx / 4;
                const int y = plotY0 + (plotY1 - plotY0) * gx / 4;
                drawLine(x, plotY0 + 1, x, plotY1 - 1, { { 228, 232, 240 } }, 0.65f);
                drawLine(plotX0 + 1, y, plotX1 - 1, y, { { 228, 232, 240 } }, 0.65f);
            }

            struct BinMean {
                double sumFg = 0.0;
                double sumBg = 0.0;
                int count = 0;
            };
            std::array<BinMean, kHistBins> binMeans = {};

            for (const auto& rec : traceRecords) {
                if (rec.pointId != pointId || !rec.bgOk) {
                    continue;
                }
                const float xNorm = std::max(0.0f, std::min(1.0f, rec.fgFiltered / std::max(1.0f, maxv)));
                const float yNorm = std::max(0.0f, std::min(1.0f, rec.bg / std::max(1.0f, maxv)));
                const int px = plotX0 + ClampInt((int)std::round(xNorm * (plotX1 - plotX0)), 0, plotX1 - plotX0);
                const int py = plotY1 - ClampInt((int)std::round(yNorm * (plotY1 - plotY0)), 0, plotY1 - plotY0);
                const auto color = frameColorFor(rec.frame, rec.accepted != 0);
                drawDot(px, py, color, rec.accepted ? 0.92f : 0.38f);

                if (rec.accepted && rec.histBin >= 0 && rec.histBin < kHistBins) {
                    auto& mean = binMeans[rec.histBin];
                    mean.sumFg += rec.fgFiltered;
                    mean.sumBg += rec.bg;
                    mean.count++;
                }
            }

            const int frameLegendY0 = plotY1 + 8;
            const int frameLegendY1 = plotY1 + 13;
            const int frameLegendBuckets = 24;
            const int frameLegendW = std::max(1, (plotX1 - plotX0 + 1) / frameLegendBuckets);
            for (int b = 0; b < frameLegendBuckets; b++) {
                const int lx0 = plotX0 + b * frameLegendW;
                const int lx1 = (b == frameLegendBuckets - 1) ? plotX1 : std::min(plotX1, lx0 + frameLegendW - 1);
                const int pseudoFrame = frameMin + (int)std::round(frameRange * (((float)b + 0.5f) / (float)frameLegendBuckets));
                fillRect(lx0, frameLegendY0, lx1, frameLegendY1, frameColorFor(pseudoFrame, true));
            }
            drawRect(plotX0, frameLegendY0, plotX1, frameLegendY1, axisColor);

            for (int b = 0; b < kHistBins; b++) {
                if (binMeans[b].count <= 0) {
                    continue;
                }
                const float fgMean = (float)(binMeans[b].sumFg / binMeans[b].count);
                const float bgMean = (float)(binMeans[b].sumBg / binMeans[b].count);
                const float xNorm = std::max(0.0f, std::min(1.0f, fgMean / std::max(1.0f, maxv)));
                const float yNorm = std::max(0.0f, std::min(1.0f, bgMean / std::max(1.0f, maxv)));
                const int px = plotX0 + ClampInt((int)std::round(xNorm * (plotX1 - plotX0)), 0, plotX1 - plotX0);
                const int py = plotY1 - ClampInt((int)std::round(yNorm * (plotY1 - plotY0)), 0, plotY1 - plotY0);
                drawCross(px, py, { { 180, 186, 198 } });
            }

            for (const auto& rep : traceBinRepresentatives) {
                if (rep.pointId != pointId || rep.histBin < 0 || rep.histBin >= kHistBins || rep.count <= 0) {
                    continue;
                }
                const auto color = binPalette[rep.histBin];
                const float xNorm = std::max(0.0f, std::min(1.0f, rep.avgFg / std::max(1.0f, maxv)));
                const float rawYNorm = std::max(0.0f, std::min(1.0f, rep.rawBg / std::max(1.0f, maxv)));
                const float adjYNorm = std::max(0.0f, std::min(1.0f, rep.adjustedBg / std::max(1.0f, maxv)));
                const int pxRaw = plotX0 + ClampInt((int)std::round(xNorm * (plotX1 - plotX0)), 0, plotX1 - plotX0);
                const int pyRaw = plotY1 - ClampInt((int)std::round(rawYNorm * (plotY1 - plotY0)), 0, plotY1 - plotY0);
                const int pxAdj = plotX0 + ClampInt((int)std::round(xNorm * (plotX1 - plotX0)), 0, plotX1 - plotX0);
                const int pyAdj = plotY1 - ClampInt((int)std::round(adjYNorm * (plotY1 - plotY0)), 0, plotY1 - plotY0);
                drawCross(pxRaw, pyRaw, { { 170, 176, 188 } });
                drawSquareMarker(pxAdj, pyAdj, color);
            }

            const int legendY0 = plotY1 + 16;
            const int legendY1 = plotY1 + 23;
            const int legendW = std::max(1, (plotX1 - plotX0 + 1) / kHistBins);
            for (int b = 0; b < kHistBins; b++) {
                const int lx0 = plotX0 + b * legendW;
                const int lx1 = (b == kHistBins - 1) ? plotX1 : std::min(plotX1, lx0 + legendW - 1);
                fillRect(lx0, legendY0, lx1, legendY1, binPalette[b]);
            }
            drawRect(plotX0, legendY0, plotX1, legendY1, axisColor);
        }

        WriteRgbBitmap(path, width, height, [&](int x, int y) {
            return img[x + y * width];
        });
    }

    // high/low 閾値から binary を1回生成する。
    // high seed から low 領域へ成長し、さらに近縁成分を昇格して取りこぼしを補う。
    void AutoDetectLogoReader::buildBinaryFromThreshold(const ScoreStageBuffers& scoreStage, const int iterIndex, const float highTh, const float lowTh, std::vector<uint8_t>& outBinary, BuildBinaryDiag* dbg) {
        // score を high/low の2値マスクへ分解する。
        std::vector<uint8_t> seed(scanw * scanh, 0);
        std::vector<uint8_t> lowMask(scanw * scanh, 0);
        RunParallelRange(threadPool, threadN, scanh, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                for (int x = 0; x < scanw; x++) {
                    const int off = x + y * scanw;
                    if (!scoreStage.validAB[off]) continue;
                    if (scoreStage.score[off] >= highTh) {
                        seed[off] = 1;
                    }
                    if (scoreStage.score[off] >= lowTh) {
                        lowMask[off] = 1;
                    }
                }
            }
            });

        if (dbg != nullptr) {
            for (int i = 0; i < scanw * scanh; i++) {
                if (seed[i]) dbg->seedOn++;
                if (lowMask[i]) dbg->lowOn++;
            }
        }

        // seed から 8近傍で lowMask を伝播し、基本 binary を作る。
        outBinary.assign(scanw * scanh, 0);
        std::queue<int> growQ;
        for (int i = 0; i < scanw * scanh; i++) {
            if (seed[i]) {
                outBinary[i] = 1;
                growQ.push(i);
            }
        }
        while (!growQ.empty()) {
            const int cur = growQ.front(); growQ.pop();
            const int cx = cur % scanw;
            const int cy = cur / scanw;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    const int nx = cx + dx;
                    const int ny = cy + dy;
                    if (nx < 0 || nx >= scanw || ny < 0 || ny >= scanh) continue;
                    const int nidx = nx + ny * scanw;
                    if (outBinary[nidx]) continue;
                    if (!lowMask[nidx]) continue;
                    outBinary[nidx] = 1;
                    growQ.push(nidx);
                }
            }
        }
        if (dbg != nullptr) {
            for (int i = 0; i < scanw * scanh; i++) {
                if (outBinary[i]) dbg->grownOn++;
            }
        }
        // seedから到達しない low 成分を、近縁かつ高信頼なら段階的に昇格する。
        // 背景: 枠線文字など「seedには届かないが、文字列としては近い」成分の救済。
        // 同一iter内で最大3-hopまで連鎖的に取り込む。
        int anchorMinX = scanw, anchorMinY = scanh, anchorMaxX = -1, anchorMaxY = -1;
        for (int y = 0; y < scanh; y++) {
            for (int x = 0; x < scanw; x++) {
                if (!outBinary[x + y * scanw]) continue;
                anchorMinX = std::min(anchorMinX, x);
                anchorMinY = std::min(anchorMinY, y);
                anchorMaxX = std::max(anchorMaxX, x);
                anchorMaxY = std::max(anchorMaxY, y);
            }
        }
        const bool hasAnchor = (anchorMaxX >= anchorMinX && anchorMaxY >= anchorMinY);
        if (!hasAnchor) {
            return;
        }

        // 未採用 low 成分を一度列挙し、後段で近縁性と信号品質で昇格判定する。
        struct PromoteCompLocal {
            int minX = 0;
            int minY = 0;
            int maxX = -1;
            int maxY = -1;
            int area = 0;
            int compW = 0;
            int compH = 0;
            float peakScore = 0.0f;
            float meanAccepted = 0.0f;
            int overlapW = 0;
            int overlapH = 0;
            int gapX = 0;
            int gapY = 0;
            int nearHorizontal = 0;
            int nearVertical = 0;
            int nearDiagonal = 0;
            int nearAnchor = 0;
            int shapeOk = 0;
            int signalOk = 0;
            bool promoted = false;
            std::vector<int> pixels;
        };

        const int initAnchorMinX = anchorMinX;
        const int initAnchorMinY = anchorMinY;
        const int initAnchorMaxX = anchorMaxX;
        const int initAnchorMaxY = anchorMaxY;
        const int initAnchorW = initAnchorMaxX - initAnchorMinX + 1;
        const int initAnchorH = initAnchorMaxY - initAnchorMinY + 1;
        const float baseSquare = 80.0f;
        const float guardRatio = 1.0f / std::sqrt(std::max(initAnchorW, 4) * std::max(initAnchorH, 4) / (baseSquare * baseSquare));
        const int guardX = std::max(32, (int)std::round(initAnchorW * guardRatio));
        const int guardY = std::max(20, (int)std::round(initAnchorH * guardRatio));
        const int initCenterX = (initAnchorMinX + initAnchorMaxX) / 2;
        const int initCenterY = (initAnchorMinY + initAnchorMaxY) / 2;
        const int guardHalfW = std::max(1, ((initAnchorMaxX - initAnchorMinX + 1) + guardX * 2) / 2);
        const int guardHalfH = std::max(1, ((initAnchorMaxY - initAnchorMinY + 1) + guardY * 2) / 2);
        const int guardMinX = std::max(0, initCenterX - guardHalfW);
        const int guardMinY = std::max(0, initCenterY - guardHalfH);
        const int guardMaxX = std::min(scanw - 1, initCenterX + guardHalfW);
        const int guardMaxY = std::min(scanh - 1, initCenterY + guardHalfH);

        std::vector<PromoteCompLocal> comps;
        std::vector<uint8_t> visited(scanw * scanh, 0);
        std::queue<int> q;
        // lowMask かつ未採用画素を連結成分として列挙してから、多段判定する。
        for (int y = 0; y < scanh; y++) {
            for (int x = 0; x < scanw; x++) {
                const int start = x + y * scanw;
                if (visited[start] || !lowMask[start] || outBinary[start]) continue;
                visited[start] = 1;
                q.push(start);
                int minX = x, minY = y, maxX = x, maxY = y;
                int area = 0;
                double peakScore = 0.0;
                double sumAccepted = 0.0;
                std::vector<int> comp;
                while (!q.empty()) {
                    const int cur = q.front(); q.pop();
                    const int cx = cur % scanw;
                    const int cy = cur / scanw;
                    comp.push_back(cur);
                    area++;
                    minX = std::min(minX, cx);
                    minY = std::min(minY, cy);
                    maxX = std::max(maxX, cx);
                    maxY = std::max(maxY, cy);
                    peakScore = std::max(peakScore, (double)scoreStage.score[cur]);
                    sumAccepted += scoreStage.mapAccepted[cur];
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) continue;
                            const int nx = cx + dx;
                            const int ny = cy + dy;
                            if (nx < 0 || nx >= scanw || ny < 0 || ny >= scanh) continue;
                            const int nidx = nx + ny * scanw;
                            if (visited[nidx] || !lowMask[nidx] || outBinary[nidx]) continue;
                            visited[nidx] = 1;
                            q.push(nidx);
                        }
                    }
                }
                if (area <= 0) continue;

                PromoteCompLocal local{};
                local.minX = minX;
                local.minY = minY;
                local.maxX = maxX;
                local.maxY = maxY;
                local.area = area;
                local.compW = maxX - minX + 1;
                local.compH = maxY - minY + 1;
                local.peakScore = (float)peakScore;
                local.meanAccepted = (float)(sumAccepted / std::max(1, area));
                local.pixels = std::move(comp);
                comps.push_back(std::move(local));
            }
        }

        // 現在の anchor 近傍だけを最大3ホップで連鎖昇格する。
        const int maxPromoteHop = 3;
        for (int hop = 0; hop < maxPromoteHop; hop++) {
            int curAnchorMinX = scanw, curAnchorMinY = scanh, curAnchorMaxX = -1, curAnchorMaxY = -1;
            for (int y = 0; y < scanh; y++) {
                for (int x = 0; x < scanw; x++) {
                    if (!outBinary[x + y * scanw]) continue;
                    curAnchorMinX = std::min(curAnchorMinX, x);
                    curAnchorMinY = std::min(curAnchorMinY, y);
                    curAnchorMaxX = std::max(curAnchorMaxX, x);
                    curAnchorMaxY = std::max(curAnchorMaxY, y);
                }
            }
            if (curAnchorMaxX < curAnchorMinX || curAnchorMaxY < curAnchorMinY) {
                break;
            }
            const int curAnchorW = curAnchorMaxX - curAnchorMinX + 1;
            const int curAnchorH = curAnchorMaxY - curAnchorMinY + 1;
            bool anyAccepted = false;

            for (auto& comp : comps) {
                if (comp.promoted) continue;
                const int overlapW = std::max(0, std::min(comp.maxX, curAnchorMaxX) - std::max(comp.minX, curAnchorMinX) + 1);
                const int overlapH = std::max(0, std::min(comp.maxY, curAnchorMaxY) - std::max(comp.minY, curAnchorMinY) + 1);
                const int gapX = std::max(0, std::max(comp.minX - curAnchorMaxX, curAnchorMinX - comp.maxX));
                const int gapY = std::max(0, std::max(comp.minY - curAnchorMaxY, curAnchorMinY - comp.maxY));
                const int nearHMin1 = std::max(2, (int)std::round(std::min(comp.compH, curAnchorH) * 0.15));
                const int nearHMin2 = std::max(2, (int)std::round(std::min(comp.compH, curAnchorH) * 0.75));
                const bool nearHorizontal = (overlapH >= nearHMin1 && gapX <= std::max(6, (int)std::round(curAnchorW * 0.20)))
                    || (overlapH >= nearHMin2 && gapX <= std::max(10, (int)std::round(curAnchorW * 0.80)));
                const int nearVMin1 = std::max(2, (int)std::round(std::min(comp.compW, curAnchorW) * 0.15));
                const bool nearVertical = nearVMin1 && gapY <= std::max(6, (int)std::round(curAnchorH * 0.15));
                const bool nearDiagonal = gapX <= std::max(6, (int)std::round(curAnchorW * 0.12)) &&
                    gapY <= std::max(4, (int)std::round(curAnchorH * 0.08));
                const bool nearAnchor = nearHorizontal || nearVertical ||
                    (nearDiagonal && (comp.peakScore >= (float)((double)lowTh * 1.50) && comp.meanAccepted >= 0.16f));

                const double aspect = std::max((double)comp.compW / std::max(1, comp.compH), (double)comp.compH / std::max(1, comp.compW));
                const bool shapeOk = comp.area >= 3 && comp.area <= (int)(scanw * scanh * 0.12) && aspect <= 10.0;
                const bool signalOk = comp.peakScore >= (float)((double)lowTh * 1.02) || comp.meanAccepted >= 0.11f;

                const int insideW = std::max(0, std::min(comp.maxX, guardMaxX) - std::max(comp.minX, guardMinX) + 1);
                const int insideH = std::max(0, std::min(comp.maxY, guardMaxY) - std::max(comp.minY, guardMinY) + 1);
                const int compArea = std::max(1, comp.compW * comp.compH);
                const float insideRatio = (float)(insideW * insideH) / (float)compArea;
                const bool insideGuard = (insideW > 0 && insideH > 0) && insideRatio >= 0.72f;

                comp.overlapW = overlapW;
                comp.overlapH = overlapH;
                comp.gapX = gapX;
                comp.gapY = gapY;
                comp.nearHorizontal = nearHorizontal ? 1 : 0;
                comp.nearVertical = nearVertical ? 1 : 0;
                comp.nearDiagonal = nearDiagonal ? 1 : 0;
                comp.nearAnchor = (nearAnchor && insideGuard) ? 1 : 0;
                comp.shapeOk = shapeOk ? 1 : 0;
                comp.signalOk = signalOk ? 1 : 0;

                const bool accepted = insideGuard && nearAnchor && shapeOk && signalOk;
                if (!accepted) continue;
                for (const int pix : comp.pixels) {
                    outBinary[pix] = 1;
                }
                comp.promoted = true;
                anyAccepted = true;
                if (dbg != nullptr) {
                    dbg->promotedOn += comp.area;
                }
                if (detailedDebug) {
                    promoteCompDebug.push_back(PromoteCompDebug{
                        iterIndex, highTh, lowTh,
                        comp.minX, comp.minY, comp.maxX, comp.maxY, comp.area, comp.compW, comp.compH,
                        comp.overlapW, comp.overlapH, comp.gapX, comp.gapY,
                        comp.peakScore, comp.meanAccepted,
                        comp.nearHorizontal, comp.nearVertical, comp.nearDiagonal, comp.nearAnchor, comp.shapeOk, comp.signalOk, 1
                        });
                }
            }
            if (!anyAccepted) {
                break;
            }
        }
        // 非採用成分もデバッグ出力に残す。
        if (detailedDebug) {
            for (const auto& comp : comps) {
                if (comp.promoted) continue;
                promoteCompDebug.push_back(PromoteCompDebug{
                    iterIndex, highTh, lowTh,
                    comp.minX, comp.minY, comp.maxX, comp.maxY, comp.area, comp.compW, comp.compH,
                    comp.overlapW, comp.overlapH, comp.gapX, comp.gapY,
                    comp.peakScore, comp.meanAccepted,
                    comp.nearHorizontal, comp.nearVertical, comp.nearDiagonal, comp.nearAnchor, comp.shapeOk, comp.signalOk, 0
                    });
            }
        }
    }

    // binary 連結成分から anchor を選び、近縁成分のみ残してノイズを除去する。
    void AutoDetectLogoReader::pruneBinaryByAnchor(const ScoreStageBuffers& scoreStage, BinaryStageBuffers& binaryStage) {
        // prune 前状態を保持し、削りすぎ時にロールバックできるようにする。
        std::vector<uint8_t> prePruneBinary = binaryStage.binary;
        const int prePruneOn = countMaskOn(prePruneBinary);
        int preMinX = 0, preMinY = 0, preMaxX = -1, preMaxY = -1;
        const bool hasPreRect = getMaskRect(prePruneBinary, preMinX, preMinY, preMaxX, preMaxY);
        bool hasAnchorBandStat = false;
        int anchorBandPreOn = 0;
        int anchorBandPostOn = 0;
        int removedOn = 0;
        int removedBelowOn = 0;
        struct BinaryComp {
            int minX = 0;
            int minY = 0;
            int maxX = -1;
            int maxY = -1;
            int area = 0;
            int w = 0;
            int h = 0;
            float peakScore = 0.0f;
            float meanScore = 0.0f;
            float meanAccepted = 0.0f;
            float meanAlpha = 0.0f;
            float meanConsistency = 0.0f;
            float anchorScore = -1.0f;
            std::vector<int> pixels;
        };

        // 連結成分ごとに面積/信号量/位置先験を集計する。
        std::vector<BinaryComp> comps;
        std::vector<uint8_t> visited(scanw * scanh, 0);
        std::queue<int> q;
        for (int y = 0; y < scanh; y++) {
            for (int x = 0; x < scanw; x++) {
                const int start = x + y * scanw;
                if (!binaryStage.binary[start] || visited[start]) continue;
                visited[start] = 1;
                q.push(start);
                BinaryComp comp{};
                comp.minX = x;
                comp.maxX = x;
                comp.minY = y;
                comp.maxY = y;
                double sumScore = 0.0;
                double sumAccepted = 0.0;
                double sumAlpha = 0.0;
                double sumConsistency = 0.0;
                while (!q.empty()) {
                    const int cur = q.front(); q.pop();
                    const int cx = cur % scanw;
                    const int cy = cur / scanw;
                    comp.pixels.push_back(cur);
                    comp.area++;
                    comp.minX = std::min(comp.minX, cx);
                    comp.maxX = std::max(comp.maxX, cx);
                    comp.minY = std::min(comp.minY, cy);
                    comp.maxY = std::max(comp.maxY, cy);
                    comp.peakScore = std::max(comp.peakScore, scoreStage.score[cur]);
                    sumScore += scoreStage.score[cur];
                    sumAccepted += scoreStage.mapAccepted[cur];
                    sumAlpha += scoreStage.mapAlpha[cur];
                    sumConsistency += scoreStage.mapConsistency[cur];
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) continue;
                            const int nx = cx + dx;
                            const int ny = cy + dy;
                            if (nx < 0 || nx >= scanw || ny < 0 || ny >= scanh) continue;
                            const int nidx = nx + ny * scanw;
                            if (!binaryStage.binary[nidx] || visited[nidx]) continue;
                            visited[nidx] = 1;
                            q.push(nidx);
                        }
                    }
                }
                if (comp.area <= 0) continue;
                comp.w = comp.maxX - comp.minX + 1;
                comp.h = comp.maxY - comp.minY + 1;
                comp.meanScore = (float)(sumScore / std::max(1, comp.area));
                comp.meanAccepted = (float)(sumAccepted / std::max(1, comp.area));
                comp.meanAlpha = (float)(sumAlpha / std::max(1, comp.area));
                comp.meanConsistency = (float)(sumConsistency / std::max(1, comp.area));

                const float cxNorm = (float)((comp.minX + comp.maxX) * 0.5 / std::max(1, scanw - 1));
                const float topNorm = (float)(1.0 - ((comp.minY + comp.maxY) * 0.5 / std::max(1, scanh - 1)));
                const float rightPrior = std::max(0.0f, std::min(1.0f, cxNorm));
                const float topPrior = std::max(0.0f, std::min(1.0f, topNorm));
                const float consistencyPrior = std::max(0.0f, std::min(1.0f, (comp.meanConsistency - 0.35f) / 1.15f));
                const float alphaPenalty = 1.0f / (1.0f + std::max(0.0f, comp.meanAlpha - 0.78f) * 1.5f);
                const float areaGain = std::sqrt((float)std::max(1, comp.area));
                const float signalMix = comp.meanScore * 0.55f + comp.peakScore * 0.25f + comp.meanAccepted * 0.20f;
                const float topWeight = (0.25f + 0.75f * topPrior);
                comp.anchorScore =
                    areaGain *
                    signalMix *
                    (topWeight * topWeight) *
                    (0.70f + 0.30f * rightPrior) *
                    (0.45f + 0.55f * consistencyPrior) *
                    alphaPenalty;
                comps.push_back(std::move(comp));
            }
        }

        // 上側帯域を優先して anchor 成分を選ぶ。
        if (!comps.empty()) {
            int anchorIdx = -1;
            float bestAnchor = -1.0f;
            int minAnchorY = scanh;
            for (int i = 0; i < (int)comps.size(); i++) {
                const auto& c = comps[i];
                if (c.area < 6) continue;
                minAnchorY = std::min(minAnchorY, c.minY);
            }
            const int topBandSlack = std::max(12, (int)std::round(scanh * 0.15));
            const int topBandLimit = (minAnchorY < scanh) ? (minAnchorY + topBandSlack) : scanh;
            for (int i = 0; i < (int)comps.size(); i++) {
                const auto& c = comps[i];
                if (c.area < 6) continue;
                if (c.minY > topBandLimit) continue;
                if (c.anchorScore > bestAnchor) {
                    bestAnchor = c.anchorScore;
                    anchorIdx = i;
                }
            }
            if (anchorIdx < 0) {
                for (int i = 0; i < (int)comps.size(); i++) {
                    const auto& c = comps[i];
                    if (c.area < 6) continue;
                    if (c.anchorScore > bestAnchor) {
                        bestAnchor = c.anchorScore;
                        anchorIdx = i;
                    }
                }
            }
            if (anchorIdx < 0) {
                for (int i = 0; i < (int)comps.size(); i++) {
                    const auto& c = comps[i];
                    if (c.area < 3) continue;
                    if (c.anchorScore > bestAnchor) {
                        bestAnchor = c.anchorScore;
                        anchorIdx = i;
                    }
                }
            }
            if (anchorIdx >= 0) {
                const auto& anchor = comps[anchorIdx];
                const int anchorW = std::max(1, anchor.w);
                const int anchorH = std::max(1, anchor.h);
                const int anchorCenterY = (anchor.minY + anchor.maxY) / 2;
                // prune の keep 判定は anchor に対する相対量を基本にしつつ、
                // 極端に弱いケースでも完全なノイズ拾いにならない程度の最小値だけ持たせる。
                // mode1/q49 のように score 全体のスケールが小さいケースでは、
                // 旧固定下限(0.004/0.003)だと promote 済みの上端・下段成分まで
                // signalOk を満たせず prune で全落ちしていた。
                const float minSignalScore = std::max(5.0e-4f, anchor.meanScore * 0.20f);
                const float minSignalPeak = std::max(6.0e-4f, anchor.peakScore * 0.10f);
                const float minSignalAccepted = std::max(5.0e-4f, anchor.meanAccepted * 0.20f);
                const float minSignalConsistency = std::max(0.35f, anchor.meanConsistency * 0.70f);
                // 多行ロゴ(放送大学等)の下段行を拾うため、下方向の許容距離を大きく取る。
                const int maxBelowAnchor = std::max(40, (int)std::round(anchorH * 2.5));
                std::vector<uint8_t> keepComp(comps.size(), 0);
                keepComp[anchorIdx] = 1;
                bool changed = true;
                // keep済み成分に近いものを段階的に採用する。
                for (int hop = 0; hop < 10 && changed; hop++) {
                    changed = false;
                    for (int i = 0; i < (int)comps.size(); i++) {
                        if (keepComp[i]) continue;
                        const auto& c = comps[i];
                        const int compCenterY = (c.minY + c.maxY) / 2;
                        const int anchorBelowLimitY = anchorCenterY + maxBelowAnchor;
                        const int lowerTopSlack = std::max(4, (int)std::round(c.h * 0.35));
                        // 下段2行ロゴを拾うため、中心Yだけでなく上端Yでも下限判定する。
                        const bool yGuard =
                            compCenterY <= anchorBelowLimitY ||
                            c.minY <= anchorBelowLimitY + lowerTopSlack;
                        const bool shapeOk = c.area >= 3 && std::max((double)c.w / std::max(1, c.h), (double)c.h / std::max(1, c.w)) <= 16.0;
                        const bool signalOk =
                            c.meanScore >= minSignalScore ||
                            c.peakScore >= minSignalPeak ||
                            c.meanAccepted >= minSignalAccepted;
                        const bool sameRowComp = std::abs(compCenterY - anchorCenterY) <= std::max(4, (int)std::round(anchorH * 0.45));
                        const bool isLowerComp = compCenterY > anchorCenterY + std::max(4, (int)std::round(anchorH * 0.30));
                        const bool lowAlphaConsistencyOk =
                            !isLowerComp ||
                            c.meanAlpha >= 0.20f ||
                            c.meanConsistency >= minSignalConsistency;
                        // q17 で左側の靄成分が残った要因:
                        // 「同じ行にある」だけで弱信号成分まで keep されていた。
                        // sameRow 救済には最低限の信号量(accepted/consistency)を要求する。
                        const bool sameRowSignalRescue =
                            sameRowComp &&
                            (c.meanAccepted >= minSignalAccepted || c.meanConsistency >= minSignalConsistency);
                        const bool signalGateOk = signalOk || sameRowSignalRescue;
                        if (!yGuard || !shapeOk || !signalGateOk || !lowAlphaConsistencyOk) continue;
                        bool nearKept = false;
                        for (int k = 0; k < (int)comps.size(); k++) {
                            if (!keepComp[k]) continue;
                            const auto& ref = comps[k];
                            const int overlapW = std::max(0, std::min(c.maxX, ref.maxX) - std::max(c.minX, ref.minX) + 1);
                            const int overlapH = std::max(0, std::min(c.maxY, ref.maxY) - std::max(c.minY, ref.minY) + 1);
                            const int gapX = std::max(0, std::max(c.minX - ref.maxX, ref.minX - c.maxX));
                            const int gapY = std::max(0, std::max(c.minY - ref.maxY, ref.minY - c.maxY));
                            // q17 改善意図:
                            // 横方向 near 判定の gapX が広すぎると、遠方の弱成分(左側靄)が連鎖 keep される。
                            // 成分の信号強度に応じて許容距離を可変化し、弱信号は短距離のみ許可する。
                            const int nearHGapBase = std::max(20, (int)std::round(std::max(anchorW, ref.w) * 3.8));
                            const float scoreRatio = c.meanScore / std::max(1.0e-6f, minSignalScore);
                            const float peakRatio = c.peakScore / std::max(1.0e-6f, minSignalPeak);
                            const float acceptedRatio = c.meanAccepted / std::max(1.0e-6f, minSignalAccepted);
                            const float consistencyRatio = c.meanConsistency / std::max(1.0e-6f, minSignalConsistency);
                            const float signalStrength =
                                std::max(std::max(scoreRatio, peakRatio), std::max(acceptedRatio, consistencyRatio));
                            const float strengthNorm = std::max(0.0f, std::min(1.0f, (signalStrength - 1.0f) / 1.5f));
                            const float nearHGapScale = 0.45f + 0.55f * strengthNorm;
                            const int nearHGapLimit = std::max(8, (int)std::round((double)nearHGapBase * nearHGapScale));
                            const bool nearH = overlapH >= std::max(2, (int)std::round(std::min(c.h, ref.h) * 0.15)) &&
                                gapX <= nearHGapLimit;
                            // 多行ロゴの行間ギャップ(18px程度)を許容するため gapY を緩和。
                            const bool nearV = overlapW >= std::max(2, (int)std::round(std::min(c.w, ref.w) * 0.15)) &&
                                gapY <= std::max(20, (int)std::round(std::max(anchorH, ref.h) * 1.50));
                            // nearD も多行ロゴの行間を考慮して gapY を緩和。
                            const bool nearD = gapX <= std::max(10, (int)std::round(std::max(anchorW, ref.w) * 0.50)) &&
                                gapY <= std::max(12, (int)std::round(std::max(anchorH, ref.h) * 1.00));
                            nearKept = (overlapW > 0 && overlapH > 0) || nearH || nearV || nearD;
                            if (nearKept) {
                                break;
                            }
                        }
                        if (!nearKept) continue;
                        const int gapYAnchor = std::max(0, std::max(c.minY - anchor.maxY, anchor.minY - c.maxY));
                        // 多行ロゴの下段行が opaqueFar で除外されないよう gapY を緩和。
                        const bool opaqueFar = c.meanAlpha >= std::max(0.72f, anchor.meanAlpha + 0.18f) &&
                            gapYAnchor > std::max(8, (int)std::round(anchorH * 2.00));
                        if (opaqueFar) continue;
                        keepComp[i] = 1;
                        changed = true;
                    }
                }
                // keep対象のみで新しい binary を再構築する。
                std::vector<uint8_t> filtered(scanw * scanh, 0);
                for (int i = 0; i < (int)comps.size(); i++) {
                    if (!keepComp[i]) continue;
                    const auto& c = comps[i];
                    for (const int pix : c.pixels) {
                        filtered[pix] = 1;
                    }
                }
                const int bandY0 = std::max(0, anchor.minY - 2);
                const int bandY1 = std::min(scanh - 1, anchor.maxY + 2);
                for (int y = bandY0; y <= bandY1; y++) {
                    const int row = y * scanw;
                    for (int x = 0; x < scanw; x++) {
                        const int off = row + x;
                        if (prePruneBinary[off]) anchorBandPreOn++;
                        if (filtered[off]) anchorBandPostOn++;
                    }
                }
                const int belowY = anchor.maxY + std::max(2, (int)std::round(anchorH * 0.12));
                for (int i = 0; i < scanw * scanh; i++) {
                    if (!prePruneBinary[i] || filtered[i]) continue;
                    removedOn++;
                    const int py = i / scanw;
                    if (py > belowY) {
                        removedBelowOn++;
                    }
                }
                hasAnchorBandStat = true;
                binaryStage.binary.swap(filtered);
            }
        }

        // 削りすぎ判定に引っかかった場合は prune 前へ戻す。
        const int postPruneOn = countMaskOn(binaryStage.binary);
        int postMinX = 0, postMinY = 0, postMaxX = -1, postMaxY = -1;
        const bool hasPostRect = getMaskRect(binaryStage.binary, postMinX, postMinY, postMaxX, postMaxY);
        if (prePruneOn > 0 && hasPreRect) {
            bool revertPrune = false;
            if (postPruneOn <= 0 || !hasPostRect) {
                revertPrune = true;
            } else {
                const int preW = preMaxX - preMinX + 1;
                const int postW = postMaxX - postMinX + 1;
                const bool areaCollapse = postPruneOn < std::max(24, (int)std::round(prePruneOn * 0.25));
                const bool severeShrink = areaCollapse && postW < (int)std::round(preW * 0.60);
                revertPrune = severeShrink;
                if (revertPrune && hasAnchorBandStat && anchorBandPreOn > 0 && removedOn > 0) {
                    const float bandRetention = (float)anchorBandPostOn / std::max(1, anchorBandPreOn);
                    const float removedBelowRatio = (float)removedBelowOn / std::max(1, removedOn);
                    const bool trimmedMostlyBelow =
                        removedOn >= std::max(24, (int)std::round(prePruneOn * 0.05)) &&
                        removedBelowRatio >= 0.60f;
                    const bool topBandPreserved = bandRetention >= 0.72f;
                    const bool postStillEnough = postPruneOn >= std::max(12, (int)std::round(prePruneOn * 0.10));
                    if (trimmedMostlyBelow && topBandPreserved && postStillEnough) {
                        revertPrune = false;
                    }
                }
            }
            if (revertPrune) {
                binaryStage.binary.swap(prePruneBinary);
            }
        }
    }

    // binary が空になったときのみ、mapAccepted 上位点から最小限の復旧を行う。
    bool AutoDetectLogoReader::applyBinaryFallbackIfEmpty(const ScoreStageBuffers& scoreStage, BinaryStageBuffers& binaryStage) {
        // 既に画素が残っている場合はフォールバック不要。
        int binaryOnCount = 0;
        for (int i = 0; i < scanw * scanh; i++) {
            if (binaryStage.binary[i]) binaryOnCount++;
        }
        if (binaryOnCount > 0) {
            return false;
        }

        // フォールバック: 通常二値化で全消しになった場合のみ適用。
        // 復旧候補の分布を作り、上位パーセンタイル閾値を決める。
        std::vector<float> acceptedVals;
        acceptedVals.reserve(scanw * scanh);
        for (int i = 0; i < scanw * scanh; i++) {
            if (!scoreStage.validAB[i]) continue;
            const double consistencyNorm = std::max(0.0, std::min(1.0, (scoreStage.mapConsistency[i] - 0.30) / 1.70));
            if (scoreStage.mapAlpha[i] < 0.015f && consistencyNorm < 0.20) continue;
            acceptedVals.push_back(scoreStage.mapAccepted[i]);
        }
        if (acceptedVals.empty()) {
            return false;
        }

        std::sort(acceptedVals.begin(), acceptedVals.end());
        const int idx = ClampInt((int)std::round((acceptedVals.size() - 1) * 0.995), 0, (int)acceptedVals.size() - 1);
        const float fbTh = std::max(0.06f, acceptedVals[idx]);
        bool restored = false;
        // 閾値を超える画素だけを binary に復帰させる。
        for (int i = 0; i < scanw * scanh; i++) {
            if (!scoreStage.validAB[i]) continue;
            const double consistencyNorm = std::max(0.0, std::min(1.0, (scoreStage.mapConsistency[i] - 0.30) / 1.70));
            if (scoreStage.mapAccepted[i] >= fbTh && (scoreStage.mapAlpha[i] >= 0.015f || consistencyNorm >= 0.20)) {
                binaryStage.binary[i] = 1;
                restored = true;
            }
        }
        return restored;
    }

    // ステージ2: scoreを2値化してロゴ候補マスク(binary)を確定する。
    // 処理概要:
    // - high/lowのヒステリシス(seed->grow)で基本マスクを生成
    // - low側の飛び地を近縁条件で昇格
    // - 反復的な閾値緩和とdelta近縁判定で取りこぼしを補完
    // - 帯ノイズ/全消し時のフォールバックを適用
    void AutoDetectLogoReader::runBinaryStage(const ScoreStageBuffers& scoreStage, BinaryStageBuffers& binaryStage, const float thHigh, const float thLow, bool enableRescue) {
        // 初回2値化（基準となる「最初の領域」）
        BuildBinaryDiag baseDiag{};
        if (detailedDebug) {
            promoteCompDebug.clear();
            deltaCompDebug.clear();
        }
        buildBinaryFromThreshold(scoreStage, 0, thHigh, thLow, binaryStage.binary, &baseDiag);
        initialSeedCount = baseDiag.seedOn;
        initialGrownCount = baseDiag.grownOn;
        if (baseDiag.seedOn <= 0) {
            setRectDetectFail(LogoRectDetectFail::NoSeed);
        }

        std::vector<uint8_t> acceptedBinary = binaryStage.binary;
        if (detailedDebug) {
            iterBinaryHistory.clear();
            iterThresholdDebug.clear();
        }
        if (detailedDebug) {
            iterBinaryHistory.push_back(acceptedBinary);
            iterThresholdDebug.push_back(IterThresholdDebug{
                thHigh, thLow,
                baseDiag.seedOn, baseDiag.lowOn, baseDiag.grownOn, baseDiag.promotedOn,
                0, 0, 0, countMaskOn(acceptedBinary), 0
                });
        }

        int initMinX = 0, initMinY = 0, initMaxX = -1, initMaxY = -1;
        const bool hasInitRect = getMaskRect(binaryStage.binary, initMinX, initMinY, initMaxX, initMaxY);
        const int initW = hasInitRect ? (initMaxX - initMinX + 1) : 0;
        const int initH = hasInitRect ? (initMaxY - initMinY + 1) : 0;

        // 閾値を段階的に緩め、増分領域が「初回領域に近縁」なら採用を継続する。
        // 増分に遠方成分が混ざった時点で、1つ前の結果で確定。
        float curThHigh = thHigh;
        float curThLow = thLow;
        // seedは維持しつつ、low側だけを強めに緩和する。
        // 背景: scoreがやや低いロゴ文字をlowMaskへ入れたい一方で、
        // highを下げすぎると遠方ノイズがseed化して不安定になりやすい。
        const float lowRelaxMin = std::max(0.0f, std::min(thLow * 0.36f, thHigh * 0.30f));
        for (int iter = 0; iter < 5; iter++) {
            curThHigh = std::max(0.0f, curThHigh * 0.94f);
            // lowは積極的に緩和して連結を促進
            curThLow = std::max(lowRelaxMin, curThLow * 0.90f);
            curThLow = std::min(curThLow, curThHigh * 0.90f);

            std::vector<uint8_t> candBinary;
            BuildBinaryDiag stepDiag{};
            buildBinaryFromThreshold(scoreStage, iter + 1, curThHigh, curThLow, candBinary, &stepDiag);

            // 直前採用結果との差分だけを抽出し、増分成分を評価する。
            std::vector<uint8_t> delta(scanw * scanh, 0);
            int newCount = 0;
            for (int i = 0; i < scanw * scanh; i++) {
                if (candBinary[i] && !acceptedBinary[i]) {
                    delta[i] = 1;
                    newCount++;
                }
            }
            // 増えていない場合はさらに緩和を継続。
            if (newCount <= 0) {
                acceptedBinary.swap(candBinary);
                if (detailedDebug) {
                    iterBinaryHistory.push_back(acceptedBinary);
                    iterThresholdDebug.push_back(IterThresholdDebug{
                        curThHigh, curThLow,
                        stepDiag.seedOn, stepDiag.lowOn, stepDiag.grownOn, stepDiag.promotedOn,
                        newCount, 0, 0, countMaskOn(acceptedBinary), 2
                        });
                }
                continue;
            }
            // 初期矩形がない場合は近縁判定を行わず、そのまま更新する。
            if (!hasInitRect) {
                acceptedBinary.swap(candBinary);
                if (detailedDebug) {
                    iterBinaryHistory.push_back(acceptedBinary);
                    iterThresholdDebug.push_back(IterThresholdDebug{
                        curThHigh, curThLow,
                        stepDiag.seedOn, stepDiag.lowOn, stepDiag.grownOn, stepDiag.promotedOn,
                        newCount, 0, 0, countMaskOn(acceptedBinary), 0
                        });
                }
                continue;
            }

            int prevMinX = 0, prevMinY = 0, prevMaxX = -1, prevMaxY = -1;
            const bool hasPrevRect = getMaskRect(acceptedBinary, prevMinX, prevMinY, prevMaxX, prevMaxY);
            const int prevW = hasPrevRect ? (prevMaxX - prevMinX + 1) : initW;
            const int prevH = hasPrevRect ? (prevMaxY - prevMinY + 1) : initH;

            // 初回領域を基準にした緩い上限（拡大暴走防止）
            const float baseSquare = 64.0f;
            const float guardRatio = 1.0f / std::sqrt(std::max(initW, 4) * std::max(initH, 4) / (baseSquare * baseSquare));
            const int guardX = std::max(32, (int)std::round(initW * guardRatio));
            const int guardY = std::max(20, (int)std::round(initH * guardRatio));
            // guardの「最大サイズ」は固定しつつ、中心は反復に応じて追従させる。
            // 背景:
            // - 初期領域が左寄り/正方形寄りだと、横長ロゴの右側拡張を止めてしまう。
            // - ただし中心を自由移動させるとノイズ側へ流されるため、初期中心からの移動量に上限を設ける。
            const int initCenterX = (initMinX + initMaxX) / 2;
            const int initCenterY = (initMinY + initMaxY) / 2;
            const int prevCenterX = hasPrevRect ? ((prevMinX + prevMaxX) / 2) : initCenterX;
            const int prevCenterY = hasPrevRect ? ((prevMinY + prevMaxY) / 2) : initCenterY;
            const int shiftLimitX = std::min(guardX, std::max(8, (iter + 1) * std::max(3, prevW / 8)));
            const int shiftLimitY = std::min(guardY, std::max(6, (iter + 1) * std::max(2, prevH / 8)));
            const int guardCenterX = initCenterX + ClampInt(prevCenterX - initCenterX, -shiftLimitX, shiftLimitX);
            const int guardCenterY = initCenterY + ClampInt(prevCenterY - initCenterY, -shiftLimitY, shiftLimitY);
            const int guardHalfW = std::max(1, ((initMaxX - initMinX + 1) + guardX * 2) / 2);
            const int guardHalfH = std::max(1, ((initMaxY - initMinY + 1) + guardY * 2) / 2);
            const int guardMinX = std::max(0, guardCenterX - guardHalfW);
            const int guardMinY = std::max(0, guardCenterY - guardHalfH);
            const int guardMaxX = std::min(scanw - 1, guardCenterX + guardHalfW);
            const int guardMaxY = std::min(scanh - 1, guardCenterY + guardHalfH);

            std::vector<uint8_t> visitedDelta(scanw * scanh, 0);
            std::queue<int> dq;
            bool hasFarDelta = false;
            bool hasNearDelta = false;
            int nearCompCount = 0;
            int farCompCount = 0;
            // delta 成分を近縁/遠方に分類し、遠方成分は今回反復から除外する。
            std::vector<uint8_t> filteredBinary = candBinary;
            for (int y = 0; y < scanh; y++) {
                for (int x = 0; x < scanw; x++) {
                    const int start = x + y * scanw;
                    if (!delta[start] || visitedDelta[start]) continue;
                    visitedDelta[start] = 1;
                    dq.push(start);
                    int minX = x, minY = y, maxX = x, maxY = y;
                    std::vector<int> compPixels;
                    while (!dq.empty()) {
                        const int cur = dq.front(); dq.pop();
                        const int cx = cur % scanw;
                        const int cy = cur / scanw;
                        compPixels.push_back(cur);
                        minX = std::min(minX, cx);
                        minY = std::min(minY, cy);
                        maxX = std::max(maxX, cx);
                        maxY = std::max(maxY, cy);
                        for (int dy = -1; dy <= 1; dy++) {
                            for (int dx = -1; dx <= 1; dx++) {
                                if (dx == 0 && dy == 0) continue;
                                const int nx = cx + dx;
                                const int ny = cy + dy;
                                if (nx < 0 || nx >= scanw || ny < 0 || ny >= scanh) continue;
                                const int nidx = nx + ny * scanw;
                                if (!delta[nidx] || visitedDelta[nidx]) continue;
                                visitedDelta[nidx] = 1;
                                dq.push(nidx);
                            }
                        }
                    }
                    const int compW = maxX - minX + 1;
                    const int compH = maxY - minY + 1;
                    const int refMinX = hasPrevRect ? prevMinX : initMinX;
                    const int refMinY = hasPrevRect ? prevMinY : initMinY;
                    const int refMaxX = hasPrevRect ? prevMaxX : initMaxX;
                    const int refMaxY = hasPrevRect ? prevMaxY : initMaxY;
                    const int overlapW = std::max(0, std::min(maxX, refMaxX) - std::max(minX, refMinX) + 1);
                    const int overlapH = std::max(0, std::min(maxY, refMaxY) - std::max(minY, refMinY) + 1);
                    const int gapX = std::max(0, std::max(minX - refMaxX, refMinX - maxX));
                    const int gapY = std::max(0, std::max(minY - refMaxY, refMinY - maxY));
                    // 「近縁」判定:
                    // - 直前採用領域と十分重なる
                    // - あるいは、文字間ギャップとして説明できる距離にある
                    // 欠落しやすかったケース向けに gap 条件をやや緩和。
                    const bool nearHorizontal = overlapH >= std::max(1, (int)std::round(std::min(compH, prevH) * 0.10)) && gapX <= std::max(30, (int)std::round(prevW * 0.78));
                    const bool nearVertical = overlapW >= std::max(1, (int)std::round(std::min(compW, prevW) * 0.10)) && gapY <= std::max(16, (int)std::round(prevH * 0.78));
                    const bool nearDiagonal = gapX <= std::max(14, (int)std::round(prevW * 0.35)) && gapY <= std::max(12, (int)std::round(prevH * 0.35));
                    const bool nearInitial = nearHorizontal || nearVertical || nearDiagonal;
                    // 完全内包だと、初期矩形が小さいケースで「近縁だが少しはみ出す」成分を落としてしまう。
                    // 交差していれば許可とし、暴走はnear判定で抑える。
                    // guard判定は「完全包含」だと文字端の小さなはみ出しで落ちやすい。
                    // そこで、内包率(成分bboxのうちguard内に入る割合)が十分高ければ許可する。
                    const int insideW = std::max(0, std::min(maxX, guardMaxX) - std::max(minX, guardMinX) + 1);
                    const int insideH = std::max(0, std::min(maxY, guardMaxY) - std::max(minY, guardMinY) + 1);
                    const int compArea = std::max(1, compW * compH);
                    const float insideRatio = (float)(insideW * insideH) / (float)compArea;
                    const bool withinInitialGuard =
                        (insideW > 0 && insideH > 0) && insideRatio >= 0.72f;
                    const bool deltaAccepted = nearInitial && withinInitialGuard;
                    if (detailedDebug) {
                        deltaCompDebug.push_back(DeltaCompDebug{
                            iter + 1, curThHigh, curThLow,
                            minX, minY, maxX, maxY, compW, compH,
                            overlapW, overlapH, gapX, gapY,
                            nearHorizontal ? 1 : 0,
                            nearVertical ? 1 : 0,
                            nearDiagonal ? 1 : 0,
                            nearInitial ? 1 : 0,
                            withinInitialGuard ? 1 : 0,
                            deltaAccepted ? 1 : 0
                            });
                    }
                    if (nearInitial && withinInitialGuard) {
                        hasNearDelta = true;
                        nearCompCount++;
                    } else {
                        hasFarDelta = true;
                        farCompCount++;
                        // 遠方増分は今回反復では採用しない（近縁増分の巻き添え防止）
                        for (const int pix : compPixels) {
                            filteredBinary[pix] = 0;
                        }
                    }
                }
            }
            // 遠方成分しか増えなかったら前回結果で確定。
            if (hasFarDelta && !hasNearDelta) {
                if (detailedDebug) {
                    iterBinaryHistory.push_back(acceptedBinary);
                    iterThresholdDebug.push_back(IterThresholdDebug{
                        curThHigh, curThLow,
                        stepDiag.seedOn, stepDiag.lowOn, stepDiag.grownOn, stepDiag.promotedOn,
                        newCount, nearCompCount, farCompCount, countMaskOn(acceptedBinary), 1
                        });
                }
                break;
            }
            // 近縁成分は採用し、遠方成分だけ除外して次反復へ進む。
            acceptedBinary.swap(filteredBinary);
            if (detailedDebug) {
                iterBinaryHistory.push_back(acceptedBinary);
                iterThresholdDebug.push_back(IterThresholdDebug{
                    curThHigh, curThLow,
                    stepDiag.seedOn, stepDiag.lowOn, stepDiag.grownOn, stepDiag.promotedOn,
                    newCount, nearCompCount, farCompCount, countMaskOn(acceptedBinary), 0
                    });
            }
        }
        binaryStage.binary.swap(acceptedBinary);

        // rescue 拡張: 通常は pass2 (passIndex==3) で実施する。
        // pass2 不成立時は enableRescue=true で pass1 フォールバック時にも適用する。
        // 反復ループ完了後の回帰バイナリを基準に後処理として実施することで、
        // 反復ループの guard 機構が rescue 拡張の影響を受けないようにする。
        // NoSeed のときは高確信度 rescue 画素を seed として直接使う緊急フォールバック。
        if (passIndex == 3 || enableRescue) {
            constexpr float kRescueLowMaskTh = 0.20f;
            constexpr float kRescueSeedTh    = 0.60f; // rescue seed 採用閾値 (NoSeed フォールバック)
            constexpr int   kRescueMaxDist   = 40;    // 回帰バイナリからの最大許容距離 (pixels)

            // 反復後の回帰バイナリから BFS で距離マップを計算する。
            // NoSeed のとき binaryStage.binary はゼロ → distMap はすべて INT_MAX。
            std::vector<int> distMap(scanw * scanh, INT_MAX);
            {
                std::queue<int> bfsQ;
                for (int i = 0; i < scanw * scanh; i++) {
                    if (binaryStage.binary[i]) {
                        distMap[i] = 0;
                        bfsQ.push(i);
                    }
                }
                while (!bfsQ.empty()) {
                    const int cur = bfsQ.front(); bfsQ.pop();
                    if (distMap[cur] >= kRescueMaxDist) continue;
                    const int cx = cur % scanw, cy = cur / scanw;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (!dx && !dy) continue;
                            const int nx = cx + dx, ny = cy + dy;
                            if (nx < 0 || nx >= scanw || ny < 0 || ny >= scanh) continue;
                            const int nidx = nx + ny * scanw;
                            if (distMap[nidx] > distMap[cur] + 1) {
                                distMap[nidx] = distMap[cur] + 1;
                                bfsQ.push(nidx);
                            }
                        }
                    }
                }
            }

            // NoSeed フォールバック: 回帰 seed がゼロのとき、高確信度 rescue 画素を seed とする。
            if (baseDiag.seedOn <= 0) {
                bool hasRescueSeed = false;
                for (int i = 0; i < scanw * scanh; i++) {
                    if (!scoreStage.validAB[i] && scoreStage.mapRescueScore[i] >= kRescueSeedTh) {
                        binaryStage.binary[i] = 1;
                        distMap[i] = 0;
                        hasRescueSeed = true;
                    }
                }
                if (hasRescueSeed) {
                    rectDetectFail = LogoRectDetectFail::None; // フォールバック成功
                    // 距離マップを rescue seeds から再計算
                    std::queue<int> bfsQ;
                    for (int i = 0; i < scanw * scanh; i++) {
                        if (binaryStage.binary[i]) bfsQ.push(i);
                    }
                    while (!bfsQ.empty()) {
                        const int cur = bfsQ.front(); bfsQ.pop();
                        if (distMap[cur] >= kRescueMaxDist) continue;
                        const int cx = cur % scanw, cy = cur / scanw;
                        for (int dy = -1; dy <= 1; dy++) {
                            for (int dx = -1; dx <= 1; dx++) {
                                if (!dx && !dy) continue;
                                const int nx = cx + dx, ny = cy + dy;
                                if (nx < 0 || nx >= scanw || ny < 0 || ny >= scanh) continue;
                                const int nidx = nx + ny * scanw;
                                if (distMap[nidx] > distMap[cur] + 1) {
                                    distMap[nidx] = distMap[cur] + 1;
                                    bfsQ.push(nidx);
                                }
                            }
                        }
                    }
                }
            }

            // 距離制約内の rescue lowMask 画素へ成長させる。
            // validAB=false の画素は常に対象。
            // enableRescue 時(pass1 フォールバック)は、contaminated 画素
            // (validAB=true, score<閾値) も対象に含める。
            // スコア段階では contaminated rescue を適用しないため、
            // binary 段階の BFS 成長で空間的に近い画素のみ取り込む。
            {
                constexpr float kContaminatedScoreTh = 0.05f;
                // まず距離制約内の rescue 画素を lowMask として用意
                std::vector<uint8_t> rescueLowMask(scanw * scanh, 0);
                for (int i = 0; i < scanw * scanh; i++) {
                    if (binaryStage.binary[i]) continue;  // すでに binary に入っている画素はスキップ
                    if (scoreStage.mapRescueScore[i] < kRescueLowMaskTh) continue;
                    if (distMap[i] > kRescueMaxDist) continue;
                    const bool isNoAB = !scoreStage.validAB[i];
                    const bool isContaminated = enableRescue && scoreStage.validAB[i]
                        && scoreStage.score[i] < kContaminatedScoreTh;
                    if (isNoAB || isContaminated) {
                        rescueLowMask[i] = 1;
                    }
                }
                // 既存 binary から rescueLowMask へ 8 近傍成長
                std::queue<int> growQ;
                for (int i = 0; i < scanw * scanh; i++) {
                    if (binaryStage.binary[i]) growQ.push(i);
                }
                while (!growQ.empty()) {
                    const int cur = growQ.front(); growQ.pop();
                    const int cx = cur % scanw, cy = cur / scanw;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (!dx && !dy) continue;
                            const int nx = cx + dx, ny = cy + dy;
                            if (nx < 0 || nx >= scanw || ny < 0 || ny >= scanh) continue;
                            const int nidx = nx + ny * scanw;
                            if (binaryStage.binary[nidx]) continue;
                            if (!rescueLowMask[nidx]) continue;
                            binaryStage.binary[nidx] = 1;
                            growQ.push(nidx);
                        }
                    }
                }
            }
        }

        // 反復閾値調整後の最終ノイズ抑制と空マスク救済を適用する。
        if (enablePruneBinaryByAnchor) {
            pruneBinaryByAnchor(scoreStage, binaryStage);
        }
        usedBinaryFallback = applyBinaryFallbackIfEmpty(scoreStage, binaryStage);
        if (usedBinaryFallback) {
            setRectDetectFail(LogoRectDetectFail::BinaryFallbackUsed);
        }
    }

    // binary の x/y 投影を作る。
    // ON画素が存在する列/行をフラグ化し、矩形候補探索で使う。
    void AutoDetectLogoReader::collectBinaryProjection(std::vector<uint8_t>& xOn, std::vector<uint8_t>& yOn, const BinaryStageBuffers& binaryStage) const {
        // 全画素を走査し、binary ON に対応する x/y を立てる。
        for (int y = 0; y < scanh; y++) {
            for (int x = 0; x < scanw; x++) {
                const int off = x + y * scanw;
                if (!binaryStage.binary[off]) continue;
                xOn[x] = 1;
                yOn[y] = 1;
            }
        }
    }

    // binary 連結成分を列挙し、矩形候補と結合候補を収集する。
    // 同時に最良候補(best)をスコア最大で更新する。
    void AutoDetectLogoReader::collectRectCandidates(std::vector<RectStageCompCandidate>& candidates, std::vector<RectStageCompCandidate>& mergeCandidates, AutoDetectRect& best, bool& hasBest, double& bestScore, const ScoreStageBuffers& scoreStage, const BinaryStageBuffers& binaryStage) {
        // 連結成分 BFS に必要なワークを初期化する。
        std::vector<uint8_t> visited(scanw * scanh, 0);
        std::queue<int> q;
        const int minArea = std::max(24, scanw * scanh / 2048);
        for (int y = 0; y < scanh; y++) {
            for (int x = 0; x < scanw; x++) {
                const int start = x + y * scanw;
                if (!binaryStage.binary[start] || visited[start]) continue;
                visited[start] = 1;
                q.push(start);
                int minX = x, maxX = x, minY = y, maxY = y, area = 0;
                int perimeter = 0;
                std::vector<int> compPixels;
                while (!q.empty()) {
                    const int cur = q.front(); q.pop();
                    const int cx = cur % scanw;
                    const int cy = cur / scanw;
                    compPixels.push_back(cur);
                    area++;
                    minX = std::min(minX, cx);
                    maxX = std::max(maxX, cx);
                    minY = std::min(minY, cy);
                    maxY = std::max(maxY, cy);
                    const int nx[4] = { cx - 1, cx + 1, cx, cx };
                    const int ny[4] = { cy, cy, cy - 1, cy + 1 };
                    for (int i = 0; i < 4; i++) {
                        if (nx[i] < 0 || nx[i] >= scanw || ny[i] < 0 || ny[i] >= scanh) {
                            perimeter++;
                            continue;
                        }
                        const int nidx = nx[i] + ny[i] * scanw;
                        if (!binaryStage.binary[nidx]) {
                            perimeter++;
                            continue;
                        }
                        if (visited[nidx]) continue;
                        visited[nidx] = 1;
                        q.push(nidx);
                    }
                }
                AutoDetectRect comp{ minX, minY, maxX - minX + 1, maxY - minY + 1 };
                if (comp.w <= 0 || comp.h <= 0) continue;
                const double boxArea = (double)comp.w * comp.h;
                if (boxArea <= 0.0) continue;
                if (area >= 4) {
                    mergeCandidates.push_back(RectStageCompCandidate{ comp, 0.0, area });
                }
                if (area < minArea) continue;

                // 明らかな帯ノイズを形状特徴で除外する。
                const double aspect = std::max((double)comp.w / std::max(1, comp.h), (double)comp.h / std::max(1, comp.w));
                const double fillRatio = area / boxArea;
                const double compactness = (perimeter > 0) ? (4.0 * 3.14159265358979323846 * area / (perimeter * perimeter)) : 0.0;
                if (aspect > 6.5) continue;
                if (fillRatio < 0.08) continue;
                if (compactness < 0.010) continue;

                std::vector<int> rowCount(comp.h, 0), colCount(comp.w, 0);
                for (const int pix : compPixels) {
                    const int px = pix % scanw;
                    const int py = pix / scanw;
                    rowCount[py - comp.y]++;
                    colCount[px - comp.x]++;
                }
                int maxRow = 0, maxCol = 0;
                int usedRows = 0, usedCols = 0;
                for (const int v : rowCount) {
                    maxRow = std::max(maxRow, v);
                    if (v > 0) usedRows++;
                }
                for (const int v : colCount) {
                    maxCol = std::max(maxCol, v);
                    if (v > 0) usedCols++;
                }
                const double maxRowRatio = (area > 0) ? (double)maxRow / area : 0.0;
                const double maxColRatio = (area > 0) ? (double)maxCol / area : 0.0;
                const double rowCoverage = (comp.h > 0) ? (double)usedRows / comp.h : 0.0;
                const double colCoverage = (comp.w > 0) ? (double)usedCols / comp.w : 0.0;
                const bool isWideBand = comp.w > (int)(scanw * 0.45) && comp.h < (int)(scanh * 0.22);
                const bool isRowLineLike = maxRowRatio > 0.15 && rowCoverage < 0.52 && aspect > 2.4;
                const bool isColLineLike = maxColRatio > 0.15 && colCoverage < 0.52 && aspect > 2.4;
                if (isWideBand || isRowLineLike || isColLineLike) continue;

                // ロゴらしさスコアを計算し、候補リストと best を更新する。
                const double cxComp = comp.x + comp.w * 0.5;
                const double cyComp = comp.y + comp.h * 0.5;
                const double rightTopPrior = std::max(0.0, std::min(1.0, (cxComp / scanw) * 0.60 + ((scanh - cyComp) / scanh) * 0.40));
                const double rowLinePenalty = std::max(0.0, (maxRowRatio - 0.11) * 2.4);
                const double colLinePenalty = std::max(0.0, (maxColRatio - 0.11) * 2.4);
                const double compScore =
                    std::log(1.0 + area) * 1.30 +
                    fillRatio * 0.55 +
                    compactness * 0.40 +
                    rightTopPrior * 0.35 -
                    rowLinePenalty * 1.60 -
                    colLinePenalty * 1.60;
                candidates.push_back(RectStageCompCandidate{ comp, compScore, area });

                if (compScore > bestScore) {
                    bestScore = compScore;
                    best = comp;
                    hasBest = true;
                }
            }
        }
    }

    // 通常候補が取れない場合の予備矩形を mapAccepted 最大点から作る。
    AutoDetectRect AutoDetectLogoReader::buildAcceptedFallbackRect(const ScoreStageBuffers& scoreStage) const {
        // もっとも信頼度の高い accepted 画素を探索する。
        int bestIdx = -1;
        float bestV = 0.0f;
        for (int i = 0; i < scanw * scanh; i++) {
            if (!scoreStage.validAB[i]) continue;
            if (scoreStage.mapAccepted[i] > bestV) {
                bestV = scoreStage.mapAccepted[i];
                bestIdx = i;
            }
        }
        if (bestIdx < 0 || bestV <= 0.0f) {
            return AutoDetectRect{ 0, 0, 0, 0 };
        }
        // 最大点を中心に固定半径の矩形を返す。
        const int cx = bestIdx % scanw;
        const int cy = bestIdx / scanw;
        const int halfW = std::min(scanw / 4, 32);
        const int halfH = std::min(scanh / 4, 24);
        return AutoDetectRect{
            std::max(0, cx - halfW),
            std::max(0, cy - halfH),
            std::min(scanw - std::max(0, cx - halfW), halfW * 2 + 1),
            std::min(scanh - std::max(0, cy - halfH), halfH * 2 + 1)
        };
    }

    void AutoDetectLogoReader::mergeRectCandidates(const std::vector<RectStageCompCandidate>& mergeCandidates, const AutoDetectRect& best, AutoDetectRect& finalRect) {
        bool mergedAny = true;
        int mergeIter = 0;
        const double bestCx = best.x + best.w * 0.5;
        const double bestCy = best.y + best.h * 0.5;
        const double maxCenterDx = std::max(80.0, best.w * 3.2);
        const double maxCenterDy = std::max(64.0, best.h * 3.2);
        while (mergedAny && mergeIter < 8) {
            mergedAny = false;
            mergeIter++;
            for (const auto& cand : mergeCandidates) {
                // q49 のような上端の点成分を残すため area=5 までは候補に残す。
                // ただし 1-3px 級まで広げるとノイズ側が勝ちやすいので 4px 未満は切る。
                if (cand.area < 4) continue;
                const AutoDetectRect& comp = cand.rect;
                if (comp.x == finalRect.x && comp.y == finalRect.y && comp.w == finalRect.w && comp.h == finalRect.h) continue;
                const int ix0 = std::max(finalRect.x, comp.x);
                const int iy0 = std::max(finalRect.y, comp.y);
                const int ix1 = std::min(finalRect.x + finalRect.w, comp.x + comp.w);
                const int iy1 = std::min(finalRect.y + finalRect.h, comp.y + comp.h);
                const int overlapW = std::max(0, ix1 - ix0);
                const int overlapH = std::max(0, iy1 - iy0);
                const int gapX = std::max(0, std::max(finalRect.x - (comp.x + comp.w), comp.x - (finalRect.x + finalRect.w)));
                const int gapY = std::max(0, std::max(finalRect.y - (comp.y + comp.h), comp.y - (finalRect.y + finalRect.h)));
                const bool nearHorizontal = overlapH >= std::max(1, (int)std::round(std::min(finalRect.h, comp.h) * 0.08)) &&
                    gapX <= std::max(28, (int)std::round(std::min(finalRect.w, comp.w) * 1.30));
                const bool nearVertical = overlapW >= std::max(1, (int)std::round(std::min(finalRect.w, comp.w) * 0.12)) &&
                    gapY <= std::max(14, (int)std::round(std::min(finalRect.h, comp.h) * 0.85));
                const bool nearDiagonal = gapX <= std::max(12, (int)std::round(std::min(finalRect.w, comp.w) * 0.45)) &&
                    gapY <= std::max(10, (int)std::round(std::min(finalRect.h, comp.h) * 0.45));
                const bool overlap = (overlapW > 0 && overlapH > 0);
                const double compCx = comp.x + comp.w * 0.5;
                const double compCy = comp.y + comp.h * 0.5;
                const bool withinSeedCenterGuard =
                    std::abs(compCx - bestCx) <= maxCenterDx &&
                    std::abs(compCy - bestCy) <= maxCenterDy;
                const double finalCx = finalRect.x + finalRect.w * 0.5;
                const double finalCy = finalRect.y + finalRect.h * 0.5;
                const double maxFinalDx = std::max(40.0, finalRect.w * 1.45);
                const double maxFinalDy = std::max(34.0, finalRect.h * 1.45);
                const bool withinFinalCenterGuard =
                    std::abs(compCx - finalCx) <= maxFinalDx &&
                    std::abs(compCy - finalCy) <= maxFinalDy;
                const bool isSmallComp = cand.area < 6;
                // 小成分は単独だとノイズ率が高いので、既存 rect に上下左右どちらかで
                // ほぼ接続している場合だけ取り込む。これで q49 の上部点は拾いつつ、
                // 離れた飛び地ノイズの混入は抑える。
                const bool smallCompNearVertical =
                    overlapW > 0 &&
                    gapY <= std::max(10, (int)std::round(std::min(finalRect.h, comp.h) * 2.5));
                const bool smallCompNearHorizontal =
                    overlapH > 0 &&
                    gapX <= std::max(8, (int)std::round(std::min(finalRect.w, comp.w) * 2.5));
                const bool smallCompAllowed = !isSmallComp || smallCompNearVertical || smallCompNearHorizontal;
                AutoDetectRect mergedRect{
                    std::min(finalRect.x, comp.x),
                    std::min(finalRect.y, comp.y),
                    std::max(finalRect.x + finalRect.w, comp.x + comp.w) - std::min(finalRect.x, comp.x),
                    std::max(finalRect.y + finalRect.h, comp.y + comp.h) - std::min(finalRect.y, comp.y)
                };
                const bool sizeGuardOk =
                    mergedRect.w <= (int)(scanw * 0.88) &&
                    mergedRect.h <= (int)(scanh * 0.62);
                bool accepted = true;
                if (!(overlap || nearHorizontal || nearVertical || nearDiagonal)) accepted = false;
                if (!(withinSeedCenterGuard || withinFinalCenterGuard)) accepted = false;
                if (!smallCompAllowed) accepted = false;
                if (!sizeGuardOk) accepted = false;
                if (detailedDebug) {
                    rectMergeDebug.push_back(RectMergeDebug{
                        mergeIter,
                        comp.x, comp.y, comp.x + comp.w - 1, comp.y + comp.h - 1,
                        cand.area,
                        overlapW, overlapH, gapX, gapY,
                        nearHorizontal ? 1 : 0,
                        nearVertical ? 1 : 0,
                        nearDiagonal ? 1 : 0,
                        overlap ? 1 : 0,
                        withinSeedCenterGuard ? 1 : 0,
                        withinFinalCenterGuard ? 1 : 0,
                        sizeGuardOk ? 1 : 0,
                        accepted ? 1 : 0
                        });
                }
                if (!accepted) continue;
                if (mergedRect.x != finalRect.x || mergedRect.y != finalRect.y || mergedRect.w != finalRect.w || mergedRect.h != finalRect.h) {
                    finalRect = mergedRect;
                    mergedAny = true;
                }
            }
        }
    }

    // ローカル矩形を絶対座標へ変換し、サイズ制約と偶数丸めを適用する。
    AutoDetectRect AutoDetectLogoReader::makeAbsRectFromLocal(const AutoDetectRect& localRect) const {
        // scan ROI のオフセットを足して絶対座標化する。
        AutoDetectRect absRect{
            localRect.x + scanx,
            localRect.y + scany,
            localRect.w,
            localRect.h
        };
        // 中心位置を維持しつつサイズをクランプする。
        const double cx = absRect.x + absRect.w * 0.5;
        const double cy = absRect.y + absRect.h * 0.5;
        absRect.w = ClampInt(absRect.w, 32, 360);
        absRect.h = ClampInt(absRect.h, 32, 360);
        absRect.x = ClampInt((int)std::round(cx - absRect.w * 0.5), 0, std::max(0, imgw - absRect.w));
        absRect.y = ClampInt((int)std::round(cy - absRect.h * 0.5), 0, std::max(0, imgh - absRect.h));
        // 解析側の前提に合わせて偶数境界へ丸める。
        absRect.x = RoundDownBy(absRect.x, 2);
        absRect.y = RoundDownBy(absRect.y, 2);
        absRect.w = RoundUpBy(absRect.w, 2);
        absRect.h = RoundUpBy(absRect.h, 2);
        absRect.w = std::min(absRect.w, std::max(2, imgw - absRect.x));
        absRect.h = std::min(absRect.h, std::max(2, imgh - absRect.y));
        return absRect;
    }

    // ステージ3: binaryマスクから最終ロゴ矩形を決定する。
    // 処理概要:
    // - 連結成分を抽出し、形状フィルタで帯ノイズを除去
    // - 最良成分をseedに近傍成分を段階結合して文字分断を復元
    // - margin付与・サイズ制約・偶数丸めを行い最終座標(rectAbs)へ変換
    void AutoDetectLogoReader::runRectStage(const ScoreStageBuffers& scoreStage, const BinaryStageBuffers& binaryStage) {
        if (detailedDebug) {
            rectMergeDebug.clear();
        }
        std::vector<uint8_t> xOn(scanw, 0), yOn(scanh, 0);
        collectBinaryProjection(xOn, yOn, binaryStage);

        if (cb && !cb(3, 0.3f, 0.78f, readFrames, searchFrames)) {
            THROW(RuntimeException, "Cancel requested");
        }

        int xStart = 0, xEnd = scanw - 1, yStart = 0, yEnd = scanh - 1;
        const bool hasProjX = TryFindLongestRun(xOn, xStart, xEnd);
        const bool hasProjY = TryFindLongestRun(yOn, yStart, yEnd);
        AutoDetectRect coarse{ 0, 0, 0, 0 };
        bool hasCoarse = false;
        if (hasProjX && hasProjY) {
            coarse = AutoDetectRect{ xStart, yStart, xEnd - xStart + 1, yEnd - yStart + 1 };
            hasCoarse = true;
        }

        AutoDetectRect best = coarse;
        bool hasBest = false;
        double bestScore = -1e30;
        std::vector<RectStageCompCandidate> candidates;
        std::vector<RectStageCompCandidate> mergeCandidates;
        collectRectCandidates(candidates, mergeCandidates, best, hasBest, bestScore, scoreStage, binaryStage);
        if (!hasBest) {
            setRectDetectFail(LogoRectDetectFail::NoBestComponent);
        }

        // 位置決定は binary成分のみで行う。
        AutoDetectRect finalRect = hasBest ? best : coarse;
        if (!hasBest && !hasCoarse) {
            finalRect = buildAcceptedFallbackRect(scoreStage);
        }
        if (finalRect.w <= 0 || finalRect.h <= 0) {
            setRectDetectFail(LogoRectDetectFail::NoBestComponent);
            throwIfRectDetectFailed();
        }

        if (hasBest && !mergeCandidates.empty()) {
            mergeRectCandidates(mergeCandidates, best, finalRect);
        }

        finalRect.x = std::max(0, finalRect.x - marginX);
        finalRect.y = std::max(0, finalRect.y - marginY);
        finalRect.w = std::min(scanw - finalRect.x, finalRect.w + marginX * 2);
        finalRect.h = std::min(scanh - finalRect.y, finalRect.h + marginY * 2);
        if (isRectSizeAbnormal(finalRect)) {
            setRectDetectFail(LogoRectDetectFail::RectSizeAbnormal);
        }
        rectLocal = finalRect;
        rectAbs = makeAbsRectFromLocal(finalRect);

        if (cb && !cb(3, 1.0f, 0.92f, readFrames, searchFrames)) {
            THROW(RuntimeException, "Cancel requested");
        }
        if (cb && !cb(4, 1.0f, 1.0f, readFrames, searchFrames)) {
            THROW(RuntimeException, "Cancel requested");
        }
    }
}

// C API for P/Invoke
extern "C" AMATSUKAZE_API int ScanLogo(AMTContext * ctx,
    const tchar * srcpath, int serviceid, const tchar * workfile, const tchar * dstpath,
    int imgx, int imgy, int w, int h, int thy, int numMaxFrames,
    logo::LOGO_ANALYZE_CB cb) {
    try {
        logo::LogoAnalyzer analyzer(*ctx,
            srcpath, serviceid, workfile, dstpath, imgx, imgy, w, h, thy, numMaxFrames, cb);
        analyzer.ScanLogo();
        return true;
    } catch (const Exception& exception) {
        ctx->setError(exception);
    }
    return false;
}

extern "C" AMATSUKAZE_API int AutoDetectLogoRect(AMTContext* ctx,
    const tchar* srcpath, int serviceid,
    int divx, int divy, int searchFrames, int blockSize, int threshold,
    int marginX, int marginY, int threadN,
    int* outX, int* outY, int* outW, int* outH, int* outRectDetectFail, int* outLogoAnalyzeFail,
    const tchar* scorePath, const tchar* binaryPath, const tchar* cclPath, const tchar* countPath, const tchar* aPath, const tchar* bPath, const tchar* alphaPath, const tchar* logoYPath, const tchar* consistencyPath, const tchar* fgVarPath, const tchar* bgVarPath, const tchar* transitionPath, const tchar* keepRatePath, const tchar* acceptedPath,
    int detailedDebug,
    logo::LOGO_AUTODETECT_CB cb) {
    AutoDetectLogoReader reader(*ctx, serviceid, divx, divy, searchFrames, blockSize, threshold, marginX, marginY, threadN, detailedDebug != 0, cb);
    if (outRectDetectFail) *outRectDetectFail = 0;
    if (outLogoAnalyzeFail) *outLogoAnalyzeFail = 0;
    try {
        const auto rect = reader.run(srcpath);
        if (outX) *outX = rect.x;
        if (outY) *outY = rect.y;
        if (outW) *outW = rect.w;
        if (outH) *outH = rect.h;
        if (outRectDetectFail) *outRectDetectFail = reader.getRectDetectFailCode();
        if (outLogoAnalyzeFail) *outLogoAnalyzeFail = reader.getLogoAnalyzeFailCode();
        if (scorePath && binaryPath && cclPath) {
            reader.writeDebug(scorePath, binaryPath, cclPath, countPath, aPath, bPath, alphaPath, logoYPath, consistencyPath, fgVarPath, bgVarPath, transitionPath, keepRatePath, acceptedPath);
        }
        return true;
    } catch (const Exception& exception) {
        if (outRectDetectFail) *outRectDetectFail = reader.getRectDetectFailCode();
        if (outLogoAnalyzeFail) *outLogoAnalyzeFail = reader.getLogoAnalyzeFailCode();
        if (scorePath && binaryPath && cclPath) {
            try {
                reader.writeDebug(scorePath, binaryPath, cclPath, countPath, aPath, bPath, alphaPath, logoYPath, consistencyPath, fgVarPath, bgVarPath, transitionPath, keepRatePath, acceptedPath);
            } catch (...) {
            }
        }
        ctx->setError(exception);
    }
    return false;
}
